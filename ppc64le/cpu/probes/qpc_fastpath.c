/*
 * qpc_fastpath.c -- the guest half of ppc64le/cpu/check-qpc-fastpath.sh.
 *
 * Guest x86-64 code.  It asks four questions about the QueryPerformanceCounter
 * fast path (include/wine/emu_qpc.h), and the interesting one is the third.
 *
 *   MECHANISM   the kernel32 export really is the fast body, and the host
 *               really armed it -- so a pass cannot come from the fast path
 *               being quietly absent.
 *   FREQUENCY   QueryPerformanceFrequency answers 10 MHz, and a measured
 *               interval agrees between the fast path and the syscall.
 *   ORDER       readings taken alternately from the fast path and from
 *               ntdll.NtQueryPerformanceCounter -- which is a plain trap stub
 *               and therefore the native answer -- form ONE non-decreasing
 *               sequence.  This is the property that matters: two clocks that
 *               are each monotone but disagree about the epoch or the rate
 *               fail here on the first pair, and a game that mixes its own QPC
 *               with the QPC inside Wine's waits and timers is doing exactly
 *               this interleaving.
 *   THREADS     the same, across two threads on two processors trading
 *               readings through shared memory, because the POWER timebase's
 *               cross-core synchronisation is an assumption this port now
 *               depends on and an unchecked assumption is not a measurement.
 *
 * Every layer prints one line beginning PASS or FAIL and the process exits
 * with the number of failures.
 */

#include <windows.h>
#include <winternl.h>

#define ROUNDS_ORDER    20000
#define ROUNDS_THREAD   50000
#define ROUNDS_LOCAL    500000

static char obuf[1024];
static int failures;

static void emit( const char *s )
{
    DWORD w;
    WriteFile( GetStdHandle( STD_OUTPUT_HANDLE ), s, lstrlenA( s ), &w, NULL );
}

#define SAY(...) do { wsprintfA( obuf, __VA_ARGS__ ); emit( obuf ); } while (0)
#define PASS(...) do { emit( "PASS " ); SAY( __VA_ARGS__ ); emit( "\n" ); } while (0)
#define FAIL(...) do { emit( "FAIL " ); SAY( __VA_ARGS__ ); emit( "\n" ); failures++; } while (0)

/* The block kernel32 exports; see struct emu_qpc_guest. */
struct qpc_block
{
    ULONG64 magic;
    ULONG64 multiplier;
    ULONG64 bias;
    UCHAR   enabled;
    UCHAR   shift;
    UCHAR   pad[6];
    ULONG64 frequency;
    ULONG64 tsc_sample;
};

static LONGLONG fast( void )
{
    LARGE_INTEGER v;
    QueryPerformanceCounter( &v );
    return v.QuadPart;
}

static LONGLONG nativ( void )
{
    LARGE_INTEGER v;
    NtQueryPerformanceCounter( &v, NULL );
    return v.QuadPart;
}

/* ---- cross-thread ping-pong ------------------------------------------- */

static volatile LONG  pp_turn;
static volatile LONG  pp_bad;
static volatile LONG  pp_rounds;
static volatile LONGLONG pp_slot;

static DWORD_PTR far_mask = 0x2;

static DWORD WINAPI pingpong( LPVOID arg )
{
    LONG me = (LONG)(LONG_PTR)arg;
    LONG i;

    /* Two different processors, and deliberately not processors 0 and 1:
     * on this machine those are SMT siblings of ONE core, which would leave
     * the cross-CORE half of the claim untested.  far_mask is set below from
     * the processor count. */
    SetThreadAffinityMask( GetCurrentThread(), me ? far_mask : 0x1 );
    for (i = 0; i < ROUNDS_THREAD; i++)
    {
        LONGLONG seen, mine;
        while (pp_turn != me) { }
        seen = pp_slot;
        mine = me ? nativ() : fast();
        if (mine < seen) InterlockedIncrement( (LONG *)&pp_bad );
        pp_slot = mine;
        InterlockedIncrement( (LONG *)&pp_rounds );
        pp_turn = me ^ 1;
    }
    return 0;
}

static LONGLONG samples[2 * ROUNDS_ORDER];

int mainCRTStartup( void )
{
    struct qpc_block *blk;
    LARGE_INTEGER f;
    HMODULE k32;
    const BYTE *ep;
    LONGLONG a0, a1, b0, b1;
    LONG i, back, worst;

    /* The first call is the one that arms the block: a disarmed stub stores
     * its RDTSC reading and falls through to the trap, and the trap's own
     * dispatch is where the host names the emulator's TSC scale. */
    (void)fast();

    /* ---- MECHANISM ---------------------------------------------------- */
    k32 = GetModuleHandleA( "kernel32.dll" );
    if (!k32) { FAIL( "no kernel32 handle" ); goto out; }

    ep = (const BYTE *)GetProcAddress( k32, "QueryPerformanceCounter" );
    /* cmpb $1, <disp32>(%rip) -- the fast body's first instruction.  A trap
     * stub would start 49 89 ca (mov r10,rcx). */
    if (!ep || ep[0] != 0x80 || ep[1] != 0x3d || ep[6] != 0x01)
        FAIL( "kernel32!QueryPerformanceCounter is not the fast body (%02x %02x ... %02x)",
              ep ? ep[0] : 0, ep ? ep[1] : 0, ep ? ep[6] : 0 );
    else
        PASS( "kernel32!QueryPerformanceCounter is the fast body" );

    blk = (struct qpc_block *)GetProcAddress( k32, "__wine_thunk_qpc" );
    if (!blk) { FAIL( "kernel32 exports no __wine_thunk_qpc" ); goto out; }
    if (!blk->enabled)
        FAIL( "the QPC block is not armed (magic %I64x, sample %I64u)",
              blk->magic, blk->tsc_sample );
    else
        PASS( "armed: tsc scale %u, multiplier %I64u, frequency %I64u",
              blk->shift, blk->multiplier, blk->frequency );

    /* ---- FREQUENCY ---------------------------------------------------- */
    QueryPerformanceFrequency( &f );
    if (f.QuadPart != 10000000)
        FAIL( "QueryPerformanceFrequency is %I64d, not 10000000", f.QuadPart );
    else
        PASS( "QueryPerformanceFrequency is 10000000" );

    a0 = fast(); b0 = nativ();
    Sleep( 300 );
    a1 = fast(); b1 = nativ();
    {
        LONGLONG da = a1 - a0, db = b1 - b0, d = da > db ? da - db : db - da;
        /* Both intervals cover the same sleep plus a handful of microseconds
         * of call overhead.  A rate error shows up as a proportional gap; a
         * millisecond of slack is far more than the overhead and far less than
         * anything a wrong scale or multiplier could produce. */
        if (d > 10000)
            FAIL( "interval disagrees: fast %I64d vs native %I64d ticks", da, db );
        else
            PASS( "300 ms measured as %I64d ticks fast, %I64d native", da, db );
    }

    /* ---- ORDER -------------------------------------------------------- */
    for (i = 0; i < ROUNDS_ORDER; i++)
    {
        samples[2 * i]     = fast();
        samples[2 * i + 1] = nativ();
    }
    back = 0; worst = 0;
    for (i = 1; i < 2 * ROUNDS_ORDER; i++)
    {
        if (samples[i] < samples[i - 1])
        {
            LONGLONG step = samples[i - 1] - samples[i];
            back++;
            if (step > worst) worst = (LONG)(step > 0x7fffffff ? 0x7fffffff : step);
        }
    }
    if (back)
        FAIL( "%d of %d interleaved readings went backwards, worst %d ticks",
              back, 2 * ROUNDS_ORDER, worst );
    else
        PASS( "%d interleaved fast/native readings are one non-decreasing sequence",
              2 * ROUNDS_ORDER );

    /* ---- LOCAL -------------------------------------------------------- */
    {
        LONGLONG prev = 0;
        LONG bad = 0;
        for (i = 0; i < ROUNDS_LOCAL; i++)
        {
            LONGLONG t = fast();
            if (t < prev) bad++;
            prev = t;
        }
        if (bad) FAIL( "%d of %d consecutive fast readings went backwards", bad, ROUNDS_LOCAL );
        else PASS( "%d consecutive fast readings are non-decreasing", ROUNDS_LOCAL );
    }

    /* ---- THREADS ------------------------------------------------------ */
    {
        HANDLE h[2];
        SYSTEM_INFO si;
        DWORD id;

        /* POWER8 runs 4 or 8 threads to a core, so processor 8 is a different
         * core than 0 on any SMT setting this machine runs.  Fall back to 1 on
         * a machine that does not have that many. */
        GetSystemInfo( &si );
        far_mask = si.dwNumberOfProcessors > 8 ? 0x100 : 0x2;

        pp_slot = fast();
        pp_turn = 0;
        h[0] = CreateThread( NULL, 0, pingpong, (LPVOID)(LONG_PTR)0, 0, &id );
        h[1] = CreateThread( NULL, 0, pingpong, (LPVOID)(LONG_PTR)1, 0, &id );
        if (!h[0] || !h[1]) FAIL( "could not create the ping-pong threads" );
        else
        {
            WaitForSingleObject( h[0], 120000 );
            WaitForSingleObject( h[1], 120000 );
            if (pp_rounds != 2 * ROUNDS_THREAD)
                FAIL( "the ping-pong did not finish: %d of %d rounds",
                      (int)pp_rounds, 2 * ROUNDS_THREAD );
            else if (pp_bad)
                FAIL( "%d of %d cross-thread handoffs went backwards",
                      (int)pp_bad, (int)pp_rounds );
            else
                PASS( "%d cross-thread fast/native handoffs on two processors are ordered",
                      (int)pp_rounds );
        }
    }

out:
    SAY( "qpc_fastpath: %d failure(s)\n", failures );
    ExitProcess( failures );
    return failures;
}
