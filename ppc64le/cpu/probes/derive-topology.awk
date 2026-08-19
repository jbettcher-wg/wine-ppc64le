# derive-topology.awk -- the gate's OWN answer for what this machine looks like.
#
# WHY A SECOND IMPLEMENTATION.  include/wine/cputopology.h is the contract both
# halves of the port derive from.  A gate that asked that header what the answer
# is would agree with the port by construction and could never catch the header
# being wrong -- it would only catch ntdll and wineserver disagreeing with each
# other.  So this reimplements the contract's ORDER from the kernel files
# directly, in a different language, and the gate cross-checks the two.  When
# they diverge, one of them has drifted, and that is worth being told about.
#
# The contract, restated (cputopology.h, "ORDER IS PART OF THE CONTRACT"):
#   1. NUMA nodes in ascending KERNEL ID order -- the ids, not 0..n-1.
#   2. Within a node, online CPUs in ascending Linux number.
#   3. Groups filled in that order.  A SECOND GROUP APPEARS ONLY WHEN THE
#      PROCESSORS DO NOT FIT IN ONE: a node that fits in what is left of the
#      current group joins it, so two nodes of twenty are one group of forty
#      and a thread's affinity mask can reach the whole machine.
#   4. A node that does not fit starts a fresh group, so a group never mixes
#      NUMA nodes; a node too big for one group is split into as FEW groups as
#      it needs and as EVENLY as those allow -- eighty processors in one node
#      become 40 and 40, not 64 and 16.
#   5. No NUMA information at all == one node holding every online CPU.
#
# NOTHING HERE IS SPECIFIC TO ONE MACHINE.  On a dense single-node x86 box --
# online "0-7", node/online "0" -- this emits count 8, one group of 8, node id
# 0, and map i -> i: the identity, which is what that machine has always had.
# On the POWER8 this was written on it emits 80 CPUs in two groups of 40 with
# node ids 0 and 8.  Both fall out of the same walk.
#
# ppc64le/cpu/probes/topology_cases.c is the other half of this: it runs the
# HEADER against ten synthetic machines.  This runs a second implementation
# against the real one.  The gate needs both -- the cases prove the rule is
# general, this proves the port is reading the machine it is actually on.
#
# Output is one record per line, whitespace separated, for the shell to read:
#   count <n>                       online CPUs == Windows processors
#   group_count <n>
#   gsize <group> <n>
#   node_count <n>
#   node <i> <kernel_id> <online_cpus_in_it>
#   map <win_index> <unix_cpu> <group> <index_in_group> <node_id>
#   highest_online <unix_cpu>       the largest online Linux CPU NUMBER
#   core_count <n|-1>               distinct thread-sibling sets, -1 if unknown
#   error <text>                    something the gate must not paper over
#
# Copyright 2026 the ppc64le port authors
#
# This library is free software; you can redistribute it and/or
# modify it under the terms of the GNU Lesser General Public
# License as published by the Free Software Foundation; either
# version 2.1 of the License, or (at your option) any later version.

# Parse one kernel "list" file -- "0-3,8-11,64" -- into arr[0..n-1] ascending.
# -> the number of values, 0 for an empty file, -1 if it could not be read.
function read_list( path, arr,    line, rc, n, i, k, parts, p, dash, beg, end, j )
{
    n = 0
    rc = (getline line < path)
    close( path )
    if (rc < 0) return -1
    if (rc == 0) return 0
    gsub( /[ \t\r\n]/, "", line )
    if (line == "") return 0
    # The kernel has a compact "N-M:S/T" stride form for some list files.  The
    # header does not parse it either, so rather than the two implementations
    # being wrong in DIFFERENT ways -- which would look like agreement failing
    # -- say so and stop.
    if (index( line, ":" ) > 0 || index( line, "/" ) > 0)
    {
        print "error", path " uses the kernel's compact stride list format" \
              " (" line "), which neither this gate nor cputopology.h parses"
        exit 3
    }
    k = split( line, parts, "," )
    for (i = 1; i <= k; i++)
    {
        p = parts[i]
        if (p == "") continue
        dash = index( p, "-" )
        if (dash > 0) { beg = substr( p, 1, dash - 1 ) + 0; end = substr( p, dash + 1 ) + 0 }
        else          { beg = p + 0; end = beg }
        if (end < beg) break               # the kernel does not do this
        for (j = beg; j <= end; j++) arr[n++] = j
    }
    return n
}

function is_online( cpu,    i )
{
    for (i = 0; i < n_online; i++) if (online[i] == cpu) return 1
    return 0
}

function place( cpu, node_id_val )
{
    map_unix[count]  = cpu
    map_group[count] = g
    map_inidx[count] = in_g
    map_node[count]  = node_id_val
    assigned[cpu]    = 1
    gsize[g]         = in_g + 1
    count++
    in_g++
}

BEGIN {
    CPUS_PER_GROUP = 64
    SYS = SYSROOT                          # "" for the real machine

    n_online = read_list( SYS "/sys/devices/system/cpu/online", online )
    if (n_online <= 0)
    {
        # Not a machine-specific assumption -- a running machine always has at
        # least one online CPU, so an unreadable or empty list means the gate
        # is looking at something that is not a Linux /sys and must say so
        # rather than invent a topology.
        print "error", "cannot read " SYS "/sys/devices/system/cpu/online"
        exit 3
    }

    n_nodes = read_list( SYS "/sys/devices/system/node/online", nodes )
    if (n_nodes <= 0) { nodes[0] = 0; n_nodes = 1 }

    count = 0; g = 0; in_g = 0; node_count = 0

    for (n = 0; n < n_nodes; n++)
    {
        path = SYS "/sys/devices/system/node/node" nodes[n] "/cpulist"
        n_ncpu = read_list( path, node_cpus )
        if (n_ncpu <= 0)
        {
            # A node with no cpulist: a memory-only node, or the node 0 we
            # synthesised because the machine has no NUMA at all.  If it is the
            # ONLY node it owns every online CPU; otherwise it owns none.
            if (n_nodes != 1) continue
            for (j = 0; j < n_online; j++) node_cpus[j] = online[j]
            n_ncpu = n_online
        }

        # A node lists every CPU it could ever hold, including offline
        # siblings.  Only the online ones are placed, and only the online ones
        # count towards whether the node fits in what is left of the group.
        placed = 0
        for (j = 0; j < n_ncpu; j++) if (is_online( node_cpus[j] )) placed++
        if (placed == 0) continue

        # A node that does not fit in what is left of the current group starts
        # a fresh one -- EVEN IF it is too big for one group and will be split
        # anyway -- so that a group never straddles two NUMA nodes.  A group is
        # the unit a thread's affinity is expressed in; a thread confined to a
        # straddling group would be spread across sockets with no way to say
        # otherwise.
        if (in_g > 0 && in_g + placed > CPUS_PER_GROUP)
        {
            g++
            in_g = 0
        }

        # Then split the node into as FEW groups as it needs and as EVENLY as
        # those allow: 80 processors in one node become 40 and 40, not 64 and
        # 16, so no group is left nearly empty.  A node that fits in a group
        # has node_groups == 1 and node_share == placed, and nothing below
        # fires.
        node_groups = int( (placed + CPUS_PER_GROUP - 1) / CPUS_PER_GROUP )
        if (node_groups < 1) node_groups = 1
        node_share = int( (placed + node_groups - 1) / node_groups )
        node_placed = 0

        for (j = 0; j < n_ncpu; j++)
        {
            cpu = node_cpus[j]
            if (!is_online( cpu )) continue
            if (assigned[cpu]) continue
            if (in_g == CPUS_PER_GROUP ||
                (node_placed > 0 && node_placed % node_share == 0 && node_placed < placed))
            {
                g++
                in_g = 0
            }
            place( cpu, nodes[n] )
            node_online[nodes[n]]++
            node_placed++
        }

        node_id[node_count] = nodes[n]
        node_count++
    }

    # An online CPU belonging to no node would otherwise be dropped silently,
    # and a dropped CPU is one the guest can never run on.  Sweep them up, the
    # same way the header does, onto the first node.
    for (i = 0; i < n_online; i++)
    {
        cpu = online[i]
        if (assigned[cpu]) continue
        fallback = (node_count > 0 ? node_id[0] : 0)
        if (in_g == CPUS_PER_GROUP) { g++; in_g = 0 }
        place( cpu, fallback )
        node_online[fallback]++
    }
    if (node_count == 0) { node_id[0] = 0; node_count = 1; node_online[0] = count }

    group_count = (count > 0 ? g + 1 : 0)

    print "count", count
    print "group_count", group_count
    for (i = 0; i < group_count; i++) print "gsize", i, gsize[i]
    print "node_count", node_count
    for (i = 0; i < node_count; i++)
        print "node", i, node_id[i], node_online[node_id[i]] + 0
    for (i = 0; i < count; i++)
        print "map", i, map_unix[i], map_group[i], map_inidx[i], map_node[i]

    hi = 0
    for (i = 0; i < n_online; i++) if (online[i] > hi) hi = online[i]
    print "highest_online", hi

    # Physical cores, for the RelationProcessorCore leg: one core per distinct
    # set of thread siblings, counting only the siblings that are ONLINE.  A
    # kernel without the topology files gives -1 and the gate skips that
    # comparison rather than guessing an SMT width -- which is exactly the kind
    # of assumption this whole exercise exists to remove.
    cores = 0
    for (i = 0; i < n_online; i++)
    {
        cpu = online[i]
        sp = SYS "/sys/devices/system/cpu/cpu" cpu "/topology/thread_siblings_list"
        n_sib = read_list( sp, sibs )
        if (n_sib < 0) { cores = -1; break }
        lowest = -1
        for (j = 0; j < n_sib; j++)
            if (is_online( sibs[j] ) && (lowest < 0 || sibs[j] < lowest)) lowest = sibs[j]
        if (lowest < 0) lowest = cpu       # siblings all offline: it is its own core
        if (!seen_core[lowest]) { seen_core[lowest] = 1; cores++ }
    }
    print "core_count", cores
}
