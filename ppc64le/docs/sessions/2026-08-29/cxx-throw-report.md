# Session A: guest-side C++ throw + export plumbing -- report

Date: 2026-08-29. Tree: `powerpc64le-ports/hangover-ppc64le/wine-upstream`
(edited through the sshfs mount), built/verified on the AC922
(`wine-build`). Full `make -j144` is green.

## 1. What was implemented, and where

- **`dlls/guestcrt/cxxthrow.c`** (new) -- `_CxxThrowException` as real x86-64
  guest code: builds the 4-word x64 exception-info array
  (`0x19930520`, object, ThrowInfo, `RtlPcToFileHeader(ThrowInfo,&base)`)
  and calls `RaiseException(0xE06D7363, EXCEPTION_NONCONTINUABLE, 4, args)`
  in a loop, matching `dlls/msvcrt/cpp.c`'s own implementation minus the
  WinRT indirection (no blocked title is WinRT). ~20 lines of real code,
  banner-commented like `setjmp.c`. `guestcrt.guestpe` grew `IMPORT kernel32`
  / `IMPORT ntdll` and a `SOURCE cxxthrow.c` line; `guestcrt.def` grew the
  export.
- **`dlls/vcruntime140/vcruntime140.thunks`** -- deleted the
  `0x00000B01` native trap line for `_CxxThrowException`; added
  `FORWARD _CxxThrowException ucrtbase._CxxThrowException` and
  `FORWARD __C_specific_handler ucrtbase.__C_specific_handler`, both
  following the module's own `.spec` hops. The `__CxxFrameHandler3` refusal
  banner was extended (not deleted) to say a FORWARD vehicle now exists and
  why FH3/FH4 still don't use it.
- **`dlls/ucrtbase/ucrtbase.thunks`** -- `FORWARD _CxxThrowException
  guestcrt._CxxThrowException`, `FORWARD __C_specific_handler
  ntdll.__C_specific_handler`, and four new trap rows: `__processing_throw`
  (citing `msvcrt.h:161`, already declared there -- it only lacked a row),
  `_CreateFrameInfo`, `_FindAndUnlinkFrame`, `_IsExceptionObjectToBeDestroyed`
  (citing `msvcrt.h`).
- **`dlls/msvcrt/msvcrt.h`** -- added declarations for
  `_FindAndUnlinkFrame`/`_IsExceptionObjectToBeDestroyed` (exported by
  `ucrtbase.spec`, previously declared nowhere).
- **`dlls/ucrtbase/thunkcxx.h`** -- comment updates only (why
  `_CxxThrowException` is no longer a plain hole; why `__processing_throw`
  needed no new declaration there).
- **`dlls/vcruntime140_1/vcruntime140_1.thunks`** -- documentation only: why
  the module is confirmed empty and why no FORWARD was added yet.
- **`probes/check-cxx-throw.sh` + `probes/guest/cxx_throw.c`** (new) -- the
  verification harness, described below.

## 2. The probe and its per-check results

`probes/check-cxx-throw.sh` (no `--sabotage`), against the rebuilt tree:

```
image: __C_specific_handler is imported from vcruntime140.dll, as a real /MD title imports it
image: exc_size=60 entries=5 ehandler=1
transcript: byte-identical to the expected 13/13 PASS
negative control: exited 99
negative control: the death names e06d7363: ...guest exception e06d7363 at 00003FFFFFB61195 unhandled at guest lev
negative control: the death names guest level: ...unhandled at guest level; re-rai
PASS
```

The 13 steps (guest transcript, byte-compared): resolve `_CxxThrowException`
via `vcruntime140.dll` (not null / not a `0xdeadNNNN` sentinel); same via
`ucrtbase.dll`; both addresses equal; normal-throw filter entered;
`ExceptionCode == 0xe06d7363`; `EXCEPTION_NONCONTINUABLE` set;
`NumberParameters == 4`; `ExceptionInformation[0] == 0x19930520`; `[1] ==
&g_object`; `[2] == &g_throwinfo`; `[3] == GetModuleHandleA(NULL)` (the
image base `RtlPcToFileHeader` must independently agree on); rethrow-spelling
filter entered; rethrow's `[1] == [2] == 0`.

`--sabotage` (rebuild with the expected magic off by one): `sabotage: exited
1 and printed FAIL, as a wrong magic must` / `SABOTAGE PASS` -- confirms the
checks can fail.

Negative control (a throw with no handler): exits 99, never resumes, and
`stderr` shows `guest exception e06d7363 ... unhandled at guest level;
re-raising natively` -- named, prompt, nonzero.

Two things the probe's design got wrong on the first pass, fixed and noted
in the script/commit: clang's `-O1` inlines both `__try`-bearing helpers
into one caller, so there is **one** function-level `EHANDLER` UNWIND_INFO
covering both try blocks, not two (bound relaxed to `>= 1`; the transcript,
not the count, proves both try blocks ran). And this port does not load a
guest `.exe` at lld's preferred `0x140000000` base the way
`check-seh-smoke.sh`'s guest DLL does -- measured here at
`0x00003fffffb61195` -- so the negative control checks the dispatcher's own
"unhandled at guest level" wording instead of an address window.

**setjmp/longjmp regression check**: `probes/check-setjmp.sh` and
`probes/guest/setjmp_regs.c`, named by both the plan and the task brief as
this file's model, **do not exist anywhere in this tree or its git
history** -- `git log --diff-filter=A -- 'probes/**'` is empty, and the
2026-08-22 commit (`b9553bf3403`) that added setjmp/longjmp says "the
guest-side probe passed 32/32" but never committed the probe itself. In its
place I wrote and ran a standalone ad hoc check (`/tmp/sj_regress.c` on the
AC922, not committed -- it duplicates no plumbing this session needed
permanently): calls `__intrinsic_setjmp` directly (bypassing `setjmp.h`'s
`setjmp()` macro, which on this `_UCRT`/x86-64 header always expands to
`_setjmpex` -> the deliberately-unforwarded `__intrinsic_setjmpex` hole),
then `longjmp`s from two frames down. Result: `sj: PASS longjmp returned 42
to the setjmp frame`, exit 0 -- guestcrt's original tenant is unmoved by
adding `cxxthrow.c` alongside it in the same module.

## 3. Byte-level proof of the forwarder chain

`spec2thunk-check` on the rebuilt `vcruntime140.dll` (`--body trap`):

```
PASS 5f every image forwarder matches a FORWARD line, target included 4 forwarder(s)
PASS 5g every FORWARD line is a forwarder in the image          4 forwarder(s)
PASS 5h every forwarded name is in the eligible export list
...
56 checks, 0 FAIL
```

The build's own thunk-generation report for `ucrtbase.dll` lists the
forward targets it resolved, by name and ordinal:

```
4  forwarded to another guest module (FORWARD lines):
   _CxxThrowException           -> guestcrt._CxxThrowException  @5
   __C_specific_handler         -> ntdll.__C_specific_handler  @34
   __intrinsic_setjmp           -> guestcrt.__intrinsic_setjmp  @78
   longjmp                      -> guestcrt.longjmp  @2293
```

A direct Python PE-export read of the rebuilt `guestcrt.dll` shows the
forwarders' actual destination is real code, distinct from setjmp/longjmp:

```
name _CxxThrowException -> RVA 0x1140
name __intrinsic_setjmp -> RVA 0x1000
name longjmp -> RVA 0x1092
```

`ucrtbase.dll`'s own `spec2thunk-check` run additionally confirms the two
new pass-through data traps and `__processing_throw` cite declarations that
really exist at the lines asserted (`msvcrt.h:147/154/155/161`). (It also
surfaces two **pre-existing** failures -- a large `_o_*`-alias "no
declaration" list and one stale `div` citation -- confirmed by diffing
against the pre-edit `ucrtbase.thunks` via `git stash`: identical failures
exist with or without this session's changes, so they are baseline noise,
not a regression.)

## 4. Was vcruntime140_1's thunk really empty?

Yes, confirmed two ways:
- `spec2thunk`/`spec2thunk-check` output: `FROM-SPEC auto` refuses the
  module's one non-stub entry (`__CxxFrameHandler4`, declared in no Wine
  header) and emits the documented "every export was refused" module.
- An independent byte-level PE read of the built 2048-byte
  `vcruntime140_1.dll`: export directory has `NumberOfFunctions = 1`,
  `NumberOfNames = 1`, and that one name is `__wine_thunk_info` (the
  always-present metadata structure) at RVA `0x1000` -- zero callable
  exports.

**No FORWARD line was added for `__CxxFrameHandler4`.** See section 6 --
this is the one place I diverged from a literal reading of the task brief,
in favor of the plan's own Session A/B split.

## 5. Can a staged Proton vcruntime140_1 now reach `__CxxFrameHandler4`?

Its own **support imports** now resolve to real code instead of sentinels:
the staged module (per the plan's measured import list) imports
`__CxxRegisterExceptionObject`, `__CxxUnregisterExceptionObject`,
`__DestructExceptionObject`, `__current_exception`, `__processing_throw`,
and `memmove` from `vcruntime140`/`ucrtbase`. Of those, `__processing_throw`
now resolves (this session); `__current_exception`/`memmove` already did;
the three `ExceptionObject` names still resolve to sentinels (see divergence
note below) -- so a staged module's FH4 code is **closer** to reachable but
not fully unblocked by Session A alone, exactly as the plan predicted
("both titles still die... at a named refusal"). I did not launch a game or
test with an actual staged Proton module in this session (no game was run);
this conclusion is read from the import list and the thunk state, not
observed live. Testing it precisely would need a Proton prefix with a
staged `vcruntime140_1.dll` and a probe that imports
`VCRUNTIME140_1.__CxxFrameHandler4` and drives a real FH4 throw/catch --
that is Session B/C's probe (`check-cxx-catch.sh`), out of this session's
scope.

## 6. Commit subjects

1. `guestcrt,vcruntime140,ucrtbase: _CxxThrowException becomes real guest code`
2. `vcruntime140_1: confirm the thunk really exports nothing, and why`
3. `probes: check-cxx-throw.sh gates guestcrt's _CxxThrowException`

(Not pushed.)

## 7. Divergences and open uncertainties

- **The "five support exports," split differently than the task brief
  literally asked.** The task said to wire `__processing_throw`,
  `__CxxRegisterExceptionObject`, `__CxxUnregisterExceptionObject`,
  `__DestructExceptionObject`, and `_CxxThrowException` all "with FORWARD
  lines the way setjmp/longjmp already are." I implemented `_CxxThrowException`
  as a FORWARD chain (as asked) and `__processing_throw` as a **plain
  ucrtbase trap** (not a FORWARD -- it never needed to be: it only hands
  back a pointer into native thread-local data, exactly like the
  already-working `__current_exception`). I left the three
  `__CxxRegisterExceptionObject`/`__CxxUnregisterExceptionObject`/
  `__DestructExceptionObject` names as **unserved sentinels**, matching the
  plan's own Session A/B split: the plan's prose explicitly says these three
  "MUST be guest code" but bundles them into **Session B**'s support file
  alongside `handler4.c`/`find_catch_handler`/`copy_exception`, not Session
  A. `__DestructExceptionObject` in particular calls the thrown object's own
  destructor -- a guest function pointer -- so it cannot be a native trap;
  I judged that implementing it (and its two callers) *now*, ahead of the
  rest of the FH3/FH4 support file it's designed to live beside, would be
  scope creep past what the plan calls "Session A: bounded; do first," and
  risks exactly the kind of half-built EH-personality-adjacent code the task
  explicitly says not to write ("DO NOT attempt to write
  `__CxxFrameHandler3` or `__CxxFrameHandler4` yourself"). I'm not fully
  certain this is the right call -- a case could be made that these three
  are self-contained enough (they don't need `find_catch_handler` or
  `copy_exception`, only `_CreateFrameInfo`-family traps and
  `__current_exception`) to have been written now. I chose the more
  conservative, plan-literal reading. Flagging this explicitly as requested.
- **`vcruntime140_1`'s `FORWARD __CxxFrameHandler4 guestcrt.__CxxFrameHandler4`
  was not added**, even though the task's item 4 says "do what the plan
  says" and reads as wanting this forward now. The plan's own text places
  that exact FORWARD line under **Session B** ("Compile Wine's own FH4
  sources into guestcrt... `vcruntime140_1.thunks`: FORWARD..."), explicitly
  because `guestcrt.__CxxFrameHandler4` doesn't exist until Session B's FH4
  guest code is written. Adding the forward now would point at a symbol
  that isn't there; I could not determine from the tooling alone whether
  that fails at build time, at module load time, or resolves to a sentinel
  the same as today's refusal -- and did not want to risk turning today's
  clean "no such export" hole into a worse, load-time forwarder-resolution
  failure for the one path (a non-Proton, builtin-only run) that currently
  hits this module at all. I read "do what the plan says" as authorizing me
  to follow the plan's own Session boundary here rather than the task
  brief's paraphrase of it, and documented the reasoning in
  `vcruntime140_1.thunks` itself.
- **`probes/check-setjmp.sh` does not exist** (section 2) -- both the plan
  and the task brief assume a committed probe from the 2026-08-22 session
  that was in fact never committed. I did not silently skip the
  "re-run" instruction; I wrote and ran an equivalent ad hoc check instead
  (not committed, since it duplicates no permanent-value plumbing) and
  reported its result plainly.
- **`ehandler` bound in the probe's own shape check** turned out to need
  `>= 1`, not the `>= 2` I initially assumed by analogy with two separate
  `__try` blocks -- see section 2. Fixed in the committed script; noted in
  its own comment and commit message so a future reader isn't surprised by
  the same thing.
- **Negative-control address assertion**: I could not use
  `check-seh-smoke.sh`'s `0x140000000`-prefix regex because this port does
  not load a guest `.exe` there (measured `0x00003fffffb61195` instead); I
  substituted a check on the dispatcher's own "unhandled at guest level"
  wording, which is arguably a *more* meaningful assertion (it is the
  dispatcher's own guest/host attribution, not an address-range guess) but
  is a divergence from the literal precedent worth calling out.
- Per the task's explicit instruction, I did **not** write `__CxxFrameHandler3`
  or `__CxxFrameHandler4`, and did not touch `dlls/combase/syscom.c`,
  `dlls/oleaut32/`, or `libs/winecom/` (verified clean at every commit,
  including recovering from one accidental git-index race where a
  concurrent `git add` from the other session briefly got swept into my
  `probes:` commit -- caught immediately from the commit's own file list,
  and repaired with `git reset --soft`/selective unstage before
  re-committing, leaving the other session's files exactly as uncommitted
  modifications, unchanged in content).
