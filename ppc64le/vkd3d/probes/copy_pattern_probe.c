/*
 * copy_pattern_probe.c -- does texture data survive the guest->vkd3d copy
 * path BYTE FOR BYTE?
 *
 * Written for the Cyberpunk 2077 corruption (2026-08-19): most textured
 * surfaces render with a fine regular speckle while UI and lights are clean,
 * settings-independent, clean on the emulated lane.  That pattern is
 * texture CONTENT damage, and the content crosses exactly one boundary this
 * probe can isolate: guest writes into a mapped upload buffer, then
 * CopyTextureRegion (the PLACED_FOOTPRINT form, served by
 * dlls/d3d12/main.c's hand_copy_texture_region) carries it to the texture,
 * and a second CopyTextureRegion brings it back to a readback buffer.
 *
 * Subtests, each PASS/FAIL on its own line:
 *
 *   buffer    CopyBufferRegion round trip, plain 64-bit stores.  The
 *             control: no texture footprints involved.
 *   texture   upload -> texture -> readback via CopyTextureRegion both ways,
 *             plain 64-bit stores.  Convicts or clears the hand walker and
 *             the footprint round trip.
 *   ntstore   the same texture trip, but the upload buffer is filled with
 *             SSE2 NON-TEMPORAL stores (_mm_stream_si128 + sfence) -- the
 *             way a streaming engine fills upload heaps.  If `texture`
 *             passes and this fails, the defect is the emulator's NT-store
 *             handling, not this port's marshalling.
 *
 * On mismatch it prints the first offset, expected/got, and a lane
 * histogram: how many bad bytes fall at each offset%16 -- the speckle's
 * stride signature, measured instead of guessed at from screenshots.
 *
 * No CRT: own entry (cp_entry), own memset/memcpy, output via WriteFile.
 * Imports only kernel32 and d3d12 (see copy_pattern_run.sh's .def files).
 *
 * Copyright 2026 the ppc64le port authors
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#define COBJMACROS
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d12.h>
#include <emmintrin.h>

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

static void out( const char *s )
{
    DWORD n, len = 0;
    while (s[len]) len++;
    WriteFile( GetStdHandle( STD_OUTPUT_HANDLE ), s, len, &n, NULL );
}

static void out_hex( unsigned long long v, int digits )
{
    char buf[17];
    int i;
    for (i = digits - 1; i >= 0; i--)
    {
        buf[i] = "0123456789abcdef"[v & 15];
        v >>= 4;
    }
    buf[digits] = 0;
    out( buf );
}

static void out_dec( unsigned long long v )
{
    char buf[21];
    int i = 20;
    buf[20] = 0;
    if (!v) buf[--i] = '0';
    while (v) { buf[--i] = '0' + (v % 10); v /= 10; }
    out( buf + i );
}

/* ---- the IIDs the probe asks for, spelled locally (no INITGUID) ---- */

static const GUID cp_IID_ID3D12Device =
    { 0x189819f1, 0x1db6, 0x4b57, { 0xbe, 0x54, 0x18, 0x21, 0x33, 0x9b, 0x85, 0xf7 } };
static const GUID cp_IID_ID3D12CommandQueue =
    { 0x0ec870a6, 0x5d7e, 0x4c22, { 0x8c, 0xfc, 0x5b, 0xaa, 0xe0, 0x76, 0x16, 0xed } };
static const GUID cp_IID_ID3D12CommandAllocator =
    { 0x6102dee4, 0xaf59, 0x4b09, { 0xb9, 0x99, 0xb4, 0x4d, 0x73, 0xf0, 0x9b, 0x24 } };
static const GUID cp_IID_ID3D12GraphicsCommandList =
    { 0x5b160d0f, 0xac1b, 0x4185, { 0x8b, 0xa8, 0xb3, 0xae, 0x42, 0xa5, 0xa4, 0x55 } };
static const GUID cp_IID_ID3D12Resource =
    { 0x696442be, 0xa72e, 0x4059, { 0xbc, 0x79, 0x5b, 0x5c, 0x98, 0x04, 0x0f, 0xad } };
static const GUID cp_IID_ID3D12Fence =
    { 0x0a753dcf, 0xc4d8, 0x4b91, { 0xad, 0xf6, 0xbe, 0x5a, 0x60, 0xd9, 0x5a, 0x76 } };

#define TEX_W 256
#define TEX_H 256
#define BPP   4

static ID3D12Device *dev;
static ID3D12CommandQueue *queue;
static ID3D12CommandAllocator *alloc;
static ID3D12GraphicsCommandList *list;
static ID3D12Fence *fence;
static UINT64 fence_value;

static int fail_count;

/* Fill 64-bit words that encode their own position, xored with a per-run
 * magic so a stale PREVIOUS upload can never compare equal. */
static UINT64 pattern_word( UINT64 magic, UINT64 word_index )
{
    return magic ^ (word_index * 0x9e3779b97f4a7c15ull) ^ (word_index << 32);
}

static HRESULT wait_idle( void )
{
    HRESULT hr;

    fence_value++;
    hr = ID3D12CommandQueue_Signal( queue, fence, fence_value );
    if (FAILED(hr)) return hr;
    /* the blocking form: a NULL event is served by the marshal table's
     * CA_EVENT row and vkd3d waits inline */
    return ID3D12Fence_SetEventOnCompletion( fence, fence_value, NULL );
}

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
            &cp_IID_ID3D12Resource, (void **)res );
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

static HRESULT make_texture( ID3D12Resource **res )
{
    D3D12_HEAP_PROPERTIES props;
    D3D12_RESOURCE_DESC desc;

    memset( &props, 0, sizeof(props) );
    props.Type = D3D12_HEAP_TYPE_DEFAULT;
    texture_desc( &desc );
    return ID3D12Device_CreateCommittedResource( dev, &props,
            D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_COPY_DEST, NULL,
            &cp_IID_ID3D12Resource, (void **)res );
}

static void barrier( ID3D12Resource *res, D3D12_RESOURCE_STATES from,
                     D3D12_RESOURCE_STATES to )
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

/* Compare `size` bytes of readback against the pattern; report and return
 * the number of mismatched bytes.  `pitch` and `row_bytes` describe the
 * footprint (for the buffer subtest pitch == row_bytes and rows == 1). */
static UINT64 check_pattern( const char *name, const unsigned char *got,
                             UINT64 magic, UINT rows, UINT row_bytes, UINT pitch )
{
    UINT64 bad = 0, first_off = ~0ull, first_want = 0, first_got = 0;
    UINT64 lane[16];
    UINT64 word_index = 0;
    UINT r, i, x;

    for (i = 0; i < 16; i++) lane[i] = 0;

    for (r = 0; r < rows; r++)
    {
        const unsigned char *row = got + (UINT64)r * pitch;
        for (x = 0; x + 8 <= row_bytes; x += 8, word_index++)
        {
            UINT64 want = pattern_word( magic, word_index );
            UINT64 have;
            memcpy( &have, row + x, 8 );
            if (have != want)
            {
                if (first_off == ~0ull)
                {
                    first_off = (UINT64)r * pitch + x;
                    first_want = want;
                    first_got = have;
                }
                for (i = 0; i < 8; i++)
                    if (((have >> (i * 8)) & 0xff) != ((want >> (i * 8)) & 0xff))
                    {
                        bad++;
                        lane[(x + i) & 15]++;
                    }
            }
        }
    }

    if (!bad)
    {
        out( name );
        out( ": PASS (" );
        out_dec( (UINT64)rows * row_bytes );
        out( " bytes byte-identical)\n" );
        return 0;
    }
    fail_count++;
    out( name );
    out( ": FAIL " );
    out_dec( bad );
    out( " bad byte(s); first at offset 0x" );
    out_hex( first_off, 8 );
    out( " want 0x" );
    out_hex( first_want, 16 );
    out( " got 0x" );
    out_hex( first_got, 16 );
    out( "\n  lane histogram (bad bytes at offset%16 = 0..15):" );
    for (i = 0; i < 16; i++)
    {
        out( " " );
        out_dec( lane[i] );
    }
    out( "\n" );
    return bad;
}

/* ---- subtest: buffer -> buffer round trip ---- */

static void test_buffer( void )
{
    static const UINT64 SIZE = 1024 * 1024;
    ID3D12Resource *up = NULL, *def = NULL, *rb = NULL;
    UINT64 magic = 0x42555ffeed000001ull;
    unsigned char *p = NULL;
    D3D12_RANGE none = { 0, 0 };
    UINT64 i;
    HRESULT hr;

    if (FAILED(make_buffer( D3D12_HEAP_TYPE_UPLOAD, SIZE,
                            D3D12_RESOURCE_STATE_GENERIC_READ, &up )) ||
        FAILED(make_buffer( D3D12_HEAP_TYPE_DEFAULT, SIZE,
                            D3D12_RESOURCE_STATE_COPY_DEST, &def )) ||
        FAILED(make_buffer( D3D12_HEAP_TYPE_READBACK, SIZE,
                            D3D12_RESOURCE_STATE_COPY_DEST, &rb )))
    {
        out( "buffer: SKIP (resource creation failed)\n" );
        fail_count++;
        return;
    }

    hr = ID3D12Resource_Map( up, 0, NULL, (void **)&p );
    if (FAILED(hr)) { out( "buffer: SKIP (map failed)\n" ); fail_count++; return; }
    for (i = 0; i < SIZE / 8; i++)
        ((UINT64 *)p)[i] = pattern_word( magic, i );
    ID3D12Resource_Unmap( up, 0, NULL );

    ID3D12GraphicsCommandList_CopyBufferRegion( list, def, 0, up, 0, SIZE );
    barrier( def, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COPY_SOURCE );
    ID3D12GraphicsCommandList_CopyBufferRegion( list, rb, 0, def, 0, SIZE );
    ID3D12GraphicsCommandList_Close( list );
    ID3D12CommandQueue_ExecuteCommandLists( queue, 1, (ID3D12CommandList **)&list );
    if (FAILED(wait_idle())) { out( "buffer: SKIP (fence wait failed)\n" ); fail_count++; return; }
    ID3D12CommandAllocator_Reset( alloc );
    ID3D12GraphicsCommandList_Reset( list, alloc, NULL );

    hr = ID3D12Resource_Map( rb, 0, NULL, (void **)&p );
    if (FAILED(hr)) { out( "buffer: SKIP (readback map failed)\n" ); fail_count++; return; }
    check_pattern( "buffer", p, magic, 1, (UINT)SIZE, (UINT)SIZE );
    ID3D12Resource_Unmap( rb, 0, &none );

    ID3D12Resource_Release( up );
    ID3D12Resource_Release( def );
    ID3D12Resource_Release( rb );
}

/* ---- subtest: upload -> texture -> readback, both CopyTextureRegion forms.
 * `nt` picks the fill: 0 = plain 64-bit stores, 1 = SSE2 non-temporal
 * streaming stores, the way an engine fills an upload heap. ---- */

static void test_texture( const char *name, UINT64 magic, int nt )
{
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp;
    D3D12_TEXTURE_COPY_LOCATION dst, src;
    D3D12_RESOURCE_DESC desc;
    ID3D12Resource *up = NULL, *tex = NULL, *rb = NULL;
    unsigned char *p = NULL;
    UINT64 total = 0, word = 0;
    UINT rows = 0, r, x;
    UINT64 row_size = 0;
    D3D12_RANGE none = { 0, 0 };
    HRESULT hr;

    if (FAILED(make_texture( &tex )))
    {
        out( name ); out( ": SKIP (texture creation failed)\n" );
        fail_count++;
        return;
    }
    texture_desc( &desc );
    memset( &fp, 0, sizeof(fp) );
    ID3D12Device_GetCopyableFootprints( dev, &desc, 0, 1, 0, &fp, &rows,
                                        &row_size, &total );
    if (!total || rows != TEX_H || row_size != TEX_W * BPP)
    {
        out( name ); out( ": FAIL (GetCopyableFootprints answered rows=" );
        out_dec( rows ); out( " row_size=" ); out_dec( row_size );
        out( " total=" ); out_dec( total ); out( ")\n" );
        fail_count++;
        return;
    }

    if (FAILED(make_buffer( D3D12_HEAP_TYPE_UPLOAD, total,
                            D3D12_RESOURCE_STATE_GENERIC_READ, &up )) ||
        FAILED(make_buffer( D3D12_HEAP_TYPE_READBACK, total,
                            D3D12_RESOURCE_STATE_COPY_DEST, &rb )))
    {
        out( name ); out( ": SKIP (buffer creation failed)\n" );
        fail_count++;
        return;
    }

    hr = ID3D12Resource_Map( up, 0, NULL, (void **)&p );
    if (FAILED(hr)) { out( name ); out( ": SKIP (map failed)\n" ); fail_count++; return; }
    for (r = 0; r < rows; r++)
    {
        unsigned char *row = p + (UINT64)r * fp.Footprint.RowPitch;
        if (!nt)
        {
            for (x = 0; x + 8 <= row_size; x += 8, word++)
            {
                UINT64 w = pattern_word( magic, word );
                memcpy( row + x, &w, 8 );
            }
        }
        else
        {
            for (x = 0; x + 16 <= row_size; x += 16, word += 2)
            {
                __m128i v = _mm_set_epi64x(
                        (long long)pattern_word( magic, word + 1 ),
                        (long long)pattern_word( magic, word ) );
                _mm_stream_si128( (__m128i *)(row + x), v );
            }
        }
    }
    if (nt) _mm_sfence();
    ID3D12Resource_Unmap( up, 0, NULL );

    memset( &dst, 0, sizeof(dst) );
    dst.pResource = tex;
    dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    dst.SubresourceIndex = 0;
    memset( &src, 0, sizeof(src) );
    src.pResource = up;
    src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    src.PlacedFootprint = fp;
    ID3D12GraphicsCommandList_CopyTextureRegion( list, &dst, 0, 0, 0, &src, NULL );

    barrier( tex, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COPY_SOURCE );

    memset( &dst, 0, sizeof(dst) );
    dst.pResource = rb;
    dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    dst.PlacedFootprint = fp;
    memset( &src, 0, sizeof(src) );
    src.pResource = tex;
    src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    src.SubresourceIndex = 0;
    ID3D12GraphicsCommandList_CopyTextureRegion( list, &dst, 0, 0, 0, &src, NULL );

    ID3D12GraphicsCommandList_Close( list );
    ID3D12CommandQueue_ExecuteCommandLists( queue, 1, (ID3D12CommandList **)&list );
    if (FAILED(wait_idle())) { out( name ); out( ": SKIP (fence wait failed)\n" ); fail_count++; return; }
    ID3D12CommandAllocator_Reset( alloc );
    ID3D12GraphicsCommandList_Reset( list, alloc, NULL );

    hr = ID3D12Resource_Map( rb, 0, NULL, (void **)&p );
    if (FAILED(hr)) { out( name ); out( ": SKIP (readback map failed)\n" ); fail_count++; return; }
    check_pattern( name, p, magic, rows, (UINT)row_size, fp.Footprint.RowPitch );
    ID3D12Resource_Unmap( rb, 0, &none );

    ID3D12Resource_Release( up );
    ID3D12Resource_Release( tex );
    ID3D12Resource_Release( rb );
}

void __attribute__((noreturn)) cp_entry( void )
{
    D3D12_COMMAND_QUEUE_DESC qdesc;
    HRESULT hr;

    hr = D3D12CreateDevice( NULL, D3D_FEATURE_LEVEL_12_0,
                            &cp_IID_ID3D12Device, (void **)&dev );
    if (FAILED(hr))
    {
        out( "copy_pattern_probe: no device, hr 0x" );
        out_hex( (UINT)hr, 8 );
        out( "\n" );
        ExitProcess( 2 );
    }

    memset( &qdesc, 0, sizeof(qdesc) );
    qdesc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    if (FAILED(ID3D12Device_CreateCommandQueue( dev, &qdesc,
                    &cp_IID_ID3D12CommandQueue, (void **)&queue )) ||
        FAILED(ID3D12Device_CreateCommandAllocator( dev,
                    D3D12_COMMAND_LIST_TYPE_DIRECT,
                    &cp_IID_ID3D12CommandAllocator, (void **)&alloc )) ||
        FAILED(ID3D12Device_CreateCommandList( dev, 0,
                    D3D12_COMMAND_LIST_TYPE_DIRECT, alloc, NULL,
                    &cp_IID_ID3D12GraphicsCommandList, (void **)&list )) ||
        FAILED(ID3D12Device_CreateFence( dev, 0, D3D12_FENCE_FLAG_NONE,
                    &cp_IID_ID3D12Fence, (void **)&fence )))
    {
        out( "copy_pattern_probe: queue/list/fence creation failed\n" );
        ExitProcess( 2 );
    }

    test_buffer();
    test_texture( "texture", 0x7465780000000001ull, 0 );
    test_texture( "ntstore", 0x6e74730000000001ull, 1 );

    if (fail_count)
    {
        out( "copy_pattern_probe: FAIL (" );
        out_dec( fail_count );
        out( " subtest(s))\n" );
        ExitProcess( 1 );
    }
    out( "copy_pattern_probe: PASS\n" );
    ExitProcess( 0 );
}
