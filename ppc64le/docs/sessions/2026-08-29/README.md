# Session records, 2026-08-29 / 08-30

Investigation reports from a single long session. They are raw working
documents, not polished docs: each one states what was MEASURED, with dates,
and several of them exist mainly to record a premise that turned out to be
false. That is the point — the expensive mistakes this day were all cases of
acting on a plausible belief nobody had checked.

Read the summary below first; the individual reports are long.

## The findings that changed what we do

**`frame-cost-budget.md` — where Cyberpunk's 34.7 ms frame floor goes.**
The single most useful document here. Measured, not inferred:
~14 ms the game's own JIT'd guest code, ~12 ms a `PeekMessageW` storm
(247,802/s), ~4-5 ms crossing machinery, ~3 ms libc, and **0.3 ms vkd3d+RADV**
(0.9%). Crossing costs: flat 544 ns, COM trap+dispatch 793 ns, const-cached
6.5 ns, in-game effective ~1.0-1.2 us. 54,800 crossings per frame.
Two premises died here: that vkd3d translation was expensive (it is 0.9%), and
that the COM bridge was a much heavier mechanism than a flat crossing (it is
~1.5x, not 10x).

**`jit-cost-attribution.md` — inside that 14 ms.**
71% is `Cyberpunk2077.exe` itself, ~17% is Wine's own guest-side thunk DLLs.
Named hot spot: two addresses inside **Wwise's `AK::MemoryMgr::StartProfileThreadUsage`**
account for ~14% of the whole JIT bucket — an audio-memory *profiler*, in a
shipping build, costing roughly 2 ms/frame. Also records that
`FEX_LIBRARYJITNAMING` is a structural no-op on this port
(`BridgeSyscallHandler::LookupExecutableFileSection` always returns `nullopt`)
and gives the workaround: cross-reference `FEX_BLOCKJITNAMING` raw addresses
against a live `/proc/pid/maps`.

**`top-consumer-designs.md` — the PeekMessage design.**
Why Cyberpunk's call shapes miss the existing spec2thunk `peek` fast path, and
how to serve them guest-side. The load-bearing argument: Wine's own no-server
empty path never looks at hwnd or the message range, so queue-global quiet
proves emptiness for every filter. Empty-only by design, giving a one-sided
error surface — the fast path can never drop, duplicate or reorder, only
wrongly say "empty". Gated behind two cheap experiments before any code is
written.

**`ue5-msvcp140.md` — the Oblivion wall is NOT the C++ runtime.**
None of the 138 missing `MSVCP140` sentinels are ever called. `SteamAPI_Init`
succeeds, then `steam_api64`'s legacy liveness probe
(`OpenEventA`/`OpenFileMappingA` on `Local\SteamStart_SharedMemFile`) finds
nothing backing it and calls `TerminateProcess(-1, 0x33)` = rc 51. Exactly the
wall `ppc64le/games/STATUS.md:644` already predicted. Also records why the
generator refuses the whole `_Cnd_*`/`_Mtx_*`/`_Thrd_*` family: they are
implemented in `dlls/msvcp90/misc.c` but declared in no header at all.

**`mouse-wedge-rescope.md` — the mouselook wedge.**
Superseded by the fix (see `git log` for
`winex11: take raw deltas from absolute axes when nothing reports relative`),
but retains the upstream research. The actual cause: Xwayland reports the
master pointer's X/Y valuators as ABSOLUTE while delivering real relative
deltas in `XI_RawMotion` raw values; `update_relative_valuators()` only
accepted `XIModeRelative` axes, so every event was discarded. Measured with a
Wine-free X11 client, `probes/relmotion.c`: 8031 raw events, all 8031 carrying
deltas, zero relative axes. Five previous fix attempts and a cosmic-comp patch
all worked the wrong side of the boundary.

**`stale-sweep.md` — an audit of refusals and TODOs whose blocker no longer exists.**
Eight actively misleading, plus twelve verified as still true (which is the
half that stops the sweep being repeated). Also flags where documentation
encodes the co-developers' POWER8 + V620 hardware rather than this
POWER9 + RX 7900 XTX box.

## The rest

`merge-1116-report.md` and `upstream-merge-plan.md` record the Wine 11.16
merge, including two genuine bugs it surfaced (a stale hardcoded `context_data`
size in `tools/make_requests`, and a drifted header-line citation in
`kernel32.thunks`). `master-cherrypicks.md` covers two master-only fixes and,
importantly, corrects a hardware assumption: **this machine runs 4 KB pages,
not 64 KB.** `smclazylink-spam.md` measures a log-spam fix and is honest that
its performance effect is ~0.1-0.2% rather than the frame-rate problem it was
suspected to be. `dxvk-redundant-copy.md`, `dex-spin-diagnosis.md`,
`ntdll-null-deref.md`, `doom-runaway-recursion.md` and `fiber-teb-stack.md`
each name a specific defect; several of them also record a hypothesis that was
disproved on the way, which is usually the more useful part.

## Caveats worth carrying

- Benchmark numbers here vary ~20% run to run. Compare floors rather than
  averages, and never draw a conclusion from a single leg.
- Some runs were taken at SMT=4 and some at SMT=2. The switch is worth roughly
  10%, which is inside the noise of a single comparison.
- Games throttle when their window is unfocused, and one unattended run stalled
  outright. Always verify a run completed (a full `frames.csv`) before
  trusting a number from it.
