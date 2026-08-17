/*
 * com_apartment -- what apartment a GUEST thread is in.
 *
 * ONE source, built TWICE and run twice: as a native ppc64 Windows PE and as
 * an x86-64 guest PE.  The two runs must print byte-identical output, exactly
 * as com_smoke.c does and for the same reason -- every value below is one
 * Wine's own combase computed, so the guest agreeing with the native run
 * means the guest thread reached the same apartment bookkeeping through the
 * thunk boundary with nothing lost on the way.
 *
 * WHY THIS EXISTS.  A 300-second DOOM run on 2026-08-17 logged 891
 * "err:ole:com_get_class_object apartment not initialised" across 31 guest
 * worker threads: DOOM's job pool calls CoCreateInstance without ever calling
 * CoInitializeEx, and every one of those calls failed.  Windows and Wine both
 * grant such a thread the process MTA -- the "implicit MTA" -- but ONLY once
 * some thread has actually joined an MTA.  That is the rule this file pins,
 * from the guest side, in both directions:
 *
 *   an implicit MTA that is not granted when it is owed is the DOOM bug;
 *   an implicit MTA granted when it is NOT owed is a worse bug, because it
 *   would make a guest thread appear initialised to code that then marshals
 *   into an apartment nobody entered.
 *
 * The observable is CoGetApartmentType, which is Windows' own answer to "what
 * apartment am I in" and reports the implicit grant explicitly
 * (APTTYPE_MTA + APTTYPEQUALIFIER_IMPLICIT_MTA), rather than an HRESULT that
 * several unrelated failures share.  Each case then does a real
 * CoCreateInstance(CLSID_FileMoniker) on the worker and calls a method on
 * what comes back, so a case that claims S_OK has to produce a working
 * object, not just a hopeful return code.
 *
 * FIVE CASES, one per process, selected by the COM_APARTMENT_CASE
 * environment variable -- apartment state is process-global and sticky, so
 * they cannot share one:
 *
 *   noinit    nobody initialises.  Worker: CO_E_NOTINITIALIZED.  This is the
 *             floor: no MTA exists, so no thread may be granted one.
 *   mta       main thread CoInitializeEx(COINIT_MULTITHREADED).  Worker, with
 *             no CoInitializeEx of its own: APTTYPE_MTA/IMPLICIT_MTA and a
 *             working object.  THIS is the grant DOOM's workers need.
 *   sta       main thread CoInitializeEx(COINIT_APARTMENTTHREADED) -- DOOM's
 *             actual sequence, measured in the trap trace.  Worker:
 *             CO_E_NOTINITIALIZED, because an STA is not an MTA and no MTA
 *             exists.  This case REPRODUCES the 891-error class, and it is
 *             not a defect: Windows answers the same (dlls/ole32/tests/
 *             compobj.c test_CoCreateInstance, dlls/combase/tests/roapi.c
 *             test_implicit_mta).
 *   stamta    main thread CoInitializeEx(COINIT_APARTMENTTHREADED) and then
 *             CoIncrementMTAUsage -- the documented way a process with an STA
 *             main thread makes the MTA exist for everyone else.  Worker:
 *             granted, as in `mta`.  This is what a native Wine component
 *             (dsound.c, rtworkq/queue.c) does on Windows and Proton, and it
 *             is why the same title logs nothing there.
 *   joiner    main thread CoInitializeEx(COINIT_APARTMENTTHREADED), and a
 *             SEPARATE thread calls CoInitializeEx(COINIT_MULTITHREADED) and
 *             stays alive -- the shape a subsystem with its own audio or
 *             work-queue thread produces.  The worker is granted WHILE that
 *             thread lives and refused again after it exits, because the MTA
 *             is reference-counted and dies with its last member.  That
 *             second half is the reason rtworkq holds a
 *             CoIncrementMTAUsage cookie rather than leaning on whichever
 *             thread happened to initialise first, and it is the trap
 *             waiting for anyone who implements this for the audio lane.
 *
 * NO C RUNTIME on the guest side (-DCOM_APARTMENT_NO_CRT): the program
 * formats its own output and writes it with WriteFile, so the only thing
 * under test is the boundary.  The case comes from the environment rather
 * than argv for the same reason -- there is no CRT to parse a command line.
 *
 * Copyright 2026 the ppc64le port authors
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 */

#define COBJMACROS

#include <windows.h>
#include <objbase.h>

/* Spelled out rather than linked from libuuid: the guest build has no Wine
 * import libraries at all, and a GUID both builds compile from the same bytes
 * cannot differ between them. */
static const GUID apt_CLSID_FileMoniker =
    { 0x00000303, 0x0000, 0x0000, { 0xc0,0x00,0x00,0x00,0x00,0x00,0x00,0x46 } };
static const GUID apt_IID_IMoniker =
    { 0x0000000f, 0x0000, 0x0000, { 0xc0,0x00,0x00,0x00,0x00,0x00,0x00,0x46 } };

/* ------------------------------------------------------------- output */

static void out( const char *s )
{
    DWORD n = 0, written;

    while (s[n]) n++;
    WriteFile( GetStdHandle( STD_OUTPUT_HANDLE ), s, n, &written, NULL );
}

static void out_hex( ULONG v, int digits )
{
    static const char hex[] = "0123456789ABCDEF";
    char buf[9];
    int i;

    for (i = 0; i < digits; i++) buf[digits - 1 - i] = hex[(v >> (4 * i)) & 0xf];
    buf[digits] = 0;
    out( buf );
}

static void out_dec( LONG v )
{
    char buf[13];
    ULONG u;
    int i = 12;

    buf[i] = 0;
    if (v < 0) u = (ULONG)-v; else u = (ULONG)v;
    do { buf[--i] = '0' + (char)(u % 10); u /= 10; } while (u);
    if (v < 0) buf[--i] = '-';
    out( buf + i );
}

static void out_hr( const char *label, HRESULT hr )
{
    out( label );
    out( "=0x" );
    out_hex( (ULONG)hr, 8 );
}

/* Named rather than numbered: a diff between the native and the guest run
 * should say which apartment, not which integer. */
static void out_apttype( APTTYPE t )
{
    switch (t)
    {
    case APTTYPE_CURRENT: out( "APTTYPE_CURRENT" ); return;
    case APTTYPE_STA:     out( "APTTYPE_STA" );     return;
    case APTTYPE_MTA:     out( "APTTYPE_MTA" );     return;
    case APTTYPE_NA:      out( "APTTYPE_NA" );      return;
    case APTTYPE_MAINSTA: out( "APTTYPE_MAINSTA" ); return;
    default:              out( "APTTYPE_?" ); out_dec( (LONG)t ); return;
    }
}

static void out_aptqual( APTTYPEQUALIFIER q )
{
    switch (q)
    {
    case APTTYPEQUALIFIER_NONE:                out( "NONE" );                return;
    case APTTYPEQUALIFIER_IMPLICIT_MTA:        out( "IMPLICIT_MTA" );        return;
    case APTTYPEQUALIFIER_NA_ON_MTA:           out( "NA_ON_MTA" );           return;
    case APTTYPEQUALIFIER_NA_ON_STA:           out( "NA_ON_STA" );           return;
    case APTTYPEQUALIFIER_NA_ON_IMPLICIT_MTA:  out( "NA_ON_IMPLICIT_MTA" );  return;
    case APTTYPEQUALIFIER_NA_ON_MAINSTA:       out( "NA_ON_MAINSTA" );       return;
    default:                                   out( "QUAL_?" ); out_dec( (LONG)q ); return;
    }
}

/* ------------------------------------------------------------- the run */

static int failures;
static int step;

static void begin( const char *what )
{
    out( "step " );
    out_dec( ++step );
    out( " " );
    out( what );
    out( ": " );
}

static void verdict( BOOL ok, const char *why )
{
    if (ok) out( " ok\n" );
    else
    {
        failures++;
        out( " FAIL (" );
        out( why );
        out( ")\n" );
    }
}

/* What one thread sees.  Filled in on the thread, printed by main, so that
 * two threads cannot interleave the output of a test whose whole value is
 * that the bytes match another run of it. */
struct probe
{
    HRESULT           apt_hr;
    APTTYPE           apt_type;
    APTTYPEQUALIFIER  apt_qual;
    HRESULT           create_hr;
    BOOL              got_iface;
    HRESULT           method_hr;
    DWORD             mksys;
};

/* 0x7f/0xff sentinels: a field combase never wrote must not read back as a
 * value the expectations happen to accept. */
static void probe_init( struct probe *p )
{
    p->apt_hr    = (HRESULT)0x7f7f7f7f;
    p->apt_type  = (APTTYPE)0x7f;
    p->apt_qual  = (APTTYPEQUALIFIER)0x7f;
    p->create_hr = (HRESULT)0x7f7f7f7f;
    p->got_iface = FALSE;
    p->method_hr = (HRESULT)0x7f7f7f7f;
    p->mksys     = 0xffffffff;
}

static void probe_run( struct probe *p )
{
    IMoniker *mk = NULL;

    p->apt_hr = CoGetApartmentType( &p->apt_type, &p->apt_qual );

    p->create_hr = CoCreateInstance( &apt_CLSID_FileMoniker, NULL,
                                     CLSCTX_INPROC_SERVER, &apt_IID_IMoniker,
                                     (void **)&mk );
    p->got_iface = (mk != NULL);
    if (mk)
    {
        /* A returned pointer proves nothing until a method on it answers with
         * a value only the real implementation knows. */
        p->method_hr = IMoniker_IsSystemMoniker( mk, &p->mksys );
        IMoniker_Release( mk );
    }
}

static void probe_report( const char *who, const struct probe *p )
{
    out( "  " );
    out( who );
    out( ": " );
    out_hr( "aptype_hr", p->apt_hr );
    out( " type=" );
    out_apttype( p->apt_type );
    out( " qual=" );
    out_aptqual( p->apt_qual );
    out( "\n  " );
    out( who );
    out( ": " );
    out_hr( "cocreate_hr", p->create_hr );
    out( " iface=" );
    out( p->got_iface ? "yes" : "no" );
    out( " " );
    out_hr( "method_hr", p->method_hr );
    out( " mksys=" );
    out_dec( (LONG)p->mksys );
    out( "\n" );
}

/* Expectation, spelled as the thing being asserted rather than as a bag of
 * comparisons, so a red line names what was owed. */
static BOOL probe_is_granted( const struct probe *p )
{
    return p->apt_hr == S_OK
        && p->apt_type == APTTYPE_MTA
        && p->apt_qual == APTTYPEQUALIFIER_IMPLICIT_MTA
        && p->create_hr == S_OK
        && p->got_iface
        && p->method_hr == S_OK
        && p->mksys == MKSYS_FILEMONIKER;
}

static BOOL probe_is_refused( const struct probe *p )
{
    return p->apt_hr == CO_E_NOTINITIALIZED
        && p->apt_type == APTTYPE_CURRENT
        && p->apt_qual == APTTYPEQUALIFIER_NONE
        && p->create_hr == CO_E_NOTINITIALIZED
        && !p->got_iface;
}

static struct probe worker, worker2;

static DWORD WINAPI worker_start( void *arg )
{
    /* Deliberately no CoInitializeEx here.  That is the whole point: this is
     * the shape of DOOM's job-pool threads. */
    probe_run( (struct probe *)arg );
    return 0;
}

/* Runs the worker probe on a FRESH thread every time.  Fresh matters: a
 * thread that has once been granted the implicit MTA holds a reference to it,
 * so re-probing on the same thread would measure that reference rather than
 * the process state. */
static BOOL run_worker( struct probe *p )
{
    HANDLE thread;

    probe_init( p );
    thread = CreateThread( NULL, 0, worker_start, p, 0, NULL );
    if (!thread) return FALSE;
    WaitForSingleObject( thread, INFINITE );
    CloseHandle( thread );
    return TRUE;
}

/* The `joiner` case's MTA member: joins, says so, and stays in the apartment
 * until told to leave.  A thread that joined and returned would take the MTA
 * with it, which is the second half of what this case measures. */
static HANDLE joiner_ready, joiner_release;
static HRESULT joiner_hr;

static DWORD WINAPI joiner_start( void *arg )
{
    joiner_hr = CoInitializeEx( NULL, COINIT_MULTITHREADED );
    SetEvent( joiner_ready );
    WaitForSingleObject( joiner_release, INFINITE );
    if (SUCCEEDED(joiner_hr)) CoUninitialize();
    return 0;
}

/* ------------------------------------------------------------- the cases */

enum apt_case { CASE_NOINIT, CASE_MTA, CASE_STA, CASE_STAMTA, CASE_JOINER,
                CASE_BAD };

/* Hand-rolled rather than lstrcmpA: every import this file does not take is
 * one fewer thunk between the two builds that could differ. */
static BOOL str_eq( const char *a, const char *b )
{
    while (*a && *a == *b) { a++; b++; }
    return *a == *b;
}

static enum apt_case read_case( char *name, DWORD size )
{
    DWORD n = GetEnvironmentVariableA( "COM_APARTMENT_CASE", name, size );

    if (!n || n >= size)
    {
        name[0] = 0;
        return CASE_BAD;
    }
    if (str_eq( name, "noinit" )) return CASE_NOINIT;
    if (str_eq( name, "mta" ))    return CASE_MTA;
    if (str_eq( name, "sta" ))    return CASE_STA;
    if (str_eq( name, "stamta" )) return CASE_STAMTA;
    if (str_eq( name, "joiner" )) return CASE_JOINER;
    return CASE_BAD;
}

static int com_apartment_run( void )
{
    CO_MTA_USAGE_COOKIE cookie = NULL;
    struct probe main_probe;
    HRESULT hr = S_OK;
    char name[32];
    enum apt_case c;
    HANDLE joiner = NULL;

    c = read_case( name, sizeof(name) );
    out( "com_apartment: case " );
    out( name[0] ? name : "(unset)" );
    out( "\n" );
    if (c == CASE_BAD)
    {
        out( "com_apartment: FAIL 0/0 (set COM_APARTMENT_CASE to one of "
             "noinit, mta, sta, stamta, joiner)\n" );
        return 2;
    }

    probe_init( &main_probe );

    /* ---- what the main thread does, which is all that differs ---------- */
    switch (c)
    {
    case CASE_NOINIT:
        begin( "main thread does not initialise COM" );
        out( "skipped" );
        verdict( TRUE, "" );
        break;
    case CASE_MTA:
        begin( "main CoInitializeEx(COINIT_MULTITHREADED)" );
        hr = CoInitializeEx( NULL, COINIT_MULTITHREADED );
        out_hr( "hr", hr );
        verdict( hr == S_OK, "not S_OK" );
        break;
    case CASE_STA:
    case CASE_STAMTA:
    case CASE_JOINER:
        begin( "main CoInitializeEx(COINIT_APARTMENTTHREADED)" );
        hr = CoInitializeEx( NULL, COINIT_APARTMENTTHREADED );
        out_hr( "hr", hr );
        verdict( hr == S_OK, "not S_OK" );
        break;
    default:
        break;
    }
    if (FAILED(hr)) goto done;

    if (c == CASE_STAMTA)
    {
        begin( "main CoIncrementMTAUsage" );
        hr = CoIncrementMTAUsage( &cookie );
        out_hr( "hr", hr );
        out( " cookie=" );
        out( cookie ? "yes" : "no" );
        verdict( hr == S_OK && cookie != NULL, "no MTA usage cookie" );
        if (FAILED(hr)) goto uninit;
    }

    /* ---- the main thread's own view ----------------------------------- */
    begin( "main thread apartment + CoCreateInstance" );
    probe_run( &main_probe );
    out( "\n" );
    probe_report( "main", &main_probe );
    switch (c)
    {
    case CASE_NOINIT:
        verdict( probe_is_refused( &main_probe ),
                 "an uninitialised main thread was not refused" );
        break;
    case CASE_MTA:
        verdict( main_probe.apt_hr == S_OK
                 && main_probe.apt_type == APTTYPE_MTA
                 && main_probe.apt_qual == APTTYPEQUALIFIER_NONE
                 && main_probe.create_hr == S_OK
                 && main_probe.method_hr == S_OK
                 && main_probe.mksys == MKSYS_FILEMONIKER,
                 "the thread that joined the MTA is not in it" );
        break;
    case CASE_STA:
    case CASE_STAMTA:
    case CASE_JOINER:
        /* The first STA in a process is the main STA -- combase marks it, and
         * CoGetApartmentType says so.  CoIncrementMTAUsage does NOT move this
         * thread: it already has an apartment. */
        verdict( main_probe.apt_hr == S_OK
                 && main_probe.apt_type == APTTYPE_MAINSTA
                 && main_probe.apt_qual == APTTYPEQUALIFIER_NONE
                 && main_probe.create_hr == S_OK
                 && main_probe.method_hr == S_OK
                 && main_probe.mksys == MKSYS_FILEMONIKER,
                 "the first STA thread is not the main STA" );
        break;
    default:
        break;
    }

    /* ---- a thread that joins the MTA and stays there ------------------- */
    if (c == CASE_JOINER)
    {
        begin( "a second thread CoInitializeEx(COINIT_MULTITHREADED)" );
        joiner_ready = CreateEventW( NULL, FALSE, FALSE, NULL );
        joiner_release = CreateEventW( NULL, FALSE, FALSE, NULL );
        joiner = (joiner_ready && joiner_release)
            ? CreateThread( NULL, 0, joiner_start, NULL, 0, NULL ) : NULL;
        if (!joiner)
        {
            out( "CreateThread/CreateEvent failed, err=" );
            out_dec( (LONG)GetLastError() );
            verdict( FALSE, "no joiner thread" );
            goto uninit;
        }
        WaitForSingleObject( joiner_ready, INFINITE );
        out_hr( "hr", joiner_hr );
        verdict( joiner_hr == S_OK,
                 "a thread with no apartment of its own could not join the MTA" );
    }

    /* ---- the worker, which never calls CoInitializeEx ------------------ */
    begin( "worker thread (no CoInitializeEx) apartment + CoCreateInstance" );
    if (!run_worker( &worker ))
    {
        out( "CreateThread failed, err=" );
        out_dec( (LONG)GetLastError() );
        verdict( FALSE, "no worker thread" );
        goto uninit;
    }
    out( "\n" );
    probe_report( "worker", &worker );

    if (c == CASE_MTA || c == CASE_STAMTA || c == CASE_JOINER)
        verdict( probe_is_granted( &worker ),
                 "an MTA exists, so this thread was owed the implicit MTA" );
    else
        verdict( probe_is_refused( &worker ),
                 "no MTA exists, so this thread must NOT have been granted one" );

    /* ---- and what happens when the MTA's only member leaves ------------ */
    if (c == CASE_JOINER)
    {
        begin( "the joiner leaves the MTA, then a fresh worker probes again" );
        SetEvent( joiner_release );
        WaitForSingleObject( joiner, INFINITE );
        CloseHandle( joiner );
        joiner = NULL;
        if (!run_worker( &worker2 ))
        {
            out( "CreateThread failed, err=" );
            out_dec( (LONG)GetLastError() );
            verdict( FALSE, "no worker thread" );
            goto uninit;
        }
        out( "\n" );
        probe_report( "worker2", &worker2 );
        verdict( probe_is_refused( &worker2 ),
                 "the MTA outlived its last member -- an implicit grant from a "
                 "dead apartment is worse than no grant at all" );
    }

uninit:
    if (joiner)
    {
        SetEvent( joiner_release );
        WaitForSingleObject( joiner, INFINITE );
        CloseHandle( joiner );
    }
    if (joiner_ready) CloseHandle( joiner_ready );
    if (joiner_release) CloseHandle( joiner_release );
    if (cookie)
    {
        begin( "CoDecrementMTAUsage" );
        CoDecrementMTAUsage( cookie );
        out( "returned" );
        verdict( TRUE, "" );
    }
    if (c != CASE_NOINIT)
    {
        begin( "CoUninitialize" );
        CoUninitialize();
        out( "returned" );
        verdict( TRUE, "" );
    }

done:
    out( failures ? "com_apartment: FAIL " : "com_apartment: PASS " );
    out_dec( (LONG)(step - failures) );
    out( "/" );
    out_dec( (LONG)step );
    out( "\n" );
    return failures ? 1 : 0;
}

#ifdef COM_APARTMENT_NO_CRT
/* The guest build has no C runtime: this IS the image entry point. */
void WINAPI com_apartment_entry( void )
{
    ExitProcess( (UINT)com_apartment_run() );
}
#else
int main( void )
{
    return com_apartment_run();
}
#endif
