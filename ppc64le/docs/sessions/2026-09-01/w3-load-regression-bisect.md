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

## The levers this bisect needed (added afterwards)

Lesson 4 above is now code.  Three environment variables, read once when a
COM surface attaches, put served rows back the way they were before the
completeness landings.  A leg that used to mean swapping built PE halves in
and out of a tree is now one `env` line, and it can run against the shipping
build.

| lever | what it does |
|---|---|
| `WINEEMUNOCOMROWS` | comma-separated `Iface::Slot` names, or `@/path/to/file`.  Each named row refuses exactly the way a generation-refused row does: one log line naming the lever, `E_NOTIMPL`, and the out-params scrubbed so the refusal is INERT. |
| `WINEEMUNOCOMIIDS` | comma-separated IIDs, either `{770aae78-f26f-4dba-a829-253c83d1b387}` or the bare leading form `770aae78`; `@file` too.  A listed IID is treated as unrostered where interfaces are handed out: the object is released, the out pointer NULLed, `E_NOINTERFACE` returned — the release-and-NULL W3 got for months. |
| `WINEEMUNOCOMWAVE` | `getfamily`, `syscom`, `dinput8`, `rest`.  Whole landings, expanded to the row and IID sets in `libs/winecom/winecom_waves.h`.  The four **partition** the landing, so naming all four is the entire stretch off. |

An `@file` takes one name per line, `#` comments allowed — **and it also
reads `wave-rows.list`'s own dialect**: `[section]` headers are skipped,
a `row `/`iid ` prefix is stripped, and each lever takes only its own kind of
line.  So the checked-in list, or any excerpt of it, can be handed straight to
either lever with no reformatting.

The wave membership is **derived from git**, not typed by hand:
`ppc64le/winecom/derive-wave-rows.py` diffs the generated marshal headers
between `984c52a6d1d` (this doc's known-good baseline) and `c199f79caf9`, and
writes `ppc64le/winecom/wave-rows.list` plus the runtime's copy of it.  Counts:

* **getfamily** — 44 rows.  The whole `XSGetShader` count-through-pointer set
  (30 rows across `ID3D11DeviceContext`..`4`), plus `OMGetRenderTargets` (6),
  `SOGetTargets` (7) and the two D3D10 `OMGet`/`SOGet` pairs.
* **syscom** — 158 rows and **12 IIDs**, including
  `{77aa99a0-1bd6-484f-8bc7-2c654c9a9b6f}` `IAudioSessionManager2`, the one
  theory 2 names.
* **dinput8** — 6 rows (`ConfigureDevices`, `EnumDevicesBySemantics`,
  `EnumCreatedEffectObjects`, each A and W).
* **rest** — 258 rows and **4 IIDs**: everything else the landing touched.
  The d3d11 event/swapchain/video serves (81 rows once `getfamily` takes its
  share), the whole mfplat wave (174 rows, and the four `INSSBuffer*` /
  `IDispatch` IIDs), and the three d3d12 rows.

The four **partition** the landing — 466 rows in total, no row in two waves,
no row the rules found in none — and `derive-wave-rows.py --check` asserts
exactly that.  So `getfamily,syscom,dinput8,rest` is the whole stretch off,
which is the leg this note could not write before.

**Where the diff disagreed with the theory list above.**  Theory 1 says the
`OMGetRenderTargets` family is part of the same wave as the `GetShader`
serves.  It is — but not by the same mechanism, and a refuse-string diff
cannot see it: `OMGetRenderTargets` never carried a refuse string at either
commit.  What changed is its `caux`, from `NULL` to a real count-parameter
array, which is exactly the "caux-at-0 fix" this doc describes: before it the
row was refused **at runtime**, by a dispatcher that read the array's count
out of parameter 0.  So the derivation uses two rules, and the second one is
the only reason `getfamily` contains the rows theory 1 is actually about.
The script's banner spells both out.

The two commits also changed mfplat (174 rows, 4 interfaces newly rostered)
and d3d12 (3 rows), neither obviously in W3's path.  Those are `rest`, named
row by row in `wave-rows.list` rather than left as a count — because a row
nobody can name is a row nobody can rule out.

### Running the legs

All of these are plain runs of the shipping build.  Nothing is swapped;
nothing is rebuilt.

```sh
# leg 1-equivalent: every pre-existing negative control at once, as before
WINEEMUNOCOMEVENT=1 WINEEMUNOREFUSESCRUB=1 WINEEMUNOCOMFP=1 <launch W3>

# leg 3-equivalent: the syscom wave back to refusing, IIDs included --
# this is what leg 3 was reaching for, and it now covers the IID half too,
# which the thunk-regeneration leg could not
WINEEMUNOCOMWAVE=syscom <launch W3>

# theory 1: the d3d11 Get-family serves (this is what leg 5 was staged for)
WINEEMUNOCOMWAVE=getfamily <launch W3>

# theory 2: the syscom native upgrades
WINEEMUNOCOMWAVE=syscom <launch W3>

# theory 3: the dinput8 shims
WINEEMUNOCOMWAVE=dinput8 <launch W3>

# theory 4, the one that did not exist before: everything ELSE the landing
# touched -- the d3d11 event/swapchain/video serves, all of mfplat, d3d12
WINEEMUNOCOMWAVE=rest <launch W3>

# ALL FOUR: the entire stretch off, because the four waves partition it.
# If THIS still faults, the fault is not in the COM rows at all and the
# bridge-skew section is where to look next.
WINEEMUNOCOMWAVE=getfamily,syscom,dinput8,rest <launch W3>
```

Narrowing after a wave goes clean: split the wave's own list.  The lever
reads `wave-rows.list` in its own dialect, so no reformatting is needed —
`[section]` headers are skipped, the `row `/`iid ` prefix is stripped, and
each lever takes only its own kind of line.

```sh
# the whole landing, from the checked-in file (same as all four waves)
WINEEMUNOCOMROWS=@$PWD/ppc64le/winecom/wave-rows.list <launch W3>

# ...and the IID half of that same file
WINEEMUNOCOMIIDS=@$PWD/ppc64le/winecom/wave-rows.list <launch W3>

# one wave's section, or half of one -- still the same dialect
sed -n '/^\[getfamily\]/,/^\[/p' ppc64le/winecom/wave-rows.list \
    | head -20 > /tmp/leg.list
WINEEMUNOCOMROWS=@/tmp/leg.list <launch W3>

# one row, the finest grain
WINEEMUNOCOMROWS=ID3D11DeviceContext::PSGetShader <launch W3>

# theory 2 at the single-IID grain: the audio-session path only
WINEEMUNOCOMIIDS=77aa99a0 <launch W3>
```

### Reading the result

Run with `WINEDEBUG=+winecom` and check stderr before believing any leg:

* `WINEEMUNOCOMROWS/WAVE armed -- N of this surface's slots forced to refuse`
  — the leg actually did something.  **If N is 0, the leg tested nothing.**
* `matches no row on this surface` — a typo.  Every name that matched nothing
  anywhere gets one of these; a bisect leg that logs one and is recorded as
  "clean" is a wrong answer, not a data point.
* `the row has out-parameters and NO scrub masks -- the refusal is PARTIAL`
  — that row's caller reads its own residue.  Real, and worth knowing, but it
  means the leg is not a clean pre-landing reproduction for that row.

The gate is `ppc64le/winecom/check-com-levers.sh` (every leg an
armed/unarmed pair; `--sabotage` runs the unarmed controls alone).  It exists
because a lever that silently does nothing is worse than no lever: a leg run
under one is recorded as "tested, clean", and that is the most expensive kind
of wrong answer this bisect can produce.

## The FLAT levers (added afterwards, same lesson)

The COM levers above exonerated the COM half: with `WINEEMUNOCOMWAVE` putting
all 466 rows and 16 IIDs back to their pre-landing refusal, the fault at
`witcher3.exe+f4a78b` is **byte-identical**.  That leaves the other half of
the completeness program, and it is the bigger one.

Commit `1edc93608b6` ("spec2thunk: the source-definition tier") newly SERVES
**2,624 flat exports** that used to be refused — signatures inferred from
Wine's own implementing C source instead of from a header declaration.
Biggest gainers: **ucrtbase +417, kernelbase +340, msvcrt +309, msvcr120
+284, ntdll +105**.  One wrongly-inferred signature on a hot CRT export
produces exactly this fault shape: this tree's sub-word / wrong-width ABI
failure class, whose symptom is a wrong NUMBER (a length that comes out `-2`,
a pointer with a garbage top half) rather than a crash at the call site.

There was no way to test that.  The tier's provenance lived only in
`spec2thunk --report`, and **the build never passes `--report`** — so nothing
beside the shipped artifact said which of its 20,625 rows were new.  It does
now: **descriptor bit 10** (`THUNK_SIG_SRCTIER` in ntdll's `signal_ppc64.c`,
`DESC_SRCTIER` in `wine_sig.py`) marks a source-tier row, one bit per export
in a table the guest PE already carries.  Header-tier rows are unchanged.

| lever | what it does |
|---|---|
| `WINEEMUNOFLATTIER=source` | Every source-tier row in every module goes back to its pre-tier state.  **This is the one-variable leg that tests the whole `1edc93608b6` landing.** |
| `WINEEMUNOFLATMODS=a,b,...` | Source-tier rows of the named guest DLLs only.  `ucrtbase` and `ucrtbase.dll` both work.  For binary-searching the five big gainers. |
| `WINEEMUNOFLATROWS=m!E,...` | Individual exports: `module!Export`, or a bare `Export` (which may match in any module, and then says which), or `@/path/file` with one target per line and `#` comments.  The finest grain. |

All three are additive, and all three reach **only** the source tier.  A
header-tier row is not what landed, so it is not a restore target; naming one
forces nothing and is reported as a miss.

### What "back to its pre-tier state" actually means

A generation-refused flat export is simply **not emitted** — no stub, no
descriptor, no `.def` line, so the guest thunk PE has no such export.  At run
time that has exactly two faces, both already in `dlls/ntdll/loader.c`:

* an **import** of it binds to `allocate_stub()`'s per-symbol `0xdead0000+n`
  sentinel (unmapped on this port), with a `WARN` at the bind site naming the
  dll and symbol.  Binding is harmless; only *calling* it faults, at an
  address that names its own symbol.
* a **GetProcAddress** of it answers `NULL`.

Both are downstream of one function answering NULL: `find_ordinal_export()`,
the single choke point for by-name and by-ordinal resolution alike, which
already answers NULL for an export-table hole.  A lever hit makes it answer
NULL for that stub, so every consequence is the pre-tier behaviour produced by
the pre-tier code.  **No new refusal path was invented**, deliberately: with
one, a leg would be measuring the lever instead of the tier.

### Leg 2 is not the answer to this

The table above records leg 2 — "7 modules' guest thunks regenerated with the
OLD tool" — as *"fault persists → source-tier thunks 'not it'"*.  Do not treat
that as settled, for three reasons:

* it ran under the **bridge skew** this note opens with, like every other leg;
* it covered seven modules, not the whole tier.  The tier touched **95
  Makefile invocations**; those seven are most of the row count but not all of
  it, and "the rest" is exactly where a binary search would end up if the
  first five come back clean;
* it swapped built halves, which is the procedure that produced the invalid
  leg 4.  `WINEEMUNOFLATTIER=source` swaps nothing and cannot make that
  mistake.

Run leg (a) below even if leg 2 is believed.  It costs one `env` line.

### The legs, in order

```sh
# (a) THE WHOLE LANDING OFF.  One variable, and it is the leg that decides
#     whether the flat tier is in this at all.
WINEEMUNOFLATTIER=source <launch W3>

# (b) If (a) is CLEAN, binary-search the modules.  Start with the five big
#     gainers, since together they are 1,455 of the 2,624 rows.
WINEEMUNOFLATMODS=ucrtbase,kernelbase,msvcrt,msvcr120,ntdll <launch W3>

#     Clean?  then the culprit is in "the rest" -- everything else the tier
#     touched.  Not clean?  halve the five:
WINEEMUNOFLATMODS=ucrtbase,kernelbase   <launch W3>
WINEEMUNOFLATMODS=msvcrt,msvcr120,ntdll <launch W3>
#     ...and keep halving to the single module.

# (c) Then binary-search that module's own source-tier rows down to ONE
#     export.  The row list comes out of the built artifact itself:
python3 ppc64le/thunks/flat-tier-rows.py \
    dlls/ucrtbase/x86_64-windows/ucrtbase.dll > /tmp/leg.list   # 417 lines
head -208 /tmp/leg.list > /tmp/leg-a.list
WINEEMUNOFLATROWS=@/tmp/leg-a.list <launch W3>
#     ...halve, repeat, and finish at the single export:
WINEEMUNOFLATROWS=ucrtbase!_o__wcsnicmp <launch W3>
```

`flat-tier-rows.py` reads the tier bit out of the **built** guest DLL, so its
output is always the rows that artifact really serves from source.
`--count` gives a one-line summary per module, `--header-tier` gives the
control set (the rows no lever can touch), `--plain` drops the `module!`
prefix.

### Reading the result

Check stderr before believing any leg:

* `WINEEMUNOFLAT* armed: tier=source, N module/export target(s)` — the run
  read the lever at all.
* `ucrtbase.dll: 417 source-tier rows forced back (of 417 source-tier, 1836
  exports)` — one line per thunk module, and **this is the line that proves
  what the leg tested.  If the count is 0, the leg tested nothing.**
* `names '<x>', and <mod> has no such SOURCE-TIER row` — a typo, or a
  header-tier export the levers do not reach.  Either way that target tested
  nothing.
* `has matched no source-tier row in any thunk module seen so far` — a bare
  export name that has not landed yet.  It may still match a module loaded
  later; if the line is still appearing at the end of the run, it never did.
* `more than 256 modules have been resolved against the flat levers` — the
  per-module cache is full and anything past it is not forced.  The leg is
  incomplete; no process has come near this, but it says so rather than
  quietly stopping.

The gate is `ppc64le/thunks/check-flat-levers.sh` (every arm an armed/unarmed
pair; `--sabotage` runs the unarmed controls alone), for the same reason as
the COM one: a lever that silently does nothing is worse than no lever.
