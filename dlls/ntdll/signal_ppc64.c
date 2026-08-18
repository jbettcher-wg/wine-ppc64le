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
#include <string.h>
#include <setjmp.h>

#include "ntstatus.h"
#include "windef.h"
#include "winternl.h"
#include "ddk/wdm.h"
#include "wine/exception.h"
#include "ntdll_misc.h"
#include "unwind.h"
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
/* The loader entry rather than the base, so that a diagnostic can say WHICH
 * module an address is in; guest_module_from_address() is the same question
 * asked by code that only needs yes or no. */
static LDR_DATA_TABLE_ENTRY *guest_module_entry_from_address( const void *addr )
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
            return mod;
    }
    return NULL;
}

static HMODULE guest_module_from_address( const void *addr )
{
    LDR_DATA_TABLE_ENTRY *mod = guest_module_entry_from_address( addr );

    return mod ? mod->DllBase : NULL;
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
    UINT                 cb_wide;  /* subset of cb_mask whose callback returns a
                                      full 64 bits rather than a sign-extended
                                      32 (an LRESULT; see the trampoline pool) */
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
        const WCHAR *base = name + wcslen( name );
        while (base > name && base[-1] != '\\' && base[-1] != '/') base--;
        /* A path can name two things in the guest namespace: a real guest
         * image on disk -- a game loading its own DLL by full path, opened
         * and machine-checked exactly as a static import's search-path hit
         * is -- or a module the namespace serves by NAME: steam_api64 hands
         * LoadLibraryEx the SteamClientDll64 registry value, which on
         * Windows is a path, and expects the client library.  Try them in
         * that order.  The name-serving fallback follows the precedent the
         * namespace already sets for sysx8664 files: the builtin outranks
         * whatever the path would have named.  Only a path neither road
         * resolves is refused. */
        if (!load_guest_dll( name, IMAGE_FILE_MACHINE_AMD64, &mod )) return (ULONG_PTR)mod;
        if (*base && !load_guest_dll( base, IMAGE_FILE_MACHINE_AMD64, &mod ))
        {
            TRACE( "path %s served by guest module %s\n", debugstr_w(name), debugstr_w(base) );
            return (ULONG_PTR)mod;
        }
        WARN( "path %s not resolvable in the guest namespace\n", debugstr_w(name) );
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

/***********************************************************************
 *           OpenGL entry points, which live in no export table anywhere
 *
 * The same namespace problem GetProcAddress has, with a second twist.
 *
 * Microsoft's opengl32.dll exports GL 1.1 and wgl* and nothing else, and has
 * since 1996.  Everything OpenGL has gained since -- all of modern GL, all of
 * what a 2016 game draws with -- is vended at RUNTIME by wglGetProcAddress and
 * appears in no export table on any Windows machine.  So:
 *
 *   * the ANSWER is a code address the guest will CALL, exactly as with
 *     GetProcAddress, and pass-through would hand it native ppc64 bytes.  It
 *     has to be the matching stub in the guest's own opengl32 thunk module;
 *
 *   * and unlike GetProcAddress there is no export table on either side to
 *     look the name up in.  The guest module carries a stub per name because
 *     dlls/opengl32/opengl32-guest.spec lists them (generated from the very
 *     table Wine's wglGetProcAddress answers from), so THIS side is an
 *     ordinary LdrGetProcedureAddress.  The native side is not, and is served
 *     by thunk_resolvers[] further down.
 *
 * WHETHER a name may be had at all stays the native module's decision: it is
 * asked first, and only a name it answers for -- this driver, this context,
 * this GL version -- gets a stub.  Everything else is NULL, which is a
 * documented answer every GL loader already tests for, said out loud once per
 * name because the interesting case is the third one:
 *
 *     1. Wine's registry has no such name           WARN, the app is probing
 *     2. it has it, this context does not offer it  WARN, normal on any driver
 *     3. it offers it and the guest module has no stub -- the signature oracle
 *        refused it -- which is a gap in THIS port and is an ERR naming it.
 */
#define MAX_GL_NAMES_SAID 4096

static BOOL emu_env_flag( const WCHAR *name );

static char *gl_names_said[MAX_GL_NAMES_SAID];
static UINT gl_names_said_count;

/* -> TRUE the first time this name is passed.  The name is COPIED: it is the
 * guest's string and the guest may free it the moment this returns.  Under the
 * loader lock, which is the lock this file already serialises its other
 * process-wide pools with.  Past the cap it answers TRUE every time -- saying
 * a thing too often is a nuisance, not saying it is the bug. */
static BOOL gl_say_once( const char *name )
{
    ULONG_PTR magic;
    BOOL ret = TRUE;
    UINT i;

    LdrLockLoaderLock( 0, NULL, &magic );
    for (i = 0; i < gl_names_said_count; i++)
    {
        if (strcmp( gl_names_said[i], name )) continue;
        ret = FALSE;
        goto done;
    }
    if (gl_names_said_count < MAX_GL_NAMES_SAID)
    {
        SIZE_T len = strlen( name ) + 1;
        char *copy = RtlAllocateHeap( GetProcessHeap(), 0, len );

        if (copy)
        {
            memcpy( copy, name, len );
            gl_names_said[gl_names_said_count++] = copy;
        }
    }
done:
    LdrUnlockLoaderLock( 0, magic );
    return ret;
}

static ULONG_PTR emu_wglGetProcAddress( const ULONG_PTR *a, void *native )
{
    static int novend = -1;
    const char *name = (const char *)a[0];
    ANSI_STRING str;
    HMODULE guest;
    void *proc;

    if (!native)
    {
        ERR( "no native opengl32 to answer wglGetProcAddress\n" );
        return 0;
    }
    if (!name) return 0;

    /* The negative control, same shape as WINEEMUNOCBWRAP: hand the guest the
     * NATIVE address, which is the defect this override exists to prevent --
     * ppc64 bytes the guest will CALL as x86-64.  ppc64le/opengl/
     * check-gl-smoke.sh requires the gate to go red under it. */
    if (novend == -1) novend = emu_env_flag( L"WINEEMUNOGLVEND" );
    if (novend)
    {
        ULONG_PTR raw = ((ULONG_PTR (*)( ULONG_PTR ))native)( a[0] );
        ERR( "WINEEMUNOGLVEND: handing the guest the native %s at %p\n",
             debugstr_a(name), (void *)raw );
        return raw;
    }

    /* The native module owns "may this caller have it?": get_function_entry
     * plus the GL version and extension checks in wglGetProcAddress.  A guest
     * that got a stub for something the driver does not implement would
     * configure itself for an extension it does not have and fail later,
     * somewhere else. */
    if (!((ULONG_PTR (*)( ULONG_PTR ))native)( a[0] ))
    {
        if (gl_say_once( name ))
            WARN( "wglGetProcAddress(%s): not offered by this context; NULL\n",
                  debugstr_a(name) );
        return 0;
    }
    if (!(guest = find_guest_module( L"opengl32.dll" )))
    {
        ERR( "wglGetProcAddress(%s) with no guest opengl32 loaded\n", debugstr_a(name) );
        return 0;
    }
    RtlInitAnsiString( &str, name );
    if (LdrGetProcedureAddress( guest, &str, 0, &proc ))
    {
        if (gl_say_once( name ))
            ERR( "wglGetProcAddress(%s): native opengl32 offers it but the guest "
                 "thunk module has no stub -- refused at generation time; NULL "
                 "(see dlls/opengl32/opengl32.thunks)\n", debugstr_a(name) );
        return 0;
    }
    TRACE( "wglGetProcAddress(%s) -> guest stub %p\n", debugstr_a(name), proc );
    return (ULONG_PTR)proc;
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

/* How many emulator runs are stacked on this thread: 0 while the thread's own
 * run is the only one, one more for each nested run underneath it.  Read by
 * the guest SEH block far below, which enters a guest language handler in a
 * run of its own and then has to decide whether an RtlUnwindEx arriving from
 * guest code is THAT handler's -- "issued at the depth the handler runs at" is
 * the exact question, and a deeper run (the handler called a guest callback
 * which unwound) is a different one that must not be answered as if it were. */
static __thread UINT guest_run_depth;

/* Set by an RtlUnwindEx that belongs to a guest language handler running in a
 * nested run: the run must END so that the frame walk which started it can
 * perform the unwind against the FAULTING stack, and this is what makes
 * emu_trap_dispatch end it.  Cleared here, by the initiator of that run. */
static __thread BOOL guest_unwind_run_end;

static ULONG_PTR call_guest_function( void *entry, void *arg )
{
    struct emu_run_entry_params params = { entry, arg, 0, emu_trap_dispatch };
    NTSTATUS status;

    params.exception_dispatcher = emu_exception_dispatch;
    guest_run_depth++;
    status = WINE_UNIX_CALL( unix_emu_run_entry, &params );
    guest_run_depth--;

    /* Not a failure: guest code inside the run called RtlUnwindEx, which does
     * not return, and ending the run is how this port spells that -- see
     * guest_request_unwind().  The request itself is already recorded in the
     * caller's struct guest_handler_call and there is no return value to
     * report, so the status the run ended with says nothing here.  Consumed
     * unconditionally, by the initiator of exactly the run that set it: a flag
     * left standing would make the NEXT nested run's failure read as an
     * unwind. */
    if (guest_unwind_run_end)
    {
        guest_unwind_run_end = FALSE;
        return 0;
    }

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

/* THE RE-RAISE IS NOT RE-ENTRANT, AND A REAL GAME PROVED IT.
 *
 * "Re-raise it natively" ends, for an exception nobody handles, in the
 * unhandled-exception filter -- and behind that filter sits THE GUEST'S OWN
 * top-level filter, as a trampoline.  A crash reporter is exactly the code
 * most likely to fault while reporting a crash, and when it does, its fault is
 * another unhandled guest exception on the same thread, which arrives back
 * here and starts the whole report again.  Nothing in the path notices.
 *
 * Measured on a live Steam launch of DOOM (2026-08-17): the game called the
 * missing-import sentinel 0xdead001d (iphlpapi.GetAdaptersInfo, served now --
 * see dlls/iphlpapi/iphlpapi.thunks), its own filter at DOOMx64vk.exe+0x19a820
 * faulted c0000005 at 0x1402836ac trying to report it, and the pair
 * "re-raising natively" -> "calling TEB handler" -> "calling guest callback"
 * repeated for 804,000 log lines, ~4.8 KiB of native stack each time, until
 * the 8 MiB thread stack was gone (c00000fd, then "stack overflow 1504
 * bytes").  The thread never died: it spun at 0% holding a critical section
 * another thread was waiting on, so the process HUNG.  A hang is strictly
 * worse than a crash -- the sentinel's whole promise is that the death names
 * the symbol, and a process that never dies never says anything.
 *
 * guest_seh_depth does not cover this.  That counter bounds nesting WITHIN a
 * guest-level dispatch and is decremented before the record is handed over;
 * this loop is one balanced guest dispatch per iteration, driven from the
 * native side.  The counter below is the native side's own, and its rule is
 * absolute: one unhandled guest exception may be reported per thread.  A
 * second one arriving while the first is still being reported means the
 * reporting itself is the thing that is broken, and the honest response is to
 * name both and terminate -- never to try again on a stack that has just been
 * proven to be running out.
 *
 * GUEST_EXC_STACK_FLOOR is the independent second net, for the same failure
 * arriving by a route this flag cannot see (a deep native stack that is not
 * this loop).  Dispatching a native exception costs a CONTEXT plus the whole
 * handler chain; entering that with less than this left is how the log's
 * c00000fd storm began, and it is diagnosable only before the fact. */
static __thread void *guest_exc_raising;   /* NULL, or where the report began */
static __thread EXCEPTION_RECORD guest_exc_first;
#define GUEST_EXC_STACK_FLOOR (64 * 1024)

/* the trap CONTEXT the innermost emu_trap_dispatch on this thread is
 * serving: what a raise-style override (emu_RaiseException) dispatches
 * against.  Saved/restored around each dispatch, so nesting works. */
static __thread AMD64_CONTEXT *emu_current_trap_ctx;

/* Set by an override that has REPLACED that CONTEXT wholesale rather than
 * returned a value into it -- a guest raise whose __except was found by the
 * frame walk, which resumes in another frame.  emu_trap_dispatch's ordinary
 * "pop the return address, store RAX" fixup is wrong in that case and only in
 * that case.  Saved/restored alongside emu_current_trap_ctx. */
static __thread BOOL emu_trap_ctx_rewritten;

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
 *           table-based guest dispatch (.pdata / .xdata)
 *
 * The mechanism an MSVC-compiled x86-64 binary actually uses.  Its __try
 * frames are not registered anywhere at runtime -- there is no chain to walk;
 * the compiler emitted a RUNTIME_FUNCTION per function into .pdata and an
 * UNWIND_INFO per function into .xdata, and dispatch means walking the guest
 * stack frame by frame, virtually unwinding an AMD64_CONTEXT with that data
 * and asking each frame's language handler whether it wants the exception.
 *
 * The unwinder itself is not written here.  Wine already solves "unwind
 * x86-64 on a host that is not x86-64" for ARM64EC, by compiling the other
 * architecture's block of unwind.c under suffixed names; the x86-64 block is
 * now built on ppc64 the same way and this file calls
 * RtlLookupFunctionEntry_amd64() / RtlVirtualUnwind2_amd64().  What is genuinely
 * new here is everything that has to cross the machine boundary:
 *
 *   - The frame walk runs NATIVELY, over a copy of the guest's AMD64_CONTEXT.
 *     Guest memory is host memory on this port, so reading the guest's .pdata
 *     and its stack is ordinary pointer work; only executing guest code needs
 *     the emulator.
 *
 *   - A language handler is ENTERED AS GUEST CODE.  It is an x86-64 function
 *     of the guest's, named by an RVA in the guest's own .xdata, and nothing
 *     in a PE says which handler it is: an image linked against the static
 *     MSVC runtime carries its own byte-identical copy of __C_specific_handler
 *     (DOOM (2016), at DOOMx64vk.exe+0x1eab2c8), its own __GSHandlerCheck
 *     (steam_api64.dll+0xed68), and its own __CxxFrameHandler*.  So the walk
 *     calls handler(rec, EstablisherFrame, ctx, DISPATCHER_CONTEXT*) through
 *     call_guest_function_args() -- the same nested-run primitive TLS
 *     callbacks and DllMain use -- with a DISPATCHER_CONTEXT_AMD64 built by
 *     guest_virtual_unwind(), and honours the disposition it returns.
 *
 *   - The guest ntdll's OWN __C_specific_handler is still recognised by
 *     identity and served natively (guest_c_specific_handler), because for
 *     that one address the semantics are known and the round trip through the
 *     emulator buys nothing.  Both paths are handed the same
 *     DISPATCHER_CONTEXT built in the same place, so they cannot drift apart
 *     structurally, and both report an accepting scope the same way.  Only the
 *     filter and __finally FUNCLETS of the native path are guest code, entered
 *     with the x64 funclet contract: filter(EXCEPTION_POINTERS*,
 *     EstablisherFrame) and termination_handler(BOOLEAN abnormal,
 *     EstablisherFrame).  A funclet addresses its parent's locals through the
 *     establisher frame it is handed, never through the stack it is running
 *     on, which is why running it on the nested run's own guest stack is
 *     correct.
 *
 *   - Nothing does a non-local jump.  Native x64 __C_specific_handler calls
 *     RtlUnwindEx, which never returns.  A guest handler entered here does the
 *     same thing, and RtlUnwindEx cannot return either -- but it also cannot
 *     jump, because the frames it would abandon include the NATIVE frames of
 *     the emulator run that is running the handler.  So the request is
 *     recorded (guest_request_unwind) and the handler's run is ENDED; the
 *     frame walk that entered the handler picks the request up and performs
 *     the unwind itself, against the faulting stack.  The identity path does
 *     the same thing with a plain return.  Guest code that calls RtlUnwindEx
 *     while not inside a handler this walk entered is unwinding within its own
 *     run, and is served in place (guest_unwind_in_place).
 *
 *   - "Resuming in the __except block" is a context write, not a stack switch.
 *     The unwind produces the target frame's AMD64_CONTEXT with Rip at the
 *     jump target and Rax at the unwind's return value; storing that into the
 *     CONTEXT the emulator trapped with and returning STATUS_SUCCESS makes the
 *     guest continue there, using the machinery that already existed for a
 *     handler that says "continue execution".
 *
 *   - A C++ CATCH BLOCK is that same context write with one extra step.  MSVC
 *     spells `catch` as STATUS_UNWIND_CONSOLIDATE: the unwind runs as usual,
 *     but instead of resuming at TargetIp the CALLBACK in
 *     ExceptionInformation[0] is run and its return value is the real resume
 *     Rip.  Implemented in guest_consolidate_callback(), one place, at the end
 *     of the unwind both entry roads share -- see it for what the callback is
 *     handed and what it is not.
 *
 *   - A COLLIDED UNWIND is an unwind started from inside an unwind, and it is
 *     implemented rather than refused.  A __finally this file is running calls
 *     RtlUnwindEx for a frame on the stack being torn down; the x64 contract
 *     says the inner unwind wins and continues FROM THE COLLISION POINT, so
 *     that the frames already unwound are not unwound twice and the scope that
 *     collided does not run twice.  Windows discovers the outer unwind by
 *     walking into ntdll's own frame and reading ExceptionCollidedUnwind off
 *     RtlpUnwindHandler; there is no such frame on a guest stack here, so the
 *     collision is recorded (struct guest_unwind_state) and adopted in situ by
 *     the loop that is already standing at exactly that point.  A handler that
 *     RETURNS ExceptionCollidedUnwind is served too, in both phases, by
 *     believing the dispatcher context it hands back (collided_adopt).
 *
 * What is NOT implemented, and refuses loudly rather than answering wrongly --
 * each with its own ERR naming itself, none of them silently continuing the
 * search:
 *
 *   - an exit unwind (RtlUnwindEx with a null target frame), which has no
 *     frame to resume in;
 *   - a collision that makes no progress: a handler that hands back the same
 *     dispatcher context, or collides at the same scope, twice running, which
 *     would otherwise re-enter that handler forever;
 *   - a collision raised more than one emulator run below the funclet, which
 *     cannot be adopted because only the innermost run can be ended;
 *   - an exception taken on the very stack an unwind is tearing down, as
 *     opposed to inside a funclet of it, which no guest code should be able to
 *     reach and which a second walk cannot serve.
 */

/* Two private dispositions, spelled far outside the four ExceptionXxx values
 * so that neither can ever be confused with one a handler actually returned.
 * Which is also why everything below returns UINT rather than
 * EXCEPTION_DISPOSITION: a value outside an enumeration should not be spelled
 * as a member of it.
 *
 *   _guest      "this frame's handler accepted, and *target says where to
 *               unwind to".  Native x86-64 needs no such value because its
 *               handler does not return in that case -- it calls RtlUnwindEx,
 *               which jumps.
 *   _refused    "this port declined to run this handler at all, loudly, and
 *               said why".  Distinct from ExceptionContinueSearch because
 *               continuing the search would silently deny the frame a handler
 *               it is entitled to. */
#define ExceptionExecuteHandler_guest 0xdead0001u
#define ExceptionHandler_refused      0xdead0002u

/* A guest frame to unwind to: filled in either by the search phase (a filter
 * accepted) or by a guest handler's RtlUnwindEx, and acted on by the walk.
 * frame == 0 means "nobody accepted".  Everything an unwind needs travels in
 * here BY VALUE, the record included: a guest handler's EXCEPTION_RECORD may
 * live on the stack of the run that is about to end. */
struct guest_unwind_target
{
    ULONG64          frame;        /* establisher frame to resume in */
    ULONG64          target_ip;    /* where in it to resume */
    ULONG64          return_value; /* RAX on arrival: the unwind's return value */
    EXCEPTION_RECORD rec;          /* the record the unwind phase runs with */
};

/* THE UNWIND THIS THREAD IS PERFORMING, if any -- one record per live
 * guest_unwind_to_target(), innermost first.  Per thread, because an unwind is
 * a per-thread activity.
 *
 * It exists for the COLLIDED UNWIND: a __finally funclet that this unwind is
 * running calls RtlUnwindEx itself, naming a frame on the stack this unwind is
 * tearing down.  On x86-64 that arrives at ntdll's own RtlpUnwindHandler, the
 * language handler of the frame the outer RtlUnwindEx is running the funclet
 * from, which reports ExceptionCollidedUnwind to the inner unwind together
 * with the OUTER unwind's DISPATCHER_CONTEXT; the inner unwind adopts that
 * state and continues from the frame the outer one had reached.  There is no
 * such native frame on a guest stack here -- the outer unwind is native code
 * of this file and the funclet is running in a nested emulator run -- so the
 * same fact is recorded here instead, and guest_unwind_to_target() adopts it
 * in situ.  See the block comment on that function.
 *
 * stack_base/stack_limit are the stack being torn down, and they are what
 * tells a collided unwind from an ordinary one: a funclet's RtlUnwindEx that
 * names a frame on the FAULTING stack is a collision, one that names a frame
 * on the funclet's own run stack is a local unwind inside that run and is
 * served in place, exactly as it would be anywhere else.
 *
 * funclet_depth is the emulator run depth a funclet of THIS unwind runs at,
 * for the same reason struct guest_handler_call carries one: a funclet that
 * calls a guest callback which itself unwinds is unwinding inside that
 * callback's run, which is a different question with a different answer. */
struct guest_unwind_state
{
    struct guest_unwind_state *prev;
    UINT                       funclet_depth;
    void                      *stack_base;
    void                      *stack_limit;
    ULONG64                    frame;      /* where this unwind is heading, for diagnostics */
    ULONG64                    target_ip;
    BOOL                       collided;   /* a funclet started a second unwind... */
    struct guest_unwind_target again;      /* ...to here */
};
static __thread struct guest_unwind_state *guest_unwind_state;

/***********************************************************************
 *           guest_unwind_current
 *
 * The unwind this thread is inside, or NULL -- with the records of unwinds
 * whose frames are GONE dropped first.
 *
 * Each record lives in its guest_unwind_to_target() frame and is unlinked by
 * that function on the way out, which is enough for every ordinary return.  It
 * is not enough for the one path that does not return: a funclet raises, the
 * exception is unhandled at guest level, raise_pending_guest_exception()
 * re-raises it natively, and a native __EXCEPT_ALL above this walk -- ntdll's
 * own dispatch_user_callback has one -- swallows it and unwinds straight past
 * the unwind's frame.  Nothing runs on the way out then, and a record left
 * standing makes every later exception on that thread look like it arrived
 * during an unwind.  Measured: DOOM (2016) reaches such a state and every
 * subsequent guest exception on the thread is answered as a collided unwind.
 *
 * The test is the one raise_pending_guest_exception() uses on its own marker,
 * and for the same reason -- a stack address needs no undoing.  A live record
 * belongs to a frame this call is nested INSIDE, so it is strictly deeper in
 * the stack's own direction of growth than this frame is; one that is not is a
 * record whose frame has been abandoned.
 */
static struct guest_unwind_state *guest_unwind_current(void)
{
    const char *here = __builtin_frame_address( 0 );

    while (guest_unwind_state && (const char *)guest_unwind_state <= here)
    {
        /* Loud, because it means an unwind was abandoned part way through:
         * the __finally blocks below the abandonment point never ran. */
        ERR( "the record of an unwind to frame %I64x ip %I64x outlived its own native "
             "frame; a native unwind carried this thread past a guest unwind in "
             "progress.  Dropping it -- keeping it would make every later exception on "
             "this thread look like a collided unwind\n",
             guest_unwind_state->frame, guest_unwind_state->target_ip );
        guest_unwind_state = guest_unwind_state->prev;
    }
    return guest_unwind_state;
}

/***********************************************************************
 *           guest_unwind_take_collision
 *
 * Whether the unwind this thread is inside has just been COLLIDED WITH -- a
 * funclet it was running started a second unwind -- and if so where that
 * second unwind wants to go.  Consumed, because one collision is adopted once.
 */
static BOOL guest_unwind_take_collision( struct guest_unwind_target *target )
{
    struct guest_unwind_state *unwind = guest_unwind_current();

    if (!unwind || !unwind->collided) return FALSE;
    *target = unwind->again;
    unwind->collided = FALSE;
    return TRUE;
}

/* One guest language handler that this dispatch has entered as guest code,
 * innermost first.  It exists so that RtlUnwindEx arriving from inside that
 * handler can be recognised as ITS unwind -- the one that must not jump,
 * because the frames between here and the target are on the FAULTING guest
 * stack, not on the stack of the run the handler is using, and the run's own
 * native frames have to be returned through rather than abandoned.
 *
 * run_depth, not merely "a handler is running": a handler that calls a guest
 * callback which itself unwinds is unwinding inside that callback's run, which
 * is a different question with a different answer (guest_unwind_in_place). */
struct guest_handler_call
{
    struct guest_handler_call *prev;
    UINT                       run_depth;  /* the emulator run the handler runs in */
    BOOL                       unwound;    /* it called RtlUnwindEx instead of returning */
    struct guest_unwind_target target;     /* ...and this is what it asked for */
};
static __thread struct guest_handler_call *guest_handler_call;

/* Recursion bound on guest exception dispatch.  A filter or __finally funclet
 * runs as guest code and can fault, which dispatches again from inside this
 * dispatch; a handful of levels is legitimate, an unbounded number is a fault
 * loop that would otherwise consume the native stack silently. */
#define GUEST_SEH_MAX_DEPTH 8
static __thread UINT guest_seh_depth;

/* Bound on the frame walk itself.  Guest code with no exception directory
 * unwinds as a leaf -- pop eight bytes and call them a return address -- so a
 * walk that starts in the wrong place climbs the stack a word at a time.  The
 * module check below normally stops that within one or two frames; this stops
 * it in every case. */
#define GUEST_SEH_MAX_FRAMES 256

/***********************************************************************
 *           guest_c_specific_handler_addr
 *
 * The address a guest's .xdata means when it names __C_specific_handler: the
 * trap stub of that export in the guest namespace's ntdll, which is what the
 * guest image's import was bound to at load time.  Cached; the module that
 * owns it is never unloaded.
 */
static void *guest_c_specific_handler_addr(void)
{
    /* Cached only on SUCCESS, so that two threads racing here can differ only
     * in doing the same lookup twice, never in one of them seeing a
     * half-published NULL and refusing a handler that is perfectly good. */
    static void *cached;
    ANSI_STRING name;
    HMODULE ntdll;
    void *proc;

    if (cached) return cached;
    if (!(ntdll = find_guest_module( L"ntdll.dll" )))
    {
        /* This used to be routine: a process where no guest image IMPORTS
         * ntdll had no guest ntdll loaded, and DOOM (2016) hit exactly that --
         * it and steam_api64.dll take RtlUnwindEx and RtlLookupFunctionEntry
         * from KERNEL32 instead, and the old wording here accused the guest
         * ntdll of missing an export it does in fact have, in a process that
         * never loaded it -- 8656 times in one startup.
         *
         * It is not routine any more.  loader_init() (dlls/ntdll/loader.c)
         * now seeds every AMD64 guest process's namespace with ntdll.dll once
         * at bringup, before any guest instruction runs, whether or not any
         * image imports it -- the guest namespace answer Windows itself
         * guarantees.  A seed failure is fatal there, loudly, and does not
         * leave a process able to reach this far.  So reaching NULL here now
         * means one of two real defects: this function was reached from a
         * process whose main image was never AMD64 (a caller-side bug, since
         * nothing but guest .xdata dispatch should ever ask), or the seed ran
         * and still could not be found under this name (a namespace bug).
         * Neither is "a fact about the process" any more -- said every time,
         * not once, because it should now be rare enough that burying it
         * would be the wrong trade. */
        ERR( "no guest ntdll is loaded in this process even though guest bringup seeds one "
             "for every AMD64 main image -- either this was reached from a non-guest process "
             "or the bringup seed did not take; no .xdata language handler can be recognised "
             "and every handler is entered as guest code\n" );
        return NULL;
    }
    RtlInitAnsiString( &name, "__C_specific_handler" );
    if (LdrGetProcedureAddress( ntdll, &name, 0, &proc ))
    {
        ERR( "the guest ntdll exports no __C_specific_handler, so no language handler "
             "in this process can be recognised; table-based dispatch is off\n" );
        return NULL;
    }
    return cached = proc;
}

/***********************************************************************
 *           follow_guest_jmp_thunk
 *
 * A language handler is an RVA into the image that owns the .xdata, so an
 * IMPORTED handler cannot be named directly: the linker plants a jump thunk in
 * the image and the RVA names the thunk.  Follow those jumps -- lld emits
 * `jmp *disp32(%rip)` for an import and `jmp rel32` for a range extension --
 * so that the comparison below is against the address the import actually
 * resolved to.  Bounded, because a chain that loops is a corrupt image and not
 * something to spin on.
 */
static ULONG_PTR follow_guest_jmp_thunk( ULONG_PTR addr )
{
    UINT i;

    for (i = 0; i < 4 && addr; i++)
    {
        const BYTE *p = (const BYTE *)addr;

        if (p[0] == 0xff && p[1] == 0x25)        /* jmp *disp32(%rip) */
            addr = *(ULONG_PTR *)(addr + 6 + *(const INT *)(p + 2));
        else if (p[0] == 0xe9)                   /* jmp rel32 */
            addr = addr + 5 + *(const INT *)(p + 1);
        else break;
    }
    return addr;
}

/***********************************************************************
 *           guest_language_handler_is_c_specific
 */
static BOOL guest_language_handler_is_c_specific( PEXCEPTION_ROUTINE handler )
{
    void *want = guest_c_specific_handler_addr();

    if (!want) return FALSE;
    if ((void *)handler == want) return TRUE;
    return (void *)follow_guest_jmp_thunk( (ULONG_PTR)handler ) == want;
}

/***********************************************************************
 *           call_guest_filter / call_guest_termination_handler
 *
 * The two funclet shapes of the x64 SEH contract, each one guest code entered
 * through the nested-run primitive.  The filter's EXCEPTION_POINTERS lives on
 * the native stack, which the guest can read: guest memory is host memory.
 */
static LONG call_guest_filter( ULONG_PTR filter, EXCEPTION_RECORD *rec,
                               AMD64_CONTEXT *ctx, ULONG64 frame )
{
    struct guest_exception_pointers ptrs = { rec, ctx };
    LONG res;

    TRACE( "calling guest filter %p ptrs %p frame %I64x\n", (void *)filter, &ptrs, frame );
    res = (LONG)(DWORD)call_guest_function_args( (void *)filter, (ULONG_PTR)&ptrs,
                                                 (ULONG_PTR)frame, 0, 0 );
    TRACE( "guest filter %p returned %d\n", (void *)filter, (int)res );
    return res;
}

static void call_guest_termination_handler( ULONG_PTR handler, ULONG64 frame, BOOL abnormal )
{
    TRACE( "calling guest __finally %p frame %I64x abnormal %u\n",
           (void *)handler, frame, abnormal );
    call_guest_function_args( (void *)handler, abnormal ? 1 : 0, (ULONG_PTR)frame, 0, 0 );
}

/***********************************************************************
 *           call_guest_handler_run
 *
 * A guest exception handler, entered as guest code with the four-argument x64
 * contract --
 *   EXCEPTION_DISPOSITION handler( EXCEPTION_RECORD *, void *EstablisherFrame,
 *                                  CONTEXT *, DISPATCHER_CONTEXT * )
 * -- and with the bookkeeping that makes an RtlUnwindEx issued from INSIDE it
 * recognisable as that handler's (guest_request_unwind).  Both kinds of guest
 * handler this file enters go through here -- a frame's language handler and a
 * TEB-chain handler -- because the difference between them is where the
 * address came from, not what happens when it decides to unwind, and a second
 * copy of this that forgot the bookkeeping would unwind the wrong stack.
 *
 * Every one of those four structures has to be readable by the guest.  They
 * are: on this port guest memory IS host memory, so the record, the
 * AMD64_CONTEXT and the DISPATCHER_CONTEXT_AMD64 that the walk built on the
 * native stack are ordinary addresses to the handler, exactly as the filter's
 * EXCEPTION_POINTERS already is.  What the handler must NOT be handed is a
 * ppc64 CONTEXT or a ppc64 DISPATCHER_CONTEXT, and the types here are the
 * _AMD64 ones by name so that a host with different shapes cannot silently
 * substitute its own.
 *
 * -> ExceptionExecuteHandler_guest with *target filled in when it unwound
 *    instead of returning; otherwise the disposition it returned.
 */
static UINT call_guest_handler_run( ULONG_PTR handler, EXCEPTION_RECORD *rec, ULONG64 frame,
                                    AMD64_CONTEXT *ctx, DISPATCHER_CONTEXT_AMD64 *dispatch,
                                    struct guest_unwind_target *target )
{
    struct guest_handler_call call = { guest_handler_call, guest_run_depth + 1 };
    UINT res;

    guest_handler_call = &call;
    res = (UINT)(DWORD)call_guest_function_args( (void *)handler, (ULONG_PTR)rec,
                                                 (ULONG_PTR)frame, (ULONG_PTR)ctx,
                                                 (ULONG_PTR)dispatch );
    guest_handler_call = call.prev;

    if (call.unwound)
    {
        /* It called RtlUnwindEx, which on Windows does not return.  Here it
         * returned by ending its own run, and the request is the answer. */
        *target = call.target;
        TRACE( "guest handler %p unwound to frame %I64x ip %I64x retval %I64x\n",
               (void *)handler, target->frame, target->target_ip, target->return_value );
        return ExceptionExecuteHandler_guest;
    }
    TRACE( "guest handler %p returned disposition %u\n", (void *)handler, res );
    return res;
}

/***********************************************************************
 *           call_guest_language_handler
 *
 * A frame's language handler: the general answer to "which handler is this",
 * which is a question a PE cannot be asked.
 *
 * The address is checked against the guest module list first.  A language
 * handler is base+RVA of the image that owns the .xdata, so it is guest code
 * by construction -- unless the RVA names an import thunk, in which case it is
 * whatever that import resolved to.  Entering a NATIVE address as guest code
 * would run ppc64 bytes as x86-64, which is the failure this port exists to
 * stop; refuse by name instead.
 */
static UINT call_guest_language_handler( EXCEPTION_RECORD *rec, AMD64_CONTEXT *ctx,
                                         DISPATCHER_CONTEXT_AMD64 *dispatch,
                                         struct guest_unwind_target *target )
{
    ULONG_PTR handler = follow_guest_jmp_thunk( (ULONG_PTR)dispatch->LanguageHandler );

    if (!handler || !guest_module_from_address( (void *)handler ))
    {
        ERR( "guest frame %I64x at %I64x names language handler %p (%p after its jump "
             "thunks), which is in no guest image; entering it would execute host code "
             "as x86-64, so it is refused rather than run\n",
             dispatch->EstablisherFrame, dispatch->ControlPc,
             dispatch->LanguageHandler, (void *)handler );
        return ExceptionHandler_refused;
    }

    TRACE( "entering guest language handler %p: rec %p code %08x flags %08x frame %I64x "
           "ctx %p dispatch %p (ControlPc %I64x ImageBase %I64x TargetIp %I64x scope %u)\n",
           (void *)handler, rec, (UINT)rec->ExceptionCode, (UINT)rec->ExceptionFlags,
           dispatch->EstablisherFrame, ctx, dispatch, dispatch->ControlPc,
           dispatch->ImageBase, dispatch->TargetIp, (UINT)dispatch->ScopeIndex );

    return call_guest_handler_run( handler, rec, dispatch->EstablisherFrame,
                                   ctx, dispatch, target );
}

/***********************************************************************
 *           guest_c_specific_handler
 *
 * __C_specific_handler over a guest frame.  Structurally the same function as
 * the x86-64 one in unwind.c -- deliberately, so that the two can be diffed --
 * with two differences: the funclets are called through the emulator, and the
 * accepting scope is REPORTED rather than unwound to from in here.
 */
static UINT guest_c_specific_handler( EXCEPTION_RECORD *rec, AMD64_CONTEXT *ctx,
                                      DISPATCHER_CONTEXT_AMD64 *dispatch,
                                      struct guest_unwind_target *target )
{
    const SCOPE_TABLE_AMD64 *table = dispatch->HandlerData;
    ULONG64 base = dispatch->ImageBase;
    ULONG64 pc = dispatch->ControlPc;
    ULONG64 frame = dispatch->EstablisherFrame;
    UINT i;

    if (!table)
    {
        ERR( "guest frame %I64x at %I64x has __C_specific_handler and no scope table\n", frame, pc );
        return ExceptionContinueSearch;
    }
    TRACE( "frame %I64x pc %I64x base %I64x scopes %u\n", frame, pc, base, (UINT)table->Count );

    if (rec->ExceptionFlags & (EXCEPTION_UNWINDING | EXCEPTION_EXIT_UNWIND))
    {
        for (i = dispatch->ScopeIndex; i < table->Count; i++)
        {
            if (pc < base + table->ScopeRecord[i].BeginAddress) continue;
            if (pc >= base + table->ScopeRecord[i].EndAddress) continue;
            if (table->ScopeRecord[i].JumpTarget) continue;   /* an __except, not a __finally */

            if ((rec->ExceptionFlags & EXCEPTION_TARGET_UNWIND) &&
                dispatch->TargetIp >= base + table->ScopeRecord[i].BeginAddress &&
                dispatch->TargetIp < base + table->ScopeRecord[i].EndAddress)
                break;   /* the __try we are unwinding INTO; its __finally must not run */

            /* ScopeIndex advances so that a scope cannot run twice if this
             * handler is entered again for the same frame -- which is not a
             * hypothetical: it is exactly what a collided unwind does, and the
             * scope that collided must not be among the ones it re-runs. */
            dispatch->ScopeIndex = i + 1;
            call_guest_termination_handler( base + table->ScopeRecord[i].HandlerAddress, frame, TRUE );

            /* The funclet may not have RETURNED in the sense that matters.  A
             * __finally that calls RtlUnwindEx while this unwind is running it
             * is a COLLIDED UNWIND: on Windows that call does not return at
             * all, and here its emulator run was ended rather than returned
             * from (guest_request_unwind).  Everything after it in this scope
             * table belongs to the unwind that has just taken over, which will
             * re-enter this handler at the ScopeIndex just advanced -- or not,
             * as its own target decides.  Reported the way a guest handler's
             * own RtlUnwindEx is, because it is the same event arriving by a
             * different door. */
            if (guest_unwind_take_collision( target ))
            {
                TRACE( "frame %I64x scope %u: the __finally started a second unwind to "
                       "frame %I64x ip %I64x\n", frame, i, target->frame, target->target_ip );
                return ExceptionExecuteHandler_guest;
            }
        }
        return ExceptionContinueSearch;
    }

    for (i = dispatch->ScopeIndex; i < table->Count; i++)
    {
        if (pc < base + table->ScopeRecord[i].BeginAddress) continue;
        if (pc >= base + table->ScopeRecord[i].EndAddress) continue;
        if (!table->ScopeRecord[i].JumpTarget) continue;      /* a __finally, not an __except */

        /* HandlerAddress == EXCEPTION_EXECUTE_HANDLER (1) is the encoding for
         * "__except(1) -- no filter funclet to call, always take it". */
        if (table->ScopeRecord[i].HandlerAddress != EXCEPTION_EXECUTE_HANDLER)
        {
            /* Ranges, not the three named constants.  A filter is an arbitrary
             * C expression and its value is only DOCUMENTED as those three; a
             * program that returns 2 gets its handler on Windows, and a switch
             * on exact values would either drop it or need a fourth arm for a
             * case that is not an error. */
            LONG res = call_guest_filter( base + table->ScopeRecord[i].HandlerAddress,
                                          rec, ctx, frame );
            if (res < 0) return ExceptionContinueExecution;   /* CONTINUE_EXECUTION */
            if (!res) continue;                               /* CONTINUE_SEARCH */
        }
        /* Exactly the RtlUnwindEx call the x86-64 __C_specific_handler makes
         * at this point, expressed as the request the walk performs: the
         * accepting frame, its jump target, and the exception code as the
         * unwind's return value.  Reported rather than jumped to for the
         * reason in the block comment above; a guest handler that reaches the
         * same conclusion arrives here through guest_request_unwind() with the
         * same struct filled in, so the two paths converge before the unwind
         * rather than after it. */
        target->frame        = frame;
        target->target_ip    = base + table->ScopeRecord[i].JumpTarget;
        target->return_value = rec->ExceptionCode;
        target->rec          = *rec;
        TRACE( "scope %u accepted; unwinding to %I64x in frame %I64x\n",
               i, target->target_ip, target->frame );
        return ExceptionExecuteHandler_guest;
    }
    return ExceptionContinueSearch;
}

/***********************************************************************
 *           guest_virtual_unwind
 *
 * One frame of the guest stack: find its RUNTIME_FUNCTION, unwind the context
 * into its caller and report the frame's language handler.  The native
 * virtual_unwind() above this file's midpoint is the same shape for ppc64.
 */
static NTSTATUS guest_virtual_unwind( ULONG type, DISPATCHER_CONTEXT_AMD64 *dispatch,
                                      AMD64_CONTEXT *context )
{
    dispatch->ImageBase       = 0;
    dispatch->ScopeIndex      = 0;
    dispatch->ControlPc       = context->Rip;
    dispatch->LanguageHandler = NULL;
    dispatch->HandlerData     = NULL;
    dispatch->FunctionEntry   = RtlLookupFunctionEntry_amd64( context->Rip, &dispatch->ImageBase,
                                                              dispatch->HistoryTable );
    return RtlVirtualUnwind2_amd64( type, dispatch->ImageBase, context->Rip, dispatch->FunctionEntry,
                                    context, NULL, &dispatch->HandlerData, &dispatch->EstablisherFrame,
                                    NULL, NULL, NULL, &dispatch->LanguageHandler, 0 );
}

/***********************************************************************
 *           guest_frame_walk_can_continue
 *
 * Whether the walk has a next frame worth looking at.  A guest stack ends at
 * the emulator's own entry frame, whose return address is not in any guest
 * image -- that, and not a sentinel, is what "the bottom" looks like here.
 */
static BOOL guest_frame_walk_can_continue( const AMD64_CONTEXT *context, ULONG64 frame,
                                           void *stack_base, void *stack_limit, UINT depth )
{
    if (!frame) return FALSE;
    if (depth >= GUEST_SEH_MAX_FRAMES)
    {
        ERR( "guest frame walk exceeded %u frames at rip %I64x; stopping\n",
             GUEST_SEH_MAX_FRAMES, context->Rip );
        return FALSE;
    }
    if (!is_valid_guest_frame( frame, stack_base, stack_limit )) return FALSE;
    if (!context->Rip) return FALSE;
    if (!is_valid_guest_frame( context->Rsp, stack_base, stack_limit )) return FALSE;
    return TRUE;
}

/***********************************************************************
 *           guest_consolidate_callback
 *
 * The last step of a CONSOLIDATING unwind: the one MSVC uses to spell `catch`.
 *
 * A C++ handler does not unwind to a jump target.  __CxxFrameHandler calls
 * RtlUnwindEx with an EXCEPTION_RECORD whose code is STATUS_UNWIND_CONSOLIDATE
 * and whose ExceptionInformation[0] is a CONSOLIDATION ROUTINE; the unwind
 * itself is ordinary -- every __finally between the throw and the catching
 * frame runs -- but the resume is not.  Instead of resuming at TargetIp, the
 * routine is CALLED with a pointer to the record, and the address it RETURNS
 * is the real resume Rip.  That routine is what runs the catch block body
 * (__CxxCallCatchBlock in a static MSVC runtime, call_catch_block in Wine's
 * own msvcrt, dlls/msvcrt/except.c -- the same protocol from the other side).
 *
 * THE WHOLE RECORD TRAVELS, not just slot 0.  Wine's own __CxxFrameHandler
 * fills eleven ExceptionInformation slots: the establisher frame in [1], the
 * function descriptor in [2], the try level in [3], the untranslated record in
 * [4], the catch handler in [5], the original C++ EXCEPTION_RECORD in [6] and
 * its CONTEXT in [7].  The routine reads them all, so anything that arrived
 * here carrying fewer would run a catch block against a frame it invented.
 * struct guest_unwind_target copies the record by value, all fifteen slots and
 * the count with it, and this hands the routine a pointer to THAT copy -- guest
 * memory is host memory, so a native-stack record is an ordinary address to it.
 *
 * WHICH STACK IT RUNS ON, and why that is sound.  On Windows the routine runs
 * on the unwound stack: RtlRestoreContext restores the target context first and
 * calls the routine from a synthesised frame on top of it.  Here it runs, like
 * every other guest funclet this file enters, in a nested emulator run with a
 * guest stack of its own -- because the frames it would otherwise sit on top of
 * are on the FAULTING stack while the native frames of this walk are still live
 * below it.  That is sound for the same reason the filter and __finally
 * funclets are: a funclet addresses its parent's locals through the establisher
 * frame it is HANDED, never through the stack it is running on, and the
 * consolidation routine is handed that frame in ExceptionInformation[1].
 *
 * The one thing that costs: a `throw` from inside the catch block is a raise
 * inside the nested run, so its search stops at that run's entry frame rather
 * than continuing up the faulting stack.  A rethrow that the catch block's OWN
 * __try catches -- which is exactly what __CxxCallCatchBlock wraps itself in --
 * is inside the run and works; one that must escape the catch block does not.
 * Said here rather than discovered later.
 */
static NTSTATUS guest_consolidate_callback( EXCEPTION_RECORD *rec, AMD64_CONTEXT *result )
{
    ULONG_PTR callback;
    ULONG64 rip;

    if (rec->NumberParameters < 1 || !rec->ExceptionInformation[0])
    {
        ERR( "consolidating unwind with no consolidation routine: NumberParameters=%u "
             "info[0]=%p.  There is nothing to run on the unwound stack and no resume "
             "address to invent; refusing\n", (UINT)rec->NumberParameters,
             rec->NumberParameters ? (void *)rec->ExceptionInformation[0] : NULL );
        return STATUS_INVALID_PARAMETER;
    }

    /* Same classification as a language handler's, and for the same reason: the
     * routine is about to be ENTERED AS GUEST CODE, and a native address would
     * run ppc64 bytes as x86-64.  Through the import thunks first, because a
     * routine that lives in the image's static runtime may be named through
     * one. */
    callback = follow_guest_jmp_thunk( rec->ExceptionInformation[0] );
    if (!callback || !guest_module_from_address( (void *)callback ))
    {
        ERR( "consolidating unwind names consolidation routine %p (%p after its jump "
             "thunks), which is in no guest image; entering it would execute host code "
             "as x86-64, so it is refused rather than run\n",
             (void *)rec->ExceptionInformation[0], (void *)callback );
        return STATUS_NOT_IMPLEMENTED;
    }

    TRACE( "consolidating unwind: entering routine %p with rec %p (%u parameters, "
           "frame %p), resuming at rsp %I64x\n", (void *)callback, rec,
           (UINT)rec->NumberParameters,
           rec->NumberParameters > 1 ? (void *)rec->ExceptionInformation[1] : NULL,
           result->Rsp );

    rip = call_guest_function_args( (void *)callback, (ULONG_PTR)rec, 0, 0, 0 );
    if (!rip)
    {
        /* The routine's return value IS the resume address, so zero is not a
         * value it can legitimately produce -- it is what call_guest_function
         * hands back when the nested run failed without a pending exception.
         * Resuming at zero would turn that into a c0000005 several frames from
         * the cause. */
        ERR( "consolidating unwind: routine %p returned a null resume address; the catch "
             "block did not run to a resume point and there is nowhere to continue\n",
             (void *)callback );
        return STATUS_NOT_IMPLEMENTED;
    }
    result->Rip = rip;
    TRACE( "consolidating unwind: guest resumes at rip %I64x rsp %I64x rax %I64x\n",
           result->Rip, result->Rsp, result->Rax );
    return STATUS_SUCCESS;
}

/***********************************************************************
 *           collided_adopt
 *
 * Adopt the DISPATCHER_CONTEXT that a handler returning ExceptionCollidedUnwind
 * has just overwritten -- the one place that decides what believing it means,
 * so that the search phase and the unwind phase cannot come to differ about it.
 *
 * The contract, from the other side of it: ntdll's own RtlpUnwindHandler (Wine
 * spells it unwind_exception_handler, dlls/ntdll/signal_x86_64.c) copies the
 * OUTER unwind's ControlPc, ImageBase, FunctionEntry, EstablisherFrame,
 * LanguageHandler, HandlerData, HistoryTable, ScopeIndex and ContextRecord into
 * the dispatcher context of the walk that reached it, keeping only that walk's
 * own TargetIp, and returns this disposition.  So by the time this is called
 * the dispatch ALREADY describes where to resume; nothing here interprets it,
 * and in particular nothing re-derives it -- a fresh guest_virtual_unwind()
 * would reset ScopeIndex to zero and re-run the scope that collided.
 *
 * `context` is the caller's WALKING context, the one an iteration is a frame
 * ahead with.  It is reset to the adopted ContextRecord and stepped one frame
 * with UNW_FLAG_NHANDLER -- no handler runs for the collided frame here; it is
 * about to be re-entered by the caller's loop with the ScopeIndex it carries.
 *
 * *last_frame / *last_scope carry the previous adoption.  A dispatcher context
 * that comes back naming the same frame at the same scope has not advanced, and
 * a handler that hands one back will hand it back again: refused by name rather
 * than re-entered until something else breaks.
 *
 * -> FALSE when it was refused, having said why.
 */
static BOOL collided_adopt( DISPATCHER_CONTEXT_AMD64 *dispatch, AMD64_CONTEXT *context,
                            ULONG64 *last_frame, DWORD *last_scope )
{
    ULONG64 frame = 0;

    if (!dispatch->ContextRecord || !dispatch->FunctionEntry)
    {
        ERR( "a handler at frame %I64x returned ExceptionCollidedUnwind with %s in the "
             "dispatcher context it handed back; there is no unwind position in it to "
             "resume from\n", dispatch->EstablisherFrame,
             dispatch->ContextRecord ? "no function entry" : "no context record" );
        return FALSE;
    }
    if (dispatch->EstablisherFrame == *last_frame && dispatch->ScopeIndex == *last_scope)
    {
        ERR( "a handler at frame %I64x returned ExceptionCollidedUnwind twice running for "
             "the same frame at the same scope (%u): the dispatcher context it hands back "
             "does not advance, so adopting it again would re-enter the same handler "
             "forever\n", dispatch->EstablisherFrame, (UINT)dispatch->ScopeIndex );
        return FALSE;
    }
    TRACE( "collided unwind: adopting the dispatcher context a handler handed back -- "
           "controlpc %I64x frame %I64x scope %u handler %p\n", dispatch->ControlPc,
           dispatch->EstablisherFrame, (UINT)dispatch->ScopeIndex, dispatch->LanguageHandler );
    *last_frame = dispatch->EstablisherFrame;
    *last_scope = dispatch->ScopeIndex;
    *context = *dispatch->ContextRecord;
    RtlVirtualUnwind_amd64( UNW_FLAG_NHANDLER, dispatch->ImageBase, dispatch->ControlPc,
                            dispatch->FunctionEntry, context, NULL, &frame, NULL );
    return TRUE;
}

/***********************************************************************
 *           guest_unwind_to_target
 *
 * (See collided_adopt() just above for the disposition both phases share.)
 *
 * The unwind half: run every __finally between the exception and the accepting
 * frame, then hand back that frame's context with Rip at the __except body.
 * RtlUnwindEx's x86-64 loop, with its one-iteration lag between `result` and
 * `context` preserved -- that lag is what makes `result` come out holding the
 * TARGET frame's registers rather than its caller's.
 *
 * THE COLLIDED UNWIND, which is the one part of this loop that is not simply
 * x86-64 RtlUnwindEx's.
 *
 * A __finally that this loop is running may itself call RtlUnwindEx, naming a
 * frame further out than the one this unwind is heading for.  Two unwinds are
 * then live over the same stack, and the x64 contract settles it: the INNER
 * one wins and CONTINUES FROM THE COLLISION POINT.  The frames below it have
 * already been unwound and must not be unwound again; the scope that collided
 * has already run and must not run again; the frames between the collision and
 * the new target have not run and must run exactly once.
 *
 * Windows spells that with a frame: RtlUnwindEx runs each handler underneath
 * call_unwind_handler, whose own language handler (RtlpUnwindHandler, or
 * unwind_exception_handler in Wine's dlls/ntdll/signal_x86_64.c) the inner
 * unwind walks into, and which returns ExceptionCollidedUnwind after copying
 * the OUTER unwind's DISPATCHER_CONTEXT -- ControlPc, ImageBase,
 * FunctionEntry, EstablisherFrame, LanguageHandler, HandlerData, ScopeIndex
 * and the ContextRecord, everything but TargetIp -- into the inner one's.  The
 * inner RtlUnwindEx then steps that context one frame with UNW_FLAG_NHANDLER
 * and resumes its loop at unwind_done, i.e. with the collided frame's handler
 * about to be re-entered at the ScopeIndex the outer unwind had reached.
 *
 * There is no such frame to walk into here.  This loop is native code and the
 * funclet runs in a nested emulator run whose own stack the inner unwind never
 * touches, so the inner unwind cannot discover the outer one by walking -- it
 * is TOLD, by guest_request_unwind(), which recognises an RtlUnwindEx issued
 * from a funclet of this unwind naming a frame on the stack this unwind is
 * tearing down.  What arrives here is therefore not a disposition to decode
 * but the state Windows would have had to reconstruct: this iteration's
 * dispatch already IS the outer unwind's dispatcher context at the colliding
 * frame, `result` already holds that frame's registers and `context` already
 * holds its caller's.  So the adoption is a `goto` and a new target, and the
 * one thing it must not do is re-derive the dispatch -- guest_virtual_unwind()
 * resets ScopeIndex to zero, and a collided unwind that re-enters the colliding
 * frame's handler at scope zero runs the __finally that collided a second time.
 *
 * A handler that RETURNS ExceptionCollidedUnwind is served the same way, and
 * that is the shape a guest which implements its own unwinder produces: by the
 * contract it has overwritten the DISPATCHER_CONTEXT it was handed with the
 * state of the unwind it collided with, so the adoption is to believe it --
 * again without re-deriving anything -- after stepping the walking context past
 * the frame it now names.  What is refused, by name, is a collision that makes
 * NO PROGRESS: the same frame at the same ScopeIndex colliding twice running is
 * an unwinder that will collide forever, and a bounded spin that re-runs its
 * handlers is worse than a diagnosis.
 */
static NTSTATUS guest_unwind_to_target( const struct guest_unwind_target *target,
                                        const AMD64_CONTEXT *from,
                                        void *stack_base, void *stack_limit,
                                        AMD64_CONTEXT *result )
{
    UNWIND_HISTORY_TABLE table = { 0 };
    DISPATCHER_CONTEXT_AMD64 dispatch = { 0 };
    struct guest_unwind_state unwind = { guest_unwind_state, guest_run_depth + 1,
                                         stack_base, stack_limit,
                                         target->frame, target->target_ip };
    /* The target BY VALUE, because a collided unwind replaces it: from the
     * collision on, this loop is performing the inner unwind and everything
     * below -- the frame it stops at, the record its handlers see, the resume
     * address and the value in RAX -- is the inner one's. */
    struct guest_unwind_target want = *target;
    EXCEPTION_RECORD unwind_rec = want.rec;
    AMD64_CONTEXT context = *from;
    NTSTATUS status = STATUS_SUCCESS;
    ULONG64 collided_frame = 0;
    DWORD collided_scope = 0;
    UINT depth = 0;

    *result = *from;
    unwind_rec.ExceptionFlags |= EXCEPTION_UNWINDING;

    dispatch.EstablisherFrame = from->Rsp;
    dispatch.TargetIp         = want.target_ip;
    dispatch.ContextRecord    = result;
    dispatch.HistoryTable     = &table;

    guest_unwind_state = &unwind;
    for (;;)
    {
        struct guest_unwind_target again = { 0 };
        UINT res;

        if ((status = guest_virtual_unwind( UNW_FLAG_UHANDLER, &dispatch, &context )))
        {
            ERR( "guest unwind failed at rip %I64x: %08x\n", context.Rip, (UINT)status );
            break;
        }
    collided:
        if (!dispatch.EstablisherFrame || depth++ >= GUEST_SEH_MAX_FRAMES ||
            !is_valid_guest_frame( dispatch.EstablisherFrame, stack_base, stack_limit ))
        {
            ERR( "guest unwind ran off the stack at frame %I64x before reaching target "
                 "frame %I64x\n", dispatch.EstablisherFrame, want.frame );
            status = STATUS_BAD_STACK;
            break;
        }
        if (dispatch.EstablisherFrame > want.frame)
        {
            ERR( "guest unwind passed target frame %I64x (now at %I64x)\n",
                 want.frame, dispatch.EstablisherFrame );
            status = STATUS_INVALID_UNWIND_TARGET;
            break;
        }
        /* Recomputed rather than only ever set, because the target can change
         * under this loop: the frame that was the target of the unwind a
         * collision interrupted is an ORDINARY frame to the unwind that took
         * over, and its __finally blocks are owed to it. */
        if (dispatch.EstablisherFrame == want.frame)
            unwind_rec.ExceptionFlags |= EXCEPTION_TARGET_UNWIND;
        else
            unwind_rec.ExceptionFlags &= ~EXCEPTION_TARGET_UNWIND;

        if (dispatch.LanguageHandler)
        {
            /* Same two paths as the search phase and the same DISPATCHER_CONTEXT:
             * the guest ntdll's own export served natively, anything else
             * entered as guest code, which is what runs a __finally belonging
             * to an image with a static MSVC runtime. */
            if (guest_language_handler_is_c_specific( dispatch.LanguageHandler ))
                res = guest_c_specific_handler( &unwind_rec, result, &dispatch, &again );
            else
                res = call_guest_language_handler( &unwind_rec, result, &dispatch, &again );

            switch (res)
            {
            case ExceptionContinueSearch:
                unwind_rec.ExceptionFlags &= ~EXCEPTION_COLLIDED_UNWIND;
                break;
            case ExceptionExecuteHandler_guest:
                /* THE COLLISION.  Both of this port's roads into a second
                 * unwind arrive here with `again` filled in -- a guest language
                 * handler that called RtlUnwindEx from inside its own run
                 * (call_guest_language_handler), and a __finally funclet that
                 * did (guest_c_specific_handler, told by guest_request_unwind)
                 * -- because the difference between them is which door the
                 * request came in by, not what it asks for. */
                if (again.frame < dispatch.EstablisherFrame)
                {
                    /* Below the collision is the part of the stack this unwind
                     * has already torn down: those frames' __finally blocks
                     * have run and their registers are gone. */
                    ERR( "guest frame %I64x handler %p started a second unwind to frame "
                         "%I64x, which the unwind it collided with has already passed "
                         "(now at %I64x); there is no such frame left to resume in\n",
                         dispatch.EstablisherFrame, dispatch.LanguageHandler,
                         again.frame, dispatch.EstablisherFrame );
                    status = STATUS_INVALID_UNWIND_TARGET;
                    goto done;
                }
                if (dispatch.EstablisherFrame == collided_frame &&
                    dispatch.ScopeIndex == collided_scope)
                {
                    ERR( "guest frame %I64x handler %p collided a second time at the same "
                         "scope (%u) with the unwind that had already adopted its first "
                         "collision; an unwinder that cannot advance past its own "
                         "collision would collide forever, so this one is refused\n",
                         dispatch.EstablisherFrame, dispatch.LanguageHandler,
                         (UINT)dispatch.ScopeIndex );
                    status = STATUS_NOT_IMPLEMENTED;
                    goto done;
                }
                TRACE( "collided unwind: frame %I64x handler %p started a second unwind "
                       "to frame %I64x ip %I64x from inside the unwind to frame %I64x ip "
                       "%I64x; adopting it at scope %u\n", dispatch.EstablisherFrame,
                       dispatch.LanguageHandler, again.frame, again.target_ip, want.frame,
                       want.target_ip, (UINT)dispatch.ScopeIndex );
                collided_frame = dispatch.EstablisherFrame;
                collided_scope = dispatch.ScopeIndex;
                want              = again;
                unwind.frame      = want.frame;
                unwind.target_ip  = want.target_ip;
                unwind_rec        = want.rec;
                unwind_rec.ExceptionFlags |= EXCEPTION_UNWINDING | EXCEPTION_COLLIDED_UNWIND;
                dispatch.TargetIp = want.target_ip;
                /* Not `continue`: the dispatch describing the colliding frame
                 * is the state to resume from, ScopeIndex included, and the
                 * top of the loop would derive a fresh one. */
                goto collided;
            case ExceptionCollidedUnwind:
                /* The other shape of the same event: a handler that says, in
                 * the disposition rather than by unwinding, that this frame
                 * belongs to an unwind already in progress -- and that has
                 * overwritten the DISPATCHER_CONTEXT with that unwind's state.
                 * Believed rather than decoded; see collided_adopt(). */
                if (!collided_adopt( &dispatch, &context, &collided_frame, &collided_scope ))
                {
                    status = STATUS_NOT_IMPLEMENTED;
                    goto done;
                }
                unwind_rec.ExceptionFlags |= EXCEPTION_COLLIDED_UNWIND;
                goto collided;
            case ExceptionHandler_refused:
                status = STATUS_NOT_IMPLEMENTED;
                goto done;
            default:
                ERR( "guest frame %I64x handler %p returned disposition %u during an "
                     "unwind; only ExceptionContinueSearch is legal there\n",
                     dispatch.EstablisherFrame, dispatch.LanguageHandler, res );
                status = STATUS_INVALID_DISPOSITION;
                goto done;
            }
        }

        if (dispatch.EstablisherFrame == want.frame) break;
        *result = context;
    }
done:
    /* Unlinked before the consolidation routine below runs, and that ordering
     * is the contract rather than tidiness: by then the unwind IS complete --
     * every __finally has run -- and the catch block the routine executes is
     * ordinary guest code that may legitimately raise, and may legitimately
     * unwind. */
    guest_unwind_state = unwind.prev;

    if (status) return status;

    /* RAX carries the unwind's ReturnValue on arrival whichever kind of unwind
     * this was: x86-64 RtlUnwindEx writes context->Rax before RtlRestoreContext
     * and does not make that conditional on the exception code. */
    result->Rax = want.return_value;

    if (unwind_rec.ExceptionCode == (DWORD)STATUS_UNWIND_CONSOLIDATE)
        return guest_consolidate_callback( &unwind_rec, result );

    result->Rip = want.target_ip;
    TRACE( "guest resumes at rip %I64x rsp %I64x rax %I64x\n",
           result->Rip, result->Rsp, result->Rax );
    return STATUS_SUCCESS;
}

/***********************************************************************
 *           guest_unwind_in_place / guest_request_unwind
 *
 * The two ways an unwind request gets performed, and the one place that
 * decides which -- so that "who owns this unwind" is answered once.
 *
 * IN PLACE is guest code unwinding inside its own emulator run: the frames
 * between the caller of RtlUnwindEx and the target are all in that run, so the
 * walk starts from the caller's own context and the result is written straight
 * into the CONTEXT the trap fired with.  The guest resumes at TargetIp exactly
 * as it resumes in an __except body, through the mechanism dispatch_guest_raise
 * already uses for that.
 *
 * DEFERRED is a guest language handler that this dispatch entered: the frames
 * it means are on the FAULTING stack, not on the stack of the run it is using,
 * and the run has live native frames of ours below it.  Jumping would abandon
 * those.  So the request is recorded in the handler's own record and the run is
 * ended; call_guest_language_handler() picks it up and hands it to the walk,
 * which unwinds the faulting stack -- the same thing the identity path does by
 * returning ExceptionExecuteHandler_guest, reached by a different road.
 */
static NTSTATUS guest_unwind_in_place( const struct guest_unwind_target *target )
{
    struct emu_guest_stack_params stack = { 0 };
    AMD64_CONTEXT *ctx = emu_current_trap_ctx;
    AMD64_CONTEXT from, result;
    NTSTATUS status;

    if (!ctx)
    {
        ERR( "no trap context for a guest unwind to frame %I64x ip %I64x\n",
             target->frame, target->target_ip );
        return STATUS_NOT_IMPLEMENTED;
    }
    WINE_UNIX_CALL( unix_emu_guest_stack, &stack );

    /* The frame the walk must start at is the CALLER of the entry point that
     * asked to unwind, which is where x86-64 RtlUnwindEx's first virtual
     * unwind lands too: the trap fired with Rip on the trap stub and Rsp on
     * the return address that guest's CALL pushed. */
    from = *ctx;
    from.Rip  = *(DWORD64 *)(ULONG_PTR)from.Rsp;
    from.Rsp += 8;

    TRACE( "unwinding in place from rip %I64x rsp %I64x to frame %I64x ip %I64x\n",
           from.Rip, from.Rsp, target->frame, target->target_ip );
    if ((status = guest_unwind_to_target( target, &from, stack.base, stack.limit, &result )))
        return status;

    *ctx = result;
    /* there is no call to return from: every register is already the one the
     * guest must resume with (see emu_trap_dispatch) */
    emu_trap_ctx_rewritten = TRUE;
    return STATUS_SUCCESS;
}

/* Whether a frame lies on the stack a given unwind is tearing down.  Strictly
 * that stack: is_valid_guest_frame() also accepts the Win32 stack, which is the
 * right answer to "could this be a frame" and the wrong one here, where the
 * whole question is WHICH of two live stacks a frame belongs to. */
static BOOL guest_frame_on_unwound_stack( ULONG64 frame, const struct guest_unwind_state *unwind )
{
    return unwind->stack_limit && (void *)(ULONG_PTR)frame > unwind->stack_limit &&
           (void *)(ULONG_PTR)frame <= unwind->stack_base;
}

static NTSTATUS guest_request_unwind( const struct guest_unwind_target *target )
{
    struct guest_unwind_state *unwind;

    if (guest_handler_call && guest_handler_call->run_depth == guest_run_depth)
    {
        guest_handler_call->target  = *target;
        guest_handler_call->unwound = TRUE;
        guest_unwind_run_end = TRUE;   /* emu_trap_dispatch ends the run on this */
        TRACE( "deferring the unwind to frame %I64x ip %I64x to the frame walk\n",
               target->frame, target->target_ip );
        return STATUS_SUCCESS;
    }

    /* THE THIRD ROAD: a __finally funclet that an unwind is running has called
     * RtlUnwindEx itself, and the frame it names is on the stack that unwind is
     * tearing down rather than on the funclet's own run stack.  That is a
     * COLLIDED UNWIND -- two unwinds live over one stack -- and it cannot be
     * served in place for the reason no unwind across this boundary can be:
     * the frames it means are not the ones this run is standing on.  So it is
     * recorded for the unwind that is running the funclet, exactly as a
     * language handler's is recorded for the walk that entered it, and the
     * funclet's run is ended.  guest_c_specific_handler() picks it up and
     * reports it; guest_unwind_to_target() adopts it.
     *
     * The stack, not merely "an unwind is in progress", is what decides: a
     * funclet is ordinary guest code and may perfectly well unwind WITHIN its
     * own run -- a __try nested inside the __finally, unwound by a __leave --
     * and that is not a collision and must still be served in place. */
    if ((unwind = guest_unwind_current()) && guest_frame_on_unwound_stack( target->frame, unwind ))
    {
        if (guest_run_depth != unwind->funclet_depth)
        {
            /* Ending a run ends the INNERMOST one, so a request from further
             * down cannot be reported this way without leaving the runs between
             * here and the funclet still executing. */
            ERR( "guest code %u emulator runs below the __finally of the unwind to frame "
                 "%I64x called RtlUnwindEx for frame %I64x ip %I64x on the stack that "
                 "unwind is tearing down; only the funclet's own run can be ended, so "
                 "this collided unwind cannot be adopted\n",
                 guest_run_depth - unwind->funclet_depth, unwind->frame,
                 target->frame, target->target_ip );
            return STATUS_NOT_IMPLEMENTED;
        }
        unwind->again    = *target;
        unwind->collided = TRUE;
        guest_unwind_run_end = TRUE;   /* emu_trap_dispatch ends the run on this */
        TRACE( "collided unwind: a __finally of the unwind to frame %I64x ip %I64x started "
               "a second unwind to frame %I64x ip %I64x; handing it to that unwind\n",
               unwind->frame, unwind->target_ip, target->frame, target->target_ip );
        return STATUS_SUCCESS;
    }
    return guest_unwind_in_place( target );
}

/***********************************************************************
 *           guest_stack_is_readable
 *
 * Whether the return address at a guest stack pointer can actually be READ, as
 * opposed to merely lying between two addresses -- which is all
 * is_valid_guest_frame() can say, and not enough for the one dereference that
 * happens before any check of this file's can intervene.
 *
 * The frame walk's first step unwinds the FAULTING frame, and for a leaf that
 * means loading eight bytes from Rsp.  That load is made by native code, in the
 * dispatcher, so a guest that faulted with a wild or hijacked stack pointer --
 * which is what a poisoned thread entry looks like, and what anti-debugging
 * code produces on purpose -- would have it fault natively: not a guest
 * exception at all, but a crash inside the machinery whose whole job is to
 * report guest ones.  Windows answers such a thread through the unhandled path,
 * and so does this, having said which register was wrong.
 *
 * One query per dispatch, on a path that is already committing to nested
 * emulator runs, so the cost is not worth avoiding by guessing.
 */
static BOOL guest_stack_is_readable( ULONG64 rsp )
{
    MEMORY_BASIC_INFORMATION mbi;
    SIZE_T len = 0;

    if (!rsp) return FALSE;
    if (NtQueryVirtualMemory( GetCurrentProcess(), (void *)(ULONG_PTR)rsp, MemoryBasicInformation,
                              &mbi, sizeof(mbi), &len ))
        return FALSE;
    if (mbi.State != MEM_COMMIT) return FALSE;
    /* PAGE_GUARD counts as unreadable here deliberately: a stack pointer on the
     * guard page is a stack overflow, and taking that fault natively inside the
     * dispatcher would grow the wrong stack. */
    if (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD)) return FALSE;
    /* ...and the eight bytes must not run off the end of the region */
    return (ULONG64)(ULONG_PTR)mbi.BaseAddress + mbi.RegionSize >= rsp + sizeof(ULONG64);
}

/***********************************************************************
 *           dispatch_guest_frames
 *
 * The frame walk proper: RtlDispatchException over a guest stack.  Returns
 * STATUS_SUCCESS with *ctx edited when the guest is to resume -- either
 * because a filter said continue-execution, or because an __except accepted
 * and *ctx now describes its frame.
 */
static NTSTATUS dispatch_guest_frames( EXCEPTION_RECORD *rec, AMD64_CONTEXT *ctx,
                                       void *stack_base, void *stack_limit,
                                       BOOL *ctx_rewritten )
{
    EXCEPTION_REGISTRATION_RECORD *teb_frame = NtCurrentTeb()->Tib.ExceptionList;
    UNWIND_HISTORY_TABLE table = { 0 };
    DISPATCHER_CONTEXT_AMD64 dispatch = { 0 };
    AMD64_CONTEXT context = *ctx;
    NTSTATUS status;
    ULONG64 collided_frame = 0;
    DWORD collided_scope = 0;
    UINT depth = 0;
    DWORD res;

    dispatch.TargetIp      = 0;
    dispatch.ContextRecord = &context;
    dispatch.HistoryTable  = &table;

    /* Before the first step, not after it: see guest_stack_is_readable(). */
    if (!guest_stack_is_readable( ctx->Rsp ))
    {
        ERR( "guest exception %08x at %p with an unreadable stack pointer (rsp %I64x, "
             "guest stack %p-%p): the walk's first step would load this frame's return "
             "address from it, natively, and fault inside the dispatcher.  Reported as "
             "unhandled, which is what such a thread gets on Windows too\n",
             (UINT)rec->ExceptionCode, rec->ExceptionAddress, ctx->Rsp, stack_limit, stack_base );
        rec->ExceptionFlags |= EXCEPTION_STACK_INVALID;
        return STATUS_UNHANDLED_EXCEPTION;
    }

    for (;;)
    {
        struct guest_unwind_target target = { 0 };

        if ((status = guest_virtual_unwind( UNW_FLAG_EHANDLER, &dispatch, &context )))
        {
            ERR( "guest frame walk failed at rip %I64x: %08x\n", context.Rip, (UINT)status );
            return status;
        }
    collided:
        if (!dispatch.EstablisherFrame) break;
        if (!is_valid_guest_frame( dispatch.EstablisherFrame, stack_base, stack_limit ))
        {
            ERR( "invalid guest frame %I64x (win32 %p-%p, guest %p-%p)\n",
                 dispatch.EstablisherFrame, NtCurrentTeb()->Tib.StackLimit,
                 NtCurrentTeb()->Tib.StackBase, stack_limit, stack_base );
            rec->ExceptionFlags |= EXCEPTION_STACK_INVALID;
            break;
        }

        if (dispatch.LanguageHandler)
        {
            /* Two paths, one DISPATCHER_CONTEXT.  The identity check is against
             * the guest ntdll's export because that is the one
             * __C_specific_handler whose semantics this port can PROVE, and for
             * that address a native implementation is simply cheaper than a
             * round trip through the emulator.  Every other handler is entered
             * as guest code, because nothing in a PE says what it is: an
             * MSVC-linked image never names ntdll's, the static vcruntime
             * carries its OWN copy of __C_specific_handler (and of
             * __GSHandlerCheck, and of __CxxFrameHandler*) in .text and the
             * .xdata names that.  Measured on both of DOOM (2016)'s: the
             * handler at DOOMx64vk.exe+0x1eab2c8 reads DispatcherContext->
             * HandlerData as a SCOPE_TABLE and iterates it from ScopeIndex,
             * i.e. it IS __C_specific_handler, byte for byte a different one;
             * steam_api64.dll+0xed68 is __GSHandlerCheck, which only validates
             * the frame's cookie and continues the search.  The three take
             * incompatible handler data, so guessing from resemblance would
             * mean running an arbitrary address as a filter funclet -- and
             * guessing is unnecessary, because the handler itself knows. */
            if (guest_language_handler_is_c_specific( dispatch.LanguageHandler ))
                res = guest_c_specific_handler( rec, ctx, &dispatch, &target );
            else
                res = call_guest_language_handler( rec, ctx, &dispatch, &target );

            /* what a handler is allowed to have left behind in the record */
            rec->ExceptionFlags &= EXCEPTION_NONCONTINUABLE;

            switch (res)
            {
            case ExceptionContinueExecution:
                if (rec->ExceptionFlags & EXCEPTION_NONCONTINUABLE) return STATUS_NONCONTINUABLE_EXCEPTION;
                return STATUS_SUCCESS;
            case ExceptionContinueSearch:
                break;
            case ExceptionNestedException:
                /* an exception was raised inside this handler and handled
                 * there; Windows records it in the flags and keeps searching */
                rec->ExceptionFlags |= EXCEPTION_NESTED_CALL;
                break;
            case ExceptionExecuteHandler_guest:
                if ((status = guest_unwind_to_target( &target, ctx, stack_base,
                                                      stack_limit, &context )))
                    return status;
                *ctx = context;
                *ctx_rewritten = TRUE;
                return STATUS_SUCCESS;
            case ExceptionCollidedUnwind:
                /* "This frame belongs to an unwind that is already in progress,
                 * and the DISPATCHER_CONTEXT I was handed now describes THAT
                 * unwind's position" -- the protocol ntdll's own
                 * RtlpUnwindHandler speaks, and the one a guest that implements
                 * its own unwinder can speak too.  Served here exactly as
                 * x86-64 call_seh_handlers serves it: step the walking context
                 * one frame past what the dispatch now names, with
                 * UNW_FLAG_NHANDLER so that no handler runs for it, and resume
                 * the walk WITHOUT re-deriving the dispatch -- the ScopeIndex it
                 * carries is the whole point of the exercise. */
                if (!collided_adopt( &dispatch, &context, &collided_frame, &collided_scope ))
                    return STATUS_NOT_IMPLEMENTED;
                goto collided;
            case ExceptionHandler_refused:
                return STATUS_NOT_IMPLEMENTED;
            default:
                ERR( "guest frame %I64x at %I64x handler %p returned disposition %u, "
                     "which is not one this port speaks\n", dispatch.EstablisherFrame,
                     dispatch.ControlPc, dispatch.LanguageHandler, res );
                return STATUS_INVALID_DISPOSITION;
            }
        }
        /* The Wine TEB-frame hack, in the one place x86-64 dispatch has room
         * for it: a frame with no language handler.  Restricted to frames on
         * the GUEST stack, because comparing a Win32-stack address against a
         * guest RSP is comparing addresses in two unrelated mappings -- and a
         * frame the guest itself pushed is on the guest stack by construction.
         * Native frames below get their own shot, with their own machine's
         * context, when the run ends. */
        else while (stack_limit && (void *)teb_frame > stack_limit &&
                    (void *)teb_frame <= stack_base && (ULONG64)teb_frame < context.Rsp)
        {
            if (!guest_module_from_address( teb_frame->Handler ))
            {
                void *image;
                if (RtlPcToFileHeader( teb_frame->Handler, &image ))
                {
                    TRACE( "frame %p handler %p is native code, stopping the guest walk\n",
                           teb_frame, teb_frame->Handler );
                    return STATUS_UNHANDLED_EXCEPTION;
                }
                /* the thread-start rule, verbatim: refuse loudly, never guess */
                ERR( "frame %p handler %p is in no image at all, refusing\n",
                     teb_frame, teb_frame->Handler );
                return STATUS_UNHANDLED_EXCEPTION;
            }
            TRACE( "calling guest TEB handler %p (rec=%p frame=%p ctx=%p)\n",
                   teb_frame->Handler, rec, teb_frame, ctx );
            /* Through the same primitive as a language handler, and for the
             * same reason: a TEB-chain handler that CATCHES does not return
             * either -- it calls RtlUnwind -- and that unwind belongs to this
             * walk's stack, not to the run the handler is executing in. */
            res = call_guest_handler_run( (ULONG_PTR)teb_frame->Handler, rec,
                                          (ULONG64)(ULONG_PTR)teb_frame, ctx, &dispatch, &target );
            switch (res)
            {
            case ExceptionContinueExecution:
                if (rec->ExceptionFlags & EXCEPTION_NONCONTINUABLE) return STATUS_NONCONTINUABLE_EXCEPTION;
                return STATUS_SUCCESS;
            case ExceptionContinueSearch:
                break;
            case ExceptionExecuteHandler_guest:
                if ((status = guest_unwind_to_target( &target, ctx, stack_base,
                                                      stack_limit, &context )))
                    return status;
                *ctx = context;
                *ctx_rewritten = TRUE;
                return STATUS_SUCCESS;
            case ExceptionCollidedUnwind:
                /* The same adoption the language-handler arm above makes, in
                 * the same order x86-64 call_seh_handlers makes it: this frame
                 * is done with either way, so it is left behind before the walk
                 * resumes at the position the handler handed back. */
                teb_frame = teb_frame->Prev;
                if (!collided_adopt( &dispatch, &context, &collided_frame, &collided_scope ))
                    return STATUS_NOT_IMPLEMENTED;
                goto collided;
            default:
                ERR( "guest TEB handler %p returned disposition %u; refusing\n",
                     teb_frame->Handler, (UINT)res );
                return STATUS_INVALID_DISPOSITION;
            }
            teb_frame = teb_frame->Prev;
        }

        if (!guest_frame_walk_can_continue( &context, dispatch.EstablisherFrame,
                                            stack_base, stack_limit, depth++ ))
            break;
        /* Once one frame has been unwound, a return address that is in no guest
         * image means the walk has reached the emulator's entry frame.  Checked
         * only from here on: the FAULTING pc may legitimately be outside any
         * image (a call through a null or wild pointer), and that frame's
         * caller is exactly the one that must still get its chance. */
        if (!guest_module_from_address( (void *)(ULONG_PTR)context.Rip ))
        {
            TRACE( "rip %I64x is in no guest image; the guest stack ends here\n", context.Rip );
            break;
        }
    }
    return STATUS_UNHANDLED_EXCEPTION;
}


/***********************************************************************
 *           dispatch_guest_exception
 *
 * Guest-level dispatch, in the order Windows uses on x86-64:
 *
 *   1. the guest's vectored handlers, first chance;
 *   2. the .pdata frame walk -- RtlDispatchException over the guest stack.
 *
 * That ORDER is a change from what this file did first, and the reason is
 * that the two mechanisms are not peers.  Vectored handlers are process-wide
 * and registered at runtime, and Windows gives them the first look.  The
 * frame walk is where a compiled __try/__except actually lives on x86-64:
 * nothing is registered for it at all, so it cannot be found by walking a
 * list.  The TEB registration chain, which used to run second here, is not
 * how x64 SEH works for compiled frames -- there is no fs:[0]/gs:[0] chain on
 * this architecture, and no MSVC-compiled x86-64 function pushes one.  It is
 * kept, because a guest that DOES push one is still owed its handler, but it
 * is folded into the frame walk in the same place Wine's own native dispatch
 * puts it: consulted only for a frame that has no language handler.  So the
 * change is a subordination rather than a removal, and a guest that never
 * pushes a TEB frame -- which is every MSVC-compiled binary -- sees exactly
 * the mechanism its compiler emitted for.
 *
 * STATUS_SUCCESS means the guest is to resume with the CONTEXT.  *ctx_rewritten
 * distinguishes the two ways that happens, and the distinction is load-bearing
 * on the RAISE path: "a handler said continue-execution" means resume where the
 * exception left off, which for a raise is after the call that raised, so the
 * caller still owes the context its ordinary return fixup; "an __except
 * accepted" means the context ALREADY describes another frame entirely and any
 * further edit to it would undo the unwind.  Anything but STATUS_SUCCESS is the
 * unhandled protocol.
 */
static NTSTATUS dispatch_guest_exception( EXCEPTION_RECORD *rec, AMD64_CONTEXT *ctx,
                                          void *stack_base, void *stack_limit,
                                          BOOL *ctx_rewritten )
{
    struct guest_unwind_state *unwind;
    NTSTATUS status;
    UINT i;

    *ctx_rewritten = FALSE;

    TRACE( "code=%x flags=%x addr=%p rip=%p rsp=%p params=%u\n",
           (UINT)rec->ExceptionCode, (UINT)rec->ExceptionFlags, rec->ExceptionAddress,
           (void *)(ULONG_PTR)ctx->Rip, (void *)(ULONG_PTR)ctx->Rsp, (UINT)rec->NumberParameters );
    for (i = 0; i < rec->NumberParameters; i++)
        TRACE( " info[%u]=%p\n", i, (void *)rec->ExceptionInformation[i] );

    /* A call through a wild function pointer, named at the call site.
     *
     * An EXECUTE fault at a pc that is in no guest image is that and nothing
     * else, and the pc alone says nothing a user can act on -- DOOM (2016)
     * reaches 0xdead0017, a value that appears in none of its images and is
     * therefore computed, and the whole question is which of its call sites
     * computed it.  The CALL that got there pushed its return address, so the
     * caller's pc is on top of the guest stack; the frame walk below unwinds
     * that frame away as a leaf and never mentions it again, so it is printed
     * HERE or not at all.
     *
     * An ERR rather than a TRACE because reaching it means the process is
     * almost certainly about to die, and it is the one line that says why. */
    if (rec->NumberParameters >= 2 && rec->ExceptionInformation[0] == EXCEPTION_EXECUTE_FAULT &&
        !guest_module_from_address( (void *)(ULONG_PTR)ctx->Rip ))
    {
        LDR_DATA_TABLE_ENTRY *mod = NULL;
        ULONG64 ret = 0;

        if (is_valid_guest_frame( ctx->Rsp, stack_base, stack_limit ))
        {
            ret = *(ULONG64 *)(ULONG_PTR)ctx->Rsp;
            mod = guest_module_entry_from_address( (void *)(ULONG_PTR)ret );
        }
        if (mod)
            ERR( "guest called through a wild pointer: %p is in no guest image; the call "
                 "was made from %p = %s+%I64x\n", (void *)(ULONG_PTR)ctx->Rip,
                 (void *)(ULONG_PTR)ret, debugstr_w(mod->BaseDllName.Buffer),
                 ret - (ULONG64)(ULONG_PTR)mod->DllBase );
        else
            ERR( "guest called through a wild pointer: %p is in no guest image, and the "
                 "return address on its stack (%p) is in none either\n",
                 (void *)(ULONG_PTR)ctx->Rip, (void *)(ULONG_PTR)ret );
    }

    /* AN EXCEPTION TAKEN WHILE THIS THREAD IS ALREADY TEARING A GUEST STACK
     * DOWN.  A __finally is ordinary guest code and may fault, and Windows
     * dispatches that fault like any other: the search runs from the funclet
     * outwards, and if it accepts anywhere it unwinds -- across the unwind
     * already in progress, which is the collided unwind this file now
     * implements (guest_unwind_to_target, guest_request_unwind).
     *
     * What makes it safe to dispatch HERE, rather than the refusal this used to
     * be, is where the funclet is running: every funclet is entered in a nested
     * emulator run, and every run gets a guest stack of its own from
     * unix_emu_run_entry.  So the walk below is over the FUNCLET's stack, ends
     * at that run's entry frame, and cannot reach -- let alone re-run the
     * handlers of -- the frames the unwind has already torn down.  A __try
     * inside a __finally therefore works, which is legal and was refused; and a
     * funclet that unwinds out past its own run does not come through here at
     * all, it comes through guest_request_unwind() as the collision it is.
     *
     * The refusal that is left is the case the paragraph above rules out: a
     * fault on the very stack being unwound, where a second walk really would
     * re-enter frames whose __finally blocks have run.  It should not be
     * reachable -- no guest code executes on that stack between the start of an
     * unwind and its resume -- and it is named rather than assumed away. */
    if ((unwind = guest_unwind_current()))
    {
        if (stack_base && stack_base == unwind->stack_base)
        {
            ERR( "guest exception %08x at %p on the very stack the unwind to frame %I64x "
                 "ip %I64x is tearing down; a second walk over it would re-enter frames "
                 "whose __finally blocks have already run, so it is refused\n",
                 (UINT)rec->ExceptionCode, rec->ExceptionAddress, unwind->frame,
                 unwind->target_ip );
            return STATUS_NOT_IMPLEMENTED;
        }
        TRACE( "guest exception %08x at %p inside a funclet of the unwind to frame %I64x "
               "ip %I64x; dispatching it over that funclet's own run stack (%p-%p)\n",
               (UINT)rec->ExceptionCode, rec->ExceptionAddress, unwind->frame,
               unwind->target_ip, stack_limit, stack_base );
    }
    if (guest_seh_depth >= GUEST_SEH_MAX_DEPTH)
    {
        ERR( "guest exception %08x at %p nested %u deep; refusing to dispatch further\n",
             (UINT)rec->ExceptionCode, rec->ExceptionAddress, guest_seh_depth );
        return STATUS_NOT_IMPLEMENTED;
    }
    guest_seh_depth++;

    for (i = 0; i < guest_veh_count; i++)
    {
        struct guest_exception_pointers ptrs = { rec, ctx };
        void *handler = guest_veh[i];
        LONG res;

        TRACE( "calling guest vectored handler %p\n", handler );
        res = (LONG)(DWORD)call_guest_function_args( handler, (ULONG_PTR)&ptrs, 0, 0, 0 );
        TRACE( "guest vectored handler %p returned %x\n", handler, (UINT)res );
        if (res == EXCEPTION_CONTINUE_EXECUTION)
        {
            guest_seh_depth--;
            return STATUS_SUCCESS;
        }
    }

    status = dispatch_guest_frames( rec, ctx, stack_base, stack_limit, ctx_rewritten );
    guest_seh_depth--;
    return status;
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
    /* The fault path resumes FROM the context either way, so it has no use for
     * the distinction the raise path turns on. */
    BOOL ctx_rewritten;
    NTSTATUS status = dispatch_guest_exception( exc->rec, exc->ctx, exc->stack_base,
                                                exc->stack_limit, &ctx_rewritten );

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
    CONTEXT context;
    volatile BOOL dispatched = FALSE;
    void *outer_report = guest_exc_raising;

    if (!guest_exc_pending) return;
    rec = guest_exc_rec;
    guest_exc_pending = FALSE;

    /* Both guards below END THE PROCESS.  See the banner on guest_exc_raising:
     * every path out of here that is not "report this one exception once" has
     * already been shown to hang rather than crash, and a hang says nothing at
     * all.  The wild-pointer ERR in dispatch_guest_exception has already named
     * the address and the call site by the time either fires.
     *
     * The marker is a STACK POINTER, not a counter, because a counter cannot be
     * balanced here: a native __EXCEPT_ALL above this frame -- ntdll's own
     * dispatch_user_callback has one -- can swallow the exception being raised
     * and unwind straight past this function, so its epilogue is not a place
     * anything can be undone.  A stack address needs no undoing: "still inside
     * the earlier report" IS "deeper than where the earlier report began", and
     * an unwind that leaves the report behind leaves the new frame ABOVE the
     * mark by construction.  STRICTLY deeper, so a guest callback that faults
     * once per item of a native enumeration -- same call site, same depth,
     * swallowed each time -- is not mistaken for this. */
    if (outer_report && (char *)&context < (char *)outer_report)
    {
        ERR( "guest exception %08x at %p while the report of an earlier one is still "
             "running on this thread (%08x at %p): the guest's own top-level filter "
             "faulted while reporting.  Terminating -- reporting it again would "
             "recurse until the native stack is gone and the thread hangs holding "
             "whatever lock it took.\n",
             (UINT)rec.ExceptionCode, rec.ExceptionAddress,
             (UINT)guest_exc_first.ExceptionCode, guest_exc_first.ExceptionAddress );
        NtTerminateProcess( GetCurrentProcess(), guest_exc_first.ExceptionCode );
    }
    if ((char *)&context - (char *)NtCurrentTeb()->Tib.StackLimit < GUEST_EXC_STACK_FLOOR)
    {
        ERR( "guest exception %08x at %p with only %u bytes of native stack left "
             "(limit %p): not enough to dispatch it.  Terminating rather than "
             "faulting inside the fault handler.\n",
             (UINT)rec.ExceptionCode, rec.ExceptionAddress,
             (UINT)((char *)&context - (char *)NtCurrentTeb()->Tib.StackLimit),
             NtCurrentTeb()->Tib.StackLimit );
        NtTerminateProcess( GetCurrentProcess(), rec.ExceptionCode );
    }
    guest_exc_first = rec;
    guest_exc_raising = &context;
    ERR( "guest exception %08x at %p unhandled at guest level; re-raising natively\n",
         (UINT)rec.ExceptionCode, rec.ExceptionAddress );
    /* Not RtlRaiseException: its wrapper rewrites rec->ExceptionAddress to its
     * own call site, which is right for a native raise and wrong here -- this
     * record exists so that ExceptionAddress names the guest pc, and the death
     * message downstream prints that field.  Capture and dispatch directly,
     * the same pair raise_exception_from_asm() uses; the guard turns a
     * handler's EXCEPTION_CONTINUE_EXECUTION resume of the captured context
     * into a plain return, which is where an RtlRaiseException resume would
     * have landed anyway. */
    RtlCaptureContext( &context );
    if (dispatched)
    {
        /* a handler resumed the captured context: this report is over, and the
         * thread is free to run into another one later.  Restore rather than
         * clear -- an outer report, if there was one, is still running. */
        guest_exc_raising = outer_report;
        return;
    }
    dispatched = TRUE;
    if (!NtCurrentTeb()->Peb->BeingDebugged)
        RtlRaiseStatus( dispatch_exception( &rec, &context ) );
    else
        RtlRaiseStatus( NtRaiseException( &rec, &context, TRUE ) );
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
    BOOL ctx_rewritten = FALSE;
    NTSTATUS status;

    WINE_UNIX_CALL( unix_emu_guest_stack, &stack );
    status = dispatch_guest_exception( rec, ctx, stack.base, stack.limit, &ctx_rewritten );
    /* An __except accepted and ctx now describes ITS frame: the trap dispatcher
     * must not then pop a return address and store a return value on top of it,
     * which is what turned a caught guest RaiseException into a jump to a
     * half-formed address. */
    if (ctx_rewritten) emu_trap_ctx_rewritten = TRUE;
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

/***********************************************************************
 *           emu_RtlUnwindEx / emu_RtlUnwind
 *
 * A guest CALLING RtlUnwindEx itself.  Where a guest language handler's
 * __except goes once it has accepted: MSVC's own __C_specific_handler ends
 * with RtlUnwindEx( EstablisherFrame, JumpTarget, rec, ExceptionCode,
 * DispatcherContext->ContextRecord, DispatcherContext->HistoryTable ), and a
 * C++ or hand-written unwinder reaches it the same way.
 *
 * On Windows it does not return -- it resumes in another frame.  Across this
 * boundary it must not JUMP either, for a reason that has nothing to do with
 * x86-64: the guest frames it abandons may have native frames of ours
 * interleaved with them (the emulator run that is running the handler), and a
 * jump would orphan those.  guest_request_unwind() decides between performing
 * the unwind in place and handing it back to the frame walk; neither of them
 * resumes anything until every __finally between here and the target has run.
 *
 * The ContextRecord and HistoryTable arguments are deliberately unused, and
 * that matches x86-64 rather than departing from it: RtlUnwindEx opens with
 * RtlCaptureContext( context ), i.e. it overwrites the context it is handed
 * with its own and walks from there, and the history table is a lookup cache.
 * Here "its own" is the guest state the trap fired with, which is where
 * guest_unwind_in_place() starts.
 */
static NTSTATUS guest_unwind_ex( ULONG64 end_frame, ULONG64 target_ip,
                                 const EXCEPTION_RECORD *rec, ULONG64 retval )
{
    struct guest_unwind_target target = { 0 };

    if (!end_frame)
    {
        ERR( "guest RtlUnwindEx(NULL, %I64x): an EXIT unwind names no frame to resume "
             "in, so there is nothing to hand a resumed guest; not implemented\n",
             target_ip );
        return STATUS_NOT_IMPLEMENTED;
    }
    target.frame        = end_frame;
    target.target_ip    = target_ip;
    target.return_value = retval;
    target.rec          = *rec;
    if (target.rec.ExceptionCode == (DWORD)STATUS_UNWIND_CONSOLIDATE)
    {
        /* The unwind whose resume point is really a CALLBACK to run once the
         * stack has been unwound, which is how MSVC gets a catch block
         * executed.  It is checked here, at the entry, rather than only at the
         * far end where the routine is entered: an image that asks for a
         * consolidating unwind and supplies no routine has a defect this side
         * cannot repair, and finding that out after every __finally between
         * here and the target has already run would be finding it out too late
         * to say anything useful about it.  target_ip is deliberately still
         * carried: the routine does not use it, but the frame walk's handlers
         * do -- it is DISPATCHER_CONTEXT->TargetIp, which is how a
         * __C_specific_handler decides not to run the __finally of the __try it
         * is unwinding INTO. */
        if (target.rec.NumberParameters < 1 || !target.rec.ExceptionInformation[0])
        {
            ERR( "guest RtlUnwindEx(frame=%I64x) with STATUS_UNWIND_CONSOLIDATE names no "
                 "consolidation routine (NumberParameters=%u): the record asks for a "
                 "callback-based resume and supplies no callback; refusing\n",
                 end_frame, (UINT)target.rec.NumberParameters );
            return STATUS_INVALID_PARAMETER;
        }
        TRACE( "guest RtlUnwindEx: CONSOLIDATING unwind to frame %I64x, routine %p, "
               "%u parameters\n", end_frame, (void *)target.rec.ExceptionInformation[0],
               (UINT)target.rec.NumberParameters );
        return guest_request_unwind( &target );
    }
    TRACE( "guest RtlUnwindEx: frame %I64x ip %I64x code %08x retval %I64x\n",
           end_frame, target_ip, (UINT)target.rec.ExceptionCode, retval );
    return guest_request_unwind( &target );
}

static ULONG_PTR emu_RtlUnwindEx( const ULONG_PTR *a, void *native )
{
    AMD64_CONTEXT *ctx = emu_current_trap_ctx;
    const EXCEPTION_RECORD *rec = (const EXCEPTION_RECORD *)a[2];
    EXCEPTION_RECORD synth = { 0 };
    NTSTATUS status;

    if (!ctx)
    {
        ERR( "no trap context for guest RtlUnwindEx(frame=%p, target=%p)\n",
             (void *)a[0], (void *)a[1] );
        RtlRaiseStatus( STATUS_NOT_IMPLEMENTED );
    }
    if (!rec)
    {
        /* the record x86-64 RtlUnwindEx builds when it is handed none, with
         * the same ExceptionAddress: the pc of the call that unwound */
        synth.ExceptionCode    = STATUS_UNWIND;
        synth.ExceptionAddress = (void *)*(ULONG_PTR *)(ULONG_PTR)ctx->Rsp;
        rec = &synth;
    }
    if ((status = guest_unwind_ex( a[0], a[1], rec, a[3] )))
    {
        /* RtlUnwindEx has no failure return: on Windows it raises, and there
         * is no state in which the guest could sensibly carry on past an
         * unwind that did not happen.  Terminate loudly rather than return. */
        ERR( "guest RtlUnwindEx(frame=%p, target=%p) failed with %08x; terminating\n",
             (void *)a[0], (void *)a[1], (UINT)status );
        RtlRaiseStatus( status );
    }
    return 0;
}

/* The four-argument form, which Windows defines as RtlUnwindEx with the
 * caller's own context and no history table -- both of which this side
 * supplies for itself, so the forward is exact rather than approximate. */
static ULONG_PTR emu_RtlUnwind( const ULONG_PTR *a, void *native )
{
    ULONG_PTR args[6] = { a[0], a[1], a[2], a[3], 0, 0 };

    return emu_RtlUnwindEx( args, native );
}

/* The unwind data of a guest image describes x86-64 frames, so a guest asking
 * ntdll to read it must be answered by the x86-64 unwinder, not by the ppc64
 * one that these names mean natively.  Without these rows a guest CRT's own
 * stack walk gets NULL from RtlLookupFunctionEntry and a ppc64 back-chain walk
 * from RtlVirtualUnwind, over memory that is neither. */
static ULONG_PTR emu_RtlLookupFunctionEntry( const ULONG_PTR *a, void *native )
{
    return (ULONG_PTR)RtlLookupFunctionEntry_amd64( a[0], (ULONG_PTR *)a[1],
                                                    (UNWIND_HISTORY_TABLE *)a[2] );
}

static ULONG_PTR emu_RtlVirtualUnwind( const ULONG_PTR *a, void *native )
{
    return (ULONG_PTR)RtlVirtualUnwind_amd64( (ULONG)a[0], a[1], a[2],
                                              (IMAGE_AMD64_RUNTIME_FUNCTION_ENTRY *)a[3],
                                              (AMD64_CONTEXT *)a[4], (PVOID *)a[5],
                                              (ULONG64 *)a[6],
                                              (KNONVOLATILE_CONTEXT_POINTERS_AMD64 *)a[7] );
}

static ULONG_PTR emu_RtlVirtualUnwind2( const ULONG_PTR *a, void *native )
{
    return (ULONG_PTR)(ULONG)RtlVirtualUnwind2_amd64( (ULONG)a[0], a[1], a[2],
                                                      (IMAGE_AMD64_RUNTIME_FUNCTION_ENTRY *)a[3],
                                                      (AMD64_CONTEXT *)a[4], (BOOLEAN *)a[5],
                                                      (void **)a[6], (ULONG_PTR *)a[7],
                                                      (KNONVOLATILE_CONTEXT_POINTERS_AMD64 *)a[8],
                                                      (ULONG_PTR *)a[9], (ULONG_PTR *)a[10],
                                                      (PEXCEPTION_ROUTINE *)a[11], (ULONG)a[12] );
}

/* And the register file itself.  A guest asking for "the context" means its
 * own machine's, and the native namesake would write a ppc64 CONTEXT into the
 * guest's buffer -- 1232 bytes of the wrong architecture, silently, into a
 * buffer a guest unwinder is about to walk frames with.  The state to report
 * is the guest state the trap fired with, at the CALLER: Rip is the return
 * address the guest's CALL pushed and Rsp is just above it, which is what
 * x86-64 RtlCaptureContext's own "skip my frame" epilogue produces. */
static ULONG_PTR emu_RtlCaptureContext( const ULONG_PTR *a, void *native )
{
    AMD64_CONTEXT *out = (AMD64_CONTEXT *)a[0], *ctx = emu_current_trap_ctx;

    if (!out) return 0;
    if (!ctx)
    {
        ERR( "no trap context for guest RtlCaptureContext(%p)\n", out );
        return 0;
    }
    *out = *ctx;
    out->Rip  = *(DWORD64 *)(ULONG_PTR)ctx->Rsp;
    out->Rsp  = ctx->Rsp + 8;
    out->ContextFlags = CONTEXT_AMD64_FULL;
    return 0;
}

/***********************************************************************
 *           emu_GetSystemInfo / emu_GetNativeSystemInfo
 *
 * What machine a guest is told it is running on.  The native answer is
 * PROCESSOR_ARCHITECTURE_PPC64 (200, Wine's own extension) with a
 * dwProcessorType of zero, which is a truthful answer to the wrong question:
 * the caller is an x86-64 program, every pointer it holds is 64-bit x86-64,
 * and 200 is a number no x86-64 binary has ever heard of.  Measured on DOOM
 * (2016): it calls GetSystemInfo during startup and kernelbase prints
 * "fixme:heap:fill_system_info Unknown processor architecture c8" -- c8 being
 * 200 -- for every call.
 *
 * So the native implementation still supplies everything that is a property of
 * the MACHINE (page size, address range, allocation granularity, processor
 * count) and only the two fields that describe the INSTRUCTION SET are
 * rewritten, to the instruction set the caller is actually executing.  Nothing
 * native ever goes through here: the override is keyed to a guest thunk.
 */
static ULONG_PTR emu_system_info( const ULONG_PTR *a, void *native )
{
    SYSTEM_INFO *si = (SYSTEM_INFO *)a[0];

    if (!native) return 0;
    ((void (*)( ULONG_PTR ))native)( a[0] );
    if (si)
    {
        si->wProcessorArchitecture = PROCESSOR_ARCHITECTURE_AMD64;
        si->dwProcessorType        = PROCESSOR_AMD_X8664;
    }
    return 0;
}

/* Same reason, one step further: the native RtlAddFunctionTable derives the
 * table's end address from a PACKED ARM64-shaped entry, which an x86-64 entry
 * is not, so the range it registers would not contain the code it describes. */
static ULONG_PTR emu_RtlAddFunctionTable( const ULONG_PTR *a, void *native )
{
    IMAGE_AMD64_RUNTIME_FUNCTION_ENTRY *table = (IMAGE_AMD64_RUNTIME_FUNCTION_ENTRY *)a[0];
    DWORD count = (DWORD)a[1];
    ULONG_PTR base = a[2], end = base;
    void *handle;

    if (count) end += table[count - 1].EndAddress;
    return !RtlAddGrowableFunctionTable( &handle, (PRUNTIME_FUNCTION)table, count, 0, base, end );
}

/* Not reached by the frame walk, which recognises the guest ntdll's own export
 * by identity and serves it natively.  An arrival here is a guest calling it:
 * a language handler that CHAINS to it (__GSHandlerCheck_SEH is the common
 * one, /GS cookie check then ordinary scope-table semantics), or an image that
 * imported it and is dispatching by hand.  Answering ExceptionContinueSearch
 * would silently drop the __except that frame was entitled to, so run the same
 * implementation the walk runs, over the DISPATCHER_CONTEXT the guest supplied
 * -- guest memory is host memory, so its scope table and its AMD64_CONTEXT are
 * ordinary reads -- and route an accepting scope through the same unwind
 * request an RtlUnwindEx would have made, which is exactly what the x86-64
 * original does at this point. */
static ULONG_PTR emu_C_specific_handler( const ULONG_PTR *a, void *native )
{
    EXCEPTION_RECORD *rec = (EXCEPTION_RECORD *)a[0];
    AMD64_CONTEXT *ctx = (AMD64_CONTEXT *)a[2];
    DISPATCHER_CONTEXT_AMD64 *dispatch = (DISPATCHER_CONTEXT_AMD64 *)a[3];
    struct guest_unwind_target target = { 0 };
    NTSTATUS status;
    UINT res;

    if (!rec || !dispatch)
    {
        ERR( "guest __C_specific_handler(rec=%p, frame=%p, ctx=%p, dispatch=%p) with a "
             "null record or dispatcher context; refusing\n",
             (void *)a[0], (void *)a[1], (void *)a[2], (void *)a[3] );
        return ExceptionContinueSearch;
    }
    TRACE( "guest __C_specific_handler: rec %p code %08x frame %p dispatch %p\n",
           rec, (UINT)rec->ExceptionCode, (void *)a[1], dispatch );

    res = guest_c_specific_handler( rec, ctx, dispatch, &target );
    if (res != ExceptionExecuteHandler_guest) return res;

    if ((status = guest_request_unwind( &target )))
    {
        ERR( "guest __C_specific_handler: the unwind to frame %I64x ip %I64x failed with "
             "%08x; terminating\n", target.frame, target.target_ip, (UINT)status );
        RtlRaiseStatus( status );
    }
    /* the x86-64 original does not return from here either */
    return ExceptionContinueSearch;
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
 * THE RETURN WIDTH IS PER SLOT, and the day the corpus demanded it has come.
 * The default is the guest's RAX with the low 32 bits sign-extended: every
 * callback this pool carried at first returns int/BOOL/LONG, an x86-64 callee
 * writing EAX zeroes the upper half, and a native caller may rely on the ELFv2
 * rule that the callee extended its 32-bit result -- a comparator returning -1
 * must not arrive as 0xffffffff.  A WINDOW PROCEDURE is the other case: it
 * returns LRESULT, sixty-four bits of it, and a WM_ message whose result has
 * any bit above 31 set -- a pointer, a handle, a 64-bit count -- would arrive
 * at native user32 with its top half replaced by a copy of bit 31.  So the
 * width is a property of the SLOT, chosen at registration by whoever knows
 * what the callback is, and it is spelled by which dispatcher the stub jumps
 * to rather than by a flag the dispatcher reads: one stub per (target, width),
 * so the two can never be confused for one another.
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
    UINT  wide;          /* ...per WIDTH too: the other half of that identity */
    UINT  pad;           /* one cache line per slot */
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

static ULONG_PTR guest_callback_run( ULONG_PTR a0, ULONG_PTR a1, ULONG_PTR a2,
                                     ULONG_PTR a3, void *fn, BOOL *ended )
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
    if ((*ended = guest_exit_requested)) return 0;
    TRACE( "guest callback %p returned %p\n", fn, (void *)ret );
    return ret;
}

/* The narrow form: the callee wrote EAX and the ELFv2 caller expects a
 * sign-extended 32-bit result. */
static ULONG_PTR guest_callback_dispatch( ULONG_PTR a0, ULONG_PTR a1, ULONG_PTR a2,
                                          ULONG_PTR a3, void *fn )
{
    BOOL ended;
    ULONG_PTR ret = guest_callback_run( a0, a1, a2, a3, fn, &ended );

    if (ended) return 0;
    return (ULONG_PTR)(LONG_PTR)(LONG)ret;
}

/* The wide form: RAX travels whole.  A window procedure's LRESULT is the case
 * that forced it -- see the block comment above -- and truncating one is silent
 * by construction, because the low half of a handle or a pointer looks like a
 * perfectly ordinary result. */
static ULONG_PTR guest_callback_dispatch_wide( ULONG_PTR a0, ULONG_PTR a1, ULONG_PTR a2,
                                               ULONG_PTR a3, void *fn )
{
    BOOL ended;
    ULONG_PTR ret = guest_callback_run( a0, a1, a2, a3, fn, &ended );

    if (ended) return 0;
    return ret;
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

static void *wrap_guest_callback_ex( void *fn, BOOL wide )
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

    /* One stub per distinct (target, return width), across every block.  The
     * width is part of the identity rather than a property of the target,
     * because the same guest function genuinely can be registered as two
     * different callback kinds -- and a lookup that ignored it would hand the
     * second registration the first one's truncating stub. */
    {
        UINT b;
        for (b = 0; b < guest_cb_blocks; b++)
        {
            UINT used = (b + 1 == guest_cb_blocks) ? guest_cb_count : GUEST_CB_BLOCK;
            for (i = 0; i < used; i++)
                if (guest_cb_block[b][i].guest_fn == fn &&
                    guest_cb_block[b][i].wide == (wide ? 1u : 0u))
                { ret = &guest_cb_block[b][i]; goto done; }
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
    p = emit_load_imm64( p, 12, (ULONG_PTR)(wide ? guest_callback_dispatch_wide
                                                 : guest_callback_dispatch) );
    *p++ = 0x7D8903A6;   /* mtctr r12 */
    *p++ = 0x4E800420;   /* bctr */
    stub->guest_fn = fn;
    stub->wide     = wide ? 1u : 0u;
    NtFlushInstructionCache( GetCurrentProcess(), stub, sizeof(*stub) );
    guest_cb_count++;    /* publish only after the code is flushed */
    TRACE( "guest callback %p -> trampoline %p (%u total, %s return)\n",
           fn, stub, guest_cb_count, wide ? "64-bit" : "sign-extended 32-bit" );
    ret = stub;
done:
    LdrUnlockLoaderLock( 0, magic );
    return ret;
}

static void *wrap_guest_callback( void *fn )
{
    return wrap_guest_callback_ex( fn, FALSE );
}

/* The trampoline factory, exported for guest-COM modules.  A COM method traps
 * inside a vtable stub array and is routed by RIP arithmetic, so the override
 * table above -- keyed on flat export names -- can never reach it; a module
 * whose hand slot receives a guest callback (dinput8's EnumDevices, comdlg32's
 * dialog hooks) resolves this with LdrGetProcedureAddress instead, so a tree
 * without it refuses loudly rather than failing to load. */
void * CDECL __wine_guest_wrap_callback( void *fn, BOOL wide )
{
    return wrap_guest_callback_ex( fn, wide );
}

/***********************************************************************
 *           wrap_guest_wndproc
 *
 * A WNDPROC, which differs from every other callback in this file twice over.
 *
 * It returns LRESULT, so it needs the wide trampoline.  And a WNDPROC-shaped
 * value is not always a code pointer at all: win32u hands one out as a WINPROC
 * HANDLE -- MAKELONG(index, 0xffff), i.e. 0xffff00nn -- whenever a window's
 * ANSI and Unicode procedures differ, and both GetWindowLongPtr and
 * SetWindowLongPtr's return value can be one.  A guest that reads one back and
 * passes it to CallWindowProc is doing exactly what it is supposed to do, and
 * the ordinary wrapper would classify that small integer as "guest code in no
 * loaded image" -- its documented fallback, and the right one for a JIT'd guest
 * callback -- and then run 0xffff0002 as x86-64.  So the one shape that cannot
 * be a pointer is recognised and passed through untouched, by name, and
 * everything else goes through the ordinary classification.
 */
static void *wrap_guest_wndproc( void *fn )
{
    ULONG_PTR val = (ULONG_PTR)fn;

    if ((val >> 16) == 0xffff)
    {
        TRACE( "wndproc %p is a win32u winproc handle, not a code pointer; "
               "passing it through\n", fn );
        return fn;
    }
    /* And a NATIVE window procedure passes through QUIETLY, which is the one
     * place this differs from the general wrapper's WARN.  Subclassing hands a
     * guest the procedure it is replacing -- user32's own DefWindowProc, or the
     * class's -- and a guest that chains to it through CallWindowProc is doing
     * precisely what it should.  The general wrapper is right to warn about a
     * native pointer arriving where a guest callback was expected; here it is
     * not unexpected, and a subclass that chains for every message would print
     * that warning once per message and bury everything else in the log. */
    if (fn && !guest_module_from_address( fn ))
    {
        void *image;
        if (RtlPcToFileHeader( fn, &image ))
        {
            TRACE( "wndproc %p is native code, passing it through\n", fn );
            return fn;
        }
    }
    return wrap_guest_callback_ex( fn, TRUE );
}

/* swap the arguments a thunk_override row declares as callbacks; `wide` names
 * the subset of them whose return value is a full 64 bits */
static void wrap_thunk_callback_args( ULONG_PTR *a, UINT argc, UINT mask, UINT wide )
{
    UINT i;
    for (i = 0; i < argc; i++)
        if (mask & (1u << i))
            a[i] = (ULONG_PTR)wrap_guest_callback_ex( (void *)a[i], (wide >> i) & 1 );
}


/***********************************************************************
 *           window procedures
 *
 * The callback class the argument-position mask cannot describe, and the one
 * every windowed application registers first: RegisterClass takes a POINTER TO
 * A STRUCT and the WNDPROC is a field inside it, so there is no argument index
 * to name.  Measured on DOOM (2016): its RegisterClassA carried the game's own
 * x86-64 WNDPROC straight through to native user32, which stored it and then
 * bctrl'd into it with WM_NCCREATE -- and user32's callback dispatcher SWALLOWS
 * what a window procedure raises, so the c000001d that produced appeared once
 * as "err:seh:dispatch_user_callback ignoring exception c000001d" and the game
 * carried on with a window that would never receive a message and no error at
 * all.  A silent wrong answer, which is the failure mode this whole
 * registration-side mechanism exists to replace with a loud one.
 *
 * So these rows copy the struct, swap the one field, and hand NATIVE user32 a
 * struct it can call.  The copy is necessary and not defensive: the caller's
 * struct is the guest's, it may be const, it may be reused for a second class,
 * and writing a trampoline into it would make the guest's own later reads of
 * lpfnWndProc report a ppc64 address.
 *
 * The other entry points a real game reaches, and what each does:
 *
 *   SetWindowLongPtrA/W( GWLP_WNDPROC )  swaps the value, and only for that
 *      index -- every other index carries data, not code.
 *   CallWindowProcA/W  swaps its FIRST argument, which is a WNDPROC the guest
 *      obtained from somewhere: our own trampoline (idempotent, comes back
 *      unchanged), a win32u winproc handle (recognised and passed through, see
 *      wrap_guest_wndproc) or a raw guest function it never registered at all.
 *      All three have to work, because subclassing is the one idiom that uses
 *      every one of them.
 *   DefWindowProcA/W  needs no row: it takes no callback, and a guest window
 *      procedure calling it is an ordinary guest-to-native thunk call.
 *   SetWindowLongA/W  needs no row either, and that is a fact about the
 *      architecture rather than a decision: user32 fails GWLP_WNDPROC with
 *      ERROR_INVALID_INDEX on _WIN64 (dlls/user32/win.c), because a 32-bit
 *      value cannot carry a 64-bit procedure address.  DOOM imports
 *      SetWindowLongA and uses it for styles.
 *   CreateWindowExA/W  carries no callback: lpParam is the application's own
 *      pointer, handed back to the WNDPROC in the CREATESTRUCT, and wrapping it
 *      would corrupt it.  Left alone deliberately.
 *   SetClassLongPtrA/W( GCLP_WNDPROC )  is the class-level analogue of the
 *      window-level row above and is NOT here, for the reason every row in this
 *      table is driven by: nothing in the corpus calls it.  DOOM (2016) imports
 *      RegisterClassA/W, CreateWindowExA/W, SetWindowLongPtrA/W,
 *      CallWindowProcA/W and DefWindowProcA/W from USER32 and not that.  When
 *      something does, it belongs beside emu_SetWindowLongPtr and needs nothing
 *      new -- which is the point of saying so here rather than adding a row
 *      nothing has ever exercised.
 *
 * WHAT NATIVE CODE READS BACK.  GetWindowLongPtr( GWLP_WNDPROC ) and
 * GetClassLongPtr( GCLP_WNDPROC ) return what was SET, which on this port is
 * the trampoline.  That is the accepted answer: from user32's point of view the
 * trampoline IS the window procedure, it is what a subsequent CallWindowProc
 * must invoke, and it is idempotent through every path here.  A guest comparing
 * a read-back WNDPROC against its own function address will see them differ --
 * true on Windows too whenever an A/W mismatch turns the value into a winproc
 * handle, which is why no correct program does that comparison.
 */
struct emu_wndclass
{
    UINT   style;
    void  *lpfnWndProc;
    INT    cbClsExtra;
    INT    cbWndExtra;
    void  *hInstance;
    void  *hIcon;
    void  *hCursor;
    void  *hbrBackground;
    void  *lpszMenuName;
    void  *lpszClassName;
};

struct emu_wndclassex
{
    UINT   cbSize;
    UINT   style;
    void  *lpfnWndProc;
    INT    cbClsExtra;
    INT    cbWndExtra;
    void  *hInstance;
    void  *hIcon;
    void  *hCursor;
    void  *hbrBackground;
    void  *lpszMenuName;
    void  *lpszClassName;
    void  *hIconSm;
};

/* Declared here rather than by including winuser.h, which ntdll deliberately
 * does not (dlls/ntdll/loader.c and actctx.c both say so in as many words), and
 * pinned rather than trusted: this struct is copied wholesale and handed to the
 * NATIVE user32, so a layout disagreement would hand it eight rewritten bytes
 * in the wrong place.  The A and W forms differ only in the type of two string
 * pointers, so one declaration serves all four entry points; MS-x64 and ELFv2
 * LP64 agree on every offset here. */
C_ASSERT( sizeof(struct emu_wndclass) == 72 );
C_ASSERT( offsetof(struct emu_wndclass, lpfnWndProc) == 8 );
C_ASSERT( offsetof(struct emu_wndclass, lpszClassName) == 64 );
C_ASSERT( sizeof(struct emu_wndclassex) == 80 );
C_ASSERT( offsetof(struct emu_wndclassex, lpfnWndProc) == 8 );
C_ASSERT( offsetof(struct emu_wndclassex, hIconSm) == 72 );

#define EMU_GWLP_WNDPROC  (-4)

static ULONG_PTR emu_RegisterClass( const ULONG_PTR *a, void *native )
{
    const struct emu_wndclass *in = (const struct emu_wndclass *)a[0];
    struct emu_wndclass wc;

    if (!native || !in) return 0;
    wc = *in;
    wc.lpfnWndProc = wrap_guest_wndproc( wc.lpfnWndProc );
    TRACE( "RegisterClass(%p): wndproc %p -> %p\n", in, in->lpfnWndProc, wc.lpfnWndProc );
    return ((ULONG_PTR (*)( const void * ))native)( &wc );
}

static ULONG_PTR emu_RegisterClassEx( const ULONG_PTR *a, void *native )
{
    const struct emu_wndclassex *in = (const struct emu_wndclassex *)a[0];
    struct emu_wndclassex wc;

    if (!native || !in) return 0;
    /* cbSize checked BEFORE the copy, not after: native RegisterClassEx rejects
     * a wrong one anyway, but reading eighty bytes out of a caller's shorter
     * struct to find that out is this side's own fault.  Passed through
     * untouched so that the caller gets user32's error and not ours. */
    if (in->cbSize != sizeof(*in))
    {
        WARN( "RegisterClassEx(%p) with cbSize %u, not %u; passing it through unwrapped "
              "so that user32 answers for it\n", in, in->cbSize, (UINT)sizeof(*in) );
        return ((ULONG_PTR (*)( const void * ))native)( in );
    }
    wc = *in;
    wc.lpfnWndProc = wrap_guest_wndproc( wc.lpfnWndProc );
    TRACE( "RegisterClassEx(%p): wndproc %p -> %p\n", in, in->lpfnWndProc, wc.lpfnWndProc );
    return ((ULONG_PTR (*)( const void * ))native)( &wc );
}

static ULONG_PTR emu_SetWindowLongPtr( const ULONG_PTR *a, void *native )
{
    ULONG_PTR value = a[2];

    if (!native) return 0;
    /* Only the one index.  DWLP_DLGPROC would be the second, and it is not here
     * because a dialog procedure is installed by DialogBoxParam's own argument
     * rather than by this call in every program the corpus contains; the day one
     * does it this way, it belongs here beside GWLP_WNDPROC. */
    if ((int)(LONG)a[1] == EMU_GWLP_WNDPROC)
    {
        value = (ULONG_PTR)wrap_guest_wndproc( (void *)a[2] );
        TRACE( "SetWindowLongPtr(%p, GWLP_WNDPROC): %p -> %p\n",
               (void *)a[0], (void *)a[2], (void *)value );
    }
    return ((ULONG_PTR (*)( ULONG_PTR, ULONG_PTR, ULONG_PTR ))native)( a[0], a[1], value );
}

static ULONG_PTR emu_CallWindowProc( const ULONG_PTR *a, void *native )
{
    ULONG_PTR proc;

    if (!native) return 0;
    proc = (ULONG_PTR)wrap_guest_wndproc( (void *)a[0] );
    TRACE( "CallWindowProc(%p -> %p, hwnd %p, msg %04x)\n",
           (void *)a[0], (void *)proc, (void *)a[1], (UINT)a[2] );
    return ((ULONG_PTR (*)( ULONG_PTR, ULONG_PTR, ULONG_PTR, ULONG_PTR, ULONG_PTR ))native)
        ( proc, a[1], a[2], a[3], a[4] );
}

/* winmm's open calls: the callback argument is a DWORD_PTR whose meaning is
 * decided by a flag in a LATER argument -- with CALLBACK_FUNCTION it is a
 * function pointer a native mixer thread calls, with CALLBACK_EVENT an event
 * HANDLE, with CALLBACK_WINDOW an HWND, with CALLBACK_THREAD a thread id.  A
 * cb_mask column cannot say "sometimes", so these carry handlers that wrap the
 * pointer only in the CALLBACK_FUNCTION case.  MEASURED 2026-08-17: a guest
 * waveOutOpen(..., CALLBACK_FUNCTION) faulted in its own image. */
#define EMU_CALLBACK_TYPEMASK   0x00070000
#define EMU_CALLBACK_FUNCTION   0x00030000
#define EMU_MMSYSERR_INVALPARAM 11

static ULONG_PTR emu_winmm_open6( const ULONG_PTR *a, void *native )
{
    ULONG_PTR cb = a[3];

    if (!native) return EMU_MMSYSERR_INVALPARAM;
    if ((a[5] & EMU_CALLBACK_TYPEMASK) == EMU_CALLBACK_FUNCTION)
    {
        cb = (ULONG_PTR)wrap_guest_callback( (void *)a[3] );
        TRACE( "winmm open: CALLBACK_FUNCTION %p -> %p\n", (void *)a[3], (void *)cb );
    }
    return ((ULONG_PTR (*)( ULONG_PTR, ULONG_PTR, ULONG_PTR, ULONG_PTR,
                            ULONG_PTR, ULONG_PTR ))native)
        ( a[0], a[1], a[2], cb, a[4], a[5] );
}

static ULONG_PTR emu_winmm_open5( const ULONG_PTR *a, void *native )
{
    ULONG_PTR cb = a[2];

    if (!native) return EMU_MMSYSERR_INVALPARAM;
    if ((a[4] & EMU_CALLBACK_TYPEMASK) == EMU_CALLBACK_FUNCTION)
    {
        cb = (ULONG_PTR)wrap_guest_callback( (void *)a[2] );
        TRACE( "winmm open: CALLBACK_FUNCTION %p -> %p\n", (void *)a[2], (void *)cb );
    }
    return ((ULONG_PTR (*)( ULONG_PTR, ULONG_PTR, ULONG_PTR, ULONG_PTR,
                            ULONG_PTR ))native)( a[0], a[1], cb, a[3], a[4] );
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
    /* msvcr100.dll: the same _onexit registration point msvcrt.dll and
     * ucrtbase.dll already have above -- Styx: Master of Shadows imports it,
     * and this is the VC++ 2010 runtime a game statically linked against the
     * DLL CRT actually calls, not msvcrt.dll's.  msvcr100 exports no
     * _crt_atexit and no public atexit (its atexit is `-private`, "not
     * imported to avoid conflicts with Mingw"), so those two have no row
     * here; there is nothing for them to intercept. */
    { L"msvcr100.dll", "_onexit",           1, emu_onexit },
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
    /* Audio device enumeration.  MEASURED 2026-08-17: a guest calling
     * DirectSoundEnumerateA with its own LPDSENUMCALLBACKA reached native
     * dsound with a raw guest address, which dsound then called -- "Unhandled
     * page fault on execute access".  The callback takes four pointers and
     * returns BOOL, so the plain sign-extended trampoline is the right shape.
     * Enumeration is how a title builds its device list before it ever calls
     * DirectSoundCreate8. */
    { L"dsound.dll", "DirectSoundEnumerateA",        2, NULL, 1u << 0 },
    { L"dsound.dll", "DirectSoundEnumerateW",        2, NULL, 1u << 0 },
    { L"dsound.dll", "DirectSoundCaptureEnumerateA", 2, NULL, 1u << 0 },
    { L"dsound.dll", "DirectSoundCaptureEnumerateW", 2, NULL, 1u << 0 },
    /* winmm's multimedia timer: LPTIMECALLBACK at argument 2, called from a
     * native timer thread.  Same shape, same reason. */
    { L"winmm.dll",  "timeSetEvent",                 5, NULL, 1u << 2 },
    /* winmm open calls: the callback argument's MEANING is decided by a flag in
     * a LATER argument (CALLBACK_FUNCTION -> a native mixer thread calls it,
     * CALLBACK_EVENT/_WINDOW/_THREAD -> not a code pointer), which a cb_mask
     * column cannot express -- so these carry handlers.  MEASURED: a guest
     * waveOutOpen(..., CALLBACK_FUNCTION) faulted in its own image. */
    { L"winmm.dll", "waveOutOpen",     6, emu_winmm_open6 },
    { L"winmm.dll", "waveInOpen",      6, emu_winmm_open6 },
    { L"winmm.dll", "midiStreamOpen",  6, emu_winmm_open6 },
    { L"winmm.dll", "midiOutOpen",     5, emu_winmm_open5 },
    { L"winmm.dll", "midiInOpen",      5, emu_winmm_open5 },
    { L"winmm.dll", "mixerOpen",       5, emu_winmm_open5 },
    /* every winetest registers a top-level exception filter; SEH dispatch
     * calling a guest filter natively was an illegal-instruction storm ending
     * in a stack overflow, which buried the REAL failure under it */
    { L"kernel32.dll",   "SetUnhandledExceptionFilter", 1, NULL, 1u << 0 },
    { L"kernelbase.dll", "SetUnhandledExceptionFilter", 1, NULL, 1u << 0 },
    /* Fiber-local storage destructors.  Every MSVC CRT allocates one FLS slot
     * for its per-thread data and hands FlsAlloc the function that frees it
     * (UCRT's __acrt_freefls), so this row is reached by ANY guest built with
     * a Microsoft toolchain -- and it is reached at PROCESS EXIT, after the
     * program's own output is already on the terminal, which is the worst
     * possible place to find it.  Steam's d3ddriverquery64.exe printed its
     * whole answer and then died c000001d at 0000000140002FEC: native ntdll's
     * RtlProcessFlsData did `callback( value )` -- an ELFv2 mtctr/bctrl -- into
     * guest x86-64 bytes, which decode as ppc64 for two words and then hit
     * primary opcode 1.  The callback takes one pointer and returns void, so
     * the plain sign-extended trampoline is the right shape.  RtlFlsAlloc is
     * the same registration one layer down, where a guest that calls ntdll
     * directly arrives (callback first, index second). */
    { L"kernel32.dll",   "FlsAlloc",    1, NULL, 1u << 0 },
    { L"kernelbase.dll", "FlsAlloc",    1, NULL, 1u << 0 },
    { L"ntdll.dll",      "RtlFlsAlloc", 2, NULL, 1u << 0 },
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
    /* A guest-initiated unwind, which is where a guest language handler's
     * accepting __except goes.  It walks the guest stack, runs every guest
     * __finally between here and the target, and resumes the guest at
     * TargetIp -- see emu_RtlUnwindEx. */
    { L"ntdll.dll",      "RtlUnwind",         4, emu_RtlUnwind },
    { L"ntdll.dll",      "RtlUnwindEx",       6, emu_RtlUnwindEx },
    /* table-based dispatch: the unwind data a guest asks about is its own
     * machine's, so these must be answered by the x86-64 unwinder */
    { L"ntdll.dll",      "RtlLookupFunctionEntry",  3, emu_RtlLookupFunctionEntry },
    { L"ntdll.dll",      "RtlVirtualUnwind",        8, emu_RtlVirtualUnwind },
    { L"ntdll.dll",      "RtlVirtualUnwind2",      13, emu_RtlVirtualUnwind2 },
    { L"ntdll.dll",      "RtlCaptureContext",       1, emu_RtlCaptureContext },
    { L"ntdll.dll",      "RtlAddFunctionTable",     3, emu_RtlAddFunctionTable },
    { L"ntdll.dll",      "__C_specific_handler",    4, emu_C_specific_handler },
    /* KERNEL32 forwards every one of these to ntdll, and a real application
     * takes them from there rather than from ntdll: DOOM (2016) imports
     * RtlUnwindEx, RtlLookupFunctionEntry, RtlVirtualUnwind, RtlCaptureContext
     * and RtlPcToFileHeader from KERNEL32.dll and nothing at all from ntdll.
     * A row keyed only to ntdll leaves those bound to the NATIVE ppc64
     * implementations, which answer questions about ppc64 frames -- silently,
     * and about the wrong machine.  (RtlPcToFileHeader needs no row: it
     * answers from the loader's module list, in which guest images appear.) */
    { L"kernel32.dll",   "RtlUnwind",               4, emu_RtlUnwind },
    { L"kernel32.dll",   "RtlUnwindEx",             6, emu_RtlUnwindEx },
    { L"kernel32.dll",   "RtlLookupFunctionEntry",  3, emu_RtlLookupFunctionEntry },
    { L"kernel32.dll",   "RtlVirtualUnwind",        8, emu_RtlVirtualUnwind },
    { L"kernel32.dll",   "RtlCaptureContext",       1, emu_RtlCaptureContext },
    { L"kernel32.dll",   "RtlAddFunctionTable",     3, emu_RtlAddFunctionTable },
    { L"kernel32.dll",   "RtlRaiseException",       1, emu_RtlRaiseException },
    { L"kernel32.dll",   "__C_specific_handler",    4, emu_C_specific_handler },
    { L"kernelbase.dll", "__C_specific_handler",    4, emu_C_specific_handler },
    /* the instruction set a guest is told it is running on; see emu_system_info */
    { L"kernel32.dll",   "GetSystemInfo",           1, emu_system_info },
    { L"kernel32.dll",   "GetNativeSystemInfo",     1, emu_system_info },
    { L"kernelbase.dll", "GetSystemInfo",           1, emu_system_info },
    { L"kernelbase.dll", "GetNativeSystemInfo",     1, emu_system_info },
    /* enumeration callbacks: native code holds the pointer for the whole
     * enumeration and calls it once per item.  DOOM (2016) asks GDI to
     * enumerate font families during its startup and died c0000005 on an
     * EXECUTE fault -- native gdi32 bctrl'd into the guest's FONTENUMPROCA
     * and ran x86-64 bytes as ppc64 until they branched somewhere
     * unmapped.  A FONTENUMPROC is (LOGFONT*, TEXTMETRIC*, DWORD, LPARAM)
     * returning INT, which is exactly the trampoline's four-argument,
     * sign-extended-32-bit-return shape. */
    { L"gdi32.dll", "EnumFontFamiliesA",   4, NULL, 1u << 2 },
    { L"gdi32.dll", "EnumFontFamiliesW",   4, NULL, 1u << 2 },
    { L"gdi32.dll", "EnumFontFamiliesExA", 5, NULL, 1u << 2 },
    { L"gdi32.dll", "EnumFontFamiliesExW", 5, NULL, 1u << 2 },
    { L"gdi32.dll", "EnumFontsA",          4, NULL, 1u << 2 },
    { L"gdi32.dll", "EnumFontsW",          4, NULL, 1u << 2 },
    /* The same class with a WORSE failure, and the reason this row is not
     * left for the next run to find: user32's callback dispatcher catches
     * what its callbacks raise, so DOOM's MONITORENUMPROC ran as ppc64,
     * faulted, and the fault was swallowed -- "ignoring exception c0000005".
     * The game got an empty monitor enumeration and no error at all. */
    { L"user32.dll", "EnumDisplayMonitors", 4, NULL, 1u << 2 },
    /* comparators: the signed-return case -- a cmp returning -1 must not
     * reach native qsort as 0xffffffff */
    { L"msvcrt.dll",   "qsort",   4, NULL, 1u << 3 },
    { L"ucrtbase.dll", "qsort",   4, NULL, 1u << 3 },
    { L"msvcr100.dll", "qsort",   4, NULL, 1u << 3 },
    { L"msvcrt.dll",   "bsearch", 5, NULL, 1u << 4 },
    { L"ucrtbase.dll", "bsearch", 5, NULL, 1u << 4 },
    { L"msvcr100.dll", "bsearch", 5, NULL, 1u << 4 },
    /* window procedures: the callback carried INSIDE A STRUCT, which no
     * argument-position mask can name, plus the other entry points through
     * which a WNDPROC reaches native user32.  See the block above the four
     * override functions for what each row does, and for the rows that are
     * deliberately absent (SetWindowLong, CreateWindowEx, SetClassLongPtr). */
    { L"user32.dll", "RegisterClassA",     1, emu_RegisterClass },
    { L"user32.dll", "RegisterClassW",     1, emu_RegisterClass },
    { L"user32.dll", "RegisterClassExA",   1, emu_RegisterClassEx },
    { L"user32.dll", "RegisterClassExW",   1, emu_RegisterClassEx },
    { L"user32.dll", "SetWindowLongPtrA",  3, emu_SetWindowLongPtr },
    { L"user32.dll", "SetWindowLongPtrW",  3, emu_SetWindowLongPtr },
    { L"user32.dll", "CallWindowProcA",    5, emu_CallWindowProc },
    { L"user32.dll", "CallWindowProcW",    5, emu_CallWindowProc },
    /* The same class reached by argument position, so a plain mask serves --
     * with the WIDE bit, because both of these return LRESULT and truncating
     * one is silent.  DOOM (2016) imports SetWindowsHookExA and SetTimer; a
     * hook procedure is called by user32 for every message in the queue, and a
     * TIMERPROC is called from the message loop, so both are native code
     * holding a guest pointer for the life of the window. */
    { L"user32.dll", "SetWindowsHookExA",  4, NULL, 1u << 1, 1u << 1 },
    { L"user32.dll", "SetWindowsHookExW",  4, NULL, 1u << 1, 1u << 1 },
    { L"user32.dll", "SetTimer",           4, NULL, 1u << 3 },
    /* the whole of modern OpenGL, which no opengl32 anywhere exports: see the
     * banner above emu_wglGetProcAddress.  wglGetDefaultProcAddress takes the
     * same row because it answers in the same namespace -- Wine's returns NULL
     * today, and the day it does not, the guest must still get a stub. */
    { L"opengl32.dll", "wglGetProcAddress",        1, emu_wglGetProcAddress },
    { L"opengl32.dll", "wglGetDefaultProcAddress", 1, emu_wglGetProcAddress },
};

/***********************************************************************
 *           native surfaces larger than a native export table
 *
 * A trapping stub is resolved by looking its name up in the native namesake's
 * export table, which is right for every module whose guest surface IS its
 * export table -- that is, all of them but one.  opengl32's guest surface is
 * its 361 exports plus 2,753 entry points that exist only in a table inside
 * the module, because that is where OpenGL has kept them since GL 1.2.  There
 * is nothing for LdrGetProcedureAddress to find and nothing wrong with the
 * module.
 *
 * So a module may name a resolver of last resort, consulted only after the
 * export table has already said no.  It resolves the name in whatever way
 * that module's own surface actually works -- for opengl32, one private
 * export, __wine_gl_entry_point, which answers the same registry
 * wglGetProcAddress answers from, with no context and no extension gating.
 * The gating already happened, in emu_wglGetProcAddress, at the moment the
 * guest asked for the address; by the time the stub traps the question is
 * settled, and asking it again would fail the standard sequence of creating a
 * throwaway context, fetching wglCreateContextAttribsARB, destroying the
 * context and only then calling what was fetched.
 *
 * The precedent is d3d12's __wine_com_dispatch: one private entry point in
 * the native module beats 2,753 exports that would exist for one caller.  A
 * resolver is NOT a place to invent an implementation -- it either finds the
 * module's own code or answers NULL, and NULL is reported by the caller
 * exactly as a missing export is.
 */
typedef void *(*thunk_resolver_func)( HMODULE native, const char *name );

static void *resolve_gl_entry_point( HMODULE native, const char *name )
{
    ULONG_PTR (*entry_point)( ULONG_PTR );
    ANSI_STRING str;

    /* Resolved per call rather than cached: this is one export-table lookup
     * next to a trap, and a cached code pointer would outlive an unload of the
     * module it points into. */
    RtlInitAnsiString( &str, "__wine_gl_entry_point" );
    if (LdrGetProcedureAddress( native, &str, 0, (void **)&entry_point ))
    {
        ERR( "native opengl32 exports no __wine_gl_entry_point; the guest's GL "
             "extension stubs cannot be resolved\n" );
        return NULL;
    }
    return (void *)entry_point( (ULONG_PTR)name );
}

static const struct
{
    const WCHAR         *module;
    thunk_resolver_func  resolve;
} thunk_resolvers[] =
{
    { L"opengl32.dll", resolve_gl_entry_point },
};

static void *resolve_beyond_exports( const WCHAR *module, HMODULE native, const char *name )
{
    UINT i;

    if (!native) return NULL;
    for (i = 0; i < ARRAY_SIZE(thunk_resolvers); i++)
    {
        if (wcsicmp( module, thunk_resolvers[i].module )) continue;
        return thunk_resolvers[i].resolve( native, name );
    }
    return NULL;
}

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
                                      struct com_thunk_hit *com, UINT *cb_mask, UINT *cb_wide,
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
            /* A module whose surface is genuinely larger than its export
             * table gets one more chance, in its own terms; everything else
             * is a missing export and says so. */
            if (!(proc = resolve_beyond_exports( mod->BaseDllName.Buffer, native,
                                                 func_name.Buffer )))
                WARN( "native %s has no %s; only an override can serve it\n",
                      debugstr_w(mod->BaseDllName.Buffer), func_name.Buffer );
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
            TRACE( "%s.%s -> override %p cb_mask %#x cb_wide %#x (%u args)\n",
                   debugstr_w(mod->BaseDllName.Buffer), thunk_overrides[i].name,
                   thunk_overrides[i].func, thunk_overrides[i].cb_mask,
                   thunk_overrides[i].cb_wide, thunk_overrides[i].argc );
            *override = thunk_overrides[i].func;
            *cb_mask  = thunk_overrides[i].cb_mask;
            *cb_wide  = thunk_overrides[i].cb_wide;
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
                   "stdu 1, -176(1)\n\t"
                   __ASM_CFI(".cfi_def_cfa_offset 176\n\t")
                   /* 32(1)-160(1) is the callee's parameter save area, sized
                    * for all THUNK_MAX_ARGS arguments rather than for the
                    * eight that fit in registers; 160 up is ours.  Keep our
                    * TOC and fp_ret above it. */
                   "std 2, 160(1)\n\t"
                   "std 6, 168(1)\n\t"           /* fp_ret, dead across the call */
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
                   /* Arguments 8 and up own doublewords in the parameter save
                    * area and travel in no register at all -- r3-r10 are
                    * positions 0-7 and there is no r11 argument.  The integer
                    * path gets this for free because it is a C call and the
                    * compiler lays the frame out; here it has to be written.
                    * Leaving it out dropped every argument past the eighth of
                    * any function that ALSO has floating point, which is
                    * glMap2f and glMap2d -- GL 1.1, ten arguments, four of
                    * them floats -- whose `vorder` and control-point array are
                    * exactly those slots.  A wrong number, not a crash.  r0 is
                    * scratch and r4 is still the gpr base at this point. */
                   "ld 0, 64(4)\n\t"  "std 0, 96(1)\n\t"
                   "ld 0, 72(4)\n\t"  "std 0, 104(1)\n\t"
                   "ld 0, 80(4)\n\t"  "std 0, 112(1)\n\t"
                   "ld 0, 88(4)\n\t"  "std 0, 120(1)\n\t"
                   "ld 0, 96(4)\n\t"  "std 0, 128(1)\n\t"
                   "ld 0, 104(4)\n\t" "std 0, 136(1)\n\t"
                   "ld 0, 112(4)\n\t" "std 0, 144(1)\n\t"
                   "ld 0, 120(4)\n\t" "std 0, 152(1)\n\t"
                   "ld 10, 56(4)\n\t"
                   "ld 9, 48(4)\n\t"
                   "ld 8, 40(4)\n\t"
                   "ld 7, 32(4)\n\t"
                   "ld 6, 24(4)\n\t"
                   "ld 5, 16(4)\n\t"
                   "ld 3, 0(4)\n\t"
                   "ld 4, 8(4)\n\t"              /* last: r4 is the base */
                   "bctrl\n\t"
                   "ld 2, 160(1)\n\t"            /* the callee may clobber r2 */
                   "ld 11, 168(1)\n\t"
                   "stfd 1, 0(11)\n\t"           /* f1 holds any FP return */
                   "addi 1, 1, 176\n\t"
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
 *   pair puts the double in XMM0 and the int in RDX, not RCX.  There are only
 *   four of those positions: XMM0-3, after which an FP argument goes on the
 *   stack like any other, in the same slot an integer would have used.
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
    /* The negative control: the floating-point path exactly as it was before
     * arguments were allowed to travel on the stack -- every FP argument read
     * out of XMMi however far along it is, and every argument past the eighth
     * dropped, because call_native_thunk_fp used to fill eight registers and
     * no parameter save area.  Both halves are one lever because they are one
     * defect: "an argument that is not in a register is not an argument".
     * ppc64le/opengl/check-gl-smoke.sh must go red at BOTH the glOrtho matrix
     * (the FP half) and the glMap2f evaluator (the GPR half) under it. */
    static int nostack = -1;
    UINT i, nfpr = 0;

    if (nostack == -1) nostack = emu_env_flag( L"WINEEMUFPNOSTACK" );

    memset( gpr, 0, THUNK_MAX_ARGS * sizeof(*gpr) );
    memset( fpr, 0, THUNK_MAX_FP_ARGS * sizeof(*fpr) );

    for (i = 0; i < argc; i++)
    {
        if (fp_mask & (1u << i))
        {
            /* Where the guest PUT it, which is not always a register.  MS-x64
             * has exactly FOUR argument slots: arguments 0-3 travel in
             * RCX/RDX/R8/R9 or, if floating point, in XMM0-3 BY POSITION --
             * and everything from the fifth argument on travels on the stack
             * whatever its type.  There is no XMM4 argument register.
             *
             * Reading &XmmRegisters[i] unconditionally therefore read a
             * volatile scratch register for every FP argument past the fourth:
             * glOrtho(l,r,b,t,near,far) got its near and far planes out of
             * XMM4/XMM5, glUniform4f(loc,x,y,z,w) its w out of XMM4.  Wrong
             * numbers, not crashes -- the class probes/check-fp-marshal.sh
             * exists for.  ppc64le/opengl/check-gl-smoke.sh value-checks
             * exactly this shape against GL's own projection matrix.
             *
             * A stack slot is eight bytes and a `float` argument occupies its
             * low four, so the same single/double choice reads it correctly.
             * gpr[i] stays zero either way: this argument's GPR slot is
             * skipped, not filled by the next integer. */
            const void *slot = (i < 4 || nostack)
                ? (const void *)&ctx->FltSave.XmmRegisters[i]
                : (const void *)(ULONG_PTR)(ctx->Rsp + 8 + i * 8);

            if (nfpr < THUNK_MAX_FP_ARGS)
                fpr[nfpr++] = (single & (1u << i)) ? (double)*(const float *)slot
                                                   : *(const double *)slot;
            continue;
        }
        if (i >= THUNK_MAX_ARGS) continue;
        /* the other half of the negative control: an argument that ELFv2 puts
         * in the parameter save area rather than a register never arrives */
        if (nostack && i >= 8) continue;
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
    BOOL prev_ctx_rewritten = emu_trap_ctx_rewritten;
    thunk_override_func override = NULL;
    struct com_thunk_hit com = { 0 };
    NTSTATUS status = STATUS_SUCCESS;
    ULONG_PTR a[THUNK_MAX_ARGS] = { 0 };
    UINT sig = 0, argc, cb_mask = 0, cb_wide = 0, fp = 0;
    ULONG_PTR ret;
    void *proc;

    /* raise-style overrides dispatch against this trap's guest state; saved
     * and restored so a nested dispatch (guest handler makes a thunk call)
     * leaves the outer one intact */
    emu_current_trap_ctx = ctx;
    emu_trap_ctx_rewritten = FALSE;

    proc = find_guest_thunk_target( ctx->Rip, &sig, &override, &com, &cb_mask, &cb_wide, &fp );
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
            if (cb_mask) wrap_thunk_callback_args( a, argc, cb_mask, cb_wide );
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
         * pushed.  Unless the override replaced the whole CONTEXT, in which
         * case there is no call to return from -- the guest is resuming in an
         * __except body several frames up and every register here is already
         * the one it must resume with. */
        if (!emu_trap_ctx_rewritten)
        {
            ctx->Rip = *(DWORD64 *)(ULONG_PTR)ctx->Rsp;
            ctx->Rsp += 8;
            ctx->Rax = ret;
        }
    }

    emu_current_trap_ctx = prev_trap_ctx;
    emu_trap_ctx_rewritten = prev_ctx_rewritten;

    /* Ending the run is how a guest ExitThread unwinds; see emu_ExitThread. */
    if (guest_exit_requested) status = STATUS_THREAD_IS_TERMINATING;
    /* ...and how an unhandled guest raise surfaces (dispatch_guest_raise);
     * the record itself waits in guest_exc_rec for the run's PE caller.
     * ...and how a guest language handler's RtlUnwindEx returns without
     * returning (guest_request_unwind): the walk that entered the handler owns
     * the unwind, and it is one native return below this run.  Both spell
     * "this run ended on an exceptional event" the same way to the unix side,
     * because which event it was is decided here, where both flags live. */
    else if ((guest_exc_pending || guest_unwind_run_end) && !status)
        status = STATUS_EMU_GUEST_EXCEPTION;

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
                /* `entry` is where this run STARTED, not where the guest
                 * died -- any failure that named a faulting guest RIP was
                 * already ERR'd (with that RIP) on the unix side of
                 * unix_emu_run_entry, and any unhandled guest exception was
                 * already reported, with its own address, by
                 * raise_pending_guest_exception() above.  Reaching here means
                 * neither happened: say plainly that this message names the
                 * run's start, not its fault, so it is not misread as "the
                 * entry point itself is bad" (see CATALOG.md item 2). */
                ERR( "guest run started at AMD64 entry point %p ended without completing, status %08x "
                     "(entry is the run's start address, not necessarily where the guest faulted)\n",
                     entry, (UINT)status );
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
