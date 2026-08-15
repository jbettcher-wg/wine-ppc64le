#!/usr/bin/env bash
# Build vkd3d-proton's native libraries as part of the Wine build.
#
# Called by the generated Makefile (see the WINE_APPEND_RULE in configure.ac:
# "Rules for the ppc64le native D3D12 lane"), never by hand-rolled ritual:
# `./configure && make` in a clean build tree must produce a working native
# D3D12 path with no environment variable set.  The seam between Wine's make
# and vkd3d's meson lives entirely in this script, the same shape as the
# wineandroid.drv rule that drives gradle.
#
#   build-for-wine.sh BUILDDIR WINEBUILDROOT
#
#   BUILDDIR       meson build tree to create/reuse (the Makefile passes
#                  $(CURDIR)/ppc64le/vkd3d-build)
#   WINEBUILDROOT  Wine build tree root; supplies tools/widl/widl, which is a
#                  product of the same build -- vkd3d's idl must be compiled
#                  by the widl this tree ships, not whatever is on PATH.
#
# src/ is reconstructed by bootstrap.sh (pinned upstream commit + our patch
# series) if absent; an existing src/ is trusted as-is so deliberate local
# work in it is not clobbered mid-development.  Run `./bootstrap.sh --check`
# when provenance is in doubt.
#
# The libraries are consumed FROM THE BUILD TREE, not installed: meson bakes
# DT_RUNPATH=$ORIGIN/../d3d12core into libvkd3d-proton-d3d12.so in the build
# layout (src/libs/d3d12/meson.build explains this) and drops it on install,
# so the build layout is the one place the front end finds d3d12core with no
# LD_LIBRARY_PATH.  The Makefile symlinks the front end next to the d3d12
# unixlib, which dlopens it there (dlls/d3d12/unix.c).
#
# ninja does not read MAKEFLAGS, so parallelism is passed explicitly.
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

die() { printf 'build-for-wine: %s\n' "$*" >&2; exit 1; }

[ $# -eq 2 ] || die "usage: build-for-wine.sh BUILDDIR WINEBUILDROOT"
BUILDDIR=$1
WINEROOT=$2

WIDL="$WINEROOT/tools/widl/widl"
[ -x "$WIDL" ] || die "no widl at $WIDL -- is $WINEROOT a Wine build tree?"
command -v meson >/dev/null || die "meson not found (needed to build vkd3d-proton)"
command -v ninja >/dev/null || die "ninja not found (needed to build vkd3d-proton)"

# The upstream checkout is deliberately not vendored; reconstruct it at the
# pinned commit with the patch series applied.  This is the one step that
# needs the network, and only on the first build of a clean checkout.
if [ ! -d "$HERE/src/.git" ]; then
    echo "build-for-wine: no src/, running bootstrap.sh (clones vkd3d-proton)"
    "$HERE/bootstrap.sh"
fi

# -mcpu=power8 matches the measured recipe (README.md "Building"); buildtype
# release matches what every probe result to date was taken against.
if [ ! -f "$BUILDDIR/build.ninja" ]; then
    PATH="$WINEROOT/tools/widl:$PATH" \
        meson setup "$BUILDDIR" "$HERE/src" \
              -Dbuildtype=release \
              -Dc_args=-mcpu=power8 -Dcpp_args=-mcpu=power8
fi

# Only the two libraries the d3d12 unixlib needs -- not tests, not demos.
PATH="$WINEROOT/tools/widl:$PATH" \
    ninja -C "$BUILDDIR" -j "$(nproc)" \
          libs/d3d12/libvkd3d-proton-d3d12.so \
          libs/d3d12core/libvkd3d-proton-d3d12core.so
