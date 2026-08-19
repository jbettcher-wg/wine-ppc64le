/*
 * guest_fibers -- the gate for FIBERS in a guest process.
 *
 * A fiber is a stack with a program counter parked on it and switched by
 * hand.  kernelbase implements them the same way everywhere -- one CONTEXT
 * per fiber, one register-saving switch -- and until this work the ppc64
 * branch of that switch did not exist: SwitchToFiber reached a
 * FIXME("not implemented") and a DbgBreakPoint.  DOOM (2016) is the title
 * that needs it, because id Tech 6's job system is built on fibers, and it
 * died there every run.
 *
 * On this host a fiber that runs guest code has TWO stacks:
 *
 *   - a NATIVE ppc64 one that kernelbase allocated for it, which is what
 *     switch_fiber moves between, and
 *   - a GUEST x86-64 one the emulator run allocated when the fiber's start
 *     routine was first entered.
 *
 * The bookkeeping tying them together (which guest stack this run is on,
 * which native stack it returns to, how deep the nesting goes) is per-RUN
 * state that the port keeps in thread-locals.  That is correct until one
 * thread has two runs that are NOT nested, which is exactly what a fiber is:
 * fiber A parks a live run, fiber B runs one of its own, and A is resumed out
 * of order.  So this probe's central assertion is not "did the switch
 * happen" but WHICH STACK EACH FIBER IS TOLD IT IS ON, checked from the
 * guest's own TEB after every switch -- gs:[0x08] is StackBase and gs:[0x10]
 * StackLimit, and a local variable of the running fiber has to lie between
 * them.  That is the check the port's negative control (WINEEMUNOFIBERSTATE)
 * breaks, and nothing else here would notice.
 *
 * The transcript is deterministic and the runner diffs it byte for byte.
 * Every number printed is either a constant this file controls or a relation
 * (in/out of bounds, equal/not equal), never an address.
 *
 * NO CRT: the entry point IS the image entry point, as in guest_callbacks.c
 * and for the same reason -- so the gate knows exactly which module each
 * import came from, and the fiber calls are not buried under a CRT's own.
 *
 * Copyright 2026 the ppc64le port authors
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 */

#include <windows.h>

#define GF_NOINLINE __attribute__((noinline))

/* ------------------------------------------------------------- output */

static void out( const char *s )
{
    DWORD n = 0, written;

    while (s[n]) n++;
    WriteFile( GetStdHandle( STD_OUTPUT_HANDLE ), s, n, &written, NULL );
}

static void out_dec( ULONG v )
{
    char buf[12];
    int i = 11;

    buf[i] = 0;
    do { buf[--i] = '0' + (char)(v % 10); v /= 10; } while (v);
    out( buf + i );
}

static int failures;

static void check( const char *what, BOOL ok )
{
    out( "guest_fibers: " );
    out( what );
    out( ok ? " ok\n" : " FAIL\n" );
    if (!ok) failures++;
}

/* ------------------------------------------------------- the guest's TEB
 *
 * Read directly rather than through NtCurrentTeb() so that this probe needs
 * no ntdll import at all: the two fields it wants are at fixed offsets in
 * the NT_TIB every x86-64 TEB begins with. */
static ULONGLONG teb_qword( ULONG off )
{
    ULONGLONG v;

    __asm__ __volatile__( "movq %%gs:(%1), %0" : "=r"(v) : "r"((ULONGLONG)off) );
    return v;
}

static ULONGLONG teb_stack_base(void)  { return teb_qword( 0x08 ); }
static ULONGLONG teb_stack_limit(void) { return teb_qword( 0x10 ); }

/* GetCurrentFiber() and GetFiberData() are MACROS: they read Tib.FiberData at
 * gs:[0x20] and dereference it, with no call for the port to intercept.  A
 * guest gets its own fiber handle from there or not at all, which makes this
 * the one part of the fiber API that has to be right in the TEB itself.
 * DOOM (2016) is what proved it matters -- it switched to whatever that field
 * held. */
static void *current_fiber(void)   { return (void *)(ULONG_PTR)teb_qword( 0x20 ); }
static void *current_fiber_data(void)
{
    void **fiber = current_fiber();

    return fiber ? *fiber : NULL;
}

static GF_NOINLINE BOOL on_my_own_stack( void )
{
    volatile char here;
    ULONGLONG addr = (ULONGLONG)(ULONG_PTR)&here;

    return addr > teb_stack_limit() && addr < teb_stack_base();
}

/* ------------------------------------------------------------ the trace
 *
 * Who ran, in order.  A set of counters cannot tell "main, A, main, B" from
 * "main, B, main, A", and the ORDER is the whole contract of a fiber switch.
 */
static char trace[64];
static int  trace_len;

static void mark( char c )
{
    if (trace_len < (int)sizeof(trace) - 1) trace[trace_len++] = c;
}

static BOOL trace_is( const char *want )
{
    int i;

    trace[trace_len] = 0;
    for (i = 0; i <= trace_len; i++) if (trace[i] != want[i]) return FALSE;
    return TRUE;
}

/* ------------------------------------------------------------- the fibers */

static void *main_fiber, *fiber_a, *fiber_b;
static int   a_turns, b_turns;
static BOOL  a_stack_ok = TRUE, b_stack_ok = TRUE, a_locals_kept = TRUE;
static ULONGLONG a_base, b_base;

/* A frame deep enough to prove the fiber is running on a REAL stack of its
 * own and not borrowing somebody's red zone: 4 KiB of touched locals, and a
 * value in them that must survive a switch away and back. */
static GF_NOINLINE ULONG deep_frame( ULONG seed, int depth )
{
    volatile ULONG pad[512];
    ULONG i;

    for (i = 0; i < 512; i++) pad[i] = seed + i;
    if (depth) return deep_frame( seed, depth - 1 ) + (pad[511] - pad[0]);
    return pad[0];
}

static void WINAPI fiber_a_proc( void *param )
{
    ULONG local = 0xa11ce;

    a_base = teb_stack_base();
    for (;;)
    {
        mark( 'A' );
        a_turns++;
        if (!on_my_own_stack()) a_stack_ok = FALSE;
        /* the same question from inside a fiber: a guest that switches by
         * GetCurrentFiber() has to see ITSELF here, not the last fiber */
        if (current_fiber() != fiber_a || current_fiber_data() != (void *)0x0a)
            a_stack_ok = FALSE;
        if (local != 0xa11ce) a_locals_kept = FALSE;
        if (param != (void *)0x0a) a_locals_kept = FALSE;
        /* the deep frame is exercised on the second turn only, so the first
         * turn proves a bare switch works before anything stresses it */
        /* deep_frame(s, d) = s + d*511 by construction; the point is not the
         * number but that 4 KiB of touched locals per frame, four frames
         * deep, are on THIS fiber's stack and come back intact. */
        if (a_turns == 2 && deep_frame( a_turns, 3 ) != a_turns + 3 * 511)
            a_locals_kept = FALSE;
        SwitchToFiber( main_fiber );
    }
}

static void WINAPI fiber_b_proc( void *param )
{
    for (;;)
    {
        mark( 'B' );
        b_turns++;
        b_base = teb_stack_base();
        if (!on_my_own_stack()) b_stack_ok = FALSE;
        if (param != (void *)0x0b) b_stack_ok = FALSE;
        /* B always switches to A rather than back to main: the resume that
         * follows is the OUT-OF-ORDER one, A picking up a run it parked
         * before B ever started, which is the case a thread-local block
         * cannot survive. */
        SwitchToFiber( fiber_a );
    }
}

/* ------------------------------------------------------------- the probe */

void __stdcall guest_fibers_entry( void )
{
    ULONGLONG main_base;
    int i;

    out( "guest_fibers: start\n" );

    check( "not a fiber yet", !IsThreadAFiber() );
    main_base = teb_stack_base();

    main_fiber = ConvertThreadToFiber( (void *)0x77 );
    check( "ConvertThreadToFiber returned a fiber", main_fiber != NULL );
    check( "now a fiber", IsThreadAFiber() != FALSE );
    check( "GetCurrentFiber sees it", current_fiber() == main_fiber );
    check( "GetFiberData sees its parameter", current_fiber_data() == (void *)0x77 );
    check( "the thread's own stack did not move", teb_stack_base() == main_base );

    fiber_a = CreateFiber( 0x20000, fiber_a_proc, (void *)0x0a );
    fiber_b = CreateFiber( 0x20000, fiber_b_proc, (void *)0x0b );
    check( "CreateFiber A", fiber_a != NULL );
    check( "CreateFiber B", fiber_b != NULL );
    if (!fiber_a || !fiber_b || !main_fiber)
    {
        out( "guest_fibers: FAIL (no fibers to switch to)\n" );
        ExitProcess( 1 );
    }

    /* main -> A -> main, twice: the plain case, and the one that runs A's
     * deep frame. */
    for (i = 0; i < 2; i++)
    {
        mark( 'M' );
        SwitchToFiber( fiber_a );
        check( "back on the thread's own stack after A", teb_stack_base() == main_base );
        check( "main's locals are still main's", on_my_own_stack() != FALSE );
    }

    /* main -> B -> A -> main: B never returns to main, it switches to A,
     * which resumes a run it parked two switches ago. */
    mark( 'M' );
    SwitchToFiber( fiber_b );
    check( "back on the thread's own stack after B->A", teb_stack_base() == main_base );

    mark( 'M' );

    check( "A ran three times", a_turns == 3 );
    check( "B ran once", b_turns == 1 );
    check( "A was told its own stack every time", a_stack_ok != FALSE );
    check( "B was told its own stack", b_stack_ok != FALSE );
    check( "A's locals survived every switch", a_locals_kept != FALSE );
    check( "A and B are on different stacks", a_base != b_base );
    check( "neither fiber ran on the thread's stack", a_base != main_base && b_base != main_base );
    check( "the switch order was M A M A M B A M", trace_is( "MAMAMBAM" ) );

    DeleteFiber( fiber_a );
    DeleteFiber( fiber_b );
    check( "still on the thread's own stack after DeleteFiber",
           teb_stack_base() == main_base && on_my_own_stack() != FALSE );

    check( "ConvertFiberToThread", ConvertFiberToThread() != FALSE );
    check( "not a fiber any more", !IsThreadAFiber() );
    check( "GetCurrentFiber says so too", current_fiber() == NULL );

    out( "guest_fibers: turns A=" );
    out_dec( a_turns );
    out( " B=" );
    out_dec( b_turns );
    out( "\n" );

    if (failures)
    {
        out( "guest_fibers: FAIL " );
        out_dec( failures );
        out( "\n" );
        ExitProcess( 1 );
    }
    out( "guest_fibers: PASS\n" );
    ExitProcess( 0 );
}
