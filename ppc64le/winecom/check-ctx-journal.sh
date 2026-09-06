#!/bin/sh
#
# check-ctx-journal.sh -- the D3D11 CONTEXT JOURNAL gate (journal_gen.h and
# the JG_SCOPE_CTX drain in libs/winecom/winecom.c).
#
# An immediate context's void state-setters -- binds, viewports, clears,
# copies, draws -- are recorded by generated guest-side snippets into a
# per-context ring and replayed, in order, at the next trap anywhere in the
# surface.  Three things can go wrong, and this gate has a layer for each:
#
#   0  THE MACHINE CODE.  On an x86-64 host with a C compiler,
#      probes/journal_gen_host.c generates every snippet in the table, runs
#      it natively with MS-x64 arguments, and checks the record byte for
#      byte (plus every fallback path).  Skipped, not failed, on a ppc64le
#      host: nothing there can execute the bytes.
#   1  MECHANISM.  probes/ctx_journal_probe.c PASSes under wine: every
#      journaled call's effect is read back through the API and compared --
#      scalars, proxies, struct arrays, proxy arrays, three arrays on one
#      count, a 16-byte blob, stack arguments through a D3D11_BOX, and the
#      texels a clear + copy + box-copy produced.
#   2  RECORDED, NOT TRAPPED.  The +winecom trace shows the generic install
#      lines, replay lines for the rows the probe drove, and NO live
#      dispatch line for those rows: had the snippet fallen back, the
#      dispatcher's own trace line for the row would be there.
#   3  BATCHED AND PRUNED (2026-09-06).  The drain hands the records to
#      d3d11.so in batches (one transition each: "batch replay of N
#      records") and skips a setter a later setter of the same state
#      overwrote before anything read it ("dead ...").  The probe's
#      interleaved topology writes produce both lines.
#
# --sabotage runs the negative controls instead, and each must go red:
#   a  WINEEMUCOMJOURNALSABOTAGE=1 records and never replays: the probe
#      must FAIL (its Gets see stale state, its texels are wrong) and the
#      transcript must be empty -- the observables of layers 1 and 2,
#      proven capable of failing.
#   b  WINEEMUNOCOMJOURNAL=1 lifts the mechanism whole: no generic install,
#      the journaled rows dispatch live, and the probe still PASSes --
#      trapping everything is the old world, and the old world works.
#   c  WINEEMUCOMBATCHSABOTAGE=1 builds every batch and runs none: the
#      transcript shows the batches, the probe FAILs.
#   d  WINEEMUNOCOMBATCH=1: no batch lines, one transition per record, and
#      the probe PASSes -- the pre-batch replay still works.
#   e  WINEEMUCOMLWWSABOTAGE=1 keeps the FIRST writer of a state: the
#      probe's last-writer-wins checks FAIL.
#   f  WINEEMUNOCOMLWW=1: no dead-record lines, the probe PASSes.
#
# Environment: WINEPREFIX (booted), WINEFEXBRIDGE, a GPU DXVK can open.
# BUILD to point at the build tree.  Exit 0 = pass, 1 = a check failed,
# 2 = could not run (not a pass).
set -u

HERE=$(cd "$(dirname "$0")" && pwd)
SRC=$(cd "$HERE/../.." && pwd)
BUILD=${BUILD:-$SRC}
OUT=${OUT:-/tmp/winecom-ctxjournal}
TIMEOUT=${TIMEOUT:-300}
SABOTAGE=0
[ "${1:-}" = "--sabotage" ] && SABOTAGE=1
fail=0

say()  { echo "check-ctx-journal: $*"; }
bad()  { echo "check-ctx-journal: FAIL $*" >&2; fail=1; }
skip() { echo "check-ctx-journal: $*" >&2; exit 2; }

mkdir -p "$OUT"

# ---- layer 0: the snippets, natively, where the host can run them -------

if [ "$SABOTAGE" = 0 ]; then
    if [ "$(uname -m)" = x86_64 ] && command -v cc >/dev/null; then
        say "layer 0: generating and executing every snippet natively"
        if cc -O1 -I "$SRC/libs/winecom" -o "$OUT/jgh" "$HERE/probes/journal_gen_host.c" 2>"$OUT/jgh.err" \
           && "$OUT/jgh" > "$OUT/jgh.out" 2>&1; then
            sed 's/^/  host| /' "$OUT/jgh.out"
        else
            sed 's/^/  host| /' "$OUT/jgh.out" "$OUT/jgh.err" 2>/dev/null >&2
            bad "the native snippet test failed"
        fi
    else
        say "layer 0: not an x86-64 host, skipping the native snippet test"
    fi
fi

# ---- build the guest probe -------------------------------------------

[ -x "$BUILD/wine" ] || skip "no wine loader at $BUILD/wine"
[ -n "${WINEPREFIX:-}" ] || skip "set WINEPREFIX to a prefix wineboot has run in"
[ -d "$WINEPREFIX/drive_c" ] || skip "WINEPREFIX has no drive_c"
[ -n "${WINEFEXBRIDGE:-}" ] || skip "set WINEFEXBRIDGE to the emulator bridge"
command -v clang >/dev/null || skip "need clang for the guest build"
command -v llvm-dlltool >/dev/null || skip "need llvm-dlltool for the guest build"

cat > "$OUT/kernel32.def" <<'EOF'
LIBRARY kernel32.dll
EXPORTS
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
        2>"$OUT/build.err" || skip "dlltool $m failed"
done

INCL="-I$BUILD/include -I$SRC/include -I$SRC/include/msvcrt"
clang -target x86_64-windows-gnu -nostdlibinc $INCL \
    -D_UCRT -Wall -O1 -fno-builtin -g \
    -c -o "$OUT/probe.o" "$HERE/probes/ctx_journal_probe.c" \
    2>>"$OUT/build.err" || { sed 's/^/  build| /' "$OUT/build.err" >&2
                             skip "guest compile failed"; }
clang -target x86_64-windows-gnu -fuse-ld=lld -nostdlib \
    -Wl,--entry=cj_entry -Wl,--subsystem,console \
    -o "$OUT/probe.exe" "$OUT/probe.o" \
    "$OUT/libd3d11.a" "$OUT/libkernel32.a" \
    2>>"$OUT/build.err" || { sed 's/^/  build| /' "$OUT/build.err" >&2
                             skip "guest link failed"; }

# ---- one leg ----------------------------------------------------------

# err+winecom,trace+winecom is APPENDED to the caller's WINEDEBUG so a
# suppressing environment cannot fake a pass.
run_leg() {   # run_leg <name> [ENV=VAL...]
    name=$1; shift
    timeout -k 5 "$TIMEOUT" env "$@" \
        WINEDEBUG="${WINEDEBUG:+$WINEDEBUG,}err+winecom,trace+winecom" \
        WINEDLLOVERRIDES="winedbg.exe=d" \
        "$BUILD/wine" "$OUT/probe.exe" > "$OUT/$name.out" 2> "$OUT/$name.err"
    rc=$?
    sed "s/^/  $name| /" "$OUT/$name.out"
    return $rc
}

# the rows the probe drives whose live dispatch would betray a fallback
DRIVEN="RSSetViewports RSSetScissorRects VSSetConstantBuffers IASetVertexBuffers OMSetBlendState OMSetRenderTargets ClearRenderTargetView CopySubresourceRegion CopyResource RSSetState IASetPrimitiveTopology Draw"

generic_installs() { grep -c 'journal: .* recorded guest-side from .* (generic' "$OUT/$1.err"; }
replays()          { grep -c 'journal: replay ID3D11DeviceContext' "$OUT/$1.err"; }
live_dispatches()  {   # live_dispatches <leg> <method>
    grep -c "ID3D11DeviceContext::$2 (iface " "$OUT/$1.err"
}

if [ "$SABOTAGE" = 0 ]; then
    say "layer 1: the probe, journal live"
    rc=0; run_leg pos || rc=$?
    if [ "$rc" = 2 ]; then skip "the probe could not create a device (log $OUT/pos.err)"; fi
    [ "$rc" = 0 ] || { sed 's/^/  pos| /' "$OUT/pos.err" | tail -20 >&2; bad "probe exited $rc"; }
    grep -q 'ctx_journal_probe: PASS' "$OUT/pos.out" || bad "probe did not PASS"

    say "layer 2: recorded, replayed, never trapped"
    n=$(generic_installs pos) || true
    [ "${n:-0}" -ge 60 ] || bad "expected >= 60 generic slots installed, saw ${n:-0}"
    n=$(replays pos) || true
    [ "${n:-0}" -ge 12 ] || bad "expected >= 12 replay lines, saw ${n:-0}"
    grep -q 'journal: replay ID3D11DeviceContext1::VSSetConstantBuffers1 proxy' "$OUT/pos.err" \
        || bad "the Context1 proxy's calls were never replayed"
    n=$(grep -c 'journal: context ring .* armed for host' "$OUT/pos.err") || true
    [ "${n:-0}" -eq 1 ] || bad "expected ONE shared context ring for the two proxies, saw ${n:-0}"
    for m in $DRIVEN; do
        grep -q "journal: replay ID3D11DeviceContext::$m proxy" "$OUT/pos.err" \
            || bad "$m was never replayed"
        n=$(live_dispatches pos "$m") || true
        [ "${n:-0}" -eq 0 ] || bad "$m dispatched live $n time(s): a snippet fell back"
    done
    # the replay of a call must come BEFORE the trap that observes it:
    # RSSetViewports' replay line precedes RSGetViewports' dispatch line
    r=$(grep -n 'journal: replay ID3D11DeviceContext::RSSetViewports' "$OUT/pos.err" | head -1 | cut -d: -f1)
    g=$(grep -n 'ID3D11DeviceContext::RSGetViewports (iface' "$OUT/pos.err" | head -1 | cut -d: -f1)
    if [ -n "$r" ] && [ -n "$g" ]; then
        [ "$r" -lt "$g" ] || bad "RSSetViewports replayed AFTER RSGetViewports was served"
    fi
    say "layer 3: batched and pruned"
    n=$(grep -c 'journal: batch replay of' "$OUT/pos.err") || true
    [ "${n:-0}" -ge 3 ] || bad "expected >= 3 batch replay lines, saw ${n:-0}"
    grep -q 'journal: dead ID3D11DeviceContext::IASetPrimitiveTopology' "$OUT/pos.err" \
        || bad "the overwritten topology writes were never pruned as dead"
    grep -q 'journal: dead ID3D11DeviceContext::RSSetState' "$OUT/pos.err" \
        || bad "the overwritten RSSetState was never pruned as dead"
    [ "$fail" = 0 ] && say "PASS"
else
    say "sabotage a: WINEEMUCOMJOURNALSABOTAGE=1 -- record, never replay"
    rc=0; run_leg sab WINEEMUCOMJOURNALSABOTAGE=1 || rc=$?
    [ "$rc" = 2 ] && skip "the probe could not create a device (log $OUT/sab.err)"
    grep -q 'WINEEMUCOMJOURNALSABOTAGE=1' "$OUT/sab.err" \
        || bad "sabotage lever unacknowledged -- is the mechanism even in?"
    if grep -q 'ctx_journal_probe: PASS' "$OUT/sab.out"; then
        bad "the probe PASSED with replay disabled -- its checks do not depend on the journal"
    else
        say "  red as required: the probe FAILED without replay"
    fi
    n=$(replays sab) || true
    [ "${n:-0}" -eq 0 ] || bad "sabotaged drain still replayed $n record(s)"

    say "sabotage b: WINEEMUNOCOMJOURNAL=1 -- the kill switch lifts it whole"
    rc=0; run_leg kill WINEEMUNOCOMJOURNAL=1 || rc=$?
    [ "$rc" = 0 ] || { sed 's/^/  kill| /' "$OUT/kill.err" | tail -20 >&2; bad "kill-switch leg exited $rc"; }
    grep -q 'ctx_journal_probe: PASS' "$OUT/kill.out" \
        || bad "kill-switch leg did not PASS -- trapping everything must still work"
    grep -q 'WINEEMUNOCOMJOURNAL=1' "$OUT/kill.err" || bad "kill switch unacknowledged"
    n=$(generic_installs kill) || true
    [ "${n:-0}" -eq 0 ] || bad "kill switch left $n generic slots installed"
    n=$(live_dispatches kill RSSetViewports) || true
    [ "${n:-0}" -ge 1 ] || bad "under the kill switch RSSetViewports never dispatched live"

    say "sabotage c: WINEEMUCOMBATCHSABOTAGE=1 -- batches built, never run"
    rc=0; run_leg bsab WINEEMUCOMBATCHSABOTAGE=1 || rc=$?
    grep -q 'WINEEMUCOMBATCHSABOTAGE=1' "$OUT/bsab.err" || bad "batch sabotage lever unacknowledged"
    n=$(grep -c 'journal: batch replay of' "$OUT/bsab.err") || true
    [ "${n:-0}" -ge 1 ] || bad "sabotaged drain built no batch at all"
    if grep -q 'ctx_journal_probe: PASS' "$OUT/bsab.out"; then
        bad "the probe PASSED with the batches never run -- the batch path carries nothing"
    else
        say "  red as required: the probe FAILED with the batches unrun"
    fi

    say "sabotage d: WINEEMUNOCOMBATCH=1 -- one transition per record"
    rc=0; run_leg nobatch WINEEMUNOCOMBATCH=1 || rc=$?
    [ "$rc" = 0 ] || { sed 's/^/  nobatch| /' "$OUT/nobatch.err" | tail -20 >&2; bad "no-batch leg exited $rc"; }
    grep -q 'ctx_journal_probe: PASS' "$OUT/nobatch.out" || bad "no-batch leg did not PASS"
    grep -q 'WINEEMUNOCOMBATCH=1' "$OUT/nobatch.err" || bad "no-batch lever unacknowledged"
    n=$(grep -c 'journal: batch replay of' "$OUT/nobatch.err") || true
    [ "${n:-0}" -eq 0 ] || bad "WINEEMUNOCOMBATCH=1 still batched $n time(s)"
    n=$(replays nobatch) || true
    [ "${n:-0}" -ge 12 ] || bad "no-batch leg replayed only ${n:-0} record(s)"

    say "sabotage e: WINEEMUCOMLWWSABOTAGE=1 -- the first writer wins"
    rc=0; run_leg lsab WINEEMUCOMLWWSABOTAGE=1 || rc=$?
    grep -q 'WINEEMUCOMLWWSABOTAGE=1' "$OUT/lsab.err" || bad "lww sabotage lever unacknowledged"
    if grep -q 'ctx_journal_probe: PASS' "$OUT/lsab.out"; then
        bad "the probe PASSED with first-writer-wins -- the dead-record pass drops nothing observable"
    else
        say "  red as required: the probe FAILED with the first writer kept"
    fi
    grep -q 'interleaved ctx/ctx1 topology writes: last writer (ctx1) wins: FAIL' "$OUT/lsab.out" \
        || bad "the interleaved topology check did not fail under first-writer-wins"

    say "sabotage f: WINEEMUNOCOMLWW=1 -- every record replays"
    rc=0; run_leg nolww WINEEMUNOCOMLWW=1 || rc=$?
    [ "$rc" = 0 ] || { sed 's/^/  nolww| /' "$OUT/nolww.err" | tail -20 >&2; bad "no-lww leg exited $rc"; }
    grep -q 'ctx_journal_probe: PASS' "$OUT/nolww.out" || bad "no-lww leg did not PASS"
    grep -q 'WINEEMUNOCOMLWW=1' "$OUT/nolww.err" || bad "no-lww lever unacknowledged"
    n=$(grep -c 'journal: dead ' "$OUT/nolww.err") || true
    [ "${n:-0}" -eq 0 ] || bad "WINEEMUNOCOMLWW=1 still pruned $n record(s)"
    [ "$fail" = 0 ] && say "PASS (all six controls red where required)"
fi

exit $fail
