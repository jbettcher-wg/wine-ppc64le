#!/bin/sh
#
# check-source-tier.sh -- spec2thunk's SOURCE-DEFINITION signature tier
# actually reads definitions, refuses what it must, and cannot be guessed
# past.
#
# The tier (wine_sig.WineSourceDefs, read its banner): an export declared in
# no Wine header is served from the module's own implementing C definition,
# clang-verified in that file's own translation unit and cross-checked
# against the .spec arity.  This gate drives it with a FIXTURE module whose
# definitions it controls, so every assertion is against a hand-verified
# expectation:
#
#   1  SERVED       a plain definition is emitted with source-definition
#      provenance and the hand-computed shape (2 args, void).
#   2  ALIAS        `@ cdecl WtstAliased(long) wtst_impl` serves from the
#      TARGET's definition, the same rescue order as the header path.
#   3  FP           a double-taking definition is emitted with an fp= word --
#      the tier's own-TU floating-point measurement is live.
#   4  DENIED       a name in the C++/SEH deny family is refused with the
#      guest-execution-contract reason even though its definition is
#      perfectly readable -- the load-bearing hole stays a hole, and now
#      says why.
#   5  CALLBACK     a TYPEDEF'd function-pointer parameter is refused with
#      the interception-row reason -- the guard reads the DESUGARED type, so
#      a callback hidden behind a typedef cannot slip into a flat serve.
#
# --sabotage: the fixture's definition gains a third argument while its
# .spec still says two.  The run must REFUSE that export with an arity
# disagreement -- an oracle that guessed from the .spec instead of reading
# the definition would emit it, which is exactly what this control exists
# to catch.
#
# Exit 0 pass, 1 fail, 2 skip (reason on stderr).
set -u

HERE=$(cd "$(dirname "$0")" && pwd)
SRC=$(cd "$HERE/../.." && pwd)
BUILD=${BUILD:-$SRC}
SABOTAGE=0
[ "${1:-}" = --sabotage ] && SABOTAGE=1

say()  { echo "check-source-tier: $*" >&2; }
bad()  { echo "check-source-tier: FAIL $*" >&2; fail=1; }
skip() { echo "check-source-tier: SKIP $*" >&2; exit 2; }
fail=0

command -v clang >/dev/null || skip "no clang"
[ -x "$SRC/tools/spec2thunk/spec2thunk" ] || skip "no spec2thunk"
[ -f "$BUILD/include/wtypes.h" ] || skip "run from the build tree (widl headers)"

OUT=${OUT:-/tmp/check-source-tier.$$}
FIX="$OUT/fix"
mkdir -p "$FIX/dlls/wtsrctier" || skip "cannot create $OUT"
trap 'rm -rf "$OUT"' EXIT

cat > "$FIX/dlls/wtsrctier/wtsrctier.spec" <<'EOF'
@ stdcall WtstPlain(ptr long)
@ cdecl WtstAliased(long) wtst_impl
@ cdecl WtstFp(double)
@ stdcall WtstFrameHandlerX(ptr)
@ stdcall WtstCallback(ptr)
EOF

cat > "$FIX/dlls/wtsrctier/Makefile.in" <<'EOF'
MODULE  = wtsrctier.dll
SOURCES = main.c
EOF

# The definitions.  None of these names is declared in any Wine header, so
# every served row below can only have come from reading THIS file.
nargs3=""
[ $SABOTAGE = 1 ] && nargs3=", int c"
cat > "$FIX/dlls/wtsrctier/main.c" <<EOF
#include <windows.h>

void WINAPI WtstPlain( void *a, unsigned short b$nargs3 )
{
    (void)a; (void)b;
}

int __cdecl wtst_impl( int x )
{
    return x + 1;
}

double __cdecl WtstFp( double d )
{
    return d * 2.0;
}

/* a perfectly readable definition whose SERVICE would be the bug: the name
 * is in the tier's C++/SEH deny family and must stay a hole, with the
 * reason */
LONG WINAPI WtstFrameHandlerX( void *a )
{
    (void)a;
    return 0;
}

typedef int (WINAPI *WTSTCB)( int );

int WINAPI WtstCallback( WTSTCB cb )
{
    (void)cb;
    return 0;
}
EOF

"$SRC/tools/spec2thunk/spec2thunk" \
    --from-spec "$FIX/dlls/wtsrctier/wtsrctier.spec" \
    --body=trap --machine x86_64 \
    --wine-source "$FIX" \
    --wine-include "$SRC/include" --wine-generated "$BUILD/include" \
    --out "$OUT/wtsrctier.dll" --report "$OUT/r.tsv" \
    > "$OUT/run.log" 2>&1
rc=$?
[ $rc = 0 ] || { sed -n '1,40p' "$OUT/run.log" >&2; bad "spec2thunk exited $rc"; }

R="$OUT/r.tsv"
[ -f "$R" ] || skip "no report produced (log: $OUT/run.log)"

if [ $SABOTAGE = 1 ]; then
    # The definition says 3 arguments, the .spec says 2.  An oracle that
    # read the definition refuses on the disagreement; one that guessed
    # from the .spec emits it.
    if grep -q '^WtstPlain	emitted' "$R"; then
        bad "sabotage: WtstPlain was EMITTED with a 3-arg definition against a 2-arg spec -- the oracle did not read the definition"
    elif grep '^WtstPlain	refused' "$R" | grep -q 'arity disagreement'; then
        say "sabotage: the 3-arg definition against the 2-arg spec was refused as an arity disagreement, as it must be"
    else
        grep '^WtstPlain' "$R" >&2 || echo "(no WtstPlain row at all)" >&2
        bad "sabotage: WtstPlain neither emitted nor refused-for-arity"
    fi
    [ $fail = 0 ] && { say "SABOTAGE PASS"; exit 0; }
    exit 1
fi

# 1: served, with provenance and the hand-verified shape.
if grep '^WtstPlain	emitted	source-definition' "$R" | grep -q '2 args, void'; then
    say "1: WtstPlain served from its definition (source-definition, 2 args, void)"
else
    grep '^WtstPlain' "$R" >&2 || echo "(no WtstPlain row)" >&2
    bad "1: WtstPlain not served with source-definition provenance + 2 args, void"
fi

# 2: the alias target's definition answers for the alias.
if grep '^WtstAliased	emitted	source-definition' "$R" | grep -q '1 args, value'; then
    say "2: WtstAliased served from its alias target's definition"
else
    grep '^WtstAliased' "$R" >&2 || echo "(no WtstAliased row)" >&2
    bad "2: WtstAliased not served through the alias rescue"
fi

# 3: the FP measurement ran in the fixture's own TU.
if grep '^WtstFp	emitted	source-definition' "$R" | grep -q 'fp=0x'; then
    say "3: WtstFp served with an fp= word (own-TU floating-point measurement)"
else
    grep '^WtstFp' "$R" >&2 || echo "(no WtstFp row)" >&2
    bad "3: WtstFp not served with an fp descriptor"
fi

# 4: the deny family holds, with the reason.
if grep '^WtstFrameHandlerX	refused' "$R" | grep -q 'guest execution contract'; then
    say "4: the deny-family name stays refused, and says why"
else
    grep '^WtstFrameHandlerX' "$R" >&2 || echo "(no WtstFrameHandlerX row)" >&2
    bad "4: WtstFrameHandlerX must be refused with the guest-execution-contract reason"
fi

# 5: a typedef'd callback parameter cannot slip past the desugared guard.
if grep '^WtstCallback	refused' "$R" | grep -q 'function pointer'; then
    say "5: the typedef'd callback parameter is refused with the interception-row reason"
else
    grep '^WtstCallback' "$R" >&2 || echo "(no WtstCallback row)" >&2
    bad "5: WtstCallback must be refused (desugared function-pointer guard)"
fi

[ $fail = 0 ] && { say "PASS"; exit 0; }
exit 1
