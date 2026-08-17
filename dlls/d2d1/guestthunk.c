/*
 * d2d1.dll -- the guest-side refusals for the three exports that carry COM
 * interfaces.
 *
 * Nine of d2d1's twelve exports reach a guest correctly as plain thunks: the
 * matrix and trigonometry helpers are pure value in / value out.  Three do
 * not, and this file is where each says so by name:
 *
 *   D2D1CreateFactory        vends an ID2D1Factory
 *   D2D1CreateDevice         takes an IDXGIDevice, vends an ID2D1Device
 *   D2D1CreateDeviceContext  takes an IDXGISurface, vends an
 *                            ID2D1DeviceContext
 *
 * This module publishes no winecom roster, so an interface written through
 * one of those out-parameters would reach the guest as a NATIVE ppc64 vtable
 * and the guest's first method call on it would execute ppc64 bytes as
 * x86-64.  The two that take an interface IN are worse still: dxgi DOES have
 * a roster (ppc64le/dxvk/interfaces_dxvk.json), so what a guest hands in is a
 * PROXY whose vtable is x86-64 trap stubs, and native d2d1 calling one would
 * execute x86-64 bytes as ppc64 -- the same defect in the other direction.
 *
 * spec2thunk's GUEST-IMPL redirect (see d2d1.thunks) points the GUEST's
 * exports here; the plain names are untouched and still serve native ppc64
 * callers.
 *
 * Copyright 2026 the ppc64le port authors
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301, USA
 */

#include <stdarg.h>

#include "windef.h"
#include "winbase.h"
#include "objbase.h"

#include "d2d1_3.h"

#include "wine/debug.h"

WINE_DEFAULT_DEBUG_CHANNEL(d2d);

static HRESULT guest_refuse( const char *what, void **out )
{
    ERR( "d2d1: refusing %s for an x86-64 guest -- Direct2D has no winecom "
         "interface roster on this port, so the interface this would cross "
         "with carries a vtable of the WRONG machine's code.  Answering "
         "E_NOTIMPL rather than handing it over\n", what );
    if (out) *out = NULL;
    return E_NOTIMPL;
}

HRESULT WINAPI __wine_guest_D2D1CreateFactory( D2D1_FACTORY_TYPE factory_type, REFIID iid,
                                               const D2D1_FACTORY_OPTIONS *factory_options,
                                               void **factory )
{
    return guest_refuse( "D2D1CreateFactory", factory );
}

HRESULT WINAPI __wine_guest_D2D1CreateDevice( IDXGIDevice *dxgi_device,
                                              const D2D1_CREATION_PROPERTIES *properties,
                                              ID2D1Device **device )
{
    return guest_refuse( "D2D1CreateDevice", (void **)device );
}

HRESULT WINAPI __wine_guest_D2D1CreateDeviceContext( IDXGISurface *surface,
                                                     const D2D1_CREATION_PROPERTIES *properties,
                                                     ID2D1DeviceContext **context )
{
    return guest_refuse( "D2D1CreateDeviceContext", (void **)context );
}
