#!/bin/sh
#
# check-d3d11-smoke32.sh -- the i386 (WoW64) half of the D3D11 runtime gate.
#
# check-d3d11-smoke.sh proves an x86-64 guest reaches the same DXVK a
# native process reaches, byte-identically.  This file holds the 32-BIT
# lane to the same standard: the SAME probes/d3d11_smoke.c, built as an
# i386 PE, run under this port's real WoW64 (emu32 + the i386 thunk halves
# + libs/winecom's 32-bit dispatch), must print stdout byte-identical to
# the native ppc64le run's.  Identical output means every stdcall frame was
# decoded and popped right, every proxy the guest saw fit its 4-byte cells,
# every divergent struct was repacked, and Map's host pointer reached the
# guest through a buffer it can actually address.
#
# Legs:
#
#   A  BUILD: the i386 d3d11 thunk half exists and the probe builds as PE32.
#   B  NATIVE: the probe, native ppc64le, headless -- the expected bytes.
#   C  GUEST32: the i386 PE runs under wine and prints PASS.
#   D  IDENTITY: cmp(native stdout, guest32 stdout) is empty.
#   E  BOUNCE: a WINEDEBUG=+d3d11 run of the guest leg must show Map's own
#      map32 trace -- either "BOUNCED" (the host mapping sat above 4 GiB
#      and the guest was served a guest-legal copy) or "guest-legal" (it
#      did not need one).  Either is a correct world; NEITHER means the
#      hand32 walker stopped serving Map and something else answered, which
#      is exactly the silent-truncation hazard this lane refuses.
#
# --sabotage: WINEEMUNOCOMWRAP=1 hands the guest raw host pointers -- 64-bit
# vtable addresses in 4-byte cells here, so the failure is even harder --
# and the run MUST NOT print PASS.
#
# Wine runs disable winedbg for the reason every sibling gate documents:
# AeDebug turns a red state into a hang, and a gate must never hang.
#
# Exit 0 = pass, 1 = a check failed, 2 = could not run at all.
set -u

HERE=$(cd "$(dirname "$0")" && pwd)
SRC=$(cd "$HERE/../.." && pwd)
BUILD=${BUILD:-$SRC}
OUT=${OUT:-/tmp/d3d11-smoke32}
SABOTAGE=0
[ "${1:-}" = "--sabotage" ] && SABOTAGE=1

say()  { echo "check-d3d11-smoke32: $*"; }
bad()  { echo "check-d3d11-smoke32: FAIL $*" >&2; fail=1; }
skip() { echo "check-d3d11-smoke32: $*" >&2; exit 2; }

[ -x "$BUILD/wine" ] || skip "no $BUILD/wine"
[ -f "$BUILD/dlls/d3d11/i386-windows/d3d11.dll" ] || \
    skip "no i386 d3d11 thunk half; configure --enable-archs=ppc64,i386 and build"
[ -n "${WINEPREFIX:-}" ] || skip "set WINEPREFIX to a prefix wineboot has run in"
[ -d "$WINEPREFIX/drive_c/windows/syswow64" ] && \
    [ -n "$(ls -A "$WINEPREFIX/drive_c/windows/syswow64" 2>/dev/null)" ] || \
    skip "$WINEPREFIX has no populated syswow64; the WoW64 lane is not staged"
command -v clang >/dev/null || skip "need clang for the guest build"
command -v llvm-dlltool >/dev/null || skip "need llvm-dlltool for the guest build"

DXVKBUILD="$BUILD/ppc64le/dxvk-build"
D3D11_SO="$DXVKBUILD/src/d3d11/libdxvk_d3d11.so"
[ -e "$D3D11_SO" ] || skip "missing $D3D11_SO -- run ppc64le/dxvk/build-for-wine.sh first"

mkdir -p "$OUT" || skip "cannot create $OUT"

# DXVK writes <appname>_d3d11.log next to the CURRENT DIRECTORY by default, and
# a gate is normally run from the top of the source tree -- so a plain run left
# native_d3d11.log, native_dxgi.log and a wine-preloader_* pair lying in the
# checkout.  Point them at this gate's own work directory instead; DXVK_LOG_PATH
# takes a directory, and "none" would suppress the logs entirely, which is worse
# when a leg fails and the log is the evidence.
DXVK_LOG_PATH=$OUT
export DXVK_LOG_PATH
fail=0
INCL="-I$BUILD/include -I$SRC/include -I$SRC/include/msvcrt"
TIMEOUT=${TIMEOUT:-180}

# ---- A: builds --------------------------------------------------------------
cat > "$OUT/kernel32.def" <<'EOF'
LIBRARY kernel32.dll
EXPORTS
GetStdHandle
WriteFile
ExitProcess
EOF
cat > "$OUT/d3d11.def" <<'EOF'
LIBRARY d3d11.dll
EXPORTS
D3D11CreateDevice
EOF
for m in kernel32 d3d11; do
    llvm-dlltool -m i386 -d "$OUT/$m.def" -l "$OUT/lib${m}32.a" \
        || skip "llvm-dlltool failed for $m"
done
clang -target i386-windows-gnu -nostdlibinc $INCL -Wall -O1 -fno-builtin -g \
    -c -o "$OUT/guest32.o" "$HERE/probes/d3d11_smoke.c" || skip "i386 compile failed"
clang -target i386-windows-gnu -fuse-ld=lld -nostdlib \
    -Wl,--entry=d3d11_smoke_entry -Wl,--subsystem,console -Wl,/safeseh:no \
    -o "$OUT/guest32.exe" "$OUT/guest32.o" "$OUT/libd3d1132.a" "$OUT/libkernel3232.a" \
    || skip "i386 link failed"
say "build: probe built as PE32"

NATIVE_INC_BASE="$HERE/src/include/native"
[ -d "$NATIVE_INC_BASE" ] || skip "no DXVK native headers at $NATIVE_INC_BASE"
NATIVE_INC="-I$NATIVE_INC_BASE -I$NATIVE_INC_BASE/windows -I$NATIVE_INC_BASE/directx"
${CC:-gcc} -std=c11 -O2 -mcpu=power8 $NATIVE_INC -Wall -fno-builtin \
    -DD3D11_SMOKE_NATIVE -c -o "$OUT/native.o" "$HERE/probes/d3d11_smoke.c" \
    || skip "native compile failed"
${CC:-gcc} -o "$OUT/native" "$OUT/native.o" -ldl || skip "native link failed"

run_wine() { timeout -k 5 "$TIMEOUT" \
                 env WINEDEBUG="${2:--all}" WINEDLLOVERRIDES="winedbg.exe=d" \
                 "$BUILD/wine" "$1"; }

# ---- sabotage ---------------------------------------------------------------
if [ "$SABOTAGE" = 1 ]; then
    timeout -k 5 "$TIMEOUT" \
        env WINEDEBUG=-all WINEDLLOVERRIDES="winedbg.exe=d" WINEEMUNOCOMWRAP=1 \
        "$BUILD/wine" "$OUT/guest32.exe" \
        > "$OUT/sab.out" 2>"$OUT/sab.err"
    if grep -q "d3d11_smoke: PASS" "$OUT/sab.out"; then
        bad "WINEEMUNOCOMWRAP=1 still PASSED -- the gate cannot go red"
    else
        say "sabotage: WINEEMUNOCOMWRAP=1 failed the guest32 run at '$(tail -1 \
            "$OUT/sab.out" | cut -c1-60)', as it must"
    fi
    [ "$fail" = 0 ] && { say PASS; exit 0; }
    exit 1
fi

# ---- B: native --------------------------------------------------------------
env -u DISPLAY -u WAYLAND_DISPLAY -u XDG_RUNTIME_DIR DXVK_WSI_DRIVER=Headless \
    timeout -k 5 "$TIMEOUT" "$OUT/native" "$D3D11_SO" \
    > "$OUT/native.out" 2>"$OUT/native.err"
rc=$?
[ $rc = 0 ] && grep -q "d3d11_smoke: PASS" "$OUT/native.out" || {
    sed 's/^/  native| /' "$OUT/native.out" >&2
    skip "the native leg did not pass (rc=$rc); nothing to hold the guest to"
}
say "native: $(tail -1 "$OUT/native.out")"

# ---- C: guest32 -------------------------------------------------------------
run_wine "$OUT/guest32.exe" > "$OUT/guest32.out" 2>"$OUT/guest32.err"
rc=$?
if [ $rc != 0 ] || ! grep -q "d3d11_smoke: PASS" "$OUT/guest32.out"; then
    sed 's/^/  guest32| /' "$OUT/guest32.out" >&2
    tail -5 "$OUT/guest32.err" | sed 's/^/  guest32-err| /' >&2
    bad "the i386 guest leg did not pass (rc=$rc)"
else
    say "guest32: $(tail -1 "$OUT/guest32.out")"
fi

# ---- D: identity ------------------------------------------------------------
if cmp -s "$OUT/native.out" "$OUT/guest32.out"; then
    say "identity: native and i386 guest stdout are byte-identical"
else
    diff "$OUT/native.out" "$OUT/guest32.out" | head -12 | sed 's/^/  diff| /' >&2
    bad "the two runs printed different bytes"
fi

# ---- E: the Map serve path is the hand32 walker ------------------------------
run_wine "$OUT/guest32.exe" +d3d11 > "$OUT/trace.out" 2>"$OUT/trace.err"
if grep -q "map32: .*BOUNCED" "$OUT/trace.err"; then
    say "bounce: $(grep -m1 -o 'map32: .*' "$OUT/trace.err" | cut -c1-90)"
elif grep -q "map32: .*guest-legal" "$OUT/trace.err"; then
    say "bounce: the host mapping was already guest-legal (no copy needed)"
else
    bad "no map32 trace at all -- ID3D11DeviceContext::Map is not being served \
by its 32-bit walker, which risks a silently truncated pData"
fi

[ "$fail" = 0 ] && { say PASS; exit 0; }
exit 1
