/*
 * mf_modules -- the runtime gate for mfmediaengine, wmvcore and evr.
 *
 * ONE source, built TWICE and run twice: as a native ppc64 Windows PE and as
 * an x86-64 guest PE.  The two runs must print byte-identical output, and
 * what they print is not "it started" -- it is network and ready states,
 * error codes, stream counts, a media duration in 100ns units, a 2D buffer's
 * contiguous length and pitch, and the exact HRESULTs Wine's own
 * implementations return.  Every number is either arithmetic the gate does
 * for itself in python (the duration, the buffer geometry) or a fact about
 * Wine's own code that is quoted beside the check.
 *
 * WHY THIS FILE EXISTS.  ppc64le/mf/README.md said, correctly, that the three
 * modules added to the roster were "surface built, unexercised": no guest had
 * ever driven one.  This is the program that drives them.
 *
 * WHAT IT FOUND ON THE FIRST RUN, and it is the reason a gate is worth more
 * than a build: mfmediaengine's ONLY door was welded shut.  Its one usable
 * flat export is DllGetClassObject, whose wrapper wraps the result by IID --
 * and IClassFactory was not on the Media Foundation roster, so the wrapper
 * released the class object and answered E_NOINTERFACE.  There was no second
 * way in.  ppc64le/mf/gen_interfaces.py now puts IClassFactory on the roster
 * beside IUnknown, and step 4 below is the check that would have caught it.
 *
 * THE THREE LANES, and what each one proves:
 *
 *   mfmediaengine  the whole point of a media engine is that it is
 *                  CALLBACK-DRIVEN: the application implements
 *                  IMFMediaEngineNotify and Media Foundation calls EventNotify
 *                  from its own thread for every state change.  This program
 *                  implements one -- an x86-64 vtable at a guest address --
 *                  and hands it in through IMFAttributes::SetUnknown, which is
 *                  a REVERSE PROXY crossing.  Then it drives the engine
 *                  through a load that must FAIL (a URL that names nothing,
 *                  so the answer needs no codec and no device and is exactly
 *                  MF_MEDIA_ENGINE_ERR_SRC_NOT_SUPPORTED) and a load that must
 *                  SUCCEED (the gate's own WAV), and checks the state each
 *                  leg leaves behind through GetNetworkState/GetReadyState --
 *                  two of the eight USHORT-returning slots a generator
 *                  inconsistency was refusing until this roster was rebuilt.
 *
 *                  AND IT IS WHERE THE FLOATING-POINT RETURNS GET DRIVEN.
 *                  PPC64EC step C served slots whose RETURN VALUE travels in
 *                  XMM0 rather than RAX and disclosed, correctly, that no
 *                  live title had ever put a number through one -- the
 *                  argument direction was measured (IMFAttributes::SetDouble,
 *                  check-mf-smoke.sh) and the return direction was built,
 *                  named and untested, filed as needing a MediaEngine title.
 *                  It needs no title: eleven of the seventeen .fpret rows in
 *                  dlls/mfplat/mf_marshal.h belong to IMFMediaEngine and
 *                  IMFMediaEngineEx, and six of those answer on a fresh
 *                  engine with no media, no device and no audio endpoint.
 *                  The block marked THE FLOATING-POINT RETURNS below reads
 *                  three constants Wine's own init_media_engine wrote
 *                  (including a quiet NaN this program could not have
 *                  produced), round-trips three distinctive bit patterns in
 *                  through the FP argument and out through the FP return,
 *                  and compares RAW BITS in both directions and in both
 *                  builds.
 *
 *   wmvcore        IWMSyncReader is the synchronous analogue of
 *                  IMFSourceReader and needs no callback at all.  Wine's
 *                  wmvcore sits on the same winegstreamer pipeline mfplat's
 *                  source reader does and its parser is the generic one
 *                  (wg_parser_create(FALSE) in dlls/winegstreamer/wm_reader.c),
 *                  so it opens the gate's own WAV and the duration it reports
 *                  is arithmetic the gate can do for itself.  GetNextSample is
 *                  REFUSED by the marshal tables -- INSSBuffer ** is a
 *                  pointer-to-pointer the generator cannot prove is plain
 *                  memory -- so a guest cannot DECODE through wmvcore
 *                  today.  Everything up to the decode is driven and
 *                  measured; the gap is stated in ppc64le/mf/README.md rather
 *                  than hidden behind a check only one of the two builds
 *                  could pass.
 *
 *   evr            the half of the enhanced video renderer that is not
 *                  DirectX.  MFCreateVideoSampleAllocator with no device
 *                  manager allocates system memory through
 *                  MFCreate2DMediaBuffer (dlls/evr/sample.c), so a guest can
 *                  drive the real allocator with no D3D device anywhere: the
 *                  sample count is enforced, the 2D buffer's contiguous length
 *                  is width*height*4 for RGB32, and its pitch is the aligned
 *                  stride.  Both are computed by the gate, not by this
 *                  program.  MFCreateVideoSampleFromSurface(NULL) is the
 *                  documented "a sample with no buffer" form and is the one
 *                  argument shape this surface can serve of the three evr
 *                  exports that take a Direct3D object.  The NON-null form is
 *                  refused by name in dlls/evr/mfcom.c and is deliberately
 *                  not driven here: the refusal exists only on the guest
 *                  side, so a native run would hand a real IDirect3DSurface9
 *                  path an object that is not one, and the two transcripts
 *                  could not be compared.
 *
 * THE ENTRY POINTS COME FROM LoadLibraryW + GetProcAddress, in both builds,
 * and that is a decision rather than a convenience.  mfmediaengine has no
 * import library at all (its Makefile.in declares none, because every export
 * but DllGetClassObject is -private), so a native build could not link
 * against it; and doing the same for all three keeps ONE code path in a file
 * whose whole value is that both builds execute the same one.  It also means
 * the guest run exercises the runtime LoadLibrary/GetProcAddress path into a
 * guest module's namespace for these three modules, which is a separate thing
 * from a static import and is what a plugin-shaped video layer actually does.
 *
 * NO C RUNTIME on the guest side (-DMF_MODULES_NO_CRT), for the reason
 * ppc64le/mf/probes/mf_smoke.c gives: a CRT would add a second variable to a
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

/* The attribute keys and the media subtypes are spelled EXTERN_GUID, which
 * declares without defining even under INITGUID.  Wine's own
 * libs/mfuuid/mfuuid.c does exactly this redefinition for exactly this
 * reason; here it is what lets the guest build link with no import library
 * for the GUIDs at all. */
#undef EXTERN_GUID
#define EXTERN_GUID DEFINE_GUID

#include <windows.h>
#include <objbase.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mfobjects.h>
#include <mferror.h>
#include <mfmediaengine.h>
#include <evr.h>
#include <wmsdkidl.h>
#include <asferr.h>   /* ASF_E_NOTFOUND, which wmsdkidl.h does not pull in */

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

/* Raw bits, and they are the point wherever this is used: a double printed as
 * a decimal number would hide exactly the failures the FP-return steps below
 * exist to catch -- a low half taken from a stale register, a single-precision
 * narrowing on the way through, a sign bit lost.  Sixteen digits, always, so
 * two transcripts line up column for column. */
static void out_hex64( ULONGLONG v, int digits )
{
    static const char hex[] = "0123456789ABCDEF";
    char buf[17];
    int i;

    for (i = 0; i < digits; i++) buf[digits - 1 - i] = hex[(v >> (4 * i)) & 0xf];
    buf[digits] = 0;
    out( buf );
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

static BOOL guid_eq( const GUID *a, const GUID *b )
{
    const BYTE *p = (const BYTE *)a, *q = (const BYTE *)b;
    int i;

    for (i = 0; i < (int)sizeof(GUID); i++) if (p[i] != q[i]) return FALSE;
    return TRUE;
}

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

/* The expected numbers are properties of the media the gate writes and of the
 * geometry the gate chooses, passed in rather than hard-coded so the gate owns
 * the arithmetic and this program owns the mechanism.  Read as decimal out of
 * the environment; no CRT, so parsed here. */
static ULONGLONG env_dec( const WCHAR *name, ULONGLONG fallback )
{
    WCHAR buf[32];
    ULONGLONG v = 0;
    DWORD n, i;

    n = GetEnvironmentVariableW( name, buf, 32 );
    if (!n || n >= 32) return fallback;
    for (i = 0; i < n; i++)
    {
        if (buf[i] < '0' || buf[i] > '9') return fallback;
        v = v * 10 + (ULONGLONG)(buf[i] - '0');
    }
    return v;
}

/* ------------------------------------------------- doubles, seen as bits */
/*
 * Every floating-point check in this file compares RAW BITS, never values,
 * and the values themselves are written as bit patterns rather than as
 * decimal literals.  Both halves of that are deliberate.
 *
 * Comparing bits is what makes the check exact: `==` on two doubles is true
 * for +0.0 and -0.0, is false for two identical NaNs, and says nothing at all
 * about the eight bytes that actually crossed the boundary.  The boundary is
 * what is under test.
 *
 * Writing the constants as bits is what makes the transcript's claim
 * checkable by hand: the number in the source IS the number the run must
 * print, with no compiler's decimal-to-binary rounding between them.  Each
 * pattern below has all-but-no-repeated nibbles on purpose -- a call that
 * copied only the low half, only the high half, byte-swapped, or narrowed to
 * single precision and widened again would land on a DIFFERENT pattern and
 * say so, where a tidy constant like 0.5 (0x3FE0000000000000, fifteen zero
 * nibbles) would survive most of those and pass.
 */
union fpbits { double d; ULONGLONG bits; };

static double bits_to_double( ULONGLONG bits )
{
    union fpbits v;

    v.bits = bits;
    return v.d;
}

static ULONGLONG double_to_bits( double d )
{
    union fpbits v;

    v.d = d;
    return v.bits;
}

/* The three values driven through the engine's FP setters, and the three
 * constants Wine's own dlls/mfmediaengine/main.c init_media_engine writes
 * into a fresh engine, which are what the FIRST reads must return before
 * anything has been set.
 *
 * NOTE what 1.0 and NaN buy that a set/get round trip alone does not.  A
 * refused float-bearing slot leaves *fpret_bits at zero and libs/winecom's
 * dispatcher writes that whole zero into XMM0 (winecom.c, the sl->fpret
 * arm), so a probe that only ever compared 0.0 would pass with the FP
 * invoker switched OFF.  1.0 and a quiet NaN are values NOTHING on the
 * refusal path can produce, and the NaN in particular was never set by this
 * program at all: it was born in native ppc64 code and the only way it
 * reaches an x86-64 guest's XMM0 is across the served FP return. */
#define ENGINE_FRESH_RATE     0x3FF0000000000000ull   /* 1.0  -- default_playback_rate, playback_rate */
#define ENGINE_FRESH_VOLUME   0x3FF0000000000000ull   /* 1.0  -- volume */
#define ENGINE_FRESH_DURATION 0x7FF8000000000000ull   /* NaN  -- duration, with no source */
#define ENGINE_SET_DEFRATE    0x4059FEDCBA987654ull   /* ~103.98 */
#define ENGINE_SET_RATE       0xC00FEDCBA9876543ull   /* ~-3.9902, sign bit SET */
#define ENGINE_SET_VOLUME     0x3FD23456789ABCDEull   /* ~0.2846 */

/* --------------------------------- a guest-implemented IMFMediaEngineNotify */
/*
 * The object that makes a media engine useful, and the one crossing on this
 * lane that goes the OTHER way: native Media Foundation calls EventNotify on
 * a vtable that lives in this image, from a thread this program never
 * created.  In the guest build that vtable is x86-64 code at a guest address
 * and the call arrives through libs/winecom/reverse.c; in the native build it
 * is an ordinary ppc64 call.  Both must produce the same transcript.
 *
 * Events are recorded into a bitmask plus a small ring of (event, param1)
 * pairs rather than printed here, for the reason mf_async_probe.c's Invoke
 * gives: this runs on MF's own thread and two threads' WriteFile calls
 * interleaving mid-line would garble the transcript without changing whether
 * the run passes.  The main thread prints, after WaitForSingleObject has made
 * these values visible to it.
 */

struct guest_notify
{
    IMFMediaEngineNotify IMFMediaEngineNotify_iface;
    LONG refs;
};

static struct guest_notify the_notify;

#define EVENT_SLOTS 64
static HANDLE          notify_event;       /* set on every EventNotify */
static volatile LONG   notify_count;
static DWORD           notify_events[EVENT_SLOTS];
static DWORD_PTR       notify_param1[EVENT_SLOTS];
static DWORD           notify_param2[EVENT_SLOTS];
static DWORD           main_tid;
static DWORD           notify_tid;

/* The log is append-only and read from a MARK rather than reset, because
 * resetting a counter another thread is still incrementing is a race even
 * when it looks harmless.  The second load's checks start from the mark the
 * first load left. */
static BOOL saw_event_from( LONG mark, DWORD want, DWORD *param1, DWORD *param2 )
{
    LONG n = notify_count, i;

    if (n > EVENT_SLOTS) n = EVENT_SLOTS;
    for (i = mark; i < n; i++)
        if (notify_events[i] == want)
        {
            if (param1) *param1 = (DWORD)notify_param1[i];
            if (param2) *param2 = notify_param2[i];
            return TRUE;
        }
    return FALSE;
}

/* Bounded on purpose: a wait that never returns is not a result.  The caller
 * turns a timeout into a FAIL that names the event it never saw. */
static BOOL wait_for_event( LONG mark, DWORD want, DWORD ms, DWORD *param1, DWORD *param2 )
{
    DWORD waited = 0;

    for (;;)
    {
        if (saw_event_from( mark, want, param1, param2 )) return TRUE;
        if (waited >= ms) return FALSE;
        WaitForSingleObject( notify_event, 100 );
        waited += 100;
    }
}

static ULONG STDMETHODCALLTYPE nf_AddRef( IMFMediaEngineNotify *iface )
{
    return ++the_notify.refs;
}

static ULONG STDMETHODCALLTYPE nf_Release( IMFMediaEngineNotify *iface )
{
    return --the_notify.refs;
}

static HRESULT STDMETHODCALLTYPE nf_QueryInterface( IMFMediaEngineNotify *iface,
                                                    REFIID riid, void **out_iface )
{
    if (guid_eq( riid, &IID_IUnknown ) || guid_eq( riid, &IID_IMFMediaEngineNotify ))
    {
        *out_iface = iface;
        nf_AddRef( iface );
        return S_OK;
    }
    *out_iface = NULL;
    return E_NOINTERFACE;
}

static HRESULT STDMETHODCALLTYPE nf_EventNotify( IMFMediaEngineNotify *iface,
                                                 DWORD event, DWORD_PTR param1, DWORD param2 )
{
    LONG i = notify_count;

    notify_tid = GetCurrentThreadId();
    if (i < EVENT_SLOTS)
    {
        notify_events[i] = event;
        notify_param1[i] = param1;
        notify_param2[i] = param2;
        notify_count = i + 1;
    }
    /* SetEvent/WaitForSingleObject is the happens-before edge that makes the
     * three stores above visible to the main thread, the same one
     * mf_async_probe.c's Invoke relies on; there is no lock here and none is
     * needed, because nothing is read before that wait has returned. */
    if (notify_event) SetEvent( notify_event );
    return S_OK;
}

static IMFMediaEngineNotifyVtbl notify_vtbl =
{
    nf_QueryInterface,
    nf_AddRef,
    nf_Release,
    nf_EventNotify,
};

/* ------------------------------------------- the three modules' entry points */
/*
 * Typed by hand because they are fetched by name rather than imported.  Each
 * spelling is copied from the module's own .spec line, which is the same text
 * tools/spec2thunk parsed to build the guest thunk these will resolve to in
 * the guest run.
 */
typedef HRESULT (WINAPI *fn_DllGetClassObject)( REFCLSID, REFIID, void ** );
typedef HRESULT (WINAPI *fn_WMCreateSyncReader)( IUnknown *, DWORD, IWMSyncReader ** );
typedef HRESULT (WINAPI *fn_WMCreateProfileManager)( IWMProfileManager ** );
typedef HRESULT (WINAPI *fn_MFCreateVideoSampleAllocator)( REFIID, void ** );
typedef HRESULT (WINAPI *fn_MFCreateVideoSampleFromSurface)( IUnknown *, IMFSample ** );
typedef HRESULT (WINAPI *fn_MFCreateVideoMediaTypeFromSubtype)( const GUID *, IMFVideoMediaType ** );

static void *get_entry( const WCHAR *dll, const char *name )
{
    HMODULE mod = LoadLibraryW( dll );

    if (!mod) return NULL;
    return (void *)GetProcAddress( mod, name );
}

/* ---------------------------------------------------------- mfmediaengine */

static void lane_mediaengine( const WCHAR *url )
{
    IMFMediaEngineClassFactory *factory = NULL;
    IMFMediaEngine *engine = NULL;
    IMFMediaTimeRange *range = NULL;
    IMFMediaError *error = NULL;
    IMFAttributes *attrs = NULL;
    fn_DllGetClassObject get_class;
    IClassFactory *cf = NULL;
    DWORD p1 = 0, p2 = 0;
    ULONGLONG fp;
    LONG mark = 0;
    HRESULT hr;
    USHORT us;
    DWORD n;
    BOOL b;

    begin( "GetProcAddress mfmediaengine.dll!DllGetClassObject" );
    get_class = (fn_DllGetClassObject)get_entry( L"mfmediaengine.dll", "DllGetClassObject" );
    verdict( get_class != NULL, "the module did not load or has no such export" );
    if (!get_class) return;

    /* THE check that was failing before IClassFactory joined the roster: the
     * wrapper wraps by IID, an IID that is not on the roster is refused, and
     * this export is the only door mfmediaengine has. */
    begin( "DllGetClassObject(CLSID_MFMediaEngineClassFactory, IID_IClassFactory)" );
    hr = get_class( &CLSID_MFMediaEngineClassFactory, &IID_IClassFactory, (void **)&cf );
    out_hr( "hr", hr );
    verdict( hr == S_OK && cf != NULL, "no class object" );
    if (!cf) return;

    /* An unknown CLSID must be refused by Wine's own code rather than by the
     * boundary, and with Wine's own HRESULT: this is the check that the
     * FAILURE path crosses intact too, not only the success path. */
    begin( "DllGetClassObject(a CLSID this module does not serve)" );
    {
        static const GUID nonesuch =
            { 0xdeadbeef, 0x0000, 0x0000, { 0xc0, 0x00, 0, 0, 0, 0, 0, 0x46 } };
        IClassFactory *bogus = (IClassFactory *)(ULONG_PTR)1;

        hr = get_class( &nonesuch, &IID_IClassFactory, (void **)&bogus );
        out_hr( "hr", hr );
        verdict( hr == CLASS_E_CLASSNOTAVAILABLE && bogus == NULL,
                 "not CLASS_E_CLASSNOTAVAILABLE with a NULL out" );
    }

    begin( "IClassFactory::CreateInstance(IID_IMFMediaEngineClassFactory)" );
    hr = IClassFactory_CreateInstance( cf, NULL, &IID_IMFMediaEngineClassFactory,
                                       (void **)&factory );
    out_hr( "hr", hr );
    verdict( hr == S_OK && factory != NULL, "no media engine class factory" );
    IClassFactory_Release( cf );
    if (!factory) return;

    /* ---- IMFMediaError: four slots, and GetErrorCode is one of the eight
     * USHORT-returning ones a generator inconsistency was refusing.  Every
     * answer here is a value this program just wrote. */
    begin( "IMFMediaEngineClassFactory::CreateError" );
    hr = IMFMediaEngineClassFactory_CreateError( factory, &error );
    out_hr( "hr", hr );
    verdict( hr == S_OK && error != NULL, "no error object" );
    if (error)
    {
        begin( "IMFMediaError::GetErrorCode on a fresh object" );
        us = IMFMediaError_GetErrorCode( error );
        out( "code=" );
        out_dec( us );
        verdict( us == MF_MEDIA_ENGINE_ERR_NOERROR, "not MF_MEDIA_ENGINE_ERR_NOERROR" );

        begin( "IMFMediaError::SetErrorCode(DECODE) then GetErrorCode" );
        hr = IMFMediaError_SetErrorCode( error, MF_MEDIA_ENGINE_ERR_DECODE );
        us = IMFMediaError_GetErrorCode( error );
        out_hr( "hr", hr );
        out( " code=" );
        out_dec( us );
        verdict( hr == S_OK && us == MF_MEDIA_ENGINE_ERR_DECODE, "the code did not round trip" );

        begin( "IMFMediaError::SetExtendedErrorCode then GetExtendedErrorCode" );
        hr = IMFMediaError_SetExtendedErrorCode( error, (HRESULT)0x80070005 );
        out_hr( "hr", hr );
        out_hr( " ext", IMFMediaError_GetExtendedErrorCode( error ) );
        verdict( hr == S_OK && IMFMediaError_GetExtendedErrorCode( error ) == (HRESULT)0x80070005,
                 "the extended code did not round trip" );

        begin( "IMFMediaError::Release" );
        n = IMFMediaError_Release( error );
        out( "refs=" );
        out_dec( n );
        verdict( n == 0, "last reference did not drop to zero" );
        error = NULL;
    }

    /* ---- IMFMediaTimeRange.  AddRange and ContainsTime pass doubles by
     * value and are refused FORWARD by construction (they carry
     * WINECOM_F_REV), so what is checked here is the empty range's own
     * arithmetic, which is where a wrong slot number would show. */
    begin( "IMFMediaEngineClassFactory::CreateTimeRange" );
    hr = IMFMediaEngineClassFactory_CreateTimeRange( factory, &range );
    out_hr( "hr", hr );
    verdict( hr == S_OK && range != NULL, "no time range" );
    if (range)
    {
        begin( "IMFMediaTimeRange::GetLength on an empty range" );
        n = IMFMediaTimeRange_GetLength( range );
        out( "n=" );
        out_dec( n );
        verdict( n == 0, "an empty range is not empty" );

        begin( "IMFMediaTimeRange::GetStart(0) on an empty range" );
        {
            double start = 42.0;
            hr = IMFMediaTimeRange_GetStart( range, 0, &start );
            out_hr( "hr", hr );
            verdict( hr == E_INVALIDARG, "not E_INVALIDARG" );
        }

        begin( "IMFMediaTimeRange::Release" );
        n = IMFMediaTimeRange_Release( range );
        out( "refs=" );
        out_dec( n );
        verdict( n == 0, "last reference did not drop to zero" );
        range = NULL;
    }

    /* ---- the engine itself, which needs the notify callback to exist at
     * all: dlls/mfmediaengine/main.c's init_media_engine fails outright if
     * MF_MEDIA_ENGINE_CALLBACK is absent.  So this next step is also the
     * check that a GUEST-implemented object crossed INTO native MF. */
    begin( "MFCreateAttributes + SetUnknown(MF_MEDIA_ENGINE_CALLBACK)" );
    hr = MFCreateAttributes( &attrs, 2 );
    if (SUCCEEDED(hr))
        hr = IMFAttributes_SetUnknown( attrs, &MF_MEDIA_ENGINE_CALLBACK,
                                       (IUnknown *)&the_notify.IMFMediaEngineNotify_iface );
    out_hr( "hr", hr );
    verdict( hr == S_OK, "the notify object did not go into the attribute store" );

    begin( "IMFMediaEngineClassFactory::CreateInstance" );
    hr = IMFMediaEngineClassFactory_CreateInstance( factory, 0, attrs, &engine );
    out_hr( "hr", hr );
    verdict( hr == S_OK && engine != NULL, "no media engine" );
    if (attrs) IMFAttributes_Release( attrs );
    IMFMediaEngineClassFactory_Release( factory );
    if (!engine) return;

    /* ---- the state a freshly created engine is in.  Every one of these is
     * a constant in dlls/mfmediaengine/main.c's init_media_engine. */
    begin( "IMFMediaEngine::GetNetworkState before any source" );
    us = IMFMediaEngine_GetNetworkState( engine );
    out( "state=" );
    out_dec( us );
    verdict( us == MF_MEDIA_ENGINE_NETWORK_EMPTY, "not MF_MEDIA_ENGINE_NETWORK_EMPTY" );

    begin( "IMFMediaEngine::GetReadyState before any source" );
    us = IMFMediaEngine_GetReadyState( engine );
    out( "state=" );
    out_dec( us );
    verdict( us == MF_MEDIA_ENGINE_READY_HAVE_NOTHING, "not HAVE_NOTHING" );

    begin( "IMFMediaEngine::IsPaused on a fresh engine" );
    b = IMFMediaEngine_IsPaused( engine );
    out( "paused=" );
    out_dec( b ? 1 : 0 );
    verdict( b, "a fresh engine is not paused (FLAGS_ENGINE_PAUSED)" );

    begin( "IMFMediaEngine::HasVideo/HasAudio before any source" );
    out( "video=" );
    out_dec( IMFMediaEngine_HasVideo( engine ) ? 1 : 0 );
    out( " audio=" );
    out_dec( IMFMediaEngine_HasAudio( engine ) ? 1 : 0 );
    verdict( !IMFMediaEngine_HasVideo( engine ) && !IMFMediaEngine_HasAudio( engine ),
             "an engine with no source claims to have streams" );

    begin( "IMFMediaEngine::SetAutoPlay/GetAutoPlay round trip" );
    hr = IMFMediaEngine_SetAutoPlay( engine, TRUE );
    b = IMFMediaEngine_GetAutoPlay( engine );
    out_hr( "hr", hr );
    out( " v=" );
    out_dec( b ? 1 : 0 );
    verdict( hr == S_OK && b, "autoplay did not round trip" );

    begin( "IMFMediaEngine::SetLoop/GetLoop round trip" );
    hr = IMFMediaEngine_SetLoop( engine, TRUE );
    b = IMFMediaEngine_GetLoop( engine );
    out_hr( "hr", hr );
    out( " v=" );
    out_dec( b ? 1 : 0 );
    verdict( hr == S_OK && b, "loop did not round trip" );

    begin( "IMFMediaEngine::SetMuted/GetMuted round trip" );
    hr = IMFMediaEngine_SetMuted( engine, TRUE );
    b = IMFMediaEngine_GetMuted( engine );
    out_hr( "hr", hr );
    out( " v=" );
    out_dec( b ? 1 : 0 );
    verdict( hr == S_OK && b, "muted did not round trip" );

    begin( "IMFMediaEngine::SetPreload/GetPreload round trip" );
    hr = IMFMediaEngine_SetPreload( engine, MF_MEDIA_ENGINE_PRELOAD_AUTOMATIC );
    n = IMFMediaEngine_GetPreload( engine );
    out_hr( "hr", hr );
    out( " v=" );
    out_dec( n );
    verdict( hr == S_OK && n == MF_MEDIA_ENGINE_PRELOAD_AUTOMATIC, "preload did not round trip" );

    /* ---- THE FLOATING-POINT RETURNS, and this block is the only place on
     * this port where one has ever been value-driven.
     *
     * ppc64le/docs/ppc64ec.md's step C served them and said so plainly: the
     * FP-return path shares every byte of its machinery with the argument
     * direction check-mf-smoke.sh already drives (IMFAttributes::SetDouble),
     * but "no live title has driven GetCurrentTime yet" -- the return itself,
     * the value that travels in XMM0 rather than RAX, had never carried a
     * number end to end.  It needed a MediaEngine title, and there is not one
     * in the corpus.  It does not need a title.  A fresh engine has the whole
     * roster of reachable FP returns on it, with no media, no device, and no
     * audio endpoint: dlls/mfmediaengine/main.c stores rate, default rate and
     * volume in plain struct fields and hands them straight back.
     *
     * The mechanism under test, in one sentence: the guest calls a slot whose
     * marshal row carries .fpret = 1 (dlls/mfplat/mf_marshal.h,
     * slots_IMFMediaEngine), libs/winecom's invoke_marshalled routes it to
     * the surface's FP invoker instead of the widest-integer one,
     * include/wine/winecom_fpcall.h calls the native vtable slot and captures
     * f1, and winecom_dispatch materialises the guest context and writes
     * those bits into the WHOLE of XMM0.  Nothing in that chain is exercised
     * by an FP ARGUMENT: the argument direction reads XMM1-3 and returns an
     * HRESULT in RAX.
     *
     * Three claims, and they are separable on purpose:
     *
     *   READ-ONLY   the three fresh-engine constants and, most of all, the
     *               NaN duration.  This program never set any of them, so
     *               there is no round trip to be satisfied by a value that
     *               never left this image -- these bits were written by
     *               native ppc64 code and read back by an x86-64 guest, and
     *               that crossing is the entire claim.
     *   ROUND TRIP  a distinctive pattern in through the FP ARGUMENT and the
     *               SAME pattern out through the FP RETURN, bit for bit,
     *               including a negative one so a lost sign bit is a failure
     *               rather than a rounding-sized nuisance.
     *   NOT ZERO    every value checked here is non-zero, which is what makes
     *               the WINEEMUNOCOMFP=1 control in check-mf-modules.sh able
     *               to go red: a refused FP slot returns exactly 0.0.
     *
     * The setters are undone again at the end of the block so the load lanes
     * below meet the engine Wine's own defaults left. */
    begin( "IMFMediaEngine::GetDefaultPlaybackRate on a fresh engine (FP RETURN)" );
    fp = double_to_bits( IMFMediaEngine_GetDefaultPlaybackRate( engine ) );
    out( "bits=0x" );
    out_hex64( fp, 16 );
    verdict( fp == ENGINE_FRESH_RATE,
             "not the 1.0 init_media_engine wrote -- 0x0000000000000000 here "
             "is the refusal path, an FP return that never happened" );

    begin( "IMFMediaEngine::GetPlaybackRate on a fresh engine (FP RETURN)" );
    fp = double_to_bits( IMFMediaEngine_GetPlaybackRate( engine ) );
    out( "bits=0x" );
    out_hex64( fp, 16 );
    verdict( fp == ENGINE_FRESH_RATE, "not the 1.0 init_media_engine wrote" );

    begin( "IMFMediaEngine::GetVolume on a fresh engine (FP RETURN)" );
    fp = double_to_bits( IMFMediaEngine_GetVolume( engine ) );
    out( "bits=0x" );
    out_hex64( fp, 16 );
    verdict( fp == ENGINE_FRESH_VOLUME, "not the 1.0 init_media_engine wrote" );

    /* The strongest of the read-only checks, and the one worth reading twice.
     * init_media_engine sets engine->duration = NAN and there is no source,
     * so the eight bytes that must arrive are a quiet NaN with a zero
     * payload -- a pattern this program cannot have produced, cannot have
     * left lying in a register, and that no integer path could carry back in
     * RAX by accident.  It is also why the check is on BITS: `nan == nan` is
     * false, so a value comparison here would fail on a correct answer. */
    begin( "IMFMediaEngine::GetDuration with no source is a quiet NaN (FP RETURN)" );
    fp = double_to_bits( IMFMediaEngine_GetDuration( engine ) );
    out( "bits=0x" );
    out_hex64( fp, 16 );
    verdict( fp == ENGINE_FRESH_DURATION,
             "the NaN dlls/mfmediaengine/main.c wrote did not survive the "
             "crossing intact" );

    /* GetCurrentTime and GetStartTime are FP-return rows too and both are
     * driven here, but they are PRINTED rather than gated: a fresh engine's
     * answer to each is 0.0 (media_engine_GetCurrentTime with no clock time,
     * and GetStartTime which is a `return 0.0;` stub), and 0.0 is exactly
     * what a REFUSED fp row leaves in XMM0.  A check that could not tell a
     * served zero from a refused one would be an assertion this file could
     * make without the mechanism existing at all, which is the one thing no
     * step here is allowed to be.  The rows are exercised, the answer is on
     * the record, and the load-bearing claims are the five above and the
     * three below. */
    out( "note IMFMediaEngine::GetCurrentTime bits=0x" );
    out_hex64( double_to_bits( IMFMediaEngine_GetCurrentTime( engine ) ), 16 );
    out( " GetStartTime bits=0x" );
    out_hex64( double_to_bits( IMFMediaEngine_GetStartTime( engine ) ), 16 );
    out( " (both 0.0 on a fresh engine, so neither is gated: see the source)\n" );

    /* ---- and now the round trips: in through XMM1, out through XMM0. */
    begin( "IMFMediaEngine::SetDefaultPlaybackRate/Get round-trips raw bits" );
    hr = IMFMediaEngine_SetDefaultPlaybackRate( engine, bits_to_double( ENGINE_SET_DEFRATE ) );
    fp = double_to_bits( IMFMediaEngine_GetDefaultPlaybackRate( engine ) );
    out_hr( "hr", hr );
    out( " sent=0x" );
    out_hex64( ENGINE_SET_DEFRATE, 16 );
    out( " got=0x" );
    out_hex64( fp, 16 );
    verdict( hr == S_OK && fp == ENGINE_SET_DEFRATE,
             "the pattern that went in as an FP argument did not come back as "
             "an FP return" );

    /* Negative on purpose.  A path that moved the value through an integer
     * register, or that widened a single back to a double, would still get
     * the magnitude approximately right; only the sign bit and the low
     * mantissa nibbles say whether the eight bytes are the SAME eight bytes.
     * Wine's media_engine_SetPlaybackRate stores whatever it is handed -- it
     * validates nothing -- so this is a pure ABI measurement and not a claim
     * about what MF does with a reverse rate. */
    begin( "IMFMediaEngine::SetPlaybackRate/Get round-trips a NEGATIVE raw bit pattern" );
    hr = IMFMediaEngine_SetPlaybackRate( engine, bits_to_double( ENGINE_SET_RATE ) );
    fp = double_to_bits( IMFMediaEngine_GetPlaybackRate( engine ) );
    out_hr( "hr", hr );
    out( " sent=0x" );
    out_hex64( ENGINE_SET_RATE, 16 );
    out( " got=0x" );
    out_hex64( fp, 16 );
    verdict( hr == S_OK && fp == ENGINE_SET_RATE,
             "the negative pattern did not survive the round trip" );

    begin( "IMFMediaEngine::SetVolume/Get round-trips raw bits" );
    hr = IMFMediaEngine_SetVolume( engine, bits_to_double( ENGINE_SET_VOLUME ) );
    fp = double_to_bits( IMFMediaEngine_GetVolume( engine ) );
    out_hr( "hr", hr );
    out( " sent=0x" );
    out_hex64( ENGINE_SET_VOLUME, 16 );
    out( " got=0x" );
    out_hex64( fp, 16 );
    verdict( hr == S_OK && fp == ENGINE_SET_VOLUME,
             "the pattern that went in as an FP argument did not come back as "
             "an FP return" );

    /* Put the engine back the way init_media_engine left it.  Each of the
     * three setters above fires an EventNotify (RATECHANGE, VOLUMECHANGE)
     * through the reverse proxy, and so does each restore; the load lanes
     * below read the event log FROM A MARK, so those extra entries cost them
     * nothing -- but an engine left at ~104x playback and a quarter volume
     * would be a different engine from the one those lanes were written
     * against, and that IS worth undoing. */
    IMFMediaEngine_SetDefaultPlaybackRate( engine, bits_to_double( ENGINE_FRESH_RATE ) );
    IMFMediaEngine_SetPlaybackRate( engine, bits_to_double( ENGINE_FRESH_RATE ) );
    IMFMediaEngine_SetVolume( engine, bits_to_double( ENGINE_FRESH_VOLUME ) );

    begin( "the engine is back at its defaults after the FP round trips" );
    out( "rate=0x" );
    out_hex64( double_to_bits( IMFMediaEngine_GetPlaybackRate( engine ) ), 16 );
    out( " volume=0x" );
    out_hex64( double_to_bits( IMFMediaEngine_GetVolume( engine ) ), 16 );
    verdict( double_to_bits( IMFMediaEngine_GetPlaybackRate( engine ) ) == ENGINE_FRESH_RATE &&
             double_to_bits( IMFMediaEngine_GetVolume( engine ) ) == ENGINE_FRESH_VOLUME,
             "the restore did not take" );

    /* ---- the load that must FAIL, and must say why.  A URL naming a file
     * that does not exist needs no codec and no audio device, so the answer
     * is the same on any machine: dlls/mfmediaengine/main.c's load handler
     * sets NETWORK_NO_SOURCE and MF_MEDIA_ENGINE_ERR_SRC_NOT_SUPPORTED and
     * calls EventNotify(MF_MEDIA_ENGINE_EVENT_ERROR, err, hr).  That
     * EventNotify is the reverse crossing this lane exists to prove. */
    begin( "IMFMediaEngine::SetSource(a path that names nothing)" );
    hr = IMFMediaEngine_SetSource( engine, (BSTR)L"Z:\\nonesuch-ppc64le-gate.mp4" );
    out_hr( "hr", hr );
    verdict( hr == S_OK, "SetSource itself failed" );

    begin( "IMFMediaEngineNotify::EventNotify(ERROR) arrives from MF's own thread" );
    b = wait_for_event( 0, MF_MEDIA_ENGINE_EVENT_ERROR, 30000, &p1, &p2 );
    out( "arrived=" );
    out_dec( b ? 1 : 0 );
    out( " err=" );
    out_dec( p1 );
    verdict( b && p1 == MF_MEDIA_ENGINE_ERR_SRC_NOT_SUPPORTED,
             "no ERROR event carrying MF_MEDIA_ENGINE_ERR_SRC_NOT_SUPPORTED" );

    begin( "IMFMediaEngine::GetNetworkState after the failed load" );
    us = IMFMediaEngine_GetNetworkState( engine );
    out( "state=" );
    out_dec( us );
    verdict( us == MF_MEDIA_ENGINE_NETWORK_NO_SOURCE, "not MF_MEDIA_ENGINE_NETWORK_NO_SOURCE" );

    begin( "IMFMediaEngine::GetError then IMFMediaError::GetErrorCode" );
    hr = IMFMediaEngine_GetError( engine, &error );
    out_hr( "hr", hr );
    if (SUCCEEDED(hr) && error)
    {
        us = IMFMediaError_GetErrorCode( error );
        out( " code=" );
        out_dec( us );
        verdict( us == MF_MEDIA_ENGINE_ERR_SRC_NOT_SUPPORTED, "not SRC_NOT_SUPPORTED" );
        IMFMediaError_Release( error );
        error = NULL;
    }
    else verdict( FALSE, "no error object after a failed load" );

    /* ---- the load that must SUCCEED.  The gate's own WAV, so the only
     * decoder involved is the one ppc64le/mf/check-mf-smoke.sh already
     * proves byte-exact, and the states below are the ones
     * media_engine_load_handler_Invoke and media_engine_set_ready_state
     * write on the way through. */
    mark = notify_count;
    begin( "IMFMediaEngine::SetSource(the gate's WAV)" );
    hr = IMFMediaEngine_SetSource( engine, (BSTR)url );
    out_hr( "hr", hr );
    verdict( hr == S_OK, "SetSource itself failed" );

    begin( "IMFMediaEngineNotify::EventNotify(CANPLAY) arrives" );
    b = wait_for_event( mark, MF_MEDIA_ENGINE_EVENT_CANPLAY, 60000, NULL, NULL );
    out( "arrived=" );
    out_dec( b ? 1 : 0 );
    out( " loadedmetadata=" );
    out_dec( saw_event_from( mark, MF_MEDIA_ENGINE_EVENT_LOADEDMETADATA, NULL, NULL ) ? 1 : 0 );
    verdict( b && saw_event_from( mark, MF_MEDIA_ENGINE_EVENT_LOADEDMETADATA, NULL, NULL ),
             "the engine never reported the media as playable" );

    begin( "IMFMediaEngine::GetNetworkState after the good load" );
    us = IMFMediaEngine_GetNetworkState( engine );
    out( "state=" );
    out_dec( us );
    verdict( us == MF_MEDIA_ENGINE_NETWORK_IDLE, "not MF_MEDIA_ENGINE_NETWORK_IDLE" );

    begin( "IMFMediaEngine::GetReadyState after the good load" );
    us = IMFMediaEngine_GetReadyState( engine );
    out( "state=" );
    out_dec( us );
    verdict( us == MF_MEDIA_ENGINE_READY_HAVE_ENOUGH_DATA, "not HAVE_ENOUGH_DATA" );

    begin( "IMFMediaEngine::HasAudio/HasVideo after the good load" );
    out( "audio=" );
    out_dec( IMFMediaEngine_HasAudio( engine ) ? 1 : 0 );
    out( " video=" );
    out_dec( IMFMediaEngine_HasVideo( engine ) ? 1 : 0 );
    verdict( IMFMediaEngine_HasAudio( engine ) && !IMFMediaEngine_HasVideo( engine ),
             "a mono WAV did not present as audio-only" );

    /* The notify object crossed into native MF and native MF called back into
     * it; the thread it called on is MF's own, never this program's.  In the
     * guest build that is a thread that had never run guest code. */
    begin( "EventNotify ran on MF's thread rather than this one" );
    out( "other=" );
    out_dec( (notify_tid && notify_tid != main_tid) ? 1 : 0 );
    verdict( notify_tid && notify_tid != main_tid, "the callback ran on the calling thread" );

    begin( "IMFMediaEngine::Shutdown" );
    hr = IMFMediaEngine_Shutdown( engine );
    out_hr( "hr", hr );
    verdict( hr == S_OK, "not S_OK" );

    begin( "IMFMediaEngine::Release" );
    n = IMFMediaEngine_Release( engine );
    out( "refs=" );
    out_dec( n );
    verdict( n == 0, "last reference did not drop to zero" );

    /* Native MF took references on the notify object and must have given
     * every one of them back.  A reverse proxy that leaked a reference would
     * show here and nowhere else. */
    begin( "the guest notify object's refcount is back where it started" );
    out( "refs=" );
    out_dec( (ULONG)the_notify.refs );
    verdict( the_notify.refs == 0, "native MF kept a reference" );
}

/* ---------------------------------------------------------------- wmvcore */

static void lane_wmvcore( const WCHAR *path )
{
    fn_WMCreateProfileManager create_profile_manager;
    fn_WMCreateSyncReader create_sync_reader;
    IWMProfileManager *pm = NULL;
    IWMOutputMediaProps *props = NULL;
    IWMSyncReader *reader = NULL;
    IWMHeaderInfo *header = NULL;
    IWMProfile *profile = NULL;
    ULONGLONG want_duration;
    WMT_STREAM_SELECTION selection;
    WMT_ATTR_DATATYPE type;
    QWORD duration = 0;
    DWORD n, out_num;
    WORD stream, size;
    GUID major;
    HRESULT hr;

    want_duration = env_dec( L"MF_MODULES_DURATION", 0 );

    begin( "GetProcAddress wmvcore.dll!WMCreateProfileManager" );
    create_profile_manager =
        (fn_WMCreateProfileManager)get_entry( L"wmvcore.dll", "WMCreateProfileManager" );
    verdict( create_profile_manager != NULL, "the module did not load or has no such export" );

    /* Wine's profile manager is a real object whose every method is a named
     * E_NOTIMPL (dlls/wmvcore/wmvcore_main.c).  That makes it a clean test of
     * the boundary and nothing else: the object must be a proxy, QI must find
     * the derived interface on it, and Wine's own refusal must arrive
     * unaltered rather than being turned into something else on the way. */
    if (create_profile_manager)
    {
        begin( "WMCreateProfileManager" );
        hr = create_profile_manager( &pm );
        out_hr( "hr", hr );
        verdict( hr == S_OK && pm != NULL, "no profile manager" );
    }
    if (pm)
    {
        IWMProfileManager2 *pm2 = NULL;

        begin( "IWMProfileManager::QueryInterface(IID_IWMProfileManager2)" );
        hr = IWMProfileManager_QueryInterface( pm, &IID_IWMProfileManager2, (void **)&pm2 );
        out_hr( "hr", hr );
        verdict( hr == S_OK && pm2 != NULL, "the derived interface was not found" );
        if (pm2) IWMProfileManager2_Release( pm2 );

        begin( "IWMProfileManager::CreateEmptyProfile (Wine returns E_NOTIMPL)" );
        hr = IWMProfileManager_CreateEmptyProfile( pm, WMT_VER_9_0, &profile );
        out_hr( "hr", hr );
        verdict( hr == E_NOTIMPL, "not Wine's own E_NOTIMPL" );

        begin( "IWMProfileManager::Release" );
        n = IWMProfileManager_Release( pm );
        out( "refs=" );
        out_dec( n );
        verdict( n == 0, "last reference did not drop to zero" );
        pm = NULL;
    }

    begin( "GetProcAddress wmvcore.dll!WMCreateSyncReader" );
    create_sync_reader =
        (fn_WMCreateSyncReader)get_entry( L"wmvcore.dll", "WMCreateSyncReader" );
    verdict( create_sync_reader != NULL, "the module did not load or has no such export" );
    if (!create_sync_reader) return;

    begin( "WMCreateSyncReader" );
    hr = create_sync_reader( NULL, 0, &reader );
    out_hr( "hr", hr );
    verdict( hr == S_OK && reader != NULL, "no sync reader" );
    if (!reader) return;

    begin( "IWMSyncReader::Open" );
    hr = IWMSyncReader_Open( reader, path );
    out_hr( "hr", hr );
    verdict( hr == S_OK, "winegstreamer did not open the file" );

    begin( "IWMSyncReader::GetOutputCount" );
    n = 0;
    hr = IWMSyncReader_GetOutputCount( reader, &n );
    out_hr( "hr", hr );
    out( " n=" );
    out_dec( n );
    verdict( hr == S_OK && n == 1, "a single-stream file did not report one output" );

    begin( "IWMSyncReader::GetStreamNumberForOutput(0)" );
    stream = 0;
    hr = IWMSyncReader_GetStreamNumberForOutput( reader, 0, &stream );
    out_hr( "hr", hr );
    out( " stream=" );
    out_dec( stream );
    verdict( hr == S_OK && stream == 1, "stream numbers are 1-based; output 0 is not stream 1" );

    begin( "IWMSyncReader::GetOutputNumberForStream(1)" );
    out_num = 0xdeadbeef;
    hr = IWMSyncReader_GetOutputNumberForStream( reader, 1, &out_num );
    out_hr( "hr", hr );
    out( " output=" );
    out_dec( out_num );
    verdict( hr == S_OK && out_num == 0, "the round trip back to output 0 failed" );

    begin( "IWMSyncReader::GetStreamSelected(1)" );
    selection = (WMT_STREAM_SELECTION)0;
    hr = IWMSyncReader_GetStreamSelected( reader, 1, &selection );
    out_hr( "hr", hr );
    out( " selection=" );
    out_dec( (ULONG)selection );
    verdict( hr == S_OK && selection == WMT_ON, "a stream is not selected by default" );

    begin( "IWMSyncReader::GetOutputProps then IWMMediaProps::GetType" );
    hr = IWMSyncReader_GetOutputProps( reader, 0, &props );
    if (SUCCEEDED(hr) && props)
    {
        hr = IWMOutputMediaProps_GetType( props, &major );
        out_hr( "hr", hr );
        verdict( hr == S_OK && guid_eq( &major, &WMMEDIATYPE_Audio ),
                 "the output of a WAV is not WMMEDIATYPE_Audio" );
        IWMOutputMediaProps_Release( props );
        props = NULL;
    }
    else
    {
        out_hr( "hr", hr );
        verdict( FALSE, "no output properties object" );
    }

    /* IWMSyncReader::GetNextSample -- the decode call -- is deliberately NOT
     * called here, and that is the one honest gap on this lane.  It is
     * REFUSED by the marshal tables: `INSSBuffer **ppSample` is a
     * pointer-to-pointer whose pointee the generator cannot prove is plain
     * memory rather than an interface, so the slot carries no plan and
     * libs/winecom answers E_NOTIMPL loudly by name.  A guest therefore gets
     * a named refusal rather than samples, and calling it HERE would make the
     * two transcripts differ -- E_NOTIMPL in the guest run, Wine's own answer
     * in the native one -- which would break the byte-identity claim that is
     * the rest of this file's whole argument -- Wine's native GetNextSample
     * hands back a real INSSBuffer.  So the honest position is this: a guest
     * cannot DECODE through wmvcore on this port today, everything up to the
     * decode is driven and measured above, and the gap is written down in
     * ppc64le/mf/README.md rather than papered over with a check only one of
     * the two builds could pass.
     */

    begin( "IWMSyncReader::QueryInterface(IID_IWMHeaderInfo)" );
    hr = IWMSyncReader_QueryInterface( reader, &IID_IWMHeaderInfo, (void **)&header );
    out_hr( "hr", hr );
    verdict( hr == S_OK && header != NULL, "the reader does not expose IWMHeaderInfo" );

    if (header)
    {
        begin( "IWMHeaderInfo::GetAttributeByName(Duration), size query" );
        stream = 0;
        size = 0;
        type = 0;
        hr = IWMHeaderInfo_GetAttributeByName( header, &stream, L"Duration", &type, NULL, &size );
        out_hr( "hr", hr );
        out( " type=" );
        out_dec( type );
        out( " size=" );
        out_dec( size );
        verdict( hr == S_OK && type == WMT_TYPE_QWORD && size == 8,
                 "Duration is not an 8-byte QWORD" );

        begin( "IWMHeaderInfo::GetAttributeByName(Duration), value" );
        stream = 0;
        size = sizeof(duration);
        hr = IWMHeaderInfo_GetAttributeByName( header, &stream, L"Duration", &type,
                                               (BYTE *)&duration, &size );
        out_hr( "hr", hr );
        out( " duration=" );
        out_dec64( duration );
        verdict( hr == S_OK && duration == want_duration,
                 "the duration is not the one the gate computed from the file" );

        begin( "IWMHeaderInfo::GetAttributeByName(Seekable)" );
        {
            BOOL seekable = FALSE;

            stream = 0;
            size = sizeof(seekable);
            hr = IWMHeaderInfo_GetAttributeByName( header, &stream, L"Seekable", &type,
                                                   (BYTE *)&seekable, &size );
            out_hr( "hr", hr );
            out( " type=" );
            out_dec( type );
            out( " seekable=" );
            out_dec( seekable ? 1 : 0 );
            verdict( hr == S_OK && type == WMT_TYPE_BOOL && seekable,
                     "a local file is not seekable" );
        }

        begin( "IWMHeaderInfo::GetAttributeByName(an attribute that does not exist)" );
        {
            BYTE buf[8];

            stream = 0;
            size = sizeof(buf);
            hr = IWMHeaderInfo_GetAttributeByName( header, &stream, L"NoSuchAttribute",
                                                   &type, buf, &size );
            out_hr( "hr", hr );
            verdict( hr == ASF_E_NOTFOUND, "not Wine's own ASF_E_NOTFOUND" );
        }

        IWMHeaderInfo_Release( header );
        header = NULL;
    }

    begin( "IWMSyncReader::Close" );
    hr = IWMSyncReader_Close( reader );
    out_hr( "hr", hr );
    verdict( hr == S_OK, "not S_OK" );

    begin( "IWMSyncReader::Release" );
    n = IWMSyncReader_Release( reader );
    out( "refs=" );
    out_dec( n );
    verdict( n == 0, "last reference did not drop to zero" );
}

/* -------------------------------------------------------------------- evr */

static void lane_evr( void )
{
    fn_MFCreateVideoMediaTypeFromSubtype create_video_type;
    fn_MFCreateVideoSampleFromSurface create_from_surface;
    fn_MFCreateVideoSampleAllocator create_allocator;
    IMFVideoSampleAllocator *allocator = NULL;
    IMFSample *s1 = NULL, *s2 = NULL, *s3 = NULL;
    IMFVideoMediaType *video_type = NULL;
    IMFMediaBuffer *buffer = NULL;
    IMF2DBuffer *buffer2d = NULL;
    IMFMediaType *type = NULL;
    ULONG want_len, want_pitch;
    ULONG width, height;
    DWORD len = 0;
    LONG pitch = 0;
    BYTE *scan0;
    HRESULT hr;
    GUID sub;
    DWORD n;

    width      = (ULONG)env_dec( L"MF_MODULES_WIDTH", 0 );
    height     = (ULONG)env_dec( L"MF_MODULES_HEIGHT", 0 );
    want_len   = (ULONG)env_dec( L"MF_MODULES_2D_LENGTH", 0 );
    want_pitch = (ULONG)env_dec( L"MF_MODULES_2D_PITCH", 0 );

    begin( "GetProcAddress evr.dll!MFCreateVideoSampleAllocator" );
    create_allocator =
        (fn_MFCreateVideoSampleAllocator)get_entry( L"evr.dll", "MFCreateVideoSampleAllocator" );
    verdict( create_allocator != NULL, "the module did not load or has no such export" );
    if (!create_allocator) return;

    begin( "MFCreateVideoSampleAllocator(IID_IMFVideoSampleAllocator)" );
    hr = create_allocator( &IID_IMFVideoSampleAllocator, (void **)&allocator );
    out_hr( "hr", hr );
    verdict( hr == S_OK && allocator != NULL, "no allocator" );
    if (!allocator) return;

    begin( "IMFVideoSampleAllocator::AllocateSample before initialisation" );
    hr = IMFVideoSampleAllocator_AllocateSample( allocator, &s1 );
    out_hr( "hr", hr );
    verdict( hr == MF_E_NOT_INITIALIZED, "not MF_E_NOT_INITIALIZED" );

    begin( "MFCreateMediaType + RGB32 video type" );
    hr = MFCreateMediaType( &type );
    if (SUCCEEDED(hr))
        hr = IMFMediaType_SetGUID( type, &MF_MT_MAJOR_TYPE, &MFMediaType_Video );
    if (SUCCEEDED(hr))
        hr = IMFMediaType_SetGUID( type, &MF_MT_SUBTYPE, &MFVideoFormat_RGB32 );
    if (SUCCEEDED(hr))
        hr = IMFMediaType_SetUINT64( type, &MF_MT_FRAME_SIZE,
                                     ((UINT64)width << 32) | height );
    out_hr( "hr", hr );
    verdict( hr == S_OK && type != NULL, "could not describe the frame" );
    if (!type) return;

    /* Two samples requested, so the third request must be refused: the count
     * is enforced by dlls/evr/sample.c's free list and is checkable without
     * any DirectX at all, because with no device manager the buffers come
     * from MFCreate2DMediaBuffer. */
    begin( "IMFVideoSampleAllocator::InitializeSampleAllocator(2)" );
    hr = IMFVideoSampleAllocator_InitializeSampleAllocator( allocator, 2, type );
    out_hr( "hr", hr );
    verdict( hr == S_OK, "the allocator refused a plain system-memory RGB32 type" );

    begin( "IMFVideoSampleAllocator::AllocateSample x2" );
    hr = IMFVideoSampleAllocator_AllocateSample( allocator, &s1 );
    if (SUCCEEDED(hr)) hr = IMFVideoSampleAllocator_AllocateSample( allocator, &s2 );
    out_hr( "hr", hr );
    verdict( hr == S_OK && s1 && s2 && s1 != s2, "did not get two distinct samples" );

    begin( "IMFVideoSampleAllocator::AllocateSample past the pool" );
    hr = IMFVideoSampleAllocator_AllocateSample( allocator, &s3 );
    out_hr( "hr", hr );
    verdict( hr == MF_E_SAMPLEALLOCATOR_EMPTY, "not MF_E_SAMPLEALLOCATOR_EMPTY" );

    if (s1)
    {
        begin( "IMFSample::GetBufferCount" );
        n = 0;
        hr = IMFSample_GetBufferCount( s1, &n );
        out_hr( "hr", hr );
        out( " n=" );
        out_dec( n );
        verdict( hr == S_OK && n == 1, "an allocated sample does not carry one buffer" );

        begin( "IMFSample::GetBufferByIndex(0)" );
        hr = IMFSample_GetBufferByIndex( s1, 0, &buffer );
        out_hr( "hr", hr );
        verdict( hr == S_OK && buffer != NULL, "no buffer" );
    }

    if (buffer)
    {
        begin( "IMFMediaBuffer::QueryInterface(IID_IMF2DBuffer)" );
        hr = IMFMediaBuffer_QueryInterface( buffer, &IID_IMF2DBuffer, (void **)&buffer2d );
        out_hr( "hr", hr );
        verdict( hr == S_OK && buffer2d != NULL, "the allocator's buffer is not 2D" );
    }

    if (buffer2d)
    {
        begin( "IMF2DBuffer::GetContiguousLength" );
        hr = IMF2DBuffer_GetContiguousLength( buffer2d, &len );
        out_hr( "hr", hr );
        out( " len=" );
        out_dec( len );
        verdict( hr == S_OK && len == want_len,
                 "not width*height*4, the length the gate computed" );

        begin( "IMF2DBuffer::Lock2D pitch" );
        hr = IMF2DBuffer_Lock2D( buffer2d, &scan0, &pitch );
        out_hr( "hr", hr );
        out( " pitch=" );
        out_dec( (ULONG)(pitch < 0 ? -pitch : pitch) );
        verdict( hr == S_OK && (ULONG)(pitch < 0 ? -pitch : pitch) == want_pitch,
                 "not the aligned stride the gate computed" );
        if (SUCCEEDED(hr)) IMF2DBuffer_Unlock2D( buffer2d );

        IMF2DBuffer_Release( buffer2d );
        buffer2d = NULL;
    }
    if (buffer) IMFMediaBuffer_Release( buffer );
    if (s1) IMFSample_Release( s1 );
    if (s2) IMFSample_Release( s2 );

    begin( "IMFVideoSampleAllocator::UninitializeSampleAllocator" );
    hr = IMFVideoSampleAllocator_UninitializeSampleAllocator( allocator );
    out_hr( "hr", hr );
    verdict( hr == S_OK, "not S_OK" );

    /* The pool is gone, so the allocator is back to the state it had before
     * InitializeSampleAllocator and must say so with the same HRESULT it used
     * then.  This is here INSTEAD of a refcount check on the allocator, and
     * the reason is worth recording: releasing a sample does not return it to
     * the free list synchronously.  IMFTrackedSample::SetAllocator queues the
     * notification, so dlls/evr/sample.c's tracking callback runs on an MF
     * worker thread and the work item holds a reference on the allocator (the
     * callback IS the allocator) until it does.  The allocator's refcount at
     * this instant is therefore a scheduling artifact -- measured as 2 on one
     * run -- and asserting on it would be asserting on thread timing, which is
     * the one thing a gate demanding byte-identical transcripts must not do.
     * This check is deterministic and tests the same object. */
    begin( "IMFVideoSampleAllocator::AllocateSample after uninitialisation" );
    hr = IMFVideoSampleAllocator_AllocateSample( allocator, &s3 );
    out_hr( "hr", hr );
    verdict( hr == MF_E_NOT_INITIALIZED, "not MF_E_NOT_INITIALIZED again" );

    IMFVideoSampleAllocator_Release( allocator );
    IMFMediaType_Release( type );

    /* MFCreateVideoSampleFromSurface(NULL) is the "give me a sample with no
     * buffer" form, and it is the argument shape this surface can serve of
     * the three evr exports that take a Direct3D object.  A NON-NULL surface
     * must be refused by name -- dlls/evr/mfcom.c's evr_refuse_d3d_owner --
     * and that refusal is checked below rather than assumed. */
    begin( "GetProcAddress evr.dll!MFCreateVideoSampleFromSurface" );
    create_from_surface =
        (fn_MFCreateVideoSampleFromSurface)get_entry( L"evr.dll",
                                                      "MFCreateVideoSampleFromSurface" );
    verdict( create_from_surface != NULL, "the module did not load or has no such export" );

    if (create_from_surface)
    {
        IMFSample *sample = NULL;
        LONGLONG t = 0;

        begin( "MFCreateVideoSampleFromSurface(NULL)" );
        hr = create_from_surface( NULL, &sample );
        out_hr( "hr", hr );
        verdict( hr == S_OK && sample != NULL, "no sample" );

        if (sample)
        {
            begin( "IMFSample::SetSampleTime/GetSampleTime round trip" );
            hr = IMFSample_SetSampleTime( sample, 1234567 );
            if (SUCCEEDED(hr)) hr = IMFSample_GetSampleTime( sample, &t );
            out_hr( "hr", hr );
            out( " t=" );
            out_dec64( (ULONGLONG)t );
            verdict( hr == S_OK && t == 1234567, "the timestamp did not round trip" );

            begin( "IMFSample::GetBufferCount on a surface-less video sample" );
            n = 0xdeadbeef;
            hr = IMFSample_GetBufferCount( sample, &n );
            out_hr( "hr", hr );
            out( " n=" );
            out_dec( n );
            verdict( hr == S_OK && n == 0, "a sample with no surface carries a buffer" );

            IMFSample_Release( sample );
        }
    }

    /* evr re-exports three of mfplat's entry points with -import, and both
     * names must resolve to mfplat's ONE wrapper (dlls/evr/evr.spec forwards
     * them).  This is the check that the forward actually arrives somewhere
     * that wraps: an unwrapped return here would be a native vtable in a
     * guest's hands. */
    begin( "GetProcAddress evr.dll!MFCreateVideoMediaTypeFromSubtype" );
    create_video_type =
        (fn_MFCreateVideoMediaTypeFromSubtype)get_entry( L"evr.dll",
                                                         "MFCreateVideoMediaTypeFromSubtype" );
    verdict( create_video_type != NULL, "the module did not load or has no such export" );

    if (create_video_type)
    {
        begin( "MFCreateVideoMediaTypeFromSubtype(RGB32) through evr's forward" );
        hr = create_video_type( &MFVideoFormat_RGB32, &video_type );
        out_hr( "hr", hr );
        verdict( hr == S_OK && video_type != NULL, "no video media type" );
    }

    if (video_type)
    {
        IMFMediaType *as_type = NULL;

        begin( "IMFVideoMediaType::QueryInterface(IID_IMFMediaType)" );
        hr = IMFVideoMediaType_QueryInterface( video_type, &IID_IMFMediaType,
                                               (void **)&as_type );
        out_hr( "hr", hr );
        verdict( hr == S_OK && as_type != NULL, "not also an IMFMediaType" );

        if (as_type)
        {
            begin( "IMFMediaType::GetGUID(MF_MT_SUBTYPE)" );
            hr = IMFMediaType_GetGUID( as_type, &MF_MT_SUBTYPE, &sub );
            out_hr( "hr", hr );
            verdict( hr == S_OK && guid_eq( &sub, &MFVideoFormat_RGB32 ),
                     "the subtype came back as something else" );
            IMFMediaType_Release( as_type );
        }
        IMFVideoMediaType_Release( video_type );
    }
}

/* ------------------------------------------------------------- the run */

static int mf_modules_run( void )
{
    WCHAR url[MAX_PATH], path[MAX_PATH];
    HRESULT hr;
    DWORD n;

    out( "mf_modules: start\n" );

    main_tid = GetCurrentThreadId();
    the_notify.IMFMediaEngineNotify_iface.lpVtbl = &notify_vtbl;
    the_notify.refs = 0;

    n = GetEnvironmentVariableW( L"MF_MODULES_URL", url, MAX_PATH );
    if (!n || n >= MAX_PATH)
    {
        out( "mf_modules: FAIL (MF_MODULES_URL is not set)\n" );
        return 1;
    }
    n = GetEnvironmentVariableW( L"MF_MODULES_PATH", path, MAX_PATH );
    if (!n || n >= MAX_PATH)
    {
        out( "mf_modules: FAIL (MF_MODULES_PATH is not set)\n" );
        return 1;
    }

    if (!(notify_event = CreateEventW( NULL, FALSE, FALSE, NULL )))
    {
        out( "mf_modules: FAIL (no event)\n" );
        return 1;
    }

    begin( "MFStartup" );
    hr = MFStartup( MF_VERSION, MFSTARTUP_FULL );
    out_hr( "hr", hr );
    verdict( hr == S_OK, "not S_OK" );
    if (FAILED(hr)) goto done;

    lane_mediaengine( url );
    lane_wmvcore( path );
    lane_evr();

    begin( "MFShutdown" );
    hr = MFShutdown();
    out_hr( "hr", hr );
    verdict( hr == S_OK, "not S_OK" );

done:
    CloseHandle( notify_event );
    out( failures ? "mf_modules: FAIL " : "mf_modules: PASS " );
    out_dec( (ULONG)(step - failures) );
    out( "/" );
    out_dec( (ULONG)step );
    out( "\n" );
    return failures ? 1 : 0;
}

#ifdef MF_MODULES_NO_CRT
/* The guest build has no C runtime: this IS the image entry point. */
void WINAPI mf_modules_entry( void )
{
    ExitProcess( (UINT)mf_modules_run() );
}
#else
int main( void )
{
    return mf_modules_run();
}
#endif
