/*
 * launch_probe.c -- the guest x86-64 program ppc64le/steamtool/
 * check-launch-smoke.sh drives the compat tool's launch path with.
 *
 * It stands exactly where Steam's legacycompat pre-step stood when task #12
 * was filed.  iscriptevaluator.exe reads an install script, decides the work
 * needs the elevated helper, and hands legacycompat\SteamService.exe to
 * ShellExecuteExW with fMask = SEE_MASK_NOCLOSEPROCESS and nShow = SW_HIDE --
 * no SEE_MASK_FLAG_NO_UI, because on Windows that call is not expected to
 * fail.  On this port it does: the helper is a 32-bit i386 PE and there is no
 * 32-bit guest, so CreateProcess answers ERROR_BAD_EXE_FORMAT, shell32 raises
 * its error box, and a process with no user sits in that box's message loop
 * for ever.  This probe reproduces that call shape exactly, so the gate is
 * testing the launch path rather than a rehearsal of it.
 *
 * LAUNCH_PROBE_FILE names the file to execute, as a Windows path.  With the
 * variable unset the probe prints its no-file line and exits, which is how
 * the gate exercises a verb without asking anything of ShellExecute -- the
 * game verb's environment is checked that way, and a probe that always
 * launched something could not be used for it.
 *
 * There is no CRT here: output goes through kernel32's WriteFile on the real
 * stdout handle, the same idiom ppc64le/steamapi/steam_bridge_probe.c and
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
#include <shellapi.h>

static void out( const char *s )
{
    DWORD n = 0, len = 0;

    while (s[len]) len++;
    WriteFile( GetStdHandle( STD_OUTPUT_HANDLE ), s, len, &n, NULL );
}

static void out_num( const char *label, unsigned long long v )
{
    char buf[128];
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

void __stdcall launch_probe_entry(void)
{
    WCHAR file[MAX_PATH];
    SHELLEXECUTEINFOW sei;
    DWORD len, i;
    BOOL ok;

    len = GetEnvironmentVariableW( L"LAUNCH_PROBE_FILE", file, MAX_PATH );
    if (!len || len >= MAX_PATH)
    {
        out( "probe: no LAUNCH_PROBE_FILE, nothing executed\n" );
        out( "probe: done\n" );
        ExitProcess( 0 );
    }

    for (i = 0; i < sizeof(sei); i++) ((BYTE *)&sei)[i] = 0;
    sei.cbSize = sizeof(sei);
    /* The evaluator's own mask, read off a +exec trace of the real launch:
     * mask=0x00000040 verb=L"open" show=0x00000000.  SEE_MASK_FLAG_NO_UI is
     * deliberately absent -- that absence is the whole point, because it is
     * what lets shell32 raise a dialog on a run with nobody to answer it. */
    sei.fMask = SEE_MASK_NOCLOSEPROCESS;
    sei.hwnd = NULL;
    sei.lpVerb = L"open";
    sei.lpFile = file;
    sei.lpParameters = L"/installscript";
    sei.lpDirectory = NULL;
    sei.nShow = SW_HIDE;

    SetLastError( 0 );
    ok = ShellExecuteExW( &sei );

    out_num( "probe: ShellExecuteExW returned ", (unsigned long long)(ok ? 1 : 0) );
    out_num( "probe: hInstApp=", (unsigned long long)(ULONG_PTR)sei.hInstApp );
    out( "probe: done\n" );
    ExitProcess( 0 );
}
