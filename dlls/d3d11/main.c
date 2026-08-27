/*
 * Native d3d11.dll -- DXVK's PE front, and the single winecom client for the
 * whole D3D11/D3D10/DXGI surface.
 *
 * This module REPLACES Wine's wined3d-backed d3d11.  It is the native half of
 * the native-lane D3D11 stack, the same shape as dlls/d3d12 and vkd3d-proton:
 *
 *   guest x86-64 PE  -->  C:\windows\sysx8664\{d3d11,dxgi,d3d10core}.dll
 *      |                  (spec2thunk COM mode: pure trap surface, no
 *      |                   marshalling knowledge)
 *      |  trap; ntdll's dispatcher maps RIP -> (iface, slot) and calls the
 *      |  NATIVE namesake's __wine_com_dispatch
 *      v
 *   __wine_com_dispatch( iface, slot, AMD64_CONTEXT * )       <-- THIS FILE
 *      |  = libs/winecom's dispatch loop over the generated marshal table
 *      |  (d3d11_marshal.h, from ppc64le/dxvk/gen_winecom.py)
 *      v
 *   d3d11.so (unixlib, unix.c)  -->  libdxvk_d3d11.so + libdxvk_dxgi.so
 *
 * ONE RUNTIME FOR THREE DLLS.  libs/winecom's state is per-linkee.  If native
 * dxgi.dll linked its own copy, the IDXGIAdapter proxy it minted would not be
 * one of THIS module's proxies, and `D3D11CreateDevice(adapter, ...)` would be
 * refused as a guest-implemented object at the first call a real game makes.
 * So this module owns the only instance; native dxgi.dll and d3d10core.dll
 * forward both their flat exports and __wine_com_dispatch here through their
 * .spec files, and all three GUEST modules publish the same roster from
 * ppc64le/dxvk/interfaces_dxvk.json -- which is what makes a proxy's guest
 * vtable interchangeable between them, and what winecom_attach's IID
 * cross-check verifies for every one of them that is loaded.
 *
 * WHY THE GUEST-FACING FLAT ENTRIES HAVE THEIR OWN NAMES.  A proxy's vtable is
 * the guest module's array of x86-64 trap stubs.  Handing one to a NATIVE
 * ppc64 caller would have it execute those bytes as ppc64.  So the exports a
 * guest reaches are __wine_guest_*, named by GUEST-IMPL lines in the .thunks
 * files, and the plain-named exports -- which only a native ppc64 PE can
 * reach -- refuse loudly rather than hand out a proxy.  dlls/d3d12 predates
 * this and exports one name for both; this is the corrected shape.
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

#include "windef.h"
#include "winbase.h"
#include "winerror.h"   /* DXGI_ERROR_MORE_DATA, and nothing else */
#include "winternl.h"
/* The two questions DXVK's win32u WSI backend asks about a window, answered
 * here on the application's own thread and pushed down to the unixlib: see
 * the presentation banner below. */
#include "ntuser.h"
#include "wine/debug.h"
#include "wine/winecom.h"

#include "unixlib.h"
#include "d3d11_marshal.h"

WINE_DEFAULT_DEBUG_CHANNEL(d3d11);

/* This module marshals; it does not implement D3D11, so it needs none of
 * D3D11's types.  Everything below is an address or a machine word, and the
 * arity of each flat entry is asserted by its .spec line and by the clang
 * oracle that built the guest thunk from the same .spec.  Pulling in
 * Wine's d3d11.h to spell `ID3D11Device **` would only invite the question of
 * whose d3d11.h -- Wine's describes wined3d's implementation, and the layouts
 * that actually matter are checked by the marshal generator against DXVK's. */


/* ------------------------------------------------------------- unix calls */

static UINT64 unix_vtbl_call( void *host, UINT slot, UINT argc, UINT64 *args )
{
    struct d3d11_call_params p;
    NTSTATUS status;

    memcpy( p.args, args, sizeof(p.args) );
    p.args[0] = (UINT64)(ULONG_PTR)host;
    p.slot = slot;
    p.argc = argc;
    p.ret = 0;
    if ((status = D3D11_UNIX_CALL( call, &p )))
    {
        ERR( "unix call failed, status %08x\n", (UINT)status );
        return (UINT64)(UINT)E_FAIL;
    }
    return p.ret;
}

/* ------------------------------------------------- the runtime instance */

/* Every guest module that publishes this roster.  winecom_attach validates
 * ALL of them that are loaded and materialises the proxy vtables from the
 * first -- so a d3d11.dll and a dxgi.dll built from different JSONs is a load
 * failure here, never a call dispatched into the neighbouring slot.
 *
 * d3d10.dll is the fourth, and it has to be listed even though only two of its
 * exports are served: a D3D10 application imports d3d10.dll and NOTHING ELSE
 * on this surface, so it is the only module in the process publishing the
 * roster.  [MEASURED] without it, D3D10CreateDevice from such a guest returned
 * E_FAIL with `winecom: no guest thunk module in this process; COM dispatch
 * cannot work` -- the proxy vtables had nowhere to come from. */
static const WCHAR *const d3d11_guest_modules[] =
    { L"d3d11.dll", L"dxgi.dll", L"d3d10core.dll", L"d3d10.dll" };

static UINT64 hand_get_private_data( void *host, UINT slot, AMD64_CONTEXT *ctx );
static UINT64 hand_set_private_data( void *host, UINT slot, AMD64_CONTEXT *ctx );
static UINT64 hand_set_private_data_iface( void *host, UINT slot, AMD64_CONTEXT *ctx );
static UINT64 hand_clear_depth_stencil_view( void *host, UINT slot, AMD64_CONTEXT *ctx );
static UINT64 hand_set_resource_min_lod( void *host, UINT slot, AMD64_CONTEXT *ctx );
static UINT64 hand_get_resource_min_lod( void *host, UINT slot, AMD64_CONTEXT *ctx );
static UINT64 hand_d3d10_clear_depth_stencil_view( void *host, UINT slot, AMD64_CONTEXT *ctx );
static UINT64 hand_create_swapchain( void *host, UINT slot, AMD64_CONTEXT *ctx );
static UINT64 hand_create_swapchain_for_hwnd( void *host, UINT slot, AMD64_CONTEXT *ctx );
static UINT64 hand_swapchain_present( void *host, UINT slot, AMD64_CONTEXT *ctx );
static UINT64 hand_swapchain_present1( void *host, UINT slot, AMD64_CONTEXT *ctx );

static const winecom_hand_fn d3d11_hand_funcs[] =
{
    hand_get_private_data,
    hand_set_private_data,
    hand_set_private_data_iface,
    hand_clear_depth_stencil_view,
    hand_set_resource_min_lod,
    hand_get_resource_min_lod,
    hand_d3d10_clear_depth_stencil_view,
    hand_create_swapchain,
    hand_create_swapchain_for_hwnd,
    hand_swapchain_present,
    hand_swapchain_present1,
};

C_ASSERT( ARRAYSIZE(d3d11_hand_funcs) == D3D11_HAND_COUNT );

static const struct winecom_surface d3d11_surface =
{
    .name = "d3d11",
    .guest_modules = d3d11_guest_modules,
    .module_count = ARRAYSIZE(d3d11_guest_modules),
    .ifaces = d3d11_com_ifaces,
    .iface_count = D3D11_IFACE_COUNT,
    .invoke = unix_vtbl_call,
    .hand_funcs = d3d11_hand_funcs,
    .hand_count = D3D11_HAND_COUNT,
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
    if (D3D11_UNIX_CALL( init, NULL ) || !winecom_attach( &d3d11_surface ))
    {
        InterlockedExchange( &com_init_state, 3 );
        return FALSE;
    }
    InterlockedExchange( &com_init_state, 2 );
    return TRUE;
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

/* short spellings for the hand-written slots below */
static UINT64 read_arg( const AMD64_CONTEXT *ctx, UINT n )
{
    return winecom_read_arg( ctx, n );
}

/* IUnknown::AddRef on a HOST interface, then intern.  winecom_wrap CONSUMES a
 * host reference, so this pair leaves the host refcount where it was and adds
 * one guest reference to the (possibly already interned) proxy -- which is
 * exactly what a COM method that returns an interface owes its caller. */
static void *host_addref_wrap( void *host, UINT iface )
{
    UINT64 args[D3D11_UNIX_MAX_ARGS] = { 0 };

    if (!host) return NULL;
    unix_vtbl_call( host, 1 /* IUnknown::AddRef */, 1, args );
    return winecom_wrap( host, iface );
}

/* ------------------------------------------ the private-data side table
 *
 * ID3D11DeviceChild::GetPrivateData's out-parameter is `void *pData`, not
 * `void **` -- there is no slot flag that could mark it, and DXVK_THUNK_STRICT
 * on the old FEX stack could not warn about it either
 * (dxvk-ppc64le/docs/hazard-hunt.md §3.2).  On a GUID the application
 * registered through SetPrivateDataInterface it hands back a RAW NATIVE
 * pointer, and the application's next call through it executes ppc64 bytes as
 * x86-64.  It is the one hazard on this surface that is invisible to static
 * classification, so it is answered dynamically: remember what the guest
 * stored, and give the guest back what it stored.
 *
 * THE ENTRY HOLDS A REFERENCE ON THE CONTAINER, and that is not an oversight.
 * The table is keyed by host object address.  A host object destroyed while an
 * entry named it would let the allocator hand that address to a NEW object,
 * whose first GetPrivateData for the same GUID would return a dangling proxy
 * -- reachable, and silent.  Holding a reference makes the address unreusable
 * for as long as the entry exists.  The cost is that an object with a private
 * interface set on it lives until the entry is removed
 * (SetPrivateDataInterface(guid, NULL), or SetPrivateData on the same GUID),
 * which is bounded by the number of (object, GUID) pairs the application
 * actually creates and is what applications do with them anyway.
 */

struct private_iface
{
    struct private_iface *next;
    void *host;          /* container, one reference held */
    void *guest;         /* what the guest gave us, handed straight back */
    GUID guid;
    UINT iface;          /* roster index of `guest` */
};

static CRITICAL_SECTION priv_cs;
static CRITICAL_SECTION_DEBUG priv_cs_debug =
{
    0, 0, &priv_cs,
    { &priv_cs_debug.ProcessLocksList, &priv_cs_debug.ProcessLocksList },
      0, 0, { (DWORD_PTR)(__FILE__ ": d3d11 priv_cs") }
};
static CRITICAL_SECTION priv_cs = { &priv_cs_debug, -1, 0, 0, 0, 0 };

static struct private_iface *private_ifaces;

/* Detach and return the entry for (host, guid), or NULL.  Caller owns what
 * comes back, including the container reference. */
static struct private_iface *private_take( void *host, const GUID *guid )
{
    struct private_iface **link, *p = NULL;

    RtlEnterCriticalSection( &priv_cs );
    for (link = &private_ifaces; *link; link = &(*link)->next)
    {
        if ((*link)->host == host && IsEqualGUID( &(*link)->guid, guid ))
        {
            p = *link;
            *link = p->next;
            break;
        }
    }
    RtlLeaveCriticalSection( &priv_cs );
    return p;
}

static void private_drop( struct private_iface *p )
{
    if (!p) return;
    winecom_host_release( p->host );
    RtlFreeHeap( NtCurrentTeb()->Peb->ProcessHeap, 0, p );
}

/* ----------------------------------------------------- hand-written slots */

/* {ID3D11DeviceChild,ID3D11Device,ID3D10DeviceChild,ID3D10Device,IDXGIObject}
 * ::GetPrivateData( REFGUID guid, UINT *size, void *data ).  If the guest
 * registered an interface under this GUID, answer from the side table with
 * the GUEST proxy it stored; otherwise this is an opaque data blob and the
 * host answers. */
static UINT64 hand_get_private_data( void *host, UINT slot, AMD64_CONTEXT *ctx )
{
    const GUID *guid = (const GUID *)(ULONG_PTR)read_arg( ctx, 1 );
    UINT *size = (UINT *)(ULONG_PTR)read_arg( ctx, 2 );
    void *data = (void *)(ULONG_PTR)read_arg( ctx, 3 );
    UINT64 args[D3D11_UNIX_MAX_ARGS] = { 0 };
    struct private_iface *p;

    if (!guid || !size) return (UINT64)(UINT)E_INVALIDARG;

    RtlEnterCriticalSection( &priv_cs );
    for (p = private_ifaces; p; p = p->next)
        if (p->host == host && IsEqualGUID( &p->guid, guid )) break;
    RtlLeaveCriticalSection( &priv_cs );

    if (p)
    {
        void *proxy_host;

        if (!data)
        {
            *size = sizeof(void *);
            return (UINT64)(UINT)S_OK;
        }
        if (*size < sizeof(void *))
        {
            *size = sizeof(void *);
            return (UINT64)(UINT)DXGI_ERROR_MORE_DATA;
        }
        /* The contract AddRefs what it returns.  Do it on the host object the
         * proxy stands for, then re-intern: that is one more guest reference
         * on the same proxy and no net change to the host's count. */
        if (!(proxy_host = winecom_unwrap( p->guest )))
            return (UINT64)(UINT)E_FAIL;
        *(void **)data = host_addref_wrap( proxy_host, p->iface );
        *size = sizeof(void *);
        return (UINT64)(UINT)S_OK;
    }

    args[1] = (UINT64)(ULONG_PTR)guid;
    args[2] = (UINT64)(ULONG_PTR)size;
    args[3] = (UINT64)(ULONG_PTR)data;
    return unix_vtbl_call( host, slot, 4, args );
}

/* ...::SetPrivateData( REFGUID guid, UINT size, const void *data ).  Hooked
 * only so that storing a data blob under a GUID that previously held an
 * interface does not leave a stale side-table entry behind -- which would
 * make the NEXT GetPrivateData return an interface the application had
 * already replaced. */
static UINT64 hand_set_private_data( void *host, UINT slot, AMD64_CONTEXT *ctx )
{
    const GUID *guid = (const GUID *)(ULONG_PTR)read_arg( ctx, 1 );
    UINT64 args[D3D11_UNIX_MAX_ARGS] = { 0 };

    if (guid) private_drop( private_take( host, guid ) );
    args[1] = (UINT64)(ULONG_PTR)guid;
    args[2] = read_arg( ctx, 2 );
    args[3] = read_arg( ctx, 3 );
    return unix_vtbl_call( host, slot, 4, args );
}

/* ...::SetPrivateDataInterface( REFGUID guid, const IUnknown *data ). */
static UINT64 hand_set_private_data_iface( void *host, UINT slot, AMD64_CONTEXT *ctx )
{
    const GUID *guid = (const GUID *)(ULONG_PTR)read_arg( ctx, 1 );
    void *guest = (void *)(ULONG_PTR)read_arg( ctx, 2 );
    UINT64 args[D3D11_UNIX_MAX_ARGS] = { 0 };
    struct private_iface *p;
    void *host_iface = NULL;
    UINT64 ret;

    if (!guid) return (UINT64)(UINT)E_INVALIDARG;
    private_drop( private_take( host, guid ) );

    if (guest && !winecom_translate_in( guest, &host_iface ))
    {
        FIXME( "SetPrivateDataInterface with a guest-implemented object %p; "
               "reverse proxies do not exist yet\n", guest );
        return (UINT64)(UINT)E_NOTIMPL;
    }

    args[1] = (UINT64)(ULONG_PTR)guid;
    args[2] = (UINT64)(ULONG_PTR)host_iface;
    ret = unix_vtbl_call( host, slot, 3, args );
    if (FAILED((HRESULT)ret) || !guest) return ret;

    if (!(p = RtlAllocateHeap( NtCurrentTeb()->Peb->ProcessHeap, 0, sizeof(*p) )))
        return (UINT64)(UINT)E_OUTOFMEMORY;
    p->host = host;
    p->guest = guest;
    p->guid = *guid;
    p->iface = D3D11_IFACE_IUnknown;
    /* Pin the container: see the side-table banner. */
    {
        UINT64 a[D3D11_UNIX_MAX_ARGS] = { 0 };
        unix_vtbl_call( host, 1 /* AddRef */, 1, a );
    }
    RtlEnterCriticalSection( &priv_cs );
    p->next = private_ifaces;
    private_ifaces = p;
    RtlLeaveCriticalSection( &priv_cs );
    return ret;
}

/* ID3D11DeviceContext::ClearDepthStencilView( ID3D11DepthStencilView *view,
 * UINT flags, FLOAT depth, UINT8 stencil ).
 *
 * `depth` is the fourth argument counting `this`, so MS-x64 put it in XMM3 --
 * NOT in a GPR, and not on the stack.  The unixlib's widest-integer call form
 * cannot express that at all: on ELFv2 the callee reads f1, which the wide
 * call never wrote.  The value would be whatever was left there, and a wrong
 * clear depth is not a crash, it is a frame that looks nearly right.
 * dlls/ntdll/signal_ppc64.c's flat FP path reads the same
 * ctx->FltSave.XmmRegisters[i] by the same rule. */
static UINT64 hand_clear_depth_stencil_view( void *host, UINT slot, AMD64_CONTEXT *ctx )
{
    struct d3d11_float_params p = { 0 };
    void *view_host = NULL;

    if (!winecom_translate_in( (void *)(ULONG_PTR)read_arg( ctx, 1 ), &view_host ))
    {
        FIXME( "ClearDepthStencilView on a guest-implemented view\n" );
        return 0;
    }
    p.self = (UINT64)(ULONG_PTR)host;
    p.res = (UINT64)(ULONG_PTR)view_host;
    p.a = (UINT)read_arg( ctx, 2 );                 /* ClearFlags */
    p.f = *(const float *)&ctx->FltSave.XmmRegisters[3];
    p.b = (UINT8)read_arg( ctx, 4 );                /* Stencil */
    p.slot = slot;
    p.shape = FLOAT_SHAPE_RES_UINT_FLOAT_BYTE;
    if (D3D11_UNIX_CALL( float, &p )) ERR( "unix float call failed\n" );
    return 0;   /* void method; RAX is scratch */
}

/* ID3D11DeviceContext::SetResourceMinLOD( ID3D11Resource *r, FLOAT lod ):
 * `lod` is argument 2, so XMM2. */
static UINT64 hand_set_resource_min_lod( void *host, UINT slot, AMD64_CONTEXT *ctx )
{
    struct d3d11_float_params p = { 0 };
    void *res = NULL;

    if (!winecom_translate_in( (void *)(ULONG_PTR)read_arg( ctx, 1 ), &res ))
    {
        FIXME( "SetResourceMinLOD on a guest-implemented resource\n" );
        return 0;
    }
    p.self = (UINT64)(ULONG_PTR)host;
    p.res = (UINT64)(ULONG_PTR)res;
    p.f = *(const float *)&ctx->FltSave.XmmRegisters[2];
    p.slot = slot;
    p.shape = FLOAT_SHAPE_RES_FLOAT;
    if (D3D11_UNIX_CALL( float, &p )) ERR( "unix float call failed\n" );
    return 0;
}

/* ID3D11DeviceContext::GetResourceMinLOD( ID3D11Resource *r ) -> FLOAT.
 * MS-x64 returns a float in XMM0, not RAX, so this writes the register the
 * guest is about to read -- the whole 16 bytes, because stale high bytes from
 * an earlier call are visible to anything that reads it wider than it wrote.
 * Identical treatment to the flat FP return path in signal_ppc64.c. */
static UINT64 hand_get_resource_min_lod( void *host, UINT slot, AMD64_CONTEXT *ctx )
{
    struct d3d11_float_params p = { 0 };
    void *res = NULL;

    if (!winecom_translate_in( (void *)(ULONG_PTR)read_arg( ctx, 1 ), &res ))
    {
        FIXME( "GetResourceMinLOD on a guest-implemented resource\n" );
        return 0;
    }
    p.self = (UINT64)(ULONG_PTR)host;
    p.res = (UINT64)(ULONG_PTR)res;
    p.slot = slot;
    p.shape = FLOAT_SHAPE_RES_RET_FLOAT;
    if (D3D11_UNIX_CALL( float, &p )) ERR( "unix float call failed\n" );
    memset( &ctx->FltSave.XmmRegisters[0], 0, sizeof(ctx->FltSave.XmmRegisters[0]) );
    *(float *)&ctx->FltSave.XmmRegisters[0] = p.ret;
    return 0;
}

/* ID3D10Device::ClearDepthStencilView( ID3D10DepthStencilView *, UINT, FLOAT,
 * UINT8 ) -- the same shape at a different slot in a different vtable. */
static UINT64 hand_d3d10_clear_depth_stencil_view( void *host, UINT slot, AMD64_CONTEXT *ctx )
{
    return hand_clear_depth_stencil_view( host, slot, ctx );
}

/* ======================================================================
 *                            presentation
 *
 * THE HWND CROSSES UNCONVERTED, AND THAT IS THE WHOLE TRICK.  A Wine window
 * handle is meaningless to DXVK's shipped WSI backends, which is why every
 * route to a swapchain used to be refused here by name.  It is NOT meaningless
 * to Wine, and the guest PE calls Wine's own user32 -- so there is exactly one
 * window-handle namespace in this process and the integer in
 * DXGI_SWAP_CHAIN_DESC::OutputWindow names the same window on both sides of
 * the emulated boundary.  This lane's DXVK patch series adds a WSI backend
 * ("Win32u", ppc64le/dxvk/dxvk-patches/0003-win32u-wsi-backend.patch) that
 * hands the HWND straight back to Wine and asks win32u's client-surface layer
 * for a VkSurfaceKHR on it -- the layer winevulkan uses, so winex11 and
 * winewayland are both served without either being named.  The standalone
 * project's foreign-X11 design, which opened a second X connection and named
 * the guest's XID, is a documented dead end under Wayland.
 *
 * WHY THESE FOUR SLOTS ARE HAND-WRITTEN AND NOT GENERATED.  Their arguments
 * would all marshal correctly now that an HWND may cross; what the generator
 * cannot express is the ORDER OF OPERATIONS around them.  win32u wants its
 * client surface updated before a present and marked presented after, and both
 * calls must happen on a Wine thread -- which the application's call into
 * Present is and DXVK's submission thread, where the real vkQueuePresentKHR
 * happens, is not.  So the hooks are driven from here, on the caller's own
 * thread, exactly as dlls/d3d12/unix_present.c drives them for vkd3d.  The
 * creation slots are hand-written for the same kind of reason: the unixlib has
 * to be told the window's client size before DXVK asks for it, and it has no
 * way to ask user32 anything itself.
 * ====================================================================== */

/* DXGI_SWAP_CHAIN_DESC, spelled here rather than included.
 *
 * This module marshals and implements nothing, so it deliberately pulls in no
 * D3D11 or DXGI header -- see the banner at the top of this file.  One
 * descriptor still has to be READ, because the output window is inside it
 * rather than in the parameter list, and this is the smallest true statement
 * of the part that is read.  The asserts below are the check that it stays
 * true: the guest's x86-64 layout and this ppc64le one agree only because
 * every member is 4 bytes except the 8-byte, 8-aligned HWND, and if a compiler
 * ever disagreed about that the build would stop here instead of handing DXVK
 * a window handle read from the middle of `Windowed`. */
struct dxgi_swap_chain_desc
{
    UINT BufferDesc_Width;              /* DXGI_MODE_DESC BufferDesc */
    UINT BufferDesc_Height;
    UINT BufferDesc_RefreshRate_Numerator;
    UINT BufferDesc_RefreshRate_Denominator;
    UINT BufferDesc_Format;
    UINT BufferDesc_ScanlineOrdering;
    UINT BufferDesc_Scaling;
    UINT SampleDesc_Count;              /* DXGI_SAMPLE_DESC SampleDesc */
    UINT SampleDesc_Quality;
    UINT BufferUsage;
    UINT BufferCount;
    HWND OutputWindow;
    BOOL Windowed;
    UINT SwapEffect;
    UINT Flags;
};

C_ASSERT( FIELD_OFFSET(struct dxgi_swap_chain_desc, OutputWindow) == 48 );
C_ASSERT( sizeof(struct dxgi_swap_chain_desc) == 72 );

/* Which window a live host swapchain presents to.
 *
 * Needed because IDXGISwapChain::Present carries no window and the hooks do.
 * Keyed by the HOST swapchain pointer, which is what a hand-written slot is
 * handed: DXVK's DxgiSwapChain answers QueryInterface for every IDXGISwapChain
 * revision with `ref(this)`, one object and one address, so a guest that asked
 * for IDXGISwapChain1 and one that kept IDXGISwapChain present through the
 * same entry here.  A miss is not fatal and is reported once rather than
 * ignored: it would mean that assumption stopped holding, and the symptom
 * would otherwise be a window that renders but never updates. */
struct swapchain_window
{
    struct swapchain_window *next;
    void *host;
    HWND hwnd;
};

static CRITICAL_SECTION swap_cs;
static CRITICAL_SECTION_DEBUG swap_cs_debug =
{
    0, 0, &swap_cs,
    { &swap_cs_debug.ProcessLocksList, &swap_cs_debug.ProcessLocksList },
      0, 0, { (DWORD_PTR)(__FILE__ ": d3d11 swap_cs") }
};
static CRITICAL_SECTION swap_cs = { &swap_cs_debug, -1, 0, 0, 0, 0 };

static struct swapchain_window *swapchain_windows;

/* Tell the unixlib what it will be asked about this window.  Pushed rather
 * than pulled because the unix side cannot reach user32: it is below the PE
 * boundary, and NtUserCallHwndParam is a syscall a PE thread makes.  Called
 * before every creation and before every present, so the size DXVK reads is
 * never older than the last frame. */
static void push_hwnd_state( HWND hwnd )
{
    struct d3d11_hwnd_params p = { 0 };
    RECT rect = { 0 };

    if (!hwnd) return;
    p.hwnd = (UINT64)(ULONG_PTR)hwnd;
    if (NtUserIsWindow( hwnd ) && NtUserGetClientRect( hwnd, &rect, 0 ))
    {
        p.width = rect.right - rect.left;
        p.height = rect.bottom - rect.top;
        p.valid = 1;
    }
    D3D11_UNIX_CALL( hwnd, &p );
}

static void swapchain_remember( void *host, HWND hwnd )
{
    struct swapchain_window *s;

    if (!host || !hwnd) return;
    if (!(s = RtlAllocateHeap( NtCurrentTeb()->Peb->ProcessHeap, 0, sizeof(*s) )))
        return;
    s->host = host;
    s->hwnd = hwnd;
    RtlEnterCriticalSection( &swap_cs );
    s->next = swapchain_windows;
    swapchain_windows = s;
    RtlLeaveCriticalSection( &swap_cs );
    TRACE( "swapchain %p presents to hwnd %p\n", host, hwnd );
}

static HWND swapchain_hwnd( void *host )
{
    struct swapchain_window *s;
    HWND hwnd = NULL;

    RtlEnterCriticalSection( &swap_cs );
    for (s = swapchain_windows; s; s = s->next)
        if (s->host == host) { hwnd = s->hwnd; break; }
    RtlLeaveCriticalSection( &swap_cs );
    return hwnd;
}

static void present_hook( HWND hwnd, UINT phase, HRESULT hr )
{
    struct d3d11_present_params p = { 0 };

    p.hwnd = (UINT64)(ULONG_PTR)hwnd;
    p.phase = phase;
    p.result = hr;
    D3D11_UNIX_CALL( present, &p );
}

/* IDXGIFactory::CreateSwapChain( IUnknown *device, DXGI_SWAP_CHAIN_DESC *desc,
 * IDXGISwapChain **swapchain ). */
static UINT64 hand_create_swapchain( void *host, UINT slot, AMD64_CONTEXT *ctx )
{
    void *guest_device = (void *)(ULONG_PTR)read_arg( ctx, 1 );
    struct dxgi_swap_chain_desc *desc =
        (struct dxgi_swap_chain_desc *)(ULONG_PTR)read_arg( ctx, 2 );
    void **out = (void **)(ULONG_PTR)read_arg( ctx, 3 );
    UINT64 args[D3D11_UNIX_MAX_ARGS] = { 0 };
    void *host_device = NULL, *host_swapchain;
    HRESULT hr;

    if (!desc || !out) return (UINT64)(UINT)E_INVALIDARG;
    if (guest_device && !winecom_translate_in( guest_device, &host_device ))
    {
        FIXME( "CreateSwapChain with a guest-implemented device %p; reverse "
               "proxies do not exist yet\n", guest_device );
        return (UINT64)(UINT)E_NOTIMPL;
    }

    TRACE( "device %p, %ux%u, hwnd %p, windowed %d, buffers %u\n", guest_device,
           desc->BufferDesc_Width, desc->BufferDesc_Height, desc->OutputWindow,
           desc->Windowed, desc->BufferCount );

    push_hwnd_state( desc->OutputWindow );
    args[1] = (UINT64)(ULONG_PTR)host_device;
    args[2] = (UINT64)(ULONG_PTR)desc;
    args[3] = (UINT64)(ULONG_PTR)out;
    hr = (HRESULT)unix_vtbl_call( host, slot, 4, args );
    if (FAILED(hr)) return (UINT64)(UINT)hr;

    host_swapchain = *out;
    winecom_wrap_static( out, D3D11_IFACE_IDXGISwapChain );
    swapchain_remember( host_swapchain, desc->OutputWindow );
    return (UINT64)(UINT)hr;
}

/* IDXGIFactory2::CreateSwapChainForHwnd( IUnknown *device, HWND window,
 * const DXGI_SWAP_CHAIN_DESC1 *desc, const DXGI_SWAP_CHAIN_FULLSCREEN_DESC
 * *fullscreen_desc, IDXGIOutput *restrict_to_output,
 * IDXGISwapChain1 **swapchain ).
 *
 * DXGI_SWAP_CHAIN_DESC1 carries no window -- that is what the separate HWND
 * argument is for -- so neither descriptor has to be read here. */
static UINT64 hand_create_swapchain_for_hwnd( void *host, UINT slot, AMD64_CONTEXT *ctx )
{
    void *guest_device = (void *)(ULONG_PTR)read_arg( ctx, 1 );
    HWND hwnd = (HWND)(ULONG_PTR)read_arg( ctx, 2 );
    void *guest_output = (void *)(ULONG_PTR)read_arg( ctx, 5 );
    void **out = (void **)(ULONG_PTR)read_arg( ctx, 6 );
    UINT64 args[D3D11_UNIX_MAX_ARGS] = { 0 };
    void *host_device = NULL, *host_output = NULL, *host_swapchain;
    HRESULT hr;

    if (!out) return (UINT64)(UINT)E_INVALIDARG;
    if ((guest_device && !winecom_translate_in( guest_device, &host_device )) ||
        (guest_output && !winecom_translate_in( guest_output, &host_output )))
    {
        /* Not this surface's pointer -- but not necessarily a guest-
         * implemented object either.  A D3D12 title reaches THIS factory
         * (dxgi.dll forwards CreateDXGIFactory here) and passes the vkd3d
         * surface's ID3D12CommandQueue proxy as the device, which this
         * instance cannot translate because winecom interning is per-linkee.
         * Hand the whole call to the d3d12 lane: it unwraps the queue in its
         * own surface, presents through its unix factory + win32u, and
         * gives the guest back a swapchain proxy of ITS surface -- the one
         * whose GetBuffer(IID_ID3D12Resource) rows a D3D12 title needs.
         * [MEASURED] Cyberpunk 2077 run 31: the old blanket refusal here
         * made the game throw on a fiber stack and die undispatchable.
         * E_NOINTERFACE from the lane means "not mine either". */
        HRESULT (WINAPI *p_d3d12)( void *, void *, const void *, const void *,
                                   void *, void ** );
        HMODULE d3d12 = LoadLibraryW( L"d3d12.dll" );

        if (d3d12 && (p_d3d12 = (void *)GetProcAddress( d3d12,
                          "__wine_d3d12_create_swapchain_for_hwnd" )))
        {
            HRESULT hr2 = p_d3d12( guest_device, hwnd,
                                   (const void *)(ULONG_PTR)read_arg( ctx, 3 ),
                                   (const void *)(ULONG_PTR)read_arg( ctx, 4 ),
                                   guest_output, out );
            if (hr2 != E_NOINTERFACE)
            {
                TRACE( "served by the d3d12 lane, hr %#x\n", (UINT)hr2 );
                return (UINT64)(UINT)hr2;
            }
        }
        FIXME( "CreateSwapChainForHwnd with a guest-implemented device or "
               "output; reverse proxies do not exist yet\n" );
        return (UINT64)(UINT)E_NOTIMPL;
    }

    TRACE( "device %p, hwnd %p, output %p\n", guest_device, hwnd, guest_output );

    push_hwnd_state( hwnd );
    args[1] = (UINT64)(ULONG_PTR)host_device;
    args[2] = (UINT64)(ULONG_PTR)hwnd;
    args[3] = read_arg( ctx, 3 );
    args[4] = read_arg( ctx, 4 );
    args[5] = (UINT64)(ULONG_PTR)host_output;
    args[6] = (UINT64)(ULONG_PTR)out;
    hr = (HRESULT)unix_vtbl_call( host, slot, 7, args );
    if (FAILED(hr)) return (UINT64)(UINT)hr;

    host_swapchain = *out;
    winecom_wrap_static( out, D3D11_IFACE_IDXGISwapChain1 );
    swapchain_remember( host_swapchain, hwnd );
    return (UINT64)(UINT)hr;
}

/* The two hooks win32u performs around a present, and DXVK's Present between
 * them.  Everything here runs on the application's thread. */
static UINT64 present_common( void *host, UINT slot, UINT argc, UINT64 *args )
{
    HWND hwnd = swapchain_hwnd( host );
    HRESULT hr;

    if (!hwnd)
    {
        static BOOL logged;

        if (!logged)
        {
            logged = TRUE;
            WARN( "presenting swapchain %p, which this module never saw "
                  "created -- no window is known for it, so win32u's client "
                  "surface will not be updated around the present.  Either the "
                  "application got it from a path that is not "
                  "CreateSwapChain/CreateSwapChainForHwnd/"
                  "D3D11CreateDeviceAndSwapChain, or DXVK stopped answering "
                  "QueryInterface for the IDXGISwapChain revisions with one "
                  "address.\n", host );
        }
        return unix_vtbl_call( host, slot, argc, args );
    }

    push_hwnd_state( hwnd );
    present_hook( hwnd, PRESENT_PHASE_BEGIN, S_OK );
    hr = (HRESULT)unix_vtbl_call( host, slot, argc, args );
    present_hook( hwnd, PRESENT_PHASE_END, hr );
    return (UINT64)(UINT)hr;
}

/* IDXGISwapChain::Present( UINT sync_interval, UINT flags ). */
static UINT64 hand_swapchain_present( void *host, UINT slot, AMD64_CONTEXT *ctx )
{
    UINT64 args[D3D11_UNIX_MAX_ARGS] = { 0 };

    args[1] = read_arg( ctx, 1 );
    args[2] = read_arg( ctx, 2 );
    return present_common( host, slot, 3, args );
}

/* IDXGISwapChain1::Present1( UINT sync_interval, UINT flags,
 * const DXGI_PRESENT_PARAMETERS *params ).  The parameter block is RECTs and
 * a POINT by pointer -- plain data, and the same layout on both sides. */
static UINT64 hand_swapchain_present1( void *host, UINT slot, AMD64_CONTEXT *ctx )
{
    UINT64 args[D3D11_UNIX_MAX_ARGS] = { 0 };

    args[1] = read_arg( ctx, 1 );
    args[2] = read_arg( ctx, 2 );
    args[3] = read_arg( ctx, 3 );
    return present_common( host, slot, 4, args );
}

/* ---------------------------------------------------------- flat entries */

static HRESULT flat_call( UINT func, UINT argc, UINT64 *args, UINT64 *ret )
{
    struct d3d11_flat_params p;
    NTSTATUS status;

    if (!com_runtime_init()) return E_FAIL;
    memcpy( p.args, args, sizeof(p.args) );
    p.func = func;
    p.argc = argc;
    p.ret = 0;
    if ((status = D3D11_UNIX_CALL( flat, &p )))
    {
        ERR( "unix flat call %u failed, status %08x\n", func, (UINT)status );
        return E_FAIL;
    }
    if (ret) *ret = p.ret;
    return (HRESULT)p.ret;
}

/* The refusal every plain-named flat export shares.  A native ppc64 PE that
 * calls one of these would be handed proxies whose vtables are the guest's
 * x86-64 trap stubs; there is no correct answer to give it, so it is told so
 * by name rather than served something that faults later somewhere else. */
static HRESULT refuse_native_caller( const char *name )
{
    FIXME( "%s called by a NATIVE ppc64 caller.  This lane's D3D11 objects are "
           "guest proxies whose vtables are x86-64 trap stubs; only the "
           "emulated guest can call them.  The guest reaches this module "
           "through __wine_guest_%s (see dlls/d3d11/d3d11.thunks).\n",
           name, name );
    return E_NOTIMPL;
}

#define REFUSE_NATIVE(name) return refuse_native_caller( #name )

/* ---- d3d11.dll ---- */

HRESULT WINAPI __wine_guest_D3D11CreateDevice( void *adapter, UINT driver_type,
                                               void *software, UINT flags,
                                               const void *feature_levels,
                                               UINT levels, UINT sdk_version,
                                               void **device, UINT *feature_level,
                                               void **context )
{
    UINT64 args[12] = { 0 };
    void *host_adapter = NULL;
    HRESULT hr;

    TRACE( "adapter %p, driver_type %#x, flags %#x, levels %u, device %p, "
           "context %p\n", adapter, driver_type, flags, levels, device, context );

    if (!com_runtime_init()) return E_FAIL;
    if (adapter && !winecom_translate_in( adapter, &host_adapter ))
    {
        FIXME( "D3D11CreateDevice with a guest-implemented IDXGIAdapter %p; "
               "reverse proxies do not exist yet\n", adapter );
        return E_NOTIMPL;
    }
    args[0] = (UINT64)(ULONG_PTR)host_adapter;
    args[1] = driver_type;
    args[2] = (UINT64)(ULONG_PTR)software;
    args[3] = flags;
    args[4] = (UINT64)(ULONG_PTR)feature_levels;
    args[5] = levels;
    args[6] = sdk_version;
    args[7] = (UINT64)(ULONG_PTR)device;
    args[8] = (UINT64)(ULONG_PTR)feature_level;
    args[9] = (UINT64)(ULONG_PTR)context;
    hr = flat_call( FLAT_D3D11CreateDevice, 10, args, NULL );
    if (FAILED(hr)) return hr;
    /* Both out-interfaces are statically typed, so there is no REFIID to look
     * up; the roster index is fixed here. */
    winecom_wrap_static( device, D3D11_IFACE_ID3D11Device );
    winecom_wrap_static( context, D3D11_IFACE_ID3D11DeviceContext );
    return hr;
}

HRESULT WINAPI D3D11CreateDevice( void *adapter, UINT driver_type, void *software,
                                  UINT flags, const void *feature_levels, UINT levels,
                                  UINT sdk_version, void **device, UINT *feature_level,
                                  void **context )
{
    REFUSE_NATIVE(D3D11CreateDevice);
}

HRESULT WINAPI __wine_guest_D3D11CoreCreateDevice( void *factory, void *adapter,
                                                   UINT flags, const void *feature_levels,
                                                   UINT levels, void **device )
{
    UINT64 args[12] = { 0 };
    void *host_factory = NULL, *host_adapter = NULL;
    HRESULT hr;

    TRACE( "factory %p, adapter %p, flags %#x, levels %u, device %p\n",
           factory, adapter, flags, levels, device );

    if (!com_runtime_init()) return E_FAIL;
    if ((factory && !winecom_translate_in( factory, &host_factory )) ||
        (adapter && !winecom_translate_in( adapter, &host_adapter )))
    {
        FIXME( "D3D11CoreCreateDevice with a guest-implemented DXGI object\n" );
        return E_NOTIMPL;
    }
    args[0] = (UINT64)(ULONG_PTR)host_factory;
    args[1] = (UINT64)(ULONG_PTR)host_adapter;
    args[2] = flags;
    args[3] = (UINT64)(ULONG_PTR)feature_levels;
    args[4] = levels;
    args[5] = (UINT64)(ULONG_PTR)device;
    hr = flat_call( FLAT_D3D11CoreCreateDevice, 6, args, NULL );
    if (FAILED(hr)) return hr;
    winecom_wrap_static( device, D3D11_IFACE_ID3D11Device );
    return hr;
}

HRESULT WINAPI D3D11CoreCreateDevice( void *factory, void *adapter, UINT flags,
                                      const void *feature_levels, UINT levels,
                                      void **device )
{
    REFUSE_NATIVE(D3D11CoreCreateDevice);
}

HRESULT WINAPI __wine_guest_D3D11CreateDeviceAndSwapChain( void *adapter, UINT driver_type,
                                                           void *software, UINT flags,
                                                           const void *feature_levels,
                                                           UINT levels, UINT sdk_version,
                                                           const void *swapchain_desc,
                                                           void **swapchain, void **device,
                                                           UINT *feature_level,
                                                           void **context )
{
    const struct dxgi_swap_chain_desc *desc = swapchain_desc;
    UINT64 args[12] = { 0 };
    void *host_adapter = NULL, *host_swapchain = NULL;
    HRESULT hr;

    TRACE( "adapter %p, driver_type %#x, flags %#x, levels %u, desc %p, "
           "swapchain %p, device %p, context %p\n", adapter, driver_type, flags,
           levels, desc, swapchain, device, context );

    if (!com_runtime_init()) return E_FAIL;
    if (adapter && !winecom_translate_in( adapter, &host_adapter ))
    {
        FIXME( "D3D11CreateDeviceAndSwapChain with a guest-implemented "
               "IDXGIAdapter %p; reverse proxies do not exist yet\n", adapter );
        return E_NOTIMPL;
    }

    /* The window's client size has to be in the unixlib's hands before DXVK
     * builds the swapchain, because DXVK asks for it during construction --
     * and an application that passed 0x0 in the descriptor is asking to be
     * sized to the window. */
    if (desc) push_hwnd_state( desc->OutputWindow );

    args[0] = (UINT64)(ULONG_PTR)host_adapter;
    args[1] = driver_type;
    args[2] = (UINT64)(ULONG_PTR)software;
    args[3] = flags;
    args[4] = (UINT64)(ULONG_PTR)feature_levels;
    args[5] = levels;
    args[6] = sdk_version;
    args[7] = (UINT64)(ULONG_PTR)swapchain_desc;
    args[8] = (UINT64)(ULONG_PTR)swapchain;
    args[9] = (UINT64)(ULONG_PTR)device;
    args[10] = (UINT64)(ULONG_PTR)feature_level;
    args[11] = (UINT64)(ULONG_PTR)context;
    hr = flat_call( FLAT_D3D11CreateDeviceAndSwapChain, 12, args, NULL );
    if (FAILED(hr)) return hr;

    if (swapchain) host_swapchain = *swapchain;
    winecom_wrap_static( swapchain, D3D11_IFACE_IDXGISwapChain );
    winecom_wrap_static( device, D3D11_IFACE_ID3D11Device );
    winecom_wrap_static( context, D3D11_IFACE_ID3D11DeviceContext );
    if (desc) swapchain_remember( host_swapchain, desc->OutputWindow );
    return hr;
}

HRESULT WINAPI D3D11CreateDeviceAndSwapChain( void *adapter, UINT driver_type,
                                              void *software, UINT flags,
                                              const void *feature_levels, UINT levels,
                                              UINT sdk_version, const void *swapchain_desc,
                                              void **swapchain, void **device,
                                              UINT *feature_level, void **context )
{
    REFUSE_NATIVE(D3D11CreateDeviceAndSwapChain);
}

HRESULT WINAPI __wine_guest_D3D11On12CreateDevice( void *device12, UINT flags,
                                                   const void *feature_levels, UINT levels,
                                                   void **queues, UINT queue_count,
                                                   UINT node_mask, void **device,
                                                   void **context, UINT *feature_level )
{
    static BOOL logged;

    if (!logged)
    {
        logged = TRUE;
        FIXME( "D3D11On12CreateDevice: this needs a live ID3D12Device from the "
               "d3d12 lane, and the two lanes hold SEPARATE winecom instances "
               "(libs/winecom state is per-linkee).  A d3d12 proxy handed to "
               "this module's runtime is not one of its proxies; it would be "
               "refused a frame later, in the middle of a resource wrap, where "
               "the reason would be illegible.  Refused here instead.\n" );
    }
    if (device) *device = NULL;
    if (context) *context = NULL;
    return E_NOTIMPL;
}

HRESULT WINAPI D3D11On12CreateDevice( void *device12, UINT flags,
                                      const void *feature_levels, UINT levels,
                                      void **queues, UINT queue_count, UINT node_mask,
                                      void **device, void **context, UINT *feature_level )
{
    REFUSE_NATIVE(D3D11On12CreateDevice);
}

HRESULT WINAPI D3D11CoreRegisterLayers( void )
{
    TRACE( "\n" );
    return S_OK;
}

/* ---- dxgi.dll (forwarded here by dlls/dxgi/dxgi.spec) ---- */

HRESULT WINAPI __wine_guest_CreateDXGIFactory( const GUID *riid, void **factory )
{
    UINT64 args[12] = { 0 };
    HRESULT hr;

    TRACE( "riid %s, factory %p\n", debugstr_guid(riid), factory );
    if (!com_runtime_init()) return E_FAIL;
    args[0] = (UINT64)(ULONG_PTR)riid;
    args[1] = (UINT64)(ULONG_PTR)factory;
    hr = flat_call( FLAT_CreateDXGIFactory, 2, args, NULL );
    return winecom_wrap_out_iface( hr, riid, factory );
}

HRESULT WINAPI CreateDXGIFactory( const GUID *riid, void **factory )
{
    REFUSE_NATIVE(CreateDXGIFactory);
}

HRESULT WINAPI __wine_guest_CreateDXGIFactory1( const GUID *riid, void **factory )
{
    UINT64 args[12] = { 0 };
    HRESULT hr;

    TRACE( "riid %s, factory %p\n", debugstr_guid(riid), factory );
    if (!com_runtime_init()) return E_FAIL;
    args[0] = (UINT64)(ULONG_PTR)riid;
    args[1] = (UINT64)(ULONG_PTR)factory;
    hr = flat_call( FLAT_CreateDXGIFactory1, 2, args, NULL );
    return winecom_wrap_out_iface( hr, riid, factory );
}

HRESULT WINAPI CreateDXGIFactory1( const GUID *riid, void **factory )
{
    REFUSE_NATIVE(CreateDXGIFactory1);
}

HRESULT WINAPI __wine_guest_CreateDXGIFactory2( UINT flags, const GUID *riid,
                                                void **factory )
{
    UINT64 args[12] = { 0 };
    HRESULT hr;

    TRACE( "flags %#x, riid %s, factory %p\n", flags, debugstr_guid(riid), factory );
    if (!com_runtime_init()) return E_FAIL;
    args[0] = flags;
    args[1] = (UINT64)(ULONG_PTR)riid;
    args[2] = (UINT64)(ULONG_PTR)factory;
    hr = flat_call( FLAT_CreateDXGIFactory2, 3, args, NULL );
    return winecom_wrap_out_iface( hr, riid, factory );
}

HRESULT WINAPI CreateDXGIFactory2( UINT flags, const GUID *riid, void **factory )
{
    REFUSE_NATIVE(CreateDXGIFactory2);
}

HRESULT WINAPI __wine_guest_DXGIGetDebugInterface1( UINT flags, const GUID *riid,
                                                    void **debug )
{
    UINT64 args[12] = { 0 };
    HRESULT hr;

    TRACE( "flags %#x, riid %s, debug %p\n", flags, debugstr_guid(riid), debug );
    if (!com_runtime_init()) return E_FAIL;
    args[0] = flags;
    args[1] = (UINT64)(ULONG_PTR)riid;
    args[2] = (UINT64)(ULONG_PTR)debug;
    hr = flat_call( FLAT_DXGIGetDebugInterface1, 3, args, NULL );
    return winecom_wrap_out_iface( hr, riid, debug );
}

HRESULT WINAPI DXGIGetDebugInterface1( UINT flags, const GUID *riid, void **debug )
{
    REFUSE_NATIVE(DXGIGetDebugInterface1);
}

/* Carries no interface pointer, so the guest and native forms are the same
 * function and no GUEST-IMPL row is needed for it. */
HRESULT WINAPI DXGIDeclareAdapterRemovalSupport( void )
{
    UINT64 args[12] = { 0 };

    TRACE( "\n" );
    return flat_call( FLAT_DXGIDeclareAdapterRemovalSupport, 0, args, NULL );
}

HRESULT WINAPI __wine_guest_DXGID3D10CreateDevice( void *d3d11_module, void *factory,
                                                   void *adapter, UINT flags,
                                                   const void *feature_levels,
                                                   UINT levels, void **device )
{
    static BOOL logged;

    if (!logged)
    {
        logged = TRUE;
        FIXME( "DXGID3D10CreateDevice: a Wine-private back door between Wine's "
               "own dxgi and d3d10core, which this lane replaces on both sides "
               "-- DXVK's d3d10core reaches its device through "
               "D3D10CoreCreateDevice and never calls this.  Its first "
               "argument is an HMODULE of Wine's d3d11, which does not exist "
               "here.\n" );
    }
    if (device) *device = NULL;
    return E_NOTIMPL;
}

HRESULT WINAPI DXGID3D10CreateDevice( void *d3d11_module, void *factory, void *adapter,
                                      UINT flags, const void *feature_levels,
                                      UINT levels, void **device )
{
    REFUSE_NATIVE(DXGID3D10CreateDevice);
}

HRESULT WINAPI DXGID3D10RegisterLayers( void *layers, UINT layer_count )
{
    FIXME( "layers %p, count %u: the Wine-private dxgi/d3d10core layer "
           "registration has no counterpart in DXVK's d3d10core, which this "
           "lane serves instead\n", layers, layer_count );
    return E_NOTIMPL;
}

/* ---- d3d10core.dll (forwarded here by dlls/d3d10core/d3d10core.spec) ---- */

HRESULT WINAPI __wine_guest_D3D10CoreCreateDevice( void *factory, void *adapter,
                                                   UINT flags, UINT feature_level,
                                                   void **device )
{
    UINT64 args[12] = { 0 };
    void *host_factory = NULL, *host_adapter = NULL;
    HRESULT hr;

    TRACE( "factory %p, adapter %p, flags %#x, feature_level %#x, device %p\n",
           factory, adapter, flags, feature_level, device );

    if (!com_runtime_init()) return E_FAIL;
    if ((factory && !winecom_translate_in( factory, &host_factory )) ||
        (adapter && !winecom_translate_in( adapter, &host_adapter )))
    {
        FIXME( "D3D10CoreCreateDevice with a guest-implemented DXGI object\n" );
        return E_NOTIMPL;
    }
    args[0] = (UINT64)(ULONG_PTR)host_factory;
    args[1] = (UINT64)(ULONG_PTR)host_adapter;
    args[2] = flags;
    args[3] = feature_level;
    args[4] = (UINT64)(ULONG_PTR)device;
    hr = flat_call( FLAT_D3D10CoreCreateDevice, 5, args, NULL );
    if (FAILED(hr)) return hr;
    winecom_wrap_static( device, D3D11_IFACE_ID3D10Device );
    return hr;
}

HRESULT WINAPI D3D10CoreCreateDevice( void *factory, void *adapter, UINT flags,
                                      UINT feature_level, void **device )
{
    REFUSE_NATIVE(D3D10CoreCreateDevice);
}

/* ---- d3d10.dll (forwarded here by dlls/d3d10/d3d10.spec) ----
 *
 * D3D10's DEVICE CREATION, AND ONLY THAT.  DXVK ships no d3d10.dll: upstream
 * removed it and kept d3d10core, which is a thin layer over its own d3d11 and
 * is what this lane serves.  Everything else Wine's d3d10.dll exports -- the
 * effects framework, the state-block helpers, the shader reflection -- is
 * Wine's OWN implementation written against ID3D10Device, and on this lane an
 * ID3D10Device is a guest proxy whose vtable is an array of x86-64 trap stubs.
 * A native ppc64 effects framework driving one would execute those bytes as
 * ppc64 on its first call.  So those exports are refused by name in
 * dlls/d3d10/d3d10.thunks and only these two are served.
 *
 * The two that are served are, in Wine's own d3d10.dll and in the real
 * runtime, thin: make a DXGI factory, take its first adapter, and hand both to
 * D3D10CoreCreateDevice.  Reproduced here with HOST-side calls so that no
 * proxy is ever minted for the factory or the adapter -- they exist for the
 * length of this function and the application never sees them, which is
 * exactly what the real runtime does too. */

/* IDXGIFactory's slot numbers, from ppc64le/dxvk/interfaces_dxvk.json rather
 * than from memory: IUnknown's three, then IDXGIObject's four (SetPrivateData,
 * SetPrivateDataInterface, GetPrivateData, GetParent), then the interface's
 * own.  They are spelled out because this is the one place in this module that
 * calls a vtable slot the marshal tables did not choose for it. */
#define DXGI_FACTORY_SLOT_ENUM_ADAPTERS    7
#define DXGI_FACTORY_SLOT_CREATE_SWAPCHAIN 10
#define IUNKNOWN_SLOT_RELEASE              2

/* D3D10_SDK_VERSION and D3D_FEATURE_LEVEL_10_0, spelled here because this
 * module includes no D3D10 header.  The SDK version is CHECKED and the feature
 * level is what D3D10CoreCreateDevice is handed -- they are different things
 * and they are the same argument position in two different functions, which is
 * a mistake worth being explicit about.  [MEASURED] passing the SDK version
 * through as the feature level produced a device anyway and DXVK logged
 * `D3D11InternalCreateDevice: Using feature level 29`; 29 is not a feature
 * level.  Wine's own dlls/d3d10/d3d10_main.c gets this right at line 144 and
 * that is where the value comes from. */
#define D3D10_SDK_VERSION_VALUE   29
#define D3D_FEATURE_LEVEL_10_0_VALUE 0xa000

static void host_release( void *host )
{
    UINT64 args[D3D11_UNIX_MAX_ARGS] = { 0 };

    if (host) unix_vtbl_call( host, IUNKNOWN_SLOT_RELEASE, 1, args );
}

/* D3D10's driver types.  Only HARDWARE (and WARP, which the real runtime and
 * Wine both fall back to hardware for) can be served here: REFERENCE, NULL and
 * SOFTWARE all want a SOFTWARE ADAPTER, made by IDXGIFactory::CreateSoftwareAdapter
 * from a rasteriser DLL, and DXVK implements neither the adapter nor the
 * rasteriser.  Refused by name rather than served with hardware, because an
 * application that asked for the reference rasteriser asked for it to compare
 * against -- silently giving it the hardware one answers the opposite of its
 * question. */
enum { D3D10_DRIVER_TYPE_HARDWARE_VALUE = 0, D3D10_DRIVER_TYPE_REFERENCE_VALUE = 1,
       D3D10_DRIVER_TYPE_NULL_VALUE = 2, D3D10_DRIVER_TYPE_SOFTWARE_VALUE = 3,
       D3D10_DRIVER_TYPE_WARP_VALUE = 5 };

static BOOL d3d10_driver_type_ok( UINT driver_type )
{
    static BOOL logged;

    if (driver_type == D3D10_DRIVER_TYPE_HARDWARE_VALUE ||
        driver_type == D3D10_DRIVER_TYPE_WARP_VALUE)
        return TRUE;

    if (!logged)
    {
        logged = TRUE;
        FIXME( "D3D10 driver type %#x asks for a software adapter -- the "
               "reference rasteriser, the null device or a caller-supplied "
               "one.  DXVK implements no software adapter and no rasteriser, "
               "and answering with the hardware device would give an "
               "application that asked to compare against the reference the "
               "opposite of what it asked for.  Refused.\n", driver_type );
    }
    return FALSE;
}

/* IID_IDXGIFactory, spelled out for the same reason this module spells out
 * DXGI_SWAP_CHAIN_DESC: it pulls in no DXGI header, and this is the one GUID it
 * needs.  Checked against include/dxgi.idl. */
static const GUID d3d11_IID_IDXGIFactory =
    { 0x7b7166ec, 0x21c7, 0x44ae, { 0xb2,0x1a,0xc9,0xae,0x32,0x1a,0xe3,0x69 } };

/* A host IDXGIFactory and its first host adapter, both unwrapped, both the
 * caller's to release.  Returns S_OK or the failing HRESULT. */
static HRESULT d3d10_host_factory( void **factory_out, void **adapter_out )
{
    UINT64 args[12] = { 0 };
    UINT64 a[D3D11_UNIX_MAX_ARGS] = { 0 };
    void *factory = NULL, *adapter = NULL;
    HRESULT hr;

    *factory_out = *adapter_out = NULL;
    args[0] = (UINT64)(ULONG_PTR)&d3d11_IID_IDXGIFactory;
    args[1] = (UINT64)(ULONG_PTR)&factory;
    if (FAILED(hr = flat_call( FLAT_CreateDXGIFactory, 2, args, NULL ))) return hr;
    if (!factory) return E_FAIL;

    a[1] = 0;                                   /* adapter index */
    a[2] = (UINT64)(ULONG_PTR)&adapter;
    hr = (HRESULT)unix_vtbl_call( factory, DXGI_FACTORY_SLOT_ENUM_ADAPTERS, 3, a );
    if (FAILED(hr) || !adapter)
    {
        host_release( factory );
        return FAILED(hr) ? hr : E_FAIL;
    }
    *factory_out = factory;
    *adapter_out = adapter;
    return S_OK;
}

HRESULT WINAPI __wine_guest_D3D10CreateDevice( void *adapter, UINT driver_type,
                                               void *software, UINT flags,
                                               UINT sdk_version, void **device )
{
    UINT64 args[12] = { 0 };
    void *factory = NULL, *host_adapter = NULL, *own_adapter = NULL;
    HRESULT hr;

    TRACE( "adapter %p, driver_type %#x, flags %#x, sdk_version %u, device %p\n",
           adapter, driver_type, flags, sdk_version, device );

    if (!device) return E_INVALIDARG;
    if (sdk_version != D3D10_SDK_VERSION_VALUE)
    {
        WARN( "invalid SDK version %#x\n", sdk_version );
        return E_INVALIDARG;
    }
    if (!d3d10_driver_type_ok( driver_type )) return E_FAIL;
    if (!com_runtime_init()) return E_FAIL;

    if (adapter && !winecom_translate_in( adapter, &host_adapter ))
    {
        FIXME( "D3D10CreateDevice with a guest-implemented IDXGIAdapter %p; "
               "reverse proxies do not exist yet\n", adapter );
        return E_NOTIMPL;
    }
    if (FAILED(hr = d3d10_host_factory( &factory, &own_adapter ))) return hr;
    if (!host_adapter) host_adapter = own_adapter;

    args[0] = (UINT64)(ULONG_PTR)factory;
    args[1] = (UINT64)(ULONG_PTR)host_adapter;
    args[2] = flags;
    args[3] = D3D_FEATURE_LEVEL_10_0_VALUE;
    args[4] = (UINT64)(ULONG_PTR)device;
    hr = flat_call( FLAT_D3D10CoreCreateDevice, 5, args, NULL );

    host_release( own_adapter );
    host_release( factory );
    if (FAILED(hr)) return hr;
    winecom_wrap_static( device, D3D11_IFACE_ID3D10Device );
    return hr;
}

HRESULT WINAPI D3D10CreateDevice( void *adapter, UINT driver_type, void *software,
                                  UINT flags, UINT sdk_version, void **device )
{
    REFUSE_NATIVE(D3D10CreateDevice);
}

HRESULT WINAPI __wine_guest_D3D10CreateDeviceAndSwapChain( void *adapter, UINT driver_type,
                                                           void *software, UINT flags,
                                                           UINT sdk_version,
                                                           void *swapchain_desc,
                                                           void **swapchain, void **device )
{
    const struct dxgi_swap_chain_desc *desc = swapchain_desc;
    UINT64 args[12] = { 0 };
    void *factory = NULL, *host_adapter = NULL, *own_adapter = NULL;
    void *host_swapchain = NULL;
    HRESULT hr;

    TRACE( "adapter %p, flags %#x, desc %p, swapchain %p, device %p\n",
           adapter, flags, desc, swapchain, device );

    if (!device) return E_INVALIDARG;
    if (sdk_version != D3D10_SDK_VERSION_VALUE)
    {
        WARN( "invalid SDK version %#x\n", sdk_version );
        return E_INVALIDARG;
    }
    if (!d3d10_driver_type_ok( driver_type )) return E_FAIL;
    if (!com_runtime_init()) return E_FAIL;

    if (adapter && !winecom_translate_in( adapter, &host_adapter ))
    {
        FIXME( "D3D10CreateDeviceAndSwapChain with a guest-implemented "
               "IDXGIAdapter %p; reverse proxies do not exist yet\n", adapter );
        return E_NOTIMPL;
    }
    if (FAILED(hr = d3d10_host_factory( &factory, &own_adapter ))) return hr;
    if (!host_adapter) host_adapter = own_adapter;

    args[0] = (UINT64)(ULONG_PTR)factory;
    args[1] = (UINT64)(ULONG_PTR)host_adapter;
    args[2] = flags;
    args[3] = D3D_FEATURE_LEVEL_10_0_VALUE;
    args[4] = (UINT64)(ULONG_PTR)device;
    hr = flat_call( FLAT_D3D10CoreCreateDevice, 5, args, NULL );
    if (FAILED(hr))
    {
        host_release( own_adapter );
        host_release( factory );
        return hr;
    }

    /* The swapchain, on the SAME host factory and the host device, before
     * either is wrapped: IDXGIFactory::CreateSwapChain wants an IUnknown the
     * runtime owns, and a proxy is not one.  Same window bookkeeping the
     * hand-written CreateSwapChain slot does, for the same reasons. */
    if (desc && swapchain)
    {
        UINT64 a[D3D11_UNIX_MAX_ARGS] = { 0 };

        push_hwnd_state( desc->OutputWindow );
        a[1] = (UINT64)(ULONG_PTR)*device;
        a[2] = (UINT64)(ULONG_PTR)desc;
        a[3] = (UINT64)(ULONG_PTR)swapchain;
        hr = (HRESULT)unix_vtbl_call( factory, DXGI_FACTORY_SLOT_CREATE_SWAPCHAIN, 4, a );
        if (SUCCEEDED(hr)) host_swapchain = *swapchain;
    }

    host_release( own_adapter );
    host_release( factory );
    if (FAILED(hr))
    {
        host_release( *device );
        *device = NULL;
        return hr;
    }
    winecom_wrap_static( device, D3D11_IFACE_ID3D10Device );
    if (host_swapchain)
    {
        winecom_wrap_static( swapchain, D3D11_IFACE_IDXGISwapChain );
        swapchain_remember( host_swapchain, desc->OutputWindow );
    }
    return hr;
}

HRESULT WINAPI D3D10CreateDeviceAndSwapChain( void *adapter, UINT driver_type,
                                              void *software, UINT flags,
                                              UINT sdk_version, void *swapchain_desc,
                                              void **swapchain, void **device )
{
    REFUSE_NATIVE(D3D10CreateDeviceAndSwapChain);
}

HRESULT WINAPI D3D10CoreGetVersion( void )
{
    UINT64 args[12] = { 0 };

    TRACE( "\n" );
    return flat_call( FLAT_D3D10CoreGetVersion, 0, args, NULL );
}

HRESULT WINAPI D3D10CoreRegisterLayers( void )
{
    TRACE( "\n" );
    return S_OK;
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

ULONG_PTR WINAPI D3D11CoreCreateDevice( ULONG_PTR a1, ULONG_PTR a2, ULONG_PTR a3, ULONG_PTR a4, ULONG_PTR a5, ULONG_PTR a6 )
{
    __wine_spec_unimplemented_stub( "d3d11.dll", "D3D11CoreCreateDevice" );
}

ULONG_PTR WINAPI D3D11CoreRegisterLayers( void )
{
    __wine_spec_unimplemented_stub( "d3d11.dll", "D3D11CoreRegisterLayers" );
}

ULONG_PTR WINAPI D3D11CreateDevice( ULONG_PTR a1, ULONG_PTR a2, ULONG_PTR a3, ULONG_PTR a4, ULONG_PTR a5, ULONG_PTR a6, ULONG_PTR a7, ULONG_PTR a8, ULONG_PTR a9, ULONG_PTR a10 )
{
    __wine_spec_unimplemented_stub( "d3d11.dll", "D3D11CreateDevice" );
}

ULONG_PTR WINAPI D3D11CreateDeviceAndSwapChain( ULONG_PTR a1, ULONG_PTR a2, ULONG_PTR a3, ULONG_PTR a4, ULONG_PTR a5, ULONG_PTR a6, ULONG_PTR a7, ULONG_PTR a8, ULONG_PTR a9, ULONG_PTR a10, ULONG_PTR a11, ULONG_PTR a12 )
{
    __wine_spec_unimplemented_stub( "d3d11.dll", "D3D11CreateDeviceAndSwapChain" );
}

ULONG_PTR WINAPI D3D11On12CreateDevice( ULONG_PTR a1, ULONG_PTR a2, ULONG_PTR a3, ULONG_PTR a4, ULONG_PTR a5, ULONG_PTR a6, ULONG_PTR a7, ULONG_PTR a8, ULONG_PTR a9, ULONG_PTR a10 )
{
    __wine_spec_unimplemented_stub( "d3d11.dll", "D3D11On12CreateDevice" );
}

ULONG_PTR WINAPI __wine_com_dispatch( ULONG_PTR a1, ULONG_PTR a2, ULONG_PTR a3 )
{
    __wine_spec_unimplemented_stub( "d3d11.dll", "__wine_com_dispatch" );
}

ULONG_PTR WINAPI __wine_com_slot_name( ULONG_PTR a1, ULONG_PTR a2, ULONG_PTR a3, ULONG_PTR a4 )
{
    __wine_spec_unimplemented_stub( "d3d11.dll", "__wine_com_slot_name" );
}

ULONG_PTR WINAPI __wine_guest_D3D11CreateDevice( ULONG_PTR a1, ULONG_PTR a2, ULONG_PTR a3, ULONG_PTR a4, ULONG_PTR a5, ULONG_PTR a6, ULONG_PTR a7, ULONG_PTR a8, ULONG_PTR a9, ULONG_PTR a10 )
{
    __wine_spec_unimplemented_stub( "d3d11.dll", "__wine_guest_D3D11CreateDevice" );
}

ULONG_PTR WINAPI __wine_guest_D3D11CreateDeviceAndSwapChain( ULONG_PTR a1, ULONG_PTR a2, ULONG_PTR a3, ULONG_PTR a4, ULONG_PTR a5, ULONG_PTR a6, ULONG_PTR a7, ULONG_PTR a8, ULONG_PTR a9, ULONG_PTR a10, ULONG_PTR a11, ULONG_PTR a12 )
{
    __wine_spec_unimplemented_stub( "d3d11.dll", "__wine_guest_D3D11CreateDeviceAndSwapChain" );
}

ULONG_PTR WINAPI __wine_guest_D3D11CoreCreateDevice( ULONG_PTR a1, ULONG_PTR a2, ULONG_PTR a3, ULONG_PTR a4, ULONG_PTR a5, ULONG_PTR a6 )
{
    __wine_spec_unimplemented_stub( "d3d11.dll", "__wine_guest_D3D11CoreCreateDevice" );
}

ULONG_PTR WINAPI __wine_guest_D3D11On12CreateDevice( ULONG_PTR a1, ULONG_PTR a2, ULONG_PTR a3, ULONG_PTR a4, ULONG_PTR a5, ULONG_PTR a6, ULONG_PTR a7, ULONG_PTR a8, ULONG_PTR a9, ULONG_PTR a10 )
{
    __wine_spec_unimplemented_stub( "d3d11.dll", "__wine_guest_D3D11On12CreateDevice" );
}

ULONG_PTR WINAPI __wine_guest_CreateDXGIFactory( ULONG_PTR a1, ULONG_PTR a2 )
{
    __wine_spec_unimplemented_stub( "d3d11.dll", "__wine_guest_CreateDXGIFactory" );
}

ULONG_PTR WINAPI __wine_guest_CreateDXGIFactory1( ULONG_PTR a1, ULONG_PTR a2 )
{
    __wine_spec_unimplemented_stub( "d3d11.dll", "__wine_guest_CreateDXGIFactory1" );
}

ULONG_PTR WINAPI __wine_guest_CreateDXGIFactory2( ULONG_PTR a1, ULONG_PTR a2, ULONG_PTR a3 )
{
    __wine_spec_unimplemented_stub( "d3d11.dll", "__wine_guest_CreateDXGIFactory2" );
}

ULONG_PTR WINAPI __wine_guest_DXGIGetDebugInterface1( ULONG_PTR a1, ULONG_PTR a2, ULONG_PTR a3 )
{
    __wine_spec_unimplemented_stub( "d3d11.dll", "__wine_guest_DXGIGetDebugInterface1" );
}

ULONG_PTR WINAPI __wine_guest_DXGID3D10CreateDevice( ULONG_PTR a1, ULONG_PTR a2, ULONG_PTR a3, ULONG_PTR a4, ULONG_PTR a5, ULONG_PTR a6, ULONG_PTR a7 )
{
    __wine_spec_unimplemented_stub( "d3d11.dll", "__wine_guest_DXGID3D10CreateDevice" );
}

ULONG_PTR WINAPI __wine_guest_D3D10CoreCreateDevice( ULONG_PTR a1, ULONG_PTR a2, ULONG_PTR a3, ULONG_PTR a4, ULONG_PTR a5 )
{
    __wine_spec_unimplemented_stub( "d3d11.dll", "__wine_guest_D3D10CoreCreateDevice" );
}

ULONG_PTR WINAPI __wine_guest_D3D10CreateDevice( ULONG_PTR a1, ULONG_PTR a2, ULONG_PTR a3, ULONG_PTR a4, ULONG_PTR a5, ULONG_PTR a6 )
{
    __wine_spec_unimplemented_stub( "d3d11.dll", "__wine_guest_D3D10CreateDevice" );
}

ULONG_PTR WINAPI __wine_guest_D3D10CreateDeviceAndSwapChain( ULONG_PTR a1, ULONG_PTR a2, ULONG_PTR a3, ULONG_PTR a4, ULONG_PTR a5, ULONG_PTR a6, ULONG_PTR a7, ULONG_PTR a8 )
{
    __wine_spec_unimplemented_stub( "d3d11.dll", "__wine_guest_D3D10CreateDeviceAndSwapChain" );
}

ULONG_PTR WINAPI CreateDXGIFactory( ULONG_PTR a1, ULONG_PTR a2 )
{
    __wine_spec_unimplemented_stub( "d3d11.dll", "CreateDXGIFactory" );
}

ULONG_PTR WINAPI CreateDXGIFactory1( ULONG_PTR a1, ULONG_PTR a2 )
{
    __wine_spec_unimplemented_stub( "d3d11.dll", "CreateDXGIFactory1" );
}

ULONG_PTR WINAPI CreateDXGIFactory2( ULONG_PTR a1, ULONG_PTR a2, ULONG_PTR a3 )
{
    __wine_spec_unimplemented_stub( "d3d11.dll", "CreateDXGIFactory2" );
}

ULONG_PTR WINAPI DXGIGetDebugInterface1( ULONG_PTR a1, ULONG_PTR a2, ULONG_PTR a3 )
{
    __wine_spec_unimplemented_stub( "d3d11.dll", "DXGIGetDebugInterface1" );
}

ULONG_PTR WINAPI DXGIDeclareAdapterRemovalSupport( void )
{
    __wine_spec_unimplemented_stub( "d3d11.dll", "DXGIDeclareAdapterRemovalSupport" );
}

ULONG_PTR WINAPI DXGID3D10CreateDevice( ULONG_PTR a1, ULONG_PTR a2, ULONG_PTR a3, ULONG_PTR a4, ULONG_PTR a5, ULONG_PTR a6, ULONG_PTR a7 )
{
    __wine_spec_unimplemented_stub( "d3d11.dll", "DXGID3D10CreateDevice" );
}

ULONG_PTR WINAPI DXGID3D10RegisterLayers( ULONG_PTR a1, ULONG_PTR a2 )
{
    __wine_spec_unimplemented_stub( "d3d11.dll", "DXGID3D10RegisterLayers" );
}

ULONG_PTR WINAPI D3D10CoreCreateDevice( ULONG_PTR a1, ULONG_PTR a2, ULONG_PTR a3, ULONG_PTR a4, ULONG_PTR a5 )
{
    __wine_spec_unimplemented_stub( "d3d11.dll", "D3D10CoreCreateDevice" );
}

ULONG_PTR WINAPI D3D10CoreGetVersion( void )
{
    __wine_spec_unimplemented_stub( "d3d11.dll", "D3D10CoreGetVersion" );
}

ULONG_PTR WINAPI D3D10CoreRegisterLayers( void )
{
    __wine_spec_unimplemented_stub( "d3d11.dll", "D3D10CoreRegisterLayers" );
}

ULONG_PTR WINAPI D3D10CreateDevice( ULONG_PTR a1, ULONG_PTR a2, ULONG_PTR a3, ULONG_PTR a4, ULONG_PTR a5, ULONG_PTR a6 )
{
    __wine_spec_unimplemented_stub( "d3d11.dll", "D3D10CreateDevice" );
}

ULONG_PTR WINAPI D3D10CreateDeviceAndSwapChain( ULONG_PTR a1, ULONG_PTR a2, ULONG_PTR a3, ULONG_PTR a4, ULONG_PTR a5, ULONG_PTR a6, ULONG_PTR a7, ULONG_PTR a8 )
{
    __wine_spec_unimplemented_stub( "d3d11.dll", "D3D10CreateDeviceAndSwapChain" );
}

#endif  /* __powerpc64__ */
