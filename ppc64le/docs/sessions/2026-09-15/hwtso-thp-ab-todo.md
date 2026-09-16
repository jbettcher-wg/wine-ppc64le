# HWTSO A/B and THP on the 64K box -- to-do list (2026-09-15)

Box: op64k, kernel 7.2.5-books-64k, 64K pages FOR GOOD (4K is retired), HASH MMU (PROT_SAO available,
CONFIG_PPC_PROT_SAO_LPAR=y), THP `madvise` mode, governor already
`performance`, two NUMA nodes (0, 8).  Tree there is at 70ee2ccb024 with an
UNCOMMITTED edit to ppc64le/steamtool/proton -- read it before any run, it
may already change the lane defaults.

Facts that shape the A/B:
- FEX_HWTSO=1 is the NATIVE-LANE DEFAULT since 2026-08-26 (steamtool/proton
  lane defaults).  So the arms are: A = default (HWTSO=1, SAO pages, no JIT
  barriers), B = FEX_HWTSO=0 (software barriers, ~3.5 ns per guest store).
  There is no "turn it on" step; the unmeasured thing is the fps delta.
- The bridge prints "FEX_HWTSO live: PROT_SAO carries ordering" when it is
  really on; kernel refusals come back through emu_hwtso_refused and the
  process falls back to barriers.  A leg without that log line is NOT arm A.
- The tell in smaps is the `ar` VmFlag on wine-managed VMAs.  On 64K pages
  every SAO region is 64K-granular; VPROT_NOSAO image sections keep their
  own protection.
- THP today: wine only ever calls madvise(MADV_NOHUGEPAGE) (write-watch
  views).  In `madvise` mode nothing else gets huge pages, so today's THP
  count for the game is whatever glibc/FEX ask for (AnonHugePages was 160 MB
  box-wide at idle).  THP for game memory is a wine-side change (part 2).

## Part 1 -- HWTSO A/B (Cyberpunk -benchmark, then Witcher 3)

1. [x] `ssh op64k`, `cd ~/Projects/power8/wine-ppc64le`, `git diff
       ppc64le/steamtool/proton` -- decide: commit it, stash it, or run
       with it (and say which in the results tag).
2. [x] Point ppc64le/cpu/bench-cp2077.sh at THIS box: TOOL, EXE, RESULTS,
       WINE_PPC64LE_TREE are the co-dev's Development/... paths (lines
       52-56).  Prefix is ~/.local/share/wine-ppc64le/nw-cp2077.  Make them
       env-overridable with defaults for op64k rather than editing in place.
3. [x] Preconditions per leg (the script checks GameThread; you check the
       rest): governor `performance` (it is), SMT mode noted in the tag
       (results file records `smt=`), no other game, compositor on :1 up,
       MANGOHUD off for the benchmark legs (its overlay costs frames).
4. [x] Arm A, 2 legs:  `bench-cp2077.sh 2 hwtso-on`.  Confirm in each
       leg's launcher log: `lane default FEX_HWTSO=1` AND `FEX_HWTSO live`.
5. [x] Arm B, 2 legs:  `FEX_HWTSO=0 bench-cp2077.sh 2 hwtso-off`.  Confirm
       the log shows NO `FEX_HWTSO live` line (a caller's value beats the
       lane default; `stripped 0 variable(s)` proves the env reached it).
6. [x] Order A A B B (user's call, 2026-09-15): two legs per arm is enough here;
       the governor is pinned so drift is small.  First leg after a reboot
       is a warm-up, discard it.
7. [x] Read: compare FLOOR (min frametime) first, average second; the
       tree's own rule is that +-0.3 fps avg is noise and scene spread is
       +-10%.  Also grep the amdgpu ring-timeout count per leg (dmesg) --
       HWTSO changes visibility of stores to the GPU-facing threads, a
       hang delta is a finding too.
8. [ ] Optional third arm for the fence-cost picture: FEX_TSOENABLED=0
       (unsound, control only, never ship) -- it bounds what HWTSO can win.
       If A ~= this arm, SAO is already delivering everything.
9. [x] Witcher 3 pass (D3D11, 81% JIT frame thread -- the title most likely
       to show a store-fence win): pinned save (leg5-save exists on the
       box), MangoHud CSV, 200 s window, same interleave.  Back up the
       autosave before each leg.
10. [x] Write the numbers into op4k-crossing-numbers / NEXT.md and, if arm A
       wins, nothing to ship (it is the default); if arm B wins or ties,
       that is a fastppcx86 finding -- hand it over; this tree is wine-side only.

## Part 2 -- THP for guest memory (wine-side, after Part 1)

11. [x] Baseline count: during a CP2077 leg,
       `grep AnonHugePages /proc/<game pid>/smaps_rollup` and
       `grep thp_ /proc/vmstat` before/after.  Expect near zero from wine.
12. [closed] Add a lever in dlls/ntdll/unix/virtual.c: `WINE_PPC64LE_THP=1`
       makes map_view / allocate_virtual_memory call madvise(MADV_HUGEPAGE)
       on anonymous committed views >= 2 MB (16 MB on hash-64K is the
       PMD size -- check `Hugepagesize` in /proc/meminfo; on hash 64K THP
       is 16 MB).  Skip write-watch views (they already opt out) and image
       views.  Log once per process how many bytes were advised.
13. [x] Check SAO x THP interaction on a probe before a game: mmap 64 MB,
       madvise HUGEPAGE, touch, mprotect(PROT_SAO|RW), then read smaps:
       want `ar` AND a non-zero AnonHugePages on the same VMA.  If the
       mprotect splits the huge pages, THP and HWTSO are exclusive and the
       lever must pick.
14. [x] Also worth a try, no code: `echo always >
       /sys/kernel/mm/transparent_hugepage/enabled` for one A/B pair --
       covers glibc heap, the emulator's code cache and wine at once.  Watch
       `thp_fault_fallback` and RSS; defrag stays `madvise`.
15. [x] A/B the same way as Part 1 (2+2, A A B B) with THP on/off, HWTSO
       at default.  DTLB/SLB misses are the metric that explains a win:
       `perf stat -e dTLB-load-misses` (or the POWER8 PM_DTLB_MISS event)
       on the GameThread for 30 s each arm.
16. [closed] If it wins, make the lever a lane default in steamtool/proton like
       HWTSO; if it loses, keep the lever off and record why (THP zeroing
       and khugepaged compaction are the usual reasons).

## RESULT, Part 1 (2026-09-15 22:37-22:49 box time, op64k, SMT=4, performance governor,
## bridge 75089121254a, MangoHud off, user's day-to-day knobs FEX_CODECACHESCOPE=all
## FEX_ENABLECODECACHINGWIP=1 FEX_HOSTPAGEMODE=force carried, A A B B)

| arm | leg | avg fps | min | max | frames | floor ms |
|---|---|---|---|---|---|---|
| HWTSO=1 (default) | 1 | 31.14 | 23.10 | 40.51 | 2002 | 22.89 |
| HWTSO=1 (default) | 2 | 31.92 | 23.55 | 41.18 | 2051 | 22.43 |
| FEX_HWTSO=0       | 1 | 28.40 | 20.84 | 37.16 | 1826 | 24.59 |
| FEX_HWTSO=0       | 2 | 28.39 | 20.55 | 37.22 | 1825 | 24.66 |

Hardware TSO (PROT_SAO) is worth **+11% average, -8.5% floor** on the Cyberpunk
flythrough; leg-to-leg spread inside each arm was under 0.8 fps, well outside
noise.  Validated per leg from the prefix logs: both A legs print "FEX_HWTSO
live" twice and "lane default FEX_HWTSO=1"; both B legs print FEX_HWTSO=0 in
the received environment and no live line.  dmesg: zero amdgpu ring timeouts
across all four legs.  Nothing to ship: HWTSO is already the lane default.
Side observation: 31-32 fps is the best this title has ever measured here
(previous record 24.42 on the 4K kernel, 2026-09-06); the 64K kernel and
today's d3d9/winecom commits are the candidates, not separated.

Step 13 probe (ppc64le/cpu/probes/sao_thp_probe.c): 64 MB anonymous, MADV_HUGEPAGE,
then mprotect(PROT_SAO): AnonHugePages stayed 65536 kB and the `ar` flag appeared
on the same VMA.  SAO and THP coexist; the THP lever does not have to choose.
Hugepagesize on hash-64K is 16 MB.

Mishap worth remembering: the first launch used run-native --name cp2077 (the
script's old hardcoded name) and booted a FRESH prefix; the game died there with
c000001d before any window and sat 25 min in the crash handler.  The script now
refuses a NAME whose prefix does not exist or does not match RESULTS.  Log kept:
~/Games/wine-ppc64le-stuff/logs/ on op64k (fresh-prefix crash) -- a separate bug.

## RESULT, Part 2 step 14 (THP `always`, no code; control = the two HWTSO=1 legs above)

| arm | leg | avg fps | min | max | frames | floor ms |
|---|---|---|---|---|---|---|
| THP madvise (control) | 1 | 31.14 | 23.10 | 40.51 | 2002 | 22.89 |
| THP madvise (control) | 2 | 31.92 | 23.55 | 41.18 | 2051 | 22.43 |
| THP always            | 1 | 31.46 | 23.70 | 40.77 | 2022 | 22.67 |
| THP always            | 2 | 31.54 | 22.90 | 41.16 | 2027 | 21.96 |

Average is a wash (31.50 vs 31.53).  Floor is 0.2-0.5 ms lower with THP,
inside the within-arm spread (0.46 / 0.71 ms), so not a measured gain.  With
`always`, 1.36-1.82 GB of the game's ~6.5 GB RSS was huge-page backed
(smaps_rollup samples), thp_fault_alloc +401, thp_fault_fallback +323 (compaction
misses -- the box had been up since 09-14).  Step 15 (TLB counters on the
GameThread, one leg per mode) decides whether TLB misses are a factor at all;
if they are not, the step-12 lever is closed on data and stays unwritten.

## RESULT, step 15 (TLB counters, whole game process, 30 s mid-flythrough, one leg per mode)

| THP mode | AnonHugePages | cycles | instructions | dTLB misses | iTLB misses | leg avg fps | floor ms |
|---|---|---|---|---|---|---|---|
| madvise | 256 MB | 1.290e12 | 5.680e11 | 115.7 M | 3.76 M | 30.95 | 22.66 |
| always  | 2.19 GB | 1.276e12 | 5.692e11 | 72.2 M | 2.47 M | 30.91 | 23.14 |

THP removes 38% of the data TLB misses and 34% of the instruction TLB misses,
and it does not matter: 116 M misses in 1.29e12 cycles is one miss per 11,000
cycles, under 1% of the time even at a pessimistic few hundred cycles per hash
walk.  fps and floor are a wash in both pairs (floor went the other way this
time).  CLOSED ON DATA: the game's frame time on this box is not TLB-bound, so
the step-12 virtual.c lever (MADV_HUGEPAGE on large views) is not written and
step 16 does not apply.  THP `enabled` was restored to `madvise`.

What this session leaves for the speed list: HWTSO stays the default (+11%
proven), THP is off the table, next candidates are PGO on ntdll.so/DXVK,
guest-side EnterCriticalSection, native large memcpy, and the queued
crossing cuts (see the 2026-09-15 conversation notes in NEXT.md's successor).

## Step 9 (Witcher 3) -- what got in the way, 2026-09-16 00:00-00:40

1. **Save-load crash, intermittent, native heap.**  Two of three launches
   died while loading the pinned save: glibc printed `malloc(): smallbin
   double linked list corrupted` / `corrupted double-linked list`, then the
   port logged EXCEPTION_WINE_ASSERTION (80000101 = SIGABRT) on thread 0238
   with the exception frame on a unix stack.  The user's allocator
   serialization guard (FEX_GUESTANCHOR/GUESTSERIALIZE_RVA, the witcher3
   registry row) WAS armed in every run, so this is not the guest-allocator
   race that guard closes; it is a native-side (wine/DXVK/winecom) heap
   overrun on the save-load path.  The op64k build is from 09-11, before the
   09-15 winecom/d3d9 commits, and the same build exited clean on 09-12/13,
   so it is not new code.  The third launch (gdb attached with sudo, which
   slows the emulator's signal path) survived and ran at ~50 fps in-world.
   Logs: ~/.local/share/wine-ppc64le/nw-witcher3/wine-ppc64le-native-20260915-235618-*.log
   and -20260916-000709-*.log.  NEXT: run one leg with
   LD_PRELOAD=/usr/lib/libc_malloc_debug.so GLIBC_TUNABLES=glibc.malloc.check=3
   under `sudo gdb -p <pgrep -x witcher3.exe>` (ptrace_scope=1 blocks a
   plain attach) with SIGABRT stop, everything else nostop/pass -- the
   attach recipe is /tmp/w3gdb.sh on op64k.
2. **Key injection.**  ~/fex-scripts/sendkey.py needs the python `evdev`
   module, which no python on op64k has any more; the driver now uses
   `YDOTOOL_SOCKET=/run/user/1000/.ydotool_socket ydotool key 57:1 57:0`
   (SPACE) / `28:1 28:0` (ENTER) with ydotoold running.  A leg whose keys do
   not land sits at the main menu at a flat 120 fps -- scene_stats.py then
   says "NO SCENE", which is the tell.
3. **Save moved to Novigrad** by the user at 00:32 (QuickSave_10ff48_7ea43c00_204e7a);
   re-pinned, every leg restores it, autosaves the game writes are discarded.
4. **dmesg (sudo):** 33 emulator-side entries this afternoon (fastppcx86 `trap`
   at FEX+0x3a4e80 in short-lived pids, two segfaults at -1) -- theirs, handed
   over as-is; ONE `wine-preloader[921975]` segfault at 18:38:24 inside
   wld_vsprintf's `%s` (loader/preloader.c) dereferencing an argv/env string
   at 0x7ffffe00f925 that was no longer readable.  Preloader page math uses
   AT_PAGESZ, but loader/preloader.c:1548 hardcodes `pargc - 0x1000` for the
   stack-overlap test -- on 64K pages that is the wrong margin.  Single
   occurrence, unreproduced; a lead, not a finding.

## Witcher 3 water scene: 30 fps (09-13 21:19) -> 50 fps (09-16 00:30), same 64K kernel line, same save spot

The wine binaries on op64k are from 09-11 in both runs, so this is not a
wine change.  The governor is ruled out by the user (known for a fact).
What did change in between: fastppcx86 build-smc rebuilt 09-15 16:33 with 65
commits since 09-13 (64K granule/shared-file mapping fixes, delayed
cache-load UAF, SMC verify-after-arm, and "wine's KUSER_SHARED_DATA clocks
are refreshed into the guest's private copy" on 09-14 20:50 -- the guest had
been reading a stale shared-data page on 64K), the bridge rebuilt 09-14
17:32, and linux-books-64k 7.2.5-2 -> -3 booted 09-13 21:49.  Attribution:
the emulator rebuild first, the kernel bump second.  Not a wine finding;
recorded so the next W3 number has a baseline with a date on it.

## RESULT, step 9 -- Witcher 3, Novigrad quicksave, native lane, A A B B (2026-09-16 00:37-01:49)

Each leg: launch, 240 s to menu, Continue via ydotool, 420 s streaming, then
the MangoHud CSV's gameplay scene isolated by ~/fex-scripts/scene_stats.py
(~17-18k scene frames per leg).  No leg crashed; no retries.

| arm | leg | scene fps | p50 ms | p95 ms | p99 ms |
|---|---|---|---|---|---|
| HWTSO=1 (default) | 1 | 24.93 | 40.00 | 47.33 | 50.62 |
| HWTSO=1 (default) | 2 | 24.20 | 41.67 | 47.28 | 50.71 |
| FEX_HWTSO=0       | 3 | 22.49 | 46.89 | 53.43 | 56.84 |
| FEX_HWTSO=0       | 4 | 22.98 | 45.26 | 52.16 | 57.74 |

Hardware TSO: **+7.5% scene fps, -12% median frame time, -11% p99** in Novigrad.
Same direction and size class as Cyberpunk (+11%).  Validated per leg from the
prefix log (live line present in A, FEX_HWTSO=0 received and no live line in B).
Nothing to ship -- it is the lane default; the number was owed.
