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
#include "wine/exception.h"
#include "wine/winecom.h"

#include "winecom_private.h"
#include "winecom_waves.h"
#include "wine/emu_qpc.h"

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

/* ------------------------------------------------------- the 32-bit guest
 *
 * ONE RUNTIME, KEYED BY GUEST MACHINE AT ATTACH.  A process has exactly one
 * guest machine for its whole life -- an i386 guest is a WoW64 process, an
 * x86-64 guest is not -- so the "one proxy runtime parameterised by width or
 * a second instantiation" question (i386-lane-design.md, the crux) resolves
 * to a per-process constant, not a per-object one.  What changes under
 * `guest32`:
 *
 *   THE PROXY OBJECT.  `struct com_proxy` is unchanged.  Its first member is
 *   still the 8-byte guest_vtbl field -- but the proxy is allocated BELOW
 *   4 GiB and the vtable it points at is below 4 GiB, so on this
 *   little-endian host the guest's 4-byte load at offset 0 reads exactly the
 *   low half of the same bytes the native side reads as 8: one struct, two
 *   correct views.  The layout pins (cached_qword at 0x20...) are 64-bit
 *   guest ABI and the fast paths that rely on them do not install here.
 *
 *   THE VTABLE.  A 32-bit vtable is an array of 4-BYTE slot entries, so the
 *   i386 module's stub addresses are materialised into a separate UINT
 *   block (below 4 GiB, since the proxies' 4-byte vtable pointers must reach
 *   it) rather than the UINT64 block the 64-bit lane builds.
 *
 *   THE MODULE.  The guest thunk module lives in the 32-BIT loader list (the
 *   i386 loader is the guest's own ntdll32; these modules never appear in
 *   the 64-bit PEB), and its exports are parsed straight out of the PE32
 *   export directory -- LdrGetProcedureAddress only serves the 64-bit list.
 *
 *   DISPATCH.  Arguments arrive in 4-byte stdcall stack slots and the CALLEE
 *   pops them, so serving a call requires the marshal table's I386_GEOM
 *   geometry -- see winecom_dispatch32 at the bottom of this file.  Rows
 *   without it are refused; rows without even the pop arithmetic fault.
 */
static BOOL guest32;

/* Guest-legal allocations: the proxies and the 32-bit vtable block must be
 * addressable by 4-byte guest pointers.  A trivial carve-out allocator over
 * NtAllocateVirtualMemory's zero_bits form, plus a free list for the one
 * fixed size that ever comes back (a proxy).  wc_cs guards both. */
static void *low_chunk;
static SIZE_T low_used, low_cap;
static void *low_free_proxies;   /* singly linked through the first word */

static void *wc_alloc_low( SIZE_T size )
{
    void *ret;

    size = (size + 15) & ~(SIZE_T)15;
    if (!low_chunk || low_used + size > low_cap)
    {
        SIZE_T cap = size > 0x10000 ? (size + 0xffff) & ~(SIZE_T)0xffff : 0x10000;
        void *mem = NULL;

        if (NtAllocateVirtualMemory( NtCurrentProcess(), &mem, 0x7fffffff, &cap,
                                     MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE ))
            return NULL;
        low_chunk = mem;    /* a part-used old chunk leaks its tail: bounded
                               by chunk count, and proxies recycle below */
        low_used = 0;
        low_cap = cap;
    }
    ret = (char *)low_chunk + low_used;
    low_used += size;
    return ret;
}

static struct com_proxy *wc_proxy_alloc(void)   /* wc_cs held */
{
    struct com_proxy *p;

    if (!guest32)
        return RtlAllocateHeap( NtCurrentTeb()->Peb->ProcessHeap, 0, sizeof(*p) );
    if ((p = low_free_proxies))
    {
        low_free_proxies = *(void **)p;
        return p;
    }
    return wc_alloc_low( sizeof(*p) );
}

static void wc_proxy_free( struct com_proxy *p )
{
    if (!guest32)
    {
        RtlFreeHeap( NtCurrentTeb()->Peb->ProcessHeap, 0, p );
        return;
    }
    RtlEnterCriticalSection( &wc_cs );
    *(void **)p = low_free_proxies;
    low_free_proxies = p;
    RtlLeaveCriticalSection( &wc_cs );
}

/* The 32-bit guest vtables, when guest32: vtbl32s[i] points into vtbl32_block,
 * whose entries are the i386 stub addresses as 4-byte words. */
static UINT **vtbl32s;
static UINT *vtbl32_block;

/* hand32_map[iface_slot_base[i] + n]: index into wc_surface->hand32, or 0xff */
static unsigned char *hand32_map;

/* refuse32/no-geometry log-once flags share refuse_logged */

/* Export lookup on a mapped PE32 image the 64-bit loader has never seen.
 * The export directory is machine-independent; forwarders are not followed
 * (every name asked for is a data table the module defines itself). */
static void *find_export32( ULONG_PTR base, const char *name )
{
    const IMAGE_EXPORT_DIRECTORY *exp;
    const UINT *names, *funcs;
    const USHORT *ords;
    ULONG size;
    UINT i;

    exp = RtlImageDirectoryEntryToData( (HMODULE)base, TRUE,
                                        IMAGE_DIRECTORY_ENTRY_EXPORT, &size );
    if (!exp || !exp->NumberOfNames) return NULL;
    names = (const UINT *)(base + exp->AddressOfNames);
    ords  = (const USHORT *)(base + exp->AddressOfNameOrdinals);
    funcs = (const UINT *)(base + exp->AddressOfFunctions);
    for (i = 0; i < exp->NumberOfNames; i++)
        if (!strcmp( (const char *)(base + names[i]), name ))
        {
            if (ords[i] >= exp->NumberOfFunctions) return NULL;
            return (void *)(base + funcs[ords[i]]);
        }
    return NULL;
}

struct wc_ldr_entry32
{
    LIST_ENTRY32     InLoadOrderLinks;
    LIST_ENTRY32     InMemoryOrderLinks;
    LIST_ENTRY32     InInitializationOrderLinks;
    ULONG            DllBase;
    ULONG            EntryPoint;
    ULONG            SizeOfImage;
    UNICODE_STRING32 FullDllName;
    UNICODE_STRING32 BaseDllName;
};

/* The 32-bit loader list walk: same question as find_guest_module below,
 * asked of the guest ntdll32's own PEB.  The list is guarded by the GUEST
 * loader's own lock, which no native thread can take, so a guest thread
 * splicing it mid-walk can hand this walk a wild link -- the walk runs
 * under __TRY and retries, the same protection ntdll's own 32-bit resolver
 * carries (see find_guest_thunk_target32's banner and the measured Dex
 * hang that motivated it). */
static HMODULE find_guest_module32_walk( const WCHAR *const *names, UINT count )
{
    PEB_LDR_DATA32 *ldr;
    LIST_ENTRY32 *mark;
    TEB32 *teb32;
    PEB32 *peb32;
    HMODULE ret = NULL;
    ULONG e32;
    UINT i;

    if (!NtCurrentTeb()->WowTebOffset) return NULL;
    teb32 = (TEB32 *)((char *)NtCurrentTeb() + NtCurrentTeb()->WowTebOffset);
    if (!(peb32 = (PEB32 *)(ULONG_PTR)teb32->Peb) ||
        !(ldr = (PEB_LDR_DATA32 *)(ULONG_PTR)peb32->LdrData))
        return NULL;

    mark = &ldr->InMemoryOrderModuleList;
    for (e32 = mark->Flink; e32 != (ULONG)(ULONG_PTR)mark && !ret;
         e32 = ((LIST_ENTRY32 *)(ULONG_PTR)e32)->Flink)
    {
        struct wc_ldr_entry32 *mod = CONTAINING_RECORD( (LIST_ENTRY32 *)(ULONG_PTR)e32,
                                                        struct wc_ldr_entry32,
                                                        InMemoryOrderLinks );
        const IMAGE_NT_HEADERS *nt = RtlImageNtHeader( (HMODULE)(ULONG_PTR)mod->DllBase );
        UNICODE_STRING want, have;

        if (!nt || nt->FileHeader.Machine != IMAGE_FILE_MACHINE_I386) continue;
        have.Buffer = (WCHAR *)(ULONG_PTR)mod->BaseDllName.Buffer;
        have.Length = mod->BaseDllName.Length;
        have.MaximumLength = mod->BaseDllName.MaximumLength;
        for (i = 0; i < count; i++)
        {
            RtlInitUnicodeString( &want, names[i] );
            if (RtlEqualUnicodeString( &want, &have, TRUE ))
            {
                /* An application can ship its OWN d3d11.dll beside the .exe
                 * (Dex does), and it loads under the same base name as this
                 * port's thunk module.  A namesake that publishes no
                 * __wine_com_thunk_info is such a copy, not a drifted
                 * roster: skip it and keep walking, so the port's own
                 * module -- which the guest also loaded, from the system
                 * directory -- is the one materialised from. */
                if (!find_export32( mod->DllBase, "__wine_com_thunk_info" ))
                {
                    TRACE( "skipping %s at %p: no __wine_com_thunk_info "
                           "(an application's own copy)\n",
                           debugstr_wn( have.Buffer, have.Length / sizeof(WCHAR) ),
                           (void *)(ULONG_PTR)mod->DllBase );
                    continue;
                }
                ret = (HMODULE)(ULONG_PTR)mod->DllBase;
                break;
            }
        }
    }
    return ret;
}

static HMODULE find_guest_module32( const WCHAR *const *names, UINT count )
{
    HMODULE ret = NULL;
    ULONG_PTR magic;
    UINT attempt;
    BOOL torn;

    for (attempt = 0; attempt < 16; attempt++)
    {
        torn = FALSE;
        LdrLockLoaderLock( 0, NULL, &magic );
        __TRY
        {
            ret = find_guest_module32_walk( names, count );
        }
        __EXCEPT_ALL
        {
            torn = TRUE;
        }
        __ENDTRY
        LdrUnlockLoaderLock( 0, magic );
        if (!torn) return ret;
        NtYieldExecution();
    }
    ERR( "the 32-bit loader-list walk kept faulting; reporting no module\n" );
    return NULL;
}

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

/* ------------------------------------------------ the wave kill switches --
 *
 * WHY THESE EXIST.  The completeness landings 74591109c3f..c199f79caf9 turned
 * hundreds of refused COM rows into served ones in one stretch, and shipped no
 * way to put any of them back.  When the Witcher 3 stopped loading afterwards,
 * bisecting that stretch cost SEVEN seat runs, each one a swap of built PE
 * halves in and out of a tree (ppc64le/docs/sessions/2026-09-01/
 * w3-load-regression-bisect.md, whose closing lesson is this file's whole
 * reason).  Three levers, all read once at attach, all making a served row
 * behave EXACTLY as a generation-refused one did:
 *
 *   WINEEMUNOCOMROWS   comma-separated `Iface::Slot` names, or `@/path/file`
 *                      with one name per line (# comments allowed).  Each
 *                      named row takes the generated-refusal path: refuse
 *                      once by name, E_NOTIMPL, and scrub_refused_outs() so
 *                      the refusal is INERT.  A refusal that is merely loud
 *                      is the crash class this runtime already learned about.
 *   WINEEMUNOCOMIIDS   comma-separated IIDs, `{xxxxxxxx-....}` or a bare
 *                      leading 8 hex digits.  A listed IID is treated as
 *                      UNROSTERED where interfaces are handed out, which is
 *                      the release-and-NULL + E_NOINTERFACE the guest got for
 *                      months before the syscom wave rostered it.
 *   WINEEMUNOCOMWAVE   comma-separated wave names from winecom_waves.h --
 *                      whole landings expanded to the row and IID sets that
 *                      ppc64le/winecom/derive-wave-rows.py derived FROM GIT.
 *                      One environment variable per theory leg.
 *
 * THE HOT PATH PAYS NOTHING WHEN THEY ARE UNSET.  The row lever's result is a
 * per-(iface,slot) byte array that is only ALLOCATED when a lever named
 * something, so the unarmed test is `forced_refuse == NULL` -- one load and a
 * predictable branch, no environment read, no string work.  The IID lever is
 * a count that stays 0.  Resolution happens ONCE, at attach, against the
 * surface's own const tables, which are never written.
 *
 * A NAME THAT MATCHES NOTHING IS LOUD.  A typo in a bisect leg that passed
 * silently would be recorded as "tested, clean" and would take the leg's
 * conclusion with it.  Every target that matched no row on any attached
 * surface gets one ERR naming it. */

static char **row_targets;             /* "Iface::Slot", NUL-terminated */
static unsigned char *row_target_hit;  /* per target: matched at least one row */
static UINT row_target_count;
static unsigned char *forced_refuse;   /* per (iface, slot); NULL = UNARMED,
                                          and that NULL is the hot-path test */

/* A blocked IID.  `data1_only` is the bare 8-hex-digit spelling the port's own
 * notes use -- "{77aa99a0} IAudioSessionManager2" -- which is enough to name
 * an interface uniquely in practice and is what a reader has in front of them
 * when they reach for this lever. */
struct blocked_iid
{
    GUID guid;
    BOOL data1_only;
};
static struct blocked_iid *blocked_iids;
static UINT blocked_iid_count;         /* 0 = UNARMED */

static const char forced_why[] =
    "forced by WINEEMUNOCOMROWS/WINEEMUNOCOMWAVE (a served row put back to its "
    "pre-landing refusal for a bisect leg)";

/* An environment variable's VALUE, ASCII, heap-allocated, or NULL.  PE-side,
 * so no getenv: RtlQueryEnvironmentVariable_U writes UTF-16 into a caller
 * buffer whose MaximumLength is a USHORT, which caps a value at 32767
 * characters -- ample for a name list, and a `@file` carries the long ones. */
static char *com_env_str( const WCHAR *name )
{
    UNICODE_STRING nameW, value;
    ULONG chars = 512;
    WCHAR *buf = NULL;
    char *ret;
    NTSTATUS st;
    UINT i, n;

    RtlInitUnicodeString( &nameW, name );
    for (;;)
    {
        if (!(buf = RtlAllocateHeap( NtCurrentTeb()->Peb->ProcessHeap, 0,
                                     chars * sizeof(WCHAR) ))) return NULL;
        value.Buffer = buf;
        value.MaximumLength = (USHORT)(chars * sizeof(WCHAR));
        value.Length = 0;
        st = RtlQueryEnvironmentVariable_U( NULL, &nameW, &value );
        if (st != STATUS_BUFFER_TOO_SMALL || chars >= 0x7fff) break;
        RtlFreeHeap( NtCurrentTeb()->Peb->ProcessHeap, 0, buf );
        chars = chars * 4 < 0x7fff ? chars * 4 : 0x7fff;
    }
    n = value.Length / sizeof(WCHAR);
    if (st || !n)
    {
        RtlFreeHeap( NtCurrentTeb()->Peb->ProcessHeap, 0, buf );
        return NULL;
    }
    if ((ret = RtlAllocateHeap( NtCurrentTeb()->Peb->ProcessHeap, 0, n + 1 )))
    {
        /* every spelling these levers accept -- C++ identifiers, GUIDs,
         * paths -- is ASCII; anything else is a typo, and '?' makes it one
         * the unmatched-target report can print */
        for (i = 0; i < n; i++) ret[i] = buf[i] < 0x80 ? (char)buf[i] : '?';
        ret[n] = 0;
    }
    RtlFreeHeap( NtCurrentTeb()->Peb->ProcessHeap, 0, buf );
    return ret;
}

/* `@path` -- the whole file as one ASCII buffer, or NULL.  A UNIX absolute
 * path is spelled through the Z: drive rather than through a unix-side
 * conversion this side has no access to: `/home/x/rows.list` is
 * `Z:\home\x\rows.list`, which is what every Wine prefix maps it to and what
 * the caller would have to type otherwise. */
static char *com_read_list_file( const char *path )
{
    UNICODE_STRING ntpath;
    OBJECT_ATTRIBUTES attr;
    IO_STATUS_BLOCK io;
    FILE_STANDARD_INFORMATION info;
    WCHAR dos[MAX_PATH + 4];
    HANDLE handle;
    char *data = NULL;
    UINT i, n = 0;
    LARGE_INTEGER off;

    if (path[0] == '/') { dos[n++] = 'Z'; dos[n++] = ':'; }
    for (i = 0; path[i] && n < ARRAYSIZE(dos) - 1; i++)
        dos[n++] = path[i] == '/' ? '\\' : (WCHAR)(unsigned char)path[i];
    dos[n] = 0;
    if (path[i])
    {
        ERR( "the COM row-list path %s is longer than MAX_PATH; ignoring it\n", path );
        return NULL;
    }
    if (!RtlDosPathNameToNtPathName_U( dos, &ntpath, NULL, NULL ))
    {
        ERR( "the COM row-list path %s is not a path this side can open\n", path );
        return NULL;
    }
    InitializeObjectAttributes( &attr, &ntpath, OBJ_CASE_INSENSITIVE, NULL, NULL );
    if (NtOpenFile( &handle, GENERIC_READ | SYNCHRONIZE, &attr, &io,
                    FILE_SHARE_READ, FILE_SYNCHRONOUS_IO_NONALERT ))
    {
        ERR( "cannot open the COM row list %s; NO rows are forced -- do not "
             "read this leg as 'tested clean'\n", path );
        RtlFreeUnicodeString( &ntpath );
        return NULL;
    }
    RtlFreeUnicodeString( &ntpath );
    if (!NtQueryInformationFile( handle, &io, &info, sizeof(info),
                                 FileStandardInformation ) &&
        info.EndOfFile.QuadPart > 0 && info.EndOfFile.QuadPart < 0x100000 &&
        (data = RtlAllocateHeap( NtCurrentTeb()->Peb->ProcessHeap, 0,
                                 (SIZE_T)info.EndOfFile.QuadPart + 1 )))
    {
        off.QuadPart = 0;
        if (NtReadFile( handle, NULL, NULL, NULL, &io, data,
                        (ULONG)info.EndOfFile.QuadPart, &off, NULL ))
        {
            RtlFreeHeap( NtCurrentTeb()->Peb->ProcessHeap, 0, data );
            data = NULL;
        }
        else data[io.Information] = 0;
    }
    NtClose( handle );
    if (!data)
        ERR( "cannot read the COM row list %s (empty, over 1 MB, or a read "
             "error); NO rows are forced\n", path );
    return data;
}

static int ascii_lower( int c ) { return c >= 'A' && c <= 'Z' ? c + 32 : c; }

static BOOL ascii_ieq( const char *a, const char *b )
{
    while (*a && ascii_lower( (unsigned char)*a ) == ascii_lower( (unsigned char)*b ))
    { a++; b++; }
    return !*a && !*b;
}

static BOOL ascii_ineq( const char *a, const char *b, UINT n )
{
    UINT i;

    for (i = 0; i < n; i++)
        if (ascii_lower( (unsigned char)a[i] ) != ascii_lower( (unsigned char)b[i] ))
            return FALSE;
    return TRUE;
}

/* Add one target name to row_targets, taking ownership of nothing: the name
 * is copied, because it may point into an environment string or a file buffer
 * that is freed as soon as parsing is done. */
static void add_row_target( const char *name, UINT len )
{
    char **grown;
    char *copy;

    if (!len) return;
    /* RtlReAllocateHeap is NOT realloc: it answers NULL for a NULL pointer
     * rather than allocating, so the first entry has to be allocated. */
    grown = row_targets
        ? RtlReAllocateHeap( NtCurrentTeb()->Peb->ProcessHeap, 0, row_targets,
                             (row_target_count + 1) * sizeof(*row_targets) )
        : RtlAllocateHeap( NtCurrentTeb()->Peb->ProcessHeap, 0, sizeof(*row_targets) );
    if (!grown) return;
    row_targets = grown;
    if (!(copy = RtlAllocateHeap( NtCurrentTeb()->Peb->ProcessHeap, 0, len + 1 ))) return;
    memcpy( copy, name, len );
    copy[len] = 0;
    row_targets[row_target_count++] = copy;
}

/* Split on commas and newlines, dropping `#` comments and surrounding space,
 * and expanding a leading `@` into the file it names.  `depth` stops an
 * `@file` whose contents name another file. */
static void parse_row_targets( const char *list, void (*take)( const char *, UINT ),
                               int depth )
{
    const char *p = list;

    while (*p)
    {
        const char *start, *end;

        while (*p == ',' || *p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') p++;
        if (*p == '#')
        {
            while (*p && *p != '\n') p++;
            continue;
        }
        start = p;
        while (*p && *p != ',' && *p != '\n' && *p != '#') p++;
        end = p;
        while (end > start && (end[-1] == ' ' || end[-1] == '\t' || end[-1] == '\r')) end--;
        if (end == start) continue;
        if (*start == '@')
        {
            char *body, *name;
            UINT len = (UINT)(end - start - 1);

            if (depth || !len)
            {
                ERR( "nested or empty @file in a COM lever list; ignored\n" );
                continue;
            }
            if (!(name = RtlAllocateHeap( NtCurrentTeb()->Peb->ProcessHeap, 0, len + 1 )))
                continue;
            memcpy( name, start + 1, len );
            name[len] = 0;
            if ((body = com_read_list_file( name )))
            {
                parse_row_targets( body, take, 1 );
                RtlFreeHeap( NtCurrentTeb()->Peb->ProcessHeap, 0, body );
            }
            RtlFreeHeap( NtCurrentTeb()->Peb->ProcessHeap, 0, name );
            continue;
        }
        take( start, (UINT)(end - start) );
    }
}

static UINT hexval( char c )
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return ~0u;
}

/* One IID target: `{xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx}` (braces optional)
 * or a bare leading 8 hex digits, which matches on Data1 alone. */
static void add_blocked_iid( const char *name, UINT len )
{
    struct blocked_iid *grown, ent;
    UINT hex[32], nhex = 0, i;

    memset( &ent, 0, sizeof(ent) );
    for (i = 0; i < len && nhex < ARRAYSIZE(hex); i++)
    {
        UINT v;

        if (name[i] == '{' || name[i] == '}' || name[i] == '-') continue;
        if ((v = hexval( name[i] )) == ~0u) { nhex = 0; break; }
        hex[nhex++] = v;
    }
    if (nhex != 8 && nhex != 32)
    {
        ERR( "WINEEMUNOCOMIIDS: %.*s is neither a full IID nor a leading "
             "8 hex digits; ignored\n", (int)len, name );
        return;
    }
    for (i = 0; i < 8; i++) ent.guid.Data1 = (ent.guid.Data1 << 4) | hex[i];
    if (nhex == 32)
    {
        for (i = 8; i < 12; i++) ent.guid.Data2 = (USHORT)((ent.guid.Data2 << 4) | hex[i]);
        for (i = 12; i < 16; i++) ent.guid.Data3 = (USHORT)((ent.guid.Data3 << 4) | hex[i]);
        for (i = 0; i < 8; i++)
            ent.guid.Data4[i] = (unsigned char)((hex[16 + i * 2] << 4) | hex[17 + i * 2]);
    }
    else ent.data1_only = TRUE;

    grown = blocked_iids     /* the NULL rule again, see add_row_target */
        ? RtlReAllocateHeap( NtCurrentTeb()->Peb->ProcessHeap, 0, blocked_iids,
                             (blocked_iid_count + 1) * sizeof(*blocked_iids) )
        : RtlAllocateHeap( NtCurrentTeb()->Peb->ProcessHeap, 0, sizeof(*blocked_iids) );
    if (!grown) return;
    blocked_iids = grown;
    blocked_iids[blocked_iid_count++] = ent;
}

static BOOL iid_is_blocked( const GUID *riid )
{
    UINT i;

    for (i = 0; i < blocked_iid_count; i++)
    {
        if (blocked_iids[i].guid.Data1 != riid->Data1) continue;
        if (blocked_iids[i].data1_only) return TRUE;
        if (!memcmp( &blocked_iids[i].guid, riid, sizeof(GUID) )) return TRUE;
    }
    return FALSE;
}

/* Does `target` (an `Iface::Slot` spelling) name this row?  Two spellings are
 * accepted for one reason: the row's OWN name carries the interface a slot was
 * DECLARED on, which for an inherited slot is a base interface -- the
 * ID3D10Device1 table's rows are all named "ID3D10Device::..." -- while the
 * roster and every doc name the interface the row LIVES on.  A reader with
 * either in front of them should get the row they meant. */
static BOOL row_target_matches( const char *target, const char *iface_name,
                                const char *slot_name )
{
    const char *tsep, *ssep;
    UINT n;

    if (!slot_name) return FALSE;
    for (tsep = target; *tsep; tsep++)
        if (tsep[0] == ':' && tsep[1] == ':') break;
    if (!*tsep || !tsep[2]) return FALSE;      /* no `::`, or nothing after it */
    if (!(n = (UINT)(tsep - target))) return FALSE;

    /* a row's own name is either `Iface::Slot` or a bare slot name */
    for (ssep = slot_name; *ssep; ssep++)
        if (ssep[0] == ':' && ssep[1] == ':') break;
    if (!ascii_ieq( tsep + 2, *ssep ? ssep + 2 : slot_name )) return FALSE;

    if (iface_name && strlen( iface_name ) == n && ascii_ineq( target, iface_name, n ))
        return TRUE;
    return *ssep && (UINT)(ssep - slot_name) == n && ascii_ineq( target, slot_name, n );
}

/* Does this row have out-parameters the generator could not give scrub masks
 * for?  A forced refusal of such a row is still the right answer -- it is what
 * the row did before the landing -- but the caller's out-params keep whatever
 * residue was in them, which is the Witcher 3 crash class.  Say so per row
 * rather than let a bisect leg carry a silent hazard. */
static BOOL row_out_params_unscrubbed( const struct winecom_slot *sl )
{
    UINT i;

    if (sl->scrubptr | sl->scrubdw | sl->scrubq) return FALSE;
    if (!sl->cls) return FALSE;
    for (i = 1; i < sl->argc; i++)
        switch (sl->cls[i - 1])
        {
        case WINECOM_CA_PPV_OUT:
        case WINECOM_CA_RET_PTR:
        case WINECOM_CA_IFACE_OUT_STATIC:
        case WINECOM_CA_IFACE_ARR_OUT_STATIC:
        case WINECOM_CA_IFACE_ARR_OUT_COUNTPTR:
            return TRUE;
        }
    return FALSE;
}

/* Read the three levers and resolve them against THIS surface's tables.  Runs
 * once, inside com_runtime_init_once, after iface_slot_base/total_slots are
 * built and BEFORE any guest-side fast path is installed -- a const-getter or
 * journal snippet on a forced row would serve it from guest code without ever
 * trapping, and the lever would be a lie. */
static void arm_row_kill_switches( void )
{
    char *rows, *iids, *waves;
    UINT i, n, t, forced = 0, partial = 0;

    rows = com_env_str( L"WINEEMUNOCOMROWS" );
    iids = com_env_str( L"WINEEMUNOCOMIIDS" );
    waves = com_env_str( L"WINEEMUNOCOMWAVE" );
    if (!rows && !iids && !waves) return;

    if (rows) parse_row_targets( rows, add_row_target, 0 );
    if (iids) parse_row_targets( iids, add_blocked_iid, 0 );
    if (waves)
    {
        /* wave names are not row names: expand each into the derived sets,
         * and name an unknown wave rather than expanding it to nothing */
        const char *p = waves;

        while (*p)
        {
            const char *start;
            UINT len, w;
            BOOL known = FALSE;

            while (*p == ',' || *p == ' ' || *p == '\t') p++;
            start = p;
            while (*p && *p != ',') p++;
            len = (UINT)(p - start);
            while (len && (start[len - 1] == ' ' || start[len - 1] == '\t')) len--;
            if (!len) continue;
            for (w = 0; w < ARRAYSIZE(wc_waves); w++)
            {
                if (strlen( wc_waves[w].name ) != len ||
                    memcmp( wc_waves[w].name, start, len )) continue;
                known = TRUE;
                for (i = 0; i < wc_waves[w].row_count; i++)
                    add_row_target( wc_waves[w].rows[i],
                                    (UINT)strlen( wc_waves[w].rows[i] ) );
                for (i = 0; i < wc_waves[w].iid_count; i++)
                    add_blocked_iid( wc_waves[w].iids[i],
                                     (UINT)strlen( wc_waves[w].iids[i] ) );
                ERR( "%s: WINEEMUNOCOMWAVE=%s -- %u rows and %u IIDs go back to "
                     "their pre-landing refusal\n", wc_surface->name,
                     wc_waves[w].name, wc_waves[w].row_count, wc_waves[w].iid_count );
                break;
            }
            if (!known)
                ERR( "WINEEMUNOCOMWAVE names no wave '%.*s' -- this leg forces "
                     "NOTHING from it; the known names are in "
                     "libs/winecom/winecom_waves.h\n", (int)len, start );
        }
    }
    RtlFreeHeap( NtCurrentTeb()->Peb->ProcessHeap, 0, rows );
    RtlFreeHeap( NtCurrentTeb()->Peb->ProcessHeap, 0, iids );
    RtlFreeHeap( NtCurrentTeb()->Peb->ProcessHeap, 0, waves );

    if (blocked_iid_count)
        ERR( "%s: WINEEMUNOCOMIIDS -- %u IIDs are treated as UNROSTERED; an "
             "interface handed out under one is released, the out pointer "
             "NULLed, E_NOINTERFACE returned\n", wc_surface->name, blocked_iid_count );
    if (!row_target_count) return;

    /* the per-(iface,slot) bitset, allocated only because something matched */
    if (!(forced_refuse = RtlAllocateHeap( NtCurrentTeb()->Peb->ProcessHeap,
                                           HEAP_ZERO_MEMORY, total_slots )))
    {
        ERR( "%s: no memory for the forced-refusal map; NO rows are forced -- "
             "do not read this leg as 'tested clean'\n", wc_surface->name );
        return;
    }
    row_target_hit = RtlAllocateHeap( NtCurrentTeb()->Peb->ProcessHeap,
                                      HEAP_ZERO_MEMORY, row_target_count );

    for (i = 0; i < wc_surface->iface_count; i++)
    {
        const struct winecom_iface *itf = &wc_surface->ifaces[i];

        if (!itf->slots) continue;
        for (n = 0; n < itf->slot_count; n++)
            for (t = 0; t < row_target_count; t++)
            {
                if (!row_target_matches( row_targets[t], itf->name,
                                         itf->slots[n].name )) continue;
                if (row_target_hit) row_target_hit[t] = 1;
                if (forced_refuse[iface_slot_base[i] + n]) break;
                forced_refuse[iface_slot_base[i] + n] = 1;
                forced++;
                if (row_out_params_unscrubbed( &itf->slots[n] ))
                {
                    partial++;
                    ERR( "%s: forcing %s::%s to refuse, but the row has "
                         "out-parameters and NO scrub masks -- the refusal is "
                         "PARTIAL and the caller reads its own residue there "
                         "(the Witcher 3 GetShader class)\n", wc_surface->name,
                         itf->name, itf->slots[n].name );
                }
                break;
            }
    }
    ERR( "%s: WINEEMUNOCOMROWS/WAVE armed -- %u of this surface's slots forced "
         "to refuse (%u of them scrub only partially)\n", wc_surface->name,
         forced, partial );

    /* A target that matched nothing HERE may well match on another surface's
     * runtime (this library is one instance per linkee), so the report names
     * the surface and stays a warning; a target that matches nowhere shows up
     * once per attached surface, which is the loudest a per-instance runtime
     * can be about it. */
    if (row_target_hit)
        for (t = 0; t < row_target_count; t++)
            if (!row_target_hit[t])
                WARN( "%s: WINEEMUNOCOMROWS names '%s', which matches no row on "
                      "this surface -- if no surface claims it, it is a TYPO and "
                      "this leg tested nothing\n", wc_surface->name, row_targets[t] );
    if (!forced)
        ERR( "%s: every name given to WINEEMUNOCOMROWS/WAVE missed; NOTHING is "
             "forced on this surface\n", wc_surface->name );
}

/* The dispatch-time question, both lanes.  Unarmed: one NULL test. */
static inline BOOL row_forced_refused( UINT iface, UINT slot )
{
    return forced_refuse && forced_refuse[iface_slot_base[iface] + slot];
}

static HMODULE find_guest_module( const WCHAR *const *names, UINT count, BOOL require_com_table )
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
                /* same shadowing rule as the 32-bit walk: an application's
                 * own copy of a thunk module publishes no com table and is
                 * not a candidate.  The QPC arm path looks kernel32 up
                 * through here too and legitimately lacks the table, so the
                 * requirement is the caller's choice. */
                if (require_com_table &&
                    !find_export32( (ULONG_PTR)mod->DllBase, "__wine_com_thunk_info" ))
                    continue;
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

    if (guest32) ptr = find_export32( base, "__wine_com_thunk_info" );
    else
    {
        RtlInitAnsiString( &name, "__wine_com_thunk_info" );
        if (LdrGetProcedureAddress( guest, &name, 0, &ptr )) ptr = NULL;
    }
    if (!ptr)
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

    /* the snippets are x86-64 encodings and the cached_qword offset is the
     * 64-bit guest ABI; an i386 guest keeps trapping, which is that lane's
     * baseline for everything */
    if (guest32) return;

    for (i = 0; i < wc_surface->iface_count; i++)
    {
        const struct winecom_iface *itf = &wc_surface->ifaces[i];
        if (!itf->slots) continue;
        for (n = 0; n < itf->slot_count; n++)
            if ((itf->slots[n].flags & WINECOM_F_CONST_QWORD) &&
                !row_forced_refused( i, n )) count++;
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

            /* A forced-refused row must keep TRAPPING: a guest-side snippet
             * would answer it from the cache with no dispatch at all, and the
             * kill switch would be a lie.  Both loops skip identically or the
             * emitted block would not be the size the first pass counted. */
            if (!(itf->slots[n].flags & WINECOM_F_CONST_QWORD) ||
                row_forced_refused( i, n )) continue;
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
                                   const UINT64 *rawargs, UINT64 *rax_out, UINT64 *fpret_bits );

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

        status = invoke_marshalled( itf, sl, p, iface, slot, rawargs, &rax, NULL );
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

    if (guest32) return;   /* x86-64 snippet encodings; see install_const_getters */
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

            /* a forced-refused row keeps TRAPPING: a guest-side journal
             * snippet would record the call and never reach the dispatcher,
             * so the kill switch would not be one */
            if (row_forced_refused( i, n )) continue;

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

/***********************************************************************
 *           the device journal -- the descriptor shadow's write half
 *
 * [MEASURED, crossings table] with the command-list journal in, the top of
 * the COM class is the DEVICE-side descriptor pair: CopyDescriptors at
 * 164k/s and CreateConstantBufferView at 95k/s.  Device methods are
 * free-threaded, so the list journal's one-recorder-per-object rule does
 * not hold and a per-proxy ring would race.  What holds instead:
 *
 *   RINGS    one ring per RECORDING THREAD, anchored at the guest TEB's
 *            SystemReserved1[0] (+0x190, unused by Wine's 64-bit side,
 *            C_ASSERT-pinned below).  A thread's first TRAPPED call on a
 *            curated slot arms its ring; while the anchor is NULL the
 *            snippet falls back to the trap.  Rings are never freed: a
 *            thread that exits leaks its 32K ring, bounded by thread
 *            count, and that is a accepted cost of keeping the guest
 *            side lock-free.
 *   ORDER    every record carries an RDTSC stamp -- under the bridge,
 *            rdtsc reads the POWER timebase, which the QPC fast path
 *            proved core-synchronized -- and the drain k-way-merges all
 *            rings by stamp.  An APP-ORDERED pair of creates spans a call
 *            return plus the app's own synchronization, so both records
 *            are ring-visible with ordered stamps before the later call
 *            is even issued; merge order equals issued order.  Two
 *            unordered creates may replay either way, exactly as D3D12
 *            allows.
 *   CONSUME  CopyDescriptors -- and every other descriptor consumer --
 *            still traps, and winecom_dispatch drains ALL device rings
 *            before serving anything (wc_dev_drain; the guest-set dirty
 *            byte makes the idle check one load).  Create-then-copy is
 *            therefore ordered no matter which threads did which.
 *   RECLAIM  headers carry pos (guest-written) and cons (native-written).
 *            Any thread's drain advances cons; only a drain running ON
 *            the ring's owner thread resets both to zero, because only
 *            then is the ring's writer provably idle (it is here, inside
 *            its own trap).  Ring-full falls back to the slot's own trap
 *            stub, whose dispatch drains first -- the slow path is the
 *            old path.
 *   ARGS     the 16-byte D3D12_CONSTANT_BUFFER_VIEW_DESC is copied INTO
 *            the record (its pointer kept only for its NULLness); the
 *            by-value descriptor handle rides in the record; the proxy
 *            pointer is re-validated through proxy_from_pointer at
 *            replay, so a corrupted ring drops loudly instead of
 *            dispatching through garbage.
 *
 * WINEEMUNOCOMDEVJOURNAL=1 is the kill switch: no snippets, every call
 * traps, the crossing counter shows the row again.  WINEEMUCOMDEVSABOTAGE=1
 * disarms the DRAIN while leaving the recording live -- creates are
 * recorded and never replayed -- which is the gate's negative control:
 * the replay transcript disappears and dependent state goes stale.
 */

#define DEV_RING_DATA   0x40      /* header size; data starts here (guest ABI) */
#define DEV_RING_SIZE   0x8000    /* 32K of records a thread, ~585 records */
#define DEV_REC_BYTES   56
#define DEV_SHAPE_CBV   8         /* outside enum journal_shape on purpose: a
                                     device record leaking into a per-proxy
                                     ring must fail its size check there */

/* The snippet trusts nothing: SystemReserved1 is reserved from WINE, not
 * from the app, and a guest that stashes its own value in slot [0] would
 * otherwise hand the recorder a wild "ring" whose cap check garbage can
 * pass -- 56-byte records sprayed into app memory, GPU-visible heaps
 * included, with no CPU-side fault to show for it.  The magic is checked
 * before anything else is believed; a foreign value falls back to the trap
 * forever, which is the old world. */
#define DEV_RING_MAGIC  0x4e524a5645444657ull   /* "WFDEVJRN" */

struct dev_ring
{
    UINT64 pos;               /* 0x00: bytes appended, guest-written (guest ABI) */
    UINT64 cap;               /* 0x08: data bytes available        (guest ABI) */
    UINT64 magic;             /* 0x10: DEV_RING_MAGIC              (guest ABI) */
    UINT64 cons;              /* bytes replayed, native-written */
    UINT   owner_tid;
    UINT   bad_cons;          /* ~0u, or the cons offset that failed validation */
    struct dev_ring *next;    /* the drain-all registry, dev_cs-guarded */
    UINT64 snap;              /* this drain's pos snapshot */
    UINT64 bad_since;         /* raw timebase when bad_cons first failed */
};
#ifdef _WIN64
C_ASSERT( sizeof(struct dev_ring) <= DEV_RING_DATA );
C_ASSERT( offsetof(struct dev_ring, pos) == 0x00 );
C_ASSERT( offsetof(struct dev_ring, cap) == 0x08 );
C_ASSERT( offsetof(struct dev_ring, magic) == 0x10 );
C_ASSERT( offsetof(TEB, SystemReserved1) == 0x190 );  /* the snippet's gs:[0x190] */
#endif

static struct dev_ring *dev_rings;
static volatile BYTE dev_dirty;         /* guest-written; address baked into snippets */
static unsigned char *dev_slot_map;     /* per (iface,slot): curated for the device journal */
static BOOL dev_journal_on;
static BOOL dev_sabotage;
static BOOL dev_double;
static UCHAR dev_stamp_shift;           /* rdtsc == mftb << shift; from __wine_thunk_qpc */
static int dev_stamp_known = -1;        /* -1 unread, 0 block not armed yet, 1 known */

#ifndef __powerpc64__
/* the device journal only installs on the ppc64 host build; on any other
 * lane dev_dirty stays 0 and the drain returns before the cut is taken */
#define emu_qpc_timebase() 0
#endif

static CRITICAL_SECTION dev_cs;
static CRITICAL_SECTION_DEBUG dev_cs_debug =
{
    0, 0, &dev_cs,
    { &dev_cs_debug.ProcessLocksList, &dev_cs_debug.ProcessLocksList },
      0, 0, { (DWORD_PTR)(__FILE__ ": winecom dev_cs") }
};
static CRITICAL_SECTION dev_cs = { &dev_cs_debug, -1, 0, 0, 0, 0 };

static struct com_proxy *proxy_from_pointer( void *ptr );

/* The drain's consistent cut needs the stamps' unit: rdtsc is the timebase
 * shifted by the emulator's TSC scale, and the armed qpc block in guest
 * kernel32 is where that scale lives (the COM thunk modules carry no
 * fast-path exports and no block of their own).  The block arms on the
 * guest's first QPC call, which can postdate COM init, so this is asked
 * again on every arming attempt until it answers -- and no ring ever
 * exists before the stamps have a native unit. */
static BOOL dev_stamp_ready( void )
{
#ifdef _WIN64
    static const WCHAR k32W[] = { 'k','e','r','n','e','l','3','2','.','d','l','l',0 };
    const WCHAR *names = k32W;
    const struct emu_qpc_guest *qpc;
    ANSI_STRING qpc_name;
    void *qpc_ptr;
    HMODULE k32;

    if (dev_stamp_known > 0) return TRUE;
    if (!(k32 = find_guest_module( &names, 1, FALSE ))) return FALSE;
    RtlInitAnsiString( &qpc_name, "__wine_thunk_qpc" );
    if (LdrGetProcedureAddress( k32, &qpc_name, 0, &qpc_ptr ) ||
        !(qpc = qpc_ptr) || qpc->magic != EMU_QPC_MAGIC || !qpc->enabled ||
        qpc->shift > EMU_QPC_MAX_SHIFT)
    {
        if (dev_stamp_known < 0)
            WARN( "guest kernel32's qpc block is not armed yet; device rings "
                  "wait for the first guest QPC call\n" );
        dev_stamp_known = 0;
        return FALSE;
    }
    dev_stamp_shift = qpc->shift;
    dev_stamp_known = 1;
    TRACE( "devjournal: stamp unit is mftb << %u\n", dev_stamp_shift );
    return TRUE;
#else
    return FALSE;
#endif
}

/* Arm the calling thread's ring: allocate, register, hang it on the TEB.
 * Runs inside the thread's own trap on a curated slot, so the thread's
 * guest half is idle and the plain anchor store is safe. */
static void wc_dev_arm( void )
{
    struct dev_ring *r;
    SIZE_T size = DEV_RING_DATA + DEV_RING_SIZE;
    void *mem = NULL;

    if (!dev_journal_on) return;
    if ((r = NtCurrentTeb()->SystemReserved1[0]))
    {
        /* nonzero already: ours (armed), or the APP's own value -- say so
         * once, because that is a thread the fast path can never serve */
        if (r->magic != DEV_RING_MAGIC)
        {
            static LONG reported;
            if (InterlockedIncrement( &reported ) <= 8)
                ERR( "thread %04lx TEB SystemReserved1[0] holds a foreign "
                     "value %p; its creates stay trapping\n",
                     HandleToULong( NtCurrentTeb()->ClientId.UniqueThread ),
                     (void *)r );
        }
        return;
    }
    if (!dev_stamp_ready()) return;
    if (NtAllocateVirtualMemory( NtCurrentProcess(), &mem, 0, &size,
                                 MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE ))
        return;   /* every call keeps trapping, which is the old world */
    r = mem;
    r->pos = 0;
    r->cap = DEV_RING_SIZE;
    r->magic = DEV_RING_MAGIC;
    r->cons = 0;
    r->owner_tid = HandleToULong( NtCurrentTeb()->ClientId.UniqueThread );
    r->bad_cons = ~0u;
    r->bad_since = 0;
    RtlEnterCriticalSection( &dev_cs );
    r->next = dev_rings;
    dev_rings = r;
    RtlLeaveCriticalSection( &dev_cs );
    NtCurrentTeb()->SystemReserved1[0] = r;
    TRACE( "devjournal: ring %p armed for thread %04x\n", r, r->owner_tid );
}

/* Replay every device-ring record, all rings merged by stamp, then let the
 * calling thread reclaim its own ring.  Serialized by dev_cs: concurrent
 * dispatches must not replay a record twice, and a dispatch about to serve
 * a dependent call must WAIT for a drain in flight, not skip it. */
static void wc_dev_drain( void )
{
    struct dev_ring *r;
    UINT64 curtid, cut;

    static BOOL dev_draining;   /* same-thread re-entry fuse: dev_cs is
                                   recursive, and a replay's classifier can
                                   reach wc_forward_host, which drains too --
                                   a nested merge would advance cons and
                                   retake snapshots under the outer one */

    if (!dev_dirty) return;
    if (dev_sabotage) return;   /* the gate's negative control */
    RtlEnterCriticalSection( &dev_cs );
    if (dev_draining)
    {
        RtlLeaveCriticalSection( &dev_cs );
        return;
    }
    dev_draining = TRUE;
    /* clear-then-read, with a FULL BARRIER between: the clear must be
     * globally visible before the pos loads below execute.  A plain store
     * can sit in this core's store buffer past those loads (POWER reorders
     * store->load freely), and that interleave STRANDS a record: our pos
     * load misses the guest's new record, the guest's dirty=1 lands, and
     * our delayed clear then overwrites it -- record invisible, flag zero.
     * The next create re-arms the flag, so the strand is usually microseconds;
     * but the LAST create before an ExecuteCommandLists has no successor,
     * the execute's own drain sees a clear flag and skips, and the GPU
     * consumes a stale descriptor.  [MEASURED 2026-08-27: exactly that --
     * one amdgpu gfx-ring timeout per Cyberpunk benchmark leg, no VM
     * fault, gone with the journal off; the seq_cst exchange closes it.] */
    __atomic_exchange_n( &dev_dirty, 0, __ATOMIC_SEQ_CST );
    /* THE CONSISTENT CUT.  The snapshot loop below walks the rings one at a
     * time while producers keep publishing, so ring A's snapshot can predate
     * a record whose app-ordered SUCCESSOR lands in later-walked ring B's --
     * merge that snapshot alone and the successor replays THIS drain, the
     * predecessor the NEXT: an ordered same-slot pair applied backwards.
     * [MEASURED 2026-08-27, +winecom leg: 1,680 stamp inversions in 2.08M
     * replays, one amdgpu gfx-ring timeout per benchmark leg from the stale
     * descriptors.]  So the merge stops at a timestamp taken HERE, before
     * any snapshot: a record stamped before the cut was CREATED before it,
     * and if its publication raced a snapshot its ordered successors cannot
     * exist yet (its own call has not returned), while everything a
     * CONSUMER is owed was published before the consumer even trapped --
     * stamped before the cut by construction.  Records past the cut keep
     * the flag set and replay next drain.  Native mftb and the snippets'
     * rdtsc share a zero: rdtsc == mftb << shift (wine/emu_qpc.h). */
    cut = emu_qpc_timebase() << dev_stamp_shift;
    for (r = dev_rings; r; r = r->next)
    {
        r->snap = __atomic_load_n( &r->pos, __ATOMIC_ACQUIRE );
        if (r->snap > r->cap || r->snap % DEV_REC_BYTES)
        {
            ERR( "devjournal: ring %p pos %I64u is corrupt (cap %I64u); "
                 "skipping its records\n", r, r->snap, r->cap );
            r->snap = r->cons;
        }
    }
    for (;;)
    {
        struct dev_ring *best = NULL;
        UINT64 best_stamp = 0;
        const BYTE *rec;
        UINT key, sizesh, iface, slot;
        const struct winecom_iface *itf;
        const struct winecom_slot *sl;
        struct com_proxy *p;
        UINT64 rawargs[16] = { 0 }, rax;
        NTSTATUS status;

        for (r = dev_rings; r; r = r->next)
        {
            UINT64 stamp;
            if (r->cons >= r->snap) continue;
            stamp = *(const UINT64 *)((const BYTE *)r + DEV_RING_DATA + r->cons + 8);
            if (!best || stamp < best_stamp) { best = r; best_stamp = stamp; }
        }
        if (!best) break;
        if (best_stamp >= cut)
        {
            /* every remaining head is younger than the cut (per-ring order
             * is per-thread order); they are the next drain's records */
            dev_dirty = 1;
            break;
        }

        rec = (const BYTE *)best + DEV_RING_DATA + best->cons;
        key = *(const UINT *)rec;
        sizesh = *(const UINT *)(rec + 4);
        iface = key >> 16;
        slot = key & 0xffff;

        /* VALIDATION-GATED ADVANCE.  [MEASURED 2026-08-27, the double-apply
         * leg: 58,583 of ~2M records read with garbage fields] a cross-thread
         * drain can read a record whose pos-publish is visible but whose
         * BYTES are not yet -- the guest's stores reach this native reader
         * unordered.  A record that fails ANY check is therefore HELD, not
         * dropped and never replayed: its bytes land within microseconds and
         * the next drain takes it.  Correct because an app-ORDERED create
         * sits behind the app's own synchronization, whose barriers flush
         * the record before the ordered consumer can trap -- only RACING
         * records are ever held, and holding a racing create is within
         * D3D12's contract.  A record still failing after a full
         * millisecond of timebase is not a torn record but a corrupt one,
         * and the ring drops loudly. */
        p = NULL;
        if (sizesh != (DEV_REC_BYTES | (DEV_SHAPE_CBV << 24)) ||
            iface >= wc_surface->iface_count ||
            slot >= wc_surface->ifaces[iface].slot_count ||
            !dev_slot_map[iface_slot_base[iface] + slot] ||
            !(p = proxy_from_pointer( *(void **)(rec + 16) )))
            /* STRUCTURE only: header word, curated slot, interned proxy.
             * The desc CONTENT is passed through exactly as the live path
             * would pass the app's own pointer -- the first cut here
             * second-guessed it and held {0,0}, which is the perfectly
             * legal NULL CBV Cyberpunk writes 58k of per run */
        {
            UINT64 now = emu_qpc_timebase();
            if (best->bad_cons != (UINT)best->cons)
            {
                best->bad_cons = (UINT)best->cons;
                best->bad_since = now;
            }
            else if (now - best->bad_since > 512000)   /* ~1ms at 512 MHz */
            {
                ERR( "devjournal: record at cons %I64u in ring %p is corrupt, "
                     "not torn (key %08x size/shape %08x, %s); dropping the "
                     "rest of the ring\n", best->cons, best, key, sizesh,
                     p ? "fields implausible" : "no proxy" );
                best->bad_cons = ~0u;
                best->cons = best->snap;
                continue;
            }
            best->snap = best->cons;   /* hold this ring for this drain */
            continue;
        }
        if (best->bad_cons == (UINT)best->cons) best->bad_cons = ~0u;
        itf = &wc_surface->ifaces[iface];
        sl = &itf->slots[slot];

        rawargs[1] = *(const UINT64 *)(rec + 24) ? (UINT64)(ULONG_PTR)(rec + 40) : 0;
        rawargs[2] = *(const UINT64 *)(rec + 32);
        TRACE( "devjournal: replay %s proxy %p va %I64x size %u handle %I64x stamp %I64u\n",
               sl->name, p, *(const UINT64 *)(rec + 40), *(const UINT *)(rec + 48),
               *(const UINT64 *)(rec + 32), best_stamp );
        status = invoke_marshalled( itf, sl, p, iface, slot, rawargs, &rax, NULL );
        if (status)
            ERR( "devjournal: replay of %s failed, status %08x; continuing\n",
                 sl->name, (UINT)status );
        best->cons += DEV_REC_BYTES;
    }
    curtid = HandleToULong( NtCurrentTeb()->ClientId.UniqueThread );
    for (r = dev_rings; r; r = r->next)
    {
        UINT64 pos = __atomic_load_n( &r->pos, __ATOMIC_RELAXED );
        if (r->owner_tid == curtid && r->cons == pos)
        {
            /* the writer is this thread and it is here, not appending */
            __atomic_store_n( &r->pos, 0, __ATOMIC_RELEASE );
            r->cons = 0;
        }
        /* a record published after our snapshot leaves cons behind pos:
         * re-arm the flag ourselves so the next dispatch drains it even if
         * that record was the last of its burst -- the belt to the
         * barrier's braces */
        else if (r->cons < pos) dev_dirty = 1;
    }
    dev_draining = FALSE;
    RtlLeaveCriticalSection( &dev_cs );
}

#ifdef _WIN64
/***********************************************************************
 *           install_dev_journal
 *
 * Emit the per-thread recording snippets for the curated device slots and
 * point their vtable entries at them.  Byte encodings llvm-mc-verified
 * (the source .s is in the commit message's session); the shape:
 *
 *     mov  r10, gs:[0x190]        ; the thread's ring; NULL -> fallback
 *     test r10, r10 ; jz fb
 *     movabs rax, MAGIC ; cmp [r10+0x10], rax ; jne fb   ; not OUR ring
 *     mov  r11, [r10]             ; pos
 *     lea  rax, [r11+56] ; cmp rax, [r10+8] ; ja fb
 *     mov  r9, rdx                ; pDesc, out of rdtsc's way
 *     rdtsc ; shl rdx,32 ; or rax,rdx
 *     lea  r11, [r10+r11+0x40]    ; the record
 *     mov  dword [r11], KEY ; mov dword [r11+4], 56|8<<24
 *     mov  [r11+8], rax           ; stamp
 *     mov  [r11+16], rcx          ; proxy
 *     mov  [r11+24], r9 ; mov [r11+32], r8
 *     test r9, r9 ; jz over the 16-byte desc copy
 *     lea  rdx, [r11-8] ; sub rdx, r10 ; mov [r10], rdx   ; publish pos
 *     movabs r10, &dev_dirty ; mov byte [r10], 1 ; ret
 * fb: jmp [rip+0] ; .quad trap_stub
 *
 * The fallback fires before anything but rax/r10/r11 is touched, so the
 * trap stub sees the call exactly as issued.
 */
static const struct { const char *name; UINT argc; } dev_slot_defs[] =
{
    { "ID3D12Device::CreateConstantBufferView", 3 },
};

static void install_dev_journal( void )
{
    static const BYTE dv_pre1[] = { 0x65, 0x4c, 0x8b, 0x14, 0x25,       /* mov r10,gs:[0x190] */
                                    0x90, 0x01, 0x00, 0x00,
                                    0x4d, 0x85, 0xd2 };                 /* test r10,r10 */
    static const BYTE dv_pre2[] = { 0x4d, 0x8b, 0x1a,                   /* mov r11,[r10] */
                                    0x49, 0x8d, 0x43, 0x38,             /* lea rax,[r11+56] */
                                    0x49, 0x3b, 0x42, 0x08 };           /* cmp rax,[r10+8] */
    static const BYTE dv_body1[] = { 0x49, 0x89, 0xd1,                  /* mov r9,rdx */
                                     0x0f, 0x31,                        /* rdtsc */
                                     0x48, 0xc1, 0xe2, 0x20,            /* shl rdx,32 */
                                     0x48, 0x09, 0xd0,                  /* or rax,rdx */
                                     0x4f, 0x8d, 0x5c, 0x1a, 0x40 };    /* lea r11,[r10+r11+0x40] */
    static const BYTE dv_body2[] = { 0x49, 0x89, 0x43, 0x08,            /* mov [r11+8],rax */
                                     0x49, 0x89, 0x4b, 0x10,            /* mov [r11+16],rcx */
                                     0x4d, 0x89, 0x4b, 0x18,            /* mov [r11+24],r9 */
                                     0x4d, 0x89, 0x43, 0x20,            /* mov [r11+32],r8 */
                                     0x4d, 0x85, 0xc9,                  /* test r9,r9 */
                                     0x74, 0x0f,                        /* jz +15 (over the copy) */
                                     0x49, 0x8b, 0x01,                  /* mov rax,[r9] */
                                     0x49, 0x89, 0x43, 0x28,            /* mov [r11+40],rax */
                                     0x49, 0x8b, 0x41, 0x08,            /* mov rax,[r9+8] */
                                     0x49, 0x89, 0x43, 0x30,            /* mov [r11+48],rax */
                                     0x49, 0x8d, 0x53, 0xf8,            /* lea rdx,[r11-8] */
                                     0x4c, 0x29, 0xd2,                  /* sub rdx,r10 */
                                     0x49, 0x89, 0x12 };                /* mov [r10],rdx */
    static const BYTE dv_epi[] = { 0x41, 0xc6, 0x02, 0x01,              /* mov byte [r10],1 */
                                   0xc3 };                              /* ret */
    static const BYTE fb[]  = { 0xff, 0x25, 0x00, 0x00, 0x00, 0x00 };   /* jmp [rip+0] */

    static const UINT snippet_stride = 160;   /* the body is ~131 */
    unsigned char *block, *code;
    SIZE_T size;
    UINT i, n, j, k, count = 0;
    BYTE tmp[16];

    if (guest32) return;   /* x86-64 snippet encodings; see install_const_getters */
    if (!(dev_slot_map = RtlAllocateHeap( NtCurrentTeb()->Peb->ProcessHeap,
                                          HEAP_ZERO_MEMORY, total_slots )))
        return;

    for (i = 0; i < wc_surface->iface_count; i++)
    {
        const struct winecom_iface *itf = &wc_surface->ifaces[i];
        if (!itf->slots) continue;
        for (n = 0; n < itf->slot_count; n++)
            for (j = 0; j < ARRAYSIZE(dev_slot_defs); j++)
                if (itf->slots[n].name && !strcmp( itf->slots[n].name, dev_slot_defs[j].name ))
                    count++;
    }
    if (!count) return;
    /* OPT-IN, not opt-out [2026-08-27]: correct at its gate and under a
     * fully-traced Cyberpunk leg (2.08M replays, zero same-handle
     * inversions, zero corruption, every replay before its consumer), the
     * journal still coincides with amdgpu gfx-ring timeouts the moment
     * creates are applied ONLY at the drain -- the double-apply diagnostic
     * (records + live application) runs the full flythrough clean, and
     * FEX_HWTSO=1 does not change the verdict.  Whatever the GPU is
     * consuming stale is not visible in the replay stream, so until that
     * is named the journal stays off unless asked for.  The investigation
     * is written up in NEXT.md item 6. */
    if (!com_env_flag( L"WINEEMUCOMDEVJOURNAL" ))
        return;
    ERR( "WINEEMUCOMDEVJOURNAL=1 -- the device journal is ON (opt-in, "
         "%u slots); see NEXT.md item 6 for why it is not the default\n", count );
    if (com_env_flag( L"WINEEMUNOCOMDEVJOURNAL" ))
    {
        ERR( "WINEEMUNOCOMDEVJOURNAL=1 -- %u device-journal slots stay trapping "
             "on every call\n", count );
        return;
    }
    dev_sabotage = com_env_flag( L"WINEEMUCOMDEVSABOTAGE" );
    if (dev_sabotage)
        ERR( "WINEEMUCOMDEVSABOTAGE=1 -- device rings record and NEVER replay; "
             "descriptor state WILL go stale\n" );
    dev_double = com_env_flag( L"WINEEMUCOMDEVDOUBLE" );
    if (dev_double)
        ERR( "WINEEMUCOMDEVDOUBLE=1 -- snippets record AND fall through to the "
             "trap: every create applies live and again at the drain "
             "(idempotent).  A diagnostic split, not a mode to play under\n" );

    block = NULL;
    size = count * snippet_stride;
    if (NtAllocateVirtualMemory( NtCurrentProcess(), (void **)&block, 0, &size,
                                 MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE ))
    {
        ERR( "no memory for %u device-journal snippets; slots stay trapping\n", count );
        return;
    }

    code = block;
    for (i = 0; i < wc_surface->iface_count; i++)
    {
        const struct winecom_iface *itf = &wc_surface->ifaces[i];
        if (!itf->slots) continue;
        for (n = 0; n < itf->slot_count; n++)
        {
            const struct winecom_slot *sl = &itf->slots[n];
            struct snippet_buf b = { code };
            UINT64 *vslot;
            UINT argc = 0;

            for (j = 0; j < ARRAYSIZE(dev_slot_defs); j++)
                if (sl->name && !strcmp( sl->name, dev_slot_defs[j].name ))
                { argc = dev_slot_defs[j].argc; break; }
            if (!argc) continue;

            /* a forced-refused row keeps TRAPPING: a guest-side journal
             * snippet would record the call and never reach the dispatcher,
             * so the kill switch would not be one */
            if (row_forced_refused( i, n )) continue;

            /* fail closed to trapping if the table row stopped matching the
             * curated shape -- a drifted roster must never produce a wrong
             * record */
            if (sl->refuse || (sl->flags & WINECOM_F_HAND) ||
                !(sl->flags & WINECOM_F_RET_VOID) || sl->argc != argc)
            {
                ERR( "device-journal slot %s does not match its curated shape "
                     "(argc %u, flags %x); it stays trapping\n",
                     sl->name, sl->argc, sl->flags );
                continue;
            }

            vslot = &guest_vtbl_block[iface_slot_base[i] + n];

            sb_emit( &b, dv_pre1, sizeof(dv_pre1) );
            sb_emit_jcc_fb( &b, 0x74 );                   /* jz fb */
            tmp[0] = 0x48; tmp[1] = 0xb8;                 /* movabs rax,MAGIC */
            *(UINT64 *)(tmp + 2) = DEV_RING_MAGIC;
            sb_emit( &b, tmp, 10 );
            tmp[0] = 0x49; tmp[1] = 0x39; tmp[2] = 0x42; tmp[3] = 0x10;
            sb_emit( &b, tmp, 4 );                        /* cmp [r10+0x10],rax */
            sb_emit_jcc_fb( &b, 0x75 );                   /* jne fb: not a ring */
            sb_emit( &b, dv_pre2, sizeof(dv_pre2) );
            sb_emit_jcc_fb( &b, 0x77 );                   /* ja fb */
            sb_emit( &b, dv_body1, sizeof(dv_body1) );
            tmp[0] = 0x41; tmp[1] = 0xc7; tmp[2] = 0x03;  /* mov dword [r11],key */
            *(UINT *)(tmp + 3) = (i << 16) | n;
            sb_emit( &b, tmp, 7 );
            tmp[0] = 0x41; tmp[1] = 0xc7; tmp[2] = 0x43; tmp[3] = 0x04;
            *(UINT *)(tmp + 4) = DEV_REC_BYTES | (DEV_SHAPE_CBV << 24);
            sb_emit( &b, tmp, 8 );                        /* mov dword [r11+4],rec|shape */
            sb_emit( &b, dv_body2, sizeof(dv_body2) );
            tmp[0] = 0x49; tmp[1] = 0xba;                 /* movabs r10,&dev_dirty */
            *(UINT64 *)(tmp + 2) = (UINT64)(ULONG_PTR)&dev_dirty;
            sb_emit( &b, tmp, 10 );
            if (!dev_double) sb_emit( &b, dv_epi, sizeof(dv_epi) );
            else
            {
                sb_emit( &b, dv_epi, sizeof(dv_epi) - 1 );  /* the dirty store, no ret */
                b.fixups[b.nfix++] = b.p + 1;               /* jmp fb: the record was
                                                               taken, now the trap
                                                               serves the call live */
                tmp[0] = 0xeb; tmp[1] = 0;
                sb_emit( &b, tmp, 2 );
            }

            for (k = 0; k < b.nfix; k++)
            {
                LONG_PTR d = b.p - (b.fixups[k] + 1);
                if (d > 127)   /* the earliest jump is ~122 from the fallback:
                                  refuse loudly if this body ever outgrows rel8 */
                {
                    ERR( "device-journal snippet for %s outgrew rel8 (%ld); "
                         "it stays trapping\n", sl->name, (long)d );
                    break;
                }
                *b.fixups[k] = (BYTE)d;
            }
            if (k < b.nfix) continue;
            sb_emit( &b, fb, sizeof(fb) );
            memcpy( b.p, vslot, sizeof(UINT64) );         /* the trap stub */
            b.p += 8;

            TRACE( "devjournal: %s slot %u recorded guest-side from %p "
                   "(fallback stub %p)\n", sl->name, n, code, *(void **)vslot );
            *vslot = (UINT64)(ULONG_PTR)code;
            dev_slot_map[iface_slot_base[i] + n] = 1;
            code += snippet_stride;
        }
    }
    dev_journal_on = TRUE;
}
#else
static void install_dev_journal( void ) { }
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

    /* The guest machine is a property of the process, settled before any
     * guest code could trap: an i386 guest is a WoW64 process. */
    guest32 = NtCurrentTeb()->WowTebOffset != 0;
    if (guest32)
        TRACE( "%s: attaching for an i386 guest (WoW64 process)\n",
               wc_surface->name );

    guest = guest32
        ? find_guest_module32( wc_surface->guest_modules, wc_surface->module_count )
        : find_guest_module( wc_surface->guest_modules, wc_surface->module_count, TRUE );
    if (!guest)
    {
        ERR( "%s: no guest thunk module in this process; COM dispatch "
             "cannot work\n", wc_surface->name );
        return FALSE;
    }
    /* validate every loaded candidate, materialise from the first */
    for (i = 0; i < wc_surface->module_count; i++)
    {
        HMODULE mod = guest32
            ? find_guest_module32( &wc_surface->guest_modules[i], 1 )
            : find_guest_module( &wc_surface->guest_modules[i], 1, TRUE );
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

    if (guest32)
    {
        /* The 32-bit vtable block: 4-byte slot entries, below 4 GiB because
         * the proxies' 4-byte vtable pointers must reach it.  vtbl32s and
         * hand32_map ride the ordinary heap; only what the GUEST addresses
         * needs to be low. */
        RtlEnterCriticalSection( &wc_cs );
        vtbl32_block = wc_alloc_low( total_slots * sizeof(UINT) );
        RtlLeaveCriticalSection( &wc_cs );
        vtbl32s = RtlAllocateHeap( NtCurrentTeb()->Peb->ProcessHeap, 0,
                                   wc_surface->iface_count * sizeof(*vtbl32s) );
        hand32_map = RtlAllocateHeap( NtCurrentTeb()->Peb->ProcessHeap, 0, total_slots );
        if (!vtbl32_block || !vtbl32s || !hand32_map) return FALSE;
        memset( hand32_map, 0xff, total_slots );
    }

    off = 0;
    for (i = 0; i < wc_surface->iface_count; i++)
    {
        iface_slot_base[i] = off;
        guest_vtbls[i] = guest_vtbl_block + off;
        for (n = 0; n < entries[i].slot_count; n++)
            guest_vtbl_block[off + n] = base + entries[i].stubs_rva + n * (UINT64)info->stride;
        if (guest32)
        {
            vtbl32s[i] = vtbl32_block + off;
            for (n = 0; n < entries[i].slot_count; n++)
                vtbl32_block[off + n] = (UINT)guest_vtbl_block[off + n];
            /* match this interface's rows against the surface's 32-bit hand
             * walkers, by slot name -- once, here, never on a dispatch */
            if (wc_surface->hand32 && wc_surface->ifaces[i].slots)
                for (n = 0; n < entries[i].slot_count; n++)
                {
                    const char *sname = wc_surface->ifaces[i].slots[n].name;
                    UINT h;

                    if (!sname) continue;
                    for (h = 0; h < wc_surface->hand32_count; h++)
                        if (!strcmp( sname, wc_surface->hand32[h].slot_name ))
                        {
                            hand32_map[off + n] = (unsigned char)h;
                            break;
                        }
                }
        }
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
    /* BEFORE the three snippet installers: each of them replaces a trap stub
     * with guest-side code, and a forced-refused row must keep trapping or the
     * kill switch never sees the call at all. */
    arm_row_kill_switches();
    install_const_getters();
    install_journal();
    install_dev_journal();
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

    /* The concrete-type upgrade: a caller may legally static_cast a returned
     * base interface to the derived type it knows the object to be, and the
     * proxy's vtable must be long enough for that call to land on a real
     * stub.  Asked BEFORE interning, so one host object interns under its
     * concrete type however it is reached. */
    if (wc_surface->wrap_concrete)
    {
        UINT up = wc_surface->wrap_concrete( host, iface );
        if (up < wc_surface->iface_count && up != iface)
        {
            TRACE( "wrapping %s host %p as its concrete %s\n",
                   wc_surface->ifaces[iface].name, host,
                   wc_surface->ifaces[up].name );
            iface = up;
            bucket = (UINT)(((ULONG_PTR)host >> 4) % INTERN_BUCKETS);
        }
    }

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
    if (!(p = wc_proxy_alloc()))
    {
        RtlLeaveCriticalSection( &wc_cs );
        ERR( "out of memory interning %s %p\n", wc_surface->ifaces[iface].name, host );
        host_release_iface( host, iface );
        return NULL;
    }
    /* For an i386 guest the vtable pointer is the 4-byte-slot block and the
     * proxy itself sits below 4 GiB (wc_proxy_alloc): on this little-endian
     * host the guest's 4-byte load at offset 0 then reads exactly the low
     * half of the same field the native side reads as 8 bytes. */
    p->guest_vtbl = guest32 ? (const void *)vtbl32s[iface] : (const void *)guest_vtbls[iface];
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
    BOOL in_block;
    UINT i;

    if (!ptr) return NULL;
    /* A 32-bit guest object's vtable pointer is 4 bytes, so only 4 bytes of
     * a foreign candidate may be read -- 8 could touch the next field, or
     * for a 4-byte object the next page. */
    if (guest32)
    {
        vtbl = *(const UINT *)cand;
        in_block = (ULONG_PTR)vtbl32_block <= vtbl &&
                   vtbl < (ULONG_PTR)(vtbl32_block + total_slots);
    }
    else
    {
        vtbl = (ULONG_PTR)cand->guest_vtbl;
        in_block = (ULONG_PTR)guest_vtbl_block <= vtbl &&
                   vtbl < (ULONG_PTR)(guest_vtbl_block + total_slots);
    }
    if (in_block)
    {
        /* points into the materialised block: confirm it is an interface
         * base, then confirm the pointer is interned */
        for (i = 0; i < wc_surface->iface_count; i++)
            if (guest32 ? (ULONG_PTR)vtbl32s[i] == vtbl
                        : (const void *)guest_vtbls[i] == cand->guest_vtbl)
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
    wc_dev_drain();
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
        wc_proxy_free( p );
    }
    return refs;
}

/* The PUBLIC form of wc_forward_release/proxy_addref (include/wine/winecom.h),
 * for a sibling module's hand walker over a by-value aggregate that MAY carry
 * one of our forward proxies -- system-com-design.md §9.2's VARIANT/PROPVARIANT
 * case.  A VARIANT's VT_UNKNOWN/VT_DISPATCH slot holds a plain guest-visible
 * pointer, not an argument winecom_dispatch ever classifies on its own, so the
 * walker that clears or copies it needs to drop or take exactly the reference
 * a forward proxy owes WITHOUT ever touching the one host reference the proxy
 * itself owns for its whole life.  That is the whole reason this exists rather
 * than the walker calling winecom_unwrap() + a native IUnknown_AddRef/Release:
 * the host reference belongs to the proxy's intern entry, not to whichever
 * guest-visible reference happens to be getting dropped, and releasing it
 * directly while the proxy still interns it double-frees the day the proxy's
 * OWN guest-visible count later reaches zero.
 *
 * Both are NULL-safe (0 back) and fail closed on a pointer that is not one of
 * our proxies (0 back, no crash) -- but the caller is expected to have already
 * classified the pointer with winecom_translate_in()/winecom_to_native() and
 * to call these only on the "yes, one of ours" branch of that classification;
 * a caller that skips classification and calls this on a guest-implemented
 * object gets a silent no-op, not the release it wanted. */
ULONG winecom_release_guest_seen( void *ptr )
{
    struct com_proxy *p = proxy_from_pointer( ptr );

    return p ? proxy_release( p ) : 0;
}

ULONG winecom_addref_guest_seen( void *ptr )
{
    struct com_proxy *p = proxy_from_pointer( ptr );

    return p ? proxy_addref( p ) : 0;
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

/* -------------------------------------------------- refusal hygiene ------
 *
 * Refused must mean INERT.  Every refusal above answers E_NOTIMPL -- but a
 * void-returning method has no HRESULT anybody reads, and even one that does
 * is routinely unchecked; if the caller's out-params are left untouched, the
 * guest reads its own uninitialized locals, which on a shared stack means
 * HOST-pointer residue.  [MEASURED] The Witcher 3 calls PSGetShader for
 * state snapshots, never checks anything, called through exactly such a
 * residue pointer, and the emulator decoded a native module's ppc64le bytes
 * as x86 until they dereferenced NULL -- then its own crash reporter
 * deadlocked on the wreck.  Days of hunt for one unwritten out-pointer.
 *
 * So every refusal that returns to the guest scrubs what the generator could
 * prove: winecom_slot's scrubptr/scrubdw/scrubq masks (pointer-width NULL /
 * 4-byte zero / 8-byte zero -- the widths are clang-measured, the IN-ness is
 * the COM constness convention; the field banner in winecom.h has the whole
 * argument).  Writing through the guest's own out pointers is what the
 * post-call wrap paths already do, so this adds no new exposure class.
 *
 * WINEEMUNOREFUSESCRUB=1 turns the scrub off -- the negative control that
 * proves it is load-bearing: with it armed, a refused slot's sentinel
 * survives and the hygiene gate goes red. */
static int refuse_scrub_off = -1;

static void scrub_refused_outs( const struct winecom_slot *sl, const UINT64 *rawargs,
                                BOOL is_guest32 )
{
    UINT i;

    if (!sl || !(sl->scrubptr | sl->scrubdw | sl->scrubq)) return;
    if (refuse_scrub_off == -1)
    {
        refuse_scrub_off = com_env_flag( L"WINEEMUNOREFUSESCRUB" );
        if (refuse_scrub_off)
            ERR( "WINEEMUNOREFUSESCRUB=1 -- refusals will NOT scrub their "
                 "out-params; unchecked callers read uninitialized locals, "
                 "which is the sabotage this lever exists to prove\n" );
    }
    if (refuse_scrub_off) return;
    for (i = 1; i < sl->argc && i <= 16; i++)
    {
        UINT bit = 1u << (i - 1);
        ULONG_PTR p = (ULONG_PTR)rawargs[i];

        if (!p) continue;
        if (sl->scrubptr & bit)
        {
            /* pointer-width pointee: (8,4) on the (x86-64, i386) guest */
            if (is_guest32) *(UINT *)p = 0;
            else *(UINT64 *)p = 0;
        }
        else if (sl->scrubdw & bit) *(UINT *)p = 0;
        else if (sl->scrubq & bit) *(UINT64 *)p = 0;
    }
}

HRESULT winecom_wrap_out_iface( HRESULT hr, const GUID *riid, void **ppv )
{
    UINT idx;

    if (FAILED(hr) || !ppv || !*ppv) return hr;
    idx = riid ? winecom_iface_from_iid( riid ) : ~0u;
    /* WINEEMUNOCOMIIDS: a listed IID is treated as if the roster had never
     * gained it, which is the release-and-NULL the guest got before the
     * syscom wave -- {77aa99a0} IAudioSessionManager2 is the one the Witcher 3
     * bisect names.  Deliberately here and not in winecom_iface_from_iid:
     * this is the choke point where an interface is HANDED OUT, and blocking
     * the lookup itself would also break QueryInterface on objects the guest
     * already holds, which is not what the pre-landing world did. */
    if (idx != ~0u && blocked_iid_count && riid && iid_is_blocked( riid ))
    {
        ERR( "%s: WINEEMUNOCOMIIDS lists %s -- treating it as unrostered, "
             "releasing the object and answering E_NOINTERFACE\n",
             wc_surface->name, debugstr_guid(riid) );
        idx = ~0u;
    }
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
                                   const UINT64 *rawargs, UINT64 *rax_out, UINT64 *fpret_bits )
{
    UINT64 args[16] = { 0 };
    UINT64 arr_buf[32];
    UINT64 *arr_heap = NULL;
    UINT64 ret;
    UINT i, n, ppv_idx = 0, riid_idx = 0;
    UINT out_static_idx[8], n_out_static = 0;
    UINT out_arr_idx[8], n_out_arr = 0;
    /* the count-through-pointer out arrays (CA_IFACE_ARR_OUT_COUNTPTR): the
     * capacity is SNAPSHOTTED here before the call, because the callee
     * overwrites the same UINT with the actual count -- which D3D11 lets
     * exceed capacity, while exactly capacity cells were written */
    UINT out_carr_idx[4], out_carr_cap[4], n_out_carr = 0;
    /* events minted for this call (max two HANDLE args anywhere in the
     * surface); reaped below if the callee FAILS -- a failing callee kept
     * nothing */
    UINT64 minted[2];
    UINT n_minted = 0;
    UINT borrowed[16], n_borrowed = 0;
    const UINT64 *arr_borrowed = NULL;
    UINT n_arr_borrowed = 0;
    BOOL have_ppv = FALSE;
    /* guest32 out-parameter staging: a 32-bit guest's interface-out cell is
     * FOUR bytes, and both the unixlib below and winecom_wrap write eight --
     * so every such parameter is redirected to a native-width local here and
     * narrowed back to the guest's cell after the wrap.  arr_out_buf serves
     * the out-ARRAY class the same way (64 elements covers every out-array
     * this surface has -- OMGet* family, 8-ish; a larger one refuses). */
    void *ppv_local = NULL;
    void *out_static_local[8];
    void *arr_out_buf[64];
    UINT arr_out_used = 0;
    UINT arr_out_base[8];

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
                scrub_refused_outs( sl, rawargs, guest32 );
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
            {
                if (guest32)
                {
                    out_static_local[n_out_static] = NULL;
                    args[i] = (UINT64)(ULONG_PTR)&out_static_local[n_out_static];
                }
                out_static_idx[n_out_static++] = i;
            }
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
                scrub_refused_outs( sl, rawargs, guest32 );
                *rax_out = (UINT)E_NOTIMPL;
                return STATUS_SUCCESS;
            }
            if (raw && n_out_arr < ARRAYSIZE(out_arr_idx))
            {
                if (guest32)
                {
                    UINT cnt = (UINT)rawargs[sl->caux[i - 1] + 1];

                    if (arr_out_used + cnt > ARRAYSIZE(arr_out_buf))
                    {
                        refuse_once( iface, slot, sl->name,
                                     "interface out-array larger than the "
                                     "32-bit lane's staging buffer" );
                        if (arr_heap) RtlFreeHeap( NtCurrentTeb()->Peb->ProcessHeap, 0, arr_heap );
                        release_borrows( args, borrowed, n_borrowed );
                        scrub_refused_outs( sl, rawargs, guest32 );
                        *rax_out = (UINT)E_NOTIMPL;
                        return STATUS_SUCCESS;
                    }
                    memset( &arr_out_buf[arr_out_used], 0, cnt * sizeof(void *) );
                    arr_out_base[n_out_arr] = arr_out_used;
                    args[i] = (UINT64)(ULONG_PTR)&arr_out_buf[arr_out_used];
                    arr_out_used += cnt;
                }
                out_arr_idx[n_out_arr++] = i;
            }
            break;
        case WINECOM_CA_IFACE_ARR_OUT_COUNTPTR:
            /* The XSGetShader shape: an out interface array whose element
             * count arrives through a UINT* (caux[i-1]) -- IN the capacity,
             * OUT the actual count.  The callee writes the guest's own
             * storage in place, exactly the ARR_OUT_STATIC arrangement, and
             * [READ FROM DXVK, GetClassInstances] fills exactly CAPACITY
             * cells -- real instances then NULL padding -- before storing
             * the actual count; with a NULL count pointer it touches
             * NOTHING, the array included.  So: snapshot capacity now (the
             * callee overwrites the same UINT), wrap capacity cells after.
             * The 32-bit lane never reaches this class (the generator marks
             * these rows refuse32); refuse rather than guess if it ever
             * does. */
            args[i] = raw;
            if (!sl->caux || guest32)
            {
                refuse_once( iface, slot, sl->name, guest32 ?
                             "count-through-pointer out-array has no 32-bit "
                             "staging; the generator should have said refuse32" :
                             "count-through-pointer out-array with no caux "
                             "count-parameter table; the generator must emit one" );
                if (arr_heap) RtlFreeHeap( NtCurrentTeb()->Peb->ProcessHeap, 0, arr_heap );
                release_borrows( args, borrowed, n_borrowed );
                scrub_refused_outs( sl, rawargs, guest32 );
                *rax_out = (UINT)E_NOTIMPL;
                return STATUS_SUCCESS;
            }
            if (raw && n_out_carr < ARRAYSIZE(out_carr_idx))
            {
                const UINT *cp = (const UINT *)(ULONG_PTR)rawargs[sl->caux[i - 1] + 1];

                if (cp && *cp)
                {
                    out_carr_idx[n_out_carr] = i;
                    out_carr_cap[n_out_carr++] = *cp;
                }
            }
            break;
        case WINECOM_CA_PPV_OUT:
            args[i] = raw;
            if (guest32 && raw)
            {
                ppv_local = NULL;
                args[i] = (UINT64)(ULONG_PTR)&ppv_local;
            }
            have_ppv = TRUE;
            ppv_idx = i;
            riid_idx = sl->aux + 1;
            break;
        case WINECOM_CA_EVENT:
        case WINECOM_CA_EVENT_ONESHOT:
            if (raw)
            {
                /* A real Wine event crosses through the surface's mint hook
                 * into the tagged-eventfd encoding the native side's sync
                 * convention understands (the vkd3d/dxvk 'EVFD' tag; the
                 * relay behind the hook signals the guest event on payout).
                 * No hook -- or the negative-control lever -- refuses
                 * exactly as the pre-relay runtime did. */
                static int event_off = -1;
                UINT64 native;

                if (event_off == -1)
                {
                    event_off = com_env_flag( L"WINEEMUNOCOMEVENT" );
                    if (event_off)
                        ERR( "WINEEMUNOCOMEVENT=1 -- every non-NULL event "
                             "argument refuses; the relay is off\n" );
                }
                if (event_off || !wc_surface->event_mint ||
                    !(native = wc_surface->event_mint( raw,
                          sl->cls[i - 1] == WINECOM_CA_EVENT_ONESHOT )))
                {
                    refuse_once( iface, slot, sl->name,
                                 "non-NULL event with no relay on this "
                                 "surface (no event_mint hook, the mint "
                                 "failed, or WINEEMUNOCOMEVENT=1)" );
                    if (arr_heap) RtlFreeHeap( NtCurrentTeb()->Peb->ProcessHeap, 0, arr_heap );
                    release_borrows( args, borrowed, n_borrowed );
                    scrub_refused_outs( sl, rawargs, guest32 );
                    *rax_out = (UINT)E_NOTIMPL;
                    return STATUS_SUCCESS;
                }
                args[i] = native;
                if (n_minted < ARRAYSIZE(minted)) minted[n_minted++] = native;
                break;
            }
            args[i] = 0;
            break;
        case WINECOM_CA_IFACE_ARR_IN:
        {
            UINT count = (UINT)rawargs[sl->aux2 + 1];
            void *const *src = (void *const *)(ULONG_PTR)raw;
            const UINT *src32 = (const UINT *)(ULONG_PTR)raw;
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
                scrub_refused_outs( sl, rawargs, guest32 );
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
                void *elem = guest32 ? (void *)(ULONG_PTR)src32[n] : src[n];
                if (!winecom_to_native( elem,
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
                    scrub_refused_outs( sl, rawargs, guest32 );
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
            scrub_refused_outs( sl, rawargs, guest32 );
            *rax_out = (UINT)E_NOTIMPL;
            return STATUS_SUCCESS;
        }
    }

    if (sl->fpmask || sl->fpret)
    {
        /* A slot whose signature carries by-value floats: the widest-integer
         * invoker cannot place them, so this goes through the surface's
         * FLOATING-POINT invoker (PPC64EC step C) with the flat lane's fpword
         * encoding.  Three ways to fail, all CLOSED and all named: a surface
         * with no FP invoker, the WINEEMUNOCOMFP=1 negative control, and a
         * path with no FP-return plumbing (the journal drains and the 32-bit
         * dispatch pass fpret_bits NULL -- an fp-marked row must never be
         * journaled, and the 32-bit lane's generator marks these rows
         * refuse32, so reaching here from either is a defect this refusal
         * makes loud instead of silent). */
        static int fp_off = -1;

        if (fp_off == -1)
        {
            fp_off = com_env_flag( L"WINEEMUNOCOMFP" );
            if (fp_off)
                ERR( "WINEEMUNOCOMFP=1 -- every float-bearing slot refuses; "
                     "the FP invoker is off\n" );
        }
        if (fp_off || !wc_surface->invoke_fp || !fpret_bits)
        {
            refuse_once( iface, slot, sl->name,
                         !fpret_bits ? "floating-point slot reached through a "
                                       "path with no FP plumbing (journal "
                                       "replay or the 32-bit dispatch)"
                                     : "float-bearing slot with no FP invoker "
                                       "on this surface (or WINEEMUNOCOMFP=1)" );
            release_borrows( args, borrowed, n_borrowed );
            for (n = 0; n < n_arr_borrowed; n++)
                winecom_to_native_end( (void *)(ULONG_PTR)arr_borrowed[n] );
            if (arr_heap) RtlFreeHeap( NtCurrentTeb()->Peb->ProcessHeap, 0, arr_heap );
            scrub_refused_outs( sl, rawargs, guest32 );
            *rax_out = (UINT)E_NOTIMPL;
            return STATUS_SUCCESS;
        }
        ret = wc_surface->invoke_fp( proxy->host, slot, sl->argc, args,
                                     (UINT)sl->fpmask |
                                     ((UINT)(sl->fpmask & (unsigned char)~sl->fpwide) << 8) |
                                     ((UINT)sl->fpret << 16),
                                     fpret_bits );
    }
    else ret = wc_surface->invoke( proxy->host, slot, sl->argc, args );

    /* A FAILED event-bearing call kept nothing: reap what was minted.  The
     * HRESULT reading is exact for every event-bearing row in the tables --
     * all of them return HRESULT or void (Flush1), and a void return is
     * "kept" by definition.  RET_VIA_ARG rows carry no event args. */
    if (n_minted && wc_surface->event_reap &&
        !(sl->flags & (WINECOM_F_RET_VOID | WINECOM_F_RET_VIA_ARG)) &&
        FAILED((HRESULT)(UINT)ret))
    {
        for (n = 0; n < n_minted; n++) wc_surface->event_reap( minted[n] );
        n_minted = 0;
    }

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
    /* The count-through-pointer arrays, by the SNAPSHOTTED capacity: the
     * count parameter now holds the callee's ACTUAL count, which D3D11 lets
     * exceed the capacity, while exactly capacity cells were written --
     * real interfaces then NULL padding (DXVK's GetClassInstances).  Same
     * whatever-the-return rule as above: these too are void readbacks. */
    for (n = 0; n < n_out_carr; n++)
    {
        UINT idx = out_carr_idx[n];
        void **out = (void **)(ULONG_PTR)args[idx];
        UINT k;

        for (k = 0; k < out_carr_cap[n]; k++)
            if (out[k]) out[k] = winecom_wrap( out[k], sl->xaux[idx - 1] );
    }

    /* The other half of the guest32 staging above: everything the callee
     * wrote -- now wrapped -- is narrowed into the guest's own 4-byte cells.
     * On failure a cell gets NULL, never whatever host pointer the local may
     * hold: a raw host pointer in a guest cell is the defect this runtime
     * exists to prevent. */
    if (guest32)
    {
        if (have_ppv && rawargs[ppv_idx])
            *(UINT *)(ULONG_PTR)rawargs[ppv_idx] =
                SUCCEEDED((HRESULT)ret) ? (UINT)(ULONG_PTR)ppv_local : 0;
        for (n = 0; n < n_out_static; n++)
            *(UINT *)(ULONG_PTR)rawargs[out_static_idx[n]] =
                SUCCEEDED((HRESULT)ret) ? (UINT)(ULONG_PTR)out_static_local[n] : 0;
        for (n = 0; n < n_out_arr; n++)
        {
            UINT idx = out_arr_idx[n];
            UINT count = (UINT)rawargs[sl->caux[idx - 1] + 1];
            UINT *gout = (UINT *)(ULONG_PTR)rawargs[idx];
            UINT k;

            for (k = 0; k < count; k++)
                gout[k] = (UINT)(ULONG_PTR)arr_out_buf[arr_out_base[n] + k];
        }
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
     * Release.  This is what makes the recorded order the issued order.
     * Device rings first: a replayed list command may depend on a
     * descriptor create recorded on another thread. */
    wc_dev_drain();
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
    /* WINEEMUNOCOMROWS/WAVE: a served row put back to the refusal it had
     * before the completeness landings.  Same path as a generated refusal,
     * scrub included -- the whole point is that the guest cannot tell which
     * kind it hit, only the log can. */
    if (!sl || sl->refuse || row_forced_refused( iface, slot ))
    {
        refuse_once( iface, slot, sl ? sl->name : NULL,
                     sl && !sl->refuse ? forced_why : sl ? sl->refuse : NULL );
        /* refusal hygiene: a generation-refused row still carries the scrub
         * masks the generator derived from its signature, so the caller's
         * out-params are made NULL/0 rather than left as the uninitialized
         * locals an unchecked caller reads back (see scrub_refused_outs).
         * Only the masked positions are read out of the trap frame. */
        if (sl && (sl->scrubptr | sl->scrubdw | sl->scrubq))
        {
            UINT64 rawargs[16] = { 0 };
            UINT i;

            for (i = 1; i < sl->argc && i < 16; i++)
                if ((sl->scrubptr | sl->scrubdw | sl->scrubq) & (1u << (i - 1)))
                    rawargs[i] = winecom_read_arg( ctx, i );
            scrub_refused_outs( sl, rawargs, FALSE );
        }
        ctx->Rax = (UINT)E_NOTIMPL;
        return STATUS_SUCCESS;
    }

    TRACE( "%s (iface %u slot %u argc %u)\n", sl->name, iface, slot, sl->argc );

    /* a device-journaled slot trapping means the caller has no ring yet
     * (or its ring is full): arm this thread so the NEXT call records */
    if (dev_journal_on && dev_slot_map[iface_slot_base[iface] + slot])
        wc_dev_arm();

    if (sl->flags & WINECOM_F_HAND)
    {
        ctx->Rax = wc_surface->hand_funcs[sl->aux]( proxy->host, slot, ctx );
        return STATUS_SUCCESS;
    }

    {
        UINT64 rawargs[16] = { 0 }, rax = 0, fpret_bits = 0;
        NTSTATUS st;

        for (i = 1; i < sl->argc && i < 16; i++) rawargs[i] = winecom_read_arg( ctx, i );
        if (sl->fpmask)
        {
            /* A floating-point argument in an XMM position: the loop above
             * read this position's INTEGER slot, which MS-x64 leaves
             * UNDEFINED when the value travelled in XMM1..XMM3 (positions 0-3
             * are register slots, and `this` owns position 0).  Lift the raw
             * bits from the register file instead -- materialize first, the
             * lazy-context contract, same as every FP hand walker.  Overall
             * positions 4 and up travel on the stack whatever their type, so
             * winecom_read_arg already produced the right bits (a float in
             * the slot's low four bytes -- exactly the shape invoke_fp
             * consumes). */
            __wine_emu_materialize_ctx( ctx );
            for (i = 1; i < sl->argc && i < 4; i++)
                if (sl->fpmask & (1u << (i - 1)))
                    rawargs[i] = ctx->FltSave.XmmRegisters[i].Low;
        }
        st = invoke_marshalled( itf, sl, proxy, iface, slot, rawargs, &rax, &fpret_bits );
        if (st == STATUS_SUCCESS)
        {
            if (sl->fpret)
            {
                /* MS-x64 returns a float or double in XMM0, not RAX.  Write
                 * the WHOLE register (the flat FP path's rule, same reason:
                 * stale high bytes from a previous call would be visible to
                 * a guest reading wider than the callee wrote), and
                 * materialize first -- a write to a still-lazy FP group is
                 * ignored at resume.  fpret_bits carries f1's double-format
                 * bits; a float return narrows here, once. */
                union { UINT64 bits; double d; } v;

                __wine_emu_materialize_ctx( ctx );
                v.bits = fpret_bits;
                memset( &ctx->FltSave.XmmRegisters[0], 0, sizeof(ctx->FltSave.XmmRegisters[0]) );
                if (sl->fpret == 2) *(float *)&ctx->FltSave.XmmRegisters[0] = (float)v.d;
                else *(double *)&ctx->FltSave.XmmRegisters[0] = v.d;
            }
            ctx->Rax = rax;
        }
        return st;
    }
}

/* ------------------------------------------------------ the 32-bit dispatch */

BOOL winecom_guest32(void)
{
    /* a property of the PROCESS, not of the attach: client code narrows its
     * guest-cell writes by this answer on paths that run before -- or
     * without -- a successful attach (a refusal stub still NULLs its out
     * parameters), so it must not read attach state */
    return NtCurrentTeb()->WowTebOffset != 0;
}

void winecom_store_guest_ptr( void *cell, void *value )
{
    if (winecom_guest32()) *(UINT *)cell = (UINT)(ULONG_PTR)value;
    else *(void **)cell = value;
}

/***********************************************************************
 *           winecom_dispatch32
 *
 * The i386 twin of winecom_dispatch, over a stdcall frame instead of MS-x64
 * registers: [Esp] the return address, [Esp+4] `this`, parameters in 4-byte
 * slots from [Esp+8], 8-byte parameters (qwordmask) in two consecutive
 * slots, and the CALLEE pops -- which is why the contract with ntdll differs
 * from the 64-bit one: this side performs the whole epilogue (Eax, Edx for
 * an EDX:EAX return, Eip from the popped return address, Esp past the
 * frame), because the pop arithmetic is per-slot knowledge only the marshal
 * table has.  Rows are served only under BOTH version stamps:
 * WINECOM_F_I386_GEOM (the frame is decodable and poppable) and
 * WINECOM_F_I386_STRUCTS_OK (every pointer parameter's pointee was audited
 * against the i386 layouts, divergent ones described in reps[]).  A row
 * with geometry but without a serve path pops correctly and answers
 * E_NOTIMPL; a row without geometry cannot even do that and the trap
 * faults, loudly.
 */
static void pop_frame32( I386_CONTEXT *ctx, UINT bytes )
{
    const ULONG *esp = (const ULONG *)(ULONG_PTR)ctx->Esp;

    ctx->Eip = esp[0];
    ctx->Esp += 4 + bytes;
}

NTSTATUS winecom_dispatch32( UINT iface, UINT slot, I386_CONTEXT *ctx )
{
    const struct winecom_iface *itf;
    const struct winecom_slot *sl;
    const ULONG *esp;
    struct com_proxy *proxy;
    UINT pop_bytes, i, cur;
    UINT64 rawargs[16] = { 0 }, rax = 0;
    NTSTATUS st;

    /* rep staging: native temporaries for divergent-layout struct params */
    struct
    {
        const struct winecom_rep *rep;
        void *guest;
        void *native;
        UINT count;
        BOOL heap;
    } fixes[8];
    UINT n_fixes = 0;
    char repbuf[1024];
    UINT rep_used = 0;

    if (!com_ready()) return STATUS_DLL_INIT_FAILED;
    if (!guest32)
    {
        ERR( "%s: 32-bit dispatch in a 64-bit-guest process\n", wc_surface->name );
        return STATUS_INVALID_PARAMETER;
    }
    if (iface >= wc_surface->iface_count) return STATUS_INVALID_PARAMETER;
    itf = &wc_surface->ifaces[iface];
    if (slot >= itf->slot_count) return STATUS_INVALID_PARAMETER;

    esp = (const ULONG *)(ULONG_PTR)ctx->Esp;
    proxy = (struct com_proxy *)(ULONG_PTR)esp[1];

    /* IUnknown's three slots head every vtable; their frames are fixed by
     * IUnknown itself (QI: riid + ppv = two slots; AddRef/Release: none). */
    if (slot < 3)
    {
        if (itf->flags & WINECOM_IF_LOCAL)
        {
            ERR( "%s is [local]; its slot %u is a real method the client must "
                 "claim before dispatch\n", itf->name, slot );
            return STATUS_ILLEGAL_INSTRUCTION;
        }
        if (!proxy)
        {
            ERR( "%s slot %u called with NULL this\n", itf->name, slot );
            ctx->Eax = (UINT)E_INVALIDARG;
            pop_frame32( ctx, slot == 0 ? 12 : 4 );
            return STATUS_SUCCESS;
        }
        switch (slot)
        {
        case 0:
        {
            void *local = NULL;
            HRESULT hr = proxy_qi( proxy, (const GUID *)(ULONG_PTR)esp[2], &local );

            if (FAILED(hr))
                WARN( "QI failed %08x: caller %08x this %08x riid %08x ppv %08x\n",
                      (UINT)hr, esp[0], esp[1], esp[2], esp[3] );
            if (esp[3]) *(UINT *)(ULONG_PTR)esp[3] = (UINT)(ULONG_PTR)local;
            ctx->Eax = (UINT)hr;
            pop_frame32( ctx, 12 );
            return STATUS_SUCCESS;
        }
        case 1:
            ctx->Eax = proxy_addref( proxy );
            pop_frame32( ctx, 4 );
            return STATUS_SUCCESS;
        case 2:
            ctx->Eax = proxy_release( proxy );
            pop_frame32( ctx, 4 );
            return STATUS_SUCCESS;
        }
    }

    sl = itf->slots ? &itf->slots[slot] : NULL;
    if (!sl)
    {
        /* an identity row past IUnknown: no slot table, no frame knowledge,
         * no honest pop -- the fault names the slot */
        ERR( "%s slot %u has no marshal row; cannot even pop its frame\n",
             itf->name, slot );
        return STATUS_ILLEGAL_INSTRUCTION;
    }
    if (!(sl->flags & WINECOM_F_I386_GEOM))
    {
        refuse_once( iface, slot, sl->name,
                     "no i386 frame geometry (x87 return, or a table "
                     "predating the i386 oracle); the frame cannot be popped" );
        return STATUS_ILLEGAL_INSTRUCTION;
    }
    pop_bytes = 4 * sl->argc + 4 * (UINT)__builtin_popcount( sl->qwordmask );

    if (!proxy)
    {
        ERR( "%s slot %u called with NULL this\n", itf->name, slot );
        ctx->Eax = (UINT)E_INVALIDARG;
        pop_frame32( ctx, pop_bytes );
        return STATUS_SUCCESS;
    }
    if (proxy->iface != iface)
        WARN( "proxy %p says iface %u (%s), stub says %u (%s)\n", proxy,
              proxy->iface, wc_surface->ifaces[proxy->iface].name, iface, itf->name );

    TRACE( "%s (iface %u slot %u argc %u) [i386]\n", sl->name, iface, slot, sl->argc );

    /* a 32-bit hand walker, matched by name at attach, serves the row before
     * any other disposition -- including a 64-bit refusal, which is about
     * the OTHER lane's marshalling.  A row the KILL SWITCH names is the one
     * exception: the lever is asked about the row, not about a lane. */
    if (hand32_map && hand32_map[iface_slot_base[iface] + slot] != 0xff &&
        !row_forced_refused( iface, slot ))
    {
        UINT64 r = wc_surface->hand32[hand32_map[iface_slot_base[iface] + slot]]
                       .fn( proxy->host, slot, ctx );

        ctx->Eax = (UINT)r;
        if (sl->flags & WINECOM_F_RET_QWORD) ctx->Edx = (UINT)(r >> 32);
        pop_frame32( ctx, pop_bytes );
        return STATUS_SUCCESS;
    }

    if (sl->refuse || sl->refuse32 || row_forced_refused( iface, slot ))
    {
        refuse_once( iface, slot, sl->name,
                     sl->refuse ? sl->refuse :
                     row_forced_refused( iface, slot ) ? forced_why : sl->refuse32 );
        /* refusal hygiene, the 32-bit spelling: the scrub masks index
         * parameters the same way, but the values come from the stdcall
         * frame -- [esp]=return, [esp+4]=this, then each parameter one
         * 4-byte slot, two where qwordmask says so (the same arithmetic
         * pop_frame32 popped by).  A guest cell is 4 bytes whatever the
         * mask class, except scrubq's genuine 8-byte pointee. */
        if (sl->scrubptr | sl->scrubdw | sl->scrubq)
        {
            UINT64 rawargs[16] = { 0 };
            UINT i, off = 8;   /* past the return address and `this` */

            for (i = 1; i < sl->argc && i < 16; i++)
            {
                if ((sl->scrubptr | sl->scrubdw | sl->scrubq) & (1u << (i - 1)))
                    rawargs[i] = *(const UINT *)(ULONG_PTR)(ctx->Esp + off);
                off += (sl->qwordmask & (1u << (i - 1))) ? 8 : 4;
            }
            scrub_refused_outs( sl, rawargs, TRUE );
        }
        ctx->Eax = (UINT)E_NOTIMPL;
        pop_frame32( ctx, pop_bytes );
        return STATUS_SUCCESS;
    }
    if (sl->flags & WINECOM_F_HAND)
    {
        refuse_once( iface, slot, sl->name,
                     "hand-written on the 64-bit lane with no 32-bit walker yet" );
        ctx->Eax = (UINT)E_NOTIMPL;
        pop_frame32( ctx, pop_bytes );
        return STATUS_SUCCESS;
    }
    if (!(sl->flags & WINECOM_F_I386_STRUCTS_OK))
    {
        refuse_once( iface, slot, sl->name,
                     "pointer parameters not audited against i386 layouts; "
                     "refusing rather than passing a divergent struct raw" );
        ctx->Eax = (UINT)E_NOTIMPL;
        pop_frame32( ctx, pop_bytes );
        return STATUS_SUCCESS;
    }

    /* the frame: rawargs[i] is parameter i exactly as the 64-bit lane's
     * winecom_read_arg would have produced it */
    for (i = 1, cur = 2; i < sl->argc && i < 16; i++)
    {
        if (sl->qwordmask & (1u << (i - 1)))
        {
            rawargs[i] = esp[cur] | ((UINT64)esp[cur + 1] << 32);
            cur += 2;
        }
        else rawargs[i] = esp[cur++];
    }

    /* divergent-layout struct parameters: repack into native temporaries
     * (and back, for the out direction) around the one real call.  More
     * reps than fixes[] can hold would mean silently passing the surplus
     * through raw -- refuse instead; no generated row comes close. */
    if (sl->rep_count > ARRAYSIZE(fixes))
    {
        refuse_once( iface, slot, sl->name,
                     "more repacked parameters than the dispatcher stages" );
        ctx->Eax = (UINT)E_NOTIMPL;
        pop_frame32( ctx, pop_bytes );
        return STATUS_SUCCESS;
    }
    for (i = 0; i < sl->rep_count && n_fixes < ARRAYSIZE(fixes); i++)
    {
        const struct winecom_rep *r = &sl->reps[i];
        UINT64 raw = rawargs[r->param + 1];
        UINT count, bytes;
        BOOL heap = FALSE;
        void *buf;

        if (!raw) continue;
        count = r->count_param == 0xff ? 1 : (UINT)rawargs[r->count_param + 1];
        if (!count) continue;
        bytes = (r->size64 * count + 15) & ~15u;
        if (rep_used + bytes <= sizeof(repbuf))
        {
            buf = repbuf + rep_used;
            rep_used += bytes;
        }
        else if ((heap = TRUE, !(buf = RtlAllocateHeap( NtCurrentTeb()->Peb->ProcessHeap, 0, bytes ))))
        {
            while (n_fixes--)
                if (fixes[n_fixes].heap)
                    RtlFreeHeap( NtCurrentTeb()->Peb->ProcessHeap, 0, fixes[n_fixes].native );
            ctx->Eax = (UINT)E_OUTOFMEMORY;
            pop_frame32( ctx, pop_bytes );
            return STATUS_SUCCESS;
        }
        fixes[n_fixes].rep = r;
        fixes[n_fixes].guest = (void *)(ULONG_PTR)raw;
        fixes[n_fixes].native = buf;
        fixes[n_fixes].count = count;
        fixes[n_fixes].heap = heap;
        n_fixes++;
        if (r->dir & 1)
        {
            UINT k;
            for (k = 0; k < count; k++)
                r->to_native( (char *)buf + (SIZE_T)k * r->size64,
                              (const char *)(ULONG_PTR)raw + (SIZE_T)k * r->size32 );
        }
        else memset( buf, 0, r->size64 * (SIZE_T)count );
        rawargs[r->param + 1] = (UINT64)(ULONG_PTR)buf;
    }

    st = invoke_marshalled( itf, sl, proxy, iface, slot, rawargs, &rax, NULL );

    for (i = 0; i < n_fixes; i++)
    {
        if (st == STATUS_SUCCESS && (fixes[i].rep->dir & 2))
        {
            UINT k;
            for (k = 0; k < fixes[i].count; k++)
                fixes[i].rep->to_guest(
                    (char *)fixes[i].guest + (SIZE_T)k * fixes[i].rep->size32,
                    (const char *)fixes[i].native + (SIZE_T)k * fixes[i].rep->size64 );
        }
        if (fixes[i].heap)
            RtlFreeHeap( NtCurrentTeb()->Peb->ProcessHeap, 0, fixes[i].native );
    }

    if (st != STATUS_SUCCESS) return st;
    /* an sret row answers the CALLER'S buffer pointer in EAX; if that
     * parameter was repacked, rax holds the native temporary -- hand back
     * the guest's own pointer, which the copy-back above just filled */
    if ((sl->flags & WINECOM_F_RET_VIA_ARG))
        for (i = 0; i < n_fixes; i++)
            if ((UINT64)(ULONG_PTR)fixes[i].native == rax)
                rax = (UINT64)(ULONG_PTR)fixes[i].guest;
    ctx->Eax = (UINT)rax;
    if (sl->flags & WINECOM_F_RET_QWORD) ctx->Edx = (UINT)(rax >> 32);
    pop_frame32( ctx, pop_bytes );
    return STATUS_SUCCESS;
}

/* ------------------------------------------------ pointer-width narrowing
 *
 * The one reporting hook the generated 32<->64 repack header needs.  That
 * header (ppc64le/thunks/gen_repack32.py) deliberately includes nothing but
 * <string.h> so that a marshalling module can consume it without pulling in
 * any D3D header, which leaves it no way to say anything.  Every consumer of
 * it imports this library, so the saying happens here.
 *
 * Called at most once per module, from wine_repack64_PTRWIDTH, when a
 * pointer-width OUT value will not fit the guest's four-byte cell.  The rep
 * has already written zero: a truncated handle names a DIFFERENT object,
 * which is worse than none, and "none" is a value every caller of these
 * methods already handles.
 */
void wine_repack32_ptrwidth_lost( unsigned long long v )
{
    ERR( "a pointer-width OUT parameter answered %#llx, whose high half a "
         "32-bit guest cell cannot hold; reported NONE rather than a "
         "truncated value that would name a different object\n", v );
}
