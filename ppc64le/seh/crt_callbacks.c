/*
 * crt_callbacks -- the gate for the five C-runtime registration points that
 * dlls/msvcr100/msvcr100.thunks used to list as STILL OPEN: _beginthread's
 * and _beginthreadex's start routines, _set_invalid_parameter_handler,
 * _set_purecall_handler and __setusermatherr.  Styx: Master of Shadows
 * imports all five from MSVCR100.dll.
 *
 * WHAT WAS WRONG.  Each of these hands the CRT a function pointer that the
 * CRT stores and calls LATER, so on this port each was a raw guest address
 * parked in a native slot, waiting for native ppc64 code to mtctr/bctrl into
 * x86-64 bytes.  That is the FlsAlloc failure mode rather than the qsort one:
 * qsort calls its comparator before it returns, so a bad comparator row is
 * visible immediately, whereas every registration here returns success and
 * the branch into guest bytes happens on some later thread, at some later
 * math error, at the first pure-virtual call -- far from the registration
 * that caused it and with nothing left naming it.
 *
 * The fix is rows in the registration-interception table in
 * dlls/ntdll/signal_ppc64.c, which swap the guest pointer for a trampoline
 * from the pool before native code ever sees it.  This file proves the rows
 * are RIGHT, not merely present, because two of them could be present and
 * still wrong in ways that do not crash:
 *
 * ARITY, and this is the sharp one.  _invalid_parameter_handler takes FIVE
 * arguments (include/msvcrt/stdlib.h:263) and dlls/msvcrt/errno.c:473 calls
 * it with all five.  The trampoline pool writes the guest target into the
 * register ONE PAST the last real argument -- r7 at a four-argument slot,
 * which is precisely ELFv2's fifth argument register, the one native
 * _invalid_parameter has already loaded with pReserved.  So a row minted at
 * the pool's default arity does not fault: the handler is called with the
 * target pointer where its fifth argument should be, and reads a plausible
 * wrong number.  Step 3 passes a pReserved with bits set across both halves (and step 5
 * does it again through the CACHED call site)
 * and requires it back exactly, which is the only way that mistake is
 * visible from inside a guest.
 *
 * RETURN WIDTH.  Four of the five callbacks return void, so nothing reads
 * the result.  __setusermatherr's does not: dlls/msvcrt/math.c:129 reads it
 * as "I handled it" and only then honours the retval the handler wrote.
 * Steps 7 and 8 run the same math error twice with the handler returning 1
 * and then 0, and require the two to produce DIFFERENT answers -- which is
 * the only way to prove the return value is being read at all rather than
 * the handler merely running.
 *
 * HOW IT IS RUN.  One source, built twice and run twice, exactly as
 * ppc64le/syscom/com_smoke.c is: once as a native ppc64 Windows PE, where
 * there is no boundary and no trampoline anywhere in the process, and once
 * as an x86-64 guest PE under the emulator.  Both transcripts must be BYTE
 * IDENTICAL.  Nothing here prints an address, a handle or a thread id, so
 * every line is a value the CRT computed and two identical transcripts mean
 * the guest reached the same implementation with nothing lost or clobbered
 * on the way.  The native leg is not decoration: it is what says the values
 * this file asserts are the C runtime's own answers rather than this
 * probe's opinion of them.
 *
 * TWO MODES, because _purecall does not return.  dlls/msvcrt/exit.c:505 calls
 * the handler and then _amsg_exit(25), so the pure-virtual leg can only ever
 * be the last thing a process does.  Rather than let that truncate the other
 * steps -- or depend on _amsg_exit's message and exit status, which are not
 * this gate's to pin -- the probe takes a mode on its command line and the
 * runner invokes it twice.  In "purecall" mode the handler prints its own
 * witness and calls ExitProcess(CRT_PURECALL_EXIT) itself, so the exit status
 * is a value only the guest handler running can produce.
 *
 * NO CRT STARTUP.  Like every other guest probe here the image entry point
 * IS the program (-nostdlib), and the CRT entry points this file calls are
 * ordinary DLL imports.  That matters for the native leg too: it links
 * libmsvcr100.a directly, so both legs reach the same msvcr100 and neither
 * runs a CRT startup that might register handlers of its own behind this
 * probe's back.
 */

#include <windows.h>
#include <math.h>
#include <process.h>
#include <stdlib.h>

/* _purecall is exported by every one of these runtimes and declared by NONE
 * of the public headers: the only declaration in the tree is Wine's own
 * internal dlls/msvcrt/msvcrt.h:234, which is where msvcr100.thunks picks it
 * up from via PROBE-EXTRA msvcrt.h so the oracle can emit a thunk for it.
 * This probe is an ordinary consumer of the public headers -- that is the
 * whole reason the guest leg can compile against them at all -- so it
 * declares the one prototype it needs rather than reaching into Wine's
 * internals.  The shape is not a guess: it is that line, copied.  Both legs
 * use this declaration, so a mismatch would break them identically rather
 * than silently differ. */
void __cdecl _purecall(void);

/* The pReserved this probe hands _invalid_parameter.  Bits are set in BOTH
 * halves on purpose: a four-argument trampoline replaces this whole value
 * with the guest target's address, and a truncating path would keep only the
 * low half, so neither mistake can coincide with it. */
#define CRT_PRESERVED   0x00c0ffee0badcafeull

/* The line number, chosen to be nothing a compiler or a header could
 * plausibly produce on its own. */
#define CRT_LINE        12345u

/* The SECOND registration's values, different from the first in both fields so
 * that a witness left over from the first invocation cannot satisfy the second.
 * See the cache-hit steps below for why there is a second registration at all. */
#define CRT_LINE2       54321u
#define CRT_PRESERVED2  0x0badf00dc0ffee11ull

/* The value the matherr handler writes into e->retval when it claims the
 * error.  Exactly representable in a double, and nowhere near any NaN. */
#define CRT_MATH_RETVAL 42.0

/* What _beginthreadex's start routine returns, read back through
 * GetExitCodeThread.  Bit 31 is SET, so a path that sign-extends the 32-bit
 * result and then truncates it back is still correct here, while a path that
 * loses the value entirely is not. */
#define CRT_THREAD_EXIT 0xc0ffee01u

/* Exit status of the purecall-mode run: only the guest handler running can
 * produce it. */
#define CRT_PURECALL_EXIT 77

/* ------------------------------------------------------------- output
 *
 * Identical in spirit to guest_callbacks.c's helpers, and duplicated for the
 * same reason that file gives: two gates that should be able to fail
 * independently must not share a source file.  Nothing here prints an
 * address; see the header comment on why the two transcripts have to match
 * byte for byte.
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

/* A double, printed as its exact IEEE-754 bit pattern rather than decimal.
 * There is no CRT formatting here to call, and a bit pattern is the stronger
 * check anyway: it distinguishes 42.0 from a NaN, and one NaN from another,
 * which is exactly the difference steps 5 and 6 turn on. */
static void out_double_bits( double d )
{
    ULONGLONG bits;

    __builtin_memcpy( &bits, &d, sizeof(bits) );
    out_hex( bits, 16 );
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

static BOOL str_eq_a( const char *a, const char *b )
{
    while (*a && *a == *b) { a++; b++; }
    return *a == *b;
}

/* =======================================================================
 *  the five callbacks
 * ======================================================================= */

/* ---- _set_invalid_parameter_handler ---------------------------------- */

static const wchar_t crt_expr[] = L"crt_callbacks_expression";
static const wchar_t crt_func[] = L"crt_callbacks_function";
static const wchar_t crt_file[] = L"crt_callbacks_file";

static int        ip_calls;
static BOOL       ip_expr_ok, ip_func_ok, ip_file_ok;
static unsigned   ip_line;
static ULONGLONG  ip_reserved;

/* Five arguments.  The pointers are compared by IDENTITY rather than by
 * content: guest memory IS host memory on this port, so the CRT hands the
 * handler back the very pointers this probe passed in, and identity is the
 * stronger statement -- it rules out a copy made at the wrong width. */
static void __cdecl crt_invalid_parameter( const wchar_t *expr, const wchar_t *func,
                                           const wchar_t *file, unsigned line,
                                           uintptr_t reserved )
{
    ip_calls++;
    ip_expr_ok  = (expr == crt_expr);
    ip_func_ok  = (func == crt_func);
    ip_file_ok  = (file == crt_file);
    ip_line     = line;
    ip_reserved = (ULONGLONG)reserved;
}

/* The SECOND invalid-parameter handler.  A DISTINCT function, because the
 * trampoline pool keys a slot on (target, width, arity) and re-registering the
 * same guest function would be idempotent -- it would hand back the slot the
 * first registration already minted and mint nothing new, which is exactly the
 * path this test must avoid. */
static int        ip2_calls;
static BOOL       ip2_expr_ok, ip2_func_ok, ip2_file_ok;
static unsigned   ip2_line;
static ULONGLONG  ip2_reserved;

static void __cdecl crt_invalid_parameter2( const wchar_t *expr, const wchar_t *func,
                                            const wchar_t *file, unsigned line,
                                            uintptr_t reserved )
{
    ip2_calls++;
    ip2_expr_ok  = (expr == crt_expr);
    ip2_func_ok  = (func == crt_func);
    ip2_file_ok  = (file == crt_file);
    ip2_line     = line;
    ip2_reserved = (ULONGLONG)reserved;
}

/* ---- __setusermatherr ------------------------------------------------ */

static int    mh_calls;
static int    mh_type;
static BOOL   mh_name_ok;
static double mh_arg1;
static BOOL   mh_claim;      /* what the handler returns -- flipped by step 6 */

static int __cdecl crt_matherr( struct _exception *e )
{
    mh_calls++;
    mh_type    = e->type;
    mh_name_ok = e->name && str_eq_a( e->name, "sqrt" );
    mh_arg1    = e->arg1;
    if (!mh_claim) return 0;         /* decline: math_error uses its own retval */
    e->retval = CRT_MATH_RETVAL;
    return 1;                        /* claim: math_error must return retval */
}

/* ---- _beginthreadex -------------------------------------------------- */

static void     *tex_arg_seen;
static HANDLE    tex_done;

static unsigned __stdcall crt_thread_ex( void *arg )
{
    tex_arg_seen = arg;
    SetEvent( tex_done );
    return CRT_THREAD_EXIT;
}

/* ---- _beginthread ---------------------------------------------------- */

static void     *tb_arg_seen;
static HANDLE    tb_done;

static void __cdecl crt_thread_plain( void *arg )
{
    tb_arg_seen = arg;
    SetEvent( tb_done );
}

/* ---- _set_purecall_handler ------------------------------------------- */

static void __cdecl crt_purecall( void )
{
    /* Reached only from native _purecall (dlls/msvcrt/exit.c:505).  Printing
     * from here is the witness: this text can only appear if native CRT code
     * successfully called back into guest code.  ExitProcess rather than a
     * return, because returning goes on to _amsg_exit(25) and this gate
     * should not depend on that function's message or status. */
    out( "purecall handler ran\n" );
    ExitProcess( CRT_PURECALL_EXIT );
}

/* =======================================================================
 *  modes
 * ======================================================================= */

/* The command line, without a CRT to have parsed it.  Only one thing is
 * being asked -- does the word "purecall" appear -- so this is a substring
 * search rather than an argv split. */
static BOOL cmdline_has( const wchar_t *needle )
{
    const wchar_t *hay = GetCommandLineW();
    int i;

    if (!hay) return FALSE;
    for (; *hay; hay++)
    {
        for (i = 0; needle[i] && hay[i] == needle[i]; i++) { }
        if (!needle[i]) return TRUE;
    }
    return FALSE;
}

static void purecall_mode( void )
{
    _purecall_handler prev;

    out( "crt_callbacks: purecall mode\n" );

    prev = _set_purecall_handler( crt_purecall );
    out( "registered" );
    out_yn( " previous_was_null", prev == NULL );
    out( "\n" );

    /* Never returns: either the handler runs and calls ExitProcess, or
     * native _purecall reaches _amsg_exit with no handler at all. */
    _purecall();

    out( "purecall returned, which it must not\n" );
    ExitProcess( 1 );
}

static void main_mode( void )
{
    _invalid_parameter_handler ip_prev, ip_read;
    volatile double neg = -1.0;      /* volatile: keep the compiler from
                                        folding sqrt(-1.0) at compile time and
                                        never calling the CRT at all */
    double r1, r2;
    uintptr_t th;
    unsigned tid = 0;
    DWORD code = 0, w;

    out( "crt_callbacks: start\n" );

    /* ---- 1: the invalid-parameter handler registers ------------------- */
    begin( "set_invalid_parameter_handler" );
    ip_prev = _set_invalid_parameter_handler( crt_invalid_parameter );
    out_yn( "previous_was_null", ip_prev == NULL );
    verdict( ip_prev == NULL, "a handler was already installed" );

    /* ---- 2: the readback is not the guest pointer --------------------- */
    /* _get_invalid_parameter_handler returns what was STORED, which on the
     * guest lane is the trampoline and on the native lane is the function
     * itself.  So the two legs cannot agree on the pointer and this step
     * prints no address: what both legs CAN agree on is that something
     * non-NULL was stored.  The identity difference is the mechanism, and
     * the runner checks that on the port's own trace instead. */
    begin( "get_invalid_parameter_handler" );
    ip_read = _get_invalid_parameter_handler();
    out_yn( "stored_non_null", ip_read != NULL );
    verdict( ip_read != NULL, "nothing was stored" );

    /* ---- 3: all five arguments arrive, pReserved included ------------- */
    begin( "invalid_parameter five arguments" );
    ip_calls = 0;
    _invalid_parameter( crt_expr, crt_func, crt_file, CRT_LINE, (uintptr_t)CRT_PRESERVED );
    out( "calls=" );        out_dec( ip_calls );
    out_yn( " expr", ip_expr_ok );
    out_yn( " func", ip_func_ok );
    out_yn( " file", ip_file_ok );
    out( " line=" );        out_dec( ip_line );
    out( " reserved=" );    out_hex( ip_reserved, 16 );
    verdict( ip_calls == 1 && ip_expr_ok && ip_func_ok && ip_file_ok &&
             ip_line == CRT_LINE && ip_reserved == CRT_PRESERVED,
             "the handler did not receive exactly what was passed" );

    /* ---- 4-5: THE SAME CALL SITE A SECOND TIME -- A THUNK-CACHE HIT ------
     *
     * Steps 1-3 registered once, which means the trap at
     * _set_invalid_parameter_handler's stub took the thunk cache's MISS path
     * and nothing else.  That is half the mechanism, and it is the half that
     * hid a real defect: cb_argc was threaded through find_guest_thunk_target
     * but NOT through thunk_rip_cache_get/put, the two functions that actually
     * copy the entry field by field.  So the arity written on a miss was
     * dropped on the floor, and a HIT handed the caller an uninitialised
     * `hit.cb_argc` -- stack garbage.  Garbage outside the pool's supported
     * arities ({4,5,6} at the time; 4 through 9 today) takes
     * wrap_guest_callback_ex's default: arm, which logs "unsupported arity"
     * and RETURNS THE RAW GUEST POINTER to native code, which is precisely the
     * failure this whole mechanism exists to prevent; garbage that lands on
     * another supported arity mints the wrong trampoline silently.
     *
     * A gate that registers once cannot see any of that.  So: register a
     * SECOND, DIFFERENT guest handler through the SAME export -- the same RIP,
     * therefore a cache hit -- and require it to receive all five arguments
     * with pReserved intact, exactly as the first one did.  The values differ
     * from step 3's in both the line and the reserved word, so a stale witness
     * cannot satisfy this.
     *
     * The handler must be a different FUNCTION, not the same one re-registered:
     * the pool is idempotent per (target, width, arity), so re-registering the
     * first handler would return the slot already minted and exercise nothing.
     */
    begin( "second registration through the same call site (cache hit)" );
    {
        _invalid_parameter_handler prev2 =
            _set_invalid_parameter_handler( crt_invalid_parameter2 );

        /* Only that SOMETHING was installed before, never its value: on the
         * native leg that is the first handler itself and on the guest leg it
         * is that handler's trampoline, so the two legs cannot agree on an
         * address and this transcript has to stay diffable. */
        out_yn( "previous_non_null", prev2 != NULL );
        verdict( prev2 != NULL,
                 "the second registration did not see the first one installed" );
    }

    /* ---- 5: and the second handler gets all five arguments too ---------- */
    begin( "invalid_parameter five arguments, through the cached site" );
    ip2_calls = 0;
    _invalid_parameter( crt_expr, crt_func, crt_file, CRT_LINE2,
                        (uintptr_t)CRT_PRESERVED2 );
    out( "calls=" );        out_dec( ip2_calls );
    out_yn( " expr", ip2_expr_ok );
    out_yn( " func", ip2_func_ok );
    out_yn( " file", ip2_file_ok );
    out( " line=" );        out_dec( ip2_line );
    out( " reserved=" );    out_hex( ip2_reserved, 16 );
    verdict( ip2_calls == 1 && ip2_expr_ok && ip2_func_ok && ip2_file_ok &&
             ip2_line == CRT_LINE2 && ip2_reserved == CRT_PRESERVED2,
             "the handler registered through the CACHED call site did not "
             "receive exactly what was passed -- cb_argc did not survive the "
             "thunk cache" );

    /* ---- 6: the math handler registers -------------------------------- */
    /* __setusermatherr returns void, so there is nothing to check here but
     * that the call itself completes; the proof is steps 5 and 6. */
    begin( "setusermatherr" );
    mh_calls = 0;
    mh_claim = TRUE;
    __setusermatherr( crt_matherr );
    out( "registered" );
    verdict( TRUE, "" );

    /* ---- 7: the handler runs, and its CLAIM is honoured ---------------- */
    begin( "matherr claims the error" );
    r1 = sqrt( neg );
    out( "calls=" );      out_dec( mh_calls );
    out( " type=" );      out_dec( (ULONG)mh_type );
    out_yn( " name_sqrt", mh_name_ok );
    out( " arg1=" );      out_double_bits( mh_arg1 );
    out( " result=" );    out_double_bits( r1 );
    verdict( mh_calls == 1 && mh_type == _DOMAIN && mh_name_ok &&
             mh_arg1 == -1.0 && r1 == CRT_MATH_RETVAL,
             "the handler did not run, or its retval was not honoured" );

    /* ---- 8: the handler DECLINES, and that is honoured too ------------- */
    /* The same call with the handler returning 0.  If the int return were
     * being dropped -- or arriving as a constant -- this would print the same
     * result as step 5.  It must not: math_error falls through to its own
     * NaN.  This is the step that proves the RETURN is read, which is why
     * __setusermatherr's row takes the sign-extending 32-bit slot rather than
     * the wide one. */
    begin( "matherr declines the error" );
    mh_calls = 0;
    mh_claim = FALSE;
    r2 = sqrt( neg );
    out( "calls=" );      out_dec( mh_calls );
    out( " result=" );    out_double_bits( r2 );
    out_yn( " differs_from_claimed", r2 != CRT_MATH_RETVAL );
    verdict( mh_calls == 1 && r2 != CRT_MATH_RETVAL,
             "declining produced the same answer as claiming; the return is not being read" );

    /* ---- 9: _beginthreadex's start routine ---------------------------- */
    /* The argument is checked by identity for the same reason step 3's
     * pointers are, and the thread's exit code is read back because that is
     * the one place _beginthreadex's 32-bit return actually surfaces:
     * dlls/msvcrt/thread.c:201 hands it to _endthreadex. */
    begin( "beginthreadex start routine" );
    tex_done = CreateEventW( NULL, TRUE, FALSE, NULL );
    if (!tex_done)
    {
        verdict( FALSE, "CreateEventW failed" );
    }
    else
    {
        th = _beginthreadex( NULL, 0, crt_thread_ex, (void *)&tex_arg_seen, 0, &tid );
        w = th ? WaitForSingleObject( (HANDLE)th, 10000 ) : WAIT_FAILED;
        if (th) GetExitCodeThread( (HANDLE)th, &code );
        out_yn( "started", th != 0 );
        out_yn( " signalled", w == WAIT_OBJECT_0 );
        out_yn( " arg", tex_arg_seen == (void *)&tex_arg_seen );
        out( " exit=" );  out_hex( code, 8 );
        out_yn( " tid_non_zero", tid != 0 );
        verdict( th != 0 && w == WAIT_OBJECT_0 &&
                 tex_arg_seen == (void *)&tex_arg_seen &&
                 code == CRT_THREAD_EXIT && tid != 0,
                 "the start routine did not run, or its argument or exit code was wrong" );
        if (th) CloseHandle( (HANDLE)th );
    }

    /* ---- 10: _beginthread's start routine ----------------------------- */
    /* void return and the CRT owns the handle, so there is no exit code to
     * read here and the event is the whole synchronisation.  What this step
     * proves that step 7 does not is the OTHER start-routine shape: a
     * different argument position (0 rather than 2) and a routine that
     * returns nothing at all. */
    begin( "beginthread start routine" );
    tb_done = CreateEventW( NULL, TRUE, FALSE, NULL );
    if (!tb_done)
    {
        verdict( FALSE, "CreateEventW failed" );
    }
    else
    {
        th = _beginthread( crt_thread_plain, 0, (void *)&tb_arg_seen );
        w = (th != (uintptr_t)-1) ? WaitForSingleObject( tb_done, 10000 ) : WAIT_FAILED;
        out_yn( "started", th != (uintptr_t)-1 );
        out_yn( " signalled", w == WAIT_OBJECT_0 );
        out_yn( " arg", tb_arg_seen == (void *)&tb_arg_seen );
        verdict( th != (uintptr_t)-1 && w == WAIT_OBJECT_0 &&
                 tb_arg_seen == (void *)&tb_arg_seen,
                 "the start routine did not run, or its argument was wrong" );
    }

    /* ---- 11: the purecall handler registers --------------------------- */
    /* Registration only.  Invoking it ends the process, which is what the
     * separate purecall mode is for; this step is here so that the readback
     * contract is checked in the same transcript as the other four. */
    begin( "set_purecall_handler" );
    {
        _purecall_handler pc_prev = _set_purecall_handler( crt_purecall );
        _purecall_handler pc_read = _get_purecall_handler();

        out_yn( "previous_was_null", pc_prev == NULL );
        out_yn( " stored_non_null", pc_read != NULL );
        verdict( pc_prev == NULL && pc_read != NULL,
                 "the purecall handler did not register cleanly" );
    }

    out( failures ? "crt_callbacks: FAIL " : "crt_callbacks: PASS " );
    out_dec( (ULONG)step );
    out( " steps, " );
    out_dec( (ULONG)failures );
    out( " failed\n" );
}

/* Both legs read the mode from GetCommandLineW rather than from argv: the
 * native leg is a native ppc64 WINDOWS PE run under this tree's own wine, so
 * the Win32 call is available to it exactly as it is to the guest, and one
 * code path means the two transcripts cannot diverge on argument parsing. */
static int crt_callbacks_run( void )
{
    if (cmdline_has( L"purecall" )) purecall_mode();   /* never returns */
    main_mode();
    return failures ? 1 : 0;
}

#ifdef CRT_CALLBACKS_NO_CRT
/* The guest build has no C runtime: this IS the image entry point. */
void WINAPI crt_callbacks_entry( void )
{
    ExitProcess( (UINT)crt_callbacks_run() );
}
#else
int main( void )
{
    return crt_callbacks_run();
}
#endif
