/*
 * mf_smoke -- the Media Foundation runtime gate.
 *
 * ONE source, built TWICE and run twice: as a native ppc64 Windows PE and as
 * an x86-64 guest PE.  The two runs must print byte-identical output, and the
 * bytes they print are not "it started" -- they are the sample rate, channel
 * count, sample count, presentation timestamps and an FNV-1a hash of every
 * decoded PCM byte that Wine's own mfreadwrite/winegstreamer pipeline
 * produced.  The gate computes the same hash over the source WAV's data chunk
 * with python3, so the number is anchored to the FILE and not merely to
 * whatever the two runs happen to agree on.
 *
 * WHAT IT EXERCISES, and why this shape.  Media Foundation's own async model
 * is IMFAsyncCallback -- an interface the APPLICATION implements and MF calls
 * back.  A guest-implemented COM object handed back into native MF is the
 * reverse-proxy gap this port does not have yet (libs/winecom.c
 * winecom_translate_in, design §6), so it is refused loudly by name and
 * probed separately by mf_async_probe.c.  What IS served is the SYNCHRONOUS
 * IMFSourceReader path -- MF_SOURCE_READER_ASYNC_CALLBACK unset, ReadSample
 * blocking -- which is how a large share of shipped games decode a cutscene:
 * pull a sample, upload it, present it, pull the next.  This program is that
 * loop, with every value checked.
 *
 * The guest run goes through:
 *
 *   guest mfplat.dll thunk :: MFStartup            (a flat FROM-SPEC thunk)
 *   guest mfreadwrite.dll thunk :: MFCreateSourceReaderFromURL
 *     -> native mfreadwrite __wine_guest_MFCreateSourceReaderFromURL
 *        -> Wine's real implementation, then __wine_com_wrap_static
 *           -> a com_proxy whose vtable is the guest module's stub array
 *   guest calls reader->lpVtbl->ReadSample -> trap
 *     -> ntdll find_guest_com_target -> mfplat's __wine_com_dispatch
 *        -> winecom_dispatch -> the native IMFSourceReader vtable slot,
 *           with the out IMFSample* wrapped on the way back
 *
 * NO C RUNTIME on the guest side (-DMF_SMOKE_NO_CRT), for the reason
 * ppc64le/syscom/com_smoke.c gives: a CRT would add a second variable to a
 * test whose whole value is that only one thing is under test.
 *
 * Copyright 2026 the ppc64le port authors
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 */

#define COBJMACROS
#define INITGUID

/* MF_PD_DURATION and the other attribute keys are spelled EXTERN_GUID, which
 * declares without defining even under INITGUID.  Wine's own
 * libs/mfuuid/mfuuid.c does exactly this redefinition for exactly this reason;
 * here it is what lets the guest build link with no import library at all. */
#undef EXTERN_GUID
#define EXTERN_GUID DEFINE_GUID

#include <windows.h>
#include <objbase.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mfobjects.h>
#include <mfreadwrite.h>
#include <mferror.h>

/* ------------------------------------------------------------- output */

static void out( const char *s )
{
    DWORD n = 0, written;

    while (s[n]) n++;
    WriteFile( GetStdHandle( STD_OUTPUT_HANDLE ), s, n, &written, NULL );
}

static void out_hex( ULONG v, int digits )
{
    static const char hex[] = "0123456789ABCDEF";
    char buf[9];
    int i;

    for (i = 0; i < digits; i++) buf[digits - 1 - i] = hex[(v >> (4 * i)) & 0xf];
    buf[digits] = 0;
    out( buf );
}

static void out_dec( ULONG v )
{
    char buf[12];
    int i = 11;

    buf[i] = 0;
    do { buf[--i] = '0' + (char)(v % 10); v /= 10; } while (v);
    out( buf + i );
}

static void out_dec64( ULONGLONG v )
{
    char buf[24];
    int i = 23;

    buf[i] = 0;
    do { buf[--i] = '0' + (char)(v % 10); v /= 10; } while (v);
    out( buf + i );
}

static void out_hr( const char *label, HRESULT hr )
{
    out( label );
    out( "=0x" );
    out_hex( (ULONG)hr, 8 );
}

static void out_guid( const GUID *g )
{
    int i;

    out( "{" );
    out_hex( g->Data1, 8 );
    out( "-" );
    out_hex( g->Data2, 4 );
    out( "-" );
    out_hex( g->Data3, 4 );
    out( "-" );
    for (i = 0; i < 8; i++)
    {
        if (i == 2) out( "-" );
        out_hex( g->Data4[i], 2 );
    }
    out( "}" );
}

/* PropVariantInit is a memset macro and this build has no CRT. */
static void pv_zero( PROPVARIANT *p )
{
    BYTE *b = (BYTE *)p;
    UINT i;
    for (i = 0; i < sizeof(*p); i++) b[i] = 0;
}

static BOOL guid_eq( const GUID *a, const GUID *b )
{
    const BYTE *p = (const BYTE *)a, *q = (const BYTE *)b;
    int i;

    for (i = 0; i < (int)sizeof(GUID); i++) if (p[i] != q[i]) return FALSE;
    return TRUE;
}

/* ------------------------------------------------------------- the run */

static int failures;
static int step;

static void begin( const char *what )
{
    out( "step " );
    out_dec( ++step );
    out( " " );
    out( what );
    out( ": " );
}

static void verdict( BOOL ok, const char *why )
{
    if (ok) out( " ok\n" );
    else
    {
        failures++;
        out( " FAIL (" );
        out( why );
        out( ")\n" );
    }
}

/* FNV-1a over every decoded byte.  32-bit and unsigned, so the arithmetic is
 * defined identically on both builds; the gate reproduces it in python over
 * the WAV's data chunk, which is what makes this an ORACLE rather than a
 * consistency check between two runs of the same possibly-wrong thing. */
#define FNV_OFFSET 2166136261u
#define FNV_PRIME  16777619u

static ULONG fnv_update( ULONG h, const BYTE *p, ULONG len )
{
    ULONG i;

    for (i = 0; i < len; i++)
    {
        h ^= p[i];
        h *= FNV_PRIME;
    }
    return h;
}

/* The expected numbers are properties of the file the gate writes, passed in
 * rather than hard-coded so the gate owns the media and the probe owns the
 * mechanism.  Read as decimal out of the environment; no CRT, so parsed here. */
static ULONG env_dec( const WCHAR *name, ULONG fallback )
{
    WCHAR buf[32];
    ULONG v = 0;
    DWORD n, i;

    n = GetEnvironmentVariableW( name, buf, 32 );
    if (!n || n >= 32) return fallback;
    for (i = 0; i < n; i++)
    {
        if (buf[i] < '0' || buf[i] > '9') return fallback;
        v = v * 10 + (ULONG)(buf[i] - '0');
    }
    return v;
}

/* The loop a game's cutscene decoder actually is, factored out because the
 * gate runs it TWICE -- once from the start of the file and once after a seek
 * back to zero -- and the second pass producing the same hash is what proves
 * IMFSourceReader::SetCurrentPosition really rewound rather than merely
 * returning S_OK. */
struct decoded
{
    ULONG samples, total, fnv;
    LONGLONG first_ts, last_ts;
    HRESULT hr;
};

static void read_all( IMFSourceReader *reader, struct decoded *d )
{
    d->samples = d->total = 0;
    d->fnv = FNV_OFFSET;
    d->first_ts = -1;
    d->last_ts = -1;
    d->hr = S_OK;

    for (;;)
    {
        IMFMediaBuffer *buffer = NULL;
        IMFSample *sample = NULL;
        DWORD flags = 0, index = 0;
        LONGLONG ts = 0;
        DWORD len = 0;
        BYTE *data;

        d->hr = IMFSourceReader_ReadSample( reader,
                    MF_SOURCE_READER_FIRST_AUDIO_STREAM, 0, &index, &flags,
                    &ts, &sample );
        if (FAILED(d->hr)) break;
        if (flags & MF_SOURCE_READERF_ENDOFSTREAM)
        {
            if (sample) IMFSample_Release( sample );
            d->hr = S_OK;
            break;
        }
        if (!sample) continue;         /* a stream tick carries no data */

        if (d->first_ts < 0) d->first_ts = ts;
        d->last_ts = ts;
        d->samples++;

        d->hr = IMFSample_ConvertToContiguousBuffer( sample, &buffer );
        if (SUCCEEDED(d->hr) && buffer)
        {
            d->hr = IMFMediaBuffer_Lock( buffer, &data, NULL, &len );
            if (SUCCEEDED(d->hr))
            {
                d->fnv = fnv_update( d->fnv, data, len );
                d->total += len;
                IMFMediaBuffer_Unlock( buffer );
            }
            IMFMediaBuffer_Release( buffer );
        }
        IMFSample_Release( sample );
        if (FAILED(d->hr)) break;
        if (d->samples > 100000) break;   /* a runaway loop is a result too */
    }
}

static void out_decoded( const struct decoded *d )
{
    out_hr( "hr", d->hr );
    out( " samples=" );
    out_dec( d->samples );
    out( " bytes=" );
    out_dec( d->total );
    out( " fnv=0x" );
    out_hex( d->fnv, 8 );
}

static int mf_smoke_run( void )
{
    ULONG want_rate, want_channels, want_bits, want_bytes, want_fnv;
    IMFSourceReader *reader = NULL;
    IMFMediaType *native = NULL;
    IMFMediaType *pcm = NULL;
    struct decoded pass1, pass2;
    PROPVARIANT pv;
    ULONG rate = 0, channels = 0, bits = 0;
    WCHAR url[MAX_PATH];
    GUID major, subtype;
    GUID time_format;
    HRESULT hr;
    DWORD n;

    out( "mf_smoke: start\n" );

    n = GetEnvironmentVariableW( L"MF_SMOKE_URL", url, MAX_PATH );
    if (!n || n >= MAX_PATH)
    {
        out( "mf_smoke: FAIL (MF_SMOKE_URL is not set)\n" );
        return 1;
    }
    want_rate     = env_dec( L"MF_SMOKE_RATE", 0 );
    want_channels = env_dec( L"MF_SMOKE_CHANNELS", 0 );
    want_bits     = env_dec( L"MF_SMOKE_BITS", 0 );
    want_bytes    = env_dec( L"MF_SMOKE_BYTES", 0 );
    want_fnv      = env_dec( L"MF_SMOKE_FNV", 0 );

    begin( "MFStartup(MF_VERSION, MFSTARTUP_FULL)" );
    hr = MFStartup( MF_VERSION, MFSTARTUP_FULL );
    out_hr( "hr", hr );
    verdict( hr == S_OK, "not S_OK" );
    if (hr != S_OK) goto done;

    /* The vending path: a flat export that hands back an interface pointer.
     * The guest reaches __wine_guest_MFCreateSourceReaderFromURL, which wraps
     * the IMFSourceReader before the guest ever sees it. */
    begin( "MFCreateSourceReaderFromURL(sine1s.wav, NULL)" );
    hr = MFCreateSourceReaderFromURL( url, NULL, &reader );
    out_hr( "hr", hr );
    verdict( hr == S_OK && reader != NULL, "no source reader" );
    if (hr != S_OK || !reader) goto shutdown;

    begin( "IMFSourceReader::SetStreamSelection(ALL_STREAMS, FALSE)" );
    hr = IMFSourceReader_SetStreamSelection( reader, MF_SOURCE_READER_ALL_STREAMS,
                                             FALSE );
    out_hr( "hr", hr );
    verdict( hr == S_OK, "not S_OK" );

    begin( "IMFSourceReader::SetStreamSelection(FIRST_AUDIO_STREAM, TRUE)" );
    hr = IMFSourceReader_SetStreamSelection( reader,
                                             MF_SOURCE_READER_FIRST_AUDIO_STREAM,
                                             TRUE );
    out_hr( "hr", hr );
    verdict( hr == S_OK, "not S_OK" );

    /* A statically typed out-interface inside the dispatch loop (as opposed to
     * inside a flat wrapper): CA_IFACE_OUT_STATIC. */
    begin( "IMFSourceReader::GetNativeMediaType(FIRST_AUDIO_STREAM, 0)" );
    hr = IMFSourceReader_GetNativeMediaType( reader,
                                             MF_SOURCE_READER_FIRST_AUDIO_STREAM,
                                             0, &native );
    out_hr( "hr", hr );
    verdict( hr == S_OK && native != NULL, "no native media type" );
    if (hr != S_OK || !native) goto release;

    begin( "IMFAttributes::GetGUID(MF_MT_MAJOR_TYPE)" );
    hr = IMFMediaType_GetGUID( native, &MF_MT_MAJOR_TYPE, &major );
    out_hr( "hr", hr );
    out( " major=" );
    out_guid( &major );
    verdict( hr == S_OK && guid_eq( &major, &MFMediaType_Audio ),
             "not MFMediaType_Audio" );

    begin( "IMFAttributes::GetGUID(MF_MT_SUBTYPE)" );
    hr = IMFMediaType_GetGUID( native, &MF_MT_SUBTYPE, &subtype );
    out_hr( "hr", hr );
    out( " subtype=" );
    out_guid( &subtype );
    verdict( hr == S_OK && guid_eq( &subtype, &MFAudioFormat_PCM ),
             "not MFAudioFormat_PCM" );

    begin( "IMFAttributes::GetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND)" );
    hr = IMFMediaType_GetUINT32( native, &MF_MT_AUDIO_SAMPLES_PER_SECOND,
                                 (UINT32 *)&rate );
    out_hr( "hr", hr );
    out( " rate=" );
    out_dec( rate );
    verdict( hr == S_OK && rate == want_rate, "wrong sample rate" );

    begin( "IMFAttributes::GetUINT32(MF_MT_AUDIO_NUM_CHANNELS)" );
    hr = IMFMediaType_GetUINT32( native, &MF_MT_AUDIO_NUM_CHANNELS,
                                 (UINT32 *)&channels );
    out_hr( "hr", hr );
    out( " channels=" );
    out_dec( channels );
    verdict( hr == S_OK && channels == want_channels, "wrong channel count" );

    begin( "IMFAttributes::GetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE)" );
    hr = IMFMediaType_GetUINT32( native, &MF_MT_AUDIO_BITS_PER_SAMPLE,
                                 (UINT32 *)&bits );
    out_hr( "hr", hr );
    out( " bits=" );
    out_dec( bits );
    verdict( hr == S_OK && bits == want_bits, "wrong sample width" );

    /* MFCreateMediaType is the other kind of flat vending export: it takes no
     * interface in and hands back one statically typed out. */
    begin( "MFCreateMediaType" );
    hr = MFCreateMediaType( &pcm );
    out_hr( "hr", hr );
    verdict( hr == S_OK && pcm != NULL, "no media type" );

    if (pcm)
    {
        begin( "IMFAttributes::SetGUID(MAJOR_TYPE=Audio, SUBTYPE=PCM)" );
        hr = IMFMediaType_SetGUID( pcm, &MF_MT_MAJOR_TYPE, &MFMediaType_Audio );
        if (SUCCEEDED(hr))
            hr = IMFMediaType_SetGUID( pcm, &MF_MT_SUBTYPE, &MFAudioFormat_PCM );
        if (SUCCEEDED(hr))
            hr = IMFMediaType_SetUINT32( pcm, &MF_MT_AUDIO_SAMPLES_PER_SECOND, rate );
        if (SUCCEEDED(hr))
            hr = IMFMediaType_SetUINT32( pcm, &MF_MT_AUDIO_NUM_CHANNELS, channels );
        if (SUCCEEDED(hr))
            hr = IMFMediaType_SetUINT32( pcm, &MF_MT_AUDIO_BITS_PER_SAMPLE, bits );
        out_hr( "hr", hr );
        verdict( hr == S_OK, "could not describe the PCM type" );

        /* PPC64EC step C, the FORWARD floating-point path.  SetDouble's
         * value travels BY VALUE in XMM1 and can only reach the native
         * callee through the surface's floating-point invoker
         * (winecom_surface::invoke_fp) -- this row was a named refusal until
         * that existed, and WINEEMUNOCOMFP=1 makes it one again, which is
         * the gate's negative control.  GetDouble returns the value through
         * a POINTER (no FP register on the way back), so this round trip
         * isolates the argument direction; the bits are printed raw so the
         * comparison is exact and the transcript byte-stable.  On the native
         * leg the same calls run natively -- identical output, layer 4's
         * rule. */
        begin( "IMFAttributes::SetDouble/GetDouble round trip" );
        {
            /* an attribute store takes any key; this one is the gate's own.
             * The round trip runs on its OWN media type, released before the
             * reader ever sees it: a foreign attribute on the type handed to
             * SetCurrentMediaType changes what winegstreamer negotiates, and
             * the first cut of this step proved it -- the guest leg's refused
             * SetDouble left the two legs' types DIFFERENT and their sample
             * chunking diverged.  A probe must measure the boundary, not
             * steer the pipeline. */
            static const GUID fp_key =
                {0x5ec5b3c1, 0xf90a, 0x4c17,
                 {0x9e,0xc6,0x0f,0x70,0xca,0x11,0x57,0x0d}};
            union { double d; ULONGLONG bits; } want, back;
            IMFMediaType *fpstore = NULL;
            double got = 0.0;

            want.d = 3.140000000000000124900090270330; /* WINECOM_ST_DOUBLE */
            hr = MFCreateMediaType( &fpstore );
            if (SUCCEEDED(hr))
                hr = IMFMediaType_SetDouble( fpstore, &fp_key, want.d );
            if (SUCCEEDED(hr))
                hr = IMFMediaType_GetDouble( fpstore, &fp_key, &got );
            back.d = got;
            out_hr( "hr", hr );
            out( " bits=" );
            out_hex( (ULONG)(back.bits >> 32), 8 );
            out_hex( (ULONG)back.bits, 8 );
            verdict( hr == S_OK && back.bits == want.bits,
                     "the by-value double did not survive the crossing" );
            if (fpstore) IMFMediaType_Release( fpstore );
        }

        /* THE PROPVARIANT FAMILY (the 2026-09-01 completeness pass).
         * SetItem/GetItem were WHOLE-SLOT refusals until the per-tag hand
         * walkers existed; three legs, one per translation class:
         *
         *   VT_UI8    plain bits through the union -- the baseline.
         *   VT_LPWSTR a payload POINTER both ways: in, the callee copies the
         *             guest's string; out, the callee CoTaskMemAllocs one
         *             and the guest's PropVariantClear frees it by crossing
         *             back into the same native allocator -- one process,
         *             one heap, which is the ownership argument in
         *             mfcom.c's mf_pv_in banner.
         *   VT_UNKNOWN the interface arm: the object stored through the
         *             hand walker's unwrap must come back from GetItem as
         *             the SAME pointer this leg stored, because
         *             winecom_wrap interns by host identity -- on the guest
         *             leg that pointer is a proxy and the equality PROVES
         *             the round trip went host-object -> same-proxy; then a
         *             QueryInterface to IMFMediaType proves the proxy is
         *             live.  Both legs print same=1 qi=1, so the transcript
         *             stays byte-identical.
         */
        begin( "IMFAttributes::SetItem/GetItem PROPVARIANT round trips" );
        {
            static const GUID pv_key_u8 =
                {0x7a01b2c3, 0x11d4, 0x4e55,
                 {0x8f,0x60,0x71,0x82,0x93,0xa4,0xb5,0xc6}};
            static const GUID pv_key_str =
                {0x7a01b2c4, 0x11d4, 0x4e55,
                 {0x8f,0x60,0x71,0x82,0x93,0xa4,0xb5,0xc6}};
            static const GUID pv_key_unk =
                {0x7a01b2c5, 0x11d4, 0x4e55,
                 {0x8f,0x60,0x71,0x82,0x93,0xa4,0xb5,0xc6}};
            static const WCHAR text[] = {'p','v','-','l','a','n','e',0};
            IMFMediaType *store = NULL, *stored = NULL, *qi = NULL;
            PROPVARIANT pv, got;
            ULONGLONG u8_bits = 0;
            ULONG str_ok = 0, same = 0, qi_ok = 0;

            hr = MFCreateMediaType( &store );

            if (SUCCEEDED(hr))                                  /* VT_UI8 */
            {
                pv_zero( &pv );
                pv.vt = VT_UI8;
                pv.uhVal.QuadPart = 0x1122334455667788ULL;
                hr = IMFMediaType_SetItem( store, &pv_key_u8, &pv );
            }
            if (SUCCEEDED(hr))
            {
                pv_zero( &got );
                hr = IMFMediaType_GetItem( store, &pv_key_u8, &got );
                if (SUCCEEDED(hr))
                {
                    if (got.vt == VT_UI8) u8_bits = got.uhVal.QuadPart;
                    PropVariantClear( &got );
                }
            }

            if (SUCCEEDED(hr))                                  /* VT_LPWSTR */
            {
                pv_zero( &pv );
                pv.vt = VT_LPWSTR;
                pv.pwszVal = (WCHAR *)text;
                hr = IMFMediaType_SetItem( store, &pv_key_str, &pv );
            }
            if (SUCCEEDED(hr))
            {
                pv_zero( &got );
                hr = IMFMediaType_GetItem( store, &pv_key_str, &got );
                if (SUCCEEDED(hr))
                {
                    UINT i;
                    str_ok = got.vt == VT_LPWSTR && got.pwszVal != NULL;
                    for (i = 0; str_ok && i < ARRAYSIZE(text); i++)
                        if (got.pwszVal[i] != text[i]) str_ok = 0;
                    PropVariantClear( &got );
                }
            }

            if (SUCCEEDED(hr))                                  /* VT_UNKNOWN */
                hr = MFCreateMediaType( &stored );
            if (SUCCEEDED(hr))
            {
                pv_zero( &pv );
                pv.vt = VT_UNKNOWN;
                pv.punkVal = (IUnknown *)stored;
                hr = IMFMediaType_SetItem( store, &pv_key_unk, &pv );
            }
            if (SUCCEEDED(hr))
            {
                pv_zero( &got );
                hr = IMFMediaType_GetItem( store, &pv_key_unk, &got );
                if (SUCCEEDED(hr))
                {
                    /* COM's identity rule: two references are the same
                     * object iff their QI(IID_IUnknown) answers are EQUAL
                     * POINTERS.  The raw values may differ legitimately --
                     * the stored reference was wrapped as IMFMediaType and
                     * the returned one as IUnknown, two interned proxies
                     * over one host object -- and proxy_qi canonicalises
                     * both to the same IUnknown, which is the assertion
                     * that actually proves host-object identity survived
                     * the round trip. */
                    IUnknown *canon_in = NULL, *canon_out = NULL;
                    if (got.vt == VT_UNKNOWN && got.punkVal &&
                        SUCCEEDED(IUnknown_QueryInterface( (IUnknown *)stored,
                                      &IID_IUnknown, (void **)&canon_in )) &&
                        SUCCEEDED(IUnknown_QueryInterface( got.punkVal,
                                      &IID_IUnknown, (void **)&canon_out )))
                        same = canon_in == canon_out;
                    if (canon_in) IUnknown_Release( canon_in );
                    if (canon_out) IUnknown_Release( canon_out );
                    if (got.vt == VT_UNKNOWN && got.punkVal &&
                        SUCCEEDED(IUnknown_QueryInterface( got.punkVal,
                                      &IID_IMFMediaType, (void **)&qi )) && qi)
                    {
                        qi_ok = 1;
                        IMFMediaType_Release( qi );
                    }
                    PropVariantClear( &got );
                }
            }

            out_hr( "hr", hr );
            out( " u8=" );
            out_hex( (ULONG)(u8_bits >> 32), 8 );
            out_hex( (ULONG)u8_bits, 8 );
            out( str_ok ? " str=1" : " str=0" );
            out( same ? " same=1" : " same=0" );
            out( qi_ok ? " qi=1" : " qi=0" );
            verdict( hr == S_OK && u8_bits == 0x1122334455667788ULL &&
                     str_ok && same && qi_ok,
                     "a PROPVARIANT arm did not survive the crossing" );
            if (stored) IMFMediaType_Release( stored );
            if (store) IMFMediaType_Release( store );
        }

        /* CA_IFACE_IN: a guest-held proxy travelling back INTO native MF as an
         * argument.  winecom_translate_in unwraps it to its host pointer --
         * the forward half of design §6.3.  A guest-IMPLEMENTED IMFMediaType
         * would be refused here instead, which is what mf_async_probe.c
         * measures. */
        begin( "IMFSourceReader::SetCurrentMediaType(PCM)" );
        hr = IMFSourceReader_SetCurrentMediaType( reader,
                 MF_SOURCE_READER_FIRST_AUDIO_STREAM, NULL, pcm );
        out_hr( "hr", hr );
        verdict( hr == S_OK, "not S_OK" );
    }

    /* The loop a game's cutscene decoder actually is. */
    begin( "IMFSourceReader::ReadSample loop (synchronous)" );
    read_all( reader, &pass1 );
    out_decoded( &pass1 );
    verdict( SUCCEEDED(pass1.hr) && pass1.samples > 0 &&
             pass1.total == want_bytes && pass1.fnv == want_fnv,
             "the decoded PCM does not match the source file" );

    begin( "presentation timestamps" );
    out( "first=" );
    out_dec64( (ULONGLONG)pass1.first_ts );
    out( " last=" );
    out_dec64( (ULONGLONG)pass1.last_ts );
    verdict( pass1.first_ts == 0 && pass1.last_ts > 0 &&
             pass1.last_ts < 10000000,
             "timestamps are not a monotonic run inside one second" );

    /* The first of the two HAND-WRITTEN slots (dlls/mfplat/mfcom.c
     * hand_propvariant_out): a PROPVARIANT the callee fills in, which the
     * table-driven classifier must refuse and a human can serve with the tag
     * audited.  MF_PD_DURATION is how a player sizes its progress bar. */
    begin( "IMFSourceReader::GetPresentationAttribute(MF_PD_DURATION)" );
    for (n = 0; n < sizeof(pv); n++) ((BYTE *)&pv)[n] = 0;
    hr = IMFSourceReader_GetPresentationAttribute( reader,
             MF_SOURCE_READER_MEDIASOURCE, &MF_PD_DURATION, &pv );
    out_hr( "hr", hr );
    out( " vt=" );
    out_dec( pv.vt );
    out( " duration=" );
    out_dec64( pv.uhVal.QuadPart );
    verdict( hr == S_OK && pv.vt == VT_UI8 &&
             pv.uhVal.QuadPart > 9900000 && pv.uhVal.QuadPart < 10100000,
             "not one second in 100ns units" );

    /* The second hand-written slot (hand_propvariant_in): a PROPVARIANT the
     * CALLER supplies.  Seeking to zero and reading the whole file again must
     * reproduce the hash exactly -- which is what separates "SetCurrentPosition
     * returned S_OK" from "SetCurrentPosition rewound". */
    begin( "IMFSourceReader::SetCurrentPosition(0)" );
    for (n = 0; n < sizeof(time_format); n++) ((BYTE *)&time_format)[n] = 0;
    for (n = 0; n < sizeof(pv); n++) ((BYTE *)&pv)[n] = 0;
    pv.vt = VT_I8;
    pv.hVal.QuadPart = 0;
    hr = IMFSourceReader_SetCurrentPosition( reader, &time_format, &pv );
    out_hr( "hr", hr );
    verdict( hr == S_OK, "not S_OK" );

    begin( "IMFSourceReader::ReadSample loop again after the seek" );
    read_all( reader, &pass2 );
    out_decoded( &pass2 );
    verdict( SUCCEEDED(pass2.hr) && pass2.total == pass1.total &&
             pass2.fnv == pass1.fnv && pass2.first_ts == 0,
             "the seek did not rewind: the second pass decoded different bytes" );

    if (pcm) IMFMediaType_Release( pcm );

release:
    if (native)
    {
        begin( "IUnknown::Release the native media type" );
        n = IMFMediaType_Release( native );
        out( "refs=" );
        out_dec( n );
        verdict( n == 0, "last reference did not drop to zero" );
    }

    begin( "IUnknown::Release the source reader" );
    n = IMFSourceReader_Release( reader );
    out( "refs=" );
    out_dec( n );
    verdict( n == 0, "last reference did not drop to zero" );

shutdown:
    begin( "MFShutdown" );
    hr = MFShutdown();
    out_hr( "hr", hr );
    verdict( hr == S_OK, "not S_OK" );

done:
    out( failures ? "mf_smoke: FAIL " : "mf_smoke: PASS " );
    out_dec( (ULONG)(step - failures) );
    out( "/" );
    out_dec( (ULONG)step );
    out( "\n" );
    return failures ? 1 : 0;
}

#ifdef MF_SMOKE_NO_CRT
/* The guest build has no C runtime: this IS the image entry point. */
void WINAPI mf_smoke_entry( void )
{
    ExitProcess( (UINT)mf_smoke_run() );
}
#else
int main( void )
{
    return mf_smoke_run();
}
#endif
