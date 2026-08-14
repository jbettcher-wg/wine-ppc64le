#!/usr/bin/env bash
# Reconstruct the vkd3d-proton source tree this project builds against.
#
# WHY THIS EXISTS. `src/` is an upstream vkd3d-proton checkout and is
# deliberately not vendored -- we do not carry a copy of someone else's
# repository. Our changes live in `vkd3d-patches/`, so a fresh clone of *this*
# repo would build vanilla vkd3d-proton and silently get none of them. Worse,
# without a pinned commit you would get whatever upstream HEAD is that day and
# the patches may not apply at all.
#
# So: clone at a known commit, apply the series, fail loudly if anything drifts.
# Same shape as dxvk-ppc64le/bootstrap.sh, on purpose -- two ports that differ
# in their bring-up ritual cost more than they save.
#
#   ./bootstrap.sh              # clone + patch
#   ./bootstrap.sh --check      # verify an existing src/ matches, change nothing
#   ./bootstrap.sh --force      # re-clone from scratch, discarding src/
#
# The series is applied to the WORKING TREE and never committed. That is what
# lets probes/tagged_handle_run.sh recover the pre-change behaviour with
# `git show HEAD:<file>` and build an honest before/after.
#
# LICENCE. vkd3d-proton is LGPL-2.1-or-later, which unlike DXVK's zlib licence
# places real obligations on anyone distributing MODIFIED BINARIES: the
# modifications stay LGPL and recipients must be able to relink. Keeping our
# changes as a patch series against a pinned upstream commit, rather than as a
# fork, is what makes that obligation cheap to meet -- the provenance of every
# line is `git diff`.
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
SRC="$HERE/src"
PATCHES="$HERE/vkd3d-patches"

# The commit our patches were written against. Do not bump this without
# re-testing the series -- a patch that applies is not a patch that is correct.
VKD3D_REPO="https://github.com/HansKristian-Work/vkd3d-proton.git"
VKD3D_COMMIT="238f157e1d64f90e0d90593557c092ab8af6e0a3"   # vkd3d: Rethink how we do barriers for COPY <-> RESOURCE.

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
    say "cloning vkd3d-proton"
    git clone --quiet "$VKD3D_REPO" "$SRC"
fi

cur="$(git -C "$SRC" rev-parse HEAD)"
if [ "$cur" != "$VKD3D_COMMIT" ]; then
    if [ "$mode" = check ]; then
        die "src/ is at $cur, expected $VKD3D_COMMIT"
    fi
    say "checking out $VKD3D_COMMIT"
    git -C "$SRC" fetch --quiet origin "$VKD3D_COMMIT" 2>/dev/null || git -C "$SRC" fetch --quiet origin
    git -C "$SRC" checkout --quiet "$VKD3D_COMMIT"
fi

# dxil-spirv and the Khronos headers are pinned by the gitlinks in that commit.
say "syncing submodules"
git -C "$SRC" submodule update --init --recursive --quiet

# ---- patches ---------------------------------------------------------------
shopt -s nullglob
series=("$PATCHES"/*.patch)
shopt -u nullglob

if [ ${#series[@]} -eq 0 ]; then
    say "no patches in vkd3d-patches/ -- nothing to apply"
    exit 0
fi

applied=0 already=0
for p in "${series[@]}"; do
    name="$(basename "$p")"
    # Already applied? Then a reverse-apply would succeed. Check that first so
    # re-running bootstrap is safe rather than an error.
    if git -C "$SRC" apply --reverse --check "$p" >/dev/null 2>&1; then
        say "already applied: $name"
        already=$((already + 1))
        continue
    fi
    if [ "$mode" = check ]; then
        die "not applied: $name"
    fi
    if ! git -C "$SRC" apply --check "$p" >/dev/null 2>&1; then
        die "does not apply: $name
    The pinned commit and the patch have drifted apart. Re-derive the change
    against $VKD3D_COMMIT rather than forcing the diff -- a hunk landing in
    roughly the right place is how a fix becomes silently wrong."
    fi
    git -C "$SRC" apply "$p"
    say "applied: $name"
    applied=$((applied + 1))
done

if [ "$mode" = check ]; then
    say "check passed: src/ at $VKD3D_COMMIT with ${already} patch(es) applied"
    exit 0
fi

printf '\nbootstrap complete: %d applied, %d already present, at %s\n' \
    "$applied" "$already" "${VKD3D_COMMIT:0:9}"
printf 'next: ./probes/tagged_handle_run.sh    # the before/after for the series\n'
