#!/bin/sh
#
# check-ec-transition.sh -- the bridge ABI 7 EC transitions really fire, can
# be turned off, and their stub simulation is load-bearing.
#
# What EC is (ppc64le/docs/ppc64ec.md step B): the first trap into a thunk
# module registers its byte-verified stub RIPs with the bridge, and every
# LATER call to those stubs compiles to a direct host call instead of the
# stub's `mov r10,rcx; syscall` trap.  The transition fires at the stub BASE
# with nothing executed, so the wine handler must simulate the rescue
# (R10 = RCX) and the advance to the trap site (+3) itself.
#
# Checks:
#   1  EC LIVE     a guest run against an ABI 7 bridge prints BOTH banners
#      ("trap view live", "ec targets live") and a probe whose thunk calls
#      carry real arguments prints the exact expected line.  The probe
#      calls each import repeatedly, so its later calls are transitions,
#      not first traps.
#   2  KILL SWITCH WINE_PPC64LE_NO_EC=1: no ec banner, probe output still
#      exact -- every stub back on the trap protocol.
#
# --sabotage runs the control that must go red: WINE_PPC64LE_EC_SABOTAGE=1
# stops the handler simulating the stub's mov r10,rcx, so a transitioned
# call reads garbage for argument 0.  The probe's output line must NOT
# survive that -- if it does, the transitions are not live and check 1 was
# proving nothing.
#
# Exit 0 pass, 1 fail, 2 skip (with the reason on stderr).
set -u

HERE=$(cd "$(dirname "$0")" && pwd)
SRC=$(cd "$HERE/../.." && pwd)
BUILD=${BUILD:-$SRC}
SABOTAGE=0
[ "${1:-}" = --sabotage ] && SABOTAGE=1

say()  { echo "check-ec-transition: $*" >&2; }
bad()  { echo "check-ec-transition: FAIL $*" >&2; fail=1; }
skip() { echo "check-ec-transition: $*" >&2; exit 2; }

[ -x "$BUILD/wine" ] || skip "no wine loader at $BUILD/wine"
[ -n "${WINEPREFIX:-}" ] || skip "set WINEPREFIX to a prefix wineboot has run in"
[ -n "${WINEFEXBRIDGE:-}" ] || skip "set WINEFEXBRIDGE to the emulator bridge"
command -v clang >/dev/null || skip "need clang for the guest probe"
command -v llvm-dlltool >/dev/null || skip "need llvm-dlltool for the guest probe"

nm -D "$WINEFEXBRIDGE" 2>/dev/null | grep -q fexbridge_set_trap_view_handler \
    || skip "$WINEFEXBRIDGE has no view protocol (ABI < 6); EC rides on the view, nothing to check"
nm -D "$WINEFEXBRIDGE" 2>/dev/null | grep -q fexbridge_register_ec_target \
    || skip "$WINEFEXBRIDGE has no fexbridge_register_ec_target (ABI < 7); EC targets do not exist here"

# err: lines must reach us whatever the caller's WINEDEBUG says.
export WINEDEBUG="${WINEDEBUG:+$WINEDEBUG,}err+all"

fail=0
OUT=${OUT:-/tmp/check-ec-transition}
mkdir -p "$OUT" || skip "cannot create $OUT"

# The probe: freestanding x86-64 guest whose thunk calls CARRY ARGUMENTS
# (handle, name pointer, buffer pointer, size), looped so the later
# iterations run through transitions.  It reads WINEECPROBE from the
# environment and prints one exact line; a mis-simulated argument 0 cannot
# produce it.
cat > "$OUT/ec_probe.c" <<'EOF'
typedef unsigned int u32;
void *__stdcall GetStdHandle( u32 which );
int __stdcall WriteFile( void *h, const void *buf, u32 len, u32 *written, void *ov );
u32 __stdcall GetEnvironmentVariableA( const char *name, char *buf, u32 size );
u32 __stdcall GetCurrentProcessId( void );
void __stdcall ExitProcess( u32 code );

void mainCRTStartup( void )
{
    char buf[64], line[96];
    u32 i, n = 0, w, len = 0;

    /* every import at least three times: the first trap into each module
     * arms EC for the WHOLE module, so iterations after the first are
     * transitions when EC is live */
    for (i = 0; i < 8; i++)
        n = GetEnvironmentVariableA( "WINEECPROBE", buf, sizeof(buf) );
    if (!GetCurrentProcessId()) n = 0;

    line[len++] = 'E'; line[len++] = 'C'; line[len++] = '-'; line[len++] = 'O';
    line[len++] = 'K'; line[len++] = ':';
    for (i = 0; i < n && len < sizeof(line) - 2; i++) line[len++] = buf[i];
    line[len++] = '\n';
    for (i = 0; i < 4; i++)
        WriteFile( GetStdHandle( (u32)-11 ), line, len, &w, 0 );
    ExitProcess( 0 );
}
EOF
cat > "$OUT/kernel32.def" <<'EOF'
LIBRARY kernel32.dll
EXPORTS
GetStdHandle
WriteFile
GetEnvironmentVariableA
GetCurrentProcessId
ExitProcess
EOF
llvm-dlltool -m i386:x86-64 -d "$OUT/kernel32.def" -l "$OUT/libkernel32.a" \
    || skip "llvm-dlltool failed"
clang -target x86_64-windows-gnu -nostdlib -fuse-ld=lld \
    -Wl,--entry=mainCRTStartup -Wl,--subsystem,console \
    -o "$OUT/ec_probe.exe" "$OUT/ec_probe.c" "$OUT/libkernel32.a" || skip "guest build failed"

TOKEN="ec-gate-$$"
WANT="EC-OK:$TOKEN"
export WINEECPROBE="$TOKEN"

if [ "$SABOTAGE" = 1 ]; then
    if ! env WINE_PPC64LE_EC_SABOTAGE=1 "$BUILD/wine" "$OUT/ec_probe.exe" \
        > "$OUT/probe.out" 2> "$OUT/probe.err"; then
        say "sabotage: the mis-simulated argument 0 killed the probe outright (rc != 0), red as required"
    elif grep -q "$WANT" "$OUT/probe.out"; then
        bad "WINE_PPC64LE_EC_SABOTAGE=1 left the probe's output EXACT: either no call transitioned or the rescue simulation is not load-bearing -- check 1 proves nothing"
    else
        say "sabotage: the probe's output did not survive the mis-simulated argument 0, red as required"
    fi
    # and the lever must announce itself, or a typo'd env var fakes a pass
    grep -q "SABOTAGE" "$OUT/probe.err" \
        || bad "no SABOTAGE line on stderr: the lever did not arm, the red above is something else"
    [ "$fail" = 0 ] && say "SABOTAGE PASS"
    exit $fail
fi

# 1: EC live -- both banners, exact output.
"$BUILD/wine" "$OUT/ec_probe.exe" > "$OUT/probe.out" 2> "$OUT/probe.err" \
    || bad "probe run failed (rc != 0) with EC live"
grep -q "trap view live" "$OUT/probe.err" || bad "no 'trap view live' banner (view off -- EC cannot be on either)"
grep -q "ec targets live" "$OUT/probe.err" || bad "no 'ec targets live' banner from a guest run against an ABI 7 bridge (WINE_PPC64LE_NO_EC set? wine built without the wiring?)"
grep -q "$WANT" "$OUT/probe.out" || bad "probe output wrong with EC live: wanted '$WANT', got: $(head -c 200 "$OUT/probe.out" | tr -d '\0')"
[ "$fail" = 0 ] && say "ec targets live: transitions serve real argument-carrying calls exactly"

# 2: the kill switch removes the ec banner and everything still works.
if env WINE_PPC64LE_NO_EC=1 "$BUILD/wine" "$OUT/ec_probe.exe" > "$OUT/probe2.out" 2> "$OUT/probe2.err"; then
    grep -q "ec targets live" "$OUT/probe2.err" && bad "WINE_PPC64LE_NO_EC=1 did not stop the registrations"
    grep -q "$WANT" "$OUT/probe2.out" || bad "probe output wrong with EC off: the trap fallback is broken"
    [ "$fail" = 0 ] && say "kill switch: WINE_PPC64LE_NO_EC=1 falls back to traps, values exact"
else
    bad "probe run failed under WINE_PPC64LE_NO_EC=1"
fi

[ "$fail" = 0 ] && say "PASS"
exit $fail
