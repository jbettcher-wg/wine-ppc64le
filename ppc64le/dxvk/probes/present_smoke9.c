/*
 * present_smoke9.c -- an x86-64 guest that puts D3D9 pixels on a real screen.
 *
 * The D3D9 sibling of present_smoke.c, and the two are deliberately separate
 * files rather than one file with an #ifdef down the middle: they share a
 * window, a colour and an output format, and share NOTHING of the API under
 * test.  Read present_smoke.c's banner first -- the two-observers design, why
 * the HWND crosses unconverted, why the probe presents in a loop and prints
 * READY, and why the colour is the one it is, are all written out there and
 * are true here word for word.
 *
 * WHAT THIS ONE PROVES THAT THE D3D11 ONE DOES NOT.  D3D9's presentation path
 * through this lane is not a smaller version of D3D11's, it is a different
 * shape:
 *
 *   - there is no DXGI and no swapchain object.  The window is an argument of
 *     IDirect3D9::CreateDevice AND a member of D3DPRESENT_PARAMETERS, the
 *     swapchain is implicit in the device, and DXVK builds its Presenter --
 *     and its VkSurfaceKHR -- inside CreateDevice itself.  So the very first
 *     call this probe makes after creating a window is the one that has to
 *     reach win32u's client-surface layer and come back with a surface.  In
 *     the D3D11 probe that does not happen until the swapchain is asked for.
 *   - Present is a method on the DEVICE, with a per-call destination window
 *     override, so which window's client surface to update is a question
 *     D3D11 never has to answer.
 *   - `Clear` carries the depth as a by-value float in the FIFTH argument
 *     position, which MS-x64 spills to the stack.  Every frame of this probe
 *     goes through that hand-written slot, so a wrong answer there shows up
 *     here as a wrong colour on the screen rather than as a subtle depth bug
 *     nobody sees.
 *
 * WHY IT DOES NOT SET d3d9.deferSurfaceCreation, AND check-d3d9-smoke.sh DOES.
 * That option exists because there is no offscreen D3D9 device: with no
 * display server, CreateDevice cannot build its implicit swapchain and fails
 * with D3DERR_NOTAVAILABLE.  The offscreen gate sets it because it has no
 * window and wants none.  This probe has a real window on a real compositor,
 * so it wants exactly the eager path -- and the fact that CreateDevice
 * SUCCEEDS here without the option is itself the measurement that the win32u
 * surface is real.
 *
 * SMOKE_BREAK, the falsification builds -- each MUST make this probe fail:
 *
 *   -DSMOKE_BREAK=1  skip Clear.  The back buffer holds whatever the swapchain
 *                    was allocated with and the screen shows it.
 *   -DSMOKE_BREAK=2  present zero frames.  Nothing reaches the screen; the
 *                    probe still reports its own steps as passing, which is
 *                    the point -- this is the case that catches a gate which
 *                    proves rendering and calls it presentation.
 *   -DSMOKE_BREAK=3  clear to a colour one step of green away.  Everything
 *                    succeeds and the capture must notice one byte.
 *
 * NO C RUNTIME: same output primitives as its sibling, for the same reason.
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
#include <d3d9.h>

#define WIN_X       32
#define WIN_Y       32
#define WIN_W       256
#define WIN_H       256

#define PRESENT_FRAMES 600
#define WARMUP_FRAMES  60

/* The same colour present_smoke.c uses, expressed the way D3D9 expresses it:
 * an exact 8-bit ARGB integer, with no float-to-UNORM rounding to reason about
 * at all.  That is a real difference between the two APIs and it is why the
 * two probes can be compared against the same expected bytes with confidence
 * -- if D3D11's rounding ever moved, this one would still say 00 40 80. */
#if SMOKE_BREAK == 3
#define CLEAR_G 0x41
#else
#define CLEAR_G 0x40
#endif
#define CLEAR_R 0x00
#define CLEAR_B 0x80

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

/* A swapchain's window has to answer messages or it never maps, and there is
 * then nothing on screen to capture. */
static void pump( void )
{
    MSG msg;
    while (PeekMessageA( &msg, NULL, 0, 0, PM_REMOVE ))
    {
        TranslateMessage( &msg );
        DispatchMessageA( &msg );
    }
}

static int present_smoke9_run( void )
{
    IDirect3D9 *d3d9 = NULL;
    IDirect3DDevice9 *device = NULL;
    D3DPRESENT_PARAMETERS pp;
    WNDCLASSA wc;
    HWND hwnd = NULL;
    HRESULT hr;
    UINT i, total, frames = 0, presented_ok = 0, cleared_ok = 0;

    out( "present_smoke9: start\n" );

    /* ---- step 1: a real window ------------------------------------------ */
    begin( "RegisterClassA + CreateWindowExA(256x256 at 32,32)" );
    {
        static const char cls[] = "present_smoke9";

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

    /* ---- step 2: the D3D9 object ---------------------------------------- */
    begin( "Direct3DCreate9(D3D_SDK_VERSION)" );
    d3d9 = Direct3DCreate9( D3D_SDK_VERSION );
    verdict( d3d9 != NULL, "Direct3DCreate9 returned NULL" );
    if (!d3d9) goto done;

    /* ---- step 3: a device ON THAT WINDOW, with no deferral --------------- */
    begin( "CreateDevice(HAL, HWVP, windowed 256x256 X8R8G8B8, the real HWND)" );
    pp.BackBufferWidth = WIN_W;
    pp.BackBufferHeight = WIN_H;
    pp.BackBufferFormat = D3DFMT_X8R8G8B8;
    pp.BackBufferCount = 1;
    pp.MultiSampleType = D3DMULTISAMPLE_NONE;
    pp.MultiSampleQuality = 0;
    pp.SwapEffect = D3DSWAPEFFECT_DISCARD;
    pp.hDeviceWindow = hwnd;
    pp.Windowed = TRUE;
    pp.EnableAutoDepthStencil = FALSE;
    pp.AutoDepthStencilFormat = D3DFMT_UNKNOWN;
    pp.Flags = 0;
    pp.FullScreen_RefreshRateInHz = 0;
    pp.PresentationInterval = D3DPRESENT_INTERVAL_ONE;

    hr = IDirect3D9_CreateDevice( d3d9, D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, hwnd,
                                  D3DCREATE_HARDWARE_VERTEXPROCESSING, &pp, &device );
    out_hr( "hr", hr );
    /* THIS is the measurement.  D3D9 has no offscreen device: CreateDevice
     * builds the implicit swapchain and DXVK builds its VkSurfaceKHR inside
     * it, so success here means win32u handed back a real surface for a real
     * Wine window.  Headless, the same call returns 0x8876086A. */
    verdict( SUCCEEDED(hr) && device != NULL,
             "CreateDevice failed -- no implicit swapchain, so no surface" );
    if (!device) goto done;

    /* ---- step 4: clear and present -------------------------------------- */
    begin( "Clear(A=FF R=00 G=" );
    out_hex( CLEAR_G, 2 );
    out( " B=80) + Present x " );
    out_dec( PRESENT_FRAMES );
    out( ": " );

#if SMOKE_BREAK == 2
    total = 0;
#else
    total = PRESENT_FRAMES;
#endif
    for (i = 0; i < total; i++)
    {
#if SMOKE_BREAK != 1
        /* The depth argument is the by-value float this lane hand-wrote a slot
         * for; it is passed even with no depth buffer, because what is under
         * test is that the value arrives. */
        hr = IDirect3DDevice9_Clear( device, 0, NULL, D3DCLEAR_TARGET,
                                     D3DCOLOR_ARGB( 0xff, CLEAR_R, CLEAR_G, CLEAR_B ),
                                     1.0f, 0 );
        if (SUCCEEDED(hr)) cleared_ok++;
#else
        cleared_ok++;
#endif
        IDirect3DDevice9_BeginScene( device );
        IDirect3DDevice9_EndScene( device );
        hr = IDirect3DDevice9_Present( device, NULL, NULL, NULL, NULL );
        frames++;
        if (SUCCEEDED(hr)) presented_ok++;
        pump();

        /* READY once the window has been on screen long enough to have
         * finished being faded in by the compositor; and, in the build that
         * presents nothing at all, from after the loop instead.  The capture
         * side waits for this line rather than for a clock. */
        if (i + 1 == WARMUP_FRAMES) out( "\npresent_smoke9: READY\n" );
    }
    if (total < WARMUP_FRAMES) out( "\npresent_smoke9: READY\n" );
    out( "frames=" ); out_dec( frames );
    out( " cleared_ok=" ); out_dec( cleared_ok );
    out( " presented_ok=" ); out_dec( presented_ok );
    verdict( frames == presented_ok && frames == cleared_ok,
             "at least one Clear or Present did not succeed" );

    out( "expected-rgb: " );
    out_hex( CLEAR_R, 2 ); out( " " );
    out_hex( CLEAR_G, 2 ); out( " " );
    out_hex( CLEAR_B, 2 ); out( "\n" );

done:
    if (device) IDirect3DDevice9_Release( device );
    if (d3d9) IDirect3D9_Release( d3d9 );
    if (hwnd) DestroyWindow( hwnd );

    out( failures ? "present_smoke9: FAIL " : "present_smoke9: PASS " );
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

void __stdcall present_smoke9_entry( void )
{
    ExitProcess( present_smoke9_run() );
}
