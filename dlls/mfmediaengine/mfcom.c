/*
 * Media Foundation for x86-64 guests -- the mfmediaengine flat wrapper.
 *
 * The runtime instance lives in dlls/mfplat/mfcom.c and there is exactly one
 * for the whole surface, because libs/winecom's state is per-linkee.  This
 * module therefore links NO winecom: it reaches the instance through mfplat's
 * exported __wine_com_* helpers, and dlls/mfmediaengine/mfmediaengine.spec
 * forwards __wine_com_dispatch to mfplat's so ntdll's trap dispatcher finds a
 * server for the guest mfmediaengine.dll's stub arrays.  Same shape as
 * dlls/mfreadwrite/mfcom.c, which is the file to read first.
 *
 * THERE IS ONLY ONE FLAT EXPORT THAT MATTERS, and that is the whole character
 * of this module: everything an application does with a media engine goes
 * through interfaces, and the only way in is
 * CoCreateInstance( CLSID_MFMediaEngineClassFactory ) -- i.e. this file's
 * DllGetClassObject, wrapped by IID like every other module's.
 *
 * WHAT IS NOT HERE, AND WHERE IT LIVES INSTEAD.  IMFMediaEngine is
 * callback-driven end to end: the application implements IMFMediaEngineNotify
 * and Media Foundation calls EventNotify from its own thread for every state
 * change, so an engine created without one is an engine that never tells
 * anybody the video is ready.  That notify object is a GUEST-implemented COM
 * object handed into native code, which needs a reverse proxy -- and reverse
 * proxies are not this file's business.  They are libs/winecom/reverse.c's,
 * they are turned on for this whole surface by dlls/mfplat/mfcom.c's
 * WINECOM_SF_REVERSE, and the notify object reaches native MF through the
 * attribute store that IMFMediaEngineClassFactory::CreateInstance takes --
 * IMFAttributes::SetUnknown, whose CA_IFACE_IN row carries the interface type
 * the reverse direction needs.  So it works by the same road
 * MF_SOURCE_READER_ASYNC_CALLBACK already travels, and there is nothing to
 * write here for it.
 *
 * MEASURED: nothing.  No corpus title has created a media engine on this port.
 * The surface is present, its refusals are named, and ppc64le/mf/README.md
 * says plainly that it is unexercised rather than letting silence imply
 * otherwise.
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

#include "mfapi.h"
#include "mfidl.h"
#include "mfobjects.h"
#include "mfmediaengine.h"
#include "mferror.h"

/* No debug channel here on purpose: neither wrapper below logs.  What would
 * be logged -- a guest-implemented object arriving where this surface cannot
 * translate one -- is logged by mfplat's own __wine_mf_translate_in, once,
 * with the export and the interface named.  A second channel declared and
 * never used is a warning, and this tree builds warning-free. */

/* The single runtime instance's helper API, exported by mfplat.dll.  Declared
 * here rather than in a shared header for the reason dlls/mfreadwrite/mfcom.c
 * gives: this is the whole interface between a sibling module and the
 * instance, and writing it out where it is used keeps the coupling visible. */
extern HRESULT WINAPI __wine_com_wrap_out_iface( HRESULT hr, const GUID *riid, void **ppv );

/* The in-process class objects.  Wrapped by IID like any other riid/void**
 * pair: a class object that is not on this surface's roster is released and
 * refused rather than handed over with a native vtable. */
HRESULT WINAPI __wine_guest_DllGetClassObject( REFCLSID clsid, REFIID riid, void **out )
{
    HRESULT hr = DllGetClassObject( clsid, riid, out );

    return __wine_com_wrap_out_iface( hr, riid, out );
}
