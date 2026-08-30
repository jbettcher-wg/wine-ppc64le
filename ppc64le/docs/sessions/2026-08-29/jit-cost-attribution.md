# The ~14 ms of JIT'd guest code in Cyberpunk 2077 — attribution and theories

2026-08-30, AC922 POWER9, ppc64le Wine port (`hangover-ppc64le` / `fastppcx86`,
branch `power9team`). Companion to `frame-cost-budget.md` (§6 there already
labels this bucket "MEASURED fraction, INFERRED window→floor scaling").

## Headline

**UPDATE (instruction-level follow-up, §9): the sharpest, most actionable finding in this whole report is in §9.1, not above.** `__wine_syscall_dispatcher` unconditionally saves the entire FP and VMX/vector register file (18 FPRs + FPSCR + 12 vector registers) on every single trap into the port — 65.5% of that function's own sampled cycles, roughly 6-7% of the ENTIRE GameThread — and its own restore path, later in the same function, is already gated on a dirty-state flag that, in this workload, is essentially never set. The function's own logic proves most of that save is dead work. This is a named, disassembly-confirmed, directly fixable pattern, not an aggregate statistic. Read the rest of the headline below for the broader module-attribution and IPC context, then go straight to section 9 for the instruction-level evidence the owner asked for.



**The JIT'd-guest-code bucket is not one thing and it is not flat.** Within
it: 71% is Cyberpunk2077.exe's own code, and *inside that*, two addresses 32
bytes apart in a single Audiokinetic Wwise function
(`AK::MemoryMgr::StartProfileThreadUsage`) account for ~14% of all JIT samples
in the capture window — a real, named, disassembly-confirmed hot spot, not a
"spread evenly, no hot spot" story. Separately, ~23% of every JIT compile
event measured is a **recompile of an address FEX had already compiled**
earlier in the same process — reproduced twice, independently, to within
0.3 percentage points. Both are genuine JIT-relevant findings. Neither,
on the numbers available, is large enough to be *most* of the 14 ms:
the recompile waste is bounded at roughly 0.1–0.3 ms/frame by the
compile-machinery time it can possibly come from, and the Wwise hot function,
while concentrated, is one C++ function doing real conditional/branchy work,
not obviously idle spinning.

**The hardware-counter verdict (added after the owner temporarily cleared
`nmi_watchdog` for one measurement — see §5b): GameThread runs at 0.61 IPC
with a 3.63% icache-miss rate and a 2.82% branch-miss rate, against 2.62 IPC
and ~0.005% miss rates on the JIT's own best-case hot loop — a 4.3x IPC gap
and 400–700x miss-rate gaps.** In raw terms that is exactly the "codegen or
layout is costing us" signal. But it is a whole-thread measurement, and only
~27–40% of GameThread's time is inside JIT'd guest code at all (§2) — the
rest is this port's own trap-dispatch and syscall-marshaling machinery,
which has every reason to be icache-hostile on its own account, independent
of how well FEX translates x86. **I cannot partition the gap between "the
JIT mistranslates guest code" and "the port's crossing machinery is what's
thrashing the frontend" without a per-sample (not whole-thread) PMU
breakdown, which I attempted and could not get working in the time
available (§5).** Given crossing machinery is the *majority* of the thread's
time, the second explanation is at least as well supported by the
proportions as the first.

**Working answer to "genuine work or poor codegen": mixed, and now backed by
a real (if partially confounded) hardware measurement.** 71% of the JIT
bucket is the game's own executable running normal (if audio-telemetry-heavy)
engine code; that part is not a port artifact. The 23% recompile-thrash rate
is a real, measured port-side inefficiency, but it's bounded to a small
absolute cost by how little total time compilation gets in the profile. The
IPC/miss-rate gap is real and large, but the evidence leans toward it
belonging mostly to the crossing/dispatch machinery `frame-cost-budget.md`
already targeted, rather than opening a new front against FEXCore's own
instruction selection. **This is a "the trap round-trip is the compressible
part, not the JIT's codegen" result — spend effort per `frame-cost-budget.md`'s
existing ranked plan, not on a JIT rewrite, unless a follow-up per-sample
PMU breakdown says otherwise.**

---

## 1. Measurement obstacle: JIT symbol naming is a structural no-op for a
   real game process on this port, and why

The brief's premise — set `FEX_LIBRARYJITNAMING=1` or `FEX_BLOCKJITNAMING=1`,
get a populated `/tmp/perf-<pid>.map`, resolve with `perf report` — is true
for a **standalone** FEX-run Linux binary (verified: the appid's own
`steamhelper`, run directly as `FEX .../steamhelper`, gets a normal
87 KB / ~750-entry map with real RootFS DLL filenames). It is **false** for
the actual game process on this port, and I confirmed why by reading source,
not by guessing:

- The game runs through `libfexbridge.so` (`Source/Tools/FexBridge/FexBridge.cpp`),
  which embeds FEXCore **inside the native ppc64 wine process** — there is no
  separate frontend, no Linux-syscall interception, and (per its own comment)
  deliberately no config-file/AppConfig layer, only environment (added
  2026-08-19 for `FEX_X87REDUCEDPRECISION` etc; this generalizes fine, ruling
  out "the env var didn't reach the bridge" as the cause).
- `BridgeSyscallHandler::LookupExecutableFileSection` (`FexBridge.cpp:151`)
  **unconditionally returns `std::nullopt`** — there is no VMA-tracking
  machinery behind it (`QueryGuestExecutableRange`'s own comment says the
  caller — wine — owns the address space, not FEX). `LookupAnonymousExecImageName`
  is never overridden, so it falls to the base class's `return nullptr;`
  (`FEXCore/include/FEXCore/HLE/SyscallHandler.h:79`) — whose *own doc
  comment* already names this exact failure mode: *"Wine loads the MAIN PE
  image this way … without this every sample in a Windows game's own engine
  code profiles as [unknown] — ~90% of a Witcher 3 in-world capture."* That
  is a **previously-diagnosed** gap that was never wired up for the bridge
  path.
- Consequence, verified empirically twice (a 66 s and a 145 s live diagnostic,
  `FEX_LIBRARYJITNAMING=1`): `/tmp/perf-<PID>.map` for the game's own PID
  stays **exactly 0 bytes** for the whole process lifetime, while a
  concurrently-running `steamhelper` process gets a populated map. This is
  not a timing fluke or a warm-JIT "nothing new to name" artifact — it's zero
  bytes from process start to natural exit.
- `FEX_BLOCKJITNAMING=1` **does** produce the ~31,000-entry (in my two runs:
  176,811 and 176,854 *unique* addresses) maps a prior investigation
  reported — but every single entry is `Core.cpp`'s **unnamed fallback**
  (`Symbols::Register(host, GuestRIP, size)` → literal text
  `JIT_0x<GuestRIP>_0x<HostAddr>`), because it hits the exact same
  `LookupExecutableFileSection` nullopt. So: the map *is* real and useful,
  but it never carries a filename — the "31k-entry map with real guest RIPs"
  claim and the "map was empty" observation are **both true**, for two
  different env vars, and neither on its own gives module names.

**The trick that unblocks it without touching FEXCore:** the bridge runs
guest x86 code in the *same address space* as the native ppc64 process (this
is the whole point of an in-process bridge — no address-space virtualization).
So the bare `GuestRIP` baked into every `JIT_0x..._0x...` symbol is a real,
valid address that also appears in that process's own `/proc/<pid>/maps` —
confirmed directly (`0x14014ab30` from a JIT symbol falls inside
`Cyberpunk2077.exe`'s own anonymous `.text` VMA in a maps snapshot taken at
the same moment). Wine loads the main PE "anonymous reserve + copy-in" style
(one small file-backed header page at file offset 0, everything else
anonymous — the same shape FEXCore's own dead `LookupAnonymousExecImageName`
doc comment describes), so a short backward walk over the maps list from an
anonymous VMA to the nearest file-backed, file-offset-0 predecessor recovers
the filename in ~100% of cases in this dataset. I implemented this as a
~60-line offline Python script (`/tmp/attribute_jit.py` on the AC922, copy
below in Artifacts) run against `perf script` output plus a maps snapshot —
**pure post-processing, no FEX rebuild, no perturbation from a source
change.** This is the method result: attribution is possible, it just isn't
what the brief assumed would produce it.

## 2. Attribution of the JIT bucket

Two full instrumented legs (`FEX_BLOCKJITNAMING=1`, `perf record -t <GameThread
tid> -F 999`, timed to land mid-flythrough after re-calibrating for a much
faster warm-shader benchmark than the brief's baseline assumed — see §6).
Leg 2 is the clean capture: 25 s, 22,576 samples, GameThread ~90% on-CPU
during the window, benchmark completed validly (1303 frames, avg 19.69 fps —
completion criteria from the coordinator's stall warning satisfied; see §6).

**Whole-thread bucket split, this window** (renormalizing my own
instrumentation's write() calls out is not needed here — BlockJITNaming has
no steady-state per-frame cost, only a per-*compile* one, see §4):

| bucket | % of GameThread |
|---|---:|
| ntdll (trap/dispatch, `__wine_syscall_dispatcher`, `emu_trap_dispatch`, …) | 30.25% |
| **JIT guest code** | **27.53%** |
| win32u (native peek/tick-count side) | 15.82% |
| libfexbridge machinery (`HandleSyscall`, state sync) | 13.63% |
| libc (mutex, getrusage, memset, TLS) | 11.03% |
| vkd3d/RADV/libvulkan_radeon | 1.25% |
| other (X11, kernel, vdso) | 0.5% |

This is the same shape as the `frame-cost-budget.md` §3 profile (trap
machinery + peek + libc together ~59–70% either way), but my measured JIT
share (27.5%) is **lower** than that report's 37.6% raw / 40.2% renormalized.
I attribute the gap to my own instrumentation biasing time *away* from the
JIT bucket and *into* the compile-machinery bucket (BlockJITNaming's
write-per-compile cost, amplified by the 23% recompile-thrash rate in §4) —
not to a real change in the game's behavior. **Treat 27.5% as a conservative
lower bound; the earlier ~37–40% figure is the better estimate of the clean
JIT share**, and is what §"Headline" arithmetic (14 ms of 34.7 ms ≈ 40%)
still rests on.

**Within the JIT bucket, by guest module** (this split, unlike the raw
percentage above, should be close to instrumentation-independent — it's a
proportion *within* already-JIT'd samples):

| module | % of JIT samples | % of all GameThread samples |
|---|---:|---:|
| **Cyberpunk2077.exe** (the game's own code) | **70.90%** | 19.52% |
| unresolved anonymous (JIT-generated trampolines/stubs with no backing image) | 9.81% | 2.70% |
| kernel32.dll (guest-side wine thunk/relay) | 8.41% | 2.32% |
| user32.dll (guest-side wine thunk/relay — PeekMessage-adjacent) | 8.22% | 2.26% |
| PhysX3_x64.dll + PhysX3Common_x64.dll | 1.10% | 0.30% |
| d3d12.dll (guest-side thunk stubs) | 0.55% | 0.15% |
| REDGalaxy64.dll (CDPR's own overlay/service layer) | 0.35% | 0.10% |
| steamclient64.dll | 0.21% | 0.06% |
| PxFoundation_x64.dll, steam_api64.dll, ws2_32.dll, msvcrt.dll, ntdll.dll | <0.1% each | — |

No `icu*`/`libxess` presence — this benchmark config uses FSR2, not XeSS, so
that's expected, not a gap.

**Answer to "which guest modules dominate": the game's own executable, by a
wide margin (71% of JIT time), with the wine-side x86 thunk/relay DLLs
(kernel32+user32, themselves guest code the port ships, not CDPR's) as the
second-largest chunk at ~16.6% combined.** PhysX and the CDPR service layer
are present but small. This is a real answer, not "spread evenly" — see the
hot-function finding next.

## 3. A named hot spot inside the game's own code (MEASURED)

The top individual JIT addresses cluster hard around two RVAs 0x3e4 bytes
apart in `Cyberpunk2077.exe`: `0x14ab30` (448 samples) and `0x14af14`
(422 samples) — **870 of 22,576 total GameThread samples, 3.85% of the whole
capture, ~14% of the entire JIT bucket, in one place.**

I statically disassembled the actual shipped binary at these RVAs
(`llvm-objdump -d --x86-asm-syntax=intel`, since the host's own `objdump`
doesn't understand PE/COFF — needed `llvm-objdump`, which does, and reads a
genuine embedded COFF symbol table CDPR left in the binary, not a
nearest-export guess: the disassembly's function prologue starts *exactly*
at the sampled address). Both RVAs are inside one function:

```
000000014014a700 <?StartProfileThreadUsage@MemoryMgr@AK@@YAXXZ>
```

— `AK::MemoryMgr::StartProfileThreadUsage`, **Audiokinetic Wwise's own
per-thread audio-memory profiling accessor.** The function does a TLS read
(`gs:[0x58]`), a couple of conditional branches gating on a per-thread flag,
and two calls into a shared "get accessor" helper at `0x14024fc64` /
`0x14024f3d4` — which independently shows up in the same top-20 hottest-address
list (`0x1402f0968`, 341 samples, is inside the same helper region). This is
real conditional/branchy work, not a one-instruction stub — I cannot tell
from static disassembly alone whether it's doing *proportionate* work for how
often it's apparently being called, or whether a profiling/telemetry hook is
firing far more often than a shipping build should exercise it. That
distinction is exactly what an IPC/branch-miss comparison (§5) would settle,
and I don't have it.

**This directly falsifies "spread evenly across the engine with no hot
spot"** for the guest-code portion specifically — there is a hot spot, it has
a name, and it's audio middleware, not rendering or physics.

## 4. Recompile thrash (MEASURED, reproduced twice)

Every `FEX_BLOCKJITNAMING=1` map records one line per **compile event**, not
per guest address — so duplicate `JIT_0x<sameaddr>_...` lines mean FEX
recompiled a RIP it had already translated earlier in the same process.

| leg | total compile events | unique addresses | recompile rate |
|---|---:|---:|---:|
| leg 1 | 229,238 | 176,854 | 22.85% |
| leg 2 | 228,443 | 176,811 | 22.60% |

Reproduced to within 0.25 points across two independent ~140 s sessions —
this is a real, stable property of this run, not noise. One address was
recompiled 28 times. Every one of the top-15 most-recompiled addresses is
inside `Cyberpunk2077.exe`'s own code (`0x140xxxxxxx`), none in a system DLL.
Given the commit history on `power9team` (`SMC: the lazy cross-thread hole is
real…`, `FEX_SMCLAZYLINK`/`FEX_SMCLAZYINVAL` work), this is plausibly
SMC-invalidation behavior — either legitimate (the game genuinely rewrites
code, e.g. hot-patched trampolines) or false-positive (a write near a code
page trips soft-invalidation for code that never actually changed). I did
not chase which; that's a clean, scoped next step for whoever owns
`SyscallsSMCTracking.cpp`.

**Bounding its cost, honestly:** the *previous* (lighter-touch) profile in
`frame-cost-budget.md` already measured `CompileCode` + `ConstrainedRAPass::Run`
+ `Decoder::DecodeInstructionsAtEntry` together at ~1.2% of GameThread. If
23% of all compiles are pure waste, that's ~0.28% of GameThread, or roughly
**0.1 ms of the 34.7 ms floor** — real and worth fixing eventually, but not a
material fraction of the 14 ms on its own. **This is a measured
inefficiency with a small, bounded, measured cost — not a hidden explanation
for the headline number.**

## 5. PMU counters: attempted, blocked (this is the honest gap)

The brief's premise (`perf stat -e` with hardware counters works here,
`perf_event_paranoid=2`, "own processes are fine") did not hold for
**attach-mode** counting:

- `perf stat -t <GameThread tid> -e cycles,instructions,...` against the
  live game: `Error: sys_perf_event_open() ... No such process` (a timing
  race in one attempt) and, when retried carefully, `<not counted>` for
  every event.
- Isolating the variable: `perf stat -p <pid>` against a **trivial
  self-spawned `sleep`** process — same uid, same session, nothing to do
  with FEX or the game — **also** returns `<not counted>` for every event,
  with perf's own hint pointing at `nmi_watchdog`. `nmi_watchdog` is `1` on
  this box and I do not have passwordless `sudo` to clear it
  (`echo 0 > /proc/sys/kernel/nmi_watchdog` needs root).
- Plain **direct-child** counting mode works fine (`perf stat -e cycles,... --
  sleep 0.2` succeeds) — so the restriction is specifically about attaching
  counters to a pid perf did not itself fork, not about the events or the
  paranoid level in general.
- `perf record` **sampling** mode via `-t <tid>` does work for attach (used
  throughout §2/§3, thousands of real samples) — but a multi-event
  group-sample-read recording (`-e '{cycles,instructions,...}:S'`) against
  the guest-local hot-loop probe (`probes/guest/com_crossing_cost.c`, rebuilt
  with `LOCAL_ITERS` bumped to 800M for a ~5 s window) produced **zero
  samples** in three attempts, including a plain single-event `-F 999`
  attempt against the same target. I could not tell, in the time available,
  whether that's the same permission wall in a different guise or a
  timing/attach race against a very short-lived process.

**At the time this section was first written, that's where it stopped: no
counting-mode data at all, for either target, for lack of root.** The owner
subsequently cleared `nmi_watchdog` for one measurement window; §5b below is
the result, including a second, unrelated bug this surfaced (a pid
misidentification, not a permission issue) in my probe-attach attempts
above. `nmi_watchdog` has been restored to its prior value on request; this
section is kept as-is for an accurate record of what did and didn't work
under the default (locked-down) configuration.

## 5b. PMU comparison (MEASURED — done after the owner temporarily cleared
   `nmi_watchdog`)

With `nmi_watchdog=0`, `perf stat` attach-mode counting worked immediately
(verified first against a trivial `yes | head`-style CPU-bound process, to
separate "counting mode is fixed" from "this specific target is fine" — the
earlier probe-attach failures turned out to be a **second, unrelated bug**:
`run-native`'s wrapper chain (`proton` → `wine` → `wineserver` →
the actual guest process) means the pid you get by grepping the launcher's
own command line for the target executable's name is the **wrapper**, not
the worker — its own CPU time stays at 0 the entire run. The real worker
shows up under its own `comm` (literally `pmu_loop.exe`, found via
`pgrep -x`) roughly 17–20 s into a ~30 s total lifetime, the rest being
wine/DLL-load overhead for even this minimal freestanding probe. Once
correctly targeted, both captures came back clean:

| | cycles | instructions | **IPC** | icache-miss rate | branch-miss rate | iTLB-miss (per icache load) |
|---|---:|---:|---:|---:|---:|---:|
| GameThread, real game, mid-flythrough (15 s) | 29.88 B | 18.11 B | **0.606** | **3.633%** | **2.815%** | 0.342% |
| guest-local hot loop, JIT best case (3 s, 800 M-iteration probe) | 11.24 B | 29.47 B | **2.623** | **0.0053%** | **0.0067%** | 0.0004% |

**The gap is large and unambiguous in raw terms: 4.3x lower IPC, 682x higher
icache-miss rate, 421x higher branch-miss rate on the real game thread than
on the JIT's own best-case hot loop.** Taken at face value this is squarely
the coordinator's second scenario — "codegen or code layout is costing us."

**But — and this matters — the comparison is confounded, and I want to say
exactly how before anyone acts on it.** The GameThread capture is a
`perf stat` over the **whole thread**, not JIT-translated code specifically.
Per §2's own sample breakdown, only ~27.5% (conservatively; ~37–40% is my
better estimate — see §2's caveat) of GameThread's samples are actually
inside JIT'd guest code. The rest — ~60–70% — is `__wine_syscall_dispatcher`,
`libfexbridge`'s trap/state-sync machinery, `win32u`'s native peek path, and
libc, none of which the JIT emits: it is hand-written native ppc64 code the
port ships, running dispatch tables and syscall marshaling that jump between
many small, unrelated functions — exactly the shape that stresses an icache
and a branch predictor **regardless of how good FEX's x86-to-ppc64 codegen
is**. I do not have a way to partition the 4.3x/682x/421x gap between "the
JIT translates guest instructions into worse code than it should" and "the
crossing/dispatch machinery this port ships is inherently icache-hostile
because of its own shape" — that would need per-sample PMU attribution
(group-mode `perf record` with `:S`, which I tried and got zero samples
from in the time available; see §5) rather than a single whole-thread count.

**So: the data supports "there is real, large, compressible overhead
sitting on the GameThread, well above what clean JIT'd code needs" with high
confidence. It does NOT cleanly support "FEX's own codegen for translated
x86 is the culprit" over the alternative "the port's crossing/dispatch
machinery is the culprit" — and given that machinery is the *majority* of
GameThread's time while JIT'd code is a minority, the second explanation is
at least as plausible as the first, and arguably favored by the proportions.
That reframes this away from "rewrite the code generator" and back toward
frame-cost-budget.md's own ranked plan (cheapen the generic trap round trip,
`__wine_syscall_dispatcher`, `libfexbridge` state sync) — which this
measurement now gives independent PMU-level support to, rather than opening
a new front in FEXCore's own instruction selection.**

**Confidence, explicitly:** high that GameThread's aggregate IPC/miss-rates
are dramatically worse than clean JIT'd code (that's a direct, unconfounded
measurement of two real things). Low-to-moderate that this specifically
indicts FEXCore's codegen quality rather than this port's crossing-machinery
code shape — that attribution is inferred from the sample-share proportions
in §2, not measured directly, and a per-DSO PMU breakdown (the natural next
step, blocked this session by the same group-sample-read issue in §5) would
settle it properly.

## 6. Verifying every run actually completed (per the coordinator's warning)

An unattended launch was reported elsewhere tonight to stall on the loading
screen (spinning, `wchan: 0`, log frozen), with window-focus loss and a named
D3D12 fence-completion refusal as competing hypotheses. I tested this
directly, hands-off, no clicks, no `xprop`, nothing touching the display,
before trusting any further run:

- One fully hands-off launch (no perf, no naming) reached the benchmark,
  produced `benchmark_2026-08-30_09-33-44`, ran to completion (rc=0,
  1165 frames, 17.20 fps) in ~145 s total.
- Both instrumented legs (§2) and a later clean control (§7) also completed
  cleanly, unattended, rc=0, each in the ~1000–1300 frame range the
  coordinator's ~1222-frame reference implies for this fixed-*duration*
  (not fixed-frame-count) benchmark — frame count scales with achieved fps
  over a ~66–74 s window, so 1029–1303 is the expected range at these
  frame­times, not a sign of truncation.
- I confirmed this by reading `summary.json`'s `frameNumber`/`time` fields
  and checking `frames.csv` line counts every time, exactly as instructed,
  and would have discarded any run that failed either check. None did,
  across five total launches today.

**I did not reproduce the stall.** That doesn't mean it doesn't happen — I
have five data points against the other report's one, on a machine I share
with concurrent Steam-client activity, so absence of a stall in my runs is
weak evidence. **Report as data, not as a
refutation: unattended launches can and did complete cleanly multiple times
today; the stall is real (reported directly, with a plausible named
mechanism) but appears intermittent, not universal.** Anyone continuing this
work should keep gating on `frames.csv`/`summary.json` exactly as done here.

## 7. Floor-drift claim: retracted — it was a units error, not a finding

**This section originally claimed a 2–3x floor regression across today's
runs. That claim was wrong and I am retracting it.** The error: I computed
"floor" as `1000 / minFps` from each run's `summary.json`, and treated that
as the same quantity `frame-cost-budget.md` calls the frametime floor. It
is not. That report's "floor" is the **minimum single-frame frametime in
`frames.csv`** (the fastest frame observed — a lower bound, hence "floor").
`summary.json`'s `minFps` is a different, evidently smoothed/windowed metric
(CDPR's own "worst-case" fps figure, closer to a 1%-low style average than
an instantaneous value) — it does not invert to the floor, and I should have
computed directly from `frames.csv` the way the original report did, rather
than reusing a same-named-sounding field from a different file. Recomputing
`max`/`min`/`median` directly from each run's `frames.csv` (`awk` over the
frametime column):

| run | frames | min (floor) | median | max (worst frame) | avg |
|---|---:|---:|---:|---:|---:|
| baseline `01-06-56` | 1222 | **34.68** | 51.79 | 149.22 | 54.06 |
| TRAP_STATS leg `02-15-48` | 1169 | 35.34 | 55.18 | 157.03 | 57.77 |
| hands-off validation `09-33-44` | 1165 | 35.83 | 55.59 | 172.93 | 58.12 |
| leg 1 `09-39-22` | 1109 | 36.01 | 61.23 | 159.96 | 63.11 |
| leg 2 `09-51-14` (SMT4→SMT2 change) | 1303 | **31.50** | 47.56 | 158.99 | 50.81 |
| same-session clean control `10-06-45` | 1029 | 42.08 | 68.78 | 228.64 | 71.37 |

The floor is **stable**: 31.50–42.08 ms across six runs, i.e. roughly ±20%
of the 34.68 ms baseline in both directions — the fastest floor of the whole
set (31.50 ms) came from one of my *instrumented* legs, and the slowest
(42.08 ms) from the uninstrumented control. That is normal run-to-run noise
at the same order the earlier TRAP_STATS A/B already established (+1.9% on
one specific comparison), not a regression, and not something that tracks
instrumentation, time-of-day, or (for the one run where it's a known
variable) the owner's SMT4→SMT2 switch in any consistent direction.
**Retract in full: do not treat today's floor numbers as untrustworthy —
they agree with the baseline to within normal noise, and every ms figure
elsewhere in this report can be compared against `frame-cost-budget.md`
without the caveat I previously attached to it.**

What *does* move more is the median and the worst-frame tail (median
47.56–68.78 ms, worst frame 149–229 ms) — consistent with ordinary scene-
and load-dependent variance across different camera-path timing, not a
system-wide drift. I have no reason to chase this further; it doesn't change
anything in §1–§6.

## 8. Emulated Proton control: not run, and why

Proton Experimental (`experimental-11.0-20260814b`) is installed and has
prior `compatdata` for appid 1091500, implying it has been launched before —
but via Steam's own GUI/library flow, not a documented headless script the
way `steamtool/run-native` exists for the native lane. I did not find a safe
way to reconstruct that invocation from the CLI within budget without either
guessing at `STEAM_COMPAT_*` plumbing or driving the Steam client UI on a
shared display — the latter risked surfacing dialogs on the owner's live
desktop, which the brief explicitly warns against for a *different* class of
script but which I judged the same risk applies to here. I chose not to
force it. **This also matters less than the brief originally framed it**:
the coordinator correctly narrowed its scope mid-investigation — both lanes
run the *same* FEXCore JIT, so a comparable floor under emulation would only
bound the crossing-tax contribution (how much of the native lane's overhead
is "translation exists at all" vs. "this port's specific bridge"), and would
say nothing about codegen quality, which is what §5's blocked PMU
measurement was actually for. **Recommended if pursued: launch via Steam
(`steam -applaunch 1091500` with the compat tool already set to Proton
Experimental for this appid, launch options carrying
`-skipStartScreen -benchmark`), verify completion the same way as §6, and
report the resulting floor explicitly labeled as a crossing-tax bound, not a
codegen control.**

---

## Ranked theories

1. **[MEASURED, primary] The 14 ms bucket is dominated by the game's own
   code (71% of JIT samples), and that code is not uniform — a named Wwise
   audio-telemetry function is ~14% of all JIT time by itself.** Evidence:
   §2 module split (source: live perf capture + address-space cross-reference,
   reproduced structurally across two legs), §3 disassembly (source: static
   analysis of the actual shipped binary, symbol table intact). Confidence:
   high on the module/function identity; the "is this pathological or normal
   for the engine" question is open (needs IPC, or a native-Windows
   reference profile, neither available here).

2. **[MEASURED, secondary, small] ~23% of JIT compiles are wasted
   recompiles of already-translated code, bounded to roughly 0.1 ms of the
   34.7 ms floor.** Evidence: §4, reproduced to 0.25 points across two
   independent sessions, concentrated entirely in the game's own executable.
   This is real, fixable, and worth doing eventually (likely in
   `SyscallsSMCTracking.cpp`'s invalidation logic) — but it is not, on the
   numbers available, a meaningful fraction of the headline 14 ms.

3. **[INFERRED, unresolved — the actual codegen-quality question] Whether
   the JIT emits materially worse code than it needs to for real game
   logic.** I could not measure this: `perf stat` attach-mode counting is
   blocked system-wide by (apparently) `nmi_watchdog` without root, and my
   attempt to get the same signal via `perf record`'s group sample-read mode
   against a long-running hot-loop probe produced no samples in the time
   available. The one indirect data point I do have — the hot Wwise
   function does real conditional/branchy TLS-accessing work, not a trivial
   no-op — leans slightly toward "genuine work," but this is a weak signal
   from one function's disassembly, not a profile-wide IPC comparison, and
   should be labeled exactly that: a hunch, not a finding.

4. **[REJECTED by this data] "Flat profile, no hot spot, nothing to
   attribute."** False for the JIT-code portion specifically — see §3. (It
   may still be roughly true of the *overall* 34.7 ms floor once crossing
   machinery and peek are folded back in, per `frame-cost-budget.md`'s own
   framing — that report's ranked fix list, which targets PeekMessage and
   the generic trap round-trip rather than the JIT, is not contradicted by
   anything found here.)

5. **[OPEN, deprioritized per scope] Emulated-Proton crossing-tax bound.**
   Not measured (§8). Would only bound one component (crossing tax), not
   settle the codegen question even if run.

## What would most change this picture

(a) is now done (see the added §5b, PMU comparison, run with the owner's
temporarily-cleared `nmi_watchdog`). Remaining, in priority order: (b) a
single gdb-attach expansion-ratio check (host ppc64 instruction count vs.
guest x86 instruction count) on the `StartProfileThreadUsage` block
specifically, now that its address is known exactly (`0x14014ab30`, no
re-discovery needed); (c) attribute the SMC recompile-thrash addresses in
§4 to actual functions (same disassembly technique used in §3) to learn
whether it's legitimate self-modification or a false-positive invalidation
pattern.

## Measured vs. inferred, explicit

**Measured directly, this session:** the JIT-naming no-op on the bridge path
(source read + two live 0-byte-map reproductions); the address-space
cross-reference method's correctness (verified against `/proc/pid/maps`);
the full JIT module-attribution table (§2); the Wwise hot-function identity
and its sample share (§3, static disassembly of the real binary); the
recompile-thrash rate, twice (§4); the `perf stat` attach-mode failure on
both the game and a trivial control process (§5); five clean run completions
against the coordinator's stall criteria (§6); the corrected floor table (§7).

**Inferred / bounded, not directly measured:** the recompile-thrash time
cost (~0.1 ms, derived from a *different* session's CompileCode profile
share, not measured in this session's own instrumented legs); "27.5% JIT
share is a lower bound, ~37–40% is closer to true" (reasoned from
BlockJITNaming's known write-per-compile mechanism, not isolated by a clean same-session A/B, since
BlockJITNaming's own overhead and the JIT share both moved together across
runs rather than being held apart); whether the Wwise hot function's call
frequency is normal for a shipping build.

**Unknown / explicitly not settled:** GameThread IPC, icache-miss rate,
iTLB-miss rate, branch-miss rate — for either the game or the hot-loop
probe; whether the reported stall (outside
this investigation) is focus-loss or the named fence-completion refusal;
attribution of the specific recompiled addresses beyond "inside the game's
own executable."

## Artifacts

AC922 (`jbettcher@192.168.2.24`):
- `/tmp/jitleg2-0830/` — the primary clean capture: `perf-gamethread.data`
  (22,576 samples, 25 s mid-flythrough), `perf-1374488.map[.final]`
  (11 MB, `FEX_BLOCKJITNAMING=1` raw address map), `pid-maps-start.txt` /
  `pid-maps-end.txt` (live `/proc/pid/maps` snapshots), `notes` (full timeline).
- `/tmp/jitleg-0830/` — leg 1 (same method, mistimed near process exit;
  used only for the recompile-thrash cross-check, not the primary module split).
- `/tmp/attribute_jit.py` — the module-attribution script (perf script +
  maps cross-reference with the anonymous-image walk-back).
- `/tmp/comcost/pmu_loop.exe`, `/tmp/comcost/pmu_loop.c` — the
  800M-iteration guest-local-loop probe built for the (blocked) PMU
  comparison; ready to reuse once `nmi_watchdog` is clear.
- `/tmp/cleancheck-0830.log` and `benchmark_2026-08-30_10-06-45/` — the
  same-session uninstrumented control run (§7).
- `/tmp/stallcheck/`, hands-off validation run →
  `benchmark_2026-08-30_09-33-44/` (§6).

Source read (no changes made): `FEXCore/Source/Interface/Core/Core.cpp:1426`,
`FEXCore/Source/Common/JitSymbols.cpp`,
`FEXCore/include/FEXCore/HLE/SyscallHandler.h:79`,
`Source/Tools/FexBridge/FexBridge.cpp:151`,
`Source/Tools/LinuxEmulation/LinuxSyscalls/SyscallsSMCTracking.cpp:1269`
(all in `~/Development/fastppcx86`, branch `power9team`, no commits made).

---

## 9. Instruction-level deep dive: which instructions are executing, and are they the right ones

Requested follow-up: aggregate IPC/miss-rate numbers (§5b) are the symptom;
this section is the mechanism, from `perf annotate` on the existing sampling
captures (no counting mode needed — sampling worked throughout) plus a live
`gdb` attach for JIT'd code. Findings are ranked by how much of the profile
each accounts for, with disassembly evidence for each. Source:
`/tmp/annotate-0830/*.txt` on the AC922 (perf annotate against
`/tmp/jitleg2-0830/perf-gamethread.data`, 22,576 samples), plus a live gdb
dump (`/tmp/gdb-expansion-0830/`).

### 9.1 [RANK 1 — ~6–7% of all GameThread cycles] `__wine_syscall_dispatcher` unconditionally saves the full FP/VMX register file on every trap; its own restore path proves most of that save is never needed

`__wine_syscall_dispatcher` is ~10–11% of GameThread (per the original
`frame-cost-budget.md` profile). Annotating it (2,267 samples) and bucketing
by address range:

| block | % of this function's samples | what it does |
|---|---:|---|
| GPR + special-register save (entry, unconditional) | 24.89% | `std r13..r29` (17 GPRs) to context, plus `mflr`/`mfcr`/`mfxer`/`mfctr` |
| **FP save (entry, unconditional)** | 12.89% | `stfd f14..f31` (18 FPRs) + `mffs`/store FPSCR, one instruction each |
| **VMX save (entry, unconditional)** | 27.76% | `stvx v20..v31` (12 vector registers), manually unrolled with `addi r11,r11,16` between each store |
| **combined unconditional save** | **65.54%** | — |
| FP restore (later in the function, gated by `andi. r0,r14,4; beq ...`) | **0.00%** | `lfd f0..f31` + `mtfsf` — entire block |
| VMX restore (gated by `andi. r0,r14,16; beq ...`) | **0.00%** | `lvx v0..v31` — entire block |

Representative save-block disassembly (the hottest single line in the
function, 11.01%, is actually in the **restore** path reloading CTR —
`ld r16,264(r31); mtctr r16` at `0x6367c` — but the save side is where the
bulk of the time is spent):

```
    2.14 :   63284:  ld      r31,888(r30)      # context pointer
    6.25 :   63288:  std     r1,8(r31)         # save SP
    4.23 :   632a0:  std     r16,128(r31)      # save r16 (unconditional GPR save loop)
    ...
    5.61 :   63368:  stfd    f0,296(r31)       # save FPSCR (after 18x stfd f14..f31 above)
    ...                                         # 12x stvx v20..v31, unrolled:
    2.09 :   63398:  stvx    v25,r31,r11
    2.65 :   633a0:  stvx    v26,r31,r11
    3.11 :   633b8:  stvx    v29,r31,r11
    3.16 :   633c8:  stvx    v31,r31,r11
```

The restore path, later in the same function, is **already flag-gated** —
it reads a dirty-flags word at context offset 304 (zeroed at trap entry,
so it must be *set* by the syscall handler on the far side of the `bctrl`)
and skips the FP/VMX reload blocks entirely unless specific bits are set.
In this 25 s, 22,576-sample capture, **those bits were never observed set**
— the entire FP-restore and VMX-restore blocks carry zero samples.

**This is the concrete, provable "dead state" pattern the coordinator asked
for**: the function's own logic already knows most syscalls don't touch
FP/VMX state (that's what the restore-side gate is *for* — and matches
commits already in this tree, `FexBridge: lazy trap contexts — store
EFLAGS/XMM only on the hop that reads them`), but the **save** side was
never given the same treatment — it unconditionally spills all 18 FPRs +
FPSCR + 12 VMX registers before the dispatcher even knows whether they'll
be needed. Given `__wine_syscall_dispatcher` is ~10–11% of GameThread and
65.5% of its own time is this unconditional save, that block alone is
**roughly 6–7% of total GameThread cycles** — done, empirically, for a
result that is then never read back on the vast majority of syscalls.

**Confidence: high.** This is a direct disassembly + sample-percentage
reading, not an inference, and the "prove it's dead" evidence is the
function's *own* restore-side gate, not an assumption about what the
callee needs.

### 9.2 [RANK 2 — ~0.1–0.15% of all GameThread cycles, but a clean, isolated measurement] `get_tick_count`'s seqlock read costs 20.7% of its own instructions in `hwsync`/`isync` barriers

Static disassembly of `win32u.so`'s `get_tick_count` (29 instructions
total): three repetitions of the identical sequence

```
        hwsync
        lwz     rX,0(rY)      # read one word of the shared tick-count page
        cmpw    rX,rX
        bne-    <self+4>      # never taken; a compiler/ISA idiom pairing cmp+bne- with isync
        isync
```

— a **seqlock-style consistency read** (read low word, high word, and a
version/parity word again, retrying if the shared page changed underneath),
needed because x86 gives this ordering for free and POWER9 does not. **3
`hwsync` + 3 `isync` = 6 of the function's 29 static instructions (20.7%)
are pure ordering overhead**, before the ordinary arithmetic that combines
the words into a tick count even begins. In the *sampled* view (§ original
report), the two `hwsync`s alone carry 3.75%+2.09% of this function's own
weight; the loads immediately after (`lwz`, 57.5%/17.1%/16.3% of the
function's samples) dominate more, consistent with a small, tight function
where almost every instruction gets *some* share — the ordering
instructions are a real, fixed, non-negligible slice, not the majority of
the cost of this specific function.

`get_tick_count` itself is ~3.8% of GameThread and is called from inside
the busy-polling peek path (~248k calls/s per `frame-cost-budget.md` §2,
called repeatedly per second regardless of scene). At that call rate, 6
ordering instructions/call × 247,802 calls/s ≈ **1.49M ordering-instruction
executions/s from this one function alone** — against GameThread's overall
~1.2 billion instructions/s (from §5b's 18.1B instructions / 15s), that's
**≈0.12% of all executed GameThread instructions**, from ordering
overhead in one small, frequently-hit function. Small in the aggregate, but
a clean, fully quantified example of the ordering tax the owner asked about
— [INFERRED: the 0.12% figure combines a measured static count with a
measured call rate from a different capture, not a single direct count].

### 9.3 Ordering-instruction density, more broadly: real, but not close to explaining the IPC gap on its own

| binary | total instructions | `hwsync`+`isync`+`lwsync`+`eieio` | density |
|---|---:|---:|---:|
| `ntdll.so` (whole file) | 162,417 | 99 (48+48+3+0) | 0.061% |
| `libfexbridge.so` (whole file) | 536,272 | 1,157 (184+347+626+0) | 0.216% |
| `__wine_syscall_dispatcher` alone | 328 | 0 (uses one `ldarx`/`stdcx.` pair instead — an atomic counter, not an ordering barrier) | 0% |
| `get_tick_count` alone | 29 | 6 | 20.7% |

**libfexbridge is ~3.5x denser in ordering instructions than ntdll** — this
matches the owner's own framing (this is where the JIT's own TSO/SAO
emulation infrastructure lives, per `FEX_HWTSO`/`PROT_SAO` machinery named
elsewhere in this investigation), not the OS-emulation crossing path.
**Owner's own read holds up**: at these whole-file densities (0.06–0.22%),
ordering instructions cannot be a primary driver of the 4.3x IPC gap or the
400–700x miss-rate gap measured in §5b — they are real, concentrated in a
few specific spots (like `get_tick_count`), and worth trimming where they
sit on a hot, high-frequency path, but they are not "what is churning" in
aggregate. §9.1 and §9.4 below are the bigger, more direct answers to that
question.

### 9.4 [RANK 3 — dominant within one function, causal mechanism NOT resolved — flagged honestly] `HandleSyscall`'s single hottest instruction

`(anonymous namespace)::BridgeSyscallHandler::HandleSyscall` (7.9% of
GameThread from the earlier symbol table) has **76.62% of its own samples
on one instruction**: `lwz r24,100(r1)` at `+0x64`, a stack-relative load.

I initially read this as a stall following a `memset(&C.FltSave, 0xDD,
sizeof(C.FltSave))` a few instructions earlier in the same disassembly
window and reported that reading — **that was wrong, and I want to correct
it explicitly rather than let it stand.** Checking the source
(`FexBridge.cpp:566-575`): that `memset` only runs when both `Lazy` and a
module-global `TrapCtxPoison` are true, and `TrapCtxPoison` defaults to
`false`, set only by an explicit env var this session never set — so that
branch was **provably not taken** in this capture, and the hot load sits at
the convergence point after the (skipped) conditional, not inside anything
poison-related. Retracted.

What I can say with confidence: this is a real, precisely located,
extremely concentrated hotspot — one stack load carries the large majority
of a function that runs on every syscall. What I cannot say with the
tooling available this session is *why*: it could be genuine sample-skid
(POWER9's PMU attributing an interrupt from a preceding call/branch-heavy
sequence a few cycles downstream, a well-known sampling artifact), a
load-hit-store hazard against a nearby recent store to the same cache
line, or something else. Settling it needs either `perf record` with
precise/PEBS-equivalent event support (not confirmed available on this
POWER9 configuration) or a targeted microbenchmark of this exact
instruction sequence in isolation — out of scope for the time available
here. **Reporting the location and the retraction rather than a guess.**

### 9.5 [supporting evidence, same shape as §9.1] `StoreStateToContext` and `call_user_mode_callback` show the identical unconditional-full-save pattern

`StoreStateToContext` (2.5% of GameThread): top samples are
`stxvd2x vs0,r30,r3` repeated at three separate addresses (10.71%, 7.40%,
2.01%), each preceded by `li r3,<constant>` — a **fully unrolled sequence
of individually-addressed VSX register stores**, one hardcoded offset at a
time rather than a loop, matching the pattern already named in §2's
per-crossing cost accounting. `call_user_mode_callback` shows the same
`stvx v24/v26/v27/v28/v29/v30,r9,r11` vector-register-save-with-manual-
`addi`-increment shape as §9.1's VMX block, at similarly concentrated
percentages (top line 6.90%). **This is not an isolated case — "save the
full vector/FP register file on every crossing, unconditionally, via a
manually unrolled store sequence" is a repeated idiom across at least
three separate functions in this port's crossing path** (`__wine_
syscall_dispatcher`, `StoreStateToContext`, `call_user_mode_callback`).
Whether each of these has the same asymmetry as §9.1 (a gated restore that
proves the save mostly unnecessary) I did not check for the latter two in
the time available — §9.1 is the one I verified end-to-end; these two are
reported as the same *pattern*, not independently proven dead.

### 9.6 Expansion ratio (guest x86 → host ppc64 instruction count): not obtained this session

A dedicated live-`gdb` capture was run for this specifically (identify a
hot JIT host address via a fresh `perf record`, cross-reference to its
guest RVA via the `FEX_BLOCKJITNAMING` map, then `gdb -p <pid> -batch
-ex "disassemble/r ..."` on the live process while it still held the code
buffer resident). **The map did not populate this run** — `/tmp/perf-
<pid>.map` stayed absent for the whole capture window, on the same
mechanism that worked twice before (§1/§2), with no code change and no
obvious cause found in the time available; this looks like an intermittent
issue with the naming pipeline itself, not a new architectural finding.
With the process still live, I disassembled two of the run's own hottest
raw host addresses directly via `gdb` anyway (`/tmp/gdb-expansion-0830/`)
and got real, live, translated ppc64 code — including a visible pattern
worth naming even without guest correlation: an XOR + `addco.` (add with
carry, record CR0) sequence immediately followed by two separate stores
(a byte and a doubleword) into what is very likely flag/status materialization
for an ALU op, done unconditionally at the point of translation. But I
could not verify which x86 instruction(s) produced it without the guest
RVA, so I am not presenting an instruction-count ratio or a validated
before/after listing — doing so without that correlation would be a guess
dressed as a measurement, which is exactly what I don't want to hand
back after already retracting one claim this session. **This is the one
item from the request I did not deliver; a repeat with the naming pipeline
confirmed working first (e.g., verify map growth before opening the perf
window, as done successfully in §1's leg 2) would close it in about the
same ~10 minutes each of the other captures took.**

### Summary, ranked by profile share

1. **§9.1, ~6–7% of all GameThread cycles**: `__wine_syscall_dispatcher`'s
   unconditional FP/VMX register-file save on every trap, proven mostly
   unnecessary by its own gated (and, in this workload, never-taken)
   restore path. Highest confidence, most concrete, most actionable finding
   in this whole investigation — a specific function, a specific 65.5%
   sub-block, and internal evidence (the restore-side gate) that most of
   it is dead work.
2. **§9.3/9.2, ~0.06–0.22% static density, ~0.12% of executed instructions
   from one function**: ordering instructions (`hwsync`/`isync`/`lwsync`)
   are real, concentrated in specific spots like `get_tick_count`'s seqlock
   read, denser in the JIT/TSO-emulation code (`libfexbridge.so`) than in
   OS-emulation code (`ntdll.so`) — matching the owner's own framing — but
   at these densities they cannot be a primary driver of the §5b IPC gap.
3. **§9.5, pattern repeated across ≥3 functions, magnitude not fully
   quantified**: the same "unconditional full vector/FP save, unrolled,
   one instruction per register" idiom recurs beyond the dispatcher; worth
   auditing as a family, not a one-off.
4. **§9.4, ~5–6% of GameThread cycles by location, cause unresolved**: a
   single stack load in `HandleSyscall` dominates its function's profile;
   flagged with a retracted wrong hypothesis rather than an unretracted
   wrong one.
5. **§9.6, not obtained**: guest-to-host expansion ratio and a validated
   register-allocation/spill audit of JIT'd code specifically — blocked by
   an intermittent tooling issue this session, not a finding either way.
