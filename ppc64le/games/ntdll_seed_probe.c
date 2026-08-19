/*
 * ntdll_seed_probe -- the guest ntdll namespace-seed gate.
 *
 * WHAT THIS REPRODUCES.  SteamStub v3.1's anti-debug prologue -- the DRM
 * wrapper every Steam release of Skyrim Special Edition and Warhammer
 * 40,000: Boltgun carries, and the wrapper this port's CATALOG.md names as
 * "the highest-value single item" the game-testing pass produced -- runs this
 * sequence before a single byte of the wrapped game executes:
 *
 *     h  = GetModuleHandleA("ntdll.dll");
 *     fn = GetProcAddress(h, "NtSetInformationThread");
 *     fn(GetCurrentThread(), ThreadHideFromDebugger, NULL, 0);
 *
 * and does not check either return value before using it.  On real Windows
 * this is safe because ntdll.dll is mapped into every process by the OS
 * loader itself, before any user code runs, so GetModuleHandleA("ntdll.dll")
 * cannot fail there.  Before the fix this port's guest module namespace held
 * only whatever a guest image's own static imports dragged in, and nothing
 * imports ntdll.dll -- guest code reaches it only through KERNEL32 forwards
 * -- so GetModuleHandleA answered NULL, GetProcAddress on a NULL module
 * (correctly) answered NULL too, and the third line called through a NULL
 * function pointer: a c0000005 EXECUTE_FAULT at address 0, at the image
 * entry, before one instruction of the actual game ran.  See CATALOG.md's
 * "The Elder Scrolls V: Skyrim Special Edition" entry and Handoff #1 for the
 * full trace this file was written from.
 *
 * The fix -- loader_init() in dlls/ntdll/loader.c -- seeds the guest module
 * namespace with ntdll.dll once, at guest-process bringup, through the
 * existing load_guest_dll(), before any guest instruction runs.  It does not
 * touch find_guest_module() or GetModuleHandle: GetModuleHandle still never
 * loads anything, it just now finds a module that was already there, exactly
 * as real Windows guarantees.
 *
 * TWO BUILDS FROM ONE SOURCE.
 *
 *   default            checked: verifies each return value before using it,
 *                       and reports a PASS/FAIL transcript like every other
 *                       gate in this tree.  Never crashes on purpose.  Built
 *                       for both the x86-64 guest PE and, with a CRT, a
 *                       native ppc64 PE -- corroboration that the transcript
 *                       describes real, achievable Windows semantics and not
 *                       something guest-specific.
 *
 *   NTDLL_SEED_PROBE_BLIND
 *                       the LITERAL SteamStub sequence: no NULL check before
 *                       the final call, exactly like the real DRM stub.  This
 *                       is the faithful reproduction.  Un-sabotaged, it must
 *                       print the same PASS the checked build does -- proof
 *                       that the exact unguarded sequence real Steam builds
 *                       run is now safe.  Under --sabotage
 *                       (WINEEMUNOGUESTNTDLLSEED=1, dlls/ntdll/loader.c) it
 *                       must reproduce the documented crash byte-for-byte:
 *                       c0000005, EXECUTE_FAULT (info[0]=8, info[1]=0), at
 *                       address 0 -- the exact wall Skyrim and Boltgun hit
 *                       before this fix existed.
 *
 * NO C RUNTIME on the guest side (-DNTDLL_SEED_PROBE_NO_CRT): the program
 * formats its own output and writes it with WriteFile, and deliberately does
 * NOT import ntdll.dll -- the whole point under test is that nothing has to.
 * A static import of ntdll would drag it into the guest namespace on its own
 * and the probe would pass for the wrong reason, fix or no fix.
 *
 * Copyright 2026 the ppc64le port authors
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 */

#include <windows.h>
#include <winternl.h>
#include <ntstatus.h>

typedef NTSTATUS (WINAPI *PNtSetInformationThread)( HANDLE, THREADINFOCLASS, LPCVOID, ULONG );

/* ------------------------------------------------------------- output */
/* Same shape as ppc64le/syscom/com_smoke.c and ppc64le/seh/seh_smoke.c: no
 * printf, no CRT, one syscall (WriteFile) per line so output already reached
 * the pipe before the blind build's final call has any chance to crash. */

static void out( const char *s )
{
    DWORD n = 0, written;

    while (s[n]) n++;
    WriteFile( GetStdHandle( STD_OUTPUT_HANDLE ), s, n, &written, NULL );
}

static void out_hex( ULONG64 v, int digits )
{
    static const char hex[] = "0123456789ABCDEF";
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

/* ------------------------------------------------------------- the run */

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

static int ntdll_seed_probe_run( void )
{
    HMODULE ntdll;
    PNtSetInformationThread fn;
    NTSTATUS status;

    out( "ntdll_seed_probe: start\n" );

    /* Step 1 and 2 are safe to run unconditionally in EITHER build: a NULL
     * module handed to GetProcAddress is refused by name
     * (dlls/ntdll/signal_ppc64.c emu_GetProcAddress) rather than dereferenced,
     * exactly as the real pre-fix log shows --
     * "GetProcAddress(0000000000000000) is not a guest module, refusing".
     * Only the final CALL through whatever GetProcAddress handed back can
     * crash, so only that step differs between the checked and blind
     * builds. */
    begin( "GetModuleHandleA(\"ntdll.dll\")" );
    ntdll = GetModuleHandleA( "ntdll.dll" );
    out( "handle=0x" );
    out_hex( (ULONG_PTR)ntdll, 16 );
    verdict( ntdll != NULL, "NULL -- the guest module namespace has no ntdll seeded" );

    begin( "GetProcAddress(ntdll, \"NtSetInformationThread\")" );
    fn = (PNtSetInformationThread)GetProcAddress( ntdll, "NtSetInformationThread" );
    out( "proc=0x" );
    out_hex( (ULONG_PTR)fn, 16 );
    verdict( fn != NULL, "NULL -- no callable NtSetInformationThread stub" );

    begin( "NtSetInformationThread(GetCurrentThread(), ThreadHideFromDebugger, NULL, 0)" );
#ifdef NTDLL_SEED_PROBE_BLIND
    /* The literal SteamStub sequence: called without checking fn for NULL,
     * exactly as the real DRM stub does.  If the seed did not take, this is
     * where the process dies -- nothing after this line ever runs, and
     * everything printed above it already reached the pipe. */
    status = fn( GetCurrentThread(), ThreadHideFromDebugger, NULL, 0 );
    out( "status=0x" );
    out_hex( (ULONG)status, 8 );
    verdict( status == STATUS_SUCCESS, "not STATUS_SUCCESS" );
#else
    if (fn)
    {
        status = fn( GetCurrentThread(), ThreadHideFromDebugger, NULL, 0 );
        out( "status=0x" );
        out_hex( (ULONG)status, 8 );
        verdict( status == STATUS_SUCCESS, "not STATUS_SUCCESS" );
    }
    else verdict( FALSE, "skipped -- GetProcAddress returned no callable stub" );
#endif

    out( failures ? "ntdll_seed_probe: FAIL " : "ntdll_seed_probe: PASS " );
    out_dec( (ULONG)(step - failures) );
    out( "/" );
    out_dec( (ULONG)step );
    out( "\n" );
    return failures ? 1 : 0;
}

#ifdef NTDLL_SEED_PROBE_NO_CRT
/* The guest build has no C runtime: this IS the image entry point. */
void WINAPI ntdll_seed_probe_entry( void )
{
    ExitProcess( (UINT)ntdll_seed_probe_run() );
}
#else
int main( void )
{
    return ntdll_seed_probe_run();
}
#endif
