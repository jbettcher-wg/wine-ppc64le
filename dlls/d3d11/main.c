/*
 * Native d3d11.dll -- DXVK's PE front, and the single winecom client for the
 * whole D3D11/D3D10/DXGI surface.
 *
 * This module REPLACES Wine's wined3d-backed d3d11.  It is the native half of
 * the native-lane D3D11 stack, the same shape as dlls/d3d12 and vkd3d-proton:
 *
 *   guest x86-64 PE  -->  C:\windows\sysx8664\{d3d11,dxgi,d3d10core}.dll
 *      |                  (spec2thunk COM mode: pure trap surface, no
 *      |                   marshalling knowledge)
 *      |  trap; ntdll's dispatcher maps RIP -> (iface, slot) and calls the
 *      |  NATIVE namesake's __wine_com_dispatch
 *      v
 *   __wine_com_dispatch( iface, slot, AMD64_CONTEXT * )       <-- THIS FILE
 *      |  = libs/winecom's dispatch loop over the generated marshal table
 *      |  (d3d11_marshal.h, from ppc64le/dxvk/gen_winecom.py)
 *      v
 *   d3d11.so (unixlib, unix.c)  -->  libdxvk_d3d11.so + libdxvk_dxgi.so
 *
 * ONE RUNTIME FOR THREE DLLS.  libs/winecom's state is per-linkee.  If native
 * dxgi.dll linked its own copy, the IDXGIAdapter proxy it minted would not be
 * one of THIS module's proxies, and `D3D11CreateDevice(adapter, ...)` would be
 * refused as a guest-implemented object at the first call a real game makes.
 * So this module owns the only instance; native dxgi.dll and d3d10core.dll
 * forward both their flat exports and __wine_com_dispatch here through their
 * .spec files, and all three GUEST modules publish the same roster from
 * ppc64le/dxvk/interfaces_dxvk.json -- which is what makes a proxy's guest
 * vtable interchangeable between them, and what winecom_attach's IID
 * cross-check verifies for every one of them that is loaded.
 *
 * WHY THE GUEST-FACING FLAT ENTRIES HAVE THEIR OWN NAMES.  A proxy's vtable is
 * the guest module's array of x86-64 trap stubs.  Handing one to a NATIVE
 * ppc64 caller would have it execute those bytes as ppc64.  So the exports a
 * guest reaches are __wine_guest_*, named by GUEST-IMPL lines in the .thunks
 * files, and the plain-named exports -- which only a native ppc64 PE can
 * reach -- refuse loudly rather than hand out a proxy.  dlls/d3d12 predates
 * this and exports one name for both; this is the corrected shape.
 *
 * Copyright 2026 the ppc64le port authors
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 */

/* The body of this file is the 64-bit native lane: its marshal tables and
 * layout asserts describe the x86-64 guest ABI, which has no meaning for an
 * i386 build of this module.  The #else arm at the bottom is what i386 gets:
 * every .spec export still exists and still links, and each one refuses with
 * the unimplemented-function exception, naming itself -- because 32-bit D3D
 * belongs to a future 32-bit DXVK lane, not to a silently-wrong build of
 * this one.  See ppc64le/wow64/DESIGN.md. */
#ifdef __powerpc64__

#include <stdarg.h>

#include "ntstatus.h"
#define WIN32_NO_STATUS

#include "windef.h"
#include "winbase.h"
#include "winerror.h"   /* DXGI_ERROR_MORE_DATA, and nothing else */
#include "winternl.h"
/* The two questions DXVK's win32u WSI backend asks about a window, answered
 * here on the application's own thread and pushed down to the unixlib: see
 * the presentation banner below. */
#include "ntuser.h"
#include "wine/debug.h"
#include "wine/winecom.h"

#include "unixlib.h"
/* The i386 struct repacks: untyped offset-copy functions (gen_repack32.py),
 * referenced by d3d11_marshal.h's reps tables, so the order matters.  No
 * D3D header rides in with them -- see that file's banner. */
#include "d3d11_repack32.h"
#include "d3d11_marshal.h"

WINE_DEFAULT_DEBUG_CHANNEL(d3d11);

/* This module marshals; it does not implement D3D11, so it needs none of
 * D3D11's types.  Everything below is an address or a machine word, and the
 * arity of each flat entry is asserted by its .spec line and by the clang
 * oracle that built the guest thunk from the same .spec.  Pulling in
 * Wine's d3d11.h to spell `ID3D11Device **` would only invite the question of
 * whose d3d11.h -- Wine's describes wined3d's implementation, and the layouts
 * that actually matter are checked by the marshal generator against DXVK's. */


/* ------------------------------------------------------------- unix calls */

static UINT64 unix_vtbl_call( void *host, UINT slot, UINT argc, UINT64 *args )
{
    struct d3d11_call_params p;
    NTSTATUS status;

    memcpy( p.args, args, sizeof(p.args) );
    p.args[0] = (UINT64)(ULONG_PTR)host;
    p.slot = slot;
    p.argc = argc;
    p.ret = 0;
    if ((status = D3D11_UNIX_CALL( call, &p )))
    {
        ERR( "unix call failed, status %08x\n", (UINT)status );
        return (UINT64)(UINT)E_FAIL;
    }
    return p.ret;
}

/* The generic FLOATING-POINT invoker (PPC64EC step C): float raw bits ride
 * the integer view across the unixlib -- the FP_SHAPE precedent, minus the
 * per-shape enum -- and the unix side splits them into ELFv2's register
 * files through the one shared implementation (wine/winecom_fpcall.h).  The
 * marshal table's fpmask/fpwide/fpret drive it; the VideoProcessor
 * SetStreamAlpha/SetStreamLumaKey rows are what it un-refuses today. */
static UINT64 unix_vtbl_call_fp( void *host, UINT slot, UINT argc, UINT64 *args,
                                 UINT fpword, UINT64 *fpret_bits )
{
    struct d3d11_fpcall_params p;
    NTSTATUS status;

    memcpy( p.args, args, sizeof(p.args) );
    p.args[0] = (UINT64)(ULONG_PTR)host;
    p.slot = slot;
    p.argc = argc;
    p.fpword = fpword;
    p.ret = 0;
    p.fpret_bits = 0;
    if ((status = D3D11_UNIX_CALL( fpcall, &p )))
    {
        ERR( "unix fp call failed, status %08x\n", (UINT)status );
        return (UINT64)(UINT)E_FAIL;
    }
    if (fpret_bits) *fpret_bits = p.fpret_bits;
    return p.ret;
}

/* ------------------------------------------------- the runtime instance */

/* Every guest module that publishes this roster.  winecom_attach validates
 * ALL of them that are loaded and materialises the proxy vtables from the
 * first -- so a d3d11.dll and a dxgi.dll built from different JSONs is a load
 * failure here, never a call dispatched into the neighbouring slot.
 *
 * d3d10.dll is the fourth, and it has to be listed even though only two of its
 * exports are served: a D3D10 application imports d3d10.dll and NOTHING ELSE
 * on this surface, so it is the only module in the process publishing the
 * roster.  [MEASURED] without it, D3D10CreateDevice from such a guest returned
 * E_FAIL with `winecom: no guest thunk module in this process; COM dispatch
 * cannot work` -- the proxy vtables had nowhere to come from. */
static const WCHAR *const d3d11_guest_modules[] =
    { L"d3d11.dll", L"dxgi.dll", L"d3d10core.dll", L"d3d10.dll" };

static UINT64 hand_get_private_data( void *host, UINT slot, AMD64_CONTEXT *ctx );
static UINT64 hand_set_private_data( void *host, UINT slot, AMD64_CONTEXT *ctx );
static UINT64 hand_set_private_data_iface( void *host, UINT slot, AMD64_CONTEXT *ctx );
static UINT64 hand_clear_depth_stencil_view( void *host, UINT slot, AMD64_CONTEXT *ctx );
static UINT64 hand_set_resource_min_lod( void *host, UINT slot, AMD64_CONTEXT *ctx );
static UINT64 hand_get_resource_min_lod( void *host, UINT slot, AMD64_CONTEXT *ctx );
static UINT64 hand_d3d10_clear_depth_stencil_view( void *host, UINT slot, AMD64_CONTEXT *ctx );
static UINT64 hand_create_swapchain( void *host, UINT slot, AMD64_CONTEXT *ctx );
static UINT64 hand_create_swapchain_for_hwnd( void *host, UINT slot, AMD64_CONTEXT *ctx );
static UINT64 hand_swapchain_present( void *host, UINT slot, AMD64_CONTEXT *ctx );
static UINT64 hand_swapchain_present1( void *host, UINT slot, AMD64_CONTEXT *ctx );
static UINT64 hand_video_processor_blt( void *host, UINT slot, AMD64_CONTEXT *ctx );

static UINT64 hand32_get_private_data( void *host, UINT slot, I386_CONTEXT *ctx );
static UINT64 hand32_set_private_data( void *host, UINT slot, I386_CONTEXT *ctx );
static UINT64 hand32_set_private_data_iface( void *host, UINT slot, I386_CONTEXT *ctx );
static UINT64 hand32_clear_depth_stencil_view( void *host, UINT slot, I386_CONTEXT *ctx );
static UINT64 hand32_set_resource_min_lod( void *host, UINT slot, I386_CONTEXT *ctx );
static UINT64 hand32_create_swapchain( void *host, UINT slot, I386_CONTEXT *ctx );
static UINT64 hand32_create_swapchain_for_hwnd( void *host, UINT slot, I386_CONTEXT *ctx );
static UINT64 hand32_swapchain_present( void *host, UINT slot, I386_CONTEXT *ctx );
static UINT64 hand32_swapchain_present1( void *host, UINT slot, I386_CONTEXT *ctx );
static UINT64 hand32_map( void *host, UINT slot, I386_CONTEXT *ctx );
static UINT64 hand32_unmap( void *host, UINT slot, I386_CONTEXT *ctx );
static UINT64 hand32_create_texture1d( void *host, UINT slot, I386_CONTEXT *ctx );
static UINT64 hand32_create_texture2d( void *host, UINT slot, I386_CONTEXT *ctx );
static UINT64 hand32_create_texture3d( void *host, UINT slot, I386_CONTEXT *ctx );

/* The 32-bit lane's hand walkers, matched to rows BY SLOT NAME at attach --
 * independent of the 64-bit hand table, because the two lanes' reasons to
 * hand-write differ: the float and presentation slots mirror their 64-bit
 * twins over stdcall slots, and the texture creates exist ONLY here, where
 * the initial-data array's element count (MipLevels x ArraySize, out of the
 * desc) is beyond any mechanical rep.  GetResourceMinLOD has no 32-bit
 * walker on purpose: it returns a float in x87 ST(0), which the dispatch
 * geometry cannot express -- it stays refused until an x87-return path
 * exists. */
static const struct winecom_hand32 d3d11_hand32[] =
{
    { "ID3D11DeviceChild::GetPrivateData",          hand32_get_private_data },
    { "ID3D11Device::GetPrivateData",               hand32_get_private_data },
    { "ID3D10DeviceChild::GetPrivateData",          hand32_get_private_data },
    { "ID3D10Device::GetPrivateData",               hand32_get_private_data },
    { "IDXGIObject::GetPrivateData",                hand32_get_private_data },
    { "ID3D11DeviceChild::SetPrivateData",          hand32_set_private_data },
    { "ID3D11Device::SetPrivateData",               hand32_set_private_data },
    { "ID3D10DeviceChild::SetPrivateData",          hand32_set_private_data },
    { "ID3D10Device::SetPrivateData",               hand32_set_private_data },
    { "IDXGIObject::SetPrivateData",                hand32_set_private_data },
    { "ID3D11DeviceChild::SetPrivateDataInterface", hand32_set_private_data_iface },
    { "ID3D11Device::SetPrivateDataInterface",      hand32_set_private_data_iface },
    { "ID3D10DeviceChild::SetPrivateDataInterface", hand32_set_private_data_iface },
    { "ID3D10Device::SetPrivateDataInterface",      hand32_set_private_data_iface },
    { "IDXGIObject::SetPrivateDataInterface",       hand32_set_private_data_iface },
    { "ID3D11DeviceContext::ClearDepthStencilView", hand32_clear_depth_stencil_view },
    { "ID3D11DeviceContext::SetResourceMinLOD",     hand32_set_resource_min_lod },
    { "ID3D10Device::ClearDepthStencilView",        hand32_clear_depth_stencil_view },
    { "IDXGIFactory::CreateSwapChain",              hand32_create_swapchain },
    { "IDXGIFactory2::CreateSwapChainForHwnd",      hand32_create_swapchain_for_hwnd },
    { "IDXGISwapChain::Present",                    hand32_swapchain_present },
    { "IDXGISwapChain1::Present1",                  hand32_swapchain_present1 },
    { "ID3D11DeviceContext::Map",                   hand32_map },
    { "ID3D11DeviceContext::Unmap",                 hand32_unmap },
    { "ID3D11Device::CreateTexture1D",              hand32_create_texture1d },
    { "ID3D11Device::CreateTexture2D",              hand32_create_texture2d },
    { "ID3D11Device::CreateTexture3D",              hand32_create_texture3d },
};

static const winecom_hand_fn d3d11_hand_funcs[] =
{
    hand_get_private_data,
    hand_set_private_data,
    hand_set_private_data_iface,
    hand_clear_depth_stencil_view,
    hand_set_resource_min_lod,
    hand_get_resource_min_lod,
    hand_d3d10_clear_depth_stencil_view,
    hand_create_swapchain,
    hand_create_swapchain_for_hwnd,
    hand_swapchain_present,
    hand_swapchain_present1,
    hand_video_processor_blt,
};

C_ASSERT( ARRAYSIZE(d3d11_hand_funcs) == D3D11_HAND_COUNT );

/* The concrete-type upgrade (winecom_surface::wrap_concrete): a caller may
 * static_cast a returned ID3D11Resource* to the texture or buffer type it
 * knows it created -- Unity casts ID3D11View::GetResource's answer to
 * ID3D11Texture2D and calls GetDesc, a slot the Resource-sized vtable does
 * not have.  The host knows the concrete type (ID3D11Resource::GetType,
 * slot 7 by fixed vtable geometry; same for ID3D10), so resources intern
 * under it and every derived-type call lands on a real stub. */
#define D3D11_RESOURCE_SLOT_GET_TYPE_ 7
static UINT d3d11_wrap_concrete( void *host, UINT iface )
{
    UINT64 args[D3D11_UNIX_MAX_ARGS] = { 0 };
    UINT dim = 0;

    if (iface != D3D11_IFACE_ID3D11Resource && iface != D3D11_IFACE_ID3D10Resource)
        return iface;
    args[1] = (UINT64)(ULONG_PTR)&dim;
    unix_vtbl_call( host, D3D11_RESOURCE_SLOT_GET_TYPE_, 2, args );
    if (iface == D3D11_IFACE_ID3D11Resource)
        switch (dim)
        {
        case 1: return D3D11_IFACE_ID3D11Buffer;
        case 2: return D3D11_IFACE_ID3D11Texture1D;
        case 3: return D3D11_IFACE_ID3D11Texture2D;
        case 4: return D3D11_IFACE_ID3D11Texture3D;
        }
    else
        switch (dim)
        {
        case 1: return D3D11_IFACE_ID3D10Buffer;
        case 2: return D3D11_IFACE_ID3D10Texture1D;
        case 3: return D3D11_IFACE_ID3D10Texture2D;
        case 4: return D3D11_IFACE_ID3D10Texture3D;
        }
    return iface;
}

/* ID3D11VideoContext::VideoProcessorBlt( ID3D11VideoProcessor *proc,
 *     ID3D11VideoProcessorOutputView *view, UINT OutputFrame,
 *     UINT StreamCount, const D3D11_VIDEO_PROCESSOR_STREAM *pStreams ).
 *
 * The stream struct carries interface-pointer ARRAYS inside itself -- past/
 * future frame view arrays plus the input view, and the same trio again for
 * the stereo right channel -- which no marshal class can walk into; the
 * hand_resource_barrier shape.  Both sides are 64-bit here (the generator
 * marks the row refuse32), so the layout below is the guest's own; the
 * C_ASSERTs pin it against header drift. */
struct vp_stream
{
    BOOL   Enable;
    UINT   OutputIndex;
    UINT   InputFrameOrField;
    UINT   PastFrames;
    UINT   FutureFrames;
    void **ppPastSurfaces;
    void  *pInputSurface;
    void **ppFutureSurfaces;
    void **ppPastSurfacesRight;
    void  *pInputSurfaceRight;
    void **ppFutureSurfacesRight;
};
C_ASSERT( sizeof(struct vp_stream) == 72 );
C_ASSERT( offsetof(struct vp_stream, ppPastSurfaces) == 24 );
C_ASSERT( offsetof(struct vp_stream, ppFutureSurfacesRight) == 64 );

#define VP_BLT_MAX_STREAMS  8   /* D3D11 video processors rarely exceed one */
#define VP_BLT_MAX_FRAMES  32   /* per past/future array, per stream */

static BOOL vp_unwrap_views( void *const *guest, UINT count, void **native )
{
    UINT i;
    for (i = 0; i < count; i++)
    {
        native[i] = NULL;
        if (guest[i] && !winecom_translate_in( guest[i], &native[i] )) return FALSE;
    }
    return TRUE;
}

static UINT64 hand_video_processor_blt( void *host, UINT slot, AMD64_CONTEXT *ctx )
{
    UINT64 args[D3D11_UNIX_MAX_ARGS] = { 0 };
    UINT count = (UINT)winecom_read_arg( ctx, 4 ), s;
    const struct vp_stream *src = (const struct vp_stream *)(ULONG_PTR)winecom_read_arg( ctx, 5 );
    struct vp_stream streams[VP_BLT_MAX_STREAMS];
    void *views[VP_BLT_MAX_STREAMS][6 * VP_BLT_MAX_FRAMES]; /* worst-case pool */
    void *proc = NULL, *view = NULL;

    if (!count || !src || count > VP_BLT_MAX_STREAMS)
    {
        if (count > VP_BLT_MAX_STREAMS)
            FIXME( "VideoProcessorBlt: %u streams exceeds the walker's %u; "
                   "refusing\n", count, VP_BLT_MAX_STREAMS );
        return (UINT)E_INVALIDARG;
    }
    if (!winecom_translate_in( (void *)(ULONG_PTR)winecom_read_arg( ctx, 1 ), &proc ) ||
        !winecom_translate_in( (void *)(ULONG_PTR)winecom_read_arg( ctx, 2 ), &view ))
    {
        FIXME( "VideoProcessorBlt on a guest-implemented processor/view\n" );
        return (UINT)E_NOTIMPL;
    }

    for (s = 0; s < count; s++)
    {
        void **pool = views[s];
        UINT past = src[s].PastFrames, fut = src[s].FutureFrames;

        streams[s] = src[s];
        if (past > VP_BLT_MAX_FRAMES || fut > VP_BLT_MAX_FRAMES)
        {
            FIXME( "VideoProcessorBlt: stream %u wants %u/%u frames, walker "
                   "cap is %u; refusing\n", s, past, fut, VP_BLT_MAX_FRAMES );
            return (UINT)E_INVALIDARG;
        }
        if (streams[s].pInputSurface &&
            !winecom_translate_in( streams[s].pInputSurface, &streams[s].pInputSurface ))
            goto guest_impl;
        if (streams[s].pInputSurfaceRight &&
            !winecom_translate_in( streams[s].pInputSurfaceRight, &streams[s].pInputSurfaceRight ))
            goto guest_impl;
        if (streams[s].ppPastSurfaces)
        {
            if (!vp_unwrap_views( streams[s].ppPastSurfaces, past, pool )) goto guest_impl;
            streams[s].ppPastSurfaces = pool; pool += past;
        }
        if (streams[s].ppFutureSurfaces)
        {
            if (!vp_unwrap_views( streams[s].ppFutureSurfaces, fut, pool )) goto guest_impl;
            streams[s].ppFutureSurfaces = pool; pool += fut;
        }
        if (streams[s].ppPastSurfacesRight)
        {
            if (!vp_unwrap_views( streams[s].ppPastSurfacesRight, past, pool )) goto guest_impl;
            streams[s].ppPastSurfacesRight = pool; pool += past;
        }
        if (streams[s].ppFutureSurfacesRight)
        {
            if (!vp_unwrap_views( streams[s].ppFutureSurfacesRight, fut, pool )) goto guest_impl;
            streams[s].ppFutureSurfacesRight = pool; pool += fut;
        }
    }

    args[0] = (UINT64)(ULONG_PTR)host;
    args[1] = (UINT64)(ULONG_PTR)proc;
    args[2] = (UINT64)(ULONG_PTR)view;
    args[3] = winecom_read_arg( ctx, 3 );
    args[4] = count;
    args[5] = (UINT64)(ULONG_PTR)streams;
    return unix_vtbl_call( host, slot, 6, args );

guest_impl:
    FIXME( "VideoProcessorBlt with a guest-implemented input view\n" );
    return (UINT)E_NOTIMPL;
}

/* ------------------------------------------------------------ event relay
 *
 * The PE half of the surface's event_mint/event_reap hooks (winecom.h): a
 * guest Wine event crossing to DXVK becomes the tagged eventfd the native
 * sync convention understands, and a process-lifetime pump thread turns
 * eventfd payouts back into NtSetEvent on the guest's own event.  The unix
 * half (unix.c) owns the eventfds and the epoll set; THIS side owns a
 * duplicated reference per entry -- taken before the mint so the relay can
 * signal an event the guest has since closed, released when the entry dies
 * (one-shot payout, or a reap after a FAILED call).  DXVK never closes a
 * caller's event (its native CloseHandle is taught to no-op tagged values --
 * dxvk-patches), so the fd's life belongs to the relay alone. */
static LONG event_pump_started;

static DWORD WINAPI event_pump_thread( void *arg )
{
    struct d3d11_event_pump_params p;

    for (;;)
    {
        p.guest_handle = 0;
        if (D3D11_UNIX_CALL( event_pump, &p ) || p.shutdown) break;
        if (!p.guest_handle) continue;
        NtSetEvent( (HANDLE)(ULONG_PTR)p.guest_handle, NULL );
        if (p.close_handle) NtClose( (HANDLE)(ULONG_PTR)p.guest_handle );
    }
    return 0;
}

static UINT64 d3d11_event_mint( UINT64 guest_handle, BOOL oneshot )
{
    struct d3d11_event_mint_params p;
    HANDLE dup = NULL;

    if (NtDuplicateObject( GetCurrentProcess(), (HANDLE)(ULONG_PTR)guest_handle,
                           GetCurrentProcess(), &dup, 0, 0, DUPLICATE_SAME_ACCESS ))
        return 0;
    p.guest_handle = (UINT64)(ULONG_PTR)dup;
    p.oneshot = oneshot;
    p.native_handle = 0;
    if (D3D11_UNIX_CALL( event_mint, &p ) || !p.native_handle)
    {
        NtClose( dup );
        return 0;
    }
    if (InterlockedCompareExchange( &event_pump_started, 1, 0 ) == 0)
    {
        HANDLE thread;
        if (!NtCreateThreadEx( &thread, THREAD_ALL_ACCESS, NULL, GetCurrentProcess(),
                               (PRTL_THREAD_START_ROUTINE)event_pump_thread,
                               NULL, 0, 0, 0, 0, NULL ))
            NtClose( thread );
        else
        {
            /* no pump = payouts never reach the guest; give the slot back
             * and refuse rather than serve a signal that cannot arrive */
            struct d3d11_event_reap_params r = { .native_handle = p.native_handle };
            event_pump_started = 0;
            D3D11_UNIX_CALL( event_reap, &r );
            NtClose( dup );
            return 0;
        }
    }
    return p.native_handle;
}

static void d3d11_event_reap( UINT64 native_handle )
{
    struct d3d11_event_reap_params p = { .native_handle = native_handle };

    if (!D3D11_UNIX_CALL( event_reap, &p ) && p.guest_handle)
        NtClose( (HANDLE)(ULONG_PTR)p.guest_handle );
}

static const struct winecom_surface d3d11_surface =
{
    .name = "d3d11",
    .guest_modules = d3d11_guest_modules,
    .module_count = ARRAYSIZE(d3d11_guest_modules),
    .ifaces = d3d11_com_ifaces,
    .iface_count = D3D11_IFACE_COUNT,
    .invoke = unix_vtbl_call,
    .hand_funcs = d3d11_hand_funcs,
    .hand_count = D3D11_HAND_COUNT,
    .hand32 = d3d11_hand32,
    .hand32_count = ARRAYSIZE(d3d11_hand32),
    .wrap_concrete = d3d11_wrap_concrete,
    .invoke_fp = unix_vtbl_call_fp,
    .event_mint = d3d11_event_mint,
    .event_reap = d3d11_event_reap,
};

static LONG com_init_state;            /* 0 = no, 1 = in progress, 2 = ok,
                                          3 = failed */

static BOOL com_runtime_init( void )
{
    LONG state;

    /* The steady state first, on a plain acquire load: every dispatch comes
     * through here, and on POWER the CAS below is a full sync plus a
     * lwarx/stwcx. pair plus an isync -- 6% of a game's render thread. */
    if (ReadAcquire( &com_init_state ) == 2) return TRUE;
    while ((state = InterlockedCompareExchange( &com_init_state, 1, 0 )))
    {
        if (state == 2) return TRUE;
        if (state == 3) return FALSE;
        NtYieldExecution();
    }
    if (D3D11_UNIX_CALL( init, NULL ) || !winecom_attach( &d3d11_surface ))
    {
        InterlockedExchange( &com_init_state, 3 );
        return FALSE;
    }
    InterlockedExchange( &com_init_state, 2 );
    return TRUE;
}

NTSTATUS WINAPI __wine_com_dispatch( UINT iface, UINT slot, AMD64_CONTEXT *ctx )
{
    if (!com_runtime_init()) return STATUS_DLL_INIT_FAILED;
    return winecom_dispatch( iface, slot, ctx );
}

/* The i386 twin: same lazy initialisation, the 32-bit dispatch loop.  The
 * contract differs from the 64-bit one -- winecom_dispatch32 owns the whole
 * epilogue including the stdcall pop; see its banner in libs/winecom. */
NTSTATUS WINAPI __wine_com_dispatch32( UINT iface, UINT slot, I386_CONTEXT *ctx )
{
    if (!com_runtime_init()) return STATUS_DLL_INIT_FAILED;
    return winecom_dispatch32( iface, slot, ctx );
}

/* The crossing-frequency sink's name lookup; see winecom_slot_names.  Never
 * on a dispatch path -- ntdll asks once per slot, when it interns the row. */
BOOL WINAPI __wine_com_slot_name( UINT iface, UINT slot, const char **iface_name,
                                  const char **slot_name )
{
    return winecom_slot_names( iface, slot, iface_name, slot_name );
}

/* short spellings for the hand-written slots below */
static UINT64 read_arg( const AMD64_CONTEXT *ctx, UINT n )
{
    return winecom_read_arg( ctx, n );
}

/* IUnknown::AddRef on a HOST interface, then intern.  winecom_wrap CONSUMES a
 * host reference, so this pair leaves the host refcount where it was and adds
 * one guest reference to the (possibly already interned) proxy -- which is
 * exactly what a COM method that returns an interface owes its caller. */
static void *host_addref_wrap( void *host, UINT iface )
{
    UINT64 args[D3D11_UNIX_MAX_ARGS] = { 0 };

    if (!host) return NULL;
    unix_vtbl_call( host, 1 /* IUnknown::AddRef */, 1, args );
    return winecom_wrap( host, iface );
}

/* ------------------------------------------ the private-data side table
 *
 * ID3D11DeviceChild::GetPrivateData's out-parameter is `void *pData`, not
 * `void **` -- there is no slot flag that could mark it, and DXVK_THUNK_STRICT
 * on the old FEX stack could not warn about it either
 * (dxvk-ppc64le/docs/hazard-hunt.md §3.2).  On a GUID the application
 * registered through SetPrivateDataInterface it hands back a RAW NATIVE
 * pointer, and the application's next call through it executes ppc64 bytes as
 * x86-64.  It is the one hazard on this surface that is invisible to static
 * classification, so it is answered dynamically: remember what the guest
 * stored, and give the guest back what it stored.
 *
 * THE ENTRY HOLDS A REFERENCE ON THE CONTAINER, and that is not an oversight.
 * The table is keyed by host object address.  A host object destroyed while an
 * entry named it would let the allocator hand that address to a NEW object,
 * whose first GetPrivateData for the same GUID would return a dangling proxy
 * -- reachable, and silent.  Holding a reference makes the address unreusable
 * for as long as the entry exists.  The cost is that an object with a private
 * interface set on it lives until the entry is removed
 * (SetPrivateDataInterface(guid, NULL), or SetPrivateData on the same GUID),
 * which is bounded by the number of (object, GUID) pairs the application
 * actually creates and is what applications do with them anyway.
 */

struct private_iface
{
    struct private_iface *next;
    void *host;          /* container, one reference held */
    void *guest;         /* what the guest gave us, handed straight back */
    GUID guid;
    UINT iface;          /* roster index of `guest` */
};

static CRITICAL_SECTION priv_cs;
static CRITICAL_SECTION_DEBUG priv_cs_debug =
{
    0, 0, &priv_cs,
    { &priv_cs_debug.ProcessLocksList, &priv_cs_debug.ProcessLocksList },
      0, 0, { (DWORD_PTR)(__FILE__ ": d3d11 priv_cs") }
};
static CRITICAL_SECTION priv_cs = { &priv_cs_debug, -1, 0, 0, 0, 0 };

static struct private_iface *private_ifaces;

/* Detach and return the entry for (host, guid), or NULL.  Caller owns what
 * comes back, including the container reference. */
static struct private_iface *private_take( void *host, const GUID *guid )
{
    struct private_iface **link, *p = NULL;

    RtlEnterCriticalSection( &priv_cs );
    for (link = &private_ifaces; *link; link = &(*link)->next)
    {
        if ((*link)->host == host && IsEqualGUID( &(*link)->guid, guid ))
        {
            p = *link;
            *link = p->next;
            break;
        }
    }
    RtlLeaveCriticalSection( &priv_cs );
    return p;
}

static void private_drop( struct private_iface *p )
{
    if (!p) return;
    winecom_host_release( p->host );
    RtlFreeHeap( NtCurrentTeb()->Peb->ProcessHeap, 0, p );
}

/* ----------------------------------------------------- hand-written slots */

/* {ID3D11DeviceChild,ID3D11Device,ID3D10DeviceChild,ID3D10Device,IDXGIObject}
 * ::GetPrivateData( REFGUID guid, UINT *size, void *data ).  If the guest
 * registered an interface under this GUID, answer from the side table with
 * the GUEST proxy it stored; otherwise this is an opaque data blob and the
 * host answers. */
static UINT64 hand_get_private_data( void *host, UINT slot, AMD64_CONTEXT *ctx )
{
    const GUID *guid = (const GUID *)(ULONG_PTR)read_arg( ctx, 1 );
    UINT *size = (UINT *)(ULONG_PTR)read_arg( ctx, 2 );
    void *data = (void *)(ULONG_PTR)read_arg( ctx, 3 );
    UINT64 args[D3D11_UNIX_MAX_ARGS] = { 0 };
    struct private_iface *p;

    /* REFUSAL HYGIENE, BY HAND, because no generated scrub mask reaches a
     * WINECOM_F_HAND row or a flat GUEST-IMPL wrapper: each owns its
     * out-params the way scrub_refused_outs() owns a table refusal's, and a
     * refusal that leaves one unwritten hands the guest its own stack residue.
     * [MEASURED] dlls/combase/syscom.c's IMMDevice::Activate is the site that
     * cost days -- the Witcher 3 read a never-written *ppv back off its stack
     * and the emulator decoded a host module's ppc64le bytes as x86.  The
     * writes go through winecom_refused_scrub_*, which honour
     * WINEEMUNOREFUSESCRUB so the hygiene gate's sabotage arm can prove them
     * load-bearing.  Native failures stay untouched: real D3D11 leaves *out
     * alone on failure and matching Windows means scrubbing only the refusals
     * this side invented.  Pointer cells go through the helper's guest-width
     * store, which is what an i386 guest's four-byte cell needs. */
    if (!guid || !size)
    {
        winecom_refused_scrub_dw( size );
        return (UINT64)(UINT)E_INVALIDARG;
    }

    RtlEnterCriticalSection( &priv_cs );
    for (p = private_ifaces; p; p = p->next)
        if (p->host == host && IsEqualGUID( &p->guid, guid )) break;
    RtlLeaveCriticalSection( &priv_cs );

    if (p)
    {
        void *proxy_host;

        if (!data)
        {
            *size = sizeof(void *);
            return (UINT64)(UINT)S_OK;
        }
        if (*size < sizeof(void *))
        {
            *size = sizeof(void *);
            return (UINT64)(UINT)DXGI_ERROR_MORE_DATA;
        }
        /* The contract AddRefs what it returns.  Do it on the host object the
         * proxy stands for, then re-intern: that is one more guest reference
         * on the same proxy and no net change to the host's count. */
        if (!(proxy_host = winecom_unwrap( p->guest )))
        {
            /* refusal hygiene by hand: *data is exactly the cell the caller
             * would have read an interface pointer out of. */
            winecom_refused_scrub_ptr( data );
            winecom_refused_scrub_dw( size );
            return (UINT64)(UINT)E_FAIL;
        }
        *(void **)data = host_addref_wrap( proxy_host, p->iface );
        *size = sizeof(void *);
        return (UINT64)(UINT)S_OK;
    }

    args[1] = (UINT64)(ULONG_PTR)guid;
    args[2] = (UINT64)(ULONG_PTR)size;
    args[3] = (UINT64)(ULONG_PTR)data;
    return unix_vtbl_call( host, slot, 4, args );
}

/* ...::SetPrivateData( REFGUID guid, UINT size, const void *data ).  Hooked
 * only so that storing a data blob under a GUID that previously held an
 * interface does not leave a stale side-table entry behind -- which would
 * make the NEXT GetPrivateData return an interface the application had
 * already replaced. */
static UINT64 hand_set_private_data( void *host, UINT slot, AMD64_CONTEXT *ctx )
{
    const GUID *guid = (const GUID *)(ULONG_PTR)read_arg( ctx, 1 );
    UINT64 args[D3D11_UNIX_MAX_ARGS] = { 0 };

    if (guid) private_drop( private_take( host, guid ) );
    args[1] = (UINT64)(ULONG_PTR)guid;
    args[2] = read_arg( ctx, 2 );
    args[3] = read_arg( ctx, 3 );
    return unix_vtbl_call( host, slot, 4, args );
}

/* ...::SetPrivateDataInterface( REFGUID guid, const IUnknown *data ). */
static UINT64 hand_set_private_data_iface( void *host, UINT slot, AMD64_CONTEXT *ctx )
{
    const GUID *guid = (const GUID *)(ULONG_PTR)read_arg( ctx, 1 );
    void *guest = (void *)(ULONG_PTR)read_arg( ctx, 2 );
    UINT64 args[D3D11_UNIX_MAX_ARGS] = { 0 };
    struct private_iface *p;
    void *host_iface = NULL;
    UINT64 ret;

    if (!guid) return (UINT64)(UINT)E_INVALIDARG;
    private_drop( private_take( host, guid ) );

    if (guest && !winecom_translate_in( guest, &host_iface ))
    {
        FIXME( "SetPrivateDataInterface with a guest-implemented object %p; "
               "reverse proxies do not exist yet\n", guest );
        return (UINT64)(UINT)E_NOTIMPL;
    }

    args[1] = (UINT64)(ULONG_PTR)guid;
    args[2] = (UINT64)(ULONG_PTR)host_iface;
    ret = unix_vtbl_call( host, slot, 3, args );
    if (FAILED((HRESULT)ret) || !guest) return ret;

    if (!(p = RtlAllocateHeap( NtCurrentTeb()->Peb->ProcessHeap, 0, sizeof(*p) )))
        return (UINT64)(UINT)E_OUTOFMEMORY;
    p->host = host;
    p->guest = guest;
    p->guid = *guid;
    p->iface = D3D11_IFACE_IUnknown;
    /* Pin the container: see the side-table banner. */
    {
        UINT64 a[D3D11_UNIX_MAX_ARGS] = { 0 };
        unix_vtbl_call( host, 1 /* AddRef */, 1, a );
    }
    RtlEnterCriticalSection( &priv_cs );
    p->next = private_ifaces;
    private_ifaces = p;
    RtlLeaveCriticalSection( &priv_cs );
    return ret;
}

/* ID3D11DeviceContext::ClearDepthStencilView( ID3D11DepthStencilView *view,
 * UINT flags, FLOAT depth, UINT8 stencil ).
 *
 * `depth` is the fourth argument counting `this`, so MS-x64 put it in XMM3 --
 * NOT in a GPR, and not on the stack.  The unixlib's widest-integer call form
 * cannot express that at all: on ELFv2 the callee reads f1, which the wide
 * call never wrote.  The value would be whatever was left there, and a wrong
 * clear depth is not a crash, it is a frame that looks nearly right.
 * dlls/ntdll/signal_ppc64.c's flat FP path reads the same
 * ctx->FltSave.XmmRegisters[i] by the same rule. */
static UINT64 hand_clear_depth_stencil_view( void *host, UINT slot, AMD64_CONTEXT *ctx )
{
    struct d3d11_float_params p = { 0 };
    void *view_host = NULL;

    if (!winecom_translate_in( (void *)(ULONG_PTR)read_arg( ctx, 1 ), &view_host ))
    {
        FIXME( "ClearDepthStencilView on a guest-implemented view\n" );
        return 0;
    }
    p.self = (UINT64)(ULONG_PTR)host;
    p.res = (UINT64)(ULONG_PTR)view_host;
    p.a = (UINT)read_arg( ctx, 2 );                 /* ClearFlags */
    __wine_emu_materialize_ctx( ctx );   /* lazy-ctx contract, wine/winecom.h */
    p.f = *(const float *)&ctx->FltSave.XmmRegisters[3];
    p.b = (UINT8)read_arg( ctx, 4 );                /* Stencil */
    p.slot = slot;
    p.shape = FLOAT_SHAPE_RES_UINT_FLOAT_BYTE;
    if (D3D11_UNIX_CALL( float, &p )) ERR( "unix float call failed\n" );
    return 0;   /* void method; RAX is scratch */
}

/* ID3D11DeviceContext::SetResourceMinLOD( ID3D11Resource *r, FLOAT lod ):
 * `lod` is argument 2, so XMM2. */
static UINT64 hand_set_resource_min_lod( void *host, UINT slot, AMD64_CONTEXT *ctx )
{
    struct d3d11_float_params p = { 0 };
    void *res = NULL;

    if (!winecom_translate_in( (void *)(ULONG_PTR)read_arg( ctx, 1 ), &res ))
    {
        FIXME( "SetResourceMinLOD on a guest-implemented resource\n" );
        return 0;
    }
    p.self = (UINT64)(ULONG_PTR)host;
    p.res = (UINT64)(ULONG_PTR)res;
    __wine_emu_materialize_ctx( ctx );   /* lazy-ctx contract, wine/winecom.h */
    p.f = *(const float *)&ctx->FltSave.XmmRegisters[2];
    p.slot = slot;
    p.shape = FLOAT_SHAPE_RES_FLOAT;
    if (D3D11_UNIX_CALL( float, &p )) ERR( "unix float call failed\n" );
    return 0;
}

/* ID3D11DeviceContext::GetResourceMinLOD( ID3D11Resource *r ) -> FLOAT.
 * MS-x64 returns a float in XMM0, not RAX, so this writes the register the
 * guest is about to read -- the whole 16 bytes, because stale high bytes from
 * an earlier call are visible to anything that reads it wider than it wrote.
 * Identical treatment to the flat FP return path in signal_ppc64.c. */
static UINT64 hand_get_resource_min_lod( void *host, UINT slot, AMD64_CONTEXT *ctx )
{
    struct d3d11_float_params p = { 0 };
    void *res = NULL;

    if (!winecom_translate_in( (void *)(ULONG_PTR)read_arg( ctx, 1 ), &res ))
    {
        FIXME( "GetResourceMinLOD on a guest-implemented resource\n" );
        return 0;
    }
    p.self = (UINT64)(ULONG_PTR)host;
    p.res = (UINT64)(ULONG_PTR)res;
    p.slot = slot;
    p.shape = FLOAT_SHAPE_RES_RET_FLOAT;
    if (D3D11_UNIX_CALL( float, &p )) ERR( "unix float call failed\n" );
    /* a write to a still-lazy FP group is IGNORED at resume: materialize
     * first (lazy-ctx contract, wine/winecom.h) */
    __wine_emu_materialize_ctx( ctx );
    memset( &ctx->FltSave.XmmRegisters[0], 0, sizeof(ctx->FltSave.XmmRegisters[0]) );
    *(float *)&ctx->FltSave.XmmRegisters[0] = p.ret;
    return 0;
}

/* ID3D10Device::ClearDepthStencilView( ID3D10DepthStencilView *, UINT, FLOAT,
 * UINT8 ) -- the same shape at a different slot in a different vtable. */
static UINT64 hand_d3d10_clear_depth_stencil_view( void *host, UINT slot, AMD64_CONTEXT *ctx )
{
    return hand_clear_depth_stencil_view( host, slot, ctx );
}

/* ======================================================================
 *                            presentation
 *
 * THE HWND CROSSES UNCONVERTED, AND THAT IS THE WHOLE TRICK.  A Wine window
 * handle is meaningless to DXVK's shipped WSI backends, which is why every
 * route to a swapchain used to be refused here by name.  It is NOT meaningless
 * to Wine, and the guest PE calls Wine's own user32 -- so there is exactly one
 * window-handle namespace in this process and the integer in
 * DXGI_SWAP_CHAIN_DESC::OutputWindow names the same window on both sides of
 * the emulated boundary.  This lane's DXVK patch series adds a WSI backend
 * ("Win32u", ppc64le/dxvk/dxvk-patches/0003-win32u-wsi-backend.patch) that
 * hands the HWND straight back to Wine and asks win32u's client-surface layer
 * for a VkSurfaceKHR on it -- the layer winevulkan uses, so winex11 and
 * winewayland are both served without either being named.  The standalone
 * project's foreign-X11 design, which opened a second X connection and named
 * the guest's XID, is a documented dead end under Wayland.
 *
 * WHY THESE FOUR SLOTS ARE HAND-WRITTEN AND NOT GENERATED.  Their arguments
 * would all marshal correctly now that an HWND may cross; what the generator
 * cannot express is the ORDER OF OPERATIONS around them.  win32u wants its
 * client surface updated before a present and marked presented after, and both
 * calls must happen on a Wine thread -- which the application's call into
 * Present is and DXVK's submission thread, where the real vkQueuePresentKHR
 * happens, is not.  So the hooks are driven from here, on the caller's own
 * thread, exactly as dlls/d3d12/unix_present.c drives them for vkd3d.  The
 * creation slots are hand-written for the same kind of reason: the unixlib has
 * to be told the window's client size before DXVK asks for it, and it has no
 * way to ask user32 anything itself.
 * ====================================================================== */

/* DXGI_SWAP_CHAIN_DESC, spelled here rather than included.
 *
 * This module marshals and implements nothing, so it deliberately pulls in no
 * D3D11 or DXGI header -- see the banner at the top of this file.  One
 * descriptor still has to be READ, because the output window is inside it
 * rather than in the parameter list, and this is the smallest true statement
 * of the part that is read.  The asserts below are the check that it stays
 * true: the guest's x86-64 layout and this ppc64le one agree only because
 * every member is 4 bytes except the 8-byte, 8-aligned HWND, and if a compiler
 * ever disagreed about that the build would stop here instead of handing DXVK
 * a window handle read from the middle of `Windowed`. */
struct dxgi_swap_chain_desc
{
    UINT BufferDesc_Width;              /* DXGI_MODE_DESC BufferDesc */
    UINT BufferDesc_Height;
    UINT BufferDesc_RefreshRate_Numerator;
    UINT BufferDesc_RefreshRate_Denominator;
    UINT BufferDesc_Format;
    UINT BufferDesc_ScanlineOrdering;
    UINT BufferDesc_Scaling;
    UINT SampleDesc_Count;              /* DXGI_SAMPLE_DESC SampleDesc */
    UINT SampleDesc_Quality;
    UINT BufferUsage;
    UINT BufferCount;
    HWND OutputWindow;
    BOOL Windowed;
    UINT SwapEffect;
    UINT Flags;
};

C_ASSERT( FIELD_OFFSET(struct dxgi_swap_chain_desc, OutputWindow) == 48 );
C_ASSERT( sizeof(struct dxgi_swap_chain_desc) == 72 );

/* Which window a live host swapchain presents to.
 *
 * Needed because IDXGISwapChain::Present carries no window and the hooks do.
 * Keyed by the HOST swapchain pointer, which is what a hand-written slot is
 * handed: DXVK's DxgiSwapChain answers QueryInterface for every IDXGISwapChain
 * revision with `ref(this)`, one object and one address, so a guest that asked
 * for IDXGISwapChain1 and one that kept IDXGISwapChain present through the
 * same entry here.  A miss is not fatal and is reported once rather than
 * ignored: it would mean that assumption stopped holding, and the symptom
 * would otherwise be a window that renders but never updates. */
struct swapchain_window
{
    struct swapchain_window *next;
    void *host;
    HWND hwnd;
};

static CRITICAL_SECTION swap_cs;
static CRITICAL_SECTION_DEBUG swap_cs_debug =
{
    0, 0, &swap_cs,
    { &swap_cs_debug.ProcessLocksList, &swap_cs_debug.ProcessLocksList },
      0, 0, { (DWORD_PTR)(__FILE__ ": d3d11 swap_cs") }
};
static CRITICAL_SECTION swap_cs = { &swap_cs_debug, -1, 0, 0, 0, 0 };

static struct swapchain_window *swapchain_windows;

/* Tell the unixlib what it will be asked about this window.  Pushed rather
 * than pulled because the unix side cannot reach user32: it is below the PE
 * boundary, and NtUserCallHwndParam is a syscall a PE thread makes.  Called
 * before every creation and before every present, so the size DXVK reads is
 * never older than the last frame. */
static void push_hwnd_state( HWND hwnd )
{
    struct d3d11_hwnd_params p = { 0 };
    RECT rect = { 0 };

    if (!hwnd) return;
    p.hwnd = (UINT64)(ULONG_PTR)hwnd;
    if (NtUserIsWindow( hwnd ) && NtUserGetClientRect( hwnd, &rect, 0 ))
    {
        p.width = rect.right - rect.left;
        p.height = rect.bottom - rect.top;
        p.valid = 1;
    }
    D3D11_UNIX_CALL( hwnd, &p );
}

static void swapchain_remember( void *host, HWND hwnd )
{
    struct swapchain_window *s;

    if (!host || !hwnd) return;
    if (!(s = RtlAllocateHeap( NtCurrentTeb()->Peb->ProcessHeap, 0, sizeof(*s) )))
        return;
    s->host = host;
    s->hwnd = hwnd;
    RtlEnterCriticalSection( &swap_cs );
    s->next = swapchain_windows;
    swapchain_windows = s;
    RtlLeaveCriticalSection( &swap_cs );
    TRACE( "swapchain %p presents to hwnd %p\n", host, hwnd );
}

static HWND swapchain_hwnd( void *host )
{
    struct swapchain_window *s;
    HWND hwnd = NULL;

    RtlEnterCriticalSection( &swap_cs );
    for (s = swapchain_windows; s; s = s->next)
        if (s->host == host) { hwnd = s->hwnd; break; }
    RtlLeaveCriticalSection( &swap_cs );
    return hwnd;
}

static void present_hook( HWND hwnd, UINT phase, HRESULT hr )
{
    struct d3d11_present_params p = { 0 };

    p.hwnd = (UINT64)(ULONG_PTR)hwnd;
    p.phase = phase;
    p.result = hr;
    D3D11_UNIX_CALL( present, &p );
}

/* IDXGIFactory::CreateSwapChain( IUnknown *device, DXGI_SWAP_CHAIN_DESC *desc,
 * IDXGISwapChain **swapchain ). */
static UINT64 hand_create_swapchain( void *host, UINT slot, AMD64_CONTEXT *ctx )
{
    void *guest_device = (void *)(ULONG_PTR)read_arg( ctx, 1 );
    struct dxgi_swap_chain_desc *desc =
        (struct dxgi_swap_chain_desc *)(ULONG_PTR)read_arg( ctx, 2 );
    void **out = (void **)(ULONG_PTR)read_arg( ctx, 3 );
    UINT64 args[D3D11_UNIX_MAX_ARGS] = { 0 };
    void *host_device = NULL, *host_swapchain;
    HRESULT hr;

    /* refusal hygiene by hand -- see hand_get_private_data */
    if (!desc || !out)
    {
        winecom_refused_scrub_ptr( out );
        return (UINT64)(UINT)E_INVALIDARG;
    }
    if (guest_device && !winecom_translate_in( guest_device, &host_device ))
    {
        FIXME( "CreateSwapChain with a guest-implemented device %p; reverse "
               "proxies do not exist yet\n", guest_device );
        winecom_refused_scrub_ptr( out );
        return (UINT64)(UINT)E_NOTIMPL;
    }

    TRACE( "device %p, %ux%u, hwnd %p, windowed %d, buffers %u\n", guest_device,
           desc->BufferDesc_Width, desc->BufferDesc_Height, desc->OutputWindow,
           desc->Windowed, desc->BufferCount );

    push_hwnd_state( desc->OutputWindow );
    args[1] = (UINT64)(ULONG_PTR)host_device;
    args[2] = (UINT64)(ULONG_PTR)desc;
    args[3] = (UINT64)(ULONG_PTR)out;
    hr = (HRESULT)unix_vtbl_call( host, slot, 4, args );
    if (FAILED(hr)) return (UINT64)(UINT)hr;

    host_swapchain = *out;
    winecom_wrap_static( out, D3D11_IFACE_IDXGISwapChain );
    swapchain_remember( host_swapchain, desc->OutputWindow );
    return (UINT64)(UINT)hr;
}

/* IDXGIFactory2::CreateSwapChainForHwnd( IUnknown *device, HWND window,
 * const DXGI_SWAP_CHAIN_DESC1 *desc, const DXGI_SWAP_CHAIN_FULLSCREEN_DESC
 * *fullscreen_desc, IDXGIOutput *restrict_to_output,
 * IDXGISwapChain1 **swapchain ).
 *
 * DXGI_SWAP_CHAIN_DESC1 carries no window -- that is what the separate HWND
 * argument is for -- so neither descriptor has to be read here. */
static UINT64 hand_create_swapchain_for_hwnd( void *host, UINT slot, AMD64_CONTEXT *ctx )
{
    void *guest_device = (void *)(ULONG_PTR)read_arg( ctx, 1 );
    HWND hwnd = (HWND)(ULONG_PTR)read_arg( ctx, 2 );
    void *guest_output = (void *)(ULONG_PTR)read_arg( ctx, 5 );
    void **out = (void **)(ULONG_PTR)read_arg( ctx, 6 );
    UINT64 args[D3D11_UNIX_MAX_ARGS] = { 0 };
    void *host_device = NULL, *host_output = NULL, *host_swapchain;
    HRESULT hr;

    if (!out) return (UINT64)(UINT)E_INVALIDARG;
    if ((guest_device && !winecom_translate_in( guest_device, &host_device )) ||
        (guest_output && !winecom_translate_in( guest_output, &host_output )))
    {
        /* Not this surface's pointer -- but not necessarily a guest-
         * implemented object either.  A D3D12 title reaches THIS factory
         * (dxgi.dll forwards CreateDXGIFactory here) and passes the vkd3d
         * surface's ID3D12CommandQueue proxy as the device, which this
         * instance cannot translate because winecom interning is per-linkee.
         * Hand the whole call to the d3d12 lane: it unwraps the queue in its
         * own surface, presents through its unix factory + win32u, and
         * gives the guest back a swapchain proxy of ITS surface -- the one
         * whose GetBuffer(IID_ID3D12Resource) rows a D3D12 title needs.
         * [MEASURED] Cyberpunk 2077 run 31: the old blanket refusal here
         * made the game throw on a fiber stack and die undispatchable.
         * E_NOINTERFACE from the lane means "not mine either". */
        HRESULT (WINAPI *p_d3d12)( void *, void *, const void *, const void *,
                                   void *, void ** );
        HMODULE d3d12 = LoadLibraryW( L"d3d12.dll" );

        if (d3d12 && (p_d3d12 = (void *)GetProcAddress( d3d12,
                          "__wine_d3d12_create_swapchain_for_hwnd" )))
        {
            HRESULT hr2 = p_d3d12( guest_device, hwnd,
                                   (const void *)(ULONG_PTR)read_arg( ctx, 3 ),
                                   (const void *)(ULONG_PTR)read_arg( ctx, 4 ),
                                   guest_output, out );
            if (hr2 != E_NOINTERFACE)
            {
                TRACE( "served by the d3d12 lane, hr %#x\n", (UINT)hr2 );
                return (UINT64)(UINT)hr2;
            }
        }
        FIXME( "CreateSwapChainForHwnd with a guest-implemented device or "
               "output; reverse proxies do not exist yet\n" );
        /* refusal hygiene by hand -- see hand_get_private_data.  The d3d12
         * hand-off above may have written *out already; it did not, or it
         * would have returned rather than fallen through to here. */
        winecom_refused_scrub_ptr( out );
        return (UINT64)(UINT)E_NOTIMPL;
    }

    TRACE( "device %p, hwnd %p, output %p\n", guest_device, hwnd, guest_output );

    push_hwnd_state( hwnd );
    args[1] = (UINT64)(ULONG_PTR)host_device;
    args[2] = (UINT64)(ULONG_PTR)hwnd;
    args[3] = read_arg( ctx, 3 );
    args[4] = read_arg( ctx, 4 );
    args[5] = (UINT64)(ULONG_PTR)host_output;
    args[6] = (UINT64)(ULONG_PTR)out;
    hr = (HRESULT)unix_vtbl_call( host, slot, 7, args );
    if (FAILED(hr)) return (UINT64)(UINT)hr;

    host_swapchain = *out;
    winecom_wrap_static( out, D3D11_IFACE_IDXGISwapChain1 );
    swapchain_remember( host_swapchain, hwnd );
    return (UINT64)(UINT)hr;
}

/* The two hooks win32u performs around a present, and DXVK's Present between
 * them.  Everything here runs on the application's thread. */
static UINT64 present_common( void *host, UINT slot, UINT argc, UINT64 *args )
{
    HWND hwnd = swapchain_hwnd( host );
    HRESULT hr;

    if (!hwnd)
    {
        static BOOL logged;

        if (!logged)
        {
            logged = TRUE;
            WARN( "presenting swapchain %p, which this module never saw "
                  "created -- no window is known for it, so win32u's client "
                  "surface will not be updated around the present.  A "
                  "COMPOSITION swapchain lands here legitimately (it has no "
                  "window by construction; DXVK's dummy path answers the "
                  "present itself, usually with a surface-creation failure, "
                  "which is its native semantics).  Otherwise: the "
                  "application got it from a path that is not "
                  "CreateSwapChain/CreateSwapChainForHwnd/"
                  "D3D11CreateDeviceAndSwapChain, or DXVK stopped answering "
                  "QueryInterface for the IDXGISwapChain revisions with one "
                  "address.\n", host );
        }
        return unix_vtbl_call( host, slot, argc, args );
    }

    push_hwnd_state( hwnd );
    present_hook( hwnd, PRESENT_PHASE_BEGIN, S_OK );
    hr = (HRESULT)unix_vtbl_call( host, slot, argc, args );
    present_hook( hwnd, PRESENT_PHASE_END, hr );
    return (UINT64)(UINT)hr;
}

/* IDXGISwapChain::Present( UINT sync_interval, UINT flags ). */
static UINT64 hand_swapchain_present( void *host, UINT slot, AMD64_CONTEXT *ctx )
{
    UINT64 args[D3D11_UNIX_MAX_ARGS] = { 0 };

    args[1] = read_arg( ctx, 1 );
    args[2] = read_arg( ctx, 2 );
    return present_common( host, slot, 3, args );
}

/* IDXGISwapChain1::Present1( UINT sync_interval, UINT flags,
 * const DXGI_PRESENT_PARAMETERS *params ).  The parameter block is RECTs and
 * a POINT by pointer -- plain data, and the same layout on both sides. */
static UINT64 hand_swapchain_present1( void *host, UINT slot, AMD64_CONTEXT *ctx )
{
    UINT64 args[D3D11_UNIX_MAX_ARGS] = { 0 };

    args[1] = read_arg( ctx, 1 );
    args[2] = read_arg( ctx, 2 );
    args[3] = read_arg( ctx, 3 );
    return present_common( host, slot, 4, args );
}

/* ------------------------------------------------- 32-bit hand walkers
 *
 * The stdcall frame a walker reads: esp[0] the return address, esp[1]
 * `this`, parameters one 4-byte slot each from esp[2] (none of these
 * methods has an 8-byte parameter).  The dispatcher performs the pop from
 * the row's geometry; a walker only reads. */

static const ULONG *frame32( const I386_CONTEXT *ctx )
{
    return (const ULONG *)(ULONG_PTR)ctx->Esp;
}

static UINT64 hand32_get_private_data( void *host, UINT slot, I386_CONTEXT *ctx )
{
    const ULONG *esp = frame32( ctx );
    const GUID *guid = (const GUID *)(ULONG_PTR)esp[2];
    UINT *size = (UINT *)(ULONG_PTR)esp[3];
    void *data = (void *)(ULONG_PTR)esp[4];
    UINT64 args[D3D11_UNIX_MAX_ARGS] = { 0 };
    struct private_iface *p;

    /* refusal hygiene by hand -- see hand_get_private_data */
    if (!guid || !size)
    {
        winecom_refused_scrub_dw( size );
        return (UINT64)(UINT)E_INVALIDARG;
    }

    RtlEnterCriticalSection( &priv_cs );
    for (p = private_ifaces; p; p = p->next)
        if (p->host == host && IsEqualGUID( &p->guid, guid )) break;
    RtlLeaveCriticalSection( &priv_cs );

    if (p)
    {
        void *proxy_host;

        /* the guest's pointer cell is FOUR bytes; so is the size contract */
        if (!data)
        {
            *size = sizeof(UINT);
            return (UINT64)(UINT)S_OK;
        }
        if (*size < sizeof(UINT))
        {
            *size = sizeof(UINT);
            return (UINT64)(UINT)DXGI_ERROR_MORE_DATA;
        }
        if (!(proxy_host = winecom_unwrap( p->guest )))
        {
            /* refusal hygiene by hand -- the i386 cell is four bytes */
            winecom_refused_scrub_dw( data );
            winecom_refused_scrub_dw( size );
            return (UINT64)(UINT)E_FAIL;
        }
        *(UINT *)data = (UINT)(ULONG_PTR)host_addref_wrap( proxy_host, p->iface );
        *size = sizeof(UINT);
        return (UINT64)(UINT)S_OK;
    }

    args[1] = (UINT64)(ULONG_PTR)guid;
    args[2] = (UINT64)(ULONG_PTR)size;
    args[3] = (UINT64)(ULONG_PTR)data;
    return unix_vtbl_call( host, slot, 4, args );
}

static UINT64 hand32_set_private_data( void *host, UINT slot, I386_CONTEXT *ctx )
{
    const ULONG *esp = frame32( ctx );
    const GUID *guid = (const GUID *)(ULONG_PTR)esp[2];
    UINT64 args[D3D11_UNIX_MAX_ARGS] = { 0 };

    if (guid) private_drop( private_take( host, guid ) );
    args[1] = (UINT64)(ULONG_PTR)guid;
    args[2] = esp[3];
    args[3] = esp[4];
    return unix_vtbl_call( host, slot, 4, args );
}

static UINT64 hand32_set_private_data_iface( void *host, UINT slot, I386_CONTEXT *ctx )
{
    const ULONG *esp = frame32( ctx );
    const GUID *guid = (const GUID *)(ULONG_PTR)esp[2];
    void *guest = (void *)(ULONG_PTR)esp[3];
    UINT64 args[D3D11_UNIX_MAX_ARGS] = { 0 };
    struct private_iface *p;
    void *host_iface = NULL;
    UINT64 ret;

    if (!guid) return (UINT64)(UINT)E_INVALIDARG;
    private_drop( private_take( host, guid ) );

    if (guest && !winecom_translate_in( guest, &host_iface ))
    {
        FIXME( "SetPrivateDataInterface with a guest-implemented object %p; "
               "reverse proxies do not exist yet\n", guest );
        return (UINT64)(UINT)E_NOTIMPL;
    }

    args[1] = (UINT64)(ULONG_PTR)guid;
    args[2] = (UINT64)(ULONG_PTR)host_iface;
    ret = unix_vtbl_call( host, slot, 3, args );
    if (FAILED((HRESULT)ret) || !guest) return ret;

    if (!(p = RtlAllocateHeap( NtCurrentTeb()->Peb->ProcessHeap, 0, sizeof(*p) )))
        return (UINT64)(UINT)E_OUTOFMEMORY;
    p->host = host;
    p->guest = guest;
    p->guid = *guid;
    p->iface = D3D11_IFACE_IUnknown;
    {
        UINT64 a[D3D11_UNIX_MAX_ARGS] = { 0 };
        unix_vtbl_call( host, 1 /* AddRef */, 1, a );
    }
    RtlEnterCriticalSection( &priv_cs );
    p->next = private_ifaces;
    private_ifaces = p;
    RtlLeaveCriticalSection( &priv_cs );
    return ret;
}

/* ClearDepthStencilView( view, UINT flags, FLOAT depth, UINT8 stencil ):
 * on i386 the float is simply the third parameter slot's bytes -- no XMM,
 * no lazy-context dance. */
static UINT64 hand32_clear_depth_stencil_view( void *host, UINT slot, I386_CONTEXT *ctx )
{
    const ULONG *esp = frame32( ctx );
    struct d3d11_float_params p = { 0 };
    void *view_host = NULL;

    if (!winecom_translate_in( (void *)(ULONG_PTR)esp[2], &view_host ))
    {
        FIXME( "ClearDepthStencilView on a guest-implemented view\n" );
        return 0;
    }
    p.self = (UINT64)(ULONG_PTR)host;
    p.res = (UINT64)(ULONG_PTR)view_host;
    p.a = esp[3];                                   /* ClearFlags */
    p.f = *(const float *)&esp[4];                  /* Depth */
    p.b = (UINT8)esp[5];                            /* Stencil */
    p.slot = slot;
    p.shape = FLOAT_SHAPE_RES_UINT_FLOAT_BYTE;
    if (D3D11_UNIX_CALL( float, &p )) ERR( "unix float call failed\n" );
    return 0;
}

static UINT64 hand32_set_resource_min_lod( void *host, UINT slot, I386_CONTEXT *ctx )
{
    const ULONG *esp = frame32( ctx );
    struct d3d11_float_params p = { 0 };
    void *res = NULL;

    if (!winecom_translate_in( (void *)(ULONG_PTR)esp[2], &res ))
    {
        FIXME( "SetResourceMinLOD on a guest-implemented resource\n" );
        return 0;
    }
    p.self = (UINT64)(ULONG_PTR)host;
    p.res = (UINT64)(ULONG_PTR)res;
    p.f = *(const float *)&esp[3];
    p.slot = slot;
    p.shape = FLOAT_SHAPE_RES_FLOAT;
    if (D3D11_UNIX_CALL( float, &p )) ERR( "unix float call failed\n" );
    return 0;
}

static UINT64 hand32_create_swapchain( void *host, UINT slot, I386_CONTEXT *ctx )
{
    const ULONG *esp = frame32( ctx );
    void *guest_device = (void *)(ULONG_PTR)esp[2];
    const void *desc32 = (const void *)(ULONG_PTR)esp[3];
    UINT *out = (UINT *)(ULONG_PTR)esp[4];          /* 4-byte guest cell */
    struct dxgi_swap_chain_desc desc;
    UINT64 args[D3D11_UNIX_MAX_ARGS] = { 0 };
    void *host_device = NULL, *host_swapchain = NULL;
    HRESULT hr;

    /* refusal hygiene by hand -- see hand_get_private_data */
    if (!desc32 || !out)
    {
        winecom_refused_scrub_dw( out );
        return (UINT64)(UINT)E_INVALIDARG;
    }
    if (guest_device && !winecom_translate_in( guest_device, &host_device ))
    {
        FIXME( "CreateSwapChain with a guest-implemented device %p\n", guest_device );
        winecom_refused_scrub_dw( out );
        return (UINT64)(UINT)E_NOTIMPL;
    }

    wine_repack32_DXGI_SWAP_CHAIN_DESC( &desc, desc32 );
    TRACE( "device %p, %ux%u, hwnd %p, windowed %d, buffers %u [i386]\n",
           guest_device, desc.BufferDesc_Width, desc.BufferDesc_Height,
           desc.OutputWindow, desc.Windowed, desc.BufferCount );

    push_hwnd_state( desc.OutputWindow );
    args[1] = (UINT64)(ULONG_PTR)host_device;
    args[2] = (UINT64)(ULONG_PTR)&desc;
    args[3] = (UINT64)(ULONG_PTR)&host_swapchain;
    hr = (HRESULT)unix_vtbl_call( host, slot, 4, args );
    if (FAILED(hr)) return (UINT64)(UINT)hr;

    swapchain_remember( host_swapchain, desc.OutputWindow );
    *out = (UINT)(ULONG_PTR)winecom_wrap( host_swapchain, D3D11_IFACE_IDXGISwapChain );
    return (UINT64)(UINT)hr;
}

/* DXGI_SWAP_CHAIN_DESC1 and DXGI_SWAP_CHAIN_FULLSCREEN_DESC carry no
 * pointer members, so both cross raw; only the out-cell narrows. */
static UINT64 hand32_create_swapchain_for_hwnd( void *host, UINT slot, I386_CONTEXT *ctx )
{
    const ULONG *esp = frame32( ctx );
    void *guest_device = (void *)(ULONG_PTR)esp[2];
    HWND hwnd = (HWND)(ULONG_PTR)esp[3];
    void *guest_output = (void *)(ULONG_PTR)esp[6];
    UINT *out = (UINT *)(ULONG_PTR)esp[7];
    UINT64 args[D3D11_UNIX_MAX_ARGS] = { 0 };
    void *host_device = NULL, *host_output = NULL, *host_swapchain = NULL;
    HRESULT hr;

    if (!out) return (UINT64)(UINT)E_INVALIDARG;
    if ((guest_device && !winecom_translate_in( guest_device, &host_device )) ||
        (guest_output && !winecom_translate_in( guest_output, &host_output )))
    {
        FIXME( "CreateSwapChainForHwnd with an untranslatable device or "
               "output on the i386 lane (the d3d12 handoff has no 32-bit "
               "path yet)\n" );
        winecom_refused_scrub_dw( out );      /* refusal hygiene by hand */
        return (UINT64)(UINT)E_NOTIMPL;
    }

    TRACE( "device %p, hwnd %p, output %p [i386]\n", guest_device, hwnd, guest_output );

    push_hwnd_state( hwnd );
    args[1] = (UINT64)(ULONG_PTR)host_device;
    args[2] = (UINT64)(ULONG_PTR)hwnd;
    args[3] = esp[4];
    args[4] = esp[5];
    args[5] = (UINT64)(ULONG_PTR)host_output;
    args[6] = (UINT64)(ULONG_PTR)&host_swapchain;
    hr = (HRESULT)unix_vtbl_call( host, slot, 7, args );
    if (FAILED(hr)) return (UINT64)(UINT)hr;

    swapchain_remember( host_swapchain, hwnd );
    *out = (UINT)(ULONG_PTR)winecom_wrap( host_swapchain, D3D11_IFACE_IDXGISwapChain1 );
    return (UINT64)(UINT)hr;
}

static UINT64 hand32_swapchain_present( void *host, UINT slot, I386_CONTEXT *ctx )
{
    const ULONG *esp = frame32( ctx );
    UINT64 args[D3D11_UNIX_MAX_ARGS] = { 0 };

    args[1] = esp[2];
    args[2] = esp[3];
    return present_common( host, slot, 3, args );
}

static UINT64 hand32_swapchain_present1( void *host, UINT slot, I386_CONTEXT *ctx )
{
    const ULONG *esp = frame32( ctx );
    UINT64 args[D3D11_UNIX_MAX_ARGS] = { 0 };
    /* DXGI_PRESENT_PARAMETERS carries RECT/POINT pointers, so its i386
     * layout diverges; 8 qwords cover (and align) the 32-byte native form */
    UINT64 params[8];

    args[1] = esp[2];
    args[2] = esp[3];
    if (esp[4])
    {
        wine_repack32_DXGI_PRESENT_PARAMETERS( params, (const void *)(ULONG_PTR)esp[4] );
        args[3] = (UINT64)(ULONG_PTR)params;
    }
    return present_common( host, slot, 4, args );
}

/* ------------------------------- Map/Unmap, i386 lane only
 *
 * ID3D11DeviceContext::Map answers with a POINTER INTO HOST MEMORY, and on
 * this 64-bit host that pointer routinely sits above 4 GiB where no i386
 * guest can address it -- truncating it into the guest's 4-byte pData cell
 * would be a silently wrong pointer, this codebase's most expensive bug
 * class (the generator refuses the mechanical repack for exactly that
 * reason; these walkers are the serve path it demands).  So the mapping is
 * BOUNCED: a guest-legal buffer sized from the resource's own description,
 * filled from the host mapping for the read modes, copied back before the
 * host Unmap for the write modes.  Buffers are cached per (resource,
 * subresource) -- a dynamic-buffer game maps the same resources every
 * frame -- and bounded by the guest's own live-mapping count.
 */

struct map_bounce
{
    struct map_bounce *next;
    void *res_host;          /* HOST resource pointer (the cache key) */
    UINT sub;
    void *low;               /* the guest-legal buffer */
    SIZE_T cap;
    void *host_ptr;          /* live host mapping, NULL when unmapped */
    SIZE_T size;             /* live mapping's byte count */
    UINT maptype;
    SIZE_T known_size;       /* the subresource's byte count, computed ONCE:
                                a resource's extent never changes, and the
                                first cut re-asked GetType+GetDesc on every
                                Map -- two host calls per dynamic-buffer
                                update, every frame [the Dex perf pass] */
    /* the WRITE-mode flush copies the whole subresource into whatever host
     * buffer DXVK handed back for THIS map -- unavoidable in general,
     * because the guest wrote into a shadow the host never sees.  But a
     * Map(WRITE_DISCARD)/Unmap pair that rewrites bytes IDENTICAL to what
     * this SAME host buffer already holds (a static UI element remapped
     * every frame out of habit, not because it changed) has nothing to
     * give the host that isn't already there, and the flush -- a write
     * into uncached/write-combined host-visible memory, the expensive
     * side of the pair -- can be skipped outright [the Dex perf pass,
     * part 2: measured cause of the 4 MiB memcpy dominating the render
     * thread].
     *
     * DXVK renames a WRITE_DISCARD resource's backing to avoid stalling
     * the GPU, so host_ptr rotates between (measured) exactly two
     * addresses for a hot dynamic texture.  A single "last thing we sent"
     * shadow would be WRONG here: buffer A's last content says nothing
     * about whether buffer B -- the one DXVK just handed back -- already
     * holds it, and skipping on that basis leaves B's stale (or plain
     * uninitialized) bytes live for the GPU to read.  So the shadow is
     * cached PER DESTINATION -- a handful of small slots, linear-searched
     * by host_ptr, each remembering the bytes last flushed into that
     * exact buffer -- and a flush is only ever skipped against the slot
     * for the buffer being written THIS time. */
    struct map_bounce_shadow
    {
        void *host_ptr;      /* which host buffer this describes, NULL == free */
        void *data;
        SIZE_T cap;
        SIZE_T size;          /* 0 == does not describe a valid flush */
    } shadows[4];
    UINT shadow_next;         /* round-robin slot to evict when all four are live */
};

static CRITICAL_SECTION bounce_cs;
static CRITICAL_SECTION_DEBUG bounce_cs_debug =
{
    0, 0, &bounce_cs,
    { &bounce_cs_debug.ProcessLocksList, &bounce_cs_debug.ProcessLocksList },
      0, 0, { (DWORD_PTR)(__FILE__ ": d3d11 bounce_cs") }
};
static CRITICAL_SECTION bounce_cs = { &bounce_cs_debug, -1, 0, 0, 0, 0 };
static struct map_bounce *map_bounces;

/* ID3D11Resource's fixed vtable geometry: IUnknown 3 + DeviceChild 4,
 * GetType at 7; the derived interfaces put GetDesc at 10. */
#define D3D11_RESOURCE_SLOT_GET_TYPE 7
#define D3D11_RESOURCE_SLOT_GET_DESC 10
enum { D3D11_DIM_BUFFER = 1, D3D11_DIM_TEX1D = 2, D3D11_DIM_TEX2D = 3,
       D3D11_DIM_TEX3D = 4 };

/* The byte size of one mapped subresource, from the resource's own
 * description plus what the host Map answered.  0 = cannot be sized, and
 * the walker refuses rather than guesses. */
static SIZE_T map_subresource_size( void *res_host, UINT sub,
                                    UINT row_pitch, UINT depth_pitch )
{
    UINT64 args[D3D11_UNIX_MAX_ARGS] = { 0 };
    UINT dim = 0;
    UINT desc[16];

    args[1] = (UINT64)(ULONG_PTR)&dim;
    unix_vtbl_call( res_host, D3D11_RESOURCE_SLOT_GET_TYPE, 2, args );
    memset( desc, 0, sizeof(desc) );
    args[1] = (UINT64)(ULONG_PTR)desc;
    if (dim != D3D11_DIM_BUFFER)
        unix_vtbl_call( res_host, D3D11_RESOURCE_SLOT_GET_DESC, 2, args );

    switch (dim)
    {
    case D3D11_DIM_BUFFER:
        /* D3D11_BUFFER_DESC::ByteWidth */
        unix_vtbl_call( res_host, D3D11_RESOURCE_SLOT_GET_DESC, 2, args );
        return desc[0];
    case D3D11_DIM_TEX1D:
    case D3D11_DIM_TEX2D:
        /* one mip slice: DXVK's Map fills DepthPitch with exactly that */
        return depth_pitch;
    case D3D11_DIM_TEX3D:
    {
        /* D3D11_TEXTURE3D_DESC: Width, Height, Depth, MipLevels, ... */
        UINT mips = desc[3] ? desc[3] : 1;
        UINT mip = mips ? sub % mips : 0;
        UINT d = desc[2] >> mip;

        if (!d) d = 1;
        return (SIZE_T)depth_pitch * d;
    }
    }
    return 0;
}

static UINT64 hand32_map( void *host, UINT slot, I386_CONTEXT *ctx )
{
    const ULONG *esp = frame32( ctx );
    void *guest_res = (void *)(ULONG_PTR)esp[2];
    UINT sub = esp[3], maptype = esp[4], mapflags = esp[5];
    UINT *out = (UINT *)(ULONG_PTR)esp[6];   /* {pData, RowPitch, DepthPitch} x 4 bytes */
    UINT64 args[D3D11_UNIX_MAX_ARGS] = { 0 };
    UINT64 mapped[2] = { 0 };                /* native D3D11_MAPPED_SUBRESOURCE */
    void *res_host = NULL;
    UINT row_pitch, depth_pitch;
    void *ptr;
    HRESULT hr;

    if (!out) return (UINT64)(UINT)E_INVALIDARG;
    if (!winecom_translate_in( guest_res, &res_host ) || !res_host)
    {
        FIXME( "Map on an untranslatable resource %p\n", guest_res );
        /* refusal hygiene by hand: pData is the residue cell here */
        winecom_refused_scrub_mem( out, 3 * sizeof(UINT) );
        return (UINT64)(UINT)E_INVALIDARG;
    }

    args[1] = (UINT64)(ULONG_PTR)res_host;
    args[2] = sub;
    args[3] = maptype;
    args[4] = mapflags;
    args[5] = (UINT64)(ULONG_PTR)mapped;
    hr = (HRESULT)unix_vtbl_call( host, slot, 6, args );
    if (FAILED(hr))
    {
        out[0] = out[1] = out[2] = 0;
        return (UINT64)(UINT)hr;
    }
    ptr = (void *)(ULONG_PTR)mapped[0];
    row_pitch = (UINT)mapped[1];
    depth_pitch = (UINT)(mapped[1] >> 32);

    if ((ULONG_PTR)ptr < 0x100000000ull)
    {
        /* already guest-legal: no bounce, no copies */
        TRACE( "map32: resource %p sub %u mapped guest-legal at %p\n",
               res_host, sub, ptr );
        out[0] = (UINT)(ULONG_PTR)ptr;
        out[1] = row_pitch;
        out[2] = depth_pitch;
        return (UINT64)(UINT)hr;
    }

    {
        struct map_bounce *b;
        SIZE_T size = 0;

        RtlEnterCriticalSection( &bounce_cs );
        for (b = map_bounces; b; b = b->next)
            if (b->res_host == res_host && b->sub == sub) break;
        if (b) size = b->known_size;
        RtlLeaveCriticalSection( &bounce_cs );
        if (!size)
            size = map_subresource_size( res_host, sub, row_pitch, depth_pitch );

        if (!size)
        {
            ERR( "cannot size the mapping of resource %p sub %u; unmapping "
                 "and refusing rather than truncating pData\n", res_host, sub );
            args[1] = (UINT64)(ULONG_PTR)res_host;
            args[2] = sub;
            unix_vtbl_call( host, slot + 1 /* Unmap */, 3, args );
            winecom_refused_scrub_mem( out, 3 * sizeof(UINT) );
            return (UINT64)(UINT)E_NOTIMPL;
        }

        RtlEnterCriticalSection( &bounce_cs );
        for (b = map_bounces; b; b = b->next)
            if (b->res_host == res_host && b->sub == sub) break;
        if (b) b->known_size = size;
        if (b && b->cap < size)
        {
            SIZE_T zero = 0;
            NtFreeVirtualMemory( NtCurrentProcess(), &b->low, &zero, MEM_RELEASE );
            b->low = NULL;
            b->cap = 0;
            /* every cached shadow describes a flush at the OLD size; a
             * resize means a new subresource geometry, so none of them
             * can be trusted any more */
            {
                UINT i;
                for (i = 0; i < ARRAY_SIZE(b->shadows); i++)
                {
                    if (b->shadows[i].data)
                        RtlFreeHeap( NtCurrentTeb()->Peb->ProcessHeap, 0, b->shadows[i].data );
                    memset( &b->shadows[i], 0, sizeof(b->shadows[i]) );
                }
            }
        }
        if (!b)
        {
            if ((b = RtlAllocateHeap( NtCurrentTeb()->Peb->ProcessHeap,
                                      HEAP_ZERO_MEMORY, sizeof(*b) )))
            {
                b->res_host = res_host;
                b->sub = sub;
                b->known_size = size;
                b->next = map_bounces;
                map_bounces = b;
            }
        }
        if (b && !b->low)
        {
            SIZE_T cap = (size + 0xffff) & ~(SIZE_T)0xffff;
            void *mem = NULL;

            if (!NtAllocateVirtualMemory( NtCurrentProcess(), &mem, 0x7fffffff, &cap,
                                          MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE ))
            {
                b->low = mem;
                b->cap = cap;
            }
        }

        if (!b || !b->low)
        {
            RtlLeaveCriticalSection( &bounce_cs );
            ERR( "no guest-legal bounce for a %Iu-byte mapping; unmapping\n", size );
            args[1] = (UINT64)(ULONG_PTR)res_host;
            args[2] = sub;
            unix_vtbl_call( host, slot + 1 /* Unmap */, 3, args );
            out[0] = out[1] = out[2] = 0;
            return (UINT64)(UINT)E_OUTOFMEMORY;
        }
        b->host_ptr = ptr;
        b->size = size;
        b->maptype = maptype;
        TRACE( "map32: resource %p sub %u BOUNCED %p -> %p (%Iu bytes, type %u)\n",
               res_host, sub, ptr, b->low, size, maptype );
        /* READ(1) and READ_WRITE(3) see the resource's current bytes */
        if (maptype == 1 || maptype == 3) memcpy( b->low, ptr, size );
        out[0] = (UINT)(ULONG_PTR)b->low;
        out[1] = row_pitch;
        out[2] = depth_pitch;
        RtlLeaveCriticalSection( &bounce_cs );
    }
    return (UINT64)(UINT)hr;
}

static UINT64 hand32_unmap( void *host, UINT slot, I386_CONTEXT *ctx )
{
    const ULONG *esp = frame32( ctx );
    void *guest_res = (void *)(ULONG_PTR)esp[2];
    UINT sub = esp[3];
    UINT64 args[D3D11_UNIX_MAX_ARGS] = { 0 };
    void *res_host = NULL;
    struct map_bounce *b;

    if (!winecom_translate_in( guest_res, &res_host ) || !res_host)
        return 0;

    RtlEnterCriticalSection( &bounce_cs );
    for (b = map_bounces; b; b = b->next)
        if (b->res_host == res_host && b->sub == sub) break;
    if (b && b->host_ptr)
    {
        /* every WRITE mode (2, 3, 4, 5) flushes the guest's bytes back --
         * unless the destination host buffer THIS map cycle landed on
         * already holds these exact bytes, per its own shadow slot (see
         * the big comment on struct map_bounce). */
        if (b->maptype != 1)
        {
            struct map_bounce_shadow *sh = NULL;
            UINT i;
            BOOL identical;

            for (i = 0; i < ARRAY_SIZE(b->shadows); i++)
                if (b->shadows[i].host_ptr == b->host_ptr) { sh = &b->shadows[i]; break; }

            identical = sh && sh->size == b->size && !memcmp( b->low, sh->data, b->size );

            if (!identical)
            {
                memcpy( b->host_ptr, b->low, b->size );

                if (!sh)
                {
                    /* claim this destination a slot -- round-robin once
                     * all four are in use by distinct host buffers, which
                     * is generous next to the two DXVK has shown so far */
                    sh = &b->shadows[b->shadow_next];
                    b->shadow_next = (b->shadow_next + 1) % ARRAY_SIZE(b->shadows);
                    sh->host_ptr = b->host_ptr;
                    sh->size = 0;
                }
                if (sh->cap < b->size)
                {
                    /* the shadow is host-side bookkeeping only (compared
                     * against, never handed to the guest), so a plain
                     * process-heap allocation is fine -- no need for the
                     * guest-legal low-2GiB arena b->low requires */
                    void *mem = sh->data
                        ? RtlReAllocateHeap( NtCurrentTeb()->Peb->ProcessHeap, 0, sh->data, b->size )
                        : RtlAllocateHeap( NtCurrentTeb()->Peb->ProcessHeap, 0, b->size );
                    if (mem) { sh->data = mem; sh->cap = b->size; }
                    else sh->cap = 0;
                }
                if (sh->cap >= b->size)
                {
                    memcpy( sh->data, b->low, b->size );
                    sh->size = b->size;
                }
                else
                    sh->size = 0;   /* allocation failed: never claim a match */
            }
        }
        b->host_ptr = NULL;
    }
    RtlLeaveCriticalSection( &bounce_cs );

    args[1] = (UINT64)(ULONG_PTR)res_host;
    args[2] = sub;
    unix_vtbl_call( host, slot, 3, args );
    return 0;
}

/* ------------------------- the texture creates, i386 lane only
 *
 * Their initial-data parameter is an ARRAY of the divergent
 * D3D11_SUBRESOURCE_DATA whose element count is MipLevels x ArraySize out
 * of the DESC -- beyond any mechanical rep, which is why the generator
 * refuse32's these rows and this table serves them.  The descs themselves
 * are all-UINT and cross raw; only what is read here is mirrored, the same
 * rule as struct dxgi_swap_chain_desc. */

static UINT full_mip_chain( UINT w, UINT h, UINT d )
{
    UINT m = 1, e = w > h ? w : h;

    if (d > e) e = d;
    while (e > 1) { e >>= 1; m++; }
    return m;
}

static UINT64 create_texture32_common( void *host, UINT slot, I386_CONTEXT *ctx,
                                       UINT nsub, UINT out_iface )
{
    const ULONG *esp = frame32( ctx );
    const void *desc = (const void *)(ULONG_PTR)esp[2];
    const char *init32 = (const char *)(ULONG_PTR)esp[3];
    UINT *out = (UINT *)(ULONG_PTR)esp[4];
    UINT64 args[D3D11_UNIX_MAX_ARGS] = { 0 };
    void *host_tex = NULL;
    void *heap = NULL;
    HRESULT hr;

    /* refusal hygiene by hand -- see hand_get_private_data */
    if (!desc)
    {
        winecom_refused_scrub_dw( out );
        return (UINT64)(UINT)E_INVALIDARG;
    }
    if (init32 && nsub)
    {
        char *dst;
        UINT k;

        if (!(heap = RtlAllocateHeap( NtCurrentTeb()->Peb->ProcessHeap, 0,
                                      nsub * (SIZE_T)16 )))
        {
            winecom_refused_scrub_dw( out );
            return (UINT64)(UINT)E_OUTOFMEMORY;
        }
        for (k = 0, dst = heap; k < nsub; k++, dst += 16)
            wine_repack32_D3D11_SUBRESOURCE_DATA( dst, init32 + (SIZE_T)k * 12 );
        C_ASSERT( WINE_REPACK32_SIZE_D3D11_SUBRESOURCE_DATA == 12 );
    }
    args[1] = (UINT64)(ULONG_PTR)desc;
    args[2] = (UINT64)(ULONG_PTR)(init32 ? heap : NULL);
    args[3] = (UINT64)(ULONG_PTR)(out ? (void *)&host_tex : NULL);
    hr = (HRESULT)unix_vtbl_call( host, slot, 4, args );
    if (heap) RtlFreeHeap( NtCurrentTeb()->Peb->ProcessHeap, 0, heap );
    if (out)
        *out = SUCCEEDED(hr) && host_tex
             ? (UINT)(ULONG_PTR)winecom_wrap( host_tex, out_iface ) : 0;
    return (UINT64)(UINT)hr;
}

static UINT64 hand32_create_texture1d( void *host, UINT slot, I386_CONTEXT *ctx )
{
    const UINT *d = (const UINT *)(ULONG_PTR)frame32( ctx )[2];
    UINT mips, nsub = 0;

    if (d)
    {
        mips = d[1] ? d[1] : full_mip_chain( d[0], 1, 1 );
        nsub = mips * d[2];                       /* MipLevels x ArraySize */
    }
    return create_texture32_common( host, slot, ctx, nsub,
                                    D3D11_IFACE_ID3D11Texture1D );
}

static UINT64 hand32_create_texture2d( void *host, UINT slot, I386_CONTEXT *ctx )
{
    const UINT *d = (const UINT *)(ULONG_PTR)frame32( ctx )[2];
    UINT mips, nsub = 0;

    if (d)
    {
        mips = d[2] ? d[2] : full_mip_chain( d[0], d[1], 1 );
        nsub = mips * d[3];                       /* MipLevels x ArraySize */
    }
    return create_texture32_common( host, slot, ctx, nsub,
                                    D3D11_IFACE_ID3D11Texture2D );
}

static UINT64 hand32_create_texture3d( void *host, UINT slot, I386_CONTEXT *ctx )
{
    const UINT *d = (const UINT *)(ULONG_PTR)frame32( ctx )[2];
    UINT nsub = 0;

    if (d) nsub = d[3] ? d[3] : full_mip_chain( d[0], d[1], d[2] );
    return create_texture32_common( host, slot, ctx, nsub,
                                    D3D11_IFACE_ID3D11Texture3D );
}

/* ---------------------------------------------------------- flat entries */

/* guest32 out-cell staging for the flat entries.  On the i386 lane the
 * guest's interface-out cell is FOUR bytes, and both the unixlib and the
 * wrap helpers write eight: stage_out() redirects such a cell to a native
 * local, and unstage_out() narrows the wrapped result back.  On the 64-bit
 * lane both are no-ops and the cell is used directly, exactly as before. */
static void **stage_out( void **cell, void **local )
{
    if (!winecom_guest32() || !cell) return cell;
    *local = NULL;
    return local;
}

static void unstage_out( void **cell, void **staged )
{
    if (cell && staged != cell) winecom_store_guest_ptr( cell, *staged );
}

static HRESULT flat_call( UINT func, UINT argc, UINT64 *args, UINT64 *ret )
{
    struct d3d11_flat_params p;
    NTSTATUS status;

    if (!com_runtime_init()) return E_FAIL;
    memcpy( p.args, args, sizeof(p.args) );
    p.func = func;
    p.argc = argc;
    p.ret = 0;
    if ((status = D3D11_UNIX_CALL( flat, &p )))
    {
        ERR( "unix flat call %u failed, status %08x\n", func, (UINT)status );
        return E_FAIL;
    }
    if (ret) *ret = p.ret;
    return (HRESULT)p.ret;
}

/* The refusal every plain-named flat export shares.  A native ppc64 PE that
 * calls one of these would be handed proxies whose vtables are the guest's
 * x86-64 trap stubs; there is no correct answer to give it, so it is told so
 * by name rather than served something that faults later somewhere else. */
static HRESULT refuse_native_caller( const char *name )
{
    FIXME( "%s called by a NATIVE ppc64 caller.  This lane's D3D11 objects are "
           "guest proxies whose vtables are x86-64 trap stubs; only the "
           "emulated guest can call them.  The guest reaches this module "
           "through __wine_guest_%s (see dlls/d3d11/d3d11.thunks).\n",
           name, name );
    return E_NOTIMPL;
}

#define REFUSE_NATIVE(name) return refuse_native_caller( #name )

/* ---- d3d11.dll ---- */

HRESULT WINAPI __wine_guest_D3D11CreateDevice( void *adapter, UINT driver_type,
                                               void *software, UINT flags,
                                               const void *feature_levels,
                                               UINT levels, UINT sdk_version,
                                               void **device, UINT *feature_level,
                                               void **context )
{
    UINT64 args[12] = { 0 };
    void *host_adapter = NULL, *dl = NULL, *cl = NULL;
    void **device_s, **context_s;
    HRESULT hr;

    TRACE( "adapter %p, driver_type %#x, flags %#x, levels %u, device %p, "
           "context %p\n", adapter, driver_type, flags, levels, device, context );

    /* refusal hygiene by hand -- see hand_get_private_data */
    winecom_refused_scrub_ptr( device );
    winecom_refused_scrub_ptr( context );
    winecom_refused_scrub_dw( feature_level );
    if (!com_runtime_init()) return E_FAIL;
    if (adapter && !winecom_translate_in( adapter, &host_adapter ))
    {
        FIXME( "D3D11CreateDevice with a guest-implemented IDXGIAdapter %p; "
               "reverse proxies do not exist yet\n", adapter );
        return E_NOTIMPL;
    }
    device_s = stage_out( device, &dl );
    context_s = stage_out( context, &cl );
    args[0] = (UINT64)(ULONG_PTR)host_adapter;
    args[1] = driver_type;
    args[2] = (UINT64)(ULONG_PTR)software;
    args[3] = flags;
    args[4] = (UINT64)(ULONG_PTR)feature_levels;
    args[5] = levels;
    args[6] = sdk_version;
    args[7] = (UINT64)(ULONG_PTR)device_s;
    args[8] = (UINT64)(ULONG_PTR)feature_level;
    args[9] = (UINT64)(ULONG_PTR)context_s;
    hr = flat_call( FLAT_D3D11CreateDevice, 10, args, NULL );
    if (FAILED(hr)) return hr;
    /* Both out-interfaces are statically typed, so there is no REFIID to look
     * up; the roster index is fixed here. */
    winecom_wrap_static( device_s, D3D11_IFACE_ID3D11Device );
    winecom_wrap_static( context_s, D3D11_IFACE_ID3D11DeviceContext );
    unstage_out( device, device_s );
    unstage_out( context, context_s );
    return hr;
}

HRESULT WINAPI D3D11CreateDevice( void *adapter, UINT driver_type, void *software,
                                  UINT flags, const void *feature_levels, UINT levels,
                                  UINT sdk_version, void **device, UINT *feature_level,
                                  void **context )
{
    REFUSE_NATIVE(D3D11CreateDevice);
}

HRESULT WINAPI __wine_guest_D3D11CoreCreateDevice( void *factory, void *adapter,
                                                   UINT flags, const void *feature_levels,
                                                   UINT levels, void **device )
{
    UINT64 args[12] = { 0 };
    void *host_factory = NULL, *host_adapter = NULL, *dl = NULL;
    void **device_s;
    HRESULT hr;

    TRACE( "factory %p, adapter %p, flags %#x, levels %u, device %p\n",
           factory, adapter, flags, levels, device );

    /* refusal hygiene by hand -- see hand_get_private_data */
    winecom_refused_scrub_ptr( device );
    if (!com_runtime_init()) return E_FAIL;
    if ((factory && !winecom_translate_in( factory, &host_factory )) ||
        (adapter && !winecom_translate_in( adapter, &host_adapter )))
    {
        FIXME( "D3D11CoreCreateDevice with a guest-implemented DXGI object\n" );
        return E_NOTIMPL;
    }
    device_s = stage_out( device, &dl );
    args[0] = (UINT64)(ULONG_PTR)host_factory;
    args[1] = (UINT64)(ULONG_PTR)host_adapter;
    args[2] = flags;
    args[3] = (UINT64)(ULONG_PTR)feature_levels;
    args[4] = levels;
    args[5] = (UINT64)(ULONG_PTR)device_s;
    hr = flat_call( FLAT_D3D11CoreCreateDevice, 6, args, NULL );
    if (FAILED(hr)) return hr;
    winecom_wrap_static( device_s, D3D11_IFACE_ID3D11Device );
    unstage_out( device, device_s );
    return hr;
}

HRESULT WINAPI D3D11CoreCreateDevice( void *factory, void *adapter, UINT flags,
                                      const void *feature_levels, UINT levels,
                                      void **device )
{
    REFUSE_NATIVE(D3D11CoreCreateDevice);
}

HRESULT WINAPI __wine_guest_D3D11CreateDeviceAndSwapChain( void *adapter, UINT driver_type,
                                                           void *software, UINT flags,
                                                           const void *feature_levels,
                                                           UINT levels, UINT sdk_version,
                                                           const void *swapchain_desc,
                                                           void **swapchain, void **device,
                                                           UINT *feature_level,
                                                           void **context )
{
    const struct dxgi_swap_chain_desc *desc = swapchain_desc;
    struct dxgi_swap_chain_desc desc_native;
    UINT64 args[12] = { 0 };
    void *host_adapter = NULL, *host_swapchain = NULL;
    void *sl = NULL, *dl = NULL, *cl = NULL;
    void **swapchain_s, **device_s, **context_s;
    HRESULT hr;

    TRACE( "adapter %p, driver_type %#x, flags %#x, levels %u, desc %p, "
           "swapchain %p, device %p, context %p\n", adapter, driver_type, flags,
           levels, desc, swapchain, device, context );

    /* refusal hygiene by hand -- see hand_get_private_data */
    winecom_refused_scrub_ptr( swapchain );
    winecom_refused_scrub_ptr( device );
    winecom_refused_scrub_ptr( context );
    winecom_refused_scrub_dw( feature_level );
    if (!com_runtime_init()) return E_FAIL;
    if (adapter && !winecom_translate_in( adapter, &host_adapter ))
    {
        FIXME( "D3D11CreateDeviceAndSwapChain with a guest-implemented "
               "IDXGIAdapter %p; reverse proxies do not exist yet\n", adapter );
        return E_NOTIMPL;
    }

    /* An i386 guest's DXGI_SWAP_CHAIN_DESC lays HWND out in four bytes and
     * everything after it moves: repack into the native form first. */
    if (desc && winecom_guest32())
    {
        wine_repack32_DXGI_SWAP_CHAIN_DESC( &desc_native, swapchain_desc );
        desc = &desc_native;
    }

    /* The window's client size has to be in the unixlib's hands before DXVK
     * builds the swapchain, because DXVK asks for it during construction --
     * and an application that passed 0x0 in the descriptor is asking to be
     * sized to the window. */
    if (desc) push_hwnd_state( desc->OutputWindow );

    swapchain_s = stage_out( swapchain, &sl );
    device_s = stage_out( device, &dl );
    context_s = stage_out( context, &cl );
    args[0] = (UINT64)(ULONG_PTR)host_adapter;
    args[1] = driver_type;
    args[2] = (UINT64)(ULONG_PTR)software;
    args[3] = flags;
    args[4] = (UINT64)(ULONG_PTR)feature_levels;
    args[5] = levels;
    args[6] = sdk_version;
    args[7] = (UINT64)(ULONG_PTR)desc;
    args[8] = (UINT64)(ULONG_PTR)swapchain_s;
    args[9] = (UINT64)(ULONG_PTR)device_s;
    args[10] = (UINT64)(ULONG_PTR)feature_level;
    args[11] = (UINT64)(ULONG_PTR)context_s;
    hr = flat_call( FLAT_D3D11CreateDeviceAndSwapChain, 12, args, NULL );
    if (FAILED(hr)) return hr;

    if (swapchain_s) host_swapchain = *swapchain_s;
    winecom_wrap_static( swapchain_s, D3D11_IFACE_IDXGISwapChain );
    winecom_wrap_static( device_s, D3D11_IFACE_ID3D11Device );
    winecom_wrap_static( context_s, D3D11_IFACE_ID3D11DeviceContext );
    unstage_out( swapchain, swapchain_s );
    unstage_out( device, device_s );
    unstage_out( context, context_s );
    if (desc) swapchain_remember( host_swapchain, desc->OutputWindow );
    return hr;
}

HRESULT WINAPI D3D11CreateDeviceAndSwapChain( void *adapter, UINT driver_type,
                                              void *software, UINT flags,
                                              const void *feature_levels, UINT levels,
                                              UINT sdk_version, const void *swapchain_desc,
                                              void **swapchain, void **device,
                                              UINT *feature_level, void **context )
{
    REFUSE_NATIVE(D3D11CreateDeviceAndSwapChain);
}

HRESULT WINAPI __wine_guest_D3D11On12CreateDevice( void *device12, UINT flags,
                                                   const void *feature_levels, UINT levels,
                                                   void **queues, UINT queue_count,
                                                   UINT node_mask, void **device,
                                                   void **context, UINT *feature_level )
{
    static BOOL logged;

    if (!logged)
    {
        logged = TRUE;
        FIXME( "D3D11On12CreateDevice: this needs a live ID3D12Device from the "
               "d3d12 lane, and the two lanes hold SEPARATE winecom instances "
               "(libs/winecom state is per-linkee).  A d3d12 proxy handed to "
               "this module's runtime is not one of its proxies; it would be "
               "refused a frame later, in the middle of a resource wrap, where "
               "the reason would be illegible.  Refused here instead.\n" );
    }
    /* refusal hygiene by hand -- see hand_get_private_data */
    winecom_refused_scrub_ptr( device );
    winecom_refused_scrub_ptr( context );
    winecom_refused_scrub_dw( feature_level );
    return E_NOTIMPL;
}

HRESULT WINAPI D3D11On12CreateDevice( void *device12, UINT flags,
                                      const void *feature_levels, UINT levels,
                                      void **queues, UINT queue_count, UINT node_mask,
                                      void **device, void **context, UINT *feature_level )
{
    REFUSE_NATIVE(D3D11On12CreateDevice);
}

HRESULT WINAPI D3D11CoreRegisterLayers( void )
{
    TRACE( "\n" );
    return S_OK;
}

/* ---- dxgi.dll (forwarded here by dlls/dxgi/dxgi.spec) ---- */

HRESULT WINAPI __wine_guest_CreateDXGIFactory( const GUID *riid, void **factory )
{
    UINT64 args[12] = { 0 };
    void *ol = NULL, **out_s;
    HRESULT hr;

    TRACE( "riid %s, factory %p\n", debugstr_guid(riid), factory );
    if (!com_runtime_init()) return E_FAIL;
    out_s = stage_out( factory, &ol );
    args[0] = (UINT64)(ULONG_PTR)riid;
    args[1] = (UINT64)(ULONG_PTR)out_s;
    hr = flat_call( FLAT_CreateDXGIFactory, 2, args, NULL );
    hr = winecom_wrap_out_iface( hr, riid, out_s );
    unstage_out( factory, out_s );
    return hr;
}

HRESULT WINAPI CreateDXGIFactory( const GUID *riid, void **factory )
{
    REFUSE_NATIVE(CreateDXGIFactory);
}

HRESULT WINAPI __wine_guest_CreateDXGIFactory1( const GUID *riid, void **factory )
{
    UINT64 args[12] = { 0 };
    void *ol = NULL, **out_s;
    HRESULT hr;

    TRACE( "riid %s, factory %p\n", debugstr_guid(riid), factory );
    if (!com_runtime_init()) return E_FAIL;
    out_s = stage_out( factory, &ol );
    args[0] = (UINT64)(ULONG_PTR)riid;
    args[1] = (UINT64)(ULONG_PTR)out_s;
    hr = flat_call( FLAT_CreateDXGIFactory1, 2, args, NULL );
    hr = winecom_wrap_out_iface( hr, riid, out_s );
    unstage_out( factory, out_s );
    return hr;
}

HRESULT WINAPI CreateDXGIFactory1( const GUID *riid, void **factory )
{
    REFUSE_NATIVE(CreateDXGIFactory1);
}

HRESULT WINAPI __wine_guest_CreateDXGIFactory2( UINT flags, const GUID *riid,
                                                void **factory )
{
    UINT64 args[12] = { 0 };
    void *ol = NULL, **out_s;
    HRESULT hr;

    TRACE( "flags %#x, riid %s, factory %p\n", flags, debugstr_guid(riid), factory );
    if (!com_runtime_init()) return E_FAIL;
    out_s = stage_out( factory, &ol );
    args[0] = flags;
    args[1] = (UINT64)(ULONG_PTR)riid;
    args[2] = (UINT64)(ULONG_PTR)out_s;
    hr = flat_call( FLAT_CreateDXGIFactory2, 3, args, NULL );
    hr = winecom_wrap_out_iface( hr, riid, out_s );
    unstage_out( factory, out_s );
    return hr;
}

HRESULT WINAPI CreateDXGIFactory2( UINT flags, const GUID *riid, void **factory )
{
    REFUSE_NATIVE(CreateDXGIFactory2);
}

HRESULT WINAPI __wine_guest_DXGIGetDebugInterface1( UINT flags, const GUID *riid,
                                                    void **debug )
{
    UINT64 args[12] = { 0 };
    void *ol = NULL, **out_s;
    HRESULT hr;

    TRACE( "flags %#x, riid %s, debug %p\n", flags, debugstr_guid(riid), debug );
    if (!com_runtime_init()) return E_FAIL;
    out_s = stage_out( debug, &ol );
    args[0] = flags;
    args[1] = (UINT64)(ULONG_PTR)riid;
    args[2] = (UINT64)(ULONG_PTR)out_s;
    hr = flat_call( FLAT_DXGIGetDebugInterface1, 3, args, NULL );
    hr = winecom_wrap_out_iface( hr, riid, out_s );
    unstage_out( debug, out_s );
    return hr;
}

HRESULT WINAPI DXGIGetDebugInterface1( UINT flags, const GUID *riid, void **debug )
{
    REFUSE_NATIVE(DXGIGetDebugInterface1);
}

/* Carries no interface pointer, so the guest and native forms are the same
 * function and no GUEST-IMPL row is needed for it. */
HRESULT WINAPI DXGIDeclareAdapterRemovalSupport( void )
{
    UINT64 args[12] = { 0 };

    TRACE( "\n" );
    return flat_call( FLAT_DXGIDeclareAdapterRemovalSupport, 0, args, NULL );
}

HRESULT WINAPI __wine_guest_DXGID3D10CreateDevice( void *d3d11_module, void *factory,
                                                   void *adapter, UINT flags,
                                                   const void *feature_levels,
                                                   UINT levels, void **device )
{
    static BOOL logged;

    if (!logged)
    {
        logged = TRUE;
        FIXME( "DXGID3D10CreateDevice: a Wine-private back door between Wine's "
               "own dxgi and d3d10core, which this lane replaces on both sides "
               "-- DXVK's d3d10core reaches its device through "
               "D3D10CoreCreateDevice and never calls this.  Its first "
               "argument is an HMODULE of Wine's d3d11, which does not exist "
               "here.\n" );
    }
    winecom_refused_scrub_ptr( device );   /* refusal hygiene by hand */
    return E_NOTIMPL;
}

HRESULT WINAPI DXGID3D10CreateDevice( void *d3d11_module, void *factory, void *adapter,
                                      UINT flags, const void *feature_levels,
                                      UINT levels, void **device )
{
    REFUSE_NATIVE(DXGID3D10CreateDevice);
}

HRESULT WINAPI DXGID3D10RegisterLayers( void *layers, UINT layer_count )
{
    FIXME( "layers %p, count %u: the Wine-private dxgi/d3d10core layer "
           "registration has no counterpart in DXVK's d3d10core, which this "
           "lane serves instead\n", layers, layer_count );
    return E_NOTIMPL;
}

/* ---- d3d10core.dll (forwarded here by dlls/d3d10core/d3d10core.spec) ---- */

HRESULT WINAPI __wine_guest_D3D10CoreCreateDevice( void *factory, void *adapter,
                                                   UINT flags, UINT feature_level,
                                                   void **device )
{
    UINT64 args[12] = { 0 };
    void *host_factory = NULL, *host_adapter = NULL, *dl = NULL;
    void **device_s;
    HRESULT hr;

    TRACE( "factory %p, adapter %p, flags %#x, feature_level %#x, device %p\n",
           factory, adapter, flags, feature_level, device );

    /* refusal hygiene by hand -- see hand_get_private_data */
    winecom_refused_scrub_ptr( device );
    if (!com_runtime_init()) return E_FAIL;
    if ((factory && !winecom_translate_in( factory, &host_factory )) ||
        (adapter && !winecom_translate_in( adapter, &host_adapter )))
    {
        FIXME( "D3D10CoreCreateDevice with a guest-implemented DXGI object\n" );
        return E_NOTIMPL;
    }
    device_s = stage_out( device, &dl );
    args[0] = (UINT64)(ULONG_PTR)host_factory;
    args[1] = (UINT64)(ULONG_PTR)host_adapter;
    args[2] = flags;
    args[3] = feature_level;
    args[4] = (UINT64)(ULONG_PTR)device_s;
    hr = flat_call( FLAT_D3D10CoreCreateDevice, 5, args, NULL );
    if (FAILED(hr)) return hr;
    winecom_wrap_static( device_s, D3D11_IFACE_ID3D10Device );
    unstage_out( device, device_s );
    return hr;
}

HRESULT WINAPI D3D10CoreCreateDevice( void *factory, void *adapter, UINT flags,
                                      UINT feature_level, void **device )
{
    REFUSE_NATIVE(D3D10CoreCreateDevice);
}

/* ---- d3d10.dll (forwarded here by dlls/d3d10/d3d10.spec) ----
 *
 * D3D10's DEVICE CREATION, AND ONLY THAT.  DXVK ships no d3d10.dll: upstream
 * removed it and kept d3d10core, which is a thin layer over its own d3d11 and
 * is what this lane serves.  Everything else Wine's d3d10.dll exports -- the
 * effects framework, the state-block helpers, the shader reflection -- is
 * Wine's OWN implementation written against ID3D10Device, and on this lane an
 * ID3D10Device is a guest proxy whose vtable is an array of x86-64 trap stubs.
 * A native ppc64 effects framework driving one would execute those bytes as
 * ppc64 on its first call.  So those exports are refused by name in
 * dlls/d3d10/d3d10.thunks and only these two are served.
 *
 * The two that are served are, in Wine's own d3d10.dll and in the real
 * runtime, thin: make a DXGI factory, take its first adapter, and hand both to
 * D3D10CoreCreateDevice.  Reproduced here with HOST-side calls so that no
 * proxy is ever minted for the factory or the adapter -- they exist for the
 * length of this function and the application never sees them, which is
 * exactly what the real runtime does too. */

/* IDXGIFactory's slot numbers, from ppc64le/dxvk/interfaces_dxvk.json rather
 * than from memory: IUnknown's three, then IDXGIObject's four (SetPrivateData,
 * SetPrivateDataInterface, GetPrivateData, GetParent), then the interface's
 * own.  They are spelled out because this is the one place in this module that
 * calls a vtable slot the marshal tables did not choose for it. */
#define DXGI_FACTORY_SLOT_ENUM_ADAPTERS    7
#define DXGI_FACTORY_SLOT_CREATE_SWAPCHAIN 10
#define IUNKNOWN_SLOT_RELEASE              2

/* D3D10_SDK_VERSION and D3D_FEATURE_LEVEL_10_0, spelled here because this
 * module includes no D3D10 header.  The SDK version is CHECKED and the feature
 * level is what D3D10CoreCreateDevice is handed -- they are different things
 * and they are the same argument position in two different functions, which is
 * a mistake worth being explicit about.  [MEASURED] passing the SDK version
 * through as the feature level produced a device anyway and DXVK logged
 * `D3D11InternalCreateDevice: Using feature level 29`; 29 is not a feature
 * level.  Wine's own dlls/d3d10/d3d10_main.c gets this right at line 144 and
 * that is where the value comes from. */
#define D3D10_SDK_VERSION_VALUE   29
#define D3D_FEATURE_LEVEL_10_0_VALUE 0xa000

static void host_release( void *host )
{
    UINT64 args[D3D11_UNIX_MAX_ARGS] = { 0 };

    if (host) unix_vtbl_call( host, IUNKNOWN_SLOT_RELEASE, 1, args );
}

/* D3D10's driver types.  Only HARDWARE (and WARP, which the real runtime and
 * Wine both fall back to hardware for) can be served here: REFERENCE, NULL and
 * SOFTWARE all want a SOFTWARE ADAPTER, made by IDXGIFactory::CreateSoftwareAdapter
 * from a rasteriser DLL, and DXVK implements neither the adapter nor the
 * rasteriser.  Refused by name rather than served with hardware, because an
 * application that asked for the reference rasteriser asked for it to compare
 * against -- silently giving it the hardware one answers the opposite of its
 * question. */
enum { D3D10_DRIVER_TYPE_HARDWARE_VALUE = 0, D3D10_DRIVER_TYPE_REFERENCE_VALUE = 1,
       D3D10_DRIVER_TYPE_NULL_VALUE = 2, D3D10_DRIVER_TYPE_SOFTWARE_VALUE = 3,
       D3D10_DRIVER_TYPE_WARP_VALUE = 5 };

static BOOL d3d10_driver_type_ok( UINT driver_type )
{
    static BOOL logged;

    if (driver_type == D3D10_DRIVER_TYPE_HARDWARE_VALUE ||
        driver_type == D3D10_DRIVER_TYPE_WARP_VALUE)
        return TRUE;

    if (!logged)
    {
        logged = TRUE;
        FIXME( "D3D10 driver type %#x asks for a software adapter -- the "
               "reference rasteriser, the null device or a caller-supplied "
               "one.  DXVK implements no software adapter and no rasteriser, "
               "and answering with the hardware device would give an "
               "application that asked to compare against the reference the "
               "opposite of what it asked for.  Refused.\n", driver_type );
    }
    return FALSE;
}

/* IID_IDXGIFactory, spelled out for the same reason this module spells out
 * DXGI_SWAP_CHAIN_DESC: it pulls in no DXGI header, and this is the one GUID it
 * needs.  Checked against include/dxgi.idl. */
static const GUID d3d11_IID_IDXGIFactory =
    { 0x7b7166ec, 0x21c7, 0x44ae, { 0xb2,0x1a,0xc9,0xae,0x32,0x1a,0xe3,0x69 } };

/* A host IDXGIFactory and its first host adapter, both unwrapped, both the
 * caller's to release.  Returns S_OK or the failing HRESULT. */
static HRESULT d3d10_host_factory( void **factory_out, void **adapter_out )
{
    UINT64 args[12] = { 0 };
    UINT64 a[D3D11_UNIX_MAX_ARGS] = { 0 };
    void *factory = NULL, *adapter = NULL;
    HRESULT hr;

    *factory_out = *adapter_out = NULL;
    args[0] = (UINT64)(ULONG_PTR)&d3d11_IID_IDXGIFactory;
    args[1] = (UINT64)(ULONG_PTR)&factory;
    if (FAILED(hr = flat_call( FLAT_CreateDXGIFactory, 2, args, NULL ))) return hr;
    if (!factory) return E_FAIL;

    a[1] = 0;                                   /* adapter index */
    a[2] = (UINT64)(ULONG_PTR)&adapter;
    hr = (HRESULT)unix_vtbl_call( factory, DXGI_FACTORY_SLOT_ENUM_ADAPTERS, 3, a );
    if (FAILED(hr) || !adapter)
    {
        host_release( factory );
        return FAILED(hr) ? hr : E_FAIL;
    }
    *factory_out = factory;
    *adapter_out = adapter;
    return S_OK;
}

HRESULT WINAPI __wine_guest_D3D10CreateDevice( void *adapter, UINT driver_type,
                                               void *software, UINT flags,
                                               UINT sdk_version, void **device )
{
    UINT64 args[12] = { 0 };
    void *factory = NULL, *host_adapter = NULL, *own_adapter = NULL, *dl = NULL;
    void **device_s;
    HRESULT hr;

    TRACE( "adapter %p, driver_type %#x, flags %#x, sdk_version %u, device %p\n",
           adapter, driver_type, flags, sdk_version, device );

    /* refusal hygiene by hand -- see hand_get_private_data */
    winecom_refused_scrub_ptr( device );
    if (!device) return E_INVALIDARG;
    if (sdk_version != D3D10_SDK_VERSION_VALUE)
    {
        WARN( "invalid SDK version %#x\n", sdk_version );
        return E_INVALIDARG;
    }
    if (!d3d10_driver_type_ok( driver_type )) return E_FAIL;
    if (!com_runtime_init()) return E_FAIL;

    if (adapter && !winecom_translate_in( adapter, &host_adapter ))
    {
        FIXME( "D3D10CreateDevice with a guest-implemented IDXGIAdapter %p; "
               "reverse proxies do not exist yet\n", adapter );
        return E_NOTIMPL;
    }
    if (FAILED(hr = d3d10_host_factory( &factory, &own_adapter ))) return hr;
    if (!host_adapter) host_adapter = own_adapter;

    device_s = stage_out( device, &dl );
    args[0] = (UINT64)(ULONG_PTR)factory;
    args[1] = (UINT64)(ULONG_PTR)host_adapter;
    args[2] = flags;
    args[3] = D3D_FEATURE_LEVEL_10_0_VALUE;
    args[4] = (UINT64)(ULONG_PTR)device_s;
    hr = flat_call( FLAT_D3D10CoreCreateDevice, 5, args, NULL );

    host_release( own_adapter );
    host_release( factory );
    if (FAILED(hr)) return hr;
    winecom_wrap_static( device_s, D3D11_IFACE_ID3D10Device );
    unstage_out( device, device_s );
    return hr;
}

HRESULT WINAPI D3D10CreateDevice( void *adapter, UINT driver_type, void *software,
                                  UINT flags, UINT sdk_version, void **device )
{
    REFUSE_NATIVE(D3D10CreateDevice);
}

HRESULT WINAPI __wine_guest_D3D10CreateDeviceAndSwapChain( void *adapter, UINT driver_type,
                                                           void *software, UINT flags,
                                                           UINT sdk_version,
                                                           void *swapchain_desc,
                                                           void **swapchain, void **device )
{
    const struct dxgi_swap_chain_desc *desc = swapchain_desc;
    struct dxgi_swap_chain_desc desc_native;
    UINT64 args[12] = { 0 };
    void *factory = NULL, *host_adapter = NULL, *own_adapter = NULL;
    void *host_swapchain = NULL, *dl = NULL, *sl = NULL;
    void **device_s, **swapchain_s;
    HRESULT hr;

    TRACE( "adapter %p, flags %#x, desc %p, swapchain %p, device %p\n",
           adapter, flags, desc, swapchain, device );

    /* refusal hygiene by hand -- see hand_get_private_data */
    winecom_refused_scrub_ptr( device );
    winecom_refused_scrub_ptr( swapchain );
    if (!device) return E_INVALIDARG;
    if (sdk_version != D3D10_SDK_VERSION_VALUE)
    {
        WARN( "invalid SDK version %#x\n", sdk_version );
        return E_INVALIDARG;
    }
    if (!d3d10_driver_type_ok( driver_type )) return E_FAIL;
    if (!com_runtime_init()) return E_FAIL;

    if (adapter && !winecom_translate_in( adapter, &host_adapter ))
    {
        FIXME( "D3D10CreateDeviceAndSwapChain with a guest-implemented "
               "IDXGIAdapter %p; reverse proxies do not exist yet\n", adapter );
        return E_NOTIMPL;
    }
    if (FAILED(hr = d3d10_host_factory( &factory, &own_adapter ))) return hr;
    if (!host_adapter) host_adapter = own_adapter;

    if (desc && winecom_guest32())
    {
        wine_repack32_DXGI_SWAP_CHAIN_DESC( &desc_native, swapchain_desc );
        desc = &desc_native;
    }
    device_s = stage_out( device, &dl );
    swapchain_s = stage_out( swapchain, &sl );
    args[0] = (UINT64)(ULONG_PTR)factory;
    args[1] = (UINT64)(ULONG_PTR)host_adapter;
    args[2] = flags;
    args[3] = D3D_FEATURE_LEVEL_10_0_VALUE;
    args[4] = (UINT64)(ULONG_PTR)device_s;
    hr = flat_call( FLAT_D3D10CoreCreateDevice, 5, args, NULL );
    if (FAILED(hr))
    {
        host_release( own_adapter );
        host_release( factory );
        return hr;
    }

    /* The swapchain, on the SAME host factory and the host device, before
     * either is wrapped: IDXGIFactory::CreateSwapChain wants an IUnknown the
     * runtime owns, and a proxy is not one.  Same window bookkeeping the
     * hand-written CreateSwapChain slot does, for the same reasons. */
    if (desc && swapchain_s)
    {
        UINT64 a[D3D11_UNIX_MAX_ARGS] = { 0 };

        push_hwnd_state( desc->OutputWindow );
        a[1] = (UINT64)(ULONG_PTR)*device_s;
        a[2] = (UINT64)(ULONG_PTR)desc;
        a[3] = (UINT64)(ULONG_PTR)swapchain_s;
        hr = (HRESULT)unix_vtbl_call( factory, DXGI_FACTORY_SLOT_CREATE_SWAPCHAIN, 4, a );
        if (SUCCEEDED(hr)) host_swapchain = *swapchain_s;
    }

    host_release( own_adapter );
    host_release( factory );
    if (FAILED(hr))
    {
        host_release( *device_s );
        *device_s = NULL;
        unstage_out( device, device_s );
        return hr;
    }
    winecom_wrap_static( device_s, D3D11_IFACE_ID3D10Device );
    unstage_out( device, device_s );
    if (host_swapchain)
    {
        winecom_wrap_static( swapchain_s, D3D11_IFACE_IDXGISwapChain );
        swapchain_remember( host_swapchain, desc->OutputWindow );
    }
    unstage_out( swapchain, swapchain_s );
    return hr;
}

HRESULT WINAPI D3D10CreateDeviceAndSwapChain( void *adapter, UINT driver_type,
                                              void *software, UINT flags,
                                              UINT sdk_version, void *swapchain_desc,
                                              void **swapchain, void **device )
{
    REFUSE_NATIVE(D3D10CreateDeviceAndSwapChain);
}

HRESULT WINAPI D3D10CoreGetVersion( void )
{
    UINT64 args[12] = { 0 };

    TRACE( "\n" );
    return flat_call( FLAT_D3D10CoreGetVersion, 0, args, NULL );
}

HRESULT WINAPI D3D10CoreRegisterLayers( void )
{
    TRACE( "\n" );
    return S_OK;
}

/* ---------------------------------------------------------------- attach */

BOOL WINAPI DllMain( HINSTANCE inst, DWORD reason, void *reserved )
{
    if (reason == DLL_PROCESS_ATTACH)
    {
        LdrDisableThreadCalloutsForDll( inst );
        if (__wine_init_unix_call()) return FALSE;
    }
    return TRUE;
}

#else  /* __powerpc64__ */

/* The i386 build: every export the .spec names still exists and still links
 * (the d3dx9 family, dxdiagn, evr and the tests import several by name), but
 * each one refuses through the same unimplemented-function exception a
 * winebuild "@ stub" raises, naming the module and entry point.  32-bit D3D
 * belongs to a future 32-bit DXVK lane; a loud, named refusal beats either a
 * missing export (silent link/load breakage elsewhere) or a build of the
 * 64-bit marshal tables whose own layout asserts refuse i386.  Signatures
 * are argument-count-faithful so the stdcall decoration and callee cleanup
 * match the .spec exactly.  See ppc64le/wow64/DESIGN.md. */

#include <stdarg.h>
#include "windef.h"
#include "winbase.h"

extern void __cdecl DECLSPEC_NORETURN __wine_spec_unimplemented_stub( const char *module,
                                                                      const char *function );

ULONG_PTR WINAPI D3D11CoreCreateDevice( ULONG_PTR a1, ULONG_PTR a2, ULONG_PTR a3, ULONG_PTR a4, ULONG_PTR a5, ULONG_PTR a6 )
{
    __wine_spec_unimplemented_stub( "d3d11.dll", "D3D11CoreCreateDevice" );
}

ULONG_PTR WINAPI D3D11CoreRegisterLayers( void )
{
    __wine_spec_unimplemented_stub( "d3d11.dll", "D3D11CoreRegisterLayers" );
}

ULONG_PTR WINAPI D3D11CreateDevice( ULONG_PTR a1, ULONG_PTR a2, ULONG_PTR a3, ULONG_PTR a4, ULONG_PTR a5, ULONG_PTR a6, ULONG_PTR a7, ULONG_PTR a8, ULONG_PTR a9, ULONG_PTR a10 )
{
    __wine_spec_unimplemented_stub( "d3d11.dll", "D3D11CreateDevice" );
}

ULONG_PTR WINAPI D3D11CreateDeviceAndSwapChain( ULONG_PTR a1, ULONG_PTR a2, ULONG_PTR a3, ULONG_PTR a4, ULONG_PTR a5, ULONG_PTR a6, ULONG_PTR a7, ULONG_PTR a8, ULONG_PTR a9, ULONG_PTR a10, ULONG_PTR a11, ULONG_PTR a12 )
{
    __wine_spec_unimplemented_stub( "d3d11.dll", "D3D11CreateDeviceAndSwapChain" );
}

ULONG_PTR WINAPI D3D11On12CreateDevice( ULONG_PTR a1, ULONG_PTR a2, ULONG_PTR a3, ULONG_PTR a4, ULONG_PTR a5, ULONG_PTR a6, ULONG_PTR a7, ULONG_PTR a8, ULONG_PTR a9, ULONG_PTR a10 )
{
    __wine_spec_unimplemented_stub( "d3d11.dll", "D3D11On12CreateDevice" );
}

ULONG_PTR WINAPI __wine_com_dispatch( ULONG_PTR a1, ULONG_PTR a2, ULONG_PTR a3 )
{
    __wine_spec_unimplemented_stub( "d3d11.dll", "__wine_com_dispatch" );
}

ULONG_PTR WINAPI __wine_com_dispatch32( ULONG_PTR a1, ULONG_PTR a2, ULONG_PTR a3 )
{
    __wine_spec_unimplemented_stub( "d3d11.dll", "__wine_com_dispatch32" );
}

ULONG_PTR WINAPI __wine_com_slot_name( ULONG_PTR a1, ULONG_PTR a2, ULONG_PTR a3, ULONG_PTR a4 )
{
    __wine_spec_unimplemented_stub( "d3d11.dll", "__wine_com_slot_name" );
}

ULONG_PTR WINAPI __wine_guest_D3D11CreateDevice( ULONG_PTR a1, ULONG_PTR a2, ULONG_PTR a3, ULONG_PTR a4, ULONG_PTR a5, ULONG_PTR a6, ULONG_PTR a7, ULONG_PTR a8, ULONG_PTR a9, ULONG_PTR a10 )
{
    __wine_spec_unimplemented_stub( "d3d11.dll", "__wine_guest_D3D11CreateDevice" );
}

ULONG_PTR WINAPI __wine_guest_D3D11CreateDeviceAndSwapChain( ULONG_PTR a1, ULONG_PTR a2, ULONG_PTR a3, ULONG_PTR a4, ULONG_PTR a5, ULONG_PTR a6, ULONG_PTR a7, ULONG_PTR a8, ULONG_PTR a9, ULONG_PTR a10, ULONG_PTR a11, ULONG_PTR a12 )
{
    __wine_spec_unimplemented_stub( "d3d11.dll", "__wine_guest_D3D11CreateDeviceAndSwapChain" );
}

ULONG_PTR WINAPI __wine_guest_D3D11CoreCreateDevice( ULONG_PTR a1, ULONG_PTR a2, ULONG_PTR a3, ULONG_PTR a4, ULONG_PTR a5, ULONG_PTR a6 )
{
    __wine_spec_unimplemented_stub( "d3d11.dll", "__wine_guest_D3D11CoreCreateDevice" );
}

ULONG_PTR WINAPI __wine_guest_D3D11On12CreateDevice( ULONG_PTR a1, ULONG_PTR a2, ULONG_PTR a3, ULONG_PTR a4, ULONG_PTR a5, ULONG_PTR a6, ULONG_PTR a7, ULONG_PTR a8, ULONG_PTR a9, ULONG_PTR a10 )
{
    __wine_spec_unimplemented_stub( "d3d11.dll", "__wine_guest_D3D11On12CreateDevice" );
}

ULONG_PTR WINAPI __wine_guest_CreateDXGIFactory( ULONG_PTR a1, ULONG_PTR a2 )
{
    __wine_spec_unimplemented_stub( "d3d11.dll", "__wine_guest_CreateDXGIFactory" );
}

ULONG_PTR WINAPI __wine_guest_CreateDXGIFactory1( ULONG_PTR a1, ULONG_PTR a2 )
{
    __wine_spec_unimplemented_stub( "d3d11.dll", "__wine_guest_CreateDXGIFactory1" );
}

ULONG_PTR WINAPI __wine_guest_CreateDXGIFactory2( ULONG_PTR a1, ULONG_PTR a2, ULONG_PTR a3 )
{
    __wine_spec_unimplemented_stub( "d3d11.dll", "__wine_guest_CreateDXGIFactory2" );
}

ULONG_PTR WINAPI __wine_guest_DXGIGetDebugInterface1( ULONG_PTR a1, ULONG_PTR a2, ULONG_PTR a3 )
{
    __wine_spec_unimplemented_stub( "d3d11.dll", "__wine_guest_DXGIGetDebugInterface1" );
}

ULONG_PTR WINAPI __wine_guest_DXGID3D10CreateDevice( ULONG_PTR a1, ULONG_PTR a2, ULONG_PTR a3, ULONG_PTR a4, ULONG_PTR a5, ULONG_PTR a6, ULONG_PTR a7 )
{
    __wine_spec_unimplemented_stub( "d3d11.dll", "__wine_guest_DXGID3D10CreateDevice" );
}

ULONG_PTR WINAPI __wine_guest_D3D10CoreCreateDevice( ULONG_PTR a1, ULONG_PTR a2, ULONG_PTR a3, ULONG_PTR a4, ULONG_PTR a5 )
{
    __wine_spec_unimplemented_stub( "d3d11.dll", "__wine_guest_D3D10CoreCreateDevice" );
}

ULONG_PTR WINAPI __wine_guest_D3D10CreateDevice( ULONG_PTR a1, ULONG_PTR a2, ULONG_PTR a3, ULONG_PTR a4, ULONG_PTR a5, ULONG_PTR a6 )
{
    __wine_spec_unimplemented_stub( "d3d11.dll", "__wine_guest_D3D10CreateDevice" );
}

ULONG_PTR WINAPI __wine_guest_D3D10CreateDeviceAndSwapChain( ULONG_PTR a1, ULONG_PTR a2, ULONG_PTR a3, ULONG_PTR a4, ULONG_PTR a5, ULONG_PTR a6, ULONG_PTR a7, ULONG_PTR a8 )
{
    __wine_spec_unimplemented_stub( "d3d11.dll", "__wine_guest_D3D10CreateDeviceAndSwapChain" );
}

ULONG_PTR WINAPI CreateDXGIFactory( ULONG_PTR a1, ULONG_PTR a2 )
{
    __wine_spec_unimplemented_stub( "d3d11.dll", "CreateDXGIFactory" );
}

ULONG_PTR WINAPI CreateDXGIFactory1( ULONG_PTR a1, ULONG_PTR a2 )
{
    __wine_spec_unimplemented_stub( "d3d11.dll", "CreateDXGIFactory1" );
}

ULONG_PTR WINAPI CreateDXGIFactory2( ULONG_PTR a1, ULONG_PTR a2, ULONG_PTR a3 )
{
    __wine_spec_unimplemented_stub( "d3d11.dll", "CreateDXGIFactory2" );
}

ULONG_PTR WINAPI DXGIGetDebugInterface1( ULONG_PTR a1, ULONG_PTR a2, ULONG_PTR a3 )
{
    __wine_spec_unimplemented_stub( "d3d11.dll", "DXGIGetDebugInterface1" );
}

ULONG_PTR WINAPI DXGIDeclareAdapterRemovalSupport( void )
{
    __wine_spec_unimplemented_stub( "d3d11.dll", "DXGIDeclareAdapterRemovalSupport" );
}

ULONG_PTR WINAPI DXGID3D10CreateDevice( ULONG_PTR a1, ULONG_PTR a2, ULONG_PTR a3, ULONG_PTR a4, ULONG_PTR a5, ULONG_PTR a6, ULONG_PTR a7 )
{
    __wine_spec_unimplemented_stub( "d3d11.dll", "DXGID3D10CreateDevice" );
}

ULONG_PTR WINAPI DXGID3D10RegisterLayers( ULONG_PTR a1, ULONG_PTR a2 )
{
    __wine_spec_unimplemented_stub( "d3d11.dll", "DXGID3D10RegisterLayers" );
}

ULONG_PTR WINAPI D3D10CoreCreateDevice( ULONG_PTR a1, ULONG_PTR a2, ULONG_PTR a3, ULONG_PTR a4, ULONG_PTR a5 )
{
    __wine_spec_unimplemented_stub( "d3d11.dll", "D3D10CoreCreateDevice" );
}

ULONG_PTR WINAPI D3D10CoreGetVersion( void )
{
    __wine_spec_unimplemented_stub( "d3d11.dll", "D3D10CoreGetVersion" );
}

ULONG_PTR WINAPI D3D10CoreRegisterLayers( void )
{
    __wine_spec_unimplemented_stub( "d3d11.dll", "D3D10CoreRegisterLayers" );
}

ULONG_PTR WINAPI D3D10CreateDevice( ULONG_PTR a1, ULONG_PTR a2, ULONG_PTR a3, ULONG_PTR a4, ULONG_PTR a5, ULONG_PTR a6 )
{
    __wine_spec_unimplemented_stub( "d3d11.dll", "D3D10CreateDevice" );
}

ULONG_PTR WINAPI D3D10CreateDeviceAndSwapChain( ULONG_PTR a1, ULONG_PTR a2, ULONG_PTR a3, ULONG_PTR a4, ULONG_PTR a5, ULONG_PTR a6, ULONG_PTR a7, ULONG_PTR a8 )
{
    __wine_spec_unimplemented_stub( "d3d11.dll", "D3D10CreateDeviceAndSwapChain" );
}

#endif  /* __powerpc64__ */
