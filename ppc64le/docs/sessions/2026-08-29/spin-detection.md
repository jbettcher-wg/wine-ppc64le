# Generic guest-spin detection and mitigation (SpinSentinel)

Session 2026-08-30, fastppcx86 branch `power9team`. Everything below is
labeled **[MEASURED]** or **[INFERRED]**.

## Headline: does SPINCOLLAPSE generalise?

**No — and the reason is structural, answerable from source.** `FEX_SPINCOLLAPSE`
(`FEXCore/Source/Interface/Core/JIT/PPC64LE/JITClass.h` banner, matcher in
`JIT.cpp::AnalyzeSpinLoops`) is a *compile-time* IR-shape rewrite: within a
validated spin region it batches the budget decrement of a counted
`dec; jnz`-around-a-poll-load loop so each iteration retires K units. The
region validator **rejects any block containing a syscall, store, atomic or
cache op by construction** (the whitelist in `AnalyzeSpinLoops` admits only
"this thread's guest state" ops) — and that rejection is load-bearing for its
soundness argument. The same is true of the other two existing spin levers,
`FEX_SPINLOOPCLAMP[AUTO]` (decode-time CMP clamp) and the SMT priority hints:
all three see only side-effect-free loop bodies inside one translation region.

The three captured cases all live at the guest↔native boundary those matchers
must never admit:

1. **PeekMessageW storm** — every iteration is a real trap and a real
   syscall. Invisible to any block-level matcher, by design.
2. **224-deep mutual crossing recursion (Quake II)** — not a guest loop at
   all; it is a repeating cycle of `fexbridge_run` re-entries and trap
   crossings. No IR shape exists to match.
3. **Post-benchmark GameThread stall** — dynamic, shape unknown at compile
   time.

So the general case needs a **new, dynamic mechanism at the crossing layer**,
complementary to SPINCOLLAPSE rather than replacing it. Both boundary
phenomena funnel through exactly one choke point each in
`Source/Tools/FexBridge/FexBridge.cpp`: `BridgeSyscallHandler::HandleSyscall`
(once per guest→native trap) and `fexbridge_run` (once per native→guest
reverse crossing, with the full nesting chain already reified as the
`RunFrame` list). That is where SpinSentinel lives.

## What was built

`FEXBRIDGE_SPINSENTINEL` in `Source/Tools/FexBridge/FexBridge.cpp`
(fastppcx86 branch `power9team`). Default **ON, report-only**. Knobs
(getenv, same lane precedent as `FEXBRIDGE_EAGER_CTX`; steamtool
appconfig `.env` files reach them per-title):

- `FEXBRIDGE_SPINSENTINEL=0` — kill switch.
- `FEXBRIDGE_SPINSENTINEL_TRACE=1` — loud mode: no rate gate, no report caps.
- `FEXBRIDGE_SPINSENTINEL_THROTTLE=<usec>` — the only mitigation offered,
  strictly opt-in, loud at init.

### Detector 1: trap storms (case 1, case 3 when it crosses)

Per-thread 64-slot direct-mapped table keyed by `rip ^ mix(rax)` (rax
distinguishes syscall numbers behind a shared bop site). A slot counts traps
at its site; at 4096 it arms (one `clock_gettime`) and starts hashing the
Win64 argument registers (r10/rdx/r8/r9, raw values kept for attribution)
and comparing the post-callback RAX. At 65536 (then ×4) it reports, gated on
measured rate ≥ 5000 traps/s:

    fexbridge: SPINSENTINEL tid=<t> trap-storm rip=0x… rax=0x… count=… rate=…/s
        args=identical|vary result=identical|varies last-rax=0x… identical-run=…
        last-args=[r10 rdx r8 r9]
    fexbridge: SPINSENTINEL tid=<t> thread-profile: [rip=… rax=… count=…] …

`args=identical result=identical` at storm rate is the poll-storm signature;
`result=varies` distinguishes a hot-but-progressing site (QPC). The
`thread-profile` line lists the thread's other hot trap sites — the rest of
the spin's cycle — and `last-args` carries the raw first argument (for a Wine
syscall stub, often the HANDLE being polled), which is the "what is it
waiting on" hook the off-CPU analysis wants.

Reports are bounded: 16/thread, 128/process, ×4 count backoff, rate gate.

### Detector 2: crossing recursion (case 2)

`RunFrame` gained `EntryRIP`/`TrapRIP`; at each reverse crossing (~18/s on
the measured workload — the walk is free) the frame chain is depth-checked.
At depth 64 (then each doubling) the innermost 16 (reverse-entry, outer-trap)
rip pairs are scanned for the shortest repeating period ≤ 8 and the cycle is
named while the thread is still alive:

    fexbridge: SPINSENTINEL tid=<t> crossing-recursion depth=64 period=1
        cycle=[reverse=0x… trap=0x…] (guest<->native call cycle; stack
        exhaustion likely if it continues)

This turns the Quake II / DOOM class of death — 1 MB kernel stack exhausted
after 200+ crossings — into a named diagnosis ~160 frames before impact.
(The Wine-side `emu_crossing_dump` diagnostic fires only at the death guard;
this one fires early, from the emulator side, with the period computed.)

## Response policy — what is safe and why

Ranked by aggression, with the project's silent-wrong-answer rule applied:

- **Report (default, shipped).** Cannot be wrong in a way that harms the
  guest; bounded output. Per the delay-injection measurement on the
  PeekMessage storm (λ ≈ 0, 34.6M calls, zero messages ever returned —
  measured by the concurrent Wine-side effort), the diagnostic is the
  highest-value deliverable: it names the storm, its no-progress signature,
  its companions and its polled argument without anyone profiling.
- **Throttle (opt-in, shipped).** After 16384 consecutive traps at one site
  with identical argument registers AND identical result, each further such
  trap sleeps `THROTTLE` µs; any change in args or result resets the run.
  This executes everything the guest asked and changes only timing — a
  wrong call is indistinguishable from scheduling delay. Worst failure mode,
  stated concretely: one sleep in flight when a poll finally lands (≤
  THROTTLE µs added latency), or a throttled loop that a game uses as a
  spin-calibration (would read as a slow machine). It can never drop an
  input, elide a check, or corrupt state, because nothing is elided.
  **[INFERRED]** its win on the elastic PeekMessage storm is CPU/power, not
  frametime — do not evaluate it by fps.
- **Collapse / answer-without-executing: deliberately NOT offered.** The
  bridge cannot know Win32 semantics (PeekMessage returning the same TRUE
  twice can still have delivered two different messages through the
  out-pointer; an elided empty poll can race a just-posted message). That
  class of fix belongs in the embedder (Wine), which owns the semantics and
  is already building the PeekMessage fast path at source.

## Always-on cost

- Fast path added to `HandleSyscall`: ~10–25 instructions, one 128-byte
  line, no locks, no clock reads (arming/report paths only). **[MEASURED]**
  On a synthetic worst case — an empty trap loop at ~6M traps/s, 300k traps,
  5 runs each — sentinel ON 46.6–50.6 ms vs OFF 45.4–49.5 ms: the difference
  is inside the run-to-run spread, i.e. indistinguishable from zero; the
  spread bounds it at ≤ ~2% of an *empty* trap. A real trap carries a
  context marshal plus an actual syscall, so the real-workload fraction is
  far smaller. **[MEASURED]** one benchmark leg ran with the sentinel live;
  floor within the historical variance band (numbers below).
- Reverse-crossing path: a bounded chain walk at ~18 crossings/s — noise.
- Disabled (`FEXBRIDGE_SPINSENTINEL=0`): one predictable branch per trap.
- Memory: ~7 KB per guest thread.

## Verification

**[MEASURED] Functional, synthetic** (standalone dlopen harness, not
committed; BridgeSmoke's 145 checks all still pass):

- Storm: reports at count 65536/262144, rate ~5.8M/s, `args=identical
  result=identical`, correct rip/rax.
- Recursion: reports at depth 64 and 128, `period=1`, correct cycle rips.
- Kill switch: zero reports.
- Throttle: 40k identical traps, `THROTTLE=200`: wall 7.1 ms → 4906.6 ms —
  matching the predicted (40000−16384) × 200 µs ≈ 4.7 s within 4%.

**[MEASURED] Real title** (Cyberpunk 2077 benchmark, native lane, new bridge
via `WINEFEXBRIDGE`, report-only): within the first minute of loading the
sentinel named tid 1406594's message-pump storm unprompted:

    trap-storm rip=0x3fffffbb20e3 rax=0x40 count=65536 rate=413890/s
        args=vary result=identical last-rax=0x0 identical-run=61439
        last-args=[0x3fff411ef478 0 0 0]

— result identically 0 for 61k consecutive calls at 413k/s, first arg a
stack MSG pointer: the PeekMessage poll shape, found by the detector rather
than by profiling. `args=vary` is the MSG pointer moving with stack depth,
which is itself diagnostic.

The storm persisted through the whole benchmark at 250k-420k/s, result
identically 0 for runs of 61k/258k consecutive calls. **[INFERRED]** the
trap site follows the ntdll stub convention (rax = syscall id), which maps
rax=0x40 to `NtOpenEvent` in this tree's `dlls/ntdll/ntsyscalls.h`; that
mapping is unverified — resolving the rip against the live guest ntdll
mapping is the follow-up, and the raw numbers in the report are exactly
what that follow-up needs. The detector also named a second, previously
unprofiled storm on tid 1406605 (rip=0x3fffffbdbfb3, rax=0x8000a389 — a
different bop convention, r10 = rip+2) at 115k/s with result identically 0,
whose thread-profile line shows companions at 53k/313k counts — a candidate
for the "what is the GameThread waiting on" analysis.

**[MEASURED] Run health and floor** (one leg, sentinel live, report-only):
benchmark completed, frames.csv 1174 frames (vs 1083/1001 in the two
immediately prior legs — count scales with fps), averageFps 17.36 (prior
leg 15.24), frametime min 34.88 ms / p1 36.62 ms against the historical
floor band 31.5-42.1 ms. One leg proves only "no regression within the
~20% machine variance," and that is all that is claimed; the sentinel's
cost bound comes from the synthetic A/B above, not from this leg.

**[NOT MEASURED] Throttle on the real title.** Per the delay-injection
measurement (the storm is elastic, lambda ~ 0), the throttle's win on this
workload would be CPU time / power on the storming thread, not frametime.
Measuring that properly needs per-thread CPU-time A/B legs
(`FEXBRIDGE_SPINSENTINEL_THROTTLE=50` vs unset, comparing the storm
thread's utime and the package power), which was deliberately left for a
session that can also confirm the storm survives the Wine-side fast path
now being built — measuring a mitigation against a workload another agent
may remove at source proves nothing durable.

## Gaps, stated plainly

- **JIT-resident spins that never cross** (a pure guest busy-loop) are not
  seen by SpinSentinel; the static matchers (SPINCOLLAPSE et al.) cover the
  counted subset, and an uncounted non-crossing spin remains undetected.
  A dynamic in-JIT detector would tax every backedge to catch it — the bad
  2%-everywhere trade — or need a sampling watchdog thread; designed but not
  built this pass. Case 3 (post-benchmark GameThread) is only covered if its
  spin crosses (PeekMessage/QPC-shaped spins do). **[INFERRED]** that it
  does; not yet re-observed under the sentinel.
- The trap-storm detector keys on consecutive-site *counts* since slot
  creation, not a sliding window; a very-long-lived moderate site could
  report once with an honest (low-ish) rate in the line. The rate gate
  (5000/s) keeps this from eating the report budget.
- Slot aliasing (64 slots, direct-mapped) can only delay detection, never
  fabricate it.
- The emulated lane (FEXInterpreter + LinuxEmulation) does not run the
  bridge; SpinSentinel covers the native lane only. The emulated lane's
  equivalent choke points (its syscall handler) could host the same table if
  wanted.

## Files

- `fastppcx86:Source/Tools/FexBridge/FexBridge.cpp` — SpinSentinel (banner
  comment above the namespace is the design doc).
- `fastppcx86:Source/Tools/FexBridge/fexbridge.h` — knob documentation
  (no ABI change; no new exports).
