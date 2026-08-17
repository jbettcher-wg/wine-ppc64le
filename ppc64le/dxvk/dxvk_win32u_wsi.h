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
 * ALL FIVE CALLS ARRIVE ON THE THREAD DXVK WAS CALLED ON, and every one of
 * them ends in win32u, which may only be entered from a Wine thread.  The Wine
 * side asserts that (see the wine_thread flag in dlls/d3d11/unix.c) rather
 * than trusting it: DXVK owns a CS thread and a submission thread, neither of
 * which has a TEB, and a win32u call from one of those is not a wrong answer,
 * it is a fault in somebody else's stack frame.
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
 * for another -- is a jump through a pointer read from the wrong offset. */
#define DXVK_WIN32U_WSI_ABI 1

#ifdef __cplusplus
extern "C" {
#endif

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
