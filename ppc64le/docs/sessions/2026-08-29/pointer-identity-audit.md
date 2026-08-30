# Pointer-identity audit: every wrap site, traced forward — 2026-08-30

Triggered by the Quake II SDL2 wndproc recursion (fix `bf260e2784f`,
`ppc64le/docs/sessions/2026-08-29/quake2-wndproc-identity.md`): the marshal
layer's own comment had called the read-back-and-compare idiom one "no
correct program does," and SDL did exactly that, in every program that
links it.  This audit's job was to find every OTHER place that same
dismissal might be hiding, before another title finds it for us.

**Method.** Every wrap site in `dlls/ntdll/signal_ppc64.c` — the callback
pool (`wrap_guest_callback[_ex]`, `wrap_guest_wndproc`) and the COM proxy
layer (`libs/winecom`, `dlls/combase/syscom.c`) — traced forward to every
API that could hand the stored value back to the guest.  A path that only
ever *calls* the wrapped value is safe by construction (the pool's whole
job is to make the call land correctly); a path that *returns* it is a
candidate, ranked by how likely real code is to compare or chain it rather
than just call it.

## Ranked findings

| # | Path | Risk | Disposition |
|---|------|------|-------------|
| 1 | `SetUnhandledExceptionFilter` previous-filter return (kernel32, kernelbase) | **High** — compare/chain against the previous filter is documented, ordinary practice | **Fixed** |
| 2 | `GetClassLongPtr(GCLP_WNDPROC)` | **Medium** — same shape as the GWLP_WNDPROC bug, one level up; superclassing code is a plausible reader | **Fixed** |
| 3 | `GetClassInfo(Ex)A/W` `.lpfnWndProc` | **Medium** — struct-shaped route to the same field, used by real superclassing code (`GetClassInfo("EDIT", &wc)` idiom) | **Fixed** |
| 4 | `mmioInstallIOProc(A/W)` MMIO_FINDPROC / MMIO_REMOVEPROC return | **Low** — real shape, but custom RIFF I/O procs are a niche, non-game-corpus feature | **Fixed** |
| 5 | `_set_new_handler` (msvcr100) previous-handler return | **Low** — reached only via msvcp100's DllMain lookup; save-and-restore is likely, compare is possible but unmeasured | **Fixed** |
| 6 | `SetClassLongPtr(GCLP_WNDPROC)` incoming value (never wrapped at all) | Separate bug class (call-safety, not identity) — already documented in-code as deliberately absent | **Left**, documented below |
| 7 | WM_TIMER's `lParam` (the TIMERPROC address, when `SetTimer` was given a callback) | Low — DispatchMessage's own auto-dispatch consumes it before a guest wndproc would ever see it | **Checked, cleared** |
| 8 | COM proxy identity (`QueryInterface`, syscom/winecom) | — | **Checked, cleared** — already correctly interned both directions |
| 9 | Hook procedures (`SetWindowsHookEx`, `CallNextHookEx`, `UnhookWindowsHook[Ex]`) | — | **Checked, cleared** |
| 10 | `SetWindowSubclass`/`GetWindowSubclass`/`RemoveWindowSubclass` (comctl32) | — | **Checked, cleared** |
| 11 | Timer procedures (`SetTimer`), enumeration callbacks (`EnumWindows` family, `EnumFontFamilies`, DirectSound enumeration, DPA/DSA callbacks) | — | **Checked, cleared** — call-only, no getter exists |
| 12 | Vectored exception handlers (`AddVectoredExceptionHandler`) | — | **Checked, cleared** — never wrapped; pseudo-handle is the raw guest pointer |
| 13 | Thread/fiber entry points, `ThreadQuerySetWin32StartAddress` | — | **Checked, cleared** — thread starts are never routed through the callback pool (composition rule 1) |
| 14 | dinput/DirectSound device-enumeration callbacks | — | **Checked, cleared** — call-only; no COM getter in this surface returns a stored callback pointer |
| 15 | `_onexit` return value | — | **Checked, cleared** — already returns the guest's own pointer (`a[0]`), never a pool stub |

Items 1–5 are fixed in `dlls/ntdll/signal_ppc64.c`. Items 6 is a pre-existing,
already-documented gap of a *different* bug class, left alone. Items 7–15
were traced and are safe for the reasons given below.

## The fix, mechanically

`unwrap_guest_wndproc()` (the WNDPROC-specific unwrap from the Quake II fix)
is now a thin wrapper around a new general helper:

```c
static void *unwrap_guest_cb( void *fn )      /* dlls/ntdll/signal_ppc64.c:5746 */
{
    void *target;
    ULONG_PTR magic;

    if (!fn) return fn;
    LdrLockLoaderLock( 0, NULL, &magic );
    target = guest_cb_target( fn );
    LdrUnlockLoaderLock( 0, magic );
    return target ? target : fn;
}
```

`unwrap_guest_wndproc()` (signal_ppc64.c:5781) keeps its own WNDPROC-only
early return (native procs and win32u's `0xffff00nn` winproc handles pass
through unexamined) and otherwise delegates to `unwrap_guest_cb()`. Every
new fix below calls one of the two.

### 1. `SetUnhandledExceptionFilter` — highest risk on the list

Real code does exactly the comparison the WNDPROC bug hinged on. The
textbook idiom:

```c
LPTOP_LEVEL_EXCEPTION_FILTER old = SetUnhandledExceptionFilter( mine );
if (old != mine) g_prev = old;   /* avoid re-chaining to ourselves */
```

`kernelbase/debug.c`'s implementation is `InterlockedExchangePointer(
&top_filter, filter )` — it returns whatever was stored last, verbatim. The
row used to be a plain `cb_mask` entry: wrap the incoming filter, hand the
raw native return straight back. That return is our pool stub whenever the
guest itself installed the previous filter, so `old != mine` was **always
true** even when the guest had installed `mine` moments earlier — exactly
the SDL mismatch, transplanted onto crash-handler chaining instead of
window subclassing. Fixed with `emu_SetUnhandledExceptionFilter()`
(signal_ppc64.c:7064), which wraps the incoming filter and unwraps the
returned previous one via `unwrap_guest_cb()`. Table rows at
`kernel32.dll`/`kernelbase.dll` now point at it instead of the bare
`cb_mask` row.

`RtlSetUnhandledExceptionFilter` (ntdll.dll) needed no change: its
prototype is `void WINAPI RtlSetUnhandledExceptionFilter(PRTL_EXCEPTION_FILTER)`
— confirmed against `dlls/ntdll/exception.c:432` and `include/winternl.h:5381`
— it returns nothing, so there is no previous value to leak.

### 2–3. `GetClassLongPtr(GCLP_WNDPROC)` and `GetClassInfo(Ex)`

`emu_RegisterClass`/`emu_RegisterClassEx` (signal_ppc64.c) already wrap a
class's `lpfnWndProc` at registration — the same mechanism the window-level
fix patched around. Nothing had ever unwrapped it on the way back out at
the *class* level: `GetClassLongPtr(hwnd, GCLP_WNDPROC)` and
`GetClassInfo(Ex)A/W`'s `lpWndClass->lpfnWndProc` both read the class
struct straight through and would have handed the guest our stub, unwrapped
— the identical shape of miss, one level up from GWLP_WNDPROC, for a title
that superclasses its own (or another guest module's) window class by
index or by `GetClassInfo` rather than by `GetWindowLongPtr`. Superclassing
a *system* class (`GetClassInfo("EDIT", &wc)`, the common idiom) was
already safe, because a system class's `lpfnWndProc` was never wrapped in
the first place — only a guest-registered class's is.

Fixed with `emu_GetClassLongPtr()` (signal_ppc64.c:6598, `GCLP_WNDPROC =
-24`) mirroring `emu_GetWindowLongPtr`, and `emu_GetClassInfo()` /
`emu_GetClassInfoEx()` (signal_ppc64.c:6623, 6641), which call native then
patch `out->lpfnWndProc` through `unwrap_guest_wndproc()` — only on success,
so a `GetClassInfoEx` call that fails on a bad `cbSize` (and therefore never
wrote the struct) is not touched.

### 4. `mmioInstallIOProc(A/W)`

`dlls/winmm/mmio.c`'s `MMIO_InstallIOProc()`: `MMIO_INSTALLPROC` returns the
argument handed in (already correct, wrapped or not, because both sides
just agreed on the same value), but `MMIO_FINDPROC` and `MMIO_REMOVEPROC`
return whatever is **stored** for the fourCC — the pool stub, if a guest
installed it — read straight out of the internal `IOProcList`. A guest
polling "is a handler already installed for this fourCC" via
`mmioInstallIOProc(fcc, NULL, MMIO_FINDPROC)` and comparing the result to
its own proc would get the same false-mismatch the WNDPROC bug produced.
Low real-world likelihood (custom RIFF/WAVE I/O procedures are a media-
authoring feature, not something the game corpus at hand exercises), but
the fix is exactly as contained as the others: `emu_mmioInstallIOProc()`
(signal_ppc64.c:7083) wraps the incoming proc (`wide=TRUE`, `argc=4` — an
`LPMMIOPROC` returns `LRESULT`) and unwraps the return.

### 5. `_set_new_handler` (msvcr100)

Same SET-returns-the-previous-one shape, and — worth flagging on its own —
the row's *own prior comment* made the identical mistake the WNDPROC
banner did: it reasoned that a guest "CALLing the returned pointer instead
of restoring it" was the only risk, judged that "no corpus title does it,"
and stopped there, never considering **comparison**. That is the exact
dismissal this whole audit exists to distrust. Fixed with
`emu_set_new_handler()` (signal_ppc64.c:7099); the row's comment
(signal_ppc64.c ~7160) is rewritten to say so.

Only the msvcr100 mangled export was reachable in the table before this
audit — `msvcrt.dll`'s own `_set_new_handler`, and ucrtbase's plain
`_set_new_handler`/`_o__set_new_handler`, have no row at all and hand a raw
guest pointer straight to native `set_new_handler` today. That is a
**call-safety gap** (the class of bug `FlsAlloc`'s row exists to prevent —
native code eventually `bctrl`s into unwrapped x86-64 bytes), not an
identity bug, and outside this audit's scope; noted here rather than fixed,
since closing it means adding rows keyed correctly across three more
`.spec` files and confirming the CRT-runtime construction order, which is
larger and riskier than a contained identity fix.

## Adjacent, pre-existing gap left alone: `SetClassLongPtr(GCLP_WNDPROC)`

There is no `SetClassLongPtr` row in `thunk_overrides[]` at all — a guest
calling it with a raw x86-64 function pointer today hands that pointer
straight to native user32 unwrapped, which is a **call-safety** bug (native
code eventually `bctrl`s into it) of the DOOM-RegisterClass class, not an
identity bug. `signal_ppc64.c`'s own comment already names this as
deliberately absent ("nothing in the corpus calls it"). Left alone: adding
it means wrapping on write as well as (now) unwrapping on read, and is a
different, larger fix than this audit's scope.

## Checked and cleared, with evidence

**COM / syscom (`libs/winecom`, `dlls/combase/syscom.c`).** This was the
audit's most likely place to find a second big miss, and it is already
correct. `winecom_wrap()` (`libs/winecom/winecom.c:1868`) interns proxies
by `(host pointer, interface)` in a hash table (`libs/winecom/winecom.c:16`,
`:1918`–`1935`): the same host+interface pair always returns the *same*
proxy pointer, so `QueryInterface` called twice for the same object and
interface is identity-correct by construction — the property this audit
exists to protect. The harder case, a guest object passed to native and
then handed back to the guest again, is also handled: `wc_reverse_guest()`
(`winecom.c:1897`) detects when `host` is actually one of *our own* reverse
proxies (a native vtable built around a guest-implemented callback
interface, e.g. `IXAudio2EngineCallback`) and returns the guest's own
pointer directly rather than double-wrapping it — the exact "unwrap on the
way back to the guest" pattern this audit's other fixes apply by hand,
already built into the COM layer's round trip. Reverse proxies get the
symmetric treatment: `libs/winecom/reverse.c:50` states outright "Reverse
proxies are interned by (guest pointer, interface), exactly as forward
proxies are interned by (host pointer, interface)". No fix needed; this
layer already assumes what the WNDPROC bug's comment wrongly dismissed.

**Hook procedures.** `SetWindowsHookExA/W` returns an opaque `HHOOK`, never
the callback pointer itself — there is no Windows API that hands a hook
procedure's address back to the guest for comparison. `CallNextHookEx`
takes a hook handle (`hhk` is documented as ignored) and never a procedure
pointer. `UnhookWindowsHook`/`UnhookWindowsHookEx` identify the hook to
remove by re-wrapping the same guest function with the same
`(wide, argc)` shape the row used at registration
(`SetWindowsHookA/W`, `UnhookWindowsHook`: all three rows carry
`cb_wide = 1u<<1`, matching), which `wrap_guest_callback_ex`'s dedup makes
land on the identical stub — an opaque-token comparison entirely on the
native side, never exposed to the guest raw. Safe by construction.

**`SetWindowSubclass`/`GetWindowSubclass`/`RemoveWindowSubclass`
(comctl32).** All three rows wrap their proc argument with the same
`(wide=1u<<1, argc=6)` shape; comctl32 internally identifies a subclass by
`(proc, id)` and never hands the proc pointer back to the guest — it is
used purely as a native-side dedup/lookup key, which the pool's
`(guest_fn, wide, argc)` dedup makes consistent across Set/Get/Remove calls
automatically. Safe.

**Timer procedures and enumeration callbacks.** `SetTimer`'s `TIMERPROC` has
no getter in the Win32 API at all — `KillTimer` takes an id, not a
procedure — so there is no read-back path to protect. `EnumWindows` and
the whole enumeration-callback family (`EnumFontFamilies`,
`DirectSoundEnumerate[Capture]`, `DPA_*`/`DSA_*` comparators/enumerators)
are called once per item during the enumerating call and never stored
anywhere a later API could hand back to the guest — call-only, safe by
absence of a getter. One theoretical path was checked and is negligible:
`WM_TIMER`'s `lParam` carries the TIMERPROC address when a callback was
used, but per documented Windows behavior `DispatchMessage` intercepts and
calls that address directly instead of ever delivering `WM_TIMER` to the
window procedure — a guest would have to `PeekMessage` and inspect
`msg.lParam` by hand, bypassing `DispatchMessage` entirely, to ever see the
wrapped value, which no title in the corpus does.

**Vectored exception handlers.** `emu_AddVectoredExceptionHandler`
(signal_ppc64.c:4806) never touches the callback pool at all — it keeps its
own flat `guest_veh[]` table of raw guest pointers and returns "the
pseudo-handle: the guest pointer itself" (verbatim comment at
signal_ppc64.c:4826). `RemoveVectoredExceptionHandler` matches by that same
raw pointer. No wrapping occurs anywhere in this path, so no identity
mismatch is possible.

**Thread and fiber entry points.** Thread start routines are deliberately
excluded from the callback pool by design ("composition rule 1",
documented at signal_ppc64.c ~7020): `CreateThread`'s start routine is
classified at invocation time by `thread_start_is_guest_code()` in
`RtlUserThreadStart`, not wrapped at registration. There is consequently
nothing in this pool for `NtQueryInformationThread`
(`ThreadQuerySetWin32StartAddress`) or any other start-address query to
read back wrapped — whatever address Wine's own thread machinery records
is the raw guest entry point, matching Windows behavior already.

**dinput / DirectSound.** No row in `thunk_overrides[]` names any `dinput`
or `dinput8` export — device and enumeration callbacks on that surface are
COM-method arguments, marshaled by the syscom layer already audited above
(cleared). `dlls/dinput/guestthunk.c`, `dlls/dinput8/guestcom.c`, and
`dlls/dsound/guestcom.c` contain no callback-storage-and-return pattern at
all (grepped for "Callback": no hits). `DirectSoundEnumerate[Capture]A/W`
(flat exports, not COM) are call-only, covered above.

**`_onexit`.** Checked because its C-runtime contract explicitly documents
returning the argument it was given (`_onexit_t _onexit(_onexit_t
function)`, "returns a pointer to function" on success), which is exactly
the shape a program might verify with `if (_onexit(f) != f) …`.
`emu_onexit()` (signal_ppc64.c:5329) doesn't even route through the
callback pool for this — it queues the guest pointer directly in its own
`guest_atexit_funcs[]` array (run later by one native trampoline,
`run_guest_atexit_handlers`) and returns `a[0]`, the guest's own pointer,
verbatim. Already correct.

## Regression check

A throwaway guest probe (`ppc64le/seh/identity_probe.c`, not a committed
gate — no matching `check-*.sh`, built and run by hand) exercises the three
newly-fixed struct/index paths end to end: register a class with a known
`WNDPROC`, then `GetClassLongPtr(GCLP_WNDPROC)`, `GetClassInfoW`, and two
chained `SetUnhandledExceptionFilter` calls, asserting the guest reads back
exactly what it registered.

**Negative control** (signal_ppc64.c reverted to the pre-audit code via
`git stash`, rebuilt, same probe):

```
FAIL GetClassLongPtr(GCLP_WNDPROC) got=0x00003fff90810000 want=0x0000000140001240
FAIL GetClassInfoW.lpfnWndProc      got=0x00003fff90810000 want=0x0000000140001240
PASS SetUnhandledExceptionFilter first prev  got=0x0000000000000000 want=0x0000000000000000
FAIL SetUnhandledExceptionFilter second prev got=0x00003fff90810068 want=0x0000000140001480
```

(the `0x3fff9081....` addresses are native pool-stub memory, not guest
image addresses — the exact mismatch shape this audit predicted).

**With the fix** (stash popped, rebuilt):

```
PASS GetClassLongPtr(GCLP_WNDPROC) got=0x0000000140001240 want=0x0000000140001240
PASS GetClassInfoW.lpfnWndProc got=0x0000000140001240 want=0x0000000140001240
PASS SetUnhandledExceptionFilter first prev got=0x0000000000000000 want=0x0000000000000000
PASS SetUnhandledExceptionFilter second prev got=0x0000000140001480 want=0x0000000140001480
identity_probe: ALL PASS
```

`mmioInstallIOProc` and `_set_new_handler` were not probed live (no cheap
guest-reachable trigger for the former without a real audio file; the
latter is a C++-mangled export awkward to call from a no-CRT probe) — those
two fixes are verified by code reading and by the build's clean compile
against `signal_ppc64.c`'s existing `wrap_guest_callback_ex`/
`unwrap_guest_cb` machinery, which the probe above does exercise for the
identical code paths (`wrap_guest_callback`, `unwrap_guest_cb`, the loader-
lock discipline).

Build: `make -j64` in `wine-build`, zero errors/warnings attributed to
`signal_ppc64.c` in either state.

## Files touched

- `dlls/ntdll/signal_ppc64.c` — `unwrap_guest_cb()` (new, general helper),
  `unwrap_guest_wndproc()` (refactored onto it, unchanged behavior),
  `emu_GetClassLongPtr()`, `emu_GetClassInfo()`, `emu_GetClassInfoEx()`
  (new), `emu_SetUnhandledExceptionFilter()`, `emu_mmioInstallIOProc()`,
  `emu_set_new_handler()` (new), and the corresponding `thunk_overrides[]`
  rows.
- `ppc64le/seh/identity_probe.c` — new, ad hoc verification probe (not a
  committed gate).
