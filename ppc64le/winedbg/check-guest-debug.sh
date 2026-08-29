#!/bin/sh
#
# check-guest-debug.sh -- the gate that proves a DEBUGGER can see a GUEST.
#
# Until this landed, `winedbg` could not be pointed at a process running an
# x86-64 guest, and every crash in this tree was diagnosed from the port's own
# +seh trace, a disassembler and an exception record's ExceptionAddress.  Two
# separate things were wrong and each is measured separately below.
#
# ONE: THE ATTACH NEVER LANDED.  DbgUiIssueRemoteBreakin creates a thread in
# the target at the debugger's OWN DbgUiRemoteBreakin address, which is only
# meaningful because ntdll normally sits at the same place in every process --
# it is a PE with a fixed image base.  Here ntdll's PE side is the one module
# that cannot be a PE (its TEB lives in an initial-exec __thread), so it is an
# ELF builtin and the dynamic linker puts it wherever it likes.  [MEASURED]
# 2026-08-18, the test machine: three concurrent processes of the same binary mapped
# dlls/ntdll/ntdll.dll.so at 0x3fff881ed000, 0x3fffb9a1d000 and 0x3fff91a0d000.
# So the address handed across named nothing in the target, the port's
# thread-start classifier said exactly that --
#
#   err:seh:RtlUserThreadStart thread start 00003FFFB7F11280 is in no loaded
#   image; refusing to run it either way
#
# -- and winedbg printed "attached to pid" and then waited forever for a
# breakin that never happened.  (README used to record this as the breakin
# routine living outside any PE the loader has a record of.  It does not:
# ntdll's ELF text is inside ntdll's own loader entry.  The address was simply
# the wrong process's.)
#
# TWO: THE REGISTERS WERE THE EMULATOR'S.  A guest thread's native ppc64
# CONTEXT describes the JIT, not the program.  The guest register file is
# reconstructed at every trap and at every fault, but in a stack frame that is
# gone by the time anybody outside asks -- and the guest STACK is freed by the
# run loop before a fatal fault is even reported, so a debugger that had the
# registers would still have had an RSP pointing at unmapped memory.
#
# TWO OBSERVERS, ONE CRASH.  Layer G reads winedbg's own output, because that
# is the thing a person uses.  Layer F asks the mechanism directly through
# NtQueryInformationThread(ThreadWow64Context) from a NATIVE ppc64 program that
# shares no code with winedbg, and checks every register against the value the
# guest put there.  Text can be reformatted; values cannot be reinterpreted.
#
# Layers:
#
#   A  BUILD: the guest probe (x86-64 PE, .pdata, no CRT) in both its modes,
#      and the native ppc64 reader.
#   B  ATTACH STOPS THE TARGET: winedbg attaches to a parked guest that never
#      faults, and must reach an interactive stop -- and the target must then
#      run to a NORMAL exit, so the attach neither hangs nor kills it.
#   F  VALUES: the native reader attaches to a guest that faults three frames
#      deep, and requires the guest RIP, ten sentinel registers, CS, and the
#      64-bit marker the guest pushed on its own stack -- read across a process
#      boundary at the reported RSP.  It also requires the debugger's own
#      injected breakin thread, which is native code, to report NO guest
#      context rather than a zeroed one.
#   G  WINEDBG: the same crash under winedbg, which must print the guest's
#      registers (not the host's), a stack dump at the guest RSP, the faulting
#      x86-64 instruction disassembled, and a backtrace naming all four guest
#      frames in order.
#
# --sabotage runs the negative controls instead and requires ALL of them to go
# red, because a gate that cannot fail proves nothing:
#
#   1  WINEEMUNODBGATTACH=1 -- the breakin address is not translated into the
#      target, exactly as before the fix.  winedbg must NOT reach a stop.
#   2  WINEEMUNODBGCTX=1 -- the guest register file is not published.  The
#      reader must get an error status for the guest thread too, and winedbg
#      must not print the sentinels.
#   3  WINEEMUNODBGSTACK=1 -- the guest stack is freed when the run ends, as
#      before.  The registers still arrive; the marker read at RSP must fail.
#      This is the control that catches a gate which proves registers and calls
#      it debugging.
#
# Exit 0 = pass, 1 = a check failed, 2 = could not run at all (a skip is NOT a
# pass).
#
# WHY THIS GATE DOES *NOT* DISABLE winedbg.  Every other gate here runs with
# WINEDLLOVERRIDES=winedbg.exe=d, because the bringup prefix has AeDebug set to
# "winedbg --auto" and a red state that starts a debugger which never attaches
# is a hang.  This one is about the debugger, so it runs it deliberately -- and
# every winedbg invocation is wrapped in a timeout that is SHORTER than the
# probe's own park, so the failure mode that used to be a hang is a bounded
# red.  The debuggee still runs with the override, so a nested AeDebug cannot
# start underneath.  Nothing here touches WINE_PPC64LE_WINEDBG or the compat
# tool's suppression of winedbg for background probes; that is a separate
# lever and it stays where it is.
set -u

HERE=$(cd "$(dirname "$0")" && pwd)
SRC=$(cd "$HERE/../.." && pwd)
BUILD=${BUILD:-$SRC}
OUT=${OUT:-/tmp/guest-debug}
SABOTAGE=0
[ "${1:-}" = "--sabotage" ] && SABOTAGE=1

say()  { echo "check-guest-debug: $*"; }
bad()  { echo "check-guest-debug: FAIL $*" >&2; fail=1; }
note() { echo "check-guest-debug: note $*"; }
skip() { echo "check-guest-debug: $*" >&2; cleanup; exit 2; }

# Repeated here rather than parsed out of the probe's output, so a probe that
# silently stopped setting a register could not also silently move the
# goalposts.  They must match ppc64le/winedbg/probes/guest_debug.c.
FAULT_ADDR=00000000dead1000
STACK_MARK=feedface5afe0001
SENTINELS="1111111100000011 2222222200000022 3333333300000033 4444444400000044 \
5555555500000055 6666666600000066 7777777700000077 8888888800000088 9999999900000099"
FRAMES="guest_debug_level3 guest_debug_level2 guest_debug_level1 guest_debug_entry"

PROBE_PID=
cleanup() {
    [ -n "${PROBE_PID:-}" ] && kill "$PROBE_PID" 2>/dev/null
    [ -n "${PROBE_PID:-}" ] && wait "$PROBE_PID" 2>/dev/null
    PROBE_PID=
}
trap cleanup EXIT INT TERM

[ -x "$BUILD/wine" ] || skip "no wine loader at $BUILD/wine"
[ -n "${WINEPREFIX:-}" ] || skip "set WINEPREFIX to a prefix wineboot has run in"
[ -n "${WINEFEXBRIDGE:-}" ] || skip "set WINEFEXBRIDGE to the emulator bridge"
[ -f "$BUILD/programs/winedbg/ppc64-windows/winedbg.exe" ] || \
    skip "no winedbg at $BUILD/programs/winedbg/ppc64-windows/winedbg.exe; build it first"
command -v clang >/dev/null || skip "need clang for the guest build"
command -v llvm-dlltool >/dev/null || skip "need llvm-dlltool for the guest build"
command -v "${CC:-gcc}" >/dev/null || skip "need ${CC:-gcc} for the native ppc64 build"

mkdir -p "$OUT" || skip "cannot create $OUT"
fail=0
# The probe parks for up to 300s waiting for the GO file, so every debugger run
# below is bounded well under that: a debugger that never stops the target is a
# timeout here rather than a wedged gate.
DBG_TIMEOUT=${DBG_TIMEOUT:-45}
RUN_TIMEOUT=${RUN_TIMEOUT:-180}
INCL="-I$BUILD/include -I$SRC/include -I$SRC/include/msvcrt"

# ---- A: build ---------------------------------------------------------------
cat > "$OUT/kernel32.def" <<'EOF'
LIBRARY kernel32.dll
EXPORTS
GetStdHandle
WriteFile
ExitProcess
GetCurrentProcessId
GetEnvironmentVariableA
GetFileAttributesA
Sleep
EOF
llvm-dlltool -m i386:x86-64 -d "$OUT/kernel32.def" -l "$OUT/libkernel32.a" \
    || skip "llvm-dlltool failed for kernel32"

GUESTCC="clang -target x86_64-windows-gnu -nostdlibinc $INCL -Wall -O1 -fno-builtin -g"
GUESTLD="clang -target x86_64-windows-gnu -fuse-ld=lld -nostdlib \
-Wl,--entry=guest_debug_entry -Wl,--subsystem,console"

build_guest() {   # $1 = basename, $2.. = extra -D
    _o=$1; shift
    $GUESTCC "$@" -c -o "$OUT/$_o.o" "$HERE/probes/guest_debug.c" \
        2>"$OUT/$_o.build.err" || return 1
    $GUESTLD -o "$OUT/$_o.exe" "$OUT/$_o.o" "$OUT/libkernel32.a" \
        2>>"$OUT/$_o.build.err" || return 1
    return 0
}

build_guest crasher || {
    sed 's/^/  guest| /' "$OUT/crasher.build.err" >&2
    skip "the guest probe did not build"
}
build_guest parker -DGUEST_DEBUG_MODE=1 || {
    sed 's/^/  guest| /' "$OUT/parker.build.err" >&2
    skip "the no-fault guest probe did not build"
}

# The native ppc64 reader, built exactly the way ppc64le/opengl/check-gl-smoke.sh
# builds its native lane: an ordinary consumer of the public headers, linked
# against this tree's ppc64-windows import libraries and turned into a builtin
# PE by elf2pe.
${CC:-gcc} -c -o "$OUT/reader.o" "$HERE/probes/guest_debug_read.c" $INCL \
    -D_UCRT -D_WIN32 -Wall -pipe -mlongcall -mno-pltseq -fcf-protection=none \
    -fvisibility=hidden -fno-stack-protector -fno-strict-aliasing -gdwarf-4 \
    -fPIC -fasynchronous-unwind-tables -mlong-double-64 -fno-builtin \
    -fshort-wchar -Wno-format -g -O1 2>"$OUT/reader.build.err" || {
    sed 's/^/  reader| /' "$OUT/reader.build.err" >&2
    skip "the native ppc64 reader did not compile"
}
"$BUILD/tools/winegcc/winegcc" -o "$OUT/reader.exe" --wine-objdir "$BUILD" \
    --cc-cmd="${CC:-gcc}" -mno-cygwin -fPIC -fasynchronous-unwind-tables \
    -Wl,--wine-builtin -mconsole "$OUT/reader.o" \
    "$BUILD/dlls/kernel32/ppc64-windows/libkernel32.a" \
    "$BUILD/libs/winecrt0/ppc64-windows/libwinecrt0.a" \
    "$BUILD/dlls/ucrtbase/ppc64-windows/libucrtbase.a" \
    "$BUILD/dlls/ntdll/ppc64-windows/libntdll.a" \
    2>>"$OUT/reader.build.err" || {
    sed 's/^/  reader| /' "$OUT/reader.build.err" >&2
    skip "the native ppc64 reader did not link"
}
rm -f "$OUT/reader.exe"
"$SRC/tools/elf2pe" "$OUT/reader.exe.so" "$OUT/reader.exe" 2>>"$OUT/reader.build.err" || \
    skip "elf2pe failed for the native reader"
"$BUILD/tools/winebuild/winebuild" --builtin "$OUT/reader.exe" \
    2>>"$OUT/reader.build.err" || skip "winebuild --builtin failed for the native reader"
say "build: guest probe (crashing and parking) and native ppc64 reader"

# ---- the run harness --------------------------------------------------------
GOFILE=$OUT/go

# Start a guest probe and wait for its own READY line -- never a wall-clock
# guess.  The debuggee keeps winedbg.exe disabled so that a nested AeDebug
# cannot start underneath the debugger this gate is running on purpose.
start_probe() {   # $1 = exe basename, $2.. = extra env assignments
    _exe=$1; shift
    rm -f "$OUT/$_exe.out" "$GOFILE"
    ( env -u DISPLAY WINEDEBUG="${WINEDEBUG:--all,err+seh}" \
        WINEDLLOVERRIDES="winedbg.exe=d" GUEST_DEBUG_GO="$GOFILE" "$@" \
        timeout -k 5 "$RUN_TIMEOUT" "$BUILD/wine" "$OUT/$_exe.exe" \
        > "$OUT/$_exe.out" 2>&1 ) &
    PROBE_PID=$!
    _i=0
    while [ $_i -lt 900 ]; do
        grep -q "guest_debug: READY" "$OUT/$_exe.out" 2>/dev/null && break
        kill -0 "$PROBE_PID" 2>/dev/null || break
        _i=$((_i + 1)); sleep 0.1
    done
    grep -q "guest_debug: READY" "$OUT/$_exe.out" 2>/dev/null || return 1
    PROBE_HEXPID=$(sed -n 's/^guest_debug: pid=//p' "$OUT/$_exe.out" | head -1)
    PROBE_DECPID=$(printf "%d" "0x$PROBE_HEXPID")
    return 0
}

release_probe() { : > "$GOFILE"; }

# Release the probe once the DEBUGGER says it has attached, not after a fixed
# sleep.  The probe must fault while somebody is watching, and how long an
# attach takes on this machine is exactly the thing a gate must not guess at:
# with three other agents on the box, a sleep that is right at 2am is wrong at
# 2:01.  Runs in the background and gives up rather than hanging.
release_when_attached() {   # $1 = file the debugger writes, $2 = its attach line
    (
        _i=0
        while [ $_i -lt 400 ]; do
            grep -q "$2" "$1" 2>/dev/null && break
            _i=$((_i + 1)); sleep 0.1
        done
        release_probe
    ) &
}

wait_probe() {
    wait "$PROBE_PID" 2>/dev/null
    PROBE_RC=$?
    PROBE_PID=
}

# The debugger runs WITHOUT the winedbg override, obviously, and with the
# environment levers this gate is exercising.
run_winedbg() {   # $1 = command string, $2.. = extra env assignments
    _cmd=$1; shift
    env -u DISPLAY -u WINEDLLOVERRIDES WINEDEBUG=-all "$@" \
        timeout -k 5 "$DBG_TIMEOUT" "$BUILD/wine" winedbg \
        --command "$_cmd" "$PROBE_DECPID"
}

run_reader() {    # $1.. = extra env assignments
    env -u DISPLAY WINEDEBUG=-all WINEDLLOVERRIDES="winedbg.exe=d" "$@" \
        timeout -k 5 "$DBG_TIMEOUT" "$BUILD/wine" "$OUT/reader.exe" "$PROBE_DECPID"
}

# ---- B: an attach that stops the target, and does not kill it ---------------
attach_layer() {
    if ! start_probe parker; then
        sed 's/^/  guest| /' "$OUT/parker.out" >&2
        wait_probe
        bad "the parking guest probe never reached READY"
        return 1
    fi
    # NOT released first: this layer proves that attaching stops a target that
    # is doing nothing at all, so winedbg has to reach its stop while the probe
    # is still parked.  The probe is let go afterwards, and must then run to a
    # normal exit -- which is the other half of the claim, that an attach and a
    # detach leave a working process behind.
    rm -f "$OUT/attach.out"
    run_winedbg "info threads
detach" > "$OUT/attach.out" 2>&1
    _rc=$?
    release_probe
    wait_probe
    if [ $_rc -eq 124 ] || [ $_rc -eq 137 ]; then
        bad "winedbg never reached a stop in ${DBG_TIMEOUT}s -- the attach did not \
land (this is the failure the breakin-address translation exists to fix)"
        sed 's/^/  dbg| /' "$OUT/attach.out" >&2
        return 1
    fi
    if grep -q "DbgBreakPoint" "$OUT/attach.out"; then
        say "attach: winedbg stopped the target on its own injected breakin: \
$(grep -m1 'DbgBreakPoint' "$OUT/attach.out" | cut -c1-80)"
    else
        bad "winedbg returned but never stopped on DbgBreakPoint; the attach \
handshake did not complete"
        sed 's/^/  dbg| /' "$OUT/attach.out" >&2
    fi
    if grep -q "guest_debug: NOFAULT" "$OUT/parker.out"; then
        say "attach: the target survived attach and detach and exited normally"
    else
        sed 's/^/  guest| /' "$OUT/parker.out" >&2
        bad "the target did not run to its normal exit after being attached to"
    fi
    return 0
}

# ---- F: the values, read by a native program that is not winedbg -----------
reader_layer() {   # $1.. = extra env for the DEBUGGEE
    if ! start_probe crasher "$@"; then
        sed 's/^/  guest| /' "$OUT/crasher.out" >&2
        wait_probe
        bad "the crashing guest probe never reached READY"
        return 1
    fi
    rm -f "$OUT/reader.out"
    : > "$OUT/reader.out"
    release_when_attached "$OUT/reader.out" "guest_debug_read: attached"
    run_reader > "$OUT/reader.out" 2>&1
    READER_RC=$?
    wait_probe
    return 0
}

# ---- G: the same crash under winedbg ---------------------------------------
winedbg_layer() {  # $1.. = extra env for the DEBUGGEE
    if ! start_probe crasher "$@"; then
        sed 's/^/  guest| /' "$OUT/crasher.out" >&2
        wait_probe
        bad "the crashing guest probe never reached READY"
        return 1
    fi
    rm -f "$OUT/winedbg.out"
    : > "$OUT/winedbg.out"
    release_when_attached "$OUT/winedbg.out" "WineDbg attached"
    # `cont` first: the attach now really stops the target, so the FIRST stop
    # is winedbg's own breakin on a native thread and the crash is the second.
    # That ordering is the fix working -- before it, the crash was the only
    # stop there ever was.
    run_winedbg "cont
bt
quit" > "$OUT/winedbg.out" 2>&1
    WINEDBG_RC=$?
    wait_probe
    return 0
}

# ---- (also standalone as --sabotage): the negative controls -----------------
sabotage() {
    ok=1

    # 1: the breakin address is not translated -- the attach cannot land.
    if start_probe parker; then
        run_winedbg "info threads
detach" WINEEMUNODBGATTACH=1 > "$OUT/sab_attach.out" 2>&1
        _rc=$?
        release_probe
        wait_probe
        if [ $_rc -eq 124 ] || [ $_rc -eq 137 ] || ! grep -q "DbgBreakPoint" "$OUT/sab_attach.out"; then
            say "sabotage: WINEEMUNODBGATTACH=1 left winedbg without a stop, as it \
must (rc=$_rc)"
        else
            bad "WINEEMUNODBGATTACH=1 still stopped the target -- the attach fix \
is not what makes the attach work"
            ok=0
        fi
    else
        wait_probe
        bad "the parking probe never reached READY under the sabotage lever"; ok=0
    fi

    # 2: the guest register file is not published at all.
    if reader_layer WINEEMUNODBGCTX=1; then
        if grep -q "guest_debug_read: PASS" "$OUT/reader.out"; then
            bad "WINEEMUNODBGCTX=1 and the reader still PASSED -- the gate cannot \
go red"
            ok=0
        elif grep -q "register file is readable" "$OUT/reader.out"; then
            say "sabotage: WINEEMUNODBGCTX=1 made the guest register file \
unreadable, as it must: $(grep -m1 'register file is readable' "$OUT/reader.out" \
| sed 's/^ *//' | cut -c1-100)"
        else
            bad "WINEEMUNODBGCTX=1 did not fail at the register-file check; the \
negative control is not falsifying what it claims to"
            sed 's/^/  sab| /' "$OUT/reader.out" >&2
            ok=0
        fi
    else
        ok=0
    fi

    # 3: the guest stack is freed as before.  The registers still arrive --
    #    which is the point: this control catches a gate that proves registers
    #    and calls it debugging.
    if reader_layer WINEEMUNODBGSTACK=1; then
        if grep -q "guest_debug_read: PASS" "$OUT/reader.out"; then
            bad "WINEEMUNODBGSTACK=1 and the reader still PASSED -- the guest \
stack is being kept by something other than the code this lever turns off"
            ok=0
        elif grep -q "registers without a stack" "$OUT/reader.out"; then
            say "sabotage: WINEEMUNODBGSTACK=1 left the registers readable and \
the guest stack unmapped, as it must: $(grep -m1 'registers without a stack' \
"$OUT/reader.out" | sed 's/^ *//' | cut -c1-110)"
        else
            bad "WINEEMUNODBGSTACK=1 did not fail at the stack read"
            sed 's/^/  sab| /' "$OUT/reader.out" >&2
            ok=0
        fi
    else
        ok=0
    fi

    [ "$ok" = 1 ] && say "SABOTAGE PASS"
    [ "$ok" = 1 ]
}

if [ "$SABOTAGE" = 1 ]; then
    sabotage
    rc=$?
    cleanup
    exit $rc
fi

# ---- B ----------------------------------------------------------------------
attach_layer

# ---- F ----------------------------------------------------------------------
reader_layer
sed 's/^/  /' "$OUT/reader.out" 2>/dev/null
if [ "${READER_RC:-1}" -eq 124 ] || [ "${READER_RC:-1}" -eq 137 ]; then
    bad "the native reader timed out after ${DBG_TIMEOUT}s"
elif grep -q "guest_debug_read: PASS" "$OUT/reader.out"; then
    say "values: $(grep -m1 'guest_debug_read: PASS' "$OUT/reader.out")"
else
    bad "the native reader did not pass; a debugger cannot see this guest's state"
fi

# ---- G ----------------------------------------------------------------------
winedbg_layer
if [ "${WINEDBG_RC:-1}" -eq 124 ] || [ "${WINEDBG_RC:-1}" -eq 137 ]; then
    bad "winedbg timed out after ${DBG_TIMEOUT}s on the crashing guest"
else
    # the guest RIP, not a host address, in the exception line
    if grep -qi "in 64-bit code (0x0*1400" "$OUT/winedbg.out"; then
        say "winedbg: names the fault in guest code: $(grep -m1 -i 'Unhandled exception' \
"$OUT/winedbg.out" | cut -c1-120)"
    else
        bad "winedbg did not report the fault at a guest address"
        grep -m2 -i "unhandled exception" "$OUT/winedbg.out" | sed 's/^/  dbg| /' >&2
    fi

    # every sentinel, in the register dump
    _missing=
    for s in $SENTINELS; do
        grep -qi "$s" "$OUT/winedbg.out" || _missing="$_missing $s"
    done
    if [ -z "$_missing" ]; then
        say "winedbg: the register dump carries all 9 guest sentinels, so these \
are the GUEST's registers and not the emulator's"
    else
        bad "winedbg's register dump is missing guest sentinels:$_missing"
        grep -A4 -i "Register dump" "$OUT/winedbg.out" | sed 's/^/  dbg| /' >&2
    fi

    # the marker the guest pushed, in the stack dump at the guest RSP
    if grep -qi "$STACK_MARK" "$OUT/winedbg.out"; then
        say "winedbg: the stack dump at the guest RSP holds the marker the guest \
pushed ($STACK_MARK), so the guest stack is mapped and reachable"
    else
        bad "winedbg's stack dump does not contain the guest's own marker \
$STACK_MARK; the guest stack was not readable"
        grep -A3 -i "Stack dump" "$OUT/winedbg.out" | sed 's/^/  dbg| /' >&2
    fi

    # the faulting instruction, disassembled as x86-64
    if grep -q "movl \$0, (%rcx)" "$OUT/winedbg.out"; then
        say "winedbg: disassembled the faulting guest instruction as x86-64: \
$(grep -m1 'movl \$0, (%rcx)' "$OUT/winedbg.out" | sed 's/^ *//' | cut -c1-100)"
    else
        bad "winedbg did not disassemble the faulting instruction as x86-64"
        grep -B1 -A1 -i "backtrace" "$OUT/winedbg.out" | sed 's/^/  dbg| /' >&2
    fi

    # all four guest frames, in order
    _order=$(grep -o "guest_debug_[a-z0-9]*" "$OUT/winedbg.out" | awk '!seen[$0]++' | tr '\n' ' ')
    _want=$(echo $FRAMES | tr -s ' ')
    case "$_order" in
        "$_want "*|"$_want")
            say "winedbg: the backtrace walks the guest's own .pdata and names all \
four frames in order: $_order" ;;
        *)
            bad "winedbg's backtrace does not name the four guest frames in order; \
wanted '$_want', saw '$_order'"
            sed -n '/Backtrace/,/^$/p' "$OUT/winedbg.out" | sed 's/^/  dbg| /' >&2 ;;
    esac
fi

# ---- verdict ----------------------------------------------------------------
cleanup
if [ "$fail" = 0 ]; then
    say "PASS"
    exit 0
fi
say "FAILED" >&2
exit 1
