/*
 * copy_storm_probe.c -- copy_pattern_probe's quiet round trip, scaled to
 * game shape: does texture-scale traffic survive the guest->vkd3d path?
 *
 * The quiet single-buffer probe is byte-clean in both placements while
 * Cyberpunk 2077 speckles at scale, and the HOST-native storm
 * (host_vk_storm.c, ~7.7 GB per leg over five queues) is byte-clean too --
 * so whatever corrupts needs BOTH the port's guest->GPU path AND scale.
 * This probe is that scale, from the guest side:
 *
 *   - STORM_THREADS guest threads (default 3), each with its own queue --
 *     thread 0 DIRECT, 1 COPY, 2 COMPUTE, cycling -- allocators, lists,
 *     fences; DEPTH submissions in flight per thread,
 *   - per iteration: fill an upload buffer, round-trip it through a
 *     default-heap buffer (even iterations) or a 2048x2048 RGBA8 texture
 *     via CopyTextureRegion (odd iterations), read back, verify,
 *   - fills rotate through the three ways a game writes an upload heap:
 *     plain 64-bit stores, SSE2 non-temporal streaming stores, and
 *     REP MOVSB out of ordinary system RAM (decompress-then-memcpy),
 *   - THREE verdicts per iteration, each naming a different culprit:
 *       staging-pre   CPU re-reads the upload bytes right after the fill;
 *                     damage here is the emulator's store/read path, the
 *                     GPU never saw the bytes,
 *       roundtrip     the readback mismatches while staging was clean:
 *                     the copy machinery (marshal walker, vkd3d, kernel),
 *       staging-post  the upload bytes changed while the GPU trip ran:
 *                     something else scribbled guest-visible memory.
 *
 * Environment (all optional): STORM_THREADS, STORM_ITERS (default 32),
 * STORM_CHUNK_MB (default 32), VKD3D_CONFIG=no_upload_hvv for the GTT leg
 * (the runner drives both).  Default traffic: 3 threads x 32 iters
 * alternating 32 MiB buffers and 16 MiB textures = ~2.3 GiB verified.
 *
 * On mismatch: first offsets, expected/got, and the offset%16 lane
 * histogram, per verdict class.
 *
 * No CRT: own entry (cs_entry), output via WriteFile.  Imports kernel32
 * and d3d12 only (see copy_storm_run.sh).
 *
 * Copyright 2026 the ppc64le port authors
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#define COBJMACROS
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d12.h>
#include <emmintrin.h>

#define DEPTH 2
#define MAX_THREADS 8
#define TEX_W 2048
#define TEX_H 2048
#define TEX_BYTES ((UINT64)TEX_W * TEX_H * 4)

/* ---- no-CRT support ---- */

void *memset( void *d, int c, size_t n )
{
    unsigned char *p = d;
    while (n--) *p++ = (unsigned char)c;
    return d;
}

void *memcpy( void *d, const void *s, size_t n )
{
    unsigned char *dp = d;
    const unsigned char *sp = s;
    while (n--) *dp++ = *sp++;
    return d;
}

static LONG out_lock;

static void lock( void )   { while (InterlockedCompareExchange( &out_lock, 1, 0 )) ; }
static void unlock( void ) { InterlockedExchange( &out_lock, 0 ); }

static void out_raw( const char *s )
{
    DWORD n, len = 0;
    while (s[len]) len++;
    WriteFile( GetStdHandle( STD_OUTPUT_HANDLE ), s, len, &n, NULL );
}

static void out_hex( char *buf, unsigned long long v, int digits )
{
    for (int i = digits - 1; i >= 0; i--)
    {
        buf[i] = "0123456789abcdef"[v & 15];
        v >>= 4;
    }
    buf[digits] = 0;
}

static void out_dec_buf( char *dst, unsigned long long v, int *len )
{
    char buf[21];
    int i = 20;
    buf[20] = 0;
    if (!v) buf[--i] = '0';
    while (v) { buf[--i] = '0' + (v % 10); v /= 10; }
    int n = 0;
    while (buf[i]) dst[n++] = buf[i++];
    dst[n] = 0;
    *len = n;
}

/* tiny formatted line builder so threads emit whole lines */
struct line { char buf[512]; int len; };
static void ladd( struct line *l, const char *s )
{
    while (*s && l->len < 500) l->buf[l->len++] = *s++;
    l->buf[l->len] = 0;
}
static void ldec( struct line *l, unsigned long long v )
{
    int n; char tmp[24];
    out_dec_buf( tmp, v, &n );
    ladd( l, tmp );
}
static void lhex( struct line *l, unsigned long long v, int digits )
{
    char tmp[20];
    out_hex( tmp, v, digits );
    ladd( l, tmp );
}
static void lflush( struct line *l )
{
    ladd( l, "\n" );
    lock();
    out_raw( l->buf );
    unlock();
    l->len = 0;
    l->buf[0] = 0;
}

/* ---- IIDs ---- */

static const GUID cs_IID_ID3D12Device =
    { 0x189819f1, 0x1db6, 0x4b57, { 0xbe, 0x54, 0x18, 0x21, 0x33, 0x9b, 0x85, 0xf7 } };
static const GUID cs_IID_ID3D12CommandQueue =
    { 0x0ec870a6, 0x5d7e, 0x4c22, { 0x8c, 0xfc, 0x5b, 0xaa, 0xe0, 0x76, 0x16, 0xed } };
static const GUID cs_IID_ID3D12CommandAllocator =
    { 0x6102dee4, 0xaf59, 0x4b09, { 0xb9, 0x99, 0xb4, 0x4d, 0x73, 0xf0, 0x9b, 0x24 } };
static const GUID cs_IID_ID3D12GraphicsCommandList =
    { 0x5b160d0f, 0xac1b, 0x4185, { 0x8b, 0xa8, 0xb3, 0xae, 0x42, 0xa5, 0xa4, 0x55 } };
static const GUID cs_IID_ID3D12Resource =
    { 0x696442be, 0xa72e, 0x4059, { 0xbc, 0x79, 0x5b, 0x5c, 0x98, 0x04, 0x0f, 0xad } };
static const GUID cs_IID_ID3D12Fence =
    { 0x0a753dcf, 0xc4d8, 0x4b91, { 0xad, 0xf6, 0xbe, 0x5a, 0x60, 0xd9, 0x5a, 0x76 } };

static ID3D12Device *dev;
static UINT64 chunk_bytes;
static int iters, nthreads;

/* verdict counters, shared */
static LONG64 bad_staging_pre, bad_roundtrip, bad_staging_post;
static LONG64 lane_hist[3][16];
static LONG first_reports;

static UINT64 pattern_word( UINT64 magic, UINT64 word_index )
{
    return magic ^ (word_index * 0x9e3779b97f4a7c15ull) ^ (word_index << 32);
}

struct set
{
    ID3D12Resource *up, *def, *rb, *tex;
    ID3D12CommandAllocator *alloc;
    ID3D12GraphicsCommandList *list;
    unsigned char *up_map, *rb_map;
    UINT64 magic;
    UINT64 nbytes;          /* bytes filled/verified this trip */
    UINT pitch, rows, row_bytes;   /* texture trip footprint; rows==1 for buffer */
    int via_tex;
    int in_flight;
    UINT64 fence_target;
};

struct storm_thread
{
    int id;
    D3D12_COMMAND_LIST_TYPE qtype;
    ID3D12CommandQueue *queue;
    ID3D12Fence *fence;
    UINT64 fence_value;
    struct set sets[DEPTH];
    unsigned char *scratch;        /* system-RAM source for the movsb fill */
    HANDLE handle;
    int failed_setup;
};

static struct storm_thread threads[MAX_THREADS];

static HRESULT make_buffer( D3D12_HEAP_TYPE heap, UINT64 size,
                            D3D12_RESOURCE_STATES state, ID3D12Resource **res )
{
    D3D12_HEAP_PROPERTIES props;
    D3D12_RESOURCE_DESC desc;

    memset( &props, 0, sizeof(props) );
    props.Type = heap;
    memset( &desc, 0, sizeof(desc) );
    desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    desc.Width = size;
    desc.Height = 1;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.Format = DXGI_FORMAT_UNKNOWN;
    desc.SampleDesc.Count = 1;
    desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    return ID3D12Device_CreateCommittedResource( dev, &props,
            D3D12_HEAP_FLAG_NONE, &desc, state, NULL,
            &cs_IID_ID3D12Resource, (void **)res );
}

static void texture_desc( D3D12_RESOURCE_DESC *desc )
{
    memset( desc, 0, sizeof(*desc) );
    desc->Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    desc->Width = TEX_W;
    desc->Height = TEX_H;
    desc->DepthOrArraySize = 1;
    desc->MipLevels = 1;
    desc->Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    desc->SampleDesc.Count = 1;
    desc->Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
}

/* fill mode rotates per iteration */
static void fill_upload( struct storm_thread *t, struct set *s, int it )
{
    UINT64 words_per_row = s->row_bytes / 8;
    UINT64 word = 0;
    int mode = it % 3;

    for (UINT r = 0; r < s->rows; r++)
    {
        unsigned char *row = s->up_map + (UINT64)r * s->pitch;
        switch (mode)
        {
        case 0:  /* plain 64-bit stores */
            for (UINT64 x = 0; x < words_per_row; x++, word++)
                ((UINT64 *)row)[x] = pattern_word( s->magic, word );
            break;
        case 1:  /* SSE2 non-temporal streaming stores */
            for (UINT64 x = 0; x + 2 <= words_per_row; x += 2, word += 2)
            {
                __m128i v = _mm_set_epi64x(
                        (long long)pattern_word( s->magic, word + 1 ),
                        (long long)pattern_word( s->magic, word ) );
                _mm_stream_si128( (__m128i *)(row + x * 8), v );
            }
            break;
        case 2:  /* pattern into system RAM, then REP MOVSB across */
        {
            for (UINT64 x = 0; x < words_per_row; x++, word++)
                ((UINT64 *)t->scratch)[x] = pattern_word( s->magic, word );
            void *dst = row;
            const void *src = t->scratch;
            UINT64 n = s->row_bytes;
            __asm__ volatile( "rep movsb"
                              : "+D"(dst), "+S"(src), "+c"(n)
                              :
                              : "memory" );
            break;
        }
        }
    }
    if (mode == 1) _mm_sfence();
}

/* verify `base` against the set's pattern; class 0/1/2 = pre/roundtrip/post */
static UINT64 verify( struct storm_thread *t, struct set *s,
                      const unsigned char *base, UINT pitch, int cls, int it )
{
    static const char *cls_name[3] = { "staging-pre", "roundtrip", "staging-post" };
    UINT64 bad = 0, first_off = ~0ull, first_want = 0, first_got = 0;
    UINT64 word = 0;

    for (UINT r = 0; r < s->rows; r++)
    {
        const unsigned char *row = base + (UINT64)r * pitch;
        for (UINT64 x = 0; x + 8 <= s->row_bytes; x += 8, word++)
        {
            UINT64 want = pattern_word( s->magic, word );
            UINT64 have;
            memcpy( &have, row + x, 8 );
            if (have == want) continue;
            if (first_off == ~0ull)
            {
                first_off = (UINT64)r * pitch + x;
                first_want = want;
                first_got = have;
            }
            for (int b = 0; b < 8; b++)
                if (((have >> (b * 8)) & 0xff) != ((want >> (b * 8)) & 0xff))
                {
                    bad++;
                    InterlockedIncrement64( &lane_hist[cls][(x + b) & 15] );
                }
        }
    }
    if (!bad) return 0;

    switch (cls)
    {
    case 0: InterlockedAdd64( &bad_staging_pre, (LONG64)bad ); break;
    case 1: InterlockedAdd64( &bad_roundtrip, (LONG64)bad ); break;
    case 2: InterlockedAdd64( &bad_staging_post, (LONG64)bad ); break;
    }
    if (InterlockedIncrement( &first_reports ) <= 12)
    {
        struct line l = { { 0 }, 0 };
        ladd( &l, "MISMATCH " );
        ladd( &l, cls_name[cls] );
        ladd( &l, " thread " );
        ldec( &l, t->id );
        ladd( &l, " iter " );
        ldec( &l, it );
        ladd( &l, s->via_tex ? " (texture" : " (buffer" );
        ladd( &l, " fill " );
        ldec( &l, it % 3 );
        ladd( &l, "): " );
        ldec( &l, bad );
        ladd( &l, " bad bytes, first at 0x" );
        lhex( &l, first_off, 8 );
        ladd( &l, " want 0x" );
        lhex( &l, first_want, 16 );
        ladd( &l, " got 0x" );
        lhex( &l, first_got, 16 );
        lflush( &l );
    }
    return bad;
}

static void barrier_on( ID3D12GraphicsCommandList *list, ID3D12Resource *res,
                        D3D12_RESOURCE_STATES from, D3D12_RESOURCE_STATES to )
{
    D3D12_RESOURCE_BARRIER b;

    memset( &b, 0, sizeof(b) );
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource = res;
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    b.Transition.StateBefore = from;
    b.Transition.StateAfter = to;
    ID3D12GraphicsCommandList_ResourceBarrier( list, 1, &b );
}

static void record_trip( struct set *s )
{
    if (!s->via_tex)
    {
        ID3D12GraphicsCommandList_CopyBufferRegion( s->list, s->def, 0,
                                                    s->up, 0, s->nbytes );
        barrier_on( s->list, s->def, D3D12_RESOURCE_STATE_COPY_DEST,
                    D3D12_RESOURCE_STATE_COPY_SOURCE );
        ID3D12GraphicsCommandList_CopyBufferRegion( s->list, s->rb, 0,
                                                    s->def, 0, s->nbytes );
        barrier_on( s->list, s->def, D3D12_RESOURCE_STATE_COPY_SOURCE,
                    D3D12_RESOURCE_STATE_COPY_DEST );
    }
    else
    {
        D3D12_TEXTURE_COPY_LOCATION dst, src;
        D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp;

        memset( &fp, 0, sizeof(fp) );
        fp.Footprint.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        fp.Footprint.Width = TEX_W;
        fp.Footprint.Height = TEX_H;
        fp.Footprint.Depth = 1;
        fp.Footprint.RowPitch = s->pitch;

        memset( &dst, 0, sizeof(dst) );
        dst.pResource = s->tex;
        dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        memset( &src, 0, sizeof(src) );
        src.pResource = s->up;
        src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        src.PlacedFootprint = fp;
        ID3D12GraphicsCommandList_CopyTextureRegion( s->list, &dst, 0, 0, 0,
                                                     &src, NULL );

        barrier_on( s->list, s->tex, D3D12_RESOURCE_STATE_COPY_DEST,
                    D3D12_RESOURCE_STATE_COPY_SOURCE );

        memset( &dst, 0, sizeof(dst) );
        dst.pResource = s->rb;
        dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        dst.PlacedFootprint = fp;
        memset( &src, 0, sizeof(src) );
        src.pResource = s->tex;
        src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        ID3D12GraphicsCommandList_CopyTextureRegion( s->list, &dst, 0, 0, 0,
                                                     &src, NULL );

        barrier_on( s->list, s->tex, D3D12_RESOURCE_STATE_COPY_SOURCE,
                    D3D12_RESOURCE_STATE_COPY_DEST );
    }
}

static int retire( struct storm_thread *t, struct set *s, int it_retired )
{
    if (FAILED(ID3D12Fence_SetEventOnCompletion( t->fence, s->fence_target, NULL )))
        return -1;
    verify( t, s, s->rb_map, s->via_tex ? s->pitch : (UINT)s->nbytes, 1, it_retired );
    verify( t, s, s->up_map, s->via_tex ? s->pitch : (UINT)s->nbytes, 2, it_retired );
    ID3D12CommandAllocator_Reset( s->alloc );
    ID3D12GraphicsCommandList_Reset( s->list, s->alloc, NULL );
    s->in_flight = 0;
    return 0;
}

static DWORD WINAPI thread_main( LPVOID arg )
{
    struct storm_thread *t = arg;
    int retired_it[DEPTH];

    for (int it = 0; it < iters; it++)
    {
        struct set *s = &t->sets[it % DEPTH];

        if (s->in_flight && retire( t, s, retired_it[it % DEPTH] ) < 0)
            return 1;

        s->via_tex = it & 1;
        s->magic = 0x570043ull ^ ((UINT64)t->id << 56) ^
                   ((UINT64)it * 0xd1342543de82ef95ull);
        if (s->via_tex)
        {
            s->rows = TEX_H;
            s->row_bytes = TEX_W * 4;
            s->pitch = TEX_W * 4;      /* 8192: already 256-aligned */
            s->nbytes = TEX_BYTES;
        }
        else
        {
            s->rows = 1;
            s->row_bytes = (UINT)chunk_bytes;
            s->pitch = (UINT)chunk_bytes;
            s->nbytes = chunk_bytes;
        }

        fill_upload( t, s, it );
        verify( t, s, s->up_map, s->pitch, 0, it );

        record_trip( s );
        ID3D12GraphicsCommandList_Close( s->list );
        ID3D12CommandQueue_ExecuteCommandLists( t->queue, 1,
                (ID3D12CommandList **)&s->list );
        s->fence_target = ++t->fence_value;
        ID3D12CommandQueue_Signal( t->queue, t->fence, s->fence_target );
        s->in_flight = 1;
        retired_it[it % DEPTH] = it;
    }

    for (int d = 0; d < DEPTH; d++)
    {
        struct set *s = &t->sets[d];
        if (s->in_flight && retire( t, s, retired_it[d] ) < 0) return 1;
    }
    return 0;
}

static int env_int( const char *name, int fallback )
{
    char buf[32];
    DWORD n = GetEnvironmentVariableA( name, buf, sizeof(buf) );
    if (!n || n >= sizeof(buf)) return fallback;
    int v = 0;
    for (DWORD i = 0; i < n; i++)
        if (buf[i] >= '0' && buf[i] <= '9') v = v * 10 + (buf[i] - '0');
    return v ? v : fallback;
}

static int setup_thread( struct storm_thread *t, int id )
{
    static const D3D12_COMMAND_LIST_TYPE types[3] = {
        D3D12_COMMAND_LIST_TYPE_DIRECT,
        D3D12_COMMAND_LIST_TYPE_COPY,
        D3D12_COMMAND_LIST_TYPE_COMPUTE,
    };
    D3D12_COMMAND_QUEUE_DESC qdesc;

    t->id = id;
    t->qtype = types[id % 3];
    memset( &qdesc, 0, sizeof(qdesc) );
    qdesc.Type = t->qtype;
    if (FAILED(ID3D12Device_CreateCommandQueue( dev, &qdesc,
                    &cs_IID_ID3D12CommandQueue, (void **)&t->queue )) ||
        FAILED(ID3D12Device_CreateFence( dev, 0, D3D12_FENCE_FLAG_NONE,
                    &cs_IID_ID3D12Fence, (void **)&t->fence )))
        return -1;

    t->scratch = VirtualAlloc( NULL, chunk_bytes, MEM_COMMIT | MEM_RESERVE,
                               PAGE_READWRITE );
    if (!t->scratch) return -1;

    for (int d = 0; d < DEPTH; d++)
    {
        struct set *s = &t->sets[d];
        D3D12_HEAP_PROPERTIES props;
        D3D12_RESOURCE_DESC desc;

        if (FAILED(make_buffer( D3D12_HEAP_TYPE_UPLOAD, chunk_bytes,
                                D3D12_RESOURCE_STATE_GENERIC_READ, &s->up )) ||
            FAILED(make_buffer( D3D12_HEAP_TYPE_DEFAULT, chunk_bytes,
                                D3D12_RESOURCE_STATE_COPY_DEST, &s->def )) ||
            FAILED(make_buffer( D3D12_HEAP_TYPE_READBACK, chunk_bytes,
                                D3D12_RESOURCE_STATE_COPY_DEST, &s->rb )))
            return -1;

        memset( &props, 0, sizeof(props) );
        props.Type = D3D12_HEAP_TYPE_DEFAULT;
        texture_desc( &desc );
        if (FAILED(ID3D12Device_CreateCommittedResource( dev, &props,
                        D3D12_HEAP_FLAG_NONE, &desc,
                        D3D12_RESOURCE_STATE_COPY_DEST, NULL,
                        &cs_IID_ID3D12Resource, (void **)&s->tex )))
            return -1;

        if (FAILED(ID3D12Resource_Map( s->up, 0, NULL, (void **)&s->up_map )) ||
            FAILED(ID3D12Resource_Map( s->rb, 0, NULL, (void **)&s->rb_map )))
            return -1;

        /* COPY-type lists must come from COPY-type allocators &c. */
        if (FAILED(ID3D12Device_CreateCommandAllocator( dev, t->qtype,
                        &cs_IID_ID3D12CommandAllocator, (void **)&s->alloc )) ||
            FAILED(ID3D12Device_CreateCommandList( dev, 0, t->qtype, s->alloc,
                        NULL, &cs_IID_ID3D12GraphicsCommandList,
                        (void **)&s->list )))
            return -1;
    }
    return 0;
}

void __attribute__((noreturn)) cs_entry( void )
{
    struct line l = { { 0 }, 0 };
    HRESULT hr;

    nthreads = env_int( "STORM_THREADS", 3 );
    if (nthreads > MAX_THREADS) nthreads = MAX_THREADS;
    iters = env_int( "STORM_ITERS", 32 );
    chunk_bytes = (UINT64)env_int( "STORM_CHUNK_MB", 32 ) << 20;
    if (chunk_bytes < TEX_BYTES) chunk_bytes = TEX_BYTES;

    hr = D3D12CreateDevice( NULL, D3D_FEATURE_LEVEL_12_0,
                            &cs_IID_ID3D12Device, (void **)&dev );
    if (FAILED(hr))
    {
        ladd( &l, "copy_storm_probe: no device, hr 0x" );
        lhex( &l, (UINT)hr, 8 );
        lflush( &l );
        ExitProcess( 2 );
    }

    ladd( &l, "copy_storm_probe: " );
    ldec( &l, nthreads );
    ladd( &l, " threads (queue types DIRECT/COPY/COMPUTE cycling), " );
    ldec( &l, iters );
    ladd( &l, " iters, chunk " );
    ldec( &l, chunk_bytes >> 20 );
    ladd( &l, " MiB, depth 2" );
    lflush( &l );

    for (int i = 0; i < nthreads; i++)
        if (setup_thread( &threads[i], i ) < 0)
        {
            ladd( &l, "copy_storm_probe: SKIP (thread " );
            ldec( &l, i );
            ladd( &l, " setup failed)" );
            lflush( &l );
            ExitProcess( 2 );
        }

    for (int i = 0; i < nthreads; i++)
        threads[i].handle = CreateThread( NULL, 0, thread_main, &threads[i],
                                          0, NULL );
    for (int i = 0; i < nthreads; i++)
        WaitForSingleObject( threads[i].handle, INFINITE );

    UINT64 traffic = 0;
    for (int i = 0; i < nthreads; i++)
        traffic += ((UINT64)(iters + 1) / 2) * chunk_bytes +
                   ((UINT64)iters / 2) * TEX_BYTES;

    ladd( &l, "copy_storm_probe: ~" );
    ldec( &l, traffic >> 20 );
    ladd( &l, " MiB round-tripped" );
    lflush( &l );

    if (bad_staging_pre + bad_roundtrip + bad_staging_post)
    {
        static const char *cls_name[3] = { "staging-pre", "roundtrip", "staging-post" };
        LONG64 counts[3];
        counts[0] = bad_staging_pre;
        counts[1] = bad_roundtrip;
        counts[2] = bad_staging_post;
        for (int c = 0; c < 3; c++)
        {
            ladd( &l, "  " );
            ladd( &l, cls_name[c] );
            ladd( &l, ": " );
            ldec( &l, (UINT64)counts[c] );
            ladd( &l, " bad bytes; lanes:" );
            for (int i = 0; i < 16; i++)
            {
                ladd( &l, " " );
                ldec( &l, (UINT64)lane_hist[c][i] );
            }
            lflush( &l );
        }
        ladd( &l, "copy_storm_probe: FAIL" );
        lflush( &l );
        ExitProcess( 1 );
    }
    ladd( &l, "copy_storm_probe: PASS" );
    lflush( &l );
    ExitProcess( 0 );
}
