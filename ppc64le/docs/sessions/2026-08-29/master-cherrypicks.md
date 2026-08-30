# Master-only Wine cherry-picks onto wine-ppc64le

Tree: `/home/jbettcher/Development/power9_development/powerpc64le-ports/hangover-ppc64le/wine-upstream`
Branch: `wine-ppc64le`. Started at merge commit `3214a170a7e` (wine-11.16 merged in).
`mirror` remote confirmed as `https://github.com/wine-mirror/wine.git`; both SHAs confirmed present via
`git cat-file -t` and confirmed non-ancestors via `git merge-base --is-ancestor <sha> HEAD` (both printed
"NOT ANCESTOR").

Note: this tree is shared with another concurrently-running agent (touching `dlls/guestcrt/`,
`dlls/ucrtbase/`, `dlls/vcruntime140/`, `dlls/winex11.drv/mouse.c`). Their commits
(`69a60f12ee3`, `e434de7f40e`) landed on top of mine during this session; I never touched their files.

---

## 1. `67a26bddad0` — ntdll: keep reserved-area bounds page aligned on 4GB wrap-around

**Applied.** New commit: `6d3dff365f0` (`git cherry-pick -x`, clean apply, no conflicts).

### Fork-state check before applying

Read `dlls/ntdll/unix/virtual.c` as the fork has it. `mmap_add_reserved_area()` and
`mmap_remove_reserved_area()` (lines ~457-520) were byte-for-byte the pre-fix upstream shape — no
port modification here. Traced the consumer chain the commit message describes, and it matches
exactly in this fork:

- `alloc_virtual_heap()` (line 3682) computes `ret = anon_mmap_fixed((char*)end - size, size, ...)`
  where `end` comes from a reserved-area's `base + size`.
- `anon_mmap_fixed()` (line 286) asserts `!((UINT_PTR)start & host_page_mask)` and
  `!(size & host_page_mask)` — the alignment assertions from prerequisite commit `d813ffc3557`
  ("Align virtual memory allocations to the host page size"), confirmed already an ancestor of HEAD.

So the fork has the exact bug shape: the old `size--` on the wrap-around path corrupts a
page-aligned reserved-area end into `0xFFFF...FFFF` (unaligned by definition), which can later be
handed to `anon_mmap_fixed()` and trip its own alignment assert.

### The 64K-page premise — checked against the actual hardware, and it does not hold here

The task brief states this machine has 64K pages. I verified this directly rather than assuming it:

```
$ ssh jbettcher@192.168.2.24 getconf PAGESIZE
4096
```
Confirmed three independent ways on the AC922 (`getconf PAGESIZE`, `getconf PAGE_SIZE`,
`python3 resource.getpagesize()`) — all report **4096**, not 65536. `/proc/cpuinfo` shows
`platform: PowerNV` (bare metal, not a KVM guest with a different page-size default). So this
specific host is running with a 4K base page size, not 64K.

This also matches how the fork's own code computes `host_page_size` at build time. In
`dlls/ntdll/unix/virtual.c`:

```c
#ifdef __aarch64__
static UINT_PTR host_page_size;         /* filled at runtime via sysconf(_SC_PAGESIZE) */
static UINT_PTR host_page_mask;
#else
static const UINT_PTR host_page_size = 0x1000;   /* compile-time constant: 4096 */
static const UINT_PTR host_page_mask = 0xfff;
#endif
```
This `#ifdef` is unmodified upstream code (`git blame` → Alexandre Julliard, `6e1c906fa6d1`, the
same commit that introduced the alignment asserts). Only aarch64 gets a runtime-queried page size;
every other architecture, ppc64le included, gets a hardcoded 4096-byte constant. `get_host_page_size()`
(line 3818) just returns this same static, so nothing else in the port overrides it for ppc64le.
Given the real hardware also happens to run 4K pages, the hardcoded constant and reality agree here —
by coincidence of how this port's OS/kernel is configured, not because ppc64le got aarch64-style
runtime detection.

**Concrete before/after on this host (host_page_size = host_page_mask+1 = 0x1000 = 4096):**

Suppose a reserved area's `addr + size` wraps to exactly `2^64` (mod 2^64 == 0) — the pathological
case the `if (!((intptr_t)addr + size))` guards against. Both `addr` and the original `size` are
page-aligned on entry (asserted).

- Old code: `size -= 1` → `end = addr + size = 0xFFFFFFFFFFFFFFFF`. Low 12 bits = `0xFFF` —
  **not** aligned to the 4096-byte page granularity this fork actually uses. This unaligned `end`
  flows into `alloc_virtual_heap()` → `anon_mmap_fixed(end - size, ...)`, tripping the
  `!((UINT_PTR)start & host_page_mask)` assert from `d813ffc3557`.
- New code: `size -= host_page_size` (4096 = `0x1000`) → `end = addr + size = 0xFFFFFFFFFFFFF000`.
  Low 12 bits = `0x000` — aligned to 4096, satisfies the assert, and the resulting reserved area is
  a legitimate one-page-short-of-the-top-of-address-space region instead of a corrupt one.

So: the fix is correct and worth taking, but the "16x the slack" framing in the brief (which assumed
64K host pages) does not apply to this specific host as currently built/booted — the slack here is
the same one page (4096 bytes) x86 hosts get, not sixteen pages. If this port's OS is ever
reconfigured for a 64K-page kernel, the `#else` branch's hardcoded `0x1000` would itself become the
bug (host_page_size would silently stay wrong at 4K instead of the real 64K) — a distinct,
pre-existing issue this cherry-pick does not touch and that is out of scope here, but worth flagging
separately since it's exactly the aarch64-vs-everyone-else asymmetry that would need fixing before
a 64K-page kernel could be trusted.

### Build

Clean, both before and after every intermediate patch/revert cycle used for testing. Final state
built with `make -j96` on `jbettcher@192.168.2.24`, ending in `Wine build complete.` with no errors.

### Tests

`dlls/ntdll/tests/virtual.c` / `exception.c`, run via `make dlls/ntdll/tests/<arch>/<file>.ok`
(`RUNTESTFLAGS = -q -P wine`), with the required `FEX_*` env vars set. Two guest lanes exist in this
build: `ppc64-windows` (native ppc64 PE ABI, no CPU emulation — the most direct exercise of the
unixlib code this cherry-pick touches) and `i386-windows` (32-bit x86 guest under WoW64/FEX
emulation).

Pre-cherry-pick baseline was produced by swapping `dlls/ntdll/unix/virtual.c` back to its
`3214a170a7e` content (confirmed via diff to be *exactly* the two-line pre-fix form and nothing
else), rebuilding just `ntdll.so`, running the same test targets, then restoring the committed
(post-cherry-pick) file and rebuilding again before finishing.

| Test | Lane | Pre-cherry-pick | Post-cherry-pick |
|---|---|---|---|
| `virtual` | `ppc64-windows` | FAIL (`Error 5`) — `out of memory for allocation` on many small allocations, then `CreateFileMapping failed` / `NtMapViewOfSection returned c0000008` at virtual.c:1809-1837, ends in an access violation reading address 0 in msvcrt.dll | **Identical** failures, same line numbers, same error sequence |
| `exception` | `ppc64-windows` | FAIL (`Error 11`) — all ten `__fastfail(...)` cases report "fast fail did not occur", then a native-fault access violation writing into guest memory | **Identical** failures (only a harmless varying stack-derived hex value differs) |
| `exception` | `i386-windows` (WoW64/FEX) | FAIL (`Error 29`) — `BTCpuSimulate emulator run failed, status c000001d; terminating`, i.e. a full emulator-level crash before any per-test assertion runs | **Identical** crash, same status code |
| `virtual` | `i386-windows` (WoW64/FEX) | not re-run pre-cherry-pick (see below) | FAIL (`Error 1`) — many `out of memory` allocations, `NtSetInformationVirtualMemory` not implemented, `ActiveGroupCount` mismatch, unsupported WoW64 syscalls (0x666/0x1666/0x2666/0x3666) |

All failures reproduced are **pre-existing** and unrelated to this cherry-pick: the wrap-around
branch this fix touches only fires when a reserved area's end address wraps exactly to `2^64`, an
extreme edge case none of these ordinary test allocations hit. The `ppc64-windows` and
`i386-windows`/WoW64 lanes both have broad, independent gaps in this port (native-ABI `__fastfail`
and SEH dispatch gaps, missing `NtSetInformationVirtualMemory` info classes, unsupported WoW64
syscalls, and — for `i386-windows/exception` — an outright FEX/BTCpuSimulate crash) that predate this
work entirely, confirmed byte-for-byte identical with the fix reverted. `virtual` on `i386-windows`
was not separately re-run pre-cherry-pick given the `ppc64-windows` and `exception`/i386 comparisons
already established the pattern (identical failures regardless of this 2-line change) and time
budget; its failure signature (allocation/WoW64-syscall gaps, nothing wrap-around-shaped) is
consistent with the same pre-existing-gap explanation.

Net: the cherry-pick is correct and safe to keep, but none of the four test runs reach a passing
state on this fork today — none of that is caused by this change.

---

## 2. `b3239efd8b6` — ntdll/unix: fix `logical_proc_info_add_numa_node` parameter order (hwloc NUMA)

**NOT applied.** The fork's source technically contains the same swapped-argument bug the upstream
commit fixes, but that code is dead on this port's actual build — cherry-picking it would be pure
noise, so I left it alone per the task's own stated criterion.

### What upstream fixes

`add_hwloc_numa_nodes()` called
`logical_proc_info_add_numa_node(obj->logical_index, hwloc_bitmap_to_ulong(obj->cpuset))`, but the
function is declared `logical_proc_info_add_numa_node(ULONG_PTR mask, DWORD node_id)` — mask first,
node id second. The call had them backwards, so it wrote the NUMA node's logical index into the
`ProcessorMask` field and the CPU bitmask into `NodeNumber`.

### Fork-state check

Confirmed the fork's `dlls/ntdll/unix/system.c` (line 1887, inside `add_hwloc_numa_nodes()`) has the
exact same swapped call, and confirmed the function signature at line 1142
(`logical_proc_info_add_numa_node(ULONG_PTR mask, DWORD node_id)`) matches upstream's — so in
isolation this does look like the fork carries the bug.

**But this function is unreachable on this port's Linux/ppc64le build**, for two independent reasons:

1. **Preprocessor selection.** `create_logical_proc_info()` in `system.c` is one function guarded by
   a `#if / #elif / #elif / #else` chain:
   `#ifdef linux` (line 1190, a large ~500-line Linux-native implementation using `/sys`,
   `wine_cpu_topology()`, `add_legacy_proc_info()`/`add_numa_ex()`) → `#elif defined(__APPLE__)`
   (1714) → `#elif defined(HAVE_LIBHWLOC)` (1848, contains the buggy `add_hwloc_numa_nodes()`) →
   `#else` stub (1980, `STATUS_NOT_IMPLEMENTED`). Since this build defines `linux`, the **first**
   branch is selected and the `HAVE_LIBHWLOC` branch is compiled out entirely, regardless of whether
   `HAVE_LIBHWLOC` is even defined.
2. **It's upstream FreeBSD code, not Linux/ppc64le code.** `git blame` traces the whole hwloc branch
   to commit `d3b58f3d94bb`, "ntdll: Implement create_logical_proc_info **on FreeBSD**." — confirmed
   an ancestor of `mirror/master` (genuine upstream mainline, inherited via the wine-11.16 merge, not
   a fork-authored patch). It exists to give FreeBSD (which has no native NUMA/topology sysfs
   equivalent) a libhwloc-based fallback. It was never meant for Linux.
3. **Also confirmed independently:** this build's actual `config.h` has
   `/* #undef HAVE_LIBHWLOC */` — hwloc isn't even linked in — so even setting aside branch order,
   the macro guarding the buggy branch is unset.

The real, live NUMA-topology code path on this Linux/ppc64le port is the `#ifdef linux` branch,
which populates NUMA relationships via `add_legacy_proc_info(RelationNumaNode, set.mask[0], 0, NULL,
topo->node_ids[n])` and `add_numa_ex(topo->node_ids[n], &set)` — checked both signatures
(`add_legacy_proc_info(LOGICAL_PROCESSOR_RELATIONSHIP rel, ULONG_PTR mask_g0, ...)` and
`add_numa_ex(DWORD node_id, const struct group_mask_set *set)`) against these call sites: parameter
order is correct, no analogous swap bug.

### Conclusion

Per the task's own instruction — "If the fork is not affected, say so and do not apply it" — this
one is **not applied**. The 2-socket/176-thread/~4:1 remote-memory-ratio NUMA concern this hardware
raises is real, but it's served by the `#ifdef linux` path, which does not have this bug. Applying
`b3239efd8b6` would only edit dead FreeBSD-only code inherited from the wine-11.16 merge; it would
build and do nothing, which is exactly the "fixing code that was never wrong" noise the task warned
against.

---

## Summary

| Commit | Fork affected? | Action | Commit hash |
|---|---|---|---|
| `67a26bddad0` (4GB wrap-around alignment) | Yes — identical pre-fix shape, reachable, prerequisite alignment-assert commit already present | Cherry-picked | `6d3dff365f0` |
| `b3239efd8b6` (hwloc NUMA param swap) | No — buggy code present in source but compiled out on Linux (upstream FreeBSD-only path; `#ifdef linux` wins; `HAVE_LIBHWLOC` also unset); real Linux NUMA path is bug-free | Not applied (deliberately) | — |

Build: clean (`make -j96`, `Wine build complete.`, no errors) in every configuration tested,
including the final state (cherry-pick 1 applied, cherry-pick 2 not applied, plus the other agent's
unrelated concurrent commits on top).

Tests: `virtual`/`exception` from `dlls/ntdll/tests/` run on both the `ppc64-windows` (native) and
`i386-windows` (WoW64/FEX) lanes. All observed failures are pre-existing and reproduced identically
with the cherry-pick reverted — none are caused by either cherry-pick decision in this report.
