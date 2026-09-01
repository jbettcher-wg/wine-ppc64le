/*
 * winecom -- the INTERFACE-ARRAY delivery self-test hook.
 *
 * libs/winecom/reverse.c's CA_IFACE_ARR_IN arm carries an ARRAY of native
 * objects into a guest-implemented method: native code hands a sink N
 * interface pointers, and every one of them has to arrive on the other side
 * as ITS OWN forward proxy.  The arm landed 2026-09-01 with the WMI async
 * sinks and was disclosed as never having executed -- there is exactly one
 * row on any roster that uses it (IWbemObjectSink::Indicate), nothing
 * headless drives WMI, and a statically-verified marshal arm is a claim
 * rather than a measurement.  This is the drive.
 *
 * WHY A HOOK RATHER THAN A DRIVEABLE SLOT.  Indicate is called by wbemprox
 * when a query it is servicing produces results, which needs a WMI service,
 * a provider, and a query -- none of which exist on a gate machine, and all
 * of which would put three subsystems between this arm and its verdict.  So
 * native code calls the sink DIRECTLY here.  What matters is that it is the
 * REAL path and not a re-implementation of it: this hook holds an ordinary
 * IWbemObjectSink pointer and calls Indicate through its vtable, and on the
 * guest lane that pointer is a reverse proxy whose slot 3 IS
 * winecom_reverse_dispatch reading cls_IWbemObjectSink_3.  Nothing here
 * knows the arm exists; it only knows it called a method.
 *
 * WHAT IS PROVEN, and by which side:
 *
 *   NATIVE (this hook, into the report)   the array it passed came back
 *     UNMUTATED (the arm copies into its own staging, so a caller that reuses
 *     its array is not handed proxies); each object was entered exactly ONCE;
 *     they were entered in ELEMENT ORDER; the empty delivery (count 0, NULL
 *     array) crossed as itself.
 *   GUEST (the probe, ppc64le/winecom/probes/arrin_probe.c)   the count
 *     arrived; every element was non-NULL and DISTINCT from every other; and
 *     calling a method on element k reached NATIVE OBJECT k -- which is the
 *     element-wise claim, and the only check that can tell a correct
 *     translation from one that wrapped the same object N times.
 *
 * The per-element answer is what carries the identity: object k's Get returns
 * WINECOM_ARRIN_HR(k), so the guest comparing what came back to what it
 * expected for that position is comparing two spellings of one constant.
 *
 * Copyright 2026 the ppc64le port authors
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 */

#ifndef __WINE_WINE_WINECOM_ARRIN_H
#define __WINE_WINE_WINECOM_ARRIN_H

#include <wbemcli.h>

/* How many objects one delivery carries.  Three rather than two because the
 * failure this leg exists to catch -- every element wrapped from the same
 * source pointer -- is invisible at one and ambiguous at two. */
#define WINECOM_ARRIN_COUNT 3

/* The base of the per-element answer.  #ifndef-guarded so the GATE can build
 * the guest probe expecting a different one: a probe whose oracle moved must
 * fail its own check, which is what proves the elements are COMPARED and not
 * merely counted.  The NATIVE side is built once, from this value, so only
 * one end ever moves.  0x0002xxxx is the FACILITY_ITF success range, so these
 * are SUCCEEDED() answers that carry data -- the same trick
 * WINECOM_ST_HR_OK plays on the mf surface. */
#ifndef WINECOM_ARRIN_HR_BASE
#define WINECOM_ARRIN_HR_BASE 0x00025100
#endif

/* Element k's native object answers this from its Get.  The k is the whole
 * point: a delivery that arrived as three copies of one object answers the
 * same HRESULT three times, and the guest says so. */
#define WINECOM_ARRIN_HR( k ) ((HRESULT)(WINECOM_ARRIN_HR_BASE + (k)))

/* What the guest sink answers, so the native side knows its call ARRIVED and
 * did not merely return.  Two spellings, because "the guest ran the full
 * delivery" and "the guest saw the empty delivery" are different facts. */
#define WINECOM_ARRIN_HR_OK    ((HRESULT)0x00025180)
#define WINECOM_ARRIN_HR_EMPTY ((HRESULT)0x00025181)

/* What the hook found.  Filled in by native code, read by the guest probe;
 * the struct crosses as plain memory, which on this port is the same memory. */
struct winecom_arrin_report
{
    unsigned int checks;        /* checks the NATIVE side ran */
    unsigned int failures;      /* ...and how many it failed */
    unsigned int first_fail;    /* the number of the first, 1-based; 0 = none */

    unsigned int sent;          /* elements handed to Indicate */
    unsigned int guest_hr;      /* what the guest's Indicate answered */
    unsigned int empty_hr;      /* ...and what the EMPTY delivery answered */

    unsigned int array_unmutated; /* the caller's own array still holds the
                                     pointers it held before the call */
    unsigned int entered_once;    /* every object was called back exactly once */
    unsigned int in_order;        /* ...and in the order they were delivered */

    /* The reference contract, which is the half a static reading of the arm
     * cannot see: winecom_to_guest takes a reference for the guest on every
     * element it wraps, and the arm has to give every one of them back.  The
     * hook reads its own objects' counts before and after, so a leak is a
     * NUMBER here rather than a slow death in a title. */
    unsigned int refs_before;
    unsigned int refs_after;
    unsigned int refs_leaked;
};

/* Deliver WINECOM_ARRIN_COUNT native objects to `sink`, then an empty
 * delivery, and fill in `report`.  `sink` is an ordinary COM interface
 * pointer as far as this function is concerned; on the guest lane it is a
 * reverse proxy, which is the entire point.
 *
 * DECLARED HERE for the reason include/wine/winecom_selftest.h gives for its
 * own twin: spec2thunk's signature oracle reads Wine headers and no Wine
 * header has any business declaring a test hook.  It is included in exactly
 * two places -- dlls/combase/syscom.c, and the oracle's translation unit
 * through PROBE-EXTRA in dlls/combase/combase.thunks -- and pulls wbemcli.h
 * itself rather than assuming either one already had it. */
HRESULT WINAPI __wine_winecom_arrin_selftest( IWbemObjectSink *sink,
                                              struct winecom_arrin_report *report );

#endif /* __WINE_WINE_WINECOM_ARRIN_H */
