/*
 * thunk_cache -- the gate for the guest-to-native thunk target cache in
 * dlls/ntdll/signal_ppc64.c.
 *
 * WHAT IS UNDER TEST, AND WHY IT NEEDED A GATE OF ITS OWN.
 *
 * Every call a guest makes into native code traps, and find_guest_thunk_target()
 * turns the trapping address back into the native function it stands for.  That
 * used to be two name lookups (LdrGetDllHandle plus LdrGetProcedureAddress) or,
 * for a COM vtable slot, a walk of the whole loaded-module list -- on EVERY
 * call, under the PROCESS-WIDE LOADER LOCK.  It is now a direct-mapped cache
 * keyed by the trapping address and read with NO LOCK AT ALL, published with a
 * per-slot sequence number.
 *
 * Both of those are the kind of change that is invisible when it is wrong.  A
 * cache that returns the wrong entry does not crash: it calls a real function
 * with the arguments meant for a different one, and the process carries on with
 * a wrong number in it.  A seqlock with a missing fence is worse -- it is right
 * every time it is tested and wrong on the machine of whoever runs it next.  So
 * this probe checks VALUES, from SEVERAL THREADS, across BOTH kinds of crossing
 * the cache serves:
 *
 *   * FLAT IMPORTS -- kernel32 and user32 entry points whose answers are known
 *     at compile time.  Every one of them is a different native function, and
 *     they are called in an interleaved loop, so an entry that answered for the
 *     wrong address gives a wrong answer here rather than somewhere else later.
 *
 *   * COM VTABLE SLOTS -- IMalloc, obtained through CoGetMalloc.  A COM slot
 *     reaches native code by a different path inside find_guest_thunk_target
 *     (find_guest_com_target, and a different half of the cached entry), and
 *     the version of this cache that shipped first did not cache them at all.
 *     Alloc/GetSize/Free is three slots with a checkable relationship: the
 *     block GetSize reports has to be at least the block Alloc was asked for.
 *
 * The threads are the point of the multi-threaded part: one thread exercises
 * the cache, several exercise the PUBLICATION of it -- readers running with no
 * lock while another thread is inside thunk_rip_cache_put() filling a slot.
 *
 * NO CRT: this file's entry point IS the image entry point, the same discipline
 * as ppc64le/seh/guest_callbacks.c and for the same reason -- a CRT drags in
 * imports of its own, and this probe's whole claim is about which crossings it
 * makes and how many.
 *
 * Copyright 2026 the ppc64le port authors
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 */

#include <windows.h>
#include <objbase.h>
#include <oleauto.h>

#define TC_THREADS         4
#define TC_ITERATIONS      200

/* ------------------------------------------------------------- output */

static void out( const char *s )
{
    DWORD n = 0, written;

    while (s[n]) n++;
    WriteFile( GetStdHandle( STD_OUTPUT_HANDLE ), s, n, &written, NULL );
}

static void out_dec( ULONG v )
{
    char buf[12];
    int i = 11;

    buf[i] = 0;
    do { buf[--i] = '0' + (char)(v % 10); v /= 10; } while (v);
    out( buf + i );
}

/* ------------------------------------------------------- the check body
 *
 * ONE function, run by the main thread first and then by every worker, so the
 * warm-cache path and the racing-publication path execute exactly the same
 * checks.  -> the number of checks that came out WRONG, which a correct port
 * makes zero of on every thread and every iteration.
 *
 * Each check is a different native function with a different compile-time
 * answer.  That is what makes a mis-keyed cache visible: the entries collide
 * only if the cache hands one call site's resolution to another, and then the
 * answer this file compares against is the one that changes.
 */
static ULONG tc_checks( IMalloc *imalloc )
{
    static const char abc[] = "abc";
    static const char abd[] = "abd";
    ULONG bad = 0;
    char buf[8];
    void *block;
    SIZE_T size;

    /* kernel32, five distinct exports */
    if (lstrlenA( "thunk-cache" ) != 11) bad++;
    if (lstrcmpA( abc, abc ) != 0) bad++;
    if (lstrcmpA( abc, abd ) >= 0) bad++;
    if (MulDiv( 1000, 7, 5 ) != 1400) bad++;
    SetLastError( 0x00c0ffee );
    if (GetLastError() != 0x00c0ffee) bad++;

    /* user32, two more, chosen because they answer differently for two
     * arguments -- a function that ignored its argument would pass a
     * one-sided test */
    if (!IsCharAlphaA( 'Q' )) bad++;
    if (IsCharAlphaA( '7' )) bad++;

    buf[0] = 'a'; buf[1] = 'b'; buf[2] = 'c'; buf[3] = 0;
    CharUpperA( buf );
    if (buf[0] != 'A' || buf[1] != 'B' || buf[2] != 'C' || buf[3] != 0) bad++;

    /* A SUB-WORD ARGUMENT, whose upper bits are the caller's leftovers.
     *
     * MS-x64 requires only the low sixteen bits of a WORD argument to be
     * meaningful, and clang emits a sixteen-bit `mov cx, 0xffff` for one --
     * so RCX keeps whatever the previous call left above it.  The lstrlenA
     * immediately above is not decoration: it puts a pointer in RCX, whose
     * upper half is emphatically not zero, which is what makes the leftovers
     * observable here rather than accidentally zero.
     *
     * ALL_PROCESSOR_GROUPS is 0xffff, and the native implementation compares
     * the whole argument against it: with the upper bits still set the test
     * fails, control falls to the arm that reads the argument as a group
     * INDEX, and the call answers 0.  Measured before the fix: native
     * kernel32 saw group = 0xffbdffff.  So this checks the two calls against
     * EACH OTHER rather than against a constant -- the processor count is a
     * property of the machine and not of this file -- and against zero, which
     * is the specific wrong answer the bug produced. */
    if (lstrlenA( "a-pointer-lives-in-rcx-for-this-call" ) != 36) bad++;
    if (GetActiveProcessorGroupCount() == 1)
    {
        DWORD all = GetActiveProcessorCount( ALL_PROCESSOR_GROUPS );
        DWORD one = GetActiveProcessorCount( 0 );

        if (!all || all != one) bad++;
    }

    /* SIGNED sub-word arguments, which the width word alone gets wrong.
     *
     * ELFv2 makes extending a sub-word argument the CALLER's job AND ties the
     * kind of extension to the argument's type; MS-x64 leaves the upper bits
     * undefined and makes ignoring them the callee's job.  So the host has to
     * know not just how wide the guest's value is but whether it is signed --
     * and zero-extending everything, which is what this port did when the
     * width word was first added, turns a negative sub-word argument into a
     * large positive one.
     *
     * These two exports were chosen because their native implementations are
     * a single assignment (dlls/oleaut32/vartype.c: `*plOut = sIn;`), so the
     * answer is the argument and nothing else can explain a wrong one.
     *
     * VARIANT_TRUE IS -1, not 1 -- a VARIANT_BOOL is a SHORT and OLE's true
     * is all-bits-set.  Zero-extended it arrives as 65535, which is the exact
     * wrong answer this checks for rather than merely checking "not equal".
     * VarI4FromI2 does the same for an ordinary negative SHORT, so a host
     * that special-cased 0xFFFF would still be caught. */
    {
        LONG l = 0;

        if (VarI4FromBool( VARIANT_TRUE, &l ) != S_OK || l != -1) bad++;
        l = 0;
        if (VarI4FromI2( -12345, &l ) != S_OK || l != -12345) bad++;
    }

    /* COM vtable slots: three of them, with a relationship between the
     * answers rather than three independent constants */
    if (imalloc)
    {
        block = IMalloc_Alloc( imalloc, 1234 );
        if (!block) bad++;
        else
        {
            size = IMalloc_GetSize( imalloc, block );
            if (size < 1234) bad++;
            IMalloc_Free( imalloc, block );
        }
    }
    return bad;
}

/* --------------------------------------------------------- worker thread */

struct tc_worker
{
    IMalloc *imalloc;
    ULONG    bad;
    ULONG    iterations;
};

static DWORD WINAPI tc_thread( void *arg )
{
    struct tc_worker *w = arg;
    ULONG i;

    /* Each worker joins the MTA itself.  Sharing the main thread's IMalloc is
     * deliberate -- the task allocator is free-threaded and one interface
     * pointer called from four threads is exactly the concurrent COM-slot
     * traffic the cache has to survive. */
    CoInitializeEx( NULL, COINIT_MULTITHREADED );
    for (i = 0; i < w->iterations; i++) w->bad += tc_checks( w->imalloc );
    CoUninitialize();
    return 0;
}

/* ------------------------------------------------------------- the probe */

void thunk_cache_entry(void)
{
    struct tc_worker workers[TC_THREADS];
    HANDLE threads[TC_THREADS];
    IMalloc *imalloc = NULL;
    ULONG i, n, bad = 0, crossings, iterations = TC_ITERATIONS;
    char envbuf[16];
    HRESULT hr;

    /* The gate runs this probe twice with tracing on, and a +seh run prints a
     * line per crossing -- so the trace legs ask for far fewer iterations than
     * the value legs, which want the thread interleaving.  Read through
     * GetEnvironmentVariableA rather than getenv because there is no CRT here,
     * and echoed in the transcript below so the two legs' transcripts differ
     * visibly rather than silently. */
    n = GetEnvironmentVariableA( "TC_ITERATIONS", envbuf, sizeof(envbuf) );
    if (n && n < sizeof(envbuf))          /* n >= sizeof means it did not fit,
                                           * and envbuf was not written */
    {
        ULONG v = 0;
        const char *p = envbuf;

        while (*p >= '0' && *p <= '9') v = v * 10 + (ULONG)(*p++ - '0');
        if (v && !*p) iterations = v;
    }

    out( "thunk-cache: guest probe\n" );

    hr = CoInitializeEx( NULL, COINIT_MULTITHREADED );
    if (FAILED(hr)) { out( "com-init=no\n" ); ExitProcess( 2 ); }
    if (FAILED(CoGetMalloc( 1, &imalloc )) || !imalloc)
    {
        /* Named rather than tolerated: the COM half of this gate is half of
         * what it is for, and a run without it is not this gate passing. */
        out( "com-malloc=no\n" );
        ExitProcess( 2 );
    }
    out( "com-malloc=yes\n" );

    /* Warm the cache on this thread, and prove the answers are right BEFORE
     * any concurrency exists -- a failure here is a plain wrong-answer bug and
     * says so, rather than being read as a race. */
    bad += tc_checks( imalloc );
    out( "warm-pass=" ); out_dec( bad == 0 ); out( "\n" );

    for (i = 0; i < TC_THREADS; i++)
    {
        workers[i].imalloc    = imalloc;
        workers[i].bad        = 0;
        workers[i].iterations = iterations;
        threads[i] = CreateThread( NULL, 0, tc_thread, &workers[i], 0, NULL );
        if (!threads[i]) { out( "thread=no\n" ); ExitProcess( 2 ); }
    }
    for (i = 0; i < TC_THREADS; i++)
    {
        WaitForSingleObject( threads[i], INFINITE );
        bad += workers[i].bad;
        CloseHandle( threads[i] );
    }

    /* SEVENTEEN CROSSINGS per pass -- lstrlenA twice, lstrcmpA twice, MulDiv,
     * SetLastError, GetLastError, IsCharAlphaA twice, CharUpperA,
     * GetActiveProcessorGroupCount, GetActiveProcessorCount twice,
     * VarI4FromBool, VarI4FromI2, and IMalloc's Alloc/GetSize/Free -- of
     * which fourteen are value checks
     * (SetLastError and Free have no answer of their own; they set up and tear
     * down the two that do).  CROSSINGS rather than checks is the number
     * printed, because it is the number the gate's cache-hit floor is actually
     * about, and it is printed rather than left implicit so that floor comes
     * from this probe's own transcript instead of a constant in the script
     * that would rot the moment a call is added here. */
    crossings = 17 * (1 + TC_THREADS * iterations);
    out( "threads=" ); out_dec( TC_THREADS );
    out( " iterations=" ); out_dec( iterations );
    out( " crossings=" ); out_dec( crossings );
    out( " wrong=" ); out_dec( bad );
    out( "\n" );

    IMalloc_Release( imalloc );
    CoUninitialize();

    if (bad) { out( "FAIL\n" ); ExitProcess( 1 ); }
    out( "PASS\n" );
    ExitProcess( 0 );
}
