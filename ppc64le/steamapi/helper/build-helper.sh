#!/bin/sh
#
# build-helper.sh -- cross-compile the Steam bridge helper: an x86-64 Linux
# executable, built on ppc64le, run under FEX via binfmt.
#
# The helper is deliberately NOT part of "make".  It is not a Wine component:
# it is a Linux program for a different architecture, linked against a
# different libc, and its whole reason to exist is that this machine's Wine
# cannot load an x86-64 SysV shared object.  Wiring a second toolchain into
# the tree's build for one program would buy nothing and would make a missing
# FEX rootfs a build failure for everybody.  So it is built by this script,
# by ppc64le/steamapi/check-steam-bridge.sh, and by whoever installs the
# compat tool.
#
# ---------------------------------------------------------------------------
# WHAT THE HOST ACTUALLY HAS (measured on the test machine, not assumed)
#
#   $ clang --version
#   clang version 22.1.8 ... Target: powerpc64le-unknown-linux-gnu
#   $ clang -print-targets | grep x86-64
#       x86-64      - 64-bit X86: EM64T and AMD64
#   $ ld.lld --version
#   LLD 22.1.8 (compatible with GNU linkers)
#
# The compiler is already a cross compiler -- LLVM ships every backend in one
# binary -- so only the linker and the sysroot need arranging.
#
# ---------------------------------------------------------------------------
# WHAT DOES NOT WORK, AND WHY (each of these was tried)
#
#  1) Without -fuse-ld=lld.  clang drives /usr/bin/ld, a ppc-only binutils:
#       /usr/bin/ld: unrecognised emulation mode: elf_x86_64
#     Its supported-emulations list contains no x86 at all.  lld is mandatory.
#
#  2) Without --sysroot.  clang falls back to the host's ppc64le glibc headers
#     and multilib-probes for a 32-bit stub:
#       /usr/include/gnu/stubs.h: 'gnu/stubs-32.h' file not found
#
#  3) With -static.  This BUILDS CLEAN and a hello-world runs, which is the
#     trap -- but glibc cannot dlopen from a fully static image, and the
#     failure is a SIGSEGV inside dlopen (exit 139) after the banner lines
#     have already printed.  It does not fail politely.  Dynamic, against the
#     rootfs's own ld-linux-x86-64.so.2, is both simpler and correct.
#
# ---------------------------------------------------------------------------
# WHAT IT BUILDS
#
#   * Proton's unix-side lsteamclient, unmodified, out of the vendored tree at
#     dlls/steamclient64/proton: unixlib.cpp, unixlib_generated.cpp, the
#     cppISteam*.cpp per-interface wrappers and the unix_steam_*.cpp manual
#     ones.  219 translation units, and they compile for x86-64 Linux against
#     this tree's Wine headers with zero errors -- the same code Proton runs,
#     so the struct conversions and callback bookkeeping are not reimplemented.
#   * steamhelper.c   -- the socket transport and the frame loop.
#   * steamhelper_stub.c -- the ntdll entry points a Wine unix library gets
#     for free and a standalone program does not.
#   * steamhelper_path.c -- DOS<->unix file names, against the drive map the
#     client measures in its own prefix and sends at connect time.
#
# Usage:  build-helper.sh [-o OUTPUT] [-j N]
# Env:    WINE_TREE   the wine-ppc64le tree (default: three levels up)
#         FEX_ROOTFS  the x86-64 sysroot (default: the FEX ArchLinux rootfs)

set -eu

HERE=$(cd "$(dirname "$0")" && pwd)
TREE=${WINE_TREE:-$(cd "$HERE/../../.." && pwd)}
ROOTFS=${FEX_ROOTFS:-$HOME/.local/share/fex-emu/RootFS/ArchLinux}
OUT=$HERE/steamhelper
JOBS=$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 8)
OBJDIR=${OBJDIR:-/tmp/steamhelper-build}

while [ $# -gt 0 ]; do
    case $1 in
    -o) OUT=$2; shift 2 ;;
    -j) JOBS=$2; shift 2 ;;
    *)  echo "build-helper.sh: unknown argument $1" >&2; exit 2 ;;
    esac
done

P=$TREE/dlls/steamclient64/proton
D=$TREE/dlls/steamclient64

for f in "$ROOTFS/usr/include/dlfcn.h" \
         "$P/unixlib.cpp" "$D/steamrpc_wire.h"; do
    [ -e "$f" ] || { echo "build-helper.sh: missing $f" >&2; exit 2; }
done

# Scrt1.o sits in usr/lib on an Arch-style rootfs and in
# usr/lib/x86_64-linux-gnu on a Debian/Ubuntu one, which uses multiarch paths.
# clang's own Linux driver knows both given --sysroot, so only this preflight
# check needed teaching -- it hardcoded the Arch layout and refused a perfectly
# good Ubuntu_24_04 rootfs with "missing .../usr/lib/Scrt1.o".
crt=
for c in "$ROOTFS/usr/lib/Scrt1.o" "$ROOTFS/usr/lib/x86_64-linux-gnu/Scrt1.o"; do
    [ -e "$c" ] && { crt=$c; break; }
done
[ -n "$crt" ] || {
    echo "build-helper.sh: no x86-64 Scrt1.o under $ROOTFS/usr/lib" >&2
    echo "  looked in usr/lib (Arch layout) and usr/lib/x86_64-linux-gnu (Debian/Ubuntu)." >&2
    echo "  FEX_ROOTFS names the sysroot; it is currently $ROOTFS" >&2
    exit 2
}

command -v clang++ >/dev/null || { echo "build-helper.sh: need clang++" >&2; exit 2; }

INCL="-I$TREE/include -I$P -I$D -I$HERE"
# -Wno-pragma-pack: Wine's own pshpack1.h nests inside Proton's, which clang
# warns about and which is intentional in both.
# -Wno-format-extra-args: one such line in Proton's unixlib.cpp; vendored code
# is not reformatted here, and the warning names it.
COMMON="-target x86_64-linux-gnu --sysroot=$ROOTFS -O1 -fPIC -g \
 -DWINE_UNIX_LIB -D__WINESRC__ -Wno-pragma-pack -Wno-format-extra-args $INCL"

mkdir -p "$OBJDIR"
rm -f "$OBJDIR"/*.o "$OBJDIR"/*.err

echo "build-helper.sh: tree   $TREE"
echo "build-helper.sh: sysroot $ROOTFS"
echo "build-helper.sh: objects $OBJDIR"

# shellcheck disable=SC2086
ls "$P"/unixlib.cpp "$P"/unixlib_generated.cpp "$P"/unix_steam_*.cpp \
   "$P"/cppISteam*.cpp \
  | xargs -P "$JOBS" -I{} sh -c \
      "clang++ $COMMON -std=c++17 -c -o $OBJDIR/\$(basename {} .cpp).o {} \
        2> $OBJDIR/\$(basename {} .cpp).err || echo 'FAILED {}'"

# shellcheck disable=SC2086
for f in "$HERE"/steamhelper.c "$HERE"/steamhelper_stub.c "$HERE"/steamhelper_path.c; do
    clang $COMMON -std=gnu11 -Wall -Wextra -Wno-unused-parameter \
        -c -o "$OBJDIR/$(basename "$f" .c).o" "$f"
done

if grep -l "error:" "$OBJDIR"/*.err >/dev/null 2>&1; then
    echo "build-helper.sh: compile errors:" >&2
    grep -h "error:" "$OBJDIR"/*.err | head -20 >&2
    exit 1
fi

clang++ -target x86_64-linux-gnu --sysroot="$ROOTFS" -fuse-ld=lld \
    -o "$OUT" "$OBJDIR"/*.o -ldl -lpthread

echo "build-helper.sh: $OUT"
file "$OUT" 2>/dev/null || true
