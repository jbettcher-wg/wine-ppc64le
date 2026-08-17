/*
 * d3d9 unixlib -- the win32u seam, shared verbatim with the d3d11 and d3d12
 * lanes.  See dlls/d3d11/unix_win32u.c: this is the same one line for the same
 * reason, a third time.  Three .so files, three copies of four symbols, ONE
 * description of the win32u ABI.
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

/* First, because makedep requires every unix source to open with it and reads
 * THIS file rather than the one it includes. */
#include "config.h"

/* Traces from this seam belong to the module that is presenting. */
#define HWNDSURF_CHANNEL_D3D9

#include "../d3d12/unix_win32u.c"
