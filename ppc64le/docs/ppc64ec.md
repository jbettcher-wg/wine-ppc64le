# PPC64EC — feasibility

> **STATUS (2026-08-31, the same day): steps 0, A and B are BUILT and
> DEFAULT-ON.**  fex `4c776150f` (ABI 6 zero-copy trap) + `3e14683cc`
> (ABI 7 EC targets) + `e4fd3b3f1` (full SRA refill for EC callees,
> batch registration); wine `7cd37bc6395` (view consumption) +
> `f922131cc78` (byte-verified stub registration, flat + COM lanes).
> The measured ladder, ns per crossing — bridge floor (BridgeSmoke S13)
> and wine level (`ppc64le/cpu/bench-crossing.sh`):
>
> | | eager | lazy (old prod) | view | **ec** |
> |---|---:|---:|---:|---:|
> | bridge floor | 296 | 145 | 86 | **68** |
> | wine level | 575 | ~430 | ~430 | **~423** |
>
> The bridge-side prediction held (2.1× at the floor).  **Same night,
> both wine-side levers landed too**: row cookies (`582e9c47b2b` + fex
> `0411b2749` — the registration cookie IS the stub's resolved dispatch
> row, `thunk_rip_cache_get` left the crossing) and the lean trap
> return (`49f0feff4d0` — `NtCallbackReturn`'s syscall left it;
> `emu_trap_return_direct` mirrors `user_mode_callback_return` without
> the gate, restore_flags-guarded with the syscall tail as fallback).
> Final ladder on the shipping build against the live bridge, one
> binary, env-var legs:
>
> | leg | ns/crossing |
> |---|---:|
> | old CONTEXT protocol (`WINE_PPC64LE_NO_TRAP_VIEW=1`) | 388 |
> | view, no EC (`WINE_PPC64LE_NO_EC=1`) | 394 |
> | no lean return (`WINE_PPC64LE_NO_LEAN_RETURN=1`) | 408 |
> | **default: view + EC + cookies + lean return** | **365** |
>
> (Legs are not nested — the lean return serves the trap path too, so
> the pre-PPC64EC production crossing, lazy CONTEXT + syscall return,
> sat at ~430–437.)
>
> **2026-09-01 follow-through, all landed the next sitting:** the entry
> half slimmed (`779715d7ed5` — TEB install inlined, per-crossing
> scratch work gone, exception path adopted the lean return; the bench
> crossing fell to **311–314 ns**, and the audit says what remains in
> the entry is the contractual frame work a mid-dispatch suspension
> reads); **step C built** (`cf38a2552d3` — COM-lane FP descriptors,
> one shared splitter/caller, fail-closed everywhere; the mf family's
> 30 float methods incl. the first served FP returns and d3d11's video
> rows stopped being refusals); and the winecom `wc_cs` theory was
> **exonerated on data** (`984c52a6d1d` — every CS caller chain is the
> game's own contended locks crossing to wait, as they must).
> Total journey: **~430 → ~313 ns per crossing, −27%, everything
> default-on.**
>
> **The FP RETURNS are value-driven now, and it took no title.** The
> disclosure this banner used to carry — "no live title has yet
> value-driven the served FP returns" — is retired by measurement
> rather than by waiting.  There are exactly **17 `.fpret` rows in the
> tree, 11 distinct methods**, all in `dlls/mfplat/mf_marshal.h`
> (d3d11's float rows are arguments); **six of the eleven answer on a
> fresh `IMFMediaEngine` with no media, no device and no audio
> endpoint**, and `check-mf-modules.sh` already creates one for the
> reverse-proxy lane.  Four are now gated on **non-zero raw bits** in
> both builds: a fresh engine's `1.0` rate and volume, the quiet NaN
> `0x7FF8000000000000` its `duration` starts at — a pattern the probe
> never produced, so it is pure crossing evidence — and three
> distinctive patterns round-tripped in through the FP argument and out
> through the FP return, one of them negative
> (`0xC00FEDCBA9876543` in, `0xC00FEDCBA9876543` out).  `GetCurrentTime`
> and `GetStartTime` are driven and **printed but not gated**: a fresh
> engine answers `0.0` and a REFUSED fp row also leaves `0.0` in XMM0,
> so a check on them could not tell the mechanism from its absence.
> That is exactly what the new control d demonstrates —
> `WINEEMUNOCOMFP=1` collapses every gated value to
> `0x0000000000000000` and the setters to `E_NOTIMPL`, and the lane
> goes red.  Still unreachable and stated as such:
> `IMFMediaEngineEx::GetBalance` (Wine's is a `0.0` stub, nothing to
> drive) and the four `IMFMediaSourceExtension`/`IMFSourceBuffer` rows,
> which are ONE gap — `CreateMediaSourceExtension` is `E_NOTIMPL`, so
> the object that owns them cannot be constructed.
>
> Still open, none urgent: row-cookie-style adoption for anything the
> profile names next, and the i386 lane stays on traps by measurement.
> Kill switches: `WINE_PPC64LE_NO_TRAP_VIEW=1`,
> `WINE_PPC64LE_NO_EC=1`, `WINE_PPC64LE_NO_LEAN_RETURN=1`,
> `WINEEMUNOCOMFP=1`, `FEXBRIDGE_EAGER_CTX=1`.  Gates:
> `check-ec-transition.sh`, `check-rip-cache.sh` 4b, `check-mf-smoke`
> step 13/control d (the FP ARGUMENT direction) and
> `check-mf-modules.sh` steps 25-32/control d (the FP RETURN
> direction).  The plan below is the original feasibility page, kept as
> written.

The question: do what ARM64EC does — compile the hot PE-side surface as
native ppc64 code carrying x86-shaped exports, so a guest→DLL call is one
cheap transition instead of a marshalled trap — and stop accumulating
per-API fast paths.

## Verdict up front

**Feasible, and most of it already exists in this tree.**  On this port,
PPC64EC is not a new compiler target, not a new file format, and not a
port of Microsoft's design.  Two of ARM64EC's three ingredients are
already built and shipping here; the third — the cheap transition — is a
bridge/JIT change in fastppcx86 plus registration plumbing in wine, and
it has an incremental path where every step is separately measurable and
separately abandonable:

* **Step A (bridge ABI 6, ~a week):** a zero-copy trap flavor — the trap
  callback reads guest registers in place instead of receiving a
  marshalled CONTEXT.  Wine-side consumption is mechanical.  No JIT
  changes, no new failure modes, keeps every existing gate meaningful.
* **Step B (transition blocks, 2–4 weeks in fastppcx86):** registered
  stub RIPs compile to a host-call transition instead of a trap.  The
  trap tax disappears for every export at once — flat, COM, i386 —
  without writing another line of hand-verified guest x86.
* **Step C (typed transitions, 2–3 weeks in gen_winecom + dispatch):**
  give the COM lane the typed FP descriptors the flat lane already has,
  so float-bearing COM slots stop being named refusals served by hand
  walkers.  The `FP_SHAPE_*` hand machinery stops growing, and the
  leftover shapes (x87 returns, struct-by-value, >64-bit returns) get
  their path.

What we deliberately do **not** build is the part of ARM64EC that made it
a multi-year Microsoft project: the single shared stack, the hybrid PE
format with fast-forward sequences, dual unwind data, and the per-call
`__os_arm64x_check_call` instrumentation.  The section "What ARM64EC
needed that we don't" argues each omission from a constraint we don't
have — chiefly, that we own the emulator's dispatch, and dispatch by
target address is free in a JIT.

Honest expectation setting: **this is not the fps lever for the current
titles.**  Witcher 3's frame thread is 81% JIT — codegen is that title's
lever.  Cyberpunk gets worker-side headroom and single-digit-percent
frame-thread relief.  Dex's crossings are measured innocent.  The case
for PPC64EC is: every FUTURE API is cheap by default, the per-API
fast-path treadmill ends, the FP refusal class dies, and the
journal/batching designs — including the device journal currently
parked behind an unattributed GPU hang — stop being necessary.  That is
a correctness and engineering-velocity payoff with a modest perf side.

## What ARM64EC actually is, distilled

One process, two instruction sets, one ABI contract.  The pieces:

1. **Layout identity.**  ARM64EC code compiles against x64 struct
   layouts, x64 primitive sizes, x64 bitfield rules — so a pointer can
   cross the boundary bare, no marshalling.
2. **Native implementations behind x64-shaped entry points.**  Every EC
   function is native arm64 code; its export is callable by x64 code.
3. **Cheap transitions.**  Entry/exit thunks per function shuffle
   registers between the two conventions; the emulator checks every
   indirect call target against an EC bitmap and, on a hit, jumps to
   native code instead of JITting.
4. **Interop glue for everything that observes the machine:** a register
   mapping so an x64 CONTEXT can carry native state, dual unwind data so
   both walkers work, fast-forward sequences so x64 code that reads or
   hot-patches function bytes sees plausible x64 bytes, one shared stack
   so stack walks and callbacks compose.

Microsoft needed all of 4 because arbitrary x64 code and EC code share
modules, stacks and threads, and because the OS (not the emulator) owns
threads and unwinding.  Our constraints are different, and each
difference deletes a deliverable — see below.

## What this tree already has

**Layout identity (ingredient 1): already load-bearing.**  The native
builtins are compiled against Wine's Windows headers — LLP64 types, MS
bitfields, the same struct layouts the guest uses.  That identity is
exactly why the 64-bit flat lane dispatches by register/stack remap with
width masks and no repacking (the i386 lane repacks because *that* pair
genuinely diverges — measured, `gen_repack32.py`).  Nothing to build.

**Native code in PE clothing (ingredient 2): already built.**  Every
builtin is ELFv2 ppc64le translated to a PE image by `tools/elf2pe`
(TRANSLATED_ARCHS in makedep), loaded by the loader as the ppc64
machine's builtin.  Its guest-visible x86-shaped face is the paired
thunk PE from `tools/spec2thunk`: real AMD64 stubs
(`mov r10,rcx; syscall`, 16-byte stride) plus `__wine_thunk_info` v4 —
per-export names, signature words, and the impl name to resolve in the
native module.  The COM lane has the same shape per interface (stub
arrays + `__wine_com_thunk_info`, marshal tables in the native module).
ARM64EC glues these two halves into one hybrid binary; ours are a
two-file pair that already behaves as a hybrid.  Merging the files buys
nothing and costs a format — skip it.

**What's missing is only ingredient 3.**  Today the transition is the
trap protocol, and it is priced as follows.

## What a crossing costs today, wall by wall

The path (64-bit lane): guest `call` lands on the stub → `syscall` →
the JIT's Syscall op spills SRA and calls the embedder trap sink
(`FexBridge.cpp`) → the sink stores the guest file into a 1232-byte
AMD64 CONTEXT (EFLAGS/XMM lazy since bridge ABI 5) and calls wine's
trap callback (`dlls/ntdll/unix/signal_ppc64.c`) → the callback keys
the RIP to module+index, packages args per the width masks, and enters
the PE side through `call_emu_trap_dispatcher` /
`call_user_mode_callback` (the KiUserCallbackDispatcher-style stack
discipline) → PE-side `emu_trap_dispatch` calls the native builtin
export → unwind the same path, write RAX/Rip/Rsp, TRAP_CONTINUE,
SRA refill, resume.

Measured, all in-tree or in the session record:

* One crossing ≈ **2.0 µs** on the i386 lane (200k-call QPC microbench,
  NEXT.md item 2).  The 64-bit lane is cheaper post-ABI-5 but the same
  shape.
* Cyberpunk before the fast paths: **3.54 M crossings/s, 196,516 per
  frame**.  After QPC/peek/CS/tid/journals: still **2.18 M/s**.
* GameThread decomposition (pre-ABI-5 profile): ~17% trap/dispatch
  machinery + ~14% bridge pack/unpack + ~7% TLS — i.e. the boundary
  cost about half the frame thread.  The ABI-5 + IE-TLS work bought
  fps 22.34 → 23.95 by shaving exactly these rows.
* Witcher 3 today: frame thread 81% JIT, ntdll ~7% — the crossing work
  on that title is done and stays done.

And the treadmill that grew to avoid this price, each item hand-written
guest x86 verified with llvm-mc, each with its own gate, each a place
correctness can leak: QPC/QPF, GetCurrentThreadId, the
critical-section pair (both spellings), PeekMessageW,
WINECOM_F_CONST_QWORD getters, the per-command-list call journal, the
device journal — **which is built, opt-in, and parked behind a
reproducible amdgpu ring timeout nobody has attributed** — and a
pending batching design for the descriptor rows.  The device journal is
the strongest argument on this page: replay/batching designs invent
ordering hazards that direct cheap calls cannot have, and we have
already paid one unexplained GPU hang for that class.

## What ARM64EC needed that we don't

* **A new compiler target.**  Not needed: layout identity already holds
  (above).  No `ppc64ec-windows` triple, no toolchain fork.
* **Per-call-site checks (`__os_arm64x_check_call`).**  Microsoft
  instruments every indirect call in native code because either side
  may hold either kind of pointer.  We own the JIT: guest indirect
  calls already resolve targets through the block cache, so
  "is this target native" is a property of the *target address*,
  registered once — zero cost at call sites.  Native code calling
  guest pointers keeps today's receive-side wrapping
  (`thunk_overrides[]`, `__wine_guest_wrap_callback`), which works and
  is gated.
* **Fast-forward sequences.**  x64 code that *reads* an EC export must
  see plausible x64 bytes (DRM checksums, Detours-style hot-patching).
  We keep the thunk stubs as the visible bytes — they never change.
  EC-ness lives in the dispatcher's registration table, not in the
  image.  Bonus: any path that reaches a stub without registration
  (single-step, TF, a stale block) still executes the stub and takes
  the old trap — the slow path remains the fallback, always correct.
* **One shared stack + dual unwind.**  Microsoft needs native frames on
  the guest stack because transitions must be free-composing in both
  directions on OS-scheduled threads.  We keep two stacks: native
  frames stay on the host stack exactly as today, the guest stack shows
  one clean call frame into the export, and every existing piece of
  unwind machinery — the interleaved-frame walk, deferred RtlUnwindEx,
  guest language-handler entry — is untouched.  A guest walking its own
  stack across an EC call sees precisely what it sees today.
* **An EC register mapping / EC CONTEXT.**  Only needed when a thread
  is *observed* mid-native-call (GetThreadContext, suspend, sampling
  DRM).  Today's trap path publishes the trap context; step B keeps a
  two-store guest-link publish (return RIP, guest RSP) so the same
  answers can be given.  Full ARM64EC-style register mapping is parked
  until a title's DRM demonstrates the need.

Prior art on tap: Wine's own ARM64EC implementation is in this tree
(`dlls/ntdll/signal_arm64ec.c`, the xtajit64 interface) —
when a semantic question comes up (what does Windows answer for a
context taken inside an EC call; how are entry thunks skipped over in
unwind), the reference implementation is in this repo to read.

## The design, in steps

### Step 0 — measure the floor (~2 days)

Before building anything: extend BridgeSmoke with a prototype
"gregs-view" trap (the sink calls the callback with a pointer into
CPUState instead of building the CONTEXT) and run the existing
qpcbench-shaped microbench A/B on op4k.  That number is the ceiling on
everything below.  Decision gate: if the floor is not ≥5× cheaper than
the current trap, stop at step A.

Estimate of the floor, from the pieces: the Syscall op's SRA spill/fill
must stay (guest RAX/RDX/RCX/RBX/RSP/RBP live in host r7–r12 and XMM in
v0–v15 — ELFv2 volatiles, clobbered by any host call; see
`ArchHelpers/PPC64Emitter.h` x64::SRA).  That is ~40 loads/stores.  On
top: one host call, arg fetch from gregs + guest stack, the TEB
stack-field flip, dispatch through a per-RIP precomputed row.  Order
100–300 ns against today's ~1–2 µs — 5–10×, not 50×.  The 2-day
prototype replaces this estimate with a measurement.

### Step A — bridge ABI 6: the zero-copy trap

ABI 5 made EFLAGS and the FP file lazy; ABI 6 finishes the thought.  A
new declaration (`fexbridge_declare_trap_gregs()` or a lazy-mask bit)
makes the trap callback receive a *view*: a pointer to the guest GPR
file in CPUState plus Rip, with `fexbridge_ctx_materialize` still
available for the callbacks that genuinely need a CONTEXT (faults,
debugger publish, GetThreadContext service).  StoreStateToContext /
LoadStateFromContextAfterTrap leave the hot path entirely; resume takes
the modified gregs in place.

Wine side: the trap callback reads args from the view, keeps the
existing RIP cache, dispatch, stats sink, and publish-on-demand.  The
kill switch and poison levers follow the ABI 5 pattern
(FEXBRIDGE_EAGER_CTX stays the master).  Gates: check-lazy-ctx grows
the gregs leg; the sabotage is a callback that touches CONTEXT fields
without materializing and must die loudly under poison.

No JIT change, no new reentrancy, no visible-bytes change.  This step
is worth doing even if everything below is abandoned.

### Step B — transition blocks: retire the trap for registered exports

New bridge call:

    fexbridge_register_ec_target(rip, handler, cookie)
    fexbridge_unregister_ec_range(start, len)      /* module unload */

When the frontend is asked to compile a registered RIP, it emits a
transition block instead of decoding the stub bytes: SRA spill, host
call `handler(thread, gregs, cookie)`, SRA refill, then continue at the
guest return address (`Rip = [Rsp]; Rsp += 8` per the handler's
convention, same as today's trap epilogue).  The handler is wine's
dispatch with the per-export row precomputed into `cookie` — no RIP
lookup at all.  Everything the Syscall op already does for the sink
(InSyscallInfo sentinel, TF honoring, fault interaction) carries over;
the transition block is a leaner sibling of an existing, proven path,
not a new kind of thing.

Correctness anchors, each with today's mechanism to lean on:

* **Fallback by construction.**  The stub bytes stay.  Unregistered or
  invalidated paths trap exactly as today.  Rollout can be per-module,
  per-export, behind WINE_PPC64LE_NO_EC=1.
* **Reentrancy.**  The handler may call back into the guest (nested
  fexbridge_run) — the same contract the trap callback already
  exercises 64 levels deep in the reverse-proxy gate.
* **Observation.**  The handler publishes the two-word guest link
  before dispatch; suspend/GetThreadContext answers from it plus the
  saved SRA frame, the way fault reconstruction already rebuilds the
  file from host state.
* **Unloading.**  Guest FreeLibrary of a thunk module unregisters the
  range under the code-invalidation lock (the invalidate API and lock
  already exist).
* **Stats.**  The handler bumps the same per-row counters; the crossing
  table keeps telling the truth (armed-only, as today).

Rollout order: one flat export end to end with a gate
(check-ec-transition.sh, sabotage = a deliberately wrong-arity
registration must be caught, kill switch must restore traps), then the
flat lane wholesale from `__wine_thunk_info`, then COM stub arrays
(winecom dispatch entry as the handler — the marshal tables and hand
walkers run unchanged; only the transport changes), then the i386
`cd 80` stubs into `emu32_dispatch_thunk` (repack/bounce unchanged;
the 2 µs crossing becomes cheap even though Dex's fps never blamed it).

Then re-run the crossing table and the -benchmark ladder, and decide on
the retirements: the guest fast bodies (QPC stays — it's better than
any transition, zero crossings; peek/CS/tid likely stay too, they're
already zero-crossing), the call journal (retire if flat), and the
device journal question dissolves — CreateCBV at ~100 ns/call needs no
batching, no replay, no ordering argument, no GPU hang.

### Step C — typed transitions: delete what's left of the FP refusal class

Correction to the first draft of this page: the FLAT lane already
serves FP — spec2thunk's oracle emits `fp=` descriptors (mask, single
bits, return class) and the dispatcher has `marshal_thunk_args_fp` /
`call_native_thunk_fp`.  What is still refused, and what step C closes:
the **COM lane's FP-by-value slots** (the `hand_clear_dsv` /
`FP_SHAPE_*` class — gen_winecom has no fp words, so every
float-bearing COM slot is a named refusal until someone hand-writes
it), FP past the XMM3/stack boundary, FP-with-variadic, struct-by-value
and >64-bit returns, and the x87-return path the i386 lane needs.
Giving gen_winecom the same typed descriptors the flat lane already
carries — and the view/transition handler the FP read/write path — is
the coverage payoff: it closes the remaining class the same way the
sub-word ABI extension did, instead of hand-building one slot per
crash.

### Parked (rung 3) — do not build until a title demands it

* **Proxy collapse**: COM vtables carrying registered native pointers
  directly, no stub arrays, no interning.  Possible under
  dispatch-by-target, but it deletes the marshal hook — only sound for
  identity interfaces, and the interning cost is already paid once per
  object.  Revisit if proxy interning ever shows in a profile.
* **Full EC CONTEXT register mapping** for threads observed mid-call:
  the guest-link publish should satisfy GetThreadContext; if a DRM
  proves pickier, crib the mapping shape from signal_arm64ec.c.
* **Single stack / hybrid PE file / dual unwind**: no constraint of
  ours requires them.  Write the reason down when tempted.

## What it buys, with today's numbers against it

| Where | Today | After B/C | Honest expectation |
|---|---|---|---|
| CP2077 | 2.18 M crossings/s left; workers batch through journals | crossings stay, cost ÷5–10; journals optional | worker headroom real; frame thread single-digit %; device-journal hang class gone |
| Witcher 3 | frame thread 81% JIT | unchanged | ~nothing — the lever is codegen, as profiled |
| Dex (i386) | 13.5k crossings/s × 2 µs ≈ 3% of a core | same count, cheap | ~nothing on fps; lane stops needing its own fast-path story |
| Next title | every hot API = a fast-path project | cheap by default | the actual point |
| FP shapes | refused, hand-served on crash | typed rows | closes the class |

## Risks, named

1. **The floor is worse than estimated** — SRA spill/fill + call
   overhead could land at 500 ns+.  Step 0 exists to find out for two
   days instead of four weeks.  Even the bad case beats 2 µs.
2. **TEB stack-field flip on the fast path** — the __chkstk class of
   bug returns if the flip is missed anywhere.  It is two pairs of
   stores; the existing guest-debug and seh gates catch a miss.
3. **Blocking calls inside transition handlers** — a wait served
   through a transition parks the thread in native code with the
   guest link published; identical to a trap-served wait today.
4. **TF/single-step and debuggers** — transition blocks honor the
   block-entry TF check by falling back to the stub trap; winedbg's
   view is unchanged (it already distrusts stub RIPs).
5. **Registration vs SMC levers** — thunk pages are ours and never
   rewritten, but the lazy-SMC knob interactions deserve one A/B on
   the 32-bit lane, where the Mono + SMC history lives.

## Recommendation

Do step 0 now (2 days, one number).  Do step A regardless (it is the
natural ABI 6 and pays for itself).  Gate step B on step 0's number and
land it export-by-export behind the kill switch.  Do step C once B
serves one real title's session cleanly — it is where the port stops
owing hand-written answers for every FP-shaped export.  Leave rung 3
written down and unbuilt.
