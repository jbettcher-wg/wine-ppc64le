/*
 * xa_smoke -- the native-vs-guest XAudio2 runtime gate.
 *
 * ONE source, built TWICE and run TWICE under the same wine: once as a NATIVE
 * ppc64 PE and once as an x86-64 Windows PE run as a GUEST.  The two runs must
 * print BYTE-IDENTICAL stdout.  Both legs reach the same xaudio2_9.dll, the
 * same FAudio and the same host device; the only difference is whether the
 * caller's instructions are ppc64 or x86-64.  Read ds_smoke.c first -- this is
 * the same claim about a harder surface.
 *
 * WHAT MAKES XAUDIO2 HARDER, AND WHICH STEPS SAY SO
 *
 *   * A VOICE IS NOT A COM OBJECT.  IXAudio2Voice and the three interfaces
 *     derived from it are `[local]` in include/xaudio2.idl: no QueryInterface,
 *     no AddRef, no Release, no IID.  Slot 0 of a mastering voice is
 *     GetVoiceDetails, and libs/winecom's dispatcher serves slots 0..2 of
 *     every interface as IUnknown -- so dlls/xaudio2_9/guestcom.c claims these
 *     interfaces and serves them itself.  STEPS 4 AND 6 ARE THAT CLAIM: if the
 *     claiming were missing, GetVoiceDetails would be answered by
 *     QueryInterface and the details struct would come back untouched.
 *
 *   * FLOATS.  MaxFrequencyRatio is CreateSourceVoice's FOURTH argument, so
 *     MS-x64 puts it on the stack rather than in an XMM register (step 5), and
 *     SetVolume/SetFrequencyRatio take a float in XMM1 (steps 9 and 10).  Every
 *     constant here is a dyadic fraction and every comparison is on the RAW
 *     BITS, because a float that crossed through an integer register is a
 *     WRONG NUMBER and not a crash.
 *
 *   * THE MIXER ACTUALLY RUNS.  Step 8 waits for SamplesPlayed to leave zero,
 *     which only happens if FAudio's thread is consuming the submitted buffer
 *     against a real device.
 *
 * THE REVERSE-PROXY DIRECTION (steps 13-19) used to be refused here.  A guest-
 * implemented COM object passed INTO native code -- IXAudio2::
 * RegisterForCallbacks, whose IXAudio2EngineCallback the application
 * implements and XAudio2 calls from its mixer thread, and
 * IXAudio2::CreateSourceVoice with a non-NULL pCallback, the
 * IXAudio2VoiceCallback a streaming title uses for buffer completion -- both
 * answered E_NOTIMPL on the guest leg, by name, and the only claim this probe
 * could make about them was that the refusal happened.
 * libs/winecom/reverse.c now builds the mirror: a NATIVE vtable for a GUEST-
 * implemented object, whose slots marshal ELFv2 arguments into MS-x64 and
 * enter the guest method through the emulator.  So both calls are SERVED, and
 * this probe implements a REAL IXAudio2VoiceCallback and a REAL
 * IXAudio2EngineCallback IN ITSELF -- native ppc64 on the native leg, x86-64
 * on the guest leg -- which is what turns "the port does not crash" into a
 * diffable identity claim: the transcripts must be byte-identical whether the
 * callback was entered directly (native leg) or through rev_enter_guest and
 * the marshalled dispatch (guest leg).
 *
 *   * STEPS 13-14 are RegisterForCallbacks and CreateSourceVoice(pCallback)
 *     themselves: both must now answer S_OK.
 *   * STEPS 15-17 are the point of the whole exercise -- submit a buffer, wait
 *     (bounded; a timeout is a FAIL and never a hang) for OnBufferEnd on
 *     XAudio2's own mixer thread, and get back the EXACT pBufferContext that
 *     was put in XAUDIO2_BUFFER.pContext.  A pointer that travelled through
 *     the wrong marshalling comes back as a different value, not a crash.
 *   * STEP 18 checks the ordering facts that are deterministic regardless of
 *     scheduling: OnBufferStart before OnBufferEnd, and at least one
 *     OnVoiceProcessingPassStart.  The exact COUNT of per-pass callbacks is
 *     NOT deterministic -- it is a function of when the mixer thread ran
 *     relative to the wait -- so it goes to stderr as a note, along with the
 *     measured mixer-thread entry cost below.
 *   * STEP 19 is the INTERNING claim.  IXAudio2VoiceCallback and
 *     IXAudio2EngineCallback are `[local]`, with no AddRef for a reference
 *     count to hang UnregisterForCallbacks off of; what makes Unregister find
 *     the SAME registration is that reverse proxies are interned by (guest
 *     pointer, interface), so the pointer handed to Unregister resolves to
 *     the same native proxy RegisterForCallbacks built.
 *     UnregisterForCallbacks is declared `void` in the IDL, so there is no
 *     HRESULT to check; the check here is stronger -- that the engine
 *     callback actually STOPS being called afterwards.  A mechanism that
 *     minted a fresh wrapper per call would still be reachable under a
 *     DIFFERENT native pointer and would keep firing forever; only a
 *     mechanism that finds the ORIGINAL registration can make it stop.
 *
 * THE MIXER-THREAD ENTRY COST, printed to stderr and never diffed -- it is
 * SUPPOSED to differ between the legs; that difference is the number this
 * gate exists to produce.  OnVoiceProcessingPassStart takes a
 * QueryPerformanceCounter reading at every entry for as long as the run
 * lasts, and step 18's report is the pass count and the min/median/max
 * microseconds between CONSECUTIVE entries: the mixer's quantum period PLUS
 * whatever it costs to re-enter the callback -- on the guest leg, through
 * rev_enter_guest and the emulator; on the native leg, an ordinary indirect
 * call.  It is not a clean isolation of the crossing alone (FAudio's own
 * per-quantum work is in there too, on both legs equally), but the two
 * periods are directly comparable because everything else about the quantum
 * is identical, so the DIFFERENCE between the two runs' medians is what a
 * crossing costs.
 *
 * XA_SMOKE_BREAK (falsification; the gate builds each variant and requires it
 * to FAIL):
 *
 *   =1  never submit the buffer, so step 7's queue depth is wrong and step 8
 *       never sees a sample played.
 *   =2  corrupt the expected MaxFrequencyRatio read-back in step 5, so the
 *       stack-float check is shown to be a check.
 *   =3  never submit the callback-bearing buffer (step 15), so OnBufferEnd
 *       never fires and step 16's bounded wait times out -- a FAIL, not a
 *       hang.
 *   =4  corrupt the expected pBufferContext in step 17, so the value check on
 *       what the callback received is shown to be a check.
 *   =5  ask the reverb effect (step 22) for an IID nothing implements, so the
 *       S_OK that step normally reports is shown to be a check rather than a
 *       constant.  The XAPO counterpart of =4.
 *
 * NO C RUNTIME on the guest leg; the program formats its own output.
 *
 * Copyright 2026 the ppc64le port authors
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 */

#ifndef XA_SMOKE_BREAK
#define XA_SMOKE_BREAK 0
#endif

/* The C-mode vtable wrappers -- IXAudio2_CreateSourceVoice and friends -- are
 * behind COBJMACROS, which is how an ordinary C application uses widl output. */
#define COBJMACROS

#include <windows.h>
#include <mmreg.h>
#ifdef XA_SMOKE_V8
/* The xaudio2_8 leg compiles against THAT MODULE'S OWN widl output rather than
 * include/xaudio2.h, and it has to: include/xaudio2.h is generated once, at
 * the 2_9 shape (its IID_IXAudio2 is 2b02e3cf-..., which is 2_9's; 2_8's is
 * 60d8dac8-...), and three of IXAudio2's methods differ between the versions.
 * A probe built against the 2_9 declarations and pointed at xaudio2_8.dll
 * would call the right slots with the wrong argument lists -- silently.  So
 * the gate passes -I<build>/dlls/xaudio2_8 and this include picks up the
 * header that module was itself compiled from, exactly as
 * dlls/xaudio2_7/guestcom.c does. */
#include <xaudio_classes.h>
#else
#include <xaudio2.h>
#endif
#include <xapo.h>

/* The two XAPO IIDs, spelled out here rather than linked from libuuid, for the
 * reason ppc64le/syscom/com_smoke.c gives for its own: the guest leg links no
 * Wine import libraries at all, so a GUID both builds compile from the same
 * literal is the only way the two legs can be comparing the same thing.  The
 * values are include/xapo.idl's own uuid() attributes, copied. */
static const GUID xa_IID_IXAPO =
    { 0xa410b984, 0x9839, 0x4819, { 0xa0, 0xbe, 0x28, 0x56, 0xae, 0x6b, 0x3a, 0xdb } };
static const GUID xa_IID_IXAPOParameters =
    { 0x26d95c66, 0x80f2, 0x499a, { 0xad, 0x54, 0x5a, 0xe7, 0xf0, 0x1c, 0x6d, 0x98 } };

/* CreateAudioReverb is declared in include/xaudio2fx.h, which this probe does
 * not include because that header drags in the effect PARAMETER structs and
 * their version conditionals for no benefit here.  One prototype, copied from
 * xaudio2fx.idl:72. */
HRESULT WINAPI CreateAudioReverb( IUnknown **out );

/* ------------------------------------------------------------- output */

static void out_fd( HANDLE h, const char *s )
{
    DWORD n = 0, written;
    while (s[n]) n++;
    WriteFile( h, s, n, &written, NULL );
}

static void out( const char *s )
{
    out_fd( GetStdHandle( STD_OUTPUT_HANDLE ), s );
}

/* The stderr counterparts of out()/out_dec(): the mixer-entry-cost report
 * (below) is a `note:` line by definition -- it is SUPPOSED to differ between
 * the legs -- so it is built out of these rather than out()/out_dec(), which
 * write the diffed transcript. */
static void err( const char *s )
{
    out_fd( GetStdHandle( STD_ERROR_HANDLE ), s );
}

static void err_dec( ULONG v )
{
    char buf[12];
    int i = 11;

    buf[i] = 0;
    do { buf[--i] = '0' + (char)(v % 10); v /= 10; } while (v);
    out_fd( GetStdHandle( STD_ERROR_HANDLE ), buf + i );
}

static void out_hex( ULONGLONG v, int digits )
{
    static const char hex[] = "0123456789ABCDEF";
    char buf[17];
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

static ULONG fbits( float f )
{
    union { float f; ULONG u; } c;
    c.f = f;
    return c.u;
}

static void zero( void *p, UINT n )
{
    BYTE *b = p;
    UINT i;
    for (i = 0; i < n; i++) b[i] = 0;
}

/* ----------------------------------------- the reverse-proxy callbacks
 *
 * A REAL IXAudio2VoiceCallback and a REAL IXAudio2EngineCallback, built by
 * hand as vtable structs -- both interfaces are `[local]`, so there is no
 * widl-generated coclass to implement them through, only the Vtbl layout
 * COBJMACROS already pulled in for the client side.  Every method here does
 * the least work that lets the MAIN THREAD compute a deterministic verdict
 * afterwards: it stamps counters and a couple of `first-writer-wins` sequence
 * numbers, and nothing here calls out(), because two threads writing the
 * diffed transcript would race and the ORDER two threads observe events in is
 * exactly what is under test, not incidental.
 */

#define MAX_PASS_SAMPLES 2048

static volatile LONG voice_pass_start_count;
static volatile LONG event_seq;
static volatile LONG buffer_start_seq;
static volatile LONG buffer_end_seq;
static void *volatile buffer_end_context;
static HANDLE buffer_end_event;

static volatile LONG pass_sample_count;
static LONGLONG pass_entry_qpc[MAX_PASS_SAMPLES];

static volatile LONG engine_pass_start_count;
static volatile LONG engine_pass_end_count;
static volatile LONG engine_critical_error_count;

/* Its ADDRESS is the distinctive pContext for the delivery test.  The address
 * itself is never printed -- a ppc64 and an x86-64 image do not share one --
 * only ever compared for identity against what OnBufferEnd received. */
static BYTE cb_marker;

static void STDMETHODCALLTYPE voice_cb_OnVoiceProcessingPassStart(
    IXAudio2VoiceCallback *This, UINT32 BytesRequired )
{
    LARGE_INTEGER now;
    LONG idx;

    (void)This; (void)BytesRequired;
    InterlockedIncrement( &voice_pass_start_count );
    QueryPerformanceCounter( &now );
    idx = InterlockedIncrement( &pass_sample_count ) - 1;
    if (idx >= 0 && idx < MAX_PASS_SAMPLES) pass_entry_qpc[idx] = now.QuadPart;
}

static void STDMETHODCALLTYPE voice_cb_OnVoiceProcessingPassEnd( IXAudio2VoiceCallback *This )
{
    (void)This;
}

static void STDMETHODCALLTYPE voice_cb_OnStreamEnd( IXAudio2VoiceCallback *This )
{
    (void)This;
}

static void STDMETHODCALLTYPE voice_cb_OnBufferStart( IXAudio2VoiceCallback *This, void *ctx )
{
    LONG seq = InterlockedIncrement( &event_seq );
    (void)This; (void)ctx;
    InterlockedCompareExchange( &buffer_start_seq, seq, 0 );
}

static void STDMETHODCALLTYPE voice_cb_OnBufferEnd( IXAudio2VoiceCallback *This, void *ctx )
{
    LONG seq = InterlockedIncrement( &event_seq );
    (void)This;
    InterlockedCompareExchange( &buffer_end_seq, seq, 0 );
    buffer_end_context = ctx;
    if (buffer_end_event) SetEvent( buffer_end_event );
}

static void STDMETHODCALLTYPE voice_cb_OnLoopEnd( IXAudio2VoiceCallback *This, void *ctx )
{
    (void)This; (void)ctx;
}

static void STDMETHODCALLTYPE voice_cb_OnVoiceError(
    IXAudio2VoiceCallback *This, void *ctx, HRESULT Error )
{
    (void)This; (void)ctx; (void)Error;
}

static const IXAudio2VoiceCallbackVtbl voice_cb_vtbl =
{
    voice_cb_OnVoiceProcessingPassStart,
    voice_cb_OnVoiceProcessingPassEnd,
    voice_cb_OnStreamEnd,
    voice_cb_OnBufferStart,
    voice_cb_OnBufferEnd,
    voice_cb_OnLoopEnd,
    voice_cb_OnVoiceError,
};
static IXAudio2VoiceCallback voice_cb = { &voice_cb_vtbl };

static void STDMETHODCALLTYPE engine_cb_OnProcessingPassStart( IXAudio2EngineCallback *This )
{
    (void)This;
    InterlockedIncrement( &engine_pass_start_count );
}

static void STDMETHODCALLTYPE engine_cb_OnProcessingPassEnd( IXAudio2EngineCallback *This )
{
    (void)This;
    InterlockedIncrement( &engine_pass_end_count );
}

static void STDMETHODCALLTYPE engine_cb_OnCriticalError(
    IXAudio2EngineCallback *This, HRESULT Error )
{
    (void)This; (void)Error;
    InterlockedIncrement( &engine_critical_error_count );
}

static const IXAudio2EngineCallbackVtbl engine_cb_vtbl =
{
    engine_cb_OnProcessingPassStart,
    engine_cb_OnProcessingPassEnd,
    engine_cb_OnCriticalError,
};
static IXAudio2EngineCallback engine_cb = { &engine_cb_vtbl };

/* Microseconds between two QueryPerformanceCounter readings.  freq is never
 * zero once QueryPerformanceFrequency has returned once, and ticks*1000000
 * stays well inside 64 bits for any period this probe ever measures. */
static LONGLONG qpc_to_us( LONGLONG ticks, LONGLONG freq )
{
    return (ticks * 1000000) / freq;
}

static void sort_i64( LONGLONG *a, UINT n )
{
    UINT i, j;
    for (i = 1; i < n; i++)
    {
        LONGLONG v = a[i];
        j = i;
        while (j > 0 && a[j - 1] > v) { a[j] = a[j - 1]; j--; }
        a[j] = v;
    }
}

/* THE MIXER-THREAD ENTRY COST (see the banner).  stderr only, by design: the
 * whole point of this report is that its number is NOT the same on both
 * legs. */
static void report_mixer_entry_cost( DWORD wall_ms )
{
    static LONGLONG periods[MAX_PASS_SAMPLES];
    LARGE_INTEGER f;
    LONGLONG freq;
    UINT count, n, i;

    count = (UINT)pass_sample_count;
    if (count > MAX_PASS_SAMPLES) count = MAX_PASS_SAMPLES;
    QueryPerformanceFrequency( &f );
    freq = f.QuadPart;

    err( "note: xaudio2 mixer entry cost: " );
    err_dec( count );
    err( " OnVoiceProcessingPassStart call(s) seen in " );
    err_dec( wall_ms );
    err( " ms" );

    if (count < 2 || freq <= 0)
    {
        err( " (too few samples for a period distribution)\n" );
        return;
    }

    n = count - 1;
    for (i = 0; i < n; i++)
        periods[i] = qpc_to_us( pass_entry_qpc[i + 1] - pass_entry_qpc[i], freq );
    sort_i64( periods, n );

    err( "; inter-arrival period min=" ); err_dec( (ULONG)periods[0] );
    err( "us median=" ); err_dec( (ULONG)periods[n / 2] );
    err( "us max=" ); err_dec( (ULONG)periods[n - 1] );
    err( "us (" ); err_dec( (ULONG)(((LONGLONG)wall_ms * 1000) / count) );
    err( "us/pass average) -- this differs between the legs BY CONSTRUCTION; "
         "that difference is what one crossing into guest code costs\n" );
}

/* ------------------------------------------------------------- the run */

static int failures;
static int step;
static const char *first_fail;

static void begin( const char *what )
{
    out( "step " );
    out_dec( (ULONG)++step );
    out( " " );
    out( what );
    out( ": " );
}

static void verdict( BOOL ok, const char *why )
{
    if (ok) out( " ok\n" );
    else
    {
        if (!first_fail) first_fail = why;
        failures++;
        out( " FAIL (" );
        out( why );
        out( ")\n" );
    }
}

static void out_hr( const char *label, HRESULT hr )
{
    out( label );
    out( "=0x" );
    out_hex( (ULONG)hr, 8 );
}

#define SAMPLE_RATE   44100
#define CHANNELS      2
#define BLOCK_ALIGN   (CHANNELS * 2)
#define PCM_BYTES     (SAMPLE_RATE * BLOCK_ALIGN)   /* exactly one second */

/* The callback-delivery test (steps 15-18) wants OnBufferEnd to fire inside a
 * bounded wait rather than after a full second of real-time playback, so it
 * gets a short slice of the same PCM: one tenth of a second. */
#define CB_PCM_BYTES  (PCM_BYTES / 10)

/* The submitted audio lives for the life of the voice, so it is static: an
 * XAUDIO2_BUFFER does not copy, it borrows. */
static BYTE pcm[PCM_BYTES];

static int xa_smoke_run( void )
{
    IXAudio2 *xa2 = NULL;
    IXAudio2MasteringVoice *master = NULL;
    IXAudio2SourceVoice *src = NULL;
    IXAudio2SourceVoice *cbvoice = NULL;
    XAUDIO2_VOICE_DETAILS details;
    XAUDIO2_VOICE_STATE state;
    XAUDIO2_BUFFER xbuf;
    WAVEFORMATEX wfx;
    HRESULT hr;
    UINT i;

    for (i = 0; i < PCM_BYTES; i++)
        pcm[i] = (BYTE)(((i * 2654435761u) >> 13) ^ (i * 131u));

    /* An auto-reset event, exactly as ds_smoke.c's IDirectSoundNotify step
     * uses: OnBufferEnd (from the mixer thread, possibly through the reverse
     * proxy) sets it, the main thread waits on it with a bound. */
    buffer_end_event = CreateEventW( NULL, FALSE, FALSE, NULL );

    /* ---- 1: the engine ---------------------------------------------- */
    begin( "XAudio2Create" );
    hr = XAudio2Create( &xa2, 0, XAUDIO2_DEFAULT_PROCESSOR );
    out_hr( "hr", hr );
    out( " obj=" ); out( xa2 ? "yes" : "no" );
    verdict( SUCCEEDED(hr) && xa2 != NULL, "no IXAudio2" );
    if (!xa2) goto done;

    begin( "StartEngine" );
    hr = IXAudio2_StartEngine( xa2 );
    out_hr( "hr", hr );
    verdict( SUCCEEDED(hr), "the engine did not start" );

    /* ---- 3-4: the mastering voice, and slot 0 of a [local] interface -- */
    begin( "CreateMasteringVoice" );
    hr = IXAudio2_CreateMasteringVoice( xa2, &master, XAUDIO2_DEFAULT_CHANNELS,
                                        XAUDIO2_DEFAULT_SAMPLERATE, 0, NULL,
                                        NULL, AudioCategory_GameEffects );
    out_hr( "hr", hr );
    out( " obj=" ); out( master ? "yes" : "no" );
    verdict( SUCCEEDED(hr) && master != NULL, "no mastering voice" );
    if (!master) goto done;

    begin( "GetVoiceDetails on the mastering voice (slot 0, not QueryInterface)" );
    zero( &details, sizeof(details) );
    IXAudio2MasteringVoice_GetVoiceDetails( master, &details );
    out( "channels=" ); out_dec( details.InputChannels );
    out( " rate=" ); out_dec( details.InputSampleRate );
    verdict( details.InputChannels > 0 && details.InputSampleRate > 0,
             "the mastering voice's details came back empty -- slot 0 was "
             "served as QueryInterface" );

    /* ---- 5: the source voice, with a float on the stack --------------- */
    zero( &wfx, sizeof(wfx) );
    wfx.wFormatTag      = WAVE_FORMAT_PCM;
    wfx.nChannels       = CHANNELS;
    wfx.nSamplesPerSec  = SAMPLE_RATE;
    wfx.wBitsPerSample  = 16;
    wfx.nBlockAlign     = BLOCK_ALIGN;
    wfx.nAvgBytesPerSec = SAMPLE_RATE * BLOCK_ALIGN;

    begin( "CreateSourceVoice (MaxFrequencyRatio is argument 4, on the stack)" );
    hr = IXAudio2_CreateSourceVoice( xa2, &src, &wfx, 0, 2.0f, NULL, NULL, NULL );
    out_hr( "hr", hr );
    out( " obj=" ); out( src ? "yes" : "no" );
    verdict( SUCCEEDED(hr) && src != NULL, "no source voice" );
    if (!src) goto done;

    begin( "GetVoiceDetails on the source voice" );
    zero( &details, sizeof(details) );
    IXAudio2SourceVoice_GetVoiceDetails( src, &details );
    out( "channels=" ); out_dec( details.InputChannels );
    out( " rate=" ); out_dec( details.InputSampleRate );
    verdict( details.InputChannels == CHANNELS &&
             details.InputSampleRate == SAMPLE_RATE,
             "the source voice does not have the format it was asked for" );

    /* MaxFrequencyRatio is not readable back directly; what IS observable is
     * that a ratio ABOVE the default 2.0 would have been rejected had the
     * argument not arrived, and one at exactly 2.0 is accepted.  So set the
     * ratio to the boundary value and read it back below (step 10). */

    /* ---- 7: submit a buffer ------------------------------------------ */
    begin( "SubmitSourceBuffer" );
    zero( &xbuf, sizeof(xbuf) );
    xbuf.AudioBytes = PCM_BYTES;
    xbuf.pAudioData = pcm;
    xbuf.Flags      = XAUDIO2_END_OF_STREAM;
#if XA_SMOKE_BREAK != 1
    hr = IXAudio2SourceVoice_SubmitSourceBuffer( src, &xbuf, NULL );
#else
    hr = S_OK;
#endif
    zero( &state, sizeof(state) );
    IXAudio2SourceVoice_GetState( src, &state, 0 );
    out_hr( "hr", hr );
    out( " queued=" ); out_dec( state.BuffersQueued );
    out( " played=" ); out_dec( (ULONG)state.SamplesPlayed );
    verdict( SUCCEEDED(hr) && state.BuffersQueued == 1 &&
             state.SamplesPlayed == 0,
             "the buffer did not reach the voice's queue" );

    /* ---- 8: the mixer consumes it ------------------------------------- */
    {
        DWORD start;
        BOOL played = FALSE;

        begin( "Start, and samples are actually played" );
        hr = IXAudio2SourceVoice_Start( src, 0, XAUDIO2_COMMIT_NOW );
        start = GetTickCount();
        while (SUCCEEDED(hr) && GetTickCount() - start < 3000)
        {
            Sleep( 20 );
            zero( &state, sizeof(state) );
            IXAudio2SourceVoice_GetState( src, &state, 0 );
            if (state.SamplesPlayed > 0) { played = TRUE; break; }
        }
        /* The COUNT is a function of when the poll ran and is deliberately not
         * printed; that the mixer moved at all is the claim. */
        out_hr( "hr", hr );
        out( " advanced=" ); out( played ? "yes" : "no" );
        verdict( SUCCEEDED(hr) && played,
                 "no sample was ever played -- the mixer is not running "
                 "against a device" );
    }

    /* ---- 9-10: the floating-point boundary ---------------------------- */
    {
        float v = 0.0f, ratio = 0.0f;

        begin( "SetVolume/GetVolume round-trip (float in XMM1)" );
        hr = IXAudio2SourceVoice_SetVolume( src, 0.375f, XAUDIO2_COMMIT_NOW );
        IXAudio2SourceVoice_GetVolume( src, &v );
        out_hr( "hr", hr );
        out( " bits=0x" ); out_hex( fbits( v ), 8 );
        out( " want=0x" ); out_hex( fbits( 0.375f ), 8 );
        verdict( SUCCEEDED(hr) && fbits( v ) == fbits( 0.375f ),
                 "a by-value float did not survive the boundary" );

        /* 2.0f is exactly the MaxFrequencyRatio asked for in step 5.  XAudio2
         * clamps a ratio to that maximum, so this value comes back unchanged
         * only if the fourth argument of CreateSourceVoice -- the one that
         * travelled on the guest's stack -- actually arrived. */
        begin( "SetFrequencyRatio at the maximum asked for in step 5" );
        hr = IXAudio2SourceVoice_SetFrequencyRatio( src, 2.0f, XAUDIO2_COMMIT_NOW );
        IXAudio2SourceVoice_GetFrequencyRatio( src, &ratio );
        out_hr( "hr", hr );
        out( " bits=0x" ); out_hex( fbits( ratio ), 8 );
#if XA_SMOKE_BREAK == 2
        out( " want=0x" ); out_hex( fbits( 99.0f ), 8 );
        verdict( SUCCEEDED(hr) && fbits( ratio ) == fbits( 99.0f ),
                 "the stack-float check is wired to its own expectation" );
#else
        out( " want=0x" ); out_hex( fbits( 2.0f ), 8 );
        verdict( SUCCEEDED(hr) && fbits( ratio ) == fbits( 2.0f ),
                 "the frequency ratio was clamped below the maximum this "
                 "voice was created with -- CreateSourceVoice's stack float "
                 "did not arrive" );
#endif
    }

    /* ---- 11: the hand-written slot's SERVED case ---------------------- */
    begin( "SetOutputVoices(NULL) -- route to the mastering voice" );
    hr = IXAudio2SourceVoice_SetOutputVoices( src, NULL );
    out_hr( "hr", hr );
    verdict( SUCCEEDED(hr), "the default routing was refused" );

    /* ---- 12: stop and flush -------------------------------------------
     *
     * The queue depth is PRINTED and diffed rather than asserted to be zero.
     * FAudio still reports the finished end-of-stream buffer as queued at this
     * point, on the native leg as much as on the guest one -- that is the
     * implementation's business and not the boundary's, and a gate that
     * asserted otherwise would be testing FAudio.  What the boundary owes is
     * that both calls are served and that the number comes back the SAME on
     * both legs, which the identity check is. */
    begin( "Stop and FlushSourceBuffers" );
    hr = IXAudio2SourceVoice_Stop( src, 0, XAUDIO2_COMMIT_NOW );
    if (SUCCEEDED(hr)) hr = IXAudio2SourceVoice_FlushSourceBuffers( src );
    zero( &state, sizeof(state) );
    IXAudio2SourceVoice_GetState( src, &state, 0 );
    out_hr( "hr", hr );
    out( " queued=" ); out_dec( state.BuffersQueued );
    verdict( SUCCEEDED(hr), "Stop or FlushSourceBuffers was refused" );

    /* ---- 13-14: the reverse-proxy direction, now SERVED (see the banner) - */
    begin( "IXAudio2::RegisterForCallbacks (a real IXAudio2EngineCallback)" );
    hr = IXAudio2_RegisterForCallbacks( xa2, &engine_cb );
    out_hr( "hr", hr );
    verdict( SUCCEEDED(hr), "RegisterForCallbacks was refused" );

    begin( "IXAudio2::CreateSourceVoice with a real IXAudio2VoiceCallback" );
    hr = IXAudio2_CreateSourceVoice( xa2, &cbvoice, &wfx, 0, 1.0f,
                                      &voice_cb, NULL, NULL );
    out_hr( "hr", hr );
    out( " obj=" ); out( cbvoice ? "yes" : "no" );
    verdict( SUCCEEDED(hr) && cbvoice != NULL, "no callback-bearing source voice" );
    if (!cbvoice) goto done;

    /* ---- 15-18: the delivery test -- the point of the whole exercise ---- */
    {
        XAUDIO2_BUFFER xbuf2;
        DWORD wait_start, wall_ms, waited;

        begin( "SubmitSourceBuffer with a distinctive pContext" );
        zero( &xbuf2, sizeof(xbuf2) );
        xbuf2.AudioBytes = CB_PCM_BYTES;
        xbuf2.pAudioData = pcm;
        xbuf2.Flags      = XAUDIO2_END_OF_STREAM;
        xbuf2.pContext   = &cb_marker;
#if XA_SMOKE_BREAK != 3
        hr = IXAudio2SourceVoice_SubmitSourceBuffer( cbvoice, &xbuf2, NULL );
#else
        hr = S_OK;
#endif
        zero( &state, sizeof(state) );
        IXAudio2SourceVoice_GetState( cbvoice, &state, 0 );
        out_hr( "hr", hr );
        out( " queued=" ); out_dec( state.BuffersQueued );
        verdict( SUCCEEDED(hr) && state.BuffersQueued == 1,
                 "the callback-bearing buffer did not reach the voice's queue" );

        begin( "Start, and wait (bounded) for OnBufferEnd" );
        hr = IXAudio2SourceVoice_Start( cbvoice, 0, XAUDIO2_COMMIT_NOW );
        wait_start = GetTickCount();
        waited = SUCCEEDED(hr)
            ? WaitForSingleObject( buffer_end_event, 5000 ) : WAIT_FAILED;
        wall_ms = GetTickCount() - wait_start;
        out_hr( "hr", hr );
        out( " signalled=" ); out( waited == WAIT_OBJECT_0 ? "yes" : "no" );
        verdict( SUCCEEDED(hr) && waited == WAIT_OBJECT_0,
                 "OnBufferEnd never fired -- a bounded wait timed out rather "
                 "than hanging" );

        begin( "OnBufferEnd delivered the exact pBufferContext" );
#if XA_SMOKE_BREAK != 4
        out( "match=" ); out( buffer_end_context == &cb_marker ? "yes" : "no" );
        verdict( buffer_end_context == &cb_marker,
                 "pBufferContext did not survive the boundary unchanged" );
#else
        out( "match=" ); out( buffer_end_context == (void *)&wfx ? "yes" : "no" );
        verdict( buffer_end_context == (void *)&wfx,
                 "the delivery check is wired to its own expectation" );
#endif

        begin( "delivery ordering: OnBufferStart before OnBufferEnd, a pass was seen" );
        out( "order=" );
        out( (buffer_start_seq != 0 && buffer_end_seq != 0 &&
              buffer_start_seq < buffer_end_seq) ? "ok" : "wrong" );
        out( " pass_seen=" ); out( voice_pass_start_count > 0 ? "yes" : "no" );
        verdict( buffer_start_seq != 0 && buffer_end_seq != 0 &&
                 buffer_start_seq < buffer_end_seq && voice_pass_start_count > 0,
                 "the callback ordering the mixer guarantees was not observed" );

        /* Not diffed, and deliberately so -- see the banner's MIXER-THREAD
         * ENTRY COST section.  This number is SUPPOSED to differ between the
         * legs. */
        report_mixer_entry_cost( wall_ms );
    }

    /* ---- 19: UnregisterForCallbacks -- the interning claim --------------- */
    {
        LONG after1, after2;

        begin( "UnregisterForCallbacks with the pointer that was registered" );
        /* UnregisterForCallbacks is `void` in the IDL (see the banner), so
         * there is no HRESULT here to check.  The stronger, checkable claim is
         * that the engine callback actually STOPS: sample twice with a pause
         * between, well after the call, and require both samples equal --
         * frozen, not merely slower. */
        IXAudio2_UnregisterForCallbacks( xa2, &engine_cb );
        Sleep( 300 );
        after1 = engine_pass_start_count;
        Sleep( 300 );
        after2 = engine_pass_start_count;
        out( "stopped=" ); out( (after1 == after2) ? "yes" : "no" );
        verdict( engine_pass_start_count > 0 && after1 == after2 &&
                 engine_critical_error_count == 0,
                 "the engine callback kept firing after UnregisterForCallbacks "
                 "-- the same registration was not found" );
    }

    /* ---- 20-22: AN XAPO EFFECT, WHICH IS A SECOND KIND OF OBJECT --------
     *
     * Everything above reaches XAudio2 through XAudio2Create and the voices it
     * vends.  An effect is different: CreateAudioReverb is a FLAT export that
     * hands back an object, and until IXAPO and IXAPOParameters were rostered
     * there was nothing to wrap it as -- so the three XAPO factories were
     * EXCLUDEd from the guest thunk and a guest asking for a reverb bound the
     * 0xdead0000 sentinel and faulted by name.  These steps are the proof that
     * a guest instantiating an audio effect now reaches native code.
     *
     * The reverb is created and interrogated but never attached to a voice: an
     * effect chain is carried INSIDE XAUDIO2_EFFECT_CHAIN, which this surface
     * still refuses by name (see dlls/xaudio2_7/guestcom.c), and a probe should
     * not pretend to test something the port says it does not do.  What is
     * tested is the whole of what the factories now serve: the object arrives,
     * both of its interfaces answer, and a method on it returns real data.
     */
    {
        IUnknown *reverb = NULL;
        IXAPO *xapo = NULL;
        IXAPOParameters *xapop = NULL;
        XAPO_REGISTRATION_PROPERTIES *props = NULL;
        HRESULT hr2;

        begin( "CreateAudioReverb (a flat export that vends an object)" );
        hr = CreateAudioReverb( &reverb );
        out_hr( "hr", hr );
        out( " obj=" ); out( reverb ? "yes" : "no" );
        verdict( SUCCEEDED(hr) && reverb != NULL,
                 "the reverb factory returned nothing" );

        begin( "QueryInterface(IID_IXAPO) + GetRegistrationProperties" );
        if (reverb)
        {
            hr = IUnknown_QueryInterface( reverb, &xa_IID_IXAPO, (void **)&xapo );
            if (SUCCEEDED(hr) && xapo)
                hr = IXAPO_GetRegistrationProperties( xapo, &props );
        }
        else hr = E_FAIL;
        out_hr( "hr", hr );
        if (SUCCEEDED(hr) && props)
        {
            /* Values, not just success.  These are the effect describing
             * itself, computed on the native side and read back through the
             * boundary; the two legs must agree on every one of them.  The
             * name is checked only for being non-empty because it is FAudio's
             * string and not this port's to pin. */
            out( " flags=" );      out_hex( props->Flags, 8 );
            out( " in=" );         out_dec( props->MinInputBufferCount );
            out( ".." );           out_dec( props->MaxInputBufferCount );
            out( " out=" );        out_dec( props->MinOutputBufferCount );
            out( ".." );           out_dec( props->MaxOutputBufferCount );
            out( " named=" );      out( props->FriendlyName[0] ? "yes" : "no" );
        }
        verdict( SUCCEEDED(hr) && xapo != NULL && props != NULL &&
                 props->FriendlyName[0] != 0 &&
                 props->MaxInputBufferCount >= props->MinInputBufferCount &&
                 props->MaxOutputBufferCount >= props->MinOutputBufferCount,
                 "the effect did not describe itself through IXAPO" );
        /* props is FAudio's allocation handed straight through by
         * dlls/xaudio2_7/xapo.c (it does not copy), and no deallocator for it
         * is exported on this surface.  Freeing it with the wrong allocator
         * would be worse than not freeing it in a probe that is about to
         * exit, so it is deliberately left alone and said so here. */

        begin( "QueryInterface(IID_IXAPOParameters) -- the second rostered interface" );
#if XA_SMOKE_BREAK == 5
        /* Ask the effect for an interface nothing implements.  The object must
         * answer E_NOINTERFACE and this step must go red -- which is what
         * shows the S_OK it normally reports is a CHECK and not a constant.
         * The bogus IID is IID_IXAPOParameters with its first byte changed. */
        {
            static const GUID bogus =
                { 0x27d95c66, 0x80f2, 0x499a,
                  { 0xad, 0x54, 0x5a, 0xe7, 0xf0, 0x1c, 0x6d, 0x98 } };
            hr2 = reverb ? IUnknown_QueryInterface( reverb, &bogus, (void **)&xapop )
                         : E_FAIL;
        }
#else
        if (reverb)
            hr2 = IUnknown_QueryInterface( reverb, &xa_IID_IXAPOParameters,
                                           (void **)&xapop );
        else hr2 = E_FAIL;
#endif
        out_hr( "hr", hr2 );
        out( " obj=" ); out( xapop ? "yes" : "no" );
        verdict( SUCCEEDED(hr2) && xapop != NULL,
                 "the effect did not answer IXAPOParameters" );

        if (xapop) IXAPOParameters_Release( xapop );
        if (xapo) IXAPO_Release( xapo );
        if (reverb) IUnknown_Release( reverb );
    }

    /* ---- 23: it all comes apart ---------------------------------------- */
    {
        ULONG refs;

        begin( "destroy the voices and release the engine" );
        if (cbvoice) IXAudio2SourceVoice_DestroyVoice( cbvoice );
        cbvoice = NULL;
        IXAudio2SourceVoice_DestroyVoice( src );
        src = NULL;
        IXAudio2MasteringVoice_DestroyVoice( master );
        master = NULL;
        IXAudio2_StopEngine( xa2 );
        refs = IXAudio2_Release( xa2 );
        xa2 = NULL;
        out( "final_refs=" ); out_dec( refs );
        verdict( refs == 0, "the engine did not reach zero references" );
    }

done:
    if (cbvoice) IXAudio2SourceVoice_DestroyVoice( cbvoice );
    if (src) IXAudio2SourceVoice_DestroyVoice( src );
    if (master) IXAudio2MasteringVoice_DestroyVoice( master );
    if (xa2) IXAudio2_Release( xa2 );
    if (buffer_end_event) CloseHandle( buffer_end_event );

    out( failures ? "xa_smoke: FAIL " : "xa_smoke: PASS " );
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

#if defined(XA_SMOKE_NATIVE)
int main( void )
{
    return xa_smoke_run();
}
#else
void WINAPI xa_smoke_entry( void )
{
    ExitProcess( (UINT)xa_smoke_run() );
}
#endif
