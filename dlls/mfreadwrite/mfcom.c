/*
 * Media Foundation for x86-64 guests -- the mfreadwrite flat wrappers.
 *
 * The runtime instance lives in dlls/mfplat/mfcom.c and there is exactly one
 * for the whole surface, because libs/winecom's state is per-linkee.  This
 * module therefore links NO winecom: it reaches the instance through mfplat's
 * exported __wine_com_* helpers, and dlls/mfreadwrite/mfreadwrite.spec
 * forwards __wine_com_dispatch to mfplat's so ntdll's trap dispatcher finds a
 * server for the guest mfreadwrite.dll's stub arrays.  That is the same shape
 * ole32/oleaut32 have against combase, and dxgi/d3d10core against d3d11.
 *
 * WHY THIS FILE MATTERS MOST.  IMFSourceReader used SYNCHRONOUSLY -- created
 * without MF_SOURCE_READER_ASYNC_CALLBACK, with ReadSample blocking until a
 * sample is decoded -- is the one Media Foundation path that needs no call
 * back into guest code at all, and it is how a large share of shipped games
 * decode a cutscene.  It is what ppc64le/mf/check-mf-smoke.sh measures, end to
 * end, against an FNV-1a hash of the source file's own PCM.
 *
 * The ASYNC construction of the same object USED to be refused, by name, in
 * each of the three creation wrappers below: an IMFSourceReaderCallback in the
 * attribute store is an object the GUEST implemented, and native MF invoking
 * it needs a REVERSE PROXY -- a native vtable whose slots enter guest code
 * through the emulator.  libs/winecom/reverse.c builds one now, so the async
 * path is served: the callback reaches the attribute store as a reverse proxy
 * through IMFAttributes::SetUnknown, and OnReadSample arrives at the guest's
 * own implementation from MF's work-queue thread with its IMFSample already
 * turned into a forward proxy.  The refusal became a trace; see
 * mf_attributes_note_callback below.
 *
 * What is still refused here is one interface further out and is a different
 * thing: an IMFMediaSource the GAME implements -- a packed-archive reader --
 * arrives at MFCreateSourceReaderFromMediaSource untyped, through the shared
 * __wine_mf_translate_in, which carries no roster index and therefore has no
 * slot table to build a reverse proxy from.  That is a wrapper's worth of work
 * and not a design gap; ppc64le/mf/README.md carries it.
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
#include "mfreadwrite.h"
#include "mferror.h"

#include "wine/debug.h"

WINE_DEFAULT_DEBUG_CHANNEL(mfplat);

/* The single runtime instance's helper API, exported by mfplat.dll (see
 * dlls/mfplat/mfcom.c and dlls/mfplat/mfplat.spec).  Declared here rather
 * than in a shared header for the reason ole32 needs none: this is the whole
 * interface between a sibling module and the instance, and writing it out
 * where it is used keeps the coupling visible. */
extern BOOL WINAPI __wine_com_translate_in( void *guest_seen, void **host_out );
extern HRESULT WINAPI __wine_com_wrap_out_iface( HRESULT hr, const GUID *riid, void **ppv );
extern BOOL WINAPI __wine_mf_translate_in( LONG *logged, const char *export_name,
                                           const char *iface_name, void *obj,
                                           void **host_out );

/* An attribute store the guest built and handed to a creation call may carry
 * MF_SOURCE_READER_ASYNC_CALLBACK / MF_SINK_WRITER_ASYNC_CALLBACK -- an
 * IMFSourceReaderCallback or IMFSinkWriterCallback the GUEST implements, which
 * Media Foundation then invokes from a work-queue thread.
 *
 * THIS USED TO BE THE REFUSAL, and it was the right one while the port had no
 * reverse proxies: a guest-implemented callback in that store meant native MF
 * would eventually call an x86-64 vtable.  It is now a TRACE, because the
 * object in the store is a reverse proxy -- IMFAttributes::SetUnknown put it
 * there through libs/winecom's dispatch loop, whose CA_IFACE_IN row for that
 * slot carries the interface type the reverse direction needs -- and calling
 * it is exactly what is supposed to happen.  Both callback interfaces are
 * fully marshalled: IMFSourceReaderCallback's OnReadSample is six arguments of
 * which one is an IMFSample, and IMFSinkWriterCallback's two are plain data.
 *
 * Kept rather than deleted because the trace is the thing a developer greps
 * for when an async reader does not call them back, and because the read goes
 * through the HOST store, so it sees exactly what MF will see. */
static void mf_attributes_note_callback( const char *export_name,
                                         IMFAttributes *host, const GUID *key,
                                         const char *iface_name )
{
    IUnknown *unk = NULL;

    if (!host) return;
    if (FAILED(IMFAttributes_GetUnknown( host, key, &IID_IUnknown, (void **)&unk )) || !unk)
        return;
    IUnknown_Release( unk );
    TRACE( "mf: %s was given an attribute store carrying a %s (%p); Media "
           "Foundation will call it back from a work-queue thread, through a "
           "reverse proxy\n", export_name, iface_name, unk );
}

/* Both halves of a creation call's input: the attribute store (optional) and
 * the source object (optional).  Returns FALSE having already logged. */
static BOOL mf_reader_attributes( LONG *logged, const char *export_name,
                                  IMFAttributes *attributes, void **host_attrs,
                                  const GUID *key, const char *iface_name )
{
    if (!__wine_mf_translate_in( logged, export_name, "IMFAttributes",
                                 attributes, host_attrs ))
        return FALSE;
    mf_attributes_note_callback( export_name, *host_attrs, key, iface_name );
    return TRUE;
}

HRESULT WINAPI __wine_guest_MFCreateSourceReaderFromURL( const WCHAR *url,
                                                         IMFAttributes *attributes,
                                                         IMFSourceReader **reader )
{
    static LONG logged;
    void *host_attrs;
    HRESULT hr;

    if (reader) *reader = NULL;
    if (!mf_reader_attributes( &logged, "MFCreateSourceReaderFromURL", attributes,
                               &host_attrs, &MF_SOURCE_READER_ASYNC_CALLBACK,
                               "IMFSourceReaderCallback" ))
        return E_NOTIMPL;
    hr = MFCreateSourceReaderFromURL( url, host_attrs, reader );
    return __wine_com_wrap_out_iface( hr, &IID_IMFSourceReader, (void **)reader );
}

HRESULT WINAPI __wine_guest_MFCreateSourceReaderFromByteStream( IMFByteStream *stream,
                                                                IMFAttributes *attributes,
                                                                IMFSourceReader **reader )
{
    static LONG logged;
    void *host_attrs, *host_stream;
    HRESULT hr;

    if (reader) *reader = NULL;
    if (!__wine_mf_translate_in( &logged, "MFCreateSourceReaderFromByteStream",
                                 "IMFByteStream", stream, &host_stream ))
        return E_NOTIMPL;
    if (!mf_reader_attributes( &logged, "MFCreateSourceReaderFromByteStream", attributes,
                               &host_attrs, &MF_SOURCE_READER_ASYNC_CALLBACK,
                               "IMFSourceReaderCallback" ))
        return E_NOTIMPL;
    hr = MFCreateSourceReaderFromByteStream( host_stream, host_attrs, reader );
    return __wine_com_wrap_out_iface( hr, &IID_IMFSourceReader, (void **)reader );
}

HRESULT WINAPI __wine_guest_MFCreateSourceReaderFromMediaSource( IMFMediaSource *source,
                                                                 IMFAttributes *attributes,
                                                                 IMFSourceReader **reader )
{
    static LONG logged;
    void *host_attrs, *host_source;
    HRESULT hr;

    if (reader) *reader = NULL;
    /* A game that implements its OWN IMFMediaSource -- a packed-archive
     * reader, say -- lands here, and this is the refusal that names it. */
    if (!__wine_mf_translate_in( &logged, "MFCreateSourceReaderFromMediaSource",
                                 "IMFMediaSource", source, &host_source ))
        return E_NOTIMPL;
    if (!mf_reader_attributes( &logged, "MFCreateSourceReaderFromMediaSource", attributes,
                               &host_attrs, &MF_SOURCE_READER_ASYNC_CALLBACK,
                               "IMFSourceReaderCallback" ))
        return E_NOTIMPL;
    hr = MFCreateSourceReaderFromMediaSource( host_source, host_attrs, reader );
    return __wine_com_wrap_out_iface( hr, &IID_IMFSourceReader, (void **)reader );
}

HRESULT WINAPI __wine_guest_MFCreateSinkWriterFromURL( const WCHAR *url,
                                                       IMFByteStream *bytestream,
                                                       IMFAttributes *attributes,
                                                       IMFSinkWriter **writer )
{
    static LONG logged;
    void *host_attrs, *host_stream;
    HRESULT hr;

    if (writer) *writer = NULL;
    if (!__wine_mf_translate_in( &logged, "MFCreateSinkWriterFromURL", "IMFByteStream",
                                 bytestream, &host_stream ))
        return E_NOTIMPL;
    if (!mf_reader_attributes( &logged, "MFCreateSinkWriterFromURL", attributes,
                               &host_attrs, &MF_SINK_WRITER_ASYNC_CALLBACK,
                               "IMFSinkWriterCallback" ))
        return E_NOTIMPL;
    hr = MFCreateSinkWriterFromURL( url, host_stream, host_attrs, writer );
    return __wine_com_wrap_out_iface( hr, &IID_IMFSinkWriter, (void **)writer );
}

HRESULT WINAPI __wine_guest_MFCreateSinkWriterFromMediaSink( IMFMediaSink *sink,
                                                             IMFAttributes *attributes,
                                                             IMFSinkWriter **writer )
{
    static LONG logged;
    void *host_attrs, *host_sink;
    HRESULT hr;

    if (writer) *writer = NULL;
    if (!__wine_mf_translate_in( &logged, "MFCreateSinkWriterFromMediaSink",
                                 "IMFMediaSink", sink, &host_sink ))
        return E_NOTIMPL;
    if (!mf_reader_attributes( &logged, "MFCreateSinkWriterFromMediaSink", attributes,
                               &host_attrs, &MF_SINK_WRITER_ASYNC_CALLBACK,
                               "IMFSinkWriterCallback" ))
        return E_NOTIMPL;
    hr = MFCreateSinkWriterFromMediaSink( host_sink, host_attrs, writer );
    return __wine_com_wrap_out_iface( hr, &IID_IMFSinkWriter, (void **)writer );
}

/* The in-process class objects.  Wrapped by IID like any other riid/void**
 * pair: a class object that is not on this surface's roster is released and
 * refused rather than handed over with a native vtable. */
HRESULT WINAPI __wine_guest_DllGetClassObject( REFCLSID clsid, REFIID riid, void **out )
{
    HRESULT hr = DllGetClassObject( clsid, riid, out );

    return __wine_com_wrap_out_iface( hr, riid, out );
}
