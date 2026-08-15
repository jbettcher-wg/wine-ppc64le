/*
 * Native d3d12.dll -- the winecom runtime and vkd3d-proton's PE front.
 *
 * This module REPLACES Wine's d3d12 forwarder into libs/vkd3d.  It is the
 * native half of the native-lane D3D12 stack (design:
 * vkd3d-ppc64le/docs/d3d12-native-lane-design.md §3.3/§3.4):
 *
 *   guest x86-64 PE  -->  C:\windows\sysx8664\d3d12.dll   (spec2thunk COM
 *      |                  mode: pure trap surface, no marshalling knowledge)
 *      |  trap; ntdll's dispatcher maps RIP -> (iface, slot) and calls
 *      v
 *   __wine_com_dispatch( iface, slot, AMD64_CONTEXT * )       <-- THIS FILE
 *      |  reads the guest's MS-x64 argument state out of the CONTEXT,
 *      |  translates proxies at the positions the generated marshal table
 *      |  marks (d3d12_marshal.h, from gen_winecom.py), and places the
 *      |  return value back in ctx->Rax
 *      v
 *   d3d12.so (unixlib, unix.c)  -->  libvkd3d-proton-d3d12.so
 *
 * DISPATCH CONTRACT with dlls/ntdll/signal_ppc64.c: the dispatcher calls
 * __wine_com_dispatch on the Win32 stack it is already standing on, with the
 * trap CONTEXT still describing the guest call (Rip on the trap instruction,
 * Rsp on the return address, arg0 rescued into R10).  On STATUS_SUCCESS this
 * function has fully handled the call -- including writing ctx->Rax -- and
 * the DISPATCHER pops the return address (Rip = [Rsp], Rsp += 8).  Any other
 * status means the trap was not served and the guest gets an illegal-
 * instruction exception; that path is for broken images, not for refused
 * slots, which are served loudly with E_NOTIMPL here.
 *
 * PROXIES.  One `struct com_proxy` per host interface pointer, interned;
 * wrap() consumes one host reference (the dxvk_proxy_wrap ownership contract,
 * ported).  The guest-facing vtable of a proxy is the address sequence of the
 * guest module's stub array for that interface type, materialised once at
 * attach from the guest d3d12.dll's own __wine_com_thunk_info -- and the IIDs
 * published there are cross-checked against our generated table, so the two
 * generators reading the same interfaces JSON cannot silently disagree.
 * All slots trap, including AddRef/Release/QueryInterface (simplicity first;
 * the guest-side refcount optimisation was an answer to FEX constraints the
 * native lane does not have).
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

#include <vkd3d_d3d12.h>

#include "winternl.h"
#include "wine/debug.h"

#include "unixlib.h"
#include "d3d12_marshal.h"

WINE_DEFAULT_DEBUG_CHANNEL(d3d12);

/* ---------------------------------------------------------------- proxies */

struct com_proxy
{
    const void *guest_vtbl;   /* first member: this IS the COM object the
                                 guest sees -- *(void**)proxy is its vtable */
    void       *host;         /* native interface pointer (vkd3d) */
    LONG        refs;         /* guest-visible refcount, served here */
    UINT        iface;        /* index into d3d12_com_ifaces[] */
    struct com_proxy *next;   /* intern-table chain */
};

#define INTERN_BUCKETS 256

static CRITICAL_SECTION com_cs;
static CRITICAL_SECTION_DEBUG com_cs_debug =
{
    0, 0, &com_cs,
    { &com_cs_debug.ProcessLocksList, &com_cs_debug.ProcessLocksList },
      0, 0, { (DWORD_PTR)(__FILE__ ": com_cs") }
};
static CRITICAL_SECTION com_cs = { &com_cs_debug, -1, 0, 0, 0, 0 };

static struct com_proxy *intern[INTERN_BUCKETS];

/* Guest-facing vtables: guest_vtbls[i][n] is the address of slot n's trap
 * stub in the guest module's stub array for interface i. */
static const UINT64 *guest_vtbls[D3D12_IFACE_COUNT];
static UINT64 *guest_vtbl_block;
static unsigned char *refuse_logged;   /* one byte per (iface, slot) */
static UINT iface_slot_base[D3D12_IFACE_COUNT];
static UINT total_slots;

static LONG com_init_state;            /* 0 = no, 1 = in progress, 2 = ok,
                                          3 = failed */

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

static HMODULE find_guest_d3d12( void )
{
    static const WCHAR nameW[] = L"d3d12.dll";
    UNICODE_STRING want;
    LIST_ENTRY *mark, *entry;
    HMODULE ret = NULL;
    ULONG_PTR magic;

    RtlInitUnicodeString( &want, nameW );
    LdrLockLoaderLock( 0, NULL, &magic );
    mark = &NtCurrentTeb()->Peb->LdrData->InMemoryOrderModuleList;
    for (entry = mark->Flink; entry != mark; entry = entry->Flink)
    {
        LDR_DATA_TABLE_ENTRY *mod = CONTAINING_RECORD( entry, LDR_DATA_TABLE_ENTRY,
                                                       InMemoryOrderLinks );
        const IMAGE_NT_HEADERS *nt = RtlImageNtHeader( mod->DllBase );

        if (!nt || nt->FileHeader.Machine != IMAGE_FILE_MACHINE_AMD64) continue;
        if (RtlEqualUnicodeString( &want, &mod->BaseDllName, TRUE ))
        {
            ret = mod->DllBase;
            break;
        }
    }
    LdrUnlockLoaderLock( 0, magic );
    return ret;
}

/* Build the guest vtables from the guest module's published stub arrays,
 * cross-checking every IID against our generated table. */
static BOOL com_runtime_init_once( void )
{
    const struct com_thunk_info *info;
    const struct com_iface_entry *entries;
    ANSI_STRING name;
    ULONG_PTR base;
    HMODULE guest;
    UINT i, n, off;
    void *ptr;

    if (!(guest = find_guest_d3d12()))
    {
        ERR( "no guest d3d12.dll in this process; COM dispatch cannot work\n" );
        return FALSE;
    }
    RtlInitAnsiString( &name, "__wine_com_thunk_info" );
    if (LdrGetProcedureAddress( guest, &name, 0, &ptr ))
    {
        ERR( "guest d3d12.dll exports no __wine_com_thunk_info\n" );
        return FALSE;
    }
    info = ptr;
    base = (ULONG_PTR)guest;
    if (info->version != COM_THUNK_INFO_VERSION || info->stride != 16)
    {
        ERR( "guest com thunk info version %u stride %u not supported\n",
             info->version, info->stride );
        return FALSE;
    }
    if (info->iface_count != D3D12_IFACE_COUNT)
    {
        ERR( "guest module has %u interfaces, marshal table has %u -- the two "
             "generators read different interface JSONs\n",
             info->iface_count, (UINT)D3D12_IFACE_COUNT );
        return FALSE;
    }
    entries = (const struct com_iface_entry *)(base + info->ifaces_rva);

    total_slots = 0;
    for (i = 0; i < D3D12_IFACE_COUNT; i++)
    {
        if (!IsEqualGUID( &entries[i].iid, &d3d12_com_ifaces[i].iid ))
        {
            ERR( "interface %u (%s) IID mismatch between guest module and "
                 "marshal table\n", i, d3d12_com_ifaces[i].name );
            return FALSE;
        }
        if (entries[i].slot_count != d3d12_com_ifaces[i].slot_count)
        {
            ERR( "interface %s: guest %u slots, table %u\n",
                 d3d12_com_ifaces[i].name, entries[i].slot_count,
                 d3d12_com_ifaces[i].slot_count );
            return FALSE;
        }
        iface_slot_base[i] = total_slots;
        total_slots += entries[i].slot_count;
    }
    if (!(guest_vtbl_block = RtlAllocateHeap( NtCurrentTeb()->Peb->ProcessHeap, 0,
                                              total_slots * sizeof(UINT64) + total_slots )))
        return FALSE;
    refuse_logged = (unsigned char *)(guest_vtbl_block + total_slots);
    memset( refuse_logged, 0, total_slots );
    off = 0;
    for (i = 0; i < D3D12_IFACE_COUNT; i++)
    {
        guest_vtbls[i] = guest_vtbl_block + off;
        for (n = 0; n < entries[i].slot_count; n++)
            guest_vtbl_block[off + n] = base + entries[i].stubs_rva + n * (UINT64)info->stride;
        off += entries[i].slot_count;
    }
    TRACE( "materialised %u guest vtable slots across %u interfaces from %p\n",
           total_slots, i, guest );
    return TRUE;
}

static BOOL com_runtime_init( void )
{
    LONG state;

    while ((state = InterlockedCompareExchange( &com_init_state, 1, 0 )))
    {
        if (state == 2) return TRUE;
        if (state == 3) return FALSE;
        NtYieldExecution();
    }
    if (D3D12_UNIX_CALL( init, NULL ))
    {
        InterlockedExchange( &com_init_state, 3 );
        return FALSE;
    }
    if (!com_runtime_init_once())
    {
        InterlockedExchange( &com_init_state, 3 );
        return FALSE;
    }
    InterlockedExchange( &com_init_state, 2 );
    return TRUE;
}

/* ------------------------------------------------------------- unix calls */

static UINT64 unix_vtbl_call( void *host, UINT slot, UINT argc, UINT64 *args )
{
    struct d3d12_call_params p;
    NTSTATUS status;

    memcpy( p.args, args, sizeof(p.args) );
    p.args[0] = (UINT64)(ULONG_PTR)host;
    p.slot = slot;
    p.argc = argc;
    p.ret = 0;
    if ((status = D3D12_UNIX_CALL( call, &p )))
    {
        ERR( "unix call failed, status %08x\n", (UINT)status );
        return (UINT64)(UINT)E_FAIL;
    }
    return p.ret;
}

static void host_release( void *host )
{
    UINT64 args[D3D12_UNIX_MAX_ARGS] = { 0 };
    unix_vtbl_call( host, 2 /* IUnknown::Release */, 1, args );
}

static HRESULT host_qi( void *host, const GUID *riid, void **out )
{
    UINT64 args[D3D12_UNIX_MAX_ARGS] = { 0 };

    *out = NULL;
    args[1] = (UINT64)(ULONG_PTR)riid;
    args[2] = (UINT64)(ULONG_PTR)out;
    return (HRESULT)unix_vtbl_call( host, 0 /* QueryInterface */, 3, args );
}

/* ------------------------------------------------------- proxy operations */

static UINT iface_from_iid( const GUID *riid )
{
    UINT i;
    for (i = 0; i < D3D12_IFACE_COUNT; i++)
        if (IsEqualGUID( riid, &d3d12_com_ifaces[i].iid )) return i;
    return ~0u;
}

/* Intern a host interface pointer as a guest-callable proxy.  CONSUMES one
 * host reference; if the pointer is already interned the surplus host
 * reference is dropped.  The returned proxy carries one guest reference. */
static void *com_wrap( void *host, UINT iface )
{
    UINT bucket = (UINT)(((ULONG_PTR)host >> 4) % INTERN_BUCKETS);
    struct com_proxy *p;

    if (!host) return NULL;
    RtlEnterCriticalSection( &com_cs );
    for (p = intern[bucket]; p; p = p->next)
        if (p->host == host && p->iface == iface) break;
    if (p)
    {
        p->refs++;
        RtlLeaveCriticalSection( &com_cs );
        host_release( host );          /* surplus reference */
        return p;
    }
    if (!(p = RtlAllocateHeap( NtCurrentTeb()->Peb->ProcessHeap, 0, sizeof(*p) )))
    {
        RtlLeaveCriticalSection( &com_cs );
        ERR( "out of memory interning %s %p\n", d3d12_com_ifaces[iface].name, host );
        host_release( host );
        return NULL;
    }
    p->guest_vtbl = guest_vtbls[iface];
    p->host = host;
    p->refs = 1;
    p->iface = iface;
    p->next = intern[bucket];
    intern[bucket] = p;
    RtlLeaveCriticalSection( &com_cs );
    TRACE( "wrapped %s host %p as proxy %p\n", d3d12_com_ifaces[iface].name,
           host, p );
    return p;
}

/* Proxy -> host pointer for interface-typed IN parameters.  NULL-safe. */
static void *com_unwrap( void *proxy )
{
    if (!proxy) return NULL;
    return ((struct com_proxy *)proxy)->host;
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
               d3d12_com_ifaces[p->iface].name, host );
        host_release( host );
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
    idx = iface_from_iid( riid );
    if (idx == ~0u)
    {
        /* The host may well implement this IID, but with no stub vtable for
         * it a host pointer handed to the guest would be called as x86-64
         * code.  Refuse loudly instead. */
        WARN( "%s: QI for unknown IID %s refused\n",
              d3d12_com_ifaces[p->iface].name, debugstr_guid(riid) );
        return E_NOINTERFACE;
    }
    hr = host_qi( p->host, riid, &host_out );
    if (FAILED(hr)) return hr;
    if (!(*ppv = com_wrap( host_out, idx ))) return E_OUTOFMEMORY;
    return S_OK;
}

/* ------------------------------------------------------- argument reading */

static UINT64 read_arg( const AMD64_CONTEXT *ctx, UINT n )
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

/* ----------------------------------------------------- hand-written slots */

typedef UINT64 (*hand_func)( void *host, UINT slot, const AMD64_CONTEXT *ctx );

/* ID3D12GraphicsCommandList::ResourceBarrier( UINT n, const
 * D3D12_RESOURCE_BARRIER *barriers ): the barrier structs carry
 * ID3D12Resource* members (directly and through unions), so the array is
 * copied shallowly and the resource pointers unwrapped in the copy. */
static UINT64 hand_resource_barrier( void *host, UINT slot, const AMD64_CONTEXT *ctx )
{
    UINT n = (UINT)read_arg( ctx, 1 );
    const D3D12_RESOURCE_BARRIER *src = (const void *)(ULONG_PTR)read_arg( ctx, 2 );
    UINT64 args[D3D12_UNIX_MAX_ARGS] = { 0 };
    D3D12_RESOURCE_BARRIER *copy = NULL;
    UINT64 ret;
    UINT i;

    if (n && src)
    {
        if (!(copy = RtlAllocateHeap( NtCurrentTeb()->Peb->ProcessHeap, 0,
                                      n * sizeof(*copy) )))
            return (UINT64)(UINT)E_OUTOFMEMORY;
        memcpy( copy, src, n * sizeof(*copy) );
        for (i = 0; i < n; i++)
        {
            switch (copy[i].Type)
            {
            case D3D12_RESOURCE_BARRIER_TYPE_TRANSITION:
                copy[i].Transition.pResource = com_unwrap( copy[i].Transition.pResource );
                break;
            case D3D12_RESOURCE_BARRIER_TYPE_ALIASING:
                copy[i].Aliasing.pResourceBefore = com_unwrap( copy[i].Aliasing.pResourceBefore );
                copy[i].Aliasing.pResourceAfter = com_unwrap( copy[i].Aliasing.pResourceAfter );
                break;
            case D3D12_RESOURCE_BARRIER_TYPE_UAV:
                copy[i].UAV.pResource = com_unwrap( copy[i].UAV.pResource );
                break;
            default:
                WARN( "unknown barrier type %u passed through\n", copy[i].Type );
                break;
            }
        }
    }
    args[1] = n;
    args[2] = (UINT64)(ULONG_PTR)(copy ? copy : src);
    ret = unix_vtbl_call( host, slot, 3, args );
    if (copy) RtlFreeHeap( NtCurrentTeb()->Peb->ProcessHeap, 0, copy );
    return ret;   /* void method; RAX is scratch */
}

/* ID3D12Device::CreateComputePipelineState( const
 * D3D12_COMPUTE_PIPELINE_STATE_DESC *desc, REFIID riid, void **ppv ):
 * the desc carries ID3D12RootSignature *pRootSignature. */
static UINT64 hand_create_compute_pso( void *host, UINT slot, const AMD64_CONTEXT *ctx )
{
    const D3D12_COMPUTE_PIPELINE_STATE_DESC *src = (const void *)(ULONG_PTR)read_arg( ctx, 1 );
    const GUID *riid = (const GUID *)(ULONG_PTR)read_arg( ctx, 2 );
    void **ppv = (void **)(ULONG_PTR)read_arg( ctx, 3 );
    D3D12_COMPUTE_PIPELINE_STATE_DESC desc;
    UINT64 args[D3D12_UNIX_MAX_ARGS] = { 0 };
    HRESULT hr;
    UINT idx;

    if (!src || !ppv) return (UINT64)(UINT)E_INVALIDARG;
    desc = *src;
    desc.pRootSignature = com_unwrap( desc.pRootSignature );
    args[1] = (UINT64)(ULONG_PTR)&desc;
    args[2] = (UINT64)(ULONG_PTR)riid;
    args[3] = (UINT64)(ULONG_PTR)ppv;
    hr = (HRESULT)unix_vtbl_call( host, slot, 4, args );
    if (SUCCEEDED(hr) && *ppv)
    {
        idx = iface_from_iid( riid );
        if (idx == ~0u)
        {
            ERR( "CreateComputePipelineState returned unknown IID %s\n",
                 debugstr_guid(riid) );
            host_release( *ppv );
            *ppv = NULL;
            return (UINT64)(UINT)E_NOINTERFACE;
        }
        *ppv = com_wrap( *ppv, idx );
    }
    return (UINT64)(UINT)hr;
}

static const hand_func d3d12_hand_funcs[] =
{
    hand_resource_barrier,
    hand_create_compute_pso,
};

C_ASSERT( ARRAYSIZE(d3d12_hand_funcs) == D3D12_HAND_COUNT );

/* -------------------------------------------------------------- dispatch */

static void refuse_once( UINT iface, UINT slot, const char *name, const char *why )
{
    unsigned char *flag = &refuse_logged[iface_slot_base[iface] + slot];
    if (*flag) return;
    *flag = 1;
    FIXME( "refusing %s (iface %u slot %u): %s\n",
           name ? name : d3d12_com_ifaces[iface].name, iface, slot,
           why ? why : "no marshal plan (not in the starter subset)" );
}

NTSTATUS WINAPI __wine_com_dispatch( UINT iface, UINT slot, AMD64_CONTEXT *ctx )
{
    const struct d3d12_com_iface *itf;
    const struct d3d12_com_slot *sl;
    struct com_proxy *proxy;
    UINT64 args[D3D12_UNIX_MAX_ARGS] = { 0 };
    UINT64 arr_buf[32];
    UINT64 *arr_heap = NULL;
    UINT64 ret;
    UINT i, ppv_idx = 0, riid_idx = 0;
    BOOL have_ppv = FALSE;

    if (!com_runtime_init()) return STATUS_DLL_INIT_FAILED;
    if (iface >= D3D12_IFACE_COUNT) return STATUS_INVALID_PARAMETER;
    itf = &d3d12_com_ifaces[iface];
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
              proxy->iface, d3d12_com_ifaces[proxy->iface].name, iface, itf->name );

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

    if (sl->flags & COMF_HAND)
    {
        ctx->Rax = d3d12_hand_funcs[sl->aux]( proxy->host, slot, ctx );
        return STATUS_SUCCESS;
    }

    args[0] = (UINT64)(ULONG_PTR)proxy->host;
    for (i = 1; i < sl->argc; i++)
    {
        UINT64 raw = read_arg( ctx, i );
        switch (sl->cls ? sl->cls[i - 1] : CA_PASS)
        {
        case CA_PASS:
        case CA_RIID:
        case CA_RET_PTR:
            args[i] = raw;
            break;
        case CA_IFACE_IN:
            args[i] = (UINT64)(ULONG_PTR)com_unwrap( (void *)(ULONG_PTR)raw );
            break;
        case CA_PPV_OUT:
            args[i] = raw;
            have_ppv = TRUE;
            ppv_idx = i;
            riid_idx = sl->aux + 1;
            break;
        case CA_EVENT:
            if (raw)
            {
                /* A real Wine event handle: the eventfd relay is design §6 /
                 * step 5.  Handing the raw handle to vkd3d would be refused
                 * there anyway (tagged-handle patch); refuse it here with the
                 * reason instead. */
                refuse_once( iface, slot, sl->name,
                             "non-NULL completion event needs the step-5 "
                             "eventfd relay" );
                if (arr_heap) RtlFreeHeap( NtCurrentTeb()->Peb->ProcessHeap, 0, arr_heap );
                ctx->Rax = (UINT)E_NOTIMPL;
                return STATUS_SUCCESS;
            }
            args[i] = 0;
            break;
        case CA_IFACE_ARR_IN:
        {
            UINT count = (UINT)read_arg( ctx, sl->aux2 + 1 );
            void *const *src = (void *const *)(ULONG_PTR)raw;
            UINT64 *dst;
            UINT n;

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
                dst[n] = (UINT64)(ULONG_PTR)com_unwrap( src[n] );
            args[i] = (UINT64)(ULONG_PTR)dst;
            break;
        }
        }
    }

    ret = unix_vtbl_call( proxy->host, slot, sl->argc, args );

    if (arr_heap) RtlFreeHeap( NtCurrentTeb()->Peb->ProcessHeap, 0, arr_heap );

    if (have_ppv && SUCCEEDED((HRESULT)ret))
    {
        void **ppv = (void **)(ULONG_PTR)args[ppv_idx];
        const GUID *riid = (const GUID *)(ULONG_PTR)args[riid_idx];

        if (ppv && *ppv)
        {
            UINT idx = riid ? iface_from_iid( riid ) : ~0u;
            if (idx == ~0u)
            {
                ERR( "%s returned an interface for unknown IID %s; releasing "
                     "it rather than handing the guest a host vtable\n",
                     sl->name, debugstr_guid(riid) );
                host_release( *ppv );
                *ppv = NULL;
                ret = (UINT)E_NOINTERFACE;
            }
            else *ppv = com_wrap( *ppv, idx );
        }
    }

    if (sl->flags & COMF_RET_VIA_ARG)
        ctx->Rax = args[1];   /* == the callee's return value; see design §5 */
    else if (sl->flags & COMF_RET_VOID)
        ctx->Rax = 0;
    else
        ctx->Rax = ret;
    return STATUS_SUCCESS;
}

/* ---------------------------------------------------------- flat entries */

static HRESULT flat_call( UINT func, UINT argc, UINT64 *args, UINT64 *ret )
{
    struct d3d12_flat_params p;
    NTSTATUS status;

    if (!com_runtime_init()) return E_FAIL;
    memcpy( p.args, args, sizeof(p.args) );
    p.func = func;
    p.argc = argc;
    p.ret = 0;
    if ((status = D3D12_UNIX_CALL( flat, &p )))
    {
        ERR( "unix flat call %u failed, status %08x\n", func, (UINT)status );
        return E_FAIL;
    }
    if (ret) *ret = p.ret;
    return (HRESULT)p.ret;
}

static HRESULT wrap_out_iface( HRESULT hr, const GUID *riid, void **ppv )
{
    UINT idx;

    if (FAILED(hr) || !ppv || !*ppv) return hr;
    idx = riid ? iface_from_iid( riid ) : ~0u;
    if (idx == ~0u)
    {
        ERR( "flat entry returned an interface for unknown IID %s\n",
             debugstr_guid(riid) );
        host_release( *ppv );
        *ppv = NULL;
        return E_NOINTERFACE;
    }
    *ppv = com_wrap( *ppv, idx );
    return hr;
}

/* Wrap a known-type out interface (the ID3DBlob pair of the serialize
 * entries), regardless of hr -- the error blob is set on FAILURE. */
static void wrap_fixed_iface( void **p, UINT idx )
{
    if (p && *p) *p = com_wrap( *p, idx );
}

HRESULT WINAPI D3D12CreateDevice( IUnknown *adapter, D3D_FEATURE_LEVEL fl,
                                  REFIID riid, void **device )
{
    UINT64 args[8] = { 0 };
    HRESULT hr;

    TRACE( "adapter %p, fl %#x, riid %s, device %p\n", adapter, fl,
           debugstr_guid(riid), device );
    if (adapter)
        FIXME( "ignoring adapter %p -- no native-lane DXGI yet, and vkd3d's "
               "native path ignores it too\n", adapter );
    args[0] = 0;   /* never a guest proxy: vkd3d would not understand it */
    args[1] = (UINT)fl;
    args[2] = (UINT64)(ULONG_PTR)riid;
    args[3] = (UINT64)(ULONG_PTR)device;
    hr = flat_call( FLAT_D3D12CreateDevice, 4, args, NULL );
    return wrap_out_iface( hr, riid, device );
}

HRESULT WINAPI D3D12GetDebugInterface( REFIID riid, void **debug )
{
    UINT64 args[8] = { 0 };
    HRESULT hr;

    TRACE( "riid %s, debug %p\n", debugstr_guid(riid), debug );
    args[0] = (UINT64)(ULONG_PTR)riid;
    args[1] = (UINT64)(ULONG_PTR)debug;
    hr = flat_call( FLAT_D3D12GetDebugInterface, 2, args, NULL );
    return wrap_out_iface( hr, riid, debug );
}

HRESULT WINAPI D3D12CreateRootSignatureDeserializer( const void *data, SIZE_T size,
                                                     REFIID riid, void **out )
{
    UINT64 args[8] = { 0 };
    HRESULT hr;

    TRACE( "data %p, size %Iu, riid %s, out %p\n", data, size,
           debugstr_guid(riid), out );
    args[0] = (UINT64)(ULONG_PTR)data;
    args[1] = size;
    args[2] = (UINT64)(ULONG_PTR)riid;
    args[3] = (UINT64)(ULONG_PTR)out;
    hr = flat_call( FLAT_D3D12CreateRootSignatureDeserializer, 4, args, NULL );
    return wrap_out_iface( hr, riid, out );
}

HRESULT WINAPI D3D12CreateVersionedRootSignatureDeserializer( const void *data, SIZE_T size,
                                                              REFIID riid, void **out )
{
    UINT64 args[8] = { 0 };
    HRESULT hr;

    TRACE( "data %p, size %Iu, riid %s, out %p\n", data, size,
           debugstr_guid(riid), out );
    args[0] = (UINT64)(ULONG_PTR)data;
    args[1] = size;
    args[2] = (UINT64)(ULONG_PTR)riid;
    args[3] = (UINT64)(ULONG_PTR)out;
    hr = flat_call( FLAT_D3D12CreateVersionedRootSignatureDeserializer, 4, args, NULL );
    return wrap_out_iface( hr, riid, out );
}

HRESULT WINAPI D3D12EnableExperimentalFeatures( UINT count, const IID *iids,
                                                void *structs, UINT *sizes )
{
    UINT64 args[8] = { 0 };

    TRACE( "count %u, iids %p, structs %p, sizes %p\n", count, iids, structs, sizes );
    args[0] = count;
    args[1] = (UINT64)(ULONG_PTR)iids;
    args[2] = (UINT64)(ULONG_PTR)structs;
    args[3] = (UINT64)(ULONG_PTR)sizes;
    return flat_call( FLAT_D3D12EnableExperimentalFeatures, 4, args, NULL );
}

HRESULT WINAPI D3D12SerializeRootSignature( const D3D12_ROOT_SIGNATURE_DESC *desc,
                                            D3D_ROOT_SIGNATURE_VERSION version,
                                            ID3DBlob **blob, ID3DBlob **error_blob )
{
    UINT64 args[8] = { 0 };
    HRESULT hr;

    TRACE( "desc %p, version %#x, blob %p, error_blob %p\n", desc, version,
           blob, error_blob );
    args[0] = (UINT64)(ULONG_PTR)desc;
    args[1] = (UINT)version;
    args[2] = (UINT64)(ULONG_PTR)blob;
    args[3] = (UINT64)(ULONG_PTR)error_blob;
    hr = flat_call( FLAT_D3D12SerializeRootSignature, 4, args, NULL );
    if (!com_runtime_init()) return E_FAIL;
    wrap_fixed_iface( (void **)blob, D3D12_IFACE_ID3D10Blob );
    wrap_fixed_iface( (void **)error_blob, D3D12_IFACE_ID3D10Blob );
    return hr;
}

HRESULT WINAPI D3D12SerializeVersionedRootSignature( const D3D12_VERSIONED_ROOT_SIGNATURE_DESC *desc,
                                                     ID3DBlob **blob, ID3DBlob **error_blob )
{
    UINT64 args[8] = { 0 };
    HRESULT hr;

    TRACE( "desc %p, blob %p, error_blob %p\n", desc, blob, error_blob );
    args[0] = (UINT64)(ULONG_PTR)desc;
    args[1] = (UINT64)(ULONG_PTR)blob;
    args[2] = (UINT64)(ULONG_PTR)error_blob;
    hr = flat_call( FLAT_D3D12SerializeVersionedRootSignature, 3, args, NULL );
    if (!com_runtime_init()) return E_FAIL;
    wrap_fixed_iface( (void **)blob, D3D12_IFACE_ID3D10Blob );
    wrap_fixed_iface( (void **)error_blob, D3D12_IFACE_ID3D10Blob );
    return hr;
}

HRESULT WINAPI D3D12GetInterface( REFCLSID clsid, REFIID riid, void **out )
{
    UINT64 args[8] = { 0 };
    HRESULT hr;

    TRACE( "clsid %s, riid %s, out %p\n", debugstr_guid(clsid),
           debugstr_guid(riid), out );
    args[0] = (UINT64)(ULONG_PTR)clsid;
    args[1] = (UINT64)(ULONG_PTR)riid;
    args[2] = (UINT64)(ULONG_PTR)out;
    hr = flat_call( FLAT_D3D12GetInterface, 3, args, NULL );
    return wrap_out_iface( hr, riid, out );
}

/* ---------------------------------------------------------------- attach */

BOOL WINAPI DllMain( HINSTANCE inst, DWORD reason, void *reserved )
{
    if (reason == DLL_PROCESS_ATTACH)
    {
        LdrDisableThreadCalloutsForDll( inst );
        if (__wine_init_unix_call()) return FALSE;
    }
    return TRUE;
}
