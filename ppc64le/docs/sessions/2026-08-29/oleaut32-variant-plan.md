# oleaut32's 112 GUEST-REFUSE exports: triage, VariantClear wrapper design, measurements

Design study, 2026-08-29. No source modified, nothing built, nothing launched.
Tree: `powerpc64le-ports/hangover-ppc64le/wine-upstream` (read through the sshfs
mount; measurements run over ssh on the AC922). Labels: **[MEASURED]** = ran a
tool / read a table on this tree today; **[VERIFIED-LOCAL]** = read the code in
this tree; **[INFERRED]** = API semantics / reasoning, not measured here.

The project's own design doc already prescribes the shape of the fix:
`hangover-ppc64le/docs/system-com-design.md` §9.2 ("interface pointers inside
by-value aggregates: hand walkers") names `VariantClear/VariantCopy/
VariantCopyInd/PropVariantClear/PropVariantCopy` as the flat walker set, says
scalar SAFEARRAYs become pure pass-through with interface-element arrays
refused by name, and tracks (§12.7) that the oleaut32 aggregate layout gate
(`gen_layout_check.py`) is *designed but has never run* — and indeed does not
exist as a file anywhere in the tree **[MEASURED]** (`find` finds no
`gen_layout_check*`). This plan follows §9.2 and the `__wine_guest_CoGetMalloc`
precedent (`dlls/combase/syscom.c`); it invents nothing the doc has not
licensed, and it supplies the two things the doc left open: the measured layout
result and the measured title demand.

---

## 1. Triage of the 112 refusals [VERIFIED-LOCAL: names from `dlls/oleaut32/oleaut32.thunks`, signatures from `oleaut32.spec`, semantics from `dlls/oleaut32/*.c`]

All 112 accounted for; class sizes sum to 112.

| # | class | exports (count) | why refused today | correct treatment | effort |
|---|-------|-----------------|-------------------|-------------------|--------|
| A | **VARIANT walker family** | `VariantClear` `VariantCopy` `VariantCopyInd` `VariantChangeType` `VariantChangeTypeEx` (5) | VARIANT payload can be `VT_UNKNOWN`/`VT_DISPATCH` → native `IUnknown_Release`/`AddRef` through a guest-seen pointer | **wrap in v1**: hand walker per §9.2 — see §2 below. Needs one new winecom helper pair (guest-ref release/addref on a forward proxy, mechanism already exists privately as `wc_forward_release`/`proxy_addref` in `libs/winecom/winecom.c`) | small; the helper is the only new mechanism |
| B | **Var math/format over `VARIANT*`** | `VarAbs VarAdd VarAnd VarCat VarCmp VarDiv VarEqv VarFix VarIdiv VarImp VarInt VarMod VarMul VarNeg VarNot VarOr VarPow VarRound VarSub VarXor` + `VarFormat VarFormatCurrency VarFormatDateTime VarFormatFromTokens VarFormatNumber VarFormatPercent` + `VarNumFromParseNum` (27) | inputs of `VT_DISPATCH`/`VT_UNKNOWN` make the native impl call `IDispatch::Invoke(DISPID_VALUE)` to fetch the default value (Wine `VARIANT_FetchDispatchValue`) — a native call through a guest pointer | **guarded pass-through when wanted**: one shared guard (`variant_vt_is_plain()` over each in-VARIANT, incl. BYREF targets) → plain: call native; interface-bearing: refuse by name. Mechanical template ×27. **No measured title demand — keep refused in v1** | mechanical once the guard exists; zero urgency |
| C | **`Var*FromDisp`** | `VarBoolFromDisp VarBstrFromDisp VarCyFromDisp VarDateFromDisp VarDecFromDisp VarI1FromDisp VarI2FromDisp VarI4FromDisp VarI8FromDisp VarR4FromDisp VarR8FromDisp VarUI1FromDisp VarUI2FromDisp VarUI4FromDisp VarUI8FromDisp` (15) | `IDispatch*` IN argument; the common caller passes a **guest-implemented** dispatch object, so translate-in needs reverse proxies + a served `IDispatch::Invoke` reverse path | **stay refused** until the §9.2 `IDispatch::Invoke` slot walker exists; then translate-in (`winecom_to_native`) + native call is mechanical | blocked on the Invoke walker; no title demand |
| D | **SafeArray, gated pass-through** | `SafeArrayCreate(+Ex) SafeArrayCreateVector(+Ex) SafeArrayAllocDescriptor(+Ex) SafeArrayAllocData SafeArrayDestroy SafeArrayDestroyData SafeArrayDestroyDescriptor SafeArrayRedim SafeArrayGetDim SafeArrayGetElemsize SafeArrayGetLBound SafeArrayGetUBound SafeArrayGetVartype SafeArrayLock SafeArrayUnlock SafeArrayAccessData SafeArrayUnaccessData SafeArrayGetElement SafeArrayPutElement SafeArrayPtrOfIndex SafeArrayCopy SafeArrayCopyData SafeArrayGetIID SafeArraySetIID` (27, incl. both Get/Set IID which move GUID *bytes*, not vtables) | descriptor is a carrier: interface-element arrays (`FADF_UNKNOWN\|FADF_DISPATCH\|FADF_VARIANT\|FADF_RECORD\|FADF_HAVEIID`) AddRef/Release elements natively in put/get/destroy | **guarded pass-through when wanted** (design doc §9.2 says exactly this): create paths gate on the `vt` argument, op paths gate on `fFeatures`/`SafeArrayGetVartype` of the descriptor (guest memory = host memory, layout identical — §3); scalar → native, interface-element → refuse by name. Single allocator (§9.1) makes descriptor/data ownership a non-issue | mechanical guards ×27; **no measured title demand — defer** |
| E | **SafeArray record info** | `SafeArrayGetRecordInfo SafeArraySetRecordInfo` (2) | `IRecordInfo*` in/out | **stay refused**; record infos only come from class G below, which stays refused | — |
| F | **BSTR vector** | `BstrFromVector VectorFromBstr` (2) | SAFEARRAY carrier | gate: vartype `VT_UI1`/`VT_BSTR` only (BSTRs are scalars on this lane, §9.1) → native; else refuse. **Defer** — no demand | trivial |
| G | **Typelib surface** | `LoadTypeLib LoadTypeLibEx LoadRegTypeLib RegisterTypeLib RegisterTypeLibForUser GetRecordInfoFromGuids GetRecordInfoFromTypeInfo` (7) | out `ITypeLib**`/`ITypeInfo*` in/`IRecordInfo**` | the Load* trio is **exactly the CoGetMalloc shape** — call native, `__wine_com_wrap_static`/`wrap_out_iface` the out slot; `ITypeLib`/`ITypeInfo`/`IRecordInfo` are **already on the roster with full slot tables** (13/22/19 slots) [MEASURED: `interfaces_syscom.json`]. Register* take an in `ITypeLib*` (translate-in of our own wrapped-out pointer — works). **DOOM statically imports `LoadTypeLib` (#161)** [MEASURED] — wrap the Load* trio in v1.5 if DOOM is ever seen calling it; refusal binds the import today | mechanical *given the roster*, but see §7 (slot-serving depth unverified) |
| H | **ErrorInfo surface** | `CreateErrorInfo GetErrorInfo SetErrorInfo` (3) | `ICreateErrorInfo**`/`IErrorInfo**` out, `IErrorInfo*` in | `CreateErrorInfo`/`GetErrorInfo` = CoGetMalloc shape (both interfaces rostered, 8 slots each) [MEASURED]. `SetErrorInfo` = translate-in; the common in-pointer is the one `CreateErrorInfo` vended (a forward proxy → unwraps). Guest-implemented `IErrorInfo` → refuse loudly in v1 | mechanical; modest value (mfc/atl error paths) |
| I | **Dispatch helper surface** | `DispCallFunc DispGetIDsOfNames DispGetParam DispInvoke CreateStdDispatch CreateDispTypeInfo` (6) | `IDispatch*`/`ITypeInfo*` args + `DISPPARAMS` full of VARIANTs, both directions | **stay refused** — needs the bidirectional DISPPARAMS walker (§9.2 "slots") and reverse proxies; `CreateStdDispatch` additionally *builds* an object around a guest `punkOuter` | genuinely hard; needs design decisions |
| J | **Active/class object** | `GetActiveObject RegisterActiveObject DllGetClassObject` (3) | out/in `IUnknown**`, out `IClassFactory` | `DllGetClassObject` (riid+ppv) and `GetActiveObject` (wrap-out) are mechanical; `RegisterActiveObject` takes a usually-guest-implemented `IUnknown*` → reverse-gated. **Stay refused** — no demand | mixed |
| K | **Picture / Font / PropertyFrame** | `OleCreateFontIndirect OleCreatePictureIndirect OleCreatePropertyFrame OleLoadPicture OleLoadPictureEx OleLoadPicturePath OleSavePictureFile` (7) | vend `IPicture`/`IFont` (**not rostered** [MEASURED]) or take `IUnknown*` arrays | **stay refused**. The loud refusal already did the load-bearing part: mfc140u's ordinals 417–420 bind (per the thunks-file 2026-08-17 note); nothing on the measured load paths calls them | roster-blocked; no demand |
| L | **RPC user-marshal surface** | `VARIANT_UserSize VARIANT_UserMarshal VARIANT_UserUnmarshal VARIANT_UserFree LPSAFEARRAY_UserSize LPSAFEARRAY_UserMarshal LPSAFEARRAY_UserUnmarshal LPSAFEARRAY_UserFree` (8) | marshalling a VARIANT that may hold a proxy is the deep end of the carrier problem | **stay refused, permanently loud**. Callers are the RPC runtime/generated proxies, not games | should stay refused |

Honest summary: **class A (5) is the v1 wrap set and needs one small new
mechanism; classes B, D, F and the Load/Get halves of G, H, J (≈62) are
mechanical guard-or-wrap templates with no measured demand — do them
data-driven, not speculatively; classes C, E, I, K, L and the Register halves
(≈45) should stay refused** until reverse-proxied IDispatch or a roster
addition licenses them. A wrapper that guesses is worse than a refusal that
names itself; every deferral above keeps the named refusal.

Cross-module note: the same walker directly serves ole32's
`PropVariantClear`/`PropVariantCopy`/`FreePropVariantArray` (GUEST-REFUSE in
`dlls/ole32/ole32.thunks` lines 68/100/101), and **`PropVariantClear` is
statically imported by Cyberpunk2077.exe, witcher3.exe and bink2w64.dll**
[MEASURED §4]. PROPVARIANT layout is measured identical too (§3). Plan for it
in the same series. (`CoSetProxyBlanket`, the other Cyberpunk hit, is a
different shape — in `IUnknown*` proxy-security call — not part of this scope.)

---

## 2. `__wine_guest_VariantClear`, case by case on `vt`

Native reference: `dlls/oleaut32/variant.c` `VariantClear` (line 627) and
`VARIANT_ClearInd` [VERIFIED-LOCAL]. Native behaviour: validate vt
(`DISP_E_BADVARTYPE` on junk); **any `VT_BYREF` frees nothing**; non-byref:
ARRAY→`SafeArrayDestroy`, `VT_BSTR`→`SysFreeString`,
`VT_RECORD`→`IRecordInfo_RecordClear`+`Release`,
`VT_UNKNOWN`/`VT_DISPATCH`→`IUnknown_Release`; then `V_VT = VT_EMPTY`.

Wrapper home, following the worked precedent exactly: a `GUEST-IMPL
VariantClear __wine_guest_VariantClear` row in `oleaut32.thunks`; the wrapper
in native oleaut32 (a new `dlls/oleaut32/guestcom.c`, or in
`dlls/combase/syscom.c` with an `oleaut32.spec` forward the way
`ole32.__wine_guest_CoGetMalloc` forwards to combase — combase would then
declare `VariantClear` extern, which it can, since native oleaut32 exports it).
It reaches the single winecom runtime instance through combase's §4.2 helper
forwards (`__wine_com_translate_in` etc.), never by re-linking libwinecom.

**One new mechanism is required** (the only one): a guest-visible-reference
drop that goes *through the proxy*, not to the host.
`libs/winecom/winecom.c` already has it privately —
`wc_forward_release()` → `proxy_release()`: decrements the proxy's
guest-visible refcount and only on zero un-interns and releases the *one* host
reference the proxy owns [VERIFIED-LOCAL, lines ~2036-2100]. Expose as
`winecom_release_guest_seen(void*)` (and the `proxy_addref` mirror for
VariantCopy) in `include/wine/winecom.h`, with combase forwards
`__wine_com_release_guest`/`__wine_com_addref_guest`. **Calling
`winecom_unwrap` + native `IUnknown_Release(host)` instead would be a bug**:
the proxy holds exactly one host reference for its whole life; releasing the
host directly while the proxy still interns it double-frees when the proxy
later dies. The refusal-of-shortcuts here is the whole point of the wrapper.

Case analysis (wrapper runs native, guest memory is host memory, layout
identical §3):

| `vt` | what the wrapper does | why |
|---|---|---|
| `VT_EMPTY`, `VT_NULL` | pass to native `VariantClear` | native touches nothing, writes `VT_EMPTY`, `S_OK`. An already-cleared VARIANT re-enters here — idempotent, correct |
| plain scalars (`VT_I1..VT_UI8`, `VT_R4/R8`, `VT_BOOL`, `VT_ERROR`, `VT_CY`, `VT_DATE`, `VT_DECIMAL`, `VT_INT/UINT`) | pass to native | payload holds no pointer at all; layout identical (§3) so native reads the same bytes the guest wrote |
| `VT_BSTR` | pass to native | `SysFreeString` on this lane frees from the **one** allocator — the guest's BSTR came from native oleaut32 via the existing pass-through thunk (§9.1 of the design doc: "the allocator question dissolves") |
| junk / unregistered vt | pass to native | `VARIANT_ValidateType` answers `DISP_E_BADVARTYPE`, byte-identical with a native run — the probe's negative-shape leg |
| **any `VT_BYREF` combination** | pass to native | native frees *nothing* through a BYREF; it only writes `V_VT = VT_EMPTY`. No pointer is dereferenced, so even `VT_BYREF\|VT_UNKNOWN` holding a proxy is safe — the *referent* stays owned by the caller, exactly Windows semantics. (`VariantClearInd`-style deep clearing is `VariantCopyInd`'s problem, not this one's.) |
| `VT_UNKNOWN`, `VT_DISPATCH` | **do not call native.** `punkVal == NULL` → write `VT_EMPTY`, return `S_OK` (native does the same). Else classify: (a) **forward proxy** (`proxy_from_pointer` hit) → `__wine_com_release_guest(punkVal)`, write `V_VT = VT_EMPTY`, `S_OK`. (b) **anything else = guest-implemented object** → FIXME naming the export + the pointer, leave the VARIANT untouched, return `E_NOTIMPL`. v1 refuses rather than guessing; the reverse-proxy machinery (`winecom_to_native`) *could* serve it later, but a Release through a borrowed reverse proxy has ownership semantics nobody has designed yet — that is a decision, not a mechanism gap | (a) is the design doc's own sentence: "the walker recognises the proxy, drops the guest reference via `proxy_release`, clears the VT slot itself". (b) fail-closed, loudly, by name |
| `VT_SAFEARRAY` / `VT_ARRAY\|*` | `parray == NULL` → native (no-op, `S_OK`). Else read the descriptor (plain shared memory): `fFeatures & (FADF_UNKNOWN\|FADF_DISPATCH\|FADF_VARIANT\|FADF_RECORD\|FADF_HAVEIID)` → **refuse by name** (`E_NOTIMPL`, VARIANT untouched); scalar features → pass to native (`SafeArrayDestroy` of a native-allocated descriptor — and today a guest can only hold a native-made or hand-rolled descriptor; destroying a hand-rolled one is UB on Windows too) | v1 does **not** recurse into `FADF_VARIANT` arrays; recursion is v2 of the SafeArray class-D work, and a loud named refusal is the project's normal shape |
| `VT_RECORD` | `pRecInfo == NULL` → native (no-op on the record branch). Else **refuse by name**: `RecordClear` + `Release` are interface calls on a pointer that is a proxy at best (class G is refused, so guest-held `IRecordInfo` is exotic today). Do not "helpfully" swap in the unwrapped host pointer — the temporary-swap trick releases the host reference the proxy owns (same double-free as above) | fail-closed |

Return-code discipline: every refusal is `E_NOTIMPL` with a FIXME that names
`VariantClear` and the vt, matching `__wine_com_refuse`'s loudness but now
*specific*; everything served returns exactly what native returned.

`VariantCopy`/`VariantCopyInd`: same walker on the source, plus dest-clear via
the `VariantClear` walker first (native does this too); interface source =
forward proxy → `__wine_com_addref_guest` + 24-byte struct copy, `S_OK`;
guest-implemented source → refuse. `VariantCopyInd` dereferences BYREF sources
— the byref-of-interface case follows the same classify-or-refuse rule.
`VariantChangeType(Ex)`: guard — if neither source vt (after BYREF strip) nor
target vt is in {UNKNOWN, DISPATCH, RECORD, ARRAY-of-those} → native; else
refuse by name (conversions *from* DISPATCH invoke the default value).

---

## 3. Layout: MEASURED, identical

Method: the same headers the COM gate compiles against
(`-I wine-build/include -I wine-upstream/include -I wine-upstream/include/msvcrt`),
one probe TU (`/tmp/varlayout.c` on the AC922), clang 22.1.8
`-fdump-record-layouts -fsyntax-only` for **both** targets
`x86_64-windows-gnu` (guest) and `powerpc64le-linux-gnu` (native lane's data
ABI), full-file diff of the two dumps; plus a gcc-built native runner printing
`sizeof`/member offsets through the `V_*` accessor macros (gcc is what the
native PE lane actually uses). Nothing in the tree was built.

Result: **every record layout in the whole translation unit is
offset-identical**. The only textual diffs in 3495 dump lines: GUID's `Data1`
spelled `unsigned long` (windows LLP64, 4 bytes) vs `unsigned int` (4 bytes),
and `__attribute__((stdcall))` noise on function-pointer types — same offsets,
same sizes, both sides. Key values (gcc native run, matches the clang dumps
and Microsoft x64 canon):

```
VARIANT   sizeof 24  vt@0  punkVal/pdispVal/bstrVal/dblVal/parray/byref@8  decVal overlay@0
PROPVARIANT sizeof 24  vt@0  punkVal@8
SAFEARRAY sizeof 32  cDims@0 fFeatures@2 cbElements@4 cLocks@8 pvData@16 rgsabound@24
SAFEARRAYBOUND sizeof 8 (cElements@0, lLbound@4)   DECIMAL sizeof 16
DISPPARAMS sizeof 24 (rgvarg@0, cArgs@16, cNamedArgs@20)   EXCEPINFO sizeof 64   CY sizeof 8
```

Nothing diverges. The §12.7 debt remains real, though: this was a one-off
probe, not the tree's gate. A `ppc64le/syscom/gen_layout_check.py` (the
designed one; `ppc64le/dxvk/layout32.py` is the two-target-compile-and-diff
model, `gen_vtbl_check.py` the syscom-side model) should pin
VARIANT/PROPVARIANT/SAFEARRAY(+BOUND)/DECIMAL/DISPPARAMS/EXCEPINFO before the
wrapper ships, so a future header change fails a gate instead of a game.

---

## 4. What the three titles actually import from oleaut32 [MEASURED]

Method: walked every x86-64 PE under each title's Steam dir on the AC922
(static import table **and** delay-load table; parser reuses
`ppc64le/thunks/import_chain.py`'s PE class). All oleaut32 imports are by
ordinal; mapped through `oleaut32.spec`. DOOM 6 PEs, Cyberpunk 74, Witcher 3 61.

| ordinal → export | DOOM | Cyberpunk 2077 | Witcher 3 | status today |
|---|---|---|---|---|
| #2 SysAllocString | exe | exe, 7za | exe, 7za, dxcompiler | passes |
| #4/#6/#7 SysAllocStringLen/SysFreeString/SysStringLen | exe | exe(+libs), 7za | exe(+libs), dxcompiler | passes |
| #8 VariantInit | exe | — | dxcompiler | GUEST-PASS |
| **#9 VariantClear** | **exe (both)** | **Cyberpunk2077.exe**, 7za | **witcher3.exe**, 7za, dxcompiler | **REFUSED — DOOM measured hitting it at runtime** |
| **#10 VariantCopy** | **exe (both)** | 7za | 7za, dxcompiler | REFUSED |
| **#12 VariantChangeType** | **exe (both)** | — | — | REFUSED |
| #114 VarBstrFromDate, #184/#185 time conv | exe | — | — | not refused (no interface) |
| #149/#150 SysStringByteLen/SysAllocStringByteLen | #150: exe | — | dxcompiler | passes |
| **#161 LoadTypeLib** | **exe (both)** | — | — | REFUSED (class G) |
| #313 VarBstrCat | — | — | dxcompiler | not refused |
| mfc140u (runtime, via Razer Chroma; documented in the thunks file) | #113, #417–#421 | same DLL ships (CChromaEditorLibrary64) | — | #113 hole by design; 417–420 REFUSED-loud (binds); 421 passes |

And from ole32, same scan: **`PropVariantClear` — Cyberpunk2077.exe,
witcher3.exe, bink2w64.dll** (refused); `CoSetProxyBlanket` — Cyberpunk2077.exe
and witcher3.exe (refused, different shape); `CoGetMalloc` — dxcompiler/dxil
(already wrapped); DOOM additionally needs the refused drag-drop/OLE-UI set
(`CoDisconnectObject`, `CoLockObjectExternal`, `OleGetClipboard`, …) which is
outside this scope.

So of the 112, the titles' static surface touches exactly **five**:
`VariantClear`, `VariantCopy`, `VariantChangeType` (class A), `LoadTypeLib`
(class G), and the picture/font ordinals via mfc140u (class K, where loud
refusal is already the right answer). Static import ≠ called — but
`VariantClear` **is** called (the port's own seh diagnostic), and
`PropVariantClear` sits in two main executables. A v1 that wraps class A (+
ole32's PropVariant siblings) and leaves everything else refused **by name**
is the project's normal shape and covers the measured demand.

---

## 5. Ordered plan, sized honestly

1. **`winecom_release_guest_seen` / `winecom_addref_guest_seen`** in
   `libs/winecom` (thin exports over `proxy_from_pointer` +
   `proxy_release`/`proxy_addref`), + combase §4.2 forwards
   `__wine_com_release_guest`/`__wine_com_addref_guest`. Mechanical —
   mechanism exists, only the export is new. *Unblocks everything below.*
   Coordination note: `libs/winecom` is active ground (32-bit runtime, journal
   commits landing); rebase-sized, not conflict-sized.
2. **`__wine_guest_VariantClear`** per §2 + `GUEST-IMPL` row + spec export.
   Mechanical given (1); every non-mechanical corner is a named refusal the
   design doc already chose. *Unblocks: DOOM's measured runtime hit; removes
   the "unchecked caller uses uninitialised memory" hazard for three titles.*
3. **`__wine_guest_VariantCopy` / `VariantCopyInd` / `VariantChangeType(Ex)`**
   — same walker, plus addref path. Mechanical. *Unblocks DOOM's other two
   static imports.*
4. **ole32 `PropVariantClear`/`PropVariantCopy`/`FreePropVariantArray`** —
   same walker over PROPVARIANT (measured same layout; extra vts: `VT_LPSTR/
   VT_LPWSTR/VT_CLSID/VT_BLOB/…` are CoTaskMem scalars on the one-allocator
   lane → native; `VT_STREAM`/`VT_STORAGE`/vector-of-VARIANT → classify or
   refuse by name). Mostly mechanical; the PROPVARIANT vt set is wider, so
   the vt table needs one careful pass. *Unblocks Cyberpunk + Witcher main-exe
   imports.*
5. **Probe leg** (§6) lands with 2, not after it; sabotage first, per gate
   culture.
6. **`gen_layout_check.py`** for the §9.2 aggregate set, gate-run (pays the
   §12.7 debt; today's numbers in §3 become its expected table).
7. Data-driven tail, only when a title/corpus log names one: class B guard
   template, class D SafeArray gates, `LoadTypeLib` trio wrap-out (roster is
   ready; verify slot serving depth first — §7), ErrorInfo pair. Each
   mechanical; each stays refused until named by data.
8. Stays refused with no current path to wrapping: classes C, E, I, K, L,
   `RegisterActiveObject`/`RegisterTypeLib*` (reverse-gated or roster-blocked
   or RPC-internal).

---

## 6. Probe design (check-com-smoke shape: one source, two builds, byte-identical stdout, sabotage leg)

`ppc64le/syscom/com_variant_smoke.c` + `check-variant-smoke.sh` (clone of
`check-com-smoke.sh`; link native leg against `libole32.a` + `liboleaut32.a`,
guest leg's `oleaut32.def` names the thunk exports).

Identity legs (printed values are computed by Wine's own oleaut32/ole32, so
byte-identity means the guest reached the same implementation):

- **L1 scalars**: `VariantInit` → `V_VT=VT_I4, lVal=42` → `VariantClear` →
  print hr, vt-after, and the untouched-payload byte check. Also `VT_R8`,
  `VT_CY`, `VT_DECIMAL`.
- **L2 BSTR**: `SysAllocString(L"…")` into `VT_BSTR` → print `SysStringLen`,
  `VariantClear` hr, vt-after. Exercises the single-allocator claim.
- **L3 bad vt**: `V_VT = 0x1fff` → print hr (must be `DISP_E_BADVARTYPE` from
  the same `VARIANT_ValidateType` on both lanes).
- **L4 BYREF**: `VT_BYREF|VT_I4` at a stack int → clear → print hr, vt-after,
  *and the still-intact referent*. Then `VT_BYREF|VT_UNKNOWN` aimed at a live
  interface slot → clear → print hr, vt-after, then use the still-live
  interface (a method call) to prove nothing was released.
- **L5 — the leg that matters, a VARIANT holding a proxy**:
  `CreateStreamOnHGlobal` → guest gets an IStream (forward proxy through the
  existing wrapped export). `AddRef`/`Release` → print the returned counts
  (proxy serves the guest-visible count; native serves the real one — the
  *printed arithmetic* must match: 2 then 1). Put it in `VT_UNKNOWN`,
  `VariantClear` → print hr, vt-after. Then prove the release *happened*:
  a second `CreateStreamOnHGlobal` and `GetHGlobalFromStream` round-trip
  still works (no crash, no stale intern), and — mechanism layer, gate-side —
  the `+winecom` trace of the guest run must show `destroying proxy … IStream`
  caused by the wrapper, with **no** `winecom_dispatch` Release trap (the
  drop went through `__wine_com_release_guest`, not through a guest stub).
  This mirrors check-com-smoke's layer 4: layers 1–3 could pass by accident,
  the trace grep cannot.
- **L6 idempotence**: `VariantClear` twice; second is the `VT_EMPTY` case.

Divergence-by-design leg (not in the byte-identical diff — a fifth check with
per-lane expectations, like the gate's layer-4 grep):

- **L7 guest-implemented object**: the guest source hand-rolls a minimal
  IUnknown (static vtable of its own functions — x86-64 code in the guest
  build, ppc64 in the native build). Native lane: `VariantClear` releases it,
  prints `S_OK` + `released=1`. Guest lane: the wrapper must classify
  not-a-proxy and answer `E_NOTIMPL` **without touching the object or the
  VARIANT** — the guest prints the refusal and `released=0`. The check
  requires exactly that pair of outputs; a guest lane that prints `S_OK`
  means native code called an x86 vtable and MUST fail the gate.

Sabotage legs:

- `--sabotage` (existing shape): `WINEEMUNOCOMWRAP=1` → L5's stream arrives
  raw; the wrapper's classifier sees not-a-proxy and refuses where the real
  run succeeds → outputs differ → red, as it must.
- Assert-shape sabotage for the new helper: with a deliberate
  `WINEEMU`-style knob (or a one-line test hack documented in the script),
  route the VT_UNKNOWN case through `winecom_unwrap` + native Release instead
  of `__wine_com_release_guest`; L5's second stream round-trip then hits the
  double-release and the run must go red. A gate that cannot go red proves
  nothing.

---

## 7. What this study could not determine, and what settles it

- **Whether DOOM/anything calls `LoadTypeLib`, `VariantCopy`,
  `VariantChangeType` at runtime** (only `VariantClear` has a measured runtime
  hit). Settled by: one native-lane DOOM run with the refusal ERRs left on —
  the diagnostic already names each refused export on first call. Do not
  wrap G speculatively; A is worth wrapping on the static evidence alone.
- **Whether `ITypeLib`/`ITypeInfo`/`IDispatch` roster rows are *served* or
  identity-plus-refused-slots at runtime** (the JSON carries full slot tables
  — 13/22/19 slots [MEASURED] — but serving depends on the generated marshal
  tables; `ITypeInfo` slots vend further interfaces and DISPPARAMS). Settled
  by: reading the generated `__wine_com_thunk_info` tables in the built
  guest modules, or a 20-line probe QI'ing a wrapped `ITypeLib`. Gates
  whether the class-G Load* trio is truly CoGetMalloc-mechanical.
- **Guest-implemented `IUnknown` inside a VARIANT in real titles** — how often
  L7's refusal would actually fire. Settled by: the same refusal-log run;
  if a title trips it, the reverse-proxy Release ownership design (a real
  decision: who owns the borrowed reverse proxy across a Release that may
  destroy the object) gets scheduled on evidence.
- **`VarBstrFromCy`-class by-value aggregates** (ordinal 113): stays a hole by
  the thunks file's own reasoning; NOTE the built guest thunk PE now exports
  391 names including `VarBstrFromCy` [MEASURED] — the thunks-file comment
  ("stays a hole") and the built surface disagree on the surface level;
  worth one look at whether an oracle change quietly filled it, since "right
  by coincidence" was the stated objection.
- **Delay-load + `GetProcAddress` traffic** is invisible to the static scan
  (delay tables were parsed — empty of oleaut32 for all three titles; runtime
  `GetProcAddress` remains unmeasured). The refusal diagnostics cover this at
  runtime by name, which is the project's own answer to unknown surface.

Files read for this study (all absolute, via the mount):
`…/wine-upstream/dlls/oleaut32/oleaut32.thunks`, `dlls/oleaut32/oleaut32.spec`,
`dlls/oleaut32/variant.c`, `dlls/combase/syscom.c`, `dlls/combase/combase.spec`,
`dlls/ole32/ole32.thunks`, `dlls/ole32/ole32.spec`, `libs/winecom/winecom.c`,
`libs/winecom/winecom_private.h`, `include/wine/winecom.h`,
`tools/spec2thunk/spec2thunk`, `ppc64le/syscom/check-com-smoke.sh`,
`ppc64le/syscom/interfaces_syscom.json`, `ppc64le/thunks/import_chain.py`,
`ppc64le/dxvk/layout32.py`, `…/hangover-ppc64le/docs/system-com-design.md`.
Measurement artifacts on the AC922: `/tmp/oleaut_audit.py`, `/tmp/varlayout.c`,
`/tmp/layout.{x86_64-windows-gnu,powerpc64le-linux-gnu}.txt`,
`/tmp/varlayout_run.c` (+ binary in /tmp only).
