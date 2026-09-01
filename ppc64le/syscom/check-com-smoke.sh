#!/bin/sh
#
# check-com-smoke.sh -- the system-COM RUNTIME gate.
#
# check-syscom-roster.sh proves the roster describes Wine's headers.  This
# proves the roster is actually USED: it builds com_smoke.c twice from the one
# source -- once as a native ppc64 Windows PE, once as an x86-64 guest PE --
# runs both, and requires their stdout to be BYTE-IDENTICAL.  Every line either
# run prints is a value Wine's own ole32 computed (a class id, a
# moniker-system constant, a hash, a refcount), so identical output means the
# guest reached the same implementation through the winecom proxy runtime.
#
# Four layers, each of which removes one way of passing by accident:
#
#   1  NATIVE: the native PE runs and reports PASS.  Establishes the expected
#      bytes without the guest lane in the picture at all.
#   2  GUEST: the guest PE runs under the emulator and reports PASS.
#   3  IDENTITY: diff(native, guest) is empty.
#   4  MECHANISM: +winecom trace of the guest run shows the guest vtables
#      being MATERIALISED, the returned interface being WRAPPED, and the
#      individual method calls arriving in winecom_dispatch by name.  Without
#      this a guest that somehow reached the right answers natively would
#      still pass layers 1-3.
#
# --sabotage runs the negative control instead: WINEEMUNOCOMWRAP=1 makes
# winecom_wrap hand the guest the RAW native pointer -- the exact defect this
# runtime exists to fix -- and the guest run MUST then fail.  A gate that
# cannot go red proves nothing.
#
# Exit 0 = pass, 1 = a check failed, 2 = could not run (not a pass).
set -u

HERE=$(cd "$(dirname "$0")" && pwd)
SRC=$(cd "$HERE/../.." && pwd)
BUILD=${BUILD:-$SRC}                    # in-tree build by default
OUT=${OUT:-/tmp/com-smoke}
SABOTAGE=0
[ "${1:-}" = "--sabotage" ] && SABOTAGE=1

say()  { echo "check-com-smoke: $*"; }
bad()  { echo "check-com-smoke: FAIL $*" >&2; fail=1; }
skip() { echo "check-com-smoke: $*" >&2; exit 2; }

[ -x "$BUILD/wine" ] || skip "no wine loader at $BUILD/wine"
[ -n "${WINEPREFIX:-}" ] || skip "set WINEPREFIX to a prefix wineboot has run in"
[ -n "${WINEFEXBRIDGE:-}" ] || skip "set WINEFEXBRIDGE to the emulator bridge"
[ -f "$BUILD/dlls/ole32/x86_64-windows/ole32.dll" ] || \
    skip "no guest ole32 thunk; build it first"
command -v clang >/dev/null || skip "need clang for the guest build"
command -v llvm-dlltool >/dev/null || skip "need llvm-dlltool for the guest build"

mkdir -p "$OUT" || skip "cannot create $OUT"
fail=0

INCL="-I$BUILD/include -I$SRC/include -I$SRC/include/msvcrt"

# ---- build 1: the native ppc64 Windows PE ---------------------------------
# The compile and link flags are the ones the tree's own Makefile uses for
# programs/winepath, minus -D__WINESRC__: this program is not Wine source, it
# is an ordinary consumer of the public headers, which is also the only way
# the guest build can see them.
${CC:-gcc} -c -o "$OUT/com_smoke.o" "$HERE/com_smoke.c" $INCL \
    -D_UCRT -D_WIN32 -Wall -pipe -mlongcall -mno-pltseq -fcf-protection=none \
    -fvisibility=hidden -fno-stack-protector -fno-strict-aliasing -gdwarf-4 \
    -fPIC -fasynchronous-unwind-tables -mlong-double-64 -fno-builtin \
    -fshort-wchar -Wno-format -g -O2 || skip "native compile failed"

"$BUILD/tools/winegcc/winegcc" -o "$OUT/com_smoke.exe" --wine-objdir "$BUILD" \
    --cc-cmd="${CC:-gcc}" -mno-cygwin -fPIC -fasynchronous-unwind-tables \
    -Wl,--wine-builtin -mconsole "$OUT/com_smoke.o" \
    "$BUILD/libs/winecrt0/ppc64-windows/libwinecrt0.a" \
    "$BUILD/dlls/ole32/ppc64-windows/libole32.a" \
    "$BUILD/dlls/oleaut32/ppc64-windows/liboleaut32.a" \
    "$BUILD/dlls/ucrtbase/ppc64-windows/libucrtbase.a" \
    "$BUILD/dlls/kernel32/ppc64-windows/libkernel32.a" \
    "$BUILD/dlls/ntdll/ppc64-windows/libntdll.a" || skip "native link failed"
rm -f "$OUT/com_smoke.exe"
"$SRC/tools/elf2pe" "$OUT/com_smoke.exe.so" "$OUT/com_smoke.exe" \
    || skip "elf2pe failed"
"$BUILD/tools/winebuild/winebuild" --builtin "$OUT/com_smoke.exe" \
    || skip "winebuild --builtin failed"

# ---- build 2: the x86-64 guest PE ----------------------------------------
# Same clang x86_64-windows-gnu machinery tools/spec2thunk drives its
# signature oracle with, and the same Wine headers -- so any disagreement
# between the two runs is the boundary, not the declarations.
#
# The imports are described by hand rather than taken from a mingw sysroot:
# the point of naming the DLL for each symbol is that the guest binds to the
# same builtins a real guest application would, and nothing else is linked in
# at all (there is no CRT here -- see com_smoke.c).
cat > "$OUT/ole32.def" <<'EOF'
LIBRARY ole32.dll
EXPORTS
CoInitializeEx
CoUninitialize
CoCreateInstance
CoGetClassObject
CreateStreamOnHGlobal
CoTaskMemAlloc
PropVariantClear
CoSetProxyBlanket
CreateErrorInfo
GetErrorInfo
SetErrorInfo
EOF
cat > "$OUT/oleaut32.def" <<'EOF'
LIBRARY oleaut32.dll
EXPORTS
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
    -DCOM_SMOKE_NO_CRT -D_UCRT -Wall -O1 -fno-builtin -g \
    -c -o "$OUT/com_smoke_guest.o" "$HERE/com_smoke.c" \
    || skip "guest compile failed"
clang -target x86_64-windows-gnu -fuse-ld=lld -nostdlib \
    -Wl,--entry=com_smoke_entry -Wl,--subsystem,console \
    -o "$OUT/com_smoke_guest.exe" "$OUT/com_smoke_guest.o" \
    "$OUT/libole32.a" "$OUT/liboleaut32.a" "$OUT/libkernel32.a" || skip "guest link failed"

# Bounded, because a run that never returns is a result too: the sabotage
# control hands the guest a native code pointer and it can spin instead of
# faulting, and an unbounded gate would hang there rather than report red.
TIMEOUT=${TIMEOUT:-120}
run_native() { timeout -k 5 "$TIMEOUT" \
                   env WINEDEBUG=-all "$BUILD/wine" "$OUT/com_smoke.exe" \
                   2>"$OUT/native.err"; }
run_guest()  { timeout -k 5 "$TIMEOUT" \
                   env WINEDEBUG=${1:--all} WINEEMUNOCOMWRAP=${2:-0} \
                   "$BUILD/wine" "$OUT/com_smoke_guest.exe" 2>"$OUT/guest.err"; }

if [ "$SABOTAGE" = 1 ]; then
    # ---- negative control -------------------------------------------------
    # WINEEMUNOCOMWRAP=1 hands the guest the raw native interface pointer, so
    # its first method call executes ppc64 bytes as x86-64.  The run MUST NOT
    # reach "com_smoke: PASS".
    run_guest +winecom 1 > "$OUT/sabotage.out"
    cp "$OUT/guest.err" "$OUT/sabotage.err"
    if grep -q "com_smoke: PASS" "$OUT/sabotage.out"; then
        bad "WINEEMUNOCOMWRAP=1 still PASSED -- the gate cannot go red"
    else
        say "sabotage: raw interface pointers failed the guest run at '$(tail -1 \
            "$OUT/sabotage.out" | cut -c1-60)', as they must"
    fi
    [ $fail -eq 0 ] && say "SABOTAGE PASS"
    exit $fail
fi

# ---- 1: native -----------------------------------------------------------
run_native > "$OUT/native.out"
if grep -q "com_smoke: PASS" "$OUT/native.out"; then
    say "native: $(tail -1 "$OUT/native.out")"
else
    sed 's/^/  native| /' "$OUT/native.out" >&2
    bad "the native ppc64 build did not pass"
fi

# ---- 2: guest ------------------------------------------------------------
run_guest > "$OUT/guest.out"
if grep -q "com_smoke: PASS" "$OUT/guest.out"; then
    say "guest:  $(tail -1 "$OUT/guest.out")"
else
    sed 's/^/  guest| /' "$OUT/guest.out" >&2
    tail -20 "$OUT/guest.err" >&2
    bad "the x86-64 guest build did not pass"
fi

# ---- 3: identity ---------------------------------------------------------
if cmp -s "$OUT/native.out" "$OUT/guest.out"; then
    say "identity: native and guest output is byte-identical ($(wc -l \
        < "$OUT/native.out") lines)"
else
    diff "$OUT/native.out" "$OUT/guest.out" >&2
    bad "native and guest output differ"
fi

# ---- 4: mechanism --------------------------------------------------------
# The guest could in principle print the right bytes while calling a native
# vtable (that IS the defect), so require the runtime's own trace to show the
# proxy path being taken.
run_guest +winecom > "$OUT/guest.trace.out"
cmp -s "$OUT/native.out" "$OUT/guest.trace.out" || \
    bad "the traced guest run did not reproduce the untraced one"
for want in "materialised .* guest vtable slots" \
            "wrapped IMoniker host .* as proxy" \
            "wrapped IClassFactory host .* as proxy" \
            "wrapped IStream host .* as proxy" \
            "winecom_dispatch IMoniker::IsSystemMoniker" \
            "winecom_dispatch IPersist::GetClassID" \
            "winecom_dispatch IMoniker::Hash" \
            "winecom_dispatch IClassFactory::CreateInstance" \
            "winecom_dispatch ISequentialStream::Write" \
            "winecom_dispatch IStream::Seek" \
            "destroying proxy .*\(IStream host"; do
    if ! grep -qE "$want" "$OUT/guest.err"; then
        bad "no '$want' in the +winecom trace of the guest run"
    fi
done
[ $fail -eq 0 ] && say "mechanism: guest vtables materialised, interface \
wrapped, methods dispatched by name"

[ $fail -eq 0 ] && say "PASS"
exit $fail
