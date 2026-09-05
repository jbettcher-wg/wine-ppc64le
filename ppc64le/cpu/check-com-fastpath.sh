#!/bin/sh
#
# check-com-fastpath.sh -- the COM fast arm of the trap dispatcher is live,
# answers exact values, can be turned off, and is load-bearing.
#
# What the arm is (dlls/ntdll/signal_ppc64.c, emu_trap_dispatch): a guest
# call into a COM vtable slot whose EC row cell is resolved is served by the
# small fast function -- call_resolved_com_slot straight to the module's
# __wine_com_dispatch, then the lean return -- instead of the full
# dispatcher body with its sixteen-register prologue.  [MEASURED] op4k
# 2026-09-04, Witcher 3 render thread: emu_trap_dispatch_slow was 5.8% of
# the thread and every D3D11 call was paying it.  It rides on EC (it needs
# the per-rip cell cookie), so it needs the ABI 7 bridge, like
# check-ec-leaf.sh.
#
# The probe is the system-COM smoke (ppc64le/syscom/check-com-smoke.sh):
# every line it prints is a value ole32 computed that travelled back through
# a proxy slot -- HRESULTs, refcounts, hashes -- and the guest's output must
# be byte-identical to the native PE's.
#
# Checks:
#   1  LIVE         the guest run prints the "com fast arm live" banner and
#      its output is byte-identical to the native run.
#   2  KILL SWITCH  WINE_PPC64LE_NO_COM_FAST=1: no banner, the same identity
#      holds on the full dispatcher.
#
# --sabotage runs the control that must go red: WINE_PPC64LE_COM_FAST_SABOTAGE=1
# flips RAX on every fast-served slot, so the guest's output must DIVERGE
# from the native run's -- if it does not, no slot was fast-served and check
# 1 was proving nothing.  Then NO_COM_FAST=1 on top must lift it, which
# proves the kill switch reroutes to the full dispatcher rather than
# decorating the arm.
#
# Exit 0 pass, 1 fail, 2 skip (with the reason on stderr).
set -u

HERE=$(cd "$(dirname "$0")" && pwd)
SRC=$(cd "$HERE/../.." && pwd)
BUILD=${BUILD:-$SRC}
SABOTAGE=0
[ "${1:-}" = --sabotage ] && SABOTAGE=1

say()  { echo "check-com-fastpath: $*" >&2; }
bad()  { echo "check-com-fastpath: FAIL $*" >&2; fail=1; }
skip() { echo "check-com-fastpath: $*" >&2; exit 2; }

[ -x "$BUILD/wine" ] || skip "no wine loader at $BUILD/wine"
[ -n "${WINEPREFIX:-}" ] || skip "set WINEPREFIX to a prefix wineboot has run in"
[ -n "${WINEFEXBRIDGE:-}" ] || skip "set WINEFEXBRIDGE to the emulator bridge"

nm -D "$WINEFEXBRIDGE" 2>/dev/null | grep -q fexbridge_register_ec_target \
    || skip "$WINEFEXBRIDGE has no fexbridge_register_ec_target (ABI < 7); no EC cells, no fast arm"

# The probe binaries come from the smoke gate, which also proves the proxy
# runtime itself before this gate asks anything of the arm on top of it.
SMOKE=${SMOKE:-/tmp/com-smoke}
if [ ! -x "$SMOKE/com_smoke_guest.exe" ] || [ ! -f "$SMOKE/native.out" ]; then
    OUT="$SMOKE" BUILD="$BUILD" "$HERE/../syscom/check-com-smoke.sh" >/dev/null 2>&1 \
        || skip "check-com-smoke.sh did not pass; fix that first"
fi
grep -q "com_smoke: PASS" "$SMOKE/native.out" || skip "no passing native run at $SMOKE/native.out"

fail=0
OUT=${OUT:-/tmp/check-com-fastpath}
mkdir -p "$OUT" || skip "cannot create $OUT"
TIMEOUT=${TIMEOUT:-120}

run() { # $1 = tag, rest = env assignments
    tag=$1; shift
    timeout -k 5 "$TIMEOUT" env WINEDEBUG=err+all "$@" \
        "$BUILD/wine" "$SMOKE/com_smoke_guest.exe" > "$OUT/$tag.out" 2> "$OUT/$tag.err"
}

if [ "$SABOTAGE" = 1 ]; then
    # 1: the flipped RAX must make the guest's values diverge from native.
    run sab WINE_PPC64LE_COM_FAST_SABOTAGE=1
    if cmp -s "$SMOKE/native.out" "$OUT/sab.out"; then
        bad "the guest output is STILL byte-identical under WINE_PPC64LE_COM_FAST_SABOTAGE=1: either no slot was fast-served or the arm's RAX is not what the guest reads -- the gate proves nothing"
    else
        say "sabotage: the flipped RAX changed the guest's output (diverges at '$(diff "$SMOKE/native.out" "$OUT/sab.out" | sed -n 2p | cut -c1-60)'), red as required"
    fi
    grep -q "SABOTAGE" "$OUT/sab.err" \
        || bad "no SABOTAGE line on stderr: the lever did not arm, the red above is something else"

    # 2: the kill switch must lift it by rerouting to the full dispatcher.
    run sab2 WINE_PPC64LE_COM_FAST_SABOTAGE=1 WINE_PPC64LE_NO_COM_FAST=1
    if cmp -s "$SMOKE/native.out" "$OUT/sab2.out"; then
        say "sabotage: WINE_PPC64LE_NO_COM_FAST=1 lifted it -- the kill switch reroutes to the full dispatcher"
    else
        diff "$SMOKE/native.out" "$OUT/sab2.out" | head -5 >&2
        bad "NO_COM_FAST did not lift the sabotage (log $OUT/sab2.out); the kill switch is decoration"
    fi
    [ "$fail" = 0 ] && say "SABOTAGE PASS"
    exit $fail
fi

# 1: live -- the banner, and byte identity with the native run.
run live
grep -q "com fast arm live" "$OUT/live.err" \
    || bad "no 'com fast arm live' banner from a run against an ABI 7 bridge (WINE_PPC64LE_NO_COM_FAST set? EC off? wine built without the arm?)"
if cmp -s "$SMOKE/native.out" "$OUT/live.out"; then
    [ "$fail" = 0 ] && say "fast arm live: guest output byte-identical to native ($(wc -l < "$OUT/live.out") lines)"
else
    diff "$SMOKE/native.out" "$OUT/live.out" | head -5 >&2
    bad "guest output differs from native with the fast arm live (log $OUT/live.out)"
fi

# 2: the kill switch removes the banner and everything still works.
run off WINE_PPC64LE_NO_COM_FAST=1
grep -q "com fast arm live" "$OUT/off.err" && bad "WINE_PPC64LE_NO_COM_FAST=1 did not stop the fast arm (banner still printed)"
grep -q "NO_COM_FAST" "$OUT/off.err" || bad "the kill switch did not announce itself on stderr"
if cmp -s "$SMOKE/native.out" "$OUT/off.out"; then
    [ "$fail" = 0 ] && say "kill switch: WINE_PPC64LE_NO_COM_FAST=1 falls back to the full dispatcher, values exact"
else
    diff "$SMOKE/native.out" "$OUT/off.out" | head -5 >&2
    bad "guest output differs from native under WINE_PPC64LE_NO_COM_FAST=1 (log $OUT/off.out)"
fi

[ "$fail" = 0 ] && say "PASS"
exit $fail
