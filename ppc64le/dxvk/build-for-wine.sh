#!/usr/bin/env bash
# Build DXVK's native ppc64le libraries as part of the Wine build.
#
# Called by the generated Makefile (see the WINE_APPEND_RULE in configure.ac:
# "Rules for the ppc64le native D3D11 lane"), never by hand-rolled ritual:
# `./configure && make` in a clean build tree must produce a working native
# D3D11 path with no environment variable set.  The seam between Wine's make
# and DXVK's meson lives entirely in this script, the same shape as
# ppc64le/vkd3d/build-for-wine.sh and the wineandroid.drv rule that drives
# gradle.
#
#   build-for-wine.sh BUILDDIR
#
#   BUILDDIR   meson build tree to create/reuse (the Makefile passes
#              $(CURDIR)/ppc64le/dxvk-build)
#
# Unlike vkd3d's, this needs no widl: DXVK's native build ships the
# MinGW-w64/widl OUTPUT vendored at src/include/native/directx and compiles
# against that, which is also the roster gen_interfaces.py reads.  One set of
# declarations, one vtable layout, no second compiler in the path.
#
# src/ is reconstructed by bootstrap.sh (pinned upstream commit + our patch
# series) if absent; an existing src/ is trusted as-is so deliberate local work
# in it is not clobbered mid-development.  Run `./bootstrap.sh --check` when
# provenance is in doubt.
#
# THE LIBRARIES ARE CONSUMED FROM THE BUILD TREE, NOT INSTALLED.  meson bakes
#   libdxvk_d3d11.so.0     DT_RUNPATH $ORIGIN/../dxgi
#   libdxvk_d3d10core.so.0 DT_RUNPATH $ORIGIN/../d3d11:$ORIGIN/../dxgi
#   libdxvk_d3d9.so.0      (no DXVK dependency -- d3d9 is self-contained)
# into the build layout and strips them on install, so the build layout is the
# one place d3d11 finds its dxgi half with no LD_LIBRARY_PATH.  The Makefile
# symlinks the three into dlls/d3d11/, and dlls/d3d11/unix.c REALPATHS the
# symlink before dlopen -- glibc expands $ORIGIN from the path the object was
# loaded by, not from its realpath, so loading through the symlink itself would
# lose the dxgi half (measured on the vkd3d lane, dlls/d3d12/unix.c).
#
# -mcpu=power8 is the project floor and is not optional here: it is what
# ppc64le/dxvk/check-d3d11-smoke.sh's scan leg asserts, and what makes these
# objects run on POWER8 as well as POWER9.  On a machine whose makepkg.conf
# says power9, "we forgot the flag" and "we targeted POWER9" produce identical
# output and nobody finds out until someone else's build will not start.
#
# ninja does not read MAKEFLAGS -- left alone it launches roughly core-count+2
# jobs, which on a big POWER box has exhausted memory and killed a link.  -j is
# passed explicitly for that reason, not for politeness.
#
# BUILD DEPENDENCIES beyond Wine's own: meson, ninja, glslang, and SDL2 or GLFW
# development files (upstream's meson.build errors out without one of them even
# though this lane presents through win32u and never uses either -- see
# README.md "What the build needs").
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

die() { printf 'build-for-wine: %s\n' "$*" >&2; exit 1; }

[ $# -eq 1 ] || die "usage: build-for-wine.sh BUILDDIR"
BUILDDIR=$1

command -v meson >/dev/null || die "meson not found (needed to build DXVK)"
command -v ninja >/dev/null || die "ninja not found (needed to build DXVK)"

# The upstream checkout is deliberately not vendored; reconstruct it at the
# pinned commit with the patch series applied.  This is the one step that needs
# the network, and only on the first build of a clean checkout.
if [ ! -d "$HERE/src/.git" ]; then
    echo "build-for-wine: no src/, running bootstrap.sh (clones DXVK)"
    "$HERE/bootstrap.sh"
fi

# The roster both halves of the COM boundary are generated from must still
# describe these headers.  A drifted JSON dispatches to the neighbouring slot,
# and nothing downstream catches that until the runtime IID cross-check at
# attach -- by which point the build has already claimed success.
"$HERE/gen_interfaces.py" --check "$HERE/interfaces_dxvk.json"

# -Wno-psabi is not cosmetic here.  GCC on ppc64le emits a psABI note for every
# function that passes a small aggregate by value -- DXVK's d3d9 has several
# (`std::pair<float,float> D3D9DeviceEx::ComputeWNearFar()`) -- and the note is
# about a GCC 10.1 calling-convention change that cannot bite a library whose
# every caller is compiled here, at once, by this compiler.  Left on, it puts
# upstream's diagnostics in the output of every Wine build, which is how a
# zero-warning build stops being a signal.
if [ ! -f "$BUILDDIR/build.ninja" ]; then
    meson setup "$BUILDDIR" "$HERE/src" \
          --buildtype release \
          -Dnative_sdl3=disabled \
          -Dc_args="-mcpu=power8 -mtune=power8 -Wno-psabi" \
          -Dcpp_args="-mcpu=power8 -mtune=power8 -Wno-psabi"
fi

# THE DEFAULT TARGET, AND NOT A LIST OF LIBRARY NAMES.
#
# This used to name the four libraries by their UNVERSIONED symlinks, on the
# reasoning that the versioned names (libdxvk_d3d11.so.0.30002) would stop
# naming anything the day the pinned commit's version changed.  The reasoning
# was right and the conclusion was wrong: MESON DOES NOT EMIT A NINJA TARGET
# FOR THE UNVERSIONED NAME.  It creates that symlink as a side effect of the
# link rule for the versioned library, so `ninja src/d3d11/libdxvk_d3d11.so`
# names a file with no rule -- which ninja treats as a SOURCE, not a target.
#
# [MEASURED] 2026-08-17, the test machine.  With a new file added to src/wsi and build.ninja
# correctly regenerated to compile it, `ninja src/dxgi/libdxvk_dxgi.so
# src/d3d11/libdxvk_d3d11.so src/d3d10/libdxvk_d3d10core.so` printed "ninja: no
# work to do" and exited zero, twice, while `ninja src/wsi/libwsi.a` on the same
# tree had twelve objects to compile.  Every incremental DXVK build this lane
# has ever done was a no-op that reported success -- invisible until the day a
# patch to DXVK had to change behaviour, which is exactly when it matters.  The
# existence check below could never have caught it: the files were there,
# because the FIRST build had built them.
#
# The default target builds d3d8 and d3d9 as well.  d3d9 is now served (see
# dlls/d3d9), d3d8 is a thin layer over it, and upstream's diagnostics are
# quieted at the source by -Wno-psabi above rather than by not compiling the
# files that emit them.
ninja -C "$BUILDDIR" -j "$(nproc)"

# What the Makefile is about to symlink must exist.  meson emits an unversioned
# symlink beside each versioned library in the build layout; those are the
# stable names, and the ones the rule in configure.ac uses.
for lib in dxgi/libdxvk_dxgi d3d11/libdxvk_d3d11 d3d10/libdxvk_d3d10core \
           d3d9/libdxvk_d3d9; do
    [ -e "$BUILDDIR/src/$lib.so" ] || \
        die "$BUILDDIR/src/$lib.so was not produced -- upstream's library
    layout has moved, and the symlinks dlls/d3d11 dlopens would dangle."
done
