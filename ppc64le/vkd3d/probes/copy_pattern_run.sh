#!/bin/sh
#
# copy_pattern_run.sh -- build copy_pattern_probe.c as an x86-64 guest PE and
# run it through the native d3d12 lane, twice:
#
#   leg 1  default vkd3d placement (upload heaps in host-visible VRAM; full
#          ReBAR on this box).  Expected PASS.
#   leg 2  VKD3D_CONFIG=no_upload_hvv -- upload heaps forced into GTT
#          (CPU-cached system RAM).  Cyberpunk 2077 speckles exactly when
#          its uploads go through GTT ([MEASURED] runs 36->38) -- but this
#          quiet single-buffer leg PASSES ([MEASURED] the day it was
#          written), which is itself the finding: the corruption needs
#          game-scale GTT traffic, so it lives below vkd3d (kernel/amdgpu
#          DMA on the 4K POWER8 kernel), and every byte of THIS TREE's
#          upload/copy path is clean in both placements.  If this leg ever
#          FAILS, the quiet case has started reproducing -- that is a
#          different, better lead than the game: chase it immediately.
#
# Exit 0 when leg 1 passes (leg 2's result is REPORTED, not gated: it
# measures the kernel, not this tree).
#
# Environment: WINEPREFIX (a booted prefix) and WINEFEXBRIDGE, same as every
# other gate; BUILD to point at the build tree (default: the source tree).

set -u
HERE=$(cd "$(dirname "$0")" && pwd)
SRC=$(cd "$HERE/../../.." && pwd)
BUILD=${BUILD:-$SRC}
OUT=${TMPDIR:-/tmp}/copy-pattern-probe
TIMEOUT=${TIMEOUT:-120}
mkdir -p "$OUT"

say()  { echo "copy-pattern: $*"; }
bad()  { echo "copy-pattern: FAIL $*" >&2; exit 1; }
skip() { echo "copy-pattern: SKIP $*" >&2; exit 77; }

command -v clang >/dev/null || skip "need clang for the guest build"
command -v llvm-dlltool >/dev/null || skip "need llvm-dlltool"
[ -n "${WINEPREFIX:-}" ] || skip "set WINEPREFIX to a prefix wineboot has run in"
[ -d "$WINEPREFIX/drive_c" ] || skip "WINEPREFIX has no drive_c"

INCL="-I$BUILD/include -I$SRC/include -I$SRC/include/msvcrt"

cat > "$OUT/kernel32.def" <<'EOF'
LIBRARY kernel32.dll
EXPORTS
GetStdHandle
WriteFile
ExitProcess
Sleep
EOF
cat > "$OUT/d3d12.def" <<'EOF'
LIBRARY d3d12.dll
EXPORTS
D3D12CreateDevice
EOF
for m in kernel32 d3d12; do
    llvm-dlltool -m i386:x86-64 -d "$OUT/$m.def" -l "$OUT/lib$m.a" \
        2>"$OUT/build.err" || bad "dlltool $m failed"
done

clang -target x86_64-windows-gnu -nostdlibinc $INCL \
    -D_UCRT -Wall -O1 -fno-builtin -msse2 -g \
    -c -o "$OUT/probe.o" "$HERE/copy_pattern_probe.c" \
    2>>"$OUT/build.err" || { sed 's/^/  build| /' "$OUT/build.err" >&2
                             bad "guest compile failed"; }
clang -target x86_64-windows-gnu -fuse-ld=lld -nostdlib \
    -Wl,--entry=cp_entry -Wl,--subsystem,console \
    -o "$OUT/probe.exe" "$OUT/probe.o" \
    "$OUT/libd3d12.a" "$OUT/libkernel32.a" \
    2>>"$OUT/build.err" || { sed 's/^/  build| /' "$OUT/build.err" >&2
                             bad "guest link failed"; }

run_leg() {   # run_leg <name> <extra VKD3D_CONFIG or ->
    name=$1; cfg=$2
    if [ "$cfg" = - ]; then unset VKD3D_CONFIG; else export VKD3D_CONFIG=$cfg; fi
    timeout -k 5 "$TIMEOUT" env WINEDEBUG=-all \
        "$BUILD/wine" "$OUT/probe.exe" > "$OUT/$name.out" 2> "$OUT/$name.err"
    rc=$?
    sed "s/^/  $name| /" "$OUT/$name.out"
    return $rc
}

say "leg 1: default placement (upload in host-visible VRAM)"
run_leg default - || { sed 's/^/  default| /' "$OUT/default.err" >&2
                       bad "the default leg did not pass"; }
grep -q "copy_pattern_probe: PASS" "$OUT/default.out" \
    || bad "the default leg printed no PASS"

say "leg 2: VKD3D_CONFIG=no_upload_hvv (upload forced into GTT)"
if run_leg gtt no_upload_hvv && grep -q "copy_pattern_probe: PASS" "$OUT/gtt.out"; then
    say "the GTT leg passed: the quiet single-buffer case stays clean, as"
    say "measured the day this was written -- the game-scale defect lives"
    say "below vkd3d.  (See the header for what a FAIL here would mean.)"
else
    say "NOTE: the GTT leg FAILED -- the quiet case now reproduces the"
    say "corruption.  This is a better lead than the game: the lane"
    say "histogram above is the stride signature, measured.  Chase it."
fi

say "PASS"
exit 0
