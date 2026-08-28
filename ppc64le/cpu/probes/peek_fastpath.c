/*
 * peek_fastpath.c -- the guest half of ppc64le/cpu/check-peek-fastpath.sh.
 *
 * Guest x86-64 code.  The PeekMessageW fast body (tools/spec2thunk, kind
 * 'peek') answers the empty null-filter poll from the thread's queue_shm
 * without crossing; the danger is not a crash but a SWALLOWED or DELAYED
 * message -- a poll that answers "empty" out of guest user space while the
 * server's wake bits say otherwise.  The layers:
 *
 *   MECHANISM   user32's PeekMessageW export begins with the fast body's
 *               movq %gs:0x198 (65 4C 8B), not the trap stub's mov r10,rcx
 *               (49 89 CA) -- a pass cannot come from the fast path being
 *               quietly absent.
 *   SEEDING     after one real peek, TEB SystemReserved1[1] (gs:0x198)
 *               carries the queue object's address; the fast path cannot
 *               have answered anything before that.
 *   LIVENESS    the per-thread trap budget (gs:0x1a4) moves across a burst
 *               of empty polls -- the fast body, not the trap, is what ran.
 *   FASTNESS    500,000 empty polls timed by QPC average far below any
 *               trap's cost.  With the fast path off this layer reads tens
 *               of microseconds; the bound is 2 us with 100x headroom both
 *               ways.
 *   DELIVERY    a message posted to this thread arrives on the very next
 *               poll: the wake bits force the fast body down its trap leg,
 *               and an "empty" answer here is exactly the bug this gate
 *               exists to catch.
 *
 * Mode `starve` (argv contains the word) runs seeding + post + a bounded
 * poll loop and reports GOT or STARVED without judging: under
 * WINE_PPC64LE_PEEK_SABOTAGE=1 the fast body ignores the bits, the message
 * must starve (the gate's negative control), and under the kill switch on
 * top of the sabotage it must flow again.
 *
 * Every layer prints one line beginning PASS or FAIL and the process exits
 * with the number of failures.
 */

#include <windows.h>
#include <winternl.h>

#define ROUNDS_TIMED 500000

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

static ULONG_PTR read_gs_ptr( ULONG off )
{
    ULONG_PTR v;
    __asm__ volatile( "movq %%gs:(%1), %0" : "=r"(v) : "r"((ULONG_PTR)off) );
    return v;
}

static ULONG read_gs_dword( ULONG off )
{
    ULONG v;
    __asm__ volatile( "movl %%gs:(%1), %0" : "=r"(v) : "r"((ULONG_PTR)off) );
    return v;
}

/* post WM_APP to this thread, then poll for it; -1 = never arrived */
static int polls_until_delivery( DWORD deadline_ms )
{
    DWORD t0 = GetTickCount();
    MSG msg;
    int polls = 0;

    if (!PostThreadMessageW( GetCurrentThreadId(), WM_APP, 7, 9 ))
    {
        FAIL( "PostThreadMessageW refused" );
        return -1;
    }
    for (;;)
    {
        polls++;
        if (PeekMessageW( &msg, 0, 0, 0, PM_REMOVE ) && msg.message == WM_APP)
        {
            if (msg.wParam != 7 || msg.lParam != 9)
                FAIL( "WM_APP arrived torn: wParam %d lParam %d",
                      (int)msg.wParam, (int)msg.lParam );
            return polls;
        }
        if (GetTickCount() - t0 > deadline_ms) return -1;
    }
}

void mainCRTStartup( void )
{
    const unsigned char *entry;
    LARGE_INTEGER t0, t1;
    ULONG_PTR seeded;
    ULONG budget0, budget1;
    MSG msg;
    int i, got, moved;

    /* mode `starve`: seeding + post + bounded poll, reported not judged --
     * the gate reads GOT/STARVED against the lever it armed. */
    {
        const WCHAR *cl = NtCurrentTeb()->Peb->ProcessParameters->CommandLine.Buffer;
        for (; cl && *cl; cl++)
            if (cl[0] == 's' && cl[1] == 't' && cl[2] == 'a' && cl[3] == 'r')
            {
                PeekMessageW( &msg, 0, 0, 0, PM_REMOVE );   /* the seeding peek */
                got = polls_until_delivery( 3000 );
                if (got < 0) emit( "STARVED\n" );
                else SAY( "GOT after %d poll(s)\n", got );
                emit( "DONE starve\n" );
                ExitProcess( 0 );
            }
    }

    /* MECHANISM */
    entry = (const unsigned char *)GetProcAddress( GetModuleHandleA( "user32.dll" ),
                                                   "PeekMessageW" );
    if (!entry) { FAIL( "mechanism: no PeekMessageW export" ); ExitProcess( failures ); }
    if (entry[0] == 0x65 && entry[1] == 0x4c && entry[2] == 0x8b)
        PASS( "mechanism: PeekMessageW begins with the fast body's gs load" );
    else if (entry[0] == 0x49 && entry[1] == 0x89 && entry[2] == 0xca)
    {
        FAIL( "mechanism: PeekMessageW is a bare trap stub; no fast body was emitted" );
        ExitProcess( failures );
    }
    else
    {
        FAIL( "mechanism: PeekMessageW begins %02x %02x %02x -- neither fast body nor stub",
              entry[0], entry[1], entry[2] );
        ExitProcess( failures );
    }

    /* SEEDING: one real peek makes win32u look the queue up and plant it */
    PeekMessageW( &msg, 0, 0, 0, PM_REMOVE );
    seeded = read_gs_ptr( 0x198 );
    if (seeded && !(seeded & 1))
        PASS( "seeding: SystemReserved1[1] carries the queue object (%p)", (void *)seeded );
    else if (!seeded)
        FAIL( "seeding: SystemReserved1[1] is still 0 after a real peek" );
    else
        FAIL( "seeding: SystemReserved1[1] carries the sabotage tag (%p) without the lever",
              (void *)seeded );

    /* LIVENESS: the budget cell only moves when the fast body runs */
    budget0 = read_gs_dword( 0x1a4 );
    moved = 0;
    for (i = 0; i < 600; i++)
    {
        PeekMessageW( &msg, 0, 0, 0, PM_REMOVE );
        budget1 = read_gs_dword( 0x1a4 );
        if (budget1 != budget0) { moved = 1; break; }
    }
    if (moved) PASS( "liveness: the trap budget moved (0x%x -> 0x%x) -- the fast body runs", budget0, budget1 );
    else FAIL( "liveness: 600 empty polls never touched the trap budget; the fast body is not executing" );

    /* FASTNESS */
    QueryPerformanceCounter( &t0 );
    for (i = 0; i < ROUNDS_TIMED; i++)
        PeekMessageW( &msg, 0, 0, 0, PM_REMOVE );
    QueryPerformanceCounter( &t1 );
    {
        /* QPC is 10 MHz; ticks * 100 = ns.  Average must sit far below any
         * trap's cost -- with the fast path off this reads microseconds. */
        ULONG64 ns = (ULONG64)(t1.QuadPart - t0.QuadPart) * 100 / ROUNDS_TIMED;
        if (ns < 2000)
            PASS( "fastness: %d empty polls averaged %d ns", ROUNDS_TIMED, (int)ns );
        else
            FAIL( "fastness: %d empty polls averaged %d ns -- trap-sized, the fast path is not answering",
                  ROUNDS_TIMED, (int)ns );
    }

    /* DELIVERY: the wake bits must force the very next poll down the trap */
    got = polls_until_delivery( 3000 );
    if (got < 0)
        FAIL( "delivery: WM_APP never arrived -- the fast path is answering over live bits" );
    else if (got <= 2)
        PASS( "delivery: WM_APP arrived on poll %d", got );
    else
        FAIL( "delivery: WM_APP took %d polls -- the bits are not forcing the trap", got );

    ExitProcess( failures );
}
