/*
 * Media Foundation for x86-64 guests -- the evr flat wrappers.
 *
 * The runtime instance lives in dlls/mfplat/mfcom.c and there is exactly one
 * for the whole surface, because libs/winecom's state is per-linkee.  This
 * module links NO winecom: it reaches the instance through mfplat's exported
 * __wine_com_* helpers, and dlls/evr/evr.spec forwards __wine_com_dispatch to
 * mfplat's.  Read dlls/mfreadwrite/mfcom.c first; this is the same shape.
 *
 * WHAT THE ENHANCED VIDEO RENDERER IS FOR, on this port.  A game that plays a
 * cutscene through a media SESSION gets an EVR sink, and everything it then
 * wants to do with the picture -- put it in the right part of the window,
 * letterbox it, freeze a frame -- is IMFVideoDisplayControl, reached with
 * MFGetService on that sink.  None of those arguments are Direct3D: they are
 * rectangles, an enum and a caller's buffer.  That is the half this file
 * serves, and it is the half a game actually calls.
 *
 * THE OTHER HALF IS A CROSS-SURFACE REFUSAL AND STAYS ONE.  MFCreateVideoMixer,
 * MFCreateVideoPresenter and MFCreateVideoMixerAndPresenter take an
 * IDirect3DDeviceManager9 as their `owner` and vend objects whose whole
 * purpose is to hold Direct3D state; MFCreateVideoSampleFromSurface takes an
 * IDirect3DSurface9.  A Direct3D interface pointer belongs to the DXVK
 * surface's winecom instance, not to this one, and libs/winecom's state being
 * per-linkee is exactly why handing one across would be wrong rather than
 * merely unsupported -- the proxy would be minted by an instance that has
 * never seen it.  ppc64le/mf/README.md records the same boundary for
 * MFCreateDXGISurfaceBuffer and MFCreateMFByteStreamOnStream.
 *
 * So those wrappers refuse, BY NAME, and they refuse the argument rather than
 * the export: a caller passing NULL for the owner -- which is legal, and is
 * what the EVR's own default path does -- is served.  Refusing the whole
 * export would take away the case that works to protect the case that does
 * not.
 *
 * MEASURED: nothing.  No corpus title has created an EVR object on this port.
 * The surface is present and its refusals are named; ppc64le/mf/README.md says
 * so rather than letting silence imply otherwise.
 *
 * Copyright 2026 the ppc64le port authors
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 */

#include <stdarg.h>

#define COBJMACROS

#include "windef.h"
#include "winbase.h"
#include "objbase.h"
#include "ole2.h"

#include "d3d9.h"
#include "dxva2api.h"
#include "mfapi.h"
#include "mfidl.h"
#include "mfobjects.h"
#include "evr.h"
#include "mferror.h"

#include "wine/debug.h"

WINE_DEFAULT_DEBUG_CHANNEL(mfplat);

/* The single runtime instance's helper API, exported by mfplat.dll. */
extern BOOL WINAPI __wine_com_translate_in( void *guest_seen, void **host_out );
extern HRESULT WINAPI __wine_com_wrap_out_iface( HRESULT hr, const GUID *riid, void **ppv );

/* An `owner`/`surface` argument that belongs to the Direct3D surface rather
 * than to this one.  NULL is fine and is the common case; anything else is a
 * pointer this instance did not mint and must not pretend to understand.
 *
 * Logged once per export, like every other refusal on this surface: a refusal
 * repeated per frame is a log nobody reads, and a refusal reported once with
 * the export and the interface named is a bug report somebody can act on. */
static BOOL evr_refuse_d3d_owner( LONG *logged, const char *export_name,
                                  const char *iface_name, void *obj )
{
    if (!obj) return TRUE;
    if (!InterlockedExchange( logged, 1 ))
        FIXME( "evr: %s was given a %s (%p) that belongs to the Direct3D winecom "
               "surface, not to Media Foundation's; libs/winecom state is "
               "per-linkee, so this instance cannot translate it.  Refusing.\n",
               export_name, iface_name, obj );
    return FALSE;
}

HRESULT WINAPI __wine_guest_MFCreateVideoMixer( IUnknown *owner, REFIID riid_device,
                                                REFIID riid, void **obj )
{
    static LONG logged;
    HRESULT hr;

    if (obj) *obj = NULL;
    if (!evr_refuse_d3d_owner( &logged, "MFCreateVideoMixer",
                               "Direct3D device manager", owner ))
        return E_NOTIMPL;
    hr = MFCreateVideoMixer( NULL, riid_device, riid, obj );
    return __wine_com_wrap_out_iface( hr, riid, obj );
}

HRESULT WINAPI __wine_guest_MFCreateVideoPresenter( IUnknown *owner, REFIID riid_device,
                                                    REFIID riid, void **obj )
{
    static LONG logged;
    HRESULT hr;

    if (obj) *obj = NULL;
    if (!evr_refuse_d3d_owner( &logged, "MFCreateVideoPresenter",
                               "Direct3D device manager", owner ))
        return E_NOTIMPL;
    hr = MFCreateVideoPresenter( NULL, riid_device, riid, obj );
    return __wine_com_wrap_out_iface( hr, riid, obj );
}

HRESULT WINAPI __wine_guest_MFCreateVideoMixerAndPresenter( IUnknown *mixer_outer,
                                                            IUnknown *presenter_outer,
                                                            REFIID riid_mixer, void **mixer,
                                                            REFIID riid_presenter, void **presenter )
{
    static LONG logged;
    HRESULT hr;

    if (mixer) *mixer = NULL;
    if (presenter) *presenter = NULL;
    if (!evr_refuse_d3d_owner( &logged, "MFCreateVideoMixerAndPresenter",
                               "Direct3D aggregation outer", mixer_outer ) ||
        !evr_refuse_d3d_owner( &logged, "MFCreateVideoMixerAndPresenter",
                               "Direct3D aggregation outer", presenter_outer ))
        return E_NOTIMPL;
    hr = MFCreateVideoMixerAndPresenter( NULL, NULL, riid_mixer, mixer,
                                         riid_presenter, presenter );
    /* Two out-parameters, and BOTH have to be wrapped or the caller gets one
     * proxy and one native vtable -- which is the failure that looks like it
     * works until the second object's first method call. */
    hr = __wine_com_wrap_out_iface( hr, riid_mixer, mixer );
    return __wine_com_wrap_out_iface( hr, riid_presenter, presenter );
}

HRESULT WINAPI __wine_guest_MFCreateVideoSampleAllocator( REFIID riid, void **allocator )
{
    HRESULT hr = MFCreateVideoSampleAllocator( riid, allocator );

    return __wine_com_wrap_out_iface( hr, riid, allocator );
}

HRESULT WINAPI __wine_guest_MFCreateVideoSampleFromSurface( IUnknown *surface,
                                                            IMFSample **sample )
{
    static LONG logged;
    HRESULT hr;

    if (sample) *sample = NULL;
    if (!evr_refuse_d3d_owner( &logged, "MFCreateVideoSampleFromSurface",
                               "IDirect3DSurface9", surface ))
        return E_NOTIMPL;
    /* A NULL surface is the documented "give me a sample with no buffer"
     * form, and it is the one this surface can serve. */
    hr = MFCreateVideoSampleFromSurface( NULL, sample );
    return __wine_com_wrap_out_iface( hr, &IID_IMFSample, (void **)sample );
}

/* The in-process class objects.  Wrapped by IID like any other riid/void**
 * pair: a class object that is not on this surface's roster is released and
 * refused rather than handed over with a native vtable. */
HRESULT WINAPI __wine_guest_DllGetClassObject( REFCLSID clsid, REFIID riid, void **out )
{
    HRESULT hr = DllGetClassObject( clsid, riid, out );

    return __wine_com_wrap_out_iface( hr, riid, out );
}
