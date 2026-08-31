/*
 * steamrpc_chkstk.c -- the stack probe clang emits for a large frame.
 *
 * Proton's load_steamclient() has two 4096-byte buffers, so clang calls a
 * stack-probe helper before moving the stack pointer.  Nothing in the guest
 * import surface provides one (the mingw runtime is not linked here), and
 * -mno-stack-arg-probe would be the wrong fix: this port grows a guest stack
 * from guard-page faults, so a frame that steps over the guard page without
 * touching it is exactly the __chkstk bug the port already fixed once.  So
 * the probe is provided, in its standard form: touch one byte per page down
 * to the new stack pointer.
 *
 * THE TWO GUEST MACHINES WANT DIFFERENT HELPERS, AND THEY ARE NOT THE SAME
 * SHAPE.  Measured with llvm-objdump on the objects clang actually produced
 * for each target, not assumed:
 *
 *   x86-64  clang emits `mov $N,%rax; call ___chkstk_ms` and then subtracts
 *           RSP ITSELF.  ___chkstk_ms only probes; it must leave RSP and
 *           every register exactly as it found them.
 *
 *   i386    clang emits `mov $N,%eax; call __alloca` and then does NOT touch
 *           ESP -- the very next instruction already addresses its incoming
 *           argument at (frame + N), so the callee has moved ESP.  This is
 *           libgcc's allocating form from config/i386/cygwin.S, where
 *           ___chkstk and __alloca are the same code: probe down, then move
 *           ESP by EAX and return through the relocated return address.
 *           Providing the x86-64 semantics under the i386 name would link
 *           cleanly and then run with a frame the compiler believes it owns
 *           and the callee never allocated.
 *
 * The i386 half defines every name libgcc does, not just the one observed
 * today, because which helper clang emits moves with the optimisation level
 * and with whether the allocation is dynamic.
 *
 * Copyright 2026 the ppc64le port authors
 *
 * This library is free software; you can redistribute it and/or modify it
 * under the terms of the GNU Lesser General Public License as published by
 * the Free Software Foundation; either version 2.1 of the License, or (at
 * your option) any later version.
 */

#ifdef __i386__

/* Two DIFFERENT helpers, exactly as libgcc's config/i386/cygwin.S has them:
 * ___chkstk_ms probes and returns with ESP untouched, while ___chkstk and
 * __alloca probe and then move ESP by EAX.  Aliasing the two together links
 * fine and then either double-allocates or never allocates, so they are kept
 * apart here even though only __alloca is observed at today's flags. */

__asm__(".globl ___chkstk_ms\n"
        "___chkstk_ms:\n\t"
        "push %ecx\n\t"
        "push %eax\n\t"
        "cmp $0x1000,%eax\n\t"
        "lea 12(%esp),%ecx\n\t"    /* past saved ECX, saved EAX, return addr */
        "jb 1f\n"
        "2:\n\t"
        "sub $0x1000,%ecx\n\t"
        "orl $0,(%ecx)\n\t"
        "sub $0x1000,%eax\n\t"
        "cmp $0x1000,%eax\n\t"
        "ja 2b\n"
        "1:\n\t"
        "sub %eax,%ecx\n\t"
        "orl $0,(%ecx)\n\t"
        "pop %eax\n\t"
        "pop %ecx\n\t"
        "ret\n");

__asm__(".globl ___chkstk\n"
        ".globl __alloca\n"
        "___chkstk:\n"
        "__alloca:\n\t"
        "push %ecx\n\t"
        "lea 8(%esp),%ecx\n\t"     /* past the saved ECX and the return addr */
        "cmp $0x1000,%eax\n\t"
        "jb 1f\n"
        "2:\n\t"
        "sub $0x1000,%ecx\n\t"
        "orl $0,(%ecx)\n\t"
        "sub $0x1000,%eax\n\t"
        "cmp $0x1000,%eax\n\t"
        "ja 2b\n"
        "1:\n\t"
        "sub %eax,%ecx\n\t"
        "orl $0,(%ecx)\n\t"
        "mov %esp,%eax\n\t"
        "mov %ecx,%esp\n\t"        /* the allocation itself */
        "mov (%eax),%ecx\n\t"      /* saved ECX */
        "mov 4(%eax),%eax\n\t"     /* the return address */
        "push %eax\n\t"            /* keep the CPU call/return stack paired */
        "ret\n");

#else

/* Probe only: RSP and every register unchanged on return. */
__asm__(".globl ___chkstk_ms\n"
        "___chkstk_ms:\n\t"
        "push %rcx\n\t"
        "push %rax\n\t"
        "cmp $0x1000,%rax\n\t"
        "lea 24(%rsp),%rcx\n\t"
        "jb 1f\n"
        "2:\n\t"
        "sub $0x1000,%rcx\n\t"
        "orq $0,(%rcx)\n\t"
        "sub $0x1000,%rax\n\t"
        "cmp $0x1000,%rax\n\t"
        "ja 2b\n"
        "1:\n\t"
        "sub %rax,%rcx\n\t"
        "orq $0,(%rcx)\n\t"
        "pop %rax\n\t"
        "pop %rcx\n\t"
        "ret\n");

#endif
