# W3 load regression after the completeness program — bisect state at pause

**Status: UNSOLVED, paused mid-bisect (2026-09-01 ~04:30).  READ THE
BRIDGE-SKEW SECTION FIRST — it may invalidate every exclusion below.**

## The symptom

After the completeness landings (`74591109c3f`..`c199f79caf9`), Witcher 3
hangs or crashes on load.  Signature, reproducible across runs:

* A NEW deterministic guest fault at `witcher3.exe+f4a78b` — a small
  copy routine (`mov edi,ecx; mov rsi,rdx; mov rcx,r8; rep movsb`)
  called with **src=2, len=-2** — on multiple worker threads.  The
  good-baseline run (20260831-233412, at `984c52a6d1d`) does NOT have
  this site (its 2 faults are different, handled, the known DRM-probe
  class).
* Fatal follow-on faults at varying sites with **half-written
  pointers** (e.g. `0B50_0000_3FF54633` — plausible low 4 bytes, garbage
  top 4): the sub-word width class, or heap corruption.
* Sometimes instead a WEDGE with **zero faults**: a thread blocks
  forever on the game CS at image+`56F5EF8` (the same CS from the
  GetShader crash-reporter deadlock analysis), holder never releases,
  138 threads sleeping.  So the corruption is not the only path to the
  hang — or the wedge is a second problem.

Logs: `~/.local/share/wine-ppc64le/nw-witcher3/wine-ppc64le-native-
20260901-{031656,031930}*.log` (user's two runs), plus the bisect legs'
logs beside them.  Diag artifacts in op4k `/tmp/w3diag/` (**reboot
loses /tmp** — stats dumps, stacks.txt, module reports, artifact
backups all live there; the tree itself is restored, see below).

## THE BRIDGE SKEW (from the user's fastppcx86 agent, verbatim intent)

> We relinked libfexbridge.so in place (the wine lane loads it by path)
> and refreshed the binfmt pin twice, MID-FLIGHT while the wine agents
> were changing things on their side.  If they're testing against
> assumptions about the bridge binary or a leftover FEXServer from
> before the rebuild, that's a real skew vector — "we rebuilt the tree
> under them", not the CoreIsolation logic.

Every bisect leg below ran under a possibly-shifting build-smc bridge
and possibly a stale FEXServer.  **A fault that "survives every wine
rollback" is exactly what emulator-side skew would look like.**  Before
trusting ANY exclusion: rebuild build-smc's fexbridge deterministically
from the pinned fex commit (`0411b2749` is what the wine side expects,
ABI 7 + cookies), verify BridgeSmoke 254/0, kill any leftover FEXServer
(scoped!), re-pin binfmt ONCE, then re-run ONE plain W3 leg as the new
baseline.  If the fault is gone, the whole bisect was chasing the
rebuild, not the code.

## Bisect legs run (all seat runs, ~5–6 min each), provisional verdicts

| leg | change | result |
|---|---|---|
| 1 | `WINEEMUNOCOMEVENT=1 WINEEMUNOREFUSESCRUB=1 WINEEMUNOCOMFP=1` | fault persists → levers "not it" |
| 2 | 7 modules' guest thunks regenerated with the OLD tool (msvcr120, kernel32, kernelbase, ntdll, user32, msvcrt, ucrtbase) | fault persists → source-tier thunks "not it" |
| 3 | combase/ole32/oleaut32 thunks regenerated from pre-syscom `.thunks` (refusals returned, wrappers unreachable) | fault persists → 17 flat wrappers "not it" |
| 4 | old d3d11 stack — **INVALID LEG**: swapped d3d11.so + libdxvk_*.so + x86_64 thunks but NOT the `ppc64-windows/*.dll` PE halves, and the marshal tables/walkers live in the PE half | fault persisted, but proves nothing |
| 5 | full old PE-half set (combase, dinput8, d3d11, dxgi, d3d10core, oleaut32, ole32) — **BUILT AND READY in `~/Projects/power8/wt-d3dold/`, never run** (user paused) | — |

Also established: the audio PROPVARIANT refusal is IDENTICAL in the
good run (not new); the FEX_SMCLAZYLINK banner flood is normal (895
lines in the good run too); the game's own faults-then-wedge is the
crash-reporter deadlock shape the user diagnosed for GetShader.

## Lane theories still standing (if the bridge re-baseline still faults)

1. **Newly-LIVE d3d11 Get-family serves**: the caux-at-0 fix woke
   OMGetRenderTargets after weeks of silent runtime refusal (W3 calls
   it constantly), and the GetShader countptr serves are new — a
   refcount imbalance or wrong-count wrap on either = wandering
   use-after-free, which fits the signature.  Leg 5 tests this
   PROPERLY (PE halves).  No runtime lever exists for these — consider
   adding one while fixing.
2. **syscom native upgrades**: {77aa99a0}=IAudioSessionManager2 is now
   SERVED where W3 got a release-and-NULL for months — a brand-new
   audio-session code path in the game.  Also in leg 5's swap set.
3. The remainder wave's dinput8 shims (W3 uses DirectInput) — in leg 5.

## Tree/box state at pause

* Main tree RESTORED to clean `c199f79caf9` artifacts and settled
  (`make` green).  All leg swaps undone from backups; no `.newtool`/
  `.newflat` strays.  Wine main tree = shipping state.
* `wt-d3dold/` KEPT: a full tree with dlls/{d3d11,dxgi,d3d10core,
  combase,dinput8,ole32,oleaut32} + libs/winecom + include/wine at
  `984c52a6d1d`, PE halves BUILT — leg 5 is one swap + one run away.
  Backups of the new artifacts: `/tmp/w3diag/d3dnew/` (**/tmp — copy
  to /home before any reboot if leg 5 is still wanted**).
* build-smc bridge: rebuilt tonight at `e4fd3b3f1` by this lane
  (BridgeSmoke 249/0 then) — but see the skew section; the fex lane
  relinked it again afterwards.
* No game processes left running; user's seat untouched beyond the
  consented test launches.

## Next actions, in order

1. Settle the bridge (fex lane + wine lane agree on ONE binary), kill
   stale FEXServer, re-pin binfmt once, BridgeSmoke green.
2. ONE plain W3 baseline run.  Fault gone → close this as bridge skew,
   re-run the full gate sweep for confidence, done.
3. Fault persists → run leg 5 (everything staged).  Clean → binary-
   search within the seven modules (PE halves individually).  Then
   code-read the guilty row (start: OMGet/GetShader wrap refcounts,
   ARR_OUT_STATIC identity path).
4. Whatever the outcome: the two structural lessons stand — the
   ppc64-windows PE half is the marshal's home (leg-4 mistake), and
   big-bang landings need a per-wave runtime lever so a regression
   bisects in minutes, not in seven seat runs.
