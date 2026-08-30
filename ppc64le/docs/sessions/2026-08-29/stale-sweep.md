# Stale-premise sweep — wine-ppc64le fork

Tree: `/home/jbettcher/Development/power9_development/powerpc64le-ports/hangover-ppc64le/wine-upstream`
(sshfs mount, read live; branch `wine-ppc64le`, HEAD `3214a170a7e`).
Swept 2026-08-29, ~20:30–21:45 MDT. Machine confirmed via `ssh` `lscpu`/`lspci`:
**POWER9, 176 threads (2 sockets × 88, NUMA nodes 0/8), AMD Navi 31 (RX 7900 XTX,
PCI 1002:744c)**. This matters directly — see hardware-mismatch findings below.

Live state note: at sweep time the tree had **uncommitted work in progress**
(`git status`): modified `dlls/guestcrt/guestcrt.def`, `guestcrt.guestpe`,
`dlls/winex11.drv/mouse.c`; new `dlls/guestcrt/exceptobj.c`. This uncommitted
file is itself mid-flight evidence of exactly the failure mode this sweep was
asked to find — see Finding A-1.

---

## A — Actively misleading today (would cause a wrong turn)

### A-1. The C++ exception-object trio: EXCLUDE lines being overtaken *as this sweep ran*

**Claim** (committed 2026-08-29 15:02:56, commit `914202a224e`, identical in all
five files):
- `dlls/vcruntime140/vcruntime140.thunks:169-171`
- `dlls/ucrtbase/ucrtbase.thunks:368-370`
- `dlls/msvcrt/msvcrt.thunks:397-399`
- `dlls/msvcr100/msvcr100.thunks:100-102`
- `dlls/msvcr120/msvcr120.thunks:67-69`

```
EXCLUDE __CxxRegisterExceptionObject
EXCLUDE __CxxUnregisterExceptionObject
EXCLUDE __DestructExceptionObject
```

justified in each file's comment: `__DestructExceptionObject` calls the thrown
object's own destructor, a **guest function pointer**, so a native trap would
mean "native ppc64 code indirect-calling an x86-64 address" — a silent wrong
answer — so EXCLUDE (a named, diagnosable sentinel) is "the correct refusal."

**Evidence it is already going stale — mid-flight, uncommitted, right now:**
`dlls/guestcrt/exceptobj.c` (new file, `stat` mtime 2026-08-29 21:13:29, **not
committed**, `git status` shows it untracked) implements exactly these three
names — plus `__current_exception` and `__processing_throw` — as real x86-64
guest code, and `dlls/guestcrt/guestcrt.def` has already been edited (also
21:13, uncommitted) to export all five from `guestcrt.dll`. The file's own
banner states why: "**MEASURED 2026-08-29, Cyberpunk 2077 (Steam) run 20:14.**
With `_CxxThrowException` served as real guest code..., the throw now reaches
a real `__CxxFrameHandler4` — and dies calling
`vcruntime140.__CxxRegisterExceptionObject`, sentinel `0xDEAD0005`." I.e. the
EXCLUDE-as-final-answer premise was falsified by an actual game run **65
minutes before I read the file**.

**Delta:** commit-to-supersession is ~6 hours same day (15:02 → 21:13); the
triggering measurement (20:14) landed 68 minutes after the EXCLUDE commit's
"day is done" framing, and the fix was still in progress, uncommitted, when
this sweep read it.

**Cost of believing the committed `.thunks` text alone right now:** high. A
second engineer picking up the tree from git HEAD (not looking at the dirty
working copy) would read five independent, consistent, well-argued comments
all concluding "this must stay a named refusal" and could easily re-litigate
or re-derive the exact same guest-code fix that already exists uncommitted on
disk, or — worse — not realize a `git status`-only check misses it entirely.
**Action implied, not just documentation:** whoever finishes this in-flight
change needs to update all five `.thunks` files' EXCLUDE lines to FORWARD (to
`guestcrt`), not just add the new source file.

### A-2. `ucrtbase.thunks:372` — a live trap about to become a silent-wrong-answer, not just a stale comment

**Claim:** `__processing_throw 0x00000E01 0 int msvcrt.h:161` — a **native
trap**, justified as safe because it "hands back a POINTER into native
thread-local data... so the guest FH3/FH4 personality... needs no host->guest
call to do so."

**Evidence it is now wrong, in the author's own words:** `exceptobj.c`'s banner
(same uncommitted file as A-1) states the general rule this trap now violates:
"serving the three Cxx* names as guest code while leaving
`__current_exception` and `__processing_throw` as native trap stubs would
split one logical structure across two machines: the guest functions would
write the guest copy, and the native stubs would hand FH4 a pointer into the
NATIVE msvcrt's thread data... FH4 would then read an exception record that
does not describe the exception in flight." `guestcrt.def`'s diff already
exports `__processing_throw` from guestcrt — but `ucrtbase.thunks:372` has
**not** been updated to FORWARD it there.

**Cost:** this is the sharper of the two — a native trap that used to be
correct is being turned into a **silent wrong answer** (not even a diagnosable
refusal) by a sibling change that hasn't reached this line yet. Anyone testing
the in-flight fix without touching this line gets corrupted/mismatched
exception state that "appears only during unwind, as corruption, with no name
attached" (exceptobj.c's own words) — exactly the failure class this whole
port's discipline exists to avoid.

### A-3. `dlls/msvcrt/msvcrt.h:149-154` — flagged stale by the very engineer writing the fix, not yet corrected in place

**Claim:** comment on `_CreateFrameInfo`/`_FindAndUnlinkFrame` says both "only
touch the native frame_info list rooted in `thread_data_t.frame_info_head`,"
so a ucrtbase trap thunk is correct for them.

**Evidence:** `exceptobj.c`'s banner, uncommitted, addresses this exact
comment by name: *"dlls/msvcrt/msvcrt.h:149-154 justifies ucrtbase trap
thunks for the first two on the grounds that they 'only touch the native
frame_info list'... That reasoning is correct for a NATIVE caller and **stale
for a guest one**: with this file in place the guest list is the one below."*

**Delta:** same-day, hours. **Cost:** low today (the author already caught it
and left a breadcrumb), but `msvcrt.h` itself still reads unconditionally
correct to anyone who doesn't also open `exceptobj.c` — this is exactly
incident #5 from the brief, mid-repair rather than post-mortem.

### A-4. `dlls/vcruntime140_1/vcruntime140_1.thunks` — a same-day theoretical claim contradicted by tonight's own measurement

**Claim** (committed today, "CORRECTION, 2026-08-29, same day, adversarial
review", lines 43-80): "every Proton prefix gets exactly the same named
refusal [FH4 unresolved] a non-Proton prefix does, from exactly this module,
**until Session B lands real guest FH4 code**."

**Evidence:** `exceptobj.c`'s banner (measured 20:14, i.e. *after* this
correction was written) reports the opposite in practice: "the throw now
reaches a real `__CxxFrameHandler4`... The prefix's staged
`vcruntime140_1.dll` **is** a WINE BUILTIN... and winedump says it exports
exactly three names: `__CxxFrameHandler4`, `__NLG_Dispatch2`,
`__NLG_Return2`... the catch side already exists as guest code the guest CPU
runs." FH4 is not a hole in practice tonight — it ran, and only its five
support imports were missing.

**Nuance, checked independently (do not over-claim):** `dlls/guestcrt/`
genuinely has no `handler4.c`/`except_x86_64.c` — the hand-ported FH4
personality "Session B" as originally scoped is **not built** by this repo.
What changed is empirical, not architectural: Proton's own staged
`vcruntime140_1.dll` already **is** real FH4 machine code, so "Session B" for
these two blocked titles may already be moot — a narrower situation than
`vcruntime140_1.thunks`'s literal words ("every Proton prefix gets... the
named refusal... until Session B") describe. **Cost:** someone reading only
this file tonight would conclude no title can ever reach a live FH4 without
first hand-porting Wine's `handler4.c` — which is not what the last hour's own
test just showed. This is worth the fix-authors reconciling explicitly in the
next commit; flagging here so the correction doesn't get re-written a third
time from the same stale premise.

---

## A-5. `ppc64le/steamtool/appconfig/1091500.env:32-41` — an ACTIVE setting justified by a 10-day-dead blocker, plus the wrong GPU

**Claim** (dated 2026-08-19, unchanged since; last file touch was 2026-08-26
23:34, commit `e0e3e9c69ad`, which edited a *different* section of the same
file):

> `nodxr`: RADV on NAVI21 exposes ray tracing, so the game sees a real DXR
> tier and builds its RT pipelines — through `ID3D12Device5::CreateStateObject`,
> whose `D3D12_STATE_OBJECT_DESC` hides interface pointers behind `const
> void*` and is a named refusal in the marshal table (**no hand-written walker
> yet**). [MEASURED 2026-08-19, run 35]...
> `VKD3D_CONFIG=nodxr`   ← currently active, uncommented

**Evidence of staleness:** `ppc64le/NEXT.md:209-219` (same repo): *"DXR walker
and PipelineLibrary loads — **DONE** (2026-08-26, `477b103fb76`)... `nodxr`
CAN come out of the appconfig now, but whether the V620 should be offered RT
is a performance decision, not a marshalling one — unmeasured, and the flag
stays until it is."* `git show -s 477b103fb76` confirms the walker landed
2026-08-26 22:55:22 -0700. The appconfig file was edited again **40 minutes
after** that commit (23:34:55, adding an unrelated spin-collapse setting) and
still did not touch the `nodxr` paragraph.

**Delta:** 10 days (blocker fixed 08-26, still cited today 08-29) — and the
file was demonstrably open and being edited only 40 minutes after the fix
landed, in the same repo, by presumably the same person, and the stale
paragraph survived that edit untouched.

**Compounding hardware-mismatch** (label: co-developers' hardware, not this
machine): the comment's performance question is "whether **the V620** should
be offered RT," and `NEXT.md:218` repeats the identical framing. The actual
build/test machine (this AC922, confirmed via `lspci`) has an **AMD Navi 31
(RX 7900 XTX)**, not a V620/Navi21 — a full RT-hardware generation apart in
both raw RT throughput and driver maturity. Anyone using this file to decide
whether to lift `nodxr` on the AC922 would be reasoning about the wrong GPU's
performance envelope entirely, on top of citing a blocker that no longer
exists.

**Cost:** high. This is a currently-*active* setting (not dead code, not just
a comment) suppressing a real, already-implemented feature (ray tracing) in
the flagship title on every launch, and both of its stated justifications —
"no walker" and "the V620's RT performance" — are wrong for *right now, on
this machine*.

## A-6. `ppc64le/games/STATUS.md:53` (Cyberpunk row) — repeats the same dead premise NEXT.md already retired

**Claim:** "...and with RADV exposing real ray tracing the game built RT
pipelines through the DXR state-object refusal and force-closed blaming GPU
drivers — `VKD3D_CONFIG=nodxr` in the appconfig hides the tier **until the
`D3D12_STATE_OBJECT_DESC` walker exists.**"

**Evidence:** same as A-5 — walker landed 08-26 (`477b103fb76`). STATUS.md's
own git history shows its last content commit predates 08-26 (most recent
touch, `50c4e37669d` on 08-28, only edited the unrelated Dex/32-bit-lane row).
**Delta:** 10 days. **Cost:** `NEXT.md` explicitly calls this file "the
per-title board" — i.e. the canonical status source — and it now directly
contradicts `NEXT.md`'s own "DONE" entry on the same fact. Someone trusting
the board over the work-list gets the stale answer.

## A-7. `ppc64le/games/STATUS.md:839` — "Nothing is committed" is flatly false today

**Claim:** *"Files this pass touched ## Nothing is committed; everything below
is uncommitted working state."* — followed by a list of 15 `.thunks` files
(`psapi`, `msvcr100`, `d3dx11_43`, `rpcrt4`, `mscoree`, etc.) and 15
`Makefile.in` edits.

**Evidence:** checked three at random — all tracked and clean:
```
$ git ls-files dlls/psapi/psapi.thunks dlls/msvcr100/msvcr100.thunks dlls/d3dx11_43/d3dx11_43.thunks
dlls/d3dx11_43/d3dx11_43.thunks
dlls/msvcr100/msvcr100.thunks
dlls/psapi/psapi.thunks
$ git status --short dlls/psapi dlls/msvcr100 dlls/d3dx11_43   → (clean)
$ git log -1 --format="%ad %s" -- dlls/psapi/psapi.thunks
2026-08-29 10:34:37 -0600  spec2thunk,psapi: PROBE-DEFINE asks psapi.h...
```
`msvcr100.thunks` traces to the 2026-08-17 corpus pass, committed long ago.

**Delta:** ≥ 7-12 days. **Cost:** moderate — this is a leftover "audit pass"
note from a single work session, left in the project's canonical per-title
status board. Anyone reading it would believe there's a large pile of
uncommitted work to stage/review; `git status` on the actual repo shows
nothing of the sort for these files. Low technical risk, real trust/orientation
cost in a document whose entire job is being trusted at a glance.

## A-8 (medium). `dlls/dinput8/dinput8.thunks:146-149` — cross-reference to two "dead" paths that are both alive

**Claim:** "...comfortably inside the FOUR the trampoline carries — which is
the ceiling that **stops** `dlls/comctl32`'s `SetWindowSubclass` and
`dlls/wininet`'s `InternetSetStatusCallback` **dead**; see those files."

**Evidence:** both are served today via 5-argument trampolines
(`call_guest_function_args5`/`__wine_guest_wrap_callback5`), added in commit
`4d7f5a4e74a` "ntdll,comctl32,wininet: callbacks with five and six arguments
cross the boundary" (2026-08-18 02:38:54). Confirmed independently:
`dlls/wininet/wininet.thunks:77-78` → `GUEST-IMPL InternetSetStatusCallbackA/W
__wine_guest_InternetSetStatusCallbackA/W`; `dlls/comctl32/comctl32.thunks:81`
→ `GUEST-IMPL SetWindowSubclass __wine_guest_SetWindowSubclass`. `dinput8.thunks`
was itself edited again on 2026-08-27 (`ffd1e5c6085`, an unrelated
crossing-count change) without correcting this line.

**Delta:** 11 days (fix 08-18 → still cited today), with an intervening
untouched edit 2 days before this sweep. **Cost:** low-medium — this is a
side reference, not the primary spec for either symbol (both target files
independently and correctly document their own served status), but it is a
live, checkable falsehood in a comment two *other* files point back to as
"the full version of the trampoline note" (`comdlg32.thunks:62-66`).

(Credit: found by a parallel sweep worker; independently re-verified above.)

---

## B — Stale but harmless (or half-stale; low/no action cost)

**B-1. `guest-cxx-eh-plan.md` does not exist anywhere in the tree or git history**, yet is cited by name and section (Session A/B/C) as an authoritative
plan in 9 places across 6 files: `dlls/ucrtbase/thunkcxx.h:31,41,48`,
`dlls/msvcrt/msvcrt.h:150`, `dlls/msvcr120/msvcr120.thunks:38`,
`dlls/msvcrt/msvcrt.thunks:370`, `dlls/msvcr100/msvcr100.thunks:73`,
`dlls/ucrtbase/ucrtbase.thunks:274,331`, `dlls/vcruntime140/vcruntime140.thunks:106,141`,
`dlls/vcruntime140_1/vcruntime140_1.thunks:34`. `git log --all --diff-filter=A
--name-only` and `find . -iname '*guest-cxx-eh-plan*'` both come up empty. Not
a "blocker no longer exists" case — the opposite problem, a citation to a
document that never existed in the repo (it appears to have been produced as
planning material outside the tree). Cost: near-zero technical risk (the
`.thunks` comments already inline the reasoning inline), but 9 dead-end
pointers waste a few minutes each for the next reader who goes looking for
"Session B's full plan."

**B-2. `ppc64le/steamtool/appconfig/379720.env:1-4`** (DOOM): "Remove this
line once the opengl32 guest thunk lands and the GL build works too." Half the
AND-condition is long satisfied: `dlls/opengl32/opengl32.thunks` (a real guest
export surface, `FROM-SPEC opengl32-guest.spec`, only one benign `EXCLUDE
__wine_gl_entry_point`) plus a full byte-identical runtime gate
(`ppc64le/opengl/check-gl-smoke.sh`, 8 legs) have existed since **2026-08-17**
— the same day this appconfig file was created. The file was touched again
today (`93343e60ee9`) without adjusting the comment. Not calling this a hard
staleness bug because the second clause ("the GL build works too") is
unverified without launching the game (out of scope) — but the comment as
written implies both conditions are still open, when one demonstrably is not.

---

## C — Checked and genuinely still true (do not re-check next week)

- **Session B of the C++ EH plan (real hand-ported `__CxxFrameHandler4`/FH3
  personality, e.g. `handler4.c`/`except_x86_64.c`) is genuinely NOT
  implemented.** `ls dlls/guestcrt/` = `cxxthrow.c`, `exceptobj.c` (new,
  uncommitted), `guestcrt.def`, `guestcrt.guestpe`, `Makefile.in`,
  `setjmp.c` — no FH3/FH4 source. `vcruntime140_1.thunks`'s core claim that
  the module's own thunk table exports nothing is still accurate (see A-4 for
  the nuance about the *staged Proton binary* being separately real).
- **`ppc64le/vkd3d/docs/fence-callback.md`'s "on the AC922 today, `/dev/ntsync`
  does not exist" still holds** — verified live: `ssh ... ls /dev/ntsync` →
  "No such file or directory."
- **`ppc64le/dxvk/docs/i386-lane-design.md`** — exemplary supersession
  pattern: the 2026-08-19 "NOT DONE" section is still physically present but
  a 2026-08-28 section (`50c4e37669d`) was inserted above it, explicitly
  labeled "BUILT (2026-08-28) — the crux resolved," with the old section
  marked "kept for the record." Nothing to fix; this is the model other docs
  in this sweep should follow.
- **`ppc64le/mf/README.md`** already self-corrects in place: text explicitly
  says mediaengine/wmvcore/evr "were listed here as not done" and are now
  served.
- **`ppc64le/vkd3d/docs/feasibility.md`**'s DXR/mesh-shader/VRS "stub" table
  describes **upstream Wine's own vkd3d** at the 2026-08-14 decision point
  (rejected in favor of vkd3d-proton) — not this fork's own d3d12 marshal
  surface. Still accurate; not the same subsystem as the `nodxr` finding
  above.
- **`ppc64le/cpu/TOPOLOGY.md`**'s "not done" list (CPU hotplug,
  `CPU_SETSIZE≥1024`, group-0-only idle-cycle time, hwloc/FreeBSD path) — no
  contradicting commit found.
- **`ppc64le/streamline/README.md`**: "not yet a proton-tool staging rule;
  promote it when a second title needs it" — Cyberpunk is still the only
  Streamline-importing title in STATUS.md; condition for promotion hasn't
  fired.
- **`WORKING-ON-THIS.md`'s own in-place correction** ("The old '~2.5-core
  ceiling' note in NEXT.md item 6 was measured on a much older tree and does
  not reproduce") is itself accurate and already self-aware — nothing to flag.
- **`dlls/combase/combase.thunks:29-67`** GUEST-REFUSE block (COM marshaling)
  — no specific landed mechanism is named as the blocker (it's a generic
  "until a wrapper/walker lands"), so nothing here is falsifiable against a
  found commit. Adjacent, not stale: `PropVariantClear`/`PropVariantCopy`/
  `FreePropVariantArray` sit right next to today's `VariantClear` proxy-safety
  fix (`5e150fa1fb6`) but that commit does not claim PROPVARIANT is covered
  (strict superset of variant types) — worth the next engineer reading that
  commit first, not a sweep finding.
- **`dlls/comctl32/comctl32.thunks:136-168`** "five real refusals" — consistent
  with the current lack of a reverse-COM-proxy lane in `libs/winecom`.
- **`dlls/d3d11/d3d11.thunks`** `EXCLUDE __wine_dxvk_*`/`__wine_guest_D3D11Create*`
  etc. — by-design routing through the DXVK unixlib, not a blocked capability.
  Correctly documented as such.
- **`comdlg32.thunks`, `dinput.thunks` (not `dinput8`), `wininet.thunks`,
  `xaudio2_8.thunks`** — all four explicitly narrate their own history ("THIS
  FILE USED TO SAY...", "THE HANDOFF this section used to describe is done")
  and are accurate as of the mechanisms they cite. Good hygiene.

---

## Hardware-mismatch findings (labeled separately per the brief — not "blocker fixed," but "wrong machine")

1. **`ppc64le/WORKING-ON-THIS.md:50-57`** — the whole lever table ("governor
   +3%", "NUMA unbound vs bound +4%", "SMT4→SMT2 +9%", "node-0 bind at SMT2
   halves framerate") is prefaced "Measured on this box (**POWER8, V620**)."
   The AC922 actually used for this sweep is **POWER9, 176 threads (2×88),
   RX 7900 XTX (Navi 31)** — a different CPU generation, thread count, NUMA
   shape, and GPU generation. The measurements aren't wrong for whatever box
   they came from, but nothing here says they transfer to this one, and nodes
   are numbered 0/8 here, not a simple 0/1 — a governor/NUMA/SMT tuning
   decision made by reading this table on the AC922 would be extrapolating
   across all three axes at once.
2. **Internal inconsistency worth flagging to the maintainers, not resolved
   by this sweep:** `ppc64le/dxvk/README.md:103` separately says `[MEASURED]
   2026-08-17, the test machine (POWER9, V620/RADV)` — i.e. a *different* doc
   claims the reference machine is POWER9 (matching the AC922's CPU
   generation) but still V620 (not matching its GPU). Whether there are two
   distinct reference boxes or one doc has the CPU generation wrong, the tree
   does not currently say, and neither claim describes the AC922 exactly.
3. **`ppc64le/steamtool/appconfig/1091500.env:43-48`** — `WINE_PPC64LE_CPU_LIMIT=64`
   justified as reproducing "the 2×40 view of this POWER8." `ppc64le/cpu/TOPOLOGY.md:102-105`
   maps "2×40" specifically to "POWER8 2×10 SMT4" (80 threads) — a machine
   distinct from the AC922 (2×88 = 176 threads, POWER9). The **workaround**
   is plausibly still needed regardless of the exact number (REDengine
   refuses *any* multi-processor-group view, not specifically a 2×40 one),
   so this is not claimed as an active bug — but the topology numbers in the
   comment do not describe the machine the workaround is now deployed on.

---

## Summary for the next person

Highest-value takeaway: **do not read `dlls/{vcruntime140,vcruntime140_1,ucrtbase,msvcrt,msvcr100,msvcr120}/*.thunks` in isolation from `dlls/guestcrt/exceptobj.c` right now** — the committed `.thunks` files and the uncommitted `guestcrt` change are mid-handoff, and three separate claims in the committed files (the EXCLUDE triad's finality, `__processing_throw`'s trap-is-safe reasoning, and FH4 "always refused until Session B") are all in the process of going stale within the same evening. Finish that handoff (FORWARD the five thunks lines, update the five comment blocks, reconcile `vcruntime140_1.thunks`'s framing against tonight's measurement) before anyone else reads those files as settled.

Second highest: **`VKD3D_CONFIG=nodxr` in `ppc64le/steamtool/appconfig/1091500.env` is an active setting citing a 10-day-dead blocker and the wrong GPU** — cheapest fix in this whole sweep (delete/rewrite one paragraph, decide whether to re-measure RT on the RX 7900 XTX), highest ratio of impact to effort.
