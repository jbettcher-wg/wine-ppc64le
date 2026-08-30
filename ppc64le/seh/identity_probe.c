/*
 * identity_probe.c -- ad hoc guest probe for the pointer-identity audit
 * (ppc64le/docs/sessions/2026-08-29/pointer-identity-audit.md).  NOT a
 * committed gate (no matching check-*.sh): built and run by hand, once, to
 * settle whether the new GetClassLongPtr(GCLP_WNDPROC), GetClassInfoW and
 * SetUnhandledExceptionFilter unwraps in dlls/ntdll/signal_ppc64.c actually
 * return what a guest registered, the same way check-guest-callbacks.sh
 * settled it for GWLP_WNDPROC.  Same construct as guest_callbacks.c: no CRT,
 * clang -target x86_64-windows-gnu, the image entry point IS the program.
 */
#include <windows.h>

static void out( const char *s )
{
    DWORD n = 0, written;
    while (s[n]) n++;
    WriteFile( GetStdHandle( STD_OUTPUT_HANDLE ), s, n, &written, NULL );
}

static void out_hex( ULONGLONG v )
{
    static const char hex[] = "0123456789abcdef";
    char buf[17];
    int i;
    for (i = 0; i < 16; i++) buf[15 - i] = hex[(v >> (4 * i)) & 0xf];
    buf[16] = 0;
    out( buf );
}

static int g_fail = 0;

static void check( const char *name, ULONGLONG got, ULONGLONG want )
{
    out( got == want ? "PASS " : "FAIL " );
    out( name );
    out( " got=0x" ); out_hex( got );
    out( " want=0x" ); out_hex( want );
    out( "\n" );
    if (got != want) g_fail++;
}

static LRESULT CALLBACK probe_wndproc( HWND hwnd, UINT msg, WPARAM wp, LPARAM lp )
{
    return DefWindowProcW( hwnd, msg, wp, lp );
}

static LONG WINAPI probe_filter1( EXCEPTION_POINTERS *ep ) { return EXCEPTION_EXECUTE_HANDLER; }
static LONG WINAPI probe_filter2( EXCEPTION_POINTERS *ep ) { return EXCEPTION_EXECUTE_HANDLER; }

void WINAPI identity_probe_entry( void )
{
    static const WCHAR clsname[] = { 'I','d','e','n','t','P','r','o','b','e',0 };
    WNDCLASSW wc;
    HWND hwnd;
    LONG_PTR got_gclp;
    WNDCLASSW got_wc;
    LPTOP_LEVEL_EXCEPTION_FILTER prev1, prev2;

    out( "identity_probe starting\n" );

    {
        char *p = (char *)&wc;
        int i;
        for (i = 0; i < (int)sizeof(wc); i++) p[i] = 0;
    }
    wc.lpfnWndProc = probe_wndproc;
    wc.hInstance = GetModuleHandleW( NULL );
    wc.lpszClassName = clsname;
    RegisterClassW( &wc );

    hwnd = CreateWindowExW( 0, clsname, clsname, WS_OVERLAPPEDWINDOW,
                             0, 0, 32, 32, NULL, NULL, wc.hInstance, NULL );
    if (!hwnd)
    {
        out( "FAIL CreateWindowExW failed, cannot continue\n" );
        ExitProcess( 1 );
    }

    /* 1: GetClassLongPtrW( GCLP_WNDPROC ) must read back exactly what was
     * registered -- the class-level analogue of the GWLP_WNDPROC bug. */
    got_gclp = GetClassLongPtrW( hwnd, GCLP_WNDPROC );
    check( "GetClassLongPtr(GCLP_WNDPROC)", (ULONGLONG)got_gclp, (ULONGLONG)(ULONG_PTR)probe_wndproc );

    /* 2: GetClassInfoW's struct-shaped route to the same field. */
    {
        char *p = (char *)&got_wc;
        int i;
        for (i = 0; i < (int)sizeof(got_wc); i++) p[i] = 0;
    }
    GetClassInfoW( wc.hInstance, clsname, &got_wc );
    check( "GetClassInfoW.lpfnWndProc", (ULONGLONG)(ULONG_PTR)got_wc.lpfnWndProc,
           (ULONGLONG)(ULONG_PTR)probe_wndproc );

    /* 3: SetUnhandledExceptionFilter's previous-value return -- the
     * highest-risk item on the audit's list.  First call installs
     * probe_filter1 with nothing previously registered (prev1 must be
     * NULL); the second installs probe_filter2 and must get back EXACTLY
     * probe_filter1, not this port's callback-pool stub for it. */
    prev1 = SetUnhandledExceptionFilter( probe_filter1 );
    check( "SetUnhandledExceptionFilter first prev", (ULONGLONG)(ULONG_PTR)prev1, 0 );

    prev2 = SetUnhandledExceptionFilter( probe_filter2 );
    check( "SetUnhandledExceptionFilter second prev", (ULONGLONG)(ULONG_PTR)prev2,
           (ULONGLONG)(ULONG_PTR)probe_filter1 );

    /* restore idiom real installers use: comparing before chaining */
    if (prev2 != probe_filter2) SetUnhandledExceptionFilter( prev2 );

    out( g_fail ? "identity_probe: FAILURES\n" : "identity_probe: ALL PASS\n" );
    ExitProcess( (UINT)g_fail );
}
