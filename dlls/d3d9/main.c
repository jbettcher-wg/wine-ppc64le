/*
 * Native d3d9.dll -- DXVK's d3d9 behind a COM thunk, and the whole winecom
 * client for the D3D9 surface.
 *
 * This module REPLACES Wine's wined3d-backed d3d9.  It is the D3D9 sibling of
 * dlls/d3d11/main.c, which is the file to read first: the trap ABI, the
 * proxy discipline, the two-names-per-interface-bearing-export rule and the
 * reasons for all three are written out there and are not repeated here.
 *
 *   guest x86-64 PE  -->  C:\windows\sysx8664\d3d9.dll
 *      |                  (spec2thunk COM mode: pure trap surface)
 *      |  trap; ntdll's dispatcher maps RIP -> (iface, slot) and calls the
 *      |  NATIVE namesake's __wine_com_dispatch
 *      v
 *   __wine_com_dispatch( iface, slot, AMD64_CONTEXT * )       <-- THIS FILE
 *      |  = libs/winecom's dispatch loop over d3d9_marshal.h
 *      v
 *   d3d9.so (unixlib, unix.c)  -->  libdxvk_d3d9.so
 *
 * A SECOND WINECOM INSTANCE, DELIBERATELY.  dlls/d3d11 holds the only instance
 * for d3d11+dxgi+d3d10core because those three share objects.  D3D9 shares
 * none with them: no D3D9 method takes a DXGI interface and no DXGI method
 * takes a D3D9 one, and libdxvk_d3d9.so has no DT_NEEDED on libdxvk_dxgi.so.
 * So this module owns its own instance over its own roster, and the two
 * surfaces never have to recognise each other's proxies.
 *
 * WHAT IS DIFFERENT FROM D3D11, AND IT IS ONLY TWO THINGS.
 *
 *   PRESENTATION IS THE DEVICE'S.  There is no DXGI here.  The window arrives
 *   as an argument of CreateDevice and as a member of D3DPRESENT_PARAMETERS,
 *   the swapchain is implicit in the device, and Reset re-creates it -- so the
 *   win32u hooks hang off IDirect3DDevice9::Present rather than
 *   IDXGISwapChain::Present, and the window has to be remembered per DEVICE.
 *
 *   FLOATS BY VALUE IN THE HOT PATH.  IDirect3DDevice9::Clear takes the depth
 *   as a by-value float and every D3D9 title calls it every frame, so refusing
 *   it would refuse the API.  Its float is the FIFTH argument counting `this`,
 *   which MS-x64 spills to the STACK rather than putting in an XMM register --
 *   which is why it is read out of the trap CONTEXT's stack image here and not
 *   out of ctx->FltSave, the way D3D11's ClearDepthStencilView (fourth
 *   argument, XMM3) has to be.
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
#include "winerror.h"
#include "winternl.h"
#include "ntuser.h"
#include "wine/debug.h"
#include "wine/winecom.h"

#include "unixlib.h"
/* The i386 struct repacks: untyped offset-copy functions (gen_repack32.py),
 * referenced by d3d9_marshal.h's reps tables, so the order matters.  No D3D
 * header rides in with them -- see that file's banner.  Eight of D3D9's
 * ninety-five aggregates diverge between the two guest machines; the two that
 * matter most (D3DLOCKED_RECT, D3DLOCKED_BOX) carry a mapped-memory pointer
 * and are marked out_unsafe, which is why they are hand-walked below rather
 * than repacked. */
#include "d3d9_repack32.h"
#include "d3d9_marshal.h"

WINE_DEFAULT_DEBUG_CHANNEL(d3d9);

/* This module marshals; it does not implement D3D9, so it needs none of D3D9's
 * types.  Everything below is an address or a machine word.  The one exception
 * is the presentation descriptor, which has to be READ -- see the C_ASSERTs on
 * it below. */


/* ------------------------------------------------------------- unix calls */

static UINT64 unix_vtbl_call( void *host, UINT slot, UINT argc, UINT64 *args )
{
    struct d3d9_call_params p;
    NTSTATUS status;

    memcpy( p.args, args, sizeof(p.args) );
    p.args[0] = (UINT64)(ULONG_PTR)host;
    p.slot = slot;
    p.argc = argc;
    p.ret = 0;
    if ((status = D3D9_UNIX_CALL( call, &p )))
    {
        ERR( "unix call failed, status %08x\n", (UINT)status );
        return (UINT64)(UINT)E_FAIL;
    }
    return p.ret;
}

/* ------------------------------------------------- the runtime instance */

static const WCHAR *const d3d9_guest_modules[] = { L"d3d9.dll" };

static UINT64 hand_d3d9_create_device( void *host, UINT slot, AMD64_CONTEXT *ctx );
static UINT64 hand_d3d9_create_device_ex( void *host, UINT slot, AMD64_CONTEXT *ctx );
static UINT64 hand_d3d9_create_swapchain( void *host, UINT slot, AMD64_CONTEXT *ctx );
static UINT64 hand_d3d9_reset( void *host, UINT slot, AMD64_CONTEXT *ctx );
static UINT64 hand_d3d9_reset_ex( void *host, UINT slot, AMD64_CONTEXT *ctx );
static UINT64 hand_d3d9_present( void *host, UINT slot, AMD64_CONTEXT *ctx );
static UINT64 hand_d3d9_present_ex( void *host, UINT slot, AMD64_CONTEXT *ctx );
static UINT64 hand_d3d9_swapchain_present( void *host, UINT slot, AMD64_CONTEXT *ctx );
static UINT64 hand_d3d9_clear( void *host, UINT slot, AMD64_CONTEXT *ctx );
static UINT64 hand_d3d9_set_npatch_mode( void *host, UINT slot, AMD64_CONTEXT *ctx );
static UINT64 hand_d3d9_get_npatch_mode( void *host, UINT slot, AMD64_CONTEXT *ctx );

/* THE ORDER HERE IS THE ORDER IN ppc64le/dxvk/gen_winecom.py's HAND_SLOTS_D3D9,
 * and the generated header's banner prints it so the two can be diffed by eye.
 * A permutation compiles and dispatches Clear into Present. */
static const winecom_hand_fn d3d9_hand_funcs[] =
{
    hand_d3d9_create_device,
    hand_d3d9_create_device_ex,
    hand_d3d9_create_swapchain,
    hand_d3d9_reset,
    hand_d3d9_reset_ex,
    hand_d3d9_present,
    hand_d3d9_present_ex,
    hand_d3d9_swapchain_present,
    hand_d3d9_clear,
    hand_d3d9_set_npatch_mode,
    hand_d3d9_get_npatch_mode,
};

C_ASSERT( ARRAYSIZE(d3d9_hand_funcs) == D3D9_HAND_COUNT );

/* ------------------------------------------------ the 32-bit hand walkers
 *
 * WHEN A ROW NEEDS ONE.  The generated 32-bit dispatcher reconstructs a
 * stdcall frame and calls the host, and it can do that for any row whose
 * every argument shape it can derive FROM THE SIGNATURE ALONE.  A hand32 row
 * is required exactly where that derivation is impossible, which on this
 * surface is four situations and no others:
 *
 *   1. the row is HAND on the 64-bit lane.  Hand-written means the marshal
 *      table has no argument classification to generate from, so the 32-bit
 *      side refuses with "hand-written on the 64-bit lane with no 32-bit
 *      walker yet".  Every one of D3D9's nineteen hand rows is here for this
 *      reason: presentation (the window bookkeeping win32u needs) and Clear
 *      (a by-value float the unixlib's integer call form cannot express).
 *
 *   2. an OUT parameter whose struct carries a POINTER the host fills in.
 *      A native answer above 4 GiB does not fit the guest's four-byte cell,
 *      and truncating it is silent corruption -- gen_repack32 marks these
 *      out_unsafe.  D3D9 has two: D3DLOCKED_RECT and D3DLOCKED_BOX, the
 *      answers to the whole Lock family.  Serving them means handing back a
 *      BELOW-4-GIB address, i.e. the bounce dlls/d3d11/main.c built for
 *      ID3D11DeviceContext::Map.
 *
 *   3. an array of a divergent struct whose element count is not a by-value
 *      parameter, so no mechanical rep can walk it.  D3D9 has none today.
 *
 *   4. a pointee the i386 layout roster never audited.  D3D9 has HANDLE
 *      (pSharedHandle, nineteen resource-creation rows) and PALETTEENTRY
 *      (four rows).  Neither is really hand32 material -- see the note above
 *      d3d9_hand32[] -- but until the generator grows the pointer-width
 *      scalar rep they refuse.
 *
 * What is NOT a reason: a row being refused on the 64-bit lane.  Matching is
 * by slot NAME at attach, so the two lanes stay independently honest.
 *
 * The walkers read the guest's stdcall frame and leave Esp/Eip alone; the
 * dispatcher pops the frame from the row's own geometry.  See the
 * winecom_hand32_fn banner in include/wine/winecom.h. */

static UINT64 hand32_d3d9_create_device( void *host, UINT slot, I386_CONTEXT *ctx );
static UINT64 hand32_d3d9_create_device_ex( void *host, UINT slot, I386_CONTEXT *ctx );
static UINT64 hand32_d3d9_create_swapchain( void *host, UINT slot, I386_CONTEXT *ctx );
static UINT64 hand32_d3d9_reset( void *host, UINT slot, I386_CONTEXT *ctx );
static UINT64 hand32_d3d9_reset_ex( void *host, UINT slot, I386_CONTEXT *ctx );
static UINT64 hand32_d3d9_present( void *host, UINT slot, I386_CONTEXT *ctx );
static UINT64 hand32_d3d9_present_ex( void *host, UINT slot, I386_CONTEXT *ctx );
static UINT64 hand32_d3d9_swapchain_present( void *host, UINT slot, I386_CONTEXT *ctx );
static UINT64 hand32_d3d9_clear( void *host, UINT slot, I386_CONTEXT *ctx );
static UINT64 hand32_d3d9_set_npatch_mode( void *host, UINT slot, I386_CONTEXT *ctx );
static UINT64 hand32_d3d9_surface_lock_rect( void *host, UINT slot, I386_CONTEXT *ctx );
static UINT64 hand32_d3d9_texture_lock_rect( void *host, UINT slot, I386_CONTEXT *ctx );
static UINT64 hand32_d3d9_cube_lock_rect( void *host, UINT slot, I386_CONTEXT *ctx );
static UINT64 hand32_d3d9_volume_lock_box( void *host, UINT slot, I386_CONTEXT *ctx );
static UINT64 hand32_d3d9_volumetex_lock_box( void *host, UINT slot, I386_CONTEXT *ctx );
static UINT64 hand32_d3d9_buffer_lock( void *host, UINT slot, I386_CONTEXT *ctx );
static UINT64 hand32_d3d9_surface_unlock_rect( void *host, UINT slot, I386_CONTEXT *ctx );
static UINT64 hand32_d3d9_texture_unlock_rect( void *host, UINT slot, I386_CONTEXT *ctx );
static UINT64 hand32_d3d9_cube_unlock_rect( void *host, UINT slot, I386_CONTEXT *ctx );
static UINT64 hand32_d3d9_volume_unlock_box( void *host, UINT slot, I386_CONTEXT *ctx );
static UINT64 hand32_d3d9_volumetex_unlock_box( void *host, UINT slot, I386_CONTEXT *ctx );
static UINT64 hand32_d3d9_buffer_unlock( void *host, UINT slot, I386_CONTEXT *ctx );

/* Matched to rows by slot NAME at attach, so this table is not ordered
 * against anything and a missing row is a refusal rather than a
 * misdispatch.  IDirect3DDevice9::GetNPatchMode is deliberately absent:
 * i386 returns a float in x87 ST(0), the row therefore publishes no frame
 * geometry, and the dispatcher fails closed BEFORE it would consult this
 * table -- a walker here could never run.  N-patches died with D3D9's first
 * service pack and no Source title asks. */
static const struct winecom_hand32 d3d9_hand32[] =
{
    { "IDirect3D9::CreateDevice",                    hand32_d3d9_create_device },
    { "IDirect3D9Ex::CreateDeviceEx",                hand32_d3d9_create_device_ex },
    { "IDirect3DDevice9::CreateAdditionalSwapChain", hand32_d3d9_create_swapchain },
    { "IDirect3DDevice9::Reset",                     hand32_d3d9_reset },
    { "IDirect3DDevice9Ex::ResetEx",                 hand32_d3d9_reset_ex },
    { "IDirect3DDevice9::Present",                   hand32_d3d9_present },
    { "IDirect3DDevice9Ex::PresentEx",               hand32_d3d9_present_ex },
    { "IDirect3DSwapChain9::Present",                hand32_d3d9_swapchain_present },
    { "IDirect3DDevice9::Clear",                     hand32_d3d9_clear },
    { "IDirect3DDevice9::SetNPatchMode",             hand32_d3d9_set_npatch_mode },

    /* the Lock family: reason 2 above -- an OUT struct (or bare cell) the
     * host fills with a pointer that must fit four guest bytes */
    { "IDirect3DSurface9::LockRect",                 hand32_d3d9_surface_lock_rect },
    { "IDirect3DTexture9::LockRect",                 hand32_d3d9_texture_lock_rect },
    { "IDirect3DCubeTexture9::LockRect",             hand32_d3d9_cube_lock_rect },
    { "IDirect3DVolume9::LockBox",                   hand32_d3d9_volume_lock_box },
    { "IDirect3DVolumeTexture9::LockBox",            hand32_d3d9_volumetex_lock_box },
    { "IDirect3DVertexBuffer9::Lock",                hand32_d3d9_buffer_lock },
    { "IDirect3DIndexBuffer9::Lock",                 hand32_d3d9_buffer_lock },
    /* ...and the Unlocks that close them.  These are mechanically
     * marshallable -- no pointer crosses them -- and the generator served
     * them until the bounce landed.  They are hand rows purely so the
     * guest's bytes can be flushed back to the host mapping BEFORE the host
     * is told the lock is over. */
    { "IDirect3DSurface9::UnlockRect",               hand32_d3d9_surface_unlock_rect },
    { "IDirect3DTexture9::UnlockRect",               hand32_d3d9_texture_unlock_rect },
    { "IDirect3DCubeTexture9::UnlockRect",           hand32_d3d9_cube_unlock_rect },
    { "IDirect3DVolume9::UnlockBox",                 hand32_d3d9_volume_unlock_box },
    { "IDirect3DVolumeTexture9::UnlockBox",          hand32_d3d9_volumetex_unlock_box },
    { "IDirect3DVertexBuffer9::Unlock",              hand32_d3d9_buffer_unlock },
    { "IDirect3DIndexBuffer9::Unlock",               hand32_d3d9_buffer_unlock },
};

static const struct winecom_surface d3d9_surface =
{
    .name = "d3d9",
    .guest_modules = d3d9_guest_modules,
    .module_count = ARRAYSIZE(d3d9_guest_modules),
    .ifaces = d3d9_com_ifaces,
    .iface_count = D3D9_IFACE_COUNT,
    .invoke = unix_vtbl_call,
    .hand_funcs = d3d9_hand_funcs,
    .hand_count = D3D9_HAND_COUNT,
    .hand32 = d3d9_hand32,
    .hand32_count = ARRAYSIZE(d3d9_hand32),
};

static LONG com_init_state;            /* 0 = no, 1 = in progress, 2 = ok,
                                          3 = failed */

static BOOL com_runtime_init( void )
{
    LONG state;

    while ((state = InterlockedCompareExchange( &com_init_state, 1, 0 )))
    {
        if (state == 2) return TRUE;
        if (state == 3) return FALSE;
        NtYieldExecution();
    }
    if (D3D9_UNIX_CALL( init, NULL ) || !winecom_attach( &d3d9_surface ))
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

/* The crossing-frequency sink's name lookup; see winecom_slot_names.  Never on
 * a dispatch path -- ntdll asks once per slot, when it interns the row. */
BOOL WINAPI __wine_com_slot_name( UINT iface, UINT slot, const char **iface_name,
                                  const char **slot_name )
{
    return winecom_slot_names( iface, slot, iface_name, slot_name );
}

static UINT64 read_arg( const AMD64_CONTEXT *ctx, UINT n )
{
    return winecom_read_arg( ctx, n );
}

/* ======================================================================
 *                            presentation
 *
 * The same design as dlls/d3d11/main.c's, one API older.  Read that banner
 * for why an HWND may cross unconverted at all, why the DXVK side is a WSI
 * backend rather than a fork, and why these slots are hand-written rather
 * than generated.  What differs is only bookkeeping: a D3D9 DEVICE owns the
 * window, so the table below is keyed by host device (and by host swapchain
 * for the additional ones), where the D3D11 table was keyed by swapchain.
 * ====================================================================== */

/* D3DPRESENT_PARAMETERS, spelled here rather than included: this module pulls
 * in no D3D9 header, for the reason dlls/d3d11/main.c gives about whose
 * d3d11.h.  Only the window is read.  The asserts are the check that the
 * layout stays what it is -- every member is 4 bytes except the 8-byte,
 * 8-aligned HWND, which is why the guest's x86-64 layout and this ppc64le one
 * agree, and if a compiler ever disagreed the build would stop here rather
 * than hand DXVK a window handle read from the middle of `Windowed`. */
struct d3d9_present_parameters
{
    UINT  BackBufferWidth;
    UINT  BackBufferHeight;
    UINT  BackBufferFormat;             /* D3DFORMAT */
    UINT  BackBufferCount;
    UINT  MultiSampleType;              /* D3DMULTISAMPLE_TYPE */
    DWORD MultiSampleQuality;
    UINT  SwapEffect;                   /* D3DSWAPEFFECT */
    HWND  hDeviceWindow;
    BOOL  Windowed;
    BOOL  EnableAutoDepthStencil;
    UINT  AutoDepthStencilFormat;       /* D3DFORMAT */
    DWORD Flags;
    UINT  FullScreen_RefreshRateInHz;
    UINT  PresentationInterval;
};

C_ASSERT( FIELD_OFFSET(struct d3d9_present_parameters, hDeviceWindow) == 32 );
C_ASSERT( FIELD_OFFSET(struct d3d9_present_parameters, Windowed) == 40 );
C_ASSERT( sizeof(struct d3d9_present_parameters) == 64 );

struct object_window
{
    struct object_window *next;
    void *host;                 /* device or additional swapchain */
    HWND hwnd;
};

static CRITICAL_SECTION win_cs;
static CRITICAL_SECTION_DEBUG win_cs_debug =
{
    0, 0, &win_cs,
    { &win_cs_debug.ProcessLocksList, &win_cs_debug.ProcessLocksList },
      0, 0, { (DWORD_PTR)(__FILE__ ": d3d9 win_cs") }
};
static CRITICAL_SECTION win_cs = { &win_cs_debug, -1, 0, 0, 0, 0 };

static struct object_window *object_windows;

/* Which window this device or swapchain presents to.
 *
 * D3D9 lets an application pass NULL for hDeviceWindow, in which case the
 * FOCUS window is used -- so both have to be looked at, in that order, and
 * the answer recorded rather than recomputed at present time when only the
 * device is in hand. */
static HWND presentation_window( const struct d3d9_present_parameters *pp, HWND focus )
{
    if (pp && pp->hDeviceWindow) return pp->hDeviceWindow;
    return focus;
}

/* Tell the unixlib what DXVK is about to ask it about this window.  Pushed
 * rather than pulled: the unix side cannot reach user32, and the PE side is
 * holding the HWND at every point where DXVK is about to ask.  Identical in
 * shape and reason to dlls/d3d11/main.c's. */
static void push_hwnd_state( HWND hwnd )
{
    struct d3d9_hwnd_params p = { 0 };
    RECT rect = { 0 };

    if (!hwnd) return;
    p.hwnd = (UINT64)(ULONG_PTR)hwnd;
    if (NtUserIsWindow( hwnd ) && NtUserGetClientRect( hwnd, &rect, 0 ))
    {
        p.width = rect.right - rect.left;
        p.height = rect.bottom - rect.top;
        p.valid = 1;
    }
    D3D9_UNIX_CALL( hwnd, &p );
}

static void remember_window( void *host, HWND hwnd )
{
    struct object_window *s;

    if (!host || !hwnd) return;
    RtlEnterCriticalSection( &win_cs );
    for (s = object_windows; s; s = s->next)
        if (s->host == host) { s->hwnd = hwnd; break; }
    RtlLeaveCriticalSection( &win_cs );
    if (s) return;

    if (!(s = RtlAllocateHeap( NtCurrentTeb()->Peb->ProcessHeap, 0, sizeof(*s) )))
        return;
    s->host = host;
    s->hwnd = hwnd;
    RtlEnterCriticalSection( &win_cs );
    s->next = object_windows;
    object_windows = s;
    RtlLeaveCriticalSection( &win_cs );
    TRACE( "object %p presents to hwnd %p\n", host, hwnd );
}

static HWND object_hwnd( void *host )
{
    struct object_window *s;
    HWND hwnd = NULL;

    RtlEnterCriticalSection( &win_cs );
    for (s = object_windows; s; s = s->next)
        if (s->host == host) { hwnd = s->hwnd; break; }
    RtlLeaveCriticalSection( &win_cs );
    return hwnd;
}

static void present_hook( HWND hwnd, UINT phase, HRESULT hr )
{
    struct d3d9_present_params p = { 0 };

    p.hwnd = (UINT64)(ULONG_PTR)hwnd;
    p.phase = phase;
    p.result = hr;
    D3D9_UNIX_CALL( present, &p );
}

/* The two hooks win32u performs around a present, and DXVK's Present between
 * them, on the application's own thread.
 *
 * `override` is D3D9's per-present destination window override: a windowed
 * Present may be told to go somewhere other than the device's own window, and
 * when it is, that is the window whose client surface has to be updated. */
static UINT64 present_common( void *host, UINT slot, UINT argc, UINT64 *args,
                              HWND override )
{
    HWND hwnd = override ? override : object_hwnd( host );
    HRESULT hr;

    if (!hwnd)
    {
        static BOOL logged;

        if (!logged)
        {
            logged = TRUE;
            WARN( "presenting object %p, which this module never saw created -- "
                  "no window is known for it, so win32u's client surface will "
                  "not be updated around the present.\n", host );
        }
        return unix_vtbl_call( host, slot, argc, args );
    }

    push_hwnd_state( hwnd );
    present_hook( hwnd, D3D9_PRESENT_BEGIN, S_OK );
    hr = (HRESULT)unix_vtbl_call( host, slot, argc, args );
    present_hook( hwnd, D3D9_PRESENT_END, hr );
    return (UINT64)(UINT)hr;
}

/* IDirect3D9::CreateDevice( UINT adapter, D3DDEVTYPE type, HWND focus_window,
 * DWORD flags, D3DPRESENT_PARAMETERS *pp, IDirect3DDevice9 **device ). */
static UINT64 hand_d3d9_create_device( void *host, UINT slot, AMD64_CONTEXT *ctx )
{
    struct d3d9_present_parameters *pp =
        (struct d3d9_present_parameters *)(ULONG_PTR)read_arg( ctx, 5 );
    HWND focus = (HWND)(ULONG_PTR)read_arg( ctx, 3 );
    void **out = (void **)(ULONG_PTR)read_arg( ctx, 6 );
    UINT64 args[D3D9_UNIX_MAX_ARGS] = { 0 };
    HWND hwnd = presentation_window( pp, focus );
    void *host_device;
    HRESULT hr;

    if (!pp || !out) return (UINT64)(UINT)E_INVALIDARG;

    TRACE( "adapter %u, type %#x, focus %p, flags %#x, %ux%u windowed %d, "
           "device window %p\n", (UINT)read_arg( ctx, 1 ), (UINT)read_arg( ctx, 2 ),
           focus, (UINT)read_arg( ctx, 4 ), pp->BackBufferWidth,
           pp->BackBufferHeight, pp->Windowed, pp->hDeviceWindow );

    push_hwnd_state( hwnd );
    args[1] = read_arg( ctx, 1 );
    args[2] = read_arg( ctx, 2 );
    args[3] = (UINT64)(ULONG_PTR)focus;
    args[4] = read_arg( ctx, 4 );
    args[5] = (UINT64)(ULONG_PTR)pp;
    args[6] = (UINT64)(ULONG_PTR)out;
    hr = (HRESULT)unix_vtbl_call( host, slot, 7, args );
    if (FAILED(hr)) return (UINT64)(UINT)hr;

    host_device = *out;
    winecom_wrap_static( out, D3D9_IFACE_IDirect3DDevice9 );
    remember_window( host_device, hwnd );
    return (UINT64)(UINT)hr;
}

/* IDirect3D9Ex::CreateDeviceEx( UINT adapter, D3DDEVTYPE type, HWND focus,
 * DWORD flags, D3DPRESENT_PARAMETERS *pp, D3DDISPLAYMODEEX *mode,
 * IDirect3DDevice9Ex **device ). */
static UINT64 hand_d3d9_create_device_ex( void *host, UINT slot, AMD64_CONTEXT *ctx )
{
    struct d3d9_present_parameters *pp =
        (struct d3d9_present_parameters *)(ULONG_PTR)read_arg( ctx, 5 );
    HWND focus = (HWND)(ULONG_PTR)read_arg( ctx, 3 );
    void **out = (void **)(ULONG_PTR)read_arg( ctx, 7 );
    UINT64 args[D3D9_UNIX_MAX_ARGS] = { 0 };
    HWND hwnd = presentation_window( pp, focus );
    void *host_device;
    HRESULT hr;

    if (!pp || !out) return (UINT64)(UINT)E_INVALIDARG;

    push_hwnd_state( hwnd );
    args[1] = read_arg( ctx, 1 );
    args[2] = read_arg( ctx, 2 );
    args[3] = (UINT64)(ULONG_PTR)focus;
    args[4] = read_arg( ctx, 4 );
    args[5] = (UINT64)(ULONG_PTR)pp;
    args[6] = read_arg( ctx, 6 );
    args[7] = (UINT64)(ULONG_PTR)out;
    hr = (HRESULT)unix_vtbl_call( host, slot, 8, args );
    if (FAILED(hr)) return (UINT64)(UINT)hr;

    host_device = *out;
    winecom_wrap_static( out, D3D9_IFACE_IDirect3DDevice9Ex );
    remember_window( host_device, hwnd );
    return (UINT64)(UINT)hr;
}

/* IDirect3DDevice9::CreateAdditionalSwapChain( D3DPRESENT_PARAMETERS *pp,
 * IDirect3DSwapChain9 **swapchain ).  An additional swapchain has its own
 * window, which is the whole reason an application asks for one. */
static UINT64 hand_d3d9_create_swapchain( void *host, UINT slot, AMD64_CONTEXT *ctx )
{
    struct d3d9_present_parameters *pp =
        (struct d3d9_present_parameters *)(ULONG_PTR)read_arg( ctx, 1 );
    void **out = (void **)(ULONG_PTR)read_arg( ctx, 2 );
    UINT64 args[D3D9_UNIX_MAX_ARGS] = { 0 };
    void *host_swapchain;
    HWND hwnd;
    HRESULT hr;

    if (!pp || !out) return (UINT64)(UINT)E_INVALIDARG;

    /* No focus window at this level: an additional swapchain names its own
     * device window or it presents to the device's. */
    hwnd = pp->hDeviceWindow ? pp->hDeviceWindow : object_hwnd( host );
    push_hwnd_state( hwnd );
    args[1] = (UINT64)(ULONG_PTR)pp;
    args[2] = (UINT64)(ULONG_PTR)out;
    hr = (HRESULT)unix_vtbl_call( host, slot, 3, args );
    if (FAILED(hr)) return (UINT64)(UINT)hr;

    host_swapchain = *out;
    winecom_wrap_static( out, D3D9_IFACE_IDirect3DSwapChain9 );
    remember_window( host_swapchain, hwnd );
    return (UINT64)(UINT)hr;
}

/* IDirect3DDevice9::Reset( D3DPRESENT_PARAMETERS *pp ).
 *
 * Reset destroys and rebuilds the implicit swapchain, which is exactly when
 * DXVK asks for a new surface -- and it may move the device to a different
 * window while it is at it, so the recorded window is updated. */
static UINT64 hand_d3d9_reset( void *host, UINT slot, AMD64_CONTEXT *ctx )
{
    struct d3d9_present_parameters *pp =
        (struct d3d9_present_parameters *)(ULONG_PTR)read_arg( ctx, 1 );
    UINT64 args[D3D9_UNIX_MAX_ARGS] = { 0 };
    HWND hwnd = pp ? presentation_window( pp, object_hwnd( host ) ) : NULL;
    HRESULT hr;

    if (hwnd) push_hwnd_state( hwnd );
    args[1] = (UINT64)(ULONG_PTR)pp;
    hr = (HRESULT)unix_vtbl_call( host, slot, 2, args );
    if (SUCCEEDED(hr) && hwnd) remember_window( host, hwnd );
    return (UINT64)(UINT)hr;
}

/* IDirect3DDevice9Ex::ResetEx( D3DPRESENT_PARAMETERS *pp,
 * D3DDISPLAYMODEEX *mode ). */
static UINT64 hand_d3d9_reset_ex( void *host, UINT slot, AMD64_CONTEXT *ctx )
{
    struct d3d9_present_parameters *pp =
        (struct d3d9_present_parameters *)(ULONG_PTR)read_arg( ctx, 1 );
    UINT64 args[D3D9_UNIX_MAX_ARGS] = { 0 };
    HWND hwnd = pp ? presentation_window( pp, object_hwnd( host ) ) : NULL;
    HRESULT hr;

    if (hwnd) push_hwnd_state( hwnd );
    args[1] = (UINT64)(ULONG_PTR)pp;
    args[2] = read_arg( ctx, 2 );
    hr = (HRESULT)unix_vtbl_call( host, slot, 3, args );
    if (SUCCEEDED(hr) && hwnd) remember_window( host, hwnd );
    return (UINT64)(UINT)hr;
}

/* IDirect3DDevice9::Present( const RECT *src, const RECT *dst,
 * HWND dst_window_override, const RGNDATA *dirty ). */
static UINT64 hand_d3d9_present( void *host, UINT slot, AMD64_CONTEXT *ctx )
{
    UINT64 args[D3D9_UNIX_MAX_ARGS] = { 0 };
    UINT i;

    for (i = 1; i <= 4; i++) args[i] = read_arg( ctx, i );
    return present_common( host, slot, 5, args, (HWND)(ULONG_PTR)args[3] );
}

/* IDirect3DDevice9Ex::PresentEx( const RECT *, const RECT *, HWND,
 * const RGNDATA *, DWORD flags ). */
static UINT64 hand_d3d9_present_ex( void *host, UINT slot, AMD64_CONTEXT *ctx )
{
    UINT64 args[D3D9_UNIX_MAX_ARGS] = { 0 };
    UINT i;

    for (i = 1; i <= 5; i++) args[i] = read_arg( ctx, i );
    return present_common( host, slot, 6, args, (HWND)(ULONG_PTR)args[3] );
}

/* IDirect3DSwapChain9::Present( const RECT *, const RECT *, HWND,
 * const RGNDATA *, DWORD flags ). */
static UINT64 hand_d3d9_swapchain_present( void *host, UINT slot, AMD64_CONTEXT *ctx )
{
    UINT64 args[D3D9_UNIX_MAX_ARGS] = { 0 };
    UINT i;

    for (i = 1; i <= 5; i++) args[i] = read_arg( ctx, i );
    return present_common( host, slot, 6, args, (HWND)(ULONG_PTR)args[3] );
}

/* IDirect3DDevice9::Clear( DWORD count, const D3DRECT *rects, DWORD flags,
 * D3DCOLOR colour, float z, DWORD stencil ).
 *
 * `z` is the FIFTH argument counting `this`.  MS-x64 puts arguments past the
 * fourth on the stack, and a float there is four bytes in an eight-byte slot
 * -- NOT an XMM register, which is what makes this different from D3D11's
 * ClearDepthStencilView.  winecom_read_arg already reads the stack image for
 * n >= 4, so the bits arrive as an ordinary integer and are reinterpreted
 * here.  On ELFv2 the callee expects the float in f1, which the unixlib's
 * widest-integer call form never writes -- hence the separate float call. */
static UINT64 hand_d3d9_clear( void *host, UINT slot, AMD64_CONTEXT *ctx )
{
    struct d3d9_float_params p = { 0 };
    UINT64 raw = read_arg( ctx, 5 );

    p.self = (UINT64)(ULONG_PTR)host;
    p.a = read_arg( ctx, 1 );                  /* Count */
    p.b = read_arg( ctx, 2 );                  /* pRects */
    p.c = read_arg( ctx, 3 );                  /* Flags */
    p.d = read_arg( ctx, 4 );                  /* Color */
    p.f = *(const float *)&raw;                /* Z */
    p.e = read_arg( ctx, 6 );                  /* Stencil */
    p.slot = slot;
    p.shape = D3D9_FLOAT_CLEAR;
    if (D3D9_UNIX_CALL( float, &p ))
    {
        ERR( "unix float call failed\n" );
        return (UINT64)(UINT)E_FAIL;
    }
    return p.ret;
}

/* IDirect3DDevice9::SetNPatchMode( float segments ): the float is argument 1,
 * so MS-x64 put it in XMM1 and it is read from the trap CONTEXT's saved
 * register file -- the same rule dlls/ntdll/signal_ppc64.c's flat FP path
 * uses, and the same one dlls/d3d11/main.c reads XMM2 and XMM3 by. */
static UINT64 hand_d3d9_set_npatch_mode( void *host, UINT slot, AMD64_CONTEXT *ctx )
{
    struct d3d9_float_params p = { 0 };

    p.self = (UINT64)(ULONG_PTR)host;
    __wine_emu_materialize_ctx( ctx );   /* lazy-ctx contract, wine/winecom.h */
    p.f = *(const float *)&ctx->FltSave.XmmRegisters[1];
    p.slot = slot;
    p.shape = D3D9_FLOAT_SET;
    if (D3D9_UNIX_CALL( float, &p ))
    {
        ERR( "unix float call failed\n" );
        return (UINT64)(UINT)E_FAIL;
    }
    return p.ret;
}

/* IDirect3DDevice9::GetNPatchMode() -> float.  MS-x64 returns a float in XMM0,
 * not in RAX, so this writes the register the guest is about to read -- all
 * sixteen bytes of it, because stale high bytes from an earlier call are
 * visible to anything that reads it wider than it wrote. */
static UINT64 hand_d3d9_get_npatch_mode( void *host, UINT slot, AMD64_CONTEXT *ctx )
{
    struct d3d9_float_params p = { 0 };

    p.self = (UINT64)(ULONG_PTR)host;
    p.slot = slot;
    p.shape = D3D9_FLOAT_GET;
    if (D3D9_UNIX_CALL( float, &p )) ERR( "unix float call failed\n" );
    /* a write to a still-lazy FP group is IGNORED at resume: materialize
     * first (lazy-ctx contract, wine/winecom.h) */
    __wine_emu_materialize_ctx( ctx );
    memset( &ctx->FltSave.XmmRegisters[0], 0, sizeof(ctx->FltSave.XmmRegisters[0]) );
    *(float *)&ctx->FltSave.XmmRegisters[0] = p.ret_f;
    return 0;
}

/* ======================================================================
 *                     the 32-bit lane's hand walkers
 *
 * One per hand row above, minus GetNPatchMode (x87 return: no geometry, the
 * dispatcher fails closed before this table is consulted).  Each reads the
 * guest's stdcall frame -- esp[0] the return address, esp[1] `this`, then one
 * four-byte slot per parameter -- and leaves Esp/Eip alone; the pop is the
 * dispatcher's, from the row's own geometry.
 *
 * D3DPRESENT_PARAMETERS is 56 bytes on i386 and 64 here [MEASURED, clang
 * record layouts for both targets, ppc64le/dxvk/repack32_d3d9.json]: the
 * eight-byte HWND is the whole difference, and it moves Windowed and
 * everything after it.  Every walker that touches one repacks it into a
 * native temporary, and the five that can have it WRITTEN BACK repack the
 * answer out again -- CreateDevice and Reset both fill in the mode they
 * actually chose when the application passed zeroes or D3DFMT_UNKNOWN, and a
 * title that reads BackBufferWidth afterwards (Source does, to size its
 * render targets) would otherwise read its own request back.
 * ====================================================================== */

/* the guest's stdcall frame; a walker only reads it */
static const ULONG *frame32( const I386_CONTEXT *ctx )
{
    return (const ULONG *)(ULONG_PTR)ctx->Esp;
}

static UINT64 hand32_d3d9_create_device( void *host, UINT slot, I386_CONTEXT *ctx )
{
    const ULONG *esp = frame32( ctx );
    const void *pp32 = (const void *)(ULONG_PTR)esp[6];
    HWND focus = (HWND)(ULONG_PTR)esp[4];
    UINT *out = (UINT *)(ULONG_PTR)esp[7];      /* 4-byte guest cell */
    struct d3d9_present_parameters pp;
    UINT64 args[D3D9_UNIX_MAX_ARGS] = { 0 };
    void *host_device = NULL;
    HWND hwnd;
    HRESULT hr;

    if (!pp32 || !out) return (UINT64)(UINT)E_INVALIDARG;

    wine_repack32_D3DPRESENT_PARAMETERS( &pp, pp32 );
    hwnd = presentation_window( &pp, focus );

    TRACE( "adapter %u, type %#x, focus %p, flags %#x, %ux%u windowed %d, "
           "device window %p [i386]\n", (UINT)esp[2], (UINT)esp[3], focus,
           (UINT)esp[5], pp.BackBufferWidth, pp.BackBufferHeight, pp.Windowed,
           pp.hDeviceWindow );

    push_hwnd_state( hwnd );
    args[1] = esp[2];
    args[2] = esp[3];
    args[3] = (UINT64)(ULONG_PTR)focus;
    args[4] = esp[5];
    args[5] = (UINT64)(ULONG_PTR)&pp;
    args[6] = (UINT64)(ULONG_PTR)&host_device;
    hr = (HRESULT)unix_vtbl_call( host, slot, 7, args );
    wine_repack64_D3DPRESENT_PARAMETERS( (void *)pp32, &pp );
    if (FAILED(hr)) return (UINT64)(UINT)hr;

    remember_window( host_device, hwnd );
    *out = (UINT)(ULONG_PTR)winecom_wrap( host_device, D3D9_IFACE_IDirect3DDevice9 );
    return (UINT64)(UINT)hr;
}

static UINT64 hand32_d3d9_create_device_ex( void *host, UINT slot, I386_CONTEXT *ctx )
{
    const ULONG *esp = frame32( ctx );
    const void *pp32 = (const void *)(ULONG_PTR)esp[6];
    HWND focus = (HWND)(ULONG_PTR)esp[4];
    UINT *out = (UINT *)(ULONG_PTR)esp[8];
    struct d3d9_present_parameters pp;
    UINT64 args[D3D9_UNIX_MAX_ARGS] = { 0 };
    void *host_device = NULL;
    HWND hwnd;
    HRESULT hr;

    if (!pp32 || !out) return (UINT64)(UINT)E_INVALIDARG;

    wine_repack32_D3DPRESENT_PARAMETERS( &pp, pp32 );
    hwnd = presentation_window( &pp, focus );
    push_hwnd_state( hwnd );
    args[1] = esp[2];
    args[2] = esp[3];
    args[3] = (UINT64)(ULONG_PTR)focus;
    args[4] = esp[5];
    args[5] = (UINT64)(ULONG_PTR)&pp;
    /* D3DDISPLAYMODEEX does NOT diverge (no pointer members, 24 bytes on
     * both) -- the roster says identical, so it crosses raw. */
    args[6] = esp[7];
    args[7] = (UINT64)(ULONG_PTR)&host_device;
    hr = (HRESULT)unix_vtbl_call( host, slot, 8, args );
    wine_repack64_D3DPRESENT_PARAMETERS( (void *)pp32, &pp );
    if (FAILED(hr)) return (UINT64)(UINT)hr;

    remember_window( host_device, hwnd );
    *out = (UINT)(ULONG_PTR)winecom_wrap( host_device, D3D9_IFACE_IDirect3DDevice9Ex );
    return (UINT64)(UINT)hr;
}

static UINT64 hand32_d3d9_create_swapchain( void *host, UINT slot, I386_CONTEXT *ctx )
{
    const ULONG *esp = frame32( ctx );
    const void *pp32 = (const void *)(ULONG_PTR)esp[2];
    UINT *out = (UINT *)(ULONG_PTR)esp[3];
    struct d3d9_present_parameters pp;
    UINT64 args[D3D9_UNIX_MAX_ARGS] = { 0 };
    void *host_swapchain = NULL;
    HWND hwnd;
    HRESULT hr;

    if (!pp32 || !out) return (UINT64)(UINT)E_INVALIDARG;

    wine_repack32_D3DPRESENT_PARAMETERS( &pp, pp32 );
    hwnd = pp.hDeviceWindow ? pp.hDeviceWindow : object_hwnd( host );
    push_hwnd_state( hwnd );
    args[1] = (UINT64)(ULONG_PTR)&pp;
    args[2] = (UINT64)(ULONG_PTR)&host_swapchain;
    hr = (HRESULT)unix_vtbl_call( host, slot, 3, args );
    wine_repack64_D3DPRESENT_PARAMETERS( (void *)pp32, &pp );
    if (FAILED(hr)) return (UINT64)(UINT)hr;

    remember_window( host_swapchain, hwnd );
    *out = (UINT)(ULONG_PTR)winecom_wrap( host_swapchain,
                                          D3D9_IFACE_IDirect3DSwapChain9 );
    return (UINT64)(UINT)hr;
}

static UINT64 hand32_d3d9_reset( void *host, UINT slot, I386_CONTEXT *ctx )
{
    const ULONG *esp = frame32( ctx );
    const void *pp32 = (const void *)(ULONG_PTR)esp[2];
    struct d3d9_present_parameters pp;
    UINT64 args[D3D9_UNIX_MAX_ARGS] = { 0 };
    HWND hwnd = NULL;
    HRESULT hr;

    if (pp32)
    {
        wine_repack32_D3DPRESENT_PARAMETERS( &pp, pp32 );
        hwnd = presentation_window( &pp, object_hwnd( host ) );
        push_hwnd_state( hwnd );
        args[1] = (UINT64)(ULONG_PTR)&pp;
    }
    hr = (HRESULT)unix_vtbl_call( host, slot, 2, args );
    if (pp32) wine_repack64_D3DPRESENT_PARAMETERS( (void *)pp32, &pp );
    if (SUCCEEDED(hr) && hwnd) remember_window( host, hwnd );
    return (UINT64)(UINT)hr;
}

static UINT64 hand32_d3d9_reset_ex( void *host, UINT slot, I386_CONTEXT *ctx )
{
    const ULONG *esp = frame32( ctx );
    const void *pp32 = (const void *)(ULONG_PTR)esp[2];
    struct d3d9_present_parameters pp;
    UINT64 args[D3D9_UNIX_MAX_ARGS] = { 0 };
    HWND hwnd = NULL;
    HRESULT hr;

    if (pp32)
    {
        wine_repack32_D3DPRESENT_PARAMETERS( &pp, pp32 );
        hwnd = presentation_window( &pp, object_hwnd( host ) );
        push_hwnd_state( hwnd );
        args[1] = (UINT64)(ULONG_PTR)&pp;
    }
    args[2] = esp[3];                     /* D3DDISPLAYMODEEX: identical layout */
    hr = (HRESULT)unix_vtbl_call( host, slot, 3, args );
    if (pp32) wine_repack64_D3DPRESENT_PARAMETERS( (void *)pp32, &pp );
    if (SUCCEEDED(hr) && hwnd) remember_window( host, hwnd );
    return (UINT64)(UINT)hr;
}

/* Present's four (five for Ex) parameters are all plain pointers or DWORDs;
 * RECT and RGNDATA do not diverge.  Only the window bookkeeping is why these
 * are hand rows at all. */
static UINT64 hand32_d3d9_present( void *host, UINT slot, I386_CONTEXT *ctx )
{
    const ULONG *esp = frame32( ctx );
    UINT64 args[D3D9_UNIX_MAX_ARGS] = { 0 };
    UINT i;

    for (i = 1; i <= 4; i++) args[i] = esp[i + 1];
    return present_common( host, slot, 5, args, (HWND)(ULONG_PTR)args[3] );
}

static UINT64 hand32_d3d9_present_ex( void *host, UINT slot, I386_CONTEXT *ctx )
{
    const ULONG *esp = frame32( ctx );
    UINT64 args[D3D9_UNIX_MAX_ARGS] = { 0 };
    UINT i;

    for (i = 1; i <= 5; i++) args[i] = esp[i + 1];
    return present_common( host, slot, 6, args, (HWND)(ULONG_PTR)args[3] );
}

static UINT64 hand32_d3d9_swapchain_present( void *host, UINT slot, I386_CONTEXT *ctx )
{
    const ULONG *esp = frame32( ctx );
    UINT64 args[D3D9_UNIX_MAX_ARGS] = { 0 };
    UINT i;

    for (i = 1; i <= 5; i++) args[i] = esp[i + 1];
    return present_common( host, slot, 6, args, (HWND)(ULONG_PTR)args[3] );
}

/* Clear's `float z`.  On i386 stdcall a float parameter is pushed as four
 * bytes in its own slot -- no register file involved, no XMM spill, no
 * stack-vs-register split to reason about.  So this walker is SIMPLER than
 * its 64-bit twin, which has to know that MS-x64 put the fifth argument on
 * the stack; here every argument is on the stack by construction and `z` is
 * just esp[6] reinterpreted.  The unixlib call is the same typed-float one,
 * because the ELFv2 callee still wants it in f1. */
static UINT64 hand32_d3d9_clear( void *host, UINT slot, I386_CONTEXT *ctx )
{
    const ULONG *esp = frame32( ctx );
    struct d3d9_float_params p = { 0 };
    ULONG raw = esp[6];

    p.self = (UINT64)(ULONG_PTR)host;
    p.a = esp[2];                              /* Count */
    p.b = esp[3];                              /* pRects */
    p.c = esp[4];                              /* Flags */
    p.d = esp[5];                              /* Color */
    p.f = *(const float *)&raw;                /* Z */
    p.e = esp[7];                              /* Stencil */
    p.slot = slot;
    p.shape = D3D9_FLOAT_CLEAR;
    if (D3D9_UNIX_CALL( float, &p ))
    {
        ERR( "unix float call failed [i386]\n" );
        return (UINT64)(UINT)E_FAIL;
    }
    return p.ret;
}

static UINT64 hand32_d3d9_set_npatch_mode( void *host, UINT slot, I386_CONTEXT *ctx )
{
    const ULONG *esp = frame32( ctx );
    struct d3d9_float_params p = { 0 };
    ULONG raw = esp[2];

    p.self = (UINT64)(ULONG_PTR)host;
    p.f = *(const float *)&raw;
    p.slot = slot;
    p.shape = D3D9_FLOAT_SET;
    if (D3D9_UNIX_CALL( float, &p ))
    {
        ERR( "unix float call failed [i386]\n" );
        return (UINT64)(UINT)E_FAIL;
    }
    return p.ret;
}

/* ------------------------------------------------- the Lock family, i386
 *
 * Every D3D9 way of getting at pixels or vertices answers with a POINTER
 * INTO HOST MEMORY, and the guest's cell for it is four bytes.  Fourteen
 * rows: seven Locks -- three LockRect, two LockBox, the two buffer Locks --
 * and the seven Unlocks that close them.  The Unlocks are mechanically
 * marshallable and were served by the generator until now; they are hand
 * rows here only because the flush has to happen BEFORE the host sees the
 * Unlock.
 *
 * [MEASURED 2026-08-30] The pointer does not fit.  ppc64le/dxvk/probes/
 * d3d9_smoke.c built as an i386 PE and run under this port was answered
 * `IDirect3DSurface9::LockRect answered 00003FFF307FB000`: DXVK's d3d9 maps
 * its system-memory surfaces above 4 GiB on this host, so the below-4-GiB
 * BOUNCE is REQUIRED, not optional.  This is that bounce, built the way
 * dlls/d3d11/main.c builds it for ID3D11DeviceContext::Map -- a guest-legal
 * buffer, filled from the host mapping, copied back before the host Unlock,
 * cached per (object, subresource) -- and the rest of this comment is about
 * the one thing D3D9 makes harder than D3D11, which is the SIZE.
 *
 * [MEASURED 2026-08-30, after] the same probe now runs to
 * `d3d9_smoke: PASS 17/17` with no refusal in the log.  Six of those steps
 * exist to hold this code down: DXT1's block pitch (128) and its one-block
 * bottom mip (pitch 8, 1x1 texels, still one 4x4 block), a sub-rect lock
 * whose flush must leave every texel outside the rect alone, a vertex
 * buffer's OffsetToLock and its SizeToLock-of-zero, a cube face proving the
 * cache key is (face, level) and not level, a volume's SlicePitch x depth
 * and the standalone IDirect3DVolume9 view of the same storage, and an index
 * buffer.  Between them they EXECUTE all seven Lock rows and all seven
 * Unlock rows; before this, four of the seven had only ever been compiled.
 *
 * WHERE THE SIZE COMES FROM.  D3D11's Map hands back a DepthPitch that IS
 * the mapped slice's byte count, so d3d11's walker barely has to compute
 * anything.  D3D9's LockRect hands back a Pitch and nothing else, the Pitch
 * is the whole MIP LEVEL's row stride (not the locked sub-rect's), pBits is
 * offset INTO that slice when a rect was given, and for block-compressed
 * formats a row of the slice is a row of 4x4 BLOCKS -- so the row count is
 * ceil(h/4), floored at one block, not one pixel.  Guessing any part of that
 * corrupts exactly the buffers this exists to protect.
 *
 * So it is not guessed.  DXVK is the host here, and D3D9DeviceEx::LockImage
 * (dxvk-ppc64le/src/src/d3d9/d3d9_device.cpp) states its own arithmetic:
 *
 *     blockCount           = ceil(levelExtent / formatInfo->blockSize)
 *     pLockedBox->RowPitch = align(formatInfo->elementSize * blockCount.width, 4)
 *     SlicePitch           = RowPitch * blockCount.height
 *     pBits                = mapPtr + CalcImageLockOffset(...)
 *     CalcImageLockOffset  = (Front/blockD)*SlicePitch
 *                          + (Top/blockH)*RowPitch
 *                          + (Left/blockW)*elementSize
 *
 * and D3D9CommonTexture::GetMipSize says the buffer behind mapPtr is exactly
 * align(elementSize * blockCount.width, 4) * blockCount.height * blockCount.depth,
 * i.e. SlicePitch * depth.  Reproducing those four lines needs one datum per
 * format -- (blockW, blockH, elementSize) -- and d3d9_format_geoms[] below
 * carries it, derived rather than remembered (see the table's own comment).
 *
 * AND IT IS CHECKED AT RUNTIME, EVERY LOCK.  The table is used to recompute
 * align(elementSize * ceil(w/blockW), 4) and that is compared against the
 * Pitch the host just answered.  If they disagree the lock is REFUSED, by
 * name, with both numbers -- because a bounce sized by arithmetic the host
 * does not share would copy the wrong bytes back, and that is the failure
 * this whole file is built to make impossible.  A format missing from the
 * table refuses the same way.  The check is free (the host already told us
 * the Pitch) and it turns the table from something trusted into something
 * verified against the actual host on every single lock.
 *
 * ONE FORMAT PAIR IS SPECIAL AND SAYS SO.  For ATI1/ATI2 DXVK deliberately
 * LIES about the geometry -- `atiHack` in LockImage reports
 * RowPitch = align(width, 4) and SlicePitch = RowPitch * height, treating a
 * block-compressed format as if it were one byte per texel, "so we need to
 * lie here, the game is expected to use this info and do a workaround".  The
 * lied slice is larger than the real buffer, so the bounce is ALLOCATED to
 * the lied size (a guest that believes the pitch cannot run off the end of
 * our buffer) and only the REAL block-compressed slice is ever copied back
 * (nothing can run off the end of the host's).  A sub-rect ATI lock is
 * refused: under the lie the guest's offset and the host's do not name the
 * same byte, and there is no honest way to reconcile them.
 *
 * WHAT IS NOT DONE HERE, DELIBERATELY.  d3d11's walker caches the subresource
 * size and keeps a per-destination shadow so an identical WRITE_DISCARD flush
 * can be skipped; both were measured wins on a real title.  Neither is here.
 * The size is recomputed from a fresh GetDesc on every lock because the cache
 * key is a HOST POINTER, D3D9 objects are created and released far more
 * freely than D3D11 resources, and a released-then-reused address with a
 * stale extent behind it is the silent-corruption bug this file exists to
 * avoid; the per-lock GetDesc is one unix call and buys that away.  The
 * dedup shadow is a pure optimisation and is worth adding the day a title
 * measures it, not before. */

/* D3DLOCKED_RECT and D3DLOCKED_BOX, spelled here for the same reason
 * d3d9_present_parameters is: this module includes no D3D9 header.  The
 * asserts pin the NATIVE layout; the guest's is the i386 ABI's, four-byte
 * aligned with a four-byte pointer, and is written field by field below. */
struct d3d9_locked_rect
{
    INT   Pitch;
    void *pBits;
};
C_ASSERT( FIELD_OFFSET(struct d3d9_locked_rect, pBits) == 8 );
C_ASSERT( sizeof(struct d3d9_locked_rect) == 16 );

struct d3d9_locked_box
{
    INT   RowPitch;
    INT   SlicePitch;
    void *pBits;
};
C_ASSERT( FIELD_OFFSET(struct d3d9_locked_box, pBits) == 8 );
C_ASSERT( sizeof(struct d3d9_locked_box) == 16 );

/* the two D3DLOCK_ flags that change what the bounce has to copy */
#define D3D9_LOCK_READONLY 0x00000010
#define D3D9_LOCK_DISCARD  0x00002000

#define D3D9_FOURCC(a,b,c,d) \
    ((UINT)(unsigned char)(a)         | ((UINT)(unsigned char)(b) << 8) | \
    ((UINT)(unsigned char)(c) << 16)  | ((UINT)(unsigned char)(d) << 24))

#define D3D9_FMT_ATI1 D3D9_FOURCC('A','T','I','1')
#define D3D9_FMT_ATI2 D3D9_FOURCC('A','T','I','2')

/* (block width, block height, bytes per block) per D3DFORMAT.
 *
 * WHERE THESE NUMBERS CAME FROM.  Not from memory.  They were derived twice,
 * from two independent in-tree sources, and the two derivations were diffed:
 *
 *   1. Wine's own tables.  dlls/d3d9/device.c's wined3dformat_from_d3dformat()
 *      maps D3DFORMAT -> wined3d_format_id (FOURCCs pass through unchanged);
 *      dlls/wined3d/utils.c's format_block_info[] gives block geometry for
 *      every block or macropixel format and formats[]/typed_formats[] give
 *      bpp for the rest.  Composing the three yields (1,1,bpp) or the block
 *      triple for each D3DFORMAT.
 *
 *   2. DXVK's, which is what actually matters, because DXVK is the host that
 *      answers the Pitch and owns the buffer.  src/d3d9/d3d9_format.cpp maps
 *      D3D9Format -> VkFormat, whose elementSize/blockSize are DxvkFormatInfo
 *      in src/dxvk/dxvk_format.h; formats DXVK does not map fall to
 *      D3D9VkFormatTable::GetUnsupportedFormatInfo(), which states an
 *      elementSize outright (R8G8B8 3, R3G3B2 1, X4R4G4B4 2, A8R3G3B2 2,
 *      A8P8 2, P8 1, W11V11U10 4, CxV8U8 2, D16_LOCKABLE 2, D32F_LOCKABLE 4,
 *      D32_LOCKABLE 4, S8_LOCKABLE 1).
 *
 * The two agree on every format below except four, and DXVK wins all four
 * because DXVK is the host: R8G8_B8G8 and G8R8_G8B8 are (2,1,4) macropixel
 * formats under DXVK (VK_FORMAT_G8B8G8R8_422_UNORM / B8G8R8G8_422_UNORM)
 * where wined3d's formats[] carries a placeholder bpp of 1; and
 * D3DFMT_D32_LOCKABLE / D3DFMT_S8_LOCKABLE are unmapped in wined3d but named
 * with a size by DXVK.  Conversion formats do NOT change the answer: the
 * Pitch DXVK reports comes from lookupFormatInfo(formatMapping.Format), the
 * plain Vulkan format, never the conversion one -- checked one by one for the
 * mixed-signedness formats (L6V5U5 -> B5G6R5_PACK16, 2; X8L8V8U8 ->
 * B8G8R8A8_UNORM, 4; A2W10V10U10 -> A2B10G10R10_PACK32, 4).
 *
 * A format not listed here is REFUSED, not approximated.  That includes the
 * ones DXVK maps to nothing and gives no unsupported-info for -- D3DFMT_D32,
 * D15S1, D24X4S4, A1, A2B10G10R10_XR_BIAS, MULTI2_ARGB8 -- for which DXVK's
 * own elementSize is 0 and the Pitch it would answer is 0, and the
 * multi-planar YUV formats, whose plane arithmetic is not this table's shape.
 *
 * ppc64le/dxvk/derive-d3d9-block-sizes.py regenerates and diffs both
 * derivations against this table. */
struct d3d9_format_geom
{
    UINT format;
    unsigned char block_w, block_h, block_bytes;
};

static const struct d3d9_format_geom d3d9_format_geoms[] =
{
    {  20, 1, 1,  3 },  /* D3DFMT_R8G8B8            */
    {  21, 1, 1,  4 },  /* D3DFMT_A8R8G8B8          */
    {  22, 1, 1,  4 },  /* D3DFMT_X8R8G8B8          */
    {  23, 1, 1,  2 },  /* D3DFMT_R5G6B5            */
    {  24, 1, 1,  2 },  /* D3DFMT_X1R5G5B5          */
    {  25, 1, 1,  2 },  /* D3DFMT_A1R5G5B5          */
    {  26, 1, 1,  2 },  /* D3DFMT_A4R4G4B4          */
    {  27, 1, 1,  1 },  /* D3DFMT_R3G3B2            */
    {  28, 1, 1,  1 },  /* D3DFMT_A8                */
    {  29, 1, 1,  2 },  /* D3DFMT_A8R3G3B2          */
    {  30, 1, 1,  2 },  /* D3DFMT_X4R4G4B4          */
    {  31, 1, 1,  4 },  /* D3DFMT_A2B10G10R10       */
    {  32, 1, 1,  4 },  /* D3DFMT_A8B8G8R8          */
    {  33, 1, 1,  4 },  /* D3DFMT_X8B8G8R8          */
    {  34, 1, 1,  4 },  /* D3DFMT_G16R16            */
    {  35, 1, 1,  4 },  /* D3DFMT_A2R10G10B10       */
    {  36, 1, 1,  8 },  /* D3DFMT_A16B16G16R16      */
    {  40, 1, 1,  2 },  /* D3DFMT_A8P8              */
    {  41, 1, 1,  1 },  /* D3DFMT_P8                */
    {  50, 1, 1,  1 },  /* D3DFMT_L8                */
    {  51, 1, 1,  2 },  /* D3DFMT_A8L8              */
    {  52, 1, 1,  1 },  /* D3DFMT_A4L4              */
    {  60, 1, 1,  2 },  /* D3DFMT_V8U8              */
    {  61, 1, 1,  2 },  /* D3DFMT_L6V5U5            */
    {  62, 1, 1,  4 },  /* D3DFMT_X8L8V8U8          */
    {  63, 1, 1,  4 },  /* D3DFMT_Q8W8V8U8          */
    {  64, 1, 1,  4 },  /* D3DFMT_V16U16            */
    {  65, 1, 1,  4 },  /* D3DFMT_W11V11U10, D3D8 only; DXVK names it */
    {  67, 1, 1,  4 },  /* D3DFMT_A2W10V10U10       */
    {  70, 1, 1,  2 },  /* D3DFMT_D16_LOCKABLE      */
    {  75, 1, 1,  4 },  /* D3DFMT_D24S8             */
    {  77, 1, 1,  4 },  /* D3DFMT_D24X8             */
    {  80, 1, 1,  2 },  /* D3DFMT_D16               */
    {  81, 1, 1,  2 },  /* D3DFMT_L16               */
    {  82, 1, 1,  4 },  /* D3DFMT_D32F_LOCKABLE     */
    {  83, 1, 1,  4 },  /* D3DFMT_D24FS8            */
    {  84, 1, 1,  4 },  /* D3DFMT_D32_LOCKABLE      */
    {  85, 1, 1,  1 },  /* D3DFMT_S8_LOCKABLE       */
    { 101, 1, 1,  2 },  /* D3DFMT_INDEX16           */
    { 102, 1, 1,  4 },  /* D3DFMT_INDEX32           */
    { 110, 1, 1,  8 },  /* D3DFMT_Q16W16V16U16      */
    { 111, 1, 1,  2 },  /* D3DFMT_R16F              */
    { 112, 1, 1,  4 },  /* D3DFMT_G16R16F           */
    { 113, 1, 1,  8 },  /* D3DFMT_A16B16G16R16F     */
    { 114, 1, 1,  4 },  /* D3DFMT_R32F              */
    { 115, 1, 1,  8 },  /* D3DFMT_G32R32F           */
    { 116, 1, 1, 16 },  /* D3DFMT_A32B32G32R32F     */
    { 117, 1, 1,  2 },  /* D3DFMT_CxV8U8            */

    /* the FOURCCs.  4x4 blocks for the compressed ones, 2x1 macropixels for
     * the packed-YUV four, and the three depth-read driver hacks DXVK maps
     * onto real depth formats. */
    { D3D9_FOURCC('D','X','T','1'), 4, 4,  8 },
    { D3D9_FOURCC('D','X','T','2'), 4, 4, 16 },
    { D3D9_FOURCC('D','X','T','3'), 4, 4, 16 },
    { D3D9_FOURCC('D','X','T','4'), 4, 4, 16 },
    { D3D9_FOURCC('D','X','T','5'), 4, 4, 16 },
    { D3D9_FMT_ATI1,                4, 4,  8 },   /* BC4; see atiHack below */
    { D3D9_FMT_ATI2,                4, 4, 16 },   /* BC5; see atiHack below */
    { D3D9_FOURCC('U','Y','V','Y'), 2, 1,  4 },
    { D3D9_FOURCC('Y','U','Y','2'), 2, 1,  4 },
    { D3D9_FOURCC('R','G','B','G'), 2, 1,  4 },   /* D3DFMT_R8G8_B8G8 */
    { D3D9_FOURCC('G','R','G','B'), 2, 1,  4 },   /* D3DFMT_G8R8_G8B8 */
    { D3D9_FOURCC('D','F','1','6'), 1, 1,  2 },
    { D3D9_FOURCC('D','F','2','4'), 1, 1,  4 },
    { D3D9_FOURCC('I','N','T','Z'), 1, 1,  4 },
};

static BOOL d3d9_format_geometry( UINT format, UINT *bw, UINT *bh, UINT *bb )
{
    UINT i;

    for (i = 0; i < ARRAY_SIZE(d3d9_format_geoms); i++)
    {
        if (d3d9_format_geoms[i].format != format) continue;
        *bw = d3d9_format_geoms[i].block_w;
        *bh = d3d9_format_geoms[i].block_h;
        *bb = d3d9_format_geoms[i].block_bytes;
        return TRUE;
    }
    return FALSE;
}

/* ------------------------------------------------- the bounce buffers */

struct lock_bounce
{
    struct lock_bounce *next;
    void   *host;        /* HOST object pointer; with sub, the cache key */
    UINT    sub;         /* level, or face<<8|level, or 0 */
    void   *low;         /* the guest-legal buffer */
    SIZE_T  cap;
    void   *host_ptr;    /* what the live lock answered, NULL when unlocked */
    SIZE_T  size;        /* the live lock's byte count */
    BOOL    flush;       /* copy back at Unlock (i.e. not READONLY) */
};

static CRITICAL_SECTION lock_cs;
static CRITICAL_SECTION_DEBUG lock_cs_debug =
{
    0, 0, &lock_cs,
    { &lock_cs_debug.ProcessLocksList, &lock_cs_debug.ProcessLocksList },
      0, 0, { (DWORD_PTR)(__FILE__ ": d3d9 lock_cs") }
};
static CRITICAL_SECTION lock_cs = { &lock_cs_debug, -1, 0, 0, 0, 0 };
static struct lock_bounce *lock_bounces;

/* Below-4-GiB address space is a FINITE resource shared with everything else
 * the guest owns, and a title that locks thousands of distinct mip levels
 * over a session would retain all of them if the cache never gave anything
 * back.  So the retained total is capped, and a lock that would push past the
 * cap first releases the buffers of entries that are not currently locked.
 * The entries themselves stay -- they are a pointer and two counts each --
 * so the cache is a cache of ALLOCATIONS, not of geometry (there is no
 * cached geometry; see the section banner). */
#define LOCK_BOUNCE_BUDGET ((SIZE_T)64 * 1024 * 1024)
static SIZE_T lock_retained;

/* caller holds lock_cs */
static void lock_bounce_release( struct lock_bounce *b )
{
    SIZE_T zero = 0;
    void *low = b->low;

    if (!low) return;
    NtFreeVirtualMemory( NtCurrentProcess(), &low, &zero, MEM_RELEASE );
    lock_retained -= b->cap;
    b->low = NULL;
    b->cap = 0;
}

/* caller holds lock_cs */
static void lock_bounce_trim( const struct lock_bounce *keep, SIZE_T need )
{
    struct lock_bounce *b;

    for (b = lock_bounces; b; b = b->next)
    {
        if (lock_retained + need <= LOCK_BOUNCE_BUDGET) return;
        if (b == keep || !b->low || b->host_ptr) continue;
        lock_bounce_release( b );
    }
}

/* The byte counts for one lock.  `size` is what is reachable from the pBits
 * the host answered -- everything from there to the end of the host's own
 * mip slice, which is the most the guest can legally touch and never one
 * byte more than the host allocated.  `alloc` is what the guest could
 * ADDRESS through the pitch it was handed, which differs from `size` only
 * under DXVK's ATI lie.
 *
 * -> FALSE is a REFUSAL, and every one of them logs which disagreement it
 * was.  Nothing here approximates. */
static BOOL lock_span( const char *what, UINT format, UINT width, UINT height,
                       UINT depth, INT pitch, INT slice_pitch, BOOL have_slice,
                       const UINT *origin, SIZE_T *size, SIZE_T *alloc )
{
    UINT bw, bh, bb, cols, rows, row_pitch;
    SIZE_T slice, total, off;

    if (pitch <= 0 || !width || !height || !depth)
    {
        ERR( "%s: host answered pitch %d for a %ux%ux%u level; refusing.\n",
             what, pitch, width, height, depth );
        return FALSE;
    }
    if (!d3d9_format_geometry( format, &bw, &bh, &bb ))
    {
        ERR( "%s: D3DFORMAT %#x is not in this module's block table, so the "
             "lock cannot be sized.  Refusing rather than guessing -- see the "
             "table's comment for how to add it and where the numbers come "
             "from.\n", what, format );
        return FALSE;
    }

    cols = (width  + bw - 1) / bw;
    rows = (height + bh - 1) / bh;
    row_pitch = (bb * cols + 3) & ~3u;

    if (format == D3D9_FMT_ATI1 || format == D3D9_FMT_ATI2)
    {
        /* DXVK's atiHack: RowPitch = align(width,4), SlicePitch = that times
         * height, as if the format were one byte per texel.  Allocate to the
         * lie so a guest that believes the pitch stays inside our buffer;
         * copy only the real block-compressed slice so nothing ever runs off
         * the end of the host's. */
        UINT lied_pitch = (width + 3) & ~3u;
        SIZE_T lied = (SIZE_T)lied_pitch * height * depth;

        if ((UINT)pitch != lied_pitch)
        {
            ERR( "%s: ATI1/ATI2 lock answered pitch %d, but DXVK's atiHack "
                 "reports align(%u, 4) = %u.  Refusing.\n",
                 what, pitch, width, lied_pitch );
            return FALSE;
        }
        if (origin && (origin[0] || origin[1] || origin[2]))
        {
            ERR( "%s: a sub-rect lock of an ATI1/ATI2 surface.  DXVK reports a "
                 "pitch geometry for these that does not describe the buffer it "
                 "hands back, so the guest's byte offset and the host's do not "
                 "name the same byte and the bounce cannot be reconciled.  "
                 "Refusing; a full-level lock of the same surface is served.\n",
                 what );
            return FALSE;
        }
        total = (SIZE_T)row_pitch * rows * depth;
        *size = total;
        *alloc = lied > total ? lied : total;
        return TRUE;
    }

    if ((UINT)pitch != row_pitch)
    {
        ERR( "%s: host answered pitch %d for a %ux%u D3DFORMAT %#x, but this "
             "module's block table computes align(%u * ceil(%u/%u), 4) = %u.  "
             "REFUSING: a bounce sized by arithmetic the host does not share "
             "would copy the wrong bytes back, which is the exact failure this "
             "walker exists to prevent.  The table is wrong for this format, "
             "or the host changed its layout.\n",
             what, pitch, width, height, format, bb, width, bw, row_pitch );
        return FALSE;
    }

    slice = (SIZE_T)row_pitch * rows;
    if (have_slice && (SIZE_T)(UINT)slice_pitch != slice)
    {
        ERR( "%s: host answered slice pitch %d, this module computes %Iu "
             "(pitch %u x %u block rows).  Refusing.\n",
             what, slice_pitch, slice, row_pitch, rows );
        return FALSE;
    }
    total = slice * depth;

    off = 0;
    if (origin)
        off = (SIZE_T)origin[2] * slice
            + (SIZE_T)(origin[1] / bh) * row_pitch
            + (SIZE_T)(origin[0] / bw) * bb;
    if (off >= total)
    {
        ERR( "%s: lock origin (%u,%u,%u) lands at byte %Iu of a %Iu-byte "
             "level.  Refusing.\n", what, origin[0], origin[1], origin[2],
             off, total );
        return FALSE;
    }
    *size = total - off;
    *alloc = *size;
    return TRUE;
}

/* Install the bounce and swap the guest-legal address in.  `full` says the
 * lock covers the whole subresource, which is the only case in which a
 * DISCARD may skip the fill-in: the flush at Unlock copies the WHOLE span
 * back, so a partial lock whose bounce started uninitialised would write
 * this module's garbage over texels the guest never touched.  (DXVK's
 * LockImage clears D3DLOCK_DISCARD for partial locks for the same reason.) */
static HRESULT lock_bounce_apply( void *host, UINT sub, const char *what,
                                  void **bits, SIZE_T size, SIZE_T alloc,
                                  DWORD flags, BOOL full )
{
    struct lock_bounce *b;
    HRESULT hr = S_OK;

    RtlEnterCriticalSection( &lock_cs );
    for (b = lock_bounces; b; b = b->next)
        if (b->host == host && b->sub == sub) break;
    if (!b)
    {
        if (!(b = RtlAllocateHeap( NtCurrentTeb()->Peb->ProcessHeap,
                                   HEAP_ZERO_MEMORY, sizeof(*b) )))
        {
            RtlLeaveCriticalSection( &lock_cs );
            return E_OUTOFMEMORY;
        }
        b->host = host;
        b->sub = sub;
        b->next = lock_bounces;
        lock_bounces = b;
    }
    if (b->cap < alloc)
    {
        SIZE_T cap = (alloc + 0xffff) & ~(SIZE_T)0xffff;
        void *mem = NULL;

        lock_bounce_release( b );
        lock_bounce_trim( b, cap );
        /* zero_bits 0x7fffffff: the allocation must land below 2 GiB, which
         * is a guest-legal 4-byte address with room to spare.  Same call
         * dlls/d3d11/main.c's Map bounce makes. */
        if (!NtAllocateVirtualMemory( NtCurrentProcess(), &mem, 0x7fffffff, &cap,
                                      MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE ))
        {
            b->low = mem;
            b->cap = cap;
            lock_retained += cap;
        }
    }
    if (!b->low)
    {
        ERR( "%s: no guest-legal bounce for a %Iu-byte lock.\n", what, alloc );
        hr = E_OUTOFMEMORY;
    }
    else
    {
        b->host_ptr = *bits;
        b->size = size;
        b->flush = !(flags & D3D9_LOCK_READONLY);
        if (!full || !(flags & D3D9_LOCK_DISCARD)) memcpy( b->low, *bits, size );
        TRACE( "%s: BOUNCED %p -> %p (%Iu bytes of %Iu, flags %#x)\n",
               what, *bits, b->low, size, b->cap, (UINT)flags );
        *bits = b->low;
    }
    RtlLeaveCriticalSection( &lock_cs );
    return hr;
}

/* everything the shared serve path needs from a walker */
struct lock_serve_params
{
    void       *host;
    const char *what;
    UINT        sub;
    UINT        desc_slot;      /* GetDesc, or GetLevelDesc */
    UINT        desc_argc;      /* 2, or 3 when it takes a level */
    UINT64      desc_level;
    BOOL        is_volume;      /* D3DVOLUME_DESC rather than D3DSURFACE_DESC */
    UINT        unlock_slot;
    UINT        unlock_argc;
    UINT64      unlock_args[D3D9_UNIX_MAX_ARGS];
    DWORD       flags;
    const UINT *origin;         /* {left, top, front} in texels; NULL = full */
    INT         pitch;
    INT         slice_pitch;
    BOOL        have_slice;
    void      **bits;
};

/* GetDesc / GetLevelDesc.  D3DSURFACE_DESC is Format, Type, Usage, Pool,
 * MultiSampleType, MultiSampleQuality, Width, Height; D3DVOLUME_DESC is
 * Format, Type, Usage, Pool, Width, Height, Depth -- both DWORD-only and both
 * listed identical 32/64 in ppc64le/dxvk/repack32_d3d9.json, so the native
 * layout read here is the layout.  A texture's GetLevelDesc already reports
 * the LEVEL's extent, so nothing is shifted by the mip index here. */
static BOOL lock_desc( const struct lock_serve_params *s, UINT *format,
                       UINT *w, UINT *h, UINT *d )
{
    UINT64 args[D3D9_UNIX_MAX_ARGS] = { 0 };
    UINT desc[8] = { 0 };

    if (s->desc_argc == 3) args[1] = s->desc_level;
    args[s->desc_argc - 1] = (UINT64)(ULONG_PTR)desc;
    if (FAILED((HRESULT)unix_vtbl_call( s->host, s->desc_slot, s->desc_argc, args )))
    {
        ERR( "%s: the object refused its own description; cannot size the "
             "lock.\n", s->what );
        return FALSE;
    }
    *format = desc[0];
    *w = s->is_volume ? desc[4] : desc[6];
    *h = s->is_volume ? desc[5] : desc[7];
    *d = s->is_volume ? desc[6] : 1;
    return TRUE;
}

/* the shared tail of the five rect/box walkers.  On refusal it UNLOCKS again
 * -- a lock left open would fail the guest's next one with INVALIDCALL and
 * blame the wrong call. */
static HRESULT lock_serve( struct lock_serve_params *s )
{
    UINT format, w, h, d;
    SIZE_T size, alloc;
    HRESULT hr;

    if (!*s->bits || (ULONG_PTR)*s->bits < 0x100000000ull)
    {
        /* already guest-legal: no bounce, no copies */
        TRACE( "%s: host answered %p, guest-legal.\n", s->what, *s->bits );
        return S_OK;
    }
    if (!lock_desc( s, &format, &w, &h, &d )
        || !lock_span( s->what, format, w, h, d, s->pitch, s->slice_pitch,
                       s->have_slice, s->origin, &size, &alloc ))
        hr = E_NOTIMPL;
    else
        hr = lock_bounce_apply( s->host, s->sub, s->what, s->bits,
                                size, alloc, s->flags, !s->origin );

    if (FAILED(hr))
        unix_vtbl_call( s->host, s->unlock_slot, s->unlock_argc, s->unlock_args );
    return hr;
}

/* a guest RECT is four LONGs and a D3DBOX six UINTs, both identical 32/64;
 * only the near corner enters the offset arithmetic. */
static BOOL lock_origin_from_rect( const LONG *rect, UINT *origin )
{
    if (!rect) return FALSE;
    /* a negative corner is not a legal RECT; clamped to zero rather than
     * refused, because the caller has already told us the lock is PARTIAL and
     * a zero origin only ever widens the span this module fills and flushes,
     * which is the safe direction. */
    origin[0] = rect[0] > 0 ? (UINT)rect[0] : 0;
    origin[1] = rect[1] > 0 ? (UINT)rect[1] : 0;
    origin[2] = 0;
    return TRUE;
}

static BOOL lock_origin_from_box( const UINT *box, UINT *origin )
{
    if (!box) return FALSE;
    origin[0] = box[0];
    origin[1] = box[1];
    origin[2] = box[4];
    return TRUE;
}

/* the shared tail of the seven Unlocks: flush, then let the host have it. */
static UINT64 unlock_bounce( void *host, UINT sub, UINT slot, UINT argc, UINT64 *args )
{
    struct lock_bounce *b;

    RtlEnterCriticalSection( &lock_cs );
    for (b = lock_bounces; b; b = b->next)
        if (b->host == host && b->sub == sub) break;
    if (b && b->host_ptr)
    {
        if (b->flush) memcpy( b->host_ptr, b->low, b->size );
        b->host_ptr = NULL;
    }
    RtlLeaveCriticalSection( &lock_cs );
    return unix_vtbl_call( host, slot, argc, args );
}

/* ------------------------------------------------- the Lock walkers
 *
 * The neighbouring slots each walker reaches for -- GetDesc/GetLevelDesc and
 * the Unlock -- are fixed offsets from the Lock slot on every one of these
 * interfaces, checked against ppc64le/dxvk/interfaces_d3d9.json:
 *
 *   IDirect3DSurface9        GetDesc 12  LockRect 13  UnlockRect 14
 *   IDirect3DTexture9        GetLevelDesc 17  LockRect 19  UnlockRect 20
 *   IDirect3DCubeTexture9    GetLevelDesc 17  LockRect 19  UnlockRect 20
 *   IDirect3DVolume9         GetDesc 8   LockBox 9    UnlockBox 10
 *   IDirect3DVolumeTexture9  GetLevelDesc 17  LockBox 19   UnlockBox 20
 *   IDirect3D{Vertex,Index}Buffer9  Lock 11  Unlock 12  GetDesc 13
 */

/* IDirect3DSurface9::LockRect( D3DLOCKED_RECT *, const RECT *, DWORD ) */
static UINT64 hand32_d3d9_surface_lock_rect( void *host, UINT slot, I386_CONTEXT *ctx )
{
    const ULONG *esp = frame32( ctx );
    UINT *out = (UINT *)(ULONG_PTR)esp[2];       /* {INT Pitch; ptr32 pBits} */
    struct d3d9_locked_rect lr = { 0 };
    UINT64 args[D3D9_UNIX_MAX_ARGS] = { 0 };
    struct lock_serve_params s = { 0 };
    UINT origin[3];
    HRESULT hr;

    if (!out) return (UINT64)(UINT)E_INVALIDARG;
    args[1] = (UINT64)(ULONG_PTR)&lr;
    args[2] = esp[3];
    args[3] = esp[4];
    hr = (HRESULT)unix_vtbl_call( host, slot, 4, args );
    if (FAILED(hr)) return (UINT64)(UINT)hr;

    s.host = host;
    s.what = "IDirect3DSurface9::LockRect";
    s.sub = 0;
    s.desc_slot = slot - 1;
    s.desc_argc = 2;
    s.unlock_slot = slot + 1;
    s.unlock_argc = 1;
    s.flags = esp[4];
    s.origin = lock_origin_from_rect( (const LONG *)(ULONG_PTR)esp[3], origin ) ? origin : NULL;
    s.pitch = lr.Pitch;
    s.bits = &lr.pBits;
    /* REFUSAL HYGIENE, BY HAND, because no generated scrub mask reaches a
     * WINECOM_F_HAND row: a hand walker that refuses owns its out-params the
     * way scrub_refused_outs() owns a table refusal's.  lock_serve() refuses
     * when it cannot size the mapping, and the guest's D3DLOCKED_RECT is then
     * whatever its stack held -- read as pBits and dereferenced.  [MEASURED]
     * dlls/combase/syscom.c's IMMDevice::Activate is the same hole one API
     * over: the Witcher 3 read a never-written *ppv and the emulator decoded
     * a host module's ppc64le bytes as x86.  The write goes through
     * winecom_refused_scrub_mem, which honours WINEEMUNOREFUSESCRUB so the
     * hygiene gate can prove it load-bearing.  A NATIVE failure above stays
     * untouched -- real D3D9 leaves the struct alone on failure too. */
    if (FAILED(hr = lock_serve( &s )))
    {
        winecom_refused_scrub_mem( out, 2 * sizeof(UINT) );
        return (UINT64)(UINT)hr;
    }

    out[0] = (UINT)lr.Pitch;
    out[1] = (UINT)(ULONG_PTR)lr.pBits;
    return (UINT64)(UINT)hr;
}

static UINT64 hand32_d3d9_surface_unlock_rect( void *host, UINT slot, I386_CONTEXT *ctx )
{
    UINT64 args[D3D9_UNIX_MAX_ARGS] = { 0 };

    return unlock_bounce( host, 0, slot, 1, args );
}

/* IDirect3DTexture9::LockRect( UINT Level, D3DLOCKED_RECT *, const RECT *,
 * DWORD ).  UnlockRect takes the level back. */
static UINT64 hand32_d3d9_texture_lock_rect( void *host, UINT slot, I386_CONTEXT *ctx )
{
    const ULONG *esp = frame32( ctx );
    UINT *out = (UINT *)(ULONG_PTR)esp[3];
    struct d3d9_locked_rect lr = { 0 };
    UINT64 args[D3D9_UNIX_MAX_ARGS] = { 0 };
    struct lock_serve_params s = { 0 };
    UINT origin[3];
    HRESULT hr;

    if (!out) return (UINT64)(UINT)E_INVALIDARG;
    args[1] = esp[2];
    args[2] = (UINT64)(ULONG_PTR)&lr;
    args[3] = esp[4];
    args[4] = esp[5];
    hr = (HRESULT)unix_vtbl_call( host, slot, 5, args );
    if (FAILED(hr)) return (UINT64)(UINT)hr;

    s.host = host;
    s.what = "IDirect3DTexture9::LockRect";
    s.sub = esp[2];
    s.desc_slot = slot - 2;              /* GetLevelDesc */
    s.desc_argc = 3;
    s.desc_level = esp[2];
    s.unlock_slot = slot + 1;
    s.unlock_argc = 2;
    s.unlock_args[1] = esp[2];
    s.flags = esp[5];
    s.origin = lock_origin_from_rect( (const LONG *)(ULONG_PTR)esp[4], origin ) ? origin : NULL;
    s.pitch = lr.Pitch;
    s.bits = &lr.pBits;
    /* refusal hygiene by hand -- see hand32_d3d9_surface_lock_rect */
    if (FAILED(hr = lock_serve( &s )))
    {
        winecom_refused_scrub_mem( out, 2 * sizeof(UINT) );
        return (UINT64)(UINT)hr;
    }

    out[0] = (UINT)lr.Pitch;
    out[1] = (UINT)(ULONG_PTR)lr.pBits;
    return (UINT64)(UINT)hr;
}

static UINT64 hand32_d3d9_texture_unlock_rect( void *host, UINT slot, I386_CONTEXT *ctx )
{
    const ULONG *esp = frame32( ctx );
    UINT64 args[D3D9_UNIX_MAX_ARGS] = { 0 };

    args[1] = esp[2];
    return unlock_bounce( host, esp[2], slot, 2, args );
}

/* IDirect3DCubeTexture9::LockRect( D3DCUBEMAP_FACES, UINT Level,
 * D3DLOCKED_RECT *, const RECT *, DWORD ) */
static UINT64 hand32_d3d9_cube_lock_rect( void *host, UINT slot, I386_CONTEXT *ctx )
{
    const ULONG *esp = frame32( ctx );
    UINT *out = (UINT *)(ULONG_PTR)esp[4];
    struct d3d9_locked_rect lr = { 0 };
    UINT64 args[D3D9_UNIX_MAX_ARGS] = { 0 };
    struct lock_serve_params s = { 0 };
    UINT origin[3];
    HRESULT hr;

    if (!out) return (UINT64)(UINT)E_INVALIDARG;
    args[1] = esp[2];
    args[2] = esp[3];
    args[3] = (UINT64)(ULONG_PTR)&lr;
    args[4] = esp[5];
    args[5] = esp[6];
    hr = (HRESULT)unix_vtbl_call( host, slot, 6, args );
    if (FAILED(hr)) return (UINT64)(UINT)hr;

    s.host = host;
    s.what = "IDirect3DCubeTexture9::LockRect";
    s.sub = (esp[2] << 8) | (esp[3] & 0xff);   /* face, level */
    s.desc_slot = slot - 2;              /* GetLevelDesc( Level, desc ) */
    s.desc_argc = 3;
    s.desc_level = esp[3];
    s.unlock_slot = slot + 1;
    s.unlock_argc = 3;
    s.unlock_args[1] = esp[2];
    s.unlock_args[2] = esp[3];
    s.flags = esp[6];
    s.origin = lock_origin_from_rect( (const LONG *)(ULONG_PTR)esp[5], origin ) ? origin : NULL;
    s.pitch = lr.Pitch;
    s.bits = &lr.pBits;
    /* refusal hygiene by hand -- see hand32_d3d9_surface_lock_rect */
    if (FAILED(hr = lock_serve( &s )))
    {
        winecom_refused_scrub_mem( out, 2 * sizeof(UINT) );
        return (UINT64)(UINT)hr;
    }

    out[0] = (UINT)lr.Pitch;
    out[1] = (UINT)(ULONG_PTR)lr.pBits;
    return (UINT64)(UINT)hr;
}

static UINT64 hand32_d3d9_cube_unlock_rect( void *host, UINT slot, I386_CONTEXT *ctx )
{
    const ULONG *esp = frame32( ctx );
    UINT64 args[D3D9_UNIX_MAX_ARGS] = { 0 };

    args[1] = esp[2];
    args[2] = esp[3];
    return unlock_bounce( host, (esp[2] << 8) | (esp[3] & 0xff), slot, 3, args );
}

/* IDirect3DVolume9::LockBox( D3DLOCKED_BOX *, const D3DBOX *, DWORD ) */
static UINT64 hand32_d3d9_volume_lock_box( void *host, UINT slot, I386_CONTEXT *ctx )
{
    const ULONG *esp = frame32( ctx );
    UINT *out = (UINT *)(ULONG_PTR)esp[2];  /* {RowPitch, SlicePitch, ptr32} */
    struct d3d9_locked_box lb = { 0 };
    UINT64 args[D3D9_UNIX_MAX_ARGS] = { 0 };
    struct lock_serve_params s = { 0 };
    UINT origin[3];
    HRESULT hr;

    if (!out) return (UINT64)(UINT)E_INVALIDARG;
    args[1] = (UINT64)(ULONG_PTR)&lb;
    args[2] = esp[3];
    args[3] = esp[4];
    hr = (HRESULT)unix_vtbl_call( host, slot, 4, args );
    if (FAILED(hr)) return (UINT64)(UINT)hr;

    s.host = host;
    s.what = "IDirect3DVolume9::LockBox";
    s.sub = 0;
    s.desc_slot = slot - 1;
    s.desc_argc = 2;
    s.is_volume = TRUE;
    s.unlock_slot = slot + 1;
    s.unlock_argc = 1;
    s.flags = esp[4];
    s.origin = lock_origin_from_box( (const UINT *)(ULONG_PTR)esp[3], origin ) ? origin : NULL;
    s.pitch = lb.RowPitch;
    s.slice_pitch = lb.SlicePitch;
    s.have_slice = TRUE;
    s.bits = &lb.pBits;
    /* refusal hygiene by hand -- see hand32_d3d9_surface_lock_rect */
    if (FAILED(hr = lock_serve( &s )))
    {
        winecom_refused_scrub_mem( out, 3 * sizeof(UINT) );
        return (UINT64)(UINT)hr;
    }

    out[0] = (UINT)lb.RowPitch;
    out[1] = (UINT)lb.SlicePitch;
    out[2] = (UINT)(ULONG_PTR)lb.pBits;
    return (UINT64)(UINT)hr;
}

static UINT64 hand32_d3d9_volume_unlock_box( void *host, UINT slot, I386_CONTEXT *ctx )
{
    UINT64 args[D3D9_UNIX_MAX_ARGS] = { 0 };

    return unlock_bounce( host, 0, slot, 1, args );
}

/* IDirect3DVolumeTexture9::LockBox( UINT Level, D3DLOCKED_BOX *,
 * const D3DBOX *, DWORD ) */
static UINT64 hand32_d3d9_volumetex_lock_box( void *host, UINT slot, I386_CONTEXT *ctx )
{
    const ULONG *esp = frame32( ctx );
    UINT *out = (UINT *)(ULONG_PTR)esp[3];
    struct d3d9_locked_box lb = { 0 };
    UINT64 args[D3D9_UNIX_MAX_ARGS] = { 0 };
    struct lock_serve_params s = { 0 };
    UINT origin[3];
    HRESULT hr;

    if (!out) return (UINT64)(UINT)E_INVALIDARG;
    args[1] = esp[2];
    args[2] = (UINT64)(ULONG_PTR)&lb;
    args[3] = esp[4];
    args[4] = esp[5];
    hr = (HRESULT)unix_vtbl_call( host, slot, 5, args );
    if (FAILED(hr)) return (UINT64)(UINT)hr;

    s.host = host;
    s.what = "IDirect3DVolumeTexture9::LockBox";
    s.sub = esp[2];
    s.desc_slot = slot - 2;              /* GetLevelDesc */
    s.desc_argc = 3;
    s.desc_level = esp[2];
    s.is_volume = TRUE;
    s.unlock_slot = slot + 1;
    s.unlock_argc = 2;
    s.unlock_args[1] = esp[2];
    s.flags = esp[5];
    s.origin = lock_origin_from_box( (const UINT *)(ULONG_PTR)esp[4], origin ) ? origin : NULL;
    s.pitch = lb.RowPitch;
    s.slice_pitch = lb.SlicePitch;
    s.have_slice = TRUE;
    s.bits = &lb.pBits;
    /* refusal hygiene by hand -- see hand32_d3d9_surface_lock_rect */
    if (FAILED(hr = lock_serve( &s )))
    {
        winecom_refused_scrub_mem( out, 3 * sizeof(UINT) );
        return (UINT64)(UINT)hr;
    }

    out[0] = (UINT)lb.RowPitch;
    out[1] = (UINT)lb.SlicePitch;
    out[2] = (UINT)(ULONG_PTR)lb.pBits;
    return (UINT64)(UINT)hr;
}

static UINT64 hand32_d3d9_volumetex_unlock_box( void *host, UINT slot, I386_CONTEXT *ctx )
{
    const ULONG *esp = frame32( ctx );
    UINT64 args[D3D9_UNIX_MAX_ARGS] = { 0 };

    args[1] = esp[2];
    return unlock_bounce( host, esp[2], slot, 2, args );
}

/* IDirect3DVertexBuffer9::Lock / IDirect3DIndexBuffer9::Lock
 * ( UINT OffsetToLock, UINT SizeToLock, void **ppbData, DWORD Flags ).
 *
 * No block arithmetic here: a buffer's mapping is desc.Size bytes and DXVK's
 * LockBuffer does nothing to it but `data += OffsetToLock` -- "the offset/size
 * is not clamped to or affected by the desc size", so the reachable span is
 * desc.Size - OffsetToLock and a SizeToLock of 0 means all of it.  D3DVERTEX-
 * BUFFER_DESC and D3DINDEXBUFFER_DESC both put Size at DWORD index 4 and both
 * are listed identical 32/64 in repack32_d3d9.json.  GetDesc is asked only
 * when it is needed -- a SizeToLock the guest named is the answer, and the
 * dynamic-buffer path that runs every frame names one. */
static UINT64 hand32_d3d9_buffer_lock( void *host, UINT slot, I386_CONTEXT *ctx )
{
    const ULONG *esp = frame32( ctx );
    UINT *out = (UINT *)(ULONG_PTR)esp[4];
    UINT offset = esp[2], want = esp[3];
    DWORD flags = esp[5];
    UINT64 args[D3D9_UNIX_MAX_ARGS] = { 0 };
    void *bits = NULL;
    SIZE_T size;
    HRESULT hr;

    if (!out) return (UINT64)(UINT)E_INVALIDARG;
    args[1] = offset;
    args[2] = want;
    args[3] = (UINT64)(ULONG_PTR)&bits;
    args[4] = flags;
    hr = (HRESULT)unix_vtbl_call( host, slot, 5, args );
    if (FAILED(hr)) return (UINT64)(UINT)hr;

    if (!bits || (ULONG_PTR)bits < 0x100000000ull)
    {
        *out = (UINT)(ULONG_PTR)bits;
        return (UINT64)(UINT)hr;
    }

    size = want;
    if (!size)
    {
        UINT desc[8] = { 0 };
        UINT64 d[D3D9_UNIX_MAX_ARGS] = { 0 };

        d[1] = (UINT64)(ULONG_PTR)desc;
        if (FAILED((HRESULT)unix_vtbl_call( host, slot + 2 /* GetDesc */, 2, d ))
            || desc[4] <= offset)
        {
            ERR( "IDirect3D{Vertex,Index}Buffer9::Lock: SizeToLock 0 means "
                 "'to the end', and the buffer would not say how long it is "
                 "(described size %u, offset %u).  Unlocking and refusing "
                 "rather than bouncing a length this module guessed.\n",
                 desc[4], offset );
            memset( d, 0, sizeof(d) );
            unix_vtbl_call( host, slot + 1 /* Unlock */, 1, d );
            /* refusal hygiene by hand -- see hand32_d3d9_surface_lock_rect */
            winecom_refused_scrub_dw( out );
            return (UINT64)(UINT)E_NOTIMPL;
        }
        size = desc[4] - offset;
    }

    /* the flush copies back exactly [OffsetToLock, +size), which is exactly
     * what the guest was given -- so a partial lock never writes over bytes
     * outside it, and DISCARD may skip the fill for any lock. */
    if (FAILED(hr = lock_bounce_apply( host, 0, "IDirect3D{Vertex,Index}Buffer9::Lock",
                                       &bits, size, size, flags, TRUE )))
    {
        UINT64 un[D3D9_UNIX_MAX_ARGS] = { 0 };

        unix_vtbl_call( host, slot + 1 /* Unlock */, 1, un );
        return (UINT64)(UINT)hr;
    }
    *out = (UINT)(ULONG_PTR)bits;
    return (UINT64)(UINT)hr;
}

static UINT64 hand32_d3d9_buffer_unlock( void *host, UINT slot, I386_CONTEXT *ctx )
{
    UINT64 args[D3D9_UNIX_MAX_ARGS] = { 0 };

    return unlock_bounce( host, 0, slot, 1, args );
}

/* ---------------------------------------------------------- flat entries */

static HRESULT flat_call( UINT func, UINT argc, UINT64 *args, UINT64 *ret )
{
    struct d3d9_flat_params p;
    NTSTATUS status;

    if (!com_runtime_init()) return E_FAIL;
    memcpy( p.args, args, sizeof(p.args) );
    p.func = func;
    p.argc = argc;
    p.ret = 0;
    if ((status = D3D9_UNIX_CALL( flat, &p )))
    {
        ERR( "unix flat call %u failed, status %08x\n", func, (UINT)status );
        return E_FAIL;
    }
    if (ret) *ret = p.ret;
    return (HRESULT)p.ret;
}

/* The refusal every plain-named interface-bearing flat export shares; see
 * dlls/d3d11/main.c for the argument at length. */
static HRESULT refuse_native_caller( const char *name )
{
    FIXME( "%s called by a NATIVE ppc64 caller.  This lane's D3D9 objects are "
           "guest proxies whose vtables are x86-64 trap stubs; only the "
           "emulated guest can call them.  The guest reaches this module "
           "through __wine_guest_%s (see dlls/d3d9/d3d9.thunks).\n",
           name, name );
    return E_NOTIMPL;
}

void * WINAPI __wine_guest_Direct3DCreate9( UINT sdk_version )
{
    UINT64 args[8] = { 0 }, ret = 0;
    void *d3d9;

    TRACE( "sdk_version %u\n", sdk_version );
    if (!com_runtime_init()) return NULL;
    args[0] = sdk_version;
    flat_call( FLAT_Direct3DCreate9, 1, args, &ret );
    if (!(d3d9 = (void *)(ULONG_PTR)ret)) return NULL;
    /* Returned BY VALUE rather than through an out-parameter, which is the
     * one shape winecom_wrap_static cannot be handed; the pointer is wrapped
     * directly.  DXVK's Direct3DCreate9 hands back a reference, and
     * winecom_wrap consumes exactly one -- so the guest owns it and the
     * refcount is where the contract says it is. */
    return winecom_wrap( d3d9, D3D9_IFACE_IDirect3D9 );
}

void * WINAPI Direct3DCreate9( UINT sdk_version )
{
    refuse_native_caller( "Direct3DCreate9" );
    return NULL;
}

HRESULT WINAPI __wine_guest_Direct3DCreate9Ex( UINT sdk_version, void **d3d9ex )
{
    UINT64 args[8] = { 0 };
    HRESULT hr;

    TRACE( "sdk_version %u, d3d9ex %p\n", sdk_version, d3d9ex );
    if (!com_runtime_init()) return E_FAIL;
    args[0] = sdk_version;
    args[1] = (UINT64)(ULONG_PTR)d3d9ex;
    hr = flat_call( FLAT_Direct3DCreate9Ex, 2, args, NULL );
    if (FAILED(hr)) return hr;
    winecom_wrap_static( d3d9ex, D3D9_IFACE_IDirect3D9Ex );
    return hr;
}

HRESULT WINAPI Direct3DCreate9Ex( UINT sdk_version, void **d3d9ex )
{
    return refuse_native_caller( "Direct3DCreate9Ex" );
}

/* D3D9On12, both spellings.  Refused with the reason rather than left to fail
 * somewhere else, and it is the SAME refusal D3D11On12CreateDevice carries one
 * API up: the two lanes hold separate winecom instances.
 *
 * These have __wine_guest_ names as well as plain ones even though nothing is
 * ever served through them, because spec2thunk's flat-surface audit classifies
 * by SIGNATURE and both of these return or write an IDirect3D9Ex.  An export
 * that vends an interface must be classified; "it always fails" is not a
 * classification the audit can see, and hard-coding it as one would mean that
 * the day somebody implemented it, the guest would silently get the plain
 * export and a native vtable with it. */
static HRESULT refuse_on12( const char *name )
{
    static BOOL logged;

    if (!logged)
    {
        logged = TRUE;
        FIXME( "%s: D3D9On12 needs a live ID3D12Device from the d3d12 lane, "
               "and the two lanes hold SEPARATE winecom instances "
               "(libs/winecom state is per-linkee).  A d3d12 proxy handed to "
               "this module's runtime is not one of its proxies and would be "
               "refused a frame later, in the middle of a resource wrap, where "
               "the reason would be illegible.  Refused here instead -- the "
               "same refusal D3D11On12CreateDevice carries one API up.\n", name );
    }
    return E_NOTIMPL;
}

HRESULT WINAPI __wine_guest_Direct3DCreate9On12Ex( UINT sdk_version, void *override_list,
                                                   UINT override_entries, void **d3d9ex )
{
    /* refusal hygiene by hand -- see hand32_d3d9_surface_lock_rect */
    winecom_refused_scrub_ptr( d3d9ex );
    return refuse_on12( "Direct3DCreate9On12Ex" );
}

HRESULT WINAPI Direct3DCreate9On12Ex( UINT sdk_version, void *override_list,
                                      UINT override_entries, void **d3d9ex )
{
    return refuse_native_caller( "Direct3DCreate9On12Ex" );
}

void * WINAPI __wine_guest_Direct3DCreate9On12( UINT sdk_version, void *override_list,
                                                UINT override_entries )
{
    refuse_on12( "Direct3DCreate9On12" );
    return NULL;
}

void * WINAPI Direct3DCreate9On12( UINT sdk_version, void *override_list,
                                   UINT override_entries )
{
    refuse_native_caller( "Direct3DCreate9On12" );
    return NULL;
}

/* The three D3DPERF entry points that take a string.
 *
 * DXVK's native headers typedef WCHAR to wchar_t, which is FOUR bytes here;
 * the guest PE's WCHAR is two.  The same refusal the generator applies to
 * ID3DUserDefinedAnnotation::BeginEvent on the D3D11 surface, for the same
 * reason and with the same consequence: a debug-annotating application sees
 * these do nothing, and nothing else changes.  Converting would be a dozen
 * lines and is worth doing the day something needs it; guessing is not. */
static int refuse_perf_wchar( const char *name )
{
    static BOOL logged;

    if (!logged)
    {
        logged = TRUE;
        FIXME( "%s and its siblings take a WCHAR string, and DXVK's native "
               "headers typedef WCHAR to wchar_t (4 bytes here) while the "
               "guest PE's is 2.  These are debug annotations: they are "
               "refused rather than handed a string that would be read at the "
               "wrong stride.\n", name );
    }
    return 0;
}

int WINAPI D3DPERF_BeginEvent( DWORD colour, const void *name )
{
    return refuse_perf_wchar( "D3DPERF_BeginEvent" );
}

void WINAPI D3DPERF_SetMarker( DWORD colour, const void *name )
{
    refuse_perf_wchar( "D3DPERF_SetMarker" );
}

void WINAPI D3DPERF_SetRegion( DWORD colour, const void *name )
{
    refuse_perf_wchar( "D3DPERF_SetRegion" );
}

int WINAPI D3DPERF_EndEvent( void )
{
    UINT64 args[8] = { 0 }, ret = 0;

    flat_call( FLAT_D3DPERF_EndEvent, 0, args, &ret );
    return (int)ret;
}

DWORD WINAPI D3DPERF_GetStatus( void )
{
    UINT64 args[8] = { 0 }, ret = 0;

    flat_call( FLAT_D3DPERF_GetStatus, 0, args, &ret );
    return (DWORD)ret;
}

BOOL WINAPI D3DPERF_QueryRepeatFrame( void )
{
    UINT64 args[8] = { 0 }, ret = 0;

    flat_call( FLAT_D3DPERF_QueryRepeatFrame, 0, args, &ret );
    return (BOOL)ret;
}

void WINAPI D3DPERF_SetOptions( DWORD options )
{
    UINT64 args[8] = { 0 };

    args[0] = options;
    flat_call( FLAT_D3DPERF_SetOptions, 1, args, NULL );
}

void WINAPI DebugSetMute( void )
{
    UINT64 args[8] = { 0 };

    flat_call( FLAT_DebugSetMute, 0, args, NULL );
}

/* Refused rather than forwarded.  Its documented return is an interface
 * (IDirect3DShaderValidator9) that is in no roster here, and a flat export
 * that hands the guest a raw NATIVE interface pointer is the exact defect
 * this whole lane exists to prevent -- the guest's first call through it
 * would execute ppc64 bytes as x86-64.  DXVK returns nothing useful from it
 * either, so nothing is lost by saying so. */
HRESULT WINAPI Direct3DShaderValidatorCreate9( void )
{
    static BOOL logged;

    if (!logged)
    {
        logged = TRUE;
        FIXME( "Direct3DShaderValidatorCreate9 returns an interface that is in "
               "no roster on this surface, so there is no guest vtable to give "
               "it and a raw native pointer would be executed as x86-64.  "
               "Refused; nothing in the shipping D3D9 runtime needs it.\n" );
    }
    return E_NOTIMPL;
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

ULONG_PTR WINAPI D3DPERF_BeginEvent( ULONG_PTR a1, ULONG_PTR a2 )
{
    __wine_spec_unimplemented_stub( "d3d9.dll", "D3DPERF_BeginEvent" );
}

ULONG_PTR WINAPI D3DPERF_EndEvent( void )
{
    __wine_spec_unimplemented_stub( "d3d9.dll", "D3DPERF_EndEvent" );
}

ULONG_PTR WINAPI D3DPERF_GetStatus( void )
{
    __wine_spec_unimplemented_stub( "d3d9.dll", "D3DPERF_GetStatus" );
}

ULONG_PTR WINAPI D3DPERF_QueryRepeatFrame( void )
{
    __wine_spec_unimplemented_stub( "d3d9.dll", "D3DPERF_QueryRepeatFrame" );
}

ULONG_PTR WINAPI D3DPERF_SetMarker( ULONG_PTR a1, ULONG_PTR a2 )
{
    __wine_spec_unimplemented_stub( "d3d9.dll", "D3DPERF_SetMarker" );
}

ULONG_PTR WINAPI D3DPERF_SetOptions( ULONG_PTR a1 )
{
    __wine_spec_unimplemented_stub( "d3d9.dll", "D3DPERF_SetOptions" );
}

ULONG_PTR WINAPI D3DPERF_SetRegion( ULONG_PTR a1, ULONG_PTR a2 )
{
    __wine_spec_unimplemented_stub( "d3d9.dll", "D3DPERF_SetRegion" );
}

ULONG_PTR WINAPI DebugSetMute( void )
{
    __wine_spec_unimplemented_stub( "d3d9.dll", "DebugSetMute" );
}

ULONG_PTR WINAPI Direct3DCreate9( ULONG_PTR a1 )
{
    __wine_spec_unimplemented_stub( "d3d9.dll", "Direct3DCreate9" );
}

ULONG_PTR WINAPI Direct3DCreate9Ex( ULONG_PTR a1, ULONG_PTR a2 )
{
    __wine_spec_unimplemented_stub( "d3d9.dll", "Direct3DCreate9Ex" );
}

ULONG_PTR WINAPI Direct3DCreate9On12( ULONG_PTR a1, ULONG_PTR a2, ULONG_PTR a3 )
{
    __wine_spec_unimplemented_stub( "d3d9.dll", "Direct3DCreate9On12" );
}

ULONG_PTR WINAPI Direct3DCreate9On12Ex( ULONG_PTR a1, ULONG_PTR a2, ULONG_PTR a3, ULONG_PTR a4 )
{
    __wine_spec_unimplemented_stub( "d3d9.dll", "Direct3DCreate9On12Ex" );
}

ULONG_PTR WINAPI Direct3DShaderValidatorCreate9( void )
{
    __wine_spec_unimplemented_stub( "d3d9.dll", "Direct3DShaderValidatorCreate9" );
}

ULONG_PTR WINAPI __wine_com_dispatch( ULONG_PTR a1, ULONG_PTR a2, ULONG_PTR a3 )
{
    __wine_spec_unimplemented_stub( "d3d9.dll", "__wine_com_dispatch" );
}

ULONG_PTR WINAPI __wine_com_dispatch32( ULONG_PTR a1, ULONG_PTR a2, ULONG_PTR a3 )
{
    __wine_spec_unimplemented_stub( "d3d9.dll", "__wine_com_dispatch32" );
}

ULONG_PTR WINAPI __wine_com_slot_name( ULONG_PTR a1, ULONG_PTR a2, ULONG_PTR a3, ULONG_PTR a4 )
{
    __wine_spec_unimplemented_stub( "d3d9.dll", "__wine_com_slot_name" );
}

ULONG_PTR WINAPI __wine_guest_Direct3DCreate9( ULONG_PTR a1 )
{
    __wine_spec_unimplemented_stub( "d3d9.dll", "__wine_guest_Direct3DCreate9" );
}

ULONG_PTR WINAPI __wine_guest_Direct3DCreate9Ex( ULONG_PTR a1, ULONG_PTR a2 )
{
    __wine_spec_unimplemented_stub( "d3d9.dll", "__wine_guest_Direct3DCreate9Ex" );
}

ULONG_PTR WINAPI __wine_guest_Direct3DCreate9On12( ULONG_PTR a1, ULONG_PTR a2, ULONG_PTR a3 )
{
    __wine_spec_unimplemented_stub( "d3d9.dll", "__wine_guest_Direct3DCreate9On12" );
}

ULONG_PTR WINAPI __wine_guest_Direct3DCreate9On12Ex( ULONG_PTR a1, ULONG_PTR a2, ULONG_PTR a3, ULONG_PTR a4 )
{
    __wine_spec_unimplemented_stub( "d3d9.dll", "__wine_guest_Direct3DCreate9On12Ex" );
}

#endif  /* __powerpc64__ */
