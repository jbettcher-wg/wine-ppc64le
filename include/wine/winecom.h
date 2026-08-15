/*
 * winecom -- the shared guest-COM proxy runtime for AMD64 guests on the
 * ppc64le port (static library libs/winecom).
 *
 * One instance of the runtime lives in each client module that links the
 * library (static-library state is per-linkee, deliberately): dlls/d3d12
 * carries one for the vkd3d-proton surface, native combase carries one for
 * the system-COM surface.  The two never share indices or interning
 * (system-com-design.md §4.2).
 *
 * The client supplies a `struct winecom_surface`: its generated marshal
 * tables (gen_winecom.py), its host invoker, its hand-written slot
 * functions, and the guest thunk modules whose published stub arrays become
 * the proxies' vtables.  Everything else -- interning, guest-vtable
 * materialisation with the attach IID cross-check, the dispatch loop, the
 * refuse-once bookkeeping, and the fail-closed wrap/refuse choke points --
 * is this library.
 *
 * The includer must have pulled in the Windows/NT types first (winternl.h;
 * AMD64_CONTEXT comes from winnt.h's cross-architecture context set).
 *
 * Copyright 2026 the ppc64le port authors
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 */

#ifndef __WINE_WINE_WINECOM_H
#define __WINE_WINE_WINECOM_H

/* Argument classes, written by gen_winecom.py.  cls[i] describes parameter
 * i counting AFTER `this`; its trap CONTEXT position is argument i+1. */
#define WINECOM_CA_PASS                 0
#define WINECOM_CA_IFACE_IN             1  /* translate-in (proxy -> host) */
#define WINECOM_CA_RIID                 2  /* the REFIID of a PPV_OUT pair */
#define WINECOM_CA_PPV_OUT              3  /* riid-typed void** out */
#define WINECOM_CA_RET_PTR              4  /* widl __ret aggregate pointer */
#define WINECOM_CA_EVENT                5  /* completion-event HANDLE */
#define WINECOM_CA_IFACE_ARR_IN         6  /* iface array + count param */
#define WINECOM_CA_IFACE_OUT_STATIC     7  /* Iface** out, type in xaux[i] */
#define WINECOM_CA_IFACE_ARR_OUT_STATIC 8  /* declared, no marshal plan yet */

#define WINECOM_F_RET_VOID    1  /* method returns void */
#define WINECOM_F_RET_VIA_ARG 2  /* sret: RAX = the __ret argument */
#define WINECOM_F_HAND        4  /* served by surface->hand_funcs[aux] */

/* Compatibility spellings for generated tables and client code that predate
 * the shared library (dlls/d3d12).  Same values, one authority. */
#define CA_PASS          WINECOM_CA_PASS
#define CA_IFACE_IN      WINECOM_CA_IFACE_IN
#define CA_RIID          WINECOM_CA_RIID
#define CA_PPV_OUT       WINECOM_CA_PPV_OUT
#define CA_RET_PTR       WINECOM_CA_RET_PTR
#define CA_EVENT         WINECOM_CA_EVENT
#define CA_IFACE_ARR_IN  WINECOM_CA_IFACE_ARR_IN
#define COMF_RET_VOID    WINECOM_F_RET_VOID
#define COMF_RET_VIA_ARG WINECOM_F_RET_VIA_ARG
#define COMF_HAND        WINECOM_F_HAND

struct winecom_slot
{
    const char *name;
    const char *refuse;         /* non-NULL: log once, answer E_NOTIMPL */
    const unsigned char *cls;   /* argc-1 classes; NULL = all CA_PASS */
    const unsigned char *xaux;  /* argc-1 per-parameter aux values
                                   (IFACE_OUT_STATIC: roster index); NULL */
    unsigned char argc;         /* including `this` */
    unsigned char flags;
    unsigned char aux;          /* CA_PPV_OUT: its RIID param index;
                                   WINECOM_F_HAND: hand function index */
    unsigned char aux2;         /* CA_IFACE_ARR_IN: count param index */
};

struct winecom_iface
{
    const char *name;
    GUID iid;
    UINT slot_count;
    const struct winecom_slot *slots;  /* NULL: identity row -- IUnknown
                                          slots served, the rest refused */
};

/* The host invoker: call `host`'s vtable slot with argc argument slots
 * (args[0] is replaced by host).  d3d12 crosses its unixlib here; system
 * COM calls the native vtable directly. */
typedef UINT64 (*winecom_invoke_fn)( void *host, UINT slot, UINT argc,
                                     UINT64 *args );
/* A hand-written slot: reads its own arguments out of the trap CONTEXT. */
typedef UINT64 (*winecom_hand_fn)( void *host, UINT slot,
                                   const AMD64_CONTEXT *ctx );

struct winecom_surface
{
    const char *name;                    /* for logs */
    const WCHAR * const *guest_modules;  /* candidate guest thunk modules
                                            publishing the stub arrays */
    UINT module_count;
    const struct winecom_iface *ifaces;  /* gen_winecom.py output, sorted */
    UINT iface_count;
    winecom_invoke_fn invoke;
    const winecom_hand_fn *hand_funcs;
    UINT hand_count;
};

/* Bind this linkee's runtime instance to `surface` and materialise the
 * guest vtables (idempotent, thread-safe; FALSE = failed, and stays
 * failed).  The IID cross-check against every loaded candidate module runs
 * here: generator drift is a load failure, not slot misalignment. */
extern BOOL winecom_attach( const struct winecom_surface *surface );

/* The COM-trap dispatch loop; the client's __wine_com_dispatch export calls
 * this after its own lazy initialisation.  Contract as established with
 * ntdll: STATUS_SUCCESS means fully served including ctx->Rax. */
extern NTSTATUS winecom_dispatch( UINT iface, UINT slot, AMD64_CONTEXT *ctx );

/* Intern a host interface pointer as a guest-callable proxy.  CONSUMES one
 * host reference; the returned proxy carries one guest reference.  NULL in,
 * NULL out.  Under WINEEMUNOCOMWRAP=1 (the negative control) the host
 * pointer is returned RAW -- the exact defect this runtime exists to fix --
 * so anything the mechanism carries must go red under it. */
extern void *winecom_wrap( void *host, UINT iface );

/* Proxy -> host pointer for a value KNOWN to be one of our proxies.
 * NULL-safe.  A pointer that is not one of our proxies answers NULL with an
 * ERR -- never a blind dereference. */
extern void *winecom_unwrap( void *maybe_proxy );

/* The conditional classifier both directions share (design §6.3), forward
 * half: one of our proxies -> its host pointer; NULL -> NULL; anything else
 * is a guest-implemented object, FALSE until reverse proxies exist. */
extern BOOL winecom_translate_in( void *guest_seen, void **host_out );

/* The single fail-closed choke point for riid-typed out interfaces: roster
 * hit -> wrap; miss -> release + *ppv = NULL + E_NOINTERFACE + ERR. */
extern HRESULT winecom_wrap_out_iface( HRESULT hr, const GUID *riid,
                                       void **ppv );

/* Wrap a statically-typed out interface in place (no-op on NULL). */
extern void winecom_wrap_static( void **p, UINT iface );

extern UINT winecom_iface_from_iid( const GUID *riid );  /* ~0u on miss */
extern UINT64 winecom_read_arg( const AMD64_CONTEXT *ctx, UINT n );
extern void winecom_host_release( void *host );

#endif  /* __WINE_WINE_WINECOM_H */
