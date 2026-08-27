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

/* ---------------------------------------------------------------- proxies */

struct com_proxy
{
    const void *guest_vtbl;   /* first member: this IS the COM object the
                                 guest sees -- *(void**)proxy is its vtable */
    void       *host;         /* native/host interface pointer */
    LONG        refs;         /* guest-visible refcount, served here */
    UINT        iface;        /* index into wc_surface->ifaces[] */
    struct com_proxy *next;   /* intern-table chain */
    /* The cached answer of this interface's WINECOM_F_CONST_QWORD slot, if it
     * has one.  0 = not yet cached, which is also the "cannot cache" sentinel
     * (see the flag's contract in wine/winecom.h).  Written by the dispatcher
     * after the slot's first real call, read by GUEST code -- the snippet
     * install_const_getters() emits does `mov rax,[rcx+0x20]` -- so the
     * offset is part of the guest ABI and pinned by the C_ASSERT below. */
    UINT64      cached_qword;
    /* The call journal (install_journal): a per-proxy ring the guest-side
     * snippets append hot void-returning command-list calls into instead of
     * trapping, drained IN ORDER at this object's next real trap (or when it
     * crosses as an argument).  jr_pos is written by GUEST code -- one
     * recording thread per command list, by D3D12's own rules -- and reset
     * by the drain, which runs on that same thread inside its trap.  All
     * three offsets are guest ABI, pinned below. */
    BYTE       *jr_base;      /* 0x28: NULL = no journal for this proxy */
    UINT64      jr_pos;       /* 0x30: bytes used, guest-written */
    UINT64      jr_cap;       /* 0x38: bytes available */
    BOOL        jr_draining;  /* re-entrancy fuse for the drain */
};
#ifdef _WIN64
/* the guest ABI pin for the 64-bit lane's snippets; the i386 build of this
 * library lays the struct out differently AND would need i386 snippet
 * encoding, so the fast paths are 64-bit-lane-only -- see
 * install_const_getters / install_journal */
C_ASSERT( offsetof(struct com_proxy, cached_qword) == 0x20 );
C_ASSERT( offsetof(struct com_proxy, jr_base) == 0x28 );
C_ASSERT( offsetof(struct com_proxy, jr_pos)  == 0x30 );
C_ASSERT( offsetof(struct com_proxy, jr_cap)  == 0x38 );
#endif

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

/***********************************************************************
 *           install_const_getters
 *
 * The guest-side fast path for WINECOM_F_CONST_QWORD slots -- the COM lane's
 * analog of spec2thunk's FAST_PATH_EXPORTS, but installed at RUNTIME by
 * rewriting the materialised vtable slot, which works here because winecom
 * owns the vtable it built (the flat lane's exports are IAT-bound and cannot
 * be re-pointed after the fact).
 *
 * Cyberpunk calls ID3D12Resource::GetGPUVirtualAddress 88,016 times a second
 * mid-flythrough [MEASURED 2026-08-27, crossings table] -- a nullary getter of
 * a value that never changes for a given buffer, each call a full trap, a
 * dispatch and an NtCallbackReturn.  After this, only the FIRST call on each
 * object crosses; the dispatcher stashes the answer in the proxy and every
 * later call is served by 10 bytes of guest x86 with no trap at all.
 *
 * Per flagged slot, the vtable entry is re-pointed at this snippet:
 *
 *     48 8b 41 20            mov  rax, [rcx+0x20]   ; proxy->cached_qword
 *     48 85 c0               test rax, rax
 *     74 01                  je   fallback          ; 0 = not cached yet
 *     c3                     ret
 *   fallback:
 *     ff 25 00 00 00 00      jmp  [rip+0]           ; the slot's own trap stub
 *     <8-byte stub address>
 *
 * The fallback jumps to the ORIGINAL stub address, so the trap arrives with
 * the RIP the dispatcher already maps and with RCX untouched (the stub does
 * its own R10 rescue) -- the slow path is byte-identical to a world where
 * this function never ran.  The proxy identity test is untouched too: it
 * matches the vtable BASE addresses, never slot contents.
 *
 * The snippets live in one PAGE_EXECUTE_READWRITE allocation, fully written
 * before any guest thread can see their addresses (the vtable rewrite below
 * is the publication), the same pattern as the callback trampoline pool.
 *
 * WINEEMUNOCOMCONSTGET=1 is the negative control: no snippet is installed,
 * every call keeps trapping, and the crossing counter's row for the slot is
 * what a gate asserts against.  The dispatcher's cache STORE stays on either
 * way; it is harmless without a reader.
 */
static void install_const_getters( void )
{
#ifndef _WIN64
    /* The i386 lane would need its own snippet encoding, its own
     * cached_qword offset AND the 32-bit proxy runtime that does not exist
     * yet (i386-lane-design.md, the crux).  Fail closed: every call keeps
     * trapping, which is the lane's current behaviour for everything. */
    return;
}
#else
    static const UINT snippet_stride = 32;   /* 24 bytes used, 16-aligned */
    unsigned char *block, *code;
    SIZE_T size;
    UINT i, n, count = 0;

    for (i = 0; i < wc_surface->iface_count; i++)
    {
        const struct winecom_iface *itf = &wc_surface->ifaces[i];
        if (!itf->slots) continue;
        for (n = 0; n < itf->slot_count; n++)
            if (itf->slots[n].flags & WINECOM_F_CONST_QWORD) count++;
    }
    if (!count) return;
    if (com_env_flag( L"WINEEMUNOCOMCONSTGET" ))
    {
        ERR( "%s: WINEEMUNOCOMCONSTGET=1 -- %u const-getter slots stay "
             "trapping on every call\n", wc_surface->name, count );
        return;
    }

    block = NULL;
    size = count * snippet_stride;
    if (NtAllocateVirtualMemory( NtCurrentProcess(), (void **)&block, 0, &size,
                                 MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE ))
    {
        ERR( "%s: no memory for %u const-getter snippets; slots stay "
             "trapping\n", wc_surface->name, count );
        return;
    }

    code = block;
    for (i = 0; i < wc_surface->iface_count; i++)
    {
        const struct winecom_iface *itf = &wc_surface->ifaces[i];
        if (!itf->slots) continue;
        for (n = 0; n < itf->slot_count; n++)
        {
            UINT64 *vslot;
            unsigned char *p = code;

            if (!(itf->slots[n].flags & WINECOM_F_CONST_QWORD)) continue;
            vslot = &guest_vtbl_block[iface_slot_base[i] + n];

            *p++ = 0x48; *p++ = 0x8b; *p++ = 0x41;             /* mov rax,[rcx+disp8] */
            *p++ = (unsigned char)offsetof(struct com_proxy, cached_qword);
            *p++ = 0x48; *p++ = 0x85; *p++ = 0xc0;             /* test rax,rax */
            *p++ = 0x74; *p++ = 0x01;                          /* je fallback */
            *p++ = 0xc3;                                       /* ret */
            *p++ = 0xff; *p++ = 0x25;                          /* jmp [rip+0] */
            *p++ = 0x00; *p++ = 0x00; *p++ = 0x00; *p++ = 0x00;
            memcpy( p, vslot, sizeof(UINT64) );                /* the trap stub */

            TRACE( "%s: %s::%s slot %u served guest-side from %p "
                   "(fallback stub %p)\n", wc_surface->name, itf->name,
                   itf->slots[n].name, n, code, *(void **)vslot );
            *vslot = (UINT64)(ULONG_PTR)code;
            code += snippet_stride;
        }
    }
}
#endif  /* _WIN64 */

/***********************************************************************
 *           the call journal
 *
 * [MEASURED 2026-08-27, crossings table] the top of the COM class is a
 * handful of void-returning ID3D12GraphicsCommandList methods -- descriptor
 * table binds, draws, vertex/index buffer binds, pipeline binds -- at
 * ~385,000 crossings a second between them, every one a full trap, dispatch
 * and NtCallbackReturn for a call whose only effect is to append a command
 * to a recording.  So the recording moves guest-side: each journaled slot's
 * vtable entry becomes a snippet that appends {slot, args} to a per-proxy
 * ring, and the ring is REPLAYED IN ORDER through invoke_marshalled() -- the
 * same classifier, narrowing and translate-in the live path uses -- at the
 * object's next real trap.  N crossings become one.
 *
 * WHY THIS IS CORRECT, wall by wall:
 *   ORDER    a command list is recorded by one thread at a time (D3D12's own
 *            rule); everything journaled is list-scoped state, and any
 *            NON-journaled call on the same list (ResourceBarrier, Close,
 *            CopyResource...) traps, and the dispatch drains the ring BEFORE
 *            serving it, so interleavings replay exactly as issued.
 *   EXECUTE  Close is not journaled, so the ring is empty by the time D3D12
 *            allows ExecuteCommandLists; and the classifier drains any
 *            forward proxy crossing as an argument anyway (wc_forward_host),
 *            so even an off-contract execute sees the drained state.
 *   ARGS     everything recorded is by-value, a proxy pointer the drain
 *            unwraps through the ordinary classifier, or a struct COPIED
 *            INTO the record by the snippet (IASetIndexBuffer's 16 bytes,
 *            IASetVertexBuffers' up-to-8 views) -- nothing in the ring
 *            dangles when the app frees its stack frame after the call.
 *   RETURNS  journal-eligible slots return void; RAX after the snippet is
 *            scratch, exactly as after a real void method.
 *   FULL     a full ring falls back to the slot's own trap stub, whose
 *            dispatch drains first -- the slow path is the old path.
 *
 * The slots are CURATED BY NAME below from per-method API knowledge (the
 * same honesty rule as CONST_QWORD_GETTERS), and install refuses a slot
 * whose table row stopped matching the curated shape -- fail closed to
 * trapping, never to a wrong record.  WINEEMUNOCOMJOURNAL=1 is the negative
 * control: no snippets, no rings, every call traps and the crossing counter
 * shows the rows again.
 */

enum journal_shape
{
    JSH_REG1 = 1,   /* 1 arg,  rdx                         rec 16  */
    JSH_REG2,       /* 2 args, rdx r8                      rec 24  */
    JSH_REG3,       /* 3 args, rdx r8 r9                   rec 32  */
    JSH_DRAW4,      /* 4 args, rdx r8 r9 stack             rec 40  */
    JSH_DRAW5,      /* 5 args, rdx r8 r9 stack stack       rec 48  */
    JSH_IBV,        /* ptr -> 16 bytes inline              rec 32  */
    JSH_VBV,        /* start count ptr -> <=8 x 24 inline  rec 224 */
};

static const struct journal_slot_def
{
    const char *name;
    enum journal_shape shape;
    UINT argc;                  /* including `this` -- must match the row */
}
journal_slots[] =
{
    { "ID3D12GraphicsCommandList::SetGraphicsRootDescriptorTable", JSH_REG2, 3 },
    { "ID3D12GraphicsCommandList::SetComputeRootDescriptorTable",  JSH_REG2, 3 },
    { "ID3D12GraphicsCommandList::SetPipelineState",               JSH_REG1, 2 },
    { "ID3D12GraphicsCommandList::SetGraphicsRoot32BitConstant",   JSH_REG3, 4 },
    { "ID3D12GraphicsCommandList::DrawIndexedInstanced",           JSH_DRAW5, 6 },
    { "ID3D12GraphicsCommandList::DrawInstanced",                  JSH_DRAW4, 5 },
    { "ID3D12GraphicsCommandList::IASetIndexBuffer",               JSH_IBV,  2 },
    { "ID3D12GraphicsCommandList::IASetVertexBuffers",             JSH_VBV,  4 },
};

#define JOURNAL_RING_SIZE  0x8000   /* 32K a list; ~150 records of the fattest shape */
#define JOURNAL_MAX_REC    224

static UINT journal_rec_bytes( enum journal_shape shape )
{
    switch (shape)
    {
    case JSH_REG1:  return 16;
    case JSH_REG2:  return 24;
    case JSH_REG3:  return 32;
    case JSH_DRAW4: return 40;
    case JSH_DRAW5: return 48;
    case JSH_IBV:   return 32;
    case JSH_VBV:   return 224;
    }
    return 0;
}

/* which interfaces have at least one journaled slot -- winecom_wrap gives
 * their proxies a ring */
static unsigned char *iface_journaled;
static BOOL journal_on;

static NTSTATUS invoke_marshalled( const struct winecom_iface *itf, const struct winecom_slot *sl,
                                   struct com_proxy *proxy, UINT iface, UINT slot,
                                   const UINT64 *rawargs, UINT64 *rax_out );

/* Replay every record in `p`'s ring, oldest first, then reset the ring.
 * Runs on the recording thread inside its own trap in the ordered cases; the
 * argument-crossing case can be another thread, where D3D12's Close-before-
 * execute rule means an empty ring unless the app is racing itself -- the
 * acquire/release pair keeps even that read coherent. */
static void wc_journal_drain( struct com_proxy *p )
{
    UINT64 pos;
    BYTE *r, *end;

    if (!p->jr_base || p->jr_draining) return;
    pos = __atomic_load_n( &p->jr_pos, __ATOMIC_ACQUIRE );
    if (!pos) return;
    p->jr_draining = TRUE;

    r = p->jr_base;
    end = r + (pos <= p->jr_cap ? pos : 0);   /* a corrupt pos drains nothing */
    if (end == r)
        ERR( "journal pos %I64u beyond cap %I64u on %s proxy %p; dropping the ring\n",
             pos, p->jr_cap, wc_surface->ifaces[p->iface].name, p );

    while (r + 8 <= end)
    {
        UINT key    = *(UINT *)r;
        UINT sizesh = *(UINT *)(r + 4);
        UINT iface  = key >> 16, slot = key & 0xffff;
        UINT bytes  = sizesh & 0xffffff;
        enum journal_shape shape = sizesh >> 24;
        const UINT64 *a = (const UINT64 *)(r + 8);
        UINT64 rawargs[16] = { 0 }, rax;
        const struct winecom_iface *itf;
        const struct winecom_slot *sl;
        NTSTATUS status;

        if (iface >= wc_surface->iface_count || bytes < 16 || bytes > JOURNAL_MAX_REC ||
            r + bytes > end || bytes != journal_rec_bytes( shape ))
        {
            ERR( "corrupt journal record (key %08x size/shape %08x) on proxy %p; "
                 "dropping the rest of the ring\n", key, sizesh, p );
            break;
        }
        itf = &wc_surface->ifaces[iface];
        if (slot >= itf->slot_count || !itf->slots)
        {
            ERR( "journal record names %s slot %u which the table does not have; "
                 "dropping the rest of the ring\n", itf->name, slot );
            break;
        }
        sl = &itf->slots[slot];

        switch (shape)
        {
        case JSH_REG1:  rawargs[1] = a[0]; break;
        case JSH_REG2:  rawargs[1] = a[0]; rawargs[2] = a[1]; break;
        case JSH_REG3:  rawargs[1] = a[0]; rawargs[2] = a[1]; rawargs[3] = a[2]; break;
        case JSH_DRAW4: rawargs[1] = a[0]; rawargs[2] = a[1]; rawargs[3] = a[2];
                        rawargs[4] = a[3]; break;
        case JSH_DRAW5: rawargs[1] = a[0]; rawargs[2] = a[1]; rawargs[3] = a[2];
                        rawargs[4] = a[3]; rawargs[5] = a[4]; break;
        case JSH_IBV:
            /* a[0] is the guest's original pointer, kept for its NULLness;
             * the bytes it pointed at live in the record */
            rawargs[1] = a[0] ? (UINT64)(ULONG_PTR)(r + 16) : 0;
            break;
        case JSH_VBV:
            rawargs[1] = a[0];
            rawargs[2] = a[1];
            rawargs[3] = a[2] ? (UINT64)(ULONG_PTR)(r + 32) : 0;
            break;
        }

        status = invoke_marshalled( itf, sl, p, iface, slot, rawargs, &rax );
        if (status)
            ERR( "journal replay of %s failed, status %08x; continuing\n",
                 sl->name, (UINT)status );
        r += bytes;
    }

    __atomic_store_n( &p->jr_pos, 0, __ATOMIC_RELEASE );
    p->jr_draining = FALSE;
}

#ifdef _WIN64
/***********************************************************************
 *           install_journal
 *
 * Emit the guest-side recording snippets and point the journaled slots'
 * vtable entries at them.  Byte encodings llvm-mc-verified (the source .s is
 * in the commit message's session); every snippet is:
 *
 *     mov  rax, [rcx+0x28]        ; ring base; NULL -> fallback
 *     test rax, rax ; jz fb
 *     mov  r10, [rcx+0x30]        ; pos
 *     lea  r11, [r10+REC]
 *     cmp  r11, [rcx+0x38] ; ja fb
 *     add  rax, r10
 *     mov  dword [rax], KEY       ; (iface<<16)|slot
 *     mov  dword [rax+4], REC|SHAPE<<24
 *     ... shape stores ...
 *     mov  [rcx+0x30], r11 ; ret
 * fb: jmp [rip+0] ; .quad trap_stub
 */
struct snippet_buf
{
    BYTE *p;
    BYTE *fixups[8];   /* rel8 sites that must land on the fallback */
    UINT nfix;
};

static void sb_emit( struct snippet_buf *b, const void *bytes, UINT n )
{
    memcpy( b->p, bytes, n );
    b->p += n;
}

static void sb_emit_jcc_fb( struct snippet_buf *b, BYTE opc )
{
    BYTE ins[2] = { opc, 0 };
    b->fixups[b->nfix++] = b->p + 1;
    sb_emit( b, ins, 2 );
}

static void install_journal( void )
{
    static const BYTE pre1[] = { 0x48, 0x8b, 0x41, 0x28,       /* mov rax,[rcx+0x28] */
                                 0x48, 0x85, 0xc0 };           /* test rax,rax */
    static const BYTE pre2[] = { 0x4c, 0x8b, 0x51, 0x30 };     /* mov r10,[rcx+0x30] */
    static const BYTE pre4[] = { 0x4c, 0x3b, 0x59, 0x38 };     /* cmp r11,[rcx+0x38] */
    static const BYTE pre6[] = { 0x4c, 0x01, 0xd0 };           /* add rax,r10 */
    static const BYTE st_rdx[]  = { 0x48, 0x89, 0x50, 0x08 };  /* mov [rax+8],rdx */
    static const BYTE st_r8[]   = { 0x4c, 0x89, 0x40, 0x10 };  /* mov [rax+16],r8 */
    static const BYTE st_r9[]   = { 0x4c, 0x89, 0x48, 0x18 };  /* mov [rax+24],r9 */
    static const BYTE st_stk4[] = { 0x4c, 0x8b, 0x54, 0x24, 0x28,   /* mov r10,[rsp+0x28] */
                                    0x4c, 0x89, 0x50, 0x20 };       /* mov [rax+0x20],r10 */
    static const BYTE st_stk5[] = { 0x4c, 0x8b, 0x54, 0x24, 0x30,   /* mov r10,[rsp+0x30] */
                                    0x4c, 0x89, 0x50, 0x28 };       /* mov [rax+0x28],r10 */
    static const BYTE ibv_cp[]  = { 0x48, 0x85, 0xd2,               /* test rdx,rdx */
                                    0x74, 0x0f,                     /* jz +15 (over the copy) */
                                    0x4c, 0x8b, 0x12,               /* mov r10,[rdx] */
                                    0x4c, 0x89, 0x50, 0x10,         /* mov [rax+0x10],r10 */
                                    0x4c, 0x8b, 0x52, 0x08,         /* mov r10,[rdx+8] */
                                    0x4c, 0x89, 0x50, 0x18 };       /* mov [rax+0x18],r10 */
    static const BYTE vbv_guard[] = { 0x49, 0x83, 0xf8, 0x08 };     /* cmp r8,8 */
    /* The copy loop's data temp is R8, NOT r11: r11 carries the new ring
     * position into the epilogue, and the first cut of this loop used it as
     * the temp -- so jr_pos got the last copied qword, a vkd3d GPU VA, and
     * the drain dropped every ring as corrupt [MEASURED 2026-08-27, the
     * first journal leg: "journal pos 702...  beyond cap"].  R8 held
     * NumViews, which is already both stored in the record and folded into
     * rdx as the qword count by the time the loop runs. */
    static const BYTE vbv_cp[]  = { 0x4d, 0x85, 0xc9,               /* test r9,r9 */
                                    0x74, 0x1d,                     /* jz +29 (over the loop) */
                                    0x4c, 0x89, 0xc2,               /* mov rdx,r8 */
                                    0x48, 0x8d, 0x14, 0x52,         /* lea rdx,[rdx+rdx*2] */
                                    0x45, 0x31, 0xd2,               /* xor r10d,r10d */
                                    /* loop: */
                                    0x49, 0x39, 0xd2,               /* cmp r10,rdx */
                                    0x73, 0x0e,                     /* jae +14 (out of the loop) */
                                    0x4f, 0x8b, 0x04, 0xd1,         /* mov r8,[r9+r10*8] */
                                    0x4e, 0x89, 0x44, 0xd0, 0x20,   /* mov [rax+r10*8+0x20],r8 */
                                    0x49, 0xff, 0xc2,               /* inc r10 */
                                    0xeb, 0xed };                   /* jmp loop (-19) */
    static const BYTE epi[] = { 0x4c, 0x89, 0x59, 0x30,        /* mov [rcx+0x30],r11 */
                                0xc3 };                        /* ret */
    static const BYTE fb[]  = { 0xff, 0x25, 0x00, 0x00, 0x00, 0x00 };  /* jmp [rip+0] */

    static const UINT snippet_stride = 160;   /* the fattest (VBV) is ~120 */
    unsigned char *block, *code;
    SIZE_T size;
    UINT i, n, j, k, count = 0;

    if (!(iface_journaled = RtlAllocateHeap( NtCurrentTeb()->Peb->ProcessHeap,
                                             HEAP_ZERO_MEMORY, wc_surface->iface_count )))
        return;

    for (i = 0; i < wc_surface->iface_count; i++)
    {
        const struct winecom_iface *itf = &wc_surface->ifaces[i];
        if (!itf->slots) continue;
        for (n = 0; n < itf->slot_count; n++)
            for (j = 0; j < ARRAYSIZE(journal_slots); j++)
                if (itf->slots[n].name && !strcmp( itf->slots[n].name, journal_slots[j].name ))
                    count++;
    }
    if (!count) return;
    if (com_env_flag( L"WINEEMUNOCOMJOURNAL" ))
    {
        ERR( "WINEEMUNOCOMJOURNAL=1 -- %u journal slots stay trapping on every call\n", count );
        return;
    }

    block = NULL;
    size = count * snippet_stride;
    if (NtAllocateVirtualMemory( NtCurrentProcess(), (void **)&block, 0, &size,
                                 MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE ))
    {
        ERR( "no memory for %u journal snippets; slots stay trapping\n", count );
        return;
    }

    code = block;
    for (i = 0; i < wc_surface->iface_count; i++)
    {
        const struct winecom_iface *itf = &wc_surface->ifaces[i];
        if (!itf->slots) continue;
        for (n = 0; n < itf->slot_count; n++)
        {
            const struct journal_slot_def *def = NULL;
            const struct winecom_slot *sl = &itf->slots[n];
            struct snippet_buf b = { code };
            UINT64 *vslot;
            UINT rec;
            BYTE tmp[16];

            for (j = 0; j < ARRAYSIZE(journal_slots); j++)
                if (sl->name && !strcmp( sl->name, journal_slots[j].name ))
                { def = &journal_slots[j]; break; }
            if (!def) continue;

            /* fail closed to trapping if the table row stopped matching the
             * curated shape -- a drifted roster must never produce a wrong
             * record */
            if (sl->refuse || (sl->flags & WINECOM_F_HAND) ||
                !(sl->flags & WINECOM_F_RET_VOID) || sl->argc != def->argc)
            {
                ERR( "journal slot %s does not match its curated shape "
                     "(argc %u, flags %x); it stays trapping\n",
                     sl->name, sl->argc, sl->flags );
                continue;
            }

            rec = journal_rec_bytes( def->shape );
            vslot = &guest_vtbl_block[iface_slot_base[i] + n];

            sb_emit( &b, pre1, sizeof(pre1) );
            sb_emit_jcc_fb( &b, 0x74 );                   /* jz fb */
            sb_emit( &b, pre2, sizeof(pre2) );
            tmp[0] = 0x4d; tmp[1] = 0x8d; tmp[2] = 0x9a;  /* lea r11,[r10+rec] */
            *(UINT *)(tmp + 3) = rec;
            sb_emit( &b, tmp, 7 );
            sb_emit( &b, pre4, sizeof(pre4) );
            sb_emit_jcc_fb( &b, 0x77 );                   /* ja fb */
            if (def->shape == JSH_VBV)
            {
                sb_emit( &b, vbv_guard, sizeof(vbv_guard) );
                sb_emit_jcc_fb( &b, 0x77 );               /* ja fb: count > 8 */
            }
            sb_emit( &b, pre6, sizeof(pre6) );
            tmp[0] = 0xc7; tmp[1] = 0x00;                 /* mov dword [rax],key */
            *(UINT *)(tmp + 2) = (i << 16) | n;
            sb_emit( &b, tmp, 6 );
            tmp[0] = 0xc7; tmp[1] = 0x40; tmp[2] = 0x04;  /* mov dword [rax+4],rec|shape */
            *(UINT *)(tmp + 3) = rec | ((UINT)def->shape << 24);
            sb_emit( &b, tmp, 7 );

            switch (def->shape)
            {
            case JSH_REG1:
                sb_emit( &b, st_rdx, sizeof(st_rdx) );
                break;
            case JSH_REG2:
                sb_emit( &b, st_rdx, sizeof(st_rdx) );
                sb_emit( &b, st_r8, sizeof(st_r8) );
                break;
            case JSH_REG3:
                sb_emit( &b, st_rdx, sizeof(st_rdx) );
                sb_emit( &b, st_r8, sizeof(st_r8) );
                sb_emit( &b, st_r9, sizeof(st_r9) );
                break;
            case JSH_DRAW4:
                sb_emit( &b, st_rdx, sizeof(st_rdx) );
                sb_emit( &b, st_r8, sizeof(st_r8) );
                sb_emit( &b, st_r9, sizeof(st_r9) );
                sb_emit( &b, st_stk4, sizeof(st_stk4) );
                break;
            case JSH_DRAW5:
                sb_emit( &b, st_rdx, sizeof(st_rdx) );
                sb_emit( &b, st_r8, sizeof(st_r8) );
                sb_emit( &b, st_r9, sizeof(st_r9) );
                sb_emit( &b, st_stk4, sizeof(st_stk4) );
                sb_emit( &b, st_stk5, sizeof(st_stk5) );
                break;
            case JSH_IBV:
                sb_emit( &b, st_rdx, sizeof(st_rdx) );
                sb_emit( &b, ibv_cp, sizeof(ibv_cp) );
                break;
            case JSH_VBV:
                sb_emit( &b, st_rdx, sizeof(st_rdx) );
                sb_emit( &b, st_r8, sizeof(st_r8) );
                sb_emit( &b, st_r9, sizeof(st_r9) );
                sb_emit( &b, vbv_cp, sizeof(vbv_cp) );
                break;
            }
            sb_emit( &b, epi, sizeof(epi) );

            /* the fallback: every recorded rel8 jump lands here */
            for (k = 0; k < b.nfix; k++)
            {
                LONG_PTR d = b.p - (b.fixups[k] + 1);
                *b.fixups[k] = (BYTE)d;
            }
            sb_emit( &b, fb, sizeof(fb) );
            memcpy( b.p, vslot, sizeof(UINT64) );         /* the trap stub */
            b.p += 8;

            TRACE( "journal: %s slot %u recorded guest-side from %p "
                   "(shape %u rec %u, fallback stub %p)\n",
                   sl->name, n, code, def->shape, rec, *(void **)vslot );
            *vslot = (UINT64)(ULONG_PTR)code;
            iface_journaled[i] = 1;
            code += snippet_stride;
        }
    }
    journal_on = TRUE;
}
#else
static void install_journal( void ) { }
#endif  /* _WIN64 */

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
    install_const_getters();
    install_journal();
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
    p->cached_qword = 0;   /* the heap does not zero, and 0 is the sentinel */
    p->jr_base = NULL;
    p->jr_pos = 0;
    p->jr_cap = 0;
    p->jr_draining = FALSE;
    if (journal_on && iface_journaled[iface])
    {
        /* one recording ring per journaled object; a failed allocation just
         * leaves the snippets on their fallback path -- every call traps,
         * which is the old world */
        SIZE_T ring = JOURNAL_RING_SIZE;
        void *mem = NULL;
        if (!NtAllocateVirtualMemory( NtCurrentProcess(), &mem, 0, &ring,
                                      MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE ))
        {
            p->jr_base = mem;
            p->jr_cap = JOURNAL_RING_SIZE;
        }
    }
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
    /* An object crossing as an ARGUMENT (ExecuteCommandLists, ExecuteBundle)
     * must be current before the callee sees it.  D3D12's Close-before-
     * execute rule means this normally finds an empty ring -- Close trapped
     * and drained -- so this is the belt to that braces. */
    wc_journal_drain( p );
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
        if (p->jr_base)
        {
            /* whatever is still in the ring dies with the object: a released
             * command list's unreplayed records could only ever have fed a
             * recording nobody can execute any more */
            SIZE_T ring = 0;
            void *mem = p->jr_base;
            NtFreeVirtualMemory( NtCurrentProcess(), &mem, &ring, MEM_RELEASE );
        }
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

/***********************************************************************
 *           invoke_marshalled
 *
 * The marshalling core of winecom_dispatch, over an argument ARRAY instead
 * of a trap CONTEXT: rawargs[i] is argument i exactly as
 * winecom_read_arg(ctx, i) would have produced it (index 0, `this`, is
 * ignored -- the proxy parameter is the authority).  Split out so the
 * journal drain can replay recorded calls through the SAME classifier,
 * narrowing, translate-in and refusal logic the live trap path uses -- two
 * marshallers would be two places for the next dwordmask bug to hide.
 * On STATUS_SUCCESS *rax_out is what RAX must carry back to the guest.
 */
static NTSTATUS invoke_marshalled( const struct winecom_iface *itf, const struct winecom_slot *sl,
                                   struct com_proxy *proxy, UINT iface, UINT slot,
                                   const UINT64 *rawargs, UINT64 *rax_out )
{
    UINT64 args[16] = { 0 };
    UINT64 arr_buf[32];
    UINT64 *arr_heap = NULL;
    UINT64 ret;
    UINT i, n, ppv_idx = 0, riid_idx = 0;
    UINT out_static_idx[8], n_out_static = 0;
    UINT out_arr_idx[8], n_out_arr = 0;
    UINT borrowed[16], n_borrowed = 0;
    const UINT64 *arr_borrowed = NULL;
    UINT n_arr_borrowed = 0;
    BOOL have_ppv = FALSE;

    args[0] = (UINT64)(ULONG_PTR)proxy->host;
    for (i = 1; i < sl->argc; i++)
    {
        UINT64 raw = rawargs[i];
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
                *rax_out = (UINT)E_NOTIMPL;
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
                *rax_out = (UINT)E_NOTIMPL;
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
                *rax_out = (UINT)E_NOTIMPL;
                return STATUS_SUCCESS;
            }
            args[i] = 0;
            break;
        case WINECOM_CA_IFACE_ARR_IN:
        {
            UINT count = (UINT)rawargs[sl->aux2 + 1];
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
                *rax_out = (UINT)E_OUTOFMEMORY;
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
                    *rax_out = (UINT)E_NOTIMPL;
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
            *rax_out = (UINT)E_NOTIMPL;
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
        UINT count = (UINT)rawargs[sl->caux[idx - 1] + 1];
        UINT k;

        for (k = 0; k < count; k++)
            if (out[k]) out[k] = winecom_wrap( out[k], sl->xaux[idx - 1] );
    }

    if (sl->flags & WINECOM_F_RET_VIA_ARG)
        *rax_out = args[1];   /* == the callee's return value */
    else if (sl->flags & WINECOM_F_RET_VOID)
        *rax_out = 0;
    else
        *rax_out = ret;

    /* The other half of install_const_getters(): the first real call caches
     * its answer in the proxy, and the guest-side snippet serves every later
     * one.  A plain aligned 8-byte store -- the value is immutable by the
     * flag's contract, so a racing second first-call stores the same bytes,
     * and a guest thread that reads it mid-publication reads either 0 (and
     * traps, correctly) or the value. */
    if (sl->flags & WINECOM_F_CONST_QWORD)
        proxy->cached_qword = ret;

    return STATUS_SUCCESS;
}

NTSTATUS winecom_dispatch( UINT iface, UINT slot, AMD64_CONTEXT *ctx )
{
    const struct winecom_iface *itf;
    const struct winecom_slot *sl;
    struct com_proxy *proxy;
    UINT i;

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

    /* Everything the guest journaled on this object happened BEFORE the call
     * that is trapping now: replay it first, whatever this call is -- a
     * non-journaled method, a journaled one whose ring filled, Close, or
     * Release.  This is what makes the recorded order the issued order. */
    wc_journal_drain( proxy );

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

    {
        UINT64 rawargs[16] = { 0 }, rax = 0;
        NTSTATUS st;

        for (i = 1; i < sl->argc && i < 16; i++) rawargs[i] = winecom_read_arg( ctx, i );
        st = invoke_marshalled( itf, sl, proxy, iface, slot, rawargs, &rax );
        if (st == STATUS_SUCCESS) ctx->Rax = rax;
        return st;
    }
}
