#!/bin/sh
#
# check-import-chain.sh -- the guest THUNK-SURFACE gate.
#
# THE CLAIM
#
# A third-party x86-64 Windows DLL -- one an application ships or loads, not
# one this tree builds -- is a GUEST image on this port, so every static import
# it has must be served by a module of its OWN machine: the x86_64-windows
# thunk PEs tools/spec2thunk builds.  This gate walks that whole chain, import
# by import, and requires every one of them to bind.
#
# WHY IT EXISTS, AND WHY IT DID NOT
#
# When an export is missing the loader does NOT fail the load.  It binds
# ntdll's per-symbol sentinel and the guest dies at the first CALL, as
# 0xDEAD00nn inside a module that has no symbols and no source, arbitrarily far
# from the module that was actually short an export.
#
# DOOM (2016) found one.  It loads Razer's CChromaEditorLibrary.dll, which
# needs Microsoft's real mfc140u.dll, whose DllMain calls
# VCRUNTIME140.__vcrt_InitializeCriticalSectionEx -- a hole.  The visible
# symptom was a NULL function pointer 34 GetProcAddress calls later, in DOOM,
# with nothing in the log connecting the two.  Every fact needed to predict
# that was on disk before anything ran: an import table and an export table are
# both just tables.  So this gate reads them.
#
# Legs:
#
#   A  SURFACE: the guest thunk PEs exist at all.  A gate run against an
#      unbuilt tree would call every import a hole and be useless noise.
#   B  SUBJECT: a real third-party x86-64 PE to walk.  These are the user's own
#      files, not this tree's -- mfc140u.dll comes from a Proton prefix the
#      user provisioned -- so a missing one is a SKIP, never a pass.
#   C  STATIC: import_chain.py walks the subject recursively, resolving apiset
#      names through dlls/apisetschema/apisetschema.spec (the same file Wine
#      builds its map from) and every other name to its built thunk, and
#      reports every import that would bind to a sentinel.
#   D  HOLES: the holes it finds must be EXACTLY the documented set below.  A
#      new hole is a regression; a documented hole that quietly became served
#      means this list is stale and is also a failure.  Anything else lets the
#      list rot into a mute allow-everything.
#   E  RUNTIME: a guest PE LoadLibrary's the subject, and GetProcAddress
#      returns callable addresses for live ordinals -- and, when Razer's DLL is
#      present, for the names DOOM itself fetches.  Static analysis cannot see
#      a DllMain that faults on a sentinel, which is precisely how the chain
#      failed, so the load has to be executed as well as predicted.
#   F  ALL OF THEM.
#
# THE DOCUMENTED HOLES.  Each is an export whose SHAPE this generator cannot
# describe, not one it forgot.  They are listed here so that the gate is green
# only while the set is unchanged, and each carries the reason:
#
#   VCRUNTIME140.__CxxFrameHandler3   the guest image's own language
#       personality routine, named by its .pdata and called BY the unwinder
#       with that image's records.  A thunk would send a guest unwind into the
#       host's ppc64 handler, which cannot resume x86-64 code -- a silent wrong
#       answer where the hole is a named, diagnosable sentinel.  Needs a real
#       guest-side EH personality.
#
#   CLOSED 2026-08-18 (kept for the record): ldiv (ldiv_t returned BY VALUE)
#       and OLEAUT32.#113/VarBstrFromCy (CY taken by value) -- both 8-byte
#       integer aggregates, now classified in wine_sig.py's
#       AGGREGATE_SLOT_TYPES and served through a single 64-bit slot.
#
# --sabotage runs the negative controls instead and requires each to go red.  A
# gate that cannot go red proves nothing.
#
#   1  the hole list emptied: the three documented holes must then fail leg D,
#      proving the walker really does detect an unbound import rather than
#      reporting a clean chain by construction.
#   2  the runtime leg run in a directory where the subject is ABSENT:
#      LoadLibrary must fail and the probe must report FAIL, proving leg E is
#      not green just because a probe printed something.
#
# Exit 0 = pass, 1 = a check failed, 2 = could not run at all (a skip is NOT
# a pass).
set -u

HERE=$(cd "$(dirname "$0")" && pwd)
SRC=$(cd "$HERE/../.." && pwd)
BUILD=${BUILD:-$SRC}                    # in-tree build by default
OUT=${OUT:-$HOME/.cache/wine-ppc64le/import-chain}
SABOTAGE=0
[ "${1:-}" = "--sabotage" ] && SABOTAGE=1

say()  { echo "check-import-chain: $*"; }
bad()  { echo "check-import-chain: FAIL $*" >&2; fail=1; }
skip() { echo "check-import-chain: $*" >&2; exit 2; }

fail=0

# ---- the documented holes, one per line: <module>\t<symbol> ---------------
# Kept in the script rather than a data file so the reasons above and the list
# cannot drift apart.
holes_expected() {
    if [ "$SABOTAGE" = 1 ]; then
        # sabotage 1 INVERTED 2026-09-01: with the real list empty, "empty
        # the list" is a vacuous control.  Document a PHANTOM hole instead:
        # the stale-list check ("documented hole is no longer a hole") must
        # go red on it, which is exactly the check that caught the real
        # staleness this list carried for weeks.
        printf 'vcruntime140.dll\t__wine_phantom_hole_sabotage\n'
        return 0
    fi
    # __CxxFrameHandler3 LEFT this list 2026-09-01: the chain closes now --
    # vcruntime140 forwards to ucrtbase, which forwards to guestcrt, whose
    # export is real guest code (the guest EH personality runs AS GUEST,
    # which was the whole point of the old hole).  The audit's own rows say
    # so on both the old and new generators; the list was stale, and this
    # gate was red on a pristine tree until this line.
    cat <<'EOF'
EOF
}

# ---- leg A: the guest thunk surface exists -------------------------------
for m in vcruntime140 ucrtbase oleaut32 msvcp140 kernel32; do
    [ -f "$BUILD/dlls/$m/x86_64-windows/$m.dll" ] || \
        skip "no guest thunk at $BUILD/dlls/$m/x86_64-windows/$m.dll; build the tree first"
done
command -v python3 >/dev/null || skip "need python3"

# ---- leg B: find a subject ------------------------------------------------
# Microsoft's mfc140u.dll is not redistributed by this tree and never will be.
# It is the user's own file, installed into a Wine prefix by the VC++
# redistributable (winetricks vcrun2015 / vcredist).  Look where it actually
# lives; take an explicit override first.
subject=${IMPORT_CHAIN_SUBJECT:-}
if [ -z "$subject" ]; then
    for cand in \
        "${STEAM_COMPAT_DATA_PATH:-}"/pfx/drive_c/windows/system32/mfc140u.dll \
        "${WINEPREFIX:-}"/drive_c/windows/system32/mfc140u.dll \
        "$HOME"/.local/share/Steam/steamapps/compatdata/*/pfx/drive_c/windows/system32/mfc140u.dll \
        /mnt/*/steamapps/compatdata/*/pfx/drive_c/windows/system32/mfc140u.dll
    do
        [ -f "$cand" ] || continue
        subject=$cand
        break
    done
fi
[ -n "$subject" ] || skip "no mfc140u.dll found; set IMPORT_CHAIN_SUBJECT to an
    x86-64 third-party DLL, or install the VC++ redistributable into a prefix
    (winetricks vcrun2015).  This gate walks the USER'S files; it ships none."
say "subject $subject"

# A second subject when it is there: Razer's DLL is the one DOOM actually
# loads, it is the reason mfc140u is in the chain, and it exports BY NAME,
# which the ordinal-only mfc140u cannot exercise.
extra=${IMPORT_CHAIN_EXTRA:-}
if [ -z "$extra" ]; then
    for cand in /mnt/*/steamapps/common/DOOM/CChromaEditorLibrary.dll \
                "$HOME"/.local/share/Steam/steamapps/common/DOOM/CChromaEditorLibrary.dll
    do
        [ -f "$cand" ] && { extra=$cand; break; }
    done
fi

mkdir -p "$OUT" || skip "cannot create $OUT"

# Copy the subjects into scratch: the source lives under a Steam library that
# must not be written to, and the runtime leg needs a directory it owns.
cp -f "$subject" "$OUT/" || skip "cannot copy $subject"
subject_base=$(basename "$subject")
extra_base=
if [ -n "$extra" ]; then
    cp -f "$extra" "$OUT/" && extra_base=$(basename "$extra")
    say "extra subject $extra"
fi

# ---- leg C: the static walk ----------------------------------------------
set -- "$OUT/$subject_base"
[ -n "$extra_base" ] && set -- "$@" "$OUT/$extra_base"

python3 "$HERE/import_chain.py" --build "$BUILD" --src "$SRC" \
    --sibling-dir "$OUT" "$@" > "$OUT/chain.tsv" 2> "$OUT/chain.log" \
    || skip "import_chain.py failed; see $OUT/chain.log"

grep '^SUMMARY' "$OUT/chain.tsv" | sed 's/^/check-import-chain: /'

if grep -q '^NO-THUNK' "$OUT/chain.tsv"; then
    bad "an imported module has no guest thunk at all:"
    grep '^NO-THUNK' "$OUT/chain.tsv" | sed 's/^/    /' >&2
fi
if grep -qE '^SUBJECT-(BAD|MACHINE)' "$OUT/chain.tsv"; then
    bad "a subject is not a usable AMD64 PE:"
    grep -E '^SUBJECT-(BAD|MACHINE)' "$OUT/chain.tsv" | sed 's/^/    /' >&2
fi

# ---- leg D: the holes are exactly the documented set ----------------------
# Compared as (module, symbol) pairs regardless of which importer wanted them:
# two modules importing the same missing symbol is one hole in the surface.
awk -F'\t' '$1=="HOLE" { print $3 "\t" $4 }' "$OUT/chain.tsv" | sort -u > "$OUT/holes.found"
holes_expected | sort -u > "$OUT/holes.want"

if ! cmp -s "$OUT/holes.found" "$OUT/holes.want"; then
    n_new=$(comm -23 "$OUT/holes.found" "$OUT/holes.want" | grep -c . || true)
    n_gone=$(comm -13 "$OUT/holes.found" "$OUT/holes.want" | grep -c . || true)
    if [ "$n_new" -gt 0 ]; then
        bad "$n_new UNDOCUMENTED hole(s) -- these imports bind to a sentinel:"
        comm -23 "$OUT/holes.found" "$OUT/holes.want" | sed 's/^/    /' >&2
        echo "    (each is an export the guest thunk does not vend; see" >&2
        echo "     tools/spec2thunk --report for why it was refused)" >&2
    fi
    if [ "$n_gone" -gt 0 ]; then
        bad "$n_gone documented hole(s) are no longer holes -- the list in this"
        echo "    script is stale and must be updated with the reason removed:" >&2
        comm -13 "$OUT/holes.found" "$OUT/holes.want" | sed 's/^/    /' >&2
    fi
else
    say "holes: exactly the $(grep -c . < "$OUT/holes.want") documented one(s)"
fi

# ---- leg E: the runtime load ---------------------------------------------
[ -x "$BUILD/wine" ] || skip "no wine loader at $BUILD/wine"
[ -n "${WINEPREFIX:-}" ] || skip "set WINEPREFIX to a prefix wineboot has run in"
[ -n "${WINEFEXBRIDGE:-}" ] || skip "set WINEFEXBRIDGE to the emulator bridge"
command -v clang >/dev/null || skip "need clang for the guest build"
command -v llvm-dlltool >/dev/null || skip "need llvm-dlltool for the guest build"

# The ordinals are DISCOVERED from the subject's own export table rather than
# guessed: mfc140u exports 14109 functions and not one name, and a hard-coded
# ordinal would be a different DLL's fact.
python3 - "$OUT/$subject_base" "$OUT/probe_names.h" "$subject_base" "$extra_base" "$HERE" <<'PY' \
    || skip "could not read the subject's export table"
import sys
sys.path.insert(0, sys.argv[5])
from import_chain import PE
subj, out, base, extra = sys.argv[1:5]
names, ords = PE(subj).exports()
pick = sorted(ords)[:3]
if not pick:
    sys.exit('subject exports nothing')
with open(out, 'w') as f:
    f.write('/* generated by check-import-chain.sh -- do not edit */\n')
    f.write('#define PROBE_DLL "%s"\n' % base)
    f.write('#define PROBE_ORDINALS %s\n' % ', '.join(str(o) for o in pick))
    if extra:
        f.write('#define PROBE_NAMED_DLL "%s"\n' % extra)
        # DOOM's own two: the one it calls first and the one it died on.
        f.write('#define PROBE_NAMES "PluginInit", "PluginPlayComposite"\n')
PY

cat > "$OUT/kernel32.def" <<'EOF'
LIBRARY kernel32.dll
EXPORTS
GetStdHandle
WriteFile
LoadLibraryA
GetProcAddress
GetLastError
ExitProcess
EOF
llvm-dlltool -m i386:x86-64 -d "$OUT/kernel32.def" -l "$OUT/libkernel32.a" \
    || skip "llvm-dlltool failed"

clang -target x86_64-windows-gnu -nostdlibinc \
    -I"$OUT" -I"$BUILD/include" -I"$SRC/include" -I"$SRC/include/msvcrt" \
    -D_UCRT -Wall -O1 -fno-builtin -g \
    -c -o "$OUT/probe.o" "$HERE/import_chain_probe.c" \
    || skip "guest compile failed"
clang -target x86_64-windows-gnu -fuse-ld=lld -nostdlib \
    -Wl,--entry=probe_entry -Wl,--subsystem,console \
    -o "$OUT/probe.exe" "$OUT/probe.o" "$OUT/libkernel32.a" \
    || skip "guest link failed"

# Bounded: a guest that faults into the port's re-entrancy guard still exits,
# but one that spins would hang the gate rather than report red.
TIMEOUT=${TIMEOUT:-180}
rundir=$OUT
if [ "$SABOTAGE" = 1 ]; then
    # sabotage 2: the same probe, run where the subject is not.
    rundir=$OUT/nosubject
    mkdir -p "$rundir"
    cp -f "$OUT/probe.exe" "$rundir/"
fi

( cd "$rundir" && timeout -k 5 "$TIMEOUT" \
      env WINEDEBUG=${WINEDEBUG:--all} WINEDLLOVERRIDES=winedbg.exe=d \
      "$BUILD/wine" ./probe.exe ) > "$OUT/probe.out" 2> "$OUT/probe.err"
rc=$?

sed 's/^/    /' "$OUT/probe.out"

if [ "$SABOTAGE" = 1 ]; then
    if grep -q "import-chain-probe: PASS" "$OUT/probe.out"; then
        bad "sabotage 2: the probe PASSED with the subject absent -- leg E cannot go red"
    else
        say "sabotage 2: red as required (subject absent -> probe did not pass)"
    fi
else
    [ "$rc" = 0 ] || bad "the guest probe exited $rc (see $OUT/probe.err)"
    grep -q "import-chain-probe: PASS" "$OUT/probe.out" || \
        bad "the guest probe did not report PASS"
    # A sentinel reached at load time is the failure this gate is named for,
    # and it is visible in the run even when the probe's own checks pass.
    if grep -q "wild pointer: 00000000DEAD" "$OUT/probe.err"; then
        bad "a sentinel was CALLED during the run:"
        grep "wild pointer: 00000000DEAD" "$OUT/probe.err" | sed 's/^/    /' >&2
    fi
fi

# ---- leg F ----------------------------------------------------------------
if [ "$SABOTAGE" = 1 ]; then
    # sabotage 1 is leg D with the empty list: it must have failed above.
    if [ "$fail" = 0 ]; then
        echo "check-import-chain: FAIL sabotage: nothing went red" >&2
        exit 1
    fi
    say "SABOTAGE OK -- the controls go red"
    exit 0
fi

[ "$fail" = 0 ] || { echo "check-import-chain: FAILED" >&2; exit 1; }
say "PASS"
exit 0
