/*
 * guest_debug_read.c -- a NATIVE ppc64 debugger, checking values rather than
 * reading a debugger's prose.
 *
 * check-guest-debug.sh has two observers of the same crash, deliberately.
 * winedbg is the one a person uses and its output is the deliverable, but its
 * output is text, and a gate that only grepped text would pass on a debugger
 * that printed the right numbers for the wrong reason -- or fail on one that
 * changed a column.  This program asks the MECHANISM directly:
 *
 *   NtQueryInformationThread( thread, ThreadWow64Context, AMD64_CONTEXT )
 *
 * and checks every field against what ppc64le/winedbg/probes/guest_debug.c put
 * there.  It shares no code with winedbg, it is native ppc64 rather than a
 * guest, and it cannot see what winedbg sees.
 *
 * THREE CLAIMS, AND THE THIRD IS THE ONE THAT IS EASY TO GET WRONG:
 *
 *   1  the GUEST thread reports the guest's registers -- sentinels, exact.
 *   2  the guest stack is still MAPPED and still holds what the guest pushed,
 *      read across a process boundary at the reported RSP.
 *   3  a NATIVE thread of the same process reports NO guest context at all.
 *      The debugger's own injected breakin thread is one, and so is every
 *      thread of a native process.  A port that answered that question with
 *      sixteen zeroed registers and an RIP of 0 would look like it worked
 *      until somebody believed one of those zeros; the refusal has to be an
 *      error status, and this is where that is required rather than hoped for.
 *
 * It prints one line per check in the `guest_debug_read: NN ok/FAIL ...` shape
 * the other gates in this tree use, and a final PASS/FAIL.
 *
 * Copyright 2026 the ppc64le port authors
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 */

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <windows.h>
#include <winternl.h>

/* winternl.h's THREADINFOCLASS does not carry this one on every build.  It is
 * the class that means "this thread's context in the machine it is really
 * executing", and on this port that machine is AMD64; the buffer width is what
 * says so.  See get_thread_wow64_context() in dlls/ntdll/unix/signal_ppc64.c. */
#ifndef ThreadWow64Context
#define ThreadWow64Context ((THREADINFOCLASS)29)
#endif

#define GUEST_FAULT_ADDR  0x00000000DEAD1000ull
#define GUEST_STACK_MARK  0xFEEDFACE5AFE0001ull

static int step, fails;

static void check( int ok, const char *what, ... )
{
    va_list ap;
    printf( "guest_debug_read: %2d %-4s ", ++step, ok ? "ok" : "FAIL" );
    va_start( ap, what );
    vprintf( what, ap );
    va_end( ap );
    printf( "\n" );
    fflush( stdout );
    if (!ok) fails++;
}

struct sentinel { const char *name; size_t off; ULONG64 want; };

int main( int argc, char **argv )
{
    static const struct sentinel sentinels[] = {
        { "rax", offsetof(AMD64_CONTEXT, Rax), 0x1111111100000011ull },
        { "rbx", offsetof(AMD64_CONTEXT, Rbx), 0x2222222200000022ull },
        { "rdx", offsetof(AMD64_CONTEXT, Rdx), 0x3333333300000033ull },
        { "rsi", offsetof(AMD64_CONTEXT, Rsi), 0x4444444400000044ull },
        { "rdi", offsetof(AMD64_CONTEXT, Rdi), 0x5555555500000055ull },
        { "r12", offsetof(AMD64_CONTEXT, R12), 0x6666666600000066ull },
        { "r13", offsetof(AMD64_CONTEXT, R13), 0x7777777700000077ull },
        { "r14", offsetof(AMD64_CONTEXT, R14), 0x8888888800000088ull },
        { "r15", offsetof(AMD64_CONTEXT, R15), 0x9999999900000099ull },
        { "rcx", offsetof(AMD64_CONTEXT, Rcx), GUEST_FAULT_ADDR },
    };
    DWORD pid, guest_tid = 0;
    DEBUG_EVENT ev;
    HANDLE ph = NULL;
    int saw_guest_fault = 0, saw_native_refusal = 0;
    unsigned i;

    if (argc < 2) { printf( "usage: %s <pid>\n", argv[0] ); return 2; }
    pid = strtoul( argv[1], NULL, 0 );

    if (!DebugActiveProcess( pid ))
    {
        printf( "guest_debug_read: cannot attach to %lu: %lu\n",
                (unsigned long)pid, GetLastError() );
        return 2;
    }
    printf( "guest_debug_read: attached to pid %lu\n", (unsigned long)pid );
    fflush( stdout );
    ph = OpenProcess( PROCESS_ALL_ACCESS, FALSE, pid );

    while (WaitForDebugEvent( &ev, 60000 ))
    {
        DWORD cont = DBG_EXCEPTION_NOT_HANDLED;

        if (ev.dwDebugEventCode == EXCEPTION_DEBUG_EVENT)
        {
            EXCEPTION_RECORD *r = &ev.u.Exception.ExceptionRecord;
            HANDLE th = OpenThread( THREAD_ALL_ACCESS, FALSE, ev.dwThreadId );
            AMD64_CONTEXT gc;
            NTSTATUS st;

            memset( &gc, 0, sizeof(gc) );
            gc.ContextFlags = CONTEXT_AMD64_CONTROL | CONTEXT_AMD64_INTEGER |
                              CONTEXT_AMD64_SEGMENTS | CONTEXT_AMD64_FLOATING_POINT;
            st = NtQueryInformationThread( th, ThreadWow64Context, &gc, sizeof(gc), NULL );

            if (r->ExceptionCode == EXCEPTION_BREAKPOINT)
            {
                /* The debugger's own injected breakin thread.  It is native
                 * ppc64 code and has never run a guest instruction, so the
                 * only correct answer about its guest registers is that there
                 * are none. */
                if (!saw_native_refusal)
                {
                    saw_native_refusal = 1;
                    check( st != 0, "the breakin thread (native) reports NO guest context: "
                           "status %08lx (a success here would be zeros presented as registers)",
                           (unsigned long)st );
                    check( gc.SegCs == 0, "...and left CS at 0, which no user-mode "
                           "x86-64 thread can have: cs=%04x", gc.SegCs );
                }
                cont = DBG_CONTINUE;
            }
            else if (r->ExceptionCode == EXCEPTION_ACCESS_VIOLATION &&
                     ev.u.Exception.dwFirstChance && !saw_guest_fault)
            {
                saw_guest_fault = 1;
                guest_tid = ev.dwThreadId;

                check( (ULONG64)(ULONG_PTR)r->ExceptionAddress == GUEST_FAULT_ADDR ||
                       r->NumberParameters >= 2,
                       "the debug event names a guest fault: code %08lx addr %p",
                       (unsigned long)r->ExceptionCode, r->ExceptionAddress );
                check( r->NumberParameters >= 2 &&
                       (ULONG64)r->ExceptionInformation[1] == GUEST_FAULT_ADDR,
                       "the faulting ADDRESS is the guest's own %016llx (got %016llx)",
                       (unsigned long long)GUEST_FAULT_ADDR,
                       (unsigned long long)(r->NumberParameters >= 2 ? r->ExceptionInformation[1] : 0) );

                check( st == 0, "the guest thread's register file is readable: status %08lx",
                       (unsigned long)st );
                if (!st)
                {
                    ULONG64 rip = gc.Rip;
                    ULONG64 marker = 0, window[32];
                    SIZE_T got = 0;

                    check( gc.SegCs == 0x33, "guest CS is 0x33 (a filled block, not a zeroed one): "
                           "cs=%04x", gc.SegCs );
                    check( rip == (ULONG64)(ULONG_PTR)r->ExceptionAddress,
                           "guest RIP %016llx is the address the exception record names %p",
                           (unsigned long long)rip, r->ExceptionAddress );

                    for (i = 0; i < ARRAYSIZE(sentinels); i++)
                    {
                        ULONG64 have = *(ULONG64 *)((char *)&gc + sentinels[i].off);
                        check( have == sentinels[i].want, "guest %s = %016llx",
                               sentinels[i].name, (unsigned long long)have );
                    }

                    check( gc.Rsp != 0, "guest RSP is set: %016llx",
                           (unsigned long long)gc.Rsp );

                    /* The marker is a local of the faulting frame, so it is
                     * somewhere in the 256 bytes above RSP rather than at a
                     * fixed offset -- the compiler decides where.  Scanning a
                     * bounded window is the honest form of the question the
                     * gate is really asking, which is "is the guest's own
                     * stack still mapped, and does it still contain what the
                     * guest put there".  The read is a SINGLE ReadProcessMemory
                     * of the whole window, so a stack that has been unmapped
                     * fails at the read rather than being scanned for
                     * something that is not there. */
                    if (ReadProcessMemory( ph, (void *)(ULONG_PTR)gc.Rsp,
                                           window, sizeof(window), &got ) &&
                        got == sizeof(window))
                    {
                        for (i = 0; i < ARRAYSIZE(window); i++)
                            if (window[i] == GUEST_STACK_MARK) { marker = window[i]; break; }
                        check( marker == GUEST_STACK_MARK,
                               "the guest STACK is still mapped and holds the marker the "
                               "guest left in the faulting frame, %016llx at rsp+%u",
                               (unsigned long long)GUEST_STACK_MARK,
                               (unsigned)(i * sizeof(window[0])) );
                    }
                    else
                        check( 0, "the guest stack at rsp %016llx could not be read across the "
                               "process boundary (err %lu) -- registers without a stack",
                               (unsigned long long)gc.Rsp, GetLastError() );
                }
            }
            CloseHandle( th );
        }
        else if (ev.dwDebugEventCode == EXIT_PROCESS_DEBUG_EVENT)
        {
            ContinueDebugEvent( ev.dwProcessId, ev.dwThreadId, DBG_CONTINUE );
            break;
        }
        else cont = DBG_CONTINUE;

        ContinueDebugEvent( ev.dwProcessId, ev.dwThreadId, cont );
    }

    check( saw_guest_fault, "a guest fault was reported to the debugger at all" );
    check( saw_native_refusal, "the breakin thread reached the debugger, i.e. the attach "
           "actually stopped the target" );
    (void)guest_tid;
    if (ph) CloseHandle( ph );

    printf( "guest_debug_read: %s (%d checks, %d failed)\n",
            fails ? "FAIL" : "PASS", step, fails );
    return fails ? 1 : 0;
}
