#!/bin/sh
#
# copy_storm_run.sh -- build copy_storm_probe.c as an x86-64 guest PE and
# run it through the native d3d12 lane, twice, like copy_pattern_run.sh but
# at game scale (threads, queue-type mix, deep pipelining, ~GBs verified):
#
#   leg 1  default vkd3d placement (upload heaps in host-visible VRAM).
#   leg 2  VKD3D_CONFIG=no_upload_hvv -- upload heaps forced into GTT,
#          the placement Cyberpunk 2077 speckled hardest in.
#
# The probe's verdict classes name the culprit on a FAIL:
#   staging-pre    the emulator's store/read path damaged the bytes before
#                  the GPU ever saw them,
#   roundtrip      the copy machinery (marshal walker, vkd3d, kernel),
#   staging-post   something else scribbled the upload heap during the trip.
#
# Exit 0 when leg 1 passes (leg 2 is reported, not gated, same convention
# as copy_pattern_run.sh).  STORM_THREADS/STORM_ITERS/STORM_CHUNK_MB pass
# through to the probe.
#
# Environment: WINEPREFIX (a booted prefix) and WINEFEXBRIDGE, same as the
# other probes; BUILD to point at the build tree (default: the source tree).

set -u
HERE=$(cd "$(dirname "$0")" && pwd)
SRC=$(cd "$HERE/../../.." && pwd)
BUILD=${BUILD:-$SRC}
OUT=${TMPDIR:-/tmp}/copy-storm-probe
TIMEOUT=${TIMEOUT:-900}
mkdir -p "$OUT"

say()  { echo "copy-storm: $*"; }
bad()  { echo "copy-storm: FAIL $*" >&2; exit 1; }
skip() { echo "copy-storm: SKIP $*" >&2; exit 77; }

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
CreateThread
WaitForSingleObject
GetEnvironmentVariableA
VirtualAlloc
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
    -c -o "$OUT/probe.o" "$HERE/copy_storm_probe.c" \
    2>>"$OUT/build.err" || { sed 's/^/  build| /' "$OUT/build.err" >&2
                             bad "guest compile failed"; }
clang -target x86_64-windows-gnu -fuse-ld=lld -nostdlib \
    -Wl,--entry=cs_entry -Wl,--subsystem,console \
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
if run_leg default - && grep -q "copy_storm_probe: PASS" "$OUT/default.out"; then
    say "the default leg passed"
else
    sed 's/^/  default| /' "$OUT/default.err" >&2
    bad "the default leg did not pass"
fi

say "leg 2: VKD3D_CONFIG=no_upload_hvv (upload forced into GTT)"
if run_leg gtt no_upload_hvv && grep -q "copy_storm_probe: PASS" "$OUT/gtt.out"; then
    say "the GTT leg passed too: game-scale traffic through this probe does"
    say "not reproduce the speckle; the defect needs something the game does"
    say "that this storm still does not"
else
    say "NOTE: the GTT leg FAILED -- the verdict classes above name the"
    say "layer; chase that before the game."
fi

say "PASS"
exit 0
