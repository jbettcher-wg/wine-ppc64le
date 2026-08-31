/*
 * steamrpc.c -- the client half of the lsteamclient bridge.
 *
 * Proton routes every Steamworks method through one macro, WINE_UNIX_CALL,
 * with a flat params struct.  steamclient_guest.h points that macro here, and
 * this file turns the call into a frame on a socket to the x86-64 Linux
 * helper that owns the real steamclient.so.  See steamrpc_wire.h for the
 * frame, tools/steamrpc/gen-steamrpc for where the per-field descriptors come
 * from, and ppc64le/steamapi/helper for the other end.
 *
 * THIS FILE IS GUEST x86-64 CODE.  It is compiled into steamclient64.dll
 * alongside Proton's PE-side wrappers and runs under the emulator like the
 * game does, so its socket calls are ordinary guest imports from the ws2_32
 * thunk and reach native Wine the same way the game's do.
 *
 * NO HELPER IS NOT AN ERROR.  A game launched outside Steam has no client to
 * talk to, and the honest answer is the one a Linux game gets: every call
 * fails, load_steamclient() returns 0, CreateInterface returns NULL, and
 * steam_api prints its own "SteamAPI_Init() failed".  That path is reached by
 * returning a failing status here, never by crashing and never by inventing a
 * plausible reply.
 *
 * Copyright 2026 the ppc64le port authors
 *
 * This library is free software; you can redistribute it and/or modify it
 * under the terms of the GNU Lesser General Public License as published by
 * the Free Software Foundation; either version 2.1 of the License, or (at
 * your option) any later version.
 */

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "windef.h"
#include "winbase.h"
#include "winsock2.h"
#include "winreg.h"

#include "steamrpc_private.h"

/* ------------------------------------------------------------- debug output */

static LONG dbg_level = -1;   /* lowest class that is logged */

static void dbg_init(void)
{
    char buf[256];
    DWORD n;

    /* One knob, read once: STEAMBRIDGEDEBUG=trace|warn|fixme|err|off.  The
     * module cannot read WINEDEBUG's channel state because the guest ntdll
     * thunk exports none of the __wine_dbg_* entry points that would answer
     * it, so it carries its own rather than pretending. */
    n = GetEnvironmentVariableA( "STEAMBRIDGEDEBUG", buf, sizeof(buf) );
    if (!n || n >= sizeof(buf)) { dbg_level = __WINE_DBCL_FIXME; return; }
    if (!strcmp( buf, "trace" ))      dbg_level = __WINE_DBCL_TRACE;
    else if (!strcmp( buf, "warn" ))  dbg_level = __WINE_DBCL_WARN;
    else if (!strcmp( buf, "fixme" )) dbg_level = __WINE_DBCL_FIXME;
    else if (!strcmp( buf, "err" ))   dbg_level = __WINE_DBCL_ERR;
    else                              dbg_level = 4;   /* off */
}

int steamclient_dbg_enabled( unsigned int cls )
{
    if (dbg_level < 0) dbg_init();
    return (LONG)cls >= dbg_level;
}

void steamclient_dbg_log( unsigned int cls, const char *func, const char *fmt, ... )
{
    static const char *const names[] = { "trace", "warn", "fixme", "err" };
    char line[1024];
    int off;
    va_list ap;

    if (!steamclient_dbg_enabled( cls )) return;
    off = _snprintf( line, sizeof(line), "%s:steamclient:%s ",
                     names[cls < 4 ? cls : 3], func );
    if (off < 0 || off >= (int)sizeof(line)) return;
    va_start( ap, fmt );
    _vsnprintf( line + off, sizeof(line) - off, fmt, ap );
    va_end( ap );
    line[sizeof(line) - 1] = 0;
    /* Straight to the process's stderr.  OutputDebugStringA would be the
     * Windows-idiomatic choice, but Wine routes it through a WARN on
     * kernelbase's own channel AND raises DBG_PRINTEXCEPTION_C on every call
     * -- an exception per log line, dispatched through this port's guest
     * exception path.  A bridge that cannot say why it failed without also
     * raising exceptions is not a diagnostic. */
    {
        DWORD n = 0;
        int len = 0;
        while (line[len]) len++;
        WriteFile( GetStdHandle( STD_ERROR_HANDLE ), line, len, &n, NULL );
    }
}

/* Two rotating slots, like Wine's own debug strings: enough for the one
 * TRACE that prints two of them, and no allocation on a logging path. */
const char *steamclient_dbgstr_a( const char *s )
{
    static char buf[2][256];
    static LONG slot;
    char *out = buf[InterlockedIncrement( &slot ) & 1];

    if (!s) return "(null)";
    _snprintf( out, 256, "\"%.240s\"", s );
    out[255] = 0;
    return out;
}

const char *steamclient_dbgstr_w( const WCHAR *s )
{
    static char buf[2][256];
    static LONG slot;
    char *out = buf[InterlockedIncrement( &slot ) & 1];
    int i;

    if (!s) return "(null)";
    out[0] = 'L'; out[1] = '"';
    for (i = 0; i < 240 && s[i]; i++) out[2 + i] = s[i] < 0x80 ? (char)s[i] : '?';
    out[2 + i] = '"';
    out[3 + i] = 0;
    return out;
}

/* --------------------------------------------------------------- transport */

static CRITICAL_SECTION rpc_cs;
static CRITICAL_SECTION_DEBUG rpc_cs_debug =
{
    0, 0, &rpc_cs,
    { &rpc_cs_debug.ProcessLocksList, &rpc_cs_debug.ProcessLocksList },
      0, 0, { (DWORD_PTR)(__FILE__ ": steamrpc") }
};
static CRITICAL_SECTION rpc_cs = { &rpc_cs_debug, -1, 0, 0, 0, 0 };

static SOCKET rpc_sock = INVALID_SOCKET;
static int rpc_state;           /* 0 = untried, 1 = connected, 2 = dead */
static char *rpc_frame;         /* one reusable request/reply buffer */
static unsigned int rpc_frame_size;
static char *rpc_tmppath;       /* the g_tmppath the PE side handed the helper */

/* THE BRIDGE DYING MUST NOT KILL THE GAME.  A Steam client can exit, crash or
 * be killed while a Windows game is running, and games survive it: every
 * Steamworks call starts failing and steam_api returns its zero values.  The
 * helper is this port's Steam client, so its death has to look the same.
 *
 * This poisons the connection permanently and deliberately.  rpc_state 2 is
 * terminal -- nothing ever clears it, no reconnection is attempted -- so every
 * later call short-circuits to STEAMRPC_STATUS_NO_HELPER before touching a
 * socket, Proton's wrappers see a failing status and return the zero-filled
 * `_ret` their params struct was initialised with, and the game keeps running
 * without Steam.  Reconnecting instead would be worse than useless: a new
 * helper is a new steamclient.so with no pipes, no user and none of the
 * handles this process already handed the game.
 *
 * The message is printed once.  A game polls Steam_BGetCallback every frame,
 * and a per-frame ERR would bury the one line that says what happened. */
static void rpc_die( const char *why )
{
    if (rpc_sock != INVALID_SOCKET) closesocket( rpc_sock );
    rpc_sock = INVALID_SOCKET;
    if (rpc_state != 2)
        ERR( "steam bridge is down: %s.  Every Steamworks call from now on "
             "fails the way they do when Steam exits under a game on Windows; "
             "this process carries on without it.\n", why );
    rpc_state = 2;
}

/* ONE VARIABLE PER WIDTH, and it has to be two rather than one.
 *
 * A frame is the params struct's own bytes, so a client can only ever talk to
 * a helper of its own pointer width (steamrpc_wire.h's magic says so, and the
 * helper enforces it).  The launcher therefore starts BOTH helpers and each
 * gets its own port -- it cannot know in advance which width will ask, because
 * a 64-bit launcher .exe spawning a 32-bit game (or the reverse) is ordinary
 * and both processes load a steamclient DLL of their own width.
 *
 * Reading one shared variable and letting the magic reject the mismatch would
 * be correct but useless: it would connect a 32-bit game to the 64-bit helper
 * and then refuse every call, which is exactly the STEAMRPC_STATUS_NO_HELPER
 * outcome this exists to remove.  So the name carries the width and each
 * client reads only its own; a helper that is never asked for costs one idle
 * process that the launcher reaps.
 *
 * There is still no default and no discovery: whoever started the helper knows
 * the port, and inventing one would connect a game to a stranger's socket. */
#ifdef __i386__
#define STEAMRPC_ADDR_VAR "STEAM_BRIDGE_ADDR32"
#else
#define STEAMRPC_ADDR_VAR "STEAM_BRIDGE_ADDR"
#endif

static int rpc_connect(void)
{
    char addr[128], *colon;
    struct sockaddr_in sin;
    WSADATA wsa;
    DWORD n;
    u_short port;

    n = GetEnvironmentVariableA( STEAMRPC_ADDR_VAR, addr, sizeof(addr) );
    if (!n || n >= sizeof(addr))
    {
        WARN( STEAMRPC_ADDR_VAR " is unset; no Steam client is reachable from "
              "this process\n" );
        return 0;
    }
    if (!(colon = strrchr( addr, ':' )))
    {
        ERR( STEAMRPC_ADDR_VAR "=%s is not host:port\n", addr );
        return 0;
    }
    *colon = 0;
    port = (u_short)atoi( colon + 1 );
    if (!port)
    {
        ERR( STEAMRPC_ADDR_VAR " names port 0\n" );
        return 0;
    }

    if (WSAStartup( 0x202, &wsa ))
    {
        ERR( "WSAStartup failed\n" );
        return 0;
    }
    if ((rpc_sock = socket( AF_INET, SOCK_STREAM, IPPROTO_TCP )) == INVALID_SOCKET)
    {
        ERR( "socket() failed, err %d\n", WSAGetLastError() );
        return 0;
    }
    memset( &sin, 0, sizeof(sin) );
    sin.sin_family = AF_INET;
    sin.sin_port = htons( port );
    sin.sin_addr.s_addr = inet_addr( addr );
    if (connect( rpc_sock, (struct sockaddr *)&sin, sizeof(sin) ))
    {
        ERR( "cannot reach the Steam bridge helper at %s:%u, err %d\n",
             addr, port, WSAGetLastError() );
        closesocket( rpc_sock );
        rpc_sock = INVALID_SOCKET;
        return 0;
    }
    {
        int one = 1;
        setsockopt( rpc_sock, IPPROTO_TCP, TCP_NODELAY, (const char *)&one,
                    sizeof(one) );
    }
    TRACE( "connected to the Steam bridge helper at %s:%u\n", addr, port );
    return 1;
}

static int rpc_send_all( const char *p, unsigned int len )
{
    while (len)
    {
        int n = send( rpc_sock, p, (int)len, 0 );
        if (n <= 0) return 0;
        p += n;
        len -= n;
    }
    return 1;
}

static int rpc_recv_all( char *p, unsigned int len )
{
    while (len)
    {
        int n = recv( rpc_sock, p, (int)len, 0 );
        if (n <= 0) return 0;
        p += n;
        len -= n;
    }
    return 1;
}

static char *rpc_reserve( unsigned int need )
{
    char *p;

    if (need <= rpc_frame_size) return rpc_frame;
    if (need > STEAMRPC_MAX_FRAME) return NULL;
    need = (need + 0xffff) & ~0xffffu;
    /* HeapReAlloc is not defined for a NULL block, so the first allocation is
     * spelled out rather than relied on. */
    if (rpc_frame) p = HeapReAlloc( GetProcessHeap(), 0, rpc_frame, need );
    else p = HeapAlloc( GetProcessHeap(), 0, need );
    if (!p) return NULL;
    rpc_frame = p;
    rpc_frame_size = need;
    return p;
}

/* ------------------------------------------------------------- marshalling */

/* One line per method, however often a game calls it: these fire on paths a
 * game polls every frame. */
static int say_once( unsigned int code, unsigned char bit )
{
    static unsigned char *logged;

    if (code >= steamrpc_method_count) return 1;   /* a local code; say it */
    if (!logged && !(logged = HeapAlloc( GetProcessHeap(), HEAP_ZERO_MEMORY,
                                         steamrpc_method_count )))
        return 0;
    if (logged[code] & bit) return 0;
    logged[code] |= bit;
    return 1;
}

static void refuse_once( unsigned int code, const struct steamrpc_method *m )
{
    if (say_once( code, 1 )) ERR( "refusing %s: %s\n", m->name, m->refuse );
}

/* The helper caught something writing past the end of a marshalled buffer and
 * refused the call rather than dying of it.  The helper has the byte counts;
 * this end has the names, so this is where the two are put together. */
static void overflow_once( unsigned int code, const struct steamrpc_method *m,
                           unsigned int offset )
{
    const char *param = "(not a marshalled field)";
    unsigned int i;

    if (!say_once( code, 2 )) return;
    for (i = 0; i < m->nfields; i++)
        if (m->fields[i].offset == offset) { param = m->fields[i].name; break; }
    ERR( "%s: the helper caught the Steam client writing past the end of the "
         "buffer marshalled for %s, and refused the call rather than "
         "corrupting its own heap.  That parameter's marshal descriptor is too "
         "small -- see tools/steamrpc/gen-steamrpc, which sizes arrays from "
         "Valve's own STEAM_*_COUNT annotations.\n", m->name, param );
}

static unsigned int field_len( const struct steamrpc_method *m,
                               const struct steamrpc_field *f,
                               unsigned int idx, const void *params,
                               const void *ptr )
{
    switch (f->kind)
    {
    case STEAMRPC_K_FIXED:
        return f->elem;
    case STEAMRPC_K_STR:
        return (unsigned int)strlen( (const char *)ptr ) + 1;
    case STEAMRPC_K_SIZED:
    {
        unsigned int off = f->lenref & 0xffff, width = f->lenref >> 16;
        const unsigned char *at = (const unsigned char *)params + off;
        unsigned long long count = 0;

        switch (width)
        {
        case 1: count = *(const unsigned char *)at; break;
        case 2: count = *(const unsigned short *)at; break;
        case 4: count = *(const unsigned int *)at; break;
        case 8: count = *(const unsigned long long *)at; break;
        }
        /* A signed count arrives here as a huge unsigned one; the frame cap
         * turns that into a named refusal rather than a wild read. */
        if (count > STEAMRPC_MAX_FRAME) return ~0u;
        return (unsigned int)(count * f->elem);
    }
    case STEAMRPC_K_EXPR:
        return m->lenfn ? m->lenfn( params, idx ) : 0;
    case STEAMRPC_K_NESTED:
    {
        /* ptr is the OUTER object here; the length lives inside it. */
        unsigned int off = f->lenref & 0xffff, width = f->lenref >> 16;
        const unsigned char *at = (const unsigned char *)ptr + off;
        unsigned long long count = 0;

        switch (width)
        {
        case 1: count = *(const unsigned char *)at; break;
        case 2: count = *(const unsigned short *)at; break;
        case 4: count = *(const unsigned int *)at; break;
        case 8: count = *(const unsigned long long *)at; break;
        }
        if (count > STEAMRPC_MAX_FRAME) return ~0u;
        return (unsigned int)count;
    }
    }
    return ~0u;
}

/* The pointer a descriptor actually copies: for everything but a nested
 * field that is the params field itself, and for a nested field it is the
 * pointer at `elem` bytes into the object the params field points at. */
static void *field_ptr( const struct steamrpc_field *f, void *params, void **outer )
{
    void *p = *(void **)((char *)params + f->offset);

    *outer = p;
    if (f->kind != STEAMRPC_K_NESTED || !p) return p;
    return *(void **)((char *)p + f->elem);
}

/* ---------------------------------------------------------------- selftest */

#define _P struct steamrpc_selftest_params

static unsigned int selftest_len( const void *_v, unsigned int _i )
{
    const _P *_p = (const _P *)_v;

    switch (_i)
    {
    case 1: return _p->str_out_len;   /* selftest_fields[1] */
    case 3: return _p->buf_len;       /* selftest_fields[3] */
    }
    return 0;
}

/* Deliberately hand-written, and deliberately one of each class the generator
 * emits: an in-string, an out buffer with an explicit length, a fixed struct
 * by pointer, a caller buffer with a length, plus an opaque handle and an
 * in-place scalar travelling in the params blob itself. */
static const struct steamrpc_field selftest_fields[] =
{
    { "str_in",  offsetof(_P, str_in),  STEAMRPC_IN,
      STEAMRPC_K_STR,   0, 0 },
    { "str_out", offsetof(_P, str_out), STEAMRPC_IN | STEAMRPC_OUT,
      STEAMRPC_K_EXPR,  0, 0 },
    { "fixed",   offsetof(_P, fixed),   STEAMRPC_IN | STEAMRPC_OUT,
      STEAMRPC_K_FIXED, sizeof(struct steamrpc_selftest_fixed), 0 },
    { "buf",     offsetof(_P, buf),     STEAMRPC_IN | STEAMRPC_OUT,
      STEAMRPC_K_EXPR,  0, 0 },
};
#undef _P

/* selftest_len is indexed by the field's position in the frame builder's
 * loop, which is its index in selftest_fields[]; keep the two in step. */
static const struct steamrpc_method selftest_method =
{
    "steamrpc_selftest", sizeof(struct steamrpc_selftest_params),
    ARRAY_SIZE(selftest_fields), selftest_fields, NULL, selftest_len
};

/* ------------------------------------------------------------ local calls
 *
 * The other three frames this port speaks that Proton does not, described the
 * same way a generated method is so that they travel through the same frame
 * builder rather than beside it.  See steamrpc_wire.h for what each is for.
 */

#define _P struct steamrpc_setdrives_params
static const struct steamrpc_field setdrives_fields[] =
{
    { "drives", offsetof(_P, drives), STEAMRPC_IN, STEAMRPC_K_SIZED,
      sizeof(struct steamrpc_drive),
      offsetof(_P, count) | (sizeof(((_P *)0)->count) << 16) },
};
#undef _P

static const struct steamrpc_method setdrives_method =
{
    "steamrpc_setdrives", sizeof(struct steamrpc_setdrives_params),
    ARRAY_SIZE(setdrives_fields), setdrives_fields, NULL, NULL
};

#define _P struct steamrpc_inject_params
static const struct steamrpc_field inject_fields[] =
{
    { "payload", offsetof(_P, payload), STEAMRPC_IN, STEAMRPC_K_SIZED, 1,
      offsetof(_P, len) | (sizeof(((_P *)0)->len) << 16) },
};
#undef _P

static const struct steamrpc_method inject_method =
{
    "steamrpc_inject", sizeof(struct steamrpc_inject_params),
    ARRAY_SIZE(inject_fields), inject_fields, NULL, NULL
};

#define _P struct steamrpc_pathtest_params

static unsigned int pathtest_len( const void *_v, unsigned int _i )
{
    const _P *_p = (const _P *)_v;

    switch (_i)
    {
    case 1: return _p->unix_len;   /* pathtest_fields[1] */
    case 2: return _p->back_len;   /* pathtest_fields[2] */
    }
    return 0;
}

static const struct steamrpc_field pathtest_fields[] =
{
    { "dos_in",   offsetof(_P, dos_in),   STEAMRPC_IN,
      STEAMRPC_K_STR,  0, 0 },
    { "unix_out", offsetof(_P, unix_out), STEAMRPC_IN | STEAMRPC_OUT,
      STEAMRPC_K_EXPR, 0, 0 },
    { "dos_back", offsetof(_P, dos_back), STEAMRPC_IN | STEAMRPC_OUT,
      STEAMRPC_K_EXPR, 0, 0 },
};
#undef _P

static const struct steamrpc_method pathtest_method =
{
    "steamrpc_pathtest", sizeof(struct steamrpc_pathtest_params),
    ARRAY_SIZE(pathtest_fields), pathtest_fields, NULL, pathtest_len
};

#define _P struct steamrpc_overrun_params

static unsigned int overrun_len( const void *_v, unsigned int _i )
{
    const _P *_p = (const _P *)_v;

    return _i == 0 ? _p->len : 0;   /* overrun_fields[0] */
}

/* Note what this descriptor says and does not say: `len` bytes.  The helper
 * writes more than that on purpose, which is the whole point -- the wire
 * carries the length the marshal plan believes, and the guard on the far side
 * is what notices when the truth is bigger. */
static const struct steamrpc_field overrun_fields[] =
{
    { "buf", offsetof(_P, buf), STEAMRPC_IN | STEAMRPC_OUT,
      STEAMRPC_K_EXPR, 0, 0 },
};
#undef _P

static const struct steamrpc_method overrun_method =
{
    "steamrpc_overrun", sizeof(struct steamrpc_overrun_params),
    ARRAY_SIZE(overrun_fields), overrun_fields, NULL, overrun_len
};

static const struct steamrpc_method *local_method( unsigned int code )
{
    switch (code)
    {
    case STEAMRPC_CODE_SELFTEST:  return &selftest_method;
    case STEAMRPC_CODE_SETDRIVES: return &setdrives_method;
    case STEAMRPC_CODE_INJECT:    return &inject_method;
    case STEAMRPC_CODE_PATHTEST:  return &pathtest_method;
    case STEAMRPC_CODE_OVERRUN:   return &overrun_method;
    }
    return NULL;
}

static void rpc_send_drives( void );

NTSTATUS steamrpc_call( unsigned int code, void *params )
{
    const struct steamrpc_method *m;
    struct steamrpc_req_hdr *req;
    struct steamrpc_rep_hdr rep;
    void *saved[32], *outers[32];
    unsigned int lens[32];
    unsigned int i, nb = 0, need, off;
    NTSTATUS status;
    char *buf;

    if (steamrpc_is_local_code( code )) m = local_method( code );
    else if (code >= steamrpc_method_count) m = NULL;
    else m = &steamrpc_methods[code];
    if (!m)
    {
        ERR( "unix call code %#x is out of range\n", code );
        return (NTSTATUS)STEAMRPC_STATUS_PROTOCOL;
    }
    if (m->refuse)
    {
        refuse_once( code, m );
        return (NTSTATUS)STEAMRPC_STATUS_REFUSED;
    }
    if (m->nfields > ARRAY_SIZE(saved))
    {
        ERR( "%s has %u marshalled fields, more than this frame builder "
             "carries\n", m->name, m->nfields );
        return (NTSTATUS)STEAMRPC_STATUS_PROTOCOL;
    }

    RtlEnterCriticalSection( &rpc_cs );

    if (!rpc_state)
    {
        rpc_state = rpc_connect() ? 1 : 2;
        if (rpc_state == 2 && rpc_sock != INVALID_SOCKET) rpc_die( "connect" );
        /* The drive map is a property of the CONNECTION, not of any one
         * method, and the helper needs it before the first call that converts
         * a path -- which can be the very next one.  rpc_state is already 1,
         * so the nested steamrpc_call below takes the ordinary path; the
         * critical section is recursive and the frame buffer is untouched
         * until the measuring loop further down. */
        if (rpc_state == 1) rpc_send_drives();
    }
    if (rpc_state != 1)
    {
        RtlLeaveCriticalSection( &rpc_cs );
        return (NTSTATUS)STEAMRPC_STATUS_NO_HELPER;
    }

    /* Measure first: the frame is one allocation, and a length that cannot be
     * trusted is refused before anything is copied. */
    need = sizeof(*req) + m->params_size;
    for (i = 0; i < m->nfields; i++)
    {
        const struct steamrpc_field *f = &m->fields[i];
        void *outer = NULL, *ptr = NULL;

        if (m->params_size) ptr = field_ptr( f, params, &outer );

        saved[i] = ptr;
        outers[i] = outer;
        lens[i] = 0;
        if (!ptr) continue;
        /* a nested field measures itself against its OUTER object */
        lens[i] = field_len( m, f, i, f->kind == STEAMRPC_K_NESTED ? outer : params,
                             f->kind == STEAMRPC_K_NESTED ? outer : ptr );
        if (lens[i] == ~0u || (unsigned long long)need + lens[i] +
            sizeof(struct steamrpc_blob_hdr) > STEAMRPC_MAX_FRAME)
        {
            ERR( "%s: parameter %s has an unusable length; refusing rather "
                 "than truncating it\n", m->name, f->name );
            RtlLeaveCriticalSection( &rpc_cs );
            return (NTSTATUS)STEAMRPC_STATUS_TOO_BIG;
        }
        need += sizeof(struct steamrpc_blob_hdr) + lens[i];
        nb++;
    }

    if (!(buf = rpc_reserve( need )))
    {
        rpc_die( "out of memory building a frame" );
        RtlLeaveCriticalSection( &rpc_cs );
        return (NTSTATUS)STEAMRPC_STATUS_TOO_BIG;
    }

    req = (struct steamrpc_req_hdr *)buf;
    req->magic = STEAMRPC_REQ_MAGIC;
    req->code = code;
    req->params_len = m->params_size;
    req->nblobs = nb;
    off = sizeof(*req);
    if (m->params_size) memcpy( buf + off, params, m->params_size );
    off += m->params_size;
    for (i = 0; i < m->nfields; i++)
    {
        struct steamrpc_blob_hdr *bh;

        if (!saved[i]) continue;
        bh = (struct steamrpc_blob_hdr *)(buf + off);
        bh->offset = m->fields[i].offset;
        bh->len = lens[i];
        bh->flags = m->fields[i].flags;
        bh->inner = 0;
        if (m->fields[i].kind == STEAMRPC_K_NESTED)
        {
            bh->flags |= STEAMRPC_NESTED;
            bh->inner = m->fields[i].elem;
        }
        off += sizeof(*bh);
        if (lens[i]) memcpy( buf + off, saved[i], lens[i] );
        off += lens[i];
    }

    if (!rpc_send_all( buf, off ) ||
        !rpc_recv_all( (char *)&rep, sizeof(rep) ))
    {
        rpc_die( "the helper closed the connection" );
        RtlLeaveCriticalSection( &rpc_cs );
        return (NTSTATUS)STEAMRPC_STATUS_NO_HELPER;
    }
    if (rep.magic != STEAMRPC_REP_MAGIC || rep.params_len != m->params_size)
    {
        rpc_die( "the helper sent a frame this client does not understand" );
        RtlLeaveCriticalSection( &rpc_cs );
        return (NTSTATUS)STEAMRPC_STATUS_PROTOCOL;
    }
    if (rep.status == STEAMRPC_STATUS_OVERFLOW) overflow_once( code, m, rep.detail );

    /* Every reply section is bounded by something this client already sent --
     * the params struct, a blob no longer than the one it was given, and the
     * fixed temp-path image -- so the request buffer is big enough to read
     * the reply back into, one section at a time. */
    buf = rpc_frame;

    if (rep.params_len && !rpc_recv_all( buf, rep.params_len ))
    {
        rpc_die( "short reply" );
        RtlLeaveCriticalSection( &rpc_cs );
        return (NTSTATUS)STEAMRPC_STATUS_NO_HELPER;
    }
    if (rep.params_len)
    {
        memcpy( params, buf, rep.params_len );
        /* Every pointer field in the reply holds a HELPER address.  Put the
         * caller's own pointers back before anything can dereference one. */
        for (i = 0; i < m->nfields; i++)
            if (m->fields[i].kind != STEAMRPC_K_NESTED)
                *(void **)((char *)params + m->fields[i].offset) = saved[i];
    }

    for (i = 0; i < rep.nblobs; i++)
    {
        struct steamrpc_blob_hdr bh;
        unsigned int j, copy;

        if (!rpc_recv_all( (char *)&bh, sizeof(bh) ))
        {
            rpc_die( "short reply blob header" );
            RtlLeaveCriticalSection( &rpc_cs );
            return (NTSTATUS)STEAMRPC_STATUS_NO_HELPER;
        }
        if (bh.len > rpc_frame_size)
        {
            rpc_die( "the helper returned more than it was given" );
            RtlLeaveCriticalSection( &rpc_cs );
            return (NTSTATUS)STEAMRPC_STATUS_PROTOCOL;
        }
        if (bh.len && !rpc_recv_all( buf, bh.len ))
        {
            rpc_die( "short reply blob" );
            RtlLeaveCriticalSection( &rpc_cs );
            return (NTSTATUS)STEAMRPC_STATUS_NO_HELPER;
        }
        /* Only ever write back into the buffer the caller gave us, and only
         * as many bytes as we told the helper it had. */
        copy = 0;
        for (j = 0; j < m->nfields; j++)
        {
            BOOL nested = (m->fields[j].kind == STEAMRPC_K_NESTED);

            if (m->fields[j].offset != bh.offset || !saved[j]) continue;
            if (nested != !!(bh.flags & STEAMRPC_NESTED)) continue;
            if (nested && m->fields[j].elem != bh.inner) continue;
            copy = bh.len < lens[j] ? bh.len : lens[j];
            if (copy) memcpy( saved[j], buf, copy );
            break;
        }
        if (j == m->nfields)
            WARN( "%s: reply names offset %u, which is not a marshalled "
                  "field; dropping it\n", m->name, bh.offset );
    }

    /* A NESTED field's pointer lives inside another blob, and that blob has
     * just been copied back verbatim -- so it now holds the address the
     * HELPER patched into it, pointing at the helper's copy of the payload.
     * The payload itself was written back into the caller's own buffer a few
     * lines above; this puts the caller's pointer back on top of it.
     *
     * Skipping this is not a cosmetic defect.  The one nested field in the
     * whole surface is Steam_BGetCallback's w_CallbackMsg_t::m_pubParam, and
     * a game reads its callback payload through exactly that pointer the
     * instant Steam_BGetCallback returns: leaving a helper address there is
     * an unmapped read in the game, on the first callback it is ever handed.
     * The out-of-params pointers get the same treatment for the same reason
     * (see the params memcpy above); this is that rule applied one level in. */
    for (i = 0; i < m->nfields; i++)
        if (m->fields[i].kind == STEAMRPC_K_NESTED && outers[i])
            *(void **)((char *)outers[i] + m->fields[i].elem) = saved[i];

    if (rep.tmppath_len)
    {
        if (rep.tmppath_len > STEAMRPC_TMPPATH_LEN ||
            !rpc_recv_all( buf, rep.tmppath_len ))
        {
            rpc_die( "bad temp-path image" );
            RtlLeaveCriticalSection( &rpc_cs );
            return (NTSTATUS)STEAMRPC_STATUS_NO_HELPER;
        }
        if (rpc_tmppath) memcpy( rpc_tmppath, buf, rep.tmppath_len );
    }
    if (code == unix_steamclient_init && m->params_size)
        rpc_tmppath = ((struct steamclient_init_params *)params)->g_tmppath;

    status = (NTSTATUS)rep.status;
    RtlLeaveCriticalSection( &rpc_cs );
    return status;
}

/* ------------------------------------------------------------ the drives
 *
 * Measured once per connection, from the one party that can measure it: this
 * process, which is in the prefix.  See the drive-map comment in
 * steamrpc_wire.h for why the mapping travels and the joining does not.
 */
static void rpc_send_drives( void )
{
    char * (CDECL *get_unix_file_name)( const WCHAR * );
    struct steamrpc_drive drives[STEAMRPC_DRIVE_MAX];
    struct steamrpc_setdrives_params p;
    WCHAR root[4] = L"A:\\";
    unsigned int n = 0, len;
    HMODULE k32;
    WCHAR c;

    /* wine_get_unix_file_name is Wine's own entry point and not a Windows
     * API, so it is resolved by name at run time -- the same way winepath.exe
     * reaches it.  A thunk that does not export it is a named failure here
     * rather than a wrong path in someone's save file later. */
    if (!(k32 = GetModuleHandleA( "kernel32.dll" )) ||
        !(get_unix_file_name = (char * (CDECL *)( const WCHAR * ))
                               GetProcAddress( k32, "wine_get_unix_file_name" )))
    {
        ERR( "kernel32 does not export wine_get_unix_file_name, so this "
             "process cannot tell the helper where its drives are; every "
             "Steam file path will cross unconverted\n" );
        return;
    }

    for (c = 'A'; c <= 'Z'; c++)
    {
        char *unix_name;

        /* Wine answers for a drive that exists and returns NULL for one that
         * does not, which is exactly the enumeration wanted: no dosdevices
         * directory is read here and no letter is assumed. */
        root[0] = c;
        if (!(unix_name = get_unix_file_name( root ))) continue;
        for (len = 0; unix_name[len]; len++) /* nothing */;
        while (len > 1 && unix_name[len - 1] == '/') len--;   /* helper joins */
        if (len && len < STEAMRPC_DRIVE_ROOT_LEN)
        {
            memset( &drives[n], 0, sizeof(drives[n]) );
            drives[n].letter = (char)c;
            memcpy( drives[n].root, unix_name, len );
            n++;
        }
        else
            WARN( "drive %c: resolves to a %u-byte unix path, which this wire "
                  "does not carry; leaving it out rather than truncating it\n",
                  (char)c, len );
        HeapFree( GetProcessHeap(), 0, unix_name );
    }

    if (!n)
    {
        ERR( "Wine named no drive root in this prefix; RemoteStorage and UGC "
             "paths cannot be converted\n" );
        return;
    }

    memset( &p, 0, sizeof(p) );
    p.count = n;
    p.stride = sizeof(struct steamrpc_drive);
    p.drives = drives;
    if (steamrpc_call( STEAMRPC_CODE_SETDRIVES, &p ))
        ERR( "the helper refused this prefix's drive map; Steam file paths "
             "will cross unconverted\n" );
    else if (p._ret != n)
        ERR( "the helper kept %u of this prefix's %u drives; paths on the "
             "rest will cross unconverted\n", p._ret, n );
    else
        TRACE( "the helper has this prefix's %u drives\n", n );
}

/* ------------------------------------------------------------- discovery
 *
 * HKCU\Software\Valve\Steam\ActiveProcess is how steam_api64.dll finds a
 * Steam client on Windows: SteamClientDll64 names the DLL to load and pid
 * names the process that owns the connection.  The port writes the string
 * values from the compat tool, where the prefix is; the pid can only be
 * written by a live process in that prefix, so it is written here, from the
 * DllMain of the very module the registry pointed at.
 *
 * The value is deliberately GetCurrentProcessId() rather than something
 * borrowed: there is no steam.exe in this prefix, and the process holding the
 * socket to the helper -- which is what "the Steam connection" means here --
 * is this one.
 */
void steamrpc_publish_active_process( void )
{
    static const WCHAR keyW[] = L"Software\\Valve\\Steam\\ActiveProcess";
    DWORD pid = GetCurrentProcessId(), disp = 0;
    HKEY key;

    if (RegCreateKeyExW( HKEY_CURRENT_USER, keyW, 0, NULL, 0, KEY_SET_VALUE,
                         NULL, &key, &disp ))
    {
        WARN( "cannot open %s; a game that checks whether Steam is running "
              "will decide it is not\n", "HKCU\\Software\\Valve\\Steam\\ActiveProcess" );
        return;
    }
    RegSetValueExW( key, L"pid", 0, REG_DWORD, (const BYTE *)&pid, sizeof(pid) );
    RegCloseKey( key );
    TRACE( "published ActiveProcess pid %lu\n", pid );
}

/* ------------------------------------------------------- the exported test
 *
 * ppc64le/steamapi/check-steam-bridge.sh drives this from a guest probe.  It
 * lives in the shipped DLL rather than in a test-only build so that the code
 * under test is the code that ships: the frame it sends goes through the same
 * steamrpc_call, the same descriptor walk and the same socket as a real
 * Steamworks method.  Returns 0 for pass, or the OR of the
 * STEAMRPC_SELFTEST_BAD_* bits that failed.
 */
int CDECL __wine_steamrpc_selftest( void )
{
    struct steamrpc_selftest_fixed fixed = { 0x11223344u, 0x55667788aabbccddull,
                                             0xeeffu };
    unsigned char buf[257];
    char str_out[32];
    static const char in_str[] = STEAMRPC_SELFTEST_IN;
    struct steamrpc_selftest_params p;
    unsigned int i;
    int bad = 0;

    memset( str_out, 0x7e, sizeof(str_out) );
    for (i = 0; i < sizeof(buf); i++) buf[i] = (unsigned char)(i ^ 0x3c);

    memset( &p, 0, sizeof(p) );
    p.buf_len = sizeof(buf);
    p.handle = 0x0123456789abcdefull;
    p.str_in = in_str;
    p.str_out = str_out;
    p.str_out_len = sizeof(str_out);
    p.fixed = &fixed;
    p.buf = buf;

    if (steamrpc_call( STEAMRPC_CODE_SELFTEST, &p ))
        return STEAMRPC_SELFTEST_BAD_STATUS;

    if (p._ret != STEAMRPC_SELFTEST_RET)       bad |= STEAMRPC_SELFTEST_BAD_RET;
    /* the helper reports what it saw of the in-string in the low bit of _ret's
     * companion: it sets buf_len to 0 when the string did not match */
    if (!p.buf_len)                            bad |= STEAMRPC_SELFTEST_BAD_STR_IN;
    if (strcmp( str_out, STEAMRPC_SELFTEST_OUT )) bad |= STEAMRPC_SELFTEST_BAD_STR_OUT;
    if (fixed.a != 0x11223345u || fixed.b != 0x55667788aabbccdeull ||
        fixed.c != 0xef00u)                    bad |= STEAMRPC_SELFTEST_BAD_FIXED;
    for (i = 0; i < sizeof(buf); i++)
        if (buf[i] != (unsigned char)(i * 7 + 3)) { bad |= STEAMRPC_SELFTEST_BAD_BUF; break; }
    if (p.handle != (0x0123456789abcdefull ^ STEAMRPC_SELFTEST_XOR))
        bad |= STEAMRPC_SELFTEST_BAD_HANDLE;
    /* the caller's own pointers must come back unchanged: a helper address
     * left in the params struct is the defect this whole layer exists to
     * prevent, and it would be invisible until something dereferenced it */
    if (p.str_out != str_out || p.buf != buf || p.fixed != &fixed ||
        p.str_in != in_str)
        bad |= STEAMRPC_SELFTEST_BAD_INPLACE;

    return bad;
}

/* -------------------------------------------------- the exported guard probe
 *
 * Asks the helper to write `past` bytes beyond the end of a `len`-byte buffer
 * this end declared -- the shape of the defect that killed the helper at
 * DOOM's title screen -- so the gate can watch the red zone catch it.  Returns
 * 1 when the helper refused the call by name, which is the pass condition; 0
 * means the overrun went UNNOTICED, which is the whole failure being guarded
 * against.
 */
int CDECL __wine_steamrpc_overrun( unsigned int len, unsigned int past )
{
    unsigned char buf[64];
    struct steamrpc_overrun_params p;
    NTSTATUS status;

    if (len > sizeof(buf)) return 0;
    memset( buf, 0, sizeof(buf) );

    memset( &p, 0, sizeof(p) );
    p.len = len;
    p.past = past;
    p.buf = buf;

    status = steamrpc_call( STEAMRPC_CODE_OVERRUN, &p );
    return status == (NTSTATUS)STEAMRPC_STATUS_OVERFLOW;
}

/* ------------------------------------------------- the exported callback hook
 *
 * Puts one synthetic callback into the helper's end of a PULL channel, so that
 * the gate can then ask for it through the export a game asks through --
 * Steam_BGetCallback, which is also what steam_api64.dll's
 * SteamAPI_ManualDispatch_GetNextCallback is built on.  Nothing here delivers
 * a callback; delivery is Proton's own PE-side code, unmodified, and that is
 * the point of injecting at the far end rather than faking a delivery at this
 * one.  Returns 1 when the helper armed it.  See steamrpc_wire.h.
 */
int CDECL __wine_steamrpc_inject( unsigned int kind, void *func, unsigned int id )
{
    unsigned char payload[STEAMRPC_INJECT_LEN];
    struct steamrpc_inject_params p;
    unsigned int i;

    for (i = 0; i < sizeof(payload); i++) payload[i] = STEAMRPC_INJECT_BYTE( i );

    memset( &p, 0, sizeof(p) );
    p.kind = kind;
    p.func = (UINT64)(ULONG_PTR)func;
    p.id = id;
    p.len = sizeof(payload);
    p.payload = payload;

    if (steamrpc_call( STEAMRPC_CODE_INJECT, &p )) return 0;
    return (int)p._ret;
}

/* -------------------------------------------------- the exported path probe
 *
 * One DOS path through the helper's converters and back, so the gate can
 * compare both directions against a path it computed itself.  The functions
 * on the far end are the ones the 85 vendored path-bearing wrappers call, not
 * a copy of them.  Returns the byte count the unix-to-DOS direction reported,
 * or 0 if the call did not go through.
 */
int CDECL __wine_steamrpc_pathtest( const char *dos_in, char *unix_out,
                                    unsigned int unix_len, char *dos_back,
                                    unsigned int back_len, unsigned int *ndrives )
{
    struct steamrpc_pathtest_params p;

    if (unix_len) unix_out[0] = 0;
    if (back_len) dos_back[0] = 0;
    if (ndrives) *ndrives = 0;

    memset( &p, 0, sizeof(p) );
    p.dos_in = dos_in;
    p.unix_out = unix_out;
    p.unix_len = unix_len;
    p.dos_back = dos_back;
    p.back_len = back_len;

    if (steamrpc_call( STEAMRPC_CODE_PATHTEST, &p )) return 0;
    if (ndrives) *ndrives = p.ndrives;
    return (int)p._ret;
}
