# Mouse-wedge rescope: where it lives, what to do, what dies

2026-08-29. Re-scoping after the port-defect premise died (DOOM under emulated
Proton — stock Wine — shows the identical wedge on the same stack, and the
port's `dlls/winex11.drv/` is byte-identical to upstream wine-11.16).

## Verdict up front

**This lives in cosmic-comp's pointer-constraint lifecycle for Xwayland
surfaces, amplified by Xwayland's ancient lock-based warp emulation and made
silent and session-permanent by Wine's X11 clipping design. It is NOT the
port, and it is a KNOWN, ACKNOWLEDGED, ACTIVELY-WORKED upstream bug.**
Confidence: ~85% compositor⇄Xwayland (with cosmic-comp the most probable
proximate cause), ~10% a Wine-visible focus divergence (the instrumented run
will say), ~5% something else. Zero meaningful probability it is the ppc64le
port or Mesa.

**Recommended course:** run the already-instrumented one-session experiment to
classify the wedge, attach the result to the existing cosmic-comp issues, ship
the owner a one-line "revive" knob (mode toggle), park the stashed winex11 fix
on a branch (do not rebase it), and stop engineering on this. The fix is being
written by the people who own the failing layer.

---

## 1. Where does this actually live?

### The mechanism, end to end

Under rootless Xwayland there is no real pointer for X clients to own. Wine's
relative-motion path (`dlls/winex11.drv/mouse.c`) assumes X semantics:

1. Game calls `ClipCursor`/uses raw input → win32u `process_wine_clipcursor`
   → driver `X11DRV_ClipCursor` → `grab_clipping_window()`: `XGrabPointer` on
   an unmapped-then-mapped clip window, plus pointer warps to recenter.
2. Xwayland cannot warp or confine a pointer it does not own. It fakes it:
   the **pointer warp emulator** (`xwl_pointer_warp_emulator_*`, confirmed
   present in the installed `/usr/bin/Xwayland` 24.1.13 by `strings`) takes a
   `zwp_locked_pointer_v1` lock from the compositor when an X client warps
   with the cursor hidden and its surface holding pointer focus.
3. Only while that Wayland-side lock/constraint is **active** does the
   compositor deliver `zwp_relative_pointer_v1` deltas; Xwayland turns those
   into `XI_RawMotion` with **relative-mode** valuators on the master pointer.
4. Wine gates hard on that mode. `update_relative_valuators()` (mouse.c:229)
   accepts only valuators 0/1 with `mode == XIModeRelative`; otherwise
   "X/Y axis valuators not found, ignoring RawMotion events" and
   `map_raw_event_coords()` returns `{0,0}` for every frame. Separately, when
   `!thread_data->clipping_cursor` the same function deliberately discards
   relative values ("when not clipping cursor, we use MotionNotify").

So the whole chain hangs on one thing: **the compositor (re)activating the
pointer constraint on the game's Xwayland surface.** When it does not — after
an alt-tab, a popup, a fullscreen remap — the lock stays dormant, no relative
events flow, the master pointer's valuators rest in absolute mode (measured
here on 2026-08-22, recorded in the stash comment), and Wine ignores every
RawMotion frame. Buttons are ordinary core events and are unaffected. That is
precisely "buttons work, mouselook dead."

### Why it is permanent for the session

Wine makes the wedge sticky, twice over:

- `X11DRV_ClipCursor` (mouse.c:1425) **always returns TRUE**, so win32u's
  `process_wine_clipcursor` (input.c:2841) sets
  `thread_info->clipping_cursor = TRUE` regardless of whether anything was
  actually grabbed or whether the Wayland lock engaged. From then on,
  `clip_fullscreen_window()` (input.c:2776) refuses to retry:
  `if (!reset && clipping_cursor && thread_info->clipping_cursor) return FALSE;
  /* already clipping */` — even though it is re-invoked on window activation
  (input.c:2204) and on every mouse click (message.c:3926). The retries exist;
  the false success defeats them.
- Two silent early-outs inside `grab_clipping_window()` return success while
  doing nothing: the `is_current_process_focused()` branch (mouse.c:382) and,
  until today, a failed `XGrabPointer` — both now instrumented (commit
  `69a60f12ee3` in the tree).

Only a clip **reset** clears the latch, and display-mode changes generate one
(server posts `WM_WINE_CLIPCURSOR` reset; sysparams.c:4383 then posts
`SET_CURSOR_FSCLIP` to the foreground window) — which is exactly why "changing
screen modes sometimes revives it." The symptom set is fully explained.

### Blame allocation

- **cosmic-comp** — most probable proximate fault. Its own contributors
  acknowledged (2026-08-19, PR #2757 thread) a still-open pointer-constraint
  bug that "should only occur under Xwayland." A separate open PR (#2255)
  identifies a constraint-activation race that exactly explains
  works-at-launch-dies-after-focus-switch. The local patch attempt
  (`0001-pointer-constraints-keep-focus-and-apply-hint.patch`) guarded the
  periodic-refocus path and the deactivation hint but did not touch the
  activation race — consistent with it not helping.
- **Xwayland 24.1.13** — structural co-conspirator. Its warp emulator can go
  stale across map/unmap/restack without any motion event (xserver draft MR
  !532, open since **2020**), constraints get confused by multi-output
  changes (#1087, open), and it has no `wp_pointer_warp_v1` (the strings are
  absent from the binary; the protocol support MR !2031 is unmerged upstream).
- **Wine winex11** — not defective, but its design (unconditional success +
  the already-clipping latch + the strict relative-mode valuator gate) turns a
  transient compositor lapse into a silent, session-permanent wedge. Same
  code ships in Proton; that is why stock Proton fails identically.
- **The ppc64le port** — exonerated. Byte-identical driver, identical failure
  under stock Wine. Nothing here is arch-specific.
- **Mesa/GPU** — never implicated by any evidence; drop from consideration.

## 2. Known upstream bugs — yes, and this changes everything

Strong matches (from today's tracker sweep):

| Ref | Status | What it says |
|---|---|---|
| [cosmic-comp #1874](https://github.com/pop-os/cosmic-comp/issues/1874) | closed "fixed?", recurring through Jan 2026 | Fullscreen XWayland games: mouse capture breaks, triggered by alt-tab, "only surefire workaround is to fully restart cosmic-comp." Exact signature. |
| [cosmic-comp PR #2757](https://github.com/pop-os/cosmic-comp/pull/2757) | closed unmerged, Aug 2026; WIP branch continues | Protocol-level reproducer for the persisting constraint bug; contributor: "this is indeed a bug, but it should only occur under Xwayland" (2026-08-19). The canonical acknowledgement. |
| [cosmic-comp PR #2255](https://github.com/pop-os/cosmic-comp/pull/2255) | open since Apr 2026 | Constraint-activation race: constraints not eagerly activated on motion — explains die-after-refocus. |
| [xserver MR !532](https://gitlab.freedesktop.org/xorg/xserver/-/merge_requests/532) | draft, open since 2020 | Warp emulator survives surface leave via map/unmap/restack — stale emulator. |
| [xserver #1087](https://gitlab.freedesktop.org/xorg/xserver/-/issues/1087) | open | Xwayland constraints confused by output changes — the mode-change half of the signature. |
| [KDE bug 482632](https://bugs.kde.org/show_bug.cgi?id=482632) | fixed in Plasma 6.0.3 | Identical symptom on KWin, fixed compositor-side. Precedent: this class of bug is fixed in the compositor. |
| [Proton #7564](https://github.com/ValveSoftware/Proton/issues/7564) | open since 2024 | Catch-all "mouse stuck on Wayland" thread. |

On `wp_pointer_warp_v1`: **no released Xwayland supports it** (MR !2031 open
since June 2025; also !1839, allowing warps with a visible cursor, open since
Feb 2025). cosmic-comp DOES implement the protocol (merged PR #2432, Epoch
1.1.0, 2026-06-23; strings confirm it in the installed binary) — but today
that only benefits Wine's native winewayland driver, not the Xwayland path.
Arch POWER's repo tops out at `xorg-xwayland 24.1.13-1`, so there is nothing
to upgrade to even if it helped.

Implication: **the failure is already understood upstream and the fix vector
is cosmic-comp's constraint handling, actively in progress two weeks ago.**
Local Wine-side engineering would be re-deriving a known bug from the wrong
end.

## 3. Cheapest decisive experiment (one run, owner-executed)

The instrumentation is already in the tree (`69a60f12ee3`). One game session
classifies the wedge:

```sh
# after rebuilding winex11.drv from the instrumented tree
WINEDEBUG=warn+cursor,trace+cursor wine <game> 2> /tmp/cursor.log
# play until mouselook works, alt-tab a few times until it wedges, quit.
grep -E "not clipping|XGrabPointer|valuators not found|clipping to" /tmp/cursor.log | uniq -c
```

Read the tea leaves at wedge onset:

- **Path A — focus divergence:** repeated `not clipping to ...: this process
  does not have the X input focus` while the game is visibly focused.
  → cosmic's XWM focus handling diverges from Wine's view; a small,
  upstreamable Wine retry fix becomes defensible (see §4).
- **Path B — grab denial:** `XGrabPointer on the clip window failed`.
  → X-side grab conflict (unlikely; would implicate Xwayland grabs).
- **Path C — lock never engages (expected):** `clipping to ...` succeeds,
  no A/B lines, but `X/Y axis valuators not found, ignoring RawMotion events`
  (or raw frames simply stop / valuator mode stays absolute).
  → the Wayland-side constraint was never (re)activated. Wine literally
  cannot see or fix this; it is cosmic-comp/Xwayland, matching PR #2757.

The same `WINEDEBUG=warn+cursor` on the **Proton/DOOM** run (stock Wine — it
lacks the new TRACE but has the valuators WARN) cross-checks path C for free.

Corroborating experiments, in cost order, all owner-run (each opens windows):

1. **Nested weston A/B** — `/usr/bin/weston` is already installed. Run
   `weston` as a window under cosmic, `WAYLAND_DISPLAY=<nested socket>
   DISPLAY=<nested Xwayland>` and launch the same game inside; alt-tab within
   nested weston. If mouselook survives focus churn there and dies under
   cosmic, cosmic is indicted with no Wine changes at all.
2. **`weston-constraints`** (installed) — native pointer-constraints demo run
   directly under cosmic; exercises cosmic's constraint activation without
   Xwayland or Wine in the loop.
3. **`xtrace`** (installed) — interpose the game's X connection and watch
   whether `XI_RawMotion` events stop arriving at wedge time (distinguishes
   "Xwayland stopped sending" from "Wine stopped listening") — no rebuild
   needed if the instrumented run is somehow inconclusive.
4. **chrisglass/pointer-lock-test** — the reproducer from PR #2757, if we
   want to hand cosmic upstream a report in their own vocabulary.

## 4. What to do, ranked

1. **Classify, then report upstream, then track.** Run the §3 experiment
   once. Attach the classified log (plus machine details) to cosmic-comp
   #1874 / the #2757 successor work — an independent reproduction on a
   non-x86 machine with a clean log is genuinely useful to them. Then watch
   the cosmic-comp WIP branch; this machine already runs `cosmic-comp-git`
   (1.6.0.r4.g8304b18), so picking up the fix when it lands is a routine
   package rebuild. **This is the recommended path.** Cost: one evening.
2. **Ship the revive knob today (documented workaround).** The mode-change
   revival is mechanistic (clip reset clears win32u's already-clipping latch
   and re-arms the constraint). `cosmic-randr` is installed: a two-line
   script that toggles the output mode (or refresh rate) down and back up,
   bound to a key, turns "permanent for the session" into "press F9."
   Document alongside it: avoid alt-tab during capture; borderless-windowed
   reduces incidence; full cosmic-comp restart is the sledgehammer. For a
   machine whose job is *testing* games, this plus option 1 is honestly
   sufficient.
3. **Wine-side retry fix — only if the run shows Path A.** If focus
   divergence is the trigger, the honest fix is small and upstreamable:
   don't report unconditional success from `X11DRV_ClipCursor` /
   re-attempt the grab on `FocusIn` when a clip rect is pending (event.c
   already reapplies for the virtual-desktop and keyboard-grab cases; this
   would be a third). Do it as a WineHQ submission with the log attached —
   it would help every Proton-on-Wayland user — not as a local carry.
   If the run shows Path C, **do not write any Wine code**; there is nothing
   for Wine to react to.
4. **Another cosmic-comp local patch — not now.** One attempt already missed
   (it guarded deactivation/refocus, not activation), upstream is actively
   rewriting the same logic, and a second local guess would compete with
   their WIP branch. Only reconsider if upstream stalls for months AND the
   §3 run plus their reproducer pin the exact activation path.
5. **Waiting for `wp_pointer_warp_v1` — not a plan.** The Xwayland MR has
   been open since June 2025, Arch POWER has no newer Xwayland to move to,
   and the warp protocol alone does not restore relative-motion delivery —
   the constraint lifecycle is the broken part. Treat it as a nice future
   simplification, nothing more.
6. **Do nothing but document — the legitimate floor.** If even option 1
   feels like too much, write the symptom + revive knob into the project
   notes and move on. This has consumed five fix attempts on a bug that
   upstream owns and is fixing. Options 1+2 cost barely more than 6 and
   leave a breadcrumb trail, which is why they rank higher — but stopping
   here would not be wrong.

## 5. The stash (`stash@{0}` in the wine tree)

**Split it; retire the mouse hunks to a reference branch; do not rebase them
onto 11.16.** Reasons, in order of weight:

- **Premise gone.** It was written as a port fix; the port is exonerated.
- **Wrong layer.** It makes Wine difference absolute valuators so motion
  flows without the Wayland lock. But absolute values clamp at screen edges —
  differencing them yields the classic "look stops at the invisible wall"
  failure (the stash's own comment block half-acknowledges this). It masks
  the wedge rather than fixing it, upstream would reject it, and it would
  need Proton-side duplication to help Proton anyway.
- **Ground shifted under it.** Upstream `9306b8e8de4` rewrote
  `map_raw_event_coords` (fractional raw accumulation in
  `thread_data->raw_x/raw_y`); the stash's hunks are against the old body and
  will not apply. A rebase would be a rewrite of code we've just decided not
  to want.
- **BUT the stash is not only the mouse fix.** `git stash show --stat` says
  34 files: guestcrt, the vulkan-1 runtime proc-address thunks in
  `dlls/ntdll/signal_ppc64.c` (with a measured Quake II crash analysis in the
  comments), spec2thunk work, steamtool/appconfig, a pile of smoke-script
  updates. That work is live and must not be dropped with the stash.

Concrete disposal:

```sh
cd .../wine-upstream
git stash branch wip/2026-08-22-split stash@{0}   # materialize everything
# commit the non-mouse work properly on that branch,
git checkout -- dlls/winex11.drv/    # or commit the mouse hunks separately
#   as "RETIRED: absolute-valuator differencing (wrong layer; see rescope doc)"
```

Keep two things from it regardless: the **measurement** (valuators rest in
absolute mode on this stack — that datum confirms the lock-not-engaged
diagnosis) and the habit of commenting measured failures. The instrumentation
commit `69a60f12ee3` already on the branch is worth keeping independently —
naming those two silent early-outs is arguably upstreamable on its own.

---

## Appendix: facts established today

- Installed: Xwayland 24.1.13 (`-rootless`, `-enable-ei-portal`), cosmic-comp
  1.6.0.r4.g8304b18 (git, self-built pkg), Arch POWER rolling, repo has no
  newer xorg-xwayland.
- `strings /usr/bin/Xwayland`: has `xwl_pointer_warp_emulator_create`,
  `zwp_pointer_constraints_v1`, `zwp_relative_pointer_v1`; **no**
  `wp_pointer_warp`.
- `strings cosmic-comp`: **has** `wp_pointer_warp_v1`, plus constraints,
  relative-pointer, locked/confined pointer.
- Wine tree at `wine-11.16-227-g69a60f12ee3`; winex11 clean vs upstream apart
  from the new instrumentation commit.
- Prior cosmic patch attempt:
  `~/Development/power9_development/cosmic/cosmic-comp-git/0001-pointer-constraints-keep-focus-and-apply-hint.patch`
  (deactivation-hint + periodic-refocus guard; did not fix).
- Useful tools already on the box: `weston` + demo clients incl.
  `weston-constraints`, `xtrace`, `cosmic-randr`. No `xinput`/`evtest`/`xev`.
