/*
 * variant_layout_probe -- confirms, at COMPILE TIME, that the by-value
 * aggregates dlls/combase/syscom.c's VariantClear wrapper depends on have
 * the offsets/sizes its comments and case table assume.  No runtime, no
 * CRT, nothing to execute: a _Static_assert either holds for a given target
 * or the compile fails naming exactly which one broke.
 *
 * Build TWICE and require both to succeed:
 *
 *   clang -target x86_64-windows-gnu    -I<build>/include -I<src>/include \
 *         -I<src>/include/msvcrt -fsyntax-only variant_layout_probe.c
 *   clang -target powerpc64le-linux-gnu -I<build>/include -I<src>/include \
 *         -I<src>/include/msvcrt -fsyntax-only variant_layout_probe.c
 *
 * Agreement is not "both compiled" alone -- every assertion below names a
 * literal offset, so a target whose real layout differs fails THAT target's
 * compile with the assertion's own message.  This is the standing, buildable
 * form of the plan's one-off /tmp/varlayout.c dual-dump; see
 * hangover-ppc64le/docs/system-com-design.md §9.2 and §12.7 (the aggregate
 * layout gate this belongs in, gen_layout_check.py, is still designed but
 * not wired to a build target -- that debt is unchanged by this file).
 *
 * Copyright 2026 the ppc64le port authors
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */
#define COBJMACROS
#include <stddef.h>
#include <oleauto.h>
#include <propidl.h>

#define ASSERT_OFFSET(type, member, off) \
    _Static_assert(offsetof(type, member) == (off), \
                   #type "." #member " is not at offset " #off)
#define ASSERT_SIZE(type, sz) \
    _Static_assert(sizeof(type) == (sz), #type " is not " #sz " bytes")

/* VARIANT (oaidl.h): vt at 0, the payload union at 8 -- punkVal/pdispVal/
 * bstrVal/dblVal/parray/byref all alias the same 8-byte slot the way native
 * VariantClear's own switch expects, decVal overlays the WHOLE struct from
 * offset 0 (it is the outer union's other arm, not the inner struct's). */
ASSERT_SIZE( VARIANT, 24 );
ASSERT_OFFSET( VARIANT, vt, 0 );
ASSERT_OFFSET( VARIANT, punkVal, 8 );
ASSERT_OFFSET( VARIANT, pdispVal, 8 );
ASSERT_OFFSET( VARIANT, bstrVal, 8 );
ASSERT_OFFSET( VARIANT, dblVal, 8 );
ASSERT_OFFSET( VARIANT, parray, 8 );
ASSERT_OFFSET( VARIANT, byref, 8 );
ASSERT_OFFSET( VARIANT, decVal, 0 );

/* PROPVARIANT (propidl.h): same header shape as VARIANT, same reason the one
 * walker serves both (ole32's PropVariantClear, not yet wrapped by this
 * series -- see the plan's §5 step 4). */
ASSERT_SIZE( PROPVARIANT, 24 );
ASSERT_OFFSET( PROPVARIANT, vt, 0 );
ASSERT_OFFSET( PROPVARIANT, punkVal, 8 );

/* SAFEARRAY (oaidl.h): the fields __wine_guest_VariantClear's SAFEARRAY
 * branch reads directly as plain shared memory (fFeatures for the FADF_*
 * gate; pvData is not read by VariantClear but pins the rest of the layout). */
ASSERT_SIZE( SAFEARRAY, 32 );
ASSERT_OFFSET( SAFEARRAY, cDims, 0 );
ASSERT_OFFSET( SAFEARRAY, fFeatures, 2 );
ASSERT_OFFSET( SAFEARRAY, cbElements, 4 );
ASSERT_OFFSET( SAFEARRAY, cLocks, 8 );
ASSERT_OFFSET( SAFEARRAY, pvData, 16 );
ASSERT_OFFSET( SAFEARRAY, rgsabound, 24 );

ASSERT_SIZE( SAFEARRAYBOUND, 8 );
ASSERT_SIZE( DECIMAL, 16 );
ASSERT_SIZE( DISPPARAMS, 24 );
ASSERT_SIZE( EXCEPINFO, 64 );
ASSERT_SIZE( CY, 8 );

int variant_layout_probe_unused;
