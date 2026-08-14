#!/bin/bash
# Build + run the fence-callback experiment on the AC922.
#
#   ./run.sh build          # compile guest (x86-64) and host (ppc64le) bits
#   ./run.sh <phase>        # run one phase under FEX
#   ./run.sh all            # run every phase, including the negative controls
#
# Everything runs against a private HOME so that a live ~/.config/fex-emu
# naming uninstalled thunk libraries cannot SIGTRAP every FEX invocation.
set -u

SRC="$(cd "$(dirname "$0")" && pwd)"
OUT="${FENCE_OUT:-$HOME/fence-exp}"
FEXBIN="${FEXBIN:-$OUT/bin/FEX}"
XGCC="${XGCC:-$HOME/Development/fexrootfs/x-tools/x86_64-linux-gnu/bin/x86_64-linux-gnu-gcc}"
ROOTFS="${ROOTFS:-$HOME/Development/fexrootfs/RootFS/Ubuntu_24_04}"

mkdir -p "$OUT/bin" "$OUT/home/.config/fex-emu" "$OUT/rv"
printf '{"Config":{"RootFS":"%s"}}' "$ROOTFS" > "$OUT/home/.config/fex-emu/Config.json"

build() {
  set -e
  "$XGCC" -O2 -g -static -pthread -o "$OUT/bin/guest_waiter" "$SRC/guest_waiter.c" -I"$SRC"
  gcc -O2 -g -shared -fPIC -pthread -o "$OUT/bin/libfencehost.so" "$SRC/host_worker.c" -I"$SRC"
  file "$OUT/bin/guest_waiter" | cut -d, -f1-2
  file "$OUT/bin/libfencehost.so" | cut -d, -f1-2
  # POWER8 floor check: the host object must contain no post-ISA-2.07 encodings.
  if objdump -d -Mpower8 "$OUT/bin/libfencehost.so" | grep -q '(bad)'; then
    echo "POWER8 CHECK: FAIL -- (bad) encodings present"; objdump -d -Mpower8 "$OUT/bin/libfencehost.so" | grep -c '(bad)'
  else
    echo "POWER8 CHECK: pass -- no (bad) under -Mpower8"
  fi
  set +e
}

run_phase() {
  local phase="$1"
  rm -f "$OUT/rv/rv.txt"
  echo "================ phase: $phase ================"
  env -i \
    HOME="$OUT/home" \
    PATH=/usr/bin:/bin \
    FENCE_EXP_DIR="$OUT/rv" \
    ${FENCE_NFD:+FENCE_NFD="$FENCE_NFD"} \
    ${FENCE_ROUNDS:+FENCE_ROUNDS="$FENCE_ROUNDS"} \
    ${FENCE_LOAD:+FENCE_LOAD="$FENCE_LOAD"} \
    ${FENCE_WATCHDOG:+FENCE_WATCHDOG="$FENCE_WATCHDOG"} \
    ${FENCE_HOST_DROP:+FENCE_HOST_DROP="$FENCE_HOST_DROP"} \
    LD_PRELOAD="$OUT/bin/libfencehost.so" \
    timeout -s KILL 90 "$FEXBIN" "$OUT/bin/guest_waiter" "$phase"
  local rc=$?
  echo "---- exit=$rc"
  return $rc
}

case "${1:-all}" in
  build) build ;;
  all)
    for p in pingpong fanout load poll epoll futex futexsh signal \
             control-nosignal control-callguest; do
      run_phase "$p"
    done ;;
  *) run_phase "$1" ;;
esac
