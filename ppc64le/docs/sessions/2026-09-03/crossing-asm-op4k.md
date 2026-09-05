# The crossing on POWER8, instruction by instruction -- and the three asm changes that took a quarter off it

**[MEASURED 2026-09-03, op4k (POWER8, 3.49 GHz), games' build-smc bridge (ABI 6 view + ABI 7 EC live), `ppc64le/cpu/bench-crossing.sh`.]**
Every crossing number in this tree before today was from the AC922 POWER9.
This is the first POWER8 characterisation, and it changes the model.

## 1. What a crossing costs here, in hardware units

`perf stat` over 20M crossings (startup-subtracted against a 200k run):

| per crossing | stock |
|---|---:|
| ns (guest QPC clock) | 313.1 |
| cycles | 1,171 |
| instructions | 1,251 |
| branches | 147 |
| **branch mispredicts** | **26.0** |

Instructions by DSO: ntdll.so 48.6%, JIT'd guest 21.9%, PE ntdll.dll.so
16.1%, libc memcpy 7.5%, libfexbridge 5.9%.  Mispredicts are spread
everywhere at ~18% of branches -- the predictor is not tracking this loop
at all, so every branch costs its static bias.  **Latency is mispredict
bound, not instruction bound**: `perf annotate` charges the samples to the
instruction after each stall (`ld r2,24(r1)` after every `bctrl`, `addis
r2,r12` at every global entry), which is skid, not the load.

The docs' "68 ns EC floor" is the bridge microbench; the wine side is
~two-thirds of the real crossing.

## 2. Three changes, measured one at a time (fixed harness, 3 interleaved rounds each, gates green)

| variant | cyc/x | ins/x | br/x | miss/x | ns/x |
|---|---:|---:|---:|---:|---:|
| stock | 1173 | 1250 | 147 | 26.0 | 314.6 |
| A. no `-mlongcall` on the unix side | 946 | 1142 | 137 | 18.9 | **272.5 (-13%)** |
| B. inline gregs copies in `emu_view_dispatch` | 984 | 1072 | 106 | 23.0 | 284.0 (-10%) |
| C. no FPR/VR reload in `emu_trap_return_direct` | 1065 | 1145 | 135 | 25.0 | 306.9 (-2.5%) |
| **A+B+C** | **820** | **984** | **104** | **16.9** | **237.2 (-25%)** |

**A.** `configure.ac` put `-mlongcall -mno-pltseq` in EXTRACFLAGS for the
whole tree to guard PE callers of winebuild import thunks (the r2-poisoning
case its comment records).  The unix libraries have no import thunks; there
the flag turns every call -- including calls to `static` functions in the
same file -- into `addis/addi/mtctr/bctrl` + a TOC reload, and the callee
enters at its global entry.  The 7 mispredicts it removes say the indirect
sites were thrashing POWER8's count cache.  Shipped as `-mno-longcall` in
UNIXDLLFLAGS, which makedep appends after EXTRACFLAGS for FLAG_C_UNIX
objects only.  PE modules, the .so builtins (PE ntdll included), tools and
the server keep the guard.

**B.** Two `memcpy(..., 128)` per crossing were PLT calls into libc under
`-fno-builtin` (9.4% of samples, all from this function).  Spelled as a
16-doubleword loop the compiler keeps them inline.

**C.** The lean return reloaded f14-f31/v20-v31 from the callback frame.
Its own guards make that dead: with `restore_flags == 0` nothing stashed a
context, and the stub is reached by an ordinary call at the normal end of
the dispatch, so every callee has returned and the registers hold the entry
values -- provided the two frames still live (`emu_trap_dispatch`,
`emu_exception_dispatch`) use none.  `check-lean-return-fpvr.sh` proves that
against the built object (2848 + 132 instructions, zero non-volatile FPR/VR
sites), keeps the syscall route's 30 loads, and checks its own detector
against call_user_mode_callback's 18 + 12 saves.

## 3. Closed negatives, so nobody re-runs them

- VMX `stvx`/`addi` chain scheduling on POWER8: four byte-identical
  encodings, 13.2 -> 12.2 cycles per 12 stores -- store-port bound, 0.3 ns.
- The FP/VMX entry save gate: `2026-08-29/fpvmx-save-gate.md` stands.
- A harness that `cp`s over a mapped ntdll.so crashes the prefix's
  services.exe/rpcss.exe (same inode rewritten under them); a shadow tree
  with a symlinked preloader loads the REAL tree's ntdll.so and measures
  stock five times.  Both happened; `strace -e openat` before trusting a
  number.

## 5. And in a game: Cyberpunk `-benchmark`, the crossing is not the frame

Same box, same night, stock tree vs the full patched build, interleaved legs
on a headless weston/Xwayland (SMT=8, ondemand -- both arms identical, not
the tuned config):

| leg | stock avg fps / floor ms | patched avg fps / floor ms |
|---|---:|---:|
| 1 | 20.738 / 33.68 | 20.612 / 33.43 |
| 2 | 20.750 / 33.67 | 21.037 / 32.16 |
| 3 | 20.545 / 32.46 | 20.864 / 32.11 |
| mean | **20.68 / 33.27** | **20.84 / 32.57** |

+0.8% average, -2% floor: inside the +-0.3 fps leg-to-leg spread.  A 25%
cheaper crossing, ~77 ns off each of the ~55k crossings a floor frame makes
(~4 ms of a 33 ms frame if they were on the critical path), buys nothing
visible.  That is the same verdict the PeekMessage fast path got
(`2026-08-30/peek-fastpath-impl.md`, f*lambda ~ 0), now from the other
side: the GameThread's crossing time is waiting in disguise -- a poll that
returns faster just polls again.  `frame-cost-budget.md` §4's "~50% of the
floor is crossing machinery" is true as a sample count and false as a
lever.  What is left for the frame rate is what the GameThread waits FOR
(the redDispatcher handoff -- `2026-08-31/ntsync-fastpath.md`) and the JIT
share.  The crossing work still stands on its own: every syscall and every
API the port serves is a quarter cheaper, and the next title whose main
thread really does cross on the critical path (a UI-heavy one, a D3D9 one)
sees it directly.

## 6. Witcher 3, headless, both arms under the same instrumentation

The first stock leg was launched without `FEX_GUESTSERIALIZE` and died
loading the save (guest c0000005 at `witcher3.exe+0x14a8813`, a write to
`0x0001_0000_3ff55a89` -- the allocator race the TLSF campaign serializes
around; log `nw-witcher3/wine-ppc64le-native-20260903-171807-389864.log`).
That is the stock tree, before any change here.  Both legs below carry the
16-pair spec `~/fex-scripts/w3ab.sh` exports, so the caveat that spec
brings (a global spinlock on the free paths, no code-cache reuse) is
identical on both arms.

First pair (stock then patched, newest save via CONTINUE, 200 s sampling
window after an 8-minute stream-in, MangoHud CSV):

| leg | window avg fps | scene p50 / p95 ms | in-game |
|---|---:|---:|---|
| stock | 13.56 | 71.9 / 85.3 | 9:32 AM, rain |
| patched | 15.06 | 65.7 / 73.9 | 11:55 AM, clear |

+11%, but CONFOUNDED: the stock leg's ten minutes in-world left a newer
autosave, so the patched leg loaded a different clock and weather.  A
third leg (patched, on the next autosave: 2:09 PM, clear, camera turned)
measured 12.91 fps in the same window -- scene-to-scene spread on this
save is +-10%, wider than anything the port did.

**Two lessons paid for here.**  CONTINUE picks the newest save by its
internal timestamp, so touching an mtime does not pin a save.  And the game
keeps THREE AutoSave_* files and deletes the oldest on each write: three
headless legs deleted BOTH of the owner's autosaves (4a26019, 4952d9e; the
QuickSave and CheckPoints survived; `~/w3-gamesaves-backup-20260903-182049`
holds the directory as it stood afterwards).  Any W3 leg must back the
directory up first and move its own autosaves out before the next leg
(`RESTORE_SAVES_SINCE` in the runner) -- which is also what makes the
clean pair below load the SAME save on both arms.

Clean pair (same autosave on both arms, verified by clock/camera/NPCs in
the loaded screenshots: 4:11 PM, clear, the cart-wheel house; stock first,
each leg's new autosave moved out before the next):

| leg | window avg fps | window p50 / p95 ms | scene fps (>25 ms frames) |
|---|---:|---:|---:|
| stock | 12.30 | 79.1 / 95.4 | 14.30 |
| patched | 12.49 | 80.1 / 87.8 | 14.36 |

+1.5% on the window average, p50 within a millisecond: a wash, as the
81%-JIT profile predicts (the crossing is ~7% of that thread; a quarter of
7% is under the scene noise).  Both games say the same thing from opposite
ends: the crossing work is real and measured at the crossing, and neither
title's frame rate is set by it today.

## 7. Two more legs the same night: the pin is a wash, the governor is not

Asymmetric SMT (cpus 1-7 offline, core 0 in ST mode, GameThread pinned to
cpu 0, every other thread on 8-159) against the plain SMT8 box, patched
tree both arms, Cyberpunk `-benchmark`:

| leg | governor | avg fps | floor | p50 | p99 |
|---|---|---:|---:|---:|---:|
| pin 1 | performance | 23.82 | 30.0 | 38.5 | 89 |
| ctl 1 | performance | 23.54 | 30.9 | 38.8 | 90 |
| pin 2 | ondemand | 17.67 | 40.4 | 50.2 | 141 |
| ctl 2 | ondemand | 19.19 | 36.5 | 46.8 | 138 |
| pin 3 | performance | 22.27 | 31.2 | 41.9 | 107 |
| ctl 3 | ondemand | 18.65 | 35.1 | 48.5 | 136 |

The frame thread alone on a whole core buys nothing under `performance`
and loses under `ondemand`: "the critical thread is crowded on its core"
is falsified as tested.  SMT2's +9% (NEXT.md item 6) lives on the worker
side.  What the six legs DO show is the governor: every `performance` leg
22-24 fps, every `ondemand` leg 18-19, p99 89 vs 136 ms -- +20-25%, far
above the +3% the 08-2x matrix recorded.  Under ondemand the clock sat at
2.93 GHz mid-benchmark (max 3.49): 25 idle-spinning workers never present
the load shape that makes it boost.  Make `performance` stick.

## 8. Items 1-3 from the list: 236 -> 219 ns

Same night, in the scratch clone, one at a time on the microbench and
together in the game:

- **PE ntdll.dll.so off `-mlongcall`** (makedep NATIVE_EXTRACFLAGS, set
  from configure's NTDLL_NATIVE_CFLAGS): emu_trap_dispatch 141 -> 11
  bctrl, 132 direct bl.  Sound because ntdll imports nothing -- no import
  thunk, no direct bl can leave the module.
- **NtCurrentTeb inline for ntdll's own PE code** + the two thunk_arg lever
  readers as a load and a predicted branch: 0 TEB calls left in
  emu_trap_dispatch, the readers gone from the profile.  The 3.4% of
  NtCurrentTeb still in the bench profile is kernelbase's
  GetCurrentProcessId body reading the TEB through the export -- every
  other module still calls; an exported TLS offset read through the IAT
  would make it three loads and no branch, parked.
- **YieldProcessor = `or 27,27,27`** (the SMT yield hint) instead of an
  empty asm; the ntsync spin already used HMT low.

Crossing: **236 -> 219 ns** (three legs 225.6 / 219.1 / 217.9; 219.8 over
20M).  Cyberpunk `-benchmark`, 3+3 interleaved, box being used (sunshine
streaming, governor flipped mid-run): stock 19.59 / 18.53 / 21.70 vs
patched 18.72 / 18.54 / 22.86 -- inside the +-5% the box state was
swinging; the pair that ran wholly on `performance` was +5%.  No
regression, no visible win, as the crossing share predicts.
`ID3D12PipelineLibrary::Load*` turned out to be served already (three
hand walkers; 594 loads vs 48 fresh creates in the census) -- STATUS.md
was stale, and the hitches are not PSO creation.

## 9. The next four: 219 -> 202 ns

- **Every PE module reads the TEB inline** (winnt.h): ntdll exports the
  thread-pointer offset of its TEB thread-local once
  (`__wine_ppc64_teb_tls_offset`); each module keeps a weak hidden copy
  and fills it on first use, so NtCurrentTeb is two loads and a predicted
  branch.  kernelbase's GetCurrentProcessId: 0 calls left.  Two build
  lessons on the way: a `-private` export is not in the import library,
  and the per-module copy has to be a weak tentative definition in the
  header -- a winecrt0 archive member is not pulled by every exe.
- **emu_trap_dispatch split**: a 184-instruction leaf fast path with zero
  non-volatile saves for a resolved EC cell naming a plain flat export
  (no COM, override, FP, callback, variadic; nothing armed, no TRACE);
  everything else takes the full 2,800-instruction body unchanged.
- **wineserver off `-mlongcall`** (same per-makefile hook): 8,432 direct
  bl, 89 bctrl left.  A server round trip is ~16 us of socket and
  scheduling on this box (`ppc64le/cpu/bench-server.sh`, new: one
  CreateEventW + CloseHandle = 33.3-33.8 us on the old server, 32.6-33.5
  on the new) -- real, small, and not where games spend time (76
  NtCreateEvent/s in the census).

Crossing: **219 -> 202 ns** (201.7 / 202.3 / 202.5; 203.5 over 20M), gates
green including check-rip-cache.  The day's total: 313 -> 202, -35%.

## 10. Two C layers fold into the bridge-facing thunks: 202 -> 192 ns

emu_view_dispatch and emu_trap_dispatch_common were two of the four C
frames between FexBridgeEcTrampoline and call_user_mode_callback, each a
prologue, an epilogue and an argument shuffle per crossing (8.8% + 11.6%
of the samples as separate symbols).  FORCEINLINE'd into emu_ec_thunk and
emu_trap_view_thunk (372 instructions, one frame), with the guest-context
publish's once-per-process env check moved out of line so it inlines
too.  [MEASURED] 192.0 / 191.6 / 191.1 ns; gates green including
check-rip-cache.  Day total: **313 -> 192 ns, -39%**.

**And the trap entry itself folds in** (call_emu_trap_dispatcher_inline in
unix_private.h, with struct syscall_frame lifted there and the refusal,
the missing-dispatcher error and the two levers out of line in
signal_ppc64.c): emu_ec_thunk is one 444-instruction frame from the
bridge trampoline to the stack switch.  [MEASURED] 184.0-188.5 vs
191.6-193.3 back to back, ~-3.5%.  **Day total: 313 -> 185 ns, -41%.**
Three Cyberpunk legs on the final tree, ondemand and streaming: 19.44 /
19.56 / 19.56 -- the ondemand band, unchanged.

Not done from the list: narrowing call_user_mode_callback's FP/VMX entry
save.  It exists for a mid-dispatch SuspendThread's GetThreadContext, and
the signal's own ucontext cannot stand in for it (a native callee may have
parked a callee-saved register in its frame by then) -- the same argument
as 2026-08-29/fpvmx-save-gate.md, unchanged by the EC path.

## 11. The EC leaf path: 185 -> 131 ns for the exports that cannot syscall

Item 4 below, built (2026-09-04).  A transitioned call whose resolved row
names an export that cannot make a syscall, raise, or call back into the
guest needs none of what `call_user_mode_callback` exists for: no callback
frame, no 18 GPR + 18 FPR + 12 VR entry save, no Win32-stack switch, no PE
dispatcher frame, no lean return.  So the unix EC thunk now calls a PE
entry (`emu_trap_leaf`) as an ordinary function on the kernel stack it is
already on, and that calls the export.  Three pieces:

- **The class** is an allowlist by (module, export) --
  `thunk_leaf_exports` in signal_ppc64.c -- each row checked against the
  native body in this tree: the TEB/PEB readers (GetCurrentProcessId,
  GetCurrentThreadId, GetCurrentProcess/Thread, GetLastError,
  SetLastError, GetProcessHeap, IsDebuggerPresent, TlsGetValue,
  FlsGetValue), the user_shared_data readers (GetTickCount, GetTickCount64)
  and QueryPerformanceFrequency, on kernel32, kernelbase and ntdll.  The
  file says why TlsSetValue, lstrlen, GetSystemTime*, QueryPerformanceCounter
  and SwitchToThread are NOT in it.  Stamped at resolve into the cache
  entry, published through the EC cell as a fourth state value
  (`EC_CELL_LEAF` = 5).
- **The decline is one load on the unix side**: the cell's state word is
  the only thing the unix side reads out of a cell (`EMU_EC_CELL_LEAF`,
  unixlib.h).  The first build called into PE to decline and that cost
  every non-leaf crossing +14 ns (215 -> 229 on the new `nonleaf` bench
  line); with the pre-check the non-leaf line is back on stock.
- **What the leaf still does**: the crossing-log push/pop and the TRAP
  publication (a mid-call SuspendThread reads the same shell the full path
  publishes; a fault inside the callee is reported against this crossing),
  the trap-ctx pair around the call, and the run-ending flag reads.  What
  it skips, and why that is safe only for the class: the kernel-stack depth
  check (a leaf nests nothing), the TEB stack-limit swap (a leaf probes no
  stack and raises nothing), the callback frame (no syscall ever looks for
  one).

`bench-crossing.sh` grew a `nonleaf` line (IsProcessorFeaturePresent: a
plain flat thunk, a trivial body, one argument, not in the class) so the
two paths are measured side by side from the same probe.  [MEASURED],
three interleaved rounds, stock / leaf on / leaf off by the kill switch:

| line | stock | leaf on | `WINE_PPC64LE_NO_EC_LEAF=1` |
|---|---:|---:|---:|
| crossing (GetCurrentProcessId, a leaf) | 185.7 / 189.4 / 184.9 | **131.3 / 132.3 / 131.0** | 187.9 / 186.6 / 187.0 |
| nonleaf (IsProcessorFeaturePresent) | 215.0 / 219.0 / 214.3 | 217.1 / 218.8 / 216.8 | 216.4 / 216.3 / 215.5 |

**A leaf crossing: 185 -> 131 ns, -29%; day total for that class 313 ->
131, -58%.**  A non-leaf crossing: unchanged inside the round-to-round
spread.  Gates green in both modes: the new `check-ec-leaf.sh` (pid, tid,
last-error, TLS and tick-count value checks; sabotage flips RAX on every
leaf-served call and the kill switch must lift it -- 1999 of 2000 wrong
under sabotage, the one being the cell-filling transition), plus
check-ec-transition, check-lean-return-fpvr, check-lazy-ctx,
check-rip-cache, check-cs-fastpath, check-peek-fastpath.
(check-qpc-fastpath's "interval disagrees" leg fails on the stock tree
too, 0.35% clock drift under ondemand; not this change.)

One bug on the way, caught by check-cs-fastpath and worth its line: the
RIP cache's seqlock copies name every field by hand, and the new `leaf`
byte was in neither the get nor the put for one build.  The cell then took
a stack byte for its leaf bit and served wsprintfA -- a variadic -- as a
leaf, without its va_list.  The struct now says so next to the field.

One Cyberpunk `-benchmark` leg on the leaf tree, headless weston with the
GL renderer (`--backend=headless --renderer=gl --xwayland`; the default
renderer gives an Xwayland vkd3d cannot present to), box quiet: ran to the
end, rc 0, 24.29 avg / 17.46 min fps over 1607 frames, and the same two
pre-existing err lines the stock tree's 01:26 log has (one guest thread
ending c000001d holding fls_section, mfc140u missing for the Chroma
plugin).  A smoke run, not an A/B: the box's legs tonight sit anywhere in
19-24 fps by governor and load, and this one was not paired.

**In games, expect nothing visible.**  The census (crossings-cp2077-
benchmark.txt) puts the leaf-class exports that still cross at ~8k/s
(GetLastError + SetLastError 5.1k, FlsGetValue 1.8k, TlsGetValue 0.8k,
GetTickCount 0.2k); GetCurrentThreadId and QueryPerformanceFrequency
already have guest-side fast bodies and never cross.  ~55 ns off 8k
crossings a second is 0.04% of a second.  The value is structural: the
crossing floor for an export is now the bridge's own trampoline plus ~45
ns of wine, and the class can grow (any export proven syscall-free,
raise-free and callback-free is one table row).

## 12. The COM fast arm: a resolved COM slot skips the full dispatcher body

Built 2026-09-04, evening, off the Witcher 3 render-thread profile
(session doc 2026-09-04): with the proxy lock gone, `emu_trap_dispatch_slow`
was 5.8% of the thread, and every D3D11 call was taking it because the fast
arm of `emu_trap_dispatch` excluded any row with a COM dispatch.  The full
body is 2,800 instructions and saves sixteen non-volatile GPRs on entry; a
COM slot needs none of that -- the module's `__wine_com_dispatch` owns all
marshalling, this side only pops the return address.

- **The arm**: `call_resolved_com_slot` (signal_ppc64.c) is the COM arm
  as one function, used by both paths: the trap-ctx pair around the
  dispatch, the call, the pop on success, `STATUS_ILLEGAL_INSTRUCTION` on a
  refused dispatch.  The fast path takes it when the cell is resolved, the
  lean return is in hand, nothing is armed that wants to watch (xstat,
  `TRACE_ON(seh)`), and the lever is not pulled.  The run-ending status
  logic became `trap_dispatch_run_status_from(status)` so the arm's own
  status composes the same way the full tail does.
- **Levers**: `WINE_PPC64LE_NO_COM_FAST=1` (every COM slot takes the
  full body -- yesterday's path exactly), `WINE_PPC64LE_COM_FAST_SABOTAGE=1`
  (RAX flipped after every fast-served dispatch).  Banner
  `com fast arm live` once per process.
- **Gate**: `check-com-fastpath.sh` -- the system-COM smoke's guest
  binary against its native output: live = banner + byte identity; kill
  switch = no banner, identity holds; `--sabotage` = identity MUST break
  (it breaks at step 11, `IUnknown::Release` refs=0 -> 1), and
  `NO_COM_FAST` on top must restore it.  Both legs PASS on op4k, plus
  check-ec-leaf, com-smoke, com-levers, reverse-proxy, blob-surface,
  d3d11-smoke, d3d11-smoke32.
- **Bench**: `bench-com-crossing.sh` / `probes/com_bench.c` -- the COM
  sibling of bench-crossing: `ID3D11Device::GetFeatureLevel` and
  `ID3D11DeviceContext::GetType` through a real DXVK device, 200k calls
  each, guest QPC clock.  This is the first direct number for what a COM
  slot costs on this port.

[MEASURED], three interleaved rounds:

| line | fast arm on | `WINE_PPC64LE_NO_COM_FAST=1` |
|---|---:|---:|
| com_getfeaturelevel | **360.7 / 361.1 / 360.5** | 370.0 / 370.0 / 369.3 |
| com_gettype | **361.1 / 360.5 / 360.0** | 369.4 / 369.3 / 368.6 |

**A COM slot call: 369.5 -> 360.6 ns, -9 ns (-2.4%).**  That is the
sixteen-register prologue/epilogue and the full body's branches, and it is
all the arm can take: the rest of the 5.8% in the profile is the part both
paths still share.  Honest reading for the render thread: ~9 ns off each of
the ~30k D3D11 calls a frame is under 0.3 ms of a 52 ms frame.

**What the number says about the next cut.**  A COM slot costs 360 ns
against 215 for a non-leaf flat export (bench-crossing's `nonleaf` line)
and 131 for a leaf.  So ~145 ns is COM on top of the trap, and a perf of
the bench (`/tmp/combench.perf` on op4k, `--comm com_bench.exe`) splits
one call roughly: JIT'd guest loop + trap stub 4.3, PE d3d11.dll code
(winecom_dispatch and its marshal, unsymbolized as `[JIT]` at
0x3fffff...) 4.7, ntdll.so 3.5 (emu_ec_thunk 1.4,
__wine_unix_call_dispatcher 0.9, call_user_mode_callback 0.5,
syscall dispatcher 0.35, emu_trap_return_direct 0.3), bridge 0.7, PE
ntdll 0.7, d3d11.so 0.5, DXVK 0.15 (percent of all samples).  Two things
are the size of the whole trap: the PE winecom layer, and the second
transition (`__wine_unix_call_dispatcher` saves the same 18+18+12
register set the callback frame just saved, 60 ns ahead of a DXVK body
that returns a field).  Either the unix EC thunk reaches d3d11.so without
the PE round trip -- which means the marshal tables and proxy objects
winecom keeps become readable from the unix side -- or the PE side gets
a cheaper unix entry for a call it can prove does not raise, syscall or
call back (the leaf argument again, one layer down).  Both are a session.

## 13. The context journal: D3D11 immediate-context calls recorded guest-side

Built 2026-09-04, night.  Section 12 ends with two cuts inside the trap;
this one takes the hot D3D11 class out of the trap instead.  The first
journal (2026-08-27) hand-encoded one snippet per shape for eight D3D12
command-list slots.  The shapes are now a table (`libs/winecom/
journal_gen.h`): a row names the slot and classes each argument as a
value, a pointer to N bytes, or a counted array with a cap, and one
generator emits the x86-64 snippet -- ring checks, count guards before any
store, the arguments as they arrived, the blobs copied in, pos published.
Sixty-eight ID3D11DeviceContext/Context1 rows are curated: every bind,
viewport, scissor, blend, clear, copy, draw, dispatch, Begin/End, Unmap,
ClearState.  Left trapping on purpose: UpdateSubresource (unbounded
payload), ExecuteCommandList, Flush, Map, every Get, the XMM rows.

- **Scope.**  A command list has one recorder and is drained at its own
  trap.  An immediate context's state is observable through every other
  object of the surface (a Release of a bound view, a Present, a Map), so
  its ring is registered and drained at EVERY dispatch: two loads per
  quiet ring.  Any thread may replay, under `jr_cs`, from a native `cons`
  to the guest's `pos`; only a trap on the context itself resets the ring
  (its writer is the thread sitting in that trap).  A full ring falls
  back to the slot's trap, which is such a reset.
- **One ring per HOST object, not per proxy.**  The first build gave
  each proxy its own ring, and Witcher 3 rendered smeared geometry over a
  black screen: the game QIs the immediate context for
  ID3D11DeviceContext1 and calls SetConstantBuffers1 through it while
  drawing through the base pointer, so two proxies of one object recorded
  into two rings and the drain replayed one after the other -- constant
  buffers bound after the draws that needed them.  Rings now live in a
  pool keyed by the host pointer (struct ctx_ring; the snippet reads pos
  and cap from the ring header), every proxy of the object shares one,
  and each record carries the proxy it came through.  The gate's probe
  now interleaves writes through both proxies and requires last-writer-
  wins and exactly one armed ring.
- **Multithread protection** (ID3D11Multithread::Enter / SetMultithread-
  Protected) would let a foreign thread's replay take DXVK's lock under
  `jr_cs` while the lock holder waits on `jr_cs`: the first dispatch of
  either row replays and detaches every context ring for the process.
- **Levers**: `WINEEMUNOCOMJOURNAL=1` (kill, every call traps),
  `WINEEMUCOMJOURNALSABOTAGE=1` (record, never replay).
- **Gates**: `check-ctx-journal.sh` -- on an x86-64 host it first
  generates and EXECUTES all 68 snippets natively against a fake ring
  (`probes/journal_gen_host.c`: record bytes, NULL pointers, over-cap and
  full-ring fallbacks with every register intact); then the guest probe
  (`probes/ctx_journal_probe.c`) sets state through the journal and reads
  it back through Gets and a texel readback across a box copy whose
  arguments travel on the stack; then the trace must show replays and NO
  live dispatch for the driven rows.  `--sabotage`: never-replay makes the
  probe FAIL, the kill switch makes it PASS with the rows dispatching
  live.  All PASS on op4k first run, plus d3d11-smoke, com-fastpath,
  com-levers, reverse-proxy, blob-surface, dev-journal.

[MEASURED], `bench-com-crossing.sh`, new line `com_journaled_topology`
(IASetPrimitiveTopology, 200k calls, the closing IAGetPrimitiveTopology
inside the timed region so every replay is counted), three interleaved
rounds, box under its evening governor (the trapped slots read ~405 here
against 360 in section 12 -- same tree, slower clock):

| line | journal on | `WINEEMUNOCOMJOURNAL=1` |
|---|---:|---:|
| com_journaled_topology | **239.9 / 234.5 / 233.9** | 416.1 / 419.5 / 418.5 |
| com_getfeaturelevel (not journaled) | 414.9 / 407.7 / 405.1 | 402.2 / 406.1 / 404.4 |

**A journaled D3D11 call: 418 -> 235 ns, -44%.**  Not the -90% a
"no crossing" reading promises, and the reason is the honest number of
this section: the trap is gone but the REPLAY still pays PE winecom's
marshal per record plus one `__wine_unix_call_dispatcher` transition per
record (its 48-register save, ~30-40 ns) before DXVK's ~30 ns body.  The
record itself is ~10 ns.  A non-journaled slot is unchanged inside the
spread (the drain-all walk is two loads per registered ring).

**In the game.**  With the shared-ring fix in, the user played Witcher 3
outside Novigrad on the fixed build: 16 fps where the same area had been
running 7-12 fps on the stock tree (user's own reading of the MangoHud
overlay, 2026-09-04 night).  One scene, one leg, not a pinned-save A/B --
but the first port change whose effect a player could see, and the
direction the 30k-calls-a-frame arithmetic predicted.

**What the number says about the next cut.**  The replay is now the
cost, and it has two halves the trap never had a way to remove: N
transitions for N records, and N trips through `invoke_marshalled` for
rows whose classes are all pass-through or proxy.  A batch replay --
the PE side unwraps proxies and writes native call descriptors for the
whole ring, ONE unix call executes them in d3d11.so -- removes both:
estimate ~235 -> ~80 ns per journaled call.  In a game the count is what
matters: Witcher 3's render thread makes ~30k of these calls a frame, and
its ring drains at the next Map, which is per draw in most engines; the
A/B on a pinned save is the next thing to run, with the section 6 caveats
(back the saves up, +-10% scene noise).

## 14. EC DIRECT: the JIT makes the call

Built 2026-09-05 (early), NOT COMMITTED -- working trees on both sides,
for review.  Section 13 took the hot D3D11 class out of the trap by
recording it; this takes the trap itself out of every call that still
crosses and whose body takes its arguments as they are.

**The mechanism.**  A registered stub RIP already compiles to a
transition block (bridge ABI 7): spill the guest file, call the bridge
trampoline, reload RIP, exit.  The block is now `EcTransition`
(FEXCore IR.json, PPC64 `DEF_OP(EcTransition)`, BranchOps.cpp): after
the same spill it reads the slot's EC cell AT RUN TIME -- the block is
compiled before the slot is ever resolved -- and when the cell's state
word carries the DIRECT bit it serves the call inline:

1. dirty byte named by the digest must read 0 (winecom's "records
   pending somewhere", set by every context-journal snippet, cleared by
   the drain before it scans);
2. positions 0..7 loaded from the frame (RCX, RDX, R8, R9) and the guest
   stack (RSP+0x28..) into r3..r10;
3. COM: `this` must be a live proxy of the digest's interface, its ring
   (if any) quiet -- list position 0, shared header pos == cons -- and
   the host object is read off it; interface arguments named by the
   digest's mask are unwrapped the same way after their vtable is
   checked against the guest vtable block;
4. per-position extension (zero/sign 8/16/32, from the row's narrow and
   dword masks, or the flat export's width/sign words);
5. the callee: the host vtable slot (COM) or the export itself (a leaf,
   FLAT), called ELFv2 with the in-flight marker set around it;
6. r3 to RAX, [RSP] to RIP, RSP += 8, the partial refill (the callee
   never touches the frame, the sentinel proves it), exit.

Any check failing falls to the trampoline path inside the same block --
DEF_OP(Thunk)'s body verbatim, sentinel disarmed first.

**The contract** (fexbridge.h "EC DIRECT calls"): the digest is the
64-byte `struct fexbridge_ec_direct` at cell + 8 (kind, nargs, slot,
iface, fn, dirty, vt_lo, vt_size, in_mask, ext[8]); the state word's bit
3 says it is published (9 = DIRECT|RESOLVED for a COM slot, 13 =
DIRECT|LEAF for a flat export, so the unix leaf pre-check still accepts a
direct leaf's fallback).  COM proxy offsets the JIT reads: host 0x08,
iface 0x14, ring 0x28, list pos 0x30, live 0x44; ring header pos 0x00,
cons 0x10 -- every one pinned by a C_ASSERT in winecom.c; the digest
layout by C_ASSERTs in loader.c and signal_ppc64.c.  Two optional bridge
symbols, no ABI bump: `fexbridge_ec_direct_in_flight`,
`fexbridge_fault_unwind_direct`.

**Who proves a slot.**  ntdll's `ec_cell_fill` asks the slot's native
module for `__wine_com_slot_direct` (d3d11, d3d12; dxgi/d3d10core/d3d10
forward to d3d11), which is winecom's `winecom_slot_direct`: slot >= 3,
not refused, not a hand function, no float, no aggregate return, every
argument PASS or IFACE_IN, at most 7 after `this`, and its vtable entry
never re-pointed at a snippet (journal, const getter) -- a snippet's
fallback trap must keep the dispatcher because it drains.  A flat export
is direct when it is a leaf (thunk_leaf_exports) with at most 8
arguments.  Not under WINE_PPC64LE_TRAP_STATS, +seh, +relay, +snoop, and
not for a class whose older lever is pulled (NO_EC_LEAF / EC_LEAF_SABOTAGE
turn off flat direct, NO_COM_FAST / COM_FAST_SABOTAGE turn off COM
direct), so those gates' negative controls stay red.

**Levers**: `WINE_PPC64LE_NO_EC_DIRECT=1` (PE half: no cell stamped),
`FEX_NO_EC_DIRECT=1` (bridge half: plain trampoline blocks),
`WINE_PPC64LE_EC_DIRECT_SABOTAGE=1` (RAX inverted after every direct-
served call).  Banner `ec direct arm live` once per process.

**Gate** `ppc64le/cpu/check-ec-direct.sh` (`probes/ec_direct_probe.c`):
pid/tid against the TEB, 1000 x GetFeatureLevel and GetType, a format
query through a pointer, GetData with a query proxy to unwrap and a UINT
on the stack to extend, a Map/Unmap/Copy/Map round trip.  Live: PASS +
banner.  `--sabotage`: RAX inverted must FAIL it (it did: pid, feature
level, type, the second CheckFormatSupport and the second GetData all
went red); each kill-switch half must PASS, the bridge half with the
banner still present; sabotage under the bridge half must be inert.
All PASS.  Also PASS with the arm live: check-com-fastpath (+ --sabotage),
check-ec-leaf (+ --sabotage), check-ctx-journal (+ --sabotage),
check-d3d11-smoke, check-dev-journal, check-com-smoke.

[MEASURED] op4k, three interleaved rounds, live vs
`WINE_PPC64LE_NO_EC_DIRECT=1`; the third column is the same box before
any of this was built (the section 13 tree, an hour earlier):

| line | direct live | kill switch | before |
|---|---:|---:|---:|
| com_getfeaturelevel | **69.8 / 69.6 / 69.4** | 367.2 / 367.4 / 368.2 | 408.8 / 413.3 / 408.0 |
| com_gettype | **70.8 / 70.4 / 70.7** | 371.1 / 371.3 / 370.7 | 411.9 / 410.6 / 410.0 |
| crossing (GetCurrentProcessId, leaf) | **77.7 / 77.8 / 78.0** | 128.7 / 128.6 / 128.6 | 130.1 / 130.9 / 129.7 |
| nonleaf (IsProcessorFeaturePresent) | 214.7 / 215.2 / 215.4 | 214.8 / 215.1 / 215.3 | 214.2 / 215.6 / 213.3 |
| com_journaled_topology | 263.4 / 260.5 / 262.8 | 266.3 / 262.7 / 261.1 | 255.0 / 253.1 / 254.2 |

**A direct COM slot: 368 -> 70 ns, -81%; a direct leaf export: 129 ->
78 ns, -40%.**  (The kill-switch arm reads 40 ns under the hour-earlier
baseline on the COM lines; same tree otherwise, the box's clock -- not
claimed.)  What the 70 ns still holds: the block entry and exit, the
full spill and the partial refill (~26 register stores and ~16 loads),
the digest and proxy checks, DXVK's 30 ns body.  The non-leaf flat line
is untouched, as it should be: IsProcessorFeaturePresent is not a leaf.

**What falls back, and why**: a slot with an interface OUT parameter,
an aggregate return, a hand function, a float, more than 7 arguments;
IUnknown's three (served from the proxy table); any slot behind a
snippet; any call while a journal ring is pending -- the dispatcher's
drain is what orders the rings, and the arm must not skip it; a `this`
whose live tag, interface or ring says no; an interface argument whose
vtable is not in the block.

**Open risks, plainly.**
- Faults inside the callee: the in-flight marker is set around the
  call; emu_handle_fault (loader.c) sees a non-JIT host PC with the
  marker up, logs it, and unwinds through `fexbridge_fault_unwind_direct`
  to the run's FAULT return at the guest call site with the spilled file
  -- the same attribution the leaf path has.  Built, not exercised by a
  gate: no probe faults inside DXVK on purpose yet.
- Memory ordering: the JIT reads the cell's state word and then the
  digest; the PE side publishes digest first, state last (WriteRelease).
  The JIT's reads are plain loads in program order; on POWER a dependent
  load ordering is not guaranteed without a barrier, so a first direct
  call racing the stamp on another thread could in principle read a
  stale digest.  Cells are stamped once and never changed, and the
  first call of every slot resolves through the trap on the stamping
  thread itself, so the window needs a second thread's first call to
  land inside the stamp -- add an `isync`/`lwsync` after the state load
  if that ever matters.
- Threads: the direct call runs DXVK on the guest thread's host stack,
  exactly where the trampoline path ran it; nothing new.
- The i386 lane keeps the trap protocol (EC is 64-bit only).
- D3D9 and the media surfaces do not export the digest yet (one spec
  line and one function each, the d3d11 pattern).

## 4. What is still on the table, by measured size

1. **PE ntdll.dll.so still builds with `-mlongcall`** (it is a .so builtin
   on arch 0, so UNIXDLLFLAGS does not reach it).  Its `emu_trap_dispatch`
   is 10% of instructions and 7.5% of mispredicts; `thunk_arg_sign_off` and
   `thunk_arg_width_off` are 3% of instructions and one mispredict per
   crossing for two `static int` flag reads that are not inlined.  Needs a
   per-module flag hook; same soundness argument (ntdll imports nothing).
2. `NtCurrentTeb` is an out-of-line call on PE (2.5%); ntdll's own PE code
   can read `ppc64_current_teb` directly.
3. The JIT side: ~7 mispredicts per crossing in the dispatcher's `bctr`
   (block linking is forced off in the bridge lane -- fastppcx86 5bfad107d).
4. **DONE (section 11)**: the EC leaf class skips `call_user_mode_callback`
   and the PE dispatcher frame for exports that cannot syscall, raise or
   call back -- 185 -> 131 ns.  Still on the table for a leaf: the
   CONTEXT shell itself (two 128-byte gregs copies per crossing) -- a
   leaf reads at most four GPRs and Rsp and writes Rax/Rip/Rsp, so a view
   handler that marshals straight from the live register file would drop
   the copies; and for every crossing, the bridge's trampoline and the
   JIT's ~7 mispredicts per crossing (item 3).
