/*
 * cs_fastpath.c -- the guest half of ppc64le/cpu/check-cs-fastpath.sh.
 *
 * Guest x86-64 code.  The critical-section fast bodies (spec2thunk kinds
 * 'ecs'/'lcs') implement Wine's own TryEnter/Leave algorithm in guest user
 * space; a wrong fast body does not crash -- it corrupts a lock word that
 * NATIVE RtlEnterCriticalSection also operates on, and the symptom is a
 * deadlock or a data race some minutes later.  So the layers check the exact
 * struct STATE Wine's algorithm defines, not merely "it did not hang":
 *
 *   MECHANISM   kernel32's EnterCriticalSection export begins with the fast
 *               body's cmpb (80 3D), not the trap stub's mov r10,rcx
 *               (49 89 CA) -- a pass cannot come from the fast path being
 *               quietly absent.
 *   FIELDS      after Enter: LockCount 0, RecursionCount 1, OwningThread ==
 *               tid.  After a recursive Enter: 1, 2, tid.  Unwound: back to
 *               -1, 0, 0.  These are the values Wine's native algorithm
 *               produces; matching them IS the interop claim, field by field.
 *   MIXED       the same section entered alternately through kernel32 (fast)
 *               and ntdll.RtlEnterCriticalSection (a trap, so the NATIVE
 *               implementation), fields checked each time -- two
 *               implementations, one lock word, one answer.
 *   CONTENTION  two threads, one section, a deliberately non-atomic shared
 *               counter: 2 x ROUNDS increments survive only if exclusion
 *               held; the contended paths (native wait, the fast leave's
 *               undo-and-trap) are exactly what this exercises.
 *
 * Mode `recurse` (argv[1]) runs ONLY init + Enter + recursive Enter + Leaves
 * and prints DONE: under WINE_PPC64LE_CS_SABOTAGE_OWNER=1 the first Enter
 * records no owner, the recursive Enter misreads the section as foreign and
 * waits on a lock its own thread holds, and the gate's timeout turns that
 * hang into the red its negative control requires.
 *
 * Every layer prints one line beginning PASS or FAIL and the process exits
 * with the number of failures.
 */

#include <windows.h>
#include <winternl.h>

#define ROUNDS_CONTEND 200000

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

typedef NTSTATUS (WINAPI *rtl_cs_fn)( RTL_CRITICAL_SECTION * );

static CRITICAL_SECTION cs;
static volatile LONG naked;      /* increments are deliberately not atomic */

static int check_fields( const char *when, LONG lock, LONG rec, ULONG_PTR owner )
{
    RTL_CRITICAL_SECTION *c = (RTL_CRITICAL_SECTION *)&cs;

    if (c->LockCount == lock && c->RecursionCount == rec &&
        (ULONG_PTR)c->OwningThread == owner)
    {
        PASS( "%s: LockCount %d RecursionCount %d owner %p", when,
              (int)lock, (int)rec, (void *)owner );
        return 1;
    }
    FAIL( "%s: LockCount %d (want %d) RecursionCount %d (want %d) owner %p (want %p)",
          when, (int)c->LockCount, (int)lock, (int)c->RecursionCount, (int)rec,
          c->OwningThread, (void *)owner );
    return 0;
}

static DWORD WINAPI contender( void *arg )
{
    int i;
    for (i = 0; i < ROUNDS_CONTEND; i++)
    {
        EnterCriticalSection( &cs );
        naked = naked + 1;       /* torn unless the lock excludes */
        LeaveCriticalSection( &cs );
    }
    return 0;
}

void mainCRTStartup( void )
{
    const unsigned char *entry;
    ULONG_PTR tid;
    rtl_cs_fn rtl_enter, rtl_leave;
    HMODULE ntdll;
    HANDLE t1, t2;
    int i;

    /* mode `recurse`: the sabotage target, nothing else.  A hang here is the
     * negative control firing; the gate owns the timeout. */
    {
        const WCHAR *cl = NtCurrentTeb()->Peb->ProcessParameters->CommandLine.Buffer;
        for (; cl && *cl; cl++)
            if (cl[0] == 'r' && cl[1] == 'e' && cl[2] == 'c' && cl[3] == 'u')
            {
                InitializeCriticalSection( &cs );
                EnterCriticalSection( &cs );
                EnterCriticalSection( &cs );   /* recursion: needs the owner the
                                                  sabotaged fast body did not write */
                LeaveCriticalSection( &cs );
                LeaveCriticalSection( &cs );
                emit( "DONE recurse\n" );
                ExitProcess( 0 );
            }
    }

    tid = (ULONG_PTR)(ULONG)GetCurrentThreadId();

    /* MECHANISM */
    entry = (const unsigned char *)GetProcAddress( GetModuleHandleA( "kernel32.dll" ),
                                                   "EnterCriticalSection" );
    if (!entry) { FAIL( "mechanism: no EnterCriticalSection export" ); ExitProcess( failures ); }
    if (entry[0] == 0x80 && entry[1] == 0x3d)
        PASS( "mechanism: EnterCriticalSection begins with the fast body's cmpb" );
    else if (entry[0] == 0x49 && entry[1] == 0x89 && entry[2] == 0xca)
        FAIL( "mechanism: EnterCriticalSection is a bare trap stub; no fast body was emitted" );
    else
        FAIL( "mechanism: EnterCriticalSection begins %02x %02x %02x, neither fast body nor stub",
              entry[0], entry[1], entry[2] );

    /* FIELDS, single thread */
    InitializeCriticalSection( &cs );
    check_fields( "fields: initialized", -1, 0, 0 );
    EnterCriticalSection( &cs );
    check_fields( "fields: entered", 0, 1, tid );
    EnterCriticalSection( &cs );
    check_fields( "fields: recursed", 1, 2, tid );
    LeaveCriticalSection( &cs );
    check_fields( "fields: unrecursed", 0, 1, tid );
    LeaveCriticalSection( &cs );
    check_fields( "fields: left", -1, 0, 0 );

    /* MIXED: the native implementation against the fast one, same lock word */
    ntdll = GetModuleHandleA( "ntdll.dll" );
    rtl_enter = (rtl_cs_fn)GetProcAddress( ntdll, "RtlEnterCriticalSection" );
    rtl_leave = (rtl_cs_fn)GetProcAddress( ntdll, "RtlLeaveCriticalSection" );
    if (!rtl_enter || !rtl_leave)
        FAIL( "mixed: ntdll exports missing (%p %p)", rtl_enter, rtl_leave );
    else
    {
        rtl_enter( (RTL_CRITICAL_SECTION *)&cs );        /* native enter */
        check_fields( "mixed: native enter", 0, 1, tid );
        EnterCriticalSection( &cs );                     /* fast recursion on it */
        check_fields( "mixed: fast recursion on a native enter", 1, 2, tid );
        rtl_leave( (RTL_CRITICAL_SECTION *)&cs );        /* native unrecurse */
        check_fields( "mixed: native unrecurse", 0, 1, tid );
        LeaveCriticalSection( &cs );                     /* fast final leave */
        check_fields( "mixed: fast leave of a native enter", -1, 0, 0 );
    }

    /* CONTENTION */
    naked = 0;
    t1 = CreateThread( NULL, 0, contender, NULL, 0, NULL );
    t2 = CreateThread( NULL, 0, contender, NULL, 0, NULL );
    for (i = 0; i < ROUNDS_CONTEND; i++)
    {
        EnterCriticalSection( &cs );
        naked = naked + 1;
        LeaveCriticalSection( &cs );
    }
    WaitForSingleObject( t1, INFINITE );
    WaitForSingleObject( t2, INFINITE );
    if (naked == 3 * ROUNDS_CONTEND)
        PASS( "contention: 3x%d increments of a non-atomic counter survived "
              "three threads", ROUNDS_CONTEND );
    else
        FAIL( "contention: counter %d, want %d -- the lock did not exclude",
              (int)naked, 3 * ROUNDS_CONTEND );
    check_fields( "contention: after the storm", -1, 0, 0 );

    ExitProcess( failures );
}
