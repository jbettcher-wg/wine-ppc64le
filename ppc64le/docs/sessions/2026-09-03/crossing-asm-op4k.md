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
4. Layer count: eight frames between the EC trampoline and the export for a
   zero-argument leaf.  A resolved cell already knows sig/fp/cb/com; a leaf
   class that skips `call_user_mode_callback`'s 48-register save and the
   CONTEXT shell is the next structural step, and the only one that gets
   under ~150 ns.
