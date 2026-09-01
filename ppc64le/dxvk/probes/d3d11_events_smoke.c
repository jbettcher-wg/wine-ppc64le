/*
 * d3d11_events_smoke.c -- the served-HANDLE and windowless-swapchain
 * surface, from the guest's chair.
 *
 * GUEST-ONLY, unlike d3d11_smoke.c's byte-compared pair: everything here
 * either exercises the winecom event relay (which exists only on the guest
 * dispatch path -- a native caller reaches DXVK's raw vtables, where a Wine
 * event is exactly the untranslated integer the tagged encoding refuses) or
 * pins a DXVK behavior the canary-served rows cite.  The transcript is
 * asserted by check-d3d11-smoke.sh leg G against expected values, not
 * against a native twin.
 *
 *   1  fence event end to end: ID3D11Device5::CreateFence +
 *      ID3D11Fence::SetEventOnCompletion(1, <real Wine event>) +
 *      ID3D11DeviceContext4::Signal + Flush, then WaitForSingleObject.
 *      The wait succeeding is the WHOLE relay proven at once: mint (the
 *      tagged eventfd crossed), DXVK's patched SetEvent (the payout), the
 *      pump (payout -> NtSetEvent).  Under WINEEMUNOCOMEVENT=1 the slot
 *      refuses and the wait MUST time out -- leg G's sabotage.
 *   2  a windowless composition swapchain: created, a buffer taken,
 *      released.  DXVK's own dummy path (dxgi.enableDummyCompositionSwapchain,
 *      set by dlls/d3d11/unix.c), no window anywhere.
 *   3  the canaries: rows served because DXVK provably never reads the
 *      hazardous parameter, each pinned to the cited behavior --
 *      GetSharedResourceAdapterLuid and CreateSwapChainForCoreWindow must
 *      answer E_NOTIMPL (dxgi_factory.cpp:418/:269).  If a DXVK update
 *      changes either, this leg goes red and the row goes back for
 *      reclassification -- that is the deal the canary-serve made.
 *   4  the annotation strings (served plainly since
 *      dxvk-patches/0005-wchar-is-16-bit): SetMarker/Begin/EndEvent with a
 *      non-ASCII label, alive afterwards.  Headless annotations are
 *      disabled inside DXVK, so the return is -1 by contract; the probe
 *      proves the crossing and the string read are harmless, not that a
 *      debugger sees the label.
 *
 * No C runtime (same discipline as d3d11_smoke.c and for the same reason).
 *
 * Copyright 2026 the ppc64le port authors
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 */

#define COBJMACROS
#define INITGUID
#include <windows.h>
#include <initguid.h>
#include <d3d11_4.h>
#include <d3d11_1.h>
#include <dxgi1_2.h>

/* ---- output, hand-rolled (no CRT) --------------------------------------- */
static void out( const char *s )
{
    DWORD n = 0, w;
    while (s[n]) n++;
    WriteFile( GetStdHandle( STD_OUTPUT_HANDLE ), s, n, &w, NULL );
}

static void out_hex( ULONG v )
{
    char buf[11] = "0x00000000";
    static const char d[] = "0123456789abcdef";
    int i;
    for (i = 0; i < 8; i++) buf[9 - i] = d[(v >> (4 * i)) & 15];
    out( buf );
}

static void out_dec( ULONG v )
{
    char buf[12];
    int i = 11;
    buf[i] = 0;
    do { buf[--i] = '0' + (v % 10); v /= 10; } while (v);
    out( buf + i );
}

static int failures;

static void verdict( BOOL ok, const char *why )
{
    if (ok) { out( " -- ok\n" ); return; }
    out( " -- FAIL (" ); out( why ); out( ")\n" );
    failures++;
}

static void memzero( void *p, unsigned n )
{
    unsigned char *b = p;
    while (n--) *b++ = 0;
}

void __stdcall d3d11_events_entry( void )
{
    static const D3D_FEATURE_LEVEL lvl = D3D_FEATURE_LEVEL_11_1;
    ID3D11Device *device = NULL;
    ID3D11DeviceContext *context = NULL;
    ID3D11Device5 *device5 = NULL;
    ID3D11DeviceContext4 *context4 = NULL;
    ID3D11Fence *fence = NULL;
    ID3DUserDefinedAnnotation *annot = NULL;
    IDXGIDevice *dxgi_dev = NULL;
    IDXGIAdapter *adapter = NULL;
    IDXGIFactory2 *factory2 = NULL;
    HRESULT hr;

    hr = D3D11CreateDevice( NULL, D3D_DRIVER_TYPE_HARDWARE, NULL, 0, &lvl, 1,
                            D3D11_SDK_VERSION, &device, NULL, &context );
    out( "device hr=" ); out_hex( (ULONG)hr );
    verdict( hr == S_OK && device && context, "no device" );
    if (FAILED(hr)) goto done;

    /* ---- 1: the fence event, end to end --------------------------------- */
    {
        HANDLE ev = CreateEventW( NULL, FALSE, FALSE, NULL );
        DWORD wait = 0xdeadbeef;

        hr = ID3D11Device_QueryInterface( device, &IID_ID3D11Device5, (void **)&device5 );
        if (SUCCEEDED(hr))
            hr = ID3D11DeviceContext_QueryInterface( context, &IID_ID3D11DeviceContext4,
                                                     (void **)&context4 );
        if (SUCCEEDED(hr))
            hr = ID3D11Device5_CreateFence( device5, 0, D3D11_FENCE_FLAG_NONE,
                                            &IID_ID3D11Fence, (void **)&fence );
        out( "fence hr=" ); out_hex( (ULONG)hr );
        if (SUCCEEDED(hr) && ev)
        {
            hr = ID3D11Fence_SetEventOnCompletion( fence, 1, ev );
            out( " seoc_hr=" ); out_hex( (ULONG)hr );
            if (SUCCEEDED(hr))
            {
                ID3D11DeviceContext4_Signal( context4, fence, 1 );
                ID3D11DeviceContext_Flush( context );
                wait = WaitForSingleObject( ev, 10000 );
            }
            out( " signaled=" ); out( wait == WAIT_OBJECT_0 ? "yes" : "no" );
            /* under WINEEMUNOCOMEVENT=1 the slot refuses (E_NOTIMPL) and the
             * wait never happens -- leg G's sabotage reads exactly this pair */
            verdict( (SUCCEEDED(hr) && wait == WAIT_OBJECT_0) ||
                     (hr == E_NOTIMPL && wait == 0xdeadbeef),
                     "the relay neither served nor refused cleanly" );
        }
        else verdict( FALSE, "no fence path (Device5/Context4/CreateFence)" );
        if (ev) CloseHandle( ev );
    }

    /* ---- 2: the windowless composition swapchain ------------------------ */
    {
        DXGI_SWAP_CHAIN_DESC1 desc;
        IDXGISwapChain1 *sc = NULL;
        ID3D11Texture2D *buf = NULL;

        hr = ID3D11Device_QueryInterface( device, &IID_IDXGIDevice, (void **)&dxgi_dev );
        if (SUCCEEDED(hr)) hr = IDXGIDevice_GetAdapter( dxgi_dev, &adapter );
        if (SUCCEEDED(hr)) hr = IDXGIAdapter_GetParent( adapter, &IID_IDXGIFactory2,
                                                        (void **)&factory2 );
        if (FAILED(hr)) { out( "factory hr=" ); out_hex( (ULONG)hr ); verdict( FALSE, "no factory2" ); goto canaries; }

        memzero( &desc, sizeof(desc) );
        desc.Width = 64;
        desc.Height = 64;
        desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        desc.SampleDesc.Count = 1;
        desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        desc.BufferCount = 2;
        desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;

        hr = IDXGIFactory2_CreateSwapChainForComposition( factory2, (IUnknown *)device,
                                                          &desc, NULL, &sc );
        out( "composition hr=" ); out_hex( (ULONG)hr );
        if (SUCCEEDED(hr))
        {
            hr = IDXGISwapChain1_GetBuffer( sc, 0, &IID_ID3D11Texture2D, (void **)&buf );
            out( " buffer hr=" ); out_hex( (ULONG)hr );
            if (buf) ID3D11Texture2D_Release( buf );
            IDXGISwapChain1_Release( sc );
        }
        verdict( SUCCEEDED(hr), "the dummy composition swapchain did not serve" );
    }

canaries:
    /* ---- 3: the canaries ------------------------------------------------- */
    if (factory2)
    {
        LUID luid;
        IDXGISwapChain1 *sc = (IDXGISwapChain1 *)(UINT_PTR)0x5AB07A6E;
        DXGI_SWAP_CHAIN_DESC1 desc;

        hr = IDXGIFactory2_GetSharedResourceAdapterLuid( factory2, (HANDLE)(UINT_PTR)0x1234,
                                                         &luid );
        out( "luid_canary hr=" ); out_hex( (ULONG)hr );
        verdict( hr == E_NOTIMPL, "dxgi_factory.cpp:418 changed -- reclassify the row" );

        memzero( &desc, sizeof(desc) );
        desc.Width = 64; desc.Height = 64;
        desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        desc.SampleDesc.Count = 1;
        desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        desc.BufferCount = 2;
        desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;
        hr = IDXGIFactory2_CreateSwapChainForCoreWindow( factory2, (IUnknown *)device,
                                                         (IUnknown *)device, &desc, NULL, &sc );
        out( "corewindow_canary hr=" ); out_hex( (ULONG)hr );
        verdict( hr == E_NOTIMPL && sc == NULL,
                 "dxgi_factory.cpp:269 changed -- reclassify the row" );
    }

    /* ---- 4: annotation strings over the 16-bit WCHAR -------------------- */
    hr = ID3D11DeviceContext_QueryInterface( context, &IID_ID3DUserDefinedAnnotation,
                                             (void **)&annot );
    out( "annotation hr=" ); out_hex( (ULONG)hr );
    if (SUCCEEDED(hr))
    {
        static const WCHAR label[] = { 0x03c0, '-', 'm', 'a', 'r', 'k', 0 }; /* pi-mark */
        INT depth;

        ID3DUserDefinedAnnotation_SetMarker( annot, label );
        depth = ID3DUserDefinedAnnotation_BeginEvent( annot, label );
        ID3DUserDefinedAnnotation_EndEvent( annot );
        out( " begin=" ); out_dec( (ULONG)depth );
        out( " alive=yes" );
        verdict( TRUE, "" );
        ID3DUserDefinedAnnotation_Release( annot );
    }
    else verdict( FALSE, "no annotation interface" );

done:
    if (factory2) IDXGIFactory2_Release( factory2 );
    if (adapter) IDXGIAdapter_Release( adapter );
    if (dxgi_dev) IDXGIDevice_Release( dxgi_dev );
    if (fence) ID3D11Fence_Release( fence );
    if (context4) ID3D11DeviceContext4_Release( context4 );
    if (device5) ID3D11Device5_Release( device5 );
    if (context) ID3D11DeviceContext_Release( context );
    if (device) ID3D11Device_Release( device );

    out( failures ? "EVENTS-SMOKE FAIL " : "EVENTS-SMOKE PASS " );
    out_dec( (ULONG)failures );
    out( "\n" );
    ExitProcess( failures ? 1 : 0 );
}
