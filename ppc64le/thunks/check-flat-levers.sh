#!/bin/sh
#
# check-flat-levers.sh -- the gate on the FLAT source-tier kill switches.
#
# WHY THIS GATE EXISTS.  Commit 1edc93608b6 ("spec2thunk: the
# source-definition tier") turned 2,624 previously-refused flat exports into
# served ones in one commit -- ucrtbase +417, kernelbase +340, msvcrt +309,
# msvcr120 +284, ntdll +105 -- with no runtime way to put any of them back.
# One wrongly-inferred signature on a hot CRT export is this tree's sub-word /
# wrong-width ABI failure class, and the Witcher 3 load regression
# (ppc64le/docs/sessions/2026-09-01/w3-load-regression-bisect.md) could not be
# tested against a tier-off world at all short of rebuilding the tree with the
# tier ripped out.  The three levers this file tests are the flat lane's
# answer, deliberately the same three grains and the same idiom as the COM
# lane's (ppc64le/winecom/check-com-levers.sh):
#
#   WINEEMUNOFLATTIER=source   every source-tier row in every module
#   WINEEMUNOFLATMODS=a,b,...  source-tier rows of the named guest DLLs only
#   WINEEMUNOFLATROWS=m!E,...  individual exports, or `@/path/file`
#
# A LEVER NOBODY CAN PROVE IS WORSE THAN NO LEVER: a bisect leg run under a
# lever that silently did nothing is recorded as "tested, clean", and the
# conclusion drawn from it is wrong in the most expensive direction.  So every
# arm here is a PAIR -- armed and unarmed -- and the two must print different
# things.
#
# THE OBSERVABLE is ppc64le/thunks/probes/flat_lever_smoke.c's per-probe
# `<tag>=served|null|nomodule`, read through GetProcAddress.  That is not a
# convenience: the state a lever restores is "the generator never emitted this
# export", whose two runtime faces are a 0xdead0000+n sentinel for an import
# and NULL for a GetProcAddress.  The second is the one that can be printed
# instead of crashing.  See the probe's own banner.
#
# THE PROBE NAMES ARE DERIVED, NEVER TYPED.  ppc64le/thunks/flat-tier-rows.py
# reads descriptor bit 10 (THUNK_SIG_SRCTIER) out of the BUILT guest DLLs, so
# the gate probes rows that really are source-tier in the artifact under test.
# A hard-coded name would drift from the tier silently and every arm below
# would then be measuring nothing.  (The build never passes spec2thunk
# --report, which is why the bit had to be in the row at all.)
#
# Legs:
#
#   A  BUILD: the tier reader works and the guest probe compiles and links.
#   B  BASELINE / SABOTAGE: with NO lever every probed export SERVES.  If one
#      refuses here, every armed arm below is measuring something else.
#   C  TIER: WINEEMUNOFLATTIER=source -> every source-tier probe answers null,
#      the header-tier control still serves, and the arming line says so per
#      module with a count.
#   D  MODS: WINEEMUNOFLATMODS=<one module> -> that module's source-tier probe
#      answers null and the OTHER module's still serves.  The half that makes
#      a binary search mean anything is the second one.
#   E  ROWS: WINEEMUNOFLATROWS=module!Export -> that one export answers null
#      and its module-mate still serves.  Plus the bare `Export` spelling,
#      which must also say WHICH module it matched, and the `@file` form.
#   F  TYPO: names that match nothing -- a bad export, a bad module, a bad
#      tier value, and a name that resolves to a HEADER-tier row -- must force
#      NOTHING and must SAY SO.  A typo in a bisect leg must never pass as
#      "tested".
#   G  HEADER-TIER IMMUNITY: a header-tier export named to WINEEMUNOFLATROWS
#      keeps serving.  The header tier is not what landed and is not a restore
#      target; a lever that could reach it would let a leg "clear" the source
#      tier while testing something else.
#
# --sabotage runs the negative controls alone (legs B and F) and requires each
# to show the lever NOT firing.  A gate that cannot go red proves nothing.
#
# Exit 0 = pass, 1 = a check failed, 2 = could not run at all (a skip is NOT a
# pass).
#
# WINEDLLOVERRIDES=winedbg.exe=d on every run, verbatim from
# check-com-levers.sh and for the identical reason: a bringup prefix has
# AeDebug pointed at "winedbg --auto", so a run that ends in an unhandled
# fault would start the debugger, which attaches and never lets go, turning
# every red state of this gate into a hang.
set -u

HERE=$(cd "$(dirname "$0")" && pwd)
SRC=$(cd "$HERE/../.." && pwd)
BUILD=${BUILD:-$SRC}
OUT=${OUT:-/tmp/flat-levers}
TIMEOUT=${TIMEOUT:-120}
SABOTAGE=0
[ "${1:-}" = "--sabotage" ] && SABOTAGE=1

say()  { echo "check-flat-levers: $*"; }
bad()  { echo "check-flat-levers: FAIL $*" >&2; fail=1; }
note() { echo "check-flat-levers: note $*"; }
skip() { echo "check-flat-levers: $*" >&2; exit 2; }

[ -x "$BUILD/wine" ] || skip "no wine loader at $BUILD/wine"
[ -n "${WINEPREFIX:-}" ] || skip "set WINEPREFIX to a prefix wineboot has run in"
[ -n "${WINEFEXBRIDGE:-}" ] || skip "set WINEFEXBRIDGE to the emulator bridge"
command -v python3 >/dev/null || skip "need python3 for the tier reader"
command -v clang >/dev/null || skip "need clang for the guest build"
command -v llvm-dlltool >/dev/null || skip "need llvm-dlltool for the guest build"

MODA=${MODA:-ucrtbase}
MODB=${MODB:-kernelbase}
DLLA="$BUILD/dlls/$MODA/x86_64-windows/$MODA.dll"
DLLB="$BUILD/dlls/$MODB/x86_64-windows/$MODB.dll"
[ -f "$DLLA" ] || skip "no guest thunk at $DLLA; build it first"
[ -f "$DLLB" ] || skip "no guest thunk at $DLLB; build it first"

mkdir -p "$OUT" || skip "cannot create $OUT"
fail=0

# ---- A: the tier reader, and the names every arm below depends on ----------
TIER="$HERE/flat-tier-rows.py"
[ -f "$TIER" ] || skip "no $TIER"

python3 "$TIER" --count "$DLLA" "$DLLB" > "$OUT/counts.txt" 2>"$OUT/tier.err" \
    || { sed 's/^/  tier| /' "$OUT/tier.err" >&2; skip "the tier reader failed"; }
sed 's/^/  /' "$OUT/counts.txt"

# The FIRST source-tier row of each module, and the first header-tier row of
# MODA.  First rather than a hand-picked name for the reason in the banner:
# whatever the artifact says is source-tier is what gets probed.
SRCA=$(python3 "$TIER" --plain "$DLLA" | head -1)
SRCA2=$(python3 "$TIER" --plain "$DLLA" | sed -n 2p)
SRCB=$(python3 "$TIER" --plain "$DLLB" | head -1)
HDRA=$(python3 "$TIER" --plain --header-tier "$DLLA" | head -1)

[ -n "$SRCA" ]  || skip "$MODA has no source-tier rows at all; nothing to test"
[ -n "$SRCA2" ] || skip "$MODA has only one source-tier row; the rows arm needs two"
[ -n "$SRCB" ]  || skip "$MODB has no source-tier rows at all; nothing to test"
[ -n "$HDRA" ]  || skip "$MODA has no header-tier rows; the immunity arm needs one"
say "probes: srca=$MODA!$SRCA srca2=$MODA!$SRCA2 srcb=$MODB!$SRCB hdra=$MODA!$HDRA"

cat > "$OUT/flat_lever_names.h" <<EOF
/* generated by check-flat-levers.sh from the BUILT thunk DLLs' tier bits */
FLAT_PROBE( "srca",  "$MODA.dll",  "$SRCA"  )
FLAT_PROBE( "srca2", "$MODA.dll",  "$SRCA2" )
FLAT_PROBE( "srcb",  "$MODB.dll",  "$SRCB"  )
FLAT_PROBE( "hdra",  "$MODA.dll",  "$HDRA"  )
EOF

cat > "$OUT/kernel32.def" <<'EOF'
LIBRARY kernel32.dll
EXPORTS
GetStdHandle
WriteFile
ExitProcess
LoadLibraryA
GetProcAddress
EOF
llvm-dlltool -m i386:x86-64 -d "$OUT/kernel32.def" -l "$OUT/libkernel32.a" \
    || skip "llvm-dlltool failed"

clang -target x86_64-windows-gnu -nostdlibinc -I"$OUT" \
    -I"$BUILD/include" -I"$SRC/include" -I"$SRC/include/msvcrt" \
    -Wall -O1 -fno-builtin -g -c -o "$OUT/probe.o" \
    "$HERE/probes/flat_lever_smoke.c" \
    2>"$OUT/build.err" || { sed 's/^/  build| /' "$OUT/build.err" >&2;
                            skip "the guest probe did not compile"; }
clang -target x86_64-windows-gnu -fuse-ld=lld -nostdlib \
    -Wl,--entry=flat_lever_entry -Wl,--subsystem,console \
    -o "$OUT/probe.exe" "$OUT/probe.o" "$OUT/libkernel32.a" \
    2>>"$OUT/build.err" || { sed 's/^/  build| /' "$OUT/build.err" >&2;
                             skip "the guest probe did not link"; }
say "build: $OUT/probe.exe"

run() {
    tag=$1; shift
    timeout -k 5 "$TIMEOUT" \
        env WINEDLLOVERRIDES="winedbg.exe=d" "$@" \
        "$BUILD/wine" "$OUT/probe.exe" \
        > "$OUT/$tag.out" 2>"$OUT/$tag.err"
    rc=$?
    if [ $rc -eq 124 ] || [ $rc -eq 137 ]; then
        bad "the $tag run HUNG (killed after ${TIMEOUT}s)"
        return 1
    fi
    grep -q "flat_lever_smoke: done" "$OUT/$tag.out" && return 0
    sed 's/^/  '"$tag"'| /' "$OUT/$tag.out" >&2
    tail -15 "$OUT/$tag.err" | sed 's/^/  '"$tag"'| /' >&2
    bad "the $tag run never reached its own last line"
    return 1
}

field() { grep -o "^$2=[a-z]*" "$OUT/$1.out" | head -1 | cut -d= -f2; }

# want <tag> <probe> <expected> <why>
want() {
    got=$(field "$1" "$2")
    if [ "$got" = "$3" ]; then
        say "$1: $2=$3 -- $4"
    else
        bad "$1: wanted $2=$3, got '${got:-<absent>}' -- $4"
    fi
}

# loud <tag> <needle> <why> -- the run must have SAID something naming <needle>
loud() {
    if grep -q -- "$2" "$OUT/$1.err"; then
        say "$1: $(grep -m1 -- "$2" "$OUT/$1.err" | cut -c1-150)"
    else
        tail -20 "$OUT/$1.err" | sed 's/^/  '"$1"'| /' >&2
        bad "$1: nothing in the log names '$2' -- $3"
    fi
}

# ---- the negative controls, also available standalone as --sabotage --------
sabotage() {
    ok=1

    # B: with NO lever every probed export must SERVE.
    if run baseline; then
        for p in srca srca2 srcb hdra; do
            if [ "$(field baseline $p)" != "served" ]; then
                bad "with NO lever set, $p did not serve (=$(field baseline $p)); \
every armed arm below would be measuring something other than the lever"
                ok=0
            fi
        done
        [ $ok = 1 ] && say "sabotage: with no lever all four probes SERVE, so the \
armed arms can mean something"
        # and nothing may be forced when nothing is asked for
        if grep -q "source-tier rows forced back" "$OUT/baseline.err"; then
            bad "an UNARMED run printed an arming line; the lever fired with no \
environment variable set"
            ok=0
        fi
    else ok=0; fi

    # F1: an export name that matches nothing.
    if run typo_row WINEEMUNOFLATROWS="$MODA!NoSuchExportHere"; then
        [ "$(field typo_row srca)" = "served" ] || \
            { bad "an export name matching nothing still forced something"; ok=0; }
        grep -q "NoSuchExportHere" "$OUT/typo_row.err" || \
            { bad "an export name matching nothing passed SILENTLY; a typo in a \
bisect leg would be recorded as 'tested, clean'"; ok=0; }
    else ok=0; fi

    # F2: a module name that names no module.
    if run typo_mod WINEEMUNOFLATMODS="nosuchmodulehere"; then
        [ "$(field typo_mod srca)" = "served" ] || \
            { bad "an unknown module name still forced something"; ok=0; }
        grep -q "nosuchmodulehere" "$OUT/typo_mod.err" || \
            { bad "an unknown module name passed SILENTLY"; ok=0; }
    else ok=0; fi

    # F3: a tier value that names no tier.
    if run typo_tier WINEEMUNOFLATTIER=header; then
        [ "$(field typo_tier srca)" = "served" ] || \
            { bad "WINEEMUNOFLATTIER=header forced something; only 'source' is \
a restore target"; ok=0; }
        grep -q "WINEEMUNOFLATTIER=header" "$OUT/typo_tier.err" || \
            { bad "an unknown tier name passed SILENTLY"; ok=0; }
    else ok=0; fi

    [ "$ok" = 1 ] && say "SABOTAGE PASS"
    [ "$ok" = 1 ]
}

if [ "$SABOTAGE" = 1 ]; then
    sabotage
    exit $?
fi

# ---- B: baseline -----------------------------------------------------------
if run baseline; then
    want baseline srca  served "the source-tier row serves with no lever"
    want baseline srca2 served "its module-mate serves too"
    want baseline srcb  served "the other module's source-tier row serves"
    want baseline hdra  served "the header-tier control serves"
    grep -q "source-tier rows forced back" "$OUT/baseline.err" && \
        bad "an UNARMED run printed an arming line"
fi

# ---- C: the whole tier off -------------------------------------------------
if run tier WINEEMUNOFLATTIER=source; then
    want tier srca  null   "the whole source tier is back to its pre-tier state"
    want tier srca2 null   "...for every row of the module, not just the named one"
    want tier srcb  null   "...and in every module, not just one"
    want tier hdra  served "the HEADER tier is untouched, as it must be"
    loud tier "WINEEMUNOFLAT\* armed" "an armed run must say so once, loudly"
    # the per-module count line is what makes a leg's log evidence
    for m in "$MODA" "$MODB"; do
        if grep -q "^.*$m.dll: [1-9][0-9]* source-tier rows forced back" "$OUT/tier.err"; then
            say "tier: $(grep -m1 "$m.dll: .* forced back" "$OUT/tier.err" | cut -c1-150)"
        else
            bad "no per-module count line for $m.dll; a leg's log cannot prove \
what it tested"
        fi
    done
fi

# ---- D: one module at a time ----------------------------------------------
# The half that makes a binary search mean anything is the SECOND assertion:
# the module NOT named must be completely unaffected.
if run mods WINEEMUNOFLATMODS="$MODA"; then
    want mods srca  null   "the named module's source-tier rows are forced"
    want mods srca2 null   "...all of them"
    want mods srcb  served "the module NOT named is untouched"
    want mods hdra  served "and its header-tier rows are untouched too"
    loud mods "$MODA.dll: " "the named module must print its own count line"
fi
# the `.dll` spelling of the same module must mean the same thing
if run mods_dll WINEEMUNOFLATMODS="$MODA.dll"; then
    want mods_dll srca null "the module.dll spelling names the same module"
fi
# two modules at once, which is what a binary-search leg actually types
if run mods_two WINEEMUNOFLATMODS="$MODA,$MODB"; then
    want mods_two srca null "both named modules are forced..."
    want mods_two srcb null "...at once"
fi

# ---- E: one export at a time ----------------------------------------------
if run rows WINEEMUNOFLATROWS="$MODA!$SRCA"; then
    want rows srca  null   "the single named export is forced"
    want rows srca2 served "its module-mate is NOT -- this is the finest grain"
    want rows srcb  served "and the other module is untouched"
fi
# the bare spelling: allowed, but it must say which module it matched
if run rows_bare WINEEMUNOFLATROWS="$SRCA"; then
    want rows_bare srca null "a bare export name matches in whatever module has it"
    if grep -q "$MODA.dll: 1 source-tier rows forced back" "$OUT/rows_bare.err" ||
       grep -q "bare '$SRCA'.*$MODA" "$OUT/rows_bare.err"; then
        say "rows_bare: $(grep -m1 "$MODA.dll: .* forced back" "$OUT/rows_bare.err" | cut -c1-150)"
    else
        tail -20 "$OUT/rows_bare.err" | sed 's/^/  rows_bare| /' >&2
        bad "a bare export name forced a row without naming the module it \
matched; a bare name that silently spans modules is not a data point"
    fi
fi
# the @file form, with a comment and both spellings in it
{
    echo "# a bisect leg, one target per line"
    echo "$MODA!$SRCA"
    echo "$MODB!$SRCB   # trailing comment"
} > "$OUT/rows.list"
if run rows_file WINEEMUNOFLATROWS="@$OUT/rows.list"; then
    want rows_file srca  null   "the @file spelling resolves the same export"
    want rows_file srcb  null   "and every other line of the file"
    want rows_file srca2 served "and nothing the file does not name"
fi
# an @file that does not exist must be LOUD, not an empty leg
if run rows_nofile WINEEMUNOFLATROWS="@$OUT/definitely-not-here.list"; then
    want rows_nofile srca served "a missing @file forces nothing"
    loud rows_nofile "definitely-not-here" "a missing @file must be said, not \
silently read as an empty list"
fi

# ---- F: the typo warnings --------------------------------------------------
if run typo_row WINEEMUNOFLATROWS="$MODA!NoSuchExportHere"; then
    want typo_row srca served "a name that matches nothing forces nothing"
    loud typo_row "NoSuchExportHere" "a typo in a bisect leg must never pass \
as 'tested'"
fi
if run typo_mod WINEEMUNOFLATMODS="nosuchmodulehere"; then
    want typo_mod srca served "an unknown module forces nothing"
    loud typo_mod "nosuchmodulehere" "an unknown module name must be said"
fi
if run typo_tier WINEEMUNOFLATTIER=header; then
    want typo_tier srca served "only 'source' is a tier this lever can restore"
    loud typo_tier "WINEEMUNOFLATTIER=header" "an unknown tier value must be said"
fi

# ---- G: header-tier immunity ----------------------------------------------
# Naming a header-tier export must force NOTHING and must report as a miss:
# the header tier is not what landed, so it is not a restore target, and a
# lever that reached it would let a leg "clear" the source tier while actually
# testing something else.
if run hdr_immune WINEEMUNOFLATROWS="$MODA!$HDRA"; then
    want hdr_immune hdra served "a header-tier export cannot be forced"
    want hdr_immune srca served "and naming one forces nothing else either"
    loud hdr_immune "$HDRA" "naming a header-tier export must report as a miss, \
not pass silently as a forced row"
fi

[ $fail -eq 0 ] && say "PASS"
exit $fail
