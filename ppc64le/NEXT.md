# What to do next

Ordered: the things at the top unblock the most, and each entry says what is
known, what is not, and where the evidence is.  `ppc64le/games/STATUS.md` is
the per-title board, `ppc64le/WORKING-ON-THIS.md` is the operational knowledge
(env knobs, measuring, traps), and this is the work list.

## Where the port stands (2026-08-19)

* **DOOM (2016) plays.**  Fibers and two callback classes were the last walls.
* **Cyberpunk 2077 renders correctly.**  The long-running "memory corruption"
  was one refused `ClearDepthStencilView`: FLOAT-by-value, so no frame ever
  cleared depth and every 3D pass tested against stale depth/HTILE.  Served by
  a hand walker plus the unixlib's typed-float call; the built-in `-benchmark`
  flythrough is clean end to end and is now the A/B harness for everything.
* **The Witcher 3 plays** — in-world, mounted, HUD and weather live.  Four
  walls fell: the msvcr120 thunk, the SSE3 feature answers, NvCameraSDK's flat
  CRT imports, and `run-native` not setting the working directory (REDengine 3
  resolves content from CWD and exited rc=0 politely without it).
* **33 gates green, sabotage sweep included** — every negative control goes
  red, none skipped, none failed.  That sentence is finally true.
* **The 32-bit lane is half built.**  dexwin boots and shows a window; it has
  no graphics because i386 d3d11 exports no dxvk surface.  Item 2 has the
  state, and `ppc64le/dxvk/docs/i386-lane-design.md` the design and the crux.
* **Performance is one thread.**  Not the old ~2.5-core ceiling (which does
  not reproduce): the game pulls ~15 cores with GameThread pinned at 92%.
  Item 6 has the measured lever matrix.

## 1. DONE — the sabotage sweep is green

```sh
for g in ppc64le/*/check-*.sh; do "$g" --sabotage; done
```

 Run 2026-08-19 on the full tree: **33 gates, every negative control red,
none skipped, none failed.**  Keep it that way — a gate whose control stops
going red is a gate that has stopped testing anything, which is worse than
not having it.

Two notes for the next run.  Several gates raise REAL modal dialogs on the
live desktop as their controls and steal focus from whatever else is running;
give the sweep its own Xvfb.  And it takes about half an hour, so it is a
"before you push" step, not an inner-loop one.

## 2. The 32-bit lane: the i386 half of the dxvk thunk surface

**DIAGNOSED, SCOPED, GEOMETRY LANDED — emitter still to write** (updated
2026-08-22).  The winecom table can now describe an i386 stdcall frame:
`winecom_slot::qwordmask` distinguishes true 64-bit scalars (UINT64,
D3D12_GPU_VIRTUAL_ADDRESS — two i386 stack slots) from pointer-sized values
(HANDLE, SIZE_T, ULONG_PTR — one), which the old QWORD_BYVAL class conflated.
`WINECOM_F_RET_QWORD` marks EDX:EAX return, and `WINECOM_F_I386_GEOM` is the
contract flag a reader must consult before trusting the fields.  Widths come
from clang for the i386 target — never from name matching, which is exactly
how HANDLE and UINT64 ended up in one bucket the first time.  314 rows gained
qword markings and 11 return EDX:EAX; float-returning slots refuse geometry
rather than lying about it (i386 returns float in x87 ST(0), and a lane
trusting a wrong flag would return garbage and leak x87 stack entries; the
seven refused are d3d11's GetResourceMinLOD and d3d9's GetNPatchMode).
Nothing in the tree READS any of the new fields yet, so the fail-closed rule
is a CONTRACT and not an enforcement — the header now says so.

dexwin (Dex's Windows build, PE32, Unity 5) is the canary and it gets further
than anyone knew: the i386 guest boots through the ABI-4 bridge and Unity puts
its window up.  It dies where the 32-bit lane has no graphics at all — i386
`dxgi.dll`'s forwards to `d3d11.__wine_dxvk_*` resolve to nothing, because
this port's i386 d3d11 exports none of the dxvk surface (`err:module:
find_forwarded_export`, four entries).  Portal 2, Half-Life 2 and the Win32
Styx wait behind the same wall, so this is the whole 32-bit game lane in one
piece of work.

`ppc64le/dxvk/docs/i386-lane-design.md` carries the design: an i386 COM
emitter in spec2thunk (`int 0x80` stubs — the instruction the wow64 lane
already routes into the OS_GENERIC sink), stub-RIP mapping in the bounded
emu32 run loop, a `dispatch32` in libs/winecom that widens stdcall's 4-byte
stack slots into the same `UINT64 args[]` everything downstream already
speaks, and hand walkers for the descriptor structs an i386 guest lays out
differently.

That last set is MEASURED, not guessed: `ppc64le/dxvk/layout32.py` compiles
all 297 D3D11_/DXGI_ aggregates from Wine's own headers for both guest
targets and diffs size/alignment — **47 diverge**, and ~35 of those are
content-protection/video surfaces (D3D11_AUTHENTICATED_*, VIDEO_DECODER_*)
that no title of this era touches and should be refused by name.  The dozen
that matter (INPUT_ELEMENT_DESC, SUBRESOURCE_DATA, MAPPED_SUBRESOURCE,
SWAP_CHAIN_DESC, ADAPTER_DESC*, OUTPUT_DESC*, ...) are all on cold
creation/query slots, one mechanical widen/narrow walker each.  The 250
identical ones keep passing their pointer straight through, as on 64-bit.

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
* **DXR walker and PipelineLibrary loads -- DONE (2026-08-26, 477b103fb76).**
  Nine of the thirteen refused slots became hand walkers and the named holes
  dropped to four: the pipeline-library load trio (Cyberpunk hit the first
  pair on EVERY boot; its pipeline cache can load now instead of rebuilding
  each PSO), the two remaining float-by-value frames (OMSetDepthBounds,
  RSSetDepthBias -- hand_clear_dsv's XMM lift, two new typed shapes),
  BeginRenderPass and the enhanced Barrier, and the
  `D3D12_STATE_OBJECT_DESC` walker for CreateStateObject/AddToStateObject
  with association-pointer remap.  `nodxr` CAN come out of the appconfig
  now, but whether the V620 should be offered RT is a performance decision,
  not a marshalling one -- unmeasured, and the flag stays until it is.  The
  four rows still refused are three structural classes, each named in the
  table: DRED's native-owned breadcrumb list, WorkGraphProperties' 16-byte
  by-value aggregate, RegisterDestructionCallback's guest function pointer.
  None of the nine new walkers has been driven by a real title yet; the
  next Cyberpunk boot is the live test (watch load time, and `grep -a
  refus` should lose the PipelineLibrary lines).
* The 15-ish `err:combase:__wine_com_refuse` flat-export refusals at boot and
  the unknown syscom IID {77aa99a0-1bd6-484f-8bc7-2c654c9a9b6f} -- survived,
  unidentified; name them with a +thunk trace when they matter.

## 4. DONE -- the trampoline pool serves four through nine arguments

Closed 2026-08-22.  `wrap_guest_callback_ex` has dispatcher pairs and
guest-side argument thunks for seven, eight and nine arguments, and the
fourteen exports that were waiting have rows: `SetWinEventHook` and
`EventRegister` (7), `DdeInitializeA/W` (8, WIDE -- PFNCALLBACK returns a
64-bit HDDEDATA) and `WSAAccept` (8), and the `CopyFileEx`/
`MoveFileWithProgress`/`MoveFileTransacted` family (9, whose by-value
LARGE_INTEGERs are plain 64-bit slots in both conventions -- no FP
anywhere).  Seven kept the twelve-word tail-jump stub (identity in r10);
eight and nine could not, because r3..r10 are all real arguments and ELFv2
makes the parameter save area OPTIONAL when everything fits in registers,
so those two arities emit a call-shaped stub instead -- build a 112-byte
frame, store the guest target (and, at nine, the ninth argument forwarded
from the caller's own parameter save area, which nine arguments force it
to have) into the new frame's parameter area, bctrl, tear down.  All
emitted encodings machine-verified with llvm-mc.  `callback_holes.txt` is
down to the two deliberate classes (thread starts, IsBadCodePtr) and the
audit's per-callback arity ceiling moved from six to nine with it.  None
of the fourteen rows has been driven by a real title yet; the first
DDE/WinEvent/progress-callback user is the live test.

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

**The passthrough is verified; the PARITY is not there (found
2026-08-26).**  The emulated stack launches every game through
`~/fex-scripts/launchers.bak/fexplay-wtsmc`, which exports the tuned
set: HWTSO, LOCKONLYTSO, the whole SMC suite (SMCCHECKS=mtrack, cheap
tier, lazy inval, soft invalidate, store backpatch/emulation, mprotect
defer, semantic patch), FUTEXMITIGATE, SCHEDPASSTHROUGH.  The native
lane's `appconfig/*.env` files set NONE of them, and the 2026-08-19
Cyberpunk log confirms only X87REDUCEDPRECISION=1 reached the run -- so
the native lane's JIT emits full TSO barriers and strict SMC checks
against an emulated lane running years of tuning.  Comparing the lanes
before fixing that measures config, not architecture.  First lever:
mirror the fexplay-wtsmc knob set into the native lane's env (per-title
.env or a steamtool/proton default), re-run the `-benchmark` A/B --
parked until the user calls for it.

**Parity landed and the split is MEASURED (2026-08-26).**  The lane
defaults are in `steamtool/proton` now -- FEX_HWTSO=1 plus the lazy SMC
recipe, applied after the per-title .env so a caller still wins -- and
the bridge confirms in the log: "FEX_HWTSO live: PROT_SAO carries
ordering, no TSO barriers emitted".  FEX_SPINCOLLAPSE=128 rides in
Cyberpunk's .env (the emulated lane's own K sweep; UNSET IS OFF).

The GameThread perf split (30s mid-flythrough, 27k samples, tid-scoped,
JIT map live): **JIT'd guest code is only ~37%** of the thread.  The
rest is boundary overhead, and it decomposes into named work:

  * ~17% trap/dispatch machinery: emu_trap_dispatch 5.5,
    __wine_syscall_dispatcher 4.0, call/return user_mode_callback 3.5,
    call_emu_trap_dispatcher 1.4, emu_trap_thunk/publish/teb_switch 2.6.
  * ~14% bridge, over half of it guest-state pack/unpack per crossing:
    Reconstruct/SetCompactedEFLAGS 4.2, XMM state sync 2.1.
  * ~7% TLS resolution per hop: __tls_get_addr_opt 3.4,
    pthread_getspecific 2.6, NtCurrentTeb 0.8.
  * ~8% libc memcpy/memset; ~5% win32u (PeekMessage, get_tick_count,
    shared queue); clock_gettime + getrusage ~1.5.

So the frame thread's ceiling is not JIT throughput -- it is the PRICE
and the COUNT of guest<->native crossings.  The levers, in order:

  1. **Cheapen the crossing -- DONE (2026-08-27, second sitting).**
     Three wine-side costs deleted in `aefb21d8c74` (initial-exec TLS on
     all 23 per-thread trap-path variables + the thread_data pthread key's
     IE mirror + the 1232-byte debugger publish becoming a pointer), and
     the bridge half in fex-src `8a4b975fa` + this tree's `cc5e5d5b320`
     (bridge ABI 5: EFLAGS/XMM are not reconstructed per trap; the
     audited consumers materialize on demand, ppc64le/cpu/check-lazy-ctx.sh
     is the falsifiable gate).  Measured across the three commits, same
     -benchmark protocol: __tls_get_addr_opt 3.6% -> absent,
     pthread_getspecific 3.3% -> 1.4%, libc memcpy 5.8% -> below the
     profile cutoff, ReconstructCompactedEFLAGS/XMM -> absent; scene fps
     22.34/22.91 (two baseline legs) -> 23.95, and JIT'd guest code
     31% -> 37% of the GameThread.
  2. **Delete the hottest crossings** -- THE COUNT IS IN, and it names
     them.  `WINE_PPC64LE_TRAP_STATS=<path>` counts every crossing per
     call site (flat export, COM slot, syscall, guest callback);
     `ppc64le/cpu/CROSSINGS.md` is the mechanism and
     `ppc64le/cpu/crossings-cp2077-benchmark.txt` the table.
     [MEASURED] 2026-08-27 over Cyberpunk's 66.6 s flythrough:
     **235.6M crossings, 3.54M/s** at 18.25 fps -- **196,516 crossings
     PER FRAME**, of which:

     | crossing | /s | per frame |
     |---|---:|---:|
     | `QueryPerformanceCounter` (+ its own `NtQueryPerformanceCounter`) | 256,638 | 14,061 |
     | `ID3D12GraphicsCommandList::SetGraphicsRootDescriptorTable` | 176,021 | 9,644 |
     | `ID3D12Device::CopyDescriptors` | 164,319 | 9,003 |
     | `PeekMessageW` (+ its own `NtUserPeekMessage`) | 100,526 | 5,508 |
     | `ID3D12Device::CreateConstantBufferView` | 95,340 | 5,224 |
     | `ID3D12Resource::GetGPUVirtualAddress` | 94,618 | 5,184 |
     | `ID3D12GraphicsCommandList::DrawIndexedInstanced` | 78,318 | 4,291 |
     | `EnterCriticalSection` / `LeaveCriticalSection` | 69,862 / 69,861 | 3,828 each |

     So: **QPC and PeekMessage are confirmed, GetTickCount is dead** --
     377/s over the whole process and ZERO in the flythrough, so the
     `win32u get_tick_count` in the profile is inside PeekMessage, not a
     crossing of its own.  Serving QPC guest-side from KUSER_SHARED_DATA
     removes 34M crossings from this route (17M traps and 17M syscalls,
     14% of everything); the uncontended critical-section pair is
     another 9.3M for a call with no syscall behind it, and
     `GetGPUVirtualAddress` is a getter on an object the guest already
     holds.  **But the COM class is bigger than the flat class** (54.9M
     vs 44.9M): the four d3d12 descriptor/draw rows above are 30M
     crossings and no shared page can serve them -- they need batching
     or a guest-side descriptor cache, which is a design, not a knob.
     Guest CALLBACKS are a non-issue at ~18/s; do not spend time there.

     **QPC IS DONE (2026-08-27).**  The guest's kernel32 answers
     `QueryPerformanceCounter` and `QueryPerformanceFrequency` in its
     own x86-64 code -- `rdtsc`, one 64x64 multiply, a shift and an add
     -- with no trap and no syscall, the way Windows answers them from
     the TSC.  Measured A/B, one binary and one env var
     (`WINE_PPC64LE_NO_QPC_BYPASS=1`) apart, in
     `ppc64le/cpu/crossings-cp2077-qpc.txt`: all three rows leave the
     flythrough window ENTIRELY (198,735/s and 198,735/s become
     absent), and total crossings fall from **187,633 to 152,718 per
     frame**.  avg fps 14.999 -> 15.312 on the same armed runs.

     What made it correct rather than merely fast is that the guest and
     the native `NtQueryPerformanceCounter` compute the SAME
     expression from the SAME timebase, so an interleaved sequence of
     the two is monotone by algebra rather than by luck --
     `include/wine/emu_qpc.h` has the derivation, the measured
     emulator TSC scale, and the 39.6 ppm the timebase drifts from
     CLOCK_BOOTTIME (which is why the two places in Wine that convert
     QPC into the wineserver's clock now say `server_monotonic_time()`).
     `ppc64le/cpu/check-qpc-fastpath.sh` is the gate; its negative
     control breaks the seeding two ways and both go red.

     `tools/spec2thunk`'s `FAST_PATH_EXPORTS` is now a mechanism, not a
     one-off: an export can carry real guest code beside the stub array
     while its stub, the stride and the trap offset stay exactly what
     the dispatcher expects.  The next candidate is NOT PeekMessage
     (real queue semantics) and NOT GetTickCount (dead).  It is the COM
     class, which is now the largest by far and wants batching.

     **The COM class got its first two answers (2026-08-27, second
     sitting).**  `GetGPUVirtualAddress` is served guest-side from the
     proxy (`2cd77653d45`, WINECOM_F_CONST_QWORD): 88,016/s -> 25/s in
     the flythrough window, one crossing per distinct buffer ever.  And
     the top of the class -- SetGraphicsRootDescriptorTable, both Draws,
     IASetVertex/IndexBuffer, SetPipelineState, SetComputeRootDescriptor-
     Table, SetGraphicsRoot32BitConstant, ~385k crossings/s between them
     -- records guest-side into a per-command-list ring replayed in order
     at the object's next real trap (the call journal in
     libs/winecom/winecom.c; the big comment above install_journal is the
     correctness argument wall by wall).  WINEEMUNOCOMJOURNAL=1 and
     WINEEMUNOCOMCONSTGET=1 are the levers.

     [MEASURED] the journal leg: SetGraphicsRootDescriptorTable 176,243/s
     -> 10/s, DrawIndexedInstanced 66k -> 10/s, IASetIndexBuffer 51k ->
     4/s, IASetVertexBuffers 55k -> 73/s (the residue is the >8-views and
     ring-full fallbacks, working as designed), SetPipelineState 12.6k ->
     0.2/s.  COM class 780k/s -> 359k/s, ALL crossings 2.9M/s -> 2.18M/s.
     Scene fps is FLAT (23.5-24.0 vs 23.95 before), and the reason is
     worth keeping: command recording happens on the ~25 redDispatcher
     WORKER threads, not on the GameThread the frame rate is bound by --
     the journal buys back worker-side CPU (and total crossings), not
     GameThread time.  Whether that headroom turns into fps at SMT2 or
     under the performance governor is unmeasured.

     The remaining unbatched hot rows are CopyDescriptors itself (176k/s,
     trapping BY DESIGN -- its trap is the device journal's ordering
     point), ResourceBarrier (9.5k/s, struct arrays), and PeekMessageW
     (a leg of its own is in flight).

     **The device journal is BUILT but OPT-IN (2026-08-27 night,
     WINEEMUCOMDEVJOURNAL=1 to enable), because a hang it causes is
     UNATTRIBUTED.**  Built to the settled design below --
     CreateConstantBufferView records guest-side into per-thread rings
     (TEB SystemReserved1[0], RDTSC stamps, k-way merge at every COM
     dispatch, dirty-byte idle check) -- and hardened three times over by
     what one night of Cyberpunk legs found: a consistent-cut timestamp
     bounds the merge (the sequential ring-snapshot walk is not a cut),
     the ring header carries a magic the snippet verifies (SystemReserved1
     is reserved from Wine, not from the app), and the drain holds
     structurally-implausible records instead of replaying them.  Gate
     ppc64le/winecom/check-dev-journal.sh: 62/62 cross-thread replays in
     issued order, sabotage red, kill switch clean.

     What keeps it off: with the journal on, Cyberpunk raises ONE amdgpu
     gfx_0.0.0 ring timeout per benchmark leg (~3 min in, no VM fault, the
     game limps at 13-14 fps of reset wreckage afterwards; one run
     escalated to a MODE1 reset crashloop that took the compositor and a
     reboot with it).  Journal OFF is clean -- 24.42 fps, the best this
     title has measured.  Everything the replay stream could confess has
     been ruled out ON DATA: a fully-traced leg (2.08M replays) shows
     zero same-handle inversions, zero corrupt records, every replay
     before its consuming dispatch -- and still hangs.  FEX_HWTSO=1 does
     not change the verdict.  The one configuration that runs the full
     flythrough clean is the WINEEMUCOMDEVDOUBLE=1 diagnostic: records
     taken AND the call served live at record time -- so the poison is
     specifically "creates reach vkd3d only at the drain", through a
     consumer the trap set does not cover.  Suspects still standing:
     dev_cs convoy starving a thread the GPU waits on through a
     GPU-side wait packet, or a descriptor consumer inside vkd3d/dxgi
     that crosses on a path winecom never sees.  Two false leads worth
     not re-walking: the 58k "torn records" a plausibility check once
     flagged were LEGAL NULL CBVs ({0,0} range inits -- Cyberpunk writes
     tens of thousands), and run-native (unlike steamtool/proton) never
     arms FEX_HWTSO, which is a real gap on the bench path but not this
     bug.  The paragraph below is the design as built; the correctness
     comments live above wc_dev_drain in libs/winecom/winecom.c.

     **The device-pair DESIGN is settled (2026-08-27 evening), unbuilt.**
     Device methods are free-threaded, so the command-list journal's
     single-recorder assumption does not hold, and the hazard is app-
     synchronized cross-thread ordering (T1 CreateCBV slot A, sync, T2
     reads/copies A).  The design that survives it: journal ONLY
     CreateConstantBufferView, per-THREAD rings anchored at the guest
     TEB's SystemReserved1[0] (win64 +0x190, unused by Wine's 64-bit
     side), records carrying an RDTSC stamp -- the timebase, which the
     QPC work PROVED core-synchronized -- and the drain k-way-merges all
     rings by stamp.  App-sync spans a call return, so any ordered pair
     has both records ring-visible before the later one exists; merge
     order equals real order.  CopyDescriptors stays trapping and every
     dispatch drains all device rings first (a guest-set dirty byte makes
     that check O(1)), which is exactly what makes create-then-copy
     ordered.  Ring headers carry pos/cons; cross-thread drains advance
     cons, only the owner thread's own drain reclaims to zero, ring-full
     falls back to the trap.  Rings leak on thread exit (bounded by
     thread count; note it in the code).
  3. **Name the memcpy 6.7%**: game streaming vs marshal copies --
     annotate call sites before optimizing either.
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
* `mfmediaengine`, `evr`, `wmvcore` have a built COM surface no title has
  driven.
* The callback audit cannot see a callback that arrives inside a **struct**
  (a `WNDPROC` in a `WNDCLASSEX`); those rows carry handlers and are found the
  hard way.  If a future crash names one, add it to
  `check-callback-rows.sh`'s reasoning rather than only to the table.
