/*
 * dxvk_win32u_wsi.h -- the ABI between DXVK's win32u WSI backend and Wine.
 *
 * THIS FILE HAS ONE COPY, and both sides of the boundary compile against it:
 * the DXVK side from src/wsi/foreign/wsi_win32u_foreign.cpp (through a
 * relative include -- the file lives in the gitignored upstream checkout, so
 * the declaration cannot live there), and the Wine side from
 * dlls/d3d11/unix.c.  It is the same rule interfaces_dxvk.json is carried
 * under, for the same reason: a second copy that drifted would call the
 * neighbouring function pointer with the neighbour's argument types, and
 * nothing downstream would catch it -- these are raw C function pointers with
 * no vtable, no IID, and no runtime cross-check behind them.  `abi` below is
 * the one check there is.
 *
 * WHY A CALLBACK TABLE AND NOT A COMPILE-TIME BACKEND.  DXVK's WSI is a
 * compile-time backend selection (src/wsi/wsi_platform.cpp picks one
 * WsiBootstrap from a static array by name).  None of the shipped backends can
 * be the right one here: the window belongs to Wine, its handle is a Wine HWND
 * that means nothing to X11 or Wayland directly, and the VkSurfaceKHR that
 * presents to it has to come from win32u's client-surface machinery -- the
 * layer winevulkan uses, so winex11's child client window and winewayland's
 * wl_subsurface are both served by construction.  The standalone project's
 * foreign-X11 design (a second X connection naming the guest's XID) is a
 * documented dead end under Wayland.  So the backend is real but empty, and
 * Wine fills it in at load time by registering this table.
 *
 * EVERYTHING CROSSES AS A PLAIN INTEGER OR A void*, deliberately.  DXVK's
 * VkSurfaceKHR is a pointer-shaped dispatchable-handle typedef and Wine's is a
 * UINT64; VkInstance and PFN_vkGetInstanceProcAddr belong to two Vulkan header
 * families that must not meet.  dlls/d3d12/unix_win32u.c exists for exactly
 * that reason and says so at the top; this table keeps the same discipline one
 * layer further out.
 *
 * EVERY CALL ARRIVES ON THE THREAD DXVK WAS CALLED ON, and every one of them
 * ends in win32u, which may only be entered from a Wine thread.  The Wine
 * side asserts that (see the wine_thread flag in dlls/d3d11/unix.c) rather
 * than trusting it: DXVK owns a CS thread and a submission thread, neither of
 * which has a TEB, and a win32u call from one of those is not a wrong answer,
 * it is a fault in somebody else's stack frame.
 *
 * THE WINDOW OPERATIONS (abi 2) EXIST BECAUSE THE PREMISE UNDER THEM CHANGED.
 * The Win32u driver derives from DXVK's foreign driver, which makes every call
 * that would move, resize, restack or restyle a window a no-op reporting
 * success, and says why in a comment: the window belongs to somebody else.
 * That is right for the foreign-X11 backend, handed a raw XID owned by another
 * process.  It is FALSE here -- a Win32u window is a Wine HWND that Wine
 * itself created and can move -- and inheriting it unchanged is what made
 * IDXGISwapChain::SetFullscreenState(TRUE) return S_OK, GetFullscreenState
 * agree, and the rectangle on screen never move.  [MEASURED] 2026-08-18, op4k:
 * 192x144 before the call and 192x144 after it, on a 640x480 screen.
 *
 * So the six calls below are the road that did not exist: the geometry and
 * display-mode operations a Wine HWND can genuinely serve, each one landing in
 * win32u the way a graphics driver's would.  They are appended rather than
 * interleaved, so the five that were here keep their offsets and their
 * meanings, and only the `abi` number says the table grew.
 *
 * Copyright 2026 the ppc64le port authors
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 */

#ifndef __PPC64LE_DXVK_WIN32U_WSI_H
#define __PPC64LE_DXVK_WIN32U_WSI_H

#include <stdint.h>

/* Bumped whenever anything below changes shape.  The registration entry point
 * refuses a table whose abi it does not recognise and says so, because the
 * alternative -- a library built against one layout calling a table laid out
 * for another -- is a jump through a pointer read from the wrong offset.
 *
 * 1 -> 2 appended the six window and display-mode operations.  The check is an
 * EXACT match rather than "at least", and that is the whole cost of the bump:
 * a libdxvk built from the three-patch series will refuse a table stamped 2,
 * and this tree's DXVK will refuse one stamped 1, in both cases by name and
 * with the bootstrap.sh line to run.  A `>=` check would be friendlier and
 * would be wrong -- an older library accepting a newer table cannot know that
 * the entries it does not read are the ones the caller is relying on, and the
 * failure would then be a swapchain that reports fullscreen and does not go
 * there, which is the exact bug this bump exists to fix. */
#define DXVK_WIN32U_WSI_ABI 2

#ifdef __cplusplus
extern "C" {
#endif

/* THE WIN32 CONSTANTS BOTH SIDES HAVE TO AGREE ON, SPELLED HERE.
 *
 * DXVK's native build compiles against its own tiny Windows shim
 * (src/include/native/windows), which declares RECT and HWND and none of
 * these: no WS_*, no SWP_*, no GWL_*.  So the DXVK side cannot say
 * WS_OVERLAPPEDWINDOW even though that is exactly what it means, and the
 * choice is between a magic number in the patch series and a name here.  This
 * file is where the two sides already agree about everything else, so the
 * names live here -- and the Wine side C_ASSERTs each one against the real
 * winuser.h value, so a drift is a compile error in dlls/d3d11/unix.c rather
 * than a window that comes back from fullscreen with its title bar missing.
 *
 * The SWP flags are OURS and are not user32's numbers: window_set_pos takes
 * this set and the Wine side translates.  That is deliberate -- a flag word
 * read out of one header family and passed into the other's namespace is the
 * one class of mistake this whole file exists to prevent, and there is no
 * reason for the wire form to be user32's bit assignment. */
#define DXVK_WIN32U_SWP_NOSIZE          0x0001
#define DXVK_WIN32U_SWP_NOMOVE          0x0002
#define DXVK_WIN32U_SWP_NOACTIVATE      0x0004
#define DXVK_WIN32U_SWP_NOZORDER        0x0008
#define DXVK_WIN32U_SWP_FRAMECHANGED    0x0010
#define DXVK_WIN32U_SWP_SHOWWINDOW      0x0020
#define DXVK_WIN32U_SWP_TOPMOST         0x0040   /* insert after HWND_TOPMOST */
#define DXVK_WIN32U_SWP_NOTOPMOST       0x0080   /* insert after HWND_NOTOPMOST */
/* cx/cy are the CLIENT size wanted, not the outer one: the Wine side grows the
 * rectangle by the window's own frame first.  D3D9's Reset resizes to a client
 * size, and on a decorated window the difference is the border and the title
 * bar -- a swapchain the height of a caption smaller than the one the
 * application asked for, which reads as a scaling bug and is not one.  It is a
 * flag rather than a seventh entry point because the adjustment needs the
 * window's current style, which only the Wine side can read anyway. */
#define DXVK_WIN32U_SWP_CLIENTSIZE      0x0100

/* window_style's first selector: which LONG is being read or written. */
#define DXVK_WIN32U_STYLE               0        /* GWL_STYLE */
#define DXVK_WIN32U_EXSTYLE             1        /* GWL_EXSTYLE */

/* The style BITS are Win32's own, because they are what the value read back
 * through window_style is made of.  Asserted against winuser.h on the Wine
 * side; see the note above. */
#define DXVK_WIN32U_WS_VISIBLE              0x10000000
#define DXVK_WIN32U_WS_OVERLAPPEDWINDOW     0x00cf0000
#define DXVK_WIN32U_WS_EX_TOPMOST           0x00000008
#define DXVK_WIN32U_WS_EX_OVERLAPPEDWINDOW  0x00000300

/* display_mode's index, when it does not mean "the nth mode". */
#define DXVK_WIN32U_MODE_CURRENT        (-1)     /* ENUM_CURRENT_SETTINGS */
#define DXVK_WIN32U_MODE_REGISTRY       (-2)     /* ENUM_REGISTRY_SETTINGS */

/* One display mode, flattened out of DEVMODEW.
 *
 * Five plain integers rather than a DEVMODEW pointer, for the reason the
 * banner gives about VkSurfaceKHR: DEVMODEW is a 220-byte Windows structure
 * whose layout DXVK's shim does not describe at all, and the five fields DXGI
 * cares about are these.  `refresh_rate` is whole Hz because that is what
 * dmDisplayFrequency is; DXVK's WsiRational denominator is always 1 on this
 * path and dxgi_output.cpp's ConvertDisplayMode never reads a finer one out of
 * a Win32 driver either. */
struct dxvk_win32u_wsi_mode
{
    uint32_t width;
    uint32_t height;
    uint32_t bits_per_pixel;
    uint32_t refresh_rate;      /* Hz; 0 when the driver does not report one */
    uint32_t interlaced;
};

struct dxvk_win32u_wsi_ops
{
    uint32_t abi;

    /* The instance extensions the caller's VkInstance must enable for
     * surface_create to be able to answer.  win32u's seam resolves
     * vkCreateXlibSurfaceKHR AND vkCreateWaylandSurfaceKHR from the caller's
     * instance and needs at least one of them, and which one it needs is
     * decided by the display driver Wine loaded -- not by anything DXVK can
     * see.  So the Wine side answers with every platform surface extension
     * the system loader actually offers, and the session decides at runtime.
     * Returns a NULL-terminated array of names owned by the callee. */
    const char *const *(*instance_extensions)( void );

    /* Create a presentable VkSurfaceKHR for `hwnd` on the caller's instance.
     * `instance` is a VkInstance, `gipa` the caller's vkGetInstanceProcAddr
     * (both void* here on purpose -- see the banner).  Returns a VkResult;
     * the surface is written as a UINT64 because that is what Wine's
     * VkSurfaceKHR is and what DXVK's pointer-shaped one fits in. */
    int32_t (*surface_create)( uint64_t hwnd, void *instance, void *gipa,
                               uint64_t *surface );

    /* Release the win32u client surface behind `surface`.  The VkSurfaceKHR
     * itself belongs to DXVK and is destroyed by DXVK; this releases only
     * Wine's side of it, and is a no-op for a surface this table never
     * created (DXVK also destroys surfaces made by other backends when the
     * driver is switched mid-process by DXVK_WSI_DRIVER). */
    void (*surface_destroy)( uint64_t surface );

    /* The window's client size.  Non-zero on success; on failure DXVK falls
     * back to its synthetic screen size, which is what the headless backend
     * has always reported. */
    int32_t (*window_size)( uint64_t hwnd, uint32_t *width, uint32_t *height );

    /* Whether `hwnd` still names a live window. */
    int32_t (*is_window)( uint64_t hwnd );

    /* ---------------------------------------------------------------- abi 2
     *
     * Everything below is NULLABLE, and the DXVK side checks every one before
     * calling it.  Not defensiveness for its own sake: WINEDXVKNOWINDOWOPS=1
     * publishes this table with exactly these entries NULL, which is the
     * negative control ppc64le/dxvk/check-fullscreen-smoke.sh --sabotage runs
     * -- the port then behaves precisely as it did before this ABI existed,
     * and the gate must go red.  A lever that only turned off the gate's own
     * assertions would prove nothing about the code. */

    /* The window's OUTER rectangle in screen coordinates -- what
     * NtUserGetWindowRect answers, not the client area window_size reports.
     * `rect` is four ints in the order left, top, right, bottom.  Non-zero on
     * success.  This is what a swapchain saves before it goes fullscreen and
     * puts back afterwards, so it has to be the outer one: restoring a client
     * rect onto a decorated window walks the frame down the screen by the
     * height of its own title bar, once per transition. */
    int32_t (*window_rect)( uint64_t hwnd, int32_t *rect );

    /* Move, resize and restack.  `flags` is the DXVK_WIN32U_SWP_* set above,
     * which the Wine side translates into user32's; x/y/cx/cy are ignored for
     * whichever of NOMOVE and NOSIZE is set.  Non-zero on success.
     *
     * This is the one call the whole ABI bump is for.  It ends in
     * NtUserSetWindowPos, which sends the window its WM_WINDOWPOSCHANGING and
     * WM_WINDOWPOSCHANGED -- to a window procedure that on this port may be
     * GUEST x86-64 code behind a trampoline.  That is not a new road: it is
     * the same one winex11.drv and winewayland.drv take every time an event
     * moves a window, and the same one dlls/user32's callback dispatcher
     * already serves.  It does mean the call must be on a Wine thread, which
     * the flag in dlls/d3d11/unix.c asserts rather than assumes. */
    int32_t (*window_set_pos)( uint64_t hwnd, int32_t x, int32_t y,
                               int32_t cx, int32_t cy, uint32_t flags );

    /* Read (set = 0) or replace (set = 1) GWL_STYLE or GWL_EXSTYLE, selected
     * by DXVK_WIN32U_STYLE / DXVK_WIN32U_EXSTYLE.  Returns the value BEFORE
     * any change, which is what a caller saving state wants; on failure it
     * returns 0, which is not a style any live window has.
     *
     * Fullscreen needs this and not only the geometry.  A window that keeps
     * WS_OVERLAPPEDWINDOW while its outer rect is the whole screen has a title
     * bar and a border ON the screen, and a CLIENT area smaller than the
     * screen by exactly their thickness -- so the swapchain is the wrong size
     * and the frame is offset, both of which look like a presentation bug and
     * are neither. */
    int64_t (*window_style)( uint64_t hwnd, int32_t which, int32_t set,
                             int64_t value );

    /* One display mode by index, or the current or registry one (the
     * DXVK_WIN32U_MODE_* selectors).  Non-zero on success; zero on an index
     * past the end, which is how DxgiOutput::GetDisplayModeList1 knows where
     * the list stops -- it calls with an increasing index until this says no.
     *
     * Without this DXGI sees exactly ONE mode, the foreign driver's synthetic
     * one, and FindClosestMatchingMode1 can only ever return the mode the
     * screen is already in.  A fullscreen transition then asks for the mode it
     * already has, and no ChangeDisplaySettings ever happens -- which is why
     * that call was recorded as unproven rather than broken. */
    int32_t (*display_mode)( int32_t index, struct dxvk_win32u_wsi_mode *mode );

    /* Ask for a mode.  Non-zero means the display took it (Wine's
     * DISP_CHANGE_SUCCESSFUL); anything else is a refusal and DXGI turns it
     * into DXGI_ERROR_NOT_CURRENTLY_AVAILABLE, which is the honest answer. */
    int32_t (*display_set_mode)( const struct dxvk_win32u_wsi_mode *mode );

    /* Put the display back the way the registry says it should be -- Wine's
     * ChangeDisplaySettingsExW(NULL, NULL, ...).  Called when a swapchain
     * leaves fullscreen and when the last one goes away, so a process that
     * dies mid-frame does not leave somebody's desktop in a mode nobody asked
     * for.  Non-zero on success. */
    int32_t (*display_restore_mode)( void );
};

/* The registration entry point each DXVK library exports.  It is per-library
 * and not per-process on purpose: src/wsi is a STATIC library linked into
 * libdxvk_dxgi.so, libdxvk_d3d11.so, libdxvk_d3d10core.so and libdxvk_d3d9.so
 * separately, so each of them carries its own WsiDriver and its own copy of
 * this table.  (That is the same fact src/wsi/wsi_platform.cpp's
 * DXVK_DEBUG_DISPLAY_LOST note was added to make visible.)  The Wine side
 * therefore resolves and calls this in every DXVK library it loads, and
 * counts them, so "registered in 3 of 4" is a number in the log rather than a
 * swapchain that silently presents nowhere.
 *
 * Returns non-zero on success, zero if the abi was refused. */
#define DXVK_WIN32U_WSI_REGISTER_NAME "dxvk_wsi_win32u_register"

typedef int32_t (*dxvk_wsi_win32u_register_fn)( const struct dxvk_win32u_wsi_ops *ops );

#ifdef __cplusplus
}
#endif

#endif /* __PPC64LE_DXVK_WIN32U_WSI_H */
