# `__CxxFrameHandler3` for Quake II -- what was free, what was written, and what still blocks the game

Date of this entry: 2026-08-30 (continuing the 2026-08-29 guest-cxx-eh-plan.md
session). Every claim below is labeled **MEASURED** (read from a log, a
built binary, or a probe's actual output) or **INFERRED** (reasoned, with
the thing that would settle it named).

Leading with the three answers the task asked for:

1. **No free FH3 exists.** Unlike `__CxxFrameHandler4` (already real guest
   code in the staged `vcruntime140_1.dll`), nothing this port ships or
   stages carries a real x86-64 `__CxxFrameHandler3`. It had to be written.
2. **The 224-deep cycle is NOT an FH3 (or any exception-dispatch) problem.**
   It is a `CallWindowProcW`/`GetPropW` self-recursion in `dlls/user32`,
   calling back into an anonymous, non-image-backed guest page over and
   over. This is unrelated to C++ exception handling and out of this
   session's scope (`dlls/user32` is explicitly another agent's territory).
3. **Quake II still dies at rc=3, from that same unrelated cycle, at
   essentially the same point in startup.** The FH3 fix is real, wired,
   loads cleanly, and is verified working end-to-end by a new compiled
   probe -- but it was never what was killing this particular run, so
   fixing it did not by itself move Quake II further.

---

## 1. Checking for a free FH3 first (MEASURED, negative)

Per the task's own instruction to check the cheap answer before writing
anything: `winedump -j export` against every module actually staged into
the run prefix's `sysx8664` --

```
vcruntime140.dll     *** This is a Wine builtin DLL ***   (this port's own build)
vcruntime140_1.dll   *** This is a Wine builtin DLL ***   exports exactly
                      __CxxFrameHandler4, __NLG_Dispatch2, __NLG_Return2
msvcp140.dll         *** This is a Wine builtin DLL ***   no CxxFrameHandler export at all
```

None of the three exports `__CxxFrameHandler3`. Separately,
`dlls/msvcrt/msvcrt.thunks`, `dlls/msvcr100/msvcr100.thunks` and
`dlls/msvcr120/msvcr120.thunks` each independently document it as a
deliberate hole, and `find dlls -iname "*.guestpe"` shows this port has
built exactly two modules as real guest PEs before this session
(`dlls/lsteamclient/steamclient64.guestpe` and `dlls/guestcrt`) -- no
msvcrt/CRT-family module has ever been a real guest PE here, so there is no
"secretly-real" msvcrt build to forward to either. The unrelated Steam
Proton `compatdata/2320` directory *does* stage genuine Microsoft CRT
redistributables (`msvcr71.dll`..`msvcr120.dll`, real FH3 code) -- but this
port's own prefix layout (`~/.local/share/wine-ppc64le/quake2/pfx`) never
reads that directory; nothing from it is visible to this run.

Conclusion: this was the "have to write it" branch, not the "wire a
forward" branch, exactly as the task anticipated as the harder outcome.

---

## 2. The 224-deep cycle, resolved with hard addresses (MEASURED)

The task's own working hypothesis was that this was an exception-dispatch
re-entering because a personality routine was missing. **That hypothesis is
false.** Resolving both addresses:

- Launched `run-native` in the background, grabbed the child PID
  (`quake2ex_steam.exe`, pid 1392948 in the reproduction run), and read
  `/proc/1392948/maps` while it was alive.
- `guest rip=0x3fffffba01b3` and `0x3fffffba1603` both fall inside
  `3fffffba0000-3fffffba3000 r-xp 0000e000 ... dlls/user32/x86_64-windows/user32.dll`
  -- this port's own **native ppc64 build of user32's trap-stub table**
  (`.thunks`, `FROM-SPEC auto`), not any hand-written EH code.
  `winedump -f` on that build shows a `.thunk` section, `VirtAddr 0x10000`,
  `raw data offs 0xe000` -- so RVA = `0x10000 + (mapped_offset)`. Both
  addresses land exactly on `stubs_rva + index*16 + 3`, the documented
  "trap instruction" offset (`ucrtbase.thunks`'s own ABI comment: `trap_off
  = 3`). Decoding the index against `winedump -j export`'s printed table:
  - `0x3fffffba01b3` -> stub #27 = **`CallWindowProcW`** (RVA `0x101b0`).
  - `0x3fffffba1603` -> stub #352 = **`GetPropW`** (RVA `0x11600`).
- `guest rip=0x6c0000` is **not inside any file-backed image at all** --
  `/proc/<pid>/maps` shows `006c0000-006c1000 rwxp 00000000 00:00 0`, a
  private anonymous page with no file behind it. The task's own working
  label for this address ("inside the game's image") is measured wrong.
- The final line of the actual crash log names it directly:
  `err:seh:call_guest_function guest callback 00000000006C0000 failed,
  status c0000001` -- `0x6c0000` is being invoked as a **guest callback
  function pointer**, i.e. exactly the shape a window-procedure argument to
  `CallWindowProcW` takes.

So the cycle really is: something calls
`CallWindowProcW(prevWndProc=0x6c0000, ...)`. Native `CallWindowProcW`'s
real (ppc64) implementation must invoke that function pointer as guest
code -- a REVERSE crossing into `0x6c0000`. Whatever is mapped there calls
`CallWindowProcW` again with the same (or an equally bogus) previous-proc
value, and the two-element REVERSE/TRAP cycle repeats until the kernel
stack guard trips. `GetPropW` (the classic mechanism window-subclassing
code uses to retrieve a stashed "previous window procedure" pointer from a
window property) appears once, at the very innermost/final frame, exactly
where the guard fired mid-call rather than mid-cycle -- consistent with the
subclass chain being *read* via `GetPropW` and then *invoked* via
`CallWindowProcW`, with the stored value resolving to a non-image address
instead of a real window procedure.

This is a `dlls/user32` (window-subclassing / property-list) bug, not an
exception-handling one. It is explicitly out of this session's scope
(`dlls/user32`, `dlls/win32u`, `dlls/winex11.drv` are another agent's
territory this session was told to stay out of), and it happens **before**
`game_x64.dll` even finishes loading -- well before any code that would
exercise `PlayFabMultiplayerWin.dll`'s C++ exception handling gets a chance
to run.

---

## 3. What was written: `dlls/guestcrt/cxxhandler3.c`

`PlayFabMultiplayerWin.dll` (Quake II's PlayFab multiplayer SDK, an AMD64
PE) statically imports `VCRUNTIME140.__CxxFrameHandler3`
(**MEASURED**, `wine-ppc64le-native-20260830-095259-1388725.log:116`), and
died on this port's own documented hole:

```
err:module:find_forwarded_export function not found for forward
'ucrtbase.__CxxFrameHandler3' used by "C:\windows\sysx8664\vcruntime140.dll"
```

`dlls/guestcrt/cxxhandler3.c` (new file) is a from-scratch port of
`dlls/msvcrt/except.c`'s `cxx_frame_handler` / `find_catch_block` /
`find_catch_handler` / `call_catch_block` / `cxx_local_unwind`, plus
`dlls/msvcrt/cppexcept.h`'s `find_caught_type`/`copy_exception`, all
reproduced field-for-field (not `#include`d -- the private msvcrt.h-adjacent
headers pull in machinery unavailable under the guestpe recipe's
`-nostdlibinc`). It is real x86-64 guest code, for the same three reasons
`_CxxThrowException` and the exceptobj.c trio already are: it decodes an
image-relative FuncInfo table, calls guest catch/unwind funclets through
guest function pointers (the `call_funclet` assembly trampoline, byte-
identical in spirit to `dlls/msvcrt/except_x86_64.c`'s `call_exc_handler`),
and its consolidation routine (`call_catch_block`) is entered AS GUEST CODE
by `dlls/ntdll/signal_ppc64.c`'s already-implemented
`guest_consolidate_callback`.

Wiring (mirrors the existing `_CxxThrowException` two-hop chain exactly):

- `dlls/guestcrt/guestcrt.guestpe` / `guestcrt.def`: added `cxxhandler3.c`
  as a source, exported `__CxxFrameHandler`, `__CxxFrameHandler2`,
  `__CxxFrameHandler3` (the first two just call the third, matching
  `msvcrt.spec`'s own forwarding).
- `dlls/vcruntime140/vcruntime140.thunks`: `FORWARD __CxxFrameHandler[23]
  ucrtbase.__CxxFrameHandler[23]` (along the spec's own existing hop,
  `vcruntime140.spec:17-19`).
- `dlls/ucrtbase/ucrtbase.thunks`: `FORWARD __CxxFrameHandler[23]
  guestcrt.__CxxFrameHandler[23]`.
- `dlls/ucrtbase/thunkcxx.h`: comment updated (FH3 is no longer "an actual
  hole", FH4 still is).

**Named scope cuts** (see the file's own banner for the full argument, kept
short here):

- A guest `RtlLookupFunctionEntry` call on a bare AMD64 pc (upstream uses
  it only to detect a nested exception) is **sidestepped**, not attempted:
  `dispatch->ControlPc` is by construction inside `dispatch->FunctionEntry`'s
  own range, so `dispatch->FunctionEntry->BeginAddress` is exactly what
  that call would report -- the fallback guest-cxx-eh-plan.md section 6.3
  itself names, with nothing new to distrust.
- `_set_se_translator` is not implemented -- this file behaves exactly like
  upstream on a thread that never installed one (the common case), not a
  degraded case of a common one.
- An exception thrown from inside a catch handler that must **escape** the
  handler (not caught by a still-more-nested try in the same funclet) is a
  **pre-existing** limit of `guest_consolidate_callback` itself (its own
  banner: "a throw from inside the catch block... one that must escape the
  catch block does not [work]"), not something this file introduces or
  attempts to paper over. `call_catch_block` therefore does the one thing
  provably correct on the path that DOES return (register, run the
  funclet, unregister-and-destroy on a *normal* return -- the only way
  execution reaches that line), and does not bolt on Wine's own
  `__TRY`/`__FINALLY_CTX` safety net, which would require porting
  `__wine_setjmpex`/`__wine_longjmp`/`__wine_exception_handler` as GUEST
  code too (confirmed reachable in principle --
  `dlls/ntdll/signal_ppc64.c`'s `dispatch_guest_frames()` already walks
  `NtCurrentTeb()->Tib.ExceptionList` as "the Wine TEB-frame hack, in the
  one place x86-64 dispatch has room for it" -- but that is real, separate,
  untested work for its own session, not something to half-land here).

---

## 4. Verification

**Build.** `make depend && make -j96` on the AC922; clean, no new warnings.
One real gotcha, same class as the commit that named it the night before:
the first `make -j96` after editing `ucrtbase.thunks` produced a
`ucrtbase.dll` with the mtime AFTER the source edit but **without** the new
`FORWARD` lines in its export table (confirmed with `winedump -j export`);
`touch`-ing the file and rebuilding fixed it. **Always re-check the built
export table with `winedump -j export` after a `.thunks` edit** -- a build
that "ran" and "finished after the edit" is not proof the edit took.
Confirmed clean after the rebuild:

```
vcruntime140.dll: __CxxFrameHandler3 (-> ucrtbase.__CxxFrameHandler3)
ucrtbase.dll:     __CxxFrameHandler3 (-> guestcrt.__CxxFrameHandler3)
guestcrt.dll:     __CxxFrameHandler3   00001500  (real code, not a forward)
```

**No regression.** `probes/check-setjmp.sh` (32/32 PASS) and
`probes/check-cxx-throw.sh` (15/15 PASS, transcript-identical, negative
control still dies loudly) both still pass after adding `cxxhandler3.c` to
the same `guestcrt.dll` module.

**New: `probes/check-cxx-catch3.sh` + `probes/guest/cxx_catch3.cpp`,
written and PASSING this session.** No probe in this tree previously
exercised the CATCH side of C++ EH at all (`check-cxx-throw.sh` only tests
the throw and a negative control). This one compiles a real
`-target x86_64-windows-msvc -fexceptions -fcxx-exceptions` translation
unit -- confirmed with `llvm-nm` to emit genuine compiler-built
`$cppxdata`/`$tryMap`/`$handlerMap`/`$stateUnwindMap`/`$ip2state` tables,
not a hand-encoded one -- with three cases chosen to exercise the supported
(non-escaping) path only:

```
case1 (throw by value, catch by reference):        g_result=42     PASS
case2 (catch(...) fallback in a second try):        g_result=1042   PASS
case3 (throw by value, catch by value):             g_result=1052   PASS
CXX-CATCH3-PROBE: PASS
```

This is real, measured, end-to-end proof that the search phase correctly
decodes a compiler-built FuncInfo3, `ip_to_state` picks the right try scope
across two independent try/catch statements in one function,
`find_caught_type` both matches the thrown type and correctly falls through
a non-matching `catch(Obj&)` to `catch(...)`, `RtlUnwindEx`'s
`STATUS_UNWIND_CONSOLIDATE` mechanism reaches `call_catch_block`, and
`copy_exception` is correct on both the by-reference and the copy-ctor
paths. It required one non-obvious link-time fix, recorded in
`cxx_catch3_stub.c`'s own comment: an all-zero placeholder for MSVC ABI's
shared `type_info` vtable slot becomes a COMMON (tentative) definition that
`ld.lld` will not resolve against the RTTI descriptor's reference; a
non-zero initializer fixes it.

**What this does NOT prove**, named rather than implied: no probe covers
`cxx_local_unwind` (destructors of locals leaving a try scope on an
unrelated unwind, not a catch), nested try/catch, or the noexcept-violation
trap. Those remain `check_noexcept`/`cxx_local_unwind` paths exercised only
by code review against upstream's algorithm, not by a running check.

---

## 5. The robustness question (depth cap): reviewed, no change made

The task raised, separately, whether the dispatcher should recurse 224
deep at all before refusing. Having now resolved what the cycle actually
is, the existing mechanism (`add1fe20134`, the night before this session)
already answers this: it is not a silent stack exhaustion. It is a named,
diagnosed refusal today -- `emu_crossing_dump` printed the exact
alternating `TRAP CallWindowProcW` / `REVERSE 0x6c0000` pattern, its peak
depth, and (on the re-run this session, post-fix) the same shape at a
slightly different depth (202 vs. 224 -- expected run-to-run variance in
per-crossing stack cost, not a regression). It already turned this exact
bug class from a hang/silent death into something a log search resolves in
minutes, which is what this section of the task was asking for.

Adding a second, purely-numeric crossing-depth cap alongside the existing
byte-based one would be redundant with a mechanism that already works and
already named this exact bug correctly, and would add a new arbitrary
threshold, in `dlls/ntdll` (not this session's owned area), for marginal
benefit. Per the task's own "implement it only if it is clean" -- judged
not to clearly earn its keep, so nothing was changed there.

---

## 6. How far Quake II gets now (MEASURED, honest)

Ran `run-native` once more this session (`timeout 75`, exited on its own at
rc=3 before the timeout -- log
`wine-ppc64le-native-20260830-102948-1406105.log`, 6497 lines).

- **The FH3 sentinel is gone.** `grep -n 'CxxFrameHandler3\|find_forwarded_export'`
  against the new log returns nothing -- the import resolves cleanly through
  the new forward chain (no `DEAD0002` sentinel, no "function not found"
  warning). Confirmed no new errors mention `guestcrt` anywhere in the log.
- **The game still dies at rc=3**, from the identical `user32`
  `CallWindowProcW`/`GetPropW` self-recursion described in section 2 above
  (same addresses: `TRAP 0x3fffffba01b3` = `CallWindowProcW`,
  `REVERSE 0x6c0000` = the same non-image guest callback, final
  `TRAP 0x3fffffba1603` = `GetPropW`), at essentially the same point in
  startup -- well before `game_x64.dll` finishes loading, i.e. before any
  code that would actually call into `PlayFabMultiplayerWin.dll`'s C++
  exception handling has a chance to run.

**Conclusion, stated plainly**: the FH3 personality is real, wired,
verified end-to-end by a dedicated compiled probe, and removes one
confirmed hazard from the log (the sentinel/forward-resolution failure) --
but it was not, and could not have been, what was blocking this particular
Quake II launch. The actual blocker is a `dlls/user32` window-subclassing
bug, unrelated to C++ exception handling, out of this session's scope, and
still present. Quake II does not run any further than before this session,
and this report makes no claim that it does -- the only new, measured fact
about Quake II's own launch is that one specific import-resolution failure
in its log is gone, not that the game progresses.
