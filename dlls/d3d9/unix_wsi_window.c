/*
 * d3d9 unixlib -- the win32u WINDOW seam, shared verbatim with the d3d11 lane.
 *
 * THIS FILE IS ONE LINE OF SUBSTANCE, for exactly the reason dlls/d3d11's
 * unix_win32u.c is: the seam between DXVK's WSI backend and the window
 * operations Wine can serve is the same seam whichever D3D version asked, and
 * two copies of it would drift the day one of them learned about a new
 * DXVK_WIN32U_SWP_ flag.  dlls/d3d11/unix_wsi_window.c is that seam, with the
 * reasons for every line of it in its banner.
 *
 * It is #included rather than linked because Wine's build gives each module its
 * own object set -- there is no shared static library between two unixlibs, and
 * adding one for this would be a worse trade.  d3d9.so and d3d11.so end up with
 * their own copies of these symbols, which is what they want: each resolves
 * win32u independently, each carries its own thread flag, and each publishes
 * its own callback table into its own libdxvk.
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

/* First, and before the define, because makedep requires every unix source to
 * open with it and reads THIS file rather than the one it includes.  The
 * included source opens with it too; config.h has an include guard, so the
 * second one is free and neither file has to know about the other's rule. */
#include "config.h"

/* Traces from this seam belong to the module that owns the window: a d3d9
 * process asked for +d3d9 and should not have to know that the code came from
 * the d3d11 directory. */
#define HWNDSURF_CHANNEL_D3D9

#include "../d3d11/unix_wsi_window.c"
