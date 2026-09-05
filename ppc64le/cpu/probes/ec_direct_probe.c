/*
 * ec_direct_probe.c -- value checks for the EC DIRECT arm (fexbridge.h
 * "EC DIRECT calls"; dlls/ntdll/signal_ppc64.c ec_cell_fill): calls the
 * JIT now serves inline, each compared with an answer the JIT cannot
 * fake.
 *
 *   flat leaf   GetCurrentProcessId / GetCurrentThreadId against the TEB's
 *               ClientId, read straight from gs: by the guest;
 *   COM scalar  ID3D11Device::GetFeatureLevel == 11_0 (the level asked for);
 *               ID3D11DeviceContext::GetType == IMMEDIATE;
 *   COM pointer ID3D11Device::CheckFormatSupport(R8G8B8A8_UNORM, &bits):
 *               S_OK and the TEXTURE2D bit set;
 *   COM Map     Map(WRITE) a staging buffer through the context, write a
 *               pattern, Unmap, CopyResource to a second staging buffer,
 *               Map(READ) it back: the bytes must match.  Map's arguments
 *               are a proxy (unwrapped by the JIT), three UINTs (extended
 *               by the digest) and a pointer the body writes through.
 *
 * Under WINE_PPC64LE_EC_DIRECT_SABOTAGE=1 every direct-served RAX is
 * inverted, so the feature level, the type, the HRESULTs and the pid all
 * go wrong: the probe MUST FAIL.  Under WINE_PPC64LE_NO_EC_DIRECT=1 it
 * must PASS through the old path.  No CRT; kernel32 + d3d11.
 *
 * Copyright 2026 the ppc64le port authors
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */
#define COBJMACROS
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <winternl.h>
#include <d3d11.h>

void *memset( void *d, int c, size_t n ) { unsigned char *p = d; while (n--) *p++ = (unsigned char)c; return d; }
int memcmp( const void *a, const void *b, size_t n )
{ const unsigned char *p = a, *q = b; while (n--) { if (*p != *q) return *p - *q; p++; q++; } return 0; }

static void out( const char *s ) { DWORD n = 0, w; while (s[n]) n++; WriteFile( GetStdHandle( STD_OUTPUT_HANDLE ), s, n, &w, NULL ); }
static void out_hex( ULONG64 v ) { static const char h[] = "0123456789abcdef"; char b[17]; int i; for (i = 0; i < 16; i++) b[15 - i] = h[(v >> (4 * i)) & 15]; b[16] = 0; out( b ); }
static int failures;
static void check( BOOL ok, const char *what ) { out( "ec_direct_probe: " ); out( what ); out( ok ? ": ok\n" : ": FAIL\n" ); if (!ok) failures++; }

void WINAPI ed_entry( void )
{
    const D3D_FEATURE_LEVEL want_fl[] = { D3D_FEATURE_LEVEL_11_0 };
    D3D_FEATURE_LEVEL got = 0;
    ID3D11Device *dev = NULL;
    ID3D11DeviceContext *ctx = NULL;
    ID3D11Buffer *a = NULL, *b = NULL;
    D3D11_BUFFER_DESC bd;
    D3D11_MAPPED_SUBRESOURCE map;
    UINT support = 0, i, iter;
    HRESULT hr;
    BYTE pattern[256];
    TEB *teb = NtCurrentTeb();
    DWORD pid = GetCurrentProcessId(), tid = GetCurrentThreadId();

    check( pid == (DWORD)(ULONG_PTR)teb->ClientId.UniqueProcess, "GetCurrentProcessId == TEB ClientId.UniqueProcess" );
    check( tid == (DWORD)(ULONG_PTR)teb->ClientId.UniqueThread, "GetCurrentThreadId == TEB ClientId.UniqueThread" );
    /* repeated: the first call resolves through the trap, later ones are the arm */
    for (iter = 0, i = 0; iter < 1000; iter++) i += GetCurrentProcessId() == pid;
    check( i == 1000, "1000 x GetCurrentProcessId stable" );

    hr = D3D11CreateDevice( NULL, D3D_DRIVER_TYPE_HARDWARE, NULL, 0, want_fl, 1, D3D11_SDK_VERSION, &dev, &got, &ctx );
    if (FAILED(hr) || !dev || !ctx) { out( "ec_direct_probe: no device hr=0x" ); out_hex( (ULONG)hr ); out( "\n" ); ExitProcess( 2 ); }

    for (iter = 0, i = 0; iter < 1000; iter++) i += ID3D11Device_GetFeatureLevel( dev ) == D3D_FEATURE_LEVEL_11_0;
    check( i == 1000, "1000 x ID3D11Device::GetFeatureLevel == 11_0" );
    for (iter = 0, i = 0; iter < 1000; iter++) i += ID3D11DeviceContext_GetType( ctx ) == D3D11_DEVICE_CONTEXT_IMMEDIATE;
    check( i == 1000, "1000 x ID3D11DeviceContext::GetType == IMMEDIATE" );
    /* a pointer the body writes through: twice, because the FIRST call of
     * any slot resolves through the trap and only the second is the arm's */
    hr = ID3D11Device_CheckFormatSupport( dev, DXGI_FORMAT_R8G8B8A8_UNORM, &support );
    check( hr == S_OK && (support & D3D11_FORMAT_SUPPORT_TEXTURE2D), "CheckFormatSupport(R8G8B8A8_UNORM) S_OK + TEXTURE2D" );
    support = 0;
    hr = ID3D11Device_CheckFormatSupport( dev, DXGI_FORMAT_R8G8B8A8_UNORM, &support );
    check( hr == S_OK && (support & D3D11_FORMAT_SUPPORT_TEXTURE2D), "CheckFormatSupport again (the arm): S_OK + TEXTURE2D" );
    /* an interface argument the JIT must unwrap (the query proxy -> host),
     * a UINT on the stack it must extend, on a context whose ring is still
     * empty -- nothing journaled has run yet, so the arm may serve it */
    {
        D3D11_QUERY_DESC qd;
        ID3D11Query *q = NULL;
        UINT64 data = 0;
        HRESULT h1, h2;
        memset( &qd, 0, sizeof(qd) );
        qd.Query = D3D11_QUERY_EVENT;
        hr = ID3D11Device_CreateQuery( dev, &qd, &q );
        check( SUCCEEDED(hr) && q, "CreateQuery(EVENT)" );
        if (q)
        {
            h1 = ID3D11DeviceContext_GetData( ctx, (ID3D11Asynchronous *)q, &data, sizeof(data), D3D11_ASYNC_GETDATA_DONOTFLUSH );
            h2 = ID3D11DeviceContext_GetData( ctx, (ID3D11Asynchronous *)q, &data, sizeof(data), D3D11_ASYNC_GETDATA_DONOTFLUSH );
            check( h1 == h2, "GetData(query) twice on a quiet context: same HRESULT (the second is the arm's)" );
            ID3D11Query_Release( q );
        }
    }

    memset( &bd, 0, sizeof(bd) );
    bd.ByteWidth = 256; bd.Usage = D3D11_USAGE_STAGING;
    bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE | D3D11_CPU_ACCESS_READ;
    hr = ID3D11Device_CreateBuffer( dev, &bd, NULL, &a );
    check( SUCCEEDED(hr) && a, "CreateBuffer (staging a)" );
    hr = ID3D11Device_CreateBuffer( dev, &bd, NULL, &b );
    check( SUCCEEDED(hr) && b, "CreateBuffer (staging b)" );
    for (i = 0; i < 256; i++) pattern[i] = (BYTE)(i * 7 + 3);
    if (a && b)
    {
        hr = ID3D11DeviceContext_Map( ctx, (ID3D11Resource *)a, 0, D3D11_MAP_WRITE, 0, &map );
        check( hr == S_OK && map.pData, "Map(WRITE) through the context" );
        if (SUCCEEDED(hr) && map.pData)
        {
            for (i = 0; i < 256; i++) ((BYTE *)map.pData)[i] = pattern[i];
            ID3D11DeviceContext_Unmap( ctx, (ID3D11Resource *)a, 0 );
        }
        ID3D11DeviceContext_CopyResource( ctx, (ID3D11Resource *)b, (ID3D11Resource *)a );
        hr = ID3D11DeviceContext_Map( ctx, (ID3D11Resource *)b, 0, D3D11_MAP_READ, 0, &map );
        check( hr == S_OK && map.pData, "Map(READ) of the copy" );
        if (SUCCEEDED(hr) && map.pData)
        {
            check( !memcmp( map.pData, pattern, 256 ), "the 256 bytes round-tripped" );
            ID3D11DeviceContext_Unmap( ctx, (ID3D11Resource *)b, 0 );
        }
        ID3D11Buffer_Release( a );
        ID3D11Buffer_Release( b );
    }
    ID3D11DeviceContext_Release( ctx );
    ID3D11Device_Release( dev );
    out( failures ? "ec_direct_probe: FAIL\n" : "ec_direct_probe: PASS\n" );
    ExitProcess( failures ? 1 : 0 );
}
