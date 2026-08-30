# Upstream merge assessment: wine-ppc64le fork vs mirror/master (265 commits)

Date: 2026-08-29.  All git analysis was read-only (`git log/show/diff/grep`,
`git merge-tree --write-tree` which writes only loose objects, never the
working tree or index).  Fork state was pinned at `f15604cf0ea` (HEAD at
analysis time; the tree is being actively committed to, so re-pin before
acting).

## Merge policy (owner's constraint — governs every resolution below)

**Embrace and extend, asymmetrically.** Upstream fixes and improvements are
TAKEN.  The fork's port functionality is NEVER dropped — not in a conflict,
not as a tiebreak, not because a hunk looks unfamiliar.  The failure mode this
plan is designed against is not a conflict marker (visible, gets resolved) but
a **silent ejection**: a three-way merge resolving an adjacent-hunk pair by
taking theirs, or a resolver reading a port-specific change as noise.  That
produces a green build with a capability quietly gone.  Countermeasures:
the per-file inventory in section 2 (know what must survive *before*
resolving) and the mechanical no-ejection check in section 6 (prove it
survived *after*).

Reference points (MEASURED):

| ref | sha | position |
|---|---|---|
| merge base (wine-11.15 point) | `e99fc2f7587` | fork's upstream base |
| fork `wine-ppc64le` | `f15604cf0ea` | 219 commits ahead of base |
| `wine-11.16` tag | `d8a16d5cead` | **109** commits above base |
| `mirror/master` | `2ecc2f84b45` | 265 above base (156 past the tag) |

## Headline (MEASURED)

`git merge-tree --write-tree` of the fork against **both** `wine-11.16` and
`mirror/master` produces **exactly two conflicted files, identical in both
cases**: `server/protocol.def` and its generated `include/wine/server_protocol.h`.
Every other overlap file — all 24, including `virtual.c`, `unwind.h`,
`vulkan.c`, both `thread.c`s — auto-merges textually clean.  The merge is far
cheaper than the 26-file overlap suggested.  The work is (a) one small
keep-both hand merge in `protocol.def`, (b) regeneration of the three
generated protocol files plus `configure`, and (c) verification — including
the no-ejection proof — which is where all the real cost lives.

Note the flip side of "24 files auto-merge clean": those are precisely the
files where git decided on its own, with nobody looking.  That is the silent
ejection surface, and section 6's check covers exactly those files.

---

## 1. What is in the 265 commits, by area

Split at the `Release 11.16` commit (`8da89f8493b`): 109 commits are in the
tag, 156 are mid-cycle work after it.  Tag membership of every commit named
below was verified with `git merge-base --is-ancestor` (MEASURED).

### Inside wine-11.16 (109 commits)

* **ntdll virtual memory** — the biggest guest-visible block:
  `4ac0555e55c` + 4 test commits validate mapped/allocated ranges against the
  *user* address limit (map_view now returns `STATUS_INVALID_PARAMETER`
  instead of `STATUS_WORKING_SET_LIMIT_RANGE` for beyond-limit fixed-address
  maps); `1276773f698` map_free_area limit clamping; `f4c5b04148d` reserves
  `0xfff00000-0xffff0000` for large-address-aware 32-bit processes;
  `548a639e11b`/`3b05fbb6166` MEM_PHYSICAL/MEM_LARGE_PAGES parameter checks;
  `a081733d481`/`ce72727a654` old-wow64 cross-process limits (fork's 32-bit
  lane is **new** wow64 → `is_old_wow64()` false, dormant); `9d7c45f0179`
  wow64 extended-parameter low-limit validation (**live** for the fork's
  real-wow64 32-bit lane).
* **Wineserver protocol / thread context**: `4423e8ed9aa` splits
  `arm64_regs.x[31]` into `x0[18]/x19[12]`, adds a `tls` union +
  `SERVER_CTX_TLS` (0x0100) and renumbers the SERVER_CTX defines to
  4-hex-digit form (values unchanged); `178f11f1c57` adds a `visible` rect to
  the `get_window_rectangles` reply.  That is the whole protocol delta and
  the entire cause of the conflict.
* **Server window/input**: thread-input attachment refcounting
  (`c28e3a549fc` + 4), `set_window_rect_visible` thread fix, focus/active
  updates in `assign_thread_input` — guest-visible focus behavior for games.
* **Exception/emulation machinery**: `8c284041138` preserves exception flags
  in the x64↔arm64 `ctx_flags_*` converters in `dlls/ntdll/unwind.h`;
  `0b8d0addf6a` RtlRaiseException entry-thunk change; three
  KiUserEmulationDispatcher commits (`f12bd89a4bd`, `d3b41a854a8`,
  `3b6b0cedd98`) — ARM64EC-host plumbing in `signal_arm64.c`/`signal_arm64ec.c`,
  files the fork does not modify; no interaction with `signal_ppc64.c`
  dispatch (INFERRED from file disjointness).
* **Graphics**: winex11 **child D3D presentation on fullscreen-exclusive
  toplevels** (`3394b734479`) and offscreen-present monitor-rect fix
  (`94e27fa07b1`) — directly under the fork's gated present path; win32u
  framebuffer-/client-surface latching work; opengl32 scissor/FBO fixes;
  winex11 GL_EXTENSIONS use-after-free fix (`f68eb9e43d0`); wined3d
  delay-clear correctness (`01e1aeaa3cd`); **`9306b8e8de4` fractional raw
  mouse motion — IN-11.16 (MEASURED), lands in the same
  `dlls/winex11.drv/mouse.c` functions as the fork's stashed raw-input
  valuator fix; see sequencing note in section 5.**
* **Runtime/CRT**: msvcrt std-handle inheritance fixes, `_isatty` x64 asm
  wrapper; RtlBarrier count fix (`e96bcf9eebf`); kernelbase
  ReadConsoleInputEx (spec stub→real export); Wine Mono 11.3 (`29a87db3c71`).
* Plus d3dx10 sprites, gdiplus metafile, secur32/msv1_0/kerberos wow64
  SecBuffer mapping fixes, comctl32 tab, drivers' IRP_MJ_CREATE, winecoreaudio.

### After the tag (156 commits, master only)

* **win32u/opengl32 mid-cycle GL rework** (largest block): emulated
  display-mode scaling, gamma-ramp emulation, SRGB framebuffers, and
  `6e07c6c1c67` changing `get_unused_client_surface()` to a 3-argument
  signature — **breaks the fork's added vulkan.c code** (see table).
* **lsass/SamSs security rework**: new `programs/lsass` + SamSs service
  (`53e2e4d8a34`), secur32 calling LSA mode functions in the lsass.exe
  process, configure.ac + wine.inf plumbing.  A new required service process
  and a prefix-schema change, still landing in pieces.
* **setupapi SetupDiGetClassDevs rework** (556 lines, cfgmgr32-based).
* **ntdll**: `2f69c014dc2` moves `is_ec_code()` out of `unix_private.h`
  (fork's only user is base `signal_arm64.c`, not compiled on ppc64le —
  harmless); `67a26bddad0` **fixes the 4GB wrap-around reservation to
  subtract `host_page_size` instead of 1 byte** — only matters on hosts with
  pages > 4K, i.e. exactly a 64K-page POWER9 host (flagged for the
  fix-hunting agent); `b3239efd8b6` fixes swapped parameters in a
  `logical_proc_info_add_numa_node` call in the **hwloc** NUMA path — the
  fork's rewritten topology code still contains `add_hwloc_numa_nodes`
  (fork system.c:1881), possibly a live bugfix (flagged, not verified);
  `fd3fbe3ef37` macOS main-thread spawn (adds `else if (data->start)` to
  `server_init_thread`; Linux behavior unchanged).
* **vkd3d 2.1 in-tree import** (`9226b10cd1d`) — the fork's d3d12 is
  DXVK/vkd3d-proton behind thunks, but in-tree `libs/vkd3d` still builds.
* `d8bb13b7019` winegcc CPU_ARM64EC multiarch dir (adjacent to, not
  touching, the fork's ppc64le cases).
* Bulk quality: winhttp (8), webservices (13), msado15, msxml3, wbemdisp,
  locale-pinned tests, include fixes, translations.

**Not touched anywhere in the 265** (MEASURED via name-only diff): winebuild,
`libs/wine`, the module loader (`dlls/ntdll/loader.c`,
`dlls/ntdll/unix/loader.c`), `loader/` except `wine.inf.in` (post-tag,
lsass), `dlls/wow64*` except `dlls/wow64/virtual.c`.  The fork's
highest-value machinery — spec2thunk output, elf2pe, `signal_ppc64.c`,
winecom, the thunk boundary — has **zero upstream overlap** and cannot be
ejected by this merge.

**Also not touched: `dlls/winex11.drv/mouse.c`'s raw-input valuator gate** —
zero upstream commits fix it (finding relayed from the other agent).  But
`9306b8e8de4` (in-tag) edits those same functions.

`ppc64le/NEXT.md:539`'s wanted `group` field for `SetThreadGroupAffinity`:
upstream's protocol.def delta touches only `context_data` and
`get_window_rectangles` — nothing near the affinity requests; the planned
field remains free to add (MEASURED).

---

## 2. Fork-owned inventory: what must survive, file by file

Extracted from `git diff e99fc2f7587..f15604cf0ea` per file (MEASURED).
This is the read-this-first list for whoever resolves the merge, and the
input to the section-6 mechanical check.  "Ours" below is never dropped;
where upstream edits the same file, both sides' hunks were verified disjoint
unless stated.

| File | Fork-owned material (must survive) |
|---|---|
| `server/protocol.def` | Four `powerpc64_regs` arms in `context_data`: ctl (`iar, msr, ctr, lr, dar, dsisr, trap, gpr1`), integer (`gpr[32], cr, xer`), fp (`fpr[32], fpscr, vr[32], vscr, vrsave`), debug (`dr[8]`). Nothing else. |
| `include/wine/server_protocol.h`, `server/request_handlers.h`, `server/trace.c` (generated part) | Regenerated from protocol.def — survival is automatic **iff** step 2's hand merge keeps the ppc64 arms. |
| `server/trace.c` (hand-written part) | +47 lines: powerpc64 context dump arms in the dump helpers above the generated-section markers. |
| `server/thread.c` | +238 lines: `cpu_map_disabled()`, `affinity_bit_to_unix_cpu()`, `affinity_is_unrestricted()`, `affinity_names_a_processor()` and the sparse-CPU/processor-group affinity handling around them. Upstream's only edit is +1 line in `copy_context` (SERVER_CTX_TLS). |
| `dlls/ntdll/unix/virtual.c` | PROT_SAO/HWTSO wiring: `VPROT_NOSAO` (0x80), `mprotect_hwtso()`, `virtual_enable_hwtso()`, the `thread_data_cache` initial-exec TLS variable, `get_system_affinity_mask()`, plus hunks in `get_unix_prot`, `get_vprot_flags`, `mprotect_exec`, `virtual_map_image`, `virtual_init_user_shared_data`, `virtual_uninterrupted_write_memory` region, `NtProtectVirtualMemory`, `NtWriteVirtualMemory`, `NtFlushInstructionCache`. Upstream touches **none** of these functions except `allocate_virtual_memory` (fork +1 line there). |
| `dlls/ntdll/unix/thread.c` | ppc64 arms in `get_server_context_flags`, `context_to_server`, `context_from_server` (all the `*.powerpc64_regs.*` lines: gpr/fpr/vr/dr transfer loops, the gpr1-as-sp note), an addition in `get_thread_context`, and the `NtGetCurrentProcessorNumber` rework. Upstream edits the arm64 arms of the same three functions — disjoint case arms, verified. |
| `dlls/ntdll/unix/server.c` | `server_wait`'s clock fix (uses `server_monotonic_time()` instead of `NtQueryPerformanceCounter` — the ppc64le-lane clock-domain comment explains why) and `thread_data_cache = data;` in `server_init_thread`. |
| `dlls/ntdll/unix/system.c` | The topology subsystem: `ntdll_no_cpu_groups()`, `ntdll_no_cpu_map()`, `init_cpu_model()`, `get_cpu_features()`, `init_shared_data_cpuinfo()`, `group_mask_set_*`, `add_cache_ex`/`add_numa_ex`/`add_group_ex`, `ntdll_cpu_topology()`, plus the rewritten `create_logical_proc_info` and cpuset code (~380 lines). Upstream: one line in `add_hwloc_numa_nodes`. |
| `dlls/ntdll/unix/unix_private.h` | ppc64 `current_machine`/machine helpers, `struct thread_data` extensions (+32 lines), `thread_data_key`/`thread_data_cache` externs, `get_system_affinity_mask`, `virtual_init_user_shared_data`, `KeServiceDescriptorTable[4]`, `call_raise_user_exception_dispatcher`-area additions (+12). Upstream (master only) deletes the nearby `is_ec_code` inline — keep the deletion, keep our additions; `is_inside_syscall`, which `signal_ppc64.c` uses, survives upstream. |
| `dlls/ntdll/unwind.h` | +25 lines: `RtlLookupFunctionEntry_amd64`, `RtlVirtualUnwind2_amd64`, `RtlVirtualUnwind_amd64` externs and the guest-unwinder glue around `context_arm_to_x64`. Upstream edits two `ctx_flags_*` one-liners elsewhere in the file. |
| `dlls/ntdll/tests/exception.c` | +49 lines inside existing tests (no new test functions). Low value density; verify via the section-6 line check rather than by symbol. |
| `dlls/win32u/vulkan.c` | `create_host_swapchain()`, `PRESENT_MODE_END`, the `present_mode_fallbacks[][4]` table, `hwnd_surface_create/update/presented/destroy`, `__wine_get_hwnd_surface_funcs()` (+271 lines; the swapchain image-count rationale is `ppc64le/vulkan/win32u-swapchain-image-count.diff`). Upstream's whole delta is post-tag and one line. |
| `dlls/opengl32/wgl.c` | `__wine_gl_entry_point()` (+37 lines, guest GL thunk entry). No upstream 11.16 delta at all. |
| `dlls/kernelbase/thread.c` | +194 lines: `switch_fiber` ppc64 asm (`__ASM_GLOBAL_FUNC`), the ppc64 `CONTEXT` offset `C_ASSERT` block (Fpr14/Gpr1/.../Vr[20]). No upstream 11.16 delta (upstream's −25 UI-language rework is post-tag). |
| `dlls/kernelbase/memory.c` | `PROCESSOR_ARCHITECTURE_PPC64` case in GetSystemInfo's processor-type switch (+8). |
| `dlls/kernel32/kernel32.spec` | Corrected signatures: `RtlUnwindEx(ptr ptr ptr ptr ptr ptr)`, `VerSetConditionMask(int64 long long)` — thunk-marshalling accuracy; a revert to base would silently mis-marshal those two through spec2thunk. Upstream's ReadConsoleInputEx lines are elsewhere in the file. |
| `dlls/kernelbase/kernelbase.spec` | 1-line fork change (same class). |
| `include/winnt.h` | +335 lines: `PROCESSOR_ARCHITECTURE_PPC64` (200), `KNONVOLATILE_CONTEXT_POINTERS_AMD64`, the AMD64_CONTEXT/RUNTIME_FUNCTION/SCOPE_TABLE aliasing block, ppc64 CONTEXT definitions. Upstream: 1 line elsewhere. |
| `include/winbase.h` | +3 prototypes (`GetPhysicallyInstalledSystemMemory`, `SetThreadSelectedCpuSets`, ...). |
| `include/Makefile.in` | +4: `wine/cputopology.h`, `wine/emu_qpc.h`, `wine/winecom.h`, `wine/winecom_selftest.h`. Upstream adds one unrelated header — both must survive. |
| `dlls/secur32/Makefile.in` | +1: `secur32.thunks`. Upstream's change is post-tag. |
| `dlls/setupapi/devinst.c` | +2: `NtPlatformExtension[] = L".NTppc64"` (`__powerpc64__` branch). Trivial but load-bearing for INF platform matching; vs master's 556-line rework, re-verify placement by hand. |
| `configure.ac` / `configure` | +217: the whole ppc64le arch detection/toolchain block. Upstream 11.16 does not touch configure.ac; `configure` is regenerated anyway. |
| `tools/winegcc/winegcc.c` | The `CPU_POWERPC64` case in `get_compat_defines` (maps `__stdcall/__cdecl/__fastcall` to no-ops — no MS ABI exists for PPC64) and the `CPU_POWERPC64` exemption in the non-PE build check at main() (~line 2014). Upstream adds an ARM64EC case in `get_multiarch_dir` — different function; note the fork has **no** ppc64 case in `get_multiarch_dir`, so upstream's edit is take-theirs. |

**Pending work adjacent to the merge (not in the 26):**
`dlls/winex11.drv/mouse.c` + `x11drv.h` — the fork has NO committed delta
(MEASURED: empty diff), but `stash@{0}` carries the raw-input valuator gate
fix, and in-tag `9306b8e8de4` changes those same functions (+10/+2).  The
stash would not conflict with the merge (stashes are not merged), but if
applied before the merge it turns a non-overlap file into an overlap file,
and if applied after, its context lines have moved.  **Sequencing: merge
first, then rewrite the mouse fix against the post-merge shape of
`x11drv_XI2_ProcessRawEvent`/friends.  Do not pop the stash into the merge.**

---

## 3. Per-file conflict table, ranked by risk

"Merge-tree" is MEASURED (identical vs both targets unless noted).  "Take
theirs would cost" states the wholesale-theirs damage per policy — none of
these are acceptable outcomes; the column exists so a resolver recognizes
the stakes at a glance.

| # | File | Merge-tree | Risk | Ownership & finding | Take-theirs would cost |
|---|---|---|---|---|---|
| 1 | `server/protocol.def` | **CONFLICT** | HIGH (mechanical but wire-format-bearing) | Both sides edit `context_data`; the conflict is adjacent-line in the `integer` union. Resolution: **keep both** — upstream's arm64 split + `tls` union + SERVER_CTX renumbering + `visible` rect, AND our four `powerpc64_regs` arms. They compose; neither subsumes the other; ppc64 arms never reference `arm64_regs.x[]`. | Every guest/server context transfer: threads, suspend, debugger, SEH. The port dies undramatically — this is the one file where "take theirs" is catastrophic and *looks* plausible to a naive resolver because our arms sit inside upstream-restructured unions. |
| 2 | `include/wine/server_protocol.h` | **CONFLICT** | none once regenerated | GENERATED — never hand-merge. Versions: base 959, fork 962, upstream 961; `tools/make_requests` auto-bumps (+1 on change) → 963, dissolving the version conflict. | n/a (regenerated) |
| 3 | `server/request_handlers.h` | clean | none once regenerated | GENERATED; the clean merge is untrustworthy by construction. Regenerate. | n/a |
| 4 | `server/trace.c` | clean | LOW | Ours: hand-written ppc64 dump arms (top section). Theirs: generated-section changes. Accept the clean merge, run make_requests, then confirm the dump arms survived (section 6). | Guest context invisible in server traces — a debugging capability silently gone; exactly the ejection shape to check for. |
| 5 | `dlls/ntdll/unix/thread.c` | clean | **MED** | Both sides edit `get_server_context_flags` / `context_to_server` / `context_from_server` — ours are ppc64/guest-x64 case arms, theirs arm64 arms + SERVER_CTX_TLS. Disjoint, verified; they compose. Theirs also adds `apple_spawn_main_thread` (`#ifdef __APPLE__`). Eyeball all three functions post-merge. | Guest thread context machinery — the port's core. Highest-value ejection target in the auto-merged set. |
| 6 | `dlls/ntdll/unix/virtual.c` | clean | **MED** | Ours: PROT_SAO/HWTSO (see inventory) — upstream touches none of those functions. Theirs: limit validation, MEM_PHYSICAL/LARGE_PAGES, old-wow64. One shared function (`allocate_virtual_memory`: ours +1 line, theirs ~50 lines above) — composes; read it. Watch: `map_view` now rejects fixed maps beyond `user_space_limit` with a new status — grep ppc64le code for fixed-address mappers post-merge (INFERRED risk). | HWTSO — the port's whole memory-ordering story for TSO guests. An ejection here doesn't fail a build OR necessarily a smoke gate; it costs correctness under contention. Priority #1 for the section-6 check. |
| 7 | `server/thread.c` | clean | LOW-MED | Ours: sparse-CPU affinity subsystem. Theirs: +1 line `copy_context` SERVER_CTX_TLS. Compose. Regenerate protocol before building (theirs needs the `tls` union). | All-cores topology/affinity (`check-cpu-topology.sh` gate catches it). |
| 8 | `dlls/ntdll/unwind.h` | clean | LOW-MED | Ours: `Rtl*_amd64` unwinder externs + glue. Theirs (in-tag `8c284041138`): exception-flag preservation in `ctx_flags_x64_to_arm`/`arm_to_x64`. Different regions; compose. **Follow-up**: our own guest-x64↔host converters in `signal_ppc64.c` may harbor the same flag-dropping bug upstream just fixed — audit; a build never catches it (INFERRED). | Guest SEH unwinding — seh gates catch total loss, not partial. |
| 9 | `dlls/win32u/vulkan.c` | clean | vs 11.16: NONE — vs master: MED, loud | Ours: `create_host_swapchain()` + fallback table + hwnd-surface plumbing. Theirs: post-tag one-liner only (`get_unused_client_surface` 3-arg). **Compose; ours is neither subsumed nor invalidated** — upstream never touched swapchain creation or image-count normalization; the Mesa min-image-count behavior our patch corrects is still unaddressed upstream. Vs master, our second call site (fork line 3346, `get_unused_client_surface(hwnd, 0)`) fails to compile → add the third arg. MEASURED. | The image-count normalization games rely on ("Windows renderers size fixed arrays from minImageCount"). Ejection = index-out-of-bounds class corruption in titles, possibly far from the swapchain in symptoms. |
| 10 | `dlls/ntdll/unix/unix_private.h` | clean | LOW | Ours: thread_data extensions + ppc64 machinery (inventory). Theirs vs 11.16: nothing; vs master: `is_ec_code` inline removed (our user is base `signal_arm64.c`, not compiled on ppc64le), `is_emulated_code` extern added. Compose. | thread_data layout — but any ejection here breaks the build loudly (signal_ppc64.c consumes these). |
| 11 | `dlls/ntdll/unix/server.c` | clean | LOW | Ours: `server_wait` clock-domain fix + `thread_data_cache` line. Theirs vs 11.16: nothing; vs master: Apple spawn + `else if (data->start)` in the same `server_init_thread` — different lines, composes. | The server_wait fix's loss = subtle absolute-timeout drift on the ppc64le lane only — the definition of silent. On the section-6 list. |
| 12 | `dlls/ntdll/unix/system.c` | clean | LOW | Ours: whole topology subsystem. Theirs vs 11.16: nothing; vs master: hwloc NUMA one-liner (composes; verify our rewrite executes that path). | CPU topology/groups (gated). |
| 13 | `dlls/ntdll/tests/exception.c` | clean | LOW | Ours: +49 in existing tests. Theirs: +124 new address-limit tests. Compose. New upstream tests run as x86-64 guests here and may newly fail under emulation — signal, not damage. | Test coverage only. |
| 14 | `configure`/`configure.ac` | clean | LOW (regenerate) | Ours: ppc64le arch block. Theirs at 11.16: `configure` version bump only (`VERSION` merges clean → 11.16); at master: +2 lsass lines. Regenerate `configure` from merged configure.ac; never trust the merged binary artifact of autoconf. | The port doesn't configure. Loud. |
| 15 | specs (`kernel32`, `kernelbase`) | clean | LOW | Ours: marshalling-accurate signatures (RtlUnwindEx, VerSetConditionMask). Theirs: ReadConsoleInputEx stub→real. Different lines; compose. Spec changes flow into spec2thunk regeneration at build; refused new exports bind to named sentinels (loud on call, by design). | Silent guest mis-marshal of RtlUnwindEx — an SEH-adjacent ejection that would NOT fail the build. On the section-6 list. |
| 16 | remaining small files (`kernelbase/{memory,thread}.c`, `winbase.h`, `winnt.h`, `include/Makefile.in`, `opengl32/wgl.c`, `secur32/Makefile.in`, `setupapi/devinst.c`, `winegcc.c`) | clean | MINIMAL | Vs 11.16 upstream touches NONE of these (all its deltas are post-tag) → our side passes through unmodified; nothing to resolve. Vs master: disjoint (setupapi 556-line rework vs our +2 — re-verify the `.NTppc64` line lands in the reworked file; winegcc ARM64EC case is take-theirs in a function we don't touch). | Various single capabilities (fibers, guest GL entry, INF platform matching, winegcc PE defines). All greppable; all on the section-6 list. |

---

## 4. Recommended target: **wine-11.16, not mirror/master**

1. **Same conflict cost, less exposure** (MEASURED): the conflicted-file set
   is identical for both targets, but 11.16 touches only **13 of the 26**
   overlap files; the other 13 (including `vulkan.c`, `unix_private.h`,
   `system.c`, `server.c`, `winegcc.c`, `configure.ac`) have zero upstream
   delta at the tag — for those, silent ejection is *impossible* at 11.16
   because there is nothing to merge.  The `get_unused_client_surface`
   compile break does not exist at 11.16.
2. **The post-tag 156 are mid-flight reworks**: the win32u GL client-surface
   series is landing in pieces (its API already changed once); lsass/SamSs
   adds a new required service process and prefix schema changes — and
   project memory records prefix state blocking titles before.
3. **A release tag is a tested, nameable cut**; master's head was 3 days old
   at analysis time.
4. What master has that the fork plausibly wants now — the 64K-page 4GB-wrap
   fix (`67a26bddad0`) and the hwloc NUMA fix (`b3239efd8b6`) — are isolated
   cherry-picks on top of the 11.16 merge (per policy: they are fixes — take
   them), pending the fix-hunting agent's confirmation.
5. Cadence: 11.16 now, 11.17 at its tag.  Tag-to-tag merges keep the
   conflict surface this small.

---

## 5. Merge plan (ordered)

**Preconditions**
0. Coordinate with the agent currently committing to the tree; do the merge
   on a separate worktree/branch on the AC922
   (`git worktree add ../wine-merge-11.16 wine-ppc64le`), never the live
   tree.  Re-pin HEAD (it has moved past `f15604cf0ea`), re-run
   `git merge-tree --write-tree <new-head> wine-11.16` to confirm the
   conflict set is still exactly {protocol.def, server_protocol.h}, and
   re-run the section-2 inventory extraction for any overlap file the fork
   touched since the pin.  Create `git branch backup/pre-11.16-merge <head>`
   — the no-ejection check diffs against this ref.
   **Leave `stash@{0}` (the winex11 mouse fix) alone** — do not pop it
   before or during the merge (see the sequencing note in section 2).

**The merge**
1. `git merge --no-commit wine-11.16` in the merge worktree.
2. Hand-resolve `server/protocol.def` — keep both sides per table row 1.
   ~5 minutes; re-read the four ppc64 arms against the inventory before
   staging.
3. **Regenerate, don't merge, the generated files**:
   * `tools/make_requests` → `server_protocol.h`, `request_handlers.h`,
     `trace.c` generated section (auto-bumps SERVER_PROTOCOL_VERSION,
     962 → 963).  Diff the output: ppc64 arms present in the generated
     context struct AND in trace.c's hand-written dump helpers.
   * `autoconf` (version-matched; `configure~` in the tree is the reference)
     → `configure` from merged configure.ac + VERSION (now 11.16).
4. Read the merged versions of the five both-sides-edited functions:
   `allocate_virtual_memory` + `map_view` (virtual.c),
   `get_server_context_flags`/`context_to_server`/`context_from_server`
   (unix/thread.c), `server_init_thread` (unix/server.c), `copy_context`
   (server/thread.c).  Textual cleanliness is measured; coherence is not.
5. **Run the no-ejection check (section 6) BEFORE the first build** — it is
   pure grep, takes seconds, and a failure at this point is a cheap re-merge
   instead of a debugging session.
6. Build.  Expected fallout at 11.16: near zero (INFERRED — every known
   API-shape change in the overlap is post-tag).
7. Commit the merge.  Then, as separate commits on top, cherry-pick the two
   flagged upstream fixes if confirmed wanted (`67a26bddad0`,
   `b3239efd8b6`).
8. **Rewrite the stashed mouse fix** against the post-merge
   `dlls/winex11.drv/mouse.c` (which now contains `9306b8e8de4`'s fractional
   raw-delta accumulation in the same functions).  Use the stash as the
   statement of intent, not as a patch to apply; drop the stash once the
   rewritten fix is committed and its gate/probe passes.

**Deferred (the eventual master/11.17 merge) — known worklist**
* `dlls/win32u/vulkan.c` fork call site: add `get_unused_client_surface`'s
  third argument.
* lsass/SamSs: prefix update (`wineboot -u`) + verify guest secur32 thunks
  still authenticate through lsass.exe (Steam login is the live consumer).
  Its own gated task.
* setupapi rework: re-verify `.NTppc64` placement; `is_ec_code` removal and
  vkd3d 2.1: expected clean.

**Prefix**
9. After a green build + gates, run `wineboot -u` against a **disposable
   clone** of a working prefix first (memory: prefix state has blocked
   titles).  11.16 brings Mono 11.3.  Promote to real prefixes only after
   the gates pass on the clone.

---

## 6. Verification plan

### 6a. The no-ejection proof (mechanical, run before first build and again before push)

A silent ejection passes a build; it must not pass this.  Two layers:

**Layer 1 — every fork-added line still present.**  The ejection surface is
exactly the files upstream also touched (elsewhere the merge cannot alter our
side).  For each of the 26, take our added lines from the pre-merge backup
and require each to appear verbatim in the merged file; generated files are
excluded (their proof is layer 2 + regeneration diff review):

```sh
cd <merge-worktree>
BASE=e99fc2f7587 OURS=backup/pre-11.16-merge
for f in server/protocol.def server/thread.c server/trace.c \
         dlls/ntdll/unix/virtual.c dlls/ntdll/unix/thread.c \
         dlls/ntdll/unix/server.c dlls/ntdll/unix/system.c \
         dlls/ntdll/unix/unix_private.h dlls/ntdll/unwind.h \
         dlls/ntdll/tests/exception.c dlls/win32u/vulkan.c \
         dlls/opengl32/wgl.c dlls/kernelbase/thread.c \
         dlls/kernelbase/memory.c dlls/kernel32/kernel32.spec \
         dlls/kernelbase/kernelbase.spec include/winnt.h include/winbase.h \
         include/Makefile.in dlls/secur32/Makefile.in \
         dlls/setupapi/devinst.c configure.ac tools/winegcc/winegcc.c; do
  git diff "$BASE".."$OURS" -- "$f" | grep '^+[^+]' | cut -c2- | \
  while IFS= read -r line; do
    # skip blank/whitespace-only lines
    [ -n "$(printf %s "$line" | tr -d '[:space:]')" ] || continue
    grep -qxF -- "$line" "$f" || printf 'EJECTED %s: %s\n' "$f" "$line"
  done
done
```
Zero `EJECTED` lines is the bar.  (Lines legitimately *rewritten* during
resolution — e.g. the vulkan.c 3-arg fix on a future master merge — will
show up; each hit must be explained by a deliberate resolution note, never
waved through.)

**Layer 2 — capability symbols, greppable in seconds** (the load-bearing
subset; also useful as a standing pre-push check):

| Symbol / pattern | File |
|---|---|
| `powerpc64_regs` (4 arms) | `server/protocol.def` AND regenerated `include/wine/server_protocol.h` |
| `powerpc64_regs` dump arms | `server/trace.c` |
| `affinity_names_a_processor` | `server/thread.c` |
| `virtual_enable_hwtso`, `mprotect_hwtso`, `VPROT_NOSAO`, `thread_data_cache` | `dlls/ntdll/unix/virtual.c` |
| `powerpc64_regs` transfer loops | `dlls/ntdll/unix/thread.c` |
| `server_monotonic_time` in `server_wait` | `dlls/ntdll/unix/server.c` |
| `ntdll_cpu_topology`, `init_shared_data_cpuinfo` | `dlls/ntdll/unix/system.c` |
| `RtlVirtualUnwind2_amd64` | `dlls/ntdll/unwind.h` |
| `create_host_swapchain`, `present_mode_fallbacks`, `__wine_get_hwnd_surface_funcs` | `dlls/win32u/vulkan.c` |
| `__wine_gl_entry_point` | `dlls/opengl32/wgl.c` |
| `switch_fiber` | `dlls/kernelbase/thread.c` |
| `PROCESSOR_ARCHITECTURE_PPC64` | `include/winnt.h` + `dlls/kernelbase/memory.c` |
| `RtlUnwindEx(ptr ptr ptr ptr ptr ptr)` | `dlls/kernel32/kernel32.spec` |
| `VerSetConditionMask(int64 long long)` | `dlls/kernel32/kernel32.spec` |
| `.NTppc64` | `dlls/setupapi/devinst.c` |
| `CPU_POWERPC64` (2 sites) | `tools/winegcc/winegcc.c` |
| `secur32.thunks` | `dlls/secur32/Makefile.in` |
| `emu_qpc.h`, `winecom.h`, `cputopology.h` | `include/Makefile.in` |

**Layer 3 — built artifacts** (after the build): `nm`/`objdump -T` for the
externally-visible subset — `virtual_enable_hwtso` in ntdll's unixlib,
`__wine_get_hwnd_surface_funcs` in win32u, `switch_fiber` in kernelbase,
`affinity_names_a_processor` in `wineserver` — proving the symbols not only
merged but compiled in.

### 6b. Runtime ladder (cheapest and safest first)

A bad merge here does **not** fail the build — guest thread, context and
exception machinery breaks at runtime.  The ladder:

1. **Build green** — catches signature/regeneration errors only.
2. **`probes/*.sh`** (sibling dir
   `~/Development/powerpc64le-ports/hangover-ppc64le/probes/`) — safe
   unattended, run all; FP-marshal and any context/boundary probes first.
3. **Targeted gates** (each raises real modal dialogs — own Xvfb, per
   NEXT.md):
   * `ppc64le/winedbg/check-guest-debug.sh` — get/set guest context through
     the **server**: the single best probe of the merged
     `context_data`/`copy_context`/`context_to_server` stack, and the one
     that catches a context-union mis-merge.
   * `ppc64le/wow64/check-wow64-smoke.sh` — 32-bit lane end to end.
   * `ppc64le/seh` gates — the unwind.h merge and guest SEH.
   * `ppc64le/cpu/check-cpu-topology.sh` — system.c + server/thread.c
     affinity work.
   * `ppc64le/opengl/check-gl-smoke.sh` (both driver legs) and
     `ppc64le/dxvk/check-present-smoke.sh` + `check-fullscreen-smoke.sh` —
     the winex11 present changes (`3394b734479`, `94e27fa07b1`) land exactly
     under these; child-window and exclusive-fullscreen legs are the ones
     that would move.
   * `ppc64le/syscom/check-com-smoke.sh`, `ppc64le/steamapi` +
     `ppc64le/steamtool` gates — boundary canaries.
4. **Full sabotage sweep**: `for g in ppc64le/*/check-*.sh; do "$g" --sabotage; done`
   on its own Xvfb, ~30 minutes, 33 gates, every negative control red.  The
   established pre-push bar.
5. **A real game A/B**: the CP2077 `-benchmark` flythrough (the project's
   A/B harness) pre- vs post-merge — covers input (the raw-mouse change),
   present, focus (server thread-input changes) and sustained guest
   thread/exception traffic no gate reaches.  Short DOOM (2016) and
   Witcher 3 launches per `ppc64le/games/STATUS.md` complete it.

### What could still go silently wrong (build-green, checks-maybe-green)

* A protocol.def mis-merge that regenerates *consistently* stays internally
  coherent (client and server share the header) — the winedbg gate is the
  designed runtime catch; layer-2's `powerpc64_regs` greps are the static
  one.
* Upstream's thread-input refcount/focus changes and fractional raw mouse
  alter game-facing behavior no gate measures — only the game A/B does.
* `map_view`'s new user-limit rejection: any fork path mapping at fixed
  addresses above the guest limit now gets a different status — grep ppc64le
  code for fixed-address NtMapViewOfSection/NtAllocateVirtualMemory callers.
* The exception-flags bug class fixed in unwind.h may exist in the fork's
  own guest converters (`signal_ppc64.c`) — audit item, invisible to every
  gate until a game unwinds through the boundary.
* New upstream guest-run tests (address limits) may fail under emulation and
  masquerade as merge damage — triage against a pre-merge run.

---

## 7. What I could not determine without actually merging

* Semantic coherence of the five both-sides-edited functions (step 4) —
  textual cleanliness is measured, coherence is not.
* Build fallout **outside** the overlap (files including changed headers;
  vs master, callers of reworked win32u internals).
* Runtime effect of the winex11 present and server input changes on the
  three playing titles.
* Whether the hwloc NUMA one-liner's path is executed by the fork's
  rewritten topology code (function survives at fork system.c:1881; call
  graph untraced).
* The exact `make_requests` output on the merged protocol.def (deterministic
  but not run — it writes files).
* Autoconf version compatibility for regenerating `configure`.
* The post-merge shape of `dlls/winex11.drv/mouse.c` that the stashed fix
  must be rewritten against (depends on how `9306b8e8de4` composes with the
  stash's intent — assessable only after the merge lands).

## Appendix: measurement provenance

* Conflict set: `git merge-tree --write-tree --no-messages f15604cf0ea {mirror/master, wine-11.16}` → result trees `bb979ac19e8` / `f185b818693`; conflict entries only for `server/protocol.def` and `include/wine/server_protocol.h`, identical both targets.
* Tag membership: `git merge-base --is-ancestor <sha> wine-11.16` per named commit (incl. `9306b8e8de4` → IN-11.16).
* Fork-owned inventory: `git diff e99fc2f7587..f15604cf0ea -- <file>` per file, added-definition extraction.
* Fork vulkan.c call sites: lines 1537 (base site upstream patches) and 3346 (fork-added, 2-arg — the master-only compile break).
* Protocol versions 959/961/962 from each ref's `server_protocol.h`; auto-bump logic at `tools/make_requests:320-427`.
* winex11 mouse: fork committed delta vs base = empty; upstream 11.16 delta = mouse.c +10/−2, x11drv.h +2.
