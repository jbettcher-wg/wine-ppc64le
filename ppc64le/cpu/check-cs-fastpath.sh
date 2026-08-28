#!/bin/sh
#
# check-cs-fastpath.sh -- the guest's EnterCriticalSection/LeaveCriticalSection
# fast bodies implement WINE'S OWN lock algorithm, against the same lock word
# native code uses.
#
# WHAT THIS IS GUARDING.  [MEASURED] 2026-08-27, Cyberpunk -benchmark:
# Enter+LeaveCriticalSection were ~160,000 crossings a second between them,
# and an uncontended pair has no syscall behind it -- the trap was the whole
# cost.  The fast bodies (tools/spec2thunk, kinds 'ecs'/'lcs') serve the
# uncontended and recursion cases in guest user space and fall to the trap
# for everything contended.
#
# The danger is not a crash.  Native RtlEnterCriticalSection keeps running
# against the SAME struct from Wine-internal callers, so a fast body whose
# algorithm diverges corrupts a lock word both sides trust, and the symptom
# is a deadlock or a torn data structure minutes later.  So the probe
# (probes/cs_fastpath.c) checks the exact field values Wine's algorithm
# defines after every transition, mixes the native and fast implementations
# on one section, and hammers a deliberately non-atomic counter from three
# threads.
#
# --sabotage arms WINE_PPC64LE_CS_SABOTAGE_OWNER=1: the fast enter skips its
# OwningThread store, so the probe's `recurse` mode -- enter, then enter
# again on the same thread -- misreads the section as foreign and waits on a
# lock its own thread holds.  The control REQUIRES that hang (a timeout), and
# then requires WINE_PPC64LE_NO_CS_BYPASS=1 to lift it, which proves the
# kill switch actually reroutes to the trap rather than decorating it.
#
# Exit 0 = pass, 1 = a check failed, 2 = could not run (not a pass).
set -u

HERE=$(cd "$(dirname "$0")" && pwd)
SRC=$(cd "$HERE/../.." && pwd)
BUILD=${BUILD:-$SRC}
OUT=${OUT:-/tmp/check-cs-fastpath}
SABOTAGE=0
[ "${1:-}" = "--sabotage" ] && SABOTAGE=1

say()  { echo "check-cs-fastpath: $*"; }
bad()  { echo "check-cs-fastpath: FAIL $*" >&2; fail=1; }
skip() { echo "check-cs-fastpath: $*" >&2; exit 2; }

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
InitializeCriticalSection
EnterCriticalSection
LeaveCriticalSection
CreateThread
WaitForSingleObject
lstrlenA
EOF
cat > "$OUT/user32.def" <<'EOF'
LIBRARY user32.dll
EXPORTS
wsprintfA
EOF
for m in kernel32 user32; do
    llvm-dlltool -m i386:x86-64 -d "$OUT/$m.def" -l "$OUT/lib$m.a" \
        || skip "llvm-dlltool failed for $m"
done

clang -target x86_64-windows-gnu -nostdlibinc $INCL -D_UCRT -Wall -O1 -fno-builtin -g \
    -c -o "$OUT/cs.o" "$HERE/probes/cs_fastpath.c" || skip "guest compile failed"
clang -target x86_64-windows-gnu -fuse-ld=lld -nostdlib \
    -Wl,--entry=mainCRTStartup -Wl,--subsystem,console \
    -o "$OUT/cs.exe" "$OUT/cs.o" "$OUT/libkernel32.a" "$OUT/libuser32.a" \
    || skip "guest link failed"

WINEDLLOVERRIDES=winedbg.exe=d; export WINEDLLOVERRIDES

if [ "$SABOTAGE" = 0 ]; then
    "$BUILD/wine" "$OUT/cs.exe" > "$OUT/pos.log" 2>&1
    rc=$?
    sed -n 's/^/check-cs-fastpath:   /p' "$OUT/pos.log" | grep -E "PASS|FAIL"
    [ "$rc" = 0 ] || bad "probe exited $rc (log $OUT/pos.log)"
    grep -q "^PASS mechanism" "$OUT/pos.log" || bad "the mechanism layer did not pass"
    [ "$fail" = 0 ] && say "PASS"
    exit $fail
fi

# ---- sabotage -------------------------------------------------------------
# 1: the deliberately ownerless fast enter must DEADLOCK the recursion probe.
WINE_PPC64LE_CS_SABOTAGE_OWNER=1 timeout 30 "$BUILD/wine" "$OUT/cs.exe" recurse \
    > "$OUT/sab.log" 2>&1
rc=$?
if [ "$rc" = 124 ]; then
    say "sabotage: the ownerless fast enter deadlocked the recursive enter, as it must"
elif grep -q "DONE recurse" "$OUT/sab.log"; then
    bad "the recursion probe COMPLETED under CS_SABOTAGE_OWNER: either the fast path is not live or the owner store is not load-bearing -- the gate proves nothing"
else
    bad "the sabotaged probe died oddly (rc=$rc, log $OUT/sab.log) instead of hanging"
fi

# 2: the kill switch must lift the same deadlock by rerouting to the trap.
WINE_PPC64LE_CS_SABOTAGE_OWNER=1 WINE_PPC64LE_NO_CS_BYPASS=1 \
    timeout 30 "$BUILD/wine" "$OUT/cs.exe" recurse > "$OUT/sab2.log" 2>&1
if grep -q "DONE recurse" "$OUT/sab2.log"; then
    say "sabotage: WINE_PPC64LE_NO_CS_BYPASS=1 lifted it -- the kill switch reroutes to the trap"
else
    bad "NO_CS_BYPASS did not lift the sabotage (log $OUT/sab2.log); the kill switch is decoration"
fi

# The deadlocked run leaves a wine process behind; reap our own mess.
"$SRC/ppc64le/reap-orphans.sh" >/dev/null 2>&1 || true

[ "$fail" = 0 ] && say "SABOTAGE PASS"
exit $fail
