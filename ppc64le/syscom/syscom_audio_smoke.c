/*
 * syscom_audio_smoke -- the CoCreateInstance AUDIO gate for the system-COM
 * surface.
 *
 * ONE source, built TWICE and run twice under the same wine: as a native ppc64
 * Windows PE and as an x86-64 guest PE.  The two runs must print
 * BYTE-IDENTICAL stdout.  Read com_smoke.c first -- this is the same claim
 * (everything printed is a value Wine's own implementation computed, so
 * agreement means the guest reached that implementation through the winecom
 * proxy runtime with nothing lost on the way) about the two coclasses a game
 * asks combase for when it wants to make a sound.
 *
 * WHY THIS IS A SEPARATE GATE FROM ppc64le/audio/check-audio-smoke.sh.  That
 * one exercises the DIRECT path: XAudio2Create() and DirectSoundCreate() are
 * flat exports of guest thunk modules with their own winecom instances and
 * their own rosters.  Nothing here calls a flat export at all.  Every object
 * below arrives through CoCreateInstance, is wrapped by COMBASE's instance from
 * ppc64le/syscom/interfaces_syscom.json, and traps into combase's
 * __wine_com_dispatch.  The two lanes share libs/winecom and nothing else.
 *
 * WHAT MAKES EACH STEP A CHECK RATHER THAN A SMOKE TEST
 *
 *   * THE VERSION IS THE POINT.  CLSID_XAudio2 {5a508685-...} is XAudio2 2.7,
 *     and IID_IXAudio2 {8bcf1f58-...} is the IID of EVERY version up to 2.7 --
 *     so the IID cannot say which vtable arrives and only the CLSID can.  This
 *     file declares the 2.7 vtable itself rather than including <xaudio2.h>,
 *     which the tree generates once at XAUDIO2_VER=9: at 2.9 there is no
 *     GetDeviceCount, no GetDeviceDetails and no Initialize, so the three slots
 *     steps 3 and 4 call would be RegisterForCallbacks, UnregisterForCallbacks
 *     and CreateSourceVoice.  Getting the roster's version wrong is not a
 *     compile error anywhere; it is these two steps returning nonsense.
 *
 *   * A VOICE IS NOT A COM OBJECT.  IXAudio2Voice and its derivatives are
 *     `[local]` in include/xaudio2.idl: no QueryInterface, no AddRef, no
 *     Release, no IID.  Slot 0 of a mastering voice is GetVoiceDetails, and
 *     libs/winecom's dispatcher serves slots 0..2 of every interface as
 *     IUnknown -- so dlls/combase/syscom.c CLAIMS these interfaces and serves
 *     them itself.  STEPS 6 AND 9 ARE THAT CLAIM: without it GetVoiceDetails
 *     would be answered by QueryInterface and the details struct would come
 *     back untouched.
 *
 *   * FLOATS.  MaxFrequencyRatio is CreateSourceVoice's FOURTH argument, so
 *     MS-x64 puts it on the guest's STACK rather than in an XMM register (step
 *     8), and SetVolume/SetFrequencyRatio take a float in XMM1 (steps 7 and
 *     10).  Every constant here is a dyadic fraction and every comparison is on
 *     the RAW BITS, because a float that crossed through an integer register is
 *     a WRONG NUMBER and not a crash.
 *
 *   * THE WASAPI CHAIN IS WALKED, not merely opened: enumerator -> default
 *     endpoint -> device state -> Activate an IAudioClient -> mix format ->
 *     Initialize -> buffer size -> GetService an IAudioRenderClient -> get a
 *     real buffer, write it, release it.  Every hop but the first is an
 *     interface pointer produced INSIDE the dispatch loop, by three different
 *     mechanisms: a statically typed out-parameter (GetDefaultAudioEndpoint),
 *     a riid/void** pair inside a hand-written slot (Activate), and a
 *     riid/void** pair in the generic loop (GetService).
 *
 * OBSERVATIONS THAT ARE TRUE OF THE PORT AND NOT OF THE API go to stderr as
 * `note:` lines and never enter the diffed transcript, because their answer
 * legitimately differs between the legs.  All four are the REVERSE-PROXY
 * direction -- a guest-implemented COM object, or a struct carrying one, passed
 * INTO native code -- which this port does not do yet:
 *
 *   IXAudio2::RegisterForCallbacks, IXAudio2::CreateSourceVoice with a
 *   non-NULL pCallback, IMMDeviceEnumerator::UnregisterEndpointNotification-
 *   Callback, and IMMDevice::OpenPropertyStore (whose IPropertyStore this
 *   roster does not carry, so there is no guest vtable to hand back).
 *
 * All four must answer E_NOTIMPL on the guest leg -- check-syscom-audio.sh
 * requires it -- and the first, second and fourth succeed on the native leg,
 * which is exactly why they cannot be diffed steps.
 *
 * A FIFTH note is the OTHER kind of refusal, and it is here because it is the
 * exact line a guest used to get for XAudio2 itself: IMMDevice::Activate for
 * IID_IAudioClient2, which Wine implements and this roster does not carry.
 * The guest must be answered E_NOINTERFACE by winecom_wrap_out_iface -- the
 * fail-closed choke point every riid-typed out-parameter passes through -- with
 * the IID named in the log, and the object released rather than handed over.
 *
 * SC_AUDIO_BREAK (falsification; the gate builds each variant of the NATIVE leg
 * and requires it to FAIL):
 *
 *   =1  expect the wrong mastering-voice channel count, so step 6's value check
 *       is shown to be a check rather than a print.
 *   =2  expect the wrong SetVolume read-back bits, so the float slot's check is
 *       shown to be a check.
 *   =3  expect the wrong default-endpoint device state.
 *
 * NO C RUNTIME on the guest leg (-DSC_AUDIO_NO_CRT): the program formats its
 * own output and writes it with WriteFile.
 *
 * Copyright 2026 the ppc64le port authors
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 */

#ifndef SC_AUDIO_BREAK
#define SC_AUDIO_BREAK 0
#endif

#define COBJMACROS

#include <windows.h>
#include <objbase.h>
#include <mmdeviceapi.h>
#include <audioclient.h>

/* ------------------------------------------------------------------ GUIDs
 *
 * Spelled out here rather than linked from libuuid: the guest build has no
 * Wine import libraries at all, and a GUID both builds compile from the same
 * bytes cannot differ between them. */

/* coclass XAudio2 at XAUDIO2_VER == 7 (include/xaudio2.idl) */
static const GUID sc_CLSID_XAudio2 =
    { 0x5a508685, 0xa254, 0x4fba, { 0x9b,0x82,0x9a,0x24,0xb0,0x03,0x06,0xaf } };
/* IID_IXAudio2 -- the SAME IID for every version up to 2.7 */
static const GUID sc_IID_IXAudio2 =
    { 0x8bcf1f58, 0x9fe7, 0x4583, { 0x8a,0xc6,0xe2,0xad,0xc4,0x65,0xc8,0xbb } };
static const GUID sc_CLSID_MMDeviceEnumerator =
    { 0xbcde0395, 0xe52f, 0x467c, { 0x8e,0x3d,0xc4,0x57,0x92,0x91,0x69,0x2e } };
static const GUID sc_IID_IMMDeviceEnumerator =
    { 0xa95664d2, 0x9614, 0x4f35, { 0xa7,0x46,0xde,0x8d,0xb6,0x36,0x17,0xe6 } };
static const GUID sc_IID_IAudioClient =
    { 0x1cb9ad4c, 0xdbfa, 0x4c32, { 0xb1,0x78,0xc2,0xf5,0x68,0xa7,0x03,0xb2 } };
static const GUID sc_IID_IAudioRenderClient =
    { 0xf294acfc, 0x3146, 0x4483, { 0xa7,0xbf,0xad,0xdc,0xa7,0xc2,0x60,0xe2 } };
/* DELIBERATELY NOT IN THE ROSTER.  Wine's mmdevapi implements IAudioClient2,
 * so the native leg gets one; the guest leg must be REFUSED, because an
 * interface with no guest stub vtable handed over is a native vtable the guest
 * would call as x86-64.  This is the one note whose refusal is E_NOINTERFACE
 * rather than E_NOTIMPL: it comes from winecom_wrap_out_iface, the fail-closed
 * choke point every riid-typed out-parameter passes through, and it is the
 * same line -- with a different GUID -- that a guest asking combase for an
 * unrostered IXAudio2 used to get. */
static const GUID sc_IID_IAudioClient2 =
    { 0x726778cd, 0xf60a, 0x4eda, { 0x82,0xde,0xe4,0x76,0x10,0xcd,0x78,0xaa } };

/* ------------------------------------------- XAudio2 2.7, declared in full
 *
 * NOT <xaudio2.h>: that header is widl output at XAUDIO2_VER=9 for the whole
 * tree, and this program deliberately speaks the shape a 2.7 application's own
 * headers state.  xaudio2.idl packs its structs to 1; every member here is a
 * UINT32, so the pragma changes nothing, and it is written anyway so that the
 * declaration says what the IDL says. */

#pragma pack(push,1)
typedef struct
{
    UINT32 CreationFlags;         /* 2.8 adds ActiveFlags here; 2.7 does not */
    UINT32 InputChannels;
    UINT32 InputSampleRate;
} SC_XAUDIO2_VOICE_DETAILS;
#pragma pack(pop)

typedef struct SC_IXAudio2 SC_IXAudio2;
typedef struct SC_IXAudio2Voice SC_IXAudio2Voice;
typedef struct SC_IXAudio2SourceVoice SC_IXAudio2SourceVoice;
typedef struct SC_IXAudio2MasteringVoice SC_IXAudio2MasteringVoice;

/* The [local] voice vtable at XAUDIO2_VER >= 4.  Only what is called below is
 * given a real prototype; the rest are placeholders that hold their SLOT, which
 * is the part that matters -- a vtable declared short would call the wrong
 * method with no diagnostic anywhere. */
#define SC_VOICE_VTBL(iface)                                                  \
    void    (STDMETHODCALLTYPE *GetVoiceDetails)( iface *, SC_XAUDIO2_VOICE_DETAILS * ); \
    HRESULT (STDMETHODCALLTYPE *SetOutputVoices)( iface *, const void * );     \
    HRESULT (STDMETHODCALLTYPE *SetEffectChain)( iface *, const void * );      \
    HRESULT (STDMETHODCALLTYPE *EnableEffect)( iface *, UINT32, UINT32 );      \
    HRESULT (STDMETHODCALLTYPE *DisableEffect)( iface *, UINT32, UINT32 );     \
    void    (STDMETHODCALLTYPE *GetEffectState)( iface *, UINT32, BOOL * );    \
    HRESULT (STDMETHODCALLTYPE *SetEffectParameters)( iface *, UINT32, const void *, UINT32, UINT32 ); \
    HRESULT (STDMETHODCALLTYPE *GetEffectParameters)( iface *, UINT32, void *, UINT32 ); \
    HRESULT (STDMETHODCALLTYPE *SetFilterParameters)( iface *, const void *, UINT32 ); \
    void    (STDMETHODCALLTYPE *GetFilterParameters)( iface *, void * );       \
    HRESULT (STDMETHODCALLTYPE *SetOutputFilterParameters)( iface *, SC_IXAudio2Voice *, const void *, UINT32 ); \
    void    (STDMETHODCALLTYPE *GetOutputFilterParameters)( iface *, SC_IXAudio2Voice *, void * ); \
    HRESULT (STDMETHODCALLTYPE *SetVolume)( iface *, float, UINT32 );          \
    void    (STDMETHODCALLTYPE *GetVolume)( iface *, float * );                \
    HRESULT (STDMETHODCALLTYPE *SetChannelVolumes)( iface *, UINT32, const float *, UINT32 ); \
    void    (STDMETHODCALLTYPE *GetChannelVolumes)( iface *, UINT32, float * ); \
    HRESULT (STDMETHODCALLTYPE *SetOutputMatrix)( iface *, SC_IXAudio2Voice *, UINT32, UINT32, const float *, UINT32 ); \
    void    (STDMETHODCALLTYPE *GetOutputMatrix)( iface *, SC_IXAudio2Voice *, UINT32, UINT32, float * ); \
    void    (STDMETHODCALLTYPE *DestroyVoice)( iface * )

typedef struct { SC_VOICE_VTBL(SC_IXAudio2Voice); } SC_IXAudio2VoiceVtbl;
struct SC_IXAudio2Voice { const SC_IXAudio2VoiceVtbl *lpVtbl; };

typedef struct { SC_VOICE_VTBL(SC_IXAudio2MasteringVoice); }
    SC_IXAudio2MasteringVoiceVtbl;
struct SC_IXAudio2MasteringVoice { const SC_IXAudio2MasteringVoiceVtbl *lpVtbl; };

typedef struct
{
    SC_VOICE_VTBL(SC_IXAudio2SourceVoice);
    HRESULT (STDMETHODCALLTYPE *Start)( SC_IXAudio2SourceVoice *, UINT32, UINT32 );
    HRESULT (STDMETHODCALLTYPE *Stop)( SC_IXAudio2SourceVoice *, UINT32, UINT32 );
    HRESULT (STDMETHODCALLTYPE *SubmitSourceBuffer)( SC_IXAudio2SourceVoice *, const void *, const void * );
    HRESULT (STDMETHODCALLTYPE *FlushSourceBuffers)( SC_IXAudio2SourceVoice * );
    HRESULT (STDMETHODCALLTYPE *Discontinuity)( SC_IXAudio2SourceVoice * );
    HRESULT (STDMETHODCALLTYPE *ExitLoop)( SC_IXAudio2SourceVoice *, UINT32 );
    void    (STDMETHODCALLTYPE *GetState)( SC_IXAudio2SourceVoice *, void * );
    HRESULT (STDMETHODCALLTYPE *SetFrequencyRatio)( SC_IXAudio2SourceVoice *, float, UINT32 );
    void    (STDMETHODCALLTYPE *GetFrequencyRatio)( SC_IXAudio2SourceVoice *, float * );
    HRESULT (STDMETHODCALLTYPE *SetSourceSampleRate)( SC_IXAudio2SourceVoice *, UINT32 );
} SC_IXAudio2SourceVoiceVtbl;
struct SC_IXAudio2SourceVoice { const SC_IXAudio2SourceVoiceVtbl *lpVtbl; };

typedef struct
{
    HRESULT (STDMETHODCALLTYPE *QueryInterface)( SC_IXAudio2 *, REFIID, void ** );
    ULONG   (STDMETHODCALLTYPE *AddRef)( SC_IXAudio2 * );
    ULONG   (STDMETHODCALLTYPE *Release)( SC_IXAudio2 * );
    /* the three slots that exist only at XAUDIO2_VER <= 7 */
    HRESULT (STDMETHODCALLTYPE *GetDeviceCount)( SC_IXAudio2 *, UINT32 * );
    HRESULT (STDMETHODCALLTYPE *GetDeviceDetails)( SC_IXAudio2 *, UINT32, void * );
    HRESULT (STDMETHODCALLTYPE *Initialize)( SC_IXAudio2 *, UINT32, UINT32 );
    HRESULT (STDMETHODCALLTYPE *RegisterForCallbacks)( SC_IXAudio2 *, void * );
    void    (STDMETHODCALLTYPE *UnregisterForCallbacks)( SC_IXAudio2 *, void * );
    HRESULT (STDMETHODCALLTYPE *CreateSourceVoice)( SC_IXAudio2 *, SC_IXAudio2SourceVoice **,
                                                    const WAVEFORMATEX *, UINT32, float,
                                                    void *, const void *, const void * );
    HRESULT (STDMETHODCALLTYPE *CreateSubmixVoice)( SC_IXAudio2 *, void **, UINT32, UINT32,
                                                    UINT32, UINT32, const void *, const void * );
    /* 2.7: a device INDEX, and no AUDIO_STREAM_CATEGORY */
    HRESULT (STDMETHODCALLTYPE *CreateMasteringVoice)( SC_IXAudio2 *, SC_IXAudio2MasteringVoice **,
                                                       UINT32, UINT32, UINT32, UINT32,
                                                       const void * );
    HRESULT (STDMETHODCALLTYPE *StartEngine)( SC_IXAudio2 * );
    void    (STDMETHODCALLTYPE *StopEngine)( SC_IXAudio2 * );
    HRESULT (STDMETHODCALLTYPE *CommitChanges)( SC_IXAudio2 *, UINT32 );
    void    (STDMETHODCALLTYPE *GetPerformanceData)( SC_IXAudio2 *, void * );
    void    (STDMETHODCALLTYPE *SetDebugConfiguration)( SC_IXAudio2 *, const void *, void * );
} SC_IXAudio2Vtbl;
struct SC_IXAudio2 { const SC_IXAudio2Vtbl *lpVtbl; };

/* ------------------------------- callbacks the PROGRAM implements
 *
 * REAL objects with real no-op methods, not a stand-in pointer.  On the native
 * leg XAudio2 keeps them and calls them from its own mixer thread, so a
 * plausible-looking address would not do: a source voice created with a
 * callback has its OnVoiceProcessingPassStart called within a mixer quantum,
 * and that is what makes the native half of the refusal check real.  On the
 * guest leg these are GUEST-implemented COM objects handed to native code --
 * the reverse-proxy direction the port refuses -- which is the whole point. */

typedef struct SC_IXAudio2VoiceCallback SC_IXAudio2VoiceCallback;
typedef struct
{
    void (STDMETHODCALLTYPE *OnVoiceProcessingPassStart)( SC_IXAudio2VoiceCallback *, UINT32 );
    void (STDMETHODCALLTYPE *OnVoiceProcessingPassEnd)( SC_IXAudio2VoiceCallback * );
    void (STDMETHODCALLTYPE *OnStreamEnd)( SC_IXAudio2VoiceCallback * );
    void (STDMETHODCALLTYPE *OnBufferStart)( SC_IXAudio2VoiceCallback *, void * );
    void (STDMETHODCALLTYPE *OnBufferEnd)( SC_IXAudio2VoiceCallback *, void * );
    void (STDMETHODCALLTYPE *OnLoopEnd)( SC_IXAudio2VoiceCallback *, void * );
    void (STDMETHODCALLTYPE *OnVoiceError)( SC_IXAudio2VoiceCallback *, void *, HRESULT );
} SC_IXAudio2VoiceCallbackVtbl;
struct SC_IXAudio2VoiceCallback { const SC_IXAudio2VoiceCallbackVtbl *lpVtbl; };

typedef struct SC_IXAudio2EngineCallback SC_IXAudio2EngineCallback;
typedef struct
{
    void (STDMETHODCALLTYPE *OnProcessingPassStart)( SC_IXAudio2EngineCallback * );
    void (STDMETHODCALLTYPE *OnProcessingPassEnd)( SC_IXAudio2EngineCallback * );
    void (STDMETHODCALLTYPE *OnCriticalError)( SC_IXAudio2EngineCallback *, HRESULT );
} SC_IXAudio2EngineCallbackVtbl;
struct SC_IXAudio2EngineCallback { const SC_IXAudio2EngineCallbackVtbl *lpVtbl; };

static void STDMETHODCALLTYPE cb_pass_start_u( SC_IXAudio2VoiceCallback *i, UINT32 n ) { (void)i; (void)n; }
static void STDMETHODCALLTYPE cb_void_v( SC_IXAudio2VoiceCallback *i ) { (void)i; }
static void STDMETHODCALLTYPE cb_ptr_v( SC_IXAudio2VoiceCallback *i, void *p ) { (void)i; (void)p; }
static void STDMETHODCALLTYPE cb_err_v( SC_IXAudio2VoiceCallback *i, void *p, HRESULT hr ) { (void)i; (void)p; (void)hr; }

static const SC_IXAudio2VoiceCallbackVtbl sc_voice_cb_vtbl =
{
    cb_pass_start_u, cb_void_v, cb_void_v,
    cb_ptr_v, cb_ptr_v, cb_ptr_v, cb_err_v
};
static SC_IXAudio2VoiceCallback sc_voice_cb = { &sc_voice_cb_vtbl };

static void STDMETHODCALLTYPE cb_void_e( SC_IXAudio2EngineCallback *i ) { (void)i; }
static void STDMETHODCALLTYPE cb_err_e( SC_IXAudio2EngineCallback *i, HRESULT hr ) { (void)i; (void)hr; }

static const SC_IXAudio2EngineCallbackVtbl sc_engine_cb_vtbl =
{
    cb_void_e, cb_void_e, cb_err_e
};
static SC_IXAudio2EngineCallback sc_engine_cb = { &sc_engine_cb_vtbl };

#define SC_XAUDIO2_ANY_PROCESSOR   0xffffffffu
#define SC_XAUDIO2_COMMIT_NOW      0
#define SC_XAUDIO2_DEFAULT_CHANNELS 0
#define SC_XAUDIO2_DEFAULT_SAMPLERATE 0

/* ------------------------------------------------------------- output */

static void out( const char *s )
{
    DWORD n = 0, written;

    while (s[n]) n++;
    WriteFile( GetStdHandle( STD_OUTPUT_HANDLE ), s, n, &written, NULL );
}

static void errout( const char *s )
{
    DWORD n = 0, written;

    while (s[n]) n++;
    WriteFile( GetStdHandle( STD_ERROR_HANDLE ), s, n, &written, NULL );
}

static void fmt_hex( char *buf, ULONGLONG v, int digits )
{
    static const char hex[] = "0123456789ABCDEF";
    int i;

    for (i = 0; i < digits; i++) buf[digits - 1 - i] = hex[(v >> (4 * i)) & 0xf];
    buf[digits] = 0;
}

static void out_hex( ULONGLONG v, int digits )
{
    char buf[17];

    fmt_hex( buf, v, digits );
    out( buf );
}

static void out_dec( ULONGLONG v )
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

/* stderr, never diffed: an observation about the PORT whose answer is allowed
 * to differ between the two legs. */
static void note( const char *what, HRESULT hr )
{
    char buf[9];

    errout( what );
    errout( " 0x" );
    fmt_hex( buf, (ULONG)hr, 8 );
    errout( buf );
    errout( "\n" );
}

/* The raw bits of a float.  A number that crossed the boundary through an
 * integer register is WRONG rather than absent, so nothing here compares
 * floats as floats. */
static ULONG fbits( float f )
{
    union { float f; ULONG u; } c;

    c.f = f;
    return c.u;
}

/* ------------------------------------------------------------- the run */

static int failures;
static int step;
static const char *first_fail;

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
        if (!first_fail) first_fail = why;
        out( " FAIL (" );
        out( why );
        out( ")\n" );
    }
}

static int sc_audio_run( void )
{
    SC_IXAudio2MasteringVoice *master = NULL;
    SC_IXAudio2SourceVoice *src = NULL;
    IMMDeviceEnumerator *devenum = NULL;
    IMMDeviceCollection *coll = NULL;
    IAudioRenderClient *render = NULL;
    IAudioClient *client = NULL;
    IMMDevice *dev = NULL, *dev2 = NULL;
    IPropertyStore *pstore = NULL;
    SC_IXAudio2 *xa2 = NULL;
    SC_XAUDIO2_VOICE_DETAILS details;
    WAVEFORMATEX *mix = NULL;
    WAVEFORMATEX wfx;
    UINT32 count = 0, frames = 0, ncoll = 0;
    DWORD state = 0;
    float v = 0.0f;
    HRESULT hr;

    out( "syscom_audio_smoke: start\n" );

    begin( "CoInitializeEx(APARTMENTTHREADED)" );
    hr = CoInitializeEx( NULL, COINIT_APARTMENTTHREADED );
    out_hr( "hr", hr );
    verdict( hr == S_OK, "not S_OK" );
    if (hr != S_OK) goto done;

    /* ================================================ XAudio2 2.7 ======== */

    begin( "CoCreateInstance(CLSID_XAudio2 2.7, IID_IXAudio2)" );
    hr = CoCreateInstance( &sc_CLSID_XAudio2, NULL, CLSCTX_INPROC_SERVER,
                           &sc_IID_IXAudio2, (void **)&xa2 );
    out_hr( "hr", hr );
    verdict( hr == S_OK && xa2 != NULL, "no IXAudio2" );
    if (hr != S_OK || !xa2) goto uninit;

    /* Slot 5.  At 2.9 this slot is CreateSourceVoice; a roster built from the
     * wrong widl run passes every other step and fails here. */
    begin( "IXAudio2::Initialize (slot 5, a 2.7-only method)" );
    hr = xa2->lpVtbl->Initialize( xa2, 0, SC_XAUDIO2_ANY_PROCESSOR );
    out_hr( "hr", hr );
    verdict( SUCCEEDED(hr), "Initialize was refused" );

    /* Slot 3, likewise 2.7-only, and the answer is a COUNTED value. */
    begin( "IXAudio2::GetDeviceCount (slot 3, a 2.7-only method)" );
    hr = xa2->lpVtbl->GetDeviceCount( xa2, &count );
    out_hr( "hr", hr );
    /* The COUNT itself is not printed: this machine's sink list belongs to the
     * person using it and can change between the two legs.  That there is at
     * least one is the checkable fact, and it is the one the call answers. */
    out( " devices>=1=" );
    out_dec( count >= 1 ? 1 : 0 );
    verdict( SUCCEEDED(hr) && count >= 1, "no audio device" );

    begin( "IXAudio2::StartEngine" );
    hr = xa2->lpVtbl->StartEngine( xa2 );
    out_hr( "hr", hr );
    verdict( SUCCEEDED(hr), "the engine did not start" );

    begin( "IXAudio2::CreateMasteringVoice (2.7 argument list)" );
    hr = xa2->lpVtbl->CreateMasteringVoice( xa2, &master,
                                            SC_XAUDIO2_DEFAULT_CHANNELS,
                                            SC_XAUDIO2_DEFAULT_SAMPLERATE,
                                            0, 0 /* device index */, NULL );
    out_hr( "hr", hr );
    verdict( SUCCEEDED(hr) && master != NULL, "no mastering voice" );
    if (!master) goto release_xa2;

    /* Slot 0 of a voice is GetVoiceDetails and NOT QueryInterface.  The struct
     * starts as a value the call must overwrite. */
    begin( "IXAudio2Voice::GetVoiceDetails on the mastering voice (slot 0)" );
    details.CreationFlags = 0xdeadbeef;
    details.InputChannels = 0xdeadbeef;
    details.InputSampleRate = 0xdeadbeef;
    master->lpVtbl->GetVoiceDetails( master, &details );
    out( "flags=0x" );
    out_hex( details.CreationFlags, 8 );
    out( " channels=" );
    out_dec( details.InputChannels );
    out( " rate=" );
    out_dec( details.InputSampleRate );
    verdict( details.InputChannels >= 1 && details.InputChannels <= 8 &&
#if SC_AUDIO_BREAK == 1
             details.InputChannels == 99 &&
#endif
             details.InputSampleRate >= 8000 && details.InputSampleRate <= 192000,
             "GetVoiceDetails answered nothing a mastering voice could have" );

    /* A float in XMM1, through the shape-keyed hand slot. */
    begin( "IXAudio2Voice::SetVolume/GetVolume round-trip (float in XMM1)" );
    hr = master->lpVtbl->SetVolume( master, 0.375f, SC_XAUDIO2_COMMIT_NOW );
    v = 0.0f;
    master->lpVtbl->GetVolume( master, &v );
    out_hr( "hr", hr );
    out( " bits=0x" );
    out_hex( fbits( v ), 8 );
    out( " want=0x" );
#if SC_AUDIO_BREAK == 2
    out_hex( fbits( 99.0f ), 8 );
    verdict( SUCCEEDED(hr) && fbits( v ) == fbits( 99.0f ), "volume did not survive" );
#else
    out_hex( fbits( 0.375f ), 8 );
    verdict( SUCCEEDED(hr) && fbits( v ) == fbits( 0.375f ), "volume did not survive" );
#endif

    /* MaxFrequencyRatio is argument FOUR, so MS-x64 puts it on the stack. */
    begin( "IXAudio2::CreateSourceVoice (MaxFrequencyRatio on the stack)" );
    wfx.wFormatTag = WAVE_FORMAT_PCM;
    wfx.nChannels = 2;
    wfx.nSamplesPerSec = 44100;
    wfx.wBitsPerSample = 16;
    wfx.nBlockAlign = (WORD)(wfx.nChannels * wfx.wBitsPerSample / 8);
    wfx.nAvgBytesPerSec = wfx.nSamplesPerSec * wfx.nBlockAlign;
    wfx.cbSize = 0;
    hr = xa2->lpVtbl->CreateSourceVoice( xa2, &src, &wfx, 0, 2.0f,
                                         NULL, NULL, NULL );
    out_hr( "hr", hr );
    verdict( SUCCEEDED(hr) && src != NULL, "no source voice" );

    if (src)
    {
        begin( "IXAudio2Voice::GetVoiceDetails on the source voice" );
        details.CreationFlags = 0xdeadbeef;
        details.InputChannels = 0xdeadbeef;
        details.InputSampleRate = 0xdeadbeef;
        src->lpVtbl->GetVoiceDetails( src, &details );
        out( "channels=" );
        out_dec( details.InputChannels );
        out( " rate=" );
        out_dec( details.InputSampleRate );
        verdict( details.InputChannels == 2 && details.InputSampleRate == 44100,
                 "the source voice did not keep the format it was created with" );

        begin( "IXAudio2SourceVoice::SetFrequencyRatio/GetFrequencyRatio" );
        hr = src->lpVtbl->SetFrequencyRatio( src, 1.5f, SC_XAUDIO2_COMMIT_NOW );
        v = 0.0f;
        src->lpVtbl->GetFrequencyRatio( src, &v );
        out_hr( "hr", hr );
        out( " bits=0x" );
        out_hex( fbits( v ), 8 );
        out( " want=0x" );
        out_hex( fbits( 1.5f ), 8 );
        verdict( SUCCEEDED(hr) && fbits( v ) == fbits( 1.5f ),
                 "the frequency ratio did not survive" );

        /* The NULL half of the two hand-written voice slots: "route to the
         * mastering voice" and "no effect chain" are real requests. */
        begin( "IXAudio2Voice::SetOutputVoices(NULL) and SetEffectChain(NULL)" );
        hr = src->lpVtbl->SetOutputVoices( src, NULL );
        out_hr( "sends", hr );
        hr = SUCCEEDED(hr) ? src->lpVtbl->SetEffectChain( src, NULL ) : hr;
        out_hr( " chain", hr );
        verdict( SUCCEEDED(hr), "the default routing was refused" );
    }

    /* ---- the port-specific observations, on stderr only ---------------- */
    {
        SC_IXAudio2SourceVoice *cbvoice = NULL;

        hr = xa2->lpVtbl->RegisterForCallbacks( xa2, &sc_engine_cb );
        note( "note: IXAudio2::RegisterForCallbacks ->", hr );
        if (SUCCEEDED(hr)) xa2->lpVtbl->UnregisterForCallbacks( xa2, &sc_engine_cb );

        hr = xa2->lpVtbl->CreateSourceVoice( xa2, &cbvoice, &wfx, 0, 1.0f,
                                             &sc_voice_cb, NULL, NULL );
        note( "note: IXAudio2::CreateSourceVoice with an IXAudio2VoiceCallback ->", hr );
        if (SUCCEEDED(hr) && cbvoice) cbvoice->lpVtbl->DestroyVoice( cbvoice );
    }

    begin( "destroy the voices and release the engine" );
    if (src) src->lpVtbl->DestroyVoice( src );
    src = NULL;
    master->lpVtbl->DestroyVoice( master );
    master = NULL;
    xa2->lpVtbl->StopEngine( xa2 );
    count = xa2->lpVtbl->Release( xa2 );
    xa2 = NULL;
    out( "final_refs=" );
    out_dec( count );
    verdict( count == 0, "the engine did not reach zero references" );

    /* ================================================ WASAPI ============= */

    begin( "CoCreateInstance(CLSID_MMDeviceEnumerator, IID_IMMDeviceEnumerator)" );
    hr = CoCreateInstance( &sc_CLSID_MMDeviceEnumerator, NULL, CLSCTX_INPROC_SERVER,
                           &sc_IID_IMMDeviceEnumerator, (void **)&devenum );
    out_hr( "hr", hr );
    verdict( hr == S_OK && devenum != NULL, "no device enumerator" );
    if (!devenum) goto uninit;

    begin( "IMMDeviceEnumerator::GetDefaultAudioEndpoint(eRender, eMultimedia)" );
    hr = IMMDeviceEnumerator_GetDefaultAudioEndpoint( devenum, eRender, eMultimedia, &dev );
    out_hr( "hr", hr );
    verdict( hr == S_OK && dev != NULL, "no default endpoint" );

    if (dev)
    {
        begin( "IMMDevice::GetState on the default endpoint" );
        state = 0;
        hr = IMMDevice_GetState( dev, &state );
        out_hr( "hr", hr );
        out( " state=0x" );
        out_hex( state, 8 );
#if SC_AUDIO_BREAK == 3
        verdict( hr == S_OK && state == DEVICE_STATE_UNPLUGGED, "not ACTIVE" );
#else
        verdict( hr == S_OK && state == DEVICE_STATE_ACTIVE, "not ACTIVE" );
#endif

        /* A riid/void** pair inside a HAND-WRITTEN slot: pActivationParams is a
         * PROPVARIANT, so the slot serves the NULL case and refuses the rest. */
        begin( "IMMDevice::Activate(IID_IAudioClient)" );
        hr = IMMDevice_Activate( dev, &sc_IID_IAudioClient, CLSCTX_ALL, NULL,
                                 (void **)&client );
        out_hr( "hr", hr );
        verdict( hr == S_OK && client != NULL, "no audio client" );

        note( "note: IMMDevice::OpenPropertyStore ->",
              IMMDevice_OpenPropertyStore( dev, STGM_READ, &pstore ) );
        if (pstore) { IUnknown_Release( (IUnknown *)pstore ); pstore = NULL; }

        pstore = NULL;
        hr = IMMDevice_Activate( dev, &sc_IID_IAudioClient2, CLSCTX_ALL, NULL,
                                 (void **)&pstore );
        note( "note: IMMDevice::Activate(IID_IAudioClient2, unrostered) ->", hr );
        if (SUCCEEDED(hr) && pstore)
        { IUnknown_Release( (IUnknown *)pstore ); pstore = NULL; }
    }

    if (client)
    {
        begin( "IAudioClient::GetMixFormat" );
        hr = IAudioClient_GetMixFormat( client, &mix );
        out_hr( "hr", hr );
        out( " channels=" );
        out_dec( mix ? mix->nChannels : 0 );
        out( " rate=" );
        out_dec( mix ? mix->nSamplesPerSec : 0 );
        verdict( hr == S_OK && mix && mix->nChannels >= 1 &&
                 mix->nSamplesPerSec >= 8000, "no usable mix format" );

        begin( "IAudioClient::Initialize(SHARED, 1s)" );
        hr = IAudioClient_Initialize( client, AUDCLNT_SHAREMODE_SHARED, 0,
                                      10000000, 0, mix, NULL );
        out_hr( "hr", hr );
        verdict( hr == S_OK, "the client did not initialize" );

        begin( "IAudioClient::GetBufferSize" );
        hr = IAudioClient_GetBufferSize( client, &frames );
        out_hr( "hr", hr );
        out( " frames>=rate=" );
        out_dec( (mix && frames >= mix->nSamplesPerSec) ? 1 : 0 );
        verdict( hr == S_OK && mix && frames >= mix->nSamplesPerSec,
                 "a one-second buffer is shorter than one second" );

        begin( "IAudioClient::GetService(IID_IAudioRenderClient)" );
        hr = IAudioClient_GetService( client, &sc_IID_IAudioRenderClient,
                                      (void **)&render );
        out_hr( "hr", hr );
        verdict( hr == S_OK && render != NULL, "no render client" );
    }

    if (render && mix)
    {
        BYTE *data = NULL;
        UINT32 want = mix->nSamplesPerSec / 100;   /* 10 ms */
        UINT32 i;

        begin( "IAudioRenderClient::GetBuffer / write / ReleaseBuffer" );
        hr = IAudioRenderClient_GetBuffer( render, want, &data );
        if (SUCCEEDED(hr) && data)
        {
            for (i = 0; i < want * mix->nBlockAlign; i++) data[i] = 0;
            hr = IAudioRenderClient_ReleaseBuffer( render, want, 0 );
        }
        out_hr( "hr", hr );
        out( " frames=" );
        out_dec( want );
        verdict( SUCCEEDED(hr) && data != NULL,
                 "the render buffer was not usable" );
    }

    begin( "IMMDeviceEnumerator::EnumAudioEndpoints / GetCount / Item(0)" );
    hr = IMMDeviceEnumerator_EnumAudioEndpoints( devenum, eRender,
                                                 DEVICE_STATE_ACTIVE, &coll );
    if (SUCCEEDED(hr) && coll)
    {
        hr = IMMDeviceCollection_GetCount( coll, &ncoll );
        if (SUCCEEDED(hr) && ncoll) hr = IMMDeviceCollection_Item( coll, 0, &dev2 );
    }
    out_hr( "hr", hr );
    out( " endpoints>=1=" );      /* see GetDeviceCount above */
    out_dec( ncoll >= 1 ? 1 : 0 );
    verdict( SUCCEEDED(hr) && ncoll >= 1 && dev2 != NULL,
             "the collection did not yield a device" );

    if (dev2)
    {
        begin( "IMMDevice::GetState on collection item 0" );
        state = 0;
        hr = IMMDevice_GetState( dev2, &state );
        out_hr( "hr", hr );
        out( " state=0x" );
        out_hex( state, 8 );
        verdict( hr == S_OK && state == DEVICE_STATE_ACTIVE, "not ACTIVE" );
    }

    /* A plain non-NULL address is enough HERE and only here: Wine's
     * MMDevEnum_UnregisterEndpointNotificationCallback walks its client list
     * COMPARING pointers and never dereferences one, so the native leg answers
     * E_NOTFOUND without touching it while the guest leg is refused by name
     * before the argument is looked at at all. */
    note( "note: IMMDeviceEnumerator::UnregisterEndpointNotificationCallback ->",
          IMMDeviceEnumerator_UnregisterEndpointNotificationCallback(
              devenum, (IMMNotificationClient *)&wfx ) );

    /* THE FINAL REFCOUNT IS NOT COMPARED HERE, and that is a finding rather
     * than a softening.  mmdevapi's MMDeviceEnumerator is a process singleton
     * and its IMMDevice objects are cached in a global list, so the number the
     * last Release returns natively is whatever ELSE in the process is still
     * holding one -- while the guest sees its PROXY's count, which is its own.
     * The two are different numbers for a correct run, unlike XAudio2's engine
     * in step 13, which is a fresh object each time and does round-trip.  What
     * IS checkable and identical is that the whole chain can be torn down and
     * built again: the guest leg's re-acquisition also destroys and re-interns
     * a proxy for the same host pointer, which nothing else here does. */
    begin( "release the WASAPI chain and acquire it again" );
    if (render) IAudioRenderClient_Release( render );
    render = NULL;
    if (client) IAudioClient_Release( client );
    client = NULL;
    if (dev2) IMMDevice_Release( dev2 );
    dev2 = NULL;
    if (dev) IMMDevice_Release( dev );
    dev = NULL;
    if (coll) IMMDeviceCollection_Release( coll );
    coll = NULL;
    IMMDeviceEnumerator_Release( devenum );
    devenum = NULL;

    hr = CoCreateInstance( &sc_CLSID_MMDeviceEnumerator, NULL, CLSCTX_INPROC_SERVER,
                           &sc_IID_IMMDeviceEnumerator, (void **)&devenum );
    if (SUCCEEDED(hr))
        hr = IMMDeviceEnumerator_GetDefaultAudioEndpoint( devenum, eRender,
                                                          eMultimedia, &dev );
    state = 0;
    if (SUCCEEDED(hr)) hr = IMMDevice_GetState( dev, &state );
    out_hr( "hr", hr );
    out( " state=0x" );
    out_hex( state, 8 );
    verdict( hr == S_OK && state == DEVICE_STATE_ACTIVE,
             "the chain did not come back after being torn down" );
    if (dev) IMMDevice_Release( dev );
    dev = NULL;
    if (devenum) IMMDeviceEnumerator_Release( devenum );
    devenum = NULL;

release_xa2:
    if (master) master->lpVtbl->DestroyVoice( master );
    if (xa2) xa2->lpVtbl->Release( xa2 );
    master = NULL;
    xa2 = NULL;

uninit:
    begin( "CoUninitialize" );
    CoUninitialize();
    out( "returned" );
    verdict( TRUE, "" );

done:
    out( failures ? "syscom_audio_smoke: FAIL " : "syscom_audio_smoke: PASS " );
    out_dec( (ULONG)(step - failures) );
    out( "/" );
    out_dec( (ULONG)step );
    if (failures && first_fail)
    {
        out( " (" );
        out( first_fail );
        out( ")" );
    }
    out( "\n" );
    return failures ? 1 : 0;
}

#ifdef SC_AUDIO_NO_CRT
/* The guest build has no C runtime: this IS the image entry point. */
void WINAPI sc_audio_entry( void )
{
    ExitProcess( (UINT)sc_audio_run() );
}
#else
int main( void )
{
    return sc_audio_run();
}
#endif
