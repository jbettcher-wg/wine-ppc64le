#!/bin/sh
#
# check-rip-cache.sh -- the gate for the guest-to-native thunk target cache.
#
# THE CLAIM.  find_guest_thunk_target() in dlls/ntdll/signal_ppc64.c answers
# "which native function does this trapping guest address stand for" out of a
# direct-mapped cache, read WITHOUT THE LOADER LOCK, for both flat imports and
# COM vtable slots.  The claim this gate makes is the only one that matters
# about a cache: THAT IT CHANGES NOTHING.  A run with the cache and a run
# without it must produce the same bytes, from several threads at once, with
# COM traffic in the mix -- and the cache must actually be being used, because
# a cache that silently never hits also "changes nothing" and would pass a
# weaker gate while the port paid the full lookup and the full lock on every
# crossing.
#
# WHY IT NEEDS A GATE THAT IS NOT ONE OF THE OTHERS.  Every other gate here
# runs guest code and so exercises this cache incidentally -- which is exactly
# the problem: incidental coverage of a cache proves the WARM path and never
# compares it to the cold one.  The two failure modes this mechanism has are
# both invisible to a gate that only asks "did the program work":
#
#   * A WRONG ENTRY.  The cache is keyed by trapping address; hand one call
#     site's resolution to another and the guest calls a real native function
#     with another function's arguments.  Nothing crashes.  A number is wrong.
#   * A PUBLICATION RACE.  Entries are written under the loader lock and read
#     with none, so a reader can be inside a slot while a writer fills it.  The
#     sequence-number protocol is what makes that safe, and a missing fence in
#     it is right on every machine it is tested on and wrong on the next one.
#
# Layers:
#
#   1  BUILD: the guest PE compiles, links, and imports exactly the three DLLs
#      this probe claims -- kernel32, user32 and ole32.  The COM half of the
#      gate is only real if ole32 is genuinely in the import table.
#   2  VALUES, cache ON: the probe runs under the emulator and prints PASS with
#      wrong=0.  Every step checks a value against a compile-time answer or a
#      relation; see probes/thunk_cache.c.
#   3  VALUES, cache OFF (WINEEMUNORIPCACHE=1): the same probe, and the
#      transcript must be BYTE-IDENTICAL to layer 2's.  This is the whole
#      claim, stated as a diff.
#   4  THE PORT'S OWN VIEW: a WINEDEBUG=+seh run must show
#      thunk_rip_cache_get()'s "thunk cache hit for %p" TRACE many times -- the
#      floor is derived from the probe's OWN reported check count rather than
#      hard-coded, since the number of crossings per check is the port's
#      business and not this script's to predict -- and the same run with
#      WINEEMUNORIPCACHE=1 must show it EXACTLY ZERO times.  Together those two
#      say the fast path exists, is taken, and is what the lever turns off.
#
# --sabotage runs the negative controls instead and requires each to go red.
# There are two, and the first is the one with something to prove:
#
#   sabotage(blind)  WINEEMURIPCACHEBLIND=1 keeps the cache but removes what
#                    makes it SAFE -- every address is forced into slot zero
#                    and the key is not compared -- so the second distinct call
#                    site gets the first one's function.  The probe must NOT
#                    print PASS.  WINEEMUNORIPCACHE alone could never do this:
#                    turning a cache off can only cost time, never correctness,
#                    so a gate built on it alone would be red for a mechanism
#                    that was never at risk.
#   sabotage(lever)  WINEEMUNORIPCACHE=1 must make layer 4's "the cache is
#                    being used" assertion fail.  A lever that had quietly
#                    stopped working would leave every other leg of this gate
#                    passing for the wrong reason.
#   sabotage(argsign) WINEEMUNOARGSIGN=1 leaves the widths alone and
#                     zero-extends the signed ones, so it reds only if
#                     SIGNEDNESS is what carries a negative sub-word argument
#                     -- which turning narrowing off altogether cannot show.
#   sabotage(argwidth) WINEEMUNOARGWIDTH=1 puts back the rule that cut every
#                    argument narrower than a pointer to 32 bits rather than
#                    to its own width, and the probe must fail.  Neither of the
#                    controls above can reach this: they are about whether the
#                    cache answers and which address it answers for, and this
#                    is about what a correct answer CONTAINS -- the width word
#                    rides the cached entry, so it is also the leg that proves
#                    the entry carries it across a cache HIT.
#
# A gate that cannot go red proves nothing.
#
# Exit 0 = pass, 1 = a check failed, 2 = could not run (not a pass).
#
# Copyright 2026 the ppc64le port authors
#
# This library is free software; you can redistribute it and/or
# modify it under the terms of the GNU Lesser General Public
# License as published by the Free Software Foundation; either
# version 2.1 of the License, or (at your option) any later version.
set -u

HERE=$(cd "$(dirname "$0")" && pwd)
SRC=$(cd "$HERE/.." && pwd)
SRC=$(cd "$SRC/.." && pwd)
BUILD=${BUILD:-$SRC}
OUT=${OUT:-/tmp/thunk-cache}
SABOTAGE=0
[ "${1:-}" = "--sabotage" ] && SABOTAGE=1

say()  { echo "check-rip-cache: $*"; }
bad()  { echo "check-rip-cache: FAIL $*" >&2; fail=1; }
note() { echo "check-rip-cache: note $*"; }
skip() { echo "check-rip-cache: $*" >&2; exit 2; }

[ -x "$BUILD/wine" ] || skip "no wine loader at $BUILD/wine"
[ -n "${WINEPREFIX:-}" ] || skip "set WINEPREFIX to a prefix wineboot has run in"
[ -n "${WINEFEXBRIDGE:-}" ] || skip "set WINEFEXBRIDGE to the emulator bridge"
for m in kernel32 user32 ole32 oleaut32; do
    [ -f "$BUILD/dlls/$m/x86_64-windows/$m.dll" ] || \
        skip "no guest $m thunk; build it first"
done
command -v clang >/dev/null || skip "need clang for the guest build"
command -v llvm-dlltool >/dev/null || skip "need llvm-dlltool for the guest build"
command -v llvm-readobj >/dev/null || skip "need llvm-readobj to read the built image"

mkdir -p "$OUT" || skip "cannot create $OUT"
fail=0

INCL="-I$BUILD/include -I$SRC/include -I$SRC/include/msvcrt"
TIMEOUT=${TIMEOUT:-180}

# ---- build: the x86-64 guest PE ------------------------------------------
cat > "$OUT/kernel32.def" <<'EOF'
LIBRARY kernel32.dll
EXPORTS
GetStdHandle
WriteFile
ExitProcess
SetLastError
GetLastError
GetEnvironmentVariableA
CreateThread
WaitForSingleObject
CloseHandle
lstrlenA
lstrcmpA
MulDiv
GetActiveProcessorCount
GetActiveProcessorGroupCount
EOF
cat > "$OUT/user32.def" <<'EOF'
LIBRARY user32.dll
EXPORTS
IsCharAlphaA
CharUpperA
EOF
cat > "$OUT/ole32.def" <<'EOF'
LIBRARY ole32.dll
EXPORTS
CoInitializeEx
CoUninitialize
CoGetMalloc
EOF
# oleaut32 is here for the SIGNED sub-word crossing and nothing else.  Its
# VarI4From* conversions are a single assignment natively, so the answer is
# the argument -- see the block in thunk_cache.c that calls them.
cat > "$OUT/oleaut32.def" <<'EOF'
LIBRARY oleaut32.dll
EXPORTS
VarI4FromBool
VarI4FromI2
EOF
for m in kernel32 user32 ole32 oleaut32; do
    llvm-dlltool -m i386:x86-64 -d "$OUT/$m.def" -l "$OUT/lib$m.a" \
        || skip "llvm-dlltool failed for $m"
done

GUESTCC="clang -target x86_64-windows-gnu -nostdlibinc $INCL -fms-extensions \
-D_UCRT -DCOBJMACROS -Wall -O1 -fno-builtin -g"
GUESTLD="clang -target x86_64-windows-gnu -fuse-ld=lld -nostdlib \
-Wl,--entry=thunk_cache_entry -Wl,--subsystem,console"

$GUESTCC -c -o "$OUT/thunk_cache.o" "$HERE/probes/thunk_cache.c" \
    || skip "guest compile failed"
$GUESTLD -o "$OUT/thunk_cache.exe" "$OUT/thunk_cache.o" \
    "$OUT/libkernel32.a" "$OUT/libuser32.a" "$OUT/libole32.a" \
    "$OUT/liboleaut32.a" \
    || skip "guest link failed"

EXE="$OUT/thunk_cache.exe"

# ---- 1: the import table really names all three modules ------------------
# Cheap, and the layer that catches "the COM half never linked" passing every
# layer below for the wrong reason -- an ole32-less probe would still print
# PASS, having quietly skipped exactly the crossings this cache serves worst.
imports=$(llvm-readobj --coff-imports "$EXE" 2>/dev/null | \
          sed -n 's/^ *Name: \(.*\)$/\1/p' | tr 'A-Z' 'a-z' | sort -u)
for m in kernel32.dll user32.dll ole32.dll oleaut32.dll; do
    case "$imports" in
        *"$m"*) ;;
        *) bad "layer 1: $EXE does not import $m (got: $(echo $imports))" ;;
    esac
done
[ "$fail" = 0 ] && say "layer 1: imports kernel32, user32, ole32 and oleaut32"

run_probe() {   # run_probe <logfile> [env assignments...]
    _log=$1; shift
    env "$@" timeout "$TIMEOUT" "$BUILD/wine" "$EXE" > "$_log" 2>"$_log.err"
    echo $?
}

# ---- the sabotage legs ---------------------------------------------------
if [ "$SABOTAGE" = 1 ]; then
    sfail=0

    rc=$(run_probe "$OUT/blind.out" WINEEMURIPCACHEBLIND=1)
    if [ "$rc" = 0 ] && grep -q '^PASS$' "$OUT/blind.out"; then
        echo "check-rip-cache: SABOTAGE FAIL blind: the probe still passed with \
the key comparison removed -- the cache is not being consulted at all, or the \
lever no longer reaches it" >&2
        sfail=1
    else
        say "sabotage(blind): the probe failed with a blind cache, as it must (rc=$rc)"
    fi

    rc=$(run_probe "$OUT/lever.out" WINEEMUNORIPCACHE=1 WINEDEBUG=+seh TC_ITERATIONS=5)
    hits=$(grep -c "thunk cache hit for" "$OUT/lever.out.err" 2>/dev/null || true)
    if [ "${hits:-0}" -gt 0 ]; then
        echo "check-rip-cache: SABOTAGE FAIL lever: WINEEMUNORIPCACHE=1 still \
produced $hits cache hits -- the negative control does not control anything" >&2
        sfail=1
    else
        say "sabotage(lever): the lever silenced every cache hit, as it must"
    fi

    # The third control, and the one the other two cannot stand in for.
    # WINEEMUNORIPCACHE turns the cache OFF and WINEEMURIPCACHEBLIND makes it
    # answer for the wrong address; neither touches what a correct answer
    # CONTAINS.  WINEEMUNOARGWIDTH puts back the rule that cut every sub-word
    # argument to 32 bits, so the probe's WORD argument arrives with the
    # caller's leftovers still above its own width -- which is the live bug
    # this leg was added for, reproduced on demand.
    rc=$(run_probe "$OUT/width.out" WINEEMUNOARGWIDTH=1 TC_ITERATIONS=5)
    if [ "$rc" = 0 ] && grep -q '^PASS$' "$OUT/width.out"; then
        echo "check-rip-cache: SABOTAGE FAIL argwidth: the probe still passed with \
sub-word arguments cut to 32 bits -- the width word is not reaching the \
marshaller, or nothing in the probe depends on it" >&2
        sfail=1
    else
        say "sabotage(argwidth): the probe failed with sub-word arguments cut to 32 bits, as it must (rc=$rc)"
    fi

    # The fourth control, and the one WINEEMUNOARGWIDTH cannot stand in for:
    # turning narrowing off entirely would hide a signedness bug behind a
    # width bug.  WINEEMUNOARGSIGN leaves every width alone and zero-extends
    # the signed ones, so this leg reds if and only if the SIGN bit is what
    # carries a negative sub-word argument across.  Measured: without it
    # VarI4FromBool(VARIANT_TRUE) answers 65535 instead of -1.
    rc=$(run_probe "$OUT/sign.out" WINEEMUNOARGSIGN=1 TC_ITERATIONS=5)
    if [ "$rc" = 0 ] && grep -q '^PASS$' "$OUT/sign.out"; then
        echo "check-rip-cache: SABOTAGE FAIL argsign: the probe still passed with \
signed sub-word arguments zero-extended -- the sign word is not reaching the \
marshaller, or nothing in the probe depends on it" >&2
        sfail=1
    else
        say "sabotage(argsign): the probe failed with signed sub-word arguments \
zero-extended, as it must (rc=$rc)"
    fi

    [ $sfail -eq 0 ] && say "SABOTAGE PASS (all four controls red)"
    exit $sfail
fi

# ---- 2: values, cache ON -------------------------------------------------
rc=$(run_probe "$OUT/on.out")
if [ "$rc" != 0 ]; then
    bad "layer 2: the probe exited $rc with the cache on"
    sed 's/^/      | /' "$OUT/on.out" >&2
    sed 's/^/      # /' "$OUT/on.out.err" | tail -20 >&2
else
    grep -q '^PASS$' "$OUT/on.out" || bad "layer 2: no PASS with the cache on"
    grep -q ' wrong=0$' "$OUT/on.out" || \
        bad "layer 2: the probe reported wrong answers: $(grep wrong= "$OUT/on.out")"
    say "layer 2: $(grep '^threads=' "$OUT/on.out")"
fi

# ---- 3: values, cache OFF, and the two transcripts must not differ -------
rc=$(run_probe "$OUT/off.out" WINEEMUNORIPCACHE=1)
if [ "$rc" != 0 ]; then
    bad "layer 3: the probe exited $rc with WINEEMUNORIPCACHE=1"
    sed 's/^/      | /' "$OUT/off.out" >&2
elif ! cmp -s "$OUT/on.out" "$OUT/off.out"; then
    bad "layer 3: the cached and uncached transcripts differ -- the cache is \
NOT transparent, which is the one thing it has to be"
    diff "$OUT/off.out" "$OUT/on.out" | sed 's/^/      | /' >&2
else
    say "layer 3: cached and uncached transcripts are byte-identical"
fi

# ---- 4: the port's own view ----------------------------------------------
# Fewer iterations: +seh prints a line per crossing, and this leg is counting
# them rather than racing them.
#
# WINE_PPC64LE_NO_EC=1 is PINNED here, deliberately: with EC transitions
# armed, every registered stub's warm crossing is served by its own row CELL
# (see "EC row cells" in signal_ppc64.c) and never consults the shared cache
# at all -- this layer would then count almost no hits and conclude, wrongly,
# that the fast path is broken.  The shared cache still serves every non-EC
# path, and layer 4b below asserts the cells with the same shape.
rc=$(run_probe "$OUT/trace-on.out" WINE_PPC64LE_NO_EC=1 WINEDEBUG=+seh TC_ITERATIONS=20)
crossings=$(sed -n "s/.* crossings=\([0-9]*\) .*/\1/p" "$OUT/trace-on.out" | tail -1)
hits=$(grep -c "thunk cache hit for" "$OUT/trace-on.out.err" 2>/dev/null || true)
if [ "$rc" != 0 ] || [ -z "$crossings" ]; then
    bad "layer 4: the traced run exited $rc without reporting its crossing count"
else
    # The floor: only the FIRST crossing at each distinct call site can miss,
    # and this probe has well under a hundred distinct call sites (twelve
    # imported functions plus the loader's own).  Half the crossing count is
    # therefore a floor with a very wide margin -- wide on purpose, because how
    # many traps a given call actually costs belongs to the port and is not
    # this script's to predict.
    floor=$((crossings / 2))
    if [ "${hits:-0}" -lt "$floor" ]; then
        bad "layer 4: only ${hits:-0} cache hits for $crossings crossings (floor \
$floor) -- the fast path is not being taken"
    else
        say "layer 4: $hits cache hits for $crossings crossings (floor $floor)"
    fi
fi

rc=$(run_probe "$OUT/trace-off.out" WINE_PPC64LE_NO_EC=1 WINEEMUNORIPCACHE=1 WINEDEBUG=+seh TC_ITERATIONS=20)
hits_off=$(grep -c "thunk cache hit for" "$OUT/trace-off.out.err" 2>/dev/null || true)
if [ "${hits_off:-0}" != 0 ]; then
    bad "layer 4: WINEEMUNORIPCACHE=1 still produced $hits_off cache hits"
else
    say "layer 4: the lever produced no cache hits at all"
fi

# ---- 4b: the EC row cells, same shape as layer 4 --------------------------
# Only meaningful when the bridge has EC targets (ABI 7): a warm transitioned
# crossing must be served by its stub's own cell ("ec cell hit for"), and
# WINEEMUNORIPCACHE must silence the cells too -- the cache levers WIN over
# the cells, or layers 1-4 prove nothing about a run where cells serve.
if nm -D "$WINEFEXBRIDGE" 2>/dev/null | grep -q fexbridge_register_ec_target; then
    rc=$(run_probe "$OUT/trace-ec.out" WINEDEBUG=+seh TC_ITERATIONS=20)
    crossings=$(sed -n "s/.* crossings=\([0-9]*\) .*/\1/p" "$OUT/trace-ec.out" | tail -1)
    cell_hits=$(grep -c "ec cell hit for" "$OUT/trace-ec.out.err" 2>/dev/null || true)
    if [ "$rc" != 0 ] || [ -z "$crossings" ]; then
        bad "layer 4b: the ec-traced run exited $rc without reporting its crossing count"
    else
        floor=$((crossings / 2))
        if [ "${cell_hits:-0}" -lt "$floor" ]; then
            bad "layer 4b: only ${cell_hits:-0} ec cell hits for $crossings crossings (floor \
$floor) -- the row cells are not serving warm transitions"
        else
            say "layer 4b: $cell_hits ec cell hits for $crossings crossings (floor $floor)"
        fi
    fi
    rc=$(run_probe "$OUT/trace-ec-off.out" WINEEMUNORIPCACHE=1 WINEDEBUG=+seh TC_ITERATIONS=20)
    cell_hits_off=$(grep -c "ec cell hit for" "$OUT/trace-ec-off.out.err" 2>/dev/null || true)
    if [ "${cell_hits_off:-0}" != 0 ]; then
        bad "layer 4b: WINEEMUNORIPCACHE=1 still produced $cell_hits_off ec cell hits -- the levers no longer win over the cells"
    else
        say "layer 4b: WINEEMUNORIPCACHE silences the cells too (the levers win)"
    fi
else
    say "layer 4b SKIP: $WINEFEXBRIDGE has no EC targets (bridge ABI < 7); the row cells cannot serve here"
fi

# A crossing that resolved to the wrong function would most often surface as a
# swallowed exception rather than a wrong number, and this gate must not read
# that as success.
if grep -qE "ignoring exception|c000001d|unhandled guest trap" "$OUT/on.out.err"; then
    bad "layer 4: the clean run's stderr names a swallowed or unhandled fault"
    grep -E "ignoring exception|c000001d|unhandled guest trap" "$OUT/on.out.err" | \
        head -5 | sed 's/^/      | /' >&2
fi

[ $fail -eq 0 ] && say "PASS"
exit $fail
