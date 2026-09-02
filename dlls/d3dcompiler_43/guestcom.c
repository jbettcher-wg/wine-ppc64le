/*
 * d3dcompiler for x86-64 guests on native ppc64le Wine -- the module-side
 * winecom runtime instance and the flat-export wrappers.
 *
 * WHY THIS FILE EXISTS, found the hard way on 2026-09-01: this module's
 * .thunks was FROM-SPEC auto with no COM-JSON, so every export crossed as
 * plain scalars -- including D3DCompile2's `ID3DBlob **ppCode`.  The native
 * compiler wrote a NATIVE ppc64 blob pointer into the guest's cell, and the
 * guest's very next line -- blob->GetBufferPointer(), always -- was an x86-64
 * `call [rax+0x18]` into ppc64 bytes.  The Witcher 3 compiles shaders while
 * loading a save; its worker thread executed wined3d.dll's bytes as x86 and
 * the fault report's native-pc line named the class.  DOOM and Cyberpunk ship
 * precompiled DXBC and never call D3DCompile, which is why this sat unfound
 * beneath every earlier title.
 *
 * The shape is dlls/dinput8/guestcom.c's, cut down: ONE real interface.
 * Everything D3DCompile-family vends is an ID3DBlob -- two parameterless
 * getters -- so the roster is ID3D10Blob (its widl name) + IUnknown, the
 * proxy's data is served by two marshalled slots, and there is nothing to
 * hand-write.  The reflection/linker family (D3DReflect, D3DCreateLinker,
 * D3DCreateFunctionLinkingGraph, D3DLoadModule) vends LARGE interfaces no
 * guest title has driven on this port; those exports are GUEST-REFUSE in the
 * .thunks -- a loud E_NOTIMPL through __wine_com_refuse below, never a
 * pass-through -- until a title demands them.
 *
 * Copyright 2026 the ppc64le port authors
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301, USA
 */

#include <stdarg.h>

#include "windef.h"
#include "winbase.h"
#include "winternl.h"
#include "objbase.h"

#include "d3dcompiler.h"

#include "wine/debug.h"
#include "wine/winecom.h"

#include "d3dcompiler_marshal.h"

WINE_DEFAULT_DEBUG_CHANNEL(d3dcompiler);

/* ------------------------------------------------------- the host invoker */

/* A direct widest-form native vtable call, exactly dinput8's and syscom's:
 * these blobs are ordinary native COM objects in this process.  The widest
 * slot here takes ONE argument (`this`); the shape stays the shared one so a
 * roster addition never has to revisit it. */
static UINT64 d3dcompiler_invoke( void *host, UINT slot, UINT argc, UINT64 *args )
{
    void **vtbl = *(void ***)host;

    args[0] = (UINT64)(ULONG_PTR)host;
    return ((UINT64 (*)( ULONG_PTR, ULONG_PTR, ULONG_PTR, ULONG_PTR, ULONG_PTR,
                         ULONG_PTR, ULONG_PTR, ULONG_PTR, ULONG_PTR, ULONG_PTR,
                         ULONG_PTR, ULONG_PTR, ULONG_PTR, ULONG_PTR, ULONG_PTR,
                         ULONG_PTR ))vtbl[slot])
        ( args[0], args[1], args[2],  args[3],  args[4],  args[5],  args[6],
          args[7], args[8], args[9],  args[10], args[11], args[12], args[13],
          args[14], args[15] );
}

/* This file is shared through PARENTSRC: each versioned module attaches the
 * surface to ITS OWN guest thunk namesake, or the cross-check would hunt a
 * module the process never loaded. */
#define GUEST_MODULE_1(v) L"d3dcompiler_" #v ".dll"
#define GUEST_MODULE(v) GUEST_MODULE_1(v)
static const WCHAR *const d3dcompiler_guest_modules[] =
    { GUEST_MODULE(D3D_COMPILER_VERSION) };

static const struct winecom_surface d3dcompiler_surface =
{
    .name = "d3dcompiler",
    .guest_modules = d3dcompiler_guest_modules,
    .module_count = ARRAYSIZE(d3dcompiler_guest_modules),
    .ifaces = d3dcompiler_com_ifaces,
    .iface_count = D3DCOMPILER_IFACE_COUNT,
    .invoke = d3dcompiler_invoke,
    /* no hand functions, no reverse direction: nothing on this surface takes
     * a guest object or a guest callback -- ID3DInclude, the one exception,
     * is refused per call in the wrappers below with the reason at the site */
};

static BOOL d3dcompiler_com_ready(void)
{
    return winecom_attach( &d3dcompiler_surface );
}

/* ---------------------------------------------------- exported dispatch */

NTSTATUS WINAPI __wine_com_dispatch( UINT iface, UINT slot, AMD64_CONTEXT *ctx )
{
    if (!d3dcompiler_com_ready()) return STATUS_DLL_INIT_FAILED;
    return winecom_dispatch( iface, slot, ctx );
}

BOOL WINAPI __wine_com_slot_name( UINT iface, UINT slot, const char **iface_name,
                                  const char **slot_name )
{
    return winecom_slot_names( iface, slot, iface_name, slot_name );
}

/* The shared loud-refusal stub every GUEST-REFUSE export resolves to; the
 * trapping export's own name is in the thunk dispatcher's TRACE.  Same
 * symbol, same contract, as dinput8's and syscom's. */
HRESULT WINAPI __wine_com_refuse(void)
{
    ERR( "d3dcompiler: refusing an interface-bearing flat export with no "
         "wrapper (see the guest thunk trace for which)\n" );
    return E_NOTIMPL;
}

/* ------------------------------------------------------ the flat wrappers */

/* Every wrapper ends here: blob out-cells become proxies.  An error blob is
 * written by the compiler ON FAILURE -- that is its whole purpose -- so the
 * wrap must not be gated on SUCCEEDED(hr); winecom_wrap_static wraps whatever
 * is there and leaves a NULL cell alone. */
static HRESULT wrap_blobs( HRESULT hr, ID3DBlob **a, ID3DBlob **b )
{
    winecom_wrap_static( (void **)a, D3DCOMPILER_IFACE_ID3D10Blob );
    winecom_wrap_static( (void **)b, D3DCOMPILER_IFACE_ID3D10Blob );
    return hr;
}

/* Refusal hygiene, the lesson of the same day this file was written (see
 * dlls/combase/syscom.c 690567d9d8e): a refusal owns its out-params.  Both
 * blob cells are NULLed BEFORE any refusing return, or the caller reads its
 * own stack residue back through them. */
static HRESULT refuse_with_nulls( const char *fn, const char *why,
                                  ID3DBlob **a, ID3DBlob **b )
{
    FIXME( "d3dcompiler: %s is refused for a guest because %s\n", fn, why );
    /* the lever-honouring family, not inline stores: WINEEMUNOREFUSESCRUB=1
     * must be able to turn these off, or the hygiene gate's sabotage arm
     * cannot prove them load-bearing (winecom.h, the by-hand scrub banner) */
    winecom_refused_scrub_ptr( a );
    winecom_refused_scrub_ptr( b );
    return E_NOTIMPL;
}

/* ID3DInclude is the one interface that flows INTO this surface, and it is a
 * guest-implemented vtable the COMPILER calls back into -- the reverse
 * direction.  NULL and the magic D3D_COMPILE_STANDARD_FILE_INCLUDE constant
 * (a non-pointer sentinel the native side interprets itself) both cross
 * fine; a real guest include handler would hand native code an x86-64
 * vtable, so it is refused with the blob cells scrubbed. */
static BOOL include_crosses( ID3DInclude *include )
{
    return !include || include == D3D_COMPILE_STANDARD_FILE_INCLUDE;
}

HRESULT WINAPI __wine_guest_D3DCompile( const void *data, SIZE_T size,
        const char *filename, const D3D_SHADER_MACRO *defines,
        ID3DInclude *include, const char *entrypoint, const char *target,
        UINT flags1, UINT flags2, ID3DBlob **shader, ID3DBlob **errors )
{
    if (!d3dcompiler_com_ready())
        return refuse_with_nulls( "D3DCompile", "the COM runtime did not attach",
                                  shader, errors );
    if (!include_crosses( include ))
        return refuse_with_nulls( "D3DCompile", "its ID3DInclude is a guest "
                                  "vtable native code would call", shader, errors );
    return wrap_blobs( D3DCompile( data, size, filename, defines, include,
                                   entrypoint, target, flags1, flags2,
                                   shader, errors ), shader, errors );
}

HRESULT WINAPI __wine_guest_D3DCompile2( const void *data, SIZE_T size,
        const char *filename, const D3D_SHADER_MACRO *defines,
        ID3DInclude *include, const char *entrypoint, const char *target,
        UINT flags1, UINT flags2, UINT secondary_flags,
        const void *secondary_data, SIZE_T secondary_size,
        ID3DBlob **shader, ID3DBlob **errors )
{
    if (!d3dcompiler_com_ready())
        return refuse_with_nulls( "D3DCompile2", "the COM runtime did not attach",
                                  shader, errors );
    if (!include_crosses( include ))
        return refuse_with_nulls( "D3DCompile2", "its ID3DInclude is a guest "
                                  "vtable native code would call", shader, errors );
    return wrap_blobs( D3DCompile2( data, size, filename, defines, include,
                                    entrypoint, target, flags1, flags2,
                                    secondary_flags, secondary_data,
                                    secondary_size, shader, errors ),
                       shader, errors );
}

HRESULT WINAPI __wine_guest_D3DCompileFromFile( const WCHAR *filename,
        const D3D_SHADER_MACRO *defines, ID3DInclude *include,
        const char *entrypoint, const char *target, UINT flags1, UINT flags2,
        ID3DBlob **code, ID3DBlob **errors )
{
    if (!d3dcompiler_com_ready())
        return refuse_with_nulls( "D3DCompileFromFile",
                                  "the COM runtime did not attach", code, errors );
    if (!include_crosses( include ))
        return refuse_with_nulls( "D3DCompileFromFile", "its ID3DInclude is a "
                                  "guest vtable native code would call",
                                  code, errors );
    return wrap_blobs( D3DCompileFromFile( filename, defines, include,
                                           entrypoint, target, flags1, flags2,
                                           code, errors ), code, errors );
}

HRESULT WINAPI __wine_guest_D3DPreprocess( const void *data, SIZE_T size,
        const char *filename, const D3D_SHADER_MACRO *defines,
        ID3DInclude *include, ID3DBlob **shader, ID3DBlob **errors )
{
    if (!d3dcompiler_com_ready())
        return refuse_with_nulls( "D3DPreprocess",
                                  "the COM runtime did not attach", shader, errors );
    if (!include_crosses( include ))
        return refuse_with_nulls( "D3DPreprocess", "its ID3DInclude is a guest "
                                  "vtable native code would call", shader, errors );
    return wrap_blobs( D3DPreprocess( data, size, filename, defines, include,
                                      shader, errors ), shader, errors );
}

HRESULT WINAPI __wine_guest_D3DDisassemble( const void *data, SIZE_T size,
        UINT flags, const char *comments, ID3DBlob **disassembly )
{
    if (!d3dcompiler_com_ready())
        return refuse_with_nulls( "D3DDisassemble",
                                  "the COM runtime did not attach", disassembly, NULL );
    return wrap_blobs( D3DDisassemble( data, size, flags, comments, disassembly ),
                       disassembly, NULL );
}

HRESULT WINAPI __wine_guest_D3DGetBlobPart( const void *data, SIZE_T size,
        D3D_BLOB_PART part, UINT flags, ID3DBlob **blob )
{
    if (!d3dcompiler_com_ready())
        return refuse_with_nulls( "D3DGetBlobPart",
                                  "the COM runtime did not attach", blob, NULL );
    return wrap_blobs( D3DGetBlobPart( data, size, part, flags, blob ), blob, NULL );
}

HRESULT WINAPI __wine_guest_D3DGetDebugInfo( const void *data, SIZE_T size,
        ID3DBlob **blob )
{
    if (!d3dcompiler_com_ready())
        return refuse_with_nulls( "D3DGetDebugInfo",
                                  "the COM runtime did not attach", blob, NULL );
    return wrap_blobs( D3DGetDebugInfo( data, size, blob ), blob, NULL );
}

HRESULT WINAPI __wine_guest_D3DGetInputSignatureBlob( const void *data,
        SIZE_T size, ID3DBlob **blob )
{
    if (!d3dcompiler_com_ready())
        return refuse_with_nulls( "D3DGetInputSignatureBlob",
                                  "the COM runtime did not attach", blob, NULL );
    return wrap_blobs( D3DGetInputSignatureBlob( data, size, blob ), blob, NULL );
}

HRESULT WINAPI __wine_guest_D3DGetOutputSignatureBlob( const void *data,
        SIZE_T size, ID3DBlob **blob )
{
    if (!d3dcompiler_com_ready())
        return refuse_with_nulls( "D3DGetOutputSignatureBlob",
                                  "the COM runtime did not attach", blob, NULL );
    return wrap_blobs( D3DGetOutputSignatureBlob( data, size, blob ), blob, NULL );
}

HRESULT WINAPI __wine_guest_D3DGetInputAndOutputSignatureBlob( const void *data,
        SIZE_T size, ID3DBlob **blob )
{
    if (!d3dcompiler_com_ready())
        return refuse_with_nulls( "D3DGetInputAndOutputSignatureBlob",
                                  "the COM runtime did not attach", blob, NULL );
    return wrap_blobs( D3DGetInputAndOutputSignatureBlob( data, size, blob ),
                       blob, NULL );
}

HRESULT WINAPI __wine_guest_D3DStripShader( const void *data, SIZE_T size,
        UINT flags, ID3DBlob **blob )
{
    if (!d3dcompiler_com_ready())
        return refuse_with_nulls( "D3DStripShader",
                                  "the COM runtime did not attach", blob, NULL );
    return wrap_blobs( D3DStripShader( data, size, flags, blob ), blob, NULL );
}

HRESULT WINAPI __wine_guest_D3DCreateBlob( SIZE_T size, ID3DBlob **blob )
{
    if (!d3dcompiler_com_ready())
        return refuse_with_nulls( "D3DCreateBlob",
                                  "the COM runtime did not attach", blob, NULL );
    return wrap_blobs( D3DCreateBlob( size, blob ), blob, NULL );
}

HRESULT WINAPI __wine_guest_D3DReadFileToBlob( const WCHAR *filename,
        ID3DBlob **contents )
{
    if (!d3dcompiler_com_ready())
        return refuse_with_nulls( "D3DReadFileToBlob",
                                  "the COM runtime did not attach", contents, NULL );
    return wrap_blobs( D3DReadFileToBlob( filename, contents ), contents, NULL );
}

/* The one blob IN: the guest hands back a proxy this file minted (or, in
 * principle, its own ID3DBlob implementation, which gets a reverse... no --
 * this surface has no reverse direction, so a guest-implemented blob is
 * refused by winecom_to_native and so is the call).  winecom_to_native
 * returns the ORIGINAL host object for a proxy of ours, which is the case
 * that exists: D3DCompile -> proxy -> D3DWriteBlobToFile. */
HRESULT WINAPI __wine_guest_D3DWriteBlobToFile( ID3DBlob *blob,
        const WCHAR *filename, BOOL overwrite )
{
    void *native = NULL;
    HRESULT hr;

    if (!d3dcompiler_com_ready())
        return refuse_with_nulls( "D3DWriteBlobToFile",
                                  "the COM runtime did not attach", NULL, NULL );
    if (blob && !winecom_to_native( blob, D3DCOMPILER_IFACE_ID3D10Blob, &native ))
        return refuse_with_nulls( "D3DWriteBlobToFile", "its blob is not one "
                                  "this surface handed out", NULL, NULL );
    hr = D3DWriteBlobToFile( (ID3DBlob *)native, filename, overwrite );
    if (native) winecom_to_native_end( native );
    return hr;
}
