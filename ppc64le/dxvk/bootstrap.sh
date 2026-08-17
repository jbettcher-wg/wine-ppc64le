#!/usr/bin/env bash
# Reconstruct the DXVK source tree this lane builds against.
#
# WHY THIS EXISTS -- and why src/ is not vendored.  `src/` is an upstream DXVK
# checkout and is deliberately gitignored: we do not carry a copy of someone
# else's repository in the Wine tree.  Our changes to DXVK live in
# `dxvk-patches/` as a revertible series, so the provenance of every changed
# line is `git diff` and DXVK's zlib notice travels with the source it belongs
# to.  But that also means a fresh clone of the Wine tree builds vanilla DXVK
# and silently gets none of the series, and without a pinned commit you get
# whatever upstream HEAD is that day -- against which the patches may not even
# apply.
#
# So: clone at a known commit, apply the series, fail loudly if anything drifts.
#
#   ./bootstrap.sh              # clone + patch
#   ./bootstrap.sh --check      # verify an existing src/ matches, change nothing
#   ./bootstrap.sh --force      # re-clone from scratch, discarding src/
#
# THE WINE BUILD RUNS THIS FOR YOU.  `./configure && make` drives
# build-for-wine.sh, which bootstraps src/ if it is absent -- the one step that
# needs the network, and only on the first build of a clean tree.
#
# Same arrangement, same reasons, as ppc64le/vkd3d/bootstrap.sh.  This one is
# the DXVK sibling of it, carried here because THE FOLD IS AUTHORITATIVE: the
# Wine build drives the meson build, reads interfaces_dxvk.json at build time,
# and versions all of it in the Wine repo.  The standalone dxvk-ppc64le/ working
# copy is a mirror for work outside the Wine tree; when the two disagree, the
# fold wins.
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
SRC="$HERE/src"
PATCHES="$HERE/dxvk-patches"

# The commit our patches were written against.  Do not bump this without
# re-testing the series -- a patch that applies is not a patch that is correct.
DXVK_REPO="https://github.com/doitsujin/dxvk.git"
DXVK_COMMIT="3a4c6fa3cb1548d56a90a38dd8f526b6c13e63fd"   # [d3d9] Don't test clip planes for pre-transformed vertices

mode="apply"
case "${1:-}" in
  --check) mode="check" ;;
  --force) mode="force" ;;
  "") ;;
  *) echo "usage: $0 [--check|--force]" >&2; exit 2 ;;
esac

say() { printf '  %s\n' "$*"; }
die() { printf 'bootstrap: %s\n' "$*" >&2; exit 1; }

# ---- src/ ------------------------------------------------------------------
if [ "$mode" = force ] && [ -d "$SRC" ]; then
    say "removing existing src/"
    rm -rf "$SRC"
fi

if [ ! -d "$SRC/.git" ]; then
    [ "$mode" = check ] && die "src/ is absent; run without --check first"
    say "cloning DXVK"
    git clone --quiet "$DXVK_REPO" "$SRC"
fi

cur="$(git -C "$SRC" rev-parse HEAD)"
if [ "$cur" != "$DXVK_COMMIT" ]; then
    if [ "$mode" = check ]; then
        die "src/ is at $cur, expected $DXVK_COMMIT"
    fi
    say "checking out $DXVK_COMMIT"
    git -C "$SRC" fetch --quiet origin "$DXVK_COMMIT" 2>/dev/null || git -C "$SRC" fetch --quiet origin
    git -C "$SRC" checkout --quiet "$DXVK_COMMIT"
fi

# Submodules are pinned by the gitlinks in that commit, so this lands them all
# at the right revisions without us tracking them separately.
say "syncing submodules"
git -C "$SRC" submodule update --init --recursive --quiet

# ---- patches ---------------------------------------------------------------
# Applied to the WORKING TREE and never committed, which is what lets a probe
# recover the pre-change behaviour with `git show HEAD:<file>`.
shopt -s nullglob
series=("$PATCHES"/*.patch)
shopt -u nullglob

if [ ${#series[@]} -eq 0 ]; then
    say "no patches in dxvk-patches/ -- nothing to apply"
    exit 0
fi

STAMP="$SRC/.dxvk-patch-stamp"
top="${series[${#series[@]} - 1]}"

# What a correctly patched src/ must contain: the commit, and the md5 of every
# patch in series order.  Regenerated here and compared, never trusted from
# disk alone -- see the banner on the check below.
want_stamp() {
    printf '%s\n' "$DXVK_COMMIT"
    for p in "${series[@]}"; do
        printf '%s  %s\n' "$(md5sum <"$p" | cut -d' ' -f1)" "$(basename "$p")"
    done
}

# THE SERIES IS STACKED, WHICH IS WHY "IS IT APPLIED?" IS NOT ASKED PER PATCH.
#
# This used to ask each patch in turn "would a reverse-apply succeed?" and call
# that already-applied.  That question stopped having an answer the moment two
# patches touched the same lines, which 0003 does: it adds `&Win32uWSI,` to the
# WSI driver table 0001 created, so 0001's own hunks no longer describe the
# file -- their context now carries 0003's line.  [MEASURED] `--check` on a
# tree with all three correctly applied reported `not applied:
# 0001-foreign-wsi-backend.patch`, which is exactly backwards, and the apply
# path would then have died with "does not apply" on a tree that was fine.
#
# Nor can the stack be checked in one go: `git apply --check a b c` re-reads
# each file FROM DISK for every patch instead of chaining them, so a reverse
# check of the whole series in reverse order fails on everything below the top.
# [MEASURED] the same tree, `git apply --reverse --check 0003 0002 0001`, four
# "patch does not apply" errors -- all of them from 0001 and 0002.  Forward
# application does chain, because each `git apply` is a separate process that
# re-reads what the last one wrote; that asymmetry is the whole problem.
#
# So the applied state is recorded, and what is recorded is the md5 of every
# patch file in series order plus the pinned commit.  A stamp that still
# matches means the tree was built from exactly these patch files.  It is
# checked together with a reverse-apply of the TOP patch, which by construction
# has nothing stacked above it and therefore must always reverse cleanly -- so
# a src/ someone reverted or rebuilt by hand is still caught.  What the pair
# does NOT prove is that a patch in the MIDDLE of the stack was not hand-edited
# in the tree afterwards; `git -C src diff` is the tool for that, and it is the
# same limitation the old per-patch loop had.
if [ -f "$STAMP" ] && [ "$(cat "$STAMP")" = "$(want_stamp)" ] &&
   git -C "$SRC" apply --reverse --check "$top" >/dev/null 2>&1; then
    for p in "${series[@]}"; do say "already applied: $(basename "$p")"; done
elif [ "$mode" = check ]; then
    if [ -f "$STAMP" ]; then
        die "src/ carries a patch stamp that no longer describes
    dxvk-patches/ (a patch was edited, added or removed since it was applied),
    or the top patch no longer reverses cleanly. Re-run ./bootstrap.sh --force."
    fi
    die "not applied: src/ has no patch stamp; the ${#series[@]}-patch series
    is absent. Run ./bootstrap.sh without --check."
else
    [ -f "$STAMP" ] && die "src/ carries a stale patch stamp.  Re-applying a
    series on top of a partly-applied one is how a hunk lands in roughly the
    right place and a fix becomes silently wrong; ./bootstrap.sh --force
    re-clones and starts over, and \`git -C src diff\` is the local work you
    would lose."
    for p in "${series[@]}"; do
        name="$(basename "$p")"
        # Separate invocations on purpose: each one re-reads what the last
        # wrote, which is the only way a stacked series applies at all.
        git -C "$SRC" apply --check "$p" 2>/dev/null || die "does not apply: $name
    The pinned commit and the patch have drifted apart. Re-derive the change
    against $DXVK_COMMIT rather than forcing the diff -- a hunk landing in
    roughly the right place is how a fix becomes silently wrong."
        git -C "$SRC" apply "$p"
        say "applied: $name"
    done
    want_stamp >"$STAMP"
fi

if [ "$mode" = check ]; then
    say "check passed: src/ at $DXVK_COMMIT with ${#series[@]} patch(es) applied"
    exit 0
fi

printf '\nbootstrap complete: %d patch(es) applied at %s\n' \
    "${#series[@]}" "${DXVK_COMMIT:0:9}"
printf 'next: nothing -- the Wine build calls build-for-wine.sh itself.\n'
printf '      ./gen_interfaces.py --check interfaces_dxvk.json   # roster provenance\n'
