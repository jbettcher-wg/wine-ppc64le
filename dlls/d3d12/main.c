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
static UINT64 hand_create_compute_pso( void *host, UINT slot, AMD64_CONTEXT *ctx )
{
    const D3D12_COMPUTE_PIPELINE_STATE_DESC *src = (const void *)(ULONG_PTR)read_arg( ctx, 1 );
    const GUID *riid = (const GUID *)(ULONG_PTR)read_arg( ctx, 2 );
    void **ppv = (void **)(ULONG_PTR)read_arg( ctx, 3 );
    D3D12_COMPUTE_PIPELINE_STATE_DESC desc;
    UINT64 args[D3D12_UNIX_MAX_ARGS] = { 0 };
    HRESULT hr;
    UINT idx;

    if (!src || !ppv) return (UINT64)(UINT)E_INVALIDARG;
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

    if (!src || !ppv) return (UINT64)(UINT)E_INVALIDARG;
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

/* ID3D12Device2::CreatePipelineState( const D3D12_PIPELINE_STATE_STREAM_DESC
 * *desc, REFIID riid, void **ppv ): the desc is a stream of subobjects --
 * a type enum, a naturally-aligned payload, a stride padded to pointer size,
 * the exact layout vkd3d's own
 * vkd3d_pipeline_state_desc_from_d3d12_stream_desc walks -- and the
 * ROOT_SIGNATURE payload is a guest proxy.  The stream is copied and the
 * proxy unwrapped in the copy; a subobject type the pinned vkd3d headers do
 * not name stops the walk, because a cursor that cannot advance cannot
 * prove the rest of the stream carries no proxies. */
static UINT64 hand_create_pipeline_state( void *host, UINT slot, AMD64_CONTEXT *ctx )
{
    const D3D12_PIPELINE_STATE_STREAM_DESC *src = (const void *)(ULONG_PTR)read_arg( ctx, 1 );
    const GUID *riid = (const GUID *)(ULONG_PTR)read_arg( ctx, 2 );
    void **ppv = (void **)(ULONG_PTR)read_arg( ctx, 3 );
    D3D12_PIPELINE_STATE_STREAM_DESC desc;
    UINT64 args[D3D12_UNIX_MAX_ARGS] = { 0 };
    char *copy = NULL, *ptr, *end;
    HRESULT hr;
    UINT idx;

    if (!src || !ppv) return (UINT64)(UINT)E_INVALIDARG;
    desc = *src;
    if (desc.SizeInBytes && desc.pPipelineStateSubobjectStream)
    {
        if (!(copy = RtlAllocateHeap( NtCurrentTeb()->Peb->ProcessHeap, 0,
                                      desc.SizeInBytes )))
            return (UINT64)(UINT)E_OUTOFMEMORY;
        memcpy( copy, desc.pPipelineStateSubobjectStream, desc.SizeInBytes );
        desc.pPipelineStateSubobjectStream = copy;

        ptr = copy;
        end = copy + desc.SizeInBytes;
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
    args[1] = (UINT64)(ULONG_PTR)&desc;
    args[2] = (UINT64)(ULONG_PTR)riid;
    args[3] = (UINT64)(ULONG_PTR)ppv;
    hr = (HRESULT)unix_vtbl_call( host, slot, 4, args );
    if (copy) RtlFreeHeap( NtCurrentTeb()->Peb->ProcessHeap, 0, copy );
    if (SUCCEEDED(hr) && *ppv)
    {
        idx = winecom_iface_from_iid( riid );
        if (idx == ~0u)
        {
            ERR( "CreatePipelineState returned unknown IID %s\n",
                 debugstr_guid(riid) );
            winecom_host_release( *ppv );
            *ppv = NULL;
            return (UINT64)(UINT)E_NOINTERFACE;
        }
        *ppv = com_wrap( *ppv, idx );
    }
    return (UINT64)(UINT)hr;

malformed:
    ERR( "malformed pipeline state stream (%Iu bytes)\n", desc.SizeInBytes );
    if (copy) RtlFreeHeap( NtCurrentTeb()->Peb->ProcessHeap, 0, copy );
    return (UINT64)(UINT)E_INVALIDARG;
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

ULONG_PTR WINAPI __wine_d3d12_create_swapchain_for_hwnd( ULONG_PTR a1, ULONG_PTR a2, ULONG_PTR a3,
                                                         ULONG_PTR a4, ULONG_PTR a5, ULONG_PTR a6 )
{
    __wine_spec_unimplemented_stub( "d3d12.dll", "__wine_d3d12_create_swapchain_for_hwnd" );
}

#endif  /* __powerpc64__ */
