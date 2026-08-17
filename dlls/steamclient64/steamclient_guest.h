/*
 * steamclient_guest.h -- what Proton's PE-side lsteamclient needs in order to
 * be compiled as a real x86-64 Windows DLL on this port, force-included ahead
 * of every vendored translation unit.
 *
 * WHY THE PE SIDE IS BUILT FOR x86-64 RATHER THAN FOR ppc64.  Every other
 * guest-facing module in this tree is a native ppc64 implementation plus a
 * generated AMD64 trap-stub thunk, and interface pointers cross through
 * libs/winecom.  That works because a COM vtable is a closed shape: three
 * IUnknown slots, HRESULT returns, and an argument classification small
 * enough to describe per slot.  A Steamworks vtable is not: no IUnknown, no
 * IID, no HRESULT, ~6500 slots across the version matrix, and signatures that
 * include float and double returns, CSteamID passed and returned by value,
 * 136-byte SteamNetworkingIdentity arguments that MS-x64 passes by hidden
 * reference, and hidden-sret returns.  winecom_dispatch can marshal integer
 * arguments into an integer RAX and nothing else, so serving those vtables
 * that way would mean generating a per-slot MS-x64 marshal descriptor for
 * every one of them -- which is precisely the code a C compiler emits when it
 * compiles Proton's own PE-side wrappers, because those wrappers ARE the
 * marshaller: each one packs its arguments into a flat params struct.
 *
 * So this port compiles them for the machine the game is running on.  The
 * game-to-steamclient64.dll boundary then stays x86-64-to-x86-64, exactly as
 * on Windows and under Proton, and the only boundary left is the one Proton
 * already drew: the flat params-struct call, which this port stretches across
 * a process to the x86-64 Linux helper (steamrpc_wire.h).
 *
 * Three things have to be arranged for that compile, and all three are here
 * rather than as edits to the vendored sources:
 *
 *   1  wine/debug.h names __wine_dbg_* entry points that the guest ntdll
 *      thunk does not export (measured: it exports none of the four).  The
 *      channel macros are redefined here on top of OutputDebugStringA, which
 *      the guest kernel32 thunk does export, so a TRACE is still visible in a
 *      WINEDEBUG run and nothing silently disappears.
 *   2  wine/unixlib.h's WINE_UNIX_CALL is the one hook Proton routes every
 *      method through.  It is redirected at the RPC client.  Nothing else in
 *      the vendored PE side knows the boundary moved.
 *   3  the guest msvcrt thunk exports the CRT under its MSVCRT-convention
 *      names (_strdup, _snprintf) and not the bare ones Proton spells.
 *
 * Copyright 2026 the ppc64le port authors
 *
 * This library is free software; you can redistribute it and/or modify it
 * under the terms of the GNU Lesser General Public License as published by
 * the Free Software Foundation; either version 2.1 of the License, or (at
 * your option) any later version.
 */

#ifndef __STEAMCLIENT_GUEST_H
#define __STEAMCLIENT_GUEST_H

/* ------------------------------------------------------------ 1: debugging */

#define __WINE_WINE_DEBUG_H           /* stand in for wine/debug.h entirely */

#include <stdarg.h>
#include <windef.h>
#include <winbase.h>
#include <winternl.h>   /* NTSTATUS, which every unix call returns */

#ifdef __cplusplus
extern "C" {
#endif

/* Implemented in steamrpc.c: formats into a stack buffer and hands the line
 * to OutputDebugStringA, gated on the WINEDEBUG-style channel state this
 * module reads once at attach. */
extern int steamclient_dbg_enabled( unsigned int cls );
extern void steamclient_dbg_log( unsigned int cls, const char *func,
                                 const char *fmt, ... );
extern const char *steamclient_dbgstr_a( const char *s );
extern const char *steamclient_dbgstr_w( const WCHAR *s );

#define __WINE_DBCL_TRACE  0
#define __WINE_DBCL_WARN   1
#define __WINE_DBCL_FIXME  2
#define __WINE_DBCL_ERR    3

#define WINE_DEFAULT_DEBUG_CHANNEL(ch)  /* one module, one channel */
#define WINE_DECLARE_DEBUG_CHANNEL(ch)
#define DEFAULT_DEBUG_CHANNEL(ch)

#define TRACE(...) steamclient_dbg_log( __WINE_DBCL_TRACE, __func__, __VA_ARGS__ )
#define WARN(...)  steamclient_dbg_log( __WINE_DBCL_WARN,  __func__, __VA_ARGS__ )
#define FIXME(...) steamclient_dbg_log( __WINE_DBCL_FIXME, __func__, __VA_ARGS__ )
#define ERR(...)   steamclient_dbg_log( __WINE_DBCL_ERR,   __func__, __VA_ARGS__ )
#define TRACE_(ch) TRACE
#define WARN_(ch)  WARN
#define FIXME_(ch) FIXME
#define ERR_(ch)   ERR
#define TRACE_ON(ch)  steamclient_dbg_enabled( __WINE_DBCL_TRACE )
#define WARN_ON(ch)   steamclient_dbg_enabled( __WINE_DBCL_WARN )
#define FIXME_ON(ch)  steamclient_dbg_enabled( __WINE_DBCL_FIXME )
#define ERR_ON(ch)    steamclient_dbg_enabled( __WINE_DBCL_ERR )

#define debugstr_a(s) steamclient_dbgstr_a(s)
#define debugstr_an(s,n) steamclient_dbgstr_a(s)
#define debugstr_w(s) steamclient_dbgstr_w(s)
#define debugstr_wn(s,n) steamclient_dbgstr_w(s)
#define wine_dbgstr_a(s) steamclient_dbgstr_a(s)
#define wine_dbgstr_w(s) steamclient_dbgstr_w(s)
#define wine_dbg_printf(...) ((void)0)

#define WINE_TRACE   TRACE
#define WINE_WARN    WARN
#define WINE_FIXME   FIXME
#define WINE_ERR     ERR

/* ------------------------------------------------------ 2: the unix call */

#define __WINE_WINE_UNIXLIB_H         /* stand in for wine/unixlib.h */

typedef UINT64 unixlib_handle_t;
typedef UINT64 unixlib_module_t;

extern NTSTATUS steamrpc_call( unsigned int code, void *params );

/* Called from the vendored DllMain; see steamrpc.c. */
extern void steamrpc_publish_active_process( void );

#define WINE_UNIX_CALL( code, args )  steamrpc_call( (code), (args) )
#define __wine_init_unix_call()       ((void)0)

/* ------------------------------------------------------------ 3: the CRT */

/* Proton's PE side spells exactly one of these bare; the thunk exports it
 * under the MSVCRT convention.  Nothing else in the vendored PE sources uses
 * a renamed CRT entry point (measured, not assumed). */
#define strdup   _strdup

#ifdef __cplusplus
}
#endif

#endif /* __STEAMCLIENT_GUEST_H */
