#!/bin/sh
#
# check-fullscreen-smoke.sh -- the gate that proves a swapchain can CHANGE.
#
# ppc64le/dxvk/check-present-smoke.sh proves a frame reaches the screen at the
# size the swapchain was created with.  Every real game then changes that size:
# a resolution setting is IDXGISwapChain::ResizeBuffers, a display-mode setting
# is user32's ChangeDisplaySettingsEx, and a fullscreen toggle is
# SetFullscreenState.  README recorded all three as unbuilt.
#
# They are not unbuilt at the boundary.  [MEASURED] dlls/d3d11/d3d11_marshal.h
# already carries complete plans for SetFullscreenState, GetFullscreenState,
# ResizeBuffers, ResizeTarget and GetContainingOutput on every IDXGISwapChain
# version the roster covers -- they are ordinary integer slots and the
# generator never refused them.  What was never established is whether they
# WORK: a slot that crosses correctly and a window operation that reaches the
# display server are two different things, and the failure shapes are
# different.  A slot that did not cross returns a wrong HRESULT.  A window
# operation that did not reach the driver returns S_OK and leaves the picture
# exactly the size it was.
#
# So this gate photographs the screen between phases and compares SIZES, which
# is the only thing that can tell those two apart.  Same compositor, same
# capture program and same colour as check-present-smoke.sh, deliberately: the
# two gates' results are then comparable byte for byte, and this one adds only
# the changes.
#
# Legs:
#
#   A  COMPOSITOR: a headless weston of this gate's own, GL renderer, on a
#      private socket in a runtime directory of its own.  Same arrangement and
#      same reasons as check-present-smoke.sh -- including that it never
#      touches a display it did not create, which matters more here than
#      anywhere else in this tree: this is the one gate that asks a program to
#      go fullscreen and to change a display mode, and neither is something to
#      do to somebody's desktop.
#   B  BUILD: the guest probe.
#   1  WINDOWED: 256x256 on screen, and the back buffer says 256x256.
#   2  RESIZED: SetWindowPos + ResizeBuffers to 192x144.  BOTH halves are
#      checked -- the rectangle on screen must be 192x144 AND DXGI's own
#      description of the back buffer must say 192x144.  Checking only the
#      screen would pass a resize that moved the window and left DXVK scaling
#      the old buffer into it, which is the commonest way to get this wrong.
#   3  FULLSCREEN: SetFullscreenState(TRUE).  Two claims, and they are not the
#      same claim.  The first is COHERENCE and it is a requirement: Set and Get
#      must agree.  The second is what the SCREEN did, and today the answer is a
#      named limitation rather than a pass or a fail -- DXGI accounts for the
#      transition and the window never moves, because this port's WSI backend
#      inherits ForeignWsiDriver's deliberate no-op enterFullscreenMode (see the
#      leg itself for why that premise is right for one backend and wrong for
#      this one).  The leg asserts THAT, positively: the rectangle must still be
#      the windowed size.  It goes red if the frame ends up some third size, and
#      it goes red if fullscreen starts working -- which is the point, because
#      the day it does, this file and ppc64le/dxvk/README.md are both wrong.
#   4  RESTORED: SetFullscreenState(FALSE) and back to 192x144, on screen and
#      in the back buffer.  A fullscreen that cannot be undone is worse than
#      one that never happened.
#   M  MODE: the probe's own EnumDisplaySettingsW/ChangeDisplaySettingsExW
#      report is echoed and required to be coherent -- at least one mode, a
#      non-zero screen size, and the same size restored afterwards.  On a
#      one-mode headless compositor a refusal is the correct answer and the
#      gate says so rather than failing.
#
# --sabotage runs the negative controls instead and requires ALL of them to go
# red:
#
#   1  FS_BREAK=1 -- the HALF-DONE RESIZE: move the window and do not call
#      ResizeBuffers.  DXVK then scales the old back buffer into the new
#      window, so the rectangle on screen is the RIGHT SIZE and the back
#      buffer is not.  This is the control that catches a gate which
#      photographs the screen and calls that a resize.
#   2  FS_BREAK=2 -- claim fullscreen without asking for it: report S_OK from
#      a SetFullscreenState that was never made.  GetFullscreenState then says
#      FALSE and the coherence check must go red.
#   3  SMOKE-level: the phase-1 capture asked for the phase-2 size must fail,
#      which is the same value check the positive legs rest on, made against a
#      picture that is known not to match.
#
# Exit 0 = pass, 1 = a check failed, 2 = could not run at all (a skip is NOT a
# pass).
#
# WHY EVERY WINE RUN DISABLES winedbg: verbatim from check-present-smoke.sh,
# because the hazard is identical -- a red state that starts a debugger which
# never attaches is a hang, and a hang is the one thing a gate must never be.
set -u

HERE=$(cd "$(dirname "$0")" && pwd)
SRC=$(cd "$HERE/../.." && pwd)
BUILD=${BUILD:-$SRC}
OUT=${OUT:-/tmp/fullscreen-smoke}
SABOTAGE=0
[ "${1:-}" = "--sabotage" ] && SABOTAGE=1

say()  { echo "check-fullscreen-smoke: $*"; }
bad()  { echo "check-fullscreen-smoke: FAIL $*" >&2; fail=1; }
note() { echo "check-fullscreen-smoke: note $*"; }
skip() { echo "check-fullscreen-smoke: $*" >&2; cleanup; exit 2; }

# Stated in probes/fullscreen_smoke.c and repeated here rather than parsed out
# of its output, so a probe that silently stopped resizing could not also
# silently move the goalposts.
P1_W=256
P1_H=256
P2_W=192
P2_H=144
SCREEN_W=640
SCREEN_H=480
WANT_R=00
WANT_G=40
WANT_B=80

RUNDIR=$OUT/runtime
SOCKET=wine-fullscreen-smoke
GOFILE=$OUT/go
WESTON_PID=
PROBE_PID=

# Kill by PID first and by PATTERN second, and never by a pattern that could
# match anything but ours -- the same arrangement, and the same measured
# reason, as check-present-smoke.sh's.
cleanup() {
    [ -n "${PROBE_PID:-}" ] && kill "$PROBE_PID" 2>/dev/null
    [ -n "${WESTON_PID:-}" ] && kill "$WESTON_PID" 2>/dev/null
    pkill -f "socket=$SOCKET" 2>/dev/null
    pkill -f "$OUT/.*\.exe" 2>/dev/null
    [ -n "${PROBE_PID:-}" ] && wait "$PROBE_PID" 2>/dev/null
    [ -n "${WESTON_PID:-}" ] && wait "$WESTON_PID" 2>/dev/null
    PROBE_PID=; WESTON_PID=
    _n=0
    while [ $_n -lt 50 ] && pgrep -f "socket=$SOCKET" >/dev/null 2>&1; do
        _n=$((_n + 1)); sleep 0.1
    done
}
trap cleanup EXIT INT TERM

[ -x "$BUILD/wine" ] || skip "no wine loader at $BUILD/wine"
[ -n "${WINEPREFIX:-}" ] || skip "set WINEPREFIX to a prefix wineboot has run in"
[ -n "${WINEFEXBRIDGE:-}" ] || skip "set WINEFEXBRIDGE to the emulator bridge"
[ -f "$BUILD/dlls/d3d11/x86_64-windows/d3d11.dll" ] || \
    skip "no guest d3d11 thunk; build it first"
[ -f "$BUILD/dlls/winewayland.drv/winewayland.so" ] || \
    skip "no winewayland.drv; this gate presents to a headless Wayland compositor"
command -v clang >/dev/null || skip "need clang for the guest build"
command -v llvm-dlltool >/dev/null || skip "need llvm-dlltool for the guest build"
command -v weston >/dev/null || skip "need weston for the isolated compositor"
command -v weston-screenshooter >/dev/null || \
    skip "need weston-screenshooter to read back what was composited"
command -v "${CC:-gcc}" >/dev/null || skip "need ${CC:-gcc} for the capture program"

mkdir -p "$OUT" || skip "cannot create $OUT"
fail=0
TIMEOUT=${TIMEOUT:-240}
INCL="-I$BUILD/include -I$SRC/include -I$SRC/include/msvcrt"

iso() { env -u DISPLAY -u WAYLAND_DISPLAY XDG_RUNTIME_DIR="$RUNDIR" "$@"; }

# ---- A: a compositor of our own ---------------------------------------------
rm -rf "$RUNDIR" && mkdir -p "$RUNDIR" && chmod 700 "$RUNDIR" || \
    skip "cannot create a private runtime directory at $RUNDIR"
env -u DISPLAY -u WAYLAND_DISPLAY XDG_RUNTIME_DIR="$RUNDIR" \
    weston --backend=headless --renderer=gl \
    --width=$SCREEN_W --height=$SCREEN_H --socket="$SOCKET" --debug \
    >"$OUT/weston.log" 2>&1 &
WESTON_PID=$!
i=0
while [ $i -lt 200 ]; do
    [ -S "$RUNDIR/$SOCKET" ] && break
    kill -0 "$WESTON_PID" 2>/dev/null || break
    i=$((i + 1)); sleep 0.1
done
[ -S "$RUNDIR/$SOCKET" ] || {
    sed 's/^/  weston| /' "$OUT/weston.log" >&2
    skip "weston did not come up on a private socket"
}
grep -q "Using GL renderer" "$OUT/weston.log" || {
    sed 's/^/  weston| /' "$OUT/weston.log" >&2
    skip "weston came up without the GL renderer; it could not import the frame \
DXVK presents, so this gate would be measuring the compositor"
}
say "compositor: headless weston ${SCREEN_W}x${SCREEN_H}, GL renderer on \
$(grep -m1 'GL renderer:' "$OUT/weston.log" | sed 's/.*GL renderer: //')"

# ---- B: build ---------------------------------------------------------------
cat > "$OUT/kernel32.def" <<'EOF'
LIBRARY kernel32.dll
EXPORTS
GetStdHandle
WriteFile
ExitProcess
GetModuleHandleA
GetEnvironmentVariableA
GetFileAttributesA
EOF
cat > "$OUT/user32.def" <<'EOF'
LIBRARY user32.dll
EXPORTS
RegisterClassA
CreateWindowExA
DestroyWindow
DefWindowProcA
ShowWindow
UpdateWindow
SetWindowPos
GetSystemMetrics
EnumDisplaySettingsW
ChangeDisplaySettingsExW
PeekMessageA
TranslateMessage
DispatchMessageA
EOF
cat > "$OUT/d3d11.def" <<'EOF'
LIBRARY d3d11.dll
EXPORTS
D3D11CreateDeviceAndSwapChain
EOF
for m in kernel32 user32 d3d11; do
    llvm-dlltool -m i386:x86-64 -d "$OUT/$m.def" -l "$OUT/lib$m.a" \
        || skip "llvm-dlltool failed for $m"
done

GUESTCC="clang -target x86_64-windows-gnu -nostdlibinc $INCL -Wall -O1 -fno-builtin -g"
GUESTLD="clang -target x86_64-windows-gnu -fuse-ld=lld -nostdlib \
-Wl,--entry=fullscreen_smoke_entry -Wl,--subsystem,windows"

build_probe() {   # $1 = output basename, $2.. = extra -D
    _o=$1; shift
    $GUESTCC "$@" -c -o "$OUT/$_o.o" "$HERE/probes/fullscreen_smoke.c" \
        2>"$OUT/$_o.build.err" || return 1
    $GUESTLD -o "$OUT/$_o.exe" "$OUT/$_o.o" \
        "$OUT/libd3d11.a" "$OUT/libuser32.a" "$OUT/libkernel32.a" \
        2>>"$OUT/$_o.build.err" || return 1
    return 0
}

build_probe fs || {
    sed 's/^/  guest| /' "$OUT/fs.build.err" >&2
    skip "the guest fullscreen probe did not build"
}

${CC:-gcc} -std=c11 -O2 -Wall -o "$OUT/capture" "$HERE/probes/present_capture.c" \
    $(pkg-config --cflags --libs libpng 2>/dev/null || echo -lpng) \
    2>"$OUT/capture.build.err" || {
    sed 's/^/  capture| /' "$OUT/capture.build.err" >&2
    skip "the native PNG capture program did not build"
}
say "build: guest fullscreen probe and native capture program"

# ---- the run harness --------------------------------------------------------
start_probe() {   # $1 = exe basename, $2.. = extra env
    _exe=$1; shift
    rm -f "$OUT/$_exe.out" "$OUT/$_exe.err" "$GOFILE".*
    ( iso timeout -k 5 "$TIMEOUT" env WAYLAND_DISPLAY="$SOCKET" \
        WINEDEBUG="${WINEDEBUG:--all}" WINEDLLOVERRIDES="winedbg.exe=d" \
        FULLSCREEN_GO="$GOFILE" "$@" \
        "$BUILD/wine" "$OUT/$_exe.exe" >"$OUT/$_exe.out" 2>"$OUT/$_exe.err" ) &
    PROBE_PID=$!
    PROBE_OUT=$OUT/$_exe.out
}

# Wait for a phase to announce itself.  Never a wall-clock guess: the probe
# prints PHASEn READY after its own warm-up frames, and the gate releases it
# with a file once the photograph is taken.
wait_phase() {   # $1 = phase number
    _i=0
    while [ $_i -lt 1800 ]; do
        grep -q "fullscreen_smoke: PHASE$1 READY" "$PROBE_OUT" 2>/dev/null && return 0
        kill -0 "$PROBE_PID" 2>/dev/null || return 1
        _i=$((_i + 1)); sleep 0.1
    done
    return 1
}
release_phase() { : > "$GOFILE.$1"; }

capture() {     # $1 r $2 g $3 b $4 w $5 h
    rm -rf "$OUT/shot" && mkdir -p "$OUT/shot" || return 2
    ( cd "$OUT/shot" && iso timeout 20 env WAYLAND_DISPLAY="$SOCKET" \
        weston-screenshooter ) >"$OUT/shot.log" 2>&1
    _png=$(ls -1t "$OUT/shot"/*.png 2>/dev/null | head -1)
    if [ -z "$_png" ]; then
        sed 's/^/  shot| /' "$OUT/shot.log" >&2
        return 2
    fi
    LAST_PNG=$_png
    "$OUT/capture" "$_png" "$4" "$5" "$1" "$2" "$3" >"$OUT/capture.out" 2>&1
}
recapture() {   # $1 r $2 g $3 b $4 w $5 h -- the same picture, a new question
    [ -n "${LAST_PNG:-}" ] || return 2
    "$OUT/capture" "$LAST_PNG" "$4" "$5" "$1" "$2" "$3" >"$OUT/capture.out" 2>&1
}

wait_probe() { wait "$PROBE_PID" 2>/dev/null; PROBE_RC=$?; PROBE_PID=; }

# Photograph one phase and release it.  $2/$3 is the size the rectangle must
# be; the capture's own message is echoed either way, because "what size WAS
# it" is the whole content of a failure here.
phase_capture() {   # $1 = phase, $2 = w, $3 = h, $4 = human description
    if ! wait_phase "$1"; then
        bad "phase $1 never announced itself ($4)"
        sed 's/^/  guest| /' "$PROBE_OUT" >&2
        return 1
    fi
    capture "$WANT_R" "$WANT_G" "$WANT_B" "$2" "$3"
    _rc=$?
    sed 's/^/  /' "$OUT/capture.out" 2>/dev/null
    case $_rc in
        0) say "phase $1: the compositor's framebuffer holds a ${2}x${3} \
rectangle of exactly RGB $WANT_R $WANT_G $WANT_B -- $4" ;;
        1) bad "phase $1: the rectangle on screen is not ${2}x${3} -- $4" ;;
        *) bad "phase $1: the capture could not be made at all" ;;
    esac
    release_phase "$1"
    return 0
}

# ---- (also standalone as --sabotage): the negative controls -----------------
sabotage() {
    ok=1

    # 1: the half-done resize.  The rectangle on screen is the RIGHT size --
    #    DXVK scales the old buffer into the new window -- and the back buffer
    #    is not.  A gate that only photographed the screen would pass this.
    if build_probe half -DFS_BREAK=1; then
        start_probe half
        wait_phase 1 && release_phase 1
        wait_phase 2 && release_phase 2
        release_phase 3; release_phase 4
        wait_probe
        if grep -q "fullscreen_smoke: PASS" "$OUT/half.out"; then
            bad "FS_BREAK=1 still PASSED -- the back-buffer size is not being \
checked, only the picture"
            ok=0
        elif grep -q "ResizeBuffers did not resize the back buffer" "$OUT/half.out"; then
            say "sabotage: FS_BREAK=1 (window moved, buffers not resized) failed \
at the back-buffer check, as it must: $(grep -m1 'phase 2: SetWindowPos' \
"$OUT/half.out" | cut -c1-120)"
        else
            bad "FS_BREAK=1 did not fail at the back-buffer check; the control \
is not falsifying what it claims to"
            sed 's/^/  half| /' "$OUT/half.out" >&2
            ok=0
        fi
    else
        bad "FS_BREAK=1 build failed; cannot prove this check can fail"; ok=0
    fi

    # 2: claim fullscreen without asking for it.
    if build_probe liar -DFS_BREAK=2; then
        start_probe liar
        wait_phase 1 && release_phase 1
        wait_phase 2 && release_phase 2
        release_phase 3; release_phase 4
        wait_probe
        if grep -q "fullscreen_smoke: PASS" "$OUT/liar.out"; then
            bad "FS_BREAK=2 still PASSED -- Set and Get are not being compared"
            ok=0
        elif grep -q "SetFullscreenState and GetFullscreenState disagree" "$OUT/liar.out"; then
            say "sabotage: FS_BREAK=2 (S_OK without the call) was caught by the \
coherence check, as it must be: $(grep -m1 'phase 3:' "$OUT/liar.out" | cut -c1-120)"
        else
            bad "FS_BREAK=2 did not fail at the coherence check"
            sed 's/^/  liar| /' "$OUT/liar.out" >&2
            ok=0
        fi
    else
        bad "FS_BREAK=2 build failed; cannot prove this check can fail"; ok=0
    fi

    # 3: the size comparison itself.  Photograph phase 1 and ask for phase 2's
    #    size; it must be refused.  Without this, every size check above could
    #    be a capture that says yes to anything.
    start_probe fs
    if wait_phase 1; then
        capture "$WANT_R" "$WANT_G" "$WANT_B" "$P1_W" "$P1_H"; _a=$?
        recapture "$WANT_R" "$WANT_G" "$WANT_B" "$P2_W" "$P2_H"; _b=$?
        release_phase 1; release_phase 2; release_phase 3; release_phase 4
        wait_probe
        if [ $_a -eq 0 ] && [ $_b -ne 0 ]; then
            say "sabotage: the same phase-1 picture is accepted at ${P1_W}x${P1_H} \
and REFUSED at ${P2_W}x${P2_H}, so the size check is on the value"
        else
            bad "the capture answered the same for two different sizes \
(${P1_W}x${P1_H} rc=$_a, ${P2_W}x${P2_H} rc=$_b); the size check proves nothing"
            ok=0
        fi
    else
        wait_probe
        bad "the probe never reached phase 1 under the size control"; ok=0
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

# ---- the run ----------------------------------------------------------------
start_probe fs

phase_capture 1 "$P1_W" "$P1_H" "windowed, the size the swapchain was created with"
phase_capture 2 "$P2_W" "$P2_H" "resized by SetWindowPos + ResizeBuffers"

# Phase 3 is only a size claim if the transition actually happened.  The probe
# says which, and the gate believes it rather than assuming -- a compositor is
# entitled to refuse fullscreen and a headless one often does.
if wait_phase 3; then
    if grep -q "fullscreen_smoke: PHASE3 REFUSED" "$OUT/fs.out"; then
        note "the compositor refused exclusive fullscreen; the port asked, \
SetFullscreenState and GetFullscreenState agreed that it did not happen, and \
that agreement is what step 3 checks.  There is no rectangle to photograph."
        release_phase 3
    else
        # DXGI SAYS FULLSCREEN.  What the screen says is the question, and the
        # answer today is a NAMED LIMITATION rather than a bug in this lane:
        #
        #   dxvk-patches/0001-foreign-wsi-backend.patch makes
        #   ForeignWsiDriver::enterFullscreenMode, leaveFullscreenMode,
        #   setWindowMode and resizeWindow no-ops that report success, with a
        #   comment saying why -- "the window belongs to somebody else", and
        #   DXVK follows whatever size the owner gives it through
        #   VkSurfaceCapabilitiesKHR::currentExtent.  That premise is right for
        #   the foreign-X11 backend, which is handed a raw XID belonging to
        #   another process.  It is WRONG for the Win32u backend, whose window
        #   is a Wine HWND that Wine can move -- and Win32uWsiDriver
        #   (0003-win32u-wsi-backend.patch) inherits those four methods
        #   unchanged.  So nothing ever asks Wine to resize the window, and
        #   SetFullscreenState is an accounting change inside DXGI.
        #
        # So this leg asserts the CURRENT, DOCUMENTED behaviour positively: the
        # rectangle must still be exactly the windowed size.  That is a real
        # value check with a real red state, and it goes red in BOTH directions
        # -- if somebody overrides those methods it stops matching and says so,
        # and if the frame ends up some third size (scaled, clipped, half
        # resized) it fails too.  What it must never do is pass quietly on a
        # transition that did not happen.
        # One photograph, two questions -- the whole screen and the window's
        # own size -- so the two answers cannot come from different frames.
        capture "$WANT_R" "$WANT_G" "$WANT_B" "$P2_W" "$P2_H"
        same_rc=$?
        recapture "$WANT_R" "$WANT_G" "$WANT_B" "$SCREEN_W" "$SCREEN_H"
        full_rc=$?
        sed 's/^/  /' "$OUT/capture.out" 2>/dev/null
        if [ $full_rc -eq 0 ]; then
            bad "phase 3: the frame now fills the whole ${SCREEN_W}x${SCREEN_H} \
screen -- exclusive fullscreen has started WORKING.  That is good news and this \
gate is now wrong: somebody has overridden ForeignWsiDriver's no-op fullscreen \
methods for the Win32u backend.  Update this leg and the 'not done' entry in \
ppc64le/dxvk/README.md."
        elif [ $same_rc -eq 0 ]; then
            note "phase 3: SetFullscreenState(TRUE) returned S_OK, \
GetFullscreenState agrees, and the rectangle on screen is STILL ${P2_W}x${P2_H} \
-- the windowed size.  The transition reached DXGI and stopped there, because \
this port's WSI backend inherits ForeignWsiDriver's deliberate no-op \
enterFullscreenMode.  Recorded as a limitation in ppc64le/dxvk/README.md; this \
leg asserts it rather than ignoring it."
        else
            bad "phase 3: the rectangle on screen is neither the screen's \
${SCREEN_W}x${SCREEN_H} nor the window's ${P2_W}x${P2_H}.  Something moved it \
partway, which is worse than either -- a scaled or clipped frame is the failure \
the two-size check exists to catch."
        fi
        release_phase 3
    fi
else
    bad "phase 3 never announced itself"
fi

phase_capture 4 "$P2_W" "$P2_H" "back to windowed after leaving fullscreen"

wait_probe
sed 's/^/  guest| /' "$OUT/fs.out"
if [ "${PROBE_RC:-1}" -eq 124 ] || [ "${PROBE_RC:-1}" -eq 137 ]; then
    bad "the guest probe timed out after ${TIMEOUT}s"
elif grep -q "fullscreen_smoke: PASS" "$OUT/fs.out"; then
    say "guest: $(tail -1 "$OUT/fs.out")"
else
    tail -20 "$OUT/fs.err" >&2
    bad "the guest probe did not pass its own checks"
fi

# ---- M: the display-mode report ---------------------------------------------
_mode=$(grep -m1 "EnumDisplaySettingsW" "$OUT/fs.out")
if [ -n "$_mode" ]; then
    say "mode: $(echo "$_mode" | sed 's/^step [0-9]* //' | cut -c1-160)"
    case "$_mode" in
        *"modes=0 "*) bad "the display driver reported NO modes at all; \
EnumDisplaySettingsW is not reaching a driver" ;;
        *"asked=none(one-mode-display)"*)
            note "this compositor has exactly one mode, so no mode change was \
requested.  ChangeDisplaySettingsExW is therefore UNPROVEN on this display -- \
it crossed and was never asked to do anything.  A display with more than one \
mode is what proves it, and a headless weston is not one." ;;
        *)
            _rc=$(echo "$_mode" | sed -n 's/.* rc=\([0-9-]*\).*/\1/p')
            if [ "${_rc:-1}" = 0 ]; then
                say "mode: ChangeDisplaySettingsExW returned DISP_CHANGE_SUCCESSFUL \
and the screen metrics followed"
            else
                note "ChangeDisplaySettingsExW returned $_rc on this display; the \
call crossed the boundary and the driver declined, which is a display answer \
rather than a boundary one"
            fi ;;
    esac
else
    bad "the probe printed no display-mode line at all"
fi

# ---- teardown ----------------------------------------------------------------
cleanup
if pgrep -f "socket=$SOCKET" >/dev/null 2>&1; then
    bad "a weston on $SOCKET is still running after cleanup"
else
    say "teardown: the compositor is gone and no probe is running"
fi

[ $fail -eq 0 ] && say "PASS"
exit $fail
