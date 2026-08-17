/*
 * dxvk_flat_surface_d3d9.h -- the D3D9 flat exports Wine declares nowhere.
 *
 * The D3D9 sibling of dxvk_flat_surface.h, and it exists for the same reason:
 * spec2thunk's clang oracle types every export by reading Wine's real headers,
 * and an export Wine ships in a .spec but declares in no header comes back as
 * "no declaration found" and is refused -- which is a fact about the tooling
 * and not about the ABI.  This file is named by `PROBE-EXTRA` in
 * dlls/d3d9/d3d9.thunks and appended to the oracle's translation unit.
 *
 * FOUR NAMES, and each is here because Wine's include/d3d9.h really does not
 * declare it, checked rather than assumed:
 *
 *   Direct3DCreate9On12, Direct3DCreate9On12Ex   D3D9-on-D3D12, which this
 *       lane refuses by name (dlls/d3d9/main.c) for the reason
 *       D3D11On12CreateDevice is refused one API up: it needs a live
 *       ID3D12Device from the d3d12 lane, and the two lanes hold separate
 *       winecom instances.  Declared here anyway, because an export that is
 *       refused at RUNTIME must still be EMITTED -- a guest that imports it
 *       should reach the refusal and read it, not fail to load.
 *   Direct3DShaderValidatorCreate9   likewise refused, and for a reason worth
 *       stating: its documented return is an interface that is in no roster on
 *       this surface, so a raw native pointer is the only thing there would be
 *       to hand back.
 *   DebugSetMute                     a scalar entry point Wine has always
 *       exported and never declared.
 *
 * The `override_list` arguments are spelled `void *` rather than
 * D3D9ON12_ARGS *: that struct is not in Wine's headers either, the exports
 * are refused before the pointer is read, and inventing a layout for something
 * nothing dereferences would be the wrong kind of precision.
 *
 * Copyright 2026 the ppc64le port authors
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 */

#include <d3d9.h>

IDirect3D9 * WINAPI Direct3DCreate9On12( UINT sdk_version, void *override_list,
                                         UINT override_entries );
HRESULT WINAPI Direct3DCreate9On12Ex( UINT sdk_version, void *override_list,
                                      UINT override_entries, IDirect3D9Ex **d3d9ex );
HRESULT WINAPI Direct3DShaderValidatorCreate9( void );
void WINAPI DebugSetMute( void );
