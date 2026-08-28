/*
 * WoW64 CPU backend for the ppc64le port: the BTCpu* surface, over the
 * embedded emulator in 32-bit mode
 *
 * Copyright 2026 Jbettcher
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

/* On x86-64 hosts the module wow64.dll loads by name is wow64cpu.dll (the
 * hardware far-jump gate) and on ARM64 it is xtajit.dll (an emulator).  On
 * this host it is ntdll itself: get_cpu_dll_name() answers "ntdll.dll" for a
 * POWERPC64 native machine, and these exports are the backend.  They live in
 * ntdll rather than a module of their own because everything they need --
 * the emulator bridge handle, its fault handling, the kernel/Win32 stack
 * discipline around entering the emulator -- already lives in ntdll on this
 * port, and a separate PE module would need a unixlib of its own only to
 * reach that state.  See ppc64le/wow64/DESIGN.md.
 *
 * The shape that matters: this backend is BOUNDED-RUN.  Every unix_emu32_run
 * returns here on every guest trap, and the syscall or unix call is then
 * dispatched from this loop as ordinary PE code on the Win32 stack.  The
 * upstream trap-callback shape (dispatch from inside the emulator) would
 * leave a live emulator run and its kernel-stack syscall frame under
 * wow64.dll's own control transfers -- wow64_NtCallbackReturn ends a user
 * callback with longjmp, wow64_NtContinueEx ends a user APC by restoring a
 * 64-bit context -- and both would cut those frames dead.  With bounded
 * runs, at the moment either fires the emulator is idle and the cut crosses
 * PE frames only, exactly as it does on the hardware gate. */

#ifdef __powerpc64__

#include <stdarg.h>
#include <string.h>

#include "ntstatus.h"
#define WIN32_NO_STATUS
#include "windef.h"
#include "winternl.h"
#include "ntdll_misc.h"
#include "wine/debug.h"

WINE_DEFAULT_DEBUG_CHANNEL(wow);

static NTSTATUS (WINAPI *pWow64SystemServiceEx)( UINT num, UINT *args );

static ULONG_PTR emu32_bop_syscall;
static ULONG_PTR emu32_bop_unixcall;

/**********************************************************************
 *           BTCpuProcessInit  (NTDLL.@)
 */
NTSTATUS WINAPI BTCpuProcessInit(void)
{
    struct emu32_init_params params = { 0 };
    UNICODE_STRING str = RTL_CONSTANT_STRING( L"wow64.dll" );
    WOW64INFO *wow64info = NtCurrentTeb()->TlsSlots[WOW64_TLS_WOW64INFO];
    HMODULE module;
    NTSTATUS status;

    /* wow64.dll is what loaded this backend, so the handle lookup cannot
     * fail while the export can: name it if it does. */
    if (LdrGetDllHandle( NULL, 0, &str, &module ) ||
        !(pWow64SystemServiceEx = RtlFindExportedRoutineByName( module, "Wow64SystemServiceEx" )))
    {
        ERR( "no Wow64SystemServiceEx in wow64.dll\n" );
        return STATUS_ENTRYPOINT_NOT_FOUND;
    }

    if ((status = WINE_UNIX_CALL( unix_emu32_init, &params )))
    {
        ERR( "32-bit emulator setup failed, status %08x\n", (UINT)status );
        return status;
    }
    emu32_bop_syscall  = params.bop_syscall;
    emu32_bop_unixcall = params.bop_unixcall;

    /* A software CPU: wow64.dll then creates the cross-process work list our
     * notification entry points below are fed from, and applies the software
     * breakpoint Eip adjustment. */
    wow64info->CpuFlags |= WOW64_CPUFLAGS_SOFTWARE;

    TRACE( "bops %08lx/%08lx\n", (unsigned long)emu32_bop_syscall, (unsigned long)emu32_bop_unixcall );
    return STATUS_SUCCESS;
}


/**********************************************************************
 *           BTCpuGetBopCode  (NTDLL.@)
 */
void * WINAPI BTCpuGetBopCode(void)
{
    return (void *)emu32_bop_syscall;
}


/**********************************************************************
 *           __wine_get_unix_opcode  (NTDLL.@)
 */
void * WINAPI __wine_get_unix_opcode(void)
{
    return (void *)emu32_bop_unixcall;
}


/**********************************************************************
 *           BTCpuThreadInit  (NTDLL.@)
 */
void WINAPI BTCpuThreadInit(void)
{
    struct emu32_thread_params params;
    NTSTATUS status;

    params.term  = 0;
    params.teb32 = (ULONG_PTR)NtCurrentTeb() + NtCurrentTeb()->WowTebOffset;
    if ((status = WINE_UNIX_CALL( unix_emu32_thread, &params )))
        ERR( "32-bit emulator thread setup failed, status %08x; "
             "the first simulate on this thread will fail\n", (UINT)status );
}


/**********************************************************************
 *           BTCpuThreadTerm  (NTDLL.@)
 */
void WINAPI BTCpuThreadTerm( HANDLE thread, LONG exit_code )
{
    struct emu32_thread_params params = { .term = 1 };
    THREAD_BASIC_INFORMATION info;

    /* The emulator's per-thread state can only be destroyed from its own
     * host thread.  Tear it down when this call is for the calling thread;
     * a thread terminated from outside keeps its state until process end --
     * the recorded cost of NtTerminateThread-from-outside, see DESIGN.md. */
    if (thread != NtCurrentThread() &&
        (NtQueryInformationThread( thread, ThreadBasicInformation, &info, sizeof(info), NULL ) ||
         info.ClientId.UniqueThread != NtCurrentTeb()->ClientId.UniqueThread))
        return;
    WINE_UNIX_CALL( unix_emu32_thread, &params );
}


/**********************************************************************
 *           BTCpuGetContext  (NTDLL.@)
 */
NTSTATUS WINAPI BTCpuGetContext( HANDLE thread, HANDLE process, void *unknown, I386_CONTEXT *ctx )
{
    return RtlWow64GetThreadContext( thread, (WOW64_CONTEXT *)ctx );
}


/**********************************************************************
 *           BTCpuSetContext  (NTDLL.@)
 */
NTSTATUS WINAPI BTCpuSetContext( HANDLE thread, HANDLE process, void *unknown, I386_CONTEXT *ctx )
{
    return RtlWow64SetThreadContext( thread, (const WOW64_CONTEXT *)ctx );
}


/**********************************************************************
 *           BTCpuIsProcessorFeaturePresent  (NTDLL.@)
 *
 * Answering for the EMULATED processor, not the host: wow64cpu can defer to
 * RtlIsProcessorFeaturePresent because its host is an x86-64; ours would
 * answer for a POWER9.  The set is the emulator's actual baseline (SSE up to
 * 4.2 -- what the tree's own guest builds already rely on), not a guess at a
 * fashionable CPU.
 */
BOOLEAN WINAPI BTCpuIsProcessorFeaturePresent( UINT feature )
{
    switch (feature)
    {
    case PF_COMPARE_EXCHANGE_DOUBLE:
    case PF_MMX_INSTRUCTIONS_AVAILABLE:
    case PF_XMMI_INSTRUCTIONS_AVAILABLE:
    case PF_RDTSC_INSTRUCTION_AVAILABLE:
    case PF_XMMI64_INSTRUCTIONS_AVAILABLE:
    case PF_NX_ENABLED:
    case PF_SSE3_INSTRUCTIONS_AVAILABLE:
    case PF_FASTFAIL_AVAILABLE:
    case PF_SSSE3_INSTRUCTIONS_AVAILABLE:
    case PF_SSE4_1_INSTRUCTIONS_AVAILABLE:
    case PF_SSE4_2_INSTRUCTIONS_AVAILABLE:
        return TRUE;
    default:
        return FALSE;
    }
}


/**********************************************************************
 *           BTCpuResetToConsistentState  (NTDLL.@)
 *
 * For the hardware gate this rewrites a fault context taken in 32-bit mode
 * into "we are at the 64-bit transition stub".  For this backend there is
 * never an inconsistent in-flight state: a guest fault comes back from
 * unix_emu32_run with the guest state already parked in the TEB cpu area,
 * and the exception is raised from BTCpuSimulate as ordinary PE code whose
 * native context needs no repair.
 */
NTSTATUS WINAPI BTCpuResetToConsistentState( EXCEPTION_POINTERS *ptrs )
{
    return STATUS_SUCCESS;
}


/**********************************************************************
 *           BTCpuTurboThunkControl  (NTDLL.@)
 */
NTSTATUS WINAPI BTCpuTurboThunkControl( ULONG enable )
{
    if (enable) return STATUS_NOT_SUPPORTED;
    /* no turbo thunks: every syscall takes the bounded-run path */
    return STATUS_SUCCESS;
}


/**********************************************************************
 *           invalidation notifications
 *
 * The emulator detects the GUEST's own stores itself; these entry points
 * cover code that appears through native writes the emulator cannot see --
 * a fresh view mapped over a reused address, WriteProcessMemory from
 * outside (fed to us through the cross-process work list), an explicit
 * NtFlushInstructionCache.  Over-invalidation is always safe, so each is a
 * plain range forward.
 */
static void emu32_invalidate( const void *addr, SIZE_T size )
{
    struct emu32_invalidate_params params;

    params.base = (ULONG_PTR)addr;
    params.size = size;
    WINE_UNIX_CALL( unix_emu32_invalidate, &params );
}

void WINAPI BTCpuFlushInstructionCache2( const void *addr, SIZE_T size )
{
    emu32_invalidate( addr, size );
}

void WINAPI BTCpuNotifyMemoryDirty( void *addr, SIZE_T size )
{
    emu32_invalidate( addr, size );
}

NTSTATUS WINAPI BTCpuNotifyMapViewOfSection( void *unknown, void *addr, void *target,
                                             SIZE_T size, ULONG alloc_type, ULONG protect )
{
    emu32_invalidate( addr, size );
    return STATUS_SUCCESS;
}

void WINAPI BTCpuNotifyUnmapViewOfSection( void *addr, BOOL after, NTSTATUS status )
{
    /* The i386 thunk dispatcher caches trap-site resolutions keyed by EIP
     * (the same RIP cache the 64-bit lane uses), and the 32-bit loader --
     * the guest's own ntdll32 -- unmaps modules through here, never through
     * free_modref, whose flush covers only the 64-bit list.  Flushing on
     * every unmap costs one pass over 1024 slots on an event that already
     * unmapped a view. */
    if (after && !status) flush_guest_thunk_cache();
}

void WINAPI BTCpuNotifyMemoryAlloc( void *addr, SIZE_T size, ULONG type, ULONG prot,
                                    BOOL after, NTSTATUS status )
{
    if (after && !status) emu32_invalidate( addr, size );
}

void WINAPI BTCpuNotifyMemoryProtect( void *addr, SIZE_T size, ULONG prot,
                                      BOOL after, NTSTATUS status )
{
    if (after && !status) emu32_invalidate( addr, size );
}


/**********************************************************************
 *           BTCpuSimulate  (NTDLL.@)
 *
 * The simulate loop: run the guest until it traps, dispatch what it trapped
 * for, repeat.  The I386_CONTEXT in the TEB's WOW64_CPURESERVED area is the
 * single source of truth in both directions -- unix_emu32_run loads the
 * guest from it and parks the complete state back into it on every stop --
 * which is what keeps RtlWow64Get/SetThreadContext and every wow64.dll
 * redirection (exception dispatch, APCs, user callbacks) working on this
 * backend with no special cases: they all just edit that context, and the
 * next iteration runs whatever it says.
 *
 * The RESET_STATE flag keeps wow64cpu's exact meaning: it is set when the
 * context was wholesale-replaced under a syscall (RtlWow64SetThreadContext
 * on the current thread), in which case the syscall's own return value must
 * NOT be written back over the new context's Eax.  The full reload the
 * hardware gate performs under the flag is this loop's every-iteration
 * behaviour, so the flag decides only the Eax question.
 */
void WINAPI BTCpuSimulate(void)
{
    WOW64_CPURESERVED *cpu = NtCurrentTeb()->TlsSlots[WOW64_TLS_CPURESERVED];
    I386_CONTEXT *wow_ctx;
    USHORT machine;
    void *unused;

    if (RtlWow64GetCurrentCpuArea( &machine, (void **)&wow_ctx, &unused ) ||
        machine != IMAGE_FILE_MACHINE_I386)
    {
        ERR( "no i386 cpu area for this thread\n" );
        RtlExitUserProcess( STATUS_INVALID_IMAGE_FORMAT );
    }

    for (;;)
    {
        struct emu32_run_params params = { .context = wow_ctx };
        NTSTATUS status = WINE_UNIX_CALL( unix_emu32_run, &params );

        if (status)
        {
            /* Terminated, not raised: BTCpuSimulate has no caller to return
             * an error to -- cpu_simulate() loops around it forever -- and
             * raising would hand the failure to a guest that cannot run (the
             * lane itself is down: bridge missing, WINEEMUNOWOW32, thread
             * init failure), so the pass-to-guest machinery would spin on a
             * dead emulator instead of failing.  The status is already named
             * by the unix side's own ERR; die with it promptly. */
            ERR( "emulator run failed, status %08x; terminating\n", (UINT)status );
            NtTerminateProcess( NtCurrentProcess(), status );
        }

        switch (params.reason)
        {
        case EMU32_RUN_SYSCALL:
        {
            ULONG *esp = ULongToPtr( wow_ctx->Esp );
            ULONG num = wow_ctx->Eax;
            NTSTATUS ret;

            /* The order is wow64cpu's syscall_32to64, restated: set the
             * return state (Eip=[esp], Esp=esp+4) into the context BEFORE
             * dispatching, then dispatch, then ALWAYS store Eax.  Both steps
             * are unconditional -- the flag does NOT gate them.
             *
             * A syscall that REPLACES the context (exception dispatch, an
             * APC, NtContinue) does so through BTCpuSetContext, which
             * overwrites Eip/Esp/regs and sets RESET_STATE; because that
             * happens INSIDE the dispatch call, the overwrite lands on top
             * of the return state set here, and the Eax stored afterwards
             * equals the replacement's own Eax (the handler returns
             * get_context_return_value).  So a single unconditional
             * store is correct for both shapes -- and gating Eax on the flag
             * was a real defect: the initial thread setup sets RESET_STATE
             * (BTCpuSetContext with CONTROL), so the FIRST syscall's result
             * would have been dropped, leaving the syscall number in Eax and
             * failing every early allocation.
             *
             * [esp] is the address the stub's `call *dispatcher` pushed --
             * the stub's own `ret $n` then does the stdcall cleanup -- and
             * the arguments sit at esp+8 (after that return address and the
             * caller's), which is esp+2 in ULONGs. */
            wow_ctx->Eip = esp[0];
            wow_ctx->Esp = (ULONG)(ULONG_PTR)(esp + 1);
            ret = pWow64SystemServiceEx( num, esp + 2 );
            wow_ctx->Eax = ret;
            cpu->Flags &= ~WOW64_CPURESERVED_FLAG_RESET_STATE;
            break;
        }

        case EMU32_RUN_UNIXCALL:
        {
            /* [esp] ret, [esp+4] 8-byte unixlib handle, [esp+12] code,
             * [esp+16] args, final esp = esp+20 -- the frame the 32-bit
             * __wine_unix_call dispatcher cell's caller builds, consumed
             * verbatim from wow64cpu's unix_call_32to64.  The handle a
             * 32-bit module was given already names its unixlib's wow64
             * table, so the 32->64 argument conversion happens in the
             * library's own wow64_* entries.  Same unconditional store as
             * the syscall case above. */
            ULONG *esp = ULongToPtr( wow_ctx->Esp );
            unixlib_handle_t handle;
            NTSTATUS ret;

            memcpy( &handle, esp + 1, sizeof(handle) );
            wow_ctx->Eip = esp[0];
            wow_ctx->Esp += 20;
            ret = __wine_unix_call( handle, esp[3], ULongToPtr( esp[4] ));
            wow_ctx->Eax = ret;
            cpu->Flags &= ~WOW64_CPURESERVED_FLAG_RESET_STATE;
            break;
        }

        case EMU32_RUN_TRAP:
            /* An int 0x80 that is neither bop: a thunk-stub site (the DXVK
             * modules' i386 halves -- the one surface real WoW64 cannot
             * serve, because this tree's implementation of it is native) or
             * a stray vector.  The dispatcher serves the former completely
             * -- result in Eax/Edx, return address and stdcall frame popped
             * -- and refuses the latter, which then raises exactly the
             * access violation 32-bit Windows raises for an unassigned
             * vector: the canonical (0, ffffffff) pair. */
            if (!emu32_dispatch_thunk( wow_ctx ))
            {
                cpu->Flags &= ~WOW64_CPURESERVED_FLAG_RESET_STATE;
                break;
            }
            {
                CONTEXT context;
                volatile BOOL raised = FALSE;
                EXCEPTION_RECORD rec = { 0 };

                rec.ExceptionCode = EXCEPTION_ACCESS_VIOLATION;
                rec.ExceptionAddress = ULongToPtr( wow_ctx->Eip );
                rec.NumberParameters = 2;
                rec.ExceptionInformation[1] = ~0u;
                RtlCaptureContext( &context );
                if (!raised)
                {
                    raised = TRUE;
                    NtRaiseException( &rec, &context, TRUE );
                }
            }
            break;

        case EMU32_RUN_FAULT:
        {
            /* Raise the guest-shaped record through the native pipeline so
             * everything a hardware fault would reach still runs: the
             * debugger (first-chance, from the unix side of
             * NtRaiseException), 64-bit vectored handlers, and finally
             * cpu_simulate()'s own handler, whose filter passes the record
             * to the 32-bit guest and unwinds back to the simulate loop.
             * The volatile flag stands in for the resume label the asm
             * gates patch into their captured context: a handler that
             * CONTINUES restores these registers, sees the flag set, and
             * falls through to re-run the (possibly repaired) guest. */
            CONTEXT context;
            volatile BOOL raised = FALSE;

            RtlCaptureContext( &context );
            if (!raised)
            {
                raised = TRUE;
                NtRaiseException( &params.rec, &context, TRUE );
            }
            break;
        }
        }
    }
}

#endif  /* __powerpc64__ */
