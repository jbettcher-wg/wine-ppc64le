/*
 * winecom -- the internals the two halves of the runtime share.
 *
 * winecom.c is the FORWARD half (a native object handed to the guest: a guest
 * vtable of trap stubs whose slots reach the native implementation).  reverse.c
 * is the MIRROR (a guest object handed to native code: a native vtable whose
 * slots enter the guest method through the emulator).  They share one surface,
 * one critical section and one notion of what "our proxy" means, because
 * round-trip identity depends on both halves recognising each other's objects:
 * a reverse proxy handed BACK to the guest must come out as the original guest
 * pointer, and a forward proxy handed back to native code must come out as the
 * original host pointer.
 *
 * This is not a public interface.  include/wine/winecom.h is.
 *
 * Copyright 2026 the ppc64le port authors
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 */

#ifndef __WINE_WINECOM_PRIVATE_H
#define __WINE_WINECOM_PRIVATE_H

/* ------------------------------------------------------------- winecom.c */

/* The one surface this linkee's runtime is bound to; NULL before attach. */
extern const struct winecom_surface *wc_surface;

/* The intern lock, shared by both directions: a proxy of either kind can be
 * created while the other kind is being looked up (a reverse call whose
 * argument is a forward proxy does exactly that), and two locks would be two
 * lock orders. */
extern CRITICAL_SECTION wc_cs;

extern BOOL wc_ready(void);
extern BOOL wc_nowrap(void);

/* TRUE if `ptr` is one of the FORWARD proxies, with its host in *host_out. */
extern BOOL wc_forward_host( void *ptr, void **host_out );

/* Drop ONE guest-visible reference from a forward proxy: what the reverse
 * dispatcher owes for a proxy it minted so a guest method could be handed a
 * native object.  A pointer that is not one of ours is left alone. */
extern void wc_forward_release( void *ptr );

/* ------------------------------------------------------------- reverse.c */

/* Called from winecom_attach once the surface is bound and the guest vtables
 * are materialised.  A surface that did not ask for reverse proxies
 * (WINECOM_SF_REVERSE) does nothing here and cannot fail. */
extern BOOL wc_reverse_init(void);

/* -> the guest pointer behind a REVERSE proxy, or NULL if `ptr` is not one.
 * This is what makes winecom_wrap() round-trip: handing a reverse proxy back
 * to the guest must produce the guest's own object, not a forward proxy
 * wrapping a native vtable that wraps the guest object. */
extern void *wc_reverse_guest( void *ptr );

/* Drop ONE native reference from a reverse proxy, without going through its
 * own vtable: winecom_wrap() unwrapping one on its way back to the guest owes
 * exactly that, and slot 2 of a [local] interface is not Release. */
extern void wc_reverse_release( void *proxy );

/* Enter the guest object's IUnknown::AddRef / ::Release.  No-ops for a
 * [local] interface, which has neither. */
extern void wc_guest_addref( void *guest, UINT iface );
extern void wc_guest_release( void *guest, UINT iface );

#endif  /* __WINE_WINECOM_PRIVATE_H */
