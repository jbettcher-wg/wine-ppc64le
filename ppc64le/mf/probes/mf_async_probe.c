/*
 * mf_async_probe -- the REVERSE-PROXY POSITIVE CONTROL for Media Foundation.
 *
 * GUEST ONLY, deliberately: there is nothing here a native ppc64 build could
 * be compared against, because what it measures is a boundary that does not
 * exist in a native run.
 *
 * Media Foundation's async model is IMFAsyncCallback -- an interface the
 * APPLICATION implements and MF invokes from a work-queue thread it owns.  On
 * this port that means native ppc64 code calling an x86-64 vtable, which
 * needs a REVERSE PROXY: a native vtable (libs/winecom/reverse.c) whose slots
 * marshal ELFv2 arguments into MS-x64 and enter the guest method through the
 * emulator.  That machinery now exists and dlls/mfplat/mfcom.c turns it on
 * for this surface (WINECOM_SF_REVERSE), so this file no longer asserts a
 * refusal -- it proves the round trip WORKS, with every value checked:
 *
 *   1  it builds a real IMFAsyncCallback in its own image -- an x86-64 vtable
 *      at a guest address -- and a second tiny guest-implemented IUnknown to
 *      use as MFPutWorkItem's `state` argument, so the state round trip has
 *      an unambiguous object to identify;
 *   2  MFPutWorkItem must return S_OK, and the callback's Invoke must
 *      actually run, signalled through an event rather than a hang: a
 *      timeout is a FAIL, never a wait forever;
 *   3  Invoke must receive a non-NULL IMFAsyncResult whose GetStatus() is
 *      S_OK, and GetState() must hand back the EXACT SAME guest pointer this
 *      program passed in to MFPutWorkItem -- not a wrapper, not a copy.  The
 *      guest handed native MF a guest-implemented IUnknown, native MF handed
 *      it back through a reverse call, and it must come back as the guest's
 *      own pointer.  This is the identity check the whole file exists for;
 *   4  Invoke must run on a thread that is not this program's main thread --
 *      native MF's own work-queue thread, which has never run guest code
 *      before, so the emulator's lazy per-thread init is load-bearing here;
 *   5  the callback's refcount must return to its starting value once the
 *      work item has completed and MFShutdown has run: native MF took
 *      references and gave every one of them back;
 *   6  IMFAsyncCallback::GetParameters is counted, not asserted -- if native
 *      MF turns out not to call it on this path, that is a finding to print,
 *      not a reason to fail;
 *   7  the SAME attribute store round trip happens through
 *      IMFAttributes::SetUnknown(MF_SOURCE_READER_ASYNC_CALLBACK) and
 *      IMFAttributes::GetUnknown: guest -> reverse proxy -> stored by native
 *      -> back out through the forward path -> must unwrap to the guest's
 *      own callback pointer again.  This is the check that would catch a
 *      mechanism that minted a fresh wrapper each way instead of recognising
 *      its own reverse proxy coming back;
 *   8  and then it opens the SAME file synchronously with that very
 *      attribute store (with the callback attribute removed again first --
 *      see the FINDING comment at the DeleteItem call below: the source
 *      reader's OWN async construction refuses that attribute outright,
 *      independently of the generic IMFAsyncCallback path steps 1-6 just
 *      proved works, because it would need a reverse proxy for a DIFFERENT
 *      interface, IMFSourceReaderCallback, that this port does not build
 *      yet) and reads one sample, so the run also shows what the new
 *      machinery costs the path a game's cutscene decoder actually takes:
 *      nothing.
 *
 * Copyright 2026 the ppc64le port authors
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 */

#define COBJMACROS
#define INITGUID

/* The attribute keys (MF_SOURCE_READER_ASYNC_CALLBACK and friends) are spelled
 * EXTERN_GUID, which declares without defining even under INITGUID.  Wine's own
 * libs/mfuuid/mfuuid.c does exactly this redefinition for exactly this reason;
 * doing it here is what lets the guest build link with no import library at
 * all, which is the point of the probe.  INITGUID above is what makes the
 * ordinary interface IIDs (IID_IUnknown, IID_IMFAsyncCallback) real
 * definitions rather than extern declarations, for the same reason. */
#undef EXTERN_GUID
#define EXTERN_GUID DEFINE_GUID

#include <windows.h>
#include <objbase.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mfobjects.h>
#include <mfreadwrite.h>
#include <mferror.h>

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

static void out_hex64( ULONGLONG v, int digits )
{
    static const char hex[] = "0123456789ABCDEF";
    char buf[17];
    int i;

    for (i = 0; i < digits; i++) buf[digits - 1 - i] = hex[(v >> (4 * i)) & 0xf];
    buf[digits] = 0;
    out( buf );
}

static void out_dec( ULONG v )
{
    char buf[12];
    int i = 11;

    buf[i] = 0;
    do { buf[--i] = '0' + (char)(v % 10); v /= 10; } while (v);
    out( buf + i );
}

static void out_hr( const char *label, HRESULT hr )
{
    out( label );
    out( "=0x" );
    out_hex( (ULONG)hr, 8 );
}

/* IsEqualIID expands to a memcmp() call, and this program links no CRT at
 * all (see the guest_build recipe in check-mf-smoke.sh) -- the same reason
 * mf_smoke.c carries its own guid_eq() instead of using the macro. */
static BOOL guid_eq( const GUID *a, const GUID *b )
{
    const BYTE *p = (const BYTE *)a, *q = (const BYTE *)b;
    int i;

    for (i = 0; i < (int)sizeof(GUID); i++) if (p[i] != q[i]) return FALSE;
    return TRUE;
}

/* Every pointer printed here is a guest address (this program IS the guest
 * image), 8 bytes on x86-64 -- out_hex's ULONG would silently truncate one,
 * which is exactly the kind of thing that makes a transcript lie. */
static void out_ptr( const void *p )
{
    out( "0x" );
    out_hex64( (ULONGLONG)(ULONG_PTR)p, 16 );
}

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

/* -------------------------------------------- a guest-implemented callback */
/* An ordinary COM object whose vtable is x86-64 code at a guest address --
 * exactly the pointer that, before the reverse proxy existed, could never
 * safely reach native code.  Now it is handed to native MF on purpose, and
 * every entry point counts itself so the run can prove native MF actually
 * called in rather than merely returning S_OK from somewhere else. */

struct guest_callback
{
    IMFAsyncCallback IMFAsyncCallback_iface;
    LONG refs;
    LONG qi_count;
    LONG addref_count;
    LONG release_count;
    LONG getparams_count;
    LONG invoke_count;
};

static struct guest_callback the_callback;

/* A second, minimal guest-implemented IUnknown, used only as MFPutWorkItem's
 * `state` argument.  It is a distinct object from the callback on purpose:
 * the identity check in Invoke (see below) must prove THIS SPECIFIC pointer
 * comes back, not merely that some guest object did. */
struct guest_state
{
    IUnknown IUnknown_iface;
    LONG refs;
};

static struct guest_state the_state;

/* Filled in by Invoke, on whatever thread native MF's work queue runs it on,
 * and read by the main thread only after WaitForSingleObject has observed
 * invoke_event -- that wait is the happens-before edge, so there is no data
 * race despite two threads touching these. */
static HANDLE invoke_event;
static DWORD main_tid;
static DWORD invoke_tid;
static BOOL invoke_result_nonnull;
static HRESULT invoke_get_status_hr = E_FAIL;
static HRESULT invoke_get_state_hr = E_FAIL;
static IUnknown *invoke_state_out;

static ULONG STDMETHODCALLTYPE cb_AddRef( IMFAsyncCallback *iface )
{
    the_callback.addref_count++;
    return ++the_callback.refs;
}

static ULONG STDMETHODCALLTYPE cb_Release( IMFAsyncCallback *iface )
{
    the_callback.release_count++;
    return --the_callback.refs;
}

static HRESULT STDMETHODCALLTYPE cb_QueryInterface( IMFAsyncCallback *iface,
                                                    REFIID riid, void **out_iface )
{
    the_callback.qi_count++;
    if (guid_eq( riid, &IID_IUnknown ) || guid_eq( riid, &IID_IMFAsyncCallback ))
    {
        *out_iface = iface;
        cb_AddRef( iface );
        return S_OK;
    }
    *out_iface = NULL;
    return E_NOINTERFACE;
}

static HRESULT STDMETHODCALLTYPE cb_GetParameters( IMFAsyncCallback *iface,
                                                   DWORD *flags, DWORD *queue )
{
    the_callback.getparams_count++;
    *flags = 0;
    *queue = 0;
    return S_OK;
}

/* THE round-trip check.  Everything it needs is captured into globals rather
 * than printed here, on purpose: this runs on native MF's work-queue thread,
 * concurrently (in principle) with the main thread's own out() calls, and
 * WriteFile calls from two threads interleaving mid-line would make the
 * transcript unreadable without changing whether the run passes.  The main
 * thread does all the printing, after the wait below has made these values
 * visible to it. */
static HRESULT STDMETHODCALLTYPE cb_Invoke( IMFAsyncCallback *iface,
                                            IMFAsyncResult *result )
{
    IUnknown *state = NULL;

    the_callback.invoke_count++;
    invoke_tid = GetCurrentThreadId();
    invoke_result_nonnull = (result != NULL);

    if (result)
    {
        invoke_get_status_hr = IMFAsyncResult_GetStatus( result );
        invoke_get_state_hr = IMFAsyncResult_GetState( result, &state );
        invoke_state_out = state;
        /* GetState AddRefs; this program only needs the pointer VALUE for the
         * identity check, so give the reference straight back. */
        if (state) IUnknown_Release( state );
    }

    if (invoke_event) SetEvent( invoke_event );
    return S_OK;
}

static IMFAsyncCallbackVtbl callback_vtbl =
{
    cb_QueryInterface,
    cb_AddRef,
    cb_Release,
    cb_GetParameters,
    cb_Invoke,
};

static HRESULT STDMETHODCALLTYPE st_QueryInterface( IUnknown *iface,
                                                     REFIID riid, void **out_iface )
{
    if (guid_eq( riid, &IID_IUnknown ))
    {
        *out_iface = iface;
        ++the_state.refs;
        return S_OK;
    }
    *out_iface = NULL;
    return E_NOINTERFACE;
}

static ULONG STDMETHODCALLTYPE st_AddRef( IUnknown *iface ) { return ++the_state.refs; }
static ULONG STDMETHODCALLTYPE st_Release( IUnknown *iface ) { return --the_state.refs; }

static IUnknownVtbl state_vtbl =
{
    st_QueryInterface,
    st_AddRef,
    st_Release,
};

/* ------------------------------------------------------------- the run */

static int mf_async_run( void )
{
    IMFAttributes *attrs = NULL;
    IMFSourceReader *reader = NULL;
    IMFAsyncCallback *cb;
    IUnknown *state;
    IUnknown *got = NULL;
    WCHAR url[MAX_PATH];
    LONG start_refs;
    HRESULT hr;
    DWORD wait, n;

    the_callback.IMFAsyncCallback_iface.lpVtbl = &callback_vtbl;
    the_callback.refs = 1;
    cb = &the_callback.IMFAsyncCallback_iface;

    the_state.IUnknown_iface.lpVtbl = &state_vtbl;
    the_state.refs = 1;
    state = &the_state.IUnknown_iface;

    main_tid = GetCurrentThreadId();

    out( "mf_async: start\n" );

    n = GetEnvironmentVariableW( L"MF_SMOKE_URL", url, MAX_PATH );
    if (!n || n >= MAX_PATH)
    {
        out( "mf_async: FAIL (MF_SMOKE_URL is not set)\n" );
        return 1;
    }

    begin( "CreateEventW(the invoke-completion event)" );
    invoke_event = CreateEventW( NULL, TRUE, FALSE, NULL );
    verdict( invoke_event != NULL, "CreateEventW failed" );

    begin( "MFStartup(MF_VERSION, MFSTARTUP_FULL)" );
    hr = MFStartup( MF_VERSION, MFSTARTUP_FULL );
    out_hr( "hr", hr );
    verdict( hr == S_OK, "not S_OK" );
    if (hr != S_OK) goto done;

    /* THE identity object: the guest's own refcount on its own callback,
     * before native MF has ever seen it.  Checked again at the very end,
     * after MFShutdown, to prove every reference native MF took came back. */
    start_refs = the_callback.refs;

    /* 1: the reverse wrapper now SERVES this call instead of refusing it --
     * dlls/mfplat/mfcom.c __wine_guest_MFPutWorkItem, through MF_IN with
     * MF_IFACE_IMFAsyncCallback, mints a reverse proxy for `cb` and another
     * for `state` and hands NATIVE pointers to rtworkq. */
    begin( "MFPutWorkItem(MFASYNC_CALLBACK_QUEUE_STANDARD, callback, state)" );
    hr = MFPutWorkItem( MFASYNC_CALLBACK_QUEUE_STANDARD, cb, state );
    out_hr( "hr", hr );
    verdict( hr == S_OK, "native MF refused the guest-implemented IMFAsyncCallback" );

    /* 2: bounded -- a timeout is a FAIL, never a hang.  If the reverse
     * mechanism silently handed native code a guest vtable it cannot execute,
     * or if MFPutWorkItem accepted the call but the work queue never runs it,
     * this is where that becomes visible instead of the gate wedging. */
    begin( "waiting for Invoke to signal completion (bounded, 5s)" );
    wait = invoke_event ? WaitForSingleObject( invoke_event, 5000 ) : WAIT_FAILED;
    out( "wait=0x" );
    out_hex( wait, 8 );
    verdict( wait == WAIT_OBJECT_0, "timed out (or no event) waiting for the "
                                    "guest callback to run" );

    /* 3: it ran, exactly once. */
    begin( "IMFAsyncCallback::Invoke was entered exactly once" );
    out( "count=" );
    out_dec( (ULONG)the_callback.invoke_count );
    verdict( the_callback.invoke_count == 1, "wrong number of Invoke calls" );

    /* 4: a non-NULL result whose status is S_OK -- the ordinary shape of a
     * work item that completed cleanly. */
    begin( "Invoke received a non-NULL IMFAsyncResult" );
    verdict( invoke_result_nonnull, "the result argument was NULL" );

    begin( "IMFAsyncResult::GetStatus() inside Invoke was S_OK" );
    out_hr( "hr", invoke_get_status_hr );
    verdict( invoke_get_status_hr == S_OK, "not S_OK" );

    /* 5: THE ROUND-TRIP IDENTITY CHECK -- the most important assertion in
     * this file.  `state` is the guest's own pointer, written here in this
     * program's own image.  It crossed into native MF as a reverse proxy
     * (MFPutWorkItem's `state` argument), native MF stored it in the
     * IMFAsyncResult it built and handed back OUT of GetState -- a forward
     * translate-out that must recognise its own reverse proxy coming back and
     * unwrap it to the guest pointer (libs/winecom/winecom.c winecom_wrap,
     * "host is our reverse proxy for guest ...; returning the guest's own
     * pointer") rather than mint a fresh wrapper around it.  If this ever
     * compares unequal, the round trip is minting new objects instead of
     * recognising old ones, and every stateful async callback in a real game
     * would silently see the wrong object. */
    begin( "IMFAsyncResult::GetState() round-trips the guest's OWN state pointer" );
    out_hr( "hr", invoke_get_state_hr );
    out( " got=" );
    out_ptr( invoke_state_out );
    out( " ours=" );
    out_ptr( state );
    verdict( invoke_get_state_hr == S_OK && invoke_state_out == state,
             "the state object that came back is not the guest's own pointer -- "
             "the round trip minted a different object instead of recognising "
             "its own reverse proxy" );

    /* 6: native MF's work queue owns this thread, and it has never run guest
     * code before -- the emulator's lazy per-thread init is what makes
     * running x86-64 guest code on a freshly-seen native thread work at all. */
    begin( "Invoke ran on a thread other than this program's main thread" );
    out( "main=0x" );
    out_hex( main_tid, 8 );
    out( " invoke=0x" );
    out_hex( invoke_tid, 8 );
    verdict( invoke_tid != 0 && invoke_tid != main_tid,
             "the work item ran on the calling thread instead of a native "
             "work-queue thread" );

    /* 7: counted, not asserted -- if it turns out native MF does not call
     * GetParameters on this path, that is a fact about MF's implementation to
     * report, not a reason to fail a probe about the reverse-proxy mechanism. */
    out( "note IMFAsyncCallback::GetParameters call count=" );
    out_dec( (ULONG)the_callback.getparams_count );
    out( "\n" );
    if (!the_callback.getparams_count)
        out( "note: native MF did not call GetParameters on this async path\n" );

    /* 8: the SAME round trip through the attribute store instead of through
     * MFPutWorkItem -- a different code path (winecom_dispatch's
     * WINECOM_CA_IFACE_IN classifier rather than a flat wrapper's MF_IN) that
     * must reach the same answer. */
    begin( "MFCreateAttributes" );
    hr = MFCreateAttributes( &attrs, 1 );
    out_hr( "hr", hr );
    verdict( hr == S_OK && attrs != NULL, "no attribute store" );

    if (attrs)
    {
        begin( "IMFAttributes::SetUnknown(MF_SOURCE_READER_ASYNC_CALLBACK) == S_OK" );
        hr = IMFAttributes_SetUnknown( attrs, &MF_SOURCE_READER_ASYNC_CALLBACK,
                                       (IUnknown *)cb );
        out_hr( "hr", hr );
        verdict( hr == S_OK, "native MF refused the guest-implemented callback "
                             "as an attribute value" );

        /* The second round trip: guest -> reverse proxy -> stored by native ->
         * back out through the forward path -> must unwrap to the guest's own
         * callback pointer again.  A mechanism that minted a fresh wrapper on
         * either leg would still return S_OK here and still be wrong. */
        begin( "IMFAttributes::GetUnknown(MF_SOURCE_READER_ASYNC_CALLBACK) "
              "round-trips the guest's OWN callback pointer" );
        hr = IMFAttributes_GetUnknown( attrs, &MF_SOURCE_READER_ASYNC_CALLBACK,
                                       &IID_IUnknown, (void **)&got );
        out_hr( "hr", hr );
        out( " got=" );
        out_ptr( got );
        out( " ours=" );
        out_ptr( cb );
        verdict( hr == S_OK && got == (IUnknown *)cb,
                 "the stored callback did not come back as the guest's own "
                 "pointer" );
        if (got) IUnknown_Release( got );

        /* FINDING, not a bug in this probe: MF_SOURCE_READER_ASYNC_CALLBACK is
         * not just "an IUnknown attribute" to IMFSourceReader's OWN creation
         * wrappers -- dlls/mfreadwrite/mfcom.c mf_attributes_carry_callback
         * inspects the host attribute store for exactly this key and refuses
         * MFCreateSourceReaderFromURL outright if it is present, REGARDLESS of
         * what the stored object implements.  That is a SEPARATE, still-
         * unserved reverse-proxy target (IMFSourceReaderCallback, invoked by
         * the source reader's own async construction) from the generic
         * IMFAsyncCallback / MFPutWorkItem path steps 1-9 above just proved
         * works -- the two are different interfaces reached through different
         * wrappers, and only one of them has a reverse proxy behind it yet.
         * So the round trip above is real and correct, but the object has to
         * come back OUT before this same store can build a source reader. */
        begin( "IMFAttributes::DeleteItem(MF_SOURCE_READER_ASYNC_CALLBACK)" );
        hr = IMFAttributes_DeleteItem( attrs, &MF_SOURCE_READER_ASYNC_CALLBACK );
        out_hr( "hr", hr );
        verdict( hr == S_OK, "could not drop the attribute again" );
    }

    /* 9: the point of building all of the above is that it costs the
     * SYNCHRONOUS path nothing -- the same attribute store this program just
     * proved the reverse-proxy round trip through, now empty of the
     * reader-level callback attribute again, still opens the file and
     * delivers a sample the ordinary way. */
    begin( "MFCreateSourceReaderFromURL with that same attribute store" );
    hr = MFCreateSourceReaderFromURL( url, attrs, &reader );
    out_hr( "hr", hr );
    verdict( hr == S_OK && reader != NULL, "the synchronous path broke too" );

    if (reader)
    {
        IMFSample *sample = NULL;
        DWORD flags = 0, index = 0;
        LONGLONG ts = 0;

        begin( "IMFSourceReader::ReadSample (synchronous) still delivers" );
        hr = IMFSourceReader_SetStreamSelection( reader,
                 MF_SOURCE_READER_FIRST_AUDIO_STREAM, TRUE );
        if (SUCCEEDED(hr))
            hr = IMFSourceReader_ReadSample( reader,
                     MF_SOURCE_READER_FIRST_AUDIO_STREAM, 0, &index, &flags,
                     &ts, &sample );
        out_hr( "hr", hr );
        out( " sample=" );
        out( sample ? "yes" : "no" );
        verdict( SUCCEEDED(hr) && sample != NULL, "no sample" );
        if (sample) IMFSample_Release( sample );
        IMFSourceReader_Release( reader );
    }

    if (attrs) IMFAttributes_Release( attrs );

    begin( "MFShutdown" );
    hr = MFShutdown();
    out_hr( "hr", hr );
    verdict( hr == S_OK, "not S_OK" );

    /* 10: THE REFCOUNT BALANCE CHECK, and it can only be answered here, after
     * MFShutdown has drained every work queue and every proxy built along the
     * way (MFPutWorkItem's, the attribute store's) has been released.  Equal
     * to what the guest started with means native MF gave back everything it
     * took; anything else means a reverse proxy -- or the guest object behind
     * one -- outlived the call that should have released it. */
    begin( "the callback's refcount returned to its starting value" );
    out( "start=" );
    out_dec( (ULONG)start_refs );
    out( " now=" );
    out_dec( (ULONG)the_callback.refs );
    verdict( the_callback.refs == start_refs,
             "native MF did not release everything it took" );

done:
    if (invoke_event) CloseHandle( invoke_event );

    out( failures ? "mf_async: FAIL " : "mf_async: PASS " );
    out_dec( (ULONG)(step - failures) );
    out( "/" );
    out_dec( (ULONG)step );
    out( "\n" );
    return failures ? 1 : 0;
}

void WINAPI mf_async_entry( void )
{
    ExitProcess( (UINT)mf_async_run() );
}
