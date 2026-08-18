/*
 * Does the topology derivation hold up on machines we do not have?
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
 * include/wine/cputopology.h has to be right on more machines than exist here:
 * a POWER box at any SMT width, a dense x86 server, something with more
 * processors than one group can hold, a machine whose NUMA nodes are numbered
 * oddly.  Testing it only against the one box it was written on would prove
 * only that it works there.
 *
 * So this writes SYNTHETIC sysfs trees -- cpu/online, node/online and each
 * node's cpulist, in the kernel's own list format -- and runs the real
 * derivation against them by compiling with WINE_CPU_SYSFS_ROOT pointed at the
 * fake.  Same code, different machine.
 *
 * The POWER cases are generated the way the hardware actually behaves: an
 * SMT8-capable part exposes 8 CPU slots per core and brings the first N of
 * every 8 online, so SMT4 gives 0-3,8-11,... and SMT1 gives 0,8,16,...  That
 * stride is why a cap on the CPU INDEX rather than the count loses more than
 * half the machine.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "wine/cputopology.h"

static const char *root;

static void write_file( const char *path, const char *text )
{
    char full[1024];
    FILE *f;

    snprintf( full, sizeof(full), "%s%s", root, path );
    f = fopen( full, "w" );
    if (!f) { fprintf( stderr, "cannot write %s\n", full ); exit( 2 ); }
    fprintf( f, "%s\n", text );
    fclose( f );
}

static void make_dirs(void)
{
    char p[1024];

    snprintf( p, sizeof(p), "%s/sys", root );               mkdir( p, 0755 );
    snprintf( p, sizeof(p), "%s/sys/devices", root );        mkdir( p, 0755 );
    snprintf( p, sizeof(p), "%s/sys/devices/system", root ); mkdir( p, 0755 );
    snprintf( p, sizeof(p), "%s/sys/devices/system/cpu", root );  mkdir( p, 0755 );
    snprintf( p, sizeof(p), "%s/sys/devices/system/node", root ); mkdir( p, 0755 );
}

/* Append "a-b" or "a" to a list being built, with commas between runs. */
static void add_run( char *buf, size_t len, unsigned int a, unsigned int b )
{
    char item[32];

    if (a == b) snprintf( item, sizeof(item), "%u", a );
    else        snprintf( item, sizeof(item), "%u-%u", a, b );
    if (*buf) strncat( buf, ",", len - strlen(buf) - 1 );
    strncat( buf, item, len - strlen(buf) - 1 );
}

/* A POWER-shaped machine: `sockets` sockets, `cores` cores each, an SMT-`smt`
 * mode on a part with `slots` thread slots per core.  Node ids are given
 * explicitly because POWER does not number them 0,1,... */
static void make_power( unsigned int sockets, unsigned int cores,
                        unsigned int smt, unsigned int slots,
                        const int *node_ids )
{
    char online[8192] = "", nodelist[256] = "";
    unsigned int s, c, t;

    make_dirs();
    for (s = 0; s < sockets; s++)
    {
        char nodecpus[8192] = "";
        char path[256];

        for (c = 0; c < cores; c++)
        {
            unsigned int base = (s * cores + c) * slots;

            for (t = 0; t < smt; t += 1)
            {
                /* one run per contiguous online block within the core */
                if (t == 0)
                {
                    add_run( online,   sizeof(online),   base, base + smt - 1 );
                    add_run( nodecpus, sizeof(nodecpus), base, base + smt - 1 );
                }
            }
        }
        add_run( nodelist, sizeof(nodelist), (unsigned)node_ids[s], (unsigned)node_ids[s] );
        snprintf( path, sizeof(path), "/sys/devices/system/node/node%d/cpulist", node_ids[s] );
        {
            char dir[1024];
            snprintf( dir, sizeof(dir), "%s/sys/devices/system/node/node%d", root, node_ids[s] );
            mkdir( dir, 0755 );
        }
        write_file( path, nodecpus );
    }
    write_file( "/sys/devices/system/cpu/online", online );
    write_file( "/sys/devices/system/node/online", nodelist );
}

/* A dense machine: `n` CPUs numbered 0..n-1 split evenly over `nodes` nodes
 * numbered 0..nodes-1.  This is every x86 box. */
static void make_dense( unsigned int n, unsigned int nodes )
{
    char online[8192] = "", nodelist[256] = "";
    unsigned int i, per = n / nodes;

    make_dirs();
    add_run( online, sizeof(online), 0, n - 1 );
    for (i = 0; i < nodes; i++)
    {
        char nodecpus[8192] = "", path[256], dir[1024];
        unsigned int beg = i * per, end = (i == nodes - 1) ? n - 1 : beg + per - 1;

        add_run( nodecpus, sizeof(nodecpus), beg, end );
        add_run( nodelist, sizeof(nodelist), i, i );
        snprintf( dir, sizeof(dir), "%s/sys/devices/system/node/node%u", root, i );
        mkdir( dir, 0755 );
        snprintf( path, sizeof(path), "/sys/devices/system/node/node%u/cpulist", i );
        write_file( path, nodecpus );
    }
    write_file( "/sys/devices/system/cpu/online", online );
    write_file( "/sys/devices/system/node/online", nodelist );
}

static int failures;

static void check( const char *what, long got, long want )
{
    if (got == want) printf( "      ok   %-34s %ld\n", what, got );
    else { printf( "      FAIL %-34s got %ld, wanted %ld\n", what, got, want ); failures++; }
}

/* Every Windows index maps to a distinct online Linux CPU, and back again. */
static void check_bijective( const struct wine_cpu_topology *t )
{
    unsigned int i;
    int bad = 0;

    for (i = 0; i < t->count; i++)
    {
        int u = t->to_unix[i];

        if (u < 0 || u >= WINE_MAX_UNIX_CPU) { bad++; continue; }
        if (t->from_unix[u] != (int)i) bad++;
        if (i && t->group_of[i] < t->group_of[i - 1]) bad++;   /* groups ascend */
    }
    check( "bijective and group-ordered", bad, 0 );
}

static void run( const char *name, unsigned int want_count,
                 unsigned int want_groups, const unsigned int *want_sizes )
{
    struct wine_cpu_topology t;
    unsigned int i;

    printf( "  %s\n", name );
    wine_cpu_topology_build( &t );
    check( "processors", t.count, want_count );
    check( "groups", t.group_count, want_groups );
    for (i = 0; i < want_groups && i < t.group_count; i++)
    {
        char label[64];
        snprintf( label, sizeof(label), "group %u size", i );
        check( label, t.group_size[i], want_sizes[i] );
    }
    check_bijective( &t );
}

int main( int argc, char **argv )
{
    static const int power_nodes[8] = { 0, 8, 16, 24, 32, 40, 48, 56 };
    unsigned int sizes[8];

    if (argc < 2) { fprintf( stderr, "usage: %s <scratch-dir>\n", argv[0] ); return 2; }
    root = argv[1];

    printf( "topology cases (WINE_CPU_SYSFS_ROOT=%s)\n", root );

    /* The machine this was written on, at every SMT width its part supports.
     * 2 sockets x 10 cores, 8 thread slots per core, nodes 0 and 8. */
    /* Four groups of forty rather than 64/64/32: a group never mixes NUMA
     * nodes, and a node too big for one group is split evenly. */
    make_power( 2, 10, 8, 8, power_nodes );
    sizes[0] = 40; sizes[1] = 40; sizes[2] = 40; sizes[3] = 40;
    run( "POWER8 2x10 SMT8  (160 online, dense 0-159)", 160, 4, sizes );

    make_power( 2, 10, 4, 8, power_nodes );
    sizes[0] = 40; sizes[1] = 40;
    run( "POWER8 2x10 SMT4  (80 online, stride 8)", 80, 2, sizes );

    /* ONE group, not one per node, and that is the rule rather than an
     * accident: Windows creates a second group only when there are more than
     * 64 logical processors.  Two nodes of 20 fit whole in one group, neither
     * is split, and a thread's affinity mask can then reach the entire
     * machine -- which it could not if these were two groups. */
    make_power( 2, 10, 2, 8, power_nodes );
    sizes[0] = 40;
    run( "POWER8 2x10 SMT2  (40 online, one group)", 40, 1, sizes );

    make_power( 2, 10, 1, 8, power_nodes );
    sizes[0] = 20;
    run( "POWER8 2x10 SMT1  (20 online, one group)", 20, 1, sizes );

    /* Machines we do not have. */
    make_dense( 8, 1 );
    sizes[0] = 8;
    run( "x86 laptop        (8 dense, one node)", 8, 1, sizes );

    make_dense( 64, 1 );
    sizes[0] = 64;
    run( "x86 64 dense      (exactly one group)", 64, 1, sizes );

    make_dense( 65, 1 );
    sizes[0] = 33; sizes[1] = 32;
    run( "x86 65 dense      (one node split evenly)", 65, 2, sizes );

    make_dense( 128, 2 );
    sizes[0] = 64; sizes[1] = 64;
    run( "x86 128 two nodes (a group per node)", 128, 2, sizes );

    make_dense( 96, 4 );
    sizes[0] = 48; sizes[1] = 48;
    run( "x86 96 four nodes (nodes packed, none split)", 96, 2, sizes );

    /* A single socket with more than a group's worth, POWER-numbered. */
    make_power( 1, 20, 4, 8, power_nodes );
    sizes[0] = 40; sizes[1] = 40;
    run( "POWER 1x20 SMT4   (80 in ONE node, split evenly)", 80, 2, sizes );

    printf( "\n%s (%d failure(s))\n", failures ? "FAILED" : "all cases passed", failures );
    return failures ? 1 : 0;
}
