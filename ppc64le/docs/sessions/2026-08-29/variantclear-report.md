# VariantClear GUEST-IMPL wrapper — implementation report

## 1. What was implemented

Per the plan (`oleaut32-variant-plan.md`) and `system-com-design.md` §9.2,
follwing `__wine_guest_CoGetMalloc` as the worked precedent.

**Mechanism (new, minimal):** `winecom_release_guest_seen(void*)` /
`winecom_addref_guest_seen(void*)` in `libs/winecom/winecom.c` (+ declared in
`include/wine/winecom.h`) — public wrappers over the existing private
`proxy_from_pointer`+`proxy_release`/`proxy_addref` (the same operation
`wc_forward_release` already performs privately for the reverse-proxy
machinery). They return the resulting guest-visible refcount (0 for NULL or
a non-proxy pointer) rather than void, since the probe needs a value to
check. `wc_forward_release` itself was left untouched (still used by
`reverse.c`) — I added new public entry points rather than renaming/exposing
it directly, to avoid touching reverse-proxy code and to get a return value.

**Combase forwards:** `__wine_com_release_guest`/`__wine_com_addref_guest`
in `dlls/combase/combase.spec` + thin `syscom.c` implementations, appended at
the end of the spec's System COM block (ordinals here are pure port-internal
plumbing — nothing external imports combase by these ordinals — so no
renumbering risk).

**The wrapper:** `__wine_guest_VariantClear` lives in `dlls/combase/syscom.c`
(per the task's explicit direction to follow `__wine_guest_CoGetMalloc`'s own
file), forwarded from `dlls/oleaut32/oleaut32.spec`
(`__wine_guest_VariantClear(ptr) combase.__wine_guest_VariantClear`), exactly
the way `ole32.__wine_guest_CoGetMalloc` forwards to combase. combase already
`DELAYIMPORTS oleaut32` and already calls `SysAllocString`/`SysFreeString`
directly (`errorinfo.c`, `combase.c`'s own `PropVariantClear`), so calling the
real `VariantClear` the same way is not new ground. `oleaut32.thunks`'
`VariantClear` row moved from `GUEST-REFUSE` to `GUEST-IMPL`.

I did **not** forward `__wine_com_translate_in`/`__wine_com_release_guest`
from `oleaut32.spec` — nothing in oleaut32 calls them; the wrapper lives in
combase itself and calls them as ordinary intra-DLL C calls (the same
pattern `__wine_guest_GetHGlobalFromStream` already uses for
`__wine_com_translate_in`). Only `__wine_guest_VariantClear` itself needs a
forward, since GUEST-IMPL redirects resolve by *name lookup on the guest's
own native module* (verified: the built guest `oleaut32.dll` embeds the
string `__wine_guest_VariantClear` right next to `VariantClear` in its export
data).

## 2. Final case table, as built

| `vt` | wrapper action | why |
|---|---|---|
| any `VT_BYREF` combination | pass straight to native, unconditionally | native's real `VariantClear` frees nothing through BYREF at all — checked `V_ISBYREF` first, only writes `VT_EMPTY` |
| `VT_UNKNOWN`/`VT_DISPATCH`, punkVal NULL or a forward proxy | `__wine_com_release_guest(punk)`, `V_VT=VT_EMPTY`, `S_OK` | classified via `__wine_com_translate_in`, which already answers TRUE-with-NULL-host for NULL, so no separate NULL case needed |
| `VT_UNKNOWN`/`VT_DISPATCH`, anything else (guest-implemented) | `E_NOTIMPL`, FIXME naming the export/pointer/vt, VARIANT untouched | v1 refuses — a Release through a borrowed reverse proxy has no designed ownership story |
| `VT_RECORD`, `pRecInfo==NULL` | pass to native (no-op) | matches native's own null check |
| `VT_RECORD`, `pRecInfo!=NULL` | `E_NOTIMPL`, FIXME, untouched | class G's `IRecordInfo` roster doesn't exist; pointer only ever null-checked, never dereferenced |
| `VT_ARRAY\|*`/`VT_SAFEARRAY`, `parray==NULL` | pass to native (no-op) | matches native |
| same, non-NULL, `fFeatures` has none of `FADF_UNKNOWN\|DISPATCH\|VARIANT\|RECORD\|HAVEIID` | pass to native | scalar elements, `SafeArrayDestroy` safe |
| same, non-NULL, one of those bits set | `E_NOTIMPL`, FIXME, untouched | would AddRef/Release elements natively; v1 does not recurse |
| plain scalars, `VT_BSTR`, `VT_EMPTY`/`VT_NULL`, junk vt | pass to native | layout-identical, native's own validator handles junk |

**Divergence from the letter of the plan:** the plan's SAFEARRAY row doesn't
mention gating the `fFeatures` peek on validity first. I added
`syscom_variant_type_ok()`, a small mirror of oleaut32's private (so
unreachable) `VARIANT_ValidateType`, because without it a garbage `vt` with
the `VT_ARRAY` bit set but an invalid base type would make the wrapper
dereference a SAFEARRAY pointer native's own validator would have refused
before ever touching. This was not theoretical — see §3 below.

## 3. The probe and its results

`ppc64le/syscom/probes/{variant_clear_smoke.c,check-variant-clear-smoke.sh}`,
`check-com-smoke.sh`'s method (one source, built native ppc64 PE + x86-64
guest PE, byte-identical stdout required). Put under `probes/` per the task's
instruction, diverging from the `ppc64le/winecom/` module's own convention
(driver script one level up from its `probes/` source dir) — deliberately,
to keep this script unambiguously in the "safe to run directly" zone.

**Normal run — all green:**
```
native: variant_clear_smoke: PASS 23/23
guest:  variant_clear_smoke: PASS 23/23
identity: native and guest output is byte-identical apart from L7 (25 lines)
L7 native: hr=0x00000000 released=1   L7 guest: hr=0x80004001 released=0
L8 guest (interface-bearing SAFEARRAY): hr=0x80004001 ... (refused cleanly)
L10 guest (non-NULL IRecordInfo):       hr=0x80004001 ... (refused cleanly)
mechanism: guest trace shows 3 IStream proxy destruction(s) (expect 3)
PASS
```

L1-L6, L9, L11 are byte-identical scalar/BSTR/bad-vt/BYREF/idempotence/
NULL-descriptor legs. **L5 is the leg that matters**: Stream B (a live
`IStream` forward proxy from `CreateStreamOnHGlobal`) goes into
`VT_UNKNOWN`, cleared, then a *second* stream's write/seek/read/
`GetHGlobalFromStream` round-trip proves no stale intern. The refcount
arithmetic: Stream A's AddRef/Release prints `refs=2/1` (matches
`com_smoke.c`'s own pattern); the mechanism check counts exactly **3**
`"destroying proxy ... (IStream host ...)"` lines in the `+winecom` trace —
Stream A (explicit Release, L4b), Stream B (**only** via the VariantClear
under test), Stream C (explicit Release, L5's second stream) — proving the
drop went through `proxy_release` and not through some other path.

L7 (a guest-implemented `IUnknown`, static vtable built in the test's own
image) runs on both lanes and diverges by design: native really releases it
(`hr=0`, `released=1`); the guest wrapper classifies not-a-proxy and refuses
(`hr=0x80004001` = E_NOTIMPL, `released=0`). L8/L10 are **guest-only**,
compiled out of the native binary entirely (`#ifdef VARIANT_SMOKE_NO_CRT`) —
see the crash story below.

**Sabotage leg A** (`WINEEMUNOCOMWRAP=1`, the existing mechanism): fails at
"stream A still alive" before even reaching L5 — the raw pointer defeats the
classifier immediately. Goes red as required.

**Sabotage leg B** (`WINEEMUVARIANTUNSAFERELEASE=1`, the new one — a
permanent, off-by-default knob in `syscom.c` that routes the VT_UNKNOWN drop
through the host vtable directly instead of `__wine_com_release_guest`):
```
IStream proxy destructions normal=3 sabotaged=1
sabotage B: the probe's own verdicts caught the corruption too (FAIL 21/23)
sabotage B: fewer proxy destructions under sabotage, as it must
SABOTAGE PASS
```
This was more interesting than "one fewer destruction": since Stream B's
proxy is never told its refcount reached zero, it stays in the intern table
keyed by its now-dangling host address. When Stream C's fresh
`CreateStreamOnHGlobal` reuses that exact address, `winecom_wrap`'s
already-interned path hands back **Stream B's old proxy** as "Stream C" —
its round trip corrupts (`E_OUTOFMEMORY`, bytes differ) and its own Release
never reaches zero. So the destroy count drops from 3 to **1** (not 2), and
the probe's own step verdicts independently go red (21/23). Both signals are
checked in the gate script; a gate that checked only the count could in
principle be satisfied by a smaller, luckier corruption than this one turned
out to be.

**A real crash I found and fixed the test for, not the wrapper:** my first
draft ran L8 (`FADF_UNKNOWN` SAFEARRAY) and L10 (non-NULL `IRecordInfo`)
through the *native* build too, for the byte-identical diff. Native's real
`VariantClear` has no such refusal gate at all — on native, my hand-rolled,
never-allocator-made stack `SAFEARRAY` went straight into `SafeArrayDestroy`,
which apparently freed the stack address via `CoTaskMemFree`, silently
corrupting the heap; the *next* step (L10, a dummy `IRecordInfo` pointer)
then crashed the native binary outright, mid-run, losing L10/L11/
CoUninitialize/the summary entirely. This is exactly what the plan/design
doc mean by "destroying a hand-rolled one is UB on Windows too" — it isn't
hypothetical, it happened on the first run. Fix: L8/L10 now compile and run
**only** in the guest build and are checked against the guest's own output,
never diffed against a native run that cannot safely attempt them.

## 4. Layout, measured myself

Reproduced the plan's dual-target method independently: a small C file
(`ppc64le/syscom/probes/variant_layout_probe.c`) of `_Static_assert(offsetof(...) == N, ...)`
lines, compiled `-fsyntax-only` for both `x86_64-windows-gnu` and
`powerpc64le-linux-gnu` with clang 22.1.8 against this tree's own headers.
Both compiled clean:

```
VARIANT       sizeof 24   vt@0   punkVal/pdispVal/bstrVal/dblVal/parray/byref@8   decVal@0
PROPVARIANT   sizeof 24   vt@0   punkVal@8
SAFEARRAY     sizeof 32   cDims@0 fFeatures@2 cbElements@4 cLocks@8 pvData@16 rgsabound@24
SAFEARRAYBOUND sizeof 8   DECIMAL sizeof 16   DISPPARAMS sizeof 24   EXCEPINFO sizeof 64   CY sizeof 8
```

Matches the plan's numbers exactly. I chose compile-time assertions over a
runtime print-and-diff (the plan's original method) because it needs no
CRT/link step at all and fails loudly, at the specific assertion, if any
target's real layout ever diverges — a smaller, more durable artifact of the
same claim. The §12.7 debt (`gen_layout_check.py` doesn't exist as a wired
gate) is unchanged; this file is the measurement it would pin, not the gate
itself.

## 5. What stays refused, and why

Only `VariantClear` is wrapped. Everything else GUEST-REFUSE in
`oleaut32.thunks`/`ole32.thunks` stays refused:

- `VariantCopy`/`VariantCopyInd`/`VariantChangeType(Ex)` — same walker shape,
  need an addref path (`winecom_addref_guest_seen`/`__wine_com_addref_guest`
  exist now, unused by this commit) plus dest-clear-then-copy logic; no
  measured runtime demand beyond DOOM's *static* import (per the plan, only
  `VariantClear` has a measured *runtime* hit).
- ole32's `PropVariantClear`/`PropVariantCopy`/`FreePropVariantArray` — same
  walker over PROPVARIANT, but PROPVARIANT's wider vt set (`VT_LPSTR`,
  `VT_STREAM`, vector-of-VARIANT, etc.) needs its own careful pass; out of
  this task's scope (plan's §5 step 4).
- The `Var*` math/format family (27), the other 25 SafeArray ops, class G
  (`LoadTypeLib` et al.), class H/I/J/K/L — no measured demand from the
  three titles' static import surface, and each has its own unresolved
  design question (reverse-proxy `IDispatch::Invoke`, roster gaps, RPC
  internals). Not touched.

## 6. Commits (in `wine-upstream`, not pushed)

1. `winecom: winecom_release_guest_seen/addref_guest_seen for hand walkers`
   (`libs/winecom/winecom.c`, `include/wine/winecom.h`)
2. `combase,oleaut32: VariantClear stops refusing a VARIANT that might carry a proxy`
   (`dlls/combase/combase.spec`, `dlls/combase/syscom.c`,
   `dlls/oleaut32/oleaut32.spec`, `dlls/oleaut32/oleaut32.thunks`)
3. `ppc64le/syscom: a written-with-the-code smoke gate for VariantClear`
   (`ppc64le/syscom/probes/{variant_layout_probe.c,variant_clear_smoke.c,check-variant-clear-smoke.sh}`)

**A git-index note, for transparency:** another concurrently-running agent
(editing `dlls/guestcrt/` etc., same shared checkout, same top-level Claude
session) ran a plain `git commit` while my `winecom.c`/`winecom.h` changes
were sitting staged in the shared index, and its first commit
(`0a1b5fde…`, "probes: check-cxx-throw.sh …") briefly swept up my two files
under its own message. That commit was itself superseded (rewritten to
`2fc48ac17bc…` with the same subject, my files no longer in it) before I
could act — I did not touch or rewrite anything myself, just re-staged my
now-unstaged files and committed them properly once the tree was quiet. No
content was lost at any point; I verified the diff was mine, byte for byte,
before committing it under commit 1 above.

`make -j144` was run clean (exit 0, no errors) after every source change,
most recently on the final tree state including all three commits.

## 7. Uncertain / worth a second look

- The exact FIXME wording for the "guest-implemented object" and
  `VT_RECORD`/SAFEARRAY refusals is mine, not lifted from the plan verbatim
  — matches the project's "name the export and the reason" convention but
  wasn't independently reviewed.
- `syscom_variant_type_ok()` duplicates (a subset of) oleaut32's private
  `VARIANT_ValidateType` logic by hand, with a comment flagging it as
  copy-not-reinvent. If oleaut32's validator ever changes shape, this needs
  to move with it — nothing enforces that today (the same §12.7 gap noted
  above would be the natural place to pin it, if that gate is ever built).
- I did not attempt `VariantCopy`/`VariantCopyInd`/`VariantChangeType(Ex)`
  or the ole32 PropVariant siblings, per the task's explicit "do not
  opportunistically wrap more" — flagging in case that scope was meant to
  extend further than I read it.
