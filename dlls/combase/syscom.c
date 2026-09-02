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
#define COBJMACROS   /* the 2026-09-01 walkers QI/Release native objects */

#include "windef.h"
#include "winbase.h"
#include "objbase.h"
#include "propidl.h"   /* PROPVARIANT + the PropVariant* prototypes the
                        * guest-side trio below wraps */
#include "oleauto.h"
#include "ocidl.h"     /* CONNECTDATA, for the IEnumConnections walker */
/* initguid BETWEEN the core headers and dmusici.h: the DirectMusic GUIDs
 * (the param tags and IID_IDirectMusicSegmentState) are in no import
 * library, so this TU defines them -- while everything included above
 * stays extern and resolves against libuuid as always. */
#include "initguid.h"
#include "dmusici.h"   /* DMUS_PMSG/OBJECTDESC/NOTIFICATION + the param
                        * tag GUIDs the 2026-09-01 walkers dispatch on */
#include "winternl.h"
#include "wine/debug.h"
#include "wine/winecom.h"

#include "syscom_marshal.h"

/* AFTER the marshal tables: the array-delivery hook's vtable width and its
 * rostered IID are both read from them (see the hook, at the end of the
 * 64-bit-lane wrappers).  wbemcli.h comes in through this header. */
#include "wine/winecom_arrin.h"

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
    /* REFUSAL HYGIENE, BY HAND, because no generated scrub mask reaches a
     * WINECOM_F_HAND row: a hand walker that refuses owns its out-params the
     * way scrub_refused_outs() owns a table refusal's.  Every refusal below
     * NULLs the voice/interface cell FIRST, because leaving it is the
     * GetShader class verbatim -- the caller reads its own stack residue,
     * calls through it, and the emulator decodes a HOST module's bytes as
     * x86.  [MEASURED] The Witcher 3's load regression: IMMDevice::Activate's
     * unscrubbed refusal below fired on every run, and the guest ended up
     * executing wined3d.dll's ppc64le bytes -- the fault report's
     * native-pc line named this file's hole.  A native failure path stays
     * untouched: real XAudio2 leaves *out alone on failure too, and matching
     * Windows means scrubbing only the refusals this side invented. */
    if (cb && !winecom_to_native( (void *)cb, SYSCOM_IFACE_IXAudio2VoiceCallback,
                                  &cb_host ))
    {
        FIXME( "xaudio2: IXAudio2::CreateSourceVoice could not give the "
               "IXAudio2VoiceCallback at %p a reverse proxy; refusing rather "
               "than handing the mixer thread an x86-64 vtable\n", cb );
        winecom_refused_scrub_ptr( out );
        return (UINT64)(UINT)E_NOTIMPL;
    }
    if (sends)
    {
        winecom_refused_scrub_ptr( out );
        return (UINT64)(UINT)refuse_bearing( "IXAudio2::CreateSourceVoice",
                 "its XAUDIO2_VOICE_SENDS, whose descriptors carry "
                 "IXAudio2Voice pointers", sends );
    }
    if (chain)
    {
        winecom_refused_scrub_ptr( out );
        return (UINT64)(UINT)refuse_bearing( "IXAudio2::CreateSourceVoice",
                 "its XAUDIO2_EFFECT_CHAIN, whose descriptors carry IUnknown "
                 "pointers", chain );
    }

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

    /* refusal hygiene by hand -- see CreateSourceVoice */
    if (sends)
    {
        winecom_refused_scrub_ptr( out );
        return (UINT64)(UINT)refuse_bearing( "IXAudio2::CreateSubmixVoice",
                 "its XAUDIO2_VOICE_SENDS, whose descriptors carry "
                 "IXAudio2Voice pointers", sends );
    }
    if (chain)
    {
        winecom_refused_scrub_ptr( out );
        return (UINT64)(UINT)refuse_bearing( "IXAudio2::CreateSubmixVoice",
                 "its XAUDIO2_EFFECT_CHAIN, whose descriptors carry IUnknown "
                 "pointers", chain );
    }

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

    /* refusal hygiene by hand -- see CreateSourceVoice */
    if (chain)
    {
        winecom_refused_scrub_ptr( out );
        return (UINT64)(UINT)refuse_bearing( "IXAudio2::CreateMasteringVoice",
                 "its XAUDIO2_EFFECT_CHAIN, whose descriptors carry IUnknown "
                 "pointers", chain );
    }

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

    /* refusal hygiene by hand -- see CreateSourceVoice.  THIS is the site the
     * Witcher 3 hit on every load: the game passes a non-NULL
     * pActivationParams, never checks the HRESULT closely enough, and read
     * whatever its stack held where *ppv was never written. */
    if (params)
    {
        winecom_refused_scrub_ptr( ppv );
        return (UINT64)(UINT)refuse_bearing( "IMMDevice::Activate",
                 "its PROPVARIANT pActivationParams, whose union carries an "
                 "IUnknown pointer", params );
    }

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

/* =========================================================================
 * The 2026-09-01 completeness pass: the walkers that emptied the refusal
 * list's servable half.  Every function below serves a slot that used to
 * answer E_NOTIMPL for a reason that was mechanical (a 16-byte aggregate, a
 * tagged union, an interface member at a fixed offset), never semantic.
 * What genuinely cannot cross refuses AT RUNTIME, per element or per tag,
 * NAMING what it refused -- and scrubs what it refused into NULL/VT_EMPTY,
 * per the refusal-hygiene rule (a caller that checks nothing must read
 * nothing that was ours).
 * ========================================================================= */

/* MS-x64 passes any aggregate that is not 1/2/4/8 bytes by HIDDEN POINTER to
 * a caller-owned temporary, so the guest's argument slot holds an address;
 * ELFv2 passes small aggregates by value in GPRs.  These helpers read the
 * hidden pointer and call a real by-value prototype -- the compiler emits
 * the ELFv2 side, nothing is hand-packed.  (dlls/mfplat/mfcom.c worked this
 * shape first; the reasoning lives there and in the GUID-case refusal text
 * these rows used to carry.) */

static UINT64 hand_nlm_get_network( void *host, UINT slot, AMD64_CONTEXT *ctx )
{
    HRESULT (WINAPI *fn)( void *, GUID, void ** ) = host_slot( host, slot );
    const GUID *id = (const GUID *)(ULONG_PTR)winecom_read_arg( ctx, 1 );
    void **out = (void **)(ULONG_PTR)winecom_read_arg( ctx, 2 );
    HRESULT hr;

    /* refusal hygiene by hand -- see CreateSourceVoice.  The GUID travels by
     * hidden pointer from a guest that cannot pass NULL by value, so a NULL
     * here is OUR refusal to read the aggregate, not native's answer. */
    if (!id)
    {
        winecom_refused_scrub_ptr( out );
        return (UINT64)(UINT)E_POINTER;
    }
    hr = fn( host, *id, out );
    if (SUCCEEDED(hr) && out && *out)
        *out = winecom_wrap( *out, SYSCOM_IFACE_INetwork );
    return (UINT64)(UINT)hr;
}

static UINT64 hand_nlm_get_network_connection( void *host, UINT slot, AMD64_CONTEXT *ctx )
{
    HRESULT (WINAPI *fn)( void *, GUID, void ** ) = host_slot( host, slot );
    const GUID *id = (const GUID *)(ULONG_PTR)winecom_read_arg( ctx, 1 );
    void **out = (void **)(ULONG_PTR)winecom_read_arg( ctx, 2 );
    HRESULT hr;

    /* refusal hygiene by hand -- see CreateSourceVoice */
    if (!id)
    {
        winecom_refused_scrub_ptr( out );
        return (UINT64)(UINT)E_POINTER;
    }
    hr = fn( host, *id, out );
    if (SUCCEEDED(hr) && out && *out)
        *out = winecom_wrap( *out, SYSCOM_IFACE_INetworkConnection );
    return (UINT64)(UINT)hr;
}

/* OnPropertyValueChanged(LPCWSTR, const PROPERTYKEY): the key is GUID+DWORD,
 * 20 bytes, hidden-pointer on the guest side and by-value here. */
static UINT64 hand_propkey_byval( void *host, UINT slot, AMD64_CONTEXT *ctx )
{
    HRESULT (WINAPI *fn)( void *, const WCHAR *, PROPERTYKEY ) = host_slot( host, slot );
    const WCHAR *dev = (const WCHAR *)(ULONG_PTR)winecom_read_arg( ctx, 1 );
    const PROPERTYKEY *key = (const PROPERTYKEY *)(ULONG_PTR)winecom_read_arg( ctx, 2 );
    PROPERTYKEY k = { 0 };

    if (key) k = *key;
    return (UINT64)(UINT)fn( host, dev, k );
}

/* ---------------------------------------------- guest-authored PROPVARIANTs
 *
 * The tagged-union discipline, IN direction: plain tags are bytes both sides
 * read identically and pass as the guest wrote them; VT_UNKNOWN/VT_DISPATCH
 * unwrap a proxy to its host object (winecom_to_native with ~0u accepts any
 * FORWARD proxy by identity and correctly fails a guest-authored object --
 * reverse-proxying an arbitrary IUnknown is not licensed on this surface);
 * the remaining interface-bearing arms refuse naming the VT.  The out-param
 * scrub duty does not arise here: SetValue writes nothing back. */
/* the shared tagged-union discipline; the wrappers below add this module's
 * once-per-class logging on top */
#include "wine/winecom_variant.h"

#define mf_style_pv_is_iface_arm winecom_vt_is_untranslatable_iface_arm

static BOOL syscom_pv_in( const PROPVARIANT *pv, PROPVARIANT *copy,
                          const PROPVARIANT **use, UINT slot )
{
    static LONG logged;

    *use = pv;
    if (!pv) return TRUE;
    switch (pv->vt)
    {
    case VT_UNKNOWN:
    case VT_DISPATCH:
    {
        void *native;

        *copy = *pv;
        if (pv->punkVal &&
            !winecom_to_native( pv->punkVal, ~0u, &native ))
        {
            if (!InterlockedExchange( &logged, 1 ))
                FIXME( "syscom: slot %u was given a PROPVARIANT (vt 0x%04x) "
                       "carrying a guest-authored object this surface cannot "
                       "reverse-proxy; refusing the call\n", slot, pv->vt );
            return FALSE;
        }
        copy->punkVal = pv->punkVal ? native : NULL;
        *use = copy;
        return TRUE;
    }
    default:
        if (mf_style_pv_is_iface_arm( pv->vt ))
        {
            if (!InterlockedExchange( &logged, 1 ))
                FIXME( "syscom: slot %u was given a PROPVARIANT of type "
                       "0x%04x -- an interface arm nothing here translates "
                       "(a VECTOR/ARRAY/BYREF of objects); refusing\n",
                       slot, pv->vt );
            return FALSE;
        }
        return TRUE;    /* plain data: strings, blobs, numbers */
    }
}

static UINT64 hand_propstore_setvalue( void *host, UINT slot, AMD64_CONTEXT *ctx )
{
    HRESULT (WINAPI *fn)( void *, const PROPERTYKEY *, const PROPVARIANT * ) =
        host_slot( host, slot );
    const PROPERTYKEY *key = (const PROPERTYKEY *)(ULONG_PTR)winecom_read_arg( ctx, 1 );
    const PROPVARIANT *pv = (const PROPVARIANT *)(ULONG_PTR)winecom_read_arg( ctx, 2 );
    const PROPVARIANT *use;
    PROPVARIANT copy;

    if (!syscom_pv_in( pv, &copy, &use, slot )) return (UINT64)(UINT)E_NOTIMPL;
    return (UINT64)(UINT)fn( host, key, use );
}

/* ------------------------------------------------------- the enum-Next family
 *
 * Next(celt, rgelt, pceltFetched): celt travels BY VALUE, the array is the
 * guest's own storage the native enumerator fills with NATIVE pointers, and
 * pceltFetched is optional (NULL is legal when celt is 1).  The walker calls
 * native into the guest's array and wraps each fetched element in place.
 * An element the roster cannot wrap is RELEASED and scrubbed to NULL -- the
 * hygiene rule -- and said once. */
static UINT64 enum_next_core( void *host, UINT slot, AMD64_CONTEXT *ctx,
                              UINT elem_iface )
{
    HRESULT (WINAPI *fn)( void *, ULONG, void **, ULONG * ) = host_slot( host, slot );
    ULONG celt = (ULONG)winecom_read_arg( ctx, 1 );
    void **rgelt = (void **)(ULONG_PTR)winecom_read_arg( ctx, 2 );
    ULONG *fetched = (ULONG *)(ULONG_PTR)winecom_read_arg( ctx, 3 );
    ULONG got, i;
    HRESULT hr;

    hr = fn( host, celt, rgelt, fetched );
    if (FAILED(hr) || !rgelt) return (UINT64)(UINT)hr;
    got = fetched ? *fetched : (hr == S_OK ? celt : 0);
    for (i = 0; i < got; i++)
        if (rgelt[i]) rgelt[i] = winecom_wrap( rgelt[i], elem_iface );
    return (UINT64)(UINT)hr;
}

static UINT64 hand_enum_next_unknown( void *host, UINT slot, AMD64_CONTEXT *ctx )
{
    /* IEnumUnknown vends nameless objects; the one honest static type for
     * the cell is IDispatch when the object answers it, else the element is
     * released and scrubbed.  This surface's IEnumUnknown consumers
     * (component categories) are IDispatch-shaped, and a miss is loud. */
    HRESULT (WINAPI *fn)( void *, ULONG, void **, ULONG * ) = host_slot( host, slot );
    ULONG celt = (ULONG)winecom_read_arg( ctx, 1 );
    void **rgelt = (void **)(ULONG_PTR)winecom_read_arg( ctx, 2 );
    ULONG *fetched = (ULONG *)(ULONG_PTR)winecom_read_arg( ctx, 3 );
    static LONG logged;
    ULONG got, i;
    HRESULT hr;

    hr = fn( host, celt, rgelt, fetched );
    if (FAILED(hr) || !rgelt) return (UINT64)(UINT)hr;
    got = fetched ? *fetched : (hr == S_OK ? celt : 0);
    for (i = 0; i < got; i++)
    {
        IUnknown *u = rgelt[i];
        void *disp;

        if (!u) continue;
        if (SUCCEEDED(IUnknown_QueryInterface( u, &IID_IDispatch, &disp )))
        {
            IUnknown_Release( u );
            rgelt[i] = winecom_wrap( disp, SYSCOM_IFACE_IDispatch );
            continue;
        }
        if (!InterlockedExchange( &logged, 1 ))
            FIXME( "syscom: IEnumUnknown::Next fetched an object that is not "
                   "IDispatch and has no static roster type; releasing it and "
                   "scrubbing the cell to NULL\n" );
        IUnknown_Release( u );
        rgelt[i] = NULL;
    }
    return (UINT64)(UINT)hr;
}

static UINT64 hand_enum_next_moniker( void *host, UINT slot, AMD64_CONTEXT *ctx )
{
    return enum_next_core( host, slot, ctx, SYSCOM_IFACE_IMoniker );
}

static UINT64 hand_enum_next_cp( void *host, UINT slot, AMD64_CONTEXT *ctx )
{
    return enum_next_core( host, slot, ctx, SYSCOM_IFACE_IConnectionPoint );
}

static UINT64 hand_enum_next_connectdata( void *host, UINT slot, AMD64_CONTEXT *ctx )
{
    /* CONNECTDATA { IUnknown *pUnk; DWORD dwCookie }: wrap each element's
     * pUnk.  The sink a guest registered arrives as a REVERSE proxy and
     * winecom_wrap unwraps it to the guest's own pointer -- identity, the
     * property Unadvise-by-cookie callers depend on. */
    HRESULT (WINAPI *fn)( void *, ULONG, CONNECTDATA *, ULONG * ) = host_slot( host, slot );
    ULONG celt = (ULONG)winecom_read_arg( ctx, 1 );
    CONNECTDATA *rgcd = (CONNECTDATA *)(ULONG_PTR)winecom_read_arg( ctx, 2 );
    ULONG *fetched = (ULONG *)(ULONG_PTR)winecom_read_arg( ctx, 3 );
    static LONG logged;
    ULONG got, i;
    HRESULT hr;

    hr = fn( host, celt, rgcd, fetched );
    if (FAILED(hr) || !rgcd) return (UINT64)(UINT)hr;
    got = fetched ? *fetched : (hr == S_OK ? celt : 0);
    for (i = 0; i < got; i++)
    {
        IUnknown *u = rgcd[i].pUnk;
        void *disp;

        if (!u) continue;
        if (SUCCEEDED(IUnknown_QueryInterface( u, &IID_IDispatch, &disp )))
        {
            IUnknown_Release( u );
            rgcd[i].pUnk = winecom_wrap( disp, SYSCOM_IFACE_IDispatch );
            continue;
        }
        if (!InterlockedExchange( &logged, 1 ))
            FIXME( "syscom: IEnumConnections::Next fetched a sink with no "
                   "static roster type; releasing and scrubbing to NULL\n" );
        IUnknown_Release( u );
        rgcd[i].pUnk = NULL;
    }
    return (UINT64)(UINT)hr;
}

static UINT64 hand_enum_next_variant( void *host, UINT slot, AMD64_CONTEXT *ctx )
{
    /* IEnumVARIANT::Next on this surface is the NLM enumerators' __NewEnum:
     * the VARIANTs hold VT_UNKNOWN/VT_DISPATCH network objects.  Plain tags
     * pass as filled; object tags wrap as IDispatch (every NLM object is
     * IDispatch-derived, and the header proves it -- netlistmgr.idl);
     * anything else object-shaped is released and scrubbed to VT_EMPTY. */
    HRESULT (WINAPI *fn)( void *, ULONG, VARIANT *, ULONG * ) = host_slot( host, slot );
    ULONG celt = (ULONG)winecom_read_arg( ctx, 1 );
    VARIANT *rgv = (VARIANT *)(ULONG_PTR)winecom_read_arg( ctx, 2 );
    ULONG *fetched = (ULONG *)(ULONG_PTR)winecom_read_arg( ctx, 3 );
    static LONG logged;
    ULONG got, i;
    HRESULT hr;

    hr = fn( host, celt, rgv, fetched );
    if (FAILED(hr) || !rgv) return (UINT64)(UINT)hr;
    got = fetched ? *fetched : (hr == S_OK ? celt : 0);
    for (i = 0; i < got; i++)
    {
        IUnknown *u;
        void *disp;

        if (V_VT(&rgv[i]) != VT_UNKNOWN && V_VT(&rgv[i]) != VT_DISPATCH)
            continue;
        u = (IUnknown *)V_UNKNOWN(&rgv[i]);
        if (!u) continue;
        if (SUCCEEDED(IUnknown_QueryInterface( u, &IID_IDispatch, &disp )))
        {
            IUnknown_Release( u );
            V_VT(&rgv[i]) = VT_DISPATCH;
            V_DISPATCH(&rgv[i]) = winecom_wrap( disp, SYSCOM_IFACE_IDispatch );
            continue;
        }
        if (!InterlockedExchange( &logged, 1 ))
            FIXME( "syscom: IEnumVARIANT::Next fetched a non-IDispatch "
                   "object; releasing and scrubbing the VARIANT to EMPTY\n" );
        IUnknown_Release( u );
        VariantInit( &rgv[i] );
    }
    return (UINT64)(UINT)hr;
}

/* QueryMultipleInterfaces: the native call fills each element's pItf with a
 * NATIVE pointer; each is then re-run through the ONE fail-closed choke
 * point this module already trusts for every out-interface.  An element the
 * roster cannot serve becomes E_NOINTERFACE + NULL (wrap_out_iface released
 * it), and the aggregate HRESULT is recomputed per the MULTI_QI contract. */
static UINT64 hand_multi_qi( void *host, UINT slot, AMD64_CONTEXT *ctx )
{
    HRESULT (WINAPI *fn)( void *, ULONG, MULTI_QI * ) = host_slot( host, slot );
    ULONG cmqi = (ULONG)winecom_read_arg( ctx, 1 );
    MULTI_QI *mqi = (MULTI_QI *)(ULONG_PTR)winecom_read_arg( ctx, 2 );
    ULONG i, served = 0;
    HRESULT hr;

    hr = fn( host, cmqi, mqi );
    if (!mqi) return (UINT64)(UINT)hr;
    for (i = 0; i < cmqi; i++)
    {
        if (FAILED(mqi[i].hr) || !mqi[i].pItf) continue;
        mqi[i].hr = winecom_wrap_out_iface( mqi[i].hr, mqi[i].pIID,
                                            (void **)&mqi[i].pItf );
        if (SUCCEEDED(mqi[i].hr)) served++;
    }
    if (served == cmqi) return (UINT64)(UINT)S_OK;
    if (!served) return (UINT64)(UINT)E_NOINTERFACE;
    return (UINT64)(UINT)CO_S_NOTALLINTERFACES;
}

/* ---------------------------------------------------------- IDispatch::Invoke
 *
 * DISPPARAMS carries a VARIANTARG array the GUEST authored.  The array is
 * cloned (never mutated -- it is the caller's memory), each element's object
 * arms unwrapped through the same rule as syscom_pv_in; the result VARIANT
 * and the byref out-arms are wrapped on the way back with the enum-Next
 * discipline.  EXCEPINFO and puArgErr are plain memory. */
static BOOL syscom_variant_in( VARIANTARG *v, UINT slot )
{
    static LONG logged;

    if (winecom_variant_in( v )) return TRUE;
    if (!InterlockedExchange( &logged, 1 ))
        FIXME( "syscom: Invoke was given a VARIANT (vt 0x%04x) carrying a "
               "guest-authored object or an untranslatable interface arm; "
               "refusing the call\n", V_VT(v) );
    return FALSE;
}

static void syscom_variant_out_fixup( VARIANT *v )
{
    static LONG logged;

    if (winecom_variant_out_fixup( v, SYSCOM_IFACE_IDispatch ) &&
        !InterlockedExchange( &logged, 1 ))
        FIXME( "syscom: Invoke returned a non-IDispatch object in a VARIANT; "
               "released and scrubbed to EMPTY\n" );
}

static UINT64 hand_dispatch_invoke( void *host, UINT slot, AMD64_CONTEXT *ctx )
{
    HRESULT (WINAPI *fn)( void *, DISPID, REFIID, LCID, WORD, DISPPARAMS *,
                          VARIANT *, EXCEPINFO *, UINT * ) = host_slot( host, slot );
    DISPID member = (DISPID)winecom_read_arg( ctx, 1 );
    REFIID riid = (REFIID)(ULONG_PTR)winecom_read_arg( ctx, 2 );
    LCID lcid = (LCID)winecom_read_arg( ctx, 3 );
    WORD flags = (WORD)winecom_read_arg( ctx, 4 );
    DISPPARAMS *dp = (DISPPARAMS *)(ULONG_PTR)winecom_read_arg( ctx, 5 );
    VARIANT *result = (VARIANT *)(ULONG_PTR)winecom_read_arg( ctx, 6 );
    EXCEPINFO *exc = (EXCEPINFO *)(ULONG_PTR)winecom_read_arg( ctx, 7 );
    UINT *arg_err = (UINT *)(ULONG_PTR)winecom_read_arg( ctx, 8 );
    VARIANTARG local[16], *args = local;
    DISPPARAMS use;
    HRESULT hr;
    UINT i;

    if (!dp) return (UINT64)(UINT)fn( host, member, riid, lcid, flags, dp,
                                      result, exc, arg_err );
    use = *dp;
    if (dp->cArgs)
    {
        if (dp->cArgs > ARRAYSIZE(local))
        {
            args = RtlAllocateHeap( GetProcessHeap(), 0,
                                    dp->cArgs * sizeof(*args) );
            if (!args) return (UINT64)(UINT)E_OUTOFMEMORY;
        }
        memcpy( args, dp->rgvarg, dp->cArgs * sizeof(*args) );
        for (i = 0; i < dp->cArgs; i++)
            if (!syscom_variant_in( &args[i], slot ))
            {
                if (args != local)
                    RtlFreeHeap( GetProcessHeap(), 0, args );
                /* refusal hygiene by hand -- see CreateSourceVoice.  Zeroed
                 * VARIANT/EXCEPINFO are the valid empty values (VT_EMPTY is
                 * 0), so an unchecked caller VariantClear()s nothing instead
                 * of a stack ghost. */
                winecom_refused_scrub_mem( result, sizeof(*result) );
                winecom_refused_scrub_mem( exc, sizeof(*exc) );
                winecom_refused_scrub_dw( arg_err );
                return (UINT64)(UINT)E_NOTIMPL;
            }
        use.rgvarg = args;
    }
    hr = fn( host, member, riid, lcid, flags, &use, result, exc, arg_err );
    if (args != local) RtlFreeHeap( GetProcessHeap(), 0, args );
    if (SUCCEEDED(hr)) syscom_variant_out_fixup( result );
    return (UINT64)(UINT)hr;
}

/* Bind's BINDPTR is a union: the FUNCDESC and VARDESC arms are data, the
 * ITypeComp arm is an object and *pDescKind says which was written.
 * ITypeInfo out is the ordinary wrap. */
static UINT64 hand_typecomp_bind( void *host, UINT slot, AMD64_CONTEXT *ctx )
{
    HRESULT (WINAPI *fn)( void *, LPOLESTR, ULONG, WORD, void **, DESCKIND *,
                          BINDPTR * ) = host_slot( host, slot );
    LPOLESTR name = (LPOLESTR)(ULONG_PTR)winecom_read_arg( ctx, 1 );
    ULONG hash = (ULONG)winecom_read_arg( ctx, 2 );
    WORD flags = (WORD)winecom_read_arg( ctx, 3 );
    void **ptinfo = (void **)(ULONG_PTR)winecom_read_arg( ctx, 4 );
    DESCKIND *kind = (DESCKIND *)(ULONG_PTR)winecom_read_arg( ctx, 5 );
    BINDPTR *bind = (BINDPTR *)(ULONG_PTR)winecom_read_arg( ctx, 6 );
    HRESULT hr;

    hr = fn( host, name, hash, flags, ptinfo, kind, bind );
    if (FAILED(hr)) return (UINT64)(UINT)hr;
    if (ptinfo && *ptinfo)
        *ptinfo = winecom_wrap( *ptinfo, SYSCOM_IFACE_ITypeInfo );
    if (kind && bind && *kind == DESCKIND_TYPECOMP && bind->lptcomp)
        bind->lptcomp = winecom_wrap( bind->lptcomp, SYSCOM_IFACE_ITypeComp );
    return (UINT64)(UINT)hr;
}

/* --------------------------------------------------- DirectMusic walkers
 *
 * The PMSG family: DMUS_PMSG's header (DMUS_PMSG_PART) carries three
 * interface members at fixed offsets -- pTool, pGraph, punkUser -- and the
 * struct's OWNERSHIP TRANSFERS on Send/Free (the guest must not touch a
 * message it sent; dmusici.h says so), which is what licenses translating
 * the members IN PLACE on those paths rather than cloning a variable-size
 * struct whose allocator is the performance itself. */

static BOOL dmus_pmsg_in( DMUS_PMSG *msg, UINT slot )
{
    static LONG logged;
    void *tool, *graph, *unk;
    void *native;

    if (!msg) return TRUE;
    /* refusal hygiene by hand -- see CreateSourceVoice, in the IN direction:
     * these members are translated IN PLACE in the guest's own message, so a
     * refusal on the SECOND or THIRD member would otherwise leave the first
     * one holding a HOST pointer in guest-visible memory.  That is the
     * GetShader class with the arrow reversed, so the originals go back. */
    tool = msg->pTool; graph = msg->pGraph; unk = msg->punkUser;
    if (msg->pTool)
    {
        if (!winecom_to_native( msg->pTool, ~0u, &native )) goto refuse;
        msg->pTool = native;
    }
    if (msg->pGraph)
    {
        if (!winecom_to_native( msg->pGraph, ~0u, &native )) goto refuse;
        msg->pGraph = (struct IDirectMusicGraph *)native;
    }
    if (msg->punkUser)
    {
        if (!winecom_to_native( msg->punkUser, ~0u, &native )) goto refuse;
        msg->punkUser = native;
    }
    return TRUE;
refuse:
    msg->pTool = tool;
    msg->pGraph = (struct IDirectMusicGraph *)graph;
    msg->punkUser = unk;
    if (!InterlockedExchange( &logged, 1 ))
        FIXME( "syscom: slot %u's DMUS_PMSG carries a guest-authored object "
               "this surface cannot reverse-proxy; refusing the call\n", slot );
    return FALSE;
}

static void dmus_pmsg_out_fixup( DMUS_PMSG *msg )
{
    static LONG logged;

    if (!msg) return;
    if (msg->pTool)
        msg->pTool = winecom_wrap( msg->pTool, SYSCOM_IFACE_IDirectMusicTool );
    if (msg->pGraph)
        msg->pGraph = winecom_wrap( msg->pGraph, SYSCOM_IFACE_IDirectMusicGraph );
    if (msg->punkUser)
    {
        /* the one member with no static type: notifications carry a segment
         * state; anything else is scrubbed rather than leaked */
        void *ss;

        if (SUCCEEDED(IUnknown_QueryInterface( msg->punkUser,
                                               &IID_IDirectMusicSegmentState, &ss )))
        {
            IUnknown_Release( msg->punkUser );
            msg->punkUser = winecom_wrap( ss, SYSCOM_IFACE_IDirectMusicSegmentState );
        }
        else
        {
            if (!InterlockedExchange( &logged, 1 ))
                FIXME( "syscom: a DMUS_PMSG's punkUser is not a segment "
                       "state; releasing and scrubbing to NULL\n" );
            IUnknown_Release( msg->punkUser );
            msg->punkUser = NULL;
        }
    }
}

/* (this, DMUS_PMSG*) with in-place member translation: SendPMsg, FreePMsg. */
static UINT64 dmus_pmsg_in_call( void *host, UINT slot, AMD64_CONTEXT *ctx )
{
    HRESULT (WINAPI *fn)( void *, DMUS_PMSG * ) = host_slot( host, slot );
    DMUS_PMSG *msg = (DMUS_PMSG *)(ULONG_PTR)winecom_read_arg( ctx, 1 );

    if (!dmus_pmsg_in( msg, slot )) return (UINT64)(UINT)E_NOTIMPL;
    return (UINT64)(UINT)fn( host, msg );
}

static UINT64 hand_dmus_send_pmsg( void *host, UINT slot, AMD64_CONTEXT *ctx )
{
    return dmus_pmsg_in_call( host, slot, ctx );
}

static UINT64 hand_dmus_free_pmsg( void *host, UINT slot, AMD64_CONTEXT *ctx )
{
    return dmus_pmsg_in_call( host, slot, ctx );
}

static UINT64 hand_dmus_alloc_pmsg( void *host, UINT slot, AMD64_CONTEXT *ctx )
{
    /* a fresh message has no live members; the call is plain */
    HRESULT (WINAPI *fn)( void *, ULONG, DMUS_PMSG ** ) = host_slot( host, slot );

    return (UINT64)(UINT)fn( host, (ULONG)winecom_read_arg( ctx, 1 ),
                             (DMUS_PMSG **)(ULONG_PTR)winecom_read_arg( ctx, 2 ) );
}

static UINT64 hand_dmus_stamp_pmsg( void *host, UINT slot, AMD64_CONTEXT *ctx )
{
    /* the graph reads pGraph/pTool and WRITES pTool (the next tool in the
     * chain, AddRef'd) -- translate in, call, wrap what came back */
    HRESULT (WINAPI *fn)( void *, DMUS_PMSG * ) = host_slot( host, slot );
    DMUS_PMSG *msg = (DMUS_PMSG *)(ULONG_PTR)winecom_read_arg( ctx, 1 );
    HRESULT hr;

    if (!dmus_pmsg_in( msg, slot )) return (UINT64)(UINT)E_NOTIMPL;
    hr = fn( host, msg );
    if (SUCCEEDED(hr)) dmus_pmsg_out_fixup( msg );
    return (UINT64)(UINT)hr;
}

static UINT64 hand_dmus_clone_pmsg( void *host, UINT slot, AMD64_CONTEXT *ctx )
{
    HRESULT (WINAPI *fn)( void *, DMUS_PMSG *, DMUS_PMSG ** ) = host_slot( host, slot );
    DMUS_PMSG *src = (DMUS_PMSG *)(ULONG_PTR)winecom_read_arg( ctx, 1 );
    DMUS_PMSG **out = (DMUS_PMSG **)(ULONG_PTR)winecom_read_arg( ctx, 2 );
    HRESULT hr;

    /* refusal hygiene by hand -- see CreateSourceVoice */
    if (!dmus_pmsg_in( src, slot ))
    {
        winecom_refused_scrub_ptr( out );
        return (UINT64)(UINT)E_NOTIMPL;
    }
    hr = fn( host, src, out );
    /* the source's members are native now and its ownership stayed with the
     * guest: wrap them back, and wrap the clone's own (AddRef'd) members */
    dmus_pmsg_out_fixup( src );
    if (SUCCEEDED(hr) && out) dmus_pmsg_out_fixup( *out );
    return (UINT64)(UINT)hr;
}

static UINT64 hand_dmus_process_pmsg( void *host, UINT slot, AMD64_CONTEXT *ctx )
{
    /* IDirectMusicTool::ProcessPMsg(perf, pmsg) -- forward direction: the
     * guest drives a NATIVE tool with its performance proxy */
    HRESULT (WINAPI *fn)( void *, void *, DMUS_PMSG * ) = host_slot( host, slot );
    void *perf = (void *)(ULONG_PTR)winecom_read_arg( ctx, 1 );
    DMUS_PMSG *msg = (DMUS_PMSG *)(ULONG_PTR)winecom_read_arg( ctx, 2 );
    void *nperf = NULL;

    if (perf && !winecom_to_native( perf, ~0u, &nperf ))
        return (UINT64)(UINT)E_NOTIMPL;
    if (!dmus_pmsg_in( msg, slot )) return (UINT64)(UINT)E_NOTIMPL;
    return (UINT64)(UINT)fn( host, nperf, msg );
}

static UINT64 hand_dmus_flush( void *host, UINT slot, AMD64_CONTEXT *ctx )
{
    HRESULT (WINAPI *fn)( void *, void *, DMUS_PMSG *, REFERENCE_TIME ) =
        host_slot( host, slot );
    void *perf = (void *)(ULONG_PTR)winecom_read_arg( ctx, 1 );
    DMUS_PMSG *msg = (DMUS_PMSG *)(ULONG_PTR)winecom_read_arg( ctx, 2 );
    void *nperf = NULL;

    if (perf && !winecom_to_native( perf, ~0u, &nperf ))
        return (UINT64)(UINT)E_NOTIMPL;
    if (!dmus_pmsg_in( msg, slot )) return (UINT64)(UINT)E_NOTIMPL;
    return (UINT64)(UINT)fn( host, nperf, msg,
                             (REFERENCE_TIME)winecom_read_arg( ctx, 3 ) );
}

static UINT64 hand_dmus_get_notification_pmsg( void *host, UINT slot, AMD64_CONTEXT *ctx )
{
    HRESULT (WINAPI *fn)( void *, DMUS_NOTIFICATION_PMSG ** ) = host_slot( host, slot );
    DMUS_NOTIFICATION_PMSG **out =
        (DMUS_NOTIFICATION_PMSG **)(ULONG_PTR)winecom_read_arg( ctx, 1 );
    HRESULT hr;

    hr = fn( host, out );
    if (hr == S_OK && out && *out) dmus_pmsg_out_fixup( (DMUS_PMSG *)*out );
    return (UINT64)(UINT)hr;
}

/* ------------------------------------------- the OBJECTDESC family
 *
 * DMUS_OBJECTDESC carries one optional interface member, pStream, behind
 * DMUS_OBJ_STREAM in dwValidData.  The IN direction clones the descriptor
 * (caller memory is never mutated) and unwraps the stream; the OUT
 * direction wraps what native filled. */

static BOOL dmus_desc_in( const DMUS_OBJECTDESC *desc, DMUS_OBJECTDESC *copy,
                          const DMUS_OBJECTDESC **use, UINT slot )
{
    static LONG logged;
    void *native;

    *use = desc;
    if (!desc || !(desc->dwValidData & DMUS_OBJ_STREAM) || !desc->pStream)
        return TRUE;
    *copy = *desc;
    if (!winecom_to_native( desc->pStream, ~0u, &native ))
    {
        if (!InterlockedExchange( &logged, 1 ))
            FIXME( "syscom: slot %u's DMUS_OBJECTDESC carries a guest-"
                   "authored stream this surface cannot reverse-proxy; "
                   "refusing the call\n", slot );
        return FALSE;
    }
    copy->pStream = native;
    *use = copy;
    return TRUE;
}

static void dmus_desc_out_fixup( DMUS_OBJECTDESC *desc )
{
    if (desc && (desc->dwValidData & DMUS_OBJ_STREAM) && desc->pStream)
        desc->pStream = winecom_wrap( desc->pStream, SYSCOM_IFACE_IStream );
}

static UINT64 hand_dmus_objdesc_in( void *host, UINT slot, AMD64_CONTEXT *ctx )
{
    HRESULT (WINAPI *fn)( void *, const DMUS_OBJECTDESC * ) = host_slot( host, slot );
    const DMUS_OBJECTDESC *desc =
        (const DMUS_OBJECTDESC *)(ULONG_PTR)winecom_read_arg( ctx, 1 );
    const DMUS_OBJECTDESC *use;
    DMUS_OBJECTDESC copy;

    if (!dmus_desc_in( desc, &copy, &use, slot )) return (UINT64)(UINT)E_NOTIMPL;
    return (UINT64)(UINT)fn( host, use );
}

static UINT64 hand_dmus_objdesc_out( void *host, UINT slot, AMD64_CONTEXT *ctx )
{
    HRESULT (WINAPI *fn)( void *, DMUS_OBJECTDESC * ) = host_slot( host, slot );
    DMUS_OBJECTDESC *desc = (DMUS_OBJECTDESC *)(ULONG_PTR)winecom_read_arg( ctx, 1 );
    HRESULT hr;

    hr = fn( host, desc );
    if (SUCCEEDED(hr)) dmus_desc_out_fixup( desc );
    return (UINT64)(UINT)hr;
}

static UINT64 hand_dmus_parse_descriptor( void *host, UINT slot, AMD64_CONTEXT *ctx )
{
    /* ParseDescriptor(LPSTREAM, LPDMUS_OBJECTDESC): stream IN, desc OUT */
    HRESULT (WINAPI *fn)( void *, void *, DMUS_OBJECTDESC * ) = host_slot( host, slot );
    void *stream = (void *)(ULONG_PTR)winecom_read_arg( ctx, 1 );
    DMUS_OBJECTDESC *desc = (DMUS_OBJECTDESC *)(ULONG_PTR)winecom_read_arg( ctx, 2 );
    void *nstream = NULL;
    HRESULT hr;

    /* refusal hygiene by hand -- see CreateSourceVoice.  desc is the OUT
     * descriptor; zeroing dwValidData (rather than the whole struct, whose
     * dwSize the caller filled in) says NO member is valid, pStream and its
     * would-be residue included. */
    if (stream && !winecom_to_native( stream, ~0u, &nstream ))
    {
        if (desc) winecom_refused_scrub_dw( &desc->dwValidData );
        return (UINT64)(UINT)E_NOTIMPL;
    }
    hr = fn( host, nstream, desc );
    if (SUCCEEDED(hr)) dmus_desc_out_fixup( desc );
    return (UINT64)(UINT)hr;
}

static UINT64 hand_dmus_loader_getobject( void *host, UINT slot, AMD64_CONTEXT *ctx )
{
    HRESULT (WINAPI *fn)( void *, const DMUS_OBJECTDESC *, REFIID, void ** ) =
        host_slot( host, slot );
    const DMUS_OBJECTDESC *desc =
        (const DMUS_OBJECTDESC *)(ULONG_PTR)winecom_read_arg( ctx, 1 );
    REFIID riid = (REFIID)(ULONG_PTR)winecom_read_arg( ctx, 2 );
    void **ppv = (void **)(ULONG_PTR)winecom_read_arg( ctx, 3 );
    const DMUS_OBJECTDESC *use;
    DMUS_OBJECTDESC copy;
    HRESULT hr;

    /* refusal hygiene by hand -- see CreateSourceVoice */
    if (!dmus_desc_in( desc, &copy, &use, slot ))
    {
        winecom_refused_scrub_ptr( ppv );
        return (UINT64)(UINT)E_NOTIMPL;
    }
    hr = fn( host, use, riid, ppv );
    /* the ONE fail-closed choke point types the out cell by IID */
    return (UINT64)(UINT)winecom_wrap_out_iface( hr, riid, ppv );
}

static UINT64 hand_dmus_enum_object( void *host, UINT slot, AMD64_CONTEXT *ctx )
{
    HRESULT (WINAPI *fn)( void *, REFGUID, DWORD, DMUS_OBJECTDESC * ) =
        host_slot( host, slot );
    DMUS_OBJECTDESC *desc = (DMUS_OBJECTDESC *)(ULONG_PTR)winecom_read_arg( ctx, 3 );
    HRESULT hr;

    hr = fn( host, (REFGUID)(ULONG_PTR)winecom_read_arg( ctx, 1 ),
             (DWORD)winecom_read_arg( ctx, 2 ), desc );
    if (SUCCEEDED(hr)) dmus_desc_out_fixup( desc );
    return (UINT64)(UINT)hr;
}

/* ------------------------------------------- the tag-dispatched params
 *
 * GetParam/SetParam's void* payload is typed by rguidType.  Known plain
 * tags pass (bytes both sides read identically); known object tags
 * translate; an unknown tag refuses AT RUNTIME naming the GUID -- and on
 * the Get side an unknown tag's payload size is unknowable, so nothing is
 * scrubbed there: the refusal is the whole answer, said loudly, and a
 * checked caller sees E_NOTIMPL.  (Every KNOWN interface-bearing tag either
 * translates below or names the roster gap that blocks it.) */

enum dmus_tag_kind
{
    DMUS_TAG_PLAIN,        /* payload is data */
    DMUS_TAG_BAND_PARAM,   /* DMUS_BAND_PARAM: pBand member */
    DMUS_TAG_IFACE_CELL,   /* payload cell holds an interface pointer (Get) */
    DMUS_TAG_IFACE_SELF,   /* pParam IS the interface pointer (Set) */
    DMUS_TAG_UNSERVABLE,   /* names a roster gap; refused with that reason */
};

static const struct dmus_tag
{
    const GUID *tag;
    enum dmus_tag_kind kind;
    UINT iface;            /* roster index for the object kinds */
    const char *note;      /* the UNSERVABLE reason */
}
dmus_tags[] =
{
    { &GUID_PerfMasterTempo,        DMUS_TAG_PLAIN, 0, NULL },
    { &GUID_PerfMasterVolume,       DMUS_TAG_PLAIN, 0, NULL },
    { &GUID_PerfMasterGrooveLevel,  DMUS_TAG_PLAIN, 0, NULL },
    { &GUID_PerfAutoDownload,       DMUS_TAG_PLAIN, 0, NULL },
    { &GUID_TempoParam,             DMUS_TAG_PLAIN, 0, NULL },
    { &GUID_TimeSignature,          DMUS_TAG_PLAIN, 0, NULL },
    { &GUID_ChordParam,             DMUS_TAG_PLAIN, 0, NULL },
    { &GUID_RhythmParam,            DMUS_TAG_PLAIN, 0, NULL },
    { &GUID_CommandParam,           DMUS_TAG_PLAIN, 0, NULL },
    { &GUID_CommandParam2,          DMUS_TAG_PLAIN, 0, NULL },
    { &GUID_CommandParamNext,       DMUS_TAG_PLAIN, 0, NULL },
    { &GUID_MuteParam,              DMUS_TAG_PLAIN, 0, NULL },
    { &GUID_Valid_Start_Time,       DMUS_TAG_PLAIN, 0, NULL },
    { &GUID_Play_Marker,            DMUS_TAG_PLAIN, 0, NULL },
    { &GUID_SeedVariations,         DMUS_TAG_PLAIN, 0, NULL },
    { &GUID_Variations,             DMUS_TAG_PLAIN, 0, NULL },
    { &GUID_DisableTempo,           DMUS_TAG_PLAIN, 0, NULL },
    { &GUID_EnableTempo,            DMUS_TAG_PLAIN, 0, NULL },
    { &GUID_DisableTimeSig,         DMUS_TAG_PLAIN, 0, NULL },
    { &GUID_EnableTimeSig,          DMUS_TAG_PLAIN, 0, NULL },
    { &GUID_Disable_Auto_Download,  DMUS_TAG_PLAIN, 0, NULL },
    { &GUID_Enable_Auto_Download,   DMUS_TAG_PLAIN, 0, NULL },
    { &GUID_Clear_All_Bands,        DMUS_TAG_PLAIN, 0, NULL },
    { &GUID_StandardMIDIFile,       DMUS_TAG_PLAIN, 0, NULL },
    { &GUID_BandParam,              DMUS_TAG_BAND_PARAM,
      SYSCOM_IFACE_IDirectMusicBand, NULL },
    { &GUID_IDirectMusicBand,       DMUS_TAG_IFACE_CELL,
      SYSCOM_IFACE_IDirectMusicBand, NULL },
    { &GUID_Download,               DMUS_TAG_IFACE_SELF, 0, NULL },
    { &GUID_Unload,                 DMUS_TAG_IFACE_SELF, 0, NULL },
    { &GUID_DownloadToAudioPath,    DMUS_TAG_IFACE_SELF, 0, NULL },
    { &GUID_UnloadFromAudioPath,    DMUS_TAG_IFACE_SELF, 0, NULL },
    { &GUID_ConnectToDLSCollection, DMUS_TAG_UNSERVABLE, 0,
      "IDirectMusicCollection is not on the roster (nothing can vend one "
      "to a guest either -- the same gap, said once)" },
    { &GUID_IDirectMusicChordMap,   DMUS_TAG_UNSERVABLE, 0,
      "IDirectMusicChordMap is not on the roster" },
    { &GUID_IDirectMusicStyle,      DMUS_TAG_UNSERVABLE, 0,
      "IDirectMusicStyle is not on the roster" },
};

static const struct dmus_tag *dmus_find_tag( const GUID *tag )
{
    UINT i;

    for (i = 0; i < ARRAYSIZE(dmus_tags); i++)
        if (IsEqualGUID( dmus_tags[i].tag, tag )) return &dmus_tags[i];
    return NULL;
}

static BOOL dmus_tag_refuse( const GUID *tag, const struct dmus_tag *t,
                             const char *dir )
{
    static LONG logged;

    if (!InterlockedExchange( &logged, 1 ))
        FIXME( "syscom: %s with tag %s: %s -- refusing this tag (every other "
               "known tag is served)\n", dir, debugstr_guid( tag ),
               t ? t->note : "a tag this walker does not know; add it to "
                             "dmus_tags[] with its payload's shape" );
    return FALSE;
}

/* Get side: returns the wrapped-up payload treatment.  pParam is the guest's
 * own buffer; native fills it, and the object kinds are wrapped after. */
static UINT64 dmus_getparam_call( void *host, UINT slot, const GUID *tag,
                                  void *pParam, HRESULT hr )
{
    const struct dmus_tag *t = dmus_find_tag( tag );

    if (FAILED(hr)) return (UINT64)(UINT)hr;
    if (!t || t->kind == DMUS_TAG_UNSERVABLE)
    {
        /* the call ALREADY ran for a tag this walker cannot translate --
         * that must not happen; the callers below check first */
        return (UINT64)(UINT)hr;
    }
    switch (t->kind)
    {
    case DMUS_TAG_BAND_PARAM:
    {
        DMUS_BAND_PARAM *bp = pParam;
        if (bp && bp->pBand)
            bp->pBand = winecom_wrap( bp->pBand, t->iface );
        break;
    }
    case DMUS_TAG_IFACE_CELL:
    {
        void **cell = pParam;
        if (cell && *cell) *cell = winecom_wrap( *cell, t->iface );
        break;
    }
    default:
        break;
    }
    return (UINT64)(UINT)hr;
}

/* Set side: translate the payload IN.  Band params clone the struct;
 * pointer-self tags unwrap the pointer value itself. */
static BOOL dmus_setparam_in( const GUID *tag, void **pparam_io,
                              DMUS_BAND_PARAM *bp_copy )
{
    const struct dmus_tag *t = dmus_find_tag( tag );
    void *native;

    if (!t || t->kind == DMUS_TAG_UNSERVABLE)
        return dmus_tag_refuse( tag, t, "SetParam" );
    switch (t->kind)
    {
    case DMUS_TAG_BAND_PARAM:
    {
        const DMUS_BAND_PARAM *bp = *pparam_io;
        if (!bp) return TRUE;
        *bp_copy = *bp;
        if (bp->pBand)
        {
            if (!winecom_to_native( bp->pBand, ~0u, &native ))
                return dmus_tag_refuse( tag, t, "SetParam" );
            bp_copy->pBand = native;
        }
        *pparam_io = bp_copy;
        return TRUE;
    }
    case DMUS_TAG_IFACE_SELF:
        if (*pparam_io)
        {
            if (!winecom_to_native( *pparam_io, ~0u, &native ))
                return dmus_tag_refuse( tag, t, "SetParam" );
            *pparam_io = native;
        }
        return TRUE;
    default:
        return TRUE;
    }
}

static BOOL dmus_getparam_ok( const GUID *tag )
{
    const struct dmus_tag *t = dmus_find_tag( tag );

    if (!t || t->kind == DMUS_TAG_UNSERVABLE)
        return dmus_tag_refuse( tag, t, "GetParam" );
    return TRUE;
}

/* GetParam(REFGUID, DWORD, DWORD, MUSIC_TIME, MUSIC_TIME*, void*) */
static UINT64 hand_dmus_getparam_p6( void *host, UINT slot, AMD64_CONTEXT *ctx )
{
    HRESULT (WINAPI *fn)( void *, const GUID *, DWORD, DWORD, MUSIC_TIME,
                          MUSIC_TIME *, void * ) = host_slot( host, slot );
    const GUID *tag = (const GUID *)(ULONG_PTR)winecom_read_arg( ctx, 1 );
    void *pParam = (void *)(ULONG_PTR)winecom_read_arg( ctx, 6 );

    /* refusal hygiene by hand -- see CreateSourceVoice.  Only pmtNext: the
     * payload's size is unknowable for the very tag being refused (the tag
     * banner above), so it is the one out this file deliberately leaves. */
    if (!tag || !dmus_getparam_ok( tag ))
    {
        winecom_refused_scrub_dw( (void *)(ULONG_PTR)winecom_read_arg( ctx, 5 ) );
        return (UINT64)(UINT)E_NOTIMPL;
    }
    return dmus_getparam_call( host, slot, tag, pParam,
        fn( host, tag, (DWORD)winecom_read_arg( ctx, 2 ),
            (DWORD)winecom_read_arg( ctx, 3 ),
            (MUSIC_TIME)winecom_read_arg( ctx, 4 ),
            (MUSIC_TIME *)(ULONG_PTR)winecom_read_arg( ctx, 5 ),
            pParam ) );
}

/* SetParam(REFGUID, DWORD, DWORD, MUSIC_TIME, void*) */
static UINT64 hand_dmus_setparam_p5( void *host, UINT slot, AMD64_CONTEXT *ctx )
{
    HRESULT (WINAPI *fn)( void *, const GUID *, DWORD, DWORD, MUSIC_TIME,
                          void * ) = host_slot( host, slot );
    const GUID *tag = (const GUID *)(ULONG_PTR)winecom_read_arg( ctx, 1 );
    void *pParam = (void *)(ULONG_PTR)winecom_read_arg( ctx, 5 );
    DMUS_BAND_PARAM bp_copy;

    if (!tag || !dmus_setparam_in( tag, &pParam, &bp_copy ))
        return (UINT64)(UINT)E_NOTIMPL;
    return (UINT64)(UINT)fn( host, tag, (DWORD)winecom_read_arg( ctx, 2 ),
                             (DWORD)winecom_read_arg( ctx, 3 ),
                             (MUSIC_TIME)winecom_read_arg( ctx, 4 ), pParam );
}

/* GetParam(REFGUID, MUSIC_TIME, MUSIC_TIME*, void*) -- the track shape */
static UINT64 hand_dmus_getparam_t4( void *host, UINT slot, AMD64_CONTEXT *ctx )
{
    HRESULT (WINAPI *fn)( void *, const GUID *, MUSIC_TIME, MUSIC_TIME *,
                          void * ) = host_slot( host, slot );
    const GUID *tag = (const GUID *)(ULONG_PTR)winecom_read_arg( ctx, 1 );
    void *pParam = (void *)(ULONG_PTR)winecom_read_arg( ctx, 4 );

    /* refusal hygiene by hand -- see CreateSourceVoice (pmtNext only) */
    if (!tag || !dmus_getparam_ok( tag ))
    {
        winecom_refused_scrub_dw( (void *)(ULONG_PTR)winecom_read_arg( ctx, 3 ) );
        return (UINT64)(UINT)E_NOTIMPL;
    }
    return dmus_getparam_call( host, slot, tag, pParam,
        fn( host, tag, (MUSIC_TIME)winecom_read_arg( ctx, 2 ),
            (MUSIC_TIME *)(ULONG_PTR)winecom_read_arg( ctx, 3 ), pParam ) );
}

/* SetParam(REFGUID, MUSIC_TIME, void*) -- the track shape */
static UINT64 hand_dmus_setparam_t3( void *host, UINT slot, AMD64_CONTEXT *ctx )
{
    HRESULT (WINAPI *fn)( void *, const GUID *, MUSIC_TIME, void * ) =
        host_slot( host, slot );
    const GUID *tag = (const GUID *)(ULONG_PTR)winecom_read_arg( ctx, 1 );
    void *pParam = (void *)(ULONG_PTR)winecom_read_arg( ctx, 3 );
    DMUS_BAND_PARAM bp_copy;

    if (!tag || !dmus_setparam_in( tag, &pParam, &bp_copy ))
        return (UINT64)(UINT)E_NOTIMPL;
    return (UINT64)(UINT)fn( host, tag,
                             (MUSIC_TIME)winecom_read_arg( ctx, 2 ), pParam );
}

/* GetParamEx(REFGUID, DWORD, DWORD, DWORD, MUSIC_TIME, MUSIC_TIME*, void*) */
static UINT64 hand_dmus_getparamex( void *host, UINT slot, AMD64_CONTEXT *ctx )
{
    HRESULT (WINAPI *fn)( void *, const GUID *, DWORD, DWORD, DWORD,
                          MUSIC_TIME, MUSIC_TIME *, void * ) = host_slot( host, slot );
    const GUID *tag = (const GUID *)(ULONG_PTR)winecom_read_arg( ctx, 1 );
    void *pParam = (void *)(ULONG_PTR)winecom_read_arg( ctx, 7 );

    /* refusal hygiene by hand -- see CreateSourceVoice (pmtNext only) */
    if (!tag || !dmus_getparam_ok( tag ))
    {
        winecom_refused_scrub_dw( (void *)(ULONG_PTR)winecom_read_arg( ctx, 6 ) );
        return (UINT64)(UINT)E_NOTIMPL;
    }
    return dmus_getparam_call( host, slot, tag, pParam,
        fn( host, tag, (DWORD)winecom_read_arg( ctx, 2 ),
            (DWORD)winecom_read_arg( ctx, 3 ),
            (DWORD)winecom_read_arg( ctx, 4 ),
            (MUSIC_TIME)winecom_read_arg( ctx, 5 ),
            (MUSIC_TIME *)(ULONG_PTR)winecom_read_arg( ctx, 6 ), pParam ) );
}

static UINT64 hand_dmus_init_audio( void *host, UINT slot, AMD64_CONTEXT *ctx )
{
    /* InitAudio(IDirectMusic**, IDirectSound**, HWND, DWORD, DWORD, DWORD,
     * DMUS_AUDIOPARAMS*): the two interface cells are IN-OUT -- a non-NULL
     * incoming value is the caller's own object (unwrapped), and whatever
     * comes back is wrapped.  Local cells keep the guest's memory unmutated
     * until the wrapped answer is ready. */
    HRESULT (WINAPI *fn)( void *, void **, void **, HWND, DWORD, DWORD,
                          DWORD, void * ) = host_slot( host, slot );
    void **pdmus = (void **)(ULONG_PTR)winecom_read_arg( ctx, 1 );
    void **pdsound = (void **)(ULONG_PTR)winecom_read_arg( ctx, 2 );
    void *dmus_cell = NULL, *dsound_cell = NULL;
    HRESULT hr;

    if (pdmus && *pdmus && !winecom_to_native( *pdmus, ~0u, &dmus_cell ))
        return (UINT64)(UINT)E_NOTIMPL;
    if (pdsound && *pdsound && !winecom_to_native( *pdsound, ~0u, &dsound_cell ))
        return (UINT64)(UINT)E_NOTIMPL;
    hr = fn( host, pdmus ? &dmus_cell : NULL, pdsound ? &dsound_cell : NULL,
             (HWND)(ULONG_PTR)winecom_read_arg( ctx, 3 ),
             (DWORD)winecom_read_arg( ctx, 4 ),
             (DWORD)winecom_read_arg( ctx, 5 ),
             (DWORD)winecom_read_arg( ctx, 6 ),
             (void *)(ULONG_PTR)winecom_read_arg( ctx, 7 ) );
    if (SUCCEEDED(hr))
    {
        if (pdmus)
            *pdmus = dmus_cell
                ? winecom_wrap( dmus_cell, SYSCOM_IFACE_IDirectMusic ) : NULL;
        if (pdsound)
            *pdsound = dsound_cell
                ? winecom_wrap( dsound_cell, SYSCOM_IFACE_IDirectSound ) : NULL;
    }
    return (UINT64)(UINT)hr;
}

/* (this, float, UINT32, const GUID *) -> HRESULT: OnSimpleVolumeChanged's
 * forward form; the row's real value is the complete plan it gives the
 * REVERSE direction, which is where a session-events sink is actually
 * called. */
static UINT64 hand_f_i_i( void *host, UINT slot, AMD64_CONTEXT *ctx )
{
    HRESULT (WINAPI *fn)( void *, float, UINT32, const void * ) = host_slot( host, slot );

    return (UINT64)(UINT)fn( host, read_float_arg( ctx, 1 ),
                             (UINT32)winecom_read_arg( ctx, 2 ),
                             (const void *)(ULONG_PTR)winecom_read_arg( ctx, 3 ) );
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
    /* the 2026-09-01 completeness pass: owned-family HAND_SLOTS order */
    hand_nlm_get_network,
    hand_nlm_get_network_connection,
    hand_propkey_byval,
    hand_propstore_setvalue,
    hand_enum_next_variant,
    /* FP_SHAPES, sorted by shape key: fi>i then fii>i */
    hand_f_i,
    hand_f_i_i,
    /* LEGACY_HAND, first-mention order (the generator dedups) */
    hand_enum_next_unknown,
    hand_enum_next_moniker,
    hand_enum_next_cp,
    hand_enum_next_connectdata,
    hand_multi_qi,
    hand_dispatch_invoke,
    hand_typecomp_bind,
    hand_dmus_stamp_pmsg,
    hand_dmus_send_pmsg,
    hand_dmus_alloc_pmsg,
    hand_dmus_free_pmsg,
    hand_dmus_clone_pmsg,
    hand_dmus_process_pmsg,
    hand_dmus_flush,
    hand_dmus_get_notification_pmsg,
    hand_dmus_loader_getobject,
    hand_dmus_objdesc_in,
    hand_dmus_enum_object,
    hand_dmus_objdesc_out,
    hand_dmus_parse_descriptor,
    hand_dmus_getparam_p6,
    hand_dmus_setparam_p5,
    hand_dmus_getparamex,
    hand_dmus_init_audio,
    hand_dmus_getparam_t4,
    hand_dmus_setparam_t3,
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

/* combase runs its OWN table dispatcher for the [local] interfaces, over the
 * same generated rows -- so its refusal exits owe the same scrub the runtime's
 * do, and until now paid none of it (winecom's was private).  This reads the
 * arguments back out of the trap frame exactly as the dispatcher would and
 * hands them to the shared, lever-honouring mask scrub. */
static void local_scrub_refused( const struct winecom_slot *sl, AMD64_CONTEXT *ctx )
{
    UINT64 raw[16] = { 0 };
    UINT i;

    for (i = 1; i < sl->argc && i < ARRAYSIZE(raw); i++)
        raw[i] = winecom_read_arg( ctx, i );
    winecom_refused_scrub_slot( sl, raw, FALSE );
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
        local_scrub_refused( sl, ctx );
        ctx->Rax = (UINT)E_INVALIDARG;
        return STATUS_SUCCESS;
    }

    if (sl->refuse)
    {
        local_refuse_once( iface, slot, sl->name, sl->refuse );
        local_scrub_refused( sl, ctx );
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
                local_scrub_refused( sl, ctx );
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
            local_scrub_refused( sl, ctx );
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

/* Every __wine_guest_* export below opens with the same not-ready refusal --
 * E_FAIL because this surface never attached, an error THIS side invented --
 * so each one that has an out-param scrubs it there, and again at whatever
 * marshal refusal it owns.  winecom_guest32() deliberately reads process
 * state rather than attach state, so the scrub is correct before attach. */
HRESULT WINAPI __wine_guest_CoCreateInstance( REFCLSID rclsid, IUnknown *outer,
                                              DWORD ctx, REFIID riid, void **ppv )
{
    HRESULT hr;

    if (!syscom_ready())
    {
        winecom_refused_scrub_ptr( ppv );
        return E_FAIL;
    }
    if ((hr = syscom_xaudio2_class_gate( rclsid )) != S_OK)
    {
        winecom_refused_scrub_ptr( ppv );
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
        winecom_refused_scrub_ptr( ppv );
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

    /* refusal hygiene by hand -- see __wine_guest_CoCreateInstance */
    if (!syscom_ready())
    {
        winecom_refused_scrub_ptr( ppMalloc );
        return E_FAIL;
    }
    hr = CoGetMalloc( context, ppMalloc );
    return __wine_com_wrap_out_iface( hr, &IID_IMalloc, (void **)ppMalloc );
}

HRESULT WINAPI __wine_guest_CoGetClassObject( REFCLSID rclsid, DWORD ctx,
                                              COSERVERINFO *info, REFIID riid,
                                              void **ppv )
{
    HRESULT hr;

    /* refusal hygiene by hand -- see __wine_guest_CoCreateInstance */
    if (!syscom_ready())
    {
        winecom_refused_scrub_ptr( ppv );
        return E_FAIL;
    }
    if ((hr = syscom_xaudio2_class_gate( rclsid )) != S_OK)
    {
        winecom_refused_scrub_ptr( ppv );
        return hr;
    }
    hr = CoGetClassObject( rclsid, ctx, info, riid, ppv );
    return __wine_com_wrap_out_iface( hr, riid, ppv );
}

HRESULT WINAPI __wine_guest_CreateStreamOnHGlobal( HGLOBAL hglobal, BOOL delete_on_release,
                                                   IStream **out )
{
    HRESULT hr;

    /* refusal hygiene by hand -- see __wine_guest_CoCreateInstance */
    if (!syscom_ready())
    {
        winecom_refused_scrub_ptr( out );
        return E_FAIL;
    }
    hr = CreateStreamOnHGlobal( hglobal, delete_on_release, out );
    if (SUCCEEDED(hr))
        __wine_com_wrap_static( (void **)out, SYSCOM_IFACE_IStream );
    return hr;
}

HRESULT WINAPI __wine_guest_GetHGlobalFromStream( IStream *stream, HGLOBAL *phglobal )
{
    void *host;

    /* refusal hygiene by hand -- see __wine_guest_CoCreateInstance */
    if (!syscom_ready())
    {
        winecom_refused_scrub_ptr( phglobal );
        return E_FAIL;
    }
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
        winecom_refused_scrub_ptr( phglobal );
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
        /* TWO DIFFERENT THINGS REACH HERE, and the message must not name only
         * one of them -- __wine_com_translate_in answers TRUE for NULL and for
         * a forward proxy of ours, and FALSE for everything else.
         *
         *   1. A genuinely guest-implemented object.  v1 refuses rather than
         *      guessing who owns a Release run through a borrowed reverse
         *      proxy: a design decision nobody has made yet.
         *   2. A pointer that WAS one of our proxies and is not any more --
         *      a guest that over-released it, so the intern entry is gone and
         *      proxy_from_pointer no longer recognises the address.
         *
         * [MEASURED 2026-08-29] (2) is reachable: a probe that clears the same
         * proxy twice takes this path on the second clear, and correctly gets
         * E_NOTIMPL with the VARIANT untouched rather than a use-after-free.
         * Naming only (1) sent a reader toward reverse-proxy ownership design
         * when the actual defect would be a guest refcount bug -- so say both,
         * and say which is the likelier reading when a real title trips it. */
        FIXME( "syscom: VariantClear refuses %s %p (vt %#x): either a "
               "guest-implemented object (Release through a reverse proxy has "
               "ownership semantics nobody has designed yet), or -- more "
               "likely if a real title reaches this -- a pointer that was one "
               "of our proxies and has already been released once too often\n",
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

#ifdef __powerpc64__   /* the guest-facing wrappers below serve the 64-bit
 * thunk lane only; the i386 lane is real WoW64 (no GUEST-IMPL consumers), and
 * compiling them there pulls dllimport stubs into collision with combase's
 * own definitions.  The spec lines carry -arch=ppc64 to match. */

/* --------------------------------------------- the PROPVARIANT trio
 *
 * PropVariantClear / PropVariantCopy / FreePropVariantArray were on the
 * GUEST-REFUSE list, and The Witcher 3's run log showed the refusal firing
 * (the 2026-08-31 seat run: err:combase:__wine_com_refuse x17 with
 * PropVariantClear among the traced names).  PROPVARIANT is VARIANT's
 * sibling tagged union -- vt at offset 0, payload at offset 8, 24 bytes on
 * this pair of ABIs, the same measured layout the VariantClear wrapper above
 * leans on -- and the same discipline applies: scalars, strings, blobs and
 * plain vectors are bytes both sides read identically, so those pass
 * through; the interface-bearing tags are the ones that must never reach
 * native unclassified.
 *
 * Native reference for every branch: dlls/combase/combase.c PropVariantClear
 * (propvar_validatetype first, memset-to-zero on both the failure and the
 * success path).  The interface-bearing tag set is native's own case list:
 * VT_DISPATCH, VT_UNKNOWN, VT_STREAM, VT_STREAMED_OBJECT, VT_STORAGE,
 * VT_STORED_OBJECT -- all of which native releases through the one pStream
 * slot. */

static BOOL syscom_propvt_iface( VARTYPE vt )
{
    switch (vt)
    {
    case VT_DISPATCH:
    case VT_UNKNOWN:
    case VT_STREAM:
    case VT_STREAMED_OBJECT:
    case VT_STORAGE:
    case VT_STORED_OBJECT:
        return TRUE;
    }
    return FALSE;
}

HRESULT WINAPI __wine_guest_PropVariantClear( PROPVARIANT *pvar )
{
    if (!syscom_ready()) return E_FAIL;
    if (!pvar) return S_OK;                    /* native: S_OK on NULL */

    if (syscom_propvt_iface( pvar->vt ))
    {
        IUnknown *punk = (IUnknown *)pvar->pStream;
        void *host;

        if (!punk)
        {
            /* native's release branch guards on the pointer and then falls
             * through to the memset; mirror both halves */
            memset( pvar, 0, sizeof(*pvar) );
            return S_OK;
        }
        if (__wine_com_translate_in( punk, &host ))
        {
            /* One of our forward proxies: the guest-visible reference is the
             * one this clear consumes, and only the guest-side release knows
             * how to drop it without touching the single host reference the
             * proxy owns for its whole life -- the VariantClear wrapper's
             * VT_UNKNOWN reasoning, verbatim. */
            __wine_com_release_guest( punk );
            memset( pvar, 0, sizeof(*pvar) );
            return S_OK;
        }
        FIXME( "syscom: PropVariantClear refuses vt %#x over %p: either a "
               "guest-implemented object (Release ownership through a "
               "reverse proxy is undesigned) or a proxy already released "
               "once too often; the PROPVARIANT is left untouched\n",
               pvar->vt, punk );
        return E_NOTIMPL;
    }
    if ((pvar->vt & VT_VECTOR) && (pvar->vt & VT_TYPEMASK) == VT_VARIANT)
    {
        /* a vector of VARIANTs may hold VT_UNKNOWN elements native would
         * Release; v1 does not recurse into elements, same stance as the
         * VariantClear wrapper's SAFEARRAY branch */
        FIXME( "syscom: PropVariantClear refuses VT_VECTOR|VT_VARIANT %p: "
               "elements could carry interface pointers and v1 does not "
               "recurse\n", pvar );
        return E_NOTIMPL;
    }
    /* everything else -- scalars, strings, blobs, CF, CLSID, plain vectors,
     * and any vt native's own validator will refuse-and-zero */
    return PropVariantClear( pvar );
}

HRESULT WINAPI __wine_guest_PropVariantCopy( PROPVARIANT *dest, const PROPVARIANT *src )
{
    /* refusal hygiene by hand -- see __wine_guest_CoCreateInstance.  dest is
     * a pure OUT: unlike PropVariantClear's in-out pvar (which already holds
     * the guest's own valid value and is left alone on every refusal here),
     * an unwritten dest is stack residue an unchecked caller PropVariantClears.
     * Zeroed is VT_EMPTY, which clears to nothing. */
    if (!syscom_ready())
    {
        winecom_refused_scrub_mem( dest, sizeof(*dest) );
        return E_FAIL;
    }
    if (!dest || !src) return E_INVALIDARG;

    if (syscom_propvt_iface( src->vt ))
    {
        IUnknown *punk = (IUnknown *)src->pStream;
        void *host;

        if (!punk || __wine_com_translate_in( punk, &host ))
        {
            /* Copying an interface-bearing PROPVARIANT is one AddRef plus a
             * struct copy (native: *pvarDest = *pvarSrc; AddRef).  For a
             * forward proxy the reference that must grow is the GUEST-side
             * one -- the host reference stays the proxy's single one. */
            *dest = *src;
            if (punk) __wine_com_addref_guest( punk );
            return S_OK;
        }
        FIXME( "syscom: PropVariantCopy refuses vt %#x over %p: a "
               "guest-implemented object's AddRef runs guest code this "
               "wrapper cannot enter\n", src->vt, punk );
        winecom_refused_scrub_mem( dest, sizeof(*dest) );
        return E_NOTIMPL;
    }
    if ((src->vt & VT_VECTOR) && (src->vt & VT_TYPEMASK) == VT_VARIANT)
    {
        FIXME( "syscom: PropVariantCopy refuses VT_VECTOR|VT_VARIANT %p: "
               "elements could carry interface pointers and v1 does not "
               "recurse\n", src );
        winecom_refused_scrub_mem( dest, sizeof(*dest) );
        return E_NOTIMPL;
    }
    return PropVariantCopy( dest, src );
}

HRESULT WINAPI __wine_guest_FreePropVariantArray( ULONG cnt, PROPVARIANT *rgvars )
{
    ULONG i;
    HRESULT hr = S_OK, first = S_OK;

    if (!syscom_ready()) return E_FAIL;
    if (!rgvars) return cnt ? E_INVALIDARG : S_OK;
    /* native (combase.c FreePropVariantArray): clear every element, report
     * the FIRST failure; a refused element here leaves that element
     * untouched, exactly as its own wrapper promises */
    for (i = 0; i < cnt; i++)
    {
        hr = __wine_guest_PropVariantClear( &rgvars[i] );
        if (FAILED( hr ) && first == S_OK) first = hr;
    }
    return first;
}

/* ------------------------------------------------- CoSetProxyBlanket
 *
 * Was GUEST-REFUSE; The Witcher 3 calls it on boot (the 2026-08-31 run log's
 * `L"ole32.dll".CoSetProxyBlanket is refused` line -- ole32's thunk forwards
 * here).  The signature is one interface IN plus plain integers/pointers:
 * translate the proxy and let native answer.  For the in-process objects
 * this lane serves, native's own answer is E_NOINTERFACE (no proxy manager),
 * which is precisely what Windows says for a non-DCOM object -- an honest
 * answer a caller must already handle, where the refusal stub's E_NOTIMPL
 * was an invented one. */
HRESULT WINAPI __wine_guest_CoSetProxyBlanket( IUnknown *proxy, DWORD authn_svc,
                                               DWORD authz_svc, OLECHAR *server,
                                               DWORD authn_level, DWORD imp_level,
                                               void *auth_info, DWORD capabilities )
{
    void *host;
    HRESULT hr;

    if (!syscom_ready()) return E_FAIL;
    if (!winecom_to_native( proxy, ~0u, &host ))
    {
        FIXME( "syscom: CoSetProxyBlanket on the guest-implemented object %p "
               "is refused: no native identity exists to set a blanket on\n",
               proxy );
        return E_NOTIMPL;
    }
    hr = CoSetProxyBlanket( (IUnknown *)host, authn_svc, authz_svc, server,
                            authn_level, imp_level, auth_info, capabilities );
    if (host) winecom_to_native_end( host );
    return hr;
}

/* --------------------------------------- the error-info flat family
 *
 * ICreateErrorInfo and IErrorInfo are BOTH on the roster, so these five
 * flats were refusals out of nothing but queue order: each is one
 * translate/wrap on machinery that already exists (the CoGetMalloc shape).
 * Games use Get/SetErrorInfo around every rich-error COM failure. */
HRESULT WINAPI __wine_guest_CreateErrorInfo( ICreateErrorInfo **pperrinfo )
{
    HRESULT hr;

    /* refusal hygiene by hand -- see __wine_guest_CoCreateInstance */
    if (!syscom_ready())
    {
        winecom_refused_scrub_ptr( pperrinfo );
        return E_FAIL;
    }
    hr = CreateErrorInfo( pperrinfo );
    return __wine_com_wrap_out_iface( hr, &IID_ICreateErrorInfo,
                                      (void **)pperrinfo );
}

HRESULT WINAPI __wine_guest_GetErrorInfo( ULONG reserved, IErrorInfo **pperrinfo )
{
    HRESULT hr;

    /* refusal hygiene by hand -- see __wine_guest_CoCreateInstance */
    if (!syscom_ready())
    {
        winecom_refused_scrub_ptr( pperrinfo );
        return E_FAIL;
    }
    hr = GetErrorInfo( reserved, pperrinfo );
    /* S_FALSE = no error info pending, *pperrinfo already NULL */
    if (hr != S_OK) return hr;
    return __wine_com_wrap_out_iface( hr, &IID_IErrorInfo, (void **)pperrinfo );
}

HRESULT WINAPI __wine_guest_SetErrorInfo( ULONG reserved, IErrorInfo *perrinfo )
{
    void *host = NULL;
    HRESULT hr;

    if (!syscom_ready()) return E_FAIL;
    if (perrinfo && !winecom_to_native( perrinfo, ~0u, &host ))
    {
        FIXME( "syscom: SetErrorInfo with the guest-implemented IErrorInfo %p "
               "is refused: the error object outlives this call and native "
               "readers would call it as x86-64\n", perrinfo );
        return E_NOTIMPL;
    }
    hr = SetErrorInfo( reserved, (IErrorInfo *)host );
    if (host) winecom_to_native_end( host );
    return hr;
}

/* the object-context pair: both are the CoCreateInstance RIID/ppv shape */
HRESULT WINAPI __wine_guest_CoGetObjectContext( REFIID riid, void **ppv )
{
    HRESULT hr;

    /* refusal hygiene by hand -- see __wine_guest_CoCreateInstance */
    if (!syscom_ready())
    {
        winecom_refused_scrub_ptr( ppv );
        return E_FAIL;
    }
    hr = CoGetObjectContext( riid, ppv );
    return __wine_com_wrap_out_iface( hr, riid, ppv );
}

HRESULT WINAPI __wine_guest_CoGetCallContext( REFIID riid, void **ppv )
{
    HRESULT hr;

    /* refusal hygiene by hand -- see __wine_guest_CoCreateInstance */
    if (!syscom_ready())
    {
        winecom_refused_scrub_ptr( ppv );
        return E_FAIL;
    }
    hr = CoGetCallContext( riid, ppv );
    return __wine_com_wrap_out_iface( hr, riid, ppv );
}

/* the translate-one-IUnknown-in trio */
HRESULT WINAPI __wine_guest_CoDisconnectObject( IUnknown *unk, DWORD reserved )
{
    void *host;
    HRESULT hr;

    if (!syscom_ready()) return E_FAIL;
    if (!winecom_to_native( unk, ~0u, &host ))
    {
        FIXME( "syscom: CoDisconnectObject on the guest-implemented object %p "
               "is refused\n", unk );
        return E_NOTIMPL;
    }
    hr = CoDisconnectObject( (IUnknown *)host, reserved );
    if (host) winecom_to_native_end( host );
    return hr;
}

HRESULT WINAPI __wine_guest_CoLockObjectExternal( IUnknown *unk, BOOL lock,
                                                  BOOL last_unlock_releases )
{
    void *host;
    HRESULT hr;

    if (!syscom_ready()) return E_FAIL;
    if (!winecom_to_native( unk, ~0u, &host ))
    {
        FIXME( "syscom: CoLockObjectExternal on the guest-implemented object "
               "%p is refused\n", unk );
        return E_NOTIMPL;
    }
    hr = CoLockObjectExternal( (IUnknown *)host, lock, last_unlock_releases );
    if (host) winecom_to_native_end( host );
    return hr;
}

HRESULT WINAPI __wine_guest_CoIsHandlerConnected( IUnknown *unk )
{
    void *host;
    HRESULT hr;

    if (!syscom_ready()) return E_FAIL;
    if (!winecom_to_native( unk, ~0u, &host ))
        return S_OK;   /* native's own answer for a local object */
    hr = CoIsHandlerConnected( (IUnknown *)host );
    if (host) winecom_to_native_end( host );
    return hr;
}

/* the HRESULT-through-a-stream helpers: IStream is rostered, the payload is
 * four plain bytes */
HRESULT WINAPI __wine_guest_CoMarshalHresult( IStream *stm, HRESULT hresult )
{
    void *host;
    HRESULT hr;

    if (!syscom_ready()) return E_FAIL;
    if (!winecom_to_native( stm, ~0u, &host ))
    {
        FIXME( "syscom: CoMarshalHresult into the guest-implemented stream %p "
               "is refused\n", stm );
        return E_NOTIMPL;
    }
    hr = CoMarshalHresult( (IStream *)host, hresult );
    if (host) winecom_to_native_end( host );
    return hr;
}

HRESULT WINAPI __wine_guest_CoUnmarshalHresult( IStream *stm, HRESULT *phresult )
{
    void *host;
    HRESULT hr;

    /* refusal hygiene by hand -- see __wine_guest_CoCreateInstance */
    if (!syscom_ready())
    {
        winecom_refused_scrub_dw( phresult );
        return E_FAIL;
    }
    if (!winecom_to_native( stm, ~0u, &host ))
    {
        FIXME( "syscom: CoUnmarshalHresult from the guest-implemented stream "
               "%p is refused\n", stm );
        winecom_refused_scrub_dw( phresult );
        return E_NOTIMPL;
    }
    hr = CoUnmarshalHresult( (IStream *)host, phresult );
    if (host) winecom_to_native_end( host );
    return hr;
}

#endif /* __powerpc64__ -- the 64-bit-lane flat wrappers */

/* ----------------------------------- the interface-ARRAY delivery self-test
 *
 * include/wine/winecom_arrin.h is the argument for why this exists and what
 * each half proves.  What is below is only the native half: a handful of
 * distinguishable native objects, one call to IWbemObjectSink::Indicate
 * through an ordinary vtable, and the checks that can only be made from this
 * side of the boundary.
 *
 * It lives HERE and not beside its mf twin because the ONE row on any roster
 * that carries CA_IFACE_ARR_IN is Indicate, and Indicate is on THIS surface.
 * A hook on the Media Foundation surface would have had to invent a row to
 * drive, which is a re-implementation of the arm rather than a drive of it.
 *
 * OUTSIDE the __powerpc64__ guard above, deliberately.  What that guard keeps
 * out of the i386 build is WRAPPERS OF REAL COMBASE EXPORTS, whose dllimport
 * declarations collide with the module's own definitions there; this is a new
 * export that wraps nothing, so it compiles on both lanes and its two spec
 * lines carry no -arch, exactly as the mf surface's hook does.
 */

/* A native IWbemClassObject, built slot by slot rather than by filling in
 * wbemcli.h's vtable struct.  What this object is FOR is being a
 * DISTINGUISHABLE native ppc64 vtable at a native address; exactly one of its
 * twenty-seven slots has to mean anything, and that is the one the guest calls
 * back through.  Saying so as an array of pointers -- with the width pinned to
 * the roster's own slot count below, so a regenerated table that grows the
 * interface breaks the build rather than leaving a short vtable for the guest
 * to walk off the end of -- is the honest spelling; twenty-three filled-in
 * method names would bury it. */
struct arrin_obj
{
    void *const *vtbl;
    LONG  refs;
    UINT  tag;          /* which element of the delivery this object is */
    UINT  gets;         /* how many times the guest called back through it */
    UINT  order;        /* 1-based, the order the callbacks arrived in */
};

static LONG arrin_seq;          /* the delivery's own callback counter */

static HRESULT WINAPI arrin_QueryInterface( struct arrin_obj *o, REFIID riid, void **ppv )
{
    if (!ppv) return E_POINTER;
    /* The rostered type as well as IUnknown, and the rostered type is read
     * FROM THE ROSTER rather than from a libuuid symbol: the object claims to
     * be exactly what the table says element type 81 is, so a regenerated
     * roster cannot leave this answering for an interface it no longer
     * describes. */
    if (IsEqualGUID( riid, &IID_IUnknown ) ||
        IsEqualGUID( riid, &syscom_com_ifaces[SYSCOM_IFACE_IWbemClassObject].iid ))
    {
        InterlockedIncrement( &o->refs );
        *ppv = o;
        return S_OK;
    }
    *ppv = NULL;
    return E_NOINTERFACE;
}

static ULONG WINAPI arrin_AddRef( struct arrin_obj *o )
{
    return (ULONG)InterlockedIncrement( &o->refs );
}

static ULONG WINAPI arrin_Release( struct arrin_obj *o )
{
    /* NOT freed at zero: these objects have automatic storage in the hook
     * below and outlive every reference by construction.  A count that goes
     * to zero is a FINDING here, not a lifetime event -- the hook reads it. */
    return (ULONG)InterlockedDecrement( &o->refs );
}

/* Slot 4.  Its answer is the object's identity: element k answers
 * WINECOM_ARRIN_HR(k), so a guest comparing what came back for position k
 * against what it expected for position k is making the element-wise claim
 * and nothing weaker.  The out-parameters are left alone -- refusal hygiene's
 * rule is about refused slots, and this one serves. */
static HRESULT WINAPI arrin_Get( struct arrin_obj *o, LPCWSTR name, LONG flags,
                                 VARIANT *val, CIMTYPE *type, LONG *flavor )
{
    (void)name; (void)flags; (void)val; (void)type; (void)flavor;
    if (!o->gets++) o->order = (UINT)InterlockedIncrement( &arrin_seq );
    return WINECOM_ARRIN_HR( o->tag );
}

/* Every other slot.  Reached only if the guest called a method the probe has
 * no business calling, which the hook would rather hear about than serve. */
static HRESULT WINAPI arrin_unused( void )
{
    ERR( "syscom arrin: the guest called a slot this test object does not "
         "serve; the probe is driving something it was not written to\n" );
    return E_NOTIMPL;
}

#define ARRIN_U ((void *)arrin_unused)
static void *const arrin_vtbl[] =
{
    (void *)arrin_QueryInterface,     /*  0 */
    (void *)arrin_AddRef,             /*  1 */
    (void *)arrin_Release,            /*  2 */
    ARRIN_U,                                /*  3 GetQualifierSet */
    (void *)arrin_Get,                /*  4 Get             */
    ARRIN_U, ARRIN_U, ARRIN_U, ARRIN_U,     /*  5..8  */
    ARRIN_U, ARRIN_U, ARRIN_U, ARRIN_U,     /*  9..12 */
    ARRIN_U, ARRIN_U, ARRIN_U, ARRIN_U,     /* 13..16 */
    ARRIN_U, ARRIN_U, ARRIN_U, ARRIN_U,     /* 17..20 */
    ARRIN_U, ARRIN_U, ARRIN_U, ARRIN_U,     /* 21..24 */
    ARRIN_U, ARRIN_U,                       /* 25..26 */
};
#undef ARRIN_U

/* The pin: the roster's own count for this interface.  If a regenerated table
 * grows IWbemClassObject, this build stops rather than handing the guest a
 * proxy whose vtable is wider than the object behind it. */
C_ASSERT( ARRAYSIZE(arrin_vtbl) == ARRAYSIZE(slots_IWbemClassObject) );

static void arrin_check( struct winecom_arrin_report *r, BOOL ok, const char *what )
{
    r->checks++;
    if (ok) return;
    r->failures++;
    if (!r->first_fail) r->first_fail = r->checks;
    ERR( "syscom arrin: check %u FAILED: %s\n", r->checks, what );
}

HRESULT WINAPI __wine_winecom_arrin_selftest( IWbemObjectSink *sink,
                                              struct winecom_arrin_report *report )
{
    struct arrin_obj objs[WINECOM_ARRIN_COUNT];
    IWbemClassObject *arr[WINECOM_ARRIN_COUNT], *orig[WINECOM_ARRIN_COUNT];
    UINT k, unmutated = 1, once = 1, in_order = 1;
    HRESULT hr;

    if (!sink || !report) return E_POINTER;
    memset( report, 0, sizeof(*report) );
    if (!syscom_ready()) return E_FAIL;

    arrin_seq = 0;
    for (k = 0; k < WINECOM_ARRIN_COUNT; k++)
    {
        memset( &objs[k], 0, sizeof(objs[k]) );
        objs[k].vtbl = arrin_vtbl;
        objs[k].refs = 1;
        objs[k].tag  = k;
        arr[k] = orig[k] = (IWbemClassObject *)&objs[k];
        report->refs_before += 1;
    }
    report->sent = WINECOM_ARRIN_COUNT;

    /* THE DELIVERY.  One ordinary vtable call; on the guest lane slot 3 of
     * this pointer is the reverse dispatcher, and cls_IWbemObjectSink_3 sends
     * the second parameter through the CA_IFACE_ARR_IN arm. */
    hr = IWbemObjectSink_Indicate( sink, WINECOM_ARRIN_COUNT, arr );
    report->guest_hr = (UINT)hr;
    arrin_check( report, hr == WINECOM_ARRIN_HR_OK,
                 "the sink's Indicate did not answer what the guest sink "
                 "answers, so the delivery did not arrive intact" );

    /* The caller's array is the caller's.  The arm copies into its own
     * staging precisely so that a native caller which reuses its array across
     * deliveries -- which is what wbemprox does -- is never handed proxies. */
    for (k = 0; k < WINECOM_ARRIN_COUNT; k++)
        if (arr[k] != orig[k]) unmutated = 0;
    report->array_unmutated = unmutated;
    arrin_check( report, unmutated != 0,
                 "the array the caller passed was MUTATED; the arm wrote its "
                 "proxies into the native caller's own storage" );

    for (k = 0; k < WINECOM_ARRIN_COUNT; k++)
    {
        if (objs[k].gets != 1) once = 0;
        if (objs[k].order != k + 1) in_order = 0;
    }
    report->entered_once = once;
    report->in_order = in_order;
    arrin_check( report, once != 0,
                 "an element was not called back exactly once: either an "
                 "object never arrived or two positions carry one object" );
    arrin_check( report, in_order != 0,
                 "the callbacks did not arrive in element order, so the "
                 "position an object arrived at is not the position it was "
                 "sent at" );

    /* THE EMPTY DELIVERY, which is a real shape and not an edge case: a WMI
     * query that matched nothing still calls Indicate.  The arm short-circuits
     * on it, and this is the leg that says the short circuit passes the count
     * and the NULL through rather than staging anything. */
    hr = IWbemObjectSink_Indicate( sink, 0, NULL );
    report->empty_hr = (UINT)hr;
    arrin_check( report, hr == WINECOM_ARRIN_HR_EMPTY,
                 "the empty delivery (count 0, NULL array) did not arrive as "
                 "itself" );

    /* THE REFERENCE CONTRACT.  winecom_to_guest takes a reference for the
     * guest on every element it wraps; the arm has to give every one of them
     * back when the call returns.  Read after both deliveries so a leak of one
     * per element per call shows as three. */
    for (k = 0; k < WINECOM_ARRIN_COUNT; k++)
        report->refs_after += (UINT)objs[k].refs;
    report->refs_leaked = (report->refs_after != report->refs_before);
    arrin_check( report, !report->refs_leaked,
                 "the delivery did not give back the references it took for "
                 "the guest; every element of every Indicate leaks one" );

    return report->failures ? E_FAIL : S_OK;
}

/* The guest-lane wrapper: the sink arrives as a GUEST pointer and becomes a
 * reverse proxy here, borrowed for the duration of the call.  This is the ONLY
 * difference between what the gate's guest leg measures and what a native
 * caller of the hook would measure, and it is the difference the gate is for. */
HRESULT WINAPI __wine_guest___wine_winecom_arrin_selftest(
        IWbemObjectSink *sink, struct winecom_arrin_report *report )
{
    void *native = NULL;
    HRESULT hr;

    if (!syscom_ready()) return E_FAIL;
    if (!winecom_to_native( sink, SYSCOM_IFACE_IWbemObjectSink, &native ))
    {
        ERR( "syscom arrin: the sink %p cannot be reverse-proxied, so there "
             "is nothing to deliver to\n", sink );
        return E_NOTIMPL;
    }
    hr = __wine_winecom_arrin_selftest( (IWbemObjectSink *)native, report );
    winecom_to_native_end( native );
    return hr;
}

