/*
 * Windows Media for x86-64 guests -- the wmvcore flat wrappers.
 *
 * The runtime instance lives in dlls/mfplat/mfcom.c and there is exactly one
 * for the whole surface, because libs/winecom's state is per-linkee.  This
 * module links NO winecom: it reaches the instance through mfplat's exported
 * __wine_com_* helpers, and dlls/wmvcore/wmvcore.spec forwards
 * __wine_com_dispatch to mfplat's.  Read dlls/mfreadwrite/mfcom.c first; this
 * is the same shape.
 *
 * WHY WINDOWS MEDIA SHARES MEDIA FOUNDATION'S ROSTER.  It is not Media
 * Foundation: IWMReader and IWMProfile are their own object graph from an SDK
 * that predates MF.  What they share is the RUNTIME.  libs/winecom's proxy
 * state is per-linkee, so a second roster would mean a second instance, and a
 * title that plays a .wmv through IWMSyncReader while decoding its cutscenes
 * through IMFSourceReader would then be one process with two proxy worlds that
 * refuse each other's objects.  Wine's wmvcore is built on winegstreamer,
 * which is the same pipeline mfplat's source reader uses, so that title is a
 * realistic one rather than a hypothetical.
 *
 * THE ONE THING TO KNOW ABOUT THIS SURFACE.  IWMReader is ASYNCHRONOUS and
 * callback-driven: IWMReader::Open takes an IWMReaderCallback the application
 * implements, and every sample arrives on a thread wmvcore created.  That is a
 * guest-implemented object handed into native code, which is exactly what
 * libs/winecom/reverse.c builds a native vtable for and what
 * dlls/mfplat/mfcom.c turns on for this whole surface with WINECOM_SF_REVERSE
 * -- and IWMReaderCallback is on the roster, so its slots have a plan.  The
 * callback does not arrive through a flat export, though: it is an argument to
 * a vtable METHOD, so nothing here has to do anything about it and the
 * generated marshal tables carry it.
 *
 * IWMSyncReader is the synchronous sibling and needs no callback at all --
 * open a file, call GetNextSample in a loop.  It is the path that works with
 * no reverse proxy involved, and it is the analogue of the synchronous
 * IMFSourceReader that ppc64le/mf/check-mf-smoke.sh measures end to end.
 *
 * MEASURED: nothing.  No corpus title has opened a Windows Media file on this
 * port.  The surface is present, its refusals are named, and
 * ppc64le/mf/README.md says it is unexercised rather than letting silence
 * imply otherwise.
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

#include "wmsdkidl.h"

/* No debug channel here on purpose: neither wrapper below logs.  What would
 * be logged -- a guest-implemented object arriving where this surface cannot
 * translate one -- is logged by mfplat's own __wine_mf_translate_in, once,
 * with the export and the interface named.  A second channel declared and
 * never used is a warning, and this tree builds warning-free. */

/* The single runtime instance's helper API, exported by mfplat.dll. */
extern HRESULT WINAPI __wine_com_wrap_out_iface( HRESULT hr, const GUID *riid, void **ppv );
extern BOOL WINAPI __wine_mf_translate_in( LONG *logged, const char *export_name,
                                           const char *iface_name, void *obj,
                                           void **host_out );

/* Every creation call here takes an optional `reserved`/`cert` IUnknown that
 * the SDK documents as NULL for everything except DRM.  It is still an
 * interface pointer, so it goes through the shared translate-in rather than
 * being passed through: a guest that hands over an object it implemented
 * would otherwise reach native wmvcore as an x86-64 vtable. */

HRESULT WINAPI __wine_guest_WMCreateReader( IUnknown *reserved, DWORD rights,
                                            IWMReader **reader )
{
    static LONG logged;
    void *host_reserved;
    HRESULT hr;

    if (reader) *reader = NULL;
    if (!__wine_mf_translate_in( &logged, "WMCreateReader", "IUnknown",
                                 reserved, &host_reserved ))
        return E_NOTIMPL;
    hr = WMCreateReader( host_reserved, rights, reader );
    return __wine_com_wrap_out_iface( hr, &IID_IWMReader, (void **)reader );
}

HRESULT WINAPI __wine_guest_WMCreateSyncReader( IUnknown *cert, DWORD rights,
                                                IWMSyncReader **reader )
{
    static LONG logged;
    void *host_cert;
    HRESULT hr;

    if (reader) *reader = NULL;
    if (!__wine_mf_translate_in( &logged, "WMCreateSyncReader", "IUnknown",
                                 cert, &host_cert ))
        return E_NOTIMPL;
    hr = WMCreateSyncReader( host_cert, rights, reader );
    return __wine_com_wrap_out_iface( hr, &IID_IWMSyncReader, (void **)reader );
}

HRESULT WINAPI __wine_guest_WMCreateWriter( IUnknown *reserved, IWMWriter **writer )
{
    static LONG logged;
    void *host_reserved;
    HRESULT hr;

    if (writer) *writer = NULL;
    if (!__wine_mf_translate_in( &logged, "WMCreateWriter", "IUnknown",
                                 reserved, &host_reserved ))
        return E_NOTIMPL;
    hr = WMCreateWriter( host_reserved, writer );
    return __wine_com_wrap_out_iface( hr, &IID_IWMWriter, (void **)writer );
}

HRESULT WINAPI __wine_guest_WMCreateProfileManager( IWMProfileManager **manager )
{
    HRESULT hr = WMCreateProfileManager( manager );

    return __wine_com_wrap_out_iface( hr, &IID_IWMProfileManager, (void **)manager );
}

HRESULT WINAPI __wine_guest_WMCreateEditor( IWMMetadataEditor **editor )
{
    HRESULT hr = WMCreateEditor( editor );

    return __wine_com_wrap_out_iface( hr, &IID_IWMMetadataEditor, (void **)editor );
}

HRESULT WINAPI __wine_guest_WMCreateBackupRestorer( IUnknown *callback,
                                                    IWMLicenseBackup **backup )
{
    static LONG logged;
    void *host_callback;
    HRESULT hr;

    if (backup) *backup = NULL;
    /* This one's IUnknown is NOT documentation's idea of reserved: it is the
     * application's IWMStatusCallback, and a guest that implements it is
     * handing a guest vtable to native code.  __wine_mf_translate_in carries
     * no roster index, so a guest-implemented object here is refused by name
     * rather than turned into a reverse proxy -- the same limit
     * MFCreateSourceReaderFromMediaSource has, recorded in
     * ppc64le/mf/README.md, and it wants the same typed translate-in to fix. */
    if (!__wine_mf_translate_in( &logged, "WMCreateBackupRestorer",
                                 "IWMStatusCallback", callback, &host_callback ))
        return E_NOTIMPL;
    hr = WMCreateBackupRestorer( host_callback, backup );
    return __wine_com_wrap_out_iface( hr, &IID_IWMLicenseBackup, (void **)backup );
}
