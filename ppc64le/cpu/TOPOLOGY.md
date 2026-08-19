# Processor topology

How this port derives the machine's processors, what it tells a guest, and what
it does with an affinity mask.

## The problem

Windows numbers processors densely, 0..N-1, in groups of at most 64. Linux
numbers CPUs however the firmware enumerated them. On x86 the two agree by
accident, so Wine assumed Windows CPU *i* is Linux CPU *i* and stopped at 64.

POWER breaks the assumption. A POWER8 at SMT4 on an SMT8 part:

```
/sys/devices/system/cpu/present   0-159
/sys/devices/system/cpu/online    0-3,8-11,16-19,...,152-155   (80)
/sys/devices/system/node/online   0,8                          (not 0,1)
```

Sparse CPUs, and NUMA node ids that are not contiguous.

## What was wrong, measured

| | before |
|---|---|
| `GetActiveProcessorCount(ALL)` | 32 (machine has 80) |
| `GetActiveProcessorGroupCount` | 1 |
| `GetNumaHighestNodeNumber` | 1 — node 8 unreachable |
| `NtGetCurrentProcessorNumber` | up to 155, on a machine reporting 80 |
| `RtlGetCurrentProcessorNumberEx` | always group 0; `Number` was the machine-wide index |
| CPUs a guest process could run on | **32 of 80** |

The last row was a scheduling confinement, not a reporting bug: every thread
got `set_thread_affinity()` at start, the `~0` default mapped bits 0-63 to
Linux CPUs 0-63, and the online set is sparse.

## Files

| file | what it does |
|---|---|
| `include/wine/cputopology.h` | Derives the topology from `/sys`. Shared by ntdll's unix side and wineserver, which must agree bit for bit; they have no common compilation unit, so it is a header. |
| `dlls/ntdll/unix/system.c` | Builds all `GLPI`/`GLPI_EX` records from it — per-group masks, kernel NUMA node ids, real group counts. `peb->NumberOfProcessors` comes from the same parser. |
| `dlls/ntdll/unix/thread.c` | `NtGetCurrentProcessorNumber` translates `sched_getcpu()` through the map. |
| `dlls/ntdll/unix/virtual.c` | KUSD `ActiveGroupCount`; `get_system_affinity_mask()` returns group 0's count. |
| `dlls/ntdll/rtl.c` | `RtlGetCurrentProcessorNumberEx` walks cached group sizes to return `{group, index-within-group}`. |
| `server/thread.c` | Translates affinity masks both directions. |
| `server/process.c` | Refuses a process mask naming no real processor. |
| `ppc64le/cpu/check-cpu-topology.sh` | Gate: counts, processor-number range, affinity placement, NUMA ids, core records. |
| `ppc64le/cpu/check-affinity.sh` | Gate: affinity round trip. |
| `ppc64le/cpu/probes/topology_cases.c` | Runs the derivation against synthetic `/sys` trees for ten machines. |

## Derivation rules

Order is part of the contract — two processes derive it separately.

1. NUMA nodes in ascending **kernel id** order (0 and 8 here, not 0 and 1).
2. Within a node, online CPUs in ascending Linux number.
3. A node joins the current group if it fits; otherwise it starts a new one.
4. A node larger than 64 is split into as few groups as it needs, as evenly as
   those allow (80 → 40+40, not 64+16).
5. A second group appears only above 64 processors. Two 20-CPU nodes share one
   group.

Rule 5 exists because a thread's affinity mask is relative to one group; more
groups than necessary means threads cannot be pointed at the whole machine.
Rule 4 exists because a group that mixes NUMA nodes cannot be expressed as a
locality.

## Affinity

An **unrestricted** mask (`~0`, the server's default, what "the application
never asked" looks like) means every online processor across all groups. An
**explicit** mask means group 0.

The asymmetry is the protocol's: `server/protocol.def` carries
`affinity_t affinity` and no group field, so the server cannot be told which
group a mask refers to. Both directions handle unrestricted the same way —
`get_thread_affinity()` reports `~0` for a thread that can use everything,
because the first process stores that value and every later thread inherits it.

## Results

| | before | after |
|---|---|---|
| `GetActiveProcessorCount(ALL)` | 32 | 80 |
| `GetActiveProcessorGroupCount` | 1 | 2 (40 / 40) |
| `GetNumaHighestNodeNumber` | 1 | 8 |
| `NtGetCurrentProcessorNumber` | up to 155 | always < 80 |
| `GetProcessAffinityMask` | `0x0f0f0f0f0f0f0f0f` | `0xffffffffff` |
| guest asks for 8 processors | refused | Linux 0-3,8-11 |
| Windows processor 20 | refused | Linux 40 |
| Windows processor 40 | ran on Linux 40 — wrong processor | refused |
| CPUs a guest process runs on | 32 | **80** |

## Machine independence

`topology_cases.c` writes synthetic sysfs trees and runs the real derivation
against them. Ten cases, all passing:

| machine | processors | groups |
|---|---|---|
| POWER8 2×10 SMT8 | 160 | 4 × 40 |
| POWER8 2×10 SMT4 | 80 | 2 × 40 |
| POWER8 2×10 SMT2 | 40 | 1 × 40 |
| POWER8 2×10 SMT1 | 20 | 1 × 20 |
| x86 8 dense, 1 node | 8 | 1 |
| x86 64 dense | 64 | 1 |
| x86 65 dense, 1 node | 65 | 33 + 32 |
| x86 128, 2 nodes | 128 | 2 × 64 |
| x86 96, 4 nodes | 96 | 2 × 48 |
| POWER 1×20 SMT4, 80 in one node | 80 | 2 × 40 |

Build with `-DWINE_CPU_SYSFS_ROOT=<dir>`. Compile-time only, deliberately not
an environment variable: it decides where threads run.

A dense single-node x86 machine derives the identity mapping and one group.

## Levers

| variable | effect |
|---|---|
| `WINEEMUNOCPUMAP=1` | Identity map for affinity (both directions) and `NtGetCurrentProcessorNumber`. |
| `WINEEMUNOCPUGROUPS=1` | One group, enumeration stops at Linux CPU 64, Linux-shaped masks. |

Both are read by ntdll **and** wineserver. `WINEEMUNOCPUMAP` is read once into
a static by the server, which outlives the run that set it — so a wineserver
left from an earlier run latches the old value and the lever appears not to
work. `check-cpu-topology.sh` runs `wineserver -k` before anything else for
that reason.

## Known limit

Explicit `SetThreadGroupAffinity(group 1, ...)` is refused: 40 of this
machine's 80 processors cannot be *named* by a guest. Unpinned threads are
unaffected and use all 80.

Closing it needs a `group` field on four requests in `server/protocol.def`, a
`SERVER_PROTOCOL_VERSION` bump, and a decision about process affinity, which on
Windows is a set of groups rather than one.

`check-cpu-topology.sh` layer 3e reports this as `LIMIT` rather than passing or
failing, and tests `protocol.def` for the field — so it becomes an assertion
again automatically once the field exists.

## Not done

- CPU hotplug after first use is not noticed; the topology is built once.
- Linux CPU numbers ≥ `CPU_SETSIZE` (1024) are dropped.
- `QueryIdleProcessorCycleTimeEx` accepts only group 0 and returns whole-machine
  entries.
- Idle-cycle *values* are unverified: op4k has no `cpufreq/base_frequency`, so
  `tsc_from_jiffies` is zero. Only the call's length is checked.
- The hwloc/FreeBSD path in `system.c` swaps the arguments to
  `logical_proc_info_add_numa_node()`. Not touched; not reachable on Linux.
