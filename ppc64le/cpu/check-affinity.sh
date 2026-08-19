#!/bin/sh
#
# check-affinity.sh -- a guest that asks for N processors gets N processors.
#
# TOPOLOGY.md says there is no affinity gate and that there should be one before
# anything touches this path.  This is it.
#
# The thing being tested is a NUMBER, not a status code.  wineserver used to map
# Windows processor i onto Linux CPU i, which is true on x86 by accident and
# false on POWER: [MEASURED] 2026-08-18, op4k, the online CPUs are
# 0-3,8-11,...,152-155, so a guest asking for eight processors named Linux CPUs
# 0-7 of which four are offline.  At the raw syscall level sched_setaffinity
# returns SUCCESS for that and the thread runs on four.  Through the whole Win32
# stack the symptom is different and no better: because the process affinity
# mask was itself built from Linux CPU numbers it came out SPARSE
# (0x0f0f0f0f0f0f0f0f), so a plain SetThreadAffinityMask(0xff) was REFUSED,
# while a mask naming bit 40 was accepted because Linux CPU 40 happens to be
# online.  A return-value check concludes the opposite of the truth in both
# directions, so this gate does not ask Wine whether it worked -- it asks the
# KERNEL where the thread may actually run, and counts.
#
# Four layers:
#
#   1  SET: the guest's SetThreadAffinityMask reports success.
#   2  READBACK: the mask read back equals the mask requested.
#   3  IDENTITY: the Linux CPU set the kernel actually installed is EXACTLY the
#      set this script derives independently from /sys -- independently, so a
#      bug in include/wine/cputopology.h cannot make the gate agree with the
#      server about a wrong answer.
#   4  COUNT: the number of ONLINE cpus in that set equals the number of bits
#      the guest asked for.  This is the layer that catches the original defect.
#
# The kernel's set is read from OUTSIDE the guest, from
# /proc/<pid>/task/<tid>/status.  An earlier version had the guest read
# /proc/thread-self/status through Wine's Z: drive; that reported the wrong
# thread (0-159, unrestricted) while the thread really was pinned to 0-3,8-11,
# so the in-guest reading is not trustworthy and is not used for the verdict.
#
# --sabotage re-runs with WINEEMUNOCPUMAP=1, which puts the original defect back
# -- wineserver treats Windows processor i as Linux CPU i again -- and requires
# the run to STOP producing the correct answer.  That is a real lever on the
# mechanism rather than a second copy of the rule.  On a machine that numbers
# its CPUs densely the identity map IS the correct map, so there is nothing to
# falsify and the sabotage run SKIPS rather than claiming a pass.
#
# Exit 0 = pass, 1 = a check failed, 2 = could not run (not a pass).
set -u

HERE=$(cd "$(dirname "$0")" && pwd)
SRC=$(cd "$HERE/../.." && pwd)
BUILD=${BUILD:-$SRC}
OUT=${OUT:-/tmp/check-affinity}
SABOTAGE=0
[ "${1:-}" = "--sabotage" ] && SABOTAGE=1

say()  { echo "check-affinity: $*"; }
bad()  { echo "check-affinity: FAIL $*" >&2; fail=1; }
skip() { echo "check-affinity: $*" >&2; exit 2; }

[ -x "$BUILD/wine" ] || skip "no wine loader at $BUILD/wine"
[ -n "${WINEPREFIX:-}" ] || skip "set WINEPREFIX to a prefix wineboot has run in"
[ -n "${WINEFEXBRIDGE:-}" ] || skip "set WINEFEXBRIDGE to the emulator bridge"
command -v clang >/dev/null || skip "need clang for the guest build"
command -v llvm-dlltool >/dev/null || skip "need llvm-dlltool for the guest build"
command -v python3 >/dev/null || skip "need python3 to derive the topology"
[ -r /sys/devices/system/cpu/online ] || skip "no /sys/devices/system/cpu/online"

mkdir -p "$OUT" || skip "cannot create $OUT"
fail=0

# ---- derive group 0 independently of include/wine/cputopology.h -----------
# Same documented rule the header follows -- NUMA nodes in ascending KERNEL id
# order (not 0..n-1: this machine's are 0 and 8), online CPUs within a node in
# ascending number, a node starting a new group if it would not fit whole in
# what is left of the current one -- but reimplemented here from /sys so the
# gate is a second opinion rather than an echo of the thing it is checking.
python3 > "$OUT/group0.txt" <<'PYEOF'
def parse(path):
    try:
        s = open(path).read().strip()
    except OSError:
        return []
    out = []
    for part in filter(None, s.split(',')):
        if '-' in part:
            a, b = part.split('-', 1)
            out.extend(range(int(a), int(b) + 1))
        else:
            out.append(int(part))
    return out

online = parse('/sys/devices/system/cpu/online')
nodes = parse('/sys/devices/system/node/online') or [0]
onset = set(online)

groups, cur = [], []
for n in sorted(nodes):
    cpus = parse('/sys/devices/system/node/node%d/cpulist' % n)
    cpus = sorted(c for c in cpus if c in onset)
    if not cpus and len(nodes) == 1:
        cpus = sorted(online)
    if not cpus:
        continue
    if cur and len(cur) + len(cpus) > 64 and len(cpus) <= 64:
        groups.append(cur)
        cur = []
    for c in cpus:
        if len(cur) == 64:
            groups.append(cur)
            cur = []
        cur.append(c)
if cur:
    groups.append(cur)

placed = {c for g in groups for c in g}
for c in sorted(online):
    if c not in placed:
        if not groups or len(groups[-1]) == 64:
            groups.append([])
        groups[-1].append(c)

print(' '.join(str(c) for c in (groups[0] if groups else [])))
PYEOF
[ -s "$OUT/group0.txt" ] || skip "could not derive group 0 from /sys"
GROUP0=$(cat "$OUT/group0.txt")
G0N=$(echo "$GROUP0" | wc -w)
[ "$G0N" -gt 0 ] || skip "group 0 came out empty"

ONLINE_RAW=$(cat /sys/devices/system/cpu/online)
say "group 0 holds $G0N processor(s); windows 0..7 -> linux $(echo "$GROUP0" | cut -d' ' -f1-8)"

# Densely numbered?  Then the old identity rule and the correct mapping are the
# same mapping and the negative control has nothing to falsify.
DENSE=1
i=0
for c in $GROUP0; do
    [ "$c" = "$i" ] || { DENSE=0; break; }
    i=$((i + 1))
done

if [ "$SABOTAGE" = 1 ] && [ "$DENSE" = 1 ]; then
    skip "SABOTAGE inapplicable: this machine numbers its CPUs densely, so the identity
      map IS the correct map and turning the translation off changes nothing.
      Not a pass."
fi

# ---- build the guest probe ------------------------------------------------
INCL="-I$BUILD/include -I$SRC/include -I$SRC/include/msvcrt"
cat > "$OUT/kernel32.def" <<'EOF'
LIBRARY kernel32.dll
EXPORTS
GetStdHandle
WriteFile
ExitProcess
GetCurrentThread
GetCurrentProcess
SetThreadAffinityMask
GetProcessAffinityMask
GetSystemInfo
CreateFileA
ReadFile
CloseHandle
Sleep
EOF
llvm-dlltool -m i386:x86-64 -d "$OUT/kernel32.def" -l "$OUT/libkernel32.a" \
    || skip "llvm-dlltool failed"

# Eight if the machine has eight, else all of group 0.  Eight is the
# interesting number: 0xff is what a real program writes for "eight
# processors", and on a 4-of-8 SMT machine bits 4-7 are exactly the offline
# ones.
WANTN=8
[ "$G0N" -ge 8 ] || WANTN=$G0N
WANTMASK=$(python3 -c "print(hex((1 << $WANTN) - 1))")

clang -target x86_64-windows-gnu -nostdlibinc $INCL \
    -DWANT_MASK="${WANTMASK}ull" -DHOLD_MS=30000 -D_UCRT -Wall -O1 -fno-builtin -g \
    -c -o "$OUT/affinity.o" "$HERE/probes/affinity.c" || skip "guest compile failed"
clang -target x86_64-windows-gnu -fuse-ld=lld -nostdlib \
    -Wl,--entry=affinity_entry -Wl,--subsystem,console \
    -o "$OUT/affinity.exe" "$OUT/affinity.o" "$OUT/libkernel32.a" \
    || skip "guest link failed"

EXPECT=$(echo "$GROUP0" | tr ' ' '\n' | head -n "$WANTN" | sort -n | tr '\n' ' ')

expand() {
    echo "$1" | tr ',' '\n' | while read -r r; do
        case "$r" in
            *-*) seq "${r%-*}" "${r#*-}" ;;
            "")  ;;
            *)   echo "$r" ;;
        esac
    done
}
# One canonical spelling for a cpu set, so two lists that mean the same thing
# compare equal: blank fields dropped, numeric order, single spaces.
norm() { echo "$1" | tr ' ,' '\n\n' | grep -v '^$' | sort -n | tr '\n' ' '; }
expand "$ONLINE_RAW" | sort > "$OUT/online.srt"

# WINEEMUNOCPUMAP is read by wineserver when it STARTS, so the running server
# for this prefix has to go before a run that changes it.  Only ever a server
# whose own environment names THIS prefix, and only ever by exact pid: other
# trees on this machine have their own wineservers and killing one of those
# would destroy unrelated work.
kill_prefix_server() {
    for s in $(pgrep -u "$USER" -x wineserver 2>/dev/null); do
        if tr '\0' '\n' < "/proc/$s/environ" 2>/dev/null | grep -qx "WINEPREFIX=$WINEPREFIX"
        then
            kill -TERM "$s" 2>/dev/null
            n=0
            while [ $n -lt 50 ] && kill -0 "$s" 2>/dev/null; do sleep 0.2; n=$((n + 1)); done
        fi
    done
}

# run_probe -> sets GOT (kernel cpulist), SETOK, RB; empty GOT means unread
run_probe() {
    kill_prefix_server
    rm -f "$OUT/out.txt" "$OUT/err.txt"
    env WINEDEBUG=-all ${1:+WINEEMUNOCPUMAP=1} "$BUILD/wine" "$OUT/affinity.exe" \
        >"$OUT/out.txt" 2>"$OUT/err.txt" &
    WPID=$!
    n=0
    ready=0
    while [ $n -lt 400 ]; do
        grep -q holding "$OUT/out.txt" 2>/dev/null && { ready=1; break; }
        kill -0 "$WPID" 2>/dev/null || break
        sleep 0.2
        n=$((n + 1))
    done
    GOT=""
    NPROC=0
    if [ "$ready" = 1 ]; then
        for p in $(pgrep -u "$USER" -f "$OUT/affinity.exe" 2>/dev/null); do
            st=/proc/$p/task/$p/status
            [ -r "$st" ] || continue
            line=$(grep Cpus_allowed_list: "$st" 2>/dev/null | awk '{print $2}')
            [ -n "$line" ] || continue
            NPROC=$((NPROC + 1))
            GOT=$line
        done
    fi
    kill -TERM "$WPID" 2>/dev/null
    wait "$WPID" 2>/dev/null
    SETOK=0
    grep -q 'set_result *= SUCCESS' "$OUT/out.txt" 2>/dev/null && SETOK=1
    RB=$(awk '/readback_mask/{print $3}' "$OUT/out.txt" 2>/dev/null)
}

if [ "$SABOTAGE" = 1 ]; then
    run_probe nocpumap
    grep -q 'WINEEMUNOCPUMAP is set' "$OUT/err.txt" \
        || bad "wineserver did not announce WINEEMUNOCPUMAP; the lever is not wired"
    GOTLIST=""
    [ -n "$GOT" ] && GOTLIST=$(norm "$(expand "$GOT" | tr '\n' ' ')")
    WANTSET=$(norm "$EXPECT")
    say "SABOTAGE: mapping off; set=$SETOK readback=$RB kernel gave: ${GOT:-<unread>}"
    if [ "$SETOK" = 1 ] && [ "$GOTLIST" = "$WANTSET" ]; then
        echo "check-affinity: FAIL with the mapping turned OFF the guest still landed on exactly [$WANTSET] -- the gate cannot go red" >&2
        exit 1
    fi
    [ $fail -eq 0 ] && say "SABOTAGE PASS (mapping off no longer produces the correct set)"
    exit $fail
fi

run_probe
[ "$NPROC" = 1 ] || bad "expected exactly one guest process to read, saw $NPROC"
[ -n "$GOT" ] || skip "could not read Cpus_allowed_list for the guest thread"

GOTLIST=$(norm "$(expand "$GOT" | tr '\n' ' ')")
expand "$GOT" | sort > "$OUT/got.srt"
EFF=$(comm -12 "$OUT/got.srt" "$OUT/online.srt" | wc -l)
WANTSET=$(norm "$EXPECT")

say "requested $WANTN processor(s) (mask $WANTMASK)"
say "kernel installed: $GOT"
say "of which online:  $EFF"

[ "$SETOK" = 1 ]           || bad "SetThreadAffinityMask did not report success"
[ "$RB" = "$WANTMASK" ]    || bad "readback mask is $RB, asked for $WANTMASK"
[ "$GOTLIST" = "$WANTSET" ] || bad "installed set is [$GOTLIST], expected [$WANTSET]"
[ "$EFF" = "$WANTN" ]      || bad "asked for $WANTN processors, thread is runnable on $EFF"

[ $fail -eq 0 ] && say "PASS"
exit $fail
