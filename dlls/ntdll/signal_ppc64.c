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
};

#define THUNK_INFO_VERSION   4
#define THUNK_SIG_ARGC(sig)  ((sig) & 0xff)   /* for a variadic, the FIXED count */
#define THUNK_SIG_VOID       0x100u           /* returns void */
#define THUNK_SIG_VARIADIC   0x200u           /* synthesise a va_list, see below */
#define THUNK_SIG_RESERVED   0xfffffc00u      /* must be zero */

#define THUNK_MAX_ARGS 16

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
    thunk_override_func  func;
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

    if (!name) return 0;
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
static ULONG_PTR call_guest_function( void *entry, void *arg )
{
    struct emu_run_entry_params params = { entry, arg, 0, emu_trap_dispatch };
    NTSTATUS status = WINE_UNIX_CALL( unix_emu_run_entry, &params );

    if (status)
    {
        ERR( "guest callback %p failed, status %08x\n", entry, (UINT)status );
        return 0;
    }
    return (ULONG_PTR)params.retval;
}


/***********************************************************************
 *           call_guest_tls_callback
 *
 * A PIMAGE_TLS_CALLBACK of a guest image, at process and thread attach and
 * detach.  The same native->guest direction as the atexit handlers, with one
 * wrinkle: a TLS callback takes three MS-x64 arguments and the run-entry
 * primitive carries exactly one (RCX).  So the one argument is a parameter
 * block, and a five-instruction guest thunk unpacks it and tail-jumps to the
 * real callback -- the tail jump leaves RSP exactly as run_entry aligned it,
 * so the callback returns straight into run_entry's own return path.
 *
 * The thunk is written once into anonymous executable memory.  It is only
 * ever entered through call_guest_function, so it needs no classification by
 * guest_module_from_address, and a freshly mapped page can have no stale
 * translation to invalidate.  Callers hold the loader lock, which serializes
 * the one-time setup.
 */
void call_guest_tls_callback( void *callback, void *module, UINT reason )
{
    static const BYTE thunk_code[] =
    {
        0x48, 0x8b, 0x01,        /* mov rax,[rcx]       callback */
        0x48, 0x8b, 0x51, 0x10,  /* mov rdx,[rcx+0x10]  reason */
        0x4c, 0x8b, 0x41, 0x18,  /* mov r8,[rcx+0x18]   reserved */
        0x48, 0x8b, 0x49, 0x08,  /* mov rcx,[rcx+0x08]  module */
        0xff, 0xe0,              /* jmp rax */
    };
    static void *thunk;
    struct
    {
        void      *callback;   /* 0x00 */
        void      *module;     /* 0x08 */
        ULONGLONG  reason;     /* 0x10 */
        void      *reserved;   /* 0x18 */
    } params = { callback, module, reason, NULL };

    if (!thunk)
    {
        void *mem = NULL;
        SIZE_T size = sizeof(thunk_code);
        NTSTATUS status = NtAllocateVirtualMemory( GetCurrentProcess(), &mem, 0, &size,
                                                   MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE );
        if (status)
        {
            ERR( "no memory for the guest TLS callback thunk, status %08x\n", (UINT)status );
            return;
        }
        memcpy( mem, thunk_code, sizeof(thunk_code) );
        thunk = mem;
    }
    TRACE( "callback %p module %p reason %u\n", callback, module, reason );
    call_guest_function( thunk, &params );
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
#define MAX_GUEST_ATEXIT 64

static void *guest_atexit_funcs[MAX_GUEST_ATEXIT];
static UINT  guest_atexit_count;

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
    if (guest_atexit_count >= MAX_GUEST_ATEXIT)
    {
        ERR( "more than %u guest atexit handlers\n", MAX_GUEST_ATEXIT );
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
    if (guest_atexit_count >= MAX_GUEST_ATEXIT)
    {
        ERR( "more than %u guest atexit handlers\n", MAX_GUEST_ATEXIT );
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
    /* native->guest: the pointer is queued here and run by our own native
     * handler at exit; see run_guest_atexit_handlers */
    { L"ucrtbase.dll", "_crt_atexit",       1, emu_crt_atexit },
    { L"msvcrt.dll",   "_crt_atexit",       1, emu_crt_atexit },
    { L"msvcrt.dll",   "atexit",            1, emu_crt_atexit },
    { L"msvcrt.dll",   "_onexit",           1, emu_onexit },
    { L"ucrtbase.dll", "_onexit",           1, emu_onexit },
    { L"kernel32.dll", "ExitThread",        1, emu_ExitThread },
    { L"kernelbase.dll", "ExitThread",      1, emu_ExitThread },
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
                                      struct com_thunk_hit *com )
{
    LIST_ENTRY *mark, *entry;
    void *ret = NULL;
    ULONG_PTR magic;

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
            TRACE( "%s.%s -> override (%u args)\n", debugstr_w(mod->BaseDllName.Buffer),
                   thunk_overrides[i].name, thunk_overrides[i].argc );
            *override = thunk_overrides[i].func;
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
static void marshal_thunk_args( const AMD64_CONTEXT *ctx, UINT argc, ULONG_PTR *a )
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
    thunk_override_func override = NULL;
    struct com_thunk_hit com = { 0 };
    NTSTATUS status = STATUS_SUCCESS;
    ULONG_PTR a[THUNK_MAX_ARGS] = { 0 };
    UINT sig = 0, argc;
    ULONG_PTR ret;
    void *proc;

    proc = find_guest_thunk_target( ctx->Rip, &sig, &override, &com );
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

        marshal_thunk_args( ctx, argc, a );
        if (sig & THUNK_SIG_VARIADIC)
        {
            /* the v-variant takes the fixed arguments plus one va_list */
            a[argc] = (ULONG_PTR)marshal_thunk_va_list( ctx, argc );
            argc++;
        }
        ret = override ? override( a, proc ) : call_native_thunk( proc, a );

        /* return to the guest's caller: the trap fired with Rip still on the
         * trap opcode and Rsp still pointing at the return address its CALL
         * pushed */
        ctx->Rip = *(DWORD64 *)(ULONG_PTR)ctx->Rsp;
        ctx->Rsp += 8;
        ctx->Rax = ret;
    }

    /* Ending the run is how a guest ExitThread unwinds; see emu_ExitThread. */
    if (guest_exit_requested) status = STATUS_THREAD_IS_TERMINATING;

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
