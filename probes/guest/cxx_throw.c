/*
 * cxx_throw -- the guest-side probe for _CxxThrowException (guest-cxx-eh-
 * plan.md, Session A).
 *
 * WHAT THIS PROVES, AND HOW.  Every step below checks a VALUE against a
 * value known before the probe ever ran, exactly the discipline
 * ppc64le/seh/check-seh-handlers.sh and check-fp-marshal.sh already hold
 * this tree to: "it did not crash" is not a passing result anywhere in this
 * file.
 *
 *   1-2  _CxxThrowException resolves at RUN TIME through BOTH forwarder
 *        chains a real title can take: LoadLibraryA("vcruntime140.dll") +
 *        GetProcAddress (vcruntime140 -> ucrtbase -> guestcrt, the chain a
 *        builtin-only run takes) and LoadLibraryA("ucrtbase.dll") +
 *        GetProcAddress (the shorter hop a prefix-staged Proton
 *        vcruntime140 -- itself a forward straight into ucrtbase -- takes).
 *        Neither pointer may be NULL, and neither may be a missing-import
 *        SENTINEL (dlls/ntdll/loader.c's 0xdead0000+n, a small absolute
 *        address with no real page behind it -- a guest that actually
 *        CALLED one would fault, not return, so the address itself is what
 *        is checked).
 *   3    Both chains resolve to the SAME address: the whole point of
 *        keeping the spec's own vcruntime140->ucrtbase hop (rather than a
 *        shortcut straight to guestcrt) is that the two mixes -- builtin-
 *        only and Proton-staged -- resolve identically.  This is the
 *        chain's proof, in bytes, that the plan's forward-following claim
 *        is what actually happens at load, not merely what the .thunks
 *        files say it should do.
 *   4-8  A normal throw: __try around a call through the resolved pointer
 *        with a known object pointer and a known "ThrowInfo" pointer.  The
 *        filter -- an ordinary clang-compiled __except, so its .xdata names
 *        __C_specific_handler, imported here from VCRUNTIME140.dll (see
 *        cxx_throw.def) to also exercise guest-cxx-eh-plan.md section 1
 *        item 2's identity-path fix -- copies the EXCEPTION_RECORD and lets
 *        the checks run afterward, each against a compile-time constant:
 *        ExceptionCode == 0xe06d7363, EXCEPTION_NONCONTINUABLE set,
 *        NumberParameters == 4, ExceptionInformation[0] == 0x19930520 (the
 *        VC6 magic -- SABOTAGE below flips this one), [1] == &g_object,
 *        [2] == &g_throwinfo, [3] == this module's own base address (read
 *        with GetModuleHandleA(NULL), which is exactly what
 *        RtlPcToFileHeader(&g_throwinfo,...) must also answer since
 *        g_throwinfo lives in THIS image).
 *   9-10 The `throw;` (rethrow) spelling: _CxxThrowException(NULL, NULL)
 *        must arrive with ExceptionInformation[1] == [2] == 0, the encoding
 *        every real handler keys "this is a bare rethrow" on.
 *
 * --sabotage rebuilds with -DSABOTAGE, which changes ONLY the value step 7
 * COMPARES against (the real throw is untouched) -- proving the checks can
 * actually fail rather than rubber-stamping whatever the guest does.
 *
 * NO C RUNTIME (matching seh_smoke.c/seh_handlers.c): this file formats its
 * own decimal/hex output and writes it with WriteFile, and cxx_throw_entry
 * IS the image entry point.
 *
 * Copyright 2026 the ppc64le port authors
 *
 * This library is free software; you can redistribute it and/or modify it
 * under the terms of the GNU Lesser General Public License as published by
 * the Free Software Foundation; either version 2.1 of the License, or (at
 * your option) any later version.
 */

#include <stdarg.h>

#include <windef.h>
#include <winbase.h>
#include <wine/exception.h>

typedef void (WINAPI *pCxxThrowException)( void *object, void *throwinfo );

#define CXX_EXCEPTION_CODE   0xe06d7363u
#define CXX_FRAME_MAGIC_VC6  0x19930520u

#ifdef SABOTAGE
/* The one deliberately-wrong expectation: the real throw still writes the
 * true magic, but step 7 below is told to expect a value one higher, so a
 * correct port makes this build FAIL -- the gate is only green if the
 * --sabotage run does NOT pass. */
#define CXX_FRAME_MAGIC_WANT (CXX_FRAME_MAGIC_VC6 + 1)
#else
#define CXX_FRAME_MAGIC_WANT CXX_FRAME_MAGIC_VC6
#endif

/* ------------------------------------------------------------- output */

static void out( const char *s )
{
    DWORD written;
    DWORD n = 0;
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

/* ------------------------------------------------------------- the throw
 * object and "ThrowInfo".  Their CONTENTS are never read by this probe or
 * by guestcrt's _CxxThrowException (it is deliberately opaque past the
 * pointer, see cxxthrow.c) -- only their ADDRESSES matter, as the values
 * args[1] and args[2] of the exception record must carry unchanged. */
static char g_object[4] = { 0x11, 0x22, 0x33, 0x44 };
static const char g_throwinfo[4] = { 0x55, 0x66, 0x77, 0x88 };

/* Only the fields this probe actually checks, copied one at a time rather
 * than `g_rec1 = *ep->ExceptionRecord` -- a whole-EXCEPTION_RECORD struct
 * copy is wide enough that clang lowers it to a memcpy() call, and this
 * freestanding guest build (-fno-builtin, -nostdlib, no CRT) has none. */
typedef struct
{
    DWORD    ExceptionCode;
    DWORD    ExceptionFlags;
    DWORD    NumberParameters;
    ULONG_PTR ExceptionInformation[EXCEPTION_MAXIMUM_PARAMETERS];
} cxx_rec_snapshot;

static cxx_rec_snapshot g_rec1, g_rec2;
static BOOL g_filter1_ran, g_filter2_ran;

static void snapshot( cxx_rec_snapshot *dst, const EXCEPTION_RECORD *rec )
{
    DWORD i;

    dst->ExceptionCode = rec->ExceptionCode;
    dst->ExceptionFlags = rec->ExceptionFlags;
    dst->NumberParameters = rec->NumberParameters;
    for (i = 0; i < EXCEPTION_MAXIMUM_PARAMETERS; i++)
        dst->ExceptionInformation[i] =
            (i < rec->NumberParameters) ? rec->ExceptionInformation[i] : 0;
}

static LONG CALLBACK cxx_filter1( PEXCEPTION_POINTERS ep )
{
    g_filter1_ran = TRUE;
    snapshot( &g_rec1, ep->ExceptionRecord );
    return EXCEPTION_EXECUTE_HANDLER;
}

static LONG CALLBACK cxx_filter2( PEXCEPTION_POINTERS ep )
{
    g_filter2_ran = TRUE;
    snapshot( &g_rec2, ep->ExceptionRecord );
    return EXCEPTION_EXECUTE_HANDLER;
}

/* An indirect call through volatile storage, matching seh_smoke.c/
 * seh_handlers.c: the target is resolved at run time (GetProcAddress), so
 * this is not merely defensive here the way it is for a raw memory fault --
 * but keeping the same shape as the rest of this tree's guest probes costs
 * nothing and rules out a future clang deciding otherwise. */
static volatile pCxxThrowException g_throw;

static void call_normal_throw( void )
{
    __TRY
    {
        g_throw( g_object, (void *)&g_throwinfo );
        out( "cxx_throw: FAIL the normal throw call RETURNED\n" );
        failures++;
    }
    __EXCEPT( cxx_filter1 )
    {
    }
    __ENDTRY
}

static void call_rethrow_spelling( void )
{
    __TRY
    {
        g_throw( NULL, NULL );
        out( "cxx_throw: FAIL the rethrow-spelling call RETURNED\n" );
        failures++;
    }
    __EXCEPT( cxx_filter2 )
    {
    }
    __ENDTRY
}

/* ------------------------------------------------------------- the run */

static int cxx_throw_run( void )
{
    HMODULE hvc, huc, hself;
    pCxxThrowException pfn_vc, pfn_uc;

    /* step 1: vcruntime140 -> ucrtbase -> guestcrt.  The resolved address
     * itself is never printed -- it is a real load address and varies run
     * to run, and this transcript is meant to be byte-comparable across
     * runs the way check-seh-smoke.sh's is; only the PASS/FAIL verdict,
     * which does not depend on the address's value, is deterministic. */
    hvc = LoadLibraryA( "vcruntime140.dll" );
    pfn_vc = (pCxxThrowException)GetProcAddress( hvc, "_CxxThrowException" );
    begin( "resolve _CxxThrowException via vcruntime140.dll" );
    verdict( hvc != NULL && pfn_vc != NULL
             && ((ULONG_PTR)pfn_vc & 0xffff0000ull) != 0xdead0000ull,
             "NULL or a missing-import sentinel" );

    /* step 2: ucrtbase -> guestcrt directly, the hop a prefix-staged Proton
     * vcruntime140 (itself a forward straight into ucrtbase) takes */
    huc = LoadLibraryA( "ucrtbase.dll" );
    pfn_uc = (pCxxThrowException)GetProcAddress( huc, "_CxxThrowException" );
    begin( "resolve _CxxThrowException via ucrtbase.dll" );
    verdict( huc != NULL && pfn_uc != NULL
             && ((ULONG_PTR)pfn_uc & 0xffff0000ull) != 0xdead0000ull,
             "NULL or a missing-import sentinel" );

    /* step 3: both chains land on the SAME guestcrt code */
    begin( "both forwarder chains resolve to the same address" );
    verdict( pfn_vc == pfn_uc, "vcruntime140.dll and ucrtbase.dll disagree" );

    g_throw = pfn_vc;

    /* steps 4-8: the normal throw */
    call_normal_throw();

    begin( "normal throw: the private filter actually ran" );
    verdict( g_filter1_ran, "__except's filter was never entered" );

    begin( "normal throw: ExceptionCode" );
    out_hex( g_rec1.ExceptionCode, 8 );
    verdict( g_rec1.ExceptionCode == CXX_EXCEPTION_CODE, "not 0xe06d7363" );

    begin( "normal throw: EXCEPTION_NONCONTINUABLE is set" );
    verdict( (g_rec1.ExceptionFlags & EXCEPTION_NONCONTINUABLE) != 0,
             "flag missing" );

    begin( "normal throw: NumberParameters" );
    out_dec( g_rec1.NumberParameters );
    verdict( g_rec1.NumberParameters == 4, "not 4" );

    begin( "normal throw: ExceptionInformation[0] (CXX_FRAME_MAGIC_VC6)" );
    out_hex( g_rec1.ExceptionInformation[0], 8 );
    verdict( g_rec1.NumberParameters >= 1
             && g_rec1.ExceptionInformation[0] == CXX_FRAME_MAGIC_WANT,
             "wrong magic" );

    begin( "normal throw: ExceptionInformation[1] == &g_object" );
    verdict( g_rec1.NumberParameters >= 2
             && g_rec1.ExceptionInformation[1] == (ULONG_PTR)g_object,
             "object pointer not carried through" );

    begin( "normal throw: ExceptionInformation[2] == &g_throwinfo" );
    verdict( g_rec1.NumberParameters >= 3
             && g_rec1.ExceptionInformation[2] == (ULONG_PTR)&g_throwinfo,
             "throwinfo pointer not carried through" );

    hself = GetModuleHandleA( NULL );
    begin( "normal throw: ExceptionInformation[3] == this module's base" );
    verdict( g_rec1.NumberParameters >= 4
             && g_rec1.ExceptionInformation[3] == (ULONG_PTR)hself,
             "not RtlPcToFileHeader(&g_throwinfo) == this module's base" );

    /* steps 9-10: `throw;` (rethrow) spelling -- object and type both NULL */
    call_rethrow_spelling();

    begin( "rethrow spelling: the private filter actually ran" );
    verdict( g_filter2_ran, "__except's filter was never entered" );

    begin( "rethrow spelling: ExceptionInformation[1] == [2] == 0" );
    verdict( g_rec2.NumberParameters == 4
             && g_rec2.ExceptionInformation[1] == 0
             && g_rec2.ExceptionInformation[2] == 0,
             "object/throwinfo not both zero" );

    out( failures ? "cxx_throw: FAIL " : "cxx_throw: PASS " );
    out_dec( (ULONG)(step - failures) );
    out( "/" );
    out_dec( (ULONG)step );
    out( "\n" );
    return failures ? 1 : 0;
}

/* ------------------------------------------------------------- the negative control
 *
 * A throw with NO handler at all.  Nothing here may catch it, and the run
 * must reach the port's existing unhandled path and die promptly, loudly
 * and by name -- never a hang, never a silent exit 0.
 */
static int cxx_throw_unhandled( void )
{
    HMODULE huc = LoadLibraryA( "ucrtbase.dll" );
    pCxxThrowException pfn = (pCxxThrowException)
        GetProcAddress( huc, "_CxxThrowException" );

    out( "cxx_throw: unhandled probe, throwing outside any __try\n" );
    pfn( g_object, (void *)&g_throwinfo );
    out( "cxx_throw: FAIL the unhandled throw returned\n" );
    return 1;
}

void WINAPI cxx_throw_entry( void )
{
#ifdef CXX_THROW_UNHANDLED
    ExitProcess( (UINT)cxx_throw_unhandled() );
#else
    ExitProcess( (UINT)cxx_throw_run() );
#endif
}
