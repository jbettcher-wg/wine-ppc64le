/*
 * topo_ref -- the native half of the processor-topology gate.
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
 * WHY A NATIVE PROGRAM IS PART OF A GUEST GATE.  The gate has to answer two
 * questions that a guest probe cannot answer for itself:
 *
 *   1. "Is the gate's own idea of this machine right?"  check-cpu-topology.sh
 *      derives the Windows-to-Linux processor mapping from /sys in awk, on
 *      purpose, so that it is a SECOND OPINION rather than an echo of
 *      include/wine/cputopology.h.  Two independent derivations are only worth
 *      having if something compares them.  `dump` prints the header's answer in
 *      the awk script's own format so the gate can diff them line for line.
 *
 *   2. "Does the OBSERVER work?"  The affinity layer is the important one and
 *      it works by reading a thread's real Cpus_allowed_list out of /proc while
 *      a guest sits pinned.  If that machinery were only ever exercised against
 *      a guest, then on a tree where the guest's affinity is broken -- which is
 *      every tree today -- a broken observer and a broken guest look exactly
 *      the same: red.  This program pins ITSELF, correctly, by construction,
 *      using the contract header directly and no Wine at all.  The observer
 *      must call that GREEN.  That is the gate's calibration, and it is the
 *      only leg that can be green before the port is fixed.
 *
 * It speaks the same two-phase handshake the guest probe does, so the gate runs
 * one observer routine against both:
 *
 *      READY-BASELINE      -- affinity untouched; the gate snapshots /proc
 *      (waits for a go file)
 *      ...pins...
 *      READY-PINNED        -- the gate snapshots again and diffs
 *      (waits for a second go file)
 *
 * SABOTAGE LEVERS.  Both are switched on by the environment and both exist so
 * that assertions in the gate can be SEEN to go red:
 *
 *   CPUTOP_REF_IDENTITY=1   pin by the identity map -- Windows CPU i IS Linux
 *                           CPU i, the assumption the whole exercise is about
 *                           -- instead of through the table.  The observer must
 *                           go red.  On a machine where the mapping already IS
 *                           the identity (a dense single-node x86 box) this
 *                           lever changes nothing and cannot make anything red;
 *                           the program says so on stdout and the gate skips
 *                           that leg rather than reporting a false pass.
 *
 *   CPUTOP_REF_TRUNCATE=1   report the topology the way the unfixed tree does:
 *                           stop at the first online CPU whose NUMBER exceeds
 *                           the width of an affinity mask, and claim one group.
 *                           The gate's count comparison must go red.  Again a
 *                           no-op on a machine with fewer than 64 CPUs, and
 *                           again it says so.
 */

#define _GNU_SOURCE
#include <errno.h>
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "wine/cputopology.h"

#define CPUS_PER_GROUP WINE_CPUS_PER_GROUP

static int env_on( const char *name )
{
    const char *v = getenv( name );
    return v && *v && strcmp( v, "0" );
}

/* Block until `path` appears, or until the deadline.  The gate creates it once
 * it has finished reading /proc.  -> 0 if it appeared. */
static int wait_for_go( const char *path, int seconds )
{
    int waited = 0;

    if (!path) return 0;
    while (access( path, F_OK ) != 0)
    {
        if (waited >= seconds * 100)
        {
            fprintf( stderr, "topo_ref: timed out waiting for %s\n", path );
            return 1;
        }
        usleep( 10000 );
        waited++;
    }
    return 0;
}

/* The contract's table, printed in derive-topology.awk's format so the two can
 * be diffed directly.  Only the records both produce are printed here: the awk
 * script also emits highest_online and core_count, which this header does not
 * and should not know about. */
static void dump( const struct wine_cpu_topology *t )
{
    unsigned int i, n;
    int truncate = env_on( "CPUTOP_REF_TRUNCATE" );
    unsigned int count = t->count, group_count = t->group_count;

    if (truncate)
    {
        /* The unfixed behaviour, reproduced exactly: create_logical_proc_info()
         * walks CPUs by NUMBER and breaks when the number passes the width of
         * an affinity mask, so on a sparse online set it stops long before it
         * has seen the whole machine.  One group, because upstream hard-codes
         * that. */
        count = 0;
        for (i = 0; i < t->count; i++)
        {
            if (t->to_unix[i] > CPUS_PER_GROUP) break;
            count++;
        }
        group_count = 1;
        printf( "truncate.effective %d\n", count != t->count || group_count != t->group_count );
    }

    printf( "count %u\n", count );
    printf( "group_count %u\n", group_count );
    for (i = 0; i < group_count; i++)
    {
        unsigned int size = 0;
        for (n = 0; n < count; n++) if ((unsigned int)t->group_of[n] == i) size++;
        printf( "gsize %u %u\n", i, truncate ? size : t->group_size[i] );
    }
    printf( "node_count %u\n", t->node_count );
    for (i = 0; i < t->node_count; i++)
    {
        unsigned int in_node = 0;
        for (n = 0; n < count; n++) if (t->node_of[n] == t->node_ids[i]) in_node++;
        printf( "node %u %d %u\n", i, t->node_ids[i], in_node );
    }
    for (i = 0; i < count; i++)
        printf( "map %u %d %d %u %d\n", i, t->to_unix[i], t->group_of[i],
                t->index_in_group[i], t->node_of[i] );
}

/* Turn a (group, mask) pair into the Linux CPU set it names, the way the port
 * is supposed to: bit b of the mask is the b-th processor OF THAT GROUP, and
 * the table says which Linux CPU that is. */
static int expand( const struct wine_cpu_topology *t, unsigned int group,
                   unsigned long long mask, int identity, int *out, int max )
{
    unsigned int b, i;
    int n = 0;

    for (b = 0; b < CPUS_PER_GROUP && n < max; b++)
    {
        if (!(mask >> b & 1)) continue;
        if (identity)
        {
            /* The assumption this gate exists to disprove: Windows CPU b IS
             * Linux CPU b, group ignored.  Kept bit-for-bit as the old code
             * had it -- server/thread.c's CPU_SET(i, &set). */
            out[n++] = (int)b;
            continue;
        }
        for (i = 0; i < t->count; i++)
        {
            if ((unsigned int)t->group_of[i] != group) continue;
            if (t->index_in_group[i] != b) continue;
            out[n++] = t->to_unix[i];
            break;
        }
        /* A bit naming a processor the group does not have is not expanded --
         * there is no CPU behind it.  The gate compares against exactly what
         * comes out of here, so a mask with such a bit is a mask the gate
         * expects to be REFUSED or ignored, not silently widened. */
    }
    return n;
}

static void print_list( const char *label, const int *v, int n )
{
    int i;

    printf( "%s ", label );
    for (i = 0; i < n; i++) printf( "%s%d", i ? "," : "", v[i] );
    if (!n) printf( "-" );
    printf( "\n" );
}

int main( int argc, char **argv )
{
    const struct wine_cpu_topology *t = wine_cpu_topology();
    const char *mode = argc > 1 ? argv[1] : "dump";

    setvbuf( stdout, NULL, _IOLBF, 0 );

    if (!strcmp( mode, "dump" ))
    {
        dump( t );
        return 0;
    }

    if (!strcmp( mode, "pin" ))
    {
        unsigned int group;
        unsigned long long mask;
        int want[WINE_CPUS_PER_GROUP], n_want, i;
        int identity = env_on( "CPUTOP_REF_IDENTITY" );
        cpu_set_t *set;
        size_t setsize;
        int rc;

        if (argc < 5)
        {
            fprintf( stderr, "usage: topo_ref pin <group> <maskhex> <gofile-prefix>\n" );
            return 2;
        }
        group = (unsigned int)strtoul( argv[2], NULL, 0 );
        mask  = strtoull( argv[3], NULL, 0 );

        n_want = expand( t, group, mask, 0, want, WINE_CPUS_PER_GROUP );
        print_list( "pin.expect", want, n_want );
        if (identity)
        {
            int naive[WINE_CPUS_PER_GROUP];
            int n_naive = expand( t, group, mask, 1, naive, WINE_CPUS_PER_GROUP );
            int same = (n_naive == n_want);

            for (i = 0; same && i < n_want; i++) if (naive[i] != want[i]) same = 0;
            /* Say whether the lever can possibly change the outcome here.  On a
             * dense single-node box it cannot, and a sabotage leg that cannot
             * go red must be reported as inapplicable rather than passed. */
            printf( "identity.effective %d\n", !same );
            print_list( "pin.identity", naive, n_naive );
            n_want = n_naive;
            memcpy( want, naive, sizeof(naive[0]) * n_naive );
        }

        printf( "pin.pid %d\n", (int)getpid() );
        printf( "READY-BASELINE\n" );
        {
            char go[512];
            snprintf( go, sizeof(go), "%s.baseline", argv[4] );
            if (wait_for_go( go, 60 )) return 3;
        }

        setsize = CPU_ALLOC_SIZE( WINE_MAX_UNIX_CPU );
        set = CPU_ALLOC( WINE_MAX_UNIX_CPU );
        if (!set) { fprintf( stderr, "topo_ref: CPU_ALLOC failed\n" ); return 3; }
        CPU_ZERO_S( setsize, set );
        for (i = 0; i < n_want; i++) CPU_SET_S( want[i], setsize, set );

        errno = 0;
        rc = sched_setaffinity( 0, setsize, set );
        printf( "pin.rc %d\n", rc );
        printf( "pin.errno %d\n", rc ? errno : 0 );
        /* Give the kernel the chance to move us, so a reader of
         * /proc/self/stat sees the placement and not the pre-call one.  The
         * ALLOWED SET changes synchronously; this is only so the gate's
         * "which CPU is it actually on" note is not misleading. */
        sched_yield();
        printf( "READY-PINNED\n" );
        {
            char go[512];
            snprintf( go, sizeof(go), "%s.pinned", argv[4] );
            if (wait_for_go( go, 60 )) return 3;
        }
        CPU_FREE( set );
        return 0;
    }

    fprintf( stderr, "topo_ref: unknown mode '%s'\n", mode );
    return 2;
}
