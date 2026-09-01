/*
 * d3d11_smoke -- the native-vs-guest D3D11 RUNTIME gate.
 *
 * ONE source, built TWICE and run TWICE: once as a native ppc64le ELF
 * binary that dlopens DXVK's own libdxvk_d3d11.so / libdxvk_dxgi.so
 * directly (no Wine, no emulation anywhere in the process -- the same
 * dynamic-attachment discipline dxvk-ppc64le/probes/native_d3d11_smoke.cpp
 * exists to prove, see the design note there), and once as an ordinary
 * x86-64 Windows PE that imports d3d11.dll and is run as a GUEST under
 * this port's wine.  The two runs must print byte-identical stdout.
 *
 * THAT is the claim, not "both sides say PASS".  check-com-smoke.sh
 * established the standard for the COM boundary: a guest that reached the
 * right ANSWER through the wrong MECHANISM is exactly the failure mode a
 * mere PASS/PASS comparison cannot see, and dlls/d3d12/main.c's own guest
 * surface is held to the same texel-exact bar downstream of it.  Every
 * byte this program prints -- a feature level, a checksum, a per-texel
 * mismatch count -- is a value the real GPU pipeline computed, by RADV, off
 * Radeon silicon, on both sides: natively through DXVK's own Vulkan
 * backend, and through it a second time reached across whatever plumbing
 * carries a guest ID3D11Device call to that same DXVK.  Identical bytes
 * means nothing was lost, swapped, or silently defaulted crossing that
 * plumbing -- not "a device could be created", which a stub could also
 * report.
 *
 * WHAT THIS DOES NOT COVER.  No swapchain, no presentation, no window, no
 * shader.  It proves memory movement through a real render target (create,
 * clear, copy, map, read back) and nothing about DXBC, dxbc-spv, or a
 * pipeline that actually executes an instruction.  The shader-draw case,
 * with a full-screen triangle rendered by a genuine SM5 DXBC vertex+pixel
 * shader pair and every texel recomputed on the CPU, is the native probe's
 * job: dxvk-ppc64le/probes/native_d3d11_smoke.cpp, stage 5.  That probe has
 * no guest leg at all (it dlopens DXVK directly, nothing else); this one
 * exists to prove the boundary a bare "it renders" cannot, and stops at the
 * point where a shader would need its own COM-thunked pipeline-state
 * surface this file has no opinion about.
 *
 * THE CLEAR COLOUR, AND ITS ROUNDING, VERIFIED RATHER THAN ASSUMED.  The
 * fixed colour is R=0.0f G=0.25f B=0.5f A=1.0f.  For an 8-bit UNORM channel
 * the D3D11 conversion is round(v * 255) (round-to-nearest, ties enumerated
 * below because two of these four values are NOT exactly representable as
 * eighths of 255):
 *
 *     R  0.00f * 255 =   0.00   -> round  0 = 0x00   (exact)
 *     G  0.25f * 255 =  63.75   -> round 64 = 0x40   (63.75 is closer to 64
 *                                                      than to 63 under ANY
 *                                                      nearest-rounding rule)
 *     B  0.50f * 255 = 127.50   -> round128 = 0x80   (an exact tie; D3D's
 *                                                      round-nearest-even and
 *                                                      the common round-half-
 *                                                      away-from-zero rule
 *                                                      AGREE here, because 128
 *                                                      is the even neighbour)
 *     A  1.00f * 255 = 255.00   -> round255 = 0xff   (exact)
 *
 * So the expected bytes, in DXGI_FORMAT_R8G8B8A8_UNORM byte order, are
 * 00 40 80 ff for every one of the 4096 texels.  That is what step 6 checks
 * and what the checksum in step 6b is a checksum OF.
 *
 * SMOKE_BREAK (falsification -- proves the checks can actually go red;
 * mirrors native_d3d11_smoke.cpp's own SMOKE_BREAK block):
 *
 *   -DSMOKE_BREAK=1   skip the ClearRenderTargetView call entirely.  The
 *                      render target keeps whatever the driver left in
 *                      freshly allocated GPU memory, which the checked
 *                      colour has no reason to match, so step 6 goes red.
 *   -DSMOKE_BREAK=2   swap R and B in the CPU's own expectation used by the
 *                      per-texel check (the actual rendered bytes are still
 *                      correct; the CHECK is deliberately wrong), so every
 *                      one of the 4096 comparisons mismatches.
 *   -DSMOKE_BREAK=3   restrict the per-texel walk to texel (0,0) only.
 *                      Coverage is part of the claim step 6 makes -- "every
 *                      one of the 4096 texels" -- so step 6's own verdict
 *                      requires checked_count == 4096 as well as
 *                      mismatches == 0; an incomplete scan fails on that
 *                      arithmetic alone, honestly, rather than by hoping the
 *                      one texel it still looked at happens to be wrong.
 *
 * D3D11_SMOKE_REFUSAL (a second, separate build, used only by
 * check-d3d11-smoke.sh's negative-control leg F): adds one more step after
 * the checksum that creates a second device and calls
 * ID3D11Device::OpenSharedResource with a fabricated HANDLE.  The guest call
 * is expected to be REFUSED rather than silently mishandled, and this build
 * exists to give the gate something to grep for.  It changes the printed
 * transcript, which is exactly why it is a separate build from the one used
 * for the byte-identical comparison (legs C/D/E): the refusal step must never
 * appear in the transcript that native and guest are diffed against.
 *
 * WHY OpenSharedResource AND NOT MakeWindowAssociation, WHICH THIS USED TO
 * CALL.  MakeWindowAssociation was picked as the textbook example of a call
 * with an HWND argument that the boundary could not make sense of -- and it
 * has since stopped being one.  This lane presents through win32u's
 * client-surface layer now, an HWND is a value both sides agree about, and
 * every window-handle slot on this surface marshals.  A negative control that
 * quietly starts passing is worse than no negative control, so it was moved
 * to a refusal that is structural rather than provisional: a by-value HANDLE
 * is a WINE object on one side and DXVK's own tagged eventfd encoding on the
 * other, two namespaces that genuinely collide, and no amount of presentation
 * work will ever make that integer mean the same thing twice.  Which slots
 * refuse and what they print is this port's libs/winecom's call, not this
 * probe's, so the gate asserts the wording loosely.
 *
 * NO C RUNTIME on the guest side (-DD3D11_SMOKE_NO_CRT, mirroring
 * com_smoke.c's -DCOM_SMOKE_NO_CRT exactly and for the same reason): the
 * program formats its own output and writes it with WriteFile.  A CRT would
 * add a second variable to a probe whose whole value is that only the
 * D3D11/DXGI boundary is under test, and neither libc's nor ucrt's printf
 * is the thing being measured.  The native leg links an ordinary libc (it
 * is a plain Linux ELF program) but calls the SAME hand-written formatting
 * routines below rather than printf, so that formatting itself can never be
 * the source of a byte mismatch between the two legs.
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

#if defined(D3D11_SMOKE_NATIVE)

/* ---- the native ppc64le ELF leg -----------------------------------------
 * An ordinary Linux program.  DXVK's D3D11CreateDevice and (REFUSAL build
 * only) CreateDXGIFactory1 are resolved with dlopen/dlsym exactly the way
 * a thunk host resolves them and exactly the way
 * dxvk-ppc64le/probes/native_d3d11_smoke.cpp does -- nine flat entry points
 * is DXVK's entire non-vtable surface, and this is two of them. */
#include <windows.h>
#include <d3d11.h>
#include <dxgi1_2.h>
#include <dlfcn.h>
#include <unistd.h>

typedef HRESULT (WINAPI *PFN_D3D11CreateDevice)(
    IDXGIAdapter *, D3D_DRIVER_TYPE, HMODULE, UINT,
    const D3D_FEATURE_LEVEL *, UINT, UINT,
    ID3D11Device **, D3D_FEATURE_LEVEL *, ID3D11DeviceContext **);

static PFN_D3D11CreateDevice p_D3D11CreateDevice;

#define D3D11CreateDevice_CALL   p_D3D11CreateDevice

/* argv[1] = path to libdxvk_d3d11.so.  Passed on the command line rather
 * than baked in, the same way a thunk host would be handed it at run time.
 * The REFUSAL build needs no second library: its one extra call is a method
 * on a device this same entry point creates. */
static int native_resolve( int argc, char **argv )
{
    void *d3d11_lib;

    if (argc < 2)
    {
        static const char usage[] =
            "usage: d3d11_smoke <libdxvk_d3d11.so>\n";
        write( 2, usage, sizeof(usage) - 1 );
        return 0;
    }
    if (!(d3d11_lib = dlopen( argv[1], RTLD_NOW ))) return 0;
    p_D3D11CreateDevice = (PFN_D3D11CreateDevice) dlsym( d3d11_lib, "D3D11CreateDevice" );
    if (!p_D3D11CreateDevice) return 0;
    return 1;
}

static void out( const char *s )
{
    size_t n = 0;
    while (s[n]) n++;
    write( 1, s, n );
}

#else /* !D3D11_SMOKE_NATIVE */

/* ---- the guest x86-64 PE leg --------------------------------------------
 * Imports d3d11.dll (and, in the REFUSAL build, dxgi.dll) the way a real
 * application does; run under this port's wine.  No CRT -- see the header
 * comment above. */
#include <windows.h>
#include <d3d11.h>
#include <dxgi1_2.h>

#define D3D11CreateDevice_CALL   D3D11CreateDevice

static void out( const char *s )
{
    DWORD n = 0, written;
    while (s[n]) n++;
    WriteFile( GetStdHandle( STD_OUTPUT_HANDLE ), s, n, &written, NULL );
}

#endif /* D3D11_SMOKE_NATIVE */

/* ---------------------------------------------------- everything below this
 * line is identical between the two legs. ---------------------------------- */

#ifdef D3D11_SMOKE_REFUSAL
/* Spelled out here rather than linked from libuuid, for the same reason
 * com_smoke.c spells out its CLSIDs: the guest build has no Wine import
 * libraries at all, and a GUID both builds compile from the same bytes
 * cannot differ between them.  Verified against include/dxgi.idl. */
static const GUID smoke_IID_ID3D11Texture2D =
    { 0x6f15aaf2, 0xd208, 0x4e89, { 0x9a,0xb4,0x48,0x95,0x35,0xd3,0x4f,0x9c } };
#endif

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

/* 32-bit FNV-1a over the bytes handed to it. */
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

static int d3d11_smoke_run( void )
{
    ID3D11Device *device = NULL;
    ID3D11DeviceContext *context = NULL;
    ID3D11Texture2D *rtt = NULL, *staging = NULL;
    ID3D11RenderTargetView *rtv = NULL;
    D3D_FEATURE_LEVEL got_fl = (D3D_FEATURE_LEVEL)0;
    const D3D_FEATURE_LEVEL want_fl[] = { D3D_FEATURE_LEVEL_11_0 };
    const UINT W = 64, H = 64;
    HRESULT hr;

    out( "d3d11_smoke: start\n" );

    /* ---- step 1: D3D11CreateDevice -------------------------------------- */
    begin( "D3D11CreateDevice(HARDWARE, feature level 11_0)" );
    hr = D3D11CreateDevice_CALL( NULL, D3D_DRIVER_TYPE_HARDWARE, NULL, 0,
                                  want_fl, 1, D3D11_SDK_VERSION,
                                  &device, &got_fl, &context );
    out_hr( "hr", hr );
    out( " fl=0x" );
    out_hex( (ULONG)got_fl, 4 );
    verdict( SUCCEEDED(hr) && device && context && got_fl == D3D_FEATURE_LEVEL_11_0,
             "device/context missing or feature level is not exactly 11_0" );
    if (!device || !context) goto done;

    /* ---- step 2: a 64x64 R8G8B8A8_UNORM DEFAULT render-target texture --- */
    begin( "CreateTexture2D(64x64 R8G8B8A8_UNORM DEFAULT BIND_RENDER_TARGET)" );
    {
        D3D11_TEXTURE2D_DESC d;
        d.Width = W; d.Height = H; d.MipLevels = 1; d.ArraySize = 1;
        d.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        d.SampleDesc.Count = 1; d.SampleDesc.Quality = 0;
        d.Usage = D3D11_USAGE_DEFAULT;
        d.BindFlags = D3D11_BIND_RENDER_TARGET;
        d.CPUAccessFlags = 0;
        d.MiscFlags = 0;
        hr = ID3D11Device_CreateTexture2D( device, &d, NULL, &rtt );
    }
    out_hr( "hr", hr );
    verdict( SUCCEEDED(hr) && rtt != NULL, "no texture" );

    /* ---- step 3: CreateRenderTargetView ---------------------------------- */
    begin( "CreateRenderTargetView" );
    if (rtt) hr = ID3D11Device_CreateRenderTargetView( device, (ID3D11Resource *)rtt, NULL, &rtv );
    else hr = E_FAIL;
    out_hr( "hr", hr );
    verdict( SUCCEEDED(hr) && rtv != NULL, "no view" );

    /* ---- step 4: ClearRenderTargetView({0.0,0.25,0.5,1.0}) --------------- */
    begin( "ClearRenderTargetView(R=0.00 G=0.25 B=0.50 A=1.00 -> 00,40,80,ff)" );
    if (rtv)
    {
#if SMOKE_BREAK == 1
        out( "skipped (SMOKE_BREAK=1)" );
#else
        const FLOAT clear[4] = { 0.0f, 0.25f, 0.5f, 1.0f };
        ID3D11DeviceContext_ClearRenderTargetView( context, rtv, clear );
        out( "cleared" );
#endif
    }
    else out( "no view to clear" );
    verdict( rtv != NULL, "no view to clear" );

    /* ---- step 5: a matching STAGING/CPU_ACCESS_READ texture, CopyResource */
    begin( "CreateTexture2D(staging CPU_ACCESS_READ) + CopyResource" );
    if (rtt)
    {
        D3D11_TEXTURE2D_DESC d;
        d.Width = W; d.Height = H; d.MipLevels = 1; d.ArraySize = 1;
        d.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        d.SampleDesc.Count = 1; d.SampleDesc.Quality = 0;
        d.Usage = D3D11_USAGE_STAGING;
        d.BindFlags = 0;
        d.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        d.MiscFlags = 0;
        hr = ID3D11Device_CreateTexture2D( device, &d, NULL, &staging );
        if (SUCCEEDED(hr) && staging)
            ID3D11DeviceContext_CopyResource( context, (ID3D11Resource *)staging, (ID3D11Resource *)rtt );
    }
    else hr = E_FAIL;
    out_hr( "hr", hr );
    verdict( SUCCEEDED(hr) && staging != NULL, "no staging texture" );

    /* ---- step 6: Map(READ), walk all 4096 texels, Unmap ------------------ */
    begin( "Map(READ) + walk all 4096 texels against the expected clear colour" );
    {
        UINT checked = 0, mismatches = 0;
        DWORD csum = 0x811c9dc5u;   /* FNV-1a offset basis, for step 6b */

        if (staging)
        {
            D3D11_MAPPED_SUBRESOURCE m;
            hr = ID3D11DeviceContext_Map( context, (ID3D11Resource *)staging, 0,
                                           D3D11_MAP_READ, 0, &m );
            if (SUCCEEDED(hr) && m.pData)
            {
                UINT x, y;
                for (y = 0; y < H; y++)
                {
                    const BYTE *row = (const BYTE *)m.pData + (size_t)y * m.RowPitch;
                    for (x = 0; x < W; x++)
                    {
                        const BYTE *texel = row + x * 4;
                        BYTE er = 0x00, eg = 0x40, eb = 0x80, ea = 0xff;

                        csum = fnv1a( csum, texel, 4 );

#if SMOKE_BREAK == 3
                        if (x != 0 || y != 0) continue;
#endif
                        checked++;
#if SMOKE_BREAK == 2
                        { BYTE t = er; er = eb; eb = t; }   /* swap R and B in the CHECK */
#endif
                        if (texel[0] != er || texel[1] != eg ||
                            texel[2] != eb || texel[3] != ea)
                            mismatches++;
                    }
                }
                ID3D11DeviceContext_Unmap( context, (ID3D11Resource *)staging, 0 );
            }
        }
        else hr = E_FAIL;

        out( "checked=" ); out_dec( checked );
        out( " mismatches=" ); out_dec( mismatches );
        verdict( SUCCEEDED(hr) && checked == W * H && mismatches == 0,
                 "did not confirm every one of the 4096 texels" );

        /* ---- step 6b (unnamed line, not a numbered step): the checksum --
         * Always over the FULL 4096 texels actually read back, independent
         * of SMOKE_BREAK=3's reduced coverage above: this line is a raw,
         * unconditional accounting, not a demonstration of falsifiability. */
        out( "checksum: fnv1a=0x" ); out_hex( csum, 8 );
        out( " texels=" ); out_dec( W * H );
        out( " mismatches=" ); out_dec( mismatches );
        out( "\n" );
    }

done:
#ifndef D3D11_SMOKE_NO_GETSHADER
    /* ---- step 7: the GetShader family (CA_IFACE_ARR_OUT_COUNTPTR) --------
     * COMPILED OUT of check-d3d11-smoke32.sh's pair (-DD3D11_SMOKE_NO_GETSHADER
     * on BOTH its native reference and its i386 guest): the 32-bit lane
     * refuses this family by design (refuse32 -- no 4-byte staging yet), and
     * the refusal HYGIENE there scrubs cells that DXVK's served semantics
     * leave untouched, so a byte-identical-to-native transcript is exactly
     * what a correct 32-bit lane cannot produce for this step.  The 64-bit
     * gate asserts the served semantics; the refusal gate (leg F) asserts
     * the hygiene.
     * -------------------------------------------------------------------
     * The Witcher 3 crash class, asserted from the caller's chair: every
     * out-param must be WRITTEN.  Locals are seeded with a sentinel that
     * looks like the stack residue W3 actually called through; if any
     * survives the call, the class (or the refusal hygiene under it) is not
     * doing its job.  No shader is ever created -- this probe is
     * deliberately DXBC-free -- so the served answers are the NULL-bound
     * state, which is exactly what an unchecked caller reads back and
     * crashes on when nothing writes it.  DXVK's own corner-case contract
     * (GetClassInstances): a NULL count pointer means NOTHING is touched,
     * array included; a live count means exactly CAPACITY cells written
     * (instances then NULL padding) and the actual count stored.  Proxy
     * IDENTITY (same guest proxy back) is asserted through
     * OMGet/SetRenderTargets, which shares the winecom_wrap interning the
     * countptr class uses -- a real shader would be needed to assert it
     * through PSGetShader itself, and this probe has none by design. */
    if (device && context && rtv)
    {
#define SMOKE_SENTINEL ((void *)(UINT_PTR)0x5AB07A6E5AB07A6EULL)
        ID3D11PixelShader *ps = (ID3D11PixelShader *)SMOKE_SENTINEL;
        ID3D11ClassInstance *inst[4];
        ID3D11ClassInstance *inst_nc[2];
        ID3D11RenderTargetView *got_rtv = (ID3D11RenderTargetView *)SMOKE_SENTINEL;
        UINT ninst = 4, ninst_null = 0xDEADBEEF;
        UINT i, pad_nulls = 0, nc_survived = 0;
        BOOL ok;

        begin( "PSGetShader family: every out-param written, DXVK's corner cases exact" );
        for (i = 0; i < 4; i++) inst[i] = (ID3D11ClassInstance *)SMOKE_SENTINEL;
        for (i = 0; i < 2; i++) inst_nc[i] = (ID3D11ClassInstance *)SMOKE_SENTINEL;

        /* the full shape: shader out + capacity-4 array + live count */
        ID3D11DeviceContext_PSGetShader( context, &ps, inst, &ninst );
        for (i = 0; i < 4; i++) pad_nulls += (inst[i] == NULL);
        out( "ps_written=" ); out( ps == (ID3D11PixelShader *)SMOKE_SENTINEL ? "no" : "yes" );
        out( " ps_null=" ); out( ps == NULL ? "yes" : "no" );
        out( " count=" ); out_dec( ninst );
        out( " padded_nulls=" ); out_dec( pad_nulls );

        /* NULL array + live count: count still written */
        ID3D11DeviceContext_PSGetShader( context, &ps, NULL, &ninst_null );
        out( " nullarr_count=" ); out_dec( ninst_null );

        /* NULL count: DXVK touches NOTHING -- the sentinels must SURVIVE */
        ID3D11DeviceContext_PSGetShader( context, &ps, inst_nc, NULL );
        for (i = 0; i < 2; i++)
            nc_survived += (inst_nc[i] == (ID3D11ClassInstance *)SMOKE_SENTINEL);
        out( " nullcount_untouched=" ); out_dec( nc_survived );

        /* proxy identity through the shared wrap/intern machinery */
        ID3D11DeviceContext_OMSetRenderTargets( context, 1, &rtv, NULL );
        ID3D11DeviceContext_OMGetRenderTargets( context, 1, &got_rtv, NULL );
        out( " omget_identity=" ); out( got_rtv == rtv ? "yes" : "no" );
        if (got_rtv && got_rtv != (ID3D11RenderTargetView *)SMOKE_SENTINEL)
            ID3D11RenderTargetView_Release( got_rtv );

        ok = ps == NULL && ninst == 0 && pad_nulls == 4 &&
             ninst_null == 0 && nc_survived == 2 && got_rtv == rtv;
        verdict( ok, "a GetShader out-param kept its sentinel or a corner case diverged" );
#undef SMOKE_SENTINEL
    }
#endif /* D3D11_SMOKE_NO_GETSHADER */

    /* ---- step 8 (7 in the smoke32 pair): release everything -------------- */
    begin( "release everything (reverse order)" );
    if (staging) ID3D11Texture2D_Release( staging );
    if (rtv) ID3D11RenderTargetView_Release( rtv );
    if (rtt) ID3D11Texture2D_Release( rtt );
    if (context) ID3D11DeviceContext_Release( context );
    if (device) ID3D11Device_Release( device );
    out( "released" );
    verdict( TRUE, "" );

#ifdef D3D11_SMOKE_REFUSAL
    /* ---- the negative-control step: only in this build, and only after
     * everything above, so the byte-identical comparison (legs C/D/E of
     * check-d3d11-smoke.sh) never sees this build at all. */
    begin( "ID3D11Device::OpenSharedResource with a fabricated HANDLE (expected: refused)" );
    {
        ID3D11Device *rdev = NULL;
        ID3D11DeviceContext *rctx = NULL;
        HRESULT dhr = D3D11CreateDevice_CALL( NULL, D3D_DRIVER_TYPE_HARDWARE, NULL, 0,
                                              want_fl, 1, D3D11_SDK_VERSION,
                                              &rdev, NULL, &rctx );
        out_hr( "hr", dhr );
        if (SUCCEEDED(dhr) && rdev)
        {
            /* A fabricated, deliberately bogus HANDLE.  Nothing in this probe
             * ever shared a resource, and that is the point: the call must be
             * refused before the integer could matter, because a Wine HANDLE
             * and DXVK's tagged-eventfd encoding are two namespaces over one
             * integer. */
            /* Seeded with the sentinel, not NULL: refusal hygiene says a
             * refused slot must WRITE its out-params (NULL here) before
             * answering E_NOTIMPL -- the Witcher 3 GetShader lesson.  The
             * gate's leg F asserts osr_scrubbed=yes, and its sabotage
             * (WINEEMUNOREFUSESCRUB=1) requires the sentinel to SURVIVE,
             * which proves the scrub was ever load-bearing at all. */
            void *res = (void *)(UINT_PTR)0x5AB07A6E5AB07A6EULL;
            HRESULT ohr = ID3D11Device_OpenSharedResource(
                rdev, (HANDLE)(UINT_PTR)0x1, &smoke_IID_ID3D11Texture2D, &res );
            out( " osr_hr=0x" ); out_hex( (ULONG)ohr, 8 );
            out( " osr_scrubbed=" );
            out( res == NULL ? "yes" : res == (void *)(UINT_PTR)0x5AB07A6E5AB07A6EULL ? "no" : "GARBAGE" );
            if (rctx) ID3D11DeviceContext_Release( rctx );
            ID3D11Device_Release( rdev );
        }
    }
    /* No pass/fail assertion here on purpose: what check-d3d11-smoke.sh's
     * leg F actually requires is the PORT's own +winecom trace naming the
     * refused method on stderr, and that this process reaches ITS OWN next
     * line rather than crashing -- both checked from outside this file. */
    out( " reached\n" );
#endif

    out( failures ? "d3d11_smoke: FAIL " : "d3d11_smoke: PASS " );
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

#if defined(D3D11_SMOKE_NATIVE)
int main( int argc, char **argv )
{
    if (!native_resolve( argc, argv ))
    {
        out( "d3d11_smoke: FAIL could not resolve the DXVK entry points via dlopen/dlsym\n" );
        return 1;
    }
    return d3d11_smoke_run();
}
#else
/* The guest build has no C runtime: this IS the image entry point. */
void WINAPI d3d11_smoke_entry( void )
{
    ExitProcess( (UINT)d3d11_smoke_run() );
}
#endif
