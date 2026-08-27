#!/bin/sh
#
# check-qpc-fastpath.sh -- the guest's QueryPerformanceCounter and the port's
# own NtQueryPerformanceCounter are ONE clock.
#
# WHAT THIS IS GUARDING.  [MEASURED] 2026-08-27, Cyberpunk 2077 -benchmark:
# KERNEL32.QueryPerformanceCounter was the hottest guest/native crossing in the
# port -- 256,638 a second, 14,061 a frame, a full trap AND a full syscall each
# -- so the guest now answers it in user space from the POWER timebase, the way
# Windows answers it from the TSC.  See include/wine/emu_qpc.h.
#
# Making that call fast is the easy half.  The half that can break a game
# silently is that the fast answer and the native answer must be the same
# clock.  They are not two clocks that happen to agree: the guest computes
# `(mulhi(rdtsc, M) >> s) + b` and the native side `mulhi(mftb, M) + b`, with
# rdtsc == mftb << s, so the two expressions are algebraically EQUAL.  A
# seeding bug -- a wrong scale, a wrong bias -- turns them into two clocks
# again, and the symptom in a game is not a crash.  It is physics and animation
# that are subtly wrong, because a frame delta computed from a QPC the game
# read and a QPC something inside Wine read is nonsense.  So the test is not
# "is QPC monotone"; it is "is the MERGED sequence monotone".
#
# Four layers, all in the guest (ppc64le/cpu/probes/qpc_fastpath.c):
#
#   1  MECHANISM  the kernel32 export's first bytes really are the fast body
#      and the host really armed it.  Without this the gate would pass just as
#      happily against a build that has no fast path at all.
#   2  FREQUENCY  QueryPerformanceFrequency is 10 MHz, and a 300 ms interval
#      measures the same through the fast path and through the syscall.
#   3  ORDER      40,000 readings taken alternately from kernel32's fast path
#      and from ntdll.NtQueryPerformanceCounter -- a plain trap stub, so the
#      native answer -- form one non-decreasing sequence; and 500,000
#      consecutive fast readings do too.
#   4  THREADS    the same alternation across two threads pinned to two
#      processors, trading readings through shared memory.  Processors 0 and
#      8, not 0 and 1: on POWER8 those two are SMT siblings of one CORE, and
#      the assumption this port now depends on is that the timebase is
#      synchronised across cores.  Checked, not assumed.
#
# --sabotage breaks the SEEDING, twice, and requires both to be caught:
#   WINE_PPC64LE_QPC_SABOTAGE_SHIFT=1  seeds the TSC scale one too high, so the
#       guest's clock runs at half rate -- the classic wrong-QpcShift bug.
#   WINE_PPC64LE_QPC_SABOTAGE_BIAS=1   seeds the epoch 100 ms ahead.
# Both leave the mechanism itself intact and change only the numbers the host
# writes into the guest's block, so the negative control falsifies the thing
# this gate claims to check rather than restating it.
#
# Exit 0 = pass, 1 = a check failed, 2 = could not run (not a pass).
set -u

HERE=$(cd "$(dirname "$0")" && pwd)
SRC=$(cd "$HERE/../.." && pwd)
BUILD=${BUILD:-$SRC}
OUT=${OUT:-/tmp/check-qpc-fastpath}
SABOTAGE=0
[ "${1:-}" = "--sabotage" ] && SABOTAGE=1

say()  { echo "check-qpc-fastpath: $*"; }
bad()  { echo "check-qpc-fastpath: FAIL $*" >&2; fail=1; }
skip() { echo "check-qpc-fastpath: $*" >&2; exit 2; }

[ -x "$BUILD/wine" ] || skip "no wine loader at $BUILD/wine"
[ -n "${WINEPREFIX:-}" ] || skip "set WINEPREFIX to a prefix wineboot has run in"
[ -n "${WINEFEXBRIDGE:-}" ] || skip "set WINEFEXBRIDGE to the emulator bridge"
command -v clang >/dev/null || skip "need clang for the guest build"
command -v llvm-dlltool >/dev/null || skip "need llvm-dlltool for the guest build"

case $(uname -m) in
    ppc64le) ;;
    *) skip "the timebase QPC fast path is a ppc64le mechanism; nothing to check here" ;;
esac

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
Sleep
GetModuleHandleA
GetProcAddress
QueryPerformanceCounter
QueryPerformanceFrequency
CreateThread
WaitForSingleObject
GetCurrentThread
SetThreadAffinityMask
GetSystemInfo
lstrlenA
EOF
cat > "$OUT/user32.def" <<'EOF'
LIBRARY user32.dll
EXPORTS
wsprintfA
EOF
# NtQueryPerformanceCounter comes from ntdll ON PURPOSE: it is a plain trap
# stub, so it is the answer the native side gives, and the fast path does not
# serve it.  That is what makes layer 3 an interleaving of two implementations
# rather than of one with itself.
cat > "$OUT/ntdll.def" <<'EOF'
LIBRARY ntdll.dll
EXPORTS
NtQueryPerformanceCounter
EOF
for m in kernel32 user32 ntdll; do
    llvm-dlltool -m i386:x86-64 -d "$OUT/$m.def" -l "$OUT/lib$m.a" \
        || skip "llvm-dlltool failed for $m"
done

clang -target x86_64-windows-gnu -nostdlibinc $INCL -D_UCRT -Wall -O1 -fno-builtin -g \
    -c -o "$OUT/qpc.o" "$HERE/probes/qpc_fastpath.c" || skip "guest compile failed"
clang -target x86_64-windows-gnu -fuse-ld=lld -nostdlib \
    -Wl,--entry=mainCRTStartup -Wl,--subsystem,console \
    -o "$OUT/qpc.exe" "$OUT/qpc.o" "$OUT/libkernel32.a" "$OUT/libuser32.a" "$OUT/libntdll.a" \
    || skip "guest link failed"

run_probe() {
    # $1 = log file; the rest of the environment is the caller's.
    "$BUILD/wine" "$OUT/qpc.exe" > "$1" 2>&1
    echo $? > "$1.rc"
}

# ---- the positive leg -----------------------------------------------------
if [ "$SABOTAGE" = 0 ]; then
    ( unset WINE_PPC64LE_QPC_SABOTAGE_SHIFT WINE_PPC64LE_QPC_SABOTAGE_BIAS
      unset WINE_PPC64LE_NO_QPC_BYPASS
      run_probe "$OUT/normal.log" )
    rc=$(cat "$OUT/normal.log.rc" 2>/dev/null || echo 99)
    grep -a '^\(PASS\|FAIL\)' "$OUT/normal.log" | sed 's/^/check-qpc-fastpath:   /'
    if [ "$rc" != 0 ]; then
        bad "the probe reported $rc failure(s)"
        sed 's/^/check-qpc-fastpath:   | /' "$OUT/normal.log" >&2
    else
        say "guest and native QPC are one clock, on one thread and across two"
    fi
    [ "$fail" = 0 ] && say "PASS"
    exit $fail
fi

# ---- the negative control -------------------------------------------------
# Each sabotage must be caught ON ITS OWN, so a gate that only notices one of
# them cannot hide behind the other.
for lever in SHIFT BIAS; do
    var=WINE_PPC64LE_QPC_SABOTAGE_$lever
    ( unset WINE_PPC64LE_QPC_SABOTAGE_SHIFT WINE_PPC64LE_QPC_SABOTAGE_BIAS
      export $var=1
      run_probe "$OUT/sabotage-$lever.log" )
    rc=$(cat "$OUT/sabotage-$lever.log.rc" 2>/dev/null || echo 99)
    if [ "$rc" = 0 ]; then
        bad "$var did not change the answer -- the gate is not measuring the seeding"
        sed 's/^/check-qpc-fastpath:   | /' "$OUT/sabotage-$lever.log" >&2
    elif [ "$rc" = 99 ]; then
        bad "$var: the probe did not run"
    else
        say "$var caught: $(grep -ac '^FAIL' "$OUT/sabotage-$lever.log") layer(s) went red"
        grep -a '^FAIL' "$OUT/sabotage-$lever.log" | sed 's/^/check-qpc-fastpath:   /'
    fi
done

[ "$fail" = 0 ] && say "SABOTAGE caught"
exit $fail
