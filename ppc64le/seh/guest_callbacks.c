/*
 * guest_callbacks -- the gate for two guest/native boundary-crossing contracts
 * that are being fixed in dlls/ntdll/signal_ppc64.c and dlls/ntdll/unix/loader.c
 * as this file is written: a WNDPROC handed to native user32 inside a struct
 * field, and CreateThread's dwStackSize reaching the SEPARATE guest stack the
 * emulator allocates for a thread, rather than only the native ppc64 one Wine
 * gives it.
 *
 * PART A: WNDPROC EVERYWHERE, NOT JUST IN ARGUMENTS.
 *
 * wrap_guest_callback() at registration time is how this port keeps native
 * code from ever bctrl-ing into x86-64 bytes: the thunk that receives a guest
 * function pointer swaps it for a native trampoline before native code ever
 * sees it (dlls/ntdll/signal_ppc64.c, "guest callback trampolines").  Every row
 * in thunk_overrides[] that carries a cb_mask says which ARGUMENT of a call is
 * a callback -- EnumFontFamiliesW's fourth argument, qsort's third.  A WNDPROC
 * is different: it does not arrive as a call argument at all, it arrives
 * buried inside WNDCLASSEXW.lpfnWndProc (and WNDCLASSW.lpfnWndProc, the
 * non-Ex form -- DOOM (2016) imports RegisterClassA and RegisterClassW, not
 * either Ex variant), and an argument-indexed mask has no way to name a field
 * inside a struct.  Until this is fixed, that WNDPROC goes through RAW, native
 * user32 stores the guest address as if it were an ordinary native function
 * pointer, and the first WM_NCCREATE it sends is a ppc64 bctrl into x86-64
 * instructions.  DOOM (2016) died exactly this way.  Worse than dying: user32's
 * own callback dispatcher (dispatch_user_callback in dlls/ntdll/exception.c,
 * reached from KiUserCallbackDispatcher) wraps the call in __TRY/__EXCEPT_ALL
 * and SWALLOWS whatever comes out --
 *
 *     err:seh:dispatch_user_callback ignoring exception c000001d
 *
 * -- so the game got a window that never received its messages and no error
 * at all.  A silent failure is the failure mode this file exists to make loud.
 *
 * The fix this gate is written against (not the current behaviour -- see the
 * runner's comments on that) has to swap the pointer at EVERY entry point a
 * WNDPROC can arrive through: RegisterClassExW and RegisterClassW (item 1/2
 * below), the LRESULT a SendMessageW extracts from one (item 3, the whole
 * reason a separate "wide" callback class exists in struct thunk_override --
 * see cb_wide next to cb_mask in signal_ppc64.c: an ordinary wrapped callback
 * sign-extends its low 32 bits because every comparator and enum callback in
 * the corpus returns int, but an LRESULT is a genuine 64-bit value and doing
 * that to one is exactly as wrong as truncating it), SetWindowLongPtrW's
 * argument AND its return value (item 4/5a), CallWindowProcW's own argument
 * when handed a RAW guest pointer that was never registered anywhere (item
 * 5b -- this is the case no registration hook can see coming, because nothing
 * was ever "registered": a guest program is entitled to keep its own function
 * pointer around and hand it straight to CallWindowProcW), and
 * GetWindowLongPtrW's readback (item 7).  DefWindowProcW (item 6) is the
 * control that says the native pass-through path -- the 99% of messages no
 * application handles -- still works when the guest chooses not to intercept.
 *
 * AS LANDED (this probe now gates the real thing, not just the contract):
 * wrap_guest_callback_ex(fn, wide) replaced wrap_guest_callback(), with a
 * PER-SLOT return width -- one trampoline per (target, width) pair rather
 * than one per target -- and wrap_guest_wndproc() is the WNDPROC-specific
 * entry point that always asks for the wide slot.  New thunk_overrides[]
 * rows drive it from emu_RegisterClassEx, emu_RegisterClass,
 * emu_SetWindowLongPtr (only when the index is GWLP_WNDPROC; SetWindowLongW
 * gets no row, because user32 itself fails that index with
 * ERROR_INVALID_INDEX on _WIN64) and emu_CallWindowProc, which wraps its OWN
 * first argument.  wrap_guest_wndproc() also passes two non-guest shapes
 * through untouched and quietly -- a win32u WINPROC handle
 * (value>>16 == 0xffff) and an ordinary native window procedure -- which is
 * exactly the "do NOT require a particular one" shape items 5/7 above
 * already only check by calling, never by comparing.
 *
 * WHY THE SIGN-EXTENSION CONSTANTS ARE SHAPED THE WAY THEY ARE.  A callback
 * that returns int is safe to sign-extend from EAX because every x86-64 callee
 * that returns one clears the top 32 bits of RAX (the ELFv2 return-value
 * convention this port's ppc64 side relies on for its OWN 32-bit callback
 * class).  An LRESULT window procedure is not that: GC_RETVAL1's low 32 bits
 * are 0xDEADBEEF, whose top nibble D=1101 has bit 31 set, so a path that
 * sign-extends the low half produces 0xFFFFFFFFDEADBEEF -- visibly wrong
 * against the true 0x00C0FFEEDEADBEEF.  GC_RETVAL2's low 32 bits are
 * 0x12345678, whose top nibble 1=0001 has bit 31 CLEAR, so sign-extension and
 * zero-extension agree with each other there and both are still wrong,
 * because either one throws away the upper half's 0x00C0FFEE.  Comparing the
 * full 64 bits catches a truncating bug regardless of which half-truth
 * extension it uses; two constants with bit 31 differently set is what proves
 * the check is not coincidentally passing for one polarity only.
 *
 * PART B: A GUEST THREAD RUNS ON TWO STACKS.
 *
 * dwStackSize is Wine's problem twice over on this port: once for the native
 * ppc64 stack the thread's own C code executes on (that half already worked;
 * init_thread_stack() has always honoured it), and once for the SEPARATE guest
 * stack the embedded emulator allocates for the x86-64 side of the same
 * thread -- see emu_run_loop() in dlls/ntdll/unix/loader.c, which until this
 * fix always sized that second stack from the image's own SizeOfStackReserve
 * regardless of what CreateThread was asked for.  DOOM (2016) asks its worker
 * threads for 8 MiB with STACK_SIZE_PARAM_IS_A_RESERVATION and says so in its
 * own log -- "Starting stack size in KB: 8388608" -- and got the image's 2 MiB
 * default instead, silently, because dwStackSize never reached the call that
 * allocates the stack the guest code actually runs on.  This probe asks for
 * 16 MiB rather than DOOM's 8 to keep the assertion an inequality
 * (StackBase - DeallocationStack >= 16 MiB) comfortably clear of the image
 * default this probe's own linker sets (2 MiB, chosen to echo DOOM's own
 * figure) rather than close enough to be a rounding argument.
 *
 * AS LANDED: emu_run_loop() now sizes the guest stack by MIRRORING the
 * thread's own NATIVE stack, which is where Wine already applies dwStackSize
 * and STACK_SIZE_PARAM_IS_A_RESERVATION -- no new plumbing carries the value
 * across the guest/native boundary a second time; the existing native
 * thread-stack sizing simply becomes the source of truth for the guest one
 * too.  Traced on the MODULE channel, not seh: "emu_run_loop guest stack for
 * <entry>: ... (<N> bytes, from this thread's own stack)", or "... from the
 * image" when no native thread stack was captured (or under the runner's
 * WINEEMUNOSTACKSIZE=1 negative control, which forces that fallback
 * unconditionally).
 *
 * TEB OFFSETS: gs:[0x08] is NT_TIB64.StackBase, gs:[0x10] is
 * NT_TIB64.StackLimit (include/winnt.h, struct _NT_TIB64, offsets 0000/0008/
 * 0010 confirmed by reading the struct in this tree before writing this
 * file), and gs:[0x1478] is TEB64.DeallocationStack (include/winternl.h,
 * struct _TEB64, confirmed the same way: NT_TIB64 Tib at 0000, then the
 * fields down to StaticUnicodeBuffer land DeallocationStack at 1478).  Read
 * by hand below rather than through <intrin.h>'s __readgsqword(), which this
 * file's build (GUESTCC in check-guest-callbacks.sh: -nostdlibinc, no CRT,
 * windows.h and nothing else) has no dependency on one way or the other --
 * gc_readgsqword() below compiles to the identical `movq %gs:(%reg), %reg`
 * the intrinsic would, with no header search path to get wrong.
 *
 * USABLE DEPTH, NOT JUST THE BOUND: a port could satisfy "StackBase -
 * DeallocationStack >= 16 MiB" by widening what the TEB SAYS without the
 * memory actually being there to grow into -- the guard-page machinery that
 * turns a reservation into real committed pages on demand is a second thing
 * that has to work.  So the big-stack thread also recurses with a 64 KiB
 * frame, touched at both ends so nothing is optimised away, until it has
 * genuinely consumed more than 12 MiB of the reservation, and reports how far
 * it got as a NUMBER OF MEBIBYTES -- never as an address, because an address
 * cannot be diffed byte for byte across runs and this gate's transcript is
 * diffed byte for byte.  It stops at a hard frame cap well short of the 16 MiB
 * bound rather than waiting to find the reservation's edge by faulting: a
 * probe that free-falls toward a guard page to see what happens is not a gate,
 * it is a coin flip with the process.  If the TEB bound check has already
 * failed -- the current, unfixed state -- the recursion is skipped outright
 * rather than attempted against a stack that was never actually widened; see
 * the runner's comments on why the FIRST observed run is expected to show
 * exactly that.
 *
 * WHY THIS GATE HAS NO NATIVE LANE, of a different shape from why
 * check-seh-handlers.sh has none.  That gate's construct (a hand-written
 * x86-64 language handler) literally cannot be expressed on ppc64.  This one
 * could: a native ppc64 WNDPROC and a native CreateThread both already work,
 * because the defect under test is not "does Wine's window/thread machinery
 * work", it is "does a pointer that crosses the x86-64<->ppc64 boundary
 * survive the crossing" -- and there is no crossing to test unless the
 * pointer in question is genuinely guest code.  A native-lane variant of this
 * file would test nothing this file does not already test better.
 *
 * NO CRT: this file's entry point IS the image entry point, exactly as in
 * seh_handlers.c, and for the same reason -- a CRT would put unrelated
 * .pdata and unrelated imports between the probe and the mechanism under
 * test, and this probe in particular needs to know precisely which DLL
 * exports it pulled in, because thunk_overrides rows are keyed per-module.
 *
 * Copyright 2026 the ppc64le port authors
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 */

#include <windows.h>

#define GC_NOINLINE __attribute__((noinline))

/* ------------------------------------------------------------- output
 *
 * Identical in spirit to seh_handlers.c's helpers (this file shares no
 * source with that one, deliberately: it is a separate deliverable and a
 * separate gate, and duplicating four small formatting functions is cheaper
 * than coupling two gates that should be able to fail independently).
 */

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
 * The same instrument seh_handlers.c uses: a set of "did it happen" booleans
 * cannot distinguish "WM_NCCREATE then WM_CREATE" from "WM_CREATE then
 * WM_NCCREATE", and a registration-time interception bug's first symptom is
 * exactly an ordering or identity mistake, not a missing call.  Only the
 * messages this probe explicitly instruments push a token; every other
 * message a real window creation sends (WM_GETMINMAXINFO and friends, which
 * are dispatched to the WNDPROC like any other message and are neither
 * counted nor suppressed here) passes through untraced, which is what keeps
 * the trace strings below exact-matchable regardless of how many messages
 * user32 privately decides to send around the ones this probe cares about.
 */

static char trace_buf[256];
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

static void out_trace( void )
{
    out( "trace='" );
    out( trace_buf );
    out( "'" );
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

/* =======================================================================
 *  PART A -- WNDPROC everywhere, not just in arguments
 * ======================================================================= */

/* Private messages, chosen well above WM_APP so nothing in user32's own
 * message space can collide with them by accident. */
#define GC_WM_RET1   (WM_APP + 7)   /* sign-extension check, low32 bit31 SET   */
#define GC_WM_RET2   (WM_APP + 8)   /* sign-extension check, low32 bit31 CLEAR */
#define GC_WM_RET3   (WM_APP + 9)   /* the swapped-in second proc's own reply  */

/* See the header comment for why these two specific low halves were chosen.
 * The 0x00c0ffee upper half is shared, so a return path that ever drops the
 * WHOLE upper 32 bits (not just sign-extends) is caught by both. */
#define GC_RETVAL1   0x00c0ffeeDEADBEEFull   /* 0xD... -> bit 31 of low32 SET   */
#define GC_RETVAL2   0x00c0ffee12345678ull   /* 0x1... -> bit 31 of low32 CLEAR */
#define GC_RETVAL3   0x00c0ffeeCAFEF00Dull   /* the second proc's own value, just
                                                 needs to be distinct from the two
                                                 above; its own bit 31 happens to
                                                 be set too (0xC... = 1100...) */

/* One witness per guest WNDPROC, seeded so it reads as .data rather than
 * .bss, incremented on every entry regardless of which message arrived (unlike
 * the per-message counters below, which only count messages this probe
 * explicitly recognises).  gc_total_witness sums all three and is the number
 * the runner's WINEDEBUG=+seh cross-check compares against the port's own
 * "calling guest callback" trace count -- read FROM THIS PROGRAM'S OWN
 * TRANSCRIPT by the shell script rather than hard-coded there, because unlike
 * seh_handlers.c's raise count (which this probe fully controls), the exact
 * number of messages CreateWindowExW privately sends before WM_NCCREATE is a
 * property of user32's internals, not of this file. */
static ULONG64 gc_witness_a     = 0x77a00000ull;
static ULONG64 gc_witness_b     = 0x77b00000ull;
static ULONG64 gc_witness_c     = 0x77c00000ull;
static ULONG64 gc_total_witness = 0x77000000ull;

static int nccreate_calls_a, create_calls_a;
static int nccreate_calls_b, create_calls_b;

/* Class A: RegisterClassExW, and the WNDPROC that answers WM_NCCREATE and
 * WM_CREATE ITSELF (no DefWindowProcW hand-off here -- class B below is where
 * the DefWindowProcW pass-through, item 6 of the contract, gets proven). */
static LRESULT CALLBACK gc_wndproc_a( HWND hwnd, UINT msg, WPARAM wp, LPARAM lp )
{
    gc_witness_a++;
    gc_total_witness++;

    switch (msg)
    {
    case WM_NCCREATE:
        nccreate_calls_a++;
        trace( "a-ncc" );
        return TRUE;
    case WM_CREATE:
        create_calls_a++;
        trace( "a-create" );
        return 0;
    case GC_WM_RET1:
        trace( "a-ret1" );
        return (LRESULT)GC_RETVAL1;
    case GC_WM_RET2:
        trace( "a-ret2" );
        return (LRESULT)GC_RETVAL2;
    default:
        return DefWindowProcW( hwnd, msg, wp, lp );
    }
}

/* Class B: RegisterClassW (the non-Ex form DOOM actually imports), and a
 * WNDPROC that hands WM_NCCREATE and WM_CREATE to DefWindowProcW after
 * tracing them -- this IS being called as guest code (gc_witness_b and the
 * trace both prove it), it is simply choosing to delegate, which is what a
 * real WNDPROC does for the overwhelming majority of messages it receives
 * and is exactly the path a broken registration-side wrap would never let a
 * DIFFERENT bug hide behind: if DefWindowProcW itself were unreachable from
 * guest code, class B's window would never finish being created. */
static LRESULT CALLBACK gc_wndproc_b( HWND hwnd, UINT msg, WPARAM wp, LPARAM lp )
{
    gc_witness_b++;
    gc_total_witness++;

    switch (msg)
    {
    case WM_NCCREATE:
        nccreate_calls_b++;
        trace( "b-ncc" );
        return DefWindowProcW( hwnd, msg, wp, lp );
    case WM_CREATE:
        create_calls_b++;
        trace( "b-create" );
        return DefWindowProcW( hwnd, msg, wp, lp );
    default:
        return DefWindowProcW( hwnd, msg, wp, lp );
    }
}

/* The proc SetWindowLongPtrW swaps class A's window onto.  Never registered
 * through RegisterClassExW/RegisterClassW at all -- it reaches native code
 * only through the GWLP_WNDPROC write in step 9 below, which is the point:
 * item 4 of the contract is specifically about a registration-shaped
 * interception that has to fire on a SECOND kind of registration site. */
static LRESULT CALLBACK gc_wndproc_c( HWND hwnd, UINT msg, WPARAM wp, LPARAM lp )
{
    gc_witness_c++;
    gc_total_witness++;

    if (msg == GC_WM_RET3)
    {
        trace( "c-ret3" );
        return (LRESULT)GC_RETVAL3;
    }
    return DefWindowProcW( hwnd, msg, wp, lp );
}

/* A WINPROC handle, in Wine's own internal shape, is a small value with no
 * high dword and 0xffff in the low word's top bits -- nothing at all like a
 * real pointer, which on a 64-bit process lives well above 4 GiB.  Printed
 * only as this relation, never as the value itself, per the "no addresses in
 * a diffed transcript" rule; the contract explicitly does not require either
 * shape, only that CallWindowProcW work with whatever came back (steps 10
 * and 12 check that separately). */
static void print_proc_shape( const char *label, LONG_PTR val )
{
    ULONG64 v = (ULONG64)(ULONG_PTR)val;
    BOOL high32_nonzero = (v >> 32) != 0;
    BOOL winproc_shaped  = !high32_nonzero && ((ULONG32)v & 0xffff0000u) == 0xffff0000u;

    out( label );
    out_yn( "_is_plain_pointer", high32_nonzero );
    out( " " );
    out( label );
    out_yn( "_matches_winproc_handle_shape", winproc_shaped );
}

static void report_part_a( void )
{
    WNDCLASSEXW wcx = { 0 };
    WNDCLASSW   wc  = { 0 };
    HINSTANCE   hInst = GetModuleHandleW( NULL );
    ATOM        atomA, atomB;
    HWND        hwndA, hwndB;
    BOOL        used_hwnd_message;
    LRESULT     ret;
    LONG_PTR    prev, cur;

    /* ---- 1: RegisterClassExW names a guest WNDPROC -------------------- */
    wcx.cbSize        = sizeof(wcx);
    wcx.lpfnWndProc    = gc_wndproc_a;
    wcx.hInstance      = hInst;
    wcx.lpszClassName  = L"GCProbeClassA";
    atomA = RegisterClassExW( &wcx );

    begin( "RegisterClassExW registers a class naming a guest WNDPROC" );
    out_yn( "atom_nonzero", atomA != 0 );
    verdict( atomA != 0, "RegisterClassExW failed outright; nothing below can run" );

    /* ---- 2: CreateWindowExW, HWND_MESSAGE preferred -------------------
     *
     * A message-only window needs no display and no window manager, which is
     * exactly what this probe wants in a headless bring-up environment.  If
     * this port's window subsystem cannot make one -- a property of the
     * user32/win32u driver stack, unrelated to the guest-callback mechanism
     * this file exists to gate -- the fallback below is a REAL WS_OVERLAPPED
     * window rather than a fabricated pass.  Which one happened is printed as
     * its own relation rather than silently swallowed, and the SAME choice is
     * reused for class B below rather than decided twice. */
    hwndA = CreateWindowExW( 0, L"GCProbeClassA", L"", WS_POPUP, 0, 0, 0, 0,
                             HWND_MESSAGE, NULL, hInst, NULL );
    used_hwnd_message = (hwndA != NULL);
    if (!hwndA)
        hwndA = CreateWindowExW( 0, L"GCProbeClassA", L"", WS_OVERLAPPED, 0, 0, 0, 0,
                                 NULL, NULL, hInst, NULL );

    begin( "CreateWindowExW creates a window of class A" );
    out_yn( "used_hwnd_message", used_hwnd_message );
    out( " " );
    out_yn( "created", hwndA != NULL );
    verdict( hwndA != NULL, "CreateWindowExW returned NULL under both the "
             "HWND_MESSAGE and the WS_OVERLAPPED style; nothing that depends "
             "on a live window can run" );
    if (!used_hwnd_message)
        out( "note: HWND_MESSAGE (a message-only, headless window) was refused "
             "by this port's window subsystem; falling back to an ordinary "
             "WS_OVERLAPPED window, which is what the two window-creation "
             "steps below actually exercised\n" );

    /* ---- 3: the guest WNDPROC ran, as guest code, for NCCREATE then
     * CREATE, in that order, and answered them itself -------------------- */
    begin( "class A's guest WNDPROC ran for WM_NCCREATE then WM_CREATE" );
    out( "nccreate_calls=" );
    out_dec( (ULONG)nccreate_calls_a );
    out( " create_calls=" );
    out_dec( (ULONG)create_calls_a );
    out( " " );
    out_trace();
    verdict( hwndA != NULL && nccreate_calls_a == 1 && create_calls_a == 1 &&
             trace_is( "a-ncc a-create" ),
             "the guest WNDPROC did not receive exactly one WM_NCCREATE "
             "followed by exactly one WM_CREATE -- either it never ran (the "
             "registration-side swap did not happen and native user32 has a "
             "raw guest pointer it cannot call), or it ran out of order" );

    /* ---- 4: RegisterClassW, the non-Ex form DOOM actually imports ------ */
    wc.lpfnWndProc   = gc_wndproc_b;
    wc.hInstance     = hInst;
    wc.lpszClassName = L"GCProbeClassB";
    atomB = RegisterClassW( &wc );

    begin( "RegisterClassW (non-Ex) registers a second guest WNDPROC" );
    out_yn( "atom_nonzero", atomB != 0 );
    verdict( atomB != 0, "RegisterClassW failed outright" );

    /* Class A's own WM_NCCREATE/WM_CREATE tokens are still sitting in the
     * trace buffer from step 3 above; without this reset, class B's tokens
     * would simply APPEND to them and the exact-match check below would fail
     * for a reason that has nothing to do with class B. */
    trace_reset();
    hwndB = used_hwnd_message
        ? CreateWindowExW( 0, L"GCProbeClassB", L"", WS_POPUP, 0, 0, 0, 0,
                           HWND_MESSAGE, NULL, hInst, NULL )
        : CreateWindowExW( 0, L"GCProbeClassB", L"", WS_OVERLAPPED, 0, 0, 0, 0,
                           NULL, NULL, hInst, NULL );

    begin( "CreateWindowExW creates a window of class B" );
    out_yn( "created", hwndB != NULL );
    verdict( hwndB != NULL, "CreateWindowExW returned NULL for class B" );

    /* ---- 6 (proven here, ahead of 5, because it rides on class B's own
     * window creation): DefWindowProcW pass-through from inside a guest
     * WNDPROC.  Class B's WNDPROC handed BOTH WM_NCCREATE and WM_CREATE to
     * DefWindowProcW rather than answering them itself; the window existing
     * at all is the proof the pass-through call worked. */
    begin( "class B's guest WNDPROC ran WM_NCCREATE/WM_CREATE through "
           "DefWindowProcW" );
    out( "nccreate_calls=" );
    out_dec( (ULONG)nccreate_calls_b );
    out( " create_calls=" );
    out_dec( (ULONG)create_calls_b );
    out( " " );
    out_trace();
    verdict( hwndB != NULL && nccreate_calls_b == 1 && create_calls_b == 1 &&
             trace_is( "b-ncc b-create" ),
             "DefWindowProcW, called from inside a guest WNDPROC, did not "
             "complete window creation for class B" );

    /* ---- 5a (first half) / the sign-extension check --------------------
     *
     * Both sends go to class A's window while class A's own WNDPROC is still
     * installed -- the swap in step 9 has not happened yet. */
    ret = SendMessageW( hwndA, GC_WM_RET1, 0, 0 );
    begin( "SendMessageW: LRESULT with bit 31 of the low half SET survives" );
    out( "got=0x" );
    out_hex( (ULONG64)ret, 16 );
    out( " want=0x" );
    out_hex( GC_RETVAL1, 16 );
    verdict( (ULONG64)ret == GC_RETVAL1,
             "the full 64-bit LRESULT did not arrive; a sign-extending "
             "return path would produce 0xffffffffdeadbeef here" );

    ret = SendMessageW( hwndA, GC_WM_RET2, 0, 0 );
    begin( "SendMessageW: LRESULT with bit 31 of the low half CLEAR survives" );
    out( "got=0x" );
    out_hex( (ULONG64)ret, 16 );
    out( " want=0x" );
    out_hex( GC_RETVAL2, 16 );
    verdict( (ULONG64)ret == GC_RETVAL2,
             "the full 64-bit LRESULT did not arrive; sign-extension and "
             "zero-extension agree on this constant's low half and are BOTH "
             "wrong, because either one drops the 0x00c0ffee upper half" );

    /* ---- 4/5: SetWindowLongPtrW swaps in a SECOND guest WNDPROC, and its
     * own return value (the previous proc) must be usable ---------------- */
    prev = SetWindowLongPtrW( hwndA, GWLP_WNDPROC, (LONG_PTR)gc_wndproc_c );
    begin( "SetWindowLongPtrW(GWLP_WNDPROC) installs a second guest WNDPROC" );
    out_yn( "prev_nonzero", prev != 0 );
    verdict( prev != 0, "SetWindowLongPtrW returned 0 for the previous "
             "WNDPROC; class A's window apparently had none installed" );

    trace_reset();
    ret = SendMessageW( hwndA, GC_WM_RET3, 0, 0 );
    begin( "after the swap, SendMessageW reaches ONLY the new WNDPROC" );
    out( "got=0x" );
    out_hex( (ULONG64)ret, 16 );
    out( " " );
    out_trace();
    verdict( (ULONG64)ret == GC_RETVAL3 && trace_is( "c-ret3" ),
             "the message did not reach gc_wndproc_c with the value it "
             "alone returns; either the swap did not take effect, or the "
             "original guest WNDPROC is still the one being called" );

    /* ---- 5b(a): CallWindowProcW with whatever SetWindowLongPtrW handed
     * back.  On this port that may legitimately be the trampoline pool's own
     * pointer, or a Wine WINPROC handle of the 0xffff00nn shape -- printed as
     * a relation, required only to WORK, not to be one particular shape. */
    ret = CallWindowProcW( (WNDPROC)prev, hwndA, GC_WM_RET1, 0, 0 );
    begin( "CallWindowProcW with the value SetWindowLongPtrW returned" );
    print_proc_shape( "prev", prev );
    out( " got=0x" );
    out_hex( (ULONG64)ret, 16 );
    verdict( (ULONG64)ret == GC_RETVAL1,
             "CallWindowProcW could not call class A's original WNDPROC "
             "through the value SetWindowLongPtrW handed back" );

    /* ---- 5b(b): CallWindowProcW with the RAW guest address, taken
     * straight from this probe's own symbol -- never registered anywhere,
     * never wrapped by anything, which is exactly the case no
     * registration-time hook can see coming: a guest program that already
     * has its own function pointer and simply hands it to CallWindowProcW.
     * CallWindowProcW's own thunk_overrides row has to wrap its OWN callback
     * argument for this to work at all. */
    ret = CallWindowProcW( gc_wndproc_a, hwndA, GC_WM_RET1, 0, 0 );
    begin( "CallWindowProcW with the RAW guest WNDPROC address" );
    out( "got=0x" );
    out_hex( (ULONG64)ret, 16 );
    verdict( (ULONG64)ret == GC_RETVAL1,
             "CallWindowProcW did not correctly call a raw, never-registered "
             "guest function pointer passed directly as its own argument" );

    /* ---- 7: GetWindowLongPtrW readback -------------------------------
     *
     * The accepted answer on this port is the TRAMPOLINE (or WINPROC handle),
     * not the raw guest pointer gc_wndproc_c: Windows' own contract for
     * GetWindowLongPtrW is "returns what was set", and on this port what was
     * ACTUALLY set, from native user32's point of view, is whatever
     * SetWindowLongPtrW's thunk wrapped the raw pointer into -- the wrapping
     * is invisible to a real Windows program only because Windows has no
     * wrapping to be visible.  So this step asserts the readback is USABLE
     * (CallWindowProcW with it reaches gc_wndproc_c), and separately notes,
     * without failing on it either way, whether it echoes the raw pointer or
     * not. */
    cur = GetWindowLongPtrW( hwndA, GWLP_WNDPROC );
    begin( "GetWindowLongPtrW(GWLP_WNDPROC) readback is usable" );
    print_proc_shape( "cur", cur );
    out( " " );
    out_yn( "readback_equals_raw_guest_pointer", (void *)cur == (void *)gc_wndproc_c );
    ret = CallWindowProcW( (WNDPROC)cur, hwndA, GC_WM_RET3, 0, 0 );
    out( " callwindowproc_got=0x" );
    out_hex( (ULONG64)ret, 16 );
    verdict( (ULONG64)ret == GC_RETVAL3,
             "the value GetWindowLongPtrW read back could not be used to "
             "reach gc_wndproc_c through CallWindowProcW" );

    /* ---- the total, across every wrapped WNDPROC in this process ------- */
    begin( "total guest WNDPROC dispatches witnessed in this process" );
    out( "total=" );
    out_dec( (ULONG)(gc_total_witness - 0x77000000ull) );
    /* The floor is exactly the sends this probe itself issued and fully
     * controls: 2 (NCCREATE+CREATE, class A) + 2 (NCCREATE+CREATE, class B) +
     * 2 (GC_WM_RET1, GC_WM_RET2) + 1 (GC_WM_RET3 via SendMessageW) + 1
     * (CallWindowProcW via prev) + 1 (CallWindowProcW via the raw pointer) +
     * 1 (CallWindowProcW via the GetWindowLongPtrW readback) = 10.  Anything
     * ABOVE that floor is user32's own business (WM_GETMINMAXINFO and
     * whatever else it privately sends during creation) and is not asserted
     * to any exact value here -- see the runner for how the exact total gets
     * cross-checked against the port's own trace instead. */
    verdict( (gc_total_witness - 0x77000000ull) >= 10,
             "fewer guest WNDPROC dispatches were witnessed than this probe "
             "itself issued; something above did not run at all" );
}

/* =======================================================================
 *  PART B -- CreateThread's dwStackSize reaching the GUEST stack
 * ======================================================================= */

#define GC_BIG_STACK_SIZE   (16ull * 1024 * 1024)   /* what this probe asks for */
/* Kept under the ~4 KiB threshold past which the Windows x64 ABI requires a
 * stack-probe call (___chkstk_ms) before a single prologue `sub rsp` may
 * cross more than one guard page at a time -- measured directly against this
 * toolchain (clang -target x86_64-windows-gnu): a 64 KiB frame produced
 * "undefined symbol: ___chkstk_ms" at link time, because this file's build
 * has no CRT and therefore no implementation of that routine to link against.
 * A probe-less multi-page `sub rsp` is not merely a link error waiting to
 * happen on a toolchain that DOES supply the symbol, either: skipping past a
 * guard page in one jump instead of touching it is exactly the case the probe
 * exists to prevent, and the guest stack this port allocates has a REAL guard
 * page (dlls/ntdll/unix/loader.c's virtual_alloc_thread_stack), so an
 * unprobed large frame is a plausible way to make this step's own recursion
 * the thing that breaks, rather than the mechanism it is trying to measure.
 * Staying under the threshold sidesteps needing a hand-rolled chkstk
 * entirely, at the cost of needing more, smaller frames for the same depth. */
#define GC_FRAME_BYTES        4000
#define GC_TARGET_DEPTH_MIB   12                      /* must exceed this many */
#define GC_TARGET_DEPTH      ((ULONG64)GC_TARGET_DEPTH_MIB * 1024 * 1024)
/* 3400 * 4000 ~= 13.0 MiB: comfortably past GC_TARGET_DEPTH, comfortably short
 * of the 16 MiB reservation, and a hard stop regardless -- this probe halts at
 * a target depth and does not go looking for the reservation's actual edge by
 * faulting into it. */
#define GC_MAX_FRAMES         3400

/* gs:[0x08]/[0x10]/[0x1478] -- see the header comment for where these three
 * offsets were confirmed against this tree's own winnt.h/winternl.h.  Written
 * by hand rather than through <intrin.h> so this file's dependency surface
 * stays exactly windows.h, matching seh_handlers.c. */
static inline ULONG64 gc_readgsqword( unsigned long off )
{
    ULONG64 v;
    __asm__ __volatile__ ( "movq %%gs:(%1), %0" : "=r"(v) : "r"((ULONG64)off) );
    return v;
}

static inline ULONG64 gc_read_rsp( void )
{
    ULONG64 v;
    __asm__ __volatile__ ( "movq %%rsp, %0" : "=r"(v) );
    return v;
}

struct gc_depth
{
    ULONG64 start_rsp;
    ULONG64 deepest_rsp;   /* the numerically SMALLEST rsp seen: the stack
                               grows down, so this is the deepest point */
};

/* Indirection for the recursive call, exactly the SEH_CALL idiom
 * seh_handlers.c uses for the same reason: a call through a VOLATILE function
 * pointer cannot be proven, at compile time, to always reach this same
 * function, which is what defeats LLVM's tail-recursion elimination.  This
 * was measured to matter, not assumed: a first version of gc_recurse called
 * itself directly, with a second touch of pad[] placed AFTER the recursive
 * call on the theory that "something happens after the call" would be enough
 * to stop it being treated as a tail call.  Run for real at -O1 on this
 * target, that version reported a "depth" of 13504 bytes -- a small fraction
 * of even one frame -- because LLVM's tail-recursion elimination does not
 * require a literal tail call in the source; it proved the trailing store
 * could be hoisted (nothing about it depends on what the recursive call
 * produces) and rewrote the whole chain into a loop reusing ONE stack frame,
 * which would have made this step pass while testing nothing. */
static void (* volatile gc_recurse_hook)( struct gc_depth *d, int frames_left );

/* Recurses until either GC_TARGET_DEPTH bytes have been consumed or
 * GC_MAX_FRAMES is exhausted, whichever comes first.
 *
 * The alloca(), and the inline-asm "escape" of its pointer immediately after,
 * are ALSO measured-necessary and not decoration.  A `volatile char
 * pad[GC_FRAME_BYTES]` fixed-size local array, touched only at two constant
 * offsets, was optimised by LLVM at -O1 into two independent one-byte stack
 * slots -- 0x38 bytes of real `sub rsp`, not GC_FRAME_BYTES -- because
 * nothing in the source proves any OTHER byte of the declared array is ever
 * read, and a compile-time-constant-sized alloca gets the identical
 * treatment for the identical reason.  The inline asm below, which takes the
 * allocation's address as an input operand with a "memory" clobber, tells the
 * compiler the pointer escapes to something it cannot see into; it must then
 * assume the WHOLE region may be read or written and keep the real
 * allocation.  Confirmed on this exact toolchain by disassembling the
 * compiled object: without the escape, `sub rsp, 0x28`; with it, `sub rsp,
 * 0xfc8` (GC_FRAME_BYTES plus this function's own locals) -- a 100x
 * difference for a two-line change nothing else in this function's observable
 * behaviour depends on. */
GC_NOINLINE static void gc_recurse( struct gc_depth *d, int frames_left )
{
    char *pad = (char *)__builtin_alloca( GC_FRAME_BYTES );
    ULONG64 here;

    __asm__ __volatile__ ( "" : : "r"(pad) : "memory" );
    pad[0] = (char)frames_left;
    here = gc_read_rsp();
    if (here < d->deepest_rsp) d->deepest_rsp = here;

    if (frames_left > 0 && (d->start_rsp - here) < GC_TARGET_DEPTH)
    {
        gc_recurse_hook = gc_recurse;
        gc_recurse_hook( d, frames_left - 1 );
    }

    pad[GC_FRAME_BYTES - 1] = (char)(pad[0] + 1);
    __asm__ __volatile__ ( "" : : "r"(pad) : "memory" );
}

struct gc_thread_result
{
    LONG    started;         /* proves the thread function was entered at all */
    ULONG64 stack_base;
    ULONG64 dealloc;
    ULONG64 reserve_bytes;   /* stack_base - dealloc: the guest stack's actual
                                 reservation, as the TEB the guest reads
                                 describes it -- a SIZE, not an address, and
                                 therefore fine to print in a diffed
                                 transcript */
    BOOL    did_recurse;
    ULONG64 depth_bytes;     /* only meaningful when did_recurse is TRUE */
};

struct gc_thread_arg
{
    struct gc_thread_result *result;
    BOOL                      do_recurse;
};

/* Both threads run this SAME function, parameterised by do_recurse, so that
 * the two are being measured by IDENTICAL code and any difference in the
 * result is a difference in what CreateThread was asked for and nothing
 * else.  Neither thread calls out()/begin()/verdict() -- ALL reporting
 * happens on the main thread after WaitForSingleObject, which is what makes
 * the transcript's ordering deterministic regardless of how the two threads
 * are actually scheduled. */
static DWORD WINAPI gc_thread_proc( LPVOID param )
{
    struct gc_thread_arg *arg = (struct gc_thread_arg *)param;
    struct gc_thread_result *r = arg->result;

    r->stack_base = gc_readgsqword( 0x08 );
    r->dealloc    = gc_readgsqword( 0x1478 );
    r->reserve_bytes = r->stack_base - r->dealloc;
    r->did_recurse = FALSE;
    r->depth_bytes = 0;

    /* Guarded by the bound already read above: recursing toward 12+ MiB on a
     * stack the TEB itself says is smaller than 16 MiB would run this probe
     * off the end of a real reservation for no reason other than to watch it
     * fault, which the header comment already rules out.  This is exactly
     * the guard that lets the FIRST, pre-fix run of this probe finish
     * cleanly instead of crashing partway through: see the runner. */
    if (arg->do_recurse && r->reserve_bytes >= GC_BIG_STACK_SIZE)
    {
        struct gc_depth d;

        d.start_rsp = gc_read_rsp();
        d.deepest_rsp = d.start_rsp;
        gc_recurse( &d, GC_MAX_FRAMES );
        r->depth_bytes = d.start_rsp - d.deepest_rsp;
        r->did_recurse = TRUE;
    }

    r->started = 1;
    return 0;
}

static void report_part_b( void )
{
    struct gc_thread_result big_result = { 0 }, ctrl_result = { 0 };
    struct gc_thread_arg    big_arg, ctrl_arg;
    HANDLE hBig, hCtrl;
    DWORD  tid;
    ULONG  reserve_mib, depth_mib;

    big_arg.result = &big_result;
    big_arg.do_recurse = TRUE;
    ctrl_arg.result = &ctrl_result;
    ctrl_arg.do_recurse = FALSE;

    /* dwStackSize is CreateThread's SECOND argument; the flag that makes it a
     * RESERVATION rather than a commit size is OR'd into dwCreationFlags, the
     * fifth.  16 MiB (see the header comment for why not DOOM's own 8) with
     * STACK_SIZE_PARAM_IS_A_RESERVATION == 0x00010000. */
    hBig = CreateThread( NULL, (SIZE_T)GC_BIG_STACK_SIZE, gc_thread_proc, &big_arg,
                         STACK_SIZE_PARAM_IS_A_RESERVATION, &tid );
    begin( "CreateThread(dwStackSize=16MiB, STACK_SIZE_PARAM_IS_A_RESERVATION)" );
    out_yn( "created", hBig != NULL );
    verdict( hBig != NULL, "CreateThread failed outright for the big-stack thread" );

    if (hBig)
    {
        DWORD w = WaitForSingleObject( hBig, 30000 );

        begin( "the big-stack thread ran and reported back" );
        out_yn( "joined", w == WAIT_OBJECT_0 );
        out( " " );
        out_yn( "started", big_result.started != 0 );
        verdict( w == WAIT_OBJECT_0 && big_result.started,
                 "WaitForSingleObject did not observe the thread finish, or "
                 "it never reached the point that publishes its result" );

        reserve_mib = (ULONG)(big_result.reserve_bytes / (1024 * 1024));
        begin( "the big-stack thread's OWN TEB describes a >=16MiB guest stack" );
        out( "reserve_mib=" );
        out_dec( reserve_mib );
        out( " reserve_bytes=" );
        out_dec( (ULONG)big_result.reserve_bytes );
        verdict( big_result.reserve_bytes >= GC_BIG_STACK_SIZE,
                 "StackBase - DeallocationStack, read from inside the thread "
                 "via gs:[0x08]/gs:[0x1478], is smaller than the 16 MiB this "
                 "thread asked CreateThread for -- dwStackSize is not "
                 "reaching the guest stack the emulator actually runs this "
                 "thread's x86-64 code on" );

        begin( "usable depth: the big-stack thread actually descended >12MiB" );
        if (!big_result.did_recurse)
        {
            out( "skipped (the bound above already failed; recursing toward "
                 "12 MiB on an undersized stack would fault rather than prove "
                 "anything)" );
            verdict( FALSE, "the depth cannot be shown to be usable when the "
                     "reservation bound is already known to be too small" );
        }
        else
        {
            depth_mib = (ULONG)(big_result.depth_bytes / (1024 * 1024));
            out( "depth_mib=" );
            out_dec( depth_mib );
            out( " depth_bytes=" );
            out_dec( (ULONG)big_result.depth_bytes );
            verdict( big_result.depth_bytes > GC_TARGET_DEPTH,
                     "the recursion did not reach past 12 MiB of actual stack "
                     "depth before hitting its frame cap -- the reservation "
                     "is wide on paper (the previous step passed) but the "
                     "guard-page growth that turns it into usable memory is "
                     "not keeping up" );
        }
    }
    else
    {
        begin( "the big-stack thread's TEB (skipped: thread was never created)" );
        verdict( FALSE, "no thread to read a TEB from" );
        begin( "usable depth (skipped: thread was never created)" );
        verdict( FALSE, "no thread to recurse on" );
    }

    /* ---- the control: dwStackSize=0 in the SAME process ---------------
     *
     * This is what makes "16 MiB" mean something rather than being a port
     * that simply always hands out 16 MiB regardless of what was asked for.
     * dwStackSize=0 is standard Win32 for "use the image's own default", and
     * this probe's own linker sets that default to 2 MiB (echoing DOOM's own
     * figure) precisely so the expected transcript can assert an exact
     * number here rather than only an inequality. */
    hCtrl = CreateThread( NULL, 0, gc_thread_proc, &ctrl_arg, 0, &tid );
    begin( "CreateThread(dwStackSize=0) is the control" );
    out_yn( "created", hCtrl != NULL );
    verdict( hCtrl != NULL, "CreateThread failed outright for the control thread" );

    if (hCtrl)
    {
        DWORD w = WaitForSingleObject( hCtrl, 30000 );

        begin( "the control thread ran and reported back" );
        out_yn( "joined", w == WAIT_OBJECT_0 );
        out( " " );
        out_yn( "started", ctrl_result.started != 0 );
        verdict( w == WAIT_OBJECT_0 && ctrl_result.started,
                 "the control thread did not finish and publish its result" );

        reserve_mib = (ULONG)(ctrl_result.reserve_bytes / (1024 * 1024));
        begin( "the control thread's guest stack is the image default, not 16MiB" );
        out( "reserve_mib=" );
        out_dec( reserve_mib );
        out_yn( " reserve_smaller_than_16mib", ctrl_result.reserve_bytes < GC_BIG_STACK_SIZE );
        verdict( ctrl_result.reserve_bytes < GC_BIG_STACK_SIZE &&
                 ctrl_result.reserve_bytes == 2ull * 1024 * 1024,
                 "the control thread's guest stack is not the 2 MiB this "
                 "probe's own linker set as the image default -- either the "
                 "control is somehow also getting 16 MiB (dwStackSize=0 is "
                 "being misread as a request rather than a default), or this "
                 "probe's own build no longer sets the stack reserve it "
                 "assumes" );
    }
    else
    {
        begin( "the control thread's guest stack (skipped: never created)" );
        verdict( FALSE, "no thread to read a TEB from" );
    }
}

/* ------------------------------------------------------------- the run */

int guest_callbacks_run( void )
{
    out( "guest_callbacks: start\n" );

    report_part_a();
    report_part_b();

    out( failures ? "guest_callbacks: FAIL " : "guest_callbacks: PASS " );
    out_dec( (ULONG)(step - failures) );
    out( "/" );
    out_dec( (ULONG)step );
    out( "\n" );
    return failures ? 1 : 0;
}

/* The guest build has no C runtime: this IS the image entry point. */
void WINAPI guest_callbacks_entry( void )
{
    ExitProcess( (UINT)guest_callbacks_run() );
}
