/*
 * DirectSound for x86-64 guests on native ppc64le Wine -- the winecom runtime
 * instance, the hand-written slots, and the flat-export wrappers.
 *
 * NOTHING HERE IMPLEMENTS DIRECTSOUND.  Wine's own dsound (dsound.c, buffer.c,
 * primary.c, mixer.c, capture.c and the rest of this directory) is the
 * implementation, unmodified, and it reaches the machine's audio through
 * mmdevapi and winealsa/winepulse exactly as it does for a native caller.
 * This file is the boundary: interface POINTERS crossing to an emulated
 * x86-64 guest are the only thing that is wrong without it, because a guest
 * handed a native vtable executes ppc64 bytes as x86-64 on its first method
 * call.
 *
 * The shape is dlls/combase/syscom.c's, and the reasons are the same:
 *
 *   * ONE winecom runtime instance for this surface.  libs/winecom's state is
 *     per-linkee by design, so dsound's proxies and indices are its own and
 *     never mix with combase's system-COM instance or d3d11's.
 *
 *   * the host invoker is a DIRECT widest-form native vtable call -- Wine's
 *     dsound objects are ordinary native PE COM objects in the same Win32
 *     world as this file, so there is no unixlib on this surface.
 *
 *   * the flat wrappers (__wine_guest_*) call the real native export through
 *     an ordinary internal call and wrap the interface pointers it returned.
 *     spec2thunk's GUEST-IMPL redirect points the GUEST export's resolution at
 *     the wrapper (dsound.thunks); the plain-named export is untouched, so
 *     native ppc64 callers of DirectSoundCreate8 get what they always got.
 *
 * WHAT IS HAND-WRITTEN AND WHY (dsound_marshal.h names them in order):
 *
 *   * the three creators that end in an aggregation `pUnkOuter`.  That
 *     argument is an IUnknown the CALLER implements: a guest one would need a
 *     reverse proxy, which this port does not have.  A NULL one needs nothing,
 *     and NULL is what every caller that is not building an aggregate passes,
 *     so these serve NULL and refuse the rest BY NAME rather than refusing the
 *     method outright.
 *
 *   * the DirectSound3D setters, which pass D3DVALUEs by value.  MS-x64 puts
 *     the first four arguments in XMM0..XMM3 when they are floating point and
 *     on the stack after that; the generic invoker calls with integer
 *     registers only, so a float travelling through it would arrive in the
 *     wrong register file entirely.  These are keyed by ARGUMENT SHAPE rather
 *     than by name (gen_winecom.py's FP_SHAPES), so one function serves
 *     SetPosition, SetVelocity and SetConeOrientation on both 3D interfaces.
 *
 * Copyright 2026 the ppc64le port authors
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 */

#include <stdarg.h>

#include "windef.h"
#include "winbase.h"
#include "winuser.h"
#include "winternl.h"
#include "mmsystem.h"
#include "d3dtypes.h"
#include "dsound.h"
#include "wine/debug.h"
#include "wine/winecom.h"

#include "dsound_marshal.h"

WINE_DEFAULT_DEBUG_CHANNEL(dsound);

/* ------------------------------------------------------- the host invoker */

/* A direct widest-form native vtable call: the host's vtable slot with up to
 * 16 ULONG_PTR arguments (args[0] is `this`).  ELFv2 callees ignore the
 * excess, so one shape serves every integer-only slot -- the same trick
 * dlls/combase/syscom.c's invoker uses, for the same reason: these are
 * ordinary native COM objects, not a unixlib away. */
static UINT64 dsound_invoke( void *host, UINT slot, UINT argc, UINT64 *args )
{
    void **vtbl = *(void ***)host;

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

/* The guest's argument n as a float.  MS-x64 assigns registers BY POSITION:
 * argument n travels in XMMn when n < 4 and it is floating point, and on the
 * stack after that -- in the low half of the same eight-byte slot an integer
 * would have used.  That is why IDirectSound3DListener::SetOrientation, whose
 * fifth and sixth floats are past XMM3, needs the stack half of this. */
static float read_float_arg( const AMD64_CONTEXT *ctx, UINT n )
{
    if (n < 4) return *(const float *)&ctx->FltSave.XmmRegisters[n];
    return *(const float *)(ULONG_PTR)(ctx->Rsp + 8 + n * (UINT64)8);
}

/* ------------------------------------------------------ hand-written slots */

/* (this, float, DWORD) -> HRESULT.  IDirectSound3DListener::SetDistanceFactor,
 * SetDopplerFactor and SetRolloffFactor; IDirectSound3DBuffer::SetMinDistance
 * and SetMaxDistance. */
static UINT64 hand_f_i( void *host, UINT slot, AMD64_CONTEXT *ctx )
{
    HRESULT (WINAPI *fn)( void *, D3DVALUE, DWORD ) = host_slot( host, slot );

    return (UINT64)(UINT)fn( host, read_float_arg( ctx, 1 ),
                             (DWORD)winecom_read_arg( ctx, 2 ) );
}

/* (this, float, float, float, DWORD) -> HRESULT.  SetPosition, SetVelocity and
 * SetConeOrientation on both 3D interfaces. */
static UINT64 hand_fff_i( void *host, UINT slot, AMD64_CONTEXT *ctx )
{
    HRESULT (WINAPI *fn)( void *, D3DVALUE, D3DVALUE, D3DVALUE, DWORD ) =
        host_slot( host, slot );

    return (UINT64)(UINT)fn( host, read_float_arg( ctx, 1 ),
                             read_float_arg( ctx, 2 ),
                             read_float_arg( ctx, 3 ),
                             (DWORD)winecom_read_arg( ctx, 4 ) );
}

/* (this, 6x float, DWORD) -> HRESULT.  IDirectSound3DListener::SetOrientation
 * only: the fourth, fifth and sixth floats are past XMM3 and arrive on the
 * guest's stack. */
static UINT64 hand_ffffff_i( void *host, UINT slot, AMD64_CONTEXT *ctx )
{
    HRESULT (WINAPI *fn)( void *, D3DVALUE, D3DVALUE, D3DVALUE,
                          D3DVALUE, D3DVALUE, D3DVALUE, DWORD ) =
        host_slot( host, slot );

    return (UINT64)(UINT)fn( host, read_float_arg( ctx, 1 ),
                             read_float_arg( ctx, 2 ),
                             read_float_arg( ctx, 3 ),
                             read_float_arg( ctx, 4 ),
                             read_float_arg( ctx, 5 ),
                             read_float_arg( ctx, 6 ),
                             (DWORD)winecom_read_arg( ctx, 7 ) );
}

/* The one refusal this file makes by name, shared by every slot and every flat
 * export that takes an aggregation outer IUnknown.  It is a REFUSAL and not a
 * silent NULL: an application that asked for aggregation and got a
 * non-aggregated object back would corrupt its own reference counting, and
 * DSERR_NOAGGREGATION is what DirectSound already returns for the case it
 * cannot serve. */
static HRESULT refuse_aggregation( const char *what, void *outer )
{
    FIXME( "dsound: %s with a non-NULL pUnkOuter %p is refused: aggregation "
           "hands native code a GUEST-implemented IUnknown, which needs the "
           "reverse-proxy direction (system-com-design.md 6) this port does "
           "not have yet\n", what, outer );
    return DSERR_NOAGGREGATION;
}

/* (this, LPCDSBUFFERDESC, IDirectSoundBuffer **, IUnknown *) -> HRESULT.
 * IDirectSound::CreateSoundBuffer and IDirectSound8's identical restatement. */
static UINT64 hand_create_sound_buffer( void *host, UINT slot, AMD64_CONTEXT *ctx )
{
    HRESULT (WINAPI *fn)( void *, LPCDSBUFFERDESC, IDirectSoundBuffer **,
                          IUnknown * ) = host_slot( host, slot );
    IDirectSoundBuffer **out = (IDirectSoundBuffer **)(ULONG_PTR)winecom_read_arg( ctx, 2 );
    IUnknown *outer = (IUnknown *)(ULONG_PTR)winecom_read_arg( ctx, 3 );
    HRESULT hr;

    if (outer) return (UINT64)(UINT)refuse_aggregation( "CreateSoundBuffer", outer );

    hr = fn( host, (LPCDSBUFFERDESC)(ULONG_PTR)winecom_read_arg( ctx, 1 ), out, NULL );
    if (SUCCEEDED(hr))
        winecom_wrap_static( (void **)out, DSOUND_IFACE_IDirectSoundBuffer );
    return (UINT64)(UINT)hr;
}

/* (this, LPCDSCBUFFERDESC, IDirectSoundCaptureBuffer **, IUnknown *) */
static UINT64 hand_create_capture_buffer( void *host, UINT slot, AMD64_CONTEXT *ctx )
{
    HRESULT (WINAPI *fn)( void *, LPCDSCBUFFERDESC, IDirectSoundCaptureBuffer **,
                          IUnknown * ) = host_slot( host, slot );
    IDirectSoundCaptureBuffer **out =
        (IDirectSoundCaptureBuffer **)(ULONG_PTR)winecom_read_arg( ctx, 2 );
    IUnknown *outer = (IUnknown *)(ULONG_PTR)winecom_read_arg( ctx, 3 );
    HRESULT hr;

    if (outer) return (UINT64)(UINT)refuse_aggregation( "CreateCaptureBuffer", outer );

    hr = fn( host, (LPCDSCBUFFERDESC)(ULONG_PTR)winecom_read_arg( ctx, 1 ), out, NULL );
    if (SUCCEEDED(hr))
        winecom_wrap_static( (void **)out, DSOUND_IFACE_IDirectSoundCaptureBuffer );
    return (UINT64)(UINT)hr;
}

/* The order here IS hand_funcs[] order in dsound_marshal.h; the C_ASSERT below
 * only catches a count mismatch, so the comment there is the authority. */
static const winecom_hand_fn dsound_hand_funcs[] =
{
    hand_create_sound_buffer,
    hand_create_capture_buffer,
    hand_ffffff_i,
    hand_fff_i,
    hand_f_i,
};

C_ASSERT( ARRAYSIZE(dsound_hand_funcs) == DSOUND_HAND_COUNT );

/* ------------------------------------------------- the runtime instance */

static const WCHAR *const dsound_guest_modules[] = { L"dsound.dll" };

static const struct winecom_surface dsound_surface =
{
    .name = "dsound",
    .guest_modules = dsound_guest_modules,
    .module_count = ARRAYSIZE(dsound_guest_modules),
    .ifaces = dsound_com_ifaces,
    .iface_count = DSOUND_IFACE_COUNT,
    .invoke = dsound_invoke,
    .hand_funcs = dsound_hand_funcs,
    .hand_count = DSOUND_HAND_COUNT,
};

static BOOL dsound_com_ready( void )
{
    return winecom_attach( &dsound_surface );
}

NTSTATUS WINAPI __wine_com_dispatch( UINT iface, UINT slot, AMD64_CONTEXT *ctx )
{
    if (!dsound_com_ready()) return STATUS_DLL_INIT_FAILED;
    return winecom_dispatch( iface, slot, ctx );
}

/* The crossing-frequency sink's name lookup; see winecom_slot_names.  Never on
 * a dispatch path -- ntdll asks once per slot, when it interns the row. */
BOOL WINAPI __wine_com_slot_name( UINT iface, UINT slot, const char **iface_name,
                                  const char **slot_name )
{
    return winecom_slot_names( iface, slot, iface_name, slot_name );
}

/* The shared loud-refusal stub every GUEST-REFUSE export resolves to: a flat
 * export that vends or consumes interfaces but has no wrapper.  Returns
 * E_NOTIMPL rather than a pass-through that would hand the guest a native
 * vtable.  The trapping export's own name is in the dispatcher TRACE. */
HRESULT WINAPI __wine_com_refuse(void)
{
    ERR( "dsound: refusing an interface-bearing flat export with no wrapper "
         "(see the guest thunk trace for which)\n" );
    return E_NOTIMPL;
}

/* ---------------------------------------------------------- flat wrappers */

#define GUEST_CREATE( name, iface_type, iface_index )                        \
    HRESULT WINAPI __wine_guest_##name( LPCGUID guid, iface_type **out,      \
                                        IUnknown *outer )                    \
    {                                                                        \
        HRESULT hr;                                                          \
        if (!dsound_com_ready()) return E_FAIL;                              \
        if (outer) return refuse_aggregation( #name, outer );                \
        if ((hr = name( guid, out, NULL )) == DS_OK)                         \
            winecom_wrap_static( (void **)out, iface_index );                \
        return hr;                                                           \
    }

GUEST_CREATE( DirectSoundCreate,         IDirectSound,        DSOUND_IFACE_IDirectSound )
GUEST_CREATE( DirectSoundCreate8,        IDirectSound8,       DSOUND_IFACE_IDirectSound8 )
GUEST_CREATE( DirectSoundCaptureCreate,  IDirectSoundCapture, DSOUND_IFACE_IDirectSoundCapture )
GUEST_CREATE( DirectSoundCaptureCreate8, IDirectSoundCapture8, DSOUND_IFACE_IDirectSoundCapture )

/* Three interface pointers out at once, and every one of them has to be
 * wrapped before the guest sees any of it.  On a partial failure DirectSound
 * writes nothing, so wrapping is all-or-nothing here too. */
HRESULT WINAPI __wine_guest_DirectSoundFullDuplexCreate(
        LPCGUID capture_guid, LPCGUID render_guid,
        LPCDSCBUFFERDESC cdesc, LPCDSBUFFERDESC rdesc, HWND hwnd, DWORD level,
        IDirectSoundFullDuplex **duplex, IDirectSoundCaptureBuffer8 **cbuf,
        IDirectSoundBuffer8 **rbuf, IUnknown *outer )
{
    HRESULT hr;

    if (!dsound_com_ready()) return E_FAIL;
    if (outer) return refuse_aggregation( "DirectSoundFullDuplexCreate", outer );

    hr = DirectSoundFullDuplexCreate( capture_guid, render_guid, cdesc, rdesc,
                                      hwnd, level, duplex, cbuf, rbuf, NULL );
    if (hr != DS_OK) return hr;
    winecom_wrap_static( (void **)duplex, DSOUND_IFACE_IDirectSoundFullDuplex );
    winecom_wrap_static( (void **)cbuf, DSOUND_IFACE_IDirectSoundCaptureBuffer8 );
    winecom_wrap_static( (void **)rbuf, DSOUND_IFACE_IDirectSoundBuffer8 );
    return hr;
}
