# Fixes for the four defects in review-cxx-throw.md

Tree: `powerpc64le-ports/hangover-ppc64le/wine-upstream` (edited through the
sshfs mount), built and verified on the AC922
(`~/Development/powerpc64le-ports/hangover-ppc64le/wine-build`, GREEN before
and after). All four fixes verified; committed as three commits (the
landmine fix and the false-documentation fix are inseparable in the text and
went together, matching this tree's own precedent of one commit per
coherent triage).

## 1. The landmine: `__CxxRegisterExceptionObject`/`__CxxUnregisterExceptionObject`/`__DestructExceptionObject`

**Mechanism confirmed first**, by reading `tools/spec2thunk/spec2thunk`'s own
docs and code: `FROM-SPEC auto` emits a name as a trap the moment the header
oracle can see *any* declaration for it, regardless of session intent. There
is no way to tell it "this name is declared but must stay refused" except
`EXCLUDE <name>`, which drops the name from the emitted list entirely --
verified in the tool's own printed report as a distinct "EXCLUDED BY THE
.thunks FILE" bucket, separate from "REFUSED EXPORTS," but producing the
identical on-disk shape: zero address-table entries, so a guest import binds
ntdll's own named `0xDEADnnnn` sentinel. `GUEST-REFUSE`/`GUEST-PASS` are a
different, COM-vtable-specific mechanism (system-com-design.md classes) and
do not apply here. EXCLUDE is the right and only tool.

**Scope was bigger than the review measured.** The review found the live
traps in `ucrtbase.dll` and `vcruntime140.dll`. I wrote a from-scratch PE
export-table reader (no `winedump` in this environment --
`/tmp/pe_exports.py`, plain Python stdlib, reads the export directory by
hand) and dumped all five modules whose `.spec` declares this trio
(`ucrtbase`, `vcruntime140`, `msvcrt`, `msvcr100`, `msvcr120` -- all of them
either PROBE-EXTRA or otherwise see `msvcrt.h:156-158`, which declares all
three). **All five had the identical live-trap landmine**, not just the two
the review measured:

| module | `__CxxRegisterExceptionObject` | `__CxxUnregisterExceptionObject` | `__DestructExceptionObject` |
|---|---|---|---|
| ucrtbase.dll | RVA 0x1c0c0 | RVA 0x1c0d0 | RVA 0x1c0e0 |
| vcruntime140.dll | RVA 0x2030 | RVA 0x2040 | RVA 0x2050 |
| msvcrt.dll | RVA 0xb050 | RVA 0xb060 | RVA 0xb070 |
| msvcr100.dll | RVA 0xe0e0 | RVA 0xe0f0 | RVA 0xe100 |
| msvcr120.dll | RVA 0x10150 | RVA 0x10160 | RVA 0x10170 |

**Fix**: `EXCLUDE __CxxRegisterExceptionObject` / `EXCLUDE
__CxxUnregisterExceptionObject` / `EXCLUDE __DestructExceptionObject` added
to all five `.thunks` files.

**Evidence it worked** -- same PE reader, rebuilt tree, all five modules:

```
=== ucrtbase ===   __CxxRegisterExceptionObject   NOT IN EXPORT TABLE AT ALL
                    __CxxUnregisterExceptionObject NOT IN EXPORT TABLE AT ALL
                    __DestructExceptionObject      NOT IN EXPORT TABLE AT ALL
=== vcruntime140 ===  (same, all three)
=== msvcrt ===        (same, all three)
=== msvcr100 ===      (same, all three)
=== msvcr120 ===      (same, all three)
```

"NOT IN EXPORT TABLE AT ALL" is the exact same shape my reader reports for a
name that was *never* declared anywhere (e.g. `_CxxThrowException` in
`msvcrt.dll`, refused since before this session) -- confirming EXCLUDE
produces a genuine, indistinguishable-from-native refusal, not merely a
zeroed address-table slot. The build's own log also now prints `EXCLUDED BY
THE .thunks FILE (3): __CxxRegisterExceptionObject,
__CxxUnregisterExceptionObject, __DestructExceptionObject` for each of the
five modules.

## 2. The false documentation claims

Two separate falsehoods, corrected in place (not deleted -- each correction
says plainly what was claimed and what was actually true, per this
project's own standard):

**(a) "stays sentinels."** False from the moment it was written (see #1).
Corrected in: `dlls/guestcrt/cxxthrow.c`'s banner, `dlls/ucrtbase/
ucrtbase.thunks`' Session A paragraph, `dlls/ucrtbase/thunkcxx.h`, and (a
finding the review did not name) `dlls/msvcr100/msvcr100.thunks`' own
top-of-file banner, which has claimed since the **2026-08-17** corpus pass
(predates this triage entirely) that this trio "keeps its slot as a named
sentinel" -- also false the whole time, now corrected in place along with
its stale eligible/emitted/refused counts (766/1185/419 had drifted;
re-measured 796/1187/391). `msvcrt.thunks` and `msvcr120.thunks` carried the
same live-trap bug but never made a documentation claim about it, so those
two only needed the code fix.

**(b) The staged-Proton parity story is fiction.** I independently verified
the review's finding by reading `dlls/ntdll/loader.c`'s `find_dll_file`
(machine_dir branch, ~3540-3565) and `load_guest_dll` (~3805): for an
unqualified guest-machine import, the loader tries (1) an already-loaded
module by basename, (2) the BUILTIN thunk from the build/install tree
(`search_dll_file(machine_dir,...)` then `find_builtin_without_file`), and
only *then* (3) the ordinary search path, where a Proton-staged file would
sit. A same-named builtin thunk therefore always wins; a staged file is
**never even opened** for a plain `LoadLibraryA("vcruntime140_1.dll")` or a
static import by that name. This confirms the review's D2 finding is
correct. Corrected in: `dlls/vcruntime140/vcruntime140.thunks` (the DEAD0009
paragraph, which claimed "one bug, two symptoms" -- there was one bug, one
symptom, entirely in the builtin lane), `dlls/vcruntime140_1/
vcruntime140_1.thunks` (the paragraph the review cited at lines 43-48),
`dlls/ucrtbase/thunkcxx.h`, and `dlls/ucrtbase/ucrtbase.thunks`.

The commit message of `bc8adc33e45` itself is correctly noted as unfixable
in git history; I did not attempt to rewrite it.

## 3. `__C_specific_handler` in msvcrt/msvcr100/msvcr120

The plan's own Session A item 3 said to check whether these needed the same
line; nobody had. Confirmed live traps at exactly the RVAs the review
measured (msvcrt 0xb040, msvcr100 0xe0d0, msvcr120 0x10130). Read
`msvcr100.thunks`' long banner in full before touching it (the "what stays
Wine's own and why" comment, lines 3-20 and 117-141 in the original) --
it is entirely about the MSVC C++ EH *personality* class
(`_CxxThrowException`, `__CxxFrameHandler{,2,3}`) and about the host->guest
callback-registration table (qsort/bsearch/`_onexit`/`_set_new_handler`).
`__C_specific_handler` is neither: it is the flat SEH dispatch helper the
sibling commit already fixed identically in vcruntime140/ucrtbase for the
same reason (address-identity recognition in `dlls/ntdll/signal_ppc64.c`).
Nothing in that banner's reasoning argues for treating this module
differently, and I found no reason any of the three should be exempted --
see part 2 of the summary below.

**Fix**: `FORWARD __C_specific_handler ntdll.__C_specific_handler` added to
`msvcrt.thunks`, `msvcr100.thunks`, `msvcr120.thunks`.

**Evidence**: rebuilt PE export dump, all three:
```
msvcrt.dll   __C_specific_handler  ordinal=58   FORWARD -> ntdll.__C_specific_handler
msvcr100.dll __C_specific_handler  ordinal=292  FORWARD -> ntdll.__C_specific_handler
msvcr120.dll __C_specific_handler  ordinal=358  FORWARD -> ntdll.__C_specific_handler
```
(Previously all three were plain trap stubs at the RVAs above.)

## 4. The probe

Moved `probes/check-cxx-throw.sh` and `probes/guest/cxx_throw.c` to
`~/Development/powerpc64le-ports/hangover-ppc64le/probes/` (sibling of the
repo, where all 19 other probes including `check-setjmp.sh` live) and
removed them from the repo in the same commit.

Four defects fixed, matching `check-setjmp.sh`'s conventions exactly:

- **Sentinel mask**: `(p & 0xffff0000) != 0xdead0000` only inspected bits
  16-31 of a 64-bit pointer. Fixed to `!(p >= 0xdead0000 && p <=
  0xdeadffff)` -- the full-value range check the review specified.
- **Rethrow lane**: added two new steps asserting `ExceptionInformation[0]`
  (the magic still comes through on a rethrow) and `[3] == 0`
  (`RtlPcToFileHeader(NULL,...)` finds no module) -- previously only `[1]==
  [2]==0` was checked. The probe is now **15/15**, not 13/13.
- **Sabotage lever**: previously corrupted only the magic-number comparison
  (step 8). Broadened to corrupt every expected-pointer/base comparison by
  a fixed offset (`SABOTAGE_OFFSET`) as well, so a sabotaged build now fails
  at every one of steps 8-11 and 13-15, proving each of those comparisons
  can actually register a wrong answer.
- **BUILD default**: renamed `BUILD` to `WINE_BUILD` and defaulted it to
  `$HERE/../wine-build` (the probe's own sibling), matching
  `check-setjmp.sh` exactly, instead of defaulting to the source tree
  (which has no wine loader and no built DLLs, so a fresh run always
  skipped with exit 2).

Also dropped the stale "prefix-staged Proton" framing from the probe
source's own comments (steps 1-2), consistent with the #2 correction.

### Results from the new location

```
$ cd ~/Development/powerpc64le-ports/hangover-ppc64le/probes
$ ./check-cxx-throw.sh
...
check-cxx-throw: transcript: byte-identical to the expected 15/15 PASS
check-cxx-throw: negative control: exited 99
check-cxx-throw: negative control: the death names e06d7363: ...
check-cxx-throw: negative control: the death names guest level: ...
check-cxx-throw: PASS          (exit 0)

$ ./check-cxx-throw.sh --sabotage
check-cxx-throw: sabotage: exited 1 and printed FAIL, as corrupted expectations must
check-cxx-throw: SABOTAGE PASS  (exit 0)
```

**Fresh-environment run** (proves the `WINE_BUILD` default works): ran with
`env -i` (no `WINE_BUILD`/`BUILD`/`WINEPREFIX` set at all) from `/tmp`,
invoking the script by full path:
```
$ env -i HOME=... PATH=... FEX_*=... /bin/sh -c 'cd /tmp && \
    ~/Development/powerpc64le-ports/hangover-ppc64le/probes/check-cxx-throw.sh'
...
check-cxx-throw: PASS          (exit 0)
```

### `check-setjmp.sh` regression

```
$ ./check-setjmp.sh
-- all 32 setjmp/longjmp checks agreed:
   ...
   SETJMP-PROBE: ALL 32 CHECKS PASS   (exit 0)
```
Guestcrt's original tenant is unmoved by any of this triage's changes.

## Commit subjects (in this repo, not pushed)

1. `guestcrt,ucrtbase,vcruntime140: refuse the ExceptionObject trio for real`
   -- the landmine fix (EXCLUDE in ucrtbase/vcruntime140) plus the
   "stays sentinels" and "staged Proton parity" documentation corrections
   (they are the same paragraphs; splitting them would have been artificial).
2. `msvcrt,msvcr100,msvcr120: the plan's unchecked item, and the same trio`
   -- the `__C_specific_handler` identity fix (fix 3) plus the same
   EXCLUDE landmine fix, discovered to also be present in these three
   sibling modules while checking fix 3.
3. `probes: move check-cxx-throw.sh out of the repo, and fix its defects`
   -- fix 4.

## Should any of msvcrt/msvcr100/msvcr120 be exempted from the identity fix?

No. I read msvcr100.thunks' full banner (the one the task specifically
warned about) before touching it. Its reasoning is entirely about the C++
EH *personality* class (`_CxxThrowException`, `__CxxFrameHandler{,2,3}`,
which must stay refused because they belong to the guest image's own
unwinder) and about a separate host->guest callback-registration mechanism
(qsort/bsearch/`_onexit`/`_set_new_handler` function pointers). Neither
concern touches `__C_specific_handler`, which is the flat SEH dispatch
helper recognized by address identity -- the same class and the same fix
already applied to vcruntime140/ucrtbase. I found nothing in any of the
three modules' history or comments arguing for a different treatment, and
applied the identical fix to all three uniformly.

## Things found that the review did not name, or that I'd push back on

- **The landmine's true scope was 5 modules, not 2.** The review measured
  ucrtbase and vcruntime140; msvcrt, msvcr100 and msvcr120 carried the
  identical live-trap bug (all five `.spec` files declare the trio and all
  five oracle-see `msvcrt.h`). Fixed in all five.
- **msvcr100.thunks' own banner had an independent, pre-existing false
  claim** ("keeps its slot as a named sentinel," dated 2026-08-17, well
  before this triage) about this exact trio. Not part of the reviewed
  commits, but directly on-topic since I was already touching this file
  for the identical name; corrected in place along with its stale
  eligible/emitted/refused counts.
- **One pre-existing "staged Proton" claim I deliberately left alone**:
  `dlls/vcruntime140/vcruntime140.thunks` line ~90 ("the chain is the same
  one a prefix-staged Proton vcruntime140 takes -- so both mixes resolve
  identically"), from the 2026-08-22 setjmp/longjmp commit (`b9553bf3403`),
  predates the three reviewed commits and is the same family of fiction.
  I did not rewrite it -- out of scope for "the just-committed change" --
  but it should get the same correction in a future pass, ideally alongside
  a similar sentence in `dlls/ucrtbase/thunkcxx.h`'s original banner (line
  9, from `e9290880452`, also pre-existing).
- **I did not find anything the review got wrong.** Every MEASURED claim I
  independently re-verified (the RVAs, the loader.c resolution order, the
  EXCLUDE mechanism, spec-forward handling in `parse_wine_spec`) held up
  exactly as stated.

## Files touched

- `dlls/guestcrt/cxxthrow.c`
- `dlls/ucrtbase/ucrtbase.thunks`, `dlls/ucrtbase/thunkcxx.h`
- `dlls/vcruntime140/vcruntime140.thunks`
- `dlls/vcruntime140_1/vcruntime140_1.thunks`
- `dlls/msvcrt/msvcrt.thunks`
- `dlls/msvcr100/msvcr100.thunks`
- `dlls/msvcr120/msvcr120.thunks`
- `probes/check-cxx-throw.sh`, `probes/guest/cxx_throw.c` (removed from
  the repo; now at
  `~/Development/powerpc64le-ports/hangover-ppc64le/probes/check-cxx-throw.sh`
  and `.../probes/guest/cxx_throw.c`)
- `/tmp/pe_exports.py` (this session's verification tool, not committed --
  a plain-stdlib PE export-table reader, used since no `winedump` exists in
  this environment)
