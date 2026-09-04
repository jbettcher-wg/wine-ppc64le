#!/bin/sh
#
# check-ec-leaf.sh -- the EC leaf path is live, answers exact values, can be
# turned off, and is load-bearing.
#
# What the leaf path is (dlls/ntdll/signal_ppc64.c, thunk_leaf_exports;
# unix/loader.c, emu_trap_dispatch_common): a transitioned call whose row
# names an export that cannot syscall, raise, or call back is served by an
# ordinary call into PE code on the kernel stack -- no call_user_mode_
# callback frame, no Win32-stack switch, no PE dispatcher frame, no lean
# return.  It rides on EC (it needs the per-rip cell cookie), so it needs
# the ABI 7 bridge and the view, like check-ec-transition.sh.
#
# Checks:
#   1  LIVE         a guest run prints the "ec leaf path live" banner and the
#      probe (probes/ec_leaf.c) passes every value check: pid/tid stable
#      over 2000 calls, SetLastError/GetLastError round trips, TLS round
#      trips against a non-leaf setter, GetTickCount monotone.
#   2  KILL SWITCH  WINE_PPC64LE_NO_EC_LEAF=1: no banner, the same checks
#      pass on the callback frame.
#
# --sabotage runs the control that must go red: WINE_PPC64LE_EC_LEAF_SABOTAGE=1
# flips RAX on every leaf-served call, so the probe's pid/tid/last-error/tls
# checks must FAIL -- if they pass, no call was leaf-served and check 1 was
# proving nothing.  Then NO_EC_LEAF=1 on top must lift it, which proves the
# kill switch reroutes to the frame rather than decorating the path.
#
# Exit 0 pass, 1 fail, 2 skip (with the reason on stderr).
set -u

HERE=$(cd "$(dirname "$0")" && pwd)
SRC=$(cd "$HERE/../.." && pwd)
BUILD=${BUILD:-$SRC}
SABOTAGE=0
[ "${1:-}" = --sabotage ] && SABOTAGE=1

say()  { echo "check-ec-leaf: $*" >&2; }
bad()  { echo "check-ec-leaf: FAIL $*" >&2; fail=1; }
skip() { echo "check-ec-leaf: $*" >&2; exit 2; }

[ -x "$BUILD/wine" ] || skip "no wine loader at $BUILD/wine"
[ -n "${WINEPREFIX:-}" ] || skip "set WINEPREFIX to a prefix wineboot has run in"
[ -n "${WINEFEXBRIDGE:-}" ] || skip "set WINEFEXBRIDGE to the emulator bridge"
command -v clang >/dev/null || skip "need clang for the guest probe"
command -v llvm-dlltool >/dev/null || skip "need llvm-dlltool for the guest probe"

nm -D "$WINEFEXBRIDGE" 2>/dev/null | grep -q fexbridge_set_trap_view_handler \
    || skip "$WINEFEXBRIDGE has no view protocol (ABI < 6); the leaf path rides on EC, nothing to check"
nm -D "$WINEFEXBRIDGE" 2>/dev/null | grep -q fexbridge_register_ec_target \
    || skip "$WINEFEXBRIDGE has no fexbridge_register_ec_target (ABI < 7); no EC, no leaf path"

# err: lines must reach us whatever the caller's WINEDEBUG says.
export WINEDEBUG="${WINEDEBUG:+$WINEDEBUG,}err+all"
WINEDLLOVERRIDES=winedbg.exe=d; export WINEDLLOVERRIDES

fail=0
OUT=${OUT:-/tmp/check-ec-leaf}
mkdir -p "$OUT" || skip "cannot create $OUT"

cat > "$OUT/kernel32.def" <<'DEF'
LIBRARY kernel32.dll
EXPORTS
GetCurrentProcessId
GetCurrentThreadId
GetLastError
SetLastError
TlsAlloc
TlsSetValue
TlsGetValue
GetTickCount
GetStdHandle
WriteFile
ExitProcess
DEF
llvm-dlltool -m i386:x86-64 -d "$OUT/kernel32.def" -l "$OUT/libkernel32.a" \
    || skip "llvm-dlltool failed"
clang -target x86_64-windows-gnu -nostdlib -O1 -fuse-ld=lld \
    -Wl,--entry=mainCRTStartup -Wl,--subsystem,console \
    -o "$OUT/ec_leaf.exe" "$HERE/probes/ec_leaf.c" "$OUT/libkernel32.a" || skip "guest build failed"

run() { # $1 = tag, rest = env assignments
    tag=$1; shift
    env "$@" "$BUILD/wine" "$OUT/ec_leaf.exe" > "$OUT/$tag.out" 2> "$OUT/$tag.err"
}
show() { sed -n "s/^/check-ec-leaf:   /p" "$OUT/$1.out" | grep -E "PASS|FAIL" >&2; }

if [ "$SABOTAGE" = 1 ]; then
    # 1: the flipped RAX must make the value checks fail.
    if run sab WINE_PPC64LE_EC_LEAF_SABOTAGE=1; then
        bad "the probe PASSED under WINE_PPC64LE_EC_LEAF_SABOTAGE=1: either no call was leaf-served or the leaf's RAX is not what the guest reads -- the gate proves nothing"
    else
        grep -q "^FAIL pid stable" "$OUT/sab.out" \
            || bad "the pid check did not go red under sabotage (log $OUT/sab.out)"
        grep -q "^FAIL last-error round trip" "$OUT/sab.out" \
            || bad "the last-error check did not go red under sabotage (log $OUT/sab.out)"
        [ "$fail" = 0 ] && say "sabotage: the flipped RAX broke pid and last-error, red as required"
    fi
    grep -q "SABOTAGE" "$OUT/sab.err" \
        || bad "no SABOTAGE line on stderr: the lever did not arm, the red above is something else"

    # 2: the kill switch must lift it by rerouting to the callback frame.
    if run sab2 WINE_PPC64LE_EC_LEAF_SABOTAGE=1 WINE_PPC64LE_NO_EC_LEAF=1 \
       && grep -q "^DONE ec-leaf" "$OUT/sab2.out" && ! grep -q "^FAIL" "$OUT/sab2.out"; then
        say "sabotage: WINE_PPC64LE_NO_EC_LEAF=1 lifted it -- the kill switch reroutes to the frame"
    else
        show sab2
        bad "NO_EC_LEAF did not lift the sabotage (log $OUT/sab2.out); the kill switch is decoration"
    fi
    [ "$fail" = 0 ] && say "SABOTAGE PASS"
    exit $fail
fi

# 1: live -- the banner, and every value check green.
if run live; then :; else bad "probe exited non-zero with the leaf path live (log $OUT/live.out)"; fi
show live
grep -q "ec targets live" "$OUT/live.err" \
    || bad "no 'ec targets live' banner (WINE_PPC64LE_NO_EC set? view off? see check-ec-transition.sh)"
grep -q "ec leaf path live" "$OUT/live.err" \
    || bad "no 'ec leaf path live' banner from a run against an ABI 7 bridge (WINE_PPC64LE_NO_EC_LEAF set? wine built without the wiring?)"
grep -q "^DONE ec-leaf" "$OUT/live.out" || bad "the probe did not run to its end"
grep -q "^FAIL" "$OUT/live.out" && bad "a value check failed with the leaf path live"
[ "$fail" = 0 ] && say "leaf path live: every value check exact"

# 2: the kill switch removes the banner and everything still works.
if run off WINE_PPC64LE_NO_EC_LEAF=1; then
    grep -q "ec leaf path live" "$OUT/off.err" && bad "WINE_PPC64LE_NO_EC_LEAF=1 did not stop the leaf path (banner still printed)"
    grep -q "NO_EC_LEAF" "$OUT/off.err" || bad "the kill switch did not announce itself on stderr"
    grep -q "^FAIL" "$OUT/off.out" && bad "a value check failed with the leaf path off"
    [ "$fail" = 0 ] && say "kill switch: WINE_PPC64LE_NO_EC_LEAF=1 falls back to the callback frame, values exact"
else
    bad "probe run failed under WINE_PPC64LE_NO_EC_LEAF=1 (log $OUT/off.out)"
fi

[ "$fail" = 0 ] && say "PASS"
exit $fail
