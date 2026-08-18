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
 * THE SECOND FAILURE IS THE ONE THIS PROBE WAS WRITTEN FOR, AND IT IS FIXED.
 * [MEASURED] 2026-08-18: SetFullscreenState(TRUE) used to return S_OK,
 * GetFullscreenState used to agree, and the rectangle on screen never moved --
 * because DXVK's Win32u WSI driver inherited a deliberate no-op
 * enterFullscreenMode from the foreign driver it derives from, whose premise
 * ("the window belongs to somebody else") is right for a raw X11 window ID and
 * wrong for a Wine HWND.  The driver overrides it now, through six new calls in
 * the WSI callback table (ppc64le/dxvk/dxvk_win32u_wsi.h, abi 2) that land in
 * NtUserSetWindowPos and NtUserChangeDisplaySettings.  So every claim below is
 * asserted rather than reported, and this probe's own numbers are only half of
 * each -- the gate photographs the compositor's framebuffer for the other half.
 *
 * FOUR PHASES against one window and one swapchain, with the gate photographing
 * the screen between them:
 *
 *   1  WINDOWED at PHASE1_W x PHASE1_H -- the control, and the same claim
 *      check-present-smoke.sh makes.
 *   2  RESIZED: SetWindowPos to PHASE2_W x PHASE2_H, then ResizeBuffers.  A
 *      game does both, in that order, and doing only one is the bug this
 *      catches -- ResizeBuffers alone leaves the window where it was, and
 *      SetWindowPos alone leaves the back buffer at the old size and DXVK
 *      scales.  The probe re-reads the back buffer's own description
 *      afterwards, so the number it reports is DXGI's and not its own.
 *   3  FULLSCREEN: SetFullscreenState(TRUE).  The WINDOW must now be exactly
 *      the screen, GetFullscreenState must agree that it happened, and
 *      ResizeBuffers(0, 0, ...) -- which asks DXVK for the window's size rather
 *      than telling it one -- must produce a back buffer of the screen's size.
 *      Three numbers from three different places, all of which were the old
 *      windowed size before this was fixed.
 *   4  BACK TO WINDOWED: SetFullscreenState(FALSE).  The probe deliberately
 *      does NOT move the window back itself.  Putting the window where it was
 *      is DXGI's job (LeaveFullscreenMode -> wsi::restoreWindowState), and a
 *      probe that re-set the geometry would hide a restore that never happened
 *      -- which is exactly the shape of "the window is stuck over the user's
 *      whole screen", the worst outcome available here.  The rectangle, the
 *      back buffer and the display mode must all come back on their own.
 *
 * THE DISPLAY MODE IS A SEPARATE QUESTION AND GETS A SEPARATE BUILD.  Compiled
 * with -DFS_MODE_SWITCH=1 the swapchain carries
 * DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH, which is what makes DXGI pick a mode
 * out of the output's list and ask for it on the way into fullscreen; without
 * the flag DXGI deliberately asks for the mode the display is already in, and
 * nothing about ChangeDisplaySettings would be exercised at all.  The two
 * builds are separate runs because their observables are different: the plain
 * one is checked against a photograph of the screen, and the mode one against
 * the screen METRICS, which is the only place an emulated mode change is
 * unambiguously visible.  It also asks the same question directly through
 * user32 first, the way a pre-DXGI game and every Unreal settings screen does.
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

#ifndef FS_MODE_SWITCH
#define FS_MODE_SWITCH 0
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

static void out_int( LONG v )
{
    if (v < 0) { out( "-" ); out_dec( (ULONG)(-v) ); }
    else out_dec( (ULONG)v );
}

static void out_hr( const char *label, HRESULT hr )
{
    out( label ); out( "=0x" ); out_hex( (ULONG)hr, 8 );
}

static void out_size( const char *label, ULONG w, ULONG h )
{
    out( label ); out( "=" ); out_dec( w ); out( "x" ); out_dec( h );
}

static void out_rect( const char *label, const RECT *r )
{
    out( label ); out( "=" ); out_int( r->left ); out( "," ); out_int( r->top );
    out( "," ); out_int( r->right ); out( "," ); out_int( r->bottom );
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

static BOOL rect_is( const RECT *r, LONG w, LONG h )
{
    return r->right - r->left == w && r->bottom - r->top == h;
}

static BOOL rect_same( const RECT *a, const RECT *b )
{
    return a->left == b->left && a->top == b->top &&
           a->right == b->right && a->bottom == b->bottom;
}

/* How many DISTINCT sizes the display driver reports, and the first one that
 * is not the size the screen is in now at the same colour depth.
 *
 * The depth matters: EnumDisplaySettings walks depths outermost, so the very
 * first mode on a Wine display is 8bpp, and asking for that is a request a
 * driver is entitled to refuse for reasons that have nothing to do with this
 * port.  A game asks for a different SIZE at the depth it is already running,
 * and so does this. */
static DWORD survey_modes( DEVMODEW *want, BOOL *have_want )
{
    DEVMODEW cur, dm;
    DWORD i, sizes = 0;
    LONG seen_w[32], seen_h[32];

    *have_want = FALSE;
    zero( &cur, sizeof(cur) );
    cur.dmSize = sizeof(cur);
    if (!EnumDisplaySettingsW( NULL, ENUM_CURRENT_SETTINGS, &cur )) return 0;

    for (i = 0; i < 512; i++)
    {
        DWORD j;

        zero( &dm, sizeof(dm) );
        dm.dmSize = sizeof(dm);
        if (!EnumDisplaySettingsW( NULL, i, &dm )) break;

        for (j = 0; j < sizes; j++)
            if (seen_w[j] == (LONG)dm.dmPelsWidth && seen_h[j] == (LONG)dm.dmPelsHeight) break;
        if (j == sizes && sizes < 32)
        {
            seen_w[sizes] = dm.dmPelsWidth;
            seen_h[sizes] = dm.dmPelsHeight;
            sizes++;
        }

        if (*have_want) continue;
        if (dm.dmBitsPerPel != cur.dmBitsPerPel) continue;
        if (dm.dmPelsWidth == cur.dmPelsWidth && dm.dmPelsHeight == cur.dmPelsHeight) continue;

        /* Field by field rather than `*want = dm`: a struct assignment
         * compiles to a memcpy, and there is no CRT here. */
        zero( want, sizeof(*want) );
        want->dmSize        = sizeof(*want);
        want->dmFields      = DM_PELSWIDTH | DM_PELSHEIGHT | DM_BITSPERPEL;
        want->dmPelsWidth   = dm.dmPelsWidth;
        want->dmPelsHeight  = dm.dmPelsHeight;
        want->dmBitsPerPel  = dm.dmBitsPerPel;
        *have_want = TRUE;
    }
    return sizes;
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
    RECT r, windowed_rect;
    LONG screen0_w, screen0_h;
    DWORD mode_sizes = 0;

    out( "fullscreen_smoke: start\n" );
    go_len = GetEnvironmentVariableA( "FULLSCREEN_GO", go_base, sizeof(go_base) - 4 );
    if (go_len >= sizeof(go_base) - 4) go_len = 0;

    screen0_w = GetSystemMetrics( SM_CXSCREEN );
    screen0_h = GetSystemMetrics( SM_CYSCREEN );
    out( "fullscreen_smoke: SCREEN " ); out_dec( screen0_w ); out( "x" );
    out_dec( screen0_h ); out( "\n" );

    /* ---- step 1: the display's own mode list, through user32 -------------
     *
     * ChangeDisplaySettingsEx has nothing to do with DXGI: it is user32, it is
     * what a pre-DXGI game and every settings screen still calls, and on this
     * port it is one more call that crosses the boundary as an ordinary thunk.
     * Asserted rather than reported WHEN THERE IS SOMETHING TO ASSERT: a
     * display with exactly one mode cannot prove a mode change and saying so is
     * the honest answer there, which is why the count is in the output. */
    begin( "EnumDisplaySettingsW + ChangeDisplaySettingsExW" );
    {
        DEVMODEW want;
        BOOL have_want = FALSE;
        LONG rc = 0, after_w = screen0_w, after_h = screen0_h;
        LONG back_w = screen0_w, back_h = screen0_h;
        BOOL ok;

        mode_sizes = survey_modes( &want, &have_want );
        out( "modes=" ); out_dec( mode_sizes );
        out( " " ); out_size( "before", screen0_w, screen0_h );

        if (have_want)
        {
            out( " " ); out_size( "asked", want.dmPelsWidth, want.dmPelsHeight );
            rc = ChangeDisplaySettingsExW( NULL, &want, NULL, CDS_FULLSCREEN, NULL );
            out( " rc=" ); out_int( rc );
            after_w = GetSystemMetrics( SM_CXSCREEN );
            after_h = GetSystemMetrics( SM_CYSCREEN );
            out( " " ); out_size( "after", after_w, after_h );
            /* Put it back before anything else happens, whatever it did.  This
             * is the same call DXVK's restoreDisplayMode ends in. */
            ChangeDisplaySettingsExW( NULL, NULL, NULL, 0, NULL );
            back_w = GetSystemMetrics( SM_CXSCREEN );
            back_h = GetSystemMetrics( SM_CYSCREEN );
            out( " " ); out_size( "restored", back_w, back_h );

            /* THE ASSERTION: the driver took it, the screen followed, and the
             * screen came back.  "rc says success but SM_CXSCREEN did not
             * move" is the exact shape of a call that crossed the boundary and
             * reached nothing, and it is what this is here to catch. */
            ok = rc == DISP_CHANGE_SUCCESSFUL &&
                 after_w == (LONG)want.dmPelsWidth && after_h == (LONG)want.dmPelsHeight &&
                 back_w == screen0_w && back_h == screen0_h;
        }
        else
        {
            out( " asked=none(one-mode-display)" );
            ok = mode_sizes > 0 && screen0_w > 0 && screen0_h > 0;
        }
        verdict( ok, have_want
                 ? "ChangeDisplaySettingsExW did not change the screen, or did not undo it"
                 : "the display driver reported no modes at all, or no screen size" );
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
#if FS_MODE_SWITCH
    /* Without this flag DXGI deliberately asks for the mode the display is
     * already in (dxgi_swapchain.cpp ChangeDisplayMode zeroes the preferred
     * width and height), so no ChangeDisplaySettings ever happens.  With it,
     * entering fullscreen picks the closest mode to the back buffer out of the
     * output's list and asks for that. */
    desc.Flags = DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH;
#endif

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
    GetWindowRect( hwnd, &r );
    out( " " ); out_rect( "winrect", &r );
    frames = hold_phase( swapchain, context, rtv, 1 );
    out( " frames=" ); out_dec( frames );
    verdict( frames > 0 && w == PHASE1_W && h == PHASE1_H &&
             rect_is( &r, PHASE1_W, PHASE1_H ),
             "the back buffer or the window is not the size the swapchain was "
             "created with, or nothing was presented" );

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
                                           DXGI_FORMAT_UNKNOWN, desc.Flags );
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
    /* Remembered here, AFTER the resize and BEFORE the transition: this is the
     * rectangle phase 4 requires DXGI to put back on its own. */
    GetWindowRect( hwnd, &windowed_rect );
    out_rect( "winrect", &windowed_rect );
    frames = hold_phase( swapchain, context, rtv, 2 );
    out( " frames=" ); out_dec( frames );
    verdict( frames > 0 && rect_is( &windowed_rect, PHASE2_W, PHASE2_H ),
             "nothing was presented after the resize, or the window is not the "
             "resized size" );

    /* ---- phase 3: fullscreen --------------------------------------------- */
    begin( "phase 3: SetFullscreenState(TRUE)" );
    {
        LONG sw, sh;

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

        sw = GetSystemMetrics( SM_CXSCREEN );
        sh = GetSystemMetrics( SM_CYSCREEN );
        GetWindowRect( hwnd, &r );
        out( " " ); out_size( "screen", sw, sh );
        out( " " ); out_rect( "winrect", &r );

        /* THE TWO CLAIMS, AND THEY ARE NOT THE SAME CLAIM.
         *
         * COHERENCE: Set and Get must agree.  A SetFullscreenState that
         * returned S_OK and a GetFullscreenState that says FALSE is the shape
         * that leaves a game rendering into a window it believes covers the
         * screen.
         *
         * AND THE WINDOW MUST ACTUALLY BE THE SCREEN.  This is the claim this
         * gate was red on until 2026-08-18, and the one the whole abi-2
         * callback table exists to make true: DXGI accounting for a transition
         * it never asked anyone to perform returns S_OK from both calls and
         * leaves the rectangle exactly where it was. */
        verdict( ((SUCCEEDED(hr) && fs) || (FAILED(hr) && !fs)) &&
                 (!fs || rect_is( &r, sw, sh )),
                 "SetFullscreenState and GetFullscreenState disagree, or the "
                 "window did not become the size of the screen" );
    }
    if (fs)
    {
        LONG sw = GetSystemMetrics( SM_CXSCREEN );
        LONG sh = GetSystemMetrics( SM_CYSCREEN );

        /* ResizeBuffers with 0x0 asks DXVK what size the window is rather than
         * telling it -- which is what an application does after a transition,
         * and which only answers correctly if the window really moved. */
        if (rtv) { ID3D11RenderTargetView_Release( rtv ); rtv = NULL; }
        ID3D11DeviceContext_OMSetRenderTargets( context, 0, NULL, NULL );
        IDXGISwapChain_ResizeBuffers( swapchain, 0, 0, 0, DXGI_FORMAT_UNKNOWN, desc.Flags );
        rebuild_rtv( device, swapchain, &rtv );
        begin( "phase 3: present fullscreen" );
        backbuffer_size( swapchain, &w, &h );
        out_size( "backbuffer", w, h );
        out( " " ); out_size( "screen", sw, sh );
#if FS_MODE_SWITCH
        out( " modeswitch=yes" );
#endif
        frames = rtv ? hold_phase( swapchain, context, rtv, 3 ) : 0;
        out( " frames=" ); out_dec( frames );
        verdict( frames > 0 && w == (UINT)sw && h == (UINT)sh,
                 "nothing was presented while fullscreen, or the back buffer is "
                 "not the size of the screen" );

#if FS_MODE_SWITCH
        /* THE MODE CLAIM.  With ALLOW_MODE_SWITCH the transition picks a mode
         * out of the output's own list, which now comes from Wine rather than
         * from the foreign driver's single synthetic one -- so on a display
         * with more than one size the screen must have CHANGED.  On a display
         * with exactly one it must not have, and saying so is the honest
         * answer rather than a failure. */
        begin( "phase 3: the display mode followed the transition" );
        out_size( "screen", sw, sh );
        out( " " ); out_size( "before", screen0_w, screen0_h );
        out( " sizes=" ); out_dec( mode_sizes );
        verdict( mode_sizes > 1 ? (sw != screen0_w || sh != screen0_h)
                                : (sw == screen0_w && sh == screen0_h),
                 "the display offered more than one mode and the fullscreen "
                 "transition did not change it" );
#endif
    }
    else
    {
        out( "fullscreen_smoke: PHASE3 REFUSED\n" );
        out( "fullscreen_smoke: PHASE3 READY\n" );   /* so the gate does not wait */
    }

    /* ---- phase 4: back to windowed --------------------------------------- */
    begin( "phase 4: SetFullscreenState(FALSE) puts everything back" );
    {
        LONG sw, sh;

        hr = IDXGISwapChain_SetFullscreenState( swapchain, FALSE, NULL );
        out_hr( "hr", hr );
        pump();
        /* NO SetWindowPos HERE, DELIBERATELY.  Restoring the window is DXGI's
         * job -- LeaveFullscreenMode calls wsi::restoreWindowState -- and a
         * probe that put the geometry back itself would pass a restore that
         * never happened, which is the failure that leaves a window stuck over
         * the whole screen. */
        if (rtv) { ID3D11RenderTargetView_Release( rtv ); rtv = NULL; }
        ID3D11DeviceContext_OMSetRenderTargets( context, 0, NULL, NULL );
        /* 0x0 again: the size must come from the window DXGI restored. */
        hr = IDXGISwapChain_ResizeBuffers( swapchain, 0, 0, 0, DXGI_FORMAT_UNKNOWN, desc.Flags );
        if (SUCCEEDED(hr)) hr = rebuild_rtv( device, swapchain, &rtv );
        backbuffer_size( swapchain, &w, &h );
        GetWindowRect( hwnd, &r );
        sw = GetSystemMetrics( SM_CXSCREEN );
        sh = GetSystemMetrics( SM_CYSCREEN );
        out( " " ); out_size( "backbuffer", w, h );
        out( " " ); out_rect( "winrect", &r );
        out( " " ); out_rect( "wanted", &windowed_rect );
        out( " " ); out_size( "screen", sw, sh );
        verdict( SUCCEEDED(hr) && rtv &&
                 w == PHASE2_W && h == PHASE2_H &&
                 rect_same( &r, &windowed_rect ) &&
                 sw == screen0_w && sh == screen0_h,
                 "leaving fullscreen did not restore the window rectangle, the "
                 "back buffer size, or the display mode" );
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
     * it by accident.  The explicit ChangeDisplaySettingsExW after it is the
     * belt to that braces: if this probe died between a mode change and a
     * transition, the display is still owed its mode back and nothing else in
     * the process is going to ask. */
    if (swapchain) IDXGISwapChain_SetFullscreenState( swapchain, FALSE, NULL );
    if (rtv) ID3D11RenderTargetView_Release( rtv );
    if (swapchain) IDXGISwapChain_Release( swapchain );
    if (context) ID3D11DeviceContext_Release( context );
    if (device) ID3D11Device_Release( device );
    if (hwnd) DestroyWindow( hwnd );
    ChangeDisplaySettingsExW( NULL, NULL, NULL, 0, NULL );
    out( "fullscreen_smoke: FINAL " );
    out_size( "screen", GetSystemMetrics( SM_CXSCREEN ),
              GetSystemMetrics( SM_CYSCREEN ) );
    out( "\n" );

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
