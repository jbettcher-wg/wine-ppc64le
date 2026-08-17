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
#define WINECOM_CA_IFACE_ARR_OUT_STATIC 8  /* Iface** out ARRAY: element type
                                              in xaux[i], element count in the
                                              by-value parameter caux[i] */

#define WINECOM_F_RET_VOID    1  /* method returns void */
#define WINECOM_F_RET_VIA_ARG 2  /* sret: RAX = the __ret argument */
#define WINECOM_F_HAND        4  /* served by surface->hand_funcs[aux] */
#define WINECOM_F_REV         8  /* THE REVERSE PLAN IS COMPLETE.  cls/xaux/
                                    fpmask/fpwide describe every parameter of
                                    this slot, so the reverse direction may
                                    serve it even though the forward direction
                                    is served some other way -- by a hand
                                    function, or not at all.
                                      The generators set this on exactly one
                                    shape today: a slot whose only problem is
                                    that it passes a float BY VALUE.  The
                                    forward invoker calls a native vtable slot
                                    with the widest INTEGER form and cannot
                                    place one; the reverse dispatcher marshals
                                    its own registers and can.  Every other
                                    refusal -- a PROPVARIANT, a by-value GUID,
                                    a struct that reaches an interface pointer,
                                    an untyped void** -- is about the SIGNATURE
                                    and stands in both directions, and does not
                                    get this flag. */

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
    const unsigned char *xaux;  /* argc-1 per-parameter aux values: the ROSTER
                                   INDEX of the interface type of a
                                   CA_IFACE_IN, CA_IFACE_OUT_STATIC or
                                   CA_IFACE_ARR_OUT_STATIC parameter; NULL.
                                   The forward direction needs it only for the
                                   OUT classes -- an IN pointer is recognised
                                   by identity, not by type -- but the REVERSE
                                   direction needs the type of an IN parameter
                                   too, because a native object arriving as an
                                   argument of a guest method has to be given
                                   one of the rostered guest vtables. */
    unsigned char argc;         /* including `this` */
    unsigned char flags;
    unsigned char aux;          /* CA_PPV_OUT: its RIID param index;
                                   WINECOM_F_HAND: hand function index */
    unsigned char aux2;         /* CA_IFACE_ARR_IN: count param index */
    const unsigned char *caux;  /* argc-1 per-parameter count-parameter
                                   indices (CA_IFACE_ARR_OUT_STATIC, whose
                                   counts differ per parameter within one
                                   slot -- OMGetRenderTargetsAndUnordered-
                                   AccessViews has two); NULL.  Appended
                                   LAST so the d3d12 lane's generated rows,
                                   which name eight members positionally,
                                   keep meaning what they meant. */
    unsigned char fpmask;       /* bit i (parameter i counting after `this`):
                                   this parameter travels in a FLOATING-POINT
                                   register on both ABIs.  Emitted even on rows
                                   the FORWARD direction refuses for being
                                   float-bearing, because the reverse direction
                                   marshals its own registers and can serve
                                   them; `refuse` still governs the forward
                                   call.  Appended after caux, same rule. */
    unsigned char fpwide;       /* bit i: that parameter is a `double`, not a
                                   `float`.  MS-x64 puts a single in the low
                                   half of its XMM register and ELFv2 puts it
                                   in an FPR in double format, so the width has
                                   to be known to move one between them. */
    unsigned char xmask;        /* bit i: xaux[i] IS A ROSTER INDEX the
                                   generator filled in for parameter i.
                                   Without this there is no way to tell "the
                                   generator wrote interface 0 here" from "the
                                   generator wrote nothing here", because a
                                   row's xaux[] is one array shared by every
                                   parameter and an untouched slot reads 0 --
                                   which is a REAL interface (the roster is
                                   sorted, so index 0 is whatever sorts first).
                                   The forward direction never noticed, because
                                   it only reads xaux for the OUT classes it
                                   knows the generator wrote.  The REVERSE
                                   direction reads it for CA_IFACE_IN too, and
                                   a table written before this field existed
                                   has xmask 0 -- so every such parameter fails
                                   CLOSED and is refused by name, which is the
                                   only safe reading of "no information".
                                   Appended last, same rule as caux. */
};

/* winecom_iface flags */
#define WINECOM_IF_LOCAL 1  /* NOT IUnknown-derived: slot 0 is a real method,
                               there is no QueryInterface/AddRef/Release and no
                               reference count.  XAudio2's voices and its two
                               callback interfaces are the corpus's [local]
                               ones.  The FORWARD dispatcher does not read this
                               (a client with [local] interfaces claims them
                               before winecom_dispatch sees them); the REVERSE
                               dispatcher does, because there is no second
                               dispatcher to claim them in. */

struct winecom_iface
{
    const char *name;
    GUID iid;
    UINT slot_count;
    const struct winecom_slot *slots;  /* NULL: identity row -- IUnknown
                                          slots served, the rest refused */
    UINT flags;                        /* WINECOM_IF_*; appended last */
};

/* The host invoker: call `host`'s vtable slot with argc argument slots
 * (args[0] is replaced by host).  d3d12 crosses its unixlib here; system
 * COM calls the native vtable directly. */
typedef UINT64 (*winecom_invoke_fn)( void *host, UINT slot, UINT argc,
                                     UINT64 *args );
/* A hand-written slot: reads its own arguments out of the trap CONTEXT.
 * The CONTEXT is NOT const, because a slot returning a float has to write
 * ctx->FltSave.XmmRegisters[0] itself -- MS-x64 returns floats there and not
 * in RAX, and the dispatcher only knows how to set RAX.  That is the same
 * write dlls/ntdll/signal_ppc64.c's flat FP path makes, at the same offset,
 * for the same reason. */
typedef UINT64 (*winecom_hand_fn)( void *host, UINT slot, AMD64_CONTEXT *ctx );

/* winecom_surface flags */
#define WINECOM_SF_REVERSE 1  /* This surface's native side may be handed
                                 REVERSE PROXIES -- native vtable objects whose
                                 slots enter guest code through the emulator --
                                 for the guest-implemented objects it is given.
                                 OFF by default, and that default is the whole
                                 point: a surface whose invoker crosses a
                                 unixlib (the d3d12 lane) must never receive
                                 one, because a reverse proxy is a PE-side
                                 object and the unix side would call its vtable
                                 with no emulator underneath.  A surface that
                                 calls native PE vtables directly may set it. */

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
    UINT flags;                          /* WINECOM_SF_*; appended last, so a
                                            client that predates it keeps
                                            exactly the behaviour it had */
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

/* ----------------------------------------------------- the reverse direction
 *
 * A REVERSE PROXY is the mirror of the object above: a NATIVE vtable wrapping
 * a guest interface pointer, whose per-slot stubs marshal ELFv2 arguments into
 * MS-x64 by the SAME generated slot tables the forward direction reads, enter
 * the guest method through the emulator's nested-run primitive, and marshal
 * the result back.  Interning and identity are symmetric with the forward
 * half, and that symmetry is what makes COM work rather than merely run: a
 * reverse proxy handed BACK to the guest unwraps to the original guest
 * pointer, a forward proxy handed back to native code unwraps to the original
 * host pointer, and the same object compared on either side is the same
 * pointer on that side.
 *
 * Only a surface with WINECOM_SF_REVERSE gets any of this; everything below
 * fails closed and loudly on a surface without it.
 */

/* Intern a guest interface pointer as a native-callable object.  CONSUMES one
 * GUEST reference (the mirror of winecom_wrap consuming a host one); the
 * returned proxy carries one native reference.  NULL in, NULL out.  Under
 * WINEEMUNOCOMWRAP=1 the guest pointer is returned RAW -- x86-64 bytes for
 * native code to call -- which is the defect this exists to fix. */
extern void *winecom_reverse_wrap( void *guest, UINT iface );

/* Reverse proxy -> guest pointer for a value KNOWN to be one of ours.
 * NULL-safe; anything else answers NULL with an ERR. */
extern void *winecom_reverse_unwrap( void *maybe_proxy );

/* GUEST value -> NATIVE value, the full classifier (design §6.3).  One of our
 * forward proxies unwraps to its host pointer; NULL stays NULL; anything else
 * is a guest-implemented object and is given a reverse proxy of type `iface`
 * (~0u = no type is known, which is the fail-closed answer and FALSE).
 *
 * The native pointer is BORROWED for the duration of one call: on TRUE the
 * caller MUST pass it to winecom_to_native_end() once the call it translated
 * for has returned.  That is what lets a callee which took its own reference
 * (a work queue holding an IMFAsyncCallback) keep the object alive while a
 * callee that did not lets it go. */
extern BOOL winecom_to_native( void *guest_seen, UINT iface, void **native_out );
extern void winecom_to_native_end( void *native );

/* NATIVE value -> GUEST value, the mirror: one of our reverse proxies unwraps
 * to its guest pointer; NULL stays NULL; anything else is a native object and
 * is given a forward proxy of type `iface`.  Used for the IN parameters of a
 * REVERSE call -- a native IMFAsyncResult arriving at a guest-implemented
 * IMFAsyncCallback::Invoke -- so a forward proxy is minted inside a reverse
 * call and the two directions meet in the middle.
 *
 * CONSUMES nothing and takes a reference for the guest when it wraps, exactly
 * as an [in] parameter's caller keeps its own. */
extern BOOL winecom_to_guest( void *native_seen, UINT iface, void **guest_out );
extern void winecom_to_guest_end( void *guest );

/* The old two-argument spelling: `guest_seen` with no type, so a
 * guest-implemented object is refused rather than reverse-proxied.  Every
 * caller that predates reverse proxies keeps exactly the behaviour it had. */
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
