/*
 * d3d9_smoke -- the native-vs-guest D3D9 RUNTIME gate, d3d11_smoke.c's
 * design applied one lane over.
 *
 * ONE source, built TWICE: once as a native ppc64le ELF binary that
 * dlopens DXVK's own libdxvk_d3d9.so directly (no Wine, no emulation
 * anywhere in the process), and once as an x86-64 Windows PE that imports
 * d3d9.dll and is meant to run as a GUEST under this port's wine.  The
 * GUEST HALF OF THIS FILE HAS NEVER BEEN BUILT OR RUN: the guest-side
 * d3d9.dll thunk does not exist in this port yet (only d3d11/dxgi/d3d10core
 * do -- see ppc64le/dxvk/README.md).  It is written to the exact contract
 * a future d3d9 thunk must satisfy, in the same way d3d11_smoke.c's guest
 * half was presumably written before its own thunk existed, and it will
 * start proving the boundary the day that thunk links.  Only the NATIVE
 * half has been compiled, linked and RUN, on the test machine, against the real DXVK
 * d3d9 library and real RADV silicon -- see the run transcript this
 * probe's commit message / accompanying report carries.
 *
 * WHY D3D9 NEEDS ITS OWN PROBE RATHER THAN REUSING d3d11_smoke's SHAPE
 * VERBATIM.  D3D11 has no notion of an implicit swapchain requirement:
 * D3D11CreateDevice happily returns a bare ID3D11Device with no window and
 * no swapchain at all, so d3d11_smoke.c never had to answer whether a
 * headless device can be created -- it just never asks for a swapchain.
 * D3D9 has no such option: IDirect3D9::CreateDevice ALWAYS creates one
 * implicit swapchain as part of the device, tied to the HWND the caller
 * passes in, whether or not the application ever intends to present.  So
 * the very first question this lane has to answer, before a single texel
 * can be checked, is whether that implicit swapchain can come into being
 * with no display server in the loop at all.  That is not something to
 * assume from reading the spec -- D3D9's implicit-swapchain requirement
 * is exactly the kind of thing a runtime is free to implement by either
 * eagerly binding a real platform surface or deferring it, and only
 * MEASUREMENT settles which DXVK does.  It was measured, by hand, with a
 * throwaway probe built with this exact recipe against
 * ~/Projects/power8/wine-ppc64le/ppc64le/dxvk-build/src/d3d9/libdxvk_d3d9.so
 * on the test machine (POWER9, V620/RADV), 2026-08-17:
 *
 *   env -u DISPLAY -u WAYLAND_DISPLAY -u XDG_RUNTIME_DIR \
 *       DXVK_WSI_DRIVER=Headless <probe> libdxvk_d3d9.so
 *
 *   -> CreateDevice hr=0x8876086A (D3DERR_NOTAVAILABLE), device=NULL
 *   -> stderr: "Foreign WSI: headless driver cannot create a surface"
 *              "Presenter: Failed to create Vulkan surface: VK_ERROR_INITIALIZATION_FAILED"
 *              "Failed to create Vulkan surface, VK_ERROR_INITIALIZATION_FAILED"
 *
 * The chain that produces that failure, read start to finish in DXVK's own
 * source (ppc64le/dxvk/src/src/...):
 *
 *   - d3d9_swapchain.cpp's D3D9SwapChainEx constructor calls
 *     UpdateWindowCtx() unconditionally, which calls CreatePresenter()
 *     unconditionally, which constructs a `new Presenter(...)`.  This runs
 *     during IDirect3D9::CreateDevice itself (via D3D9DeviceEx::InitialReset
 *     -> ResetSwapChain), not lazily at first Present.
 *   - dxvk_presenter.cpp's Presenter constructor:
 *       "if (!desc.deferSurfaceCreation) {
 *          VkResult vr = createSurface();
 *          if (vr != VK_SUCCESS && vr != VK_ERROR_NATIVE_WINDOW_IN_USE_KHR)
 *            throw DxvkError(...);
 *        }"
 *     so unless surface creation is explicitly deferred, the constructor
 *     itself tries to make a real VkSurfaceKHR right there and THROWS if it
 *     cannot.
 *   - d3d9_swapchain.cpp's CreatePresenter sets
 *       "presenterDesc.deferSurfaceCreation = m_parent->GetOptions()->deferSurfaceCreation;"
 *     and d3d9_options.cpp's default is
 *       "this->deferSurfaceCreation = config.getOption<bool>(\"d3d9.deferSurfaceCreation\", false);"
 *     -- FALSE.  (config.cpp lists "True" for this key under a number of
 *     *per-application* profiles for specific games with known headless-ish
 *     behaviour; none of those regexes match a probe binary, so the probe
 *     always gets the library-wide default, which is false.)
 *   - with deferSurfaceCreation false, the Presenter constructor calls
 *     createSurface() eagerly, which reaches
 *     wsi_window_foreign.cpp's ForeignWsiDriver::createSurface(): in
 *     headless mode (DXVK_WSI_DRIVER=Headless, m_useX11 == false) it is
 *       "Logger::warn(\"Foreign WSI: headless driver cannot create a surface\");
 *        return VK_ERROR_INITIALIZATION_FAILED;"
 *     unconditionally -- there is no offscreen-image fallback in this WSI
 *     backend, only a refusal.
 *   - VK_ERROR_INITIALIZATION_FAILED is neither VK_SUCCESS nor
 *     VK_ERROR_NATIVE_WINDOW_IN_USE_KHR, so the Presenter constructor
 *     throws DxvkError("Failed to create Vulkan surface, ...").  That
 *     exception unwinds out of D3D9SwapChainEx's constructor, out of
 *     InitialReset, and is caught in d3d9_interface.cpp's CreateDevice:
 *       "catch (const DxvkError& e) { Logger::err(e.message()); return D3DERR_NOTAVAILABLE; }"
 *
 * SETTING IT RIGHT: the SAME probe, unmodified, with one more environment
 * variable --
 *
 *   env -u DISPLAY -u WAYLAND_DISPLAY -u XDG_RUNTIME_DIR \
 *       DXVK_WSI_DRIVER=Headless \
 *       DXVK_CONFIG="d3d9.deferSurfaceCreation = True" \
 *       <probe> libdxvk_d3d9.so
 *
 *   -> CreateDevice hr=0x00000000, device=<non-null>
 *   -> CreateDeviceEx hr=0x00000000, device=<non-null>
 *
 * -- succeeds outright, because the Presenter constructor now skips
 * createSurface() entirely and CreateBackBuffers() (the implicit
 * swapchain's actual backbuffer images) never touches a VkSurfaceKHR at
 * all; it allocates ordinary DxvkImages exactly the way d3d11_smoke's own
 * render-target texture does.  THIS IS WHY A FUTURE check-d3d9-smoke.sh
 * MUST set DXVK_CONFIG="d3d9.deferSurfaceCreation = True" alongside
 * DXVK_WSI_DRIVER=Headless -- omitting it makes CreateDevice fail
 * deterministically, every run, regardless of what HWND value is passed.
 * (The fabricated HWND value itself -- (HWND)(UINT_PTR)0x1 below -- turned
 * out not to matter for CreateDevice at all: ForeignWsiDriver's headless
 * mode never dereferences it before the point analysed above, and once
 * deferSurfaceCreation defers surface creation past CreateDevice, nothing
 * up to and including GetRenderTargetData below asks the WSI layer to
 * resolve it either. Presenting to it would be a different question this
 * probe does not answer, because it never calls Present -- see below.)
 *
 * A REAL, SEPARATE HEADER DEFECT FOUND WHILE WRITING THE NATIVE LEG, NOT
 * PART OF THE QUESTION ABOVE.  ppc64le/dxvk/src/include/native/windows/
 * windows_base.h #defines THIS_ and THIS as EMPTY, unconditionally, with
 * no __cplusplus or CINTERFACE guard (lines 309-310).  d3d9.h (unlike
 * d3d11.h, which is generated in the modern widl style and spells
 * "ID3D11Device *This" explicitly in every vtable slot and never reads
 * THIS_) is written in the classic DirectX-SDK "STDMETHOD(Foo)(THIS_ args)
 * PURE" style throughout.  With THIS_ empty, every vtable function-pointer
 * TYPE this header generates for a d3d9 interface is missing its own
 * leading this-pointer parameter -- while the SAME header's own
 * IDirect3D9_CreateDevice(p,a,b,c,d,e,f)-style macros still pass p as an
 * explicit extra argument, so the two halves of the header disagree with
 * each other and GCC catches it as a real type mismatch (confirmed: it
 * reports the vtable slot's declared type as taking UINT first, not
 * IDirect3D9*).  DXVK's actual compiled vtable slot obviously HAS an
 * implicit this-pointer -- it is an ordinary C++ virtual method under the
 * hood -- so calling through the header's own macro as declared would be a
 * genuine ABI mismatch, not a warning to silence with a cast.  Every d3d9
 * COM call in the native leg below is therefore made through a hand-typed
 * function-pointer cast of the vtable SLOT (call_* below), which is
 * correct regardless of what THIS_ expands to, because it never asks the
 * header what the slot's type is supposed to be -- only where it lives.
 * This is exactly the technique dxvk_flat_surface.h exists for at the flat-
 * export layer (ppc64le/dxvk/README.md); here it is the same idea one
 * level down, at individual vtable slots, because d3d9.h's own macros
 * cannot be trusted for this header set.  It does not need editing this
 * header to fix (that header is bootstrapped by ppc64le/dxvk/bootstrap.sh
 * from a pinned upstream commit and is not this project's to hand-patch);
 * it needs routing around, which is what call_* does, and doing so in the
 * SHARED body below (rather than only in the native-only top section)
 * means the same known-correct calling convention is used on both legs,
 * so a header quirk on one side can never become a silent difference in
 * what the two legs actually asked DXVK to do.
 *
 * D3D9's Clear() TAKES THE COLOUR AS A DIRECT 8-BIT ARGB INTEGER, UNLIKE
 * D3D11's FLOAT CLEAR.  d3d11_smoke.c spends a full paragraph on rounding
 * 0.25f and 0.5f to 8-bit UNORM channels because D3D11's ClearRenderTargetView
 * takes FLOAT[4] and the driver has to convert; D3D9's Clear() takes a
 * D3DCOLOR (a bare DWORD, D3DCOLOR_ARGB(a,r,g,b) with each channel already
 * an 8-bit integer) and there is no float-to-UNORM conversion anywhere in
 * the call -- the bytes this probe asks for are the exact bytes DXVK is
 * asked to write, byte for byte, with no rounding question to verify.  The
 * chosen colour, D3DCOLOR_ARGB(0xFF, 0x00, 0x40, 0x80), reuses
 * d3d11_smoke's own R=0x00 G=0x40 B=0x80 A=0xFF for continuity between the
 * two probes' transcripts, not because either 0x40 or 0x80 needed deriving
 * here.
 *
 * WHY THE RENDER TARGET IS D3DFMT_A8R8G8B8, NOT THE X8R8G8B8 THE PRESENT
 * PARAMETERS USE.  X8R8G8B8's alpha byte is architecturally unspecified
 * (d3d9_monitor.cpp's own comment: "This is still 32 bit even though the
 * alpha is unspecified"), which is fatal to a probe whose whole claim is
 * that it checked EVERY byte of EVERY texel -- an unspecified byte cannot
 * be asserted equal to anything and honestly reported as checked.  So the
 * present parameters keep D3DFMT_X8R8G8B8 (matching the exact
 * configuration measured above, in case the X-channel path itself ever
 * turns out to matter to device creation), but the actual texture this
 * probe clears and reads back is a SEPARATE offscreen D3DFMT_A8R8G8B8
 * render target -- mirroring d3d11_smoke.c's own choice to check a
 * dedicated render-target texture rather than the implicit swapchain's
 * backbuffer.  d3d9_format.cpp maps both D3D9Format::A8R8G8B8 and
 * D3D9Format::X8R8G8B8 to VK_FORMAT_B8G8R8A8_UNORM with no swizzle for
 * A8R8G8B8, so a locked A8R8G8B8 surface's in-memory byte order is exactly
 * B,G,R,A (byte 0 = blue) on this little-endian target -- which is why the
 * per-texel check below compares texel[0..3] against B,G,R,A in that
 * order, not R,G,B,A the way d3d11_smoke.c's R8G8B8A8_UNORM texture does.
 *
 * THE READBACK PATH: GetRenderTargetData, not CopyResource+Map.  D3D9 has
 * no CopyResource; the documented way to read a render target back to the
 * CPU is CreateOffscreenPlainSurface(D3DPOOL_SYSTEMMEM) + GetRenderTargetData
 * (a driver-side copy from the render target's D3DPOOL_DEFAULT memory into
 * the system-memory surface) + LockRect/UnlockRect on the SYSTEMMEM
 * surface -- the D3D9 equivalent of d3d11_smoke's STAGING texture + Map,
 * one API generation earlier.
 *
 * WHAT THIS DOES NOT COVER.  Exactly what d3d11_smoke.c's own "WHAT THIS
 * DOES NOT COVER" paragraph says, one word substituted: no swapchain
 * PRESENTATION, no window, no shader.  CreateDevice's implicit swapchain
 * exists (it has to, to get a device at all) but Present() is never
 * called, and what Present() would do with the fabricated HWND once
 * deferSurfaceCreation lets it reach real surface creation is a genuinely
 * open question this probe does not answer -- it proves memory movement
 * through a real, off-screen render target (create, clear, read back) and
 * nothing about presentation working at all.
 *
 * SMOKE_BREAK (falsification -- mirrors d3d11_smoke.c's own three cases,
 * same numbering, same meaning, applied to this probe's Clear/check instead):
 *
 *   -DSMOKE_BREAK=1   skip the Clear call entirely.  The render target
 *                      keeps whatever the driver left in freshly allocated
 *                      GPU memory, which the checked colour has no reason
 *                      to match, so the per-texel check goes red.
 *   -DSMOKE_BREAK=2   swap the expected R and B bytes in the CPU's own
 *                      check (the actual rendered bytes are untouched;
 *                      the CHECK is deliberately wrong), so all 4096
 *                      per-texel comparisons mismatch.
 *   -DSMOKE_BREAK=3   restrict the per-texel walk to texel (0,0) only.
 *                      Coverage is part of the claim ("every one of the
 *                      4096 texels"), so the verdict requires
 *                      checked_count == 4096 as well as mismatches == 0;
 *                      an incomplete scan fails on that arithmetic alone.
 *   -DSMOKE_BREAK=4   skip the SECOND depth Clear (step 11) only.  The
 *                      depth surface still holds step 10's 0.25f, so the
 *                      check against 0.75f mismatches on all 4096 texels.
 *                      This is the case that proves the two depth values
 *                      are really being read rather than one of them being
 *                      matched by a buffer nobody wrote.
 *   -DSMOKE_BREAK=5   check step 10's depth texels against 0.5f, a value
 *                      never written by anything (the actual depth data is
 *                      untouched; the CHECK is deliberately wrong), so all
 *                      4096 depth comparisons mismatch.  The depth
 *                      equivalent of case 2.
 *
 *   [MEASURED] 2026-08-17, the test machine: native leg built and run under
 *   env -u DISPLAY -u WAYLAND_DISPLAY -u XDG_RUNTIME_DIR
 *   DXVK_WSI_DRIVER=Headless DXVK_CONFIG="d3d9.deferSurfaceCreation = True",
 *   baseline reported "d3d9_smoke: PASS 8/8" (checksum fnv1a=0x4C431DC5,
 *   texels=4096 mismatches=0) and exited 0; SMOKE_BREAK=1, =2 and =3 each
 *   reported "d3d9_smoke: FAIL 7/8 (did not confirm every one of the 4096
 *   texels)" and each exited 1.  Step 8 -- the by-value float round trip --
 *   was added after that run, so the step COUNTS are now 9 and 8/9; the
 *   texel numbers are unchanged and the SMOKE_BREAK verdicts are the same
 *   sentence one step later.  Run without DXVK_CONFIG set at all --
 *   the negative control for the finding two sections up -- step 2 itself
 *   reports hr=0x8876086A and the run FAILs at 2/3, exactly as the header
 *   banner's chain of reasoning predicts.
 *
 * NO C RUNTIME on the guest side (-DD3D9_SMOKE_NO_CRT would be the flag if
 * this file needed one; it does not, by the same reasoning
 * d3d11_smoke.c's header gives): the program formats its own output with
 * the same hand-written out_hex/out_dec/out_hr/out routines, copied
 * verbatim rather than shared via a header, so that formatting can never
 * be the source of a byte difference between native and guest.  The
 * native leg links an ordinary libc (it is a plain Linux ELF program) but
 * never calls printf -- same discipline, same reason.
 *
 * Copyright 2026 the ppc64le port authors
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 */

#define COBJMACROS

#ifndef SMOKE_BREAK
#define SMOKE_BREAK 0
#endif

#if defined(D3D9_SMOKE_NATIVE)

/* ---- the native ppc64le ELF leg -----------------------------------------
 * An ordinary Linux program.  DXVK's Direct3DCreate9 is resolved with
 * dlopen/dlsym exactly the way d3d11_smoke.c's native leg resolves
 * D3D11CreateDevice, and exactly the way a thunk host would resolve it. */
#include <windows.h>
#include <d3d9.h>
#include <dlfcn.h>
#include <unistd.h>

typedef IDirect3D9 *(WINAPI *PFN_Direct3DCreate9)(UINT);
static PFN_Direct3DCreate9 p_Direct3DCreate9;

#define Direct3DCreate9_CALL   p_Direct3DCreate9

/* argv[1] = path to libdxvk_d3d9.so, passed on the command line rather
 * than baked in, the same way check-d3d11-smoke.sh hands its native leg
 * libdxvk_d3d11.so and the same way a thunk host would be handed it. */
static int native_resolve( int argc, char **argv )
{
    void *d3d9_lib;

    if (argc < 2)
    {
        static const char usage[] = "usage: d3d9_smoke <libdxvk_d3d9.so>\n";
        write( 2, usage, sizeof(usage) - 1 );
        return 0;
    }
    if (!(d3d9_lib = dlopen( argv[1], RTLD_NOW ))) return 0;
    p_Direct3DCreate9 = (PFN_Direct3DCreate9) dlsym( d3d9_lib, "Direct3DCreate9" );
    return p_Direct3DCreate9 != NULL;
}

static void out( const char *s )
{
    size_t n = 0;
    while (s[n]) n++;
    write( 1, s, n );
}

#else /* !D3D9_SMOKE_NATIVE */

/* ---- the guest x86-64 PE leg (untested -- see header banner) -----------
 * Imports d3d9.dll the way a real application does; meant to run under
 * this port's wine once a d3d9 guest thunk exists.  No CRT -- see the
 * header comment above. */
#include <windows.h>
#include <d3d9.h>

#define Direct3DCreate9_CALL   Direct3DCreate9

static void out( const char *s )
{
    DWORD n = 0, written;
    while (s[n]) n++;
    WriteFile( GetStdHandle( STD_OUTPUT_HANDLE ), s, n, &written, NULL );
}

#endif /* D3D9_SMOKE_NATIVE */

/* ---------------------------------------------------- everything below this
 * line is identical between the two legs. ----------------------------------
 *
 * BOOL and not WINBOOL, which is the spelling d3d9.h's own prototypes use.
 * DXVK's vendored windows_base.h has `typedef INT BOOL; typedef BOOL WINBOOL;`
 * so both work on the NATIVE leg, and Wine's headers declare only BOOL -- so
 * WINBOOL compiles on one leg and not the other, which is the one shape a
 * one-source-two-builds probe must never have. */

/* ---- hand-typed vtable-slot calls, in place of this header's own
 * IDirect3D9_Foo(...)-style macros -- see the header banner's "A REAL,
 * SEPARATE HEADER DEFECT" paragraph for why the macros cannot be trusted
 * here.  Each one casts the vtable SLOT (never the header's declared type
 * for it) to the calling convention DXVK's compiled vtable actually has:
 * an explicit this-pointer first, exactly the way every C++ virtual method
 * is compiled, regardless of what THIS_ expanded to when this header's
 * struct was declared. */

typedef HRESULT (WINAPI *PFN_CreateDevice)(
    IDirect3D9 *, UINT, D3DDEVTYPE, HWND, DWORD,
    D3DPRESENT_PARAMETERS *, IDirect3DDevice9 **);
typedef ULONG   (WINAPI *PFN_Release_D3D9)(IDirect3D9 *);
typedef ULONG   (WINAPI *PFN_Release_Device)(IDirect3DDevice9 *);
typedef ULONG   (WINAPI *PFN_Release_Surface)(IDirect3DSurface9 *);
typedef HRESULT (WINAPI *PFN_CreateRenderTarget)(
    IDirect3DDevice9 *, UINT, UINT, D3DFORMAT, D3DMULTISAMPLE_TYPE, DWORD,
    BOOL, IDirect3DSurface9 **, HANDLE *);
typedef HRESULT (WINAPI *PFN_SetRenderTarget)(
    IDirect3DDevice9 *, DWORD, IDirect3DSurface9 *);
typedef HRESULT (WINAPI *PFN_Clear)(
    IDirect3DDevice9 *, DWORD, const D3DRECT *, DWORD, D3DCOLOR, float, DWORD);
typedef HRESULT (WINAPI *PFN_CreateOffscreenPlainSurface)(
    IDirect3DDevice9 *, UINT, UINT, D3DFORMAT, D3DPOOL, IDirect3DSurface9 **, HANDLE *);
typedef HRESULT (WINAPI *PFN_GetRenderTargetData)(
    IDirect3DDevice9 *, IDirect3DSurface9 *, IDirect3DSurface9 *);
typedef HRESULT (WINAPI *PFN_LockRect)(
    IDirect3DSurface9 *, D3DLOCKED_RECT *, const RECT *, DWORD);
typedef HRESULT (WINAPI *PFN_UnlockRect)(IDirect3DSurface9 *);
typedef HRESULT (WINAPI *PFN_SetNPatchMode)(IDirect3DDevice9 *, float);
typedef float   (WINAPI *PFN_GetNPatchMode)(IDirect3DDevice9 *);
typedef HRESULT (WINAPI *PFN_CreateDepthStencilSurface)(
    IDirect3DDevice9 *, UINT, UINT, D3DFORMAT, D3DMULTISAMPLE_TYPE, DWORD,
    BOOL, IDirect3DSurface9 **, HANDLE *);
typedef HRESULT (WINAPI *PFN_SetDepthStencilSurface)(
    IDirect3DDevice9 *, IDirect3DSurface9 *);

static HRESULT call_CreateDevice( IDirect3D9 *d3d9, UINT adapter, D3DDEVTYPE type,
    HWND hwnd, DWORD flags, D3DPRESENT_PARAMETERS *pp, IDirect3DDevice9 **out_dev )
{
    return ((PFN_CreateDevice)d3d9->lpVtbl->CreateDevice)( d3d9, adapter, type, hwnd, flags, pp, out_dev );
}
static void call_Release_D3D9( IDirect3D9 *p )
{
    if (p) ((PFN_Release_D3D9)p->lpVtbl->Release)( p );
}
static void call_Release_Device( IDirect3DDevice9 *p )
{
    if (p) ((PFN_Release_Device)p->lpVtbl->Release)( p );
}
static void call_Release_Surface( IDirect3DSurface9 *p )
{
    if (p) ((PFN_Release_Surface)p->lpVtbl->Release)( p );
}
static HRESULT call_CreateRenderTarget( IDirect3DDevice9 *dev, UINT w, UINT h, D3DFORMAT fmt,
    D3DMULTISAMPLE_TYPE ms, DWORD msq, BOOL lockable, IDirect3DSurface9 **out_surf )
{
    return ((PFN_CreateRenderTarget)dev->lpVtbl->CreateRenderTarget)(
        dev, w, h, fmt, ms, msq, lockable, out_surf, NULL );
}
static HRESULT call_SetRenderTarget( IDirect3DDevice9 *dev, DWORD idx, IDirect3DSurface9 *surf )
{
    return ((PFN_SetRenderTarget)dev->lpVtbl->SetRenderTarget)( dev, idx, surf );
}
static HRESULT call_Clear( IDirect3DDevice9 *dev, D3DCOLOR colour )
{
    return ((PFN_Clear)dev->lpVtbl->Clear)( dev, 0, NULL, D3DCLEAR_TARGET, colour, 0.0f, 0 );
}
/* The same slot with D3DCLEAR_ZBUFFER instead, which is what carries the
 * float Z.  Kept separate from call_Clear rather than adding parameters to
 * it, so the colour steps above are textually untouched by the depth work
 * below and a reader can see at a glance which flag each step passes. */
static HRESULT call_ClearZ( IDirect3DDevice9 *dev, float z )
{
    return ((PFN_Clear)dev->lpVtbl->Clear)( dev, 0, NULL, D3DCLEAR_ZBUFFER, 0, z, 0 );
}
static HRESULT call_CreateDepthStencilSurface( IDirect3DDevice9 *dev, UINT w, UINT h,
    D3DFORMAT fmt, IDirect3DSurface9 **out_surf )
{
    /* Discard=FALSE is the load-bearing argument: with TRUE the runtime is
     * entitled to throw the contents away the moment the surface stops being
     * bound, and this probe's whole claim is about what is IN it afterwards. */
    return ((PFN_CreateDepthStencilSurface)dev->lpVtbl->CreateDepthStencilSurface)(
        dev, w, h, fmt, D3DMULTISAMPLE_NONE, 0, FALSE, out_surf, NULL );
}
static HRESULT call_SetDepthStencilSurface( IDirect3DDevice9 *dev, IDirect3DSurface9 *surf )
{
    return ((PFN_SetDepthStencilSurface)dev->lpVtbl->SetDepthStencilSurface)( dev, surf );
}
static HRESULT call_CreateOffscreenPlainSurface( IDirect3DDevice9 *dev, UINT w, UINT h,
    D3DFORMAT fmt, D3DPOOL pool, IDirect3DSurface9 **out_surf )
{
    return ((PFN_CreateOffscreenPlainSurface)dev->lpVtbl->CreateOffscreenPlainSurface)(
        dev, w, h, fmt, pool, out_surf, NULL );
}
static HRESULT call_GetRenderTargetData( IDirect3DDevice9 *dev, IDirect3DSurface9 *rt, IDirect3DSurface9 *dst )
{
    return ((PFN_GetRenderTargetData)dev->lpVtbl->GetRenderTargetData)( dev, rt, dst );
}
static HRESULT call_LockRect( IDirect3DSurface9 *surf, D3DLOCKED_RECT *lr, DWORD flags )
{
    return ((PFN_LockRect)surf->lpVtbl->LockRect)( surf, lr, NULL, flags );
}
#ifndef __i386__
/* Defined only where they are called.  The i386 leg does not attempt
 * NPatchMode at all -- see the step that says so -- and a helper it cannot
 * use would only warn. */
static HRESULT call_SetNPatchMode( IDirect3DDevice9 *dev, float segments )
{
    return ((PFN_SetNPatchMode)dev->lpVtbl->SetNPatchMode)( dev, segments );
}
static float call_GetNPatchMode( IDirect3DDevice9 *dev )
{
    return ((PFN_GetNPatchMode)dev->lpVtbl->GetNPatchMode)( dev );
}
#endif
static HRESULT call_UnlockRect( IDirect3DSurface9 *surf )
{
    return ((PFN_UnlockRect)surf->lpVtbl->UnlockRect)( surf );
}

#ifdef __i386__
/* ---- the Lock-family helpers, for the i386 leg's bounce steps ------------
 *
 * Guarded to the leg that calls them, like the two NPatchMode helpers above
 * and for the same reason: a static function no leg calls is a warning, and
 * only the i386 leg bounces anything (see the bounce steps' banner).  The
 * day those steps are promoted to every leg this guard comes off with them. */
typedef HRESULT (WINAPI *PFN_CreateTexture)(
    IDirect3DDevice9 *, UINT, UINT, UINT, DWORD, D3DFORMAT, D3DPOOL,
    IDirect3DTexture9 **, HANDLE *);
typedef HRESULT (WINAPI *PFN_TexLockRect)(
    IDirect3DTexture9 *, UINT, D3DLOCKED_RECT *, const RECT *, DWORD);
typedef HRESULT (WINAPI *PFN_TexUnlockRect)(IDirect3DTexture9 *, UINT);
typedef ULONG   (WINAPI *PFN_Release_Texture)(IDirect3DTexture9 *);
typedef HRESULT (WINAPI *PFN_CreateVertexBuffer)(
    IDirect3DDevice9 *, UINT, DWORD, DWORD, D3DPOOL,
    IDirect3DVertexBuffer9 **, HANDLE *);
typedef HRESULT (WINAPI *PFN_VBLock)(
    IDirect3DVertexBuffer9 *, UINT, UINT, void **, DWORD);
typedef HRESULT (WINAPI *PFN_VBUnlock)(IDirect3DVertexBuffer9 *);
typedef ULONG   (WINAPI *PFN_Release_VB)(IDirect3DVertexBuffer9 *);

static HRESULT call_CreateTexture( IDirect3DDevice9 *dev, UINT w, UINT h, UINT levels,
    D3DFORMAT fmt, D3DPOOL pool, IDirect3DTexture9 **out_tex )
{
    return ((PFN_CreateTexture)dev->lpVtbl->CreateTexture)(
        dev, w, h, levels, 0, fmt, pool, out_tex, NULL );
}
static HRESULT call_TexLockRect( IDirect3DTexture9 *tex, UINT level, D3DLOCKED_RECT *lr,
    const RECT *rect, DWORD flags )
{
    return ((PFN_TexLockRect)tex->lpVtbl->LockRect)( tex, level, lr, rect, flags );
}
static HRESULT call_TexUnlockRect( IDirect3DTexture9 *tex, UINT level )
{
    return ((PFN_TexUnlockRect)tex->lpVtbl->UnlockRect)( tex, level );
}
static void call_Release_Texture( IDirect3DTexture9 *p )
{
    if (p) ((PFN_Release_Texture)p->lpVtbl->Release)( p );
}
static HRESULT call_CreateVertexBuffer( IDirect3DDevice9 *dev, UINT length, D3DPOOL pool,
    IDirect3DVertexBuffer9 **out_vb )
{
    return ((PFN_CreateVertexBuffer)dev->lpVtbl->CreateVertexBuffer)(
        dev, length, 0, 0, pool, out_vb, NULL );
}
static HRESULT call_VBLock( IDirect3DVertexBuffer9 *vb, UINT offset, UINT size,
    void **bits, DWORD flags )
{
    return ((PFN_VBLock)vb->lpVtbl->Lock)( vb, offset, size, bits, flags );
}
static HRESULT call_VBUnlock( IDirect3DVertexBuffer9 *vb )
{
    return ((PFN_VBUnlock)vb->lpVtbl->Unlock)( vb );
}
static void call_Release_VB( IDirect3DVertexBuffer9 *p )
{
    if (p) ((PFN_Release_VB)p->lpVtbl->Release)( p );
}
typedef HRESULT (WINAPI *PFN_CreateCubeTexture)(
    IDirect3DDevice9 *, UINT, UINT, DWORD, D3DFORMAT, D3DPOOL,
    IDirect3DCubeTexture9 **, HANDLE *);
typedef HRESULT (WINAPI *PFN_CubeLockRect)(
    IDirect3DCubeTexture9 *, D3DCUBEMAP_FACES, UINT, D3DLOCKED_RECT *, const RECT *, DWORD);
typedef HRESULT (WINAPI *PFN_CubeUnlockRect)(
    IDirect3DCubeTexture9 *, D3DCUBEMAP_FACES, UINT);
typedef ULONG   (WINAPI *PFN_Release_Cube)(IDirect3DCubeTexture9 *);
typedef HRESULT (WINAPI *PFN_CreateVolumeTexture)(
    IDirect3DDevice9 *, UINT, UINT, UINT, UINT, DWORD, D3DFORMAT, D3DPOOL,
    IDirect3DVolumeTexture9 **, HANDLE *);
typedef HRESULT (WINAPI *PFN_VolTexLockBox)(
    IDirect3DVolumeTexture9 *, UINT, D3DLOCKED_BOX *, const D3DBOX *, DWORD);
typedef HRESULT (WINAPI *PFN_VolTexUnlockBox)(IDirect3DVolumeTexture9 *, UINT);
typedef HRESULT (WINAPI *PFN_GetVolumeLevel)(
    IDirect3DVolumeTexture9 *, UINT, IDirect3DVolume9 **);
typedef ULONG   (WINAPI *PFN_Release_VolTex)(IDirect3DVolumeTexture9 *);
typedef HRESULT (WINAPI *PFN_VolLockBox)(
    IDirect3DVolume9 *, D3DLOCKED_BOX *, const D3DBOX *, DWORD);
typedef HRESULT (WINAPI *PFN_VolUnlockBox)(IDirect3DVolume9 *);
typedef ULONG   (WINAPI *PFN_Release_Volume)(IDirect3DVolume9 *);
typedef HRESULT (WINAPI *PFN_CreateIndexBuffer)(
    IDirect3DDevice9 *, UINT, DWORD, D3DFORMAT, D3DPOOL,
    IDirect3DIndexBuffer9 **, HANDLE *);
typedef HRESULT (WINAPI *PFN_IBLock)(IDirect3DIndexBuffer9 *, UINT, UINT, void **, DWORD);
typedef HRESULT (WINAPI *PFN_IBUnlock)(IDirect3DIndexBuffer9 *);
typedef ULONG   (WINAPI *PFN_Release_IB)(IDirect3DIndexBuffer9 *);

static HRESULT call_CreateCubeTexture( IDirect3DDevice9 *dev, UINT edge, UINT levels,
    D3DFORMAT fmt, D3DPOOL pool, IDirect3DCubeTexture9 **out_tex )
{
    return ((PFN_CreateCubeTexture)dev->lpVtbl->CreateCubeTexture)(
        dev, edge, levels, 0, fmt, pool, out_tex, NULL );
}
static HRESULT call_CubeLockRect( IDirect3DCubeTexture9 *tex, D3DCUBEMAP_FACES face,
    UINT level, D3DLOCKED_RECT *lr, DWORD flags )
{
    return ((PFN_CubeLockRect)tex->lpVtbl->LockRect)( tex, face, level, lr, NULL, flags );
}
static HRESULT call_CubeUnlockRect( IDirect3DCubeTexture9 *tex, D3DCUBEMAP_FACES face, UINT level )
{
    return ((PFN_CubeUnlockRect)tex->lpVtbl->UnlockRect)( tex, face, level );
}
static void call_Release_Cube( IDirect3DCubeTexture9 *p )
{
    if (p) ((PFN_Release_Cube)p->lpVtbl->Release)( p );
}
static HRESULT call_CreateVolumeTexture( IDirect3DDevice9 *dev, UINT w, UINT h, UINT d,
    UINT levels, D3DFORMAT fmt, D3DPOOL pool, IDirect3DVolumeTexture9 **out_tex )
{
    return ((PFN_CreateVolumeTexture)dev->lpVtbl->CreateVolumeTexture)(
        dev, w, h, d, levels, 0, fmt, pool, out_tex, NULL );
}
static HRESULT call_VolTexLockBox( IDirect3DVolumeTexture9 *tex, UINT level,
    D3DLOCKED_BOX *lb, DWORD flags )
{
    return ((PFN_VolTexLockBox)tex->lpVtbl->LockBox)( tex, level, lb, NULL, flags );
}
static HRESULT call_VolTexUnlockBox( IDirect3DVolumeTexture9 *tex, UINT level )
{
    return ((PFN_VolTexUnlockBox)tex->lpVtbl->UnlockBox)( tex, level );
}
static HRESULT call_GetVolumeLevel( IDirect3DVolumeTexture9 *tex, UINT level,
    IDirect3DVolume9 **out_vol )
{
    return ((PFN_GetVolumeLevel)tex->lpVtbl->GetVolumeLevel)( tex, level, out_vol );
}
static void call_Release_VolTex( IDirect3DVolumeTexture9 *p )
{
    if (p) ((PFN_Release_VolTex)p->lpVtbl->Release)( p );
}
static HRESULT call_VolLockBox( IDirect3DVolume9 *vol, D3DLOCKED_BOX *lb, DWORD flags )
{
    return ((PFN_VolLockBox)vol->lpVtbl->LockBox)( vol, lb, NULL, flags );
}
static HRESULT call_VolUnlockBox( IDirect3DVolume9 *vol )
{
    return ((PFN_VolUnlockBox)vol->lpVtbl->UnlockBox)( vol );
}
static void call_Release_Volume( IDirect3DVolume9 *p )
{
    if (p) ((PFN_Release_Volume)p->lpVtbl->Release)( p );
}
static HRESULT call_CreateIndexBuffer( IDirect3DDevice9 *dev, UINT length, D3DPOOL pool,
    IDirect3DIndexBuffer9 **out_ib )
{
    return ((PFN_CreateIndexBuffer)dev->lpVtbl->CreateIndexBuffer)(
        dev, length, 0, D3DFMT_INDEX16, pool, out_ib, NULL );
}
static HRESULT call_IBLock( IDirect3DIndexBuffer9 *ib, UINT offset, UINT size,
    void **bits, DWORD flags )
{
    return ((PFN_IBLock)ib->lpVtbl->Lock)( ib, offset, size, bits, flags );
}
static HRESULT call_IBUnlock( IDirect3DIndexBuffer9 *ib )
{
    return ((PFN_IBUnlock)ib->lpVtbl->Unlock)( ib );
}
static void call_Release_IB( IDirect3DIndexBuffer9 *p )
{
    if (p) ((PFN_Release_IB)p->lpVtbl->Release)( p );
}
#endif /* __i386__ */

/* ------------------------------------------------------------- output */

static void out_hex( ULONG v, int digits )
{
    static const char hex[] = "0123456789ABCDEF";
    char buf[9];
    int i;

    for (i = 0; i < digits; i++) buf[digits - 1 - i] = hex[(v >> (4 * i)) & 0xf];
    buf[digits] = 0;
    out( buf );
}

static void out_dec( ULONG v )
{
    char buf[12];
    int i = 11;

    buf[i] = 0;
    do { buf[--i] = '0' + (char)(v % 10); v /= 10; } while (v);
    out( buf + i );
}

static void out_hr( const char *label, HRESULT hr )
{
    out( label );
    out( "=0x" );
    out_hex( (ULONG)hr, 8 );
}

/* ------------------------------------------------------------- the run */

static int failures;
static int step;
static const char *first_fail;

static void begin( const char *what )
{
    out( "step " );
    out_dec( ++step );
    out( " " );
    out( what );
    out( ": " );
}

static void verdict( BOOL ok, const char *why )
{
    if (ok) out( " ok\n" );
    else
    {
        if (!first_fail) first_fail = why;
        failures++;
        out( " FAIL (" );
        out( why );
        out( ")\n" );
    }
}

/* 32-bit FNV-1a over the bytes handed to it -- identical to d3d11_smoke.c's. */
static DWORD fnv1a( DWORD hash, const BYTE *p, UINT n )
{
    UINT i;
    for (i = 0; i < n; i++)
    {
        hash ^= p[i];
        hash *= 0x01000193u;
    }
    return hash;
}

static int d3d9_smoke_run( void )
{
    IDirect3D9 *d3d9 = NULL;
    IDirect3DDevice9 *device = NULL;
    IDirect3DSurface9 *rt = NULL, *sysmem = NULL, *ds = NULL;
    const UINT W = 64, H = 64;
    /* The two depth values steps 9-11 clear to and read back.  Both are
     * exactly representable in binary32 -- 0.25 is 0x3E800000 and 0.75 is
     * 0x3F400000, each a power-of-two mantissa -- and D3DFMT_D32F_LOCKABLE is
     * a straight VK_FORMAT_D32_SFLOAT, so the value a Clear writes is the
     * value a LockRect reads, bit for bit, with no rounding anywhere in
     * between.  That is what lets the check below be exact equality rather
     * than a tolerance, which is the whole point: a tolerance would hide
     * precisely the boundary bug this gate is looking for.  Two DIFFERENT
     * values, because one alone cannot tell a depth buffer that was really
     * cleared from one that happened to contain the expected number. */
    const DWORD DEPTH_A_BITS = 0x3E800000u;   /* 0.25f */
    const DWORD DEPTH_B_BITS = 0x3F400000u;   /* 0.75f */
    /* A fabricated, deliberately bogus HWND: nothing in this probe ever
     * created a window, and per the header banner's measurement, nothing
     * this probe does (device creation with deferSurfaceCreation forced,
     * an offscreen render target, GetRenderTargetData) ever dereferences
     * it -- see the header banner for exactly which DXVK code path proves
     * that, and what calling this on a real one (Present) would still be
     * an open question. */
    HWND fake_hwnd = (HWND)(UINT_PTR)0x1;
    HRESULT hr;

    out( "d3d9_smoke: start\n" );

    /* ---- step 1: Direct3DCreate9 ------------------------------------- */
    begin( "Direct3DCreate9(D3D_SDK_VERSION)" );
    d3d9 = Direct3DCreate9_CALL( D3D_SDK_VERSION );
    verdict( d3d9 != NULL, "Direct3DCreate9 returned NULL" );
    if (!d3d9) goto done;

    /* ---- step 2: CreateDevice(HAL, HWVP, windowed 64x64 X8R8G8B8) ----
     * The route the header banner's measurement proved works: HARDWARE
     * vertex processing, D3DDEVTYPE_HAL, windowed present parameters with
     * the fabricated HWND as hDeviceWindow.  Requires the CALLER (a future
     * check-d3d9-smoke.sh) to run this process with
     * DXVK_CONFIG="d3d9.deferSurfaceCreation = True" in the environment,
     * in addition to DXVK_WSI_DRIVER=Headless -- see the header banner for
     * why omitting either one makes this step fail every time, by design,
     * not by flakiness. */
    begin( "CreateDevice(HAL, HWVP, windowed 64x64 X8R8G8B8, fabricated HWND)" );
    {
        /* Every field assigned by hand, no memset: the guest leg links no
         * CRT at all (see header banner), and a struct-zero-initializer
         * this size can lower to a compiler-synthesized memset call under
         * optimisation, which would fail to link there -- the same reason
         * d3d11_smoke.c's own D3D11_TEXTURE2D_DESC is built field by field. */
        D3DPRESENT_PARAMETERS pp;
        pp.BackBufferWidth          = W;
        pp.BackBufferHeight         = H;
        pp.BackBufferFormat         = D3DFMT_X8R8G8B8;
        pp.BackBufferCount          = 1;
        pp.MultiSampleType          = D3DMULTISAMPLE_NONE;
        pp.MultiSampleQuality       = 0;
        pp.SwapEffect               = D3DSWAPEFFECT_DISCARD;
        pp.hDeviceWindow            = fake_hwnd;
        pp.Windowed                 = TRUE;
        pp.EnableAutoDepthStencil   = FALSE;
        pp.AutoDepthStencilFormat   = 0;
        pp.Flags                    = 0;
        pp.FullScreen_RefreshRateInHz = 0;
        pp.PresentationInterval     = D3DPRESENT_INTERVAL_IMMEDIATE;

        hr = call_CreateDevice( d3d9, D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, fake_hwnd,
                                 D3DCREATE_HARDWARE_VERTEXPROCESSING, &pp, &device );
    }
    out_hr( "hr", hr );
    verdict( SUCCEEDED(hr) && device != NULL, "device missing (see header banner: "
             "did the caller set DXVK_CONFIG=\"d3d9.deferSurfaceCreation = True\"?)" );
    if (!device) goto done;

    /* ---- step 3: a 64x64 A8R8G8B8 offscreen render target ------------- */
    begin( "CreateRenderTarget(64x64 A8R8G8B8, not lockable)" );
    hr = call_CreateRenderTarget( device, W, H, D3DFMT_A8R8G8B8, D3DMULTISAMPLE_NONE, 0, FALSE, &rt );
    out_hr( "hr", hr );
    verdict( SUCCEEDED(hr) && rt != NULL, "no render target" );

    /* ---- step 4: SetRenderTarget --------------------------------------- */
    begin( "SetRenderTarget(0, rt)" );
    if (rt) hr = call_SetRenderTarget( device, 0, rt );
    else hr = E_FAIL;
    out_hr( "hr", hr );
    verdict( SUCCEEDED(hr), "SetRenderTarget failed" );

    /* ---- step 5: Clear(D3DCOLOR_ARGB(0xFF,0x00,0x40,0x80)) ------------- */
    begin( "Clear(A=FF R=00 G=40 B=80, a direct 8-bit ARGB integer -- no rounding)" );
    if (rt)
    {
#if SMOKE_BREAK == 1
        out( "skipped (SMOKE_BREAK=1)" );
        hr = D3D_OK;
#else
        hr = call_Clear( device, D3DCOLOR_ARGB( 0xFF, 0x00, 0x40, 0x80 ) );
        out( "cleared" );
#endif
    }
    else { out( "no target to clear" ); hr = E_FAIL; }
    out_hr( " hr", hr );
    verdict( SUCCEEDED(hr) && rt != NULL, "no target to clear" );

    /* ---- step 6: a D3DPOOL_SYSTEMMEM surface + GetRenderTargetData ----- */
    begin( "CreateOffscreenPlainSurface(SYSTEMMEM) + GetRenderTargetData" );
    if (rt)
    {
        hr = call_CreateOffscreenPlainSurface( device, W, H, D3DFMT_A8R8G8B8, D3DPOOL_SYSTEMMEM, &sysmem );
        if (SUCCEEDED(hr) && sysmem)
            hr = call_GetRenderTargetData( device, rt, sysmem );
    }
    else hr = E_FAIL;
    out_hr( "hr", hr );
    verdict( SUCCEEDED(hr) && sysmem != NULL, "no system-memory readback surface" );

    /* ---- step 7: LockRect, walk all 4096 texels, UnlockRect ------------ */
    begin( "LockRect(READONLY) + walk all 4096 texels against the expected colour" );
    {
        UINT checked = 0, mismatches = 0;
        DWORD csum = 0x811c9dc5u;   /* FNV-1a offset basis, for step 7b */

        if (sysmem)
        {
            D3DLOCKED_RECT lr;
            hr = call_LockRect( sysmem, &lr, D3DLOCK_READONLY );
            if (SUCCEEDED(hr) && lr.pBits)
            {
                UINT x, y;
                for (y = 0; y < H; y++)
                {
                    const BYTE *row = (const BYTE *)lr.pBits + (size_t)y * lr.Pitch;
                    for (x = 0; x < W; x++)
                    {
                        /* D3DFMT_A8R8G8B8 -> VK_FORMAT_B8G8R8A8_UNORM, no
                         * swizzle (see header banner): byte order in
                         * memory is B, G, R, A. */
                        const BYTE *texel = row + x * 4;
                        BYTE eb = 0x80, eg = 0x40, er = 0x00, ea = 0xff;

                        csum = fnv1a( csum, texel, 4 );

#if SMOKE_BREAK == 3
                        if (x != 0 || y != 0) continue;
#endif
                        checked++;
#if SMOKE_BREAK == 2
                        { BYTE t = er; er = eb; eb = t; }   /* swap R and B in the CHECK */
#endif
                        if (texel[0] != eb || texel[1] != eg ||
                            texel[2] != er || texel[3] != ea)
                            mismatches++;
                    }
                }
                call_UnlockRect( sysmem );
            }
        }
        else hr = E_FAIL;

        out( "checked=" ); out_dec( checked );
        out( " mismatches=" ); out_dec( mismatches );
        verdict( SUCCEEDED(hr) && checked == W * H && mismatches == 0,
                 "did not confirm every one of the 4096 texels" );

        /* ---- step 7b (unnamed line, not a numbered step): the checksum,
         * over the FULL 4096 texels actually read back, independent of
         * SMOKE_BREAK=3's reduced coverage above -- see d3d11_smoke.c's
         * matching step 6b for why this line is unconditional. */
        out( "checksum: fnv1a=0x" ); out_hex( csum, 8 );
        out( " texels=" ); out_dec( W * H );
        out( " mismatches=" ); out_dec( mismatches );
        out( "\n" );
    }

#ifdef __i386__
    /* =================================================================
     *  THE BELOW-4-GiB LOCK BOUNCE -- i386 LEG ONLY
     * =================================================================
     *
     * WHY THESE THREE STEPS EXIST AND WHY THEY ARE GUARDED.  On this host
     * DXVK's d3d9 maps above 4 GiB, so on the i386 lane every LockRect,
     * LockBox and buffer Lock is served by a BOUNCE: dlls/d3d9/main.c hands
     * the guest a below-4-GiB buffer, fills it from the host mapping, and
     * copies it back before the host sees the Unlock.  Getting the SIZE of
     * that copy right is block-compressed mip arithmetic per format, and a
     * wrong size corrupts exactly the buffers the bounce exists to protect,
     * silently.  Steps above prove the READ direction on one uncompressed
     * full-surface lock; these three prove the three things that one cannot:
     * block-compressed pitch and the one-block mip floor, the WRITE
     * direction, and that a sub-rect flush leaves everything outside the
     * rect alone.
     *
     * They are #ifdef'd to i386 because check-d3d9-smoke.sh's gate is a DIFF
     * of the native leg against the x86-64 guest leg, neither of which
     * bounces anything -- adding steps there would change a gate this work
     * cannot run (it raises modal dialogs on a desktop somebody is using).
     * Promoting them to all three legs is a follow-up that needs that gate
     * run once, not a thing to do blind. */
    {
        IDirect3DTexture9 *dxt = NULL, *argb = NULL;
        IDirect3DCubeTexture9 *cube = NULL;
        IDirect3DVolumeTexture9 *voltex = NULL;
        IDirect3DVolume9 *vol = NULL;
        IDirect3DVertexBuffer9 *vb = NULL;
        IDirect3DIndexBuffer9 *ib = NULL;
        D3DLOCKED_RECT lr;
        UINT i, bad;

        /* ---- DXT1: the block pitch, and the mip that floors at one block --
         *
         * 64x64 DXT1 is 16x16 blocks of 8 bytes: pitch = align(8*16,4) = 128,
         * 16 block rows, 2048 bytes.  Level 6 is 1x1 TEXELS, which is still
         * ONE 4x4 BLOCK: pitch 8, one row, 8 bytes -- the case a pitch-times-
         * height sizing gets wrong by 4x in each direction and the case that
         * makes a bounce write off the end of the host's slice. */
        begin( "CreateTexture(64x64 DXT1, full chain) + LockRect L0 and L6: "
               "block pitch, write, read back" );
        bad = 0;
        hr = call_CreateTexture( device, 64, 64, 0, D3DFMT_DXT1, D3DPOOL_SYSTEMMEM, &dxt );
        if (SUCCEEDED(hr) && dxt)
        {
            UINT pitch0 = 0, pitch6 = 0;

            hr = call_TexLockRect( dxt, 0, &lr, NULL, 0 );
            if (SUCCEEDED(hr) && lr.pBits)
            {
                pitch0 = (UINT)lr.Pitch;
                for (i = 0; i < 16u * 128u; i++) ((BYTE *)lr.pBits)[i] = (BYTE)(i * 7 + 3);
                call_TexUnlockRect( dxt, 0 );
                hr = call_TexLockRect( dxt, 0, &lr, NULL, D3DLOCK_READONLY );
                if (SUCCEEDED(hr) && lr.pBits)
                {
                    for (i = 0; i < 16u * 128u; i++)
                        if (((const BYTE *)lr.pBits)[i] != (BYTE)(i * 7 + 3)) bad++;
                    call_TexUnlockRect( dxt, 0 );
                }
                else bad++;
            }
            else bad++;

            if (SUCCEEDED(hr))
            {
                hr = call_TexLockRect( dxt, 6, &lr, NULL, 0 );
                if (SUCCEEDED(hr) && lr.pBits)
                {
                    pitch6 = (UINT)lr.Pitch;
                    for (i = 0; i < 8; i++) ((BYTE *)lr.pBits)[i] = (BYTE)(0xA0 + i);
                    call_TexUnlockRect( dxt, 6 );
                    hr = call_TexLockRect( dxt, 6, &lr, NULL, D3DLOCK_READONLY );
                    if (SUCCEEDED(hr) && lr.pBits)
                    {
                        for (i = 0; i < 8; i++)
                            if (((const BYTE *)lr.pBits)[i] != (BYTE)(0xA0 + i)) bad++;
                        call_TexUnlockRect( dxt, 6 );
                    }
                    else bad++;
                }
                else bad++;
            }
            out( "L0pitch=" ); out_dec( pitch0 );
            out( " L6pitch=" ); out_dec( pitch6 );
            out( " bad=" ); out_dec( bad );
            verdict( SUCCEEDED(hr) && pitch0 == 128 && pitch6 == 8 && !bad,
                     "DXT1 block pitch or the one-block mip did not round trip" );
        }
        else
        {
            out_hr( "hr", hr );
            verdict( FALSE, "no DXT1 texture" );
        }

        /* ---- the sub-rect flush must not spill ---------------------------
         *
         * Fill a 32x32 A8R8G8B8 level with 0x11, then lock ONLY (8,8)-(24,24)
         * and write 0x22 there.  A bounce that flushed its whole span would
         * write its own uninitialised bytes over the 0x11 texels outside the
         * rect; a bounce with the offset arithmetic wrong would land the
         * 0x22 block somewhere else.  Both show up here as a nonzero count,
         * and the two counts are printed separately so a failure says WHICH. */
        begin( "LockRect(sub-rect 8,8-24,24) on 32x32 A8R8G8B8: the flush must "
               "leave every texel outside the rect alone" );
        bad = 0;
        {
            UINT inside_bad = 0, outside_bad = 0;
            RECT sub;

            sub.left = 8; sub.top = 8; sub.right = 24; sub.bottom = 24;
            hr = call_CreateTexture( device, 32, 32, 1, D3DFMT_A8R8G8B8,
                                     D3DPOOL_SYSTEMMEM, &argb );
            if (SUCCEEDED(hr) && argb)
            {
                hr = call_TexLockRect( argb, 0, &lr, NULL, 0 );
                if (SUCCEEDED(hr) && lr.pBits)
                {
                    UINT y;
                    for (y = 0; y < 32; y++)
                    {
                        BYTE *row = (BYTE *)lr.pBits + (size_t)y * lr.Pitch;
                        for (i = 0; i < 32 * 4; i++) row[i] = 0x11;
                    }
                    call_TexUnlockRect( argb, 0 );
                }
                else bad++;
            }
            if (SUCCEEDED(hr) && argb && !bad)
            {
                hr = call_TexLockRect( argb, 0, &lr, &sub, 0 );
                if (SUCCEEDED(hr) && lr.pBits)
                {
                    UINT y;
                    for (y = 0; y < 16; y++)
                    {
                        BYTE *row = (BYTE *)lr.pBits + (size_t)y * lr.Pitch;
                        for (i = 0; i < 16 * 4; i++) row[i] = 0x22;
                    }
                    call_TexUnlockRect( argb, 0 );
                }
                else bad++;
            }
            if (SUCCEEDED(hr) && argb && !bad)
            {
                hr = call_TexLockRect( argb, 0, &lr, NULL, D3DLOCK_READONLY );
                if (SUCCEEDED(hr) && lr.pBits)
                {
                    UINT x, y;
                    for (y = 0; y < 32; y++)
                    {
                        const BYTE *row = (const BYTE *)lr.pBits + (size_t)y * lr.Pitch;
                        for (x = 0; x < 32; x++)
                        {
                            BOOL in = (x >= 8 && x < 24 && y >= 8 && y < 24);
                            BYTE want = in ? 0x22 : 0x11;
                            for (i = 0; i < 4; i++)
                                if (row[x * 4 + i] != want) { if (in) inside_bad++; else outside_bad++; }
                        }
                    }
                    call_TexUnlockRect( argb, 0 );
                }
                else bad++;
            }
            out( "inside_bad=" ); out_dec( inside_bad );
            out( " outside_bad=" ); out_dec( outside_bad );
            verdict( SUCCEEDED(hr) && !bad && !inside_bad && !outside_bad,
                     "a sub-rect lock did not land, or its flush spilled outside the rect" );
        }

        /* ---- the buffer Lock: OffsetToLock, and a SizeToLock of 0 --------
         *
         * These two rows were compiled but NEVER EXECUTED before this probe
         * step existed -- nothing in the lane had reached them.  The first
         * lock passes SizeToLock 0, which means "to the end" and is the one
         * case that has to ask the buffer its own length; the second locks a
         * 512-byte window at offset 256, whose flush must touch that window
         * and nothing either side of it. */
        begin( "CreateVertexBuffer(1024, MANAGED) + Lock(0,0) then Lock(256,512): "
               "the offset and the implicit length" );
        bad = 0;
        {
            void *bits = NULL;

            hr = call_CreateVertexBuffer( device, 1024, D3DPOOL_MANAGED, &vb );
            if (SUCCEEDED(hr) && vb)
            {
                hr = call_VBLock( vb, 0, 0, &bits, 0 );
                if (SUCCEEDED(hr) && bits)
                {
                    for (i = 0; i < 1024; i++) ((BYTE *)bits)[i] = (BYTE)(i * 5 + 1);
                    call_VBUnlock( vb );
                }
                else bad++;
            }
            else bad++;
            if (!bad)
            {
                hr = call_VBLock( vb, 256, 512, &bits, 0 );
                if (SUCCEEDED(hr) && bits)
                {
                    for (i = 0; i < 512; i++) ((BYTE *)bits)[i] = 0xAA;
                    call_VBUnlock( vb );
                }
                else bad++;
            }
            if (!bad)
            {
                hr = call_VBLock( vb, 0, 0, &bits, D3DLOCK_READONLY );
                if (SUCCEEDED(hr) && bits)
                {
                    for (i = 0; i < 1024; i++)
                    {
                        BYTE want = (i >= 256 && i < 768) ? 0xAA : (BYTE)(i * 5 + 1);
                        if (((const BYTE *)bits)[i] != want) bad++;
                    }
                    call_VBUnlock( vb );
                }
                else bad++;
            }
            out( "bad=" ); out_dec( bad );
            verdict( SUCCEEDED(hr) && !bad,
                     "a vertex-buffer lock lost its bytes, its offset, or its implicit length" );
        }


        /* ---- the four rows nothing in this lane had ever reached ---------
         *
         * The cube LockRect, both LockBox rows and the index-buffer Lock were
         * COMPILED BUT NEVER EXECUTED: no probe and no title on this lane had
         * called any of them, so "it builds" was the whole of what was known
         * about them.  These steps execute all four.  The cube one also pins
         * the walker's cache key, which is (face, level) and not level alone
         * -- face 3 level 0 must not collide with face 0 level 0. */
        begin( "CreateCubeTexture(32x32 A8R8G8B8) + LockRect(face 3 and face 0, "
               "level 0): write both, read both back" );
        bad = 0;
        {
            UINT pitch = 0, y;

            hr = call_CreateCubeTexture( device, 32, 1, D3DFMT_A8R8G8B8,
                                         D3DPOOL_SYSTEMMEM, &cube );
            if (SUCCEEDED(hr) && cube)
            {
                UINT face;
                for (face = 0; face < 4 && SUCCEEDED(hr); face += 3)
                {
                    hr = call_CubeLockRect( cube, (D3DCUBEMAP_FACES)face, 0, &lr, 0 );
                    if (SUCCEEDED(hr) && lr.pBits)
                    {
                        pitch = (UINT)lr.Pitch;
                        for (y = 0; y < 32; y++)
                        {
                            BYTE *row = (BYTE *)lr.pBits + (size_t)y * lr.Pitch;
                            for (i = 0; i < 32 * 4; i++) row[i] = (BYTE)(face * 16 + y);
                        }
                        call_CubeUnlockRect( cube, (D3DCUBEMAP_FACES)face, 0 );
                    }
                    else bad++;
                }
                for (face = 0; face < 4 && SUCCEEDED(hr); face += 3)
                {
                    hr = call_CubeLockRect( cube, (D3DCUBEMAP_FACES)face, 0, &lr, D3DLOCK_READONLY );
                    if (SUCCEEDED(hr) && lr.pBits)
                    {
                        for (y = 0; y < 32; y++)
                        {
                            const BYTE *row = (const BYTE *)lr.pBits + (size_t)y * lr.Pitch;
                            for (i = 0; i < 32 * 4; i++)
                                if (row[i] != (BYTE)(face * 16 + y)) bad++;
                        }
                        call_CubeUnlockRect( cube, (D3DCUBEMAP_FACES)face, 0 );
                    }
                    else bad++;
                }
            }
            else bad++;
            out( "pitch=" ); out_dec( pitch );
            out( " bad=" ); out_dec( bad );
            verdict( SUCCEEDED(hr) && pitch == 128 && !bad,
                     "a cube face did not round trip, or two faces shared one bounce" );
        }

        /* A volume's SlicePitch is RowPitch x block rows and its total is that
         * times DEPTH -- the one place the bounce has to multiply by a third
         * dimension.  16x16x4 A8R8G8B8: RowPitch 64, SlicePitch 1024, 4096
         * bytes.  Then the SAME storage is re-locked through the standalone
         * IDirect3DVolume9 that GetVolumeLevel hands back, which is a
         * different host object on a different vtable and therefore a
         * different walker and a different bounce. */
        begin( "CreateVolumeTexture(16x16x4 A8R8G8B8) + LockBox, then the same "
               "storage through GetVolumeLevel(0) + IDirect3DVolume9::LockBox" );
        bad = 0;
        {
            D3DLOCKED_BOX lb;
            UINT rp = 0, sp = 0, z;

            hr = call_CreateVolumeTexture( device, 16, 16, 4, 1, D3DFMT_A8R8G8B8,
                                           D3DPOOL_SYSTEMMEM, &voltex );
            if (SUCCEEDED(hr) && voltex)
            {
                hr = call_VolTexLockBox( voltex, 0, &lb, 0 );
                if (SUCCEEDED(hr) && lb.pBits)
                {
                    rp = (UINT)lb.RowPitch;
                    sp = (UINT)lb.SlicePitch;
                    for (z = 0; z < 4; z++)
                    {
                        BYTE *slice = (BYTE *)lb.pBits + (size_t)z * lb.SlicePitch;
                        for (i = 0; i < 16u * 64u; i++) slice[i] = (BYTE)(z * 32 + (i & 31));
                    }
                    call_VolTexUnlockBox( voltex, 0 );
                }
                else bad++;
            }
            else bad++;
            if (!bad && SUCCEEDED(hr))
            {
                hr = call_GetVolumeLevel( voltex, 0, &vol );
                if (SUCCEEDED(hr) && vol)
                {
                    hr = call_VolLockBox( vol, &lb, D3DLOCK_READONLY );
                    if (SUCCEEDED(hr) && lb.pBits)
                    {
                        for (z = 0; z < 4; z++)
                        {
                            const BYTE *slice = (const BYTE *)lb.pBits + (size_t)z * lb.SlicePitch;
                            for (i = 0; i < 16u * 64u; i++)
                                if (slice[i] != (BYTE)(z * 32 + (i & 31))) bad++;
                        }
                        call_VolUnlockBox( vol );
                    }
                    else bad++;
                }
                else bad++;
            }
            out( "RowPitch=" ); out_dec( rp );
            out( " SlicePitch=" ); out_dec( sp );
            out( " bad=" ); out_dec( bad );
            verdict( SUCCEEDED(hr) && rp == 64 && sp == 1024 && !bad,
                     "the volume's depth or its standalone-volume view did not round trip" );
        }

        begin( "CreateIndexBuffer(512, INDEX16, MANAGED) + Lock(0,0): write, read back" );
        bad = 0;
        {
            void *bits = NULL;

            hr = call_CreateIndexBuffer( device, 512, D3DPOOL_MANAGED, &ib );
            if (SUCCEEDED(hr) && ib)
            {
                hr = call_IBLock( ib, 0, 0, &bits, 0 );
                if (SUCCEEDED(hr) && bits)
                {
                    for (i = 0; i < 512; i++) ((BYTE *)bits)[i] = (BYTE)(i ^ 0x5a);
                    call_IBUnlock( ib );
                    hr = call_IBLock( ib, 0, 0, &bits, D3DLOCK_READONLY );
                    if (SUCCEEDED(hr) && bits)
                    {
                        for (i = 0; i < 512; i++)
                            if (((const BYTE *)bits)[i] != (BYTE)(i ^ 0x5a)) bad++;
                        call_IBUnlock( ib );
                    }
                    else bad++;
                }
                else bad++;
            }
            else bad++;
            out( "bad=" ); out_dec( bad );
            verdict( SUCCEEDED(hr) && !bad, "the index buffer lost its bytes" );
        }

        call_Release_IB( ib );
        call_Release_VB( vb );
        call_Release_Volume( vol );
        call_Release_VolTex( voltex );
        call_Release_Cube( cube );
        call_Release_Texture( argb );
        call_Release_Texture( dxt );
    }
#endif /* __i386__ */

    /* ---- step 8: the by-value float, both directions --------------------
     *
     * SetNPatchMode/GetNPatchMode is the only round trip on this surface that
     * carries a float BY VALUE in and BY VALUE out, and DXVK stores and
     * returns it verbatim (d3d9_device.cpp: `m_state.nPatchSegments =
     * nSegments;` and `return m_state.nPatchSegments;`), so anything other
     * than the exact bits back means the boundary lost them.
     *
     * It is worth its own step because the two directions fail differently
     * and both are invisible in a colour: MS-x64 puts the argument in XMM1
     * and returns the result in XMM0, while ELFv2 uses f1 for both -- and the
     * unixlib's widest-INTEGER call form writes neither.  A wrong value here
     * is not a crash; it is a tessellation setting that is quietly ignored.
     * The BITS are printed, not the number, because a probe that printed
     * "3.500000" would hide a low-mantissa difference behind its own
     * formatting -- and because these two legs must be byte-identical. */
#ifdef __i386__
    /* NOT RUN ON THE i386 LEG, and not as a convenience.  GetNPatchMode
     * returns its float in x87 ST(0); the marshalling row for it publishes no
     * i386 frame geometry and fails closed rather than popping a frame it
     * cannot describe.  That refusal cannot pop the guest's stdcall frame
     * either, so on this leg the call does not merely fail this step -- it
     * takes the process down, and every step below it (the depth pair, the
     * release, the PASS/FAIL line) never runs at all.  So the i386 leg says
     * so and walks past.  It is not counted as a step: nothing was proved.
     * The native and x86-64 legs, which are what check-d3d9-smoke.sh diffs,
     * still run it exactly as before. */
    out( "SetNPatchMode/GetNPatchMode: not attempted on the i386 lane -- the "
         "x87 ST(0) return has no i386 frame geometry, and the refusal cannot "
         "pop the guest frame\n" );
#else
    if (device)
    {
        union { float f; DWORD u; } set, got;

        set.u = 0x40600000u;             /* 3.5f, exact in binary32 */
        begin( "SetNPatchMode(0x40600000) + GetNPatchMode (XMM1 in, XMM0 out)" );
        hr = call_SetNPatchMode( device, set.f );
        out_hr( "hr", hr );
        got.f = call_GetNPatchMode( device );
        out( " got=0x" ); out_hex( got.u, 8 );
        verdict( SUCCEEDED(hr) && got.u == set.u,
                 "the by-value float did not survive the round trip" );
    }
#endif

    /* =================================================================
     *  steps 9-11: THE DEPTH BUFFER, BY VALUE
     * =================================================================
     *
     * Everything above this line is a COLOUR claim.  A depth buffer is a
     * separate attachment, a separate format, a separate Clear flag and a
     * separate argument to that Clear -- and the float Z is the only
     * argument in this whole probe that reaches DXVK through the
     * floating-point registers rather than the integer ones.  MS-x64 passes
     * it in XMM3 (the fourth argument slot, indexed BY POSITION) and ELFv2
     * in f1 (the first FP register, indexed BY ORDER); that is the exact
     * mismatch README records ldexp's exponent being lost to, and a Z that
     * arrives wrong is not a crash, it is a depth buffer quietly full of the
     * wrong number.  Step 8's SetNPatchMode proves a float survives a
     * scalar round trip; this proves one survives into a GPU resource and
     * back out through a mapped pointer.
     *
     * D3DFMT_D32F_LOCKABLE is what makes reading the depth buffer BACK legal
     * at all.  An ordinary D3DFMT_D24S8 depth surface in D3DPOOL_DEFAULT
     * cannot be locked and cannot be the source of GetRenderTargetData, so
     * the colour path used above has no depth equivalent.  D32F_LOCKABLE
     * exists in D3D9 precisely for this and DXVK implements it as a plain
     * VK_FORMAT_D32_SFLOAT wherever the driver is not Intel
     * (src/d3d9/d3d9_format.cpp: `m_d32flockableSupport = !isIntel;`), which
     * on this gate's AMD adapter is true.  If a future adapter says no,
     * CreateDepthStencilSurface fails and step 9 goes red by name rather
     * than these steps quietly not testing anything.
     */

    /* ---- step 9: a lockable 64x64 D32F depth surface, bound ------------- */
    begin( "CreateDepthStencilSurface(64x64 D32F_LOCKABLE, Discard=FALSE) + SetDepthStencilSurface" );
    if (device)
    {
        hr = call_CreateDepthStencilSurface( device, W, H, D3DFMT_D32F_LOCKABLE, &ds );
        if (SUCCEEDED(hr) && ds) hr = call_SetDepthStencilSurface( device, ds );
    }
    else hr = E_FAIL;
    out_hr( "hr", hr );
    verdict( SUCCEEDED(hr) && ds != NULL,
             "no lockable depth surface (does this adapter support D32F_LOCKABLE?)" );

    /* ---- step 10: Clear(ZBUFFER, 0.25f), read back all 4096 ------------- */
    /* The value is known at COMPILE TIME -- DEPTH_A_BITS above -- so this is
     * the depth equivalent of step 7's per-texel colour walk, and it is
     * checked the same way: every texel, exact bits, and the coverage count
     * asserted alongside the mismatch count so that a short walk cannot pass
     * by checking nothing. */
    begin( "Clear(D3DCLEAR_ZBUFFER, Z=0.25f) + walk all 4096 depth texels" );
    {
        UINT checked = 0, mismatches = 0;
        union { float f; DWORD u; } za;

        za.u = DEPTH_A_BITS;
        if (ds)
        {
            hr = call_ClearZ( device, za.f );
            if (SUCCEEDED(hr))
            {
                D3DLOCKED_RECT lr;
                hr = call_LockRect( ds, &lr, D3DLOCK_READONLY );
                if (SUCCEEDED(hr) && lr.pBits)
                {
                    UINT x, y;
                    for (y = 0; y < H; y++)
                    {
                        const BYTE *row = (const BYTE *)lr.pBits + (size_t)y * lr.Pitch;
                        for (x = 0; x < W; x++)
                        {
                            DWORD got;
                            __builtin_memcpy( &got, row + x * 4, 4 );
                            checked++;
#if SMOKE_BREAK == 5
                            if (got != 0x3F000000u) mismatches++;   /* expect 0.5f, which was never written */
#else
                            if (got != DEPTH_A_BITS) mismatches++;
#endif
                        }
                    }
                    call_UnlockRect( ds );
                }
            }
        }
        else hr = E_FAIL;
        out_hr( "hr", hr );
        out( " checked=" ); out_dec( checked );
        out( " mismatches=" ); out_dec( mismatches );
        verdict( SUCCEEDED(hr) && checked == W * H && mismatches == 0,
                 "the depth buffer does not read back as the 0.25f it was cleared to" );
    }

    /* ---- step 11: Clear(ZBUFFER, 0.75f), read back all 4096 ------------- */
    /* The second value, and the reason there are two.  A buffer that is
     * never actually written -- or a Z argument that never arrives and
     * leaves the driver clearing to some fixed default -- can match ONE
     * expected number by luck or by construction.  It cannot match two
     * different ones.  This step is also where SMOKE_BREAK=4 bites: it skips
     * this Clear, so the surface still holds step 10's 0.25f and the check
     * against 0.75f goes red on all 4096 texels. */
    begin( "Clear(D3DCLEAR_ZBUFFER, Z=0.75f) + walk all 4096 depth texels again" );
    {
        UINT checked = 0, mismatches = 0;
        union { float f; DWORD u; } zb;

        zb.u = DEPTH_B_BITS;
        if (ds)
        {
#if SMOKE_BREAK == 4
            out( "clear skipped (SMOKE_BREAK=4) " );
            hr = D3D_OK;
#else
            hr = call_ClearZ( device, zb.f );
#endif
            if (SUCCEEDED(hr))
            {
                D3DLOCKED_RECT lr;
                hr = call_LockRect( ds, &lr, D3DLOCK_READONLY );
                if (SUCCEEDED(hr) && lr.pBits)
                {
                    UINT x, y;
                    for (y = 0; y < H; y++)
                    {
                        const BYTE *row = (const BYTE *)lr.pBits + (size_t)y * lr.Pitch;
                        for (x = 0; x < W; x++)
                        {
                            DWORD got;
                            __builtin_memcpy( &got, row + x * 4, 4 );
                            checked++;
                            if (got != DEPTH_B_BITS) mismatches++;
                        }
                    }
                    call_UnlockRect( ds );
                }
            }
        }
        else hr = E_FAIL;
        out_hr( "hr", hr );
        out( " checked=" ); out_dec( checked );
        out( " mismatches=" ); out_dec( mismatches );
        verdict( SUCCEEDED(hr) && checked == W * H && mismatches == 0,
                 "the depth buffer does not read back as the 0.75f it was cleared to" );
    }

done:
    /* ---- step 12: release everything in reverse order ------------------- */
    begin( "release everything (reverse order)" );
    call_Release_Surface( ds );
    call_Release_Surface( sysmem );
    call_Release_Surface( rt );
    call_Release_Device( device );
    call_Release_D3D9( d3d9 );
    out( "released" );
    verdict( TRUE, "" );

    out( failures ? "d3d9_smoke: FAIL " : "d3d9_smoke: PASS " );
    out_dec( (ULONG)(step - failures) );
    out( "/" );
    out_dec( (ULONG)step );
    if (failures && first_fail)
    {
        out( " (" );
        out( first_fail );
        out( ")" );
    }
    out( "\n" );
    return failures ? 1 : 0;
}

#if defined(D3D9_SMOKE_NATIVE)
int main( int argc, char **argv )
{
    if (!native_resolve( argc, argv ))
    {
        out( "d3d9_smoke: FAIL could not resolve the DXVK entry points via dlopen/dlsym\n" );
        return 1;
    }
    return d3d9_smoke_run();
}
#else
/* The guest build has no C runtime: this IS the image entry point. */
void WINAPI d3d9_smoke_entry( void )
{
    ExitProcess( (UINT)d3d9_smoke_run() );
}
#endif
