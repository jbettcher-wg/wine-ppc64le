# FEX_SMCLAZYLINK log-spam fix and performance measurement

Repo: `~/Development/fastppcx86` on the AC922 (`ssh jbettcher@192.168.2.24`), branch `power9team`.
Commit: **`67c370ffa`** — "PPC64LE JIT: log FEX_SMCLAZYLINK block-linking notice once per process"
**Not pushed** (`git status` shows `power9team` ahead of `origin/power9team` by 1 commit; working tree clean).

## 1. Emission site

`FEXCore/Source/Interface/Core/JIT/PPC64LE/JIT.cpp:2597` (pre-fix), inside the
`PPC64JITCore::PPC64JITCore` constructor (starts at line 2482):

```cpp
} else if (BlockLinkingEnabled && FEXCore::Config::Get_SMCLAZYINVAL() && LazyLinkArmed) {
  LogMan::Msg::IFmt("FEX_SMCLAZYLINK: BlockLinking stays ON under lazy SMC invalidation; "
                    "same-thread drains ride the InterruptFaultPage poke.");
}
```

This is **not** literally inside the block-linking hot loop; `PPC64JITCore`'s
constructor runs exactly once per guest thread, invoked from
`ContextImpl::InitializeCompiler` (`FEXCore/Source/Interface/Core/Core.cpp:694`,
`Thread->CPUBackend = FEXCore::CPU::CreatePPC64JITCore(...)` at line 716),
which itself is called once from `ContextImpl::CreateThread`
(`Core.cpp:725`/742). So the defect is "once per guest-thread creation," not
"once per block link" — but a Windows-engine-style workload under
Wine/FexBridge that churns short-lived worker threads turns that into
effectively the same problem: one log line per thread spawned.

The `fexbridge[4]:` prefix in the reported log comes from
`Source/Tools/FexBridge/FexBridge.cpp:66`, the fallback path in `EmitLog()`:

```cpp
void EmitLog(int Level, const char* Message) {
  if (auto Cb = LogCb.load(std::memory_order_acquire)) {
    Cb(Level, Message);
  } else {
    fprintf(stderr, "fexbridge[%d]: %s\n", Level, Message);
  }
}
```

`stderr` is unbuffered by libc default, so each call is one formatted-write
syscall to wherever `stderr` is redirected — a real per-occurrence cost, not
just visual noise. (Note: this is a *different* handler from the standalone
`FEX`/`FEXInterpreter` binary's `MsgHandler`, which additionally calls
`fsync()` after every line — that path is not what DOOM was using here, since
the observed tag is `fexbridge[N]`, not FEXInterpreter's color-coded
`I <message>` format.)

## 2. Why it fired per "block" and the fix chosen

The message states a **process-wide configuration verdict**
(`FEX_SMCLAZYLINK && FEX_SMCLAZYSCRUB && !SMCSemanticPatch`, i.e.
`LazyLinkArmed`) — it never depends on per-thread state. It reads exactly
like a startup explainer that was never gated to fire only once, and every
new guest thread re-derives and re-logs the identical verdict.

**Fix chosen:** gate it with `std::once_flag` / `std::call_once`, matching
the idiom already used **~60 lines later in the very same constructor**
(`JIT.cpp:2663-2669`), which announces the exit-RIP-width decision with the
same "one process-wide fact, but this constructor runs per thread" framing:

```cpp
// Announce the decision once per process (this constructor runs per guest
// thread). ...
{
  static std::once_flag Announce;
  std::call_once(Announce, [this]() { LogMan::Msg::IFmt(...); });
}
```

I used the identical pattern for the SMCLAZYLINK message:

```cpp
} else if (BlockLinkingEnabled && FEXCore::Config::Get_SMCLAZYINVAL() && LazyLinkArmed) {
  static std::once_flag SMCLazyLinkAnnounce;
  std::call_once(SMCLazyLinkAnnounce, [] {
    LogMan::Msg::IFmt("FEX_SMCLAZYLINK: BlockLinking stays ON under lazy SMC invalidation; "
                      "same-thread drains ride the InterruptFaultPage poke.");
  });
}
```

**Why this over the alternatives:**
- *Log-level gate*: rejected. This message is already at `IFmt` (INFO), the
  same level as dozens of other startup/decision notices in this file
  (`MSG_LEVEL` is a compile-time `constexpr = INFO`, so there's no coarser
  runtime verbosity knob to hide it behind without also silencing everything
  else at INFO — it wouldn't fix the "per-thread repeat" defect anyway, since
  a raised threshold would suppress it unconditionally rather than making it
  discoverable-once).
- *A bare `static bool` flag* (the `WARN_ONCE_FMT` macro pattern in
  `LogManager.h:149`) is close but is a `DFmt`(DEBUG)-only, non-thread-safe
  macro (fine for its single-threaded use site); this constructor runs
  concurrently from many guest threads, so a plain non-atomic bool has a
  benign-but-untidy race (harmless here — worst case a couple of duplicate
  lines — but `std::once_flag` is already the established, race-free idiom
  in this exact file for this exact situation).
- Matching the sibling `Announce` block keeps the file internally consistent:
  a future reader who understands one understands the other.

I left the neighboring `if` branch's message ("BlockLinking disabled:
incompatible with FEX_SMCSEMANTICPATCH/FEX_SMCLAZYINVAL...", `JIT.cpp:2592`)
untouched — it has the identical latent defect (same constructor, same
per-thread firing) but wasn't the one reported, and touching it wasn't asked
for. Worth a follow-up if it's ever observed doing the same thing.

The information is **still discoverable**: `FEX_SILENTLOG=0
FEX_OUTPUTLOG=stderr <run>` now prints the line exactly once per process,
still at INFO level, still verbatim — someone diagnosing "why is
BlockLinking on/off under lazy SMC" finds it exactly as before, just not
27,000 times.

## 3. Performance measurement

### Setup

Cross-compiled a small x86-64 static test guest with `clang -target
x86_64-linux-gnu --sysroot=$FEX_ROOTFS -fuse-ld=lld` (per this repo's
documented cross-toolchain trick) that repeatedly does `pthread_create` +
trivial work + `pthread_join` in a loop — each `pthread_create` triggers
`ContextImpl::CreateThread` → `InitializeCompiler` →
`CreatePPC64JITCore`, i.e. exactly the code path that fires the log line.
Ran under `FEX_SMCCHECKS=mtrack FEX_SMCSOFTINVALIDATE=1 FEX_SMCLAZYINVAL=1
FEX_SMCLAZYSCRUB=1 FEX_SMCLAZYLINK=1 FEX_SMCFILEIMMUTABLE=1` (this repo's own
`Recipes.cpp` SMC-lazy recipe) plus `FEX_SILENTLOG=0 FEX_OUTPUTLOG=stderr` to
force the logging on, with the required `FEX_APP_DATA_LOCATION` /
`FEX_ROOTFS` / thunk env vars per the task's constraints.

Built two `Bin/FEX` binaries from the same tree via `git stash` /
`git stash pop` around one line, so the *only* difference between "buggy"
and "fixed" is this one log call:
- **buggy**: pre-fix, one `FEX_SMCLAZYLINK: BlockLinking stays ON...` line
  per thread created.
- **fixed**: post-fix (this commit), one line total per process.

### Measurement A — thread-churn loop, timed inside the guest (excludes FEX/thunk startup)

6,000 `pthread_create`/`join` cycles (6,001 `PPC64JITCore` constructions
including main), 8 alternating repeats per binary, timed with
`clock_gettime(CLOCK_MONOTONIC)` bracketing only the loop:

| | mean loop time | stdev | n |
|---|---|---|---|
| fixed | 6.687 s | 0.225 s | 8 |
| buggy | 6.735 s | 0.137 s | 8 |

Difference: **+47 ms over 6,001 lines (buggy slower)**, i.e. ~7.9 µs/line —
but the standard deviation on both sides (0.14–0.22 s) is **4–5x larger than
the mean difference**. This machine had other work running during the
measurement (per the task's own constraint), and one rep on each side (rep 8,
both binaries) jumped ~0.5s above its own mean simultaneously, pointing at a
shared external cause (scheduler/thermal/another job), not our change. At
this signal-to-noise ratio, **the end-to-end thread-churn wall-clock number
is directionally consistent with a real cost but not statistically
distinguishable from zero** on this shared, busy box.

Confirmed independently via line/byte counts on the same runs: buggy
produced exactly 6,001 `FEX_SMCLAZYLINK` lines / 739,007 bytes of stderr per
run; fixed produced exactly 1 line / 1,007 bytes — so the *mechanism*
(per-thread vs. per-process) is unambiguous even though the wall-clock delta
is noisy.

### Measurement B — isolated write-path cost (low-noise, native ppc64le, no FEX/JIT/thread overhead)

To get a clean number for "cost of one suppressed occurrence" without
JIT/scheduler noise, I wrote a native ppc64le C program that does exactly what
`FexBridge.cpp:66`'s fallback does — `fprintf(stderr, "fexbridge[%d]: %s\n",
4, <the exact message text>)` — in a tight loop, `stderr` redirected to a
real file on the AC922's disk-backed root filesystem (btrfs on NVMe, *not*
tmpfs — verified with `mount`/`df -T`).

Ran **27,164 iterations** (matching the exact occurrence count from the
earlier real DOOM run cited in the task), 5 repeats:

```
n=27164 loop_seconds=0.150871  per_call_us=5.554
n=27164 loop_seconds=0.157646  per_call_us=5.803
n=27164 loop_seconds=0.159231  per_call_us=5.862
n=27164 loop_seconds=0.159540  per_call_us=5.873
n=27164 loop_seconds=0.159641  per_call_us=5.877
```

Mean ≈ **157 ms total, ≈5.8 µs/occurrence**, tight variance (first-run cache
warm-up aside). Each run also produced a **3,667,140-byte** log file — this
lines up almost exactly with the task's reported "3.5 MB log," which is good
independent corroboration that this message and this write path are in fact
what dominated the real DOOM log.

### Putting the two together, honestly

- Isolated write-path cost: **~5.8 µs/occurrence** (low noise, real disk,
  exact real-world occurrence count and message).
- In-FEX thread-churn delta: **~7.9 µs/occurrence** (same order of magnitude,
  but statistically noisy — could be anywhere from ~0 to a few tens of µs
  given the observed variance).
- Extrapolating the isolated (more trustworthy) number to the actual incident
  (27,164 occurrences over ~2 minutes): **≈157 ms total**, i.e. **≈0.13% of
  a 120-second run**. Extrapolating the noisier in-FEX number instead gives
  ≈215 ms, same order, same conclusion.

**Conclusion: measured, not assumed — and the claim "this costs frame rate"
does not hold up.** The formatted-write-per-thread defect is real (confirmed
mechanism, confirmed byte-for-byte match to the reported 3.5 MB), and the
fix is correct and necessary for log hygiene and disk usage. But the
*performance* cost of the writes themselves, at the actual observed
occurrence rate, is on the order of one to two hundred milliseconds spread
over a two-minute run — roughly a tenth of one percent of wall time. That is
not a plausible explanation for a frame-rate problem by itself.

**One caveat I can't close without more information:** this measures pure
write-syscall + formatting cost to a real disk file. It does **not** rule out
a *different* mechanism mattering more — e.g. if in the real DOOM session
`CreateThread`/`PPC64JITCore` construction (and hence this log call) happens
on a thread that is itself on the per-frame critical path (a main-loop thread
spawning worker threads every frame, common in some engines' job systems),
then even a few-microsecond addition per spawn, landing inside a
16.6ms/8.3ms frame budget many times a frame, could matter more than the
raw two-minute aggregate suggests. I did not have DOOM's actual threading
pattern to test against (per the task's limits: no game launches), so I'm
reporting the aggregate/isolated numbers as measured and flagging this
frame-alignment question as **unmeasured** rather than guessing at it.

## 4. Rebuild confirmation

Rebuilt `~/Development/fastppcx86/build` with `ninja -j96 FEX` after the fix
(clean build, only pre-existing unrelated warnings: `nodiscard` on
`GetEmptyCodeBuffer()`, unused `LUDIV`/`LDIV`). Verified with the same
`FEX_SILENTLOG=0 FEX_OUTPUTLOG=stderr` + SMC-lazy env combo:
- 20 threads → exactly 1 `FEX_SMCLAZYLINK: BlockLinking stays ON...` line.
- 3,000 threads → exactly 1 line.
- 6,000 threads → exactly 1 line.

Message text, level, and content are unchanged — only the firing frequency
changed, from once-per-thread to once-per-process.

**binfmt note**: per the task's known limitation, a rebuilt `FEX` is not
picked up automatically by binfmt (the `F` flag pins the old interpreter
inode) without `sudo systemctl restart systemd-binfmt`, which I did not run.
All verification above invoked `~/Development/fastppcx86/build/Bin/FEX`
directly against the cross-compiled guest test binaries, sidestepping
binfmt entirely — this doesn't affect the validity of the fix or the
measurement, but it does mean binfmt-launched processes (e.g. double-clicking
a `.exe`) won't see this fix until that restart happens.

## Files touched

- `FEXCore/Source/Interface/Core/JIT/PPC64LE/JIT.cpp` (the fix; committed)

## Scratch/benchmark artifacts

All benchmark source (`threadspam.c`, `threadspam2.c`, `logcost2.c`) and
run scripts lived under `~/smclazylink-bench` on the AC922, outside the git
repo. They were cleaned up after the measurement (not committed, not left
behind) since they have no lasting value beyond this investigation; the
methodology above is reproducible from the commands documented here if
someone wants to re-run it.

## Commit

```
67c370ffa PPC64LE JIT: log FEX_SMCLAZYLINK block-linking notice once per process
```

On branch `power9team`, 1 commit ahead of `origin/power9team`.
**Not pushed.** Working tree clean (`git status` confirms).
