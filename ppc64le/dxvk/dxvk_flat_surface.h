/*
 * The flat exports of the DXVK lane that NO WINE HEADER DECLARES, declared
 * here so spec2thunk's clang oracle can type them.
 *
 * WHY THIS FILE EXISTS.  The oracle builds one translation unit out of Wine's
 * headers and reads each export's real signature from it -- that is what makes
 * a generated thunk's arity a fact rather than a transcription of the .spec.
 * Nine of this lane's exports are private entry points that Wine ships in a
 * .spec and declares nowhere: D3D11CoreCreateDevice, D3D11CoreRegisterLayers,
 * D3D11On12CreateDevice, the three D3D10Core* and the three DXGI* below.  An
 * undeclared export is REFUSED by the oracle and simply does not appear in the
 * guest module -- which for D3D10CoreCreateDevice means a guest d3d10core.dll
 * with no exports at all.
 *
 * AND IT MATTERS THAT THE TYPES ARE REAL ONES.  spec2thunk's COM flat-surface
 * audit decides which exports carry interface pointers by looking at the
 * signature the oracle read.  Declaring these with `void *` would make the
 * audit see nothing to classify and let an interface pointer cross to the
 * guest untranslated -- the exact defect the audit exists to catch.  So they
 * are declared with the interface types they really take, and each one then
 * has to be answered by a GUEST-IMPL or GUEST-REFUSE row in the .thunks files.
 *
 * Named by a PROBE-EXTRA row in each of the three .thunks files -- appended to
 * the oracle's default translation unit, not replacing it, because everything
 * else about these modules IS declared in Wine's headers.  Same mechanism, and
 * the same reason, as msvcrt's mtdll.h.
 *
 * Copyright 2026 the ppc64le port authors
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 */

#ifndef __WINE_PPC64LE_DXVK_FLAT_SURFACE_H
#define __WINE_PPC64LE_DXVK_FLAT_SURFACE_H

#include <d3d11.h>
#include <d3d10_1.h>
#include <dxgi1_6.h>

/* d3d11.dll */
HRESULT WINAPI D3D11CoreCreateDevice(IDXGIFactory *factory, IDXGIAdapter *adapter,
        UINT flags, const D3D_FEATURE_LEVEL *feature_levels, UINT levels,
        ID3D11Device **device);
HRESULT WINAPI D3D11CoreRegisterLayers(void);
HRESULT WINAPI D3D11On12CreateDevice(IUnknown *device, UINT flags,
        const D3D_FEATURE_LEVEL *feature_levels, UINT levels,
        IUnknown * const *queues, UINT queue_count, UINT node_mask,
        ID3D11Device **device_out, ID3D11DeviceContext **context,
        D3D_FEATURE_LEVEL *obtained_level);

/* dxgi.dll */
HRESULT WINAPI DXGID3D10CreateDevice(HMODULE d3d10core, IDXGIFactory *factory,
        IDXGIAdapter *adapter, UINT flags, const D3D_FEATURE_LEVEL *feature_levels,
        UINT levels, void **device);
HRESULT WINAPI DXGID3D10RegisterLayers(const void *layers, UINT layer_count);
HRESULT WINAPI DXGIDeclareAdapterRemovalSupport(void);

/* d3d10core.dll */
HRESULT WINAPI D3D10CoreCreateDevice(IDXGIFactory *factory, IDXGIAdapter *adapter,
        UINT flags, D3D_FEATURE_LEVEL feature_level, ID3D10Device **device);
HRESULT WINAPI D3D10CoreGetVersion(void);
HRESULT WINAPI D3D10CoreRegisterLayers(void);

#endif /* __WINE_PPC64LE_DXVK_FLAT_SURFACE_H */
