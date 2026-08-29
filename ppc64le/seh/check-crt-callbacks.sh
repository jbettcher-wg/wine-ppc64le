#!/bin/sh
#
# check-crt-callbacks.sh -- the RUNTIME gate for the five C-runtime
# registration points dlls/msvcr100/msvcr100.thunks used to list as STILL
# OPEN: _beginthread's and _beginthreadex's start routines,
# _set_invalid_parameter_handler, _set_purecall_handler and __setusermatherr.
# Styx: Master of Shadows imports all five from MSVCR100.dll.
#
# Structurally this is check-com-smoke.sh's shape rather than
# check-guest-callbacks.sh's: crt_callbacks.c is built TWICE from one source
# -- once as a native ppc64 Windows PE, where no boundary and no trampoline
# exist anywhere in the process, and once as an x86-64 guest PE under the
# emulator -- and the two transcripts must be BYTE IDENTICAL.  The probe
# prints no address, handle or thread id, so every line is a value the C
# runtime itself computed.  The native leg is what makes the numbers this
# gate asserts the CRT's own answers rather than the probe's opinion.
#
# WHAT WOULD BE MISSED WITHOUT VALUE CHECKS.  Three of the five callbacks
# return void and take arguments the caller never looks at again, so a wrong
# row can very easily not crash:
#
#   * _set_invalid_parameter_handler's callback takes FIVE arguments.  The
#     trampoline pool puts the guest target in the register one past the last
#     real argument, which at the pool's default four-argument arity is r7 --
#     ELFv2's FIFTH argument register, the one native _invalid_parameter has
#     already loaded with pReserved.  A row minted at the default arity
#     therefore hands the handler the target pointer where pReserved belongs
#     and passes four arguments instead of five.  Nothing faults.  Step 3
#     passes a pReserved with bits set in both halves and requires it back
#     exactly; that is the only way this is visible from inside a guest, and
#     it is why the row carries an explicit cb_argc of 5.
#
#     AND IT IS REGISTERED TWICE, through the same export, which is a separate
#     claim.  One registration only ever exercises the thunk cache's MISS path.
#     cb_argc was first threaded through find_guest_thunk_target but NOT through
#     thunk_rip_cache_get/put -- the two functions that actually copy the entry
#     field by field -- so the arity was dropped on a miss and read back as
#     uninitialised stack garbage on a HIT.  Garbage outside {4,5,6} takes
#     wrap_guest_callback_ex's default: arm, which hands the RAW guest pointer
#     to native code, the exact failure this mechanism exists to prevent;
#     garbage that happens to be 5 or 6 mints the wrong trampoline in silence.
#     Steps 4 and 5 register a SECOND, DIFFERENT guest handler through the same
#     call site -- same RIP, therefore a cache hit -- and require it to receive
#     all five arguments too, with a different line and a different pReserved
#     so a stale witness cannot satisfy them.  A different FUNCTION, not the
#     same one again: the pool is idempotent per (target, width, arity), so
#     re-registering the first handler would mint nothing and test nothing.
#
#   * __setusermatherr's callback is the one whose RETURN is read --
#     dlls/msvcrt/math.c:129 treats it as "I handled it".  Steps 5 and 6 run
#     the same domain error twice, with the handler claiming and then
#     declining, and require the two to produce DIFFERENT results.  A gate
#     that only ran the handler once would pass with the return value
#     dropped entirely.
#
#   * _beginthread and _beginthreadex are NOT reached by the invocation-time
#     thread-start interception in RtlUserThreadStart, which is what covers
#     CreateThread: dlls/msvcrt/thread.c hands CreateThread its own NATIVE
#     trampoline and calls the guest routine from inside it, so the thread
#     entry the port classifies is native and the guest pointer is never
#     inspected.  Steps 9 and 10 check the argument each start routine
#     received by identity, and step 9 additionally reads the thread's exit
#     code back, because that is the only place _beginthreadex's 32-bit
#     return surfaces.
#
# TWO RUNS PER LEG, because _purecall does not return: dlls/msvcrt/exit.c:505
# calls the handler and then _amsg_exit(25).  So the probe takes a mode word
# on its command line, and the pure-virtual leg is a second invocation whose
# handler prints its own witness and exits with a status (77) that only the
# guest handler running can produce.  Depending on _amsg_exit's message or
# status instead would be pinning something that is not this gate's to pin.
#
# Layers:
#
#   1  BUILD/IMPORTS: the guest PE compiles, links, and imports each CRT entry
#      point from msvcr100.dll specifically.  This is the layer that catches a
#      probe which silently bound to some other runtime and then passed
#      everything below for the wrong reason.
#   2  NATIVE: the native ppc64 PE runs both modes and reports PASS / the
#      purecall witness.  Establishes the expected bytes with the guest lane
#      out of the picture entirely.
#   3  GUEST: the guest PE runs both modes under the emulator and reports the
#      same.
#   4  IDENTITY: diff(native, guest) is empty for BOTH modes, and the purecall
#      mode exits 77 on both legs.
#   5  MECHANISM: a +seh run of the guest leg must show wrap_guest_callback_ex
#      minting trampolines, and among them at least one "5 args" slot -- the
#      _invalid_parameter_handler row's arity, which no other callback in this
#      probe uses.  Without this layer a port that reached the right answers
#      by some other route would still pass layers 1-4.
#
# --sabotage runs the port's own negative control: WINEEMUNOCBWRAP=1
# (dlls/ntdll/signal_ppc64.c, wrap_guest_callback_ex) hands the RAW guest
# pointer to native code at every registration -- the defect these rows exist
# to fix, switched back on -- and the guest run MUST then fail: promptly, not
# by hanging, nonzero, never printing PASS, and with the port saying
# something.  A gate that cannot go red proves nothing.
#
# WHY EVERY RUN DISABLES winedbg, verbatim from check-guest-callbacks.sh: the
# bringup prefix has AeDebug configured with "winedbg --auto", so an unhandled
# guest fault -- and the sabotage leg's whole point is to produce one -- would
# otherwise start a debugger that attaches and never lets go, turning the red
# state this gate is SUPPOSED to reach into a hang.
#
# Exit 0 = pass, 1 = a check failed, 2 = could not run (not a pass).
set -u

HERE=$(cd "$(dirname "$0")" && pwd)
SRC=$(cd "$HERE/../.." && pwd)
BUILD=${BUILD:-$SRC}
OUT=${OUT:-/tmp/crt-callbacks}
SABOTAGE=0
[ "${1:-}" = "--sabotage" ] && SABOTAGE=1

say()  { echo "check-crt-callbacks: $*"; }
bad()  { echo "check-crt-callbacks: FAIL $*" >&2; fail=1; }
skip() { echo "check-crt-callbacks: $*" >&2; exit 2; }

[ -x "$BUILD/wine" ] || skip "no wine loader at $BUILD/wine"
[ -n "${WINEPREFIX:-}" ] || skip "set WINEPREFIX to a prefix wineboot has run in"
[ -n "${WINEFEXBRIDGE:-}" ] || skip "set WINEFEXBRIDGE to the emulator bridge"
[ -f "$BUILD/dlls/msvcr100/x86_64-windows/msvcr100.dll" ] || \
    skip "no guest msvcr100 thunk; build it first"
[ -f "$BUILD/dlls/msvcr100/ppc64-windows/libmsvcr100.a" ] || \
    skip "no native msvcr100 import library; build it first"
command -v clang >/dev/null || skip "need clang for the guest build"
command -v llvm-dlltool >/dev/null || skip "need llvm-dlltool for the guest build"
command -v llvm-readobj >/dev/null || skip "need llvm-readobj to read the built image"

mkdir -p "$OUT" || skip "cannot create $OUT"
fail=0

INCL="-I$BUILD/include -I$SRC/include -I$SRC/include/msvcrt"
TIMEOUT=${TIMEOUT:-120}

# ---- build 1: the native ppc64 Windows PE ---------------------------------
# The same recipe check-com-smoke.sh uses, with msvcr100 in place of ole32:
# the tree's own programs/winepath flags minus -D__WINESRC__, because this
# probe is an ordinary consumer of the public headers, which is also the only
# way the guest build can see them.  -D_MSVCR_VER=100 so both legs read the
# msvcr100 shape of the CRT headers -- notably that _invalid_parameter uses
# the global handler rather than the per-thread one, which is a _MSVCR_VER>=140
# path and would otherwise make the two legs test different code.
${CC:-gcc} -c -o "$OUT/crt_callbacks.o" "$HERE/crt_callbacks.c" $INCL \
    -D_UCRT -D_WIN32 -D_MSVCR_VER=100 -Wall -pipe -mlongcall -mno-pltseq \
    -fcf-protection=none -fvisibility=hidden -fno-stack-protector \
    -fno-strict-aliasing -gdwarf-4 -fPIC -fasynchronous-unwind-tables \
    -mlong-double-64 -fno-builtin -fshort-wchar -Wno-format -g -O2 \
    || skip "native compile failed"

"$BUILD/tools/winegcc/winegcc" -o "$OUT/crt_callbacks.exe" --wine-objdir "$BUILD" \
    --cc-cmd="${CC:-gcc}" -mno-cygwin -fPIC -fasynchronous-unwind-tables \
    -Wl,--wine-builtin -mconsole "$OUT/crt_callbacks.o" \
    "$BUILD/libs/winecrt0/ppc64-windows/libwinecrt0.a" \
    "$BUILD/dlls/msvcr100/ppc64-windows/libmsvcr100.a" \
    "$BUILD/dlls/kernel32/ppc64-windows/libkernel32.a" \
    "$BUILD/dlls/ntdll/ppc64-windows/libntdll.a" || skip "native link failed"
rm -f "$OUT/crt_callbacks.exe"
"$SRC/tools/elf2pe" "$OUT/crt_callbacks.exe.so" "$OUT/crt_callbacks.exe" \
    || skip "elf2pe failed"
"$BUILD/tools/winebuild/winebuild" --builtin "$OUT/crt_callbacks.exe" \
    || skip "winebuild --builtin failed"

# ---- build 2: the x86-64 guest PE ----------------------------------------
# Same clang x86_64-windows-gnu machinery tools/spec2thunk drives its signature
# oracle with, and the same Wine headers, so any disagreement between the two
# runs is the boundary and not the declarations.  The imports are described by
# hand rather than taken from a mingw sysroot: naming the DLL per symbol is
# what makes the guest bind to the same builtins a real guest application
# would, and there is no CRT startup linked in at all.
cat > "$OUT/msvcr100.def" <<'EOF'
LIBRARY msvcr100.dll
EXPORTS
_set_invalid_parameter_handler
_get_invalid_parameter_handler
_invalid_parameter
_set_purecall_handler
_get_purecall_handler
_purecall
__setusermatherr
sqrt
_beginthread
_beginthreadex
EOF
cat > "$OUT/kernel32.def" <<'EOF'
LIBRARY kernel32.dll
EXPORTS
GetStdHandle
WriteFile
ExitProcess
GetCommandLineW
CreateEventW
SetEvent
WaitForSingleObject
GetExitCodeThread
CloseHandle
EOF
for m in msvcr100 kernel32; do
    llvm-dlltool -m i386:x86-64 -d "$OUT/$m.def" -l "$OUT/lib$m.a" \
        || skip "llvm-dlltool failed for $m"
done

GUESTCC="clang -target x86_64-windows-gnu -nostdlibinc $INCL -fms-extensions \
-D_UCRT -D_MSVCR_VER=100 -DCRT_CALLBACKS_NO_CRT -Wall -O1 -fno-builtin -g"
GUESTLD="clang -target x86_64-windows-gnu -fuse-ld=lld -nostdlib \
-Wl,--entry=crt_callbacks_entry -Wl,--subsystem,console"

$GUESTCC -c -o "$OUT/crt_callbacks_guest.o" "$HERE/crt_callbacks.c" \
    || skip "guest compile failed"
$GUESTLD -o "$OUT/crt_callbacks_guest.exe" "$OUT/crt_callbacks_guest.o" \
    "$OUT/libmsvcr100.a" "$OUT/libkernel32.a" \
    || skip "guest link failed"

GEXE="$OUT/crt_callbacks_guest.exe"
NEXE="$OUT/crt_callbacks.exe"

# -all,err+seh: stdout stays clean enough to diff while the port's own
# callback-wrapping diagnostics still reach stderr.  Appended to any
# caller-supplied WINEDEBUG rather than replacing it, exactly as in
# check-guest-callbacks.sh, so a caller exporting -all does not turn a
# passing port into a false FAIL.
WDBG=${WINEDEBUG:--all},err+seh
run_wine() { timeout -k 5 "$TIMEOUT" \
                 env WINEDEBUG="$WDBG" WINEDLLOVERRIDES="winedbg.exe=d" \
                 "$BUILD/wine" "$@"; }

# ---- --sabotage: the raw-pointer path, switched back on -------------------
sabotage_cbwrap() {
    started=$(date +%s)
    timeout -k 5 "${DEADLINE:-30}" \
        env WINEDEBUG="$WDBG" WINEDLLOVERRIDES="winedbg.exe=d" WINEEMUNOCBWRAP=1 \
        "$BUILD/wine" "$GEXE" >"$OUT/cbwrap.out" 2>"$OUT/cbwrap.err"
    st=$?
    elapsed=$(( $(date +%s) - started ))
    if [ $st -eq 124 ] || [ $st -eq 137 ]; then
        bad "WINEEMUNOCBWRAP=1 HUNG (killed after ${DEADLINE:-30}s); the raw-pointer \
path must fail promptly, not hang"
        tail -10 "$OUT/cbwrap.err" | sed 's/^/  cbwrap| /' >&2
        return
    fi
    if [ $st -eq 0 ]; then
        bad "WINEEMUNOCBWRAP=1 exited 0; the raw-pointer path must not be a silent success"
    else
        say "WINEEMUNOCBWRAP=1: exited $st after ${elapsed}s"
    fi
    if ! grep -q "^crt_callbacks: start" "$OUT/cbwrap.out"; then
        bad "WINEEMUNOCBWRAP=1 never reached the probe's first marker; it died \
before the thing under test and proves nothing"
    fi
    if grep -q "crt_callbacks: PASS" "$OUT/cbwrap.out"; then
        bad "WINEEMUNOCBWRAP=1 still printed PASS; the raw-pointer defect did not \
reach anything this probe checks"
    fi
    if grep -qE "WINEEMUNOCBWRAP|c000001d|c0000005|ignoring exception|illegal instruction" \
            "$OUT/cbwrap.err"; then
        say "WINEEMUNOCBWRAP=1: the port said something: $(grep -Eim1 \
            'WINEEMUNOCBWRAP|c000001d|c0000005|ignoring exception|illegal instruction' \
            "$OUT/cbwrap.err" | cut -c1-120)"
    else
        sed 's/^/  cbwrap| /' "$OUT/cbwrap.err" >&2
        bad "WINEEMUNOCBWRAP=1 died without the port naming why anywhere in its \
diagnostics; a silent death here is exactly the failure mode this gate exists \
to make loud"
    fi
}

if [ "$SABOTAGE" = 1 ]; then
    sabotage_cbwrap
    [ $fail -eq 0 ] && say "SABOTAGE PASS"
    exit $fail
fi

# ---- 1: build/shape -------------------------------------------------------
# A probe that silently linked against the wrong runtime would pass every
# layer below for the wrong reason.
llvm-readobj --coff-imports "$GEXE" > "$OUT/imports.txt" 2>&1
imported_from() {   # $1 = symbol -> prints the DLL that provides it
    awk -v s="$1" '/Name: .*\.dll/ { dll = $2 }
                   $0 ~ ("Symbol: " s " ") { print dll }' "$OUT/imports.txt"
}
for want in \
    "_set_invalid_parameter_handler msvcr100.dll" \
    "_get_invalid_parameter_handler msvcr100.dll" \
    "_invalid_parameter msvcr100.dll" \
    "_set_purecall_handler msvcr100.dll" \
    "_get_purecall_handler msvcr100.dll" \
    "_purecall msvcr100.dll" \
    "__setusermatherr msvcr100.dll" \
    "sqrt msvcr100.dll" \
    "_beginthread msvcr100.dll" \
    "_beginthreadex msvcr100.dll"
do
    sym=${want% *}; dll=${want#* }
    if imported_from "$sym" | grep -qx "$dll"; then
        say "image: $sym is imported from $dll"
    else
        bad "the guest exe does not import $sym from $dll; the layer that needs \
it would be silently untested (imports seen: $(imported_from "$sym" | tr '\n' ' '))"
    fi
done

# ---- 2: native ------------------------------------------------------------
run_wine "$NEXE" > "$OUT/native.out" 2>"$OUT/native.err"
nst=$?
if grep -q "crt_callbacks: PASS" "$OUT/native.out"; then
    say "native: $(tail -1 "$OUT/native.out")"
else
    sed 's/^/  native| /' "$OUT/native.out" >&2
    tail -20 "$OUT/native.err" >&2
    bad "the native ppc64 build did not pass (exit $nst)"
fi

run_wine "$NEXE" purecall > "$OUT/native.pure.out" 2>"$OUT/native.pure.err"
npst=$?
if [ $npst -eq 77 ] && grep -q "^purecall handler ran$" "$OUT/native.pure.out"; then
    say "native purecall: handler ran, exit 77"
else
    sed 's/^/  native-pure| /' "$OUT/native.pure.out" >&2
    bad "the native purecall leg did not run its handler and exit 77 (exit $npst)"
fi

# ---- 3: guest -------------------------------------------------------------
run_wine "$GEXE" > "$OUT/guest.out" 2>"$OUT/guest.err"
gst=$?
if [ $gst -eq 124 ] || [ $gst -eq 137 ]; then
    bad "the guest run timed out after ${TIMEOUT}s"
elif grep -q "crt_callbacks: PASS" "$OUT/guest.out"; then
    say "guest: $(tail -1 "$OUT/guest.out")"
else
    sed 's/^/  guest| /' "$OUT/guest.out" >&2
    tail -30 "$OUT/guest.err" >&2
    bad "the x86-64 guest build did not pass (exit $gst)"
fi

run_wine "$GEXE" purecall > "$OUT/guest.pure.out" 2>"$OUT/guest.pure.err"
gpst=$?
if [ $gpst -eq 77 ] && grep -q "^purecall handler ran$" "$OUT/guest.pure.out"; then
    say "guest purecall: handler ran as guest code, exit 77"
else
    sed 's/^/  guest-pure| /' "$OUT/guest.pure.out" >&2
    tail -20 "$OUT/guest.pure.err" >&2
    bad "the guest purecall leg did not run its handler and exit 77 (exit $gpst)"
fi

# ---- 4: identity ----------------------------------------------------------
if diff -u "$OUT/native.out" "$OUT/guest.out" > "$OUT/main.diff" 2>&1; then
    say "identity: native and guest transcripts are byte-identical ($(wc -l < "$OUT/native.out") lines)"
else
    sed 's/^/  diff| /' "$OUT/main.diff" >&2
    bad "the native and guest transcripts differ; a value crossed the boundary wrong"
fi
if diff -u "$OUT/native.pure.out" "$OUT/guest.pure.out" > "$OUT/pure.diff" 2>&1; then
    say "identity: purecall transcripts are byte-identical"
else
    sed 's/^/  diff| /' "$OUT/pure.diff" >&2
    bad "the native and guest purecall transcripts differ"
fi

# ---- 5: mechanism ---------------------------------------------------------
# The port's own trace, so that a guest which somehow reached the right
# answers without the trampolines cannot pass.  The "5 args" slot is the
# _invalid_parameter_handler row and nothing else in this probe mints one, so
# its presence is the direct evidence for cb_argc.
env WINEDEBUG="${WINEDEBUG:--all},trace+seh" WINEDLLOVERRIDES="winedbg.exe=d" \
    timeout -k 5 "$TIMEOUT" "$BUILD/wine" "$GEXE" \
    >"$OUT/trace.out" 2>"$OUT/trace.err"
n_tramp=$(grep -c 'guest callback .* -> trampoline' "$OUT/trace.err" 2>/dev/null || echo 0)
n_five=$(grep -c 'guest callback .* -> trampoline .*, 5 args)' "$OUT/trace.err" 2>/dev/null || echo 0)
if [ "$n_tramp" -ge 5 ]; then
    say "mechanism: the port minted $n_tramp callback trampolines"
else
    tail -20 "$OUT/trace.err" | sed 's/^/  trace| /' >&2
    bad "the port's own +seh trace shows only $n_tramp callback trampolines; this \
probe registers five distinct callbacks and each needs one"
fi
if [ "$n_five" -ge 1 ]; then
    say "mechanism: $n_five trampoline(s) minted at 5 args -- the \
_invalid_parameter_handler row's arity, which nothing else here uses"
else
    grep 'guest callback .* -> trampoline' "$OUT/trace.err" | sed 's/^/  trace| /' >&2
    bad "no 5-argument trampoline was minted; _set_invalid_parameter_handler's \
row is not carrying cb_argc=5, and its callback is being handed the target \
pointer where pReserved belongs"
fi

[ $fail -eq 0 ] && say "PASS"
exit $fail
