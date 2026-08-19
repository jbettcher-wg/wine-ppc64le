/*
 * A guest thread with a big stack frame, which is what DOOM dies in.
 *
 * Copyright 2026 the ppc64le port authors
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * ---------------------------------------------------------------------------
 *
 * [MEASURED] 2026-08-18, DOOM (2016) on the test machine: 120 access violations in a
 * single run, all at ONE address, whose function begins
 *
 *     pushq %rbx
 *     movl  $0x4480, %eax        ; 17,536 bytes of stack frame
 *     callq __chkstk             ; reads gs:0x10, probes down a page at a time
 *     subq  %rax, %rsp
 *
 * and one more at a second site whose prologue is a plain `movq %rbx,
 * 0x8(%rsp)`.  Both are STACK WRITES.  The same run also produced
 * `err:seh:call_seh_handlers invalid frame ... Exception frame is not in
 * stack limits`, which is a check against the SAME TEB fields __chkstk reads.
 *
 * This probe is that shape and nothing else, so a failure here is a fact
 * about the port rather than about a 60 MB game: report the stack bounds the
 * guest is given, then take a 17,536-byte frame and touch every page of it,
 * then grow the stack far past its initial commit, on the initial thread and
 * on a worker that asked for 8 MiB the way DOOM's do.
 *
 * It judges nothing about absolute addresses -- those are ASLR'd -- only the
 * relationships: that the frame fits between the reported bounds, that
 * touching it does not fault, and that the limit moves down when the stack
 * grows.
 */

#include <windows.h>
#include <winternl.h>
#include <stdlib.h>

#define FRAME_BYTES 0x4480      /* exactly DOOM's */

/* THE STACK PROBE ITSELF, written out rather than linked, and that is the
 * point of the probe rather than an accident of building with -nostdlib.
 *
 * A frame bigger than a page cannot simply move RSP: the pages between the old
 * and new tops may not be committed, and on Windows the one immediately below
 * the committed region is a GUARD page whose job is to fault once so the
 * kernel can commit the next one.  So the compiler emits a call that walks
 * down a page at a time touching each one.  MSVC calls it __chkstk; clang's
 * mingw target calls it ___chkstk_ms and expects the runtime to supply it.
 *
 * Supplying it here means the loop under test is the same shape as the one in
 * DOOM's binary -- `orq $0, (%rcx)` per page, a read-modify-write that faults
 * on a page which is not there -- rather than whatever a toolchain happened to
 * link in.  The size arrives in RAX and RSP is the caller's to adjust.
 */
__asm__(
    ".text\n"
    ".globl ___chkstk_ms\n"
    "___chkstk_ms:\n"
    "    pushq %rcx\n"
    "    pushq %rax\n"
    "    cmpq  $0x1000, %rax\n"
    "    leaq  24(%rsp), %rcx\n"
    "    jb    2f\n"
    "1:  subq  $0x1000, %rcx\n"
    "    orq   $0, (%rcx)\n"
    "    subq  $0x1000, %rax\n"
    "    cmpq  $0x1000, %rax\n"
    "    ja    1b\n"
    "2:  subq  %rax, %rcx\n"
    "    orq   $0, (%rcx)\n"
    "    popq  %rax\n"
    "    popq  %rcx\n"
    "    ret\n"
);

static void out( const char *s )
{
    DWORD n = 0, len = 0;

    while (s[len]) len++;
    WriteFile( GetStdHandle( STD_OUTPUT_HANDLE ), s, len, &n, NULL );
}

static void out_hex( const char *label, ULONG64 v )
{
    static const char digits[] = "0123456789ABCDEF";
    char buf[96];
    int i = 0, j;

    while (*label) buf[i++] = *label++;
    buf[i++] = '0'; buf[i++] = 'x';
    for (j = 60; j >= 0; j -= 4) buf[i++] = digits[(v >> j) & 0xf];
    buf[i++] = '\n'; buf[i] = 0;
    out( buf );
}

static void out_dec( const char *label, ULONG64 v )
{
    char buf[96], d[24];
    int i = 0, j, n = 0;

    while (*label) buf[i++] = *label++;
    if (!v) d[n++] = '0';
    while (v) { d[n++] = (char)('0' + (v % 10)); v /= 10; }
    for (j = n - 1; j >= 0; j--) buf[i++] = d[j];
    buf[i++] = '\n'; buf[i] = 0;
    out( buf );
}

static void report_bounds( const char *who )
{
    TEB *teb = NtCurrentTeb();
    NT_TIB *tib = (NT_TIB *)teb;

    out( who );
    out_hex( "  StackBase        = ", (ULONG64)(ULONG_PTR)tib->StackBase );
    out_hex( "  StackLimit       = ", (ULONG64)(ULONG_PTR)tib->StackLimit );
    out_hex( "  DeallocationStack= ", (ULONG64)(ULONG_PTR)teb->DeallocationStack );
    out_dec( "  committed bytes  = ",
             (ULONG64)((char *)tib->StackBase - (char *)tib->StackLimit) );
    out_dec( "  reserved bytes   = ",
             (ULONG64)((char *)tib->StackBase - (char *)teb->DeallocationStack) );
}

static ULONG64 big_frame( ULONG64 seed );

/* DOOM'S ACTUAL SEQUENCE, which is the thing worth testing.
 *
 * The faulting function does not merely have a big frame; it is reached from
 * code that has just been across the boundary.  This port keeps ONE TEB for
 * two stacks -- the native ppc64 one and the guest one -- and rewrites
 * NtTib.StackBase/StackLimit to describe whichever machine is running (see
 * dlls/ntdll/unix/loader.c).  __chkstk reads those same fields.  So the
 * interesting moment is the first big frame taken AFTER returning from a
 * native call, when the fields have just been switched back.
 *
 * A plain overflow (c00000fd) means the probe walked off a stack the port
 * knows about, which is ordinary.  An ACCESS VIOLATION (c0000005), which is
 * what DOOM gets, means it touched an address the port does not consider part
 * of any stack -- the signature of the bounds describing the wrong one. */
static ULONG64 after_boundary_crossing( int iterations )
{
    NT_TIB *tib = (NT_TIB *)NtCurrentTeb();
    ULONG64 sum = 0;
    int i;

    for (i = 0; i < iterations; i++)
    {
        /* Across the boundary and back: an ordinary thunk call into native
         * kernel32, of the kind DOOM makes constantly. */
        DWORD tick = GetTickCount();
        void *limit = tib->StackLimit;

        /* ...and immediately a frame far bigger than a page, so __chkstk runs
         * against whatever the TEB says right now. */
        sum += big_frame( tick );
        if (tib->StackLimit != limit) out( "  NOTE: StackLimit moved across the call\n" );
    }
    return sum;
}

/* The DOOM shape: a frame bigger than a page, every page of it touched.
 * `volatile` and the checksum keep the compiler from eliding the array. */
static ULONG64 big_frame( ULONG64 seed )
{
    volatile char frame[FRAME_BYTES];
    ULONG64 sum = 0;
    int i;

    for (i = 0; i < FRAME_BYTES; i += 0x400) frame[i] = (char)(seed + i);
    for (i = 0; i < FRAME_BYTES; i += 0x400) sum += (unsigned char)frame[i];
    return sum;
}

/* Grow the stack well past any initial commit, one big frame per level. */
static ULONG64 deep( int depth, ULONG64 seed )
{
    volatile char frame[FRAME_BYTES];
    int i;

    for (i = 0; i < FRAME_BYTES; i += 0x400) frame[i] = (char)(depth + i);
    if (depth <= 0) return seed + (unsigned char)frame[0];
    return deep( depth - 1, seed ) + (unsigned char)frame[0];
}

static ULONG64 exercise( const char *who, int levels )
{
    NT_TIB *tib = (NT_TIB *)NtCurrentTeb();
    void *limit_before, *limit_after;
    ULONG64 sum;

    report_bounds( who );

    sum = big_frame( 1 );
    out( "  one 17536-byte frame: touched every page ok\n" );

    sum += after_boundary_crossing( 200 );
    out( "  200 x (native call, then a 17536-byte frame): ok\n" );

    limit_before = tib->StackLimit;
    sum += deep( levels, 0 );
    limit_after = tib->StackLimit;

    out_dec( "  recursion levels = ", (ULONG64)levels );
    out_hex( "  StackLimit after = ", (ULONG64)(ULONG_PTR)limit_after );
    out_dec( "  stack grew bytes = ",
             (ULONG64)((char *)limit_before - (char *)limit_after) );
    return sum;
}

/* A GUEST CALLBACK WITH A BIG FRAME, entered from NATIVE code.
 *
 * The shapes above all run guest code that the guest itself called.  DOOM's
 * 120 identical faults look instead like one function reached over and over,
 * and the way native code reaches guest code here is a TRAMPOLINE: the port
 * intercepts a callback at REGISTRATION and hands native code a stub, which
 * funnels into the shared run-entry primitive (dlls/ntdll/signal_ppc64.c).
 *
 * That path enters guest code on a thread that was running native code a
 * moment earlier, so it is the one place where "which stack does the TEB
 * describe" is decided by the port rather than by the guest.  qsort is the
 * cheapest way to be called back: native msvcrt sorts, the comparator is
 * ours, and it takes a frame far bigger than a page. */
static ULONG64 cb_sum;

static int __cdecl big_frame_compare( const void *a, const void *b )
{
    volatile char frame[FRAME_BYTES];
    int i;

    for (i = 0; i < FRAME_BYTES; i += 0x400) frame[i] = (char)i;
    for (i = 0; i < FRAME_BYTES; i += 0x400) cb_sum += (unsigned char)frame[i];
    return (*(const int *)a > *(const int *)b) - (*(const int *)a < *(const int *)b);
}

static void callback_with_big_frame(void)
{
    static int values[64];
    int i;

    for (i = 0; i < 64; i++) values[i] = 64 - i;
    out( "  native msvcrt calling a guest comparator with a 17536-byte frame\n" );
    qsort( values, 64, sizeof(values[0]), big_frame_compare );
    if (values[0] == 1 && values[63] == 64) out( "  callback frames: sorted ok\n" );
    else out( "  callback frames: WRONG ORDER\n" );
}

/* AN EXCEPTION RAISED INSIDE A NATIVE->GUEST CALLBACK.
 *
 * DOOM's crash reports prove its own handler ran (dbghelp symbolising), and
 * the port logged `invalid frame ... (bounds)` while dispatching -- the
 * establisher frame was on a NATIVE stack while the TEB described the GUEST
 * one.  dlls/ntdll/signal_ppc64.c's emu_trap_dispatch banner records the port
 * hitting that same message once before, from Win32 code running on the
 * kernel stack, so a remaining path of the same kind is the hypothesis.
 *
 * This is the nastiest ordinary shape: native code calls a guest callback,
 * and the guest raises and handles an exception inside it -- so the dispatcher
 * runs with a native frame below it and a guest frame above. */
static LONG CALLBACK veh( EXCEPTION_POINTERS *info )
{
    /* Only ONE question is asked here: was a guest vectored handler reached
     * at all when the exception was raised inside a native-called callback?
     * A VEH cannot "handle" a RaiseException -- its only returns are continue
     * -execution and continue-search -- so this reports and stands aside.  The
     * process is expected to die afterwards; whether this line appears first
     * is the result. */
    if (info->ExceptionRecord->ExceptionCode == 0xE0001234)
        out( "  VEH ENTERED: the guest handler was reached from the callback\n" );
    return EXCEPTION_CONTINUE_SEARCH;
}

static int __cdecl raising_compare( const void *a, const void *b )
{
    volatile char frame[FRAME_BYTES];
    int i;

    for (i = 0; i < FRAME_BYTES; i += 0x400) frame[i] = (char)i;

    /* Raise and let the vectored handler take it.  If the dispatcher walks a
     * native frame against guest bounds, this is where it says so. */
    RaiseException( 0xE0001234, 0, 0, NULL );

    cb_sum += (unsigned char)frame[0];
    return (*(const int *)a > *(const int *)b) - (*(const int *)a < *(const int *)b);
}

static void exception_in_callback(void)
{
    static int values[16];
    void *h;
    int i;

    for (i = 0; i < 16; i++) values[i] = 16 - i;
    h = AddVectoredExceptionHandler( 1, veh );
    if (!h) { out( "  AddVectoredExceptionHandler failed\n" ); return; }
    out( "  raising an exception inside a native-called guest callback\n" );
    qsort( values, 16, sizeof(values[0]), raising_compare );
    RemoveVectoredExceptionHandler( h );
    out( "  exception in callback: survived\n" );
}

static DWORD WINAPI worker( void *arg )
{
    return (DWORD)exercise( "worker thread (asked for 8 MiB):\n", (int)(ULONG_PTR)arg );
}

void bigframe_entry(void)
{
    HANDLE h;
    DWORD rc = 1;
    ULONG64 sum;

    /* 300 levels x 17,536 bytes is about 5 MiB -- past any one-megabyte
     * initial commit, and past the 2 MiB a PE typically reserves, so a stack
     * that cannot grow fails here rather than passing by being big enough. */
    /* Modest on the initial thread: its stack is the image's, and this probe
     * links with the default reserve, so a deep recursion here would overflow
     * legitimately and prove nothing.  The worker is the DOOM shape -- it asks
     * for 8 MiB, so 200 levels (about 3.5 MiB) must fit if the request was
     * honoured and the stack can grow into it. */
    sum = exercise( "initial thread:\n", 20 );

    callback_with_big_frame();

    /* Opt-in, because it ENDS THE PROCESS by design: a vectored handler cannot
     * consume a RaiseException, so the exception goes unhandled once the
     * handler has reported being reached.  Set BIGFRAME_RAISE=1 to run it. */
    {
        char buf[8];
        if (GetEnvironmentVariableA( "BIGFRAME_RAISE", buf, sizeof(buf) ) && buf[0] == '1')
            exception_in_callback();
    }

    h = CreateThread( NULL, 8 * 1024 * 1024, worker, (void *)(ULONG_PTR)200, 0, NULL );
    if (!h)
    {
        out( "bigframe: CreateThread failed\n" );
        ExitProcess( 2 );
    }
    if (WaitForSingleObject( h, 60000 ) != WAIT_OBJECT_0)
    {
        out( "bigframe: worker did not finish\n" );
        ExitProcess( 3 );
    }
    GetExitCodeThread( h, &rc );
    CloseHandle( h );

    if (sum) out( "bigframe: PASS\n" );
    ExitProcess( 0 );
}
