#!/bin/bash
# bench-cp2077.sh -- run Cyberpunk 2077's built-in benchmark N times and print
# one line per leg.  Exists because single-leg numbers on this port vary ~10%
# run to run, so any A/B needs several legs and a floor, not one average.
#
# Reports the FLOOR (min frametime) alongside the average: the floor is the
# steady per-frame overhead and moves when the port changes, while the average
# also absorbs scene variance and scheduling noise.
#
# INTERLOCKS, and why they are not paranoia.  Two copies of this script once ran
# at the same time.  The second one's "wait for the game to appear" latched onto
# the FIRST one's already-running GameThread, saw it exit, declared its own leg
# finished before that leg had even finished loading, and launched leg 2 on top
# of its own still-starting leg 1 -- two games in one prefix, contending, both
# numbers worthless.  So:
#   - a flock means only one copy of this script runs at a time;
#   - each leg refuses to start until NO GameThread exists anywhere;
#   - a leg is matched to its result by the newest directory NAME changing,
#     never by a count, because a count cannot tell whose directory appeared.
#
# Never SIGKILLs: a hard kill mid-GPU-submission has wedged the GPU on this box
# and needed a reboot.  A leg that overruns gets SIGTERM to the launcher, then
# waits, and is recorded as TIMEOUT.
#
#   usage: bench-cp2077.sh [legs] [tag]
set -u
LEGS=${1:-3}
TAG=${2:-run}
HERE=$(cd "$(dirname "$0")" && pwd)
LOG=${LOG:-$HERE/bench-cp2077-results.txt}

# Per-leg launcher output goes to a scratch directory, NOT $HOME.  A day of
# runs left 32 loose files and 250 MB in the owner's home directory before
# anyone noticed; the results file above is the durable artefact and lives in
# the tree, everything else is transient.
SCRATCH=${SCRATCH:-$HOME/Games/wine-ppc64le-stuff/logs}
mkdir -p "$SCRATCH"
LEG_TIMEOUT=${LEG_TIMEOUT:-1500}     # seconds before a leg is abandoned
IDLE_TIMEOUT=${IDLE_TIMEOUT:-600}    # seconds to wait for the box to go quiet

exec 9>"$HERE/.bench-cp2077.lock"
if ! flock -n 9; then
    echo "bench-cp2077: another copy is already running; refusing" >&2
    exit 2
fi

export DISPLAY=:1 XDG_RUNTIME_DIR=/run/user/1000
export FEX_APP_DATA_LOCATION=$HOME/Development/fexrootfs/
export FEX_ROOTFS=$HOME/Development/fexrootfs/RootFS/Ubuntu_24_04
export FEX_THUNKGUESTLIBS=$HOME/Development/fastppcx86/build-thunks/Guest
export FEX_THUNKHOSTLIBS=$HOME/Development/fastppcx86/build-thunks/HostLibs_64
export WINE_PPC64LE_TREE=$HOME/Development/powerpc64le-ports/hangover-ppc64le/wine-build

TOOL=$HOME/Development/powerpc64le-ports/hangover-ppc64le/wine-upstream/ppc64le/steamtool
EXE="$HOME/.local/share/Steam/steamapps/common/Cyberpunk 2077/bin/x64/Cyberpunk2077.exe"
RESULTS="$HOME/.local/share/wine-ppc64le/cp2077/pfx/drive_c/users/jbettcher/Documents/CD Projekt Red/Cyberpunk 2077/benchmarkResults"

wait_idle() {   # no GameThread anywhere, or give up
    local t0=$SECONDS
    while [ $((SECONDS-t0)) -lt "$IDLE_TIMEOUT" ]; do
        pgrep -x GameThread >/dev/null || return 0
        sleep 10
    done
    return 1
}

echo "# $(date -Is)  tag=$TAG  legs=$LEGS  smt=$(ppc64_cpu --smt 2>/dev/null)  bridge=$(md5sum "$WINE_PPC64LE_TREE/dlls/ntdll/libfexbridge.so" | cut -c1-12)" >> "$LOG"

for leg in $(seq 1 "$LEGS"); do
    if ! wait_idle; then
        echo "$TAG leg $leg: ABORT -- a GameThread is still running after ${IDLE_TIMEOUT}s" | tee -a "$LOG"
        break
    fi

    newest_before=$(ls -t "$RESULTS" 2>/dev/null | head -1)

    ( cd "$TOOL" && setsid ./run-native --name cp2077 --appid 1091500 "$EXE" -skipStartScreen -benchmark ) \
        > "$SCRATCH/bench-cp2077-$TAG-$leg.out" 2>&1 < /dev/null &
    launcher=$!

    t0=$SECONDS
    while [ $((SECONDS-t0)) -lt "$LEG_TIMEOUT" ]; do
        pgrep -x GameThread >/dev/null && break
        sleep 5
    done
    while [ $((SECONDS-t0)) -lt "$LEG_TIMEOUT" ]; do
        pgrep -x GameThread >/dev/null || break
        sleep 10
    done

    if pgrep -x GameThread >/dev/null; then
        kill -TERM "$launcher" 2>/dev/null
        pkill -TERM -f "steamtool/proton waitforexitandrun" 2>/dev/null
        wait_idle
        echo "$TAG leg $leg: TIMEOUT after ${LEG_TIMEOUT}s" | tee -a "$LOG"
        continue
    fi
    sleep 10

    d=$(ls -t "$RESULTS" 2>/dev/null | head -1)
    if [ -z "$d" ] || [ "$d" = "$newest_before" ] || [ ! -f "$RESULTS/$d/frames.csv" ]; then
        echo "$TAG leg $leg: NO RESULT (run did not finish; newest=$d)" | tee -a "$LOG"
        continue
    fi

    S="$RESULTS/$d/summary.json"
    F="$RESULTS/$d/frames.csv"
    avg=$(grep -oE "\"averageFps\": *[0-9.]+" "$S" | grep -oE "[0-9.]+" | awk "{printf \"%.3f\", \$1}")
    mn=$(grep -oE "\"minFps\": *[0-9.]+" "$S" | grep -oE "[0-9.]+" | awk "{printf \"%.3f\", \$1}")
    mx=$(grep -oE "\"maxFps\": *[0-9.]+" "$S" | grep -oE "[0-9.]+" | awk "{printf \"%.3f\", \$1}")
    nf=$(( $(wc -l < "$F") - 1 ))
    floor=$(awk -F, "NR>1 && \$2+0>0 {if (x==\"\" || \$2+0<x) x=\$2+0} END {printf \"%.2f\", x}" "$F")
    echo "$TAG leg $leg: avg=$avg min=$mn max=$mx frames=$nf floor_ms=$floor dir=$d" | tee -a "$LOG"
done
echo "# done $(date -Is)" >> "$LOG"
