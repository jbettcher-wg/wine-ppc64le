#!/bin/sh
#
# bench-crossing.sh -- measure the price of one guest->native crossing on the
# 64-bit lane, from inside the guest.
#
# NOT A GATE.  This prints numbers; it never fails a sweep.  It exists so a
# crossing-cost change (a bridge protocol, a dispatch rework) gets an A/B in
# seconds instead of a game session:
#
#     WINEPREFIX=... WINEFEXBRIDGE=... ppc64le/cpu/bench-crossing.sh
#     WINE_PPC64LE_NO_TRAP_VIEW=1 WINEPREFIX=... ppc64le/cpu/bench-crossing.sh
#
# The pair above is the trap-view A/B (bridge ABI 6): the first leg runs the
# zero-copy view protocol when the bridge has it, the second forces the
# CONTEXT protocol.  Historical reference points: one i386-lane wow64
# crossing measured ~2.0us (NEXT.md item 2); the bridge-level trap floor
# measured 296/145/86 ns eager/lazy/view (fex 4c776150f, BridgeSmoke S13).
#
# The probe is ppc64le/cpu/probes/crossing_bench.c -- see its header for why
# GetCurrentProcessId is the measured export and guest-side QPC the clock.
# Output (guest stdout, passed through):
#     BENCH qpc_only_ns_per_call=...   the clock's own cost (noise check)
#     BENCH crossing_ns_per_call=...   the number
#
# Exit 0 = ran and printed, 2 = could not run.
set -u

HERE=$(cd "$(dirname "$0")" && pwd)
SRC=$(cd "$HERE/../.." && pwd)
BUILD=${BUILD:-$SRC}

skip() { echo "bench-crossing: $*" >&2; exit 2; }

[ -x "$BUILD/wine" ] || skip "no wine loader at $BUILD/wine"
[ -n "${WINEPREFIX:-}" ] || skip "set WINEPREFIX to a prefix wineboot has run in"
[ -n "${WINEFEXBRIDGE:-}" ] || skip "set WINEFEXBRIDGE to the emulator bridge"
command -v clang >/dev/null || skip "need clang to build the guest probe"
command -v llvm-dlltool >/dev/null || skip "need llvm-dlltool to build the guest probe"

OUT=${OUT:-/tmp/bench-crossing}
mkdir -p "$OUT" || skip "cannot create $OUT"

cat > "$OUT/kernel32.def" <<'EOF'
LIBRARY kernel32.dll
EXPORTS
QueryPerformanceCounter
QueryPerformanceFrequency
GetCurrentProcessId
GetStdHandle
WriteFile
ExitProcess
EOF
llvm-dlltool -m i386:x86-64 -d "$OUT/kernel32.def" -l "$OUT/libkernel32.a" \
    || skip "llvm-dlltool failed"
clang -target x86_64-windows-gnu -nostdlib -O2 -fuse-ld=lld \
    -Wl,--entry=mainCRTStartup -Wl,--subsystem,console \
    -o "$OUT/crossing_bench.exe" "$HERE/probes/crossing_bench.c" "$OUT/libkernel32.a" \
    || skip "guest probe build failed"

echo "bench-crossing: protocol lines from the launch (view/lazy state):" >&2
"$BUILD/wine" "$OUT/crossing_bench.exe" 2>"$OUT/stderr.log"
rc=$?
grep -E "trap view live|lazy trap contexts live|NO_TRAP_VIEW|EAGER_CTX" "$OUT/stderr.log" >&2 || true
[ $rc -eq 0 ] || skip "guest probe exited rc=$rc (stderr: $OUT/stderr.log)"
