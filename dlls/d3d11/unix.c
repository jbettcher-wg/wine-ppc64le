/*
 * d3d11 unixlib -- native DXVK lives here.
 *
 * This is the bottom of the native-lane D3D11 stack: dlopen DXVK's own
 * libdxvk_d3d11.so, libdxvk_dxgi.so and libdxvk_d3d10core.so, resolve their
 * eleven flat entry points, and call COM vtable slots with the widest integer
 * form.  Nothing here knows what any slot means; the PE side's marshal tables
 * (ppc64le/dxvk/gen_winecom.py) decided which slots may cross at all, and
 * everything that crosses is integer-class by construction -- extra arguments
 * are harmless on ELFv2, the same argument dlls/ntdll's call_native_thunk
 * makes, and the same one dlls/d3d12/unix.c makes for vkd3d-proton.
 *
 * WHY THREE LIBRARIES AND NOT ONE.  DXVK builds d3d11, dxgi and d3d10core as
 * separate shared objects with real ELF dependencies between them
 * (libdxvk_d3d11.so NEEDs libdxvk_dxgi.so).  Loading all three here, into one
 * unixlib, is what keeps ONE winecom instance in front of the whole surface;
 * see the banner in unixlib.h for why splitting it would break at the first
 * `D3D11CreateDevice(adapter, ...)`.
 *
 * They are found, in order: DXVK_LIB_DIR when set (an override for pointing at
 * a scratch build); next to this unixlib itself, which is where the Wine build
 * puts them -- the Makefile rule added by configure.ac drives DXVK's meson
 * build and symlinks the three libraries into dlls/d3d11/ beside d3d11.so, so
 * a clean `./configure && make` needs no environment at all; and finally the
 * ordinary dynamic-linker search.
 *
 * THE SYMLINK IS RESOLVED BEFORE THE dlopen, and that is not tidiness.
 * libdxvk_d3d11.so carries DT_RUNPATH=$ORIGIN/../dxgi from the meson build
 * layout, and glibc expands $ORIGIN from the path the object was LOADED BY,
 * not from its realpath -- so loading through the symlink in dlls/d3d11/ would
 * look for the dxgi half in dlls/dxgi/ and not find it.  Measured on the d3d12
 * lane first (dlls/d3d12/unix.c says so at the same spot).
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
#include <stdio.h>
#include <string.h>
#include <dlfcn.h>
#include <pthread.h>
#include <unistd.h>
#include <errno.h>
#include <sys/eventfd.h>
#include <sys/epoll.h>

#include "ntstatus.h"
#define WIN32_NO_STATUS
#include "windef.h"
#include "winternl.h"
#include "wine/unixlib.h"
#include "wine/debug.h"
#include "wine/winecom_fpcall.h"

/* The ONE copy of the WSI callback ABI, shared with the DXVK patch series.
 * dlls/d3d11/Makefile.in puts ppc64le/dxvk on this TU's include path. */
#include "dxvk_win32u_wsi.h"

#include "unixlib.h"

WINE_DEFAULT_DEBUG_CHANNEL(d3d11);

/* The win32u seam, integer-typed on purpose -- see unix_win32u.c, which is
 * dlls/d3d12/unix_win32u.c compiled a second time into this module. */
extern int  hwndsurf_create_gipa( UINT64 hwnd, void *vk_instance, void *gipa,
                                  UINT64 *surface, void **cookie );
extern void hwndsurf_update( void *cookie );
extern void hwndsurf_presented( void *cookie, int present_result );
extern void hwndsurf_destroy( void *cookie );

/* The unversioned symlinks meson emits beside each versioned library in the
 * build layout.  Named, not globbed: a glob would silently pick up whatever
 * a stale build left behind. */
#define DXGI_SONAME     "libdxvk_dxgi.so"
#define D3D11_SONAME    "libdxvk_d3d11.so"
#define D3D10_SONAME    "libdxvk_d3d10core.so"

static const char *const flat_names[FLAT_FUNC_COUNT] =
{
    "D3D11CreateDevice",
    "D3D11CreateDeviceAndSwapChain",
    "D3D11CoreCreateDevice",
    "CreateDXGIFactory",
    "CreateDXGIFactory1",
    "CreateDXGIFactory2",
    "DXGIGetDebugInterface1",
    "DXGIDeclareAdapterRemovalSupport",
    "D3D10CoreCreateDevice",
    "D3D10CoreGetVersion",
    "D3D10CoreRegisterLayers",
};

/* which library each flat entry point lives in */
static const int flat_lib[FLAT_FUNC_COUNT] = { 1,1,1, 0,0,0,0,0, 2,2,2 };

typedef UINT64 (*wide_func)( UINT64, UINT64, UINT64, UINT64, UINT64, UINT64,
                             UINT64, UINT64, UINT64, UINT64, UINT64, UINT64,
                             UINT64, UINT64, UINT64, UINT64 );

static void *dxvk_handle[3];
static wide_func flat_funcs[FLAT_FUNC_COUNT];

/* Resolve one of DXVK's libraries the same three ways, in the same order. */
static void *load_dxvk_lib( const char *soname )
{
    const char *dir = getenv( "DXVK_LIB_DIR" );
    char path[1024];
    Dl_info info;
    void *handle = NULL;

    if (dir && dir[0])
    {
        snprintf( path, sizeof(path), "%s/%s", dir, soname );
        if (!(handle = dlopen( path, RTLD_NOW | RTLD_GLOBAL )))
            ERR( "cannot load %s: %s\n", path, dlerror() );
    }
    if (!handle && dladdr( (void *)load_dxvk_lib, &info ) && info.dli_fname)
    {
        const char *slash = strrchr( info.dli_fname, '/' );
        if (slash && (size_t)(slash - info.dli_fname) + strlen(soname) + 2 < sizeof(path))
        {
            char *real;
            snprintf( path, sizeof(path), "%.*s/%s",
                      (int)(slash - info.dli_fname), info.dli_fname, soname );
            if ((real = realpath( path, NULL )))
            {
                if (!(handle = dlopen( real, RTLD_NOW | RTLD_GLOBAL )))
                    WARN( "cannot load %s: %s\n", real, dlerror() );
                free( real );
            }
            else TRACE( "no %s beside the unixlib, trying the linker path\n", soname );
        }
    }
    if (!handle) handle = dlopen( soname, RTLD_NOW | RTLD_GLOBAL );
    return handle;
}

/* ======================================================================
 *                    presentation: the win32u side of DXVK's WSI
 *
 * DXVK's window-system backend is a compile-time selection, so this lane's
 * patch series adds one more -- "Win32u" -- whose every answer comes from the
 * table registered below (ppc64le/dxvk/dxvk_win32u_wsi.h, and
 * ppc64le/dxvk/dxvk-patches/0003-win32u-wsi-backend.patch for the DXVK half).
 * The HWND DXVK is handed is a Wine window handle, unconverted: the guest PE
 * calls Wine's own user32, so there is exactly one window-handle namespace in
 * the process, which is the fact that lets DXGI_SWAP_CHAIN_DESC::OutputWindow
 * cross the boundary as a plain integer.
 *
 * EVERY CALL BELOW ENDS IN win32u, AND win32u MAY ONLY BE ENTERED FROM A WINE
 * THREAD.  A thread DXVK created with std::thread has no TEB, and win32u's
 * client-surface machinery dereferences one; the fault would land inside
 * somebody else's stack frame with no trace of where it came from.  Rather
 * than trust that DXVK will only ever ask from the application's thread, this
 * asserts it: every unixlib entry point sets a thread-local flag for the
 * duration of the call, and a callback arriving without it is refused by name.
 * In practice the flag is always set, because every path that reaches DXVK at
 * all came through one of those entry points -- and the one that would not, a
 * surface destroyed as the last Rc<Presenter> reference dies on the submission
 * thread, is handled by deferring rather than by refusing.
 * ====================================================================== */

/* Set for the duration of any unixlib entry point.  See the banner.
 *
 * DEFINED IN unix_wsi_window.c, not here.  That file's window and display-mode
 * operations need the same flag for the same reason, and it is the one that
 * carries the whole argument for it now; two definitions of one rule is how a
 * rule stops being one.  There is a copy of the object per .so, which is what
 * each unixlib wants -- d3d11.so's threads are not d3d9.so's. */
extern __thread int wsi_on_wine_thread;

/* unix_wsi_window.c, the shared win32u WINDOW seam.  Integer-typed across, for
 * the reason its banner and dxvk_win32u_wsi.h both give. */
extern void wsiwin_ops_init( struct dxvk_win32u_wsi_ops *ops );
extern int  wsiwin_client_size( UINT64 hwnd, unsigned int *width, unsigned int *height );

struct present_surface
{
    struct present_surface *next;
    UINT64 hwnd;
    UINT64 surface;         /* VkSurfaceKHR, DXVK's, destroyed by DXVK */
    void *cookie;           /* win32u client surface, released by us */
    int orphaned;           /* DXVK let go of the surface off a Wine thread */
};

struct hwnd_state
{
    struct hwnd_state *next;
    UINT64 hwnd;
    UINT width, height;
    UINT valid;
};

static pthread_mutex_t present_lock = PTHREAD_MUTEX_INITIALIZER;
static struct present_surface *present_surfaces;
static struct hwnd_state *hwnd_states;

/* Release every client surface DXVK abandoned on a thread that could not talk
 * to win32u.  Called at the head of every op that IS on a Wine thread, which
 * bounds how long an orphan can live to "until the next present or swapchain
 * creation anywhere in the process". */
static void drain_orphans( void )
{
    struct present_surface **link, *p;

    for (link = &present_surfaces; (p = *link);)
    {
        if (!p->orphaned) { link = &p->next; continue; }
        *link = p->next;
        TRACE( "releasing orphaned client surface for hwnd %p\n",
               (void *)(ULONG_PTR)p->hwnd );
        hwndsurf_destroy( p->cookie );
        free( p );
    }
}

/* The platform surface extensions the caller's instance must enable.
 *
 * win32u's seam resolves BOTH vkCreateXlibSurfaceKHR and
 * vkCreateWaylandSurfaceKHR from the instance it is given and needs whichever
 * one the loaded display driver implements -- a fact that lives in winex11 or
 * winewayland and is not visible from here.  So both are asked for, filtered
 * by what the system loader actually offers, and the session decides at
 * runtime.  Asking for an extension the loader does not have would fail
 * vkCreateInstance outright and take the whole lane down with it, which is why
 * the filter is not optional. */
static const char *ext_names[4];

/* VkExtensionProperties is `{ char extensionName[256]; uint32_t specVersion; }`
 * -- 260 bytes, and read here as an opaque record of that size rather than by
 * including a Vulkan header.  This TU deliberately has no Vulkan header family
 * of its own: unix_win32u.c owns that seam, and keeping the two apart is the
 * whole reason it exists (its banner says so).  The result is a couple of
 * literal sizes, which is a smaller price than a second VkSurfaceKHR
 * definition in one module.  The VkResult that comes back is used only as
 * zero/non-zero, so `int` is enough to say. */
#define VK_EXTENSION_PROPERTIES_SIZE 260

typedef int (*enum_instance_ext_fn)( const char *, unsigned int *, void * );

static void init_instance_extensions( void )
{
    static const char *const wanted[] =
    {
        "VK_KHR_surface", "VK_KHR_xlib_surface", "VK_KHR_wayland_surface"
    };
    enum_instance_ext_fn enum_ext;
    unsigned int i, j, n = 0, count = 0;
    void *libvulkan;
    char *props;

#ifdef SONAME_LIBVULKAN
    if (!(libvulkan = dlopen( SONAME_LIBVULKAN, RTLD_NOW )))
    {
        ERR( "cannot load %s: %s -- no presentable surface extensions can be "
             "named, so every swapchain in this process will be refused\n",
             SONAME_LIBVULKAN, dlerror() );
        return;
    }
#else
    ERR( "built without SONAME_LIBVULKAN; presentation is not available\n" );
    return;
#endif
    if (!(enum_ext = (enum_instance_ext_fn)(ULONG_PTR)
                     dlsym( libvulkan, "vkEnumerateInstanceExtensionProperties" )))
    {
        ERR( "%s exports no vkEnumerateInstanceExtensionProperties\n", SONAME_LIBVULKAN );
        return;
    }
    if (enum_ext( NULL, &count, NULL ) || !count) return;
    if (!(props = calloc( count, VK_EXTENSION_PROPERTIES_SIZE ))) return;
    if (enum_ext( NULL, &count, props )) { free( props ); return; }

    for (i = 0; i < ARRAYSIZE(wanted); i++)
    {
        for (j = 0; j < count; j++)
        {
            const char *have = props + (size_t)j * VK_EXTENSION_PROPERTIES_SIZE;
            if (strcmp( have, wanted[i] )) continue;
            ext_names[n++] = wanted[i];
            break;
        }
    }
    free( props );
    TRACE( "%u of %u instance extension(s) available for presentation\n",
           n, (unsigned int)ARRAYSIZE(wanted) );
}

static const char *const *w32u_instance_extensions( void )
{
    static pthread_once_t once = PTHREAD_ONCE_INIT;

    pthread_once( &once, init_instance_extensions );
    return ext_names;
}

static int w32u_surface_create( UINT64 hwnd, void *instance, void *gipa, UINT64 *surface )
{
    struct present_surface *p;
    void *cookie = NULL;
    UINT64 out = 0;
    int res;

    if (!wsi_on_wine_thread)
    {
        ERR( "refusing to create a client surface for hwnd %p off a Wine "
             "thread: this call ends in win32u, which dereferences a TEB that "
             "a DXVK worker thread does not have.  DXVK asked for a surface "
             "from somewhere other than the application's own call into this "
             "module -- report it with a +d3d11 trace; the swapchain will "
             "report failure rather than fault.\n", (void *)(ULONG_PTR)hwnd );
        return -1000000000; /* VK_ERROR_INITIALIZATION_FAILED */
    }

    pthread_mutex_lock( &present_lock );
    drain_orphans();
    pthread_mutex_unlock( &present_lock );

    res = hwndsurf_create_gipa( hwnd, instance, gipa, &out, &cookie );
    if (res || !out)
    {
        WARN( "win32u refused a surface for hwnd %p, VkResult %d\n",
              (void *)(ULONG_PTR)hwnd, res );
        return res ? res : -1000000000;
    }
    if (!(p = calloc( 1, sizeof(*p) )))
    {
        hwndsurf_destroy( cookie );
        return -1000000003; /* VK_ERROR_OUT_OF_HOST_MEMORY */
    }
    p->hwnd = hwnd;
    p->surface = out;
    p->cookie = cookie;

    pthread_mutex_lock( &present_lock );
    p->next = present_surfaces;
    present_surfaces = p;
    pthread_mutex_unlock( &present_lock );

    *surface = out;
    TRACE( "hwnd %p -> surface 0x%s cookie %p\n", (void *)(ULONG_PTR)hwnd,
           wine_dbgstr_longlong( out ), cookie );
    return 0;
}

static void w32u_surface_destroy( UINT64 surface )
{
    struct present_surface **link, *p = NULL;

    pthread_mutex_lock( &present_lock );
    for (link = &present_surfaces; *link; link = &(*link)->next)
    {
        if ((*link)->surface != surface) continue;
        p = *link;
        /* Off a Wine thread this cannot be released here -- the last
         * Rc<Presenter> reference can be dropped by DXVK's submission thread.
         * Marking it is not a leak: drain_orphans() takes it on the next call
         * that IS on a Wine thread, and there is always one, because a process
         * that has stopped presenting entirely is a process on its way out. */
        if (wsi_on_wine_thread) *link = p->next;
        else p->orphaned = 1;
        break;
    }
    if (p && wsi_on_wine_thread) drain_orphans();
    pthread_mutex_unlock( &present_lock );

    if (!p) return;
    if (!wsi_on_wine_thread)
    {
        TRACE( "deferring release of the client surface for hwnd %p: DXVK "
               "destroyed surface 0x%s off a Wine thread\n",
               (void *)(ULONG_PTR)p->hwnd, wine_dbgstr_longlong( surface ) );
        return;
    }
    hwndsurf_destroy( p->cookie );
    free( p );
}

static int w32u_window_size( UINT64 hwnd, UINT32 *width, UINT32 *height )
{
    struct hwnd_state *s;
    unsigned int w, h;
    int found = 0;

    /* ASK win32u FIRST, and fall back to what the PE side pushed.
     *
     * The pushed size used to be the only answer there was, because the unix
     * side could not reach user32.  It can now (unix_wsi_window.c), and asking
     * matters because DXVK can now MOVE this window: a fullscreen transition
     * changes the size inside one call the application made, and the PE side
     * has no reason to push again until the next present -- so an application's
     * ResizeBuffers(0, 0, ...) right after the transition, which is the normal
     * thing to do there, would size its back buffer from the window's previous
     * size.  The push stays as the fallback for the case that cannot ask: a
     * query arriving on a thread DXVK made, where win32u may not be entered. */
    if (wsiwin_client_size( hwnd, &w, &h ))
    {
        *width = w;
        *height = h;
        return 1;
    }

    pthread_mutex_lock( &present_lock );
    for (s = hwnd_states; s; s = s->next)
    {
        if (s->hwnd != hwnd || !s->valid || !s->width || !s->height) continue;
        *width = s->width;
        *height = s->height;
        found = 1;
        break;
    }
    pthread_mutex_unlock( &present_lock );
    return found;
}

static int w32u_is_window( UINT64 hwnd )
{
    struct hwnd_state *s;
    int valid = 1;

    /* Unknown means "not ours to judge": DXGI asks about windows this module
     * never saw created (MakeWindowAssociation, GetContainingOutput), and
     * answering no there would make DXVK skip work it should do.  Only a
     * window this module WATCHED go away answers no. */
    pthread_mutex_lock( &present_lock );
    for (s = hwnd_states; s; s = s->next)
        if (s->hwnd == hwnd) { valid = s->valid != 0; break; }
    pthread_mutex_unlock( &present_lock );
    return valid;
}

/* NOT const any more, and the reason is the abi-2 half.
 *
 * The six window and display-mode operations are filled in by
 * wsiwin_ops_init() at registration time rather than here, because whether
 * they are published at all is a RUNTIME decision: WINEDXVKNOWINDOWOPS=1
 * leaves them NULL and the port behaves exactly as it did before abi 2, which
 * is the negative control ppc64le/dxvk/check-fullscreen-smoke.sh --sabotage
 * runs.  A static initialiser cannot express that, and putting the lever on the
 * DXVK side instead would have put it in the half of the boundary this tree
 * carries as a patch series rather than as source.  The table is still written
 * exactly once, on the loader's thread, before any DXVK entry point has run. */
static struct dxvk_win32u_wsi_ops win32u_wsi_ops =
{
    .abi = DXVK_WIN32U_WSI_ABI,
    .instance_extensions = w32u_instance_extensions,
    .surface_create = w32u_surface_create,
    .surface_destroy = w32u_surface_destroy,
    .window_size = w32u_window_size,
    .is_window = w32u_is_window,
};

/* Registered into EVERY DXVK library, because src/wsi is a static library and
 * each of them carries its own copy of it: the D3D11 swapchain's surface is
 * created by libdxvk_dxgi.so's WsiDriver, not libdxvk_d3d11.so's.  The count
 * is logged because "registered in some of them" presents to nothing and
 * looks, from the application's side, exactly like success. */
static void register_wsi_ops( void *const *handles, unsigned int count )
{
    unsigned int i, done = 0;

    /* Before the first library sees the table, and once: the abi-2 half, or
     * the deliberate absence of it.  See the note on win32u_wsi_ops. */
    wsiwin_ops_init( &win32u_wsi_ops );

    for (i = 0; i < count; i++)
    {
        dxvk_wsi_win32u_register_fn fn;

        if (!handles[i]) continue;
        if (!(fn = (dxvk_wsi_win32u_register_fn)(ULONG_PTR)
                   dlsym( handles[i], DXVK_WIN32U_WSI_REGISTER_NAME )))
            continue;
        if (fn( &win32u_wsi_ops )) done++;
        else ERR( "%s refused the win32u WSI table; this lane's DXVK patch "
                  "series and ppc64le/dxvk/dxvk_win32u_wsi.h have come apart\n",
                  DXVK_WIN32U_WSI_REGISTER_NAME );
    }
    TRACE( "win32u WSI table registered in %u DXVK librar%s\n",
           done, done == 1 ? "y" : "ies" );
    if (!done)
        ERR( "no DXVK library accepted the win32u WSI table -- every swapchain "
             "in this process will be refused.  Was DXVK built from "
             "ppc64le/dxvk/dxvk-patches/ (bootstrap.sh --check)?\n" );
}

static NTSTATUS d3d11_unix_present( void *args )
{
    struct d3d11_present_params *p = args;
    struct present_surface *s;
    void *cookie = NULL;

    pthread_mutex_lock( &present_lock );
    drain_orphans();
    for (s = present_surfaces; s; s = s->next)
        if (s->hwnd == p->hwnd && !s->orphaned) { cookie = s->cookie; break; }
    pthread_mutex_unlock( &present_lock );

    /* No surface for this window is not an error: an occluded or headless
     * swapchain never got one, and DXVK's Present still has work to do. */
    if (!cookie) return STATUS_SUCCESS;

    if (p->phase == PRESENT_PHASE_BEGIN) hwndsurf_update( cookie );
    else hwndsurf_presented( cookie, p->result );
    return STATUS_SUCCESS;
}

static NTSTATUS d3d11_unix_hwnd( void *args )
{
    struct d3d11_hwnd_params *p = args;
    struct hwnd_state *s;

    pthread_mutex_lock( &present_lock );
    for (s = hwnd_states; s; s = s->next) if (s->hwnd == p->hwnd) break;
    if (!s && (s = calloc( 1, sizeof(*s) )))
    {
        s->hwnd = p->hwnd;
        s->next = hwnd_states;
        hwnd_states = s;
    }
    if (s)
    {
        s->width = p->width;
        s->height = p->height;
        s->valid = p->valid;
    }
    pthread_mutex_unlock( &present_lock );
    return STATUS_SUCCESS;
}

static NTSTATUS d3d11_unix_init( void *args )
{
    static const char *const sonames[3] = { DXGI_SONAME, D3D11_SONAME, D3D10_SONAME };
    unsigned int i;

    if (dxvk_handle[1]) return STATUS_SUCCESS;

    /* PICK A WSI BACKEND BEFORE DXVK IS LOADED, or there is no device at all.
     *
     * DXVK's native build selects its window-system backend from
     * DXVK_WSI_DRIVER, and with the variable unset it refuses to create a DXGI
     * factory -- which fails D3D11CreateDevice, which fails everything.  It is
     * not a presentation-only concern: an application that never asks for a
     * swapchain still needs the factory.  [MEASURED] the guest leg of
     * ppc64le/dxvk/check-d3d11-smoke.sh failed exactly this way, with
     * `err: DXVK_WSI_DRIVER environment variable unset` followed by
     * `D3D11CreateDevice: Failed to create a DXGI factory`.
     *
     * Win32u is this lane's own backend (ppc64le/dxvk/dxvk-patches/
     * 0003-win32u-wsi-backend.patch): every question it is asked about a
     * window is forwarded to the table registered below, and through it to
     * win32u's client-surface layer -- the same seam winevulkan uses, so X11
     * and Wayland are both served without either being named.  It is the right
     * choice even in a process that never presents: with no window it answers
     * exactly as Headless did, because there is nothing to create a surface
     * for and it says so.  This line used to read "Headless" for the length of
     * time this lane had no presentation path at all.
     *
     * overwrite = 0, so someone debugging with DXVK_WSI_DRIVER=SDL2 in the
     * environment still gets what they asked for -- and so the native leg of
     * ppc64le/dxvk/check-d3d11-smoke.sh, which sets Headless explicitly to
     * prove the offscreen path needs no window system at all, still gets it. */
    setenv( "DXVK_WSI_DRIVER", "Win32u", 0 );

    /* Windowless composition swapchains, served DXVK'S OWN WAY.  Upstream
     * gates CreateSwapChainForComposition behind this option and passes a
     * NULL window into CreateSwapChainBase; the presenter takes a surface
     * FACTORY, not a surface, so nothing tries to realise a null-window
     * surface until a Present actually happens -- the app runs and renders,
     * visibly nowhere, exactly Windows-composition-without-a-compositor
     * semantics.  The old refusal reasoned "visible nowhere is worse than a
     * refusal"; that judgment is REVERSED by direction (2026-09-01): a
     * refusal kills an app that would otherwise run.  overwrite = 0 for the
     * same debuggability reason as the WSI line above -- a user-set
     * DXVK_CONFIG wins, and loses this option knowingly. */
    setenv( "DXVK_CONFIG", "dxgi.enableDummyCompositionSwapchain = True; "
                           "dxgi.deferSurfaceCreation = True", 0 );
    /* deferSurfaceCreation rides along because the dummy path is only dummy
     * if nothing asks the WSI for a null-window surface at CREATION time --
     * the presenter takes a factory callback either way, and this option is
     * what keeps it from invoking the callback until a Present actually
     * happens.  [MEASURED] without it the win32u backend (correctly)
     * refused the null HWND at swapchain build and the composition serve
     * failed with VK_ERROR_INITIALIZATION_FAILED.  For ordinary windowed
     * swapchains the option only SHIFTS surface creation from create-time
     * to first-present -- a timing DXVK itself ships as a per-game default
     * (config.cpp lists titles pinning it True) -- and the byte-compared
     * smoke legs hold it to producing identical results. */

    /* dxgi first: the other two NEED it, and loading it by our own search
     * rules rather than leaving it to the linker is what makes DXVK_LIB_DIR
     * mean what it says. */
    for (i = 0; i < 3; i++)
    {
        if (dxvk_handle[i]) continue;
        if (!(dxvk_handle[i] = load_dxvk_lib( sonames[i] )))
        {
            ERR( "cannot load %s: %s -- did the build's DXVK step run?  "
                 "(ppc64le/dxvk/build-for-wine.sh; DXVK_LIB_DIR overrides the "
                 "search)\n", sonames[i], dlerror() );
            return STATUS_DLL_NOT_FOUND;
        }
    }
    for (i = 0; i < FLAT_FUNC_COUNT; i++)
    {
        if (!(flat_funcs[i] = (wide_func)(ULONG_PTR)dlsym( dxvk_handle[flat_lib[i]],
                                                           flat_names[i] )))
        {
            ERR( "%s exports no %s: %s\n", sonames[flat_lib[i]], flat_names[i],
                 dlerror() );
            return STATUS_ENTRYPOINT_NOT_FOUND;
        }
    }
    TRACE( "loaded DXVK with %u flat entries\n", i );

    /* Before anything asks DXVK for a DXGI factory: wsi::init() runs on the
     * first factory and builds the Win32u driver, whose very first act is to
     * report whether this table is there. */
    register_wsi_ops( (void *const *)dxvk_handle, 3 );
    return STATUS_SUCCESS;
}

static UINT64 call_wide( wide_func fn, const UINT64 *a )
{
    return fn( a[0], a[1], a[2],  a[3],  a[4],  a[5],  a[6],  a[7],
               a[8], a[9], a[10], a[11], a[12], a[13], a[14], a[15] );
}

static NTSTATUS d3d11_unix_call( void *args )
{
    struct d3d11_call_params *p = args;
    void **vtbl;

    if (!p->args[0]) return STATUS_INVALID_PARAMETER;
    vtbl = *(void ***)(ULONG_PTR)p->args[0];
    p->ret = call_wide( (wide_func)vtbl[p->slot], p->args );
    return STATUS_SUCCESS;
}

/* The float-bearing slots.  There are exactly three of them outside the video
 * path, and each gets its real prototype here rather than a cast of the wide
 * form: on ELFv2 a `float` argument is passed in f1..f13 and the wide form
 * would leave the FPRs untouched, so the callee would read whatever was
 * already there.  A wrong Depth on a ClearDepthStencilView is not a crash --
 * it is a frame that looks nearly right, which is worse. */
static NTSTATUS d3d11_unix_float( void *args )
{
    struct d3d11_float_params *p = args;
    void **vtbl;

    if (!p->self) return STATUS_INVALID_PARAMETER;
    vtbl = *(void ***)(ULONG_PTR)p->self;
    switch (p->shape)
    {
    case FLOAT_SHAPE_RES_UINT_FLOAT_BYTE:
    {
        void (*fn)( void *, void *, UINT, float, UINT8 ) = (void *)vtbl[p->slot];
        fn( (void *)(ULONG_PTR)p->self, (void *)(ULONG_PTR)p->res,
            (UINT)p->a, p->f, (UINT8)p->b );
        return STATUS_SUCCESS;
    }
    case FLOAT_SHAPE_RES_FLOAT:
    {
        void (*fn)( void *, void *, float ) = (void *)vtbl[p->slot];
        fn( (void *)(ULONG_PTR)p->self, (void *)(ULONG_PTR)p->res, p->f );
        return STATUS_SUCCESS;
    }
    case FLOAT_SHAPE_RES_RET_FLOAT:
    {
        float (*fn)( void *, void * ) = (void *)vtbl[p->slot];
        p->ret = fn( (void *)(ULONG_PTR)p->self, (void *)(ULONG_PTR)p->res );
        return STATUS_SUCCESS;
    }
    }
    return STATUS_INVALID_PARAMETER;
}

static NTSTATUS d3d11_unix_flat( void *args )
{
    struct d3d11_flat_params *p = args;
    UINT64 a[D3D11_UNIX_MAX_ARGS] = { 0 };

    if (p->func >= FLAT_FUNC_COUNT || !flat_funcs[p->func])
        return STATUS_INVALID_PARAMETER;
    memcpy( a, p->args, sizeof(p->args) );
    p->ret = call_wide( flat_funcs[p->func], a );
    return STATUS_SUCCESS;
}

/* The GENERIC float-bearing vtable call: the generated rows' counterpart to
 * d3d11_unix_float's hand-walker shapes (unixlib.h has the contract).  The
 * split-and-call is the ONE shared implementation every FP-serving surface
 * uses -- wine/winecom_fpcall.h -- so the register rule has a single home. */
WINECOM_DEFINE_FP_CALLER( d3d11_fp_caller )

static NTSTATUS d3d11_unix_fpcall( void *args )
{
    struct d3d11_fpcall_params *p = args;
    void **vtbl;

    if (!p->args[0] || p->argc > D3D11_UNIX_MAX_ARGS) return STATUS_INVALID_PARAMETER;
    vtbl = *(void ***)(ULONG_PTR)p->args[0];
    p->ret = winecom_fp_invoke( d3d11_fp_caller, vtbl[p->slot], p->argc,
                                p->args, p->fpword, &p->fpret_bits );
    return STATUS_SUCCESS;
}

/* THE ONE PLACE THAT KNOWS THIS IS A WINE THREAD.
 *
 * Everything below this point may be re-entered by DXVK through the WSI
 * callback table, and those callbacks end in win32u, which may only be called
 * from a thread with a TEB.  A unixlib entry point is by construction on one:
 * the only way to get here is through NtQueryVirtualMemory's unix-call
 * mechanism from PE code.  So the flag is set here, once, for every entry --
 * and a callback that arrives with it clear came from a thread DXVK made,
 * which is the case w32u_surface_create refuses by name and
 * w32u_surface_destroy defers.
 *
 * The counter, not a plain assignment: a callback may itself call back into
 * DXVK, and nesting must not clear the flag on the way out of the inner
 * frame.  It costs one increment per boundary crossing on a path that already
 * costs a syscall. */
#define WINE_THREAD_ENTRY(name, impl)                                        \
    static NTSTATUS name( void *args )                                       \
    {                                                                        \
        NTSTATUS status;                                                     \
        wsi_on_wine_thread++;                                                \
        status = impl( args );                                               \
        wsi_on_wine_thread--;                                                \
        return status;                                                       \
    }

/* ------------------------------------------------------------ event relay
 *
 * The unix half of the event translation (unixlib.h has the flow).  The tag
 * is the vkd3d/dxvk native event convention -- 'EVFD' over the fd -- spelled
 * here a third time for the same reason the vkd3d README gives for its own
 * respellings: the consumers build independently, and the gate asserts the
 * spellings agree rather than trusting an include path across projects. */
#define D3D11_NATIVE_EVENT_TAG 0x4556464400000000ull

struct event_relay_entry
{
    int    efd;            /* -1 = free slot */
    UINT64 guest_handle;   /* the PE side's duplicated reference */
    BOOL   oneshot;
};

#define EVENT_RELAY_MAX 64
static struct event_relay_entry event_relay[EVENT_RELAY_MAX];
static pthread_mutex_t event_relay_lock = PTHREAD_MUTEX_INITIALIZER;
static int event_epoll = -1;

static NTSTATUS d3d11_unix_event_mint( void *args )
{
    struct d3d11_event_mint_params *params = args;
    int i, efd;

    params->native_handle = 0;

    pthread_mutex_lock( &event_relay_lock );
    if (event_epoll < 0)
    {
        /* first mint: the free-slot sentinel is -1 and a static array is
         * ZERO-initialized -- and fd 0 is a real fd, so 0 cannot mean free.
         * [MEASURED] before this loop existed every slot read as occupied
         * and the first mint refused with "relay table full (64)". */
        for (i = 0; i < EVENT_RELAY_MAX; i++) event_relay[i].efd = -1;
        if ((event_epoll = epoll_create1( EPOLL_CLOEXEC )) < 0)
        {
            pthread_mutex_unlock( &event_relay_lock );
            return STATUS_SUCCESS;  /* native_handle 0: the row refuses */
        }
    }
    for (i = 0; i < EVENT_RELAY_MAX; i++) if (event_relay[i].efd < 0) break;
    if (i == EVENT_RELAY_MAX)
    {
        /* Registrations are once-per-app and one-shots reap at payout; a
         * full table means something is registering in a loop.  Refusing
         * (fail closed) beats growing without bound. */
        fprintf( stderr, "d3d11: event relay table full (%u); refusing\n",
                 EVENT_RELAY_MAX );
        pthread_mutex_unlock( &event_relay_lock );
        return STATUS_SUCCESS;
    }
    if ((efd = eventfd( 0, EFD_CLOEXEC | EFD_NONBLOCK )) < 0)
    {
        pthread_mutex_unlock( &event_relay_lock );
        return STATUS_SUCCESS;
    }
    else
    {
        struct epoll_event ev = { .events = EPOLLIN, .data.u32 = (UINT)i };

        if (epoll_ctl( event_epoll, EPOLL_CTL_ADD, efd, &ev ) < 0)
        {
            close( efd );
            pthread_mutex_unlock( &event_relay_lock );
            return STATUS_SUCCESS;
        }
    }
    event_relay[i].efd = efd;
    event_relay[i].guest_handle = params->guest_handle;
    event_relay[i].oneshot = !!params->oneshot;
    pthread_mutex_unlock( &event_relay_lock );

    params->native_handle = D3D11_NATIVE_EVENT_TAG | (UINT64)(UINT)efd;
    return STATUS_SUCCESS;
}

/* Blocks in epoll until some eventfd pays out; called in a loop by the PE
 * pump thread, which does the NtSetEvent this side cannot.  epoll_ctl from
 * concurrent mint/reap calls while this thread waits is defined behavior. */
static NTSTATUS d3d11_unix_event_pump( void *args )
{
    struct d3d11_event_pump_params *params = args;
    struct epoll_event ev;
    int n, idx;
    UINT64 v;

    params->guest_handle = 0;
    params->close_handle = 0;
    params->shutdown = 0;

    for (;;)
    {
        n = epoll_wait( event_epoll, &ev, 1, -1 );
        if (n < 0)
        {
            if (errno == EINTR) continue;
            params->shutdown = 1;   /* epoll fd itself broke; stop the pump */
            return STATUS_SUCCESS;
        }
        idx = (int)ev.data.u32;

        pthread_mutex_lock( &event_relay_lock );
        if (idx < 0 || idx >= EVENT_RELAY_MAX || event_relay[idx].efd < 0)
        {
            /* reaped between wakeup and lock; nothing to signal */
            pthread_mutex_unlock( &event_relay_lock );
            continue;
        }
        if (read( event_relay[idx].efd, &v, sizeof(v) ) != sizeof(v))
        {
            pthread_mutex_unlock( &event_relay_lock );
            continue;
        }
        params->guest_handle = event_relay[idx].guest_handle;
        if (event_relay[idx].oneshot)
        {
            epoll_ctl( event_epoll, EPOLL_CTL_DEL, event_relay[idx].efd, NULL );
            close( event_relay[idx].efd );
            event_relay[idx].efd = -1;
            params->close_handle = 1;
        }
        pthread_mutex_unlock( &event_relay_lock );
        return STATUS_SUCCESS;
    }
}

static NTSTATUS d3d11_unix_event_reap( void *args )
{
    struct d3d11_event_reap_params *params = args;
    int i, efd = (int)(UINT)(params->native_handle & 0xffffffffu);

    params->guest_handle = 0;
    if ((params->native_handle & ~0xffffffffull) != D3D11_NATIVE_EVENT_TAG)
        return STATUS_SUCCESS;

    pthread_mutex_lock( &event_relay_lock );
    for (i = 0; i < EVENT_RELAY_MAX; i++)
    {
        if (event_relay[i].efd != efd) continue;
        epoll_ctl( event_epoll, EPOLL_CTL_DEL, efd, NULL );
        close( efd );
        event_relay[i].efd = -1;
        params->guest_handle = event_relay[i].guest_handle;
        break;
    }
    pthread_mutex_unlock( &event_relay_lock );
    return STATUS_SUCCESS;
}

WINE_THREAD_ENTRY( d3d11_enter_init,    d3d11_unix_init )
WINE_THREAD_ENTRY( d3d11_enter_call,    d3d11_unix_call )
WINE_THREAD_ENTRY( d3d11_enter_float,   d3d11_unix_float )
WINE_THREAD_ENTRY( d3d11_enter_flat,    d3d11_unix_flat )
WINE_THREAD_ENTRY( d3d11_enter_present, d3d11_unix_present )
WINE_THREAD_ENTRY( d3d11_enter_hwnd,    d3d11_unix_hwnd )
WINE_THREAD_ENTRY( d3d11_enter_fpcall,  d3d11_unix_fpcall )
WINE_THREAD_ENTRY( d3d11_enter_event_mint, d3d11_unix_event_mint )
WINE_THREAD_ENTRY( d3d11_enter_event_pump, d3d11_unix_event_pump )
WINE_THREAD_ENTRY( d3d11_enter_event_reap, d3d11_unix_event_reap )

const unixlib_entry_t __wine_unix_call_funcs[] =
{
    d3d11_enter_init,
    d3d11_enter_call,
    d3d11_enter_float,
    d3d11_enter_flat,
    d3d11_enter_present,
    d3d11_enter_hwnd,
    d3d11_enter_fpcall,
    d3d11_enter_event_mint,
    d3d11_enter_event_pump,
    d3d11_enter_event_reap,
};

C_ASSERT( ARRAYSIZE(__wine_unix_call_funcs) == unix_funcs_count );
