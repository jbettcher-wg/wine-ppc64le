/*
 * Media Foundation for x86-64 guests -- the mf.dll flat wrappers.
 *
 * The pipeline half of the surface: IMFMediaSession, IMFTopology,
 * IMFTopologyNode, the media sinks and the renderer activates.  The runtime
 * instance lives in dlls/mfplat/mfcom.c and there is exactly one for the whole
 * surface, so this module links no winecom and reaches the instance through
 * mfplat's exported __wine_com_* helpers, with dlls/mf/mf.spec forwarding
 * __wine_com_dispatch there for ntdll's trap dispatcher.
 *
 * HOW FAR THE PIPELINE ACTUALLY GOES, stated here rather than implied.  Every
 * object below is vended as a proxy and its methods dispatch, so a guest can
 * build a topology, hand it to a session and start it.  What it cannot do is
 * LEARN WHAT HAPPENED: IMFMediaEventGenerator's whole contract is
 * BeginGetEvent(IMFAsyncCallback *, ...) -- an object the GUEST implements,
 * which MF invokes -- and that is the reverse-proxy gap.  GetEvent(MF_EVENT_
 * FLAG_NO_WAIT) blocks instead of calling back and IS served, so a guest that
 * polls can drive a session; a guest that waits on callbacks cannot.  This is
 * why ppc64le/mf/check-mf-smoke.sh gates the SOURCE READER rather than the
 * session: the reader is the path that needs no callback at all.
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
#include "mftransform.h"
#include "mferror.h"

#include "wine/debug.h"

WINE_DEFAULT_DEBUG_CHANNEL(mfplat);

/* The single runtime instance's helper API, exported by mfplat.dll (see
 * dlls/mfplat/mfcom.c and dlls/mfplat/mfplat.spec). */
extern BOOL WINAPI __wine_com_translate_in( void *guest_seen, void **host_out );
extern HRESULT WINAPI __wine_com_wrap_out_iface( HRESULT hr, const GUID *riid, void **ppv );
extern void *WINAPI __wine_com_wrap( void *host, UINT iface );
extern UINT WINAPI __wine_com_iface_from_iid( const GUID *riid );
extern BOOL WINAPI __wine_mf_translate_in( LONG *logged, const char *export_name,
                                           const char *iface_name, void *obj,
                                           void **host_out );
extern HRESULT WINAPI __wine_mf_refuse_cross_surface( LONG *logged, const char *export_name,
                                                      const char *iface_name,
                                                      const char *owner );
extern HRESULT WINAPI __wine_mf_audit_propvariant_out( LONG *logged, const char *export_name,
                                                       HRESULT hr, PROPVARIANT *pv );

/* Vending with no interface going in: one line each, and the IID is what
 * types the wrap, so an interface this surface's roster does not carry is
 * released and refused rather than handed over with a native vtable. */
#define MF_VEND( name, iid, decl_args, call_args, outp )                      \
    HRESULT WINAPI __wine_guest_##name decl_args                              \
    {                                                                         \
        HRESULT hr = name call_args;                                          \
                                                                              \
        return __wine_com_wrap_out_iface( hr, &iid, (void **)(outp) );        \
    }

MF_VEND( MFCreateAudioRendererActivate, IID_IMFActivate,
         ( IMFActivate **activate ), ( activate ), activate )
MF_VEND( MFCreatePresentationClock, IID_IMFPresentationClock,
         ( IMFPresentationClock **clock ), ( clock ), clock )
MF_VEND( MFCreateSampleCopierMFT, IID_IMFTransform,
         ( IMFTransform **transform ), ( transform ), transform )
MF_VEND( MFCreateSimpleTypeHandler, IID_IMFMediaTypeHandler,
         ( IMFMediaTypeHandler **handler ), ( handler ), handler )
MF_VEND( MFCreateStandardQualityManager, IID_IMFQualityManager,
         ( IMFQualityManager **manager ), ( manager ), manager )
MF_VEND( MFCreateTopoLoader, IID_IMFTopoLoader,
         ( IMFTopoLoader **loader ), ( loader ), loader )
MF_VEND( MFCreateTopology, IID_IMFTopology,
         ( IMFTopology **topology ), ( topology ), topology )
MF_VEND( MFCreateTopologyNode, IID_IMFTopologyNode,
         ( MF_TOPOLOGY_TYPE node_type, IMFTopologyNode **node ),
         ( node_type, node ), node )
MF_VEND( MFCreateVideoRendererActivate, IID_IMFActivate,
         ( HWND hwnd, IMFActivate **activate ), ( hwnd, activate ), activate )

/* `IUnknown *reserved` is documented NULL, but "documented NULL" is not the
 * same as "cannot be a guest proxy", and the audit only saw this parameter
 * once IUnknown joined the roster.  Translated like any other. */
HRESULT WINAPI __wine_guest_MFCreateSequencerSource( IUnknown *reserved,
                                                     IMFSequencerSource **seq_source )
{
    static LONG logged;
    void *host;
    HRESULT hr;

    if (!__wine_mf_translate_in( &logged, "MFCreateSequencerSource", "IUnknown",
                                 reserved, &host ))
        return E_NOTIMPL;
    hr = MFCreateSequencerSource( host, seq_source );
    return __wine_com_wrap_out_iface( hr, &IID_IMFSequencerSource, (void **)seq_source );
}

/* Shutting an object down is the one call a guest makes on an object it is
 * about to drop, so it has to work for every proxy on the surface -- and it
 * was INVISIBLE to the flat audit until IUnknown was rostered, because the
 * audit builds its token set from the roster. */
HRESULT WINAPI __wine_guest_MFShutdownObject( IUnknown *object )
{
    static LONG logged;
    void *host;

    if (!__wine_mf_translate_in( &logged, "MFShutdownObject", "IUnknown",
                                 object, &host ))
        return E_NOTIMPL;
    return MFShutdownObject( host );
}

/* An IMFAttributes configuration going in and an object coming out. */
#define MF_VEND_CONFIG( name, iid, decl_args, call_args, outp )               \
    HRESULT WINAPI __wine_guest_##name decl_args                              \
    {                                                                         \
        static LONG logged;                                                   \
        void *host;                                                           \
        HRESULT hr;                                                           \
                                                                              \
        if (!__wine_mf_translate_in( &logged, #name, "IMFAttributes",         \
                                     config, &host ))                         \
            return E_NOTIMPL;                                                 \
        hr = name call_args;                                                  \
        return __wine_com_wrap_out_iface( hr, &iid, (void **)(outp) );        \
    }

MF_VEND_CONFIG( MFCreateMediaSession, IID_IMFMediaSession,
                ( IMFAttributes *config, IMFMediaSession **session ),
                ( host, session ), session )
MF_VEND_CONFIG( MFCreateAudioRenderer, IID_IMFMediaSink,
                ( IMFAttributes *config, IMFMediaSink **sink ), ( host, sink ), sink )
MF_VEND_CONFIG( MFCreateDeviceSource, IID_IMFMediaSource,
                ( IMFAttributes *config, IMFMediaSource **source ),
                ( host, source ), source )

/* The media sinks: a byte stream in, optionally one or two media types, a
 * sink out.  Each input is translated separately so the refusal names the
 * interface the guest actually implemented. */
#define MF_SINK_1( name, decl_args, call_args )                               \
    HRESULT WINAPI __wine_guest_##name decl_args                              \
    {                                                                         \
        static LONG logged;                                                   \
        void *hs;                                                             \
        HRESULT hr;                                                           \
                                                                              \
        if (!__wine_mf_translate_in( &logged, #name, "IMFByteStream",         \
                                     stream, &hs ))                           \
            return E_NOTIMPL;                                                 \
        hr = name call_args;                                                  \
        return __wine_com_wrap_out_iface( hr, &IID_IMFMediaSink, (void **)sink ); \
    }

MF_SINK_1( MFCreateMP3MediaSink,
           ( IMFByteStream *stream, IMFMediaSink **sink ), ( hs, sink ) )

#define MF_SINK_2( name, decl_args, call_args )                               \
    HRESULT WINAPI __wine_guest_##name decl_args                              \
    {                                                                         \
        static LONG logged;                                                   \
        void *hs, *ha;                                                        \
        HRESULT hr;                                                           \
                                                                              \
        if (!__wine_mf_translate_in( &logged, #name, "IMFByteStream",         \
                                     stream, &hs ) ||                         \
            !__wine_mf_translate_in( &logged, #name, "IMFMediaType",          \
                                     audio_type, &ha ))                       \
            return E_NOTIMPL;                                                 \
        hr = name call_args;                                                  \
        return __wine_com_wrap_out_iface( hr, &IID_IMFMediaSink, (void **)sink ); \
    }

MF_SINK_2( MFCreateAC3MediaSink,
           ( IMFByteStream *stream, IMFMediaType *audio_type, IMFMediaSink **sink ),
           ( hs, ha, sink ) )
MF_SINK_2( MFCreateADTSMediaSink,
           ( IMFByteStream *stream, IMFMediaType *audio_type, IMFMediaSink **sink ),
           ( hs, ha, sink ) )

#define MF_SINK_3( name )                                                     \
    HRESULT WINAPI __wine_guest_##name( IMFByteStream *stream,                \
                                        IMFMediaType *video_type,             \
                                        IMFMediaType *audio_type,             \
                                        IMFMediaSink **sink )                 \
    {                                                                         \
        static LONG logged;                                                   \
        void *hs, *hv, *ha;                                                   \
        HRESULT hr;                                                           \
                                                                              \
        if (!__wine_mf_translate_in( &logged, #name, "IMFByteStream",         \
                                     stream, &hs ) ||                         \
            !__wine_mf_translate_in( &logged, #name, "IMFMediaType",          \
                                     video_type, &hv ) ||                     \
            !__wine_mf_translate_in( &logged, #name, "IMFMediaType",          \
                                     audio_type, &ha ))                       \
            return E_NOTIMPL;                                                 \
        hr = name( hs, hv, ha, sink );                                        \
        return __wine_com_wrap_out_iface( hr, &IID_IMFMediaSink, (void **)sink ); \
    }

MF_SINK_3( MFCreate3GPMediaSink )
MF_SINK_3( MFCreateFMPEG4MediaSink )
MF_SINK_3( MFCreateMPEG4MediaSink )

/* IMFSampleGrabberSinkCallback is the sample-grabber's whole point, and a
 * game that uses it implements it: OnProcessSample is the frame arriving.
 * This is the reverse-proxy gap wearing its most useful hat, so the refusal
 * names it. */
HRESULT WINAPI __wine_guest_MFCreateSampleGrabberSinkActivate( IMFMediaType *media_type,
                                                               IMFSampleGrabberSinkCallback *callback,
                                                               IMFActivate **activate )
{
    static LONG logged;
    void *ht, *hc;
    HRESULT hr;

    if (activate) *activate = NULL;
    if (!__wine_mf_translate_in( &logged, "MFCreateSampleGrabberSinkActivate",
                                 "IMFMediaType", media_type, &ht ) ||
        !__wine_mf_translate_in( &logged, "MFCreateSampleGrabberSinkActivate",
                                 "IMFSampleGrabberSinkCallback", callback, &hc ))
        return E_NOTIMPL;
    hr = MFCreateSampleGrabberSinkActivate( ht, hc, activate );
    return __wine_com_wrap_out_iface( hr, &IID_IMFActivate, (void **)activate );
}

HRESULT WINAPI __wine_guest_MFGetTopoNodeCurrentType( IMFTopologyNode *node, DWORD stream,
                                                      BOOL output, IMFMediaType **type )
{
    static LONG logged;
    void *host;
    HRESULT hr;

    if (!__wine_mf_translate_in( &logged, "MFGetTopoNodeCurrentType",
                                 "IMFTopologyNode", node, &host ))
        return E_NOTIMPL;
    hr = MFGetTopoNodeCurrentType( host, stream, output, type );
    return __wine_com_wrap_out_iface( hr, &IID_IMFMediaType, (void **)type );
}

HRESULT WINAPI __wine_guest_MFRequireProtectedEnvironment( IMFPresentationDescriptor *pd )
{
    static LONG logged;
    void *host;

    if (!__wine_mf_translate_in( &logged, "MFRequireProtectedEnvironment",
                                 "IMFPresentationDescriptor", pd, &host ))
        return E_NOTIMPL;
    return MFRequireProtectedEnvironment( host );
}

HRESULT WINAPI __wine_guest_MFTranscodeGetAudioOutputAvailableTypes( REFGUID subtype,
                                                                     DWORD flags,
                                                                     IMFAttributes *config,
                                                                     IMFCollection **types )
{
    static LONG logged;
    void *host;
    HRESULT hr;

    if (!__wine_mf_translate_in( &logged, "MFTranscodeGetAudioOutputAvailableTypes",
                                 "IMFAttributes", config, &host ))
        return E_NOTIMPL;
    hr = MFTranscodeGetAudioOutputAvailableTypes( subtype, flags, host, types );
    return __wine_com_wrap_out_iface( hr, &IID_IMFCollection, (void **)types );
}

/* The one interface-typed OUT ARRAY on the flat surface: MF allocates the
 * array with CoTaskMemAlloc and the caller frees it, so the array itself
 * crosses as an address and only its ELEMENTS need wrapping -- in place,
 * because the guest will hand the same block back to CoTaskMemFree. */
HRESULT WINAPI __wine_guest_MFEnumDeviceSources( IMFAttributes *attributes,
                                                 IMFActivate ***sources, UINT32 *count )
{
    static LONG logged;
    void *host;
    HRESULT hr;
    UINT idx;
    UINT32 i;

    if (!__wine_mf_translate_in( &logged, "MFEnumDeviceSources", "IMFAttributes",
                                 attributes, &host ))
        return E_NOTIMPL;
    hr = MFEnumDeviceSources( host, sources, count );
    if (FAILED(hr) || !sources || !*sources || !count) return hr;
    if ((idx = __wine_com_iface_from_iid( &IID_IMFActivate )) == ~0u)
    {
        ERR( "mf: IMFActivate is not on the roster; refusing to hand the guest "
             "%u native vtables from MFEnumDeviceSources\n", *count );
        return E_NOINTERFACE;
    }
    for (i = 0; i < *count; i++)
        (*sources)[i] = __wine_com_wrap( (*sources)[i], idx );
    return hr;
}

/* riid/void** pairs. */
HRESULT WINAPI __wine_guest_MFGetService( IUnknown *object, REFGUID service, REFIID iid,
                                          void **obj )
{
    static LONG logged;
    void *host;
    HRESULT hr;

    if (!__wine_mf_translate_in( &logged, "MFGetService", "IUnknown (the service "
                                 "provider)", object, &host ))
        return E_NOTIMPL;
    hr = MFGetService( host, service, iid, obj );
    return __wine_com_wrap_out_iface( hr, iid, obj );
}

HRESULT WINAPI __wine_guest_DllGetClassObject( REFCLSID rclsid, REFIID riid, void **obj )
{
    HRESULT hr = DllGetClassObject( rclsid, riid, obj );

    return __wine_com_wrap_out_iface( hr, riid, obj );
}

/* mfplat's MFCreateSourceResolver is reached through mf.dll's own forward
 * (mf.spec), so there is no C definition here: both names must resolve to the
 * SAME wrapper or the resolver would arrive unwrapped through one of them. */

/* PROPVARIANT out-parameters: served, then audited for a tag that carries an
 * interface pointer (dlls/mfplat/mfcom.c holds the one copy of the rule). */
HRESULT WINAPI __wine_guest_MFGetSupportedMimeTypes( PROPVARIANT *array )
{
    static LONG logged;

    return __wine_mf_audit_propvariant_out( &logged, "MFGetSupportedMimeTypes",
                                            MFGetSupportedMimeTypes( array ), array );
}

HRESULT WINAPI __wine_guest_MFGetSupportedSchemes( PROPVARIANT *array )
{
    static LONG logged;

    return __wine_mf_audit_propvariant_out( &logged, "MFGetSupportedSchemes",
                                            MFGetSupportedSchemes( array ), array );
}

HRESULT WINAPI __wine_guest_MFCreateSequencerSegmentOffset( MFSequencerElementId id,
                                                            MFTIME offset,
                                                            PROPVARIANT *segment_offset )
{
    static LONG logged;

    return __wine_mf_audit_propvariant_out(
               &logged, "MFCreateSequencerSegmentOffset",
               MFCreateSequencerSegmentOffset( id, offset, segment_offset ),
               segment_offset );
}
