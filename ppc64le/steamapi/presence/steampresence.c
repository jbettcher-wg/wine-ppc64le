/*
 * steampresence.c -- publish the Windows-side "a Steam client is running"
 * named kernel objects into this prefix, for as long as a run lasts.
 *
 * WHY THIS EXISTS.  Both Oblivion Remastered and Frostpunk 2 ship a
 * shipping .exe whose PE entry point is in a `.bind` section -- Valve's
 * SteamStub wrapper.  Before it jumps to the real OEP the stub asks, in the
 * only way a Windows process can, whether a Steam client is present in this
 * session:
 *
 *     OpenEventA( SYNCHRONIZE, FALSE, "Local\\SteamStart_SharedMemLock" )
 *     OpenFileMappingA( FILE_MAP_WRITE, FALSE, "Local\\SteamStart_SharedMemFile" )
 *
 * Both names are Valve's own and were read out of the Steam install on this
 * machine -- `strings` on ~/.local/share/Steam/legacycompat/steamclient.dll,
 * which builds them from the format pair "%s%s_SharedMemFile" /
 * "%s%s_SharedMemLock" and also carries both fully-formed literals.  They are
 * NOT guessed from a web page and NOT copied out of any emulator.  On Windows
 * they are published by steam.exe's CSharedMemStream; nothing on this port
 * published them, so the stub concluded Steam was not running, put up an
 * MB_ICONERROR box and called TerminateProcess(-1, 0x33) -- the rc=51 both
 * titles died with.
 *
 * WHAT THIS IS NOT.  It is not a Steam emulator, it does not speak to any
 * Valve service, it does not fabricate an account, a licence or a ticket, and
 * it does not decrypt or patch anything in the game.  It publishes two empty
 * named objects that say "a Steam client is present in this session", and it
 * refuses to do even that unless a Steam client really IS present: it runs
 * only when the launcher has already brought up ppc64le/steamapi/helper and
 * put its address in STEAM_BRIDGE_ADDR, and that helper only comes up when it
 * has dlopen'd the user's real, running, logged-in Steam client library.  No
 * bridge, no presence.
 *
 * WHY A SEPARATE PROCESS rather than a few lines in steamclient64.dll's
 * DllMain: the stub runs at the process entry point and chooses when to load
 * steam_api64.dll, so anything published from inside the game process is
 * published at an ordering the game controls.  A process the launcher starts
 * BEFORE the game cannot lose that race.  It is also what Proton does, with
 * its steam.exe, and what Windows does, with steam.exe proper.  The objects
 * live in the prefix's session namespace, which every process under one
 * wineserver shares, so a holder outside the game is enough.
 *
 * There is no CRT here (-nostdlib), for the same reason
 * ppc64le/steamapi/steam_bridge_probe.c has none: a CRT is a second thing
 * that can fail in a program that exists to do one thing.
 *
 * Copyright 2026 the ppc64le port authors
 *
 * This library is free software; you can redistribute it and/or modify it
 * under the terms of the GNU Lesser General Public License as published by
 * the Free Software Foundation; either version 2.1 of the License, or (at
 * your option) any later version.
 */

#include <windows.h>

/* Valve's names, verbatim.  See the header comment for where they came from. */
static const char lock_name[] = "Local\\SteamStart_SharedMemLock";
static const char file_name[] = "Local\\SteamStart_SharedMemFile";

/* One page.  The stub only OPENS the mapping -- it never mapped a view in any
 * measured run -- so nothing here is a claim about CSharedMemStream's real
 * layout, which this port does not know and does not guess.  The view is left
 * zeroed rather than filled with a plausible-looking header: a wrong header
 * read as a right one is the silent-wrong-answer this tree refuses.  If a
 * title is ever seen to MapViewOfFile this section and then fail, THAT is the
 * measurement that earns a layout, and it goes in the log first. */
#define SHAREDMEM_SIZE 0x1000

static void out( const char *s )
{
    DWORD n = 0, len = 0;

    while (s[len]) len++;
    WriteFile( GetStdHandle( STD_OUTPUT_HANDLE ), s, len, &n, NULL );
}

static void out_num( const char *label, unsigned long long v )
{
    char buf[96], d[24];
    int i = 0, j, n = 0;

    while (label[i]) { buf[i] = label[i]; i++; }
    buf[i++] = '0'; buf[i++] = 'x';
    if (!v) d[n++] = '0';
    while (v) { int dig = (int)(v & 0xf); d[n++] = (char)(dig < 10 ? '0' + dig : 'a' + dig - 10); v >>= 4; }
    for (j = n - 1; j >= 0; j--) buf[i++] = d[j];
    buf[i++] = '\n'; buf[i] = 0;
    out( buf );
}

static BOOL cmdline_has( const char *needle )
{
    const char *hay = GetCommandLineA();
    int i, j;

    if (!hay) return FALSE;
    for (i = 0; hay[i]; i++)
    {
        for (j = 0; needle[j] && hay[i + j] == needle[j]; j++) ;
        if (!needle[j]) return TRUE;
    }
    return FALSE;
}

/* --probe: ask for the two objects exactly the way the stub asks for them, so
 * that "does a holder in another process satisfy this?" can be answered
 * without launching a 23 GB game.  Same access masks, same names, same order
 * as the measured trace (event first, then mapping). */
static int probe( void )
{
    HANDLE ev, fm;
    int ok = 1;

    ev = OpenEventA( SYNCHRONIZE, FALSE, lock_name );
    out( ev ? "probe: OpenEventA(SYNCHRONIZE, Local\\SteamStart_SharedMemLock) OK\n"
            : "probe: OpenEventA(SYNCHRONIZE, Local\\SteamStart_SharedMemLock) FAILED\n" );
    if (!ev) { out_num( "  last error ", GetLastError() ); ok = 0; }
    else CloseHandle( ev );

    fm = OpenFileMappingA( FILE_MAP_WRITE, FALSE, file_name );
    out( fm ? "probe: OpenFileMappingA(FILE_MAP_WRITE, Local\\SteamStart_SharedMemFile) OK\n"
            : "probe: OpenFileMappingA(FILE_MAP_WRITE, Local\\SteamStart_SharedMemFile) FAILED\n" );
    if (!fm) { out_num( "  last error ", GetLastError() ); ok = 0; }
    else
    {
        /* The stub has never been seen to do this, but if it ever does, the
         * view has to be mappable with the access it opened.  Prove it here
         * rather than discover it inside a game. */
        void *view = MapViewOfFile( fm, FILE_MAP_WRITE, 0, 0, 0 );
        out( view ? "probe: MapViewOfFile(FILE_MAP_WRITE) OK\n"
                  : "probe: MapViewOfFile(FILE_MAP_WRITE) FAILED\n" );
        if (!view) { out_num( "  last error ", GetLastError() ); ok = 0; }
        else UnmapViewOfFile( view );
        CloseHandle( fm );
    }

    out( ok ? "probe: PRESENT\n" : "probe: ABSENT\n" );
    return ok ? 0 : 1;
}

void __stdcall steampresence_entry( void )
{
    HANDLE ev, fm;
    char addr[64];

    if (cmdline_has( "--probe" )) ExitProcess( probe() );

    /* THE HONESTY GATE.  STEAM_BRIDGE_ADDR is set by ppc64le/steamtool/proton
     * only after ppc64le/steamapi/helper came up, and the helper comes up only
     * when it has loaded the user's real Steam client library.  Without it
     * there is no running client and this process has nothing true to say. */
    if (!GetEnvironmentVariableA( "STEAM_BRIDGE_ADDR", addr, sizeof(addr) ))
    {
        out( "steampresence: STEAM_BRIDGE_ADDR is unset, so no Steam client is\n"
             "  reachable and there is no presence to publish.  Refusing.\n" );
        ExitProcess( 2 );
    }

    /* Manual-reset and already signalled.  The stub only asks for SYNCHRONIZE
     * and never waits, but a manual-reset signalled event is the shape that
     * cannot deadlock anyone who does wait on it, and a wait on it cannot
     * change the state out from under a second waiter. */
    ev = CreateEventA( NULL, TRUE, TRUE, lock_name );
    if (!ev)
    {
        out( "steampresence: CreateEventA(Local\\SteamStart_SharedMemLock) FAILED\n" );
        out_num( "  last error ", GetLastError() );
        ExitProcess( 1 );
    }
    if (GetLastError() == ERROR_ALREADY_EXISTS)
        out( "steampresence: Local\\SteamStart_SharedMemLock already existed; adopting it\n" );

    fm = CreateFileMappingA( INVALID_HANDLE_VALUE, NULL, PAGE_READWRITE,
                             0, SHAREDMEM_SIZE, file_name );
    if (!fm)
    {
        out( "steampresence: CreateFileMappingA(Local\\SteamStart_SharedMemFile) FAILED\n" );
        out_num( "  last error ", GetLastError() );
        ExitProcess( 1 );
    }
    if (GetLastError() == ERROR_ALREADY_EXISTS)
        out( "steampresence: Local\\SteamStart_SharedMemFile already existed; adopting it\n" );

    out( "steampresence: published Local\\SteamStart_SharedMemLock (event, manual-reset, set)\n" );
    out( "steampresence: published Local\\SteamStart_SharedMemFile (section, 0x1000, zeroed)\n" );
    out( "steampresence: holding.  Kill this process to withdraw them.\n" );
    FlushFileBuffers( GetStdHandle( STD_OUTPUT_HANDLE ) );

    /* Hold the handles.  The objects live exactly as long as this process:
     * the launcher starts it before the game and kills it after, so a run
     * that is over stops claiming a client is there. */
    for (;;) Sleep( 60000 );
}
