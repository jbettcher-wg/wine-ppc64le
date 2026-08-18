/*
 * wininet_status -- the RUNTIME proof for the five-argument
 * INTERNET_STATUS_CALLBACK that dlls/wininet/guestthunk.c wraps.
 *
 * That file and ntdll's __wine_guest_wrap_callback5 landed together and were
 * only ever proved STRUCTURALLY: the wrapper exists, it resolves the factory,
 * the factory mints a five-argument slot.  None of that says the callback is
 * ever actually invoked with real values, which is the only thing that
 * matters, because the failure this mechanism exists to prevent is not a
 * missing call -- it is a call that arrives with the wrong fifth argument.
 *
 *     typedef VOID (CALLBACK *INTERNET_STATUS_CALLBACK)(
 *         HINTERNET hInternet, DWORD_PTR dwContext, DWORD dwInternetStatus,
 *         LPVOID lpvStatusInformation, DWORD dwStatusInformationLength );
 *
 * dwStatusInformationLength is the fifth.  The trampoline pool writes the
 * guest target into the register ONE PAST the last real argument, so a
 * four-argument slot puts it in r7 -- ELFv2's fifth argument register, which
 * native wininet has already loaded with the length.  The callback would then
 * read a pointer-sized address as a byte count and, for the status codes that
 * describe a buffer, copy or scan that many bytes.  Nothing faults at the
 * boundary; the damage is inside the guest, on a thread it never entered.
 *
 * WHY THIS RUNS AGAINST A LOOPBACK SERVER.  A status callback only says
 * anything if something real drives it, and the values it carries have to be
 * ones this file can predict.  ppc64le/wininet/loopback_server.py binds
 * 127.0.0.1 on an EPHEMERAL port -- no name resolution off the machine, no
 * network access, no fixed port for two runs to collide over -- and the gate
 * passes the port here on the command line.
 *
 * WHAT THE FIFTH ARGUMENT IS, EXACTLY, AND WHY IT IS THREE DIFFERENT NUMBERS.
 * A single expected length would be matched by a constant, by a dropped
 * argument that happened to leave the right value in the register, or by a
 * callback invoked once.  Wine's own wininet gives three different ones for
 * the statuses this probe drives, and each is derivable here at compile time:
 *
 *   INTERNET_STATUS_RESOLVING_NAME   the host name, and because this probe
 *                                    registers through InternetSetStatusCallbackW
 *                                    the INET_CALLBACKW path in
 *                                    dlls/wininet/utility.c:260 keeps it as
 *                                    WCHAR -- (9+1)*2 = 20 bytes for
 *                                    "127.0.0.1".  (The ANSI registration
 *                                    would rewrite info_len to strlen+1 and
 *                                    collapse this to the same 10 as the next
 *                                    one, which is exactly why this probe
 *                                    registers the W form.)
 *   INTERNET_STATUS_NAME_RESOLVED    the resolved address as ANSI,
 *                                    strlen("127.0.0.1")+1 = 10
 *                                    (dlls/wininet/http.c:1778)
 *   INTERNET_STATUS_HANDLE_CREATED   an INTERNET_ASYNC_RESULT, 16 bytes
 *   INTERNET_STATUS_REQUEST_COMPLETE an INTERNET_ASYNC_RESULT, 16 bytes
 *
 * 20, 10 and 16 -- and the CONTENTS are checked too, not just the sizes: the
 * address string must literally be "127.0.0.1".
 *
 * dwContext, the SECOND argument, is checked for the same reason and carries
 * a value with bits set above 32.  It also has to be non-zero for any
 * callback to arrive at all: dlls/wininet/internet.c's INTERNET_SendCallback
 * returns early on a zero context, which would make a broken run look like a
 * quiet one.
 *
 * ASYNC ON PURPOSE.  INTERNET_FLAG_ASYNC is what makes wininet run the
 * request on its OWN worker thread and call back from there -- a thread the
 * guest never entered, with no guest frame anywhere beneath it.  That is the
 * case guestthunk.c's header calls "worse than either [FONTENUMPROC or
 * MONITORENUMPROC], because it fires asynchronously".  A synchronous run
 * would exercise the same wrapper on the probe's own thread and prove
 * strictly less.
 *
 * NO CRT.  Like every other guest probe here, the image entry point IS the
 * program in the guest build; the native build has a main() and the same
 * source otherwise, so the two transcripts can be compared byte for byte.
 */

#include <windows.h>
#include <wininet.h>

/* The context handed to InternetOpenUrlW and required back on every
 * callback.  Bits above 32 are set so that a DWORD_PTR truncated anywhere on
 * the way is visible, and it is emphatically not zero -- see the header. */
#define WS_CONTEXT   0x00c0ffee0badf00dull

/* The host, fixed so the two resolve-time lengths below are compile-time
 * facts rather than observations. */
#define WS_HOST_A    "127.0.0.1"
#define WS_HOST_W    L"127.0.0.1"
#define WS_HOST_LEN  9                      /* strlen(WS_HOST_A) */

#define WS_LEN_RESOLVING  ((WS_HOST_LEN + 1) * 2)   /* WCHAR, W callback: 20 */
#define WS_LEN_RESOLVED   (WS_HOST_LEN + 1)         /* ANSI address:      10 */
#define WS_LEN_ASYNC      16                        /* INTERNET_ASYNC_RESULT */

/* The body loopback_server.py serves, and its length.  Both legs check the
 * bytes they actually received against this. */
#define WS_BODY      "ppc64le-wininet-callback-gate-body\n"
#define WS_BODY_LEN  35

#define WS_MAX_EVENTS 64

/* ------------------------------------------------------------- output */

static void out( const char *s )
{
    DWORD n = 0, written;

    while (s[n]) n++;
    WriteFile( GetStdHandle( STD_OUTPUT_HANDLE ), s, n, &written, NULL );
}

static void out_hex( ULONGLONG v, int digits )
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

static void out_yn( const char *label, BOOL yes )
{
    out( label );
    out( yes ? "=yes" : "=no" );
}

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

/* ------------------------------------------------- tiny string helpers
 *
 * There is no CRT in the guest leg, so these are written out.  Both legs use
 * them, so neither can be right for a reason the other is not. */

static BOOL mem_eq( const void *a, const void *b, UINT n )
{
    const BYTE *p = a, *q = b;
    UINT i;
    for (i = 0; i < n; i++) if (p[i] != q[i]) return FALSE;
    return TRUE;
}

/* ------------------------------------------------------------- the record
 *
 * A per-status tally rather than a set of booleans: the gate needs to say
 * WHICH status carried WHICH length, and a "did any callback happen" flag
 * would pass with every length wrong.  Written from wininet's worker thread
 * and read from the probe's own after the completion event, so the event is
 * the synchronisation and nothing here needs a lock. */

struct ws_event
{
    DWORD      status;
    DWORD      info_len;
    ULONGLONG  context;
    BOOL       info_null;
};

static struct ws_event ws_events[WS_MAX_EVENTS];
static volatile LONG   ws_count;
static HANDLE          ws_complete;

/* Set when NAME_RESOLVED's payload matched WS_HOST_A byte for byte. */
static BOOL ws_resolved_text_ok;
static BOOL ws_saw_resolved;

/* The completion result wininet reports through REQUEST_COMPLETE. */
static ULONGLONG ws_async_result;
static DWORD     ws_async_error;

static void CALLBACK ws_callback( HINTERNET h, DWORD_PTR context, DWORD status,
                                  LPVOID info, DWORD info_len )
{
    LONG idx = InterlockedIncrement( &ws_count ) - 1;

    (void)h;
    if (idx < WS_MAX_EVENTS)
    {
        ws_events[idx].status    = status;
        ws_events[idx].info_len  = info_len;
        ws_events[idx].context   = (ULONGLONG)context;
        ws_events[idx].info_null = (info == NULL);
    }

    if (status == INTERNET_STATUS_NAME_RESOLVED && info)
    {
        ws_saw_resolved = TRUE;
        /* Content, not just length: the address string wininet resolved
         * 127.0.0.1 to is 127.0.0.1, and a fifth argument that were really a
         * pointer would not make this comparison true either. */
        ws_resolved_text_ok = (info_len == WS_LEN_RESOLVED) &&
                              mem_eq( info, WS_HOST_A, WS_HOST_LEN + 1 );
    }

    if (status == INTERNET_STATUS_REQUEST_COMPLETE)
    {
        if (info && info_len >= WS_LEN_ASYNC)
        {
            const INTERNET_ASYNC_RESULT *r = info;
            ws_async_result = (ULONGLONG)r->dwResult;
            ws_async_error  = r->dwError;
        }
        if (ws_complete) SetEvent( ws_complete );
    }
}

/* -> how many events carried this status */
static UINT ws_count_status( DWORD status )
{
    LONG n = ws_count, i;
    UINT c = 0;

    if (n > WS_MAX_EVENTS) n = WS_MAX_EVENTS;
    for (i = 0; i < n; i++) if (ws_events[i].status == status) c++;
    return c;
}

/* -> the info_len the FIRST event with this status carried, or 0xffffffff if
 * there was none.  0xffffffff rather than 0 because 0 is a legal length. */
static DWORD ws_len_of( DWORD status )
{
    LONG n = ws_count, i;

    if (n > WS_MAX_EVENTS) n = WS_MAX_EVENTS;
    for (i = 0; i < n; i++) if (ws_events[i].status == status) return ws_events[i].info_len;
    return 0xffffffffu;
}

/* -> TRUE if every recorded event carried our context */
static BOOL ws_all_contexts_ok( void )
{
    LONG n = ws_count, i;

    if (n > WS_MAX_EVENTS) n = WS_MAX_EVENTS;
    if (n == 0) return FALSE;
    for (i = 0; i < n; i++) if (ws_events[i].context != WS_CONTEXT) return FALSE;
    return TRUE;
}

/* ------------------------------------------------- the port, off the cmdline */

static DWORD parse_port( void )
{
    const WCHAR *p = GetCommandLineW();
    DWORD port = 0;
    const WCHAR *last = NULL;

    if (!p) return 0;
    /* The last run of digits on the command line is the port.  The image path
     * in argv[0] can contain digits, so scanning for the LAST run rather than
     * the first is what keeps a build directory called .../wt-sweep2/ from
     * being read as a port number. */
    while (*p)
    {
        if (*p >= '0' && *p <= '9')
        {
            const WCHAR *start = p;
            while (*p >= '0' && *p <= '9') p++;
            last = start;
        }
        else p++;
    }
    if (!last) return 0;
    while (*last >= '0' && *last <= '9')
    {
        port = port * 10 + (DWORD)(*last - '0');
        last++;
    }
    return port;
}

/* Build L"http://127.0.0.1:<port>/" without a CRT. */
static void build_url( WCHAR *buf, DWORD port )
{
    /* Spelled with the wide host macro rather than concatenating a wide and
     * a narrow literal, which is a C11 extension this probe has no need of. */
    static const WCHAR prefix[] = L"http://" WS_HOST_W L":";
    UINT i = 0, j;
    WCHAR digits[8];
    int d = 0;

    for (j = 0; prefix[j]; j++) buf[i++] = prefix[j];
    if (!port) digits[d++] = '0';
    while (port) { digits[d++] = (WCHAR)('0' + (port % 10)); port /= 10; }
    while (d) buf[i++] = digits[--d];
    buf[i++] = '/';
    buf[i] = 0;
}

/* ------------------------------------------------------------- the run */

static int wininet_status_run( void )
{
    HINTERNET hinet = NULL, hurl = NULL;
    WCHAR url[64];
    DWORD port = parse_port();
    DWORD err = 0, waited = 0;
    BYTE body[128];
    DWORD body_read = 0;
    INTERNET_STATUS_CALLBACK prev_cb;

    out( "wininet_status: start\n" );

    /* ---- step 1: a port arrived on the command line -------------------- */
    /* Deliberately its own step: without it every step below fails for a
     * reason that has nothing to do with the boundary, and a gate should
     * never make a harness mistake look like a port defect. */
    begin( "port from the command line" );
    out_yn( "non_zero", port != 0 );
    verdict( port != 0, "the gate did not pass a loopback port" );
    if (!port) goto done;

    build_url( url, port );

    /* ---- step 2: InternetOpenW(INTERNET_FLAG_ASYNC) -------------------- */
    begin( "InternetOpenW(DIRECT, INTERNET_FLAG_ASYNC)" );
    hinet = InternetOpenW( L"ppc64le-wininet-gate", INTERNET_OPEN_TYPE_DIRECT,
                           NULL, NULL, INTERNET_FLAG_ASYNC );
    out_yn( "opened", hinet != NULL );
    verdict( hinet != NULL, "InternetOpenW failed" );
    if (!hinet) goto done;

    /* ---- step 3: register, and get NULL back --------------------------- */
    /* The RETURN is the previously registered callback.  Nothing registered
     * one, so it must be NULL -- and on this port that is also the statement
     * that the wrapper did not invent a trampoline for a NULL input, which
     * guestthunk.c's guest_wrap5 is written to avoid. */
    begin( "InternetSetStatusCallbackW registers, previous is NULL" );
    ws_complete = CreateEventW( NULL, TRUE, FALSE, NULL );
    prev_cb = InternetSetStatusCallbackW( hinet, ws_callback );
    out_yn( "previous_null", prev_cb == NULL );
    out_yn( " not_invalid", prev_cb != INTERNET_INVALID_STATUS_CALLBACK );
    out_yn( " event", ws_complete != NULL );
    verdict( prev_cb == NULL && prev_cb != INTERNET_INVALID_STATUS_CALLBACK &&
             ws_complete != NULL,
             "the callback did not register" );

    /* ---- step 4: the async request ------------------------------------- */
    /* With INTERNET_FLAG_ASYNC and a non-zero context this returns NULL and
     * ERROR_IO_PENDING, and the handle arrives later through
     * REQUEST_COMPLETE.  A synchronous completion is accepted too -- wininet
     * is entitled to it -- and the step records which happened rather than
     * insisting, because that choice is wininet's and not this port's. */
    begin( "InternetOpenUrlW(async, non-zero context)" );
    hurl = InternetOpenUrlW( hinet, url, NULL, 0,
                             INTERNET_FLAG_RELOAD | INTERNET_FLAG_NO_CACHE_WRITE,
                             (DWORD_PTR)WS_CONTEXT );
    err = GetLastError();
    out_yn( "pending", hurl == NULL && err == ERROR_IO_PENDING );
    out_yn( " immediate", hurl != NULL );
    verdict( hurl != NULL || err == ERROR_IO_PENDING,
             "InternetOpenUrlW neither completed nor went pending" );

    /* ---- step 5: the completion arrives on wininet's own thread -------- */
    begin( "REQUEST_COMPLETE arrives" );
    if (!hurl)
    {
        waited = WaitForSingleObject( ws_complete, 30000 );
        if (waited == WAIT_OBJECT_0 && ws_async_result)
            hurl = (HINTERNET)(ULONG_PTR)ws_async_result;
    }
    else waited = WAIT_OBJECT_0;   /* completed synchronously; nothing to wait for */
    out_yn( "signalled", waited == WAIT_OBJECT_0 );
    out( " async_error=" ); out_dec( ws_async_error );
    out_yn( " have_handle", hurl != NULL );
    verdict( waited == WAIT_OBJECT_0 && hurl != NULL && ws_async_error == 0,
             "the request never completed" );

    /* ---- step 6: the callback ran, with OUR context -------------------- */
    /* dwContext is the second argument and the one INTERNET_SendCallback
     * refuses to proceed on when it is zero, so a zero here would have meant
     * no callbacks at all rather than callbacks with a wrong value. */
    begin( "every callback carried the context, unchanged" );
    out( "events=" ); out_dec( (ULONG)ws_count );
    out_yn( " all_context_ok", ws_all_contexts_ok() );
    out( " context=" ); out_hex( WS_CONTEXT, 16 );
    verdict( ws_count > 0 && ws_all_contexts_ok(),
             "the callback never ran, or dwContext did not survive" );

    /* ---- step 7: THE FIFTH ARGUMENT, three different lengths ----------- */
    /* This is the step the whole file exists for.  Each length below is a
     * compile-time constant derived from Wine's own wininet source, and they
     * are three DIFFERENT numbers, so a fifth argument that is really the
     * trampoline's target pointer -- or a constant, or a dropped register --
     * cannot satisfy all three. */
    begin( "dwStatusInformationLength per status (20 / 10 / 16)" );
    {
        DWORD l_res  = ws_len_of( INTERNET_STATUS_RESOLVING_NAME );
        DWORD l_nres = ws_len_of( INTERNET_STATUS_NAME_RESOLVED );
        DWORD l_done = ws_len_of( INTERNET_STATUS_REQUEST_COMPLETE );
        BOOL ok;

        out( "resolving=" );  out_dec( l_res );
        out( " resolved=" );  out_dec( l_nres );
        out( " complete=" );  out_dec( l_done );
        ok = (l_res == WS_LEN_RESOLVING) &&
             (l_nres == WS_LEN_RESOLVED) &&
             (l_done == WS_LEN_ASYNC);
        verdict( ok, "a status carried the wrong information length; the fifth "
                     "argument is not arriving" );
    }

    /* ---- step 8: and the payload those lengths describe ----------------- */
    begin( "NAME_RESOLVED payload is the address itself" );
    out_yn( "saw_resolved", ws_saw_resolved );
    out_yn( " text_ok", ws_resolved_text_ok );
    verdict( ws_saw_resolved && ws_resolved_text_ok,
             "the resolved-address payload was not \"" WS_HOST_A "\"" );

    /* ---- step 9: the status sequence is a real one ---------------------- */
    /* Counting the individual statuses rather than asserting an exact
     * sequence: which of them wininet emits, and how many times, is wininet's
     * business (a connection reused, a redirect, a retry all change it).
     * What must be true is that the ones this probe's lengths are derived
     * from happened at least once each. */
    begin( "the statuses this gate reads all occurred" );
    {
        UINT c_res  = ws_count_status( INTERNET_STATUS_RESOLVING_NAME );
        UINT c_nres = ws_count_status( INTERNET_STATUS_NAME_RESOLVED );
        UINT c_conn = ws_count_status( INTERNET_STATUS_CONNECTING_TO_SERVER );
        UINT c_sent = ws_count_status( INTERNET_STATUS_REQUEST_SENT );
        UINT c_done = ws_count_status( INTERNET_STATUS_REQUEST_COMPLETE );

        out( "resolving=" ); out_dec( c_res );
        out( " resolved=" ); out_dec( c_nres );
        out( " connecting=" ); out_dec( c_conn );
        out( " sent=" ); out_dec( c_sent );
        out( " complete=" ); out_dec( c_done );
        verdict( c_res >= 1 && c_nres >= 1 && c_conn >= 1 && c_sent >= 1 && c_done >= 1,
                 "wininet did not drive the status sequence this gate reads" );
    }

    /* ---- step 10: the body, so the request really happened -------------- */
    /* Without this the whole run could be satisfied by callbacks fired around
     * a request that fetched nothing.  The body is a constant in
     * loopback_server.py and a constant here. */
    begin( "InternetReadFile returns the server's body" );
    {
        DWORD got = 0;
        BOOL rok = FALSE;

        if (hurl)
        {
            rok = InternetReadFile( hurl, body, sizeof(body), &got );
            body_read = got;
        }
        out_yn( "read_ok", rok );
        out( " len=" ); out_dec( body_read );
        out_yn( " matches", body_read == WS_BODY_LEN &&
                            mem_eq( body, WS_BODY, WS_BODY_LEN ) );
        verdict( rok && body_read == WS_BODY_LEN &&
                 mem_eq( body, WS_BODY, WS_BODY_LEN ),
                 "the loopback body did not come back intact" );
    }

    /* ---- step 11: unregister ------------------------------------------- */
    /* Registering NULL is how a guest unregisters, and guestthunk.c passes
     * NULL straight through rather than minting a trampoline for it.  The
     * return is the callback that WAS installed, which on the guest lane is
     * one of the port's own trampolines -- so this step checks only that it
     * is non-NULL and not the failure sentinel, never its value, because the
     * two legs cannot agree on an address. */
    begin( "InternetSetStatusCallbackW(NULL) unregisters" );
    prev_cb = InternetSetStatusCallbackW( hinet, NULL );
    out_yn( "previous_non_null", prev_cb != NULL );
    out_yn( " not_invalid", prev_cb != INTERNET_INVALID_STATUS_CALLBACK );
    verdict( prev_cb != NULL && prev_cb != INTERNET_INVALID_STATUS_CALLBACK,
             "unregistering did not return the installed callback" );

done:
    if (hurl) InternetCloseHandle( hurl );
    if (hinet) InternetCloseHandle( hinet );
    if (ws_complete) CloseHandle( ws_complete );

    out( failures ? "wininet_status: FAIL " : "wininet_status: PASS " );
    out_dec( (ULONG)(step - failures) );
    out( "/" );
    out_dec( (ULONG)step );
    out( "\n" );
    return failures ? 1 : 0;
}

#ifdef WININET_STATUS_NO_CRT
/* The guest build has no C runtime: this IS the image entry point. */
void WINAPI wininet_status_entry( void )
{
    ExitProcess( (UINT)wininet_status_run() );
}
#else
int main( void )
{
    return wininet_status_run();
}
#endif
