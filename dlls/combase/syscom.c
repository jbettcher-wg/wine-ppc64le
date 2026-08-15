/*
 * System COM for x86-64 guests on native ppc64le Wine -- the combase-side
 * runtime instance and the flat-export wrappers.
 *
 * This is the native half of the system-COM boundary
 * (hangover-ppc64le/docs/system-com-design.md).  Nothing here replaces any
 * Wine implementation: combase/ole32/oleaut32 and every object they vend
 * are Wine's own, and the flat FROM-SPEC thunks already reach them
 * correctly.  Only interface POINTERS crossing to the guest are wrong, and
 * this file is the wrapping layer that fixes them:
 *
 *   * it holds THE ONE winecom runtime instance for the whole system-COM
 *     surface (static-library state is per-linkee; combase is the module
 *     every other COM DLL imports, so the instance lives here and
 *     ole32/oleaut32 reach it through the exported __wine_com_* helpers and
 *     a spec forward of __wine_com_dispatch);
 *
 *   * the host invoker is a DIRECT widest-form native vtable call -- the
 *     implementations are ordinary native PE code in the same Win32 world,
 *     so there is no unixlib on this surface (§4.2).  That one function
 *     pointer is the entire difference from d3d12's invoker;
 *
 *   * the flat wrappers (__wine_guest_*) call the real native export through
 *     an ordinary internal call and wrap/translate interface pointers at
 *     the classified positions.  spec2thunk's GUEST-IMPL redirect points the
 *     guest export's native resolution at the wrapper (§4.3); the build-time
 *     flat-surface audit refuses to generate if any interface-bearing flat
 *     export is left unclassified.
 *
 * Copyright 2026 the ppc64le port authors
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 */

#include <stdarg.h>

#include "windef.h"
#include "winbase.h"
#include "objbase.h"
#include "winternl.h"
#include "wine/debug.h"
#include "wine/winecom.h"

#include "syscom_marshal.h"

WINE_DEFAULT_DEBUG_CHANNEL(combase);

/* ------------------------------------------------------- the host invoker */

/* A direct widest-form native vtable call: host's vtable slot with up to 16
 * ULONG_PTR arguments (args[0] is `this`).  ELFv2 callees ignore the excess,
 * so one shape serves every slot -- the same trick call_native_thunk uses.
 * No unixlib: these are ordinary native COM objects. */
static UINT64 syscom_invoke( void *host, UINT slot, UINT argc, UINT64 *args )
{
    void **vtbl = *(void ***)host;

    args[0] = (UINT64)(ULONG_PTR)host;
    return ((UINT64 (*)( ULONG_PTR, ULONG_PTR, ULONG_PTR, ULONG_PTR, ULONG_PTR,
                         ULONG_PTR, ULONG_PTR, ULONG_PTR, ULONG_PTR, ULONG_PTR,
                         ULONG_PTR, ULONG_PTR, ULONG_PTR, ULONG_PTR, ULONG_PTR,
                         ULONG_PTR ))vtbl[slot])
        ( args[0], args[1], args[2],  args[3],  args[4],  args[5],  args[6],
          args[7], args[8], args[9],  args[10], args[11], args[12], args[13],
          args[14], args[15] );
}

static const WCHAR *const syscom_guest_modules[] =
{
    L"combase.dll", L"ole32.dll", L"oleaut32.dll",
};

static const struct winecom_surface syscom_surface =
{
    .name = "syscom",
    .guest_modules = syscom_guest_modules,
    .module_count = ARRAYSIZE(syscom_guest_modules),
    .ifaces = syscom_com_ifaces,
    .iface_count = SYSCOM_IFACE_COUNT,
    .invoke = syscom_invoke,
    .hand_funcs = NULL,
    .hand_count = 0,
};

C_ASSERT( SYSCOM_HAND_COUNT == 0 );

static BOOL syscom_ready( void )
{
    return winecom_attach( &syscom_surface );
}

/* ---------------------------------------------------- exported dispatch */

NTSTATUS WINAPI __wine_com_dispatch( UINT iface, UINT slot, AMD64_CONTEXT *ctx )
{
    if (!syscom_ready()) return STATUS_DLL_INIT_FAILED;
    return winecom_dispatch( iface, slot, ctx );
}

/* The sibling-module helper API (§4.2): ole32 and oleaut32 wrappers reach
 * the single runtime instance through these forwards, never by re-linking
 * libwinecom (which would give them their own tables). */
void *WINAPI __wine_com_wrap( void *host, UINT iface )
{
    if (!syscom_ready()) return NULL;
    return winecom_wrap( host, iface );
}

void *WINAPI __wine_com_unwrap( void *proxy )
{
    return winecom_unwrap( proxy );
}

BOOL WINAPI __wine_com_translate_in( void *guest_seen, void **host_out )
{
    return winecom_translate_in( guest_seen, host_out );
}

HRESULT WINAPI __wine_com_wrap_out_iface( HRESULT hr, const GUID *riid, void **ppv )
{
    if (!syscom_ready()) return hr;
    return winecom_wrap_out_iface( hr, riid, ppv );
}

void WINAPI __wine_com_wrap_static( void **p, UINT iface )
{
    if (!syscom_ready()) return;
    winecom_wrap_static( p, iface );
}

UINT WINAPI __wine_com_iface_from_iid( const GUID *riid )
{
    return winecom_iface_from_iid( riid );
}

/* The shared loud-refusal stub every GUEST-REFUSE export resolves to: a flat
 * export that vends or consumes interfaces but has no wrapper yet.  Returns
 * E_NOTIMPL (0 = NULL for the pointer/void-returning ones), never a
 * pass-through that would hand the guest a native vtable.  The trapping
 * export's own name is in the dispatcher TRACE. */
HRESULT WINAPI __wine_com_refuse(void)
{
    ERR( "syscom: refusing an interface-bearing flat export with no wrapper "
         "yet (see the guest thunk trace for which)\n" );
    return E_NOTIMPL;
}

/* ------------------------------------------------------- flat wrappers */
/* Each calls the real native export (declared here to avoid dragging the
 * whole combase private surface in) and wraps/translates at the classified
 * positions.  The interception is spec2thunk's GUEST-IMPL redirect, so the
 * guest still imports the plain export name. */

HRESULT WINAPI CoCreateInstance( REFCLSID, IUnknown *, DWORD, REFIID, void ** );
HRESULT WINAPI CoGetClassObject( REFCLSID, DWORD, COSERVERINFO *, REFIID, void ** );
HRESULT WINAPI CreateStreamOnHGlobal( HGLOBAL, BOOL, IStream ** );
HRESULT WINAPI GetHGlobalFromStream( IStream *, HGLOBAL * );

HRESULT WINAPI __wine_guest_CoCreateInstance( REFCLSID rclsid, IUnknown *outer,
                                              DWORD ctx, REFIID riid, void **ppv )
{
    HRESULT hr;

    if (!syscom_ready()) return E_FAIL;
    if (outer)
    {
        /* Aggregation hands native code a guest IUnknown -- needs a reverse
         * proxy (design §6 / step 5), which does not exist yet. */
        FIXME( "syscom: CoCreateInstance with a non-NULL pUnkOuter %p is "
               "refused until reverse proxies land\n", outer );
        if (ppv) *ppv = NULL;
        return CLASS_E_NOAGGREGATION;
    }
    hr = CoCreateInstance( rclsid, NULL, ctx, riid, ppv );
    return __wine_com_wrap_out_iface( hr, riid, ppv );
}

HRESULT WINAPI __wine_guest_CoGetClassObject( REFCLSID rclsid, DWORD ctx,
                                              COSERVERINFO *info, REFIID riid,
                                              void **ppv )
{
    HRESULT hr;

    if (!syscom_ready()) return E_FAIL;
    hr = CoGetClassObject( rclsid, ctx, info, riid, ppv );
    return __wine_com_wrap_out_iface( hr, riid, ppv );
}

HRESULT WINAPI __wine_guest_CreateStreamOnHGlobal( HGLOBAL hglobal, BOOL delete_on_release,
                                                   IStream **out )
{
    HRESULT hr;

    if (!syscom_ready()) return E_FAIL;
    hr = CreateStreamOnHGlobal( hglobal, delete_on_release, out );
    if (SUCCEEDED(hr))
        __wine_com_wrap_static( (void **)out, SYSCOM_IFACE_IStream );
    return hr;
}

HRESULT WINAPI __wine_guest_GetHGlobalFromStream( IStream *stream, HGLOBAL *phglobal )
{
    void *host;

    if (!syscom_ready()) return E_FAIL;
    if (!__wine_com_translate_in( stream, &host ))
    {
        FIXME( "syscom: GetHGlobalFromStream on a guest-implemented stream %p "
               "is refused until reverse proxies land\n", stream );
        return E_NOTIMPL;
    }
    return GetHGlobalFromStream( host, phglobal );
}
