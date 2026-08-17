/*
 * steam_bridge_probe.c -- the guest x86-64 program ppc64le/steamapi/
 * check-steam-bridge.sh drives the lsteamclient bridge with.
 *
 * It stands exactly where a game's steam_api64.dll stands: an x86-64 Windows
 * image that loads steamclient64.dll by name, takes CreateInterface out of
 * it, and calls through the vtable it is handed.  Nothing about the way it
 * reaches the DLL is special-cased for the test.
 *
 * It prints one value-checked line per step, in a fixed order, so the gate
 * can compare a transcript rather than grep for "PASS".  There is no CRT
 * here -- output goes through kernel32's WriteFile on the real stdout handle,
 * the same idiom ppc64le/syscom/com_smoke.c uses -- because a CRT would add a
 * second thing that can fail and this probe exists to test one thing.
 *
 * Two of its steps are switched on by the environment rather than always run:
 * the callback round-trip (STEAM_BRIDGE_CALLBACK=1) and the path translation
 * (STEAM_BRIDGE_PATH_IN=<a DOS path>).  Both need a helper to talk to and
 * neither needs a Steam client, and keeping them off by default is what lets
 * the no-helper scenarios keep comparing a fixed transcript.
 *
 * Copyright 2026 the ppc64le port authors
 *
 * This library is free software; you can redistribute it and/or modify it
 * under the terms of the GNU Lesser General Public License as published by
 * the Free Software Foundation; either version 2.1 of the License, or (at
 * your option) any later version.
 */

#include <windows.h>

/* The wire header itself, so that every constant this probe checks against is
 * the one the DLL and the helper compiled -- an id or a payload byte spelled
 * twice is a test that can agree with itself and disagree with the code. */
#include "steamrpc_wire.h"

static void out( const char *s )
{
    DWORD n = 0, len = 0;

    while (s[len]) len++;
    WriteFile( GetStdHandle( STD_OUTPUT_HANDLE ), s, len, &n, NULL );
}

static void out_num( const char *label, long long v, int hex )
{
    char buf[96];
    int i = 0, j, neg = 0;
    char d[32];
    int n = 0;

    while (label[i]) { buf[i] = label[i]; i++; }
    if (v < 0 && !hex) { neg = 1; v = -v; }
    if (!v) d[n++] = '0';
    while (v)
    {
        int dig = (int)(hex ? (v & 0xf) : (v % 10));
        d[n++] = (char)(dig < 10 ? '0' + dig : 'a' + dig - 10);
        v = hex ? (long long)((unsigned long long)v >> 4) : v / 10;
    }
    if (neg) buf[i++] = '-';
    if (hex) { buf[i++] = '0'; buf[i++] = 'x'; }
    for (j = n - 1; j >= 0; j--) buf[i++] = d[j];
    buf[i++] = '\n';
    buf[i] = 0;
    out( buf );
}

static void out_str( const char *label, const char *v )
{
    out( label );
    out( v );
    out( "\n" );
}

typedef void *(__cdecl *create_interface_t)( const char *name, int *code );
typedef signed char (__cdecl *is_known_t)( const char *version );
typedef int (__cdecl *selftest_t)( void );
typedef int (__cdecl *inject_t)( unsigned int kind, void *func, unsigned int id );
typedef int (__cdecl *pathtest_t)( const char *dos_in, char *unix_out,
                                   unsigned int unix_len, char *dos_back,
                                   unsigned int back_len, unsigned int *ndrives );
typedef int (__cdecl *overrun_t)( unsigned int len, unsigned int past );

/* CallbackMsg_t as the guest sees it: Proton's w64_CallbackMsg_t, from
 * proton/steamclient_structs_generated.h.  Declared here rather than included
 * because that header needs Proton's whole struct universe, and a probe that
 * needed the DLL's private headers to describe a public SDK struct would be
 * testing the wrong thing -- this is the shape a GAME declares. */
#pragma pack(push, 8)
struct probe_CallbackMsg
{
    int m_hSteamUser;
    int m_iCallback;
    unsigned char *m_pubParam;
    int m_cubParam;
    int __pad;
};
#pragma pack(pop)

typedef signed char (__cdecl *bgetcallback_t)( int pipe, struct probe_CallbackMsg *msg,
                                               int *ignored );
typedef signed char (__cdecl *freelast_t)( int pipe );

/* The queued CALL_CDECL_FUNC_DATA callback lands here, called by Proton's own
 * execute_pending_callbacks() with the payload the helper queued.  This is a
 * plain guest function, standing exactly where a game's Steamworks hook
 * stands: the helper stored its address and never called it, the PE side did.
 */
static int cdecl_calls;
static int cdecl_bad;

static void __cdecl probe_cdecl_callback( void *data )
{
    const unsigned char *p = (const unsigned char *)data;
    unsigned int i;

    cdecl_calls++;
    for (i = 0; i < STEAMRPC_INJECT_LEN; i++)
        if (p[i] != STEAMRPC_INJECT_BYTE( i )) { cdecl_bad = 1; return; }
}

/* Layer: one callback through each PULL channel, with no Steam client.  See
 * the injection comment in steamrpc_wire.h for why a synthetic callback at
 * the helper's end proves the part that can actually be wrong. */
static void probe_callbacks( HMODULE mod )
{
    struct probe_CallbackMsg msg;
    inject_t inject;
    bgetcallback_t bget;
    freelast_t freelast;
    int ignored = 0;
    unsigned int i;
    int bad = 0;

    inject = (inject_t)GetProcAddress( mod, "__wine_steamrpc_inject" );
    bget = (bgetcallback_t)GetProcAddress( mod, "Steam_BGetCallback" );
    freelast = (freelast_t)GetProcAddress( mod, "Steam_FreeLastCallback" );
    if (!inject || !bget || !freelast)
    {
        out( "bridge: cb=absent\n" );
        return;
    }

    out_num( "bridge: cb_inject_cdecl=",
             inject( STEAMRPC_INJECT_CDECL, (void *)probe_cdecl_callback, 0 ), 0 );
    out_num( "bridge: cb_inject_msg=",
             inject( STEAMRPC_INJECT_MSG, NULL, STEAMRPC_INJECT_ID ), 0 );

    /* One call, both channels: Proton drains the queue on the way in and
     * answers the message on the way out.  This is the export steam_api64's
     * SteamAPI_ManualDispatch_GetNextCallback and SteamAPI_RunCallbacks are
     * both built on -- there is no second entry point to test. */
    /* Zeroed by hand: this probe links no CRT, and a struct initialiser is
     * one of the shapes clang turns into a call to memset. */
    msg.m_hSteamUser = 0;
    msg.m_iCallback = 0;
    msg.m_pubParam = NULL;
    msg.m_cubParam = 0;
    msg.__pad = 0;

    out_num( "bridge: cb_bget=", bget( 0, &msg, &ignored ), 0 );
    out_num( "bridge: cb_id=", msg.m_iCallback, 1 );
    out_num( "bridge: cb_size=", msg.m_cubParam, 0 );

    /* The payload pointer is the whole point of the nested-pointer class: it
     * must still be the buffer the PE side allocated, not the helper's copy.
     * IsBadReadPtr answers that without faulting, so a regression here is a
     * red line in a transcript rather than a crashed probe. */
    if (!msg.m_pubParam || IsBadReadPtr( msg.m_pubParam, (UINT_PTR)msg.m_cubParam ))
        out( "bridge: cb_ptr=unreadable\n" );
    else
    {
        out( "bridge: cb_ptr=readable\n" );
        for (i = 0; i < STEAMRPC_INJECT_LEN && (int)i < msg.m_cubParam; i++)
            if (msg.m_pubParam[i] != STEAMRPC_INJECT_BYTE( i )) { bad = 1; break; }
        out( bad ? "bridge: cb_payload=bad\n" : "bridge: cb_payload=ok\n" );
    }

    out_num( "bridge: cb_cdecl_calls=", cdecl_calls, 0 );
    out( cdecl_bad ? "bridge: cb_cdecl_payload=bad\n"
                   : "bridge: cb_cdecl_payload=ok\n" );
    out_num( "bridge: cb_free=", freelast( 0 ), 0 );
}

/* Layer: the helper's red zone.  A marshal descriptor that is smaller than
 * what the Steam client actually writes used to end as glibc aborting the
 * helper (`free(): invalid size`, DOOM's title screen) and the game losing
 * Steam mid-session.  Ask the helper to overrun a buffer on purpose and check
 * that it is caught, refused by name, and survived -- the survival being the
 * point, so the second call below has something to answer it. */
static void probe_guard( HMODULE mod )
{
    overrun_t overrun;
    selftest_t selftest;

    overrun = (overrun_t)GetProcAddress( mod, "__wine_steamrpc_overrun" );
    selftest = (selftest_t)GetProcAddress( mod, "__wine_steamrpc_selftest" );
    if (!overrun || !selftest) { out( "bridge: guard=absent\n" ); return; }

    /* A one-byte overrun: the smallest thing the guard has to notice. */
    out_num( "bridge: guard_caught_1=", overrun( 8, 1 ), 0 );
    /* And 120, which is exactly what GetConnectedControllers overran by. */
    out_num( "bridge: guard_caught_120=", overrun( 8, 120 ), 0 );
    /* The helper must still be there and still be right afterwards.  A guard
     * that catches the overrun by dying of it would pass the two lines above
     * and fail this one. */
    out_num( "bridge: guard_alive_selftest=", selftest(), 1 );
}

/* Layer: what a game sees when the helper goes away mid-session.  The gate
 * kills it between these two calls; everything after that must fail cleanly
 * and promptly, and this process must reach its own next line. */
static void probe_after_death( HMODULE mod )
{
    selftest_t selftest;
    is_known_t is_known;

    HANDLE marker;
    int i;

    selftest = (selftest_t)GetProcAddress( mod, "__wine_steamrpc_selftest" );
    is_known = (is_known_t)GetProcAddress( mod, "Steam_IsKnownInterface" );
    if (!selftest || !is_known) { out( "bridge: postmortem=absent\n" ); return; }

    /* The bridge has to be working first, or "it stopped working" proves
     * nothing. */
    out_num( "bridge: predeath_selftest=", selftest(), 1 );

    /* Hand the gate a starting gun rather than a sleep: it kills the helper
     * when this file appears, and the loop below ends when the socket dies.
     * Nothing here depends on how long that takes. */
    marker = CreateFileA( "kill-the-helper-now", GENERIC_WRITE, 0, NULL,
                          CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL );
    if (marker == INVALID_HANDLE_VALUE)
    {
        out( "bridge: postmortem=no-marker\n" );
        return;
    }
    CloseHandle( marker );

    for (i = 0; i < 3000; i++)          /* up to 30s */
    {
        if (selftest()) break;
        Sleep( 10 );
    }
    if (i == 3000)
    {
        out( "bridge: postmortem=still-alive\n" );
        return;
    }

    /* BAD_STATUS alone (0x1) is the right answer: the call could not be sent.
     * Any other bit would mean the marshaller also disagreed about something,
     * which a dead socket must not be able to cause. */
    out_num( "bridge: dead_selftest=", selftest(), 1 );
    out_num( "bridge: dead_is_known=", is_known( "SteamClient017" ), 0 );
    out( "bridge: dead_survived\n" );
}

/* Layer: the same channel against a LIVE client, with no injection at all --
 * connect a user to the pipe and poll for whatever the client actually sends.
 * Only reached when a Steam client is running, and only ever under the SDK's
 * own test appid; see the gate. */
static void probe_live_callbacks( HMODULE mod, void *iface, int pipe )
{
    typedef int (*connect_user_t)( void *this_ );
    struct probe_CallbackMsg msg;
    bgetcallback_t bget;
    freelast_t freelast;
    int ignored = 0, user, n = 0, tries;

    bget = (bgetcallback_t)GetProcAddress( mod, "Steam_BGetCallback" );
    freelast = (freelast_t)GetProcAddress( mod, "Steam_FreeLastCallback" );
    if (!bget || !freelast) { out( "bridge: live_cb=absent\n" ); return; }

    /* ISteamClient017 slot 2, ConnectToGlobalUser -- named against Proton's
     * winISteamClient.c like slot 0 above.  Nothing is delivered on a pipe
     * with no user on it. */
    user = ((connect_user_t *)(*(void ***)iface))[2]( iface );
    out_num( "bridge: live_user=", user, 0 );
    if (!user) return;

    for (tries = 0; tries < 300 && n < 8; tries++)
    {
        msg.m_hSteamUser = 0;
        msg.m_iCallback = 0;
        msg.m_pubParam = NULL;
        msg.m_cubParam = 0;
        msg.__pad = 0;

        if (!bget( pipe, &msg, &ignored )) { Sleep( 10 ); continue; }
        out_num( "bridge: live_cb_id=", msg.m_iCallback, 0 );
        n++;
        freelast( pipe );
    }
    out_num( "bridge: live_cb_count=", n, 0 );
}

/* Layer: one DOS path to unix and back, through the helper's translation and
 * Proton's own converters.  The path comes from the environment because the
 * gate is the only party that can compute what the answer must be. */
static void probe_path( HMODULE mod )
{
    char dos_in[512], unix_out[1024], dos_back[1024];
    unsigned int ndrives = 0;
    pathtest_t pathtest;
    DWORD n;

    n = GetEnvironmentVariableA( "STEAM_BRIDGE_PATH_IN", dos_in, sizeof(dos_in) );
    if (!n || n >= sizeof(dos_in)) return;

    if (!(pathtest = (pathtest_t)GetProcAddress( mod, "__wine_steamrpc_pathtest" )))
    {
        out( "bridge: path=absent\n" );
        return;
    }

    out_num( "bridge: path_ret=",
             pathtest( dos_in, unix_out, sizeof(unix_out), dos_back,
                       sizeof(dos_back), &ndrives ), 0 );
    out_num( "bridge: path_drives=", ndrives, 0 );
    out_str( "bridge: path_unix=", unix_out );
    out_str( "bridge: path_dos=", dos_back );
}

/* ISteamClient017, as Proton lays it out (winISteamClient.c): slot 0 is
 * CreateSteamPipe, slot 2 ConnectToGlobalUser.  A vtable index is the one
 * thing a probe must not guess, so both are named here against that file. */
typedef int (*create_pipe_t)( void *this_ );

void __stdcall steam_bridge_probe_entry(void)
{
    create_interface_t create_interface;
    is_known_t is_known;
    selftest_t selftest;
    HMODULE mod;
    void *iface;
    int cb_mode = 0;
    int code = 0;
    int pipe = 0;
    int rc = 0;

    /* Bare name only: this port's guest loader resolves a guest DLL through
     * the AMD64 search order and refuses a path outright, so a bare name is
     * both what works and what a game's registry value has to end up saying. */
    if (!(mod = LoadLibraryA( "steamclient64.dll" )))
    {
        out( "bridge: LoadLibraryA(steamclient64.dll) FAILED\n" );
        ExitProcess( 2 );
    }
    out( "bridge: loaded steamclient64.dll\n" );

    create_interface = (create_interface_t)GetProcAddress( mod, "CreateInterface" );
    is_known = (is_known_t)GetProcAddress( mod, "Steam_IsKnownInterface" );
    selftest = (selftest_t)GetProcAddress( mod, "__wine_steamrpc_selftest" );
    if (!create_interface || !is_known)
    {
        out( "bridge: GetProcAddress FAILED\n" );
        ExitProcess( 2 );
    }
    out( "bridge: exports resolved\n" );

    /* 1. the marshaller, end to end, with values both ends check.  This is
     *    the only step that does not need a Steam client at all. */
    /* The value is printed, never judged here: 0 means every class
     * round-tripped, and 0x1 (BAD_STATUS alone) is the correct answer when
     * there is no helper to talk to.  Which of those is expected depends on
     * the scenario, so the gate decides, not the probe. */
    if (selftest) out_num( "bridge: selftest=", selftest(), 1 );
    else out( "bridge: selftest=absent\n" );

    /* Optional, off by default so the transcript is stable: a game polls
     * Steam_BGetCallback every frame, so the per-call cost of a loopback
     * round trip is the number that decides whether this design is viable.
     * STEAM_BRIDGE_PROBE_REPEAT=N reports it. */
    if (selftest)
    {
        char rep[16];
        DWORD got = GetEnvironmentVariableA( "STEAM_BRIDGE_PROBE_REPEAT",
                                             rep, sizeof(rep) );
        if (got && got < sizeof(rep))
        {
            int n = 0, k;
            DWORD t0;
            for (k = 0; rep[k] >= '0' && rep[k] <= '9'; k++) n = n * 10 + (rep[k] - '0');
            t0 = GetTickCount();
            for (k = 0; k < n; k++) selftest();
            out_num( "bridge: repeat_count=", n, 0 );
            out_num( "bridge: repeat_ms=", (long long)(GetTickCount() - t0), 0 );
        }
    }

    /* 2. a string in, a bool out, through Proton's own flat export. */
    out_num( "bridge: IsKnownInterface(SteamClient017)=",
             is_known( "SteamClient017" ), 0 );

    /* 2b. callbacks and path translation, both off unless the gate asks, so
     *     that the no-helper transcripts stay byte-stable.  Both need a
     *     helper (one injects into it, the other is its translation), and
     *     neither needs a Steam client.  STEAM_BRIDGE_CALLBACK=2 is the
     *     opposite case -- a live client, nothing injected -- and happens
     *     after the pipe exists, further down. */
    {
        char on[8];
        DWORD n = GetEnvironmentVariableA( "STEAM_BRIDGE_CALLBACK", on, sizeof(on) );

        cb_mode = (n && n < sizeof(on)) ? on[0] : 0;
        if (cb_mode == '1') probe_callbacks( mod );
    }
    probe_path( mod );

    /* 2c. the marshalling safety net, and what a game sees when the helper
     *     goes away underneath it.  Both off unless the gate asks, for the
     *     same transcript-stability reason as 2b -- and the second one ends
     *     with no bridge at all, so nothing after it could run. */
    {
        char on[8];
        DWORD n = GetEnvironmentVariableA( "STEAM_BRIDGE_GUARD", on, sizeof(on) );

        if (n && n < sizeof(on) && on[0] == '1') probe_guard( mod );
        n = GetEnvironmentVariableA( "STEAM_BRIDGE_KILL", on, sizeof(on) );
        if (n && n < sizeof(on) && on[0] == '1')
        {
            probe_after_death( mod );
            out( "bridge: DONE\n" );
            ExitProcess( 0 );
        }
    }

    /* 3. the interface object itself: a guest-callable vtable made of guest
     *    code, handed back by CreateInterface. */
    iface = create_interface( "SteamClient017", &code );
    if (!iface)
    {
        out( "bridge: CreateInterface(SteamClient017)=NULL\n" );
        ExitProcess( rc );
    }
    out( "bridge: CreateInterface(SteamClient017)=object\n" );

    /* 4. one vtable call.  With no Steam client running this must return 0 --
     *    "no pipe" -- which is exactly what a Linux game sees, and it must
     *    return rather than fault. */
    pipe = (*(create_pipe_t **)iface)[0]( iface );
    out_num( "bridge: CreateSteamPipe=", pipe, 0 );

    /* 5. with a real client on the far end, the same callback channel with
     *    nothing injected: whatever the client itself sends. */
    if (pipe && cb_mode == '2') probe_live_callbacks( mod, iface, pipe );

    out( "bridge: DONE\n" );
    ExitProcess( rc );
}
