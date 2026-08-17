/*
 * com_smoke -- the system-COM runtime gate.
 *
 * ONE source, built TWICE and run twice: as a native ppc64 Windows PE and as
 * an x86-64 guest PE.  The two runs must print byte-identical output.  That
 * is the whole point: everything this program prints is a value Wine's own
 * COM implementation computed, so the guest run agreeing with the native run
 * means the guest reached the same implementation through the winecom proxy
 * runtime with nothing lost or mistranslated on the way.
 *
 * WHAT IT EXERCISES.  CoCreateInstance( CLSID_FileMoniker ) is served
 * in-process by Wine's own ole32 -- no out-of-process server, no device, no
 * registry beyond what wineboot writes -- and it vends an IMoniker, which the
 * system-COM roster covers with a full marshal plan (23 slots, no refusals
 * except ParseDisplayName).  The methods called below were chosen because
 * each returns a value that is CHECKABLE rather than merely non-crashing:
 * a class id, a moniker-system constant, a hash, a refcount.  "It started"
 * is exactly the class of result this file exists not to accept.
 *
 * The guest run goes through:
 *
 *   guest ole32.dll thunk :: CoCreateInstance   (spec2thunk GUEST-IMPL)
 *     -> native combase __wine_guest_CoCreateInstance      (dlls/combase/syscom.c)
 *        -> Wine's real CoCreateInstance, then winecom_wrap_out_iface
 *           -> a com_proxy whose vtable is the guest module's stub array
 *   guest calls proxy->lpVtbl->Hash        -> trap
 *     -> ntdll find_guest_com_target -> __wine_com_dispatch -> winecom_dispatch
 *        -> the native IMoniker vtable slot
 *
 * NO C RUNTIME on the guest side (-DCOM_SMOKE_NO_CRT): the program formats
 * its own output and writes it with WriteFile.  A CRT would add a second
 * variable to a test whose whole value is that only one thing is under test,
 * and printf's own %x is not the thing being measured.
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

/* Spelled out here rather than linked from libuuid: the guest build has no
 * Wine import libraries at all, and a GUID both builds compile from the same
 * bytes cannot differ between them. */
static const GUID smoke_CLSID_FileMoniker =
    { 0x00000303, 0x0000, 0x0000, { 0xc0,0x00,0x00,0x00,0x00,0x00,0x00,0x46 } };
static const GUID smoke_IID_IUnknown =
    { 0x00000000, 0x0000, 0x0000, { 0xc0,0x00,0x00,0x00,0x00,0x00,0x00,0x46 } };
static const GUID smoke_IID_IMoniker =
    { 0x0000000f, 0x0000, 0x0000, { 0xc0,0x00,0x00,0x00,0x00,0x00,0x00,0x46 } };
static const GUID smoke_IID_IPersistStream =
    { 0x00000109, 0x0000, 0x0000, { 0xc0,0x00,0x00,0x00,0x00,0x00,0x00,0x46 } };
static const GUID smoke_IID_IClassFactory =
    { 0x00000001, 0x0000, 0x0000, { 0xc0,0x00,0x00,0x00,0x00,0x00,0x00,0x46 } };

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

static void out_guid( const GUID *g )
{
    int i;

    out( "{" );
    out_hex( g->Data1, 8 );
    out( "-" );
    out_hex( g->Data2, 4 );
    out( "-" );
    out_hex( g->Data3, 4 );
    out( "-" );
    for (i = 0; i < 8; i++)
    {
        if (i == 2) out( "-" );
        out_hex( g->Data4[i], 2 );
    }
    out( "}" );
}

static BOOL guid_eq( const GUID *a, const GUID *b )
{
    const BYTE *p = (const BYTE *)a, *q = (const BYTE *)b;
    int i;

    for (i = 0; i < (int)sizeof(GUID); i++) if (p[i] != q[i]) return FALSE;
    return TRUE;
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

static const char msg[] = "com_smoke!";

static int com_smoke_run( void )
{
    IPersistStream *ps = NULL;
    IClassFactory *cf = NULL;
    IMoniker *mk = NULL;
    IStream *stm = NULL;
    ULONG r1 = 0, r2 = 0;
    DWORD mksys = 0, hash = 0;
    CLSID clsid;
    HRESULT hr;

    out( "com_smoke: start\n" );

    begin( "CoInitializeEx(APARTMENTTHREADED)" );
    hr = CoInitializeEx( NULL, COINIT_APARTMENTTHREADED );
    out_hr( "hr", hr );
    verdict( hr == S_OK, "not S_OK" );
    if (hr != S_OK) goto done;

    begin( "CoCreateInstance(CLSID_FileMoniker, IID_IMoniker)" );
    hr = CoCreateInstance( &smoke_CLSID_FileMoniker, NULL, CLSCTX_INPROC_SERVER,
                           &smoke_IID_IMoniker, (void **)&mk );
    out_hr( "hr", hr );
    verdict( hr == S_OK && mk != NULL, "no interface" );
    if (hr != S_OK || !mk) goto uninit;

    begin( "IMoniker::IsSystemMoniker" );
    hr = IMoniker_IsSystemMoniker( mk, &mksys );
    out_hr( "hr", hr );
    out( " mksys=" );
    out_dec( mksys );
    verdict( hr == S_OK && mksys == MKSYS_FILEMONIKER,
             "not MKSYS_FILEMONIKER" );

    begin( "IPersist::GetClassID" );
    clsid = smoke_IID_IUnknown;              /* a value it must overwrite */
    hr = IMoniker_GetClassID( mk, &clsid );
    out_hr( "hr", hr );
    out( " clsid=" );
    out_guid( &clsid );
    verdict( hr == S_OK && guid_eq( &clsid, &smoke_CLSID_FileMoniker ),
             "not CLSID_FileMoniker" );

    begin( "IMoniker::Hash" );
    hr = IMoniker_Hash( mk, &hash );
    out_hr( "hr", hr );
    out( " hash=0x" );
    out_hex( hash, 8 );
    verdict( hr == S_OK, "not S_OK" );

    begin( "IUnknown::AddRef/Release" );
    r1 = IMoniker_AddRef( mk );
    r2 = IMoniker_Release( mk );
    out( "refs=" );
    out_dec( r1 );
    out( "/" );
    out_dec( r2 );
    verdict( r1 == 2 && r2 == 1, "refcount does not round-trip" );

    begin( "IUnknown::QueryInterface(IID_IPersistStream)" );
    hr = IMoniker_QueryInterface( mk, &smoke_IID_IPersistStream, (void **)&ps );
    out_hr( "hr", hr );
    verdict( hr == S_OK && ps != NULL, "no interface" );

    if (ps)
    {
        begin( "IPersist::GetClassID through the QI'd IPersistStream" );
        clsid = smoke_IID_IUnknown;
        hr = IPersistStream_GetClassID( ps, &clsid );
        out_hr( "hr", hr );
        out( " clsid=" );
        out_guid( &clsid );
        verdict( hr == S_OK && guid_eq( &clsid, &smoke_CLSID_FileMoniker ),
                 "not CLSID_FileMoniker" );

        begin( "IPersistStream::IsDirty" );
        hr = IPersistStream_IsDirty( ps );
        out_hr( "hr", hr );
        verdict( hr == S_OK || hr == S_FALSE, "neither S_OK nor S_FALSE" );

        begin( "IUnknown::Release the QI'd IPersistStream" );
        IPersistStream_Release( ps );
        out( "released" );
        verdict( TRUE, "" );
    }

    begin( "IUnknown::Release the moniker" );
    r1 = IMoniker_Release( mk );
    out( "refs=" );
    out_dec( r1 );
    verdict( r1 == 0, "last reference did not drop to zero" );

    /* The second vending wrapper, and the only path that reaches
     * winecom_dispatch's riid/void** out-parameter pair: CoCreateInstance
     * wraps its result in the flat wrapper, IClassFactory::CreateInstance
     * wraps it inside the dispatch loop, and those are different code. */
    begin( "CoGetClassObject(CLSID_FileMoniker, IID_IClassFactory)" );
    hr = CoGetClassObject( &smoke_CLSID_FileMoniker, CLSCTX_INPROC_SERVER, NULL,
                           &smoke_IID_IClassFactory, (void **)&cf );
    out_hr( "hr", hr );
    verdict( hr == S_OK && cf != NULL, "no class object" );

    if (cf)
    {
        begin( "IClassFactory::CreateInstance(NULL, IID_IMoniker)" );
        hr = IClassFactory_CreateInstance( cf, NULL, &smoke_IID_IMoniker,
                                           (void **)&mk );
        out_hr( "hr", hr );
        verdict( hr == S_OK && mk != NULL, "no interface" );

        if (mk)
        {
            begin( "IMoniker::IsSystemMoniker on the class-object instance" );
            mksys = 0;
            hr = IMoniker_IsSystemMoniker( mk, &mksys );
            out_hr( "hr", hr );
            out( " mksys=" );
            out_dec( mksys );
            verdict( hr == S_OK && mksys == MKSYS_FILEMONIKER,
                     "not MKSYS_FILEMONIKER" );
            IMoniker_Release( mk );
            mk = NULL;
        }
        IClassFactory_Release( cf );
    }

    /* The third vending wrapper: a statically typed out-interface, wrapped by
     * __wine_com_wrap_static rather than by IID lookup.  The round trip below
     * is the only step whose answer comes back as BYTES the guest wrote. */
    begin( "CreateStreamOnHGlobal" );
    hr = CreateStreamOnHGlobal( NULL, TRUE, &stm );
    out_hr( "hr", hr );
    verdict( hr == S_OK && stm != NULL, "no stream" );

    if (stm)
    {
        ULARGE_INTEGER pos;
        LARGE_INTEGER zero;
        ULONG done = 0;
        char back[16];
        STATSTG st;
        int i, same;

        begin( "IStream::Write" );
        hr = IStream_Write( stm, msg, sizeof(msg), &done );
        out_hr( "hr", hr );
        out( " written=" );
        out_dec( done );
        verdict( hr == S_OK && done == sizeof(msg), "short write" );

        begin( "IStream::Seek(0, STREAM_SEEK_SET)" );
        zero.QuadPart = 0;
        pos.QuadPart = ~(ULONGLONG)0;
        hr = IStream_Seek( stm, zero, STREAM_SEEK_SET, &pos );
        out_hr( "hr", hr );
        out( " pos=" );
        out_dec( (ULONG)pos.QuadPart );
        verdict( hr == S_OK && pos.QuadPart == 0, "did not rewind" );

        begin( "IStream::Read" );
        done = 0;
        for (i = 0; i < (int)sizeof(back); i++) back[i] = 0;
        hr = IStream_Read( stm, back, sizeof(msg), &done );
        same = (done == sizeof(msg));
        for (i = 0; same && i < (int)sizeof(msg); i++)
            if (back[i] != msg[i]) same = 0;
        out_hr( "hr", hr );
        out( " read=" );
        out_dec( done );
        out( " bytes=" );
        out( same ? "match" : "differ" );
        verdict( hr == S_OK && same, "the bytes did not survive the round trip" );

        begin( "IStream::Stat(STATFLAG_NONAME)" );
        st.cbSize.QuadPart = ~(ULONGLONG)0;
        hr = IStream_Stat( stm, &st, STATFLAG_NONAME );
        out_hr( "hr", hr );
        out( " cbSize=" );
        out_dec( (ULONG)st.cbSize.QuadPart );
        verdict( hr == S_OK && st.cbSize.QuadPart == sizeof(msg),
                 "wrong stream size" );

        begin( "IUnknown::Release the stream" );
        r1 = IStream_Release( stm );
        out( "refs=" );
        out_dec( r1 );
        verdict( r1 == 0, "last reference did not drop to zero" );
    }

uninit:
    begin( "CoUninitialize" );
    CoUninitialize();
    out( "returned" );
    verdict( TRUE, "" );

done:
    out( failures ? "com_smoke: FAIL " : "com_smoke: PASS " );
    out_dec( (ULONG)(step - failures) );
    out( "/" );
    out_dec( (ULONG)step );
    out( "\n" );
    return failures ? 1 : 0;
}

#ifdef COM_SMOKE_NO_CRT
/* The guest build has no C runtime: this IS the image entry point. */
void WINAPI com_smoke_entry( void )
{
    ExitProcess( (UINT)com_smoke_run() );
}
#else
int main( void )
{
    return com_smoke_run();
}
#endif
