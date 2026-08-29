/*
 * Guest-side x86-64 __intrinsic_setjmp / longjmp.
 *
 * WHY THIS IS REAL GUEST CODE AND NOT A THUNK.  setjmp's contract is "save
 * the caller's register file and stack pointer"; longjmp's is "put them
 * back and resume at the saved RIP".  The caller is x86-64 guest code, so
 * the registers that must be saved are the GUEST's RBX/RBP/RSI/RDI/R12-R15,
 * RSP, RIP, XMM6-XMM15, MxCsr and the x87 control word -- the Windows-x64
 * callee-saved set.  A spec2thunk trap stub marshals out to the native
 * ppc64 side and would save that machine's registers instead: a silent
 * wrong answer.  So these two functions are hand-written x86-64 executed by
 * the guest instruction stream itself, byte-compatible with the layout
 * Wine's own x86-64 ntdll uses (dlls/ntdll/signal_x86_64.c,
 * NTDLL__setjmpex/longjmp_regs), which is Microsoft's documented
 * _JUMP_BUFFER.
 *
 * JMP_BUF LAYOUT.  Exactly include/msvcrt/setjmp.h's x86-64 _JUMP_BUFFER --
 * 256 bytes, the caller allocates it -- and the _Static_asserts below prove
 * at compile time that every offset used by the assembly matches that
 * header.  Offsets:
 *   0x00 Frame   0x08 Rbx   0x10 Rsp   0x18 Rbp   0x20 Rsi   0x28 Rdi
 *   0x30 R12     0x38 R13   0x40 R14   0x48 R15   0x50 Rip
 *   0x58 MxCsr   0x5c FpCsr 0x5e Spare 0x60..0xff Xmm6..Xmm15
 * Rsp is saved as the value it will have AFTER the setjmp call returns
 * (entry rsp + 8), and Rip is the return address -- the same convention as
 * Microsoft's and Wine's, so longjmp is "mov rsp, saved; jmp saved-rip".
 *
 * THE UNWINDING LIMITATION, STATED PLAINLY.  Microsoft's longjmp runs SEH
 * unwind handlers (RtlUnwindEx) when jmp_buf->Frame is non-zero, so
 * __finally blocks and C++ destructors between the longjmp and the setjmp
 * frame execute.  This implementation NEVER unwinds:
 *
 *   - __intrinsic_setjmp stores Frame = 0 unconditionally (MSVC's intrinsic
 *     passes a frame pointer in RDX when the caller has EH state; it is
 *     deliberately ignored), so a longjmp on a buffer written here always
 *     takes the plain register-restore path.  Handlers between the two
 *     frames are SKIPPED, exactly as they are for a plain C /EHs-less
 *     _setjmp.  For the id-engine Com_Error idiom -- C code, no
 *     destructors -- that is the correct behaviour; for MSVC C++ code that
 *     longjmps across active destructors it is not, and closing THAT gap
 *     needs the guest-side EH machinery the vcruntime140.thunks comment
 *     already calls for (out of scope here, on purpose).
 *
 *   - longjmp on a buffer whose Frame is NON-zero -- one written by some
 *     OTHER setjmp implementation that recorded a real frame and was
 *     promised unwinding -- REFUSES rather than approximates: it executes
 *     ud2, an illegal-instruction fault at a named symbol in guestcrt.dll,
 *     instead of silently skipping the handlers that setjmp was told to
 *     arrange.  Since every setjmp writer this process can reach resolves
 *     to this module (the vcruntime140/ucrtbase forwarder chains), the
 *     refusal can only fire for a foreign or hand-built jmp_buf.
 *
 * longjmp(buf, 0) RETURNS 1, per the C standard and Microsoft's CRT; the
 *     probe (probes/guest/setjmp_regs.c) proves it at run time.
 *
 * __intrinsic_setjmpex IS DELIBERATELY NOT EXPORTED.  Its contract is the
 * unwinding variant; serving it with a Frame=0 store would silently
 * downgrade exactly the callers that asked for unwinding.  It stays a hole
 * (a named, diagnosable sentinel) until real guest-side unwinding exists.
 *
 * Copyright 2026 the ppc64le port authors
 *
 * This library is free software; you can redistribute it and/or modify it
 * under the terms of the GNU Lesser General Public License as published by
 * the Free Software Foundation; either version 2.1 of the License, or (at
 * your option) any later version.
 */

#include <setjmp.h>

/* The assembly below hard-codes these offsets; if include/msvcrt/setjmp.h
 * ever moves a field, fail the COMPILE, not the game. */
_Static_assert( sizeof(_JUMP_BUFFER) == 256, "jmp_buf must stay 256 bytes" );
_Static_assert( __builtin_offsetof(_JUMP_BUFFER, Frame) == 0x00, "Frame" );
_Static_assert( __builtin_offsetof(_JUMP_BUFFER, Rbx)   == 0x08, "Rbx" );
_Static_assert( __builtin_offsetof(_JUMP_BUFFER, Rsp)   == 0x10, "Rsp" );
_Static_assert( __builtin_offsetof(_JUMP_BUFFER, Rbp)   == 0x18, "Rbp" );
_Static_assert( __builtin_offsetof(_JUMP_BUFFER, Rsi)   == 0x20, "Rsi" );
_Static_assert( __builtin_offsetof(_JUMP_BUFFER, Rdi)   == 0x28, "Rdi" );
_Static_assert( __builtin_offsetof(_JUMP_BUFFER, R12)   == 0x30, "R12" );
_Static_assert( __builtin_offsetof(_JUMP_BUFFER, R13)   == 0x38, "R13" );
_Static_assert( __builtin_offsetof(_JUMP_BUFFER, R14)   == 0x40, "R14" );
_Static_assert( __builtin_offsetof(_JUMP_BUFFER, R15)   == 0x48, "R15" );
_Static_assert( __builtin_offsetof(_JUMP_BUFFER, Rip)   == 0x50, "Rip" );
_Static_assert( __builtin_offsetof(_JUMP_BUFFER, MxCsr) == 0x58, "MxCsr" );
_Static_assert( __builtin_offsetof(_JUMP_BUFFER, FpCsr) == 0x5c, "FpCsr" );
_Static_assert( __builtin_offsetof(_JUMP_BUFFER, Spare) == 0x5e, "Spare" );
_Static_assert( __builtin_offsetof(_JUMP_BUFFER, Xmm6)  == 0x60, "Xmm6" );
_Static_assert( __builtin_offsetof(_JUMP_BUFFER, Xmm15) == 0xf0, "Xmm15" );

/* Instruction-for-instruction the layout of Wine's own
 * dlls/ntdll/signal_x86_64.c NTDLL__setjmpex, with two deliberate
 * differences called out in the banner above: Frame is stored as 0 (not
 * RDX), and a non-zero Frame in longjmp refuses with ud2 instead of
 * unwinding.  movdqa (not movdqu): the type is 16-byte aligned and
 * Microsoft's own code faults on a misaligned buffer too, so hiding a
 * misalignment here would only defer the corruption.  The .seh_* directives
 * give both functions PE unwind entries; setjmp is a leaf, and longjmp
 * never returns through its own frame. */
__asm__(
    ".text\n"

    ".globl __intrinsic_setjmp\n"
    ".seh_proc __intrinsic_setjmp\n"
    "__intrinsic_setjmp:\n"
    ".seh_endprologue\n"
    "xorl %eax,%eax\n\t"
    "movq %rax,(%rcx)\n\t"           /* Frame = 0: never promise an unwind */
    "movq %rbx,0x8(%rcx)\n\t"
    "leaq 0x8(%rsp),%rax\n\t"
    "movq %rax,0x10(%rcx)\n\t"       /* Rsp as of after this call returns */
    "movq %rbp,0x18(%rcx)\n\t"
    "movq %rsi,0x20(%rcx)\n\t"
    "movq %rdi,0x28(%rcx)\n\t"
    "movq %r12,0x30(%rcx)\n\t"
    "movq %r13,0x38(%rcx)\n\t"
    "movq %r14,0x40(%rcx)\n\t"
    "movq %r15,0x48(%rcx)\n\t"
    "movq (%rsp),%rax\n\t"
    "movq %rax,0x50(%rcx)\n\t"       /* Rip = our return address */
    "stmxcsr 0x58(%rcx)\n\t"
    "fnstcw 0x5c(%rcx)\n\t"
    "movdqa %xmm6,0x60(%rcx)\n\t"
    "movdqa %xmm7,0x70(%rcx)\n\t"
    "movdqa %xmm8,0x80(%rcx)\n\t"
    "movdqa %xmm9,0x90(%rcx)\n\t"
    "movdqa %xmm10,0xa0(%rcx)\n\t"
    "movdqa %xmm11,0xb0(%rcx)\n\t"
    "movdqa %xmm12,0xc0(%rcx)\n\t"
    "movdqa %xmm13,0xd0(%rcx)\n\t"
    "movdqa %xmm14,0xe0(%rcx)\n\t"
    "movdqa %xmm15,0xf0(%rcx)\n\t"
    "xorl %eax,%eax\n\t"             /* direct return: 0 */
    "retq\n"
    ".seh_endproc\n"

    ".globl longjmp\n"
    ".seh_proc longjmp\n"
    "longjmp:\n"
    ".seh_endprologue\n"
    "cmpq $0,(%rcx)\n\t"             /* Frame != 0: an unwind was promised */
    "jne 1f\n\t"                     /*   -> refuse loudly, see banner      */
    "movl %edx,%eax\n\t"
    "testl %eax,%eax\n\t"            /* longjmp(buf, 0) must return 1 */
    "jnz 2f\n\t"
    "movl $1,%eax\n"
    "2:\t"
    "movq 0x8(%rcx),%rbx\n\t"
    "movq 0x18(%rcx),%rbp\n\t"
    "movq 0x20(%rcx),%rsi\n\t"
    "movq 0x28(%rcx),%rdi\n\t"
    "movq 0x30(%rcx),%r12\n\t"
    "movq 0x38(%rcx),%r13\n\t"
    "movq 0x40(%rcx),%r14\n\t"
    "movq 0x48(%rcx),%r15\n\t"
    "movq 0x50(%rcx),%rdx\n\t"
    "ldmxcsr 0x58(%rcx)\n\t"
    "fnclex\n\t"                     /* pending x87 faults must not fire on
                                      * the fldcw that unmasks them */
    "fldcw 0x5c(%rcx)\n\t"
    "movdqa 0x60(%rcx),%xmm6\n\t"
    "movdqa 0x70(%rcx),%xmm7\n\t"
    "movdqa 0x80(%rcx),%xmm8\n\t"
    "movdqa 0x90(%rcx),%xmm9\n\t"
    "movdqa 0xa0(%rcx),%xmm10\n\t"
    "movdqa 0xb0(%rcx),%xmm11\n\t"
    "movdqa 0xc0(%rcx),%xmm12\n\t"
    "movdqa 0xd0(%rcx),%xmm13\n\t"
    "movdqa 0xe0(%rcx),%xmm14\n\t"
    "movdqa 0xf0(%rcx),%xmm15\n\t"
    "movq 0x10(%rcx),%rsp\n\t"
    "jmp *%rdx\n"
    "1:\t"
    "ud2\n"                          /* refused: cannot unwind -- c000001d
                                      * at guestcrt.dll!longjmp+.., named in
                                      * any crash log, never a wrong answer */
    ".seh_endproc\n"
);

/* Wine relocates builtin PEs; an image with no .reloc directory cannot be.
 * The assembly above is entirely RIP-relative, so force one absolute
 * pointer the same way spec2thunk's generated stubs do. */
static void *const guestcrt_reloc_anchor __attribute__((used))
    = (void *)&guestcrt_reloc_anchor;

/* The PE entry point (guestcrt.guestpe's ENTRY line).  Nothing to set up:
 * no CRT, no TLS, no imports. */
int __stdcall DllMain( void *instance, unsigned int reason, void *reserved )
{
    (void)instance; (void)reason; (void)reserved;
    return 1;
}
