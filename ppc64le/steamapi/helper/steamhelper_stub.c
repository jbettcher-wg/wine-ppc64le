/*
 * steamhelper_stub.c -- the eleven entry points a Wine unix library gets from
 * ntdll and a standalone Linux program does not.
 *
 * Proton's unix side is written to run inside Wine, so it reaches ntdll for
 * three things: debug output, the registry, and DOS<->unix path translation.
 * Two of those are answered here properly and one is refused by name:
 *
 *   DEBUG OUTPUT is real.  Proton's own TRACE/WARN/FIXME/ERR lines are the
 *   best diagnostic this bridge has for what the Steam client actually did,
 *   so they go to stderr, gated by STEAMHELPERDEBUG in the same
 *   "+channel"-free spirit as WINEDEBUG's classes: unset means warn and
 *   above, "trace" means everything, "off" means nothing.
 *
 *   CHARACTER CONVERSION is real.  ntdll_umbstowcs/wcstoumbs are plain
 *   UTF-8 <-> UTF-16 and the helper does them itself, so the path code that
 *   calls them behaves rather than corrupting.
 *
 *   PATH TRANSLATION IS REAL, and lives in steamhelper_path.c.  It needs the
 *   prefix's drive mapping, which the helper cannot have and the client can:
 *   the client measures it with Wine's own wine_get_unix_file_name and sends
 *   it once per connection, and the helper does the joining.  See that file.
 *
 *   THE REGISTRY IS REFUSED, LOUDLY.  NtCreateKey wants the calling process's
 *   registry and there is not one here.  Its only caller is
 *   steamclient_init_registry, which this port does not call at all: the
 *   ActiveProcess keys the game reads are written on the Wine side, by the
 *   compat tool and by this module's own DllMain, where the prefix actually
 *   is.  Answering "not implemented" makes that call fail legibly instead of
 *   half-succeeding.
 *
 * Copyright 2026 the ppc64le port authors
 *
 * This library is free software; you can redistribute it and/or modify it
 * under the terms of the GNU Lesser General Public License as published by
 * the Free Software Foundation; either version 2.1 of the License, or (at
 * your option) any later version.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ntstatus.h"
#define WIN32_NO_STATUS

#include "windef.h"
#include "winbase.h"
#include "winternl.h"
#include "wine/debug.h"
#include "wine/unixlib.h"

#include "steamrpc_count.h"

/* ---------------------------------------------------------------- dispatch */

extern const unixlib_entry_t __wine_unix_call_funcs[];

const unsigned int steamhelper_func_count = STEAMRPC_METHOD_COUNT;

NTSTATUS steamhelper_call( unsigned int code, void *params )
{
    if (code >= STEAMRPC_METHOD_COUNT) return STATUS_INVALID_PARAMETER;
    return __wine_unix_call_funcs[code]( params );
}

/* ------------------------------------------------------------------- debug */

/* Wine's classes are ordered FIXME, ERR, WARN, TRACE -- increasing noise, not
 * increasing severity -- so the gate is "class <= dbg_max".  Unset means what
 * an ordinary Wine run means: FIXME and ERR, nothing else. */
static int dbg_max = -2;

static void dbg_init(void)
{
    const char *v = getenv( "STEAMHELPERDEBUG" );

    if (!v)                         dbg_max = __WINE_DBCL_ERR;
    else if (!strcmp( v, "trace" )) dbg_max = __WINE_DBCL_TRACE;
    else if (!strcmp( v, "warn" ))  dbg_max = __WINE_DBCL_WARN;
    else if (!strcmp( v, "fixme" )) dbg_max = __WINE_DBCL_FIXME;
    else if (!strcmp( v, "err" ))   dbg_max = __WINE_DBCL_ERR;
    else                            dbg_max = -1;   /* off */
}

unsigned char __cdecl __wine_dbg_get_channel_flags( struct __wine_debug_channel *channel )
{
    int i;
    unsigned char flags = 0;

    if (dbg_max < -1) dbg_init();
    for (i = 0; i <= dbg_max; i++) flags |= (unsigned char)(1 << i);
    return flags;
}

/* Two rotating slots; the callers print at most two per line. */
const char * __cdecl __wine_dbg_strdup( const char *str )
{
    static char buf[8][512];
    static unsigned slot;
    char *out = buf[slot++ & 7];

    snprintf( out, sizeof(buf[0]), "%s", str ? str : "(null)" );
    return out;
}

/* Called only after __wine_dbg_header said yes, so it does not re-gate. */
int __cdecl __wine_dbg_output( const char *str )
{
    fputs( str, stderr );
    fflush( stderr );
    return (int)strlen( str );
}

int __cdecl __wine_dbg_header( enum __wine_debug_class cls,
                               struct __wine_debug_channel *channel,
                               const char *function )
{
    static const char *const names[] = { "fixme", "err", "warn", "trace" };

    if (dbg_max < -1) dbg_init();
    if ((int)cls > dbg_max) return -1;
    return fprintf( stderr, "helper:%s:%s:%s ",
                    (unsigned)cls < 4 ? names[cls] : "?",
                    channel ? channel->name : "steamclient",
                    function ? function : "" );
}

/* ------------------------------------------------------ character conversion */

DWORD ntdll_umbstowcs( const char *src, DWORD srclen, WCHAR *dst, DWORD dstlen )
{
    DWORD i = 0, out = 0;

    /* UTF-8 in, UTF-16 out.  Malformed input becomes U+FFFD rather than
     * being dropped, so a bad byte never shortens a path silently. */
    while (i < srclen && out < dstlen)
    {
        unsigned char c = (unsigned char)src[i];
        unsigned int cp;
        DWORD extra;

        if (c < 0x80)      { cp = c;         extra = 0; }
        else if (c < 0xe0) { cp = c & 0x1f;  extra = 1; }
        else if (c < 0xf0) { cp = c & 0x0f;  extra = 2; }
        else               { cp = c & 0x07;  extra = 3; }
        if (i + extra >= srclen + (extra ? 0 : 1)) { cp = 0xfffd; extra = 0; }
        i++;
        while (extra--)
        {
            if (i >= srclen || ((unsigned char)src[i] & 0xc0) != 0x80) { cp = 0xfffd; break; }
            cp = (cp << 6) | ((unsigned char)src[i++] & 0x3f);
        }
        if (cp >= 0x10000)
        {
            if (out + 2 > dstlen) break;
            cp -= 0x10000;
            dst[out++] = (WCHAR)(0xd800 + (cp >> 10));
            dst[out++] = (WCHAR)(0xdc00 + (cp & 0x3ff));
        }
        else dst[out++] = (WCHAR)cp;
    }
    return out;
}

int ntdll_wcstoumbs( const WCHAR *src, DWORD srclen, char *dst, DWORD dstlen, BOOL strict )
{
    DWORD i = 0, out = 0;

    while (i < srclen)
    {
        unsigned int cp = src[i++];

        if (cp >= 0xd800 && cp < 0xdc00 && i < srclen &&
            src[i] >= 0xdc00 && src[i] < 0xe000)
            cp = 0x10000 + ((cp - 0xd800) << 10) + (src[i++] - 0xdc00);

        if (cp < 0x80)
        {
            if (out + 1 > dstlen) break;
            dst[out++] = (char)cp;
        }
        else if (cp < 0x800)
        {
            if (out + 2 > dstlen) break;
            dst[out++] = (char)(0xc0 | (cp >> 6));
            dst[out++] = (char)(0x80 | (cp & 0x3f));
        }
        else if (cp < 0x10000)
        {
            if (out + 3 > dstlen) break;
            dst[out++] = (char)(0xe0 | (cp >> 12));
            dst[out++] = (char)(0x80 | ((cp >> 6) & 0x3f));
            dst[out++] = (char)(0x80 | (cp & 0x3f));
        }
        else
        {
            if (out + 4 > dstlen) break;
            dst[out++] = (char)(0xf0 | (cp >> 18));
            dst[out++] = (char)(0x80 | ((cp >> 12) & 0x3f));
            dst[out++] = (char)(0x80 | ((cp >> 6) & 0x3f));
            dst[out++] = (char)(0x80 | (cp & 0x3f));
        }
    }
    return (int)out;
}

/* --------------------------------------------------------- named refusals */

static void refuse_once( const char *what )
{
    static const char *last;

    if (last == what) return;
    last = what;
    fprintf( stderr, "helper: %s needs a Wine prefix, which this helper "
             "process does not have; refusing rather than answering wrongly\n",
             what );
}

NTSTATUS WINAPI NtCreateKey( HANDLE *key, ACCESS_MASK access, const OBJECT_ATTRIBUTES *attr,
                             ULONG index, const UNICODE_STRING *cls, ULONG options,
                             ULONG *dispos )
{
    refuse_once( "the registry" );
    if (key) *key = 0;
    return STATUS_NOT_IMPLEMENTED;
}

NTSTATUS WINAPI NtSetValueKey( HANDLE key, const UNICODE_STRING *name, ULONG index,
                               ULONG type, const void *data, ULONG count )
{
    refuse_once( "the registry" );
    return STATUS_NOT_IMPLEMENTED;
}

NTSTATUS WINAPI NtClose( HANDLE handle )
{
    return STATUS_SUCCESS;
}

NTSTATUS WINAPI NtQueryInformationToken( HANDLE token, TOKEN_INFORMATION_CLASS cls,
                                         void *info, ULONG length, ULONG *retlen )
{
    refuse_once( "the process token" );
    if (retlen) *retlen = 0;
    return STATUS_NOT_IMPLEMENTED;
}
