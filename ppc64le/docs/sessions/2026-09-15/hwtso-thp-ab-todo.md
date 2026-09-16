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
9. [ ] Witcher 3 pass (D3D11, 81% JIT frame thread -- the title most likely
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
