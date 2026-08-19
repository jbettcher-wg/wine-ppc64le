/* What a guest actually gets when it asks for processors by affinity mask.
 *
 * The question this answers is not "did SetThreadAffinityMask return success"
 * -- it always did -- but "how many processors is the thread now really allowed
 * to run on".  So the probe asks the KERNEL, not Wine: it reads
 * /proc/thread-self/status through Wine's Z: drive, which is the calling
 * thread's own status file, and prints the Cpus_allowed_list line verbatim.
 * The harness intersects that with the online CPU list, because
 * Cpus_allowed_list reports the set that was REQUESTED and the kernel silently
 * drops the offline members of it -- which is the whole bug.
 */
#include <windows.h>

#ifndef HOLD_MS
#define HOLD_MS 0
#endif

#ifndef WANT_MASK
#define WANT_MASK 0xFFull
#endif

static void out( const char *s )
{
    DWORD n = 0, len = 0;
    while (s[len]) len++;
    WriteFile( GetStdHandle( STD_OUTPUT_HANDLE ), s, len, &n, NULL );
}

static void out_dec( const char *label, unsigned long long v )
{
    char buf[128], d[24];
    int i = 0, j, n = 0;
    while (label[i]) { buf[i] = label[i]; i++; }
    if (!v) d[n++] = '0';
    while (v) { d[n++] = (char)('0' + (v % 10)); v /= 10; }
    for (j = n - 1; j >= 0; j--) buf[i++] = d[j];
    buf[i++] = '\n'; buf[i] = 0;
    out( buf );
}

static void out_hex( const char *label, unsigned long long v )
{
    static const char digits[] = "0123456789abcdef";
    char buf[128], d[24];
    int i = 0, j, n = 0;
    while (label[i]) { buf[i] = label[i]; i++; }
    buf[i++] = '0'; buf[i++] = 'x';
    if (!v) d[n++] = '0';
    while (v) { d[n++] = digits[v & 0xf]; v >>= 4; }
    for (j = n - 1; j >= 0; j--) buf[i++] = d[j];
    buf[i++] = '\n'; buf[i] = 0;
    out( buf );
}

/* Print the calling thread's Cpus_allowed_list, straight from the kernel.
 * /proc files report a size of 0, so this reads until ReadFile stops giving
 * bytes rather than trusting GetFileSize. */
static void out_cpus_allowed( const char *label )
{
    /* static, not a local: a 16K stack frame pulls in the compiler's stack
     * probe (___chkstk_ms), and there is no CRT here to provide it. */
    static char buf[16384];
    DWORD total = 0, got = 0;
    HANDLE h;
    unsigned int i;

    h = CreateFileA( "Z:\\proc\\thread-self\\status", GENERIC_READ,
                     FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL );
    if (h == INVALID_HANDLE_VALUE)
    {
        out( label );
        out( "UNREADABLE (no Z:\\proc\\thread-self\\status)\n" );
        return;
    }
    while (total < sizeof(buf) - 1 && ReadFile( h, buf + total, sizeof(buf) - 1 - total, &got, NULL ) && got)
        total += got;
    CloseHandle( h );
    buf[total] = 0;

    for (i = 0; i + 18 <= total; i++)
    {
        if (buf[i] != 'C' || buf[i + 1] != 'p') continue;
        {
            static const char key[] = "Cpus_allowed_list:";
            unsigned int k = 0;
            while (key[k] && buf[i + k] == key[k]) k++;
            if (key[k]) continue;
            i += k;
            while (buf[i] == ' ' || buf[i] == '\t') i++;
            out( label );
            while (i < total && buf[i] != '\n') { char c[2]; c[0] = buf[i]; c[1] = 0; out( c ); i++; }
            out( "\n" );
            return;
        }
    }
    out( label );
    out( "ABSENT (no Cpus_allowed_list line)\n" );
}

void affinity_entry(void)
{
    DWORD_PTR want = (DWORD_PTR)(WANT_MASK), prev, readback, procmask = 0, sysmask = 0;
    SYSTEM_INFO si;
    unsigned int n = 0, i;

    GetSystemInfo( &si );
    out_dec( "numberofprocessors = ", si.dwNumberOfProcessors );
    GetProcessAffinityMask( GetCurrentProcess(), &procmask, &sysmask );
    out_hex( "process_affinity   = ", procmask );
    out_hex( "system_affinity    = ", sysmask );

    out_cpus_allowed( "cpus_before        = " );

    for (i = 0; i < 8 * sizeof(want); i++) if (want & ((DWORD_PTR)1 << i)) n++;
    out_hex( "request_mask       = ", want );
    out_dec( "request_processors = ", n );

    prev = SetThreadAffinityMask( GetCurrentThread(), want );
    out_hex( "set_returned_prev  = ", prev );
    if (!prev) out( "set_result         = FAILED\n" );
    else       out( "set_result         = SUCCESS\n" );

    out_cpus_allowed( "cpus_after         = " );

    /* There is no GetThreadAffinityMask; setting the same mask again returns
     * the mask currently in force, which is the read-back this needs. */
    readback = SetThreadAffinityMask( GetCurrentThread(), want );
    out_hex( "readback_mask      = ", readback );

    /* Optionally stay alive so a harness outside can read this thread's
     * affinity from /proc independently -- a second, unrelated witness to the
     * same fact, which is the only way to catch the probe reading the wrong
     * thread's status file. */
#if HOLD_MS
    out( "holding\n" );
    Sleep( HOLD_MS );
#endif
    ExitProcess( 0 );
}
