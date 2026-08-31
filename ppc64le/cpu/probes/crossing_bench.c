/*
 * crossing_bench.c -- the price of one guest->native crossing, measured from
 * inside the guest.
 *
 * A guest x86-64 program that times N calls of GetCurrentProcessId().  That
 * export is chosen with care and the choice is load-bearing:
 *
 *   - it is a plain trap thunk: NOT in spec2thunk's FAST_PATH_EXPORTS (qpc,
 *     qpf, tid, the critical-section pair, peek), so every call really
 *     crosses -- there is no guest-side fast body to quietly measure instead;
 *   - its native body is trivial (read the PEB/process id), so the number is
 *     the CROSSING, not the API;
 *   - it takes no arguments and its result is stable, so the loop cannot be
 *     served from anything but the call.
 *
 * The clock is the guest-side QueryPerformanceCounter fast path -- rdtsc plus
 * one multiply, no trap, proven monotone against the native clock by algebra
 * (include/wine/emu_qpc.h) -- so the measurement itself adds no crossings.
 * If QPC were NOT served guest-side this bench would be measuring itself;
 * the sanity check below (a QPC-only loop) reports the clock's own cost so a
 * reader can see it is noise (sub-percent) against the crossing.
 *
 * This is a MEASUREMENT TOOL, not a gate: there is no pass/fail, only
 * numbers on stdout.  ppc64le/cpu/bench-crossing.sh builds and runs it.
 * Output lines are parse-stable:
 *
 *     BENCH qpc_only_ns_per_call=<f> (N=<n>)
 *     BENCH crossing_ns_per_call=<f> (N=<n>)
 *
 * No CRT: freestanding, kernel32-only imports, hand-rolled number printing
 * (the same discipline as the sibling probes' guest halves).
 */

typedef unsigned long long u64;
typedef unsigned int u32;

int __stdcall QueryPerformanceCounter( u64 *count );
int __stdcall QueryPerformanceFrequency( u64 *freq );
u32 __stdcall GetCurrentProcessId( void );
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

/* fixed-point one-decimal print: 12345 tenths -> "1234.5" */
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
    /* ns/call in tenths, order kept so nothing overflows: the elapsed tick
     * count fits 32 bits at any plausible frequency and N */
    u64 ns_tenths = ((t1 - t0) * 10000000000ull / freq) / n;
    put_str( "BENCH " );
    put_str( name );
    put_str( "_ns_per_call=" );
    put_tenths( ns_tenths );
    put_str( " (N=" );
    put_u64( n );
    put_str( ")\n" );
}

void mainCRTStartup( void )
{
    u64 freq, t0, t1;
    u32 sink = 0;
    u64 i;
    const u64 WARM = 10000, N_QPC = 2000000, N_CROSS = 200000;

    if (!QueryPerformanceFrequency( &freq ) || !freq) ExitProcess( 2 );

    /* clock-cost sanity leg: a QPC-only loop.  Also the warmup for the
     * QPC seeding itself. */
    for (i = 0; i < WARM; i++) QueryPerformanceCounter( &t0 );
    QueryPerformanceCounter( &t0 );
    for (i = 0; i < N_QPC; i++) QueryPerformanceCounter( &t1 );
    QueryPerformanceCounter( &t1 );
    report( "qpc_only", t0, t1, freq, N_QPC );

    /* the crossing: warm the thunk page and the JIT block first */
    for (i = 0; i < WARM; i++) sink += GetCurrentProcessId();
    QueryPerformanceCounter( &t0 );
    for (i = 0; i < N_CROSS; i++) sink += GetCurrentProcessId();
    QueryPerformanceCounter( &t1 );
    report( "crossing", t0, t1, freq, N_CROSS );

    /* keep `sink` observable so no future optimizer deletes the loops */
    ExitProcess( sink ? 0 : 0 );
}
