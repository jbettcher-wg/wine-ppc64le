#!/bin/sh
#
# check-callback-rows.sh -- every native export that takes a guest FUNCTION
# POINTER is either wrapped or written down.
#
# THE FAILURE THIS EXISTS FOR.  A guest hands a native module a pointer to its
# own code -- a WNDPROC, a DLGPROC, a completion routine, an APC -- and the
# module stores it and calls it later.  Nothing in the pointer says which
# machine it belongs to, so unless the port swaps it for a trampoline at the
# moment of registration, the NATIVE ppc64 core eventually executes x86-64
# bytes.  It does not stop when it does: it decodes them as ppc64 instructions
# and runs them, and the first one that touches memory raises an access
# violation at an address inside the GUEST image -- which reads exactly like
# the game dereferencing a null pointer.  DOOM (2016) cost this port days in
# that shape twice, most recently through user32's DialogBoxParamA and
# winhttp's WinHttpSetStatusCallback.
#
# Every row in thunk_overrides[] was, until now, written after a program died
# on the export it names.  callback_audit.py asks the question the other way
# round, from Wine's own headers through the same clang oracle the thunk
# generator uses: which exports take a parameter whose type is a pointer to a
# function, and which of those has no row.  What is left over is
# callback_holes.txt, matched EXACTLY -- so a new hole fails this gate, and a
# hole that gets filled without being struck off fails it too.
#
# Layers:
#
#   1  AUDIT: the whole thunk surface, every export, every parameter.  Passes
#      when the uncovered set equals callback_holes.txt exactly.
#   2  NEGATIVE CONTROL (--sabotage): a copy of signal_ppc64.c with one row
#      deleted -- gdi32's EnumFontFamiliesA, the row that closed DOOM's first
#      crash of this class -- must come back as a NEW HOLE.  Without this the
#      gate would pass just as happily if it had stopped reading the table at
#      all.  It has to be a row whose callback arrives as an ARGUMENT: a
#      WNDPROC inside a WNDCLASSEX is the same failure but no parameter type
#      names it, which is why those rows have handlers and why this audit
#      cannot see them.
#
# This gate needs no wine, no prefix and no emulator: it reads headers and
# source.  It does need the build's generated headers, so it skips rather than
# fails when run against a tree that has not been configured.
#
# Exit 0 = pass, 1 = a check failed, 2 = could not run (not a pass).
set -u

HERE=$(cd "$(dirname "$0")" && pwd)
SRC=$(cd "$HERE/../.." && pwd)
BUILD=${BUILD:-$SRC}
OUT=${OUT:-/tmp/callback-rows}
SABOTAGE=0
[ "${1:-}" = "--sabotage" ] && SABOTAGE=1

say()  { echo "check-callback-rows: $*"; }
bad()  { echo "check-callback-rows: FAIL $*" >&2; fail=1; }
skip() { echo "check-callback-rows: $*" >&2; exit 2; }

command -v clang >/dev/null || skip "need clang for the header oracle"
[ -f "$BUILD/include/wtypes.h" ] || \
    skip "no widl-generated headers under $BUILD/include; configure the tree first"
[ -f "$SRC/ppc64le/thunks/callback_holes.txt" ] || skip "no callback_holes.txt"
mkdir -p "$OUT" || skip "cannot create $OUT"
fail=0

AUDIT="python3 $HERE/callback_audit.py --source $SRC --build $BUILD"

if [ $SABOTAGE -eq 0 ]; then
    if $AUDIT > "$OUT/audit.txt" 2>&1; then
        say "$(head -1 "$OUT/audit.txt")"
        say "$(tail -1 "$OUT/audit.txt")"
    else
        bad "the audit and callback_holes.txt disagree:"
        grep -E "NEW HOLE|FILLED|FAIL" "$OUT/audit.txt" | sed 's/^/  /' >&2
    fi
fi

# ---- --sabotage: a row removed must show up as a hole ---------------------
sabotage_row() {
    grep -v '{ L"gdi32.dll", "EnumFontFamiliesA",   4, NULL, 1u << 2 },' \
        "$SRC/dlls/ntdll/signal_ppc64.c" > "$OUT/signal_no_row.c"
    if cmp -s "$SRC/dlls/ntdll/signal_ppc64.c" "$OUT/signal_no_row.c"; then
        bad "the row this control removes is not in signal_ppc64.c any more; \
the control proves nothing until it names one that is"
        return
    fi
    if $AUDIT --signal-c "$OUT/signal_no_row.c" > "$OUT/sab.txt" 2>&1; then
        bad "with gdi32's EnumFontFamiliesA row deleted the audit still passed; \
it is not reading the table it claims to read"
        return
    fi
    if grep -q "NEW HOLE   gdi32.dll EnumFontFamiliesA" "$OUT/sab.txt"; then
        say "row deleted: the audit reported it as a new hole"
    else
        sed 's/^/  sab| /' "$OUT/sab.txt" >&2
        bad "with the row deleted the audit failed for some OTHER reason; a \
control that fails for the wrong reason is not a control"
    fi
}

[ $SABOTAGE -eq 1 ] && sabotage_row

if [ $fail -eq 0 ]; then
    if [ $SABOTAGE -eq 1 ]; then say "SABOTAGE PASS"; else say "PASS"; fi
    exit 0
fi
say "FAILED" >&2
exit 1
