/*
 * d3d9 unixlib -- native DXVK's d3d9 lives here.
 *
 * The bottom of the native-lane D3D9 stack, and the sibling of
 * dlls/d3d11/unix.c: dlopen DXVK's libdxvk_d3d9.so, resolve its flat entry
 * points, and call COM vtable slots with the widest integer form.  Nothing
 * here knows what any slot means; the PE side's marshal tables
 * (ppc64le/dxvk/gen_winecom.py over ppc64le/dxvk/interfaces_d3d9.json) decided
 * which slots may cross at all, and everything that crosses is integer-class
 * by construction.
 *
 * ONE LIBRARY, NOT THREE.  libdxvk_d3d9.so has no DT_NEEDED on any other DXVK
 * library -- checked with readelf, not assumed -- so unlike the D3D11 lane
 * there is no set of libraries to keep together and no $ORIGIN/../dxgi to
 * resolve.  The library is still loaded from the meson BUILD layout for the
 * same reason the D3D11 lane's are: that is where the Wine build puts it, and
 * the Makefile symlinks it in beside this unixlib.
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

#include "ntstatus.h"
#define WIN32_NO_STATUS
#include "windef.h"
#include "winternl.h"
#include "wine/unixlib.h"
#include "wine/debug.h"

/* The ONE copy of the WSI callback ABI, shared with the DXVK patch series and
 * with dlls/d3d11/unix.c.  dlls/d3d9/Makefile.in puts ppc64le/dxvk on this
 * TU's include path. */
#include "dxvk_win32u_wsi.h"

#include "unixlib.h"

WINE_DEFAULT_DEBUG_CHANNEL(d3d9);

/* The win32u seam, integer-typed on purpose -- see unix_win32u.c, which is
 * dlls/d3d12/unix_win32u.c compiled a third time into this module. */
extern int  hwndsurf_create_gipa( UINT64 hwnd, void *vk_instance, void *gipa,
                                  UINT64 *surface, void **cookie );
extern void hwndsurf_update( void *cookie );
extern void hwndsurf_presented( void *cookie, int present_result );
extern void hwndsurf_destroy( void *cookie );

#define D3D9_SONAME "libdxvk_d3d9.so"

static const char *const flat_names[FLAT_FUNC_COUNT] =
{
    "Direct3DCreate9",
    "Direct3DCreate9Ex",
    "Direct3DShaderValidatorCreate9",
    "D3DPERF_EndEvent",
    "D3DPERF_GetStatus",
    "D3DPERF_QueryRepeatFrame",
    "D3DPERF_SetOptions",
    "DebugSetMute",
};

typedef UINT64 (*wide_func)( UINT64, UINT64, UINT64, UINT64, UINT64, UINT64,
                             UINT64, UINT64, UINT64, UINT64, UINT64, UINT64,
                             UINT64, UINT64, UINT64, UINT64 );

static void *dxvk_handle;
static wide_func flat_funcs[FLAT_FUNC_COUNT];

/* Resolve DXVK's d3d9 the same three ways dlls/d3d11/unix.c resolves its
 * three, in the same order and for the same reasons: DXVK_LIB_DIR when set,
 * then beside this unixlib (realpathed, because glibc expands $ORIGIN from the
 * path an object was loaded by), then the ordinary linker search. */
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
 *          presentation: the win32u side of DXVK's WSI, for D3D9
 *
 * Structurally identical to dlls/d3d11/unix.c's half, and it must be a SECOND
 * copy rather than a shared one: libdxvk_d3d9.so links its own copy of DXVK's
 * WSI static library, with its own WsiDriver and its own callback table, so
 * the registration below reaches d3d9's copy and only that.  The state here is
 * this module's own for the same reason.  Everything the banner in
 * dlls/d3d11/unix.c says about Wine threads applies here word for word.
 * ====================================================================== */

/* DEFINED IN unix_wsi_window.c -- see the note on the same line of
 * dlls/d3d11/unix.c.  d3d9.so links its own copy of that file and so gets its
 * own object, which is what the paragraph above is about. */
extern __thread int wsi_on_wine_thread;

/* unix_wsi_window.c, the shared win32u WINDOW seam. */
extern void wsiwin_ops_init( struct dxvk_win32u_wsi_ops *ops );
extern int  wsiwin_client_size( UINT64 hwnd, unsigned int *width, unsigned int *height );

struct present_surface
{
    struct present_surface *next;
    UINT64 hwnd;
    UINT64 surface;
    void *cookie;
    int orphaned;
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

#define VK_EXTENSION_PROPERTIES_SIZE 260

typedef int (*enum_instance_ext_fn)( const char *, unsigned int *, void * );

static const char *ext_names[4];

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
             "named, so every D3D9 device in this process will be refused\n",
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
             "a DXVK worker thread does not have.  The device will report "
             "failure rather than fault.\n", (void *)(ULONG_PTR)hwnd );
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
        if (wsi_on_wine_thread) *link = p->next;
        else p->orphaned = 1;
        break;
    }
    if (p && wsi_on_wine_thread) drain_orphans();
    pthread_mutex_unlock( &present_lock );

    if (!p || !wsi_on_wine_thread) return;
    hwndsurf_destroy( p->cookie );
    free( p );
}

static int w32u_window_size( UINT64 hwnd, UINT32 *width, UINT32 *height )
{
    struct hwnd_state *s;
    unsigned int w, h;
    int found = 0;

    /* Ask win32u first, fall back to what the PE side pushed -- verbatim from
     * dlls/d3d11/unix.c, where the reason is written out.  In short: DXVK can
     * move this window now, and the pushed size is only as fresh as the last
     * present. */
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

    pthread_mutex_lock( &present_lock );
    for (s = hwnd_states; s; s = s->next)
        if (s->hwnd == hwnd) { valid = s->valid != 0; break; }
    pthread_mutex_unlock( &present_lock );
    return valid;
}

/* NOT const, for the reason dlls/d3d11/unix.c gives at the same place: the
 * abi-2 half is filled in at registration time, and WINEDXVKNOWINDOWOPS=1
 * leaves it out. */
static struct dxvk_win32u_wsi_ops win32u_wsi_ops =
{
    .abi = DXVK_WIN32U_WSI_ABI,
    .instance_extensions = w32u_instance_extensions,
    .surface_create = w32u_surface_create,
    .surface_destroy = w32u_surface_destroy,
    .window_size = w32u_window_size,
    .is_window = w32u_is_window,
};

static NTSTATUS d3d9_unix_present( void *args )
{
    struct d3d9_present_params *p = args;
    struct present_surface *s;
    void *cookie = NULL;

    pthread_mutex_lock( &present_lock );
    drain_orphans();
    for (s = present_surfaces; s; s = s->next)
        if (s->hwnd == p->hwnd && !s->orphaned) { cookie = s->cookie; break; }
    pthread_mutex_unlock( &present_lock );

    if (!cookie) return STATUS_SUCCESS;

    if (p->phase == D3D9_PRESENT_BEGIN) hwndsurf_update( cookie );
    else hwndsurf_presented( cookie, p->result );
    return STATUS_SUCCESS;
}

static NTSTATUS d3d9_unix_hwnd( void *args )
{
    struct d3d9_hwnd_params *p = args;
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

static NTSTATUS d3d9_unix_init( void *args )
{
    dxvk_wsi_win32u_register_fn reg;
    unsigned int i;

    if (dxvk_handle) return STATUS_SUCCESS;

    /* PICK A WSI BACKEND BEFORE DXVK IS LOADED.  D3D9 needs one even harder
     * than D3D11 did: there is no offscreen D3D9 device at all -- the implicit
     * swapchain is created inside CreateDevice (D3D9SwapChainEx's constructor
     * calls UpdateWindowCtx unconditionally), so a device with no window
     * system is a device that does not exist.  Win32u is this lane's own
     * backend and forwards every question to the table registered below. */
    setenv( "DXVK_WSI_DRIVER", "Win32u", 0 );

    if (!(dxvk_handle = load_dxvk_lib( D3D9_SONAME )))
    {
        ERR( "cannot load %s: %s -- did the build's DXVK step run?  "
             "(ppc64le/dxvk/build-for-wine.sh; DXVK_LIB_DIR overrides the "
             "search)\n", D3D9_SONAME, dlerror() );
        return STATUS_DLL_NOT_FOUND;
    }
    for (i = 0; i < FLAT_FUNC_COUNT; i++)
    {
        if (!(flat_funcs[i] = (wide_func)(ULONG_PTR)dlsym( dxvk_handle, flat_names[i] )))
        {
            ERR( "%s exports no %s: %s\n", D3D9_SONAME, flat_names[i], dlerror() );
            return STATUS_ENTRYPOINT_NOT_FOUND;
        }
    }
    /* Before the library sees the table, and once: the abi-2 half, or the
     * deliberate absence of it.  See the note on win32u_wsi_ops. */
    wsiwin_ops_init( &win32u_wsi_ops );

    if (!(reg = (dxvk_wsi_win32u_register_fn)(ULONG_PTR)
                dlsym( dxvk_handle, DXVK_WIN32U_WSI_REGISTER_NAME )) ||
        !reg( &win32u_wsi_ops ))
    {
        ERR( "%s did not accept the win32u WSI table -- every D3D9 device in "
             "this process will be refused.  Was DXVK built from "
             "ppc64le/dxvk/dxvk-patches/ (bootstrap.sh --check)?\n", D3D9_SONAME );
        return STATUS_ENTRYPOINT_NOT_FOUND;
    }
    TRACE( "loaded DXVK's d3d9 with %u flat entries and the win32u WSI table\n", i );
    return STATUS_SUCCESS;
}

static UINT64 call_wide( wide_func fn, const UINT64 *a )
{
    return fn( a[0], a[1], a[2],  a[3],  a[4],  a[5],  a[6],  a[7],
               a[8], a[9], a[10], a[11], a[12], a[13], a[14], a[15] );
}

static NTSTATUS d3d9_unix_call( void *args )
{
    struct d3d9_call_params *p = args;
    void **vtbl;

    if (!p->args[0]) return STATUS_INVALID_PARAMETER;
    vtbl = *(void ***)(ULONG_PTR)p->args[0];
    p->ret = call_wide( (wide_func)vtbl[p->slot], p->args );
    return STATUS_SUCCESS;
}

/* The float-bearing slots, each with its REAL prototype rather than a cast of
 * the wide form: on ELFv2 a float argument lives in f1..f13 and a float return
 * comes back in f1, neither of which the widest-integer form touches.  A wrong
 * clear depth is not a crash, it is a frame that looks nearly right. */
static NTSTATUS d3d9_unix_float( void *args )
{
    struct d3d9_float_params *p = args;
    void **vtbl;

    if (!p->self) return STATUS_INVALID_PARAMETER;
    vtbl = *(void ***)(ULONG_PTR)p->self;
    switch (p->shape)
    {
    case D3D9_FLOAT_CLEAR:
    {
        UINT64 (*fn)( void *, DWORD, const void *, DWORD, DWORD, float, DWORD ) =
            (void *)vtbl[p->slot];
        p->ret = fn( (void *)(ULONG_PTR)p->self, (DWORD)p->a,
                     (const void *)(ULONG_PTR)p->b, (DWORD)p->c, (DWORD)p->d,
                     p->f, (DWORD)p->e );
        return STATUS_SUCCESS;
    }
    case D3D9_FLOAT_SET:
    {
        UINT64 (*fn)( void *, float ) = (void *)vtbl[p->slot];
        p->ret = fn( (void *)(ULONG_PTR)p->self, p->f );
        return STATUS_SUCCESS;
    }
    case D3D9_FLOAT_GET:
    {
        float (*fn)( void * ) = (void *)vtbl[p->slot];
        p->ret_f = fn( (void *)(ULONG_PTR)p->self );
        return STATUS_SUCCESS;
    }
    }
    return STATUS_INVALID_PARAMETER;
}

static NTSTATUS d3d9_unix_flat( void *args )
{
    struct d3d9_flat_params *p = args;
    UINT64 a[D3D9_UNIX_MAX_ARGS] = { 0 };

    if (p->func >= FLAT_FUNC_COUNT || !flat_funcs[p->func])
        return STATUS_INVALID_PARAMETER;
    memcpy( a, p->args, sizeof(p->args) );
    p->ret = call_wide( flat_funcs[p->func], a );
    return STATUS_SUCCESS;
}

/* See the identical banner in dlls/d3d11/unix.c: this is the one place that
 * knows the caller is on a Wine thread, and the WSI callbacks above are the
 * only readers of it. */
#define WINE_THREAD_ENTRY(name, impl)                                        \
    static NTSTATUS name( void *args )                                       \
    {                                                                        \
        NTSTATUS status;                                                     \
        wsi_on_wine_thread++;                                                \
        status = impl( args );                                               \
        wsi_on_wine_thread--;                                                \
        return status;                                                       \
    }

WINE_THREAD_ENTRY( d3d9_enter_init,    d3d9_unix_init )
WINE_THREAD_ENTRY( d3d9_enter_call,    d3d9_unix_call )
WINE_THREAD_ENTRY( d3d9_enter_float,   d3d9_unix_float )
WINE_THREAD_ENTRY( d3d9_enter_flat,    d3d9_unix_flat )
WINE_THREAD_ENTRY( d3d9_enter_present, d3d9_unix_present )
WINE_THREAD_ENTRY( d3d9_enter_hwnd,    d3d9_unix_hwnd )

const unixlib_entry_t __wine_unix_call_funcs[] =
{
    d3d9_enter_init,
    d3d9_enter_call,
    d3d9_enter_float,
    d3d9_enter_flat,
    d3d9_enter_present,
    d3d9_enter_hwnd,
};

C_ASSERT( ARRAYSIZE(__wine_unix_call_funcs) == unix_funcs_count );
