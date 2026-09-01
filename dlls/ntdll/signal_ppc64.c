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
#include "wine/emu_qpc.h"
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

/* Every other per-thread variable in this file is initial-exec too, for the
 * cost and not the semantics: the default global-dynamic model is one
 * __tls_get_addr_opt call per access, paid on the per-crossing path
 * (emu_trap_dispatch touches several of these on every trap), and measured at
 * 3.6% of the GameThread on 2026-08-27.  The model costs no static TLS:
 * ppc64_current_teb above already commits this module's whole PT_TLS block to
 * the static area at load, so the attribute only changes the access sequence.
 * What DOES stay bounded is the block's SIZE -- see the 1232-byte lesson at
 * struct thread_data::emu_guest_ctx in unix/unix_private.h before adding a
 * large variable here. */
#define EMU_THREAD_VAR __thread __attribute__((tls_model("initial-exec")))

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

/* Say WHERE a rejected frame actually is, not just that it is not between the
 * TEB's two fields.
 *
 * "invalid frame X (limit-base)" tells you the frame is outside the stack the
 * TEB currently describes.  It does not tell you which stack it IS on, and on
 * this port that is the whole question: a thread running guest code has two --
 * the native ppc64 one and the guest one -- and the TEB describes whichever
 * machine is executing (see dlls/ntdll/unix/loader.c).  A frame that is on the
 * native stack while the TEB describes the guest stack means the dispatcher
 * was entered without the bounds being switched back; a frame in neither means
 * something else entirely.
 *
 * NtQueryVirtualMemory answers that without needing anything from the unix
 * side: the allocation base and size name the mapping the frame belongs to, so
 * comparing it against the TEB's own region distinguishes the two cases.
 * [MEASURED] 2026-08-18: DOOM produced `invalid frame 3fff490ff730
 * (00003FF044032000-00003FF044130000)` -- a frame in the 0x3fff.. range while
 * the TEB described a megabyte at 0x3ff044.., which are different mappings
 * entirely, and this is what says so out loud. */
static void report_invalid_frame( ULONG64 frame )
{
    MEMORY_BASIC_INFORMATION info;
    SIZE_T len = 0;
    NT_TIB *tib = (NT_TIB *)NtCurrentTeb();

    if (!NtQueryVirtualMemory( GetCurrentProcess(), (void *)(ULONG_PTR)frame,
                               MemoryBasicInformation, &info, sizeof(info), &len ) &&
        len >= sizeof(info))
    {
        ERR( "  frame %I64x is in the mapping %p+%Ix (state %04x protect %04x); "
             "the TEB describes %p-%p, which is %s mapping\n",
             frame, info.AllocationBase, (SIZE_T)info.RegionSize,
             (UINT)info.State, (UINT)info.Protect,
             tib->StackLimit, tib->StackBase,
             ((char *)tib->StackLimit >= (char *)info.AllocationBase &&
              (char *)tib->StackLimit <  (char *)info.AllocationBase + info.RegionSize)
                 ? "the SAME" : "a DIFFERENT" );
    }
    else ERR( "  frame %I64x is in no mapping at all\n", frame );

    /* And what the TEB's own stack looks like, so the two can be compared by
     * SIZE.  This port mirrors a thread's guest stack onto its native reserve,
     * so a thread's two stacks are the same size -- if these two differ, they
     * are not the two stacks of one thread and the unwind has left this
     * thread's stack entirely. */
    if (!NtQueryVirtualMemory( GetCurrentProcess(), tib->StackLimit,
                               MemoryBasicInformation, &info, sizeof(info), &len ) &&
        len >= sizeof(info))
        ERR( "  the TEB's stack is in the mapping %p+%Ix, dealloc %p, "
             "reserve %Ix\n", info.AllocationBase, (SIZE_T)info.RegionSize,
             NtCurrentTeb()->DeallocationStack,
             (SIZE_T)((char *)tib->StackBase - (char *)NtCurrentTeb()->DeallocationStack) );

    /* AND WHAT THE PORT'S OWN RUN-LEVEL BOOKKEEPING SAYS, because the two
     * questions "is the frame in the stack the TEB names" and "does the TEB
     * agree with the run this thread is actually on" are different questions,
     * and the first ERR above can only ask the first one.
     *
     * emu_guest_teb_stack (dlls/ntdll/unix/loader.c) is what a fiber switch
     * writes to and what emu_teb_stack_switch() copies into the TEB every
     * time guest code resumes; unixcall_emu_fiber_stack's QUERY op reads it
     * back without disturbing it.  If it AGREES with the TEB (below) and the
     * frame is still rejected, the bookkeeping itself is stale -- something
     * upstream of here (a switch, a run's entry/exit) failed to update BOTH
     * copies together, and the fault is not in this dispatcher at all.  If it
     * DISAGREES with the TEB, something wrote Tib.StackBase/StackLimit
     * directly without going through emu_teb_stack_switch(), which is a
     * narrower, more findable bug.  HasFiberData says whether a Fiber API was
     * even in use on this thread at the time -- FALSE would rule fibers out
     * for this occurrence outright, the way a debugger's first question
     * ("were you on a fiber?") cannot currently be answered from a log alone.
     *
     * A guest-side [MEASURED] callout awaits the NEXT run this fires on:
     * as of 2026-08-29 no synthetic probe (same-thread fiber fault,
     * cross-thread fiber theft, a fiber switch performed from inside a
     * nested/native-invoked run, a hand-rolled RSP swap with no Fiber API at
     * all, and a recursive vectored-handler exception storm) reproduces this
     * report at all -- every one of them reaches the ordinary "unhandled at
     * guest level; re-raising natively" report instead.  This block exists so
     * that if Cyberpunk 2077's own job system trips it again, the log says
     * which of the two questions above is the one with the surprising
     * answer, instead of requiring a live debugger session to find out. */
    {
        struct emu_fiber_params fiber_stack = { EMU_FIBER_QUERY };

        if (!WINE_UNIX_CALL( unix_emu_fiber_stack, &fiber_stack ) && fiber_stack.base)
            ERR( "  the port's own run bookkeeping says base %p limit %p dealloc %p, "
                 "which %s the TEB above; this thread %s fiber data (Tib.FiberData %p)\n",
                 fiber_stack.base, fiber_stack.limit, fiber_stack.dealloc,
                 (fiber_stack.base == tib->StackBase && fiber_stack.limit == tib->StackLimit &&
                  fiber_stack.dealloc == NtCurrentTeb()->DeallocationStack) ? "AGREES WITH" : "DISAGREES WITH",
                 NtCurrentTeb()->HasFiberData ? "HAS" : "has no",
                 NtCurrentTeb()->Tib.FiberData );
        else
            ERR( "  the port's own run bookkeeping has no guest stack recorded "
                 "(no guest run is active on this thread); this thread %s fiber data\n",
                 NtCurrentTeb()->HasFiberData ? "HAS" : "has no" );
    }
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
            report_invalid_frame( dispatch.EstablisherFrame );
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


static LDR_DATA_TABLE_ENTRY *guest_module_entry_from_address( const void *addr );

/***********************************************************************
 *           module_entry_from_address
 *
 * The loader entry containing an address, whatever machine it is for.
 * guest_module_entry_from_address() asks the same question of AMD64 images
 * only; this one is for diagnostics that must name a NATIVE caller.
 */
static LDR_DATA_TABLE_ENTRY *module_entry_from_address( const void *addr )
{
    LIST_ENTRY *mark, *entry;

    if (!addr) return NULL;
    mark = &NtCurrentTeb()->Peb->LdrData->InMemoryOrderModuleList;
    for (entry = mark->Flink; entry != mark; entry = entry->Flink)
    {
        LDR_DATA_TABLE_ENTRY *mod = CONTAINING_RECORD( entry, LDR_DATA_TABLE_ENTRY, InMemoryOrderLinks );
        const IMAGE_NT_HEADERS *nt = RtlImageNtHeader( mod->DllBase );
        const char *base = (const char *)mod->DllBase;

        if (!nt) continue;
        if ((const char *)addr >= base && (const char *)addr < base + nt->OptionalHeader.SizeOfImage)
            return mod;
    }
    return NULL;
}


/***********************************************************************
 *           report_native_pc_in_guest_image
 *
 * THE NATIVE CPU EXECUTING GUEST CODE, which is the one thing this port must
 * never let happen and the hardest failure to recognise from a log.
 *
 * A ppc64 core turned loose on x86-64 bytes does not stop.  It decodes them as
 * ppc64 instructions and runs them, and the first one that touches memory
 * raises an access violation somewhere meaningless -- so the report says "a
 * read of 0x44 at an address inside the game", which reads exactly like the
 * game dereferencing a null pointer, and sends the investigation after the
 * game's own bugs.  DOOM (2016) spent this port a fortnight there.
 *
 * Two things separate the cases, and both are checked rather than argued:
 *
 *   - The faulting pc is INSIDE a loaded AMD64 image.  Guest code never
 *     executes natively, so a native pc there is already the whole finding.
 *   - It is four-byte aligned, because it is a ppc64 program counter.  An x86
 *     pc reported for a guest fault lands wherever the instruction boundary
 *     is, and a "mid-instruction" pc that is nevertheless a multiple of four,
 *     twice running, is the signature of this and of nothing else.
 *
 * What is printed is what identifies the culprit: the branch was made either
 * by `bctrl` (through CTR) or `bl` (leaving LR at the return address), so CTR
 * and LR between them name the call site, and LR is resolved to module+offset
 * because that is the line that says which native function called a guest
 * pointer without going through the emulator.
 */
static void report_native_pc_in_guest_image( EXCEPTION_RECORD *rec, CONTEXT *context )
{
    LDR_DATA_TABLE_ENTRY *guest, *caller;
    static LONG reported;

    if (InterlockedIncrement( &reported ) > 4) return;

    if (!(guest = guest_module_entry_from_address( (void *)(ULONG_PTR)context->Iar )))
    {
        /* An ordinary NATIVE fault, which this function has nothing to say
         * about except the one thing the bare address does not: which module
         * it is in, and where it was called from.  Printed here rather than
         * left to a debugger because a guest process on this port frequently
         * has no usable one -- see [[misleading-crash-reports]] -- and
         * because the module+offset is what addr2line takes. */
        LDR_DATA_TABLE_ENTRY *at = module_entry_from_address( (void *)(ULONG_PTR)context->Iar );
        LDR_DATA_TABLE_ENTRY *from = module_entry_from_address( (void *)(ULONG_PTR)context->Lr );

        if (at)
            ERR( "native fault at %s+%I64x (nip %I64x)\n", debugstr_w(at->BaseDllName.Buffer),
                 context->Iar - (ULONG64)(ULONG_PTR)at->DllBase, context->Iar );
        else
            ERR( "native fault at nip %I64x, which is in no loaded module\n", context->Iar );
        if (from)
            ERR( "  called from lr=%I64x = %s+%I64x (r3=%I64x r4=%I64x r5=%I64x sp=%I64x)\n",
                 context->Lr, debugstr_w(from->BaseDllName.Buffer),
                 context->Lr - (ULONG64)(ULONG_PTR)from->DllBase,
                 context->Gpr3, context->Gpr4, context->Gpr5, context->Gpr1 );
        else
            ERR( "  called from lr=%I64x, which is in no loaded module (r3=%I64x r4=%I64x "
                 "r5=%I64x sp=%I64x)\n", context->Lr, context->Gpr3, context->Gpr4,
                 context->Gpr5, context->Gpr1 );
        return;
    }

    ERR( "NATIVE ppc64 execution has branched INTO GUEST CODE: nip=%I64x = %s+%I64x, "
         "which the native cpu is decoding as ppc64 instructions -- the reported fault "
         "(%08x touching %p) is what those decoded to, not what the guest did\n",
         context->Iar, debugstr_w(guest->BaseDllName.Buffer),
         context->Iar - (ULONG64)(ULONG_PTR)guest->DllBase,
         (UINT)rec->ExceptionCode,
         rec->NumberParameters >= 2 ? (void *)rec->ExceptionInformation[1] : NULL );

    caller = module_entry_from_address( (void *)(ULONG_PTR)context->Lr );
    if (caller)
        ERR( "  the branch came from lr=%I64x = %s+%I64x (ctr=%I64x, r12=%I64x): that call "
             "site handed a guest address to the native cpu\n", context->Lr,
             debugstr_w(caller->BaseDllName.Buffer),
             context->Lr - (ULONG64)(ULONG_PTR)caller->DllBase, context->Ctr, context->Gpr12 );
    else
        ERR( "  the branch came from lr=%I64x, which is in no loaded module (ctr=%I64x, "
             "r12=%I64x)\n", context->Lr, context->Ctr, context->Gpr12 );

    ERR( "  args as ppc64 would have them: r3=%I64x r4=%I64x r5=%I64x r6=%I64x r7=%I64x "
         "r8=%I64x sp=%I64x\n", context->Gpr3, context->Gpr4, context->Gpr5, context->Gpr6,
         context->Gpr7, context->Gpr8, context->Gpr1 );
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
    NTSTATUS status;

    /* In a WoW64 process, wow64.dll gets the exception before native
     * dispatch, exactly as the aarch64 and x86-64 dispatchers hand it over:
     * it forwards to the CPU backend's BTCpuResetToConsistentState.  On this
     * backend that is a stated no-op -- a guest fault arrives here with the
     * guest state already parked in the TEB cpu area -- so the call is kept
     * for the contract, not for work it does today. */
    if (pWow64PrepareForException) pWow64PrepareForException( rec, context );

    /* WHICH STACK the interrupted code was on, BEFORE dispatch is even
     * attempted and for EVERY exception code, not only EXCEPTION_ACCESS_
     * VIOLATION.
     *
     * [MEASURED] 2026-08-29, Cyberpunk 2077 (Steam): this dispatcher used to
     * ask that question only inside the access-violation arm below, on the
     * theory that a fault taken with sp off the Win32 stack would always be
     * one this port's own report_guest_access_violation()/emu_trap_dispatch
     * machinery had already named.  It is not: this run reached
     * KiUserExceptionDispatcher with an exception code that never went
     * through that arm at all, and the FIRST anyone learned the interrupted
     * sp was not on this thread's registered stack was three ERR lines
     * later, inside call_seh_handlers/report_invalid_frame, phrased only as
     * "invalid frame X (limit-base)" -- true, but it does not say WHY, and by
     * then the process is already on its way to NtTerminateProcess.
     *
     * call_user_exception_dispatcher builds the dispatcher's frame on
     * context->Gpr1, and call_seh_handlers then validates every unwound
     * frame against the TEB's Win32 bounds -- so a fault taken while the
     * emulator's JIT is running on some OTHER native stack (its own
     * dispatch/trampoline stack, not the Win32 one this thread was given)
     * produces a Gpr1 that can never walk to a valid frame, for ANY
     * exception code the host signal handler turns into one of these.  This
     * is that same question, asked immediately and unconditionally, so the
     * answer is on record before the walk that depends on it fails. */
    {
        static LONG stack_reported;

        if (InterlockedIncrement( &stack_reported ) <= 8)
        {
            NT_TIB *tib = (NT_TIB *)NtCurrentTeb();
            BOOL on_win32_stack = ((char *)context->Gpr1 >= (char *)tib->StackLimit &&
                                   (char *)context->Gpr1 <  (char *)tib->StackBase);

            ERR( "KiUserExceptionDispatcher: code=%08x interrupted sp=%p is on the %s "
                 "stack (TEB %p-%p)%s\n", (UINT)rec->ExceptionCode,
                 (void *)(ULONG_PTR)context->Gpr1, on_win32_stack ? "WIN32" : "UNIX/other",
                 tib->StackLimit, tib->StackBase,
                 on_win32_stack ? "" : " -- this looks like a fault inside the emulator's "
                 "own execution, not a Windows exception; the walk below is expected to fail" );
        }
    }

    /* WHAT AN ACCESS VIOLATION WAS ACTUALLY TOUCHING.
     *
     * The guest's own crash reporter cannot be trusted for this -- DOOM's
     * writes an all-zero register block with EFlags holding a native pointer
     * fragment -- and +seh answers it only inside 22 GB of trace.  So the two
     * fields that identify the fault are reported here directly: info[0] is
     * 0 for a read, 1 for a write, 8 for an execute, and info[1] is the
     * address that could not be touched.
     *
     * Bounded, because a guest that catches its own faults can raise thousands:
     * the first few are what identify the bug, and the rest are the same one. */
    if (rec->ExceptionCode == EXCEPTION_ACCESS_VIOLATION && rec->NumberParameters >= 2)
    {
        static LONG reported;

        report_native_pc_in_guest_image( rec, context );
        if (InterlockedIncrement( &reported ) <= 8)
            ERR( "access violation at %p: %s %p (info[0]=%Ix info[1]=%Ix)\n",
                 rec->ExceptionAddress,
                 rec->ExceptionInformation[0] == 0 ? "reading" :
                 rec->ExceptionInformation[0] == 1 ? "writing" : "executing",
                 (void *)rec->ExceptionInformation[1],
                 (SIZE_T)rec->ExceptionInformation[0],
                 (SIZE_T)rec->ExceptionInformation[1] );
    }

    status = dispatch_exception( rec, context );
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
    /* The context rides along as a FOURTH argument, exactly as the x86-64
     * dispatcher passes it in r9 and the aarch64 one in x3: an ordinary
     * 3-argument APC routine never looks at r6, but wow64.dll's
     * Wow64ApcRoutine -- which is what every APC queued for 32-bit code
     * resolves to (apc_32to64) -- is defined to receive the interrupted
     * 64-bit CONTEXT there, and wow64_NtContinueEx restores it to end the
     * APC.  Without it the wow64 APC path reads a garbage register. */
    void (WINAPI *func_with_context)( ULONG_PTR, ULONG_PTR, ULONG_PTR, CONTEXT * ) = (void *)func;

    func_with_context( arg1, arg2, arg3, context );
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
            report_invalid_frame( dispatch.EstablisherFrame );
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
 * This used to return FALSE unconditionally -- "the PF_* numbers are all
 * x86/ARM specific; none describes a PowerPC capability".  True about the
 * HOST and wrong about the CALLERS that matter: an x86 guest reaches this
 * through the kernelbase thunk, and witcher3.exe read the unconditional
 * FALSE as "CPU does not meet minimal requirements.  Support for SSE3
 * instructions is required." over a POWER8 emulating SSE4.2 ([MEASURED]
 * 2026-08-19).  Read the shared feature array like every other
 * architecture; dlls/ntdll/unix/system.c's ppc64 init seeds it with what
 * the emulation lane actually provides, and a query this port has no
 * answer for stays FALSE there.
 */
BOOLEAN WINAPI RtlIsProcessorFeaturePresent( UINT feature )
{
    return feature < PROCESSOR_FEATURE_MAX && user_shared_data->ProcessorFeatures[feature];
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
    UINT widths_rva;      /* count argument-width descriptors, two bits per
                             argument, see THUNK_WIDTH below.  Added at
                             version 6, by the same rule as fp_rva. */
    UINT signs_rva;       /* count signedness descriptors, ONE bit per
                             argument: that sub-word argument is SIGNED and
                             must be sign-extended rather than zero-extended.
                             Added at version 7, by the same rule again. */
    UINT geom32_rva;      /* count i386 stdcall frame words, see THUNK_GEOM32_*
                             below.  Added at version 8; all-zero words in an
                             x86_64 module (the field exists so one version
                             describes both guest halves), measured words in
                             an i386 one -- and 0 there is a REFUSAL the
                             dispatcher must honour, never "one slot each",
                             because a frame it cannot decode is also a frame
                             it cannot pop. */
};

#define THUNK_INFO_VERSION   8
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
 * The argument-width descriptor (version 6), one UINT per export.
 * ---------------------------------------------------------------------------
 * THE MASK ABOVE SAYS ONLY "NARROWER THAN EIGHT BYTES", and the host read that
 * as "narrow to four".  For a DWORD that is exactly right and it is what the
 * mask was written for.  For a WORD, a BYTE, a BOOLEAN or a WCHAR it is not:
 * MS-x64 requires only the low bits of a sub-word argument to be meaningful,
 * and real compilers leave the rest alone -- clang emits `mov cx, 0xffff` for
 * a WORD argument, so RCX keeps whatever its upper 48 bits held before.
 *
 * MEASURED, end to end, and it was live: a guest calling
 * GetActiveProcessorCount(ALL_PROCESSOR_GROUPS) reached native kernel32 with
 * group = 0xffbdffff.  The `group == ALL_PROCESSOR_GROUPS` test therefore
 * failed, control fell through to the arm that treats the argument as a group
 * INDEX, and the call answered 0 where Windows answers the processor count.
 * A program sizing an array from that gets a zero-length allocation.  Nothing
 * crashes; a number is simply wrong, which is the failure class this port
 * exists to keep out.  GetActiveProcessorCount(0) was correct throughout,
 * which is why this hid: only the all-bits-set argument exposed it.
 *
 * Two bits per argument, so sixteen arguments fill the word exactly:
 *
 *   0  eight bytes -- leave it alone (a pointer, a HANDLE, a 64-bit integer)
 *   1  four bytes
 *   2  two bytes
 *   3  one byte
 *
 * Zero is therefore also "no narrowing anywhere", which is what an export
 * whose widths the generator's oracle could not measure gets -- exactly the
 * behaviour those entries had before this word existed.  The narrow mask
 * stays as it is and stays checked: it is the same measurement at lower
 * resolution, and keeping both lets the generator's own checker cross the
 * two against each other. */
#define THUNK_WIDTH(w, i)    (((w) >> ((i) * 2u)) & 3u)

/* ---------------------------------------------------------------------------
 * The signedness descriptor (version 7), one UINT per export.
 * ---------------------------------------------------------------------------
 * The width word says how many of the guest's bits are real.  It does not say
 * what the rest should BECOME, and the two ABIs put that question on opposite
 * sides of the call: MS-x64 leaves a sub-word argument's upper bits undefined
 * and makes ignoring them the callee's problem, while ELFv2 makes extension
 * the CALLER's job and ties the KIND of extension to the argument's declared
 * type -- sign for a SHORT, zero for a WORD.  A ppc64 callee compiled at -O2
 * is entitled to skip re-extending, so getting the kind wrong is not caught
 * downstream.
 *
 * Zero-extending everything, which is what this host did when the width word
 * was first added, turns a SHORT of -1 into 65535 on the way in.  That is the
 * same wrong-number class as the GetActiveProcessorCount case above and just
 * as quiet -- the call returns, the value is nonsense.
 *
 * One bit per argument, and only bits whose width is 1 or 2 bytes can be set:
 * a four-byte argument is narrowed with a cast to UINT and re-read as 32 bits
 * by the callee either way, and an eight-byte one is not narrowed at all.
 * Zero is "nothing signed", which is what every export the generator's sign
 * oracle could not measure gets -- exactly the treatment they had before this
 * word existed.
 *
 * libs/winecom carries the same bit per COM vtable slot
 * (winecom_slot::narrowsign, measured on IWMSyncReader::GetStreamSelected).
 * This is the flat-surface half; the two lanes agree deliberately. */
#define THUNK_SIGNED(s, i)   (((s) >> (i)) & 1u)

/* -> the argument as its declared width, extended as its type demands.
 *
 * The 32-bit case stays UNSIGNED whatever the sign word says, and that is not
 * an oversight.  The PE side of this port is LP64: a negative 32-bit value
 * has always reached an LP64 `long` parameter zero-extended, from before any
 * of these descriptors existed, and everything built against that behaviour
 * expects it.  Sub-word arguments have no such history -- they were being
 * handed over with stack garbage in the upper bits until version 6 -- so
 * there is nothing to preserve and the ABI's own rule can simply be followed.
 */
static inline ULONG_PTR narrow_thunk_arg( ULONG_PTR v, UINT width, UINT sign )
{
    switch (width)
    {
    case 1: return (UINT)v;
    case 2: return sign ? (ULONG_PTR)(LONG_PTR)(SHORT)v : (ULONG_PTR)(USHORT)v;
    case 3: return sign ? (ULONG_PTR)(LONG_PTR)(signed char)v : (ULONG_PTR)(BYTE)v;
    default: return v;
    }
}

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

/* ---------------------------------------------------------------------------
 * The i386 frame word (version 8), one UINT per export of an i386 module.
 * ---------------------------------------------------------------------------
 * The width word above answers "how many of the bits are real" for the
 * x86-64 guest and cannot answer the i386 frame question: its 8-byte class
 * names both a pointer (ONE 4-byte stdcall slot there) and a 64-bit integer
 * (TWO), and a dispatcher that cannot tell them apart cannot walk the stack
 * -- or perform the stdcall pop, which is wrong-Esp-forever, not merely a
 * wrong argument.  So the i386 half of a module carries its own word,
 * measured by the same clang oracle at the i386 target, where sizeof IS the
 * slot answer.  See the GEOM32 banner in tools/spec2thunk/spec2thunk. */
#define THUNK_GEOM32_VALID(g)     ((g) & 0x80000000u)
#define THUNK_GEOM32_SLOTS(g)     ((g) & 0xffu)          /* total 4-byte slots */
#define THUNK_GEOM32_QWORD(g, i)  (((g) >> (8 + (i))) & 1u)  /* arg i: 2 slots */
#define THUNK_GEOM32_RET_QWORD(g) ((g) & 0x01000000u)    /* EDX:EAX return */

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
    UINT                 cb_argc;  /* how many arguments the CALLBACK itself
                                      takes -- 4 through 9, and 0 means the 4 that
                                      every row carried before this field
                                      existed.  Not the same number as `argc`
                                      above, which counts the arguments of the
                                      REGISTERING export: _set_invalid_parameter_
                                      handler takes one argument and that
                                      argument is a five-argument callback.  It
                                      has to be per row for the same reason the
                                      width does: the pool keys a slot on
                                      (target, width, arity) and puts the guest
                                      target in the register one past the last
                                      real argument, so a slot minted at the
                                      wrong arity does not fail -- it overwrites
                                      a live argument register with the target
                                      pointer and passes the callback one
                                      argument too few. */
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
/* the crossing-frequency sink, defined below the callback pool it counts */
static void xstat_event( ULONG_PTR ev, const char *name );

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

/***********************************************************************
 *           Vulkan entry points, vended at runtime like GL's
 *
 * vkGetInstanceProcAddr and vkGetDeviceProcAddr are wglGetProcAddress's
 * problem in Vulkan clothes: the ANSWER is a code address the guest will
 * CALL, and pass-through hands it native winevulkan code.  MEASURED
 * 2026-08-22, Quake II (2023 remaster): SDL2's SDL_Vulkan_LoadLibrary got
 * vkGetInstanceProcAddr from the guest vulkan-1 thunk (a fine guest stub),
 * called it, received the RAW NATIVE pointer the pass-through returned, and
 * called that -- c0000005 at winevulkan.dll+0x2bf68, ppc64 bytes decoded as
 * x86-64, "in no guest image".  A title that statically imports vulkan-1
 * never sees this; every title that RESOLVES vulkan at runtime -- SDL,
 * every loader library, DXVK as a guest DLL -- does.
 *
 * Easier than GL in one way: there is an export table to answer from.  The
 * guest vulkan-1 thunk module exports the whole surface the oracle accepted
 * (253 names, vkCreateWin32SurfaceKHR and the other extensions included),
 * and native vulkan-1.dll exports the same names as forwards into
 * winevulkan -- so the stub is an ordinary LdrGetProcedureAddress against
 * the guest module, and no thunk_resolvers[] entry is needed.  WHETHER a
 * name may be had stays the native side's decision, exactly as with GL: it
 * is asked first, and only a name it answers for gets a stub.  A name it
 * offers that the guest module lacks is a gap in THIS port and is an ERR
 * naming it, once (gl_say_once serves both APIs; the namespaces do not
 * collide).
 *
 * One handler serves both registration points: both are (handle, name) with
 * the name in slot 1, and `native` is the export the row stands in for, so
 * the probe call is the right one either way.
 *
 * KNOWN GAP, deliberate: the guest module's surface is vulkan-1.spec's, not
 * winevulkan's.  winevulkan answers ~745 names through these two entry
 * points -- every KHR alias and EXT included -- and the guest thunk carries
 * stubs for the ~262 vulkan-1.spec exports, so a name outside that set
 * (vkGetPhysicalDeviceProperties2KHR, say, from DXVK loaded as a guest DLL)
 * answers NULL here, with the ERR above naming it once.  That is the
 * refuse-and-name trade this tree makes everywhere: pass-through's answer
 * for the same name was a raw native pointer and a c0000005 in no guest
 * image.  Widening the answer is generation-time work in
 * dlls/vulkan-1/vulkan-1.thunks, not a runtime patch here. */
static ULONG_PTR emu_vkGetProcAddr( const ULONG_PTR *a, void *native )
{
    const char *name = (const char *)a[1];
    ANSI_STRING str;
    HMODULE guest;
    void *proc;

    if (!native)
    {
        ERR( "no native vulkan-1 to answer vkGet*ProcAddr\n" );
        return 0;
    }
    if (!name) return 0;

    if (!((ULONG_PTR (*)( ULONG_PTR, ULONG_PTR ))native)( a[0], a[1] ))
    {
        if (gl_say_once( name ))
            WARN( "vkGet*ProcAddr(%s): not offered natively; NULL\n", debugstr_a(name) );
        return 0;
    }
    if (!(guest = find_guest_module( L"vulkan-1.dll" )))
    {
        ERR( "vkGet*ProcAddr(%s) with no guest vulkan-1 loaded\n", debugstr_a(name) );
        return 0;
    }
    RtlInitAnsiString( &str, name );
    if (LdrGetProcedureAddress( guest, &str, 0, &proc ))
    {
        if (gl_say_once( name ))
            ERR( "vkGet*ProcAddr(%s): native vulkan-1 offers it but the guest "
                 "thunk module has no stub -- refused at generation time; NULL "
                 "(see dlls/vulkan-1/vulkan-1.thunks)\n", debugstr_a(name) );
        return 0;
    }
    TRACE( "vkGet*ProcAddr(%s) -> guest stub %p\n", debugstr_a(name), proc );
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
static EMU_THREAD_VAR UINT guest_run_depth;

/* Set by an RtlUnwindEx that belongs to a guest language handler running in a
 * nested run: the run must END so that the frame walk which started it can
 * perform the unwind against the FAULTING stack, and this is what makes
 * emu_trap_dispatch end it.  Cleared here, by the initiator of that run. */
static EMU_THREAD_VAR BOOL guest_unwind_run_end;

/* The guest stack of the last nested run that ended for an unwind, handed
 * over unfreed by the unix side (emu_run_entry_params.kept_stack) because the
 * request's record may point into it.  One slot, because it is consumed
 * immediately: call_guest_handler_run() moves it into the request's
 * guest_unwind_target, and the frame walk sweeps it after every funclet it
 * runs (the collided road has no guest_handler_call to carry it). */
static EMU_THREAD_VAR void *guest_kept_run_stack;

static void free_guest_run_stack( void *addr )
{
    SIZE_T size = 0;

    if (!addr) return;
    NtFreeVirtualMemory( GetCurrentProcess(), &addr, &size, MEM_RELEASE );
}

static ULONG_PTR call_guest_function( void *entry, void *arg )
{
    struct emu_run_entry_params params = { entry, arg, 0, emu_trap_dispatch };
    NTSTATUS status;

    params.exception_dispatcher = emu_exception_dispatch;
    xstat_event( XSTAT_EV_NESTED_RUN, "nested guest run (call_guest_function)" );
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
        /* the run's stack stays mapped until the unwind that was requested
         * from it has no more use for it; see guest_unwind_target.kept_stack */
        if (guest_kept_run_stack)
        {
            ERR( "a kept run stack %p was never claimed; freeing it late\n",
                 guest_kept_run_stack );
            free_guest_run_stack( guest_kept_run_stack );
        }
        guest_kept_run_stack = params.kept_stack;
        return 0;
    }

    if (status)
    {
        /* a guest exception the nested run could not consume propagates
         * natively from here -- the callback's native caller and the
         * unhandled machinery above it get their shot with their own
         * machine's context.  Does not return when one was pending, in which
         * case the run's stack is left mapped for the report's sake -- the
         * thread is on its way to a reported death. */
        raise_pending_guest_exception();
        ERR( "guest callback %p failed, status %08x\n", entry, (UINT)status );
        free_guest_run_stack( params.kept_stack );
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
 *           call_guest_function_args5 / call_guest_function_args6
 *
 * The five- and six-argument forms call_guest_function_args's own comment
 * says would go here: "A callback with stack arguments (five or more) would
 * need a thunk that builds a frame; nothing in the corpus has one, and this
 * is where it would go."  Two consumers now have one -- SetWindowSubclass /
 * RemoveWindowSubclass's SUBCLASSPROC (six arguments) and
 * InternetSetStatusCallback's INTERNET_STATUS_CALLBACK (five) -- both
 * integer/pointer-only, so neither of these carries floating-point handling;
 * see the FP note on call_native_thunk_fp if that ever changes.
 *
 * MS-x64 passes the fifth and sixth arguments on the STACK, above the 32
 * bytes of shadow space the first four reserve: at [rsp+0x28] and [rsp+0x30]
 * from the callee's own entry point, the same offsets marshal_thunk_args()
 * reads them at for the opposite direction (guest calling host) --
 * ctx->Rsp + 8 + i*8 for i=4,5, i.e. Rsp+0x28 and Rsp+0x30 there too.
 *
 * MEASURED, and the first cut of this comment was wrong.  It argued run_entry
 * leaves "room above the shadow space for nobody yet" and that a plain
 * write to [rsp+0x28]/[rsp+0x30] before the tail jump was therefore safe, no
 * sub-rsp of our own needed.  A ppc64le/shell/ gate exercising
 * SetWindowSubclass -> SendMessage instead produced:
 *
 *   Unhandled page fault on WRITE access to 00003FFF53E00000 at address
 *   00003FFF53E10013
 *
 * PC (00003FFF53E10013) is this thunk's own code, offset 0x13 -- exactly the
 * `mov [rsp+0x28],r10` instruction.  The faulting address is
 * PC_thunk_base - 0x10000, i.e. nowhere near this code; it is where RSP's
 * OWN valid page ends.  0x10013 - 0x13 = 0x10000 confirms the fault address
 * is page-aligned, and 0x53E00000 minus the write's own +0x28 offset puts
 * RSP at exactly base-0x28: run_entry reserves precisely the 0x28 bytes a
 * four-argument call needs (a return address plus the four-register shadow
 * space, offsets 0x00-0x27) and NOT ONE BYTE MORE -- the page immediately
 * above that is unmapped.  So the four-argument thunk's own comment about
 * "room above the shadow space" was describing memory that was never there;
 * it merely never got read that far.
 *
 * The fix is the shape dlls/comctl32/comctl32.thunks originally sketched
 * (build a frame) with a smaller footprint than that sketch used (no CALL,
 * no RET -- still one tail jump): save the return address run_entry placed
 * at [rsp+0], SUB RSP into the stack's own (downward, and -- empirically --
 * spacious) free region, restore the return address at the NEW [rsp+0], and
 * only then write a4 (and a5) at the new frame's +0x28 (and +0x30).  Every
 * offset this thunk touches after the sub is AT OR BELOW the original RSP,
 * never above it -- the one direction this crash proved is actually backed
 * by mapped memory.  Machine-verified with objdump -D -b binary
 * -m i386:x86-64 -M intel against the exact byte arrays below, not reasoned
 * about by eye: both disassemble to precisely the intended instructions, in
 * the intended order, with the intended ModRM/SIB/displacement encoding for
 * every [rsp+disp8] operand (mod=01, rm=100 forces the SIB byte; a dropped
 * SIB is the classic way this silently becomes a different addressing mode,
 * and objdump is how this pass confirmed none is missing).
 */
static ULONG_PTR call_guest_function_args5( void *fn, ULONG_PTR a0, ULONG_PTR a1,
                                            ULONG_PTR a2, ULONG_PTR a3, ULONG_PTR a4 )
{
    static const BYTE thunk_code[] =
    {
        0x4c, 0x8b, 0x1c, 0x24,        /* mov r11,[rsp]        save run_entry's return addr */
        0x48, 0x83, 0xec, 0x40,        /* sub rsp,0x40         into the stack's own free space below */
        0x4c, 0x89, 0x1c, 0x24,        /* mov [rsp],r11        return addr back at the NEW frame's +0x00 */
        0x48, 0x8b, 0x01,              /* mov rax,[rcx]       target */
        0x48, 0x8b, 0x51, 0x10,        /* mov rdx,[rcx+0x10]  a1 */
        0x4c, 0x8b, 0x41, 0x18,        /* mov r8,[rcx+0x18]   a2 */
        0x4c, 0x8b, 0x49, 0x20,        /* mov r9,[rcx+0x20]   a3 */
        0x4c, 0x8b, 0x51, 0x28,        /* mov r10,[rcx+0x28]  a4 */
        0x4c, 0x89, 0x54, 0x24, 0x28,  /* mov [rsp+0x28],r10  a4's stack slot, new frame */
        0x48, 0x8b, 0x49, 0x08,        /* mov rcx,[rcx+0x08]  a0 */
        0xff, 0xe0,                    /* jmp rax */
    };
    static void *thunk;
    struct
    {
        void      *fn;    /* 0x00 */
        ULONG_PTR  a[5];  /* 0x08 0x10 0x18 0x20 0x28 */
    } params = { fn, { a0, a1, a2, a3, a4 } };

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

static ULONG_PTR call_guest_function_args6( void *fn, ULONG_PTR a0, ULONG_PTR a1,
                                            ULONG_PTR a2, ULONG_PTR a3, ULONG_PTR a4,
                                            ULONG_PTR a5 )
{
    static const BYTE thunk_code[] =
    {
        0x4c, 0x8b, 0x1c, 0x24,        /* mov r11,[rsp]        save run_entry's return addr */
        0x48, 0x83, 0xec, 0x40,        /* sub rsp,0x40         into the stack's own free space below */
        0x4c, 0x89, 0x1c, 0x24,        /* mov [rsp],r11        return addr back at the NEW frame's +0x00 */
        0x48, 0x8b, 0x01,              /* mov rax,[rcx]       target */
        0x48, 0x8b, 0x51, 0x10,        /* mov rdx,[rcx+0x10]  a1 */
        0x4c, 0x8b, 0x41, 0x18,        /* mov r8,[rcx+0x18]   a2 */
        0x4c, 0x8b, 0x49, 0x20,        /* mov r9,[rcx+0x20]   a3 */
        0x4c, 0x8b, 0x51, 0x28,        /* mov r10,[rcx+0x28]  a4 */
        0x4c, 0x89, 0x54, 0x24, 0x28,  /* mov [rsp+0x28],r10  a4's stack slot, new frame */
        0x4c, 0x8b, 0x51, 0x30,        /* mov r10,[rcx+0x30]  a5 */
        0x4c, 0x89, 0x54, 0x24, 0x30,  /* mov [rsp+0x30],r10  a5's stack slot, new frame */
        0x48, 0x8b, 0x49, 0x08,        /* mov rcx,[rcx+0x08]  a0 */
        0xff, 0xe0,                    /* jmp rax */
    };
    static void *thunk;
    struct
    {
        void      *fn;    /* 0x00 */
        ULONG_PTR  a[6];  /* 0x08 0x10 0x18 0x20 0x28 0x30 */
    } params = { fn, { a0, a1, a2, a3, a4, a5 } };

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
 *           call_guest_function_args7 / call_guest_function_args8 /
 *           call_guest_function_args9
 *
 * The seven-, eight- and nine-argument forms, for the callback rows the pool
 * refused until it had dispatchers for them: WINEVENTPROC and PENABLECALLBACK
 * at seven, PFNCALLBACK and LPCONDITIONPROC at eight, LPPROGRESS_ROUTINE at
 * nine.  The frame-building shape is exactly the one the five/six pair above
 * earned the hard way, one more MS-x64 stack slot per extra argument:
 * a4..a8 live at the callee's [rsp+0x28] through [rsp+0x48], so seven
 * arguments still fit the 0x40 frame and eight and nine take 0x50 -- the
 * eighth argument's slot is [rsp+0x40], one past the 0x40 frame's edge, and
 * 0x50 is the next multiple of 16, which keeps RSP congruent to the
 * alignment run_entry established.  Every write after the sub stays at or
 * below the original RSP, the one direction the five-argument crash proved
 * is actually mapped.
 *
 * Integer/pointer-only, all of them, in BOTH conventions -- which at nine
 * arguments is a sentence that has to be earned rather than assumed:
 * LPPROGRESS_ROUTINE's first four parameters are LARGE_INTEGERs BY VALUE,
 * and an 8-byte aggregate is passed as a plain 64-bit value in the
 * argument's own slot on MS-x64 and in the argument's own GPR on ELFv2.  No
 * XMM register, no FPR, no skipped GPR slot on either side; see the FP note
 * on call_native_thunk_fp if a float-bearing callback ever needs this pool.
 * Byte arrays machine-verified with llvm-mc against the exact encodings
 * below, the same discipline the five/six pair got from objdump.
 */
static ULONG_PTR call_guest_function_args7( void *fn, ULONG_PTR a0, ULONG_PTR a1,
                                            ULONG_PTR a2, ULONG_PTR a3, ULONG_PTR a4,
                                            ULONG_PTR a5, ULONG_PTR a6 )
{
    static const BYTE thunk_code[] =
    {
        0x4c, 0x8b, 0x1c, 0x24,        /* mov r11,[rsp]        save run_entry's return addr */
        0x48, 0x83, 0xec, 0x40,        /* sub rsp,0x40         into the stack's own free space below */
        0x4c, 0x89, 0x1c, 0x24,        /* mov [rsp],r11        return addr back at the NEW frame's +0x00 */
        0x48, 0x8b, 0x01,              /* mov rax,[rcx]       target */
        0x48, 0x8b, 0x51, 0x10,        /* mov rdx,[rcx+0x10]  a1 */
        0x4c, 0x8b, 0x41, 0x18,        /* mov r8,[rcx+0x18]   a2 */
        0x4c, 0x8b, 0x49, 0x20,        /* mov r9,[rcx+0x20]   a3 */
        0x4c, 0x8b, 0x51, 0x28,        /* mov r10,[rcx+0x28]  a4 */
        0x4c, 0x89, 0x54, 0x24, 0x28,  /* mov [rsp+0x28],r10  a4's stack slot, new frame */
        0x4c, 0x8b, 0x51, 0x30,        /* mov r10,[rcx+0x30]  a5 */
        0x4c, 0x89, 0x54, 0x24, 0x30,  /* mov [rsp+0x30],r10  a5's stack slot, new frame */
        0x4c, 0x8b, 0x51, 0x38,        /* mov r10,[rcx+0x38]  a6 */
        0x4c, 0x89, 0x54, 0x24, 0x38,  /* mov [rsp+0x38],r10  a6's stack slot, new frame */
        0x48, 0x8b, 0x49, 0x08,        /* mov rcx,[rcx+0x08]  a0 */
        0xff, 0xe0,                    /* jmp rax */
    };
    static void *thunk;
    struct
    {
        void      *fn;    /* 0x00 */
        ULONG_PTR  a[7];  /* 0x08 0x10 0x18 0x20 0x28 0x30 0x38 */
    } params = { fn, { a0, a1, a2, a3, a4, a5, a6 } };

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

static ULONG_PTR call_guest_function_args8( void *fn, ULONG_PTR a0, ULONG_PTR a1,
                                            ULONG_PTR a2, ULONG_PTR a3, ULONG_PTR a4,
                                            ULONG_PTR a5, ULONG_PTR a6, ULONG_PTR a7 )
{
    static const BYTE thunk_code[] =
    {
        0x4c, 0x8b, 0x1c, 0x24,        /* mov r11,[rsp]        save run_entry's return addr */
        0x48, 0x83, 0xec, 0x50,        /* sub rsp,0x50         a7's slot is +0x40, one past the
                                        *                      0x40 frame; 0x50 keeps alignment */
        0x4c, 0x89, 0x1c, 0x24,        /* mov [rsp],r11        return addr back at the NEW frame's +0x00 */
        0x48, 0x8b, 0x01,              /* mov rax,[rcx]       target */
        0x48, 0x8b, 0x51, 0x10,        /* mov rdx,[rcx+0x10]  a1 */
        0x4c, 0x8b, 0x41, 0x18,        /* mov r8,[rcx+0x18]   a2 */
        0x4c, 0x8b, 0x49, 0x20,        /* mov r9,[rcx+0x20]   a3 */
        0x4c, 0x8b, 0x51, 0x28,        /* mov r10,[rcx+0x28]  a4 */
        0x4c, 0x89, 0x54, 0x24, 0x28,  /* mov [rsp+0x28],r10  a4's stack slot, new frame */
        0x4c, 0x8b, 0x51, 0x30,        /* mov r10,[rcx+0x30]  a5 */
        0x4c, 0x89, 0x54, 0x24, 0x30,  /* mov [rsp+0x30],r10  a5's stack slot, new frame */
        0x4c, 0x8b, 0x51, 0x38,        /* mov r10,[rcx+0x38]  a6 */
        0x4c, 0x89, 0x54, 0x24, 0x38,  /* mov [rsp+0x38],r10  a6's stack slot, new frame */
        0x4c, 0x8b, 0x51, 0x40,        /* mov r10,[rcx+0x40]  a7 */
        0x4c, 0x89, 0x54, 0x24, 0x40,  /* mov [rsp+0x40],r10  a7's stack slot, new frame */
        0x48, 0x8b, 0x49, 0x08,        /* mov rcx,[rcx+0x08]  a0 */
        0xff, 0xe0,                    /* jmp rax */
    };
    static void *thunk;
    struct
    {
        void      *fn;    /* 0x00 */
        ULONG_PTR  a[8];  /* 0x08 0x10 0x18 0x20 0x28 0x30 0x38 0x40 */
    } params = { fn, { a0, a1, a2, a3, a4, a5, a6, a7 } };

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

static ULONG_PTR call_guest_function_args9( void *fn, ULONG_PTR a0, ULONG_PTR a1,
                                            ULONG_PTR a2, ULONG_PTR a3, ULONG_PTR a4,
                                            ULONG_PTR a5, ULONG_PTR a6, ULONG_PTR a7,
                                            ULONG_PTR a8 )
{
    static const BYTE thunk_code[] =
    {
        0x4c, 0x8b, 0x1c, 0x24,        /* mov r11,[rsp]        save run_entry's return addr */
        0x48, 0x83, 0xec, 0x50,        /* sub rsp,0x50         a8's slot is +0x48; same frame as eight */
        0x4c, 0x89, 0x1c, 0x24,        /* mov [rsp],r11        return addr back at the NEW frame's +0x00 */
        0x48, 0x8b, 0x01,              /* mov rax,[rcx]       target */
        0x48, 0x8b, 0x51, 0x10,        /* mov rdx,[rcx+0x10]  a1 */
        0x4c, 0x8b, 0x41, 0x18,        /* mov r8,[rcx+0x18]   a2 */
        0x4c, 0x8b, 0x49, 0x20,        /* mov r9,[rcx+0x20]   a3 */
        0x4c, 0x8b, 0x51, 0x28,        /* mov r10,[rcx+0x28]  a4 */
        0x4c, 0x89, 0x54, 0x24, 0x28,  /* mov [rsp+0x28],r10  a4's stack slot, new frame */
        0x4c, 0x8b, 0x51, 0x30,        /* mov r10,[rcx+0x30]  a5 */
        0x4c, 0x89, 0x54, 0x24, 0x30,  /* mov [rsp+0x30],r10  a5's stack slot, new frame */
        0x4c, 0x8b, 0x51, 0x38,        /* mov r10,[rcx+0x38]  a6 */
        0x4c, 0x89, 0x54, 0x24, 0x38,  /* mov [rsp+0x38],r10  a6's stack slot, new frame */
        0x4c, 0x8b, 0x51, 0x40,        /* mov r10,[rcx+0x40]  a7 */
        0x4c, 0x89, 0x54, 0x24, 0x40,  /* mov [rsp+0x40],r10  a7's stack slot, new frame */
        0x4c, 0x8b, 0x51, 0x48,        /* mov r10,[rcx+0x48]  a8 */
        0x4c, 0x89, 0x54, 0x24, 0x48,  /* mov [rsp+0x48],r10  a8's stack slot, new frame */
        0x48, 0x8b, 0x49, 0x08,        /* mov rcx,[rcx+0x08]  a0 */
        0xff, 0xe0,                    /* jmp rax */
    };
    static void *thunk;
    struct
    {
        void      *fn;    /* 0x00 */
        ULONG_PTR  a[9];  /* 0x08 0x10 0x18 0x20 0x28 0x30 0x38 0x40 0x48 */
    } params = { fn, { a0, a1, a2, a3, a4, a5, a6, a7, a8 } };

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


/***********************************************************************
 *           __wine_guest_InitSecurityInterfaceW   (NTDLL.@)
 *
 * What a guest's GetProcAddress(secur32, "InitSecurityInterfaceW") resolves
 * to (GUEST-IMPL in secur32.thunks, forwarded here from secur32.spec).  The
 * real export returns a SecurityFunctionTableW of NATIVE function pointers,
 * and a guest calling through one of those is the native-pc-in-a-guest-image
 * failure class -- so the guest gets a table built from the GUEST secur32
 * thunk module's own export addresses, each of which is an AMD64 stub that
 * traps into the same dispatch a named import would.  Cyberpunk 2077's
 * REDGalaxy64.dll is the wanting title: it GetProcAddress()es exactly this
 * name and throws gog::RuntimeError on NULL, which its engine treats as
 * fatal init.
 *
 * The slot order is Windows' SecurityFunctionTableW, copied from
 * dlls/secur32/secur32.c's own table -- including Windows' quirk of packing
 * EncryptMessage/DecryptMessage into the Reserved3/Reserved4 slots.  A name
 * the guest thunk does not export leaves a NULL slot, which is what a real
 * table has in its Reserved slots too; nothing is guessed.
 */
static const char * const sspi_tablew_names[] =
{
    "EnumerateSecurityPackagesW",
    "QueryCredentialsAttributesW",
    "AcquireCredentialsHandleW",
    "FreeCredentialsHandle",
    NULL,                            /* Reserved2 */
    "InitializeSecurityContextW",
    "AcceptSecurityContext",
    "CompleteAuthToken",
    "DeleteSecurityContext",
    "ApplyControlToken",
    "QueryContextAttributesW",
    "ImpersonateSecurityContext",
    "RevertSecurityContext",
    "MakeSignature",
    "VerifySignature",
    "FreeContextBuffer",
    "QuerySecurityPackageInfoW",
    "EncryptMessage",                /* Reserved3, as on Windows */
    "DecryptMessage",                /* Reserved4, as on Windows */
    "ExportSecurityContext",
    "ImportSecurityContextW",
    "AddCredentialsW",
    NULL,                            /* Reserved8 */
    "QuerySecurityContextToken",
    "EncryptMessage",
    "DecryptMessage",
    "SetContextAttributesW",
};

void * CDECL __wine_guest_InitSecurityInterfaceW(void)
{
    /* ULONG then pointers: dwVersion pads to one slot on LP64, which is the
     * MS-x64 layout of SecurityFunctionTableW.  Static and filled once; the
     * table outlives every caller, exactly like the real export's. */
    static struct
    {
        ULONG version;
        void *fn[ARRAY_SIZE(sspi_tablew_names)];
    } table;
    static HMODULE guest_secur32;

    if (!guest_secur32)
    {
        LIST_ENTRY *mark = &NtCurrentTeb()->Peb->LdrData->InMemoryOrderModuleList, *entry;
        HMODULE found = NULL;
        unsigned int i;

        for (entry = mark->Flink; entry != mark; entry = entry->Flink)
        {
            LDR_DATA_TABLE_ENTRY *mod = CONTAINING_RECORD( entry, LDR_DATA_TABLE_ENTRY, InMemoryOrderLinks );
            const IMAGE_NT_HEADERS *nt = RtlImageNtHeader( mod->DllBase );

            if (!nt || nt->FileHeader.Machine != IMAGE_FILE_MACHINE_AMD64) continue;
            if (wcsicmp( mod->BaseDllName.Buffer, L"secur32.dll" )) continue;
            found = mod->DllBase;
            break;
        }
        if (!found)
        {
            /* The caller reached this code through the guest secur32 thunk,
             * so its absence from the module list is a loader inconsistency,
             * not a user error. */
            ERR( "no guest secur32.dll in the module list\n" );
            return NULL;
        }
        table.version = 1;  /* SECURITY_SUPPORT_PROVIDER_INTERFACE_VERSION */
        for (i = 0; i < ARRAY_SIZE(sspi_tablew_names); i++)
        {
            ANSI_STRING name;
            void *proc = NULL;

            if (!sspi_tablew_names[i]) continue;
            RtlInitAnsiString( &name, sspi_tablew_names[i] );
            if (LdrGetProcedureAddress( found, &name, 0, &proc ))
            {
                TRACE( "guest secur32 has no %s; leaving the slot NULL\n",
                       sspi_tablew_names[i] );
                proc = NULL;
            }
            table.fn[i] = proc;
        }
        guest_secur32 = found;
    }
    TRACE( "-> %p (guest module %p)\n", &table, guest_secur32 );
    return &table;
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
static EMU_THREAD_VAR BOOL guest_exc_pending;
static EMU_THREAD_VAR EXCEPTION_RECORD guest_exc_rec;

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
static EMU_THREAD_VAR void *guest_exc_raising;   /* NULL, or where the report began */
static EMU_THREAD_VAR EXCEPTION_RECORD guest_exc_first;
#define GUEST_EXC_STACK_FLOOR (64 * 1024)

/* the trap CONTEXT the innermost emu_trap_dispatch on this thread is
 * serving: what a raise-style override (emu_RaiseException) dispatches
 * against.  Saved/restored around each dispatch, so nesting works. */
static EMU_THREAD_VAR AMD64_CONTEXT *emu_current_trap_ctx;

/* Under the bridge's lazy declaration (ABI 5, made by the unix side at
 * handler registration) a trap CONTEXT arrives WITHOUT its EFLAGS and FP
 * bytes, and any consumer that reads or rewrites those groups must fill them
 * first.  This is that fill: a no-op when the declaration is off, idempotent
 * when the groups are already there, so every consumer calls it
 * unconditionally.  The audited consumers are the FP-typed thunk branch in
 * emu_trap_dispatch, the raise/unwind family that hands the CONTEXT to guest
 * handlers, RtlCaptureContext, and the fiber pair that parks and replaces it
 * wholesale; the ordinary integer thunk hop -- nearly every crossing -- calls
 * nothing and pays for nothing, which is the point of the whole mechanism.
 * The gate's FEXBRIDGE_CTX_POISON lever is what catches a consumer this list
 * misses: poisoned values fail its value checks instead of working by luck. */
static void materialize_trap_ctx( AMD64_CONTEXT *ctx )
{
    WINE_UNIX_CALL( unix_emu_ctx_materialize, ctx );
}

/* The same fill for the FP hand walkers in OTHER PE modules (d3d12, d3d11,
 * d3d9, dsound, xaudio2, combase) -- they read XMM arguments and write XMM
 * returns on the trap CONTEXT winecom hands them, and cannot reach this
 * module's unixcall table.  Contract in wine/winecom.h. */
void CDECL __wine_emu_materialize_ctx( AMD64_CONTEXT *ctx )
{
    materialize_trap_ctx( ctx );
}

/* Set by an override that has REPLACED that CONTEXT wholesale rather than
 * returned a value into it -- a guest raise whose __except was found by the
 * frame walk, which resumes in another frame.  emu_trap_dispatch's ordinary
 * "pop the return address, store RAX" fixup is wrong in that case and only in
 * that case.  Saved/restored alongside emu_current_trap_ctx. */
static EMU_THREAD_VAR BOOL emu_trap_ctx_rewritten;

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
    ULONG64          context_out;  /* guest CONTEXT* to write the unwound state
                                    * back into, or 0.  RtlUnwindEx's
                                    * ContextRecord parameter is an OUTPUT:
                                    * Windows walks in it and leaves it holding
                                    * the target frame's context, and MSVC's
                                    * C++ personality reads the establisher
                                    * frame back out of it in its consolidation
                                    * routine.  Measured: GfnRuntimeSdk's catch
                                    * funclet entered with rdx=0 because the
                                    * routine read the STALE pre-unwind context,
                                    * whose Rbp the throw had left null. */
    void            *kept_stack;   /* the guest stack of the run that ISSUED
                                    * this request, kept mapped because the
                                    * record's slots may point into it (FH4's
                                    * establisher travels as a pointer to a
                                    * personality-run local), or NULL.  Freed
                                    * by the walk once nothing can read it --
                                    * after the consolidation routine has
                                    * run. */
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
static EMU_THREAD_VAR struct guest_unwind_state *guest_unwind_state;

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
static EMU_THREAD_VAR struct guest_handler_call *guest_handler_call;

/* Recursion bound on guest exception dispatch.  A filter or __finally funclet
 * runs as guest code and can fault, which dispatches again from inside this
 * dispatch; a handful of levels is legitimate, an unbounded number is a fault
 * loop that would otherwise consume the native stack silently. */
#define GUEST_SEH_MAX_DEPTH 8
static EMU_THREAD_VAR UINT guest_seh_depth;

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
         * returned by ending its own run, and the request is the answer.
         * The run's stack rides along: the record it recorded may point into
         * it (see guest_unwind_target.kept_stack). */
        *target = call.target;
        target->kept_stack = guest_kept_run_stack;
        guest_kept_run_stack = NULL;
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
    /* Every request adopted by this walk may bring the guest stack of the run
     * it was issued from (guest_unwind_target.kept_stack): the record's slots
     * may point into it, and the last reader is the consolidation routine at
     * the very end.  Collected here, freed on every exit after that point. */
    void *kept_stacks[GUEST_SEH_MAX_FRAMES];
    UINT n_kept = 0;

    if (want.kept_stack) kept_stacks[n_kept++] = want.kept_stack;

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

            /* A funclet run that ended for a COLLIDED unwind has no
             * guest_handler_call to carry its kept stack; sweep it here so
             * the record it recorded stays readable until this walk is done. */
            if (guest_kept_run_stack)
            {
                if (n_kept < GUEST_SEH_MAX_FRAMES) kept_stacks[n_kept++] = guest_kept_run_stack;
                else free_guest_run_stack( guest_kept_run_stack );
                guest_kept_run_stack = NULL;
            }

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
                if (again.kept_stack)
                {
                    if (n_kept < GUEST_SEH_MAX_FRAMES) kept_stacks[n_kept++] = again.kept_stack;
                    else free_guest_run_stack( again.kept_stack );
                }
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

    if (!status)
    {
        /* RAX carries the unwind's ReturnValue on arrival whichever kind of
         * unwind this was: x86-64 RtlUnwindEx writes context->Rax before
         * RtlRestoreContext and does not make that conditional on the
         * exception code. */
        result->Rax = want.return_value;

        /* RtlUnwindEx's ContextRecord parameter is an output: the issuer's
         * copy must hold the TARGET frame's context once the unwind is done,
         * because MSVC's consolidation routine reads the establisher frame
         * back out of it (see the field's comment).  Written before the
         * routine runs, which is exactly when Windows' in-place walk has it
         * written too. */
        if (want.context_out)
            *(AMD64_CONTEXT *)(ULONG_PTR)want.context_out = *result;

        if (unwind_rec.ExceptionCode == (DWORD)STATUS_UNWIND_CONSOLIDATE)
            status = guest_consolidate_callback( &unwind_rec, result );
        else
        {
            result->Rip = want.target_ip;
            TRACE( "guest resumes at rip %I64x rsp %I64x rax %I64x\n",
                   result->Rip, result->Rsp, result->Rax );
        }
    }

    /* The consolidation routine was the last thing that could read a record
     * slot pointing into an issuer's run stack; the kept stacks go now, on
     * every exit. */
    while (n_kept) free_guest_run_stack( kept_stacks[--n_kept] );

    return status;
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
    materialize_trap_ctx( ctx );   /* the walk reads it whole and rewrites it whole */

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
        /* A language handler unwinding WITHIN its own run is not the handler
         * ACCEPTING.  MSVC's C++ personality wraps its funclet dispatch in
         * SEH of its own and RtlUnwinds to its own establisher on the way
         * out -- measured: GfnRuntimeSdk's __CxxFrameHandler, called at
         * unwind time during a consolidating C++ unwind, RtlUnwind()s to a
         * frame at the base of the run stack it is executing on, and
         * deferring THAT to the frame walk handed the walk a frame on the
         * wrong stack, which it could only read as "target already passed"
         * (STATUS_INVALID_UNWIND_TARGET, and GeForce NOW's SDK took
         * Cyberpunk 2077 down with it).  The frames such a request means are
         * on the RUN's stack; the walk cannot serve them and does not need
         * to -- it is a local unwind, served in place like any other.  Only
         * a request naming a frame OFF this run's stack is the handler
         * accepting and unwinding the faulting stack. */
        struct emu_guest_stack_params stack = { 0 };

        WINE_UNIX_CALL( unix_emu_guest_stack, &stack );
        if (stack.limit && (void *)(ULONG_PTR)target->frame > stack.limit &&
            (void *)(ULONG_PTR)target->frame <= stack.base)
        {
            TRACE( "handler's unwind to frame %I64x ip %I64x stays on its own run "
                   "stack (%p-%p): a local unwind, served in place\n",
                   target->frame, target->target_ip, stack.limit, stack.base );
            return guest_unwind_in_place( target );
        }
        guest_handler_call->target  = *target;
        /* This road ENDS the issuer's run, and the run's guest stack with
         * it.  A ContextRecord that lives on that stack is about to dangle,
         * so the write-back the walk owes RtlUnwindEx's caller cannot be
         * delivered there -- dropped, with the reason, rather than written
         * into whatever reuses the pages.  One that lives anywhere else
         * (the dispatcher's own context, which is what a personality passes)
         * outlives the run and keeps its write-back. */
        if (target->context_out &&
            (void *)(ULONG_PTR)target->context_out > stack.limit &&
            (void *)(ULONG_PTR)target->context_out <= stack.base)
        {
            TRACE( "ContextRecord %I64x lives on the run stack this defer ends; "
                   "dropping the unwound-context write-back\n", target->context_out );
            guest_handler_call->target.context_out = 0;
        }
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
        /* Same run-stack lifetime rule as the deferred road above: this
         * collision ends the funclet's run too. */
        if (target->context_out)
        {
            struct emu_guest_stack_params fstack = { 0 };

            WINE_UNIX_CALL( unix_emu_guest_stack, &fstack );
            if ((void *)(ULONG_PTR)target->context_out > fstack.limit &&
                (void *)(ULONG_PTR)target->context_out <= fstack.base)
            {
                TRACE( "ContextRecord %I64x lives on the run stack this collision ends; "
                       "dropping the unwound-context write-back\n", target->context_out );
                unwind->again.context_out = 0;
            }
        }
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
 *           report_guest_access_violation
 *
 * WHAT THE GUEST WAS HOLDING WHEN IT COULD NOT TOUCH SOMETHING.
 *
 * An address and a fault kind name the symptom; they do not name the bug.  On
 * this port the two questions that do are "which register was wrong" and "who
 * transferred control here", and both were reachable only through +seh -- 22 GB
 * of trace for one DOOM run -- or through the guest's own crash reporter, which
 * writes an all-zero register block and cannot be believed at all.
 *
 * So the whole guest register file is printed, with the pc and the stack
 * pointer resolved to module+RVA, sixteen bytes of the code around the pc, and
 * the top of the guest stack with every value that lands in a guest image
 * resolved the same way.  A CALL pushes its return address, so the first such
 * value is normally the caller: that is the line that says who branched here.
 *
 * It also settles a question the pc alone cannot.  DOOM (2016) faults at a pc
 * four bytes into a function, i.e. INSIDE the instruction that begins it, which
 * either means the guest really branched into the middle of an instruction or
 * means the reported pc is imprecise.  The registers decide it: if the pc is
 * exact, the operands of the instruction decoded there must explain the
 * address that faulted, and if they do not, the pc is not where the fault was.
 *
 * Bounded to the first four, because a guest that catches its own faults raises
 * thousands of them and they are all the same one.
 */
static void report_guest_access_violation( EXCEPTION_RECORD *rec, const AMD64_CONTEXT *ctx,
                                           void *stack_base, void *stack_limit )
{
    static const char * const names[16] =
        { "rax", "rcx", "rdx", "rbx", "rsp", "rbp", "rsi", "rdi",
          "r8 ", "r9 ", "r10", "r11", "r12", "r13", "r14", "r15" };
    const ULONG64 regs[16] =
        { ctx->Rax, ctx->Rcx, ctx->Rdx, ctx->Rbx, ctx->Rsp, ctx->Rbp, ctx->Rsi, ctx->Rdi,
          ctx->R8,  ctx->R9,  ctx->R10, ctx->R11, ctx->R12, ctx->R13, ctx->R14, ctx->R15 };
    static LONG reported;
    LDR_DATA_TABLE_ENTRY *mod;
    UINT i;

    if (InterlockedIncrement( &reported ) > 4) return;

    if ((mod = guest_module_entry_from_address( (void *)(ULONG_PTR)ctx->Rip )))
        ERR( "guest fault %08x at %s+%I64x (rip %I64x), touching %p\n", (UINT)rec->ExceptionCode,
             debugstr_w(mod->BaseDllName.Buffer), ctx->Rip - (ULONG64)(ULONG_PTR)mod->DllBase,
             ctx->Rip, (void *)rec->ExceptionInformation[1] );
    else
        ERR( "guest fault %08x at rip %I64x, which is in no guest image, touching %p\n",
             (UINT)rec->ExceptionCode, ctx->Rip, (void *)rec->ExceptionInformation[1] );

    for (i = 0; i < 16; i += 4)
        ERR( "  %s=%016I64x  %s=%016I64x  %s=%016I64x  %s=%016I64x\n",
             names[i],   regs[i],   names[i+1], regs[i+1],
             names[i+2], regs[i+2], names[i+3], regs[i+3] );

    /* The code around the pc, read from the LIVE image rather than the file on
     * disk, because a DRM stub that rewrites its own text makes those two
     * different things.  Only read when the pc is inside a module, where the
     * whole window is mapped and this cannot itself fault. */
    if (mod)
    {
        const IMAGE_NT_HEADERS *nt = RtlImageNtHeader( mod->DllBase );
        const char *base = (const char *)mod->DllBase;
        const char *end = base + (nt ? nt->OptionalHeader.SizeOfImage : 0);
        const char *rip = (const char *)(ULONG_PTR)ctx->Rip;
        const char *from = (rip - 8 >= base) ? rip - 8 : base;
        const char *to = (rip + 16 <= end) ? rip + 16 : end;
        static const char hex[] = "0123456789abcdef";
        char buf[3 * 24 + 1];
        UINT n = 0;

        while (from + n / 3 < to && n + 3 < sizeof(buf))
        {
            BYTE b = (BYTE)from[n / 3];

            buf[n++] = hex[b >> 4];
            buf[n++] = hex[b & 0xf];
            buf[n++] = ' ';
        }
        buf[n] = 0;
        ERR( "  code at rip%+d: %s(rip is byte %d of that window)\n",
             (int)(from - rip), buf, (int)(rip - from) );
    }

    /* The top of the guest stack.  The first value in a guest image is normally
     * the return address a CALL pushed, and therefore the caller. */
    if (is_valid_guest_frame( ctx->Rsp, stack_base, stack_limit ) && guest_stack_is_readable( ctx->Rsp ))
    {
        for (i = 0; i < 8; i++)
        {
            ULONG64 slot = ctx->Rsp + i * 8;
            ULONG64 val;

            if (!is_valid_guest_frame( slot, stack_base, stack_limit )) break;
            if (!guest_stack_is_readable( slot )) break;
            val = *(ULONG64 *)(ULONG_PTR)slot;
            if ((mod = guest_module_entry_from_address( (void *)(ULONG_PTR)val )))
                ERR( "  [rsp+%02x]=%016I64x = %s+%I64x\n", i * 8, val,
                     debugstr_w(mod->BaseDllName.Buffer), val - (ULONG64)(ULONG_PTR)mod->DllBase );
            else
                ERR( "  [rsp+%02x]=%016I64x\n", i * 8, val );
        }
    }
    else ERR( "  rsp %I64x is not a readable frame on this guest stack (%p-%p)\n",
              ctx->Rsp, stack_limit, stack_base );
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

    if (rec->ExceptionCode == EXCEPTION_ACCESS_VIOLATION && rec->NumberParameters >= 2)
        report_guest_access_violation( rec, ctx, stack_base, stack_limit );

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
    NTSTATUS status;

    xstat_event( XSTAT_EV_GUEST_FAULT, "guest exception dispatch" );
    status = dispatch_guest_exception( exc->rec, exc->ctx, exc->stack_base,
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
    materialize_trap_ctx( ctx );   /* guest handlers receive this CONTEXT whole */
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
    materialize_trap_ctx( ctx );   /* guest handlers receive this CONTEXT whole */
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
 * The ContextRecord argument is unused as an INPUT, and that matches x86-64
 * rather than departing from it: RtlUnwindEx opens with
 * RtlCaptureContext( context ), i.e. it overwrites the context it is handed
 * with its own and walks from there, and the history table is a lookup cache.
 * Here "its own" is the guest state the trap fired with, which is where
 * guest_unwind_in_place() starts.  As an OUTPUT it is honoured: the walk
 * leaves the target frame's context in it, which is what MSVC's C++
 * personality reads its establisher frame back out of -- see
 * guest_unwind_target.context_out.
 */
static NTSTATUS guest_unwind_ex( ULONG64 end_frame, ULONG64 target_ip,
                                 const EXCEPTION_RECORD *rec, ULONG64 retval,
                                 ULONG64 context_out )
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
    target.context_out  = context_out;
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
        if (TRACE_ON(seh))
        {
            UINT i;
            for (i = 0; i < target.rec.NumberParameters && i < EXCEPTION_MAXIMUM_PARAMETERS; i++)
                TRACE( "  consolidate info[%u]=%I64x\n", i, target.rec.ExceptionInformation[i] );
        }
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
    if ((status = guest_unwind_ex( a[0], a[1], rec, a[3], a[4] )))
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
    materialize_trap_ctx( ctx );   /* the capture is a whole-CONTEXT read */
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
static EMU_THREAD_VAR BOOL guest_exit_requested;
static EMU_THREAD_VAR ULONG guest_exit_code;

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
 *           crossing-frequency counters  (WINE_PPC64LE_TRAP_STATS=<path>)
 *
 * WINEEMUPROFILE answers "which cache slots are hot"; this answers "which
 * NAMED call crosses the boundary most per second", which is the question a
 * fast path is chosen against.  A sampling profiler cannot answer it at all:
 * it attributes time to the callee and never counts the crossings that
 * reached it.
 *
 * ONE RELAXED ADD ON THE COUNTING PATH, and nothing else.  Every identity is
 * resolved to a row index once, on the path that was already resolving that
 * site under the loader lock, and the index is then carried in the same
 * structure the site's other loop-invariant answers are carried in -- the
 * thunk RIP cache, for flat exports and COM slots.  The add is
 * __ATOMIC_RELAXED, not InterlockedIncrement: a count must not lose
 * increments across threads, but nothing downstream orders against it, and a
 * full barrier per crossing would be a measurable share of the thing being
 * measured.
 *
 * NO STRINGS ARE TOUCHED WHILE COUNTING.  Names are copied into the row once,
 * when the row is interned, and the syscall class does not intern at all --
 * it counts into the dispatcher's own CounterTable and is resolved to names
 * by the unix side at dump time.
 *
 * OFF COSTS TWO PREDICTED BRANCHES: one on the probe flag, one on the control
 * pointer.  Neither the rows nor the syscall counter arrays are allocated
 * unless the environment names a file.
 */
static struct emu_xstat_ctl *xstat;      /* NULL unless armed */
static int xstat_probed;
static LONG xstat_lock;                  /* interning only; never counting */

/* Crossings on THIS thread between automatic-dump checks.  Per-thread and
 * plain, because a shared tick counter would be a contended cache line
 * written by every crossing -- the exact cost this instrumentation must not
 * add.  A power of two so the test is a mask. */
static EMU_THREAD_VAR UINT xstat_tick;
#define XSTAT_TICK_MASK 0xfffff

#define XSTAT_NO_ROW (~0u)

void emu_xstat_dump(void)
{
    if (xstat) WINE_UNIX_CALL( unix_emu_xstat_dump, NULL );
}

static void xstat_probe(void)
{
    struct emu_xstat_init_params params = { NULL };

    if (!WINE_UNIX_CALL( unix_emu_xstat_init, &params )) xstat = params.ctl;
    xstat_probed = 1;
}

/* The pointer is what decides, not the flag: two threads crossing for the
 * first time together both probe (the unix side's init is idempotent under a
 * mutex), and a thread that sees the flag set before the pointer simply does
 * not count for the microseconds until its cache line catches up.  Ordering
 * that with a barrier would put one on every crossing to buy nothing. */
static BOOL xstat_on(void)
{
    if (!xstat_probed) xstat_probe();
    return xstat != NULL;
}

static UINT xstat_hash( UINT cls, ULONG_PTR key )
{
    ULONG64 h = (ULONG64)key + (ULONG64)cls * 0x9e3779b97f4a7c15ull;

    h ^= h >> 30;
    h *= 0xbf58476d1ce4e5b9ull;
    h ^= h >> 27;
    return (UINT)(h >> 32);
}

/* Lock-free: a row's key is published with a release store AFTER its class
 * and name are written, so a nonzero key seen here means a complete row. */
static UINT xstat_find( UINT cls, ULONG_PTR key )
{
    UINT mask = xstat->rows_max - 1;
    UINT i = xstat_hash( cls, key ) & mask, n;

    for (n = 0; n <= mask; n++, i = (i + 1) & mask)
    {
        const struct emu_xstat_row *r = &xstat->rows[i];
        ULONG_PTR k = __atomic_load_n( &r->key, __ATOMIC_ACQUIRE );

        if (!k) return XSTAT_NO_ROW;
        if (k == key && r->cls == cls) return i;
    }
    return XSTAT_NO_ROW;
}

/* -> the row for (cls, key), interning it under `name` if it is new.  Insert
 * takes a spin lock, which is legitimate here and nowhere else: it runs once
 * per distinct call site, on a path that has already taken the loader lock or
 * is about to start a nested emulator run. */
static UINT xstat_intern( UINT cls, ULONG_PTR key, const char *name )
{
    UINT row, i, n, mask;

    if ((row = xstat_find( cls, key )) != XSTAT_NO_ROW) return row;

    while (InterlockedCompareExchange( &xstat_lock, 1, 0 )) YieldProcessor();
    if ((row = xstat_find( cls, key )) == XSTAT_NO_ROW)
    {
        mask = xstat->rows_max - 1;
        for (n = 0, i = xstat_hash( cls, key ) & mask; n <= mask; n++, i = (i + 1) & mask)
        {
            struct emu_xstat_row *r = &xstat->rows[i];

            if (r->key) continue;
            r->cls = cls;
            if (name)
            {
                SIZE_T len = strlen( name );

                if (len >= sizeof(r->name)) len = sizeof(r->name) - 1;
                memcpy( r->name, name, len );
                r->name[len] = 0;
            }
            __atomic_store_n( &r->key, key, __ATOMIC_RELEASE );
            row = i;
            break;
        }
    }
    WriteRelease( (LONG volatile *)&xstat_lock, 0 );
    return row;
}

/* The whole counting path.  Anything that is not this add belongs elsewhere. */
static void xstat_hit( UINT row )
{
    if (row == XSTAT_NO_ROW) return;
    __atomic_fetch_add( &xstat->rows[row].count, 1, __ATOMIC_RELAXED );
}

/* Dumped periodically as well as at shutdown, because the runs worth measuring
 * are exactly the ones that do not shut down -- a game a timeout kills, or one
 * that dies on its own error path.  SIGUSR2 asks for a dump at a chosen moment
 * and is noticed here within a few thousand crossings.
 *
 * Called only from crossing paths that hold NO lock.  The resolve path holds
 * the loader lock, and a dump formats and writes a file; a site resolves once,
 * so leaving it out of the tick costs nothing measurable and keeps the
 * process-wide lock off the write. */
static void xstat_tick_check(void)
{
    if (++xstat_tick & 0xfff) return;
    if (!(xstat_tick & XSTAT_TICK_MASK) || xstat->dump_req)
    {
        xstat->dump_req = 0;
        emu_xstat_dump();
    }
}

static void xstat_event( ULONG_PTR ev, const char *name )
{
    if (!xstat_on()) return;
    xstat_hit( xstat_intern( EMU_XSTAT_EVENT, ev, name ) );
}

/* Just enough formatting to name a row, hand-rolled.  The PE side of ntdll
 * has no snprintf it can call: printf.c defines the export under an internal
 * name (NTDLL__snprintf), so a caller inside ntdll would not link.  A row is
 * named once, when it is interned. */
static SIZE_T xstat_put( char *buf, SIZE_T len, SIZE_T at, const char *s )
{
    while (s && *s && at + 1 < len) buf[at++] = *s++;
    if (at < len) buf[at] = 0;
    return at;
}

static SIZE_T xstat_put_w( char *buf, SIZE_T len, SIZE_T at, const WCHAR *s )
{
    while (s && *s && at + 1 < len) buf[at++] = (char)*s++;
    if (at < len) buf[at] = 0;
    return at;
}

static SIZE_T xstat_put_num( char *buf, SIZE_T len, SIZE_T at, ULONG64 v, UINT base )
{
    char tmp[24];
    UINT n = 0;

    do
    {
        UINT d = (UINT)(v % base);
        tmp[n++] = d < 10 ? '0' + d : 'a' + d - 10;
        v /= base;
    } while (v && n < sizeof(tmp));
    while (n && at + 1 < len) buf[at++] = tmp[--n];
    if (at < len) buf[at] = 0;
    return at;
}

/* module.Export, out of the guest module's own name table. */
static void xstat_name_export( char *buf, SIZE_T len, const WCHAR *mod, const char *fn )
{
    SIZE_T at = xstat_put_w( buf, len, 0, mod );

    at = xstat_put( buf, len, at, "." );
    xstat_put( buf, len, at, fn );
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
 * allocate_stub's.  Up to seven arguments each is twelve instructions:
 *
 *      r(3+argc) = guest target          (one past the last real argument)
 *      r12       = guest_callback_dispatch[5-9][_wide]  (its global entry)
 *      mtctr r12 / bctr
 *
 * r3..r(2+argc) pass through untouched, so the dispatcher receives the
 * native caller's first argc integer arguments plus the guest target, and
 * argc arguments always travel for that slot's dispatcher (see
 * call_guest_function_args[5-9]).  ARITY IS PER SLOT, the same way WIDTH is
 * (next paragraph): argc is 4 for every callback this pool carried until
 * SetWindowSubclass's SUBCLASSPROC (six arguments) and
 * InternetSetStatusCallback's INTERNET_STATUS_CALLBACK (five) needed their
 * own, so the identity register moves with it -- r7 at argc=4 (the original,
 * fixed shape), r8 at argc=5, r9 at argc=6, r10 at argc=7 -- and is never
 * one of the real argument registers for that slot's arity.
 *
 * AT EIGHT THE TAIL JUMP RUNS OUT OF REGISTERS, and the stub changes shape
 * rather than the rule changing meaning.  An eight-argument callback owns
 * r3..r10 -- all eight ELFv2 argument registers -- so the identity has no
 * register to ride, and the dispatcher's ninth parameter belongs in its
 * caller's parameter save area, which a native caller invoking an
 * eight-argument function pointer is NOT required to have allocated (ELFv2
 * makes that area optional when every parameter fits in registers): a
 * tail-jumping stub writing r1+96 would be writing memory nobody promised
 * exists.  So the eight- and nine-argument stubs are a real CALL instead of
 * a jump: the standard prologue (mflr/std/stdu) builds a 112-byte frame
 * whose parameter save area is the dispatcher's by right, the guest target
 * is stored there as the trailing parameter -- and, at nine, the ninth REAL
 * argument is first copied over from the native caller's own parameter save
 * area at old-r1+96, where ELFv2 guarantees it, because nine arguments
 * force the caller to allocate one -- then bctrl, tear down, blr.  r3..r10
 * still pass through untouched, the result rides back in r3 untouched, and
 * the frame is back-chained with LR saved at the ABI's own slot (caller's
 * frame +16), so a walker that follows back chains reads straight through
 * it.  code[] grew from 12 words to 22 -- the nine-argument call stub's
 * exact length -- to hold the larger shape.
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
 * to rather than by a flag the dispatcher reads: one stub per
 * (target, width, arity), so none of the three can be confused with another.
 *
 * WINEEMUNOCBWRAP=1 is the negative control, same shape as
 * WINEEMUNOGSTHREADS: registration hands the raw guest pointer to native
 * code, which is exactly the bug this pool exists to fix, so anything this
 * mechanism carries MUST go red under it.
 */
struct guest_callback_stub
{
    UINT  code[22];      /* 4-7 arguments, twelve words: r(3+argc) = guest_fn;
                          * r12 = dispatch; mtctr; bctr -- the identity register
                          * sits one past the last real ELFv2 argument register,
                          * r7 for a 4-argument slot (the original, fixed shape)
                          * through r10 for a 7-argument one.  8-9 arguments:
                          * the call-shaped stub the pool banner describes --
                          * prologue, guest_fn (and at nine the forwarded ninth
                          * argument) stored into the new frame's parameter save
                          * area, bctrl, epilogue -- twenty words at eight,
                          * twenty-two at nine */
    void *guest_fn;      /* identity: one stub per target, and post-mortem */
    UINT  wide;          /* ...per WIDTH too: the other half of that identity */
    UINT  argc;          /* ...and per ARITY: the third and last part of it,
                          * since the same guest function can legitimately be
                          * registered as callbacks of different shapes.  Was
                          * an unused `pad` field; a slot outgrew its single
                          * cache line when code[] did, which nothing depended
                          * on. */
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
#define GUEST_CB_BLOCK 1024        /* stubs per block, a 104KB allocation */
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

/* -> the guest target of the pool stub at fn, or NULL if fn is not the base
 * address of a live stub.  Interior pointers return NULL deliberately: the
 * pool only ever hands out stub BASES, so anything else is not ours.  Callers
 * hold the loader lock, as every reader and writer of the pool does. */
static void *guest_cb_target( const void *fn )
{
    UINT b;

    for (b = 0; b < guest_cb_blocks; b++)
    {
        const struct guest_callback_stub *base = guest_cb_block[b];
        UINT used = (b + 1 == guest_cb_blocks) ? guest_cb_count : GUEST_CB_BLOCK;

        if (fn >= (const void *)base && fn < (const void *)(base + used))
        {
            SIZE_T off = (const char *)fn - (const char *)base;
            if (off % sizeof(*base)) return NULL;
            return base[off / sizeof(*base)].guest_fn;
        }
    }
    return NULL;
}

/* The general inverse of wrap_guest_callback[_ex], for ANY pool stub
 * travelling INTO the guest through a plain "SET returns the previous one"
 * registration point: a stub goes back as the guest function it dispatches
 * to, and a native function, a never-wrapped value, or NULL passes through
 * untouched.  Idempotent against the wrap in both directions, for the same
 * reason unwrap_guest_wndproc below is.
 *
 * WHY THIS EXISTS BESIDE unwrap_guest_wndproc -- audited, 2026-08-30
 * (ppc64le/docs/sessions/2026-08-29/pointer-identity-audit.md), after the
 * WNDPROC fix (see unwrap_guest_wndproc) made the shape of the bug obvious:
 * ANY registration API that hands back "the one you replaced" is a
 * save-and-compare or save-and-chain candidate, not just window procedures.
 * SetUnhandledExceptionFilter is the textbook case -- comparing or chaining
 * the previous filter is documented, ordinary practice -- and
 * mmioInstallIOProc's MMIO_FINDPROC/MMIO_REMOVEPROC modes and
 * _set_new_handler's save-and-restore share the exact shape.  A WNDPROC
 * additionally has to let win32u's 0xffff00nn handles and native procs
 * through unexamined by guest_cb_target, which is what makes
 * unwrap_guest_wndproc a thin wrapper around this rather than a duplicate of
 * it. */
static void *unwrap_guest_cb( void *fn )
{
    void *target;
    ULONG_PTR magic;

    if (!fn) return fn;
    LdrLockLoaderLock( 0, NULL, &magic );
    target = guest_cb_target( fn );
    LdrUnlockLoaderLock( 0, magic );
    return target ? target : fn;
}

/* The inverse of wrap_guest_wndproc, for WNDPROC values travelling INTO the
 * guest: a pool stub goes back as the guest function it dispatches to, and
 * everything else -- native procs, win32u winproc handles, values that were
 * never wrapped -- passes through untouched.  Idempotent against the wrap
 * (re-wrapping the unwrapped pointer finds the same stub in the pool), so a
 * value can cross the boundary any number of times in either direction.
 *
 * WHY THIS EXISTS -- measured, Quake II rerelease, 2026-08-30.  Its SDL2
 * subclasses the way SDL has always done: WIN_SetupWindowData reads
 * GetWindowLongPtrW(GWLP_WNDPROC) back and compares it against its own
 * WIN_WindowProc; only on a MISMATCH does it store the value as "previous
 * proc" and subclass.  On Windows the read-back is the raw pointer it
 * registered and the comparison matches.  On this port the read-back was the
 * pool stub, the comparison missed, and SDL chained to a "previous proc"
 * that dispatches straight back into WIN_WindowProc: every message recursed
 * stub->proc->CallWindowProc(stub) 224 crossings deep until the kernel-stack
 * floor check killed the callback (c0000001, 113 times), and the game exited
 * through its own error path (rc=3).  The old comment here dismissed the
 * read-back-and-compare idiom as one "no correct program does" -- SDL does,
 * in every program that links it, and it is correct on Windows whenever the
 * registration and the read are the same flavor.  See unwrap_guest_cb above
 * for the same fix applied to the other registration points the follow-up
 * audit found in the same shape. */
static void *unwrap_guest_wndproc( void *fn )
{
    void *target;

    if (!fn || ((ULONG_PTR)fn >> 16) == 0xffff) return fn;
    target = unwrap_guest_cb( fn );
    if (target != fn) TRACE( "wndproc stub %p unwraps to guest %p\n", fn, target );
    return target;
}

/* One row per guest callback TARGET, which is what the pool's own identity is
 * keyed on too (one stub per target/width/arity).  Named by the guest module
 * and offset rather than by the API that registered it: the registering export
 * is already a row of its own in the flat class, and plumbing its name down
 * through wrap_thunk_callback_args would put a string on a path that has none.
 *
 * A hash probe rather than a carried index, because the dispatchers receive the
 * guest target and not the stub that holds it.  That probe is two loads in
 * front of a nested emulator run, which is the cheapest thing on this path by
 * three orders of magnitude. */
static void xstat_count_callback( void *fn )
{
    UINT row;

    if (!xstat_on()) return;
    if ((row = xstat_find( EMU_XSTAT_CALLBACK, (ULONG_PTR)fn )) == XSTAT_NO_ROW)
    {
        LDR_DATA_TABLE_ENTRY *mod = NULL;
        char name[EMU_XSTAT_NAME];
        SIZE_T i = 0;

        if (!LdrFindEntryForAddress( fn, &mod ) && mod)
        {
            i = xstat_put_w( name, sizeof(name), 0, mod->BaseDllName.Buffer );
            i = xstat_put( name, sizeof(name), i, "+0x" );
            xstat_put_num( name, sizeof(name), i,
                           (ULONG64)((ULONG_PTR)fn - (ULONG_PTR)mod->DllBase), 16 );
        }
        else
        {
            i = xstat_put( name, sizeof(name), 0, "guest 0x" );
            xstat_put_num( name, sizeof(name), i, (ULONG64)(ULONG_PTR)fn, 16 );
        }
        row = xstat_intern( EMU_XSTAT_CALLBACK, (ULONG_PTR)fn, name );
    }
    xstat_hit( row );
    xstat_tick_check();
}

static ULONG_PTR guest_callback_run( ULONG_PTR a0, ULONG_PTR a1, ULONG_PTR a2,
                                     ULONG_PTR a3, void *fn, BOOL *ended )
{
    ULONG_PTR ret;

    xstat_count_callback( fn );
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

/* The five- and six-argument forms of guest_callback_run/_dispatch[_wide],
 * for the two consumers that need call_guest_function_args5/6: comctl32's
 * SUBCLASSPROC (six arguments, LRESULT return -> wide) and wininet's
 * INTERNET_STATUS_CALLBACK (five arguments, void return -> narrow is fine
 * either way, since there is no result to truncate).  Integer/pointer-only,
 * same as every other row this pool carries; see the FP note beside
 * call_guest_function_args5/6 above. */
static ULONG_PTR guest_callback_run5( ULONG_PTR a0, ULONG_PTR a1, ULONG_PTR a2,
                                      ULONG_PTR a3, ULONG_PTR a4, void *fn, BOOL *ended )
{
    ULONG_PTR ret;

    xstat_count_callback( fn );
    TRACE( "calling guest callback %p (%p,%p,%p,%p,%p)\n", fn,
           (void *)a0, (void *)a1, (void *)a2, (void *)a3, (void *)a4 );
    ret = call_guest_function_args5( fn, a0, a1, a2, a3, a4 );
    if ((*ended = guest_exit_requested)) return 0;
    TRACE( "guest callback %p returned %p\n", fn, (void *)ret );
    return ret;
}

static ULONG_PTR guest_callback_dispatch5( ULONG_PTR a0, ULONG_PTR a1, ULONG_PTR a2,
                                           ULONG_PTR a3, ULONG_PTR a4, void *fn )
{
    BOOL ended;
    ULONG_PTR ret = guest_callback_run5( a0, a1, a2, a3, a4, fn, &ended );

    if (ended) return 0;
    return (ULONG_PTR)(LONG_PTR)(LONG)ret;
}

static ULONG_PTR guest_callback_dispatch5_wide( ULONG_PTR a0, ULONG_PTR a1, ULONG_PTR a2,
                                                ULONG_PTR a3, ULONG_PTR a4, void *fn )
{
    BOOL ended;
    ULONG_PTR ret = guest_callback_run5( a0, a1, a2, a3, a4, fn, &ended );

    if (ended) return 0;
    return ret;
}

static ULONG_PTR guest_callback_run6( ULONG_PTR a0, ULONG_PTR a1, ULONG_PTR a2,
                                      ULONG_PTR a3, ULONG_PTR a4, ULONG_PTR a5,
                                      void *fn, BOOL *ended )
{
    ULONG_PTR ret;

    xstat_count_callback( fn );
    TRACE( "calling guest callback %p (%p,%p,%p,%p,%p,%p)\n", fn,
           (void *)a0, (void *)a1, (void *)a2, (void *)a3, (void *)a4, (void *)a5 );
    ret = call_guest_function_args6( fn, a0, a1, a2, a3, a4, a5 );
    if ((*ended = guest_exit_requested)) return 0;
    TRACE( "guest callback %p returned %p\n", fn, (void *)ret );
    return ret;
}

static ULONG_PTR guest_callback_dispatch6( ULONG_PTR a0, ULONG_PTR a1, ULONG_PTR a2,
                                           ULONG_PTR a3, ULONG_PTR a4, ULONG_PTR a5,
                                           void *fn )
{
    BOOL ended;
    ULONG_PTR ret = guest_callback_run6( a0, a1, a2, a3, a4, a5, fn, &ended );

    if (ended) return 0;
    return (ULONG_PTR)(LONG_PTR)(LONG)ret;
}

static ULONG_PTR guest_callback_dispatch6_wide( ULONG_PTR a0, ULONG_PTR a1, ULONG_PTR a2,
                                                ULONG_PTR a3, ULONG_PTR a4, ULONG_PTR a5,
                                                void *fn )
{
    BOOL ended;
    ULONG_PTR ret = guest_callback_run6( a0, a1, a2, a3, a4, a5, fn, &ended );

    if (ended) return 0;
    return ret;
}

/* The seven-, eight- and nine-argument forms, for the rows
 * wrap_guest_callback_ex refused by name until now: user32's WINEVENTPROC
 * and advapi32's PENABLECALLBACK (seven), user32's DDE PFNCALLBACK and
 * ws2_32's LPCONDITIONPROC (eight), and kernel32's LPPROGRESS_ROUTINE
 * (nine).  Integer/pointer-only like every other row this pool carries --
 * LPPROGRESS_ROUTINE's by-value LARGE_INTEGERs are ordinary 64-bit slots in
 * both conventions; see the note beside call_guest_function_args7/8/9 above.
 *
 * At seven the trailing fn still rides an ELFv2 argument register (r10, the
 * eighth and last).  At eight and nine it is the dispatcher's ninth or tenth
 * parameter -- a STACK parameter, read from the caller's parameter save
 * area, which is why those two arities need the call-shaped stub the pool
 * banner describes: the stub builds the frame that parameter save area
 * lives in.
 *
 * PFNCALLBACK is why the eight-argument pair ships a _wide form that is not
 * merely symmetry: it returns HDDEDATA, a full 64-bit handle, and the
 * default sign-extended-32 return would replace a handle's top half with a
 * copy of bit 31 -- plausible, silent and wrong, the class this pool exists
 * to kill. */
static ULONG_PTR guest_callback_run7( ULONG_PTR a0, ULONG_PTR a1, ULONG_PTR a2,
                                      ULONG_PTR a3, ULONG_PTR a4, ULONG_PTR a5,
                                      ULONG_PTR a6, void *fn, BOOL *ended )
{
    ULONG_PTR ret;

    xstat_count_callback( fn );
    TRACE( "calling guest callback %p (%p,%p,%p,%p,%p,%p,%p)\n", fn,
           (void *)a0, (void *)a1, (void *)a2, (void *)a3, (void *)a4,
           (void *)a5, (void *)a6 );
    ret = call_guest_function_args7( fn, a0, a1, a2, a3, a4, a5, a6 );
    if ((*ended = guest_exit_requested)) return 0;
    TRACE( "guest callback %p returned %p\n", fn, (void *)ret );
    return ret;
}

static ULONG_PTR guest_callback_dispatch7( ULONG_PTR a0, ULONG_PTR a1, ULONG_PTR a2,
                                           ULONG_PTR a3, ULONG_PTR a4, ULONG_PTR a5,
                                           ULONG_PTR a6, void *fn )
{
    BOOL ended;
    ULONG_PTR ret = guest_callback_run7( a0, a1, a2, a3, a4, a5, a6, fn, &ended );

    if (ended) return 0;
    return (ULONG_PTR)(LONG_PTR)(LONG)ret;
}

static ULONG_PTR guest_callback_dispatch7_wide( ULONG_PTR a0, ULONG_PTR a1, ULONG_PTR a2,
                                                ULONG_PTR a3, ULONG_PTR a4, ULONG_PTR a5,
                                                ULONG_PTR a6, void *fn )
{
    BOOL ended;
    ULONG_PTR ret = guest_callback_run7( a0, a1, a2, a3, a4, a5, a6, fn, &ended );

    if (ended) return 0;
    return ret;
}

static ULONG_PTR guest_callback_run8( ULONG_PTR a0, ULONG_PTR a1, ULONG_PTR a2,
                                      ULONG_PTR a3, ULONG_PTR a4, ULONG_PTR a5,
                                      ULONG_PTR a6, ULONG_PTR a7, void *fn, BOOL *ended )
{
    ULONG_PTR ret;

    xstat_count_callback( fn );
    TRACE( "calling guest callback %p (%p,%p,%p,%p,%p,%p,%p,%p)\n", fn,
           (void *)a0, (void *)a1, (void *)a2, (void *)a3, (void *)a4,
           (void *)a5, (void *)a6, (void *)a7 );
    ret = call_guest_function_args8( fn, a0, a1, a2, a3, a4, a5, a6, a7 );
    if ((*ended = guest_exit_requested)) return 0;
    TRACE( "guest callback %p returned %p\n", fn, (void *)ret );
    return ret;
}

static ULONG_PTR guest_callback_dispatch8( ULONG_PTR a0, ULONG_PTR a1, ULONG_PTR a2,
                                           ULONG_PTR a3, ULONG_PTR a4, ULONG_PTR a5,
                                           ULONG_PTR a6, ULONG_PTR a7, void *fn )
{
    BOOL ended;
    ULONG_PTR ret = guest_callback_run8( a0, a1, a2, a3, a4, a5, a6, a7, fn, &ended );

    if (ended) return 0;
    return (ULONG_PTR)(LONG_PTR)(LONG)ret;
}

static ULONG_PTR guest_callback_dispatch8_wide( ULONG_PTR a0, ULONG_PTR a1, ULONG_PTR a2,
                                                ULONG_PTR a3, ULONG_PTR a4, ULONG_PTR a5,
                                                ULONG_PTR a6, ULONG_PTR a7, void *fn )
{
    BOOL ended;
    ULONG_PTR ret = guest_callback_run8( a0, a1, a2, a3, a4, a5, a6, a7, fn, &ended );

    if (ended) return 0;
    return ret;
}

static ULONG_PTR guest_callback_run9( ULONG_PTR a0, ULONG_PTR a1, ULONG_PTR a2,
                                      ULONG_PTR a3, ULONG_PTR a4, ULONG_PTR a5,
                                      ULONG_PTR a6, ULONG_PTR a7, ULONG_PTR a8,
                                      void *fn, BOOL *ended )
{
    ULONG_PTR ret;

    xstat_count_callback( fn );
    TRACE( "calling guest callback %p (%p,%p,%p,%p,%p,%p,%p,%p,%p)\n", fn,
           (void *)a0, (void *)a1, (void *)a2, (void *)a3, (void *)a4,
           (void *)a5, (void *)a6, (void *)a7, (void *)a8 );
    ret = call_guest_function_args9( fn, a0, a1, a2, a3, a4, a5, a6, a7, a8 );
    if ((*ended = guest_exit_requested)) return 0;
    TRACE( "guest callback %p returned %p\n", fn, (void *)ret );
    return ret;
}

static ULONG_PTR guest_callback_dispatch9( ULONG_PTR a0, ULONG_PTR a1, ULONG_PTR a2,
                                           ULONG_PTR a3, ULONG_PTR a4, ULONG_PTR a5,
                                           ULONG_PTR a6, ULONG_PTR a7, ULONG_PTR a8,
                                           void *fn )
{
    BOOL ended;
    ULONG_PTR ret = guest_callback_run9( a0, a1, a2, a3, a4, a5, a6, a7, a8, fn, &ended );

    if (ended) return 0;
    return (ULONG_PTR)(LONG_PTR)(LONG)ret;
}

static ULONG_PTR guest_callback_dispatch9_wide( ULONG_PTR a0, ULONG_PTR a1, ULONG_PTR a2,
                                                ULONG_PTR a3, ULONG_PTR a4, ULONG_PTR a5,
                                                ULONG_PTR a6, ULONG_PTR a7, ULONG_PTR a8,
                                                void *fn )
{
    BOOL ended;
    ULONG_PTR ret = guest_callback_run9( a0, a1, a2, a3, a4, a5, a6, a7, a8, fn, &ended );

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

/* argc is 4 through 9: which fixed-arity trampoline pair (dispatch,
 * dispatch_wide) the stub hands control to, and -- through seven -- which
 * register carries the guest_fn identity; at eight and nine the stub is the
 * call-shaped one instead.  See the comment on struct
 * guest_callback_stub.code above. */
static void *wrap_guest_callback_ex( void *fn, BOOL wide, UINT argc )
{
    static int nowrap = -1;
    struct guest_callback_stub *stub;
    ULONG_PTR magic;
    void *ret = fn;
    UINT *p, i, fn_reg;
    ULONG_PTR dispatch;   /* a function pointer, cast the same way the single
                          * dispatch it used to be always was -- see the
                          * emit_load_imm64 call below */

    if (!fn) return fn;

    switch (argc)
    {
    case 4: dispatch = (ULONG_PTR)(wide ? guest_callback_dispatch_wide  : guest_callback_dispatch);  break;
    case 5: dispatch = (ULONG_PTR)(wide ? guest_callback_dispatch5_wide : guest_callback_dispatch5); break;
    case 6: dispatch = (ULONG_PTR)(wide ? guest_callback_dispatch6_wide : guest_callback_dispatch6); break;
    case 7: dispatch = (ULONG_PTR)(wide ? guest_callback_dispatch7_wide : guest_callback_dispatch7); break;
    case 8: dispatch = (ULONG_PTR)(wide ? guest_callback_dispatch8_wide : guest_callback_dispatch8); break;
    case 9: dispatch = (ULONG_PTR)(wide ? guest_callback_dispatch9_wide : guest_callback_dispatch9); break;
    default:
        ERR( "guest callback %p registered with unsupported arity %u\n", fn, argc );
        return fn;
    }
    fn_reg = 3 + argc;   /* meaningful only for the tail-jump shape (argc <= 7) */

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

    /* One stub per distinct (target, return width, arity), across every
     * block.  Width and arity are both part of the identity rather than a
     * property of the target, because the same guest function genuinely can
     * be registered as two different callback kinds -- and a lookup that
     * ignored either would hand the second registration the first one's
     * stub, truncating its result or misreading its fifth/sixth argument. */
    {
        UINT b;
        for (b = 0; b < guest_cb_blocks; b++)
        {
            UINT used = (b + 1 == guest_cb_blocks) ? guest_cb_count : GUEST_CB_BLOCK;
            for (i = 0; i < used; i++)
                if (guest_cb_block[b][i].guest_fn == fn &&
                    guest_cb_block[b][i].wide == (wide ? 1u : 0u) &&
                    guest_cb_block[b][i].argc == argc)
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
    if (argc <= 7)
    {
        /* the original tail jump: the identity rides the register one past
         * the last real argument, and the dispatcher returns straight to
         * our caller */
        p = emit_load_imm64( p, fn_reg, (ULONG_PTR)fn );
        p = emit_load_imm64( p, 12, dispatch );
        *p++ = 0x7D8903A6;   /* mtctr r12 */
        *p++ = 0x4E800420;   /* bctr */
    }
    else
    {
        /* Eight or nine arguments: r3..r10 are all spoken for, so this is a
         * real call with a frame of our own -- see the pool banner for why a
         * tail jump cannot serve these two arities.  The frame is 112 bytes:
         * the 32-byte header plus a parameter save area wide enough for the
         * dispatcher's ten possible slots, 16-byte aligned.  Words
         * machine-verified with llvm-mc, not reasoned about by eye. */
        *p++ = 0x7C0802A6;               /* mflr r0 */
        if (argc == 9)
            *p++ = 0xE9610060;           /* ld   r11,96(r1) -- the ninth real
                                          * argument, from the native caller's
                                          * own parameter save area, which
                                          * nine arguments oblige it to have */
        *p++ = 0xF8010010;               /* std  r0,16(r1) -- LR, at the ABI's
                                          * slot in the CALLER's frame */
        *p++ = 0xF821FF91;               /* stdu r1,-112(r1) */
        if (argc == 9)
            *p++ = 0xF9610060;           /* std  r11,96(r1) -- slot 8: the
                                          * ninth argument again, now in the
                                          * dispatcher's parameter save area */
        p = emit_load_imm64( p, 11, (ULONG_PTR)fn );
        *p++ = (argc == 9) ? 0xF9610068  /* std  r11,104(r1) -- guest_fn as the
                                          * dispatcher's trailing parameter:
                                          * slot 9 at nine arguments... */
                           : 0xF9610060; /* std  r11,96(r1)  -- ...slot 8 at
                                          * eight */
        p = emit_load_imm64( p, 12, dispatch );
        *p++ = 0x7D8903A6;               /* mtctr r12 */
        *p++ = 0x4E800421;               /* bctrl */
        *p++ = 0x38210070;               /* addi r1,r1,112 */
        *p++ = 0xE8010010;               /* ld   r0,16(r1) */
        *p++ = 0x7C0803A6;               /* mtlr r0 */
        *p++ = 0x4E800020;               /* blr -- r3 carries the dispatcher's
                                          * result through untouched */
    }
    stub->guest_fn = fn;
    stub->wide     = wide ? 1u : 0u;
    stub->argc     = argc;
    NtFlushInstructionCache( GetCurrentProcess(), stub, sizeof(*stub) );
    guest_cb_count++;    /* publish only after the code is flushed */
    TRACE( "guest callback %p -> trampoline %p (%u total, %s return, %u args)\n",
           fn, stub, guest_cb_count, wide ? "64-bit" : "sign-extended 32-bit", argc );
    ret = stub;
done:
    LdrUnlockLoaderLock( 0, magic );
    return ret;
}

static void *wrap_guest_callback( void *fn )
{
    return wrap_guest_callback_ex( fn, FALSE, 4 );
}

/* The trampoline factory, exported for guest-COM modules.  A COM method traps
 * inside a vtable stub array and is routed by RIP arithmetic, so the override
 * table above -- keyed on flat export names -- can never reach it; a module
 * whose hand slot receives a guest callback (dinput8's EnumDevices, comdlg32's
 * dialog hooks) resolves this with LdrGetProcedureAddress instead, so a tree
 * without it refuses loudly rather than failing to load. */
void * CDECL __wine_guest_wrap_callback( void *fn, BOOL wide )
{
    return wrap_guest_callback_ex( fn, wide, 4 );
}

/* The five- and six-argument factory exports, same shape as
 * __wine_guest_wrap_callback above but naming the arity the way the existing
 * one names the return width -- exactly the follow-up comctl32.thunks and
 * wininet.thunks both ask for by name (__wine_guest_wrap_callback6 for
 * SetWindowSubclass/RemoveWindowSubclass's SUBCLASSPROC, this one's sibling
 * for InternetSetStatusCallback's INTERNET_STATUS_CALLBACK).  Arity is a
 * property of the SLOT, exactly as width already is: the pool above keys on
 * (target, width, argc) so wrapping the same guest function through two
 * different factory exports can never be confused for one registration. */
void * CDECL __wine_guest_wrap_callback5( void *fn, BOOL wide )
{
    return wrap_guest_callback_ex( fn, wide, 5 );
}

void * CDECL __wine_guest_wrap_callback6( void *fn, BOOL wide )
{
    return wrap_guest_callback_ex( fn, wide, 6 );
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
    return wrap_guest_callback_ex( fn, TRUE, 4 );
}

/* swap the arguments a thunk_override row declares as callbacks; `wide` names
 * the subset of them whose return value is a full 64 bits, and `cb_argc` how
 * many arguments those callbacks themselves take.
 *
 * That last one used to be the literal 4, with a comment saying a row needing
 * five or six would pass its own argc here instead.  This is that row:
 * _set_invalid_parameter_handler's callback takes five, and minting it at four
 * would put the pool's identity register on top of the fifth argument native
 * code had already placed there.  Zero keeps the original shape, so every row
 * written before the field existed means exactly what it meant then. */
static void wrap_thunk_callback_args( ULONG_PTR *a, UINT argc, UINT mask, UINT wide, UINT cb_argc )
{
    UINT i;
    if (!cb_argc) cb_argc = 4;
    for (i = 0; i < argc; i++)
        if (mask & (1u << i))
            a[i] = (ULONG_PTR)wrap_guest_callback_ex( (void *)a[i], (wide >> i) & 1, cb_argc );
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
 * WHAT EACH SIDE READS BACK.  NATIVE code reading GWLP_WNDPROC sees the
 * trampoline, which from user32's point of view IS the window procedure --
 * correct, native must be able to call what it reads.  The GUEST reading it
 * back through GetWindowLongPtrA/W, or receiving it as SetWindowLongPtr's
 * previous-value return, gets the pool stub UNWRAPPED to the guest function
 * it stands for (emu_GetWindowLongPtr, and the setter's return path): what a
 * guest registered is what a guest reads back, exactly as on Windows.  An
 * earlier revision of this comment declared the trampoline-visible answer
 * accepted and the read-back-and-compare idiom one "no correct program
 * does" -- and then Quake II's SDL2 did precisely that comparison, missed,
 * subclassed a window with its own procedure as its own "previous", and
 * recursed to kernel-stack exhaustion on the first message.  See
 * unwrap_guest_wndproc for the full mechanism.  GetClassLongPtr(
 * GCLP_WNDPROC ) still returns the trampoline to a guest: nothing in the
 * corpus reads it, and the row belongs beside these two when something does.
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
        ULONG_PTR prev;

        value = (ULONG_PTR)wrap_guest_wndproc( (void *)a[2] );
        prev = ((ULONG_PTR (*)( ULONG_PTR, ULONG_PTR, ULONG_PTR ))native)( a[0], a[1], value );
        /* the previous procedure travels back INTO the guest: a pool stub
         * must go back as the guest function it stands for, or a subclasser
         * that compares (SDL) or chains (everything) sees our plumbing --
         * see unwrap_guest_wndproc for the measured failure */
        prev = (ULONG_PTR)unwrap_guest_wndproc( (void *)prev );
        TRACE( "SetWindowLongPtr(%p, GWLP_WNDPROC): %p -> %p, prev %p\n",
               (void *)a[0], (void *)a[2], (void *)value, (void *)prev );
        return prev;
    }
    return ((ULONG_PTR (*)( ULONG_PTR, ULONG_PTR, ULONG_PTR ))native)( a[0], a[1], value );
}

static ULONG_PTR emu_GetWindowLongPtr( const ULONG_PTR *a, void *native )
{
    ULONG_PTR value;

    if (!native) return 0;
    value = ((ULONG_PTR (*)( ULONG_PTR, ULONG_PTR ))native)( a[0], a[1] );
    /* only the one index, same as the setter: everything else is data */
    if ((int)(LONG)a[1] == EMU_GWLP_WNDPROC)
    {
        ULONG_PTR unwrapped = (ULONG_PTR)unwrap_guest_wndproc( (void *)value );
        if (unwrapped != value)
            TRACE( "GetWindowLongPtr(%p, GWLP_WNDPROC): %p -> guest %p\n",
                   (void *)a[0], (void *)value, (void *)unwrapped );
        value = unwrapped;
    }
    return value;
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

/* GCLP_WNDPROC: the class-level analogue of GWLP_WNDPROC, found by the
 * pointer-identity audit (ppc64le/docs/sessions/2026-08-29/
 * pointer-identity-audit.md) rather than by a title exercising it -- exactly
 * the position the WNDPROC row itself was in until Quake II's SDL2 did.
 * RegisterClass/RegisterClassEx above already wrap a class's lpfnWndProc, so
 * GetClassLongPtr( GCLP_WNDPROC ) was returning the pool stub to the guest
 * unexamined, the same shape of miss for a title that superclasses its OWN
 * class (or another guest module's) by index rather than by
 * GetClassInfo(Ex).  Same fix, same function: unwrap_guest_wndproc. */
#define EMU_GCLP_WNDPROC  (-24)

static ULONG_PTR emu_GetClassLongPtr( const ULONG_PTR *a, void *native )
{
    ULONG_PTR value;

    if (!native) return 0;
    value = ((ULONG_PTR (*)( ULONG_PTR, ULONG_PTR ))native)( a[0], a[1] );
    if ((int)(LONG)a[1] == EMU_GCLP_WNDPROC)
    {
        ULONG_PTR unwrapped = (ULONG_PTR)unwrap_guest_wndproc( (void *)value );
        if (unwrapped != value)
            TRACE( "GetClassLongPtr(%p, GCLP_WNDPROC): %p -> guest %p\n",
                   (void *)a[0], (void *)value, (void *)unwrapped );
        value = unwrapped;
    }
    return value;
}

/* The struct-shaped route to the same field: GetClassInfo(Ex) fills in a
 * WNDCLASS(EX) whose lpfnWndProc is the class's, which is wrapped for the
 * identical reason GetClassLongPtr's is -- a title superclassing a class
 * registered by its own (or another guest) module reads this back and may
 * compare it before deciding whether to chain.  Only touched on success:
 * GetClassInfoEx fails without writing the struct at all when cbSize is
 * wrong, and touching lpfnWndProc first would read past what native wrote
 * (or, worse, past what it left alone). */
static ULONG_PTR emu_GetClassInfo( const ULONG_PTR *a, void *native )
{
    ULONG_PTR ret;
    struct emu_wndclass *out = (struct emu_wndclass *)a[2];

    if (!native) return 0;
    ret = ((ULONG_PTR (*)( ULONG_PTR, ULONG_PTR, ULONG_PTR ))native)( a[0], a[1], a[2] );
    if (ret && out)
    {
        void *unwrapped = unwrap_guest_wndproc( out->lpfnWndProc );
        if (unwrapped != out->lpfnWndProc)
            TRACE( "GetClassInfo(%p): wndproc %p -> guest %p\n",
                   out, out->lpfnWndProc, unwrapped );
        out->lpfnWndProc = unwrapped;
    }
    return ret;
}

static ULONG_PTR emu_GetClassInfoEx( const ULONG_PTR *a, void *native )
{
    ULONG_PTR ret;
    struct emu_wndclassex *out = (struct emu_wndclassex *)a[2];

    if (!native) return 0;
    ret = ((ULONG_PTR (*)( ULONG_PTR, ULONG_PTR, ULONG_PTR ))native)( a[0], a[1], a[2] );
    if (ret && out)
    {
        void *unwrapped = unwrap_guest_wndproc( out->lpfnWndProc );
        if (unwrapped != out->lpfnWndProc)
            TRACE( "GetClassInfoEx(%p): wndproc %p -> guest %p\n",
                   out, out->lpfnWndProc, unwrapped );
        out->lpfnWndProc = unwrapped;
    }
    return ret;
}

/***********************************************************************
 *           guest fibers
 *
 * A fiber is a stack with a program counter parked on it, switched by hand.
 * kernelbase implements them for native code and this port now has the ppc64
 * half of that switch (dlls/kernelbase/thread.c, switch_fiber).  A GUEST
 * fiber is served here instead, and does not go through kernelbase at all.
 *
 * WHY NOT SIMPLY LET THE GUEST USE kernelbase's.  It was tried, and the way
 * it fails is worth writing down.  kernelbase gives each fiber a NATIVE
 * stack; a guest fiber's start routine would then be an ordinary wrapped
 * callback, so entering it opens a SECOND emulator run, and switching away
 * from that fiber abandons that run without ever returning from it.  The
 * emulator's per-thread state is not a stack of independent runs -- the trap
 * this port dispatches from is called from INSIDE fexbridge_run -- so the
 * outer run resumes holding the inner run's register file.  Measured: the
 * first switch back from a fiber re-entered the fiber instead of returning
 * to the switcher, forever.
 *
 * So a guest fiber is exactly what it is on Windows and nothing more: a
 * guest stack plus a saved guest CONTEXT.  There is one emulator run per
 * thread at any moment, and a switch REPLACES the context that run resumes
 * from -- the same mechanism a guest __except already resumes through
 * (emu_trap_ctx_rewritten).  No native stack per fiber, no nested run, and
 * nothing the emulator has to be told.
 *
 * WHAT A SWITCH MOVES.  The saved context is the switching fiber's, fixed up
 * as its own return: SwitchToFiber returns void, so the fiber resumes at the
 * return address its CALL pushed, one slot further up its own stack.  The
 * incoming fiber's context is installed wholesale, and the guest stack
 * BOUNDS move with it -- the emulator lane keeps them per run, the guest's
 * TEB is set from them, and every __chkstk probe and SEH frame check reads
 * them.  A switch that moved the context and not the bounds would leave a
 * fiber running on its own stack while being told it is on another's, which
 * is silent until the first deep frame or the first exception.
 *
 * DOOM (2016) is the title that needs this: id Tech 6's job system is built
 * on fibers, and every run died at kernelbase's `switch_fiber not
 * implemented` FIXME before this existed.
 */
struct guest_fiber
{
    /* THE PARAMETER IS FIRST, AND THAT IS AN ABI, NOT A LAYOUT CHOICE.
     * GetFiberData() is a macro that dereferences the fiber pointer --
     * *(void **)GetCurrentFiber() -- so a guest reads its own argument out of
     * offset zero of whatever SwitchToFiber was given, without calling
     * anything.  Wine's own struct fiber_data puts it there for the same
     * reason. */
    void          *param;         /* the argument its start routine gets */
    AMD64_CONTEXT  ctx;           /* where it resumes; valid while parked */
    void          *start;         /* guest LPFIBER_START_ROUTINE, or NULL for
                                     the fiber a thread converted itself into */
    void          *stack_base;    /* the guest stack: ours to free unless... */
    void          *stack_limit;
    void          *stack_alloc;
    BOOL           owns_stack;    /* ...this is FALSE, for a converted thread,
                                     whose stack belongs to the run */
    BOOL           started;
    ULONG          magic;         /* GUEST_FIBER_MAGIC while live */
};

/* A guest hands SwitchToFiber a pointer it got from GetCurrentFiber(), which
 * is a macro reading the TEB rather than anything this file can vet -- so the
 * pointer is checked before 1232 bytes are copied out of it.  DOOM (2016)
 * passed a stale small value that way and the copy faulted inside ntdll,
 * which is a far worse report than the refusal below. */
#define GUEST_FIBER_MAGIC 0x46424752   /* 'FBGR' */

static BOOL guest_fiber_valid( const struct guest_fiber *fiber )
{
    return fiber && !((ULONG_PTR)fiber & 7) && fiber->magic == GUEST_FIBER_MAGIC;
}

static EMU_THREAD_VAR struct guest_fiber *guest_current_fiber;

/* WHAT THE TEB HAS TO SAY, because two of the fiber API's four answers are
 * not calls at all.  GetCurrentFiber() and GetFiberData() are macros that
 * read Tib.FiberData directly -- no export, nothing to intercept -- so a
 * guest gets its own fiber handle from there or not at all.  DOOM (2016)
 * does exactly that: it handed SwitchToFiber whatever was in that field, and
 * with the field left alone that was a stale small value which this file then
 * treated as a fiber and copied 1232 bytes out of.
 *
 * HasFiberData is what IsThreadAFiber reads, and the two are kept in step
 * here rather than at each call site. */
static void guest_fiber_make_current( struct guest_fiber *fiber )
{
    guest_current_fiber = fiber;
    NtCurrentTeb()->Tib.FiberData = fiber;
    NtCurrentTeb()->HasFiberData  = fiber != NULL;
}

/* The guest stack bounds the running emulator run is using, and the HLT page
 * a guest entry frame returns to.  Both live on the unix side of this lane;
 * see unixcall_emu_fiber_stack. */
static NTSTATUS guest_fiber_stack( struct emu_fiber_params *params )
{
    return WINE_UNIX_CALL( unix_emu_fiber_stack, params );
}

static struct guest_fiber *guest_fiber_alloc(void)
{
    struct guest_fiber *fiber = RtlAllocateHeap( GetProcessHeap(), HEAP_ZERO_MEMORY,
                                                 sizeof(*fiber) );
    if (!fiber) RtlSetLastWin32Error( ERROR_NOT_ENOUGH_MEMORY );
    else fiber->magic = GUEST_FIBER_MAGIC;
    return fiber;
}

/* ConvertThreadToFiber(Ex): the running guest execution becomes a fiber.  It
 * keeps the stack it is already on -- the run's -- and its context is not
 * written until it switches away, because until then the context IS the
 * running one. */
static ULONG_PTR emu_ConvertThreadToFiber( const ULONG_PTR *a, void *native )
{
    struct emu_fiber_params stack = { 0 };
    struct guest_fiber *fiber;

    if (guest_current_fiber)
    {
        RtlSetLastWin32Error( ERROR_ALREADY_FIBER );
        return 0;
    }
    if (guest_fiber_stack( &stack ) || !stack.base)
    {
        ERR( "ConvertThreadToFiber outside a guest run: there is no guest stack "
             "to make a fiber out of\n" );
        RtlSetLastWin32Error( ERROR_INVALID_PARAMETER );
        return 0;
    }
    if (!(fiber = guest_fiber_alloc())) return 0;
    fiber->param       = (void *)a[0];
    fiber->stack_base  = stack.base;
    fiber->stack_limit = stack.limit;
    fiber->stack_alloc = stack.dealloc;
    fiber->owns_stack  = FALSE;
    fiber->started     = TRUE;
    guest_fiber_make_current( fiber );
    TRACE( "thread is now fiber %p on stack %p-%p\n", fiber, stack.limit, stack.base );
    return (ULONG_PTR)fiber;
}

static ULONG_PTR emu_ConvertFiberToThread( const ULONG_PTR *a, void *native )
{
    struct guest_fiber *fiber = guest_current_fiber;

    if (!fiber)
    {
        RtlSetLastWin32Error( ERROR_ALREADY_THREAD );
        return 0;
    }
    guest_fiber_make_current( NULL );
    RtlFreeHeap( GetProcessHeap(), 0, fiber );
    return 1;
}

static ULONG_PTR emu_IsThreadAFiber( const ULONG_PTR *a, void *native )
{
    return guest_current_fiber != NULL;
}

/* CreateFiber(Ex): a guest stack and a context parked at the start routine.
 *
 * The context is COPIED from the one this call trapped out of and then
 * edited, so every field a fresh one would have to invent -- the segment
 * registers, EFlags, MxCsr, ContextFlags -- is the running guest's own
 * rather than this file's guess at it.
 *
 * The entry frame is the MS-x64 shape the emulator's own run entry builds:
 * 32 bytes of shadow space, a pushed return address, RCX carrying the single
 * argument.  That return address is the HLT page, so a fiber whose start
 * routine RETURNS ends the run -- which is what Windows does too: "if the
 * fiber's start routine returns, the thread exits". */
static ULONG_PTR emu_CreateFiber( const ULONG_PTR *a, void *native, ULONG_PTR stack_reserve,
                                  ULONG_PTR stack_commit, void *start, void *param )
{
    struct emu_fiber_params stack = { 0 };
    struct guest_fiber *fiber;
    INITIAL_TEB teb_stack;
    NTSTATUS status;
    ULONG_PTR rsp;

    if (!emu_current_trap_ctx)
    {
        ERR( "CreateFiber outside a guest trap: there is no guest context to "
             "build a fiber's first one from\n" );
        RtlSetLastWin32Error( ERROR_INVALID_PARAMETER );
        return 0;
    }
    if (guest_fiber_stack( &stack ) || !stack.hlt)
    {
        ERR( "CreateFiber: the emulator lane has no HLT page, so a fiber whose "
             "start routine returns would return to nowhere\n" );
        RtlSetLastWin32Error( ERROR_INVALID_PARAMETER );
        return 0;
    }
    if (!(fiber = guest_fiber_alloc())) return 0;

    status = RtlCreateUserStack( stack_commit, stack_reserve, 0, 1, 1, &teb_stack );
    if (status)
    {
        ERR( "cannot allocate a guest fiber stack (commit %Ix reserve %Ix): %08x\n",
             stack_commit, stack_reserve, (UINT)status );
        RtlFreeHeap( GetProcessHeap(), 0, fiber );
        RtlSetLastWin32Error( ERROR_NOT_ENOUGH_MEMORY );
        return 0;
    }
    fiber->param       = param;
    fiber->start       = start;
    fiber->stack_base  = teb_stack.StackBase;
    fiber->stack_limit = teb_stack.StackLimit;
    fiber->stack_alloc = teb_stack.DeallocationStack;
    fiber->owns_stack  = TRUE;

    materialize_trap_ctx( emu_current_trap_ctx );   /* the copy takes EFlags/MxCsr too */
    fiber->ctx = *emu_current_trap_ctx;
    rsp = ((ULONG_PTR)teb_stack.StackBase & ~(ULONG_PTR)15) - 0x28;
    *(ULONG64 *)rsp = stack.hlt;
    fiber->ctx.Rsp = rsp;
    fiber->ctx.Rip = (ULONG64)(ULONG_PTR)start;
    fiber->ctx.Rcx = (ULONG64)(ULONG_PTR)param;
    fiber->ctx.Rax = 0;
    TRACE( "fiber %p: start %p param %p stack %p-%p\n", fiber, start, param,
           teb_stack.DeallocationStack, teb_stack.StackBase );
    return (ULONG_PTR)fiber;
}

static ULONG_PTR emu_CreateFiber3( const ULONG_PTR *a, void *native )
{
    /* CreateFiber(dwStackSize, start, param): one size, which Windows treats
     * as the COMMIT and reserves the image's default around. */
    return emu_CreateFiber( a, native, 0, a[0], (void *)a[1], (void *)a[2] );
}

static ULONG_PTR emu_CreateFiberEx( const ULONG_PTR *a, void *native )
{
    /* CreateFiberEx(commit, reserve, flags, start, param).  FIBER_FLAG_FLOAT_
     * SWITCH is ignored for the reason kernelbase ignores it: the whole guest
     * context travels, floating point included, so the flag asks for
     * something that already happens. */
    return emu_CreateFiber( a, native, a[1], a[0], (void *)a[3], (void *)a[4] );
}

static void guest_fiber_free( struct guest_fiber *fiber )
{
    fiber->magic = 0;
    if (fiber->owns_stack && fiber->stack_alloc) RtlFreeUserStack( fiber->stack_alloc );
    RtlFreeHeap( GetProcessHeap(), 0, fiber );
}

static ULONG_PTR emu_DeleteFiber( const ULONG_PTR *a, void *native )
{
    struct guest_fiber *fiber = (struct guest_fiber *)a[0];

    if (!fiber) return 0;
    if (!guest_fiber_valid( fiber ))
    {
        ERR( "DeleteFiber(%p) refused: not a fiber this process made\n", fiber );
        return 0;
    }
    if (fiber == guest_current_fiber)
    {
        /* Windows: deleting the RUNNING fiber terminates the thread.  Said
         * out loud rather than done silently, because a guest that reaches
         * this has almost certainly confused two fiber handles. */
        ERR( "DeleteFiber on the running fiber %p: the thread ends here, which "
             "is what Windows does\n", fiber );
        guest_fiber_make_current( NULL );
        guest_fiber_free( fiber );
        RtlExitUserThread( 1 );
    }
    guest_fiber_free( fiber );
    return 0;
}

/* SwitchToFiber(fiber).  The switch is a context replacement, so it happens
 * entirely in the trap this call arrived through: the outgoing fiber's
 * context is this trap's, fixed up as an ordinary void return, and the
 * incoming fiber's context replaces it.  Nothing unwinds and nothing nests. */
static ULONG_PTR emu_SwitchToFiber( const ULONG_PTR *a, void *native )
{
    static int no_fiber_stacks = -1;
    struct guest_fiber *target = (struct guest_fiber *)a[0];
    struct guest_fiber *cur = guest_current_fiber;
    struct emu_fiber_params stack = { 0 };
    AMD64_CONTEXT *ctx = emu_current_trap_ctx;

    if (!guest_fiber_valid( target ) || !cur || !ctx)
    {
        ERR( "SwitchToFiber(%p) refused: it is %s, the current fiber is %p and the "
             "trap context %p -- a switch needs a fiber this process made and a "
             "running guest fiber to switch away from\n", target,
             guest_fiber_valid( target ) ? "a fiber" : "not a fiber of ours",
             cur, ctx );
        return 0;
    }
    if (target == cur) return 0;

    /* park the outgoing one, as its own return.  Its committed limit is read
     * back rather than remembered: a stack that grew while this fiber ran
     * moved it, and reinstalling the value from when the fiber was made would
     * hand the guest a TEB describing pages it has already committed as if
     * they were still guard. */
    stack.op = EMU_FIBER_QUERY;
    if (!guest_fiber_stack( &stack ) && stack.limit) cur->stack_limit = stack.limit;
    memset( &stack, 0, sizeof(stack) );
    /* the park copies the whole register file AND the wholesale replace below
     * writes the whole register file: both halves need the groups real */
    materialize_trap_ctx( ctx );
    cur->ctx      = *ctx;
    cur->ctx.Rip  = *(DWORD64 *)(ULONG_PTR)ctx->Rsp;
    cur->ctx.Rsp  = ctx->Rsp + 8;
    cur->ctx.Rax  = 0;
    cur->started  = TRUE;

    /* ...and resume the incoming one in its place */
    *ctx = target->ctx;
    emu_trap_ctx_rewritten = TRUE;
    guest_fiber_make_current( target );

    /* THE BOUNDS MOVE WITH IT.  The negative control leaves them behind,
     * which is the whole bug this pays for: the resumed fiber runs on its own
     * stack while its TEB describes the last one's. */
    if (no_fiber_stacks == -1)
    {
        no_fiber_stacks = emu_env_flag( L"WINEEMUNOFIBERSTATE" );
        if (no_fiber_stacks)
            ERR( "WINEEMUNOFIBERSTATE: a fiber switch will not move the guest "
                 "stack bounds, so a resumed fiber is told whichever stack the "
                 "last one left behind\n" );
    }
    if (!no_fiber_stacks)
    {
        stack.op      = EMU_FIBER_SET_STACK;
        stack.base    = target->stack_base;
        stack.limit   = target->stack_limit;
        stack.dealloc = target->stack_alloc;
        guest_fiber_stack( &stack );
    }
    TRACE( "fiber %p -> %p: rip %I64x rsp %I64x stack %p-%p\n", cur, target,
           ctx->Rip, ctx->Rsp, target->stack_alloc, target->stack_base );
    return 0;
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

/* SET-returns-the-previous-one registrations, found by the pointer-identity
 * audit (ppc64le/docs/sessions/2026-08-29/pointer-identity-audit.md) that
 * followed the WNDPROC fix.  Each of these used to be a plain cb_mask row --
 * wrap the incoming callback, hand the raw native return straight back --
 * which is exactly the WNDPROC bug's shape transplanted onto a different
 * API: the "previous" value handed back to the guest was OUR pool stub, not
 * what the guest itself registered, so a guest that COMPARES it (rather than
 * just calling it) takes the wrong branch.
 *
 * SetUnhandledExceptionFilter is the highest-risk of the three: saving the
 * previous filter and comparing or chaining it --
 *
 *     LPTOP_LEVEL_EXCEPTION_FILTER old = SetUnhandledExceptionFilter( mine );
 *     if (old != mine) g_prev = old;   // avoid re-chaining to ourselves
 *
 * -- is documented, ordinary practice (crash-reporting libraries installing
 * exactly once, or chaining to whatever filter came before), not the
 * "no correct program does that" case this port has already been wrong
 * about once.  mmioInstallIOProc's MMIO_FINDPROC/MMIO_REMOVEPROC modes and
 * _set_new_handler's save-and-restore share the identical shape; the RETURN
 * has to be unwrap_guest_cb'd for the same reason across all three. */
static ULONG_PTR emu_SetUnhandledExceptionFilter( const ULONG_PTR *a, void *native )
{
    ULONG_PTR wrapped, prev;

    if (!native) return 0;
    wrapped = (ULONG_PTR)wrap_guest_callback( (void *)a[0] );
    prev = ((ULONG_PTR (*)( ULONG_PTR ))native)( wrapped );
    prev = (ULONG_PTR)unwrap_guest_cb( (void *)prev );
    TRACE( "SetUnhandledExceptionFilter(%p -> %p): prev %p\n",
           (void *)a[0], (void *)wrapped, (void *)prev );
    return prev;
}

/* mmioInstallIOProc(A/W): MMIO_INSTALLPROC returns the argument handed in
 * (already correct, wrapped or not, because it is the SAME value both
 * sides just agreed on), but MMIO_FINDPROC and MMIO_REMOVEPROC return
 * whatever is STORED for the fourCC -- our pool stub, if a guest installed
 * it -- straight from dlls/winmm/mmio.c's IOProcList.  The callback itself
 * is LPMMIOPROC, four arguments returning LRESULT, hence wide=TRUE. */
static ULONG_PTR emu_mmioInstallIOProc( const ULONG_PTR *a, void *native )
{
    ULONG_PTR wrapped, prev;

    if (!native) return 0;
    wrapped = (ULONG_PTR)wrap_guest_callback_ex( (void *)a[1], TRUE, 4 );
    prev = ((ULONG_PTR (*)( ULONG_PTR, ULONG_PTR, ULONG_PTR ))native)( a[0], wrapped, a[2] );
    prev = (ULONG_PTR)unwrap_guest_cb( (void *)prev );
    TRACE( "mmioInstallIOProc(%08x, %p -> %p, %#x): prev %p\n",
           (UINT)a[0], (void *)a[1], (void *)wrapped, (UINT)a[2], (void *)prev );
    return prev;
}

/* msvcr100's _set_new_handler: the C++ new-handler, `int (*)(size_t)`.  Same
 * save-and-restore shape as the two above; see the row's old comment (kept
 * below, in the table) for why this one is reached at all. */
static ULONG_PTR emu_set_new_handler( const ULONG_PTR *a, void *native )
{
    ULONG_PTR wrapped, prev;

    if (!native) return 0;
    wrapped = (ULONG_PTR)wrap_guest_callback( (void *)a[0] );
    prev = ((ULONG_PTR (*)( ULONG_PTR ))native)( wrapped );
    prev = (ULONG_PTR)unwrap_guest_cb( (void *)prev );
    return prev;
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
    /* Vulkan's runtime-vended entry points: the answer is a code address the
     * guest will CALL, so it must be the guest thunk module's own stub; see
     * emu_vkGetProcAddr above.  Both rows are (handle, name). */
    { L"vulkan-1.dll",   "vkGetInstanceProcAddr", 2, emu_vkGetProcAddr },
    { L"vulkan-1.dll",   "vkGetDeviceProcAddr",   2, emu_vkGetProcAddr },
    /* native->guest: the pointer is queued here and run by our own native
     * handler at exit; see run_guest_atexit_handlers */
    { L"ucrtbase.dll", "_crt_atexit",       1, emu_crt_atexit },
    /* ucrtbase's _o_-prefixed aliases bind to the SAME native registration
     * points, so each alias of a row above needs its own row -- the table is
     * keyed by export name, and _o__crt_atexit arriving without one would
     * park a raw guest pointer in the native atexit queue.  The aliases only
     * became reachable when spec2thunk learned to take an alias's signature
     * from its target; before that they were refusals. */
    { L"ucrtbase.dll", "_o__crt_atexit",    1, emu_crt_atexit },
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
    { L"msvcr120.dll", "_onexit",           1, emu_onexit },
    /* The C++ new-handler, `int (*)(size_t)`, which native operator new calls
     * when an allocation fails.  The shape it shares with
     * SetUnhandledExceptionFilter is the reason it gets its own handler
     * function rather than a plain cb_mask row: both are a SET rather than a
     * queue, and both RETURN THE ONE THEY REPLACED, so the guest's
     * save-and-restore idiom
     *
     *     _PNH old = _set_new_handler( mine );  ...  _set_new_handler( old );
     *
     * hands our trampoline back in.  Restoring it is fine either way --
     * wrap_guest_callback re-wraps an already-wrapped value idempotently --
     * but the pointer-identity audit
     * (ppc64le/docs/sessions/2026-08-29/pointer-identity-audit.md) found
     * that an EARLIER revision of this comment stopped there and called a
     * guest CALLING or COMPARING the returned value instead "the same
     * accepted limit the exception-filter row has carried" -- the exact
     * reasoning that turned out wrong for GWLP_WNDPROC's read-back
     * (dlls/ntdll/signal_ppc64.c's unwrap_guest_wndproc banner).  Fixed the
     * same way: emu_set_new_handler unwraps the previous value before it
     * reaches the guest, so both the call idiom and any comparison idiom see
     * exactly what they registered.
     *
     * Why it needs a row at all: dlls/msvcrt/heap.c keeps the pointer in a
     * LOCK_HEAP'd global and calls it from operator new's failure path, so an
     * unwrapped guest address does not fault at registration -- it faults the
     * first time an allocation fails, under memory pressure, arbitrarily far
     * from the call that installed it.  msvcp100.dll's DllMain looks this name
     * up (see dlls/msvcr100/msvcr100.thunks), so any guest carrying the VC++
     * 2010 C++ runtime reaches it.  One argument, `int` back: the default
     * four-slot, sign-extended-32-bit trampoline is the right one. */
    { L"msvcr100.dll", "?_set_new_handler@@YAP6AH_K@ZP6AH0@Z@Z", 1, emu_set_new_handler },
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
     * in a stack overflow, which buried the REAL failure under it.  A
     * handler function rather than a plain cb_mask row since the PREVIOUS
     * filter this returns to the guest must be unwrapped too -- see
     * emu_SetUnhandledExceptionFilter and the pointer-identity audit. */
    { L"kernel32.dll",   "SetUnhandledExceptionFilter", 1, emu_SetUnhandledExceptionFilter },
    { L"kernelbase.dll", "SetUnhandledExceptionFilter", 1, emu_SetUnhandledExceptionFilter },
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
    { L"ucrtbase.dll", "_o_qsort",   4, NULL, 1u << 3 },
    { L"msvcr100.dll", "qsort",   4, NULL, 1u << 3 },
    { L"msvcrt.dll",   "bsearch", 5, NULL, 1u << 4 },
    { L"ucrtbase.dll", "bsearch", 5, NULL, 1u << 4 },
    { L"ucrtbase.dll", "_o_bsearch", 5, NULL, 1u << 4 },
    { L"msvcr100.dll", "bsearch", 5, NULL, 1u << 4 },
    /* The rest of the C runtime's registration points -- the ones
     * msvcr100.thunks named as STILL OPEN, each a raw guest pointer parked in
     * a native slot that native CRT code later calls directly.  This is the
     * FlsAlloc failure mode rather than the qsort one: a comparator is called
     * back before qsort returns, so a wrong row shows up at once, whereas
     * every registration below stores the pointer and returns success, and the
     * ELFv2 bctrl into x86-64 bytes happens minutes later on whichever thread
     * happens to divide by zero or start.  Styx: Master of Shadows imports
     * _beginthread, _beginthreadex, _set_invalid_parameter_handler,
     * _set_purecall_handler and __setusermatherr from msvcr100.
     *
     * Which module gets which row is read off the .spec files rather than
     * assumed symmetric, because they genuinely differ: msvcrt.dll exports
     * neither _set_invalid_parameter_handler nor _set_purecall_handler (it has
     * _invalid_parameter and _purecall, which are the CALL sites, not the
     * registrations), so it gets no row for either -- there is nothing to
     * intercept.  ucrtbase additionally publishes the _o_* forwarders that the
     * VC runtime imports in preference to the plain names, and a row keyed to
     * the plain name alone would pass those straight through; that is the
     * kernelbase lesson above, one export table further on.
     *
     * ARITY.  The pool's minimum slot is four arguments and surplus argument
     * registers are ignored by both ABIs, so a callback taking one argument or
     * none is served correctly by the default -- there is no real argument for
     * the identity register to land on.  _set_invalid_parameter_handler is the
     * exception and the reason cb_argc exists: its callback is
     *   void (__cdecl *)(const wchar_t *, const wchar_t *, const wchar_t *,
     *                    unsigned, uintptr_t)
     * (include/msvcrt/stdlib.h:263), five arguments, and dlls/msvcrt/errno.c:473
     * calls it with all five.  At the default arity the stub writes the guest
     * target into r7 -- ELFv2's FIFTH argument register, which native
     * _invalid_parameter has already loaded with pReserved -- and then passes
     * four.  That is not a crash: the handler reads a plausible wrong value,
     * which is the class this port treats as worse than a fault.
     *
     * WIDTH.  Four of the five callbacks return void, so nothing reads the
     * result and the default sign extension is unobservable.  __setusermatherr
     * is the one that is read: dlls/msvcrt/math.c:129 does
     * `if (MSVCRT_default_matherr_func && MSVCRT_default_matherr_func(&exception))`
     * and takes the handler's answer as "I handled it", so the default
     * sign-extended 32-bit return is required rather than merely harmless -- a
     * handler returning int must arrive as a full-width zero or non-zero.  None
     * of the five needs the wide slot: no LRESULT here.
     */
    /* _beginthread(start, stack_size, arglist): the start routine is argument
     * 0 and is `void (__cdecl *)(void *)` -- one argument, no return.  It needs
     * a row even though thread starts are normally intercepted at INVOCATION
     * (composition rule 1 above), because this pointer is not a thread start as
     * far as the kernel is concerned: dlls/msvcrt/thread.c:152 hands CreateThread
     * the NATIVE _beginthread_trampoline and parks the guest routine in a heap
     * struct, so RtlUserThreadStart classifies a native entry point, runs it
     * directly, and thread.c:127 then does start_address(arglist) -- a plain
     * bctrl straight into x86-64 bytes.  Rule 1 forbids a row for CreateThread's
     * OWN start routine; this is a different pointer on a different path. */
    { L"msvcrt.dll",   "_beginthread",     3, NULL, 1u << 0 },
    { L"msvcr100.dll", "_beginthread",     3, NULL, 1u << 0 },
    { L"msvcr120.dll", "_beginthread",     3, NULL, 1u << 0 },
    { L"ucrtbase.dll", "_beginthread",     3, NULL, 1u << 0 },
    { L"ucrtbase.dll", "_o__beginthread",  3, NULL, 1u << 0 },
    /* _beginthreadex(security, stack_size, start, arglist, initflag, thrdaddr):
     * the start routine is argument 2 and is `unsigned (__stdcall *)(void *)`.
     * __stdcall is the documented difference from _beginthread's routine and it
     * is a no-op here -- x86-64 Windows has one calling convention and the
     * attribute is ignored -- so what actually differs is the RETURN: unsigned
     * rather than void.  It still takes the default width, because
     * thread.c:201 feeds it to _endthreadex(unsigned), which reads 32 bits, so
     * the upper half the default sign-extends is discarded before anything
     * looks at it.  Same trampoline path as above: thread.c:238 gives
     * CreateThread the native _beginthreadex_trampoline. */
    { L"msvcrt.dll",   "_beginthreadex",     6, NULL, 1u << 2 },
    { L"msvcr100.dll", "_beginthreadex",     6, NULL, 1u << 2 },
    { L"msvcr120.dll", "_beginthreadex",     6, NULL, 1u << 2 },
    { L"ucrtbase.dll", "_beginthreadex",     6, NULL, 1u << 2 },
    { L"ucrtbase.dll", "_o__beginthreadex",  6, NULL, 1u << 2 },
    /* _set_invalid_parameter_handler(handler): one argument, and it is the
     * five-argument callback described above -- the only row in this table that
     * needs cb_argc.  The thread-local form stores into the per-thread slot
     * (dlls/msvcrt/errno.c:549) that errno.c:468 prefers over the global one;
     * it is the same callback shape, so it is the same row. */
    { L"msvcr100.dll", "_set_invalid_parameter_handler",                    1, NULL, 1u << 0, 0, 5 },
    { L"msvcr120.dll", "_set_invalid_parameter_handler",                    1, NULL, 1u << 0, 0, 5 },
    { L"ucrtbase.dll", "_set_invalid_parameter_handler",                    1, NULL, 1u << 0, 0, 5 },
    { L"ucrtbase.dll", "_o__set_invalid_parameter_handler",                 1, NULL, 1u << 0, 0, 5 },
    { L"ucrtbase.dll", "_set_thread_local_invalid_parameter_handler",       1, NULL, 1u << 0, 0, 5 },
    { L"ucrtbase.dll", "_o__set_thread_local_invalid_parameter_handler",    1, NULL, 1u << 0, 0, 5 },
    /* _set_purecall_handler(handler): `void (__cdecl *)(void)`, zero arguments
     * (include/msvcrt/stdlib.h:259), called bare at dlls/msvcrt/exit.c:504 when
     * a pure virtual is invoked.  The default four-argument slot is right by
     * the surplus-register rule: there is no real argument to overwrite.
     * ucrtbase exports _o__purecall but no _o__set_purecall_handler, so there
     * is no forwarder row to add -- _purecall is the call site, not a
     * registration, and needs none. */
    { L"msvcr100.dll", "_set_purecall_handler",  1, NULL, 1u << 0 },
    { L"msvcr120.dll", "_set_purecall_handler",  1, NULL, 1u << 0 },
    { L"ucrtbase.dll", "_set_purecall_handler",  1, NULL, 1u << 0 },
    /* __setusermatherr(func): `int (__cdecl *)(struct _exception *)` -- one
     * pointer argument, and the int return that math.c:129 actually tests.  All
     * three CRTs export it and none exports an _o_ forwarder for it. */
    { L"msvcrt.dll",   "__setusermatherr",  1, NULL, 1u << 0 },
    { L"msvcr100.dll", "__setusermatherr",  1, NULL, 1u << 0 },
    { L"msvcr120.dll", "__setusermatherr",  1, NULL, 1u << 0 },
    { L"ucrtbase.dll", "__setusermatherr",  1, NULL, 1u << 0 },
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
    { L"user32.dll", "GetWindowLongPtrA",  2, emu_GetWindowLongPtr },
    { L"user32.dll", "GetWindowLongPtrW",  2, emu_GetWindowLongPtr },
    /* the class-level analogues, added by the pointer-identity audit -- see
     * the banner above emu_GetClassLongPtr */
    { L"user32.dll", "GetClassLongPtrA",   2, emu_GetClassLongPtr },
    { L"user32.dll", "GetClassLongPtrW",   2, emu_GetClassLongPtr },
    { L"user32.dll", "GetClassInfoA",      3, emu_GetClassInfo },
    { L"user32.dll", "GetClassInfoW",      3, emu_GetClassInfo },
    { L"user32.dll", "GetClassInfoExA",    3, emu_GetClassInfoEx },
    { L"user32.dll", "GetClassInfoExW",    3, emu_GetClassInfoEx },
    { L"user32.dll", "CallWindowProcA",    5, emu_CallWindowProc },
    { L"user32.dll", "CallWindowProcW",    5, emu_CallWindowProc },
    /* The same class reached by argument position, so a plain mask serves --
     * with the WIDE bit, because both of these return LRESULT and truncating
     * one is silent.  DOOM (2016) imports SetWindowsHookExA and SetTimer; a
     * hook procedure is called by user32 for every message in the queue, and a
     * TIMERPROC is called from the message loop, so both are native code
     * holding a guest pointer for the life of the window. */
    /* DIALOG PROCEDURES, which are the same class one entry point further on
     * and were reached for the first time by DOOM (2016): user32 keeps a
     * DLGPROC for the life of the dialog and calls it through call_dialog_proc
     * for every message the dialog and its controls receive.  Nothing about
     * that pointer says which machine it belongs to, so an unwrapped guest
     * DLGPROC put the NATIVE ppc64 core on x86-64 bytes -- reported as an
     * access violation INSIDE the game, at an address four bytes into one of
     * its own functions, which reads exactly like the game dereferencing a
     * null pointer and is nothing of the kind.  (report_native_pc_in_guest_image
     * is what says so now.)
     *
     * The proc is the FOURTH argument of all eight entry points, and the two
     * AorW forms differ only by a trailing flag, so the mask is the same for
     * all ten rows.  A DLGPROC returns INT_PTR, not BOOL: DWLP_MSGRESULT is
     * what carries the real answer for most messages, but the return value is
     * read as a full pointer-width value, so the wide bit is set.
     *
     * DELIBERATELY ABSENT, and named so the next reader does not think it was
     * forgotten: SetWindowLongPtr(DWLP_DLGPROC).  DWLP_DLGPROC is not a fixed
     * index -- it is an offset into the window's EXTRA bytes, meaningful only
     * for a window whose class reserved DLGWINDOWEXTRA -- so a row keyed on
     * the index alone would wrap an ordinary application's extra-byte write
     * and corrupt it.  A guest that sets its dialog procedure that way is
     * still owed the wrap; it needs the window's class checked first. */
    { L"user32.dll", "DialogBoxParamA",              5, NULL, 1u << 3, 1u << 3 },
    { L"user32.dll", "DialogBoxParamW",              5, NULL, 1u << 3, 1u << 3 },
    { L"user32.dll", "DialogBoxIndirectParamA",      5, NULL, 1u << 3, 1u << 3 },
    { L"user32.dll", "DialogBoxIndirectParamW",      5, NULL, 1u << 3, 1u << 3 },
    { L"user32.dll", "DialogBoxIndirectParamAorW",   6, NULL, 1u << 3, 1u << 3 },
    { L"user32.dll", "CreateDialogParamA",           5, NULL, 1u << 3, 1u << 3 },
    { L"user32.dll", "CreateDialogParamW",           5, NULL, 1u << 3, 1u << 3 },
    { L"user32.dll", "CreateDialogIndirectParamA",   5, NULL, 1u << 3, 1u << 3 },
    { L"user32.dll", "CreateDialogIndirectParamW",   5, NULL, 1u << 3, 1u << 3 },
    { L"user32.dll", "CreateDialogIndirectParamAorW", 6, NULL, 1u << 3, 1u << 3 },
    /* WINHTTP_STATUS_CALLBACK, the other half of the same DOOM finding, and
     * the worse-behaved half: winhttp calls it from its own request threads,
     * so the branch into guest code happens on a thread the game never made
     * and long after the registration returned success.  Five arguments
     * (HINTERNET, DWORD_PTR context, DWORD status, LPVOID info, DWORD len)
     * and no return value, so cb_argc is 5 and no wide bit. */
    { L"winhttp.dll", "WinHttpSetStatusCallback",    4, NULL, 1u << 1, 0, 5 },
    { L"user32.dll", "SetWindowsHookExA",  4, NULL, 1u << 1, 1u << 1 },
    { L"user32.dll", "SetWindowsHookExW",  4, NULL, 1u << 1, 1u << 1 },
    { L"user32.dll", "SetTimer",           4, NULL, 1u << 3 },
    /* FIBERS.  The whole API, served here rather than by kernelbase: a guest
     * fiber is a guest stack and a saved guest CONTEXT, and kernelbase's --
     * which is a NATIVE stack and a native CONTEXT -- cannot be made to mean
     * that.  See the block above emu_ConvertThreadToFiber for what was tried
     * first and how it fails.  Every entry point is intercepted, including
     * the two that only read state (IsThreadAFiber, ConvertFiberToThread),
     * because a guest that got half its fiber answers from kernelbase and
     * half from here would see two different threads.
     *
     * DOOM (2016) imports CreateFiberEx, SwitchToFiber, ConvertThreadToFiber
     * and DeleteFiber from kernel32; kernelbase carries the same rows because
     * a guest may import either. */
    { L"kernel32.dll",   "ConvertThreadToFiber",   1, emu_ConvertThreadToFiber },
    { L"kernel32.dll",   "ConvertThreadToFiberEx", 2, emu_ConvertThreadToFiber },
    { L"kernel32.dll",   "ConvertFiberToThread",   0, emu_ConvertFiberToThread },
    { L"kernel32.dll",   "CreateFiber",            3, emu_CreateFiber3 },
    { L"kernel32.dll",   "CreateFiberEx",          5, emu_CreateFiberEx },
    { L"kernel32.dll",   "SwitchToFiber",          1, emu_SwitchToFiber },
    { L"kernel32.dll",   "DeleteFiber",            1, emu_DeleteFiber },
    { L"kernel32.dll",   "IsThreadAFiber",         0, emu_IsThreadAFiber },
    { L"kernelbase.dll", "ConvertThreadToFiber",   1, emu_ConvertThreadToFiber },
    { L"kernelbase.dll", "ConvertThreadToFiberEx", 2, emu_ConvertThreadToFiber },
    { L"kernelbase.dll", "ConvertFiberToThread",   0, emu_ConvertFiberToThread },
    { L"kernelbase.dll", "CreateFiber",            3, emu_CreateFiber3 },
    { L"kernelbase.dll", "CreateFiberEx",          5, emu_CreateFiberEx },
    { L"kernelbase.dll", "SwitchToFiber",          1, emu_SwitchToFiber },
    { L"kernelbase.dll", "DeleteFiber",            1, emu_DeleteFiber },
    { L"kernelbase.dll", "IsThreadAFiber",         0, emu_IsThreadAFiber },
    /* THE REST OF THE REGISTRATION SURFACE, found by audit rather than by
     * crashing into it one export at a time.
     *
     * Every row above this block was written after a program died on the
     * export it names.  ppc64le/thunks/callback_audit.py asks the question
     * the other way round: it reads every export of every thunked module
     * through the same clang oracle the thunk generator uses, and reports
     * each one that takes a parameter whose type is a POINTER TO FUNCTION.
     * 9,190 exports, 250 such parameters, 146 rows already covering them --
     * and the rest are these.
     *
     * The two numbers a row cannot be guessed at are measured, not read off
     * the name: how many arguments the callback itself takes (its parameter
     * list, split at depth zero) and whether its return is a full 64 bits
     * (sizeof of its return type, measured by clang for the guest target).
     * That is where LRESULT-returning rows like SetWindowsHookA and
     * SUBCLASSPROC get their wide bit, and where the six-argument
     * SetWindowSubclass gets its arity.
     *
     * DELIBERATELY NOT HERE, each for a reason:
     *
     *   CreateThread, CreateRemoteThread(Ex), NtCreateThreadEx,
     *   RtlCreateUserThread -- a thread's start routine is already handled,
     *   one layer down, by thread_start_is_guest_code() at RtlUserThreadStart,
     *   which also gives the thread's guest stack the size the thread asked
     *   for.  A row here would route it through a trampoline instead and
     *   quietly bypass both.
     *
     *   RtlUserThreadStart itself -- it takes the entry point as an argument
     *   because this port CALLS it; nothing registers anything there.
     *
     *   IsBadCodePtr -- probes a pointer, never calls it.  Wrapping would
     *   have it answer about the trampoline.
     *
     *   Anything whose callback takes more than NINE arguments: the
     *   trampoline pool has fixed-arity dispatchers for four through nine
     *   and refuses anything else by name.  Seven through nine were added
     *   for the rows below -- SetWinEventHook and EventRegister (7),
     *   DdeInitialize and WSAAccept (8), the CopyFileEx /
     *   MoveFileWithProgress family (9) -- which sat in
     *   ppc64le/thunks/callback_holes.txt until the pool could carry them.
     *   Nothing in the audited surface asks for more than nine today; the
     *   day something does, the pool banner's eight/nine stub shape is the
     *   pattern to extend.
     */
    { L"advapi32.dll", "EventRegister",                      4, NULL, 1u << 1,             0,          7 },
    { L"advapi32.dll", "PerfStartProvider",                  3, NULL, 1u << 1,             0,          0 },
    { L"advapi32.dll", "ReadEncryptedFileRaw",               3, NULL, 1u << 0,             0,          0 },
    { L"advapi32.dll", "RegisterServiceCtrlHandlerA",        2, NULL, 1u << 1,             0,          0 },
    { L"advapi32.dll", "RegisterServiceCtrlHandlerExA",      3, NULL, 1u << 1,             0,          0 },
    { L"advapi32.dll", "RegisterServiceCtrlHandlerExW",      3, NULL, 1u << 1,             0,          0 },
    { L"advapi32.dll", "RegisterServiceCtrlHandlerW",        2, NULL, 1u << 1,             0,          0 },
    { L"advapi32.dll", "RegisterTraceGuidsA",                8, NULL, 1u << 0,             0,          0 },
    { L"advapi32.dll", "RegisterTraceGuidsW",                8, NULL, 1u << 0,             0,          0 },
    { L"advapi32.dll", "WriteEncryptedFileRaw",              3, NULL, 1u << 0,             0,          0 },

    { L"cfgmgr32.dll", "CM_Register_Notification",           4, NULL, 1u << 2,             0,          5 },

    { L"comctl32.dll", "DPA_DestroyCallback",                3, NULL, 1u << 1,             0,          0 },
    { L"comctl32.dll", "DPA_EnumCallback",                   3, NULL, 1u << 1,             0,          0 },
    { L"comctl32.dll", "DPA_LoadStream",                     4, NULL, 1u << 1,             0,          0 },
    { L"comctl32.dll", "DPA_Merge",                          6, NULL, 1u << 3 | 1u << 4,   1u << 4,    0 },
    { L"comctl32.dll", "DPA_SaveStream",                     4, NULL, 1u << 1,             0,          0 },
    { L"comctl32.dll", "DPA_Search",                         6, NULL, 1u << 3,             0,          0 },
    { L"comctl32.dll", "DPA_Sort",                           3, NULL, 1u << 1,             0,          0 },
    { L"comctl32.dll", "DSA_DestroyCallback",                3, NULL, 1u << 1,             0,          0 },
    { L"comctl32.dll", "GetWindowSubclass",                  4, NULL, 1u << 1,             1u << 1,    6 },
    { L"comctl32.dll", "RemoveWindowSubclass",               3, NULL, 1u << 1,             1u << 1,    6 },
    { L"comctl32.dll", "SetWindowSubclass",                  4, NULL, 1u << 1,             1u << 1,    6 },

    { L"crypt32.dll", "CertEnumPhysicalStore",              4, NULL, 1u << 3,             0,          6 },
    { L"crypt32.dll", "CertEnumSystemStore",                4, NULL, 1u << 3,             0,          5 },
    { L"crypt32.dll", "CryptEnumOIDInfo",                   4, NULL, 1u << 3,             0,          0 },

    { L"gdi32.dll", "EnumEnhMetaFile",                    5, NULL, 1u << 2,             0,          5 },
    { L"gdi32.dll", "EnumICMProfilesA",                   3, NULL, 1u << 1,             0,          0 },
    { L"gdi32.dll", "EnumICMProfilesW",                   3, NULL, 1u << 1,             0,          0 },
    { L"gdi32.dll", "EnumMetaFile",                       4, NULL, 1u << 2,             0,          5 },
    { L"gdi32.dll", "EnumObjects",                        4, NULL, 1u << 2,             0,          0 },
    { L"gdi32.dll", "LineDDA",                            6, NULL, 1u << 4,             0,          0 },
    { L"gdi32.dll", "SetAbortProc",                       2, NULL, 1u << 1,             0,          0 },

    { L"imm32.dll", "ImmEnumInputContext",                3, NULL, 1u << 1,             0,          0 },
    { L"imm32.dll", "ImmEnumRegisterWordA",               6, NULL, 1u << 1,             0,          0 },
    { L"imm32.dll", "ImmEnumRegisterWordW",               6, NULL, 1u << 1,             0,          0 },

    { L"kernel32.dll", "BindIoCompletionCallback",           3, NULL, 1u << 1,             0,          0 },
    { L"kernel32.dll", "CopyFileExA",                        6, NULL, 1u << 2,             0,          9 },
    { L"kernel32.dll", "CopyFileExW",                        6, NULL, 1u << 2,             0,          9 },
    { L"kernel32.dll", "CreateThreadpoolIo",                 4, NULL, 1u << 1,             0,          6 },
    { L"kernel32.dll", "CreateThreadpoolTimer",              3, NULL, 1u << 0,             0,          0 },
    { L"kernel32.dll", "CreateThreadpoolWait",               3, NULL, 1u << 0,             0,          0 },
    { L"kernel32.dll", "CreateThreadpoolWork",               3, NULL, 1u << 0,             0,          0 },
    { L"kernel32.dll", "CreateTimerQueueTimer",              7, NULL, 1u << 2,             0,          0 },
    { L"kernel32.dll", "EnumCalendarInfoA",                  4, NULL, 1u << 0,             0,          0 },
    { L"kernel32.dll", "EnumCalendarInfoExA",                4, NULL, 1u << 0,             0,          0 },
    { L"kernel32.dll", "EnumCalendarInfoExEx",               6, NULL, 1u << 0,             0,          0 },
    { L"kernel32.dll", "EnumCalendarInfoExW",                4, NULL, 1u << 0,             0,          0 },
    { L"kernel32.dll", "EnumCalendarInfoW",                  4, NULL, 1u << 0,             0,          0 },
    { L"kernel32.dll", "EnumDateFormatsA",                   3, NULL, 1u << 0,             0,          0 },
    { L"kernel32.dll", "EnumDateFormatsExA",                 3, NULL, 1u << 0,             0,          0 },
    { L"kernel32.dll", "EnumDateFormatsExEx",                4, NULL, 1u << 0,             0,          0 },
    { L"kernel32.dll", "EnumDateFormatsExW",                 3, NULL, 1u << 0,             0,          0 },
    { L"kernel32.dll", "EnumDateFormatsW",                   3, NULL, 1u << 0,             0,          0 },
    { L"kernel32.dll", "EnumLanguageGroupLocalesA",          4, NULL, 1u << 0,             0,          0 },
    { L"kernel32.dll", "EnumLanguageGroupLocalesW",          4, NULL, 1u << 0,             0,          0 },
    { L"kernel32.dll", "EnumResourceLanguagesA",             5, NULL, 1u << 3,             0,          5 },
    { L"kernel32.dll", "EnumResourceLanguagesExA",           7, NULL, 1u << 3,             0,          5 },
    { L"kernel32.dll", "EnumResourceLanguagesExW",           7, NULL, 1u << 3,             0,          5 },
    { L"kernel32.dll", "EnumResourceLanguagesW",             5, NULL, 1u << 3,             0,          5 },
    { L"kernel32.dll", "EnumResourceNamesA",                 4, NULL, 1u << 2,             0,          0 },
    { L"kernel32.dll", "EnumResourceNamesExA",               6, NULL, 1u << 2,             0,          0 },
    { L"kernel32.dll", "EnumResourceNamesExW",               6, NULL, 1u << 2,             0,          0 },
    { L"kernel32.dll", "EnumResourceNamesW",                 4, NULL, 1u << 2,             0,          0 },
    { L"kernel32.dll", "EnumResourceTypesA",                 3, NULL, 1u << 1,             0,          0 },
    { L"kernel32.dll", "EnumResourceTypesExA",               5, NULL, 1u << 1,             0,          0 },
    { L"kernel32.dll", "EnumResourceTypesExW",               5, NULL, 1u << 1,             0,          0 },
    { L"kernel32.dll", "EnumResourceTypesW",                 3, NULL, 1u << 1,             0,          0 },
    { L"kernel32.dll", "EnumSystemCodePagesA",               2, NULL, 1u << 0,             0,          0 },
    { L"kernel32.dll", "EnumSystemCodePagesW",               2, NULL, 1u << 0,             0,          0 },
    { L"kernel32.dll", "EnumSystemGeoID",                    3, NULL, 1u << 2,             0,          0 },
    { L"kernel32.dll", "EnumSystemLanguageGroupsA",          3, NULL, 1u << 0,             0,          5 },
    { L"kernel32.dll", "EnumSystemLanguageGroupsW",          3, NULL, 1u << 0,             0,          5 },
    { L"kernel32.dll", "EnumSystemLocalesA",                 2, NULL, 1u << 0,             0,          0 },
    { L"kernel32.dll", "EnumSystemLocalesEx",                4, NULL, 1u << 0,             0,          0 },
    { L"kernel32.dll", "EnumSystemLocalesW",                 2, NULL, 1u << 0,             0,          0 },
    { L"kernel32.dll", "EnumTimeFormatsA",                   3, NULL, 1u << 0,             0,          0 },
    { L"kernel32.dll", "EnumTimeFormatsEx",                  4, NULL, 1u << 0,             0,          0 },
    { L"kernel32.dll", "EnumTimeFormatsW",                   3, NULL, 1u << 0,             0,          0 },
    { L"kernel32.dll", "EnumUILanguagesA",                   3, NULL, 1u << 0,             0,          0 },
    { L"kernel32.dll", "EnumUILanguagesW",                   3, NULL, 1u << 0,             0,          0 },
    { L"kernel32.dll", "InitOnceExecuteOnce",                4, NULL, 1u << 1,             0,          0 },
    { L"kernel32.dll", "MoveFileTransactedA",                6, NULL, 1u << 2,             0,          9 },
    { L"kernel32.dll", "MoveFileTransactedW",                6, NULL, 1u << 2,             0,          9 },
    { L"kernel32.dll", "MoveFileWithProgressA",              5, NULL, 1u << 2,             0,          9 },
    { L"kernel32.dll", "MoveFileWithProgressW",              5, NULL, 1u << 2,             0,          9 },
    { L"kernel32.dll", "QueueUserAPC",                       3, NULL, 1u << 0,             0,          0 },
    { L"kernel32.dll", "QueueUserAPC2",                      4, NULL, 1u << 0,             0,          0 },
    { L"kernel32.dll", "QueueUserWorkItem",                  3, NULL, 1u << 0,             0,          0 },
    { L"kernel32.dll", "ReadDirectoryChangesW",              8, NULL, 1u << 7,             0,          0 },
    { L"kernel32.dll", "ReadFileEx",                         5, NULL, 1u << 4,             0,          0 },
    { L"kernel32.dll", "RegisterApplicationRecoveryCallback",  4, NULL, 1u << 0,             0,          0 },
    { L"kernel32.dll", "RegisterWaitForSingleObject",        6, NULL, 1u << 2,             0,          0 },
    { L"kernel32.dll", "RegisterWaitForSingleObjectEx",      5, NULL, 1u << 1,             0,          0 },
    { L"kernel32.dll", "RtlInstallFunctionTableCallback",    6, NULL, 1u << 3,             1u << 3,    0 },
    { L"kernel32.dll", "SetConsoleCtrlHandler",              2, NULL, 1u << 0,             0,          0 },
    { L"kernel32.dll", "SetWaitableTimer",                   6, NULL, 1u << 3,             0,          0 },
    { L"kernel32.dll", "SetWaitableTimerEx",                 7, NULL, 1u << 3,             0,          0 },
    { L"kernel32.dll", "TrySubmitThreadpoolCallback",        3, NULL, 1u << 0,             0,          0 },
    { L"kernel32.dll", "WriteFileEx",                        5, NULL, 1u << 4,             0,          0 },

    { L"kernelbase.dll", "CopyFileExW",                        6, NULL, 1u << 2,             0,          9 },
    { L"kernelbase.dll", "CreateThreadpoolIo",                 4, NULL, 1u << 1,             0,          6 },
    { L"kernelbase.dll", "CreateThreadpoolTimer",              3, NULL, 1u << 0,             0,          0 },
    { L"kernelbase.dll", "CreateThreadpoolWait",               3, NULL, 1u << 0,             0,          0 },
    { L"kernelbase.dll", "CreateThreadpoolWork",               3, NULL, 1u << 0,             0,          0 },
    { L"kernelbase.dll", "CreateTimerQueueTimer",              7, NULL, 1u << 2,             0,          0 },
    { L"kernelbase.dll", "EnumCalendarInfoExEx",               6, NULL, 1u << 0,             0,          0 },
    { L"kernelbase.dll", "EnumCalendarInfoExW",                4, NULL, 1u << 0,             0,          0 },
    { L"kernelbase.dll", "EnumCalendarInfoW",                  4, NULL, 1u << 0,             0,          0 },
    { L"kernelbase.dll", "EnumDateFormatsExEx",                4, NULL, 1u << 0,             0,          0 },
    { L"kernelbase.dll", "EnumDateFormatsExW",                 3, NULL, 1u << 0,             0,          0 },
    { L"kernelbase.dll", "EnumDateFormatsW",                   3, NULL, 1u << 0,             0,          0 },
    { L"kernelbase.dll", "EnumLanguageGroupLocalesW",          4, NULL, 1u << 0,             0,          0 },
    { L"kernelbase.dll", "EnumResourceLanguagesExA",           7, NULL, 1u << 3,             0,          5 },
    { L"kernelbase.dll", "EnumResourceLanguagesExW",           7, NULL, 1u << 3,             0,          5 },
    { L"kernelbase.dll", "EnumResourceNamesExA",               6, NULL, 1u << 2,             0,          0 },
    { L"kernelbase.dll", "EnumResourceNamesExW",               6, NULL, 1u << 2,             0,          0 },
    { L"kernelbase.dll", "EnumResourceNamesW",                 4, NULL, 1u << 2,             0,          0 },
    { L"kernelbase.dll", "EnumResourceTypesExA",               5, NULL, 1u << 1,             0,          0 },
    { L"kernelbase.dll", "EnumResourceTypesExW",               5, NULL, 1u << 1,             0,          0 },
    { L"kernelbase.dll", "EnumSystemCodePagesW",               2, NULL, 1u << 0,             0,          0 },
    { L"kernelbase.dll", "EnumSystemGeoID",                    3, NULL, 1u << 2,             0,          0 },
    { L"kernelbase.dll", "EnumSystemLanguageGroupsW",          3, NULL, 1u << 0,             0,          5 },
    { L"kernelbase.dll", "EnumSystemLocalesA",                 2, NULL, 1u << 0,             0,          0 },
    { L"kernelbase.dll", "EnumSystemLocalesEx",                4, NULL, 1u << 0,             0,          0 },
    { L"kernelbase.dll", "EnumSystemLocalesW",                 2, NULL, 1u << 0,             0,          0 },
    { L"kernelbase.dll", "EnumTimeFormatsEx",                  4, NULL, 1u << 0,             0,          0 },
    { L"kernelbase.dll", "EnumTimeFormatsW",                   3, NULL, 1u << 0,             0,          0 },
    { L"kernelbase.dll", "EnumUILanguagesW",                   3, NULL, 1u << 0,             0,          0 },
    { L"kernelbase.dll", "EventRegister",                      4, NULL, 1u << 1,             0,          7 },
    { L"kernelbase.dll", "InitOnceExecuteOnce",                4, NULL, 1u << 1,             0,          0 },
    { L"kernelbase.dll", "MoveFileWithProgressW",              5, NULL, 1u << 2,             0,          9 },
    { L"kernelbase.dll", "PerfStartProvider",                  3, NULL, 1u << 1,             0,          0 },
    { L"kernelbase.dll", "QueueUserAPC",                       3, NULL, 1u << 0,             0,          0 },
    { L"kernelbase.dll", "QueueUserAPC2",                      4, NULL, 1u << 0,             0,          0 },
    { L"kernelbase.dll", "QueueUserWorkItem",                  3, NULL, 1u << 0,             0,          0 },
    { L"kernelbase.dll", "ReadDirectoryChangesW",              8, NULL, 1u << 7,             0,          0 },
    { L"kernelbase.dll", "ReadFileEx",                         5, NULL, 1u << 4,             0,          0 },
    { L"kernelbase.dll", "RegisterTraceGuidsW",                8, NULL, 1u << 0,             0,          0 },
    { L"kernelbase.dll", "RegisterWaitForSingleObjectEx",      5, NULL, 1u << 1,             0,          0 },
    { L"kernelbase.dll", "SetConsoleCtrlHandler",              2, NULL, 1u << 0,             0,          0 },
    { L"kernelbase.dll", "SetWaitableTimer",                   6, NULL, 1u << 3,             0,          0 },
    { L"kernelbase.dll", "SetWaitableTimerEx",                 7, NULL, 1u << 3,             0,          0 },
    { L"kernelbase.dll", "TrySubmitThreadpoolCallback",        3, NULL, 1u << 0,             0,          0 },
    { L"kernelbase.dll", "WriteFileEx",                        5, NULL, 1u << 4,             0,          0 },

    { L"msvcr100.dll", "signal",                             2, NULL, 1u << 1,             0,          0 },
    { L"msvcr120.dll", "signal",                             2, NULL, 1u << 1,             0,          0 },

    { L"msvcrt.dll", "signal",                             2, NULL, 1u << 1,             0,          0 },

    { L"ntdll.dll", "LdrRegisterDllNotification",         4, NULL, 1u << 1,             0,          0 },
    { L"ntdll.dll", "NtDeviceIoControlFile",             10, NULL, 1u << 2,             0,          0 },
    { L"ntdll.dll", "NtFsControlFile",                   10, NULL, 1u << 2,             0,          0 },
    { L"ntdll.dll", "NtLockFile",                        10, NULL, 1u << 2,             0,          0 },
    { L"ntdll.dll", "NtNotifyChangeDirectoryFile",        9, NULL, 1u << 2,             0,          0 },
    { L"ntdll.dll", "NtNotifyChangeKey",                 10, NULL, 1u << 2,             0,          0 },
    { L"ntdll.dll", "NtNotifyChangeMultipleKeys",        12, NULL, 1u << 4,             0,          0 },
    { L"ntdll.dll", "NtQueryDirectoryFile",              11, NULL, 1u << 2,             0,          0 },
    { L"ntdll.dll", "NtQueueApcThread",                   5, NULL, 1u << 1,             0,          0 },
    { L"ntdll.dll", "NtQueueApcThreadEx",                 6, NULL, 1u << 2,             0,          0 },
    { L"ntdll.dll", "NtQueueApcThreadEx2",                7, NULL, 1u << 3,             0,          0 },
    { L"ntdll.dll", "NtReadFile",                         9, NULL, 1u << 2,             0,          0 },
    { L"ntdll.dll", "NtReadFileScatter",                  9, NULL, 1u << 2,             0,          0 },
    { L"ntdll.dll", "NtSetTimer",                         7, NULL, 1u << 2,             0,          0 },
    { L"ntdll.dll", "NtWriteFile",                        9, NULL, 1u << 2,             0,          0 },
    { L"ntdll.dll", "NtWriteFileGather",                  9, NULL, 1u << 2,             0,          0 },
    { L"ntdll.dll", "RtlAddVectoredContinueHandler",      2, NULL, 1u << 1,             0,          0 },
    { L"ntdll.dll", "RtlCreateTimer",                     7, NULL, 1u << 2,             0,          0 },
    { L"ntdll.dll", "RtlInstallFunctionTableCallback",    6, NULL, 1u << 3,             1u << 3,    0 },
    { L"ntdll.dll", "RtlQueueWorkItem",                   3, NULL, 1u << 0,             0,          0 },
    { L"ntdll.dll", "RtlRegisterWait",                    6, NULL, 1u << 2,             0,          0 },
    { L"ntdll.dll", "RtlSetIoCompletionCallback",         3, NULL, 1u << 1,             0,          0 },
    { L"ntdll.dll", "RtlSetUnhandledExceptionFilter",     1, NULL, 1u << 0,             0,          0 },
    { L"ntdll.dll", "TpAllocIoCompletion",                5, NULL, 1u << 2,             0,          5 },
    { L"ntdll.dll", "TpAllocTimer",                       4, NULL, 1u << 1,             0,          0 },
    { L"ntdll.dll", "TpAllocWait",                        4, NULL, 1u << 1,             0,          0 },
    { L"ntdll.dll", "TpAllocWork",                        4, NULL, 1u << 1,             0,          0 },
    { L"ntdll.dll", "TpSimpleTryPost",                    3, NULL, 1u << 0,             0,          0 },

    { L"rpcrt4.dll", "NDRSContextMarshall",                3, NULL, 1u << 2,             0,          0 },
    { L"rpcrt4.dll", "NDRSContextMarshall2",               6, NULL, 1u << 3,             0,          0 },
    { L"rpcrt4.dll", "NDRSContextMarshallEx",              4, NULL, 1u << 3,             0,          0 },
    { L"rpcrt4.dll", "NdrServerContextMarshall",           3, NULL, 1u << 2,             0,          0 },
    { L"rpcrt4.dll", "NdrServerContextNewMarshall",        4, NULL, 1u << 2,             0,          0 },
    { L"rpcrt4.dll", "RpcMgmtSetAuthorizationFn",          1, NULL, 1u << 0,             0,          0 },
    { L"rpcrt4.dll", "RpcServerRegisterAuthInfoA",         4, NULL, 1u << 2,             0,          5 },
    { L"rpcrt4.dll", "RpcServerRegisterAuthInfoW",         4, NULL, 1u << 2,             0,          5 },

    { L"setupapi.dll", "SetupCommitFileQueueA",              4, NULL, 1u << 2,             0,          0 },
    { L"setupapi.dll", "SetupCommitFileQueueW",              4, NULL, 1u << 2,             0,          0 },
    { L"setupapi.dll", "SetupDiRegisterDeviceInfo",          6, NULL, 1u << 3,             0,          0 },
    { L"setupapi.dll", "SetupInstallFileA",                  8, NULL, 1u << 6,             0,          0 },
    { L"setupapi.dll", "SetupInstallFileExA",                9, NULL, 1u << 6,             0,          0 },
    { L"setupapi.dll", "SetupInstallFileExW",                9, NULL, 1u << 6,             0,          0 },
    { L"setupapi.dll", "SetupInstallFileW",                  8, NULL, 1u << 6,             0,          0 },
    { L"setupapi.dll", "SetupInstallFromInfSectionA",       11, NULL, 1u << 7,             0,          0 },
    { L"setupapi.dll", "SetupInstallFromInfSectionW",       11, NULL, 1u << 7,             0,          0 },
    { L"setupapi.dll", "SetupIterateCabinetA",               4, NULL, 1u << 2,             0,          0 },
    { L"setupapi.dll", "SetupIterateCabinetW",               4, NULL, 1u << 2,             0,          0 },
    { L"setupapi.dll", "SetupScanFileQueueA",                6, NULL, 1u << 3,             0,          0 },
    { L"setupapi.dll", "SetupScanFileQueueW",                6, NULL, 1u << 3,             0,          0 },

    { L"shell32.dll", "CDefFolderMenu_Create2",             9, NULL, 1u << 5,             0,          6 },
    { L"shell32.dll", "SHAddFromPropSheetExtArray",         3, NULL, 1u << 1,             0,          0 },
    { L"shell32.dll", "SHReplaceFromPropSheetExtArray",     4, NULL, 1u << 2,             0,          0 },

    { L"ucrtbase.dll", "_crt_at_quick_exit",                 1, NULL, 1u << 0,             0,          0 },
    { L"ucrtbase.dll", "_register_onexit_function",          2, NULL, 1u << 1,             0,          0 },
    { L"ucrtbase.dll", "_o__register_onexit_function",       2, NULL, 1u << 1,             0,          0 },
    { L"ucrtbase.dll", "signal",                             2, NULL, 1u << 1,             0,          0 },

    { L"user32.dll", "DdeInitializeA",                     4, NULL, 1u << 1,             1u << 1,    8 },
    { L"user32.dll", "DdeInitializeW",                     4, NULL, 1u << 1,             1u << 1,    8 },
    { L"user32.dll", "DrawStateA",                        10, NULL, 1u << 2,             0,          5 },
    { L"user32.dll", "DrawStateW",                        10, NULL, 1u << 2,             0,          5 },
    { L"user32.dll", "EnumChildWindows",                   3, NULL, 1u << 1,             0,          0 },
    { L"user32.dll", "EnumDesktopWindows",                 3, NULL, 1u << 1,             0,          0 },
    { L"user32.dll", "EnumDesktopsA",                      3, NULL, 1u << 1,             0,          0 },
    { L"user32.dll", "EnumDesktopsW",                      3, NULL, 1u << 1,             0,          0 },
    { L"user32.dll", "EnumPropsA",                         2, NULL, 1u << 1,             0,          0 },
    { L"user32.dll", "EnumPropsExA",                       3, NULL, 1u << 1,             0,          0 },
    { L"user32.dll", "EnumPropsExW",                       3, NULL, 1u << 1,             0,          0 },
    { L"user32.dll", "EnumPropsW",                         2, NULL, 1u << 1,             0,          0 },
    { L"user32.dll", "EnumThreadWindows",                  3, NULL, 1u << 1,             0,          0 },
    { L"user32.dll", "EnumWindowStationsA",                2, NULL, 1u << 0,             0,          0 },
    { L"user32.dll", "EnumWindowStationsW",                2, NULL, 1u << 0,             0,          0 },
    { L"user32.dll", "EnumWindows",                        2, NULL, 1u << 0,             0,          0 },
    { L"user32.dll", "GrayStringA",                        9, NULL, 1u << 2,             0,          0 },
    { L"user32.dll", "GrayStringW",                        9, NULL, 1u << 2,             0,          0 },
    { L"user32.dll", "SendMessageCallbackA",               6, NULL, 1u << 4,             0,          0 },
    { L"user32.dll", "SendMessageCallbackW",               6, NULL, 1u << 4,             0,          0 },
    { L"user32.dll", "SetCoalescableTimer",                5, NULL, 1u << 3,             0,          0 },
    { L"user32.dll", "SetWinEventHook",                    7, NULL, 1u << 3,             0,          7 },
    { L"user32.dll", "SetWindowsHookA",                    2, NULL, 1u << 1,             1u << 1,    0 },
    { L"user32.dll", "SetWindowsHookW",                    2, NULL, 1u << 1,             1u << 1,    0 },
    { L"user32.dll", "UnhookWindowsHook",                  2, NULL, 1u << 1,             1u << 1,    0 },

    { L"vcruntime140.dll", "_set_purecall_handler",              1, NULL, 1u << 0,             0,          0 },

    { L"winmm.dll", "mciSetYieldProc",                    3, NULL, 1u << 1,             0,          0 },
    /* A handler function rather than the plain cb_mask row this used to be:
     * MMIO_FINDPROC and MMIO_REMOVEPROC return whatever is stored for the
     * fourCC, which is our pool stub when a guest installed it -- see
     * emu_mmioInstallIOProc and the pointer-identity audit. */
    { L"winmm.dll", "mmioInstallIOProcA",                 3, emu_mmioInstallIOProc },
    { L"winmm.dll", "mmioInstallIOProcW",                 3, emu_mmioInstallIOProc },

    { L"ws2_32.dll", "GetAddrInfoExW",                    10, NULL, 1u << 8,             0,          0 },
    { L"ws2_32.dll", "WSAAccept",                          5, NULL, 1u << 3,             0,          8 },
    { L"ws2_32.dll", "WSAIoctl",                           9, NULL, 1u << 8,             0,          0 },
    { L"ws2_32.dll", "WSAProviderConfigChange",            3, NULL, 1u << 2,             0,          0 },
    { L"ws2_32.dll", "WSARecv",                            7, NULL, 1u << 6,             0,          0 },
    { L"ws2_32.dll", "WSARecvFrom",                        9, NULL, 1u << 8,             0,          0 },
    { L"ws2_32.dll", "WSASend",                            7, NULL, 1u << 6,             0,          0 },
    { L"ws2_32.dll", "WSASendTo",                          9, NULL, 1u << 8,             0,          0 },
    { L"ws2_32.dll", "WSASetBlockingHook",                 1, NULL, 1u << 0,             1u << 0,    0 },

    { L"wsock32.dll", "WSASetBlockingHook",                 1, NULL, 1u << 0,             1u << 0,    0 },

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

/* Native namesakes are loaded FROM THE SYSTEM DIRECTORY, never searched by
 * bare name: the default search order starts at the application directory,
 * and a game that ships its own guest-machine copy of a thunked module
 * beside the .exe (Dex ships d3d11.dll and dxgi.dll) would have the loader
 * pick the GUEST's image up as the "native" module -- import_dll then walks
 * a wrong-machine image and faults with loader_section held.  Nothing an
 * application ships can be the native half of a thunk module. */
static WCHAR native_system_dir[] = { 'C',':','\\','w','i','n','d','o','w','s','\\',
                                     's','y','s','t','e','m','3','2',0 };

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
            if (LdrGetDllHandle( native_system_dir, 0, &mod->BaseDllName, &native ) &&
                LdrLoadDll( native_system_dir, 0, &mod->BaseDllName, &native ))
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

/* Interface::Method for a COM slot, asked of the module that owns the marshal
 * table.  ntdll has no view of a winecom surface -- the runtime is a static
 * library, one instance per linkee -- so the client exports the two names
 * beside the dispatch entry ntdll already calls.  Caller holds the loader
 * lock; this runs once per slot, when its row is interned. */
static void xstat_name_com_slot( LDR_DATA_TABLE_ENTRY *mod, const struct com_thunk_hit *com,
                                 char *buf, SIZE_T len )
{
    BOOL (WINAPI *slot_name)( UINT, UINT, const char **, const char ** );
    const char *iname = NULL, *sname = NULL;
    ANSI_STRING name;
    HMODULE native;

    RtlInitAnsiString( &name, "__wine_com_slot_name" );
    if (!LdrGetDllHandle( NULL, 0, &mod->BaseDllName, &native ) &&
        !LdrGetProcedureAddress( native, &name, 0, (void **)&slot_name ) &&
        slot_name( com->iface, com->slot, &iname, &sname ) && iname && sname)
    {
        /* A generated slot name already carries the interface that DECLARED
         * the method, which is not always the one the guest called through:
         * IMalloc's Release is declared by IUnknown.  Print the declaring
         * name once, and the calling interface only when it differs. */
        SIZE_T ilen = strlen( iname );
        SIZE_T at = 0;

        if (!strncmp( sname, iname, ilen ) && !strncmp( sname + ilen, "::", 2 ))
            xstat_put( buf, len, 0, sname );
        else
        {
            at = xstat_put( buf, len, 0, iname );
            at = xstat_put( buf, len, at, strstr( sname, "::" ) ? "/" : "::" );
            xstat_put( buf, len, at, sname );
        }
    }
    else
    {
        SIZE_T at = xstat_put_w( buf, len, 0, mod->BaseDllName.Buffer );

        at = xstat_put( buf, len, at, " iface " );
        at = xstat_put_num( buf, len, at, com->iface, 10 );
        at = xstat_put( buf, len, at, " slot " );
        xstat_put_num( buf, len, at, com->slot, 10 );
    }
}

/* The 32-bit lane's spelling of the same lookup: the guest module is named
 * by a UNICODE_STRING32 out of the 32-bit loader list rather than by a
 * 64-bit LDR entry, and the native namesake -- which owns __wine_com_slot_name
 * -- is found by that name.  Same one-shot cost, same fallback naming. */
static void xstat_name_com_slot32( const UNICODE_STRING32 *mod32, const struct com_thunk_hit *com,
                                   char *buf, SIZE_T len )
{
    BOOL (WINAPI *slot_name)( UINT, UINT, const char **, const char ** );
    const char *iname = NULL, *sname = NULL;
    UNICODE_STRING modname;
    ANSI_STRING name;
    HMODULE native;

    modname.Buffer = (WCHAR *)(ULONG_PTR)mod32->Buffer;
    modname.Length = mod32->Length;
    modname.MaximumLength = mod32->MaximumLength;
    RtlInitAnsiString( &name, "__wine_com_slot_name" );
    if (!LdrGetDllHandle( NULL, 0, &modname, &native ) &&
        !LdrGetProcedureAddress( native, &name, 0, (void **)&slot_name ) &&
        slot_name( com->iface, com->slot, &iname, &sname ) && iname && sname)
    {
        SIZE_T ilen = strlen( iname );
        SIZE_T at = 0;

        if (!strncmp( sname, iname, ilen ) && !strncmp( sname + ilen, "::", 2 ))
            xstat_put( buf, len, 0, sname );
        else
        {
            at = xstat_put( buf, len, 0, iname );
            at = xstat_put( buf, len, at, strstr( sname, "::" ) ? "/" : "::" );
            xstat_put( buf, len, at, sname );
        }
    }
    else
    {
        SIZE_T at = xstat_put_w( buf, len, 0, modname.Buffer );

        at = xstat_put( buf, len, at, " iface " );
        at = xstat_put_num( buf, len, at, com->iface, 10 );
        at = xstat_put( buf, len, at, " slot " );
        xstat_put_num( buf, len, at, com->slot, 10 );
    }
}

/* A direct-mapped cache of find_guest_thunk_target's answer, keyed by the
 * trapping RIP, AND READ WITHOUT THE LOADER LOCK.  Both halves of that
 * sentence are load-bearing, and the second one is the point.
 *
 * WHAT THE LOOKUP COSTS.  A flat guest thunk resolves through LdrGetDllHandle
 * (or LdrLoadDll) AND LdrGetProcedureAddress -- two name lookups, each walking
 * a loaded-module hash table -- and a COM slot walks the module list to find
 * the image the RIP lies in.  That happened on EVERY trap, including the two
 * or more GL entry points a single frame's inner loop calls thousands of
 * times.  A guest CALL site's RIP is a fixed address for as long as its
 * module stays mapped there, so the resolution is loop-invariant; only the
 * lookup was not being treated as one.
 *
 * WHAT THE LOCK COSTS, WHICH IS MORE.  find_guest_thunk_target takes the
 * LOADER LOCK, and it has to for the resolving path: that path can call
 * LdrLoadDll, and it reads the module list while another thread may be
 * splicing it.  But the loader lock is PROCESS-WIDE and shared with every
 * LoadLibrary, GetProcAddress and module walk in the process -- and this
 * function is on the path of EVERY SINGLE guest-to-native call.  A game with
 * eight worker threads all calling into Win32 does not have eight threads: it
 * has one, and seven waiting to make a hash-table lookup they have already
 * made.  Caching under the lock shortened the hold; it did not remove the
 * serialization, which is the thing that was actually costing the cores.
 * (Measured shape: DOOM 2016 uses ~2.5 cores natively against 7-9 fully
 * emulated on the same box -- the emulated stack has no such choke point.)
 *
 * SO THE HIT PATH TAKES NO LOCK AT ALL, and the entry is published with a
 * SEQLOCK.  Writers are already serialized against each other by the loader
 * lock they hold for the resolve, so one sequence number per slot is enough:
 * odd means "being written", even means stable, and a reader that sees the
 * same even value before and after copying the payload knows no writer
 * overlapped its read.  The alternative -- comparing only the key before and
 * after -- is not sound, because a slot can be evicted to some other RIP and
 * back again while a reader is inside it, and the reader would then splice
 * two different entries' fields together and match the key at both ends.
 * That is a wrong answer rather than a crash, which is the failure mode this
 * whole port is most careful about.  (The sequence number is 32 bits, so the
 * protocol has a seqlock's usual theoretical hole: 2^32 writes to ONE slot
 * inside ONE reader's ten-field copy.  Writers are serialized by the loader
 * lock and the copy is a dozen loads.)
 *
 * The fences are the explicit compiler builtins rather than MemoryBarrier():
 * this file is ppc64-only, the two orderings needed are store-store on the
 * write side and load-load on the read side, and both are `lwsync` on POWER
 * where MemoryBarrier() is the far heavier full `sync`.  Paying a full
 * barrier per crossing would give back some of what removing the lock buys.
 *
 * ONE slot per hash bucket, like a CPU cache: a collision simply evicts,
 * which only costs a lookup on the next hit for that RIP, never a wrong
 * answer -- the slot carries the exact RIP it was resolved for, so a stale or
 * colliding entry is a MISS, not a wrong hit.
 *
 * COM SLOTS ARE CACHED HERE TOO, and that is a change from the first version
 * of this cache, which skipped them on the grounds that find_guest_com_target
 * has its own per-module dispatch cache.  It does -- but that cache only
 * saves the __wine_com_dispatch lookup, not the module-list walk, and not the
 * loader lock, which was the whole cost.  A COM-heavy guest (Direct3D 12,
 * XAudio2 -- which is to say a game) crosses the boundary through vtable
 * slots far more often than through flat imports.
 *
 * INVALIDATION.  free_modref() calls flush_guest_thunk_cache() when a module
 * is unmapped, so an entry can never outlive the image it names.  The first
 * version relied on nothing in this port unloading and reloading a guest DLL
 * at a reused base address; that was true of the corpus on the day it was
 * written and is not a property anything enforces -- a game that FreeLibrary's
 * a plugin and loads another one is ordinary, and mmap reuses addresses.
 * Flushing costs one pass over 1024 slots on an event that already unmaps a
 * view.
 *
 * WINEEMUNORIPCACHE=1 is the negative control: it forces every lookup to
 * miss, so a bug that only the cached path has (or only the uncached path
 * has) shows up as a behaviour difference with the flag toggled, the same
 * shape as WINEEMUNOCBWRAP/WINEEMUNOGLVEND above. */
#define THUNK_RIP_CACHE_BITS  10
#define THUNK_RIP_CACHE_SIZE  (1u << THUNK_RIP_CACHE_BITS)

struct thunk_rip_cache_entry
{
    LONG seq;                    /* odd while written; see the seqlock note */
    ULONG_PTR rip;               /* 0 = empty; otherwise the exact key */
    void *proc;
    UINT sig;
    thunk_override_func override;
    UINT cb_mask, cb_wide, cb_argc, fp, widths, signs;
    UINT geom32;                 /* the version-8 i386 frame word; 0 on the
                                    64-bit lane and for a refused i386 frame */
    UCHAR lane32;                /* resolved by the i386 dispatcher: com.dispatch
                                    is then a com_dispatch32_func and geom32 is
                                    live.  The two lanes' RIPs cannot collide
                                    (i386 images sit below 4 GiB, the x86-64
                                    thunk base is 0x180000000), so this is a
                                    cross-check, not a namespace. */
    struct com_thunk_hit com;    /* com.dispatch NULL unless this RIP is a slot */
    UINT stat_row;               /* WINE_PPC64LE_TRAP_STATS row, or XSTAT_NO_ROW.
                                  * Here rather than in a table of its own for
                                  * the reason the whole cache exists: resolving
                                  * this site's identity is loop-invariant, and
                                  * the counting path must not resolve anything.
                                  * Cleared with the rest on unmap, so a row can
                                  * outlive the entry but never mis-name it. */
};

static struct thunk_rip_cache_entry thunk_rip_cache[THUNK_RIP_CACHE_SIZE];

/* WINEEMUPROFILE=1: count the crossings per cache slot and print a histogram
 * at process shutdown.
 *
 * "Which native functions is this guest actually calling, and how often" has
 * no answer from outside the process, and it is the first question worth
 * asking about any boundary cost -- a profiler samples the CALLEE and tells
 * you where the time went inside it, never how many times it was crossed to
 * get there.  Measured on DOOM (2016): its worker pool wakes ~54,000 times a
 * second while using 2.4 cores, and the shape of that number is a property of
 * which APIs it crosses for, not of how long any of them takes.
 *
 * OFF BY DEFAULT AND A SEPARATE ARRAY, both deliberately.  The counter is a
 * shared cache line written by every thread that crosses; keeping it out of
 * the cache entry keeps the entry immutable, which is what the seqlock above
 * relies on, and keeping the increment behind a flag keeps the measurement
 * from being the thing it measures.  With the flag off this is one
 * already-loaded branch on a path that just did a dozen loads. */
static LONG thunk_rip_hits[THUNK_RIP_CACHE_SIZE];
static LONG thunk_rip_total;
static int thunk_rip_profile = -1;

/* Crossings between periodic histogram prints.  Two million is about a second
 * of a loading game on this port, which is often enough to see a phase change
 * and rare enough that the printing is not itself the measurement. */
#define THUNK_PROFILE_INTERVAL 2000000

static UINT thunk_rip_cache_slot( ULONG_PTR rip )
{
    /* trap sites are THUNK stride apart (a handful of bytes), not naturally
     * spread over the low bits a plain modulo would key on, so mix them the
     * way any address-keyed direct-mapped cache does. */
    ULONG_PTR h = rip >> 3;
    h ^= h >> 17;
    h ^= h >> 31;
    return (UINT)(h & (THUNK_RIP_CACHE_SIZE - 1));
}

void dump_guest_thunk_profile(void);

/* Sabotage lever, read once.  Both halves of the cache have to honour it or
 * it proves nothing: a reader forced to slot zero while the writer still
 * files by hash would simply find that slot empty and miss forever, which
 * looks exactly like a gate passing. */
static int thunk_rip_cache_blind(void)
{
    static int blind = -1;

    if (blind == -1)
    {
        blind = emu_env_flag( L"WINEEMURIPCACHEBLIND" );
        if (blind)
            ERR( "WINEEMURIPCACHEBLIND: the thunk cache will answer from one "
                 "slot without comparing the trapping address; every crossing "
                 "past the first is liable to reach the wrong function\n" );
    }
    return blind;
}

/* -> TRUE with *out filled when this RIP is cached.  No lock: see above.
 *
 * WINEEMURIPCACHEBLIND=1 is the second negative control, and it is the one
 * that has something to prove.  WINEEMUNORIPCACHE only turns the cache OFF,
 * which can only ever make the port slower, never wrong -- a gate built on it
 * alone would go red for a mechanism that was never at risk.  BLIND instead
 * keeps the cache and removes the two things that make it SAFE: every RIP is
 * forced into slot zero and the key is not compared.  The second distinct
 * call site then gets the first one's function, which is a wrong answer of
 * exactly the class this cache could produce if the key check were ever
 * dropped, and it is deterministic rather than a race a gate would have to
 * get lucky to catch. */
static BOOL thunk_rip_cache_get( ULONG_PTR rip, struct thunk_rip_cache_entry *out )
{
    const struct thunk_rip_cache_entry volatile *e;
    int blind = thunk_rip_cache_blind();
    LONG s1, s2;

    e = &thunk_rip_cache[blind ? 0 : thunk_rip_cache_slot( rip )];

    s1 = ReadAcquire( (LONG const volatile *)&e->seq );
    if (s1 & 1) return FALSE;                 /* a writer is inside this slot */

    /* Field by field through a volatile pointer so the compiler really loads
     * each one between the two sequence reads rather than sinking a struct
     * copy past the fence below. */
    out->rip          = e->rip;
    out->proc         = e->proc;
    out->sig          = e->sig;
    out->override     = e->override;
    out->cb_mask      = e->cb_mask;
    out->cb_wide      = e->cb_wide;
    out->cb_argc      = e->cb_argc;
    out->fp           = e->fp;
    out->widths       = e->widths;
    out->signs        = e->signs;
    out->geom32       = e->geom32;
    out->lane32       = e->lane32;
    out->com.dispatch = e->com.dispatch;
    out->com.iface    = e->com.iface;
    out->com.slot     = e->com.slot;
    out->stat_row     = e->stat_row;

    __atomic_thread_fence( __ATOMIC_ACQUIRE );   /* payload loads, THEN the recheck */
    s2 = ReadNoFence( (LONG const volatile *)&e->seq );
    if (s1 != s2) return FALSE;
    if (!blind && out->rip != rip) return FALSE;
    if (!out->rip) return FALSE;                /* an empty slot answers nothing */

    /* Traced because a cache is otherwise invisible: "did the fast path run"
     * has no other answer from outside the process, and ppc64le/thunks/
     * check-rip-cache.sh asserts an exact count of these against a probe that
     * controls exactly how many crossings it makes. */
    TRACE( "thunk cache hit for %p -> proc %p com %p\n",
           (void *)rip, out->proc, out->com.dispatch );
    return TRUE;
}

/***********************************************************************
 *           dump_guest_thunk_profile
 *
 * Print the crossing histogram WINEEMUPROFILE=1 has been collecting, busiest
 * first, as module+offset rather than a bare address so `nm` can finish the
 * job.  Called from LdrShutdownProcess(); a process that dies without
 * shutting down cleanly simply prints nothing, which is honest.
 */
void dump_guest_thunk_profile(void)
{
    static int profile_all = -1;
    UINT order[THUNK_RIP_CACHE_SIZE];
    UINT i, j, n = 0;
    ULONG total = 0;

    if (thunk_rip_profile != 1) return;
    ERR( "WINEEMUPROFILE: --- histogram at %u crossings ---\n", (UINT)thunk_rip_total );

    for (i = 0; i < THUNK_RIP_CACHE_SIZE; i++)
    {
        if (!thunk_rip_hits[i]) continue;
        order[n++] = i;
        total += (ULONG)thunk_rip_hits[i];
    }
    /* insertion sort: n is at most 1024 and this runs once, at shutdown */
    for (i = 1; i < n; i++)
    {
        UINT key = order[i];
        for (j = i; j && thunk_rip_hits[order[j - 1]] < thunk_rip_hits[key]; j--)
            order[j] = order[j - 1];
        order[j] = key;
    }

    ERR( "WINEEMUPROFILE: %u guest->native crossings served from %u cache slots\n",
         (UINT)total, n );
    /* Forty rows is the right default: it is the working set, it fits on a
     * screen, and it answers "what is this program spending its crossings
     * on".  It is the wrong list for "what could have corrupted memory",
     * because the call that writes past the end of a buffer is as likely to
     * be made once as a million times -- and everything called once sorts to
     * the bottom, below the cut, invisible.  WINEEMUPROFILEALL=1 prints every
     * live slot so the tail can be read too. */
    if (profile_all == -1) profile_all = emu_env_flag( L"WINEEMUPROFILEALL" );
    for (i = 0; i < n && (profile_all || i < 40); i++)
    {
        struct thunk_rip_cache_entry volatile *e = &thunk_rip_cache[order[i]];
        void *proc = e->proc;
        void *image = NULL;
        const WCHAR *modname = NULL;
        ULONG_PTR off = 0;

        if (proc && RtlPcToFileHeader( proc, &image ) && image)
        {
            LDR_DATA_TABLE_ENTRY *mod;
            if (!LdrFindEntryForAddress( proc, &mod ))
            {
                modname = mod->BaseDllName.Buffer;
                off = (ULONG_PTR)proc - (ULONG_PTR)mod->DllBase;
            }
        }
        if (modname)
            ERR( "WINEEMUPROFILE: %10u  rip %p -> %s+%#I64x\n",
                 (UINT)thunk_rip_hits[order[i]], (void *)e->rip,
                 debugstr_w(modname), (ULONG64)off );
        else if (e->com.dispatch)
            ERR( "WINEEMUPROFILE: %10u  rip %p -> COM iface %u slot %u\n",
                 (UINT)thunk_rip_hits[order[i]], (void *)e->rip,
                 e->com.iface, e->com.slot );
        else
            ERR( "WINEEMUPROFILE: %10u  rip %p -> proc %p\n",
                 (UINT)thunk_rip_hits[order[i]], (void *)e->rip, proc );
    }
}

/* Publish an answer.  CALLER HOLDS THE LOADER LOCK, which is what serializes
 * writers against each other and is why one sequence number is enough. */
static void thunk_rip_cache_put( ULONG_PTR rip, const struct thunk_rip_cache_entry *val )
{
    struct thunk_rip_cache_entry volatile *e =
        &thunk_rip_cache[thunk_rip_cache_blind() ? 0 : thunk_rip_cache_slot( rip )];
    LONG s = e->seq;

    /* An ODD sequence here means a writer is already inside this slot, and
     * since the loader lock serializes writers ACROSS threads the only way to
     * see it is a write nested inside another write ON THIS ONE -- a resolve
     * that re-entered find_guest_thunk_target while filling a slot.  Nothing
     * in the port does that today; the point is that if anything ever does,
     * the inner write would restart the sequence from an odd value and leave
     * the slot looking stable while it was not, which is a wrong answer.
     * Skipping the fill instead costs one lookup next time. */
    if (s & 1) return;

    WriteRelease( (LONG volatile *)&e->seq, s + 1 );   /* odd: writing */
    __atomic_thread_fence( __ATOMIC_RELEASE );         /* ...before any payload store */

    e->rip          = rip;
    e->proc         = val->proc;
    e->sig          = val->sig;
    e->override     = val->override;
    e->cb_mask      = val->cb_mask;
    e->cb_wide      = val->cb_wide;
    e->cb_argc      = val->cb_argc;
    e->fp           = val->fp;
    e->widths       = val->widths;
    e->signs        = val->signs;
    e->geom32       = val->geom32;
    e->lane32       = val->lane32;
    e->com.dispatch = val->com.dispatch;
    e->com.iface    = val->com.iface;
    e->com.slot     = val->com.slot;
    e->stat_row     = val->stat_row;

    WriteRelease( (LONG volatile *)&e->seq, s + 2 );   /* even: stable again */
}

/***********************************************************************
 *           flush_guest_thunk_cache
 *
 * Every cached answer names an address inside some mapped image, so every one
 * of them stops being an answer the moment a module is unmapped.  Called from
 * free_modref() in dlls/ntdll/loader.c, which holds the loader section -- the
 * same lock thunk_rip_cache_put() writes under, so this cannot race a writer
 * and only has to be safe against concurrent READERS, which the sequence
 * protocol already makes it.
 */
void flush_guest_thunk_cache(void)
{
    UINT i;

    for (i = 0; i < THUNK_RIP_CACHE_SIZE; i++)
    {
        struct thunk_rip_cache_entry volatile *e = &thunk_rip_cache[i];
        LONG s;

        if (!e->rip) continue;
        s = e->seq;
        WriteRelease( (LONG volatile *)&e->seq, s + 1 );
        __atomic_thread_fence( __ATOMIC_RELEASE );
        e->rip          = 0;
        e->proc         = NULL;
        e->override     = NULL;
        e->com.dispatch = NULL;
        WriteRelease( (LONG volatile *)&e->seq, s + 2 );
    }
}

/***********************************************************************
 *           qpc_arm_module
 *
 * Fill in and switch on a guest module's QPC fast-path block, once.
 *
 * A fast-path stub is linked DISARMED, so the first QueryPerformanceCounter a
 * guest makes goes down the stub's slow leg: it stores its own RDTSC reading
 * into the block and falls through to the trap it always took.  That reading
 * is the whole point.  It is a guest-visible counter value a few microseconds
 * old, and the guest counter is the host timebase shifted left by the
 * emulator's TSC scale with the same zero ([MEASURED] -- see
 * include/wine/emu_qpc.h), so one native mftb() here NAMES that scale by
 * matching `sample >> s` against it.  No question is asked of the emulator, no
 * guest code is run from the host, and a counter that does not look like the
 * timebase at any scale leaves the block off, which costs the trap and nothing
 * else.
 *
 * Called from the module walk below, which runs once per new call site under
 * the loader lock.  The first QPC call IS such a site, so the block is armed
 * by the time the second one runs; nothing later depends on another miss
 * happening.
 */
#define QPC_PROBED_MAX 32
static const void *qpc_probed[QPC_PROBED_MAX];
static UINT qpc_probed_count;

static void qpc_arm_module( LDR_DATA_TABLE_ENTRY *mod )
{
    const struct emu_qpc_session *sess;
    struct emu_qpc_guest *g;
    ANSI_STRING name;
    ULONG64 tb, sample;
    UINT s, i;

    for (i = 0; i < qpc_probed_count; i++)
        if (qpc_probed[i] == mod->DllBase) return;

    RtlInitAnsiString( &name, "__wine_thunk_qpc" );
    if (LdrGetProcedureAddress( mod->DllBase, &name, 0, (void **)&g )) goto done;
    if (g->magic != EMU_QPC_MAGIC)
    {
        ERR( "%s exports __wine_thunk_qpc with magic %I64x, not %I64x\n",
             debugstr_w(mod->BaseDllName.Buffer), g->magic, (ULONG64)EMU_QPC_MAGIC );
        goto done;
    }
    /* The critical-section levers ride in this block but are NOT part of the
     * clock's arming dance: the CS fast bodies are live at byte value 0 (they
     * have no host-seeded parameter -- see wine/emu_qpc.h), so all the host
     * ever writes here is a deliberate 1.  Written before every early return
     * below so the levers work even when the clock never arms; idempotent, so
     * running again for an already-probed module costs two compares. */
    if (emu_env_flag( L"WINE_PPC64LE_NO_CS_BYPASS" ) && !g->cs_disable)
    {
        ERR( "guest critical-section fast path disabled by WINE_PPC64LE_NO_CS_BYPASS\n" );
        g->cs_disable = 1;
    }
    if (emu_env_flag( L"WINE_PPC64LE_CS_SABOTAGE_OWNER" ) && !g->cs_sabotage)
    {
        ERR( "SABOTAGE: the guest's fast EnterCriticalSection will not record "
             "an owner; recursive and native enters on the owning thread will "
             "deadlock, which is what the gate's negative control requires\n" );
        g->cs_sabotage = 1;
    }

    if (g->enabled) goto done;
    /* No stub has run yet.  NOT recorded as probed: the sample appears on the
     * first fast-path call, and that call's own miss is what arms this. */
    if (!(sample = g->tsc_sample)) return;

    if (emu_env_flag( L"WINE_PPC64LE_NO_QPC_BYPASS" ))
    {
        ERR( "guest QPC fast path disabled by WINE_PPC64LE_NO_QPC_BYPASS\n" );
        goto done;
    }

    sess = (const struct emu_qpc_session *)((const char *)user_shared_data + EMU_QPC_SESSION_OFFSET);
    if (!emu_qpc_session_ok( sess ))
    {
        ERR( "the session's QPC parameters were never seeded; fast path stays off\n" );
        goto done;
    }

    /* Two seconds either way.  Adjacent candidate scales are a factor of two
     * apart -- half the machine's uptime in timebase ticks, hundreds of
     * billions -- so this window cannot pick the wrong one; it is wide only so
     * that a first trap which had to LdrLoadDll a native counterpart still
     * matches. */
    tb = emu_qpc_timebase();
    for (s = 0; s <= EMU_QPC_MAX_SHIFT; s++)
    {
        ULONG64 guess = sample >> s;
        ULONG64 d = guess > tb ? guess - tb : tb - guess;
        if (d < sess->tb_freq * 2) break;
    }
    if (s > EMU_QPC_MAX_SHIFT)
    {
        ERR( "guest RDTSC %I64u is not the timebase (%I64u) at any scale; "
             "QPC fast path stays off\n", sample, tb );
        goto done;
    }

    g->multiplier = sess->multiplier;
    g->bias       = sess->bias;
    g->frequency  = sess->qpc_freq;
    g->shift      = (UCHAR)s;

    /* The negative controls the gate drives.  Each breaks the SEEDING, which
     * is the part a wrong answer would come from, and leaves the mechanism
     * itself intact -- so ppc64le/cpu/check-qpc-fastpath.sh is falsifying the
     * thing it claims to check and not re-stating it. */
    if (emu_env_flag( L"WINE_PPC64LE_QPC_SABOTAGE_SHIFT" ))
    {
        ERR( "SABOTAGE: seeding the guest QPC shift as %u instead of %u\n", s + 1, s );
        g->shift = (UCHAR)(s + 1);
    }
    if (emu_env_flag( L"WINE_PPC64LE_QPC_SABOTAGE_BIAS" ))
    {
        ERR( "SABOTAGE: seeding the guest QPC bias 100 ms off\n" );
        g->bias = sess->bias + 1000000;
    }

    TRACE( "%s: guest QPC armed, tsc scale %u, multiplier %I64u, bias %I64d\n",
           debugstr_w(mod->BaseDllName.Buffer), s, g->multiplier, g->bias );
    __atomic_store_n( &g->enabled, 1, __ATOMIC_RELEASE );

done:
    if (qpc_probed_count < QPC_PROBED_MAX) qpc_probed[qpc_probed_count++] = mod->DllBase;
}


/***********************************************************************
 *           EC row cells (NEXT.md item 7, "row cookies")
 *
 * A transitioned call's identity is loop-invariant twice over: the RIP is
 * fixed for the mapping's life AND the bridge already delivers a per-rip
 * cookie to the EC handler.  So give every registered stub its own private
 * dispatch cell, resolved lazily on first use and immutable after -- the
 * cookie IS the cell, and a warm transition reads its row out of one line
 * with no hash, no key compare, no seqlock and no shared slot to be evicted
 * from.  [MEASURED, before this existed]: thunk_rip_cache_get was 1.61% of
 * Witcher 3's D3D11-submission thread with every crossing already
 * transitioning (/tmp/w3-ec.perf, 2026-08-31).
 *
 * The cell caches struct thunk_rip_cache_entry -- the exact payload the
 * shared cache publishes, stat_row included -- and it is filled FROM that
 * cache right after the ordinary resolve, never by a parallel resolution
 * path.  Publication: fields first, then a release-store of state; readers
 * acquire-load state and read in place (a resolved cell never changes, so
 * no copy and no seqlock are needed; concurrent first-users both resolve
 * under the loader lock and write identical bytes).
 *
 * THE THREE CACHE LEVERS WIN over the cells, or the levers prove nothing:
 * WINEEMUNORIPCACHE (resolve every call), WINEEMUPROFILE (count every
 * crossing in find), and WINEEMURIPCACHEBLIND (the sabotage that must
 * corrupt dispatch) each mark cells SLOW so every crossing keeps going
 * through find_guest_thunk_target, where each lever already does its job. */
struct ec_row_cell
{
    LONG state;                      /* EC_CELL_*; release-published */
    struct thunk_rip_cache_entry e;  /* valid only once RESOLVED */
};
enum { EC_CELL_UNRESOLVED = 0, EC_CELL_RESOLVED = 1, EC_CELL_SLOW = 2 };

static void ec_cell_fill( struct ec_row_cell *cell, ULONG_PTR rip )
{
    static int cells_off = -1;
    struct thunk_rip_cache_entry tmp;

    if (ReadNoFence( &cell->state ) != EC_CELL_UNRESOLVED) return;
    if (cells_off == -1)
        cells_off = emu_env_flag( L"WINEEMUNORIPCACHE" ) || emu_env_flag( L"WINEEMUPROFILE" );
    if (cells_off || thunk_rip_cache_blind())
    {
        WriteRelease( &cell->state, EC_CELL_SLOW );
        return;
    }
    /* the resolve that just ran filed this RIP in the shared cache; read the
     * finished entry back rather than re-deriving any of it.  A miss here is
     * an eviction race (another RIP hashed onto the slot between the resolve
     * and this read) -- stay UNRESOLVED and let a later call retry. */
    if (!thunk_rip_cache_get( rip, &tmp ) || tmp.rip != rip) return;
    cell->e = tmp;
    WriteRelease( &cell->state, EC_CELL_RESOLVED );
}


/***********************************************************************
 *           ec_arm_module
 *
 * Ask the unix side to register this thunk module's stub arrays as bridge
 * ABI 7 EC targets (ppc64le/docs/ppc64ec.md step B): the flat array from
 * __wine_thunk_info, plus every COM interface's vtable stub array from
 * __wine_com_thunk_info when the module has one.  Same seam and same
 * cadence as qpc_arm_module above -- the FIRST trap into a module arms the
 * whole module, under the loader lock, once -- so no eager module walk and
 * no second parser exist anywhere.  The unix side byte-verifies every slot
 * and silently does nothing when EC is not armed (no ABI 7 bridge, view
 * off, WINE_PPC64LE_NO_EC=1), so this costs one unixcall per module in the
 * worst case and nothing per trap ever.
 *
 * The probed[] cadence has one accepted consequence, stated where it is
 * decided: if a module is UNMAPPED (the unix side drops its registrations
 * at the unmap seam) and the same module is mapped again AT THE SAME BASE,
 * this list still names it and the module keeps trapping -- correct, just
 * slower.  A different base arms fresh. */
#define EC_PROBED_MAX 64
static const void *ec_probed[EC_PROBED_MAX];
static UINT ec_probed_count;

/* Whether EC registration is live in this process, asked ONCE with a
 * zero-count probe call: cells are real memory (one per stub slot for the
 * module's life), and allocating them in a process where EC never arms --
 * ABI < 7 bridge, view off, WINE_PPC64LE_NO_EC=1 -- would spend megabytes
 * feeding a path that never runs. */
static int ec_active = -1;

static struct ec_row_cell *ec_alloc_cells( UINT count )
{
    /* Module-lifetime, and LEAKED on the rare guest module unload, the same
     * deliberate bounded leak as the bridge's retired descriptors: an
     * in-flight transition may hold the cookie while the unmap runs, and a
     * refcount on the hot path would cost more than the leak.  Zeroed =
     * every cell starts EC_CELL_UNRESOLVED. */
    if (ec_active != 1 || !count) return NULL;
    return RtlAllocateHeap( GetProcessHeap(), HEAP_ZERO_MEMORY, count * sizeof(struct ec_row_cell) );
}

static void ec_arm_module( LDR_DATA_TABLE_ENTRY *mod, ULONG_PTR base, const struct thunk_info *info )
{
    struct emu_register_ec_params params;
    const struct com_thunk_info *cinfo;
    UINT i, registered = 0, skipped = 0;
    ANSI_STRING name;

    for (i = 0; i < ec_probed_count; i++)
        if (ec_probed[i] == mod->DllBase) return;
    if (ec_probed_count >= EC_PROBED_MAX)
    {
        ERR( "more than %u thunk modules; %s keeps trapping\n",
             EC_PROBED_MAX, debugstr_w(mod->BaseDllName.Buffer) );
        return;
    }
    ec_probed[ec_probed_count++] = mod->DllBase;

    if (ec_active == -1)
    {
        memset( &params, 0, sizeof(params) );
        WINE_UNIX_CALL( unix_emu_register_ec, &params );
        ec_active = params.armed ? 1 : 0;
    }
    if (!ec_active) return;

    params.first_stub = base + info->stubs_rva;
    params.count      = info->count;
    params.stride     = info->stride;
    params.cells      = (ULONG_PTR)ec_alloc_cells( info->count );
    params.cell_size  = sizeof(struct ec_row_cell);
    WINE_UNIX_CALL( unix_emu_register_ec, &params );
    registered += params.registered;
    skipped    += params.skipped;

    RtlInitAnsiString( &name, "__wine_com_thunk_info" );
    if (!LdrGetProcedureAddress( mod->DllBase, &name, 0, (void **)&cinfo ) &&
        cinfo->version == COM_THUNK_INFO_VERSION && cinfo->stride)
    {
        const struct com_thunk_iface *ifaces = (const struct com_thunk_iface *)(base + cinfo->ifaces_rva);
        for (i = 0; i < cinfo->iface_count; i++)
        {
            params.first_stub = base + ifaces[i].stubs_rva;
            params.count      = ifaces[i].slot_count;
            params.stride     = cinfo->stride;
            params.cells      = (ULONG_PTR)ec_alloc_cells( ifaces[i].slot_count );
            params.cell_size  = sizeof(struct ec_row_cell);
            WINE_UNIX_CALL( unix_emu_register_ec, &params );
            registered += params.registered;
            skipped    += params.skipped;
        }
    }

    if (registered || skipped)
        TRACE( "%s: %u stubs registered as ec targets, %u skipped\n",
               debugstr_w(mod->BaseDllName.Buffer), registered, skipped );
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
                                      UINT *cb_argc, UINT *fp, UINT *widths,
                                      UINT *signs )
{
    static int no_cache = -1;
    struct thunk_rip_cache_entry hit;
    LIST_ENTRY *mark, *entry;
    UINT stat_row = XSTAT_NO_ROW;
    void *ret = NULL;
    ULONG_PTR magic;

    if (no_cache == -1) no_cache = emu_env_flag( L"WINEEMUNORIPCACHE" );
    if (thunk_rip_profile == -1) thunk_rip_profile = emu_env_flag( L"WINEEMUPROFILE" );
    if (!xstat_probed) xstat_probe();

    /* Counted HERE rather than on the hit path, because a crossing that
     * misses is still a crossing -- and a call site that is only ever crossed
     * once is exactly the kind the histogram should show as rare rather than
     * not show at all. */
    if (thunk_rip_profile)
    {
        LONG n;

        InterlockedIncrement( &thunk_rip_hits[thunk_rip_cache_slot( rip )] );
        /* Printed every so often as well as at shutdown, because the runs
         * worth profiling are exactly the ones that do not shut down: a game
         * that dies on its own error path, or one a timeout kills, reaches no
         * LdrShutdownProcess and would otherwise measure nothing at all. */
        n = InterlockedIncrement( &thunk_rip_total );
        if (n % THUNK_PROFILE_INTERVAL == 0) dump_guest_thunk_profile();
    }

    *fp = 0;
    *widths = 0;
    *signs  = 0;

    /* BEFORE THE LOCK, deliberately.  This is the whole reason the cache
     * exists: a warm trap site answers out of one cache line and never enters
     * the process-wide loader lock at all, so N guest threads crossing the
     * boundary are N threads crossing it rather than one at a time.  See the
     * seqlock note above thunk_rip_cache_get for why this is safe without a
     * lock and why comparing the key alone would not have been. */
    if (!no_cache && thunk_rip_cache_get( rip, &hit ))
    {
        *sig_out  = hit.sig;
        *override = hit.override;
        *cb_mask  = hit.cb_mask;
        *cb_wide  = hit.cb_wide;
        *cb_argc  = hit.cb_argc;
        *fp       = hit.fp;
        *widths   = hit.widths;
        *signs    = hit.signs;
        /* a COM entry carries a dispatch and a NULL proc; a flat one the
         * reverse, and must leave the caller's zeroed *com alone */
        if (com && hit.com.dispatch) *com = hit.com;
        /* THE COUNTING PATH: one relaxed add against an index this site
         * resolved once.  Everything that made the index -- the module walk,
         * the name lookups, the string copy -- happened on the miss path
         * below, and happens once per site for the life of the mapping. */
        if (xstat)
        {
            xstat_hit( hit.stat_row );
            xstat_tick_check();
        }
        return hit.proc;
    }

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

        /* Once per module, and only from a module that really is a thunk
         * module: see qpc_arm_module above. */
        qpc_arm_module( mod );
        ec_arm_module( mod, base, info );

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
        *widths = ((const UINT *)(base + info->widths_rva))[idx];
        *signs  = ((const UINT *)(base + info->signs_rva))[idx];
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

        /* The native namespace: same base name, resolved for our own machine
         * FROM THE SYSTEM DIRECTORY (see native_system_dir above -- an
         * application's own copy of a thunked module must never be loaded as
         * the native half).  Nothing native necessarily references it -- a
         * guest process may be the only reason the module is wanted at all --
         * so load it if it is not already present rather than treating that
         * as a failure. */
        if (LdrGetDllHandle( native_system_dir, 0, &mod->BaseDllName, &native ) &&
            LdrLoadDll( native_system_dir, 0, &mod->BaseDllName, &native ))
        {
            WARN( "no native %s; only an override can serve this\n",
                  debugstr_w(mod->BaseDllName.Buffer) );
            native = NULL;
        }
        RtlInitAnsiString( &func_name, (char *)(base + impl_names[idx]) );

        /* A GUEST-REFUSE export resolves to the ONE shared refusal stub, which
         * takes no arguments and therefore cannot name itself -- all
         * __wine_com_refuse can say is "see the guest thunk trace for which",
         * and the trace that would answer it is a +seh run measured in
         * gigabytes.  The name is known right here, so say it right here.
         * This is the discipline the loader already applies to a missing
         * import, where a per-symbol 0xdead0000+n sentinel makes the faulting
         * address name its symbol; a refusal deserves the same.
         *
         * The second half of the message is the part that costs sessions: the
         * stub answers E_NOTIMPL and writes NOTHING to an out-pointer, so a
         * caller that does not check the HRESULT reads whatever was on its
         * stack.  [MEASURED 2026-08-22] DOOM does exactly that and ends up
         * calling RtlTryEnterCriticalSection(NULL), which faults inside the
         * lock and leaves two threads deadlocked against each other -- forty
         * minutes and three symptoms away from this line. */
        if (!strcmp( func_name.Buffer, "__wine_com_refuse" ))
            ERR( "%s.%s is refused (GUEST-REFUSE): answers E_NOTIMPL and writes "
                 "no out-pointer, so an unchecked caller uses uninitialised "
                 "memory\n",
                 debugstr_w(mod->BaseDllName.Buffer),
                 (const char *)(base + names[idx]) );

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
            TRACE( "%s.%s -> override %p cb_mask %#x cb_wide %#x cb_argc %u (%u args)\n",
                   debugstr_w(mod->BaseDllName.Buffer), thunk_overrides[i].name,
                   thunk_overrides[i].func, thunk_overrides[i].cb_mask,
                   thunk_overrides[i].cb_wide,
                   thunk_overrides[i].cb_argc ? thunk_overrides[i].cb_argc : 4,
                   thunk_overrides[i].argc );
            *override = thunk_overrides[i].func;
            *cb_mask  = thunk_overrides[i].cb_mask;
            *cb_wide  = thunk_overrides[i].cb_wide;
            *cb_argc  = thunk_overrides[i].cb_argc;
            break;
        }

        ret = proc;
        if (xstat)
        {
            char name[EMU_XSTAT_NAME];

            xstat_name_export( name, sizeof(name), mod->BaseDllName.Buffer,
                               (const char *)(base + names[idx]) );
            stat_row = xstat_intern( EMU_XSTAT_FLAT, rip, name );
            xstat_hit( stat_row );
        }
        if (!no_cache)
        {
            struct thunk_rip_cache_entry val = { 0 };

            val.stat_row = stat_row;
            val.proc     = proc;
            val.sig      = *sig_out;
            val.override = *override;
            val.cb_mask  = *cb_mask;
            val.cb_wide  = *cb_wide;
            val.cb_argc  = *cb_argc;
            val.fp       = *fp;
            val.widths   = *widths;
            val.signs    = *signs;
            thunk_rip_cache_put( rip, &val );
        }
        goto done;

    try_com:
        /* Cached on the same terms as a flat answer, and for a stronger
         * reason: resolving a COM slot walked the whole module list to get
         * here.  find_guest_com_target's per-module dispatch cache saves the
         * __wine_com_dispatch lookup and nothing else -- not the walk, and
         * not the loader lock, which is what the crossing was actually
         * paying.  A Direct3D 12 or XAudio2 guest crosses through vtable
         * slots far more often than through flat imports. */
        if (com && find_guest_com_target( mod, rip, com ))
        {
            if (xstat)
            {
                char name[EMU_XSTAT_NAME];

                xstat_name_com_slot( mod, com, name, sizeof(name) );
                stat_row = xstat_intern( EMU_XSTAT_COM, rip, name );
                xstat_hit( stat_row );
            }
            if (!no_cache)
            {
                struct thunk_rip_cache_entry val = { 0 };

                val.stat_row = stat_row;
                val.com = *com;
                thunk_rip_cache_put( rip, &val );
            }
        }
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
/* WINEEMUNOARGWIDTH=1 puts the old one-bit rule back: every argument the
 * generator measured as narrower than a pointer is cut to 32 bits and no
 * further, which is what this port did before the width word existed.  The
 * negative control for ppc64le/thunks/check-rip-cache.sh's sub-word leg, and
 * the only lever that can make it fail -- turning the CACHE off cannot,
 * because the bug was in what the cache carried rather than in the caching. */
static BOOL thunk_arg_width_off(void)
{
    static int off = -1;

    if (off == -1)
    {
        off = emu_env_flag( L"WINEEMUNOARGWIDTH" );
        if (off)
            ERR( "WINEEMUNOARGWIDTH: sub-word arguments will be cut to 32 bits "
                 "and keep the caller's leftovers above their own width\n" );
    }
    return off;
}

/* The same lever one step finer.  WINEEMUNOARGWIDTH turns off narrowing
 * altogether, which would also hide a signedness bug behind a width bug; this
 * one leaves the widths alone and zero-extends everything, so a gate can prove
 * that the SIGN bit is what carries a negative sub-word argument across and
 * not merely that some narrowing happens. */
static BOOL thunk_arg_sign_off(void)
{
    static int off = -1;

    if (off == -1)
    {
        off = emu_env_flag( L"WINEEMUNOARGSIGN" );
        if (off)
            ERR( "WINEEMUNOARGSIGN: signed sub-word arguments will be "
                 "zero-extended, so a negative one arrives as a large "
                 "positive\n" );
    }
    return off;
}

static void marshal_thunk_args( const AMD64_CONTEXT *ctx, UINT argc, UINT narrow,
                                UINT widths, UINT signs, ULONG_PTR *a )
{
    UINT i;

    if (thunk_arg_width_off()) widths = 0;
    if (thunk_arg_sign_off()) signs = 0;

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
        /* An argument narrower than a pointer carries the caller's leftovers
         * above its own width, and LP64-built native code reads all 64 bits.
         * The width word says how far down to cut; the narrow mask is the same
         * measurement at one-bit resolution and is what this used to use, which
         * cut a WORD or a BYTE to 32 bits and left half its garbage in place.
         * See THUNK_WIDTH.  An export the oracle could not measure has width 0
         * everywhere, so the mask still decides for it, exactly as before. */
        if (widths) a[i] = narrow_thunk_arg( a[i], THUNK_WIDTH( widths, i ),
                                            THUNK_SIGNED( signs, i ) );
        else if (narrow & (1u << i)) a[i] = (UINT)a[i];
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
static void marshal_thunk_args_fp( const AMD64_CONTEXT *ctx, UINT argc, UINT narrow,
                                   UINT widths, UINT signs, UINT fp,
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
    if (thunk_arg_width_off()) widths = 0;
    if (thunk_arg_sign_off()) signs = 0;

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
        /* the same cut to the argument's own width as the integer path;
         * see marshal_thunk_args() and THUNK_WIDTH */
        if (widths) gpr[i] = narrow_thunk_arg( gpr[i], THUNK_WIDTH( widths, i ),
                                                THUNK_SIGNED( signs, i ) );
        else if (narrow & (1u << i)) gpr[i] = (UINT)gpr[i];
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
    /* the second pointer of the args block is the EC registration's cookie:
     * this stub's own row cell, or NULL on every trap-protocol path (and
     * from an unextended caller, which the len check keeps honest) */
    struct ec_row_cell *cell = len >= 2 * sizeof(void *) ? ((void **)args)[1] : NULL;
    /* the third is the unix side's lean-return stub: the normal end of this
     * dispatch calls it instead of paying NtCallbackReturn's syscall round
     * trip.  NULL under WINE_PPC64LE_NO_LEAN_RETURN=1 and from any caller
     * that predates it -- the syscall tail below is always the fallback. */
    NTSTATUS (*lean_return)( NTSTATUS, TEB * ) =
        len >= 3 * sizeof(void *) ? (NTSTATUS (*)( NTSTATUS, TEB * ))((void **)args)[2] : NULL;
    AMD64_CONTEXT *prev_trap_ctx = emu_current_trap_ctx;
    BOOL prev_ctx_rewritten = emu_trap_ctx_rewritten;
    thunk_override_func override = NULL;
    struct com_thunk_hit com = { 0 };
    NTSTATUS status = STATUS_SUCCESS;
    ULONG_PTR a[THUNK_MAX_ARGS] = { 0 };
    UINT sig = 0, argc, cb_mask = 0, cb_wide = 0, cb_argc = 0, fp = 0, widths = 0, signs = 0;
    ULONG_PTR ret;
    void *proc = NULL;
    BOOL from_cell = FALSE;

    /* raise-style overrides dispatch against this trap's guest state; saved
     * and restored so a nested dispatch (guest handler makes a thunk call)
     * leaves the outer one intact */
    emu_current_trap_ctx = ctx;
    emu_trap_ctx_rewritten = FALSE;

    if (cell && ReadAcquire( &cell->state ) == EC_CELL_RESOLVED)
    {
        /* the warm transition: this stub's row, read in place (a resolved
         * cell is immutable -- see the ec_row_cell note), no lookup at all.
         * The counting mirrors find_guest_thunk_target's cache-hit path
         * exactly: same row index, same tick check. */
        sig      = cell->e.sig;
        override = cell->e.override;
        cb_mask  = cell->e.cb_mask;
        cb_wide  = cell->e.cb_wide;
        cb_argc  = cell->e.cb_argc;
        fp       = cell->e.fp;
        widths   = cell->e.widths;
        signs    = cell->e.signs;
        if (cell->e.com.dispatch) com = cell->e.com;
        proc = cell->e.proc;
        if (xstat)
        {
            xstat_hit( cell->e.stat_row );
            xstat_tick_check();
        }
        /* check-rip-cache layer 4b counts these the way layer 4 counts the
         * shared cache's "thunk cache hit for" lines */
        TRACE( "ec cell hit for %p -> proc %p com %p\n",
               (void *)(ULONG_PTR)ctx->Rip, proc, com.dispatch );
        from_cell = TRUE;
    }
    if (!from_cell)
    {
        proc = find_guest_thunk_target( ctx->Rip, &sig, &override, &com, &cb_mask, &cb_wide, &cb_argc, &fp, &widths, &signs );
        /* first use of this stub's cell: file the freshly-cached row in it
         * (or mark it SLOW when a cache lever is armed -- the levers win) */
        if (cell) ec_cell_fill( cell, ctx->Rip );
    }
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
        xstat_event( XSTAT_EV_TRAP_UNHANDLED, "guest trap with no target" );
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

            /* FP arguments are read out of XmmRegisters and an FP return is
             * written into XmmRegisters[0]: both sides of the lazy contract */
            materialize_trap_ctx( ctx );
            marshal_thunk_args_fp( ctx, argc, THUNK_SIG_NARROW(sig), widths, signs, fp, gpr, fpr );
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
            marshal_thunk_args( ctx, argc, THUNK_SIG_NARROW(sig), widths, signs, a );
            /* registration-side interception of guest callbacks: swap each
             * declared callback argument for a native trampoline BEFORE the
             * native callee ever sees the pointer */
            if (cb_mask) wrap_thunk_callback_args( a, argc, cb_mask, cb_wide, cb_argc );
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

    /* The lean return: straight back into call_emu_trap_dispatcher's frame
     * with no syscall.  EVERY status value rides it -- the unix side reads
     * NtCallbackReturn's status as a plain return value (its implementation
     * is a prev_frame check and user_mode_callback_return; the
     * THREAD_IS_TERMINATING / EMU_GUEST_EXCEPTION arms live in
     * emu_trap_dispatch_common AFTER the return, on either route).  The stub
     * comes back only when it cannot serve -- a context stash on the frame,
     * or no callback frame at all -- and then the syscall tail below is
     * exactly yesterday's path.  A non-sentinel RETURN from the stub is
     * impossible by construction (success does not return); turning it into
     * a loud raise keeps that claim falsifiable. */
    if (lean_return)
    {
        NTSTATUS lr = lean_return( status, NtCurrentTeb() );
        if (lr != EMU_LEAN_RETURN_FALLBACK) RtlRaiseStatus( STATUS_INTERNAL_ERROR );
        TRACE( "lean return fell back to NtCallbackReturn (restore_flags stash or no frame)\n" );
    }
    status = NtCallbackReturn( NULL, 0, status );
    RtlRaiseStatus( status );
}


/***********************************************************************
 *           the i386 (WoW64) guest thunk dispatcher
 *
 * The 32-bit lane is real WoW64: Wine's own i386 builtins run under the
 * emulator and convert at the SYSCALL boundary, so almost nothing 32-bit
 * ever reaches this file.  The exception is the modules whose implementation
 * this tree REPLACED with a native library behind a unixlib -- d3d11, dxgi,
 * d3d10core (DXVK) -- whose i386 halves are spec2thunk stub PEs exactly like
 * their x86-64 siblings: `int 0x80` at every export and every COM vtable
 * slot.  unix_emu32_run classifies such a trap as EMU32_RUN_TRAP and
 * BTCpuSimulate hands the I386_CONTEXT here, already on the Win32 stack (the
 * bounded-run shape: there is no emulator frame below us to preserve).
 *
 * The resolve walks the 32-BIT loader list -- the i386 loader is the guest's
 * own ntdll32 and these modules never appear in the 64-bit PEB -- and
 * resolves the NATIVE namesake exactly as the 64-bit lane does, through
 * LdrLoadDll/LdrGetProcedureAddress, which work fine from native code in a
 * wow64 process (wow64.dll itself is loaded that way).  Flat slots then need
 * what MS-x64 never did: the stdcall FRAME -- i386 passes everything in
 * 4-byte stack slots and the CALLEE pops them -- which is exactly what the
 * version-8 geom32 word carries, measured by the generator's own clang at
 * the i386 target.  A slot with no geometry is REFUSED, loudly, because a
 * frame that cannot be decoded cannot be popped either.
 *
 * A COM slot is handed whole to the native module's __wine_com_dispatch32
 * (libs/winecom's 32-bit dispatcher), which owns everything including the
 * pop: the frame arithmetic lives in the marshal table's I386_GEOM rows and
 * this side has no view of them.  Contrast the 64-bit contract, where the
 * dispatcher pops -- on MS-x64 the caller cleans up and the pop is one
 * return-address; on i386 stdcall the pop IS per-slot knowledge.
 *
 * The RIP cache is shared with the 64-bit lane: the two key spaces cannot
 * collide (i386 images sit below 4 GiB, the x86-64 thunk base is
 * 0x180000000), and lane32 cross-checks it anyway.
 */
typedef NTSTATUS (WINAPI *com_dispatch32_func)( UINT iface, UINT slot, I386_CONTEXT *ctx );

typedef struct
{
    LIST_ENTRY32        InLoadOrderLinks;
    LIST_ENTRY32        InMemoryOrderLinks;
    LIST_ENTRY32        InInitializationOrderLinks;
    ULONG               DllBase;
    ULONG               EntryPoint;
    ULONG               SizeOfImage;
    UNICODE_STRING32    FullDllName;
    UNICODE_STRING32    BaseDllName;
} EMU32_LDR_ENTRY;

/* Export lookup on a mapped PE32 image that the 64-bit loader has never
 * seen: RtlImageDirectoryEntryToData reads either optional-header shape, and
 * the export directory itself is machine-independent.  Forwarders are not
 * followed -- every name asked for here is a data table or a private entry
 * the module defines itself. */
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

/* The native namesake of a 32-bit thunk module, loaded on demand.  The name
 * arrives as a UNICODE_STRING32 -- and the load is PINNED to the system
 * directory, not searched by bare name: the default search order starts at
 * the APPLICATION directory, and a game that ships its own i386 d3d11.dll /
 * dxgi.dll beside the .exe (Dex does) would have the 64-bit loader pick THE
 * GUEST'S COPY up as the "native" module.  [MEASURED 2026-08-28: import_dll
 * faulted at +0x31c reading inside the mis-loaded image, the exception
 * leaked loader_section, and every later thread in the process piled up
 * behind it.]  The native builtins live in system32 on this port; nothing
 * an application ships can be the native half of a thunk module. */
static HMODULE emu32_native_module( const UNICODE_STRING32 *name32 )
{
    UNICODE_STRING name;
    HMODULE native;

    name.Buffer = (WCHAR *)(ULONG_PTR)name32->Buffer;
    name.Length = name32->Length;
    name.MaximumLength = name32->MaximumLength;
    if (LdrGetDllHandle( native_system_dir, 0, &name, &native ) &&
        LdrLoadDll( native_system_dir, 0, &name, &native ))
        return NULL;
    return native;
}

/* The locked walk half of the 32-bit resolve; the caller below owns the
 * loader lock and the fault protection.  no_cache is passed in so the two
 * halves read one answer. */
static void *find_guest_thunk_target32_walk( ULONG_PTR rip, UINT *sig_out, UINT *fp,
                                             UINT *widths, UINT *signs, UINT *geom32,
                                             struct com_thunk_hit *com, UINT *stat_out,
                                             int no_cache )
{
    UINT stat_row = XSTAT_NO_ROW;
    PEB_LDR_DATA32 *ldr;
    LIST_ENTRY32 *mark;
    ULONG e32;
    TEB32 *teb32;
    PEB32 *peb32;
    void *ret = NULL;

    teb32 = (TEB32 *)((char *)NtCurrentTeb() + NtCurrentTeb()->WowTebOffset);
    if (!NtCurrentTeb()->WowTebOffset || !(peb32 = (PEB32 *)(ULONG_PTR)teb32->Peb) ||
        !(ldr = (PEB_LDR_DATA32 *)(ULONG_PTR)peb32->LdrData))
        return NULL;

    mark = &ldr->InMemoryOrderModuleList;
    for (e32 = mark->Flink; e32 != (ULONG)(ULONG_PTR)mark;
         e32 = ((LIST_ENTRY32 *)(ULONG_PTR)e32)->Flink)
    {
        EMU32_LDR_ENTRY *mod = CONTAINING_RECORD( (LIST_ENTRY32 *)(ULONG_PTR)e32,
                                                  EMU32_LDR_ENTRY, InMemoryOrderLinks );
        ULONG_PTR base = mod->DllBase;
        const IMAGE_NT_HEADERS *nt = RtlImageNtHeader( (HMODULE)base );
        const struct thunk_info *info;
        const struct com_thunk_info *cinfo;
        const UINT *names, *impl_names;
        ANSI_STRING func_name;
        HMODULE native;
        void *proc;
        UINT idx, sig, i;

        if (!nt || nt->FileHeader.Machine != IMAGE_FILE_MACHINE_I386) continue;
        if (rip < base || rip >= base + nt->OptionalHeader.SizeOfImage) continue;

        if (!(info = find_export32( base, "__wine_thunk_info" )))
        {
            /* Wine's own i386 builtins and the application's DLLs live in
             * this list too; an int 0x80 inside one of those is not a thunk
             * trap, it is the guest's problem.  Fall through to the fault. */
            TRACE( "i386 trap at %p is inside %s, which is not a thunk module\n",
                   (void *)rip, debugstr_wn( (WCHAR *)(ULONG_PTR)mod->BaseDllName.Buffer,
                                             mod->BaseDllName.Length / sizeof(WCHAR) ) );
            goto done;
        }
        if (info->version != THUNK_INFO_VERSION || !info->stride)
        {
            ERR( "i386 module at %p has thunk info version %u, expected %u\n",
                 (void *)base, info->version, THUNK_INFO_VERSION );
            goto done;
        }

        if (rip < base + info->stubs_rva + info->trap_off) goto try_com32;
        idx = (rip - (base + info->stubs_rva + info->trap_off)) / info->stride;
        if (idx >= info->count) goto try_com32;
        if (base + info->stubs_rva + info->trap_off + (ULONG_PTR)idx * info->stride != rip)
            goto try_com32;

        names      = (const UINT *)(base + info->names_rva);
        impl_names = (const UINT *)(base + info->impl_names_rva);
        sig     = ((const UINT *)(base + info->sigs_rva))[idx];
        *fp     = ((const UINT *)(base + info->fp_rva))[idx];
        *widths = ((const UINT *)(base + info->widths_rva))[idx];
        *signs  = ((const UINT *)(base + info->signs_rva))[idx];
        *geom32 = ((const UINT *)(base + info->geom32_rva))[idx];
        *sig_out = sig;

        if (!(native = emu32_native_module( &mod->BaseDllName )))
        {
            WARN( "no native module to serve i386 %s\n",
                  (const char *)(base + names[idx]) );
            proc = NULL;
        }
        else
        {
            RtlInitAnsiString( &func_name, (char *)(base + impl_names[idx]) );
            if (LdrGetProcedureAddress( native, &func_name, 0, &proc ))
            {
                WARN( "native module has no %s to serve the i386 stub\n",
                      func_name.Buffer );
                proc = NULL;
            }
        }

        TRACE( "i386 %s -> %s %p (%u args, geom %08x)\n",
               (const char *)(base + names[idx]),
               (const char *)(base + impl_names[idx]), proc,
               THUNK_SIG_ARGC(sig), *geom32 );

        ret = proc;
        if (xstat)
        {
            char name[EMU_XSTAT_NAME];

            xstat_name_export( name, sizeof(name),
                               (WCHAR *)(ULONG_PTR)mod->BaseDllName.Buffer,
                               (const char *)(base + names[idx]) );
            stat_row = xstat_intern( EMU_XSTAT_FLAT, rip, name );
            xstat_hit( stat_row );
        }
        *stat_out = stat_row;
        if (!no_cache)
        {
            struct thunk_rip_cache_entry val = { 0 };

            val.stat_row = stat_row;
            val.proc     = proc;
            val.sig      = sig;
            val.fp       = *fp;
            val.widths   = *widths;
            val.signs    = *signs;
            val.geom32   = *geom32;
            val.lane32   = 1;
            thunk_rip_cache_put( rip, &val );
        }
        goto done;

    try_com32:
        if (!com || !(cinfo = find_export32( base, "__wine_com_thunk_info" )))
            goto done;
        if (cinfo->version != COM_THUNK_INFO_VERSION || !cinfo->stride) goto done;
        for (i = 0; i < cinfo->iface_count; i++)
        {
            const struct com_thunk_iface *ifaces =
                (const struct com_thunk_iface *)(base + cinfo->ifaces_rva);
            ULONG_PTR start = base + ifaces[i].stubs_rva + cinfo->trap_off;
            UINT slot;

            if (rip < start) continue;
            slot = (rip - start) / cinfo->stride;
            if (slot >= ifaces[i].slot_count) continue;
            if (start + (ULONG_PTR)slot * cinfo->stride != rip) continue;

            if (!(native = emu32_native_module( &mod->BaseDllName )))
            {
                ERR( "no native module to serve an i386 COM slot at %p\n", (void *)rip );
                goto done;
            }
            RtlInitAnsiString( &func_name, "__wine_com_dispatch32" );
            if (LdrGetProcedureAddress( native, &func_name, 0, &proc ))
            {
                ERR( "the native module behind the i386 COM stubs at %p exports "
                     "no __wine_com_dispatch32\n", (void *)rip );
                goto done;
            }
            com->dispatch = (com_dispatch_func)proc;   /* really com_dispatch32_func */
            com->iface = i;
            com->slot = slot;
            if (xstat)
            {
                char name[EMU_XSTAT_NAME];

                xstat_name_com_slot32( &mod->BaseDllName, com, name, sizeof(name) );
                stat_row = xstat_intern( EMU_XSTAT_COM, rip, name );
                xstat_hit( stat_row );
            }
            *stat_out = stat_row;
            if (!no_cache)
            {
                struct thunk_rip_cache_entry val = { 0 };

                val.stat_row = stat_row;
                val.com = *com;
                val.lane32 = 1;
                thunk_rip_cache_put( rip, &val );
            }
            goto done;
        }
        goto done;
    }
done:
    return ret;
}

/* Resolve a trapping i386 EIP to a flat native proc or a COM slot, through
 * the shared RIP cache.  Caller is BTCpuSimulate: PE code, no locks held.
 *
 * THE WALK CAN FAULT, and the first Dex run proved it [MEASURED 2026-08-28:
 * native fault in ntdll reading 0xFEB703B0 with the loader lock held, every
 * other thread then piling up behind loader_section].  The 32-bit loader
 * list belongs to the guest's own ntdll32 and is guarded by the GUEST's
 * loader lock, which no native thread can take -- so a guest thread inside
 * its 32-bit LoadLibrary can splice the list mid-walk, and a torn link is
 * not just a miss, it is a wild pointer.  The walk therefore runs under
 * __TRY, the loader lock is released on the way out of a fault instead of
 * leaking (which is what turned one torn read into a process-wide hang),
 * and the resolve retries: the splice window is instructions wide, so the
 * second attempt lands after the store that tore the first. */
static void *find_guest_thunk_target32( ULONG_PTR rip, UINT *sig_out, UINT *fp,
                                        UINT *widths, UINT *signs, UINT *geom32,
                                        struct com_thunk_hit *com, UINT *stat_out )
{
    static int no_cache = -1;
    struct thunk_rip_cache_entry hit;
    void *ret = NULL;
    ULONG_PTR magic;
    UINT attempt;
    BOOL torn;

    if (no_cache == -1) no_cache = emu_env_flag( L"WINEEMUNORIPCACHE" );
    if (!xstat_probed) xstat_probe();

    *sig_out = *fp = *widths = *signs = *geom32 = 0;
    *stat_out = XSTAT_NO_ROW;

    if (!no_cache && thunk_rip_cache_get( rip, &hit ))
    {
        if (!hit.lane32)
        {
            /* the two lanes' keys cannot collide; if this ever fires the
             * cache is corrupt and a miss is the only safe answer */
            ERR( "i386 trap at %p hit a 64-bit cache entry\n", (void *)rip );
            return NULL;
        }
        *sig_out = hit.sig;
        *fp      = hit.fp;
        *widths  = hit.widths;
        *signs   = hit.signs;
        *geom32  = hit.geom32;
        *stat_out = hit.stat_row;
        if (com && hit.com.dispatch) *com = hit.com;
        if (xstat)
        {
            xstat_hit( hit.stat_row );
            xstat_tick_check();
        }
        return hit.proc;
    }

    for (attempt = 0; attempt < 16; attempt++)
    {
        torn = FALSE;
        *sig_out = *fp = *widths = *signs = *geom32 = 0;
        *stat_out = XSTAT_NO_ROW;
        memset( com, 0, sizeof(*com) );

        /* The 64-bit loader lock, not for the 32-bit list (it does not guard
         * it) but because the resolve loads native modules (LdrLoadDll) and
         * one lock order beats two. */
        LdrLockLoaderLock( 0, NULL, &magic );
        __TRY
        {
            ret = find_guest_thunk_target32_walk( rip, sig_out, fp, widths, signs,
                                                  geom32, com, stat_out, no_cache );
        }
        __EXCEPT_ALL
        {
            torn = TRUE;
        }
        __ENDTRY
        LdrUnlockLoaderLock( 0, magic );
        if (!torn) return ret;
        WARN( "torn 32-bit loader-list walk resolving %p (attempt %u); retrying\n",
              (void *)rip, attempt );
        NtYieldExecution();
    }
    ERR( "the 32-bit loader-list walk for %p kept faulting; giving the trap up\n",
         (void *)rip );
    return NULL;
}

/***********************************************************************
 *           emu32_dispatch_thunk
 *
 * Serve one i386 thunk trap: flat export or COM vtable slot.  Returns
 * STATUS_SUCCESS with the context advanced past the call (Eip popped, the
 * stdcall frame popped, Eax/Edx carrying the result); any other status means
 * the trap was not served and the caller raises the guest-shaped fault.
 */
NTSTATUS emu32_dispatch_thunk( I386_CONTEXT *ctx )
{
    struct com_thunk_hit com = { 0 };
    ULONG_PTR rip = ctx->Eip;
    const ULONG *esp = (const ULONG *)(ULONG_PTR)ctx->Esp;
    UINT sig = 0, fp = 0, widths = 0, signs = 0, geom = 0, stat_row = XSTAT_NO_ROW;
    UINT argc, i, slot;
    ULONG_PTR ret;
    void *proc;

    proc = find_guest_thunk_target32( rip, &sig, &fp, &widths, &signs, &geom,
                                      &com, &stat_row );

    if (com.dispatch)
    {
        NTSTATUS status = ((com_dispatch32_func)com.dispatch)( com.iface, com.slot, ctx );
        if (status)
            ERR( "i386 com dispatch iface %u slot %u failed, status %08x\n",
                 com.iface, com.slot, (UINT)status );
        return status ? STATUS_ILLEGAL_INSTRUCTION : STATUS_SUCCESS;
    }
    if (!proc)
    {
        ERR( "unhandled i386 guest trap at %p\n", (void *)rip );
        return STATUS_ILLEGAL_INSTRUCTION;
    }
    if (!THUNK_GEOM32_VALID( geom ))
    {
        /* No frame word, no service: without it this side cannot even pop
         * the caller's arguments, and a wrong Esp is forever.  The generator
         * refuses geometry for variadics, x87 returns, hidden-sret frames
         * and anything its oracle could not measure. */
        ERR( "i386 stub at %p resolves to %p but publishes no frame geometry; "
             "refusing the call\n", (void *)rip, proc );
        return STATUS_ILLEGAL_INSTRUCTION;
    }

    argc = THUNK_SIG_ARGC( sig );

    if (!fp)
    {
        ULONG_PTR a[THUNK_MAX_ARGS] = { 0 };

        for (i = 0, slot = 1; i < argc && i < THUNK_MAX_ARGS; i++)
        {
            if (THUNK_GEOM32_QWORD( geom, i ))
            {
                a[i] = esp[slot] | ((ULONG_PTR)esp[slot + 1] << 32);
                slot += 2;
            }
            else a[i] = esp[slot++];
            /* the same declared-width cut as the 64-bit lane: an i386 caller
             * owes only the declared bits of a sub-word slot too */
            a[i] = narrow_thunk_arg( a[i], THUNK_WIDTH( widths, i ),
                                     THUNK_SIGNED( signs, i ) );
        }
        ret = call_native_thunk( proc, a );
    }
    else
    {
        /* Floating point on either side: same two ELFv2 register files as
         * the 64-bit lane, loaded from i386 stack slots instead of XMMs.  An
         * FP RETURN never gets here -- the generator publishes no geometry
         * for x87 ST(0) returns and the VALID check above already refused. */
        const UINT fp_mask = THUNK_FP_MASK( fp ), single = THUNK_FP_SINGLE( fp );
        ULONG_PTR gpr[THUNK_MAX_ARGS];
        double fpr[THUNK_MAX_FP_ARGS], fp_ret = 0.0;
        UINT nfpr = 0;

        memset( gpr, 0, sizeof(gpr) );
        memset( fpr, 0, sizeof(fpr) );
        for (i = 0, slot = 1; i < argc && i < THUNK_MAX_ARGS; i++)
        {
            if (fp_mask & (1u << i))
            {
                double v;
                if (single & (1u << i)) v = *(const float *)&esp[slot];
                else memcpy( &v, &esp[slot], sizeof(v) );
                if (nfpr < THUNK_MAX_FP_ARGS) fpr[nfpr++] = v;
            }
            else
            {
                if (THUNK_GEOM32_QWORD( geom, i ))
                    gpr[i] = esp[slot] | ((ULONG_PTR)esp[slot + 1] << 32);
                else
                    gpr[i] = esp[slot];
                gpr[i] = narrow_thunk_arg( gpr[i], THUNK_WIDTH( widths, i ),
                                           THUNK_SIGNED( signs, i ) );
            }
            slot += THUNK_GEOM32_QWORD( geom, i ) ? 2 : 1;
        }
        ret = call_native_thunk_fp( proc, gpr, fpr, &fp_ret );
        (void)fp_ret;   /* no geometry for an FP return; unreachable by the guard */
    }

    ctx->Eax = (ULONG)ret;
    if (THUNK_GEOM32_RET_QWORD( geom )) ctx->Edx = (ULONG)(ret >> 32);
    ctx->Eip = esp[0];
    ctx->Esp += 4 + 4 * THUNK_GEOM32_SLOTS( geom );
    return STATUS_SUCCESS;
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
