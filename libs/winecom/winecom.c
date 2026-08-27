/*
 * winecom -- the shared guest-COM proxy runtime (see include/wine/winecom.h
 * for the contract, hangover-ppc64le/docs/system-com-design.md for the
 * design, and dlls/d3d12/main.c for the module this was extracted from).
 *
 * DISPATCH CONTRACT with dlls/ntdll/signal_ppc64.c: the dispatcher calls
 * the client's __wine_com_dispatch on the Win32 stack it is already
 * standing on, with the trap CONTEXT still describing the guest call (Rip
 * on the trap instruction, Rsp on the return address, arg0 rescued into
 * R10).  On STATUS_SUCCESS the call is fully handled -- including writing
 * ctx->Rax -- and the DISPATCHER pops the return address.  Any other
 * status means the trap was not served and the guest gets an illegal-
 * instruction exception; that path is for broken images, not for refused
 * slots, which are served loudly with E_NOTIMPL here.
 *
 * PROXIES.  One `struct com_proxy` per (host pointer, interface), interned;
 * winecom_wrap() consumes one host reference.  The guest-facing vtable of a
 * proxy is the address sequence of a guest thunk module's stub array for
 * that interface type, materialised once at attach from the module's own
 * __wine_com_thunk_info -- and the IIDs published there are cross-checked
 * against the client's generated table, so the two generators reading the
 * same interfaces JSON cannot silently disagree.  All slots trap, including
 * AddRef/Release/QueryInterface (simplicity first).
 *
 * TRANSLATE-IN (design §6.3).  An interface-typed IN value is never blindly
 * unwrapped: one of our proxies unwraps to its host pointer, NULL stays NULL,
 * and anything else is a guest-IMPLEMENTED object.  That last case is what
 * reverse.c serves -- it gets a REVERSE PROXY, a native vtable whose slots
 * enter the guest method through the emulator -- on a surface that asked for
 * one (WINECOM_SF_REVERSE) and with the interface type the generated table
 * recorded in xaux.  A surface that did not, or a table row with no type,
 * still refuses loudly, which is the fail-closed default this replaced the
 * original d3d12 runtime's unconditional unwrap with.
 *
 * THE OTHER HALF IS IN reverse.c, and the two are one runtime rather than two:
 * they share the surface, the intern lock and each other's identity tests, so
 * that an object crossing the boundary twice comes back as itself.
 *
 * WINEEMUNOCOMWRAP=1 is the negative control, same shape as
 * WINEEMUNOGSTHREADS / WINEEMUNOCBWRAP: winecom_wrap hands the RAW host
 * pointer to the guest, which is exactly the defect this runtime exists to
 * fix, so anything the mechanism carries MUST go red under it.
 *
 * Copyright 2026 the ppc64le port authors
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 */

#include <stdarg.h>

#include "ntstatus.h"
#define WIN32_NO_STATUS

#include "windef.h"
#include "winbase.h"
#include "winternl.h"
#include "wine/debug.h"
#include "wine/winecom.h"

#include "winecom_private.h"

WINE_DEFAULT_DEBUG_CHANNEL(winecom);

/* ---------------------------------------------------------------- proxies */

struct com_proxy
{
    const void *guest_vtbl;   /* first member: this IS the COM object the
                                 guest sees -- *(void**)proxy is its vtable */
    void       *host;         /* native/host interface pointer */
    LONG        refs;         /* guest-visible refcount, served here */
    UINT        iface;        /* index into wc_surface->ifaces[] */
    struct com_proxy *next;   /* intern-table chain */
};

#define INTERN_BUCKETS 256

/* THE intern lock, shared with reverse.c through winecom_private.h: a proxy of
 * either direction can be created while the other kind is being looked up -- a
 * reverse call whose argument is a forward proxy does exactly that -- and two
 * locks would be two lock orders. */
CRITICAL_SECTION wc_cs;
static CRITICAL_SECTION_DEBUG com_cs_debug =
{
    0, 0, &wc_cs,
    { &com_cs_debug.ProcessLocksList, &com_cs_debug.ProcessLocksList },
      0, 0, { (DWORD_PTR)(__FILE__ ": winecom wc_cs") }
};
CRITICAL_SECTION wc_cs = { &com_cs_debug, -1, 0, 0, 0, 0 };

static struct com_proxy *intern[INTERN_BUCKETS];

const struct winecom_surface *wc_surface;

/* Guest-facing vtables: guest_vtbls[i][n] is the address of slot n's trap
 * stub in the guest module's stub array for interface i. */
static const UINT64 **guest_vtbls;
static UINT64 *guest_vtbl_block;
static UINT64 guest_vtbl_lo, guest_vtbl_hi;  /* published stub address range,
                                                for the O(1) proxy test */
static unsigned char *refuse_logged;   /* one byte per (iface, slot) */
static UINT *iface_slot_base;
static UINT total_slots;

static LONG com_init_state;            /* 0 = no, 1 = in progress, 2 = ok,
                                          3 = failed */
static int nowrap = -1;                /* WINEEMUNOCOMWRAP */

/* What the guest thunk module publishes; must match spec2thunk COM mode. */
struct com_thunk_info
{
    UINT version;
    UINT iface_count;
    UINT stride;
    UINT trap_off;
    UINT ifaces_rva;
};
#define COM_THUNK_INFO_VERSION 1

struct com_iface_entry
{
    GUID iid;
    UINT slot_count;
    UINT stubs_rva;
};

/* a "1" in the process environment; PE-side, so no getenv here */
static BOOL com_env_flag( const WCHAR *name )
{
    UNICODE_STRING nameW, value;
    WCHAR buf[4];

    value.Buffer = buf;
    value.MaximumLength = sizeof(buf);
    value.Length = 0;
    RtlInitUnicodeString( &nameW, name );
    return !RtlQueryEnvironmentVariable_U( NULL, &nameW, &value ) &&
           value.Length && buf[0] == '1';
}

static HMODULE find_guest_module( const WCHAR *const *names, UINT count )
{
    LIST_ENTRY *mark, *entry;
    HMODULE ret = NULL;
    ULONG_PTR magic;
    UINT i;

    LdrLockLoaderLock( 0, NULL, &magic );
    mark = &NtCurrentTeb()->Peb->LdrData->InMemoryOrderModuleList;
    for (entry = mark->Flink; entry != mark && !ret; entry = entry->Flink)
    {
        LDR_DATA_TABLE_ENTRY *mod = CONTAINING_RECORD( entry, LDR_DATA_TABLE_ENTRY,
                                                       InMemoryOrderLinks );
        const IMAGE_NT_HEADERS *nt = RtlImageNtHeader( mod->DllBase );
        UNICODE_STRING want;

        if (!nt || nt->FileHeader.Machine != IMAGE_FILE_MACHINE_AMD64) continue;
        for (i = 0; i < count; i++)
        {
            RtlInitUnicodeString( &want, names[i] );
            if (RtlEqualUnicodeString( &want, &mod->BaseDllName, TRUE ))
            {
                ret = mod->DllBase;
                break;
            }
        }
    }
    LdrUnlockLoaderLock( 0, magic );
    return ret;
}

/* Validate one guest module's published COM table against the client's
 * generated table.  Cheap, and run for EVERY loaded candidate module: a
 * copy of the roster that drifted is a load failure here, never a
 * mis-dispatched neighbour slot. */
static BOOL com_check_module( HMODULE guest, const struct com_thunk_info **info_out )
{
    const struct com_thunk_info *info;
    const struct com_iface_entry *entries;
    ANSI_STRING name;
    ULONG_PTR base = (ULONG_PTR)guest;
    void *ptr;
    UINT i;

    RtlInitAnsiString( &name, "__wine_com_thunk_info" );
    if (LdrGetProcedureAddress( guest, &name, 0, &ptr ))
    {
        ERR( "%s: guest module %p exports no __wine_com_thunk_info\n",
             wc_surface->name, guest );
        return FALSE;
    }
    info = ptr;
    if (info->version != COM_THUNK_INFO_VERSION || info->stride != 16)
    {
        ERR( "%s: guest com thunk info version %u stride %u not supported\n",
             wc_surface->name, info->version, info->stride );
        return FALSE;
    }
    if (info->iface_count != wc_surface->iface_count)
    {
        ERR( "%s: guest module has %u interfaces, marshal table has %u -- "
             "the two generators read different interface JSONs\n",
             wc_surface->name, info->iface_count, wc_surface->iface_count );
        return FALSE;
    }
    entries = (const struct com_iface_entry *)(base + info->ifaces_rva);
    for (i = 0; i < wc_surface->iface_count; i++)
    {
        if (!IsEqualGUID( &entries[i].iid, &wc_surface->ifaces[i].iid ))
        {
            ERR( "%s: interface %u (%s) IID mismatch between guest module "
                 "and marshal table\n", wc_surface->name, i,
                 wc_surface->ifaces[i].name );
            return FALSE;
        }
        if (entries[i].slot_count != wc_surface->ifaces[i].slot_count)
        {
            ERR( "%s: interface %s: guest %u slots, table %u\n",
                 wc_surface->name, wc_surface->ifaces[i].name,
                 entries[i].slot_count, wc_surface->ifaces[i].slot_count );
            return FALSE;
        }
    }
    if (info_out) *info_out = info;
    return TRUE;
}

/* Build the guest vtables from a guest module's published stub arrays. */
static BOOL com_runtime_init_once( void )
{
    const struct com_thunk_info *info;
    const struct com_iface_entry *entries;
    ULONG_PTR base;
    HMODULE guest;
    UINT i, n, off;

    nowrap = com_env_flag( L"WINEEMUNOCOMWRAP" );

    if (!(guest = find_guest_module( wc_surface->guest_modules,
                                     wc_surface->module_count )))
    {
        ERR( "%s: no guest thunk module in this process; COM dispatch "
             "cannot work\n", wc_surface->name );
        return FALSE;
    }
    /* validate every loaded candidate, materialise from the first */
    for (i = 0; i < wc_surface->module_count; i++)
    {
        HMODULE mod = find_guest_module( &wc_surface->guest_modules[i], 1 );
        if (mod && !com_check_module( mod, NULL )) return FALSE;
    }
    if (!com_check_module( guest, &info )) return FALSE;

    base = (ULONG_PTR)guest;
    entries = (const struct com_iface_entry *)(base + info->ifaces_rva);

    total_slots = 0;
    for (i = 0; i < wc_surface->iface_count; i++) total_slots += entries[i].slot_count;

    if (!(guest_vtbls = RtlAllocateHeap( NtCurrentTeb()->Peb->ProcessHeap, 0,
                                         wc_surface->iface_count * sizeof(*guest_vtbls)
                                         + wc_surface->iface_count * sizeof(*iface_slot_base)
                                         + total_slots * sizeof(UINT64) + total_slots )))
        return FALSE;
    iface_slot_base = (UINT *)(guest_vtbls + wc_surface->iface_count);
    guest_vtbl_block = (UINT64 *)(iface_slot_base + wc_surface->iface_count);
    refuse_logged = (unsigned char *)(guest_vtbl_block + total_slots);
    memset( refuse_logged, 0, total_slots );

    off = 0;
    for (i = 0; i < wc_surface->iface_count; i++)
    {
        iface_slot_base[i] = off;
        guest_vtbls[i] = guest_vtbl_block + off;
        for (n = 0; n < entries[i].slot_count; n++)
            guest_vtbl_block[off + n] = base + entries[i].stubs_rva + n * (UINT64)info->stride;
        off += entries[i].slot_count;
    }
    /* The stub arrays are contiguous per interface but not necessarily as a
     * whole; the published range is only used as a fast pre-filter for the
     * proxy test, which then confirms via the intern table. */
    guest_vtbl_lo = guest_vtbl_block[0];
    guest_vtbl_hi = guest_vtbl_block[0];
    for (n = 0; n < total_slots; n++)
    {
        if (guest_vtbl_block[n] < guest_vtbl_lo) guest_vtbl_lo = guest_vtbl_block[n];
        if (guest_vtbl_block[n] > guest_vtbl_hi) guest_vtbl_hi = guest_vtbl_block[n];
    }
    TRACE( "%s: materialised %u guest vtable slots across %u interfaces from %p\n",
           wc_surface->name, total_slots, i, guest );
    if (nowrap)
        ERR( "%s: WINEEMUNOCOMWRAP=1 -- interface pointers will cross RAW; "
             "expect guest calls into native vtables\n", wc_surface->name );
    return TRUE;
}

BOOL winecom_attach( const struct winecom_surface *s )
{
    LONG state;

    while ((state = InterlockedCompareExchange( &com_init_state, 1, 0 )))
    {
        if (state == 2) return TRUE;
        if (state == 3) return FALSE;
        NtYieldExecution();
    }
    wc_surface = s;
    if (!com_runtime_init_once() || !wc_reverse_init())
    {
        InterlockedExchange( &com_init_state, 3 );
        return FALSE;
    }
    InterlockedExchange( &com_init_state, 2 );
    return TRUE;
}

static BOOL com_ready( void )
{
    return com_init_state == 2;
}

BOOL wc_ready( void )
{
    return com_ready();
}

BOOL wc_nowrap( void )
{
    return nowrap > 0;
}

/* ------------------------------------------------------------ host calls */

void winecom_host_release( void *host )
{
    UINT64 args[16] = { 0 };
    wc_surface->invoke( host, 2 /* IUnknown::Release */, 1, args );
}

/* A [local] INTERFACE HAS NO REFERENCE MANAGEMENT AT ALL, and slot 2 of one is
 * a real method with a real effect.  On the audio surfaces it is
 * IXAudio2Voice::SetEffectChain: a voice is destroyed by DestroyVoice, has no
 * Release to call, and calling "Release" on one tears its effect chain off
 * instead.
 *
 * Both audio clients had grown a guard of their own against exactly this --
 * dlls/xaudio2_9/guestcom.c keeps a registry of live voice hosts and refuses
 * slot 2 on one, and dlls/combase's system-COM invoker does the same -- and
 * MEASURED on DOOM (2016), the combase guard fired three times on one voice
 * during audio setup.  It fired because THIS layer asked, and it asked because
 * the surplus-reference drop below did not know what kind of interface it was
 * holding.
 *
 * So the knowledge moves here, where the interface index is already in hand.
 * The clients' guards stay as backstops and should now never fire; if one
 * does, it is saying something this function got wrong. */
static void host_release_iface( void *host, UINT iface )
{
    if (!host) return;
    if (iface < wc_surface->iface_count &&
        (wc_surface->ifaces[iface].flags & WINECOM_IF_LOCAL))
    {
        TRACE( "not dropping a reference on %s %p: a [local] interface has no "
               "reference count and its slot 2 is an ordinary method\n",
               wc_surface->ifaces[iface].name, host );
        return;
    }
    winecom_host_release( host );
}

static HRESULT host_qi( void *host, const GUID *riid, void **out )
{
    UINT64 args[16] = { 0 };

    *out = NULL;
    args[1] = (UINT64)(ULONG_PTR)riid;
    args[2] = (UINT64)(ULONG_PTR)out;
    return (HRESULT)wc_surface->invoke( host, 0 /* QueryInterface */, 3, args );
}

BOOL winecom_slot_names( UINT iface, UINT slot, const char **iface_name,
                         const char **slot_name )
{
    const struct winecom_iface *itf;

    if (!wc_surface || iface >= wc_surface->iface_count) return FALSE;
    itf = &wc_surface->ifaces[iface];
    if (slot >= itf->slot_count) return FALSE;
    *iface_name = itf->name;
    /* An identity row carries no slot table: IUnknown's three slots are served
     * by name and the rest are refused, so name them the same way. */
    if (!itf->slots)
        *slot_name = slot == 0 ? "QueryInterface" : slot == 1 ? "AddRef"
                   : slot == 2 ? "Release" : "<identity>";
    else if (!itf->slots[slot].name) return FALSE;
    else *slot_name = itf->slots[slot].name;
    return TRUE;
}

/* ------------------------------------------------------- proxy operations */

UINT winecom_iface_from_iid( const GUID *riid )
{
    UINT i;
    for (i = 0; i < wc_surface->iface_count; i++)
        if (IsEqualGUID( riid, &wc_surface->ifaces[i].iid )) return i;
    return ~0u;
}

void *winecom_wrap( void *host, UINT iface )
{
    UINT bucket = (UINT)(((ULONG_PTR)host >> 4) % INTERN_BUCKETS);
    struct com_proxy *p;
    void *guest;

    if (!host) return NULL;

    /* THE ROUND TRIP.  `host` may be one of the REVERSE proxies -- a native
     * vtable this runtime built around an object the guest implemented -- on
     * its way back to the guest that wrote it.  Wrapping one would give the
     * guest a forward proxy whose host is a reverse proxy whose guest is the
     * object the guest already has, and the guest's identity comparison
     * against its own pointer would fail.  So it comes back as itself: one
     * guest reference for the caller, and the native reference winecom_wrap
     * was handed is consumed by releasing the reverse proxy. */
    if ((guest = wc_reverse_guest( host )))
    {
        TRACE( "host %p is our reverse proxy for guest %p; returning the "
               "guest's own pointer\n", host, guest );
        wc_guest_addref( guest, iface );
        wc_reverse_release( host );
        return guest;
    }

    if (nowrap)
    {
        ERR( "WINEEMUNOCOMWRAP: handing raw host %s %p to the guest\n",
             wc_surface->ifaces[iface].name, host );
        return host;
    }
    RtlEnterCriticalSection( &wc_cs );
    for (p = intern[bucket]; p; p = p->next)
        if (p->host == host && p->iface == iface) break;
    if (p)
    {
        p->refs++;
        RtlLeaveCriticalSection( &wc_cs );
        /* The surplus reference: this pair is already interned and already
         * holds one, so the one the caller handed us goes back.  Unless the
         * interface has no reference count -- see host_release_iface. */
        host_release_iface( host, iface );
        return p;
    }
    if (!(p = RtlAllocateHeap( NtCurrentTeb()->Peb->ProcessHeap, 0, sizeof(*p) )))
    {
        RtlLeaveCriticalSection( &wc_cs );
        ERR( "out of memory interning %s %p\n", wc_surface->ifaces[iface].name, host );
        host_release_iface( host, iface );
        return NULL;
    }
    p->guest_vtbl = guest_vtbls[iface];
    p->host = host;
    p->refs = 1;
    p->iface = iface;
    p->next = intern[bucket];
    intern[bucket] = p;
    RtlLeaveCriticalSection( &wc_cs );
    TRACE( "wrapped %s host %p as proxy %p\n", wc_surface->ifaces[iface].name,
           host, p );
    return p;
}

/* Is this pointer one of our proxies?  O(1) and dereferences nothing
 * unvetted beyond the first word: a proxy's vtable pointer is one of the
 * materialised guest_vtbls entries. */
static struct com_proxy *proxy_from_pointer( void *ptr )
{
    struct com_proxy *cand = ptr;
    ULONG_PTR vtbl;
    UINT i;

    if (!ptr) return NULL;
    vtbl = (ULONG_PTR)cand->guest_vtbl;
    if ((ULONG_PTR)(guest_vtbl_block) <= vtbl &&
        vtbl < (ULONG_PTR)(guest_vtbl_block + total_slots))
    {
        /* points into the materialised block: confirm it is an interface
         * base, then confirm the pointer is interned */
        for (i = 0; i < wc_surface->iface_count; i++)
            if ((const void *)guest_vtbls[i] == cand->guest_vtbl)
            {
                UINT bucket = (UINT)(((ULONG_PTR)cand->host >> 4) % INTERN_BUCKETS);
                struct com_proxy *p;
                RtlEnterCriticalSection( &wc_cs );
                for (p = intern[bucket]; p; p = p->next)
                    if (p == cand) break;
                RtlLeaveCriticalSection( &wc_cs );
                return p;
            }
    }
    return NULL;
}

void *winecom_unwrap( void *maybe_proxy )
{
    struct com_proxy *p;

    if (!maybe_proxy) return NULL;
    if (!(p = proxy_from_pointer( maybe_proxy )))
    {
        ERR( "%p is not one of ours; refusing to unwrap it\n", maybe_proxy );
        return NULL;
    }
    return p->host;
}

/* The forward half of the classifier, for reverse.c: is this guest-visible
 * pointer one of OUR proxies, and if so what is behind it.  winecom_to_native
 * (reverse.c) is the whole classifier and this is the half that knows about
 * forward proxies. */
BOOL wc_forward_host( void *ptr, void **host_out )
{
    struct com_proxy *p;

    *host_out = NULL;
    if (!ptr) return FALSE;
    if (!(p = proxy_from_pointer( ptr ))) return FALSE;
    *host_out = p->host;
    return TRUE;
}

static ULONG proxy_release( struct com_proxy *p );

/* Drop one guest-visible reference from a forward proxy the reverse
 * dispatcher minted so a guest method could be handed a native object. */
void wc_forward_release( void *ptr )
{
    struct com_proxy *p = proxy_from_pointer( ptr );

    if (p) proxy_release( p );
}

static ULONG proxy_addref( struct com_proxy *p )
{
    ULONG refs;
    RtlEnterCriticalSection( &wc_cs );
    refs = ++p->refs;
    RtlLeaveCriticalSection( &wc_cs );
    return refs;
}

static ULONG proxy_release( struct com_proxy *p )
{
    UINT iface = p->iface;   /* read before the free below */
    UINT bucket = (UINT)(((ULONG_PTR)p->host >> 4) % INTERN_BUCKETS);
    struct com_proxy **link;
    void *host = NULL;
    ULONG refs;

    RtlEnterCriticalSection( &wc_cs );
    refs = --p->refs;
    if (!refs)
    {
        for (link = &intern[bucket]; *link; link = &(*link)->next)
            if (*link == p) { *link = p->next; break; }
        host = p->host;
    }
    RtlLeaveCriticalSection( &wc_cs );
    if (!refs)
    {
        TRACE( "destroying proxy %p (%s host %p)\n", p,
               wc_surface->ifaces[p->iface].name, host );
        host_release_iface( host, iface );
        RtlFreeHeap( NtCurrentTeb()->Peb->ProcessHeap, 0, p );
    }
    return refs;
}

static HRESULT proxy_qi( struct com_proxy *p, const GUID *riid, void **ppv )
{
    UINT idx;
    void *host_out;
    HRESULT hr;

    if (!ppv) return E_POINTER;
    *ppv = NULL;
    if (!riid) return E_INVALIDARG;
    if (wc_surface->ifaces[p->iface].flags & WINECOM_IF_LOCAL)
    {
        /* Slot 0 of a [local] interface is a real method -- GetVoiceDetails,
         * not QueryInterface -- so there is nothing to ask.  A client with
         * [local] interfaces claims them before this dispatcher sees them, so
         * reaching here means the roster and the claim disagree. */
        ERR( "%s is [local] and has no QueryInterface; the client did not "
             "claim it before winecom_dispatch saw it\n",
             wc_surface->ifaces[p->iface].name );
        return E_NOINTERFACE;
    }
    idx = winecom_iface_from_iid( riid );
    if (idx == ~0u)
    {
        /* The host may well implement this IID, but with no stub vtable for
         * it a host pointer handed to the guest would be called as x86-64
         * code.  Refuse loudly instead. */
        WARN( "%s: QI for unknown IID %s refused\n",
              wc_surface->ifaces[p->iface].name, debugstr_guid(riid) );
        return E_NOINTERFACE;
    }
    hr = host_qi( p->host, riid, &host_out );
    if (FAILED(hr)) return hr;
    if (!(*ppv = winecom_wrap( host_out, idx ))) return E_OUTOFMEMORY;
    return S_OK;
}

/* ------------------------------------------------------- argument reading */

UINT64 winecom_read_arg( const AMD64_CONTEXT *ctx, UINT n )
{
    switch (n)
    {
    case 0: return ctx->R10;   /* `this`; SYSCALL clobbered RCX, the stub
                                  rescued it -- same rule as the flat path */
    case 1: return ctx->Rdx;
    case 2: return ctx->R8;
    case 3: return ctx->R9;
    default: return *(UINT64 *)(ULONG_PTR)(ctx->Rsp + 8 + n * (UINT64)8);
    }
}

/* -------------------------------------------------------------- dispatch */

static void refuse_once( UINT iface, UINT slot, const char *name, const char *why )
{
    unsigned char *flag = &refuse_logged[iface_slot_base[iface] + slot];
    if (*flag) return;
    *flag = 1;
    FIXME( "%s: refusing %s (iface %u slot %u): %s\n", wc_surface->name,
           name ? name : wc_surface->ifaces[iface].name, iface, slot,
           why ? why : "no marshal plan" );
}

HRESULT winecom_wrap_out_iface( HRESULT hr, const GUID *riid, void **ppv )
{
    UINT idx;

    if (FAILED(hr) || !ppv || !*ppv) return hr;
    idx = riid ? winecom_iface_from_iid( riid ) : ~0u;
    if (idx == ~0u)
    {
        ERR( "%s: an interface for unknown IID %s; releasing it rather than "
             "handing the guest a host vtable\n", wc_surface->name,
             debugstr_guid(riid) );
        winecom_host_release( *ppv );
        *ppv = NULL;
        return E_NOINTERFACE;
    }
    *ppv = winecom_wrap( *ppv, idx );
    return hr;
}

void winecom_wrap_static( void **p, UINT iface )
{
    if (p && *p) *p = winecom_wrap( *p, iface );
}

/* Give back every reverse proxy this call borrowed for an in-parameter.  A
 * callee that took its own reference keeps the object alive; a callee that did
 * not lets the proxy -- and with it the one guest reference it holds -- go. */
static void release_borrows( const UINT64 *args, const UINT *borrowed, UINT count )
{
    UINT n;

    for (n = 0; n < count; n++)
        winecom_to_native_end( (void *)(ULONG_PTR)args[borrowed[n]] );
}

NTSTATUS winecom_dispatch( UINT iface, UINT slot, AMD64_CONTEXT *ctx )
{
    const struct winecom_iface *itf;
    const struct winecom_slot *sl;
    struct com_proxy *proxy;
    UINT64 args[16] = { 0 };
    UINT64 arr_buf[32];
    UINT64 *arr_heap = NULL;
    UINT64 ret;
    UINT i, n, ppv_idx = 0, riid_idx = 0;
    UINT out_static_idx[8], n_out_static = 0;
    UINT out_arr_idx[8], n_out_arr = 0;
    /* Argument positions holding a REVERSE proxy this call minted for a
     * guest-implemented in-parameter.  Borrowed for the duration of the call
     * and given back on every path out of it -- see release_borrows(). */
    UINT borrowed[16], n_borrowed = 0;
    const UINT64 *arr_borrowed = NULL;
    UINT n_arr_borrowed = 0;
    BOOL have_ppv = FALSE;

    if (!com_ready()) return STATUS_DLL_INIT_FAILED;
    if (iface >= wc_surface->iface_count) return STATUS_INVALID_PARAMETER;
    itf = &wc_surface->ifaces[iface];
    if (slot >= itf->slot_count) return STATUS_INVALID_PARAMETER;

    proxy = (struct com_proxy *)(ULONG_PTR)ctx->R10;
    if (!proxy)
    {
        ERR( "%s slot %u called with NULL this\n", itf->name, slot );
        ctx->Rax = (UINT)E_INVALIDARG;
        return STATUS_SUCCESS;
    }
    if (proxy->iface != iface)
        WARN( "proxy %p says iface %u (%s), stub says %u (%s)\n", proxy,
              proxy->iface, wc_surface->ifaces[proxy->iface].name, iface, itf->name );

    /* IUnknown's three slots head every vtable and are served from the proxy
     * table; Release of the last reference is the only crossing. */
    if (slot < 3)
    {
        switch (slot)
        {
        case 0:
            ctx->Rax = (UINT)proxy_qi( proxy,
                                       (const GUID *)(ULONG_PTR)ctx->Rdx,
                                       (void **)(ULONG_PTR)ctx->R8 );
            break;
        case 1:
            ctx->Rax = proxy_addref( proxy );
            break;
        case 2:
            ctx->Rax = proxy_release( proxy );
            break;
        }
        return STATUS_SUCCESS;
    }

    sl = itf->slots ? &itf->slots[slot] : NULL;
    if (!sl || sl->refuse)
    {
        refuse_once( iface, slot, sl ? sl->name : NULL, sl ? sl->refuse : NULL );
        ctx->Rax = (UINT)E_NOTIMPL;
        return STATUS_SUCCESS;
    }

    TRACE( "%s (iface %u slot %u argc %u)\n", sl->name, iface, slot, sl->argc );

    if (sl->flags & WINECOM_F_HAND)
    {
        ctx->Rax = wc_surface->hand_funcs[sl->aux]( proxy->host, slot, ctx );
        return STATUS_SUCCESS;
    }

    args[0] = (UINT64)(ULONG_PTR)proxy->host;
    for (i = 1; i < sl->argc; i++)
    {
        UINT64 raw = winecom_read_arg( ctx, i );
        switch (sl->cls ? sl->cls[i - 1] : WINECOM_CA_PASS)
        {
        case WINECOM_CA_PASS:
        case WINECOM_CA_RIID:
        case WINECOM_CA_RET_PTR:
            /* A by-value integer narrower than 32 bits arrives with UNDEFINED
             * upper bits: MS-x64 lets the caller write only the declared width
             * (clang emits `movw $0x1, %dx`) and makes ignoring the rest the
             * callee's job, while ELFv2 makes extending it the CALLER's job
             * and the ppc64 callee trusts that it happened.  Do it here, which
             * is the only place that knows both the register and the declared
             * width.  See struct winecom_slot::narrowmask for the measurement.
             */
            if (sl->narrowmask & (1u << (i - 1)))
            {
                unsigned int bits = (sl->narrowwide & (1u << (i - 1))) ? 16 : 8;
                UINT64 mask = (1ull << bits) - 1;

                raw &= mask;
                if ((sl->narrowsign & (1u << (i - 1))) &&
                    (raw & (1ull << (bits - 1))))
                    raw |= ~mask;
            }
            /* A FOUR-byte argument is clean in a register (x86-64 zero-extends
             * 32-bit register writes) but NOT on the stack, where the guest's
             * 32-bit store leaves the slot's upper half stale and an ELFv2
             * callee trusts the caller extended it.  Extend per the declared
             * signedness -- see winecom_slot::dwordmask for the measurement
             * (CopyDescriptors' heap type, argument seven). */
            else if (sl->dwordmask & (1u << (i - 1)))
            {
                if (sl->dwordsign & (1u << (i - 1)))
                    raw = (UINT64)(INT64)(INT)raw;
                else
                    raw = (UINT)raw;
            }
            args[i] = raw;
            break;
        case WINECOM_CA_IFACE_IN:
        {
            /* One of our proxies unwraps to its host; NULL stays NULL; a
             * guest-IMPLEMENTED object gets a REVERSE proxy of the type the
             * signature declared, which the table records in xaux for exactly
             * this (a surface without WINECOM_SF_REVERSE, or a table that
             * predates the xaux row, still refuses -- fail closed).  The
             * reverse proxy is BORROWED for the duration of the call and given
             * back below, so a callee that took its own reference keeps the
             * object and a callee that did not lets it go. */
            void *host;
            if (!winecom_to_native( (void *)(ULONG_PTR)raw,
                                    (sl->xaux && (sl->xmask & (1u << (i - 1))))
                                        ? sl->xaux[i - 1] : ~0u, &host ))
            {
                refuse_once( iface, slot, sl->name,
                             "guest-implemented object as an in-parameter that "
                             "this surface cannot reverse-proxy" );
                if (arr_heap) RtlFreeHeap( NtCurrentTeb()->Peb->ProcessHeap, 0, arr_heap );
                release_borrows( args, borrowed, n_borrowed );
                ctx->Rax = (UINT)E_NOTIMPL;
                return STATUS_SUCCESS;
            }
            args[i] = (UINT64)(ULONG_PTR)host;
            if (host && n_borrowed < ARRAYSIZE(borrowed)) borrowed[n_borrowed++] = i;
            break;
        }
        case WINECOM_CA_IFACE_OUT_STATIC:
            args[i] = raw;
            if (raw && n_out_static < ARRAYSIZE(out_static_idx))
                out_static_idx[n_out_static++] = i;
            break;
        case WINECOM_CA_IFACE_ARR_OUT_STATIC:
            /* The array itself is the caller's buffer, so it crosses as an
             * address; what needs doing is on the way back, where every
             * element the callee wrote is a HOST pointer that must become a
             * proxy before the guest sees it.  Unlike CA_IFACE_ARR_IN there
             * is no copy: the callee writes the guest's own storage, and we
             * replace each element in place. */
            args[i] = raw;
            if (!sl->caux)
            {
                refuse_once( iface, slot, sl->name,
                             "interface out-array with no caux count-parameter "
                             "table; the generator must emit one" );
                if (arr_heap) RtlFreeHeap( NtCurrentTeb()->Peb->ProcessHeap, 0, arr_heap );
                release_borrows( args, borrowed, n_borrowed );
                ctx->Rax = (UINT)E_NOTIMPL;
                return STATUS_SUCCESS;
            }
            if (raw && n_out_arr < ARRAYSIZE(out_arr_idx))
                out_arr_idx[n_out_arr++] = i;
            break;
        case WINECOM_CA_PPV_OUT:
            args[i] = raw;
            have_ppv = TRUE;
            ppv_idx = i;
            riid_idx = sl->aux + 1;
            break;
        case WINECOM_CA_EVENT:
            if (raw)
            {
                /* d3d12 only: a real Wine event handle needs the eventfd
                 * relay; refuse with the reason instead of corrupting. */
                refuse_once( iface, slot, sl->name,
                             "non-NULL completion event needs the eventfd "
                             "relay" );
                if (arr_heap) RtlFreeHeap( NtCurrentTeb()->Peb->ProcessHeap, 0, arr_heap );
                release_borrows( args, borrowed, n_borrowed );
                ctx->Rax = (UINT)E_NOTIMPL;
                return STATUS_SUCCESS;
            }
            args[i] = 0;
            break;
        case WINECOM_CA_IFACE_ARR_IN:
        {
            UINT count = (UINT)winecom_read_arg( ctx, sl->aux2 + 1 );
            void *const *src = (void *const *)(ULONG_PTR)raw;
            UINT64 *dst;

            if (!src || !count)
            {
                args[i] = raw;
                break;
            }
            if (count <= ARRAYSIZE(arr_buf)) dst = arr_buf;
            else if (!(dst = arr_heap = RtlAllocateHeap( NtCurrentTeb()->Peb->ProcessHeap, 0,
                                                         count * sizeof(*dst) )))
            {
                release_borrows( args, borrowed, n_borrowed );
                ctx->Rax = (UINT)E_OUTOFMEMORY;
                return STATUS_SUCCESS;
            }
            for (n = 0; n < count; n++)
            {
                /* Element by element, the same classifier the scalar case
                 * uses.  A reverse proxy minted here is NOT recorded in
                 * borrowed[] -- that array is indexed by argument position and
                 * an array has one position for many objects -- so the element
                 * proxies are given back by their own loop after the call. */
                void *host;
                if (!winecom_to_native( src[n],
                                        (sl->xaux && (sl->xmask & (1u << (i - 1))))
                                            ? sl->xaux[i - 1] : ~0u, &host ))
                {
                    refuse_once( iface, slot, sl->name,
                                 "guest-implemented object in an array "
                                 "in-parameter that this surface cannot "
                                 "reverse-proxy" );
                    while (n--) winecom_to_native_end( (void *)(ULONG_PTR)dst[n] );
                    if (arr_heap) RtlFreeHeap( NtCurrentTeb()->Peb->ProcessHeap, 0, arr_heap );
                    release_borrows( args, borrowed, n_borrowed );
                    ctx->Rax = (UINT)E_NOTIMPL;
                    return STATUS_SUCCESS;
                }
                dst[n] = (UINT64)(ULONG_PTR)host;
            }
            arr_borrowed = dst;
            n_arr_borrowed = count;
            args[i] = (UINT64)(ULONG_PTR)dst;
            break;
        }
        default:
            refuse_once( iface, slot, sl->name, "argument class with no "
                         "runtime marshal path" );
            if (arr_heap) RtlFreeHeap( NtCurrentTeb()->Peb->ProcessHeap, 0, arr_heap );
            release_borrows( args, borrowed, n_borrowed );
            ctx->Rax = (UINT)E_NOTIMPL;
            return STATUS_SUCCESS;
        }
    }

    ret = wc_surface->invoke( proxy->host, slot, sl->argc, args );

    release_borrows( args, borrowed, n_borrowed );
    for (n = 0; n < n_arr_borrowed; n++)
        winecom_to_native_end( (void *)(ULONG_PTR)arr_borrowed[n] );
    if (arr_heap) RtlFreeHeap( NtCurrentTeb()->Peb->ProcessHeap, 0, arr_heap );

    if (have_ppv && SUCCEEDED((HRESULT)ret))
    {
        void **ppv = (void **)(ULONG_PTR)args[ppv_idx];
        const GUID *riid = (const GUID *)(ULONG_PTR)args[riid_idx];

        if (ppv && *ppv)
        {
            UINT idx = riid ? winecom_iface_from_iid( riid ) : ~0u;
            if (idx == ~0u)
            {
                ERR( "%s returned an interface for unknown IID %s; releasing "
                     "it rather than handing the guest a host vtable\n",
                     sl->name, debugstr_guid(riid) );
                winecom_host_release( *ppv );
                *ppv = NULL;
                ret = (UINT)E_NOINTERFACE;
            }
            else *ppv = winecom_wrap( *ppv, idx );
        }
    }
    if (n_out_static && SUCCEEDED((HRESULT)ret))
    {
        for (n = 0; n < n_out_static; n++)
        {
            void **out = (void **)(ULONG_PTR)args[out_static_idx[n]];
            if (out && *out)
                *out = winecom_wrap( *out, sl->xaux[out_static_idx[n] - 1] );
        }
    }
    /* Out-arrays are wrapped whatever the return value: the methods that
     * carry them are the void-returning state readbacks (OMGetRenderTargets
     * and its family), where RAX is scratch and SUCCEEDED() of scratch is
     * meaningless. */
    for (n = 0; n < n_out_arr; n++)
    {
        UINT idx = out_arr_idx[n];
        void **out = (void **)(ULONG_PTR)args[idx];
        UINT count = (UINT)winecom_read_arg( ctx, sl->caux[idx - 1] + 1 );
        UINT k;

        for (k = 0; k < count; k++)
            if (out[k]) out[k] = winecom_wrap( out[k], sl->xaux[idx - 1] );
    }

    if (sl->flags & WINECOM_F_RET_VIA_ARG)
        ctx->Rax = args[1];   /* == the callee's return value */
    else if (sl->flags & WINECOM_F_RET_VOID)
        ctx->Rax = 0;
    else
        ctx->Rax = ret;
    return STATUS_SUCCESS;
}
