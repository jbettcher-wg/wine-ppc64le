/*
 * cputopo -- what a guest is told about this machine's processors, and where
 * its threads actually end up.
 *
 * Copyright 2026 the ppc64le port authors
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * ---------------------------------------------------------------------------
 *
 * THIS PROBE JUDGES NOTHING.  It prints, in `key value` lines, every number the
 * Win32 and NT processor interfaces will give it, and then it stops.  All the
 * judging happens in check-cpu-topology.sh, against a topology derived from
 * /sys at run time.
 *
 * That division is deliberate.  A guest PE cannot read /sys -- it is on the far
 * side of the boundary this gate is about -- so any expected value compiled
 * into it would either be hardcoded to one machine (which is the failure this
 * whole exercise exists to remove) or would have to be told to it by the very
 * port under test.  Printing and letting the shell judge keeps the truth on the
 * kernel's side of the fence.
 *
 * Modes come from the environment rather than argv, because this links no CRT
 * and therefore has no argv:
 *
 *   CPUTOPO_MODE=report    (default) print every count and every record.
 *   CPUTOPO_MODE=pin       set thread affinity and hold still while a native
 *                          observer reads the thread's REAL Linux CPU set out
 *                          of /proc.  CPUTOPO_GROUP and CPUTOPO_MASK say what
 *                          to ask for; CPUTOPO_GO names a DOS path prefix, and
 *                          the probe waits for <prefix>.baseline and then
 *                          <prefix>.pinned to appear before moving on.  That
 *                          two-phase pause is what lets the observer diff the
 *                          before and after states of every thread in the
 *                          process and find the one that moved, without having
 *                          to guess which Linux tid the guest thread is --
 *                          which a guest cannot tell it.
 *
 *   CPUTOPO_API=mask       use SetThreadAffinityMask (what games actually
 *                          call, and the path server/thread.c serves).
 *   CPUTOPO_API=group      use SetThreadGroupAffinity, the only way to name a
 *                          processor in a group other than the current one.
 *
 * WHY THE PIN PHASE HOLDS STILL RATHER THAN REPORTING ITS OWN PLACEMENT.  The
 * measured defect is that sched_setaffinity SUCCEEDS and leaves the thread
 * runnable on half of what was asked for.  Every value this probe could read
 * about itself comes back through the same port that got it wrong; only
 * something outside, reading the kernel, can see it.
 */

#define _WIN32_WINNT 0x0A00
#include <windows.h>

/* ntdll's, not kernel32's: this is the one measured returning a raw
 * sched_getcpu() value larger than the processor count the same port
 * reports. */
ULONG WINAPI NtGetCurrentProcessorNumber(void);

static void out( const char *s )
{
    DWORD n = 0, len = 0;

    while (s[len]) len++;
    WriteFile( GetStdHandle( STD_OUTPUT_HANDLE ), s, len, &n, NULL );
}

static int append_str( char *buf, int i, const char *s )
{
    while (*s) buf[i++] = *s++;
    return i;
}

static int append_dec( char *buf, int i, unsigned long long v )
{
    char d[24];
    int n = 0, j;

    if (!v) d[n++] = '0';
    while (v) { d[n++] = (char)('0' + (v % 10)); v /= 10; }
    for (j = n - 1; j >= 0; j--) buf[i++] = d[j];
    return i;
}

static int append_hex( char *buf, int i, unsigned long long v )
{
    static const char digits[] = "0123456789abcdef";
    char d[24];
    int n = 0, j;

    if (!v) d[n++] = '0';
    while (v) { d[n++] = digits[v & 0xf]; v >>= 4; }
    i = append_str( buf, i, "0x" );
    for (j = n - 1; j >= 0; j--) buf[i++] = d[j];
    return i;
}

/* key <dec>  --  the whole output format, one line at a time. */
static void kv( const char *key, unsigned long long v )
{
    char buf[160];
    int i = append_str( buf, 0, key );

    buf[i++] = ' ';
    i = append_dec( buf, i, v );
    buf[i++] = '\n';
    buf[i] = 0;
    out( buf );
}

/* key.<n> <dec>  --  for the per-group and per-record lines. */
static void kv_idx( const char *prefix, unsigned int idx, const char *suffix,
                    unsigned long long v )
{
    char buf[200];
    int i = append_str( buf, 0, prefix );

    buf[i++] = '.';
    i = append_dec( buf, i, idx );
    i = append_str( buf, i, suffix );
    buf[i++] = ' ';
    i = append_dec( buf, i, v );
    buf[i++] = '\n';
    buf[i] = 0;
    out( buf );
}

static void kv_hex( const char *key, unsigned long long v )
{
    char buf[160];
    int i = append_str( buf, 0, key );

    buf[i++] = ' ';
    i = append_hex( buf, i, v );
    buf[i++] = '\n';
    buf[i] = 0;
    out( buf );
}

static unsigned int popcount64( unsigned long long v )
{
    unsigned int n = 0;

    while (v) { n += (unsigned int)(v & 1); v >>= 1; }
    return n;
}

static unsigned int env_uint( const char *name, unsigned int def )
{
    char buf[64];
    DWORD n = GetEnvironmentVariableA( name, buf, sizeof(buf) );
    unsigned int v = 0, i = 0;
    int base = 10;

    if (!n || n >= sizeof(buf)) return def;
    if (buf[0] == '0' && (buf[1] == 'x' || buf[1] == 'X')) { base = 16; i = 2; }
    for (; buf[i]; i++)
    {
        int d;
        if (buf[i] >= '0' && buf[i] <= '9') d = buf[i] - '0';
        else if (base == 16 && buf[i] >= 'a' && buf[i] <= 'f') d = buf[i] - 'a' + 10;
        else if (base == 16 && buf[i] >= 'A' && buf[i] <= 'F') d = buf[i] - 'A' + 10;
        else break;
        v = v * (unsigned int)base + (unsigned int)d;
    }
    return v;
}

static ULONGLONG env_u64( const char *name, ULONGLONG def )
{
    char buf[64];
    DWORD n = GetEnvironmentVariableA( name, buf, sizeof(buf) );
    ULONGLONG v = 0;
    unsigned int i = 0;
    int base = 10;

    if (!n || n >= sizeof(buf)) return def;
    if (buf[0] == '0' && (buf[1] == 'x' || buf[1] == 'X')) { base = 16; i = 2; }
    for (; buf[i]; i++)
    {
        int d;
        if (buf[i] >= '0' && buf[i] <= '9') d = buf[i] - '0';
        else if (base == 16 && buf[i] >= 'a' && buf[i] <= 'f') d = buf[i] - 'a' + 10;
        else if (base == 16 && buf[i] >= 'A' && buf[i] <= 'F') d = buf[i] - 'A' + 10;
        else break;
        v = v * (ULONGLONG)base + (ULONGLONG)d;
    }
    return v;
}

static int env_is( const char *name, const char *want )
{
    char buf[64];
    DWORD n = GetEnvironmentVariableA( name, buf, sizeof(buf) );
    DWORD i;

    if (!n || n >= sizeof(buf)) return 0;
    for (i = 0; want[i]; i++) if (buf[i] != want[i]) return 0;
    return buf[i] == 0;
}

/* Wait for the observer to say it has finished reading /proc.  Bounded, so a
 * gate that dies leaves no guest spinning in the prefix forever. */
static int wait_go( const char *suffix )
{
    char path[MAX_PATH + 32];
    DWORD n = GetEnvironmentVariableA( "CPUTOPO_GO", path, MAX_PATH );
    int i = 0, waited = 0;

    if (!n || n >= MAX_PATH) return 1;
    i = (int)n;
    while (*suffix) path[i++] = *suffix++;
    path[i] = 0;
    while (GetFileAttributesA( path ) == INVALID_FILE_ATTRIBUTES)
    {
        if (waited++ > 6000) return 1;             /* 60s */
        Sleep( 10 );
    }
    return 0;
}

/* ------------------------------------------------------------------------ */

static BYTE glpi_buf[512 * 1024];

static void report_glpi( LOGICAL_PROCESSOR_RELATIONSHIP rel, const char *tag )
{
    SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX *p;
    DWORD size = sizeof(glpi_buf);
    DWORD off = 0;
    unsigned int records = 0, total_pop = 0;
    char key[64];
    int k;

    if (!GetLogicalProcessorInformationEx( rel, (void *)glpi_buf, &size ))
    {
        k = append_str( key, 0, tag );
        k = append_str( key, k, ".error" );
        key[k] = 0;
        kv( key, GetLastError() );
        k = append_str( key, 0, tag );
        k = append_str( key, k, ".records" );
        key[k] = 0;
        kv( key, 0 );
        return;
    }

    while (off < size)
    {
        p = (SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX *)(glpi_buf + off);
        if (!p->Size || off + p->Size > size) break;

        if (p->Relationship == RelationGroup)
        {
            unsigned int g;

            kv( "glpi.group.activegroupcount", p->Group.ActiveGroupCount );
            kv( "glpi.group.maximumgroupcount", p->Group.MaximumGroupCount );
            for (g = 0; g < p->Group.ActiveGroupCount && g < 20; g++)
            {
                kv_idx( "glpi.group", g, ".active",
                        p->Group.GroupInfo[g].ActiveProcessorCount );
                kv_idx( "glpi.group", g, ".maximum",
                        p->Group.GroupInfo[g].MaximumProcessorCount );
                kv_idx( "glpi.group", g, ".maskpop",
                        popcount64( p->Group.GroupInfo[g].ActiveProcessorMask ) );
                total_pop += popcount64( p->Group.GroupInfo[g].ActiveProcessorMask );
            }
        }
        else if (p->Relationship == RelationNumaNode)
        {
            kv_idx( "glpi.numa", records, ".node", p->NumaNode.NodeNumber );
            kv_idx( "glpi.numa", records, ".group", p->NumaNode.GroupMask.Group );
            kv_idx( "glpi.numa", records, ".maskpop",
                    popcount64( p->NumaNode.GroupMask.Mask ) );
            total_pop += popcount64( p->NumaNode.GroupMask.Mask );
        }
        else if (p->Relationship == RelationProcessorCore)
        {
            unsigned int i, pop = 0;

            /* GroupCount 0 means the single-GroupMask form of the union, which
             * is what a machine with one group produces; treat it as one. */
            unsigned int gc = p->Processor.GroupCount ? p->Processor.GroupCount : 1;

            for (i = 0; i < gc && i < 20; i++)
                pop += popcount64( p->Processor.GroupMask[i].Mask );
            total_pop += pop;
            if (records < 4)      /* a sample, so the log stays readable */
            {
                kv_idx( "glpi.core", records, ".maskpop", pop );
                kv_idx( "glpi.core", records, ".group",
                        p->Processor.GroupMask[0].Group );
                kv_idx( "glpi.core", records, ".flags", p->Processor.Flags );
            }
        }
        records++;
        off += p->Size;
    }

    k = append_str( key, 0, tag );
    k = append_str( key, k, ".records" );
    key[k] = 0;
    kv( key, records );
    k = append_str( key, 0, tag );
    k = append_str( key, k, ".maskpop.total" );
    key[k] = 0;
    kv( key, total_pop );
}

static void do_report(void)
{
    SYSTEM_INFO si;
    DWORD_PTR proc_mask = 0, sys_mask = 0;
    PROCESSOR_NUMBER pn;
    unsigned int g, agc;
    ULONG lo = 0xffffffff, hi = 0;
    int i;
    ULONG hnn = 0;

    GetSystemInfo( &si );
    kv( "si.numberofprocessors", si.dwNumberOfProcessors );
    kv_hex( "si.activeprocessormask", (ULONGLONG)si.dwActiveProcessorMask );
    kv( "si.activeprocessormask.pop", popcount64( (ULONGLONG)si.dwActiveProcessorMask ) );

    agc = GetActiveProcessorGroupCount();
    kv( "agc", agc );
    kv( "apc.all", GetActiveProcessorCount( ALL_PROCESSOR_GROUPS ) );
    kv( "mpc.all", GetMaximumProcessorCount( ALL_PROCESSOR_GROUPS ) );
    kv( "mgc", GetMaximumProcessorGroupCount() );

    for (g = 0; g < agc && g < 20; g++)
    {
        kv_idx( "group", g, ".active", GetActiveProcessorCount( (WORD)g ) );
        kv_idx( "group", g, ".maximum", GetMaximumProcessorCount( (WORD)g ) );
    }

    if (GetProcessAffinityMask( GetCurrentProcess(), &proc_mask, &sys_mask ))
    {
        kv_hex( "pam.process", (ULONGLONG)proc_mask );
        kv_hex( "pam.system", (ULONGLONG)sys_mask );
        kv( "pam.process.pop", popcount64( (ULONGLONG)proc_mask ) );
        kv( "pam.system.pop", popcount64( (ULONGLONG)sys_mask ) );
    }
    else kv( "pam.error", GetLastError() );

    if (GetNumaHighestNodeNumber( &hnn )) kv( "numa.highest", hnn );
    else kv( "numa.highest.error", GetLastError() );

    report_glpi( RelationGroup, "glpi.group" );
    report_glpi( RelationNumaNode, "glpi.numa" );
    report_glpi( RelationProcessorCore, "glpi.core" );

    /* NtGetCurrentProcessorNumber, sampled while the thread is free to migrate.
     * min and max are what the gate checks against the reported count: an index
     * outside it is a guest indexing an N-entry array with something bigger
     * than N, which is a memory-safety bug and not a reporting one. */
    for (i = 0; i < 4000; i++)
    {
        ULONG n = NtGetCurrentProcessorNumber();

        if (n < lo) lo = n;
        if (n > hi) hi = n;
        if ((i & 63) == 63) SwitchToThread();
    }
    kv( "procnum.min", lo );
    kv( "procnum.max", hi );
    kv( "procnum.samples", 4000 );
    kv( "procnum.k32", GetCurrentProcessorNumber() );

    pn.Group = 0xffff;
    pn.Number = 0xff;
    pn.Reserved = 0xff;
    GetCurrentProcessorNumberEx( &pn );
    kv( "procnumex.group", pn.Group );
    kv( "procnumex.number", pn.Number );
    kv( "procnumex.reserved", pn.Reserved );
    /* Read the plain one immediately after, on the same thread, so the two can
     * be required to agree: Windows defines GetCurrentProcessorNumber as the
     * Number field of GetCurrentProcessorNumberEx, and a port that answers
     * group-relative from one and machine-global from the other has invented a
     * third convention that no guest is written against.  They can legitimately
     * differ if the thread migrated between the two calls, which is why the
     * gate only requires this when the thread is pinned. */
    kv( "procnum.now", NtGetCurrentProcessorNumber() );

    out( "END\n" );
}

static void do_pin(void)
{
    unsigned int group = env_uint( "CPUTOPO_GROUP", 0 );
    ULONGLONG mask = env_u64( "CPUTOPO_MASK", 1 );
    int use_group_api = env_is( "CPUTOPO_API", "group" );
    PROCESSOR_NUMBER pn;
    GROUP_AFFINITY ga, prev;
    DWORD_PTR old;
    BOOL ok;

    kv( "pin.group", group );
    kv_hex( "pin.mask", mask );
    kv( "pin.pid", GetCurrentProcessId() );
    kv( "pin.api.group", use_group_api );

    /* The process affinity mask, reported here and not only in report mode,
     * because it is what DECIDES whether the call below is refused: a thread
     * mask has to be a subset of it.  [MEASURED] 2026-08-18, op4k: it comes
     * back as 0x0f0f0f0f0f0f0f0f -- the online Linux CPUs below 64 -- so bits
     * naming offline CPUs are clear and a guest asking for them is refused,
     * while a bit naming an online CPU past the end of its own group is
     * accepted.  Printing it next to the request turns "error 87" from a
     * puzzle into an explanation. */
    {
        DWORD_PTR proc_mask = 0, sys_mask = 0;

        if (GetProcessAffinityMask( GetCurrentProcess(), &proc_mask, &sys_mask ))
        {
            kv_hex( "pin.pam.process", (ULONGLONG)proc_mask );
            kv( "pin.pam.process.pop", popcount64( (ULONGLONG)proc_mask ) );
        }
    }

    out( "READY-BASELINE\n" );
    if (wait_go( ".baseline" )) { out( "TIMEOUT-BASELINE\n" ); ExitProcess( 4 ); }

    SetLastError( 0 );
    if (use_group_api)
    {
        ga.Mask = (KAFFINITY)mask;
        ga.Group = (WORD)group;
        ga.Reserved[0] = ga.Reserved[1] = ga.Reserved[2] = 0;
        prev.Mask = 0;
        prev.Group = 0xffff;
        ok = SetThreadGroupAffinity( GetCurrentThread(), &ga, &prev );
        kv( "pin.rc", ok );
        kv( "pin.lasterror", GetLastError() );
        kv_hex( "pin.previous", (ULONGLONG)prev.Mask );
    }
    else
    {
        old = SetThreadAffinityMask( GetCurrentThread(), (DWORD_PTR)mask );
        kv( "pin.rc", old != 0 );
        kv( "pin.lasterror", old ? 0 : GetLastError() );
        kv_hex( "pin.previous", (ULONGLONG)old );
    }

    /* Read the placement back through the port's own eyes.  Not the proof --
     * the observer is the proof -- but a readback that disagrees with what was
     * asked for is worth seeing next to it. */
    ga.Mask = 0;
    ga.Group = 0xffff;
    if (GetThreadGroupAffinity( GetCurrentThread(), &ga ))
    {
        kv( "pin.readback.group", ga.Group );
        kv_hex( "pin.readback.mask", (ULONGLONG)ga.Mask );
        kv( "pin.readback.maskpop", popcount64( (ULONGLONG)ga.Mask ) );
    }
    else kv( "pin.readback.error", GetLastError() );

    /* Yield first: the thread must actually be running somewhere the new mask
     * allows before asking where it is. */
    SwitchToThread();
    Sleep( 50 );
    pn.Group = 0xffff;
    pn.Number = 0xff;
    pn.Reserved = 0;
    GetCurrentProcessorNumberEx( &pn );
    kv( "pin.procnumex.group", pn.Group );
    kv( "pin.procnumex.number", pn.Number );
    kv( "pin.procnum", NtGetCurrentProcessorNumber() );
    kv( "pin.procnum.k32", GetCurrentProcessorNumber() );

    out( "READY-PINNED\n" );
    if (wait_go( ".pinned" )) { out( "TIMEOUT-PINNED\n" ); ExitProcess( 5 ); }
    out( "END\n" );
}

void cputopo_entry(void)
{
    if (env_is( "CPUTOPO_MODE", "pin" )) do_pin();
    else do_report();
    ExitProcess( 0 );
}
