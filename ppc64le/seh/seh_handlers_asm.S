/*
 * seh_handlers_asm.S -- the one frame in this probe that clang cannot emit.
 *
 * WHY THIS FILE EXISTS AT ALL.
 *
 * check-seh-handlers.sh gates the path where a guest frame's .xdata names a
 * language handler that is NOT ntdll's __C_specific_handler, and the port has
 * to ENTER that handler as guest code because nothing in a PE says what it is.
 * Every __try clang compiles names __C_specific_handler -- that is not a
 * setting, it is what the -windows-gnu and -windows-msvc back ends both emit --
 * so a probe built only from C reaches the port's IDENTITY fast path and never
 * touches the code under test.  There is exactly one construction on this
 * toolchain that puts a private handler into a real UNWIND_INFO: write the
 * frame by hand with the SEH directives mingw-w64 uses and let clang's
 * integrated assembler build the .pdata/.xdata for it.
 *
 * The directives, and what each one buys:
 *
 *   .seh_proc / .seh_endproc     emit the RUNTIME_FUNCTION into .pdata, so
 *                                RtlLookupFunctionEntry finds this frame at all.
 *   .seh_pushreg / .seh_stackalloc
 *                                emit the UNWIND_CODEs, so the virtual unwinder
 *                                can compute this frame's EstablisherFrame and
 *                                restore its nonvolatiles.  Two pushes and an
 *                                allocation rather than the minimum, because a
 *                                frame that unwinds correctly by accident (a
 *                                leaf, say) would prove nothing about the codes.
 *   .seh_handler pf_language_handler, @except, @unwind
 *                                sets UNW_FLAG_EHANDLER|UNW_FLAG_UHANDLER and
 *                                writes the handler RVA after the unwind codes.
 *                                THIS is the line the whole gate turns on.
 *   .seh_handlerdata             opens .xdata immediately after that RVA, where
 *                                a real SCOPE_TABLE is emitted by hand: a DWORD
 *                                Count followed by Count records of four
 *                                image-relative DWORDs.  The port hands this
 *                                block to the handler as DISPATCHER_CONTEXT->
 *                                HandlerData without interpreting it -- correct,
 *                                because handler data belongs to whichever
 *                                handler the .xdata named -- and the probe reads
 *                                it back to prove the pointer arrived intact.
 *
 * WHAT THE FRAME DOES.
 *
 *   ULONG64 pf_call( ULONG64 (*fn)( ULONG64 ), ULONG64 arg )
 *
 * calls fn(arg) inside its guarded range and returns its value; if instead an
 * unwind lands on pf_landing, it records what arrived in RAX and in the
 * nonvolatile RBX and returns that RAX.  Two things are being measured there
 * that no C frame can measure:
 *
 *   - RAX on arrival must be the ReturnValue the handler passed to RtlUnwindEx.
 *     A C __except body cannot see RAX; a landing pad written in assembler can
 *     store it before anything else touches it.
 *   - RBX must still hold the sentinel this frame put there BEFORE the call.
 *     Everything between the fault and here saved and restored RBX through its
 *     own unwind codes, so the sentinel surviving says the unwind rebuilt the
 *     TARGET frame's register state rather than resuming with the faulting
 *     frame's.  That is the difference between a working unwind and a jump.
 *
 * The frame has a SECOND landing pad, pf_consolidate_landing, for the
 * consolidating unwind; see the comment on it for why the two have to be
 * distinguishable.
 *
 * STACK ALIGNMENT, since it is the kind of thing that is wrong silently: the
 * call into this function leaves RSP == 8 (mod 16); two pushes take it to
 * 8 (mod 16) again and `subq $0x28` -- 40 bytes, which is 8 (mod 16) and at
 * least the 32 bytes of shadow space the x64 ABI requires a caller to reserve
 * -- takes it to 0 (mod 16), which is what `callq` needs.
 *
 * Copyright 2026 the ppc64le port authors
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 */

/* Kept in step with seh_handlers.c by hand; the probe asserts the value it
 * reads back, so a drift between the two is a failed step and not a mystery. */
#define PF_RBX_SENTINEL 0x5eb15eb15eb15eb1
#define PF_R12_SENTINEL 0x5eb25eb25eb25eb2

    .text

    .globl  pf_call
    .def    pf_call; .scl 2; .type 32; .endef
    .seh_proc pf_call
pf_call:
    pushq   %rbp
    .seh_pushreg %rbp
    pushq   %rbx
    .seh_pushreg %rbx
    subq    $0x28, %rsp
    .seh_stackalloc 0x28
    .seh_handler pf_language_handler, @except, @unwind
    .seh_endprologue

    /* The sentinel goes in before the guarded range opens, so that it is part
     * of this frame's state at every ControlPc the walk can see. */
    movabsq $PF_RBX_SENTINEL, %rbx

    /* This frame's ESTABLISHER FRAME, published for the one stage that has to
     * name it from outside: a guest calling RtlUnwindEx directly, rather than
     * from inside a language handler this walk entered, has no
     * DISPATCHER_CONTEXT to read it out of and must say which frame to unwind
     * to.  For a function that uses no frame register the x64 establisher frame
     * IS the post-prologue RSP, and the probe checks that claim rather than
     * assuming it: the private handler compares this against the
     * DISPATCHER_CONTEXT it is handed, and if the two ever disagreed the
     * in-place stage would be unwinding to an address of its own invention. */
    movq    %rsp, pf_call_frame(%rip)

.Lpf_try_begin:
    movq    %rcx, %rax          /* fn  */
    movq    %rdx, %rcx          /* arg */
    callq   *%rax
    jmp     .Lpf_done
.Lpf_try_end:

    /* The unwind target named by the scope table below, and exported so that
     * the handler in seh_handlers.c can pass its address to RtlUnwindEx.  The
     * unwind arrives here with this frame's RSP and nonvolatiles restored and
     * RAX holding the unwind's ReturnValue. */
    .globl  pf_landing
pf_landing:
.Lpf_landing:
    movq    %rax, pf_landing_rax(%rip)
    movq    %rbx, pf_landing_rbx(%rip)
    movq    $1,   pf_landing_hit(%rip)
    jmp     .Lpf_done

    /* THE SECOND LANDING PAD, and it is reached by a different road.
     *
     * A consolidating unwind -- RtlUnwindEx with STATUS_UNWIND_CONSOLIDATE,
     * which is how MSVC spells `catch` -- does not resume at the TargetIp it
     * was handed.  It runs the CONSOLIDATION ROUTINE in
     * ExceptionInformation[0] once the stack has been unwound, and the address
     * that routine RETURNS is the resume point.  So the probe hands
     * RtlUnwindEx pf_landing as its TargetIp and has the routine return THIS
     * label instead: which of the two pads ran then says, by itself and
     * without reading any port diagnostic, whether the resume came from the
     * routine's return value or from the ignored TargetIp.  A port that
     * quietly fell back to TargetIp would land one label earlier and the gate
     * would say so.
     *
     * Nothing falls into here: the pad above jumps past it. */
    .globl  pf_consolidate_landing
pf_consolidate_landing:
.Lpf_consolidate_landing:
    movq    %rax, pf_consolidate_rax(%rip)
    movq    %rbx, pf_consolidate_rbx(%rip)
    movq    $1,   pf_consolidate_hit(%rip)

.Lpf_done:
    addq    $0x28, %rsp
    popq    %rbx
    popq    %rbp
    retq

    /* Exported only so that the runner and the probe can state the frame's
     * bounds without hard-coding a size; nothing branches here. */
    .globl  pf_call_end
pf_call_end:

    /* The handler data.  A SCOPE_TABLE with one record:
     *   BeginAddress/EndAddress  the guarded range -- the call and the branch
     *                            past the landing pad, which is where every
     *                            ControlPc this frame can be interrupted at
     *                            lies;
     *   HandlerAddress           1 == EXCEPTION_EXECUTE_HANDLER, the encoding
     *                            __C_specific_handler uses for "__except(1),
     *                            no filter funclet".  Nothing in this port
     *                            interprets it for a private handler -- the
     *                            handler in seh_handlers.c decides for itself
     *                            -- and the value is chosen so that the block
     *                            is a WELL-FORMED scope table rather than a
     *                            plausible-looking one;
     *   JumpTarget               pf_landing, the address the handler asks
     *                            RtlUnwindEx to resume at.
     * @IMGREL is the image-relative (RVA) relocation an UNWIND_INFO's data
     * must use; an absolute address here would be read as an RVA and land
     * nowhere. */
    .seh_handlerdata
    .long   1
    .long   .Lpf_try_begin@IMGREL
    .long   .Lpf_try_end@IMGREL
    .long   1
    .long   .Lpf_landing@IMGREL

    /* .seh_handlerdata switches the assembler to .xdata; everything after a
     * handler data block has to switch back by hand or .seh_endproc emits the
     * end of the function into the wrong section. */
    .text
    .seh_endproc

    /* One byte of separation, then alignment.  Without it pf_call_end and the
     * start of the next frame are the SAME address, and a tool that names an
     * address by looking up a symbol is then free to call the second frame's
     * start "pf_call_end" -- which is exactly what llvm-readobj does, and which
     * would make the runner's second-opinion assertion unreadable.  int3 rather
     * than nop because nothing may fall through here. */
    .byte 0xcc
    .p2align 4, 0xcc


/*
 * THE SECOND FRAME: a CHAINING private handler.
 *
 * pf_call above covers "the .xdata names a handler of ours and that handler
 * decides everything itself".  This one covers the other shape a real image
 * produces, and the one an MSVC /GS build produces constantly:
 * __GSHandlerCheck_SEH, whose .xdata-named handler validates the frame's stack
 * cookie and then TAIL-CALLS __C_specific_handler with the same four arguments.
 * The port cannot serve that by identity -- the handler the .xdata names is the
 * image's own -- so it enters the chaining handler as guest code, and the
 * chaining handler then calls ntdll's __C_specific_handler as an ordinary
 * imported function.  That arrival is a different entry point in the port
 * (emu_C_specific_handler) from the one the frame walk uses, and it is the one
 * that used to be a loud refusal.
 *
 * Everything here is the same shape as pf_call, with two differences that are
 * deliberate rather than incidental:
 *
 *   - the sentinel lives in %r12 instead of %rbx, so that between the two
 *     frames the unwind codes for two DIFFERENT nonvolatile registers are
 *     exercised rather than the same one twice;
 *   - the scope record's HandlerAddress is again 1 (EXCEPTION_EXECUTE_HANDLER,
 *     the "__except(1), no filter funclet" encoding), which is what makes
 *     __C_specific_handler accept this scope without a filter of its own and
 *     unwind to JumpTarget with the exception code as the return value.  The
 *     landing pad therefore reads back the EXCEPTION CODE in RAX, which is a
 *     value nothing on this side chose -- __C_specific_handler's own semantics
 *     did -- and is the tightest check available that those semantics ran.
 */

    .globl  pf_chain_call
    .def    pf_chain_call; .scl 2; .type 32; .endef
    .seh_proc pf_chain_call
pf_chain_call:
    pushq   %rbp
    .seh_pushreg %rbp
    pushq   %r12
    .seh_pushreg %r12
    subq    $0x28, %rsp
    .seh_stackalloc 0x28
    .seh_handler pf_chain_handler, @except, @unwind
    .seh_endprologue

    movabsq $PF_R12_SENTINEL, %r12

.Lpc_try_begin:
    movq    %rcx, %rax          /* fn  */
    movq    %rdx, %rcx          /* arg */
    callq   *%rax
    jmp     .Lpc_done
.Lpc_try_end:

    .globl  pf_chain_landing
pf_chain_landing:
.Lpc_landing:
    movq    %rax, pf_chain_rax(%rip)
    movq    %r12, pf_chain_r12(%rip)
    movq    $1,   pf_chain_hit(%rip)

.Lpc_done:
    addq    $0x28, %rsp
    popq    %r12
    popq    %rbp
    retq

    .globl  pf_chain_call_end
pf_chain_call_end:

    .seh_handlerdata
    .long   1
    .long   .Lpc_try_begin@IMGREL
    .long   .Lpc_try_end@IMGREL
    .long   1
    .long   .Lpc_landing@IMGREL

    .text
    .seh_endproc
