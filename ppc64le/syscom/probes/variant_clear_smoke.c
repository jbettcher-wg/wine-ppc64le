/*
 * variant_clear_smoke -- the VariantClear GUEST-IMPL wrapper's gate.
 *
 * Same method as ppc64le/syscom/com_smoke.c: ONE source, built TWICE (native
 * ppc64 Windows PE, x86-64 guest PE), run twice, byte-identical stdout
 * required for every step except the one the design says diverges on
 * purpose (L7, marked and excluded from the diff below).  Every printed
 * value is something Wine's own oleaut32/ole32 computed, so agreement means
 * the guest reached the same implementation through
 * dlls/combase/syscom.c __wine_guest_VariantClear with nothing lost.
 *
 * THE LEG THAT MATTERS MOST is L5: a live IStream forward proxy (from the
 * already-wrapped CreateStreamOnHGlobal) put inside a VARIANT's VT_UNKNOWN
 * slot and cleared.  The correct wrapper drops the guest-visible reference
 * THROUGH THE PROXY (__wine_com_release_guest / winecom_release_guest_seen /
 * proxy_release) and never touches the ONE host reference the proxy owns
 * directly.  The gate script's trace check is the real proof: proxy_release
 * is the only thing that ever prints "destroying proxy ... (IStream host
 * ...)", and this program creates exactly three IStream proxies through
 * ordinary, always-correct paths (Stream A explicitly Released at the end of
 * L4b, Stream C explicitly Released at the end of L5) plus Stream B, whose
 * ONLY route to destruction is the VariantClear call under test.  A correct
 * run therefore prints that trace line exactly THREE times; a run sabotaged
 * with WINEEMUVARIANTUNSAFERELEASE=1 (see dlls/combase/syscom.c) releases
 * Stream B's host reference directly and skips proxy_release entirely, so
 * the proxy never learns its count reached zero and the line prints only
 * TWICE.  That is a small, deterministic, non-heap-timing-dependent
 * fingerprint of the double-free-in-waiting the wrapper's own comment
 * forbids -- no crash needs to happen for the gate to see it.
 *
 * L1-L11 cover every row of the case table in dlls/combase/syscom.c's
 * __wine_guest_VariantClear comment:
 *
 *   L1  plain scalars (I4, R8, CY, DECIMAL)            -> pass, untouched payload
 *   L2  VT_BSTR                                        -> pass
 *   L3  a vt VARIANT_ValidateType rejects (0x1fff)      -> DISP_E_BADVARTYPE, untouched
 *   L4a VT_BYREF|VT_I4                                  -> pass, referent intact
 *   L4b VT_BYREF|VT_UNKNOWN over a LIVE proxy            -> pass, proxy still alive after
 *   L5  VT_UNKNOWN holding a forward proxy (THE leg)     -> guest-visible ref dropped
 *       through the proxy; second stream's round trip proves no stale intern
 *   L6  idempotence: clearing an already-VT_EMPTY VARIANT -> S_OK, still VT_EMPTY
 *   L7  VT_UNKNOWN holding a GUEST-IMPLEMENTED object    -> DIVERGES BY DESIGN:
 *       native actually releases it (S_OK, released=1); the guest wrapper
 *       classifies it as not-a-proxy and refuses (E_NOTIMPL, released=0)
 *   L8  VT_ARRAY|VT_UNKNOWN, a hand-built descriptor with FADF_UNKNOWN set
 *                                                        -> refused, descriptor untouched
 *   L9  VT_ARRAY|VT_I4 with parray==NULL                 -> pass (no-op), S_OK
 *   L10 VT_RECORD with a non-NULL (never dereferenced) IRecordInfo
 *                                                        -> refused, untouched
 *   L11 VT_RECORD with pRecInfo==NULL                    -> pass (no-op), S_OK
 *
 * L7 runs on BOTH lanes (a real IUnknown release is well-defined either way)
 * and its one line is excluded from the byte-identical diff, checked against
 * a per-lane expectation instead.  L8 and L10 are GUEST-ONLY and compiled
 * out of the native build entirely (#ifdef VARIANT_SMOKE_NO_CRT): they hand
 * the wrapper a stack SAFEARRAY/dummy IRecordInfo built so the wrapper's
 * refusal path is the only one that can be taken SAFELY, and real native
 * VariantClear has no such refusal -- it would call SafeArrayDestroy /
 * IRecordInfo_RecordClear on our fake descriptor for real, which is
 * undefined behaviour on real Windows too (system-com-design.md §9.2).  An
 * earlier version of this file ran L8/L10 on native anyway "just to diff
 * it" and it silently corrupted the native binary's heap (see the comment
 * at the guest-only block below).
 *
 * NO C RUNTIME on the guest side (-DVARIANT_SMOKE_NO_CRT), matching
 * com_smoke.c's reasoning exactly.
 *
 * Copyright 2026 the ppc64le port authors
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#define COBJMACROS

#include <windows.h>
#include <objbase.h>
#include <oleauto.h>

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
    char buf[16];
    int i = 15;
    ULONG u;
    BOOL neg = v < 0;

    u = neg ? (ULONG)(-v) : (ULONG)v;
    buf[i] = 0;
    do { buf[--i] = '0' + (char)(u % 10); u /= 10; } while (u);
    if (neg) buf[--i] = '-';
    out( buf + i );
}

static void out_hr( const char *label, HRESULT hr )
{
    out( label );
    out( "=0x" );
    out_hex( (ULONG)hr, 8 );
}

static void out_vt( const char *label, VARTYPE vt )
{
    out( label );
    out( "=0x" );
    out_hex( (ULONG)vt, 4 );
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

/* ------------------------------------------------- a guest-implemented IUnknown
 *
 * A minimal object with its OWN vtable, built entirely in this program's own
 * image -- x86-64 code on the guest build, ppc64 code on the native build.
 * It never goes through winecom_wrap, so it is never one of our proxies: the
 * wrapper's __wine_com_translate_in classification MUST answer "no" for it,
 * which is exactly L7's point.
 */
typedef struct fake_unknown
{
    const IUnknownVtbl *lpVtbl;
    LONG refs;
    int released;
} fake_unknown;

static HRESULT WINAPI fake_QueryInterface( IUnknown *iface, REFIID riid, void **ppv )
{
    (void)iface; (void)riid;
    if (ppv) *ppv = NULL;
    return E_NOINTERFACE;
}

static ULONG WINAPI fake_AddRef( IUnknown *iface )
{
    fake_unknown *f = (fake_unknown *)iface;
    return (ULONG)++f->refs;
}

static ULONG WINAPI fake_Release( IUnknown *iface )
{
    fake_unknown *f = (fake_unknown *)iface;
    ULONG r = (ULONG)--f->refs;
    if (r == 0) f->released = 1;
    return r;
}

static const IUnknownVtbl fake_vtbl =
{
    fake_QueryInterface, fake_AddRef, fake_Release
};

/* ---------------------------------------------------------------------- */

static int variant_clear_smoke_run( void )
{
    VARIANT v;
    HRESULT hr;
    IStream *streamA = NULL, *streamB = NULL, *streamC = NULL;
    ULONG r1, r2;

    out( "variant_clear_smoke: start\n" );

    begin( "CoInitializeEx(APARTMENTTHREADED)" );
    hr = CoInitializeEx( NULL, COINIT_APARTMENTTHREADED );
    out_hr( "hr", hr );
    verdict( hr == S_OK, "not S_OK" );
    if (hr != S_OK) goto done;

    /* ---- L1: plain scalars ---- */
    begin( "L1a VariantClear(VT_I4=42)" );
    VariantInit( &v );
    V_VT( &v ) = VT_I4;
    V_I4( &v ) = 42;
    hr = VariantClear( &v );
    out_hr( "hr", hr );
    out_vt( " vt", V_VT( &v ) );
    out( " lVal=" );
    out_dec( V_I4( &v ) );
    verdict( hr == S_OK && V_VT( &v ) == VT_EMPTY && V_I4( &v ) == 42,
             "wrong result or payload touched" );

    begin( "L1b VariantClear(VT_R8)" );
    VariantInit( &v );
    V_VT( &v ) = VT_R8;
    V_R8( &v ) = 3.0;
    hr = VariantClear( &v );
    out_hr( "hr", hr );
    out_vt( " vt", V_VT( &v ) );
    verdict( hr == S_OK && V_VT( &v ) == VT_EMPTY, "wrong result" );

    begin( "L1c VariantClear(VT_CY)" );
    VariantInit( &v );
    V_VT( &v ) = VT_CY;
    V_CY( &v ).int64 = 12345;
    hr = VariantClear( &v );
    out_hr( "hr", hr );
    out_vt( " vt", V_VT( &v ) );
    verdict( hr == S_OK && V_VT( &v ) == VT_EMPTY, "wrong result" );

    begin( "L1d VariantClear(VT_DECIMAL)" );
    VariantInit( &v );
    V_DECIMAL( &v ).Lo64 = 7;
    V_VT( &v ) = VT_DECIMAL;
    hr = VariantClear( &v );
    out_hr( "hr", hr );
    out_vt( " vt", V_VT( &v ) );
    verdict( hr == S_OK && V_VT( &v ) == VT_EMPTY, "wrong result" );

    /* ---- L2: BSTR ---- */
    begin( "L2 VariantClear(VT_BSTR)" );
    VariantInit( &v );
    V_VT( &v ) = VT_BSTR;
    V_BSTR( &v ) = SysAllocString( L"variant_clear_smoke" );
    out( "len=" );
    out_dec( (LONG)SysStringLen( V_BSTR( &v ) ) );
    hr = VariantClear( &v );
    out_hr( " hr", hr );
    out_vt( " vt", V_VT( &v ) );
    verdict( hr == S_OK && V_VT( &v ) == VT_EMPTY, "wrong result" );

    /* ---- L3: bad vt ---- */
    begin( "L3 VariantClear(bad vt 0x1fff)" );
    VariantInit( &v );
    V_VT( &v ) = 0x1fff;
    hr = VariantClear( &v );
    out_hr( "hr", hr );
    out_vt( " vt", V_VT( &v ) );
    verdict( hr == DISP_E_BADVARTYPE && V_VT( &v ) == 0x1fff,
             "not DISP_E_BADVARTYPE or VARIANT was touched" );

    /* ---- L4a: VT_BYREF|VT_I4 ---- */
    begin( "L4a VariantClear(VT_BYREF|VT_I4)" );
    {
        LONG referent = 99;
        VariantInit( &v );
        V_VT( &v ) = VT_BYREF | VT_I4;
        V_I4REF( &v ) = &referent;
        hr = VariantClear( &v );
        out_hr( "hr", hr );
        out_vt( " vt", V_VT( &v ) );
        out( " referent=" );
        out_dec( referent );
        verdict( hr == S_OK && V_VT( &v ) == VT_EMPTY && referent == 99,
                 "wrong result or referent touched" );
    }

    /* ---- L4b: VT_BYREF|VT_UNKNOWN over a LIVE proxy ---- */
    begin( "CreateStreamOnHGlobal (stream A)" );
    hr = CreateStreamOnHGlobal( NULL, TRUE, &streamA );
    out_hr( "hr", hr );
    verdict( hr == S_OK && streamA != NULL, "no stream" );

    begin( "L4b VariantClear(VT_BYREF|VT_UNKNOWN) over stream A" );
    VariantInit( &v );
    V_VT( &v ) = VT_BYREF | VT_UNKNOWN;
    V_UNKNOWNREF( &v ) = (IUnknown **)&streamA;
    hr = VariantClear( &v );
    out_hr( "hr", hr );
    out_vt( " vt", V_VT( &v ) );
    verdict( hr == S_OK && V_VT( &v ) == VT_EMPTY && streamA != NULL,
             "wrong result or referent touched" );

    begin( "stream A still alive: AddRef/Release" );
    r1 = IStream_AddRef( streamA );
    r2 = IStream_Release( streamA );
    out( "refs=" );
    out_dec( (LONG)r1 );
    out( "/" );
    out_dec( (LONG)r2 );
    verdict( r1 == 2 && r2 == 1, "BYREF clear released something" );

    begin( "release stream A" );
    r1 = IStream_Release( streamA );
    out( "refs=" );
    out_dec( (LONG)r1 );
    verdict( r1 == 0, "did not drop to zero" );
    streamA = NULL;

    /* ---- L5: the leg that matters -- VT_UNKNOWN holding a forward proxy ---- */
    begin( "CreateStreamOnHGlobal (stream B)" );
    hr = CreateStreamOnHGlobal( NULL, TRUE, &streamB );
    out_hr( "hr", hr );
    verdict( hr == S_OK && streamB != NULL, "no stream" );

    begin( "stream B: AddRef/Release (guest-visible count)" );
    r1 = IStream_AddRef( streamB );
    r2 = IStream_Release( streamB );
    out( "refs=" );
    out_dec( (LONG)r1 );
    out( "/" );
    out_dec( (LONG)r2 );
    verdict( r1 == 2 && r2 == 1, "refcount does not round-trip" );

    begin( "L5 VariantClear(VT_UNKNOWN) holding stream B" );
    VariantInit( &v );
    V_VT( &v ) = VT_UNKNOWN;
    V_UNKNOWN( &v ) = (IUnknown *)streamB;
    hr = VariantClear( &v );
    out_hr( "hr", hr );
    out_vt( " vt", V_VT( &v ) );
    verdict( hr == S_OK && V_VT( &v ) == VT_EMPTY, "wrong result" );
    streamB = NULL;   /* the reference is gone; do not touch it again */

    begin( "CreateStreamOnHGlobal (stream C, proves no stale intern)" );
    hr = CreateStreamOnHGlobal( NULL, TRUE, &streamC );
    out_hr( "hr", hr );
    verdict( hr == S_OK && streamC != NULL, "no stream" );

    if (streamC)
    {
        static const char msg[] = "variant_clear_smoke!";
        ULARGE_INTEGER pos;
        LARGE_INTEGER zero;
        ULONG done = 0;
        char back[32];
        HGLOBAL hg = NULL;
        int i, same;

        begin( "stream C: Write/Seek/Read round trip" );
        hr = IStream_Write( streamC, msg, sizeof(msg), &done );
        if (hr == S_OK)
        {
            zero.QuadPart = 0;
            pos.QuadPart = ~(ULONGLONG)0;
            hr = IStream_Seek( streamC, zero, STREAM_SEEK_SET, &pos );
        }
        for (i = 0; i < (int)sizeof(back); i++) back[i] = 0;
        if (hr == S_OK) hr = IStream_Read( streamC, back, sizeof(msg), &done );
        same = (done == sizeof(msg));
        for (i = 0; same && i < (int)sizeof(msg); i++)
            if (back[i] != msg[i]) same = 0;
        out_hr( "hr", hr );
        out( " bytes=" );
        out( same ? "match" : "differ" );
        verdict( hr == S_OK && same, "round trip did not survive" );

        begin( "GetHGlobalFromStream(stream C)" );
        hr = GetHGlobalFromStream( streamC, &hg );
        out_hr( "hr", hr );
        verdict( hr == S_OK && hg != NULL, "no hglobal" );

        begin( "release stream C" );
        r1 = IStream_Release( streamC );
        out( "refs=" );
        out_dec( (LONG)r1 );
        verdict( r1 == 0, "did not drop to zero" );
    }

    /* ---- L6: idempotence ---- */
    begin( "L6 VariantClear again (already VT_EMPTY)" );
    hr = VariantClear( &v );
    out_hr( "hr", hr );
    out_vt( " vt", V_VT( &v ) );
    verdict( hr == S_OK && V_VT( &v ) == VT_EMPTY, "wrong result" );

    /* ---- L9: SAFEARRAY shape, NULL descriptor -- no-op pass-through ---- */
    begin( "L9 VariantClear(VT_ARRAY|VT_I4, parray==NULL)" );
    VariantInit( &v );
    V_VT( &v ) = VT_ARRAY | VT_I4;
    V_ARRAY( &v ) = NULL;
    hr = VariantClear( &v );
    out_hr( "hr", hr );
    out_vt( " vt", V_VT( &v ) );
    verdict( hr == S_OK && V_VT( &v ) == VT_EMPTY, "wrong result" );

    /* L8 and L10 (the refusal legs for an interface-bearing SAFEARRAY and a
     * non-NULL IRecordInfo) are NOT here: see the guest-only block below the
     * PASS/FAIL summary for why they cannot run on the native lane at all. */

    /* ---- L11: VT_RECORD, NULL pRecInfo -- no-op pass-through ---- */
    begin( "L11 VariantClear(VT_RECORD, pRecInfo==NULL)" );
    VariantInit( &v );
    V_VT( &v ) = VT_RECORD;
    V_RECORD( &v ) = NULL;
    V_RECORDINFO( &v ) = NULL;
    hr = VariantClear( &v );
    out_hr( "hr", hr );
    out_vt( " vt", V_VT( &v ) );
    verdict( hr == S_OK && V_VT( &v ) == VT_EMPTY, "wrong result" );

    begin( "CoUninitialize" );
    CoUninitialize();
    out( "returned" );
    verdict( TRUE, "" );

done:
    out( failures ? "variant_clear_smoke: FAIL " : "variant_clear_smoke: PASS " );
    out_dec( step - failures );
    out( "/" );
    out_dec( step );
    out( "\n" );

    /* ---- L7, DIVERGENT BY DESIGN: printed AFTER the summary and on its own
     * clearly marked line so the gate script can exclude it from the
     * byte-identical diff and check it against a PER-LANE expectation
     * instead.  Not counted in the failures/PASS tally above. */
    {
        fake_unknown fake = { &fake_vtbl, 1, 0 };
        VariantInit( &v );
        V_VT( &v ) = VT_UNKNOWN;
        V_UNKNOWN( &v ) = (IUnknown *)&fake;
        hr = VariantClear( &v );
        out( "variant_clear_smoke: L7 hr=0x" );
        out_hex( (ULONG)hr, 8 );
        out( " released=" );
        out_dec( fake.released );
        out( " vt=0x" );
        out_hex( (ULONG)V_VT( &v ), 4 );
        out( "\n" );
    }

#ifdef VARIANT_SMOKE_NO_CRT
    /* L8 and L10, GUEST-ONLY and unavoidably so: they exercise refusal paths
     * that exist ONLY in the wrapper (dlls/combase/syscom.c), because the
     * whole reason they exist is that a value crossing from the guest MIGHT
     * be a proxy.  Real Wine/Windows VariantClear has no FADF_* / pRecInfo
     * gate at all -- on native, it just does the (for these hand-rolled,
     * never-allocator-made descriptors, UNDEFINED) thing directly.  This was
     * not a guess: an earlier version of this probe ran the identical
     * descriptors through the native build and it corrupted its heap
     * (SafeArrayDestroy/CoTaskMemFree on a stack address) and crashed a few
     * steps later -- exactly the "destroying a hand-rolled one is UB on
     * Windows too" the design doc warns about.  So these two never compile
     * into the native binary at all; the gate checks them by reading THIS
     * build's own output only, never by diffing against a native run that
     * cannot safely attempt them. */
    {
        SAFEARRAY sa;
        sa.cDims = 1;
        sa.fFeatures = 0x200 /* FADF_UNKNOWN */;
        sa.cbElements = sizeof(void *);
        sa.cLocks = 0;
        sa.pvData = NULL;
        sa.rgsabound[0].cElements = 1;
        sa.rgsabound[0].lLbound = 0;
        VariantInit( &v );
        V_VT( &v ) = VT_ARRAY | VT_UNKNOWN;
        V_ARRAY( &v ) = &sa;
        hr = VariantClear( &v );
        out( "variant_clear_smoke: L8 hr=0x" );
        out_hex( (ULONG)hr, 8 );
        out( " vt=0x" );
        out_hex( (ULONG)V_VT( &v ), 4 );
        out( " fFeatures=0x" );
        out_hex( sa.fFeatures, 4 );
        out( " pvData=" );
        out( sa.pvData == NULL ? "NULL" : "TOUCHED" );
        out( "\n" );
    }
    {
        VariantInit( &v );
        V_VT( &v ) = VT_RECORD;
        V_RECORD( &v ) = (PVOID)(ULONG_PTR)0x1000;
        V_RECORDINFO( &v ) = (IRecordInfo *)(ULONG_PTR)0x2000;
        hr = VariantClear( &v );
        out( "variant_clear_smoke: L10 hr=0x" );
        out_hex( (ULONG)hr, 8 );
        out( " vt=0x" );
        out_hex( (ULONG)V_VT( &v ), 4 );
        out( " pRecInfo=" );
        out( V_RECORDINFO( &v ) == (IRecordInfo *)(ULONG_PTR)0x2000 ? "0x2000" : "CHANGED" );
        out( "\n" );
    }
#endif

    return failures ? 1 : 0;
}

#ifdef VARIANT_SMOKE_NO_CRT
void WINAPI variant_clear_smoke_entry( void )
{
    ExitProcess( (UINT)variant_clear_smoke_run() );
}
#else
int main( void )
{
    return variant_clear_smoke_run();
}
#endif
