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
 * INTO HOST MEMORY, and the guest's cell for it is four bytes.  Seven rows:
 * three LockRect, two LockBox, and the two buffer Locks whose answer is a
 * bare `void **`.
 *
 * WHAT THIS DOES, AND WHAT IT DELIBERATELY DOES NOT.  It calls the host,
 * looks at the address, and hands it back when it fits below 4 GiB.  When it
 * does not fit it UNLOCKS AGAIN and refuses by name, because a truncated
 * pointer is not a worse pointer, it is a different one, and the guest would
 * write the application's vertices over whatever lives at the low half.
 *
 * It does NOT bounce.  dlls/d3d11/main.c's Map walker keeps a below-4-GiB
 * shadow per subresource and copies in and out around Unmap, and that is the
 * complete answer -- but it needs to SIZE the mapping, which on D3D11 meant a
 * subresource-size computation and on D3D9 means block-compressed mip
 * arithmetic per format.  Sizing a D3D9 lock correctly is the next piece of
 * work here, and guessing at it would corrupt exactly the buffers this is
 * meant to protect.  So: correct or loud, and the loudness names the wall.
 *
 * [UNVERIFIED as of 2026-08-30] Whether DXVK's d3d9 actually maps below
 * 4 GiB on this host is not known -- the D3D11 lane found it went both ways.
 * If it does, these rows work as they stand; if it does not, the ERR below
 * fires on the first lock and the bounce is required.  The log tells which,
 * on the first run, which is why it is written this way round. */

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

/* -> TRUE when the address fits a guest cell.  One log per module: a lock
 * that does not fit repeats every frame and would drown the run. */
static BOOL guest_legal( const void *p, const char *what )
{
    if (!p || (ULONG_PTR)p < 0x100000000ull) return TRUE;
    {
        static BOOL logged;

        if (!logged)
        {
            logged = TRUE;
            ERR( "%s answered %p, above 4 GiB, which a 32-bit guest's pointer "
                 "cell cannot hold.  Unlocking and refusing rather than "
                 "handing back a truncated address that names different "
                 "memory.  Serving this needs the below-4-GiB bounce "
                 "dlls/d3d11/main.c built for ID3D11DeviceContext::Map, which "
                 "in turn needs a correct size for a D3D9 lock.\n", what, p );
        }
    }
    return FALSE;
}

/* the shared tail of the five rect/box walkers: `unlock_slot` is the row
 * that undoes this one, which on every one of these interfaces is the very
 * next vtable slot (LockRect/UnlockRect, LockBox/UnlockBox -- checked
 * against ppc64le/dxvk/interfaces_d3d9.json, and the C_ASSERT-free way to
 * say so is that a wrong guess only ever unlocks something already
 * unlocked). */
static UINT64 lock_answer( void *host, UINT unlock_slot, UINT unlock_argc,
                           UINT64 *unlock_args, HRESULT hr, const void *bits,
                           const char *what )
{
    if (FAILED(hr)) return (UINT64)(UINT)hr;
    if (guest_legal( bits, what )) return (UINT64)(UINT)hr;
    unix_vtbl_call( host, unlock_slot, unlock_argc, unlock_args );
    return (UINT64)(UINT)E_NOTIMPL;
}

/* IDirect3DSurface9::LockRect( D3DLOCKED_RECT *, const RECT *, DWORD ) */
static UINT64 hand32_d3d9_surface_lock_rect( void *host, UINT slot, I386_CONTEXT *ctx )
{
    const ULONG *esp = frame32( ctx );
    UINT *out = (UINT *)(ULONG_PTR)esp[2];       /* {INT Pitch; ptr32 pBits} */
    struct d3d9_locked_rect lr = { 0 };
    UINT64 args[D3D9_UNIX_MAX_ARGS] = { 0 };
    UINT64 un[D3D9_UNIX_MAX_ARGS] = { 0 };
    HRESULT hr;

    if (!out) return (UINT64)(UINT)E_INVALIDARG;
    args[1] = (UINT64)(ULONG_PTR)&lr;
    args[2] = esp[3];
    args[3] = esp[4];
    hr = (HRESULT)unix_vtbl_call( host, slot, 4, args );
    hr = (HRESULT)(UINT)lock_answer( host, slot + 1, 1, un, hr, lr.pBits,
                                     "IDirect3DSurface9::LockRect" );
    if (FAILED(hr)) return (UINT64)(UINT)hr;
    out[0] = (UINT)lr.Pitch;
    out[1] = (UINT)(ULONG_PTR)lr.pBits;
    return (UINT64)(UINT)hr;
}

/* IDirect3DTexture9::LockRect( UINT Level, D3DLOCKED_RECT *, const RECT *,
 * DWORD ).  UnlockRect takes the level back. */
static UINT64 hand32_d3d9_texture_lock_rect( void *host, UINT slot, I386_CONTEXT *ctx )
{
    const ULONG *esp = frame32( ctx );
    UINT *out = (UINT *)(ULONG_PTR)esp[3];
    struct d3d9_locked_rect lr = { 0 };
    UINT64 args[D3D9_UNIX_MAX_ARGS] = { 0 };
    UINT64 un[D3D9_UNIX_MAX_ARGS] = { 0 };
    HRESULT hr;

    if (!out) return (UINT64)(UINT)E_INVALIDARG;
    args[1] = esp[2];
    args[2] = (UINT64)(ULONG_PTR)&lr;
    args[3] = esp[4];
    args[4] = esp[5];
    hr = (HRESULT)unix_vtbl_call( host, slot, 5, args );
    un[1] = esp[2];
    hr = (HRESULT)(UINT)lock_answer( host, slot + 1, 2, un, hr, lr.pBits,
                                     "IDirect3DTexture9::LockRect" );
    if (FAILED(hr)) return (UINT64)(UINT)hr;
    out[0] = (UINT)lr.Pitch;
    out[1] = (UINT)(ULONG_PTR)lr.pBits;
    return (UINT64)(UINT)hr;
}

/* IDirect3DCubeTexture9::LockRect( D3DCUBEMAP_FACES, UINT Level,
 * D3DLOCKED_RECT *, const RECT *, DWORD ) */
static UINT64 hand32_d3d9_cube_lock_rect( void *host, UINT slot, I386_CONTEXT *ctx )
{
    const ULONG *esp = frame32( ctx );
    UINT *out = (UINT *)(ULONG_PTR)esp[4];
    struct d3d9_locked_rect lr = { 0 };
    UINT64 args[D3D9_UNIX_MAX_ARGS] = { 0 };
    UINT64 un[D3D9_UNIX_MAX_ARGS] = { 0 };
    HRESULT hr;

    if (!out) return (UINT64)(UINT)E_INVALIDARG;
    args[1] = esp[2];
    args[2] = esp[3];
    args[3] = (UINT64)(ULONG_PTR)&lr;
    args[4] = esp[5];
    args[5] = esp[6];
    hr = (HRESULT)unix_vtbl_call( host, slot, 6, args );
    un[1] = esp[2];
    un[2] = esp[3];
    hr = (HRESULT)(UINT)lock_answer( host, slot + 1, 3, un, hr, lr.pBits,
                                     "IDirect3DCubeTexture9::LockRect" );
    if (FAILED(hr)) return (UINT64)(UINT)hr;
    out[0] = (UINT)lr.Pitch;
    out[1] = (UINT)(ULONG_PTR)lr.pBits;
    return (UINT64)(UINT)hr;
}

/* IDirect3DVolume9::LockBox( D3DLOCKED_BOX *, const D3DBOX *, DWORD ) */
static UINT64 hand32_d3d9_volume_lock_box( void *host, UINT slot, I386_CONTEXT *ctx )
{
    const ULONG *esp = frame32( ctx );
    UINT *out = (UINT *)(ULONG_PTR)esp[2];  /* {RowPitch, SlicePitch, ptr32} */
    struct d3d9_locked_box lb = { 0 };
    UINT64 args[D3D9_UNIX_MAX_ARGS] = { 0 };
    UINT64 un[D3D9_UNIX_MAX_ARGS] = { 0 };
    HRESULT hr;

    if (!out) return (UINT64)(UINT)E_INVALIDARG;
    args[1] = (UINT64)(ULONG_PTR)&lb;
    args[2] = esp[3];
    args[3] = esp[4];
    hr = (HRESULT)unix_vtbl_call( host, slot, 4, args );
    hr = (HRESULT)(UINT)lock_answer( host, slot + 1, 1, un, hr, lb.pBits,
                                     "IDirect3DVolume9::LockBox" );
    if (FAILED(hr)) return (UINT64)(UINT)hr;
    out[0] = (UINT)lb.RowPitch;
    out[1] = (UINT)lb.SlicePitch;
    out[2] = (UINT)(ULONG_PTR)lb.pBits;
    return (UINT64)(UINT)hr;
}

/* IDirect3DVolumeTexture9::LockBox( UINT Level, D3DLOCKED_BOX *,
 * const D3DBOX *, DWORD ) */
static UINT64 hand32_d3d9_volumetex_lock_box( void *host, UINT slot, I386_CONTEXT *ctx )
{
    const ULONG *esp = frame32( ctx );
    UINT *out = (UINT *)(ULONG_PTR)esp[3];
    struct d3d9_locked_box lb = { 0 };
    UINT64 args[D3D9_UNIX_MAX_ARGS] = { 0 };
    UINT64 un[D3D9_UNIX_MAX_ARGS] = { 0 };
    HRESULT hr;

    if (!out) return (UINT64)(UINT)E_INVALIDARG;
    args[1] = esp[2];
    args[2] = (UINT64)(ULONG_PTR)&lb;
    args[3] = esp[4];
    args[4] = esp[5];
    hr = (HRESULT)unix_vtbl_call( host, slot, 5, args );
    un[1] = esp[2];
    hr = (HRESULT)(UINT)lock_answer( host, slot + 1, 2, un, hr, lb.pBits,
                                     "IDirect3DVolumeTexture9::LockBox" );
    if (FAILED(hr)) return (UINT64)(UINT)hr;
    out[0] = (UINT)lb.RowPitch;
    out[1] = (UINT)lb.SlicePitch;
    out[2] = (UINT)(ULONG_PTR)lb.pBits;
    return (UINT64)(UINT)hr;
}

/* IDirect3DVertexBuffer9::Lock / IDirect3DIndexBuffer9::Lock
 * ( UINT OffsetToLock, UINT SizeToLock, void **ppbData, DWORD Flags ).
 * Unlock takes nothing, and is the next slot on both interfaces. */
static UINT64 hand32_d3d9_buffer_lock( void *host, UINT slot, I386_CONTEXT *ctx )
{
    const ULONG *esp = frame32( ctx );
    UINT *out = (UINT *)(ULONG_PTR)esp[4];
    UINT64 args[D3D9_UNIX_MAX_ARGS] = { 0 };
    UINT64 un[D3D9_UNIX_MAX_ARGS] = { 0 };
    void *bits = NULL;
    HRESULT hr;

    if (!out) return (UINT64)(UINT)E_INVALIDARG;
    args[1] = esp[2];
    args[2] = esp[3];
    args[3] = (UINT64)(ULONG_PTR)&bits;
    args[4] = esp[5];
    hr = (HRESULT)unix_vtbl_call( host, slot, 5, args );
    hr = (HRESULT)(UINT)lock_answer( host, slot + 1, 1, un, hr, bits,
                                     "IDirect3D{Vertex,Index}Buffer9::Lock" );
    if (FAILED(hr)) return (UINT64)(UINT)hr;
    *out = (UINT)(ULONG_PTR)bits;
    return (UINT64)(UINT)hr;
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
    if (d3d9ex) *d3d9ex = NULL;
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
