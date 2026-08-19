#!/bin/sh
#
# check-fibers.sh -- the gate for FIBERS in a guest process.
#
# A fiber is a stack with a program counter parked on it, switched by hand.
# Two things had to exist for a guest to use one on this host, and this gate
# holds both:
#
#   A. THE ppc64 HALF OF kernelbase's SWITCH.  switch_fiber() saves what
#      ELFv2 makes non-volatile -- r14-r31, r1, r2, f14-f31, v20-v31, CR, LR
#      -- into one fiber's CONTEXT and loads the other's, resuming through
#      CTR with r12 = the target, which is the ELFv2 global-entry contract.
#      Until this work the ppc64 branch did not exist at all: every
#      SwitchToFiber reached FIXME("not implemented") and the DbgBreakPoint
#      behind it.  DOOM (2016) died there every run, because id Tech 6's job
#      system is built on fibers.
#
#   B. THE PER-RUN EMULATOR STATE CROSSING THE SWITCH.  A fiber that runs
#      guest code has two stacks -- the native one kernelbase allocated and
#      the guest one the emulator run allocated -- and the bookkeeping tying
#      them together lives in thread-locals, which is correct only while a
#      thread's runs are NESTED.  Fibers are exactly the case that are not:
#      fiber A parks a live run, B runs one of its own, A resumes out of
#      order.  emu_SwitchToFiber (dlls/ntdll/signal_ppc64.c) carries the block
#      across, saved in a local of the intercepting frame -- which lives on
#      the switching fiber's own native stack, so the lifetime is right with
#      no table and nothing to clean up.
#
# Layers:
#
#   1  BUILD: the guest PE compiles, links, and imports every fiber entry
#      point from kernel32.dll -- the module the rows are keyed to, and the
#      module DOOM itself imports them from.
#   2  GUEST: the probe runs under the emulator and prints PASS.  Its central
#      assertion is not "did the switch happen" but WHICH STACK EACH FIBER IS
#      TOLD IT IS ON, read from the guest's own TEB after every switch.
#   3  TRANSCRIPT: stdout is byte-identical to the one embedded below.
#
# There is no native control here, and the reason is worth stating rather
# than leaving as an absence: the guest run already exercises the ppc64
# switch_fiber itself -- the switch is native code either way, and the guest
# transcript cannot come out right if the ELFv2 asm is wrong.  What a native
# run would add is only "is it wrong for a native caller too", which no
# caller in this port has.
#
# --sabotage (also the standalone flag) runs the port's own negative control,
# WINEEMUNOFIBERSTATE=1: the switch happens but the per-run block does not
# travel with it, which is precisely the state before part B.  Under it the
# run MUST fail, MUST still reach the probe's first marker (so it is failing
# at the thing under test and not before it), and the port MUST say why.
#
# WHY THE RUN DISABLES winedbg, verbatim from check-guest-callbacks.sh: the
# bringup prefix has AeDebug configured with "winedbg --auto", so an unhandled
# guest fault -- which the sabotage leg is trying to produce -- would start a
# debugger that attaches and never lets go, turning a red state into a hang.
#
# Exit 0 = pass, 1 = a check failed, 2 = could not run (not a pass).
set -u

HERE=$(cd "$(dirname "$0")" && pwd)
SRC=$(cd "$HERE/../.." && pwd)
BUILD=${BUILD:-$SRC}
OUT=${OUT:-/tmp/guest-fibers}
SABOTAGE=0
[ "${1:-}" = "--sabotage" ] && SABOTAGE=1

say()  { echo "check-fibers: $*"; }
bad()  { echo "check-fibers: FAIL $*" >&2; fail=1; }
skip() { echo "check-fibers: $*" >&2; exit 2; }

[ -x "$BUILD/wine" ] || skip "no wine loader at $BUILD/wine"
[ -n "${WINEPREFIX:-}" ] || skip "set WINEPREFIX to a prefix wineboot has run in"
[ -n "${WINEFEXBRIDGE:-}" ] || skip "set WINEFEXBRIDGE to the emulator bridge"
[ -f "$BUILD/dlls/kernel32/x86_64-windows/kernel32.dll" ] || \
    skip "no guest kernel32 thunk; build it first"
command -v clang >/dev/null || skip "need clang for the guest build"
command -v llvm-dlltool >/dev/null || skip "need llvm-dlltool for the guest build"
command -v llvm-readobj >/dev/null || skip "need llvm-readobj to read the built image"

mkdir -p "$OUT" || skip "cannot create $OUT"
fail=0

INCL="-I$BUILD/include -I$SRC/include -I$SRC/include/msvcrt"
TIMEOUT=${TIMEOUT:-60}

# ---- build: the x86-64 guest PE ------------------------------------------
cat > "$OUT/kernel32.def" <<'EOF'
LIBRARY kernel32.dll
EXPORTS
GetStdHandle
WriteFile
ExitProcess
ConvertThreadToFiber
ConvertFiberToThread
CreateFiber
DeleteFiber
SwitchToFiber
IsThreadAFiber
EOF
llvm-dlltool -m i386:x86-64 -d "$OUT/kernel32.def" -l "$OUT/libkernel32.a" \
    || skip "llvm-dlltool failed"

GUESTCC="clang -target x86_64-windows-gnu -nostdlibinc $INCL -fms-extensions \
-D_UCRT -Wall -O1 -fno-builtin -g"
GUESTLD="clang -target x86_64-windows-gnu -fuse-ld=lld -nostdlib \
-Wl,--entry=guest_fibers_entry -Wl,--subsystem,console"

$GUESTCC -c -o "$OUT/guest_fibers.o" "$HERE/guest_fibers.c" \
    || skip "guest compile failed"
$GUESTLD -o "$OUT/guest_fibers.exe" "$OUT/guest_fibers.o" "$OUT/libkernel32.a" \
    || skip "guest link failed"
EXE="$OUT/guest_fibers.exe"

# ---- 1: the image imports what it claims ---------------------------------
llvm-readobj --coff-imports "$EXE" > "$OUT/imports.txt" 2>&1
imported_from() {
    awk -v s="$1" '/Name: .*\.dll/ { dll = $2 }
                   $0 ~ ("Symbol: " s " ") { print dll }' "$OUT/imports.txt"
}
for sym in ConvertThreadToFiber ConvertFiberToThread CreateFiber DeleteFiber \
           SwitchToFiber IsThreadAFiber
do
    if imported_from "$sym" | grep -qx "kernel32.dll"; then
        say "image: $sym is imported from kernel32.dll"
    else
        bad "the guest exe does not import $sym from kernel32.dll (seen: \
$(imported_from "$sym" | tr '\n' ' '))"
    fi
done

WDBG=${WINEDEBUG:--all},err+seh

# ---- the transcript both runs must produce -------------------------------
cat > "$OUT/want.txt" <<'EOF'
guest_fibers: start
guest_fibers: not a fiber yet ok
guest_fibers: ConvertThreadToFiber returned a fiber ok
guest_fibers: now a fiber ok
guest_fibers: the thread's own stack did not move ok
guest_fibers: CreateFiber A ok
guest_fibers: CreateFiber B ok
guest_fibers: back on the thread's own stack after A ok
guest_fibers: main's locals are still main's ok
guest_fibers: back on the thread's own stack after A ok
guest_fibers: main's locals are still main's ok
guest_fibers: back on the thread's own stack after B->A ok
guest_fibers: A ran three times ok
guest_fibers: B ran once ok
guest_fibers: A was told its own stack every time ok
guest_fibers: B was told its own stack ok
guest_fibers: A's locals survived every switch ok
guest_fibers: A and B are on different stacks ok
guest_fibers: neither fiber ran on the thread's stack ok
guest_fibers: the switch order was M A M A M B A M ok
guest_fibers: still on the thread's own stack after DeleteFiber ok
guest_fibers: ConvertFiberToThread ok
guest_fibers: not a fiber any more ok
guest_fibers: turns A=3 B=1
guest_fibers: PASS
EOF

# ---- 2 and 3: the guest run ----------------------------------------------
run_guest() {
    timeout -k 5 "$TIMEOUT" env WINEDEBUG="$WDBG" WINEDLLOVERRIDES="winedbg.exe=d" \
        "$BUILD/wine" "$EXE" >"$OUT/guest.out" 2>"$OUT/guest.err"
}

if [ $SABOTAGE -eq 0 ]; then
    run_guest
    st=$?
    if [ $st -ne 0 ]; then
        bad "the guest run exited $st"
        tail -20 "$OUT/guest.err" | sed 's/^/  guest| /' >&2
        tail -20 "$OUT/guest.out" | sed 's/^/  guest| /' >&2
    else
        say "guest run exited 0"
    fi
    if diff -u "$OUT/want.txt" "$OUT/guest.out" > "$OUT/guest.diff" 2>&1; then
        say "guest transcript is byte-identical to the expected one"
    else
        bad "the guest transcript differs:"
        sed 's/^/  /' "$OUT/guest.diff" >&2
    fi

fi

# ---- --sabotage: the state does not travel with the switch ---------------
sabotage_fiberstate() {
    started=$(date +%s)
    timeout -k 5 "${DEADLINE:-30}" env WINEDEBUG="$WDBG" \
        WINEDLLOVERRIDES="winedbg.exe=d" WINEEMUNOFIBERSTATE=1 \
        "$BUILD/wine" "$EXE" >"$OUT/sab.out" 2>"$OUT/sab.err"
    st=$?
    elapsed=$(( $(date +%s) - started ))
    if [ $st -eq 124 ] || [ $st -eq 137 ]; then
        bad "WINEEMUNOFIBERSTATE=1 HUNG (killed after ${DEADLINE:-30}s); it must \
fail promptly, not hang"
        tail -10 "$OUT/sab.err" | sed 's/^/  sab| /' >&2
        return
    fi
    if [ $st -eq 0 ] && ! grep -q "FAIL" "$OUT/sab.out"; then
        bad "WINEEMUNOFIBERSTATE=1 exited 0 and reported no failure; the block \
this gate exists for is not actually load-bearing, or nothing checks it"
    else
        say "WINEEMUNOFIBERSTATE=1: exited $st after ${elapsed}s"
    fi
    if ! grep -q "^guest_fibers: start" "$OUT/sab.out"; then
        bad "WINEEMUNOFIBERSTATE=1 never reached the probe's first marker; it \
died before the thing under test and proves nothing"
    fi
    if grep -q "guest_fibers: PASS" "$OUT/sab.out"; then
        bad "WINEEMUNOFIBERSTATE=1 still printed PASS"
    fi
    if grep -q "WINEEMUNOFIBERSTATE" "$OUT/sab.err"; then
        say "WINEEMUNOFIBERSTATE=1: the port named the lever in its own diagnostics"
    else
        bad "WINEEMUNOFIBERSTATE=1 changed behaviour without the port saying so"
    fi
}

[ $SABOTAGE -eq 1 ] && sabotage_fiberstate

if [ $fail -eq 0 ]; then
    if [ $SABOTAGE -eq 1 ]; then say "SABOTAGE PASS"; else say "PASS"; fi
    exit 0
fi
say "FAILED" >&2
exit 1
