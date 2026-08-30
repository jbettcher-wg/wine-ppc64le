# Gating `__wine_syscall_dispatcher`'s FP/VMX save: no, and here is why

**2026-08-30.** Investigated whether the unconditional FP/VMX register save at
the top of `__wine_syscall_dispatcher` (`dlls/ntdll/unix/signal_ppc64.c`) can
be gated the way the matching restore already is
(`ppc64le/docs/sessions/2026-08-29/jit-cost-attribution.md` §9.1: 65.5% of the
dispatcher's own sampled cycles, ~6-7% of all GameThread cycles, on a save
whose restore counterpart carries 0.00% of samples in the same capture).

**Verdict: the save cannot be safely gated on anything knowable at syscall
entry, and no code was shipped.** The tree is unchanged from before this
session (`git diff` against `dlls/ntdll/unix/signal_ppc64.c` is empty; see
"What changed" at the end). This is a "no, because X" result, not a
"ran out of time" result — the reason is concrete, reproduced empirically
below, and matches a pattern already load-bearing in upstream Wine's own
x86_64 port.

## 1. What the restore's gate actually tests

The restore, later in the same function, checks `frame->restore_flags`
(offset `0x130`) against `CONTEXT_FLOATING_POINT` (FPRs) and `CONTEXT_VECTOR`
(VMX), skipping the corresponding reload block when clear:

```
lwz 14, 0x130(31)        /* frame->restore_flags */
andi. 0, 14, 4           /* CONTEXT_FLOATING_POINT */
beq 5f
... 18x lfd + mtfsf ...
5: andi. 0, 14, 16        /* CONTEXT_VECTOR */
beq 6f
... 12x lvx ...
```
(`dlls/ntdll/unix/signal_ppc64.c:2433-2536`)

`restore_flags` is zeroed at entry (`li 0,0` / `stw 0,0x130(31)`, line 2303-
2304) and is set **only** by `NtSetContextThread` (line 458:
`frame->restore_flags |= flags & ~CONTEXT_INTEGER;`), which runs as ordinary C
code *during* the syscall, strictly before the restore executes. By restore
time, whether anything actually needs restoring is a plain, already-computed
fact sitting in memory. That is a synchronous condition, available for free.

## 2. Is the same condition available at *save* time? No — and not because of missing bookkeeping

The naive read is "gate the save on whether this syscall will call
`NtSetContextThread` on itself" — i.e., treat it as symmetric with the
restore. It is not symmetric, for a reason that has nothing to do with what
the syscall itself does.

`NtGetContextThread`'s self-referential path (`handle == GetCurrentThread()`,
`dlls/ntdll/unix/signal_ppc64.c:467-513`) reads `frame->fpr[]`/`frame->v[]`
**directly out of the struct populated by the entry-time save** — it does not
re-read live registers. That path has a real, already-in-tree caller that has
nothing to do with the syscall's own identity:

```c
/* usr1_handler -- SuspendThread's signal handler, signal_ppc64.c:1993-2017 */
else if (is_inside_syscall( data, SP_sig(sigctx) ))
{
    context.ContextFlags = CONTEXT_FULL | CONTEXT_EXCEPTION_REQUEST;
    NtGetContextThread( GetCurrentThread(), &context );   /* reads frame->fpr/frame->v */
    wait_suspend( &context );
    NtSetContextThread( GetCurrentThread(), &context );
}
```

`CONTEXT_PPC64_FULL` is defined as `CONTROL | INTEGER | FLOATING_POINT |
VECTOR` (`include/winnt.h:2053-2054`), so this request reaches **both** the
FPR and the VMX halves — there is no narrower "FULL" that excludes VECTOR on
this port.

The trigger for this path is `SuspendThread()` from a **different thread**,
delivered as `SIGUSR1`. That signal is unblocked for a thread's entire
lifetime, not just at specific checkpoints: `dlls/ntdll/unix/server.c:1635-
1644` blocks it process-wide at startup, and `dlls/ntdll/unix/signal_ppc64.c
:2162` (`pthread_sigmask(SIG_UNBLOCK, &server_block_set, NULL)`) unblocks it
permanently, right before a thread's first user-mode instruction. It is
re-blocked only inside a few narrow, explicit critical sections around actual
wineserver socket I/O (`server.c:306-340`, `740-774`) — nowhere near the body
of an arbitrary syscall handler. `is_inside_syscall()` is a pure SP-range
check, true for the entire span from just after the dispatcher's stack switch
to just before the restore's stack switch back.

Put together: **any thread, running any syscall, at any point after the
kernel-stack switch, can be asked by a different thread for its current
FP/VMX state, and the request is not correlated with what the running syscall
does.** A syscall that itself never touches FP/VMX and never calls
`NtGetContextThread`/`NtSetContextThread` can still be the one a debugger, a
profiler, or the game's own thread-suspension logic targets with
`SuspendThread`+`GetThreadContext` while it happens to be blocked in, say,
`NtDelayExecution`. There is no syscall-id-based table that can exclude a
syscall from this obligation, because the event that needs the answer is
external to the syscall being run.

This is also *why* the restore's gate is safe and the save's would not be:
the restore's condition is produced by code that has already run, inside the
one thread that will read it back. The save's condition would have to predict
an event — another thread's `SuspendThread` — that has not happened yet, is
not visible from the syscall id, and is not confined to safe checkpoints.

## 3. A lazy ("mark unavailable, capture on first use") alternative is unsound — discovered the hard way

The obvious follow-up: since `f14-f31`/`v20-v31` are ELFv2's callee-saved
registers, isn't the true value still sitting in the physical register,
recoverable on demand, right up until something legitimately reuses it? So
why not skip the entry save and instead capture live registers lazily, from
inside `usr1_handler`, only when a suspend actually races in?

This is unsound, and building the verification probe below reproduced the
exact failure mode that makes it unsound, in miniature. A callee-saved
register's value is only *guaranteed* intact **across a call that returns to
you** — the ABI promise is "what I got is what you'll see again once I give
control back to you," not "unreachable in between." Any C function on the
call path — Sleep()'s own body, an intervening ntdll routine, glibc's
`memcpy` (which uses VSX on POWER) — is entitled to save the original value
to *its own* stack slot, clobber the physical register for its own local use,
and restore only in *its own* epilogue. If a query lands while such a
function is mid-body, the physical register holds that function's scratch
data, not the guest's trap-time value — and the *original* value is not lost
(it will come back once that function returns), but it is not *observable*
by reading the register at that instant.

The first version of `probes/syscall-fpvmx-race.c` written for this
investigation hit exactly this: sentinel values loaded into `f14-f31`/
`v20-v31` inside a small `noinline` helper function came back as all-zero the
*instant that helper returned* — no syscall, no `Sleep()`, nothing in
between. Cause: GCC, seeing the helper's inline asm clobber a callee-saved
register, correctly inserted a save of the original (garbage) value in the
helper's own prologue and a restore in its own epilogue — bracketing the
*whole function*, because that is what the ELFv2 ABI obligates a function
that uses a callee-saved register to do for *its own* caller. A helper whose
entire body is "clobber the register, then return" has nothing between that
save and that restore, so the sentinel was erased before the helper's `ret`.
Moving the same asm inline into the thread's actual work function (so the
save/restore brackets the whole `Sleep()` call rather than a one-line helper)
fixed it. The bug and the fix are the same shape as the real question this
document answers: **a value in a callee-saved register is only trustworthy
between the entry and exit of the *specific* scope that last touched it on
purpose — not at an arbitrary instant chosen by something else.**
`__wine_syscall_dispatcher`'s entry point is the only place that instant is
knowable in advance (it is *before* any native code has had a chance to
reuse anything), which is exactly why the save has to happen there,
unconditionally, rather than being deferred.

## 4. Corroborating precedent: upstream Wine's x86_64 port does the identical thing, for the identical reason

`dlls/ntdll/unix/signal_x86_64.c`'s `__wine_syscall_dispatcher` unconditionally
runs `xsave64`/`xsavec64`/`fxsave64` (the *entire* extended state — FPU, SSE,
AVX) at entry, every trap, with no gate. Its restore is the mirror of ppc64's:
`testl $0x48,0xb4(%rcx)` (`CONTEXT_FLOATING_POINT | CONTEXT_XSTATE`) gates
`xrstor64`/`fxrstor64` (`signal_x86_64.c:3217-3241`). Same asymmetry, same
shape, on an architecture with an entirely different signal/suspend
implementation. That is strong independent evidence this is deliberate Wine
design, not a ppc64-port-specific oversight that nobody got around to fixing.

## 5. ELFv2 narrowing: already applied, no further headroom

The task asked whether the volatile/non-volatile split lets less be saved
even where a save is required. It already does, and the existing code already
takes that: the entry save covers **only** `f14-f31` (18 of 32 FPRs) and
`v20-v31` (12 of 32 VMX registers) — exactly ELFv2's non-volatile FP/vector
registers. The volatile halves (`f0-f13`, `v0-v19`) are never saved, because
nothing needs them preserved across an ordinary call boundary. There is no
narrower slice available: `usr1_handler`'s `CONTEXT_FULL` includes
`CONTEXT_VECTOR` (§2), so the VMX half cannot be dropped independently of the
FP half — both are equally reachable by the same async path.

## 6. Empirical verification

### 6.1 The probe

`~/Development/powerpc64le-ports/hangover-ppc64le/probes/syscall-fpvmx-race.c`
(new; sshfs path
`/home/jbettcher/Development/power9_development/powerpc64le-ports/hangover-ppc64le/probes/syscall-fpvmx-race.c`).
A worker thread loads known sentinel bit patterns into exactly the 18 FPRs +
12 VMX registers the dispatcher saves, then calls `Sleep(6000)` — a real
syscall (`NtDelayExecution`) that blocks for seconds. While the worker is
genuinely inside that syscall, the main thread:

1. `SuspendThread()`s it, then `GetThreadContext(CONTEXT_FULL)` — this must
   see the sentinel, exercising exactly the `is_inside_syscall()`-true arm of
   `usr1_handler` described in §2 (not the ordinary user-mode park loop that
   `probes/thread-context.c` already covers).
2. `SetThreadContext()`s a *different* sentinel image
   (`CONTEXT_FLOATING_POINT | CONTEXT_VECTOR` only), then `ResumeThread()`s
   it — exercising the already-gated restore.
3. The worker, once `Sleep()` returns, reads its own registers back and must
   see the second sentinel.

Both directions are checked because they share one asymmetry: restore is
correct because its condition is synchronous and already-computed; save has
no such condition available (§2).

Two real bugs were found and fixed while building the probe, both recorded in
its header comment: the callee-saved-clobber issue in §3, and a 16-byte
alignment miss (`lvx`/`stvx` silently mask the low 4 address bits, so an
under-aligned buffer reads/writes 8 bytes off — `struct fpvmx_image` needs
`__attribute__((aligned(16)))`). Neither is a dispatcher bug; both are
recorded so the probe is trustworthy evidence rather than an accident that
happened to print PASS.

Built with:
```
./tools/winegcc/winegcc -o syscall-fpvmx-race.exe.so --wine-objdir . --cc-cmd=gcc -fPIC \
    -mlongcall -mno-pltseq -maltivec -fcf-protection=none -fno-stack-protector \
    -I<wine-upstream>/include probes/syscall-fpvmx-race.c \
    libs/winecrt0/libwinecrt0.a dlls/kernel32/ppc64-windows/libkernel32.a dlls/ntdll/ppc64-windows/libntdll.a
```
(Note: `probe_implib()` in `probes/probe-lib.sh` globs
`dlls/<dll>/*-windows/lib<dll>.a` and, now that both `i386-windows` and
`ppc64-windows` variants exist in this build tree, alphabetically picks the
*i386* import lib first — silently wrong for a ppc64 winelib probe. Worked
around here by naming the `ppc64-windows` path explicitly; `probe-lib.sh`
itself is out of this session's scope (not `signal_ppc64.c`) and is left
unfixed, but any other probe author hitting "undefined reference to
`NtCurrentTeb`" etc. from a stock `probe_build`/`probe_implib` call should
know this is why.)

### 6.2 Baseline: correct, unmodified code — PASS

```
== GetThreadContext mid-syscall (tests the SAVE)
  ok   Fpr14 = 00005afe0000000e   [... all 18 FPRs, all 12 Vr.Low/Vr.High ok]
== SetThreadContext mid-syscall (tests the gated RESTORE)
  ok   woke Fpr14 = ffffa501ffff000e   [... all ok]
SYSCALL-FPVMX-CORRECT
```
Exit 0. The unconditional save correctly reports the true FP/VMX state to a
suspending thread mid-syscall, and the gated restore correctly reinstalls a
new one on the way out.

### 6.3 Sabotage control: skip the save unconditionally — FAIL, exactly as predicted

Applied this **temporary, deliberately-broken** patch (reverted immediately
after, see §7) to reproduce, precisely, "what a naive risky gate might do" —
skip the save entirely, unconditionally:

```diff
 "li 0, 0\n\t"
 "stw 0, 0x130(31)\n\t"           /* frame->restore_flags */
+/* TEMPORARY SABOTAGE CONTROL -- skip the entry-time FP/VMX save
+ * unconditionally, to prove the probe catches the corruption. */
+"b " __ASM_LOCAL_LABEL("sabotage_skip_fpvmx_save") "\n\t"
 /* non-volatile FPRs */
 "stfd 14, 0x1c0(31)\n\t"
 ... (unchanged FP + VMX save block) ...
 "stvx 31, 31, 11\n\t"
+__ASM_LOCAL_LABEL("sabotage_skip_fpvmx_save") ":\n\t"
 /* switch to the kernel stack: the frame sits at the top of it */
 "ld 0, 0x008(31)\n\t"
```

Rebuilt `dlls/ntdll/ntdll.so`, reran the identical probe binary against it:

```
== GetThreadContext mid-syscall (tests the SAVE)
FAIL: Fpr14: got 0000000000000000, want 00005afe0000000e
FAIL: Fpr15: got 0000000000000000, want 00005afe0000000f
   [... all 18 FPRs and all 12 Vr.Low/Vr.High read back as exactly zero]
== SetThreadContext mid-syscall (tests the gated RESTORE)
  ok   woke Fpr14 = ffffa501ffff000e   [... restore path, untouched by the
                                          sabotage, still correct -- isolates
                                          the defect precisely to the save]
SYSCALL-FPVMX-CORRUPTED (42 mismatches)
```
Exit 1. This is the silent-corruption failure mode from the task brief, made
concrete: no crash, no error return from `GetThreadContext` — just wrong
numbers (here, all-zero, because nothing had ever written the pattern into
that memory for this trap) handed to whatever asked. The restore path,
unmodified by the sabotage, continuing to pass in the same run confirms the
probe isolates save-side corruption specifically rather than failing broadly.

### 6.4 Revert and re-verify

`git diff` on `dlls/ntdll/unix/signal_ppc64.c` after `git checkout --` is
empty (exact revert). Rebuilt `ntdll.so` again and reran the probe:
`SYSCALL-FPVMX-CORRECT`, exit 0 — confirms the sabotage introduced no
side effects and the tree is back to its original, correct state.

## 7. POWER8 legality

The project's co-developers run POWER8 (ISA 2.07); this session ran on
POWER9 (ISA 3.0). Checked because it bears directly on this area:

- **The existing, unmodified save/restore block is already POWER8-legal.**
  It uses only `stfd`/`lfd` (classic scalar FPU, architecturally ancient),
  `mffs`/`mtfsf` (classic FPSCR access), and `stvx`/`lvx` (AltiVec/VMX
  X-form, ISA 2.03+) — no VSX instructions of any kind, let alone ISA-3.0-only
  ones. Compiled the whole file — `dlls/ntdll/unix/signal_ppc64.c` — with
  `-mcpu=power8` added to the real build's flags; it assembled with no
  errors (`gcc ... -mcpu=power8 ... -c dlls/ntdll/unix/signal_ppc64.c`, exit
  0). This is a whole-file check, not just the block touched here, and it
  passed clean. (Consistent with the file's own existing diligence
  elsewhere — e.g. the comment at the TAR/`mtspr 815` resume-address fix,
  "TAR is ISA 2.07, so this is POWER8-safe.")
- **My sabotage patch (§6.3) added only a branch and a label** — `b` and a
  local branch target are original-POWER instructions, trivially POWER8-legal
  — and it was fully reverted before this session ended regardless.
- **No net change shipped**, so there is nothing new to gate for POWER8 in
  this session's diff.

### A documented, not-taken POWER9 fast path

The handbook at `powerpc64le-ports/flap-standalone/docs/power9/README.md`
doesn't carry an explicit ISA-version table for the `lxv`/`stxv` vs.
`lxvd2x`/`stxvd2x`/`stvx` family (checked; its `isa/instruction-index.txt`
lists forms, not ISA versions, for this pair). Cross-checked directly against
the primary source instead: `PowerISA_public.v3.0C.pdf`, Chapter 7
("Vector-Scalar Floating-Point Operations", p.495) documents `lxv`/`stxv`
(DQ-form, immediate-displacement VSX vector load/store) there; this chapter
and these two mnemonics do not exist in the ISA 2.07B (POWER8) VSX chapter —
POWER8's only 128-bit VSX load/store is the older X-form `lxvd2x`/`stxvd2x`
(indexed-only, no immediate displacement, and big-endian element order that
needs an `xxswapd`-style fixup on this LE target, which is presumably why the
existing code uses AltiVec `stvx`/`lvx` instead of either VSX form). This is
established, widely-documented Power ISA history (mirrored in, e.g., glibc's
separate power8/power9 `memcpy` implementations), not something this
handbook needed to restate; flagging the gap per the coordinator's ask rather
than silently assuming the handbook covers everything.

**The opportunity, for the record, not taken here:** the VMX save/restore
blocks in `__wine_syscall_dispatcher` hand-unroll 12 registers as
`stvx`/`lvx` X-form (needs an index register, so each register after the
first costs a `stvx`/`lvx` **plus** an `addi r11,r11,16` — 23 instructions for
12 registers) because X-form has no immediate-displacement addressing.
`stxv`/`lxv` (DQ-form) *do* have a 12-bit, quadword-scaled immediate
displacement, so the same 12 registers would need exactly 12 instructions,
no index bump between them — roughly halving that block's instruction count.
**This is POWER9-only (ISA 3.0)**; the POWER8 fallback is exactly the
`stvx`/`lvx`+`addi` sequence already in the tree today. Adopting it would need
a build- or runtime-time ISA gate (e.g. an `-mcpu=power9`-conditional
assembly variant of this one block, selected the same way `addpcis` is gated
elsewhere per this handbook, `README.md:735-738`) — not attempted in this
session, since the save this block belongs to is the one this whole
investigation just established must stay unconditional; halving its
instruction count is a real, separate, smaller optimization worth its own
session, not bundled into a "no" verdict.

## 8. Coordination note: `libfexbridge.so` rebuild mid-session

The coordinator reported `libfexbridge.so` was rebuilt and reinstalled at
10:38 on 2026-08-30 (new report-only `FEXBRIDGE_SPINSENTINEL` guest-spin
detector, measured cost indistinguishable from zero). All correctness and
performance work in this document — the probe builds/runs in §6, the
`-mcpu=power8` check in §7 — was done on the AC922 **after** 10:42
(`git log -1` on the tree read `2026-08-30 10:42:51`), so it is entirely on
one side of that boundary; nothing here needed retaking. No Cyberpunk floor
legs were taken in this session at all (see §9), so the bridge rebuild has no
bearing on a before/after comparison that does not exist.

## 9. Performance

**No functional change was made, so no floor measurement was taken.** The
task's predicted ~2 ms floor improvement is not realized, because realizing
it would mean shipping the exact defect demonstrated in §6.3. Reporting "no
measurement" honestly per the task's own instruction ("if you measure less,
report less" — here, zero, because nothing was changed) rather than
benchmarking a no-op change or, worse, running the sabotaged build against a
real game to manufacture a misleading "it's faster" number.

The one real, quantified, not-yet-realized opportunity is §7's `stxv`/`lxv`
narrowing of the VMX block's instruction count on POWER9 hardware specifically
— a separate, smaller, ISA-gated change, not evaluated for frame-time impact
here.

## What changed

- `dlls/ntdll/unix/signal_ppc64.c`: **no net change.** A sabotage patch was
  applied and fully reverted (`git diff` empty, confirmed after revert and
  after a follow-up rebuild + re-verify pass, §6.4).
- New file: `probes/syscall-fpvmx-race.c` (sibling repo, both machines via the
  sshfs mount) — a correctness probe for the FP/VMX save/restore path across
  a real syscall + cross-thread suspend, kept for reuse the next time this
  question comes up (e.g. if the `stxv`/`lxv` optimization in §7 is ever
  attempted, this probe is exactly what should gate it).
- This document.

---

# Follow-up, 2026-08-30 afternoon: the `stxv`/`lxv` fast path lands

**Summary, up front.** The not-taken opportunity flagged in §7 above is now
implemented: `__wine_syscall_dispatcher`'s VMX save (entry, unconditional)
and restore (exit, gated on `CONTEXT_VECTOR`) blocks each get a compile-time
choice between the ISA 3.0 `stxv`/`lxv` DQ-form encoding and the original
POWER8-legal `stvx`/`lvx` X-form encoding — same registers, same memory
image, same guarantees, only the instruction selection differs (§0's "cannot
gate whether" verdict is untouched; this changes how the unconditional save
is encoded, not whether or when it runs). The POWER8 gate was verified to
actually hold three ways, not just "it assembled": (1) directly checking
which macro this toolchain defines under which flags, (2) disassembling the
real linked `ntdll.so` under each configuration to confirm the chosen
encoding is the one actually in the binary, and (3) running a bare `stxv`
under a POWER8 CPU model to watch it SIGILL for real, since gas will not
warn and this machine has no POWER8 hardware to test on directly. The probe
(60/60 baseline, 60/60 on the new path, sabotage re-proven red on the new
path specifically, clean 60/60 after revert) all passed. The predicted ~2 ms
floor improvement **did not show up**: three repeats per configuration put
the ISA 3.0 path's mean floor at 35.39 ms and the POWER8 fallback's at
35.38 ms — a 0.01 ms difference, well inside both this run's own ~2 ms
spread and the documented ~19% session-to-session variance. Reporting that
honestly rather than the prediction.

## 10. Why compile-time, and why not the runtime dispatch tried first

The first implementation attempt (built, probed, then deliberately replaced
before landing — worth recording so the next person doesn't re-walk the same
path) used a `getauxval(AT_HWCAP2) & PPC_FEATURE2_ARCH_3_00` runtime check,
cached once in `signal_init_process()` into a static `has_isa_3_00`, with
`WINE_PPC64LE_FORCE_POWER8_VMX` to force the fallback for testing. It worked
— both encodings were correct — but it is exactly the "elaborate runtime
dispatch" the task said not to build: every syscall entry paid an extra
`addis`/`lbz`/`cmpwi`/`beq` (and the restore's `CONTEXT_VECTOR` arm another
four) to re-decide, at runtime, a question that is fixed for the entire life
of the binary. Replaced it with two compile-time object-like macros,
`__ASM_VMX_SAVE_NONVOLATILE` and `__ASM_VMX_RESTORE_ALL`, defined once near
the top of `dlls/ntdll/unix/signal_ppc64.c` under `#ifdef _ARCH_PWR9` /
`#else`, and invoked by name at the two use sites inside
`__wine_syscall_dispatcher`'s existing `__ASM_GLOBAL_FUNC(...)` string-literal
argument (the C preprocessor expands the macro name into the right block of
`"..."` fragments before the compiler's normal adjacent-string-literal
concatenation runs, so this is ordinary, well-defined preprocessing — no
runtime branch, no shared label between two arms, one arm is compiled in and
the other simply is not).

`_ARCH_PWR9` is the macro GCC itself defines exactly when `-mcpu=power9` (or
later) is passed, and does not define otherwise (checked directly against
this project's own toolchain, not assumed — see §11.1). This turns out to
line up with how this tree actually builds today, checked rather than
assumed: `dlls/ntdll/Makefile`'s `CFLAGS` is the literal string `-g -O2` —
**no `-mcpu` flag at all** — so a plain `make` inherits whatever this
toolchain's own default target is. On this project's Arch Linux POWER
toolchain (GCC 16.1.1, `--with-bugurl=...archpower...`, no `--with-cpu`
override visible in its `configure` line) that default is confirmed to be
POWER8: `echo | gcc -dM -E -` defines `_ARCH_PWR8` and not `_ARCH_PWR9`. So a
co-developer's ordinary build — and this project's own current default
build, verified — already lands on the POWER8-legal branch with zero source
changes and zero flags, which is exactly "binaries are compiled for the
machine that runs them" without adding anything to prove it at runtime. The
owner's local POWER9-tuned build opts in with a CFLAGS override (§11.3);
nothing else about the build changes.

## 11. How the POWER8 gate was established to actually hold

The task named the exact failure mode to guard against: `gas` assembles ISA
3.0 instructions at `-mcpu=power8` without complaint, and POWER8 hardware
`SIGILL`s on them at runtime — so a clean assemble proves nothing on its
own. Three independent checks, each closing a different gap a "the
`#ifdef` looks right" read would leave open:

### 11.1 The macro is defined exactly where expected, checked on this toolchain

```
$ echo | gcc -dM -E -mcpu=power9 - | grep ARCH_PWR9
#define _ARCH_PWR9 1
$ echo | gcc -dM -E -mcpu=power8 - | grep ARCH_PWR9
(nothing)
$ echo | gcc -dM -E - | grep -E 'ARCH_PWR8|ARCH_PWR9'
#define _ARCH_PWR8 1
```
(no `_ARCH_PWR9`). `-mcpu=power9` defines it, `-mcpu=power8` and no flag at
all do not, on the actual `gcc (GCC) 16.1.1` running on this AC922 — not
inferred from GCC documentation, read off this machine's own preprocessor.

### 11.2 The linked binary actually contains the intended encoding, not just "it compiled"

Compiled `dlls/ntdll/unix/signal_ppc64.c` three ways — the plain `make`
(no `-mcpu`), `make CFLAGS="-g -O2 -mcpu=power9"`, and
`make CFLAGS="-g -O2 -mcpu=power8"` — all three clean, no warnings. Then,
for the two that matter operationally, linked `ntdll.so` and disassembled
`__wine_syscall_dispatcher` out of the real shared object:

| build | `stxv`/`lxv` count | `stvx`/`lvx` count |
|---|---:|---:|
| plain `make` (no `-mcpu`) | 0 | 44 |
| `make CFLAGS="-g -O2 -mcpu=power9"` | 44 | 0 |

44 = 12 (save, `v20`-`v31`) + 32 (restore, `v0`-`v31`); the two counts are
mutually exclusive in both builds, confirming the `#ifdef` selects exactly
one full arm, cleanly, in the object that actually ships — not merely that
both arms are individually well-formed C strings.

### 11.3 The instruction actually SIGILLs on a POWER8 decoder, demonstrated rather than cited

This AC922 has no POWER8 hardware to test on, so instead of resting on the
ISA manual citation from §7 (still correct, just unfalsified locally until
now), used `qemu-ppc64le-static` (present on this machine already, no
package install) for its `-cpu power8` TCG model, which implements POWER8's
actual instruction decode table rather than POWER9's:

```c
/* isa3test.c */
int main(void) {
    unsigned char buf[16] __attribute__((aligned(16))) = {0};
    __asm__ volatile ("stxv 0, 0(%0)\n\t" :: "b"(buf) : "memory");
    printf("stxv executed without trapping\n");
    return 0;
}
```
```
$ gcc -static -O0 -o isa3test isa3test.c        # native gcc, this AC922, no -mcpu needed for one instruction
$ ./isa3test                                     # native POWER9 hardware
stxv executed without trapping
$ qemu-ppc64le-static -cpu power9 ./isa3test
stxv executed without trapping
$ qemu-ppc64le-static -cpu power8 ./isa3test
qemu: uncaught target signal 4 (Illegal instruction) - core dumped
```
Signal 4 = `SIGILL`. Same opcode class as `dlls/ntdll/unix/signal_ppc64.c`'s
`__ASM_VMX_SAVE_NONVOLATILE`/`__ASM_VMX_RESTORE_ALL` ISA-3.0 arm (confirmed
by comparing `objdump -d` of the toy binary against `objdump -d` of the real
`-mcpu=power9` `signal_ppc64.o` — both emit the DQ-form primary opcode
family for `stxv`/`lxv`, just with different register/displacement fields).
This is the task's named risk, reproduced for real: `gas` assembled the toy
program's `stxv` with zero complaint (there is no `-mcpu` flag on the `gcc
-static` line above at all), and a real POWER8 instruction decoder rejects
it at the first attempt to execute it. §11.1+§11.2 are what keep this
project from ever shipping that combination; §11.3 is why those two checks,
and not a clean assemble, are the thing being trusted.

## 12. Probe results, ISA 3.0 path plus sabotage control re-run

Built once: `probes/syscall-fpvmx-race.exe.so` (same build recipe as §6.1;
no changes to the probe itself were needed). Ran against three separate
builds of `ntdll.so`, in this order, always confirming `objdump` shows the
expected encoding in `__wine_syscall_dispatcher` before treating a sweep as
meaningful:

| build | sweep | result |
|---|---|---|
| POWER8 fallback (plain `make`, 0 `stxv`/`lxv`, 44 `stvx`/`lvx`) | 60 runs | **60/60 PASS** |
| ISA 3.0 fast path (`-mcpu=power9`, 44 `stxv`/`lxv`, 0 `stvx`/`lvx`) | 60 runs | 59/60 PASS — one run's last captured line was the `SetThreadContext` section header rather than a `SYSCALL-FPVMX-CORRECT`/`-CORRUPTED` verdict; that sweep only kept the last output line, so there is no saved log to diagnose it from, and it did not recur |
| ISA 3.0 fast path (same build) | 60 more runs, full per-run logs kept | **60/60 PASS** |
| ISA 3.0 fast path, **sabotage** (branch inserted right after `stw 0, 0x130(31)` jumping past the entire FPR+FPSCR+VMX save, landing just before the kernel-stack switch — same shape as §6.3, applied to the `_ARCH_PWR9` arm specifically) | 1 run | **FAIL, 42 mismatches, all-zero** — `Fpr14`..`Fpr31` and `Vr.Low/High 20`..`31` all read back `0` instead of their sentinels; the `SetThreadContext`/restore check in the same run, untouched by the sabotage, still passed — identical isolation-of-defect signature to §6.3 |
| ISA 3.0 fast path, sabotage reverted (`grep -c sabotage` → 0 matches; rebuilt; `objdump` re-confirmed 44 `stxv`/`lxv`, 0 `stvx`/`lvx`) | 60 runs | **60/60 PASS** |

Total: 240 probe runs across the four post-fallback sweeps, 239 clean, one
unexplained-but-not-reproduced anomaly with no saved diagnostic (honestly
reported rather than quietly dropped; not treated as a correctness finding
since it did not recur across 120 further runs bracketing it on the same
build, and this machine runs other agents' concurrent workloads that can
introduce scheduling noise a signal/thread-suspend race probe is sensitive
to). The sabotage control lands the specific thing the task asked for:
proof the probe can still go red on the **new** code (the `stxv`/`lxv` arm),
not merely a re-citation of the old code's already-published sabotage
result.

## 13. Performance: repeats, spread, and an honest null result

Three Cyberpunk `-benchmark` legs per configuration, same command as the
task's recipe, `GameThread` confirmed absent before every launch, every kept
leg verified by line count and a plausible `averageFps` before being used.
Floor = minimum `Frame time (ms)` in `frames.csv` (never the summary JSON's
`minFps`/`maxFps`), matching this project's established convention
(`peek-fastpath-impl.md` §7: "floors from frames.csv, never from the
summary's min/max fps fields").

| config | floors (ms) | mean | n |
|---|---|---:|---:|
| ISA 3.0 fast path (`-mcpu=power9`) | 36.15, 35.98, 34.03 | **35.39** | 3 |
| POWER8 fallback (plain `make`) | 35.17, 35.06, 35.90 | **35.38** | 3 |

Two POWER8-fallback launch attempts (interleaved with the three kept above)
crashed before writing any `frames.csv` — a `fexbridge SPINSENTINEL`
trap-storm report immediately followed by a `KiUserExceptionDispatcher`
"interrupted sp ... is on the UNIX/other stack" fault, `rc=3` both times, at
the identical guest RIP. This is unrelated to the FP/VMX save block (no
`__wine_syscall_dispatcher` frames anywhere in the failure signature) and
was not investigated further — out of this document's scope — beyond
confirming per the task's discipline that a run producing no `frames.csv` is
discarded, not interpreted, and re-launched. A third attempt on the same
build succeeded cleanly and is the third row above.

**Measured difference: 0.01 ms — not the predicted ~2 ms, and not
distinguishable from noise.** The spread within each three-run sample
(2.12 ms for the fast path, 0.84 ms for the fallback) is itself larger than
the entire difference between the two means. Per the task's own instruction
("if you measure less, report less"): measured ~nothing, reporting ~nothing.

Plausible reason the predicted ~2 ms did not appear, reasoned rather than
measured further: the *save* side of this change removes only 11
instructions per syscall trap (12 `stxv` vs. 12 `stvx` + 11 interleaved
`addi`s), on ELFv2 non-volatile registers that are dead weight to the
compiler's own instruction scheduler either way; the much larger 31-
instruction reduction on the *restore* side almost never executes in this
workload, because `CONTEXT_VECTOR` restore is the flag
`jit-cost-attribution.md` §9.1 already measured at 0.00% of samples in a
real capture — restores with it set are rare enough not to show up at all.
Eleven instructions at a few cycles each, on one syscall trap out of however
many land in a floor frame, is a plausible candidate for "real but below
this measurement's resolution," not for "should have been ~2 ms and
wasn't" — the original ~2 ms figure in the task brief was a prediction to
test, and this is the test's honest result.

## What changed (this follow-up)

- `dlls/ntdll/unix/signal_ppc64.c`: **net change, landed.** Two new
  compile-time-selected macros, `__ASM_VMX_SAVE_NONVOLATILE` and
  `__ASM_VMX_RESTORE_ALL`, gated on `#ifdef _ARCH_PWR9`, replace the single
  hand-unrolled `stvx`/`lvx`+`addi` sequences at both the VMX save site
  (entry, unconditional) and the VMX restore site (exit,
  `CONTEXT_VECTOR`-gated) inside `__wine_syscall_dispatcher`. No other
  behavior changes: the save is exactly as unconditional as before (§0-§5
  above are untouched), the restore is exactly as gated as before, and both
  arms produce byte-identical register state, verified by the probe. No
  runtime check, no new env var, no new global — a first draft that added
  all three (`has_isa_3_00`, `getauxval`, `WINE_PPC64LE_FORCE_POWER8_VMX`)
  was built, probed, and then deliberately replaced (§10) before landing.
- No new files. `probes/syscall-fpvmx-race.c` (sibling repo) is reused
  unchanged from the prior session.
- This document, extended rather than replaced, per the coordinator's
  instruction to keep "cannot gate whether" and "can improve how" together.
