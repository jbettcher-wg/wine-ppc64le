/* What this port tells a guest about the machine's processors. */
#include <windows.h>

static void out( const char *s )
{
    DWORD n = 0, len = 0;
    while (s[len]) len++;
    WriteFile( GetStdHandle( STD_OUTPUT_HANDLE ), s, len, &n, NULL );
}

static void out_dec( const char *label, unsigned long long v )
{
    char buf[96], d[24];
    int i = 0, j, n = 0;
    while (label[i]) { buf[i] = label[i]; i++; }
    if (!v) d[n++] = '0';
    while (v) { d[n++] = (char)('0' + (v % 10)); v /= 10; }
    for (j = n - 1; j >= 0; j--) buf[i++] = d[j];
    buf[i++] = '\n'; buf[i] = 0;
    out( buf );
}

void cpucount_entry(void)
{
    SYSTEM_INFO si;
    DWORD size = 0;

    GetSystemInfo( &si );
    out_dec( "GetSystemInfo.dwNumberOfProcessors = ", si.dwNumberOfProcessors );
    out_dec( "GetActiveProcessorGroupCount       = ", GetActiveProcessorGroupCount() );
    out_dec( "GetActiveProcessorCount(ALL)       = ", GetActiveProcessorCount( ALL_PROCESSOR_GROUPS ) );
    out_dec( "GetActiveProcessorCount(0)         = ", GetActiveProcessorCount( 0 ) );
    out_dec( "GetMaximumProcessorCount(ALL)      = ", GetMaximumProcessorCount( ALL_PROCESSOR_GROUPS ) );
    out_dec( "GetMaximumProcessorCount(0)        = ", GetMaximumProcessorCount( 0 ) );
    GetLogicalProcessorInformationEx( RelationGroup, NULL, &size );
    out_dec( "GLPIEx(RelationGroup) size         = ", size );
    ExitProcess( 0 );
}
