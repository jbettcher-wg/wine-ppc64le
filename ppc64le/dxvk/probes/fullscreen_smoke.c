/*
 * fullscreen_smoke.c -- an x86-64 guest that RESIZES and goes FULLSCREEN.
 *
 * ppc64le/dxvk/check-present-smoke.sh proves a frame reaches the screen at the
 * size the swapchain was created with, and never changes its mind.  Real games
 * change their minds constantly: a resolution setting is an IDXGISwapChain
 * ResizeBuffers, a borderless-window toggle is a SetWindowPos followed by one,
 * and an exclusive-fullscreen option is SetFullscreenState.  Each of those is
 * a vtable slot that has to cross the machine boundary AND a window operation
 * that has to reach the display server, and the two halves fail differently:
 * a slot that did not cross returns a wrong HRESULT, a window operation that
 * did not reach the driver returns S_OK and leaves the picture the size it was.
 *
 * So this probe drives four PHASES against one window and one swapchain, and
 * the gate photographs the screen between them.  Every phase clears to the
 * same colour and presents continuously; what changes is the SIZE of the
 * rectangle that colour occupies, which is the only thing that can distinguish
 * "the resize happened" from "the call returned S_OK".
 *
 *   1  WINDOWED at PHASE1_W x PHASE1_H -- the control, and the same claim
 *      check-present-smoke.sh makes.
 *   2  RESIZED: SetWindowPos to PHASE2_W x PHASE2_H, then ResizeBuffers.  A
 *      game does both, in that order, and doing only one is the bug this
 *      catches -- ResizeBuffers alone leaves the window where it was, and
 *      SetWindowPos alone leaves the back buffer at the old size and DXVK
 *      scales.  The probe re-reads the back buffer's own description
 *      afterwards, so the number it reports is DXGI's and not its own.
 *   3  FULLSCREEN: SetFullscreenState(TRUE).  The HRESULT is reported rather
 *      than asserted, because a display server is entitled to refuse; what is
 *      asserted is that GetFullscreenState AGREES with whatever happened, and
 *      that the rectangle on screen is the size that agreement implies.
 *   4  BACK TO WINDOWED: SetFullscreenState(FALSE), and the rectangle must
 *      return to phase 2's size.  A fullscreen transition that cannot be
 *      undone is worse than one that never happened, because the window is
 *      then stuck over the user's whole screen.
 *
 * IT ALSO ASKS THE DISPLAY MODE QUESTION SEPARATELY, in step 1, because
 * ChangeDisplaySettingsEx has nothing to do with DXGI: it is user32, it is
 * what a pre-DXGI game and every Unreal/Unity settings screen still calls, and
 * on this port it is one more call that crosses the boundary as an ordinary
 * thunk.  The probe enumerates the modes the driver reports, asks for one, and
 * prints what GetSystemMetrics says afterwards -- reporting rather than
 * asserting, because a headless compositor has exactly one mode and refusing
 * is the correct answer there.  The gate reads the numbers.
 *
 * PHASES ADVANCE ON A FILE, never on a timer.  The gate creates
 * $FULLSCREEN_GO.N when it has finished photographing phase N, so neither side
 * ever guesses how long a compositor takes.
 *
 * NO C RUNTIME, for the same reason present_smoke.c has none.
 *
 * Copyright 2026 the ppc64le port authors
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 */

#define COBJMACROS

#ifndef FS_BREAK
#define FS_BREAK 0
#endif

#include <windows.h>
#include <d3d11.h>
#include <dxgi.h>

#define WIN_X       32
#define WIN_Y       32
#define PHASE1_W    256
#define PHASE1_H    256
#define PHASE2_W    192
#define PHASE2_H    144

/* The same colour every other probe in this fold clears to, for the same
 * reason: its float-to-UNORM rounding is written down and a byte off by one is
 * a change in the path rather than in the arithmetic. */
#define CLEAR_R     0.00f
#define CLEAR_G     0.25f
#define CLEAR_B     0.50f

/* Frames presented before a phase announces itself, so a compositor's fade is
 * over before anybody photographs it.  Same number, same reason, as
 * present_smoke.c's WARMUP_FRAMES. */
#define WARMUP_FRAMES 60
/* Bound on a phase that nobody ever releases: a wedged run must end as a
 * timeout rather than as a window left on somebody's display. */
#define PHASE_FRAMES  3000

static const GUID fs_IID_ID3D11Texture2D =
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
    out( label ); out( "=0x" ); out_hex( (ULONG)hr, 8 );
}

static void out_size( const char *label, ULONG w, ULONG h )
{
    out( label ); out( "=" ); out_dec( w ); out( "x" ); out_dec( h );
}

static int failures;
static int step;
static const char *first_fail;

static void begin( const char *what )
{
    out( "step " ); out_dec( ++step ); out( " " ); out( what ); out( ": " );
}

static void verdict( BOOL ok, const char *why )
{
    if (ok) out( " ok\n" );
    else
    {
        if (!first_fail) first_fail = why;
        failures++;
        out( " FAIL (" ); out( why ); out( ")\n" );
    }
}

/* No CRT, so no memset and no struct assignment (the compiler turns one into
 * a memcpy).  present_smoke.c avoids both by construction; this probe has
 * DEVMODEW and DXGI_SWAP_CHAIN_DESC to clear, so it needs a byte writer of its
 * own.  Four lines is cheaper than linking a runtime whose startup would then
 * be a second variable in a probe about presentation. */
static void zero( void *p, unsigned n )
{
    unsigned char *b = p;
    while (n--) *b++ = 0;
}

static void pump( void )
{
    MSG msg;
    while (PeekMessageA( &msg, NULL, 0, 0, PM_REMOVE ))
    {
        TranslateMessage( &msg );
        DispatchMessageA( &msg );
    }
}

/* Where the gate drops its go-files.  One per phase, so a phase can never
 * start before its predecessor has been photographed. */
static char go_base[MAX_PATH];
static DWORD go_len;

static BOOL go_seen( int phase )
{
    if (!go_len) return TRUE;              /* no gate driving us: free-run */
    go_base[go_len] = '.';
    go_base[go_len + 1] = (char)('0' + phase);
    go_base[go_len + 2] = 0;
    return GetFileAttributesA( go_base ) != INVALID_FILE_ATTRIBUTES;
}

/* Present until the gate releases this phase (or the bound runs out).  Returns
 * the number of frames presented; zero means Present itself failed, which is a
 * different failure from "nobody looked". */
static UINT hold_phase( IDXGISwapChain *swapchain, ID3D11DeviceContext *context,
                        ID3D11RenderTargetView *rtv, int phase )
{
    static const FLOAT colour[4] = { CLEAR_R, CLEAR_G, CLEAR_B, 1.0f };
    UINT frames = 0, i;
    BOOL announced = FALSE;

    for (i = 0; i < PHASE_FRAMES; i++)
    {
        ID3D11DeviceContext_ClearRenderTargetView( context, rtv, colour );
        if (FAILED(IDXGISwapChain_Present( swapchain, 1, 0 ))) break;
        frames++;
        pump();
        if (!announced && frames >= WARMUP_FRAMES)
        {
            out( "fullscreen_smoke: PHASE" ); out_dec( phase ); out( " READY\n" );
            announced = TRUE;
        }
        if (announced && go_seen( phase )) break;
    }
    return frames;
}

/* The back buffer's own idea of its size, read from DXGI rather than from
 * anything this probe remembers.  A resize that changed only the window would
 * leave this at the old size, which is precisely the half-done case. */
static BOOL backbuffer_size( IDXGISwapChain *swapchain, UINT *w, UINT *h )
{
    ID3D11Texture2D *bb = NULL;
    D3D11_TEXTURE2D_DESC td;

    *w = *h = 0;
    if (FAILED(IDXGISwapChain_GetBuffer( swapchain, 0, &fs_IID_ID3D11Texture2D,
                                         (void **)&bb )) || !bb)
        return FALSE;
    ID3D11Texture2D_GetDesc( bb, &td );
    ID3D11Texture2D_Release( bb );
    *w = td.Width;
    *h = td.Height;
    return TRUE;
}

/* Rebuild the render target view over whatever the back buffer is now.  A view
 * survives no resize: ResizeBuffers requires every reference to the old
 * buffers to be released first, and a game that forgot would get
 * DXGI_ERROR_INVALID_CALL.  Doing it correctly here is what makes the
 * resize's HRESULT mean something. */
static HRESULT rebuild_rtv( ID3D11Device *device, IDXGISwapChain *swapchain,
                            ID3D11RenderTargetView **rtv )
{
    ID3D11Texture2D *bb = NULL;
    HRESULT hr;

    if (*rtv) { ID3D11RenderTargetView_Release( *rtv ); *rtv = NULL; }
    hr = IDXGISwapChain_GetBuffer( swapchain, 0, &fs_IID_ID3D11Texture2D, (void **)&bb );
    if (FAILED(hr) || !bb) return hr;
    hr = ID3D11Device_CreateRenderTargetView( device, (ID3D11Resource *)bb, NULL, rtv );
    ID3D11Texture2D_Release( bb );
    return hr;
}

static int fullscreen_smoke_run( void )
{
    static const D3D_FEATURE_LEVEL want_fl[] = { D3D_FEATURE_LEVEL_11_0 };
    ID3D11Device *device = NULL;
    ID3D11DeviceContext *context = NULL;
    IDXGISwapChain *swapchain = NULL;
    ID3D11RenderTargetView *rtv = NULL;
    D3D_FEATURE_LEVEL got_fl = (D3D_FEATURE_LEVEL)0;
    DXGI_SWAP_CHAIN_DESC desc;
    WNDCLASSA wc;
    HWND hwnd = NULL;
    HRESULT hr;
    UINT w, h, frames;
    BOOL fs = FALSE;
    IDXGIOutput *target = NULL;

    out( "fullscreen_smoke: start\n" );
    go_len = GetEnvironmentVariableA( "FULLSCREEN_GO", go_base, sizeof(go_base) - 4 );
    if (go_len >= sizeof(go_base) - 4) go_len = 0;

    /* ---- step 1: what the driver says the display can do ----------------- */
    /* Reported, not asserted.  A headless compositor has one mode and saying
     * so is correct; a real X server has many.  What the gate reads out of
     * this is whether the port carries the calls at all -- an EnumDisplaySettings
     * that returned nothing, or a ChangeDisplaySettingsEx that never reached a
     * driver, would show up here as zeros. */
    begin( "EnumDisplaySettingsW + ChangeDisplaySettingsExW" );
    {
        DEVMODEW dm;
        DWORD i, n = 0;
        LONG rc = DISP_CHANGE_FAILED;
        int before_x = GetSystemMetrics( SM_CXSCREEN );
        int before_y = GetSystemMetrics( SM_CYSCREEN );
        DEVMODEW want;
        BOOL have_want = FALSE;

        zero( &want, sizeof(want) );
        for (i = 0; i < 256; i++)
        {
            zero( &dm, sizeof(dm) );
            dm.dmSize = sizeof(dm);
            if (!EnumDisplaySettingsW( NULL, i, &dm )) break;
            n++;
            /* the first mode that is NOT the current one, so asking for it is
             * a real request rather than a no-op the driver can shortcut */
            if (!have_want && ((int)dm.dmPelsWidth != before_x ||
                               (int)dm.dmPelsHeight != before_y))
            {
                /* Field by field rather than `want = dm`: a struct assignment
                 * compiles to a memcpy, and there is no CRT here. */
                want.dmPelsWidth  = dm.dmPelsWidth;
                want.dmPelsHeight = dm.dmPelsHeight;
                want.dmBitsPerPel = dm.dmBitsPerPel;
                have_want = TRUE;
            }
        }
        out( "modes=" ); out_dec( n );
        out( " " ); out_size( "before", before_x, before_y );
        if (have_want)
        {
            out( " " ); out_size( "asked", want.dmPelsWidth, want.dmPelsHeight );
            want.dmSize = sizeof(want);
            want.dmFields = DM_PELSWIDTH | DM_PELSHEIGHT | DM_BITSPERPEL;
            rc = ChangeDisplaySettingsExW( NULL, &want, NULL, CDS_FULLSCREEN, NULL );
            out( " rc=" ); out_dec( (ULONG)rc );
            out( " " ); out_size( "after", GetSystemMetrics( SM_CXSCREEN ),
                                  GetSystemMetrics( SM_CYSCREEN ) );
            /* put it back before anything else happens, whatever it did */
            ChangeDisplaySettingsExW( NULL, NULL, NULL, 0, NULL );
            out( " " ); out_size( "restored", GetSystemMetrics( SM_CXSCREEN ),
                                  GetSystemMetrics( SM_CYSCREEN ) );
        }
        else out( " asked=none(one-mode-display)" );
        /* The claim is only that the calls CROSS and answer coherently: the
         * driver reported at least one mode, and the screen metrics are not
         * zero.  Whether a mode change is possible belongs to the display. */
        verdict( n > 0 && before_x > 0 && before_y > 0,
                 "the display driver reported no modes at all, or no screen size" );
    }

    /* ---- step 2: a window and a swapchain -------------------------------- */
    begin( "RegisterClassA + CreateWindowExA + D3D11CreateDeviceAndSwapChain" );
    {
        static const char cls[] = "fullscreen_smoke";

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
                                WIN_X, WIN_Y, PHASE1_W, PHASE1_H,
                                NULL, NULL, wc.hInstance, NULL );
    }
    if (hwnd) { ShowWindow( hwnd, SW_SHOW ); UpdateWindow( hwnd ); pump(); }

    zero( &desc, sizeof(desc) );
    desc.BufferDesc.Width = PHASE1_W;
    desc.BufferDesc.Height = PHASE1_H;
    desc.BufferDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    desc.BufferCount = 2;
    desc.OutputWindow = hwnd;
    desc.Windowed = TRUE;
    desc.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;

    hr = hwnd ? D3D11CreateDeviceAndSwapChain( NULL, D3D_DRIVER_TYPE_HARDWARE, NULL, 0,
                                               want_fl, 1, D3D11_SDK_VERSION,
                                               &desc, &swapchain, &device, &got_fl,
                                               &context )
              : E_FAIL;
    out_hr( "hr", hr );
    verdict( SUCCEEDED(hr) && swapchain && device && context,
             "no window, or no swapchain/device/context" );
    if (!swapchain || !device || !context) goto done;

    hr = rebuild_rtv( device, swapchain, &rtv );
    begin( "CreateRenderTargetView over the back buffer" );
    out_hr( "hr", hr );
    verdict( SUCCEEDED(hr) && rtv, "no render target view" );
    if (!rtv) goto done;

    /* ---- phase 1: windowed at PHASE1 ------------------------------------- */
    begin( "phase 1: present windowed" );
    backbuffer_size( swapchain, &w, &h );
    out_size( "backbuffer", w, h );
    frames = hold_phase( swapchain, context, rtv, 1 );
    out( " frames=" ); out_dec( frames );
    verdict( frames > 0 && w == PHASE1_W && h == PHASE1_H,
             "the back buffer is not the size the swapchain was created with, "
             "or nothing was presented" );

    /* ---- phase 2: resize ------------------------------------------------- */
    begin( "phase 2: SetWindowPos + ResizeBuffers" );
    {
        BOOL moved;
#if FS_BREAK == 1
        /* THE HALF-DONE RESIZE.  The window moves and the back buffer does
         * not, so DXVK scales the old buffer into the new window: every call
         * succeeds, the screen shows the right colour, and the rectangle is
         * the right size -- which is why the gate checks the BACK BUFFER's
         * size out of DXGI as well as the rectangle on screen. */
        moved = SetWindowPos( hwnd, NULL, WIN_X, WIN_Y, PHASE2_W, PHASE2_H,
                              SWP_NOZORDER | SWP_NOACTIVATE );
        hr = S_OK;
#else
        moved = SetWindowPos( hwnd, NULL, WIN_X, WIN_Y, PHASE2_W, PHASE2_H,
                              SWP_NOZORDER | SWP_NOACTIVATE );
        pump();
        /* Every reference to the old back buffers has to go first; that is
         * DXGI's rule and not this port's. */
        if (rtv) { ID3D11RenderTargetView_Release( rtv ); rtv = NULL; }
        ID3D11DeviceContext_OMSetRenderTargets( context, 0, NULL, NULL );
        ID3D11DeviceContext_Flush( context );
        hr = IDXGISwapChain_ResizeBuffers( swapchain, 0, PHASE2_W, PHASE2_H,
                                           DXGI_FORMAT_UNKNOWN, 0 );
        if (SUCCEEDED(hr)) hr = rebuild_rtv( device, swapchain, &rtv );
#endif
        out( "movewindow=" ); out( moved ? "yes" : "no" ); out( " " );
        out_hr( "resize_hr", hr );
        backbuffer_size( swapchain, &w, &h );
        out( " " ); out_size( "backbuffer", w, h );
        verdict( moved && SUCCEEDED(hr) && rtv && w == PHASE2_W && h == PHASE2_H,
                 "ResizeBuffers did not resize the back buffer" );
    }
    if (!rtv) goto done;
    begin( "phase 2: present resized" );
    frames = hold_phase( swapchain, context, rtv, 2 );
    out( "frames=" ); out_dec( frames );
    verdict( frames > 0, "nothing was presented after the resize" );

    /* ---- phase 3: fullscreen --------------------------------------------- */
    begin( "phase 3: SetFullscreenState(TRUE)" );
    {
#if FS_BREAK == 2
        hr = S_OK;            /* claim it worked without asking for it */
#else
        hr = IDXGISwapChain_SetFullscreenState( swapchain, TRUE, NULL );
#endif
        out_hr( "hr", hr );
        pump();
        fs = FALSE;
        if (FAILED(IDXGISwapChain_GetFullscreenState( swapchain, &fs, &target )))
            fs = FALSE;
        if (target) { IDXGIOutput_Release( target ); target = NULL; }
        out( " getfullscreenstate=" ); out( fs ? "TRUE" : "FALSE" );
        out( " " ); out_size( "screen", GetSystemMetrics( SM_CXSCREEN ),
                              GetSystemMetrics( SM_CYSCREEN ) );
        /* The two must AGREE.  A SetFullscreenState that returned S_OK and a
         * GetFullscreenState that says FALSE is the shape that leaves a game
         * rendering into a window it believes covers the screen. */
        verdict( (SUCCEEDED(hr) && fs) || (FAILED(hr) && !fs),
                 "SetFullscreenState and GetFullscreenState disagree" );
    }
    if (fs)
    {
        /* The buffers follow the target when the transition really happened. */
        if (rtv) { ID3D11RenderTargetView_Release( rtv ); rtv = NULL; }
        ID3D11DeviceContext_OMSetRenderTargets( context, 0, NULL, NULL );
        IDXGISwapChain_ResizeBuffers( swapchain, 0, 0, 0, DXGI_FORMAT_UNKNOWN, 0 );
        rebuild_rtv( device, swapchain, &rtv );
        begin( "phase 3: present fullscreen" );
        backbuffer_size( swapchain, &w, &h );
        out_size( "backbuffer", w, h );
        frames = rtv ? hold_phase( swapchain, context, rtv, 3 ) : 0;
        out( " frames=" ); out_dec( frames );
        verdict( frames > 0, "nothing was presented while fullscreen" );
    }
    else
    {
        out( "fullscreen_smoke: PHASE3 REFUSED\n" );
        out( "fullscreen_smoke: PHASE3 READY\n" );   /* so the gate does not wait */
    }

    /* ---- phase 4: back to windowed --------------------------------------- */
    begin( "phase 4: SetFullscreenState(FALSE) and back to the windowed size" );
    {
        hr = IDXGISwapChain_SetFullscreenState( swapchain, FALSE, NULL );
        out_hr( "hr", hr );
        pump();
        SetWindowPos( hwnd, NULL, WIN_X, WIN_Y, PHASE2_W, PHASE2_H,
                      SWP_NOZORDER | SWP_NOACTIVATE );
        pump();
        if (rtv) { ID3D11RenderTargetView_Release( rtv ); rtv = NULL; }
        ID3D11DeviceContext_OMSetRenderTargets( context, 0, NULL, NULL );
        hr = IDXGISwapChain_ResizeBuffers( swapchain, 0, PHASE2_W, PHASE2_H,
                                           DXGI_FORMAT_UNKNOWN, 0 );
        if (SUCCEEDED(hr)) hr = rebuild_rtv( device, swapchain, &rtv );
        backbuffer_size( swapchain, &w, &h );
        out( " " ); out_size( "backbuffer", w, h );
        verdict( SUCCEEDED(hr) && rtv && w == PHASE2_W && h == PHASE2_H,
                 "the swapchain did not come back to the windowed size" );
    }
    if (rtv)
    {
        begin( "phase 4: present windowed again" );
        frames = hold_phase( swapchain, context, rtv, 4 );
        out( "frames=" ); out_dec( frames );
        verdict( frames > 0, "nothing was presented after leaving fullscreen" );
    }

done:
    /* Leaving a swapchain fullscreen at exit is how a display is left in a
     * mode nobody asked for; DXGI says so and so does everyone who has done
     * it by accident. */
    if (swapchain) IDXGISwapChain_SetFullscreenState( swapchain, FALSE, NULL );
    if (rtv) ID3D11RenderTargetView_Release( rtv );
    if (swapchain) IDXGISwapChain_Release( swapchain );
    if (context) ID3D11DeviceContext_Release( context );
    if (device) ID3D11Device_Release( device );
    if (hwnd) DestroyWindow( hwnd );

    out( "fullscreen_smoke: " );
    if (failures)
    {
        out( "FAIL " ); out_dec( step - failures ); out( "/" ); out_dec( step );
        out( " (" ); out( first_fail ); out( ")\n" );
    }
    else { out( "PASS " ); out_dec( step ); out( "/" ); out_dec( step ); out( "\n" ); }
    return failures ? 1 : 0;
}

void __stdcall fullscreen_smoke_entry( void )
{
    ExitProcess( fullscreen_smoke_run() );
}
