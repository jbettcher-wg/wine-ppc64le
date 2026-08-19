/*
 * d3d12 unixlib -- the presentation glue ("winesurface", design §4).
 *
 * Host-side COM objects, plain ELF C, living below the winecom marshal
 * boundary and above vkd3d-proton:
 *
 *   present_factory   IDXGIFactory2-shaped.  One real slot,
 *                     CreateSwapChainForHwnd: QIs the ID3D12CommandQueue for
 *                     vkd3d's IDXGIVkSwapChainFactory and builds the
 *                     swapchain below.  Reached from the PE side's
 *                     D3D12GetInterface intercept (CLSID_WineDXGIFactory).
 *
 *   present_swapchain IDXGISwapChain3-shaped over vkd3d's IDXGIVkSwapChain.
 *                     Present wraps the win32u client-surface hooks around
 *                     vkd3d's Present (update before, presented after), on
 *                     the calling Wine thread -- never on vkd3d's queue
 *                     thread.  GetBuffer/GetCurrentBackBufferIndex forward.
 *
 *   surface factory   vkd3d's caller-implemented IDXGIVkSurfaceFactory: the
 *                     one CreateSurface call forwards to win32u's
 *                     __wine_get_hwnd_surface_funcs seam (unix_win32u.c), so
 *                     the VkSurfaceKHR lands on vkd3d's own VkInstance but
 *                     lives on the win32u client surface -- winex11's child
 *                     client window or winewayland's wl_subsurface, chosen
 *                     by whatever driver the session runs.
 *
 * vkd3d-proton is UNMODIFIED: everything here consumes its public factory
 * interfaces (vkd3d_swapchain_factory.idl).
 *
 * Copyright 2026 the ppc64le port authors
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 */

#if 0
#pragma makedep unix
#endif

#include "config.h"

#include <stdarg.h>
#include <stdlib.h>
#include <string.h>

#include "ntstatus.h"
#define WIN32_NO_STATUS
#define COBJMACROS
#define INITGUID
#define COM_NO_WINDOWS_H
/* Wine's base and COM headers supply the environment the vkd3d generated
 * headers otherwise fabricate for themselves (__VKD3D_UNKNOWN_H below skips
 * their non-_WIN32 typedef block, which conflicts with windef.h). */
#include "windef.h"
#include "winbase.h"
#include "objbase.h"
/* The real Vulkan types (VkResult, VkInstance, ...): vkd3d_vk_includes.h
 * only forward-declares them, and the surface factory needs them complete.
 * These are the Khronos headers vkd3d itself builds against. */
#include <vulkan/vulkan_core.h>
#define __VKD3D_UNKNOWN_H
#include <vkd3d_dxgi1_4.h>
#include <vkd3d_swapchain_factory.h>

#include "winternl.h"
#include "wine/unixlib.h"
#include "wine/debug.h"

#include "unixlib.h"

WINE_DEFAULT_DEBUG_CHANNEL(d3d12);

/* The win32u seam, integer-typed on purpose -- see unix_win32u.c. */
extern int  hwndsurf_create( UINT64 hwnd, void *vk_instance, UINT64 *surface, void **cookie );
extern void hwndsurf_update( void *cookie );
extern void hwndsurf_presented( void *cookie, int present_result );
extern void hwndsurf_destroy( void *cookie );

/* ------------------------------------------------- vkd3d surface factory */

struct surface_factory
{
    IDXGIVkSurfaceFactory IDXGIVkSurfaceFactory_iface;
    LONG refs;
    UINT64 hwnd;
    void *cookie;              /* win32u client-surface cookie, once created */
};

static struct surface_factory *impl_from_IDXGIVkSurfaceFactory( IDXGIVkSurfaceFactory *iface )
{
    return CONTAINING_RECORD( iface, struct surface_factory, IDXGIVkSurfaceFactory_iface );
}

static HRESULT STDMETHODCALLTYPE surface_factory_QueryInterface( IDXGIVkSurfaceFactory *iface,
        REFIID riid, void **object )
{
    if (IsEqualGUID( riid, &IID_IUnknown ) || IsEqualGUID( riid, &IID_IDXGIVkSurfaceFactory ))
    {
        IDXGIVkSurfaceFactory_AddRef( iface );
        *object = iface;
        return S_OK;
    }
    *object = NULL;
    return E_NOINTERFACE;
}

static ULONG STDMETHODCALLTYPE surface_factory_AddRef( IDXGIVkSurfaceFactory *iface )
{
    struct surface_factory *factory = impl_from_IDXGIVkSurfaceFactory( iface );
    return InterlockedIncrement( &factory->refs );
}

static ULONG STDMETHODCALLTYPE surface_factory_Release( IDXGIVkSurfaceFactory *iface )
{
    struct surface_factory *factory = impl_from_IDXGIVkSurfaceFactory( iface );
    ULONG refs = InterlockedDecrement( &factory->refs );
    if (!refs)
    {
        /* The VkSurfaceKHR was destroyed by vkd3d (it owns surfaces its
         * factory returns); only the win32u side remains. */
        if (factory->cookie) hwndsurf_destroy( factory->cookie );
        free( factory );
    }
    return refs;
}

static VkResult STDMETHODCALLTYPE surface_factory_CreateSurface( IDXGIVkSurfaceFactory *iface,
        VkInstance vk_instance, VkPhysicalDevice vk_physical_device, VkSurfaceKHR *vk_surface )
{
    struct surface_factory *factory = impl_from_IDXGIVkSurfaceFactory( iface );
    UINT64 surface = 0;
    int res;

    TRACE( "instance %p, physical device %p\n", vk_instance, vk_physical_device );

    if (factory->cookie)
    {
        ERR( "surface already created for hwnd %p\n", (void *)(ULONG_PTR)factory->hwnd );
        return VK_ERROR_INITIALIZATION_FAILED;
    }
    res = hwndsurf_create( factory->hwnd, vk_instance, &surface, &factory->cookie );
    *vk_surface = (VkSurfaceKHR)(ULONG_PTR)surface;
    return res;
}

static CONST_VTBL IDXGIVkSurfaceFactoryVtbl surface_factory_vtbl =
{
    surface_factory_QueryInterface,
    surface_factory_AddRef,
    surface_factory_Release,
    surface_factory_CreateSurface,
};

/* ------------------------------------------------------- the swapchain */

struct present_swapchain
{
    IDXGISwapChain3 IDXGISwapChain3_iface;
    LONG refs;
    IDXGIVkSwapChain *vk_swapchain;
    struct surface_factory *surface_factory;  /* owns the win32u cookie */
    IUnknown *queue;                          /* keeps the device alive */
    DXGI_SWAP_CHAIN_DESC1 desc;
    UINT64 hwnd;
};

static struct present_swapchain *impl_from_IDXGISwapChain3( IDXGISwapChain3 *iface )
{
    return CONTAINING_RECORD( iface, struct present_swapchain, IDXGISwapChain3_iface );
}

static HRESULT STDMETHODCALLTYPE swapchain_QueryInterface( IDXGISwapChain3 *iface, REFIID riid, void **object )
{
    if (IsEqualGUID( riid, &IID_IUnknown )
            || IsEqualGUID( riid, &IID_IDXGIObject )
            || IsEqualGUID( riid, &IID_IDXGIDeviceSubObject )
            || IsEqualGUID( riid, &IID_IDXGISwapChain )
            || IsEqualGUID( riid, &IID_IDXGISwapChain1 )
            || IsEqualGUID( riid, &IID_IDXGISwapChain2 )
            || IsEqualGUID( riid, &IID_IDXGISwapChain3 ))
    {
        IDXGISwapChain3_AddRef( iface );
        *object = iface;
        return S_OK;
    }
    WARN( "unsupported riid %s\n", debugstr_guid( riid ) );
    *object = NULL;
    return E_NOINTERFACE;
}

static ULONG STDMETHODCALLTYPE swapchain_AddRef( IDXGISwapChain3 *iface )
{
    struct present_swapchain *swapchain = impl_from_IDXGISwapChain3( iface );
    return InterlockedIncrement( &swapchain->refs );
}

static ULONG STDMETHODCALLTYPE swapchain_Release( IDXGISwapChain3 *iface )
{
    struct present_swapchain *swapchain = impl_from_IDXGISwapChain3( iface );
    ULONG refs = InterlockedDecrement( &swapchain->refs );
    if (!refs)
    {
        TRACE( "destroying swapchain %p (hwnd %p)\n", swapchain, (void *)(ULONG_PTR)swapchain->hwnd );
        /* vkd3d drains its queue and destroys the VkSurfaceKHR here ... */
        IDXGIVkSwapChain_Release( swapchain->vk_swapchain );
        /* ... then the win32u client surface goes. */
        IDXGIVkSurfaceFactory_Release( &swapchain->surface_factory->IDXGIVkSurfaceFactory_iface );
        IUnknown_Release( swapchain->queue );
        free( swapchain );
    }
    return refs;
}

static HRESULT STDMETHODCALLTYPE swapchain_SetPrivateData( IDXGISwapChain3 *iface, REFGUID guid, UINT size, const void *data )
{
    return S_OK;   /* accepted and dropped; nothing reads it back */
}

static HRESULT STDMETHODCALLTYPE swapchain_SetPrivateDataInterface( IDXGISwapChain3 *iface, REFGUID guid, const IUnknown *object )
{
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE swapchain_GetPrivateData( IDXGISwapChain3 *iface, REFGUID guid, UINT *size, void *data )
{
    return DXGI_ERROR_NOT_FOUND;
}

static HRESULT STDMETHODCALLTYPE swapchain_GetParent( IDXGISwapChain3 *iface, REFIID riid, void **parent )
{
    FIXME( "phase (a): no parent factory exposed\n" );
    *parent = NULL;
    return E_NOINTERFACE;
}

static HRESULT STDMETHODCALLTYPE swapchain_GetDevice( IDXGISwapChain3 *iface, REFIID riid, void **device )
{
    struct present_swapchain *swapchain = impl_from_IDXGISwapChain3( iface );
    return IUnknown_QueryInterface( swapchain->queue, riid, device );
}

static HRESULT STDMETHODCALLTYPE swapchain_Present( IDXGISwapChain3 *iface, UINT sync_interval, UINT flags )
{
    struct present_swapchain *swapchain = impl_from_IDXGISwapChain3( iface );
    HRESULT hr;

    TRACE( "sync_interval %u, flags %#x\n", sync_interval, flags );

    /* The win32u hooks bracket the engine present, on this Wine thread:
     * update attaches/repositions the client surface, presented drives the
     * offscreen blit / subsurface commit (presentation-design.md §2.3).
     * vkd3d's own vkQueuePresentKHR happens on its queue submission thread
     * up to a frame later; the accepted skew is design §11.2. */
    hwndsurf_update( swapchain->surface_factory->cookie );
    hr = IDXGIVkSwapChain_Present( swapchain->vk_swapchain, sync_interval, flags, NULL );
    hwndsurf_presented( swapchain->surface_factory->cookie, 0 );
    return hr;
}

static HRESULT STDMETHODCALLTYPE swapchain_GetBuffer( IDXGISwapChain3 *iface, UINT buffer_idx, REFIID riid, void **surface )
{
    struct present_swapchain *swapchain = impl_from_IDXGISwapChain3( iface );
    TRACE( "buffer_idx %u, riid %s\n", buffer_idx, debugstr_guid( riid ) );
    return IDXGIVkSwapChain_GetImage( swapchain->vk_swapchain, buffer_idx, riid, surface );
}

static HRESULT STDMETHODCALLTYPE swapchain_SetFullscreenState( IDXGISwapChain3 *iface, BOOL fullscreen, IDXGIOutput *target )
{
    FIXME( "fullscreen %d: phase (a) is windowed-only (P4)\n", fullscreen );
    return fullscreen ? DXGI_ERROR_UNSUPPORTED : S_OK;
}

static HRESULT STDMETHODCALLTYPE swapchain_GetFullscreenState( IDXGISwapChain3 *iface, BOOL *fullscreen, IDXGIOutput **target )
{
    if (fullscreen) *fullscreen = FALSE;
    if (target) *target = NULL;
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE swapchain_GetDesc( IDXGISwapChain3 *iface, DXGI_SWAP_CHAIN_DESC *desc )
{
    /* DXGI_SWAP_CHAIN_DESC is only forward-declared by vkd3d's dxgi idl
     * (it consumes DESC1 exclusively); serve GetDesc1 instead. */
    FIXME( "legacy GetDesc not served in phase (a); use GetDesc1\n" );
    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE swapchain_ResizeBuffers( IDXGISwapChain3 *iface, UINT buffer_count,
        UINT width, UINT height, DXGI_FORMAT format, UINT flags )
{
    FIXME( "resize is P4; refusing\n" );
    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE swapchain_ResizeTarget( IDXGISwapChain3 *iface, const DXGI_MODE_DESC *mode )
{
    FIXME( "resize is P4; refusing\n" );
    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE swapchain_GetContainingOutput( IDXGISwapChain3 *iface, IDXGIOutput **output )
{
    *output = NULL;
    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE swapchain_GetFrameStatistics( IDXGISwapChain3 *iface, DXGI_FRAME_STATISTICS *stats )
{
    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE swapchain_GetLastPresentCount( IDXGISwapChain3 *iface, UINT *count )
{
    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE swapchain_GetDesc1( IDXGISwapChain3 *iface, DXGI_SWAP_CHAIN_DESC1 *desc )
{
    struct present_swapchain *swapchain = impl_from_IDXGISwapChain3( iface );
    if (!desc) return E_INVALIDARG;
    *desc = swapchain->desc;
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE swapchain_GetFullscreenDesc( IDXGISwapChain3 *iface, DXGI_SWAP_CHAIN_FULLSCREEN_DESC *desc )
{
    /* Forward-declared only, as with GetDesc; phase (a) is windowed-only. */
    FIXME( "GetFullscreenDesc not served in phase (a)\n" );
    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE swapchain_GetHwnd( IDXGISwapChain3 *iface, HWND *hwnd )
{
    struct present_swapchain *swapchain = impl_from_IDXGISwapChain3( iface );
    if (!hwnd) return E_INVALIDARG;
    *hwnd = (HWND)(ULONG_PTR)swapchain->hwnd;
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE swapchain_GetCoreWindow( IDXGISwapChain3 *iface, REFIID riid, void **unk )
{
    *unk = NULL;
    return E_NOINTERFACE;
}

static HRESULT STDMETHODCALLTYPE swapchain_Present1( IDXGISwapChain3 *iface, UINT sync_interval,
        UINT flags, const DXGI_PRESENT_PARAMETERS *params )
{
    struct present_swapchain *swapchain = impl_from_IDXGISwapChain3( iface );
    HRESULT hr;

    hwndsurf_update( swapchain->surface_factory->cookie );
    hr = IDXGIVkSwapChain_Present( swapchain->vk_swapchain, sync_interval, flags, params );
    hwndsurf_presented( swapchain->surface_factory->cookie, 0 );
    return hr;
}

static BOOL STDMETHODCALLTYPE swapchain_IsTemporaryMonoSupported( IDXGISwapChain3 *iface )
{
    return FALSE;
}

static HRESULT STDMETHODCALLTYPE swapchain_GetRestrictToOutput( IDXGISwapChain3 *iface, IDXGIOutput **output )
{
    *output = NULL;
    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE swapchain_SetBackgroundColor( IDXGISwapChain3 *iface, const DXGI_RGBA *color )
{
    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE swapchain_GetBackgroundColor( IDXGISwapChain3 *iface, DXGI_RGBA *color )
{
    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE swapchain_SetRotation( IDXGISwapChain3 *iface, DXGI_MODE_ROTATION rotation )
{
    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE swapchain_GetRotation( IDXGISwapChain3 *iface, DXGI_MODE_ROTATION *rotation )
{
    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE swapchain_SetSourceSize( IDXGISwapChain3 *iface, UINT width, UINT height )
{
    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE swapchain_GetSourceSize( IDXGISwapChain3 *iface, UINT *width, UINT *height )
{
    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE swapchain_SetMaximumFrameLatency( IDXGISwapChain3 *iface, UINT latency )
{
    struct present_swapchain *swapchain = impl_from_IDXGISwapChain3( iface );
    TRACE( "latency %u\n", latency );
    return IDXGIVkSwapChain_SetFrameLatency( swapchain->vk_swapchain, latency );
}

static HRESULT STDMETHODCALLTYPE swapchain_GetMaximumFrameLatency( IDXGISwapChain3 *iface, UINT *latency )
{
    struct present_swapchain *swapchain = impl_from_IDXGISwapChain3( iface );
    if (!latency) return E_INVALIDARG;
    *latency = IDXGIVkSwapChain_GetFrameLatency( swapchain->vk_swapchain );
    return S_OK;
}

static HANDLE STDMETHODCALLTYPE swapchain_GetFrameLatencyWaitableObject( IDXGISwapChain3 *iface )
{
    FIXME( "the eventfd->semaphore relay is P5; returning NULL\n" );
    return NULL;
}

static HRESULT STDMETHODCALLTYPE swapchain_SetMatrixTransform( IDXGISwapChain3 *iface, const DXGI_MATRIX_3X2_F *matrix )
{
    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE swapchain_GetMatrixTransform( IDXGISwapChain3 *iface, DXGI_MATRIX_3X2_F *matrix )
{
    return E_NOTIMPL;
}

static UINT STDMETHODCALLTYPE swapchain_GetCurrentBackBufferIndex( IDXGISwapChain3 *iface )
{
    struct present_swapchain *swapchain = impl_from_IDXGISwapChain3( iface );
    return IDXGIVkSwapChain_GetImageIndex( swapchain->vk_swapchain );
}

static HRESULT STDMETHODCALLTYPE swapchain_CheckColorSpaceSupport( IDXGISwapChain3 *iface,
        DXGI_COLOR_SPACE_TYPE colour_space, UINT *support )
{
    struct present_swapchain *swapchain = impl_from_IDXGISwapChain3( iface );
    if (!support) return E_INVALIDARG;
    *support = IDXGIVkSwapChain_CheckColorSpaceSupport( swapchain->vk_swapchain, colour_space );
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE swapchain_SetColorSpace1( IDXGISwapChain3 *iface, DXGI_COLOR_SPACE_TYPE colour_space )
{
    struct present_swapchain *swapchain = impl_from_IDXGISwapChain3( iface );
    return IDXGIVkSwapChain_SetColorSpace( swapchain->vk_swapchain, colour_space );
}

static HRESULT STDMETHODCALLTYPE swapchain_ResizeBuffers1( IDXGISwapChain3 *iface, UINT buffer_count,
        UINT width, UINT height, DXGI_FORMAT format, UINT flags, const UINT *node_mask,
        IUnknown *const *queues )
{
    FIXME( "resize is P4; refusing\n" );
    return E_NOTIMPL;
}

static CONST_VTBL IDXGISwapChain3Vtbl present_swapchain_vtbl =
{
    swapchain_QueryInterface,
    swapchain_AddRef,
    swapchain_Release,
    swapchain_SetPrivateData,
    swapchain_SetPrivateDataInterface,
    swapchain_GetPrivateData,
    swapchain_GetParent,
    swapchain_GetDevice,
    swapchain_Present,
    swapchain_GetBuffer,
    swapchain_SetFullscreenState,
    swapchain_GetFullscreenState,
    swapchain_GetDesc,
    swapchain_ResizeBuffers,
    swapchain_ResizeTarget,
    swapchain_GetContainingOutput,
    swapchain_GetFrameStatistics,
    swapchain_GetLastPresentCount,
    swapchain_GetDesc1,
    swapchain_GetFullscreenDesc,
    swapchain_GetHwnd,
    swapchain_GetCoreWindow,
    swapchain_Present1,
    swapchain_IsTemporaryMonoSupported,
    swapchain_GetRestrictToOutput,
    swapchain_SetBackgroundColor,
    swapchain_GetBackgroundColor,
    swapchain_SetRotation,
    swapchain_GetRotation,
    swapchain_SetSourceSize,
    swapchain_GetSourceSize,
    swapchain_SetMaximumFrameLatency,
    swapchain_GetMaximumFrameLatency,
    swapchain_GetFrameLatencyWaitableObject,
    swapchain_SetMatrixTransform,
    swapchain_GetMatrixTransform,
    swapchain_GetCurrentBackBufferIndex,
    swapchain_CheckColorSpaceSupport,
    swapchain_SetColorSpace1,
    swapchain_ResizeBuffers1,
};

/* ------------------------------------------------------- the factory */

struct present_factory
{
    IDXGIFactory2 IDXGIFactory2_iface;
    LONG refs;
};

static struct present_factory *impl_from_IDXGIFactory2( IDXGIFactory2 *iface )
{
    return CONTAINING_RECORD( iface, struct present_factory, IDXGIFactory2_iface );
}

static HRESULT STDMETHODCALLTYPE factory_QueryInterface( IDXGIFactory2 *iface, REFIID riid, void **object )
{
    if (IsEqualGUID( riid, &IID_IUnknown )
            || IsEqualGUID( riid, &IID_IDXGIObject )
            || IsEqualGUID( riid, &IID_IDXGIFactory )
            || IsEqualGUID( riid, &IID_IDXGIFactory1 )
            || IsEqualGUID( riid, &IID_IDXGIFactory2 ))
    {
        IDXGIFactory2_AddRef( iface );
        *object = iface;
        return S_OK;
    }
    WARN( "unsupported riid %s\n", debugstr_guid( riid ) );
    *object = NULL;
    return E_NOINTERFACE;
}

static ULONG STDMETHODCALLTYPE factory_AddRef( IDXGIFactory2 *iface )
{
    struct present_factory *factory = impl_from_IDXGIFactory2( iface );
    return InterlockedIncrement( &factory->refs );
}

static ULONG STDMETHODCALLTYPE factory_Release( IDXGIFactory2 *iface )
{
    struct present_factory *factory = impl_from_IDXGIFactory2( iface );
    ULONG refs = InterlockedDecrement( &factory->refs );
    if (!refs) free( factory );
    return refs;
}

static HRESULT STDMETHODCALLTYPE factory_SetPrivateData( IDXGIFactory2 *iface, REFGUID guid, UINT size, const void *data )
{
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE factory_SetPrivateDataInterface( IDXGIFactory2 *iface, REFGUID guid, const IUnknown *object )
{
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE factory_GetPrivateData( IDXGIFactory2 *iface, REFGUID guid, UINT *size, void *data )
{
    return DXGI_ERROR_NOT_FOUND;
}

static HRESULT STDMETHODCALLTYPE factory_GetParent( IDXGIFactory2 *iface, REFIID riid, void **parent )
{
    *parent = NULL;
    return E_NOINTERFACE;
}

static HRESULT STDMETHODCALLTYPE factory_EnumAdapters( IDXGIFactory2 *iface, UINT adapter_idx, IDXGIAdapter **adapter )
{
    FIXME( "phase (a): no adapter enumeration\n" );
    if (adapter) *adapter = NULL;
    return DXGI_ERROR_NOT_FOUND;
}

static HRESULT STDMETHODCALLTYPE factory_MakeWindowAssociation( IDXGIFactory2 *iface, HWND hwnd, UINT flags )
{
    TRACE( "hwnd %p flags %#x: accepted, phase (a) does nothing with it\n", hwnd, flags );
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE factory_GetWindowAssociation( IDXGIFactory2 *iface, HWND *hwnd )
{
    if (hwnd) *hwnd = NULL;
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE factory_CreateSwapChain( IDXGIFactory2 *iface, IUnknown *device,
        DXGI_SWAP_CHAIN_DESC *desc, IDXGISwapChain **swapchain )
{
    FIXME( "legacy CreateSwapChain not served; CreateSwapChainForHwnd is the phase-(a) path\n" );
    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE factory_CreateSoftwareAdapter( IDXGIFactory2 *iface, HMODULE swrast, IDXGIAdapter **adapter )
{
    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE factory_EnumAdapters1( IDXGIFactory2 *iface, UINT adapter_idx, IDXGIAdapter1 **adapter )
{
    if (adapter) *adapter = NULL;
    return DXGI_ERROR_NOT_FOUND;
}

static BOOL STDMETHODCALLTYPE factory_IsCurrent( IDXGIFactory2 *iface )
{
    return TRUE;
}

static BOOL STDMETHODCALLTYPE factory_IsWindowedStereoEnabled( IDXGIFactory2 *iface )
{
    return FALSE;
}

static HRESULT STDMETHODCALLTYPE factory_CreateSwapChainForHwnd( IDXGIFactory2 *iface,
        IUnknown *device, HWND hwnd, const DXGI_SWAP_CHAIN_DESC1 *desc,
        const DXGI_SWAP_CHAIN_FULLSCREEN_DESC *fullscreen_desc,
        IDXGIOutput *restrict_output, IDXGISwapChain1 **swapchain )
{
    IDXGIVkSwapChainFactory *vk_factory = NULL;
    struct present_swapchain *object = NULL;
    struct surface_factory *surface = NULL;
    DXGI_SWAP_CHAIN_DESC1 swap_desc;
    HRESULT hr;

    TRACE( "device %p, hwnd %p, desc %p, swapchain %p\n", device, hwnd, desc, swapchain );

    if (!device || !hwnd || !desc || !swapchain) return E_INVALIDARG;
    *swapchain = NULL;
    if (restrict_output) FIXME( "ignoring restrict_output %p\n", restrict_output );
    if (fullscreen_desc)
        FIXME( "ignoring fullscreen desc %p; phase (a) creates windowed\n", fullscreen_desc );

    /* vkd3d's swapchain factory lives on the command queue (command.c). */
    if (FAILED(hr = IUnknown_QueryInterface( device, &IID_IDXGIVkSwapChainFactory, (void **)&vk_factory )))
    {
        ERR( "device %p is not an ID3D12CommandQueue (QI for IDXGIVkSwapChainFactory: %#x); "
             "phase (a) accepts only command queues\n", device, (UINT)hr );
        return DXGI_ERROR_UNSUPPORTED;
    }

    if (!(surface = calloc( 1, sizeof(*surface) )))
    {
        hr = E_OUTOFMEMORY;
        goto done;
    }
    surface->IDXGIVkSurfaceFactory_iface.lpVtbl = &surface_factory_vtbl;
    surface->refs = 1;
    surface->hwnd = (UINT64)(ULONG_PTR)hwnd;

    if (!(object = calloc( 1, sizeof(*object) )))
    {
        hr = E_OUTOFMEMORY;
        goto done;
    }
    object->IDXGISwapChain3_iface.lpVtbl = &present_swapchain_vtbl;
    object->refs = 1;
    object->hwnd = (UINT64)(ULONG_PTR)hwnd;
    swap_desc = *desc;
    object->desc = swap_desc;

    hr = IDXGIVkSwapChainFactory_CreateSwapChain( vk_factory,
            &surface->IDXGIVkSurfaceFactory_iface, &swap_desc, &object->vk_swapchain );
    if (FAILED(hr))
    {
        ERR( "vkd3d CreateSwapChain failed, hr %#x\n", (UINT)hr );
        goto done;
    }

    object->surface_factory = surface;   /* transfer our reference */
    surface = NULL;
    object->queue = device;
    IUnknown_AddRef( device );

    TRACE( "created swapchain %p (%ux%u format %u, %u buffers) for hwnd %p\n", object,
           swap_desc.Width, swap_desc.Height, swap_desc.Format, swap_desc.BufferCount, hwnd );
    *swapchain = (IDXGISwapChain1 *)&object->IDXGISwapChain3_iface;
    object = NULL;

done:
    if (surface) IDXGIVkSurfaceFactory_Release( &surface->IDXGIVkSurfaceFactory_iface );
    if (object) free( object );
    if (vk_factory) IDXGIVkSwapChainFactory_Release( vk_factory );
    return hr;
}

static HRESULT STDMETHODCALLTYPE factory_CreateSwapChainForCoreWindow( IDXGIFactory2 *iface,
        IUnknown *device, IUnknown *window, const DXGI_SWAP_CHAIN_DESC1 *desc,
        IDXGIOutput *restrict_output, IDXGISwapChain1 **swapchain )
{
    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE factory_GetSharedResourceAdapterLuid( IDXGIFactory2 *iface, HANDLE resource, LUID *luid )
{
    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE factory_RegisterStereoStatusWindow( IDXGIFactory2 *iface, HWND hwnd, UINT msg, DWORD *cookie )
{
    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE factory_RegisterStereoStatusEvent( IDXGIFactory2 *iface, HANDLE event, DWORD *cookie )
{
    return E_NOTIMPL;
}

static void STDMETHODCALLTYPE factory_UnregisterStereoStatus( IDXGIFactory2 *iface, DWORD cookie )
{
}

static HRESULT STDMETHODCALLTYPE factory_RegisterOcclusionStatusWindow( IDXGIFactory2 *iface, HWND hwnd, UINT msg, DWORD *cookie )
{
    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE factory_RegisterOcclusionStatusEvent( IDXGIFactory2 *iface, HANDLE event, DWORD *cookie )
{
    return E_NOTIMPL;
}

static void STDMETHODCALLTYPE factory_UnregisterOcclusionStatus( IDXGIFactory2 *iface, DWORD cookie )
{
}

static HRESULT STDMETHODCALLTYPE factory_CreateSwapChainForComposition( IDXGIFactory2 *iface,
        IUnknown *device, const DXGI_SWAP_CHAIN_DESC1 *desc, IDXGIOutput *restrict_output,
        IDXGISwapChain1 **swapchain )
{
    return E_NOTIMPL;
}

static CONST_VTBL IDXGIFactory2Vtbl present_factory_vtbl =
{
    factory_QueryInterface,
    factory_AddRef,
    factory_Release,
    factory_SetPrivateData,
    factory_SetPrivateDataInterface,
    factory_GetPrivateData,
    factory_GetParent,
    factory_EnumAdapters,
    factory_MakeWindowAssociation,
    factory_GetWindowAssociation,
    factory_CreateSwapChain,
    factory_CreateSoftwareAdapter,
    factory_EnumAdapters1,
    factory_IsCurrent,
    factory_IsWindowedStereoEnabled,
    factory_CreateSwapChainForHwnd,
    factory_CreateSwapChainForCoreWindow,
    factory_GetSharedResourceAdapterLuid,
    factory_RegisterStereoStatusWindow,
    factory_RegisterStereoStatusEvent,
    factory_UnregisterStereoStatus,
    factory_RegisterOcclusionStatusWindow,
    factory_RegisterOcclusionStatusEvent,
    factory_UnregisterOcclusionStatus,
    factory_CreateSwapChainForComposition,
};

/* ------------------------------------------------------- unixlib entry */

NTSTATUS d3d12_unix_present_factory( void *args )
{
    struct d3d12_present_factory_params *params = args;
    struct present_factory *factory;

    if (!(factory = calloc( 1, sizeof(*factory) ))) return STATUS_NO_MEMORY;
    factory->IDXGIFactory2_iface.lpVtbl = &present_factory_vtbl;
    factory->refs = 1;
    params->factory = (UINT64)(ULONG_PTR)&factory->IDXGIFactory2_iface;
    TRACE( "created present factory %p\n", factory );
    return STATUS_SUCCESS;
}
