/*
 * arrin_probe -- the CA_IFACE_ARR_IN DELIVERY leg, measured.
 *
 * GUEST ONLY, and unavoidably so, for the same reason reverse_probe.c is:
 * what it measures is native ppc64 code calling an x86-64 vtable, which is a
 * thing that does not exist in a native run.  Every claim it makes is a value
 * check against a constant both sides compile from
 * include/wine/winecom_arrin.h.
 *
 * WHAT IT IS.  The program builds ONE COM object in its own image -- an
 * IWbemObjectSink, an x86-64 vtable at a guest address -- and hands it to
 * __wine_winecom_arrin_selftest, a native hook (dlls/combase/syscom.c) that
 * calls IWbemObjectSink::Indicate on it with an ARRAY of native
 * IWbemClassObject pointers.  That is the only row on any roster this port
 * serves whose class table carries WINECOM_CA_IFACE_ARR_IN, so this is the
 * arm being driven and not a model of it.
 *
 * THE ELEMENT-WISE CLAIM is the whole point, and it is made HERE because it
 * can only be made here.  Native side can see that its array came back
 * unmutated and that each of its objects was entered once; it cannot see
 * WHICH proxy the guest was handed for WHICH position.  This program can: it
 * calls a method on element k and requires the answer that only native object
 * k gives.  A marshal arm that wrapped the same source pointer for every
 * position, or that shuffled them, passes every native-side check and fails
 * this one.
 *
 * Two further things the elements have to be, and both are checked before any
 * of them is called: NON-NULL (the arm scrubs an unwrappable element to NULL
 * loudly, which is correct behaviour and a failed delivery) and DISTINCT from
 * each other (three positions, three objects, three proxies).
 *
 * NO C RUNTIME (-DARRIN_PROBE_NO_CRT): the program formats its own output and
 * writes it with WriteFile, the same discipline reverse_probe.c keeps and for
 * the same reason -- a CRT would add a second variable to a test whose whole
 * value is that only one thing is under test.
 *
 * Copyright 2026 the ppc64le port authors
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 */

/* INITGUID before anything else: widl spells an interface's IID as a
 * DEFINE_GUID, which under this macro DEFINES it here instead of declaring it
 * for a libuuid that is not in this link -- the probe's own .def describes
 * every import it has, and it has two. */
#define COBJMACROS
#define INITGUID

#include <windows.h>
#include <objbase.h>
#include <wbemcli.h>

#include <wine/winecom_arrin.h>

/* ------------------------------------------------------------- output */

static void out( const char *s )
{
    DWORD n = 0, written;

    while (s[n]) n++;
    WriteFile( GetStdHandle( STD_OUTPUT_HANDLE ), s, n, &written, NULL );
}

static void out_hex( ULONG v )
{
    static const char hex[] = "0123456789ABCDEF";
    char buf[9];
    int i;

    for (i = 0; i < 8; i++) buf[7 - i] = hex[(v >> (4 * i)) & 0xf];
    buf[8] = 0;
    out( buf );
}

static void out_dec( ULONG v )
{
    char buf[12];
    int i = 11;

    buf[i] = 0;
    do { buf[--i] = (char)('0' + (v % 10)); v /= 10; } while (v);
    out( buf + i );
}

/* IsEqualGUID is memcmp and there is no CRT here.  Comparing the fields is
 * also the honest thing: a GUID is a struct, not sixteen adjacent bytes. */
static BOOL guid_eq( const GUID *a, const GUID *b )
{
    int i;

    if (a->Data1 != b->Data1 || a->Data2 != b->Data2 || a->Data3 != b->Data3)
        return FALSE;
    for (i = 0; i < 8; i++) if (a->Data4[i] != b->Data4[i]) return FALSE;
    return TRUE;
}

static int checks, failures;

static void step( const char *what, int ok, const char *detail )
{
    checks++;
    if (!ok) failures++;
    out( "step " );
    out_dec( checks );
    out( " " );
    out( what );
    out( ok ? ": ok" : ": FAIL" );
    if (detail) { out( " (" ); out( detail ); out( ")" ); }
    out( "\n" );
}

/* ---------------------------------------------------- the guest COM object */

/* What the deliveries told us.  RECORDED rather than checked on the spot: a
 * delivery that never arrived and a delivery that arrived wrong have to be
 * told apart, and "never arrived" is only visible once the run is over. */
static struct
{
    ULONG   indicates;                          /* Indicate calls entered */
    ULONG   empty_indicates;                    /* ...of which empty ones */
    LONG    full_count;                         /* the count the FULL delivery
                                                   carried, by value */
    void   *elem[WINECOM_ARRIN_COUNT];          /* the pointers we were handed */
    HRESULT elem_hr[WINECOM_ARRIN_COUNT];       /* ...and what each answered */
    ULONG   all_nonnull, all_distinct, all_right;
    ULONG   empty_null;                         /* the empty delivery's array
                                                   arrived as NULL, not as a
                                                   staging buffer's address */
    ULONG   setstatus;                          /* must stay 0 */
} seen;

struct guest_sink
{
    IWbemObjectSink IWbemObjectSink_iface;
    LONG refs;
};

static struct guest_sink the_sink;

static HRESULT WINAPI gs_QueryInterface( IWbemObjectSink *iface, REFIID riid, void **out_p )
{
    if (!out_p) return E_POINTER;
    *out_p = NULL;
    if (guid_eq( riid, &IID_IUnknown ) || guid_eq( riid, &IID_IWbemObjectSink ))
    {
        *out_p = iface;
        IWbemObjectSink_AddRef( iface );
        return S_OK;
    }
    return E_NOINTERFACE;
}

static ULONG WINAPI gs_AddRef( IWbemObjectSink *iface )
{
    struct guest_sink *o = (struct guest_sink *)iface;

    return (ULONG)InterlockedIncrement( &o->refs );
}

static ULONG WINAPI gs_Release( IWbemObjectSink *iface )
{
    struct guest_sink *o = (struct guest_sink *)iface;

    return (ULONG)InterlockedDecrement( &o->refs );
}

/* THE ROW.  cls_IWbemObjectSink_3 is { CA_PASS, CA_IFACE_ARR_IN } with aux2
 * naming the count parameter, so everything below arrived through the arm:
 * lObjectCount by value, and apObjArray as the address of the dispatcher's
 * own staging, holding one forward proxy per native object. */
static HRESULT WINAPI gs_Indicate( IWbemObjectSink *iface, LONG count,
                                   IWbemClassObject **objs )
{
    ULONG nonnull = 1, distinct = 1, right = 1;
    LONG i, j;

    (void)iface;
    seen.indicates++;

    /* The empty delivery -- a WMI query that matched nothing still calls
     * Indicate -- must arrive as itself: the arm short-circuits on a zero
     * count and must pass the caller's NULL through rather than the address
     * of a staging buffer it did not fill. */
    if (!count)
    {
        seen.empty_indicates++;
        if (!objs) seen.empty_null++;
        return WINECOM_ARRIN_HR_EMPTY;
    }

    seen.full_count = count;
    if (count != WINECOM_ARRIN_COUNT || !objs) return E_INVALIDARG;

    for (i = 0; i < count; i++)
    {
        seen.elem[i] = objs[i];
        if (!objs[i]) { nonnull = 0; continue; }
        for (j = 0; j < i; j++) if (objs[j] == objs[i]) distinct = 0;

        /* THE ELEMENT-WISE CHECK.  Element k's native object answers
         * WINECOM_ARRIN_HR(k) and nothing else does, so this HRESULT names
         * WHICH object is behind this position.  The call itself is an
         * ordinary FORWARD crossing out of a reverse call, which is the one
         * place the two directions meet. */
        seen.elem_hr[i] = IWbemClassObject_Get( objs[i], NULL, 0, NULL, NULL, NULL );
        if (seen.elem_hr[i] != WINECOM_ARRIN_HR( i )) right = 0;
    }

    seen.all_nonnull  = nonnull;
    seen.all_distinct = distinct;
    seen.all_right    = right;
    return WINECOM_ARRIN_HR_OK;
}

static HRESULT WINAPI gs_SetStatus( IWbemObjectSink *iface, LONG flags, HRESULT res,
                                    BSTR param, IWbemClassObject *obj )
{
    (void)iface; (void)flags; (void)res; (void)param; (void)obj;
    seen.setstatus++;          /* nothing should ever reach this */
    return E_NOTIMPL;
}

/* Not `const`: IWbemObjectSink::lpVtbl is a pointer to non-const, which is
 * widl's spelling for every interface, and a guest object is the one place a
 * probe has to satisfy it. */
static IWbemObjectSinkVtbl gs_vtbl =
{
    gs_QueryInterface,
    gs_AddRef,
    gs_Release,
    gs_Indicate,
    gs_SetStatus,
};

/* ------------------------------------------------------------- the run */

void WINAPI arrin_probe_entry( void )
{
    struct winecom_arrin_report report;
    HRESULT hr;
    ULONG i;

    the_sink.IWbemObjectSink_iface.lpVtbl = &gs_vtbl;
    the_sink.refs = 1;
    for (i = 0; i < sizeof(report); i++) ((BYTE *)&report)[i] = 0xA5;

    hr = __wine_winecom_arrin_selftest( &the_sink.IWbemObjectSink_iface, &report );

    /* ---- what arrived HERE ---- */
    step( "both deliveries arrived", seen.indicates == 2, NULL );
    step( "the count crossed by value",
          seen.full_count == WINECOM_ARRIN_COUNT, NULL );
    step( "every element is non-NULL", seen.all_nonnull != 0,
          "a NULL would be the arm scrubbing an element it could not wrap" );
    step( "every element is a DISTINCT proxy", seen.all_distinct != 0,
          "three positions, three pointers" );
    step( "element k is native object k", seen.all_right != 0,
          "the per-element answer names which object arrived where" );
    step( "the empty delivery arrived, with a NULL array",
          seen.empty_indicates == 1 && seen.empty_null == 1, NULL );
    step( "no other slot of the sink was entered", seen.setstatus == 0, NULL );

    /* ---- what the hook reported ---- */
    step( "the hook ran its own checks", report.checks != 0 && report.checks != 0xA5A5A5A5,
          "the seed pattern is gone, so the report was written" );
    step( "the hook's checks all passed", report.failures == 0, NULL );
    step( "the caller's array was not mutated", report.array_unmutated != 0, NULL );
    step( "each object was entered exactly once", report.entered_once != 0, NULL );
    step( "the objects were entered in element order", report.in_order != 0, NULL );
    step( "the delivery gave back every reference it took",
          report.refs_leaked == 0,
          "the arm takes one per element for the guest and must return it" );
    step( "the hook answered S_OK", hr == S_OK, NULL );

    out( "elements:" );
    for (i = 0; i < WINECOM_ARRIN_COUNT; i++)
    {
        out( " [" ); out_dec( i ); out( "]=0x" ); out_hex( (ULONG)seen.elem_hr[i] );
    }
    out( "\nrefs: before " ); out_dec( report.refs_before );
    out( " after " ); out_dec( report.refs_after );
    out( "\narrin_probe: " );
    out( failures ? "FAIL" : "PASS" );
    out( " (" ); out_dec( (ULONG)checks ); out( " checks, " );
    out_dec( (ULONG)failures ); out( " failed)\n" );

    ExitProcess( failures ? 1 : 0 );
}
