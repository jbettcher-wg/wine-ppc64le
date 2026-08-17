#!/bin/sh
#
# check-ordinal-imports.sh -- the gate for IMPORTS THAT CARRY NO NAME.
#
# A PE import descriptor may name an ordinal instead of a symbol.  When it
# does, the number IS the entire request: the guest's loader looks up N in the
# target module's export table, and if the table is numbered differently it
# either finds nothing (ntdll binds a sentinel and the first call faults on a
# wild pointer) or, worse, finds SOMETHING ELSE and binds it silently.
#
# tools/spec2thunk used to write a .def that listed export NAMES only and let
# lld-link number them 1..N in name order.  Every module in this tree that
# pins ordinals -- gdiplus, shlwapi, oleaut32, user32, ws2_32, wsock32,
# winspool.drv, uxtheme, gdi32, xinput1_3/4, winmm, shell32, msimg32, d3d12 --
# published the wrong numbers to guests, and nothing noticed until Steam ran
# its own GPU probe: d3ddriverquery64.exe imports d3d12.dll ordinal 101 with no
# name at all (Microsoft's d3d12.dll exports D3D12CreateDevice there), bound
# 0xdead0001, and died calling it from d3ddriverquery64.exe+0x108e.  The fix is
# in the generator -- every export goes into the .def with the ordinal its own
# source pins, numbered exactly as tools/winebuild/parser.c:assign_ordinals()
# numbers the real module -- and this file is what keeps it fixed.
#
# Six legs:
#
#   A  BUILD: the probe compiles and links, and ITS OWN import table really is
#      by ordinal -- llvm-readobj must show d3d12.dll asking for `(101)` and
#      `(60001)` with NO name beside either.  This is the layer that catches
#      an import library that quietly fell back to by-name binding, which
#      would let every layer below pass for the wrong reason.
#   B  EXPORT TABLES, ACROSS EVERY THUNK: for each built guest thunk PE, every
#      ordinal its module's own Wine .spec PINS must be the ordinal that export
#      answers to in the thunk.  The expected numbers are re-read here from the
#      .spec by awk -- this script shares no code with the generator that wrote
#      them, which is the only reason the comparison means anything.
#   C  GUEST: the probe runs under the emulator and prints PASS.  Every step is
#      a value check; see ordinal_import.c for what each one is and why.
#   D  TRANSCRIPT: stdout is byte-identical to the transcript embedded below.
#   E  LOUD AT BIND TIME: a WINEDEBUG=warn+module run must name the ordinal it
#      could not serve -- "No implementation for d3d12.dll.60001 imported
#      from ..." -- because a hole in an export table that binds silently is
#      the failure this whole gate exists to make impossible.
#   F  LOUD AT CALL TIME: the separate ORDINAL_PROBE_CALL_BOGUS build CALLS
#      that sentinel.  The process must die -- nonzero, and without printing
#      the line that follows the call in the source.  A sentinel that returned
#      a plausible value would be undiagnosable.
#   G  AND PROMPTLY, IN THE SHAPE A LIVE STEAM LAUNCH HAS: the same call with a
#      top-level exception filter installed that faults while reporting, which
#      is what DOOM's crash reporter did on 2026-08-17 on top of exactly such a
#      sentinel.  Each fault restarted the whole unhandled report, ~4.8 KiB of
#      native stack deeper, until the thread's 8 MiB was gone and it spun at 0%
#      holding a critical section another thread was waiting on -- so the
#      process hung rather than died, and a hang names nothing.  This leg
#      asserts on the CLOCK as well as the exit status, requires the sentinel
#      to be named in stderr, and requires no EXCEPTION_STACK_OVERFLOW anywhere
#      in it.  The guard is guest_exc_raising in dlls/ntdll/signal_ppc64.c.
#
# --sabotage runs the negative controls instead, and requires BOTH to go red.
# Neither needs Wine rebuilt: the lever is in the .def the probe is linked
# against, which is the same place the real defect lived.
#
#   1  ordinal 102 (D3D12GetDebugInterface) is bound under the name the probe
#      believes is 101.  Nothing is NULL and no sentinel appears -- so leg C
#      must still fail, at step 4, which is what proves this gate compares
#      VALUES and not liveness.
#   2  the "bogus" slot is given ordinal 101, a real export.  Step 5 -- the
#      sentinel check -- must fail.
#
# A gate that cannot go red proves nothing; this is how it proves it can.
#
# Exit 0 = pass, 1 = a check failed, 2 = could not run at all (a skip is NOT
# a pass).
#
#
# WHY EVERY WINE RUN DISABLES winedbg, verbatim from check-d3d11-smoke.sh and
# check-seh-smoke.sh because the hazard is identical here: the bringup prefix
# has AeDebug configured with "winedbg --auto", so any run that ends in an
# unhandled fault -- which is exactly what leg F asks for ON PURPOSE -- starts
# the debugger, which attaches, loads its GUI stack and never lets go.  That
# turns the red state of this gate into a hang, which is the one thing a gate
# must never be.  WINEDLLOVERRIDES=winedbg.exe=d makes start_debugger's
# CreateProcess fail, so UnhandledExceptionFilter falls straight through to
# terminating the process.  This is an environment override for the duration
# of one run and touches nothing in the prefix.
set -u

HERE=$(cd "$(dirname "$0")" && pwd)
SRC=$(cd "$HERE/../.." && pwd)
BUILD=${BUILD:-$SRC}                    # in-tree build by default
OUT=${OUT:-/tmp/ordinal-imports}
SABOTAGE=0
[ "${1:-}" = "--sabotage" ] && SABOTAGE=1

say()  { echo "check-ordinal-imports: $*"; }
bad()  { echo "check-ordinal-imports: FAIL $*" >&2; fail=1; }
note() { echo "check-ordinal-imports: note $*"; }
skip() { echo "check-ordinal-imports: $*" >&2; exit 2; }

[ -x "$BUILD/wine" ] || skip "no wine loader at $BUILD/wine"
[ -n "${WINEPREFIX:-}" ] || skip "set WINEPREFIX to a prefix wineboot has run in"
[ -n "${WINEFEXBRIDGE:-}" ] || skip "set WINEFEXBRIDGE to the emulator bridge"
[ -f "$BUILD/dlls/d3d12/x86_64-windows/d3d12.dll" ] || \
    skip "no guest d3d12 thunk; build it first"
command -v clang >/dev/null || skip "need clang for the guest build"
command -v llvm-dlltool >/dev/null || skip "need llvm-dlltool for the guest build"
command -v llvm-readobj >/dev/null || skip "need llvm-readobj to read the tables"

mkdir -p "$OUT" || skip "cannot create $OUT"
fail=0

INCL="-I$BUILD/include -I$SRC/include -I$SRC/include/msvcrt"
TIMEOUT=${TIMEOUT:-120}
GUESTCC="clang -target x86_64-windows-gnu -nostdlibinc $INCL -Wall -O1 -fno-builtin -g"
GUESTLD="clang -target x86_64-windows-gnu -fuse-ld=lld -nostdlib \
-Wl,--entry=ordinal_import_entry -Wl,--subsystem,console"

# The ordinal this probe asks for that d3d12 cannot possibly serve.  Past the
# module's whole ordinal space (its highest export is 108 and the generator's
# own __wine_thunk_info sits at 109), so it can only ever be a hole.
BOGUS=60001

# ---- the import libraries ---------------------------------------------------
# Hand-written, the same way check-d3d11-smoke.sh and check-com-smoke.sh
# describe their imports by hand: the probe binds to the builtins a real guest
# application binds to and nothing else is linked in at all.  What is different
# here is the `@N NONAME` form -- that, and only that, is what makes
# llvm-dlltool emit an import the LOADER must satisfy by number.
write_d3d12_def()   # $1 = ordinal for the D3D12CreateDevice slot, $2 = bogus slot
{
    cat > "$OUT/d3d12.def" <<EOF
LIBRARY d3d12.dll
EXPORTS
D3D12CreateDevice @$1 NONAME
ordprobe_bogus_ordinal @$2 NONAME
EOF
    llvm-dlltool -m i386:x86-64 -d "$OUT/d3d12.def" -l "$OUT/libd3d12.a" \
        || skip "llvm-dlltool failed for d3d12.def"
}

cat > "$OUT/kernel32.def" <<'EOF'
LIBRARY kernel32.dll
EXPORTS
GetStdHandle
WriteFile
ExitProcess
GetProcAddress
GetModuleHandleW
SetUnhandledExceptionFilter
EOF
llvm-dlltool -m i386:x86-64 -d "$OUT/kernel32.def" -l "$OUT/libkernel32.a" \
    || skip "llvm-dlltool failed for kernel32.def"

build_probe()   # $1 = output exe, $2... = extra -D flags
{
    exe=$1; shift
    $GUESTCC "$@" -c -o "$OUT/probe.o" "$HERE/probes/ordinal_import.c" \
        > "$OUT/build.err" 2>&1 || { sed 's/^/  cc| /' "$OUT/build.err" >&2; return 1; }
    $GUESTLD -o "$exe" "$OUT/probe.o" "$OUT/libd3d12.a" "$OUT/libkernel32.a" \
        >> "$OUT/build.err" 2>&1 || { sed 's/^/  ld| /' "$OUT/build.err" >&2; return 1; }
    return 0
}

WDBG=${WINEDEBUG:--all},err+seh
run_wine() { timeout -k 5 "$TIMEOUT" \
                 env WINEDEBUG="$WDBG" WINEDLLOVERRIDES="winedbg.exe=d" \
                 "$BUILD/wine" "$@"; }

# ---- (also available standalone as --sabotage): the negative controls -------
sabotage() {
    ok=1

    # 1: ordinal 102 under the name the probe believes is 101.
    write_d3d12_def 102 "$BOGUS"
    if build_probe "$OUT/sab1.exe" -DORDINAL_PROBE_SABOTAGE=1; then
        run_wine "$OUT/sab1.exe" > "$OUT/sab1.out" 2> "$OUT/sab1.err"
        if grep -q "ordinal_import: PASS" "$OUT/sab1.out"; then
            bad "SABOTAGE=1 still PASSED; the identity check cannot go red"; ok=0
        elif grep -q "^step 4 .*FAIL" "$OUT/sab1.out"; then
            say "sabotage 1: step 4 went red as it must: $(grep -m1 '^step 4' \
                "$OUT/sab1.out" | cut -c1-110)"
        else
            bad "SABOTAGE=1 failed, but NOT at step 4 -- the lever proves \
nothing about the identity check"; ok=0
            sed 's/^/  sab1| /' "$OUT/sab1.out" >&2
        fi
    else
        bad "SABOTAGE=1 build failed; cannot prove this check can fail"; ok=0
    fi

    # 2: a real export in the bogus slot.
    write_d3d12_def 101 101
    if build_probe "$OUT/sab2.exe" -DORDINAL_PROBE_SABOTAGE=2; then
        run_wine "$OUT/sab2.exe" > "$OUT/sab2.out" 2> "$OUT/sab2.err"
        if grep -q "ordinal_import: PASS" "$OUT/sab2.out"; then
            bad "SABOTAGE=2 still PASSED; the sentinel check cannot go red"; ok=0
        elif grep -q "^step 5 .*FAIL" "$OUT/sab2.out"; then
            say "sabotage 2: step 5 went red as it must: $(grep -m1 '^step 5' \
                "$OUT/sab2.out" | cut -c1-110)"
        else
            bad "SABOTAGE=2 failed, but NOT at step 5"; ok=0
            sed 's/^/  sab2| /' "$OUT/sab2.out" >&2
        fi
    else
        bad "SABOTAGE=2 build failed; cannot prove this check can fail"; ok=0
    fi

    [ "$ok" = 1 ] && say "SABOTAGE PASS"
    [ "$ok" = 1 ]
}

if [ "$SABOTAGE" = 1 ]; then
    sabotage && exit 0
    exit 1
fi

# ---- A: build, and prove the import really has no name ----------------------
write_d3d12_def 101 "$BOGUS"
build_probe "$OUT/guest.exe" || skip "guest build failed"
llvm-readobj --coff-imports "$OUT/guest.exe" > "$OUT/guest.imports" 2>&1 \
    || skip "llvm-readobj could not read $OUT/guest.exe"
# The descriptor block for d3d12.dll, and only that one.
awk '/^Import \{/{keep=0} /Name: d3d12.dll/{keep=1} keep' "$OUT/guest.imports" \
    > "$OUT/guest.d3d12.imports"
for want in "Symbol:  (101)" "Symbol:  ($BOGUS)"; do
    if grep -qF "$want" "$OUT/guest.d3d12.imports"; then
        say "build: the probe imports d3d12.dll by \"${want#Symbol:  }\", with no name"
    else
        bad "the probe's d3d12 import is not '$want' -- it did not bind by ordinal"
        sed 's/^/  imports| /' "$OUT/guest.d3d12.imports" >&2
    fi
done

# ---- B: every pinned ordinal, in every built thunk --------------------------
# Read straight out of each module's own .spec, here, by a completely different
# mechanism from the one that wrote the export table.  A pinned line looks like
#
#     101 stdcall D3D12CreateDevice(ptr long ptr ptr)
#     115 stdcall -private WSAStartup(long ptr)
#
# -- the ordinal first, then the type, then any number of -flags, then the name
# with its argument list attached.  Entries this machine does not export at all
# (-i386, -arch= without x86_64) are dropped: they are not in an AMD64 export
# table and their numbers belong to a different machine's.
pinned_ordinals() {
    awk '
        { sub(/#.*/, "") }
        $1 ~ /^[0-9]+$/ && NF >= 3 {
            ord = $1
            skip = 0
            i = 3
            while (i <= NF && substr($i, 1, 1) == "-") {
                if ($i == "-i386") skip = 1
                else if ($i ~ /^-arch=/) {
                    a = substr($i, 7)
                    if (a ~ /!x86_64/ || a ~ /!win64/) skip = 1
                    else if (a !~ /x86_64/ && a !~ /amd64/ && a !~ /win64/) skip = 1
                }
                i++
            }
            if (skip || i > NF) next
            name = $i
            sub(/\(.*/, "", name)
            if (name == "" || name == "@") next
            print ord "\t" name
        }' "$1"
}

thunk_ordinals() {
    llvm-readobj --coff-exports "$1" \
        | awk '/Ordinal:/{o=$2} /Name:/{ if (NF > 1) print o "\t" $2 }'
}

checked=0; modules=0; mismatched=0
for dll in "$BUILD"/dlls/*/x86_64-windows/*.dll; do
    [ -e "$dll" ] || continue
    stem=$(basename "$dll" .dll)
    spec="$SRC/dlls/$stem/$stem.spec"
    [ -f "$spec" ] || { note "no .spec beside $stem; nothing to cross-check"; continue; }
    pinned_ordinals "$spec" | sort -u > "$OUT/$stem.pinned"
    [ -s "$OUT/$stem.pinned" ] || continue
    thunk_ordinals "$dll" | sort -u > "$OUT/$stem.have"
    modules=$((modules + 1))
    # For every name the .spec pins AND the thunk actually exports, the two
    # ordinals must agree.  Names the thunk refused are not asserted here --
    # their numbers are holes on purpose, which leg C's step 5 covers.
    awk -F'\t' -v mod="$stem" '
        NR == FNR { want[$2] = $1; next }
        ($2 in want) {
            n++
            if (want[$2] != $1)
                printf "%s: %s is ordinal %s, the .spec pins %s\n", mod, $2, $1, want[$2]
        }
        END { printf "COUNT %d\n", n }
    ' "$OUT/$stem.pinned" "$OUT/$stem.have" > "$OUT/$stem.cmp"
    n=$(sed -n 's/^COUNT //p' "$OUT/$stem.cmp")
    checked=$((checked + n))
    if grep -qv '^COUNT ' "$OUT/$stem.cmp"; then
        bad "$stem publishes ordinals its .spec does not pin:"
        grep -v '^COUNT ' "$OUT/$stem.cmp" | head -10 | sed 's/^/  ordinals| /' >&2
        mismatched=$((mismatched + 1))
    fi
done
# A floor, not a target.  Most pinned names never reach a thunk at all -- 366
# of shlwapi's are -noname and every one of its 360 eligible exports is refused
# as variadic or int128, so that module contributes nothing here and honestly
# should.  What the floor guards against is a build tree with no thunks in it,
# where an all-green leg would mean "nothing was compared".  The d3d12 check
# below is the sharp one: those are the two ordinals the defect was found on.
if [ "$checked" -lt 400 ] || [ "$modules" -lt 5 ]; then
    bad "only $checked pinned ordinal(s) cross-checked across $modules module(s)\
 -- that is too few for this leg to mean anything; is the build tree populated?"
else
    say "export tables: $checked pinned ordinal(s) across $modules module(s) \
agree with their .spec, $mismatched module(s) disagree"
fi
for want in "101	D3D12CreateDevice" "102	D3D12GetDebugInterface"; do
    if [ -f "$OUT/d3d12.have" ] && grep -qF "$want" "$OUT/d3d12.have"; then
        say "export tables: d3d12 answers to $(echo "$want" | tr '\t' ' ')"
    else
        bad "the d3d12 thunk does not export '$want' -- the exact binding \
Steam's d3ddriverquery64.exe asks for"
    fi
done

# ---- C+D: the guest run and its transcript ----------------------------------
run_wine "$OUT/guest.exe" > "$OUT/guest.out" 2> "$OUT/guest.err"
rc=$?
if [ "$rc" != 0 ]; then
    bad "the guest probe exited $rc"
    tail -20 "$OUT/guest.err" | sed 's/^/  guest| /' >&2
fi
if grep -q "ordinal_import: PASS" "$OUT/guest.out"; then
    say "guest: the probe reports PASS"
else
    bad "the guest probe did not report PASS"
fi
sed 's/^/  guest| /' "$OUT/guest.out"

cat > "$OUT/expected.out" <<EOF
ordinal_import: probe start
step 1 ordinal 101 bound: ok
step 2 GetModuleHandleW(d3d12.dll): ok
step 3 GetProcAddress(D3D12CreateDevice): ok
step 4 ordinal 101 is the same export as the name: ok
step 5 bogus ordinal $BOGUS bound to a sentinel: ok
ordinal_import: PASS
EOF
if cmp -s "$OUT/expected.out" "$OUT/guest.out"; then
    say "transcript: byte-identical to the expected transcript"
else
    bad "the transcript differs from the expected one:"
    diff -u "$OUT/expected.out" "$OUT/guest.out" | sed 's/^/  diff| /' >&2
fi

# ---- E: the unresolvable ordinal is named at bind time ----------------------
timeout -k 5 "$TIMEOUT" env WINEDEBUG="warn+module" \
    WINEDLLOVERRIDES="winedbg.exe=d" "$BUILD/wine" "$OUT/guest.exe" \
    > /dev/null 2> "$OUT/bind.err"
if grep -q "No implementation for d3d12.dll.$BOGUS" "$OUT/bind.err"; then
    say "bind: the loader names the ordinal it cannot serve: \
$(grep -m1 -o "No implementation for d3d12.dll.$BOGUS[^,]*" "$OUT/bind.err" | cut -c1-80)"
else
    bad "nothing in warn+module named the unserved ordinal d3d12.dll.$BOGUS"
    grep -i "d3d12" "$OUT/bind.err" | head -5 | sed 's/^/  bind| /' >&2
fi

# ---- F: calling the sentinel kills the process ------------------------------
if build_probe "$OUT/bogus.exe" -DORDINAL_PROBE_CALL_BOGUS; then
    run_wine "$OUT/bogus.exe" > "$OUT/bogus.out" 2> "$OUT/bogus.err"
    brc=$?
    if [ "$brc" = 0 ]; then
        bad "calling the missing-import sentinel exited 0; it must be fatal"
    elif grep -q "the sentinel call RETURNED" "$OUT/bogus.out"; then
        bad "the sentinel CALL RETURNED -- a missing import bound to something\
 callable"
    else
        say "call: the sentinel call killed the process (exit $brc), and the \
port said: $(grep -m1 -i "dead0\|Unhandled\|c0000005" "$OUT/bogus.err" | cut -c1-100)"
        if ! grep -qi "dead0\|Unhandled\|c0000005" "$OUT/bogus.err"; then
            bad "the process died, but silently -- nothing in stderr named the\
 fault, so a real one would be undiagnosable"
        fi
    fi
else
    bad "the CALL_BOGUS build failed; leg F proves nothing"
fi

# ---- G: the same call in the shape a live Steam launch has -----------------
# A crash reporter that faults while reporting is not a hypothetical: DOOM's
# did, on 2026-08-17, on top of a missing-import sentinel, and the port
# restarted the whole unhandled report for each fault until the thread's 8 MiB
# native stack was gone -- then spun at 0% holding a critical section another
# thread was waiting on.  The process HUNG.  So this leg asserts on the CLOCK
# as well as the exit status: a death that takes as long as a hang is a hang.
CRASH_TIMEOUT=${CRASH_TIMEOUT:-45}
if build_probe "$OUT/crashfilter.exe" -DORDINAL_PROBE_CRASHING_FILTER; then
    t0=$(date +%s)
    timeout -k 5 "$CRASH_TIMEOUT" env WINEDEBUG="$WDBG" \
        WINEDLLOVERRIDES="winedbg.exe=d" "$BUILD/wine" "$OUT/crashfilter.exe" \
        > "$OUT/crashfilter.out" 2> "$OUT/crashfilter.err"
    crc=$?
    elapsed=$(( $(date +%s) - t0 ))
    if [ "$crc" = 124 ] || [ "$crc" = 137 ]; then
        bad "a wild-pointer call with a faulting top-level filter did not die \
within ${CRASH_TIMEOUT}s -- that is the hang this leg exists for"
    elif [ "$crc" = 0 ]; then
        bad "the crashing-filter run exited 0; it must be fatal"
    elif grep -q "the sentinel call RETURNED" "$OUT/crashfilter.out"; then
        bad "the sentinel CALL RETURNED under the crashing filter"
    else
        say "crash filter: died in ${elapsed}s (exit $crc) instead of recursing"
    fi
    # The promise the sentinel design makes is that the death NAMES the symbol.
    if grep -qi "wild pointer\|dead0\|No implementation" "$OUT/crashfilter.err"; then
        say "crash filter: the port named the wild pointer: $(grep -m1 -io \
            "guest called through a wild pointer[^;]*" "$OUT/crashfilter.err" \
            | cut -c1-90)"
    else
        bad "the crashing-filter run died without naming the sentinel anywhere \
in stderr -- the death is undiagnosable, which is the whole point of the design"
    fi
    # And it must have stopped ITSELF rather than been stopped by the stack
    # running out: one report per thread, said out loud.
    if grep -q "still running on this thread" "$OUT/crashfilter.err"; then
        say "crash filter: the re-entrancy guard fired and said so"
    else
        note "the re-entrancy guard did not fire; the filter's own fault was \
handled at guest level this time, which is a pass but proves less"
    fi
    if grep -qi "STACK_OVERFLOW\|c00000fd" "$OUT/crashfilter.err"; then
        bad "the native stack overflowed -- the recursion happened anyway, it \
just ended sooner than a hang"
    fi
else
    bad "the CRASHING_FILTER build failed; leg G proves nothing"
fi

[ "$fail" = 0 ] && say "PASS"
exit "$fail"
