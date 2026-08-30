# Cyberpunk 2077 frame-cost budget on the ppc64le port — 2026-08-30

Question: where does the 34.7 ms per-frame floor go (baseline `benchmark_2026-08-30_01-06-56`,
4K Medium FSR2, AC922 POWER9, RX 7900 XTX, avg 18.49 fps, min frametime 34.68 ms, GPU nearly idle)?

Headline: **a COM crossing costs ~0.8–1.0 µs, not 5–10 µs — the COM class does not explain the
floor and mostly runs on worker threads.  The floor lives on the GameThread, and roughly half of
it is crossing machinery, of which the single biggest named consumer is a ~248,000/s
PeekMessageW storm that the existing peek fast path serves none of.**  vkd3d translation is <1%
of the GameThread.

Accounted: ~33–34 of the 34.7 ms, with the scaling assumption stated in §4 (window→floor
composition).  Honest residual: ~1–2 ms plus that assumption.

---

## 1. MEASURED: cost per crossing (probe, this run)

`probes/guest/com_crossing_cost.c` (new; freestanding x86-64 guest PE, headless
`D3D12CreateDevice(NULL, FL11_0)` on the real stack: guest COM stub → ntdll COM dispatch →
native d3d12 winecom → unixlib → vkd3d-proton).  Timed with the QPC guest fast path,
2026-08-30 on the AC922, machine otherwise idle.  Build/run recipe in the file header;
binary and log at `/tmp/comcost/` on the AC922, log
`~/.local/share/wine-ppc64le/probe/wine-ppc64le-native-20260830-011308-1336198.log`.

| loop | ns/call | what it isolates |
|---|---:|---|
| guest-local call | 6.4 | JIT baseline (matches crossing_cost.c's 6.6) |
| QueryPerformanceCounter | 25.3 | the guest-side QPC fast path — works |
| GetTickCount | 543.8 | flat crossing reference (matches prior 558) |
| **AddRef+Release, halved** | **792.8** | **pure COM trap + dispatch** (IUnknown slots are proxy-served, no marshal walk, no host callee) |
| GetNodeCount | 821.0 | + marshal walk + unixlib + trivial vkd3d callee |
| GetDescriptorHandleIncrementSize | 863.1 | same, one scalar arg |
| ID3D12Fence::GetCompletedValue | 955.1 | a real polled getter |
| GetGPUVirtualAddress (const-cached) | 6.5 | the WINECOM_F_CONST_QWORD guest-side cache — **guest-local speed** |
| CreateConstantBufferView (null {0,0}) | 884.5 | hottest still-trapping device row |
| CopyDescriptorsSimple (count 1) | 872.8 | the other still-trapping descriptor row |

So: **the COM mechanism is ~0.79 µs and marshalling+vkd3d adds only 30–160 ns on trivial
methods.**  The "COM bridge is a much heavier mechanism" premise is wrong by an order of
magnitude — it is ~1.5x a flat crossing, not 10x.  These are hot-loop, single-thread,
cache-warm numbers; §4 derives the in-game cost (~1.0–1.2 µs) from the profile.

## 2. MEASURED: per-frame call counts (one instrumented benchmark leg)

One `-benchmark` run 2026-08-30 01:15 (`benchmark_2026-08-30_02-15-48` in-prefix clock),
`WINE_PPC64LE_TRAP_STATS` armed + `FEX_LIBRARYJITNAMING=1` + a 15 s tid-scoped perf capture.

**Perturbation check (against the clean 01-06-56 baseline):** avg 18.49 → 17.31 fps (−6.4%,
matching TRAP_STATS' documented 6.2% cost); frametime floor 34.68 → 35.34 ms (+1.9%); shape
preserved (median 51.79 → 55.18, p99 88.68 → 101.11).  Ranking is unbiased (every counted event
pays the same add); absolute rates are conservative by ~6%.

Flythrough window: 16.85 s (112.75 → 129.60 s of process life), **251 frames** (window frames
avg 67.2 ms — end-of-route is heavier than run average).  Raw snapshots at
`/tmp/bench-instr-0830/xstat-{early,late}/` on the AC922; subtraction via
`ppc64le/cpu/xstat_window.py`.

26.61M crossings in the window = **1.579 M/s = 106k per window-frame** (at the floor frametime
of 34.7 ms that is ~54,800 per frame).  By class: syscall 976k/s, flat 397k/s, com 206k/s
(callback/event noise).  Top rows:

| row | /s | per window-frame | note |
|---|---:|---:|---|
| NtCallbackReturn | 603,665 | 40,530 | the trap-return; 1:1 with flat+com, identity holds |
| **PeekMessageW (+ NtUserPeekMessage each)** | **247,802** | **16,635** | **every single one a real trap AND a real syscall** |
| ID3D12Device::CopyDescriptors | 102,464 | 6,879 | worker-side (command recording) |
| ID3D12Device::CreateConstantBufferView | 56,346 | 3,783 | worker-side; device journal off by default |
| WaitForSingleObject | 31,344 | 2,104 | |
| ReleaseSemaphore | 30,961 | 2,078 | job-system wakes |
| NtWaitForAlertByThreadId / NtAlertThreadByThreadId | 20,812 / 19,802 | ~1,400 each | |
| SRW lock ops (4 rows) | ~38,600 | ~2,600 | |
| whole COM class | 206,455 | 13,860 | |

The peek fast body IS present in the built guest user32.dll (gs:0x198 signature at 0x10de0),
but flat==syscall count equality says **none of CP2077's peeks are served by it** — the game's
call shape (non-null hwnd, ranged filter, or PM_QS_* flags — not yet captured) never matches the
null-filter-only fast shape.  Peek rate is ~constant across phases (203k/s during loading,
248k/s in flythrough), which smells like a poll/busy-wait idiom, not a once-per-frame pump.

## 3. MEASURED: where the GameThread's time goes

perf, 14,881 samples @999 Hz, tid-scoped to the main thread (tid==pid, comm "GameThread" — note
18 threads carry that comm; this is the one prior work identified as the bound thread), last
~15 s of the flythrough.  GameThread on-CPU **87.6%** of wall time.  Data:
`/tmp/bench-instr-0830/perf-gamethread.data`.

| bucket | % of thread | top symbols |
|---|---:|---|
| JIT'd guest code | 37.6 | anonymous JIT blocks |
| trap/dispatch/bridge machinery | ~34 | __wine_syscall_dispatcher 11.7, libfexbridge 13.2 (mostly anon = state sync/trampolines), call/return_user_mode_callback 3.3, emu_trap_dispatch 2.0, thunk_rip_cache_get 1.2, emu_trap_thunk/teb_switch 1.7 |
| win32u (the peek's native side) | 8.8 | get_tick_count 3.8(!), get_user_thread_info 1.9, NtUserPeekMessage 1.1, get_shared_queue 1.0, peek_message 0.6 |
| libc | 8.4 | pthread_mutex_lock 1.8, anon 1.8, getrusage 1.4, memset 0.9, sched_yield 0.8, pthread_getspecific 0.7 |
| **xstat_hit (my instrumentation)** | **6.5** | subtract before scaling |
| vkd3d-proton + RADV | **0.9** | — the "cost is inside vkd3d's translation" hypothesis is dead for the floor |

## 4. The arithmetic: the 34.7 ms floor

Derivation chain (each step labeled):

* [MEASURED] GameThread util 87.6%; profile fractions above, renormalized without the 6.5%
  instrumentation: JIT 40.2%, machinery ~37%, win32u 9.4%, libc 9.0%, vkd3d 0.9%, small tail ~3%.
* [INFERRED] At a floor frame the same composition holds and the thread is ~fully busy
  (the floor is by definition the frame with no scene slack).  This window→floor scaling is
  the one big assumption in this budget.
* [MEASURED] Peek rate 247.8k/s → ~8,600 peeks per 34.7 ms floor frame; process-wide non-peek
  flat is 149k/s and COM 206k/s, of which the GameThread plausibly owns only ~1–3k/frame
  (the big COM rows are command-recording, i.e. redDispatcher-side).  So GT crossings/frame
  ≈ 10–12k, ~75–85% of them peeks.
* [DERIVED] In-game crossing machinery cost = machinery time ÷ GT crossings ≈ 13 ms ÷ ~11k
  ≈ **1.0–1.2 µs per crossing** — about 2x the hot-loop probe, which is what cache/branch
  pressure does; the probe and the profile are consistent.
* [DERIVED] Native peek work = win32u time ÷ peeks ≈ 3.3 ms ÷ 8.6k ≈ 0.4 µs each.

Floor budget (34.7 ms):

| component | ms | basis |
|---|---:|---|
| JIT'd guest code (the game's own work) | ~14.0 | measured fraction × floor |
| PeekMessageW storm, total (trap machinery + native peek) | ~12 | 8.6k × ~1.4 µs; = its share of machinery + all of win32u |
| other GameThread crossings' machinery | ~4–5 | remainder of the machinery bucket |
| libc (mutex, getrusage, memset, TLS) | ~3 | measured fraction |
| vkd3d + RADV on GameThread | ~0.3 | measured fraction |
| unattributed residual | ~1–2 | sub-cutoff symbols + off-CPU (12.4%) |

**Accounted: ~33–34 of 34.7 ms**, conditional on the composition-scaling assumption.
Equivalently: ~40% of the floor is the game's own code under the JIT, ~50% is crossing
machinery + peek, ~10% libc/residual.

What the COM class actually costs, for completeness: 206k/s × ~0.85 µs ≈ 175 ms of CPU per
second — ~10 ms per frame — **but spread across ~25 worker threads that idle**, which is
exactly why the 2026-08-27 call journal measured fps-flat.  Batching COM further does not
move the floor.

## 5. Ranked fix plan (by measured value at the floor)

1. **Serve Cyberpunk's PeekMessageW shape guest-side — up to ~12 ms of the floor; the single
   largest addressable item.**
   * Step 1 (hours): capture the real call shape — a temporary counted ERR in win32u's
     NtUserPeekMessage (or the stub fallback) logging (hwnd, first, last, flags) for the first
     ~200 calls.  This decides everything downstream and also settles whether a TEB-seed bug,
     not shape, is the cause.
   * Step 2 (days): extend the spec2thunk 'peek' fast body to the observed shape.  The
     queue_shm wake/changed bits can prove emptiness for filtered polls too (win32u's own
     check maps filter ranges to QS classes); keep the existing force-real-peek budget and the
     sabotage/gate pattern from check-peek-fastpath.sh.
   * Risk: message starvation (input death) — subtle; that is what the gate exists for.
   * Honesty caveat: the phase-independent rate (203k/s loading vs 248k/s flythrough) suggests
     a poll/busy-wait idiom.  If the loop is genuinely waiting on something else, removing the
     per-poll cost wins less than 12 ms — but the thread is on-CPU 87.6%, so most of that poll
     cost is real critical-path burn.  Step 1's shape data plus one A/B run answers it for
     ~one day of work; expected outcome if 90% served: floor ~25 ms, avg fps up 20–30%.

2. **Cheapen the generic trap round trip — ~17 ms/frame of machinery+win32u is billed at
   ~1.0–1.2 µs per crossing; every 100 ns shaved saves ~1.1 ms per floor frame.**
   Named targets from the profile: `__wine_syscall_dispatcher` (11.7% of GT; every trap pays an
   NtCallbackReturn transit and every peek a second transit — a shortened return path for the
   trap-return case is the concrete candidate), libfexbridge anonymous 9.5% (residual state
   sync after ABI 5 — needs its own micro-profile), `get_tick_count` inside the peek path
   (3.8% of GT for reading a clock — worth a look independent of item 1).
   Effort: weeks, core-infrastructure risk, benefits every title.  Do after item 1, informed by
   its A/B.

3. **Name the libc getrusage/pthread_mutex_lock callers (~3 ms at floor).**  getrusage at 1.4%
   of the GameThread is odd (likely NtQueryInformation* thread/process times called at high
   rate); if it is a per-frame telemetry call it may be cacheable.  Half a day to attribute
   (perf callchain or gdb sampling), decide then.

4. **COM batching / device-journal hang hunt — deprioritize for frame rate.**  Measured: the
   COM class is worker-side; vkd3d on the GameThread is 0.9%.  The device journal's
   unattributed GPU hang is not worth chasing for fps; keep it off.  Batching value is CPU/power
   only (~175 ms/s of worker CPU back).

5. **Do nothing — rejected by the numbers.**  Only ~40% of the floor is the game's own code;
   the rest is port overhead with named, addressable mechanisms.  18.5 fps is not inherent to
   this hardware+title; items 1+2 together plausibly reach the mid-20s at 4K Medium.

## 6. Measured vs inferred, and open gaps

Measured directly: every number in §1–§3; the perturbation pair; the peek body's presence in
the built dll.  Inferred and labeled: window→floor composition scaling (§4); GameThread
ownership of ~all peeks (consistent with win32u time ÷ rate, not per-thread counted);
GT non-peek crossing share (bounded, not counted — TRAP_STATS is process-wide).  Unknown:
CP2077's actual peek arguments (step 1 of the plan); what the peek loop waits on; the
getrusage caller; per-thread crossing counts.  Also note 18 threads share the comm
"GameThread"; the profiled one is the main thread (tid==pid).

Artifacts: probe source `probes/guest/com_crossing_cost.c` (hangover-ppc64le tree, not under
git); AC922: `/tmp/comcost/` (probe + implibs + log), `/tmp/bench-instr-0830/` (notes, perf
data, xstat snapshots, thread lists), `/tmp/xstat-0830/` (live stats files), benchmark results
`benchmark_2026-08-30_02-15-48` beside the baseline.
