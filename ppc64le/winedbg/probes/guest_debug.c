/*
 * guest_debug.c -- an x86-64 guest that makes itself worth debugging.
 *
 * check-guest-debug.sh points winedbg at this and compares what the debugger
 * prints against values this file put there.  Everything it does is chosen so
 * that a WRONG answer cannot look like a right one:
 *
 *   THE REGISTERS ARE SENTINELS, not whatever the compiler left lying around.
 *   Sixteen bytes of pattern per register, distinct per register, and set by
 *   inline assembly immediately before the fault so that no call sequence can
 *   overwrite them.  A debugger that showed the HOST's registers, or a stale
 *   guest snapshot, or a zeroed block, cannot produce these numbers by
 *   accident -- and a debugger that transposed two registers is caught too,
 *   because each pattern names its own register.
 *
 *   THE STACK CARRIES A MARKER.  A register dump alone would still pass if the
 *   guest stack had been unmapped by the time the debugger looked, which is
 *   exactly what used to happen: the emulator's run loop frees the guest stack
 *   when the run ends, and a fatal guest fault ends the run before the report
 *   is made.  So the deepest frame keeps GUEST_STACK_MARK in a stack local and
 *   requires the debugger to find it in the debuggee's memory just above the
 *   guest RSP.
 *
 *   THE FAULT IS THREE CALLS DEEP, and each level is a real function with its
 *   own frame and its own .pdata entry, so a backtrace has something to walk.
 *   The levels are named and must appear in order.  One frame proves the
 *   program counter was found; four prove the x86-64 unwinder ran against the
 *   guest's own unwind data on a host that cannot execute a single one of the
 *   guest's instructions.
 *
 *   IT PARKS BEFORE FAULTING.  The gate needs a live process to attach to and
 *   a fault to arrive AFTER the attach, so the probe prints its pid, prints
 *   READY, and then waits for a file the gate creates -- never a wall-clock
 *   guess on either side.
 *
 * NO C RUNTIME, for the same reason every other guest probe here has none: a
 * CRT would put its own frames, its own registers and its own exception
 * handling between the thing under test and the answer.
 *
 * GUEST_DEBUG_MODE selects what the probe does, so one binary serves every
 * layer:
 *   0  park, then fault three frames deep with sentinel registers  (default)
 *   1  park, then return without faulting -- the attach-only layer, which
 *      must not need a crash to prove that attaching stops the process
 *
 * Copyright 2026 the ppc64le port authors
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 */

#include <windows.h>

#ifndef GUEST_DEBUG_MODE
#define GUEST_DEBUG_MODE 0
#endif

/* The one address the probe writes through, and the one it faults on.  Chosen
 * so that the fault's reported address is recognisable in a log at a glance
 * and cannot be confused with a real allocation. */
#define GUEST_FAULT_ADDR  0x00000000DEAD1000ull

/* Left on the guest stack, in the faulting frame.  The gate requires a
 * cross-process read just above the reported guest RSP to find it. */
#define GUEST_STACK_MARK  0xFEEDFACE5AFE0001ull

/* Written to by every frame so that none of them can be optimised away. */
static volatile int guest_debug_sink;

static void out( const char *s )
{
    DWORD n = 0, len = 0;
    while (s[len]) len++;
    WriteFile( GetStdHandle( STD_OUTPUT_HANDLE ), s, len, &n, NULL );
}

static void out_hex64( unsigned long long v )
{
    char d[17];
    int i;
    for (i = 15; i >= 0; i--) { d[i] = "0123456789ABCDEF"[v & 0xf]; v >>= 4; }
    d[16] = 0;
    out( d );
}

/* The deepest frame: put the marker on the stack, load the sentinels, fault.
 *
 * The sentinels go into the callee-saved registers as well as the volatile
 * ones because the two are reported by different mechanisms -- a volatile
 * register is whatever the trapping instruction left, a callee-saved one has
 * to survive the emulator's own bookkeeping -- and a port that got one right
 * and the other wrong would still look correct on half a dump.
 *
 * RBX/RSI/RDI/R12/R13/R14/R15 are callee-saved under MS-x64, so clobbering
 * them is declared to the compiler and it restores them; the fault means it
 * never gets the chance, which is the point.
 *
 * THE MARKER IS A LOCAL AND NOT A `push`, and that is a lesson rather than a
 * style choice.  It was a `pushq` inside the block below at first, which put
 * it exactly at [rsp] -- and moved RSP eight bytes behind the compiler's back.
 * The unwind info in .pdata describes the frame the COMPILER built, so the
 * x86-64 unwinder then read the return address one slot off and came back with
 * zero: [MEASURED] a backtrace of exactly one frame, produced by an unwinder
 * that was working perfectly on a stack the probe had lied about.  A volatile
 * local lives in the frame the unwind data describes, so both claims -- "the
 * guest stack is readable" and "the guest stack can be walked" -- can be made
 * by the same probe at the same time. */
static __attribute__((noinline)) void guest_debug_level3( void )
{
    volatile unsigned long long mark = GUEST_STACK_MARK;

    guest_debug_sink += (int)mark;      /* force it to memory, not a register */

    __asm__ __volatile__(
        "movabsq $0x1111111100000011, %%rax\n\t"
        "movabsq $0x2222222200000022, %%rbx\n\t"
        "movabsq $0x3333333300000033, %%rdx\n\t"
        "movabsq $0x4444444400000044, %%rsi\n\t"
        "movabsq $0x5555555500000055, %%rdi\n\t"
        "movabsq $0x6666666600000066, %%r12\n\t"
        "movabsq $0x7777777700000077, %%r13\n\t"
        "movabsq $0x8888888800000088, %%r14\n\t"
        "movabsq $0x9999999900000099, %%r15\n\t"
        "movabsq %0, %%rcx\n\t"
        "movl $0, (%%rcx)\n\t"               /* the fault */
        :
        : "i" ((unsigned long long)GUEST_FAULT_ADDR)
        : "rax", "rbx", "rcx", "rdx", "rsi", "rdi",
          "r12", "r13", "r14", "r15", "memory" );
}

/* Two ordinary frames above it.  noinline AND a volatile global in each, so
 * that neither can be inlined away nor turned into a tail call: an inlined
 * call still SHOWS in a backtrace, because dbghelp synthesises a frame per
 * DWARF inline record at the same PC, and a gate that accepted those would
 * pass without the x86-64 unwinder having stepped a single frame.  Four
 * distinct return addresses is the claim. */
static __attribute__((noinline)) void guest_debug_level2( void )
{
    guest_debug_sink++;
    guest_debug_level3();
    guest_debug_sink++;
}

static __attribute__((noinline)) void guest_debug_level1( void )
{
    guest_debug_sink++;
    guest_debug_level2();
    guest_debug_sink++;
}

void __stdcall guest_debug_entry( void )
{
    char path[MAX_PATH];
    DWORD n;

    out( "guest_debug: pid=" );
    out_hex64( GetCurrentProcessId() );
    out( "\n" );
    out( "guest_debug: fault-addr=" );
    out_hex64( GUEST_FAULT_ADDR );
    out( "\n" );
    out( "guest_debug: stack-mark=" );
    out_hex64( GUEST_STACK_MARK );
    out( "\n" );
    out( "guest_debug: READY\n" );

    /* Park until the gate says the debugger is attached.  A file rather than a
     * sleep, so the gate never has to guess how long an attach takes on a
     * loaded machine and the probe never faults before anybody is watching. */
    n = GetEnvironmentVariableA( "GUEST_DEBUG_GO", path, sizeof(path) );
    if (n && n < sizeof(path))
    {
        int i;
        for (i = 0; i < 3000; i++)
        {
            if (GetFileAttributesA( path ) != INVALID_FILE_ATTRIBUTES) break;
            Sleep( 100 );
        }
    }
    else Sleep( 5000 );

#if GUEST_DEBUG_MODE == 1
    out( "guest_debug: NOFAULT\n" );
    ExitProcess( 0 );
#else
    out( "guest_debug: FAULTING\n" );
    guest_debug_level1();
    out( "guest_debug: SURVIVED\n" );   /* must never print */
    ExitProcess( 1 );
#endif
}
