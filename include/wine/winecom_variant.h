/*
 * The ONE shared VARIANT/DISPPARAMS translation discipline for winecom
 * surfaces (2026-09-01, the completeness pass).
 *
 * A VARIANT is oleaut's tagged union; on this port both sides of every
 * winecom surface are Wine PE code in one address space, so every arm that
 * is DATA (numbers, BSTRs, blobs, SAFEARRAYs of data) crosses as the bytes
 * it already is.  The arms that are OBJECTS must cross the proxy boundary:
 *
 *   IN  (guest -> native): VT_UNKNOWN/VT_DISPATCH unwrap through
 *       winecom_to_native.  ~0u accepts any FORWARD proxy by identity and
 *       fails a guest-AUTHORED object, which the caller refuses loudly --
 *       reverse-proxying an arbitrary IUnknown is licensed on no surface.
 *   OUT (native -> guest): the object is QI'd for IDispatch and wrapped
 *       under the roster index the CALLER passes (each surface's own
 *       IDispatch row); an object that is not IDispatch has no static type
 *       a VARIANT can name, so it is RELEASED and the VARIANT scrubbed to
 *       VT_EMPTY -- the refusal-hygiene rule: loud, and never a native
 *       vtable in guest hands.
 *
 * Everything here is static inline: winecom state is per-linkee by design,
 * so each module's copy binds to its own runtime instance -- the same
 * pattern as include/wine/winecom_fpcall.h, and the reason this is a header
 * rather than a libs/winecom export.
 *
 * Callers pass their own FIXME channel through the `logged` LONG they own,
 * so each surface says each failure class once, under its own debug channel.
 */

#ifndef __WINE_WINECOM_VARIANT_H
#define __WINE_WINECOM_VARIANT_H

#include "oleauto.h"
#include "wine/winecom.h"

/* The interface-bearing arms nothing translates: VECTOR/ARRAY/BYREF of
 * objects (a container of proxies would need per-element lifetime the
 * VARIANT contract does not expose to a walker), and the stream/storage
 * arms.  VT_UNKNOWN and VT_DISPATCH themselves are handled by the callers. */
static inline BOOL winecom_vt_is_untranslatable_iface_arm( VARTYPE vt )
{
    VARTYPE base = vt & VT_TYPEMASK;

    if ((vt & (VT_VECTOR | VT_ARRAY | VT_BYREF)) &&
        (base == VT_UNKNOWN || base == VT_DISPATCH || base == VT_VARIANT))
        return TRUE;
    switch (base)
    {
    case VT_STREAM:
    case VT_STREAMED_OBJECT:
    case VT_STORAGE:
    case VT_STORED_OBJECT:
        return TRUE;
    }
    return FALSE;
}

/* IN direction, in place on a caller-owned COPY: TRUE = proceed, FALSE =
 * refuse the call (the object arm could not be unwrapped, or the arm is
 * untranslatable).  The caller logs; this only answers. */
static inline BOOL winecom_variant_in( VARIANTARG *v )
{
    void *native;

    switch (V_VT(v))
    {
    case VT_UNKNOWN:
    case VT_DISPATCH:
        if (V_UNKNOWN(v) && !winecom_to_native( V_UNKNOWN(v), ~0u, &native ))
            return FALSE;
        if (V_UNKNOWN(v)) V_UNKNOWN(v) = native;
        return TRUE;
    default:
        return !winecom_vt_is_untranslatable_iface_arm( V_VT(v) );
    }
}

/* OUT direction, in place on the guest's own VARIANT, after a SUCCEEDED
 * call: wrap the object arm as the surface's IDispatch row, scrub what
 * cannot be typed.  Returns TRUE when it scrubbed (so the caller can say
 * so once). */
static inline BOOL winecom_variant_out_fixup( VARIANT *v, UINT idispatch_iface )
{
    IUnknown *u;
    void *disp;

    if (!v) return FALSE;
    if (V_VT(v) != VT_UNKNOWN && V_VT(v) != VT_DISPATCH) return FALSE;
    u = (IUnknown *)V_UNKNOWN(v);
    if (!u) return FALSE;
    if (SUCCEEDED(IUnknown_QueryInterface( u, &IID_IDispatch, &disp )))
    {
        IUnknown_Release( u );
        V_VT(v) = VT_DISPATCH;
        V_DISPATCH(v) = winecom_wrap( disp, idispatch_iface );
        return FALSE;
    }
    IUnknown_Release( u );
    /* the scrub by hand rather than VariantInit: this header must not pull
     * an oleaut32 import into modules (mfplat's i386 half) that have none,
     * and VT_EMPTY with a zeroed arm is exactly what VariantInit writes. */
    V_VT(v) = VT_EMPTY;
    V_UNKNOWN(v) = NULL;
    return TRUE;
}

#endif /* __WINE_WINECOM_VARIANT_H */
