/*
 * optional_module_probe.c -- the guest half of check-optional-module.sh.
 *
 * WHAT IT PROVES.  A guest that goes looking for a Wine builtin at RUNTIME --
 * LoadLibraryW by name, then GetProcAddress for each entry point it wants --
 * must get a real module and real, callable addresses.  Static import tables
 * cannot cover this: a module reached only through LoadLibrary appears in no
 * import table at all, so nothing in the build or in check-import-chain.sh
 * notices when this tree has no guest thunk for it.
 *
 * WHY THAT MATTERS MORE THAN "AN OPTIONAL MODULE IS OPTIONAL".  A guest whose
 * probe answers NULL does not necessarily carry on: it takes its own failure
 * path, and that path is the least-tested code in the program.  DOOM (2016)
 * probes for Pdh.dll, and when the probe failed here it ran a cleanup that
 * releases the object it had just allocated with operator new through the
 * ENGINE's allocator instead of operator delete -- a mismatch that is latent
 * on Windows because the load never fails there.  What the user saw was
 *
 *     FATAL ERROR: Memory corruption before block!
 *
 * with a heap that was, measurably, entirely intact.  So the cost of an
 * unserved optional module is not a missing feature; it is an arbitrary bug in
 * a path the application has never run, reported as something else entirely.
 *
 * Built for x86_64-windows and run as a GUEST.  No CRT: the only imports are
 * the kernel32 entry points below, so the only thing under test is the module
 * being probed.
 */

#include <windows.h>

#define PDH_FMT_LONG            0x00000100
#define PDH_INVALID_ARGUMENT    0xc0000bbd

typedef LONG PDH_STATUS;

typedef struct
{
    DWORD CStatus;
    union {
        LONG     longValue;
        double   doubleValue;
        LONGLONG largeValue;
        LPCSTR   AnsiStringValue;
        LPCWSTR  WideStringValue;
    } u;
} PDH_FMT_COUNTERVALUE;

typedef PDH_STATUS (WINAPI *pOpenQueryW)( LPCWSTR, DWORD_PTR, void ** );
typedef PDH_STATUS (WINAPI *pCloseQuery)( void * );
typedef PDH_STATUS (WINAPI *pAddCounterW)( void *, LPCWSTR, DWORD_PTR, void ** );
typedef PDH_STATUS (WINAPI *pCollectQueryData)( void * );
typedef PDH_STATUS (WINAPI *pGetFormattedCounterValue)( void *, DWORD, LPDWORD,
                                                        PDH_FMT_COUNTERVALUE * );

static void put( const char *s )
{
    DWORD n = 0, w;
    const char *p = s;
    while (*p++) n++;
    WriteFile( GetStdHandle( STD_OUTPUT_HANDLE ), s, n, &w, NULL );
}

static void puthex( ULONG v )
{
    static const char d[] = "0123456789abcdef";
    char b[11];
    int i;
    b[0] = '0'; b[1] = 'x';
    for (i = 0; i < 8; i++) b[2 + i] = d[(v >> (28 - 4 * i)) & 0xf];
    b[10] = 0;
    put( b );
}

static int fails;

static void expect( int ok, const char *what )
{
    put( ok ? "  ok    " : "  FAIL  " );
    put( what );
    put( "\n" );
    if (!ok) fails++;
}

static void expect_status( PDH_STATUS got, PDH_STATUS want, const char *what )
{
    put( got == want ? "  ok    " : "  FAIL  " );
    put( what );
    put( " = " );
    puthex( (ULONG)got );
    if (got != want) { put( ", wanted " ); puthex( (ULONG)want ); }
    put( "\n" );
    if (got != want) fails++;
}

void probe_entry(void)
{
    /* The five DOOM binds, in the order it binds them.  The list is the
     * application's, not a guess: they are the GetProcAddress arguments at
     * DOOMx64vk.exe+0x19f4d40, and if any one answers NULL the game takes the
     * cleanup path described in the banner. */
    pOpenQueryW                open_query;
    pCloseQuery                close_query;
    pAddCounterW               add_counter;
    pCollectQueryData          collect;
    pGetFormattedCounterValue  get_formatted;

    void *query = NULL, *counter = NULL, *bogus = (void *)~(ULONG_PTR)0;
    PDH_FMT_COUNTERVALUE value;
    DWORD type = 0;
    PDH_STATUS st;
    HMODULE h;

    put( "optional-module-probe: subject Pdh.dll\n" );

    /* By the WIDE name and with the extension, exactly as DOOM asks for it. */
    h = LoadLibraryW( L"Pdh.dll" );
    expect( h != NULL, "LoadLibraryW(L\"Pdh.dll\")" );
    if (!h)
    {
        /* Nothing below can mean anything without the module.  Say so and
         * leave, rather than reporting a cascade of derived failures. */
        put( "optional-module-probe: FAIL (module did not load)\n" );
        ExitProcess( 1 );
    }

    open_query    = (pOpenQueryW)               GetProcAddress( h, "PdhOpenQueryW" );
    close_query   = (pCloseQuery)               GetProcAddress( h, "PdhCloseQuery" );
    add_counter   = (pAddCounterW)              GetProcAddress( h, "PdhAddCounterW" );
    collect       = (pCollectQueryData)         GetProcAddress( h, "PdhCollectQueryData" );
    get_formatted = (pGetFormattedCounterValue) GetProcAddress( h, "PdhGetFormattedCounterValue" );

    expect( open_query    != NULL, "GetProcAddress(PdhOpenQueryW)" );
    expect( close_query   != NULL, "GetProcAddress(PdhCloseQuery)" );
    expect( add_counter   != NULL, "GetProcAddress(PdhAddCounterW)" );
    expect( collect       != NULL, "GetProcAddress(PdhCollectQueryData)" );
    expect( get_formatted != NULL, "GetProcAddress(PdhGetFormattedCounterValue)" );
    if (fails)
    {
        put( "optional-module-probe: FAIL (an entry point is missing)\n" );
        ExitProcess( 1 );
    }

    /* VALUES, not liveness.  A thunk that reached the wrong function, or
     * marshalled its arguments badly, still returns SOMETHING -- so every call
     * below is checked against the answer Wine's own pdh gives, and the two
     * pointer-out arguments are checked for having been written. */

    /* A source name is a log file, which Wine does not implement, and it says
     * so with a specific status.  This is the one call whose interesting
     * argument is a non-NULL pointer, so it is also the check that the first
     * argument crossed the boundary at all rather than arriving as zero. */
    st = open_query( L"nonexistent.blg", 0, &query );
    expect_status( st, (PDH_STATUS)PDH_INVALID_ARGUMENT, "PdhOpenQueryW(source)" );

    st = open_query( NULL, 0, &query );
    expect_status( st, ERROR_SUCCESS, "PdhOpenQueryW(NULL)" );
    expect( query != NULL, "PdhOpenQueryW wrote the query handle" );

    /* One of the two counter paths Wine's pdh actually implements. */
    st = add_counter( query, L"\\Processor(_Total)\\% Processor Time", 0, &counter );
    expect_status( st, ERROR_SUCCESS, "PdhAddCounterW(processor time)" );
    expect( counter != NULL, "PdhAddCounterW wrote the counter handle" );

    /* A counter path Wine does NOT implement must be refused, and refused with
     * the right status -- otherwise "returns 0" would pass this gate.  Into a
     * SEPARATE handle: PdhAddCounterW zeroes its out-parameter before it looks
     * anything up, so reusing `counter` here would quietly throw away the good
     * handle the previous call just produced. */
    st = add_counter( query, L"\\NoSuchObject\\NoSuchCounter", 0, &bogus );
    expect_status( st, (PDH_STATUS)0xc0000bb9 /* PDH_CSTATUS_NO_COUNTER */,
                   "PdhAddCounterW(bogus path)" );
    expect( bogus == NULL, "PdhAddCounterW cleared the handle it refused" );

    st = collect( query );
    expect_status( st, ERROR_SUCCESS, "PdhCollectQueryData" );

    /* Two collections: a rate counter has no value until it has an interval to
     * divide by, so a single sample is not a fair test of the marshalling. */
    st = collect( query );
    expect_status( st, ERROR_SUCCESS, "PdhCollectQueryData (second)" );

    value.CStatus = 0xdeadbeef;
    value.u.longValue = -1;
    st = get_formatted( counter, PDH_FMT_LONG, &type, &value );
    expect_status( st, ERROR_SUCCESS, "PdhGetFormattedCounterValue" );
    /* The out-struct is the reason this call is here: it is the only one that
     * makes the native side WRITE into a caller buffer that crossed the
     * boundary, so an unwritten struct is a marshalling failure the status
     * alone would not show. */
    expect( value.CStatus != 0xdeadbeef, "PdhGetFormattedCounterValue wrote CStatus" );
    expect( value.u.longValue >= 0 && value.u.longValue <= 100,
            "processor time is a percentage in 0..100" );

    st = close_query( query );
    expect_status( st, ERROR_SUCCESS, "PdhCloseQuery" );

    /* A handle the module never issued must be refused.  Deliberately NULL and
     * not the just-closed query: PdhCloseQuery frees the query, so passing it
     * again would read freed memory -- a use-after-free in the gate itself,
     * whose answer would depend on the allocator rather than on the boundary.
     * NULL exercises the same check with a defined answer. */
    st = collect( NULL );
    expect_status( st, (PDH_STATUS)0xc0000bbc /* PDH_INVALID_HANDLE */,
                   "PdhCollectQueryData(NULL)" );

    if (fails) put( "optional-module-probe: FAIL\n" );
    else       put( "optional-module-probe: PASS\n" );
    ExitProcess( fails ? 1 : 0 );
}
