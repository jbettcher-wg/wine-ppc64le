/*
 * Ntdll Unix interface
 *
 * Copyright (C) 2020 Alexandre Julliard
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301, USA
 */

#ifndef __NTDLL_UNIXLIB_H
#define __NTDLL_UNIXLIB_H

#include "wine/unixlib.h"
#include "emu_xstat.h"

struct _DISPATCHER_CONTEXT;

struct wine_dbg_write_params
{
    const char  *str;
    unsigned int len;
};

struct wine_server_fd_to_handle_params
{
    int          fd;
    unsigned int access;
    unsigned int attributes;
    HANDLE      *handle;
};

struct wine_server_handle_to_fd_params
{
    HANDLE        handle;
    unsigned int  access;
    int          *unix_fd;
    unsigned int *options;
};

struct wine_spawnvp_params
{
    char       **argv;
    int          wait;
};

struct load_so_dll_params
{
    UNICODE_STRING              nt_name;
    void                      **module;
};

struct unwind_builtin_dll_params
{
    ULONG                       type;
    struct _DISPATCHER_CONTEXT *dispatch;
    CONTEXT                    *context;
};

/* Runs an x86-64 guest entry point through the embedded emulator and returns
 * when the guest returns to its caller.  ppc64le-only: the native machine
 * cannot execute the main image's code, and the WoW64 machinery is 32-on-64
 * by construction, so a 64-bit guest image is dispatched here instead —
 * from RtlUserThreadStart, the last common chokepoint before the first guest
 * instruction would run.  See dlls/ntdll/signal_ppc64.c. */
struct emu_run_entry_params
{
    void      *entry;    /* guest entry point (MS-x64: arg in RCX) */
    void      *arg;      /* single argument (PEB for the main thread) */
    ULONGLONG  retval;   /* out: guest RAX when the entry returned */
    /* PE-side entry for a guest trap.  Resolving the call needs the module
     * list and the export tables, which only the PE loader has, and the
     * functions it dispatches to are Win32 code -- so it is entered on the
     * Win32 stack through call_user_mode_callback() and returns through
     * NtCallbackReturn, with the same (id, args, len) shape as
     * KiUserCallbackDispatcher.  args holds one pointer: the AMD64 CONTEXT the
     * emulator marshalled, whose Rip the dispatcher owns.  Returning
     * STATUS_SUCCESS resumes the guest, anything else ends the run.  See
     * emu_trap_dispatch() in dlls/ntdll/signal_ppc64.c. */
    void     (*trap_dispatcher)( ULONG id, void *args, ULONG len );
    /* out: the guest called ExitThread rather than returning.  The run is
     * ended from inside the trap dispatch (see emu_ExitThread) instead of
     * letting native ExitThread reach pthread_exit with a live fexbridge_run
     * and JIT frames of unknown CFI quality below it on the kernel stack --
     * the forced unwind across those is the empty-FDE hazard class, which
     * fails as an invisible spinning core rather than a crash. */
    BOOL       exit_requested;
    ULONG      exit_code;
    /* PE-side entry for a guest exception (fault or raise), entered exactly
     * like trap_dispatcher; args holds one pointer to a
     * struct emu_exception_params.  Returning STATUS_SUCCESS resumes the
     * guest with the (possibly handler-edited) CONTEXT; anything else ends
     * the run with STATUS_EMU_GUEST_EXCEPTION, the record having been
     * surfaced PE-side by the dispatcher itself (see emu_exception_dispatch
     * in dlls/ntdll/signal_ppc64.c).  NULL restores the old behaviour: a
     * guest fault fails the run with no dispatch of any kind. */
    void     (*exception_dispatcher)( ULONG id, void *args, ULONG len );
    /* out: the run's guest stack, handed to the PE side UNFREED when the run
     * ended on STATUS_EMU_GUEST_EXCEPTION.  Both flavors of that ending need
     * the stack to outlive the run: an unwind request's EXCEPTION_RECORD may
     * carry POINTERS into it (MSVC's FH4 passes the catch establisher as a
     * pointer to a personality-run local -- measured, GfnRuntimeSdk), and a
     * fault report is worthless over a freed stack.  The PE side frees it
     * when the unwind that needed it completes, or immediately when nothing
     * does; see call_guest_function() in signal_ppc64.c.  NULL on every
     * other ending, where the run frees its own stack as before. */
    void      *kept_stack;
};

/* What emu_exception_dispatch receives: the guest state to dispatch against
 * and the record built where the fault was taken.  Two register files exist
 * per guest thread and never merge; a guest exception is dispatched against
 * the guest's AMD64_CONTEXT only, and the native CONTEXT plays no role (see
 * docs/guest-seh.md section 4).  The pointers live in the unix-side run
 * loop's frame, one nesting level per active run (U5 there). */
struct emu_exception_params
{
    AMD64_CONTEXT    *ctx;         /* guest state; edits are honoured on resume */
    EXCEPTION_RECORD *rec;         /* ExceptionAddress already rewritten to ctx->Rip */
    void             *stack_base;  /* guest stack of the faulting run ... */
    void             *stack_limit; /* ... for TEB-frame validity (grows down) */
};

/* Run status: the guest raised or faulted, no guest-level handler consumed
 * it, and the record is waiting PE-side (see guest_exc_pending in
 * signal_ppc64.c).  The run's PE caller re-raises it natively so the
 * existing unhandled-exception machinery produces a correctly-coded death.
 * Customer-defined NTSTATUS ('EMU'), impossible to collide with a real one. */
#define STATUS_EMU_GUEST_EXCEPTION ((NTSTATUS)0xE0454D55)

/* The innermost active run's guest stack bounds on the calling thread (zero
 * when no run is active): what a raise-path guest dispatch validates TEB
 * frames against, the fault path receiving the same bounds directly in its
 * emu_exception_params. */
struct emu_guest_stack_params
{
    void *base;    /* highest address; the stack grows down */
    void *limit;   /* lowest address */
};

/* GUEST FIBERS: the two things the PE side cannot see for itself.
 *
 * A guest fiber is a guest stack and a saved guest CONTEXT, switched by
 * replacing the context the running emulator run resumes from
 * (dlls/ntdll/signal_ppc64.c, "guest fibers").  Two pieces of that live down
 * here: the stack bounds the run is using -- which the guest's TEB is set
 * from, and which therefore have to MOVE with a switch -- and the address of
 * the HLT page, which is the return address a fiber's start routine is
 * entered with, so that returning from it ends the run exactly as returning
 * from a thread's entry point does.
 */
#define EMU_FIBER_QUERY      0   /* -> the running run's bounds, and hlt */
#define EMU_FIBER_SET_STACK  1   /* install base/limit/dealloc as the run's */

struct emu_fiber_params
{
    int        op;
    void      *base;      /* highest address; the stack grows down */
    void      *limit;     /* committed low bound: what the TEB shows */
    void      *dealloc;   /* reserved low bound */
    ULONG_PTR  hlt;       /* out (QUERY): the guest HLT page, 0 if none yet */
};

/* The 32-bit (WoW64) lane: the CPU backend wow64.dll drives through ntdll's
 * BTCpu* exports (dlls/ntdll/wow64cpu_ppc64.c) reaches the embedded emulator
 * through these three calls.  Unlike the AMD64 lane above, which keeps one
 * run alive and dispatches from inside the emulator's trap callback, this
 * lane is BOUNDED-RUN: every emu32_run returns to the PE side on every trap,
 * so that wow64.dll's own control transfers -- NtCallbackReturn's longjmp,
 * an APC's NtContinueEx -- only ever cut PE frames, never a live emulator
 * run or its kernel-stack syscall frames.  See ppc64le/wow64/DESIGN.md. */
struct emu32_init_params
{
    ULONG_PTR bop_syscall;    /* out: guest address of the syscall trap */
    ULONG_PTR bop_unixcall;   /* out: guest address of the unix-call trap */
};

struct emu32_thread_params
{
    int       term;           /* 0: adopt this thread for guest runs; 1: tear it down */
    ULONG_PTR teb32;          /* in (init): the 32-bit TEB, installed as the FS base */
};

/* Why the run stopped.  SYSCALL/UNIXCALL name the bop site the guest reached
 * (told apart by Eip); FAULT carries a guest-shaped record in rec.  TRAP is
 * an int 0x80 at any OTHER address: a thunk-stub site if the PE-side
 * dispatcher recognises the Eip (emu32_dispatch_thunk), an unassigned vector
 * -- 32-bit Windows' canonical (0, ffffffff) access violation -- if not.
 * The unix side cannot tell those apart (the stub tables live in PE modules
 * it has no view of), so the classification is the PE side's. */
#define EMU32_RUN_SYSCALL   0
#define EMU32_RUN_UNIXCALL  1
#define EMU32_RUN_FAULT     2
#define EMU32_RUN_TRAP      3

struct emu32_run_params
{
    I386_CONTEXT     *context;  /* in/out: the WOW64_CPURESERVED context in the TEB */
    ULONG             reason;   /* out: EMU32_RUN_* */
    EXCEPTION_RECORD  rec;      /* out (FAULT): ExceptionAddress = guest Eip */
};

/* Native writes into guest pages the emulator's own store detection cannot
 * see: forwarded to the emulator's code invalidation (BTCpu notify hooks). */
struct emu32_invalidate_params
{
    ULONG_PTR base;
    SIZE_T    size;
};

enum ntdll_unix_funcs
{
    unix_load_so_dll,
    unix_unwind_builtin_dll,
    unix_wine_dbg_write,
    unix_wine_server_call,
    unix_wine_server_fd_to_handle,
    unix_wine_server_handle_to_fd,
    unix_wine_spawnvp,
    unix_system_time_precise,
    unix_emu_run_entry,
    unix_emu_guest_stack,
    unix_emu_fiber_stack,
    unix_emu32_init,
    unix_emu32_thread,
    unix_emu32_run,
    unix_emu32_invalidate,
    unix_emu_xstat_init,
    unix_emu_xstat_dump,
    /* args IS the AMD64_CONTEXT* of the innermost trap: fill the lazily
     * skipped EFLAGS/FP groups from the live guest state before PE code
     * reads or rewrites them.  A no-op when the lazy declaration is off or
     * the bridge predates ABI 5.  See emu_ctx_materialize_full(). */
    unix_emu_ctx_materialize,
};

extern unixlib_handle_t __wine_unixlib_handle;

#endif /* __NTDLL_UNIXLIB_H */
