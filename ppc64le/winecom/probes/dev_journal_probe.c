/*
 * dev_journal_probe.c -- drive the DEVICE JOURNAL (the descriptor shadow's
 * write half, libs/winecom/winecom.c) through its one hazardous shape:
 * app-synchronized descriptor creates from TWO threads, consumed by a
 * dependent device call.
 *
 * Two threads alternate strictly, handing a baton back and forth with
 * auto-reset events: thread A creates the constant-buffer view for global
 * index 0, 2, 4, ...; thread B for 1, 3, 5, ...; each waits for the other
 * between creates.  Every create g uses BufferLocation = base + 256*g, so
 * THE ISSUED ORDER IS READABLE OFF THE ADDRESSES.  When both threads are
 * done, the main thread calls CopyDescriptorsSimple -- a non-journaled
 * device method, so its trap must drain and replay every recorded create,
 * merged across both threads' rings by RDTSC stamp, BEFORE it is served.
 *
 * The probe itself only checks that the calls succeed (the API is void; the
 * replay's correctness is invisible from here).  The GATE reads the
 * +winecom trace: each thread's FIRST create traps (that is what arms its
 * ring), every later one must be recorded and replayed, and the replayed
 * BufferLocations must come out in exactly the issued order -- 2N-2 lines,
 * consecutive, ascending by 0x100, all before CopyDescriptorsSimple's own
 * dispatch line.
 *
 * No CRT: own entry (dj_entry), output via WriteFile.  Imports kernel32 and
 * d3d12 only (see check-dev-journal.sh).
 *
 * Copyright 2026 the ppc64le port authors
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#define COBJMACROS
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d12.h>

#define ROUNDS 32                 /* creates per thread; 2*ROUNDS total */

/* ---- no-CRT support ---- */

void *memset( void *d, int c, size_t n )
{
    unsigned char *p = d;
    while (n--) *p++ = (unsigned char)c;
    return d;
}

static void out_raw( const char *s )
{
    DWORD n, len = 0;
    while (s[len]) len++;
    WriteFile( GetStdHandle( STD_OUTPUT_HANDLE ), s, len, &n, NULL );
}

static void out_hex( const char *what, UINT64 v )
{
    char buf[64];
    int i, p = 0;
    while (what[p]) { buf[p] = what[p]; p++; }
    buf[p++] = ' '; buf[p++] = '0'; buf[p++] = 'x';
    for (i = 60; i >= 0; i -= 4) buf[p++] = "0123456789abcdef"[(v >> i) & 15];
    buf[p++] = '\n'; buf[p] = 0;
    out_raw( buf );
}

static void die( const char *why, HRESULT hr )
{
    out_raw( "dev_journal_probe: FAIL " );
    out_raw( why );
    out_hex( ", hr", (UINT)hr );
    ExitProcess( 1 );
}

/* ---- the probe ---- */

/* no uuid lib in a no-CRT build: the three IIDs, by value */
static const GUID dj_IID_ID3D12Device =
    { 0x189819f1, 0x1db6, 0x4b57, { 0xbe, 0x54, 0x18, 0x21, 0x33, 0x9b, 0x85, 0xf7 } };
static const GUID dj_IID_ID3D12Resource =
    { 0x696442be, 0xa72e, 0x4059, { 0xbc, 0x79, 0x5b, 0x5c, 0x98, 0x04, 0x0f, 0xad } };
static const GUID dj_IID_ID3D12DescriptorHeap =
    { 0x8efb471d, 0x616c, 0x4f49, { 0x90, 0xf7, 0x12, 0x7b, 0xb7, 0x63, 0xfa, 0x51 } };

static ID3D12Device *dev;
static D3D12_GPU_VIRTUAL_ADDRESS buf_va;
static D3D12_CPU_DESCRIPTOR_HANDLE src_start;
static UINT inc;
static HANDLE ev[2];              /* ev[0] wakes thread 0, ev[1] thread 1 */

static void create_one( UINT g )
{
    D3D12_CONSTANT_BUFFER_VIEW_DESC desc;
    D3D12_CPU_DESCRIPTOR_HANDLE h;

    desc.BufferLocation = buf_va + 256u * g;
    desc.SizeInBytes = 256;
    h.ptr = src_start.ptr + (SIZE_T)inc * g;
    ID3D12Device_CreateConstantBufferView( dev, &desc, h );
}

static DWORD WINAPI baton_thread( void *arg )
{
    UINT me = (UINT)(UINT_PTR)arg, k;

    for (k = 0; k < ROUNDS; k++)
    {
        if (WaitForSingleObject( ev[me], 30000 ) != WAIT_OBJECT_0)
        {
            out_raw( "dev_journal_probe: FAIL baton wait timed out\n" );
            ExitProcess( 1 );
        }
        create_one( 2 * k + me );
        SetEvent( ev[1 - me] );
    }
    return 0;
}

void dj_entry( void )
{
    D3D12_HEAP_PROPERTIES props;
    D3D12_RESOURCE_DESC rd;
    D3D12_DESCRIPTOR_HEAP_DESC hd;
    ID3D12Resource *buf;
    ID3D12DescriptorHeap *src_heap, *dst_heap;
    D3D12_CPU_DESCRIPTOR_HANDLE dst_start;
    HANDLE threads[2];
    HRESULT hr;

    /* one guest QPC call arms kernel32's qpc block -- the device journal
     * refuses to arm rings until the stamps have a native unit, exactly as
     * a real title's timer queries would have done long before D3D12 */
    {
        LARGE_INTEGER li;
        QueryPerformanceCounter( &li );
    }

    hr = D3D12CreateDevice( NULL, D3D_FEATURE_LEVEL_11_0,
                            &dj_IID_ID3D12Device, (void **)&dev );
    if (hr) die( "D3D12CreateDevice", hr );

    memset( &props, 0, sizeof(props) );
    props.Type = D3D12_HEAP_TYPE_UPLOAD;
    memset( &rd, 0, sizeof(rd) );
    rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    rd.Width = 256u * 2 * ROUNDS;
    rd.Height = 1;
    rd.DepthOrArraySize = 1;
    rd.MipLevels = 1;
    rd.SampleDesc.Count = 1;
    rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    hr = ID3D12Device_CreateCommittedResource( dev, &props, D3D12_HEAP_FLAG_NONE,
                                               &rd, D3D12_RESOURCE_STATE_GENERIC_READ,
                                               NULL, &dj_IID_ID3D12Resource, (void **)&buf );
    if (hr) die( "CreateCommittedResource", hr );
    buf_va = ID3D12Resource_GetGPUVirtualAddress( buf );
    if (!buf_va) die( "GetGPUVirtualAddress returned 0", 0 );

    memset( &hd, 0, sizeof(hd) );
    hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    hd.NumDescriptors = 2 * ROUNDS;
    hr = ID3D12Device_CreateDescriptorHeap( dev, &hd, &dj_IID_ID3D12DescriptorHeap,
                                            (void **)&src_heap );
    if (hr) die( "CreateDescriptorHeap src", hr );
    hr = ID3D12Device_CreateDescriptorHeap( dev, &hd, &dj_IID_ID3D12DescriptorHeap,
                                            (void **)&dst_heap );
    if (hr) die( "CreateDescriptorHeap dst", hr );
    /* aggregate return: the C header's plain macro cannot carry the hidden
     * return slot, so call through the vtable directly */
    src_heap->lpVtbl->GetCPUDescriptorHandleForHeapStart( src_heap, &src_start );
    dst_heap->lpVtbl->GetCPUDescriptorHandleForHeapStart( dst_heap, &dst_start );
    inc = ID3D12Device_GetDescriptorHandleIncrementSize( dev,
              D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV );

    out_hex( "dev_journal_probe: base", buf_va );

    ev[0] = CreateEventA( NULL, FALSE, FALSE, NULL );
    ev[1] = CreateEventA( NULL, FALSE, FALSE, NULL );
    if (!ev[0] || !ev[1]) die( "CreateEventA", 0 );
    threads[0] = CreateThread( NULL, 0, baton_thread, (void *)(UINT_PTR)0, 0, NULL );
    threads[1] = CreateThread( NULL, 0, baton_thread, (void *)(UINT_PTR)1, 0, NULL );
    if (!threads[0] || !threads[1]) die( "CreateThread", 0 );

    SetEvent( ev[0] );            /* hand thread 0 the baton */
    if (WaitForSingleObject( threads[0], 60000 ) != WAIT_OBJECT_0 ||
        WaitForSingleObject( threads[1], 60000 ) != WAIT_OBJECT_0 )
        die( "thread join timed out", 0 );

    /* the dependent call: a non-journaled device method whose dispatch must
     * replay every recorded create before it runs */
    ID3D12Device_CopyDescriptorsSimple( dev, 2 * ROUNDS, dst_start, src_start,
                                        D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV );

    out_raw( "dev_journal_probe: PASS\n" );
    ExitProcess( 0 );
}
