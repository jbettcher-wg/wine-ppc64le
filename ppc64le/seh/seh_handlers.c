/*
 * seh_handlers -- the gate for a guest language handler ENTERED AS GUEST CODE.
 *
 * check-seh-smoke.sh proved that a compiled __try reaches a filter: that gate's
 * every frame names ntdll's __C_specific_handler, and the port recognises that
 * one address by identity and runs its semantics natively.  That is the fast
 * path, and it is the only path a probe built from C can reach, because clang
 * names __C_specific_handler in the .xdata of every __try it compiles -- for
 * -windows-gnu and -windows-msvc alike.
 *
 * A real application does not stay on that path.  An image linked against the
 * static MSVC runtime carries its OWN byte-identical copy of
 * __C_specific_handler (DOOM (2016), at DOOMx64vk.exe+0x1eab2c8), its own
 * __GSHandlerCheck (steam_api64.dll+0xed68) and its own __CxxFrameHandler*, and
 * the .xdata names those.  Nothing in a PE says which handler an RVA is, so the
 * port has to ENTER IT AS GUEST CODE -- handler( EXCEPTION_RECORD *, void
 * *EstablisherFrame, CONTEXT *, DISPATCHER_CONTEXT * ), MS-x64, in a nested
 * emulator run -- and honour the disposition it returns.  That is the code this
 * file exists to gate, together with the two things such a handler then does:
 * read the DISPATCHER_CONTEXT it was handed, and call RtlUnwindEx.
 *
 * WHAT IS MEASURED, AND WHY EACH THING IS MEASURED AS A RELATION.
 *
 *   (a) A PRIVATE handler.  The frame is hand-written in seh_handlers_asm.S,
 *       because there is no way to make clang name a handler of ours.  The
 *       handler prints what its DISPATCHER_CONTEXT said -- but as RELATIONS
 *       ("imagebase_matches=yes"), never as addresses, because a gate whose
 *       expected output contains an address cannot be diffed byte for byte
 *       across runs, and a gate that is not diffed is a gate that drifts.
 *
 *   (b) The MSVC thread-naming idiom, RaiseException( 0x406D1388, 0, 4, info ).
 *       It is here because it is the exception a real Windows program raises
 *       most often and always deliberately: every threading library on Windows
 *       raises it once per thread and expects the debugger, or nobody, to eat
 *       it.  Both dispositions a filter can answer with are exercised, under
 *       BOTH handler paths -- the private one and an ordinary clang __try --
 *       and the two paths are then required to have seen the SAME record.  Two
 *       implementations of the same contract that disagree about what arrived
 *       are worth more as a comparison than either is alone.
 *
 *   (c) RtlUnwindEx called BY the guest handler, with a __finally in a frame
 *       between the raise and the target that must run exactly once and
 *       abnormally, and with RAX on arrival carrying the ReturnValue.  The
 *       landing pad is assembler for that reason: a C __except body cannot see
 *       RAX.  Both import routes are exercised, KERNEL32.dll (which is where
 *       DOOM takes it from, importing nothing at all from ntdll) and ntdll.dll,
 *       because the port needs a separate thunk override row for each module
 *       and a missing row is silent -- the call simply lands in the native
 *       ppc64 RtlUnwindEx, which unwinds a stack that is not there.
 *
 *   (d) A CHAINING private handler: the .xdata names a handler of ours, and
 *       that handler tail-calls ntdll's __C_specific_handler with the four
 *       arguments it was given.  This is what an MSVC /GS build produces all
 *       day -- __GSHandlerCheck_SEH validates the frame's stack cookie and then
 *       chains -- and it arrives at the port through a DIFFERENT door from the
 *       frame walk's: not the identity fast path, but a guest CALL to the
 *       ntdll export, i.e. the thunk override.  Answering that with
 *       ExceptionContinueSearch would silently deny the frame the __except it
 *       is entitled to, which is the failure this stage exists to catch.
 *
 *   (e) A CONSOLIDATING unwind: RtlUnwindEx with STATUS_UNWIND_CONSOLIDATE,
 *       which is how MSVC spells `catch` and what DOOM (2016) reaches the
 *       instant SteamAPI_Init fails and its error path throws.  The unwind runs
 *       normally -- the intermediate __finally and all -- and then, instead of
 *       resuming at TargetIp, calls the routine in ExceptionInformation[0] and
 *       resumes at the address that routine returns.  Three things are measured
 *       that nothing else here measures: that the whole eleven-slot record
 *       reaches the routine (a real __CxxCallCatchBlock reads slots 1, 2, 4, 5,
 *       6 and 7 and would run a catch block against an invented frame without
 *       them), that the __finally ran BEFORE the routine did, and that the
 *       resume address came from the routine's return value rather than from
 *       the TargetIp the protocol requires to be ignored -- which is provable
 *       because the frame has two landing pads and they are handed different
 *       roles.
 *
 * WHAT COULD NOT BE MEASURED WITH A REAL try/catch, and it is a measurement
 * rather than an omission, and it was measured with the toolchain this gate
 * runs on rather than assumed.  clang -target x86_64-windows-gnu compiles C++
 * try/catch with `.seh_handler __gxx_personality_seh0` -- libstdc++'s SEH
 * personality, which is not MSVC's and does not use STATUS_UNWIND_CONSOLIDATE
 * at all -- and linking one into a -nostdlib image fails on
 * __cxa_allocate_exception, __cxa_throw and __cxa_begin_catch.  -target
 * x86_64-windows-msvc does emit `.seh_handler __CxxFrameHandler3` and a call to
 * _CxxThrowException, and this tree's guest msvcrt thunk exports neither of
 * those names among its 664, so that lane does not link either.  So the record
 * is hand-built to the shape Wine's own __CxxFrameHandler builds
 * (dlls/msvcrt/except.c, find_catch_block), which is the same protocol seen
 * from the producing side.
 *
 * NO NATIVE LANE, AND THIS IS THE PLACE THAT SAYS SO.  check-seh-smoke.sh has a
 * native ppc64 lane because its source is written in Wine's __TRY macros, which
 * have a setjmp expansion.  This probe cannot have one: the construct under
 * test is a hand-written x86-64 .seh_proc with a .seh_handler directive and an
 * @IMGREL scope table.  There is no ppc64 spelling of that -- not a different
 * spelling, none -- so there is nothing to corroborate against and the embedded
 * transcript is the whole value gate.  What replaces the native lane here is
 * the (b) comparison: the private path and ntdll's own __C_specific_handler are
 * two independent implementations reached over the same boundary, and they are
 * required to agree.
 *
 * NO C RUNTIME: this file's entry point IS the image
 * entry point and it formats its own output for WriteFile.  A CRT would put a
 * mountain of unrelated .pdata between the raise and the frame under test, and
 * -- worse for this probe in particular -- would import __C_specific_handler
 * into frames the walk crosses on its way out.
 *
 * THE VOLATILE FUNCTION POINTER, inherited from seh_smoke.c and for the same
 * measured reason: clang attaches a language handler only to a __try whose body
 * it believes can unwind, -fasync-exceptions is accepted-and-ignored for
 * -target x86_64-windows-gnu, and a __try that lost its handler compiles, links,
 * runs and passes while proving nothing.  Every __try body here, and every call
 * into the hand-written frame, goes through volatile storage.  The runner
 * asserts the result on the built image rather than trusting this paragraph.
 *
 * Copyright 2026 the ppc64le port authors
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 */

#include <windows.h>
/* GetExceptionInformation() is not a function: it is _exception_info(), the
 * compiler intrinsic that is only meaningful inside a __except filter
 * expression, spelled by <excpt.h>.  wine/exception.h reaches it the same way;
 * this file does not use the __TRY macros, because there is no native lane to
 * share a source with. */
#include <excpt.h>

#define SEH_NOINLINE __attribute__((noinline))

/* ------------------------------------------------------------- constants
 *
 * Private codes, deliberately outside every system facility, so that a record
 * arriving at a handler can only have come from this file.
 */
#define PF_CODE_A          0xe5e50001    /* stage A: the dispatcher-context probe */
#define PF_A_ARG0          0x0a0a0001
#define PF_A_ARG1          0x0a0a0002
#define PF_CODE_C          0xe5e50003    /* stage C: the unwind probe */
#define PF_CODE_D          0xe5e50004    /* stage D: the chaining-handler probe */
#define PF_CODE_G          0xe5e50005    /* stages G and H: the exception the OUTER unwind answers */
#define PF_CODE_G_INNER    0xe5e50006    /* ...and the record the COLLIDING unwind carries */
#define PF_G_INNER_INFO    0xc0111de50000ull

/* Stage E, the consolidating unwind: one distinct value per ExceptionInformation
 * slot beyond the two the protocol defines, so that a port which carried slot 0
 * and dropped the rest -- or shifted them by one -- produces a wrong VALUE at a
 * named index rather than a plausible-looking record.  Nine of them, because
 * Wine's own __CxxFrameHandler fills eleven slots (dlls/msvcrt/except.c,
 * find_catch_block) and this record is built to that width deliberately. */
#define PF_CONS_MAGIC(i)   (0xc0c0de0000000000ull + ((ULONGLONG)(i) << 8) + (i))
#define PF_CONS_NPARAM     11

/* The MSVC thread-naming exception, byte for byte as every Windows threading
 * library raises it.  The shape is MSVC's THREADNAME_INFO { DWORD dwType;
 * LPCSTR szName; DWORD dwThreadID; DWORD dwFlags; } passed as an array of
 * ULONG_PTR with a count of 4 -- which is the form the task under gate names,
 * and which is also what a caller that writes the parameters out by hand
 * produces (the classic snippet's sizeof(info)/sizeof(ULONG_PTR) yields 3 on
 * x64 because of the struct's padding; both counts have to survive, and 4 is
 * the one that puts a fourth word in ExceptionInformation to be checked). */
#define MS_VC_EXCEPTION    0x406d1388
#define PF_THREAD_NAME     "seh-handlers-probe"

/* Unwind return values: one per unwind performed, all distinct, so that a
 * landing pad reporting the wrong one is a wrong VALUE and not a coin flip. */
#define PF_RETVAL_B        0x00c0ffee0000eeb0ull   /* stage B, KERNEL32 route */
#define PF_RETVAL_CK       0x00c0ffee0000eec1ull   /* stage C, KERNEL32 route */
#define PF_RETVAL_CN       0x00c0ffee0000eec2ull   /* stage C, ntdll route    */
#define PF_RETVAL_E        0x00c0ffee0000eee5ull   /* stage E, the consolidating unwind */
#define PF_RETVAL_F        0x00c0ffee0000eef6ull   /* stage F, the same in place */
#define PF_RETVAL_G        0x00c0ffee0000ee97ull   /* stage G, the collided unwind, deferred road */
#define PF_RETVAL_H        0x00c0ffee0000ee98ull   /* stage H, the collided unwind, in place */

/* Kept in step with seh_handlers_asm.S by hand.  The probe reads the value back
 * out of the landing pad, so a drift between the two files is a failed step. */
#define PF_RBX_SENTINEL    0x5eb15eb15eb15eb1ull
#define PF_R12_SENTINEL    0x5eb25eb25eb25eb2ull

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

static void out_yn( const char *label, BOOL yes )
{
    out( label );
    out( yes ? "=yes" : "=no" );
}

/* ------------------------------------------------------------- the trace
 *
 * The same instrument seh_smoke.c uses, and for the same reason: a set of "did
 * it run" booleans cannot tell "ran once, in the right place" from "ran twice,
 * in the wrong order", and an unwinder's first two failure modes are exactly
 * those two.  Here it also has to record ORDER ACROSS THE MACHINE BOUNDARY --
 * the private handler's marker is written by x86-64 code running in a nested
 * emulator run, into the same buffer the guest's ordinary flow writes.
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

static void out_trace( void )
{
    out( "trace='" );
    out( trace_buf );
    out( "'" );
}

/* ------------------------------------------------------------- the boundary */

/* Every guarded body and every call into the hand-written frame goes through
 * volatile storage.  See the header comment: without it clang decides the body
 * cannot unwind and emits no language handler at all. */
typedef void (*pf_body)( ULONG_PTR arg );

static pf_body volatile pf_hook;
#define SEH_CALL(f)  do { pf_hook = (f); pf_hook( 0 ); } while (0)

/* seh_handlers_asm.S.  pf_call( fn, arg ) establishes the frame whose .xdata
 * names pf_language_handler and calls fn(arg) inside its guarded range;
 * pf_landing is the scope table's JumpTarget, where an unwind lands. */
extern ULONG64 pf_call( pf_body fn, ULONG_PTR arg );
extern void    pf_landing( void );
extern void    pf_call_end( void );

/* The frame's SECOND landing pad, reached only when a consolidation routine
 * returns its address.  See seh_handlers_asm.S: handing RtlUnwindEx pf_landing
 * as TargetIp and resuming at THIS label is what tells the two roads apart. */
extern void    pf_consolidate_landing( void );

static ULONG64 (* volatile pf_call_hook)( pf_body, ULONG_PTR );
#define PF_CALL(f,a)  (pf_call_hook = pf_call, pf_call_hook( (f), (a) ))

/* Written by the landing pad, read here.  Not static: seh_handlers_asm.S names
 * them.  x86-64 PE has no leading underscore, so the names match as written. */
ULONG64 pf_landing_rax;
ULONG64 pf_landing_rbx;
ULONG64 pf_landing_hit;
ULONG64 pf_consolidate_rax;
ULONG64 pf_consolidate_rbx;
ULONG64 pf_consolidate_hit;

/* pf_call's own establisher frame, written by the frame itself; see the
 * comment on the store in seh_handlers_asm.S.  Read by the stage that unwinds
 * to this frame WITHOUT a DISPATCHER_CONTEXT to take it from. */
ULONG64 pf_call_frame;

/* The second hand-written frame: same shape, but its .xdata names the CHAINING
 * handler below.  Its scope record's HandlerAddress is 1, so once the chain
 * reaches __C_specific_handler that handler accepts the scope with no filter
 * funclet and unwinds here with the EXCEPTION CODE as the return value. */
extern ULONG64 pf_chain_call( pf_body fn, ULONG_PTR arg );
extern void    pf_chain_landing( void );
extern void    pf_chain_call_end( void );
ULONG64 pf_chain_rax;
ULONG64 pf_chain_r12;
ULONG64 pf_chain_hit;

/* The image's own base, as the linker knows it.  Used to turn the
 * DISPATCHER_CONTEXT's absolute ImageBase into a statement this probe can check
 * without printing an address. */
extern IMAGE_DOS_HEADER __ImageBase;

/* RtlUnwindEx by two routes.  KERNEL32.dll is the one that matters most -- DOOM
 * (2016) imports RtlUnwindEx, RtlLookupFunctionEntry, RtlVirtualUnwind and
 * RtlCaptureContext from KERNEL32 and nothing whatever from ntdll -- and
 * ntdll.dll is the canonical one.  They are two different import thunks in this
 * image, resolved through two different guest thunk modules, and the port needs
 * a separate override row for each; a missing row does not fail, it binds to
 * the NATIVE ppc64 RtlUnwindEx and unwinds a stack that is not there.  The
 * second name is an aliased import (`RtlUnwindEx_ntdll == RtlUnwindEx` in the
 * .def the runner writes), which is how one image imports one exported name
 * from two DLLs at once. */
void WINAPI RtlUnwindEx_ntdll( void *, void *, EXCEPTION_RECORD *, void *,
                               CONTEXT *, UNWIND_HISTORY_TABLE * );

typedef void (WINAPI *pf_unwind_fn)( void *, void *, EXCEPTION_RECORD *, void *,
                                     CONTEXT *, UNWIND_HISTORY_TABLE * );

/* ------------------------------------------------------------- what arrived
 *
 * One record of "what a handler was handed", filled in identically by the
 * private handler and by an ordinary clang __except filter, so that the two can
 * be compared field by field.
 */
struct seen
{
    int       calls;
    DWORD     code;
    DWORD     flags;
    DWORD     nparam;
    ULONGLONG info[4];
    ULONG_PTR addr;
};

static void record( struct seen *s, const EXCEPTION_RECORD *rec )
{
    UINT i;

    s->calls++;
    s->code   = rec->ExceptionCode;
    s->flags  = rec->ExceptionFlags;
    s->nparam = rec->NumberParameters;
    for (i = 0; i < 4; i++)
        s->info[i] = i < rec->NumberParameters ? rec->ExceptionInformation[i] : ~(ULONGLONG)0;
    s->addr = (ULONG_PTR)rec->ExceptionAddress;
}

static BOOL seen_same( const struct seen *a, const struct seen *b, BOOL *code, BOOL *flags,
                       BOOL *nparam, BOOL *info, BOOL *addr )
{
    UINT i;

    *code   = a->code   == b->code;
    *flags  = a->flags  == b->flags;
    *nparam = a->nparam == b->nparam;
    *addr   = a->addr   == b->addr;
    *info   = TRUE;
    for (i = 0; i < 4; i++) if (a->info[i] != b->info[i]) *info = FALSE;
    return *code && *flags && *nparam && *info && *addr;
}

/* ------------------------------------------------------------- the private handler
 *
 * The function the hand-written frame's .xdata names, and the reason this file
 * exists.  Its four-argument shape is the x64 language handler contract; on
 * -target x86_64-windows-gnu the default calling convention IS MS-x64, so no
 * attribute is needed and none is used -- an attribute here would hide a
 * mismatch rather than prevent one.
 *
 * HOW THIS PROVES IT RAN AS GUEST CODE.  The instructions in this function are
 * x86-64.  The only thing in this process that executes x86-64 is the emulator.
 * So the observable that settles it is simply "did anything in here run", and
 * the cheapest honest way to observe that from the guest side is a counter in
 * the probe image's own .data which only this function writes -- pf_witness,
 * seeded with a value the linker put there so that it is .data and not merely
 * zeroed .bss, and incremented once per entry.  The probe checks the exact
 * total.  That is the IN-PROCESS half; the runner independently greps the
 * port's own WINEDEBUG=+seh output for the "entering guest language handler"
 * line naming this function's address, which is the PORT's half.  Neither half
 * is worth much alone: a counter says code ran but not through which door, and
 * a trace line says the port intended to open the door but not that anything
 * came through it.  Together they close.
 */
ULONG64 pf_witness = 0x5e400000ull;      /* .data, seeded; only the handler writes it */

enum pf_mode
{
    PF_MODE_REPORT,        /* record everything, then ExceptionContinueSearch */
    PF_MODE_ACCEPT,        /* record, then RtlUnwindEx to pf_landing */
    PF_MODE_CONTINUE,      /* record, then ExceptionContinueExecution */
    PF_MODE_COLLIDE,       /* return ExceptionCollidedUnwind, forever -- must be REFUSED */
    PF_MODE_COLLIDE_DEFER, /* stage G: unwind from inside the unwind, from this handler */
    PF_MODE_COLLIDE_INPLACE, /* stage H: the same, from a __finally; this handler only lands */
    PF_MODE_EXIT_UNWIND,   /* RtlUnwindEx( NULL, ... ) -- must be REFUSED */
    PF_MODE_CONSOLIDATE,   /* RtlUnwindEx with STATUS_UNWIND_CONSOLIDATE */
    PF_MODE_CONS_NOROUTINE /* ...and no routine in it -- must be REFUSED */
};

static volatile int          pf_mode;
static volatile pf_unwind_fn pf_unwind;        /* which RtlUnwindEx to call */
static volatile ULONG64      pf_retval;        /* what to pass as ReturnValue */

/* Everything the handler observed about one stage.  Reset per stage, so that
 * the counts printed are that stage's and not the run's. */
struct dispatch_seen
{
    int   search_calls;
    int   unwind_calls;
    int   target_calls;

    /* identity: does the DISPATCHER_CONTEXT describe the frame we know it to be */
    BOOL  imagebase_matches;
    BOOL  functionentry_begin_matches;
    BOOL  functionentry_end_matches;
    BOOL  controlpc_in_frame;
    BOOL  controlpc_in_scope;
    BOOL  languagehandler_is_self;
    BOOL  establisherframe_matches_arg;

    /* the handler data: our own scope table, read back through the port */
    DWORD scope_count;
    BOOL  scope0_begin_matches;
    BOOL  scope0_end_matches;
    DWORD scope0_handler;
    BOOL  scope0_jumptarget_is_landing;

    /* phase-dependent fields */
    DWORD scopeindex_search;
    BOOL  targetip_zero_in_search;
    BOOL  targetip_nonzero_in_unwind;
    BOOL  ctxrecord_rip_is_controlpc_search;
    BOOL  ctxrecord_rip_is_controlpc_unwind;
    BOOL  ctxarg_is_ctxrecord_search;
    BOOL  ctxarg_is_ctxrecord_unwind;
    BOOL  unwinding_flag_in_unwind;
    BOOL  target_unwind_flag_seen;

    /* the unwinder's establisher frame against the frame's OWN post-prologue
     * RSP: two independent statements of the same number, and the premise of
     * the in-place stage, which names that frame from outside */
    BOOL  frame_matches_asm;
};

static struct dispatch_seen pf_seen;
static struct seen          pf_record;         /* the record the private handler saw */

/* ------------------------------------------------------------- stages G and H
 *
 * THE COLLIDED UNWIND: an unwind started from inside an unwind.
 *
 * Four frames, outermost first, so that every one of the contract's four claims
 * has a frame whose behaviour states it:
 *
 *   A  pf_call( g_body )        the INNER unwind's target.  Its private handler
 *                               is entered once, at the target, and its landing
 *                               pad reports RAX and the RBX sentinel.
 *   M  g_mid                    a __try/__finally BETWEEN the collision and the
 *                               inner target.  It is outside the OUTER unwind
 *                               entirely, so it must run exactly once, and only
 *                               because the collided unwind carried on past the
 *                               frame the outer one was stopping at.
 *   X  g_outer_target           a __try/__except whose filter ACCEPTS.  That is
 *                               what starts the outer unwind, and X is the frame
 *                               that unwind was heading for -- so its __except
 *                               body must never run: the collision preempted it.
 *   K  the colliding frame      whose __finally calls RtlUnwindEx for A while
 *                               the outer unwind is running it.  Two shapes, one
 *                               per road the port has:
 *                                 stage G  K is a second pf_call, and the
 *                                          private handler collides -- the
 *                                          DEFERRED road, a guest language
 *                                          handler unwinding from inside its own
 *                                          emulator run;
 *                                 stage H  K is an ordinary clang __finally, run
 *                                          by the port's own
 *                                          __C_specific_handler -- the road that
 *                                          used to end in guest_unwind_in_place
 *                                          and now ends in the collision.
 *
 * WHAT MAKES THIS A VALUE GATE RATHER THAN A CRASH TEST.  Four counters and one
 * ordering, each of which a plausible wrong implementation gets wrong:
 *
 *   - K's __finally / colliding handler runs ONCE.  An adoption that re-derived
 *     the dispatcher context would reset ScopeIndex to zero and run it again;
 *     that is the single most likely way to get this wrong, and stage H's
 *     k_finally count is exactly the instrument for it.
 *   - M's __finally runs ONCE.  Zero would mean the inner unwind stopped where
 *     the outer one was going; two would mean the walk restarted.
 *   - X's __except body runs NEVER, and its filter runs ONCE.
 *   - the landing pad reports the INNER unwind's ReturnValue in RAX, the frame's
 *     own RBX sentinel, and stage G's re-entered handler reports the inner
 *     unwind's RECORD -- a code this file uses nowhere else -- with
 *     EXCEPTION_COLLIDED_UNWIND set and the new TargetIp.  Nothing of the outer
 *     unwind's may be visible there.
 */
static ULONG64 g_target_frame;      /* pf_call instance A's establisher frame */

static int     g_search_k_calls;    /* the private handler at K, search phase */
static int     g_collide_calls;     /* ...and the collisions it started */
static BOOL    g_collided;          /* one collision per stage; see the comment at the call */
static ULONG64 g_collide_frame;     /* the DISPATCHER_CONTEXT at the collision... */
static DWORD   g_collide_scope;

/* What the colliding frame's handler saw when the port RE-ENTERED it after
 * adopting the collision -- the x64 contract's most exacting claim, and the one
 * no counter can make: same frame, same ScopeIndex, the INNER unwind's record,
 * the new TargetIp, EXCEPTION_COLLIDED_UNWIND set and EXCEPTION_TARGET_UNWIND
 * not (K is not the inner unwind's target). */
static int     g_readopt_calls;
static BOOL    g_readopt_same_frame, g_readopt_same_scope, g_readopt_collided_flag,
               g_readopt_no_target_flag, g_readopt_targetip_is_landing,
               g_readopt_code_is_inner;

/* ...and what the TARGET frame's handler saw when the collided unwind arrived. */
static int     g_arrive_calls;
static BOOL    g_arrive_code_is_inner, g_arrive_info_is_inner, g_arrive_target_flag,
               g_arrive_unwinding_flag, g_arrive_targetip_is_landing, g_arrive_frame_is_target;

static int     g_mid_finally_calls, g_mid_finally_abnormal;
static int     g_k_finally_calls, g_k_finally_abnormal;
static int     g_outer_filter_calls, g_outer_except_calls;

static void g_stage_reset( void )
{
    g_search_k_calls = g_collide_calls = g_readopt_calls = g_arrive_calls = 0;
    g_collided = FALSE;
    g_collide_frame = 0;
    g_collide_scope = 0;
    g_readopt_same_frame = g_readopt_same_scope = g_readopt_collided_flag =
        g_readopt_no_target_flag = g_readopt_targetip_is_landing =
        g_readopt_code_is_inner = FALSE;
    g_arrive_code_is_inner = g_arrive_info_is_inner = g_arrive_target_flag =
        g_arrive_unwinding_flag = g_arrive_targetip_is_landing =
        g_arrive_frame_is_target = FALSE;
    g_mid_finally_calls = g_mid_finally_abnormal = 0;
    g_k_finally_calls = g_k_finally_abnormal = 0;
    g_outer_filter_calls = g_outer_except_calls = 0;
}

/* The record the COLLIDING unwind carries.  Built rather than forwarded, and
 * with a code this file uses nowhere else, so that "the record the adopted
 * unwind runs with is the inner one's" is a value check and not an inference:
 * a port that carried the outer unwind's record past the collision would show
 * PF_CODE_G here, which is a different number. */
static void g_build_inner_record( EXCEPTION_RECORD *rec, void *addr )
{
    UINT i;

    rec->ExceptionCode    = PF_CODE_G_INNER;
    rec->ExceptionFlags   = 0;
    rec->ExceptionRecord  = NULL;
    rec->ExceptionAddress = addr;
    rec->NumberParameters = 1;
    for (i = 0; i < EXCEPTION_MAXIMUM_PARAMETERS; i++) rec->ExceptionInformation[i] = 0;
    rec->ExceptionInformation[0] = PF_G_INNER_INFO;
}

/* ------------------------------------------------------------- stage E
 *
 * The consolidation routine: the guest function a consolidating unwind runs
 * once the stack has been unwound, and whose RETURN VALUE is the resume
 * address.  In a real image this is __CxxCallCatchBlock and its body is the
 * catch block; here it is a checker, because the thing worth measuring is not
 * that a catch block can run but that the record the routine is handed arrived
 * whole.
 *
 * Everything it looks at is compile-time knowledge of this file: the code, the
 * flag the unwind is required to have added, the parameter COUNT, its own
 * address in slot 0, the establisher frame the handler recorded before it
 * asked for the unwind in slot 1, and nine distinct constants in the rest.
 * Written into statics and reported by the main flow rather than printed from
 * here, so the transcript stays ordered.
 *
 * It runs as x86-64 guest code in a nested emulator run, like every other
 * funclet this probe hands the port, and the witness counter says so.
 */
ULONG64 pf_consolidate_witness = 0x5e420000ull;  /* .data, seeded; only the routine writes it */

static volatile ULONG64 pf_cons_frame;   /* what the handler passed in slot 1 */

struct cons_seen
{
    int   calls;
    DWORD code;
    DWORD nparam;
    BOOL  code_is_consolidate;
    BOOL  unwinding_flag_set;
    BOOL  nparam_matches;
    BOOL  info0_is_the_routine;
    BOOL  info1_is_the_frame;
    BOOL  magics_intact;
    int   first_bad_magic;
};
static struct cons_seen pf_cons;

void * WINAPI pf_consolidate( EXCEPTION_RECORD *rec )
{
    UINT i;

    pf_consolidate_witness++;
    trace( "consolidate" );

    pf_cons.calls++;
    pf_cons.code   = rec->ExceptionCode;
    pf_cons.nparam = rec->NumberParameters;
    pf_cons.code_is_consolidate = rec->ExceptionCode == (DWORD)STATUS_UNWIND_CONSOLIDATE;
    /* The unwind ORs EXCEPTION_UNWINDING into the record before it runs the
     * handlers it crosses, and the routine is handed THAT record rather than
     * the one the caller built -- which is a claim about identity as much as
     * about a flag: a port that handed the routine some other record would not
     * have the flag in it. */
    pf_cons.unwinding_flag_set   = (rec->ExceptionFlags & EXCEPTION_UNWINDING) != 0;
    pf_cons.nparam_matches       = rec->NumberParameters == PF_CONS_NPARAM;
    pf_cons.info0_is_the_routine = rec->NumberParameters > 0 &&
        rec->ExceptionInformation[0] == (ULONG_PTR)pf_consolidate;
    pf_cons.info1_is_the_frame   = rec->NumberParameters > 1 &&
        rec->ExceptionInformation[1] == pf_cons_frame;
    pf_cons.magics_intact   = TRUE;
    pf_cons.first_bad_magic = -1;
    for (i = 2; i < PF_CONS_NPARAM; i++)
    {
        if (i < rec->NumberParameters &&
            rec->ExceptionInformation[i] == PF_CONS_MAGIC(i)) continue;
        pf_cons.magics_intact = FALSE;
        if (pf_cons.first_bad_magic < 0) pf_cons.first_bad_magic = (int)i;
    }

    /* NOT the TargetIp the unwind was handed.  See seh_handlers_asm.S. */
    return (void *)pf_consolidate_landing;
}

/* Named by seh_handlers_asm.S's .seh_handler directive, and compared against
 * itself below, so it needs to be visible before it is defined. */
EXCEPTION_DISPOSITION pf_language_handler( EXCEPTION_RECORD *rec, void *frame,
                                           CONTEXT *ctx, DISPATCHER_CONTEXT *dispatch );

static void pf_check_identity( void *frame, DISPATCHER_CONTEXT *dispatch )
{
    ULONG64 base  = (ULONG64)(ULONG_PTR)&__ImageBase;
    ULONG64 begin = (ULONG64)(ULONG_PTR)pf_call;
    ULONG64 end   = (ULONG64)(ULONG_PTR)pf_call_end;
    const SCOPE_TABLE *table = (const SCOPE_TABLE *)dispatch->HandlerData;

    pf_seen.imagebase_matches           = dispatch->ImageBase == base;
    pf_seen.functionentry_begin_matches = dispatch->FunctionEntry &&
        base + dispatch->FunctionEntry->BeginAddress == begin;
    pf_seen.functionentry_end_matches   = dispatch->FunctionEntry &&
        base + dispatch->FunctionEntry->EndAddress == end;
    pf_seen.controlpc_in_frame          = dispatch->ControlPc >= begin &&
                                          dispatch->ControlPc <  end;
    pf_seen.languagehandler_is_self     =
        (void *)dispatch->LanguageHandler == (void *)pf_language_handler;
    pf_seen.establisherframe_matches_arg = dispatch->EstablisherFrame == (ULONG64)(ULONG_PTR)frame;

    /* The scope table this probe emitted by hand, arriving back through the
     * port untouched.  Everything in it is an RVA, which is what makes the
     * check meaningful: a HandlerData pointer that had been mangled, or taken
     * from the wrong UNWIND_INFO, would not decode into these four values. */
    if (!table)
    {
        pf_seen.scope_count = 0xffffffff;
        return;
    }
    pf_seen.scope_count = table->Count;
    if (!table->Count) return;
    pf_seen.scope0_begin_matches = dispatch->ControlPc >=
                                   base + table->ScopeRecord[0].BeginAddress;
    pf_seen.scope0_end_matches   = dispatch->ControlPc <
                                   base + table->ScopeRecord[0].EndAddress;
    pf_seen.controlpc_in_scope   = pf_seen.scope0_begin_matches && pf_seen.scope0_end_matches;
    pf_seen.scope0_handler       = table->ScopeRecord[0].HandlerAddress;
    pf_seen.scope0_jumptarget_is_landing =
        base + table->ScopeRecord[0].JumpTarget == (ULONG64)(ULONG_PTR)pf_landing;
}

EXCEPTION_DISPOSITION pf_language_handler( EXCEPTION_RECORD *rec, void *frame,
                                           CONTEXT *ctx, DISPATCHER_CONTEXT *dispatch )
{
    pf_witness++;

    if (rec->ExceptionFlags & (EXCEPTION_UNWINDING | EXCEPTION_EXIT_UNWIND))
    {
        /* The unwind phase.  A private handler is entered here too -- the frame
         * asked for it with @unwind (UNW_FLAG_UHANDLER) -- and the two fields
         * that are phase-dependent are worth recording precisely because they
         * differ between the phases: during an unwind the DISPATCHER_CONTEXT's
         * ContextRecord describes THIS frame (its Rip is the ControlPc), while
         * during the search it describes the frame the walk has already stepped
         * to.  That is not a quirk of this port; Wine's own x86-64 dispatch
         * (dlls/ntdll/signal_x86_64.c, call_seh_handlers) has exactly the same
         * one-frame lag, and MSVC handlers never notice because the only thing
         * they do with the field is hand it to RtlUnwindEx, which overwrites it
         * with RtlCaptureContext. */
        pf_seen.unwind_calls++;
        pf_seen.unwinding_flag_in_unwind = TRUE;
        pf_seen.frame_matches_asm = dispatch->EstablisherFrame == pf_call_frame;
        pf_seen.ctxrecord_rip_is_controlpc_unwind =
            dispatch->ContextRecord && dispatch->ContextRecord->Rip == dispatch->ControlPc;
        pf_seen.ctxarg_is_ctxrecord_unwind = ctx == dispatch->ContextRecord;
        pf_seen.targetip_nonzero_in_unwind = dispatch->TargetIp != 0;

        if (pf_mode == PF_MODE_COLLIDE_DEFER || pf_mode == PF_MODE_COLLIDE_INPLACE)
        {
            /* Two frames of this image name this handler at once in stages G
             * and H -- A, the inner unwind's target, and (in stage G) K, the
             * frame that collides -- so which one this entry is FOR is the
             * first thing to decide, and it is decided by the establisher
             * frame rather than by a counter: a port that entered the wrong
             * frame's handler would otherwise look like a port that entered
             * the right one twice. */
            if (dispatch->EstablisherFrame == g_target_frame)
            {
                g_arrive_calls++;
                g_arrive_frame_is_target     = TRUE;
                g_arrive_code_is_inner       = rec->ExceptionCode == PF_CODE_G_INNER;
                g_arrive_info_is_inner       = rec->NumberParameters >= 1 &&
                    rec->ExceptionInformation[0] == PF_G_INNER_INFO;
                g_arrive_unwinding_flag      = (rec->ExceptionFlags & EXCEPTION_UNWINDING) != 0;
                g_arrive_target_flag         = (rec->ExceptionFlags & EXCEPTION_TARGET_UNWIND) != 0;
                g_arrive_targetip_is_landing = dispatch->TargetIp == (ULONG64)(ULONG_PTR)pf_landing;
                trace( "ph-target" );
                return ExceptionContinueSearch;
            }
            /* frame K, stage G: the collision, and then the re-entry */
            if (!g_collided)
            {
                EXCEPTION_RECORD inner;

                /* ONCE.  A handler that collides every time it is entered is a
                 * handler that cannot advance past its own collision, and the
                 * port refuses that by name rather than spinning -- correctly,
                 * and it is the collided-unwind CONTROL below that gates it.
                 * A real scope-table handler advances by ScopeIndex; this one
                 * has no scope table, so it advances by saying so. */
                g_collided = TRUE;
                g_collide_calls++;
                g_collide_frame = dispatch->EstablisherFrame;
                g_collide_scope = dispatch->ScopeIndex;
                trace( "kh-collide" );
                g_build_inner_record( &inner, rec->ExceptionAddress );
                pf_unwind( (void *)(ULONG_PTR)g_target_frame, (void *)pf_landing, &inner,
                           (void *)(ULONG_PTR)pf_retval, ctx, dispatch->HistoryTable );
                trace( "kh-collide-returned-BUG" );
                return ExceptionContinueSearch;
            }
            g_readopt_calls++;
            g_readopt_same_frame  = dispatch->EstablisherFrame == g_collide_frame;
            g_readopt_same_scope  = dispatch->ScopeIndex == g_collide_scope;
            g_readopt_collided_flag = (rec->ExceptionFlags & EXCEPTION_COLLIDED_UNWIND) != 0;
            g_readopt_no_target_flag = !(rec->ExceptionFlags & EXCEPTION_TARGET_UNWIND);
            g_readopt_targetip_is_landing = dispatch->TargetIp == (ULONG64)(ULONG_PTR)pf_landing;
            g_readopt_code_is_inner = rec->ExceptionCode == PF_CODE_G_INNER;
            trace( "kh-readopted" );
            return ExceptionContinueSearch;
        }

        if (rec->ExceptionFlags & EXCEPTION_TARGET_UNWIND)
        {
            pf_seen.target_calls++;
            pf_seen.target_unwind_flag_seen = TRUE;
            trace( "ph-target" );
        }
        else trace( "ph-unwind" );
        return ExceptionContinueSearch;
    }

    pf_seen.search_calls++;
    trace( "ph-search" );
    record( &pf_record, rec );
    pf_check_identity( frame, dispatch );
    pf_seen.scopeindex_search = dispatch->ScopeIndex;
    pf_seen.targetip_zero_in_search = dispatch->TargetIp == 0;
    pf_seen.ctxrecord_rip_is_controlpc_search =
        dispatch->ContextRecord && dispatch->ContextRecord->Rip == dispatch->ControlPc;
    pf_seen.ctxarg_is_ctxrecord_search = ctx == dispatch->ContextRecord;

    switch (pf_mode)
    {
    case PF_MODE_ACCEPT:
        /* Exactly what __C_specific_handler does once a filter has accepted:
         * unwind to this frame, resuming at the scope's JumpTarget with the
         * chosen ReturnValue in RAX.  It does not return on Windows and it does
         * not return here either -- the port records the request and ends this
         * handler's emulator run, because the frames between here and the
         * target are on the FAULTING stack and jumping would orphan the native
         * frames of the run this handler is using. */
        pf_unwind( (void *)(ULONG_PTR)dispatch->EstablisherFrame, (void *)pf_landing,
                   rec, (void *)(ULONG_PTR)pf_retval, ctx, dispatch->HistoryTable );
        /* not reached; if it ever is, the trace says so out loud */
        trace( "ph-unwind-returned-BUG" );
        return ExceptionContinueSearch;

    case PF_MODE_CONTINUE:
        return ExceptionContinueExecution;

    case PF_MODE_COLLIDE_DEFER:
    case PF_MODE_COLLIDE_INPLACE:
        /* Declining is the whole job in the search phase of stages G and H: the
         * frame that must accept is the clang __except further out, and this
         * frame's turn comes in the UNWIND phase, which is the only phase in
         * which a collision can happen at all. */
        g_search_k_calls++;
        trace( "kh-search" );
        return ExceptionContinueSearch;

    case PF_MODE_COLLIDE:
        /* Not a thing this handler could legitimately want.  It is here to
         * prove the REFUSAL: ExceptionCollidedUnwind is meaningful only during
         * an unwind that has already been re-entered, the port produces no such
         * state, and it must therefore refuse by name rather than guess. */
        return ExceptionCollidedUnwind;

    case PF_MODE_EXIT_UNWIND:
        /* An exit unwind names no frame to resume in.  Also a refusal probe. */
        pf_unwind( NULL, (void *)pf_landing, rec, 0, ctx, dispatch->HistoryTable );
        trace( "ph-exit-unwind-returned-BUG" );
        return ExceptionContinueSearch;

    case PF_MODE_CONSOLIDATE:
    case PF_MODE_CONS_NOROUTINE:
    {
        /* The consolidating unwind, which is how MSVC spells `catch`:
         * RtlUnwindEx with a record whose code is STATUS_UNWIND_CONSOLIDATE and
         * whose ExceptionInformation[0] is a CONSOLIDATION ROUTINE.  The unwind
         * itself is ordinary -- every __finally between the raise and the
         * target runs, which is where C++ destructors live -- but the resume is
         * not: the routine is called once the stack is unwound and the address
         * it RETURNS is where execution continues.
         *
         * The record is built to the WIDTH a real one has, eleven parameters,
         * because carrying slot 0 and dropping the rest is the failure this
         * stage is looking for.  Wine's own __CxxFrameHandler fills exactly
         * eleven (dlls/msvcrt/except.c, find_catch_block): [1] is the
         * establisher frame the catch funclet addresses its locals through, [2]
         * the function descriptor, [4] the untranslated record, [5] the catch
         * handler, [6] the original C++ record and [7] its CONTEXT.  A routine
         * handed only [0] would run a catch block against a frame it invented.
         * Here [1] is the real establisher frame and [2..10] are nine distinct
         * constants, so a dropped, shifted or truncated slot is a wrong value
         * at a named index.
         *
         * TargetIp is pf_landing and the routine returns pf_consolidate_landing:
         * two different addresses in the same frame, so which one runs says
         * whether the resume came from the routine or from the TargetIp a
         * consolidating unwind is required to ignore.
         *
         * PF_MODE_CONS_NOROUTINE is the control for the same code path: the
         * identical record with NumberParameters left at zero, i.e. an unwind
         * that asks for a callback-based resume and supplies no callback.  That
         * one must be refused by name -- there is no resume address to invent
         * and nothing this side could substitute. */
        /* Built field by field rather than as `= *rec`.  There is no C runtime
         * in this image, and clang lowers a struct assignment of this size to a
         * call to memcpy, which would not link -- measured, not guessed. */
        EXCEPTION_RECORD consolidate;
        UINT i;

        consolidate.ExceptionCode    = STATUS_UNWIND_CONSOLIDATE;
        /* EXCEPTION_NONCONTINUABLE, exactly as __CxxFrameHandler sets it: a
         * consolidating unwind is not a thing a handler may decline part way
         * through. */
        consolidate.ExceptionFlags   = EXCEPTION_NONCONTINUABLE;
        consolidate.ExceptionRecord  = NULL;
        consolidate.ExceptionAddress = rec->ExceptionAddress;
        consolidate.NumberParameters = 0;
        for (i = 0; i < EXCEPTION_MAXIMUM_PARAMETERS; i++)
            consolidate.ExceptionInformation[i] = 0;
        if (pf_mode == PF_MODE_CONSOLIDATE)
        {
            pf_cons_frame = dispatch->EstablisherFrame;
            consolidate.NumberParameters = PF_CONS_NPARAM;
            consolidate.ExceptionInformation[0] = (ULONG_PTR)pf_consolidate;
            consolidate.ExceptionInformation[1] = dispatch->EstablisherFrame;
            for (i = 2; i < PF_CONS_NPARAM; i++)
                consolidate.ExceptionInformation[i] = PF_CONS_MAGIC(i);
        }
        pf_unwind( (void *)(ULONG_PTR)dispatch->EstablisherFrame, (void *)pf_landing,
                   &consolidate, (void *)(ULONG_PTR)pf_retval, ctx, dispatch->HistoryTable );
        trace( "ph-consolidate-returned-BUG" );
        return ExceptionContinueSearch;
    }

    default:
        return ExceptionContinueSearch;
    }
}

/***********************************************************************
 *           pf_chain_handler
 *
 * The other shape a real image's .xdata names: a handler of the image's own
 * that does a little work and then CHAINS to ntdll's __C_specific_handler with
 * the same four arguments.  __GSHandlerCheck_SEH is the one every MSVC /GS
 * build carries; it checks the frame's stack cookie and then chains, with the
 * HandlerData pointer advanced past its own GS data.  Nothing is advanced here,
 * because the point is not to reproduce the cookie check but to reach the port
 * through the door the chain uses.
 *
 * That door is emu_C_specific_handler in dlls/ntdll/signal_ppc64.c -- reached
 * by a guest CALL to the ntdll export, not by the frame walk's identity check,
 * which never fires for this frame because the .xdata names pf_chain_handler.
 * The port used to refuse this arrival; refusing it means answering
 * ExceptionContinueSearch to a frame that is entitled to its __except, which is
 * a silent wrong answer rather than a loud one.  So the observable is not
 * "nothing crashed" but "the accepting scope unwound here, with the value
 * __C_specific_handler's own semantics choose": the exception code in RAX.
 */
ULONG64 pf_chain_witness = 0x5e410000ull;   /* .data; only this handler writes it */

EXCEPTION_DISPOSITION pf_chain_handler( EXCEPTION_RECORD *rec, void *frame,
                                        CONTEXT *ctx, DISPATCHER_CONTEXT *dispatch )
{
    pf_chain_witness++;

    if (rec->ExceptionFlags & (EXCEPTION_UNWINDING | EXCEPTION_EXIT_UNWIND))
        trace( (rec->ExceptionFlags & EXCEPTION_TARGET_UNWIND) ? "chain-target"
                                                               : "chain-unwind" );
    else trace( "chain-search" );

    /* The tail call.  __C_specific_handler here is the ntdll import -- layer 1
     * of the runner asserts that it is imported from ntdll.dll -- so this is a
     * guest CALL into the guest thunk, which is precisely the arrival the port
     * has to serve.  Its return value is this handler's return value, exactly
     * as in a chaining handler that has nothing to add. */
    return __C_specific_handler( rec, frame, ctx, dispatch );
}

static void pf_stage_reset( int mode, pf_unwind_fn fn, ULONG64 retval )
{
    UINT i;
    char *p = (char *)&pf_seen;

    for (i = 0; i < sizeof(pf_seen); i++) p[i] = 0;
    pf_record.calls = 0;
    pf_landing_rax  = 0;
    pf_landing_rbx  = 0;
    pf_landing_hit  = 0;
    pf_chain_rax    = 0;
    pf_chain_r12    = 0;
    pf_chain_hit    = 0;
    pf_consolidate_rax = 0;
    pf_consolidate_rbx = 0;
    pf_consolidate_hit = 0;
    for (i = 0; i < sizeof(pf_cons); i++) ((char *)&pf_cons)[i] = 0;
    pf_mode   = mode;
    pf_unwind = fn;
    pf_retval = retval;
    trace_reset();
}

/* ------------------------------------------------------------- stage A bodies */

static int * volatile null_ptr;

SEH_NOINLINE static void stage_a_raise( ULONG_PTR arg )
{
    ULONG_PTR args[2];

    (void)arg;
    args[0] = PF_A_ARG0;
    args[1] = PF_A_ARG1;
    trace( "araise" );
    RaiseException( PF_CODE_A, 0, 2, args );
    trace( "araise-returned-BUG" );
}

/* The fault used by the negative control: a genuine machine fault through a
 * volatile null, inside the private frame, so that the unhandled path is
 * reached from a frame whose language handler ran and declined. */
SEH_NOINLINE static void stage_a_fault( ULONG_PTR arg )
{
    (void)arg;
    trace( "afault" );
    *null_ptr = 0x5e;
    trace( "afault-returned-BUG" );
}

static struct seen seen_a_outer;

static LONG CALLBACK a_outer_filter( EXCEPTION_POINTERS *ptrs )
{
    trace( "afilt" );
    record( &seen_a_outer, ptrs->ExceptionRecord );
    return EXCEPTION_EXECUTE_HANDLER;
}

/* The private frame's handler declines, so this ordinary clang __try -- whose
 * .xdata names ntdll's __C_specific_handler -- is where the exception must end
 * up.  Declining is the disposition a real __GSHandlerCheck returns for every
 * exception it does not own, so it is the one that has to work first. */
SEH_NOINLINE static void stage_a( void )
{
    __try
    {
        PF_CALL( stage_a_raise, 0 );
    }
    __except( a_outer_filter( GetExceptionInformation() ) )
    {
        trace( "ahandler" );
    }
}

/* ------------------------------------------------------------- stage B bodies */

static ULONG_PTR thread_name_info[4];
static DWORD     thread_name_tid;

static void thread_name_init(void)
{
    thread_name_tid    = GetCurrentThreadId();
    thread_name_info[0] = 0x1000;                          /* dwType     */
    thread_name_info[1] = (ULONG_PTR)PF_THREAD_NAME;       /* szName     */
    thread_name_info[2] = thread_name_tid;                 /* dwThreadID */
    thread_name_info[3] = 0;                               /* dwFlags    */
}

SEH_NOINLINE static void stage_b_raise( ULONG_PTR arg )
{
    (void)arg;
    trace( "braise" );
    RaiseException( MS_VC_EXCEPTION, 0, 4, thread_name_info );
    /* Reached only when a filter answered EXCEPTION_CONTINUE_EXECUTION.  That
     * this marker appears at all is the proof that execution continued after
     * the raise, which is a different claim from "the filter returned -1". */
    trace( "braise-after" );
}

static struct seen seen_b_try;

static LONG CALLBACK b_filter_execute( EXCEPTION_POINTERS *ptrs )
{
    trace( "bfilt" );
    record( &seen_b_try, ptrs->ExceptionRecord );
    return EXCEPTION_EXECUTE_HANDLER;
}

static LONG CALLBACK b_filter_continue( EXCEPTION_POINTERS *ptrs )
{
    trace( "bfilt" );
    (void)ptrs;
    return EXCEPTION_CONTINUE_EXECUTION;
}

SEH_NOINLINE static void stage_b_try_execute( void )
{
    __try
    {
        SEH_CALL( stage_b_raise );
    }
    __except( b_filter_execute( GetExceptionInformation() ) )
    {
        trace( "bhandler" );
    }
}

SEH_NOINLINE static void stage_b_try_continue( void )
{
    __try
    {
        SEH_CALL( stage_b_raise );
    }
    __except( b_filter_continue( GetExceptionInformation() ) )
    {
        trace( "bhandler-BUG" );
    }
}

static BOOL thread_name_arrived( const struct seen *s )
{
    const char *want = PF_THREAD_NAME;
    const char *got  = (const char *)(ULONG_PTR)s->info[1];
    int i;

    if (s->code != MS_VC_EXCEPTION || s->nparam != 4) return FALSE;
    if (s->info[0] != 0x1000 || s->info[2] != thread_name_tid || s->info[3] != 0) return FALSE;
    if (!got) return FALSE;
    /* The name pointer is a guest address handed across the boundary inside an
     * ExceptionInformation word.  Following it and comparing the bytes is the
     * only check that says the word survived as a POINTER rather than as a
     * number that happens to look like one. */
    for (i = 0; want[i]; i++) if (got[i] != want[i]) return FALSE;
    return got[i] == 0;
}

/* ------------------------------------------------------------- stage C bodies
 *
 * raise (no handler)  ->  mid (__finally, must run once, abnormally)
 *                     ->  pf_call (the private handler; unwinds to pf_landing)
 */

static int  c_finally_calls;
static int  c_finally_abnormal;
static int  c_finally_agreed;
static BOOL c_try_completed;

SEH_NOINLINE static void stage_c_raise( ULONG_PTR arg )
{
    (void)arg;
    trace( "craise" );
    RaiseException( PF_CODE_C, 0, 0, NULL );
    trace( "craise-returned-BUG" );
}

/* "Abnormal" is measured TWICE here and the two are required to agree, because
 * they are different claims.
 *
 *   c_try_completed is a fact about this program's control flow: the guarded
 *   body sets it on its way out, so a __finally that sees it clear was reached
 *   by an unwind and not by falling through.
 *
 *   AbnormalTermination() is _abnormal_termination(), which reads the FIRST
 *   ARGUMENT the funclet was called with -- the x64 termination-handler
 *   contract is handler( BOOLEAN abnormal, void *EstablisherFrame ), and this
 *   port passes that argument from call_guest_termination_handler() in
 *   dlls/ntdll/signal_ppc64.c.  Nothing else in this probe looks at it, and a
 *   port that passed it wrongly would run every __finally with the wrong idea
 *   of why it is running -- which is exactly the sort of thing that produces a
 *   resource leak nobody can trace back to SEH.
 */
/* What the guarded body of the intermediate frame calls.  A variable rather
 * than a fixed name so that the SAME frame -- the same __finally, the same
 * measured "did it run once, abnormally, in the right place" -- can sit between
 * the private handler and a raise (stages C and E) or between it and a guest
 * that calls RtlUnwindEx itself (stage F).  Sharing the frame is the point: it
 * is what makes the three orderings comparable. */
static pf_body volatile c_mid_body = stage_c_raise;

SEH_NOINLINE static void stage_c_mid( ULONG_PTR arg )
{
    (void)arg;
    c_try_completed = FALSE;
    __try
    {
        SEH_CALL( c_mid_body );
        c_try_completed = TRUE;
    }
    __finally
    {
        BOOL by_arg  = AbnormalTermination() != 0;
        BOOL by_flow = !c_try_completed;

        c_finally_calls++;
        if (by_arg) c_finally_abnormal++;
        if (by_arg == by_flow) c_finally_agreed++;
        trace( by_flow ? "midfin-abnormal" : "midfin-normal" );
    }
    trace( "cmid-returned-BUG" );
}

/* ------------------------------------------------------------- stage F body
 *
 * The OTHER road into the same code, and the reason it needs its own stage:
 * this port decides who owns an unwind by where the request came from.  A
 * guest language handler that calls RtlUnwindEx is unwinding a stack whose
 * frames are interleaved with the emulator run's own native frames, so the
 * request is recorded and the frame walk performs it (the DEFERRED path, which
 * is what stages B, C and E exercise).  Guest code that calls RtlUnwindEx while
 * NOT inside such a handler is unwinding within its own run, and the port
 * serves it IN PLACE, from the trap context, writing the result straight back.
 * The consolidating unwind has to work on both, and it does because both end in
 * the same function -- but "the same function" is an argument, and this is a
 * measurement.
 *
 * Calling RtlUnwindEx from ordinary guest code means naming the target frame
 * without a DISPATCHER_CONTEXT to read it from, which is what pf_call_frame is
 * for.
 */
SEH_NOINLINE static void stage_f_unwind( ULONG_PTR arg )
{
    /* Handed to RtlUnwindEx because the API takes one.  Deliberately not
     * initialised: x86-64 RtlUnwindEx opens with RtlCaptureContext( context ),
     * i.e. it overwrites whatever is here, and this port reads the guest state
     * the trap fired with instead -- so a memset would be describing a
     * contract that does not exist (and there is no CRT here to do it with). */
    CONTEXT unwind_ctx;
    EXCEPTION_RECORD consolidate;
    UINT i;

    (void)arg;
    trace( "fcall" );

    pf_cons_frame = pf_call_frame;
    consolidate.ExceptionCode    = STATUS_UNWIND_CONSOLIDATE;
    consolidate.ExceptionFlags   = EXCEPTION_NONCONTINUABLE;
    consolidate.ExceptionRecord  = NULL;
    consolidate.ExceptionAddress = (void *)stage_f_unwind;
    consolidate.NumberParameters = PF_CONS_NPARAM;
    for (i = 0; i < EXCEPTION_MAXIMUM_PARAMETERS; i++)
        consolidate.ExceptionInformation[i] = 0;
    consolidate.ExceptionInformation[0] = (ULONG_PTR)pf_consolidate;
    consolidate.ExceptionInformation[1] = pf_call_frame;
    for (i = 2; i < PF_CONS_NPARAM; i++)
        consolidate.ExceptionInformation[i] = PF_CONS_MAGIC(i);

    RtlUnwindEx( (void *)(ULONG_PTR)pf_call_frame, (void *)pf_landing, &consolidate,
                 (void *)(ULONG_PTR)PF_RETVAL_F, &unwind_ctx, NULL );
    trace( "funwind-returned-BUG" );
}

/* ------------------------------------------------------------- stages G and H bodies
 *
 * The four frames of the collided unwind, innermost first here and outermost
 * first in the comment on the state above.  Each one is an ordinary clang
 * function except where it has to be a pf_call, and every guarded body goes
 * through the volatile hook for the reason the whole file does: clang drops the
 * language handler of a __try whose body it believes cannot unwind.
 */
SEH_NOINLINE static void g_raise( ULONG_PTR arg )
{
    (void)arg;
    trace( "graise" );
    RaiseException( PF_CODE_G, 0, 0, NULL );
    trace( "graise-returned-BUG" );
}

/* K, stage G: a second pf_call, so that the frame the outer unwind is running
 * when the collision happens is one whose language handler is the PRIVATE one
 * -- a guest handler entered as guest code, unwinding from inside its own
 * emulator run.  That is the port's deferred road. */
SEH_NOINLINE static void g_collide_handler_frame( ULONG_PTR arg )
{
    (void)arg;
    pf_call( g_raise, 0 );
    trace( "gk-returned-BUG" );
}

/* K, stage H: an ordinary clang __try/__finally.  Its handler is ntdll's
 * __C_specific_handler, which this port serves natively and which runs this
 * __finally as a funclet in a nested emulator run; the RtlUnwindEx below
 * therefore arrives with no language handler of ours in the picture at all,
 * which is the port's in-place road and the one that had to learn to tell a
 * collision from a local unwind. */
SEH_NOINLINE static void g_collide_finally_frame( ULONG_PTR arg )
{
    (void)arg;
    __try
    {
        SEH_CALL( g_raise );
        trace( "gk-try-completed-BUG" );
    }
    __finally
    {
        g_k_finally_calls++;
        if (AbnormalTermination()) g_k_finally_abnormal++;
        trace( "gkfin" );
        if (!g_collided)
        {
            /* Deliberately not initialised; see stage_f_unwind for why a
             * memset here would be describing a contract that does not exist. */
            CONTEXT unwind_ctx;
            EXCEPTION_RECORD inner;

            g_collided = TRUE;
            g_collide_calls++;
            g_build_inner_record( &inner, (void *)g_collide_finally_frame );
            RtlUnwindEx( (void *)(ULONG_PTR)g_target_frame, (void *)pf_landing, &inner,
                         (void *)(ULONG_PTR)PF_RETVAL_H, &unwind_ctx, NULL );
            trace( "gkfin-returned-BUG" );
        }
    }
    trace( "gk-returned-BUG" );
}

/* X: the frame the OUTER unwind is heading for.  Its filter is what starts that
 * unwind; its __except body is what the collision must cost it. */
static pf_body volatile g_inner_body;

static LONG CALLBACK g_outer_filter( EXCEPTION_POINTERS *ptrs )
{
    (void)ptrs;
    g_outer_filter_calls++;
    trace( "gfilt" );
    return EXCEPTION_EXECUTE_HANDLER;
}

SEH_NOINLINE static void g_outer_target_frame( ULONG_PTR arg )
{
    (void)arg;
    __try
    {
        SEH_CALL( g_inner_body );
        trace( "gx-try-completed-BUG" );
    }
    __except( g_outer_filter( GetExceptionInformation() ) )
    {
        /* The outer unwind was on its way here when it was collided with.  If
         * this ever runs, the inner unwind lost. */
        g_outer_except_calls++;
        trace( "gexcept-BUG" );
    }
}

/* M: the __finally between the collision and the INNER unwind's target.  It is
 * outside the outer unwind entirely -- that one stops at X, one frame in -- so
 * it runs if and only if the collided unwind carried on past X. */
SEH_NOINLINE static void g_mid_frame( ULONG_PTR arg )
{
    (void)arg;
    __try
    {
        SEH_CALL( g_outer_target_frame );
        trace( "gm-try-completed-BUG" );
    }
    __finally
    {
        g_mid_finally_calls++;
        if (AbnormalTermination()) g_mid_finally_abnormal++;
        trace( "gmidfin" );
    }
    trace( "gm-returned-BUG" );
}

/* A's body: the first thing it does is publish A's establisher frame, because
 * the frame K of stage G is a pf_call too and overwrites pf_call_frame with its
 * own.  Everything downstream names the INNER unwind's target by this value. */
SEH_NOINLINE static void g_body( ULONG_PTR arg )
{
    (void)arg;
    g_target_frame = pf_call_frame;
    SEH_CALL( g_mid_frame );
    trace( "gbody-returned-BUG" );
}

/* ------------------------------------------------------------- stage D body */

SEH_NOINLINE static void stage_d_raise( ULONG_PTR arg )
{
    (void)arg;
    trace( "draise" );
    RaiseException( PF_CODE_D, 0, 0, NULL );
    trace( "draise-returned-BUG" );
}

/* ------------------------------------------------------------- the run */

static void report_stage_a( void )
{
    begin( "private handler: it ran, as x86-64 guest code, in both phases" );
    out( "search_calls=" );
    out_dec( (ULONG)pf_seen.search_calls );
    out( " unwind_calls=" );
    out_dec( (ULONG)pf_seen.unwind_calls );
    out( " witness_delta=" );
    out_dec( (ULONG)(pf_witness - 0x5e400000ull) );
    verdict( pf_seen.search_calls == 1 && pf_seen.unwind_calls == 1 &&
             pf_witness == 0x5e400000ull + 2,
             "the private handler was not entered exactly once per phase" );

    begin( "private handler: DISPATCHER_CONTEXT names this frame" );
    out_yn( "imagebase_matches", pf_seen.imagebase_matches );
    out_yn( " functionentry_begin_matches", pf_seen.functionentry_begin_matches );
    out_yn( " functionentry_end_matches", pf_seen.functionentry_end_matches );
    out_yn( " controlpc_in_frame", pf_seen.controlpc_in_frame );
    out_yn( " languagehandler_is_self", pf_seen.languagehandler_is_self );
    out_yn( " establisherframe_matches_arg", pf_seen.establisherframe_matches_arg );
    verdict( pf_seen.imagebase_matches && pf_seen.functionentry_begin_matches &&
             pf_seen.functionentry_end_matches && pf_seen.controlpc_in_frame &&
             pf_seen.languagehandler_is_self && pf_seen.establisherframe_matches_arg,
             "the DISPATCHER_CONTEXT does not describe the frame that was interrupted" );

    begin( "private handler: HandlerData is this frame's own scope table" );
    out( "handlerdata_scope_count=" );
    out_dec( pf_seen.scope_count );
    out_yn( " controlpc_in_scope", pf_seen.controlpc_in_scope );
    out( " scope0_handler=" );
    out_dec( pf_seen.scope0_handler );
    out_yn( " scope0_jumptarget_is_landing", pf_seen.scope0_jumptarget_is_landing );
    verdict( pf_seen.scope_count == 1 && pf_seen.controlpc_in_scope &&
             pf_seen.scope0_handler == EXCEPTION_EXECUTE_HANDLER &&
             pf_seen.scope0_jumptarget_is_landing,
             "the scope table emitted by seh_handlers_asm.S did not arrive intact" );

    /* Every one of these is printed for BOTH phases where it has two values,
     * including the two that come out "no", because a measured "no" that is
     * written down is a fact the next reader can act on and a measured "no"
     * that is only mentioned in a comment is a rumour.  The two that are
     * expected to be "no" in the SEARCH phase are the same relation seen from
     * two angles: during the search, DISPATCHER_CONTEXT->ContextRecord
     * describes the frame the walk has already stepped to -- one further out
     * than the one whose handler is running -- and the handler's third argument
     * is the ORIGINAL exception context, so the two are neither equal to each
     * other nor equal to this frame's ControlPc.  That is not this port's
     * invention: Wine's own x86-64 dispatch has exactly the same one-frame lag
     * (dlls/ntdll/signal_x86_64.c, call_seh_handlers: dispatch.ContextRecord =
     * &context, handler called with orig_context), and no MSVC handler notices,
     * because the only thing one ever does with the field is hand it to
     * RtlUnwindEx, which overwrites it with RtlCaptureContext.  During the
     * UNWIND phase both become yes, which is the state the contract does
     * depend on.  If the port ever moves to the other convention this step goes
     * red and this comment is where the reader lands. */
    begin( "private handler: the phase-dependent fields differ by phase" );
    out( "scopeindex=" );
    out_dec( pf_seen.scopeindex_search );
    out_yn( " targetip_zero_in_search", pf_seen.targetip_zero_in_search );
    out_yn( " targetip_nonzero_in_unwind", pf_seen.targetip_nonzero_in_unwind );
    out_yn( " unwinding_flag_in_unwind", pf_seen.unwinding_flag_in_unwind );
    out_yn( " target_unwind_flag_in_unwind", pf_seen.target_unwind_flag_seen );
    out_yn( " contextrecord_rip_is_controlpc_in_search",
            pf_seen.ctxrecord_rip_is_controlpc_search );
    out_yn( " contextrecord_rip_is_controlpc_in_unwind",
            pf_seen.ctxrecord_rip_is_controlpc_unwind );
    out_yn( " ctxarg_is_contextrecord_in_search", pf_seen.ctxarg_is_ctxrecord_search );
    out_yn( " ctxarg_is_contextrecord_in_unwind", pf_seen.ctxarg_is_ctxrecord_unwind );
    verdict( pf_seen.scopeindex_search == 0 && pf_seen.targetip_zero_in_search &&
             pf_seen.targetip_nonzero_in_unwind &&
             pf_seen.unwinding_flag_in_unwind &&
             /* stage A unwinds to the ENCLOSING clang frame, so the private
              * frame is crossed by the unwind and is not its target */
             !pf_seen.target_unwind_flag_seen &&
             !pf_seen.ctxrecord_rip_is_controlpc_search &&
             pf_seen.ctxrecord_rip_is_controlpc_unwind &&
             !pf_seen.ctxarg_is_ctxrecord_search &&
             pf_seen.ctxarg_is_ctxrecord_unwind,
             "a DISPATCHER_CONTEXT field did not carry its phase's value" );

    begin( "private handler: the record it was handed" );
    out( "code=0x" );
    out_hex( pf_record.code, 8 );
    out( " nparam=" );
    out_dec( pf_record.nparam );
    out( " info0=0x" );
    out_hex( pf_record.info[0], 8 );
    out( " info1=0x" );
    out_hex( pf_record.info[1], 8 );
    verdict( pf_record.calls == 1 && pf_record.code == PF_CODE_A && pf_record.nparam == 2 &&
             pf_record.info[0] == PF_A_ARG0 && pf_record.info[1] == PF_A_ARG1,
             "the private code or its parameters did not arrive" );

    begin( "private handler: declining hands the exception to the enclosing __try" );
    out_trace();
    verdict( trace_is( "araise ph-search afilt ph-unwind ahandler" ) &&
             seen_a_outer.calls == 1 && seen_a_outer.code == PF_CODE_A,
             "ExceptionContinueSearch did not continue the search in order" );
}

static void report_stage_b( void )
{
    BOOL code, flags, nparam, info, addr;
    struct seen b_private;

    /* ---- under the private handler, filter says EXECUTE_HANDLER ---------- */
    pf_stage_reset( PF_MODE_ACCEPT, RtlUnwindEx, PF_RETVAL_B );
    PF_CALL( stage_b_raise, 0 );
    trace( pf_landing_hit ? "landed" : "returned" );
    b_private = pf_record;

    begin( "thread name, private handler: the idiom's record arrived whole" );
    out( "code=0x" );
    out_hex( b_private.code, 8 );
    out( " nparam=" );
    out_dec( b_private.nparam );
    out( " info0=0x" );
    out_hex( b_private.info[0], 4 );
    out_yn( " name_matches", thread_name_arrived( &b_private ) );
    out_yn( " tid_matches", b_private.info[2] == thread_name_tid );
    out( " info3=0x" );
    out_hex( b_private.info[3], 1 );
    verdict( thread_name_arrived( &b_private ), "RaiseException(0x406d1388, 0, 4, info) did "
             "not arrive at the private handler intact" );

    begin( "thread name, private handler: accepting unwinds into the frame" );
    out( "landed=" );
    out_dec( (ULONG)pf_landing_hit );
    out_yn( " rax_is_returnvalue", pf_landing_rax == PF_RETVAL_B );
    out_yn( " rbx_is_frame_sentinel", pf_landing_rbx == PF_RBX_SENTINEL );
    out( " " );
    out_trace();
    verdict( pf_landing_hit == 1 && pf_landing_rax == PF_RETVAL_B &&
             pf_landing_rbx == PF_RBX_SENTINEL &&
             trace_is( "braise ph-search ph-target landed" ),
             "the unwind did not resume in the private frame with the right registers" );

    /* ---- under the private handler, filter says CONTINUE_EXECUTION ------- */
    pf_stage_reset( PF_MODE_CONTINUE, RtlUnwindEx, 0 );
    PF_CALL( stage_b_raise, 0 );
    trace( pf_landing_hit ? "landed" : "returned" );

    begin( "thread name, private handler: continuing resumes after the raise" );
    out( "landed=" );
    out_dec( (ULONG)pf_landing_hit );
    out( " " );
    out_trace();
    verdict( pf_landing_hit == 0 && trace_is( "braise ph-search braise-after returned" ),
             "ExceptionContinueExecution did not resume after RaiseException" );

    /* ---- the same two under an ordinary clang __try ---------------------- */
    trace_reset();
    stage_b_try_execute();

    begin( "thread name, clang __try: accepting runs the __except body" );
    out_trace();
    verdict( trace_is( "braise bfilt bhandler" ) && seen_b_try.calls == 1,
             "EXCEPTION_EXECUTE_HANDLER did not reach the __except body" );

    trace_reset();
    stage_b_try_continue();
    trace( "returned" );

    begin( "thread name, clang __try: continuing resumes after the raise" );
    out_trace();
    verdict( trace_is( "braise bfilt braise-after returned" ),
             "EXCEPTION_CONTINUE_EXECUTION did not resume after RaiseException" );

    /* ---- and the two paths must agree ----------------------------------- */
    begin( "thread name: the private handler and __C_specific_handler agree" );
    seen_same( &b_private, &seen_b_try, &code, &flags, &nparam, &info, &addr );
    out_yn( "code", code );
    out_yn( " flags", flags );
    out_yn( " nparam", nparam );
    out_yn( " info", info );
    out_yn( " address", addr );
    verdict( code && flags && nparam && info && addr,
             "the two handler paths were handed different records" );
}

static void report_stage_c( const char *what, pf_unwind_fn fn, ULONG64 retval )
{
    ULONG64 ret;

    c_finally_calls = c_finally_abnormal = c_finally_agreed = 0;
    pf_stage_reset( PF_MODE_ACCEPT, fn, retval );
    ret = PF_CALL( stage_c_mid, 0 );
    /* The marker is the OBSERVED way out, not an assumed one: "landed" means
     * the landing pad ran, "returned" means pf_call came back through its own
     * epilogue.  A trace that said "landed" either way would turn the whole
     * ordering step into a tautology. */
    trace( pf_landing_hit ? "landed" : "returned" );

    begin( what );
    out( "order: " );
    out_trace();
    verdict( trace_is( "craise ph-search midfin-abnormal ph-target landed" ),
             "the unwind did not run the intermediate __finally between the "
             "handler and the landing pad" );

    begin( what );
    out( "the intermediate __finally: calls=" );
    out_dec( (ULONG)c_finally_calls );
    out( " abnormal=" );
    out_dec( (ULONG)c_finally_abnormal );
    out_yn( " abnormal_arg_agrees", c_finally_agreed == c_finally_calls );
    verdict( c_finally_calls == 1 && c_finally_abnormal == 1 &&
             c_finally_agreed == c_finally_calls,
             "the __finally between the raise and the target did not run exactly "
             "once, abnormally, with the funclet's own abnormal argument agreeing" );

    begin( what );
    out( "arrival: landed=" );
    out_dec( (ULONG)pf_landing_hit );
    out_yn( " rax_is_returnvalue", pf_landing_rax == retval );
    out_yn( " rbx_is_frame_sentinel", pf_landing_rbx == PF_RBX_SENTINEL );
    out_yn( " pf_call_returned_rax", ret == retval );
    verdict( pf_landing_hit == 1 && pf_landing_rax == retval &&
             pf_landing_rbx == PF_RBX_SENTINEL && ret == retval,
             "the frame did not resume at the JumpTarget with the ReturnValue in RAX" );
}

static void report_stage_d( void )
{
    ULONG64 ret;

    /* pf_mode is irrelevant here: this frame's handler is pf_chain_handler, and
     * it has no modes -- it chains, unconditionally, the way a real one does. */
    pf_stage_reset( PF_MODE_REPORT, RtlUnwindEx, 0 );
    ret = pf_chain_call( stage_d_raise, 0 );
    trace( pf_chain_hit ? "landed" : "returned" );

    begin( "chained handler: a private handler that tail-calls __C_specific_handler" );
    out( "chain_calls=" );
    out_dec( (ULONG)(pf_chain_witness - 0x5e410000ull) );
    out( " " );
    out_trace();
    verdict( pf_chain_witness == 0x5e410000ull + 2 &&
             trace_is( "draise chain-search chain-target landed" ),
             "the chaining handler did not run in both phases, in order" );

    begin( "chained handler: __C_specific_handler served the guest's own call" );
    out( "landed=" );
    out_dec( (ULONG)pf_chain_hit );
    out_yn( " rax_is_exception_code", pf_chain_rax == PF_CODE_D );
    out_yn( " r12_is_frame_sentinel", pf_chain_r12 == PF_R12_SENTINEL );
    out_yn( " pf_chain_call_returned_rax", ret == pf_chain_rax );
    verdict( pf_chain_hit == 1 && pf_chain_rax == PF_CODE_D &&
             pf_chain_r12 == PF_R12_SENTINEL && ret == pf_chain_rax,
             "the accepting scope did not unwind to the JumpTarget with the "
             "exception code in RAX -- the guest's direct call to "
             "__C_specific_handler was not served" );
}

/* ------------------------------------------------------------- stage E
 *
 * The consolidating unwind, end to end, over the SAME frame layout stage C
 * uses -- raise, an intermediate __finally, the private handler -- so that the
 * only difference between the two stages is the protocol the handler asks for.
 * That is deliberate: the ordering claim ("every __finally between the raise
 * and the target runs BEFORE the routine does") is exactly the C++ claim that
 * destructors run before the catch block, and it is only worth anything if the
 * same instrument measured the same thing one stage earlier without the
 * consolidation in the picture.
 */
static void report_stage_e( void )
{
    ULONG64 ret;

    c_finally_calls = c_finally_abnormal = c_finally_agreed = 0;
    pf_stage_reset( PF_MODE_CONSOLIDATE, RtlUnwindEx, PF_RETVAL_E );
    ret = PF_CALL( stage_c_mid, 0 );
    /* Three outcomes, told apart rather than assumed: the consolidating pad,
     * the plain pad (which would mean the port resumed at the TargetIp a
     * consolidating unwind must ignore), or no unwind at all. */
    trace( pf_consolidate_hit ? "landed-consolidate"
                              : pf_landing_hit ? "landed-plain-BUG" : "returned" );

    begin( "consolidating unwind: the __finally runs before the routine does" );
    out( "order: " );
    out_trace();
    out( " finally_calls=" );
    out_dec( (ULONG)c_finally_calls );
    out( " abnormal=" );
    out_dec( (ULONG)c_finally_abnormal );
    verdict( trace_is( "craise ph-search midfin-abnormal ph-target consolidate "
                       "landed-consolidate" ) &&
             c_finally_calls == 1 && c_finally_abnormal == 1 &&
             c_finally_agreed == c_finally_calls,
             "the consolidating unwind did not run the intermediate __finally, once and "
             "abnormally, before the consolidation routine" );

    begin( "consolidating unwind: the record the routine was handed" );
    out( "calls=" );
    out_dec( (ULONG)pf_cons.calls );
    out( " code=0x" );
    out_hex( pf_cons.code, 8 );
    out( " nparam=" );
    out_dec( pf_cons.nparam );
    out_yn( " unwinding_flag", pf_cons.unwinding_flag_set );
    out_yn( " info0_is_the_routine", pf_cons.info0_is_the_routine );
    out_yn( " info1_is_the_frame", pf_cons.info1_is_the_frame );
    out_yn( " info2_to_10_intact", pf_cons.magics_intact );
    verdict( pf_cons.calls == 1 && pf_cons.code_is_consolidate && pf_cons.nparam_matches &&
             pf_cons.unwinding_flag_set && pf_cons.info0_is_the_routine &&
             pf_cons.info1_is_the_frame && pf_cons.magics_intact,
             "the routine was not handed the record the handler built, whole" );

    begin( "consolidating unwind: it resumed where the ROUTINE said" );
    out( "consolidate_landed=" );
    out_dec( (ULONG)pf_consolidate_hit );
    out( " plain_landed=" );
    out_dec( (ULONG)pf_landing_hit );
    out_yn( " rax_is_returnvalue", pf_consolidate_rax == PF_RETVAL_E );
    out_yn( " rbx_is_frame_sentinel", pf_consolidate_rbx == PF_RBX_SENTINEL );
    out_yn( " pf_call_returned_rax", ret == PF_RETVAL_E );
    out( " routine_calls=" );
    out_dec( (ULONG)(pf_consolidate_witness - 0x5e420000ull) );
    verdict( pf_consolidate_hit == 1 && pf_landing_hit == 0 &&
             pf_consolidate_rax == PF_RETVAL_E &&
             pf_consolidate_rbx == PF_RBX_SENTINEL && ret == PF_RETVAL_E &&
             pf_consolidate_witness == 0x5e420000ull + 1,
             "the guest did not resume at the address the consolidation routine returned, "
             "in the target frame, with the unwind's ReturnValue in RAX" );
}

/* ------------------------------------------------------------- stage F
 *
 * The same protocol down the port's OTHER road: guest code calls RtlUnwindEx
 * itself rather than a language handler doing it, so the port serves the unwind
 * IN PLACE instead of deferring it to the frame walk.  See stage_f_unwind.
 */
static void report_stage_f( void )
{
    ULONG64 ret;

    c_finally_calls = c_finally_abnormal = c_finally_agreed = 0;
    pf_stage_reset( PF_MODE_REPORT, RtlUnwindEx, PF_RETVAL_F );
    c_mid_body = stage_f_unwind;
    ret = PF_CALL( stage_c_mid, 0 );
    c_mid_body = stage_c_raise;
    trace( pf_consolidate_hit ? "landed-consolidate"
                              : pf_landing_hit ? "landed-plain-BUG" : "returned" );

    begin( "consolidating unwind in place: a guest calling RtlUnwindEx itself" );
    out( "order: " );
    out_trace();
    out( " finally_calls=" );
    out_dec( (ULONG)c_finally_calls );
    out( " abnormal=" );
    out_dec( (ULONG)c_finally_abnormal );
    out_yn( " establisherframe_is_frames_own_rsp", pf_seen.frame_matches_asm );
    verdict( trace_is( "fcall midfin-abnormal ph-target consolidate landed-consolidate" ) &&
             c_finally_calls == 1 && c_finally_abnormal == 1 &&
             pf_seen.frame_matches_asm,
             "the in-place unwind did not cross the intermediate __finally and reach the "
             "target frame the probe named" );

    begin( "consolidating unwind in place: the record and the arrival" );
    out( "routine_calls=" );
    out_dec( (ULONG)pf_cons.calls );
    out( " nparam=" );
    out_dec( pf_cons.nparam );
    out_yn( " unwinding_flag", pf_cons.unwinding_flag_set );
    out_yn( " info0_is_the_routine", pf_cons.info0_is_the_routine );
    out_yn( " info1_is_the_frame", pf_cons.info1_is_the_frame );
    out_yn( " info2_to_10_intact", pf_cons.magics_intact );
    out( " consolidate_landed=" );
    out_dec( (ULONG)pf_consolidate_hit );
    out_yn( " rax_is_returnvalue", pf_consolidate_rax == PF_RETVAL_F );
    out_yn( " pf_call_returned_rax", ret == PF_RETVAL_F );
    verdict( pf_cons.calls == 1 && pf_cons.code_is_consolidate && pf_cons.nparam_matches &&
             pf_cons.unwinding_flag_set && pf_cons.info0_is_the_routine &&
             pf_cons.info1_is_the_frame && pf_cons.magics_intact &&
             pf_consolidate_hit == 1 && pf_landing_hit == 0 &&
             pf_consolidate_rax == PF_RETVAL_F &&
             pf_consolidate_rbx == PF_RBX_SENTINEL && ret == PF_RETVAL_F,
             "the in-place consolidating unwind did not hand the routine the whole record "
             "and resume where it said" );
}

/* ------------------------------------------------------------- stages G and H
 *
 * The collided unwind, once per road.  See the block comment on the state above
 * for the frame layout and for what each counter would catch.
 */
static void report_stage_g( void )
{
    ULONG64 ret;

    g_stage_reset();
    pf_stage_reset( PF_MODE_COLLIDE_DEFER, RtlUnwindEx, PF_RETVAL_G );
    g_inner_body = g_collide_handler_frame;
    ret = PF_CALL( g_body, 0 );
    trace( pf_landing_hit ? "landed" : "returned" );

    begin( "collided unwind, deferred road: a guest handler unwinds from inside the unwind" );
    out( "order: " );
    out_trace();
    out( " collide_calls=" );
    out_dec( (ULONG)g_collide_calls );
    out( " outer_filter_calls=" );
    out_dec( (ULONG)g_outer_filter_calls );
    out( " outer_except_calls=" );
    out_dec( (ULONG)g_outer_except_calls );
    /* "ph-search kh-search" is one entry into the private handler, not two: the
     * search phase marks every entry with its own token before the mode's, and
     * kh-search is what says that entry was frame K's. */
    verdict( trace_is( "graise ph-search kh-search gfilt kh-collide kh-readopted "
                       "gmidfin ph-target landed" ) &&
             g_collide_calls == 1 && g_outer_filter_calls == 1 && g_outer_except_calls == 0,
             "the collision did not preempt the unwind it collided with, or the frame the "
             "outer unwind was heading for ran its __except anyway" );

    begin( "collided unwind, deferred road: the colliding frame is re-entered where it was left" );
    out( "readopt_calls=" );
    out_dec( (ULONG)g_readopt_calls );
    out_yn( " same_frame", g_readopt_same_frame );
    out_yn( " same_scopeindex", g_readopt_same_scope );
    out_yn( " collided_flag", g_readopt_collided_flag );
    out_yn( " target_unwind_flag_absent", g_readopt_no_target_flag );
    out_yn( " targetip_is_the_new_target", g_readopt_targetip_is_landing );
    out_yn( " code_is_the_inner_record", g_readopt_code_is_inner );
    verdict( g_readopt_calls == 1 && g_readopt_same_frame && g_readopt_same_scope &&
             g_readopt_collided_flag && g_readopt_no_target_flag &&
             g_readopt_targetip_is_landing && g_readopt_code_is_inner,
             "the adopted unwind did not resume at the collision point with the inner "
             "unwind's record and the outer unwind's scope index" );

    begin( "collided unwind, deferred road: the inner unwind's target is reached" );
    out( "mid_finally=" );
    out_dec( (ULONG)g_mid_finally_calls );
    out( " abnormal=" );
    out_dec( (ULONG)g_mid_finally_abnormal );
    out( " arrive_calls=" );
    out_dec( (ULONG)g_arrive_calls );
    out_yn( " code_is_inner", g_arrive_code_is_inner );
    out_yn( " info_is_inner", g_arrive_info_is_inner );
    out_yn( " target_unwind_flag", g_arrive_target_flag );
    out( " landed=" );
    out_dec( (ULONG)pf_landing_hit );
    out_yn( " rax_is_returnvalue", pf_landing_rax == PF_RETVAL_G );
    out_yn( " rbx_is_frame_sentinel", pf_landing_rbx == PF_RBX_SENTINEL );
    out_yn( " pf_call_returned_rax", ret == PF_RETVAL_G );
    verdict( g_mid_finally_calls == 1 && g_mid_finally_abnormal == 1 && g_arrive_calls == 1 &&
             g_arrive_code_is_inner && g_arrive_info_is_inner && g_arrive_target_flag &&
             g_arrive_unwinding_flag && g_arrive_targetip_is_landing &&
             pf_landing_hit == 1 && pf_landing_rax == PF_RETVAL_G &&
             pf_landing_rbx == PF_RBX_SENTINEL && ret == PF_RETVAL_G,
             "the collided unwind did not run the __finally between the collision and its "
             "own target exactly once and resume there with its own ReturnValue" );
}

static void report_stage_h( void )
{
    ULONG64 ret;

    g_stage_reset();
    pf_stage_reset( PF_MODE_COLLIDE_INPLACE, RtlUnwindEx, PF_RETVAL_H );
    g_inner_body = g_collide_finally_frame;
    ret = PF_CALL( g_body, 0 );
    trace( pf_landing_hit ? "landed" : "returned" );

    begin( "collided unwind, in-place road: a __finally unwinds from inside the unwind" );
    out( "order: " );
    out_trace();
    out( " collide_calls=" );
    out_dec( (ULONG)g_collide_calls );
    out( " outer_filter_calls=" );
    out_dec( (ULONG)g_outer_filter_calls );
    out( " outer_except_calls=" );
    out_dec( (ULONG)g_outer_except_calls );
    verdict( trace_is( "graise gfilt gkfin gmidfin ph-target landed" ) &&
             g_collide_calls == 1 && g_outer_filter_calls == 1 && g_outer_except_calls == 0,
             "the __finally's own unwind did not preempt the unwind that was running it" );

    begin( "collided unwind, in-place road: the scope that collided does not run again" );
    out( "k_finally=" );
    out_dec( (ULONG)g_k_finally_calls );
    out( " abnormal=" );
    out_dec( (ULONG)g_k_finally_abnormal );
    out( " mid_finally=" );
    out_dec( (ULONG)g_mid_finally_calls );
    out( " abnormal=" );
    out_dec( (ULONG)g_mid_finally_abnormal );
    verdict( g_k_finally_calls == 1 && g_k_finally_abnormal == 1 &&
             g_mid_finally_calls == 1 && g_mid_finally_abnormal == 1,
             "the __finally that collided ran a second time, or the one between the "
             "collision and the target did not run -- the adopted unwind did not resume "
             "at the scope index the unwind it collided with had reached" );

    begin( "collided unwind, in-place road: the inner unwind's target is reached" );
    out( "arrive_calls=" );
    out_dec( (ULONG)g_arrive_calls );
    out_yn( " code_is_inner", g_arrive_code_is_inner );
    out_yn( " info_is_inner", g_arrive_info_is_inner );
    out_yn( " target_unwind_flag", g_arrive_target_flag );
    out( " landed=" );
    out_dec( (ULONG)pf_landing_hit );
    out_yn( " rax_is_returnvalue", pf_landing_rax == PF_RETVAL_H );
    out_yn( " rbx_is_frame_sentinel", pf_landing_rbx == PF_RBX_SENTINEL );
    out_yn( " pf_call_returned_rax", ret == PF_RETVAL_H );
    verdict( g_arrive_calls == 1 && g_arrive_code_is_inner && g_arrive_info_is_inner &&
             g_arrive_target_flag && g_arrive_unwinding_flag && g_arrive_targetip_is_landing &&
             pf_landing_hit == 1 && pf_landing_rax == PF_RETVAL_H &&
             pf_landing_rbx == PF_RBX_SENTINEL && ret == PF_RETVAL_H,
             "the collided unwind did not resume in the frame the __finally named, with "
             "that unwind's own record and ReturnValue" );
}

int seh_handlers_run( void )
{
    out( "seh_handlers: start\n" );
    thread_name_init();

    /* ---- A: the private handler and its DISPATCHER_CONTEXT -------------- */
    pf_stage_reset( PF_MODE_REPORT, RtlUnwindEx, 0 );
    stage_a();
    report_stage_a();

    /* ---- B: the MSVC thread-naming idiom, both dispositions, both paths -- */
    report_stage_b();

    /* ---- C: RtlUnwindEx called by the guest handler, by both routes ------ */
    report_stage_c( "RtlUnwindEx from a guest handler, imported from KERNEL32.dll",
                    RtlUnwindEx, PF_RETVAL_CK );
    report_stage_c( "RtlUnwindEx from a guest handler, imported from ntdll.dll",
                    RtlUnwindEx_ntdll, PF_RETVAL_CN );

    /* ---- D: a private handler that chains to __C_specific_handler -------- */
    report_stage_d();

    /* ---- E: the consolidating unwind, which is how MSVC spells `catch` --- */
    report_stage_e();

    /* ---- F: the same, down the port's other road (in place) -------------- */
    report_stage_f();

    /* ---- G and H: the collided unwind, once per road --------------------- */
    report_stage_g();
    report_stage_h();

    /* ---- the total, across every stage ---------------------------------- */
    begin( "the private handler was entered this many times in all" );
    out( "witness_delta=" );
    out_dec( (ULONG)(pf_witness - 0x5e400000ull) );
    verdict( pf_witness == 0x5e400000ull + 17,
             "the private handler ran a different number of times than the "
             "stages above account for" );

    out( failures ? "seh_handlers: FAIL " : "seh_handlers: PASS " );
    out_dec( (ULONG)(step - failures) );
    out( "/" );
    out_dec( (ULONG)step );
    out( "\n" );
    return failures ? 1 : 0;
}

/* ------------------------------------------------------------- the controls
 *
 * Three separate builds of this file, each of which must DIE -- promptly,
 * nonzero, and by name.  They are separate images rather than stages of the
 * run because each of them ends the process, and because a control that shares
 * a process with the thing it is controlling is not a control.
 */

int seh_handlers_unhandled( void )
{
    out( "seh_handlers: unhandled probe, faulting under a declining private handler\n" );
    pf_stage_reset( PF_MODE_REPORT, RtlUnwindEx, 0 );
    PF_CALL( stage_a_fault, 0 );
    out( "seh_handlers: FAIL the unhandled fault returned\n" );
    return 1;
}

/* The control for the collided unwind, now that the collision itself is four
 * passing steps: a handler that returns ExceptionCollidedUnwind and CHANGES
 * NOTHING -- the same frame, the same ScopeIndex, the same dispatcher context,
 * every time it is entered.  Adopting that a second time would re-enter this
 * handler forever, so the port must refuse it by name.  It is the degenerate
 * case rather than the unimplemented one: the disposition is served now, and
 * what is refused is a collision that cannot advance. */
int seh_handlers_collided( void )
{
    out( "seh_handlers: collided-unwind probe, the private handler returns "
         "ExceptionCollidedUnwind without ever advancing\n" );
    pf_stage_reset( PF_MODE_COLLIDE, RtlUnwindEx, 0 );
    PF_CALL( stage_a_raise, 0 );
    out( "seh_handlers: FAIL the refused disposition was accepted\n" );
    return 1;
}

int seh_handlers_exit_unwind( void )
{
    out( "seh_handlers: exit-unwind probe, the private handler calls "
         "RtlUnwindEx(NULL, ...)\n" );
    pf_stage_reset( PF_MODE_EXIT_UNWIND, RtlUnwindEx, 0 );
    PF_CALL( stage_a_raise, 0 );
    out( "seh_handlers: FAIL the exit unwind was accepted\n" );
    return 1;
}

/* The control for the consolidating unwind, now that the unwind itself is a
 * PASSING stage: a record that asks for a callback-based resume and names no
 * callback.  There is no resume address in it and none this side could invent,
 * so it must be refused by name rather than resumed at TargetIp -- which is the
 * one plausible wrong answer, and a silent one, because TargetIp is a perfectly
 * good address that simply is not where a catch block lives. */
int seh_handlers_consolidate_noroutine( void )
{
    out( "seh_handlers: consolidating-unwind probe, the private handler calls "
         "RtlUnwindEx with STATUS_UNWIND_CONSOLIDATE and no routine\n" );
    pf_stage_reset( PF_MODE_CONS_NOROUTINE, RtlUnwindEx, 0 );
    PF_CALL( stage_a_raise, 0 );
    out( "seh_handlers: FAIL the routineless consolidating unwind was accepted\n" );
    return 1;
}

/* The guest build has no C runtime: this IS the image entry point. */
void WINAPI seh_handlers_entry( void )
{
#if defined(SEH_HANDLERS_UNHANDLED)
    ExitProcess( (UINT)seh_handlers_unhandled() );
#elif defined(SEH_HANDLERS_COLLIDED)
    ExitProcess( (UINT)seh_handlers_collided() );
#elif defined(SEH_HANDLERS_EXIT_UNWIND)
    ExitProcess( (UINT)seh_handlers_exit_unwind() );
#elif defined(SEH_HANDLERS_CONS_NOROUTINE)
    ExitProcess( (UINT)seh_handlers_consolidate_noroutine() );
#else
    ExitProcess( (UINT)seh_handlers_run() );
#endif
}
