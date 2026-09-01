/*
 * winecom -- REVERSE proxies: a guest-implemented COM object handed to native
 * ppc64 code, which then calls its methods.
 *
 * This is the exact mirror of winecom.c.  There, a NATIVE interface pointer is
 * given a GUEST vtable -- the trap-stub array a spec2thunk COM module
 * publishes -- so an x86-64 caller's `call [rax+0x18]` lands on a trap that
 * ntdll routes back into the dispatch loop.  Here, a GUEST interface pointer
 * is given a NATIVE vtable, so a ppc64 caller's `bctrl` through slot n lands
 * on one of the stubs below, which marshals the ELFv2 argument registers into
 * an MS-x64 argument block and enters the guest method through the emulator.
 *
 * WHY THE MIRROR HAS TO EXIST.  Six subsystems filed the same need and it is
 * always the same sentence: the API's whole contract is "you implement it, we
 * call it".  Media Foundation's async model IS IMFAsyncCallback.  XAudio2
 * reports buffer completion through IXAudio2VoiceCallback, from its mixer
 * thread.  DirectSound aggregates through pUnkOuter.  Without the mirror all
 * of that is refused, which is honest but is also most of what those APIs are.
 *
 * THE THREE PIECES.
 *
 * 1. THE NATIVE VTABLE.  rev_stub_0..rev_stub_63 below are ordinary C
 *    functions whose PROTOTYPE is a deliberate over-declaration: eight integer
 *    parameters and thirteen doubles, which under ELFv2 is exactly r3-r10 and
 *    f1-f13 -- the whole argument register file, with nothing spilling to the
 *    parameter save area.  A native caller calling slot n with its own real
 *    signature leaves its arguments in some subset of those registers; the
 *    stub reads all of them and the slot's GENERATED class table says which
 *    are real and which register each came from.  That is why there is one
 *    stub per SLOT NUMBER and not one per method: the interface is carried by
 *    `this`, and the shape is carried by the table.
 *
 *    A float is the case that makes the register file worth reading whole.
 *    ELFv2 puts a floating-point argument in the next FPR and RESERVES its
 *    integer slot, so a method (float, int) leaves the int in r5 and not r4 --
 *    which is exactly what walking the class table reproduces.
 *
 * 2. THE ENTRY.  dlls/ntdll/signal_ppc64.c's guest-callback trampoline pool
 *    carries four integer arguments into a guest function, which is not enough
 *    for a COM method (up to eight, some floating point, some past the
 *    register file).  So the ONE argument it does carry is a parameter block,
 *    and the guest end of the call is a small x86-64 shim -- 109 bytes,
 *    written here into anonymous executable memory exactly as
 *    call_guest_function_args writes its own -- which unpacks the block into
 *    real MS-x64 registers and stack slots, calls the guest method, and writes
 *    RAX and XMM0 back.  ONE trampoline for the whole mechanism, because the
 *    per-call target is IN the block: no pool lookup and no loader lock on a
 *    path that runs on a realtime audio thread.
 *
 * 3. IDENTITY.  Reverse proxies are interned by (guest pointer, interface),
 *    exactly as forward proxies are interned by (host pointer, interface), and
 *    the two halves recognise each other:
 *
 *      winecom_wrap( a reverse proxy )      -> the original guest pointer
 *      winecom_to_native( a forward proxy ) -> the original host pointer
 *      winecom_to_guest( a reverse proxy )  -> the original guest pointer
 *
 *    Round-trip identity is not a nicety.  COM callers compare interface
 *    pointers for identity, aggregate on the assumption that
 *    QueryInterface(IID_IUnknown) is canonical, and unregister a sink by
 *    handing back the pointer they registered.  A mechanism that minted a
 *    fresh wrapper each way would break every one of those quietly.
 *
 * WHAT IS STILL REFUSED, and refused HERE rather than papered over: a slot
 * whose forward row carries a `refuse` string (the tables' own judgement about
 * its signature stands in both directions), a hand-written slot (its signature
 * lives in a C function, so there is nothing to marshal by), an aggregate
 * return, a completion-event handle, and anything wider than the
 * eight-argument register file.  Each says which and why, once.  (An
 * interface ARRAY left this list 2026-09-01: the CA_IFACE_ARR_IN arm
 * forward-mints each element for the guest -- IWbemObjectSink::Indicate.)
 *
 * The ONE exception is spelled by the generator rather than assumed here:
 * WINECOM_F_REV on a row means "cls/xaux/fpmask describe this signature
 * completely", and the generators set it on exactly one shape -- a slot whose
 * only problem is a BY-VALUE FLOAT.  The forward invoker calls a native vtable
 * slot in its widest INTEGER form and cannot place one; this dispatcher
 * marshals its own registers and can.  IMFClockStateSink::OnClockSetRate is
 * the method that shape is named after, and it is a sink -- i.e. it is only
 * ever called in this direction anyway.
 *
 * THE RETURN VALUE is the guest's RAX with its low 32 bits SIGN-EXTENDED, the
 * same rule and for the same reason as the trampoline pool's narrow form: every
 * COM method in every roster on this port returns HRESULT, ULONG or void, an
 * x86-64 callee writing EAX zeroes the upper half, and an ELFv2 caller is
 * entitled to assume its callee extended a 32-bit result -- so E_NOTIMPL must
 * not arrive as 0x0000000080004001 and read as a positive number.
 *
 * Copyright 2026 the ppc64le port authors
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 */

#include <stdarg.h>
#include <string.h>

#include "ntstatus.h"
#define WIN32_NO_STATUS

#include "windef.h"
#include "winbase.h"
#include "winternl.h"
#include "wine/debug.h"
#include "wine/winecom.h"

#include "winecom_private.h"

WINE_DEFAULT_DEBUG_CHANNEL(winecom);

/* The widest COM method this mechanism carries: `this` plus seven parameters,
 * which is r3-r10 on the ELFv2 side and RCX/RDX/R8/R9 plus four stack slots on
 * the MS-x64 side.  Nothing on any roster this port serves is wider; a wider
 * one is refused by name rather than truncated. */
#define REV_MAX_ARGS 8

/* Floating-point parameters live in f1-f13 under ELFv2.  A method with more
 * than seven parameters is already refused above, so this is never the binding
 * bound -- it is the size of the register file the stubs declare, written once
 * so the declaration and the marshaller cannot disagree. */
#define REV_MAX_FPR 13

/* One native stub per SLOT NUMBER, shared by every interface: the interface is
 * carried by `this`, so slot 7 of one interface and slot 7 of another want the
 * same stub.  53 is the widest interface on any roster this port has
 * (IDirectMusicPerformance8); 64 leaves room and keeps the vtable small. */
#define REV_MAX_SLOTS 64

/* ------------------------------------------------------------- the entry */

/* The parameter block the x86-64 shim reads.  Its offsets are baked into the
 * shim's bytes, so the two are checked against each other below. */
struct rev_block
{
    void   *target;                 /* 0x00  the guest method */
    UINT64  args[REV_MAX_ARGS];     /* 0x08  MS-x64 arguments, `this` first */
    UINT64  rax;                    /* 0x48  what the guest returned */
    UINT64  xmm0;                   /* 0x50  ...if it returned it in XMM0 */
};

C_ASSERT( FIELD_OFFSET(struct rev_block, target) == 0x00 );
C_ASSERT( FIELD_OFFSET(struct rev_block, args)   == 0x08 );
C_ASSERT( FIELD_OFFSET(struct rev_block, rax)    == 0x48 );
C_ASSERT( FIELD_OFFSET(struct rev_block, xmm0)   == 0x50 );

/* The guest end of a reverse call, in x86-64 machine code, entered with
 * RCX = &block through the trampoline pool's four-argument primitive:
 *
 *      push rbp / mov rbp,rsp / push rbx / and rsp,-16 / sub rsp,0x40
 *      rbx = rcx                             the block
 *      rax = [rbx+0x00]                      the target
 *      [rsp+0x20 .. +0x38] = args[4..7]      the stack arguments
 *      xmm0..xmm3 = args[0..3]               both files, for a position this
 *      rcx,rdx,r8,r9 = args[0..3]            one piece of code cannot type
 *      call rax
 *      [rbx+0x48] = rax ; [rbx+0x50] = xmm0
 *      lea rsp,[rbp-8] / pop rbx / pop rbp / ret
 *
 * `and rsp,-16` is not defensive clutter.  An MS-x64 callee may assume RSP+8
 * is 16-byte aligned at entry and use aligned SSE stores on its own locals,
 * and this shim is entered from a run-entry path that aligned the stack for a
 * different call shape; aligning here makes the guarantee local and true.
 *
 * Loading XMM0-3 as well as RCX/RDX/R8/R9 is the varargs rule, deliberately:
 * one piece of code serves every signature, so it cannot know whether argument
 * 2 is an integer or a float.  Loading both is always correct -- an MS-x64
 * callee reads exactly the register its own prototype names -- and costs four
 * moves.
 *
 * RBX is non-volatile in MS-x64, which is why the block survives the call in
 * it.  R11 would not (a callee may clobber it), and a stack slot would have to
 * be placed clear of the shadow space the callee owns. */
static const BYTE rev_shim_code[] =
{
    0x55, 0x48, 0x89, 0xe5, 0x53, 0x48, 0x83, 0xe4,
    0xf0, 0x48, 0x83, 0xec, 0x40, 0x48, 0x89, 0xcb,
    0x48, 0x8b, 0x03, 0x4c, 0x8b, 0x53, 0x28, 0x4c,
    0x89, 0x54, 0x24, 0x20, 0x4c, 0x8b, 0x53, 0x30,
    0x4c, 0x89, 0x54, 0x24, 0x28, 0x4c, 0x8b, 0x53,
    0x38, 0x4c, 0x89, 0x54, 0x24, 0x30, 0x4c, 0x8b,
    0x53, 0x40, 0x4c, 0x89, 0x54, 0x24, 0x38, 0xf2,
    0x0f, 0x10, 0x43, 0x08, 0xf2, 0x0f, 0x10, 0x4b,
    0x10, 0xf2, 0x0f, 0x10, 0x53, 0x18, 0xf2, 0x0f,
    0x10, 0x5b, 0x20, 0x48, 0x8b, 0x53, 0x10, 0x4c,
    0x8b, 0x43, 0x18, 0x4c, 0x8b, 0x4b, 0x20, 0x48,
    0x8b, 0x4b, 0x08, 0xff, 0xd0, 0x48, 0x89, 0x43,
    0x48, 0xf2, 0x0f, 0x11, 0x43, 0x50, 0x48, 0x8d,
    0x65, 0xf8, 0x5b, 0x5d, 0xc3,
};

/* The trampoline dlls/ntdll/signal_ppc64.c minted for the shim: a native
 * function pointer carrying four integer arguments into guest code.  ONE for
 * the whole mechanism, so the hot path takes no lock and does no lookup. */
static ULONG_PTR (*rev_enter)( ULONG_PTR a0, ULONG_PTR a1, ULONG_PTR a2, ULONG_PTR a3 );

/* -------------------------------------------------------- reverse proxies */

struct rev_proxy
{
    const void **native_vtbl;  /* first member: this IS the COM object native
                                  code sees -- *(void**)proxy is its vtable */
    void        *guest;        /* the guest interface pointer */
    LONG         refs;         /* native-visible refcount, served here */
    UINT         iface;        /* index into wc_surface->ifaces[] */
    struct rev_proxy *next;    /* intern-table chain */
};

#define REV_BUCKETS 128

static struct rev_proxy *rev_intern[REV_BUCKETS];

/* THE one native vtable.  Every reverse proxy points at it, whatever its
 * interface, because slot n's stub is the same code for every interface -- so
 * "is this pointer one of ours" is a single pointer comparison, with no range
 * arithmetic and no table walk, on the path an audio callback takes. */
static const void *rev_vtbl[REV_MAX_SLOTS];

static unsigned char *rev_refused;   /* one byte per (iface, slot) */
static UINT *rev_slot_base;
static UINT rev_total_slots;
static BOOL rev_enabled;

static UINT rev_bucket( const void *guest, UINT iface )
{
    return (UINT)((((ULONG_PTR)guest >> 4) + iface * 131u) % REV_BUCKETS);
}

static const struct winecom_iface *rev_iface( UINT iface )
{
    return &wc_surface->ifaces[iface];
}

static BOOL rev_is_local( UINT iface )
{
    return (rev_iface( iface )->flags & WINECOM_IF_LOCAL) != 0;
}

static void rev_refuse_once( UINT iface, UINT slot, const char *name, const char *why )
{
    unsigned char *flag;

    if (!rev_refused || iface >= wc_surface->iface_count ||
        slot >= rev_iface( iface )->slot_count)
    {
        FIXME( "%s: refusing a reverse call: %s\n", wc_surface->name, why );
        return;
    }
    flag = &rev_refused[rev_slot_base[iface] + slot];
    if (*flag) return;
    *flag = 1;
    FIXME( "%s: refusing the REVERSE call %s (iface %u slot %u): %s\n",
           wc_surface->name, name ? name : rev_iface( iface )->name, iface,
           slot, why );
}

/* --------------------------------------------------------- entering guest */

/* Run one guest method.  Everything about the crossing is in the block, so
 * this is the only place the emulator is entered and therefore the only place
 * to instrument when the cost of a crossing is in question. */
static void rev_enter_guest( struct rev_block *b )
{
    b->rax = b->xmm0 = 0;
    TRACE( "entering guest method %p, this %p\n", b->target,
           (void *)(ULONG_PTR)b->args[0] );
    rev_enter( (ULONG_PTR)b, 0, 0, 0 );
}

/* -> the guest method at `slot` of `guest`'s own vtable.  Guest memory IS host
 * memory on this port, so the vtable is READ directly; it is the CALL that
 * needs the emulator, not the load. */
static void *rev_guest_method( void *guest, UINT slot )
{
    void **vtbl = *(void ***)guest;
    return vtbl[slot];
}

void wc_guest_addref( void *guest, UINT iface )
{
    struct rev_block b = { 0 };

    if (!guest || !rev_enter || rev_is_local( iface )) return;
    b.target = rev_guest_method( guest, 1 );
    b.args[0] = (UINT64)(ULONG_PTR)guest;
    rev_enter_guest( &b );
}

void wc_guest_release( void *guest, UINT iface )
{
    struct rev_block b = { 0 };

    if (!guest || !rev_enter || rev_is_local( iface )) return;
    b.target = rev_guest_method( guest, 2 );
    b.args[0] = (UINT64)(ULONG_PTR)guest;
    rev_enter_guest( &b );
}

/* ----------------------------------------------------------- interning */

static struct rev_proxy *rev_from_pointer( void *ptr )
{
    struct rev_proxy *cand = ptr;
    struct rev_proxy *p;
    UINT bucket;

    if (!ptr || !rev_enabled) return NULL;
    /* One word, one comparison: a reverse proxy's vtable pointer IS the single
     * shared stub array, whose address is private to this file.  Anything else
     * -- a native object, a forward proxy, a guest object -- stops here. */
    if ((const void *)cand->native_vtbl != (const void *)rev_vtbl) return NULL;

    /* ...then confirm it is interned, reading the candidate's own fields to
     * find the chain, exactly as the forward half confirms a proxy.  The
     * comparison above has already established that the object is one of ours,
     * so these fields are ours to read. */
    bucket = rev_bucket( cand->guest, cand->iface );
    RtlEnterCriticalSection( &wc_cs );
    for (p = rev_intern[bucket]; p; p = p->next)
        if (p == cand) break;
    RtlLeaveCriticalSection( &wc_cs );
    if (!p)
        ERR( "%p carries the reverse-proxy vtable but is not interned; "
             "refusing to treat it as one\n", ptr );
    return p;
}

void *wc_reverse_guest( void *ptr )
{
    struct rev_proxy *p = rev_from_pointer( ptr );
    return p ? p->guest : NULL;
}

void *winecom_reverse_unwrap( void *maybe_proxy )
{
    struct rev_proxy *p;

    if (!maybe_proxy) return NULL;
    if (!(p = rev_from_pointer( maybe_proxy )))
    {
        ERR( "%p is not one of our reverse proxies; refusing to unwrap it\n",
             maybe_proxy );
        return NULL;
    }
    return p->guest;
}

void *winecom_reverse_wrap( void *guest, UINT iface )
{
    struct rev_proxy *p;
    UINT bucket;

    if (!guest) return NULL;
    if (!wc_ready() || !rev_enabled)
    {
        ERR( "%s: a guest-implemented object (%p) needs a reverse proxy and "
             "this surface does not enable them (WINECOM_SF_REVERSE); "
             "refusing rather than handing native code an x86-64 vtable\n",
             wc_surface ? wc_surface->name : "winecom", guest );
        return NULL;
    }
    if (iface >= wc_surface->iface_count)
    {
        ERR( "%s: no roster type for the guest object %p; refusing to build a "
             "reverse proxy with no slot table behind it\n",
             wc_surface->name, guest );
        return NULL;
    }
    if (rev_iface( iface )->slot_count > REV_MAX_SLOTS)
    {
        ERR( "%s: %s has %u slots and the reverse vtable has %u; refusing\n",
             wc_surface->name, rev_iface( iface )->name,
             rev_iface( iface )->slot_count, REV_MAX_SLOTS );
        return NULL;
    }
    if (wc_nowrap())
    {
        ERR( "WINEEMUNOCOMWRAP: handing raw guest %s %p to native code\n",
             rev_iface( iface )->name, guest );
        return guest;
    }

    bucket = rev_bucket( guest, iface );
    RtlEnterCriticalSection( &wc_cs );
    for (p = rev_intern[bucket]; p; p = p->next)
        if (p->guest == guest && p->iface == iface) break;
    if (p)
    {
        p->refs++;
        RtlLeaveCriticalSection( &wc_cs );
        /* The caller handed us a guest reference for an object we already hold
         * one for: the surplus goes back, exactly as winecom_wrap drops a
         * surplus host reference. */
        wc_guest_release( guest, iface );
        return p;
    }
    if (!(p = RtlAllocateHeap( NtCurrentTeb()->Peb->ProcessHeap, 0, sizeof(*p) )))
    {
        RtlLeaveCriticalSection( &wc_cs );
        ERR( "out of memory interning guest %s %p\n",
             rev_iface( iface )->name, guest );
        wc_guest_release( guest, iface );
        return NULL;
    }
    p->native_vtbl = rev_vtbl;
    p->guest = guest;
    p->refs = 1;
    p->iface = iface;
    p->next = rev_intern[bucket];
    rev_intern[bucket] = p;
    RtlLeaveCriticalSection( &wc_cs );
    TRACE( "reverse-wrapped guest %s %p as native proxy %p\n",
           rev_iface( iface )->name, guest, p );
    return p;
}

static ULONG rev_addref( struct rev_proxy *p )
{
    ULONG refs;

    RtlEnterCriticalSection( &wc_cs );
    refs = ++p->refs;
    RtlLeaveCriticalSection( &wc_cs );
    return refs;
}

/* THE LIFETIME EDGE, and it is the one thing here that cannot be made safe by
 * construction.  The object's reference count is the GUEST's: a reverse proxy
 * holds exactly one guest reference for its whole life and hands it back here.
 * If native code is still holding the proxy when the guest tears its object
 * down anyway -- a game that frees its callback while an XAudio2 voice still
 * points at it -- the guest has broken the COM rules and the next reverse call
 * enters freed guest memory.  That is the same failure a native-only program
 * would have, at the same moment, for the same reason; this layer neither adds
 * it nor can remove it.  What it can do is make it legible, which is why the
 * destruction is traced with BOTH pointers. */
static ULONG rev_release( struct rev_proxy *p )
{
    struct rev_proxy **link;
    void *guest = NULL;
    UINT iface = p->iface;
    UINT bucket;
    ULONG refs;

    /* A [local] interface HAS NO REFERENCE COUNT.  IXAudio2VoiceCallback is
     * the case: XAudio2 stores the pointer a CreateSourceVoice gave it and
     * calls it for the voice's whole life without ever AddRef'ing, because
     * there is nothing to AddRef -- slot 1 of that vtable is
     * OnVoiceProcessingPassEnd.  So the proxy for one is PERMANENT: interned
     * once, never freed, and the object's lifetime stays the application's
     * business exactly as it is on Windows.  Interning bounds how many there
     * can be, which is the only reason this is affordable.
     *
     * Freeing one instead would hand XAudio2's mixer thread a dangling native
     * vtable at the next buffer boundary, and it would do it on the ONE thread
     * where a fault is least recoverable. */
    if (rev_is_local( p->iface )) return p->refs;

    bucket = rev_bucket( p->guest, p->iface );
    RtlEnterCriticalSection( &wc_cs );
    refs = --p->refs;
    if (!refs)
    {
        for (link = &rev_intern[bucket]; *link; link = &(*link)->next)
            if (*link == p) { *link = p->next; break; }
        guest = p->guest;
    }
    RtlLeaveCriticalSection( &wc_cs );
    if (!refs)
    {
        TRACE( "destroying reverse proxy %p (%s guest %p)\n", p,
               rev_iface( iface )->name, guest );
        wc_guest_release( guest, iface );
        RtlFreeHeap( NtCurrentTeb()->Peb->ProcessHeap, 0, p );
    }
    return refs;
}

/* AddRef whatever a native pointer turns out to be: one of our reverse
 * proxies, or an ordinary native object through the surface's own invoker. */
static void rev_native_addref( void *native, UINT iface )
{
    UINT64 args[16] = { 0 };
    struct rev_proxy *p;

    if (!native || rev_is_local( iface )) return;
    if ((p = rev_from_pointer( native )))
    {
        rev_addref( p );
        return;
    }
    wc_surface->invoke( native, 1 /* IUnknown::AddRef */, 1, args );
}

/* ------------------------------------------------------ the two translators */

BOOL winecom_to_native( void *guest_seen, UINT iface, void **native_out )
{
    void *host;

    *native_out = NULL;
    if (!guest_seen) return TRUE;
    if (!wc_ready()) return FALSE;

    /* One of OUR forward proxies: the guest is handing back an object it got
     * from native code in the first place, and the native side must see the
     * pointer it originally gave out.  Borrowed, so no reference moves. */
    if (wc_forward_host( guest_seen, &host ))
    {
        *native_out = host;
        return TRUE;
    }

    /* A guest-implemented object.  It needs a reverse proxy, and the proxy
     * needs a type: the slot table's xaux says what the signature declared. */
    if (iface == ~0u)
    {
        /* The fail-closed answer, and the one every caller that predates
         * reverse proxies keeps: no type in the signature means no slot table,
         * and a native vtable with no slot table can marshal nothing. */
        return FALSE;
    }
    if (!rev_enabled)
    {
        static LONG logged;
        if (!InterlockedExchange( &logged, 1 ))
            FIXME( "%s: a guest-implemented %s (%p) arrived where native code "
                   "will call it, and this surface does not enable reverse "
                   "proxies (WINECOM_SF_REVERSE)\n", wc_surface->name,
                   iface < wc_surface->iface_count
                       ? wc_surface->ifaces[iface].name : "object", guest_seen );
        return FALSE;
    }
    /* A borrow for one call: take the reference the callee may or may not keep,
     * and give the caller winecom_to_native_end() to drop it with.  Creating
     * the proxy consumes ONE guest reference for its whole life, so AddRef the
     * guest object first and let winecom_reverse_wrap consume it -- that keeps
     * every path into the intern table consuming exactly one guest reference,
     * whatever the object turns out to be. */
    wc_guest_addref( guest_seen, iface );
    if (!(*native_out = winecom_reverse_wrap( guest_seen, iface )))
    {
        wc_guest_release( guest_seen, iface );
        return FALSE;
    }
    return TRUE;
}

void winecom_to_native_end( void *native )
{
    struct rev_proxy *p;

    if (!native || !rev_enabled) return;
    if ((p = rev_from_pointer( native ))) rev_release( p );
}

void wc_reverse_release( void *proxy )
{
    struct rev_proxy *p;

    if ((p = rev_from_pointer( proxy ))) rev_release( p );
}

BOOL winecom_translate_in( void *guest_seen, void **host_out )
{
    return winecom_to_native( guest_seen, ~0u, host_out );
}

BOOL winecom_to_guest( void *native_seen, UINT iface, void **guest_out )
{
    struct rev_proxy *p;

    *guest_out = NULL;
    if (!native_seen) return TRUE;
    if (!wc_ready()) return FALSE;

    /* One of OUR reverse proxies: native code is handing the guest back its
     * own object, and it must come out as the guest pointer it gave us.  This
     * is the round trip -- to_guest(to_native(p)) == p -- and it is what lets a
     * sink be unregistered with the pointer it was registered with. */
    if ((p = rev_from_pointer( native_seen )))
    {
        *guest_out = p->guest;
        return TRUE;
    }
    if (iface == ~0u || iface >= wc_surface->iface_count)
    {
        ERR( "%s: a native object (%p) is travelling to the guest with no "
             "roster type; refusing rather than handing over a ppc64 vtable\n",
             wc_surface->name, native_seen );
        return FALSE;
    }
    /* An ordinary native object arriving as an argument of a guest method: a
     * FORWARD proxy, minted inside a reverse call, which is where the two
     * directions meet.  winecom_wrap consumes a host reference and the caller
     * of the reverse call keeps its own, so take one for the guest first. */
    rev_native_addref( native_seen, iface );
    if (!(*guest_out = winecom_wrap( native_seen, iface ))) return FALSE;
    return TRUE;
}

void winecom_to_guest_end( void *guest )
{
    void *host;

    if (!guest) return;
    /* Only a forward proxy this layer minted has anything to drop; a reverse
     * proxy that unwrapped to the guest's own pointer never took a reference
     * to begin with. */
    if (wc_forward_host( guest, &host )) wc_forward_release( guest );
}

/* The OWNING form of winecom_to_native, for a value the guest handed over WITH
 * a reference: a QueryInterface result, an out-parameter of a reverse call.
 * Whatever the value turns out to be, the native caller ends up owning one
 * reference on what it gets and the guest ends up having given one away. */
static BOOL rev_to_native_owned( void *guest, UINT iface, void **out )
{
    void *native;

    *out = NULL;
    if (!guest) return TRUE;
    if (!winecom_to_native( guest, iface, &native )) return FALSE;
    rev_native_addref( native, iface );   /* the reference the caller keeps */
    winecom_to_native_end( native );      /* ...and the borrow above goes */
    wc_guest_release( guest, iface );     /* the one the guest handed us */
    *out = native;
    return TRUE;
}

/* -------------------------------------------------------- IUnknown, reverse */

static UINT64 rev_query_interface( struct rev_proxy *p, const GUID *riid, void **ppv )
{
    struct rev_block b = { 0 };
    void *guest_out = NULL;
    UINT idx;
    HRESULT hr;

    if (!ppv) return (UINT)E_POINTER;
    *ppv = NULL;
    if (!riid) return (UINT)E_INVALIDARG;

    idx = winecom_iface_from_iid( riid );
    if (idx == ~0u)
    {
        /* The guest may well implement this IID, but with no slot table for it
         * there is nothing to build a native vtable out of.  Refuse loudly
         * rather than answer with an object whose methods cannot be called. */
        WARN( "%s: reverse QI for unknown IID %s refused\n",
              rev_iface( p->iface )->name, debugstr_guid(riid) );
        return (UINT)E_NOINTERFACE;
    }

    /* Always ASK THE GUEST, including for IID_IUnknown and for the interface
     * this proxy already is.  Answering from here would be faster and wrong:
     * COM identity is the guest object's to define, and a guest that returns
     * one canonical IUnknown for several interface pointers is relying on
     * exactly that.  Interning then makes the answer stable on this side. */
    b.target = rev_guest_method( p->guest, 0 );
    b.args[0] = (UINT64)(ULONG_PTR)p->guest;
    b.args[1] = (UINT64)(ULONG_PTR)riid;
    b.args[2] = (UINT64)(ULONG_PTR)&guest_out;
    rev_enter_guest( &b );

    hr = (HRESULT)(UINT)b.rax;
    if (FAILED(hr)) return (UINT)hr;
    if (!rev_to_native_owned( guest_out, idx, ppv )) return (UINT)E_NOINTERFACE;
    return (UINT)S_OK;
}

/* ------------------------------------------------------------- the dispatch */

/* Marshal one reverse call and run it.  `gpr` is r3-r10 as the stub received
 * them (gpr[0] is `this`), `fpr` is f1-f13. */
static UINT64 winecom_reverse_dispatch( UINT slot, const UINT64 *gpr, const double *fpr )
{
    const struct winecom_iface *itf;
    const struct winecom_slot *sl;
    struct rev_proxy *proxy;
    struct rev_block b = { 0 };
    void *out_ptrs[REV_MAX_ARGS];
    UINT out_iface[REV_MAX_ARGS];
    UINT in_ifaces[REV_MAX_ARGS];
    /* staging for the ONE interface-array-in row (see the ARR_IN arm):
     * function scope, because its address rides in the block through
     * rev_enter_guest */
    void *arr_stage[64];
    UINT n_out = 0, n_in = 0;
    UINT i, nfpr = 0, iface;

    if (!(proxy = rev_from_pointer( (void *)(ULONG_PTR)gpr[0] )))
    {
        ERR( "reverse slot %u called on %p, which is not one of our reverse "
             "proxies\n", slot, (void *)(ULONG_PTR)gpr[0] );
        return (UINT64)(LONG_PTR)(LONG)E_INVALIDARG;
    }
    iface = proxy->iface;
    itf = rev_iface( iface );
    if (slot >= itf->slot_count)
    {
        ERR( "%s: reverse slot %u is past its %u slots\n", itf->name, slot,
             itf->slot_count );
        return (UINT64)(LONG_PTR)(LONG)E_INVALIDARG;
    }

    /* IUnknown's three slots are served HERE from the proxy, exactly as the
     * forward dispatcher serves them from its proxy -- except QueryInterface,
     * which has to ask the guest (see rev_query_interface).  A [local]
     * interface has no IUnknown at all and its slots 0..2 are real methods. */
    if (slot < 3 && !(itf->flags & WINECOM_IF_LOCAL))
    {
        switch (slot)
        {
        case 0: return rev_query_interface( proxy, (const GUID *)(ULONG_PTR)gpr[1],
                                            (void **)(ULONG_PTR)gpr[2] );
        case 1: return rev_addref( proxy );
        default: return rev_release( proxy );
        }
    }

    sl = itf->slots ? &itf->slots[slot] : NULL;
    if (!sl)
    {
        rev_refuse_once( iface, slot, itf->name,
                         "the interface carries an identity row only, so no "
                         "slot of it has a marshal plan in either direction" );
        return (UINT64)(LONG_PTR)(LONG)E_NOTIMPL;
    }
    /* WINECOM_F_REV is the generator saying "cls/xaux/fpmask describe this
     * signature completely".  Without it, the tables' own judgement about the
     * slot stands in BOTH directions: a `refuse` string is a statement about
     * the SIGNATURE (a PROPVARIANT, a by-value GUID, a struct that reaches an
     * interface pointer) and a hand-written slot keeps its signature in a C
     * function where there is nothing to marshal by.  With it, the row carries
     * a complete plan and the reverse direction serves it -- which is how a
     * by-value float, refused forward because that invoker has integer
     * registers only, is served here. */
    if (!(sl->flags & WINECOM_F_REV))
    {
        if (sl->refuse)
        {
            rev_refuse_once( iface, slot, sl->name, sl->refuse );
            return (sl->flags & WINECOM_F_RET_VOID)
                ? 0 : (UINT64)(LONG_PTR)(LONG)E_NOTIMPL;
        }
        if (sl->flags & WINECOM_F_HAND)
        {
            rev_refuse_once( iface, slot, sl->name,
                             "is served forward by a HAND-WRITTEN slot, whose "
                             "signature lives in a C function rather than in "
                             "the table; there is nothing here to marshal by" );
            return (sl->flags & WINECOM_F_RET_VOID)
                ? 0 : (UINT64)(LONG_PTR)(LONG)E_NOTIMPL;
        }
    }
    if (sl->flags & WINECOM_F_RET_VIA_ARG)
    {
        rev_refuse_once( iface, slot, sl->name,
                         "returns an aggregate through a hidden first "
                         "argument, which shifts every other argument by one "
                         "position on one ABI and not on the other" );
        return (UINT64)(LONG_PTR)(LONG)E_NOTIMPL;
    }
    if (sl->argc > REV_MAX_ARGS)
    {
        rev_refuse_once( iface, slot, sl->name,
                         "takes more arguments than the ELFv2 integer register "
                         "file carries, so some of them arrived in the native "
                         "caller's parameter save area rather than in a "
                         "register these stubs declare" );
        return (UINT64)(LONG_PTR)(LONG)E_NOTIMPL;
    }

    TRACE( "%s (iface %u slot %u argc %u)\n", sl->name, iface, slot, sl->argc );

    b.args[0] = (UINT64)(ULONG_PTR)proxy->guest;
    for (i = 1; i < sl->argc; i++)
    {
        UINT p = i - 1;                       /* parameter index after `this` */
        unsigned char cls = sl->cls ? sl->cls[p] : WINECOM_CA_PASS;
        UINT64 raw;

        if (sl->fpmask & (1u << p))
        {
            /* A floating-point parameter.  ELFv2 put it in the next FPR and
             * RESERVED its integer slot, so the register index of every later
             * argument is unaffected -- which is why this loop still indexes
             * gpr[] by position.  MS-x64 wants a single in the low half of its
             * XMM register and a double in all of it, and the FPR holds a
             * double either way, so the width is applied here. */
            double d;
            float f;
            UINT bits;

            if (nfpr >= REV_MAX_FPR)
            {
                rev_refuse_once( iface, slot, sl->name,
                                 "has more floating-point parameters than "
                                 "f1-f13 carries" );
                return (UINT64)(LONG_PTR)(LONG)E_NOTIMPL;
            }
            if (cls != WINECOM_CA_PASS)
            {
                rev_refuse_once( iface, slot, sl->name,
                                 "has a parameter that is both floating point "
                                 "and interface-typed, which no ABI spells" );
                return (UINT64)(LONG_PTR)(LONG)E_NOTIMPL;
            }
            d = fpr[nfpr++];
            if (sl->fpwide & (1u << p)) memcpy( &raw, &d, sizeof(raw) );
            else
            {
                f = (float)d;
                memcpy( &bits, &f, sizeof(bits) );
                raw = bits;
            }
            b.args[i] = raw;
            continue;
        }

        raw = gpr[i];
        switch (cls)
        {
        case WINECOM_CA_PASS:
        case WINECOM_CA_RIID:
            /* Plain data, and an address means the same thing on both sides:
             * guest memory IS host memory here.  A REFIID travels as the
             * address of a GUID the native caller owns. */
            b.args[i] = raw;
            break;
        case WINECOM_CA_IFACE_IN:
        {
            /* A native object arriving at a guest method.  It becomes a
             * FORWARD proxy -- minted inside a reverse call, which is where
             * the two directions meet -- unless it is one of our reverse
             * proxies, in which case it unwraps to the guest's own object. */
            void *g;

            if (!sl->xaux || !(sl->xmask & (1u << p)))
            {
                rev_refuse_once( iface, slot, sl->name,
                                 "has an interface IN-parameter whose type the "
                                 "generated table does not record (no xmask "
                                 "bit for it); regenerate the marshal tables "
                                 "-- reading xaux without the bit would read a "
                                 "zero the generator never wrote and call it "
                                 "roster index 0" );
                return (UINT64)(LONG_PTR)(LONG)E_NOTIMPL;
            }
            if (!winecom_to_guest( (void *)(ULONG_PTR)raw, sl->xaux[p], &g ))
            {
                rev_refuse_once( iface, slot, sl->name,
                                 "was handed a native object of a type its "
                                 "signature does not roster" );
                return (UINT64)(LONG_PTR)(LONG)E_NOTIMPL;
            }
            b.args[i] = (UINT64)(ULONG_PTR)g;
            if (g) in_ifaces[n_in++] = i;
            break;
        }
        case WINECOM_CA_PPV_OUT:
        case WINECOM_CA_IFACE_OUT_STATIC:
        {
            /* The guest writes a GUEST pointer into storage the native caller
             * owns, and it has to be a native pointer by the time the call
             * returns.  The address itself crosses unchanged. */
            UINT type = ~0u;

            if (cls == WINECOM_CA_IFACE_OUT_STATIC)
            {
                if (!sl->xaux || !(sl->xmask & (1u << p)))
                {
                    rev_refuse_once( iface, slot, sl->name,
                                     "has a statically typed interface "
                                     "out-parameter with no xmask bit to say "
                                     "its xaux entry was written" );
                    return (UINT64)(LONG_PTR)(LONG)E_NOTIMPL;
                }
                type = sl->xaux[p];
            }
            b.args[i] = raw;
            if (raw && n_out < REV_MAX_ARGS)
            {
                out_ptrs[n_out] = (void *)(ULONG_PTR)raw;
                out_iface[n_out] = type;
                n_out++;
            }
            break;
        }
        case WINECOM_CA_IFACE_ARR_IN:
        {
            /* An ARRAY of native objects arriving at a guest method --
             * IWbemObjectSink::Indicate is the row this arm exists for
             * (2026-09-01): wbemprox delivers its results by handing the
             * application's sink lObjectCount IWbemClassObject pointers.
             * Each element FORWARD-mints exactly as the scalar case above
             * (a guest object cannot cross INTO native through this arm,
             * which is what keeps the sink licence bounded); the wrapped
             * pointers live in a stack-side copy so the native caller's
             * array is never mutated.  The count is BY VALUE at the
             * parameter aux2 names -- same field the forward direction
             * reads for this class (winecom.h).  An element that cannot
             * be wrapped scrubs to NULL, loudly: the guest sees a NULL it
             * can check, never a native vtable. */
            const void *const *src = (const void *const *)(ULONG_PTR)raw;
            UINT64 count;
            UINT k;
            static LONG arr_logged;

            if (!sl->xaux || !(sl->xmask & (1u << p)))
            {
                rev_refuse_once( iface, slot, sl->name,
                                 "has an interface ARRAY whose element type "
                                 "the generated table does not record (no "
                                 "xmask bit); regenerate the marshal tables" );
                return (UINT64)(LONG_PTR)(LONG)E_NOTIMPL;
            }
            count = gpr[1 + sl->aux2];   /* the count parameter, by value */
            if (!src || !count)
            {
                b.args[i] = raw;
                break;
            }
            if (count > ARRAYSIZE(arr_stage))
            {
                rev_refuse_once( iface, slot, sl->name,
                                 "delivered more array elements than the "
                                 "reverse stub's staging carries (64); raise "
                                 "the bound if a real sink ever needs it" );
                return (UINT64)(LONG_PTR)(LONG)E_NOTIMPL;
            }
            for (k = 0; k < count; k++)
            {
                void *g = NULL;

                if (src[k] && !winecom_to_guest( (void *)src[k],
                                                 sl->xaux[p], &g ))
                {
                    if (!InterlockedExchange( &arr_logged, 1 ))
                        ERR( "reverse %s: array element %u cannot be wrapped "
                             "for the guest; scrubbing to NULL\n",
                             sl->name, k );
                    g = NULL;
                }
                arr_stage[k] = g;
            }
            b.args[i] = (UINT64)(ULONG_PTR)arr_stage;
            break;
        }
        default:
            rev_refuse_once( iface, slot, sl->name,
                             "has an argument class with no REVERSE marshal "
                             "path (a completion event or an aggregate "
                             "return)" );
            return (UINT64)(LONG_PTR)(LONG)E_NOTIMPL;
        }
    }

    b.target = rev_guest_method( proxy->guest, slot );
    rev_enter_guest( &b );

    for (i = 0; i < n_out; i++)
    {
        void **outp = out_ptrs[i];
        UINT type = out_iface[i];
        void *native;

        if (!*outp) continue;
        if (type == ~0u)
        {
            /* A riid-typed out-parameter: its type is the RIID the native
             * caller passed, resolved now. */
            const GUID *riid = (const GUID *)(ULONG_PTR)b.args[sl->aux + 1];
            type = riid ? winecom_iface_from_iid( riid ) : ~0u;
        }
        if (type == ~0u || !rev_to_native_owned( *outp, type, &native ))
        {
            ERR( "%s: the guest wrote an interface pointer this surface does "
                 "not roster into an out-parameter; clearing it rather than "
                 "handing native code an x86-64 vtable\n", sl->name );
            *outp = NULL;
            continue;
        }
        *outp = native;
    }

    /* Give back the forward proxies minted for this call's IN parameters: the
     * guest borrowed them for the duration of the call, exactly as native code
     * borrows a reverse proxy on the way in.  A guest that kept one AddRef'd it
     * through its own trap, so this drops only the reference this layer took. */
    for (i = 0; i < n_in; i++)
        winecom_to_guest_end( (void *)(ULONG_PTR)b.args[in_ifaces[i]] );

    if (sl->flags & WINECOM_F_RET_VOID) return 0;
    return (UINT64)(LONG_PTR)(LONG)b.rax;
}

/* --------------------------------------------------------- the native stubs */

/* The over-declared prototype: eight integer parameters and thirteen doubles
 * is r3-r10 and f1-f13 under ELFv2, with nothing landing in the parameter save
 * area.  A caller with a real signature fills a subset; the class table says
 * which. */
#define REV_STUB_PARAMS                                                        \
    void *self, UINT64 i1, UINT64 i2, UINT64 i3, UINT64 i4, UINT64 i5,         \
    UINT64 i6, UINT64 i7, double d0, double d1, double d2, double d3,          \
    double d4, double d5, double d6, double d7, double d8, double d9,          \
    double d10, double d11, double d12

typedef UINT64 (*rev_stub_fn)( REV_STUB_PARAMS );

#define REV_STUB( n )                                                          \
static UINT64 rev_stub_##n( REV_STUB_PARAMS )                                  \
{                                                                              \
    const UINT64 gpr[8] =                                                      \
        { (UINT64)(ULONG_PTR)self, i1, i2, i3, i4, i5, i6, i7 };               \
    const double fpr[13] =                                                     \
        { d0, d1, d2, d3, d4, d5, d6, d7, d8, d9, d10, d11, d12 };             \
                                                                               \
    return winecom_reverse_dispatch( n, gpr, fpr );                            \
}

REV_STUB(0) REV_STUB(1) REV_STUB(2) REV_STUB(3)
REV_STUB(4) REV_STUB(5) REV_STUB(6) REV_STUB(7)
REV_STUB(8) REV_STUB(9) REV_STUB(10) REV_STUB(11)
REV_STUB(12) REV_STUB(13) REV_STUB(14) REV_STUB(15)
REV_STUB(16) REV_STUB(17) REV_STUB(18) REV_STUB(19)
REV_STUB(20) REV_STUB(21) REV_STUB(22) REV_STUB(23)
REV_STUB(24) REV_STUB(25) REV_STUB(26) REV_STUB(27)
REV_STUB(28) REV_STUB(29) REV_STUB(30) REV_STUB(31)
REV_STUB(32) REV_STUB(33) REV_STUB(34) REV_STUB(35)
REV_STUB(36) REV_STUB(37) REV_STUB(38) REV_STUB(39)
REV_STUB(40) REV_STUB(41) REV_STUB(42) REV_STUB(43)
REV_STUB(44) REV_STUB(45) REV_STUB(46) REV_STUB(47)
REV_STUB(48) REV_STUB(49) REV_STUB(50) REV_STUB(51)
REV_STUB(52) REV_STUB(53) REV_STUB(54) REV_STUB(55)
REV_STUB(56) REV_STUB(57) REV_STUB(58) REV_STUB(59)
REV_STUB(60) REV_STUB(61) REV_STUB(62) REV_STUB(63)

static const rev_stub_fn rev_stub_table[REV_MAX_SLOTS] =
{
    rev_stub_0 , rev_stub_1 , rev_stub_2 , rev_stub_3 ,
    rev_stub_4 , rev_stub_5 , rev_stub_6 , rev_stub_7 ,
    rev_stub_8 , rev_stub_9 , rev_stub_10, rev_stub_11,
    rev_stub_12, rev_stub_13, rev_stub_14, rev_stub_15,
    rev_stub_16, rev_stub_17, rev_stub_18, rev_stub_19,
    rev_stub_20, rev_stub_21, rev_stub_22, rev_stub_23,
    rev_stub_24, rev_stub_25, rev_stub_26, rev_stub_27,
    rev_stub_28, rev_stub_29, rev_stub_30, rev_stub_31,
    rev_stub_32, rev_stub_33, rev_stub_34, rev_stub_35,
    rev_stub_36, rev_stub_37, rev_stub_38, rev_stub_39,
    rev_stub_40, rev_stub_41, rev_stub_42, rev_stub_43,
    rev_stub_44, rev_stub_45, rev_stub_46, rev_stub_47,
    rev_stub_48, rev_stub_49, rev_stub_50, rev_stub_51,
    rev_stub_52, rev_stub_53, rev_stub_54, rev_stub_55,
    rev_stub_56, rev_stub_57, rev_stub_58, rev_stub_59,
    rev_stub_60, rev_stub_61, rev_stub_62, rev_stub_63,
};

/* ------------------------------------------------------------------ attach */

/* The guest-callback trampoline factory, exported by ntdll for exactly this: a
 * COM method traps inside a vtable stub array and is routed by RIP arithmetic,
 * so ntdll's export-name override table can never reach it, and a module that
 * needs a guest pointer wrapped resolves the factory by name.  A tree without
 * it refuses loudly here rather than failing to load. */
static void *rev_wrap_callback( void *fn )
{
    void *(CDECL *wrap)( void *, BOOL );
    UNICODE_STRING ntdllW;
    ANSI_STRING name;
    HMODULE ntdll;
    void *proc;

    RtlInitUnicodeString( &ntdllW, L"ntdll.dll" );
    if (LdrGetDllHandle( NULL, 0, &ntdllW, &ntdll ))
    {
        ERR( "no ntdll.dll in the loader list\n" );
        return NULL;
    }
    RtlInitAnsiString( &name, "__wine_guest_wrap_callback" );
    if (LdrGetProcedureAddress( ntdll, &name, 0, &proc ))
    {
        ERR( "this ntdll exports no __wine_guest_wrap_callback; reverse "
             "proxies need the guest-callback trampoline pool "
             "(dlls/ntdll/signal_ppc64.c)\n" );
        return NULL;
    }
    wrap = proc;
    /* The WIDE form.  The value that matters comes out of the block, not out
     * of the trampoline, but a narrow trampoline would sign-extend the shim's
     * own return -- and the shim returns whatever RAX happened to hold.  Wide
     * is the form that carries it unchanged, which is the honest one. */
    return wrap( fn, TRUE );
}

BOOL wc_reverse_init(void)
{
    SIZE_T size = sizeof(rev_shim_code);
    NTSTATUS status;
    void *shim = NULL;
    UINT i, off;

    if (!(wc_surface->flags & WINECOM_SF_REVERSE))
    {
        TRACE( "%s: no reverse proxies on this surface\n", wc_surface->name );
        return TRUE;
    }

    status = NtAllocateVirtualMemory( NtCurrentProcess(), &shim, 0, &size,
                                      MEM_COMMIT | MEM_RESERVE,
                                      PAGE_EXECUTE_READWRITE );
    if (status)
    {
        ERR( "%s: no memory for the reverse-call guest shim, status %08x\n",
             wc_surface->name, (UINT)status );
        return FALSE;
    }
    memcpy( shim, rev_shim_code, sizeof(rev_shim_code) );
    NtFlushInstructionCache( NtCurrentProcess(), shim, sizeof(rev_shim_code) );

    if (!(rev_enter = (void *)rev_wrap_callback( shim ))) return FALSE;
    if ((void *)rev_enter == shim)
    {
        /* WINEEMUNOCBWRAP=1, or a pool that could not allocate: the raw x86-64
         * shim address came back, and calling that as ppc64 is a c000001d one
         * frame later.  Say so and fail the attach instead. */
        ERR( "%s: the guest-callback pool returned the raw shim address; "
             "reverse proxies cannot work (WINEEMUNOCBWRAP=1?)\n",
             wc_surface->name );
        rev_enter = NULL;
        return FALSE;
    }

    rev_total_slots = 0;
    for (i = 0; i < wc_surface->iface_count; i++)
        rev_total_slots += wc_surface->ifaces[i].slot_count;
    if (!(rev_slot_base = RtlAllocateHeap( NtCurrentTeb()->Peb->ProcessHeap,
                                           HEAP_ZERO_MEMORY,
                                           wc_surface->iface_count * sizeof(*rev_slot_base)
                                           + rev_total_slots )))
    {
        rev_enter = NULL;
        return FALSE;
    }
    rev_refused = (unsigned char *)(rev_slot_base + wc_surface->iface_count);
    for (i = 0, off = 0; i < wc_surface->iface_count; i++)
    {
        rev_slot_base[i] = off;
        off += wc_surface->ifaces[i].slot_count;
    }

    for (i = 0; i < REV_MAX_SLOTS; i++)
        rev_vtbl[i] = (const void *)rev_stub_table[i];

    rev_enabled = TRUE;
    TRACE( "%s: reverse proxies armed; shim %p, trampoline %p, %u native "
           "vtable slots over %u interfaces\n", wc_surface->name, shim,
           (void *)rev_enter, REV_MAX_SLOTS, wc_surface->iface_count );
    if (wc_nowrap())
        ERR( "%s: WINEEMUNOCOMWRAP=1 -- guest objects will cross to native "
             "code RAW; expect native calls into x86-64 vtables\n",
             wc_surface->name );
    return TRUE;
}
