#!/bin/sh
#
# check-com-apartment.sh -- the implicit-MTA gate for GUEST threads.
#
# check-com-smoke.sh proves a guest thread that DID call CoInitializeEx
# reaches Wine's COM through the proxy runtime.  This proves the other half,
# the one a real title actually depends on: what happens on a guest thread
# that never called CoInitializeEx at all.
#
# THE MEASUREMENT THIS EXISTS FOR.  A 300s DOOM run on 2026-08-17
# (compatdata/379720, wine-ppc64le-native-20260817-215349-954825.log) logged
# 891 "err:ole:com_get_class_object apartment not initialised" across 31 guest
# worker threads.  The trap trace of the same title shows why: the main thread
# calls CoInitializeEx(NULL, COINIT_APARTMENTTHREADED) -- an STA -- and its
# job-pool threads then CoCreateInstance with no initialisation of their own.
# Windows and Wine grant such a thread the process MTA, but only once an MTA
# EXISTS, and an STA is not one.  So the error class has two possible causes
# and they need opposite fixes:
#
#   (a) the port fails to grant the implicit MTA across the thunk boundary
#       -- a port defect, and this gate goes red on the `mta` case; or
#   (b) no MTA exists in the process at all -- in which case the refusal is
#       Windows-faithful (dlls/ole32/tests/compobj.c test_CoCreateInstance,
#       dlls/combase/tests/roapi.c test_implicit_mta both assert exactly it)
#       and the missing piece is whoever was supposed to join the MTA.
#
# Five cases, one process each, run as BOTH a native ppc64 PE and an x86-64
# guest PE; see com_apartment.c for what each does.  Layers:
#
#   1  NATIVE: all five cases pass natively.  This is Wine's own answer with
#      the guest lane out of the picture -- the reference for layer 3.
#   2  GUEST: all five cases pass under the emulator.
#   3  IDENTITY: diff(native, guest) is empty, per case.  A guest thread that
#      landed in a different apartment from the native thread shows up here.
#   4  MECHANISM: the granted worker's moniker really came back through the
#      winecom proxy runtime (+winecom trace, by name), not by some native
#      shortcut that would make layers 1-3 pass for the wrong reason.
#   5  ERROR CLASS: each guest run must log the exact DOOM error string the
#      exact number of times its case is designed to earn -- 1 for `sta`, 1
#      for `joiner` (the probe after the joiner leaves), 2 for `noinit`, and
#      NONE for `mta`/`stamta`.  That ties this gate to the 891 errors by
#      their own text rather than by argument, in both directions.
#
# --sabotage runs the negative control: WINEEMUNOCOMWRAP=1 hands the guest the
# raw native interface pointer on the `mta` case -- the defect the proxy
# runtime exists to prevent -- and that run MUST fail.  A gate that cannot go
# red proves nothing.
#
# Exit 0 = pass, 1 = a check failed, 2 = could not run (not a pass).
set -u

HERE=$(cd "$(dirname "$0")" && pwd)
SRC=$(cd "$HERE/../.." && pwd)
BUILD=${BUILD:-$SRC}                    # in-tree build by default
OUT=${OUT:-/tmp/com-apartment}
SABOTAGE=0
[ "${1:-}" = "--sabotage" ] && SABOTAGE=1

CASES="noinit mta sta stamta joiner"

say()  { echo "check-com-apartment: $*"; }
bad()  { echo "check-com-apartment: FAIL $*" >&2; fail=1; }
skip() { echo "check-com-apartment: $*" >&2; exit 2; }

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
# Flag-for-flag what check-com-smoke.sh uses, and for the same reason: this
# program is not Wine source, it is an ordinary consumer of the public
# headers, which is also the only way the guest build can see them.
${CC:-gcc} -c -o "$OUT/com_apartment.o" "$HERE/com_apartment.c" $INCL \
    -D_UCRT -D_WIN32 -Wall -pipe -mlongcall -mno-pltseq -fcf-protection=none \
    -fvisibility=hidden -fno-stack-protector -fno-strict-aliasing -gdwarf-4 \
    -fPIC -fasynchronous-unwind-tables -mlong-double-64 -fno-builtin \
    -fshort-wchar -Wno-format -g -O2 || skip "native compile failed"

"$BUILD/tools/winegcc/winegcc" -o "$OUT/com_apartment.exe" --wine-objdir "$BUILD" \
    --cc-cmd="${CC:-gcc}" -mno-cygwin -fPIC -fasynchronous-unwind-tables \
    -Wl,--wine-builtin -mconsole "$OUT/com_apartment.o" \
    "$BUILD/libs/winecrt0/ppc64-windows/libwinecrt0.a" \
    "$BUILD/dlls/ole32/ppc64-windows/libole32.a" \
    "$BUILD/dlls/ucrtbase/ppc64-windows/libucrtbase.a" \
    "$BUILD/dlls/kernel32/ppc64-windows/libkernel32.a" \
    "$BUILD/dlls/ntdll/ppc64-windows/libntdll.a" || skip "native link failed"
rm -f "$OUT/com_apartment.exe"
"$SRC/tools/elf2pe" "$OUT/com_apartment.exe.so" "$OUT/com_apartment.exe" \
    || skip "elf2pe failed"
"$BUILD/tools/winebuild/winebuild" --builtin "$OUT/com_apartment.exe" \
    || skip "winebuild --builtin failed"

# ---- build 2: the x86-64 guest PE ----------------------------------------
# Imports named by hand so the guest binds to the same builtins a real guest
# application would, and to nothing else -- there is no CRT here.
cat > "$OUT/ole32.def" <<'EOF'
LIBRARY ole32.dll
EXPORTS
CoInitializeEx
CoUninitialize
CoCreateInstance
CoGetApartmentType
CoIncrementMTAUsage
CoDecrementMTAUsage
EOF
cat > "$OUT/kernel32.def" <<'EOF'
LIBRARY kernel32.dll
EXPORTS
GetStdHandle
WriteFile
ExitProcess
GetEnvironmentVariableA
GetLastError
CreateThread
WaitForSingleObject
CloseHandle
CreateEventW
SetEvent
EOF
for m in ole32 kernel32; do
    llvm-dlltool -m i386:x86-64 -d "$OUT/$m.def" -l "$OUT/lib$m.a" \
        || skip "llvm-dlltool failed for $m"
done

clang -target x86_64-windows-gnu -nostdlibinc $INCL \
    -DCOM_APARTMENT_NO_CRT -D_UCRT -Wall -O1 -fno-builtin -g \
    -c -o "$OUT/com_apartment_guest.o" "$HERE/com_apartment.c" \
    || skip "guest compile failed"
clang -target x86_64-windows-gnu -fuse-ld=lld -nostdlib \
    -Wl,--entry=com_apartment_entry -Wl,--subsystem,console \
    -o "$OUT/com_apartment_guest.exe" "$OUT/com_apartment_guest.o" \
    "$OUT/libole32.a" "$OUT/libkernel32.a" || skip "guest link failed"

# Bounded: a run that never returns is a result too, and the sabotage control
# can spin rather than fault.
TIMEOUT=${TIMEOUT:-120}
run_native() { timeout -k 5 "$TIMEOUT" \
                   env WINEDEBUG=-all COM_APARTMENT_CASE="$1" \
                   "$BUILD/wine" "$OUT/com_apartment.exe" \
                   2>"$OUT/native.$1.err"; }
run_guest()  { timeout -k 5 "$TIMEOUT" \
                   env WINEDEBUG=${2:-+ole} WINEEMUNOCOMWRAP=${3:-0} \
                   COM_APARTMENT_CASE="$1" \
                   "$BUILD/wine" "$OUT/com_apartment_guest.exe" \
                   2>"$OUT/guest.$1.err"; }

if [ "$SABOTAGE" = 1 ]; then
    # ---- negative control -------------------------------------------------
    # The `mta` case is the one that must SUCCEED, so it is the one worth
    # breaking: with WINEEMUNOCOMWRAP=1 the worker gets the raw native
    # IMoniker and its first method call executes ppc64 bytes as x86-64.
    run_guest mta +winecom 1 > "$OUT/sabotage.out"
    if grep -q "com_apartment: PASS" "$OUT/sabotage.out"; then
        bad "WINEEMUNOCOMWRAP=1 still PASSED the mta case -- the gate cannot go red"
    else
        say "sabotage: raw interface pointers failed the mta case at '$(tail -1 \
            "$OUT/sabotage.out" | cut -c1-60)', as they must"
    fi
    [ $fail -eq 0 ] && say "SABOTAGE PASS"
    exit $fail
fi

# ---- 1: native -----------------------------------------------------------
for c in $CASES; do
    run_native "$c" > "$OUT/native.$c.out"
    if grep -q "com_apartment: PASS" "$OUT/native.$c.out"; then
        say "native $c: $(tail -1 "$OUT/native.$c.out")"
    else
        sed "s/^/  native $c| /" "$OUT/native.$c.out" >&2
        bad "the native ppc64 build did not pass case $c"
    fi
done

# ---- 2: guest ------------------------------------------------------------
for c in $CASES; do
    run_guest "$c" > "$OUT/guest.$c.out"
    if grep -q "com_apartment: PASS" "$OUT/guest.$c.out"; then
        say "guest  $c: $(tail -1 "$OUT/guest.$c.out")"
    else
        sed "s/^/  guest $c| /" "$OUT/guest.$c.out" >&2
        tail -20 "$OUT/guest.$c.err" >&2
        bad "the x86-64 guest build did not pass case $c"
    fi
done

# ---- 3: identity ---------------------------------------------------------
for c in $CASES; do
    if cmp -s "$OUT/native.$c.out" "$OUT/guest.$c.out"; then
        say "identity $c: native and guest output is byte-identical"
    else
        diff "$OUT/native.$c.out" "$OUT/guest.$c.out" >&2
        bad "native and guest output differ for case $c"
    fi
done

# ---- 4: mechanism --------------------------------------------------------
# The granted worker could in principle print the right bytes while holding a
# native vtable (that IS the defect winecom exists to prevent), so require the
# runtime's own trace to show the proxy path being taken ON THE WORKER.
run_guest mta +winecom > "$OUT/guest.mta.trace.out"
cmp -s "$OUT/native.mta.out" "$OUT/guest.mta.trace.out" || \
    bad "the traced guest mta run did not reproduce the untraced one"
for want in "wrapped IMoniker host .* as proxy" \
            "winecom_dispatch IMoniker::IsSystemMoniker"; do
    if ! grep -qE "$want" "$OUT/guest.mta.err"; then
        bad "no '$want' in the +winecom trace of the guest mta run"
    fi
done
[ $fail -eq 0 ] && say "mechanism: the implicitly-granted worker reached the \
moniker through the winecom proxy runtime"

# ---- 5: the error class this gate was written for ------------------------
# Named by its own text, so that a future change which silences the message
# without changing the behaviour cannot quietly pass this.
# Counted, not merely present-or-absent: each count below is one refused
# CoCreateInstance the case is designed to make, so a count that drifts means
# a thread was granted or refused an apartment somewhere this gate did not
# ask about.
ERRCLASS="err:ole:com_get_class_object apartment not initialised"
errs_noinit=2   # the main thread and the worker, neither initialised
errs_mta=0      # everyone is in the MTA
errs_sta=1      # main is an STA and succeeds; the worker is the one refused
errs_stamta=0   # the usage cookie keeps the MTA alive for the worker
errs_joiner=1   # only the probe made AFTER the joiner left the MTA
for c in $CASES; do
    eval want=\$errs_$c
    got=$(grep -cF "$ERRCLASS" "$OUT/guest.$c.err")
    if [ "$got" = "$want" ]; then
        say "error class $c: $got occurrence(s), as expected"
    else
        grep -F "$ERRCLASS" "$OUT/guest.$c.err" | head -5 >&2
        bad "case $c logged the DOOM error $got time(s), expected $want"
    fi
done
[ $fail -eq 0 ] && say "error class: reproduced exactly where Windows also \
refuses (sta, joiner-after-exit, noinit) and gone everywhere an MTA exists"

[ $fail -eq 0 ] && say "PASS"
exit $fail
