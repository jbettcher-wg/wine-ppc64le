/*
 * d3d12 unixlib -- vkd3d-proton lives here.
 *
 * This is the bottom of the native-lane D3D12 stack (design §3.4): dlopen
 * vkd3d-proton's own libvkd3d-proton-d3d12.so, resolve its eight flat entry
 * points, and call COM vtable slots with the widest integer form.  Nothing
 * here knows what any slot means; the PE side's marshal tables decided which
 * slots may cross at all, and everything that crosses is integer-class by
 * construction (extra arguments are harmless on ELFv2 -- the callee ignores
 * the high registers, the same argument dlls/ntdll's call_native_thunk
 * makes).
 *
 * vkd3d-proton is found, in order: VKD3D_PROTON_LIB_DIR when set (an
 * override for pointing at a scratch build); next to this unixlib itself,
 * which is where the Wine build puts it -- the Makefile rule added by
 * configure.ac drives vkd3d's meson build and symlinks the front end into
 * dlls/d3d12/ beside d3d12.so, so a clean `./configure && make` needs no
 * environment at all; and finally the ordinary dynamic-linker search, which
 * is what a system-installed copy resolves through.  The front-end .so loads
 * -d3d12core.so itself, the same way upstream's own front end does (its
 * build-tree DT_RUNPATH survives the symlink, since $ORIGIN is resolved
 * after following it) -- we deliberately reuse that mechanism rather than
 * duplicating its search logic here.
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

#include "ntstatus.h"
#define WIN32_NO_STATUS
#include "windef.h"
#include "winternl.h"
#include "wine/unixlib.h"
#include "wine/debug.h"

#include "unixlib.h"

WINE_DEFAULT_DEBUG_CHANNEL(d3d12);

#define VKD3D_SONAME "libvkd3d-proton-d3d12.so"

static const char *flat_names[FLAT_FUNC_COUNT] =
{
    "D3D12CreateDevice",
    "D3D12GetDebugInterface",
    "D3D12CreateRootSignatureDeserializer",
    "D3D12CreateVersionedRootSignatureDeserializer",
    "D3D12EnableExperimentalFeatures",
    "D3D12SerializeRootSignature",
    "D3D12SerializeVersionedRootSignature",
    "D3D12GetInterface",
};

typedef UINT64 (*wide_func)( UINT64, UINT64, UINT64, UINT64, UINT64, UINT64,
                             UINT64, UINT64, UINT64, UINT64, UINT64, UINT64,
                             UINT64, UINT64, UINT64, UINT64 );

static void *vkd3d_handle;
static wide_func flat_funcs[FLAT_FUNC_COUNT];

static NTSTATUS d3d12_unix_init( void *args )
{
    const char *dir = getenv( "VKD3D_PROTON_LIB_DIR" );
    char path[1024];
    Dl_info info;
    unsigned int i;

    if (vkd3d_handle) return STATUS_SUCCESS;

    if (dir && dir[0])
    {
        snprintf( path, sizeof(path), "%s/%s", dir, VKD3D_SONAME );
        vkd3d_handle = dlopen( path, RTLD_NOW | RTLD_LOCAL );
        if (!vkd3d_handle)
            ERR( "cannot load %s: %s\n", path, dlerror() );
    }
    /* The build places a symlink to vkd3d-proton next to this unixlib.  It
     * must be resolved before the dlopen: the front end finds its
     * -d3d12core.so half through DT_RUNPATH=$ORIGIN/../d3d12core, and glibc
     * expands $ORIGIN from the path the object was loaded by, NOT from its
     * realpath -- measured here: loading through the symlink loses d3d12core. */
    if (!vkd3d_handle && dladdr( (void *)d3d12_unix_init, &info ) && info.dli_fname)
    {
        const char *slash = strrchr( info.dli_fname, '/' );
        if (slash && (size_t)(slash - info.dli_fname) + sizeof(VKD3D_SONAME) + 1 < sizeof(path))
        {
            char *real;
            snprintf( path, sizeof(path), "%.*s/%s",
                      (int)(slash - info.dli_fname), info.dli_fname, VKD3D_SONAME );
            if ((real = realpath( path, NULL )))
            {
                vkd3d_handle = dlopen( real, RTLD_NOW | RTLD_LOCAL );
                if (!vkd3d_handle)
                    WARN( "cannot load %s: %s\n", real, dlerror() );
                free( real );
            }
            else TRACE( "no %s beside the unixlib, trying the linker path\n", VKD3D_SONAME );
        }
    }
    if (!vkd3d_handle)
        vkd3d_handle = dlopen( VKD3D_SONAME, RTLD_NOW | RTLD_LOCAL );
    if (!vkd3d_handle)
    {
        ERR( "cannot load %s: %s -- did the build's vkd3d step run?  "
             "(VKD3D_PROTON_LIB_DIR overrides the search)\n", VKD3D_SONAME, dlerror() );
        return STATUS_DLL_NOT_FOUND;
    }
    for (i = 0; i < FLAT_FUNC_COUNT; i++)
    {
        if (!(flat_funcs[i] = (wide_func)(ULONG_PTR)dlsym( vkd3d_handle,
                                                           flat_names[i] )))
        {
            ERR( "%s exports no %s: %s\n", VKD3D_SONAME, flat_names[i],
                 dlerror() );
            dlclose( vkd3d_handle );
            vkd3d_handle = NULL;
            return STATUS_ENTRYPOINT_NOT_FOUND;
        }
    }
    TRACE( "loaded %s with %u flat entries\n", VKD3D_SONAME, i );
    return STATUS_SUCCESS;
}

static UINT64 call_wide( wide_func fn, const UINT64 *a )
{
    return fn( a[0], a[1], a[2],  a[3],  a[4],  a[5],  a[6],  a[7],
               a[8], a[9], a[10], a[11], a[12], a[13], a[14], a[15] );
}

static NTSTATUS d3d12_unix_call( void *args )
{
    struct d3d12_call_params *p = args;
    void **vtbl;

    if (!p->args[0]) return STATUS_INVALID_PARAMETER;
    vtbl = *(void ***)(ULONG_PTR)p->args[0];
    p->ret = call_wide( (wide_func)vtbl[p->slot], p->args );
    return STATUS_SUCCESS;
}

static NTSTATUS d3d12_unix_flat( void *args )
{
    struct d3d12_flat_params *p = args;
    UINT64 a[D3D12_UNIX_MAX_ARGS] = { 0 };

    if (p->func >= FLAT_FUNC_COUNT || !flat_funcs[p->func])
        return STATUS_INVALID_PARAMETER;
    memcpy( a, p->args, sizeof(p->args) );
    p->ret = call_wide( flat_funcs[p->func], a );
    return STATUS_SUCCESS;
}

/* A float-bearing vtable slot: the integer-wide form cannot place the
 * value, so each served shape is called through its real prototype and the
 * float is reconstituted from the raw bits the PE side sent (unixlib.h,
 * enum d3d12_fp_shape). */
static NTSTATUS d3d12_unix_call_fp( void *args )
{
    struct d3d12_fp_call_params *p = args;
    void **vtbl;

    if (!p->args[0]) return STATUS_INVALID_PARAMETER;
    vtbl = *(void ***)(ULONG_PTR)p->args[0];
    switch (p->shape)
    {
    case FP_SHAPE_CLEAR_DSV:
    {
        /* D3D12_CPU_DESCRIPTOR_HANDLE is struct { SIZE_T } -- ELFv2 passes
         * the one-member aggregate in a GPR, so UINT64 is its exact shape. */
        typedef void (*clear_dsv_fn)( void *iface, UINT64 dsv_handle,
                                      unsigned int clear_flags, float depth,
                                      unsigned char stencil,
                                      unsigned int num_rects,
                                      const void *rects );
        union { UINT32 bits; float f; } depth;

        depth.bits = (UINT32)p->args[3];
        ((clear_dsv_fn)vtbl[p->slot])( (void *)(ULONG_PTR)p->args[0],
                                       p->args[1], (unsigned int)p->args[2],
                                       depth.f, (unsigned char)p->args[4],
                                       (unsigned int)p->args[5],
                                       (const void *)(ULONG_PTR)p->args[6] );
        return STATUS_SUCCESS;
    }
    case FP_SHAPE_DEPTH_BOUNDS:
    {
        typedef void (*depth_bounds_fn)( void *iface, float min, float max );
        union { UINT32 bits; float f; } fmin, fmax;

        fmin.bits = (UINT32)p->args[1];
        fmax.bits = (UINT32)p->args[2];
        ((depth_bounds_fn)vtbl[p->slot])( (void *)(ULONG_PTR)p->args[0],
                                          fmin.f, fmax.f );
        return STATUS_SUCCESS;
    }
    case FP_SHAPE_DEPTH_BIAS:
    {
        typedef void (*depth_bias_fn)( void *iface, float bias, float clamp,
                                       float slope_scaled_bias );
        union { UINT32 bits; float f; } bias, clamp, slope;

        bias.bits  = (UINT32)p->args[1];
        clamp.bits = (UINT32)p->args[2];
        slope.bits = (UINT32)p->args[3];
        ((depth_bias_fn)vtbl[p->slot])( (void *)(ULONG_PTR)p->args[0],
                                        bias.f, clamp.f, slope.f );
        return STATUS_SUCCESS;
    }
    }
    return STATUS_INVALID_PARAMETER;
}

/* unix_present.c: the presentation factory (design §4). */
extern NTSTATUS d3d12_unix_present_factory( void *args );

const unixlib_entry_t __wine_unix_call_funcs[] =
{
    d3d12_unix_init,
    d3d12_unix_call,
    d3d12_unix_flat,
    d3d12_unix_present_factory,
    d3d12_unix_call_fp,
};

C_ASSERT( ARRAYSIZE(__wine_unix_call_funcs) == unix_funcs_count );
