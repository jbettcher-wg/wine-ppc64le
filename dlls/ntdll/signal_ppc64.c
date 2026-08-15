/*
 * PowerPC64 (ELFv2) PE-side signal/exception support
 *
 * Copyright 1999, 2005 Alexandre Julliard
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

#ifdef __powerpc64__

#include <assert.h>
#include <signal.h>
#include <stdlib.h>
#include <stdarg.h>
#include <stdio.h>
#include <setjmp.h>

#include "ntstatus.h"
#include "windef.h"
#include "winternl.h"
#include "ddk/wdm.h"
#include "wine/exception.h"
#include "ntdll_misc.h"
#include "wine/debug.h"
#include "ntsyscalls.h"

WINE_DEFAULT_DEBUG_CHANNEL(seh);
WINE_DECLARE_DEBUG_CHANNEL(relay);

/* PowerPC64 has no Microsoft ABI, no PE unwind format and no .pdata producer.
 * On this port every "PE" module is in fact an ELF shared object built by
 * winegcc (PE_ARCHS is empty, DLLEXT is .so), so:
 *
 *   - RtlLookupFunctionEntry() always returns NULL and dispatch.LanguageHandler
 *     is therefore always NULL.  Structured exception handling in builtin
 *     modules runs entirely through the TEB exception-registration list that
 *     wine/exception.h's __TRY/__EXCEPT macros push, and those handlers resume
 *     with __wine_longjmp rather than by restoring a CONTEXT.
 *   - Stack walking uses the ELFv2 back chain (0(r1)) and the LR save slot
 *     (16(r1) of the caller's frame), which every ABI-conforming ppc64 function
 *     maintains.  See RtlVirtualUnwind2() in unwind.c.
 *
 * Consequences that are deliberately NOT implemented here, and are recorded as
 * such rather than papered over:
 *   - virtual_unwind() cannot restore non-volatile registers, so an unwind
 *     that resumes by RtlRestoreContext() will resume with the *unwinder's*
 *     r14-r31/f14-f31/v20-v31, not the target frame's.  That is correct for
 *     the __TRY/__EXCEPT + __wine_longjmp path (longjmp restores them from the
 *     jmp_buf) and wrong for a hypothetical PE __try/__except, of which there
 *     are none on this architecture.
 *   - Nested/collided unwind detection inside call_seh_handler() /
 *     call_unwind_handler() is absent: there is no .seh_handler equivalent in
 *     ELF asm and the TEB-frame hack would recurse.  Those two helpers are
 *     plain calls.
 */

/* CONTEXT field offsets used by the assembly below; kept honest by C_ASSERT. */
#define CTX_Fpr0         0x000
#define CTX_Fpscr        0x100
#define CTX_Gpr0         0x108
#define CTX_Gpr1         0x110
#define CTX_Gpr2         0x118
#define CTX_Gpr3         0x120
#define CTX_Cr           0x208
#define CTX_Xer          0x210
#define CTX_Msr          0x218
#define CTX_Iar          0x220
#define CTX_Lr           0x228
#define CTX_Ctr          0x230
#define CTX_ContextFlags 0x238
#define CTX_Vscr         0x298
#define CTX_Vrsave       0x29c
#define CTX_Vr0          0x2a0
#define CTX_SIZE         0x4a0

C_ASSERT( offsetof(CONTEXT,Fpr0)         == CTX_Fpr0 );
C_ASSERT( offsetof(CONTEXT,Fpscr)        == CTX_Fpscr );
C_ASSERT( offsetof(CONTEXT,Gpr0)         == CTX_Gpr0 );
C_ASSERT( offsetof(CONTEXT,Gpr1)         == CTX_Gpr1 );
C_ASSERT( offsetof(CONTEXT,Gpr2)         == CTX_Gpr2 );
C_ASSERT( offsetof(CONTEXT,Gpr3)         == CTX_Gpr3 );
C_ASSERT( offsetof(CONTEXT,Gpr31)        == CTX_Gpr0 + 31 * 8 );
C_ASSERT( offsetof(CONTEXT,Cr)           == CTX_Cr );
C_ASSERT( offsetof(CONTEXT,Xer)          == CTX_Xer );
C_ASSERT( offsetof(CONTEXT,Msr)          == CTX_Msr );
C_ASSERT( offsetof(CONTEXT,Iar)          == CTX_Iar );
C_ASSERT( offsetof(CONTEXT,Lr)           == CTX_Lr );
C_ASSERT( offsetof(CONTEXT,Ctr)          == CTX_Ctr );
C_ASSERT( offsetof(CONTEXT,ContextFlags) == CTX_ContextFlags );
C_ASSERT( offsetof(CONTEXT,Vscr)         == CTX_Vscr );
C_ASSERT( offsetof(CONTEXT,Vrsave)       == CTX_Vrsave );
C_ASSERT( offsetof(CONTEXT,Vr)           == CTX_Vr0 );
C_ASSERT( sizeof(CONTEXT)                == CTX_SIZE );

/* _JUMP_BUFFER must stay byte-identical to the __wine_jmp_buf that
 * libs/winecrt0/setjmp.c writes; both are hand-written assembly against literal
 * offsets, so pin every one of them here rather than trusting an eyeball match.
 * The numbers on the right are the offsets that appear in winecrt0's
 * __wine_setjmpex. */
C_ASSERT( offsetof(struct _JUMP_BUFFER,Frame) ==   0 );
C_ASSERT( offsetof(struct _JUMP_BUFFER,Gpr)   ==   8 );   /* r14-r31 */
C_ASSERT( offsetof(struct _JUMP_BUFFER,Sp)    == 152 );
C_ASSERT( offsetof(struct _JUMP_BUFFER,Toc)   == 160 );
C_ASSERT( offsetof(struct _JUMP_BUFFER,Cr)    == 168 );
C_ASSERT( offsetof(struct _JUMP_BUFFER,Lr)    == 176 );
C_ASSERT( offsetof(struct _JUMP_BUFFER,Fpr)   == 184 );   /* f14-f31 */
C_ASSERT( offsetof(struct _JUMP_BUFFER,Vr)    == 336 );   /* v20-v31 */
C_ASSERT( offsetof(struct _JUMP_BUFFER,Vr) % 16 == 0 );   /* stvx force-aligns */
C_ASSERT( sizeof(struct _JUMP_BUFFER)         <= sizeof(__wine_jmp_buf) );

/* CONTEXT_FULL, spelled out for the assembly that builds it with lis/ori. */
C_ASSERT( CONTEXT_FULL == 0x800017 );


/***********************************************************************
 *           NtCurrentTeb   (NTDLL.@)
 *
 * PowerPC64 has no register the Wine ABI can steal for the TEB: r13 is the ELF
 * thread pointer (glibc's), r2 is the TOC and r1 the stack pointer.  So the TEB
 * lives in an initial-exec thread-local of this module.  Measured on the AC922:
 * an initial-exec __thread in a dlopen()ed shared object resolves correctly and
 * compiles to "ld rN,off(r2); add rN,rN,r13" - no __tls_get_addr, no libc
 * dependency, which matters because ntdll.dll.so is linked -nodefaultlibs.
 *
 * The syscall thunks in include/wine/asm.h read this same variable directly,
 * with the ordinary @got@tprel/@tls relocations, so there is no cached offset
 * anywhere and nothing to initialise before the first syscall.  A cache was
 * tried and rejected: it would have to be filled by a constructor, and Wine
 * renames DT_INIT_ARRAY to a private dynamic tag in these modules so that it
 * can run constructors itself at DLL_PROCESS_ATTACH - far too late.  Measured:
 * a constructor added here was present in .init_array and still never ran.
 * An unfilled cache would have made every thunk read a "TEB" out of glibc's
 * TCB at 0(r13) and continue with garbage.
 */
__attribute__((visibility("hidden"))) __thread TEB *ppc64_current_teb
    __attribute__((tls_model("initial-exec")));

TEB * WINAPI NtCurrentTeb(void)
{
    return ppc64_current_teb;
}

/***********************************************************************
 *           __wine_init_teb
 *
 * Called by the unix library on every thread before any PE code runs, and
 * before the first syscall of that thread.  Not exported through the spec file;
 * the unix loader resolves it out of the PE ntdll export table by name.
 */
void CDECL __wine_init_teb( TEB *teb )
{
    ppc64_current_teb = teb;
}


/*******************************************************************
 *         syscall thunks
 *
 * __ASM_SYSCALL_FUNC (include/wine/asm.h) emits, per syscall:
 *
 *      <global entry>  addis r2,r12,.TOC.-name@ha ; addi r2,r2,.TOC.-name@l
 *      <local entry>   li    r11,<id>
 *                      b     __wine_syscall
 *
 * so on arrival at __wine_syscall: r2 = ntdll's TOC, r11 = syscall id, LR still
 * holds the PE caller's return address (the thunk branched, it did not call),
 * and r3-r10 plus the caller's parameter save area still hold the arguments.
 *
 * __wine_syscall adds r0 = TEB and tail-jumps to __wine_syscall_dispatcher with
 * r12 = the dispatcher address, which is what an ELFv2 global entry point
 * requires.  Handing the TEB over in r0 spares the unix-side dispatcher from
 * having to do TLS access in assembly; r0 is volatile and is not an argument
 * register, and r11/r12 are the only other registers free at this point.
 */
#define SYSCALL_ENTRY(id,name,args) __ASM_SYSCALL_FUNC( id, name )
ALL_SYSCALLS
#undef SYSCALL_ENTRY


/**************************************************************************
 *		__chkstk (NTDLL.@)
 *
 * Supposed to touch all the stack pages, but we shouldn't need that.
 */
__ASM_GLOBAL_FUNC( __chkstk, "blr" )


/***********************************************************************
 *		RtlCaptureContext (NTDLL.@)
 *
 * Deliberately TOC-free so that it neither needs nor destroys the caller's r2,
 * which it must record as Gpr2.  st_other stays 0, which is exactly the ELFv2
 * encoding for "single entry point, does not use r2".
 *
 * The vector stores use stvx, which force-aligns its effective address down to
 * a 16-byte boundary.  CONTEXT is DECLSPEC_ALIGN(16), so a properly declared
 * CONTEXT is always suitably aligned; a misaligned one would silently corrupt
 * memory below it.  Same contract as libs/winecrt0/setjmp.c.
 */
__ASM_GLOBAL_FUNC( RtlCaptureContext,
                   "stfd 0, 0x000(3)\n\t"
                   "stfd 1, 0x008(3)\n\t"
                   "stfd 2, 0x010(3)\n\t"
                   "stfd 3, 0x018(3)\n\t"
                   "stfd 4, 0x020(3)\n\t"
                   "stfd 5, 0x028(3)\n\t"
                   "stfd 6, 0x030(3)\n\t"
                   "stfd 7, 0x038(3)\n\t"
                   "stfd 8, 0x040(3)\n\t"
                   "stfd 9, 0x048(3)\n\t"
                   "stfd 10, 0x050(3)\n\t"
                   "stfd 11, 0x058(3)\n\t"
                   "stfd 12, 0x060(3)\n\t"
                   "stfd 13, 0x068(3)\n\t"
                   "stfd 14, 0x070(3)\n\t"
                   "stfd 15, 0x078(3)\n\t"
                   "stfd 16, 0x080(3)\n\t"
                   "stfd 17, 0x088(3)\n\t"
                   "stfd 18, 0x090(3)\n\t"
                   "stfd 19, 0x098(3)\n\t"
                   "stfd 20, 0x0a0(3)\n\t"
                   "stfd 21, 0x0a8(3)\n\t"
                   "stfd 22, 0x0b0(3)\n\t"
                   "stfd 23, 0x0b8(3)\n\t"
                   "stfd 24, 0x0c0(3)\n\t"
                   "stfd 25, 0x0c8(3)\n\t"
                   "stfd 26, 0x0d0(3)\n\t"
                   "stfd 27, 0x0d8(3)\n\t"
                   "stfd 28, 0x0e0(3)\n\t"
                   "stfd 29, 0x0e8(3)\n\t"
                   "stfd 30, 0x0f0(3)\n\t"
                   "stfd 31, 0x0f8(3)\n\t"
                   "mffs 0\n\t"                  /* f0 is already saved */
                   "stfd 0, 0x100(3)\n\t"        /* Fpscr; mffs writes f0, r0 is intact */
                   "std 0, 0x108(3)\n\t"         /* Gpr0, still the caller's value */
                   "std 1, 0x110(3)\n\t"         /* Gpr1: caller's sp, we push nothing */
                   "std 2, 0x118(3)\n\t"         /* Gpr2: caller's TOC */
                   "std 3, 0x120(3)\n\t"
                   "std 4, 0x128(3)\n\t"
                   "std 5, 0x130(3)\n\t"
                   "std 6, 0x138(3)\n\t"
                   "std 7, 0x140(3)\n\t"
                   "std 8, 0x148(3)\n\t"
                   "std 9, 0x150(3)\n\t"
                   "std 10, 0x158(3)\n\t"
                   "std 11, 0x160(3)\n\t"
                   "std 12, 0x168(3)\n\t"
                   "std 13, 0x170(3)\n\t"
                   "std 14, 0x178(3)\n\t"
                   "std 15, 0x180(3)\n\t"
                   "std 16, 0x188(3)\n\t"
                   "std 17, 0x190(3)\n\t"
                   "std 18, 0x198(3)\n\t"
                   "std 19, 0x1a0(3)\n\t"
                   "std 20, 0x1a8(3)\n\t"
                   "std 21, 0x1b0(3)\n\t"
                   "std 22, 0x1b8(3)\n\t"
                   "std 23, 0x1c0(3)\n\t"
                   "std 24, 0x1c8(3)\n\t"
                   "std 25, 0x1d0(3)\n\t"
                   "std 26, 0x1d8(3)\n\t"
                   "std 27, 0x1e0(3)\n\t"
                   "std 28, 0x1e8(3)\n\t"
                   "std 29, 0x1f0(3)\n\t"
                   "std 30, 0x1f8(3)\n\t"
                   "std 31, 0x200(3)\n\t"
                   "mfcr 0\n\t"
                   "std 0, 0x208(3)\n\t"         /* Cr */
                   "mfxer 0\n\t"
                   "std 0, 0x210(3)\n\t"         /* Xer */
                   "li 0, 0\n\t"
                   "std 0, 0x218(3)\n\t"         /* Msr: not readable in problem state */
                   "std 0, 0x240(3)\n\t"         /* Dar */
                   "std 0, 0x248(3)\n\t"         /* Dsisr */
                   "std 0, 0x250(3)\n\t"         /* Trap */
                   "stw 0, 0x298(3)\n\t"         /* Vscr: see comment below */
                   "stw 0, 0x29c(3)\n\t"         /* Vrsave */
                   "mflr 0\n\t"
                   "std 0, 0x220(3)\n\t"         /* Iar = our return address */
                   "std 0, 0x228(3)\n\t"         /* Lr */
                   "mfctr 0\n\t"
                   "std 0, 0x230(3)\n\t"         /* Ctr */
                   /* v0-v31.  The index register must not be r0: "addi 0,0,16"
                    * is not "r0 += 16" -- addi reads RA=0 as the literal zero, so
                    * it assembles to li 0,16 and every store after the first landed
                    * at Context+16, i.e. on top of Fpr2/Fpr3, while v1-v31 were
                    * never saved at all.  r11 is volatile in ELFv2 and the caller's
                    * value is already in Gpr11 by the time we get here. */
                   "li 11, 0x2a0\n\t"
                   "stvx 0, 3, 11\n\t"  "addi 11, 11, 16\n\t"
                   "stvx 1, 3, 11\n\t"  "addi 11, 11, 16\n\t"
                   "stvx 2, 3, 11\n\t"  "addi 11, 11, 16\n\t"
                   "stvx 3, 3, 11\n\t"  "addi 11, 11, 16\n\t"
                   "stvx 4, 3, 11\n\t"  "addi 11, 11, 16\n\t"
                   "stvx 5, 3, 11\n\t"  "addi 11, 11, 16\n\t"
                   "stvx 6, 3, 11\n\t"  "addi 11, 11, 16\n\t"
                   "stvx 7, 3, 11\n\t"  "addi 11, 11, 16\n\t"
                   "stvx 8, 3, 11\n\t"  "addi 11, 11, 16\n\t"
                   "stvx 9, 3, 11\n\t"  "addi 11, 11, 16\n\t"
                   "stvx 10, 3, 11\n\t" "addi 11, 11, 16\n\t"
                   "stvx 11, 3, 11\n\t" "addi 11, 11, 16\n\t"
                   "stvx 12, 3, 11\n\t" "addi 11, 11, 16\n\t"
                   "stvx 13, 3, 11\n\t" "addi 11, 11, 16\n\t"
                   "stvx 14, 3, 11\n\t" "addi 11, 11, 16\n\t"
                   "stvx 15, 3, 11\n\t" "addi 11, 11, 16\n\t"
                   "stvx 16, 3, 11\n\t" "addi 11, 11, 16\n\t"
                   "stvx 17, 3, 11\n\t" "addi 11, 11, 16\n\t"
                   "stvx 18, 3, 11\n\t" "addi 11, 11, 16\n\t"
                   "stvx 19, 3, 11\n\t" "addi 11, 11, 16\n\t"
                   "stvx 20, 3, 11\n\t" "addi 11, 11, 16\n\t"
                   "stvx 21, 3, 11\n\t" "addi 11, 11, 16\n\t"
                   "stvx 22, 3, 11\n\t" "addi 11, 11, 16\n\t"
                   "stvx 23, 3, 11\n\t" "addi 11, 11, 16\n\t"
                   "stvx 24, 3, 11\n\t" "addi 11, 11, 16\n\t"
                   "stvx 25, 3, 11\n\t" "addi 11, 11, 16\n\t"
                   "stvx 26, 3, 11\n\t" "addi 11, 11, 16\n\t"
                   "stvx 27, 3, 11\n\t" "addi 11, 11, 16\n\t"
                   "stvx 28, 3, 11\n\t" "addi 11, 11, 16\n\t"
                   "stvx 29, 3, 11\n\t" "addi 11, 11, 16\n\t"
                   "stvx 30, 3, 11\n\t" "addi 11, 11, 16\n\t"
                   "stvx 31, 3, 11\n\t"
                   "lis 0, 0x80\n\t"             /* CONTEXT_FULL = 0x00800017 */
                   "ori 0, 0, 0x17\n\t"
                   "std 0, 0x238(3)\n\t"
                   "blr" )

/* Vscr and Vrsave are stored as zero rather than captured: mfvscr delivers VSCR
 * inside a vector register whose element order differs between BE and LE, and
 * getting that wrong writes a plausible-looking wrong value.  Zero is the
 * architectural reset state (NJ=0, SAT=0).  Recorded as a stub. */


/**********************************************************************
 *           virtual_unwind
 */
static NTSTATUS virtual_unwind( ULONG type, DISPATCHER_CONTEXT *dispatch, CONTEXT *context )
{
    DWORD64 pc = context->Iar;

    dispatch->ScopeIndex = 0;
    dispatch->ControlPc  = pc;
    dispatch->ControlPcIsUnwound = (context->ContextFlags & CONTEXT_UNWOUND_TO_CALL) != 0;
    if (dispatch->ControlPcIsUnwound) pc -= 4;

    dispatch->FunctionEntry = RtlLookupFunctionEntry( pc, &dispatch->ImageBase, dispatch->HistoryTable );

    if (RtlVirtualUnwind2( type, dispatch->ImageBase, pc, dispatch->FunctionEntry, context,
                           NULL, &dispatch->HandlerData, &dispatch->EstablisherFrame,
                           NULL, NULL, NULL, &dispatch->LanguageHandler, 0 ))
    {
        WARN( "exception data not found for pc %p, lr %p\n", (void *)pc, (void *)context->Lr );
        return STATUS_INVALID_DISPOSITION;
    }
    return STATUS_SUCCESS;
}


/*******************************************************************
 *         nested_exception_handler
 */
EXCEPTION_DISPOSITION WINAPI nested_exception_handler( EXCEPTION_RECORD *rec, void *frame,
                                                       CONTEXT *context, void *dispatch )
{
    if (rec->ExceptionFlags & (EXCEPTION_UNWINDING | EXCEPTION_EXIT_UNWIND)) return ExceptionContinueSearch;
    return ExceptionNestedException;
}


/***********************************************************************
 *		call_seh_handler / call_unwind_handler
 *
 * STUB, and knowingly so: on the other architectures these are assembly
 * trampolines carrying a .seh_handler, so that an exception raised *inside* a
 * handler is reported as ExceptionNestedException / ExceptionCollidedUnwind.
 * ELF ppc64 has no .seh_handler, and pushing a TEB frame here would be seen by
 * call_seh_handlers()'s own TEB-frame walk.  Nested and collided unwinds are
 * therefore not detected on this architecture.
 */
static DWORD call_seh_handler( EXCEPTION_RECORD *rec, ULONG_PTR frame,
                               CONTEXT *context, void *dispatch, PEXCEPTION_ROUTINE handler )
{
    return handler( rec, (void *)frame, context, dispatch );
}

static DWORD call_unwind_handler( EXCEPTION_RECORD *rec, ULONG_PTR frame,
                                  CONTEXT *context, void *dispatch, PEXCEPTION_ROUTINE handler )
{
    return handler( rec, (void *)frame, context, dispatch );
}


/**********************************************************************
 *           call_seh_handlers
 *
 * Call the SEH handlers.
 */
NTSTATUS call_seh_handlers( EXCEPTION_RECORD *rec, CONTEXT *orig_context )
{
    EXCEPTION_REGISTRATION_RECORD *teb_frame = NtCurrentTeb()->Tib.ExceptionList;
    UNWIND_HISTORY_TABLE table;
    DISPATCHER_CONTEXT dispatch;
    CONTEXT context;
    NTSTATUS status;
    ULONG_PTR frame;
    DWORD res;

    context = *orig_context;
    dispatch.TargetPc      = 0;
    dispatch.ContextRecord = &context;
    dispatch.HistoryTable  = &table;
    dispatch.NonVolatileRegisters = NULL;

    for (;;)
    {
        status = virtual_unwind( UNW_FLAG_EHANDLER, &dispatch, &context );
        if (status != STATUS_SUCCESS) return status;

    unwind_done:
        if (!dispatch.EstablisherFrame) break;

        if (!is_valid_frame( dispatch.EstablisherFrame ))
        {
            ERR( "invalid frame %I64x (%p-%p)\n", dispatch.EstablisherFrame,
                 NtCurrentTeb()->Tib.StackLimit, NtCurrentTeb()->Tib.StackBase );
            rec->ExceptionFlags |= EXCEPTION_STACK_INVALID;
            break;
        }

        if (dispatch.LanguageHandler)
        {
            TRACE( "calling handler %p (rec=%p, frame=%I64x context=%p, dispatch=%p)\n",
                   dispatch.LanguageHandler, rec, dispatch.EstablisherFrame, orig_context, &dispatch );
            res = call_seh_handler( rec, dispatch.EstablisherFrame, orig_context,
                                    &dispatch, dispatch.LanguageHandler );
            rec->ExceptionFlags &= EXCEPTION_NONCONTINUABLE;
            TRACE( "handler at %p returned %lu\n", dispatch.LanguageHandler, res );

            switch (res)
            {
            case ExceptionContinueExecution:
                if (rec->ExceptionFlags & EXCEPTION_NONCONTINUABLE) return STATUS_NONCONTINUABLE_EXCEPTION;
                return STATUS_SUCCESS;
            case ExceptionContinueSearch:
                break;
            case ExceptionNestedException:
                rec->ExceptionFlags |= EXCEPTION_NESTED_CALL;
                TRACE( "nested exception\n" );
                break;
            case ExceptionCollidedUnwind:
                RtlVirtualUnwind( UNW_FLAG_NHANDLER, dispatch.ImageBase,
                                  dispatch.ControlPc, dispatch.FunctionEntry,
                                  &context, &dispatch.HandlerData, &frame, NULL );
                goto unwind_done;
            default:
                return STATUS_INVALID_DISPOSITION;
            }
        }
        /* hack: call wine handlers registered in the tib list */
        else while (is_valid_frame( (ULONG_PTR)teb_frame ) && (ULONG64)teb_frame < context.Gpr1)
        {
            TRACE( "calling TEB handler %p (rec=%p frame=%p context=%p dispatch=%p) sp=%I64x\n",
                   teb_frame->Handler, rec, teb_frame, orig_context, &dispatch, context.Gpr1 );
            res = call_seh_handler( rec, (ULONG_PTR)teb_frame, orig_context,
                                    &dispatch, (PEXCEPTION_ROUTINE)teb_frame->Handler );
            TRACE( "TEB handler at %p returned %lu\n", teb_frame->Handler, res );

            switch (res)
            {
            case ExceptionContinueExecution:
                if (rec->ExceptionFlags & EXCEPTION_NONCONTINUABLE) return STATUS_NONCONTINUABLE_EXCEPTION;
                return STATUS_SUCCESS;
            case ExceptionContinueSearch:
                break;
            case ExceptionNestedException:
                rec->ExceptionFlags |= EXCEPTION_NESTED_CALL;
                TRACE( "nested exception\n" );
                break;
            case ExceptionCollidedUnwind:
                RtlVirtualUnwind( UNW_FLAG_NHANDLER, dispatch.ImageBase,
                                  dispatch.ControlPc, dispatch.FunctionEntry,
                                  &context, &dispatch.HandlerData, &frame, NULL );
                teb_frame = teb_frame->Prev;
                goto unwind_done;
            default:
                return STATUS_INVALID_DISPOSITION;
            }
            teb_frame = teb_frame->Prev;
        }

        if (context.Gpr1 == (ULONG64)NtCurrentTeb()->Tib.StackBase) break;
    }
    return STATUS_UNHANDLED_EXCEPTION;
}


/*******************************************************************
 *		KiUserExceptionDispatcher (NTDLL.@)
 *
 * Entered by a jump from the unix side, NOT by a call.  Contract (ppc64 only,
 * there is no Windows ABI to match):
 *      r3  = EXCEPTION_RECORD *      (on the user stack)
 *      r4  = CONTEXT *               (on the user stack, 16-byte aligned)
 *      r12 = KiUserExceptionDispatcher, so the ELFv2 global entry prologue that
 *            the compiler emits for this C function computes the right TOC
 *      r1  = a 16-byte-aligned user stack pointer with a valid back chain,
 *            below the record and context
 * LR is undefined on entry; this function never returns.
 */
void WINAPI KiUserExceptionDispatcher( EXCEPTION_RECORD *rec, CONTEXT *context )
{
    NTSTATUS status = dispatch_exception( rec, context );
    RtlRaiseStatus( status );
}


/*******************************************************************
 *		KiUserApcDispatcher (NTDLL.@)
 *
 * Entered by a jump from the unix side with the arguments in r3-r8 and r12 =
 * KiUserApcDispatcher; see KiUserExceptionDispatcher for the rationale.
 */
void WINAPI KiUserApcDispatcher( CONTEXT *context, ULONG_PTR arg1, ULONG_PTR arg2, ULONG_PTR arg3,
                                 PNTAPCFUNC func )
{
    func( arg1, arg2, arg3 );
    NtContinue( context, TRUE );
    RtlRaiseStatus( STATUS_ACCESS_VIOLATION );
}


/*******************************************************************
 *		KiUserCallbackDispatcher (NTDLL.@)
 */
void WINAPI KiUserCallbackDispatcher( ULONG id, void *args, ULONG len )
{
    NTSTATUS status = dispatch_user_callback( args, len, id );

    status = NtCallbackReturn( NULL, 0, status );
    RtlRaiseStatus( status );
}


/*******************************************************************
 *              RtlRestoreContext (NTDLL.@)
 */
void CDECL RtlRestoreContext( CONTEXT *context, EXCEPTION_RECORD *rec )
{
    EXCEPTION_REGISTRATION_RECORD *teb_frame = NtCurrentTeb()->Tib.ExceptionList;

    if (rec && rec->ExceptionCode == STATUS_LONGJUMP && rec->NumberParameters >= 1)
    {
        struct _JUMP_BUFFER *jmp = (struct _JUMP_BUFFER *)rec->ExceptionInformation[0];
        int i;

        for (i = 0; i < 18; i++) (&context->Gpr14)[i] = jmp->Gpr[i];
        for (i = 0; i < 18; i++) (&context->Fpr14)[i] = jmp->Fpr[i];
        for (i = 0; i < 12; i++)
        {
            context->Vr[20 + i].Low  = jmp->Vr[2 * i];
            context->Vr[20 + i].High = jmp->Vr[2 * i + 1];
        }
        context->Gpr1 = jmp->Sp;
        context->Gpr2 = jmp->Toc;
        context->Cr   = jmp->Cr;
        context->Lr   = jmp->Lr;
        context->Iar  = jmp->Lr;
    }
    else if (rec && rec->ExceptionCode == STATUS_UNWIND_CONSOLIDATE && rec->NumberParameters >= 1)
    {
        PVOID (CALLBACK *consolidate)(EXCEPTION_RECORD *) = (void *)rec->ExceptionInformation[0];
        TRACE( "calling consolidate callback %p (rec=%p)\n", consolidate, rec );
        /* STUB: the other architectures run the callback on a synthesised frame
         * (consolidate_callback/invoke_callback) so that an RtlUnwindEx issued
         * from inside it - which C++ handlers do - skips the frames already
         * processed.  That trick needs a resumable CONTEXT, and the back-chain
         * unwinder cannot produce one.  Called directly instead: a nested
         * unwind from the callback will re-walk frames it has already unwound. */
        context->Iar = (ULONG64)consolidate( rec );
        context->Lr  = context->Iar;
        context->Gpr12 = context->Iar;   /* see RtlUnwindEx: ELFv2 global entry needs r12 */
    }

    /* hack: remove no longer accessible TEB frames */
    while (is_valid_frame( (ULONG_PTR)teb_frame ) && (ULONG64)teb_frame < context->Gpr1)
    {
        TRACE( "removing TEB frame: %p\n", teb_frame );
        teb_frame = __wine_pop_frame( teb_frame );
    }

    TRACE( "returning to %I64x stack %I64x\n", context->Iar, context->Gpr1 );
    NtContinue( context, FALSE );
}


/*******************************************************************
 *		RtlUnwindEx (NTDLL.@)
 */
void WINAPI RtlUnwindEx( PVOID end_frame, PVOID target_ip, EXCEPTION_RECORD *rec,
                         PVOID retval, CONTEXT *context, UNWIND_HISTORY_TABLE *table )
{
    EXCEPTION_REGISTRATION_RECORD *teb_frame = NtCurrentTeb()->Tib.ExceptionList;
    EXCEPTION_RECORD record;
    DISPATCHER_CONTEXT dispatch;
    CONTEXT new_context;
    NTSTATUS status;
    ULONG_PTR frame;
    DWORD i, res;

    RtlCaptureContext( context );
    new_context = *context;

    /* build an exception record, if we do not have one */
    if (!rec)
    {
        record.ExceptionCode    = STATUS_UNWIND;
        record.ExceptionFlags   = 0;
        record.ExceptionRecord  = NULL;
        record.ExceptionAddress = (void *)context->Iar;
        record.NumberParameters = 0;
        rec = &record;
    }

    rec->ExceptionFlags |= EXCEPTION_UNWINDING | (end_frame ? 0 : EXCEPTION_EXIT_UNWIND);

    TRACE( "code=%lx flags=%lx end_frame=%p target_ip=%p\n",
           rec->ExceptionCode, rec->ExceptionFlags, end_frame, target_ip );
    for (i = 0; i < min( EXCEPTION_MAXIMUM_PARAMETERS, rec->NumberParameters ); i++)
        TRACE( " info[%ld]=%016I64x\n", i, rec->ExceptionInformation[i] );
    TRACE_CONTEXT( context );

    dispatch.TargetPc         = (ULONG64)target_ip;
    dispatch.ContextRecord    = context;
    dispatch.HistoryTable     = table;
    dispatch.NonVolatileRegisters = NULL;

    for (;;)
    {
        status = virtual_unwind( UNW_FLAG_UHANDLER, &dispatch, &new_context );
        if (status != STATUS_SUCCESS) raise_status( status, rec );

    unwind_done:
        if (!dispatch.EstablisherFrame) break;

        if (!is_valid_frame( dispatch.EstablisherFrame ))
        {
            ERR( "invalid frame %I64x (%p-%p)\n", dispatch.EstablisherFrame,
                 NtCurrentTeb()->Tib.StackLimit, NtCurrentTeb()->Tib.StackBase );
            rec->ExceptionFlags |= EXCEPTION_STACK_INVALID;
            break;
        }

        if (dispatch.LanguageHandler)
        {
            if (end_frame && (dispatch.EstablisherFrame > (ULONG64)end_frame))
            {
                ERR( "invalid end frame %I64x/%p\n", dispatch.EstablisherFrame, end_frame );
                raise_status( STATUS_INVALID_UNWIND_TARGET, rec );
            }
            if (dispatch.EstablisherFrame == (ULONG64)end_frame) rec->ExceptionFlags |= EXCEPTION_TARGET_UNWIND;

            res = call_unwind_handler( rec, dispatch.EstablisherFrame, dispatch.ContextRecord,
                                       &dispatch, dispatch.LanguageHandler );
            TRACE( "handler %p returned %lx\n", dispatch.LanguageHandler, res );

            switch (res)
            {
            case ExceptionContinueSearch:
                rec->ExceptionFlags &= ~EXCEPTION_COLLIDED_UNWIND;
                break;
            case ExceptionCollidedUnwind:
                new_context = *context;
                RtlVirtualUnwind( UNW_FLAG_NHANDLER, dispatch.ImageBase,
                                  dispatch.ControlPc, dispatch.FunctionEntry,
                                  &new_context, &dispatch.HandlerData, &frame, NULL );
                rec->ExceptionFlags |= EXCEPTION_COLLIDED_UNWIND;
                goto unwind_done;
            default:
                raise_status( STATUS_INVALID_DISPOSITION, rec );
                break;
            }
        }
        else  /* hack: call builtin handlers registered in the tib list */
        {
            ULONG_PTR last_frame = new_context.Gpr1;
            if (end_frame && (ULONG_PTR)end_frame < last_frame) last_frame = (ULONG_PTR)end_frame;

            while (is_valid_frame( (ULONG_PTR)teb_frame ) && (ULONG_PTR)teb_frame < last_frame)
            {
                TRACE( "calling TEB handler %p (rec=%p, frame=%p context=%p, dispatch=%p)\n",
                       teb_frame->Handler, rec, teb_frame, dispatch.ContextRecord, &dispatch );
                res = call_unwind_handler( rec, (ULONG_PTR)teb_frame, dispatch.ContextRecord, &dispatch,
                                           (PEXCEPTION_ROUTINE)teb_frame->Handler );
                TRACE( "handler at %p returned %lu\n", teb_frame->Handler, res );
                teb_frame = __wine_pop_frame( teb_frame );

                switch (res)
                {
                case ExceptionContinueSearch:
                    rec->ExceptionFlags &= ~EXCEPTION_COLLIDED_UNWIND;
                    break;
                case ExceptionCollidedUnwind:
                    new_context = *context;
                    RtlVirtualUnwind( UNW_FLAG_NHANDLER, dispatch.ImageBase,
                                      dispatch.ControlPc, dispatch.FunctionEntry,
                                      &new_context, &dispatch.HandlerData,
                                      &frame, NULL );
                    rec->ExceptionFlags |= EXCEPTION_COLLIDED_UNWIND;
                    goto unwind_done;
                default:
                    raise_status( STATUS_INVALID_DISPOSITION, rec );
                    break;
                }
            }
            if ((ULONG_PTR)teb_frame == last_frame && last_frame < new_context.Gpr1) break;
        }

        if (dispatch.EstablisherFrame == (ULONG64)end_frame) break;
        *context = new_context;
    }

    if (rec->ExceptionCode != STATUS_UNWIND_CONSOLIDATE)
    {
        context->Iar = (ULONG64)target_ip;
        context->Lr  = (ULONG64)target_ip;
        /* An ELFv2 global entry point begins "addis r2,r12,.TOC.-f@ha; addi
         * r2,r2,.TOC.-f@l", so resuming at the entry of a function reached
         * through a function pointer - which is what winecrt0's unwind_target
         * is - only computes the right TOC if r12 holds that entry address.
         * Nothing else on this path sets it: virtual_unwind() walks the back
         * chain and recovers Iar, Lr and Gpr1 only, so Gpr12 would otherwise
         * still be RtlUnwindEx's own.  Measured: leaving it produced a garbage
         * r2, a garbage indirect branch out of unwind_target, and an endless
         * fault loop during kernel32's PROCESS_ATTACH.
         * r12 is volatile, so writing it is harmless when target_ip is an
         * ordinary label inside a function rather than an entry point. */
        context->Gpr12 = (ULONG64)target_ip;
    }

    context->Gpr3 = (ULONG64)retval;
    RtlRestoreContext( context, rec );
}


/*************************************************************************
 *		RtlGetNativeSystemInformation (NTDLL.@)
 */
NTSTATUS WINAPI RtlGetNativeSystemInformation( SYSTEM_INFORMATION_CLASS class,
                                               void *info, ULONG size, ULONG *ret_size )
{
    return NtQuerySystemInformation( class, info, size, ret_size );
}


/***********************************************************************
 *           RtlIsProcessorFeaturePresent [NTDLL.@]
 *
 * The PF_* feature numbers are all x86/ARM specific; none of them describes a
 * PowerPC capability, so nothing is ever present.  Stub, deliberately.
 */
BOOLEAN WINAPI RtlIsProcessorFeaturePresent( UINT feature )
{
    return FALSE;
}


/*************************************************************************
 *		RtlWalkFrameChain (NTDLL.@)
 */
ULONG WINAPI RtlWalkFrameChain( void **buffer, ULONG count, ULONG flags )
{
    UNWIND_HISTORY_TABLE table;
    RUNTIME_FUNCTION *func;
    PEXCEPTION_ROUTINE handler;
    ULONG_PTR pc, frame, base;
    CONTEXT context;
    void *data;
    ULONG i, skip = flags >> 8, num_entries = 0;

    RtlCaptureContext( &context );

    for (i = 0; i < count; i++)
    {
        pc = context.Iar;
        if (context.ContextFlags & CONTEXT_UNWOUND_TO_CALL) pc -= 4;
        func = RtlLookupFunctionEntry( pc, &base, &table );
        if (RtlVirtualUnwind2( UNW_FLAG_NHANDLER, base, pc, func, &context, NULL,
                               &data, &frame, NULL, NULL, NULL, &handler, 0 ))
            break;
        if (!context.Iar) break;
        if (!frame || !is_valid_frame( frame )) break;
        if (context.Gpr1 == (ULONG_PTR)NtCurrentTeb()->Tib.StackBase) break;
        if (i >= skip) buffer[num_entries++] = (void *)context.Iar;
    }
    return num_entries;
}


/***********************************************************************
 *		__C_ExecuteExceptionFilter
 *
 * Only reachable from __C_specific_handler, which only ever runs against PE
 * unwind data.  There is none on this architecture, so this cannot be called;
 * it exists so that unwind.c links.  Non-volatile registers are NOT reloaded
 * from the DISPATCHER_CONTEXT block (r6), because nothing populates it.
 */
LONG WINAPI __C_ExecuteExceptionFilter( void *record, void *frame,
                                        PEXCEPTION_FILTER filter, BYTE *nonvolatile )
{
    ERR( "not supported on PowerPC64\n" );
    return EXCEPTION_CONTINUE_SEARCH;
}


/***********************************************************************
 *		RtlRaiseException (NTDLL.@)
 *
 * Assembly wrapper: capture the caller's context, point it at the call site
 * rather than at RtlCaptureContext, and hand it to raise_exception_from_asm().
 * The 0x4c0-byte frame is CONTEXT (0x4a0, 16-byte aligned at offset 0x20 from
 * the new r1) plus the 32-byte ELFv2 linkage area.
 *
 * The two __ASM_CFI() directives are required, not cosmetic: __ASM_GLOBAL_FUNC
 * always emits .cfi_startproc/.cfi_endproc, so with no directives between them
 * the FDE is empty, and an empty FDE claims CFA = current r1 and "return address
 * still in lr" -- both false once the stdu has run.  glibc's forced unwind
 * (pthread_exit, cancellation) loops forever on a frame like that, burning a core
 * for the life of the process while the thread join still succeeds.  This frame
 * stays live across the whole of raise_exception_from_asm() below, which is
 * reached by a plain branch, so it is on the stack for the entire exception
 * dispatch.  (No FDE at all would have been safe; libgcc returns END_OF_STACK.)
 * probes/empty-fde-scan.py enumerates frames in this state and is a gate
 * component -- see probes/check-empty-fde.sh.
 */
extern void DECLSPEC_NORETURN raise_exception_from_asm( EXCEPTION_RECORD *rec, CONTEXT *context );
__ASM_GLOBAL_FUNC( RtlRaiseException,
                   "addis 2, 12, .TOC.-" __ASM_NAME("RtlRaiseException") "@ha\n\t"
                   "addi 2, 2, .TOC.-" __ASM_NAME("RtlRaiseException") "@l\n\t"
                   ".localentry " __ASM_NAME("RtlRaiseException") ", .-" __ASM_NAME("RtlRaiseException") "\n\t"
                   "mflr 0\n\t"
                   "std 0, 16(1)\n\t"
                   __ASM_CFI(".cfi_offset 65, 16\n\t")
                   "std 3, -8(1)\n\t"            /* stash rec in the red zone */
                   "stdu 1, -0x4e0(1)\n\t"
                   __ASM_CFI(".cfi_def_cfa_offset 0x4e0\n\t")
                   "addi 3, 1, 0x20\n\t"         /* &context */
                   "bl " __ASM_NAME("RtlCaptureContext") "\n\t"
                   "addi 4, 1, 0x20\n\t"         /* context */
                   "ld 3, 0x4d8(1)\n\t"          /* rec, from the red-zone stash */
                   "addi 5, 1, 0x4e0\n\t"
                   "std 5, 0x110(4)\n\t"         /* context->Gpr1 = caller's sp */
                   "std 3, 0x120(4)\n\t"         /* context->Gpr3 = rec */
                   "ld 0, 0x4f0(1)\n\t"          /* caller's LR save slot */
                   "std 0, 0x220(4)\n\t"         /* context->Iar */
                   "std 0, 0x228(4)\n\t"         /* context->Lr */
                   "std 0, 0x10(3)\n\t"          /* rec->ExceptionAddress */
                   "ld 0, 0x238(4)\n\t"
                   "oris 0, 0, 0x2000\n\t"       /* CONTEXT_UNWOUND_TO_CALL */
                   "std 0, 0x238(4)\n\t"
                   "b " __ASM_NAME("raise_exception_from_asm") )

void DECLSPEC_NORETURN raise_exception_from_asm( EXCEPTION_RECORD *rec, CONTEXT *context )
{
    NTSTATUS status;

    if (!NtCurrentTeb()->Peb->BeingDebugged) status = dispatch_exception( rec, context );
    else status = NtRaiseException( rec, context, TRUE );
    RtlRaiseStatus( status );
}


/***********************************************************************
 *           _setjmpex (NTDLL.@)
 *
 * Layout must stay identical to libs/winecrt0/setjmp.c and to the _JUMP_BUFFER
 * in include/msvcrt/setjmp.h.
 */
__ASM_GLOBAL_FUNC( NTDLL__setjmpex,
                   "std  4, 0(3)\n\t"       /* Frame */
                   "std 14, 8(3)\n\t"
                   "std 15, 16(3)\n\t"
                   "std 16, 24(3)\n\t"
                   "std 17, 32(3)\n\t"
                   "std 18, 40(3)\n\t"
                   "std 19, 48(3)\n\t"
                   "std 20, 56(3)\n\t"
                   "std 21, 64(3)\n\t"
                   "std 22, 72(3)\n\t"
                   "std 23, 80(3)\n\t"
                   "std 24, 88(3)\n\t"
                   "std 25, 96(3)\n\t"
                   "std 26, 104(3)\n\t"
                   "std 27, 112(3)\n\t"
                   "std 28, 120(3)\n\t"
                   "std 29, 128(3)\n\t"
                   "std 30, 136(3)\n\t"
                   "std 31, 144(3)\n\t"
                   "std  1, 152(3)\n\t"     /* Sp */
                   "std  2, 160(3)\n\t"     /* Toc */
                   "mfcr 0\n\t"
                   "std  0, 168(3)\n\t"     /* Cr */
                   "mflr 0\n\t"
                   "std  0, 176(3)\n\t"     /* Lr */
                   "stfd 14, 184(3)\n\t"
                   "stfd 15, 192(3)\n\t"
                   "stfd 16, 200(3)\n\t"
                   "stfd 17, 208(3)\n\t"
                   "stfd 18, 216(3)\n\t"
                   "stfd 19, 224(3)\n\t"
                   "stfd 20, 232(3)\n\t"
                   "stfd 21, 240(3)\n\t"
                   "stfd 22, 248(3)\n\t"
                   "stfd 23, 256(3)\n\t"
                   "stfd 24, 264(3)\n\t"
                   "stfd 25, 272(3)\n\t"
                   "stfd 26, 280(3)\n\t"
                   "stfd 27, 288(3)\n\t"
                   "stfd 28, 296(3)\n\t"
                   "stfd 29, 304(3)\n\t"
                   "stfd 30, 312(3)\n\t"
                   "stfd 31, 320(3)\n\t"
                   "li  11, 336\n\t"    /* v20-v31; r11, not r0: see RtlCaptureContext */
                   "stvx 20, 3, 11\n\t" "addi 11, 11, 16\n\t"
                   "stvx 21, 3, 11\n\t" "addi 11, 11, 16\n\t"
                   "stvx 22, 3, 11\n\t" "addi 11, 11, 16\n\t"
                   "stvx 23, 3, 11\n\t" "addi 11, 11, 16\n\t"
                   "stvx 24, 3, 11\n\t" "addi 11, 11, 16\n\t"
                   "stvx 25, 3, 11\n\t" "addi 11, 11, 16\n\t"
                   "stvx 26, 3, 11\n\t" "addi 11, 11, 16\n\t"
                   "stvx 27, 3, 11\n\t" "addi 11, 11, 16\n\t"
                   "stvx 28, 3, 11\n\t" "addi 11, 11, 16\n\t"
                   "stvx 29, 3, 11\n\t" "addi 11, 11, 16\n\t"
                   "stvx 30, 3, 11\n\t" "addi 11, 11, 16\n\t"
                   "stvx 31, 3, 11\n\t"
                   "li 3, 0\n\t"
                   "blr" )


/*******************************************************************
 *		longjmp (NTDLL.@)
 */
void __cdecl NTDLL_longjmp( _JUMP_BUFFER *buf, int retval )
{
    EXCEPTION_RECORD rec;

    if (!retval) retval = 1;

    rec.ExceptionCode = STATUS_LONGJUMP;
    rec.ExceptionFlags = 0;
    rec.ExceptionRecord = NULL;
    rec.ExceptionAddress = NULL;
    rec.NumberParameters = 1;
    rec.ExceptionInformation[0] = (DWORD_PTR)buf;
    RtlUnwind( (void *)buf->Frame, (void *)buf->Lr, &rec, IntToPtr(retval) );
}


/***********************************************************************
 *           guest_module_from_address
 *
 * The loaded AMD64 module containing an address, or NULL.  This is what
 * "is this pointer guest code" means on this port: guest code lives in guest
 * images, and native code never does.  Callers must hold the loader lock.
 *
 * It cannot see guest code outside any module -- a packer or JIT writing into
 * anonymous guest memory -- so callers that must not guess treat "in no module
 * at all" as a distinct case rather than as "native".
 */
static HMODULE guest_module_from_address( const void *addr )
{
    LIST_ENTRY *mark, *entry;

    mark = &NtCurrentTeb()->Peb->LdrData->InMemoryOrderModuleList;
    for (entry = mark->Flink; entry != mark; entry = entry->Flink)
    {
        LDR_DATA_TABLE_ENTRY *mod = CONTAINING_RECORD( entry, LDR_DATA_TABLE_ENTRY, InMemoryOrderLinks );
        const IMAGE_NT_HEADERS *nt = RtlImageNtHeader( mod->DllBase );
        const char *base = (const char *)mod->DllBase;

        if (!nt || nt->FileHeader.Machine != IMAGE_FILE_MACHINE_AMD64) continue;
        if ((const char *)addr >= base && (const char *)addr < base + nt->OptionalHeader.SizeOfImage)
            return mod->DllBase;
    }
    return NULL;
}

/***********************************************************************
 *           thread_start_is_guest_code
 *
 * Whether a thread start routine is guest code the native cpu cannot execute.
 *
 * Any loaded AMD64 module counts, not just the main image: a guest that calls
 * CreateThread with a start routine in one of its own DLLs is the same case,
 * and the main image is simply one such module -- so this is one rule rather
 * than a special case plus a gap.
 *
 * A start routine in NO module is the case this cannot classify: guest code
 * generated into anonymous memory looks exactly like a native pointer here.
 * Guessing either way is wrong, so say so and refuse the thread -- a diagnosed
 * failure rather than executing x86-64 bytes as ppc64, which is what happened
 * before.  If that class ever matters the fix is registration-side wrapping of
 * CreateThread, which composes with this rather than replacing it.
 */
static BOOL thread_start_is_guest_code( const void *entry, BOOL *unclassifiable )
{
    const IMAGE_NT_HEADERS *nt = RtlImageNtHeader( NtCurrentTeb()->Peb->ImageBaseAddress );
    ULONG_PTR magic;
    BOOL ret;

    *unclassifiable = FALSE;
    LdrLockLoaderLock( 0, NULL, &magic );
    ret = guest_module_from_address( entry ) != NULL;
    if (!ret && nt && nt->FileHeader.Machine == IMAGE_FILE_MACHINE_AMD64 &&
        !RtlPcToFileHeader( (void *)entry, (void **)&nt ))
    {
        /* guest process, but the entry belongs to no loaded image at all */
        *unclassifiable = TRUE;
    }
    LdrUnlockLoaderLock( 0, magic );
    return ret;
}


/***********************************************************************
 *           guest import thunks
 *
 * A guest AMD64 image binds its imports to AMD64 thunk modules served from
 * that machine's own system directory (see get_machine_system_dir() in
 * loader.c), whose exported functions are nothing but a five-byte stub ending
 * in the emulator bridge's trap opcode.  Executing one traps out with the
 * complete guest register file, which at that instant is exactly the MS-x64
 * argument-passing state of the call the guest just made -- so converting it
 * to an ELFv2 call is a plain C call with the arguments read out of the
 * CONTEXT.  Nothing has to be copied: the emulator runs guest code in this
 * process's address space, so a guest pointer IS a host pointer and a guest
 * UTF-16 string IS a WCHAR string.
 *
 * The native implementation is then the same-named export of the same-named
 * module in the native machine's namespace, which the two are kept in by the
 * same directory split that chose the thunk in the first place.
 */

/* Published by a thunk module through its __wine_thunk_info export. */
struct thunk_info
{
    UINT version;         /* 4 */
    UINT count;           /* number of stubs */
    UINT stubs_rva;       /* stub i is at stubs_rva + i * stride */
    UINT stride;
    UINT names_rva;       /* count RVAs of NUL-terminated ASCII export names */
    UINT sigs_rva;        /* count descriptors, see below */
    UINT trap_off;        /* offset of the trapping instruction within a stub */
    UINT impl_names_rva;  /* count RVAs of the name to resolve natively: the
                             same string as names_rva except for a variadic,
                             where it names the callee's v-variant */
    UINT fp_rva;          /* count floating-point descriptors, see THUNK_FP_*
                             below.  Added at version 5; the version check is
                             an exact match, so a module either has this field
                             or is rejected outright -- there is no mixed
                             build to be compatible with. */
};

#define THUNK_INFO_VERSION   5
#define THUNK_SIG_ARGC(sig)  ((sig) & 0xff)   /* for a variadic, the FIXED count */
#define THUNK_SIG_VOID       0x100u           /* returns void */
#define THUNK_SIG_VARIADIC   0x200u           /* synthesise a va_list, see below */
#define THUNK_SIG_RESERVED   0x0000fc00u      /* must be zero */
/* Bit 16+i set: argument i is a 32-bit slot, measured by the generator's
 * clang oracle from the parameter's type on the guest target (NOT from the
 * .spec argument class, whose `long` names plenty of HANDLEs).  An MS-x64
 * caller stores such an argument with a 32-bit store, leaving whatever was on
 * the stack in the slot's upper half -- and PE-side code on this port is
 * LP64, so a native ULONG parameter reads all 64 bits of it.  Measured:
 * mspatcha's ApplyPatchToFileByBuffers received a poisoned buffer size,
 * concluded the caller's buffer was big enough, and the LZXD decoder ran off
 * the end of a guest stack buffer into the stack's top guard.  Zero-extension
 * matches what the x86-64 register file already does to the four register
 * arguments (writing a 32-bit register clears the upper half), so narrow
 * stack slots simply become consistent with narrow register slots.  The mask
 * carries no signedness, so a negative 32-bit value still reaches an LP64
 * `long` parameter zero- rather than sign-extended -- a known limitation of
 * the LP64 PE side, not of this mask. */
#define THUNK_SIG_NARROW(sig) (((sig) >> 16) & 0xffffu)

/* ---------------------------------------------------------------------------
 * The floating-point descriptor (version 5), one UINT per export.
 * ---------------------------------------------------------------------------
 * A separate word rather than more bits in `sig` because sig is full: bits
 * 0-9 are arity and flags, 16-31 are the narrow mask, and what is left cannot
 * hold a per-argument mask plus a return kind.
 *
 * Zero means "no floating point anywhere", which is every export this port
 * emitted before version 5 and the overwhelming majority after it, so the
 * integer path stays exactly as it was.
 *
 * Only the first 8 arguments can be floating point.  MS-x64 has just XMM0-3
 * for arguments and ELFv2 stops at f13; the oracle refuses anything past the
 * eighth rather than have this word silently describe less than the truth. */
#define THUNK_FP_MASK(fp)    ((fp) & 0xffu)          /* bit i: argument i is FP */
#define THUNK_FP_SINGLE(fp)  (((fp) >> 8) & 0xffu)   /* bit i: that FP arg is a
                                                        float, not a double */
#define THUNK_FP_RET(fp)     (((fp) >> 16) & 0x3u)   /* 0 none, 1 double, 2 float */
#define THUNK_FP_RET_NONE    0u
#define THUNK_FP_RET_DOUBLE  1u
#define THUNK_FP_RET_FLOAT   2u

#define THUNK_MAX_ARGS 16
#define THUNK_MAX_FP_ARGS 8

/* FEXBRIDGE_TRAP_* */
#define GUEST_TRAP_CONTINUE 0
#define GUEST_TRAP_EXIT     1

/***********************************************************************
 *           guest namespace overrides
 *
 * Almost every thunked export is pass-through: read the guest's arguments out
 * of the CONTEXT and call the identically-named native export, since guest
 * memory is host memory and a HANDLE is an opaque token either way.
 *
 * A few cannot be, because what they return only means anything in one
 * machine's namespace.  GetProcAddress is the sharp one: pass-through hands
 * the guest the address of native ppc64 code, which the guest then CALLs --
 * ppc64 bytes decoded as x86-64.  What the guest actually asked for is the
 * address of the matching stub in its OWN thunk module, which is a perfectly
 * good guest-callable address.  So these few are implemented here against the
 * guest module list instead of being forwarded.
 *
 * An override takes the already-marshalled argument slots, so it sees exactly
 * what the guest passed.
 */
/* An override receives the marshalled arguments and the native export it
 * stands in for, so it can either replace the native call or wrap it.  NULL
 * native means the name did not resolve natively -- an override that needs it
 * must check. */
typedef ULONG_PTR (*thunk_override_func)( const ULONG_PTR *a, void *native );

struct thunk_override
{
    const WCHAR         *module;
    const char          *name;
    UINT                 argc;
    thunk_override_func  func;     /* replaces the native call; NULL means the
                                      row only rewrites arguments */
    UINT                 cb_mask;  /* bit i set: argument i is a guest callback,
                                      swapped for a trampoline at registration
                                      (see wrap_guest_callback) */
};

/***********************************************************************
 *           find_guest_module
 *
 * A loaded module of the guest machine, by base name.  Deliberately does not
 * fall back to the native namespace: a guest asking for "kernel32" must get
 * its own kernel32 or nothing.
 */


static HMODULE find_guest_module( const WCHAR *name )
{
    WCHAR buf[64];
    UNICODE_STRING want;
    LIST_ENTRY *mark, *entry;

    if (!wcschr( name, '.' ))  /* GetModuleHandle appends .dll when there is no extension */
    {
        if (wcslen( name ) + 5 > ARRAY_SIZE(buf)) return NULL;
        wcscpy( buf, name );
        wcscat( buf, L".dll" );
        name = buf;
    }
    RtlInitUnicodeString( &want, name );

    mark = &NtCurrentTeb()->Peb->LdrData->InMemoryOrderModuleList;
    for (entry = mark->Flink; entry != mark; entry = entry->Flink)
    {
        LDR_DATA_TABLE_ENTRY *mod = CONTAINING_RECORD( entry, LDR_DATA_TABLE_ENTRY, InMemoryOrderLinks );
        const IMAGE_NT_HEADERS *nt = RtlImageNtHeader( mod->DllBase );

        if (!nt || nt->FileHeader.Machine != IMAGE_FILE_MACHINE_AMD64) continue;
        if (RtlEqualUnicodeString( &want, &mod->BaseDllName, TRUE )) return mod->DllBase;
    }
    return NULL;
}

static ULONG_PTR emu_GetModuleHandleW( const ULONG_PTR *a, void *native )
{
    /* NULL means "the main image", which is the guest exe either way */
    if (!a[0]) return (ULONG_PTR)NtCurrentTeb()->Peb->ImageBaseAddress;
    return (ULONG_PTR)find_guest_module( (const WCHAR *)a[0] );
}

static ULONG_PTR emu_GetModuleHandleA( const ULONG_PTR *a, void *native )
{
    UNICODE_STRING str;
    ANSI_STRING ansi;
    ULONG_PTR ret;

    if (!a[0]) return (ULONG_PTR)NtCurrentTeb()->Peb->ImageBaseAddress;
    RtlInitAnsiString( &ansi, (const char *)a[0] );
    if (RtlAnsiStringToUnicodeString( &str, &ansi, TRUE )) return 0;
    ret = (ULONG_PTR)find_guest_module( str.Buffer );
    RtlFreeUnicodeString( &str );
    return ret;
}

static ULONG_PTR emu_GetProcAddress( const ULONG_PTR *a, void *native )
{
    const IMAGE_NT_HEADERS *nt = RtlImageNtHeader( (HMODULE)a[0] );
    ANSI_STRING name;
    void *proc;

    /* The result is going to be CALLED by guest code, so only a guest module
     * can answer.  Handing back a native address would crash later and a long
     * way from here, so refuse instead -- the guest sees NULL, which is a
     * documented outcome its caller already has to handle. */
    if (!nt || nt->FileHeader.Machine != IMAGE_FILE_MACHINE_AMD64)
    {
        WARN( "GetProcAddress(%p) is not a guest module, refusing\n", (void *)a[0] );
        return 0;
    }
    if (a[1] >> 16)
    {
        RtlInitAnsiString( &name, (const char *)a[1] );
        if (LdrGetProcedureAddress( (HMODULE)a[0], &name, 0, &proc )) return 0;
    }
    else if (LdrGetProcedureAddress( (HMODULE)a[0], NULL, (ULONG)a[1], &proc )) return 0;

    return (ULONG_PTR)proc;
}

/* LoadLibrary answers in a namespace too: the guest wants a module whose
 * exports IT can call, so resolve the name against the guest machine's own
 * loader namespace (load_guest_dll -> the machine's builtin thunk modules),
 * exactly as its static imports were resolved.  Passing through to native
 * LoadLibrary hands back a native HMODULE, and the guest's next step is
 * GetProcAddress on it -- whose override then rightly refuses, turning every
 * dynamically-resolved API into a spurious "not available".  No guest module
 * of that name means NULL, the documented outcome the caller already handles. */
static ULONG_PTR load_guest_library( const WCHAR *name )
{
    HMODULE mod;
    WCHAR *apiset;
    ULONG_PTR ret;

    if (!name) return 0;
    /* An apiset is a name for another module, and the guest namespace has no
     * module of that name -- so resolve it first, exactly as a static import
     * does through build_import_name().  Without this every runtime probe for
     * an api-ms-win-* set answered NULL, and a caller that treats NULL as
     * "this platform lacks the feature" quietly configured itself wrong. */
    if ((apiset = get_apiset_target_name( name )))
    {
        TRACE( "apiset %s -> %s\n", debugstr_w(name), debugstr_w(apiset) );
        ret = load_guest_library( apiset );
        RtlFreeHeap( GetProcessHeap(), 0, apiset );
        return ret;
    }
    if (wcschr( name, '\\' ) || wcschr( name, '/' ))
    {
        FIXME( "path %s not resolvable in the guest namespace, refusing\n", debugstr_w(name) );
        return 0;
    }
    if (load_guest_dll( name, IMAGE_FILE_MACHINE_AMD64, &mod ))
    {
        WARN( "no guest %s\n", debugstr_w(name) );
        return 0;
    }
    return (ULONG_PTR)mod;
}

static ULONG_PTR emu_LoadLibraryW( const ULONG_PTR *a, void *native )
{
    return load_guest_library( (const WCHAR *)a[0] );
}

/* The Ex forms need the same treatment, and missing them is not academic: a
 * modern binary almost never calls plain LoadLibraryW.  Quake II went
 * LoadLibraryExW -> native HMODULE -> GetProcAddress -> refused (correctly,
 * it is not a guest module) -> NULL into its own function-pointer table ->
 * a null-vtable call 5000 traps later, with nothing in between naming the
 * cause.
 *
 * The flags are deliberately not forwarded, with one exception.  They select
 * a SEARCH PATH, and the guest namespace is resolved by name rather than by
 * path, so there is nothing for LOAD_LIBRARY_SEARCH_* to change.  A DATAFILE
 * or IMAGE_RESOURCE load is different in kind: the caller wants a resource
 * handle and will never call GetProcAddress on it, and a guest thunk module
 * carries no resources -- so that one passes through to native. */
#define LOAD_LIBRARY_DATAFILE_ANY 0x00000062u  /* AS_DATAFILE | AS_IMAGE_RESOURCE
                                                  | AS_DATAFILE_EXCLUSIVE */

static ULONG_PTR emu_LoadLibraryExW( const ULONG_PTR *a, void *native )
{
    if (a[2] & LOAD_LIBRARY_DATAFILE_ANY)
    {
        if (!native) return 0;
        return ((ULONG_PTR (*)( ULONG_PTR, ULONG_PTR, ULONG_PTR ))native)( a[0], a[1], a[2] );
    }
    return load_guest_library( (const WCHAR *)a[0] );
}

static ULONG_PTR emu_LoadLibraryExA( const ULONG_PTR *a, void *native )
{
    UNICODE_STRING str;
    ANSI_STRING ansi;
    ULONG_PTR ret;

    if (a[2] & LOAD_LIBRARY_DATAFILE_ANY)
    {
        if (!native) return 0;
        return ((ULONG_PTR (*)( ULONG_PTR, ULONG_PTR, ULONG_PTR ))native)( a[0], a[1], a[2] );
    }
    if (!a[0]) return 0;
    RtlInitAnsiString( &ansi, (const char *)a[0] );
    if (RtlAnsiStringToUnicodeString( &str, &ansi, TRUE )) return 0;
    ret = load_guest_library( str.Buffer );
    RtlFreeUnicodeString( &str );
    return ret;
}

static ULONG_PTR emu_LoadLibraryA( const ULONG_PTR *a, void *native )
{
    UNICODE_STRING str;
    ANSI_STRING ansi;
    ULONG_PTR ret;

    if (!a[0]) return 0;
    RtlInitAnsiString( &ansi, (const char *)a[0] );
    if (RtlAnsiStringToUnicodeString( &str, &ansi, TRUE )) return 0;
    ret = load_guest_library( str.Buffer );
    RtlFreeUnicodeString( &str );
    return ret;
}

void WINAPI emu_trap_dispatch( ULONG id, void *args, ULONG len );

/***********************************************************************
 *           call_guest_function
 *
 * Run a guest procedure on this thread and return its RAX.
 *
 * Deliberately the SAME primitive the main image and guest threads use rather
 * than a second way into the bridge: unix_emu_run_entry adopts a bridge handle
 * for whatever thread calls it and sets that thread's GS base, so a native
 * worker thread invoking a guest callback needs no thread-specific code here.
 *
 * On a thread already inside guest code this nests, which the bridge supports
 * provided the caller does not disturb the outer guest register file.  It does
 * not: the outer state lives in the trap CONTEXT, which the bridge writes back
 * when the trap returns TRAP_CONTINUE.  The nested run gets its own guest
 * stack from run_entry, so the outer guest frame is untouched too.
 */
static void raise_pending_guest_exception(void);
void WINAPI emu_exception_dispatch( ULONG id, void *args, ULONG len );

static ULONG_PTR call_guest_function( void *entry, void *arg )
{
    struct emu_run_entry_params params = { entry, arg, 0, emu_trap_dispatch };
    NTSTATUS status;

    params.exception_dispatcher = emu_exception_dispatch;
    status = WINE_UNIX_CALL( unix_emu_run_entry, &params );

    if (status)
    {
        /* a guest exception the nested run could not consume propagates
         * natively from here -- the callback's native caller and the
         * unhandled machinery above it get their shot with their own
         * machine's context.  Does not return when one was pending. */
        raise_pending_guest_exception();
        ERR( "guest callback %p failed, status %08x\n", entry, (UINT)status );
        return 0;
    }
    return (ULONG_PTR)params.retval;
}


/***********************************************************************
 *           call_guest_function_args
 *
 * Run a guest procedure with up to four integer arguments and return its RAX.
 *
 * The run-entry primitive carries exactly one argument (RCX), so the one
 * argument is a parameter block, and a six-instruction guest thunk unpacks it
 * and tail-jumps to the real target -- the tail jump leaves RSP exactly as
 * run_entry aligned it, MS-x64 shadow space included, so the target returns
 * straight into run_entry's own return path with its RAX intact.
 *
 * Four arguments always travel, whatever the target's real arity: an MS-x64
 * callee ignores argument registers beyond its own parameters, so a
 * two-argument comparator called with garbage in R8/R9 cannot observe it.
 * That keeps the block layout fixed and spares every caller from carrying an
 * arity.  A callback with stack arguments (five or more) would need a thunk
 * that builds a frame; nothing in the corpus has one, and this is where it
 * would go.
 *
 * The thunk is written once into anonymous executable memory.  It is only
 * ever entered through call_guest_function, so it needs no classification by
 * guest_module_from_address, and a freshly mapped page can have no stale
 * translation to invalidate.  Publication is a compare-exchange: concurrent
 * first callers race benignly, the loser frees its copy.
 */
static ULONG_PTR call_guest_function_args( void *fn, ULONG_PTR a0, ULONG_PTR a1,
                                           ULONG_PTR a2, ULONG_PTR a3 )
{
    static const BYTE thunk_code[] =
    {
        0x48, 0x8b, 0x01,        /* mov rax,[rcx]       target */
        0x48, 0x8b, 0x51, 0x10,  /* mov rdx,[rcx+0x10]  a1 */
        0x4c, 0x8b, 0x41, 0x18,  /* mov r8,[rcx+0x18]   a2 */
        0x4c, 0x8b, 0x49, 0x20,  /* mov r9,[rcx+0x20]   a3 */
        0x48, 0x8b, 0x49, 0x08,  /* mov rcx,[rcx+0x08]  a0 */
        0xff, 0xe0,              /* jmp rax */
    };
    static void *thunk;
    struct
    {
        void      *fn;    /* 0x00 */
        ULONG_PTR  a[4];  /* 0x08 0x10 0x18 0x20 */
    } params = { fn, { a0, a1, a2, a3 } };

    if (!thunk)
    {
        void *mem = NULL;
        SIZE_T size = sizeof(thunk_code);
        NTSTATUS status = NtAllocateVirtualMemory( GetCurrentProcess(), &mem, 0, &size,
                                                   MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE );
        if (status)
        {
            ERR( "no memory for the guest argument thunk, status %08x\n", (UINT)status );
            return 0;
        }
        memcpy( mem, thunk_code, sizeof(thunk_code) );
        if (InterlockedCompareExchangePointer( &thunk, mem, NULL ))
        {
            SIZE_T free_size = 0;
            NtFreeVirtualMemory( GetCurrentProcess(), &mem, &free_size, MEM_RELEASE );
        }
    }
    return call_guest_function( thunk, &params );
}


/***********************************************************************
 *           call_guest_tls_callback
 *
 * A PIMAGE_TLS_CALLBACK of a guest image, at process and thread attach and
 * detach: (module, reason, reserved), MS-x64, through the generic argument
 * thunk.  The same native->guest direction as the atexit handlers.
 */
/***********************************************************************
 *           __wine_guest__initterm / __wine_guest__initterm_e   (NTDLL.@)
 *
 * What a guest image's import of the C runtime's _initterm resolves to
 * (GUEST-IMPL in msvcrt.thunks / ucrtbase.thunks, forwarded here from those
 * modules' .spec).  The table is the CALLER's, so for a guest caller every
 * entry is x86-64 code while the walker is ppc64 -- msvcrt's own _initterm
 * would bctrl straight into it, which is the c000001d a game's SDL2.dll died
 * on during PROCESS_ATTACH.
 *
 * These live in ntdll rather than in msvcrt/data.c for two reasons: the guest
 * machinery is here, and data.c is shared by every CRT variant in the tree
 * (crtdll, msvcrtd, msvcr70/71/80, ...), none of which should grow a
 * dependency on it.
 *
 * A thunk's callback mask cannot describe this: _initterm takes a pointer to
 * an ARRAY of function pointers, not a function pointer, so each entry is
 * wrapped as it is reached.  wrap_guest_callback() returns NULL and non-guest
 * pointers unchanged, so a mixed or native table stays correct and a native
 * caller -- which never goes through this redirect anyway -- is unaffected.
 */
static void *wrap_guest_callback( void *fn );

void CDECL __wine_guest__initterm( void (**start)(void), void (**end)(void) )
{
    void (**cur)(void);

    TRACE( "(%p,%p)\n", start, end );
    for (cur = start; cur < end; cur++)
    {
        void (*fn)(void);

        if (!*cur) continue;
        fn = wrap_guest_callback( *cur );
        TRACE( "calling %p (guest %p)\n", fn, *cur );
        fn();
    }
}

int CDECL __wine_guest__initterm_e( int (**start)(void), int (**end)(void) )
{
    int (**cur)(void);
    int res = 0;

    TRACE( "(%p,%p)\n", start, end );
    for (cur = start; !res && cur < end; cur++)
    {
        int (*fn)(void);

        if (!*cur) continue;
        fn = wrap_guest_callback( *cur );
        res = fn();
        if (res) TRACE( "function %p failed: %#x\n", *cur, res );
    }
    return res;
}


void call_guest_tls_callback( void *callback, void *module, UINT reason )
{
    TRACE( "callback %p module %p reason %u\n", callback, module, reason );
    call_guest_function_args( callback, (ULONG_PTR)module, reason, 0, 0 );
}


/***********************************************************************
 *           call_guest_dll_entry_point
 *
 * A guest image's DllMain: (module, reason, reserved), MS-x64, through the
 * same generic argument thunk as the TLS callbacks.  It is separate from
 * call_guest_tls_callback only because DllMain RETURNS a value and a
 * PIMAGE_TLS_CALLBACK does not -- a DllMain that returns FALSE has to become
 * STATUS_DLL_INIT_FAILED, so the BOOL cannot be dropped.
 *
 * Until an application's own DLLs became loadable this had no callers: every
 * guest module in the process was one of our own thunk builtins, whose entry
 * point is a trap stub rather than guest code.  The first real one -- a game's
 * XAudio2_9Redist.dll -- went through call_dll_entry_point instead and died
 * c000001d, x86-64 bytes executed as ppc64 off a plain bctrl.
 */
BOOL call_guest_dll_entry_point( void *entry, void *module, UINT reason, void *reserved )
{
    TRACE( "entry %p module %p reason %u reserved %p\n", entry, module, reason, reserved );
    return (BOOL)(DWORD)call_guest_function_args( entry, (ULONG_PTR)module, reason,
                                                  (ULONG_PTR)reserved, 0 );
}


/***********************************************************************
 *           guest SEH dispatch (docs/guest-seh.md)
 *
 * Two register files exist per guest thread and never merge: a guest
 * exception is dispatched against the guest's AMD64_CONTEXT, a native one
 * against the native CONTEXT, and the two machines cross only as an
 * NTSTATUS.  The EXCEPTION_RECORD is the one object that crosses by value:
 * its layout is identical between MS-x64 and LP64 ELFv2, which these
 * asserts pin rather than trust.
 */
C_ASSERT( sizeof(EXCEPTION_RECORD) == 152 );
C_ASSERT( offsetof(EXCEPTION_RECORD, ExceptionRecord) == 8 );
C_ASSERT( offsetof(EXCEPTION_RECORD, ExceptionAddress) == 16 );
C_ASSERT( offsetof(EXCEPTION_RECORD, NumberParameters) == 24 );
C_ASSERT( offsetof(EXCEPTION_RECORD, ExceptionInformation) == 32 );

/* what a guest handler compiled against winnt.h expects EXCEPTION_POINTERS
 * to be: two pointers, the CONTEXT one being its own machine's */
struct guest_exception_pointers
{
    EXCEPTION_RECORD *ExceptionRecord;
    AMD64_CONTEXT    *ContextRecord;
};
C_ASSERT( sizeof(struct guest_exception_pointers) == 16 );

/* A guest exception went unhandled at guest level; the record waits here for
 * the run's PE caller (RtlUserThreadStart divert / call_guest_function) to
 * re-raise it NATIVELY, so the existing machinery -- vectored handlers, the
 * RtlUserThreadStart __EXCEPT, the unhandled-exception filter (behind which
 * a guest filter sits as a trampoline already), NtTerminateProcess -- turns
 * it into a correctly-coded, reported death instead of "emulator bridge
 * failed (1)".  Per thread, consumed exactly once. */
static __thread BOOL guest_exc_pending;
static __thread EXCEPTION_RECORD guest_exc_rec;

/* the trap CONTEXT the innermost emu_trap_dispatch on this thread is
 * serving: what a raise-style override (emu_RaiseException) dispatches
 * against.  Saved/restored around each dispatch, so nesting works. */
static __thread AMD64_CONTEXT *emu_current_trap_ctx;

/* Guest vectored handlers, recorded at REGISTRATION through the thunk
 * overrides below -- the atexit pattern: a pointer handed through a guest
 * thunk is guest-callable by construction and needs no classification at
 * call time.  Native vectored handlers are deliberately NOT offered guest
 * exceptions in v1 (they would read an AMD64 CONTEXT through a native
 * CONTEXT pointer); TRACE'd when that decision is taken.  Order matters:
 * FIRST handlers prepend, others append, as RtlAddVectoredExceptionHandler
 * defines. */
/* Grown on demand, for the reason the atexit table is: a table sized for the
 * probes that first exercised it silently stops registering once a real
 * program goes past it, and a vectored handler that was never registered is
 * invisible until the exception it was meant to see arrives.  Nothing here
 * hands the array's address out, so unlike the callback trampolines this one
 * can simply move. */
static void **guest_veh;
static UINT   guest_veh_count;
static UINT   guest_veh_capacity;

static BOOL guest_veh_reserve(void)
{
    UINT want;
    void **grown;

    if (guest_veh_count < guest_veh_capacity) return TRUE;
    want = guest_veh_capacity ? guest_veh_capacity * 2 : 32;
    grown = RtlReAllocateHeap( GetProcessHeap(), HEAP_ZERO_MEMORY,
                               guest_veh, want * sizeof(*grown) );
    if (!grown && !(grown = RtlAllocateHeap( GetProcessHeap(), HEAP_ZERO_MEMORY,
                                             want * sizeof(*grown) )))
        return FALSE;
    guest_veh = grown;
    guest_veh_capacity = want;
    return TRUE;
}

static BOOL is_valid_guest_frame( ULONG_PTR frame, void *stack_base, void *stack_limit )
{
    if (frame & (sizeof(void *) - 1)) return FALSE;
    if ((void *)frame >= NtCurrentTeb()->Tib.StackLimit &&
        (void *)frame <= NtCurrentTeb()->Tib.StackBase) return TRUE;
    return stack_limit && (void *)frame > stack_limit && (void *)frame <= stack_base;
}

/***********************************************************************
 *           dispatch_guest_exception
 *
 * Guest-level dispatch: guest vectored handlers, then the shared TEB chain
 * -- the guest pushes its __TRY frames through gs:[0x30] onto the very list
 * native dispatch walks.  Every handler is invoked as guest code through
 * the nested-run primitive; the first native frame stops the walk, because
 * native frames get their shot from the native dispatch after the run ends,
 * with their own machine's context.
 *
 * STATUS_SUCCESS means a handler said continue-execution and the (possibly
 * edited) guest CONTEXT should be resumed; anything else is the unhandled
 * protocol.
 */
static NTSTATUS dispatch_guest_exception( EXCEPTION_RECORD *rec, AMD64_CONTEXT *ctx,
                                          void *stack_base, void *stack_limit )
{
    EXCEPTION_REGISTRATION_RECORD *frame;
    UINT i;

    TRACE( "code=%x flags=%x addr=%p rip=%p rsp=%p params=%u\n",
           (UINT)rec->ExceptionCode, (UINT)rec->ExceptionFlags, rec->ExceptionAddress,
           (void *)(ULONG_PTR)ctx->Rip, (void *)(ULONG_PTR)ctx->Rsp, (UINT)rec->NumberParameters );
    for (i = 0; i < rec->NumberParameters; i++)
        TRACE( " info[%u]=%p\n", i, (void *)rec->ExceptionInformation[i] );

    for (i = 0; i < guest_veh_count; i++)
    {
        struct guest_exception_pointers ptrs = { rec, ctx };
        void *handler = guest_veh[i];
        LONG res;

        TRACE( "calling guest vectored handler %p\n", handler );
        res = (LONG)(DWORD)call_guest_function_args( handler, (ULONG_PTR)&ptrs, 0, 0, 0 );
        TRACE( "guest vectored handler %p returned %x\n", handler, (UINT)res );
        if (res == EXCEPTION_CONTINUE_EXECUTION) return STATUS_SUCCESS;
    }

    for (frame = NtCurrentTeb()->Tib.ExceptionList;
         frame != (EXCEPTION_REGISTRATION_RECORD *)~(ULONG_PTR)0;
         frame = frame->Prev)
    {
        DWORD res;

        if (!is_valid_guest_frame( (ULONG_PTR)frame, stack_base, stack_limit ))
        {
            ERR( "invalid frame %p (win32 %p-%p, guest %p-%p)\n", frame,
                 NtCurrentTeb()->Tib.StackLimit, NtCurrentTeb()->Tib.StackBase,
                 stack_limit, stack_base );
            rec->ExceptionFlags |= EXCEPTION_STACK_INVALID;
            break;
        }
        if (!guest_module_from_address( frame->Handler ))
        {
            void *image;
            if (RtlPcToFileHeader( frame->Handler, &image ))
            {
                /* first native frame: stop the guest-level walk */
                TRACE( "frame %p handler %p is native code, stopping the guest walk\n",
                       frame, frame->Handler );
                break;
            }
            /* the thread-start rule, verbatim: refuse loudly, never guess */
            ERR( "frame %p handler %p is in no image at all, refusing\n", frame, frame->Handler );
            break;
        }
        TRACE( "calling guest TEB handler %p (rec=%p frame=%p ctx=%p)\n",
               frame->Handler, rec, frame, ctx );
        res = (DWORD)call_guest_function_args( (void *)frame->Handler, (ULONG_PTR)rec,
                                               (ULONG_PTR)frame, (ULONG_PTR)ctx, 0 );
        TRACE( "guest TEB handler %p returned %u\n", frame->Handler, res );
        switch (res)
        {
        case ExceptionContinueExecution:
            if (rec->ExceptionFlags & EXCEPTION_NONCONTINUABLE) return STATUS_NONCONTINUABLE_EXCEPTION;
            return STATUS_SUCCESS;
        case ExceptionContinueSearch:
            break;
        default:
            /* a guest handler that CATCHES never returns -- it unwinds via
             * RtlUnwind (tripwired below, S4) -- so any other disposition
             * here is a protocol we do not speak yet */
            ERR( "guest handler %p returned disposition %u; refusing\n", frame->Handler, (UINT)res );
            return STATUS_INVALID_DISPOSITION;
        }
    }
    return STATUS_UNHANDLED_EXCEPTION;
}

/***********************************************************************
 *           emu_exception_dispatch
 *
 * PE-side entry for a guest fault, entered from the unix run loop through
 * call_emu_trap_dispatcher exactly like emu_trap_dispatch: same callback
 * stack layout, same syscall-frame push, same NtCallbackReturn return, same
 * FDE story.  args holds one pointer to a struct emu_exception_params.
 */
void WINAPI emu_exception_dispatch( ULONG id, void *args, ULONG len )
{
    struct emu_exception_params *exc = *(struct emu_exception_params **)args;
    NTSTATUS status = dispatch_guest_exception( exc->rec, exc->ctx,
                                                exc->stack_base, exc->stack_limit );

    if (status != STATUS_SUCCESS)
    {
        guest_exc_rec = *exc->rec;
        guest_exc_pending = TRUE;
        status = STATUS_EMU_GUEST_EXCEPTION;
    }
    status = NtCallbackReturn( NULL, 0, status );
    RtlRaiseStatus( status );
}

/***********************************************************************
 *           raise_pending_guest_exception
 *
 * The unhandled half of the protocol, at the run's PE caller: pick up the
 * record a guest-level dispatch could not consume and re-raise it natively.
 * Does not return when a record was pending.
 */
static void raise_pending_guest_exception(void)
{
    EXCEPTION_RECORD rec;

    if (!guest_exc_pending) return;
    rec = guest_exc_rec;
    guest_exc_pending = FALSE;
    ERR( "guest exception %08x at %p unhandled at guest level; re-raising natively\n",
         (UINT)rec.ExceptionCode, rec.ExceptionAddress );
    RtlRaiseException( &rec );
}

/* registration-side interception, the atexit pattern (D3r) */
static ULONG_PTR emu_AddVectoredExceptionHandler( const ULONG_PTR *a, void *native )
{
    BOOL first = a[0] != 0;
    void *handler = (void *)a[1];
    ULONG_PTR magic;
    void *ret = NULL;

    LdrLockLoaderLock( 0, NULL, &magic );
    if (!handler) { /* nothing */ }
    else if (!guest_veh_reserve())
        ERR( "cannot grow the guest vectored handler table past %u; %p not "
             "registered\n", guest_veh_capacity, handler );
    else
    {
        if (first)
        {
            memmove( guest_veh + 1, guest_veh, guest_veh_count * sizeof(*guest_veh) );
            guest_veh[0] = handler;
        }
        else guest_veh[guest_veh_count] = handler;
        guest_veh_count++;
        ret = handler;   /* the pseudo-handle: the guest pointer itself */
        TRACE( "guest vectored handler %p registered (%s, %u total)\n",
               handler, first ? "first" : "last", guest_veh_count );
    }
    LdrUnlockLoaderLock( 0, magic );
    return (ULONG_PTR)ret;
}

static ULONG_PTR emu_RemoveVectoredExceptionHandler( const ULONG_PTR *a, void *native )
{
    void *handler = (void *)a[0];
    ULONG_PTR magic, ret = 0;
    UINT i;

    LdrLockLoaderLock( 0, NULL, &magic );
    for (i = 0; i < guest_veh_count; i++)
    {
        if (guest_veh[i] != handler) continue;
        memmove( guest_veh + i, guest_veh + i + 1,
                 (guest_veh_count - i - 1) * sizeof(*guest_veh) );
        guest_veh_count--;
        ret = 1;
        TRACE( "guest vectored handler %p removed (%u left)\n", handler, guest_veh_count );
        break;
    }
    LdrUnlockLoaderLock( 0, magic );
    return ret;
}

/* Guest RaiseException: the state to dispatch against and the handlers to
 * run are the guest's, so this replaces the old pass-through to the native
 * implementation -- whose native unwind off the switched stack terminated
 * measurably (a_unwind) but could never find a guest handler.  The same
 * prompt-termination observable is preserved by the unhandled protocol. */
static NTSTATUS dispatch_guest_raise( EXCEPTION_RECORD *rec, AMD64_CONTEXT *ctx )
{
    struct emu_guest_stack_params stack = { 0 };
    NTSTATUS status;

    WINE_UNIX_CALL( unix_emu_guest_stack, &stack );
    status = dispatch_guest_exception( rec, ctx, stack.base, stack.limit );
    if (status == STATUS_SUCCESS) return STATUS_SUCCESS;   /* continue after the call */
    guest_exc_rec = *rec;
    guest_exc_pending = TRUE;   /* emu_trap_dispatch ends the run on this */
    return status;
}

static ULONG_PTR emu_RaiseException( const ULONG_PTR *a, void *native )
{
    AMD64_CONTEXT *ctx = emu_current_trap_ctx;
    const ULONG_PTR *info = (const ULONG_PTR *)a[3];
    EXCEPTION_RECORD rec = { 0 };
    UINT i;

    rec.ExceptionCode  = (DWORD)a[0];
    rec.ExceptionFlags = (DWORD)a[1] & EXCEPTION_NONCONTINUABLE;
    if (info)
    {
        rec.NumberParameters = min( (DWORD)a[2], EXCEPTION_MAXIMUM_PARAMETERS );
        for (i = 0; i < rec.NumberParameters; i++) rec.ExceptionInformation[i] = info[i];
    }
    if (!ctx)
    {
        ERR( "no trap context for guest RaiseException(%08x)\n", (UINT)rec.ExceptionCode );
        return 0;
    }
    /* the address of the raise is the return address its CALL pushed */
    rec.ExceptionAddress = (void *)*(ULONG_PTR *)(ULONG_PTR)ctx->Rsp;
    dispatch_guest_raise( &rec, ctx );
    return 0;
}

static ULONG_PTR emu_RtlRaiseException( const ULONG_PTR *a, void *native )
{
    AMD64_CONTEXT *ctx = emu_current_trap_ctx;
    EXCEPTION_RECORD rec;

    if (!a[0] || !ctx)
    {
        ERR( "no record (%p) or trap context for guest RtlRaiseException\n", (void *)a[0] );
        return 0;
    }
    rec = *(const EXCEPTION_RECORD *)a[0];   /* layout-identical; asserted above */
    rec.ExceptionAddress = (void *)*(ULONG_PTR *)(ULONG_PTR)ctx->Rsp;
    dispatch_guest_raise( &rec, ctx );
    return 0;
}

/* S4 tripwire: a guest handler that CATCHES unwinds through RtlUnwind, whose
 * cross-machine per-level protocol does not exist yet.  Letting the native
 * implementation run against a guest target frame would orphan the nested
 * run's native frames -- a slow corruption that presents much later -- so
 * this fails loudly and immediately instead. */
static ULONG_PTR emu_RtlUnwind( const ULONG_PTR *a, void *native )
{
    ERR( "guest RtlUnwind(frame=%p, target=%p): cross-machine unwind (guest-seh S4) "
         "is not built; terminating\n", (void *)a[0], (void *)a[1] );
    RtlRaiseStatus( STATUS_NOT_IMPLEMENTED );
    return 0;   /* unreachable */
}


/***********************************************************************
 *           guest atexit handlers
 *
 * A guest registering an atexit handler hands native code a guest function
 * pointer, and native exit() then calls it -- executing x86-64 bytes as ppc64.
 * That is the whole native->guest direction in one API, and it is exactly what
 * made winepath print its answer correctly and then die in do_global_dtors.
 *
 * The interception is at REGISTRATION, not at the call.  A native caller
 * holding a function pointer cannot be taught to classify it, but the thunk
 * that receives the pointer knows precisely what it is.  Address-range
 * classification exists (guest_module_from_address) and is used as a check,
 * not as the mechanism, because it cannot see guest code that never lived in
 * a loaded image.
 *
 * atexit handlers carry no identity -- every one is void(*)(void) -- so one
 * native handler walking a list serves them all and no per-callback
 * trampoline has to be generated.  An API whose callbacks must be told apart
 * (a window procedure, a comparator) needs a trampoline pool instead; that is
 * the generalisation this case happens not to need.
 */
/* Grown on demand rather than fixed.  64 was enough for a probe and is not
 * remotely enough for a real program: a game's C++ static destructors, one
 * _crt_atexit registration each, ran a Quake II build past it during startup
 * and every handler past the 64th was DROPPED -- silently, since a refused
 * registration only returns -1 to a CRT that mostly ignores it. */
static void **guest_atexit_funcs;
static UINT   guest_atexit_count;
static UINT   guest_atexit_capacity;

/* -> FALSE only if the table could not grow. */
static BOOL guest_atexit_reserve(void)
{
    UINT want;
    void **grown;

    if (guest_atexit_count < guest_atexit_capacity) return TRUE;
    want = guest_atexit_capacity ? guest_atexit_capacity * 2 : 64;
    grown = RtlReAllocateHeap( GetProcessHeap(), HEAP_ZERO_MEMORY,
                               guest_atexit_funcs, want * sizeof(*grown) );
    if (!grown && !(grown = RtlAllocateHeap( GetProcessHeap(), HEAP_ZERO_MEMORY,
                                             want * sizeof(*grown) )))
        return FALSE;
    guest_atexit_funcs = grown;
    guest_atexit_capacity = want;
    return TRUE;
}

static void run_guest_atexit_handlers(void)
{
    /* LIFO, as atexit specifies */
    while (guest_atexit_count)
    {
        void *fn = guest_atexit_funcs[--guest_atexit_count];
        TRACE( "running guest atexit handler %p\n", fn );
        call_guest_function( fn, NULL );
    }
}

/***********************************************************************
 *           emu_ExitThread
 *
 * A guest thread calling ExitThread must not reach native ExitThread from
 * inside the trap dispatch: that ends in pthread_exit with a live
 * fexbridge_run and FEX JIT frames below it on the kernel stack, and the
 * forced unwind has to cross frames whose CFI quality is unknown.  That is the
 * empty-FDE hazard class, whose failure mode is a spinning core rather than a
 * crash.
 *
 * So unwind by protocol instead: record the request, and return a status that
 * makes emu_trap_dispatch end the run through the mechanism that already
 * exists for it (NtCallbackReturn -> nonzero -> FEXBRIDGE_TRAP_EXIT).  The
 * thread is an ordinary native Wine thread again before any native teardown
 * runs, which is the load-bearing ordering.
 */
static __thread BOOL guest_exit_requested;
static __thread ULONG guest_exit_code;

static ULONG_PTR emu_ExitThread( const ULONG_PTR *a, void *native )
{
    guest_exit_requested = TRUE;
    guest_exit_code = (ULONG)a[0];
    TRACE( "guest requested ExitThread(%u); ending the run\n", guest_exit_code );
    return 0;
}


static ULONG_PTR emu_crt_atexit( const ULONG_PTR *a, void *native )
{
    void *fn = (void *)a[0];
    ULONG_PTR magic;
    BOOL is_guest;

    if (!fn) return 0;

    LdrLockLoaderLock( 0, NULL, &magic );
    is_guest = guest_module_from_address( fn ) != NULL;
    LdrUnlockLoaderLock( 0, magic );

    /* A native pointer here would mean something other than the guest CRT
     * registered it; pass those straight through rather than queueing them. */
    if (!is_guest)
    {
        if (!native) return -1;
        return ((int (*)( void * ))native)( fn );
    }
    if (!guest_atexit_reserve())
    {
        ERR( "cannot grow the guest atexit table past %u\n", guest_atexit_capacity );
        return -1;
    }
    /* Register the single native handler on the first guest registration, so
     * native exit() reaches the guest ones through the normal path.  They all
     * run at this point in the native order rather than interleaved with
     * native handlers -- correct for a guest whose handlers are its own. */
    if (!guest_atexit_count)
    {
        if (!native)
        {
            ERR( "no native _crt_atexit to hang guest handlers off\n" );
            return -1;
        }
        ((int (*)( void (*)(void) ))native)( run_guest_atexit_handlers );
    }
    guest_atexit_funcs[guest_atexit_count++] = fn;
    TRACE( "queued guest atexit handler %p (%u total)\n", fn, guest_atexit_count );
    return 0;
}

/* msvcrt's older registration API, the same native->guest boundary as
 * _crt_atexit -- apphelp_test printed its full 15-test summary and THEN
 * segfaulted in native exit() calling the guest handler _onexit had
 * registered.  Result convention differs: _onexit returns the function on
 * success and NULL on failure, so this cannot share emu_crt_atexit's body. */
static ULONG_PTR emu_onexit( const ULONG_PTR *a, void *native )
{
    void *fn = (void *)a[0];
    ULONG_PTR magic;
    BOOL is_guest;

    if (!fn) return 0;

    LdrLockLoaderLock( 0, NULL, &magic );
    is_guest = guest_module_from_address( fn ) != NULL;
    LdrUnlockLoaderLock( 0, magic );

    if (!is_guest)
    {
        if (!native) return 0;
        return ((ULONG_PTR (*)( void * ))native)( fn );
    }
    if (!guest_atexit_reserve())
    {
        ERR( "cannot grow the guest atexit table past %u\n", guest_atexit_capacity );
        return 0;
    }
    if (!guest_atexit_count)
    {
        if (!native)
        {
            ERR( "no native _onexit to hang guest handlers off\n" );
            return 0;
        }
        if (!((ULONG_PTR (*)( void (*)(void) ))native)( run_guest_atexit_handlers )) return 0;
    }
    guest_atexit_funcs[guest_atexit_count++] = fn;
    TRACE( "queued guest onexit handler %p (%u total)\n", fn, guest_atexit_count );
    return a[0];
}


/***********************************************************************
 *           guest callback trampolines
 *
 * The generalisation the atexit handlers dodged: an API whose callbacks must
 * be told apart -- a progress callback, a comparator, an exception filter --
 * hands native code a guest function pointer that native code then CALLS,
 * with arguments.  The interception is at REGISTRATION, exactly as for
 * atexit: the thunk that receives the pointer knows precisely what it is,
 * where the native caller holding it later cannot be taught to classify it.
 * Registration swaps the guest pointer for a native trampoline; native code
 * calls the trampoline like any function pointer, and the trampoline funnels
 * into the shared run-entry primitive -- which adopts whatever thread the
 * caller happens to be on, so a native worker thread invoking a guest
 * callback needs no thread-specific code here either.
 *
 * Trampolines come allocate_stub()-style from a pool OUTSIDE any AMD64 image
 * range, so the widened thread_start_is_guest_code classifies one as native
 * (guest-threads.md composition rule 2).  One trampoline per distinct guest
 * target, found by lookup, so re-registration is idempotent and a callback
 * invoked a million times costs one slot; slots live for the process, like
 * allocate_stub's.  Each is twelve instructions:
 *
 *      r7  = guest target                (the fifth ELFv2 argument)
 *      r12 = guest_callback_dispatch     (entered by its global entry)
 *      mtctr r12 / bctr
 *
 * r3-r6 pass through untouched, so the dispatcher receives the native
 * caller's first four integer arguments plus the guest target, and four
 * arguments always travel (see call_guest_function_args).
 *
 * The return value is the guest's RAX with the low 32 bits sign-extended:
 * every callback the corpus registers returns int/BOOL/LONG, an x86-64
 * callee writing EAX zeroes the upper half, and a native caller may rely on
 * the ELFv2 rule that the callee extended its 32-bit result -- a comparator
 * returning -1 must not arrive as 0xffffffff.  A callback class returning a
 * genuine 64-bit value (an LRESULT window procedure) needs a per-slot flag
 * here on the day the corpus demands one.
 *
 * WINEEMUNOCBWRAP=1 is the negative control, same shape as
 * WINEEMUNOGSTHREADS: registration hands the raw guest pointer to native
 * code, which is exactly the bug this pool exists to fix, so anything this
 * mechanism carries MUST go red under it.
 */
struct guest_callback_stub
{
    UINT  code[12];      /* r7 = guest_fn; r12 = dispatch; mtctr; bctr */
    void *guest_fn;      /* identity: one stub per target, and post-mortem */
    void *pad;           /* one cache line per slot */
};

/* Trampolines are handed OUT, so a full pool cannot be reallocated: native
 * code and the guest both hold pointers into it.  Chain fresh blocks instead,
 * which leaves every address already issued exactly where it was.
 *
 * A single 1024-entry block was enough while the only callbacks came from
 * probes.  A real program blows through it -- a Quake II build exhausted it
 * during startup -- and the old behaviour on exhaustion was to hand the RAW
 * guest pointer to native code, i.e. to schedule a c0000005 for later rather
 * than fail at the registration that caused it. */
#define GUEST_CB_BLOCK 1024        /* stubs per block, a 64KB allocation */
#define MAX_GUEST_CB_BLOCKS 64     /* 65536 callbacks before we genuinely stop */

static struct guest_callback_stub *guest_cb_block[MAX_GUEST_CB_BLOCKS];
static UINT guest_cb_blocks;       /* blocks allocated */
static UINT guest_cb_count;        /* stubs used in the LAST block */

/* -> the block containing fn, or NULL.  Used for idempotence: a pointer that
 * is already a trampoline must come back unchanged. */
static BOOL guest_cb_owns( const void *fn )
{
    UINT b;

    for (b = 0; b < guest_cb_blocks; b++)
    {
        const struct guest_callback_stub *base = guest_cb_block[b];
        UINT used = (b + 1 == guest_cb_blocks) ? guest_cb_count : GUEST_CB_BLOCK;

        if (fn >= (const void *)base && fn < (const void *)(base + used)) return TRUE;
    }
    return FALSE;
}

static ULONG_PTR guest_callback_dispatch( ULONG_PTR a0, ULONG_PTR a1, ULONG_PTR a2,
                                          ULONG_PTR a3, void *fn )
{
    ULONG_PTR ret;

    TRACE( "calling guest callback %p (%p,%p,%p,%p)\n", fn,
           (void *)a0, (void *)a1, (void *)a2, (void *)a3 );
    ret = call_guest_function_args( fn, a0, a1, a2, a3 );
    /* A guest ExitThread inside the callback ended the nested run: RAX is
     * meaningless, and every enclosing trap return keeps unwinding
     * (guest-threads.md composition rule 3).  Hand back a value that stops
     * the native caller doing more work; 0 is "stop" for every callback kind
     * the corpus registers. */
    if (guest_exit_requested) return 0;
    TRACE( "guest callback %p returned %p\n", fn, (void *)ret );
    return (ULONG_PTR)(LONG_PTR)(LONG)ret;
}

/* emit `reg = val' as the classic five-instruction absolute load */
static UINT *emit_load_imm64( UINT *p, UINT reg, ULONG_PTR val )
{
    *p++ = 0x3C000000 | (reg << 21) | ((val >> 48) & 0xffff);               /* lis  */
    *p++ = 0x60000000 | (reg << 21) | (reg << 16) | ((val >> 32) & 0xffff); /* ori  */
    *p++ = 0x780007C6 | (reg << 21) | (reg << 16);                          /* sldi 32 */
    *p++ = 0x64000000 | (reg << 21) | (reg << 16) | ((val >> 16) & 0xffff); /* oris */
    *p++ = 0x60000000 | (reg << 21) | (reg << 16) | (val & 0xffff);         /* ori  */
    return p;
}

/* a "1" in the process environment; PE-side, so no getenv here */
static BOOL emu_env_flag( const WCHAR *name )
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

static void *wrap_guest_callback( void *fn )
{
    static int nowrap = -1;
    struct guest_callback_stub *stub;
    ULONG_PTR magic;
    void *ret = fn;
    UINT *p, i;

    if (!fn) return fn;

    if (nowrap == -1) nowrap = emu_env_flag( L"WINEEMUNOCBWRAP" );
    if (nowrap)
    {
        ERR( "WINEEMUNOCBWRAP: handing raw guest callback %p to native code\n", fn );
        return fn;
    }

    LdrLockLoaderLock( 0, NULL, &magic );

    /* Idempotence: a pointer that is already one of our trampolines comes
     * back unchanged.  That happens legitimately -- SetUnhandledExceptionFilter
     * returns the previous filter, which may be a trampoline the guest is now
     * restoring -- and wrapping a trampoline would run its ppc64 bytes as
     * x86-64. */
    if (guest_cb_owns( fn )) goto done;

    /* Classification is a CHECK here, never the mechanism: the thunk knows
     * its argument is a callback, which no address range can tell it. */
    if (!guest_module_from_address( fn ))
    {
        void *image;
        if (RtlPcToFileHeader( fn, &image ))
        {
            /* native code is native-callable exactly as it is */
            WARN( "callback %p is native code, not wrapping\n", fn );
            goto done;
        }
        /* in no image at all: guest code generated outside any module -- the
         * registration site knows better than the address does */
        TRACE( "callback %p lies in no image, wrapping as guest code\n", fn );
    }

    /* One stub per distinct target, across every block. */
    {
        UINT b;
        for (b = 0; b < guest_cb_blocks; b++)
        {
            UINT used = (b + 1 == guest_cb_blocks) ? guest_cb_count : GUEST_CB_BLOCK;
            for (i = 0; i < used; i++)
                if (guest_cb_block[b][i].guest_fn == fn) { ret = &guest_cb_block[b][i]; goto done; }
        }
    }

    if (!guest_cb_blocks || guest_cb_count >= GUEST_CB_BLOCK)
    {
        void *mem = NULL;
        SIZE_T size = GUEST_CB_BLOCK * sizeof(struct guest_callback_stub);
        NTSTATUS status;

        if (guest_cb_blocks >= MAX_GUEST_CB_BLOCKS)
        {
            ERR( "more than %u guest callbacks; %p goes to native code raw\n",
                 MAX_GUEST_CB_BLOCKS * GUEST_CB_BLOCK, fn );
            goto done;
        }
        status = NtAllocateVirtualMemory( GetCurrentProcess(), &mem, 0, &size,
                                          MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE );
        if (status)
        {
            ERR( "no memory for guest callback trampolines, status %08x\n", (UINT)status );
            goto done;   /* raw pointer: a diagnosed crash, not a dropped callback */
        }
        guest_cb_block[guest_cb_blocks++] = mem;
        guest_cb_count = 0;
    }

    stub = &guest_cb_block[guest_cb_blocks - 1][guest_cb_count];
    p = stub->code;
    p = emit_load_imm64( p, 7, (ULONG_PTR)fn );
    p = emit_load_imm64( p, 12, (ULONG_PTR)guest_callback_dispatch );
    *p++ = 0x7D8903A6;   /* mtctr r12 */
    *p++ = 0x4E800420;   /* bctr */
    stub->guest_fn = fn;
    NtFlushInstructionCache( GetCurrentProcess(), stub, sizeof(*stub) );
    guest_cb_count++;    /* publish only after the code is flushed */
    TRACE( "guest callback %p -> trampoline %p (%u total)\n", fn, stub, guest_cb_count );
    ret = stub;
done:
    LdrUnlockLoaderLock( 0, magic );
    return ret;
}

/* swap the arguments a thunk_override row declares as callbacks */
static void wrap_thunk_callback_args( ULONG_PTR *a, UINT argc, UINT mask )
{
    UINT i;
    for (i = 0; i < argc; i++)
        if (mask & (1u << i)) a[i] = (ULONG_PTR)wrap_guest_callback( (void *)a[i] );
}


static const struct thunk_override thunk_overrides[] =
{
    { L"kernel32.dll", "GetProcAddress",    2, emu_GetProcAddress },
    { L"kernel32.dll", "GetModuleHandleW",  1, emu_GetModuleHandleW },
    { L"kernel32.dll", "GetModuleHandleA",  1, emu_GetModuleHandleA },
    /* kernelbase exports the same sharp functions, and a modern CRT imports
     * them from THERE: find.exe went GetModuleHandleW -> native handle ->
     * GetProcAddress -> native ppc64 code address -> guest CALLed it.  An
     * override keyed only to kernel32 silently passes those through, so
     * every module that exports one of these needs its own row. */
    { L"kernelbase.dll", "GetProcAddress",   2, emu_GetProcAddress },
    { L"kernelbase.dll", "GetModuleHandleW", 1, emu_GetModuleHandleW },
    { L"kernelbase.dll", "GetModuleHandleA", 1, emu_GetModuleHandleA },
    { L"kernel32.dll",   "LoadLibraryA",     1, emu_LoadLibraryA },
    { L"kernel32.dll",   "LoadLibraryW",     1, emu_LoadLibraryW },
    { L"kernelbase.dll", "LoadLibraryA",     1, emu_LoadLibraryA },
    { L"kernelbase.dll", "LoadLibraryW",     1, emu_LoadLibraryW },
    { L"kernel32.dll",   "LoadLibraryExA",   3, emu_LoadLibraryExA },
    { L"kernel32.dll",   "LoadLibraryExW",   3, emu_LoadLibraryExW },
    { L"kernelbase.dll", "LoadLibraryExA",   3, emu_LoadLibraryExA },
    { L"kernelbase.dll", "LoadLibraryExW",   3, emu_LoadLibraryExW },
    /* native->guest: the pointer is queued here and run by our own native
     * handler at exit; see run_guest_atexit_handlers */
    { L"ucrtbase.dll", "_crt_atexit",       1, emu_crt_atexit },
    { L"msvcrt.dll",   "_crt_atexit",       1, emu_crt_atexit },
    { L"msvcrt.dll",   "atexit",            1, emu_crt_atexit },
    { L"msvcrt.dll",   "_onexit",           1, emu_onexit },
    { L"ucrtbase.dll", "_onexit",           1, emu_onexit },
    { L"kernel32.dll", "ExitThread",        1, emu_ExitThread },
    { L"kernelbase.dll", "ExitThread",      1, emu_ExitThread },
    /* native->guest WITH identity: these thunks receive a guest function
     * pointer that native code later calls with arguments, so registration
     * swaps it for a trampoline from the pool above.  Rows are driven by what
     * the corpus actually registers, not by API taxonomy.  NOTE composition
     * rule 1 (guest-threads.md #3): CreateThread's start routine must NOT
     * appear here -- thread starts are intercepted at invocation, in
     * RtlUserThreadStart. */
    { L"mspatcha.dll", "ApplyPatchToFileExA",          6, NULL, 1u << 4 },
    { L"mspatcha.dll", "ApplyPatchToFileExW",          6, NULL, 1u << 4 },
    { L"mspatcha.dll", "ApplyPatchToFileByHandlesEx",  6, NULL, 1u << 4 },
    { L"mspatcha.dll", "ApplyPatchToFileByBuffers",   11, NULL, 1u << 9 },
    /* every winetest registers a top-level exception filter; SEH dispatch
     * calling a guest filter natively was an illegal-instruction storm ending
     * in a stack overflow, which buried the REAL failure under it */
    { L"kernel32.dll",   "SetUnhandledExceptionFilter", 1, NULL, 1u << 0 },
    { L"kernelbase.dll", "SetUnhandledExceptionFilter", 1, NULL, 1u << 0 },
    /* guest SEH (docs/guest-seh.md S3): vectored handlers are recorded at
     * registration for GUEST-level dispatch -- not wrapped and handed to the
     * native table, where a guest exception would run them twice and a
     * native one would hand them a native CONTEXT */
    { L"ntdll.dll",      "RtlAddVectoredExceptionHandler",    2, emu_AddVectoredExceptionHandler },
    { L"ntdll.dll",      "AddVectoredExceptionHandler",       2, emu_AddVectoredExceptionHandler },
    { L"kernel32.dll",   "AddVectoredExceptionHandler",       2, emu_AddVectoredExceptionHandler },
    { L"kernelbase.dll", "AddVectoredExceptionHandler",       2, emu_AddVectoredExceptionHandler },
    { L"ntdll.dll",      "RtlRemoveVectoredExceptionHandler", 1, emu_RemoveVectoredExceptionHandler },
    { L"ntdll.dll",      "RemoveVectoredExceptionHandler",    1, emu_RemoveVectoredExceptionHandler },
    { L"kernel32.dll",   "RemoveVectoredExceptionHandler",    1, emu_RemoveVectoredExceptionHandler },
    { L"kernelbase.dll", "RemoveVectoredExceptionHandler",    1, emu_RemoveVectoredExceptionHandler },
    /* a guest raise is dispatched against the guest state (S3/5.3); the old
     * pass-through unwound native frames still live under the emulator and
     * could never find a guest handler */
    { L"kernel32.dll",   "RaiseException",    4, emu_RaiseException },
    { L"kernelbase.dll", "RaiseException",    4, emu_RaiseException },
    { L"ntdll.dll",      "RtlRaiseException", 1, emu_RtlRaiseException },
    /* S4 tripwire: a guest catch unwinds through here; fail loudly, never leak */
    { L"ntdll.dll",      "RtlUnwind",         4, emu_RtlUnwind },
    { L"ntdll.dll",      "RtlUnwindEx",       6, emu_RtlUnwind },
    /* comparators: the signed-return case -- a cmp returning -1 must not
     * reach native qsort as 0xffffffff */
    { L"msvcrt.dll",   "qsort",   4, NULL, 1u << 3 },
    { L"ucrtbase.dll", "qsort",   4, NULL, 1u << 3 },
    { L"msvcrt.dll",   "bsearch", 5, NULL, 1u << 4 },
    { L"ucrtbase.dll", "bsearch", 5, NULL, 1u << 4 },
};

/***********************************************************************
 *           guest COM vtable thunks
 *
 * A COM-mode thunk module (tools/spec2thunk COM mode; first client d3d12)
 * publishes a second table alongside __wine_thunk_info: per interface TYPE,
 * one contiguous array of the same 5-byte trap stubs, one per vtable slot.
 * A trapping RIP inside one of those arrays maps to (iface, slot) by the
 * same exact-hit arithmetic the flat stubs use.
 *
 * This dispatcher stays deliberately ignorant of everything COM: argument
 * classes, proxies, sret, floats and return placement all belong to the
 * native namesake module, reached through its single __wine_com_dispatch
 * export.  CONTRACT: __wine_com_dispatch( iface, slot, AMD64_CONTEXT * )
 * returns STATUS_SUCCESS once it has fully served the call -- including
 * writing ctx->Rax -- and THIS side then pops the guest return address.
 */
struct com_thunk_info
{
    UINT version;         /* 1 */
    UINT iface_count;
    UINT stride;          /* 16, same stub body as the flat thunks */
    UINT trap_off;        /* 3 */
    UINT ifaces_rva;      /* iface_count x struct com_thunk_iface */
};
#define COM_THUNK_INFO_VERSION 1

struct com_thunk_iface
{
    GUID iid;
    UINT slot_count;
    UINT stubs_rva;
};

typedef NTSTATUS (WINAPI *com_dispatch_func)( UINT iface, UINT slot, AMD64_CONTEXT *ctx );

struct com_thunk_hit
{
    com_dispatch_func dispatch;
    UINT iface;
    UINT slot;
};

/* Resolve a trapping RIP inside `mod` to a COM vtable slot, or FALSE.
 * Caller holds the loader lock (the resolve below may LdrLoadDll). */
static BOOL find_guest_com_target( LDR_DATA_TABLE_ENTRY *mod, ULONG_PTR rip,
                                   struct com_thunk_hit *hit )
{
    /* the native __wine_com_dispatch, cached per guest module base */
    static struct { void *base; com_dispatch_func fn; } cache[8];

    ULONG_PTR base = (ULONG_PTR)mod->DllBase;
    const struct com_thunk_info *info;
    const struct com_thunk_iface *ifaces;
    ANSI_STRING name;
    HMODULE native;
    void *proc;
    UINT i, n;

    RtlInitAnsiString( &name, "__wine_com_thunk_info" );
    if (LdrGetProcedureAddress( mod->DllBase, &name, 0, (void **)&info ))
        return FALSE;                        /* not a COM-mode module */
    if (info->version != COM_THUNK_INFO_VERSION || !info->stride)
    {
        ERR( "%s has com thunk info version %u, expected %u\n",
             debugstr_w(mod->BaseDllName.Buffer), info->version, COM_THUNK_INFO_VERSION );
        return FALSE;
    }
    ifaces = (const struct com_thunk_iface *)(base + info->ifaces_rva);
    for (i = 0; i < info->iface_count; i++)
    {
        ULONG_PTR start = base + ifaces[i].stubs_rva + info->trap_off;
        UINT slot;

        if (rip < start) continue;
        slot = (rip - start) / info->stride;
        if (slot >= ifaces[i].slot_count) continue;
        if (start + (ULONG_PTR)slot * info->stride != rip)
        {
            /* inside this interface's stub array but not on a published trap
             * site; the arrays do not overlap, so nothing else can claim it */
            ERR( "%s: trap at %p is inside iface %u's stubs but off-site\n",
                 debugstr_w(mod->BaseDllName.Buffer), (void *)rip, i );
            return FALSE;
        }

        for (proc = NULL, n = 0; n < ARRAY_SIZE(cache); n++)
            if (cache[n].base == mod->DllBase) { proc = (void *)cache[n].fn; break; }
        if (!proc)
        {
            if (LdrGetDllHandle( NULL, 0, &mod->BaseDllName, &native ) &&
                LdrLoadDll( NULL, 0, &mod->BaseDllName, &native ))
            {
                ERR( "no native %s to serve COM slots\n", debugstr_w(mod->BaseDllName.Buffer) );
                return FALSE;
            }
            RtlInitAnsiString( &name, "__wine_com_dispatch" );
            if (LdrGetProcedureAddress( native, &name, 0, &proc ))
            {
                ERR( "native %s exports no __wine_com_dispatch\n",
                     debugstr_w(mod->BaseDllName.Buffer) );
                return FALSE;
            }
            for (n = 0; n < ARRAY_SIZE(cache); n++)
                if (!cache[n].base)
                {
                    cache[n].base = mod->DllBase;
                    cache[n].fn = (com_dispatch_func)proc;
                    break;
                }
        }
        hit->dispatch = (com_dispatch_func)proc;
        hit->iface = i;
        hit->slot = slot;
        TRACE( "%s com trap -> iface %u slot %u\n",
               debugstr_w(mod->BaseDllName.Buffer), i, slot );
        return TRUE;
    }
    return FALSE;
}

/***********************************************************************
 *           find_guest_thunk_target
 *
 * Resolve a trapping guest address to the native function it stands for, or
 * to the override that stands in for it.  A hit in a COM stub array instead
 * fills *com and returns NULL.
 */
static void *find_guest_thunk_target( ULONG_PTR rip, UINT *sig_out, thunk_override_func *override,
                                      struct com_thunk_hit *com, UINT *cb_mask,
                                      UINT *fp )
{
    LIST_ENTRY *mark, *entry;
    void *ret = NULL;
    ULONG_PTR magic;

    *fp = 0;

    /* The loader lock, not a private one: this walk calls LdrLoadDll below when
     * a guest module's native counterpart is not loaded yet, and that takes the
     * loader lock itself.  Anything else here deadlocks.  Without it, a trap
     * resolving while another guest thread is inside LoadLibrary reads a list
     * that is being spliced. */
    LdrLockLoaderLock( 0, NULL, &magic );

    mark = &NtCurrentTeb()->Peb->LdrData->InMemoryOrderModuleList;
    for (entry = mark->Flink; entry != mark; entry = entry->Flink)
    {
        LDR_DATA_TABLE_ENTRY *mod = CONTAINING_RECORD( entry, LDR_DATA_TABLE_ENTRY, InMemoryOrderLinks );
        const IMAGE_NT_HEADERS *nt = RtlImageNtHeader( mod->DllBase );
        ULONG_PTR base = (ULONG_PTR)mod->DllBase;
        ANSI_STRING func_name;
        struct thunk_info *info;
        const UINT *names, *sigs, *impl_names;
        HMODULE native;
        void *proc;
        UINT idx, sig, i;

        if (!nt || nt->FileHeader.Machine != IMAGE_FILE_MACHINE_AMD64) continue;
        if (rip < base || rip >= base + nt->OptionalHeader.SizeOfImage) continue;

        RtlInitAnsiString( &func_name, "__wine_thunk_info" );
        if (LdrGetProcedureAddress( mod->DllBase, &func_name, 0, (void **)&info ))
        {
            ERR( "%s is a guest module but exports no __wine_thunk_info\n",
                 debugstr_w(mod->BaseDllName.Buffer) );
            goto done;
        }
        if (info->version != THUNK_INFO_VERSION || !info->stride)
        {
            ERR( "%s has thunk info version %u, expected %u\n",
                 debugstr_w(mod->BaseDllName.Buffer), info->version, THUNK_INFO_VERSION );
            goto done;
        }

        /* The stub rescues the guest's first argument before trapping, so the
         * trap is at a fixed offset inside the stub rather than at its start;
         * the module publishes that offset.  Requiring an exact hit keeps a
         * stray trap from anywhere else in the image out of the dispatch.
         * Anything that is not a flat stub site may still be a COM vtable
         * stub site of the same module; see find_guest_com_target. */
        if (rip < base + info->stubs_rva + info->trap_off) goto try_com;
        idx = (rip - (base + info->stubs_rva + info->trap_off)) / info->stride;
        if (idx >= info->count) goto try_com;
        if (base + info->stubs_rva + info->trap_off + (ULONG_PTR)idx * info->stride != rip) goto try_com;

        names = (const UINT *)(base + info->names_rva);
        sigs  = (const UINT *)(base + info->sigs_rva);
        impl_names = (const UINT *)(base + info->impl_names_rva);
        *fp = ((const UINT *)(base + info->fp_rva))[idx];
        sig = sigs[idx];
        /* a variadic needs one slot beyond its fixed arguments for the va_list */
        if ((sig & THUNK_SIG_RESERVED) ||
            THUNK_SIG_ARGC(sig) + ((sig & THUNK_SIG_VARIADIC) ? 1u : 0u) > THUNK_MAX_ARGS)
        {
            ERR( "%s stub %u has unusable signature %08x\n",
                 debugstr_w(mod->BaseDllName.Buffer), idx, sig );
            goto done;
        }

        *sig_out = sig;

        /* The native namespace: same base name, resolved for our own machine.
         * Nothing native necessarily references it -- a guest process may be
         * the only reason the module is wanted at all -- so load it if it is
         * not already present rather than treating that as a failure. */
        if (LdrGetDllHandle( NULL, 0, &mod->BaseDllName, &native ) &&
            LdrLoadDll( NULL, 0, &mod->BaseDllName, &native ))
        {
            WARN( "no native %s; only an override can serve this\n",
                  debugstr_w(mod->BaseDllName.Buffer) );
            native = NULL;
        }
        RtlInitAnsiString( &func_name, (char *)(base + impl_names[idx]) );
        if (LdrGetProcedureAddress( native, &func_name, 0, &proc ))
        {
            WARN( "native %s has no %s; only an override can serve it\n",
                  debugstr_w(mod->BaseDllName.Buffer), func_name.Buffer );
            proc = NULL;
        }

        TRACE( "%s.%s -> %s %p (%u%s args)\n", debugstr_w(mod->BaseDllName.Buffer),
               (char *)(base + names[idx]), (char *)(base + impl_names[idx]), proc,
               THUNK_SIG_ARGC(sig), (sig & THUNK_SIG_VARIADIC) ? "+va" : "" );
        /* An export whose answer only means something in one machine's
         * namespace is answered here rather than forwarded; see above. */
        for (i = 0; i < ARRAY_SIZE(thunk_overrides); i++)
        {
            if (strcmp( (char *)(base + names[idx]), thunk_overrides[i].name )) continue;
            if (wcsicmp( mod->BaseDllName.Buffer, thunk_overrides[i].module )) continue;
            if (THUNK_SIG_ARGC(sig) != thunk_overrides[i].argc)
            {
                ERR( "%s.%s override expects %u args, thunk says %u\n",
                     debugstr_w(mod->BaseDllName.Buffer), thunk_overrides[i].name,
                     thunk_overrides[i].argc, THUNK_SIG_ARGC(sig) );
                goto done;
            }
            TRACE( "%s.%s -> override %p cb_mask %#x (%u args)\n",
                   debugstr_w(mod->BaseDllName.Buffer), thunk_overrides[i].name,
                   thunk_overrides[i].func, thunk_overrides[i].cb_mask,
                   thunk_overrides[i].argc );
            *override = thunk_overrides[i].func;
            *cb_mask  = thunk_overrides[i].cb_mask;
            break;
        }

        ret = proc;
        goto done;

    try_com:
        if (com) find_guest_com_target( mod, rip, com );
        goto done;
    }
done:
    LdrUnlockLoaderLock( 0, magic );
    return ret;
}

/***********************************************************************
 *           call_native_thunk
 *
 * MS-x64 -> ELFv2.  Extra arguments are harmless on ELFv2 (the callee simply
 * ignores the high registers), so a single widest-form call covers every
 * arity; only the arguments the function actually has are read, since the
 * stack ones past the shadow space would otherwise be uninitialised memory.
 *
 * Argument 0 comes from R10, not RCX.  The trap opcode is x86-64 SYSCALL,
 * which architecturally overwrites RCX with the address of the following
 * instruction (and R11 with the flags) -- and RCX is precisely where MS-x64
 * puts the first argument.  The stub therefore does `mov r10, rcx` before
 * trapping, the same rescue the Linux syscall ABI performs for its own
 * fourth argument.  R10 is not an MS-x64 argument register and SYSCALL does
 * not touch it.  Both RCX and R11 are volatile under MS-x64, so the guest
 * cannot observe the damage after the call returns.
 */
static void marshal_thunk_args( const AMD64_CONTEXT *ctx, UINT argc, UINT narrow, ULONG_PTR *a )
{
    UINT i;

    for (i = 0; i < argc; i++)
    {
        switch (i)
        {
        case 0: a[0] = ctx->R10; break;
        case 1: a[1] = ctx->Rdx; break;
        case 2: a[2] = ctx->R8;  break;
        case 3: a[3] = ctx->R9;  break;
        /* the caller's stack arguments sit past the pushed return address and
         * the 32 bytes of shadow space MS-x64 reserves for the register ones */
        default: a[i] = *(ULONG_PTR *)(ULONG_PTR)(ctx->Rsp + 8 + i * 8); break;
        }
        /* a 32-bit argument's slot carries stack garbage above bit 31, and
         * LP64-built native code reads all 64 bits; see THUNK_SIG_NARROW */
        if (narrow & (1u << i)) a[i] = (UINT)a[i];
    }
}

/***********************************************************************
 *           marshal_thunk_va_list
 *
 * A true ellipsis export cannot be forwarded by a fixed-arity stub, which has
 * no way to know how many arguments there are.  It does not have to be: it is
 * forwarded to the callee own v-variant instead, with a va_list built here.
 * That is possible only because ppc64le ELFv2 va_list is a plain pointer into
 * a flat run of 8-byte argument slots, structurally identical to MS-x64 --
 * measured, not assumed.  (The sibling wine-spec-thunk project refuses va_list
 * outright; it is solving a different ABI pair, so do not copy that rule here.)
 *
 * MS-x64 requires every caller to reserve 32 bytes of shadow space above the
 * return address, and that is exactly where a variadic callee spills its four
 * register arguments to make the argument list contiguous.  Do the same spill
 * here and the shadow slots, plus the caller stack arguments which already sit
 * immediately above them at Rsp+0x28, form one contiguous save area.  The
 * va_list is then the address of the slot after the last fixed argument.
 *
 * Argument 0 comes from R10 rather than RCX for the usual reason: the trap
 * opcode is SYSCALL, which destroys RCX.
 */
static ULONG_PTR *marshal_thunk_va_list( const AMD64_CONTEXT *ctx, UINT nfixed )
{
    ULONG_PTR *home = (ULONG_PTR *)(ULONG_PTR)(ctx->Rsp + 8);

    home[0] = ctx->R10;
    home[1] = ctx->Rdx;
    home[2] = ctx->R8;
    home[3] = ctx->R9;
    return home + nfixed;
}

/***********************************************************************
 *           call_native_thunk_fp
 *
 * The floating-point form of call_native_thunk().  It cannot be written in C
 * for the same reason libffi exists: ELFv2 decides which register an argument
 * travels in from the callee's OWN prototype, and a generic call site does not
 * have one.  Rather than a switch over every (arity x fp-mask x return-kind)
 * shape, load the register files directly and branch -- one routine, every
 * shape.
 *
 * The three ELFv2 rules it implements are MEASURED, not assumed; see the
 * handbook's winebuild-ppc64-relays-stubs.md §2, which established them on this
 * machine with a set of probe functions:
 *
 *   - a floating-point argument goes in the NEXT FREE FPR regardless of its
 *     positional index, and its GPR slot is skipped;
 *   - the FPRs run out at f13 (only f1-f8 are loaded here, which is every
 *     shape this port's oracle will accept);
 *   - argument i owns the doubleword at 32 + 8*i in the caller's parameter
 *     save area whether or not it travels in a register.
 *
 * So the caller hands over two already-separated arrays -- gpr[] in ELFv2 GPR
 * order, fpr[] in FPR order -- and this only has to load them.  The mapping
 * from MS-x64's position-indexed XMM0-3 to ELFv2's order-indexed f1-f8 is
 * marshal_thunk_args_fp()'s job, where it can be read.
 *
 * Loading order is load-bearing: the FPRs come from r5 and the GPRs from r4,
 * both of which are themselves overwritten as argument registers, so f1-f8 are
 * filled before r5 dies and r4 is read last of all.
 *
 * The two __ASM_CFI() directives are required for the reason spelled out above
 * RtlRaiseException: an empty FDE claims CFA = current r1 with the return
 * address still in lr, and glibc's forced unwind spins forever on such a frame.
 * probes/check-empty-fde.sh gates this.
 */
extern ULONG_PTR call_native_thunk_fp( void *proc, const ULONG_PTR *gpr,
                                       const double *fpr, double *fp_ret );
__ASM_GLOBAL_FUNC( call_native_thunk_fp,
                   "addis 2, 12, .TOC.-" __ASM_NAME("call_native_thunk_fp") "@ha\n\t"
                   "addi 2, 2, .TOC.-" __ASM_NAME("call_native_thunk_fp") "@l\n\t"
                   ".localentry " __ASM_NAME("call_native_thunk_fp") ", .-" __ASM_NAME("call_native_thunk_fp") "\n\t"
                   "mflr 0\n\t"
                   "std 0, 16(1)\n\t"
                   __ASM_CFI(".cfi_offset 65, 16\n\t")
                   "stdu 1, -160(1)\n\t"
                   __ASM_CFI(".cfi_def_cfa_offset 160\n\t")
                   /* 32(1)-96(1) is the callee's parameter save area; 96 up is
                    * ours.  Keep our TOC and fp_ret above it. */
                   "std 2, 104(1)\n\t"
                   "std 6, 112(1)\n\t"           /* fp_ret, dead across the call */
                   "mtctr 3\n\t"
                   "mr 12, 3\n\t"                /* ELFv2 global entry wants r12 */
                   "lfd 1, 0(5)\n\t"
                   "lfd 2, 8(5)\n\t"
                   "lfd 3, 16(5)\n\t"
                   "lfd 4, 24(5)\n\t"
                   "lfd 5, 32(5)\n\t"
                   "lfd 6, 40(5)\n\t"
                   "lfd 7, 48(5)\n\t"
                   "lfd 8, 56(5)\n\t"
                   "ld 10, 56(4)\n\t"
                   "ld 9, 48(4)\n\t"
                   "ld 8, 40(4)\n\t"
                   "ld 7, 32(4)\n\t"
                   "ld 6, 24(4)\n\t"
                   "ld 5, 16(4)\n\t"
                   "ld 3, 0(4)\n\t"
                   "ld 4, 8(4)\n\t"              /* last: r4 is the base */
                   "bctrl\n\t"
                   "ld 2, 104(1)\n\t"            /* the callee may clobber r2 */
                   "ld 11, 112(1)\n\t"
                   "stfd 1, 0(11)\n\t"           /* f1 holds any FP return */
                   "addi 1, 1, 160\n\t"
                   "ld 0, 16(1)\n\t"
                   "mtlr 0\n\t"
                   "blr" )


/***********************************************************************
 *           marshal_thunk_args_fp
 *
 * Split the guest's argument list into the two register files ELFv2 wants.
 *
 * The two ABIs index their float registers differently, and that is the whole
 * of the work here:
 *
 *   MS-x64 indexes by POSITION.  Argument i, if it is floating point, is in
 *   XMMi -- and the matching integer register is skipped, so a (double, int)
 *   pair puts the double in XMM0 and the int in RDX, not RCX.
 *
 *   ELFv2 indexes the FPRs by ORDER -- the n'th floating-point argument is in
 *   f(n+1) whatever its position -- but the GPRs still by POSITION.  An FP
 *   argument takes its FPR and SKIPS its GPR, leaving that register unused
 *   rather than closing the integer arguments up behind it (handbook
 *   winebuild-ppc64-relays-stubs.md §2, whose worked example annotates the
 *   hole: "std 5, 120(1)  # arg2 long -> r5  (r4 skipped by arg1)").
 *
 * That asymmetry is the whole trap.  Packing the integers down instead put
 * ldexp(double, int)'s exponent in r3 where the callee reads r4, so it saw 0
 * and returned its input unscaled -- a wrong number, not a crash.  It is what
 * probes/check-fp-marshal.sh exists to catch, and did.
 *
 * So this walks the arguments once, placing each in whichever file it
 * belongs in.  A float (as opposed to double) argument arrives from the guest
 * as 32 bits and is widened here, because ELFv2 passes single-precision
 * arguments in an FPR as the double-precision value.
 *
 * Argument 0 comes from R10 rather than RCX for the reason marshal_thunk_args()
 * gives: the trap opcode is SYSCALL and it destroys RCX.  An FP argument 0 is
 * unaffected -- it was never in RCX to begin with.
 */
static void marshal_thunk_args_fp( const AMD64_CONTEXT *ctx, UINT argc, UINT narrow, UINT fp,
                                   ULONG_PTR *gpr, double *fpr )
{
    const UINT fp_mask = THUNK_FP_MASK( fp ), single = THUNK_FP_SINGLE( fp );
    UINT i, nfpr = 0;

    memset( gpr, 0, THUNK_MAX_ARGS * sizeof(*gpr) );
    memset( fpr, 0, THUNK_MAX_FP_ARGS * sizeof(*fpr) );

    for (i = 0; i < argc; i++)
    {
        if (fp_mask & (1u << i))
        {
            /* XMMi, by position on the guest side.  The guest's XMM registers
             * are in the context the bridge published; only the low 8 bytes
             * carry the value.  gpr[i] stays zero: this argument's GPR slot is
             * skipped, not filled by the next integer. */
            const void *xmm = &ctx->FltSave.XmmRegisters[i];

            if (nfpr < THUNK_MAX_FP_ARGS)
                fpr[nfpr++] = (single & (1u << i)) ? (double)*(const float *)xmm
                                                   : *(const double *)xmm;
            continue;
        }
        if (i >= THUNK_MAX_ARGS) continue;
        switch (i)
        {
        case 0: gpr[i] = ctx->R10; break;
        case 1: gpr[i] = ctx->Rdx; break;
        case 2: gpr[i] = ctx->R8;  break;
        case 3: gpr[i] = ctx->R9;  break;
        default: gpr[i] = *(ULONG_PTR *)(ULONG_PTR)(ctx->Rsp + 8 + i * 8); break;
        }
        if (narrow & (1u << i)) gpr[i] = (UINT)gpr[i];
    }
}


static ULONG_PTR call_native_thunk( void *proc, const ULONG_PTR *a )
{
    return ((ULONG_PTR (*)( ULONG_PTR, ULONG_PTR, ULONG_PTR, ULONG_PTR, ULONG_PTR, ULONG_PTR,
                            ULONG_PTR, ULONG_PTR, ULONG_PTR, ULONG_PTR, ULONG_PTR, ULONG_PTR,
                            ULONG_PTR, ULONG_PTR, ULONG_PTR, ULONG_PTR ))proc)
        ( a[0], a[1], a[2],  a[3],  a[4],  a[5],  a[6],  a[7],
          a[8], a[9], a[10], a[11], a[12], a[13], a[14], a[15] );
}

/***********************************************************************
 *           emu_trap_dispatch
 *
 * Entered on the Win32 stack through call_user_mode_callback(), the same way
 * KiUserCallbackDispatcher is, and returns through NtCallbackReturn.
 *
 * It cannot simply be called from the emulator's trap callback.  The emulator
 * is unix code and runs on this thread's kernel stack, whereas the functions
 * this dispatches to are Win32 code.  Running them on the kernel stack is not
 * just a convention violation: the syscall dispatcher would reuse the very
 * syscall frame that still describes the unix call the emulator is running
 * inside, and would switch r1 back into the live frames below it.  So any
 * callee that made an NT syscall or raised an exception corrupted the kernel
 * stack under the emulator -- measured as a glibc stack-protector abort
 * followed by "Exception frame is not in stack limits".
 */
void WINAPI emu_trap_dispatch( ULONG id, void *args, ULONG len )
{
    AMD64_CONTEXT *ctx = *(AMD64_CONTEXT **)args;
    AMD64_CONTEXT *prev_trap_ctx = emu_current_trap_ctx;
    thunk_override_func override = NULL;
    struct com_thunk_hit com = { 0 };
    NTSTATUS status = STATUS_SUCCESS;
    ULONG_PTR a[THUNK_MAX_ARGS] = { 0 };
    UINT sig = 0, argc, cb_mask = 0, fp = 0;
    ULONG_PTR ret;
    void *proc;

    /* raise-style overrides dispatch against this trap's guest state; saved
     * and restored so a nested dispatch (guest handler makes a thunk call)
     * leaves the outer one intact */
    emu_current_trap_ctx = ctx;

    proc = find_guest_thunk_target( ctx->Rip, &sig, &override, &com, &cb_mask, &fp );
    argc = THUNK_SIG_ARGC(sig);
    if (com.dispatch)
    {
        /* A COM vtable slot.  The module behind __wine_com_dispatch owns all
         * marshalling and has written ctx->Rax; this side owns control flow:
         * pop the return address the guest's CALL pushed. */
        status = com.dispatch( com.iface, com.slot, ctx );
        if (status)
        {
            ERR( "com dispatch iface %u slot %u failed, status %08x\n",
                 com.iface, com.slot, (UINT)status );
            status = STATUS_ILLEGAL_INSTRUCTION;
        }
        else
        {
            ctx->Rip = *(DWORD64 *)(ULONG_PTR)ctx->Rsp;
            ctx->Rsp += 8;
        }
    }
    else if (!proc && !override)
    {
        ERR( "unhandled guest trap at %p\n", (void *)(ULONG_PTR)ctx->Rip );
        status = STATUS_ILLEGAL_INSTRUCTION;
    }
    else
    {
        TRACE( "trap at %p: arg0(r10)=%p rdx=%p r8=%p r9=%p guest rsp=%p; host sp ~%p in %p-%p\n",
               (void *)(ULONG_PTR)ctx->Rip, (void *)(ULONG_PTR)ctx->R10, (void *)(ULONG_PTR)ctx->Rdx,
               (void *)(ULONG_PTR)ctx->R8, (void *)(ULONG_PTR)ctx->R9, (void *)(ULONG_PTR)ctx->Rsp,
               &argc, NtCurrentTeb()->Tib.StackLimit, NtCurrentTeb()->Tib.StackBase );

        if (fp && !override)
        {
            /* Floating point on either side: the arguments have to be split
             * into ELFv2's two register files and the result may come back in
             * f1 rather than r3.  See marshal_thunk_args_fp(). */
            ULONG_PTR gpr[THUNK_MAX_ARGS];
            double fpr[THUNK_MAX_FP_ARGS], fp_ret = 0.0;

            marshal_thunk_args_fp( ctx, argc, THUNK_SIG_NARROW(sig), fp, gpr, fpr );
            ret = call_native_thunk_fp( proc, gpr, fpr, &fp_ret );

            /* MS-x64 returns a float or double in XMM0, not RAX.  Write the
             * whole register: the guest reads 4 or 8 bytes of it and stale
             * high bytes from a previous call would be visible to code that
             * reads it wider than it wrote. */
            switch (THUNK_FP_RET( fp ))
            {
            case THUNK_FP_RET_DOUBLE:
                memset( &ctx->FltSave.XmmRegisters[0], 0, sizeof(ctx->FltSave.XmmRegisters[0]) );
                *(double *)&ctx->FltSave.XmmRegisters[0] = fp_ret;
                break;
            case THUNK_FP_RET_FLOAT:
                memset( &ctx->FltSave.XmmRegisters[0], 0, sizeof(ctx->FltSave.XmmRegisters[0]) );
                *(float *)&ctx->FltSave.XmmRegisters[0] = (float)fp_ret;
                break;
            }
        }
        else
        {
            marshal_thunk_args( ctx, argc, THUNK_SIG_NARROW(sig), a );
            /* registration-side interception of guest callbacks: swap each
             * declared callback argument for a native trampoline BEFORE the
             * native callee ever sees the pointer */
            if (cb_mask) wrap_thunk_callback_args( a, argc, cb_mask );
            if (sig & THUNK_SIG_VARIADIC)
            {
                /* the v-variant takes the fixed arguments plus one va_list */
                a[argc] = (ULONG_PTR)marshal_thunk_va_list( ctx, argc );
                argc++;
            }
            ret = override ? override( a, proc ) : call_native_thunk( proc, a );
        }

        /* return to the guest's caller: the trap fired with Rip still on the
         * trap opcode and Rsp still pointing at the return address its CALL
         * pushed */
        ctx->Rip = *(DWORD64 *)(ULONG_PTR)ctx->Rsp;
        ctx->Rsp += 8;
        ctx->Rax = ret;
    }

    emu_current_trap_ctx = prev_trap_ctx;

    /* Ending the run is how a guest ExitThread unwinds; see emu_ExitThread. */
    if (guest_exit_requested) status = STATUS_THREAD_IS_TERMINATING;
    /* ...and how an unhandled guest raise surfaces (dispatch_guest_raise);
     * the record itself waits in guest_exc_rec for the run's PE caller. */
    else if (guest_exc_pending && !status) status = STATUS_EMU_GUEST_EXCEPTION;

    status = NtCallbackReturn( NULL, 0, status );
    RtlRaiseStatus( status );
}


/***********************************************************************
 *           RtlUserThreadStart (NTDLL.@)
 */
/* the thread that ran the main image; see RtlUserThreadStart */
static HANDLE emu_first_guest_thread;

void WINAPI RtlUserThreadStart( PRTL_THREAD_START_ROUTINE entry, void *arg )
{
    __TRY
    {
        BOOL unclassifiable;

        if (thread_start_is_guest_code( entry, &unclassifiable ) || unclassifiable)
        {
            struct emu_run_entry_params params = { (void *)entry, arg, 0, emu_trap_dispatch };
            NTSTATUS status;

            params.exception_dispatcher = emu_exception_dispatch;

            /* The first thread to run guest code is the one that ran the main
             * image: a guest cannot create a thread before its own entry point
             * executes, so this is unambiguous and needs no lock. */
            if (!emu_first_guest_thread) emu_first_guest_thread = NtCurrentTeb()->ClientId.UniqueThread;

            if (unclassifiable)
            {
                ERR( "thread start %p is in no loaded image; refusing to run it either way\n", entry );
                RtlExitUserThread( STATUS_INVALID_IMAGE_FORMAT );
            }
            status = WINE_UNIX_CALL( unix_emu_run_entry, &params );

            /* A guest ExitThread ended the run deliberately; that is not a
             * failure and must be checked before the error path below. */
            if (guest_exit_requested)
            {
                TRACE( "guest thread exited with %u\n", guest_exit_code );
                RtlExitUserThread( guest_exit_code );
            }

            /* A guest exception no guest-level handler consumed: re-raise it
             * natively inside this __TRY, so the vectored handlers, the
             * unhandled-exception filter (a guest filter sits behind it as a
             * trampoline already) and the __EXCEPT below produce a
             * correctly-coded, reported death -- the guest Rip is in the
             * record.  Does not return when one was pending. */
            raise_pending_guest_exception();

            /* not wine_dbgstr_longlong(): on this port PE-side code is built by
             * the native ELF compiler, so its `unsigned long` is 64 bits and the
             * helper takes its single-%lx branch -- which ntdll's MSVC-style
             * printf then truncates to 32 bits.  %p is formatted by width. */
            TRACE_(relay)( "\1Guest AMD64 entry %p returned rax=%p status=%08x\n",
                           entry, (void *)(ULONG_PTR)params.retval, (UINT)status );
            if (status)
            {
                ERR( "failed to emulate AMD64 entry point %p, status %08x\n", entry, (UINT)status );
                /* Killing the process is right only for the initial thread,
                 * where nothing else can be running.  A guest worker thread
                 * that fails to start must take down itself, not everything. */
                if (NtCurrentTeb()->ClientId.UniqueThread == emu_first_guest_thread)
                    NtTerminateProcess( GetCurrentProcess(), status );
                RtlExitUserThread( status );
            }
            RtlExitUserThread( (ULONG)params.retval );
        }
        pBaseThreadInitThunk( 0, (LPTHREAD_START_ROUTINE)entry, arg );
    }
    __EXCEPT( call_unhandled_exception_filter )
    {
        NtTerminateProcess( GetCurrentProcess(), GetExceptionCode() );
    }
    __ENDTRY
}


/******************************************************************
 *		LdrInitializeThunk (NTDLL.@)
 */
void WINAPI LdrInitializeThunk( CONTEXT *context, ULONG_PTR unk2, ULONG_PTR unk3, ULONG_PTR unk4 )
{
    loader_init( context, (void **)&context->Gpr3 );
    TRACE_(relay)( "\1Starting thread proc %p (arg=%p)\n", (void *)context->Gpr3, (void *)context->Gpr4 );
    NtContinue( context, TRUE );
}


/***********************************************************************
 *           process_breakpoint
 *
 * "trap" raises SIGTRAP, which the unix side turns into a
 * STATUS_BREAKPOINT.  If a debugger is not attached the exception is
 * unhandled, so unlike the other architectures - which install an SEH handler
 * that steps over the trap - this deliberately only traps when a debugger is
 * present.  There is no .seh_handler on ELF ppc64 to do it the other way.
 */
void WINAPI process_breakpoint(void)
{
    if (NtCurrentTeb()->Peb->BeingDebugged) DbgBreakPoint();
}


/***********************************************************************
 *		DbgUiRemoteBreakin   (NTDLL.@)
 */
void WINAPI DbgUiRemoteBreakin( void *arg )
{
    __TRY
    {
        if (NtCurrentTeb()->Peb->BeingDebugged) DbgBreakPoint();
    }
    __EXCEPT_ALL
    {
        /* ignore */
    }
    __ENDTRY
    RtlExitUserThread( STATUS_SUCCESS );
}


/**********************************************************************
 *              DbgBreakPoint   (NTDLL.@)
 *
 * Padded with nops so that a debugger can patch the entry point, as on the
 * other architectures.
 */
__ASM_GLOBAL_FUNC( DbgBreakPoint, "trap\n\tblr\n\t"
                   "nop; nop; nop; nop; nop; nop; nop; nop\n\t"
                   "nop; nop; nop; nop; nop; nop" )

/**********************************************************************
 *              DbgUserBreakPoint   (NTDLL.@)
 */
__ASM_GLOBAL_FUNC( DbgUserBreakPoint, "trap\n\tblr\n\t"
                   "nop; nop; nop; nop; nop; nop; nop; nop\n\t"
                   "nop; nop; nop; nop; nop; nop" )

#endif  /* __powerpc64__ */
