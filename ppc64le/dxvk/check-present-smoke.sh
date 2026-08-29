#!/bin/sh
#
# check-present-smoke.sh -- the gate that proves a frame reaches the SCREEN.
#
# ppc64le/dxvk/check-d3d11-smoke.sh proves a guest D3D11 call reaches the same
# DXVK a native process reaches and gets the same texels back, with no window
# anywhere in the process.  That is everything except the claim a person
# actually cares about.  This file proves the rest: an x86-64 guest creates a
# real Wine window, creates a swapchain on it, clears and presents -- and a
# SEPARATE native ppc64le process, with no Wine and no guest in it, reads the
# COMPOSITOR's own framebuffer and finds exactly those bytes in exactly a
# window-sized rectangle.
#
# TWO INDEPENDENT OBSERVERS, ONE COLOUR.  probes/present_smoke.c reports what
# D3D11 says is in the back buffer; probes/present_capture.c reports what the
# display server says it composited.  They share no code, no ABI and no
# process, and neither can see what the other sees.  "It renders" and "it is
# visible" are separate claims and this is the arrangement in which they stay
# separate -- a gate that only mapped the back buffer would pass with the
# surface attached to nothing at all, which is exactly the state this lane was
# in before presentation existed.
#
# WHY A HEADLESS WESTON AND NOT AN Xvfb.  [MEASURED] 2026-08-17, the test machine: an Xvfb
# has no DRI3, and RADV refuses to present to an X server without it.  With no
# Wine anywhere in the process, `DISPLAY=:73 vkcube` on an Xvfb prints
#
#     MESA: info: vulkan: No DRI3 support detected - required for presentation
#
# and dumps core.  Driven through this lane the same limit shows up one layer
# up: the win32u client surface is created, DXVK builds a 256x256
# B8G8R8A8_UNORM swapchain on it, and the first vkAcquireNextImageKHR returns
# VK_ERROR_SURFACE_LOST_KHR.  That is the display server's limitation and not
# this lane's, but a gate cannot be run on a display that cannot present.  A
# headless Weston with the GL renderer has no such problem -- it imports the
# frame as a dmabuf on the same GPU -- and it is MORE isolated than an Xvfb,
# not less: its own socket, in its own XDG_RUNTIME_DIR, reachable by nothing
# else on the machine.
#
# IT NEVER TOUCHES A DISPLAY IT DID NOT CREATE.  $DISPLAY, $WAYLAND_DISPLAY and
# $XDG_RUNTIME_DIR are all unset for every process this script starts, and
# replaced by a runtime directory of its own.  The person running this may be
# logged in on a desktop of their own; a gate that could paint a rectangle on
# it -- or read it -- is not something to leave lying around in a repository.
#
# Six legs:
#
#   A  COMPOSITOR: a headless Weston of our own, GL renderer, 640x480, on a
#      private socket.  --debug is passed because Weston refuses screen
#      capture to unprivileged clients without it ("Output capture error:
#      unauthorized"), and the capture is the whole point of the exercise.
#   B  BUILD: the guest PE probe (imports d3d11.dll, user32.dll, kernel32.dll
#      -- what a real application imports) and the native PNG capture program.
#   C  GUEST: the probe runs under this port's wine against that compositor,
#      presents WARMUP frames, prints READY, and keeps presenting.  Its own
#      back-buffer readback must find every texel of the expected colour.
#   D  CAPTURE: while it is still presenting, weston-screenshooter writes what
#      the compositor composited, and the capture program requires the exact
#      colour to occupy a bounding box of exactly the window's size, entirely
#      filled.  A scaled frame fails on the size; a faded or blended one fails
#      because there are no exact matches at all.
#   D9 THE SAME TWO LEGS FOR D3D9, with probes/present_smoke9.c and the same
#      compositor and the same capture program.  D3D9's path is not a smaller
#      version of D3D11's: there is no DXGI and no swapchain object, the
#      surface is built inside IDirect3D9::CreateDevice itself, Present is a
#      method on the DEVICE with a per-call window override, and every frame
#      goes through the hand-written Clear slot whose by-value float MS-x64
#      spills to the stack.  Sharing the compositor and the capture makes the
#      two legs' results comparable byte for byte: both must put the SAME
#      colour in the SAME sized rectangle.
#   E  AGREEMENT: the colour the probe says it confirmed and the colour the
#      capture was asked about are compared as bytes, not as descriptions.
#   F  TEARDOWN: the probe exited on its own, the compositor is gone, and
#      nothing this script started is still running.
#
# --sabotage runs the negative controls instead and requires ALL of them to go
# red, because a gate that cannot fail proves nothing:
#
#   1  SMOKE_BREAK=2 -- the probe presents ZERO frames.  Its own readback
#      still passes; the capture MUST fail.  This is the control that catches
#      a gate which proves rendering and calls it presentation.
#   2  SMOKE_BREAK=3 -- the probe clears to a colour one step of green away.
#      The capture must REJECT the original colour and ACCEPT the new one, so
#      the comparison is on the value and not on the fact that something was
#      drawn.
#   3  DXVK_WSI_DRIVER=Headless -- the identical binary with the WSI backend
#      that owns no window.  Swapchain creation must fail rather than succeed
#      and present nowhere.
#   4  WINEEMUNOCOMWRAP=1 -- raw host pointers handed to the guest, the defect
#      the proxy runtime exists to prevent.  The probe must not print PASS.
#   5  The D3D9 probe with SMOKE_BREAK=2 -- zero frames presented, same
#      control as (1) on the other API.
#
# Exit 0 = pass, 1 = a check failed, 2 = could not run at all (a skip is NOT a
# pass).
#
# WHY EVERY WINE RUN DISABLES winedbg, verbatim from check-d3d11-smoke.sh
# because the hazard is identical: the bringup prefix has AeDebug configured
# with "winedbg --auto", so any run that ends in an unhandled fault -- which is
# what a defect in this path looks like from outside -- starts a debugger that
# attaches and never lets go, turning every red state into a hang.
set -u

HERE=$(cd "$(dirname "$0")" && pwd)
SRC=$(cd "$HERE/../.." && pwd)
BUILD=${BUILD:-$SRC}
OUT=${OUT:-/tmp/present-smoke}
SABOTAGE=0
[ "${1:-}" = "--sabotage" ] && SABOTAGE=1

say()  { echo "check-present-smoke: $*"; }
bad()  { echo "check-present-smoke: FAIL $*" >&2; fail=1; }
note() { echo "check-present-smoke: note $*"; }
skip() { echo "check-present-smoke: $*" >&2; cleanup; exit 2; }

# The window the probe creates and the colour it clears to.  Stated in
# probes/present_smoke.c and repeated here rather than parsed out of its
# output, so a probe that silently stopped clearing could not also silently
# move the goalposts.  RGB order, which is the order a PNG stores.
WIN_W=256
WIN_H=256
WANT_R=00
WANT_G=40
WANT_B=80
WANT_G3=41          # the SMOKE_BREAK=3 colour, one step of green
CHILD_W=128         # the WS_CHILD leg's swapchain, from probes/present_smoke.c
CHILD_H=96

RUNDIR=$OUT/runtime
SOCKET=wine-present-smoke
WESTON_PID=
PROBE_PID=

# Kill by PID first and by PATTERN second, and never by a pattern that could
# match anything but ours.  [MEASURED] the PID alone was not enough: a shell
# FUNCTION backgrounded with & runs in a subshell, so $! named the subshell and
# not the compositor, and two of them were left running on this machine -- one
# per red run.  The patterns below name this script's own socket and its own
# scratch directory, both unique to it; another agent's headless weston on
# another socket was running at the same time and must not be touched.
cleanup() {
    [ -n "${PROBE_PID:-}" ] && kill "$PROBE_PID" 2>/dev/null
    [ -n "${WESTON_PID:-}" ] && kill "$WESTON_PID" 2>/dev/null
    pkill -f "socket=$SOCKET" 2>/dev/null
    pkill -f "$OUT/.*\.exe" 2>/dev/null
    [ -n "${PROBE_PID:-}" ] && wait "$PROBE_PID" 2>/dev/null
    [ -n "${WESTON_PID:-}" ] && wait "$WESTON_PID" 2>/dev/null
    PROBE_PID=; WESTON_PID=
    # Give the compositor's own children (its shell client, its keyboard) the
    # moment they need to follow it out, so leg F is measuring teardown and not
    # scheduling.
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
    skip "no winewayland.drv; this gate presents to a headless Wayland \
compositor (see the banner for why not an Xvfb)"
command -v clang >/dev/null || skip "need clang for the guest build"
command -v llvm-dlltool >/dev/null || skip "need llvm-dlltool for the guest build"
command -v weston >/dev/null || skip "need weston for the isolated compositor"
command -v weston-screenshooter >/dev/null || \
    skip "need weston-screenshooter to read back what was composited"
command -v "${CC:-gcc}" >/dev/null || skip "need ${CC:-gcc} for the capture program"

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
TIMEOUT=${TIMEOUT:-180}
INCL="-I$BUILD/include -I$SRC/include -I$SRC/include/msvcrt"

# Every process below runs with THIS environment and no other.  -u on all three
# display variables is the load-bearing part: with XDG_RUNTIME_DIR left alone,
# a Wayland client finds the caller's compositor through the default socket
# name even with WAYLAND_DISPLAY unset, which is how a probe ends up drawing on
# somebody's desktop.
iso() { env -u DISPLAY -u WAYLAND_DISPLAY XDG_RUNTIME_DIR="$RUNDIR" "$@"; }

# ---- A: a compositor of our own ---------------------------------------------
rm -rf "$RUNDIR" && mkdir -p "$RUNDIR" && chmod 700 "$RUNDIR" || \
    skip "cannot create a private runtime directory at $RUNDIR"

# Spelled out rather than run through iso(), because a shell FUNCTION started
# with & becomes a subshell and $! would then name the subshell rather than the
# compositor -- see cleanup() for what that cost.  `env` execs its argument, so
# this PID really is weston's.
env -u DISPLAY -u WAYLAND_DISPLAY XDG_RUNTIME_DIR="$RUNDIR" \
    weston --backend=headless --renderer=gl \
    --width=640 --height=480 --socket="$SOCKET" --debug \
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
if ! grep -q "Using GL renderer" "$OUT/weston.log"; then
    sed 's/^/  weston| /' "$OUT/weston.log" >&2
    skip "weston came up without the GL renderer; a pixman-rendered compositor \
cannot import the dmabuf DXVK presents, so this gate would be measuring the \
compositor and not the lane"
fi
say "compositor: headless weston 640x480, GL renderer on $(grep -m1 'GL renderer:' \
"$OUT/weston.log" | sed 's/.*GL renderer: //'), socket $RUNDIR/$SOCKET"

# ---- B: build ---------------------------------------------------------------
cat > "$OUT/kernel32.def" <<'EOF'
LIBRARY kernel32.dll
EXPORTS
GetStdHandle
WriteFile
ExitProcess
GetModuleHandleA
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
-Wl,--entry=present_smoke_entry -Wl,--subsystem,windows"

build_probe() {   # $1 = output basename, $2.. = extra -D
    _o=$1; shift
    $GUESTCC "$@" -c -o "$OUT/$_o.o" "$HERE/probes/present_smoke.c" \
        2>"$OUT/$_o.build.err" || return 1
    $GUESTLD -o "$OUT/$_o.exe" "$OUT/$_o.o" \
        "$OUT/libd3d11.a" "$OUT/libuser32.a" "$OUT/libkernel32.a" \
        2>>"$OUT/$_o.build.err" || return 1
    return 0
}

GUESTLD9="clang -target x86_64-windows-gnu -fuse-ld=lld -nostdlib \
-Wl,--entry=present_smoke9_entry -Wl,--subsystem,windows"

build_probe9() {  # $1 = output basename, $2.. = extra -D
    _o=$1; shift
    $GUESTCC "$@" -c -o "$OUT/$_o.o" "$HERE/probes/present_smoke9.c" \
        2>"$OUT/$_o.build.err" || return 1
    $GUESTLD9 -o "$OUT/$_o.exe" "$OUT/$_o.o" \
        "$OUT/libd3d9.a" "$OUT/libuser32.a" "$OUT/libkernel32.a" \
        2>>"$OUT/$_o.build.err" || return 1
    return 0
}

build_probe present || {
    sed 's/^/  guest| /' "$OUT/present.build.err" >&2
    skip "the guest present probe did not build"
}

# The D3D9 leg.  Built only if the guest d3d9 thunk exists, so this gate still
# means something on a tree where the D3D9 lane has not been built -- but it is
# a NOTE and not a silent omission, because "the D3D9 leg did not run" and "the
# D3D9 leg passed" must never look the same from the outside.
D3D9_LEG=1
if [ ! -f "$BUILD/dlls/d3d9/x86_64-windows/d3d9.dll" ]; then
    D3D9_LEG=0
    note "no guest d3d9 thunk at $BUILD/dlls/d3d9/x86_64-windows/d3d9.dll; the \
D3D9 presentation leg will not run"
else
    cat > "$OUT/d3d9.def" <<'EOF'
LIBRARY d3d9.dll
EXPORTS
Direct3DCreate9
EOF
    llvm-dlltool -m i386:x86-64 -d "$OUT/d3d9.def" -l "$OUT/libd3d9.a" \
        || skip "llvm-dlltool failed for d3d9"
    build_probe9 present9 || {
        sed 's/^/  guest9| /' "$OUT/present9.build.err" >&2
        skip "the guest D3D9 present probe did not build"
    }
fi

${CC:-gcc} -std=c11 -O2 -Wall -o "$OUT/capture" "$HERE/probes/present_capture.c" \
    $(pkg-config --cflags --libs libpng 2>/dev/null || echo -lpng) \
    2>"$OUT/capture.build.err" || {
    sed 's/^/  capture| /' "$OUT/capture.build.err" >&2
    skip "the native PNG capture program did not build (are libpng development \
files installed?)"
}
say "build: guest present probe and native capture program"

# ---- the run harness --------------------------------------------------------
# Start the probe, wait for its own READY line -- never for a wall-clock guess.
run_probe() {   # $1 = exe basename, $2.. = extra env assignments
    _exe=$1; shift
    rm -f "$OUT/$_exe.out" "$OUT/$_exe.err"
    ( iso timeout -k 5 "$TIMEOUT" env WAYLAND_DISPLAY="$SOCKET" \
        WINEDEBUG="${WINEDEBUG:--all}" WINEDLLOVERRIDES="winedbg.exe=d" "$@" \
        "$BUILD/wine" "$OUT/$_exe.exe" >"$OUT/$_exe.out" 2>"$OUT/$_exe.err" ) &
    PROBE_PID=$!
    _i=0
    while [ $_i -lt 900 ]; do
        grep -q "present_smoke9\?: READY" "$OUT/$_exe.out" 2>/dev/null && return 0
        kill -0 "$PROBE_PID" 2>/dev/null || return 1
        _i=$((_i + 1)); sleep 0.1
    done
    return 1
}

# Ask the compositor for what it composited.  weston-screenshooter writes into
# its working directory with a name it chooses, so it gets a directory of its
# own and the newest file in it is the answer.
# $4/$5 are the rectangle's expected size and default to the top-level window's.
# The child-window leg passes the CHILD's size for one question and the
# parent's for the other, against the SAME screenshot -- so it must be possible
# to ask twice without taking a second picture in between, which is why the
# screenshot is only retaken when a size is not supplied for it.
capture() {     # $1 = r, $2 = g, $3 = b, [$4 = w, $5 = h]
    rm -rf "$OUT/shot" && mkdir -p "$OUT/shot" || return 2
    ( cd "$OUT/shot" && iso timeout 20 env WAYLAND_DISPLAY="$SOCKET" \
        weston-screenshooter ) >"$OUT/shot.log" 2>&1
    _png=$(ls -1t "$OUT/shot"/*.png 2>/dev/null | head -1)
    if [ -z "$_png" ]; then
        sed 's/^/  shot| /' "$OUT/shot.log" >&2
        return 2
    fi
    LAST_PNG=$_png
    "$OUT/capture" "$_png" "${4:-$WIN_W}" "${5:-$WIN_H}" "$1" "$2" "$3" \
        >"$OUT/capture.out" 2>&1
}

# ...and this asks a different question of the picture already taken.
recapture() {   # $1 = r, $2 = g, $3 = b, $4 = w, $5 = h
    [ -n "${LAST_PNG:-}" ] || return 2
    "$OUT/capture" "$LAST_PNG" "$4" "$5" "$1" "$2" "$3" >"$OUT/capture.out" 2>&1
}

wait_probe() {
    wait "$PROBE_PID" 2>/dev/null
    PROBE_RC=$?
    PROBE_PID=
}

# ---- (also available standalone as --sabotage): the negative controls -------
sabotage() {
    ok=1

    # 1: zero frames presented.  The probe's own readback still passes and the
    #    screen must stay blank -- the case a back-buffer-only gate misses.
    if build_probe nopresent -DSMOKE_BREAK=2; then
        if run_probe nopresent; then
            capture "$WANT_R" "$WANT_G" "$WANT_B"; crc=$?
            wait_probe
            if [ $crc -eq 0 ]; then
                bad "SMOKE_BREAK=2 presented nothing and the capture still \
matched -- this gate is reading something other than the screen"; ok=0
            else
                say "sabotage: SMOKE_BREAK=2 (no Present) failed the capture, \
as it must: $(grep -m1 'exact matches' "$OUT/capture.out")"
            fi
        else
            wait_probe
            bad "SMOKE_BREAK=2 probe never reached READY"; ok=0
        fi
    else
        bad "SMOKE_BREAK=2 build failed; cannot prove this check can fail"; ok=0
    fi

    # 2: one step of green.  The capture must reject the ORIGINAL colour and
    #    accept the new one, so the comparison is on the value.
    if build_probe green -DSMOKE_BREAK=3; then
        if run_probe green; then
            capture "$WANT_R" "$WANT_G" "$WANT_B"; crc_old=$?
            capture "$WANT_R" "$WANT_G3" "$WANT_B"; crc_new=$?
            wait_probe
            if [ $crc_old -eq 0 ]; then
                bad "SMOKE_BREAK=3 drew 0x41 green and the capture still \
matched 0x40 -- the comparison has a tolerance it must not have"; ok=0
            elif [ $crc_new -ne 0 ]; then
                bad "SMOKE_BREAK=3 drew 0x41 green and the capture did not \
match 0x41 either; the capture is not reading the window"
                sed 's/^/  capture| /' "$OUT/capture.out" >&2
                ok=0
            else
                say "sabotage: SMOKE_BREAK=3 (green 0x40 -> 0x41) was rejected \
against the old colour and accepted against the new one, one byte apart"
            fi
        else
            wait_probe
            bad "SMOKE_BREAK=3 probe never reached READY"; ok=0
        fi
    else
        bad "SMOKE_BREAK=3 build failed; cannot prove this check can fail"; ok=0
    fi

    # 3: the WSI backend that owns no window.  Swapchain creation must FAIL.
    iso timeout -k 5 "$TIMEOUT" env WAYLAND_DISPLAY="$SOCKET" \
        WINEDEBUG="${WINEDEBUG:--all}" WINEDLLOVERRIDES="winedbg.exe=d" \
        DXVK_WSI_DRIVER=Headless "$BUILD/wine" "$OUT/present.exe" \
        >"$OUT/headless.out" 2>"$OUT/headless.err"
    if grep -q "present_smoke: PASS" "$OUT/headless.out"; then
        bad "DXVK_WSI_DRIVER=Headless still PASSED -- a backend that owns no \
window created a swapchain anyway"; ok=0
    else
        say "sabotage: DXVK_WSI_DRIVER=Headless failed the run, as it must: \
$(grep -m1 'step 2' "$OUT/headless.out" | cut -c1-90)"
    fi

    # 4: raw host pointers.
    iso timeout -k 5 "$TIMEOUT" env WAYLAND_DISPLAY="$SOCKET" \
        WINEDEBUG="${WINEDEBUG:--all}" WINEDLLOVERRIDES="winedbg.exe=d" \
        WINEEMUNOCOMWRAP=1 "$BUILD/wine" "$OUT/present.exe" \
        >"$OUT/nowrap.out" 2>"$OUT/nowrap.err"
    if grep -q "present_smoke: PASS" "$OUT/nowrap.out"; then
        bad "WINEEMUNOCOMWRAP=1 still PASSED -- the gate cannot go red"; ok=0
    else
        say "sabotage: WINEEMUNOCOMWRAP=1 failed the guest run, as it must"
    fi

    # 5: the same zero-frames control on the D3D9 leg.  Its own steps still
    #    pass -- CreateDevice succeeded, the surface is real -- and the screen
    #    must stay blank.
    if [ "$D3D9_LEG" = 1 ]; then
        if build_probe9 nopresent9 -DSMOKE_BREAK=2; then
            if run_probe nopresent9; then
                capture "$WANT_R" "$WANT_G" "$WANT_B"; crc9=$?
                wait_probe
                if [ $crc9 -eq 0 ]; then
                    bad "the D3D9 SMOKE_BREAK=2 build presented nothing and the \
capture still matched"; ok=0
                else
                    say "sabotage: the D3D9 SMOKE_BREAK=2 build (no Present) \
failed the capture, as it must: $(grep -m1 'exact matches' "$OUT/capture.out")"
                fi
            else
                wait_probe
                bad "the D3D9 SMOKE_BREAK=2 probe never reached READY"; ok=0
            fi
        else
            bad "the D3D9 SMOKE_BREAK=2 build failed; cannot prove this check \
can fail"; ok=0
        fi
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

# ---- C: the guest presents ---------------------------------------------------
if ! run_probe present; then
    sed 's/^/  guest| /' "$OUT/present.out" >&2
    tail -20 "$OUT/present.err" >&2
    wait_probe
    bad "the guest probe never reached READY (it did not get a swapchain)"
    cleanup
    exit $fail
fi
say "guest: reached READY and is presenting"

# ---- D: what the compositor composited --------------------------------------
capture "$WANT_R" "$WANT_G" "$WANT_B"
cap_rc=$?
sed 's/^/  /' "$OUT/capture.out" 2>/dev/null
case $cap_rc in
    0) say "capture: the compositor's own framebuffer holds a ${WIN_W}x${WIN_H} \
rectangle of exactly RGB $WANT_R $WANT_G $WANT_B" ;;
    1) bad "what the compositor composited is not the colour the guest cleared \
to, or not at the size it was presented at" ;;
    *) bad "the capture could not be made at all" ;;
esac

# ---- let the probe finish, then read what it said ---------------------------
wait_probe
if [ "$PROBE_RC" -eq 124 ] || [ "$PROBE_RC" -eq 137 ]; then
    bad "the guest probe timed out after ${TIMEOUT}s"
elif grep -q "present_smoke: PASS" "$OUT/present.out"; then
    say "guest:  $(tail -1 "$OUT/present.out")"
    say "guest:  $(grep -m1 '^backbuffer:' "$OUT/present.out")"
else
    sed 's/^/  guest| /' "$OUT/present.out" >&2
    tail -20 "$OUT/present.err" >&2
    bad "the guest probe did not pass its own back-buffer readback"
fi

# ---- E: the two observers agree ---------------------------------------------
# Compared as bytes.  The probe prints the BGRA it expected and confirmed in
# the back buffer; this script asked the capture about the same colour in RGB.
# If the probe ever stops clearing to the colour this script asks about, this
# is the line that notices.
probe_want=$(sed -n 's/^expected-bgra: //p' "$OUT/present.out" | head -1)
want_bgra=$(echo "$WANT_B $WANT_G $WANT_R ff" | tr 'A-F' 'a-f')
if [ "$(echo "$probe_want" | tr 'A-F' 'a-f')" = "$want_bgra" ]; then
    say "agreement: the guest confirmed BGRA '$probe_want' in the back buffer \
and the capture was asked for the same colour as RGB '$WANT_R $WANT_G $WANT_B'"
else
    bad "the guest says it cleared to BGRA '$probe_want' but this gate asked \
the capture about '$want_bgra'; the two halves of this gate have come apart"
fi

# ---- D9: the same two legs, for D3D9 ----------------------------------------
# The same compositor, the same capture program and the same expected bytes.
# D3D9's route to a surface is entirely different -- it is built inside
# IDirect3D9::CreateDevice, before any swapchain object exists -- so this is a
# second independent path to the same win32u seam, not a variation on the
# first.  It also runs WITHOUT d3d9.deferSurfaceCreation, which is the option
# check-d3d9-smoke.sh must set to get a device at all: here the window is real,
# so the eager path is the one that has to work.
if [ "$D3D9_LEG" = 1 ]; then
    if run_probe present9; then
        say "guest9: reached READY and is presenting"
        capture "$WANT_R" "$WANT_G" "$WANT_B"
        cap9_rc=$?
        sed 's/^/  /' "$OUT/capture.out" 2>/dev/null
        case $cap9_rc in
            0) say "capture9: the compositor's own framebuffer holds a \
${WIN_W}x${WIN_H} rectangle of exactly RGB $WANT_R $WANT_G $WANT_B, presented \
by D3D9" ;;
            1) bad "the D3D9 rectangle on screen is not the colour the guest \
cleared to, or not at the size it was presented at" ;;
            *) bad "the D3D9 capture could not be made at all" ;;
        esac
        wait_probe
        if grep -q "present_smoke9: PASS" "$OUT/present9.out"; then
            say "guest9: $(tail -1 "$OUT/present9.out")"
        else
            sed 's/^/  guest9| /' "$OUT/present9.out" >&2
            tail -20 "$OUT/present9.err" >&2
            bad "the guest D3D9 probe did not pass its own steps"
        fi
        probe9_want=$(sed -n 's/^expected-rgb: //p' "$OUT/present9.out" | head -1)
        if [ "$(echo "$probe9_want" | tr 'A-F' 'a-f')" = \
             "$(echo "$WANT_R $WANT_G $WANT_B" | tr 'A-F' 'a-f')" ]; then
            say "agreement9: the D3D9 guest cleared to RGB '$probe9_want' and \
the capture was asked for the same"
        else
            bad "the D3D9 guest says it cleared to RGB '$probe9_want' but this \
gate asked the capture about '$WANT_R $WANT_G $WANT_B'"
        fi
    else
        wait_probe
        sed 's/^/  guest9| /' "$OUT/present9.out" >&2
        tail -20 "$OUT/present9.err" >&2
        bad "the guest D3D9 probe never reached READY (CreateDevice got no \
implicit swapchain, so win32u gave it no surface)"
    fi
fi

# ---- CH: the swapchain on a CHILD window ------------------------------------
# A launcher renders into a child of its own frame, and so does every in-game
# UI panel and every embedded video view.  A child HWND is a different object
# to the graphics driver -- winex11 gives it its own X window, winewayland a
# subsurface -- so "the swapchain is on the child" is a claim about which
# window DXVK's WSI backend was handed, and getting it wrong is INVISIBLE from
# inside the process: every call succeeds, the back buffer holds the right
# texels, and the picture is merely in the wrong place at the wrong size.
#
# So the child is a different size from its parent (128x96 inside 256x256) and
# this leg asks the capture TWICE: the child's size must be found, and the
# parent's must not.  The second question is the one that does the work -- it
# is the exact shape of "presented to the parent instead", and it is checked
# rather than assumed because it is the failure this shape produces.
#
# [MEASURED] 2026-08-18, the test machine: it already works.  DXVK reports "Buffer size:
# 128x96", and the compositor's own framebuffer holds a 128x96 rectangle of
# exactly the cleared colour.  This leg exists so that it keeps working.
if build_probe child -DPRESENT_CHILD=1; then
    if run_probe child; then
        say "child: reached READY with the swapchain on a WS_CHILD window"
        capture "$WANT_R" "$WANT_G" "$WANT_B" "$CHILD_W" "$CHILD_H"
        ch_rc=$?
        sed 's/^/  /' "$OUT/capture.out" 2>/dev/null
        case $ch_rc in
            0) say "capture-child: the compositor's own framebuffer holds a \
${CHILD_W}x${CHILD_H} rectangle of exactly RGB $WANT_R $WANT_G $WANT_B -- the \
CHILD's size, presented by a swapchain created on the child" ;;
            1) bad "the rectangle on screen is not the child's size; the frame \
went somewhere other than the child window" ;;
            *) bad "the child capture could not be made at all" ;;
        esac
        # ...and it is NOT the parent's size.  Same screenshot, different
        # question; a frame that had gone to the parent would answer this one
        # instead of the one above.
        recapture "$WANT_R" "$WANT_G" "$WANT_B" "$WIN_W" "$WIN_H"
        if [ $? -eq 0 ]; then
            bad "the same frame ALSO fills a ${WIN_W}x${WIN_H} rectangle, which \
is the PARENT's size -- the swapchain is presenting to the parent"
        else
            say "capture-child: the same frame does NOT fill the parent's \
${WIN_W}x${WIN_H}, so the child is where it went"
        fi
        wait_probe
        if grep -q "present_smoke: PASS" "$OUT/child.out"; then
            say "child:  $(tail -1 "$OUT/child.out")"
        else
            sed 's/^/  child| /' "$OUT/child.out" >&2
            tail -20 "$OUT/child.err" >&2
            bad "the child-window probe did not pass its own back-buffer readback"
        fi
    else
        wait_probe
        sed 's/^/  child| /' "$OUT/child.out" >&2
        tail -20 "$OUT/child.err" >&2
        bad "the child-window probe never reached READY (no swapchain on a child \
HWND)"
    fi
else
    sed 's/^/  child| /' "$OUT/child.build.err" >&2
    bad "the child-window probe did not build; this leg cannot run"
fi

# ---- F: nothing left behind --------------------------------------------------
cleanup
if pgrep -f "socket=$SOCKET" >/dev/null 2>&1; then
    bad "a weston on $SOCKET is still running after cleanup"
elif pgrep -f "$OUT/.*\.exe" >/dev/null 2>&1; then
    bad "a guest probe from $OUT is still running after cleanup"
else
    say "teardown: the compositor is gone and no probe is running"
fi

[ $fail -eq 0 ] && say "PASS"
exit $fail
