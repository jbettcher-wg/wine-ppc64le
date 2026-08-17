/*
 * dwrite.dll -- the guest-side refusal for the one export that vends a COM
 * interface.
 *
 * dwrite has exactly one entry point, and it hands back an IDWriteFactory.
 * There is no winecom roster for the DirectWrite surface, so what a
 * pass-through would give an x86-64 guest is a NATIVE ppc64 vtable, and the
 * guest's first method call on it -- CreateTextFormat, typically, one line
 * later -- would execute ppc64 bytes as x86-64.  That is the defect
 * hangover-ppc64le/docs/system-com-design.md exists for, and its signature is
 * a c000001d somewhere with no guest frame to blame it on.
 *
 * So the guest export is redirected here (spec2thunk GUEST-IMPL, see
 * dwrite.thunks) and answers E_NOTIMPL with the module and reason NAMED.  The
 * plain DWriteCreateFactory stays exactly as Wine wrote it for native ppc64
 * callers -- gdi32 and d2d1 both use it -- and is untouched by this file.
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
#include "dwrite_3.h"

#include "wine/debug.h"

WINE_DEFAULT_DEBUG_CHANNEL(dwrite);

HRESULT WINAPI __wine_guest_DWriteCreateFactory( DWRITE_FACTORY_TYPE type, REFIID iid,
                                                 IUnknown **factory )
{
    ERR( "dwrite: refusing DWriteCreateFactory(type %u) for an x86-64 guest -- "
         "DirectWrite has no winecom interface roster on this port, so the "
         "IDWriteFactory this would return carries a NATIVE ppc64 vtable that "
         "the guest cannot call.  Answering E_NOTIMPL instead of handing it "
         "over; a caller that falls back to GDI text will work\n", type );
    if (factory) *factory = NULL;
    return E_NOTIMPL;
}
