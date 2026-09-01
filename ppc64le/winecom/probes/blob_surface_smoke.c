/* blob_surface_smoke.c -- the d3dcompiler blob surface, driven end-to-end.
 *
 * Guest-only observer for ppc64le/winecom/check-blob-surface.sh: calls
 * D3DCompile through the guest thunk (the wrapper __wine_guest_D3DCompile on
 * the native side), then reads the compiled bytes back THROUGH THE PROXY'S
 * VTABLE -- GetBufferPointer is slot 3, GetBufferSize slot 4 -- which is the
 * exact call that executed ppc64 bytes as x86-64 before the surface existed
 * (the Witcher 3's save-load crash).  A second, broken compile proves the
 * ERROR blob is wrapped too: the compiler writes it on FAILURE, which is why
 * the wrapper must not gate the wrap on SUCCEEDED(hr).
 *
 * No CRT: raw vtable calls through explicit slot indices, output via
 * WriteFile, exit via ExitProcess.  Fields, one per line:
 *   compile_hr=<hex>  code_magic=<4 chars|none>  code_size=<dec>
 *   err_hr=<hex>      err_blob=<text|null>       released=<n>
 */

typedef unsigned int UINT;
typedef unsigned long long UINT64;
typedef int BOOL;
typedef unsigned int DWORD;
typedef void *HANDLE;
typedef long HRESULT;
typedef unsigned long long SIZE_T_;

#define STD_OUTPUT_HANDLE ((DWORD)-11)

HANDLE __stdcall GetStdHandle( DWORD which );
BOOL __stdcall WriteFile( HANDLE h, const void *buf, DWORD n, DWORD *written, void *ov );
__attribute__((noreturn)) void __stdcall ExitProcess( UINT code );

HRESULT __stdcall D3DCompile( const void *data, SIZE_T_ size, const char *filename,
                              const void *defines, void *include,
                              const char *entrypoint, const char *target,
                              UINT flags1, UINT flags2, void **shader, void **errors );

static void put( const char *s )
{
    DWORD n = 0, w;
    while (s[n]) n++;
    WriteFile( GetStdHandle( STD_OUTPUT_HANDLE ), s, n, &w, 0 );
}

static void put_hex( const char *k, UINT v )
{
    char b[64]; int i = 0, j;
    while (k[i]) { b[i] = k[i]; i++; }
    b[i++] = '=';
    b[i++] = '0'; b[i++] = 'x';
    for (j = 7; j >= 0; j--) b[i++] = "0123456789abcdef"[(v >> (j * 4)) & 15];
    b[i++] = '\n'; b[i] = 0;
    put( b );
}

static void put_dec( const char *k, UINT64 v )
{
    char b[64], d[24]; int i = 0, n = 0;
    while (k[i]) { b[i] = k[i]; i++; }
    b[i++] = '=';
    if (!v) d[n++] = '0';
    while (v) { d[n++] = '0' + (v % 10); v /= 10; }
    while (n) b[i++] = d[--n];
    b[i++] = '\n'; b[i] = 0;
    put( b );
}

/* vtable slot call: obj is IUnknown-shaped; the blob methods take `this`
 * only.  Release is slot 2. */
typedef UINT64 (__stdcall *slot0_fn)( void * );

static UINT64 vcall0( void *obj, int slot )
{
    return ((slot0_fn *)*(void **)obj)[slot]( obj );
}

static const char good_src[] =
    "float4 main() : SV_Target { return float4(1, 0, 0, 1); }";
static const char bad_src[] =
    "float4 main() : SV_Target { return does_not_exist; }";

void com_lever_entry(void)
{
    void *code = (void *)(UINT64)0x5151515151515151ull;   /* residue sentinel */
    void *errs = (void *)(UINT64)0x5151515151515151ull;
    HRESULT hr;
    UINT released = 0;

    /* leg 1: a good compile -- the code blob comes back and reads DXBC */
    hr = D3DCompile( good_src, sizeof(good_src) - 1, "smoke.hlsl", 0, 0,
                     "main", "ps_5_0", 0, 0, &code, &errs );
    put_hex( "compile_hr", (UINT)hr );
    if (code && code != (void *)(UINT64)0x5151515151515151ull)
    {
        const char *p = (const char *)(UINT64)vcall0( code, 3 );  /* GetBufferPointer */
        UINT64 sz = vcall0( code, 4 );                            /* GetBufferSize */
        char magic[16] = "code_magic=????\n";

        if (p && sz >= 4)
        {
            magic[11] = p[0]; magic[12] = p[1];
            magic[13] = p[2]; magic[14] = p[3];
        }
        put( magic );
        put_dec( "code_size", sz );
        vcall0( code, 2 ); released++;                            /* Release */
    }
    else put( code ? "code_magic=residue\n" : "code_magic=none\n" );
    if (errs && errs != (void *)(UINT64)0x5151515151515151ull)
    { vcall0( errs, 2 ); released++; }

    /* leg 2: a broken compile -- the ERROR blob must come back wrapped */
    code = 0; errs = 0;
    hr = D3DCompile( bad_src, sizeof(bad_src) - 1, "smoke.hlsl", 0, 0,
                     "main", "ps_5_0", 0, 0, &code, &errs );
    put_hex( "err_hr", (UINT)hr );
    if (errs)
    {
        const char *p = (const char *)(UINT64)vcall0( errs, 3 );
        UINT64 sz = vcall0( errs, 4 );

        put( (p && sz > 0 && p[0] >= 32 && p[0] < 127) ? "err_blob=text\n"
                                                       : "err_blob=odd\n" );
        vcall0( errs, 2 ); released++;
    }
    else put( "err_blob=null\n" );
    if (code) { vcall0( code, 2 ); released++; }

    put_dec( "released", released );
    put( "blob_surface_smoke: done\n" );
    ExitProcess( 0 );
}
