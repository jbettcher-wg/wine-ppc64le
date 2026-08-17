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
# The generator tree sits BESIDE the wine tree, not one level above it.  The
# old path was ../../dxvk-ppc64le/thunk, which does not exist anywhere, so
# this gate exited 2 on every run and named a directory nobody could create
# usefully.  A skip whose stated reason is wrong is worse than no skip.
THUNK=${THUNK:-$(cd "$SRCTREE/.." && pwd)/dxvk-ppc64le/thunk}
JSON="$HERE/interfaces_syscom.json"
WORK=${WORK:-/tmp/syscom-roster-check}

say()  { echo "check-syscom-roster: $*"; }
bad()  { echo "check-syscom-roster: FAIL $*" >&2; fail=1; }
skip() { echo "check-syscom-roster: $*" >&2; exit 2; }

[ -f "$JSON" ] || skip "no committed roster at $JSON"
[ -d "$BUILD/include" ] || skip "no build include dir under ${BUILD:-?}"
[ -d "$THUNK" ] || skip "no generator tree at $THUNK (set THUNK= to point at it)"

# Name what is missing, one thing at a time.  MEASURED 2026-08-17 on the tree
# beside this one: gen_interfaces.py is there but carries no wine-syscom
# surface (its SURFACES table is the dxvk/d3d9 one), and gen_vtbl_check.py and
# gen_guid_check.py do not exist in it at all -- so layers 1, 2 and 3 each
# have no instrument.  Until that generator tree grows them, this gate cannot
# run, and it should say exactly that rather than blame a missing directory.
missing=
for g in gen_interfaces.py gen_vtbl_check.py gen_guid_check.py; do
    [ -f "$THUNK/$g" ] || missing="$missing $g"
done
[ -z "$missing" ] || skip "the generator tree at $THUNK is missing:$missing -- \
layers 1-3 have no instrument there.  The audio family's roster authority in \
THIS tree is ppc64le/syscom/gen_syscom_audio.py, which regenerates \
dlls/combase/syscom_marshal.h and cross-checks every reused row against this \
roster; the header-vs-Wine-vtable and IID checks these three scripts perform \
have no in-tree equivalent yet"
grep -q "wine-syscom\|\"syscom\"" "$THUNK/gen_interfaces.py" 2>/dev/null || \
    skip "$THUNK/gen_interfaces.py has no wine-syscom surface, so layer 1 \
cannot regenerate $JSON"
command -v clang >/dev/null || skip "need clang"
command -v python3 >/dev/null || skip "need python3"

mkdir -p "$WORK"
fail=0

CFLAGS="-target x86_64-windows-gnu -fsyntax-only -nostdlibinc \
 -Werror=incompatible-pointer-types -Werror=incompatible-function-pointer-types \
 -I$BUILD/include -I$SRCTREE/include -I$SRCTREE/include/msvcrt"

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
    python3 "$THUNK/gen_vtbl_check.py" --half wine-syscom \
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
    if python3 "$THUNK/gen_guid_check.py" --json "$WORK/sab_guid.json" \
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
python3 "$THUNK/gen_interfaces.py" --surface wine-syscom \
    --json "$WORK/regen.json" >"$WORK/regen.log" 2>&1 \
    || { cat "$WORK/regen.log" >&2; skip "extractor failed"; }
if cmp -s "$JSON" "$WORK/regen.json"; then
    say "regeneration: committed roster is byte-identical"
else
    bad "committed interfaces_syscom.json differs from regeneration"
fi

# ---- 2: compiled slot probe ----------------------------------------------
python3 "$THUNK/gen_vtbl_check.py" --half wine-syscom --json "$JSON" \
    > "$WORK/vtbl_check.c" 2>"$WORK/vtbl_emit.log" \
    || { cat "$WORK/vtbl_emit.log" >&2; skip "probe emission failed"; }
if clang $CFLAGS "$WORK/vtbl_check.c" >"$WORK/vtbl.log" 2>&1; then
    say "slot layout: compiled probe green ($(grep -c _Static_assert \
        "$WORK/vtbl_check.c") static assertions + typed slot checks)"
else
    head -20 "$WORK/vtbl.log" >&2
    bad "slot probe did not compile"
fi

# ---- 3: IID bytes ---------------------------------------------------------
if python3 "$THUNK/gen_guid_check.py" --json "$JSON" \
    --wine-src "$SRCTREE/include" --wine-gen "$BUILD/include" \
    >"$WORK/guid.log" 2>&1; then
    say "iid bytes: $(tail -1 "$WORK/guid.log")"
else
    cat "$WORK/guid.log" >&2
    bad "IID byte check failed"
fi

[ $fail -eq 0 ] && say "PASS"
exit $fail
