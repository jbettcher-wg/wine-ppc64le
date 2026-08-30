# Guest-side C++ exception handling for the ppc64le port — design study

Date: 2026-08-29.  Sources: the wine-upstream tree read through the sshfs
mount, plus PE import tables of the actual blocking binaries read over ssh on
the AC922 (a stdlib-only import dumper, `/tmp/peimports.py` on that machine —
no game was launched, nothing was built, no source file was modified).

Every claim is labeled **MEASURED** (read from the tree or the binaries today)
or **INFERRED** (reasoned, with the thing that would settle it named).

---

## 1. The crux: the dispatch mechanism already serves guest personalities — the refusal is about *trap thunks*, and it should stand

**What the port does today when dispatch meets a language handler in a guest
image** (MEASURED — `dlls/ntdll/signal_ppc64.c` ~2860–3350 and README.md
"Table-based .pdata exception dispatch"):

- The frame walk builds a `DISPATCHER_CONTEXT_AMD64` and calls
  `handler(rec, EstablisherFrame, ctx, dispatch)` **as guest code** through the
  nested-run primitive (`call_guest_handler_run`), honouring the disposition.
  No identification of the handler is attempted — that is the whole point.
- `follow_guest_jmp_thunk()` (signal_ppc64.c:3186) follows `jmp *disp32(%rip)`
  and `jmp rel32` chains, because **an imported personality cannot be named by
  .xdata directly** — the linker plants a jump thunk in the image and the RVA
  names the thunk.  So the address actually entered is *whatever the import
  resolved to*.
- If that resolved address is in no guest image (a `0xDEADxxxx` sentinel, or a
  native address), `call_guest_language_handler` refuses **by name**
  (`ExceptionHandler_refused`) rather than run it.
- A handler that calls `RtlUnwindEx` from inside its run is served: the request
  is recorded, the run is ended, the walk performs the unwind, every guest
  `__finally` runs as guest code.
- `STATUS_UNWIND_CONSOLIDATE` — MSVC's spelling of `catch` — is served
  (`guest_consolidate_callback`), with the **whole eleven-slot record**
  carried, and `ppc64le/seh/check-seh-handlers.sh` steps 21–25 prove it
  field-by-field against exactly the shape Wine's own `__CxxFrameHandler`
  produces, down both unwind roads (MEASURED — README and seh_handlers.c).

**So: yes, the existing mechanism serves `__CxxFrameHandler3/4` — provided the
export the guest's IAT binds is real x86-64 guest code.**  A statically-linked
image already gets this for free (its personality is its own `.text`), and the
Cyberpunk record is consistent with static-CRT C++ throws having already gone
through this machinery ("two guest-unwinder defects GfnRuntimeSdk's C++ throw
exposed … fixed", NEXT.md item — INFERRED that GfnRuntimeSdk is /MT; its
import table would confirm).

**Why the refusal still stands, and why it is not wrong:** the
`vcruntime140.thunks` comment (lines 51–61) refuses `__CxxFrameHandler3` *as a
trap thunk* — a trap would hand the guest's `DISPATCHER_CONTEXT_AMD64` to the
native ppc64 handler, which has neither the guest frame layout nor a way to
resume x86-64.  That reasoning is correct and this plan does not undo it.
What the comment could not say on 2026-08-17 is that a third vehicle now
exists: `dlls/guestcrt` + `spec2thunk FORWARD` landed on 2026-08-22
(commit `b9553bf3403`).  A `FORWARD` export is a PE forwarder into a
guestpe-built module of **real x86-64 code**; the guest IAT then holds a guest
address, `follow_guest_jmp_thunk` lands in a guest image, and the dispatcher
enters it exactly as it enters DOOM's static copy.  **No dispatcher change is
needed.  The missing piece is purely the guest personality implementation and
the export plumbing.**  Nobody has written it yet; the refusal comment is a
correct statement about the wrong-vehicle, not a claim that the right vehicle
is impossible.

### Two latent wrong answers found while answering this (both current-tree)

1. **`vcruntime140.thunks:65` trap-thunks `_CxxThrowException` (0x00000B01) to
   native** (MEASURED — the line is in the file today, added in the 2026-08-17
   DOOM triage to close mfc140u's *load*; never called on that path).  If a
   non-staged run ever calls it, native ppc64 `_CxxThrowException` raises
   `RtlRaiseException` on the **native** stack — dispatch walks native frames,
   no guest handler is ever asked.  `dlls/ucrtbase/thunkcxx.h` (2026-08-19)
   already names this class "never right" and deliberately refuses the same
   symbol in ucrtbase.  The two files disagree; the thunkcxx.h posture is the
   correct one.  (This is also why the measured sentinel is `DEAD0009`: the
   Proton-staged x86-64 vcruntime140 shadows the builtin thunk in every Proton
   prefix, its `_CxxThrowException` is a spec forward to ucrtbase, and the
   guest ucrtbase thunk refuses the name — the forward dies at load and the
   import gets the sentinel.  MEASURED: wine's vcruntime140.spec:2 is
   `@ stdcall _CxxThrowException(ptr ptr) ucrtbase._CxxThrowException`, and
   Proton stages the CRT into `compatdata/<id>/pfx/drive_c/windows/system32/`
   — confirmed present for prefix 2320.)

2. **`__C_specific_handler` in the vcruntime140/ucrtbase guest thunks is a
   trap stub, not a forwarder** (MEASURED — no `FORWARD` or override line for
   it in either .thunks file; FROM-SPEC auto emits a declared-in-winnt.h name
   as a trap).  All four blocking binaries import
   `VCRUNTIME140.__C_specific_handler` (MEASURED, import tables below).  The
   dispatcher's identity fast-path (`guest_c_specific_handler_addr`) only
   recognises the **guest ntdll's** stub; a frame whose `.xdata` thunk resolves
   into the vcruntime140 trap stub fails the identity check, is entered "as
   guest code", and the stub then traps to the native ppc64
   `__C_specific_handler` with AMD64-shaped context/dispatcher records — a
   silent wrong answer waiting for the first /MD `__try` frame that dispatch
   crosses.  Fix is two `FORWARD __C_specific_handler ntdll.__C_specific_handler`
   lines (vcruntime140 via ucrtbase, matching the spec's own hop): the game IAT
   then holds the guest ntdll stub address and identity recognition works.
   (INFERRED consequence, not yet observed in a log — no dynamic-CRT `__try`
   has been *exercised* by an exception yet; settle by a probe that imports
   `__C_specific_handler` from vcruntime140.)

---

## 2. `_CxxThrowException`: exact contract, and yes — it is writable as guest code today

**The contract** (MEASURED from this tree — `dlls/msvcrt/cpp.c:902`,
`cppexcept.h:30–33,149`, `cxx.h:389`; matches Microsoft's documented layout):

```c
void WINAPI DECLSPEC_NORETURN _CxxThrowException( void *object, const cxx_exception_type *type )
{
    ULONG_PTR args[4];                       /* CXX_EXCEPTION_PARAMS == 4 on x64 (3 on x86) */
    args[0] = 0x19930520;                    /* CXX_FRAME_MAGIC_VC6 -- the record magic is
                                                always VC6; handlers accept 0x19930520..22 */
    args[1] = (ULONG_PTR)object;             /* the thrown object (NULL for `throw;`) */
    args[2] = (ULONG_PTR)type;               /* ThrowInfo* (NULL for `throw;`) */
    args[3] = RtlPcToFileHeader(type, &b);   /* image base of the throwing module: on x64 the
                                                ThrowInfo's internals are image-relative RVAs */
    for (;;) RaiseException( 0xE06D7363 /* 'msc'|0xE0 */, EXCEPTION_NONCONTINUABLE, 4, args );
}
```

(The WinRT `TYPE_FLAG_WINRT` indirection in Wine's version can be carried over
verbatim or dropped for v1 with a named refusal; no blocked title is WinRT.)

**Everything it needs exists in the guest namespace already:**

- `KERNEL32.RaiseException` — MEASURED, `kernel32.thunks:91`
  (`0x0000BEE7 4 void winbase.h:2170`), and a guest probe already raises
  through it (`probes/guest/a_unwind.c:18`); the seh gates prove a
  guest-raised exception dispatches over guest frames with its
  `ExceptionInformation` intact.
- `RtlPcToFileHeader` — the native implementation *sees guest images*
  (MEASURED — thunkcxx.h says so in as many words, and the __RTtypeid
  family relies on it).  Reachable via the ntdll/kernel32 thunk surface; if
  the export turns out to be a hole, the fallback is one page-walk in guest
  code (`type` → module base via the PEB `Ldr` list, which guest code can
  read), but a trap is simpler and consistent.
- `guestcrt` can grow `IMPORT kernel32` / `IMPORT ntdll` lines — guestpe
  supports imports (MEASURED — `steamclient64.guestpe` lines 65–72, and
  `tools/guestpe/guestpe` links directly against thunk-DLL export tables).

**Confirmed: this half is bounded.**  ~40 lines of guest C in
`dlls/guestcrt/cxxthrow.c`, two `FORWARD` lines
(`vcruntime140 → ucrtbase → guestcrt`, replacing the 0x0B01 trap line), plus
`ucrtbase.thunks` gaining `FORWARD _CxxThrowException guestcrt._CxxThrowException`
— which is the hop that makes the **staged Proton vcruntime140 resolve too**,
exactly as the setjmp precedent demands ("both mixes resolve identically",
commit b9553bf3403).

---

## 3. What the two blocked titles actually need (MEASURED import tables)

Read on the AC922 from the shipped binaries, 2026-08-29:

| binary | throw import | personality imports | FH3? |
|---|---|---|---|
| `Quake 2/rerelease/baseq2/game_x64.dll` | `VCRUNTIME140._CxxThrowException` | `VCRUNTIME140_1.__CxxFrameHandler4`, `VCRUNTIME140.__C_specific_handler` | **no** |
| `Quake 2/rerelease/quake2ex_steam.exe` | same | same pair | **no** |
| `Cyberpunk 2077/bin/x64/libxess.dll` | same | same pair | **no** |
| `Cyberpunk 2077/bin/x64/libxess_fg.dll` | same | same pair | **no** |

**Not one of the four binaries imports `__CxxFrameHandler3`.  All four are
FH4.**  (VS2019+ defaults to /d2FH4; a 2023 remaster and Intel's current XeSS
are exactly that era.)  This inverts the task's assumed ordering: for these
titles, FH4 is the load-bearing personality and FH3 is the future-titles /
gate-lane piece.

Also imported by all four and load-bearing for the same dispatch:
`__current_exception`, `__current_exception_context` (served — thunkcxx.h),
`__std_terminate` (served — 0x0B02), `__C_specific_handler` (mis-served — see
§1 item 2).

**Throw-only vs catch:**

- **Throw-only does not unblock either title.**  The x64 search phase asks the
  language handler of *every* EH-bearing frame from the faulting frame out.
  The throwing module's own frames come first, their `.xdata` thunks resolve
  through the IAT to `__CxxFrameHandler4` = sentinel, and
  `call_guest_language_handler` refuses (loudly, correctly) at the very first
  one.  With throw-only, both titles die at the same place with a better
  epitaph.  (INFERRED from the dispatch code; certain enough to plan on.)
- **Quake II**: `game_x64.dll` calling `_CxxThrowException` from the game
  module on the load/entry path is id's remaster using C++ EH in earnest (the
  2023 rerelease is C++; its KexEngine layer throws and catches during game
  module init — INFERRED from era and structure; the measured fact is only
  that the very first reached symbol is the throw).  Assume it must **catch**.
- **XeSS**: plausibly an error path (feature probing on non-Intel hardware
  throws and the caller catches and disables XeSS — INFERRED).  But the catch
  frames are inside `libxess.dll`/REDengine, both FH4, so even the graceful
  path needs the handler.  Same conclusion: **catch, via FH4**.

One more measured fact that shapes the plan: **Proton already stages a real
x86-64 `vcruntime140_1.dll` — Wine's own build, real `__CxxFrameHandler4`
machine code — into every Proton prefix** (MEASURED: present in
`compatdata/2320/.../system32/`, and
`Proton 9.0 (Beta)/files/lib64/wine/x86_64-windows/vcruntime140_1.dll`
disassembles to a normal PE whose imports are: kernel32 {Fls*, Heap*, QPC,
GetModuleHandleW, GetProcAddress, GetTickCount, RaiseException, RtlUnwind,
RtlUnwindEx}, ntdll {_vsnprintf}, ucrtbase {13 flat C functions + terminate},
vcruntime140 {`__CxxRegisterExceptionObject`, `__CxxUnregisterExceptionObject`,
`__DestructExceptionObject`, `__current_exception`, `__processing_throw`,
`memmove`}).  In the Quake II prefix **the FH4 code is already guest code
today** — what is missing is its *support surface*: `__processing_throw`,
`__CxxRegisterExceptionObject`, `__CxxUnregisterExceptionObject`,
`__DestructExceptionObject` all currently resolve to sentinels through the
staged vcruntime140's forwards into the guest ucrtbase thunk.

---

## 4. What a v1 is: ranked options

### Recommended shape — three sessions, each independently shippable

**Session A — the throw half plus the hygiene fixes.  Bounded; do first.**
1. `dlls/guestcrt/cxxthrow.c` — guest `_CxxThrowException` (§2), plus
   `IMPORT kernel32` (and `ntdll` if `RtlPcToFileHeader` is taken as a trap)
   in `guestcrt.guestpe`, `.def` entry.
2. `vcruntime140.thunks`: delete the `0x00000B01` trap line;
   `FORWARD _CxxThrowException ucrtbase._CxxThrowException`.
   `ucrtbase.thunks`: `FORWARD _CxxThrowException guestcrt._CxxThrowException`.
3. `FORWARD __C_specific_handler ntdll.__C_specific_handler` in
   `ucrtbase.thunks`, and `FORWARD __C_specific_handler
   ucrtbase.__C_specific_handler` in `vcruntime140.thunks` (§1 item 2) — this
   makes the identity fast-path hold for every /MD `__try` frame.  (Check
   whether `msvcrt.thunks`/`msvcr100.thunks` need the same line.)
4. Flat additions to the ucrtbase surface, all data-only on their success
   path: `__processing_throw` (add to thunkcxx.h — returns `int*` into native
   thread data, guest-writable shared memory), `_CreateFrameInfo`,
   `_FindAndUnlinkFrame`, `_IsExceptionObjectToBeDestroyed` (declared in
   `dlls/msvcrt/msvcrt.h`, so PROBE-EXTRA serves them; they only link/unlink a
   list rooted in native thread data — a trap is *correct* for them).
5. Probe `probes/check-cxx-throw.sh` (§5).

Value if shipped alone: both titles still die (see §3), but at a **named
refusal at the FH4 frame** instead of a wild pointer; the staged-CRT
resolution warnings drop; the latent trap-to-native throw and the
`__C_specific_handler` mis-serve are gone; and everything Session B needs is
in place.  Honest sizing: one session including the probe.

**Session B — `__CxxFrameHandler4` as guest code.  The unblock.**
- Compile Wine's **own** FH4 sources into guestcrt rather than hand-porting:
  `dlls/msvcrt/handler4.c` (769 lines) + `except_x86_64.c` (156 lines — the
  funclet-call assembly, compiles as guest x86-64 exactly as setjmp.c does) +
  a small support file carrying `find_catch_handler`, `copy_exception`,
  `find_caught_type`, `__DestructExceptionObject`,
  `__CxxRegister/UnregisterExceptionObject` adapted from `except.c`/`cpp.c`
  (the three ExceptionObject functions MUST be guest code — they call the
  thrown object's destructor/copy-ctor, which are guest function pointers; the
  thread-data fields they touch are reached through the
  `__current_exception()` / `__current_exception_context()` /
  `__processing_throw()` trap calls plus the `_CreateFrameInfo` trap — state
  stays native and single-sourced, code runs guest).  This is exactly the
  split Wine's own PE `vcruntime140_1.dll` already embodies (its measured
  import list *is* this design), which is strong evidence the split is sound.
- `vcruntime140_1.thunks`: `FORWARD __CxxFrameHandler4
  guestcrt.__CxxFrameHandler4` (the module currently exports nothing — its
  only callable spec entry has no oracle-visible declaration; MEASURED spec,
  INFERRED generated emptiness — confirm with `spec2thunk-check` at build
  time).  `ucrtbase.thunks`/`vcruntime140.thunks`: FORWARD the three
  ExceptionObject names to guestcrt so the **staged** Proton vcruntime140_1
  resolves through the same code.
- Known-good interactions, already gated: the FH4 `catch` is the eleven-slot
  consolidate the port already serves; `RtlUnwindEx` from a handler's run is
  recorded-and-ended; the rethrow `__TRY` inside `call_catch_block4` lives
  within the same nested run, which the walk searches (the "escapes the catch
  block" cost is already named in the source and accepted).
- Sizing: 1–2 sessions.  The genuinely new risks are compile-time (Wine's
  `__TRY` macros and `msvcrt.h` under the guestpe recipe — fallback is the
  hand-`.seh_proc` style `ppc64le/seh/seh_handlers.c` already uses) and the
  `get_se_translator()` hack (`__current_exception()[-2]` — holds cross-ISA
  because `thread_data_t` is all pointer-sized fields there, MEASURED
  msvcrt.h:195–199; assert it in the probe once se_translator matters).

**Session C — `__CxxFrameHandler3`, same infrastructure.**
- From `except.c`'s `cxx_frame_handler` (~470 lines sharing Session B's
  support file).  Two reasons it earns a session even though no blocked title
  imports it: (1) it is the only personality the gate toolchain can *emit*
  (`clang -target x86_64-windows-msvc` produces FH3 + `_CxxThrowException` —
  README, measured there), so it buys the compiled end-to-end probe lane that
  FH4 cannot have; (2) Microsoft's real msvcp140/mfc140u and every VS2015-2017
  /MD title need it.  One extra dependency FH4 does not have: its
  nested-exception detection calls `RtlLookupFunctionEntry(dispatch->ControlPc)`
  from guest code — whether the native trap answers correctly for an AMD64 pc
  must be checked (open question §6.3); if not, the call can be replaced with
  the `FunctionEntry` already present in the DISPATCHER_CONTEXT.
- `__CxxFrameHandler2`/`__CxxFrameHandler` can forward to the same entry as
  Wine's spec does.  `handler4.c`'s decoder is FH4-only; nothing is shared
  with FH3's table walk except the support file — the "separate, later piece"
  intuition in the task is right, just in the opposite order.

### Rejected shapes, and why

- **(a) throw-only as the whole v1** — unblocks nothing (§3); ship it only as
  Session A of the sequence, where its probe and hygiene fixes stand alone.
- **Trap-thunking either personality** — the existing refusal is correct;
  nothing here weakens it.
- **Serving FH4 by blessing the Proton-staged vcruntime140_1 as the plan** —
  it is measured to be present and will start working the moment Session A's
  support surface lands (worth knowing: Quake II may partially unblock after
  Session A *in a Proton prefix* for exactly this reason).  But the tree
  cannot gate on an artifact Steam stages, non-Proton prefixes would diverge,
  and the builtin vcruntime140_1 would still export nothing.  Use it as the
  free A/B check, not as the mechanism.
- **Building Wine's whole msvcrt family as real guest PEs (hangover-style)** —
  a second CRT state fighting the native one; against this tree's discipline;
  vastly more surface than two personalities and five helpers.

---

## 5. Verification: the probes

Shape copied from `probes/check-setjmp.sh` (build a freestanding guest PE
against generated thunk implibs, run under the port, require every
compile-time-known check, log to a per-check file) and
`ppc64le/seh/check-seh-handlers.sh` (`--sabotage` negative control, transcript
compare, `+seh` trace cross-check).

**`probes/check-cxx-throw.sh` + `probes/guest/cxx_throw.c` (Session A):**
- Resolves `_CxxThrowException` at run time via
  `LoadLibraryA("vcruntime140.dll")` + `GetProcAddress` — proving the full
  forwarder chain including the ucrtbase hop, exactly as the setjmp probe
  proves its chain (and a second lane via `"ucrtbase"` directly, the path the
  staged Proton CRT takes).
- A hand-written `.seh_proc`/`.seh_handler __C_specific_handler` frame (the
  seh_handlers.c idiom — clang cannot emit an MSVC-personality `__try`) whose
  filter asserts, against compile-time constants: `ExceptionCode ==
  0xE06D7363`; `EXCEPTION_NONCONTINUABLE` set; `NumberParameters == 4`;
  `ExceptionInformation[0] == 0x19930520`; `[1] == &the_static_object`;
  `[2] == &the_static_throwinfo`; `[3] == its own image base` (read from the
  PEB, compile-time-known relative to `&__ImageBase`).  Then the `__except`
  body runs and execution continues — which additionally exercises the
  `__C_specific_handler`-identity path *through a vcruntime140 import*,
  gating §1 item 2's fix.
- A `throw;`-encoding lane: `_CxxThrowException(NULL, NULL)` must arrive with
  `[1] == [2] == 0` (the rethrow spelling the handlers key on).
- Sabotage lever: `--sabotage` re-runs the probe built with `-DSABOTAGE`,
  which flips one expected constant (magic + 1) — the gate is green only if
  that run **fails**, proving the checks can fail; plus the seh gates'
  negative control (a throw with no handler at all must die promptly, nonzero,
  naming `e06d7363` and a guest pc, never exit 0).

**`probes/check-cxx-catch.sh` (Sessions B/C), two lanes:**
- **FH3 lane (compiled, the honest end-to-end):** a `-target
  x86_64-windows-msvc -fexceptions` translation unit — real compiler-emitted
  FuncInfo — throwing and catching: catch-by-value of an int (value checked),
  catch-by-ref of a struct with a counting destructor and copy-ctor
  (compile-time-known counts: ctor 1, copy 1, dtor 2), `catch(...)`, a nested
  try with rethrow caught one frame out, destructors of in-flight locals
  running in order (a transcript, byte-compared like seh_handlers), and
  execution provably resuming from the consolidation routine's return (two
  landing pads, the check-seh-handlers trick).  Linked `-nodefaultlib` against
  the vcruntime140/vcruntime140_1/ucrtbase thunk implibs so every EH symbol
  crosses the real chain.
- **FH4 lane (hand-built data):** clang cannot emit FH4, so the FH4 probe is a
  hand-authored compressed `FuncInfo4` + `.pdata` naming an imported
  `__CxxFrameHandler4` thunk — the same "hand-built because it cannot be
  compiled" posture check-seh-handlers already documents for consolidate
  records.  Upstream Wine's `dlls/vcruntime140_1/tests` (not present in this
  tree — INFERRED to exist upstream; check before writing) is the donor for
  known-good encodings.  Asserts: the same catch/dtor/rethrow set as the FH3
  lane, plus one FH4-specific: a `noexcept` frame must terminate, not
  propagate.
- Layer like the seh gates: a `+seh` re-run must show the port's own trace
  naming the guestcrt handler address as many times as the probe counted; and
  the sabotage/negative-control layer as above.
- Final acceptance for the titles themselves stays where it lives today:
  `ppc64le/games/STATUS.md` entries, `import_chain.py`-style static audit
  first (zero EH names on sentinels in a libxess/game_x64 walk), then a launch.

---

## 6. What this study could not determine, and what settles each

1. **Whether the generated vcruntime140_1 guest thunk truly exports nothing
   today** (INFERRED from spec + oracle rules).  Settle: run `spec2thunk-check`
   or read the build artifact `dlls/vcruntime140_1/x86_64-windows/` on the
   AC922 build tree.
2. **Whether Quake II / XeSS catch, or throw-and-exit** (INFERRED "catch").
   Settled for free by Session A + a Proton prefix: with the throw real and
   the staged FH4 resolving its support imports, the next run answers it —
   either gameplay or the next named refusal.
3. **Guest-called `RtlLookupFunctionEntry` on an AMD64 pc** (needed by FH3
   only).  Settle: one probe step calling it through the kernel32 thunk on the
   probe's own pc and checking `BeginAddress` against the linked value — or
   sidestep in the FH3 port by using `dispatch->FunctionEntry`.
4. **Whether Wine's `__TRY`/`msvcrt.h` compile under the guestpe recipe**
   (clang x86_64-windows-gnu, `-D_MSVCR_VER=0`).  Settle: compile
   `handler4.c` standalone with that command line before committing to the
   source-reuse shape; fallback is the hand-`.seh` idiom.
5. **The `_set_se_translator` path** (SEH→C++ translation, games do use it).
   The `[-2]` layout hack holds by layout (MEASURED msvcrt.h), but the
   translator itself is a *guest function pointer stored by a native trap*
   (`_set_se_translator` is served natively) and *called from guest handler
   code* — that direction is fine (guest calls guest), but the native
   `terminate_handler` twin called from native `terminate()` is not.  Out of
   scope for v1; named refusal if hit.
6. **Exceptions that must be caught below a nested run** — the README's
   already-named untested limit; C++ EH inside DllMain/TLS-callback runs will
   share it.  Not new work, but FH4 traffic will hit it sooner; keep the
   existing loud refusal.
7. **`__CxxDetectRethrow`, `exception_ptr`, `std::current_exception` deep
   integration** — native exception_ptr copy-ctor calls on guest objects are
   the same native-calls-guest-pointer class as `__DestructExceptionObject`;
   the guest handler writes the shared `__current_exception` slot so the
   *common* cases line up, but `std::exception_ptr` round-trips are untested.
   Later session, own probe.

---

## Appendix: file inventory for the implementing session

- `dlls/guestcrt/` — `cxxthrow.c` (new), `cxxeh.c`/`cxxeh_support.c` (new,
  Session B/C), `guestcrt.guestpe` (+IMPORT kernel32, ntdll, ucrtbase...),
  `guestcrt.def` (+exports).  Precedent: `setjmp.c` banner style, static
  asserts against `cppexcept.h` offsets, `.seh_proc` on every asm function,
  the reloc anchor, DllMain untouched.
- `dlls/vcruntime140/vcruntime140.thunks` — remove 0x0B01; add FORWARDs
  (`_CxxThrowException`, `__C_specific_handler`, later `__CxxFrameHandler3`,
  the three ExceptionObject names); extend the header comment rather than
  deleting it: the refusal of *trap-thunking* stands, the export is now served
  by guest code.
- `dlls/ucrtbase/ucrtbase.thunks` + `thunkcxx.h` — FORWARDs to guestcrt;
  `__processing_throw` declaration; `_CreateFrameInfo` family rows.
- `dlls/vcruntime140_1/vcruntime140_1.thunks` — `FORWARD __CxxFrameHandler4
  guestcrt.__CxxFrameHandler4`.
- `probes/check-cxx-throw.sh`, `probes/check-cxx-catch.sh`,
  `probes/guest/cxx_throw.c`, `cxx_catch3.cpp`, `cxx_catch4.c` + hand data.
- Reference sources to adapt, all in-tree: `dlls/msvcrt/except.c` (44–510,
  894–1010), `handler4.c`, `except_x86_64.c`, `cppexcept.h`, `cxx.h`,
  `exception_ptr.c` (later).
