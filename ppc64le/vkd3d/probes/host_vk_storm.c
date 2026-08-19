/*
 * host_vk_storm.c -- does texture-scale copy traffic survive the HOST's own
 * Vulkan stack byte for byte?  No Wine, no FEX, no guest: a plain ppc64le
 * program against RADV, so a failure here convicts kernel/amdgpu/Mesa on
 * this box and a pass points the finger back at the port's guest->GPU path.
 *
 * Written for the Cyberpunk 2077 speckle (2026-08-19): the quiet
 * single-buffer guest probe (copy_pattern_probe.c) is byte-clean in both
 * placements, the game corrupts at scale in both placements (worse when
 * uploads sit in GTT).  This probe recreates the SCALE outside the port:
 *
 *   - one worker per queue (gfx + every compute queue RADV exposes),
 *   - DEPTH submissions in flight per worker (fill set N+1 while N runs),
 *   - per iteration: staging -> device-local buffer -> readback, verified
 *     word by word; every other iteration goes through an OPTIMAL-tiled
 *     RGBA8 image instead (vkCmdCopyBufferToImage / ImageToBuffer), which
 *     is the CopyTextureRegion shape the game actually drives,
 *   - staging placement chosen by STORM_STAGING=gtt|bar (GTT = host-visible
 *     system RAM; bar = host-visible device-local, the ReBAR heap).
 *
 * Traffic: STORM_CHUNK_MB per set (default 32) x DEPTH (default 2) x
 * workers x STORM_ITERS (default 64) -- with 5 queues that is ~20 GB of
 * verified round trips per leg.
 *
 * On mismatch: first offsets, expected/got, an offset%16 lane histogram and
 * an offset%4096 page-line histogram (the speckle's stride signature), and
 * exit 1.  Run it while the game (or any load) owns the GPU to add the
 * "GPU under load" axis for free.
 *
 * Build (on op4k):  gcc -O2 -o host_vk_storm host_vk_storm.c -lvulkan -lpthread
 *
 * Copyright 2026 the ppc64le port authors
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#include <vulkan/vulkan.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <inttypes.h>

#define MAX_WORKERS 8
#define DEPTH 2
#define IMG_W 2048           /* 2048x2048 RGBA8 = 16 MiB per image trip */
#define IMG_H 2048
#define IMG_BYTES ((uint64_t)IMG_W * IMG_H * 4)

static VkInstance instance;
static VkPhysicalDevice phys;
static VkDevice dev;
static VkPhysicalDeviceMemoryProperties memprops;

static uint32_t gfx_family, comp_family, comp_count;
static int use_bar_staging;          /* STORM_STAGING=bar */
static uint64_t chunk_bytes;
static int iters;
static uint64_t run_seed;

static pthread_mutex_t report_lock = PTHREAD_MUTEX_INITIALIZER;
static uint64_t total_bad_words;
static uint64_t lane_hist[16];       /* bad-byte offset % 16 */
static uint64_t page_hist[8];        /* bad-byte (offset % 4096) / 512 */
static int reported_first;

#define CHECK(expr) do { VkResult r_ = (expr); if (r_ != VK_SUCCESS) { \
    fprintf(stderr, "host_vk_storm: %s -> %d\n", #expr, r_); exit(2); } } while (0)

static uint64_t pattern_word(uint64_t magic, uint64_t idx)
{
    return magic ^ (idx * 0x9e3779b97f4a7c15ull) ^ (idx << 32);
}

static uint32_t find_mem_type(uint32_t type_bits, VkMemoryPropertyFlags want,
                              VkMemoryPropertyFlags reject)
{
    for (uint32_t i = 0; i < memprops.memoryTypeCount; i++)
    {
        VkMemoryPropertyFlags f = memprops.memoryTypes[i].propertyFlags;
        if (!(type_bits & (1u << i))) continue;
        if ((f & want) != want) continue;
        if (f & reject) continue;
        return i;
    }
    return UINT32_MAX;
}

struct chunk
{
    VkBuffer buf;
    VkDeviceMemory mem;
    void *map;                       /* NULL for the device-local buffer */
};

static void make_chunk(struct chunk *c, uint64_t size,
                       VkMemoryPropertyFlags want, VkMemoryPropertyFlags reject,
                       int mapped)
{
    VkBufferCreateInfo bi = {
        .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .size = size,
        .usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
    };
    CHECK(vkCreateBuffer(dev, &bi, NULL, &c->buf));

    VkMemoryRequirements req;
    vkGetBufferMemoryRequirements(dev, c->buf, &req);
    uint32_t type = find_mem_type(req.memoryTypeBits, want, reject);
    if (type == UINT32_MAX)
    {
        fprintf(stderr, "host_vk_storm: no memory type want=0x%x reject=0x%x\n",
                want, reject);
        exit(2);
    }
    VkMemoryAllocateInfo ai = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .allocationSize = req.size,
        .memoryTypeIndex = type,
    };
    CHECK(vkAllocateMemory(dev, &ai, NULL, &c->mem));
    CHECK(vkBindBufferMemory(dev, c->buf, c->mem, 0));
    c->map = NULL;
    if (mapped) CHECK(vkMapMemory(dev, c->mem, 0, VK_WHOLE_SIZE, 0, &c->map));
}

struct set
{
    struct chunk staging, devbuf, readback;
    VkImage image;
    VkDeviceMemory image_mem;
    VkCommandBuffer cmd_buffer;      /* buffer->buffer trip */
    VkCommandBuffer cmd_image;       /* buffer->image->buffer trip */
    VkFence fence;
    uint64_t magic;                  /* pattern seed of the fill in flight */
    int via_image;                   /* which trip the in-flight fill took */
    int in_flight;
};

struct worker
{
    int id;
    uint32_t family;
    VkQueue queue;
    VkCommandPool pool;
    struct set sets[DEPTH];
    pthread_t thread;
    uint64_t bad_words;
};

static void make_image(struct set *s)
{
    VkImageCreateInfo ii = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
        .imageType = VK_IMAGE_TYPE_2D,
        .format = VK_FORMAT_R8G8B8A8_UNORM,
        .extent = { IMG_W, IMG_H, 1 },
        .mipLevels = 1,
        .arrayLayers = 1,
        .samples = VK_SAMPLE_COUNT_1_BIT,
        .tiling = VK_IMAGE_TILING_OPTIMAL,
        .usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
        .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
    };
    CHECK(vkCreateImage(dev, &ii, NULL, &s->image));

    VkMemoryRequirements req;
    vkGetImageMemoryRequirements(dev, s->image, &req);
    uint32_t type = find_mem_type(req.memoryTypeBits,
                                  VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
                                  VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT);
    if (type == UINT32_MAX)  /* fall back: any device-local */
        type = find_mem_type(req.memoryTypeBits,
                             VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, 0);
    VkMemoryAllocateInfo ai = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .allocationSize = req.size,
        .memoryTypeIndex = type,
    };
    CHECK(vkAllocateMemory(dev, &ai, NULL, &s->image_mem));
    CHECK(vkBindImageMemory(dev, s->image, s->image_mem, 0));
}

static void record_buffer_trip(struct set *s, VkCommandBuffer cmd)
{
    VkCommandBufferBeginInfo bi = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
    };
    CHECK(vkBeginCommandBuffer(cmd, &bi));
    VkBufferCopy region = { 0, 0, chunk_bytes };
    vkCmdCopyBuffer(cmd, s->staging.buf, s->devbuf.buf, 1, &region);
    VkBufferMemoryBarrier bar = {
        .sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER,
        .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
        .dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .buffer = s->devbuf.buf,
        .size = VK_WHOLE_SIZE,
    };
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
                         VK_PIPELINE_STAGE_TRANSFER_BIT, 0,
                         0, NULL, 1, &bar, 0, NULL);
    vkCmdCopyBuffer(cmd, s->devbuf.buf, s->readback.buf, 1, &region);
    CHECK(vkEndCommandBuffer(cmd));
}

static void record_image_trip(struct set *s, VkCommandBuffer cmd)
{
    VkCommandBufferBeginInfo bi = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
    };
    CHECK(vkBeginCommandBuffer(cmd, &bi));

    VkImageMemoryBarrier to_dst = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
        .srcAccessMask = 0,
        .dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
        .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
        .newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .image = s->image,
        .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 },
    };
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                         VK_PIPELINE_STAGE_TRANSFER_BIT, 0,
                         0, NULL, 0, NULL, 1, &to_dst);

    VkBufferImageCopy region = {
        .bufferOffset = 0,
        .imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
        .imageExtent = { IMG_W, IMG_H, 1 },
    };
    vkCmdCopyBufferToImage(cmd, s->staging.buf, s->image,
                           VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);

    VkImageMemoryBarrier to_src = to_dst;
    to_src.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    to_src.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    to_src.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    to_src.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
                         VK_PIPELINE_STAGE_TRANSFER_BIT, 0,
                         0, NULL, 0, NULL, 1, &to_src);

    vkCmdCopyImageToBuffer(cmd, s->image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                           s->readback.buf, 1, &region);
    CHECK(vkEndCommandBuffer(cmd));
}

static void fill_staging(struct set *s, uint64_t magic, uint64_t nbytes)
{
    volatile uint64_t *p = s->staging.map;
    uint64_t words = nbytes / 8;
    for (uint64_t i = 0; i < words; i++) p[i] = pattern_word(magic, i);
}

static void verify_readback(struct worker *w, struct set *s, uint64_t nbytes)
{
    VkMappedMemoryRange range = {
        .sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE,
        .memory = s->readback.mem,
        .size = VK_WHOLE_SIZE,
    };
    vkInvalidateMappedMemoryRanges(dev, 1, &range);

    const uint64_t *p = s->readback.map;
    uint64_t words = nbytes / 8;
    for (uint64_t i = 0; i < words; i++)
    {
        uint64_t want = pattern_word(s->magic, i);
        uint64_t got = p[i];
        if (got == want) continue;

        w->bad_words++;
        pthread_mutex_lock(&report_lock);
        total_bad_words++;
        uint64_t diff = got ^ want;
        for (int b = 0; b < 8; b++)
        {
            if (!((diff >> (b * 8)) & 0xff)) continue;
            uint64_t off = i * 8 + b;
            lane_hist[off % 16]++;
            page_hist[(off % 4096) / 512]++;
        }
        if (reported_first < 8)
        {
            reported_first++;
            printf("MISMATCH worker %d %s trip: offset 0x%" PRIx64
                   " want %016" PRIx64 " got %016" PRIx64 "\n",
                   w->id, s->via_image ? "image" : "buffer",
                   i * 8, want, got);
            fflush(stdout);
        }
        pthread_mutex_unlock(&report_lock);
    }
}

static void *worker_main(void *arg)
{
    struct worker *w = arg;
    uint64_t image_fill_bytes = IMG_BYTES < chunk_bytes ? IMG_BYTES : chunk_bytes;

    for (int it = 0; it < iters; it++)
    {
        struct set *s = &w->sets[it % DEPTH];
        int via_image = it & 1;
        uint64_t nbytes = via_image ? image_fill_bytes : chunk_bytes;

        if (s->in_flight)
        {
            CHECK(vkWaitForFences(dev, 1, &s->fence, VK_TRUE, UINT64_MAX));
            CHECK(vkResetFences(dev, 1, &s->fence));
            verify_readback(w, s, s->via_image ? image_fill_bytes : chunk_bytes);
            s->in_flight = 0;
        }

        s->magic = run_seed ^ ((uint64_t)w->id << 56) ^ ((uint64_t)it * 0xd1342543de82ef95ull);
        s->via_image = via_image;
        fill_staging(s, s->magic, nbytes);
        VkMappedMemoryRange range = {
            .sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE,
            .memory = s->staging.mem,
            .size = VK_WHOLE_SIZE,
        };
        vkFlushMappedMemoryRanges(dev, 1, &range);

        VkCommandBuffer cmd = via_image ? s->cmd_image : s->cmd_buffer;
        VkSubmitInfo si = {
            .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
            .commandBufferCount = 1,
            .pCommandBuffers = &cmd,
        };
        CHECK(vkQueueSubmit(w->queue, 1, &si, s->fence));
        s->in_flight = 1;
    }

    for (int d = 0; d < DEPTH; d++)
    {
        struct set *s = &w->sets[d];
        if (!s->in_flight) continue;
        CHECK(vkWaitForFences(dev, 1, &s->fence, VK_TRUE, UINT64_MAX));
        verify_readback(w, s, s->via_image ? image_fill_bytes : chunk_bytes);
        s->in_flight = 0;
    }
    return NULL;
}

int main(void)
{
    const char *staging_env = getenv("STORM_STAGING");
    use_bar_staging = staging_env && !strcmp(staging_env, "bar");
    chunk_bytes = (uint64_t)(getenv("STORM_CHUNK_MB") ?
                             atoi(getenv("STORM_CHUNK_MB")) : 32) << 20;
    iters = getenv("STORM_ITERS") ? atoi(getenv("STORM_ITERS")) : 64;
    run_seed = getenv("STORM_SEED") ? strtoull(getenv("STORM_SEED"), NULL, 0)
                                    : 0x5eed5eed5eed5eedull;

    VkApplicationInfo app = {
        .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
        .pApplicationName = "host_vk_storm",
        .apiVersion = VK_API_VERSION_1_1,
    };
    VkInstanceCreateInfo ici = {
        .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
        .pApplicationInfo = &app,
    };
    CHECK(vkCreateInstance(&ici, NULL, &instance));

    uint32_t n = 8;
    VkPhysicalDevice devs[8];
    VkResult er = vkEnumeratePhysicalDevices(instance, &n, devs);
    if (er != VK_SUCCESS && er != VK_INCOMPLETE)
    {
        fprintf(stderr, "host_vk_storm: enumerate -> %d\n", er);
        return 2;
    }
    phys = devs[0];
    for (uint32_t i = 0; i < n; i++)
    {
        VkPhysicalDeviceProperties props;
        vkGetPhysicalDeviceProperties(devs[i], &props);
        if (props.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU)
        {
            phys = devs[i];
            printf("host_vk_storm: using %s\n", props.deviceName);
            break;
        }
    }
    vkGetPhysicalDeviceMemoryProperties(phys, &memprops);

    uint32_t fam_n = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(phys, &fam_n, NULL);
    VkQueueFamilyProperties fams[16];
    if (fam_n > 16) fam_n = 16;
    vkGetPhysicalDeviceQueueFamilyProperties(phys, &fam_n, fams);

    gfx_family = comp_family = UINT32_MAX;
    comp_count = 0;
    for (uint32_t i = 0; i < fam_n; i++)
    {
        VkQueueFlags f = fams[i].queueFlags;
        if (gfx_family == UINT32_MAX && (f & VK_QUEUE_GRAPHICS_BIT))
            gfx_family = i;
        else if (comp_family == UINT32_MAX && (f & VK_QUEUE_COMPUTE_BIT))
        {
            comp_family = i;
            comp_count = fams[i].queueCount;
            if (comp_count > MAX_WORKERS - 1) comp_count = MAX_WORKERS - 1;
        }
    }
    if (gfx_family == UINT32_MAX)
    {
        fprintf(stderr, "host_vk_storm: no graphics queue family\n");
        return 2;
    }

    float prios[MAX_WORKERS] = { 0 };
    VkDeviceQueueCreateInfo qcis[2] = {
        { .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
          .queueFamilyIndex = gfx_family, .queueCount = 1,
          .pQueuePriorities = prios },
        { .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
          .queueFamilyIndex = comp_family, .queueCount = comp_count,
          .pQueuePriorities = prios },
    };
    VkDeviceCreateInfo dci = {
        .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
        .queueCreateInfoCount = comp_count ? 2u : 1u,
        .pQueueCreateInfos = qcis,
    };
    CHECK(vkCreateDevice(phys, &dci, NULL, &dev));

    int nworkers = 1 + (int)comp_count;
    static struct worker workers[MAX_WORKERS];

    VkMemoryPropertyFlags staging_want = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                         VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
    VkMemoryPropertyFlags staging_reject = 0;
    if (use_bar_staging) staging_want |= VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
    else staging_reject = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;

    printf("host_vk_storm: %d workers (gfx family %u + %u compute), "
           "chunk %" PRIu64 " MiB, depth %d, iters %d, staging %s\n",
           nworkers, gfx_family, comp_count, chunk_bytes >> 20, DEPTH, iters,
           use_bar_staging ? "BAR (host-visible VRAM)" : "GTT (system RAM)");
    fflush(stdout);

    for (int i = 0; i < nworkers; i++)
    {
        struct worker *w = &workers[i];
        w->id = i;
        w->family = i == 0 ? gfx_family : comp_family;
        vkGetDeviceQueue(dev, w->family, i == 0 ? 0 : (uint32_t)(i - 1), &w->queue);

        VkCommandPoolCreateInfo pci = {
            .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
            .queueFamilyIndex = w->family,
        };
        CHECK(vkCreateCommandPool(dev, &pci, NULL, &w->pool));

        for (int d = 0; d < DEPTH; d++)
        {
            struct set *s = &w->sets[d];
            make_chunk(&s->staging, chunk_bytes, staging_want, staging_reject, 1);
            make_chunk(&s->devbuf, chunk_bytes,
                       VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
                       VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT, 0);
            make_chunk(&s->readback, chunk_bytes,
                       VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                       VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                       VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, 1);
            make_image(s);

            VkCommandBufferAllocateInfo cai = {
                .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
                .commandPool = w->pool,
                .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
                .commandBufferCount = 1,
            };
            CHECK(vkAllocateCommandBuffers(dev, &cai, &s->cmd_buffer));
            CHECK(vkAllocateCommandBuffers(dev, &cai, &s->cmd_image));
            record_buffer_trip(s, s->cmd_buffer);
            record_image_trip(s, s->cmd_image);

            VkFenceCreateInfo fci = { .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
            CHECK(vkCreateFence(dev, &fci, NULL, &s->fence));
        }
    }

    for (int i = 0; i < nworkers; i++)
        pthread_create(&workers[i].thread, NULL, worker_main, &workers[i]);
    for (int i = 0; i < nworkers; i++)
        pthread_join(workers[i].thread, NULL);

    uint64_t buffer_iters = (iters + 1) / 2, image_iters = iters / 2;
    uint64_t traffic = (uint64_t)nworkers *
        (buffer_iters * chunk_bytes +
         image_iters * (IMG_BYTES < chunk_bytes ? IMG_BYTES : chunk_bytes));
    printf("host_vk_storm: ~%" PRIu64 " MiB round-tripped and verified\n",
           traffic >> 20);

    if (total_bad_words)
    {
        printf("host_vk_storm: FAIL %" PRIu64 " bad words\n", total_bad_words);
        printf("  lane histogram (bad byte offset %% 16):\n   ");
        for (int i = 0; i < 16; i++) printf(" %" PRIu64, lane_hist[i]);
        printf("\n  page histogram (offset %% 4096, 512-byte bins):\n   ");
        for (int i = 0; i < 8; i++) printf(" %" PRIu64, page_hist[i]);
        printf("\n");
        return 1;
    }
    printf("host_vk_storm: PASS\n");
    return 0;
}
