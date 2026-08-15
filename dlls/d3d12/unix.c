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
 * vkd3d-proton is found via VKD3D_PROTON_LIB_DIR when set (the bring-up
 * arrangement, pointing at a build tree), or the ordinary dynamic-linker
 * search otherwise.  The d3d12 front-end .so loads -d3d12core.so itself, the
 * same way upstream's own front-end does -- we deliberately reuse that
 * mechanism rather than duplicating its search logic here.
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
    char path[512];
    unsigned int i;

    if (vkd3d_handle) return STATUS_SUCCESS;

    if (dir && dir[0])
    {
        snprintf( path, sizeof(path), "%s/%s", dir, VKD3D_SONAME );
        vkd3d_handle = dlopen( path, RTLD_NOW | RTLD_LOCAL );
        if (!vkd3d_handle)
            ERR( "cannot load %s: %s\n", path, dlerror() );
    }
    if (!vkd3d_handle)
        vkd3d_handle = dlopen( VKD3D_SONAME, RTLD_NOW | RTLD_LOCAL );
    if (!vkd3d_handle)
    {
        ERR( "cannot load %s: %s -- set VKD3D_PROTON_LIB_DIR or put it on "
             "the linker path\n", VKD3D_SONAME, dlerror() );
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

const unixlib_entry_t __wine_unix_call_funcs[] =
{
    d3d12_unix_init,
    d3d12_unix_call,
    d3d12_unix_flat,
};

C_ASSERT( ARRAYSIZE(__wine_unix_call_funcs) == unix_funcs_count );
