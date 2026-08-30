#!/bin/sh
#
# run-d3d9-smoke32.sh -- build probes/d3d9_smoke.c as an i386 PE and run it
# under this port, to exercise the 32-bit D3D9 lane WITHOUT launching a game.
#
# WHY THIS IS NOT A check-*.sh.  The check- scripts in this directory are
# gates: they compare a guest run against a native reference and are expected
# to be green.  This one is a MEASURING INSTRUMENT.  The 32-bit D3D9 lane is
# not finished -- the Lock family still refuses, loudly, until the below-4-GiB
# bounce exists -- so a green run is not yet the expected outcome and calling
# this a gate would be a lie.  Promote it when the bounce lands.
#
# It is also the cheap way to work on this lane: no foreground, no game lock,
# no GPU submission beyond a couple of clears, and it reaches every hand32
# walker on the surface in about four seconds.
#
# The probe fabricates an HWND and never calls Present, so it needs
# d3d9.deferSurfaceCreation -- probes/d3d9_smoke.c's own banner explains why
# at length, and without it CreateDevice fails in win32u's desktop-window
# path rather than in anything to do with marshalling.
#
#   WINEPREFIX=<a prefix wineboot has run in> ./run-d3d9-smoke32.sh
#
# [MEASURED 2026-08-30] steps 1-6 pass; step 7 (LockRect) refuses with the
# host address it was handed, which is what proves the bounce is required.
set -u

HERE=$(cd "$(dirname "$0")" && pwd)
SRC=$(cd "$HERE/../.." && pwd)
BUILD=${WINE_PPC64LE_TREE:-$SRC/../wine-build}
OUT=$BUILD/d3d9probe32

say()  { echo "run-d3d9-smoke32: $*"; }
skip() { echo "run-d3d9-smoke32: $*" >&2; exit 2; }

[ -x "$BUILD/wine" ] || skip "no $BUILD/wine"
[ -f "$BUILD/dlls/d3d9/i386-windows/d3d9.dll" ] || \
    skip "no i386 d3d9 thunk half; d3d9.thunks needs GUEST-MACHINE i386 and a build"
[ -n "${WINEPREFIX:-}" ] || skip "set WINEPREFIX to a prefix wineboot has run in"
[ -n "$(ls -A "$WINEPREFIX/drive_c/windows/syswow64" 2>/dev/null)" ] || \
    skip "$WINEPREFIX has no populated syswow64; the WoW64 lane is not staged"
command -v clang >/dev/null || skip "need clang"
command -v llvm-dlltool >/dev/null || skip "need llvm-dlltool"

mkdir -p "$OUT" || skip "cannot create $OUT"
INCL="-I$BUILD/include -I$SRC/include -I$SRC/include/msvcrt"

cat > "$OUT/kernel32.def" <<'EOF'
LIBRARY kernel32.dll
EXPORTS
GetStdHandle
WriteFile
ExitProcess
EOF
cat > "$OUT/d3d9.def" <<'EOF'
LIBRARY d3d9.dll
EXPORTS
Direct3DCreate9
EOF
for m in kernel32 d3d9; do
    llvm-dlltool -m i386 -d "$OUT/$m.def" -l "$OUT/lib${m}32.a" \
        || skip "llvm-dlltool failed for $m"
done
clang -target i386-windows-gnu -nostdlibinc $INCL -Wall -O1 -fno-builtin -g \
    -c -o "$OUT/guest32.o" "$HERE/probes/d3d9_smoke.c" || skip "i386 compile failed"
# /safeseh:no -- a hand-written stub array carries no exception handlers, the
# same flag the i386 thunk halves are linked with.
clang -target i386-windows-gnu -fuse-ld=lld -nostdlib \
    -Wl,--entry=d3d9_smoke_entry -Wl,--subsystem,console -Wl,/safeseh:no \
    -o "$OUT/guest32.exe" "$OUT/guest32.o" "$OUT/libd3d932.a" "$OUT/libkernel3232.a" \
    2>/dev/null || skip "i386 link failed"
say "built $OUT/guest32.exe"

DXVK_LOG_PATH=$OUT
DXVK_CONFIG=${DXVK_CONFIG:-"d3d9.deferSurfaceCreation = True"}
export DXVK_LOG_PATH DXVK_CONFIG

timeout -s TERM "${TIMEOUT:-180}" \
    env WINEDEBUG="${WINEDEBUG:-fixme+winecom,err+winecom,+d3d9}" \
        WINEDLLOVERRIDES="winedbg.exe=d" \
    "$BUILD/wine" "$OUT/guest32.exe" > "$OUT/g32.out" 2> "$OUT/g32.err"
rc=$?

cat "$OUT/g32.out"
echo
say "rc=$rc; refusals and hand32 traces below (full stderr in $OUT/g32.err)"
grep -iE "refus|guest_legal|hand32_d3d9" "$OUT/g32.err" || say "(none)"
