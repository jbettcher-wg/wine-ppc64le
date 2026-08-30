/*
 * dllload_probe.c -- load a guest DLL, run its DllMain, say what happened.
 *
 * WHY THIS EXISTS.  A title that dies in loader_init because one of its
 * bundled DLLs took a 0xDEAD.... sentinel at PROCESS_ATTACH is a two-minute
 * launch to reproduce -- but it is a launch: it needs the foreground, the GPU
 * and the game lock, and on a box where four agents are queueing behind one
 * display that is the expensive part of the loop, not the fix.
 *
 * The failing thing is not the game.  It is one LoadLibrary of one DLL and
 * the import binding underneath it.  So do exactly that, from a guest x86-64
 * process, with no window, no device and no lock:
 *
 *   DLLLOAD_PROBE_FILE   the module to load (a Windows path, or a bare name
 *                        to be resolved on the ordinary search order).
 *   DLLLOAD_PROBE_DIR    optional; SetCurrentDirectory + SetDllDirectory to
 *                        this first, so a DLL that sits beside its siblings
 *                        in a game directory finds them the way the game
 *                        would.
 *   DLLLOAD_PROBE_PROC   optional; after a successful load, GetProcAddress
 *                        this name and report the answer.  Binding is what
 *                        the sentinels are about, so being able to ask for
 *                        one specific export closes the loop.
 *
 * Exit code is 0 for a load that succeeded, 1 for one that failed, 2 for a
 * probe used wrongly -- so a caller can gate on it without parsing.
 *
 * A DLL whose DllMain calls through a sentinel FAULTS here rather than
 * returning an error, which is the point: the fault carries the same
 * 0xDEAD.... value and the same "which is in no guest image" line the game
 * produced, and with WINEDEBUG=warn+module the run's own
 *
 *   warn:module:import_dll No implementation for <dll>.<symbol> imported
 *     from L"...", setting to 00000000DEAD00nn
 *
 * lines name every sentinel it handed out.  NOTE THE MESSAGE TEXT: the string
 * "allocating stub" appears only in dlls/ntdll/loader.c's no-export-directory
 * branch (line 1307) and NOT on the ordinary per-symbol path (1327, 1343),
 * which says "setting to" instead.  Grepping for the wrong one of those two
 * is what made this class of bug look undiagnosable on 2026-08-30.
 *
 * There is no CRT here: output goes through kernel32's WriteFile on the real
 * stdout handle, the same idiom ppc64le/steamtool/launch_probe.c and
 * ppc64le/syscom/com_smoke.c use.  A CRT would add a second thing that can
 * fail, and this probe exists to test one thing.
 *
 * Copyright 2026 the ppc64le port authors
 *
 * This library is free software; you can redistribute it and/or modify it
 * under the terms of the GNU Lesser General Public License as published by
 * the Free Software Foundation; either version 2.1 of the License, or (at
 * your option) any later version.
 */

#include <windows.h>

static void out( const char *s )
{
    DWORD n = 0, len = 0;

    while (s[len]) len++;
    WriteFile( GetStdHandle( STD_OUTPUT_HANDLE ), s, len, &n, NULL );
}

static void out_num( const char *label, unsigned long long v )
{
    char buf[160];
    char d[32];
    int i = 0, j, n = 0;

    while (label[i]) { buf[i] = label[i]; i++; }
    if (!v) d[n++] = '0';
    while (v) { d[n++] = (char)('0' + (v % 10)); v /= 10; }
    for (j = n - 1; j >= 0; j--) buf[i++] = d[j];
    buf[i++] = '\n';
    buf[i] = 0;
    out( buf );
}

static void out_hex( const char *label, unsigned long long v )
{
    static const char hex[] = "0123456789ABCDEF";
    char buf[160];
    int i = 0, j;

    while (label[i]) { buf[i] = label[i]; i++; }
    buf[i++] = '0'; buf[i++] = 'x';
    for (j = 60; j >= 0; j -= 4) buf[i++] = hex[(v >> j) & 0xf];
    buf[i++] = '\n';
    buf[i] = 0;
    out( buf );
}

/* The ANSI name of an export, out of a wide environment variable.  No CRT, so
 * no wcstombs; export names are ASCII by definition of the PE format. */
static int narrow( const WCHAR *w, char *a, int max )
{
    int i = 0;

    while (w[i] && i < max - 1) { a[i] = (char)w[i]; i++; }
    a[i] = 0;
    return i;
}

void __stdcall dllload_probe_entry(void)
{
    WCHAR file[MAX_PATH], dir[MAX_PATH], proc[256];
    char procA[256];
    HMODULE mod;
    DWORD len;

    len = GetEnvironmentVariableW( L"DLLLOAD_PROBE_FILE", file, MAX_PATH );
    if (!len || len >= MAX_PATH)
    {
        out( "dllload_probe: set DLLLOAD_PROBE_FILE to the module to load\n" );
        ExitProcess( 2 );
    }

    len = GetEnvironmentVariableW( L"DLLLOAD_PROBE_DIR", dir, MAX_PATH );
    if (len && len < MAX_PATH)
    {
        if (!SetCurrentDirectoryW( dir ))
            out_num( "dllload_probe: SetCurrentDirectory failed, error ", GetLastError() );
        /* A game's own DLLs sit beside it, and the loader's default search
         * order does not include the current directory for a dependency of a
         * dependency.  This is what the game itself gets from being started
         * in its own directory. */
        SetDllDirectoryW( dir );
    }

    out( "dllload_probe: loading\n" );
    SetLastError( 0 );
    mod = LoadLibraryW( file );
    if (!mod)
    {
        out_num( "dllload_probe: LoadLibrary FAILED, error ", GetLastError() );
        ExitProcess( 1 );
    }
    out_hex( "dllload_probe: loaded at ", (unsigned long long)(ULONG_PTR)mod );

    len = GetEnvironmentVariableW( L"DLLLOAD_PROBE_PROC", proc, 256 );
    if (len && len < 256)
    {
        FARPROC p;

        narrow( proc, procA, sizeof(procA) );
        SetLastError( 0 );
        p = GetProcAddress( mod, procA );
        if (p) out_hex( "dllload_probe: proc at ", (unsigned long long)(ULONG_PTR)p );
        else out_num( "dllload_probe: GetProcAddress FAILED, error ", GetLastError() );
    }

    /* Not FreeLibrary: a module that survived PROCESS_ATTACH can still fault
     * in PROCESS_DETACH, and a clean unload is not what is being measured.
     * Leave it mapped and exit -- the process is about to end anyway. */
    out( "dllload_probe: OK\n" );
    ExitProcess( 0 );
}
