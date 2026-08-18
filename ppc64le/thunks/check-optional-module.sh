#!/bin/sh
#
# check-optional-module.sh -- the RUNTIME-PROBED module gate.
#
# THE CLAIM
#
# A module a guest reaches only through LoadLibrary + GetProcAddress must be
# served to it as a guest module, exactly like one it imports statically.
#
# WHY IT EXISTS, AND WHY check-import-chain.sh CANNOT COVER IT
#
# That gate walks import TABLES.  A module nothing imports statically appears
# in no table, so a tree that builds it as a native ppc64 module and generates
# no AMD64 thunk for it looks complete to every static check there is -- and to
# the build, which happily produces the native module.  The gap is only visible
# at runtime, and only to a program that goes looking.
#
# DOOM (2016) goes looking.  It probes for Pdh.dll -- the Performance Data
# Helper -- and binds PdhOpenQueryW, PdhCloseQuery, PdhAddCounterW,
# PdhGetFormattedCounterValue and PdhCollectQueryData to read CPU counters.
# This tree had dlls/pdh with all five in its .spec and no pdh.thunks, so
# LoadLibraryW answered NULL.
#
# WHAT THAT COST, WHICH IS THE POINT OF GATING IT
#
# Not a missing feature.  DOOM allocates its loader object with operator new
# (0x58 bytes at DOOMx64vk.exe+0x19f4d69) and, on the path where the load
# fails, releases it through the ENGINE's allocator -- Mem_Free, which expects
# a sixteen-byte id Tech header the object never had.  The game then reports
#
#     FATAL ERROR: Memory corruption before block!
#
# and dies at 99% of startup.  The heap was measurably intact when it said so:
# under GlobalFlag=0x10 every one of ~19,600 live allocations still had its
# sixteen-byte 0xab tail canary, every subheap chain walked cleanly, and all
# 13,578 of the game's own blocks validated against their own cookie.  What was
# wrong was the pointer, not the memory -- it was a bare RtlAllocateHeap data
# pointer, with Wine's own struct block (08 00 20 00 05 00 75 80) sitting where
# DOOM expected its header.
#
# That is the general shape and it is not per-title: a failed optional probe
# does not make a guest do less, it makes a guest run code it has never run, and
# the failure surfaces as whatever that code happens to get wrong.  So the gate
# is on the probe succeeding, not on anything about pdh in particular.
#
# Legs:
#
#   A  SURFACE: the guest thunk PE for pdh exists at all.  Against an unbuilt
#      tree every other leg would be meaningless noise, so this is a skip.
#   B  EXPORTS: the five entry points DOOM binds are in that PE's export table.
#      Read from the built module, so "the thunk exists" cannot pass for "the
#      thunk vends what the application asks for".  That distinction is not
#      hypothetical here: spec2thunk refuses 11 of pdh's 50 eligible exports as
#      unrepresentable, and a module can be emitted with no exports at all --
#      it still loads, and every import of it binds a sentinel instead.
#   C  RUNTIME: a guest PE does what DOOM does -- LoadLibraryW(L"Pdh.dll"),
#      GetProcAddress for each of the five -- and then CALLS them and checks
#      the answers, including the two that write through caller pointers.
#      A thunk that reached the wrong function still returns something.
#   D  ALL OF THEM.
#
# --sabotage runs the negative controls instead and requires each to go red.  A
# gate that cannot go red proves nothing.
#
#   1  the built guest thunk moved aside for the duration of the run: the
#      module is then exactly as unserved as it was before pdh.thunks existed,
#      LoadLibraryW must answer NULL and the probe must not pass.  This is the
#      control that has something to prove -- it reproduces the original defect
#      mechanically rather than simulating it.
#   2  leg B asked for an export pdh does NOT vend: it must fail, proving the
#      export check reads the table rather than assuming it.
#
# Exit 0 = pass, 1 = a check failed, 2 = could not run at all (a skip is NOT
# a pass).
set -u

HERE=$(cd "$(dirname "$0")" && pwd)
SRC=$(cd "$HERE/../.." && pwd)
BUILD=${BUILD:-$SRC}                    # in-tree build by default
OUT=${OUT:-$HOME/.cache/wine-ppc64le/optional-module}
SABOTAGE=0
[ "${1:-}" = "--sabotage" ] && SABOTAGE=1

say()  { echo "check-optional-module: $*"; }
bad()  { echo "check-optional-module: FAIL $*" >&2; fail=1; }
skip() { echo "check-optional-module: $*" >&2; exit 2; }

fail=0
THUNK=$BUILD/dlls/pdh/x86_64-windows/pdh.dll
HIDDEN=

# The sabotage hides a build artefact, so it has to come back whatever happens
# -- including a timeout that kills the gate between the move and the restore.
restore() {
    if [ -n "$HIDDEN" ] && [ -f "$HIDDEN" ]; then
        mv -f "$HIDDEN" "$THUNK" && say "restored $THUNK"
        HIDDEN=
    fi
}
trap 'restore' EXIT INT TERM HUP

# ---- leg A: the guest thunk exists ---------------------------------------
[ -f "$THUNK" ] || skip "no guest thunk at $THUNK; build the tree first"
command -v python3 >/dev/null || skip "need python3"
say "thunk $THUNK"

mkdir -p "$OUT" || skip "cannot create $OUT"

# ---- leg B: it vends the five entry points DOOM binds ---------------------
# The list is the application's: these are the GetProcAddress arguments at
# DOOMx64vk.exe+0x19f4d40, in the order it asks for them.
want_exports() {
    cat <<'EOF'
PdhOpenQueryW
PdhCloseQuery
PdhAddCounterW
PdhGetFormattedCounterValue
PdhCollectQueryData
EOF
    # sabotage 2: ask for one that is deliberately not there.  Taken from the
    # generator's own refusal list rather than invented -- pdh.spec declares
    # PdhExpandCounterPathW and spec2thunk refuses it as "no declaration found
    # in Wine headers", so this is a real hole in the emitted table.
    [ "$SABOTAGE" = 1 ] && echo "PdhExpandCounterPathW"
    return 0
}

python3 - "$THUNK" "$HERE" > "$OUT/exports.txt" 2> "$OUT/exports.log" <<'PY' \
    || skip "could not read the thunk's export table; see $OUT/exports.log"
import sys
sys.path.insert(0, sys.argv[2])
from import_chain import PE
names, ords = PE(sys.argv[1]).exports()
for n in sorted(names):
    print(n)
PY

say "the thunk vends $(grep -c . < "$OUT/exports.txt") export(s)"
want_exports | sort -u > "$OUT/exports.want"
missing=$(comm -23 "$OUT/exports.want" "$OUT/exports.txt" | grep -c . || true)
if [ "$missing" -gt 0 ]; then
    if [ "$SABOTAGE" = 1 ]; then
        say "sabotage 2: red as required -- $missing requested export(s) absent:"
        comm -23 "$OUT/exports.want" "$OUT/exports.txt" | sed 's/^/    /'
        fail=1
    else
        bad "$missing entry point(s) DOOM binds are not vended by the thunk:"
        comm -23 "$OUT/exports.want" "$OUT/exports.txt" | sed 's/^/    /' >&2
        echo "    (see tools/spec2thunk --report for why each was refused)" >&2
    fi
else
    [ "$SABOTAGE" = 1 ] && bad "sabotage 2: an export that should be refused was vended"
    say "all five entry points DOOM binds are vended"
fi

# ---- leg C: the runtime probe --------------------------------------------
[ -x "$BUILD/wine" ] || skip "no wine loader at $BUILD/wine"
[ -n "${WINEPREFIX:-}" ] || skip "set WINEPREFIX to a prefix wineboot has run in"
[ -n "${WINEFEXBRIDGE:-}" ] || skip "set WINEFEXBRIDGE to the emulator bridge"
command -v clang >/dev/null || skip "need clang for the guest build"
command -v llvm-dlltool >/dev/null || skip "need llvm-dlltool for the guest build"

cat > "$OUT/kernel32.def" <<'EOF'
LIBRARY kernel32.dll
EXPORTS
GetStdHandle
WriteFile
LoadLibraryW
GetProcAddress
ExitProcess
EOF
llvm-dlltool -m i386:x86-64 -d "$OUT/kernel32.def" -l "$OUT/libkernel32.a" \
    || skip "llvm-dlltool failed"

clang -target x86_64-windows-gnu -nostdlibinc \
    -I"$BUILD/include" -I"$SRC/include" -I"$SRC/include/msvcrt" \
    -D_UCRT -Wall -O1 -fno-builtin -g \
    -c -o "$OUT/probe.o" "$HERE/optional_module_probe.c" \
    || skip "guest compile failed"
clang -target x86_64-windows-gnu -fuse-ld=lld -nostdlib \
    -Wl,--entry=probe_entry -Wl,--subsystem,console \
    -o "$OUT/probe.exe" "$OUT/probe.o" "$OUT/libkernel32.a" \
    || skip "guest link failed"

# sabotage 1: put the module back the way it was before pdh.thunks existed.
if [ "$SABOTAGE" = 1 ]; then
    HIDDEN=$OUT/pdh.dll.hidden
    mv -f "$THUNK" "$HIDDEN" || skip "cannot move $THUNK aside"
    say "sabotage 1: $THUNK moved aside"
fi

# Bounded: a guest that faults into the port's re-entrancy guard still exits,
# but one that spins would hang the gate rather than report red.
TIMEOUT=${TIMEOUT:-180}
( cd "$OUT" && timeout -k 5 "$TIMEOUT" \
      env WINEDEBUG=${WINEDEBUG:--all} WINEDLLOVERRIDES=winedbg.exe=d \
      "$BUILD/wine" ./probe.exe ) > "$OUT/probe.out" 2> "$OUT/probe.err"
rc=$?
restore

sed 's/^/    /' "$OUT/probe.out"

if [ "$SABOTAGE" = 1 ]; then
    if grep -q "optional-module-probe: PASS" "$OUT/probe.out"; then
        bad "sabotage 1: the probe PASSED with the guest thunk absent -- leg C cannot go red"
    else
        say "sabotage 1: red as required (thunk absent -> probe did not pass)"
        fail=1
    fi
else
    [ "$rc" = 0 ] || bad "the guest probe exited $rc (see $OUT/probe.err)"
    grep -q "optional-module-probe: PASS" "$OUT/probe.out" || \
        bad "the guest probe did not report PASS"
    # The sentinel the loader binds for a missing export is the other way this
    # can fail, and it is visible in the run even when a status check passes.
    if grep -q "wild pointer: 00000000DEAD" "$OUT/probe.err"; then
        bad "a missing-import sentinel was CALLED during the run:"
        grep "wild pointer: 00000000DEAD" "$OUT/probe.err" | sed 's/^/    /' >&2
    fi
fi

# ---- leg D ----------------------------------------------------------------
if [ "$SABOTAGE" = 1 ]; then
    if [ "$fail" = 0 ]; then
        echo "check-optional-module: FAIL sabotage: nothing went red" >&2
        exit 1
    fi
    say "SABOTAGE OK -- the controls go red"
    exit 0
fi

[ "$fail" = 0 ] || { echo "check-optional-module: FAILED" >&2; exit 1; }
say "PASS"
exit 0
