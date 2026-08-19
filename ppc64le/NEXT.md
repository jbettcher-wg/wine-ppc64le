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

* **Graphics corruption — STILL PRESENT.  Improved by the GTT finding,
  not fixed, and the user's read is memory corruption.**  What is measured:
  across runs 36-38 the dense speckle tracked `VKD3D_CONFIG=no_upload_hvv`
  exactly (mesh shaders on or off), and dropping it (appconfig updated; the
  V620's 30 GiB device heap is fully host-visible/ReBAR, the flag bought
  nothing) made scenes visibly better — but the game is NOT clean: garbled
  surfaces remain in ordinary play, and it looks like memory corruption at
  scale.  `ppc64le/vkd3d/probes/copy_pattern_run.sh` cleared ONLY the quiet
  single-buffer copy path: pattern upload->CopyTextureRegion->texture->
  readback is byte-identical in BOTH placements, so this tree's marshal/
  copy slots are not the corrupter — but that clears a path, not the
  system.  Open, in the order worth measuring: (1) game-scale GTT/system-
  memory DMA on the custom 4K POWER8 kernel (amdgpu TCE/window shape, high
  physical pages) — the strongest lead given the no_upload_hvv split;
  (2) a scaled-up probe (hundreds of MB, many concurrent uploads, GPU
  under load) to make the corruption reproduce OUTSIDE the game, which is
  the difference between a screenshot and a bug report; (3) whatever
  writes guest-visible memory at scale in this port (mapped VRAM through
  the emulator, wineserver shared sections) — "memory corruption" is the
  user's read of the pattern and nothing measured yet contradicts it.
  The probe's GTT leg stands as the tripwire: if the quiet case ever
  fails, chase that before anything else.
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

## 6. Performance: the ~2.5-core ceiling

Unchanged and unexplained: latency-bound, ~54,000 wakeups/s, while an emulated
GE-Proton on the same machine uses 7-9 cores.  DOOM at 500%+ CPU during load
suggests the ceiling is not where it was measured, so **re-measure before
theorising**.  The NUMA lever (`WINE_PPC64LE_NUMA_NODE`) is in place and
unmeasured: bound and unbound runs of the same scene, frame times compared, is
an afternoon's work and would settle whether it is worth a default.

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
