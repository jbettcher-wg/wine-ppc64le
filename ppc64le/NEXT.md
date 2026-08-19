# What to do next

Written 2026-08-18, at the end of the session that got DOOM (2016) into
gameplay.  It is ordered: the things at the top are the ones that unblock the
most, and each entry says what is known, what is not, and where the evidence
is.  `ppc64le/games/STATUS.md` is the per-title board; this is the work list.

## Where the port stands

* **DOOM (2016) plays.** Fibers and two callback classes were the last walls;
  see the commits from `ntdll: say when the native cpu is the one executing
  guest code` onwards.
* **33 gates**, each with a negative control.  The full suite was green on the
  build DOOM runs on.  The `--sabotage` half of that sweep was interrupted and
  has not been re-run since the 226 callback rows landed — **do that first**,
  it is twenty minutes and it is the only thing between here and "the suite is
  green" being a true sentence.
* Cyberpunk 2077 reaches its own code; Boltgun reaches its engine; Portal 2,
  the first 32-bit title actually tried, does not launch and nobody has looked
  at why.

## 1. Finish the sabotage sweep

```sh
for g in ppc64le/*/check-*.sh; do "$g" --sabotage; done
```

Every gate's negative control must go red.  Two were added today
(`check-fibers.sh`, `check-callback-rows.sh`) and both pass their controls
individually; what has not been proven is that the other 31 still fail when
they should, with 226 new rows in `thunk_overrides[]` underneath them.

## 2. Portal 2, and the 32-bit lane in general

Portal 2 (appid 620) was launched on 2026-08-18 and did not start.  Nothing
has been diagnosed.  Logs are in `steamapps/compatdata/620/`.

It matters beyond one game: Half-Life 2 and its episodes, FreeInfantry and the
Win32 build of Styx are all PE32, and the WoW64 lane has never carried a real
game — only `check-wow64-smoke.sh`.  Whatever Portal 2 hits is likely to be
what all of them hit.

## 3. Cyberpunk: from character creation to playable, and the graphics issues on the way

The 2026-08-19 sessions closed the old item 3 and everything behind it: the
whole d3d12 marshal surface is generated again (`ppc64le/vkd3d/gen_winecom.py`
-- the lane's lost generator, rewritten into the family; every one of the 117
interfaces has a real table now), the 32-bit-stack-slot extension the flat
lane already had is in the COM lane (`winecom_slot::dwordmask`, measured on
CopyDescriptors' heap type), IPropertyStore is rostered, the swapchain call
crosses from DXVK's factory to the d3d12 lane
(`__wine_d3d12_create_swapchain_for_hwnd`), the phase-(a) swapchain answers
`IDXGISwapChain4`, and `VKD3D_CONFIG=nodxr` hides the RT tier the state-object
refusal cannot yet serve.  THE GAME RUNS: title screen at 245 fps, menus,
character creation, all user-confirmed on screen.  What remains, in order of
what is actually seen:

* **Graphics corruption — SOLVED (2026-08-19, the -benchmark sessions).**
  It was never memory, DMA, or upload placement:
  `ID3D12GraphicsCommandList::ClearDepthStencilView` passes FLOAT by value
  and sat as a named refusal in the marshal table, so the game never
  cleared depth and every 3D pass tested against stale depth/HTILE —
  per-pixel confetti in dark scenes, a fixed dot lattice in haze (the
  HTILE tile pattern), UI/Bink video untouched.  Served now by
  `hand_clear_dsv` + the unixlib's typed-float call (`FP_SHAPE_CLEAR_DSV`
  in dlls/d3d12/unixlib.h — the pattern the NEXT float-bearing slot should
  reuse).  Benchmark (`Cyberpunk2077.exe -benchmark`, results JSON under
  the prefix's Documents) renders clean end to end; avg fps unchanged at
  ~15.6, the lane stays CPU-bound (item 6).  The elimination record, all
  committed as probes: `host_vk_storm.c` (host Vulkan, no Wine/FEX,
  7.7 GiB/leg, both placements, clean), `copy_storm_run.sh` (guest lane,
  3 queue types, 3 fill modes, 2.3 GiB/leg, both placements, clean, with
  staging-pre/roundtrip/staging-post verdict classes), cross-ISA
  dxil-spirv byte-compare (2176 game shaders, zero divergent), and this
  tree's vkd3d cross-built x86-64-PE running clean in the emulated lane.
  What that record buys the next chaser: the whole guest->GPU data path
  and the shader pipeline are PROVEN clean at game scale — when something
  garbles next, look at what the log says was REFUSED first (`grep -a
  refus` the run log; refuse_once prints ONCE per slot, so one line can
  mean sixty times a second).
* **Frame-latency waitable**: `GetFrameLatencyWaitableObject` returns NULL
  (unix_present.c, "the eventfd->semaphore relay is P5").  vkd3d's side
  already hands out a TAGGED eventfd (vkd3d-patches/0001); the lift is the
  relay a guest can `WaitForSingleObject` on -- an NT semaphore the unix side
  releases when the eventfd pays out.  The game runs without it today.
* **DXR honestly**: the `D3D12_STATE_OBJECT_DESC` walker
  (ID3D12Device5::CreateStateObject and Device7::AddToStateObject), so nodxr
  can come back out of the appconfig on hardware that can afford it.
* **PipelineLibrary Load pair**: LoadGraphicsPipeline/LoadComputePipeline
  refuse (desc structs carry the root signature); the game falls back to
  fresh PSO creation every boot, which costs load time, not correctness.
* The 15-ish `err:combase:__wine_com_refuse` flat-export refusals at boot and
  the unknown syscom IID {77aa99a0-1bd6-484f-8bc7-2c654c9a9b6f} -- survived,
  unidentified; name them with a +thunk trace when they matter.

## 4. The trampoline pool stops at six arguments

`ppc64le/thunks/callback_holes.txt` lists 24 exports with no wrapping row.
Fourteen of them are waiting on one thing: `wrap_guest_callback_ex` has
fixed-arity dispatchers for four, five and six arguments and refuses anything
else by name.  Extending it to seven, eight and nine closes
`SetWinEventHook` (7), `DdeInitialize` (8), `WSAAccept` (8), `EventRegister`
(7) and the `CopyFileEx`/`MoveFileWithProgress` family (9).  The pattern for
five and six is already in the file; this is mechanical.

## 5. Make a new title's first run boring

The tools exist and are not yet joined up:

* `ppc64le/games/library_sweep.py --audit` reads the whole Steam library and
  says what will happen to each title, in under two seconds.
* `ppc64le/thunks/import_chain.py` does the same for one binary anywhere on
  disk, which is how Cyberpunk's three missing modules were found in seconds
  after the run had already cost minutes.
* What is missing is the loop: a title that needs a per-game setting still
  needs somebody to know that.  `ppc64le/steamtool/appconfig/<appid>.env` is
  the mechanism (DOOM has one); filling it in from sweep results rather than
  from failed launches is the difference between this working for other people
  and working for its author.

## 6. Performance: the ceiling re-measured (2026-08-19) — it is one thread

The ~2.5-core story is DEAD.  Measured mid-flythrough on the Cyberpunk
`-benchmark` harness: the game process pulls **~15 cores** (1498%),
**GameThread is pinned at 92%** — the frame rate is that one thread's
JIT throughput — ~25 redDispatcher workers idle-spin at ~50% each, the
process context-switches 45k/s, and wineserver is a NON-factor (4.5%,
2k switches/s).  Whatever produced the old 2.5-core measurement, the
current tree does not reproduce it.

The lever matrix, one `-benchmark` run each (avg fps; noise ±0.1 on
back-to-back runs):

| config                                   | avg fps |
|------------------------------------------|--------:|
| as found (ondemand gov, SMT4, node0)     | 15.04 / 14.94 |
| performance gov, SMT4, node0             | 15.45 |
| performance gov, SMT4, unbound           | 16.05 |
| performance gov, **SMT2, node0**         | **7.47 — never do this** |
| performance gov, **SMT2, unbound**       | **17.56 / 17.75 / 16.34** (three samples) |

So: **performance governor + SMT2 + NO numa bind** is the measured
winner, and the node-0 bind must never be combined with SMT2 (node 0
then has 20 threads against the game's ~15-thread appetite; it halves
the frame rate).  SMT2's win is exactly what the thread profile
predicts: the fatter per-thread core feeds the one thread that matters.
Note SMT2-unbound run variance is real (~1.4 fps across samples), much
wider than SMT4's ±0.1.

**A correction stands where FEX_* rows used to be.**  The first knob
matrix (SPINCOLLAPSE, X87REDUCEDPRECISION) ran through steamtool/
proton, whose environment filter STRIPPED FEX_* wholesale — a rule from
before the bridge read any environment — so every "knob" leg actually
ran bare and its delta was run noise.  The filter now passes FEX_*
through by default (WINE_PPC64LE_STRIP_FEX_ENV=1 restores the strip),
`stripped 0 variable(s)` in the launch log is the tell, and the knobs
are UNMEASURED on this lane by deliberate choice: the user has done the
JIT testing; what this tree owes is passthrough, which is verified.
FEX_* knobs reach the native lane since fastppcx86 `54df357cb` (the
bridge environment layer), and FEX_HWTSO works for real since
`d4168c1ec` + this tree's PROT_SAO wiring (see the commit).

What remains: the GameThread itself.  Next instrument is a perf profile
of that one tid mid-benchmark (`FEX_GLOBALJITNAMING=1` writes
/tmp/perf-<pid>.map, and the bridge honors it now) to split its 92%
between JIT'd guest code, bridge helpers, and TSO barrier overhead.
HWTSO/PROT_SAO is NO LONGER FEXInterpreter-only: fastppcx86 `d4168c1ec`
re-hosts the probe and the refusal/revocation closure in the bridge,
and this tree carries the bit through get_unix_prot(), retro-applies it
at the lazy bridge init (virtual_enable_hwtso) and reports kernel
refusals back (mprotect_hwtso -> emu_hwtso_refused).  `FEX_HWTSO=1 fex
nw-<title>` is live end to end — verified by smaps ('ar' VmFlags on 83
wine-managed VMAs vs 0 in control) and the gate set, per the user's
direction measured no further.

## 7. Smaller, known, and written down

* `SetThreadGroupAffinity(group 1)` needs a `group` field in
  `server/protocol.def`; `check-cpu-topology.sh` reports it as a LIMIT and
  re-arms itself when the field appears.
* Quake II is on a library drive that is not mounted.
* `mfmediaengine`, `evr`, `wmvcore` have a built COM surface no title has
  driven.
* The callback audit cannot see a callback that arrives inside a **struct**
  (a `WNDPROC` in a `WNDCLASSEX`); those rows carry handlers and are found the
  hard way.  If a future crash names one, add it to
  `check-callback-rows.sh`'s reasoning rather than only to the table.
