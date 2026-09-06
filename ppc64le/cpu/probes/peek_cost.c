/*
 * peek_cost.c -- the guest half of ppc64le/cpu/bench-peek-cost.sh.
 *
 * Guest x86-64 code.  Times an empty PeekMessageW per call, per SHAPE, so
 * the two halves of the poll can be told apart:
 *
 *   fast     PeekMessageW(&msg, 0, 0, 0, PM_REMOVE) -- the null-filter shape
 *            the spec2thunk 'peek' body answers guest-side (tens of ns).
 *   storm    PeekMessageW(&msg, 0, 0, 0, PM_NOREMOVE|PM_NOYIELD|PM_QS_SENDMESSAGE)
 *            -- Cyberpunk 2077's poll [MEASURED 2026-08-30, PEEK_SHAPE
 *            capture: 34.6M of 34.6M GameThread peeks], never served
 *            guest-side: a full trap + NtUserPeekMessage each, and inside
 *            it check_for_driver_events + peek_message + flush_window_surfaces.
 *   yield    the storm shape without PM_NOYIELD -- the upstream idle-courtesy
 *            block (two KeUserDispatchCallback crossings + NtYieldExecution).
 *
 * Each shape runs first with no window, then again after this thread has
 * created and shown a top-level window (the driver registers a window
 * surface for it, so flush_window_surfaces has a list to walk -- the
 * in-game situation).  `threads=N` runs N-1 extra threads polling the storm
 * shape for the duration of the measured loops, the way a game with several
 * message loops contends the same locks.
 *
 * Output: one `COST <shape> <window> <ns>` line per cell; DONE at the end.
 */

#include <windows.h>
#include <winternl.h>

static char obuf[1024];
static volatile LONG stop_others;

static void emit( const char *s )
{
    DWORD w;
    WriteFile( GetStdHandle( STD_OUTPUT_HANDLE ), s, lstrlenA( s ), &w, NULL );
}

#define SAY(...) do { wsprintfA( obuf, __VA_ARGS__ ); emit( obuf ); } while (0)

struct shape { const char *name; UINT flags; UINT rounds; };

static const struct shape shapes[] =
{
    { "fast",  PM_REMOVE,                                     1000000 },
    { "storm", PM_NOREMOVE | PM_NOYIELD | PM_QS_SENDMESSAGE,   300000 },
    { "yield", PM_NOREMOVE | PM_QS_SENDMESSAGE,                100000 },
};

static ULONG64 time_shape( const struct shape *s )
{
    LARGE_INTEGER t0, t1, f;
    MSG msg;
    UINT i;

    QueryPerformanceFrequency( &f );
    QueryPerformanceCounter( &t0 );
    for (i = 0; i < s->rounds; i++) PeekMessageW( &msg, 0, 0, 0, s->flags );
    QueryPerformanceCounter( &t1 );
    return (ULONG64)(t1.QuadPart - t0.QuadPart) * 1000000000ull / f.QuadPart / s->rounds;
}

static DWORD WINAPI other_thread( void *arg )
{
    MSG msg;
    PeekMessageW( &msg, 0, 0, 0, PM_REMOVE );  /* create the thread queue */
    while (!stop_others) PeekMessageW( &msg, 0, 0, 0, PM_NOREMOVE | PM_NOYIELD | PM_QS_SENDMESSAGE );
    return 0;
}

static LRESULT CALLBACK wnd_proc( HWND hwnd, UINT msg, WPARAM wp, LPARAM lp )
{
    return DefWindowProcW( hwnd, msg, wp, lp );
}

static HWND make_window( void )
{
    static const WCHAR cls[] = {'p','e','e','k','_','c','o','s','t',0};
    WNDCLASSW wc = { 0 };
    HWND hwnd;
    MSG msg;

    wc.lpfnWndProc = wnd_proc;
    wc.hInstance = GetModuleHandleA( NULL );
    wc.lpszClassName = cls;
    wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    if (!RegisterClassW( &wc )) { SAY( "FAIL RegisterClassW error %u\n", (unsigned)GetLastError() ); return NULL; }
    hwnd = CreateWindowExW( 0, cls, cls, WS_OVERLAPPEDWINDOW | WS_VISIBLE,
                            100, 100, 320, 240, NULL, NULL, wc.hInstance, NULL );
    if (!hwnd) { SAY( "FAIL CreateWindowExW error %u\n", (unsigned)GetLastError() ); return NULL; }
    /* let the driver map it and paint once, so its surface is listed and clean */
    UpdateWindow( hwnd );
    Sleep( 300 );
    while (PeekMessageW( &msg, 0, 0, 0, PM_REMOVE )) { TranslateMessage( &msg ); DispatchMessageW( &msg ); }
    Sleep( 100 );
    while (PeekMessageW( &msg, 0, 0, 0, PM_REMOVE )) { TranslateMessage( &msg ); DispatchMessageW( &msg ); }
    return hwnd;
}

void mainCRTStartup( void )
{
    HANDLE others[16];
    int nthreads = 1, i, w;
    HWND hwnd = NULL;
    MSG msg;

    {
        const WCHAR *cl = NtCurrentTeb()->Peb->ProcessParameters->CommandLine.Buffer;
        for (; cl && *cl; cl++)
            if (cl[0] == 't' && cl[1] == 'h' && cl[2] == 'r' && cl[3] == 'e' && cl[4] == 'a' &&
                cl[5] == 'd' && cl[6] == 's' && cl[7] == '=')
            {
                nthreads = 0;
                for (cl += 8; *cl >= '0' && *cl <= '9'; cl++) nthreads = nthreads * 10 + (*cl - '0');
                if (nthreads < 1) nthreads = 1;
                if (nthreads > 16) nthreads = 16;
            }
    }

    PeekMessageW( &msg, 0, 0, 0, PM_REMOVE );  /* create the queue, seed the fast body */

    for (i = 1; i < nthreads; i++) others[i] = CreateThread( NULL, 0, other_thread, NULL, 0, NULL );
    if (nthreads > 1) Sleep( 200 );

    for (w = 0; w < 2; w++)
    {
        if (w == 1 && !(hwnd = make_window()))
        {
            emit( "FAIL no window\n" );
            break;
        }
        for (i = 0; i < (int)(sizeof(shapes) / sizeof(shapes[0])); i++)
        {
            ULONG64 ns = time_shape( &shapes[i] );
            SAY( "COST %s %s threads=%d %u ns\n", shapes[i].name, w ? "window" : "nowindow", nthreads, (unsigned)ns );
        }
    }

    stop_others = 1;
    for (i = 1; i < nthreads; i++) WaitForSingleObject( others[i], 5000 );
    if (hwnd) DestroyWindow( hwnd );
    emit( "DONE\n" );
    ExitProcess( 0 );
}
