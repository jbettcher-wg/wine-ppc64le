/*
 * vk_swapchain_smoke -- the native-vs-guest Vulkan WSI runtime gate.
 *
 * ONE source, built TWICE and run TWICE under the same wine: once as a
 * NATIVE ppc64 PE (winegcc, the machine's own architecture, no emulation
 * anywhere in the process) and once as an x86-64 Windows PE run as a GUEST.
 * The two runs must print BYTE-IDENTICAL stdout.
 *
 * WHY THIS PROBE EXISTS
 *
 * A guest x86-64 game reached vkGetSwapchainImagesKHR through the vulkan-1
 * guest thunk, got VK_SUCCESS back, and died anyway -- "getSwapchainImagesKHR
 * failed with error (VK_SUCCESS)", which is what a game prints when the RESULT
 * was fine and the VALUE was not.  Nothing in this port had ever called that
 * function from a Win32 client: the D3D12 and D3D11 legs present through
 * __wine_get_hwnd_surface_funcs on their OWN foreign VkInstance (see
 * include/wine/vulkan_driver.h), which does not touch win32u's client-object
 * wrapping at all, and ppc64le/dxvk/check-d3d11-smoke.sh runs headless and
 * never creates a swapchain.  So the entire vulkan-1 -> winevulkan -> win32u
 * WSI path -- the shortest route from a real game's draw calls to this
 * machine's GPU -- was ungated.  This is that gate.
 *
 * WHAT IT PROVES, STEP BY STEP
 *
 *   1  an instance with VK_KHR_surface + VK_KHR_win32_surface.
 *   2  a real window, and vkCreateWin32SurfaceKHR on its HWND.  The surface
 *      is a WRAPPED object: win32u allocates a struct surface, hands the
 *      client a pointer to it and keeps the host VkSurfaceKHR inside.  Every
 *      later call has to unwrap it again.
 *   3  a physical device that reports PRESENT SUPPORT for that surface on a
 *      graphics queue family, and exposes VK_KHR_swapchain.
 *   4  vkGetPhysicalDeviceSurfaceCapabilitiesKHR / FormatsKHR /
 *      PresentModesKHR -- the three count-then-fetch queries a swapchain is
 *      built from, all of which write into the CALLER's memory.
 *   5  a device and its queue.
 *   6  vkCreateSwapchainKHR.  A second wrapped object, and the one whose
 *      create info carries a wrapped VkSurfaceKHR in the middle of a struct.
 *   7  THE DEFECT'S OWN STEP.  vkGetSwapchainImagesKHR twice, the
 *      count-query + fetch pattern every Vulkan renderer uses:
 *        a  count query with pImages NULL, into a sentinel-filled uint32.
 *           The count must come back, and be non-zero.
 *        b  fetch into a sentinel-filled array.  Every returned handle must
 *           be non-NULL, none may still hold the sentinel, and they must all
 *           be distinct.
 *        c  the fetch REPEATED into a second sentinel-filled array.  The two
 *           arrays must be byte-identical: a swapchain's images do not move.
 *      A count of zero, an array left untouched, or an array whose tail was
 *      never written are three different bugs that all return VK_SUCCESS,
 *      and each of them is what the game saw.
 *   8  vkAcquireNextImageKHR with a fence, waited on.  The index must be in
 *      range.
 *   9  TEXEL-EXACT.  The acquired image is cleared to an exact colour, then
 *      copied to a host-visible staging buffer BEFORE it is presented, and
 *      every texel of the copy is checked.  Pure 0x00/0xff channels, so the
 *      expected bytes are exact under any rounding rule and a swapped
 *      channel order cannot hide.  This is the standard the rest of the
 *      port's graphics gates hold to.
 *  10  vkQueuePresentKHR of that image.  A present is the only thing that
 *      proves the swapchain was ever real.
 *
 * NOT ON STDOUT: handles and pointers.  A VkImage is a driver address and
 * differs between two runs of the same program on the same machine, so the
 * transcript carries only what is DERIVED from them -- counts, non-nullness,
 * distinctness, equality between the two fetches.  The raw values go to
 * stderr as notes, where check-swapchain-smoke.sh reads them for the report
 * and no diff can see them.
 *
 * VK_SWAPCHAIN_BREAK (falsification; the gate builds each variant and
 * requires the NATIVE leg to FAIL, because a gate that cannot go red proves
 * nothing):
 *
 *   =1  skip the clear -- the readback then sees whatever the driver left in
 *       a fresh swapchain image, and step 9 goes red.
 *   =2  swap R and B in the CHECK's own expectation (the rendered bytes are
 *       still right; the check is deliberately wrong), so every texel
 *       mismatches.
 *   =3  check texel (0,0) only.  Coverage is part of step 9's claim, so its
 *       verdict requires checked == width*height as well as mismatches == 0,
 *       and an incomplete scan fails on that arithmetic rather than by luck.
 *   =4  skip the SECOND vkGetSwapchainImagesKHR call entirely, leaving the
 *       array full of sentinels.  This is the game's own failure mode --
 *       VK_SUCCESS and no usable images -- and step 7b must go red on it.
 *   =5  corrupt one handle of the repeated fetch, so step 7c goes red: proof
 *       that the consistency check is a check and not a formality.
 *
 * NO C RUNTIME on the guest leg, for the reason ppc64le/opengl/probes/
 * gl_smoke.c gives: the program formats its own output and writes it with
 * WriteFile, so neither libc's nor ucrt's printf can be the source of a byte
 * difference.  The native leg links ucrtbase (winegcc wants a CRT to start a
 * PE) but calls the same hand-written formatters.
 *
 * Copyright 2026 the ppc64le port authors
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 */

#ifndef VK_SWAPCHAIN_BREAK
#define VK_SWAPCHAIN_BREAK 0
#endif

#include <windows.h>
#include <wine/vulkan.h>

/* The guest leg links no CRT at all, and the compiler still lowers struct
 * initialisation to memset/memcpy however hard -fno-builtin tries.  Supply
 * them; the volatile cursor keeps the definitions from being recognised as
 * the idioms they implement and rewritten into calls to themselves. */
#ifndef VK_SMOKE_NATIVE
void *memset( void *d, int c, size_t n );
void *memcpy( void *d, const void *s, size_t n );
void *memset( void *d, int c, size_t n )
{
    volatile unsigned char *p = d;
    while (n--) *p++ = (unsigned char)c;
    return d;
}
void *memcpy( void *d, const void *s, size_t n )
{
    volatile unsigned char *p = d;
    const unsigned char *q = s;
    while (n--) *p++ = *q++;
    return d;
}
#endif

/* ------------------------------------------------------------- output */

static void out_fd( HANDLE h, const char *s )
{
    DWORD n = 0, written;
    while (s[n]) n++;
    WriteFile( h, s, n, &written, NULL );
}

static void out( const char *s )
{
    out_fd( GetStdHandle( STD_OUTPUT_HANDLE ), s );
}

static void out_dec_to( HANDLE h, ULONG v )
{
    char buf[12];
    int i = 11;

    buf[i] = 0;
    do { buf[--i] = '0' + (char)(v % 10); v /= 10; } while (v);
    out_fd( h, buf + i );
}

static void out_dec( ULONG v )
{
    out_dec_to( GetStdHandle( STD_OUTPUT_HANDLE ), v );
}

/* A VkResult is a SIGNED enum and every error is negative; printing it as an
 * unsigned would turn VK_ERROR_OUT_OF_HOST_MEMORY into 4294967295 and make
 * two different errors look alike at a glance. */
static void out_int( int v )
{
    if (v < 0) { out( "-" ); out_dec( (ULONG)(-(LONG)v) ); }
    else out_dec( (ULONG)v );
}

static void out_hex_to( HANDLE h, ULONGLONG v, int digits )
{
    static const char hex[] = "0123456789ABCDEF";
    char buf[17];
    int i;

    for (i = 0; i < digits; i++) buf[digits - 1 - i] = hex[(v >> (4 * i)) & 0xf];
    buf[digits] = 0;
    out_fd( h, buf );
}

static void out_hex( ULONGLONG v, int digits )
{
    out_hex_to( GetStdHandle( STD_OUTPUT_HANDLE ), v, digits );
}

/* Anything whose answer is TRUE OF THE PORT rather than of Vulkan -- a
 * handle, an address -- goes here, never into the transcript the two legs
 * are diffed against.  Assembled and written in ONE call: stderr is where
 * the port's own debug channels write too, and a line emitted in four
 * WriteFiles can arrive with an err:seh line spliced through the middle. */
static void note_handle( const char *label, ULONG idx, ULONGLONG v )
{
    static const char hex[] = "0123456789ABCDEF";
    char buf[160];
    int n = 0, i;

    while (label[n] && n < 100) { buf[n] = label[n]; n++; }
    buf[n++] = '[';
    if (idx >= 10) buf[n++] = '0' + (char)(idx / 10);
    buf[n++] = '0' + (char)(idx % 10);
    buf[n++] = ']'; buf[n++] = '='; buf[n++] = '0'; buf[n++] = 'x';
    for (i = 15; i >= 0; i--) buf[n++] = hex[(v >> (4 * i)) & 0xf];
    buf[n++] = '\n';
    buf[n] = 0;
    out_fd( GetStdHandle( STD_ERROR_HANDLE ), buf );
}

/* A device name, printed with anything unprintable escaped, so the
 * transcript is diffable whatever the driver put there. */
static void out_str_bounded( const char *s, UINT max )
{
    char buf[2];
    UINT i;

    if (!s) { out( "(null)" ); return; }
    buf[1] = 0;
    for (i = 0; i < max && s[i]; i++)
    {
        if (s[i] < 0x20 || s[i] > 0x7e) { out( "?" ); continue; }
        buf[0] = s[i];
        out( buf );
    }
}

static int str_eq( const char *a, const char *b )
{
    while (*a && *a == *b) { a++; b++; }
    return *a == *b;
}

/* ------------------------------------------------------------- the run */

static int failures;
static int step;
static const char *first_fail;

static void begin( const char *what )
{
    out( "step " );
    out_dec( (ULONG)++step );
    out( " " );
    out( what );
    out( ": " );
}

static void verdict( BOOL ok, const char *why )
{
    if (ok) out( " ok\n" );
    else
    {
        if (!first_fail) first_fail = why;
        failures++;
        out( " FAIL (" );
        out( why );
        out( ")\n" );
    }
}

/* The clear colour.  Every channel is 0.0 or 1.0, so the 8-bit result is
 * exact under every rounding rule and in both UNORM and SRGB encodings --
 * there is no tie to argue about, unlike a 0.5 that lands on 127.5.  R and B
 * differ, so a swapped channel order cannot pass. */
#define CLEAR_R 1.0f
#define CLEAR_G 0.0f
#define CLEAR_B 0.0f
#define CLEAR_A 1.0f

#define WIN_W 256
#define WIN_H 256

#define MAX_IMAGES 32
#define SENTINEL_IMAGE 0xCCCCCCCCCCCCCCCCull
#define SENTINEL_COUNT 0xDEADBEEFu

/* Big enough that a frame's worth of texels never sits on the stack: the
 * guest leg has no CRT and therefore no stack-probe helper, so a frame over
 * a page would be a fault rather than a diagnosis. */
static VkImage images_a[MAX_IMAGES];
static VkImage images_b[MAX_IMAGES];
static VkPhysicalDevice phys_devices[16];
static VkQueueFamilyProperties queue_families[32];
static VkSurfaceFormatKHR surface_formats[256];
static VkPresentModeKHR present_modes[16];
static VkExtensionProperties device_extensions[512];
static VkPhysicalDeviceProperties device_props;
static VkPhysicalDeviceMemoryProperties memory_props;

static LRESULT CALLBACK smoke_wndproc( HWND hwnd, UINT msg, WPARAM wp, LPARAM lp )
{
    return DefWindowProcA( hwnd, msg, wp, lp );
}

static int vk_swapchain_smoke_run( void )
{
    VkApplicationInfo app_info = { VK_STRUCTURE_TYPE_APPLICATION_INFO };
    VkInstanceCreateInfo instance_info = { VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO };
    VkWin32SurfaceCreateInfoKHR surface_info = { VK_STRUCTURE_TYPE_WIN32_SURFACE_CREATE_INFO_KHR };
    VkDeviceQueueCreateInfo queue_info = { VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO };
    VkDeviceCreateInfo device_info = { VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO };
    VkSwapchainCreateInfoKHR swapchain_info = { VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR };
    VkSurfaceCapabilitiesKHR caps;
    VkCommandPoolCreateInfo pool_info = { VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO };
    VkCommandBufferAllocateInfo cmd_alloc = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO };
    VkCommandBufferBeginInfo begin_info = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
    VkFenceCreateInfo fence_info = { VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
    VkBufferCreateInfo buffer_info = { VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
    VkMemoryAllocateInfo mem_info = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
    VkImageMemoryBarrier barrier = { VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
    VkSubmitInfo submit_info = { VK_STRUCTURE_TYPE_SUBMIT_INFO };
    VkPresentInfoKHR present_info = { VK_STRUCTURE_TYPE_PRESENT_INFO_KHR };
    VkMemoryRequirements mem_reqs;
    VkBufferImageCopy copy;
    VkClearColorValue clear;
    VkImageSubresourceRange range;
    static const char *instance_exts[2] = { VK_KHR_SURFACE_EXTENSION_NAME,
                                            VK_KHR_WIN32_SURFACE_EXTENSION_NAME };
    static const char *device_exts[1] = { VK_KHR_SWAPCHAIN_EXTENSION_NAME };
    static const float queue_priority = 1.0f;

    VkInstance instance = VK_NULL_HANDLE;
    VkPhysicalDevice phys = VK_NULL_HANDLE;
    VkSurfaceKHR surface = VK_NULL_HANDLE;
    VkDevice device = VK_NULL_HANDLE;
    VkQueue queue = VK_NULL_HANDLE;
    VkSwapchainKHR swapchain = VK_NULL_HANDLE;
    VkCommandPool pool = VK_NULL_HANDLE;
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    VkFence fence = VK_NULL_HANDLE;
    VkBuffer staging = VK_NULL_HANDLE;
    VkDeviceMemory staging_mem = VK_NULL_HANDLE;
    HWND hwnd = NULL;
    WNDCLASSA wc;
    RECT client_rect;

    VkResult vr, vr2, present_result;
    UINT32 count, count_a, count_b, family = ~0u, mem_type = ~0u;
    UINT32 image_index = SENTINEL_COUNT;
    UINT32 i, j, chosen_format_index = 0;
    UINT32 nonnull, sentinels, distinct;
    UINT32 count_present_modes = 0, contract_bad = 0;
    UINT32 checked = 0, mismatches = 0;
    UINT32 width, height;
    BYTE expect[4], first4[4] = { 0, 0, 0, 0 };
    BYTE *mapped = NULL;
    int rc;

    /* ---- 1: the instance -------------------------------------------------- */
    begin( "instance" );
    app_info.pApplicationName = "vk_swapchain_smoke";
    app_info.apiVersion = VK_API_VERSION_1_1;
    instance_info.pApplicationInfo = &app_info;
    instance_info.enabledExtensionCount = 2;
    instance_info.ppEnabledExtensionNames = instance_exts;
    vr = vkCreateInstance( &instance_info, NULL, &instance );
    out( "vkCreateInstance=" ); out_int( vr );
    verdict( vr == VK_SUCCESS && instance != VK_NULL_HANDLE, "no instance" );
    if (vr != VK_SUCCESS) goto done;
    note_handle( "instance", 0, (ULONGLONG)(ULONG_PTR)instance );

    /* ---- 2: a window and a Win32 surface on it ---------------------------- */
    begin( "surface" );
    memset( &wc, 0, sizeof(wc) );
    wc.lpfnWndProc = smoke_wndproc;
    wc.hInstance = GetModuleHandleA( NULL );
    wc.lpszClassName = "vk_swapchain_smoke";
    RegisterClassA( &wc );
    hwnd = CreateWindowExA( 0, "vk_swapchain_smoke", "vk_swapchain_smoke",
                            WS_OVERLAPPEDWINDOW, 0, 0, WIN_W, WIN_H,
                            NULL, NULL, wc.hInstance, NULL );
    ShowWindow( hwnd, SW_SHOW );
    memset( &client_rect, 0, sizeof(client_rect) );
    GetClientRect( hwnd, &client_rect );
    surface_info.hinstance = wc.hInstance;
    surface_info.hwnd = hwnd;
    vr = vkCreateWin32SurfaceKHR( instance, &surface_info, NULL, &surface );
    out( "hwnd=" ); out( hwnd ? "yes" : "no" );
    out( " client=" ); out_dec( (ULONG)(client_rect.right - client_rect.left) );
    out( "x" ); out_dec( (ULONG)(client_rect.bottom - client_rect.top) );
    out( " vkCreateWin32SurfaceKHR=" ); out_int( vr );
    verdict( hwnd && vr == VK_SUCCESS && surface != VK_NULL_HANDLE, "no surface" );
    if (vr != VK_SUCCESS) goto done;
    note_handle( "surface", 0, (ULONGLONG)surface );

    /* ---- 3: a physical device that can present to it ---------------------- */
    begin( "device-pick" );
    count = 16;
    vr = vkEnumeratePhysicalDevices( instance, &count, phys_devices );
    for (i = 0; i < count && phys == VK_NULL_HANDLE; i++)
    {
        UINT32 nfam = 32, next = 512;
        BOOL has_swapchain = FALSE;

        vkGetPhysicalDeviceQueueFamilyProperties( phys_devices[i], &nfam, queue_families );
        if (vkEnumerateDeviceExtensionProperties( phys_devices[i], NULL, &next, device_extensions ))
            continue;
        for (j = 0; j < next; j++)
            if (str_eq( device_extensions[j].extensionName, VK_KHR_SWAPCHAIN_EXTENSION_NAME ))
                has_swapchain = TRUE;
        if (!has_swapchain) continue;
        for (j = 0; j < nfam; j++)
        {
            VkBool32 supported = VK_FALSE;
            if (!(queue_families[j].queueFlags & VK_QUEUE_GRAPHICS_BIT)) continue;
            if (vkGetPhysicalDeviceSurfaceSupportKHR( phys_devices[i], j, surface, &supported ))
                continue;
            if (!supported) continue;
            phys = phys_devices[i];
            family = j;
            break;
        }
    }
    out( "devices=" ); out_dec( count );
    out( " enumerate=" ); out_int( vr );
    if (phys)
    {
        memset( &device_props, 0, sizeof(device_props) );
        vkGetPhysicalDeviceProperties( phys, &device_props );
        out( " picked=\"" ); out_str_bounded( device_props.deviceName, VK_MAX_PHYSICAL_DEVICE_NAME_SIZE );
        out( "\" vendor=" ); out_hex( device_props.vendorID, 4 );
        out( " device=" ); out_hex( device_props.deviceID, 4 );
        out( " family=" ); out_dec( family );
    }
    verdict( vr == VK_SUCCESS && phys != VK_NULL_HANDLE,
             "no physical device reported present support for this surface" );
    if (!phys) goto done;

    /* ---- 4: the three count-then-fetch surface queries --------------------- */
    begin( "surface-caps" );
    memset( &caps, 0, sizeof(caps) );
    vr = vkGetPhysicalDeviceSurfaceCapabilitiesKHR( phys, surface, &caps );
    out( "result=" ); out_int( vr );
    out( " minImageCount=" ); out_dec( caps.minImageCount );
    out( " maxImageCount=" ); out_dec( caps.maxImageCount );
    out( " currentExtent=" ); out_dec( caps.currentExtent.width );
    out( "x" ); out_dec( caps.currentExtent.height );
    out( " usage=" ); out_hex( caps.supportedUsageFlags, 8 );
    verdict( vr == VK_SUCCESS && caps.minImageCount >= 1 &&
             (caps.supportedUsageFlags & VK_IMAGE_USAGE_TRANSFER_DST_BIT) &&
             (caps.supportedUsageFlags & VK_IMAGE_USAGE_TRANSFER_SRC_BIT),
             "the surface cannot carry a transferable swapchain image" );

    begin( "surface-formats" );
    count = SENTINEL_COUNT;
    vr = vkGetPhysicalDeviceSurfaceFormatsKHR( phys, surface, &count, NULL );
    count_a = count;
    if (count_a > 256) count_a = 256;
    count = count_a;
    memset( surface_formats, 0, sizeof(surface_formats) );
    vr2 = vkGetPhysicalDeviceSurfaceFormatsKHR( phys, surface, &count, surface_formats );
    for (i = 0; i < count; i++)
        if (surface_formats[i].format == VK_FORMAT_B8G8R8A8_UNORM &&
            surface_formats[i].colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR)
        { chosen_format_index = i; break; }
    out( "count-query=" ); out_int( vr );
    out( " count=" ); out_dec( count );
    out( " fetch=" ); out_int( vr2 );
    out( " chosen=" ); out_dec( (ULONG)surface_formats[chosen_format_index].format );
    out( "/" ); out_dec( (ULONG)surface_formats[chosen_format_index].colorSpace );
    verdict( vr == VK_SUCCESS && vr2 == VK_SUCCESS && count > 0 &&
             surface_formats[chosen_format_index].format != VK_FORMAT_UNDEFINED,
             "the surface offered no usable format" );

    begin( "surface-present-modes" );
    count = SENTINEL_COUNT;
    vr = vkGetPhysicalDeviceSurfacePresentModesKHR( phys, surface, &count, NULL );
    count_a = count;
    if (count_a > 16) count_a = 16;
    count = count_a;
    memset( present_modes, 0, sizeof(present_modes) );
    vr2 = vkGetPhysicalDeviceSurfacePresentModesKHR( phys, surface, &count, present_modes );
    count_present_modes = count;
    out( "count-query=" ); out_int( vr );
    out( " count=" ); out_dec( count );
    out( " fetch=" ); out_int( vr2 );
    out( " modes=" );
    for (i = 0; i < count; i++) { if (i) out( "," ); out_dec( (ULONG)present_modes[i] ); }
    verdict( vr == VK_SUCCESS && vr2 == VK_SUCCESS && count > 0,
             "the surface offered no present mode" );

    /* ---- 5: the device ----------------------------------------------------- */
    begin( "device" );
    queue_info.queueFamilyIndex = family;
    queue_info.queueCount = 1;
    queue_info.pQueuePriorities = &queue_priority;
    device_info.queueCreateInfoCount = 1;
    device_info.pQueueCreateInfos = &queue_info;
    device_info.enabledExtensionCount = 1;
    device_info.ppEnabledExtensionNames = device_exts;
    vr = vkCreateDevice( phys, &device_info, NULL, &device );
    if (vr == VK_SUCCESS) vkGetDeviceQueue( device, family, 0, &queue );
    out( "vkCreateDevice=" ); out_int( vr );
    out( " queue=" ); out( queue ? "yes" : "no" );
    verdict( vr == VK_SUCCESS && device && queue, "no device" );
    if (vr != VK_SUCCESS) goto done;

    /* ---- 6: the swapchain --------------------------------------------------- */
    begin( "swapchain" );
    width = caps.currentExtent.width;
    height = caps.currentExtent.height;
    if (width == 0xffffffffu || width == 0) width = WIN_W;
    if (height == 0xffffffffu || height == 0) height = WIN_H;
    swapchain_info.surface = surface;
    swapchain_info.minImageCount = caps.minImageCount;
    if (caps.maxImageCount && swapchain_info.minImageCount > caps.maxImageCount)
        swapchain_info.minImageCount = caps.maxImageCount;
    swapchain_info.imageFormat = surface_formats[chosen_format_index].format;
    swapchain_info.imageColorSpace = surface_formats[chosen_format_index].colorSpace;
    swapchain_info.imageExtent.width = width;
    swapchain_info.imageExtent.height = height;
    swapchain_info.imageArrayLayers = 1;
    swapchain_info.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
                                VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                                VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    swapchain_info.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    swapchain_info.preTransform = caps.currentTransform;
    swapchain_info.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    swapchain_info.presentMode = VK_PRESENT_MODE_FIFO_KHR;
    swapchain_info.clipped = VK_TRUE;
    vr = vkCreateSwapchainKHR( device, &swapchain_info, NULL, &swapchain );
    out( "vkCreateSwapchainKHR=" ); out_int( vr );
    out( " asked=" ); out_dec( swapchain_info.minImageCount );
    out( " extent=" ); out_dec( width ); out( "x" ); out_dec( height );
    verdict( vr == VK_SUCCESS && swapchain != VK_NULL_HANDLE, "no swapchain" );
    if (vr != VK_SUCCESS) goto done;
    note_handle( "swapchain", 0, (ULONGLONG)swapchain );

    /* ---- 7a: the count query ------------------------------------------------ */
    begin( "images-count" );
    count_a = SENTINEL_COUNT;
    vr = vkGetSwapchainImagesKHR( device, swapchain, &count_a, NULL );
    out( "result=" ); out_int( vr );
    out( " count=" );
    if (count_a == SENTINEL_COUNT) out( "UNWRITTEN" ); else out_dec( count_a );
    verdict( vr == VK_SUCCESS && count_a != SENTINEL_COUNT && count_a > 0 &&
             count_a <= MAX_IMAGES && count_a >= swapchain_info.minImageCount,
             "the count query returned VK_SUCCESS and no usable count" );
    if (vr != VK_SUCCESS || count_a == SENTINEL_COUNT || !count_a || count_a > MAX_IMAGES)
        goto done;

    /* ---- 7b: the fetch ------------------------------------------------------ */
    begin( "images-fetch" );
    for (i = 0; i < MAX_IMAGES; i++) images_a[i] = (VkImage)SENTINEL_IMAGE;
    count_b = count_a;
    vr = VK_SUCCESS;
#if VK_SWAPCHAIN_BREAK != 4
    vr = vkGetSwapchainImagesKHR( device, swapchain, &count_b, images_a );
#endif
    nonnull = sentinels = distinct = 0;
    for (i = 0; i < count_b && i < MAX_IMAGES; i++)
    {
        note_handle( "image_a", i, (ULONGLONG)images_a[i] );
        if (images_a[i] == (VkImage)SENTINEL_IMAGE) { sentinels++; continue; }
        if (images_a[i] != VK_NULL_HANDLE) nonnull++;
        for (j = 0; j < i; j++) if (images_a[j] == images_a[i]) break;
        if (j == i) distinct++;
    }
    out( "result=" ); out_int( vr );
    out( " count=" ); out_dec( count_b );
    out( " nonnull=" ); out_dec( nonnull );
    out( " sentinels=" ); out_dec( sentinels );
    out( " distinct=" ); out_dec( distinct );
    verdict( vr == VK_SUCCESS && count_b == count_a && sentinels == 0 &&
             nonnull == count_b && distinct == count_b,
             "the fetch returned VK_SUCCESS and left images the caller cannot use" );

    /* ---- 7c: the fetch, repeated -------------------------------------------- */
    begin( "images-stable" );
    for (i = 0; i < MAX_IMAGES; i++) images_b[i] = (VkImage)SENTINEL_IMAGE;
    count = count_a;
    vr = vkGetSwapchainImagesKHR( device, swapchain, &count, images_b );
#if VK_SWAPCHAIN_BREAK == 5
    images_b[0] = (VkImage)(SENTINEL_IMAGE ^ 1);
#endif
    for (i = 0, j = 0; i < count && i < MAX_IMAGES; i++)
        if (images_b[i] == images_a[i]) j++;
    out( "result=" ); out_int( vr );
    out( " count=" ); out_dec( count );
    out( " same=" ); out_dec( j );
    verdict( vr == VK_SUCCESS && count == count_a && j == count_a,
             "two identical queries answered differently" );

    /* ---- 8: acquire ---------------------------------------------------------- */
    begin( "acquire" );
    vr = vkCreateFence( device, &fence_info, NULL, &fence );
    if (vr == VK_SUCCESS)
        vr = vkAcquireNextImageKHR( device, swapchain, ~0ull, VK_NULL_HANDLE, fence, &image_index );
    if (vr == VK_SUCCESS || vr == VK_SUBOPTIMAL_KHR)
    {
        vkWaitForFences( device, 1, &fence, VK_TRUE, ~0ull );
        vkResetFences( device, 1, &fence );
    }
    out( "result=" ); out_int( vr );
    out( " index=" );
    if (image_index == SENTINEL_COUNT) out( "UNWRITTEN" ); else out_dec( image_index );
    verdict( (vr == VK_SUCCESS || vr == VK_SUBOPTIMAL_KHR) &&
             image_index != SENTINEL_COUNT && image_index < count_a,
             "no acquirable image" );
    if (image_index >= count_a) goto done;

    /* ---- 9: clear, copy back before presenting, check every texel ------------ */
    begin( "readback" );
    pool_info.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    pool_info.queueFamilyIndex = family;
    vr = vkCreateCommandPool( device, &pool_info, NULL, &pool );
    cmd_alloc.commandPool = pool;
    cmd_alloc.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cmd_alloc.commandBufferCount = 1;
    if (!vr) vr = vkAllocateCommandBuffers( device, &cmd_alloc, &cmd );

    buffer_info.size = (VkDeviceSize)width * height * 4;
    buffer_info.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    buffer_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    if (!vr) vr = vkCreateBuffer( device, &buffer_info, NULL, &staging );
    memset( &mem_reqs, 0, sizeof(mem_reqs) );
    if (!vr) vkGetBufferMemoryRequirements( device, staging, &mem_reqs );
    memset( &memory_props, 0, sizeof(memory_props) );
    vkGetPhysicalDeviceMemoryProperties( phys, &memory_props );
    for (i = 0; i < memory_props.memoryTypeCount; i++)
    {
        if (!(mem_reqs.memoryTypeBits & (1u << i))) continue;
        if ((memory_props.memoryTypes[i].propertyFlags &
             (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)) !=
            (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT))
            continue;
        mem_type = i;
        break;
    }
    mem_info.allocationSize = mem_reqs.size;
    mem_info.memoryTypeIndex = mem_type;
    if (!vr && mem_type != ~0u) vr = vkAllocateMemory( device, &mem_info, NULL, &staging_mem );
    if (!vr) vr = vkBindBufferMemory( device, staging, staging_mem, 0 );

    memset( &range, 0, sizeof(range) );
    range.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    range.levelCount = 1;
    range.layerCount = 1;

    memset( &clear, 0, sizeof(clear) );
    clear.float32[0] = CLEAR_R;
    clear.float32[1] = CLEAR_G;
    clear.float32[2] = CLEAR_B;
    clear.float32[3] = CLEAR_A;

    if (!vr) vr = vkBeginCommandBuffer( cmd, &begin_info );
    if (!vr)
    {
        barrier.srcAccessMask = 0;
        barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.image = images_a[image_index];
        barrier.subresourceRange = range;
        vkCmdPipelineBarrier( cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                              0, 0, NULL, 0, NULL, 1, &barrier );

#if VK_SWAPCHAIN_BREAK != 1
        vkCmdClearColorImage( cmd, images_a[image_index], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                              &clear, 1, &range );
#endif

        barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        barrier.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
        barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        vkCmdPipelineBarrier( cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                              0, 0, NULL, 0, NULL, 1, &barrier );

        memset( &copy, 0, sizeof(copy) );
        copy.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        copy.imageSubresource.layerCount = 1;
        copy.imageExtent.width = width;
        copy.imageExtent.height = height;
        copy.imageExtent.depth = 1;
        vkCmdCopyImageToBuffer( cmd, images_a[image_index], VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                                staging, 1, &copy );

        barrier.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
        barrier.dstAccessMask = 0;
        barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        barrier.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
        vkCmdPipelineBarrier( cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
                              0, 0, NULL, 0, NULL, 1, &barrier );
        vr = vkEndCommandBuffer( cmd );
    }
    submit_info.commandBufferCount = 1;
    submit_info.pCommandBuffers = &cmd;
    if (!vr) vr = vkQueueSubmit( queue, 1, &submit_info, fence );
    if (!vr) vr = vkWaitForFences( device, 1, &fence, VK_TRUE, ~0ull );
    if (!vr) vr = vkMapMemory( device, staging_mem, 0, VK_WHOLE_SIZE, 0, (void **)&mapped );

    /* B8G8R8A8 is what this probe asks for; anything else and the byte order
     * is spelled out from the format rather than assumed. */
    expect[0] = (BYTE)(CLEAR_B * 255.0f);
    expect[1] = (BYTE)(CLEAR_G * 255.0f);
    expect[2] = (BYTE)(CLEAR_R * 255.0f);
    expect[3] = (BYTE)(CLEAR_A * 255.0f);
    if (swapchain_info.imageFormat == VK_FORMAT_R8G8B8A8_UNORM ||
        swapchain_info.imageFormat == VK_FORMAT_R8G8B8A8_SRGB)
    {
        expect[0] = (BYTE)(CLEAR_R * 255.0f);
        expect[2] = (BYTE)(CLEAR_B * 255.0f);
    }
#if VK_SWAPCHAIN_BREAK == 2
    { BYTE t = expect[0]; expect[0] = expect[2]; expect[2] = t; }
#endif
    if (mapped)
    {
        UINT32 texels = width * height;
#if VK_SWAPCHAIN_BREAK == 3
        texels = 1;
#endif
        for (i = 0; i < texels; i++)
        {
            checked++;
            for (j = 0; j < 4; j++)
                if (mapped[i * 4 + j] != expect[j]) { mismatches++; break; }
        }
        /* copied out BEFORE the unmap: the diagnostic below prints it, and a
         * mapping is only valid until vkUnmapMemory returns */
        for (j = 0; j < 4; j++) first4[j] = mapped[j];
        vkUnmapMemory( device, staging_mem );
        mapped = (BYTE *)1;   /* "there was a mapping", not "there is one" */
    }
    out( "result=" ); out_int( vr );
    out( " texels=" ); out_dec( (ULONG)width * height );
    out( " checked=" ); out_dec( checked );
    out( " mismatches=" ); out_dec( mismatches );
    out( " expect=" );
    out_hex( expect[0], 2 ); out_hex( expect[1], 2 );
    out_hex( expect[2], 2 ); out_hex( expect[3], 2 );
    if (mapped && mismatches)
    {
        out( " first=" );
        out_hex( first4[0], 2 ); out_hex( first4[1], 2 );
        out_hex( first4[2], 2 ); out_hex( first4[3], 2 );
    }
    verdict( vr == VK_SUCCESS && mapped && checked == width * height && mismatches == 0,
             "the acquired swapchain image did not read back as it was cleared" );

    /* ---- 10: present --------------------------------------------------------- */
    begin( "present" );
    present_result = VK_ERROR_UNKNOWN;
    present_info.swapchainCount = 1;
    present_info.pSwapchains = &swapchain;
    present_info.pImageIndices = &image_index;
    present_info.pResults = &present_result;
    vr = vkQueuePresentKHR( queue, &present_info );
    out( "result=" ); out_int( vr );
    out( " per-swapchain=" ); out_int( present_result );
    verdict( (vr == VK_SUCCESS || vr == VK_SUBOPTIMAL_KHR) &&
             (present_result == VK_SUCCESS || present_result == VK_SUBOPTIMAL_KHR),
             "the swapchain would not present" );
    vkDeviceWaitIdle( device );

    /* ---- 11: the image count is the one that was asked for ------------------- */
    /* THE CLAIM THE GAME DIED ON, stated generally and without naming it.
     *
     * On Windows a swapchain created with minImageCount = N has exactly N
     * images.  Every Windows Vulkan renderer is written to that, because it is
     * what every Windows ICD does; several size a fixed array from it and index
     * that array by the acquired index.  A host WSI is under no such obligation
     * -- the spec calls the field a MINIMUM -- and Mesa's Wayland WSI raises a
     * VK_PRESENT_MODE_MAILBOX_KHR swapchain to four images whatever was asked,
     * because its mailbox queue needs four.  A caller that asked for two then
     * gets four back with VK_SUCCESS, and cannot tell from the result that
     * anything happened.
     *
     * Normalising that is win32u's job, exactly as it already normalises
     * maxImageCount == 0 and the surface extents so that a Windows program sees
     * Windows numbers (dlls/win32u/vulkan.c, adjust_surface_capabilities).  It
     * does not, and this step is what says so.
     *
     * The request is SPEC-LEGAL: minImageCount is the surface's own reported
     * minimum, so no present mode may lawfully refuse it, and any count above
     * it is the host WSI substituting its own judgement for the caller's.  One
     * swapchain per supported present mode, created and destroyed in turn after
     * the main one is gone, so nothing here depends on two swapchains coexisting
     * on one surface. */
    if (swapchain)
    {
        vkDestroySwapchainKHR( device, swapchain, NULL );
        swapchain = VK_NULL_HANDLE;
    }
    begin( "count-contract" );
    out( "asked=" ); out_dec( caps.minImageCount );
    for (i = 0; i < count_present_modes && i < 16; i++)
    {
        VkSwapchainKHR probe_swapchain = VK_NULL_HANDLE;
        UINT32 got = SENTINEL_COUNT;

        swapchain_info.presentMode = present_modes[i];
        swapchain_info.minImageCount = caps.minImageCount;
        swapchain_info.oldSwapchain = VK_NULL_HANDLE;
        vr = vkCreateSwapchainKHR( device, &swapchain_info, NULL, &probe_swapchain );
        if (!vr) vr = vkGetSwapchainImagesKHR( device, probe_swapchain, &got, NULL );
        if (probe_swapchain) vkDestroySwapchainKHR( device, probe_swapchain, NULL );

        out( " mode" ); out_dec( (ULONG)present_modes[i] ); out( "=" );
        if (vr) { out( "err" ); out_int( vr ); contract_bad++; continue; }
        out_dec( got );
        if (got != caps.minImageCount) contract_bad++;
    }
    verdict( contract_bad == 0,
             "a swapchain was given more images than the caller asked for" );

done:
    if (device)
    {
        vkDeviceWaitIdle( device );
        if (staging) vkDestroyBuffer( device, staging, NULL );
        if (staging_mem) vkFreeMemory( device, staging_mem, NULL );
        if (fence) vkDestroyFence( device, fence, NULL );
        if (pool) vkDestroyCommandPool( device, pool, NULL );
        if (swapchain) vkDestroySwapchainKHR( device, swapchain, NULL );
        vkDestroyDevice( device, NULL );
    }
    if (surface) vkDestroySurfaceKHR( instance, surface, NULL );
    if (instance) vkDestroyInstance( instance, NULL );
    if (hwnd) DestroyWindow( hwnd );

    out( failures ? "vk_swapchain_smoke: FAIL " : "vk_swapchain_smoke: PASS " );
    out_dec( (ULONG)(step - failures) );
    out( "/" );
    out_dec( (ULONG)step );
    if (failures && first_fail)
    {
        out( " (" );
        out( first_fail );
        out( ")" );
    }
    out( "\n" );
    rc = failures ? 1 : 0;
    return rc;
}

#if defined(VK_SMOKE_NATIVE)
/* The native ppc64 PE leg: winegcc links a CRT, so this is an ordinary main. */
int main( void )
{
    return vk_swapchain_smoke_run();
}
#else
/* The guest leg has no C runtime: this IS the image entry point. */
void WINAPI vk_swapchain_smoke_entry( void )
{
    ExitProcess( (UINT)vk_swapchain_smoke_run() );
}
#endif
