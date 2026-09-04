/*
 * ec_leaf.c -- the EC leaf path answers the same values the callback frame
 * does, for the exports it serves.
 *
 * A guest x86-64 program that calls leaf-class exports (thunk_leaf_exports
 * in dlls/ntdll/signal_ppc64.c) in loops and checks every answer against
 * what the export must say:
 *
 *   - GetCurrentProcessId: constant for the process, so every later call
 *     must equal the first.  The first call is a trap (it resolves the row
 *     and arms the module), the next is the first EC transition (it fills
 *     the cell), and everything after is a leaf-served call when the path
 *     is live -- which the sabotage run shows exactly: 1999 of 2000 wrong.
 *   - GetCurrentThreadId: the CONTROL that never crosses.  spec2thunk gives
 *     it a guest-side fast body (FAST_PATH_EXPORTS), so it stays green
 *     under sabotage; a red here would mean the fast body is gone and the
 *     row is crossing after all.
 *   - SetLastError(v) / GetLastError() == v: a value IN through argument 0
 *     (the R10 rescue on a transition) and a value OUT through RAX, on two
 *     leaf-served calls in a row.
 *   - TlsGetValue after TlsSetValue: TlsSetValue is NOT a leaf (its
 *     expansion arm allocates), so this mixes a full-path call and a leaf
 *     call on the same TEB slot.
 *   - GetTickCount never goes backwards within the loop.
 *
 * The sabotage lever (WINE_PPC64LE_EC_LEAF_SABOTAGE=1) flips RAX on every
 * leaf-served call, so the pid, tid, last-error and TLS checks must all go
 * red under it, and WINE_PPC64LE_NO_EC_LEAF=1 must put them back.
 *
 * Output (guest stdout): one PASS/FAIL line per check, then "DONE ec-leaf".
 * Exit code 0 when every check passed, 1 otherwise.  No CRT: freestanding,
 * kernel32-only imports, the sibling probes' discipline.
 */

typedef unsigned int u32;

u32 __stdcall GetCurrentProcessId( void );
u32 __stdcall GetCurrentThreadId( void );
u32 __stdcall GetLastError( void );
void __stdcall SetLastError( u32 err );
u32 __stdcall TlsAlloc( void );
int __stdcall TlsSetValue( u32 index, void *value );
void *__stdcall TlsGetValue( u32 index );
u32 __stdcall GetTickCount( void );
void *__stdcall GetStdHandle( u32 which );
int __stdcall WriteFile( void *h, const void *buf, u32 len, u32 *written, void *ov );
void __stdcall ExitProcess( u32 code );

#define N 2000

static void put_str( const char *s )
{
    u32 len = 0, w;
    while (s[len]) len++;
    WriteFile( GetStdHandle( (u32)-11 ), s, len, &w, 0 );
}

static void put_u32( u32 v )
{
    char buf[16];
    int i = 16;
    buf[--i] = 0;
    if (!v) buf[--i] = '0';
    while (v) { buf[--i] = '0' + (char)(v % 10); v /= 10; }
    put_str( buf + i );
}

static u32 failed;

static void report( const char *what, u32 bad )
{
    put_str( bad ? "FAIL " : "PASS " );
    put_str( what );
    if (bad) { put_str( ": " ); put_u32( bad ); put_str( " of " ); put_u32( N ); put_str( " wrong" ); }
    put_str( "\n" );
    if (bad) failed = 1;
}

void mainCRTStartup( void )
{
    u32 pid0, tid0, i, bad, idx, t0;

    pid0 = GetCurrentProcessId();
    for (bad = 0, i = 0; i < N; i++) if (GetCurrentProcessId() != pid0) bad++;
    report( "pid stable", bad );

    tid0 = GetCurrentThreadId();
    for (bad = 0, i = 0; i < N; i++) if (GetCurrentThreadId() != tid0) bad++;
    report( "tid stable", bad );

    for (bad = 0, i = 0; i < N; i++)
    {
        SetLastError( 0x1000 + i );
        if (GetLastError() != 0x1000 + i) bad++;
    }
    report( "last-error round trip", bad );

    idx = TlsAlloc();
    for (bad = 0, i = 0; i < N; i++)
    {
        TlsSetValue( idx, (void *)(unsigned long long)(i * 7 + 1) );
        if (TlsGetValue( idx ) != (void *)(unsigned long long)(i * 7 + 1)) bad++;
    }
    report( "tls round trip", bad );

    t0 = GetTickCount();
    for (bad = 0, i = 0; i < N; i++)
    {
        u32 t = GetTickCount();
        if ((int)(t - t0) < 0) bad++;
        t0 = t;
    }
    report( "tick count monotone", bad );

    put_str( "DONE ec-leaf\n" );
    ExitProcess( failed );
}
