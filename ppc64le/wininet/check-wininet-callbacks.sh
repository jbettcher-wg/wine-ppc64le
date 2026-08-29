#!/bin/sh
#
# check-wininet-callbacks.sh -- the RUNTIME gate for wininet's five-argument
# INTERNET_STATUS_CALLBACK.
#
# dlls/wininet/guestthunk.c and ntdll's __wine_guest_wrap_callback5 landed
# together and were proved only STRUCTURALLY -- the wrapper exists, it
# resolves the factory, the factory mints a five-argument slot.  That says
# nothing about the callback ever being invoked, and the defect this
# mechanism exists to prevent is not a missing call: it is a call whose FIFTH
# argument is the trampoline's own target pointer instead of
# dwStatusInformationLength, arriving on a wininet worker thread with no
# guest frame beneath it.  Nothing faults.  The guest reads a pointer as a
# byte count.
#
# THE SERVER IS PART OF THE GATE.  ppc64le/wininet/loopback_server.py is
# spawned here, binds 127.0.0.1 on an EPHEMERAL port, prints the port it got,
# and exits by itself once it has served its requests.  There is no network
# access anywhere in this gate and there must never be: a status callback
# driven by the open internet would be neither reproducible nor allowed to
# fail for reasons belonging to somebody else's DNS.  The ephemeral port is
# what lets two copies of this gate run at once.
#
# WHAT IS ACTUALLY CHECKED, AND WHY IT IS THREE NUMBERS.  Wine's own wininet
# gives dwStatusInformationLength three different values for the statuses this
# probe drives, each derivable at compile time from Wine's source:
#
#   RESOLVING_NAME     20  the host as WCHAR, (9+1)*2 -- the probe registers
#                          the W callback precisely so this stays wide;
#                          dlls/wininet/utility.c:260 rewrites it to strlen+1
#                          for an ANSI registration, which would collapse it
#                          onto the next value and weaken the check
#   NAME_RESOLVED      10  the resolved address as ANSI, strlen("127.0.0.1")+1
#                          (dlls/wininet/http.c:1778)
#   REQUEST_COMPLETE   16  an INTERNET_ASYNC_RESULT
#
# 20, 10, 16.  A fifth argument that is really a pointer, or a constant, or a
# register nobody wrote, cannot be all three.  The CONTENTS are checked too --
# the NAME_RESOLVED payload must literally be "127.0.0.1" -- and so is
# dwContext, the second argument, which carries bits above 32 and must come
# back unchanged on every single callback.  (dwContext also has to be
# non-zero: INTERNET_SendCallback returns early on a zero one, so a probe
# passing zero would see NO callbacks and could mistake that for calm.)
#
# ASYNC ON PURPOSE: INTERNET_FLAG_ASYNC makes wininet run the request on its
# own worker thread and call back from there, which is the case
# guestthunk.c's header singles out as worse than the enumeration callbacks.
#
# Layers:
#
#   1  BUILD/IMPORTS: the guest PE imports each wininet entry point from
#      wininet.dll specifically.
#   2  NATIVE: the probe as a native ppc64 Windows PE -- no boundary, no
#      trampoline in the process -- drives the same loopback server and
#      reports PASS.  This is what makes the three lengths facts about
#      wininet rather than this gate's opinion.
#   3  GUEST: the x86-64 guest PE under the emulator reports PASS.
#   4  IDENTITY: diff(native, guest) is empty.  The probe prints no address
#      and no port, so every line is a value wininet computed.
#   5  MECHANISM: a +guestcb run of the guest leg must show
#      dlls/wininet/guestthunk.c's own "-> five-argument trampoline" TRACE.
#      Without it a guest that reached the right answers by some other route
#      would still pass layers 1-4.
#
# --sabotage runs the port's own negative control: WINEEMUNOCBWRAP=1 makes
# wrap_guest_callback_ex hand the RAW guest pointer to native wininet -- the
# defect itself -- and the guest run MUST then fail.  It is expected to die
# rather than merely mis-report, because native wininet will bctrl into
# x86-64 bytes from its worker thread; either way it must not print PASS, and
# the port must say something.
#
# WHY EVERY RUN DISABLES winedbg, verbatim from check-guest-callbacks.sh: the
# bringup prefix has AeDebug configured with "winedbg --auto", so an unhandled
# guest fault -- which is exactly what the sabotage leg is for -- would
# otherwise start a debugger that attaches and never lets go, turning a red
# state into a hang.
#
# Exit 0 = pass, 1 = a check failed, 2 = could not run (not a pass).
set -u

HERE=$(cd "$(dirname "$0")" && pwd)
SRC=$(cd "$HERE/.." && pwd)
SRC=$(cd "$SRC/.." && pwd)
BUILD=${BUILD:-$SRC}
OUT=${OUT:-/tmp/wininet-callbacks}
SABOTAGE=0
[ "${1:-}" = "--sabotage" ] && SABOTAGE=1

say()  { echo "check-wininet-callbacks: $*"; }
bad()  { echo "check-wininet-callbacks: FAIL $*" >&2; fail=1; }
skip() { echo "check-wininet-callbacks: $*" >&2; exit 2; }

[ -x "$BUILD/wine" ] || skip "no wine loader at $BUILD/wine"
[ -n "${WINEPREFIX:-}" ] || skip "set WINEPREFIX to a prefix wineboot has run in"
[ -n "${WINEFEXBRIDGE:-}" ] || skip "set WINEFEXBRIDGE to the emulator bridge"
[ -f "$BUILD/dlls/wininet/x86_64-windows/wininet.dll" ] || \
    skip "no guest wininet thunk; build it first"
[ -f "$BUILD/dlls/wininet/ppc64-windows/libwininet.a" ] || \
    skip "no native wininet import library; build it first"
command -v clang >/dev/null || skip "need clang for the guest build"
command -v llvm-dlltool >/dev/null || skip "need llvm-dlltool for the guest build"
command -v llvm-readobj >/dev/null || skip "need llvm-readobj to read the built image"
command -v python3 >/dev/null || skip "need python3 for the loopback server"

mkdir -p "$OUT" || skip "cannot create $OUT"
fail=0

INCL="-I$BUILD/include -I$SRC/include -I$SRC/include/msvcrt"
TIMEOUT=${TIMEOUT:-120}

# ---- build 1: the native ppc64 Windows PE ---------------------------------
# check-com-smoke.sh's recipe with wininet in place of ole32.
${CC:-gcc} -c -o "$OUT/wininet_status.o" "$HERE/wininet_status.c" $INCL \
    -D_UCRT -D_WIN32 -Wall -pipe -mlongcall -mno-pltseq \
    -fcf-protection=none -fvisibility=hidden -fno-stack-protector \
    -fno-strict-aliasing -gdwarf-4 -fPIC -fasynchronous-unwind-tables \
    -mlong-double-64 -fno-builtin -fshort-wchar -Wno-format -g -O2 \
    || skip "native compile failed"

"$BUILD/tools/winegcc/winegcc" -o "$OUT/wininet_status.exe" --wine-objdir "$BUILD" \
    --cc-cmd="${CC:-gcc}" -mno-cygwin -fPIC -fasynchronous-unwind-tables \
    -Wl,--wine-builtin -mconsole "$OUT/wininet_status.o" \
    "$BUILD/libs/winecrt0/ppc64-windows/libwinecrt0.a" \
    "$BUILD/dlls/wininet/ppc64-windows/libwininet.a" \
    "$BUILD/dlls/ucrtbase/ppc64-windows/libucrtbase.a" \
    "$BUILD/dlls/kernel32/ppc64-windows/libkernel32.a" \
    "$BUILD/dlls/ntdll/ppc64-windows/libntdll.a" || skip "native link failed"
rm -f "$OUT/wininet_status.exe"
"$SRC/tools/elf2pe" "$OUT/wininet_status.exe.so" "$OUT/wininet_status.exe" \
    || skip "elf2pe failed"
"$BUILD/tools/winebuild/winebuild" --builtin "$OUT/wininet_status.exe" \
    || skip "winebuild --builtin failed"

# ---- build 2: the x86-64 guest PE ----------------------------------------
cat > "$OUT/wininet.def" <<'EOF'
LIBRARY wininet.dll
EXPORTS
InternetOpenW
InternetSetStatusCallbackW
InternetOpenUrlW
InternetReadFile
InternetCloseHandle
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
CloseHandle
GetLastError
InterlockedIncrement
EOF
for m in wininet kernel32; do
    llvm-dlltool -m i386:x86-64 -d "$OUT/$m.def" -l "$OUT/lib$m.a" \
        || skip "llvm-dlltool failed for $m"
done

GUESTCC="clang -target x86_64-windows-gnu -nostdlibinc $INCL -fms-extensions \
-D_UCRT -DWININET_STATUS_NO_CRT -Wall -O1 -fno-builtin -g"
GUESTLD="clang -target x86_64-windows-gnu -fuse-ld=lld -nostdlib \
-Wl,--entry=wininet_status_entry -Wl,--subsystem,console"

$GUESTCC -c -o "$OUT/wininet_status_guest.o" "$HERE/wininet_status.c" \
    || skip "guest compile failed"
$GUESTLD -o "$OUT/wininet_status_guest.exe" "$OUT/wininet_status_guest.o" \
    "$OUT/libwininet.a" "$OUT/libkernel32.a" \
    || skip "guest link failed"

GEXE="$OUT/wininet_status_guest.exe"
NEXE="$OUT/wininet_status.exe"

WDBG=${WINEDEBUG:--all},err+seh

# ---- the loopback server -------------------------------------------------
# Started per run, port read off its first line, and reaped afterwards.  It
# exits on its own once it has served --requests, so the kill below is a
# belt-and-braces cleanup for the case where the probe never connected --
# never the normal path, and never a pkill: the pid is exactly the one this
# function started.
SERVER_PID=""
SERVER_PORT=""
start_server() {
    rm -f "$OUT/server.out"
    python3 "$HERE/loopback_server.py" --requests "${1:-2}" --timeout 90 \
        > "$OUT/server.out" 2>"$OUT/server.err" &
    SERVER_PID=$!
    # Wait for the PORT line rather than sleeping: the probe cannot be started
    # before the listener exists, and a fixed sleep would be either flaky or
    # slow.
    i=0
    while [ $i -lt 100 ]; do
        SERVER_PORT=$(awk '/^PORT /{print $2; exit}' "$OUT/server.out" 2>/dev/null)
        [ -n "$SERVER_PORT" ] && break
        i=$((i + 1))
        sleep 0.1
    done
    if [ -z "$SERVER_PORT" ]; then
        cat "$OUT/server.err" >&2
        return 1
    fi
    return 0
}
stop_server() {
    [ -n "$SERVER_PID" ] || return 0
    kill "$SERVER_PID" 2>/dev/null
    wait "$SERVER_PID" 2>/dev/null
    SERVER_PID=""
}

run_probe() {   # $1 = exe, $2 = stdout file, $3 = stderr file, $4.. = extra env
    start_server 2 || { bad "the loopback server did not start"; return 1; }
    timeout -k 5 "$TIMEOUT" \
        env WINEDEBUG="$WDBG" WINEDLLOVERRIDES="winedbg.exe=d" \
        "$BUILD/wine" "$1" "$SERVER_PORT" >"$2" 2>"$3"
    rc=$?
    stop_server
    return $rc
}

# ---- --sabotage: the raw-pointer path, switched back on -------------------
sabotage_cbwrap() {
    start_server 2 || { bad "the loopback server did not start"; return; }
    started=$(date +%s)
    timeout -k 5 "${DEADLINE:-40}" \
        env WINEDEBUG="$WDBG" WINEDLLOVERRIDES="winedbg.exe=d" WINEEMUNOCBWRAP=1 \
        "$BUILD/wine" "$GEXE" "$SERVER_PORT" \
        >"$OUT/cbwrap.out" 2>"$OUT/cbwrap.err"
    st=$?
    elapsed=$(( $(date +%s) - started ))
    stop_server
    if [ $st -eq 124 ] || [ $st -eq 137 ]; then
        bad "WINEEMUNOCBWRAP=1 HUNG (killed after ${DEADLINE:-40}s); the raw-pointer \
path must fail promptly, not hang"
        tail -10 "$OUT/cbwrap.err" | sed 's/^/  cbwrap| /' >&2
        return
    fi
    if [ $st -eq 0 ]; then
        bad "WINEEMUNOCBWRAP=1 exited 0; the raw-pointer path must not be a silent success"
    else
        say "WINEEMUNOCBWRAP=1: exited $st after ${elapsed}s"
    fi
    if ! grep -q "^wininet_status: start" "$OUT/cbwrap.out"; then
        bad "WINEEMUNOCBWRAP=1 never reached the probe's first marker; it died \
before the thing under test and proves nothing"
    fi
    if grep -q "wininet_status: PASS" "$OUT/cbwrap.out"; then
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
diagnostics"
    fi
}

if [ "$SABOTAGE" = 1 ]; then
    sabotage_cbwrap
    [ $fail -eq 0 ] && say "SABOTAGE PASS"
    exit $fail
fi

# ---- 1: build/shape -------------------------------------------------------
llvm-readobj --coff-imports "$GEXE" > "$OUT/imports.txt" 2>&1
imported_from() {
    awk -v s="$1" '/Name: .*\.dll/ { dll = $2 }
                   $0 ~ ("Symbol: " s " ") { print dll }' "$OUT/imports.txt"
}
for want in \
    "InternetOpenW wininet.dll" \
    "InternetSetStatusCallbackW wininet.dll" \
    "InternetOpenUrlW wininet.dll" \
    "InternetReadFile wininet.dll" \
    "InternetCloseHandle wininet.dll"
do
    sym=${want% *}; dll=${want#* }
    if imported_from "$sym" | grep -qx "$dll"; then
        say "image: $sym is imported from $dll"
    else
        bad "the guest exe does not import $sym from $dll (imports seen: \
$(imported_from "$sym" | tr '\n' ' '))"
    fi
done

# ---- 2: native ------------------------------------------------------------
run_probe "$NEXE" "$OUT/native.out" "$OUT/native.err"
nst=$?
if grep -q "wininet_status: PASS" "$OUT/native.out"; then
    say "native: $(tail -1 "$OUT/native.out")"
else
    sed 's/^/  native| /' "$OUT/native.out" >&2
    tail -20 "$OUT/native.err" >&2
    bad "the native ppc64 build did not pass (exit $nst)"
fi

# ---- 3: guest -------------------------------------------------------------
run_probe "$GEXE" "$OUT/guest.out" "$OUT/guest.err"
gst=$?
if [ $gst -eq 124 ] || [ $gst -eq 137 ]; then
    bad "the guest run timed out after ${TIMEOUT}s"
elif grep -q "wininet_status: PASS" "$OUT/guest.out"; then
    say "guest: $(tail -1 "$OUT/guest.out")"
else
    sed 's/^/  guest| /' "$OUT/guest.out" >&2
    tail -30 "$OUT/guest.err" >&2
    bad "the x86-64 guest build did not pass (exit $gst)"
fi

# ---- 4: identity ----------------------------------------------------------
if diff -u "$OUT/native.out" "$OUT/guest.out" > "$OUT/main.diff" 2>&1; then
    say "identity: native and guest transcripts are byte-identical \
($(wc -l < "$OUT/native.out") lines)"
else
    sed 's/^/  diff| /' "$OUT/main.diff" >&2
    bad "the native and guest transcripts differ; a value crossed the boundary wrong"
fi

# ---- 5: mechanism ---------------------------------------------------------
# guestthunk.c's own channel, so this cannot pass on a run that reached the
# right answers without the five-argument wrapper.
start_server 2 || bad "the loopback server did not start for the trace run"
if [ -n "$SERVER_PORT" ]; then
    env WINEDEBUG="${WINEDEBUG:--all},trace+guestcb" WINEDLLOVERRIDES="winedbg.exe=d" \
        timeout -k 5 "$TIMEOUT" "$BUILD/wine" "$GEXE" "$SERVER_PORT" \
        >"$OUT/trace.out" 2>"$OUT/trace.err"
    stop_server
    n5=$(grep -c 'five-argument trampoline' "$OUT/trace.err" 2>/dev/null || echo 0)
    if [ "$n5" -ge 1 ]; then
        say "mechanism: wininet wrapped the callback into a five-argument \
trampoline $n5 time(s): $(grep -m1 'five-argument trampoline' "$OUT/trace.err" | cut -c1-110)"
    else
        tail -20 "$OUT/trace.err" | sed 's/^/  trace| /' >&2
        bad "dlls/wininet/guestthunk.c never logged a five-argument trampoline; \
the callback did not go through the wrapper this gate exists for"
    fi
fi

[ $fail -eq 0 ] && say "PASS"
exit $fail
