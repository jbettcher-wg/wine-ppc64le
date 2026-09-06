#!/bin/sh
#
# bench-peek-cost.sh -- what one empty PeekMessageW costs the guest, per shape.
#
# Builds probes/peek_cost.c as an x86-64 guest PE and runs it in the given
# prefix.  Prints one `COST <shape> <window|nowindow> threads=N <ns> ns` line
# per cell (see the probe header for the shapes).  Not a gate: a bench, run
# on both sides of a change to win32u's peek path and compared.
#
#   WINEPREFIX=... WINEFEXBRIDGE=... [THREADS=1] [BUILD=...] ppc64le/cpu/bench-peek-cost.sh
#
# Exit 0 = ran, 2 = could not run.
set -u

HERE=$(cd "$(dirname "$0")" && pwd)
SRC=$(cd "$HERE/../.." && pwd)
BUILD=${BUILD:-$SRC}
OUT=${OUT:-/tmp/bench-peek-cost}
THREADS=${THREADS:-1}

skip() { echo "bench-peek-cost: $*" >&2; exit 2; }

[ -x "$BUILD/wine" ] || skip "no wine loader at $BUILD/wine"
[ -n "${WINEPREFIX:-}" ] || skip "set WINEPREFIX to a prefix wineboot has run in"
[ -n "${WINEFEXBRIDGE:-}" ] || skip "set WINEFEXBRIDGE to the emulator bridge"
command -v clang >/dev/null || skip "need clang for the guest build"
command -v llvm-dlltool >/dev/null || skip "need llvm-dlltool for the guest build"

mkdir -p "$OUT" || skip "cannot create $OUT"
INCL="-I$BUILD/include -I$SRC/include -I$SRC/include/msvcrt"

cat > "$OUT/kernel32.def" <<'DEF'
LIBRARY kernel32.dll
EXPORTS
GetStdHandle
WriteFile
ExitProcess
GetModuleHandleA
GetLastError
CreateThread
WaitForSingleObject
Sleep
QueryPerformanceCounter
QueryPerformanceFrequency
lstrlenA
DEF
cat > "$OUT/user32.def" <<'DEF'
LIBRARY user32.dll
EXPORTS
PeekMessageW
TranslateMessage
DispatchMessageW
DefWindowProcW
RegisterClassW
CreateWindowExW
UpdateWindow
DestroyWindow
wsprintfA
DEF
for m in kernel32 user32; do
    llvm-dlltool -m i386:x86-64 -d "$OUT/$m.def" -l "$OUT/lib$m.a" || skip "llvm-dlltool failed for $m"
done

clang -target x86_64-windows-gnu -nostdlibinc $INCL -D_UCRT -Wall -O1 -fno-builtin -g \
    -c -o "$OUT/peek_cost.o" "$HERE/probes/peek_cost.c" || skip "guest compile failed"
clang -target x86_64-windows-gnu -fuse-ld=lld -nostdlib \
    -Wl,--entry=mainCRTStartup -Wl,--subsystem,windows \
    -o "$OUT/peek_cost.exe" "$OUT/peek_cost.o" "$OUT/libkernel32.a" "$OUT/libuser32.a" \
    || skip "guest link failed"

WINEDLLOVERRIDES=winedbg.exe=d; export WINEDLLOVERRIDES
timeout 300 "$BUILD/wine" "$OUT/peek_cost.exe" "threads=$THREADS" > "$OUT/run.log" 2>"$OUT/run.err"
rc=$?
grep -E "^(COST|FAIL|DONE)" "$OUT/run.log"
[ "$rc" = 0 ] || { echo "bench-peek-cost: probe exited $rc (log $OUT/run.err)" >&2; exit 2; }
exit 0
