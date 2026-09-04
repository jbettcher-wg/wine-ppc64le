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
