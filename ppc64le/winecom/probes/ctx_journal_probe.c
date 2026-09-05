/*
 * ctx_journal_probe.c -- drive the D3D11 CONTEXT JOURNAL (journal_gen.h,
 * libs/winecom/winecom.c) through every record kind it has, and read every
 * effect back through the API so a wrong or missing replay shows up as a
 * wrong answer rather than a silent frame.
 *
 * Every journaled call below is a void state-setter recorded guest-side and
 * replayed at the next trap.  Each is followed by the trap that observes
 * it -- a Get* on the context, or a Map after a copy -- so the sequence is
 * record, trap, drain, replay, observe, and the observation is compared
 * with what was set:
 *
 *   JG_V scalars          IASetPrimitiveTopology -> IAGetPrimitiveTopology
 *   JG_V proxy            RSSetState -> RSGetState (same proxy pointer back)
 *   JG_A structs (24 B)   RSSetViewports(3) -> RSGetViewports, byte-compared
 *   JG_A structs (16 B)   RSSetScissorRects(4) -> RSGetScissorRects
 *   JG_A proxies          VSSetConstantBuffers(1..3) -> VSGetConstantBuffers
 *   three arrays, one cnt IASetVertexBuffers(2) -> IAGetVertexBuffers
 *   JG_P float[4]         OMSetBlendState -> OMGetBlendState
 *   proxy array + proxy   OMSetRenderTargets(1, &rtv, dsv=NULL) -> OMGetRenderTargets
 *   JG_P + stack args     ClearRenderTargetView + CopySubresourceRegion(pBox,
 *                         arguments 5..8 on the stack) + Map: the texels
 *   Unmap                 journaled, observed by the next Map succeeding
 *   a draw                Draw(3, 0) with no pipeline bound: must not crash
 *   two proxies           QI for ID3D11DeviceContext1, writes interleaved
 *                         through both proxies, last writer must win
 *
 * The clear colour and its 8-bit rounding are d3d11_smoke's (00 40 80 ff);
 * the second render target is cleared to (1, 0.5, 0.25, 1) = ff 80 40 ff
 * and copied into an 8x8 box at (8, 8) of the staging copy of the first,
 * so the readback has two known regions and the box arguments -- which
 * travel on the caller's stack -- decide where the second one lands.
 *
 * Prints one line per check and "ctx_journal_probe: PASS" or "FAIL <why>"
 * last; exit 0 on PASS, 1 on FAIL, 2 when no device could be created.
 * WINEEMUCOMJOURNALSABOTAGE=1 (record, never replay) MUST make this FAIL;
 * WINEEMUNOCOMJOURNAL=1 (every call traps) must keep it PASSing.
 *
 * No CRT: own entry (cj_entry), output via WriteFile, imports kernel32
 * and d3d11 only (see check-ctx-journal.sh).
 *
 * Copyright 2026 the ppc64le port authors
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#define COBJMACROS
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d11_1.h>

/* ---- no-CRT support ---- */

void *memset( void *d, int c, size_t n )
{
    unsigned char *p = d;
    while (n--) *p++ = (unsigned char)c;
    return d;
}
void *memcpy( void *d, const void *s, size_t n )
{
    unsigned char *p = d; const unsigned char *q = s;
    while (n--) *p++ = *q++;
    return d;
}
int memcmp( const void *a, const void *b, size_t n )
{
    const unsigned char *p = a, *q = b;
    while (n--) { if (*p != *q) return *p - *q; p++; q++; }
    return 0;
}

static void out( const char *s )
{
    DWORD n = 0, w;
    while (s[n]) n++;
    WriteFile( GetStdHandle( STD_OUTPUT_HANDLE ), s, n, &w, NULL );
}
static void out_hex( ULONG64 v, int digits )
{
    static const char hex[] = "0123456789abcdef";
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

static int failures;
static const char *first_fail;
static void check( BOOL ok, const char *what )
{
    out( "ctx_journal_probe: " );
    out( what );
    out( ok ? ": ok\n" : ": FAIL\n" );
    if (!ok)
    {
        failures++;
        if (!first_fail) first_fail = what;
    }
}

static DWORD fnv1a( DWORD hash, const BYTE *p, UINT n )
{
    UINT i;
    for (i = 0; i < n; i++) { hash ^= p[i]; hash *= 0x01000193u; }
    return hash;
}

static const GUID cj_IID_ID3D11Texture2D =
    { 0x6f15aaf2, 0xd208, 0x4e89, { 0x9a,0xb4,0x48,0x95,0x35,0xd3,0x4f,0x9c } };

static ID3D11Device *dev;
static ID3D11DeviceContext *ctx;

static ID3D11Buffer *make_buffer( UINT bytes, UINT bind )
{
    D3D11_BUFFER_DESC bd;
    ID3D11Buffer *b = NULL;
    memset( &bd, 0, sizeof(bd) );
    bd.ByteWidth = bytes;
    bd.Usage = D3D11_USAGE_DEFAULT;
    bd.BindFlags = bind;
    if (FAILED(ID3D11Device_CreateBuffer( dev, &bd, NULL, &b ))) return NULL;
    return b;
}

static ID3D11Texture2D *make_tex( UINT w, UINT h, D3D11_USAGE usage, UINT bind, UINT cpu )
{
    D3D11_TEXTURE2D_DESC td;
    ID3D11Texture2D *t = NULL;
    memset( &td, 0, sizeof(td) );
    td.Width = w; td.Height = h; td.MipLevels = 1; td.ArraySize = 1;
    td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    td.SampleDesc.Count = 1;
    td.Usage = usage; td.BindFlags = bind; td.CPUAccessFlags = cpu;
    if (FAILED(ID3D11Device_CreateTexture2D( dev, &td, NULL, &t ))) return NULL;
    return t;
}

static int run( void )
{
    const D3D_FEATURE_LEVEL want_fl[] = { D3D_FEATURE_LEVEL_11_0 };
    D3D_FEATURE_LEVEL got_fl = 0;
    ID3D11Texture2D *rt1 = NULL, *rt2 = NULL, *staging = NULL;
    ID3D11RenderTargetView *rtv1 = NULL, *rtv2 = NULL, *rtv_back = NULL;
    ID3D11DepthStencilView *dsv_back = (ID3D11DepthStencilView *)(ULONG_PTR)1;
    ID3D11RasterizerState *rs = NULL, *rs_back = NULL;
    ID3D11Buffer *cb[3] = { NULL }, *cb_back[3], *vb[2] = { NULL }, *vb_back[2];
    D3D11_VIEWPORT vp[3], vp_back[3];
    D3D11_RECT sc[4], sc_back[4];
    D3D11_RASTERIZER_DESC rd;
    D3D11_PRIMITIVE_TOPOLOGY topo = D3D11_PRIMITIVE_TOPOLOGY_LINESTRIP;
    UINT n, i, strides[2] = { 16, 32 }, offsets[2] = { 0, 64 }, strides_back[2], offsets_back[2];
    FLOAT factor[4] = { 0.125f, 0.25f, 0.5f, 0.75f }, factor_back[4];
    UINT mask_back = 0;
    ID3D11BlendState *bs_back = (ID3D11BlendState *)(ULONG_PTR)1;
    const FLOAT c1[4] = { 0.0f, 0.25f, 0.5f, 1.0f };
    const FLOAT c2[4] = { 1.0f, 0.5f, 0.25f, 1.0f };
    D3D11_BOX box;
    D3D11_MAPPED_SUBRESOURCE map;
    HRESULT hr;
    DWORD csum = 0x811c9dc5u;
    UINT bad_a = 0, bad_b = 0;

    out( "ctx_journal_probe: start\n" );
    hr = D3D11CreateDevice( NULL, D3D_DRIVER_TYPE_HARDWARE, NULL, 0, want_fl, 1,
                            D3D11_SDK_VERSION, &dev, &got_fl, &ctx );
    if (FAILED(hr) || !dev || !ctx)
    {
        out( "ctx_journal_probe: no device (hr=0x" ); out_hex( (ULONG)hr, 8 ); out( ")\n" );
        return 2;
    }

    /* ---- scalars: topology ---- */
    ID3D11DeviceContext_IASetPrimitiveTopology( ctx, D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP );
    ID3D11DeviceContext_IAGetPrimitiveTopology( ctx, &topo );
    check( topo == D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP, "IASetPrimitiveTopology -> IAGetPrimitiveTopology" );

    /* ---- a proxy by value: rasterizer state ---- */
    memset( &rd, 0, sizeof(rd) );
    rd.FillMode = D3D11_FILL_SOLID; rd.CullMode = D3D11_CULL_FRONT; rd.DepthClipEnable = TRUE;
    hr = ID3D11Device_CreateRasterizerState( dev, &rd, &rs );
    check( SUCCEEDED(hr) && rs, "CreateRasterizerState" );
    ID3D11DeviceContext_RSSetState( ctx, rs );
    ID3D11DeviceContext_RSGetState( ctx, &rs_back );
    check( rs_back == rs, "RSSetState -> RSGetState returns the same proxy" );
    if (rs_back) ID3D11RasterizerState_Release( rs_back );

    /* ---- an array of 24-byte structs, count in a register ---- */
    for (i = 0; i < 3; i++)
    {
        vp[i].TopLeftX = 1.0f + i; vp[i].TopLeftY = 2.0f * i; vp[i].Width = 64.0f - i;
        vp[i].Height = 32.0f + i; vp[i].MinDepth = 0.0f; vp[i].MaxDepth = 1.0f - 0.125f * i;
    }
    ID3D11DeviceContext_RSSetViewports( ctx, 3, vp );
    n = 3; memset( vp_back, 0, sizeof(vp_back) );
    ID3D11DeviceContext_RSGetViewports( ctx, &n, vp_back );
    check( n == 3 && !memcmp( vp, vp_back, sizeof(vp) ), "RSSetViewports(3) -> RSGetViewports byte-identical" );

    /* ---- an array of 16-byte structs ---- */
    for (i = 0; i < 4; i++) { sc[i].left = i; sc[i].top = 10 + i; sc[i].right = 50 + i; sc[i].bottom = 60 + 2 * i; }
    ID3D11DeviceContext_RSSetScissorRects( ctx, 4, sc );
    n = 4; memset( sc_back, 0, sizeof(sc_back) );
    ID3D11DeviceContext_RSGetScissorRects( ctx, &n, sc_back );
    check( n == 4 && !memcmp( sc, sc_back, sizeof(sc) ), "RSSetScissorRects(4) -> RSGetScissorRects byte-identical" );

    /* ---- an array of proxies ---- */
    for (i = 0; i < 3; i++) cb[i] = make_buffer( 256, D3D11_BIND_CONSTANT_BUFFER );
    check( cb[0] && cb[1] && cb[2], "CreateBuffer x3 (constant buffers)" );
    ID3D11DeviceContext_VSSetConstantBuffers( ctx, 1, 3, cb );
    memset( cb_back, 0, sizeof(cb_back) );
    ID3D11DeviceContext_VSGetConstantBuffers( ctx, 1, 3, cb_back );
    check( cb_back[0] == cb[0] && cb_back[1] == cb[1] && cb_back[2] == cb[2],
           "VSSetConstantBuffers(1,3) -> VSGetConstantBuffers same proxies" );
    for (i = 0; i < 3; i++) if (cb_back[i]) ID3D11Buffer_Release( cb_back[i] );

    /* ---- three arrays sharing one count ---- */
    vb[0] = make_buffer( 1024, D3D11_BIND_VERTEX_BUFFER );
    vb[1] = make_buffer( 2048, D3D11_BIND_VERTEX_BUFFER );
    check( vb[0] && vb[1], "CreateBuffer x2 (vertex buffers)" );
    ID3D11DeviceContext_IASetVertexBuffers( ctx, 0, 2, vb, strides, offsets );
    memset( vb_back, 0, sizeof(vb_back) ); memset( strides_back, 0, sizeof(strides_back) );
    memset( offsets_back, 0, sizeof(offsets_back) );
    ID3D11DeviceContext_IAGetVertexBuffers( ctx, 0, 2, vb_back, strides_back, offsets_back );
    check( vb_back[0] == vb[0] && vb_back[1] == vb[1] &&
           strides_back[0] == 16 && strides_back[1] == 32 &&
           offsets_back[0] == 0 && offsets_back[1] == 64,
           "IASetVertexBuffers(2) -> IAGetVertexBuffers buffers, strides, offsets" );
    for (i = 0; i < 2; i++) if (vb_back[i]) ID3D11Buffer_Release( vb_back[i] );

    /* ---- a pointer to 16 bytes: the blend factor ---- */
    ID3D11DeviceContext_OMSetBlendState( ctx, NULL, factor, 0x5 );
    memset( factor_back, 0, sizeof(factor_back) );
    ID3D11DeviceContext_OMGetBlendState( ctx, &bs_back, factor_back, &mask_back );
    check( bs_back == NULL && mask_back == 0x5 && !memcmp( factor, factor_back, sizeof(factor) ),
           "OMSetBlendState(NULL, factor, 5) -> OMGetBlendState" );

    /* ---- render targets, clears, a box copy through the stack, readback ---- */
    rt1 = make_tex( 64, 64, D3D11_USAGE_DEFAULT, D3D11_BIND_RENDER_TARGET, 0 );
    rt2 = make_tex( 64, 64, D3D11_USAGE_DEFAULT, D3D11_BIND_RENDER_TARGET, 0 );
    staging = make_tex( 64, 64, D3D11_USAGE_STAGING, 0, D3D11_CPU_ACCESS_READ );
    check( rt1 && rt2 && staging, "CreateTexture2D x3" );
    if (!rt1 || !rt2 || !staging) goto done;
    hr = ID3D11Device_CreateRenderTargetView( dev, (ID3D11Resource *)rt1, NULL, &rtv1 );
    check( SUCCEEDED(hr) && rtv1, "CreateRenderTargetView (rt1)" );
    hr = ID3D11Device_CreateRenderTargetView( dev, (ID3D11Resource *)rt2, NULL, &rtv2 );
    check( SUCCEEDED(hr) && rtv2, "CreateRenderTargetView (rt2)" );
    if (!rtv1 || !rtv2) goto done;

    ID3D11DeviceContext_OMSetRenderTargets( ctx, 1, &rtv1, NULL );
    ID3D11DeviceContext_OMGetRenderTargets( ctx, 1, &rtv_back, &dsv_back );
    check( rtv_back == rtv1 && dsv_back == NULL, "OMSetRenderTargets(1, rtv1, NULL) -> OMGetRenderTargets" );
    if (rtv_back) ID3D11RenderTargetView_Release( rtv_back );

    /* a draw with nothing bound but a render target: DXVK skips it, and the
     * point is that the recorded call replays without harm */
    ID3D11DeviceContext_Draw( ctx, 3, 0 );

    ID3D11DeviceContext_ClearRenderTargetView( ctx, rtv1, c1 );
    ID3D11DeviceContext_ClearRenderTargetView( ctx, rtv2, c2 );
    ID3D11DeviceContext_CopyResource( ctx, (ID3D11Resource *)staging, (ID3D11Resource *)rt1 );
    box.left = 0; box.top = 0; box.front = 0; box.right = 8; box.bottom = 8; box.back = 1;
    ID3D11DeviceContext_CopySubresourceRegion( ctx, (ID3D11Resource *)staging, 0, 8, 8, 0,
                                               (ID3D11Resource *)rt2, 0, &box );

    hr = ID3D11DeviceContext_Map( ctx, (ID3D11Resource *)staging, 0, D3D11_MAP_READ, 0, &map );
    check( SUCCEEDED(hr) && map.pData, "Map(READ) after clear + copy + box copy" );
    if (SUCCEEDED(hr) && map.pData)
    {
        UINT x, y;
        for (y = 0; y < 64; y++)
        {
            const BYTE *row = (const BYTE *)map.pData + y * map.RowPitch;
            for (x = 0; x < 64; x++)
            {
                const BYTE *t = row + 4 * x;
                BOOL in_box = x >= 8 && x < 16 && y >= 8 && y < 16;
                csum = fnv1a( csum, t, 4 );
                if (in_box)
                {
                    if (t[0] != 0xff || t[1] != 0x80 || t[2] != 0x40 || t[3] != 0xff) bad_b++;
                }
                else if (t[0] != 0x00 || t[1] != 0x40 || t[2] != 0x80 || t[3] != 0xff) bad_a++;
            }
        }
        ID3D11DeviceContext_Unmap( ctx, (ID3D11Resource *)staging, 0 );
        out( "ctx_journal_probe: readback fnv1a=0x" ); out_hex( csum, 8 );
        out( " outside-box mismatches=" ); out_dec( bad_a );
        out( " in-box mismatches=" ); out_dec( bad_b ); out( "\n" );
        check( bad_a == 0, "clear colour 00 40 80 ff everywhere outside the box" );
        check( bad_b == 0, "box copy ff 80 40 ff at (8,8)-(16,16): stack arguments and D3D11_BOX blob" );

        /* Unmap was journaled; a second Map must succeed only if it replayed */
        hr = ID3D11DeviceContext_Map( ctx, (ID3D11Resource *)staging, 0, D3D11_MAP_READ, 0, &map );
        check( SUCCEEDED(hr), "Map again after a journaled Unmap" );
        if (SUCCEEDED(hr)) ID3D11DeviceContext_Unmap( ctx, (ID3D11Resource *)staging, 0 );
    }

    /* ---- TWO PROXIES OF ONE CONTEXT, calls interleaved through both.
     * A QueryInterface for ID3D11DeviceContext1 wraps a second proxy of the
     * same DXVK object; its calls must land in the SAME ring as the base
     * proxy's, in issue order.  Topology is the order-sensitive observable:
     * last writer wins, and the writers alternate proxies. */
    {
        ID3D11DeviceContext1 *ctx1 = NULL;
        static const GUID cj_IID_ID3D11DeviceContext1 =
            { 0xbb2c6faa, 0xb5fb, 0x4082, { 0x8e,0x6b,0x38,0x8b,0x8c,0xfa,0x90,0xe1 } };
        hr = ID3D11DeviceContext_QueryInterface( ctx, &cj_IID_ID3D11DeviceContext1, (void **)&ctx1 );
        check( SUCCEEDED(hr) && ctx1 && (void *)ctx1 != (void *)ctx,
               "QueryInterface(ID3D11DeviceContext1) gives a second proxy" );
        if (SUCCEEDED(hr) && ctx1)
        {
            UINT first[1] = { 0 }, num[1] = { 16 };
            ID3D11DeviceContext_IASetPrimitiveTopology( ctx, D3D11_PRIMITIVE_TOPOLOGY_POINTLIST );
            ID3D11DeviceContext1_VSSetConstantBuffers1( ctx1, 0, 1, cb, first, num );
            ID3D11DeviceContext1_IASetPrimitiveTopology( ctx1, D3D11_PRIMITIVE_TOPOLOGY_LINELIST );
            ID3D11DeviceContext_RSSetState( ctx, rs );
            ID3D11DeviceContext_IASetPrimitiveTopology( ctx, D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST );
            ID3D11DeviceContext1_RSSetState( ctx1, NULL );
            ID3D11DeviceContext1_IASetPrimitiveTopology( ctx1, D3D11_PRIMITIVE_TOPOLOGY_LINESTRIP );
            topo = 0;
            ID3D11DeviceContext_IAGetPrimitiveTopology( ctx, &topo );
            check( topo == D3D11_PRIMITIVE_TOPOLOGY_LINESTRIP,
                   "interleaved ctx/ctx1 topology writes: last writer (ctx1) wins" );
            rs_back = (ID3D11RasterizerState *)(ULONG_PTR)1;
            ID3D11DeviceContext1_RSGetState( ctx1, &rs_back );
            check( rs_back == NULL, "interleaved ctx/ctx1 RSSetState: last writer (ctx1, NULL) wins" );
            memset( cb_back, 0, sizeof(cb_back) );
            ID3D11DeviceContext_VSGetConstantBuffers( ctx, 0, 1, cb_back );
            check( cb_back[0] == cb[0], "VSSetConstantBuffers1 through ctx1 -> VSGetConstantBuffers through ctx" );
            if (cb_back[0]) ID3D11Buffer_Release( cb_back[0] );
            /* and the other way round: base proxy last */
            ID3D11DeviceContext1_IASetPrimitiveTopology( ctx1, D3D11_PRIMITIVE_TOPOLOGY_LINELIST );
            ID3D11DeviceContext_IASetPrimitiveTopology( ctx, D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP );
            topo = 0;
            ID3D11DeviceContext1_IAGetPrimitiveTopology( ctx1, &topo );
            check( topo == D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP,
                   "interleaved ctx1/ctx topology writes: last writer (ctx) wins" );
            ID3D11DeviceContext1_Release( ctx1 );
        }
    }

    /* ---- ClearState, then a Get shows the defaults ---- */
    ID3D11DeviceContext_ClearState( ctx );
    rs_back = (ID3D11RasterizerState *)(ULONG_PTR)1;
    ID3D11DeviceContext_RSGetState( ctx, &rs_back );
    check( rs_back == NULL, "ClearState -> RSGetState NULL" );

done:
    if (rtv1) ID3D11RenderTargetView_Release( rtv1 );
    if (rtv2) ID3D11RenderTargetView_Release( rtv2 );
    if (rt1) ID3D11Texture2D_Release( rt1 );
    if (rt2) ID3D11Texture2D_Release( rt2 );
    if (staging) ID3D11Texture2D_Release( staging );
    if (rs) ID3D11RasterizerState_Release( rs );
    for (i = 0; i < 3; i++) if (cb[i]) ID3D11Buffer_Release( cb[i] );
    for (i = 0; i < 2; i++) if (vb[i]) ID3D11Buffer_Release( vb[i] );
    ID3D11DeviceContext_Release( ctx );
    ID3D11Device_Release( dev );

    if (failures)
    {
        out( "ctx_journal_probe: FAIL " ); out( first_fail ); out( "\n" );
        return 1;
    }
    out( "ctx_journal_probe: PASS\n" );
    return 0;
}

void WINAPI cj_entry( void )
{
    ExitProcess( (UINT)run() );
}
