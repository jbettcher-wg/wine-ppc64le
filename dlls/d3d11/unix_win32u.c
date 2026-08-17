/*
 * d3d11 unixlib -- the win32u seam, shared verbatim with the d3d12 lane.
 *
 * THIS FILE IS ONE LINE OF SUBSTANCE, AND THAT IS THE POINT.  The seam between
 * a native graphics engine holding a raw VkInstance and win32u's client-surface
 * layer is the same seam whether the engine is vkd3d-proton or DXVK: bind
 * win32u only if it is already loaded, resolve the system loader's
 * vkGetInstanceProcAddr, and expose four integer-typed calls that mention
 * neither engine.  dlls/d3d12/unix_win32u.c is that seam, with the reasons for
 * every line of it in its banner.  Compiling it here rather than copying it is
 * what keeps ONE description of the win32u ABI in the tree: two copies would
 * drift the day WINE_HWND_SURFACE_VERSION moves, and the symptom would be a
 * swapchain that presents to a surface built from a struct laid out for the
 * other version.
 *
 * It is #included rather than linked because Wine's build gives each module
 * its own object set -- there is no shared static library between two unixlibs
 * and adding one for 120 lines would be a worse trade.  The two .so files end
 * up with their own copies of the four symbols, which is what they want: each
 * binds win32u independently and holds its own function table.
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

/* Traces from this seam belong to the module that is presenting; a d3d11
 * process asked for +d3d11 and should not have to know that the code came
 * from the d3d12 directory. */
#define HWNDSURF_CHANNEL_D3D11

#include "../d3d12/unix_win32u.c"
