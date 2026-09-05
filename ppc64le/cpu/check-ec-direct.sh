#!/bin/sh
#
# check-ec-direct.sh -- the EC DIRECT gate: the emulator's JIT serves a
# proven slot (a COM slot whose arguments are values or proxies, a leaf
# export) inline from guest code, with no dispatcher in the path
# (fexbridge.h "EC DIRECT calls", dlls/ntdll/signal_ppc64.c ec_cell_fill,
# FEXCore DEF_OP(EcTransition)).
#
# Positive leg: probes/ec_direct_probe.c PASSes its value checks (pid and
# tid against the TEB, feature level, context type, a format query, a
# Map/Unmap/Copy/Map round trip through the context), AND the err: banner
# "ec direct arm live" appears -- the arm stamped at least one cell.
#
# --sabotage runs three controls:
#   a  WINE_PPC64LE_EC_DIRECT_SABOTAGE=1: the JIT inverts RAX after every
#      direct-served call.  The probe MUST FAIL -- proof the arm is serving
#      the calls the probe checks, not a path that happens to agree.
#   b  WINE_PPC64LE_NO_EC_DIRECT=1 (the PE half of the kill switch): PASS,
#      no banner, the lever acknowledged.
#   c  FEX_NO_EC_DIRECT=1 (the bridge half): PASS with the banner (cells
#      are stamped, the JIT compiles the plain trampoline) -- proof the
#      bridge alone can refuse the arm.
#
# Environment: WINEPREFIX (booted), WINEFEXBRIDGE (an ABI 7 bridge with
# fexbridge_ec_direct_in_flight), a GPU DXVK can open.  Exit 0 = pass,
# 1 = a check failed, 2 = could not run.
set -u

HERE=$(cd "$(dirname "$0")" && pwd)
SRC=$(cd "$HERE/../.." && pwd)
BUILD=${BUILD:-$SRC}
OUT=${OUT:-/tmp/check-ec-direct}
TIMEOUT=${TIMEOUT:-300}
SABOTAGE=0
[ "${1:-}" = --sabotage ] && SABOTAGE=1
fail=0

say()  { echo "check-ec-direct: $*"; }
bad()  { echo "check-ec-direct: FAIL $*" >&2; fail=1; }
skip() { echo "check-ec-direct: $*" >&2; exit 2; }

[ -x "$BUILD/wine" ] || skip "no wine loader at $BUILD/wine"
[ -n "${WINEPREFIX:-}" ] || skip "set WINEPREFIX"
[ -n "${WINEFEXBRIDGE:-}" ] || skip "set WINEFEXBRIDGE"
command -v clang >/dev/null || skip "need clang"
command -v llvm-dlltool >/dev/null || skip "need llvm-dlltool"
mkdir -p "$OUT"

cat > "$OUT/kernel32.def" <<'EOF'
LIBRARY kernel32.dll
EXPORTS
GetStdHandle
WriteFile
ExitProcess
GetCurrentProcessId
GetCurrentThreadId
EOF
cat > "$OUT/d3d11.def" <<'EOF'
LIBRARY d3d11.dll
EXPORTS
D3D11CreateDevice
EOF
for m in kernel32 d3d11; do
    llvm-dlltool -m i386:x86-64 -d "$OUT/$m.def" -l "$OUT/lib$m.a" 2>"$OUT/build.err" || skip "dlltool $m failed"
done
INCL="-I$BUILD/include -I$SRC/include -I$SRC/include/msvcrt"
clang -target x86_64-windows-gnu -nostdlibinc $INCL -D_UCRT -Wall -O1 -fno-builtin -g \
    -c -o "$OUT/probe.o" "$HERE/probes/ec_direct_probe.c" 2>>"$OUT/build.err" \
    || { sed 's/^/  build| /' "$OUT/build.err" >&2; skip "guest compile failed"; }
clang -target x86_64-windows-gnu -fuse-ld=lld -nostdlib -Wl,--entry=ed_entry -Wl,--subsystem,console \
    -o "$OUT/probe.exe" "$OUT/probe.o" "$OUT/libd3d11.a" "$OUT/libkernel32.a" 2>>"$OUT/build.err" \
    || { sed 's/^/  build| /' "$OUT/build.err" >&2; skip "guest link failed"; }

# err: lines must reach us whatever the caller's WINEDEBUG says; no trace
# channel is added, because +seh would turn the arm off by design.
run_leg() {   # run_leg <name> [ENV=VAL...]
    name=$1; shift
    timeout -k 5 "$TIMEOUT" env "$@" WINEDEBUG="${WINEDEBUG:+$WINEDEBUG,}err+seh" \
        WINEDLLOVERRIDES="winedbg.exe=d" "$BUILD/wine" "$OUT/probe.exe" > "$OUT/$name.out" 2> "$OUT/$name.err"
    rc=$?
    sed "s/^/  $name| /" "$OUT/$name.out"
    return $rc
}
banner() { grep -q 'ec direct arm live' "$OUT/$1.err"; }

if [ "$SABOTAGE" = 0 ]; then
    say "positive leg: values through the direct arm"
    rc=0; run_leg pos || rc=$?
    [ "$rc" = 2 ] && skip "the probe could not create a device (log $OUT/pos.err)"
    [ "$rc" = 0 ] || { tail -10 "$OUT/pos.err" | sed 's/^/  pos| /' >&2; bad "probe exited $rc"; }
    grep -q 'ec_direct_probe: PASS' "$OUT/pos.out" || bad "probe did not PASS"
    banner pos || bad "no 'ec direct arm live' banner: the arm never stamped a cell"
    [ "$fail" = 0 ] && say "PASS"
else
    say "sabotage a: WINE_PPC64LE_EC_DIRECT_SABOTAGE=1 -- RAX inverted on every direct-served call"
    rc=0; run_leg sab WINE_PPC64LE_EC_DIRECT_SABOTAGE=1 || rc=$?
    [ "$rc" = 2 ] && skip "the probe could not create a device (log $OUT/sab.err)"
    grep -q 'SABOTAGE: every direct-served call' "$OUT/sab.err" || bad "sabotage lever unacknowledged"
    if grep -q 'ec_direct_probe: PASS' "$OUT/sab.out"; then
        bad "the probe PASSED under sabotage -- nothing it checks is direct-served"
    else
        say "  red as required: the probe FAILED with RAX inverted"
    fi

    say "sabotage b: WINE_PPC64LE_NO_EC_DIRECT=1 -- the PE half of the kill switch"
    rc=0; run_leg kill WINE_PPC64LE_NO_EC_DIRECT=1 || rc=$?
    [ "$rc" = 0 ] || bad "kill-switch leg exited $rc"
    grep -q 'ec_direct_probe: PASS' "$OUT/kill.out" || bad "kill-switch leg did not PASS"
    grep -q 'WINE_PPC64LE_NO_EC_DIRECT' "$OUT/kill.err" || bad "kill switch unacknowledged"
    banner kill && bad "kill switch left the arm live"

    say "sabotage c: FEX_NO_EC_DIRECT=1 -- the bridge half of the kill switch"
    rc=0; run_leg fexkill FEX_NO_EC_DIRECT=1 || rc=$?
    [ "$rc" = 0 ] || bad "bridge kill leg exited $rc"
    grep -q 'ec_direct_probe: PASS' "$OUT/fexkill.out" || bad "bridge kill leg did not PASS"
    banner fexkill || bad "bridge kill leg: PE side did not stamp (banner missing)"
    # and the sabotage lever must be inert when the bridge refuses the arm
    rc=0; run_leg fexkillsab FEX_NO_EC_DIRECT=1 WINE_PPC64LE_EC_DIRECT_SABOTAGE=1 || rc=$?
    grep -q 'ec_direct_probe: PASS' "$OUT/fexkillsab.out" \
        || bad "with the bridge refusing the arm, the sabotage lever still broke the probe: something else inverts RAX"
    [ "$fail" = 0 ] && say "PASS (controls red and green where required)"
fi
exit $fail
