#!/bin/sh
#
# check-peek-fastpath.sh -- the guest's PeekMessageW fast body answers the
# empty null-filter poll from the thread's queue_shm, and NOTHING ELSE.
#
# WHAT THIS IS GUARDING.  [MEASURED] 2026-08-27, Cyberpunk -benchmark:
# PeekMessageW was 100,526 crossings a second, each a full trap AND a full
# syscall, the top flat row once QPC fell -- and nearly every one an empty
# render-loop poll.  The fast body (tools/spec2thunk, kind 'peek') performs
# check_queue_bits' own shared-memory test guest-side and returns FALSE with
# zero crossings when the server's wake bits show nothing; anything else --
# a live bit, a filter, an unseeded thread, a moving seqlock, the 256-poll
# trap budget, a new 16ms tick -- is the trap it always was.
#
# The danger is a poll that answers "empty" while the bits say otherwise: a
# swallowed or starved message.  The probe (probes/peek_fastpath.c) proves
# the body is present, seeded, executing and fast, and then that a posted
# message still arrives on the very next poll.
#
# --sabotage arms WINE_PPC64LE_PEEK_SABOTAGE=1: win32u seeds the queue
# pointer TAGGED, the fast body answers every null-filter poll empty without
# reading the bits, and a posted message must STARVE -- which proves the
# guest path, not the trap, is what answers.  Then
# WINE_PPC64LE_NO_PEEK_BYPASS=1 on top must lift exactly that starvation,
# which proves the kill switch reroutes to the trap rather than decorating.
#
# Exit 0 = pass, 1 = a check failed, 2 = could not run (not a pass).
set -u

HERE=$(cd "$(dirname "$0")" && pwd)
SRC=$(cd "$HERE/../.." && pwd)
BUILD=${BUILD:-$SRC}
OUT=${OUT:-/tmp/check-peek-fastpath}
SABOTAGE=0
[ "${1:-}" = "--sabotage" ] && SABOTAGE=1

say()  { echo "check-peek-fastpath: $*"; }
bad()  { echo "check-peek-fastpath: FAIL $*" >&2; fail=1; }
skip() { echo "check-peek-fastpath: $*" >&2; exit 2; }

[ -x "$BUILD/wine" ] || skip "no wine loader at $BUILD/wine"
[ -n "${WINEPREFIX:-}" ] || skip "set WINEPREFIX to a prefix wineboot has run in"
[ -n "${WINEFEXBRIDGE:-}" ] || skip "set WINEFEXBRIDGE to the emulator bridge"
command -v clang >/dev/null || skip "need clang for the guest build"
command -v llvm-dlltool >/dev/null || skip "need llvm-dlltool for the guest build"

mkdir -p "$OUT" || skip "cannot create $OUT"
fail=0

# ---- build the guest probe ------------------------------------------------
INCL="-I$BUILD/include -I$SRC/include -I$SRC/include/msvcrt"

cat > "$OUT/kernel32.def" <<'EOF'
LIBRARY kernel32.dll
EXPORTS
GetStdHandle
WriteFile
ExitProcess
GetModuleHandleA
GetProcAddress
GetCurrentThreadId
GetTickCount
QueryPerformanceCounter
lstrlenA
EOF
cat > "$OUT/user32.def" <<'EOF'
LIBRARY user32.dll
EXPORTS
PeekMessageW
PostThreadMessageW
wsprintfA
EOF
for m in kernel32 user32; do
    llvm-dlltool -m i386:x86-64 -d "$OUT/$m.def" -l "$OUT/lib$m.a" \
        || skip "llvm-dlltool failed for $m"
done

clang -target x86_64-windows-gnu -nostdlibinc $INCL -D_UCRT -Wall -O1 -fno-builtin -g \
    -c -o "$OUT/peek.o" "$HERE/probes/peek_fastpath.c" || skip "guest compile failed"
clang -target x86_64-windows-gnu -fuse-ld=lld -nostdlib \
    -Wl,--entry=mainCRTStartup -Wl,--subsystem,console \
    -o "$OUT/peek.exe" "$OUT/peek.o" "$OUT/libkernel32.a" "$OUT/libuser32.a" \
    || skip "guest link failed"

WINEDLLOVERRIDES=winedbg.exe=d; export WINEDLLOVERRIDES

if [ "$SABOTAGE" = 0 ]; then
    timeout 120 "$BUILD/wine" "$OUT/peek.exe" > "$OUT/pos.log" 2>&1
    rc=$?
    sed -n 's/^/check-peek-fastpath:   /p' "$OUT/pos.log" | grep -E "PASS|FAIL"
    [ "$rc" = 0 ] || bad "probe exited $rc (log $OUT/pos.log)"
    grep -q "^PASS mechanism" "$OUT/pos.log" || bad "the mechanism layer did not pass"
    grep -q "^PASS delivery" "$OUT/pos.log" || bad "the delivery layer did not pass"

    # The kill switch is not decoration: with it armed, messages still flow
    # (through the trap, since the pointer is never seeded).
    WINE_PPC64LE_NO_PEEK_BYPASS=1 timeout 60 "$BUILD/wine" "$OUT/peek.exe" starve \
        > "$OUT/kill.log" 2>&1
    if grep -q "^GOT" "$OUT/kill.log"; then
        say "kill switch: messages flow with WINE_PPC64LE_NO_PEEK_BYPASS=1"
    else
        bad "no delivery under NO_PEEK_BYPASS (log $OUT/kill.log) -- the trap path itself is broken"
    fi

    [ "$fail" = 0 ] && say "PASS"
    exit $fail
fi

# ---- sabotage -------------------------------------------------------------
# 1: with the seeded pointer tagged, the fast body answers empty over LIVE
#    wake bits and the posted message must starve -- the proof that the
#    answer really comes from guest user space.
WINE_PPC64LE_PEEK_SABOTAGE=1 timeout 60 "$BUILD/wine" "$OUT/peek.exe" starve \
    > "$OUT/sab.log" 2>&1
if grep -q "^STARVED" "$OUT/sab.log"; then
    say "sabotage: the tagged fast path starved a posted message, as it must"
elif grep -q "^GOT" "$OUT/sab.log"; then
    bad "the message ARRIVED under PEEK_SABOTAGE: either the fast path is not live or the bits check is not load-bearing -- the gate proves nothing"
else
    bad "the sabotaged probe died oddly (log $OUT/sab.log) instead of starving"
fi

# 2: the kill switch must lift the same starvation by never seeding at all.
WINE_PPC64LE_PEEK_SABOTAGE=1 WINE_PPC64LE_NO_PEEK_BYPASS=1 \
    timeout 60 "$BUILD/wine" "$OUT/peek.exe" starve > "$OUT/sab2.log" 2>&1
if grep -q "^GOT" "$OUT/sab2.log"; then
    say "sabotage: WINE_PPC64LE_NO_PEEK_BYPASS=1 lifted it -- the kill switch reroutes to the trap"
else
    bad "NO_PEEK_BYPASS did not lift the sabotage (log $OUT/sab2.log); the kill switch is decoration"
fi

[ "$fail" = 0 ] && say "SABOTAGE PASS"
exit $fail
