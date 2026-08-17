/*
 * d3d12 unixlib -- the win32u seam of the presentation glue.
 *
 * This TU exists to keep two Vulkan header families apart: win32u's side of
 * the seam speaks wine/vulkan.h types (via wine/vulkan_driver.h), while
 * unix_present.c compiles against vkd3d-proton's generated headers, whose
 * VkSurfaceKHR typedef is a pointer where Wine's is a UINT64.  Everything
 * crosses this internal seam as plain integers, so neither family leaks into
 * the other.
 *
 * All functions here must be called from Wine threads (they end in win32u),
 * never from vkd3d's queue submission thread -- which is why the present
 * hooks run from the unixlib Present slot, not from inside vkd3d
 * (presentation-design.md §4, division of labour).
 *
 * THERE IS ONE COPY OF THIS FILE AND TWO UNIXLIBS COMPILE IT.  dlls/d3d11's
 * and dlls/d3d9's unixlibs present through the same seam for DXVK and include
 * this source directly (each of their unix_win32u.c is that one line and its
 * reasons); the three .so files each get their own copy of these four symbols,
 * which is what they want, and there is still only one place where the win32u
 * ABI is spelled.  The only thing that varies is the debug channel, because a
 * d3d11 process filtering +d3d11 should see these traces.
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
#include <stddef.h>
#include <dlfcn.h>
#include <pthread.h>

#include "ntstatus.h"
#define WIN32_NO_STATUS
#include "windef.h"
#include "winbase.h"
#include "winternl.h"
#include "wine/vulkan.h"
#include "wine/vulkan_driver.h"
#include "wine/debug.h"

/* Spelled out rather than macro-pasted: WINE_DEFAULT_DEBUG_CHANNEL stringifies
 * its argument to name the channel, so a macro passed to it would produce a
 * channel literally called HWNDSURF_CHANNEL that no WINEDEBUG spelling could
 * ever switch on. */
#if defined(HWNDSURF_CHANNEL_D3D11)
WINE_DEFAULT_DEBUG_CHANNEL(d3d11);
#elif defined(HWNDSURF_CHANNEL_D3D9)
WINE_DEFAULT_DEBUG_CHANNEL(d3d9);
#else
WINE_DEFAULT_DEBUG_CHANNEL(d3d12);
#endif

static const struct hwnd_surface_funcs *hwnd_funcs;
static PFN_vkGetInstanceProcAddr system_gipa;
static pthread_once_t init_once = PTHREAD_ONCE_INIT;

static void hwndsurf_init_once( void )
{
    const struct hwnd_surface_funcs *(*get_funcs)( UINT version );
    void *libvulkan, *win32u;

    /* Bind win32u at runtime, and only if it is ALREADY loaded: linking
     * -lwin32u gives this unixlib a DT_NEEDED the build-tree layout cannot
     * satisfy in a headless process, and d3d12.so must keep loading there
     * (the headless gate is exactly that).  A process presenting to a window
     * has win32u.so loaded by construction. */
    if (!(win32u = dlopen( "win32u.so", RTLD_NOW | RTLD_NOLOAD )))
    {
        ERR( "win32u.so is not loaded in this process; no window to present to\n" );
        return;
    }
    get_funcs = dlsym( win32u, "__wine_get_hwnd_surface_funcs" );
    if (!get_funcs || !(hwnd_funcs = get_funcs( WINE_HWND_SURFACE_VERSION )))
    {
        ERR( "win32u refused hwnd surface funcs version %u (get_funcs %p)\n",
             WINE_HWND_SURFACE_VERSION, get_funcs );
        return;
    }
    /* The system loader's vkGetInstanceProcAddr: vkd3d's instance came from
     * the same loader (the d3d12core front end dlopens libvulkan.so.1), so
     * this gipa resolves surface entry points on that instance. */
#ifdef SONAME_LIBVULKAN
    if (!(libvulkan = dlopen( SONAME_LIBVULKAN, RTLD_NOW )))
    {
        ERR( "cannot load %s: %s\n", SONAME_LIBVULKAN, dlerror() );
        hwnd_funcs = NULL;
        return;
    }
    system_gipa = (PFN_vkGetInstanceProcAddr)dlsym( libvulkan, "vkGetInstanceProcAddr" );
#endif
    if (!system_gipa)
    {
        ERR( "no system vkGetInstanceProcAddr\n" );
        hwnd_funcs = NULL;
    }
}

/* Returns VkResult as int; surface out as UINT64.
 *
 * `gipa` is the vkGetInstanceProcAddr the CALLER's instance was created
 * through, and win32u resolves vkCreate{Xlib,Wayland}SurfaceKHR from it.
 * Passing NULL asks for the system loader's, resolved here from
 * SONAME_LIBVULKAN -- which is what vkd3d wants, because its instance came
 * from that loader and it has no gipa of its own to hand over.  DXVK does have
 * one and passes it, so its surface is created through exactly the dispatch
 * chain its VkInstance was built with; guessing there would work right up
 * until somebody put a layer in front of it. */
int hwndsurf_create_gipa( UINT64 hwnd, void *vk_instance, void *gipa,
                          UINT64 *surface, void **cookie )
{
    VkSurfaceKHR host_surface = 0;
    VkResult res;

    pthread_once( &init_once, hwndsurf_init_once );
    if (!hwnd_funcs) return VK_ERROR_INITIALIZATION_FAILED;

    res = hwnd_funcs->surface_create( (HWND)(ULONG_PTR)hwnd, (VkInstance)vk_instance,
                                      gipa ? (PFN_vkGetInstanceProcAddr)gipa : system_gipa,
                                      &host_surface, cookie );
    *surface = (UINT64)host_surface;
    TRACE( "hwnd %p instance %p gipa %p -> res %d surface 0x%s\n",
           (void *)(ULONG_PTR)hwnd, vk_instance, gipa, res,
           wine_dbgstr_longlong( host_surface ) );
    return res;
}

int hwndsurf_create( UINT64 hwnd, void *vk_instance, UINT64 *surface, void **cookie )
{
    return hwndsurf_create_gipa( hwnd, vk_instance, NULL, surface, cookie );
}

void hwndsurf_update( void *cookie )
{
    if (hwnd_funcs) hwnd_funcs->surface_update( cookie );
}

void hwndsurf_presented( void *cookie, int present_result )
{
    if (hwnd_funcs) hwnd_funcs->surface_presented( cookie, present_result );
}

void hwndsurf_destroy( void *cookie )
{
    if (hwnd_funcs) hwnd_funcs->surface_destroy( cookie );
}
