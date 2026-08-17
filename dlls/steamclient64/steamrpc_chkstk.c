/*
 * steamrpc_chkstk.c -- the stack probe clang emits for a large frame.
 *
 * Proton's load_steamclient() has two 4096-byte buffers, so clang calls
 * ___chkstk_ms before moving RSP.  Nothing in the guest import surface
 * provides it (the mingw runtime is not linked here), and -mno-stack-arg-probe
 * would be the wrong fix: this port grows a guest stack from guard-page
 * faults, so a frame that steps over the guard page without touching it is
 * exactly the __chkstk bug the port already fixed once.  So the probe is
 * provided, in its standard form: touch one byte per page down to the new
 * RSP, leaving RSP and every register unchanged.
 *
 * Copyright 2026 the ppc64le port authors
 *
 * This library is free software; you can redistribute it and/or modify it
 * under the terms of the GNU Lesser General Public License as published by
 * the Free Software Foundation; either version 2.1 of the License, or (at
 * your option) any later version.
 */

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
