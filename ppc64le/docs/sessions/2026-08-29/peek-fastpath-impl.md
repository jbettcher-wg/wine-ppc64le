# The PeekMessageW fast path: both gates run, verdict DO NOT BUILD — 2026-08-30

Assignment: implement the generalized guest-side `PeekMessageW` fast path of
`top-consumer-designs.md` §A, gated behind the two experiments that design
specifies (A.2 shape capture, A.7 delay-injection A/B), building only if
serve-fraction × elasticity ≳ 0.6.

**Verdict: f ≈ 1.0, λ ≈ 0. f·λ ≈ 0 — far under the 0.6 bar.  The fast path
was NOT built into the shipped DLLs.**  The storm is real, it is one shape,
the generalized predicate would serve essentially every call of it — and
serving them recovers no frametime, because the storm is an elastic idle
spin, not critical-path burn.  The gates did exactly the job they were
specified for: ~4 benchmark legs and one temporary win32u instrumentation
killed a 2–3 day implementation-and-validation effort that would have
shipped a behavior-bearing guest fast path for nothing.

The implementation was nonetheless drafted before the verdict landed (it was
written while the legs ran) and is parked, unshipped, on the local branch
`peek-fastpath-generalized` — see §5.  Its dry-run artifacts pass
`spec2thunk-check`; its probe suite is written but HAS NEVER RUN.  Nothing
of it is in any built DLL.

All numbers below are from 2026-08-30 on the AC922 (SMT=2 era — today's
clean floor is ~38.2 ms, not the 34.68 of the SMT=4 baseline).  Instrumented
legs: `WINE_PPC64LE_PEEK_SHAPE=200` armed in all of legs 1–4; wine logs at
`~/.local/share/wine-ppc64le/cp2077/wine-ppc64le-native-20260830-{100156-1392262,101106-1395116,101424-1396167,…}.log`,
results under `…/benchmarkResults/benchmark_2026-08-30_{11-02-23,11-11-34,11-15-00,…}`.

---

## 1. Gate 1 — shape capture: the storm is ONE shape and f ≈ 1.0

Instrumentation: env-gated capture in `dlls/win32u/message.c`
`NtUserPeekMessage` (`WINE_PPC64LE_PEEK_SHAPE=N`: N verbatim lines, then a
histogram keyed by `(hwnd!=0, first, last, flags)` with the shm state read
under one seqlock pass, the A.4 generalized predicate computed in C on the
spot, dumped every ~10 s).  This code is kept in the tree — one predicted
branch per call when unarmed — so any future leg can re-measure.

[MEASURED] One benchmark leg (leg 1), GameThread tid 0130, final totals:

| shape | calls | empty | msg | generalized-predicate serve | narrow (old body) serve |
|---|---:|---:|---:|---:|---:|
| `hwnd=0, 0, 0, flags=0x00400002` | 34,577,484 | 34,577,483 | **0** | 34,577,482 (99.99999%) | **0** |
| `hwnd=0, 0, 0, flags=0x00001` | 5,344 | 848 | 4,496 | 849 | 849 |

* The storm shape is `PeekMessageW(&msg, NULL, 0, 0,
  PM_NOREMOVE | PM_NOYIELD | PM_QS_SENDMESSAGE)` — "is a cross-thread
  SendMessage pending?", polled ~300k/s (this leg; 247.8k/s in the
  TRAP_STATS leg).  It NEVER returned a message in 34.6M calls.  It misses
  the shipped narrow fast body solely because of the `PM_QS_SENDMESSAGE`
  word — **H-shape, exactly**, not H-seed (seed slot present and clean on
  every call) and not H-state (ONE bits-refusal and ONE mask-refusal in
  34.6M: the interleaved-wait mask churn §A.3 predicted self-heals, measured
  at 1 occurrence).
* The generalized predicate for this shape degenerates to
  `masks==0 && !(wake_bits & QS_SENDMESSAGE)` — the cheapest possible case,
  and sent-message liveness is carried by the tested bit itself.
* The second shape is the real pump (`PM_REMOVE`, null filter): 84% of its
  calls RETURN a message — the population the empty-only design excludes,
  and it is 0.015% of call volume.  Its refusal bits (QS_PAINT 4.2k,
  QS_MOUSEMOVE 1.0k, QS_TIMER 0.7k, QS_RAWINPUT 1.4k) are all
  message-present cases, i.e. correct refusals.
* `servemsg` (predicate said empty, call returned a message — the benign
  post-after-snapshot race) : **1 in 34.6M**, matching §A.5's linearization
  argument.
* No other shape exists.  No hwnd'd peeks, no ranged peeks, from anything
  in the process worth counting (launcher processes contribute a handful of
  null-filter `PM_REMOVE` calls).

So the capture half of the decision table said: build A.4, it will serve
~99.99% — IF the elasticity half agreed.

## 2. Gate 2 — delay injection: the storm absorbs added cost, λ ≈ 0

Instrumentation: `WINE_PPC64LE_PEEK_DELAY_NS=X` — a `clock_gettime`
busy-wait inserted immediately before every empty return of
`NtUserPeekMessage` (same win32u build; also kept in tree).  Verified armed
in every leg ("PEEKDELAY armed" in 3 processes/leg) and verified *effective*
by an independent channel: the probe's fastness layer moved 25→43 ns under a
5 µs delay (the heartbeat traps' amortized share, 5000/256 ≈ 20 ns — the
arithmetic closes), and the in-game poll rate responded exactly as an
elastic loop must.

[MEASURED] Four legs plus the other agent's clean 09:54 run as reference.
Frametime stats from `frames.csv` (not the summary's smoothed min/max);
peek rate from the capture's own 10 s dump deltas:

| leg | delay | capture | storm rate | injected/frame (median) | min ms | p5 | median | mean |
|---|---:|---|---:|---:|---:|---:|---:|---:|
| ref 10-54-49 | — | off | — | — | 38.21 | 46.46 | 61.81 | 64.86 |
| 1 (11-02-23) | 0 | on | ~305k/s | 0 | 38.20 | 48.35 | 68.00 | 70.23 |
| 2 (11-11-34) | 2000 ns | on | ~165k/s | **~20.7 ms** | 38.14 | 44.80 | 62.79 | 65.28 |
| 3 (11-15-00) | 5000 ns | on | ~103k/s | ~37 ms | 45.69 | 54.62 | 72.56 | 75.70 |
| 4 (11-19-07) | 5000 ns | on | ~95k/s | ~30.3 ms | 38.28 | 43.75 | 63.78 | 65.60 |

Every leg completed (1002–1080 frames, plausible averageFps 13.2–15.4;
run-to-run variance on this machine is ~20% and the floors list in the
budget doc spans 31.5–42.1 ms).

**The 2000 ns point is the verdict.**  Injecting 2 µs into every empty peek
— MORE than the ~1.4 µs the fast path could remove — added ~20 ms of peek
cost to every median frame, and the frametime floor moved **−0.06 ms**
(38.20 → 38.14; median −5.2 ms, i.e. noise-negative).  The loop simply
iterated half as often (305k/s → 165k/s) inside the same wall time.  That
is the elastic-spin signature of §A.7, measured: the peeks fill idle time
on a thread whose frame duration is bounded by something else.  λ(±2 µs
around the current cost) = 0.0 ± 0.15, and the fast path's removal lives
inside exactly that neighborhood.  f·λ ≤ ~0.15 < 0.6.

The 5000 ns legs probe further out: leg 3 rose (floor +7.5 ms, median
+4.6 ms over leg 1) — but its replicate, leg 4, landed exactly back on
baseline (floor 38.28, median 63.78) while injecting ~30 ms of peek cost
into every median frame.  Leg 3 was the documented ~20% run-to-run
variance, one outlier in five runs, and the elasticity conclusion is
replicated: **the spin absorbs at least 30 ms/frame of added peek cost with
zero frametime effect.**  λ ≈ 0.0 across the whole 0–5 µs range, and you
cannot recover frametime by cheapening calls whose current cost is already
fully absorbed.

Reconciling the design's two priors: both were right.  The thread IS
on-CPU 87.6% and the peeks ARE finely interleaved with work (no multi-ms
spin phase) — but the interleaved peek bursts are *waiting*, not working:
an as-fast-as-possible scheduler/wait loop that polls for sent messages
while real work is blocked on dependencies elsewhere.  The per-second-
constant rate (203k/s loading, 248–305k/s in game) was the honest tell.

## 3. What this changes in the budget and the roadmap

* The "~12 ms PeekMessageW storm" line of `frame-cost-budget.md` §4 is real
  CPU but **not recoverable frametime**.  The floor's true composition is:
  the peek line's ~12 ms is slack-filling; the binding constraint is
  whatever the GameThread's spin is waiting FOR.
* **Report item 2 (cheapening the trap/crossing) is deprioritized for fps**
  by the same measurement — most of its beneficiaries are these same
  elastic peeks.  Its remaining value is the non-peek crossings
  (~2–3k/frame on the GameThread).
* The next lever, per §A.7's own pivot: **off-CPU/wakeup analysis** of what
  the storm-polling loop unblocks on — `perf sched`/wakeup tracing of the
  GameThread's dependencies (worker threads, fences, the redDispatcher
  side), plus Part B's JIT attribution which is untouched by this result.
* The fast path itself remains a *CPU/power* win, not an fps win: at
  305k/s × ~1.6 µs the storm burns ~50% of a hardware thread that an SMT=2
  sibling could use.  If that ever matters, the parked branch is the work.

## 4. What is in the tree (main line)

* `dlls/win32u/message.c` — the A.2/A.7 instrumentation, env-gated, one
  predicted branch per call when off: `WINE_PPC64LE_PEEK_SHAPE=N` (capture +
  10 s histogram dumps via ERR) and `WINE_PPC64LE_PEEK_DELAY_NS=X` (empty-
  return busy-wait).  Capture cost when armed: leg 1 vs ref ≈ −7% avg fps,
  TRAP_STATS-sized; uniform across legs so the A/B slope is unbiased.
* `dlls/win32u/winstation.c` — C_ASSERTs pinning the `shared_object_t`/
  `queue_shm_t` offsets the guest fast body hardcodes (+0 seq, +24
  wake_mask, +28 wake_bits, +32 changed_mask, +36 changed_bits).  The
  drift fence for §A.5's named residual risk was previously comments only;
  an upstream rebase that moves a field now fails at compile time.

## 5. The parked implementation (branch `peek-fastpath-generalized`)

Written during the legs, then gated OFF the main line by the verdict.
Contents:

* `tools/spec2thunk/spec2thunk` — the 'peek' fast body generalized from
  null-filter-only to every shape: signal/clear computed from the call's
  own `flags`/`first`/`last` exactly as `peek_message()` does, then the
  same seqlock predicate against the same four shm fields.  hwnd and range
  no longer refuse admission (per §A.3 they cannot affect emptiness);
  `rdx` is pushed around the seqlock window (register pressure: rax=signal,
  r11d=clear, r10=queue) and restored on every exit so the fallback stub
  still sees its arguments.  Seed check, sabotage tag, tick/256-budget
  heartbeat, fallback-jmp-first layout: unchanged.  One deliberate,
  documented divergence: an explicit `(first=0, last=~0)` caller gets
  `clear` without QS_ALLPOSTMESSAGE where native includes it — same
  empty/nonempty answer either way (pending posts show in
  `wake_bits & QS_POSTMESSAGE`, which IS tested), one-trap-deferred
  changed-bit clearing, same self-heal class as the mask rule.
* `ppc64le/cpu/probes/peek_shapes.c` — NEW probe: per-shape delivery /
  no-leak / no-swallow proofs (ranged, ranged-miss, hwnd=-1, message-only
  window hwnd, hwnd-miss, PM_QS_POSTMESSAGE, PM_QS_INPUT, the storm shape,
  PM_NOREMOVE double-view), cross-thread `SendMessageTimeoutW` liveness
  against a storm-shape poll loop (the deadlock class made loud), and a
  5000-serial exactly-once in-order differential fuzz layer; plus a starve
  mode reporting GOT/STARVED per shape for the sabotage controls.
* `ppc64le/cpu/check-peek-fastpath.sh` — extended: builds/runs the new
  probe in the fast lane AND the trap-only lane (the differential half),
  and requires per-shape STARVED under sabotage / GOT under sabotage+kill.

Validation state, honestly: `spec2thunk --spec user32.thunks` dry-run to
`/tmp/peekgate/user32-test.dll` assembles, `spec2thunk-check --body trap`
passes including 8q (fast-body head + fallback-jmp contract; the two FAILs
are dry-run naming artifacts), and the emitted body was disassembled and
read against the predicate.  **The probe suite has never executed** — no
DLL carrying the generalized body has ever run.  If this branch is ever
landed: run the full extended gate (positive, differential, sabotage,
kill), then §A.6.5's in-game TRAP_STATS + manual input validation.  Do not
skip the sabotage legs; they are what prove the guest body, not the trap,
answers each shape.

## 6. Sniff test around the message path

Read with fresh context; NOT fixed unless noted:

1. **`NtUserPeekMessage` ignores `PM_NOYIELD`** (`dlls/win32u/message.c`
   empty path): every empty peek pays `NtYieldExecution` plus TWO
   `KeUserDispatchCallback` thunk-lock round trips (each a user-mode
   callback crossing on this port), while Cyberpunk's storm shape
   explicitly passes PM_NOYIELD.  Upstream Wine behaves the same, and
   PM_NOYIELD's documented semantics are the Win16 cooperative one, so
   honoring it is a behavior question — but on this port those two
   callbacks are a measurable slice of the 1.4 µs.  Elasticity makes this
   worthless for fps (same reason as the fast path); worth remembering if
   the CPU/power angle ever matters.
2. **The `queue_shm` layout had no compile-time fence** against upstream
   field moves while two generated guest bodies hardcode its offsets.
   Fixed on main (winstation.c C_ASSERTs) — trivial and safe.
3. **`spec2thunk-check` 8q verifies only the body HEAD and the fallback
   jmp**, not the tested constants (0x1cff/0x1dbf today) — the "third
   spelling" discipline covers the QPC block layout but not the peek
   predicate.  If the generalized body ever lands, its constants join the
   comment-only tier; a disassembling check of the immediates would close
   it.  Not fixed (checker surgery is not a sniff-test change).
4. **The ecs/lcs fast bodies read clean** against RtlEnterCriticalSection
   semantics (cmpxchg −1→0 acquire, owner-then-recursion restore order on
   the failed-release undo, kill switch first).  One observation: the ecs
   recursion re-entry path does `lock addl $1` on LockCount then increments
   RecursionCount non-atomically — correct, because only the owner writes
   RecursionCount while holding, same as native.
5. **SRW lock exports** (`RtlAcquireSRWLockExclusive` + 3 siblings,
   ~38.6k/s in the budget's table) are the nearest same-class candidate for
   a future fast body — but the lesson of this session applies: measure
   elasticity of the caller first.  Most SRW traffic is on worker threads
   whose slack is unknown.
6. **Operational, cost me one leg's wait and worth writing down**: a
   `pgrep -f "Cyberpunk2077.exe"` wait loop matches ITS OWN or any other
   shell whose command line contains the string — a stale such watcher
   (PID 1379771, from a previous session) was self-matching in an infinite
   loop and made my first leg's wait loop hang too.  Killed it.  The robust
   spelling is `pgrep -x GameThread` (the game process's comm), which
   cannot match a shell.
7. The capture itself sanity-checked the shipped narrow body end to end in
   passing: null-filter `PM_REMOVE` calls that reach native and would have
   matched the narrow shape are exactly the heartbeat/bits-forced traps
   (`narrow=849` on 5.3k trapping calls, the other ~99.98% of that shape's
   polls served invisibly guest-side), and `seednull=1` occurs once per
   thread — the seeding call itself.  The fast path machinery works as
   documented; Cyberpunk just calls a shape it never covered.

## 7. Reproduction

```
# capture:  WINE_PPC64LE_PEEK_SHAPE=200  (ERR-channel PEEKCALL/PEEKSHAPE/PEEKBIT lines)
# delay A/B: WINE_PPC64LE_PEEK_DELAY_NS={0,2000,5000}
cd ppc64le/steamtool && WINE_PPC64LE_PEEK_SHAPE=200 WINE_PPC64LE_PEEK_DELAY_NS=2000 \
  ./run-native --name cp2077 --appid 1091500 ".../Cyberpunk2077.exe" -skipStartScreen -benchmark
# floors from frames.csv, never from the summary's min/max fps fields
```
