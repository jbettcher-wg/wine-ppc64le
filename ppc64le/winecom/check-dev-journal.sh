#!/bin/sh
#
# check-dev-journal.sh -- the DEVICE JOURNAL gate (the descriptor shadow's
# write half, libs/winecom/winecom.c).
#
# Device methods are free-threaded, so the command-list journal's
# one-recorder-per-object rule does not cover them; the device journal
# records curated creates into PER-THREAD rings and k-way-merges them by
# RDTSC stamp at the next real dispatch.  The hazard it must survive is
# app-synchronized cross-thread ordering: thread A creates a descriptor,
# synchronizes, thread B's work depends on it.  probes/dev_journal_probe.c
# builds exactly that shape: two threads alternate creates under a strict
# event baton, every create's BufferLocation encodes its issue index, and a
# dependent CopyDescriptorsSimple forces the drain.
#
# Layers:
#   1  MECHANISM.  The probe PASSes: every call succeeded end to end.
#   2  ARMING.    The +winecom trace shows both threads' rings armed --
#      each thread's first curated call trapped and armed, so exactly two.
#   3  ORDER.     The replay transcript is complete and IN ISSUED ORDER:
#      2N-2 replay lines (the two arming calls were served live), their
#      BufferLocations consecutive and ascending by 0x100 from base+0x200,
#      and every one BEFORE CopyDescriptorsSimple's own dispatch line.
#      This is the k-way merge proven at the only observable that matters.
#
# --sabotage runs two negative controls instead, and both must go red:
#   a  WINEEMUCOMDEVSABOTAGE=1 leaves recording live but never replays.
#      The order transcript (layer 3) must be EMPTY -- the same observable
#      the positive leg trusts, proven capable of failing.
#   b  WINEEMUNOCOMDEVJOURNAL=1 (the kill switch) must lift the mechanism
#      whole: no arming, no replays, and the probe still PASSes -- every
#      call trapping is the old world, and the old world works.
#
# Environment: WINEPREFIX (booted), WINEFEXBRIDGE, a working GPU (the
# probe creates a real D3D12 device).  BUILD to point at the build tree.
#
# Exit 0 = pass, 1 = a check failed, 2 = could not run (not a pass).
set -u

HERE=$(cd "$(dirname "$0")" && pwd)
SRC=$(cd "$HERE/../.." && pwd)
BUILD=${BUILD:-$SRC}
OUT=${OUT:-/tmp/winecom-devjournal}
TIMEOUT=${TIMEOUT:-300}
SABOTAGE=0
[ "${1:-}" = "--sabotage" ] && SABOTAGE=1
fail=0

say()  { echo "check-dev-journal: $*"; }
bad()  { echo "check-dev-journal: FAIL $*" >&2; fail=1; }
skip() { echo "check-dev-journal: $*" >&2; exit 2; }

[ -x "$BUILD/wine" ] || skip "no wine loader at $BUILD/wine"
[ -n "${WINEPREFIX:-}" ] || skip "set WINEPREFIX to a prefix wineboot has run in"
[ -d "$WINEPREFIX/drive_c" ] || skip "WINEPREFIX has no drive_c"
[ -n "${WINEFEXBRIDGE:-}" ] || skip "set WINEFEXBRIDGE to the emulator bridge"
command -v clang >/dev/null || skip "need clang for the guest build"
command -v llvm-dlltool >/dev/null || skip "need llvm-dlltool for the guest build"
command -v python3 >/dev/null || skip "need python3 for the order check"

mkdir -p "$OUT"

# ---- build the guest probe --------------------------------------------

cat > "$OUT/kernel32.def" <<'EOF'
LIBRARY kernel32.dll
EXPORTS
GetStdHandle
WriteFile
ExitProcess
CreateThread
WaitForSingleObject
SetEvent
CreateEventA
QueryPerformanceCounter
EOF
cat > "$OUT/d3d12.def" <<'EOF'
LIBRARY d3d12.dll
EXPORTS
D3D12CreateDevice
EOF
for m in kernel32 d3d12; do
    llvm-dlltool -m i386:x86-64 -d "$OUT/$m.def" -l "$OUT/lib$m.a" \
        2>"$OUT/build.err" || bad "dlltool $m failed"
done

INCL="-I$BUILD/include -I$SRC/include -I$SRC/include/msvcrt"
clang -target x86_64-windows-gnu -nostdlibinc $INCL \
    -D_UCRT -Wall -O1 -fno-builtin -g \
    -c -o "$OUT/probe.o" "$HERE/probes/dev_journal_probe.c" \
    2>>"$OUT/build.err" || { sed 's/^/  build| /' "$OUT/build.err" >&2
                             skip "guest compile failed"; }
clang -target x86_64-windows-gnu -fuse-ld=lld -nostdlib \
    -Wl,--entry=dj_entry -Wl,--subsystem,console \
    -o "$OUT/probe.exe" "$OUT/probe.o" \
    "$OUT/libd3d12.a" "$OUT/libkernel32.a" \
    2>>"$OUT/build.err" || { sed 's/^/  build| /' "$OUT/build.err" >&2
                             skip "guest link failed"; }

# ---- one leg ----------------------------------------------------------

# The gate reads the winecom channel: replay lines, arming lines, and the
# dispatch line of the dependent call.  err+winecom is APPENDED to the
# caller's WINEDEBUG so a suppressing environment cannot fake a pass.
run_leg() {   # run_leg <name> [ENV=VAL...]
    name=$1; shift
    timeout -k 5 "$TIMEOUT" env WINEEMUCOMDEVJOURNAL=1 "$@" \
        WINEDEBUG="${WINEDEBUG:+$WINEDEBUG,}err+winecom,trace+winecom" \
        "$BUILD/wine" "$OUT/probe.exe" > "$OUT/$name.out" 2> "$OUT/$name.err"
    rc=$?
    sed "s/^/  $name| /" "$OUT/$name.out"
    return $rc
}

replay_lines() { grep 'devjournal: replay ID3D12Device::CreateConstantBufferView' "$OUT/$1.err"; }
armed_count()  { grep -c 'devjournal: ring .* armed' "$OUT/$1.err"; }

# Layer 3: the transcript.  62 replays (2*32 issued, 2 armed live), their
# BufferLocations ascending by 0x100 starting at base+0x200, all before the
# CopyDescriptorsSimple dispatch line.
check_order() {   # check_order <name>
    base=$(sed -n 's/.*dev_journal_probe: base 0x0*//p' "$OUT/$1.out" | head -1)
    [ -n "$base" ] || { bad "$1: probe never printed its base VA"; return; }
    replay_lines "$1" | sed -n 's/.*va \([0-9a-f]*\) .*/\1/p' > "$OUT/$1.vas"
    n=$(wc -l < "$OUT/$1.vas")
    [ "$n" -eq 62 ] || { bad "$1: expected 62 replays, got $n"; return; }
    python3 - "$base" "$OUT/$1.vas" <<'EOF' || bad "replayed order is not the issued order"
import sys
base = int(sys.argv[1], 16)
vas = [int(l, 16) for l in open(sys.argv[2])]
want = [base + 256*g for g in range(2, 64)]
if vas != want:
    for i, (v, w) in enumerate(zip(vas, want)):
        if v != w:
            print(f"first divergence at replay {i}: got {v:#x} want {w:#x}",
                  file=sys.stderr)
            break
    sys.exit(1)
EOF
    # every replay before the dependent call's own dispatch
    copyline=$(grep -n 'ID3D12Device::CopyDescriptorsSimple' "$OUT/$1.err" | head -1 | cut -d: -f1)
    lastreplay=$(grep -n 'devjournal: replay' "$OUT/$1.err" | tail -1 | cut -d: -f1)
    [ -n "$copyline" ] || { bad "$1: CopyDescriptorsSimple never dispatched"; return; }
    [ "$lastreplay" -lt "$copyline" ] || \
        bad "$1: a replay came AFTER the dependent call was served"
}

# ---- the legs ---------------------------------------------------------

if [ "$SABOTAGE" = 0 ]; then
    say "positive leg: two threads, strict baton, dependent copy"
    run_leg pos || { sed 's/^/  pos| /' "$OUT/pos.err" | tail -20 >&2
                     bad "probe did not exit 0"; }
    grep -q 'dev_journal_probe: PASS' "$OUT/pos.out" || bad "probe did not PASS"
    n=$(armed_count pos) || true
    [ "$n" -eq 2 ] || bad "expected exactly 2 armed rings, got ${n:-0}"
    check_order pos
    [ "$fail" = 0 ] && say "PASS"
else
    say "sabotage a: WINEEMUCOMDEVSABOTAGE=1 -- record, never replay"
    run_leg sab WINEEMUCOMDEVSABOTAGE=1 || true
    grep -q 'WINEEMUCOMDEVSABOTAGE=1' "$OUT/sab.err" || \
        bad "sabotage lever unacknowledged -- is the mechanism even in?"
    n=$(armed_count sab) || true
    [ "$n" -ge 1 ] || bad "sabotage leg never armed a ring -- recording is not live"
    if replay_lines sab >/dev/null; then
        bad "sabotaged drain still replayed -- the control is not a control"
    else
        say "  red as required: rings recorded, transcript empty"
    fi

    say "sabotage b: WINEEMUNOCOMDEVJOURNAL=1 -- the kill switch lifts it whole"
    run_leg kill WINEEMUNOCOMDEVJOURNAL=1 || { sed 's/^/  kill| /' "$OUT/kill.err" | tail -20 >&2
                                               bad "kill-switch leg did not exit 0"; }
    grep -q 'dev_journal_probe: PASS' "$OUT/kill.out" || \
        bad "kill-switch leg did not PASS -- trapping-everything must still work"
    grep -q 'WINEEMUNOCOMDEVJOURNAL=1' "$OUT/kill.err" || \
        bad "kill switch unacknowledged"
    n=$(armed_count kill) || true
    [ "$n" -eq 0 ] || bad "kill switch left $n rings armed"
    if replay_lines kill >/dev/null; then bad "kill switch left replays live"; fi
    [ "$fail" = 0 ] && say "PASS (both controls red where required)"
fi

exit $fail
