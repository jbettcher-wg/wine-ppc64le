# Processor topology on POWER: what a guest is told, and what it costs

A decision document, not a mechanism one. Nothing here is implemented; this
records the measurement, the audit, and a recommendation about whether to act.

**Status: open. Recommendation is to defer — see the last section for why, and
for the one condition that should change the answer.**

## What a guest is told today

`ppc64le/cpu/probes/cpucount.c` is the guest program these numbers come from.
op4k, 2 sockets x 10 cores x SMT4 = **80 online CPUs**:

| call | answers | should be |
|---|---|---|
| `GetSystemInfo().dwNumberOfProcessors` | **80** | 80 |
| `GetActiveProcessorCount(ALL_PROCESSOR_GROUPS)` | **32** | 80 |
| `GetActiveProcessorCount(0)` | **32** | 64 (one group's worth) |
| `GetMaximumProcessorCount(ALL)` | **32** | 80 |
| `GetActiveProcessorGroupCount()` | **1** | 2 |
| `NtGetCurrentProcessorNumber()` | a raw Linux CPU id, **up to 155** | < the group's count |

Two Win32 calls disagree about the size of the machine by a factor of 2.5, and
a third can return a processor number larger than the count the other two
report.

## Why

`create_logical_proc_info()` (`dlls/ntdll/unix/system.c`) stops at a CPU whose
**index** reaches the width of an `ULONG_PTR`:

```c
if (i > 8 * sizeof(ULONG_PTR)) break;
```

That is the widest affinity mask one processor group can hold, and upstream
hard-codes the group count to 1. Where CPU numbering is dense — every x86 box —
index and count are the same thing, so the cap costs you the cores past 64 and
the FIXME above it ("only 64 supported") is accurate.

**POWER numbering is not dense.** A POWER8 running SMT4 out of an SMT8 part
reports:

```
/sys/devices/system/cpu/present   0-159
/sys/devices/system/cpu/online    0-3,8-11,16-19,24-27,...,152-155
```

Four online out of every eight slots. Index 64 arrives after only **33** online
CPUs, so the cap bites at 32 rather than at 64.

It is also unstable across SMT modes, which is worth knowing before anyone
"fixes" it by changing the machine's SMT setting:

| SMT mode | online set | CPUs the guest sees |
|---|---|---|
| SMT1 | `0,8,16,...` (20) | ~9 |
| SMT4 (today) | `0-3,8-11,...` (80) | **32** |
| SMT8 | `0-159` dense (160) | 64 |

## What it actually costs

Three separate costs, in descending order of how much they matter.

**1. Affinity requests are silently halved.** This is the sharpest one because
it is not a reporting bug — it changes what the scheduler does. Windows CPU *i*
is Linux CPU *i* (`server/thread.c:752`, `CPU_SET(i, &set)`), so a guest calling
`SetThreadAffinityMask(h, 0xFF)` — "give me eight processors" — asks for Linux
CPUs 0-7, of which **4-7 are offline**.

[MEASURED] op4k, `sched_setaffinity` with CPUs 0-7 set:

```
sched_setaffinity(cpus 0-7) = 0
actually runnable on 4 cpus: 0 1 2 3
```

The call **succeeds**. Nothing reports an error. The thread simply gets half the
parallelism it asked for. This is the same wrong-number-not-a-crash class as the
sub-word argument work ([`subword-abi-extension`](../thunks/), version 6/7): the
program runs, the answer is wrong, and no check that only asks whether the call
returned can see it.

**2. Worker pools are sized from the small number.** A guest that sizes its job
system from `GetActiveProcessorCount` builds 32 workers on an 80-way machine.

**3. The two counts disagreeing is itself a hazard.** Code that allocates an
array of `GetSystemInfo().dwNumberOfProcessors` entries and indexes it by
`NtGetCurrentProcessorNumber()` writes out of bounds here: 80 entries, an index
that can reach 155.

**But note what this is NOT.** It is not the current performance ceiling. The
port uses ~2.5 cores; 32 is not the binding constraint on that and neither would
64 be. The benefit is almost entirely **latent** — it is realised only after the
concurrency ceiling is lifted. That single fact drives the recommendation.

## The options

### A. Do nothing

Cost nothing. Guest sees 32 of 80, affinity requests keep being quietly halved.

### B. Dense renumbering, still one group

Windows CPU *n* becomes the *n*-th **online** Linux CPU rather than Linux CPU
*n*. Both ntdll and wineserver build the same table from
`/sys/devices/system/cpu/online`, which is deterministic and readable by both.

Gets the guest **64 of 80** — double today, and the first 64 are then real:
affinity means what it says, and `NtGetCurrentProcessorNumber` returns something
inside the reported range.

Does not reach 80. That needs option C.

### C. Real processor groups

Two groups of 40. Gets all 80 and makes `GetActiveProcessorGroupCount` honest.
Upstream has not done this and says so in the comment; every group-aware
structure (`GROUP_AFFINITY`, `PROCESSOR_NUMBER`, the `Ex` info classes) and both
sides of the affinity path would have to learn about it. Substantially larger
than B and built on top of it.

## The audit — what B would have to touch

Ten sites across two processes. The list is the risk, so it is spelled out.

**ntdll (`dlls/ntdll/unix/`)**

1. `system.c` `create_logical_proc_info()` — build the online table; `thread_mask` becomes `1 << dense_index` instead of `1 << i`
2. `system.c` `system_cpu_mask` (built at 1106, tested at 1726 and 2844, published at 1786) — becomes a dense mask
3. `system.c` NUMA `cpumap` parsing (1415) — the kernel gives a host-indexed bitmap; needs translating
4. `system.c` `/proc/stat` per-CPU times (2844) — **already separates `index` from `host_index`**; this one is a precedent for the pattern rather than a new problem
5. `system.c` `tsc_from_jiffies[MAXIMUM_PROCESSORS]` — indexed by host index, so it needs bounds or translation
6. `system.c` cpuset info (4290) — sized from `peb->NumberOfProcessors`; check only
7. `thread.c` `NtGetCurrentProcessorNumber()` (2956) — returns `sched_getcpu()` raw; needs the **reverse** table

**wineserver (`server/`)**

8. `thread.c` `set_thread_affinity()` (752) — `CPU_SET(i)` becomes `CPU_SET(online[i])`
9. `thread.c` `get_thread_affinity()` (772) — the reverse
10. `process.c` `set_process_affinity()` (1672) — inherits from the thread path

## The risks, honestly

**Two processes must agree on one table, forever.** ntdll and wineserver are
separate address spaces. Both can read `/sys/devices/system/cpu/online` and
build the table deterministically, so agreement is achievable — but if they ever
disagree, threads are pinned to the **wrong CPU**, silently, with no error
anywhere. That is a worse failure than today's, which at least is consistent.

**Hotplug and SMT changes invalidate it.** Today's code is equally wrong across
a runtime topology change; the difference is that after B the wrongness is an
active mispinning rather than a missing CPU.

**`sched_getcpu()` has no correct answer on a miss.** If the running CPU is not
in the table (hotplug, or a CPU past the 64 the table holds),
`NtGetCurrentProcessorNumber` has to invent something.

**Blast radius is every thread in every process.** Affinity is not a corner of
the system.

**Worst of all: the timing.** We are actively investigating a ~2.5-core
concurrency ceiling. Introducing a change that can mispin threads, in the middle
of that investigation, risks contaminating the very measurements meant to lift
it — and an affinity bug looks exactly like a concurrency bug.

**There is no gate for affinity.** B would need a new one: a guest sets an
affinity mask, and a native observer reads the thread's real Linux CPU set and
compares. That gate is genuinely worth having on its own merits, and it does not
depend on B — it can be built first.

## Recommendation

**Defer B until the concurrency ceiling is understood, then do it.**

The reasoning is not that B is hard — it is about ten sites and the `/proc/stat`
path already demonstrates the pattern. It is that:

* the benefit is **latent** (32 vs 64 changes nothing while the port uses 2.5
  cores), and
* the risk is **concentrated exactly where the current investigation is
  looking** (thread placement and concurrency).

Paying a real risk now for a benefit that cannot be observed until later, in the
one subsystem that would most confuse the work in flight, is the wrong trade.

**Two things worth doing before then, in this order:**

1. **Build the affinity gate.** It has value today — it would have caught the
   silent halving above — it is independent of B, and it is what makes B
   reviewable when the time comes.
2. **Fix `NtGetCurrentProcessorNumber` returning an out-of-range value.** This
   one is separable from the renumbering: clamping or translating it stops a
   guest indexing an 80-entry array with 155, which is a memory-safety bug
   rather than a performance one. Small, contained, and does not touch affinity.

**The condition that should flip this to "do it now":** a corpus title whose
behaviour demonstrably depends on the processor count or on affinity — a job
system that refuses to scale past what `GetActiveProcessorCount` reports, or a
title that pins threads. At that point the benefit stops being latent and B
becomes the cheapest way to get it.

## Related

* `dlls/ntdll/unix/system.c` — the analysis is repeated at the site, above the
  enumeration loop, so it is found by whoever reads the FIXME
* `ppc64le/corpus/CATALOG.md` — where a title that turns out to depend on this
  should be recorded
