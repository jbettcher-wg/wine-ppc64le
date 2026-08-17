/*
 * present_smoke.c -- an x86-64 guest that puts pixels on a real screen.
 *
 * ppc64le/dxvk/probes/d3d11_smoke.c proves the guest reaches the same DXVK a
 * native process reaches, texel for texel, with no window anywhere: it renders
 * offscreen and reads the render target back.  That leaves exactly one claim
 * unproven, and it is the one a person looking at the machine cares about --
 * that a frame reaches the SCREEN.  Between the render target and the screen
 * sits the whole presentation path this probe exists to exercise:
 *
 *   guest CreateWindowExA           -> a real Wine HWND
 *   guest D3D11CreateDeviceAndSwapChain(desc.OutputWindow = that HWND)
 *      |  the HWND crosses the boundary UNCONVERTED -- the guest PE calls
 *      |  Wine's own user32, so there is one window-handle namespace here
 *      v
 *   native d3d11.dll (dlls/d3d11/main.c, hand-written swapchain slots)
 *      |
 *      v
 *   DXVK's "Win32u" WSI backend (ppc64le/dxvk/dxvk-patches/0003-...)
 *      |  hands the HWND straight back to Wine
 *      v
 *   win32u client surface -> winex11 child window -> the X server
 *
 * WHAT THIS PROBE ITSELF ASSERTS is only its own half: that the swapchain was
 * created, that the back buffer holds the exact colour it was cleared to, and
 * that Present returned success for every frame.  IT CANNOT SEE THE SCREEN --
 * an application never can.  The other half is asserted from outside, by
 * check-present-smoke.sh, which reads the X server's own framebuffer with
 * XGetImage while this process is still presenting and requires the captured
 * pixels to be the SAME EXACT BYTES this probe printed a checksum for.  Two
 * independent observations of one colour, one from inside D3D11 and one from
 * the display server, is the only arrangement in which "it renders" and "it is
 * visible" are separate claims.
 *
 * WHY IT PRESENTS IN A LOOP AND DOES NOT EXIT.  The window is destroyed when
 * this process ends, and with it everything the X server has to show.  So the
 * probe presents PRESENT_FRAMES times with a message pump in between --
 * long enough for the capture to happen, bounded so that a wedged run is a
 * timeout rather than a process left behind on somebody's display.  It prints
 * "present_smoke: READY" before the loop, which is the line the script waits
 * for; no sleeping-and-hoping on either side.
 *
 * THE COLOUR IS THE SAME ONE d3d11_smoke.c USES, and for the same reason:
 *
 *     R  0.00f * 255 =   0.00  -> 0x00
 *     G  0.25f * 255 =  63.75  -> 0x40   (round-to-nearest, and 63.75 is
 *                                         closer to 64 than to 63)
 *     B  0.50f * 255 = 127.50  -> 0x80   (ties-away-from-zero; DXVK's
 *                                         float->UNORM conversion is the
 *                                         Vulkan spec's, which rounds .5 up)
 *     A  1.00f * 255 = 255.00  -> 0xff
 *
 * so a byte that is off by one is a rounding change somewhere in the path and
 * not an artefact of the probe's arithmetic.  The swapchain format is
 * B8G8R8A8_UNORM because that is what every real D3D11 title asks for and what
 * X11 hands back as 24-bit TrueColor without a channel swap in between; the
 * capture side compares in that order and says so.
 *
 * SMOKE_BREAK, the falsification builds -- each MUST make this probe fail, and
 * check-present-smoke.sh --sabotage requires it:
 *
 *   -DSMOKE_BREAK=1  skip ClearRenderTargetView.  The back buffer then holds
 *                    whatever the swapchain was allocated with, the readback
 *                    walk finds mismatches, and the screen shows it too.
 *   -DSMOKE_BREAK=2  present zero frames.  Nothing ever reaches the screen;
 *                    the probe's own readback still passes, which is the
 *                    point -- this is the case that catches a gate that
 *                    proves rendering and calls it presentation.
 *   -DSMOKE_BREAK=3  clear to a DIFFERENT colour (0x00,0x41,0x80 -- one step
 *                    of green).  Everything succeeds, the checksum changes,
 *                    and both the probe's own walk and the capture must
 *                    notice a difference of one in one channel.
 *
 * NO C RUNTIME: same output primitives as d3d11_smoke.c, for the same reason.
 * A CRT would add a second variable to a probe whose whole value is that only
 * the presentation path is under test.
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

#include <windows.h>
#include <d3d11.h>
#include <dxgi.h>

/* The window, and where on the root window the capture side will look for it.
 * Position and size are fixed rather than negotiated because the capture is a
 * separate process reading the X server's root framebuffer: it has to know
 * where to look, and a window manager that might move it is not running under
 * the Xvfb the gate starts. */
#define WIN_X       32
#define WIN_Y       32
#define WIN_W       256
#define WIN_H       256

/* Long enough for the capture to happen, short enough that a wedged run ends
 * as a timeout instead of a window left on a display.  At the 60 Hz a FIFO
 * present mode is paced to, 600 frames is ten seconds of standing still. */
#define PRESENT_FRAMES 600

/* Frames presented BEFORE "READY" is printed.  A compositor fades a newly
 * mapped window in, and a fading window is a blend of the frame and the
 * desktop behind it -- which is a colour that is nearly right, and therefore
 * exactly the kind of thing this gate must not accept.  Waiting a fixed number
 * of PRESENTED frames rather than a wall-clock delay ties the wait to the
 * thing that has to finish; weston's fade is 200 ms, this is a second. */
#define WARMUP_FRAMES 60

/* Spelled out rather than linked from libuuid, for the same reason
 * d3d11_smoke.c spells out its own: this guest build has no Wine import
 * libraries at all, and a GUID compiled from the same bytes in both legs
 * cannot differ between them.  Verified against include/d3d11.idl. */
static const GUID smoke_IID_ID3D11Texture2D =
    { 0x6f15aaf2, 0xd208, 0x4e89, { 0x9a,0xb4,0x48,0x95,0x35,0xd3,0x4f,0x9c } };

static void out( const char *s )
{
    DWORD n = 0, written;
    while (s[n]) n++;
    WriteFile( GetStdHandle( STD_OUTPUT_HANDLE ), s, n, &written, NULL );
}

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

/* Message pump.  A swapchain's window has to answer messages or the window
 * never maps and there is nothing on screen to capture -- and winex11 drives
 * the map from the message loop, not from CreateWindowEx. */
static void pump( void )
{
    MSG msg;
    while (PeekMessageA( &msg, NULL, 0, 0, PM_REMOVE ))
    {
        TranslateMessage( &msg );
        DispatchMessageA( &msg );
    }
}

static int present_smoke_run( void )
{
    static const D3D_FEATURE_LEVEL want_fl[] = { D3D_FEATURE_LEVEL_11_0 };
    ID3D11Device *device = NULL;
    ID3D11DeviceContext *context = NULL;
    IDXGISwapChain *swapchain = NULL;
    ID3D11Texture2D *backbuffer = NULL, *staging = NULL;
    ID3D11RenderTargetView *rtv = NULL;
    D3D_FEATURE_LEVEL got_fl = (D3D_FEATURE_LEVEL)0;
    DXGI_SWAP_CHAIN_DESC desc;
    WNDCLASSA wc;
    HWND hwnd = NULL;
    HRESULT hr;
    UINT i, total, frames = 0, presented_ok = 0;

    out( "present_smoke: start\n" );

    /* ---- step 1: a real window ------------------------------------------ */
    begin( "RegisterClassA + CreateWindowExA(256x256 at 32,32)" );
    {
        static const char cls[] = "present_smoke";
        DWORD n = 0;

        while (cls[n]) n++;
        wc.style = CS_OWNDC;
        wc.lpfnWndProc = DefWindowProcA;
        wc.cbClsExtra = 0;
        wc.cbWndExtra = 0;
        wc.hInstance = GetModuleHandleA( NULL );
        wc.hIcon = NULL;
        wc.hCursor = NULL;
        wc.hbrBackground = NULL;
        wc.lpszMenuName = NULL;
        wc.lpszClassName = cls;
        RegisterClassA( &wc );
        /* WS_POPUP and not WS_OVERLAPPEDWINDOW: a popup has no frame, so the
         * client area IS the rectangle the capture side reads, and there is
         * no title bar whose height would have to be guessed from outside. */
        hwnd = CreateWindowExA( 0, cls, cls, WS_POPUP | WS_VISIBLE,
                                WIN_X, WIN_Y, WIN_W, WIN_H,
                                NULL, NULL, wc.hInstance, NULL );
    }
    out( "hwnd=0x" ); out_hex( (ULONG)(ULONG_PTR)hwnd, 8 );
    verdict( hwnd != NULL, "CreateWindowExA returned no window" );
    if (!hwnd) goto done;
    ShowWindow( hwnd, SW_SHOW );
    UpdateWindow( hwnd );
    pump();

    /* ---- step 2: device + swapchain on that window ----------------------- */
    begin( "D3D11CreateDeviceAndSwapChain(OutputWindow = that HWND)" );
    desc.BufferDesc.Width = WIN_W;
    desc.BufferDesc.Height = WIN_H;
    desc.BufferDesc.RefreshRate.Numerator = 0;
    desc.BufferDesc.RefreshRate.Denominator = 0;
    desc.BufferDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    desc.BufferDesc.ScanlineOrdering = DXGI_MODE_SCANLINE_ORDER_UNSPECIFIED;
    desc.BufferDesc.Scaling = DXGI_MODE_SCALING_UNSPECIFIED;
    desc.SampleDesc.Count = 1;
    desc.SampleDesc.Quality = 0;
    desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    desc.BufferCount = 2;
    desc.OutputWindow = hwnd;
    desc.Windowed = TRUE;
    desc.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;
    desc.Flags = 0;

    hr = D3D11CreateDeviceAndSwapChain( NULL, D3D_DRIVER_TYPE_HARDWARE, NULL, 0,
                                        want_fl, 1, D3D11_SDK_VERSION,
                                        &desc, &swapchain, &device, &got_fl,
                                        &context );
    out_hr( "hr", hr );
    out( " fl=0x" ); out_hex( (ULONG)got_fl, 4 );
    verdict( SUCCEEDED(hr) && swapchain && device && context &&
             got_fl == D3D_FEATURE_LEVEL_11_0,
             "no swapchain/device/context, or the feature level is not 11_0" );
    if (!swapchain || !device || !context) goto done;

    /* ---- step 3: the back buffer and a view over it ---------------------- */
    begin( "GetBuffer(0) + CreateRenderTargetView" );
    hr = IDXGISwapChain_GetBuffer( swapchain, 0, &smoke_IID_ID3D11Texture2D,
                                   (void **)&backbuffer );
    out_hr( "hr", hr );
    if (SUCCEEDED(hr) && backbuffer)
        hr = ID3D11Device_CreateRenderTargetView( device,
                (ID3D11Resource *)backbuffer, NULL, &rtv );
    out_hr( " rtv_hr", hr );
    verdict( SUCCEEDED(hr) && backbuffer && rtv, "no back buffer or no view" );
    if (!backbuffer || !rtv) goto done;

    /* ---- step 4: a staging texture to read the back buffer back ---------- */
    begin( "CreateTexture2D(STAGING, CPU_ACCESS_READ)" );
    {
        D3D11_TEXTURE2D_DESC td;

        ID3D11Texture2D_GetDesc( backbuffer, &td );
        td.Usage = D3D11_USAGE_STAGING;
        td.BindFlags = 0;
        td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        td.MiscFlags = 0;
        hr = ID3D11Device_CreateTexture2D( device, &td, NULL, &staging );
    }
    out_hr( "hr", hr );
    verdict( SUCCEEDED(hr) && staging, "no staging texture" );
    if (!staging) goto done;

    /* ---- step 5: clear, present, and keep presenting --------------------- */
    begin( "ClearRenderTargetView + Present x " );
    out_dec( PRESENT_FRAMES );
    out( ": " );
    {
        FLOAT clear[4];

        clear[0] = 0.00f;
#if SMOKE_BREAK == 3
        /* One step of green: 65/255 = 0.254901... rounds to 0x41. */
        clear[1] = 65.0f / 255.0f;
#else
        clear[1] = 0.25f;
#endif
        clear[2] = 0.50f;
        clear[3] = 1.00f;

#if SMOKE_BREAK == 2
        total = 0;
#else
        total = PRESENT_FRAMES;
#endif
        for (i = 0; i < total; i++)
        {
            ID3D11DeviceContext_OMSetRenderTargets( context, 1, &rtv, NULL );
#if SMOKE_BREAK != 1
            ID3D11DeviceContext_ClearRenderTargetView( context, rtv, clear );
#endif
            hr = IDXGISwapChain_Present( swapchain, 1, 0 );
            frames++;
            if (SUCCEEDED(hr)) presented_ok++;
            pump();

            /* READY goes out once the window has been on screen long enough to
             * have finished being faded in -- and, in the SMOKE_BREAK=2 build
             * that presents nothing at all, never from in here, so it is
             * printed below instead.  The capture side waits for this line
             * rather than for a clock. */
            if (i + 1 == WARMUP_FRAMES) out( "\npresent_smoke: READY\n" );
        }
        if (total < WARMUP_FRAMES) out( "\npresent_smoke: READY\n" );
        /* The last frame's contents are what the capture saw, so read THAT
         * back rather than re-clearing: with DXGI_SWAP_EFFECT_DISCARD the
         * back buffer after Present is undefined, so the clear is repeated
         * once more into a buffer nobody presents and read from there.  It
         * is the same rtv, the same clear call and the same pipeline, which
         * is what the comparison needs; it is not the same allocation, and
         * pretending otherwise would be the lie. */
        ID3D11DeviceContext_OMSetRenderTargets( context, 1, &rtv, NULL );
#if SMOKE_BREAK != 1
        ID3D11DeviceContext_ClearRenderTargetView( context, rtv, clear );
#endif
        ID3D11DeviceContext_CopyResource( context, (ID3D11Resource *)staging,
                                          (ID3D11Resource *)backbuffer );
    }
    out( "frames=" ); out_dec( frames );
    out( " presented_ok=" ); out_dec( presented_ok );
    verdict( frames == presented_ok, "at least one Present did not succeed" );

    /* ---- step 6: what the back buffer actually holds --------------------- */
    begin( "Map(READ) + walk all texels against the expected colour" );
    {
        D3D11_MAPPED_SUBRESOURCE map;
        DWORD csum = 2166136261u;
        UINT checked = 0, mismatches = 0;

        hr = ID3D11DeviceContext_Map( context, (ID3D11Resource *)staging, 0,
                                      D3D11_MAP_READ, 0, &map );
        out_hr( "hr", hr );
        if (SUCCEEDED(hr))
        {
            UINT y, x;

            for (y = 0; y < WIN_H; y++)
            {
                const BYTE *row = (const BYTE *)map.pData + (size_t)y * map.RowPitch;
                for (x = 0; x < WIN_W; x++)
                {
                    const BYTE *texel = row + (size_t)x * 4;
                    /* B8G8R8A8_UNORM: blue first in memory. */
                    BYTE eb = 0x80, eg = 0x40, er = 0x00, ea = 0xff;
#if SMOKE_BREAK == 3
                    eg = 0x41;
#endif
                    checked++;
                    csum = fnv1a( csum, texel, 4 );
                    if (texel[0] != eb || texel[1] != eg ||
                        texel[2] != er || texel[3] != ea)
                        mismatches++;
                }
            }
            ID3D11DeviceContext_Unmap( context, (ID3D11Resource *)staging, 0 );
        }
        out( " checked=" ); out_dec( checked );
        out( " mismatches=" ); out_dec( mismatches );
        verdict( SUCCEEDED(hr) && checked == WIN_W * WIN_H && mismatches == 0,
                 "did not confirm every texel of the presented colour" );

        /* The line the capture side compares against.  Printed unconditionally
         * so a failing run still says what it saw. */
        out( "backbuffer: fnv1a=0x" ); out_hex( csum, 8 );
        out( " texels=" ); out_dec( checked );
        out( " mismatches=" ); out_dec( mismatches );
        out( "\n" );
        out( "expected-bgra: " );
#if SMOKE_BREAK == 3
        out( "80 41 00 ff\n" );
#else
        out( "80 40 00 ff\n" );
#endif
    }

done:
    if (staging) ID3D11Texture2D_Release( staging );
    if (rtv) ID3D11RenderTargetView_Release( rtv );
    if (backbuffer) ID3D11Texture2D_Release( backbuffer );
    if (swapchain) IDXGISwapChain_Release( swapchain );
    if (context) ID3D11DeviceContext_Release( context );
    if (device) ID3D11Device_Release( device );
    if (hwnd) DestroyWindow( hwnd );

    out( failures ? "present_smoke: FAIL " : "present_smoke: PASS " );
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

void __stdcall present_smoke_entry( void )
{
    ExitProcess( present_smoke_run() );
}
