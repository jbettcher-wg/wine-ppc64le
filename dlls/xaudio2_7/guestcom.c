/*
 * XAudio2 for x86-64 guests on native ppc64le Wine -- the winecom runtime
 * instance, the [local]-interface dispatcher, the hand-written slots and the
 * flat-export wrapper.
 *
 * NOTHING HERE IMPLEMENTS XAUDIO2.  Wine's own xaudio_dll.c and the FAudio it
 * is built on are the implementation, unmodified, and they reach the machine
 * through mmdevapi exactly as they do for a native caller.  This file is the
 * boundary.  Read dlls/dsound/guestcom.c first: that is the same arrangement
 * without the two things XAudio2 adds.
 *
 * THING ONE: INTERFACES THAT ARE NOT COM.  IXAudio2Voice and the three
 * interfaces derived from it -- source, submix and mastering voices -- are
 * declared `[local]` in include/xaudio2.idl and are NOT IUnknown-derived.
 * They have no QueryInterface, no AddRef, no Release and no IID; slot 0 is
 * GetVoiceDetails.  libs/winecom's dispatcher serves slots 0..2 of every
 * interface from the proxy table, which for a voice would answer
 * GetVoiceDetails with QueryInterface -- so this module CLAIMS those
 * interfaces in __wine_com_dispatch and serves them itself, from the same
 * generated tables, before winecom_dispatch ever sees them.  The generated
 * header's xaudio2_iface_local[] says which, and gen_winecom.py refuses to
 * emit a [local] interface at all unless the surface is marked as claiming
 * them.  Their roster IIDs are SYNTHETIC (gen_interfaces.py's synth_iid): a
 * private key for the attach cross-check between this module's table and the
 * guest thunk's, never compared against anything outside the two.
 *
 * THING TWO: A VOICE IS NOT REFERENCE-COUNTED.  It is destroyed by
 * DestroyVoice, not by Release, so a voice proxy's reference never falls to
 * zero and libs/winecom never calls Release on a voice -- with one exception
 * that has to be closed here.  winecom_wrap() drops a SURPLUS reference by
 * invoking slot 2 when it interns a (host, interface) pair it already holds,
 * and slot 2 of a voice is SetEffectChain.  That can only happen if a
 * destroyed voice's address is reused by a new voice of the same kind, which
 * the allocator will do sooner or later.  So this module keeps a registry of
 * live voice hosts -- entered when a voice is wrapped, removed when
 * DestroyVoice runs -- and xaudio2_invoke refuses slot 2 on a registered
 * voice, loudly.  The registry is small (it holds live voices only) and it is
 * the only piece of state here that is not generated.
 *
 * THING THREE: THE CALLBACKS, AND THE THREAD THEY ARRIVE ON.  XAudio2's whole
 * reporting model is an interface the APPLICATION implements:
 * IXAudio2EngineCallback for engine-wide events, IXAudio2VoiceCallback for a
 * voice's buffer boundaries.  Both used to be refused here, because serving
 * them means native code calling an x86-64 vtable.  libs/winecom/reverse.c
 * builds the mirror now, both interfaces are on the roster, and both are
 * served -- so RegisterForCallbacks goes through the ordinary marshal table
 * and CreateSourceVoice's pCallback goes through the hand slot below.
 *
 * The thread is what makes this surface different from every other consumer of
 * the mechanism.  Media Foundation invokes a callback on a work-queue thread
 * where a millisecond costs nothing.  XAudio2 invokes OnBufferEnd from its
 * MIXER thread, which has a period to hit, and every one of those calls now
 * enters the emulator.  The port's position is CORRECT FIRST: a guest callback
 * that runs late may glitch, and a guest callback that never runs is a game
 * that deadlocks waiting for it.  ppc64le/audio/check-audio-smoke.sh measures
 * the entry cost so the number is a fact rather than a worry.
 *
 * Reverse proxies for these two are PERMANENT, and that is not a leak: both
 * interfaces are [local], so they have no AddRef for XAudio2 to take and the
 * object's lifetime is the application's business exactly as on Windows.  They
 * are interned by (guest pointer, interface), which is also what makes
 * UnregisterForCallbacks find the registration -- it is handed the same guest
 * pointer and it must reach the same native one.
 *
 * WHAT IS STILL REFUSED, AND WHY IT IS NOT LAZINESS:
 *
 *   * XAUDIO2_VOICE_SENDS and XAUDIO2_EFFECT_CHAIN carry IXAudio2Voice* and
 *     IUnknown* members INSIDE a struct.  A send list holds proxies the guest
 *     got FROM us and wants unwrapping, not reverse-proxying; an effect chain
 *     holds XAPO objects the GUEST implemented, which needs a reverse proxy
 *     built from a struct FIELD rather than from an argument position.  Note
 *     that IXAPO and IXAPOParameters are on the roster now -- that is what
 *     lets the three factories at the end of this file hand a guest an effect
 *     THIS module created -- so what is missing here is specifically the
 *     struct-carried reverse direction, not the interface.
 *     Both are refused BY NAME, and the three creators stay hand-written so
 *     that the NULL case -- which is what a game that pushes buffers and polls
 *     actually passes -- is fully served.
 *
 * Copyright 2026 the ppc64le port authors
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 */

#include <stdarg.h>

#include "ntstatus.h"
#define WIN32_NO_STATUS

#include "windef.h"
#include "winbase.h"
#include "winternl.h"
#include "wine/debug.h"
#include "wine/winecom.h"

#include <xaudio_classes.h>

#include "xaudio2_marshal.h"

WINE_DEFAULT_DEBUG_CHANNEL(xaudio2);

/* Declared here rather than by including include/xaudio2.h: that header is
 * generated at ONE version for everybody, and this module's declarations come
 * from its own widl run (xaudio_classes.h).  The prototype is xaudio_dll.c's. */
HRESULT WINAPI XAudio2Create( IXAudio2 **ppxa2, UINT32 flags, XAUDIO2_PROCESSOR proc );
HRESULT WINAPI XAudio2CreateWithVersionInfo( IXAudio2 **ppxa2, UINT32 flags,
                                             XAUDIO2_PROCESSOR proc, DWORD version );
/* The XAPO factories, declared here for the same reason: their prototypes live
 * in include/xapofx.h and include/xapo.h, which this module does not include
 * because those too are generated once for everybody.  These are xapo.c's and
 * xapofx.c's own lines, copied -- CreateFX is CDECL and the other two are
 * WINAPI, exactly as the .spec says. */
HRESULT WINAPI CreateAudioReverb( IUnknown **out );
HRESULT WINAPI CreateAudioVolumeMeter( IUnknown **out );
HRESULT CDECL  CreateFX( REFCLSID clsid, IUnknown **out, void *initdata,
                         UINT32 initdata_bytes );

/* ------------------------------------------------ the live-voice registry */

static CRITICAL_SECTION voice_cs;
static CRITICAL_SECTION_DEBUG voice_cs_debug =
{
    0, 0, &voice_cs,
    { &voice_cs_debug.ProcessLocksList, &voice_cs_debug.ProcessLocksList },
      0, 0, { (DWORD_PTR)(__FILE__ ": xaudio2 voice_cs") }
};
static CRITICAL_SECTION voice_cs = { &voice_cs_debug, -1, 0, 0, 0, 0 };

static void **voices;
static UINT voice_count, voice_capacity;

static void voice_remember( void *host )
{
    RtlEnterCriticalSection( &voice_cs );
    if (voice_count == voice_capacity)
    {
        UINT want = voice_capacity ? voice_capacity * 2 : 32;
        void **grown = voices
            ? RtlReAllocateHeap( GetProcessHeap(), 0, voices, want * sizeof(*voices) )
            : RtlAllocateHeap( GetProcessHeap(), 0, want * sizeof(*voices) );
        if (!grown)
        {
            RtlLeaveCriticalSection( &voice_cs );
            ERR( "out of memory registering voice %p; its slot-2 guard is off\n",
                 host );
            return;
        }
        voices = grown;
        voice_capacity = want;
    }
    voices[voice_count++] = host;
    RtlLeaveCriticalSection( &voice_cs );
}

static void voice_forget( void *host )
{
    UINT i;

    RtlEnterCriticalSection( &voice_cs );
    for (i = 0; i < voice_count; i++)
        if (voices[i] == host)
        {
            voices[i] = voices[--voice_count];
            break;
        }
    RtlLeaveCriticalSection( &voice_cs );
}

static BOOL voice_known( void *host )
{
    BOOL found = FALSE;
    UINT i;

    RtlEnterCriticalSection( &voice_cs );
    for (i = 0; i < voice_count && !found; i++) found = (voices[i] == host);
    RtlLeaveCriticalSection( &voice_cs );
    return found;
}

/* ------------------------------------------------------- the host invoker */

static UINT64 xaudio2_invoke( void *host, UINT slot, UINT argc, UINT64 *args )
{
    void **vtbl = *(void ***)host;

    /* THE SLOT-2 GUARD (see the banner), NOW A BACKSTOP.  libs/winecom used to
     * drop a surplus reference by invoking slot 2 without knowing what kind of
     * interface it held; on a voice that slot is SetEffectChain, and a voice
     * has no reference to drop in the first place.  winecom knows now
     * (host_release_iface, keyed on WINECOM_IF_LOCAL), so this should never
     * fire again -- and it is kept precisely because if it does, it is saying
     * something the shared layer got wrong, in the one place that can tell. */
    if (slot == 2 && voice_known( host ))
    {
        ERR( "refusing a reference-drop on voice %p: a voice is destroyed by "
             "DestroyVoice, and its slot 2 is SetEffectChain.  libs/winecom "
             "should have known this interface is [local] and not asked\n",
             host );
        return 0;
    }

    args[0] = (UINT64)(ULONG_PTR)host;
    return ((UINT64 (*)( ULONG_PTR, ULONG_PTR, ULONG_PTR, ULONG_PTR, ULONG_PTR,
                         ULONG_PTR, ULONG_PTR, ULONG_PTR, ULONG_PTR, ULONG_PTR,
                         ULONG_PTR, ULONG_PTR, ULONG_PTR, ULONG_PTR, ULONG_PTR,
                         ULONG_PTR ))vtbl[slot])
        ( args[0], args[1], args[2],  args[3],  args[4],  args[5],  args[6],
          args[7], args[8], args[9],  args[10], args[11], args[12], args[13],
          args[14], args[15] );
}

static void *host_slot( void *host, UINT slot )
{
    return (*(void ***)host)[slot];
}

/* MS-x64 assigns registers BY POSITION: argument n travels in XMMn when n < 4
 * and it is floating point, and on the stack after that -- in the low half of
 * the eight-byte slot an integer would have used.  Same rule, same code, as
 * dlls/dsound/guestcom.c. */
static float read_float_arg( const AMD64_CONTEXT *ctx, UINT n )
{
    /* the lazy-ctx contract (wine/winecom.h): the FP group may not be there
     * until asked for; idempotent, so once per argument is merely honest */
    __wine_emu_materialize_ctx( (AMD64_CONTEXT *)ctx );
    if (n < 4) return *(const float *)&ctx->FltSave.XmmRegisters[n];
    return *(const float *)(ULONG_PTR)(ctx->Rsp + 8 + n * (UINT64)8);
}

/* Wrap a voice the implementation just created and enter it in the registry.
 * winecom_wrap consumes a host reference, which a voice does not have and does
 * not need: nothing will ever release it, because a voice's vtable has no
 * Release and this module claims every one of its slots. */
static void *wrap_voice( void *host, UINT iface )
{
    if (!host) return NULL;
    voice_remember( host );
    return winecom_wrap( host, iface );
}

static HRESULT refuse_reverse( const char *method, const char *what, const void *p )
{
    FIXME( "xaudio2: %s is refused because %s (%p) is a GUEST-implemented "
           "object handed to native code -- the reverse-proxy direction "
           "(system-com-design.md 6) this port does not have yet\n",
           method, what, p );
    return E_NOTIMPL;
}

/* ------------------------------------------------------ hand-written slots */

/* IXAudio2::CreateSourceVoice( IXAudio2SourceVoice **, const WAVEFORMATEX *,
 *     UINT32 Flags, float MaxFrequencyRatio, IXAudio2VoiceCallback *,
 *     const XAUDIO2_VOICE_SENDS *, const XAUDIO2_EFFECT_CHAIN * )
 *
 * MaxFrequencyRatio is argument 4, so it is past XMM3 and arrives on the
 * guest's stack -- the case a host that read every float out of an XMM
 * register gets wrong with a number rather than a crash. */
static UINT64 hand_create_source_voice( void *host, UINT slot, AMD64_CONTEXT *ctx )
{
    HRESULT (WINAPI *fn)( void *, IXAudio2SourceVoice **, const WAVEFORMATEX *,
                          UINT32, float, IXAudio2VoiceCallback *,
                          const XAUDIO2_VOICE_SENDS *,
                          const XAUDIO2_EFFECT_CHAIN * ) = host_slot( host, slot );
    IXAudio2SourceVoice **out =
        (IXAudio2SourceVoice **)(ULONG_PTR)winecom_read_arg( ctx, 1 );
    const void *cb    = (const void *)(ULONG_PTR)winecom_read_arg( ctx, 5 );
    const void *sends = (const void *)(ULONG_PTR)winecom_read_arg( ctx, 6 );
    const void *chain = (const void *)(ULONG_PTR)winecom_read_arg( ctx, 7 );
    void *cb_host = NULL;
    HRESULT hr;

    if (cb && !winecom_to_native( (void *)cb, XAUDIO2_IFACE_IXAudio2VoiceCallback,
                                  &cb_host ))
    {
        FIXME( "xaudio2: CreateSourceVoice could not give the "
               "IXAudio2VoiceCallback at %p a reverse proxy; refusing rather "
               "than handing the mixer thread an x86-64 vtable\n", cb );
        return (UINT64)(UINT)E_NOTIMPL;
    }
    if (sends)
        return (UINT64)(UINT)refuse_reverse( "IXAudio2::CreateSourceVoice",
                 "its XAUDIO2_VOICE_SENDS, whose descriptors carry "
                 "IXAudio2Voice pointers", sends );
    if (chain)
        return (UINT64)(UINT)refuse_reverse( "IXAudio2::CreateSourceVoice",
                 "its XAUDIO2_EFFECT_CHAIN, whose descriptors carry IUnknown "
                 "pointers", chain );

    /* NOTE what is NOT here: a matching winecom_to_native_end.  XAudio2 keeps
     * pCallback for the voice's whole life and never AddRefs it, because
     * IXAudio2VoiceCallback is [local] and HAS no AddRef -- slot 1 of that
     * vtable is OnVoiceProcessingPassEnd.  A reverse proxy for a [local]
     * interface is permanent for exactly that reason (libs/winecom/reverse.c
     * rev_release), so there is no borrow to give back and giving one back
     * would be the bug. */
    hr = fn( host, out,
             (const WAVEFORMATEX *)(ULONG_PTR)winecom_read_arg( ctx, 2 ),
             (UINT32)winecom_read_arg( ctx, 3 ),
             read_float_arg( ctx, 4 ), cb_host, NULL, NULL );
    if (SUCCEEDED(hr) && out)
        *out = wrap_voice( *out, XAUDIO2_IFACE_IXAudio2SourceVoice );
    return (UINT64)(UINT)hr;
}

/* IXAudio2::CreateSubmixVoice( IXAudio2SubmixVoice **, UINT32, UINT32, UINT32,
 *     UINT32, const XAUDIO2_VOICE_SENDS *, const XAUDIO2_EFFECT_CHAIN * ) */
static UINT64 hand_create_submix_voice( void *host, UINT slot, AMD64_CONTEXT *ctx )
{
    HRESULT (WINAPI *fn)( void *, IXAudio2SubmixVoice **, UINT32, UINT32,
                          UINT32, UINT32, const XAUDIO2_VOICE_SENDS *,
                          const XAUDIO2_EFFECT_CHAIN * ) = host_slot( host, slot );
    IXAudio2SubmixVoice **out =
        (IXAudio2SubmixVoice **)(ULONG_PTR)winecom_read_arg( ctx, 1 );
    const void *sends = (const void *)(ULONG_PTR)winecom_read_arg( ctx, 6 );
    const void *chain = (const void *)(ULONG_PTR)winecom_read_arg( ctx, 7 );
    HRESULT hr;

    if (sends)
        return (UINT64)(UINT)refuse_reverse( "IXAudio2::CreateSubmixVoice",
                 "its XAUDIO2_VOICE_SENDS, whose descriptors carry "
                 "IXAudio2Voice pointers", sends );
    if (chain)
        return (UINT64)(UINT)refuse_reverse( "IXAudio2::CreateSubmixVoice",
                 "its XAUDIO2_EFFECT_CHAIN, whose descriptors carry IUnknown "
                 "pointers", chain );

    hr = fn( host, out, (UINT32)winecom_read_arg( ctx, 2 ),
             (UINT32)winecom_read_arg( ctx, 3 ),
             (UINT32)winecom_read_arg( ctx, 4 ),
             (UINT32)winecom_read_arg( ctx, 5 ), NULL, NULL );
    if (SUCCEEDED(hr) && out)
        *out = wrap_voice( *out, XAUDIO2_IFACE_IXAudio2SubmixVoice );
    return (UINT64)(UINT)hr;
}

/* IXAudio2::CreateMasteringVoice( IXAudio2MasteringVoice **, UINT32, UINT32,
 *     UINT32, LPCWSTR DeviceId, const XAUDIO2_EFFECT_CHAIN *,
 *     AUDIO_STREAM_CATEGORY )
 *
 * DeviceId crosses as an ordinary address: unlike the DXVK lane, both sides
 * here are Wine PE code built with -fshort-wchar, so a WCHAR is two bytes on
 * both. */
static UINT64 hand_create_mastering_voice( void *host, UINT slot, AMD64_CONTEXT *ctx )
{
    HRESULT (WINAPI *fn)( void *, IXAudio2MasteringVoice **, UINT32, UINT32,
                          UINT32, LPCWSTR, const XAUDIO2_EFFECT_CHAIN *,
                          AUDIO_STREAM_CATEGORY ) = host_slot( host, slot );
    IXAudio2MasteringVoice **out =
        (IXAudio2MasteringVoice **)(ULONG_PTR)winecom_read_arg( ctx, 1 );
    const void *chain = (const void *)(ULONG_PTR)winecom_read_arg( ctx, 6 );
    HRESULT hr;

    if (chain)
        return (UINT64)(UINT)refuse_reverse( "IXAudio2::CreateMasteringVoice",
                 "its XAUDIO2_EFFECT_CHAIN, whose descriptors carry IUnknown "
                 "pointers", chain );

    hr = fn( host, out, (UINT32)winecom_read_arg( ctx, 2 ),
             (UINT32)winecom_read_arg( ctx, 3 ),
             (UINT32)winecom_read_arg( ctx, 4 ),
             (LPCWSTR)(ULONG_PTR)winecom_read_arg( ctx, 5 ), NULL,
             (AUDIO_STREAM_CATEGORY)winecom_read_arg( ctx, 7 ) );
    if (SUCCEEDED(hr) && out)
        *out = wrap_voice( *out, XAUDIO2_IFACE_IXAudio2MasteringVoice );
    return (UINT64)(UINT)hr;
}

/* IXAudio2Voice::SetOutputVoices( const XAUDIO2_VOICE_SENDS * ).  NULL means
 * "route to the mastering voice", which is a real and common request and is
 * served; a real send list carries IXAudio2Voice pointers and is refused. */
static UINT64 hand_set_output_voices( void *host, UINT slot, AMD64_CONTEXT *ctx )
{
    HRESULT (WINAPI *fn)( void *, const XAUDIO2_VOICE_SENDS * ) =
        host_slot( host, slot );
    const void *sends = (const void *)(ULONG_PTR)winecom_read_arg( ctx, 1 );

    if (sends)
        return (UINT64)(UINT)refuse_reverse( "IXAudio2Voice::SetOutputVoices",
                 "its XAUDIO2_VOICE_SENDS, whose descriptors carry "
                 "IXAudio2Voice pointers", sends );
    return (UINT64)(UINT)fn( host, NULL );
}

/* IXAudio2Voice::SetEffectChain( const XAUDIO2_EFFECT_CHAIN * ).  NULL removes
 * the chain and is served; a real chain carries IUnknown pointers. */
static UINT64 hand_set_effect_chain( void *host, UINT slot, AMD64_CONTEXT *ctx )
{
    HRESULT (WINAPI *fn)( void *, const XAUDIO2_EFFECT_CHAIN * ) =
        host_slot( host, slot );
    const void *chain = (const void *)(ULONG_PTR)winecom_read_arg( ctx, 1 );

    if (chain)
        return (UINT64)(UINT)refuse_reverse( "IXAudio2Voice::SetEffectChain",
                 "its XAUDIO2_EFFECT_CHAIN, whose descriptors carry IUnknown "
                 "pointers", chain );
    return (UINT64)(UINT)fn( host, NULL );
}

/* (this, float, UINT32) -> HRESULT.  IXAudio2Voice::SetVolume and
 * IXAudio2SourceVoice::SetFrequencyRatio. */
static UINT64 hand_f_i( void *host, UINT slot, AMD64_CONTEXT *ctx )
{
    HRESULT (WINAPI *fn)( void *, float, UINT32 ) = host_slot( host, slot );

    return (UINT64)(UINT)fn( host, read_float_arg( ctx, 1 ),
                             (UINT32)winecom_read_arg( ctx, 2 ) );
}

/* No slot on this surface has these shapes; they exist because the shape table
 * in gen_winecom.py is shared with the DirectSound surface and its indices are
 * fixed.  Reaching one is a generator bug, so it says so. */
static UINT64 hand_unused_shape( void *host, UINT slot, AMD64_CONTEXT *ctx )
{
    ERR( "no slot on the xaudio2 surface has this float shape (host %p slot "
         "%u ctx %p) -- gen_winecom.py routed one here by mistake\n",
         host, slot, ctx );
    return (UINT64)(UINT)E_NOTIMPL;
}

/* The order here IS hand_funcs[] order in xaudio2_marshal.h. */
static const winecom_hand_fn xaudio2_hand_funcs[] =
{
    hand_create_source_voice,
    hand_create_submix_voice,
    hand_create_mastering_voice,
    hand_set_output_voices,
    hand_set_effect_chain,
    hand_unused_shape,        /* hand_ffffff_i: six floats, DirectSound only */
    hand_unused_shape,        /* hand_fff_i:  three floats, DirectSound only */
    hand_f_i,
};

C_ASSERT( ARRAYSIZE(xaudio2_hand_funcs) == XAUDIO2_HAND_COUNT );

/* ------------------------------------------------- the runtime instance
 *
 * THE ONLY VERSION-DEPENDENT LINES IN THIS FILE, which is why it lives in
 * xaudio2_7 and is reached by both 2_8 and 2_9 through their PARENTSRC rather
 * than copied.  Everything above -- the marshal dispatch, the hand slots, the
 * reverse-proxy plumbing -- is identical for the two versions, because the
 * ROSTERS are the same shape (8 interfaces, 115 slots); what differs between
 * them is IXAudio2's IID and the argument lists of three of its methods, and
 * all of that is data in the generated xaudio2_marshal.h each module compiles
 * against its own copy of.  Sharing the code and NOT the table is the whole
 * point: a second copy of 609 lines would drift.
 *
 * Spelled as an explicit per-version block rather than token-pasting
 * XAUDIO2_VER into a name, so that the set of versions this file serves is
 * something you can read.  2_7 is deliberately absent: it exports no creator
 * at all, so a 2.7 guest arrives through CoCreateInstance in combase's
 * winecom instance instead, and there is nothing for this surface to serve
 * (see the banner in dlls/xaudio2_9/xaudio2_9.thunks).
 */

#if XAUDIO2_VER == 9
# define XAUDIO2_GUEST_MODULE  L"xaudio2_9.dll"
# define XAUDIO2_SURFACE_NAME  "xaudio2_9"
#elif XAUDIO2_VER == 8
# define XAUDIO2_GUEST_MODULE  L"xaudio2_8.dll"
# define XAUDIO2_SURFACE_NAME  "xaudio2_8"
#else
# error "this winecom surface serves xaudio2_8 and xaudio2_9 only; see the comment above"
#endif

static const WCHAR *const xaudio2_guest_modules[] = { XAUDIO2_GUEST_MODULE };

static const struct winecom_surface xaudio2_surface =
{
    .name = XAUDIO2_SURFACE_NAME,
    .guest_modules = xaudio2_guest_modules,
    .module_count = ARRAYSIZE(xaudio2_guest_modules),
    .ifaces = xaudio2_com_ifaces,
    .iface_count = XAUDIO2_IFACE_COUNT,
    .invoke = xaudio2_invoke,
    .hand_funcs = xaudio2_hand_funcs,
    .hand_count = XAUDIO2_HAND_COUNT,
    /* THE REVERSE DIRECTION IS ON.  xaudio2_invoke calls a native PE vtable
     * directly, so a REVERSE PROXY -- a PE-side object whose slots enter guest
     * code through the emulator -- is a thing FAudio can be handed and call.
     * It will call it from the MIXER THREAD, which is where this surface
     * differs from every other consumer of the mechanism; see the banner. */
    .flags = WINECOM_SF_REVERSE,
};

static BOOL xaudio2_com_ready( void )
{
    return winecom_attach( &xaudio2_surface );
}

/* ------------------------------- the [local]-interface dispatcher (thing one)
 *
 * The same loop libs/winecom runs, minus the one thing that cannot apply: the
 * IUnknown special case for slots 0..2.  It uses the SAME generated tables, so
 * a slot's argument classes are stated exactly once whichever dispatcher
 * serves it, and it reaches proxies through the exported winecom_unwrap /
 * winecom_translate_in / winecom_wrap, so a voice handed back as
 * pDestinationVoice is recognised by the same intern table.  Only the argument
 * classes that occur on this surface are implemented; anything else refuses
 * loudly rather than passing a value it has not classified. */

static UINT refuse_logged[XAUDIO2_IFACE_COUNT];   /* one bit per slot */

static void refuse_once( UINT iface, UINT slot, const char *name, const char *why )
{
    if (slot < 32)
    {
        if (refuse_logged[iface] & (1u << slot)) return;
        refuse_logged[iface] |= 1u << slot;
    }
    FIXME( "xaudio2: refusing %s (iface %u slot %u): %s\n", name, iface, slot,
           why ? why : "no marshal plan" );
}

static NTSTATUS local_dispatch( UINT iface, UINT slot, AMD64_CONTEXT *ctx )
{
    const struct winecom_iface *itf = &xaudio2_com_ifaces[iface];
    const struct winecom_slot *sl;
    UINT out_static_idx[4], n_out_static = 0;
    UINT64 args[16] = { 0 };
    UINT64 ret;
    void *host;
    UINT i, n;

    if (slot >= itf->slot_count) return STATUS_INVALID_PARAMETER;
    sl = &itf->slots[slot];

    if (!(host = winecom_unwrap( (void *)(ULONG_PTR)ctx->R10 )))
    {
        ERR( "%s slot %u called on %p, which is not one of our proxies\n",
             itf->name, slot, (void *)(ULONG_PTR)ctx->R10 );
        ctx->Rax = (UINT)E_INVALIDARG;
        return STATUS_SUCCESS;
    }

    if (sl->refuse)
    {
        refuse_once( iface, slot, sl->name, sl->refuse );
        ctx->Rax = (UINT)E_NOTIMPL;
        return STATUS_SUCCESS;
    }

    /* DestroyVoice ends this host's life as a voice, so its registry entry
     * goes before the call rather than after: after the call the object is
     * gone and the pointer is only good for comparing. */
    if (slot == XAUDIO2_SLOT_IXAudio2Voice_DestroyVoice) voice_forget( host );

    if (sl->flags & WINECOM_F_HAND)
    {
        ctx->Rax = xaudio2_hand_funcs[sl->aux]( host, slot, ctx );
        return STATUS_SUCCESS;
    }

    args[0] = (UINT64)(ULONG_PTR)host;
    for (i = 1; i < sl->argc; i++)
    {
        UINT64 raw = winecom_read_arg( ctx, i );

        switch (sl->cls ? sl->cls[i - 1] : WINECOM_CA_PASS)
        {
        case WINECOM_CA_PASS:
            args[i] = raw;
            break;
        case WINECOM_CA_IFACE_IN:
        {
            /* One of our proxies unwraps to its host; a guest-IMPLEMENTED
             * object gets a REVERSE proxy of the type the generated table
             * recorded in xaux.  IXAudio2Voice::SetOutputVoices' destination
             * is the first kind, IXAudio2::RegisterForCallbacks' engine
             * callback is the second. */
            void *in_host;
            if (!winecom_to_native( (void *)(ULONG_PTR)raw,
                                    sl->xaux ? sl->xaux[i - 1] : ~0u, &in_host ))
            {
                refuse_once( iface, slot, sl->name,
                             "an in-parameter this surface cannot translate; "
                             "the generated table records no interface type "
                             "for it" );
                ctx->Rax = (UINT)E_NOTIMPL;
                return STATUS_SUCCESS;
            }
            args[i] = (UINT64)(ULONG_PTR)in_host;
            break;
        }
        case WINECOM_CA_IFACE_OUT_STATIC:
            args[i] = raw;
            if (raw && n_out_static < ARRAYSIZE(out_static_idx))
                out_static_idx[n_out_static++] = i;
            break;
        default:
            refuse_once( iface, slot, sl->name,
                         "argument class with no marshal path in this "
                         "module's [local] dispatcher" );
            ctx->Rax = (UINT)E_NOTIMPL;
            return STATUS_SUCCESS;
        }
    }

    ret = xaudio2_invoke( host, slot, sl->argc, args );

    for (n = 0; n < n_out_static; n++)
    {
        void **out = (void **)(ULONG_PTR)args[out_static_idx[n]];
        if (out && *out)
            *out = wrap_voice( *out, sl->xaux[out_static_idx[n] - 1] );
    }

    ctx->Rax = (sl->flags & WINECOM_F_RET_VOID) ? 0 : ret;
    return STATUS_SUCCESS;
}

NTSTATUS WINAPI __wine_com_dispatch( UINT iface, UINT slot, AMD64_CONTEXT *ctx )
{
    if (!xaudio2_com_ready()) return STATUS_DLL_INIT_FAILED;
    if (iface >= XAUDIO2_IFACE_COUNT) return STATUS_INVALID_PARAMETER;
    if (xaudio2_iface_local[iface]) return local_dispatch( iface, slot, ctx );
    return winecom_dispatch( iface, slot, ctx );
}

/* The crossing-frequency sink's name lookup; see winecom_slot_names.  Never on
 * a dispatch path -- ntdll asks once per slot, when it interns the row. */
BOOL WINAPI __wine_com_slot_name( UINT iface, UINT slot, const char **iface_name,
                                  const char **slot_name )
{
    return winecom_slot_names( iface, slot, iface_name, slot_name );
}

HRESULT WINAPI __wine_com_refuse(void)
{
    ERR( "xaudio2: refusing an interface-bearing flat export with no wrapper "
         "(see the guest thunk trace for which)\n" );
    return E_NOTIMPL;
}

/* ---------------------------------------------------------- flat wrappers */

HRESULT WINAPI __wine_guest_XAudio2Create( IXAudio2 **out, UINT32 flags,
                                           XAUDIO2_PROCESSOR proc )
{
    HRESULT hr;

    if (!xaudio2_com_ready()) return E_FAIL;
    hr = XAudio2Create( out, flags, proc );
    if (SUCCEEDED(hr)) winecom_wrap_static( (void **)out, XAUDIO2_IFACE_IXAudio2 );
    return hr;
}

HRESULT WINAPI __wine_guest_XAudio2CreateWithVersionInfo( IXAudio2 **out, UINT32 flags,
                                                          XAUDIO2_PROCESSOR proc,
                                                          DWORD version )
{
    HRESULT hr;

    if (!xaudio2_com_ready()) return E_FAIL;
    hr = XAudio2CreateWithVersionInfo( out, flags, proc, version );
    if (SUCCEEDED(hr)) winecom_wrap_static( (void **)out, XAUDIO2_IFACE_IXAudio2 );
    return hr;
}

/* ------------------------------------------------- the XAPO factories
 *
 * The three effect factories, which xaudio2_9.thunks used to EXCLUDE -- so a
 * guest asking for a reverb bound a 0xdead0000 sentinel and faulted by name.
 * They are served now because IXAPO and IXAPOParameters are on the roster.
 *
 * WHY THE WRAP IS IXAPO AND NOT SOMETHING CLEVERER.  All three hand back
 * `IUnknown **`, but the pointer is not an anonymous IUnknown: dlls/
 * xaudio2_7/xapo.c writes `*out = (IUnknown *)&object->IXAPO_iface` in both
 * CreateAudioReverb and CreateAudioVolumeMeter, and xapofx.c's CreateFX
 * reaches the same object through its class factory.  So the vtable at that
 * address IS an IXAPO vtable, and wrapping it as anything else would line the
 * guest's stub array up against the wrong slot list.  IXAPOParameters is
 * reached the way COM says it is -- QueryInterface on the returned object,
 * which xapo.c answers from the same allocation -- and libs/winecom wraps
 * that result through the roster like any other interface-returning slot.
 *
 * WHY THEY ARE GUEST-IMPL RATHER THAN GUEST-REFUSE, and this is the part
 * xaudio2_9.thunks called a finding rather than a preference: spec2thunk's
 * flat-surface audit classifies a parameter as interface-bearing when it
 * names a ROSTERED interface, a known carrier struct or a bare void**, and
 * `IUnknown **ppApo` was none of those.  It still is none of those -- the
 * audit's blind spot is not closed by this change and is recorded as open in
 * dlls/xaudio2_9/xaudio2_9.thunks -- but with a GUEST-IMPL there is nothing
 * for the audit to be wrong about: the redirect is explicit, the wrap is
 * written here by hand, and a guest never sees the native pointer at all.
 *
 * CreateFX is CDECL, unlike the other two, because its .spec line is -- it
 * takes four arguments and the trailing two are pass-through.  The
 * XAUDIO2_VER split is xapofx.c's own (the 2.7 form takes two arguments);
 * this file only serves 2_8 and 2_9, both of which have the four-argument
 * form, so there is no conditional here.
 */

HRESULT WINAPI __wine_guest_CreateAudioReverb( IUnknown **out )
{
    HRESULT hr;

    if (!xaudio2_com_ready()) return E_FAIL;
    hr = CreateAudioReverb( out );
    if (SUCCEEDED(hr)) winecom_wrap_static( (void **)out, XAUDIO2_IFACE_IXAPO );
    return hr;
}

HRESULT WINAPI __wine_guest_CreateAudioVolumeMeter( IUnknown **out )
{
    HRESULT hr;

    if (!xaudio2_com_ready()) return E_FAIL;
    hr = CreateAudioVolumeMeter( out );
    if (SUCCEEDED(hr)) winecom_wrap_static( (void **)out, XAUDIO2_IFACE_IXAPO );
    return hr;
}

HRESULT CDECL __wine_guest_CreateFX( REFCLSID clsid, IUnknown **out,
                                     void *initdata, UINT32 initdata_bytes )
{
    HRESULT hr;

    if (!xaudio2_com_ready()) return E_FAIL;
    hr = CreateFX( clsid, out, initdata, initdata_bytes );
    if (SUCCEEDED(hr)) winecom_wrap_static( (void **)out, XAUDIO2_IFACE_IXAPO );
    return hr;
}
