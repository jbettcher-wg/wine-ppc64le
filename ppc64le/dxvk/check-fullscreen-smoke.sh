#!/bin/sh
#
# check-fullscreen-smoke.sh -- the gate that proves a swapchain can CHANGE.
#
# ppc64le/dxvk/check-present-smoke.sh proves a frame reaches the screen at the
# size the swapchain was created with.  Every real game then changes that size:
# a resolution setting is IDXGISwapChain::ResizeBuffers, a display-mode setting
# is user32's ChangeDisplaySettingsEx, and a fullscreen toggle is
# SetFullscreenState.
#
# They were never unbuilt at the boundary.  [MEASURED] dlls/d3d11/d3d11_marshal.h
# has always carried complete plans for SetFullscreenState, GetFullscreenState,
# ResizeBuffers, ResizeTarget and GetContainingOutput on every IDXGISwapChain
# version the roster covers -- ordinary integer slots the generator never
# refused.  What was missing was underneath: a slot that crosses correctly and a
# window operation that reaches the display server are two different things, and
# they fail differently.  A slot that did not cross returns a wrong HRESULT.  A
# window operation that did not reach the driver returns S_OK and leaves the
# picture exactly the size it was.
#
# [MEASURED] 2026-08-18, and this gate is what found it: SetFullscreenState(TRUE)
# returned S_OK, GetFullscreenState agreed, and the rectangle on screen was
# still the windowed one -- because DXVK's Win32u WSI driver inherited
# ForeignWsiDriver's deliberate no-op enterFullscreenMode, whose premise ("the
# window belongs to somebody else") is right for a raw X11 window ID owned by
# another process and wrong for a Wine HWND.  The driver overrides it now
# (dxvk-patches/0004-win32u-window-ops.patch) through six new entries in the WSI
# callback table (ppc64le/dxvk/dxvk_win32u_wsi.h, abi 2) that end in
# NtUserSetWindowPos and NtUserChangeDisplaySettings.  This gate asserts the
# FIXED behaviour; the version of it that asserted the broken behaviour
# positively is what went red the day the fix landed, which was the point.
#
# WHY THERE IS NO NATIVE-VS-GUEST LEG HERE, unlike check-d3d11-smoke.sh.  There
# cannot be one, and the reason is structural rather than unfinished work: this
# probe needs a Wine HWND, and a native ppc64le process has no Wine in it at
# all, while a native ppc64le PE running UNDER Wine is refused this surface by
# construction -- dlls/d3d11/main.c's plain (non-__wine_guest_) flat exports
# refuse a native caller loudly, because a proxy's vtable is the guest thunk
# module's array of x86-64 trap stubs and a native caller would execute them as
# ppc64.  check-present-smoke.sh is the same family and has the same shape.  The
# two-observer standard is met a different way, and it is the stronger one for a
# claim about the screen: the guest probe reports what DXGI and user32 say, and
# a SEPARATE native ppc64le program with no Wine, no guest, no DXVK and no
# Vulkan in it reads the compositor's own framebuffer.  Nothing is shared
# between them that could make both wrong the same way.
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
#
#      ITS SIZE IS 1024x768 AND THAT IS LOAD-BEARING.  [MEASURED] win32u
#      synthesises the mode list for a display whose driver reports one mode
#      (dlls/win32u/sysparams.c get_virtual_modes), and the smallest entry in
#      its table is 640x480 -- so on the 640x480 compositor this gate used to
#      start, the whole list was three modes that were all 640x480 and NO mode
#      change could ever be requested.  That is why ChangeDisplaySettingsExW was
#      recorded as unproven rather than broken.  At 1024x768 the same code
#      offers 640x480, 800x600, 960x540 and 1024x768, and a mode change is a
#      real request with a real answer.
#   B  BUILD: the guest probes, from one source.
#   1  WINDOWED: 256x256 on screen, and the back buffer says 256x256.
#   2  RESIZED: SetWindowPos + ResizeBuffers to 192x144.  BOTH halves are
#      checked -- the rectangle on screen must be 192x144 AND DXGI's own
#      description of the back buffer must say 192x144.  Checking only the
#      screen would pass a resize that moved the window and left DXVK scaling
#      the old buffer into it, which is the commonest way to get this wrong.
#   3  FULLSCREEN: SetFullscreenState(TRUE).  Three claims from three places,
#      and they are not the same claim.  Set and Get must AGREE.  The window
#      must actually BE the screen (GetWindowRect, in the guest).  And the
#      compositor's own framebuffer must hold a rectangle of the SCREEN's size
#      in the presented colour -- which is the claim no application can make
#      about itself.  The same photograph is then asked whether it holds a
#      rectangle of the WINDOWED size, and must say no, because "still 192x144"
#      is precisely what this used to be.
#
#      THE PHOTOGRAPH IS OF A CLIPPED RECTANGLE, AND THAT IS A WAYLAND FACT
#      RATHER THAN A WEAKENING.  [MEASURED] 2026-08-18: no Wayland client may
#      place its own top-levels, and this weston puts the window somewhere when
#      it is mapped and never moves it again -- the same origin answered phases
#      1, 2 and 4.  A screen-sized rectangle at a non-zero origin therefore
#      runs off the screen, and what can be photographed is the part that is
#      on it.  So the gate reads the origin out of phase 2's own photograph and
#      requires phase 3's box to be exactly the on-screen part of a
#      SCREEN_W x SCREEN_H rectangle there, completely filled.  Every number is
#      still checked; none is a tolerance.  (It is not weston refusing to place
#      fullscreen surfaces in general -- its own weston-fullscreen client lands
#      at (0,0) covering the output on the same compositor.  A Wine window that
#      goes fullscreen AFTER being mapped windowed does not follow, and that is
#      a winewayland question and not a DXVK one.)
#   4  RESTORED: SetFullscreenState(FALSE) and back to 192x144, on screen, in
#      the back buffer, in the window rectangle and in the display mode -- with
#      the probe deliberately NOT moving the window itself, so what is being
#      checked is that DXGI restored it.  A fullscreen that cannot be undone is
#      worse than one that never happened.
#   M  MODE: a SECOND run of the same source built with
#      -DFS_MODE_SWITCH=1, whose swapchain carries
#      DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH.  Without that flag DXGI
#      deliberately asks for the mode the display is already in and no
#      ChangeDisplaySettings happens at all; with it, entering fullscreen picks
#      a mode out of the output's list -- which now comes from Wine through the
#      new callback rather than from the foreign driver's single synthetic one
#      -- and asks for it.  This leg takes no photograph on purpose: Wine
#      EMULATES a mode change on a display whose driver has one real mode, so
#      the compositor's framebuffer looks the same either way and the screen
#      METRICS are the only place the change is unambiguous.  The leg requires
#      the screen to change on the way in and to come back on the way out.
#
# --sabotage runs the negative controls instead and requires ALL of them to go
# red:
#
#   1  WINEDXVKNOWINDOWOPS=1 -- THE LEVER ON THE FIX ITSELF.  Wine publishes the
#      WSI callback table with its six abi-2 entries NULL; DXVK's driver checks
#      every one before calling it and falls back to the inherited no-op, so the
#      port behaves EXACTLY as it did before this work.  The main leg must go
#      red, in the guest's own numbers and in the photograph both.  A gate whose
#      only negative control turned off its own assertions would prove nothing
#      about the code; this one turns off the code.
#   2  WINEDXVKNOWINDOWOPS=1 on the MODE leg, which must go red on the display
#      mode rather than on the rectangle -- a different assertion failing for a
#      different reason, from the same lever.
#   3  FS_BREAK=1 -- the HALF-DONE RESIZE: move the window and do not call
#      ResizeBuffers.  DXVK then scales the old back buffer into the new
#      window, so the rectangle on screen is the RIGHT SIZE and the back
#      buffer is not.  This is the control that catches a gate which
#      photographs the screen and calls that a resize.
#   4  FS_BREAK=2 -- claim fullscreen without asking for it: report S_OK from
#      a SetFullscreenState that was never made.  GetFullscreenState then says
#      FALSE and the coherence check must go red.
#   5  SMOKE-level: the phase-1 capture asked for the phase-2 size must fail,
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
# EXPECT_RED is set while a negative control is running, and changes only the
# WORD.  The whole point of the lever controls is that the positive legs run
# again unchanged and fail, so the same `bad` has to record the same thing --
# but a reader watching --sabotage scroll past must be able to tell a control
# doing its job from the gate actually breaking, and "FAIL" everywhere makes
# those two look identical.
EXPECT_RED=0
bad()  {
    if [ "$EXPECT_RED" = 1 ]; then echo "check-fullscreen-smoke: red-as-expected $*" >&2
    else echo "check-fullscreen-smoke: FAIL $*" >&2; fi
    fail=1
}
note() { echo "check-fullscreen-smoke: note $*"; }
skip() { echo "check-fullscreen-smoke: $*" >&2; cleanup; exit 2; }

# Stated in probes/fullscreen_smoke.c and repeated here rather than parsed out
# of its output, so a probe that silently stopped resizing could not also
# silently move the goalposts.
P1_W=256
P1_H=256
P2_W=192
P2_H=144
# The compositor's size, which is also the size a fullscreen window has to
# become.  See the note on leg A about why it is not 640x480 any more.
SCREEN_W=1024
SCREEN_H=768
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

# DXVK writes <appname>_d3d11.log next to the CURRENT DIRECTORY by default, and
# a gate is normally run from the top of the source tree -- so a plain run left
# native_d3d11.log, native_dxgi.log and a wine-preloader_* pair lying in the
# checkout.  Point them at this gate's own work directory instead; DXVK_LOG_PATH
# takes a directory, and "none" would suppress the logs entirely, which is worse
# when a leg fails and the log is the evidence.
DXVK_LOG_PATH=$OUT
export DXVK_LOG_PATH
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
GetWindowRect
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
build_probe fsmode -DFS_MODE_SWITCH=1 || {
    sed 's/^/  guest| /' "$OUT/fsmode.build.err" >&2
    skip "the guest mode-switch probe did not build"
}

${CC:-gcc} -std=c11 -O2 -Wall -o "$OUT/capture" "$HERE/probes/present_capture.c" \
    $(pkg-config --cflags --libs libpng 2>/dev/null || echo -lpng) \
    2>"$OUT/capture.build.err" || {
    sed 's/^/  capture| /' "$OUT/capture.build.err" >&2
    skip "the native PNG capture program did not build"
}
say "build: guest fullscreen and mode-switch probes, and the native capture \
program"

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
release_all()   { release_phase 1; release_phase 2; release_phase 3; release_phase 4; }

# $6/$7, when given, are the ORIGIN the rectangle is expected at, and switch
# the capture into its clipped mode -- see probes/present_capture.c for the
# measured Wayland fact that makes that mode necessary, and remember_origin
# below for where the number comes from.
capture() {     # $1 r $2 g $3 b $4 w $5 h [$6 x $7 y]
    rm -rf "$OUT/shot" && mkdir -p "$OUT/shot" || return 2
    ( cd "$OUT/shot" && iso timeout 20 env WAYLAND_DISPLAY="$SOCKET" \
        weston-screenshooter ) >"$OUT/shot.log" 2>&1
    _png=$(ls -1t "$OUT/shot"/*.png 2>/dev/null | head -1)
    if [ -z "$_png" ]; then
        sed 's/^/  shot| /' "$OUT/shot.log" >&2
        return 2
    fi
    LAST_PNG=$_png
    "$OUT/capture" "$_png" "$4" "$5" "$1" "$2" "$3" ${6:+"$6" "$7"} \
        >"$OUT/capture.out" 2>&1
}
recapture() {   # $1 r $2 g $3 b $4 w $5 h [$6 x $7 y] -- same picture, new question
    [ -n "${LAST_PNG:-}" ] || return 2
    "$OUT/capture" "$LAST_PNG" "$4" "$5" "$1" "$2" "$3" ${6:+"$6" "$7"} \
        >"$OUT/capture.out" 2>&1
}

# Where the compositor put this window.  A Wayland client cannot place its own
# top-levels, and this weston places one when it is mapped and never moves it
# again -- so the origin is READ OUT of a windowed phase's own photograph and
# handed to the fullscreen one, rather than assumed to be anything.
ORIGIN_X=
ORIGIN_Y=
remember_origin() {
    _o=$(sed -n 's/^capture: origin=//p' "$OUT/capture.out" | head -1)
    [ -n "$_o" ] || return 1
    ORIGIN_X=${_o%,*}
    ORIGIN_Y=${_o#*,}
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
    remember_origin
    release_phase "$1"
    return 0
}

# Drive the main probe through all four phases with the photographs, into the
# caller's `fail`.  Used by the positive run and, under --sabotage, by the
# lever run that must NOT pass -- so the same code makes both judgements and a
# check that quietly stopped running could not pass one and fail the other.
run_main_leg() {    # $1.. = extra env for the probe
    start_probe fs "$@"

    phase_capture 1 "$P1_W" "$P1_H" "windowed, the size the swapchain was created with"
    phase_capture 2 "$P2_W" "$P2_H" "resized by SetWindowPos + ResizeBuffers"

    # Phase 3 is only a size claim if the transition actually happened.  The
    # probe says which, and the gate believes it rather than assuming -- a
    # compositor is entitled to refuse fullscreen.
    if wait_phase 3; then
        if grep -q "fullscreen_smoke: PHASE3 REFUSED" "$OUT/fs.out"; then
            bad "phase 3: the transition was refused.  SetFullscreenState and \
GetFullscreenState agreed that it did not happen, which is coherent -- but this \
gate exists to prove that it DOES happen, and a compositor that cannot go \
fullscreen cannot answer that question either way."
            release_phase 3
        else
            # ONE PHOTOGRAPH, TWO QUESTIONS, so the two answers cannot come
            # from different frames: the screen's size must be there, and the
            # windowed size must NOT -- because "still 192x144" is exactly what
            # this used to be, and a check that only asked the first question
            # would pass a frame that was somehow both.
            #
            # The screen-sized question is asked AT THE ORIGIN phase 2 measured,
            # because a screen-sized rectangle that does not start at (0,0) runs
            # off the screen and only part of it can be photographed.  That is
            # not this port's doing and not something it can change: a Wayland
            # client may not place its own top-levels, and this compositor put
            # the window where it did when it was mapped and has not moved it
            # since -- the same origin answered phases 1, 2 and 4.  The check is
            # still entirely on values: the box must be exactly the on-screen
            # part of a SCREEN_W x SCREEN_H rectangle there, completely filled,
            # which a windowed frame cannot be and a scaled one cannot fill.
            capture "$WANT_R" "$WANT_G" "$WANT_B" "$SCREEN_W" "$SCREEN_H" \
                    "${ORIGIN_X:-0}" "${ORIGIN_Y:-0}"
            full_rc=$?
            # Echoed between the two questions, not after both: they write the
            # same file, and a gate that printed only the second one would hide
            # the numbers the fullscreen claim actually rests on.
            sed 's/^/  fullscreen| /' "$OUT/capture.out" 2>/dev/null
            recapture "$WANT_R" "$WANT_G" "$WANT_B" "$P2_W" "$P2_H"
            same_rc=$?
            sed 's/^/  windowed?| /' "$OUT/capture.out" 2>/dev/null
            if [ $full_rc -eq 0 ] && [ $same_rc -ne 0 ]; then
                say "phase 3: the frame fills the whole ${SCREEN_W}x${SCREEN_H} \
screen and is NOT the windowed ${P2_W}x${P2_H} -- exclusive fullscreen reached \
the display server"
            elif [ $same_rc -eq 0 ]; then
                bad "phase 3: the rectangle on screen is still the windowed \
${P2_W}x${P2_H}.  SetFullscreenState was an accounting change inside DXGI and \
nothing asked Wine to move the window -- which is what this lane did before \
dxvk-patches/0004 and abi 2 of ppc64le/dxvk/dxvk_win32u_wsi.h."
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

    phase_capture 4 "$P2_W" "$P2_H" "back to windowed after leaving fullscreen \
-- restored by DXGI, not by the probe"

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
}

# The mode-switch leg.  No photograph, deliberately -- see the banner.
run_mode_leg() {    # $1.. = extra env for the probe
    start_probe fsmode "$@"
    # Nothing to photograph, so every phase is released as it arrives.
    for _p in 1 2 3 4; do wait_phase $_p && release_phase $_p; done
    release_all
    wait_probe
    sed 's/^/  mode| /' "$OUT/fsmode.out"
}

# ---- (also standalone as --sabotage): the negative controls -----------------
sabotage() {
    ok=1
    fail=0

    # 1: THE LEVER ON THE FIX.  Wine publishes the callback table with its six
    #    abi-2 entries NULL, DXVK falls back to the no-ops it used to inherit,
    #    and the whole main leg must go red.
    EXPECT_RED=1
    run_main_leg WINEDXVKNOWINDOWOPS=1
    EXPECT_RED=0
    if [ "$fail" = 0 ]; then
        bad "WINEDXVKNOWINDOWOPS=1 still PASSED the main leg -- the fullscreen \
checks are not resting on the window operations they claim to"
        ok=0
    else
        say "sabotage: WINEDXVKNOWINDOWOPS=1 (the abi-2 window operations \
withheld) took the main leg red, as it must: $(grep -m1 'phase 3:' \
"$OUT/fs.out" | cut -c1-140)"
    fi
    fail=0

    # 2: the same lever against the MODE leg, which must fail on the display
    #    mode rather than on the rectangle.
    run_mode_leg WINEDXVKNOWINDOWOPS=1
    if grep -q "fullscreen_smoke: PASS" "$OUT/fsmode.out"; then
        bad "WINEDXVKNOWINDOWOPS=1 still PASSED the mode leg -- the display-mode \
check is not resting on the new callbacks"
        ok=0
    elif grep -q "the fullscreen transition did not change it" "$OUT/fsmode.out" ||
         grep -q "ChangeDisplaySettingsExW did not change the screen" "$OUT/fsmode.out" ||
         grep -q "did not restore the window rectangle" "$OUT/fsmode.out"; then
        # Quote the DISPLAY-MODE step, not the first failure in the file: the
        # rectangle check fails first under this lever and would otherwise be
        # the line printed, which would read as the rectangle control again
        # rather than as the separate claim this leg is here for.
        say "sabotage: WINEDXVKNOWINDOWOPS=1 took the mode leg red on the mode \
itself: $(grep -m1 'the display mode followed the transition' "$OUT/fsmode.out" \
| cut -c1-160)"
    else
        bad "the mode leg failed under WINEDXVKNOWINDOWOPS=1, but not on a \
display-mode check; the control is not falsifying what it claims to"
        ok=0
    fi
    fail=0

    # 3: the half-done resize.  The rectangle on screen is the RIGHT size --
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

    # 4: claim fullscreen without asking for it.
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

    # 5: the size comparison itself.  Photograph phase 1 and ask for phase 2's
    #    size; it must be refused.  Without this, every size check above could
    #    be a capture that says yes to anything.
    start_probe fs
    if wait_phase 1; then
        capture "$WANT_R" "$WANT_G" "$WANT_B" "$P1_W" "$P1_H"; _a=$?
        recapture "$WANT_R" "$WANT_G" "$WANT_B" "$P2_W" "$P2_H"; _b=$?
        release_all
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
run_main_leg

# Cross-check the size the probe SAW against the compositor this gate started.
# The photographs above are all compared against SCREEN_W/SCREEN_H, so a Wine
# that reported a different screen than the one weston is running would make
# every one of those comparisons a comparison against the wrong number.
_seen=$(sed -n 's/^fullscreen_smoke: SCREEN //p' "$OUT/fs.out" | head -1)
if [ "$_seen" = "${SCREEN_W}x${SCREEN_H}" ]; then
    say "screen: the guest and this gate agree the display is $_seen"
else
    bad "the guest sees a ${_seen:-missing} screen and this gate started a \
${SCREEN_W}x${SCREEN_H} compositor; every size comparison above was made \
against the wrong number"
fi

# ---- M: the display-mode leg ------------------------------------------------
run_mode_leg
_mode=$(grep -m1 "EnumDisplaySettingsW" "$OUT/fsmode.out")
if [ -z "$_mode" ]; then
    bad "the mode probe printed no display-mode line at all"
elif grep -q "fullscreen_smoke: PASS" "$OUT/fsmode.out"; then
    say "mode: $(echo "$_mode" | sed 's/^step [0-9]* //' | cut -c1-160)"
    say "mode: $(grep -m1 'phase 3: the display mode followed' "$OUT/fsmode.out" \
| cut -c1-160)"
    say "mode: $(tail -2 "$OUT/fsmode.out" | head -1)"
    case "$_mode" in
        *"asked=none(one-mode-display)"*)
            note "this display has exactly one mode, so no mode change was \
requested and ChangeDisplaySettingsExW is UNPROVEN here.  That is what a \
640x480 compositor gets (win32u's synthetic mode list starts at 640x480), and \
it is why this gate runs a 1024x768 one." ;;
        *) say "mode: the display mode changed on the way into fullscreen and \
came back on the way out, both through the WSI callbacks" ;;
    esac
else
    tail -20 "$OUT/fsmode.err" >&2
    bad "the mode-switch probe did not pass its own checks"
fi

# The display must be exactly where it started, whatever happened above.  This
# gate is the one that changes display modes, so it is the one that has to say
# out loud that it put the display back.
_final=$(sed -n 's/^fullscreen_smoke: FINAL screen=//p' "$OUT/fsmode.out" | head -1)
if [ "$_final" = "${SCREEN_W}x${SCREEN_H}" ]; then
    say "mode: the display is back at ${SCREEN_W}x${SCREEN_H} after the run"
else
    bad "the display was left at ${_final:-an unknown size} rather than \
${SCREEN_W}x${SCREEN_H}; a gate that changes a display mode must restore it"
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
