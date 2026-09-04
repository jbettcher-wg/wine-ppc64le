/*
 * server_bench.c -- the price of one wineserver round trip, measured from
 * inside the guest.
 *
 * Times N pairs of CreateEventW + CloseHandle: two flat crossings whose
 * native bodies are each one server request (create_event, close_handle),
 * so the number is dominated by the server round trip -- the socket write,
 * wineserver's turn, the reply -- with the crossing itself known from
 * crossing_bench.c and subtractable.  Same freestanding shape and the same
 * guest-side QPC clock as crossing_bench.c; ppc64le/cpu/bench-server.sh
 * builds and runs it.
 *
 *     BENCH server_pair_ns=<f> (N=<n>)      one CreateEventW + CloseHandle
 */
typedef unsigned long long u64;
typedef unsigned int u32;
typedef unsigned short u16;

int __stdcall QueryPerformanceCounter( u64 *out );
int __stdcall QueryPerformanceFrequency( u64 *out );
void *__stdcall CreateEventW( void *sa, int manual, int initial, const u16 *name );
int __stdcall CloseHandle( void *h );
void *__stdcall GetStdHandle( u32 which );
int __stdcall WriteFile( void *h, const void *buf, u32 len, u32 *written, void *ov );
void __stdcall ExitProcess( u32 code );

#define STD_OUTPUT_HANDLE ((u32)-11)

static void put_str( const char *s )
{
    u32 len = 0, w;
    while (s[len]) len++;
    WriteFile( GetStdHandle( STD_OUTPUT_HANDLE ), s, len, &w, 0 );
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

void mainCRTStartup( void )
{
    u64 freq, t0, t1, i;
    const u64 WARM = 2000, N = 100000;
    u32 fails = 0;

    if (!QueryPerformanceFrequency( &freq ) || !freq) ExitProcess( 2 );
    for (i = 0; i < WARM; i++) { void *h = CreateEventW( 0, 0, 0, 0 ); if (!h || !CloseHandle( h )) fails++; }
    QueryPerformanceCounter( &t0 );
    for (i = 0; i < N; i++) { void *h = CreateEventW( 0, 0, 0, 0 ); if (!h || !CloseHandle( h )) fails++; }
    QueryPerformanceCounter( &t1 );
    put_str( "BENCH server_pair_ns=" );
    put_tenths( ((t1 - t0) * 10000000000ull / freq) / N );
    put_str( " (N=" ); put_u64( N ); put_str( ")\n" );
    ExitProcess( fails ? 1 : 0 );
}
