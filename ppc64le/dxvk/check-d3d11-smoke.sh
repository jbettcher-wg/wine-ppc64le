#!/bin/sh
#
# check-d3d11-smoke.sh -- the native-vs-guest D3D11 RUNTIME gate.
#
# ppc64le/dxvk/build-for-wine.sh proves DXVK builds clean for this machine.
# ppc64le/dxvk/scan-isa.sh proves what it builds stays on the -mcpu=power8
# floor.  Neither says the result actually WORKS -- that a guest D3D11
# call reaches the same DXVK, through whatever plumbing carries it there,
# and gets back the same answer a process with no guest in the loop at all
# would get.  That is what this file proves, the same way
# check-com-smoke.sh proved it for the COM boundary: build probes/
# d3d11_smoke.c TWICE from the one source -- once as a native ppc64le ELF
# binary that dlopens DXVK directly (no Wine, no emulation anywhere), once
# as an x86-64 guest PE that imports d3d11.dll and runs under this port's
# wine -- and require the two runs to print BYTE-IDENTICAL stdout.  Every
# line either run prints is a value the real GPU pipeline computed (a
# feature level, an FNV-1a checksum, a per-texel mismatch count), so
# identical output means the guest reached the same implementation with
# nothing lost, swapped, or silently defaulted on the way.  It is the same
# texel-exact standard dlls/d3d12/main.c's guest surface is held to
# downstream of this file, applied one layer closer to the metal.
#
# Seven legs:
#
#   A  BUILD: the three DXVK libraries (dxgi, d3d11, d3d10core) exist under
#      the meson build tree.  Not built here -- see build-for-wine.sh.
#   B  ISA FLOOR: scan-isa.sh reports the build tree CLEAN against the
#      -mcpu=power8 floor.
#   C  NATIVE: the probe, built for ppc64le and run headless with no Wine
#      and no guest in the process, reports PASS.  Establishes the expected
#      bytes with the boundary out of the picture entirely.
#   D  GUEST: the same probe, built as an x86-64 PE, runs under the
#      emulator and reports PASS.
#   E  IDENTITY: cmp(native stdout, guest stdout) is empty.
#   F  REFUSAL: a SEPARATE guest build (D3D11_SMOKE_REFUSAL -- see the
#      probe's header) drives ID3D11Device::OpenSharedResource with a
#      fabricated HANDLE, and the port's own +winecom trace must name both
#      the method and the word "refusing" -- and the process must still
#      reach its own next line rather than crash.  A defect refused loudly
#      is not this gate's problem; a defect served silently would be.
#   H  FENCE FLAGS: the D3D11_FENCE_FLAG enumerators this tree's IDL declares
#      and the ones DXVK's own vendored header declares are the same numbers.
#      CreateFence's flags cross BY VALUE, so they have to be.  Source-level
#      and cheap, and run before leg G because leg G is the leg that creates a
#      fence.
#   G  EVENTS: the fence-event relay, the windowless composition swapchain and
#      the canary pins -- guest-only, and the ONE leg that runs inside a
#      compositor of this gate's own (check-present-smoke.sh's weston pattern;
#      the weston_up banner says what that does and does not buy, including the
#      premise it corrects).  Every other leg stays headless with no session at
#      all, and leg G falls back to ambient where there is no weston.
#
# --sabotage runs negative controls instead, and requires EVERY one to go red.
# The first two are the gate's original pair; parts 3 to 5 arrived with the
# legs they falsify, each naming the lever it pulls:
#
#   1  WINEEMUNOCOMWRAP=1 hands the guest raw host pointers -- the exact
#      defect this port's proxy runtime exists to fix -- and the guest run
#      MUST NOT print "d3d11_smoke: PASS".
#   2  The native probe rebuilt with -DSMOKE_BREAK=1, =2, and =3 (see the
#      probe's header for what each one falsifies) MUST each FAIL.
#
# A gate that cannot go red proves nothing; this is how it proves it can.
#
# Exit 0 = pass, 1 = a check failed, 2 = could not run at all (a skip is
# NOT a pass).
#
#
# WHY EVERY WINE RUN DISABLES winedbg, verbatim from check-seh-smoke.sh and
# check-seh-handlers.sh because the hazard is identical here: the bringup
# prefix has AeDebug configured with "winedbg --auto", so any run that ends
# in an unhandled fault -- which is exactly what a defect in this boundary
# looks like from the outside -- starts the debugger, which attaches, loads
# its GUI stack and never lets go.  That turns every red state of this gate
# into a hang, which is the one thing a gate must never be.
# WINEDLLOVERRIDES=winedbg.exe=d makes start_debugger's CreateProcess fail,
# so UnhandledExceptionFilter falls straight through to terminating the
# process instead.  This is an environment override for the duration of one
# run and touches nothing in the prefix.
set -u

HERE=$(cd "$(dirname "$0")" && pwd)
SRC=$(cd "$HERE/../.." && pwd)
BUILD=${BUILD:-$SRC}                    # in-tree build by default
OUT=${OUT:-/tmp/d3d11-smoke}
SABOTAGE=0
[ "${1:-}" = "--sabotage" ] && SABOTAGE=1

say()  { echo "check-d3d11-smoke: $*"; }
bad()  { echo "check-d3d11-smoke: FAIL $*" >&2; fail=1; }
note() { echo "check-d3d11-smoke: note $*"; }
skip() { echo "check-d3d11-smoke: $*" >&2; exit 2; }

[ -x "$BUILD/wine" ] || skip "no wine loader at $BUILD/wine"
[ -n "${WINEPREFIX:-}" ] || skip "set WINEPREFIX to a prefix wineboot has run in"
[ -n "${WINEFEXBRIDGE:-}" ] || skip "set WINEFEXBRIDGE to the emulator bridge"
[ -f "$BUILD/dlls/d3d11/x86_64-windows/d3d11.dll" ] || \
    skip "no guest d3d11 thunk; build it first"
command -v clang >/dev/null || skip "need clang for the guest build"
command -v llvm-dlltool >/dev/null || skip "need llvm-dlltool for the guest build"
command -v "${CC:-gcc}" >/dev/null || skip "need ${CC:-gcc} for the native ppc64le build"

mkdir -p "$OUT" || skip "cannot create $OUT"

# DXVK writes <appname>_d3d11.log next to the CURRENT DIRECTORY by default, and
# a gate is normally run from the top of the source tree -- so a plain run left
# native_d3d11.log, native_dxgi.log and a wine-preloader_* pair lying in the
# checkout.  Point them at this gate's own work directory instead; DXVK_LOG_PATH
# takes a directory, and "none" would suppress the logs entirely, which is worse
# when a leg fails and the log is the evidence.
DXVK_LOG_PATH=$OUT
export DXVK_LOG_PATH
fail=0

INCL="-I$BUILD/include -I$SRC/include -I$SRC/include/msvcrt"
TIMEOUT=${TIMEOUT:-120}

# ---- leg G's presentation session -------------------------------------------
#
# LEG G ALONE runs inside a compositor of this gate's own, and every other leg
# keeps the bare headless environment it has always had.
#
# WHY, AND WHAT THE PREMISE TURNED OUT TO BE.  Leg G was filed as failing on a
# bare Xvfb -- an X server with no compositor and, on this lane, no winewayland
# session behind it -- on the reading that DXVK's fence-event path is armed by
# the presentation loop.  [MEASURED 2026-09-01, and it is a correction] it is
# NOT: with DXVK's foreign WSI in headless mode the fence signals on a bare
# Xvfb too, and the probe reports signaled=yes either way.  So the session
# below is not what makes leg G pass today.
#
# It stays, and stays only around leg G, for the reason that survives the
# correction: leg G is the one leg that touches presentation at all (a
# composition swapchain, a fence armed against a device that has one), and a
# leg that presents should be measured in the environment a title presents in
# rather than in whatever the caller's terminal happened to inherit.  Running
# it under a compositor of this gate's own is also what makes it SAFE to run
# beside a machine somebody is using, which the ambient path never was.  When
# no compositor is available the leg still runs, ambiently, and says so -- the
# other seven legs are worth keeping on a machine with no weston.
#
# This is check-present-smoke.sh's pattern, borrowed whole, including the two
# things that make it safe:
#
#   * ITS OWN SOCKET IN ITS OWN XDG_RUNTIME_DIR, and $DISPLAY /
#     $WAYLAND_DISPLAY unset for every process started inside it.  Unsetting
#     the display variables is the load-bearing part: with XDG_RUNTIME_DIR
#     left alone a Wayland client finds the caller's compositor through the
#     DEFAULT socket name even with WAYLAND_DISPLAY unset, which is how a
#     probe ends up drawing on somebody's desktop.
#   * KILLED BY PID, never by pattern.  `env` execs its argument, so the pid
#     recorded below really is weston's; a shell function backgrounded with &
#     would have left $! naming a subshell, which is how two compositors were
#     once left running on this machine, one per red run.
G_RUNDIR=$OUT/runtime
G_SOCKET=wine-d3d11-events
WESTON_PID=

g_iso() { env -u DISPLAY -u WAYLAND_DISPLAY XDG_RUNTIME_DIR="$G_RUNDIR" "$@"; }

weston_down() {
    [ -n "${WESTON_PID:-}" ] || return 0
    kill "$WESTON_PID" 2>/dev/null
    wait "$WESTON_PID" 2>/dev/null
    WESTON_PID=
    _n=0
    while [ $_n -lt 50 ] && [ -S "$G_RUNDIR/$G_SOCKET" ]; do
        _n=$((_n + 1)); sleep 0.1
    done
}
trap weston_down EXIT INT TERM

# Three answers, never a skip of the whole gate: legs A-F have already run by
# the time this is called and their verdicts are worth keeping.
#   0  a session is up; run leg G inside it
#   1  a session was WANTED and could not be built -- the gate is already red
#   2  this machine has no compositor to build one from; run leg G ambiently
weston_up() {
    [ -n "${WESTON_PID:-}" ] && return 0
    if ! command -v weston >/dev/null || \
       [ ! -f "$BUILD/dlls/winewayland.drv/winewayland.so" ]; then
        note "leg G: no weston or no winewayland.drv, so this leg runs in the \
ambient environment instead of a session of its own; it still measures the \
relay, it just shares whatever display the caller has"
        return 2
    fi
    rm -rf "$G_RUNDIR" && mkdir -p "$G_RUNDIR" && chmod 700 "$G_RUNDIR" || {
        bad "leg G: cannot create a private runtime directory at $G_RUNDIR"
        return 1
    }
    env -u DISPLAY -u WAYLAND_DISPLAY XDG_RUNTIME_DIR="$G_RUNDIR" \
        weston --backend=headless --renderer=gl \
        --width=640 --height=480 --socket="$G_SOCKET" \
        >"$OUT/weston.log" 2>&1 &
    WESTON_PID=$!
    _i=0
    while [ $_i -lt 200 ]; do
        [ -S "$G_RUNDIR/$G_SOCKET" ] && break
        kill -0 "$WESTON_PID" 2>/dev/null || break
        _i=$((_i + 1)); sleep 0.1
    done
    if [ ! -S "$G_RUNDIR/$G_SOCKET" ]; then
        sed 's/^/  weston| /' "$OUT/weston.log" >&2
        WESTON_PID=
        bad "leg G: weston did not come up on a private socket"
        return 1
    fi
    say "leg G: headless weston 640x480 on $G_RUNDIR/$G_SOCKET"
    return 0
}

# One guest run inside that session.  Everything else about it -- the
# winedbg override, the timeout -- is what every other run in this file uses.
run_wine_session() {   # <exe> <extra env assignment>... ; WINEDEBUG from $WDBG
    _exe=$1; shift
    g_iso timeout -k 5 "$TIMEOUT" env WAYLAND_DISPLAY="$G_SOCKET" \
        WINEDEBUG="$WDBG" WINEDLLOVERRIDES="winedbg.exe=d" "$@" \
        "$BUILD/wine" "$_exe"
}

# ---- leg H's two authorities ------------------------------------------------
# The value of a D3D11_FENCE_FLAG is agreed by two files that were never
# checked against each other: this tree's IDL, from which widl generates the
# d3d11_3.h a guest compiles against, and the MinGW-w64-derived header DXVK
# vendors and its own fence code reads.  Named here rather than inline so the
# sabotage control can doctor a copy of one of them.
WINE_FENCE_IDL="$SRC/include/d3d11_3.idl"
DXVK_FENCE_H="$HERE/src/include/native/directx/d3d11_3.h"

# Both spellings normalised to decimal, so "0x2" and "2" cannot read as a
# difference and a difference that IS one cannot hide in a base.
fence_flags() {   # <file> -> "NONE=1 SHARED=2 ..."
    sed -n 's/.*D3D11_FENCE_FLAG_\([A-Z_][A-Z_]*\)[ 	]*=[ 	]*\([0-9xXa-fA-F]*\).*/\1 \2/p' \
        "$1" | while read -r _n _v; do printf '%s=%d ' "$_n" "$((_v))"; done
}

# ---- A: build check --------------------------------------------------------
# meson bakes DT_RUNPATH $ORIGIN/../dxgi (etc.) into these exact locations
# in the BUILD tree -- see build-for-wine.sh's own note on why the libraries
# are consumed from here rather than from an install prefix.  The native
# leg below dlopens libdxvk_d3d11.so from this literal path for that reason:
# $ORIGIN resolution depends on it.
DXVKBUILD="$BUILD/ppc64le/dxvk-build"
D3D11_SO="$DXVKBUILD/src/d3d11/libdxvk_d3d11.so"
DXGI_SO="$DXVKBUILD/src/dxgi/libdxvk_dxgi.so"
D3D10_SO="$DXVKBUILD/src/d3d10/libdxvk_d3d10core.so"
for f in "$D3D11_SO" "$DXGI_SO" "$D3D10_SO"; do
    [ -e "$f" ] || \
        skip "missing $f -- run ppc64le/dxvk/build-for-wine.sh $DXVKBUILD first"
done
say "build: all three DXVK libraries are present under $DXVKBUILD/src"

# ---- B: ISA floor -----------------------------------------------------------
[ -x "$HERE/scan-isa.sh" ] || skip "no $HERE/scan-isa.sh"
SCANOUT="$OUT/scan-isa.out"
"$HERE/scan-isa.sh" "$DXVKBUILD" >"$SCANOUT" 2>&1
scan_rc=$?
case $scan_rc in
    0) say "isa: $(grep -m1 'CLEAN' "$SCANOUT")" ;;
    1) sed 's/^/  scan-isa| /' "$SCANOUT" >&2
       bad "the dxvk build tree contains instructions above the -mcpu=power8 floor" ;;
    *) sed 's/^/  scan-isa| /' "$SCANOUT" >&2
       skip "scan-isa.sh could not run (exit $scan_rc); the ISA floor cannot be asserted" ;;
esac

# ---- build: the native ppc64le ELF leg --------------------------------------
# An ordinary Linux program, not a PE and not run under wine: DXVK's own
# headers, dlopen, no emulation anywhere in the process.  Same include
# layout dxvk-ppc64le/probes/native_d3d11_smoke.cpp documents building
# against (N=src/include/native, plus its windows/ and directx/
# subdirectories), because that vendored MinGW-w64 header set is what makes
# the SAME probe source's COBJMACROS calls compile on both legs.
NATIVE_INC_BASE="$HERE/src/include/native"
[ -d "$NATIVE_INC_BASE" ] || \
    skip "no DXVK native headers at $NATIVE_INC_BASE -- run \
ppc64le/dxvk/build-for-wine.sh (which runs bootstrap.sh if needed) first"
NATIVE_INC="-I$NATIVE_INC_BASE -I$NATIVE_INC_BASE/windows -I$NATIVE_INC_BASE/directx"
NATIVECC="${CC:-gcc} -std=c11 -O2 -mcpu=power8 $NATIVE_INC -Wall -fno-builtin"

$NATIVECC -DD3D11_SMOKE_NATIVE -c -o "$OUT/native.o" "$HERE/probes/d3d11_smoke.c" \
    || skip "native compile failed"
${CC:-gcc} -o "$OUT/native" "$OUT/native.o" -ldl || skip "native link failed"

# ---- build: the x86-64 guest PE leg -----------------------------------------
# Same clang x86_64-windows-gnu machinery check-com-smoke.sh drives its
# guest build with, and the same Wine headers, so any disagreement between
# the native and guest legs is the boundary, not the declarations.  The
# import is described by hand, naming only what the probe actually calls
# (D3D11CreateDevice; CreateDXGIFactory1 only in the separate REFUSAL
# build below), for the same reason check-com-smoke.sh's ole32.def does:
# the guest binds to the same builtins a real guest application would, and
# nothing else is linked in at all -- there is no CRT here, see the
# probe's header.
cat > "$OUT/kernel32.def" <<'EOF'
LIBRARY kernel32.dll
EXPORTS
GetStdHandle
WriteFile
ExitProcess
CreateEventW
WaitForSingleObject
CloseHandle
EOF
cat > "$OUT/d3d11.def" <<'EOF'
LIBRARY d3d11.dll
EXPORTS
D3D11CreateDevice
EOF
for m in kernel32 d3d11; do
    llvm-dlltool -m i386:x86-64 -d "$OUT/$m.def" -l "$OUT/lib$m.a" \
        || skip "llvm-dlltool failed for $m"
done

GUESTCC="clang -target x86_64-windows-gnu -nostdlibinc $INCL -Wall -O1 -fno-builtin -g"
GUESTLD="clang -target x86_64-windows-gnu -fuse-ld=lld -nostdlib \
-Wl,--entry=d3d11_smoke_entry -Wl,--subsystem,console"

$GUESTCC -c -o "$OUT/guest.o" "$HERE/probes/d3d11_smoke.c" || skip "guest compile failed"
$GUESTLD -o "$OUT/guest.exe" "$OUT/guest.o" "$OUT/libd3d11.a" "$OUT/libkernel32.a" \
    || skip "guest link failed"

WDBG=${WINEDEBUG:--all}
run_wine() { timeout -k 5 "$2" \
                 env WINEDEBUG="$WDBG" WINEDLLOVERRIDES="winedbg.exe=d" \
                 "$BUILD/wine" "$1"; }

# ---- (also available standalone as --sabotage): the negative controls -----
sabotage() {
    ok=1

    # part 1: WINEEMUNOCOMWRAP=1 hands the guest raw host pointers.
    timeout -k 5 "$TIMEOUT" \
        env WINEDEBUG="$WDBG" WINEDLLOVERRIDES="winedbg.exe=d" WINEEMUNOCOMWRAP=1 \
        "$BUILD/wine" "$OUT/guest.exe" \
        > "$OUT/sabotage_wrap.out" 2>"$OUT/sabotage_wrap.err"
    if grep -q "d3d11_smoke: PASS" "$OUT/sabotage_wrap.out"; then
        bad "WINEEMUNOCOMWRAP=1 still PASSED -- the gate cannot go red"; ok=0
    else
        say "sabotage: WINEEMUNOCOMWRAP=1 failed the guest run at '$(tail -1 \
            "$OUT/sabotage_wrap.out" | cut -c1-60)', as it must"
    fi

    # part 2: each SMOKE_BREAK variant, native, must FAIL.
    for n in 1 2 3; do
        if $NATIVECC -DD3D11_SMOKE_NATIVE -DSMOKE_BREAK=$n \
                -c -o "$OUT/native_break$n.o" "$HERE/probes/d3d11_smoke.c" \
                2>"$OUT/native_break$n.build.err" \
           && ${CC:-gcc} -o "$OUT/native_break$n" "$OUT/native_break$n.o" -ldl \
                2>>"$OUT/native_break$n.build.err"; then
            timeout -k 5 "$TIMEOUT" \
                env -u DISPLAY -u WAYLAND_DISPLAY -u XDG_RUNTIME_DIR \
                DXVK_WSI_DRIVER=Headless \
                "$OUT/native_break$n" "$D3D11_SO" \
                > "$OUT/native_break$n.out" 2>"$OUT/native_break$n.err"
            if grep -q "d3d11_smoke: PASS" "$OUT/native_break$n.out"; then
                bad "SMOKE_BREAK=$n PASSED; the falsification build must FAIL"; ok=0
            else
                say "sabotage: SMOKE_BREAK=$n failed as it must: $(tail -1 \
                    "$OUT/native_break$n.out" | cut -c1-100)"
            fi
        else
            bad "SMOKE_BREAK=$n native build failed; cannot prove this check can fail"
            tail -5 "$OUT/native_break$n.build.err" | sed 's/^/  break'"$n"'| /' >&2
            ok=0
        fi
    done

    # part 3: refusal hygiene's own negative control.  The refusal probe's
    # OpenSharedResource out-param is seeded with a sentinel; the normal leg F
    # requires osr_scrubbed=yes (a refused slot WRITES NULL before answering
    # -- the Witcher 3 GetShader lesson).  Here the scrub is disabled by its
    # lever, and the sentinel MUST survive: if osr_scrubbed stays "yes" with
    # the lever armed, the scrub the positive leg observed was not the thing
    # this lever controls, and neither leg proves anything.
    if [ -x "$OUT/refusal.exe" ] || [ -f "$OUT/refusal.exe" ]; then
        timeout -k 5 "$TIMEOUT" \
            env WINEDEBUG="$WDBG" WINEDLLOVERRIDES="winedbg.exe=d" \
            WINEEMUNOREFUSESCRUB=1 \
            "$BUILD/wine" "$OUT/refusal.exe" \
            > "$OUT/sabotage_scrub.out" 2>"$OUT/sabotage_scrub.err"
        if grep -q "osr_scrubbed=no" "$OUT/sabotage_scrub.out"; then
            say "sabotage: WINEEMUNOREFUSESCRUB=1 left the sentinel in the \
refused out-param, as it must (the scrub is load-bearing)"
        else
            bad "WINEEMUNOREFUSESCRUB=1 did not stop the refusal scrub \
(wanted osr_scrubbed=no): $(grep -o 'osr_scrubbed=[a-zA-Z]*' \
"$OUT/sabotage_scrub.out" | head -1)"; ok=0
        fi
    else
        note "sabotage: no refusal.exe from a prior full run; skipping the \
refusal-hygiene control (run the gate without --sabotage first)"
    fi

    # part 4: the event relay's negative control.  WINEEMUNOCOMEVENT=1 makes
    # every non-NULL event argument refuse, so the fence-event step's wait
    # must NEVER succeed -- if signaled=yes survives the lever, whatever leg
    # G observed was not the relay.  (The probe itself treats the clean
    # refusal as its own pass -- both worlds are correct worlds; the LEVER
    # is what this control tests.)
    if [ ! -f "$OUT/events.exe" ]; then
        note "sabotage: no events.exe from a prior full run; skipping the \
event-relay control (run the gate without --sabotage first)"
    elif ! { weston_up; [ $? != 1 ]; }; then
        ok=0
    else
        # In leg G's own session, for leg G's own reason: the control has to
        # fail where the positive leg passes, or it is falsifying a different
        # run.  The two assignments after the helper's own override its
        # WINEDEBUG (env takes the last spelling) and arm the lever.
        run_wine_session "$OUT/events.exe" \
            WINEDEBUG=+winecom WINEEMUNOCOMEVENT=1 \
            > "$OUT/sabotage_event.out" 2>"$OUT/sabotage_event.err"
        if grep -q "signaled=yes" "$OUT/sabotage_event.out"; then
            bad "WINEEMUNOCOMEVENT=1 did not stop the event relay \
(signaled=yes survived the lever)"; ok=0
        elif ! grep -qi "refus" "$OUT/sabotage_event.err"; then
            bad "WINEEMUNOCOMEVENT=1 stopped the signal but the +winecom \
trace never named a refusal; the failure is not the lever's"; ok=0
        else
            say "sabotage: WINEEMUNOCOMEVENT=1 refused the event and the \
wait never paid out, as it must"
        fi
    fi
    weston_down

    # part 5: the fence-flag pin's own control.  Leg H compares two files that
    # AGREE; a check that only ever compares equal things has never been shown
    # to notice a difference.  Doctor one enumerator in a COPY of the DXVK
    # header and require the comparison to go red.
    sed 's/D3D11_FENCE_FLAG_SHARED = 0x2/D3D11_FENCE_FLAG_SHARED = 0x20/' \
        "$DXVK_FENCE_H" > "$OUT/fence_doctored.h" 2>/dev/null
    if [ "$(fence_flags "$OUT/fence_doctored.h")" = "$(fence_flags "$WINE_FENCE_IDL")" ]; then
        bad "the fence-flag comparison does not notice a moved value; leg H \
cannot go red"; ok=0
    else
        say "sabotage: a moved D3D11_FENCE_FLAG_SHARED broke the pin, as it \
must ($(fence_flags "$OUT/fence_doctored.h"))"
    fi

    [ "$ok" = 1 ] && say "SABOTAGE PASS"
    [ "$ok" = 1 ]
}

if [ "$SABOTAGE" = 1 ]; then
    sabotage
    exit $?
fi

# ---- C: native ---------------------------------------------------------
# Headless, exactly as dxvk-ppc64le/probes/native_d3d11_smoke.cpp's own run
# recipe requires: no DISPLAY, no WAYLAND_DISPLAY, no XDG_RUNTIME_DIR, and
# DXVK_WSI_DRIVER=Headless so no WSI toolkit is touched at all.  This probe
# never creates a swapchain (see its header), so headless is not a
# workaround here, it is simply correct.
timeout -k 5 "$TIMEOUT" \
    env -u DISPLAY -u WAYLAND_DISPLAY -u XDG_RUNTIME_DIR DXVK_WSI_DRIVER=Headless \
    "$OUT/native" "$D3D11_SO" > "$OUT/native.out" 2>"$OUT/native.err"
nst=$?
if [ $nst -eq 124 ] || [ $nst -eq 137 ]; then
    bad "the native run timed out after ${TIMEOUT}s"
elif grep -q "d3d11_smoke: PASS" "$OUT/native.out"; then
    say "native: $(tail -1 "$OUT/native.out")"
else
    sed 's/^/  native| /' "$OUT/native.out" >&2
    tail -20 "$OUT/native.err" >&2
    bad "the native ppc64le build did not pass"
fi

# ---- D: guest -----------------------------------------------------------
run_wine "$OUT/guest.exe" "$TIMEOUT" > "$OUT/guest.out" 2>"$OUT/guest.err"
gst=$?
if [ $gst -eq 124 ] || [ $gst -eq 137 ]; then
    bad "the guest run timed out after ${TIMEOUT}s"
elif grep -q "d3d11_smoke: PASS" "$OUT/guest.out"; then
    say "guest:  $(tail -1 "$OUT/guest.out")"
else
    sed 's/^/  guest| /' "$OUT/guest.out" >&2
    tail -20 "$OUT/guest.err" >&2
    bad "the x86-64 guest build did not pass"
fi

# ---- E: identity ----------------------------------------------------------
if cmp -s "$OUT/native.out" "$OUT/guest.out"; then
    say "identity: native and guest output is byte-identical ($(wc -l \
        < "$OUT/native.out") lines)"
else
    FIRST=$(diff "$OUT/native.out" "$OUT/guest.out" | head -1)
    bad "native and guest output differ; first difference: $FIRST"
    diff "$OUT/native.out" "$OUT/guest.out" | sed 's/^/  /' | head -20 >&2
fi

# ---- F: refusal negative control -------------------------------------------
# A SEPARATE build (D3D11_SMOKE_REFUSAL) from the one just compared for
# identity above -- see the probe's header for why: this variant's
# transcript is not the one native and guest are diffed against, and must
# never be.  It drives one more call after everything else has already
# passed or failed on its own: a second D3D11CreateDevice, then
# ID3D11Device::OpenSharedResource with a fabricated HANDLE that names no
# shared resource anywhere.
#
# THIS LEG USED TO DRIVE IDXGIFactory::MakeWindowAssociation, on the
# reasoning that an HWND is a value the boundary cannot make sense of.  It
# now can -- this lane presents through win32u's client-surface layer and
# every window-handle slot on the surface marshals -- so that control would
# have quietly started PASSING, which is worse than not having one.  A
# by-value HANDLE never will: it names a Wine object on one side and DXVK's
# tagged-eventfd encoding on the other, two namespaces over one integer, and
# no amount of presentation work changes that.  The control moved to a
# refusal that is structural rather than provisional.
#
# Guarded: if the probe cannot be built, this leg is skipped with a clear
# note rather than failing the whole gate -- legs A-E already established
# the boundary this file exists to test; leg F is one additional negative
# control on top of that, not the load-bearing check.  It needs no import
# beyond d3d11.dll now, because the refused call is a method on a device
# D3D11CreateDevice hands back.
refusal_ok=1
if ! $GUESTCC -DD3D11_SMOKE_REFUSAL -c -o "$OUT/refusal.o" \
        "$HERE/probes/d3d11_smoke.c" 2>"$OUT/refusal.build.err" \
     || ! $GUESTLD -o "$OUT/refusal.exe" "$OUT/refusal.o" \
        "$OUT/libd3d11.a" "$OUT/libkernel32.a" \
        2>>"$OUT/refusal.build.err"; then
    refusal_ok=0
    note "leg F: the D3D11_SMOKE_REFUSAL build failed; skipping the \
refusal negative control (the probe was not built with that call). Last error:"
    tail -5 "$OUT/refusal.build.err" | sed 's/^/  refusal| /'
fi

if [ "$refusal_ok" = 1 ]; then
    timeout -k 5 "$TIMEOUT" \
        env WINEDEBUG=+winecom WINEDLLOVERRIDES="winedbg.exe=d" \
        "$BUILD/wine" "$OUT/refusal.exe" \
        > "$OUT/refusal.out" 2>"$OUT/refusal.err"
    rst=$?
    if [ $rst -eq 124 ] || [ $rst -eq 137 ]; then
        bad "leg F: the refusal probe HUNG (killed after ${TIMEOUT}s) instead \
of exiting cleanly"
        tail -10 "$OUT/refusal.err" | sed 's/^/  refusal| /' >&2
    elif grep -q " reached$" "$OUT/refusal.out"; then
        say "leg F: the refusal probe exited $rst and reached its own next \
line after the refused call"
    else
        sed 's/^/  refusal| /' "$OUT/refusal.out" >&2
        tail -10 "$OUT/refusal.err" | sed 's/^/  refusal| /' >&2
        bad "leg F: the refusal probe never reached its own line after the \
refused call; it crashed instead of being refused cleanly"
    fi
    if grep -qi "refusing" "$OUT/refusal.err" && \
       grep -q "OpenSharedResource" "$OUT/refusal.err"; then
        say "leg F: $(grep -im1 'refusing' "$OUT/refusal.err" | cut -c1-140)"
    else
        sed 's/^/  refusal| /' "$OUT/refusal.err" >&2
        bad "leg F: the port's +winecom trace does not name both 'refusing' \
and 'OpenSharedResource'; the refusal is either missing or unnamed"
    fi
    # refusal hygiene: refused means INERT.  The probe seeds the out-param
    # with a sentinel; the dispatcher must have scrubbed it to NULL before
    # answering E_NOTIMPL, or an unchecked caller reads stack residue -- the
    # Witcher 3 GetShader crash class.  --sabotage part 3 proves the other
    # direction with WINEEMUNOREFUSESCRUB=1.
    if grep -q "osr_scrubbed=yes" "$OUT/refusal.out"; then
        say "leg F: the refused out-param came back scrubbed (NULL)"
    else
        bad "leg F: the refused OpenSharedResource left its out-param \
unscrubbed: $(grep -o 'osr_scrubbed=[a-zA-Z]*' "$OUT/refusal.out" | head -1)"
    fi
fi

# ---- leg H: the D3D11_FENCE_FLAG value pin ---------------------------------
#
# LOOK BEFORE ANY TITLE PASSES SHARED.  ID3D11Device5::CreateFence's flags
# parameter crosses this boundary as a plain by-value integer -- the marshal
# row classes it WINECOM_CA_PASS, with only the 32-bit extension the two ABIs
# disagree about applied -- so the NUMBER the guest wrote is the number DXVK
# reads.  That is correct exactly as long as the two sides spell the enumerator
# with the same number, and nothing had ever checked that they do.  Two files,
# neither of which knows about the other:
#
#   * $SRC/include/d3d11_3.idl, which widl turns into the d3d11_3.h a guest
#     application compiles D3D11_FENCE_FLAG_SHARED from;
#   * DXVK's vendored include/native/directx/d3d11_3.h, which
#     src/d3d11/d3d11_fence.cpp reads when it decides whether to ask Vulkan for
#     an external semaphore.
#
# [AUDITED 2026-09-01] THEY AGREE, value for value: NONE 0x1, SHARED 0x2,
# SHARED_CROSS_ADAPTER 0x4, NON_MONITORED 0x8.  Both descend from the same
# MinGW-w64 lineage, which is why they agree and also why the agreement is
# worth pinning rather than assuming -- it is a shared ancestor, not a shared
# authority, and either can be regenerated from somewhere else.  So there is
# NO translation to do at the boundary and none is added; what is added is the
# assertion that makes drift go red instead of going unnoticed.
#
# ONE THING THE AUDIT FOUND that is worth writing down where the next reader
# will look: this lineage spells NONE as 0x1, not as the 0 an enumerator named
# NONE reads like.  DXVK's fence constructor logs "Fence flags 0x1 not
# supported" for anything outside SHARED -- so a title (or leg G's own probe)
# passing the literal D3D11_FENCE_FLAG_NONE produces that line in every DXVK
# log.  It is noise and not a defect: the fence is still created, and the bit
# is not SHARED, so no external semaphore is asked for.  A future reader
# hunting that log line should stop here rather than at the boundary.
WINE_FF=$(fence_flags "$WINE_FENCE_IDL")
DXVK_FF=$(fence_flags "$DXVK_FENCE_H")
WANT_FF="NONE=1 SHARED=2 SHARED_CROSS_ADAPTER=4 NON_MONITORED=8 "
if [ -z "$WINE_FF" ] || [ -z "$DXVK_FF" ]; then
    bad "leg H: could not read D3D11_FENCE_FLAG out of one of the two \
headers (wine='$WINE_FF' dxvk='$DXVK_FF'); the pin is asserting nothing"
elif [ "$WINE_FF" != "$DXVK_FF" ]; then
    bad "leg H: D3D11_FENCE_FLAG SKEW -- the guest's header says '$WINE_FF' \
and DXVK's says '$DXVK_FF'.  CreateFence's flags cross by value, so a title \
passing SHARED is asking DXVK for a different flag than it wrote"
elif [ "$WINE_FF" != "$WANT_FF" ]; then
    bad "leg H: both sides agree on '$WINE_FF', but that is not the audited \
value set '$WANT_FF' -- they moved together, which is a change to what a \
guest binary compiled against an older header means"
else
    say "leg H: D3D11_FENCE_FLAG agrees on both sides and is unmoved since \
the audit ($WINE_FF)"
fi

# ---- leg G: events, windowless swapchains, and the canaries (guest-only) ---
#
# d3d11_events_smoke.c -- see its header.  GUEST-ONLY by nature: the winecom
# event relay exists only on the guest dispatch path, so there is no native
# twin to byte-compare against; the transcript is asserted against expected
# values instead.  Three families in one leg: the fence event end to end
# (mint -> DXVK's patched SetEvent -> pump -> NtSetEvent, proven by ONE
# WaitForSingleObject succeeding), the dummy composition swapchain, and the
# canary pins (rows served because DXVK provably never reads the hazardous
# parameter -- if either canary line changes, a DXVK update broke the
# citation and the row must be reclassified, which is the deal the
# canary-serve made).
#
# First: the tag-coherence assert the dxvk patch and the vkd3d series both
# promise -- the 'EVFD' constant is respelled per project, so the gate is
# what keeps the spellings honest (both files are IN THIS TREE; no
# bootstrapped checkout needed).
DXVK_TAG=$(grep -o "0x4556464400000000" "$HERE/dxvk-patches/0006-tagged-native-events.patch" | head -1)
VKD3D_TAG=$(grep -o "0x4556464400000000" "$HERE/../vkd3d/vkd3d-patches/0001-tagged-native-event-handles.patch" | head -1)
if [ -n "$DXVK_TAG" ] && [ "$DXVK_TAG" = "$VKD3D_TAG" ]; then
    say "leg G: the native event tag agrees across the dxvk and vkd3d series ($DXVK_TAG)"
else
    bad "leg G: the native event tag DIVERGED between dxvk-patches/0006 and \
vkd3d-patches/0001 -- a handle minted for one library is garbage to the other"
fi

if ! $GUESTCC -c -o "$OUT/events.o" "$HERE/probes/d3d11_events_smoke.c" \
        2>"$OUT/events.build.err" \
     || ! clang -target x86_64-windows-gnu -fuse-ld=lld -nostdlib \
        -Wl,--entry=d3d11_events_entry -Wl,--subsystem,console \
        -o "$OUT/events.exe" "$OUT/events.o" \
        "$OUT/libd3d11.a" "$OUT/libkernel32.a" 2>>"$OUT/events.build.err"; then
    bad "leg G: the events probe failed to build"
    tail -5 "$OUT/events.build.err" | sed 's/^/  events| /' >&2
else
    weston_up; g_session=$?
    if [ $g_session = 1 ]; then
        est=0   # weston_up already said what went wrong and set the gate red
        : > "$OUT/events.out"; : > "$OUT/events.err"
    elif [ $g_session = 0 ]; then
        # IN ITS OWN SESSION, and only this leg -- see the weston_up banner for
        # why the session is here and what it does and does not prove.
        run_wine_session "$OUT/events.exe" \
            > "$OUT/events.out" 2>"$OUT/events.err"
        est=$?
    else
        run_wine "$OUT/events.exe" "$TIMEOUT" \
            > "$OUT/events.out" 2>"$OUT/events.err"
        est=$?
    fi
    sed 's/^/  events| /' "$OUT/events.out"
    if [ $est -eq 124 ] || [ $est -eq 137 ]; then
        bad "leg G: the events probe HUNG"
    elif ! grep -q "EVENTS-SMOKE PASS 0" "$OUT/events.out"; then
        tail -10 "$OUT/events.err" | sed 's/^/  events| /' >&2
        bad "leg G: the events probe failed its own verdicts"
    elif ! grep -q "signaled=yes" "$OUT/events.out"; then
        bad "leg G: the fence event never signaled -- the relay is not live"
    elif ! grep -q "composition hr=0x00000000" "$OUT/events.out"; then
        bad "leg G: the composition swapchain did not serve"
    elif ! grep -q "luid_canary hr=0x80004001" "$OUT/events.out" || \
         ! grep -q "corewindow_canary hr=0x80004001" "$OUT/events.out"; then
        bad "leg G: a canary moved -- a DXVK behavior a served row cites has \
changed; reclassify the row (see CANARY_SERVE_DXVK)"
    else
        say "leg G: fence event relayed, composition swapchain served, \
canaries hold, annotations alive"
    fi
    weston_down
fi

[ $fail -eq 0 ] && say "PASS"
exit $fail
