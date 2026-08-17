/*
 * ordinal_import -- a guest x86-64 PE that imports d3d12.dll BY ORDINAL.
 *
 * An import descriptor does not have to carry a name.  IMAGE_SNAP_BY_ORDINAL
 * says "bind whatever answers to number N in that module's export table", and
 * that is all the guest's loader is given: no string to fall back on, no way
 * to notice it got the wrong function, and no way to notice it got nothing at
 * all until the first CALL lands on ntdll's missing-import sentinel.
 *
 * Steam's own d3ddriverquery64.exe is built exactly that way -- its whole
 * d3d12.dll import descriptor is `Symbol: (101)` -- because Microsoft's
 * d3d12.dll has always exported D3D12CreateDevice at ordinal 101.  Under this
 * port it bound 0xdead0001 and died on the first call from
 * d3ddriverquery64.exe+0x108e, because the generated thunk PE's export table
 * was numbered 1..N in NAME order: tools/spec2thunk wrote a .def that listed
 * names only and let lld-link number them.  The ordinals a module publishes
 * are not the linker's to choose.
 *
 * WHAT THIS PROBE ASSERTS, and why each one is a value check rather than a
 * "did it load" check:
 *
 *   1  The ordinal-101 import bound to something that is neither NULL nor one
 *      of ntdll's per-binding sentinels (0xdead0000 + n, dlls/ntdll/loader.c
 *      allocate_stub).  A sentinel IS a plausible-looking non-NULL pointer, so
 *      "not NULL" alone would have passed the whole defect.
 *   2/3 The same module and the same function are reachable by NAME, through
 *      the guest namespace: GetModuleHandleW + GetProcAddress, both of which
 *      this port answers against the GUEST module list.
 *   4  THE POINT: the two are THE SAME ADDRESS.  Ordinal 101 must resolve to
 *      the very export the name resolves to -- not merely to some valid stub
 *      of the same module, which a table numbered 1..N in name order would
 *      also have produced (there, ordinal 101 would have been out of range,
 *      but a shifted-by-one table would not, and that is the failure a mere
 *      liveness check cannot see).
 *   5  An ordinal the module CANNOT serve must bind a sentinel, not silently
 *      alias some other export.  This probe imports 60001, far past d3d12's
 *      whole ordinal space, and requires the bound value to be in the sentinel
 *      range -- which is also what proves the holes left by every export the
 *      thunk generator refuses stay holes.
 *   6  (ORDINAL_PROBE_CALL_BOGUS build only) Calling that sentinel must KILL
 *      the process.  A gap that faults with a named symbol next to it in the
 *      log is a diagnosable defect; one that returns a plausible value is not.
 *   7  (ORDINAL_PROBE_CRASHING_FILTER build only) The same call, in the shape
 *      a real Steam launch has: with a top-level exception filter installed
 *      that FAULTS while reporting, which is what DOOM's crash reporter did
 *      and what turned a diagnosable death into an eight-hour hang.  The
 *      process must still die, promptly, with the sentinel named.  See
 *      crashing_filter() below.
 *
 * NO C RUNTIME, for the reason d3d11_smoke.c and com_smoke.c give: the program
 * formats its own output and writes it with WriteFile, so nothing but the
 * boundary under test can move a byte of the transcript.  The image entry
 * point IS the program.
 *
 * FALSIFICATION (drives check-ordinal-imports.sh --sabotage; both builds must
 * go RED, and neither needs Wine rebuilt):
 *
 *   -DORDINAL_PROBE_SABOTAGE=1  the import library gives the probe ordinal 102
 *      (D3D12GetDebugInterface) under the name it believes is 101.  Everything
 *      still binds, nothing is NULL, no sentinel appears -- and step 4 must
 *      FAIL, which is the whole claim: this gate compares VALUES, not liveness.
 *   -DORDINAL_PROBE_SABOTAGE=2  the "bogus" slot is given ordinal 101, a real
 *      export.  Step 5 must FAIL, proving the sentinel check is a check and
 *      not a formality.
 *
 * Both sabotage levers live in the .def files the gate writes, not in this
 * source; this file only names them so that the transcript says which one is
 * running.  See check-ordinal-imports.sh.
 *
 * Copyright 2026 the ppc64le port authors
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 */

#include <windows.h>

/* The two d3d12 entry points this probe binds.  __declspec(dllimport) is not
 * decoration here: without it, taking the address of an imported function
 * yields the LINKER'S jump thunk in this image's own .text, and the probe
 * would compare that thunk against GetProcAddress's answer and always
 * disagree.  With it, the address IS the IAT slot's contents -- exactly the
 * pointer the guest loader bound, which is what is under test.
 *
 * The declared shapes are honest but never called: nothing here calls
 * D3D12CreateDevice (creating a device is check-d3d11-smoke.sh's and the
 * vkd3d lane's business, not this file's), and the bogus one is called only
 * in the CALL_BOGUS build, where the call is expected to be fatal. */
__declspec(dllimport) HRESULT WINAPI D3D12CreateDevice( void *adapter, UINT min_feature_level,
                                                        REFIID iid, void **device );
__declspec(dllimport) HRESULT WINAPI ordprobe_bogus_ordinal( void );

#ifdef ORDINAL_PROBE_CRASHING_FILTER
/* The shape a real Steam launch has, and the reason step 6 is not enough on
 * its own.  DOOM (2016) installs a crash reporter with
 * SetUnhandledExceptionFilter and then, on 2026-08-17, called a
 * missing-import sentinel; its reporter FAULTED while reporting, and that
 * fault was another unhandled guest exception on the same thread, which
 * restarted the whole unhandled report -- 804,000 log lines and 8 MiB of
 * native stack later the thread was spinning at 0% holding a critical
 * section, and the process HUNG instead of dying.  See the guest_exc_raising
 * banner in dlls/ntdll/signal_ppc64.c.
 *
 * This filter reproduces that exactly: it is reached only because the process
 * is already dying, and the first thing it does is fault. */
static LONG WINAPI crashing_filter( EXCEPTION_POINTERS *info )
{
    *(volatile int *)(ULONG_PTR)0x40 = 1;
    return EXCEPTION_EXECUTE_HANDLER;
}
#endif

/* ------------------------------------------------------------- output */

static void out( const char *s )
{
    DWORD n = 0, written;

    while (s[n]) n++;
    WriteFile( GetStdHandle( STD_OUTPUT_HANDLE ), s, n, &written, NULL );
}

static void out_hex64( ULONG64 v )
{
    static const char hex[] = "0123456789ABCDEF";
    char buf[17];
    int i;

    for (i = 0; i < 16; i++) buf[15 - i] = hex[(v >> (4 * i)) & 0xf];
    buf[16] = 0;
    out( "0x" );
    out( buf );
}

static int failures;
static int step;

static void begin( const char *what )
{
    char n[2];

    n[0] = (char)('0' + ++step);
    n[1] = 0;
    out( "step " );
    out( n );
    out( " " );
    out( what );
    out( ": " );
}

/* A passing step prints no address: a transcript this gate compares
 * byte-for-byte must not carry a module base.  A failing one prints
 * everything it has, because at that point the numbers are the evidence. */
static void verdict( BOOL ok, const char *why, ULONG64 got, ULONG64 want )
{
    if (ok)
    {
        out( "ok\n" );
        return;
    }
    failures++;
    out( "FAIL (" );
    out( why );
    out( ") got " );
    out_hex64( got );
    out( " want " );
    out_hex64( want );
    out( "\n" );
}

/* ntdll hands every unresolved import its own sentinel, 0xdead0000 + n --
 * dlls/ntdll/loader.c allocate_stub(), whose banner explains why they are
 * distinct rather than one shared 0xdeadbeef.  The page is never mapped. */
static BOOL is_sentinel( ULONG64 v )
{
    return (v >> 16) == 0xdead;
}

/* ------------------------------------------------------------- the run */

void WINAPI ordinal_import_entry( void )
{
    ULONG64 by_ordinal, by_name = 0, bogus;
    HMODULE d3d12;

#if defined(ORDINAL_PROBE_SABOTAGE)
    out( "ordinal_import: SABOTAGE build " );
    out( ORDINAL_PROBE_SABOTAGE == 1 ? "1 (ordinal 102 under 101's name)\n"
                                     : "2 (a real export in the bogus slot)\n" );
#else
    out( "ordinal_import: probe start\n" );
#endif

    by_ordinal = (ULONG64)(ULONG_PTR)&D3D12CreateDevice;
    bogus      = (ULONG64)(ULONG_PTR)&ordprobe_bogus_ordinal;

    begin( "ordinal 101 bound" );
    verdict( by_ordinal && !is_sentinel( by_ordinal ),
             by_ordinal ? "bound to a missing-import sentinel" : "bound to NULL",
             by_ordinal, 0 );

    begin( "GetModuleHandleW(d3d12.dll)" );
    d3d12 = GetModuleHandleW( L"d3d12.dll" );
    verdict( d3d12 != NULL, "the guest namespace has no d3d12.dll",
             (ULONG64)(ULONG_PTR)d3d12, 0 );

    begin( "GetProcAddress(D3D12CreateDevice)" );
    if (d3d12) by_name = (ULONG64)(ULONG_PTR)GetProcAddress( d3d12, "D3D12CreateDevice" );
    verdict( by_name != 0, "the name does not resolve", by_name, 0 );

    begin( "ordinal 101 is the same export as the name" );
    verdict( by_ordinal == by_name && by_name != 0,
             "the ordinal bound a different export", by_ordinal, by_name );

    begin( "bogus ordinal 60001 bound to a sentinel" );
    verdict( is_sentinel( bogus ), "an ordinal this module cannot serve bound "
             "to something callable", bogus, 0xdead0000 );

#if defined(ORDINAL_PROBE_CALL_BOGUS) || defined(ORDINAL_PROBE_CRASHING_FILTER)
    /* Deliberately fatal.  Nothing may be printed after this line: the gate
     * requires the process to die here, and reads the absence of the line
     * below as the proof.  If the sentinel were ever a real, callable address
     * this would return and say so. */
#ifdef ORDINAL_PROBE_CRASHING_FILTER
    SetUnhandledExceptionFilter( crashing_filter );
#endif
    begin( "calling the sentinel" );
    out( "\n" );
    ordprobe_bogus_ordinal();
    out( "ordinal_import: FAIL the sentinel call RETURNED\n" );
    ExitProcess( 3 );
#endif

    out( failures ? "ordinal_import: FAIL\n" : "ordinal_import: PASS\n" );
    ExitProcess( failures ? 1 : 0 );
}
