# Dex pathological slowness: diagnosis

## Headline answer

**The render thread (`UnityGfxDeviceW`) is not spin-waiting on a flag and not
churning Mono-JIT/SMC translation. It is stuck almost entirely inside one small
memcpy-shaped loop in the NATIVE ppc64 build of `ucrtbase.dll`** (the
"hangover"-style system DLL that runs as real PPC64 machine code, not
FEX-emulated x86). Two independent DWARF-unwound profiles, taken minutes apart
in two separate process launches, both show **~97% of that thread's sampled
cycles in the same ~40-byte loop with no resolvable caller** — inconsistent
with a legitimate one-time bulk copy (it would have to be moving an
implausible amount of memory to occupy a thread that long) and consistent with
either a runaway/garbage copy length or a caller-side retry that never
terminates.

Meanwhile the **main thread's ~97% CPU is genuinely spread across dozens of
distinct, individually-named Mono/guest JIT blocks** — it is doing real (if
slow) computation, not spinning on one address. And the **JIT/SMC side of the
port is bounded, not a storm**: one code-buffer discard-and-regrow in the
first 2 seconds, ~31,000 blocks compiled by ~90 seconds, then completely flat
for the remaining 8+ minutes of the run, confirmed twice.

**Classification: split.** Main thread = **(a)**, genuinely slow (if bounded)
emulated computation. Render thread = **(c)/(b)-shaped port defect**: real
native PPC64 code stuck in a loop that should not run this long, most likely
because of a bad/garbage length or an unproductive retry — but I could not
read the live count register or unwind the caller (see "What I could not do"),
so I cannot yet name the exact missing signal. This is **not** the Mono
SMC-storm hypothesis — that was directly measured and refuted (see below).

## Evidence

### 1. Rebuilt FEX to get current JIT-symbol code, launched Dex fresh

- fastppcx86 HEAD (`67c370ffa`, branch `power9team`) was one commit ahead of
  the last `build/Bin/FEX` build; rebuilt with `ninja -j176 FEX` (17.5s) so the
  write-through JIT-symbol fix (`bad112e7a`) and everything else is live.
- Env var spellings confirmed from `Config.json.in` / `ConfigValues.inl`:
  `FEX_BLOCKJITNAMING=1` (per-block, what I used), `FEX_LIBRARYJITNAMING=1`,
  `FEX_GLOBALJITNAMING=1`, `FEX_GDBSYMBOLS=1`. `FEX_BLOCKJITNAMING=1` writes
  `/tmp/perf-<pid>.map` in the standard `perf` map format
  (`FEXCore/Source/Common/JitSymbols.cpp`), naming every compiled guest block
  `JIT_0x<guestRIP>_<hostAddr>`.
- Launched Dex twice (pid 1251408, then pid 1253839 after the first was
  externally SIGKILLed — not by me; see "Process was killed twice, not by me"
  below) with `FEX_BLOCKJITNAMING=1` and, on the second run,
  `FEX_BUFSTATS=<file>` added.

### 2. The JIT/SMC side converges — it is not a storm

- `/tmp/perf-<pid>.map` (one line per compiled guest block) reached **30,956
  / 30,901 entries within the first ~90 seconds** of each run and then **did
  not grow again** for the rest of an 8+ minute run (checked twice, ~25s
  apart, identical counts both times).
- `FEX_SMC_AUDIT=<file>` (Core.cpp's compile-side tracker,
  `SMCAuditCompileFD`) logged **33,441 `compile rip=... page=...` lines**
  (one per compiled block/page-registration — matches the block count above)
  plus 46 `guest-mmap`/`guest-mprotect` housekeeping lines, and **zero**
  `fault addr=...` lines. The fault-path audit macro
  (`SyscallsSMCTracking.cpp`'s `SMCAuditFD`) lives in
  `Source/Tools/LinuxEmulation`, which — see §4 — is not linked into the
  bridge that runs Dex, so it cannot itself prove zero real SMC faults; but
  the flat, unchanging `perf-<pid>.map` is enough on its own, since a real
  invalidate→recompile cycle would necessarily register a new/changed JIT
  symbol and would show up as map growth. It never did.
- `FEX_BUFSTATS=<file>` recorded exactly **one** code-buffer event for the
  whole run: `codebuffer grow: 16 MiB -> 32 MiB, discarding 15725232 bytes /
  49075 blocks` at **+1.917s**. One discard, not a "steady-state rotation."
- **Conclusion: this directly refutes "continuous Mono JIT/SMC re-translation
  that never converges."** There is a real, measurable, front-loaded
  JIT/SMC cost (tens of thousands of compiles and one 15.7 MB buffer discard
  in under two seconds is not free), but it is bounded and it does converge —
  the game spends the following 8+ minutes with SMC/JIT activity completely
  flat while still pegged at ~97-138% CPU. Whatever it's stuck on afterward,
  it isn't retranslation.

### 3. The two original hot addresses resolve to a native ucrtbase.dll copy loop, not guest code

perf (`perf record -F 999`, cross-checked with a DWARF call-graph capture
scoped to just the `UnityGfxDeviceW` tid) reproduces the original brief's
finding almost exactly, and pins it down further:

```
    56.34% UnityGfxDeviceW  [JIT] tid <pid>  [.] 0x00003fffff912eb8
    37.21% UnityGfxDeviceW  [JIT] tid <pid>  [.] 0x00003fffff912ecc
     3.19% UnityGfxDeviceW  [JIT] tid <pid>  [.] 0x00003fffff912eb0
```
(scoped purely to that one thread over a fresh 5s DWARF-unwound capture:
~97% of its samples, **zero resolvable call-graph parents** — the unwinder
could not find a caller, meaning the thread had been inside this loop for the
entire capture window, not making brief repeated visits).

These addresses are **identical, byte-for-byte, across two independent
process launches** (pid 1251408 and pid 1253839) — not something that
happens to guest/Mono JIT blocks (their host addresses do shift between runs,
e.g. `JIT_0x7bde3060_0x101d3d54dc0` vs `JIT_0x7bde3060_0x101d245a420` for the
same guest RIP in the two runs). A stable address across relaunches plus
"not resolved by `FEX_BLOCKJITNAMING`" both point away from guest/Mono code
and toward something fixed in the port itself.

`/proc/<pid>/maps` places the address inside an anonymous r-xp region
(`3fffff841000-3fffff99c000`, size `0x15b000`) sandwiched exactly between the
header pages of `ucrtbase.dll` and `d3d11.dll`
(`wine-build/dlls/ucrtbase/ppc64-windows/ucrtbase.dll`) — this port's
"hangover"-style scheme of building select system DLLs as **native PPC64 PE**
code instead of x86-to-emulate (`FEXCore::SHMStats::AppType::WIN_ARM64EC` is
the same idea by name). Manually parsing that DLL's PE section table
confirmed `.text` is `VA 0x1000, size 0x15b000` — an exact match for the
mapping — so the hot RVA is `0xd2eb8`, file offset `0xd22b8`.

`ptrace_scope=1` and no root blocked `gdb -p`/`/proc/<pid>/mem` (not the
parent process, so attach is refused), so I disassembled the **on-disk DLL
bytes** at that file offset instead (`objdump -D -b binary -m
powerpc:common64 -EL`, since the file's custom PE machine ID `0x1f3` isn't
recognized as a container format by objdump):

```
3fffff912eb0:  ld      r9,8(r9)
3fffff912eb4:  addi    r8,r8,16
3fffff912eb8:  std     r9,8(r7)      <- hottest sample (10-11% / 56% scoped)
3fffff912ebc:  ld      r6,8(r10)
3fffff912ec0:  addi    r9,r10,8
3fffff912ec4:  addi    r7,r8,8
3fffff912ec8:  addi    r10,r10,16
3fffff912ecc:  std     r6,8(r8)      <- 2nd hottest (7-8% / 37% scoped)
3fffff912ed0:  bdnz    0x3fffff912eb0
```

with the preamble just above it computing `ctr = ((count-1)>>1)+1` from an
odd/even split of a length argument (`andi. r7,r9,1`, `srdi`, `mtctr`). This
is textbook compiler-generated **memcpy/memmove** codegen: an
`mtctr`+`bdnz`-counted loop copying two 8-byte doublewords per iteration.

### 4. fastppcx86's own Mono/SMC mitigation system is dead code in this deployment (real, but not today's bottleneck)

While chasing the owner's SMC-storm hypothesis I found that fastppcx86
already has a full Mono-specific SMC mitigation system — `mono.dll`/
`libmono*.so` detection (`IsMonoRuntimeLibraryPath`,
`MaybeDetectMonoFromPath`, git commit `a0435bdb2`, explicitly about Dex), a
backpatcher write-hook (`DetectMonoBackpatcherBlock` /
`MonoBackpatcherWrite`), and live per-thread counters
(`AccumulatedSMCCount` etc. in `FEXCore/include/FEXCore/Utils/SHMStats.h`).

**All of the detector code lives in
`Source/Tools/LinuxEmulation/LinuxSyscalls/{Syscalls,SyscallsSMCTracking}.cpp`**,
which is FEXCore's standalone-Linux-process front end. `nm -C` on the actual
`libfexbridge.so` loaded by Dex's Wine process confirms **none of
`IsMonoRuntimeLibraryPath`, `MaybeDetectMonoFromPath`, `MaybeRecordMonoMapping`,
`DetectMonoBackpatcherBlock`, `ArmMonoFallbackRange`** are linked in — only
the FEXCore-core "consumer" half (`MarkMonoBackpatcherBlock`, the
`MonoBackpatcherWrite` IR op) exists in the bridge. `AreMonoHacksActive()`
(`Context.h:612`) is `Config.MonoHacks && MonoDetected`, and nothing in the
bridge can ever set `MonoDetected` — so it is permanently false for every
Wine-hosted Mono/Unity title, Dex included. Confirmed further by there being
no `/dev/shm/fex-*-stats` segment at all for Dex's pid (that shared-memory
stats mechanism is also only wired up in `LinuxEmulation`/`ThreadManager.cpp`).

This is a real, verifiable gap (the mitigation the team already built for
exactly this class of game cannot fire under the Wine bridge), but per §2 it
is **not what is causing the current hang** — there is no live SMC storm for
it to have prevented in this run.

## Process was killed twice, not by me

Both launches ended with `proton: line 909: <pid> Killed` (rc=137, SIGKILL)
roughly 7-9 minutes in. I did not send this signal — no `kill`/`pkill` was run
from this session, `dmesg` shows no OOM-killer activity, and `free -h` showed
346 GiB free at the time. Something external (plausibly the owner or a
desktop "not responding" policy, since `DISPLAY=:1` is a live session) is
terminating these hung runs. Flagging this since a repeat SIGKILL mid-run is
exactly the failure mode the task asked me to avoid causing myself.

## UPDATE: root cause named, with `ptrace_scope=0`

`kernel.yama.ptrace_scope` is now 0. Relaunched Dex (pid 1271606), waited for
`UnityGfxDeviceW` (tid 1271658) to become hot, then sampled it repeatedly with
`gdb -p 1271606 --nx -batch` (a Python script that finds the thread by LWP,
switches to it, and reads `$pc`/`$ctr`/`$lr`/`$r1`/`$r3`/`$r4`/`$r5`, then
manually walks the PPC64 ELFv2 back-chain twice: `calleeOfThunk =
*(*(r1)+16)`, `callerOfCaller = *(*(*(r1))+16)`) — 30 rapid attach/detach
cycles, 8 of which landed inside the loop.

### 1. CTR (remaining iteration count)

Eight in-loop samples, `ctr` values: **141327, 217118, 34967, 236822, 85800,
60376, 43346, 196720** — scattered across nearly the whole possible range for
a loop whose `mtctr` seed is `(length/16)`. With `r5` (length, see below)
fixed at exactly `0x400000` (4 MiB), a full `ctr` is `0x400000/16 = 262144`.
The samples span from near-complete (34967 left) to near-the-start (236822
left), taken ~1-2s apart over a ~20s window.

**This settles question 3 directly: it is not one enormous copy.** A single
4 MiB copy at any plausible throughput finishes in well under a second; eight
samples spread ~1-2s apart landing all over the 0-262144 range means the
thread completed **many separate 4 MiB copies** in that window and is
re-entering the same call site over and over, back-to-back.

### 2. The caller — a full 3-level chain, identical every single time

`r1` (stack pointer) was byte-identical (`0x10194e0f0`) across all 8 hits, and
so were both levels of caller resolved by walking the back-chain:

```
0x3fffffa11118  <- d3d11.dll (RVA 0x51118, file offset 0x50518)   [outermost, see below]
      calls
0x3fffff9fad4c  <- d3d11.dll (RVA 0x3ad4c, file offset 0x3a14c)   [thin wrapper]
      calls (via a save-TOC/bctrl import-thunk at ucrtbase RVA 0xddb04)
0x3fffff91db04  <- ucrtbase.dll import thunk (RVA 0xddb04)
      calls
0x3fffff912eb0-0x3fffff912ecc  <- ucrtbase.dll memcpy loop (RVA 0xd2eb8)
```

Disassembling the outer site (`objdump -D -b binary -m powerpc:common64 -EL`
on the raw on-disk `d3d11.dll` bytes at the computed file offsets, since the
custom PE machine ID `0x1f3` isn't a container `objdump` recognizes) shows the
**instruction immediately before the `0x3fffffa11118` return address is a COM
vtable call**: a byte fetched from a table (`lbzx r9,r10,r9`, with a `0xFF`
sentinel check), scaled by 16 and added to a vtable base loaded through the
TOC, then `ld r12,8(r9); mtctr r12; bctrl` — i.e. `object->lpVtbl->Method(...)`
dispatched **through a byte-indexed table of small handler routines**, with
`r3` = a preserved "this" pointer (`ld r3,8(r27)`) and `r4`/`r5` = loop state
carried in preserved registers (`mr r4,r30` / `mr r5,r31`) across iterations.
That shape — a per-format-code dispatch table calling small per-entry
handlers that in turn `memcpy` a fixed block — is exactly how Wine's
`wined3d`/`d3d11` CPU-side surface/texture format-conversion ("blit") code is
structured. I do not have symbols and did not have budget to build a matched
non-stripped ppc64 object to confirm the exact function name; this is a
static-disassembly inference from the instruction shape, not a symbol lookup.

### 3. The operands — fixed length, fixed source, ping-ponging destination

Across all 8 in-loop hits: **`r5` (length) is exactly `0x400000` (4 MiB)
every single time** — not a garbage or absurd value, ruling out "runaway
length." **`r4` (source) is exactly `0x184c0000` every single time** — a
low, 32-bit-range address consistent with a guest-side buffer (Dex's own
module maps sit in the same `0x00400000-0x01032000` neighborhood). **`r3`
(destination) alternates between exactly two values, `0x3ff1c6bfe000` and
`0x3ff1c6ffe000`, which differ by precisely `0x400000`** — i.e. one 4 MiB
buffer's length apart, the signature of a double-buffered internal
destination.

### Conclusion, updated

**Named mechanism:** the `UnityGfxDeviceW` thread spends ~97% of its time
calling into a COM-vtable-dispatched routine inside Wine's native-ppc64
`d3d11.dll`, which `memcpy`s a fixed, unchanging 4 MiB source buffer into one
of two alternating 4 MiB destination buffers — back-to-back, continuously,
through the identical static call chain every time (same stack frame, same
three return addresses, same operands modulo the destination ping-pong). This
is not Mono, not guest/x86 JIT code, not FEX's SMC machinery, and not a
"runaway length" bug — the length is fixed and sane. It is a **genuine,
named emulation/port defect**: something in Wine's ppc64-native D3D11/wined3d
path is re-doing a fixed 4 MiB buffer copy on every call with nothing
observed to gate or pace it, exactly the "spin on real work instead of a
no-op" shape the task asked me to distinguish from a true idle spin.

**Classification, final: (c).** This is a port-side defect in the native
(non-emulated) D3D11 implementation, not (a) genuinely-slow guest computation
and not the SMC-storm/Mono hypothesis (independently refuted in §2 above).

**What would still need doing (out of scope here, diagnosis only):** identify
the actual wined3d/d3d11 source function at RVA `0x51118`/`0x3ad4c` (needs a
non-stripped/matching build or manual data-flow beyond what static
objdump-on-raw-bytes gives), and find what should be gating this copy
(dirty-flag check, fence wait, or frame-rate pacing) but isn't. I did not
attempt any fix, environment-variable change, or Mono substitution, per the
task's instructions, and did not verify anything on-screen — everything above
is profiler/register/disassembly evidence gathered against a live,
un-modified process (pid 1271606, confirmed still running throughout, never
SIGKILLed by me).

## Key paths

- `/home/jbettcher/Development/fastppcx86/FEXCore/Source/Common/JitSymbols.cpp`,
  `.../Source/Interface/Core/Core.cpp` (JIT symbol naming, SMC audit compile side)
- `/home/jbettcher/Development/fastppcx86/Source/Tools/LinuxEmulation/LinuxSyscalls/{Syscalls,SyscallsSMCTracking}.cpp`
  (Mono detection/backpatcher system, confirmed unlinked from the bridge)
- `/home/jbettcher/Development/fastppcx86/FEXCore/include/FEXCore/Utils/SHMStats.h`
- `/home/jbettcher/Development/powerpc64le-ports/hangover-ppc64le/wine-build/dlls/ucrtbase/ppc64-windows/ucrtbase.dll`
  (hot loop, RVA `0xd2eb8`)
- `/tmp/dex-diag/smc-audit.log`, `/tmp/dex-diag2/bufstats.log`,
  `/tmp/dex-diag/perf.data`, `/tmp/dex-diag2/perf.data`,
  `/tmp/dex-diag2/perf-gfx.data` (raw captures on the AC922, left in place)
