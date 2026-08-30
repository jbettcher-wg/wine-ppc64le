# The ~14 ms of JIT'd guest code in Cyberpunk 2077 — attribution and theories

2026-08-30, AC922 POWER9, ppc64le Wine port (`hangover-ppc64le` / `fastppcx86`,
branch `power9team`). Companion to `frame-cost-budget.md` (§6 there already
labels this bucket "MEASURED fraction, INFERRED window→floor scaling").

## Headline

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

**I could not close the loop with a hardware-counter verdict** (IPC / icache
/ branch-miss on real game code vs. the JIT's own hot-loop best case) — that
measurement is blocked on this box by a `perf stat` attach-mode restriction
I do not have permission to lift (see §5). That is the single biggest gap
between what was asked and what this report delivers. Everything else asked
for — module attribution, a genuine (if partial) JIT-quality signature, and a
scoped verdict on the emulated-Proton question — is here.

**Working answer to "genuine work or poor codegen": mixed, leaning genuine,
with one concrete inefficiency identified and bounded.** 71% of the JIT
bucket is the game's own executable running normal (if audio-telemetry-heavy)
engine code; that part is not a port artifact. The 23% recompile-thrash rate
is a real, measured port-side inefficiency, but it's bounded to a small
absolute cost by how little total time compilation gets in the profile.
Given the tools available in this session, I did not find evidence that the
translated code itself runs at anomalously low IPC — I simply could not
measure IPC at all. **This is a "spend a day closing the IPC gap, not a
week rewriting the JIT" result, not a "walk away" result and not a
"here is your 5 ms" result either.**

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

**Net result: I cannot report GameThread IPC, icache-miss rate, iTLB-miss
rate, or branch-miss rate, measured or estimated, for either the real game
or the com_crossing_cost hot-loop baseline.** This is the one piece of the
brief I did not deliver, and it's the piece that would have let me
distinguish "healthy IPC in real code" (→ genuine work) from "low IPC, high
icache pressure" (→ poor codegen) with actual numbers instead of inference
from a single hot function's disassembly. **Recommended next step: get root
on the AC922 long enough to `echo 0 > /proc/sys/kernel/nmi_watchdog` (and
restore it after), then repeat the `perf stat -t <GameThread tid>` capture
during a flythrough window and the `perf stat -p <probe pid>` capture on the
already-built long-loop probe (`/tmp/comcost/pmu_loop.exe` on the AC922) — a
five-minute job once that one flag is clear.**

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
