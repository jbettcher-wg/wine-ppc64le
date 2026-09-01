/*
 * d3d11 unixlib interface -- the PE half's window into native DXVK.
 *
 * The native d3d11.dll REPLACES Wine's wined3d-backed implementation: the
 * unix side dlopens DXVK's own libdxvk_d3d11.so (which in turn loads
 * libdxvk_dxgi.so through its build-tree DT_RUNPATH, exactly as upstream
 * intends) and everything above it speaks Wine's ordinary args-struct
 * unixlib boundary.  Same shape as dlls/d3d12's window into vkd3d-proton.
 *
 * ONE MODULE OWNS THREE DLLS.  libs/winecom's state is per-linkee, so a proxy
 * interned by dxgi.dll's runtime would not be recognised by d3d11.dll's, and
 * the very first `D3D11CreateDevice(adapter_from_dxgi, ...)` would be refused
 * as "not one of ours".  So this module holds the ONLY winecom instance for
 * the whole DXVK surface; native dxgi.dll and d3d10core.dll are forwarders
 * into it (their .spec files forward both their flat exports and
 * __wine_com_dispatch here), and all three guest thunk modules publish the
 * SAME roster from ppc64le/dxvk/interfaces_dxvk.json, so a proxy's guest
 * vtable is interchangeable between them.
 *
 * There are deliberately only four calls.  Every COM slot crossing carries
 * (host iface, vtable slot, the guest's integer arguments); the unix side
 * calls through the vtable with the widest integer form, which is correct for
 * every marshalled slot because ppc64le/dxvk/gen_winecom.py refuses anything
 * that is not integer-class.  The one exception is `float_call`, which exists
 * because three D3D11 slots take or return a float BY VALUE -- an argument
 * class the widest-integer form cannot express at all, since ELFv2 would put
 * it in the floating-point register file.  Argument translation -- proxies,
 * REFIID/void** wraps -- happens entirely on the PE side, which owns the
 * winecom runtime state.
 *
 * Copyright 2026 the ppc64le port authors
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 */

#ifndef __WINE_D3D11_UNIXLIB_H
#define __WINE_D3D11_UNIXLIB_H

#include "windef.h"
#include "winternl.h"
#include "wine/unixlib.h"

/* The flat entry points across all three libraries, resolved by name at init.
 * The names are DXVK's own .def files (ppc64le/dxvk/src/src/{d3d11,dxgi,
 * d3d10}, each module's own .def) -- what we promise callers is what DXVK
 * implements. */
enum d3d11_flat_func
{
    FLAT_D3D11CreateDevice,
    FLAT_D3D11CreateDeviceAndSwapChain,
    FLAT_D3D11CoreCreateDevice,
    FLAT_CreateDXGIFactory,
    FLAT_CreateDXGIFactory1,
    FLAT_CreateDXGIFactory2,
    FLAT_DXGIGetDebugInterface1,
    FLAT_DXGIDeclareAdapterRemovalSupport,
    FLAT_D3D10CoreCreateDevice,
    FLAT_D3D10CoreGetVersion,
    FLAT_D3D10CoreRegisterLayers,
    FLAT_FUNC_COUNT
};

#define D3D11_UNIX_MAX_ARGS 16

struct d3d11_init_params
{
    UINT64 unused;
};

/* One COM vtable call.  args[0] is the HOST interface pointer (the PE side
 * has already unwrapped the proxy); slot indexes its vtable. */
struct d3d11_call_params
{
    UINT64 args[D3D11_UNIX_MAX_ARGS];
    UINT64 ret;
    UINT   slot;
    UINT   argc;
};

/* The three float-bearing slots, by shape rather than by name -- the shape is
 * what the C prototype on the unix side has to be, and there is exactly one
 * right prototype per shape.  See dlls/d3d11/unix.c. */
enum d3d11_float_shape
{
    FLOAT_SHAPE_RES_UINT_FLOAT_BYTE,   /* void (self, void*, UINT, float, UINT8)
                                          ClearDepthStencilView */
    FLOAT_SHAPE_RES_FLOAT,             /* void (self, void*, float)
                                          SetResourceMinLOD */
    FLOAT_SHAPE_RES_RET_FLOAT,         /* float (self, void*)
                                          GetResourceMinLOD */
    FLOAT_SHAPE_COUNT
};

struct d3d11_float_params
{
    UINT64 self;        /* host interface pointer */
    UINT64 res;         /* host ID3D11Resource / view pointer */
    UINT64 a;           /* the integer argument, if the shape has one */
    UINT64 b;
    float  f;           /* in */
    float  ret;         /* out, for FLOAT_SHAPE_RES_RET_FLOAT */
    UINT   slot;
    UINT   shape;       /* enum d3d11_float_shape */
};

/* One flat entry-point call. */
struct d3d11_flat_params
{
    UINT64 args[12];
    UINT64 ret;
    UINT   func;    /* enum d3d11_flat_func */
    UINT   argc;
};

/* The GENERIC float-bearing vtable call (PPC64EC step C): what unix_float's
 * per-shape enum is for the hand walkers, this is for the GENERATED rows --
 * the marshal table's fpmask/fpwide/fpret name the positions, so no shape
 * list has to grow one case per newly-served slot.  args[] is the integer
 * view with each floating-point position carrying the value's raw bits
 * (winecom_invoke_fp_fn's exact contract, wine/winecom.h); fpword is the
 * flat lane's encoding (mask | single<<8 | ret<<16); the unix side splits
 * into ELFv2's two register files through the one shared implementation
 * (wine/winecom_fpcall.h) and calls the host slot.  fpret_bits carries f1's
 * double-format bits back when the return is floating point; ret carries
 * RAX's worth as always. */
struct d3d11_fpcall_params
{
    UINT64 args[D3D11_UNIX_MAX_ARGS];
    UINT64 fpret_bits;
    UINT64 ret;
    UINT   slot;
    UINT   argc;
    UINT   fpword;
};

/* ------------------------------------------------------------ presentation
 *
 * The two calls below carry everything the presentation path needs across the
 * boundary, and they exist as unixlib entry points rather than as callbacks
 * FROM DXVK for one reason: win32u may only be entered from a Wine thread, and
 * a unixlib entry point is by construction on one.  DXVK owns a CS thread and
 * a submission thread, and its Present work runs on both -- so the hooks
 * win32u wants around a present are driven from the PE side's Present slot,
 * on the application's own thread, exactly as dlls/d3d12/unix_present.c does
 * for vkd3d.  See ppc64le/dxvk/dxvk_win32u_wsi.h for the other half.
 */

/* Where in a present we are.  win32u's own vkQueuePresentKHR wrapper updates
 * the client surface before the present and marks it presented after; this
 * lane's "after" is after the present was QUEUED, because DXVK's actual
 * vkQueuePresentKHR happens on its submission thread.  Both hooks are about
 * keeping the client surface's geometry and damage in step with the window,
 * not about GPU synchronisation, so the difference does not lose anything. */
enum d3d11_present_phase
{
    PRESENT_PHASE_BEGIN,
    PRESENT_PHASE_END,
};

struct d3d11_present_params
{
    UINT64 hwnd;        /* the swapchain's output window */
    UINT   phase;       /* enum d3d11_present_phase */
    INT    result;      /* the HRESULT DXVK's Present returned, for END */
};

/* The PE side's answer to the two questions DXVK asks win32u about a window
 * (its client size, and whether it still exists).  Pushed rather than pulled:
 * the unix side has no way to ask user32 anything, and the PE side is holding
 * the HWND anyway at every point where DXVK is about to ask. */
struct d3d11_hwnd_params
{
    UINT64 hwnd;
    UINT   width;
    UINT   height;
    UINT   valid;       /* zero once the window is gone or the swapchain went */
};

/* ------------------------------------------------------------ event relay
 *
 * The unix half of the surface's event_mint/event_reap hooks (winecom.h):
 * Wine events cross to DXVK as the tagged eventfd the native sync convention
 * understands -- VKD3D_NATIVE_EVENT_TAG in ppc64le/vkd3d/src/include/
 * vkd3d_native_event_handle.h, 'EVFD' in bits 63..32 over the fd, the SAME
 * constant our dxvk-patches teach util_win32_compat's SetEvent (the
 * check-d3d11-smoke gate asserts the two spellings agree).  The PE side owns
 * a duplicated reference to the guest event and a pump thread; this side
 * owns the eventfds and the epoll set.  Payout flow: DXVK writes the
 * eventfd -> the pump's epoll wakes -> unix_event_pump returns the entry's
 * guest handle -> the PE pump thread NtSetEvent()s it and re-enters. */
struct d3d11_event_mint_params
{
    UINT64 guest_handle;   /* the PE side's OWN duplicated reference */
    UINT   oneshot;        /* entry dies at first payout */
    UINT64 native_handle;  /* out: the tagged eventfd value, 0 on failure */
};

struct d3d11_event_pump_params
{
    UINT64 guest_handle;   /* out: signal this event */
    UINT   close_handle;   /* out: one-shot paid out -- NtClose the ref too */
    UINT   shutdown;       /* out: never set today; the pump is
                              process-lifetime by design */
};

struct d3d11_event_reap_params
{
    UINT64 native_handle;
    UINT64 guest_handle;   /* out: the reference to NtClose, 0 if unknown */
};

enum d3d11_unix_func
{
    unix_init,
    unix_call,
    unix_float,
    unix_flat,
    unix_present,
    unix_hwnd,
    unix_fpcall,    /* appended last: existing ids keep their values */
    unix_event_mint,
    unix_event_pump,
    unix_event_reap,
    unix_funcs_count
};

#define D3D11_UNIX_CALL( code, params ) WINE_UNIX_CALL( unix_ ## code, params )

#endif /* __WINE_D3D11_UNIXLIB_H */
