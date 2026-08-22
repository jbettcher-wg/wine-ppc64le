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
#define WINECOM_F_RET_QWORD  16 /* the return value is an 8-byte scalar in the
                                   GUEST's own ABI.  An x86-64 guest never
                                   needed the distinction (everything comes
                                   back in RAX), which is why no table carried
                                   it; an i386 guest returns 64 bits in
                                   EDX:EAX and 32 in EAX alone, so a stub that
                                   does not know the width either truncates
                                   ID3D11Fence::GetCompletedValue or hands
                                   back a stale EDX as the upper half -- a
                                   plausible number, not a crash.  Set from
                                   the i386 clang oracle's sizeof of the
                                   declared return type, never from the type's
                                   NAME: SIZE_T is 8 bytes on one guest and 4
                                   on the other, and ID3D10Blob::GetBufferSize
                                   returning it gets NO flag for exactly that
                                   reason.
                                     These two spellings -- EAX, EDX:EAX --
                                   are the ONLY return classes the geometry
                                   can express.  i386 returns float and
                                   double in x87 ST(0), so a slot whose
                                   return type clang says is floating-point
                                   (GetResourceMinLOD, GetNPatchMode) gets
                                   NO WINECOM_F_I386_GEOM at all: reading its
                                   "return" out of EAX would hand back
                                   garbage AND leave ST(0) unpopped, an x87
                                   stack that is all NaN eight calls later. */
#define WINECOM_F_I386_GEOM  32 /* qwordmask and WINECOM_F_RET_QWORD were
                                   DERIVED, from clang's own i386 layout of
                                   every parameter of this slot, and together
                                   with argc they lay out the i386 stdcall
                                   frame: parameter i -- counting AFTER
                                   `this`, the same indexing as every other
                                   mask in this struct, NOT the stack-slot
                                   index -- occupies two 4-byte stack slots
                                   where qwordmask has bit i, one where it
                                   does not, and the callee pops
                                   4*argc + 4*popcount(qwordmask) bytes.
                                   Same reading rule as xmask: without this
                                   flag a zero qwordmask cannot be told from
                                   "the generator never looked" -- a table
                                   generated before this field existed, or a
                                   slot whose frame the mask cannot express (a
                                   by-value GUID is four slots, a float
                                   return is ST(0)) -- so a 32-bit lane MUST
                                   treat its absence as "no geometry" and
                                   fail closed, not as "every parameter is
                                   one slot".
                                     TODAY THAT RULE IS CONTRACT, NOT
                                   ENFORCEMENT.  No code in this tree reads
                                   qwordmask, WINECOM_F_RET_QWORD or this
                                   flag yet -- there is no i386 consumer, and
                                   the 64-bit dispatcher never looks at them
                                   -- so "fails closed" describes what the
                                   first 32-bit reader MUST be written to do,
                                   not something any code currently does.  Do
                                   not mistake this paragraph for a check
                                   that exists. */

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
    unsigned char narrowmask;   /* bit i: parameter i is a by-value integer
                                   NARROWER THAN 32 BITS, and the dispatcher
                                   must extend it before handing it to the
                                   native callee.

                                   THE TWO ABIs DISAGREE ABOUT WHOSE JOB THAT
                                   IS.  MS-x64 leaves the upper bits of a
                                   register holding a narrow argument
                                   UNDEFINED -- clang really does emit
                                   `movw $0x1, %dx`, a 16-bit store that leaves
                                   RDX's top 48 bits as whatever was there --
                                   and requires the CALLEE to ignore them.
                                   ELFv2 says the opposite: arguments smaller
                                   than a doubleword are extended BY THE
                                   CALLER, and a ppc64 callee is compiled to
                                   trust that.  So the raw register handed
                                   straight through arrives as garbage above
                                   the declared width.

                                   [MEASURED] IWMSyncReader::GetStreamSelected
                                   and ::GetOutputNumberForStream both take a
                                   WORD stream number.  A guest passing 1 was
                                   read by Wine's own winegstreamer code as
                                   0x40000001: one call answered E_INVALIDARG
                                   and the other returned output number
                                   0x40000000.  A WRONG NUMBER, NOT A CRASH --
                                   the same class as the ldexp() exponent the
                                   FP marshalling work found, and invisible to
                                   any check that only asks whether the call
                                   returned.

                                   32-bit arguments IN REGISTERS need
                                   nothing: x86-64 zero-extends every 32-bit
                                   register write to 64 bits by hardware rule,
                                   so a DWORD in RDX is already clean.  A
                                   32-bit argument ON THE STACK is a different
                                   animal -- see dwordmask below, measured on
                                   ID3D12Device::CopyDescriptors.

                                   A table generated before this field existed
                                   has narrowmask 0 and behaves exactly as it
                                   did.  That is deliberately NOT fail-closed,
                                   because the alternative -- refusing every
                                   row on every surface whose generator has not
                                   been taught this yet -- would take working
                                   lanes away to fix a hazard they may not
                                   have; the lanes that have not been
                                   regenerated are named in
                                   ppc64le/mf/README.md instead. */
    unsigned char narrowwide;   /* bit i: that parameter is 16 bits, not 8.
                                   Same shape as fpwide above and for the same
                                   reason: the width has to be known to move
                                   the value correctly. */
    unsigned char narrowsign;   /* bit i: that parameter is SIGNED, so it must
                                   be sign-extended rather than zero-extended.
                                   SHORT and CHAR are the ones that care: a
                                   SHORT of -1 arrives as DX=0xFFFF and has to
                                   reach the callee as 0xFFFFFFFFFFFFFFFF, not
                                   as 0x000000000000FFFF. */
    unsigned short dwordmask;   /* bit i: parameter i is a FOUR-BYTE by-value
                                   slot.  The register story above does not
                                   cover the stack: an MS-x64 caller stores a
                                   32-bit stack argument with a 32-bit store
                                   and leaves whatever was in the slot's upper
                                   half, and an ELFv2 callee at -O2 is
                                   entitled to trust the caller extended it.

                                   [MEASURED] Cyberpunk 2077's
                                   ID3D12Device::CopyDescriptors passes the
                                   descriptor heap type as its seventh
                                   argument -- a stack slot -- and vkd3d's
                                   64-bit switch missed every case of an enum
                                   whose low half said 0 -- 32768 refused
                                   descriptor copies in the first minute of a
                                   run, no crash anywhere.  The flat lane has carried the same
                                   fix since its version-6 width descriptors
                                   (dlls/ntdll/signal_ppc64.c THUNK_WIDTH,
                                   measured there on mspatcha).

                                   unsigned short where the elder masks are
                                   unsigned char, because refusing a slot for
                                   having a 32-bit argument in position 9
                                   (UpdateTileMappings' flags) would take a
                                   served row away; sixteen bits cover every
                                   argc this surface has.  A table generated
                                   before this field existed has 0 here and
                                   behaves exactly as it did -- same
                                   deliberate not-fail-closed rule as
                                   narrowmask, and the lanes that have not
                                   been regenerated keep their latent stack
                                   hazard until they are. */
    unsigned short dwordsign;   /* bit i: that four-byte parameter is SIGNED
                                   (INT, LONG, BOOL -- int underneath -- and
                                   any enum whose UNDERLYING type is signed),
                                   so it is sign-extended; unsigned ones are
                                   zero-extended.  ELFv2's own rule, applied
                                   as written -- to the declared type as
                                   CLANG parses it, never to a name-list
                                   guess.  "An enum is int underneath" is
                                   NOT a rule the generators use any more:
                                   DXGI_COLOR_SPACE_TYPE carries the
                                   enumerator DXGI_COLOR_SPACE_CUSTOM =
                                   0xffffffff, which makes its underlying
                                   type UNSIGNED in the C the native callee
                                   is compiled as, so it zero-extends --
                                   sign-extending it handed the callee
                                   0xFFFFFFFFFFFFFFFF where the ABI demands
                                   0x00000000FFFFFFFF. */
    unsigned short qwordmask;   /* bit i: parameter i is EIGHT bytes in the
                                   i386 guest's own ABI -- TWO 4-byte stdcall
                                   stack slots, where every unmarked parameter
                                   of the slot is one.  Meaningful only under
                                   WINECOM_F_I386_GEOM, whose banner has the
                                   frame arithmetic.

                                   This mask exists because "8-byte class on
                                   the x86-64 guest" names TWO different i386
                                   widths and no field could tell them apart:
                                   HANDLE / HWND / SIZE_T / ULONG_PTR and
                                   every pointer shrink to 4 bytes and ONE
                                   slot on i386, while UINT64 /
                                   D3D12_GPU_VIRTUAL_ADDRESS stay 8 bytes and
                                   TWO -- so ID3D11Fence::Signal(fence,
                                   UINT64) has its value at a different stack
                                   offset than any pointer-taking neighbour,
                                   and a 32-bit lane that guessed from the
                                   64-bit tables would read a value that is
                                   half fence-value, half whatever came next:
                                   plausible, wrong, and silent.  The widths
                                   come from the i386 clang oracle
                                   (gen_winecom.py asks sizeof for the exact
                                   declared type, the ppc64le/dxvk/layout32.py
                                   mechanism), NEVER from a name list -- a
                                   name list is how HANDLE and UINT64 ended up
                                   in one bucket to begin with.

                                   unsigned short like dwordmask, sixteen
                                   parameter positions; a slot with an 8-byte
                                   parameter past bit 15 REFUSES AT GENERATION
                                   TIME rather than truncating the mask.  A
                                   table generated before this field existed
                                   has 0 here AND lacks WINECOM_F_I386_GEOM,
                                   so a 32-bit reader sees "no geometry", not
                                   "all narrow" -- that pairing is what makes
                                   this append safe. */
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
