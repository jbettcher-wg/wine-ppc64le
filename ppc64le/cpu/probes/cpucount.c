/* What this port tells a guest about the machine's processors.
 *
 * Freestanding: no CRT, every call is an explicit kernel32 import, output is
 * WriteFile to stdout so the harness can diff it.  Sections below mirror the
 * ways a real program learns the machine's size; the point of the probe is
 * that they must all AGREE. */
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

static void out_hex( const char *label, unsigned long long v )
{
    char buf[96];
    int i = 0, j;
    while (label[i]) { buf[i] = label[i]; i++; }
    buf[i++] = '0'; buf[i++] = 'x';
    for (j = 60; j >= 0; j -= 4)
    {
        unsigned int nib = (unsigned int)((v >> j) & 0xf);
        buf[i++] = (char)(nib < 10 ? '0' + nib : 'a' + nib - 10);
    }
    buf[i++] = '\n'; buf[i] = 0;
    out( buf );
}

static unsigned int bits64( unsigned long long v )
{
    unsigned int n = 0;
    while (v) { n += (unsigned int)(v & 1); v >>= 1; }
    return n;
}

/* ntdll, for the one class kernel32 has no convenient wrapper for */
typedef struct
{
    LARGE_INTEGER IdleTime;
    LARGE_INTEGER KernelTime;
    LARGE_INTEGER UserTime;
    LARGE_INTEGER DpcTime;
    LARGE_INTEGER InterruptTime;
    ULONG InterruptCount;
} SPPI;
__declspec(dllimport) LONG __stdcall NtQuerySystemInformation( ULONG cls, void *info,
                                                               ULONG len, ULONG *retlen );
#define SysProcessorPerformanceInformation 8

static BYTE glpi_buf[256 * 1024];

/* one GetLogicalProcessorInformationEx query, returns bytes used or 0 */
static DWORD glpiex( LOGICAL_PROCESSOR_RELATIONSHIP rel )
{
    DWORD size = sizeof(glpi_buf);
    if (!GetLogicalProcessorInformationEx( rel, (void *)glpi_buf, &size )) return 0;
    return size;
}

void cpucount_entry(void)
{
    SYSTEM_INFO si;
    DWORD size, ofs;
    unsigned int i;

    /* ---- the headline counts, which must all describe the same machine -- */
    GetSystemInfo( &si );
    out_dec( "GetSystemInfo.dwNumberOfProcessors = ", si.dwNumberOfProcessors );
    out_hex( "GetSystemInfo.dwActiveProcessorMask= ", si.dwActiveProcessorMask );
    out_dec( "GetActiveProcessorGroupCount       = ", GetActiveProcessorGroupCount() );
    out_dec( "GetMaximumProcessorGroupCount      = ", GetMaximumProcessorGroupCount() );
    out_dec( "GetActiveProcessorCount(ALL)       = ", GetActiveProcessorCount( ALL_PROCESSOR_GROUPS ) );
    out_dec( "GetActiveProcessorCount(0)         = ", GetActiveProcessorCount( 0 ) );
    out_dec( "GetActiveProcessorCount(1)         = ", GetActiveProcessorCount( 1 ) );
    out_dec( "GetMaximumProcessorCount(ALL)      = ", GetMaximumProcessorCount( ALL_PROCESSOR_GROUPS ) );
    out_dec( "GetMaximumProcessorCount(0)        = ", GetMaximumProcessorCount( 0 ) );
    size = 0;
    GetLogicalProcessorInformationEx( RelationGroup, NULL, &size );
    out_dec( "GLPIEx(RelationGroup) size         = ", size );

    /* ---- the RelationGroup record itself ------------------------------- */
    if ((size = glpiex( RelationGroup )))
    {
        const SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX *p = (const void *)glpi_buf;
        out_dec( "Group.ActiveGroupCount             = ", p->Group.ActiveGroupCount );
        out_dec( "Group.MaximumGroupCount            = ", p->Group.MaximumGroupCount );
        for (i = 0; i < p->Group.ActiveGroupCount; i++)
        {
            out_dec( "  group index                      = ", i );
            out_dec( "  .ActiveProcessorCount            = ", p->Group.GroupInfo[i].ActiveProcessorCount );
            out_dec( "  .MaximumProcessorCount           = ", p->Group.GroupInfo[i].MaximumProcessorCount );
            out_hex( "  .ActiveProcessorMask             = ", p->Group.GroupInfo[i].ActiveProcessorMask );
        }
    }
    else out( "GLPIEx(RelationGroup) FAILED\n" );

    /* ---- packages and cores: counts and thread totals ------------------ */
    if ((size = glpiex( RelationProcessorPackage )))
    {
        unsigned int pkgs = 0, span2 = 0;
        for (ofs = 0; ofs < size; )
        {
            const SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX *p = (const void *)(glpi_buf + ofs);
            pkgs++;
            if (p->Processor.GroupCount > 1) span2++;
            ofs += p->Size;
        }
        out_dec( "packages                           = ", pkgs );
        out_dec( "packages spanning >1 group         = ", span2 );
    }
    else out( "GLPIEx(RelationProcessorPackage) FAILED\n" );

    if ((size = glpiex( RelationProcessorCore )))
    {
        unsigned int cores = 0, threads = 0, smt = 0, g;
        for (ofs = 0; ofs < size; )
        {
            const SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX *p = (const void *)(glpi_buf + ofs);
            cores++;
            if (p->Processor.Flags & LTP_PC_SMT) smt++;
            for (g = 0; g < p->Processor.GroupCount; g++)
                threads += bits64( p->Processor.GroupMask[g].Mask );
            ofs += p->Size;
        }
        out_dec( "cores                              = ", cores );
        out_dec( "core thread bits total             = ", threads );
        out_dec( "cores flagged SMT                  = ", smt );
    }
    else out( "GLPIEx(RelationProcessorCore) FAILED\n" );

    /* ---- NUMA: kernel node ids, per-group masks ------------------------ */
    if ((size = glpiex( RelationNumaNode )))
    {
        unsigned int g;
        for (ofs = 0; ofs < size; )
        {
            const SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX *p = (const void *)(glpi_buf + ofs);
            out_dec( "numa node id                       = ", p->NumaNode.NodeNumber );
            out_dec( "  .GroupCount                      = ", p->NumaNode.GroupCount );
            for (g = 0; g < (p->NumaNode.GroupCount ? p->NumaNode.GroupCount : 1); g++)
            {
                out_dec( "  .GroupMasks[].Group              = ", p->NumaNode.GroupMasks[g].Group );
                out_hex( "  .GroupMasks[].Mask               = ", p->NumaNode.GroupMasks[g].Mask );
            }
            ofs += p->Size;
        }
    }
    else out( "GLPIEx(RelationNumaNode) FAILED\n" );

    {
        ULONG highest = 0xdead;
        GetNumaHighestNodeNumber( &highest );
        out_dec( "GetNumaHighestNodeNumber           = ", highest );
        for (i = 0; i <= highest && i < 64; i++)
        {
            GROUP_AFFINITY ga;
            ga.Mask = 0; ga.Group = 0xffff;
            if (GetNumaNodeProcessorMaskEx( (USHORT)i, &ga ) && ga.Mask)
            {
                out_dec( "numa mask for node                 = ", i );
                out_dec( "  .Group                           = ", ga.Group );
                out_hex( "  .Mask                            = ", ga.Mask );
            }
        }
    }

    /* ---- the legacy (single-group) API --------------------------------- */
    {
        DWORD len = sizeof(glpi_buf);
        if (GetLogicalProcessorInformation( (void *)glpi_buf, &len ))
        {
            const SYSTEM_LOGICAL_PROCESSOR_INFORMATION *p = (const void *)glpi_buf;
            unsigned int nrec = len / sizeof(*p), cores = 0, threads = 0;
            unsigned long long allmask = 0;
            for (i = 0; i < nrec; i++)
            {
                if (p[i].Relationship == RelationProcessorCore)
                {
                    cores++;
                    threads += bits64( p[i].ProcessorMask );
                }
                allmask |= p[i].ProcessorMask;
            }
            out_dec( "legacy records                     = ", nrec );
            out_dec( "legacy cores                       = ", cores );
            out_dec( "legacy core thread bits            = ", threads );
            out_hex( "legacy mask union                  = ", allmask );
        }
        else out( "GetLogicalProcessorInformation FAILED\n" );
    }

    /* ---- per-CPU times must cover every processor, not the first 33 ----- */
    {
        static SPPI sppi[1280];
        ULONG ret = 0;
        LONG st = NtQuerySystemInformation( SysProcessorPerformanceInformation,
                                            sppi, sizeof(sppi), &ret );
        if (!st)
        {
            unsigned int nonzero = 0, entries = ret / sizeof(SPPI);
            for (i = 0; i < entries; i++)
                if (sppi[i].KernelTime.QuadPart) nonzero++;  /* KernelTime includes idle: never 0 for a live CPU */
            out_dec( "SPPI entries returned              = ", entries );
            out_dec( "SPPI entries with nonzero kernel   = ", nonzero );
        }
        else out( "NtQuerySystemInformation(SPPI) FAILED\n" );
    }

    /* ---- the legacy process/system affinity view ----------------------- */
    {
        DWORD_PTR pmask = 0, smask = 0;
        if (GetProcessAffinityMask( GetCurrentProcess(), &pmask, &smask ))
        {
            out_hex( "GetProcessAffinityMask.process     = ", pmask );
            out_dec( "  popcount                         = ", bits64( pmask ) );
            out_hex( "GetProcessAffinityMask.system      = ", smask );
            out_dec( "  popcount                         = ", bits64( smask ) );
        }
        else out( "GetProcessAffinityMask FAILED\n" );
    }

    /* ---- idle cycle times: the call must survive and size to the machine.
     * The VALUES cannot be verified on the measured POWER8: they are scaled
     * by cpufreq/base_frequency, which its sysfs does not provide, so every
     * entry is zero before and after the topology work. */
    {
        static ULONG64 cycles[1280];
        ULONG len = sizeof(cycles);
        if (QueryIdleProcessorCycleTimeEx( 0, &len, cycles ))
            out_dec( "QueryIdleProcessorCycleTimeEx len  = ", len );
        else out( "QueryIdleProcessorCycleTimeEx FAILED\n" );
    }

    /* ---- CpuSets: one entry per processor, group-1 CPUs on their node ---- */
    {
        ULONG len = 0;
        if (GetSystemCpuSetInformation( (void *)glpi_buf, sizeof(glpi_buf), &len, GetCurrentProcess(), 0 ) && len)
        {
            unsigned int entries = 0, node8 = 0, maxlp = 0, maxcore = 0;
            for (ofs = 0; ofs < len; )
            {
                const SYSTEM_CPU_SET_INFORMATION *c = (const void *)(glpi_buf + ofs);
                entries++;
                if (c->CpuSet.NumaNodeIndex == 8) node8++;
                if (c->CpuSet.LogicalProcessorIndex > maxlp) maxlp = c->CpuSet.LogicalProcessorIndex;
                if (c->CpuSet.CoreIndex > maxcore) maxcore = c->CpuSet.CoreIndex;
                ofs += c->Size;
            }
            out_dec( "CpuSet entries                     = ", entries );
            out_dec( "CpuSet entries on numa node 8      = ", node8 );
            out_dec( "CpuSet max LogicalProcessorIndex   = ", maxlp );
            out_dec( "CpuSet max CoreIndex               = ", maxcore );
        }
        else out( "GetSystemCpuSetInformation FAILED\n" );
    }

    /* ---- where am I running?  Must be < dwNumberOfProcessors, always ---- */
    {
        DWORD maxseen = 0, n;
        PROCESSOR_NUMBER pn;
        for (i = 0; i < 4096; i++)
        {
            n = GetCurrentProcessorNumber();
            if (n > maxseen) maxseen = n;
            if ((i & 255) == 255) Sleep( 0 );  /* give the scheduler a chance to move us */
        }
        out_dec( "GetCurrentProcessorNumber one-shot = ", GetCurrentProcessorNumber() );
        out_dec( "GetCurrentProcessorNumber max/4096 = ", maxseen );
        out_dec( "  in range (max < count)           = ", maxseen < si.dwNumberOfProcessors );
        pn.Group = 0xffff; pn.Number = 0xff; pn.Reserved = 0;
        GetCurrentProcessorNumberEx( &pn );
        out_dec( "GetCurrentProcessorNumberEx.Group  = ", pn.Group );
        out_dec( "GetCurrentProcessorNumberEx.Number = ", pn.Number );
    }

    ExitProcess( 0 );
}
