/*
 * System COM for x86-64 guests on native ppc64le Wine -- the combase-side
 * runtime instance and the flat-export wrappers.
 *
 * This is the native half of the system-COM boundary
 * (hangover-ppc64le/docs/system-com-design.md).  Nothing here replaces any
 * Wine implementation: combase/ole32/oleaut32 and every object they vend
 * are Wine's own, and the flat FROM-SPEC thunks already reach them
 * correctly.  Only interface POINTERS crossing to the guest are wrong, and
 * this file is the wrapping layer that fixes them:
 *
 *   * it holds THE ONE winecom runtime instance for the whole system-COM
 *     surface (static-library state is per-linkee; combase is the module
 *     every other COM DLL imports, so the instance lives here and
 *     ole32/oleaut32 reach it through the exported __wine_com_* helpers and
 *     a spec forward of __wine_com_dispatch);
 *
 *   * the host invoker is a DIRECT widest-form native vtable call -- the
 *     implementations are ordinary native PE code in the same Win32 world,
 *     so there is no unixlib on this surface (§4.2).  That one function
 *     pointer is the entire difference from d3d12's invoker;
 *
 *   * the flat wrappers (__wine_guest_*) call the real native export through
 *     an ordinary internal call and wrap/translate interface pointers at
 *     the classified positions.  spec2thunk's GUEST-IMPL redirect points the
 *     guest export's native resolution at the wrapper (§4.3); the build-time
 *     flat-surface audit refuses to generate if any interface-bearing flat
 *     export is left unclassified.
 *
 * THE AUDIO FAMILY, and why it is HERE rather than in an audio module.
 * XAudio2 2.7 exports no creator at all: a 2.7 application -- DOOM (2016) and
 * everything else built against the 2010-era DirectX redistributable -- reaches
 * the engine only through CoCreateInstance( CLSID_XAudio2 ), and so does every
 * user of WASAPI through CoCreateInstance( CLSID_MMDeviceEnumerator ).  Both
 * land here, so both must be wrapped by THIS instance, from THIS roster.
 * dlls/xaudio2_9's instance cannot take them over: its roster is the 2.9 SHAPE
 * (2.7's IXAudio2 has three extra methods at the head of its vtable and a
 * different CreateMasteringVoice), and a winecom instance is per-linkee, so a
 * proxy it made would trap into ITS __wine_com_dispatch and be interned in a
 * table this module cannot see.  ppc64le/syscom/gen_syscom_audio.py says the
 * same thing at greater length and generates the tables.
 *
 * That decision carries a cost, and it is the two things
 * dlls/xaudio2_9/guestcom.c explains at length, reproduced below for the 2.7
 * shape because they are properties of XAudio2 and not of that module:
 *
 *   * IXAudio2Voice and its three derivatives are `[local]` and NOT
 *     IUnknown-derived -- slot 0 is GetVoiceDetails, not QueryInterface -- so
 *     this module CLAIMS them in __wine_com_dispatch and serves them from the
 *     same generated tables before winecom_dispatch can serve slot 0 as QI;
 *   * a voice is destroyed by DestroyVoice, not by Release, so the surplus-
 *     reference drop winecom_wrap() makes on a re-interned (host, interface)
 *     pair would call SetEffectChain.  A registry of live voice hosts closes it.
 *
 * THE REVERSE DIRECTION IS ON for this surface (WINECOM_SF_REVERSE), and that
 * is the third thing XAudio2 forces and the one this file used to refuse.  Both
 * of the objects a 2.7 title reaches through CoCreateInstance report through an
 * interface the APPLICATION implements -- IXAudio2EngineCallback for the engine,
 * IMMNotificationClient for the endpoint list -- so RegisterForCallbacks and
 * RegisterEndpointNotificationCallback are the whole point of those APIs and
 * used to answer E_NOTIMPL.  libs/winecom/reverse.c builds the native vtable
 * that enters guest code, both interfaces are on the roster, and the ordinary
 * marshal path serves all four slots.  The refusals that REMAIN below are about
 * SIGNATURES rather than about direction -- a struct that reaches an interface
 * pointer through its own members, an interface this roster does not carry --
 * and they stand whichever way the call is going.
 *
 * What makes the flip legitimate HERE and not everywhere: syscom_invoke calls a
 * native PE vtable directly (there is no unixlib on this surface), so a reverse
 * proxy -- a PE-side object whose slots enter the emulator -- is a thing the
 * implementation can be handed and can call.  A surface whose invoker crossed a
 * unixlib must never set the flag; include/wine/winecom.h says why.
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
#include "objbase.h"
#include "oleauto.h"
#include "winternl.h"
#include "wine/debug.h"
#include "wine/winecom.h"

#include "syscom_marshal.h"

WINE_DEFAULT_DEBUG_CHANNEL(combase);

/* ------------------------------------------------ the live-voice registry
 *
 * Entered when a voice is wrapped, removed when DestroyVoice runs.  It exists
 * for one reason: winecom_wrap() drops a SURPLUS reference by invoking slot 2
 * when it interns a (host, interface) pair it already holds, and slot 2 of a
 * voice is SetEffectChain.  That can only happen once a destroyed voice's
 * address is reused by a new voice of the same kind, which the allocator will
 * do sooner or later.  Same registry, same reason, as
 * dlls/xaudio2_9/guestcom.c's. */

static CRITICAL_SECTION voice_cs;
static CRITICAL_SECTION_DEBUG voice_cs_debug =
{
    0, 0, &voice_cs,
    { &voice_cs_debug.ProcessLocksList, &voice_cs_debug.ProcessLocksList },
      0, 0, { (DWORD_PTR)(__FILE__ ": syscom voice_cs") }
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

/* --------------------------------------- the registered-notification registry
 *
 * WHAT IT GUARDS, and it is not the same hazard as the voice registry above.
 * mmdevapi never DEREFERENCES an IMMNotificationClient it is handed: register
 * stores the pointer in a list, unregister walks the list COMPARING pointers
 * and answers E_NOTFOUND if it is not there (dlls/mmdevapi/devenum.c).  So on
 * Windows, and on native Wine, unregistering something that was never
 * registered is a well-defined no-op on any address at all.
 *
 * Translating that address is not.  A guest-implemented object becomes a
 * reverse proxy, and building one begins by calling the object's own AddRef
 * through the emulator -- which for an address that is not a COM object is a
 * jump into whatever it points at.  So this surface remembers what it actually
 * registered, and the unregister slot answers E_NOTFOUND -- mmdevapi's own
 * answer, byte for byte -- for anything else, without touching it.
 *
 * The entries are GUEST pointers, because that is what both slots are handed
 * and what libs/winecom interns a reverse proxy by. */

static CRITICAL_SECTION notify_cs;
static CRITICAL_SECTION_DEBUG notify_cs_debug =
{
    0, 0, &notify_cs,
    { &notify_cs_debug.ProcessLocksList, &notify_cs_debug.ProcessLocksList },
      0, 0, { (DWORD_PTR)(__FILE__ ": syscom notify_cs") }
};
static CRITICAL_SECTION notify_cs = { &notify_cs_debug, -1, 0, 0, 0, 0 };

static void **notify_clients;
static UINT notify_count, notify_capacity;

static void notify_remember( void *guest )
{
    RtlEnterCriticalSection( &notify_cs );
    if (notify_count == notify_capacity)
    {
        UINT want = notify_capacity ? notify_capacity * 2 : 8;
        void **grown = notify_clients
            ? RtlReAllocateHeap( GetProcessHeap(), 0, notify_clients,
                                 want * sizeof(*notify_clients) )
            : RtlAllocateHeap( GetProcessHeap(), 0, want * sizeof(*notify_clients) );
        if (!grown)
        {
            RtlLeaveCriticalSection( &notify_cs );
            ERR( "out of memory registering notification client %p; it will "
                 "not be unregisterable\n", guest );
            return;
        }
        notify_clients = grown;
        notify_capacity = want;
    }
    notify_clients[notify_count++] = guest;
    RtlLeaveCriticalSection( &notify_cs );
}

static BOOL notify_forget( void *guest )
{
    BOOL found = FALSE;
    UINT i;

    RtlEnterCriticalSection( &notify_cs );
    for (i = 0; i < notify_count; i++)
        if (notify_clients[i] == guest)
        {
            notify_clients[i] = notify_clients[--notify_count];
            found = TRUE;
            break;
        }
    RtlLeaveCriticalSection( &notify_cs );
    return found;
}

/* ------------------------------------------------------- the host invoker */

/* A direct widest-form native vtable call: host's vtable slot with up to 16
 * ULONG_PTR arguments (args[0] is `this`).  ELFv2 callees ignore the excess,
 * so one shape serves every slot -- the same trick call_native_thunk uses.
 * No unixlib: these are ordinary native COM objects. */
static UINT64 syscom_invoke( void *host, UINT slot, UINT argc, UINT64 *args )
{
    void **vtbl = *(void ***)host;

    /* THE SLOT-2 GUARD (see the voice registry above).  winecom_host_release()
     * invokes slot 2 to drop a surplus reference; on a voice that slot is
     * SetEffectChain, and a voice has no reference to drop in the first
     * place. */
    if (slot == 2 && voice_known( host ))
    {
        ERR( "refusing a reference-drop on voice %p: a voice is destroyed by "
             "DestroyVoice, and its slot 2 is SetEffectChain\n", host );
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

/* ------------------------------------------------------ hand-written slots
 *
 * Each of these takes something no static argument class can express: a struct
 * that reaches an interface pointer through its own members, a callback the
 * APPLICATION implements, or a by-value float.  They are hand-written rather
 * than refused outright so that the NULL case -- which is what a program that
 * just plays a sound or opens the default endpoint actually passes -- is fully
 * served, and only the rest is refused BY NAME.
 *
 * The XAudio2 declarations are spelled with void* rather than taken from
 * <xaudio2.h> ON PURPOSE: include/xaudio2.h is generated once at
 * XAUDIO2_VER=9, and the object reached from here is 2.7-shaped.  Every
 * argument these functions touch is either passed straight through or tested
 * against NULL, so an opaque pointer states exactly as much as is known. */

static void *host_slot( void *host, UINT slot )
{
    return (*(void ***)host)[slot];
}

/* MS-x64 assigns registers BY POSITION: argument n travels in XMMn when n < 4
 * and it is floating point, and on the stack after that -- in the low half of
 * the eight-byte slot an integer would have used.  Same rule, same code, as
 * dlls/xaudio2_9/guestcom.c. */
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

/* The refusal the STRUCT-BEARING arguments get, and it is not about direction.
 * XAUDIO2_VOICE_SENDS and XAUDIO2_EFFECT_CHAIN reach interface pointers through
 * their own members: the descriptors inside a send list are voices the guest got
 * FROM us and wants unwrapping, the ones inside an effect chain are XAPO objects
 * of an interface this roster does not carry.  Neither is a pointer any static
 * argument class describes, so both are refused by name whichever way the call
 * is going -- exactly as dlls/xaudio2_9/guestcom.c refuses them with the
 * reverse direction fully built. */
static HRESULT refuse_bearing( const char *method, const char *what, const void *p )
{
    FIXME( "xaudio2: %s is refused because %s (%p) reaches an interface pointer "
           "through its own members, which no argument class describes and no "
           "direction fixes\n", method, what, p );
    return E_NOTIMPL;
}

/* IXAudio2::CreateSourceVoice( IXAudio2SourceVoice **, const WAVEFORMATEX *,
 *     UINT32 Flags, float MaxFrequencyRatio, IXAudio2VoiceCallback *,
 *     const XAUDIO2_VOICE_SENDS *, const XAUDIO2_EFFECT_CHAIN * )
 *
 * MaxFrequencyRatio is argument 4, so it is past XMM3 and arrives on the
 * guest's stack -- the case a host that read every float out of an XMM
 * register gets wrong with a number rather than a crash. */
static UINT64 hand_create_source_voice( void *host, UINT slot, AMD64_CONTEXT *ctx )
{
    HRESULT (WINAPI *fn)( void *, void **, const void *, UINT32, float,
                          void *, const void *, const void * ) = host_slot( host, slot );
    void **out        = (void **)(ULONG_PTR)winecom_read_arg( ctx, 1 );
    const void *cb    = (const void *)(ULONG_PTR)winecom_read_arg( ctx, 5 );
    const void *sends = (const void *)(ULONG_PTR)winecom_read_arg( ctx, 6 );
    const void *chain = (const void *)(ULONG_PTR)winecom_read_arg( ctx, 7 );
    void *cb_host = NULL;
    HRESULT hr;

    /* pCallback is a GUEST-implemented object handed to native code, so it
     * needs the REVERSE direction: a native vtable, built from this
     * interface's slot table, whose stubs enter guest code.  This is the same
     * call dlls/xaudio2_9/guestcom.c makes for the 2.9 shape, against THIS
     * module's roster and THIS module's winecom instance -- a proxy is
     * per-linkee, and one minted by the audio module would trap into a
     * dispatch table combase cannot see.
     *
     * The refusal that stood here said the interface was off the roster
     * because the hand slot had not been rewritten.  It has been now, and it
     * is rewritten rather than generated for the same reason it always was:
     * the two struct arguments below have no argument class, so this function
     * names the callback's interface index itself instead of reading one out
     * of a generated row. */
    if (cb && !winecom_to_native( (void *)cb, SYSCOM_IFACE_IXAudio2VoiceCallback,
                                  &cb_host ))
    {
        FIXME( "xaudio2: IXAudio2::CreateSourceVoice could not give the "
               "IXAudio2VoiceCallback at %p a reverse proxy; refusing rather "
               "than handing the mixer thread an x86-64 vtable\n", cb );
        return (UINT64)(UINT)E_NOTIMPL;
    }
    if (sends)
        return (UINT64)(UINT)refuse_bearing( "IXAudio2::CreateSourceVoice",
                 "its XAUDIO2_VOICE_SENDS, whose descriptors carry "
                 "IXAudio2Voice pointers", sends );
    if (chain)
        return (UINT64)(UINT)refuse_bearing( "IXAudio2::CreateSourceVoice",
                 "its XAUDIO2_EFFECT_CHAIN, whose descriptors carry IUnknown "
                 "pointers", chain );

    /* NOTE what is NOT here: a matching winecom_to_native_end.  XAudio2 keeps
     * pCallback for the voice's whole life and never AddRefs it, because
     * IXAudio2VoiceCallback is [local] and HAS no AddRef -- slot 1 of that
     * vtable is OnVoiceProcessingPassEnd, not Release.  libs/winecom's
     * rev_release() returns early for a [local] interface for exactly that
     * reason, so the proxy is permanent: there is no borrow to give back, and
     * giving one back would be the bug.  Freeing it would hand the mixer
     * thread a dangling native vtable at the next buffer boundary. */
    hr = fn( host, out, (const void *)(ULONG_PTR)winecom_read_arg( ctx, 2 ),
             (UINT32)winecom_read_arg( ctx, 3 ),
             read_float_arg( ctx, 4 ), cb_host, NULL, NULL );
    if (SUCCEEDED(hr) && out)
        *out = wrap_voice( *out, SYSCOM_IFACE_IXAudio2SourceVoice );
    return (UINT64)(UINT)hr;
}

/* IXAudio2::CreateSubmixVoice( IXAudio2SubmixVoice **, UINT32, UINT32, UINT32,
 *     UINT32, const XAUDIO2_VOICE_SENDS *, const XAUDIO2_EFFECT_CHAIN * ) */
static UINT64 hand_create_submix_voice( void *host, UINT slot, AMD64_CONTEXT *ctx )
{
    HRESULT (WINAPI *fn)( void *, void **, UINT32, UINT32, UINT32, UINT32,
                          const void *, const void * ) = host_slot( host, slot );
    void **out        = (void **)(ULONG_PTR)winecom_read_arg( ctx, 1 );
    const void *sends = (const void *)(ULONG_PTR)winecom_read_arg( ctx, 6 );
    const void *chain = (const void *)(ULONG_PTR)winecom_read_arg( ctx, 7 );
    HRESULT hr;

    if (sends)
        return (UINT64)(UINT)refuse_bearing( "IXAudio2::CreateSubmixVoice",
                 "its XAUDIO2_VOICE_SENDS, whose descriptors carry "
                 "IXAudio2Voice pointers", sends );
    if (chain)
        return (UINT64)(UINT)refuse_bearing( "IXAudio2::CreateSubmixVoice",
                 "its XAUDIO2_EFFECT_CHAIN, whose descriptors carry IUnknown "
                 "pointers", chain );

    hr = fn( host, out, (UINT32)winecom_read_arg( ctx, 2 ),
             (UINT32)winecom_read_arg( ctx, 3 ),
             (UINT32)winecom_read_arg( ctx, 4 ),
             (UINT32)winecom_read_arg( ctx, 5 ), NULL, NULL );
    if (SUCCEEDED(hr) && out)
        *out = wrap_voice( *out, SYSCOM_IFACE_IXAudio2SubmixVoice );
    return (UINT64)(UINT)hr;
}

/* IXAudio2::CreateMasteringVoice( IXAudio2MasteringVoice **, UINT32, UINT32,
 *     UINT32, UINT32 index, const XAUDIO2_EFFECT_CHAIN * )
 *
 * THE 2.7 SHAPE, and this is where the version difference bites: 2.8 and later
 * take an LPCWSTR DeviceId here and a trailing AUDIO_STREAM_CATEGORY.  2.7
 * takes a device INDEX into IXAudio2::GetDeviceCount's list and stops at the
 * effect chain, which is why the roster is generated from
 * dlls/xaudio2_7/xaudio_classes.h and not from include/xaudio2.h. */
static UINT64 hand_create_mastering_voice( void *host, UINT slot, AMD64_CONTEXT *ctx )
{
    HRESULT (WINAPI *fn)( void *, void **, UINT32, UINT32, UINT32, UINT32,
                          const void * ) = host_slot( host, slot );
    void **out        = (void **)(ULONG_PTR)winecom_read_arg( ctx, 1 );
    const void *chain = (const void *)(ULONG_PTR)winecom_read_arg( ctx, 6 );
    HRESULT hr;

    if (chain)
        return (UINT64)(UINT)refuse_bearing( "IXAudio2::CreateMasteringVoice",
                 "its XAUDIO2_EFFECT_CHAIN, whose descriptors carry IUnknown "
                 "pointers", chain );

    hr = fn( host, out, (UINT32)winecom_read_arg( ctx, 2 ),
             (UINT32)winecom_read_arg( ctx, 3 ),
             (UINT32)winecom_read_arg( ctx, 4 ),
             (UINT32)winecom_read_arg( ctx, 5 ), NULL );
    if (SUCCEEDED(hr) && out)
        *out = wrap_voice( *out, SYSCOM_IFACE_IXAudio2MasteringVoice );
    return (UINT64)(UINT)hr;
}

/* IXAudio2Voice::SetOutputVoices( const XAUDIO2_VOICE_SENDS * ).  NULL means
 * "route to the mastering voice", which is a real and common request and is
 * served; a real send list carries IXAudio2Voice pointers and is refused. */
static UINT64 hand_set_output_voices( void *host, UINT slot, AMD64_CONTEXT *ctx )
{
    HRESULT (WINAPI *fn)( void *, const void * ) = host_slot( host, slot );
    const void *sends = (const void *)(ULONG_PTR)winecom_read_arg( ctx, 1 );

    if (sends)
        return (UINT64)(UINT)refuse_bearing( "IXAudio2Voice::SetOutputVoices",
                 "its XAUDIO2_VOICE_SENDS, whose descriptors carry "
                 "IXAudio2Voice pointers", sends );
    return (UINT64)(UINT)fn( host, NULL );
}

/* IXAudio2Voice::SetEffectChain( const XAUDIO2_EFFECT_CHAIN * ).  NULL removes
 * the chain and is served; a real chain carries IUnknown pointers. */
static UINT64 hand_set_effect_chain( void *host, UINT slot, AMD64_CONTEXT *ctx )
{
    HRESULT (WINAPI *fn)( void *, const void * ) = host_slot( host, slot );
    const void *chain = (const void *)(ULONG_PTR)winecom_read_arg( ctx, 1 );

    if (chain)
        return (UINT64)(UINT)refuse_bearing( "IXAudio2Voice::SetEffectChain",
                 "its XAUDIO2_EFFECT_CHAIN, whose descriptors carry IUnknown "
                 "pointers", chain );
    return (UINT64)(UINT)fn( host, NULL );
}

/* IMMDevice::Activate( REFIID, DWORD, PROPVARIANT *pActivationParams,
 *     void **ppv ).  pActivationParams is a PROPVARIANT, whose union has an
 * IUnknown* arm, so a non-NULL one can hand native code a guest object; every
 * caller that is activating an IAudioClient passes NULL.  The result goes
 * through winecom_wrap_out_iface, which is the fail-closed choke point: an IID
 * this roster does not carry is released and answered E_NOINTERFACE by name
 * rather than handed over as a native vtable. */
static UINT64 hand_mmdevice_activate( void *host, UINT slot, AMD64_CONTEXT *ctx )
{
    HRESULT (WINAPI *fn)( void *, const GUID *, DWORD, void *, void ** ) =
        host_slot( host, slot );
    const GUID *iid = (const GUID *)(ULONG_PTR)winecom_read_arg( ctx, 1 );
    void *params    = (void *)(ULONG_PTR)winecom_read_arg( ctx, 3 );
    void **ppv      = (void **)(ULONG_PTR)winecom_read_arg( ctx, 4 );
    HRESULT hr;

    if (params)
        return (UINT64)(UINT)refuse_bearing( "IMMDevice::Activate",
                 "its PROPVARIANT pActivationParams, whose union carries an "
                 "IUnknown pointer", params );

    hr = fn( host, iid, (DWORD)winecom_read_arg( ctx, 2 ), NULL, ppv );
    return (UINT64)(UINT)winecom_wrap_out_iface( hr, iid, ppv );
}

/* IMMDeviceEnumerator::RegisterEndpointNotificationCallback(
 *     IMMNotificationClient * )
 *
 * THE REVERSE DIRECTION, through the same door dlls/xaudio2_9/guestcom.c uses:
 * winecom_to_native turns the guest's object into a native one, which is a
 * reverse proxy when the guest implemented it and the original host pointer
 * when the guest is handing back something it got from us.
 *
 * NOTE WHAT IS NOT HERE, and it is the same note CreateSourceVoice's pCallback
 * carries in the 2.9 module: no matching winecom_to_native_end on the success
 * path.  mmdevapi keeps the pointer for the whole life of the registration and
 * never AddRefs it, so the borrow this call took IS the registration and giving
 * it back would free the proxy under a notification thread.  It goes back in
 * the unregister slot below, which is the only place the registration ends. */
static UINT64 hand_mmdev_register_notify( void *host, UINT slot, AMD64_CONTEXT *ctx )
{
    HRESULT (WINAPI *fn)( void *, void * ) = host_slot( host, slot );
    void *client = (void *)(ULONG_PTR)winecom_read_arg( ctx, 1 );
    void *native = NULL;
    HRESULT hr;

    if (!client) return (UINT64)(UINT)fn( host, NULL );   /* mmdevapi: E_POINTER */

    if (!winecom_to_native( client, SYSCOM_IFACE_IMMNotificationClient, &native ))
    {
        FIXME( "syscom: RegisterEndpointNotificationCallback could not give the "
               "IMMNotificationClient at %p a reverse proxy; refusing rather "
               "than handing the notification thread an x86-64 vtable\n", client );
        return (UINT64)(UINT)E_NOTIMPL;
    }
    hr = fn( host, native );
    if (SUCCEEDED(hr)) notify_remember( client );
    else winecom_to_native_end( native );
    return (UINT64)(UINT)hr;
}

/* IMMDeviceEnumerator::UnregisterEndpointNotificationCallback(
 *     IMMNotificationClient * )
 *
 * A pointer this surface did not register is answered E_NOTFOUND -- which is
 * mmdevapi's own answer for a client that is not in its list -- WITHOUT being
 * translated, because translating it would enter its AddRef and mmdevapi's
 * contract lets it be an address that is not an object at all.  See the
 * registry above.
 *
 * A pointer it did register translates to the SAME native proxy the register
 * slot built, because reverse proxies are interned by (guest pointer,
 * interface); that identity is what lets mmdevapi's pointer comparison find the
 * registration.  Two references then go back: the borrow this call took, and
 * the one the registration was holding. */
static UINT64 hand_mmdev_unregister_notify( void *host, UINT slot, AMD64_CONTEXT *ctx )
{
    HRESULT (WINAPI *fn)( void *, void * ) = host_slot( host, slot );
    void *client = (void *)(ULONG_PTR)winecom_read_arg( ctx, 1 );
    void *native = NULL;
    HRESULT hr;

    if (!client) return (UINT64)(UINT)fn( host, NULL );   /* mmdevapi: E_POINTER */

    if (!notify_forget( client ))
    {
        TRACE( "%p was never registered through this surface; answering "
               "E_NOTFOUND without translating it\n", client );
        return (UINT64)(UINT)HRESULT_FROM_WIN32( ERROR_NOT_FOUND );
    }
    if (!winecom_to_native( client, SYSCOM_IFACE_IMMNotificationClient, &native ))
    {
        /* Cannot happen -- the register slot is the only way into the registry
         * and it interned the proxy -- but if it ever does, the registration is
         * still live in mmdevapi's list, so it stays live here too rather than
         * becoming a registration nothing can name. */
        notify_remember( client );
        ERR( "syscom: %p was registered and cannot be translated back; the "
             "registration cannot be removed\n", client );
        return (UINT64)(UINT)E_UNEXPECTED;
    }
    hr = fn( host, native );
    winecom_to_native_end( native );      /* the borrow this call took... */
    winecom_to_native_end( native );      /* ...and the one the registration held */
    return (UINT64)(UINT)hr;
}

/* (this, float, UINT32) -> HRESULT.  IXAudio2Voice::SetVolume and
 * IXAudio2SourceVoice::SetFrequencyRatio.  Routed by argument SHAPE rather
 * than by name (ppc64le/syscom/gen_syscom_audio.py's FP_SHAPES), so a new slot
 * with an existing shape is served automatically and a new shape is a named
 * refusal rather than a silent wrong-register call. */
static UINT64 hand_f_i( void *host, UINT slot, AMD64_CONTEXT *ctx )
{
    HRESULT (WINAPI *fn)( void *, float, UINT32 ) = host_slot( host, slot );

    return (UINT64)(UINT)fn( host, read_float_arg( ctx, 1 ),
                             (UINT32)winecom_read_arg( ctx, 2 ) );
}

/* The order here IS hand_funcs[] order in syscom_marshal.h. */
static const winecom_hand_fn syscom_hand_funcs[] =
{
    hand_create_source_voice,
    hand_create_submix_voice,
    hand_create_mastering_voice,
    hand_set_output_voices,
    hand_set_effect_chain,
    hand_mmdevice_activate,
    hand_mmdev_register_notify,
    hand_mmdev_unregister_notify,
    hand_f_i,
};

C_ASSERT( ARRAYSIZE(syscom_hand_funcs) == SYSCOM_HAND_COUNT );

/* ------------------------------------------------- the runtime instance */

static const WCHAR *const syscom_guest_modules[] =
{
    L"combase.dll", L"ole32.dll", L"oleaut32.dll",
};

static const struct winecom_surface syscom_surface =
{
    .name = "syscom",
    .guest_modules = syscom_guest_modules,
    .module_count = ARRAYSIZE(syscom_guest_modules),
    .ifaces = syscom_com_ifaces,
    .iface_count = SYSCOM_IFACE_COUNT,
    .invoke = syscom_invoke,
    .hand_funcs = syscom_hand_funcs,
    .hand_count = SYSCOM_HAND_COUNT,
    /* THE REVERSE DIRECTION IS ON; the banner says why it is legitimate here.
     * In one line: syscom_invoke calls a native PE vtable directly, so a
     * reverse proxy is a PE-side object the implementation can call, and the
     * two objects CoCreateInstance vends on this surface both report through
     * an interface the application implements. */
    .flags = WINECOM_SF_REVERSE,
};

static BOOL syscom_ready( void )
{
    return winecom_attach( &syscom_surface );
}

/* ------------------------------- the [local]-interface dispatcher
 *
 * The same loop libs/winecom runs, minus the one thing that cannot apply to a
 * voice: the IUnknown special case for slots 0..2.  It uses the SAME generated
 * tables, so a slot's argument classes are stated exactly once whichever
 * dispatcher serves it, and it reaches proxies through winecom_unwrap /
 * winecom_translate_in / winecom_wrap, so a voice handed back as
 * pDestinationVoice is recognised by the same intern table.  Only the argument
 * classes that occur on these four interfaces are implemented; anything else
 * refuses loudly rather than passing a value it has not classified.
 *
 * This is dlls/xaudio2_9/guestcom.c's local_dispatch(), for the 2.7 shape. */

static UINT refuse_logged[SYSCOM_IFACE_COUNT];   /* one bit per slot */

static void local_refuse_once( UINT iface, UINT slot, const char *name,
                               const char *why )
{
    if (slot < 32)
    {
        if (refuse_logged[iface] & (1u << slot)) return;
        refuse_logged[iface] |= 1u << slot;
    }
    FIXME( "syscom: refusing %s (iface %u slot %u): %s\n", name, iface, slot,
           why ? why : "no marshal plan" );
}

static NTSTATUS local_dispatch( UINT iface, UINT slot, AMD64_CONTEXT *ctx )
{
    const struct winecom_iface *itf = &syscom_com_ifaces[iface];
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
        local_refuse_once( iface, slot, sl->name, sl->refuse );
        ctx->Rax = (UINT)E_NOTIMPL;
        return STATUS_SUCCESS;
    }

    /* DestroyVoice ends this host's life as a voice, so its registry entry
     * goes before the call rather than after: after the call the object is
     * gone and the pointer is only good for comparing. */
    if (slot == SYSCOM_SLOT_IXAudio2Voice_DestroyVoice) voice_forget( host );

    if (sl->flags & WINECOM_F_HAND)
    {
        ctx->Rax = syscom_hand_funcs[sl->aux]( host, slot, ctx );
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
            /* The same classifier libs/winecom's own loop runs, read from the
             * same generated row: one of our proxies unwraps to its host, NULL
             * stays NULL, and a guest-IMPLEMENTED object gets a REVERSE proxy
             * of the type xaux records -- but ONLY when xmask says the
             * generator wrote that entry.  Without the bit, xaux[i] reads a
             * zero nobody wrote, which is a real roster index, so the parameter
             * fails closed and is refused by name.  That is what keeps the
             * reused interface blocks shut. */
            void *in_host;
            if (!winecom_to_native( (void *)(ULONG_PTR)raw,
                                    (sl->xaux && (sl->xmask & (1u << (i - 1))))
                                        ? sl->xaux[i - 1] : ~0u, &in_host ))
            {
                local_refuse_once( iface, slot, sl->name,
                             "guest-implemented object as an in-parameter whose "
                             "interface type the generated table does not "
                             "record (no xmask bit for it)" );
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
            local_refuse_once( iface, slot, sl->name,
                         "argument class with no marshal path in combase's "
                         "[local] dispatcher" );
            ctx->Rax = (UINT)E_NOTIMPL;
            return STATUS_SUCCESS;
        }
    }

    ret = syscom_invoke( host, slot, sl->argc, args );

    for (n = 0; n < n_out_static; n++)
    {
        void **out = (void **)(ULONG_PTR)args[out_static_idx[n]];
        if (out && *out)
            *out = wrap_voice( *out, sl->xaux[out_static_idx[n] - 1] );
    }

    ctx->Rax = (sl->flags & WINECOM_F_RET_VOID) ? 0 : ret;
    return STATUS_SUCCESS;
}

/* ---------------------------------------------------- exported dispatch */

NTSTATUS WINAPI __wine_com_dispatch( UINT iface, UINT slot, AMD64_CONTEXT *ctx )
{
    if (!syscom_ready()) return STATUS_DLL_INIT_FAILED;
    if (iface >= SYSCOM_IFACE_COUNT) return STATUS_INVALID_PARAMETER;
    if (syscom_iface_local[iface]) return local_dispatch( iface, slot, ctx );
    return winecom_dispatch( iface, slot, ctx );
}

/* The crossing-frequency sink's name lookup; see winecom_slot_names.  Never
 * on a dispatch path -- ntdll asks once per slot, when it interns the row. */
BOOL WINAPI __wine_com_slot_name( UINT iface, UINT slot, const char **iface_name,
                                  const char **slot_name )
{
    return winecom_slot_names( iface, slot, iface_name, slot_name );
}

/* The sibling-module helper API (§4.2): ole32 and oleaut32 wrappers reach
 * the single runtime instance through these forwards, never by re-linking
 * libwinecom (which would give them their own tables). */
void *WINAPI __wine_com_wrap( void *host, UINT iface )
{
    if (!syscom_ready()) return NULL;
    return winecom_wrap( host, iface );
}

void *WINAPI __wine_com_unwrap( void *proxy )
{
    return winecom_unwrap( proxy );
}

BOOL WINAPI __wine_com_translate_in( void *guest_seen, void **host_out )
{
    return winecom_translate_in( guest_seen, host_out );
}

HRESULT WINAPI __wine_com_wrap_out_iface( HRESULT hr, const GUID *riid, void **ppv )
{
    if (!syscom_ready()) return hr;
    return winecom_wrap_out_iface( hr, riid, ppv );
}

void WINAPI __wine_com_wrap_static( void **p, UINT iface )
{
    if (!syscom_ready()) return;
    winecom_wrap_static( p, iface );
}

UINT WINAPI __wine_com_iface_from_iid( const GUID *riid )
{
    return winecom_iface_from_iid( riid );
}

/* The sibling-module surface for a hand walker over a by-value aggregate that
 * may CARRY a forward proxy (system-com-design.md §9.2) -- VariantClear's
 * VT_UNKNOWN/VT_DISPATCH slot below, and PropVariantClear's mirror later.
 * Same no-readiness-check shape as __wine_com_unwrap above: a caller can only
 * have a genuine proxy pointer to pass here once winecom_wrap() already
 * succeeded once, which already required the runtime to be ready. */
ULONG WINAPI __wine_com_release_guest( void *ptr )
{
    return winecom_release_guest_seen( ptr );
}

ULONG WINAPI __wine_com_addref_guest( void *ptr )
{
    return winecom_addref_guest_seen( ptr );
}

/* The shared loud-refusal stub every GUEST-REFUSE export resolves to: a flat
 * export that vends or consumes interfaces but has no wrapper yet.  Returns
 * E_NOTIMPL (0 = NULL for the pointer/void-returning ones), never a
 * pass-through that would hand the guest a native vtable.  The trapping
 * export's own name is in the dispatcher TRACE. */
HRESULT WINAPI __wine_com_refuse(void)
{
    ERR( "syscom: refusing an interface-bearing flat export with no wrapper "
         "yet (see the guest thunk trace for which)\n" );
    return E_NOTIMPL;
}

/* ------------------------------------------------------- flat wrappers */
/* Each calls the real native export (declared here to avoid dragging the
 * whole combase private surface in) and wraps/translates at the classified
 * positions.  The interception is spec2thunk's GUEST-IMPL redirect, so the
 * guest still imports the plain export name. */

HRESULT WINAPI CoCreateInstance( REFCLSID, IUnknown *, DWORD, REFIID, void ** );
HRESULT WINAPI CoGetClassObject( REFCLSID, DWORD, COSERVERINFO *, REFIID, void ** );
HRESULT WINAPI CreateStreamOnHGlobal( HGLOBAL, BOOL, IStream ** );
HRESULT WINAPI GetHGlobalFromStream( IStream *, HGLOBAL * );

/* ---------------------------------------------- the XAudio2 version gate
 *
 * IID_IXAudio2 is {8bcf1f58-9fe7-4583-8ac6-e2adc465c8bb} for EVERY version of
 * XAudio2 up to and including 2.7 (include/xaudio2.idl), so the IID alone
 * cannot type the object: it says "an XAudio2 engine", not which vtable.  The
 * CLSID does, and it is the only thing that does.
 *
 * What actually differs, measured against xaudio2.idl's conditionals rather
 * than assumed: IXAudio2's own vtable is IDENTICAL across 2.0 to 2.7, but
 * `#if XAUDIO2_VER >= 4` adds SetOutputFilterParameters and
 * GetOutputFilterParameters to IXAudio2Voice and SetSourceSampleRate to
 * IXAudio2SourceVoice.  So an engine of 2.4 or later has exactly the vtables
 * this roster describes, and 2.0 to 2.3 have voice vtables two slots shorter --
 * against which the guest's own SetVolume call would land on
 * SetOutputFilterParameters.
 *
 * Hence: serve 2.4 and later, refuse the rest BY NAME here, at the only two
 * doors an XAudio2 coclass can come through.  Refusing at CoGetClassObject as
 * well is what makes the CoCreateInstance test sound: without it a guest could
 * take an old class object and call IClassFactory::CreateInstance, which is
 * dispatched inside winecom_dispatch with no CLSID anywhere in sight. */

struct xaudio2_class
{
    GUID clsid;
    int  ver;
    BOOL debug;
};

static const struct xaudio2_class xaudio2_classes[] =
{
    { {0xfac23f48,0x31f5,0x45a8,{0xb4,0x9b,0x52,0x25,0xd6,0x14,0x01,0xaa}}, 0, FALSE },
    { {0xfac23f48,0x31f5,0x45a8,{0xb4,0x9b,0x52,0x25,0xd6,0x14,0x01,0xdb}}, 0, TRUE  },
    { {0xe21a7345,0xeb21,0x468e,{0xbe,0x50,0x80,0x4d,0xb9,0x7c,0xf7,0x08}}, 1, FALSE },
    { {0xf7a76c21,0x53d4,0x46bb,{0xac,0x53,0x8b,0x45,0x9c,0xae,0x46,0xbd}}, 1, TRUE  },
    { {0xb802058a,0x464a,0x42db,{0xbc,0x10,0xb6,0x50,0xd6,0xf2,0x58,0x6a}}, 2, FALSE },
    { {0x97dfb7e7,0x5161,0x4015,{0x87,0xa9,0xc7,0x9e,0x6a,0x19,0x52,0xcc}}, 2, TRUE  },
    { {0x4c5e637a,0x16c7,0x4de3,{0x9c,0x46,0x5e,0xd2,0x21,0x81,0x96,0x2d}}, 3, FALSE },
    { {0xef0aa05d,0x8075,0x4e5d,{0xbe,0xad,0x45,0xbe,0x0c,0x3c,0xcb,0xb3}}, 3, TRUE  },
    { {0x03219e78,0x5bc3,0x44d1,{0xb9,0x2e,0xf6,0x3d,0x89,0xcc,0x65,0x26}}, 4, FALSE },
    { {0x4256535c,0x1ea4,0x4d4b,{0x8a,0xd5,0xf9,0xdb,0x76,0x2e,0xca,0x9e}}, 4, TRUE  },
    { {0x4c9b6dde,0x6809,0x46e6,{0xa2,0x78,0x9b,0x6a,0x97,0x58,0x86,0x70}}, 5, FALSE },
    { {0x715bdd1a,0xaa82,0x436b,{0xb0,0xfa,0x6a,0xce,0xa3,0x9b,0xd0,0xa1}}, 5, TRUE  },
    { {0x3eda9b49,0x2085,0x498b,{0x9b,0xb2,0x39,0xa6,0x77,0x84,0x93,0xde}}, 6, FALSE },
    { {0x47199894,0x7cc2,0x444d,{0x98,0x73,0xce,0xd2,0x56,0x2c,0xc6,0x0e}}, 6, TRUE  },
    { {0x5a508685,0xa254,0x4fba,{0x9b,0x82,0x9a,0x24,0xb0,0x03,0x06,0xaf}}, 7, FALSE },
    { {0xdb05ea35,0x0329,0x4d4b,{0xa5,0x3a,0x6d,0xea,0xd0,0x3d,0x38,0x52}}, 7, TRUE  },
};

/* S_OK = go ahead; anything else is the refusal the caller must answer. */
static HRESULT syscom_xaudio2_class_gate( REFCLSID rclsid )
{
    UINT i;

    if (!rclsid) return S_OK;
    for (i = 0; i < ARRAYSIZE(xaudio2_classes); i++)
    {
        if (!IsEqualGUID( rclsid, &xaudio2_classes[i].clsid )) continue;
        if (xaudio2_classes[i].ver >= 4)
        {
            TRACE( "XAudio2 2.%d%s: vtable-identical with the 2.7 roster\n",
                   xaudio2_classes[i].ver,
                   xaudio2_classes[i].debug ? " (debug)" : "" );
            return S_OK;
        }
        ERR( "syscom: refusing CLSID_XAudio2%s for XAudio2 2.%d: the roster "
             "describes the 2.7 vtables, and before 2.4 IXAudio2Voice has no "
             "SetOutputFilterParameters/GetOutputFilterParameters and "
             "IXAudio2SourceVoice no SetSourceSampleRate, so every voice call "
             "past slot 9 would land on the neighbouring method.  Serving this "
             "needs a second roster at that shape\n",
             xaudio2_classes[i].debug ? "Debug" : "", xaudio2_classes[i].ver );
        return REGDB_E_CLASSNOTREG;
    }
    return S_OK;
}

HRESULT WINAPI __wine_guest_CoCreateInstance( REFCLSID rclsid, IUnknown *outer,
                                              DWORD ctx, REFIID riid, void **ppv )
{
    HRESULT hr;

    if (!syscom_ready()) return E_FAIL;
    if ((hr = syscom_xaudio2_class_gate( rclsid )) != S_OK)
    {
        if (ppv) *ppv = NULL;
        return hr;
    }
    if (outer)
    {
        /* Aggregation hands a guest IUnknown to a coclass this call has not
         * even resolved yet, and an aggregated inner object delegates EVERY
         * QueryInterface to that outer pointer -- for any IID at all, including
         * ones this roster does not carry and cannot build a vtable for.  So
         * this is refused even with reverse proxies built: the mechanism exists,
         * the bounded licence to use it here does not.  CLASS_E_NOAGGREGATION is
         * what a coclass that does not support aggregation answers, which is a
         * case every caller of CoCreateInstance already handles. */
        FIXME( "syscom: CoCreateInstance with a non-NULL pUnkOuter %p is "
               "refused: an aggregated inner object delegates QueryInterface "
               "for arbitrary IIDs to it, which this roster cannot bound\n",
               outer );
        if (ppv) *ppv = NULL;
        return CLASS_E_NOAGGREGATION;
    }
    hr = CoCreateInstance( rclsid, NULL, ctx, riid, ppv );
    return __wine_com_wrap_out_iface( hr, riid, ppv );
}

/* The task allocator, which a guest reaches as an INTERFACE rather than as the
 * flat CoTaskMemAlloc/Free pair beside it -- so it has to come back wrapped
 * like any other out-interface, and until now it did not: CoGetMalloc was on
 * the GUEST-REFUSE list, and a guest asking for IMalloc got
 * __wine_com_refuse's "interface-bearing flat export with no wrapper yet".
 *
 * It needs no riid argument to decide what to wrap, which is the only way it
 * differs from CoCreateInstance above: the interface is IMalloc by definition
 * of the call, so the IID is named here rather than taken from the caller.
 * IMalloc is already on the roster (SYSCOM_IFACE_IMalloc), so the vtable this
 * builds is the same generated slot table every other interface gets.
 */
HRESULT WINAPI __wine_guest_CoGetMalloc( DWORD context, IMalloc **ppMalloc )
{
    HRESULT hr;

    if (!syscom_ready()) return E_FAIL;
    hr = CoGetMalloc( context, ppMalloc );
    return __wine_com_wrap_out_iface( hr, &IID_IMalloc, (void **)ppMalloc );
}

HRESULT WINAPI __wine_guest_CoGetClassObject( REFCLSID rclsid, DWORD ctx,
                                              COSERVERINFO *info, REFIID riid,
                                              void **ppv )
{
    HRESULT hr;

    if (!syscom_ready()) return E_FAIL;
    if ((hr = syscom_xaudio2_class_gate( rclsid )) != S_OK)
    {
        if (ppv) *ppv = NULL;
        return hr;
    }
    hr = CoGetClassObject( rclsid, ctx, info, riid, ppv );
    return __wine_com_wrap_out_iface( hr, riid, ppv );
}

HRESULT WINAPI __wine_guest_CreateStreamOnHGlobal( HGLOBAL hglobal, BOOL delete_on_release,
                                                   IStream **out )
{
    HRESULT hr;

    if (!syscom_ready()) return E_FAIL;
    hr = CreateStreamOnHGlobal( hglobal, delete_on_release, out );
    if (SUCCEEDED(hr))
        __wine_com_wrap_static( (void **)out, SYSCOM_IFACE_IStream );
    return hr;
}

HRESULT WINAPI __wine_guest_GetHGlobalFromStream( IStream *stream, HGLOBAL *phglobal )
{
    void *host;

    if (!syscom_ready()) return E_FAIL;
    if (!__wine_com_translate_in( stream, &host ))
    {
        /* Not a direction problem and not fixed by reverse proxies: this
         * export answers with the HGLOBAL that CreateStreamOnHGlobal put
         * INSIDE one of ole32's own stream objects.  A guest-implemented
         * stream has no such HGLOBAL to report, and ole32 says so itself
         * (E_INVALIDARG) for any stream that is not one of its own. */
        FIXME( "syscom: GetHGlobalFromStream on the guest-implemented stream "
               "%p is refused: only a stream ole32 itself created on an HGLOBAL "
               "has one to report\n", stream );
        return E_NOTIMPL;
    }
    return GetHGlobalFromStream( host, phglobal );
}

/* ------------------------------------------------------- VariantClear
 *
 * system-com-design.md §9.2's first "hand walker": VARIANT is a by-value
 * carrier that CAN hold an interface pointer (VT_UNKNOWN, VT_DISPATCH, and
 * the VT_ARRAY/VT_RECORD hulls that can hold either at one remove), so unlike
 * an ordinary flat export, native VariantClear can never be handed a
 * guest-visible pointer unclassified.  Layout is measured identical on this
 * lane for every case this wrapper serves (ppc64le/syscom/probes/
 * variant_layout_probe.c): VARIANT is 24 bytes, vt at offset 0, the payload
 * union at offset 8 -- a scalar/BSTR/BYREF/bad-vt VARIANT is bytes native
 * oleaut32 reads exactly as the guest wrote them, so those cases are pure
 * pass-through below.
 *
 * Native reference: dlls/oleaut32/variant.c VariantClear (its actual body,
 * not VARIANT_ClearInd, which VariantCopyInd uses and this export does not).
 * Its shape drives every branch here:
 *
 *   - it validates vt FIRST and touches nothing on failure -- a bad vt comes
 *     back DISP_E_BADVARTYPE with the VARIANT untouched, which is also this
 *     wrapper's answer for anything it does not specifically classify below;
 *
 *   - ANY VT_BYREF combination frees NOTHING -- V_ISBYREF() short-circuits
 *     the whole free block and only V_VT is ever written -- so even
 *     VT_BYREF|VT_UNKNOWN over a live proxy is safe to hand native as-is: no
 *     pointer the BYREF slot points AT is ever dereferenced, so the referent
 *     stays exactly as owned as it was before the call;
 *
 *   - VT_UNKNOWN/VT_DISPATCH (non-byref) is the one case that must NEVER
 *     reach native as-is: native would IUnknown_Release() a guest-visible
 *     pointer, and for one of our forward proxies that pointer is not the
 *     host IUnknown at all -- releasing it directly would either crash (it
 *     is not a real vtable) or, if it happened to alias something, release a
 *     reference nobody asked native to touch.  The proxy's OWN guest-visible
 *     reference is what has to go, and only __wine_com_release_guest knows
 *     how to do that without touching the ONE host reference the proxy
 *     itself owns for its whole life;
 *
 *   - VT_RECORD (non-byref) with a non-NULL pRecInfo calls
 *     IRecordInfo_RecordClear + Release through an interface pointer this
 *     lane's class-G roster does not carry yet (see the design study this
 *     wrapper was built from, §1 class E) -- refused by name rather than
 *     guessed;
 *
 *   - VT_SAFEARRAY/VT_ARRAY|* with a non-NULL descriptor is scalar-element
 *     pass-through UNLESS fFeatures says the array holds interface elements
 *     (FADF_UNKNOWN/FADF_DISPATCH/FADF_VARIANT/FADF_RECORD/FADF_HAVEIID),
 *     which SafeArrayDestroy would AddRef/Release per element natively --
 *     refused by name, v1 does not recurse into array elements.
 *
 * Every refusal below returns E_NOTIMPL and leaves the VARIANT untouched --
 * exactly __wine_com_refuse's discipline, but now naming VariantClear and the
 * concrete vt instead of the generic "no wrapper yet" message. */

/* Mirrors dlls/oleaut32/variant.c's VARIANT_ValidateType (static, so this
 * wrapper cannot call it directly) for the ONE thing this wrapper needs to
 * know before native does its own validation: whether `vt` is even shaped
 * like a real VARIANT type, i.e. whether it is safe to read the payload
 * union as a SAFEARRAY* and dereference its fFeatures.  Without this gate, a
 * guest VARIANT with the VT_ARRAY bit set over a bogus base type (which
 * native's validator would refuse before ever touching V_ARRAY) would make
 * this wrapper dereference a pointer native itself would never have looked
 * at.  Copied rather than reinvented; if oleaut32's validator ever changes
 * this must move with it, and gen_layout_check.py's oleaut32 extension
 * (design doc §9.2/§12.7) is the right place to pin that in a gate. */
static BOOL syscom_variant_type_ok( VARTYPE vt )
{
    VARTYPE extra = vt & (VT_VECTOR | VT_ARRAY | VT_BYREF | VT_RESERVED);
    VARTYPE base = vt & VT_TYPEMASK;

    if (extra & (VT_VECTOR | VT_RESERVED)) return FALSE;
    if (!(base < VT_VOID || base == VT_RECORD || base == VT_CLSID)) return FALSE;
    if ((extra & (VT_BYREF | VT_ARRAY)) && base <= VT_NULL) return FALSE;
    return base != 15;
}

/* WINEEMUVARIANTUNSAFERELEASE=1 -- the negative control for the ONE new
 * mechanism this wrapper adds, same shape as WINEEMUNOCOMWRAP for the older
 * one.  Set, it makes the VT_UNKNOWN/VT_DISPATCH branch below release the
 * HOST reference directly instead of going through __wine_com_release_guest,
 * which is exactly the double-free ppc64le/syscom/probes/variant_clear_smoke.c
 * proves out: the proxy still interns that host reference, so a second
 * legitimate use of the same underlying object corrupts or crashes.  A gate
 * that cannot go red proves nothing. */
static BOOL syscom_variant_unsafe_release( void )
{
    static int cached = -1;

    if (cached < 0)
    {
        WCHAR buf[8];
        cached = GetEnvironmentVariableW( L"WINEEMUVARIANTUNSAFERELEASE",
                                          buf, ARRAYSIZE(buf) ) > 0 &&
                 buf[0] == '1';
    }
    return cached;
}

HRESULT WINAPI __wine_guest_VariantClear( VARIANTARG *v )
{
    VARTYPE vt;

    if (!syscom_ready()) return E_FAIL;

    vt = V_VT( v );

    /* Any BYREF form: pass straight to native, unconditionally.  See the
     * file comment above -- native frees nothing through a BYREF slot. */
    if (V_ISBYREF( v ))
        return VariantClear( v );

    switch (vt)
    {
    case VT_UNKNOWN:
    case VT_DISPATCH:
    {
        IUnknown *punk = V_UNKNOWN( v );
        void *host;

        /* NULL classifies as a hit here (__wine_com_translate_in(NULL, ...)
         * answers TRUE with host NULL), which lands it on the same path
         * native takes for a NULL punkVal: nothing to release, write
         * VT_EMPTY, S_OK.  No separate NULL check needed. */
        if (__wine_com_translate_in( punk, &host ))
        {
            /* A forward proxy (or NULL): drop the GUEST-VISIBLE reference
             * through the proxy itself and clear the slot ourselves.  Never
             * __wine_com_unwrap() + native IUnknown_Release(host) here --
             * see the file comment; that double-frees the proxy's own host
             * reference the day the proxy later dies. */
            if (syscom_variant_unsafe_release() && host)
            {
                /* THE SABOTAGE LEG, permanently in the tree and off by
                 * default: exactly the bug the comment above forbids,
                 * released through the host vtable instead of the proxy. */
                ((IUnknown *)host)->lpVtbl->Release( (IUnknown *)host );
            }
            else
                __wine_com_release_guest( punk );
            V_VT( v ) = VT_EMPTY;
            return S_OK;
        }
        /* A guest-implemented object.  v1 refuses rather than guessing who
         * owns a Release run through a borrowed reverse proxy -- a design
         * decision nobody has made yet, not a mechanism gap. */
        FIXME( "syscom: VariantClear refuses a guest-implemented %s %p "
               "(vt %#x): releasing it through a reverse proxy has "
               "ownership semantics nobody has designed yet\n",
               vt == VT_DISPATCH ? "IDispatch" : "IUnknown", punk, vt );
        return E_NOTIMPL;
    }

    case VT_RECORD:
        if (V_RECORDINFO( v ))
        {
            FIXME( "syscom: VariantClear refuses VT_RECORD (vt %#x) with a "
                   "non-NULL IRecordInfo %p: RecordClear+Release is an "
                   "interface call this lane's roster does not carry yet, "
                   "and swapping in an unwrapped pointer would double-free "
                   "whatever it is a proxy for\n", vt, V_RECORDINFO( v ) );
            return E_NOTIMPL;
        }
        return VariantClear( v );

    default:
        if ((V_ISARRAY( v ) || (vt & ~VT_BYREF) == VT_SAFEARRAY) &&
            syscom_variant_type_ok( vt ))
        {
            SAFEARRAY *sa = V_ARRAY( v );

            if (sa && (sa->fFeatures & (FADF_UNKNOWN | FADF_DISPATCH |
                                        FADF_VARIANT | FADF_RECORD |
                                        FADF_HAVEIID)))
            {
                FIXME( "syscom: VariantClear refuses vt %#x (SAFEARRAY %p, "
                       "fFeatures %#x): SafeArrayDestroy would AddRef/"
                       "Release its elements natively and v1 does not "
                       "recurse into array elements\n",
                       vt, sa, sa->fFeatures );
                return E_NOTIMPL;
            }
        }
        return VariantClear( v );
    }
}
