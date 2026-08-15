/*
 * The native-lane presentation bootstrap CLSID, shared between the native
 * d3d12.dll (main.c's D3D12GetInterface intercept) and guest-side callers
 * (hangover-ppc64le/probes/guest/d3d12_present.c).
 *
 * Phase (a) of presentation-design.md §4 has no native dxgi.dll yet, so
 * there is no CreateDXGIFactory to reach the swapchain path through.  Until
 * phase (b) lands one, D3D12GetInterface(CLSID_WineDXGIFactory,
 * IID_IDXGIFactory2, ...) returns the d3d12 module's own minimal factory,
 * whose CreateSwapChainForHwnd presents through vkd3d-proton and win32u's
 * client-surface machinery.  Wine-private; no real runtime answers it.
 *
 * Copyright 2026 the ppc64le port authors
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#ifndef __WINE_D3D12_WINE_PRESENT_H
#define __WINE_D3D12_WINE_PRESENT_H

/* {8f47b0d6-2d0c-4ae1-8e2f-6f2b7c93a001}.  A static definition rather than
 * DEFINE_GUID so no TU needs INITGUID for it. */
static const GUID CLSID_WineDXGIFactory =
    { 0x8f47b0d6, 0x2d0c, 0x4ae1, { 0x8e, 0x2f, 0x6f, 0x2b, 0x7c, 0x93, 0xa0, 0x01 } };

#endif /* __WINE_D3D12_WINE_PRESENT_H */
