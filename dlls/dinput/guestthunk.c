/*
 * dinput.dll -- the guest-side refusals for the three exports that vend a COM
 * interface.
 *
 * This is the DirectInput 3/5/7 module.  All three of its creators write an
 * IDirectInputA/W through an out-parameter, and this module publishes no
 * winecom roster, so what a pass-through would give an x86-64 guest is a
 * NATIVE ppc64 vtable -- and the guest's next line is always
 * IDirectInput_CreateDevice, an x86-64 `call [rax+0x18]` into ppc64 bytes.
 *
 * dinput8.dll DOES have a roster (ppc64le/shell/interfaces_dinput.json), and
 * extending it to cover this module is mechanical rather than mysterious:
 * IDirectInputA/W, IDirectInput2A/W, IDirectInput7A/W,
 * IDirectInputDeviceA/W, IDirectInputDevice2A/W and IDirectInputDevice7A/W
 * are declared in the same include/dinput.h with the same DECLARE_INTERFACE_
 * shape the generator already reads, and they carry the same fourteen
 * callback methods with the same one blocker.  It is not done because
 * DirectInput 7 is a pre-2001 interface and every title that still ships is
 * on dinput8 -- the surface would be twelve more vtables for a caller that
 * does not exist.  If one turns up, add its names to SURFACE in
 * ppc64le/shell/gen_dinput_surface.py, give this module a guestcom.c the
 * shape of dlls/dinput8/guestcom.c, and delete this file.
 *
 * Until then each guest export resolves here (spec2thunk GUEST-IMPL, see
 * dinput.thunks) and answers DIERR_OUTOFMEMORY -- the failure every
 * DirectInputCreate caller already handles -- with the reason NAMED.  The
 * plain exports are untouched and still serve native ppc64 callers.
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

#include "dinput.h"

#include "wine/debug.h"

WINE_DEFAULT_DEBUG_CHANNEL(dinput);

static HRESULT guest_refuse( const char *what, void **out )
{
    ERR( "dinput: refusing %s for an x86-64 guest -- DirectInput 7 has no "
         "winecom interface roster on this port (dinput8.dll does; see "
         "ppc64le/shell/interfaces_dinput.json), so the IDirectInput this "
         "would return carries a NATIVE ppc64 vtable the guest cannot call.  "
         "Answering DIERR_OUTOFMEMORY rather than handing it over\n", what );
    if (out) *out = NULL;
    return DIERR_OUTOFMEMORY;
}

HRESULT WINAPI __wine_guest_DirectInputCreateA( HINSTANCE hinst, DWORD version,
                                                IDirectInputA **out, IUnknown *outer )
{
    return guest_refuse( "DirectInputCreateA", (void **)out );
}

HRESULT WINAPI __wine_guest_DirectInputCreateW( HINSTANCE hinst, DWORD version,
                                                IDirectInputW **out, IUnknown *outer )
{
    return guest_refuse( "DirectInputCreateW", (void **)out );
}

HRESULT WINAPI __wine_guest_DirectInputCreateEx( HINSTANCE hinst, DWORD version,
                                                 REFIID iid, void **out, IUnknown *outer )
{
    return guest_refuse( "DirectInputCreateEx", out );
}
