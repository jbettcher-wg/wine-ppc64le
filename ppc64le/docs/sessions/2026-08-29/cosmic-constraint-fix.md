# cosmic-comp Xwayland pointer-constraint reactivation fix

## Status: built, NOT installed. Package is ready for the owner to install by hand.

## What was already there (previous local attempt)

`~/Development/cosmic/cosmic-comp-git/0001-pointer-constraints-keep-focus-and-apply-hint.patch`,
applied via the PKGBUILD's `prepare()`, was already checked into the working
tree at `src/cosmic-comp/src/shell/focus/mod.rs`. It does two things:

1. In `update_focus_state()`'s deactivation-on-focus-loss block: apply any
   pending cursor-position hint *before* calling `constraint.deactivate()`
   (a backport of smithay `a12d486ba561`'s semantics).
2. In `update_pointer_focus()` (the *periodic* pointer-focus refresh that
   fires whenever the surface stack under the pointer changes, e.g. a
   notification or popup): if the currently-focused surface has an *active*
   constraint, return early instead of letting the refresh re-focus the
   pointer and silently drop it.

This fixes a different, real bug (cursor trapped at 0,0) but it is exactly
the flaw the task described: it only *guards* the periodic path against
stealing focus from an already-active constraint. It contains no code that
*reactivates* a constraint that has already gone inactive — which is what
alt-tab does. **I kept it** (both hunks are still correct and needed) and
added the missing reactivation path on top of it.

## Upstream research

- **Issue #1874** ("Fullscreen XWayland apps mouse capture issue") — closed,
  80 comments, recurring. Confirms the general shape of the bug.
- **PR #2255** (`RALaBarge`, open, unmerged) — turned out to be **stale**.
  Its `input/mod.rs` "eagerly activate on motion" mechanism and its
  `new_constraint()` "check physical position as a fallback" mechanism are
  **both already present in upstream master** via earlier commits
  (`9702aae5`, then rewritten more robustly in `208c2128`) that predate the
  PR. Maintainer `Drakulix` pushed back on the PR for exactly this reason
  ("make sure current_focus is updating correctly instead of implementing a
  secondary focus check"). **Not used** — applying its diff would have been
  redundant with code already in our tree.
  - RALaBarge's own follow-up *comment* on that PR (2026-04-05, never
    committed) independently diagnosed the exact alt-tab scenario this task
    describes: "the player alt-tabs away and back... if the pointer was
    already over the game window... `pointer.motion()` is skipped, and
    neither of our previous fix points is reached." No code was ever pushed
    for it, but the diagnosis is correct and informed the design below.
- **PR #2757** (`chrisglass`, closed, "Fix pointer constraints in all (?)
  cases") — this is the one that mattered. Despite being closed (the author
  self-flagged it as unpolished/AI-assisted and it needs a re-review), its
  `src/wayland/handlers/pointer_constraints.rs`, `src/shell/focus/mod.rs`,
  and `src/xwayland.rs` hunks are a real, targeted mechanism:
  - A new `State::maybe_activate_pointer_constraint()` helper that checks
    keyboard focus (including an Xwayland override, see below) and the
    constraint's region, then activates it if eligible.
  - `Common::xwayland_constraint_focus_override()`: because Xwayland routes
    keyboard input to whichever X11 window has *X* input focus, independent
    of which of that client's surfaces holds *our* Wayland keyboard focus, a
    game's pointer-locked surface (often an override-redirect fullscreen
    window) can legitimately never hold Wayland keyboard focus itself. The
    plain `has_surface()` check used everywhere else then always says "not
    focused" for that surface, so its constraint gets deactivated on any
    focus churn and is never eligible to reactivate. This override says: if
    Xwayland's virtual keyboard target (`is_xwm()`) is focused and the
    surface belongs to that same X11 client, treat it as focused too.
  - A `still_eligible` check on the deactivation path in
    `update_focus_state()`: don't tear down a constraint just because
    `target != old_target`, if the new target is still the same
    (Xwayland-aware) focus.
  - I discarded two things from that PR that did not apply here: an
    unused/stray `use ron::de::Position;` import (dead code, unrelated to
    the fix), and its `RelativePointerManagerState::new()` additions to the
    `winit`/`x11` nested backends — irrelevant, since the real DRM/KMS
    backend this machine actually uses already registers that global
    (`src/backend/kms/mod.rs`).
  - This PR's thread also contains the 2026-08-19 exchange the task
    mentioned: contributor `skygrango` says "this is indeed a bug, but it
    should only occur under Xwayland," matching `xwayland_constraint_focus_override`'s
    scope exactly. `chrisglass` mentions moving to "a WIP branch, with
    better comments" but no such branch has been pushed publicly yet (only
    `fix-pointer-constraints` and an unrelated `global-pointer-protocol-state`
    exist on their fork) — nothing further to pull from there.
- **KWin** (prior art, web search only, no direct diff pulled): KWin's fix
  for the equivalent bug unconditionally re-checks/unconstrains on
  keyboard-focus-change signals rather than relying on the periodic pointer
  refresh — i.e. it treats the *focus-change event itself* as the place to
  reconcile constraint state, not a secondary poll. This matches the
  direction taken below (see the third change).

## What I actually changed

Adapted from PR #2757 (attributed above), plus one piece of original glue
code that PR #2757 itself did not add (see item 3 — nothing upstream
contained this exact hook, so I wrote it, reasoning from RALaBarge's comment
and the KWin precedent that the fix has to live on the focus-*gained* event,
not only on a pointer-motion event).

1. **`src/wayland/handlers/pointer_constraints.rs`** (from PR #2757): added
   `State::maybe_activate_pointer_constraint(seat, surface, surface_location)`,
   and refactored `new_constraint()` to call it instead of duplicating the
   focus/region logic inline.
2. **`src/input/mod.rs`** (from PR #2757): replaced the existing inline
   "eagerly activate on motion" block in the `PointerMotion` handler with a
   call to the same new helper — same behavior, de-duplicated.
3. **`src/shell/focus/mod.rs`** — three edits, merged with the existing
   local patch:
   - Added the `still_eligible` / `xwayland_constraint_focus_override` guard
     to the existing deactivation block (from PR #2757), so a constraint
     isn't torn down by an Xwayland-internal focus wobble that isn't a real
     focus change for that X11 client.
   - **New (not from any upstream diff — this is the piece nothing upstream
     had committed):** right after `keyboard.set_focus(...)` in
     `update_focus_state()` — i.e. exactly at the point a surface *regains*
     keyboard focus (alt-tab back) — look up whatever surface is currently
     under the pointer and call `maybe_activate_pointer_constraint()` on it.
     This is the actual "constraint reactivation on refocus" path the task
     asked for: it fires synchronously with the focus-change event itself,
     independent of whether a pointer-motion event happens to follow (which
     it may not, if the cursor never physically left the fullscreen game
     surface during the alt-tab).
   - Added the same `maybe_activate_pointer_constraint()` call to
     `update_pointer_focus()`'s existing "pointer entered a new surface"
     branch (from PR #2757), as a second opportunity when pointer focus (as
     opposed to keyboard focus) changes.
4. **`src/xwayland.rs`** (from PR #2757): added
   `Common::xwayland_constraint_focus_override()` and the two extra imports
   (`Resource`, `WlSurface`) it needs.

Net diff: `+206 / -76` across those four files (`git diff --stat` inside
`src/cosmic-comp`).

## Why this is the right target for the observed bug

The task's diagnosis is that reactivation fails specifically on the
alt-tab-*back* path. Tracing the actual code confirmed there is genuinely no
prior reactivation hook at all on that path: `new_constraint()` only fires
once, when the client first creates the lock object — Wine does not destroy
and recreate it across an alt-tab, it just observes `deactivate()` happen
once and never gets told to look again. The only other reactivation
opportunity that existed was the `PointerMotion`-triggered eager-activate
(already upstream), which depends on an actual mouse-motion event arriving
after refocus — and does nothing if the cursor never physically left the
game's (often fullscreen) surface during the switch, which is the common
case. That gap is exactly what change 3's new hook closes.

## Built package

```
/home/jbettcher/Development/cosmic/cosmic-comp-git/cosmic-comp-git-1.6.0.r4.g8304b18-1-powerpc64le.pkg.tar.zst
/home/jbettcher/Development/cosmic/cosmic-comp-git/cosmic-comp-git-debug-1.6.0.r4.g8304b18-1-powerpc64le.pkg.tar.zst
```

(built 2026-08-29 21:58 on the AC922; same pkgver as the currently-installed
package since the pinned upstream commit, `8304b18`, didn't change — only
the local source did.)

Built via `makepkg -R` from `~/Development/cosmic/cosmic-comp-git/`, which
repackages from the already-compiled `src/cosmic-comp/target/release/`
without re-extracting sources or rerunning `prepare()`/`build()` — this was
deliberate, to avoid `makepkg`'s default source re-extraction discarding the
hand-applied edits sitting in the working tree. The actual compile step was
`nice make ARGS+=" --frozen --release -j 96"` run inside `src/cosmic-comp`
with `ulimit -n 65536` (the default 1024 fd limit was insufficient for a
176-thread parallel Rust build and caused a `Too many open files` failure on
the first attempt).

## Verification performed (build-only, no install/restart)

- `cargo`/`make` build finished with exit code 0, no errors (one pre-existing
  unrelated `proc-macro-error2` future-incompatibility warning).
- `nm` on the built `target/release/cosmic-comp` shows the new
  `maybe_activate_pointer_constraint` symbol linked into the binary
  (`xwayland_constraint_focus_override` was small enough to be inlined, so
  it doesn't appear as a separate symbol — expected in a release build).
- Extracted the packaged binary from the `.pkg.tar.zst` and confirmed it's a
  valid stripped ppc64le ELF PIE executable.
- Read through the full call graph by hand: `update_focus_state()` (called
  on every keyboard-focus change, including alt-tab) → new reactivation
  block → `State::maybe_activate_pointer_constraint()` → `is_focused` check
  now includes `xwayland_constraint_focus_override()` → `constraint.activate()`
  when the pointer is within the constraint's region. This is the exact
  code path that runs when an Xwayland surface regains focus.

**What I could not verify**: actual runtime behavior. That requires the
package installed and a fresh cosmic-comp session (Wayland compositors
don't hot-reload; a re-login, not just a restart, is needed to pick up a new
binary from the seat). Per the safety constraints for this task, I did not
install the package, run `pacman -U`, or touch the running compositor.

## To install and test (owner must do this)

```
sudo pacman -U /home/jbettcher/Development/cosmic/cosmic-comp-git/cosmic-comp-git-1.6.0.r4.g8304b18-1-powerpc64le.pkg.tar.zst
```

(Optionally also the matching `-debug` package if you want symbols for a
future `coredumpctl`/`gdb` session: same command with the `-debug` file.)

Then **log out and back in** (a `-U` upgrade does not replace the running
compositor process; cosmic-comp only picks up the new binary on the next
session start via the greeter/systemd unit).

Test: launch a game that uses relative mouselook under Xwayland (native or
Proton), confirm mouselook works, alt-tab out and back in 3-4 times in a
row, confirm mouselook still works after each cycle instead of dying
permanently. If it regresses to the old behavior, that would mean either the
`xwayland_constraint_focus_override` scoping is too narrow for that specific
game's X11 window structure, or there's a second, distinct trigger path —
worth reporting back with which game/launcher combination if so.
