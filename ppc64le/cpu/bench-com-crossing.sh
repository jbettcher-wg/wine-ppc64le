#!/bin/sh
#
# bench-com-crossing.sh -- measure the price of one guest->native COM slot
# call on the 64-bit lane, from inside the guest.
#
# NOT A GATE.  Prints numbers; never fails a sweep.  The sibling
# bench-crossing.sh prices a flat export's trap; this prices what a COM
# vtable slot pays on top -- the dispatcher's COM arm, winecom_dispatch,
# and for d3d11 the second transition into d3d11.so -- so a change to any
# of those layers gets an A/B in seconds:
#
#     WINEPREFIX=... WINEFEXBRIDGE=... ppc64le/cpu/bench-com-crossing.sh
#     WINE_PPC64LE_NO_COM_FAST=1 WINEPREFIX=... ppc64le/cpu/bench-com-crossing.sh
#
# The pair above is the COM fast arm A/B (2026-09-04): the first leg serves
# a resolved COM slot from emu_trap_dispatch's small fast function, the
# second forces every slot through the full dispatcher body.
#
# The probe is ppc64le/cpu/probes/com_bench.c -- see its header for the
# slot choice.  It needs a GPU DXVK can open (D3D11CreateDevice HARDWARE);
# without one the probe exits 2 and so does this script.
# Output (guest stdout, passed through):
#     BENCH qpc_only_ns_per_call=...             the clock's own cost
#     BENCH com_getfeaturelevel_ns_per_call=...  ID3D11Device::GetFeatureLevel
#     BENCH com_gettype_ns_per_call=...          ID3D11DeviceContext::GetType
#     BENCH com_journaled_topology_ns_per_call=  IASetPrimitiveTopology, a
#                                                journaled slot: record +
#                                                amortized replay.  A/B with
#                                                WINEEMUNOCOMJOURNAL=1.
#
# The journaled line answers to four levers (2026-09-06, all in libs/winecom):
#     (default)                      record; batch replay; dead records pruned
#                                    (the probe cycles four topologies, so
#                                    every record but the last is dead: this
#                                    is the record's own cost)
#     WINEEMUNOCOMLWW=1              every record replays, in batches: the
#                                    per-record replay cost
#     WINEEMUNOCOMLWW=1 WINEEMUNOCOMBATCH=1   one transition per record, the
#                                    2026-09-04 replay
#     WINEEMUNOCOMJOURNAL=1          no journal: the slot traps, and since
#                                    EC DIRECT (round 9) that is the JIT's
#                                    inline call
#
# PERF_STAT=1 adds the hardware counters the timing alone cannot show --
# the crossing's cost on POWER8 is its branch mispredicts, not its
# instructions (crossing-asm-op4k.md section 1).  Each loop runs twice
# under `perf stat` (own processes, no privilege needed), once at
# COM_BENCH_N=200000 and once at 0 (device creation only), and the
# counter deltas divided by N are printed per call:
#     PERFSTAT <loop> cycles=... insns=... branches=... mispredicts=...
# A leg's timing line and its counter line together are the A/B record.
#
# Exit 0 = ran and printed, 2 = could not run.
set -u

HERE=$(cd "$(dirname "$0")" && pwd)
SRC=$(cd "$HERE/../.." && pwd)
BUILD=${BUILD:-$SRC}

skip() { echo "bench-com-crossing: $*" >&2; exit 2; }

[ -x "$BUILD/wine" ] || skip "no wine loader at $BUILD/wine"
[ -n "${WINEPREFIX:-}" ] || skip "set WINEPREFIX to a prefix wineboot has run in"
[ -n "${WINEFEXBRIDGE:-}" ] || skip "set WINEFEXBRIDGE to the emulator bridge"
command -v clang >/dev/null || skip "need clang to build the guest probe"
command -v llvm-dlltool >/dev/null || skip "need llvm-dlltool to build the guest probe"

OUT=${OUT:-/tmp/bench-com-crossing}
mkdir -p "$OUT" || skip "cannot create $OUT"
INCL="-I$BUILD/include -I$SRC/include -I$SRC/include/msvcrt"

cat > "$OUT/kernel32.def" <<'EOF'
LIBRARY kernel32.dll
EXPORTS
QueryPerformanceCounter
QueryPerformanceFrequency
GetEnvironmentVariableA
GetStdHandle
WriteFile
ExitProcess
EOF
cat > "$OUT/d3d11.def" <<'EOF'
LIBRARY d3d11.dll
EXPORTS
D3D11CreateDevice
EOF
for m in kernel32 d3d11; do
    llvm-dlltool -m i386:x86-64 -d "$OUT/$m.def" -l "$OUT/lib$m.a" \
        || skip "llvm-dlltool failed for $m"
done
clang -target x86_64-windows-gnu -nostdlibinc $INCL -Wall -O2 -fno-builtin -g \
    -c -o "$OUT/com_bench.o" "$HERE/probes/com_bench.c" || skip "guest compile failed"
clang -target x86_64-windows-gnu -fuse-ld=lld -nostdlib \
    -Wl,--entry=com_bench_entry -Wl,--subsystem,console \
    -o "$OUT/com_bench.exe" "$OUT/com_bench.o" "$OUT/libd3d11.a" "$OUT/libkernel32.a" \
    || skip "guest link failed"

WINEDEBUG=${WINEDEBUG:--all} WINEDLLOVERRIDES="winedbg.exe=d" \
    timeout -k 5 "${TIMEOUT:-300}" "$BUILD/wine" "$OUT/com_bench.exe" 2>"$OUT/bench.err"
rc=$?
[ "$rc" = 0 ] || { tail -5 "$OUT/bench.err" >&2; skip "the guest probe exited $rc (log $OUT/bench.err)"; }

if [ "${PERF_STAT:-0}" = 1 ]; then
    command -v perf >/dev/null || skip "PERF_STAT=1 needs perf"
    EVENTS=cycles,instructions,branches,branch-misses
    N=${PERF_N:-200000}
    # counts <loop> <n> -> "cycles insns branches misses" summed over the run
    counts() {
        COM_BENCH_LOOP=$1 COM_BENCH_N=$2 WINEDEBUG=-all WINEDLLOVERRIDES="winedbg.exe=d" \
            perf stat -x, -e $EVENTS -o "$OUT/perf-$1-$2.csv" -- \
            timeout -k 5 "${TIMEOUT:-300}" "$BUILD/wine" "$OUT/com_bench.exe" >/dev/null 2>"$OUT/perf-$1-$2.err" \
            || return 1
        awk -F, '/,cycles/{c=$1} /,instructions/{i=$1} /,branches/{b=$1} /,branch-misses/{m=$1}
                 END{print c, i, b, m}' "$OUT/perf-$1-$2.csv"
    }
    for loop in getfeaturelevel gettype journaled; do
        base=$(counts $loop 0) || { echo "PERFSTAT $loop: the N=0 run failed" >&2; continue; }
        full=$(counts $loop $N) || { echo "PERFSTAT $loop: the N=$N run failed" >&2; continue; }
        echo "$base $full $N" | awk '{
            printf "PERFSTAT %s cycles=%.0f insns=%.0f branches=%.1f mispredicts=%.2f per call (N=%d)\n",
                   "'"$loop"'", ($5-$1)/$9, ($6-$2)/$9, ($7-$3)/$9, ($8-$4)/$9, $9 }'
    done
fi
exit 0
