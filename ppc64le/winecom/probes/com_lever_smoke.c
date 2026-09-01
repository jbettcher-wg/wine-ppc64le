/*
 * com_lever_smoke.c -- the guest probe behind ppc64le/winecom/check-com-levers.sh.
 *
 * WHAT IT IS FOR.  The completeness landings turned hundreds of refused COM
 * rows into served ones with no way to put any of them back, and bisecting the
 * Witcher 3 load regression that followed cost seven seat runs (see
 * ppc64le/docs/sessions/2026-09-01/w3-load-regression-bisect.md).  Three
 * runtime levers now exist -- WINEEMUNOCOMROWS, WINEEMUNOCOMIIDS,
 * WINEEMUNOCOMWAVE -- and a lever nobody can prove works is worse than no
 * lever, because a bisect leg run under a broken one is recorded as "tested,
 * clean".  This probe is what makes each of them observable FROM THE CALLER'S
 * CHAIR, which is the only chair that matters:
 *
 *   omget_out=   ID3D11DeviceContext::OMGetRenderTargets, called after
 *                OMSetRenderTargets bound a known view, with the out-param
 *                seeded with the residue-shaped sentinel W3 actually called
 *                through.  Exactly three answers are possible and each one
 *                names a different world:
 *                  rtv       the row SERVED  (no lever, or the lever missed)
 *                  null      the row REFUSED and the refusal SCRUBBED the
 *                            out-param -- refused means INERT
 *                  sentinel  the row REFUSED and did NOT scrub, which is only
 *                            correct under WINEEMUNOREFUSESCRUB=1 and is the
 *                            crash class everywhere else
 *                That three-way split is why this row and not PSGetShader:
 *                PSGetShader's SERVED answer on a device with no shader bound
 *                is a NULL shader and a zero count, which is byte-identical to
 *                its scrubbed refusal.  A gate whose two arms print the same
 *                thing proves nothing.
 *
 *   factory_hr=/factory_out=
 *                CreateDXGIFactory1( IID_IDXGIFactory1, &f ) -- a riid-typed
 *                handout, so it goes through winecom_wrap_out_iface, which is
 *                the one choke point WINEEMUNOCOMIIDS acts on.  A blocked IID
 *                must come back E_NOINTERFACE with the out pointer NULLED and
 *                the host object released: the release-and-NULL the guest got
 *                for months before the syscom wave rostered {77aa99a0}.
 *
 * The probe asserts NOTHING itself and always exits 0 after printing.  What is
 * correct depends entirely on which levers the runner set, and the runner is
 * check-com-levers.sh; a probe that decided for itself would have to know the
 * environment, and then the gate would be testing the probe.
 *
 * GUEST-ONLY.  There is no native twin to byte-compare against: every lever
 * here lives on the guest dispatch path and a native ELF run has no dispatcher
 * at all.  No CRT either, the same rule as ppc64le/dxvk/probes/d3d11_smoke.c
 * and for the same reason -- the program formats its own output and writes it
 * with WriteFile, so nothing but the COM boundary is under test.
 *
 * Copyright 2026 the ppc64le port authors
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 */

#define COBJMACROS

#include <windows.h>
#include <d3d11.h>
#include <dxgi1_2.h>

/* Spelled out rather than linked from libuuid: the guest build has no Wine
 * import libraries at all.  Verified against include/dxgi.idl. */
static const GUID lever_IID_IDXGIFactory1 =
    { 0x770aae78, 0xf26f, 0x4dba, { 0xa8,0x29,0x25,0x3c,0x83,0xd1,0xb3,0x87 } };

/* the residue-shaped sentinel from the Witcher 3 lesson, verbatim */
#define LEVER_SENTINEL ((void *)(UINT_PTR)0x5AB07A6E5AB07A6EULL)

static void out( const char *s )
{
    DWORD n = 0, written;

    while (s[n]) n++;
    WriteFile( GetStdHandle( STD_OUTPUT_HANDLE ), s, n, &written, NULL );
}

static void out_hex8( ULONG v )
{
    static const char digits[] = "0123456789abcdef";
    char buf[9];
    int i;

    for (i = 7; i >= 0; i--) { buf[i] = digits[v & 0xf]; v >>= 4; }
    buf[8] = 0;
    out( buf );
}

static void ptr_verdict( const void *got, const void *expected )
{
    out( got == expected ? "rtv" :
         got == NULL ? "null" :
         got == LEVER_SENTINEL ? "sentinel" : "other" );
}

/* ---- the IID lever's arm ------------------------------------------------- */
static void factory_leg( void )
{
    IDXGIFactory1 *factory = (IDXGIFactory1 *)LEVER_SENTINEL;
    HRESULT hr;

    out( "factory_hr=0x" );
    hr = CreateDXGIFactory1( &lever_IID_IDXGIFactory1, (void **)&factory );
    out_hex8( (ULONG)hr );
    out( " factory_out=" );
    out( factory == NULL ? "null" :
         factory == (IDXGIFactory1 *)LEVER_SENTINEL ? "sentinel" : "object" );
    out( "\n" );
    if (SUCCEEDED(hr) && factory && factory != (IDXGIFactory1 *)LEVER_SENTINEL)
        IDXGIFactory1_Release( factory );
}

/* ---- the row lever's arm ------------------------------------------------- */
static void omget_leg( void )
{
    static const D3D_FEATURE_LEVEL want_fl = D3D_FEATURE_LEVEL_11_0;
    D3D11_TEXTURE2D_DESC td;
    ID3D11Device *device = NULL;
    ID3D11DeviceContext *context = NULL;
    ID3D11Texture2D *rtt = NULL;
    ID3D11RenderTargetView *rtv = NULL;
    ID3D11RenderTargetView *got = (ID3D11RenderTargetView *)LEVER_SENTINEL;
    HRESULT hr;

    out( "device_hr=0x" );
    hr = D3D11CreateDevice( NULL, D3D_DRIVER_TYPE_HARDWARE, NULL, 0,
                            &want_fl, 1, D3D11_SDK_VERSION,
                            &device, NULL, &context );
    out_hex8( (ULONG)hr );
    if (FAILED(hr) || !device || !context)
    {
        out( " omget_out=nodevice\n" );
        return;
    }

    td.Width = 16;
    td.Height = 16;
    td.MipLevels = 1;
    td.ArraySize = 1;
    td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    td.SampleDesc.Count = 1;
    td.SampleDesc.Quality = 0;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_RENDER_TARGET;
    td.CPUAccessFlags = 0;
    td.MiscFlags = 0;
    if (SUCCEEDED(ID3D11Device_CreateTexture2D( device, &td, NULL, &rtt )) && rtt)
        ID3D11Device_CreateRenderTargetView( device, (ID3D11Resource *)rtt, NULL, &rtv );

    if (rtv)
    {
        /* bind a view we own, then ask for it back.  The SERVED answer is
         * that same proxy pointer (winecom's interning guarantees identity);
         * a refusal cannot produce it, whatever it does to the cell. */
        ID3D11DeviceContext_OMSetRenderTargets( context, 1, &rtv, NULL );
        ID3D11DeviceContext_OMGetRenderTargets( context, 1, &got, NULL );
    }
    out( " omget_out=" );
    if (!rtv) out( "nortv" );
    else ptr_verdict( got, rtv );
    out( "\n" );

    if (got && got != (ID3D11RenderTargetView *)LEVER_SENTINEL && got != rtv)
        ID3D11RenderTargetView_Release( got );
    else if (got == rtv) ID3D11RenderTargetView_Release( got );
    if (rtv) ID3D11RenderTargetView_Release( rtv );
    if (rtt) ID3D11Texture2D_Release( rtt );
    ID3D11DeviceContext_Release( context );
    ID3D11Device_Release( device );
}

void __stdcall com_lever_entry( void )
{
    out( "com_lever_smoke: start\n" );
    factory_leg();
    omget_leg();
    out( "com_lever_smoke: done\n" );
    ExitProcess( 0 );
}
