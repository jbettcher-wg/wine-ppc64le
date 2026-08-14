#!/usr/bin/env bash
# Build probes/tagged_handle_probe.c TWICE and run both.
#
#   patched   the tree as bootstrap.sh leaves it        -- must PASS
#   upstream  the same probe against the header exactly as it is at the pinned
#             upstream commit, extracted with `git show`  -- must FAIL
#
# The second build is not a simulation of the old behaviour. It is the old
# behaviour, compiled: `git show HEAD:include/private/vkd3d_native_sync_handle.h`
# is pristine because the series is applied to the working tree and never
# committed, the same arrangement dxvk-ppc64le uses. If it ever stops failing,
# either the probe stopped testing anything or upstream fixed it.
#
#   ./tagged_handle_run.sh
set -uo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(dirname "$HERE")"
SRC="$ROOT/src"
OUT=${OUT:-$HERE/build}
CC=${CC:-cc}
MCPU=${PPC_MCPU:-power8}

[ -d "$SRC/.git" ] || { echo "no $SRC -- run ./bootstrap.sh first" >&2; exit 1; }
mkdir -p "$OUT/upstream-include"

# DXVK's copy of the tag, if this checkout is next to one. Case F is the whole
# point of the change and is skipped rather than faked when it is absent.
DXVK_EVENT_H=${DXVK_EVENT_H:-$ROOT/../dxvk-ppc64le/src/include/native/windows/dxvk_native_event.h}
DXVK_FLAGS=()
if [ -f "$DXVK_EVENT_H" ]; then
    DXVK_FLAGS=(-DHAVE_DXVK_NATIVE_EVENT -I"$(dirname "$DXVK_EVENT_H")")
    echo "  DXVK tag from $DXVK_EVENT_H"
else
    echo "  no dxvk_native_event.h found -- case F will be SKIPPED, not passed"
fi

COMMON=(-std=gnu11 -O2 -Wall -Wextra -Wno-unused-parameter -mcpu="$MCPU"
        -I"$SRC/include" -I"$SRC/include/private")

fail=0

echo "=== build: patched"
"$CC" "${COMMON[@]}" "${DXVK_FLAGS[@]}" \
    -o "$OUT/tagged_handle_patched" "$HERE/tagged_handle_probe.c" || exit 1

echo "=== build: upstream (header at the pinned commit)"
git -C "$SRC" show HEAD:include/private/vkd3d_native_sync_handle.h \
    > "$OUT/upstream-include/vkd3d_native_sync_handle.h" || exit 1
# The A/B only means anything if the extracted copy really is upstream's and
# really is found first. Check the text, not the intention.
if ! grep -q "Treats FD 0 as invalid" "$OUT/upstream-include/vkd3d_native_sync_handle.h"; then
    echo "  the extracted header is not upstream's -- refusing to claim an A/B" >&2
    exit 1
fi
# -I order is load-bearing: the extracted copy must come BEFORE
# src/include/private or the compiler finds the patched header and the "before"
# build silently becomes a second "after" build. That is exactly what the first
# version of this script did, and the only thing that gave it away was the
# probe printing its own convention.
"$CC" -I"$OUT/upstream-include" "${COMMON[@]}" "${DXVK_FLAGS[@]}" \
    -o "$OUT/tagged_handle_upstream" "$HERE/tagged_handle_probe.c" || exit 1
if "$OUT/tagged_handle_upstream" 2>/dev/null | head -1 | grep -q PRESENT; then
    echo "  the 'upstream' build compiled the PATCHED header -- not an A/B" >&2
    exit 1
fi

if [ -x "$ROOT/../dxvk-ppc64le/scan-isa.sh" ]; then
    echo "=== POWER8 scan"
    "$ROOT/../dxvk-ppc64le/scan-isa.sh" "$OUT/tagged_handle_patched" 2>&1 | tail -3
fi

echo
echo "=== run: patched  (must PASS)"
"$OUT/tagged_handle_patched"; PRC=$?
[ $PRC -eq 0 ] || { echo "  PATCHED BUILD FAILED"; fail=1; }

echo
echo "=== run: upstream (must FAIL -- this is the defect)"
"$OUT/tagged_handle_upstream"; URC=$?
if [ $URC -eq 0 ]; then
    echo "  UPSTREAM BUILD PASSED, which means the probe tests nothing"
    fail=1
else
    echo "  upstream failed as required (exit $URC)"
fi

echo
[ $fail -eq 0 ] && echo "=== ALL OK" || echo "=== FAILURES"
exit $fail
