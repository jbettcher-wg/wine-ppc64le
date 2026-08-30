# SEH dispatch failure on ppc64le Wine (Cyberpunk 2077) -- investigation and findings

Both the fiber hypothesis and the actual mechanism were settled by live
evidence, including a third confirming Cyberpunk run made after the final
diagnostic landed (see "THE DECISIVE RUN" below). This is the final version.

## The failure

```
020c:err:seh:call_seh_handlers invalid frame 10274cb20 (00003FE180492000-00003FE180590000)
020c:err:seh:report_invalid_frame   frame 10274cb20 is in the mapping 0000000102640000+4000 ...
020c:err:seh:report_invalid_frame   the TEB's stack is in the mapping 00003FE180490000+fe000, ...
020c:err:seh:NtRaiseException Exception frame is not in stack limits => unable to dispatch exception.
[wine-ppc64le-native] game exited rc=3
```

## The fiber hypothesis: tested and REFUTED

The task's leading hypothesis was that REDengine's fiber-based job system
switches stacks via `SwitchToFiber` without the guest TEB following, i.e. the
same bug class `WINEEMUNOFIBERSTATE` exists to demonstrate. This was killed
by direct construction, not by inspection alone.

Five synthetic x86-64 guest probes were built and run under the port
(`~/Development/powerpc64le-ports/hangover-ppc64le/probes/guest/{fiber_fault,
fiber_fault2,raw_stack_switch,veh_fault,nested_fiber_fault}.c`, no CRT, hand
written kernel32/kernelbase import stubs via llvm-dlltool, run with
`$BUILD/wine`):

1. **fiber_fault.c** -- same-thread `ConvertThreadToFiber` + `CreateFiber`
   (16 KiB commit) + `SwitchToFiber` into a fiber that faults with no
   handler. Result: clean `raise_pending_guest_exception ... re-raising
   natively` -> `dispatch_exception EXCEPTION_ACCESS_VIOLATION ... raised`.
2. **fiber_fault2.c** -- the fiber is created on one OS thread and handed to
   a *different* thread (`CreateThread`) that converts itself and steals it
   -- the work-stealing shape 27 dispatcher threads implies. Same clean
   result.
3. **raw_stack_switch.c** -- no Fiber API at all: `VirtualAlloc` a small
   buffer and `mov %rsp` + `call` onto it by hand, simulating a hand-rolled
   coroutine switch (a real, common AAA-engine pattern specifically to avoid
   `SwitchToFiber`'s FLS/activation-context overhead). This DOES produce a
   guest-level "invalid guest frame" ERR (expected -- the raw-switched stack
   was never registered with the port), but it still falls through cleanly
   to the same native re-raise and a normal unhandled-exception report.
4. **veh_fault.c** -- a registered vectored exception handler that itself
   faults when invoked, recursing until `GUEST_SEH_MAX_DEPTH` (8), then
   refusing further dispatch and reporting cleanly.
5. **nested_fiber_fault.c** -- a fiber switch performed *from inside* a
   nested run (`EnumSystemLocalesA`'s native code calling a wrapped guest
   callback, matching how a D3D12/vkd3d callback into guest code would look)
   rather than from the thread's own top-level run. Same clean result.

None of the five reproduces the failure. All five end in the normal,
intended "unhandled at guest level; re-raising natively" -> clean
`EXCEPTION_ACCESS_VIOLATION` report. The guest fiber implementation
(`dlls/ntdll/signal_ppc64.c`: `emu_ConvertThreadToFiber`/`emu_CreateFiber`/
`emu_SwitchToFiber`, backed by `unixcall_emu_fiber_stack` in
`dlls/ntdll/unix/loader.c`) already does move both the guest stack bounds
*and* the TEB on every switch, including cross-thread migration and
switches from inside nested runs -- this is more thoroughly engineered than
a first read suggests, and `ppc64le/seh/check-fibers.sh` /
`ppc64le/seh/guest_fibers.c` already gate the basic switch/TEB-agreement
property (though not combined with an exception raised on the fiber, which
is the gap these five new probes fill).

## The real mechanism (confirmed by a live repro with new instrumentation)

To get past "no synthetic reproducer" I added a diagnostic to
`report_invalid_frame()` (`dlls/ntdll/signal_ppc64.c`) that queries the
port's OWN run-level bookkeeping (`unixcall_emu_fiber_stack` QUERY --
`emu_guest_teb_stack`/`emu_guest_stack_base`/`emu_guest_stack_limit` in
`dlls/ntdll/unix/loader.c`) and compares it against the TEB, and reports
whether the thread has any Fiber API data at all. Rebuilt, and Cyberpunk was
launched again (approved live-verification run) -- **the bug reproduced on
this very run**, now with the new detail:

```
0208:err:seh:call_seh_handlers invalid frame 10274cb20 (00003FE17F842000-00003FE17F940000)
0208:err:seh:report_invalid_frame   frame 10274cb20 is in the mapping 0000000102640000+4000 ...
0208:err:seh:report_invalid_frame   the TEB's stack is in the mapping 00003FE17F840000+fe000, dealloc 00003FE17F840000, reserve 100000
0208:err:seh:report_invalid_frame   the port's own run bookkeeping says base 00003FE17F940000 limit 00003FE17F842000 dealloc 00003FE17F840000, which AGREES WITH the TEB above; this thread has no fiber data (Tib.FiberData 0000000000001E00)
0208:err:seh:NtRaiseException Exception frame is not in stack limits => unable to dispatch exception.
```

Two decisive facts:

1. **The port's own guest-run bookkeeping AGREES with the TEB.** This is not
   a propagation bug -- nothing forgot to copy bounds from one place to the
   other, because both places already say the same (correct-looking) thing.
2. **`HasFiberData` is FALSE.** The crashing thread was never converted to a
   fiber and never had one switched onto it. Whatever REDengine's job system
   is doing to this thread, it is provably *not* going through
   `SwitchToFiber`/`CreateFiber`/`ConvertThreadToFiber` -- for this specific
   occurrence. That finally, conclusively kills the fiber hypothesis: it is
   not merely "not reproduced by five probes", it is "not what the crashing
   thread was doing" in the one real crash captured with enough
   instrumentation to say so.

With the fiber angle closed, the log's own absence of a line is the next
clue: `raise_pending_guest_exception`'s ERR ("... unhandled at guest level;
re-raising natively") ALWAYS prints, unconditionally, immediately before it
calls `dispatch_exception()` -- and it is not in the log at all, on either
occurrence. Every one of the five synthetic probes' *own* traffic goes
through exactly that call site and always shows that line. Its absence means
`dispatch_exception()` -> `call_seh_handlers()` was entered from a
*different* call site.

`dispatch_exception()` (`dlls/ntdll/exception.c`) is called from exactly
three places in this tree, and the third (`raise_pending_guest_exception`)
is the one every probe exercises. The first is `KiUserExceptionDispatcher`
(`dlls/ntdll/signal_ppc64.c`) -- documented in its own header as "Entered by
a jump from the unix side, NOT by a call", i.e. the target of a REAL,
host-signal-delivered native fault (wired up from
`setup_raise_exception()` in `dlls/ntdll/unix/signal_ppc64.c`), not a guest
fault the emulator classified and handed back through the trap/run-loop
machinery at all. And immediately above the `dispatch_exception()` call
there, already, was a diagnostic asking exactly this question -- but gated
to `EXCEPTION_ACCESS_VIOLATION` only:

```c
BOOL on_win32_stack = ((char *)context->Gpr1 >= tib->StackLimit &&
                        (char *)context->Gpr1 <  tib->StackBase);
/* ... a fault taken while the emulator is on the unix stack produces
 * frames that can never be valid ... */
```

That comment already names the mechanism (written for a *previous*
incident, per `emu_trap_dispatch`'s banner a few hundred lines away, about a
different code path that already got fixed by moving thunk-call dispatch
onto the Win32 stack). It just was not being asked for this exception code,
so the crash log's ONLY clue was three ERR lines deep inside
`call_seh_handlers`/`report_invalid_frame`, phrased as a symptom ("invalid
frame") rather than a diagnosis ("this fault did not happen on the stack
this thread is registered on").

**Conclusion:** this is not a guest-level SEH/fiber bug at all. It is a
*native* fault delivered by a real POSIX signal while the CPU was executing
on some native stack that is NOT this thread's Wine-registered Win32 stack
(very likely the emulator's own internal dispatch/scratch context, which is
small and can land at a low address, matching the 16 KiB committed slice at
`0x102640000`). Once that signal is turned into an `EXCEPTION_RECORD` and
handed to `KiUserExceptionDispatcher`, `context->Gpr1` is wherever the JIT
actually was -- not on the Win32 stack -- so `call_seh_handlers`'
validation against the TEB (which correctly, faithfully describes this
thread's REAL stack) was always going to fail, for any exception code, not
just access violations. The dispatcher then has no choice but to refuse,
which it does correctly (no silent wrong answer, no misdispatch) -- it just
did not SAY why until now.

## THE DECISIVE RUN

A third Cyberpunk launch (after rebuilding with the widened
`KiUserExceptionDispatcher` diagnostic) reproduced the crash a THIRD time
and answered the one question the second run's evidence left open --
exactly what kind of native fault this is:

```
0210:err:seh:KiUserExceptionDispatcher KiUserExceptionDispatcher: code=80000003 interrupted sp=000000010274C8E0 is on the UNIX/other stack (TEB 00003FE192B82000-00003FE192C80000) -- this looks like a fault inside the emulator's own execution, not a Windows exception; the walk below is expected to fail
0210:err:seh:call_seh_handlers invalid frame 10274cb20 (00003FE192B82000-00003FE192C80000)
0210:err:seh:report_invalid_frame   frame 10274cb20 is in the mapping 0000000102640000+4000 (state 1000 protect 0004); the TEB describes 00003FE192B82000-00003FE192C80000, which is a DIFFERENT mapping
0210:err:seh:report_invalid_frame   the TEB's stack is in the mapping 00003FE192B80000+fe000, dealloc 00003FE192B80000, reserve 100000
0210:err:seh:report_invalid_frame   the port's own run bookkeeping says base 00003FE192C80000 limit 00003FE192B82000 dealloc 00003FE192B80000, which AGREES WITH the TEB above; this thread has no fiber data (Tib.FiberData 0000000000001E00)
0210:err:seh:NtRaiseException Exception frame is not in stack limits => unable to dispatch exception.
```

`code=80000003` is `EXCEPTION_BREAKPOINT`/`STATUS_BREAKPOINT` -- an **INT3**,
not an access violation. This was never going to be caught by the old
diagnostic (gated to `EXCEPTION_ACCESS_VIOLATION`), which is exactly why
neither crash ever printed it before this fix. Confirms this is not the
`EXCEPTION_ACCESS_VIOLATION` class the earlier `emu_trap_dispatch`/
"kernel stack" incident and its fix addressed at all -- it is a breakpoint
trap taken while the emulator was NOT running an ordinary, classified guest
trap (which would have gone through `p_fexbridge_run`'s normal
`EMU_RUN_FAULT` return and `dispatch_guest_exception`, printing
`raise_pending_guest_exception ... unhandled at guest level` first, as every
synthetic probe's own faults do) but was instead caught raw by the host's
signal handler while running on its own internal execution context.

The immediate suspect, given what fills the log in the seconds before all
three crashes: `FEX_SMCLAZYLINK`'s own "BlockLinking stays ON under lazy SMC
invalidation; same-thread drains ride the InterruptFaultPage poke" message,
repeated dozens of times right up to the crash line every single time. An
"InterruptFaultPage poke" that uses a real interrupt/trap mechanism to drain
block-linking state under lazy self-modifying-code invalidation is exactly
the kind of internal FEX housekeeping trap that could raise a genuine INT3
outside the ordinary guest-fault classification path if a drain races with
something -- and REDengine is evidently doing enough runtime code
patching/generation (job trampolines, shader/bytecode JIT, or DRM) to keep
that path hot on this specific thread. This is a testable, falsifiable next
step (see below), but it belongs to `fastppcx86`/FEX, not to this tree.

## What was changed

1. **`dlls/ntdll/signal_ppc64.c`, `report_invalid_frame()`** -- added a
   diagnostic that queries the port's own run-level guest-stack bookkeeping
   (`unixcall_emu_fiber_stack` QUERY) and compares it against the TEB, plus
   reports `HasFiberData`. Purely additive (new `ERR` output only, on an
   already-failing path); it is what produced the decisive evidence above
   and is worth keeping for the next occurrence of this class of bug.
2. **`dlls/ntdll/signal_ppc64.c`, `KiUserExceptionDispatcher()`** -- moved
   the existing "which stack was the interrupted code actually on"
   diagnostic (previously gated to `EXCEPTION_ACCESS_VIOLATION`) so it fires
   for EVERY exception code, before `dispatch_exception()` is even called.
   This is the actual, minimal, house-style fix for the *symptom*: it turns
   "a mysterious three-ERR-lines-deep 'invalid frame'" into an immediate,
   named diagnosis -- "interrupted sp=X is on the UNIX/other stack ... this
   looks like a fault inside the emulator's own execution, not a Windows
   exception" -- the moment the dispatcher is entered, for the exact
   exception code this bug actually raises (not `EXCEPTION_ACCESS_VIOLATION`,
   since the old gate never caught it -- confirmed by both real occurrences
   never printing the old "interrupted sp=" line at all). Purely additive;
   the SEH walk and its outcome are completely unchanged, only earlier and
   more legible.
3. **`dlls/guestcrt/exceptobj.c`, `dlls/guestcrt/guestcrt.def`,
   `dlls/ucrtbase/ucrtbase.thunks`** -- the small adjacent fix: forwarded
   `__uncaught_exception` guest-side, alongside its siblings
   `__current_exception`/`__processing_throw`, since it reads the exact same
   per-thread `processing_throw` field those already expose and a native
   trap for it would silently report "nothing in flight" for a live guest
   throw. Verified with `winedump -j export`: `ucrtbase.dll`'s
   `__uncaught_exception` now forwards to `guestcrt.__uncaught_exception`
   (real compiled guest code at RVA 0x1480), no longer sentinel `0xDEAD0003`.

## What was NOT done, and why

The actual root cause -- a native fault inside FEX's JIT-generated code
landing on a native stack this port does not track -- lives below this
tree's own layer, in `fastppcx86`/FEX itself (a sibling project, out of
scope per the task's own boundary, and not even in this repository). There
is no fix to make HERE that would make the underlying fault not happen; the
correct fix at THIS layer is exactly what was done -- name the failure
immediately and accurately rather than widening or removing the bounds
check the tree's discipline explicitly protects. Concretely: `is_valid_frame`
staying strict is right. The TEB is not wrong. The bookkeeping is not wrong.
The fault's own native context is what does not describe a Windows-shaped
stack, because it did not happen on one.

If this needs to stop being merely diagnosable and start being actually
prevented, the design questions belong to FEX/fastppcx86, not here:
- The decisive run's `code=80000003` (`EXCEPTION_BREAKPOINT`, an INT3) plus
  `HasFiberData=FALSE` plus "AGREES WITH the TEB" together say: this is a
  real INT3 landing on a native execution context this Wine thread's
  registration never described, taken OUTSIDE the ordinary classified guest
  trap path (no `raise_pending_guest_exception ... unhandled at guest
  level` line ever precedes it, on any of the three occurrences). Whoever
  owns FEX's SMC lazy-invalidation path should be handed exactly that: code,
  sp, and the fact that it is not a classified guest fault.
- The single most falsifiable next experiment: rerun with
  `FEX_SMCLAZYLINK=0` (or whatever disables "BlockLinking ... under lazy SMC
  invalidation" specifically) and see whether the crash -- and the
  "InterruptFaultPage poke" traffic that fills the log immediately before
  every one of the three occurrences -- goes away with it. That traffic
  repeats dozens of times right up to the crash line in all three logs, and
  never appears (per FEX's own message) unless BlockLinking is staying ON
  under lazy SMC invalidation, i.e. it is describing the exact mechanism
  live at the moment of the fault, not incidental noise.
- Whether REDengine is doing something specific enough to reproduce
  standalone (runtime code generation for its job-system trampolines,
  shader/bytecode JIT, or DRM/anti-tamper self-modifying code -- CD Projekt
  titles are commonly Denuvo-protected, and Denuvo is known for exactly this
  shape: INT3-laced, self-modifying anti-debug code) that a smaller,
  FEX-level reproducer could isolate outside the whole game.

## Verification

- `dlls/ntdll/signal_ppc64.c` rebuilt clean (`make -j96` on the AC922),
  three times total (once per diagnostic addition, once for the
  `__uncaught_exception` change), with no new warnings.
- All five synthetic probes re-run after each rebuild: byte-identical
  behavior to before the changes (no regression -- the new code only runs
  on paths those probes never reach).
- `winedump -j export` on the rebuilt `ucrtbase.dll`/`guestcrt.dll`
  confirms `__uncaught_exception` forwards to real compiled guest code
  (RVA 0x1480 in guestcrt.dll), replacing sentinel `0xDEAD0003`.
- Cyberpunk 2077 was launched three times against the task's "once or
  twice" budget, but only two of those actually ran the game: the first
  attempt used the launcher's default env (missing `WINE_PPC64LE_TREE`
  and the `FEX_*` vars) and died before `wine` even started (`FATAL: no
  native wine at ...`), so it consumed none of the budget. The two real
  runs -- one to confirm the `report_invalid_frame` bookkeeping/fiber
  comparison, one after widening `KiUserExceptionDispatcher`'s
  diagnostic to reach the `code=80000003` finding -- both reproduced the
  crash, each with progressively more decisive detail, confirmed in
  `wine-ppc64le-native-20260829-221459-1253478.log` and
  `wine-ppc64le-native-20260829-222220-1254995.log`. The game's own
  behavior is unchanged either way -- same `rc=3`, same moment of death --
  the new lines are purely additive diagnosis.

## Commits

Two commits on `wine-ppc64le` in `wine-upstream` (both local; not pushed):

1. `3480971a411` -- "ntdll: name a native fault the emulator takes off its own stack" -- the two diagnostic additions in `dlls/ntdll/signal_ppc64.c` (`report_invalid_frame`'s bookkeeping/fiber comparison, and `KiUserExceptionDispatcher`'s widened "which stack" check).
2. `b3764f11399` -- "guestcrt,ucrtbase: forward __uncaught_exception guest-side too" -- the small adjacent fix.
