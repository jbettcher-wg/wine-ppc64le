/*
 * com_bench.c -- the price of one guest->native COM slot call, measured
 * from inside the guest.
 *
 * The flat-export sibling (crossing_bench.c) prices the trap itself.  A COM
 * vtable slot pays more on top of that trap, and the layers are the point:
 * the PE dispatcher's COM arm, winecom_dispatch (proxy lookup, journal
 * drain, argument marshal), and for the DXVK modules a SECOND transition --
 * the PE d3d11.dll reaches d3d11.so through __wine_unix_call_dispatcher
 * before DXVK runs.  [MEASURED] op4k 2026-09-04, Witcher 3 render thread:
 * those layers were ~20% of the thread with no lock left in them.  This
 * bench exists so a change to any of them gets an A/B in seconds.
 *
 * The measured slots are chosen the way GetCurrentProcessId was:
 *   - ID3D11Device::GetFeatureLevel  -- no arguments, DXVK returns a field;
 *   - ID3D11DeviceContext::GetType   -- same shape on the context proxy.
 * Both are plain marshalled slots (no hand walker, no FP, no out-params),
 * with stable answers, so the loop measures the path and not the API.
 * The device is created once, outside the timed region; a failed create
 * exits 2 so a GPU-less box reads as "could not run", not as a number.
 *
 * The clock is the guest-side QPC fast path (see crossing_bench.c).  No
 * CRT; kernel32 + d3d11 imports only.  Output lines are parse-stable:
 *
 *     BENCH qpc_only_ns_per_call=<f> (N=<n>)
 *     BENCH com_getfeaturelevel_ns_per_call=<f> (N=<n>)
 *     BENCH com_gettype_ns_per_call=<f> (N=<n>)
 *     BENCH com_journaled_topology_ns_per_call=<f> (N=<n>)   a journaled slot
 */

#define COBJMACROS
#include <windows.h>
#include <d3d11.h>

typedef unsigned long long u64;

static void put_str( const char *s )
{
    DWORD len = 0, w;
    while (s[len]) len++;
    WriteFile( GetStdHandle( STD_OUTPUT_HANDLE ), s, len, &w, 0 );
}

static void put_tenths( u64 tenths )
{
    char buf[32];
    int i = 32;
    u64 whole = tenths / 10;
    buf[--i] = 0;
    buf[--i] = '0' + (char)(tenths % 10);
    buf[--i] = '.';
    if (!whole) buf[--i] = '0';
    while (whole) { buf[--i] = '0' + (char)(whole % 10); whole /= 10; }
    put_str( buf + i );
}

static void put_u64( u64 v )
{
    char buf[24];
    int i = 24;
    buf[--i] = 0;
    if (!v) buf[--i] = '0';
    while (v) { buf[--i] = '0' + (char)(v % 10); v /= 10; }
    put_str( buf + i );
}

static void report( const char *name, u64 t0, u64 t1, u64 freq, u64 n )
{
    u64 ns_tenths = ((t1 - t0) * 10000000000ull / freq) / n;
    put_str( "BENCH " );
    put_str( name );
    put_str( "_ns_per_call=" );
    put_tenths( ns_tenths );
    put_str( " (N=" );
    put_u64( n );
    put_str( ")\n" );
}

void com_bench_entry( void )
{
    ID3D11Device *device = NULL;
    ID3D11DeviceContext *context = NULL;
    D3D_FEATURE_LEVEL got_fl = 0;
    const D3D_FEATURE_LEVEL want_fl[] = { D3D_FEATURE_LEVEL_11_0 };
    LARGE_INTEGER freq, t0, t1;
    u64 i, sink = 0;
    const u64 WARM = 10000, N_QPC = 2000000, N_COM = 200000;
    HRESULT hr;

    if (!QueryPerformanceFrequency( &freq ) || !freq.QuadPart) ExitProcess( 2 );

    hr = D3D11CreateDevice( NULL, D3D_DRIVER_TYPE_HARDWARE, NULL, 0, want_fl, 1,
                            D3D11_SDK_VERSION, &device, &got_fl, &context );
    if (FAILED(hr) || !device || !context)
    {
        put_str( "com_bench: D3D11CreateDevice failed\n" );
        ExitProcess( 2 );
    }

    for (i = 0; i < WARM; i++) QueryPerformanceCounter( &t0 );
    QueryPerformanceCounter( &t0 );
    for (i = 0; i < N_QPC; i++) QueryPerformanceCounter( &t1 );
    QueryPerformanceCounter( &t1 );
    report( "qpc_only", t0.QuadPart, t1.QuadPart, freq.QuadPart, N_QPC );

    for (i = 0; i < WARM; i++) sink += ID3D11Device_GetFeatureLevel( device );
    QueryPerformanceCounter( &t0 );
    for (i = 0; i < N_COM; i++) sink += ID3D11Device_GetFeatureLevel( device );
    QueryPerformanceCounter( &t1 );
    report( "com_getfeaturelevel", t0.QuadPart, t1.QuadPart, freq.QuadPart, N_COM );

    for (i = 0; i < WARM; i++) sink += ID3D11DeviceContext_GetType( context );
    QueryPerformanceCounter( &t0 );
    for (i = 0; i < N_COM; i++) sink += ID3D11DeviceContext_GetType( context );
    QueryPerformanceCounter( &t1 );
    report( "com_gettype", t0.QuadPart, t1.QuadPart, freq.QuadPart, N_COM );

    /* A JOURNALED slot (journal_gen.h): the call is recorded guest-side
     * and replayed at the next trap, so this loop pays the record per call
     * plus a share of one replay drain whenever the ring fills.  Under
     * WINEEMUNOCOMJOURNAL=1 it is an ordinary trapped COM slot, which is
     * the A/B.  The final IAGetPrimitiveTopology drains what is left so the
     * measured region includes every replay it caused. */
    for (i = 0; i < WARM; i++)
        ID3D11DeviceContext_IASetPrimitiveTopology( context, D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST );
    QueryPerformanceCounter( &t0 );
    for (i = 0; i < N_COM; i++)
        ID3D11DeviceContext_IASetPrimitiveTopology( context, (D3D11_PRIMITIVE_TOPOLOGY)(1 + (i & 3)) );
    {
        D3D11_PRIMITIVE_TOPOLOGY topo = 0;
        ID3D11DeviceContext_IAGetPrimitiveTopology( context, &topo );
        sink += topo;
    }
    QueryPerformanceCounter( &t1 );
    report( "com_journaled_topology", t0.QuadPart, t1.QuadPart, freq.QuadPart, N_COM );

    ID3D11DeviceContext_Release( context );
    ID3D11Device_Release( device );
    ExitProcess( sink == 0xdeadbeef ? 1 : 0 );
}
