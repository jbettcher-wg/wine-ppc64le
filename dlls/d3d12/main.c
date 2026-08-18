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

static const winecom_hand_fn d3d12_hand_funcs[] =
{
    hand_resource_barrier,
    hand_create_compute_pso,
    hand_create_swapchain_for_hwnd,
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

#endif  /* __powerpc64__ */
