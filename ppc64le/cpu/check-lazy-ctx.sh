#!/bin/sh
#
# check-lazy-ctx.sh -- every consumer of a trap CONTEXT's EFLAGS/FP state
# materializes it before looking.
#
# WHAT THIS IS GUARDING.  Bridge ABI 5 (fex-src 8a4b975fa + this tree's
# consumer wiring): the trap path stops building the EFLAGS word and the
# XMM/x87 block on every crossing -- ~4-5% of the Cyberpunk GameThread was
# that reconstruction for hops that read neither -- and any consumer that DOES
# read or write those groups must call the materialize entry first
# (emu_ctx_materialize_full unix-side, __wine_emu_materialize_ctx for PE hand
# walkers; the audited list is at materialize_trap_ctx in
# dlls/ntdll/signal_ppc64.c).
#
# The failure mode of a missed consumer is not a crash: it is an FP argument
# read as garbage, a fiber resumed with the wrong MxCsr, physics subtly wrong.
# So the check is value-based, and it borrows the strictest FP value-checker
# in the tree: ppc64le/opengl/check-gl-smoke.sh, which drives doubles and
# floats through registers AND the stack and reads GL's own state back.
#
#   1  DECLARED   a live launch's log carries the bridge's "lazy trap contexts
#      live: EFLAGS FP" line, so the lazy path is actually on -- without this
#      the gate would pass against an eager world and prove nothing.
#   2  POISONED VALUES  check-gl-smoke.sh passes with FEXBRIDGE_CTX_POISON=1.
#      The bridge fills every unmaterialized EFLAGS/FP group with 0xDEADF1A6 /
#      0xDD bytes, so this passing means every consumer on the path REALLY
#      materialized -- correct values cannot come out of poison by luck.
#      (Under the ABI 6 view protocol there is no bridge CONTEXT to poison;
#      emu_trap_view_thunk writes the same patterns into its shell when the
#      lever is armed, so this check keeps meaning the same thing there.)
#
# Bridge ABI 6 adds the zero-copy trap view (ppc64le/docs/ppc64ec.md step A),
# and the gate grows three legs when the bridge has it, SKIP-with-a-reason
# when it does not:
#   3  VIEW LIVE   a guest launch's stderr carries wine's "trap view live"
#      line -- the view registration actually happened.
#   4  KILL SWITCH WINE_PPC64LE_NO_TRAP_VIEW=1: no view line, and the FP
#      value gate stays green on the CONTEXT protocol.
#   5  BRIDGE VETO FEXBRIDGE_EAGER_CTX=1: the bridge refuses lazy AND view
#      (fully eager world); no view line, no lazy line, values green.
#      With the view live by default, checks 2 and the sabotage pair run
#      against the VIEW protocol -- which is the point.
#
# --sabotage runs the pair that must go red:
#   FEXBRIDGE_CTX_POISON=1 + WINEEMUNOCTXMAT=1 -- consumers stop
#      materializing (the "forgot the contract" world) while the poison is
#      armed; check-gl-smoke.sh must FAIL on values (the 0xDD pattern lands in
#      GL state).  This falsifies the consumer audit itself.
#
# Exit 0 = pass, 1 = a check failed, 2 = could not run (not a pass).
set -u

HERE=$(cd "$(dirname "$0")" && pwd)
SRC=$(cd "$HERE/../.." && pwd)
BUILD=${BUILD:-$SRC}
SABOTAGE=0
[ "${1:-}" = "--sabotage" ] && SABOTAGE=1

say()  { echo "check-lazy-ctx: $*"; }
bad()  { echo "check-lazy-ctx: FAIL $*" >&2; fail=1; }
skip() { echo "check-lazy-ctx: $*" >&2; exit 2; }

[ -x "$BUILD/wine" ] || skip "no wine loader at $BUILD/wine"
[ -n "${WINEPREFIX:-}" ] || skip "set WINEPREFIX to a prefix wineboot has run in"
[ -n "${WINEFEXBRIDGE:-}" ] || skip "set WINEFEXBRIDGE to the emulator bridge"
[ -x "$HERE/../opengl/check-gl-smoke.sh" ] || skip "no check-gl-smoke.sh to borrow"
command -v clang >/dev/null || skip "need clang for the guest probe"
command -v llvm-dlltool >/dev/null || skip "need llvm-dlltool for the guest probe"

# An ABI-4 bridge has no lazy path: nothing to check, nothing to sabotage.
# Objdump over dlsym: no process launch needed to answer "does the bridge
# export the declaration".
if ! nm -D "$WINEFEXBRIDGE" 2>/dev/null | grep -q fexbridge_declare_trap_ctx; then
    skip "$WINEFEXBRIDGE has no fexbridge_declare_trap_ctx (ABI < 5); the lazy path does not exist here"
fi

fail=0

if [ "$SABOTAGE" = 1 ]; then
    # The consumer audit must be falsifiable: poison armed, materialize off,
    # the FP value gate must go red.
    if FEXBRIDGE_CTX_POISON=1 WINEEMUNOCTXMAT=1 "$HERE/../opengl/check-gl-smoke.sh" \
        > /tmp/check-lazy-ctx-sab.log 2>&1; then
        bad "FEXBRIDGE_CTX_POISON=1 + WINEEMUNOCTXMAT=1 left check-gl-smoke GREEN: either the lazy path is not live or poison never reaches a consumer -- the positive check proves nothing"
    else
        say "sabotage: poison + no-materialize made the FP value gate go red, as it must"
    fi
    [ "$fail" = 0 ] && say "SABOTAGE PASS"
    exit $fail
fi

# 1: the declaration is live in a real launch.  The declaration happens when
# the first GUEST run installs the trap dispatcher, so the probe must BE a
# guest -- `wine cmd` runs a ppc64 builtin and proves nothing (measured: no
# line).  A minimal x86-64 exe that exits is enough; the bridge prints its
# "lazy trap contexts live" banner on stderr the moment the guest starts.
OUT=${OUT:-/tmp/check-lazy-ctx}
mkdir -p "$OUT" || skip "cannot create $OUT"
cat > "$OUT/hello.c" <<'EOF'
void __stdcall ExitProcess( unsigned int code );
void mainCRTStartup( void ) { ExitProcess( 0 ); }
EOF
cat > "$OUT/kernel32.def" <<'EOF'
LIBRARY kernel32.dll
EXPORTS
ExitProcess
EOF
llvm-dlltool -m i386:x86-64 -d "$OUT/kernel32.def" -l "$OUT/libkernel32.a" \
    || skip "llvm-dlltool failed"
clang -target x86_64-windows-gnu -nostdlib -fuse-ld=lld \
    -Wl,--entry=mainCRTStartup -Wl,--subsystem,console \
    -o "$OUT/hello.exe" "$OUT/hello.c" "$OUT/libkernel32.a" || skip "guest build failed"
if ! "$BUILD/wine" "$OUT/hello.exe" 2>&1 | grep -q "lazy trap contexts live"; then
    bad "no 'lazy trap contexts live' line from a guest launch: the declaration did not happen (old bridge? WINEEMUNOLAZYCTX set? wine built without the wiring?)"
else
    say "the lazy declaration is live (bridge accepted EFLAGS+FP)"
fi

# 3-5: the ABI 6 view protocol, when this bridge has it.
if nm -D "$WINEFEXBRIDGE" 2>/dev/null | grep -q fexbridge_set_trap_view_handler; then
    # 3: the view registration is live in a real launch (same probe, same
    # reasoning as check 1: the line prints when a guest run installs it).
    if ! "$BUILD/wine" "$OUT/hello.exe" 2>&1 | grep -q "trap view live"; then
        bad "no 'trap view live' line from a guest launch against an ABI 6 bridge: the view registration did not happen (WINE_PPC64LE_NO_TRAP_VIEW set? wine built without the wiring?)"
    else
        say "the trap view is live (zero-copy crossings)"
    fi
    # 4: this side's kill switch lands on the CONTEXT protocol, values intact.
    if WINE_PPC64LE_NO_TRAP_VIEW=1 "$BUILD/wine" "$OUT/hello.exe" 2>&1 | grep -q "trap view live"; then
        bad "WINE_PPC64LE_NO_TRAP_VIEW=1 did not stop the view registration"
    elif ! WINE_PPC64LE_NO_TRAP_VIEW=1 "$HERE/../opengl/check-gl-smoke.sh" \
        > /tmp/check-lazy-ctx-noview.log 2>&1; then
        bad "check-gl-smoke FAILED with the view off: the CONTEXT protocol fallback is broken (log /tmp/check-lazy-ctx-noview.log)"
    else
        say "kill switch: WINE_PPC64LE_NO_TRAP_VIEW=1 falls back to the CONTEXT protocol, values green"
    fi
    # 5: the bridge's veto forces the fully-eager world end to end.
    if FEXBRIDGE_EAGER_CTX=1 "$BUILD/wine" "$OUT/hello.exe" 2>&1 | grep -qE "trap view live|lazy trap contexts live"; then
        bad "FEXBRIDGE_EAGER_CTX=1 left the view or the lazy declaration live"
    elif ! FEXBRIDGE_EAGER_CTX=1 "$HERE/../opengl/check-gl-smoke.sh" \
        > /tmp/check-lazy-ctx-eager.log 2>&1; then
        bad "check-gl-smoke FAILED under FEXBRIDGE_EAGER_CTX=1: the eager world is broken (log /tmp/check-lazy-ctx-eager.log)"
    else
        say "bridge veto: FEXBRIDGE_EAGER_CTX=1 runs fully eager, values green"
    fi
else
    say "SKIP view legs: $WINEFEXBRIDGE has no fexbridge_set_trap_view_handler (bridge ABI < 6); checks 3-5 not run" >&2
fi

# 2: every consumer materializes -- poisoned unmaterialized state, real values.
if FEXBRIDGE_CTX_POISON=1 "$HERE/../opengl/check-gl-smoke.sh" \
    > /tmp/check-lazy-ctx-poison.log 2>&1; then
    say "check-gl-smoke under FEXBRIDGE_CTX_POISON=1: PASS -- FP consumers materialize before reading"
else
    bad "check-gl-smoke FAILED under FEXBRIDGE_CTX_POISON=1: a consumer read or wrote EFLAGS/FP without materializing (log /tmp/check-lazy-ctx-poison.log)"
fi

[ "$fail" = 0 ] && say "PASS"
exit $fail
