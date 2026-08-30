# Adversarial review: VariantClear GUEST-IMPL series (3e6c4f3515c, 5e150fa1fb6, bf801dd6512)

Reviewed 2026-08-29, read-only. Every claim below was verified against the code
in `wine-upstream` (paths below are under
`/home/jbettcher/Development/power9_development/powerpc64le-ports/hangover-ppc64le/wine-upstream/`),
and the two experiments at the bottom were run live on the AC922. I re-ran the
gate under review myself (both normal and `--sabotage`) rather than trusting the
report, and wrote and ran my own adversarial probe for the corners the author's
probe does not cover.

## Verdict table

| # | Area | Verdict |
|---|------|---------|
| 1 | Reference counting | **CORRECT** — verified by reading and by live experiment |
| 2 | Public helpers | **CORRECT**, contract safe for the shipped caller; misuse fails closed (verified live); one theoretical TOCTOU inherited from the private paths |
| 3 | `syscom_variant_type_ok` | **CORRECT** — exact logical mirror of `VARIANT_ValidateType`, closes a real hazard, introduces no new refusals itself |
| 4 | Case table | **CORRECT** for safety on every combination I could construct; two deliberate fail-closed divergences with a real functional cost (see D2) |
| 5 | Clears to VT_EMPTY | **CORRECT** on every served path; refusal paths deliberately leave the VARIANT valid-and-untouched — a leak risk for hr-ignoring callers, not the uninitialised-memory hazard |
| 6 | The probe | **CORRECT/STRONG** — independently re-run, both sabotage legs go red for the right reason; the L8/L10 asymmetry is legitimate; minor gaps noted (D5) |
| 7 | Collateral | **CORRECT** — no consumer disturbed; but the commit renumbers combase auto-ordinals contrary to the spec file's own discipline comment and the report's claim (D1) |

## The three headline questions

**Can this double-free?** No path found. The wrapper's only host-direct release
is behind `WINEEMUVARIANTUNSAFERELEASE=1` (off by default, exists as the negative
control). On every real path the drop goes `__wine_com_release_guest` →
`winecom_release_guest_seen` → `proxy_from_pointer` + `proxy_release`
(`libs/winecom/winecom.c:2128`), which decrements only the guest-visible count
and releases the proxy's single host reference exactly once, at zero, under
`wc_cs` (`winecom.c:2072-2105`). `winecom_wrap`'s surplus-reference discipline
(`winecom.c:1924-1929`: intern hit → `p->refs++` + `host_release_iface` of the
caller's surplus host ref) means the proxy holds exactly one host reference no
matter how many guest refs exist — verified consistent with the release side.

**Can it leak?** Yes, on the refusal rows, by design: a guest-implemented
`IUnknown`/`IDispatch`, a non-NULL `pRecInfo`, an interface-flagged SAFEARRAY,
or a proxy belonging to a *different* winecom surface (a d3d11/d3d12/dxgi proxy
inside a VARIANT — combase's instance classifies it "not ours" and refuses)
returns `E_NOTIMPL` with the resource still in the VARIANT. A caller that
ignores the hr (most do) leaks it. That is the project's stated fail-closed
shape; it is a functional gap, not a safety bug.

**Can it hand a guest a native vtable?** No. The wrapper writes only
`VT_EMPTY` into the VARIANT; no path stores a host pointer into guest-visible
memory. The one route native code could have called a guest vtable —
`SafeArrayDestroy` releasing FADF-flagged elements, or `IUnknown_Release` on a
raw guest pointer — is exactly what the classification and the fFeatures gate
refuse. The BYREF pass-through is proven safe by the native source: the entire
free block in `dlls/oleaut32/variant.c:637-661` sits under `if (!V_ISBYREF)`,
so `VT_BYREF|VT_UNKNOWN` never dereferences the referent (confirmed live:
`byref-disp hr=0 vt=0`, referent untouched).

## Findings, most severe first

None of these is a refcount-correctness defect. In severity order:

**D1 — combase.spec ordinal renumbering contradicts both the file's own
discipline and the report** (`dlls/combase/combase.spec:367-368, 374`; low).
`__wine_com_release_guest`/`__wine_com_addref_guest` were inserted after
`__wine_com_refuse`, and `__wine_guest_VariantClear` after
`GetHGlobalFromStream` — all *above* the "Appended at the END so no `@` export
above it is renumbered" comment. This shifts the auto-assigned ordinals of the
five `__wine_guest_*` exports by +2 and `__wine_com_slot_name` by +3. The
report claims the additions were "appended at the end … no renumbering risk",
which is not what happened. It is *benign today* — I verified every consumer
resolves these by name: ntdll (`dlls/ntdll/signal_ppc64.c:7677, 7715, 7763`,
all `RtlInitAnsiString` name lookups), GUEST-IMPL redirects (name lookup on the
native namesake, `tools/spec2thunk/spec2thunk:1149`), oleaut32/ole32 forwards
(by name), and combase pins no numeric ordinals so check-ordinal-imports' leg B
is unaffected. Concrete failure scenario: none in-tree; a stale out-of-tree
guest artifact that imported a combase `@` export by ordinal against the old
numbering would silently bind the wrong function. Worth a follow-up commit that
either moves the three lines to the true end or amends the block comment.

**D2 — `FADF_VARIANT` refusal binds on scalar-only VARIANT arrays**
(`dlls/combase/syscom.c` SAFEARRAY branch; low, functional). A
`VT_ARRAY|VT_VARIANT` SAFEARRAY whose elements are all scalars would be
perfectly safe to `SafeArrayDestroy` natively, but the gate cannot know that
without walking elements, so it refuses. SAFEARRAY-of-VARIANT is the most
common automation shape; if DOOM's measured runtime `VariantClear` hit carries
one, the title now gets `E_NOTIMPL` + leak instead of a clear. Safe-but-wrong
in the prompt's terms; only a refusal-log run under DOOM settles whether it
matters (the author's plan already names this as the v2/class-D recursion).

**D3 — misleading FIXME on dangling ex-proxies** (`dlls/combase/syscom.c`
VT_UNKNOWN refusal; low, diagnostics). Measured live: after a proxy is
destroyed, clearing another VARIANT still holding the stale pointer prints
"refuses a guest-implemented IUnknown … reverse proxy … ownership semantics
nobody has designed yet". The pointer is not guest-implemented — it is a dead
forward proxy, i.e. evidence of a guest over-release bug. Triage following the
message would investigate reverse-proxy design instead of the real bug. The
refusal itself is the correct fail-closed behaviour (E_NOTIMPL, untouched, no
crash — verified).

**D4 — refusal paths leave live resources behind E_NOTIMPL** (by design; info).
Callers that ignore `VariantClear`'s return (the norm) leak the punk/parray/
record on every refused row, and the `!syscom_ready()` path returns `E_FAIL`
without clearing even a scalar VARIANT (matching every sibling
`__wine_guest_*` wrapper's precedent). Memory-safe: the VARIANT stays valid,
which is *not* the uninitialised-out-pointer hazard the GUEST-REFUSE machinery
complained about, and native itself leaves the VARIANT untouched on
`DISP_E_BADVARTYPE`.

**D5 — probe gaps** (info). (a) No `VT_DISPATCH` leg — I covered it (below):
correct. (b) `variant_layout_probe.c` asserts `punkVal@8` etc. but not
`pRecInfo@16`, which the wrapper's `V_RECORDINFO` read depends on. (c) The
script asserts L7's hr/released but not its printed vt. (d) The positive run's
3-destroy-count cannot deterministically catch a hypothetical wrapper that both
proxy-releases AND host-releases (count would still be 3; the resulting
corruption is probabilistic) — the sabotage leg only proves the single-swap
bug is caught. None of these hides a present defect; they narrow future
regression coverage slightly.

**D6 — a permanent corruption knob in production code** (info).
`WINEEMUVARIANTUNSAFERELEASE=1` deliberately double-frees. Consistent with the
existing `WINEEMUNOCOMWRAP` precedent and off by default; a user setting it in
a real prefix gets exactly the corruption the gate demonstrates. The env read
is cached once per process (benign non-atomic `static int`).

**Theoretical, inherited, not new** — TOCTOU in
`winecom_release_guest_seen`: `proxy_from_pointer` confirms intern membership
under `wc_cs`, drops the lock, then `proxy_release` re-acquires and decrements.
If the caller does not actually own the reference it is dropping, a concurrent
last-release frees `p` in the window and the decrement is a UAF. This is
byte-identical to the preexisting `wc_forward_release` (`winecom.c:2056`) and
to the dispatch path, and under COM rules a Release without an owned reference
is UB anyway. Not fixable without a lookup-and-release-under-one-lock helper;
not worth it unless a real caller appears that can race.

## What was verified, and how

- **Interning/refcount discipline** (`libs/winecom/winecom.c`): one proxy per
  (host, iface), one host ref for the proxy's life, surplus host refs released
  on intern hits, un-intern + single host release at guest-zero. Locking:
  `proxy_addref`/`proxy_release`/`proxy_from_pointer` all take `wc_cs`
  themselves — the public helpers hold exactly the locking the private paths
  had; nothing relied on a caller-held lock.
- **`proxy_from_pointer` fail-closed**: vtable-block range check then
  intern-membership re-check under the lock, so stale/foreign/garbage pointers
  return NULL (verified live with a dangling ex-proxy). A wrong caller gets a
  0-return no-op, not intern-table corruption. Residual (shared with all of
  COM): a freed `com_proxy` heap block reallocated as another live proxy would
  alias — no worse than a native stale-Release.
- **`winecom_translate_in` TRUE-set is exactly {NULL, own forward proxy}**
  (`libs/winecom/reverse.c:496-546, 563`): with `iface == ~0u` the
  guest-implemented branch returns FALSE *before* minting any reverse proxy, so
  the wrapper's TRUE branch can never leak a reverse-proxy reference and the
  "silent no-op release" worry (translate TRUE but release finds no proxy) is
  impossible except for NULL, where it is correct.
- **`syscom_variant_type_ok`** vs `dlls/oleaut32/variant.c:523`
  `VARIANT_ValidateType`: same mask (`VT_EXTRA_TYPE` =
  VECTOR|ARRAY|BYREF|RESERVED), same base-type set, same `base<=VT_NULL` and
  `!=15` rules — no false negatives (a false negative here would be the
  native-Release-through-guest-vtable hazard), no false positives.
- **Native reference behaviour** read from `dlls/oleaut32/variant.c:627-666`:
  validate-first-untouched-on-failure, BYREF frees nothing, `VT_EMPTY` written
  on every SUCCEEDED path (including after a failed `SafeArrayDestroy`, which
  the wrapper reaches only for scalar arrays — matching).
- **Collateral**: `include/wine/winecom.h` gains two extern decls only; the
  generated `*_marshal.h` headers consume types/slot classes, not these; every
  module links its own `libwinecom.a` copy, no symbol clashes
  (repo-wide grep: only combase uses the new names); `wc_forward_release` and
  `reverse.c` untouched; `struct com_proxy` unchanged, nothing moved or grew.

## Experiments run (AC922, guest lane, FEX env per project requirements)

1. **Re-ran the gate under review** —
   `ppc64le/syscom/probes/check-variant-clear-smoke.sh`: PASS 23/23 both lanes,
   byte-identical apart from L7, mechanism count exactly 3. `--sabotage`: leg A
   dies at "stream A still alive" (classifier defeated by raw pointer); leg B
   destroy count 3→1 *and* the probe's own verdicts go red (FAIL 21/23) — both
   signals independently checked by the script. The author's reported outputs
   reproduce exactly.
2. **My own adversarial probe** (`/tmp/vc_adv.c` on the AC922, guest-only,
   built against the gate's import libs): same IStream in **two VARIANTs**
   (VT_UNKNOWN + **VT_DISPATCH**, the path the author never tested) with one
   explicit AddRef — first clear S_OK and object provably still alive (2/1
   arithmetic), second clear S_OK with exactly **one** `destroying proxy`
   trace in the whole run; **stale pointer after destruction** → E_NOTIMPL,
   VARIANT untouched, no crash; `VT_RESERVED|VT_UNKNOWN` junk →
   DISP_E_BADVARTYPE untouched with no release attempted;
   `VT_BYREF|VT_DISPATCH` → S_OK, referent never dereferenced. All correct.

## Not settled by reading, and the experiment that would settle it

- **Whether DOOM's runtime VariantClear payloads all land on served rows.** If
  it passes a guest-implemented IDispatch or a FADF_VARIANT array it now gets
  E_NOTIMPL + leak where it previously got a uniform refusal — behaviourally
  better, but not a clear. Settle: one native-lane DOOM run with FIXMEs on;
  the new refusals name themselves (modulo D3's misleading wording).
- **Concurrent clears of aliased VARIANTs under real contention.** The TOCTOU
  above is unreachable for a correct caller; a threaded probe hammering
  `winecom_release_guest_seen` against dispatcher Release on the same proxy
  would characterise the misuse behaviour, but it can only demonstrate UB that
  COM already declares UB. Low value.
- **The heap-reuse aliasing of a freed `com_proxy`** (stale guest pointer, new
  proxy at the same address): my stale-pointer leg exercised the freed-read
  path safely once, but proving the alias case needs allocation grooming.
  Same hazard class as any native stale-Release; not introduced by this series.
