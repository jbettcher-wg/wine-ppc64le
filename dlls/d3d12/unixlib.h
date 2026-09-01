/*
 * d3d12 unixlib interface -- the PE half's window into vkd3d-proton.
 *
 * The native d3d12.dll REPLACES Wine's forwarder into libs/vkd3d: the unix
 * side dlopens vkd3d-proton's own libvkd3d-proton-d3d12.so (which in turn
 * loads -d3d12core.so exactly as upstream intends -- see
 * ppc64le/vkd3d/src/libs/d3d12/main.c) and everything above it speaks Wine's
 * ordinary args-struct unixlib boundary.  Design:
 * vkd3d-ppc64le/docs/d3d12-native-lane-design.md §3.4.
 *
 * There are deliberately only three calls.  Every COM slot crossing carries
 * (host iface, vtable slot, the guest's integer arguments); the unix side
 * calls through the vtable with the widest integer form, which is correct for
 * every marshalled slot because the generator refuses anything that is not
 * integer-class (see dxvk-ppc64le/thunk/gen_winecom.py).  Argument
 * translation -- proxies, REFIID/void** wraps, sret placement -- happens
 * entirely on the PE side, which owns the winecom runtime state.
 *
 * Copyright 2026 the ppc64le port authors
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 */

#ifndef __WINE_D3D12_UNIXLIB_H
#define __WINE_D3D12_UNIXLIB_H

#include "windef.h"
#include "winternl.h"
#include "wine/unixlib.h"

/* The flat entry points, in vkd3d's d3d12.def order.  Hand-written on both
 * sides (8 entries); the unix side resolves them by name at init. */
enum d3d12_flat_func
{
    FLAT_D3D12CreateDevice,
    FLAT_D3D12GetDebugInterface,
    FLAT_D3D12CreateRootSignatureDeserializer,
    FLAT_D3D12CreateVersionedRootSignatureDeserializer,
    FLAT_D3D12EnableExperimentalFeatures,
    FLAT_D3D12SerializeRootSignature,
    FLAT_D3D12SerializeVersionedRootSignature,
    FLAT_D3D12GetInterface,
    FLAT_FUNC_COUNT
};

#define D3D12_UNIX_MAX_ARGS 16

struct d3d12_init_params
{
    UINT64 unused;
};

/* One COM vtable call.  args[0] is the HOST interface pointer (the PE side
 * has already unwrapped the proxy); slot indexes its vtable. */
struct d3d12_call_params
{
    UINT64 args[D3D12_UNIX_MAX_ARGS];
    UINT64 ret;
    UINT   slot;
    UINT   argc;
};

/* One flat entry-point call. */
struct d3d12_flat_params
{
    UINT64 args[8];
    UINT64 ret;
    UINT   func;    /* enum d3d12_flat_func */
    UINT   argc;
};

/* A vtable slot whose prototype carries a by-value float cannot cross
 * through the widest-integer form -- the value belongs in the other
 * register file.  Each such slot the surface serves gets a TYPED call here:
 * the PE side lifts the float out of the guest's XMM register and sends its
 * raw bits in an integer slot, and the unix side calls the host slot
 * through a correctly-typed function pointer.  One enum case per shape. */
enum d3d12_fp_shape
{
    /* void (this, D3D12_CPU_DESCRIPTOR_HANDLE, D3D12_CLEAR_FLAGS, FLOAT,
     *       UINT8, UINT, const D3D12_RECT *) */
    FP_SHAPE_CLEAR_DSV,
    /* void (this, FLOAT, FLOAT) -- OMSetDepthBounds */
    FP_SHAPE_DEPTH_BOUNDS,
    /* void (this, FLOAT, FLOAT, FLOAT) -- RSSetDepthBias */
    FP_SHAPE_DEPTH_BIAS,
};

struct d3d12_fp_call_params
{
    UINT64 args[8];   /* integer view of the guest arguments; float
                         positions carry the value's raw bits */
    UINT   slot;
    UINT   shape;     /* enum d3d12_fp_shape */
};

/* Create the unix-side presentation factory (unix_present.c): a host COM
 * object shaped like IDXGIFactory2 whose CreateSwapChainForHwnd builds a
 * vkd3d swapchain presenting through win32u's client-surface machinery
 * (presentation-design.md §4).  Reached from the PE side's D3D12GetInterface
 * intercept; everything after creation flows through unix_call. */
struct d3d12_present_factory_params
{
    UINT64 factory;   /* out: host IDXGIFactory2-shaped interface pointer */
};

/* D3D12_NODE_ID by value (ID3D12WorkGraphProperties::GetNodeIndex /
 * GetEntrypointIndex, 2026-09-01): a { const WCHAR *Name; UINT ArrayIndex; }
 * sixteen-byte aggregate MS-x64 passes by hidden pointer and ELFv2 passes
 * in two GPRs.  The PE side dereferences the guest's hidden pointer and
 * sends the two fields; the unix side calls the host slot through a
 * correctly-typed by-value prototype.  The return is a UINT (a node/
 * entrypoint index, not an HRESULT). */
struct d3d12_nodeid_params
{
    UINT64 name;      /* the aggregate's Name pointer, guest==host memory */
    UINT   arrindex;
    UINT   slot;
    UINT64 host;
    UINT   ret;
};

enum d3d12_unix_func
{
    unix_init,
    unix_call,
    unix_flat,
    unix_present_factory,
    unix_call_fp,
    unix_call_nodeid,   /* appended last: existing ids keep their values */
    unix_funcs_count
};

#define D3D12_UNIX_CALL( code, params ) WINE_UNIX_CALL( unix_ ## code, params )

#endif /* __WINE_D3D12_UNIXLIB_H */
