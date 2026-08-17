/*
 * winecom -- the REVERSE-PROXY self-test hook.
 *
 * The mechanism gate (ppc64le/winecom/check-reverse-proxy.sh) needs one thing
 * no ordinary API gives it: a piece of NATIVE ppc64 code that calls a
 * guest-implemented COM object's methods ON PURPOSE, one method class at a
 * time, and reports what came back.  Media Foundation calls an IMFAsyncCallback
 * from a work queue and XAudio2 calls an IXAudio2VoiceCallback from its mixer
 * thread, and both of those are measured by their own gates -- but neither
 * calls a method that takes a float, or a string, or an interface pointer, and
 * the mechanism has to be proved for the argument classes it CARRIES rather
 * than only for the ones its first two consumers happen to use.
 *
 * So this is a test hook, said plainly.  It lives on the Media Foundation
 * surface because that surface already rosters an interface with one method of
 * every class this mechanism marshals -- IMFAttributes: SetUINT32 (an integer),
 * SetUINT64 (a wide integer), SetDouble (a by-value double, which the FORWARD
 * direction refuses and the reverse one serves), SetString (a WCHAR pointer),
 * SetUnknown (an interface IN), GetUnknown (an interface OUT through a REFIID)
 * and SetItem (a PROPVARIANT, which is refused in BOTH directions and is here
 * to prove the refusal discipline survived) -- plus IMFSimpleAudioVolume for
 * the one class IMFAttributes has no method of, a by-value single-precision
 * float.
 *
 * Nothing in Wine calls this and nothing but the gate ever will.  It is
 * exported so the gate's guest probe can reach it through the ordinary
 * spec2thunk GUEST-IMPL path, which is also the point: the hook is entered the
 * same way every other flat export is, so the gate measures the real boundary
 * and not a private back door into it.
 *
 * Copyright 2026 the ppc64le port authors
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 */

#ifndef __WINE_WINE_WINECOM_SELFTEST_H
#define __WINE_WINE_WINECOM_SELFTEST_H

/* The values the hook sends into the guest object.  Both sides compile them
 * from this one header, so "the guest saw what native sent" is a comparison of
 * two spellings of the same constant and not of two hand-copied literals.
 *
 * Each is #ifndef-guarded so the GATE can override one on the command line
 * when it builds the guest probe.  That is the gate's third negative control:
 * a probe built expecting a different constant must fail its own value check,
 * which is what proves the arguments are COMPARED rather than merely printed.
 * The NATIVE side is built once, from these values, so only one end moves. */
#ifndef WINECOM_ST_UINT32
#define WINECOM_ST_UINT32   0xC0FFEE01u
#endif
#ifndef WINECOM_ST_UINT64
#define WINECOM_ST_UINT64   0x0123456789ABCDEFull
#endif
#ifndef WINECOM_ST_DOUBLE
#define WINECOM_ST_DOUBLE   3.140000000000000124900090270330
#endif
#ifndef WINECOM_ST_FLOAT
#define WINECOM_ST_FLOAT    0.75f
#endif
#ifndef WINECOM_ST_STRING
#define WINECOM_ST_STRING   L"winecom-reverse"
#endif

/* What the guest returns from each rostered method, so the NATIVE side can
 * prove the return value crossed too.  A method that merely ran and a method
 * whose result came back are different facts. */
#define WINECOM_ST_HR_OK    ((HRESULT)0x00021234)

/* What the hook found.  Filled in by native code, read by the guest probe;
 * the struct crosses as plain memory, which on this port is the same memory. */
struct winecom_selftest_report
{
    unsigned int checks;        /* checks the NATIVE side ran */
    unsigned int failures;      /* ...and how many it failed */
    unsigned int first_fail;    /* the number of the first, 1-based; 0 = none */
    unsigned int calls;         /* reverse calls made into the guest object */
    unsigned int identity_ok;   /* QueryInterface came back as this same proxy */
    unsigned int roundtrip_ok;  /* the native object the guest was handed came
                                   back out of the guest as itself */
    unsigned int refuse_ok;     /* a slot the tables refuse still refused */
    unsigned int refs_leaked;   /* the reverse proxy's reference count did not
                                   return to where it started */
    unsigned int guest_count;   /* what the guest's GetCount answered */

    /* WHAT ONE CROSSING COSTS, in nanoseconds, measured by the hook over
     * WINECOM_ST_TIMED_CALLS repetitions of one trivial method.  Three numbers
     * rather than one, because a number with nothing to compare it against is
     * not a measurement:
     *
     *   ns_native   the same method on a NATIVE object through the same
     *               vtable-call shape -- the floor, and the thing the crossing
     *               is a multiple of;
     *   ns_reverse  the same method on the GUEST object through a reverse
     *               proxy, on the thread that is already running guest code;
     *   ns_foreign  the FIRST reverse call on a thread created by native code
     *               that has never run guest code in its life.  That one is
     *               the lazy per-thread emulator init, paid once per thread,
     *               and it is the number an audio mixer thread pays at its
     *               first buffer boundary and never again.
     *
     * XAudio2 calls a voice callback from its mixer thread, which has a period
     * to hit; this is where the cost of doing that is a fact instead of a
     * worry. */
    unsigned int ns_native;
    unsigned int ns_reverse;
    unsigned int ns_foreign;
    unsigned int foreign_ok;    /* the foreign thread's call actually reached
                                   the guest object and came back */
};

/* Enough repetitions that the timer's own resolution is not the answer, few
 * enough that the gate does not become a benchmark. */
#define WINECOM_ST_TIMED_CALLS 2000

/* Drive `attributes` and `volume` through one method of every argument class
 * this mechanism marshals and fill in `report`.  Both objects are ordinary COM
 * interface pointers as far as this function is concerned; on the guest lane
 * they are reverse proxies, which is the entire point.
 *
 * DECLARED HERE because spec2thunk's signature oracle reads Wine headers and
 * no Wine header has any business declaring a test hook.  The declaration
 * needs the Media Foundation types, so this header must follow mfobjects.h --
 * which it does in the two places it is included: dlls/mfplat/mfcom.c, and the
 * oracle's translation unit through PROBE-EXTRA in dlls/mfplat/mfplat.thunks. */
HRESULT WINAPI __wine_winecom_reverse_selftest( IMFAttributes *attributes,
                                                IMFSimpleAudioVolume *volume,
                                                struct winecom_selftest_report *report );

/* THE NESTING LEG.  One call to this is one native->guest REVERSE call
 * (IMFAttributes::LockStore); the guest's implementation of that method calls
 * this export again, which is one guest->native THUNK call.  So each round
 * trip is exactly the ping-pong a callback-driven API does -- OnBufferEnd
 * calls SubmitSourceBuffer, which completes a buffer, which calls back -- and
 * the depth is whatever the guest decides to stop at.
 *
 * Every crossing spends kernel stack, in both directions, and nothing in the
 * port bounds how deep an application nests.  This is what turns "it died
 * somewhere deep" into a number. */
#define WINECOM_ST_NEST_DEPTH 64

HRESULT WINAPI __wine_winecom_reverse_nest( IMFAttributes *attributes,
                                            unsigned int *depth_reached );

#endif  /* __WINE_WINE_WINECOM_SELFTEST_H */
