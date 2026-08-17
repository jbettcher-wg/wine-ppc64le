/*
 * d3d9 unixlib interface -- the PE half's window into native DXVK's d3d9.
 *
 * The native d3d9.dll REPLACES Wine's wined3d-backed implementation, the same
 * shape as dlls/d3d11 does for D3D11 and dlls/d3d12 does for D3D12.  Read
 * dlls/d3d11/unixlib.h first; everything structural here is that file's, and
 * only the differences are written out below.
 *
 * A SECOND WINECOM INSTANCE, AND THAT IS CORRECT.  dlls/d3d11 holds the ONLY
 * instance for d3d11+dxgi+d3d10core because those three surfaces share
 * objects: one `D3D11CreateDevice(adapter, ...)` spans dxgi and d3d11, so a
 * proxy minted by one runtime must be recognised by the other.  D3D9 shares
 * nothing with them -- no D3D9 method takes a DXGI interface and no DXGI
 * method takes a D3D9 one -- and DXVK says the same thing in the linker's
 * words: libdxvk_d3d9.so has no DT_NEEDED on libdxvk_dxgi.so and is entirely
 * self-contained.  So this module holds its own instance over its own roster
 * (ppc64le/dxvk/interfaces_d3d9.json, 21 interfaces / 497 slots) and the two
 * never meet.
 *
 * WHAT D3D9 NEEDS THAT D3D11 DID NOT.  Two things, and both come from D3D9
 * being an older and blunter API:
 *
 *   * FLOATS BY VALUE IN THE HOT PATH.  `IDirect3DDevice9::Clear` takes the
 *     depth as a by-value float, and every D3D9 title clears every frame.
 *     D3D11 has three such slots in corners of the API; D3D9 has one in the
 *     middle of it.  The float shapes below are what the unix side needs to
 *     call them with a real prototype -- see dlls/d3d9/unix.c.
 *   * NO DXGI.  The window is an argument of CreateDevice and a member of
 *     D3DPRESENT_PARAMETERS, the swapchain is implicit in the device, and
 *     Reset re-creates it.  So presentation is the device's business here
 *     rather than a separate object's, and the present hooks hang off
 *     IDirect3DDevice9::Present rather than IDXGISwapChain::Present.
 *
 * Copyright 2026 the ppc64le port authors
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 */

#ifndef __WINE_D3D9_UNIXLIB_H
#define __WINE_D3D9_UNIXLIB_H

#include "windef.h"
#include "winternl.h"
#include "wine/unixlib.h"

/* The flat entry points, resolved by name at init.  The names are DXVK's own
 * src/d3d9/d3d9.def -- what we promise callers is what DXVK implements. */
enum d3d9_flat_func
{
    FLAT_Direct3DCreate9,
    FLAT_Direct3DCreate9Ex,
    FLAT_Direct3DShaderValidatorCreate9,
    FLAT_D3DPERF_EndEvent,
    FLAT_D3DPERF_GetStatus,
    FLAT_D3DPERF_QueryRepeatFrame,
    FLAT_D3DPERF_SetOptions,
    FLAT_DebugSetMute,
    FLAT_FUNC_COUNT
};

#define D3D9_UNIX_MAX_ARGS 16

struct d3d9_init_params
{
    UINT64 unused;
};

/* One COM vtable call.  args[0] is the HOST interface pointer (the PE side has
 * already unwrapped the proxy); slot indexes its vtable. */
struct d3d9_call_params
{
    UINT64 args[D3D9_UNIX_MAX_ARGS];
    UINT64 ret;
    UINT   slot;
    UINT   argc;
};

/* The float-bearing slots, by shape rather than by name.
 *
 * `Clear`'s float is the FIFTH argument counting `this`, which MS-x64 puts on
 * the STACK and not in an XMM register -- past the four register slots a float
 * is spilled as four bytes in an eight-byte stack slot.  That is why it is
 * read out of the trap CONTEXT's stack image on the PE side and arrives here
 * as an ordinary `float` field, unlike D3D11's ClearDepthStencilView whose
 * depth is the fourth argument and lives in XMM3.  The ELFv2 side of the call
 * is the same problem either way: a float argument goes in f1..f13, which the
 * unixlib's widest-integer call form never writes. */
enum d3d9_float_shape
{
    D3D9_FLOAT_CLEAR,          /* HRESULT (self, DWORD, const void *, DWORD,
                                  DWORD, float, DWORD)  -- Clear */
    D3D9_FLOAT_SET,            /* HRESULT (self, float)  -- SetNPatchMode */
    D3D9_FLOAT_GET,            /* float (self)           -- GetNPatchMode */
    D3D9_FLOAT_SHAPE_COUNT
};

struct d3d9_float_params
{
    UINT64 self;        /* host interface pointer */
    UINT64 a, b, c, d, e;  /* the integer arguments, if the shape has them */
    float  f;           /* in */
    float  ret_f;       /* out, for D3D9_FLOAT_GET */
    UINT64 ret;         /* out, the HRESULT for the others */
    UINT   slot;
    UINT   shape;       /* enum d3d9_float_shape */
};

/* One flat entry-point call. */
struct d3d9_flat_params
{
    UINT64 args[8];
    UINT64 ret;
    UINT   func;    /* enum d3d9_flat_func */
    UINT   argc;
};

/* Presentation.  Identical in shape and in reason to dlls/d3d11/unixlib.h's
 * pair -- read the banner there.  The only difference is which object owns the
 * window: a D3D9 device does, where a D3D11 swapchain did. */
enum d3d9_present_phase
{
    D3D9_PRESENT_BEGIN,
    D3D9_PRESENT_END,
};

struct d3d9_present_params
{
    UINT64 hwnd;
    UINT   phase;       /* enum d3d9_present_phase */
    INT    result;
};

struct d3d9_hwnd_params
{
    UINT64 hwnd;
    UINT   width;
    UINT   height;
    UINT   valid;
};

enum d3d9_unix_func
{
    unix_init,
    unix_call,
    unix_float,
    unix_flat,
    unix_present,
    unix_hwnd,
    unix_funcs_count
};

#define D3D9_UNIX_CALL( code, params ) WINE_UNIX_CALL( unix_ ## code, params )

#endif /* __WINE_D3D9_UNIXLIB_H */
