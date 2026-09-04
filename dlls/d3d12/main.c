/*
 * Native d3d12.dll -- vkd3d-proton's PE front, first client of the shared
 * winecom runtime (libs/winecom).
 *
 * This module REPLACES Wine's d3d12 forwarder into libs/vkd3d.  It is the
 * native half of the native-lane D3D12 stack (design:
 * vkd3d-ppc64le/docs/d3d12-native-lane-design.md §3.3/§3.4):
 *
 *   guest x86-64 PE  -->  C:\windows\sysx8664\d3d12.dll   (spec2thunk COM
 *      |                  mode: pure trap surface, no marshalling knowledge)
 *      |  trap; ntdll's dispatcher maps RIP -> (iface, slot) and calls
 *      v
 *   __wine_com_dispatch( iface, slot, AMD64_CONTEXT * )       <-- THIS FILE
 *      |  = libs/winecom's dispatch loop over the generated marshal table
 *      |  (d3d12_marshal.h, from gen_winecom.py --surface d3d12)
 *      v
 *   d3d12.so (unixlib, unix.c)  -->  libvkd3d-proton-d3d12.so
 *
 * The proxy runtime -- interning, guest-vtable materialisation with the
 * attach IID cross-check, refuse-once, the wrap/refuse choke points, and
 * the dispatch contract with dlls/ntdll/signal_ppc64.c -- was EXTRACTED
 * into libs/winecom (hangover-ppc64le/docs/system-com-design.md §10); what
 * stays here is exactly what is d3d12's own: the unixlib host invoker, the
 * flat entry points, the hand-written slot functions, and the phase-(a)
 * presentation bootstrap.  This module keeps its own PRIVATE runtime
 * instance (static-library state is per-linkee); it shares nothing with
 * combase's system-COM instance.
 *
 * One semantic change came with the shared core: interface-typed IN
 * parameters are translate-in classified rather than blindly unwrapped --
 * an unrecognised pointer used to be dereferenced as a proxy, now it is
 * refused loudly (design §6.3).
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

#include <vkd3d_d3d12.h>

#include "winternl.h"
#include "wine/debug.h"
#include "wine/winecom.h"

#include "unixlib.h"
#include "d3d12_marshal.h"
#include "wine_present.h"

WINE_DEFAULT_DEBUG_CHANNEL(d3d12);

/* ------------------------------------------------------------- unix calls */

static UINT64 unix_vtbl_call( void *host, UINT slot, UINT argc, UINT64 *args )
{
    struct d3d12_call_params p;
    NTSTATUS status;

    memcpy( p.args, args, sizeof(p.args) );
    p.args[0] = (UINT64)(ULONG_PTR)host;
    p.slot = slot;
    p.argc = argc;
    p.ret = 0;
    if ((status = D3D12_UNIX_CALL( call, &p )))
    {
        ERR( "unix call failed, status %08x\n", (UINT)status );
        return (UINT64)(UINT)E_FAIL;
    }
    return p.ret;
}

/* ------------------------------------------------- the runtime instance */

static const WCHAR *const d3d12_guest_modules[] = { L"d3d12.dll" };

static UINT64 hand_resource_barrier( void *host, UINT slot, AMD64_CONTEXT *ctx );
static UINT64 hand_create_compute_pso( void *host, UINT slot, AMD64_CONTEXT *ctx );
static UINT64 hand_create_swapchain_for_hwnd( void *host, UINT slot, AMD64_CONTEXT *ctx );
static UINT64 hand_create_graphics_pso( void *host, UINT slot, AMD64_CONTEXT *ctx );
static UINT64 hand_create_pipeline_state( void *host, UINT slot, AMD64_CONTEXT *ctx );
static UINT64 hand_copy_texture_region( void *host, UINT slot, AMD64_CONTEXT *ctx );
static UINT64 hand_clear_dsv( void *host, UINT slot, AMD64_CONTEXT *ctx );
static UINT64 hand_load_graphics_pipeline( void *host, UINT slot, AMD64_CONTEXT *ctx );
static UINT64 hand_load_compute_pipeline( void *host, UINT slot, AMD64_CONTEXT *ctx );
static UINT64 hand_load_pipeline( void *host, UINT slot, AMD64_CONTEXT *ctx );
static UINT64 hand_om_set_depth_bounds( void *host, UINT slot, AMD64_CONTEXT *ctx );
static UINT64 hand_rs_set_depth_bias( void *host, UINT slot, AMD64_CONTEXT *ctx );
static UINT64 hand_begin_render_pass( void *host, UINT slot, AMD64_CONTEXT *ctx );
static UINT64 hand_barrier_groups( void *host, UINT slot, AMD64_CONTEXT *ctx );
static UINT64 hand_create_state_object( void *host, UINT slot, AMD64_CONTEXT *ctx );
static UINT64 hand_add_to_state_object( void *host, UINT slot, AMD64_CONTEXT *ctx );
static UINT64 hand_node_id_byval( void *host, UINT slot, AMD64_CONTEXT *ctx );
static UINT64 hand_dred_breadcrumbs( void *host, UINT slot, AMD64_CONTEXT *ctx );
static void *com_wrap( void *host, UINT iface );

/* --------------------------------------------------------------------------
 * The 2026-09-01 completeness pass: the WorkGraph 16-byte aggregate and the
 * DRED breadcrumb chain.
 * ------------------------------------------------------------------------ */

/* D3D12_NODE_ID by value: MS-x64 put a HIDDEN POINTER in the argument slot;
 * the fields cross the unixlib flat and the unix side calls the real
 * by-value prototype (unix.c).  Returns UINT (an index, not an HRESULT). */
static UINT64 hand_node_id_byval( void *host, UINT slot, AMD64_CONTEXT *ctx )
{
    struct d3d12_nodeid_params p;
    const struct { const WCHAR *name; UINT arrindex; } *id =
        (const void *)(ULONG_PTR)winecom_read_arg( ctx, 1 );
    NTSTATUS status;

    p.name = id ? (UINT64)(ULONG_PTR)id->name : 0;
    p.arrindex = id ? id->arrindex : 0;
    p.slot = slot;
    p.host = (UINT64)(ULONG_PTR)host;
    p.ret = 0;
    if ((status = D3D12_UNIX_CALL( call_nodeid, &p )))
    {
        ERR( "nodeid call failed, status %#x\n", (UINT)status );
        return 0;
    }
    return p.ret;
}

/* DRED's breadcrumb chain: native-owned nodes carrying a command list and a
 * queue per node.  Mutating DRED's own list would corrupt native state, so
 * the chain is DEEP-COPIED with the two interfaces wrapped per node; the
 * copy is freed at the NEXT call, which is DRED's own post-mortem usage
 * shape (documented lifetime: valid until the next GetAutoBreadcrumbsOutput).
 * The node layout is transcribed from the D3D12 SDK headers -- vkd3d builds
 * against the same ABI. */
struct dred_node
{
    const char *list_name_a;
    const WCHAR *list_name_w;
    const char *queue_name_a;
    const WCHAR *queue_name_w;
    void *command_list;
    void *command_queue;
    UINT breadcrumb_count;
    const UINT *last_value;
    const UINT *history;
    const struct dred_node *next;
};

struct dred_output { const struct dred_node *head; };

static struct dred_node *dred_prev_copy;

static UINT64 hand_dred_breadcrumbs( void *host, UINT slot, AMD64_CONTEXT *ctx )
{
    struct dred_output *out = (struct dred_output *)(ULONG_PTR)winecom_read_arg( ctx, 1 );
    UINT64 args[16] = { 0 };
    UINT64 ret;
    const struct dred_node *n;
    struct dred_node *copy = NULL, **tail = &copy, *old;
    UINT count = 0;

    args[1] = (UINT64)(ULONG_PTR)out;
    ret = unix_vtbl_call( host, slot, 2, args );
    if (FAILED((HRESULT)(UINT)ret) || !out || !out->head) return ret;

    for (n = out->head; n && count < 4096; n = n->next, count++)
    {
        struct dred_node *c = RtlAllocateHeap( GetProcessHeap(), 0, sizeof(*c) );

        if (!c) break;
        *c = *n;
        /* the chain LENDS its pointers and com_wrap CONSUMES a reference:
         * take one first (slot 1 is AddRef on every COM vtable), so DRED's
         * own release balance is untouched and the interned proxy owns
         * what it holds -- the dinput8 shim's rule, same reason. */
        if (c->command_list)
        {
            UINT64 a[2] = { 0 };
            unix_vtbl_call( c->command_list, 1, 1, a );
            c->command_list = com_wrap( c->command_list,
                                        D3D12_IFACE_ID3D12GraphicsCommandList );
        }
        if (c->command_queue)
        {
            UINT64 a[2] = { 0 };
            unix_vtbl_call( c->command_queue, 1, 1, a );
            c->command_queue = com_wrap( c->command_queue,
                                         D3D12_IFACE_ID3D12CommandQueue );
        }
        c->next = NULL;
        *tail = c;
        tail = (struct dred_node **)&c->next;
    }
    old = InterlockedExchangePointer( (void **)&dred_prev_copy, copy );
    while (old)
    {
        struct dred_node *next = (struct dred_node *)old->next;
        RtlFreeHeap( GetProcessHeap(), 0, old );
        old = next;
    }
    out->head = copy;
    return ret;
}

/* Order is the generated header's hand_funcs[] order -- see the
 * "hand_funcs[] order" comment ppc64le/vkd3d/gen_winecom.py emits there. */
static const winecom_hand_fn d3d12_hand_funcs[] =
{
    hand_resource_barrier,
    hand_create_compute_pso,
    hand_create_swapchain_for_hwnd,
    hand_create_graphics_pso,
    hand_create_pipeline_state,
    hand_copy_texture_region,
    hand_clear_dsv,
    hand_load_graphics_pipeline,
    hand_load_compute_pipeline,
    hand_load_pipeline,
    hand_om_set_depth_bounds,
    hand_rs_set_depth_bias,
    hand_begin_render_pass,
    hand_barrier_groups,
    hand_create_state_object,
    hand_add_to_state_object,
    hand_node_id_byval,
    hand_dred_breadcrumbs,
};

C_ASSERT( ARRAYSIZE(d3d12_hand_funcs) == D3D12_HAND_COUNT );

static const struct winecom_surface d3d12_surface =
{
    .name = "d3d12",
    .guest_modules = d3d12_guest_modules,
    .module_count = ARRAYSIZE(d3d12_guest_modules),
    .ifaces = d3d12_com_ifaces,
    .iface_count = D3D12_IFACE_COUNT,
    .invoke = unix_vtbl_call,
    .hand_funcs = d3d12_hand_funcs,
    .hand_count = D3D12_HAND_COUNT,
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
    if (D3D12_UNIX_CALL( init, NULL ) || !winecom_attach( &d3d12_surface ))
    {
        InterlockedExchange( &com_init_state, 3 );
        return FALSE;
    }
    InterlockedExchange( &com_init_state, 2 );
    return TRUE;
}

/* short spellings for the hand-written slots below */
static UINT64 read_arg( const AMD64_CONTEXT *ctx, UINT n )
{
    return winecom_read_arg( ctx, n );
}

static void *com_unwrap( void *proxy )
{
    return winecom_unwrap( proxy );
}

static void *com_wrap( void *host, UINT iface )
{
    return winecom_wrap( host, iface );
}

NTSTATUS WINAPI __wine_com_dispatch( UINT iface, UINT slot, AMD64_CONTEXT *ctx )
{
    if (!com_runtime_init()) return STATUS_DLL_INIT_FAILED;
    return winecom_dispatch( iface, slot, ctx );
}

/* The crossing-frequency sink's name lookup; see winecom_slot_names.  Never
 * on a dispatch path -- ntdll asks once per slot, when it interns the row. */
BOOL WINAPI __wine_com_slot_name( UINT iface, UINT slot, const char **iface_name,
                                  const char **slot_name )
{
    return winecom_slot_names( iface, slot, iface_name, slot_name );
}

/* ----------------------------------------------------- hand-written slots */

/* ID3D12GraphicsCommandList::ResourceBarrier( UINT n, const
 * D3D12_RESOURCE_BARRIER *barriers ): the barrier structs carry
 * ID3D12Resource* members (directly and through unions), so the array is
 * copied shallowly and the resource pointers unwrapped in the copy. */
static UINT64 hand_resource_barrier( void *host, UINT slot, AMD64_CONTEXT *ctx )
{
    UINT n = (UINT)read_arg( ctx, 1 );
    const D3D12_RESOURCE_BARRIER *src = (const void *)(ULONG_PTR)read_arg( ctx, 2 );
    UINT64 args[D3D12_UNIX_MAX_ARGS] = { 0 };
    D3D12_RESOURCE_BARRIER *copy = NULL;
    UINT64 ret;
    UINT i;

    if (n && src)
    {
        if (!(copy = RtlAllocateHeap( NtCurrentTeb()->Peb->ProcessHeap, 0,
                                      n * sizeof(*copy) )))
            return (UINT64)(UINT)E_OUTOFMEMORY;
        memcpy( copy, src, n * sizeof(*copy) );
        for (i = 0; i < n; i++)
        {
            switch (copy[i].Type)
            {
            case D3D12_RESOURCE_BARRIER_TYPE_TRANSITION:
                copy[i].Transition.pResource = com_unwrap( copy[i].Transition.pResource );
                break;
            case D3D12_RESOURCE_BARRIER_TYPE_ALIASING:
                copy[i].Aliasing.pResourceBefore = com_unwrap( copy[i].Aliasing.pResourceBefore );
                copy[i].Aliasing.pResourceAfter = com_unwrap( copy[i].Aliasing.pResourceAfter );
                break;
            case D3D12_RESOURCE_BARRIER_TYPE_UAV:
                copy[i].UAV.pResource = com_unwrap( copy[i].UAV.pResource );
                break;
            default:
                WARN( "unknown barrier type %u passed through\n", copy[i].Type );
                break;
            }
        }
    }
    args[1] = n;
    args[2] = (UINT64)(ULONG_PTR)(copy ? copy : src);
    ret = unix_vtbl_call( host, slot, 3, args );
    if (copy) RtlFreeHeap( NtCurrentTeb()->Peb->ProcessHeap, 0, copy );
    return ret;   /* void method; RAX is scratch */
}

/* ID3D12GraphicsCommandList::ClearDepthStencilView(
 * D3D12_CPU_DESCRIPTOR_HANDLE dsv, D3D12_CLEAR_FLAGS flags, FLOAT depth,
 * UINT8 stencil, UINT num_rects, const D3D12_RECT *rects ): the FLOAT is
 * why this is a hand slot.  MS-x64 put Depth in XMM3 (argument position 3),
 * where the integer-wide invoker cannot see it, and the refusal this
 * replaces silently cost every frame its depth clear -- the whole scene
 * then rendered against stale depth ([MEASURED] 2026-08-19, Cyberpunk 2077
 * -benchmark: dense screen-space speckle, gone with this walker).  The
 * value crosses as raw bits and the unixlib's typed-float call
 * (FP_SHAPE_CLEAR_DSV) reconstitutes it. */
static UINT64 hand_clear_dsv( void *host, UINT slot, AMD64_CONTEXT *ctx )
{
    struct d3d12_fp_call_params p = { { 0 } };
    NTSTATUS status;

    p.args[0] = (UINT64)(ULONG_PTR)host;
    p.args[1] = read_arg( ctx, 1 );                       /* the DSV handle */
    p.args[2] = (UINT)read_arg( ctx, 2 );                 /* D3D12_CLEAR_FLAGS */
    __wine_emu_materialize_ctx( ctx );   /* lazy-ctx contract, wine/winecom.h */
    p.args[3] = ctx->FltSave.XmmRegisters[3].Low & 0xffffffffu;  /* Depth */
    p.args[4] = (BYTE)read_arg( ctx, 4 );                 /* Stencil */
    p.args[5] = (UINT)read_arg( ctx, 5 );                 /* NumRects */
    p.args[6] = read_arg( ctx, 6 );                       /* pRects */
    p.slot = slot;
    p.shape = FP_SHAPE_CLEAR_DSV;
    if ((status = D3D12_UNIX_CALL( call_fp, &p )))
        ERR( "unix call_fp failed, status %08x\n", (UINT)status );
    return 0;   /* void method; RAX is scratch */
}

/* ID3D12Device::CreateComputePipelineState( const
 * D3D12_COMPUTE_PIPELINE_STATE_DESC *desc, REFIID riid, void **ppv ):
 * the desc carries ID3D12RootSignature *pRootSignature. */
/* REFUSAL HYGIENE, BY HAND, because no generated scrub mask reaches a
 * WINECOM_F_HAND row: a hand walker that refuses owns its out-params the way
 * scrub_refused_outs() owns a table refusal's, and an unwritten *ppv is stack
 * residue the caller calls through.  [MEASURED] dlls/combase/syscom.c's
 * IMMDevice::Activate cost days exactly so: the Witcher 3 read a never-written
 * *ppv and the emulator decoded a host module's ppc64le bytes as x86.  This
 * file has NO E_NOTIMPL sites -- its refusals wear other spellings
 * (E_INVALIDARG for an argument shape it cannot walk, E_OUTOFMEMORY for its
 * own copy) and they are refusals all the same.  The write goes through
 * winecom_refused_scrub_ptr, which honours WINEEMUNOREFUSESCRUB so the hygiene
 * gate can prove it load-bearing.  A NATIVE failure stays untouched, and the
 * *ppv = NULL that follows a winecom_host_release below is NOT this: that one
 * is mandatory memory safety and must run whatever the lever says. */
static void refuse_scrub_ppv( void **ppv )
{
    winecom_refused_scrub_ptr( ppv );
}

static UINT64 hand_create_compute_pso( void *host, UINT slot, AMD64_CONTEXT *ctx )
{
    const D3D12_COMPUTE_PIPELINE_STATE_DESC *src = (const void *)(ULONG_PTR)read_arg( ctx, 1 );
    const GUID *riid = (const GUID *)(ULONG_PTR)read_arg( ctx, 2 );
    void **ppv = (void **)(ULONG_PTR)read_arg( ctx, 3 );
    D3D12_COMPUTE_PIPELINE_STATE_DESC desc;
    UINT64 args[D3D12_UNIX_MAX_ARGS] = { 0 };
    HRESULT hr;
    UINT idx;

    if (!src || !ppv)
    {
        refuse_scrub_ppv( ppv );          /* refusal hygiene by hand */
        return (UINT64)(UINT)E_INVALIDARG;
    }
    desc = *src;
    desc.pRootSignature = com_unwrap( desc.pRootSignature );
    args[1] = (UINT64)(ULONG_PTR)&desc;
    args[2] = (UINT64)(ULONG_PTR)riid;
    args[3] = (UINT64)(ULONG_PTR)ppv;
    hr = (HRESULT)unix_vtbl_call( host, slot, 4, args );
    if (SUCCEEDED(hr) && *ppv)
    {
        idx = winecom_iface_from_iid( riid );
        if (idx == ~0u)
        {
            ERR( "CreateComputePipelineState returned unknown IID %s\n",
                 debugstr_guid(riid) );
            winecom_host_release( *ppv );
            *ppv = NULL;
            return (UINT64)(UINT)E_NOINTERFACE;
        }
        *ppv = com_wrap( *ppv, idx );
    }
    return (UINT64)(UINT)hr;
}

/* IDXGIFactory2::CreateSwapChainForHwnd( IUnknown *device, HWND hwnd,
 * const DXGI_SWAP_CHAIN_DESC1 *desc, const DXGI_SWAP_CHAIN_FULLSCREEN_DESC
 * *fs_desc, IDXGIOutput *restrict_output, IDXGISwapChain1 **out ):
 * IDXGISwapChain1** carries no REFIID, so the wrap type is fixed here; the
 * device (an ID3D12CommandQueue proxy) unwraps going in.  The host factory
 * is the unixlib's own present factory (unix_present.c). */
static UINT64 hand_create_swapchain_for_hwnd( void *host, UINT slot, AMD64_CONTEXT *ctx )
{
    void *device_proxy = (void *)(ULONG_PTR)read_arg( ctx, 1 );
    void *restrict_proxy = (void *)(ULONG_PTR)read_arg( ctx, 5 );
    void **out = (void **)(ULONG_PTR)read_arg( ctx, 6 );
    UINT64 args[D3D12_UNIX_MAX_ARGS] = { 0 };
    void *host_out = NULL;
    HRESULT hr;

    if (!out) return (UINT64)(UINT)E_INVALIDARG;
    *out = NULL;
    args[1] = (UINT64)(ULONG_PTR)com_unwrap( device_proxy );
    args[2] = read_arg( ctx, 2 );   /* HWND */
    args[3] = read_arg( ctx, 3 );   /* desc, no interface members */
    args[4] = read_arg( ctx, 4 );   /* fullscreen desc, ditto */
    args[5] = (UINT64)(ULONG_PTR)com_unwrap( restrict_proxy );
    args[6] = (UINT64)(ULONG_PTR)&host_out;
    hr = (HRESULT)unix_vtbl_call( host, slot, 7, args );
    if (SUCCEEDED(hr) && host_out)
    {
        if (!(*out = com_wrap( host_out, D3D12_IFACE_IDXGISwapChain1 )))
            return (UINT64)(UINT)E_OUTOFMEMORY;
    }
    return (UINT64)(UINT)hr;
}

/* ID3D12Device::CreateGraphicsPipelineState( const
 * D3D12_GRAPHICS_PIPELINE_STATE_DESC *desc, REFIID riid, void **ppv ):
 * the desc's one interface member is pRootSignature -- every other pointer
 * in it (shader bytecode, input layout, cached PSO) is plain data. */
static UINT64 hand_create_graphics_pso( void *host, UINT slot, AMD64_CONTEXT *ctx )
{
    const D3D12_GRAPHICS_PIPELINE_STATE_DESC *src = (const void *)(ULONG_PTR)read_arg( ctx, 1 );
    const GUID *riid = (const GUID *)(ULONG_PTR)read_arg( ctx, 2 );
    void **ppv = (void **)(ULONG_PTR)read_arg( ctx, 3 );
    D3D12_GRAPHICS_PIPELINE_STATE_DESC desc;
    UINT64 args[D3D12_UNIX_MAX_ARGS] = { 0 };
    HRESULT hr;
    UINT idx;

    if (!src || !ppv)
    {
        refuse_scrub_ppv( ppv );          /* refusal hygiene by hand */
        return (UINT64)(UINT)E_INVALIDARG;
    }
    desc = *src;
    desc.pRootSignature = com_unwrap( desc.pRootSignature );
    args[1] = (UINT64)(ULONG_PTR)&desc;
    args[2] = (UINT64)(ULONG_PTR)riid;
    args[3] = (UINT64)(ULONG_PTR)ppv;
    hr = (HRESULT)unix_vtbl_call( host, slot, 4, args );
    if (SUCCEEDED(hr) && *ppv)
    {
        idx = winecom_iface_from_iid( riid );
        if (idx == ~0u)
        {
            ERR( "CreateGraphicsPipelineState returned unknown IID %s\n",
                 debugstr_guid(riid) );
            winecom_host_release( *ppv );
            *ppv = NULL;
            return (UINT64)(UINT)E_NOINTERFACE;
        }
        *ppv = com_wrap( *ppv, idx );
    }
    return (UINT64)(UINT)hr;
}

/* A D3D12_PIPELINE_STATE_STREAM_DESC is a stream of subobjects --
 * a type enum, a naturally-aligned payload, a stride padded to pointer size,
 * the exact layout vkd3d's own
 * vkd3d_pipeline_state_desc_from_d3d12_stream_desc walks -- and the
 * ROOT_SIGNATURE payload is a guest proxy.  The stream is copied and the
 * proxy unwrapped in the copy; a subobject type the pinned vkd3d headers do
 * not name stops the walk, because a cursor that cannot advance cannot
 * prove the rest of the stream carries no proxies.  Shared by
 * hand_create_pipeline_state and hand_load_pipeline; on success the desc
 * points at the heap copy returned through *copy_out (NULL for an empty
 * stream) and the caller frees it after the host call. */
static HRESULT pso_stream_unwrap( D3D12_PIPELINE_STATE_STREAM_DESC *desc, char **copy_out )
{
    char *copy, *ptr, *end;

    *copy_out = NULL;
    if (!desc->SizeInBytes || !desc->pPipelineStateSubobjectStream) return S_OK;
    if (!(copy = RtlAllocateHeap( NtCurrentTeb()->Peb->ProcessHeap, 0,
                                  desc->SizeInBytes )))
        return E_OUTOFMEMORY;
    memcpy( copy, desc->pPipelineStateSubobjectStream, desc->SizeInBytes );
    desc->pPipelineStateSubobjectStream = copy;

    {
        ptr = copy;
        end = copy + desc->SizeInBytes;
#define ALIGN_PTR(x) (((x) + sizeof(void *) - 1) & ~(sizeof(void *) - 1))
#define WALK_SUBOBJECT(type_enum, type_name, fixup)                          \
        case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_ ## type_enum:              \
        {                                                                    \
            struct                                                           \
            {                                                                \
                D3D12_PIPELINE_STATE_SUBOBJECT_TYPE type;                    \
                type_name data;                                              \
            } *so = (void *)ptr;                                             \
            if (ptr + sizeof(*so) > end) goto malformed;                     \
            fixup;                                                           \
            ptr += ALIGN_PTR( sizeof(*so) );                                 \
            break;                                                           \
        }
        while (ptr < end)
        {
            if (ptr + sizeof(D3D12_PIPELINE_STATE_SUBOBJECT_TYPE) > end) goto malformed;
            switch (*(const D3D12_PIPELINE_STATE_SUBOBJECT_TYPE *)ptr)
            {
            WALK_SUBOBJECT( ROOT_SIGNATURE, ID3D12RootSignature *,
                            so->data = com_unwrap( so->data ) )
            WALK_SUBOBJECT( VS, D3D12_SHADER_BYTECODE, (void)0 )
            WALK_SUBOBJECT( PS, D3D12_SHADER_BYTECODE, (void)0 )
            WALK_SUBOBJECT( DS, D3D12_SHADER_BYTECODE, (void)0 )
            WALK_SUBOBJECT( HS, D3D12_SHADER_BYTECODE, (void)0 )
            WALK_SUBOBJECT( GS, D3D12_SHADER_BYTECODE, (void)0 )
            WALK_SUBOBJECT( CS, D3D12_SHADER_BYTECODE, (void)0 )
            WALK_SUBOBJECT( AS, D3D12_SHADER_BYTECODE, (void)0 )
            WALK_SUBOBJECT( MS, D3D12_SHADER_BYTECODE, (void)0 )
            WALK_SUBOBJECT( STREAM_OUTPUT, D3D12_STREAM_OUTPUT_DESC, (void)0 )
            WALK_SUBOBJECT( BLEND, D3D12_BLEND_DESC, (void)0 )
            WALK_SUBOBJECT( SAMPLE_MASK, UINT, (void)0 )
            WALK_SUBOBJECT( RASTERIZER, D3D12_RASTERIZER_DESC, (void)0 )
            WALK_SUBOBJECT( RASTERIZER1, D3D12_RASTERIZER_DESC1, (void)0 )
            WALK_SUBOBJECT( RASTERIZER2, D3D12_RASTERIZER_DESC2, (void)0 )
            WALK_SUBOBJECT( DEPTH_STENCIL, D3D12_DEPTH_STENCIL_DESC, (void)0 )
            WALK_SUBOBJECT( DEPTH_STENCIL1, D3D12_DEPTH_STENCIL_DESC1, (void)0 )
            WALK_SUBOBJECT( DEPTH_STENCIL2, D3D12_DEPTH_STENCIL_DESC2, (void)0 )
            WALK_SUBOBJECT( INPUT_LAYOUT, D3D12_INPUT_LAYOUT_DESC, (void)0 )
            WALK_SUBOBJECT( IB_STRIP_CUT_VALUE, D3D12_INDEX_BUFFER_STRIP_CUT_VALUE, (void)0 )
            WALK_SUBOBJECT( PRIMITIVE_TOPOLOGY, D3D12_PRIMITIVE_TOPOLOGY_TYPE, (void)0 )
            WALK_SUBOBJECT( RENDER_TARGET_FORMATS, D3D12_RT_FORMAT_ARRAY, (void)0 )
            WALK_SUBOBJECT( DEPTH_STENCIL_FORMAT, DXGI_FORMAT, (void)0 )
            WALK_SUBOBJECT( SAMPLE_DESC, DXGI_SAMPLE_DESC, (void)0 )
            WALK_SUBOBJECT( NODE_MASK, UINT, (void)0 )
            WALK_SUBOBJECT( CACHED_PSO, D3D12_CACHED_PIPELINE_STATE, (void)0 )
            WALK_SUBOBJECT( FLAGS, D3D12_PIPELINE_STATE_FLAGS, (void)0 )
            WALK_SUBOBJECT( VIEW_INSTANCING, D3D12_VIEW_INSTANCING_DESC, (void)0 )
            default:
                WARN( "unknown pipeline subobject type %u; cannot walk past it\n",
                      *(const UINT *)ptr );
                goto malformed;
            }
        }
#undef WALK_SUBOBJECT
#undef ALIGN_PTR
    }
    *copy_out = copy;
    return S_OK;

malformed:
    ERR( "malformed pipeline state stream (%Iu bytes)\n", desc->SizeInBytes );
    RtlFreeHeap( NtCurrentTeb()->Peb->ProcessHeap, 0, copy );
    return E_INVALIDARG;
}

/* ID3D12Device2::CreatePipelineState( const D3D12_PIPELINE_STATE_STREAM_DESC
 * *desc, REFIID riid, void **ppv ) -- pso_stream_unwrap above is the
 * walker. */
static UINT64 hand_create_pipeline_state( void *host, UINT slot, AMD64_CONTEXT *ctx )
{
    const D3D12_PIPELINE_STATE_STREAM_DESC *src = (const void *)(ULONG_PTR)read_arg( ctx, 1 );
    const GUID *riid = (const GUID *)(ULONG_PTR)read_arg( ctx, 2 );
    void **ppv = (void **)(ULONG_PTR)read_arg( ctx, 3 );
    D3D12_PIPELINE_STATE_STREAM_DESC desc;
    UINT64 args[D3D12_UNIX_MAX_ARGS] = { 0 };
    char *copy;
    HRESULT hr;

    if (!src || !ppv)
    {
        refuse_scrub_ppv( ppv );          /* refusal hygiene by hand */
        return (UINT64)(UINT)E_INVALIDARG;
    }
    desc = *src;
    if (FAILED(hr = pso_stream_unwrap( &desc, &copy )))
    {
        refuse_scrub_ppv( ppv );          /* refusal hygiene by hand */
        return (UINT64)(UINT)hr;
    }
    args[1] = (UINT64)(ULONG_PTR)&desc;
    args[2] = (UINT64)(ULONG_PTR)riid;
    args[3] = (UINT64)(ULONG_PTR)ppv;
    hr = (HRESULT)unix_vtbl_call( host, slot, 4, args );
    if (copy) RtlFreeHeap( NtCurrentTeb()->Peb->ProcessHeap, 0, copy );
    return (UINT64)(UINT)winecom_wrap_out_iface( hr, riid, ppv );
}

/* ID3D12GraphicsCommandList::CopyTextureRegion( const
 * D3D12_TEXTURE_COPY_LOCATION *dst, UINT x, UINT y, UINT z, const
 * D3D12_TEXTURE_COPY_LOCATION *src, const D3D12_BOX *box ): each location's
 * pResource is a proxy; the union behind it is plain data. */
static UINT64 hand_copy_texture_region( void *host, UINT slot, AMD64_CONTEXT *ctx )
{
    const D3D12_TEXTURE_COPY_LOCATION *dst = (const void *)(ULONG_PTR)read_arg( ctx, 1 );
    const D3D12_TEXTURE_COPY_LOCATION *src = (const void *)(ULONG_PTR)read_arg( ctx, 5 );
    D3D12_TEXTURE_COPY_LOCATION dst_copy, src_copy;
    UINT64 args[D3D12_UNIX_MAX_ARGS] = { 0 };

    if (dst)
    {
        dst_copy = *dst;
        dst_copy.pResource = com_unwrap( dst_copy.pResource );
    }
    if (src)
    {
        src_copy = *src;
        src_copy.pResource = com_unwrap( src_copy.pResource );
    }
    args[1] = (UINT64)(ULONG_PTR)(dst ? &dst_copy : NULL);
    args[2] = read_arg( ctx, 2 );
    args[3] = read_arg( ctx, 3 );
    /* dst_z is a UINT in the fifth slot -- a STACK argument, so the guest's
     * 32-bit store left the slot's upper half stale (winecom_slot::dwordmask
     * is the table-driven form of this same extension). */
    args[4] = (UINT)read_arg( ctx, 4 );
    args[5] = (UINT64)(ULONG_PTR)(src ? &src_copy : NULL);
    args[6] = read_arg( ctx, 6 );
    return unix_vtbl_call( host, slot, 7, args );   /* void method; RAX is scratch */
}

/* ID3D12PipelineLibrary::LoadGraphicsPipeline( LPCWSTR name, const
 * D3D12_GRAPHICS_PIPELINE_STATE_DESC *desc, REFIID riid, void **ppv ):
 * CreateGraphicsPipelineState's desc walk, one name earlier in the frame.
 * A name the host library does not hold answers E_INVALIDARG and the
 * caller falls back to full PSO creation -- serving the slot is what
 * makes that fallback reachable. */
static UINT64 hand_load_graphics_pipeline( void *host, UINT slot, AMD64_CONTEXT *ctx )
{
    const WCHAR *name = (const WCHAR *)(ULONG_PTR)read_arg( ctx, 1 );
    const D3D12_GRAPHICS_PIPELINE_STATE_DESC *src = (const void *)(ULONG_PTR)read_arg( ctx, 2 );
    const GUID *riid = (const GUID *)(ULONG_PTR)read_arg( ctx, 3 );
    void **ppv = (void **)(ULONG_PTR)read_arg( ctx, 4 );
    D3D12_GRAPHICS_PIPELINE_STATE_DESC desc;
    UINT64 args[D3D12_UNIX_MAX_ARGS] = { 0 };
    HRESULT hr;

    if (!src || !ppv)
    {
        refuse_scrub_ppv( ppv );          /* refusal hygiene by hand */
        return (UINT64)(UINT)E_INVALIDARG;
    }
    desc = *src;
    desc.pRootSignature = com_unwrap( desc.pRootSignature );
    args[1] = (UINT64)(ULONG_PTR)name;
    args[2] = (UINT64)(ULONG_PTR)&desc;
    args[3] = (UINT64)(ULONG_PTR)riid;
    args[4] = (UINT64)(ULONG_PTR)ppv;
    hr = (HRESULT)unix_vtbl_call( host, slot, 5, args );
    return (UINT64)(UINT)winecom_wrap_out_iface( hr, riid, ppv );
}

/* ID3D12PipelineLibrary::LoadComputePipeline: the compute twin of the
 * slot above. */
static UINT64 hand_load_compute_pipeline( void *host, UINT slot, AMD64_CONTEXT *ctx )
{
    const WCHAR *name = (const WCHAR *)(ULONG_PTR)read_arg( ctx, 1 );
    const D3D12_COMPUTE_PIPELINE_STATE_DESC *src = (const void *)(ULONG_PTR)read_arg( ctx, 2 );
    const GUID *riid = (const GUID *)(ULONG_PTR)read_arg( ctx, 3 );
    void **ppv = (void **)(ULONG_PTR)read_arg( ctx, 4 );
    D3D12_COMPUTE_PIPELINE_STATE_DESC desc;
    UINT64 args[D3D12_UNIX_MAX_ARGS] = { 0 };
    HRESULT hr;

    if (!src || !ppv)
    {
        refuse_scrub_ppv( ppv );          /* refusal hygiene by hand */
        return (UINT64)(UINT)E_INVALIDARG;
    }
    desc = *src;
    desc.pRootSignature = com_unwrap( desc.pRootSignature );
    args[1] = (UINT64)(ULONG_PTR)name;
    args[2] = (UINT64)(ULONG_PTR)&desc;
    args[3] = (UINT64)(ULONG_PTR)riid;
    args[4] = (UINT64)(ULONG_PTR)ppv;
    hr = (HRESULT)unix_vtbl_call( host, slot, 5, args );
    return (UINT64)(UINT)winecom_wrap_out_iface( hr, riid, ppv );
}

/* ID3D12PipelineLibrary1::LoadPipeline( LPCWSTR name, const
 * D3D12_PIPELINE_STATE_STREAM_DESC *desc, REFIID riid, void **ppv ):
 * CreatePipelineState's stream walk behind a library name. */
static UINT64 hand_load_pipeline( void *host, UINT slot, AMD64_CONTEXT *ctx )
{
    const WCHAR *name = (const WCHAR *)(ULONG_PTR)read_arg( ctx, 1 );
    const D3D12_PIPELINE_STATE_STREAM_DESC *src = (const void *)(ULONG_PTR)read_arg( ctx, 2 );
    const GUID *riid = (const GUID *)(ULONG_PTR)read_arg( ctx, 3 );
    void **ppv = (void **)(ULONG_PTR)read_arg( ctx, 4 );
    D3D12_PIPELINE_STATE_STREAM_DESC desc;
    UINT64 args[D3D12_UNIX_MAX_ARGS] = { 0 };
    char *copy;
    HRESULT hr;

    if (!src || !ppv)
    {
        refuse_scrub_ppv( ppv );          /* refusal hygiene by hand */
        return (UINT64)(UINT)E_INVALIDARG;
    }
    desc = *src;
    if (FAILED(hr = pso_stream_unwrap( &desc, &copy )))
    {
        refuse_scrub_ppv( ppv );          /* refusal hygiene by hand */
        return (UINT64)(UINT)hr;
    }
    args[1] = (UINT64)(ULONG_PTR)name;
    args[2] = (UINT64)(ULONG_PTR)&desc;
    args[3] = (UINT64)(ULONG_PTR)riid;
    args[4] = (UINT64)(ULONG_PTR)ppv;
    hr = (HRESULT)unix_vtbl_call( host, slot, 5, args );
    if (copy) RtlFreeHeap( NtCurrentTeb()->Peb->ProcessHeap, 0, copy );
    return (UINT64)(UINT)winecom_wrap_out_iface( hr, riid, ppv );
}

/* ID3D12GraphicsCommandList1::OMSetDepthBounds( FLOAT min, FLOAT max ):
 * an all-float frame, the same wrong-register-file problem
 * ClearDepthStencilView had -- MS-x64 put the values in XMM1/XMM2 where
 * the integer-wide invoker cannot see them.  The raw bits cross and the
 * unixlib's typed call (FP_SHAPE_DEPTH_BOUNDS) reconstitutes them. */
static UINT64 hand_om_set_depth_bounds( void *host, UINT slot, AMD64_CONTEXT *ctx )
{
    struct d3d12_fp_call_params p = { { 0 } };
    NTSTATUS status;

    p.args[0] = (UINT64)(ULONG_PTR)host;
    __wine_emu_materialize_ctx( ctx );   /* lazy-ctx contract, wine/winecom.h */
    p.args[1] = ctx->FltSave.XmmRegisters[1].Low & 0xffffffffu;   /* Min */
    p.args[2] = ctx->FltSave.XmmRegisters[2].Low & 0xffffffffu;   /* Max */
    p.slot = slot;
    p.shape = FP_SHAPE_DEPTH_BOUNDS;
    if ((status = D3D12_UNIX_CALL( call_fp, &p )))
        ERR( "unix call_fp failed, status %08x\n", (UINT)status );
    return 0;   /* void method; RAX is scratch */
}

/* ID3D12GraphicsCommandList9::RSSetDepthBias( FLOAT bias, FLOAT clamp,
 * FLOAT slope_scaled_bias ): XMM1..XMM3, same shape as the slot above. */
static UINT64 hand_rs_set_depth_bias( void *host, UINT slot, AMD64_CONTEXT *ctx )
{
    struct d3d12_fp_call_params p = { { 0 } };
    NTSTATUS status;

    p.args[0] = (UINT64)(ULONG_PTR)host;
    __wine_emu_materialize_ctx( ctx );   /* lazy-ctx contract, wine/winecom.h */
    p.args[1] = ctx->FltSave.XmmRegisters[1].Low & 0xffffffffu;   /* DepthBias */
    p.args[2] = ctx->FltSave.XmmRegisters[2].Low & 0xffffffffu;   /* DepthBiasClamp */
    p.args[3] = ctx->FltSave.XmmRegisters[3].Low & 0xffffffffu;   /* SlopeScaledDepthBias */
    p.slot = slot;
    p.shape = FP_SHAPE_DEPTH_BIAS;
    if ((status = D3D12_UNIX_CALL( call_fp, &p )))
        ERR( "unix call_fp failed, status %08x\n", (UINT)status );
    return 0;   /* void method; RAX is scratch */
}

/* ID3D12GraphicsCommandList4::BeginRenderPass( UINT n, const
 * D3D12_RENDER_PASS_RENDER_TARGET_DESC *rts, const
 * D3D12_RENDER_PASS_DEPTH_STENCIL_DESC *ds, D3D12_RENDER_PASS_FLAGS flags ):
 * the only interface members are the resolve source/dest resources inside
 * each ENDING_ACCESS -- a BeginningAccess carries clear values only. */
static UINT64 hand_begin_render_pass( void *host, UINT slot, AMD64_CONTEXT *ctx )
{
    UINT n = (UINT)read_arg( ctx, 1 );
    const D3D12_RENDER_PASS_RENDER_TARGET_DESC *rts = (const void *)(ULONG_PTR)read_arg( ctx, 2 );
    const D3D12_RENDER_PASS_DEPTH_STENCIL_DESC *ds = (const void *)(ULONG_PTR)read_arg( ctx, 3 );
    D3D12_RENDER_PASS_DEPTH_STENCIL_DESC ds_copy;
    D3D12_RENDER_PASS_RENDER_TARGET_DESC *copy = NULL;
    UINT64 args[D3D12_UNIX_MAX_ARGS] = { 0 };
    UINT64 ret;
    UINT i;

    if (n && rts)
    {
        if (!(copy = RtlAllocateHeap( NtCurrentTeb()->Peb->ProcessHeap, 0,
                                      n * sizeof(*copy) )))
            return (UINT64)(UINT)E_OUTOFMEMORY;
        memcpy( copy, rts, n * sizeof(*copy) );
        for (i = 0; i < n; i++)
            if (copy[i].EndingAccess.Type == D3D12_RENDER_PASS_ENDING_ACCESS_TYPE_RESOLVE)
            {
                copy[i].EndingAccess.Resolve.pSrcResource =
                    com_unwrap( copy[i].EndingAccess.Resolve.pSrcResource );
                copy[i].EndingAccess.Resolve.pDstResource =
                    com_unwrap( copy[i].EndingAccess.Resolve.pDstResource );
            }
    }
    if (ds)
    {
        ds_copy = *ds;
        if (ds_copy.DepthEndingAccess.Type == D3D12_RENDER_PASS_ENDING_ACCESS_TYPE_RESOLVE)
        {
            ds_copy.DepthEndingAccess.Resolve.pSrcResource =
                com_unwrap( ds_copy.DepthEndingAccess.Resolve.pSrcResource );
            ds_copy.DepthEndingAccess.Resolve.pDstResource =
                com_unwrap( ds_copy.DepthEndingAccess.Resolve.pDstResource );
        }
        if (ds_copy.StencilEndingAccess.Type == D3D12_RENDER_PASS_ENDING_ACCESS_TYPE_RESOLVE)
        {
            ds_copy.StencilEndingAccess.Resolve.pSrcResource =
                com_unwrap( ds_copy.StencilEndingAccess.Resolve.pSrcResource );
            ds_copy.StencilEndingAccess.Resolve.pDstResource =
                com_unwrap( ds_copy.StencilEndingAccess.Resolve.pDstResource );
        }
    }
    args[1] = n;
    args[2] = (UINT64)(ULONG_PTR)(copy ? copy : rts);
    args[3] = (UINT64)(ULONG_PTR)(ds ? &ds_copy : NULL);
    /* flags is the fifth slot -- a STACK argument, stale-topped above
     * bit 31 (see hand_copy_texture_region) */
    args[4] = (UINT)read_arg( ctx, 4 );
    ret = unix_vtbl_call( host, slot, 5, args );
    if (copy) RtlFreeHeap( NtCurrentTeb()->Peb->ProcessHeap, 0, copy );
    return ret;   /* void method; RAX is scratch */
}

/* ID3D12GraphicsCommandList7::Barrier( UINT32 n, const D3D12_BARRIER_GROUP
 * *groups ): each group points at an array of buffer/texture/global
 * barriers, and the buffer and texture elements carry ID3D12Resource*.
 * Both levels are copied and the resources unwrapped in the copies; a
 * global group's array carries no interfaces and passes through. */
static UINT64 hand_barrier_groups( void *host, UINT slot, AMD64_CONTEXT *ctx )
{
    UINT n = (UINT)read_arg( ctx, 1 );
    const D3D12_BARRIER_GROUP *src = (const void *)(ULONG_PTR)read_arg( ctx, 2 );
    D3D12_BARRIER_GROUP *copy = NULL;
    UINT64 args[D3D12_UNIX_MAX_ARGS] = { 0 };
    HRESULT hr = S_OK;
    UINT64 ret;
    UINT i, j;

    if (n && src)
    {
        if (!(copy = RtlAllocateHeap( NtCurrentTeb()->Peb->ProcessHeap, 0,
                                      n * sizeof(*copy) )))
            return (UINT64)(UINT)E_OUTOFMEMORY;
        memcpy( copy, src, n * sizeof(*copy) );
        for (i = 0; i < n && SUCCEEDED(hr); i++)
        {
            UINT nb = copy[i].NumBarriers;
            if (!nb) continue;
            switch (copy[i].Type)
            {
            case D3D12_BARRIER_TYPE_BUFFER:
            {
                D3D12_BUFFER_BARRIER *b;
                if (!(b = RtlAllocateHeap( NtCurrentTeb()->Peb->ProcessHeap, 0,
                                           nb * sizeof(*b) )))
                {
                    hr = E_OUTOFMEMORY;
                    break;
                }
                memcpy( b, copy[i].pBufferBarriers, nb * sizeof(*b) );
                for (j = 0; j < nb; j++) b[j].pResource = com_unwrap( b[j].pResource );
                copy[i].pBufferBarriers = b;
                break;
            }
            case D3D12_BARRIER_TYPE_TEXTURE:
            {
                D3D12_TEXTURE_BARRIER *t;
                if (!(t = RtlAllocateHeap( NtCurrentTeb()->Peb->ProcessHeap, 0,
                                           nb * sizeof(*t) )))
                {
                    hr = E_OUTOFMEMORY;
                    break;
                }
                memcpy( t, copy[i].pTextureBarriers, nb * sizeof(*t) );
                for (j = 0; j < nb; j++) t[j].pResource = com_unwrap( t[j].pResource );
                copy[i].pTextureBarriers = t;
                break;
            }
            case D3D12_BARRIER_TYPE_GLOBAL:
                break;   /* no interface members */
            default:
                WARN( "unknown barrier group type %u passed through\n", copy[i].Type );
                break;
            }
        }
    }
    if (SUCCEEDED(hr))
    {
        args[1] = n;
        args[2] = (UINT64)(ULONG_PTR)(copy ? copy : src);
        ret = unix_vtbl_call( host, slot, 3, args );
    }
    else
    {
        /* calling with unfixed proxies would hand vkd3d guest pointers;
         * dropping the barriers is the lesser wrong and it is logged */
        ERR( "out of memory copying %u barrier group(s); call dropped\n", n );
        ret = (UINT64)(UINT)hr;
    }
    if (copy)
    {
        for (i = 0; i < n; i++)
            if (copy[i].pGlobalBarriers != src[i].pGlobalBarriers)
                RtlFreeHeap( NtCurrentTeb()->Peb->ProcessHeap, 0,
                             (void *)copy[i].pGlobalBarriers );
        RtlFreeHeap( NtCurrentTeb()->Peb->ProcessHeap, 0, copy );
    }
    return ret;   /* void method; RAX is scratch */
}

/* The D3D12_STATE_OBJECT_DESC walker: DXR state objects hide their
 * interface pointers behind D3D12_STATE_SUBOBJECT's const void* payloads
 * (the reason the generator refuses the raw struct).  The subobject array
 * is copied; the payloads that carry interfaces (the two root-signature
 * kinds, EXISTING_COLLECTION) are copied and unwrapped; a
 * SUBOBJECT_TO_EXPORTS_ASSOCIATION's pointer at another subobject is
 * remapped into the copy, because the callee resolves the association
 * against the array it is handed.  Payload types that provably carry no
 * interfaces pass through pointing at guest memory (vkd3d deep-copies at
 * create).  Anything else fails closed -- a payload this walker cannot
 * name could hide a proxy. */
static HRESULT state_object_desc_unwrap( const D3D12_STATE_OBJECT_DESC *src,
                                         D3D12_STATE_OBJECT_DESC *out, void **blob )
{
    SIZE_T extra = 0, subs;
    D3D12_STATE_SUBOBJECT *sub;
    char *payload;
    UINT i;

    *out = *src;
    *blob = NULL;
    if (!src->NumSubobjects || !src->pSubobjects) return S_OK;

    for (i = 0; i < src->NumSubobjects; i++)
    {
        switch (src->pSubobjects[i].Type)
        {
        case D3D12_STATE_SUBOBJECT_TYPE_GLOBAL_ROOT_SIGNATURE:
        case D3D12_STATE_SUBOBJECT_TYPE_LOCAL_ROOT_SIGNATURE:
            extra += sizeof(D3D12_GLOBAL_ROOT_SIGNATURE);
            break;
        case D3D12_STATE_SUBOBJECT_TYPE_EXISTING_COLLECTION:
            extra += sizeof(D3D12_EXISTING_COLLECTION_DESC);
            break;
        case D3D12_STATE_SUBOBJECT_TYPE_SUBOBJECT_TO_EXPORTS_ASSOCIATION:
            extra += sizeof(D3D12_SUBOBJECT_TO_EXPORTS_ASSOCIATION);
            break;
        case D3D12_STATE_SUBOBJECT_TYPE_STATE_OBJECT_CONFIG:
        case D3D12_STATE_SUBOBJECT_TYPE_NODE_MASK:
        case D3D12_STATE_SUBOBJECT_TYPE_DXIL_LIBRARY:
        case D3D12_STATE_SUBOBJECT_TYPE_DXIL_SUBOBJECT_TO_EXPORTS_ASSOCIATION:
        case D3D12_STATE_SUBOBJECT_TYPE_HIT_GROUP:
        case D3D12_STATE_SUBOBJECT_TYPE_RAYTRACING_SHADER_CONFIG:
        case D3D12_STATE_SUBOBJECT_TYPE_RAYTRACING_PIPELINE_CONFIG:
        case D3D12_STATE_SUBOBJECT_TYPE_RAYTRACING_PIPELINE_CONFIG1:
            break;
        default:
            ERR( "state object subobject %u has type %u this walker cannot "
                 "prove interface-free\n", i, src->pSubobjects[i].Type );
            return E_INVALIDARG;
        }
    }

    subs = src->NumSubobjects * sizeof(*sub);
    if (!(sub = RtlAllocateHeap( NtCurrentTeb()->Peb->ProcessHeap, 0, subs + extra )))
        return E_OUTOFMEMORY;
    memcpy( sub, src->pSubobjects, subs );
    payload = (char *)sub + subs;

    for (i = 0; i < src->NumSubobjects; i++)
    {
        const void *desc = src->pSubobjects[i].pDesc;
        if (!desc) continue;   /* the callee's validation problem, not ours */
        switch (sub[i].Type)
        {
        case D3D12_STATE_SUBOBJECT_TYPE_GLOBAL_ROOT_SIGNATURE:
        {
            D3D12_GLOBAL_ROOT_SIGNATURE *rs = (void *)payload;
            *rs = *(const D3D12_GLOBAL_ROOT_SIGNATURE *)desc;
            rs->pGlobalRootSignature = com_unwrap( rs->pGlobalRootSignature );
            sub[i].pDesc = rs;
            payload += sizeof(*rs);
            break;
        }
        case D3D12_STATE_SUBOBJECT_TYPE_LOCAL_ROOT_SIGNATURE:
        {
            D3D12_LOCAL_ROOT_SIGNATURE *rs = (void *)payload;
            *rs = *(const D3D12_LOCAL_ROOT_SIGNATURE *)desc;
            rs->pLocalRootSignature = com_unwrap( rs->pLocalRootSignature );
            sub[i].pDesc = rs;
            payload += sizeof(*rs);
            break;
        }
        case D3D12_STATE_SUBOBJECT_TYPE_EXISTING_COLLECTION:
        {
            D3D12_EXISTING_COLLECTION_DESC *ec = (void *)payload;
            *ec = *(const D3D12_EXISTING_COLLECTION_DESC *)desc;
            ec->pExistingCollection = com_unwrap( ec->pExistingCollection );
            sub[i].pDesc = ec;
            payload += sizeof(*ec);
            break;
        }
        case D3D12_STATE_SUBOBJECT_TYPE_SUBOBJECT_TO_EXPORTS_ASSOCIATION:
        {
            D3D12_SUBOBJECT_TO_EXPORTS_ASSOCIATION *as = (void *)payload;
            *as = *(const D3D12_SUBOBJECT_TO_EXPORTS_ASSOCIATION *)desc;
            if (as->pSubobjectToAssociate)
            {
                SIZE_T idx = as->pSubobjectToAssociate - src->pSubobjects;
                if (as->pSubobjectToAssociate < src->pSubobjects ||
                    idx >= src->NumSubobjects)
                {
                    ERR( "association subobject %u points outside the "
                         "subobject array\n", i );
                    RtlFreeHeap( NtCurrentTeb()->Peb->ProcessHeap, 0, sub );
                    return E_INVALIDARG;
                }
                as->pSubobjectToAssociate = &sub[idx];
            }
            sub[i].pDesc = as;
            payload += sizeof(*as);
            break;
        }
        default:
            break;   /* pass-through types stay pointing at guest memory */
        }
    }
    out->pSubobjects = sub;
    *blob = sub;
    return S_OK;
}

/* ID3D12Device5::CreateStateObject( const D3D12_STATE_OBJECT_DESC *desc,
 * REFIID riid, void **ppv ): the DXR entry VKD3D_CONFIG=nodxr has been
 * hiding -- with the walker above it can be offered honestly. */
static UINT64 hand_create_state_object( void *host, UINT slot, AMD64_CONTEXT *ctx )
{
    const D3D12_STATE_OBJECT_DESC *src = (const void *)(ULONG_PTR)read_arg( ctx, 1 );
    const GUID *riid = (const GUID *)(ULONG_PTR)read_arg( ctx, 2 );
    void **ppv = (void **)(ULONG_PTR)read_arg( ctx, 3 );
    D3D12_STATE_OBJECT_DESC desc;
    UINT64 args[D3D12_UNIX_MAX_ARGS] = { 0 };
    void *blob;
    HRESULT hr;

    if (!src || !ppv)
    {
        refuse_scrub_ppv( ppv );          /* refusal hygiene by hand */
        return (UINT64)(UINT)E_INVALIDARG;
    }
    if (FAILED(hr = state_object_desc_unwrap( src, &desc, &blob )))
    {
        refuse_scrub_ppv( ppv );          /* refusal hygiene by hand */
        return (UINT64)(UINT)hr;
    }
    args[1] = (UINT64)(ULONG_PTR)&desc;
    args[2] = (UINT64)(ULONG_PTR)riid;
    args[3] = (UINT64)(ULONG_PTR)ppv;
    hr = (HRESULT)unix_vtbl_call( host, slot, 4, args );
    if (blob) RtlFreeHeap( NtCurrentTeb()->Peb->ProcessHeap, 0, blob );
    return (UINT64)(UINT)winecom_wrap_out_iface( hr, riid, ppv );
}

/* ID3D12Device7::AddToStateObject( const D3D12_STATE_OBJECT_DESC *addition,
 * ID3D12StateObject *grow_from, REFIID riid, void **ppv ): the walker
 * above plus one interface argument. */
static UINT64 hand_add_to_state_object( void *host, UINT slot, AMD64_CONTEXT *ctx )
{
    const D3D12_STATE_OBJECT_DESC *src = (const void *)(ULONG_PTR)read_arg( ctx, 1 );
    void *grow_from = (void *)(ULONG_PTR)read_arg( ctx, 2 );
    const GUID *riid = (const GUID *)(ULONG_PTR)read_arg( ctx, 3 );
    void **ppv = (void **)(ULONG_PTR)read_arg( ctx, 4 );
    D3D12_STATE_OBJECT_DESC desc;
    UINT64 args[D3D12_UNIX_MAX_ARGS] = { 0 };
    void *blob;
    HRESULT hr;

    if (!src || !ppv)
    {
        refuse_scrub_ppv( ppv );          /* refusal hygiene by hand */
        return (UINT64)(UINT)E_INVALIDARG;
    }
    if (FAILED(hr = state_object_desc_unwrap( src, &desc, &blob )))
    {
        refuse_scrub_ppv( ppv );          /* refusal hygiene by hand */
        return (UINT64)(UINT)hr;
    }
    args[1] = (UINT64)(ULONG_PTR)&desc;
    args[2] = (UINT64)(ULONG_PTR)com_unwrap( grow_from );
    args[3] = (UINT64)(ULONG_PTR)riid;
    args[4] = (UINT64)(ULONG_PTR)ppv;
    hr = (HRESULT)unix_vtbl_call( host, slot, 5, args );
    if (blob) RtlFreeHeap( NtCurrentTeb()->Peb->ProcessHeap, 0, blob );
    return (UINT64)(UINT)winecom_wrap_out_iface( hr, riid, ppv );
}

/* ---------------------------------------------------------- flat entries */

static HRESULT flat_call( UINT func, UINT argc, UINT64 *args, UINT64 *ret )
{
    struct d3d12_flat_params p;
    NTSTATUS status;

    if (!com_runtime_init()) return E_FAIL;
    memcpy( p.args, args, sizeof(p.args) );
    p.func = func;
    p.argc = argc;
    p.ret = 0;
    if ((status = D3D12_UNIX_CALL( flat, &p )))
    {
        ERR( "unix flat call %u failed, status %08x\n", func, (UINT)status );
        return E_FAIL;
    }
    if (ret) *ret = p.ret;
    return (HRESULT)p.ret;
}

HRESULT WINAPI D3D12CreateDevice( IUnknown *adapter, D3D_FEATURE_LEVEL fl,
                                  REFIID riid, void **device )
{
    UINT64 args[8] = { 0 };
    HRESULT hr;

    TRACE( "adapter %p, fl %#x, riid %s, device %p\n", adapter, fl,
           debugstr_guid(riid), device );
    if (adapter)
        FIXME( "ignoring adapter %p -- no native-lane DXGI yet, and vkd3d's "
               "native path ignores it too\n", adapter );
    args[0] = 0;   /* never a guest proxy: vkd3d would not understand it */
    args[1] = (UINT)fl;
    args[2] = (UINT64)(ULONG_PTR)riid;
    args[3] = (UINT64)(ULONG_PTR)device;
    hr = flat_call( FLAT_D3D12CreateDevice, 4, args, NULL );
    return winecom_wrap_out_iface( hr, riid, device );
}

HRESULT WINAPI D3D12GetDebugInterface( REFIID riid, void **debug )
{
    UINT64 args[8] = { 0 };
    HRESULT hr;

    TRACE( "riid %s, debug %p\n", debugstr_guid(riid), debug );
    args[0] = (UINT64)(ULONG_PTR)riid;
    args[1] = (UINT64)(ULONG_PTR)debug;
    hr = flat_call( FLAT_D3D12GetDebugInterface, 2, args, NULL );
    return winecom_wrap_out_iface( hr, riid, debug );
}

HRESULT WINAPI D3D12CreateRootSignatureDeserializer( const void *data, SIZE_T size,
                                                     REFIID riid, void **out )
{
    UINT64 args[8] = { 0 };
    HRESULT hr;

    TRACE( "data %p, size %Iu, riid %s, out %p\n", data, size,
           debugstr_guid(riid), out );
    args[0] = (UINT64)(ULONG_PTR)data;
    args[1] = size;
    args[2] = (UINT64)(ULONG_PTR)riid;
    args[3] = (UINT64)(ULONG_PTR)out;
    hr = flat_call( FLAT_D3D12CreateRootSignatureDeserializer, 4, args, NULL );
    return winecom_wrap_out_iface( hr, riid, out );
}

HRESULT WINAPI D3D12CreateVersionedRootSignatureDeserializer( const void *data, SIZE_T size,
                                                              REFIID riid, void **out )
{
    UINT64 args[8] = { 0 };
    HRESULT hr;

    TRACE( "data %p, size %Iu, riid %s, out %p\n", data, size,
           debugstr_guid(riid), out );
    args[0] = (UINT64)(ULONG_PTR)data;
    args[1] = size;
    args[2] = (UINT64)(ULONG_PTR)riid;
    args[3] = (UINT64)(ULONG_PTR)out;
    hr = flat_call( FLAT_D3D12CreateVersionedRootSignatureDeserializer, 4, args, NULL );
    return winecom_wrap_out_iface( hr, riid, out );
}

HRESULT WINAPI D3D12EnableExperimentalFeatures( UINT count, const IID *iids,
                                                void *structs, UINT *sizes )
{
    UINT64 args[8] = { 0 };

    TRACE( "count %u, iids %p, structs %p, sizes %p\n", count, iids, structs, sizes );
    args[0] = count;
    args[1] = (UINT64)(ULONG_PTR)iids;
    args[2] = (UINT64)(ULONG_PTR)structs;
    args[3] = (UINT64)(ULONG_PTR)sizes;
    return flat_call( FLAT_D3D12EnableExperimentalFeatures, 4, args, NULL );
}

HRESULT WINAPI D3D12SerializeRootSignature( const D3D12_ROOT_SIGNATURE_DESC *desc,
                                            D3D_ROOT_SIGNATURE_VERSION version,
                                            ID3DBlob **blob, ID3DBlob **error_blob )
{
    UINT64 args[8] = { 0 };
    HRESULT hr;

    TRACE( "desc %p, version %#x, blob %p, error_blob %p\n", desc, version,
           blob, error_blob );
    args[0] = (UINT64)(ULONG_PTR)desc;
    args[1] = (UINT)version;
    args[2] = (UINT64)(ULONG_PTR)blob;
    args[3] = (UINT64)(ULONG_PTR)error_blob;
    hr = flat_call( FLAT_D3D12SerializeRootSignature, 4, args, NULL );
    if (!com_runtime_init()) return E_FAIL;
    /* the error blob is set on FAILURE too, so wrap regardless of hr */
    winecom_wrap_static( (void **)blob, D3D12_IFACE_ID3D10Blob );
    winecom_wrap_static( (void **)error_blob, D3D12_IFACE_ID3D10Blob );
    return hr;
}

HRESULT WINAPI D3D12SerializeVersionedRootSignature( const D3D12_VERSIONED_ROOT_SIGNATURE_DESC *desc,
                                                     ID3DBlob **blob, ID3DBlob **error_blob )
{
    UINT64 args[8] = { 0 };
    HRESULT hr;

    TRACE( "desc %p, blob %p, error_blob %p\n", desc, blob, error_blob );
    args[0] = (UINT64)(ULONG_PTR)desc;
    args[1] = (UINT64)(ULONG_PTR)blob;
    args[2] = (UINT64)(ULONG_PTR)error_blob;
    hr = flat_call( FLAT_D3D12SerializeVersionedRootSignature, 3, args, NULL );
    if (!com_runtime_init()) return E_FAIL;
    winecom_wrap_static( (void **)blob, D3D12_IFACE_ID3D10Blob );
    winecom_wrap_static( (void **)error_blob, D3D12_IFACE_ID3D10Blob );
    return hr;
}

/* The cross-lane swapchain entry, called by NATIVE d3d11.dll (DXVK's lane).
 *
 * A D3D12 title creates its swapchain through dxgi.dll's CreateDXGIFactory,
 * which this port forwards to DXVK -- but the device it passes is THIS
 * surface's ID3D12CommandQueue proxy, and winecom instances are per-linkee,
 * so DXVK's hand_create_swapchain_for_hwnd sees a pointer it cannot
 * translate.  [MEASURED] Cyberpunk 2077, run 31: DXVK's lane refused with
 * "guest-implemented device", the game threw, and the throw died on a
 * fiber stack.  Rather than teaching either surface the other's interning,
 * DXVK's hand slot forwards the whole call HERE, where the queue unwraps in
 * its own surface, the unix present factory creates the swapchain through
 * vkd3d + win32u, and the guest gets back a swapchain proxy of THIS surface
 * -- whose GetBuffer(IID_ID3D12Resource) and Present1 rows are the ones a
 * D3D12 title needs anyway.
 *
 * Returns E_NOINTERFACE iff a non-NULL device/output is NOT this surface's
 * proxy -- the caller keeps its own refusal for that case.  The descriptors
 * cross as opaque pointers; neither carries an interface or a window. */
HRESULT WINAPI __wine_d3d12_create_swapchain_for_hwnd( void *guest_device, void *hwnd,
                                                       const void *desc, const void *fs_desc,
                                                       void *guest_output, void **out )
{
    struct d3d12_present_factory_params params = { 0 };
    UINT64 args[D3D12_UNIX_MAX_ARGS] = { 0 };
    UINT64 rel[2] = { 0 };
    void *host_device = NULL, *host_output = NULL, *host_out = NULL;
    HRESULT hr;

    TRACE( "device %p, hwnd %p, desc %p, fs_desc %p, output %p, out %p\n",
           guest_device, hwnd, desc, fs_desc, guest_output, out );

    if (!out) return E_POINTER;
    *out = NULL;
    if (!com_runtime_init()) return E_FAIL;
    if (guest_device && !winecom_translate_in( guest_device, &host_device ))
        return E_NOINTERFACE;
    if (guest_output && !winecom_translate_in( guest_output, &host_output ))
        return E_NOINTERFACE;
    if (D3D12_UNIX_CALL( present_factory, &params ) || !params.factory)
    {
        ERR( "unix present factory creation failed\n" );
        return E_FAIL;
    }
    args[1] = (UINT64)(ULONG_PTR)host_device;
    args[2] = (UINT64)(ULONG_PTR)hwnd;
    args[3] = (UINT64)(ULONG_PTR)desc;
    args[4] = (UINT64)(ULONG_PTR)fs_desc;
    args[5] = (UINT64)(ULONG_PTR)host_output;
    args[6] = (UINT64)(ULONG_PTR)&host_out;
    /* slot 15 = IDXGIFactory2::CreateSwapChainForHwnd, the same slot the
     * surface's own hand function is registered on (d3d12_marshal.h) */
    hr = (HRESULT)unix_vtbl_call( (void *)(ULONG_PTR)params.factory, 15, 7, args );
    /* drop the creation reference; the swapchain does not reach back into
     * the factory (unix_present.c), same lifetime the guest-proxy path has */
    unix_vtbl_call( (void *)(ULONG_PTR)params.factory, 2, 1, rel );
    if (SUCCEEDED(hr) && host_out)
    {
        if (!(*out = com_wrap( host_out, D3D12_IFACE_IDXGISwapChain1 )))
            return E_OUTOFMEMORY;
    }
    return hr;
}

HRESULT WINAPI D3D12GetInterface( REFCLSID clsid, REFIID riid, void **out )
{
    UINT64 args[8] = { 0 };
    HRESULT hr;

    TRACE( "clsid %s, riid %s, out %p\n", debugstr_guid(clsid),
           debugstr_guid(riid), out );

    /* Phase (a) presentation bootstrap (wine_present.h): no native dxgi.dll
     * exists yet, so the d3d12 module's own minimal factory answers this
     * one private CLSID.  vkd3d never sees it. */
    if (clsid && IsEqualGUID( clsid, &CLSID_WineDXGIFactory ))
    {
        struct d3d12_present_factory_params params = { 0 };

        if (!out) return E_POINTER;
        *out = NULL;
        if (!riid || !IsEqualGUID( riid, &d3d12_com_ifaces[D3D12_IFACE_IDXGIFactory2].iid ))
        {
            WARN( "present factory asked for %s; only IDXGIFactory2 is served in phase (a)\n",
                  debugstr_guid(riid) );
            return E_NOINTERFACE;
        }
        if (!com_runtime_init()) return E_FAIL;
        if (D3D12_UNIX_CALL( present_factory, &params ) || !params.factory)
        {
            ERR( "unix present factory creation failed\n" );
            return E_FAIL;
        }
        if (!(*out = com_wrap( (void *)(ULONG_PTR)params.factory, D3D12_IFACE_IDXGIFactory2 )))
            return E_OUTOFMEMORY;
        return S_OK;
    }
    args[0] = (UINT64)(ULONG_PTR)clsid;
    args[1] = (UINT64)(ULONG_PTR)riid;
    args[2] = (UINT64)(ULONG_PTR)out;
    hr = flat_call( FLAT_D3D12GetInterface, 3, args, NULL );
    return winecom_wrap_out_iface( hr, riid, out );
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

ULONG_PTR WINAPI D3D12CreateDevice( ULONG_PTR a1, ULONG_PTR a2, ULONG_PTR a3, ULONG_PTR a4 )
{
    __wine_spec_unimplemented_stub( "d3d12.dll", "D3D12CreateDevice" );
}

ULONG_PTR WINAPI D3D12GetDebugInterface( ULONG_PTR a1, ULONG_PTR a2 )
{
    __wine_spec_unimplemented_stub( "d3d12.dll", "D3D12GetDebugInterface" );
}

ULONG_PTR WINAPI D3D12CreateRootSignatureDeserializer( ULONG_PTR a1, ULONG_PTR a2, ULONG_PTR a3, ULONG_PTR a4 )
{
    __wine_spec_unimplemented_stub( "d3d12.dll", "D3D12CreateRootSignatureDeserializer" );
}

ULONG_PTR WINAPI D3D12CreateVersionedRootSignatureDeserializer( ULONG_PTR a1, ULONG_PTR a2, ULONG_PTR a3, ULONG_PTR a4 )
{
    __wine_spec_unimplemented_stub( "d3d12.dll", "D3D12CreateVersionedRootSignatureDeserializer" );
}

ULONG_PTR WINAPI D3D12EnableExperimentalFeatures( ULONG_PTR a1, ULONG_PTR a2, ULONG_PTR a3, ULONG_PTR a4 )
{
    __wine_spec_unimplemented_stub( "d3d12.dll", "D3D12EnableExperimentalFeatures" );
}

ULONG_PTR WINAPI D3D12SerializeRootSignature( ULONG_PTR a1, ULONG_PTR a2, ULONG_PTR a3, ULONG_PTR a4 )
{
    __wine_spec_unimplemented_stub( "d3d12.dll", "D3D12SerializeRootSignature" );
}

ULONG_PTR WINAPI D3D12SerializeVersionedRootSignature( ULONG_PTR a1, ULONG_PTR a2, ULONG_PTR a3 )
{
    __wine_spec_unimplemented_stub( "d3d12.dll", "D3D12SerializeVersionedRootSignature" );
}

ULONG_PTR WINAPI D3D12GetInterface( ULONG_PTR a1, ULONG_PTR a2, ULONG_PTR a3 )
{
    __wine_spec_unimplemented_stub( "d3d12.dll", "D3D12GetInterface" );
}

ULONG_PTR WINAPI __wine_com_dispatch( ULONG_PTR a1, ULONG_PTR a2, ULONG_PTR a3 )
{
    __wine_spec_unimplemented_stub( "d3d12.dll", "__wine_com_dispatch" );
}

ULONG_PTR WINAPI __wine_com_slot_name( ULONG_PTR a1, ULONG_PTR a2, ULONG_PTR a3, ULONG_PTR a4 )
{
    __wine_spec_unimplemented_stub( "d3d12.dll", "__wine_com_slot_name" );
}

ULONG_PTR WINAPI __wine_d3d12_create_swapchain_for_hwnd( ULONG_PTR a1, ULONG_PTR a2, ULONG_PTR a3,
                                                         ULONG_PTR a4, ULONG_PTR a5, ULONG_PTR a6 )
{
    __wine_spec_unimplemented_stub( "d3d12.dll", "__wine_d3d12_create_swapchain_for_hwnd" );
}

#endif  /* __powerpc64__ */
