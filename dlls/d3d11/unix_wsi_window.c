/*
 * The win32u WINDOW seam: the abi-2 half of DXVK's WSI callback table.
 *
 * dlls/d3d12/unix_win32u.c is the seam that turns an HWND into a presentable
 * VkSurfaceKHR.  This is the seam that MOVES the HWND, and it exists because
 * the premise underneath DXVK's foreign WSI driver is false for this backend.
 * That driver makes every call which would move, resize, restack or restyle a
 * window a no-op reporting success, and its comment says why: the window
 * belongs to somebody else.  True for the foreign-X11 backend, which is handed
 * a raw XID owned by another process.  FALSE for the Win32u backend, whose
 * window is a Wine HWND that Wine created and can move.  [MEASURED] 2026-08-18,
 * the test machine: IDXGISwapChain::SetFullscreenState(TRUE) returned S_OK,
 * GetFullscreenState agreed, GetSystemMetrics reported the whole screen, and
 * the rectangle on screen was still the 192x144 the window had been resized to.
 *
 * WHY THIS IS A FILE AND NOT TWO COPIES.  Both DXVK lanes publish this table --
 * dlls/d3d11/unix.c for D3D11/DXGI/D3D10 and dlls/d3d9/unix.c for D3D9 -- and
 * both need every operation here.  dlls/d3d9/unix_wsi_window.c is one line that
 * includes this file, exactly as dlls/d3d11/unix_win32u.c is one line that
 * includes the d3d12 lane's.  The d3d12 lane does NOT compile this one: vkd3d
 * drives its own presentation and asks Wine for no window geometry, so giving
 * its unixlib two hundred lines of dead code and a second reason to resolve
 * win32u entry points would be a cost with no answer.
 *
 * WHY dlsym AND NOT -lwin32u.  Verbatim from dlls/d3d12/unix_win32u.c, for the
 * identical measured reason: linking win32u gives this unixlib a DT_NEEDED the
 * build-tree layout cannot satisfy in a headless process, and these libraries
 * must keep loading there.  A process that owns a window has win32u.so loaded
 * by construction, so RTLD_NOLOAD either finds it or correctly reports that
 * there is no window system to talk to.
 *
 * THE ENTRY POINTS ARE THE ONES A GRAPHICS DRIVER USES.  NtUserSetWindowPos,
 * NtUserSetWindowLong, NtUserEnumDisplaySettings and NtUserChangeDisplaySettings
 * are what winex11.drv and winewayland.drv call from their own unix halves;
 * this is the same layer, reached the same way.  The two NtUserCall* dispatchers
 * are here because the helpers in include/ntuser.h that wrap them
 * (NtUserGetWindowRect, NtUserGetWindowLongW, NtUserGetClientRect) are static
 * inlines that name the imports directly, which is precisely the DT_NEEDED this
 * file is avoiding -- so their two-line bodies are reproduced against a
 * resolved pointer instead, and the parameter structs and call codes still come
 * from ntuser.h so there is nothing here to drift.
 *
 * EVERY CALL BELOW ENDS IN win32u AND MAY ONLY BE MADE FROM A WINE THREAD, and
 * one of them -- NtUserSetWindowPos -- goes further than any call this lane had
 * made before: it sends the window WM_WINDOWPOSCHANGING and WM_WINDOWPOSCHANGED,
 * so win32u calls back out to a window procedure, which on this port can be
 * GUEST x86-64 code behind an interception trampoline.  That is not a new road.
 * It is the road winex11.drv takes every time an X event moves a window, and
 * the one dlls/user32's callback dispatcher already serves for guests.  What is
 * new is entering it from inside a unixlib call rather than from inside a
 * message wait, so the thread rule is asserted here rather than assumed: the
 * flag below is set by every unixlib entry point in the module that links this
 * file, and a callback arriving without it is refused by name.
 *
 * Copyright 2026 the ppc64le port authors
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 */

#if 0
#pragma makedep unix
#endif

#include "config.h"

#include <stdarg.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <dlfcn.h>
#include <pthread.h>

#include "ntstatus.h"
#define WIN32_NO_STATUS
#include "windef.h"
#include "winbase.h"
#include "wingdi.h"
#include "winuser.h"
#include "winternl.h"
#include "ntuser.h"
#include "wine/debug.h"

#include "dxvk_win32u_wsi.h"

/* Spelled out rather than macro-pasted, for the reason dlls/d3d12/unix_win32u.c
 * gives: WINE_DEFAULT_DEBUG_CHANNEL stringifies its argument, so a macro handed
 * to it would name a channel no WINEDEBUG spelling could switch on. */
#if defined(HWNDSURF_CHANNEL_D3D9)
WINE_DEFAULT_DEBUG_CHANNEL(d3d9);
#else
WINE_DEFAULT_DEBUG_CHANNEL(d3d11);
#endif

/* THE ONE PLACE THAT KNOWS THIS IS A WINE THREAD, for the module that links
 * this file.  It used to be a static in dlls/d3d11/unix.c and dlls/d3d9/unix.c;
 * it lives here now because the operations that most need it live here, and
 * because two definitions of the same rule is how the rule stops being one.
 * Each unixlib still SETS it, from its own WINE_THREAD_ENTRY wrapper -- there
 * is one copy of this object per .so, which is what each of them wants. */
__thread int wsi_on_wine_thread;

/* ---------------------------------------------------------------------------
 * The constants the callback ABI and winuser.h both have to agree on.
 *
 * ppc64le/dxvk/dxvk_win32u_wsi.h spells them because DXVK's native Windows shim
 * (src/include/native/windows) declares none of them, so the DXVK side cannot
 * say WS_OVERLAPPEDWINDOW even where that is exactly what it means.  These
 * asserts are the other end of that bargain: if a Windows header ever
 * disagreed, the build stops here rather than a fullscreen window coming back
 * with its title bar stripped off for good.
 */
C_ASSERT( DXVK_WIN32U_WS_VISIBLE             == WS_VISIBLE );
C_ASSERT( DXVK_WIN32U_WS_OVERLAPPEDWINDOW    == WS_OVERLAPPEDWINDOW );
C_ASSERT( DXVK_WIN32U_WS_EX_TOPMOST          == WS_EX_TOPMOST );
C_ASSERT( DXVK_WIN32U_WS_EX_OVERLAPPEDWINDOW == WS_EX_OVERLAPPEDWINDOW );

/* --------------------------------------------------------------------------
 * win32u's own entry points, resolved rather than linked.
 */
static BOOL      (WINAPI *p_NtUserSetWindowPos)( HWND, HWND, INT, INT, INT, INT, UINT );
static LONG      (WINAPI *p_NtUserSetWindowLong)( HWND, INT, LONG, BOOL );
static ULONG_PTR (WINAPI *p_NtUserCallHwndParam)( HWND, UINT_PTR, ULONG );
static ULONG_PTR (WINAPI *p_NtUserCallTwoParam)( ULONG_PTR, ULONG_PTR, ULONG );
static BOOL      (WINAPI *p_NtUserEnumDisplaySettings)( UNICODE_STRING *, DWORD, DEVMODEW *, DWORD );
static LONG      (WINAPI *p_NtUserChangeDisplaySettings)( UNICODE_STRING *, DEVMODEW *, HWND, DWORD, void * );

static int wsiwin_bound;
static pthread_once_t wsiwin_once = PTHREAD_ONCE_INIT;

static void wsiwin_init_once( void )
{
    static const struct { const char *name; void **slot; } wanted[] =
    {
        { "NtUserSetWindowPos",           (void **)&p_NtUserSetWindowPos },
        { "NtUserSetWindowLong",          (void **)&p_NtUserSetWindowLong },
        { "NtUserCallHwndParam",          (void **)&p_NtUserCallHwndParam },
        { "NtUserCallTwoParam",           (void **)&p_NtUserCallTwoParam },
        { "NtUserEnumDisplaySettings",    (void **)&p_NtUserEnumDisplaySettings },
        { "NtUserChangeDisplaySettings",  (void **)&p_NtUserChangeDisplaySettings },
    };
    unsigned int i;
    void *win32u;

    if (!(win32u = dlopen( "win32u.so", RTLD_NOW | RTLD_NOLOAD )))
    {
        ERR( "win32u.so is not loaded in this process; a swapchain here has no "
             "window to move, so every fullscreen transition will be refused\n" );
        return;
    }
    for (i = 0; i < ARRAYSIZE(wanted); i++)
    {
        if ((*wanted[i].slot = dlsym( win32u, wanted[i].name ))) continue;
        /* All or nothing.  Half a table is worse than none: DXVK's driver
         * falls back to the inherited no-op for whatever is missing, so a
         * partial bind produces a window that moves but never restyles, and
         * the symptom is a frame that is the right size in the wrong place. */
        ERR( "win32u.so exports no %s; the window operations are unavailable "
             "in this process\n", wanted[i].name );
        p_NtUserSetWindowPos = NULL;
        p_NtUserSetWindowLong = NULL;
        p_NtUserCallHwndParam = NULL;
        p_NtUserCallTwoParam = NULL;
        p_NtUserEnumDisplaySettings = NULL;
        p_NtUserChangeDisplaySettings = NULL;
        return;
    }
    wsiwin_bound = 1;
    TRACE( "bound %u win32u window entry points\n", (unsigned int)ARRAYSIZE(wanted) );
}

/* Every operation opens with this.  Two questions, and they fail differently:
 * a table that could not be bound is a process without a window system, and a
 * call off a Wine thread is DXVK asking from one of its own threads, which has
 * no TEB for win32u to dereference.
 *
 * `what` NULL asks the same question SILENTLY, and there is one caller that
 * legitimately wants that: answering DXVK's window_size question is allowed to
 * fall back to the size the PE side pushed down, so a query arriving on a DXVK
 * thread there is a fallback rather than a fault, and saying so would be noise
 * in every trace that ever hit it. */
static int wsiwin_ready( const char *what )
{
    pthread_once( &wsiwin_once, wsiwin_init_once );
    if (!wsiwin_bound) return 0;
    if (!wsi_on_wine_thread)
    {
        static int warned;

        if (what && !warned++)
            ERR( "refusing %s off a Wine thread: this call ends in win32u, "
                 "which dereferences a TEB that a DXVK worker thread does not "
                 "have.  DXVK asked for a window operation from somewhere other "
                 "than the application's own call into this module -- report it "
                 "with a +d3d11 trace; the transition will report failure "
                 "rather than fault.\n", what );
        return 0;
    }
    return 1;
}

/* --------------------------------------------------------------------------
 * The six operations.
 */

static int32_t wsiwin_window_rect( uint64_t hwnd, int32_t *rect )
{
    struct get_window_rects_params params;
    RECT r = { 0, 0, 0, 0 };

    if (!rect || !wsiwin_ready( "a window rectangle query" )) return 0;
    params.rect = &r;
    params.dpi = 0;
    if (!p_NtUserCallHwndParam( (HWND)(ULONG_PTR)hwnd, (UINT_PTR)&params,
                                NtUserCallHwndParam_GetWindowRect ))
        return 0;
    rect[0] = r.left;
    rect[1] = r.top;
    rect[2] = r.right;
    rect[3] = r.bottom;
    TRACE( "hwnd %p rect %s\n", (void *)(ULONG_PTR)hwnd, wine_dbgstr_rect( &r ) );
    return 1;
}

static int32_t wsiwin_window_set_pos( uint64_t hwnd, int32_t x, int32_t y,
                                      int32_t cx, int32_t cy, uint32_t flags )
{
    HWND after = NULL;
    UINT f = 0;
    BOOL ok;

    if (!wsiwin_ready( "a window move" )) return 0;

    /* Our flag word, not user32's -- the ABI header says why it is ours, and
     * this is the one place that knows both spellings. */
    if (flags & DXVK_WIN32U_SWP_NOSIZE)       f |= SWP_NOSIZE;
    if (flags & DXVK_WIN32U_SWP_NOMOVE)       f |= SWP_NOMOVE;
    if (flags & DXVK_WIN32U_SWP_NOACTIVATE)   f |= SWP_NOACTIVATE;
    if (flags & DXVK_WIN32U_SWP_FRAMECHANGED) f |= SWP_FRAMECHANGED;
    if (flags & DXVK_WIN32U_SWP_SHOWWINDOW)   f |= SWP_SHOWWINDOW;

    if (flags & DXVK_WIN32U_SWP_TOPMOST)         after = HWND_TOPMOST;
    else if (flags & DXVK_WIN32U_SWP_NOTOPMOST)  after = HWND_NOTOPMOST;
    else                                         f |= SWP_NOZORDER;

    /* CLIENTSIZE means cx/cy are what the CLIENT area has to end up being, so
     * grow them by this window's own frame first.  Only the Wine side can do
     * this: the adjustment needs the window's current style and exstyle, which
     * are the very things DXVK cannot read for itself. */
    if ((flags & DXVK_WIN32U_SWP_CLIENTSIZE) && !(flags & DXVK_WIN32U_SWP_NOSIZE))
    {
        struct adjust_window_rect_params params;
        RECT r;

        r.left = 0;
        r.top = 0;
        r.right = cx;
        r.bottom = cy;
        params.style = (DWORD)p_NtUserCallHwndParam( (HWND)(ULONG_PTR)hwnd, GWL_STYLE,
                                                     NtUserCallHwndParam_GetWindowLongW );
        params.ex_style = (DWORD)p_NtUserCallHwndParam( (HWND)(ULONG_PTR)hwnd, GWL_EXSTYLE,
                                                        NtUserCallHwndParam_GetWindowLongW );
        params.menu = FALSE;
        params.dpi = 0;
        if (p_NtUserCallTwoParam( (ULONG_PTR)&r, (ULONG_PTR)&params,
                                  NtUserCallTwoParam_AdjustWindowRect ))
        {
            cx = r.right - r.left;
            cy = r.bottom - r.top;
        }
    }

    ok = p_NtUserSetWindowPos( (HWND)(ULONG_PTR)hwnd, after, x, y, cx, cy, f );
    TRACE( "hwnd %p (%d,%d) %dx%d flags %#x -> %u\n", (void *)(ULONG_PTR)hwnd,
           x, y, cx, cy, flags, ok );
    return ok != 0;
}

static int64_t wsiwin_window_style( uint64_t hwnd, int32_t which, int32_t set,
                                    int64_t value )
{
    INT offset = (which == DXVK_WIN32U_EXSTYLE) ? GWL_EXSTYLE : GWL_STYLE;
    LONG old;

    if (!wsiwin_ready( "a window style change" )) return 0;
    old = (LONG)p_NtUserCallHwndParam( (HWND)(ULONG_PTR)hwnd, offset,
                                       NtUserCallHwndParam_GetWindowLongW );
    /* ansi = FALSE: the W form, because the only thing this touches is the
     * style bits and the A/W distinction there is about which window procedure
     * the handle carries, which is not ours to change. */
    if (set) p_NtUserSetWindowLong( (HWND)(ULONG_PTR)hwnd, offset, (LONG)value, FALSE );
    TRACE( "hwnd %p %s %#x%s\n", (void *)(ULONG_PTR)hwnd,
           offset == GWL_EXSTYLE ? "exstyle" : "style", (unsigned int)old,
           set ? wine_dbg_sprintf( " -> %#x", (unsigned int)value ) : "" );
    return old;
}

static int32_t wsiwin_display_mode( int32_t index, struct dxvk_win32u_wsi_mode *mode )
{
    DEVMODEW dm;
    DWORD which;

    if (!mode || !wsiwin_ready( "a display mode query" )) return 0;
    if (index >= 0)                             which = (DWORD)index;
    else if (index == DXVK_WIN32U_MODE_CURRENT) which = ENUM_CURRENT_SETTINGS;
    else                                        which = ENUM_REGISTRY_SETTINGS;

    memset( &dm, 0, sizeof(dm) );
    dm.dmSize = sizeof(dm);
    /* device NULL is the primary adapter.  This lane reports exactly one
     * synthetic monitor to DXGI (ppc64le/dxvk/dxvk-patches/0001), so there is
     * one display to name and naming it by string would be a second way to get
     * the same answer wrong. */
    if (!p_NtUserEnumDisplaySettings( NULL, which, &dm, 0 )) return 0;
    if (!dm.dmPelsWidth || !dm.dmPelsHeight) return 0;

    mode->width          = dm.dmPelsWidth;
    mode->height         = dm.dmPelsHeight;
    mode->bits_per_pixel = (dm.dmFields & DM_BITSPERPEL) ? dm.dmBitsPerPel : 32;
    mode->refresh_rate   = (dm.dmFields & DM_DISPLAYFREQUENCY) ? dm.dmDisplayFrequency : 0;
    mode->interlaced     = (dm.dmFields & DM_DISPLAYFLAGS) &&
                           (dm.dmDisplayFlags & DM_INTERLACED);
    return 1;
}

static int32_t wsiwin_display_set_mode( const struct dxvk_win32u_wsi_mode *mode )
{
    DEVMODEW dm;
    LONG rc;

    if (!mode || !wsiwin_ready( "a display mode change" )) return 0;

    memset( &dm, 0, sizeof(dm) );
    dm.dmSize        = sizeof(dm);
    dm.dmFields      = DM_PELSWIDTH | DM_PELSHEIGHT | DM_BITSPERPEL;
    dm.dmPelsWidth   = mode->width;
    dm.dmPelsHeight  = mode->height;
    dm.dmBitsPerPel  = mode->bits_per_pixel ? mode->bits_per_pixel : 32;
    if (mode->refresh_rate)
    {
        dm.dmFields |= DM_DISPLAYFREQUENCY;
        dm.dmDisplayFrequency = mode->refresh_rate;
    }

    rc = p_NtUserChangeDisplaySettings( NULL, &dm, NULL, CDS_FULLSCREEN, NULL );
    if (rc != DISP_CHANGE_SUCCESSFUL && (dm.dmFields & DM_DISPLAYFREQUENCY))
    {
        /* The same retry DXVK's own Win32 backend makes, and for the same
         * reason: a driver that has the size but not that exact refresh rate
         * refuses the whole request, and the size is the part the application
         * asked for. */
        dm.dmFields &= ~DM_DISPLAYFREQUENCY;
        rc = p_NtUserChangeDisplaySettings( NULL, &dm, NULL, CDS_FULLSCREEN, NULL );
    }
    TRACE( "%ux%u %ubpp @%u -> %d\n", mode->width, mode->height,
           mode->bits_per_pixel, mode->refresh_rate, (int)rc );
    return rc == DISP_CHANGE_SUCCESSFUL;
}

static int32_t wsiwin_display_restore_mode( void )
{
    LONG rc;

    if (!wsiwin_ready( "a display mode restore" )) return 0;
    /* devmode NULL is "whatever the registry says this display should be",
     * which is what user32's ChangeDisplaySettingsEx(NULL, NULL, ...) means
     * and what every application's own cleanup path calls. */
    rc = p_NtUserChangeDisplaySettings( NULL, NULL, NULL, 0, NULL );
    TRACE( "-> %d\n", (int)rc );
    return rc == DISP_CHANGE_SUCCESSFUL;
}

/* --------------------------------------------------------------------------
 * Not an ABI entry: the client size, asked directly.
 *
 * dlls/d3d11/unix.c answers DXVK's window_size question from a size the PE side
 * PUSHES down, because the unix side had no way to ask user32 anything.  It has
 * one now, and this is it: the pushed size becomes the FALLBACK rather than the
 * answer, which closes a hole that opens the moment DXVK can move a window
 * itself.  IDXGISwapChain::ResizeBuffers(0, 0, ...) immediately after a
 * fullscreen transition -- exactly what an application does there -- asks DXVK
 * for the window's size, and the PE side has had no reason to push a new one
 * since before the transition.  The back buffer would be sized from the
 * window's PREVIOUS size and DXVK would scale it into the fullscreen window:
 * right size on screen, wrong size in the buffer, which is precisely the
 * failure ppc64le/dxvk/check-fullscreen-smoke.sh's first negative control
 * exists to catch.
 *
 * Silent when it cannot answer, for the reason wsiwin_ready() gives.
 */
int wsiwin_client_size( uint64_t hwnd, unsigned int *width, unsigned int *height )
{
    struct get_window_rects_params params;
    RECT r = { 0, 0, 0, 0 };

    if (!wsiwin_ready( NULL )) return 0;
    params.rect = &r;
    params.dpi = 0;
    if (!p_NtUserCallHwndParam( (HWND)(ULONG_PTR)hwnd, (UINT_PTR)&params,
                                NtUserCallHwndParam_GetClientRect ))
        return 0;
    *width  = r.right - r.left;
    *height = r.bottom - r.top;
    return *width && *height;
}

/* --------------------------------------------------------------------------
 * Publishing, and the negative control.
 *
 * WINEDXVKNOWINDOWOPS=1 leaves all six entries NULL, which is not a debugging
 * convenience: it is the lever ppc64le/dxvk/check-fullscreen-smoke.sh
 * --sabotage pulls.  DXVK's Win32u driver checks every one of these before
 * calling it and falls back to the inherited no-op, so with the lever set the
 * port behaves EXACTLY as it did before this ABI existed -- S_OK from
 * SetFullscreenState, agreement from GetFullscreenState, and a rectangle that
 * never moves.  A gate whose only negative control turned off its own
 * assertions would prove nothing about this code; this one turns off the code.
 */
void wsiwin_ops_init( struct dxvk_win32u_wsi_ops *ops )
{
    const char *off = getenv( "WINEDXVKNOWINDOWOPS" );

    if (off && *off != '0')
    {
        ERR( "WINEDXVKNOWINDOWOPS is set: publishing the WSI table with no "
             "window operations.  Exclusive fullscreen, display-mode changes "
             "and window resizes driven by DXVK will all report success and do "
             "nothing, which is what this port did before abi 2.  Unset it "
             "unless you are running a negative control.\n" );
        return;
    }

    ops->window_rect          = wsiwin_window_rect;
    ops->window_set_pos       = wsiwin_window_set_pos;
    ops->window_style         = wsiwin_window_style;
    ops->display_mode         = wsiwin_display_mode;
    ops->display_set_mode     = wsiwin_display_set_mode;
    ops->display_restore_mode = wsiwin_display_restore_mode;
}
