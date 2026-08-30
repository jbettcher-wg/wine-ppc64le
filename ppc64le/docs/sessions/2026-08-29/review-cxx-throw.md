# Adversarial review: guest-side `_CxxThrowException` (commits bc8adc33e45, fc8df7fb957, 2fc48ac17bc)

Reviewed 2026-08-29 against the tree at
`powerpc64le-ports/hangover-ppc64le/wine-upstream`, the built PEs in
`wine-build`, the Proton prefix 2320 staged binaries, and by re-running the
gates myself on the AC922. Every claim below labeled MEASURED was verified at
byte level or by execution, not read from the author's report.

## Verdict table

| # | Area | Verdict |
|---|------|---------|
| 1 | Exception record (count/order/magic/4th param) | **CORRECT** |
| 2 | Image base | **CORRECT** (proven for EXE-resident ThrowInfo; DLL case reasoned, untested) |
| 3 | Calling convention / ABI / emitted code | **CORRECT** (disassembled; real x86-64, IAT-bound, .pdata present) |
| 4 | FORWARD chain | **CORRECT for the builtin lane; the staged-Proton parity rationale is factually WRONG** — the staged files are the MSVC redist and the loader loads builtins first anyway |
| 5 | `__C_specific_handler` | **CORRECT and no /MT regression** — latent-wrong-answer claim empirically confirmed; **but the identical bug is left live in msvcrt/msvcr100/msvcr120, which the plan told the author to check** |
| 6 | What it does NOT do | **WRONG as documented** — the three "sentinel" names are in fact live native TRAP stubs, contradicting the commit message, the report, cxxthrow.c's banner, and thunkcxx.h's own doctrine |
| 7 | The probe | **MOSTLY CORRECT** (reproduced 13/13 + sabotage red for the right reason) with four smaller defects |

## Defects, most severe first

### D1 — The "stay sentinels" safety claim is false; `__DestructExceptionObject` is a live native trap (WRONG, area 6)

Commit bc8adc33e45's message, the report (§5, §7), `dlls/guestcrt/cxxthrow.c`'s
banner ("stay sentinels pending Session B"), and the new
`dlls/ucrtbase/ucrtbase.thunks` comment ("stay holes here pending ...
Session B") all assert that `__CxxRegisterExceptionObject`,
`__CxxUnregisterExceptionObject`, `__DestructExceptionObject` are unserved
sentinels.

MEASURED: all three are exported as ordinary native trap stubs
(`mov r10,rcx; syscall`, identical body to the `_CreateFrameInfo` trap) in
BOTH built thunks:

- `wine-build/dlls/ucrtbase/x86_64-windows/ucrtbase.dll`: `__CxxRegisterExceptionObject` RVA 0x1c0c0, `__DestructExceptionObject` RVA 0x1c0e0
- `wine-build/dlls/vcruntime140/x86_64-windows/vcruntime140.dll`: RVAs 0x2030/0x2040/0x2050

Cause: they are declared at `dlls/msvcrt/msvcrt.h:156-158` (pre-existing
upstream lines, sitting immediately below the block this commit ADDED at
`msvcrt.h:147-155`), and both thunks carry `PROBE-EXTRA msvcrt.h`, so
`FROM-SPEC auto`'s oracle sees the declarations and emits traps. spec2thunk's
own documentation says a refused export gets a ZERO address-table entry and
binds a sentinel; these have real bodies — they were never refused. I
confirmed this is pre-existing by regenerating the thunk from the PRE-commit
`vcruntime140.thunks`: the trio is trapped there too (RVAs 0x2050/0x2070).

Why it matters: `__DestructExceptionObject` calls the thrown object's own
destructor — a guest x86-64 function pointer — so its native trap is exactly
the "silent wrong answer" class `thunkcxx.h` exists to refuse
("Deliberately NOT declared, so they stay refused" — but they ARE declared,
in the other header the oracle also reads). Concrete failure: the moment any
guest FH3/FH4 code exists (Session B, or a Wine-PE vcruntime140_1 loaded in
some mix) and calls `__DestructExceptionObject`, native ppc64 code will
indirect-call an x86-64 destructor address — SIGILL or silent corruption, not
a named refusal. Today nothing reaches them (FH4 is a genuine sentinel), so
this is a mis-documented armed landmine, not an active regression — but
Session A's stated safety analysis for area 6 rests on a false premise, and
the honest Session-A move under the tree's own discipline was three explicit
refusal rows. The author never read the built export table for these names.

### D2 — The staged-Proton parity story, a stated rationale of commit 1, is fiction (WRONG premise, area 4)

The commit message: "so a prefix-staged Proton vcruntime140 (itself a bare
forward to ucrtbase) resolves through the identical chain", and
`vcruntime140_1.thunks:43-48` (commit 2): "Proton stages Wine's own build ...
and THAT module's own imports ... are what vcruntime140.thunks and
ucrtbase.thunks now serve."

MEASURED, twice over:

1. The staged files in the one prefix the plan names
   (`~/.steam/steam/steamapps/compatdata/2320/pfx/drive_c/windows/system32/`)
   are the **genuine Microsoft redist**, not Wine builds: no "Wine builtin
   DLL" signature (the staged `ucrtbase.dll` HAS one; the two vcruntime
   files do not), Microsoft certificate strings, and — decisively —
   `_CxxThrowException` is that module's own code at RVA 0x5510, **not a PE
   forwarder to ucrtbase**. Its `vcruntime140_1.dll` is the real FH4 runtime
   (imports `memcpy/__processing_throw/__C_specific_handler/memmove/
   __current_exception` from VCRUNTIME140 — no ExceptionObject trio at all,
   unlike the Wine build the plan measured in Proton's `files/` dir and
   conflated with the prefix).
2. `dlls/ntdll/loader.c` (machine_dir branch, ~3540-3565, and
   `load_guest_dll`, ~3805) resolves a guest-machine module: already-loaded
   basename → **build/install-tree builtin thunk** → builtin-without-file →
   ordinary search path. The builtin thunk therefore shadows a prefix-staged
   file, the exact opposite of the plan's "the Proton-staged x86-64
   vcruntime140 shadows the builtin thunk in every Proton prefix". The
   DEAD0009 sentinel the plan attributes to a staged-forward dying at load is
   far more simply explained by the builtin lane itself (pre-change, the
   builtin vcruntime140_1 exports nothing, so `__CxxFrameHandler4` binds a
   sentinel — in every prefix).

Consequences: the FORWARD-along-the-spec-hop design is still fine and the
builtin chain is real (probe steps 1-3 prove it), but the parity rationale
repeated in the commit, the .thunks comments, and probe step 2/3's framing
("the path a prefix-staged Proton vcruntime140 takes") describes a mechanism
that does not exist; and the plan's prediction that Quake II "may partially
unblock after Session A in a Proton prefix" cannot happen — the empty builtin
vcruntime140_1 shadows the staged real FH4. The next session will plan
against this fiction unless corrected.

### D3 — The `__C_specific_handler` identity fix is left incomplete in three sibling modules the plan explicitly flagged (area 5)

MEASURED in the built thunks: `msvcrt.dll` (RVA 0xb040), `msvcr100.dll`
(0xe0d0), `msvcr120.dll` (0x10130) all still export `__C_specific_handler`
as a plain trap stub — the exact identity-defeating shape commit 1 fixes for
vcruntime140/ucrtbase (I regenerated the pre-change vcruntime140 thunk from
git and confirmed it was a trap stub at 0x2040, so the "latent wrong answer"
claim itself is CORRECT). The plan's own Session A item 3 says "(Check
whether msvcrt.thunks/msvcr100.thunks need the same line.)" — they do, the
check was not done, and neither commit nor report mentions it. Concrete
failure: the first exercised `__try` frame of any title whose `.xdata`
handler resolves through MSVCRT/MSVCR100/MSVCR120 (VS2010/2013-era /MD
titles) is entered as unknown guest code and traps into native ppc64
`__C_specific_handler` with an AMD64 `DISPATCHER_CONTEXT`.

### D4 — Probe assertion defects (area 7)

`probes/guest/cxx_throw.c:245,254` (both resolve steps):
`((ULONG_PTR)pfn & 0xffff0000ull) != 0xdead0000ull` masks ONLY bits 16-31,
so any genuine 64-bit guest address of the form `0x....dead....` (bits 16-31
== 0xdead) is falsely reported as a sentinel — a 1-in-65536-per-pointer
flaky-red in a gate whose transcripts are byte-compared. The correct test is
against the full value (`0xdead0000 <= p <= 0xdeadffff`).

Smaller: the rethrow lane (step 13) asserts `[1]==[2]==0` but not
`[0]==0x19930520` or `[3]==0` (cxxthrow.c's own comment promises `[3]` comes
out 0 via `RtlPcToFileHeader(NULL)`; nothing checks it); the sabotage lever
covers only the magic equality, so steps 9-11's comparisons have no
checks-can-fail control; and no step pins the resolved address into
guestcrt.dll's range (a same-address wrong resolution is only caught
indirectly, via step 4 failing).

### D5 — Probe placement and defaults diverge from the tree's own gate convention (area 7 / process)

`probes/check-cxx-throw.sh:54`: `BUILD=${BUILD:-$SRC}` defaults BUILD to the
SOURCE tree, which contains no `wine` loader and no built DLLs — a fresh
`./check-cxx-throw.sh` exits 2 (skip) unless the caller knows to set BUILD.
The established gate (`~/Development/powerpc64le-ports/hangover-ppc64le/probes/check-setjmp.sh:22`)
defaults to `../wine-build`, correctly. Worse, the new probe was committed
into a NEW `wine-upstream/probes/` directory while the project's actual probe
suite — including its own model — lives in the sibling
`hangover-ppc64le/probes/` directory, un-wired into `gate-suite.sh` there.

### D6 — The report's "check-setjmp.sh does not exist anywhere" claim is false; the real regression gate was never run (process)

`~/Development/powerpc64le-ports/hangover-ppc64le/probes/check-setjmp.sh`
exists (sibling of the repo, exactly where the rest of the gate suite lives)
together with `probes/guest/setjmp_regs.c`. The author searched only inside
the git repo, declared it nonexistent "in this tree or its git history"
(true and irrelevant), and substituted a weaker ad-hoc check (single lane,
no 32-entry register matrix). **I ran the real gate against the rebuilt
tree: `SETJMP-PROBE: ALL 32 CHECKS PASS`** — guestcrt's original tenant is
intact, but the author's claim of having regression-checked it was made with
a materially weaker instrument.

## The two explicit questions

**Is the image base right? — YES.** `cxxthrow.c` passes the **ThrowInfo
pointer itself** to `RtlPcToFileHeader`, not any pc: `args[3] =
RtlPcToFileHeader(pThrowInfo, &base)`. That is byte-for-byte Wine's own
`cxx_rva_base(type)` (`dlls/msvcrt/cxx.h:389-393`, used by
`dlls/msvcrt/cpp.c:902`'s `_CxxThrowException`) and matches real MSVC
throw.cpp. Because the argument is the caller's static ThrowInfo — data in
the throwing module's image — it can never be a native ppc64 pc, a
trampoline, or a thunk; and it is correct even for the cross-module case
(`std::rethrow_exception` from msvcp: the base must be the ThrowInfo's OWNING
module, which pc-of-caller schemes get wrong and this gets right).
Disassembly of RVA 0x1140 confirms rcx=pThrowInfo at the call. The probe
independently cross-checks the answer against `GetModuleHandleA(NULL)` — a
different oracle — and I reproduced 13/13. Residual, honestly held open: only
the EXE-resident-ThrowInfo case is exercised; a throw from a guest DLL
(the actual game_x64.dll shape) relies on native `RtlPcToFileHeader` seeing
guest DLLs in the shared Ldr list — near-certain (one loader, one list;
`thunkcxx.h`'s `__RTtypeid` already depends on it) but unproven by the gate.

**Did this regress the static-CRT path that already worked? — NO.** None of
the three commits touches `dlls/ntdll/signal_ppc64.c`, the ntdll thunks, or
anything a /MT image binds. The identity fast-path target —
`guest_c_specific_handler_addr()` = the guest ntdll's own export (measured
RVA 0x1e290) — is unchanged; the new FORWARDs make MORE routes land on that
one recognized address and remove none. A /MT image's personality is its own
`.text` and is served by the unchanged `call_guest_handler_run` path. The
`check-seh-handlers` steps 21-25 machinery (consolidate records) is untouched
source. Executed evidence: sibling `check-setjmp.sh` 32/32 PASS and
`check-cxx-throw.sh` 13/13 + SABOTAGE PASS, both re-run by me against the
built tree — and the cxx-throw probe itself exercises `__C_specific_handler`
dispatch, filter entry, and post-`__except` resumption end to end.

## What execution confirmed (so it is on the record)

- `guestcrt.dll` export `_CxxThrowException` RVA 0x1140 disassembles to real
  x86-64: builds `{0x19930520, obj, ti, RtlPcToFileHeader(ti)}` on its own
  stack, then `RaiseException(0xe06d7363, 1 /*NONCONTINUABLE*/, 4, args)` in
  a loop, both callees through the IAT (`kernel32.dll: RaiseException,
  RtlPcToFileHeader`, both real trap exports in the kernel32 thunk). Magic:
  VC6-only is correct — Wine's own throw side always emits VC6 and every
  handler accepts VC6..VC8 (`cppexcept.h:153-155`); real MSVC of this era
  raises with 0x19930520 too. The WinRT indirection omission is documented
  and produces a silent (not named-refusal) divergence only for WinRT throws.
- guestcrt.dll carries a 3-entry exception directory, so the search phase can
  unwind THROUGH `_CxxThrowException`'s non-leaf frame — proven live by the
  probe's filters running.
- Forwarder chains in the built PEs: vcruntime140 → `ucrtbase._CxxThrowException`
  / `ucrtbase.__C_specific_handler`; ucrtbase → `guestcrt._CxxThrowException`
  / `ntdll.__C_specific_handler`; plus the two setjmp forwards. All four
  targets exist as real exports. `vcruntime140_1.dll` thunk exports only
  `__wine_thunk_info` — commit 2's emptiness claim is byte-confirmed.
- Builtin-lane coherence (area 6, behavior): an unhandled guest throw dies
  promptly, exit 99, `err:seh:raise_pending_guest_exception guest exception
  e06d7363 ... unhandled at guest level; re-raising natively`. A real title's
  throw will now reach the first FH4 frame and get the named
  `ExceptionHandler_refused` on the 0xdeadNNNN import — strictly better than
  the old native-stack raise. No worse-failure conversion in builtin runs;
  the worse-failure risk is entirely the D1 trio, which nothing reaches yet.

## Not settled by reading, and the experiment that would settle each

1. **Does a Proton-prefix run really load the builtin thunk over the staged
   redist?** (My loader.c reading says builtin wins; the plan claims the
   opposite; both can't be right.) Experiment: `WINEDEBUG=+loaddll` run of a
   trivial guest exe doing `LoadLibraryA("vcruntime140.dll")` with
   `WINEPREFIX` pointed at a scratch COPY of a Proton-style prefix, and read
   which path got mapped.
2. **`RtlPcToFileHeader` for a ThrowInfo in a guest DLL** (the game_x64.dll
   shape). Experiment: extend cxx_throw with a small guest DLL lane — DLL
   throws through the same chain, EXE catches, assert `[3] ==
   GetModuleHandleA("the_dll")`.
3. **That the D1 trio really traps to native when called** (presence of a
   trap body is measured; the native round-trip is inferred). Experiment: a
   three-line guest probe calling `ucrtbase.__CxxRegisterExceptionObject`
   with a benign record — if it returns, the trap is live (and
   `__DestructExceptionObject`'s guest-pointer call is therefore reachable
   the day FH code calls it).
