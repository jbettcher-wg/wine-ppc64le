/*
 * seh_smoke -- the table-based (.pdata/.xdata) SEH dispatch gate.
 *
 * ONE source, built up to THREE times: as an x86-64 guest PE with real MSVC
 * structured exception handling (clang's __try/__except/__finally, which emit
 * a .pdata/.xdata exception directory naming __C_specific_handler), as the
 * same guest PE with -DSEH_SMOKE_UNHANDLED (a fault outside any __try, the
 * negative control), and -- to whatever extent the ppc64 toolchain can express
 * it at all, see check-seh-smoke.sh -- as a native ppc64 Windows PE.
 *
 * WHAT IT EXERCISES, AND WHY IT IS BUILT THE WAY IT IS.
 *
 * Every step below checks a VALUE, not "it did not crash".  A dispatcher that
 * merely reaches a filter is not a dispatcher; the things that go wrong in a
 * cross-machine unwind are the exception code arriving mangled, the faulting
 * address being the emulator's rather than the guest's, a __finally running
 * twice or not at all, the frame walk stopping at the first frame, and
 * execution resuming with the wrong stack pointer.  So:
 *
 *   1  the filter's GetExceptionCode() is STATUS_ACCESS_VIOLATION for a null
 *      store, and ExceptionInformation says "write" at address 0;
 *   2  the faulting address lies inside the function that faulted;
 *   3  RaiseException of a private code arrives with that code and both of
 *      its parameters;
 *   4  a __finally runs EXACTLY ONCE and in the right ORDER -- filter first
 *      (search phase), then the __finally (unwind phase), then the __except
 *      body -- checked as a whole ordered trace, not as a set of flags;
 *   5  a fault three frames down unwinds through an intermediate frame whose
 *      own filter says CONTINUE_SEARCH, running that frame's __finally on the
 *      way out and landing in the outermost __except;
 *   6  execution continues after the __except block with the target frame's
 *      locals intact -- one witness written by the FILTER through a pointer
 *      taken before the __try (so the faulting frame was still live when the
 *      filter ran) and one written by the handler body;
 *   7  a __finally reached by ordinary fall-through runs exactly once.
 *
 * THE VOLATILE FUNCTION POINTER (SEH_CALL).  clang does not treat a memory
 * fault as an exceptional edge unless -fasync-exceptions is in force, and
 * -fasync-exceptions is silently IGNORED for -target x86_64-windows-gnu (it
 * is accepted only for the -windows-msvc target; clang warns "argument unused
 * during compilation" and carries on).  A __try whose body clang believes
 * cannot unwind gets NO handler at all: the UNWIND_INFO flags byte comes out
 * 0, the __except funclet becomes dead code, and the probe would compile,
 * link, run and pass while proving nothing.  That was measured on this tree's
 * clang 22.1.8, at -O0 as well as -O1.
 *
 * So every __try body here calls THROUGH a volatile function pointer.  An
 * indirect call to a value clang has to reload from volatile storage cannot
 * be proven nounwind by any interprocedural pass, so the handler is always
 * emitted.  check-seh-smoke.sh asserts that on the built image rather than
 * trusting this comment: it counts the UNWIND_INFOs that actually carry
 * UNW_FLAG_EHANDLER and requires __C_specific_handler among the imports.
 *
 * ONE SOURCE, TWO EXCEPTION SYNTAXES.  The bodies are written with Wine's own
 * __TRY/__EXCEPT/__FINALLY/__ENDTRY from wine/exception.h.  With
 * USE_COMPILER_EXCEPTIONS defined -- which the guest build defines -- that
 * header expands them to the compiler's __try/__except/__finally, which is
 * exactly the construct under test.  Without it they expand to Wine's
 * setjmp-and-TEB-chain emulation, which is a DIFFERENT mechanism with the
 * same surface semantics; see the native-lane discussion in the runner for
 * what that lane can and cannot be said to prove.
 *
 * NO C RUNTIME on the guest side (-DSEH_SMOKE_NO_CRT): the program formats
 * its own output and writes it with WriteFile, and this file's entry point IS
 * the image entry point.  A CRT would put a mountain of unrelated .pdata
 * between the fault and the frame under test.
 *
 * Copyright 2026 the ppc64le port authors
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 */

#include <windows.h>
#include <wine/exception.h>

#define SEH_NOINLINE __attribute__((noinline))

/* the private code raised by step 3; not a system facility, deliberately */
#define SEH_SMOKE_RAISE_CODE   0xe5e40001
#define SEH_SMOKE_RAISE_ARG0   0x11223344
#define SEH_SMOKE_RAISE_ARG1   0x55667788

/* ------------------------------------------------------------- output */

static void out( const char *s )
{
    DWORD n = 0, written;

    while (s[n]) n++;
    WriteFile( GetStdHandle( STD_OUTPUT_HANDLE ), s, n, &written, NULL );
}

static void out_hex( ULONGLONG v, int digits )
{
    static const char hex[] = "0123456789abcdef";
    char buf[17];
    int i;

    for (i = 0; i < digits; i++) buf[digits - 1 - i] = hex[(v >> (4 * i)) & 0xf];
    buf[digits] = 0;
    out( buf );
}

static void out_dec( ULONG v )
{
    char buf[12];
    int i = 11;

    buf[i] = 0;
    do { buf[--i] = '0' + (char)(v % 10); v /= 10; } while (v);
    out( buf + i );
}

/* ------------------------------------------------------------- the trace
 *
 * The ordered marker trace is the instrument for the ordering steps.  A set
 * of "did the finally run" booleans cannot tell "ran once, in the right
 * place" from "ran twice, in the wrong order", and those are precisely the
 * two ways a hand-written unwinder fails first.
 */

static char trace_buf[512];
static int  trace_len;

static void trace_reset(void)
{
    trace_len = 0;
    trace_buf[0] = 0;
}

static void trace( const char *tok )
{
    int i = 0;

    if (trace_len && trace_len < (int)sizeof(trace_buf) - 1) trace_buf[trace_len++] = ' ';
    while (tok[i] && trace_len < (int)sizeof(trace_buf) - 1) trace_buf[trace_len++] = tok[i++];
    trace_buf[trace_len] = 0;
}

static BOOL trace_is( const char *want )
{
    int i;

    for (i = 0; want[i]; i++) if (trace_buf[i] != want[i]) return FALSE;
    return trace_buf[i] == 0;
}

/* ------------------------------------------------------------- stepping */

static int failures;
static int step;

static void begin( const char *what )
{
    out( "step " );
    out_dec( ++step );
    out( " " );
    out( what );
    out( ": " );
}

static void verdict( BOOL ok, const char *why )
{
    if (ok) out( " ok\n" );
    else
    {
        failures++;
        out( " FAIL (" );
        out( why );
        out( ")\n" );
    }
}

/* ------------------------------------------------------------- the faults
 *
 * null_ptr is volatile and never assigned, so the store below is a genuine
 * runtime null store the compiler may not fold into a trap instruction of its
 * own choosing; the fault has to come from the machine.
 */

static int * volatile null_ptr;

typedef void (*seh_fn)( void );
static seh_fn volatile seh_hook;

/* Every __try body in this file calls through this.  See the header comment:
 * an indirect call through volatile storage is the only construction that
 * survives clang's "this body cannot unwind, drop the handler" reasoning on
 * the -windows-gnu target, where -fasync-exceptions does not apply. */
#define SEH_CALL(f)  do { seh_hook = (f); seh_hook(); } while (0)

SEH_NOINLINE static void fault_null_store( void )
{
    trace( "store" );
    *null_ptr = 0x5e;
    trace( "store-returned-BUG" );
}

/* The window the faulting address must fall inside.  Taking it from the
 * function's own address rather than printing the address keeps the whole
 * transcript deterministic, which is what lets the runner diff it. */
#define FAULT_WINDOW 0x400

SEH_NOINLINE static void raise_private_code( void )
{
    ULONG_PTR args[2];

    args[0] = SEH_SMOKE_RAISE_ARG0;
    args[1] = SEH_SMOKE_RAISE_ARG1;
    trace( "raise" );
    RaiseException( SEH_SMOKE_RAISE_CODE, 0, 2, args );
    trace( "raise-returned-BUG" );
}

SEH_NOINLINE static void do_nothing( void )
{
    trace( "nofault" );
}

/* ------------------------------------------------------------- what a filter saw */

struct seen
{
    DWORD     code;
    DWORD     flags;
    DWORD     nparam;
    ULONGLONG info0;
    ULONGLONG info1;
    ULONG_PTR addr;
    int       calls;
};

static struct seen seen_av;
static struct seen seen_raise;

static void record( struct seen *s, EXCEPTION_POINTERS *ptrs )
{
    EXCEPTION_RECORD *rec = ptrs->ExceptionRecord;

    s->calls++;
    s->code   = rec->ExceptionCode;
    s->flags  = rec->ExceptionFlags;
    s->nparam = rec->NumberParameters;
    s->info0  = rec->NumberParameters > 0 ? rec->ExceptionInformation[0] : ~(ULONGLONG)0;
    s->info1  = rec->NumberParameters > 1 ? rec->ExceptionInformation[1] : ~(ULONGLONG)0;
    s->addr   = (ULONG_PTR)rec->ExceptionAddress;
}

static LONG CALLBACK av_filter( EXCEPTION_POINTERS *ptrs )
{
    trace( "avfilt" );
    record( &seen_av, ptrs );
    return EXCEPTION_EXECUTE_HANDLER;
}

static LONG CALLBACK raise_filter( EXCEPTION_POINTERS *ptrs )
{
    trace( "raisefilt" );
    record( &seen_raise, ptrs );
    return EXCEPTION_EXECUTE_HANDLER;
}

/* ------------------------------------------------------------- 1-2: the AV */

SEH_NOINLINE static void run_av( void )
{
    __TRY
    {
        SEH_CALL( fault_null_store );
    }
    __EXCEPT( av_filter )
    {
        trace( "avhandler" );
    }
    __ENDTRY
}

/* ------------------------------------------------------------- 3: RaiseException */

SEH_NOINLINE static void run_raise( void )
{
    __TRY
    {
        SEH_CALL( raise_private_code );
    }
    __EXCEPT( raise_filter )
    {
        trace( "raisehandler" );
    }
    __ENDTRY
}

/* ------------------------------------------------------------- 4: ordering */

static int order_finally_calls;
static int order_finally_abnormal;

static void CALLBACK order_finally( BOOL normal )
{
    order_finally_calls++;
    if (!normal) order_finally_abnormal++;
    trace( normal ? "fin-normal" : "fin-abnormal" );
}

static LONG CALLBACK order_filter( EXCEPTION_POINTERS *ptrs )
{
    trace( "orderfilt" );
    (void)ptrs;
    return EXCEPTION_EXECUTE_HANDLER;
}

/* MSVC SEH has no __try/__except/__finally in one block -- neither does
 * Wine's macro form -- so the __finally lives one frame down, which is also
 * the only arrangement in which "the __finally ran during the unwind" is
 * distinguishable from "the __finally ran on the way out of its own frame". */
SEH_NOINLINE static void order_inner( void )
{
    __TRY
    {
        SEH_CALL( fault_null_store );
    }
    __FINALLY( order_finally )
    trace( "order_inner-returned-BUG" );
}

SEH_NOINLINE static void run_order( void )
{
    __TRY
    {
        SEH_CALL( order_inner );
    }
    __EXCEPT( order_filter )
    {
        trace( "orderhandler" );
    }
    __ENDTRY
}

/* ------------------------------------------------------------- 5: two frames */

static int nest_finally_calls;
static DWORD nest_search_code;

static void CALLBACK nest_finally( BOOL normal )
{
    nest_finally_calls++;
    trace( normal ? "midfin-normal" : "midfin-abnormal" );
}

static LONG CALLBACK nest_search_filter( EXCEPTION_POINTERS *ptrs )
{
    trace( "innerfilt" );
    nest_search_code = ptrs->ExceptionRecord->ExceptionCode;
    return EXCEPTION_CONTINUE_SEARCH;
}

static LONG CALLBACK nest_outer_filter( EXCEPTION_POINTERS *ptrs )
{
    trace( "outerfilt" );
    (void)ptrs;
    return EXCEPTION_EXECUTE_HANDLER;
}

/* level 3: faults inside its own __try, whose filter declines */
SEH_NOINLINE static void nest_level3( void )
{
    trace( "L3" );
    __TRY
    {
        SEH_CALL( fault_null_store );
    }
    __EXCEPT( nest_search_filter )
    {
        trace( "L3handler-BUG" );
    }
    __ENDTRY
    trace( "L3-returned-BUG" );
}

/* level 2: no handler, only a __finally the unwind has to run on its way past */
SEH_NOINLINE static void nest_level2( void )
{
    trace( "L2" );
    __TRY
    {
        SEH_CALL( nest_level3 );
    }
    __FINALLY( nest_finally )
    trace( "L2-returned-BUG" );
}

SEH_NOINLINE static void run_nested( void )
{
    __TRY
    {
        trace( "L1" );
        SEH_CALL( nest_level2 );
    }
    __EXCEPT( nest_outer_filter )
    {
        trace( "L1handler" );
    }
    __ENDTRY
    trace( "L1after" );
}

/* ------------------------------------------------------------- 6: continuation */

static volatile int *witness_ptr;

static LONG CALLBACK witness_filter( EXCEPTION_POINTERS *ptrs )
{
    trace( "witnessfilt" );
    (void)ptrs;
    /* The faulting frame is still live during the search phase, so this write
     * lands in run_continue's live stack frame.  If the unwind then restores
     * that frame correctly, run_continue reads it back after the handler. */
    if (witness_ptr) *witness_ptr = 0x111;
    return EXCEPTION_EXECUTE_HANDLER;
}

SEH_NOINLINE static int run_continue( void )
{
    volatile int witness = 0x11;

    witness_ptr = &witness;
    __TRY
    {
        SEH_CALL( fault_null_store );
    }
    __EXCEPT( witness_filter )
    {
        trace( "witnesshandler" );
        witness = witness + 0x111;
    }
    __ENDTRY
    trace( "witnessafter" );
    witness_ptr = NULL;
    return witness;
}

/* ------------------------------------------------------------- 7: fall-through */

static int fall_finally_calls;

static void CALLBACK fall_finally( BOOL normal )
{
    fall_finally_calls++;
    trace( normal ? "fall-normal" : "fall-abnormal" );
}

SEH_NOINLINE static void run_fallthrough( void )
{
    __TRY
    {
        SEH_CALL( do_nothing );
    }
    __FINALLY( fall_finally )
}

/* ------------------------------------------------------------- the run */

/* External linkage on both entry points, so that whichever of the two this
 * build does not use is not a -Wunused-function warning. */
int seh_smoke_run( void )
{
    ULONG_PTR fault_fn = (ULONG_PTR)(void *)fault_null_store;
    ULONG_PTR raise_fn = (ULONG_PTR)(void *)raise_private_code;
    int witness;

    out( "seh_smoke: start\n" );

    /* ---- 1: the access violation reaches a filter with the right code ---- */
    trace_reset();
    run_av();
    begin( "null store: filter code" );
    out( "code=0x" );
    out_hex( seen_av.code, 8 );
    out( " calls=" );
    out_dec( (ULONG)seen_av.calls );
    verdict( seen_av.calls == 1 && seen_av.code == (DWORD)STATUS_ACCESS_VIOLATION,
             "not exactly one filter call with STATUS_ACCESS_VIOLATION" );

    begin( "null store: exception information" );
    out( "nparam=" );
    out_dec( seen_av.nparam );
    out( " info0=0x" );
    out_hex( seen_av.info0, 16 );
    out( " info1=0x" );
    out_hex( seen_av.info1, 16 );
    verdict( seen_av.nparam >= 2 && seen_av.info0 == 1 && seen_av.info1 == 0,
             "not a write (1) to address 0" );

    begin( "null store: faulting address is inside the faulting function" );
    out( "in_window=" );
    out( (seen_av.addr >= fault_fn && seen_av.addr < fault_fn + FAULT_WINDOW)
         ? "yes" : "no" );
    verdict( seen_av.addr >= fault_fn && seen_av.addr < fault_fn + FAULT_WINDOW,
             "ExceptionAddress is not in fault_null_store" );

    begin( "null store: trace" );
    out( "'" );
    out( trace_buf );
    out( "'" );
    verdict( trace_is( "store avfilt avhandler" ), "wrong order" );

    /* ---- 2: RaiseException of a private code -------------------------- */
    trace_reset();
    run_raise();
    begin( "RaiseException: filter code and parameters" );
    out( "code=0x" );
    out_hex( seen_raise.code, 8 );
    out( " nparam=" );
    out_dec( seen_raise.nparam );
    out( " info0=0x" );
    out_hex( seen_raise.info0, 8 );
    out( " info1=0x" );
    out_hex( seen_raise.info1, 8 );
    verdict( seen_raise.calls == 1 && seen_raise.code == SEH_SMOKE_RAISE_CODE &&
             seen_raise.nparam == 2 && seen_raise.info0 == SEH_SMOKE_RAISE_ARG0 &&
             seen_raise.info1 == SEH_SMOKE_RAISE_ARG1,
             "the private code or its parameters did not arrive" );

    begin( "RaiseException: raising address is inside the raising function" );
    out( "in_window=" );
    out( (seen_raise.addr >= raise_fn && seen_raise.addr < raise_fn + FAULT_WINDOW)
         ? "yes" : "no" );
    verdict( seen_raise.addr >= raise_fn && seen_raise.addr < raise_fn + FAULT_WINDOW,
             "ExceptionAddress is not in raise_private_code" );

    begin( "RaiseException: trace" );
    out( "'" );
    out( trace_buf );
    out( "'" );
    verdict( trace_is( "raise raisefilt raisehandler" ), "wrong order" );

    /* ---- 3: __finally exactly once, filter before it, handler after --- */
    trace_reset();
    run_order();
    begin( "unwind order: filter, then __finally, then the __except body" );
    out( "'" );
    out( trace_buf );
    out( "'" );
    verdict( trace_is( "store orderfilt fin-abnormal orderhandler" ),
             "the __finally did not run between the filter and the handler" );

    begin( "unwind order: the __finally ran exactly once, abnormally" );
    out( "calls=" );
    out_dec( (ULONG)order_finally_calls );
    out( " abnormal=" );
    out_dec( (ULONG)order_finally_abnormal );
    verdict( order_finally_calls == 1 && order_finally_abnormal == 1,
             "not exactly one abnormal termination" );

    /* ---- 4: the walk crosses frames ----------------------------------- */
    trace_reset();
    run_nested();
    begin( "two frames: declining filter, intermediate __finally, outer handler" );
    out( "'" );
    out( trace_buf );
    out( "'" );
    verdict( trace_is( "L1 L2 L3 store innerfilt outerfilt midfin-abnormal "
                       "L1handler L1after" ),
             "the frame walk did not cross two frames in order" );

    begin( "two frames: the declining filter saw the code, the __finally ran once" );
    out( "innercode=0x" );
    out_hex( nest_search_code, 8 );
    out( " midfin=" );
    out_dec( (ULONG)nest_finally_calls );
    verdict( nest_search_code == (DWORD)STATUS_ACCESS_VIOLATION &&
             nest_finally_calls == 1,
             "the intermediate frame was skipped or run twice" );

    /* ---- 5: execution continues, in the right frame -------------------- */
    trace_reset();
    witness = run_continue();
    begin( "continuation: the frame's local survives filter and handler" );
    out( "witness=0x" );
    out_hex( (ULONGLONG)(ULONG)witness, 3 );
    verdict( witness == 0x222,
             "0x11 -> filter 0x111 -> handler +0x111 did not end at 0x222" );

    begin( "continuation: the code after the __except block runs" );
    out( "'" );
    out( trace_buf );
    out( "'" );
    verdict( trace_is( "store witnessfilt witnesshandler witnessafter" ),
             "execution did not resume after the __except block" );

    /* ---- 6: fall-through --------------------------------------------- */
    trace_reset();
    run_fallthrough();
    begin( "fall-through: __finally runs exactly once, normally" );
    out( "calls=" );
    out_dec( (ULONG)fall_finally_calls );
    out( " trace='" );
    out( trace_buf );
    out( "'" );
    verdict( fall_finally_calls == 1 && trace_is( "nofault fall-normal" ),
             "the fall-through __finally did not run exactly once" );

    out( failures ? "seh_smoke: FAIL " : "seh_smoke: PASS " );
    out_dec( (ULONG)(step - failures) );
    out( "/" );
    out_dec( (ULONG)step );
    out( "\n" );
    return failures ? 1 : 0;
}

/* ------------------------------------------------------------- the negative control
 *
 * A fault OUTSIDE any __try.  Nothing here may catch it: the run must reach
 * the port's existing unhandled path and die promptly, loudly and by name.
 * A gate whose red state is a hang or a silent zero exit is not a gate, so
 * the runner asserts on the status AND on the text.
 */

int seh_smoke_unhandled( void )
{
    out( "seh_smoke: unhandled probe, faulting outside any __try\n" );
    SEH_CALL( fault_null_store );
    out( "seh_smoke: FAIL the unhandled fault returned\n" );
    return 1;
}

#ifdef SEH_SMOKE_NO_CRT
/* The guest build has no C runtime: this IS the image entry point. */
void WINAPI seh_smoke_entry( void )
{
#ifdef SEH_SMOKE_UNHANDLED
    ExitProcess( (UINT)seh_smoke_unhandled() );
#else
    ExitProcess( (UINT)seh_smoke_run() );
#endif
}
#else
int main( void )
{
#ifdef SEH_SMOKE_UNHANDLED
    return seh_smoke_unhandled();
#else
    return seh_smoke_run();
#endif
}
#endif
