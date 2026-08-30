# wine-11.16 merge into wine-ppc64le — report

Merge commit: `3214a170a7e` on `wine-ppc64le` (parents `53b5c996330` fork HEAD,
`8da89f8493b` wine-11.16 tag). Backup ref `backup/pre-11.16-merge` points at
the pre-merge HEAD. Not pushed.

Note on scope drift: the plan was written against fork HEAD `f15604cf0ea`;
by the time this ran the fork had moved 25 more commits to `53b5c996330`
(pre-existing commit count already included one cherry-pick the plan
anticipated, `d8ca4eea468`). Re-ran `merge-tree` against current HEAD before
touching anything, per the plan's own precondition step.

## 1. What conflicted and how it was resolved

`git merge-tree` against current HEAD found **three** conflicted files, not
the plan's two — the extra one is a direct consequence of the tree having
moved since the plan was measured:

- **`server/protocol.def`** — exactly as the plan described. Adjacent-line
  conflict in `struct context_data`'s `integer` union. Kept both: upstream's
  `arm64_regs` split (`x[31]` → `x0[18], x19[12]`) plus the fork's four
  `powerpc64_regs` arms (ctl/integer/fp/debug), which the plan's table
  correctly predicted compose cleanly. Verified the full merged struct by
  hand — all four ppc64 arms present, `tls` union / `SERVER_CTX_TLS 0x0100`
  present, nothing dropped.

- **`include/wine/server_protocol.h`** — generated, not hand-merged (see §2).

- **`dlls/winex11.drv/init.c`** (not in the plan's conflict set) — upstream's
  `94e27fa07b1` had already been cherry-picked onto the fork as `d8ca4eea468`
  before this merge started, exactly as the task brief flagged. Upstream's
  own history has that same patch immediately followed by `3394b734479`
  ("Allow child D3D presentation on fullscreen exclusive toplevel"), which
  touches the identical lines in `X11DRV_client_surface_present`. Git saw:
  base → ours (= upstream's first patch alone) → theirs (= upstream's first
  patch + the second one), and flagged it as a conflict rather than
  recognizing theirs as a superset. Resolved by taking upstream's side
  whole. Checked before resolving: **zero** ppc64/powerpc references
  anywhere in this file, either side — it's 100% upstream-authored driver
  code, so there was no fork content at risk and no ejection judgment call
  needed. Post-resolution `git diff wine-11.16 -- dlls/winex11.drv/init.c`
  is empty, confirming a clean, complete take. The cherry-pick did not
  double-apply or get reverted.

## 2. What was regenerated, and with what

- `include/wine/server_protocol.h`, `server/request_handlers.h`,
  `server/request_trace.h`: `perl tools/make_requests`, run from the repo
  root after `protocol.def` was hand-resolved. `SERVER_PROTOCOL_VERSION`
  auto-bumped 962 → 963 as the plan predicted, dissolving the version-line
  collision.
- `configure`: `autoconf` (2.73, matching the tree's own generated-by
  banner) from the merged `configure.ac`. Output was **byte-identical** to
  the version git's own 3-way merge had already produced for `configure` —
  a nice independent confirmation that the textual auto-merge of that file
  was sound, not just lucky.
- `server/trace.c` was **not** regenerated — it auto-merged cleanly (its
  hand-written ppc64 dump arms sit above the generated-section markers,
  disjoint from upstream's edits) and `tools/make_requests` doesn't touch
  it at all; it only writes the three files above.

### Two mid-merge fixes `make_requests` could not catch on its own

Both surfaced as real build failures, not as merge conflicts, and both are
fixed now:

1. **`tools/make_requests:46`** hardcodes `struct context_data`'s expected
   size for its own `C_ASSERT` sanity check: `[2040, 8]`. This is a
   maintenance constant, *not* computed from `protocol.def` — the script's
   request/reply-body parser only handles fields inside `@REQ`/`@REPLY`
   bodies via a lookup table; whole-struct sizes like this one are
   hand-entered. At the merge base this was `1720`; the fork bumped it to
   `2040` when it added the four ppc64 arms; upstream never touched it
   because their arm64 split (−8 bytes) and `tls` addition (+8 bytes)
   cancel out, leaving their own struct at the same 1720 both before and
   after. Git's 3-way merge on this single line trivially took "ours"
   (base==theirs, ours differs) — completely correct git behavior, but the
   constant it kept doesn't know about upstream's structural change,
   because ppc64's `powerpc64_regs` integer arm (272 bytes) already
   dominated the `integer` union's size before *and* after the arm64 split,
   so the two sides' independent deltas don't simply add. I compiled the
   actual merged `struct context_data` standalone (extracted it from
   `server_protocol.h`, compiled with a throwaway `main()`) to get the
   ground truth: **2048 bytes**, not 2040 and not the naively-summed 2040.
   Fixed the constant to `2048`, reran `make_requests`. The resulting
   `C_ASSERT` in `request_handlers.h` now reads `2048` and matches the real
   compiled layout — checked twice: once against the standalone extraction,
   once by the compiler itself accepting the assert during the full build.

2. **`dlls/kernel32/kernel32.thunks`** pins `WriteConsoleW`'s declaration at
   `consoleapi.h:87` as a header-drift guard (this is fork machinery: every
   `.thunks` manifest entry cites `header.h:line`, and `spec2thunk` asks
   clang where the declaration actually is and fails the build if they
   disagree — a real safety feature, working as designed here). Upstream's
   `ReadConsoleInputEx` addition inserts 3 lines above `WriteConsoleW` in
   `consoleapi.h`, moving its real declaration from line 87 to line 90.
   Rather than fix this one citation and hope, I diffed exactly which
   headers upstream's 109 commits touch at all (`consoleapi.h`,
   `d3dx10math.h`, `memoryapi.h`, `wine/condrv.h`, `wine/gdi_driver.h`,
   `wine/opengl_driver.h`, `wine/wined3d.h` — computed directly, not from
   the plan) and grepped all 80+ `.thunks` files for citations into any of
   them. `WriteConsoleW` was the **only** hit. Repinned to `:90`.

Neither of these is a hand-merge of generated content — both are
maintenance constants/citations that the merge correctly left alone
(no conflict) but that had gone stale as a side effect of composing both
sides' real changes. Worth flagging for whoever does the 11.17 merge: the
`.thunks` header-citation sweep and the `make_requests` size-table check
are cheap (a few minutes) and should be standard steps, not things this
merge happened to catch by accident.

## 3. No-ejection check results

Run before the first successful build, per the plan (the first two build
attempts failed on the two issues in §2 before any ejection check would
have mattered — no ejection was involved in either failure, both were
stale-constant issues in fork-owned generator/manifest files, confirmed
by reading the actual mechanism, not inferred).

- **Layer 1 (line-replay)**: every added line from
  `git diff e99fc2f7587..backup/pre-11.16-merge` for each of the plan's 22
  non-generated fork-owned files, checked verbatim against the merged file.
  **0 EJECTED hits**, both before and after the two mid-merge fixes (fixes
  touched `tools/make_requests` and `kernel32.thunks`, neither of which is
  in this 22-file list, so the check was re-run afterward purely as a
  sanity re-confirmation, not because those files could regress it).
- **Layer 2 (capability-symbol grep)**: all 27 symbols/patterns from the
  plan's table present: `powerpc64_regs` ×4 in `protocol.def`, ×4 in
  `server_protocol.h`, dump arms in `trace.c`,
  `affinity_names_a_processor`, `virtual_enable_hwtso`, `mprotect_hwtso`,
  `VPROT_NOSAO`, `thread_data_cache`, the ppc64 transfer loops in
  `unix/thread.c`, `server_monotonic_time` in `server_wait`,
  `ntdll_cpu_topology`, `init_shared_data_cpuinfo`,
  `RtlVirtualUnwind2_amd64`, `create_host_swapchain`,
  `present_mode_fallbacks`, `__wine_get_hwnd_surface_funcs`,
  `__wine_gl_entry_point`, `switch_fiber`,
  `PROCESSOR_ARCHITECTURE_PPC64` (×2 files), the `RtlUnwindEx`/
  `VerSetConditionMask` spec signatures, `.NTppc64`, `CPU_POWERPC64` ×2
  sites in winegcc.c, `secur32.thunks`, and
  `emu_qpc.h`/`winecom.h`/`cputopology.h` in `include/Makefile.in`.
  **27/27 OK, 0 MISS.**
- **Layer 3 (built artifacts, `nm`)**: `affinity_names_a_processor` present
  (T, defined) in `server/wineserver`; `virtual_enable_hwtso` and
  `mprotect_hwtso` present (t, local) in `dlls/ntdll/ntdll.so`;
  `__wine_get_hwnd_surface_funcs` present in `dlls/win32u/win32u.so`;
  `switch_fiber` present (t) in `dlls/kernelbase/ppc64-windows/kernelbase.dll.so`
  (this is the fork's native ppc64-windows-ABI target, which is where the
  fork's inventory said the assembly lives — not the guest x86_64
  `kernelbase.dll`).

**The no-ejection check passed cleanly at all three layers, with the
numbers above.**

## 4. Build and probe results

- `make -j144` in an isolated, freshly-configured build directory
  (`wine-merge-11.16-build`, `configure --enable-archs=ppc64,i386
  --without-opencl --without-mingw`) reached **"Wine build complete."**
  with zero `Error N` / `make: ***` lines in the final log, after three
  attempts:
  1. First attempt failed on `ppc64le/vkd3d/src/libs/d3d12/d3d12.def`
     missing — a fresh `git worktree add` doesn't carry gitignored
     upstream-checkout directories. `ppc64le/dxvk/src` auto-bootstrapped
     itself via a Makefile rule that clones/patches it on demand (network
     access, succeeded); `ppc64le/vkd3d/src` has no equivalent rule. Fixed
     by symlinking `ppc64le/vkd3d/src` from the main tree's already-populated
     checkout (swept the rest of `ppc64le/` for similar gaps first — vkd3d
     and dxvk are the only two gitignored-and-build-needed directories under
     `ppc64le/`, confirmed by checking every top-level subdirectory's
     tracked-file count against its disk size). This symlink is **not
     committed** — it's a worktree-local build convenience for a directory
     git doesn't track; a future worktree either needs the same symlink or
     to run the dxvk-style bootstrap once for vkd3d.
  2. Second attempt failed on the `context_data` size assertion (§2, item 1).
  3. Third attempt failed on the `WriteConsoleW` thunks citation (§2, item 2).
  4. Fourth attempt: clean. `loader/wine`, `server/wineserver`,
     `dlls/ntdll/ntdll.dll.so`/`ntdll.so`, `dlls/guestcrt/x86_64-windows/guestcrt.dll`
     all present.
  A prebuilt `dlls/ntdll/libfexbridge.so` also had to be copied in from the
  tested `wine-build` directory (a normal `make` doesn't produce it; it's
  installed by a separate `fexbridge/build-fexbridge.sh` step, and
  `loader.c`'s bridge-loading code has zero upstream overlap, so a plain
  copy from the already-tested build is exactly as valid as rebuilding it).
  This is also not something that gets committed — it's a build-output
  artifact.

- Probes, run from the sibling `probes/` directory with `WINE_BUILD`
  pointed at the isolated build, and the required `FEX_*` env vars set:
  - **`check-setjmp.sh`: PASS** — all 32 checks (GPR/XMM6-15 preservation
    through longjmp, `jmp_buf` layout, no buffer overrun).
  - **`check-cxx-throw.sh`: PASS** — 15/15 transcript match,
    `__C_specific_handler` import shape correct, negative control died with
    the expected unhandled-exception signature.
  - **`check-fp-marshal.sh`: PASS** — all 11 floating-point marshalling
    checks.
  - Bonus, not required: **`check-thread-context.sh`** — attempted since it
    exercises the merged `context_data` union most directly, but its build
    harness doesn't honor `WINE_BUILD` the way the other three probes do
    (it linked against `libwinecrt0.a` from the isolated build dir but
    resolved header/library paths that pointed partly back at the *original*
    `wine-build`/`wine-upstream` pairing, producing undefined-reference
    link errors). This is a probe-harness/path-plumbing gap, not a merge
    regression — confirmed by the fact that the failure is a link-time
    "undefined reference to GetLastError/CloseHandle/etc." pattern
    consistent with a stale/mismatched library search path, not a runtime
    or context-content failure. Left unrun rather than spending further
    budget adapting a probe that wasn't part of the required set; flagging
    it as worth fixing before it's relied on for the next merge.

## 5. Reading of the five shared functions — coherent, not just clean

All five read as semantically coherent:

- **`allocate_virtual_memory`** (`dlls/ntdll/unix/virtual.c`): upstream's
  ~50 lines of new type/attribute validation (`MEM_PHYSICAL`/
  `MEM_LARGE_PAGES`, `MemExtendedParameterAttributeFlags`, arm64ec range
  commit) sit well clear of the fork's one addition, `emu_invalidate_code_range(
  base, size )` after a successful allocation — present, in the right
  place (after `*size_ptr = size;`, before the `STATUS_NO_MEMORY` branch),
  matching its pre-merge position exactly.
- **`map_view`**: upstream's `4ac0555e55c` swap from
  `STATUS_WORKING_SET_LIMIT_RANGE` to `STATUS_INVALID_PARAMETER` for
  beyond-`user_space_limit` fixed-address maps (line ~2333) landed cleanly;
  the fork has zero lines in this function, so there was nothing to
  compose. Flagging per the plan: any ppc64le-side code doing fixed-address
  `NtMapViewOfSection`/`NtAllocateVirtualMemory` now gets a different
  status in that one case — worth a grep before the next release if the
  port has any such caller (I did not find one touching this path while
  reading the merge; not exhaustively audited).
- **`get_server_context_flags` / `context_to_server` / `context_from_server`**
  (`dlls/ntdll/unix/thread.c`): read all three in full. The
  `MAKELONG(IMAGE_FILE_MACHINE_POWERPC64, IMAGE_FILE_MACHINE_POWERPC64)`
  case arms are completely disjoint from the `ARM64→ARM64` arms upstream
  touched (which gained `SERVER_CTX_TLS`/`CONTEXT_ARM64_X18` handling) —
  adjacent in the switch, never overlapping. The gpr1/stack-pointer
  dual-write comment and the "don't clobber the control block's fresher
  copy" logic in `context_from_server`'s integer loop are both intact and
  make sense read cold.
- **`server_init_thread`** (`dlls/ntdll/unix/server.c`): `thread_data_cache
  = data;` present exactly where expected, right after
  `pthread_setspecific`. Upstream's own change to this function
  (`fd3fbe3ef37`, adding an `else if (data->start)` branch for macOS main-
  thread spawn) is **not** in this merge — it's post-tag/master-only, so
  its absence here is correct, not a miss.
- **`copy_context`** (`server/thread.c`): purely mechanical
  whole-member-copy function; `if (flags & SERVER_CTX_TLS) to->tls =
  from->tls;` slotted in as one more line in the same pattern as every
  other flag. Nothing to misread here.

## 6. Upstream changes that affect fork behavior — watch list

- **`67a26bddad0`** (4GB-wrap-around fix, subtracts `host_page_size` instead
  of 1 byte) and **`b3239efd8b6`** (hwloc NUMA swapped-parameter fix) are
  both **post-tag, master-only** — confirmed not present in this merge
  (`git merge-base --is-ancestor` against `wine-11.16` was already checked
  by the plan; I did not re-verify independently but the merge only pulled
  the tag's 109 commits by construction). Per the plan's §5 step 7 these
  were flagged as candidate follow-up cherry-picks, not required parts of
  this merge. **I did not cherry-pick either one** — see §7, this is a
  place I read the plan's intent narrowly rather than acting on it, since
  the task instructions asked me to merge `wine-11.16` and report on watch
  items, not to also pull two extra master-only commits. Both remain
  genuinely worth doing as isolated cherry-picks before or alongside the
  11.17 merge; the 4GB-wrap one in particular matters on this POWER9 host's
  64K pages.
- **Fractional raw mouse motion (`9306b8e8de4`)** is now in the tree (it's
  in-tag). It changes `map_raw_event_coords()`/`update_relative_valuators()`
  in `dlls/winex11.drv/mouse.c` — the exact functions the stashed
  `stash@{0}` fix targets. Per the plan and the task brief, I left the
  stash untouched and did not pop it or write the rewritten fix — that's
  explicitly a separate, subsequent task ("merge first, then rewrite the
  mouse fix against the post-merge shape"). Confirmed post-merge:
  `dlls/winex11.drv/mouse.c` now contains the fractional-accumulation
  code; the stash is still `stash@{0}`, unpopped, unmodified.
- Upstream's `map_view` status-code change (§5) is a small behavioral
  change worth a grep before relying on any fixed-address mapping path.
- New upstream address-limit tests landed in `dlls/ntdll/tests/{exception,virtual}.c`;
  if any fail when run as x86-64 guests under this port's emulation, triage
  against a pre-merge run before treating it as merge damage — flagged by
  the plan, not something I ran (test execution wasn't in the required
  verification list).

## 7. Uncertain / disagreements / follow-ups

- **The `dlls/winex11.drv/init.c` conflict** was not in the plan (measured
  against an older HEAD). I resolved it by policy (take upstream whole,
  after confirming zero ppc64 content) rather than by a pre-written
  instruction — flagging per the task's "where you disagree, say so"
  guidance, though I don't think this is actually a disagreement so much
  as the plan being stale on this one point; my resolution follows the
  same logic the plan applies everywhere else.
- **Did not cherry-pick `67a26bddad0` or `b3239efd8b6`** (§6). The task's
  numbered instructions describe them as things to watch and report on,
  not to do; the plan's own step 7 puts them after "commit the merge" as a
  conditional follow-up ("if confirmed wanted"). I read this as out of
  scope for a wine-11.16 merge task and left them for a deliberate,
  separate decision — but this is a place where a different reading of
  the brief would have had me pull both in as part of "finishing the job."
- **`check-thread-context.sh` did not run successfully** (§4). Not one of
  the three required probes, and the failure looks environmental
  (WINE_BUILD not honored by that script's build step) rather than a merge
  regression, but I have not proven that with the same rigor as the three
  required probes — flagging rather than asserting it's fine.
- **Did not rebuild the tested `wine-build` directory** or run `wineboot -u`
  against a prefix clone. The task's VERIFY section asked for a full build
  and the three named probes, both done in an isolated directory precisely
  so the owner's tested tree/build stayed untouched "until done." Now that
  the merge is fast-forwarded into the real `wine-ppc64le` branch, the
  natural next step is rebuilding `wine-build` itself and running the
  fuller gate ladder (plan §6b) — I left that for the owner or a follow-up
  task rather than doing it unasked, since it touches the live, tested
  build the instructions were explicit about protecting.
- **Did not audit `signal_ppc64.c`'s guest-context-flag converters** against
  the `8c284041138` exception-flag-preservation pattern the plan flagged as
  an audit item — out of scope for this merge's verification list, but
  worth stating plainly rather than letting it look implicitly checked.
- The isolated build directory `wine-merge-11.16-build` was left on disk
  (376M+189M of vkd3d/dxvk sources aside, disk has 1.1T free) in case it's
  useful for follow-up debugging; delete it whenever convenient.
