#!/bin/sh
#
# check-syscom-roster.sh -- system-COM roster verification
# (hangover-ppc64le/docs/system-com-design.md §11 step 1).
#
# Three layers, each of which removes one regex from the trust chain:
#
#   1  REGENERATION: gen_interfaces.py --surface wine-syscom reproduces the
#      committed interfaces_syscom.json byte-for-byte -- the committed roster
#      is not hand-edited and not stale against the headers.
#   2  SLOT LAYOUT: the compiled probe (gen_vtbl_check.py --half wine-syscom)
#      _Static_asserts every slot offset, every vtable size, and every typed
#      signature against Wine's OWN C vtbl structs.  Compiling IS the pass.
#   3  IID BYTES: gen_guid_check.py compares every rostered IID against the
#      compiler's own -DINITGUID expansion of the same headers (the §12.6
#      versioned-family mismap hazard).
#
# --sabotage runs the negative controls instead: a swapped slot pair must
# FAIL layer 2 and a mismapped IID must FAIL layer 3.  A checker that cannot
# go red proves nothing.
#
# Exit 0 = pass, 1 = a check failed, 2 = could not run (not a pass).
set -u

HERE=$(cd "$(dirname "$0")" && pwd)
SRCTREE=$(cd "$HERE/../.." && pwd)                    # wine-upstream
SABOTAGE=0
BUILD=""
for a in "$@"; do
    case "$a" in
        --sabotage) SABOTAGE=1 ;;
        *) BUILD=$a ;;
    esac
done
# An out-of-tree build directory if there is one, and otherwise this tree:
# every other gate here defaults to the in-tree build the same way, and
# looking only for a sibling wine-build made this script skip on a tree that
# was perfectly capable of running it.
[ -n "$BUILD" ] || BUILD=$(cd "$SRCTREE/../wine-build" 2>/dev/null && pwd)
[ -n "$BUILD" ] || BUILD=$SRCTREE
# Layers 2 and 3's instruments -- gen_vtbl_check.py and gen_guid_check.py --
# now live IN this tree, beside the roster they check, because nothing
# outside this tree ever grew them (see git history: they lived with a
# former co-developer and left with them).  They are found at $HERE
# unconditionally; there is no separate knob for them.
#
# Layer 1's instrument (gen_interfaces.py --surface wine-syscom, the FULL
# 71-interface regeneration) is a different, larger tool that nothing in
# this tree claims to be: ppc64le/syscom/gen_syscom_audio.py regenerates only
# the 12-interface audio family and reuses the other 58 rows verbatim (see
# its own docstring), which is not what layer 1 needs.  THUNK is where a
# full extractor would live if this port grows one; today it holds the DXVK
# lane's gen_interfaces.py, which has no wine-syscom surface.  Layer 1 is
# therefore SKIPPED, individually, when THUNK has no usable extractor --
# not the whole gate, because layers 2 and 3 do not depend on it and must
# not be hidden behind a gap that is really layer 1's alone.
THUNK=${THUNK:-$(cd "$SRCTREE/.." && pwd)/dxvk-ppc64le/thunk}
JSON="$HERE/interfaces_syscom.json"
WORK=${WORK:-/tmp/syscom-roster-check}

say()  { echo "check-syscom-roster: $*"; }
bad()  { echo "check-syscom-roster: FAIL $*" >&2; fail=1; }
skip() { echo "check-syscom-roster: $*" >&2; exit 2; }
note() { echo "check-syscom-roster: NOTE $*" >&2; }

[ -f "$JSON" ] || skip "no committed roster at $JSON"
[ -d "$BUILD/include" ] || skip "no build include dir under ${BUILD:-?}"

# The hard dependency: without these two, NOTHING in this gate can run, in
# sabotage mode or otherwise.
missing=
for g in gen_vtbl_check.py gen_guid_check.py; do
    [ -f "$HERE/$g" ] || missing="$missing $g"
done
[ -z "$missing" ] || skip "$HERE is missing:$missing -- layers 2 and 3 have \
no instrument"
command -v clang >/dev/null || skip "need clang"
command -v python3 >/dev/null || skip "need python3"

# The soft dependency: layer 1 alone.  Checked here, used only in the
# non-sabotage regeneration step below, so a report of what is missing does
# not also block the sabotage controls, which never touch this at all.
LAYER1_OK=1
LAYER1_WHY=
if [ ! -d "$THUNK" ]; then
    LAYER1_OK=0
    LAYER1_WHY="no generator tree at $THUNK (set THUNK= to point at it)"
elif [ ! -f "$THUNK/gen_interfaces.py" ]; then
    LAYER1_OK=0
    LAYER1_WHY="$THUNK has no gen_interfaces.py"
elif ! grep -q "wine-syscom\|\"syscom\"" "$THUNK/gen_interfaces.py" 2>/dev/null; then
    LAYER1_OK=0
    LAYER1_WHY="$THUNK/gen_interfaces.py has no wine-syscom surface, so it \
cannot regenerate $JSON.  ppc64le/syscom/gen_syscom_audio.py regenerates \
only the 12-interface audio family and reuses the other 58 rows verbatim \
(see its own docstring) -- it is not a substitute for the full extractor \
layer 1 needs."
fi

mkdir -p "$WORK"
fail=0
incomplete=0

# The extra -I is xaudio_classes.h's: it is widl output that lands under
# dlls/xaudio2_7/ in a build tree, not under an include/ directory the other
# three -I's would ever find (see gen_vtbl_check.py's docstring).
CFLAGS="-target x86_64-windows-gnu -fsyntax-only -nostdlibinc \
 -Werror=incompatible-pointer-types -Werror=incompatible-function-pointer-types \
 -I$BUILD/include -I$SRCTREE/include -I$SRCTREE/include/msvcrt \
 -I$BUILD/dlls/xaudio2_7"

if [ "$SABOTAGE" = 1 ]; then
    # ---- negative controls: each layer must be able to go red -----------
    python3 - "$JSON" "$WORK/sab_slots.json" <<'EOF'
import json, sys
d = json.load(open(sys.argv[1]))
s = d["interfaces"]["IStream"]["slots"]
s[3]["name"], s[4]["name"] = s[4]["name"], s[3]["name"]
s[3]["params"], s[4]["params"] = s[4]["params"], s[3]["params"]
json.dump(d, open(sys.argv[2], "w"))
EOF
    python3 "$HERE/gen_vtbl_check.py" --half wine-syscom \
        --json "$WORK/sab_slots.json" > "$WORK/sab_slots.c" 2>/dev/null \
        || skip "cannot emit the sabotaged probe"
    if clang $CFLAGS "$WORK/sab_slots.c" >"$WORK/sab_slots.log" 2>&1; then
        bad "swapped slot pair COMPILED -- the slot probe cannot go red"
    else
        say "sabotage(slots): swapped pair failed the probe, as it must"
    fi

    python3 - "$JSON" "$WORK/sab_guid.json" <<'EOF'
import json, sys
d = json.load(open(sys.argv[1]))
d["interfaces"]["IDirectMusicLoader"]["uuid"] = \
    d["interfaces"]["IDirectMusicLoader8"]["uuid"]
json.dump(d, open(sys.argv[2], "w"))
EOF
    if python3 "$HERE/gen_guid_check.py" --json "$WORK/sab_guid.json" \
        --wine-src "$SRCTREE/include" --wine-gen "$BUILD/include" \
        >"$WORK/sab_guid.log" 2>&1; then
        bad "mismapped IID PASSED -- the guid check cannot go red"
    else
        say "sabotage(iid): versioned-family mismap failed the check, as it must"
    fi
    [ $fail -eq 0 ] && say "SABOTAGE PASS (both controls red)"
    exit $fail
fi

# ---- 1: regeneration ------------------------------------------------------
if [ "$LAYER1_OK" = 1 ]; then
    python3 "$THUNK/gen_interfaces.py" --surface wine-syscom \
        --json "$WORK/regen.json" >"$WORK/regen.log" 2>&1 \
        || { cat "$WORK/regen.log" >&2; skip "extractor failed"; }
    if cmp -s "$JSON" "$WORK/regen.json"; then
        say "regeneration: committed roster is byte-identical"
    else
        bad "committed interfaces_syscom.json differs from regeneration"
    fi
else
    note "layer 1 (regeneration) SKIPPED: $LAYER1_WHY"
    incomplete=1
fi

# ---- 2: compiled slot probe ----------------------------------------------
python3 "$HERE/gen_vtbl_check.py" --half wine-syscom --json "$JSON" \
    > "$WORK/vtbl_check.c" 2>"$WORK/vtbl_emit.log" \
    || { cat "$WORK/vtbl_emit.log" >&2; skip "probe emission failed"; }
if clang $CFLAGS "$WORK/vtbl_check.c" >"$WORK/vtbl.log" 2>&1; then
    say "slot layout: compiled probe green ($(grep -c _Static_assert \
        "$WORK/vtbl_check.c") static assertions + typed slot checks)"
elif grep -q "file not found" "$WORK/vtbl.log"; then
    # Most of this surface's headers (objidl.h, oaidl.h, audioclient.h, ...)
    # are widl OUTPUT: they do not exist until something has run widl over
    # the matching .idl, which needs a real build and this invocation's BUILD
    # tree does not have.  That is a missing instrument, not a probe that
    # found drift -- the distinction check-syscom-audio.sh and every other
    # gate here draws between bad() and skip() -- so it is named and left for
    # the ppc64le host rather than reported as a FAIL.
    head -5 "$WORK/vtbl.log" >&2
    note "slot layout SKIPPED: headers this probe needs are build output \
(widl), and $BUILD has not built them -- run on a host with a configured \
build, or point BUILD= at one"
    incomplete=1
else
    head -20 "$WORK/vtbl.log" >&2
    bad "slot probe did not compile"
fi

# ---- 3: IID bytes ---------------------------------------------------------
if python3 "$HERE/gen_guid_check.py" --json "$JSON" \
    --wine-src "$SRCTREE/include" --wine-gen "$BUILD/include" \
    >"$WORK/guid.log" 2>&1; then
    say "iid bytes: $(tail -1 "$WORK/guid.log")"
else
    cat "$WORK/guid.log" >&2
    bad "IID byte check failed"
fi

if [ $fail -ne 0 ]; then
    exit 1
elif [ "$incomplete" -ne 0 ]; then
    say "no check that ran found any disagreement, but this host could not \
run everything (see the NOTE lines above) -- not a pass"
    exit 2
else
    say "PASS"
    exit 0
fi
