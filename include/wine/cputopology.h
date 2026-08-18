/*
 * The machine's processor topology, as Windows needs to see it.
 *
 * Copyright 2026 the ppc64le port authors
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301, USA
 *
 * ---------------------------------------------------------------------------
 *
 * WHY THIS EXISTS.  Windows numbers its processors densely, 0..N-1, and splits
 * them into GROUPS of at most 64 because an affinity mask is one word wide.
 * Linux numbers its CPUs however the firmware enumerated them, and does not
 * promise that the numbers are dense or that the online ones are contiguous.
 *
 * On x86 the two agree by accident: CPUs come up 0,1,2,... and everything
 * online is below 64 on all but the largest machines, so Wine has always been
 * able to say "Windows CPU i IS Linux CPU i" and stop at 64.
 *
 * That accident does not survive POWER.  [MEASURED] 2026-08-18, a POWER8
 * running SMT4 out of an SMT8 part:
 *
 *   /sys/devices/system/cpu/present    0-159
 *   /sys/devices/system/cpu/online     0-3,8-11,16-19,...,152-155   (80 of them)
 *   /sys/devices/system/node/online    0,8                          (not 0,1!)
 *
 * Four online out of every eight slots, and NUMA nodes numbered 0 and 8 rather
 * than 0 and 1.  Stopping at Linux CPU index 64 therefore stops after 33 ONLINE
 * processors, and a machine with 80 tells a guest it has 32.  A guest that
 * asks for eight processors by affinity mask gets four, silently, because bits
 * 4-7 name CPUs that are offline -- measured, with sched_setaffinity returning
 * SUCCESS.
 *
 * NOTHING HERE IS SPECIFIC TO THAT MACHINE, and that is the point.  Every
 * number is read from the kernel: which CPUs are online, which NUMA node each
 * belongs to, and how many nodes there are.  A dense x86 box comes out of this
 * with the identity mapping it always had.
 *
 * WHY A HEADER RATHER THAN A FILE.  Two processes need this answer and must
 * never disagree about it: ntdll's unix side, which tells the guest what the
 * machine looks like, and wineserver, which turns an affinity mask into a real
 * sched_setaffinity() call.  They are separate binaries with no shared
 * compilation unit -- the same-named files in server/ and dlls/ntdll/ are
 * different files -- so the choice is one header included by both, or two
 * copies that will drift.  A disagreement here does not fail loudly: it pins
 * threads to the wrong CPUs.
 *
 * The derivation is a pure function of files under /sys, so both processes
 * reading it independently get the same answer.
 */

#ifndef __WINE_CPUTOPOLOGY_H
#define __WINE_CPUTOPOLOGY_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The sysfs root, overridable ONLY at compile time and defaulting to the real
 * one.  A topology derivation that can only be exercised on the machine it was
 * written for is a topology derivation nobody can check: this port has to be
 * right on a dense x86 box, on POWER at every SMT width, and on machines with
 * more processors than one group can hold, none of which are available to try.
 * A test translation unit defines this to a directory of synthetic
 * cpu/online and per-node cpulist files and gets the same code under test.
 *
 * Deliberately NOT an environment variable.  This decides where threads run;
 * a runtime knob that redirects it would be a way to get that wrong quietly. */
#ifndef WINE_CPU_SYSFS_ROOT
#define WINE_CPU_SYSFS_ROOT ""
#endif

/* Windows' own limits.  A group holds at most 64 processors because that is
 * how wide KAFFINITY is, and Windows supports at most 20 groups. */
#define WINE_CPUS_PER_GROUP  64
#define WINE_MAX_CPU_GROUPS  20
#define WINE_MAX_CPUS        (WINE_CPUS_PER_GROUP * WINE_MAX_CPU_GROUPS)

/* Linux CPU numbers can exceed the count -- `present 0-159` with 80 online --
 * so the reverse map has to be indexed by NUMBER, not by position. */
#define WINE_MAX_UNIX_CPU    4096

struct wine_cpu_topology
{
    unsigned int  count;                       /* online CPUs == Windows processors */
    unsigned int  group_count;
    unsigned int  group_size[WINE_MAX_CPU_GROUPS];
    int           to_unix[WINE_MAX_CPUS];      /* Windows index -> Linux CPU number */
    short         group_of[WINE_MAX_CPUS];     /* Windows index -> group */
    unsigned char index_in_group[WINE_MAX_CPUS];
    int           node_of[WINE_MAX_CPUS];      /* Windows index -> NUMA node id */
    int           from_unix[WINE_MAX_UNIX_CPU];/* Linux CPU number -> Windows index, -1 */
    unsigned int  node_count;
    int           node_ids[WINE_MAX_CPU_GROUPS * 4];  /* the ids the kernel used */
};

/* Parse a kernel "list" file -- "0-3,8-11,64" -- calling `cb` with each value.
 * Returns the number of values seen, or -1 if the file could not be read.
 * The format is the kernel's cpulist/nodelist format and nothing else. */
static inline int wine_cpu_parse_list( const char *path, void *ctx,
                                       void (*cb)( void *ctx, unsigned int value ) )
{
    char buf[8192];
    unsigned int beg, end;
    int seen = 0;
    char *p;
    FILE *f = fopen( path, "r" );

    if (!f) return -1;
    if (!fgets( buf, sizeof(buf), f )) { fclose( f ); return -1; }
    fclose( f );

    p = buf;
    while (*p)
    {
        char *next;

        while (*p == ' ' || *p == ',' || *p == '\n') p++;
        if (!*p) break;
        beg = end = (unsigned int)strtoul( p, &next, 10 );
        if (next == p) break;                  /* not a number: stop, do not spin */
        p = next;
        if (*p == '-')
        {
            p++;
            end = (unsigned int)strtoul( p, &next, 10 );
            if (next == p) break;
            p = next;
        }
        if (end < beg) break;                  /* the kernel does not do this */
        for (; beg <= end; beg++)
        {
            if (cb) cb( ctx, beg );
            seen++;
        }
    }
    return seen;
}

struct wine_cpu_list_ctx
{
    unsigned int *values;
    unsigned int  count;
    unsigned int  max;
};

static inline void wine_cpu_list_collect( void *ctx, unsigned int value )
{
    struct wine_cpu_list_ctx *c = (struct wine_cpu_list_ctx *)ctx;

    if (c->count < c->max) c->values[c->count++] = value;
}

/* -> nonzero if `cpu` is in the file's list.  Used to place an online CPU in
 * its NUMA node without assuming the node's list is a subset of anything. */
struct wine_cpu_find_ctx { unsigned int want; int found; };

static inline void wine_cpu_list_find( void *ctx, unsigned int value )
{
    struct wine_cpu_find_ctx *c = (struct wine_cpu_find_ctx *)ctx;

    if (value == c->want) c->found = 1;
}

/* ---------------------------------------------------------------------------
 * Build the topology.
 *
 * ORDER IS PART OF THE CONTRACT, because two processes derive this separately
 * and a guest's affinity mask means nothing unless both agree bit for bit:
 *
 *   1. NUMA nodes in ASCENDING KERNEL ID order.  Not 0..n-1 -- the ids
 *      themselves, read from /sys/devices/system/node/online, because POWER
 *      hands out 0 and 8 for a two-node machine.
 *   2. Within a node, online CPUs in ascending Linux number.
 *   3. Groups are filled in that order.  A node starts a new group when it
 *      would not fit whole in what remains of the current one, so a node is
 *      never split unless it is larger than a whole group -- in which case it
 *      is split across consecutive groups because there is no alternative.
 *
 * A SECOND GROUP APPEARS ONLY WHEN THE MACHINE HAS MORE THAN 64 PROCESSORS,
 * which is Windows' own rule and not merely a consequence of the above.  Two
 * NUMA nodes of twenty processors share one group rather than taking one
 * each, and that is what is wanted: a thread's affinity mask is relative to a
 * single group, so a machine cut into more groups than it needs is a machine
 * whose threads cannot be pointed at all of it.  [MEASURED] the same POWER8
 * part at SMT2 and SMT1 yields 40 and 20 processors and one group each way.
 *
 * A machine with no NUMA information at all is treated as one node holding
 * every online CPU, which is what a single-socket box looks like anyway.
 */
static inline void wine_cpu_topology_build( struct wine_cpu_topology *t )
{
    static const char *const online_path = WINE_CPU_SYSFS_ROOT "/sys/devices/system/cpu/online";
    static const char *const nodes_path  = WINE_CPU_SYSFS_ROOT "/sys/devices/system/node/online";
    unsigned int online[WINE_MAX_CPUS];
    unsigned int nodes[WINE_MAX_CPU_GROUPS * 4];
    struct wine_cpu_list_ctx oc, nc;
    unsigned int i, n, g = 0, in_g = 0;

    memset( t, 0, sizeof(*t) );
    for (i = 0; i < WINE_MAX_UNIX_CPU; i++) t->from_unix[i] = -1;
    for (i = 0; i < WINE_MAX_CPUS; i++) t->to_unix[i] = -1;

    oc.values = online; oc.count = 0; oc.max = WINE_MAX_CPUS;
    if (wine_cpu_parse_list( online_path, &oc, wine_cpu_list_collect ) <= 0 || !oc.count)
    {
        /* No sysfs, or an empty online set, which cannot be true of a running
         * machine.  One CPU is the honest answer: it is what every caller can
         * cope with, and it is better than claiming a topology that was never
         * read. */
        t->count = t->group_count = t->group_size[0] = 1;
        t->to_unix[0] = 0;
        t->from_unix[0] = 0;
        t->node_count = 1;
        t->node_ids[0] = 0;
        return;
    }

    nc.values = nodes; nc.count = 0; nc.max = sizeof(nodes) / sizeof(nodes[0]);
    if (wine_cpu_parse_list( nodes_path, &nc, wine_cpu_list_collect ) <= 0 || !nc.count)
    {
        nodes[0] = 0;
        nc.count = 1;
    }

    /* Walk the nodes in kernel-id order and place their online CPUs. */
    for (n = 0; n < nc.count; n++)
    {
        char path[256];
        unsigned int node_cpus[WINE_MAX_CPUS];
        struct wine_cpu_list_ctx pc;
        unsigned int j, placed = 0, node_groups, node_share, node_placed;

        snprintf( path, sizeof(path),
                  WINE_CPU_SYSFS_ROOT "/sys/devices/system/node/node%u/cpulist",
                  nodes[n] );
        pc.values = node_cpus; pc.count = 0; pc.max = WINE_MAX_CPUS;
        if (wine_cpu_parse_list( path, &pc, wine_cpu_list_collect ) < 0 || !pc.count)
        {
            /* A node with no cpulist (memory-only node, or no NUMA at all when
             * we synthesised node 0): if this is the ONLY node, it owns every
             * online CPU.  Otherwise it owns none and is skipped. */
            if (nc.count != 1) continue;
            memcpy( node_cpus, online, oc.count * sizeof(online[0]) );
            pc.count = oc.count;
        }

        /* Count how many of this node's CPUs are actually online -- a node
         * lists every CPU it could ever hold, including offline siblings. */
        for (j = 0; j < pc.count; j++)
        {
            unsigned int k;
            for (k = 0; k < oc.count; k++) if (online[k] == node_cpus[j]) { placed++; break; }
        }
        if (!placed) continue;

        /* WHERE THIS NODE'S PROCESSORS GO.
         *
         * A node that fits in what is left of the current group joins it --
         * that is how a 40-processor machine with two nodes ends up as one
         * group, which is what Windows does and what lets a thread's affinity
         * mask reach the whole machine.
         *
         * A node that does not fit starts a fresh group, EVEN IF it is too big
         * for one group and will have to be split anyway.  Letting it spill
         * into a partly-filled group would put two different NUMA nodes in one
         * group, and a group is the unit a thread's affinity is expressed in:
         * a thread confined to such a group would be spread across sockets
         * with no way to say otherwise.  [MEASURED] the alternative, packing
         * greedily, turned a 160-processor two-node machine into groups of
         * 64/64/32 whose middle group straddled both sockets.
         *
         * A node too big for one group is then split into as FEW groups as it
         * needs and as EVENLY as those allow -- 80 processors become 40 and 40
         * rather than 64 and 16 -- so no group is left nearly empty and the
         * scheduler has the same room in each. */
        if (in_g && in_g + placed > WINE_CPUS_PER_GROUP)
        {
            if (g + 1 >= WINE_MAX_CPU_GROUPS) break;
            g++;
            in_g = 0;
        }
        node_groups = (placed + WINE_CPUS_PER_GROUP - 1) / WINE_CPUS_PER_GROUP;
        if (!node_groups) node_groups = 1;
        node_share = (placed + node_groups - 1) / node_groups;
        node_placed = 0;

        for (j = 0; j < pc.count; j++)
        {
            unsigned int k, cpu = node_cpus[j];
            int is_online = 0;

            for (k = 0; k < oc.count; k++) if (online[k] == cpu) { is_online = 1; break; }
            if (!is_online) continue;
            if (cpu >= WINE_MAX_UNIX_CPU) continue;
            if (t->count >= WINE_MAX_CPUS) break;

            /* An even share of this node per group, and never more than a
             * group holds.  node_share is only below the hard limit when the
             * node is being split, so a node that fits is unaffected. */
            if (in_g == WINE_CPUS_PER_GROUP ||
                (node_placed && node_placed % node_share == 0 &&
                 node_placed < placed))
            {
                if (g + 1 >= WINE_MAX_CPU_GROUPS) break;
                g++;
                in_g = 0;
            }

            t->to_unix[t->count]        = (int)cpu;
            t->from_unix[cpu]           = (int)t->count;
            t->group_of[t->count]       = (short)g;
            t->index_in_group[t->count] = (unsigned char)in_g;
            t->node_of[t->count]        = (int)nodes[n];
            t->group_size[g]            = in_g + 1;
            t->count++;
            in_g++;
            node_placed++;
        }

        if (t->node_count < sizeof(t->node_ids) / sizeof(t->node_ids[0]))
            t->node_ids[t->node_count++] = (int)nodes[n];
    }

    /* An online CPU belonging to no node -- possible if the node cpulists and
     * the online list disagree -- would otherwise be dropped silently, and a
     * dropped CPU is a CPU the guest can never run on.  Sweep them up. */
    for (i = 0; i < oc.count; i++)
    {
        unsigned int cpu = online[i];

        if (cpu >= WINE_MAX_UNIX_CPU || t->from_unix[cpu] != -1) continue;
        if (t->count >= WINE_MAX_CPUS) break;
        if (in_g == WINE_CPUS_PER_GROUP)
        {
            if (g + 1 >= WINE_MAX_CPU_GROUPS) break;
            g++;
            in_g = 0;
        }
        t->to_unix[t->count]        = (int)cpu;
        t->from_unix[cpu]           = (int)t->count;
        t->group_of[t->count]       = (short)g;
        t->index_in_group[t->count] = (unsigned char)in_g;
        t->node_of[t->count]        = t->node_count ? t->node_ids[0] : 0;
        t->group_size[g]            = in_g + 1;
        t->count++;
        in_g++;
    }

    t->group_count = t->count ? (unsigned int)g + 1 : 0;
    if (!t->node_count) { t->node_ids[0] = 0; t->node_count = 1; }
}

/* The process-wide answer, built once.  Both users are single-threaded at the
 * point they first need it (ntdll during init, wineserver before it serves a
 * request), so no locking is implied here; a caller that needs it earlier or
 * from several threads must arrange that itself. */
static inline const struct wine_cpu_topology *wine_cpu_topology(void)
{
    static struct wine_cpu_topology topology;
    static int built;

    if (!built)
    {
        wine_cpu_topology_build( &topology );
        built = 1;
    }
    return &topology;
}

#endif /* __WINE_CPUTOPOLOGY_H */
