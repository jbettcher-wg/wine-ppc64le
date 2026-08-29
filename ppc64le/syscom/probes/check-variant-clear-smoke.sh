#!/bin/sh
#
# check-variant-clear-smoke.sh -- the VariantClear GUEST-IMPL wrapper's gate.
#
# Lives under probes/, not beside check-com-smoke.sh, on purpose: it does not
# raise a modal dialog and is safe to run directly (see the module README /
# task notes for the ppc64le/*/check-*.sh caution -- that caution is about
# the desktop-facing gate suite, not about scripts under a probes/ directory).
#
# Same shape as ../check-com-smoke.sh: ONE source built twice (native ppc64
# Windows PE, x86-64 guest PE), byte-identical stdout required, except for
# the ONE line the design says diverges on purpose (L7 -- a guest-implemented
# IUnknown inside a VARIANT; native releases it for real, the guest wrapper
# refuses it by name).  See variant_clear_smoke.c's file comment for what
# every step (L1-L11) checks.
#
# THE CHECK THAT MATTERS: L5 puts a live IStream forward proxy inside a
# VARIANT and clears it.  proxy_release (libs/winecom/winecom.c) is the only
# thing that ever prints "destroying proxy ... (IStream host ...)", and the
# probe creates exactly three IStream proxies through paths that are ALWAYS
# correct (an explicit Release at the end of L4b and at the end of L5's
# second stream) plus the one under test (Stream B, whose only route to
# destruction is the VariantClear call).  A correct guest run therefore
# prints that trace line exactly ONE MORE TIME than a run sabotaged with
# WINEEMUVARIANTUNSAFERELEASE=1 (dlls/combase/syscom.c), which releases
# Stream B's host reference directly and never tells the proxy its count
# reached zero.  That count, not a crash, is the fingerprint --  a double
# free does not have to crash to be one.
#
# Exit 0 = pass, 1 = a check failed, 2 = could not run (not a pass).
set -u

HERE=$(cd "$(dirname "$0")" && pwd)
SRC=$(cd "$HERE/../../.." && pwd)
BUILD=${BUILD:-$SRC}
OUT=${OUT:-/tmp/variant-clear-smoke}
SABOTAGE=${1:-}

say()  { echo "check-variant-clear-smoke: $*"; }
bad()  { echo "check-variant-clear-smoke: FAIL $*" >&2; fail=1; }
skip() { echo "check-variant-clear-smoke: $*" >&2; exit 2; }

[ -x "$BUILD/wine" ] || skip "no wine loader at $BUILD/wine"
[ -n "${WINEPREFIX:-}" ] || skip "set WINEPREFIX to a prefix wineboot has run in"
[ -n "${WINEFEXBRIDGE:-}" ] || skip "set WINEFEXBRIDGE to the emulator bridge"
[ -f "$BUILD/dlls/ole32/x86_64-windows/ole32.dll" ] || \
    skip "no guest ole32 thunk; build it first"
[ -f "$BUILD/dlls/oleaut32/x86_64-windows/oleaut32.dll" ] || \
    skip "no guest oleaut32 thunk; build it first"
command -v clang >/dev/null || skip "need clang for the guest build"
command -v llvm-dlltool >/dev/null || skip "need llvm-dlltool for the guest build"

mkdir -p "$OUT" || skip "cannot create $OUT"
fail=0

INCL="-I$BUILD/include -I$SRC/include -I$SRC/include/msvcrt"

# ---- build 1: the native ppc64 Windows PE ---------------------------------
${CC:-gcc} -c -o "$OUT/vcs.o" "$HERE/variant_clear_smoke.c" $INCL \
    -D_UCRT -D_WIN32 -Wall -pipe -mlongcall -mno-pltseq -fcf-protection=none \
    -fvisibility=hidden -fno-stack-protector -fno-strict-aliasing -gdwarf-4 \
    -fPIC -fasynchronous-unwind-tables -mlong-double-64 -fno-builtin \
    -fshort-wchar -Wno-format -g -O2 || skip "native compile failed"

"$BUILD/tools/winegcc/winegcc" -o "$OUT/vcs.exe" --wine-objdir "$BUILD" \
    --cc-cmd="${CC:-gcc}" -mno-cygwin -fPIC -fasynchronous-unwind-tables \
    -Wl,--wine-builtin -mconsole "$OUT/vcs.o" \
    "$BUILD/libs/winecrt0/ppc64-windows/libwinecrt0.a" \
    "$BUILD/dlls/oleaut32/ppc64-windows/liboleaut32.a" \
    "$BUILD/dlls/ole32/ppc64-windows/libole32.a" \
    "$BUILD/dlls/ucrtbase/ppc64-windows/libucrtbase.a" \
    "$BUILD/dlls/kernel32/ppc64-windows/libkernel32.a" \
    "$BUILD/dlls/ntdll/ppc64-windows/libntdll.a" || skip "native link failed"
rm -f "$OUT/vcs.exe"
"$SRC/tools/elf2pe" "$OUT/vcs.exe.so" "$OUT/vcs.exe" || skip "elf2pe failed"
"$BUILD/tools/winebuild/winebuild" --builtin "$OUT/vcs.exe" \
    || skip "winebuild --builtin failed"

# ---- build 2: the x86-64 guest PE ----------------------------------------
cat > "$OUT/ole32.def" <<'EOF'
LIBRARY ole32.dll
EXPORTS
CoInitializeEx
CoUninitialize
CreateStreamOnHGlobal
GetHGlobalFromStream
EOF
cat > "$OUT/oleaut32.def" <<'EOF'
LIBRARY oleaut32.dll
EXPORTS
VariantInit
VariantClear
SysAllocString
SysFreeString
SysStringLen
EOF
cat > "$OUT/kernel32.def" <<'EOF'
LIBRARY kernel32.dll
EXPORTS
GetStdHandle
WriteFile
ExitProcess
EOF
for m in ole32 oleaut32 kernel32; do
    llvm-dlltool -m i386:x86-64 -d "$OUT/$m.def" -l "$OUT/lib$m.a" \
        || skip "llvm-dlltool failed for $m"
done

clang -target x86_64-windows-gnu -nostdlibinc $INCL \
    -DVARIANT_SMOKE_NO_CRT -D_UCRT -Wall -O1 -fno-builtin -g \
    -c -o "$OUT/vcs_guest.o" "$HERE/variant_clear_smoke.c" \
    || skip "guest compile failed"
clang -target x86_64-windows-gnu -fuse-ld=lld -nostdlib \
    -Wl,--entry=variant_clear_smoke_entry -Wl,--subsystem,console \
    -o "$OUT/vcs_guest.exe" "$OUT/vcs_guest.o" \
    "$OUT/liboleaut32.a" "$OUT/libole32.a" "$OUT/libkernel32.a" \
    || skip "guest link failed"

TIMEOUT=${TIMEOUT:-120}
run_native() { timeout -k 5 "$TIMEOUT" \
                   env WINEDEBUG=-all "$BUILD/wine" "$OUT/vcs.exe" \
                   2>"$OUT/native.err"; }
run_guest()  { timeout -k 5 "$TIMEOUT" \
                   env WINEDEBUG=${1:--all} WINEEMUNOCOMWRAP=${2:-0} \
                   WINEEMUVARIANTUNSAFERELEASE=${3:-0} \
                   "$BUILD/wine" "$OUT/vcs_guest.exe" 2>"$OUT/guest.err"; }

strip_divergent() { grep -v '^variant_clear_smoke: L[0-9]* ' "$1"; }

if [ "$SABOTAGE" = "--sabotage" ]; then
    # ---- negative control A: the OLDER mechanism, same shape as
    # check-com-smoke.sh --sabotage.  WINEEMUNOCOMWRAP=1 hands L5's stream
    # in raw; the wrapper's classifier sees not-a-proxy and refuses where the
    # real run succeeds, so the run must not reach "... PASS ...".
    run_guest +winecom 1 0 > "$OUT/sab_nocomwrap.out"
    if grep -q "variant_clear_smoke: PASS" "$OUT/sab_nocomwrap.out"; then
        bad "WINEEMUNOCOMWRAP=1 still PASSED -- the gate cannot go red"
    else
        say "sabotage A (WINEEMUNOCOMWRAP=1): failed as it must, at '$(tail -1 \
            "$OUT/sab_nocomwrap.out" | cut -c1-70)'"
    fi

    # ---- negative control B: THE NEW mechanism.  WINEEMUVARIANTUNSAFERELEASE=1
    # routes L5's drop through the host vtable directly instead of
    # __wine_com_release_guest.  Measured effect (2026-08-29): it is not a
    # quiet one-fewer-destruction leak -- the intern table still holds Stream
    # B's proxy keyed by its (now-freed) host address, so when Stream C's
    # fresh CreateStreamOnHGlobal allocation reuses that address,
    # winecom_wrap's already-interned path hands back STREAM B'S OLD PROXY
    # as "stream C".  That shows up two ways: the PASS/FAIL tally itself
    # goes red (stream C's write/seek/read round trip corrupts and its
    # final Release does not reach zero), and the destroy count drops by
    # MORE than one (measured 3 -> 1, not 3 -> 2) because the merged B/C
    # identity never reaches a zero guest-visible refcount within the run.
    # Both signals are checked; a gate that only checked the count could in
    # principle be satisfied by a smaller, luckier corruption than this one
    # turned out to be.
    #
    # run_guest redirects stderr to a fixed $OUT/guest.err internally, so
    # each result must be saved off before the next call overwrites it.
    run_guest +winecom 0 0 > "$OUT/normal.trace.out"
    cp "$OUT/guest.err" "$OUT/normal.trace.err"
    run_guest +winecom 0 1 > "$OUT/sab_unsafe.trace.out"
    cp "$OUT/guest.err" "$OUT/sab_unsafe.trace.err"
    n_normal=$(grep -c 'destroying proxy .*(IStream host' "$OUT/normal.trace.err")
    n_sab=$(grep -c 'destroying proxy .*(IStream host' "$OUT/sab_unsafe.trace.err")
    say "sabotage B (WINEEMUVARIANTUNSAFERELEASE=1): IStream proxy destructions normal=$n_normal sabotaged=$n_sab"
    if grep -q "variant_clear_smoke: PASS" "$OUT/sab_unsafe.trace.out"; then
        bad "WINEEMUVARIANTUNSAFERELEASE=1 still PASSED all its own step \
verdicts -- the gate cannot go red"
    else
        say "sabotage B: the probe's own verdicts caught the corruption \
too ($(grep 'variant_clear_smoke: FAIL\|variant_clear_smoke: PASS' "$OUT/sab_unsafe.trace.out"))"
    fi
    if [ "$n_sab" -lt "$n_normal" ]; then
        say "sabotage B: fewer proxy destructions under sabotage, as it must \
(Stream B's host reference was dropped directly and the proxy never learned)"
    else
        bad "WINEEMUVARIANTUNSAFERELEASE=1 destroyed the same number of \
IStream proxies ($n_sab) as the normal run ($n_normal) -- the gate cannot go red"
    fi

    [ $fail -eq 0 ] && say "SABOTAGE PASS"
    exit $fail
fi

# ---- 1: native -----------------------------------------------------------
run_native > "$OUT/native.out"
if grep -q "variant_clear_smoke: PASS" "$OUT/native.out"; then
    say "native: $(grep 'variant_clear_smoke: PASS\|variant_clear_smoke: FAIL' "$OUT/native.out")"
else
    sed 's/^/  native| /' "$OUT/native.out" >&2
    bad "the native ppc64 build did not pass"
fi

# ---- 2: guest ------------------------------------------------------------
run_guest > "$OUT/guest.out"
if grep -q "variant_clear_smoke: PASS" "$OUT/guest.out"; then
    say "guest:  $(grep 'variant_clear_smoke: PASS\|variant_clear_smoke: FAIL' "$OUT/guest.out")"
else
    sed 's/^/  guest| /' "$OUT/guest.out" >&2
    tail -20 "$OUT/guest.err" >&2
    bad "the x86-64 guest build did not pass"
fi

# ---- 3: identity (excluding L7, which diverges by design) ----------------
strip_divergent "$OUT/native.out" > "$OUT/native.stripped.out"
strip_divergent "$OUT/guest.out" > "$OUT/guest.stripped.out"
if cmp -s "$OUT/native.stripped.out" "$OUT/guest.stripped.out"; then
    say "identity: native and guest output is byte-identical apart from L7 \
($(wc -l < "$OUT/native.stripped.out") lines)"
else
    diff "$OUT/native.stripped.out" "$OUT/guest.stripped.out" >&2
    bad "native and guest output differ outside of L7"
fi

# ---- 3b: L7 diverges EXACTLY as designed ----------------------------------
native_l7=$(grep '^variant_clear_smoke: L7 ' "$OUT/native.out")
guest_l7=$(grep '^variant_clear_smoke: L7 ' "$OUT/guest.out")
say "L7 native: $native_l7"
say "L7 guest:  $guest_l7"
case "$native_l7" in
    *"hr=0x00000000 released=1"*) ;;
    *) bad "L7 native did not actually release the guest-implemented object (hr should be 0, released should be 1)" ;;
esac
case "$guest_l7" in
    *"hr=0x80004001 released=0"*) ;;
    *) bad "L7 guest did not refuse the guest-implemented object cleanly (expected E_NOTIMPL, released=0)" ;;
esac

# ---- 3c: L8/L10, GUEST-ONLY refusals (see variant_clear_smoke.c for why
# these never run on the native build at all: real VariantClear has no
# FADF_*/pRecInfo gate, and running the same hand-rolled descriptors through
# it corrupted the native binary's heap when an earlier version of this
# probe tried).  Checked against guest.out alone, no native comparison.
guest_l8=$(grep '^variant_clear_smoke: L8 ' "$OUT/guest.out")
guest_l10=$(grep '^variant_clear_smoke: L10 ' "$OUT/guest.out")
say "L8 guest (interface-bearing SAFEARRAY): $guest_l8"
say "L10 guest (non-NULL IRecordInfo):       $guest_l10"
case "$guest_l8" in
    *"hr=0x80004001 vt=0x200D fFeatures=0x0200 pvData=NULL"*) ;;
    *) bad "L8 guest did not refuse the interface-bearing SAFEARRAY cleanly" ;;
esac
case "$guest_l10" in
    *"hr=0x80004001 vt=0x0024 pRecInfo=0x2000"*) ;;
    *) bad "L10 guest did not refuse the non-NULL IRecordInfo cleanly" ;;
esac

# ---- 4: mechanism ---------------------------------------------------------
run_guest +winecom > "$OUT/guest.trace.out"
strip_divergent "$OUT/guest.trace.out" > "$OUT/guest.trace.stripped.out"
cmp -s "$OUT/native.stripped.out" "$OUT/guest.trace.stripped.out" || \
    bad "the traced guest run did not reproduce the untraced one"
for want in "wrapped IStream host .* as proxy"; do
    if ! grep -qE "$want" "$OUT/guest.err"; then
        bad "no '$want' in the +winecom trace of the guest run"
    fi
done
n_destroy=$(grep -c 'destroying proxy .*(IStream host' "$OUT/guest.err")
say "mechanism: guest trace shows $n_destroy IStream proxy destruction(s) \
(expect 3: stream A at end of L4b, stream B via the VariantClear under test, \
stream C at end of L5)"
[ "$n_destroy" -eq 3 ] || \
    bad "expected exactly 3 'destroying proxy ... IStream' lines, got $n_destroy"

[ $fail -eq 0 ] && say "PASS"
exit $fail
