/*
 * interface_probe.c -- a guest PE that stands exactly where steam_api.dll
 * stands, in FOUR escalating steps, so that a failure names which step broke.
 *
 * Built for BOTH guest machines from this one source by probe-interface.sh,
 * which is where the findings, the measured table and the known dead ends are
 * written up.  Read that header first; this file is only the mechanism.
 *
 * THE FOUR STEPS, each a superset of the one before:
 *
 *   1  LOAD        LoadLibraryA on the guest steamclient DLL.
 *   2  CREATE      + GetProcAddress("CreateInterface") and call it.
 *   3  AFTER       + an OutputDebugStringA once the interface exists.  Not
 *                    decoration: OutputDebugStringA raises
 *                    DBG_PRINTEXCEPTION_C and runs the whole exception
 *                    dispatch path, so it is a cheap probe for state that an
 *                    earlier step corrupted but that nothing has tripped over
 *                    yet.
 *   4  VTABLE      + one call through the returned C++ vtable, slot 0.
 *
 * WHY SLOT 0 IS THE RIGHT SLOT, and it was checked rather than assumed:
 * dlls/steamclient64/proton/winISteamClient.c builds the SteamClient020
 * vtable with __ASM_VTABLE, and its first VTABLE_ADD_FUNC is CreateSteamPipe.
 * It is also the ideal call to test with -- Proton's descriptor for it
 * (steamrpc_generated.c) has NO pointer fields at all, so nothing has to be
 * marshalled and a failure cannot be blamed on the marshal plan.
 *
 * THE CALLING CONVENTION IS THE OBVIOUS THING TO GET WRONG HERE, so it is
 * spelled out.  Steamworks methods are __thiscall: on i386 `this` arrives in
 * ecx, and on x86_64 there is no such convention and `this` is simply the
 * first argument.  Wine's DEFINE_THISCALL_WRAPPER asm thunk is deliberately
 * NOT used in this build -- include/wine/asm.h gates it on
 * `!defined(__MINGW32__)`, and a guest PE is compiled -target
 * <machine>-windows-gnu, which defines __MINGW32__ -- so the vtable slot
 * holds the raw __thiscall function and a __thiscall call site is exactly
 * right.  VERIFIED IN THE OBJECT, not merely reasoned about: the i386 build
 * disassembles to
 *
 *     movl (%esi), %eax      ; eax = vtable
 *     movl %esi, %ecx        ; ecx = this
 *     calll *(%eax)          ; slot 0
 *
 * so when this probe faults on i386 it is not because the probe called wrong.
 *
 * Exit codes, so nothing has to be parsed out of a log:
 *
 *    10  INTERFACE_PROBE_DLL unset
 *    11  LoadLibrary failed
 *    12  no CreateInterface export
 *    13  CreateInterface returned NULL
 *    20  reached the end of step 1, 2 or 3 with no fault
 *    30  step 4: the vtable call returned NON-ZERO  (a live Steam client)
 *    31  step 4: the vtable call returned ZERO      (no client, but it
 *                RETURNED -- which is the documented no-client answer and is
 *                a pass for everything this probe is testing)
 *
 * Copyright 2026 the ppc64le port authors
 *
 * This library is free software; you can redistribute it and/or modify it
 * under the terms of the GNU Lesser General Public License as published by
 * the Free Software Foundation; either version 2.1 of the License, or (at
 * your option) any later version.
 */

#include <windows.h>

#ifdef __i386__
typedef int (__thiscall *slot0_t)( void *self );
#else
typedef int (*slot0_t)( void *self );
#endif
typedef void * (__cdecl *create_interface_t)( const char *name, int *ret );

static void say( const char *s ) { OutputDebugStringA( s ); }

static int env_level( void )
{
    char buf[16];
    if (!GetEnvironmentVariableA( "INTERFACE_PROBE_LEVEL", buf, sizeof(buf) )) return 4;
    if (buf[0] < '1' || buf[0] > '4') return 4;
    return buf[0] - '0';
}

void __stdcall interface_probe_entry( void )
{
    char path[512], iface[64];
    HMODULE mod;
    create_interface_t create_interface;
    void *obj, **vtbl;
    int ret = 0, level = env_level(), slot0;

    if (!GetEnvironmentVariableA( "INTERFACE_PROBE_DLL", path, sizeof(path) )) ExitProcess( 10 );
    if (!GetEnvironmentVariableA( "INTERFACE_PROBE_IFACE", iface, sizeof(iface) ))
        lstrcpyA( iface, "SteamClient020" );

    say( "interface_probe: step 1 LoadLibrary\n" );
    if (!(mod = LoadLibraryA( path ))) ExitProcess( 11 );
    if (level < 2) { say( "interface_probe: stopped after step 1\n" ); ExitProcess( 20 ); }

    say( "interface_probe: step 2 CreateInterface\n" );
    if (!(create_interface = (create_interface_t)GetProcAddress( mod, "CreateInterface" )))
        ExitProcess( 12 );
    if (!(obj = create_interface( iface, &ret ))) ExitProcess( 13 );
    if (level < 3) { say( "interface_probe: stopped after step 2\n" ); ExitProcess( 20 ); }

    /* Step 3 is this line: an exception dispatch AFTER the interface exists. */
    say( "interface_probe: step 3 exception dispatch after CreateInterface\n" );
    if (level < 4) { say( "interface_probe: stopped after step 3\n" ); ExitProcess( 20 ); }

    say( "interface_probe: step 4 calling vtable slot 0\n" );
    vtbl = *(void ***)obj;
    slot0 = ((slot0_t)vtbl[0])( obj );
    say( slot0 ? "interface_probe: vtable slot 0 returned NON-ZERO\n"
               : "interface_probe: vtable slot 0 returned ZERO\n" );
    ExitProcess( slot0 ? 30 : 31 );
}
