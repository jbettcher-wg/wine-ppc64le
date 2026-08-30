#!/bin/sh
#
# build-presence.sh -- build the guest x86-64 steampresence.exe.
#
# Same recipe ppc64le/steamapi/check-steam-bridge.sh uses for its guest probe,
# and for the same reason: clang on this box is already a cross compiler (LLVM
# ships every backend), so a guest PE needs no mingw and no extra toolchain.
# The imports are named by hand -- the one kernel32 the tree just built -- so
# what this binds to is visible here rather than in a generated Makefile.
#
# The output is committed alongside the source, exactly as helper/steamhelper
# is, so that a checkout can launch a title without a build step first.
#
# WINE_PPC64LE_TREE names the wine BUILD directory (default: the sibling
# wine-build beside the source tree).
set -eu

here=$(cd "$(dirname "$0")" && pwd)
src=$(cd "$here/../../.." && pwd)
build=${WINE_PPC64LE_TREE:-$(cd "$src/.." && pwd)/wine-build}

[ -d "$build" ] || { echo "build-presence: no wine build at $build" >&2; exit 1; }
k32=$build/dlls/kernel32/x86_64-windows/kernel32.dll
[ -f "$k32" ] || { echo "build-presence: no guest kernel32 at $k32" >&2; exit 1; }
command -v clang >/dev/null || { echo "build-presence: need clang" >&2; exit 1; }

incl="-I$build/include -I$src/include -I$src/include/msvcrt"

clang -target x86_64-windows-gnu -nostdlibinc $incl -D_MSVCR_VER=0 \
    -Wall -Wextra -O1 -fno-builtin -g -c \
    -o "$here/steampresence.o" "$here/steampresence.c"

clang -target x86_64-windows-gnu -fuse-ld=lld -nostdlib \
    -Wl,--entry=steampresence_entry -Wl,--subsystem,console \
    -o "$here/steampresence.exe" "$here/steampresence.o" "$k32"

rm -f "$here/steampresence.o"
echo "build-presence: $here/steampresence.exe"
