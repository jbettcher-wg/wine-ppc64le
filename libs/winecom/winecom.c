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
 * TRANSLATE-IN (design §6.3, forward half).  An interface-typed IN value is
 * never blindly unwrapped: one of our proxies unwraps to its host pointer,
 * NULL stays NULL, and anything else is a guest-implemented object, which
 * refuses loudly until reverse proxies (§6) exist.  This replaces the
 * original d3d12 runtime's unconditional unwrap -- strictly safer: an
 * unrecognised pointer used to be dereferenced as a proxy.
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

WINE_DEFAULT_DEBUG_CHANNEL(winecom);

/* ---------------------------------------------------------------- proxies */

struct com_proxy
{
    const void *guest_vtbl;   /* first member: this IS the COM object the
                                 guest sees -- *(void**)proxy is its vtable */
    void       *host;         /* native/host interface pointer */
    LONG        refs;         /* guest-visible refcount, served here */
    UINT        iface;        /* index into surface->ifaces[] */
    struct com_proxy *next;   /* intern-table chain */
};

#define INTERN_BUCKETS 256

static CRITICAL_SECTION com_cs;
static CRITICAL_SECTION_DEBUG com_cs_debug =
{
    0, 0, &com_cs,
    { &com_cs_debug.ProcessLocksList, &com_cs_debug.ProcessLocksList },
      0, 0, { (DWORD_PTR)(__FILE__ ": winecom com_cs") }
};
static CRITICAL_SECTION com_cs = { &com_cs_debug, -1, 0, 0, 0, 0 };

static struct com_proxy *intern[INTERN_BUCKETS];

static const struct winecom_surface *surface;

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
             surface->name, guest );
        return FALSE;
    }
    info = ptr;
    if (info->version != COM_THUNK_INFO_VERSION || info->stride != 16)
    {
        ERR( "%s: guest com thunk info version %u stride %u not supported\n",
             surface->name, info->version, info->stride );
        return FALSE;
    }
    if (info->iface_count != surface->iface_count)
    {
        ERR( "%s: guest module has %u interfaces, marshal table has %u -- "
             "the two generators read different interface JSONs\n",
             surface->name, info->iface_count, surface->iface_count );
        return FALSE;
    }
    entries = (const struct com_iface_entry *)(base + info->ifaces_rva);
    for (i = 0; i < surface->iface_count; i++)
    {
        if (!IsEqualGUID( &entries[i].iid, &surface->ifaces[i].iid ))
        {
            ERR( "%s: interface %u (%s) IID mismatch between guest module "
                 "and marshal table\n", surface->name, i,
                 surface->ifaces[i].name );
            return FALSE;
        }
        if (entries[i].slot_count != surface->ifaces[i].slot_count)
        {
            ERR( "%s: interface %s: guest %u slots, table %u\n",
                 surface->name, surface->ifaces[i].name,
                 entries[i].slot_count, surface->ifaces[i].slot_count );
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

    if (!(guest = find_guest_module( surface->guest_modules,
                                     surface->module_count )))
    {
        ERR( "%s: no guest thunk module in this process; COM dispatch "
             "cannot work\n", surface->name );
        return FALSE;
    }
    /* validate every loaded candidate, materialise from the first */
    for (i = 0; i < surface->module_count; i++)
    {
        HMODULE mod = find_guest_module( &surface->guest_modules[i], 1 );
        if (mod && !com_check_module( mod, NULL )) return FALSE;
    }
    if (!com_check_module( guest, &info )) return FALSE;

    base = (ULONG_PTR)guest;
    entries = (const struct com_iface_entry *)(base + info->ifaces_rva);

    total_slots = 0;
    for (i = 0; i < surface->iface_count; i++) total_slots += entries[i].slot_count;

    if (!(guest_vtbls = RtlAllocateHeap( NtCurrentTeb()->Peb->ProcessHeap, 0,
                                         surface->iface_count * sizeof(*guest_vtbls)
                                         + surface->iface_count * sizeof(*iface_slot_base)
                                         + total_slots * sizeof(UINT64) + total_slots )))
        return FALSE;
    iface_slot_base = (UINT *)(guest_vtbls + surface->iface_count);
    guest_vtbl_block = (UINT64 *)(iface_slot_base + surface->iface_count);
    refuse_logged = (unsigned char *)(guest_vtbl_block + total_slots);
    memset( refuse_logged, 0, total_slots );

    off = 0;
    for (i = 0; i < surface->iface_count; i++)
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
           surface->name, total_slots, i, guest );
    if (nowrap)
        ERR( "%s: WINEEMUNOCOMWRAP=1 -- interface pointers will cross RAW; "
             "expect guest calls into native vtables\n", surface->name );
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
    surface = s;
    if (!com_runtime_init_once())
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

/* ------------------------------------------------------------ host calls */

void winecom_host_release( void *host )
{
    UINT64 args[16] = { 0 };
    surface->invoke( host, 2 /* IUnknown::Release */, 1, args );
}

static HRESULT host_qi( void *host, const GUID *riid, void **out )
{
    UINT64 args[16] = { 0 };

    *out = NULL;
    args[1] = (UINT64)(ULONG_PTR)riid;
    args[2] = (UINT64)(ULONG_PTR)out;
    return (HRESULT)surface->invoke( host, 0 /* QueryInterface */, 3, args );
}

/* ------------------------------------------------------- proxy operations */

UINT winecom_iface_from_iid( const GUID *riid )
{
    UINT i;
    for (i = 0; i < surface->iface_count; i++)
        if (IsEqualGUID( riid, &surface->ifaces[i].iid )) return i;
    return ~0u;
}

void *winecom_wrap( void *host, UINT iface )
{
    UINT bucket = (UINT)(((ULONG_PTR)host >> 4) % INTERN_BUCKETS);
    struct com_proxy *p;

    if (!host) return NULL;
    if (nowrap)
    {
        ERR( "WINEEMUNOCOMWRAP: handing raw host %s %p to the guest\n",
             surface->ifaces[iface].name, host );
        return host;
    }
    RtlEnterCriticalSection( &com_cs );
    for (p = intern[bucket]; p; p = p->next)
        if (p->host == host && p->iface == iface) break;
    if (p)
    {
        p->refs++;
        RtlLeaveCriticalSection( &com_cs );
        winecom_host_release( host );  /* surplus reference */
        return p;
    }
    if (!(p = RtlAllocateHeap( NtCurrentTeb()->Peb->ProcessHeap, 0, sizeof(*p) )))
    {
        RtlLeaveCriticalSection( &com_cs );
        ERR( "out of memory interning %s %p\n", surface->ifaces[iface].name, host );
        winecom_host_release( host );
        return NULL;
    }
    p->guest_vtbl = guest_vtbls[iface];
    p->host = host;
    p->refs = 1;
    p->iface = iface;
    p->next = intern[bucket];
    intern[bucket] = p;
    RtlLeaveCriticalSection( &com_cs );
    TRACE( "wrapped %s host %p as proxy %p\n", surface->ifaces[iface].name,
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
        for (i = 0; i < surface->iface_count; i++)
            if ((const void *)guest_vtbls[i] == cand->guest_vtbl)
            {
                UINT bucket = (UINT)(((ULONG_PTR)cand->host >> 4) % INTERN_BUCKETS);
                struct com_proxy *p;
                RtlEnterCriticalSection( &com_cs );
                for (p = intern[bucket]; p; p = p->next)
                    if (p == cand) break;
                RtlLeaveCriticalSection( &com_cs );
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

BOOL winecom_translate_in( void *guest_seen, void **host_out )
{
    struct com_proxy *p;

    *host_out = NULL;
    if (!guest_seen) return TRUE;
    if ((p = proxy_from_pointer( guest_seen )))
    {
        *host_out = p->host;
        return TRUE;
    }
    /* A guest-implemented object: needs a reverse proxy (design §6), which
     * does not exist yet.  Refusing here is the fail-closed answer; the
     * caller turns it into a loud E_NOTIMPL. */
    return FALSE;
}

static ULONG proxy_addref( struct com_proxy *p )
{
    ULONG refs;
    RtlEnterCriticalSection( &com_cs );
    refs = ++p->refs;
    RtlLeaveCriticalSection( &com_cs );
    return refs;
}

static ULONG proxy_release( struct com_proxy *p )
{
    UINT bucket = (UINT)(((ULONG_PTR)p->host >> 4) % INTERN_BUCKETS);
    struct com_proxy **link;
    void *host = NULL;
    ULONG refs;

    RtlEnterCriticalSection( &com_cs );
    refs = --p->refs;
    if (!refs)
    {
        for (link = &intern[bucket]; *link; link = &(*link)->next)
            if (*link == p) { *link = p->next; break; }
        host = p->host;
    }
    RtlLeaveCriticalSection( &com_cs );
    if (!refs)
    {
        TRACE( "destroying proxy %p (%s host %p)\n", p,
               surface->ifaces[p->iface].name, host );
        winecom_host_release( host );
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
    idx = winecom_iface_from_iid( riid );
    if (idx == ~0u)
    {
        /* The host may well implement this IID, but with no stub vtable for
         * it a host pointer handed to the guest would be called as x86-64
         * code.  Refuse loudly instead. */
        WARN( "%s: QI for unknown IID %s refused\n",
              surface->ifaces[p->iface].name, debugstr_guid(riid) );
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
    FIXME( "%s: refusing %s (iface %u slot %u): %s\n", surface->name,
           name ? name : surface->ifaces[iface].name, iface, slot,
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
             "handing the guest a host vtable\n", surface->name,
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
    UINT out_static_idx[4], n_out_static = 0;
    BOOL have_ppv = FALSE;

    if (!com_ready()) return STATUS_DLL_INIT_FAILED;
    if (iface >= surface->iface_count) return STATUS_INVALID_PARAMETER;
    itf = &surface->ifaces[iface];
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
              proxy->iface, surface->ifaces[proxy->iface].name, iface, itf->name );

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
        ctx->Rax = surface->hand_funcs[sl->aux]( proxy->host, slot, ctx );
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
            args[i] = raw;
            break;
        case WINECOM_CA_IFACE_IN:
        {
            void *host;
            if (!winecom_translate_in( (void *)(ULONG_PTR)raw, &host ))
            {
                refuse_once( iface, slot, sl->name,
                             "guest-implemented object as an in-parameter; "
                             "reverse proxies (design step 5) not built yet" );
                if (arr_heap) RtlFreeHeap( NtCurrentTeb()->Peb->ProcessHeap, 0, arr_heap );
                ctx->Rax = (UINT)E_NOTIMPL;
                return STATUS_SUCCESS;
            }
            args[i] = (UINT64)(ULONG_PTR)host;
            break;
        }
        case WINECOM_CA_IFACE_OUT_STATIC:
            args[i] = raw;
            if (raw && n_out_static < ARRAYSIZE(out_static_idx))
                out_static_idx[n_out_static++] = i;
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
                ctx->Rax = (UINT)E_OUTOFMEMORY;
                return STATUS_SUCCESS;
            }
            for (n = 0; n < count; n++)
            {
                void *host;
                if (!winecom_translate_in( src[n], &host ))
                {
                    refuse_once( iface, slot, sl->name,
                                 "guest-implemented object in an array "
                                 "in-parameter; reverse proxies not built yet" );
                    if (arr_heap) RtlFreeHeap( NtCurrentTeb()->Peb->ProcessHeap, 0, arr_heap );
                    ctx->Rax = (UINT)E_NOTIMPL;
                    return STATUS_SUCCESS;
                }
                dst[n] = (UINT64)(ULONG_PTR)host;
            }
            args[i] = (UINT64)(ULONG_PTR)dst;
            break;
        }
        default:
            refuse_once( iface, slot, sl->name, "argument class with no "
                         "runtime marshal path" );
            if (arr_heap) RtlFreeHeap( NtCurrentTeb()->Peb->ProcessHeap, 0, arr_heap );
            ctx->Rax = (UINT)E_NOTIMPL;
            return STATUS_SUCCESS;
        }
    }

    ret = surface->invoke( proxy->host, slot, sl->argc, args );

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

    if (sl->flags & WINECOM_F_RET_VIA_ARG)
        ctx->Rax = args[1];   /* == the callee's return value */
    else if (sl->flags & WINECOM_F_RET_VOID)
        ctx->Rax = 0;
    else
        ctx->Rax = ret;
    return STATUS_SUCCESS;
}
