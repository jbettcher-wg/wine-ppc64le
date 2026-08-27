#!/bin/sh
#
# check-launch-smoke.sh -- the compat tool's LAUNCH PATH gate.
#
# WHAT IS BEING TESTED.  Everything else in ppc64le/*/check-*.sh tests a
# mechanism inside Wine.  This tests the thing Steam actually runs:
# ppc64le/steamtool/proton, the shell script that escapes the outer emulator,
# sanitises Steam's environment, chooses a prefix, starts and stops the Steam
# bridge helper, and finally execs a guest.  That path had no gate at all,
# which is exactly how task #12 got in.
#
# WHAT TASK #12 WAS.  After any change to a compatibility tool Steam inserts
# its legacycompat pre-step: iscriptevaluator.exe, run through the tool with
# the `run` verb, with the whole launch blocked behind its exit code.  It
# calls ShellExecuteExW on legacycompat\SteamService.exe -- a 32-bit i386 PE.
# This port has no 32-bit guest, so CreateProcess answers
# ERROR_BAD_EXE_FORMAT (193), shell32's SHELL_execute raises its error box
# because the caller did not pass SEE_MASK_FLAG_NO_UI, and the process then
# sits in that dialog's own message loop for ever:
#
#   NtWaitForMultipleObjects <- win32u wait_message <- the modal dialog loop
#   <- user32 MessageBoxW <- shell32 do_error_dialog <- SHELL_execute
#   <- ShellExecuteExW <- the guest's thunk
#
# With nobody to press OK that is a permanent wait -- rc=124 under a timeout,
# and rc=1 plus a mystifying "Invalid handle." dialog when a human happens to
# be at the machine and clicks it.  (The "Invalid handle." was a second bug:
# do_error_dialog described GetLastError(), which by then had been overwritten
# by SHELL_ExecuteW's own CloseHandle(NULL).)
#
# Six layers, each of which removes one way of passing by accident:
#
#   1  PREFIX SELECTION, three shapes, using destroyprefix so that no prefix
#      has to be created to check the choice: an empty compatdata gets pfx; a
#      compatdata holding somebody else's prefix is left alone and gets
#      pfx-ppc64le-native beside it; and only a prefix carrying this tool's
#      own marker is ever destroyed.  Values, not "it ran": the log's
#      WINEPREFIX line, and whether the directory is still on disk after.
#   2  FIRST RUN.  A compatdata with no prefix must get one -- wineboot --init
#      through the tool, the marker written, and the SECOND run must not do it
#      again.  This is also what makes the rest of the gate hermetic: every
#      later layer runs against a prefix this gate made.
#   3  THE PRE-STEP THAT USED TO HANG.  The guest probe makes the evaluator's
#      exact ShellExecuteExW call -- same fMask, same verb, same nShow -- at a
#      32-bit PE, under `run`.  It must come back inside the bound, and the
#      values checked are the ones that were wrong: ShellExecuteExW returns
#      FALSE, hInstApp is 11 (ERROR_BAD_FORMAT), the box was ANSWERED rather
#      than shown, and shell32's own line names last error 193 -- the real
#      reason -- rather than the 6 that used to reach the dialog.
#  3b  THE EXE SWAP IS THE GAME'S.  WINE_PPC64LE_RUN_EXE replaces the binary
#      Steam's launch config picked, so it must not be applied to a pre-step,
#      whose exe comes from somewhere else -- and it must still be refused
#      loudly on the game verb when it names a file that is not there.
#   4  THE GAME KEEPS ITS DIALOGS.  Steam sends the game as
#      waitforexitandrun and its background pre-steps as run, so the
#      suppression is scoped to run.  A gate that only proved dialogs go away
#      would be happy with a tool that silenced the game too.
#   5  TEARDOWN.  The tool owns the Steam bridge helper for the length of a
#      run and stops it on the way out; a leaked helper holds a connection to
#      the user's Steam client indefinitely (measured: one found still running
#      hours later).  The pid and port are read out of the run's own log and
#      checked to be gone AFTER the tool has exited.
#   6  NEGATIVE CONTROL, and the whole of --sabotage.  The same layer-3 run
#      with WINE_PPC64LE_NO_DIALOGS=0, which the tool passes through because
#      it only sets the variable when the caller has not.  Without the
#      mechanism the run MUST hang and be killed by the timeout.  A gate that
#      cannot go red proves nothing, and here going red is the original bug.
#
# The hang is caught in bounded time on purpose: every run of the tool in this
# script is under `timeout`, and layer 3 fails on rc=124 exactly as loudly as
# it fails on a wrong value.
#
# Exit 0 = pass, 1 = a check failed, 2 = could not run (not a pass).
set -u

HERE=$(cd "$(dirname "$0")" && pwd)
SRC=$(cd "$HERE/../.." && pwd)
BUILD=${BUILD:-$SRC}
OUT=${OUT:-/tmp/launch-smoke}
TOOL=$HERE/proton

# Layer 2 creates a prefix through the tool, which is the slowest thing here.
INIT_TIMEOUT=${INIT_TIMEOUT:-300}
# Layer 3's bound.  The run it replaces never finished at all, so anything
# short enough to be a gate and long enough for a guest start is right.
RUN_TIMEOUT=${RUN_TIMEOUT:-90}
# Layer 6 waits for the hang.  Shorter, because a pass here is a timeout.
HANG_TIMEOUT=${HANG_TIMEOUT:-40}

SABOTAGE=0
[ "${1:-}" = "--sabotage" ] && SABOTAGE=1

say()  { echo "check-launch-smoke: $*"; }
bad()  { echo "check-launch-smoke: FAIL $*" >&2; fail=1; }
skip() { echo "check-launch-smoke: $*" >&2; exit 2; }

fail=0

[ -x "$TOOL" ]        || skip "no compat tool at $TOOL"
[ -x "$BUILD/wine" ]  || skip "no wine loader at $BUILD/wine"
[ -n "${WINEFEXBRIDGE:-}" ] || skip "set WINEFEXBRIDGE to the emulator bridge"
[ -r "$WINEFEXBRIDGE" ]     || skip "WINEFEXBRIDGE=$WINEFEXBRIDGE is not readable"
command -v clang >/dev/null || skip "need clang for the guest probe"
command -v ss    >/dev/null || skip "need ss (iproute2) for the teardown layer"
[ -f "$BUILD/dlls/shell32/x86_64-windows/shell32.dll" ] || \
    skip "no guest shell32 thunk; build it first"

# A DISPLAY IS A PREREQUISITE, and not for the reason it usually is.
#
# Measured: with no display driver at all, MessageBox never gets a window --
# nodrv_CreateWindow refuses, the call returns 0, and the pre-step exits
# rc=1 in a couple of seconds.  So a truly headless box never had this bug,
# and the negative control in layer 6 would "pass" there by proving nothing.
# Anything that accepts a window will do, including the Xvfb this box runs on
# :0; no GPU and no compositor are involved, only a window manager-less X
# server that lets a dialog exist.
#
# THE DISPLAY IS THIS GATE'S OWN, AND THAT IS NOT A COURTESY.  The whole point
# of layer 6 is to raise a real modal dialog; pointed at a session somebody is
# using, that dialog lands on THEIR screen, and it outlives the run because the
# tool does not block on it.  [MEASURED] 2026-08-18: running this suite with
# DISPLAY=:1 put "Bad format." boxes onto the user's desktop, repeatedly, with
# no indication of where they came from.  A gate that raises dialogs has to
# own the screen it raises them on -- the same rule check-fullscreen-smoke.sh
# follows with its private weston.
# $OUT must exist BEFORE the Xvfb block below writes its log into it.  This
# used to sit after the block and worked only when /tmp/launch-smoke survived
# from an earlier run; the first run after a reboot skipped on "could not
# start a private Xvfb" whose real cause was the redirect failing [MEASURED
# 2026-08-27].
mkdir -p "$OUT" || skip "cannot create $OUT"

XVFB_PID=
if [ -z "${LAUNCH_SMOKE_USE_CALLER_DISPLAY:-}" ]; then
    command -v Xvfb >/dev/null || \
        skip "need Xvfb so this gate's dialogs land on its own display, not \
somebody's session; set LAUNCH_SMOKE_USE_CALLER_DISPLAY=1 to override"
    for n in 71 72 73 74 75; do
        [ -e "/tmp/.X11-unix/X$n" ] && continue
        Xvfb ":$n" -screen 0 1024x768x24 >"$OUT/xvfb.log" 2>&1 &
        XVFB_PID=$!
        for _ in 1 2 3 4 5 6 7 8 9 10; do
            [ -e "/tmp/.X11-unix/X$n" ] && break
            sleep 0.3
        done
        if [ -e "/tmp/.X11-unix/X$n" ]; then
            DISPLAY=":$n"; export DISPLAY
            unset WAYLAND_DISPLAY
            say "display: private Xvfb on :$n (pid $XVFB_PID)"
            break
        fi
        kill "$XVFB_PID" 2>/dev/null; XVFB_PID=
    done
    [ -n "$XVFB_PID" ] || skip "could not start a private Xvfb"
    trap 'kill "$XVFB_PID" 2>/dev/null' EXIT INT TERM HUP
fi

[ -n "${DISPLAY:-}${WAYLAND_DISPLAY:-}" ] || \
    skip "set DISPLAY (or WAYLAND_DISPLAY): with no driver a message box cannot be created, and the negative control would prove nothing"

# ---------------------------------------------------------------------------
# The guest probe, and the REFUSED image it is pointed at.
#
# The image is the probe itself with one 16-bit field rewritten: the PE machine
# word at e_lfanew+4.  That makes a real machine-marked PE rather than a mock of
# one, so the refusal it draws is the loader's own and the gate needs no foreign
# toolchain to produce it.
#
# THE MACHINE IS ARMNT (0x01c4), AND IT USED TO BE I386 (0x014c).  I386 stopped
# working as a negative control the day this port grew a 32-bit lane: the server
# now advertises IMAGE_FILE_MACHINE_I386, so an i386-marked PE is ACCEPTED, no
# ERROR_BAD_EXE_FORMAT is raised, no dialog appears, and this control silently
# proved nothing.  [MEASURED] 2026-08-18: layer 6 exited 0 in 11s instead of
# hanging, on a build where every other layer passed.
#
# ARMNT is durable rather than merely different.  server/registry.c lists the
# machines this prefix will accept and says why ARM is not among them -- "there
# is no ARM emulator in this stack, and listing a machine is a promise the
# prefix will run it".  A control built on a machine that is refused BY DESIGN
# survives the port growing new architectures; one built on a machine that
# merely has not been implemented yet does not.
# ---------------------------------------------------------------------------
INCL="-I$BUILD/include -I$SRC/include -I$SRC/include/msvcrt"
clang -target x86_64-windows-gnu -nostdlibinc $INCL -D_MSVCR_VER=0 \
    -Wall -O1 -fno-builtin -g -c -o "$OUT/launch_probe.o" "$HERE/launch_probe.c" \
    || skip "guest probe compile failed"
clang -target x86_64-windows-gnu -fuse-ld=lld -nostdlib \
    -Wl,--entry=launch_probe_entry -Wl,--subsystem,console \
    -o "$OUT/launch_probe.exe" "$OUT/launch_probe.o" \
    "$BUILD/dlls/kernel32/x86_64-windows/kernel32.dll" \
    "$BUILD/dlls/shell32/x86_64-windows/shell32.dll" \
    || skip "guest probe link failed"

cp -f "$OUT/launch_probe.exe" "$OUT/fake32.exe" || skip "cannot write $OUT/fake32.exe"
lfanew=$(od -An -tu4 -j60 -N4 "$OUT/fake32.exe" | tr -d ' ')
printf '\304\001' | dd of="$OUT/fake32.exe" bs=1 seek=$((lfanew + 4)) \
    conv=notrunc status=none || skip "cannot patch the machine word"
machine=$(od -An -tx2 -j$((lfanew + 4)) -N2 "$OUT/fake32.exe" | tr -d ' ')
[ "$machine" = "01c4" ] || skip "machine word is $machine, expected 01c4"
say "guest probe built; $OUT/fake32.exe is machine 0x$machine (ARMNT, refused by design)"

# Z: is / in every prefix, so the DOS path can be computed without a wine run.
FAKE32_DOS="Z:$(printf '%s' "$OUT/fake32.exe" | tr '/' '\\')"

# ---------------------------------------------------------------------------
# Driving the tool.
#
# Nothing here strips the caller's environment: the tool's own sanitisation is
# part of what is being tested, and it is what removes WINEPREFIX and friends.
# TOOL_ENV carries the per-layer variables and is deliberately unquoted so it
# word-splits -- every value this file puts in it is a single token.
# ---------------------------------------------------------------------------
TOOL_ENV=
TOOL_LOG=
RC=

# Sets RC and TOOL_LOG rather than printing the exit code: a command
# substitution would run this in a subshell, and TOOL_LOG -- which every
# value check below reads -- would never reach the caller.  (It did not, the
# first time this gate was run, and every layer then "failed" against an
# empty filename instead of against the tool.)
run_tool() {   # $1 = verb, $2 = compatdata, $3 = timeout, rest = argv
    _verb=$1; _compat=$2; _tmo=$3
    shift 3
    mkdir -p "$_compat"
    rm -f "$_compat"/wine-ppc64le-native-*.log
    timeout -k 5 "$_tmo" env \
        STEAM_COMPAT_DATA_PATH="$_compat" \
        WINE_PPC64LE_TREE="$BUILD" \
        WINE_PPC64LE_FEXBRIDGE="$WINEFEXBRIDGE" \
        $TOOL_ENV \
        "$TOOL" "$_verb" "$@" >"$OUT/tool.out" 2>"$OUT/tool.err"
    RC=$?
    TOOL_LOG=$(sed -n 's/^wine-ppc64le-native: log -> //p' "$OUT/tool.err" | head -1)
    [ -n "$TOOL_LOG" ] || bad "the tool printed no log path; see $OUT/tool.err"
}

want_log() {   # $1 = fixed string the run's log must contain
    if grep -qF "$1" "$TOOL_LOG" 2>/dev/null; then
        say "  ok: $1"
    else
        bad "expected \"$1\" in $TOOL_LOG"
    fi
}

want_no_log() {   # $1 = fixed string the run's log must NOT contain
    if grep -qF "$1" "$TOOL_LOG" 2>/dev/null; then
        bad "did not expect \"$1\" in $TOOL_LOG"
        grep -nF "$1" "$TOOL_LOG" | sed 's/^/      got: /' >&2
    else
        say "  ok: no \"$1\""
    fi
}

# Kill by exact pid, never by pattern: the pattern is only used to FIND the
# pids, and the path it matches is this gate's own $OUT.  A run killed by the
# timeout orphans its guest, and a guest left holding a modal dialog would
# still be there when the next layer runs.
reap_probe() {
    for _p in $(pgrep -u "$(id -u)" -f "$OUT/launch_probe.exe" 2>/dev/null); do
        say "  reaping orphaned probe pid $_p"
        kill "$_p" 2>/dev/null
    done
}

# ---------------------------------------------------------------------------
# LAYER 6 (negative control) -- also the whole of --sabotage.
#
# Run first when sabotaging so nothing else has to happen; run last otherwise,
# because it deliberately leaves a killed process behind.
# ---------------------------------------------------------------------------
sabotage_layer() {
    say "layer 6: negative control -- WINE_PPC64LE_NO_DIALOGS=0, the dialog must hang the run"
    TOOL_ENV="WINE_PPC64LE_NO_DIALOGS=0 LAUNCH_PROBE_FILE=$FAKE32_DOS"
    t0=$(date +%s)
    run_tool run "$SABOTAGE_COMPAT" "$HANG_TIMEOUT" "$OUT/launch_probe.exe"
rc=$RC
    t1=$(date +%s)
    TOOL_ENV=
    say "  tool exit $rc after $((t1 - t0))s"
    # RED IS "THE DIALOG WAS RAISED", not "the run hung", and the difference is
    # measured rather than assumed.  Whether an unanswered box HANGS depends on
    # what the caller does with it: the pre-step that wedged a real Steam launch
    # waited on it, but this probe's ShellExecuteExW returns and the tool walks
    # on.  So the control asks the only question that is always meaningful --
    # did the port put a message box up at all -- and shell32's own
    # do_error_dialog line is the evidence.  A hang is still accepted, because
    # on a caller's display with something to block on it is what happens.
    if [ "$rc" = 124 ] || [ "$rc" = 137 ]; then
        say "  ok: without the suppression the pre-step never returned -- the gate goes red"
        want_no_log "probe: done"
    elif grep -qF "do_error_dialog" "$TOOL_LOG" 2>/dev/null; then
        say "  ok: without the suppression the port raised the dialog -- $(grep -m1 -oF 'Bad format' "$TOOL_LOG")"
    elif grep -qF "nodrv_CreateWindow" "$TOOL_LOG" 2>/dev/null; then
        # Not a red and not a green: the dialog was never created, so nothing
        # about the suppression was exercised.  Say so rather than let a
        # display that rejects windows read as either answer.
        bad "no window could be created on \${DISPLAY:-\$WAYLAND_DISPLAY} (nodrv_CreateWindow)," \
            "so this leg tested nothing; point DISPLAY at a session that accepts windows"
    else
        bad "with WINE_PPC64LE_NO_DIALOGS=0 the run exited $rc instead of hanging;" \
            "either the tool no longer passes the caller's value through, or the" \
            "dialog is no longer raised at all -- this gate can then never go red"
    fi
    reap_probe
}

if [ "$SABOTAGE" = 1 ]; then
    SABOTAGE_COMPAT=$OUT/compat
    [ -e "$SABOTAGE_COMPAT/pfx/.wine-ppc64le-native" ] || \
        skip "no prefix at $SABOTAGE_COMPAT/pfx; run this gate without --sabotage first"
    sabotage_layer
    [ "$fail" = 0 ] && say "sabotage PASS" || say "sabotage FAIL"
    exit "$fail"
fi

# ---------------------------------------------------------------------------
# LAYER 1 -- prefix selection.  destroyprefix returns before any prefix is
# created, so all three shapes are checked without a wineboot.
# ---------------------------------------------------------------------------
say "layer 1: prefix selection"

C1=$OUT/pfxsel-fresh
rm -rf "$C1"
run_tool destroyprefix "$C1" 60
rc=$RC
[ "$rc" = 0 ] || bad "destroyprefix on an empty compatdata exited $rc"
want_log "WINEPREFIX=$C1/pfx"
want_log "refusing to destroy $C1/pfx: not created by this tool"
[ -e "$C1/pfx" ] && bad "$C1/pfx was created by a verb that must not create one"

C2=$OUT/pfxsel-foreign
rm -rf "$C2"
mkdir -p "$C2/pfx/drive_c"
: > "$C2/pfx/system.reg"
run_tool destroyprefix "$C2" 60
rc=$RC
[ "$rc" = 0 ] || bad "destroyprefix on a foreign prefix exited $rc"
want_log "existing foreign prefix at $C2/pfx left alone; using $C2/pfx-ppc64le-native"
want_log "WINEPREFIX=$C2/pfx-ppc64le-native"
want_log "refusing to destroy $C2/pfx-ppc64le-native: not created by this tool"
[ -f "$C2/pfx/system.reg" ] || bad "the foreign prefix at $C2/pfx was not left alone"

C3=$OUT/pfxsel-ours
rm -rf "$C3"
mkdir -p "$C3/pfx/drive_c"
: > "$C3/pfx/.wine-ppc64le-native"
: > "$C3/pfx/system.reg"
run_tool destroyprefix "$C3" 60
rc=$RC
[ "$rc" = 0 ] || bad "destroyprefix on our own prefix exited $rc"
want_log "WINEPREFIX=$C3/pfx"
want_log "destroying $C3/pfx"
[ -e "$C3/pfx" ] && bad "$C3/pfx survived a destroyprefix of a prefix this tool made"

# ---------------------------------------------------------------------------
# LAYER 2 -- first run initialises a prefix, and only the first run does.
#
# LAUNCH_SMOKE_KEEP_PREFIX=1 keeps a prefix an earlier run of this gate made,
# for iterating; the default is a fresh one so the first-run path is actually
# exercised rather than assumed.
# ---------------------------------------------------------------------------
COMPAT=$OUT/compat
if [ "${LAUNCH_SMOKE_KEEP_PREFIX:-0}" != 1 ]; then
    rm -rf "$COMPAT"
fi

say "layer 2: first run initialises the prefix"
if [ -e "$COMPAT/pfx/.wine-ppc64le-native" ]; then
    say "  kept an existing prefix at $COMPAT/pfx (LAUNCH_SMOKE_KEEP_PREFIX=1)"
else
    t0=$(date +%s)
    run_tool run "$COMPAT" "$INIT_TIMEOUT" "$OUT/launch_probe.exe"
rc=$RC
    t1=$(date +%s)
    say "  tool exit $rc after $((t1 - t0))s"
    [ "$rc" = 0 ] || bad "the first run exited $rc"
    want_log "first run: wineboot --init in $COMPAT/pfx"
    want_log "prefix initialised"
    [ -e "$COMPAT/pfx/.wine-ppc64le-native" ] || \
        bad "no $COMPAT/pfx/.wine-ppc64le-native marker after the first run"
    want_log "probe: no LAUNCH_PROBE_FILE, nothing executed"
    want_log "probe: done"
fi

# ---------------------------------------------------------------------------
# LAYER 3 -- the pre-step that used to hang.
# ---------------------------------------------------------------------------
say "layer 3: the evaluator-shaped pre-step returns instead of hanging"
TOOL_ENV="LAUNCH_PROBE_FILE=$FAKE32_DOS"
t0=$(date +%s)
run_tool run "$COMPAT" "$RUN_TIMEOUT" "$OUT/launch_probe.exe"
rc=$RC
t1=$(date +%s)
TOOL_ENV=
say "  tool exit $rc after $((t1 - t0))s"
if [ "$rc" = 124 ] || [ "$rc" = 137 ]; then
    bad "the pre-step did not return within ${RUN_TIMEOUT}s -- this IS task #12"
    reap_probe
elif [ "$rc" != 0 ]; then
    bad "the pre-step exited $rc, expected 0"
fi
# The second run of this prefix: it must not initialise anything again.
want_no_log "first run: wineboot --init"
want_log "background pre-step: message boxes answered, not shown (WINE_PPC64LE_NO_DIALOGS=1)"
# The guest's own answers.  ShellExecuteExW fails, and hInstApp is
# ERROR_BAD_FORMAT (11) -- what SHELL_ExecuteW turns a CreateProcess refusal
# into, and what a caller reads to find out.
want_log "probe: ShellExecuteExW returned 0"
want_log "probe: hInstApp=11"
want_log "probe: done"
# The box was answered rather than shown, and the answer is on the record.
want_log "unattended: message box"
want_log "answered 1"
# THE SYMPTOM-1 CHECK.  The reason the dialog gave used to be whatever
# GetLastError() happened to hold -- 6, ERROR_INVALID_HANDLE, left there by
# SHELL_ExecuteW's own CloseHandle(NULL).  It must now be the real one:
# ERROR_BAD_EXE_FORMAT (193), because the image is i386.  Checked as numbers
# rather than prose, since FormatMessage answers in the machine's language.
want_log "ShellExecute returned 11, described 11, last error 193"
want_no_log "last error 6)"

# ---------------------------------------------------------------------------
# LAYER 3b -- the game's exe swap belongs to the game.
#
# WINE_PPC64LE_RUN_EXE names a replacement for the binary Steam's launch
# config picked, and its bare-name form resolves beside whatever this tool was
# handed.  A pre-step's exe comes from Steam's legacycompat directory, so
# applying the swap there looks for the game's binary in the wrong place,
# does not find it, and takes the refuse-loudly branch -- killing the pre-step
# before any guest starts.  Both halves are checked here, because a swap that
# quietly did nothing would be just as wrong: it would launch the binary this
# port cannot serve and report that binary's failure.
# ---------------------------------------------------------------------------
say "layer 3b: the exe swap applies to the game verb only"
TOOL_ENV="WINE_PPC64LE_RUN_EXE=no-such-binary.exe"
run_tool run "$COMPAT" "$RUN_TIMEOUT" "$OUT/launch_probe.exe"
rc=$RC
say "  run exit $rc"
[ "$rc" = 0 ] || bad "the pre-step exited $rc with WINE_PPC64LE_RUN_EXE set; it must ignore it"
want_no_log "FATAL: WINE_PPC64LE_RUN_EXE"
want_log "probe: done"

run_tool waitforexitandrun "$COMPAT" "$RUN_TIMEOUT" "$OUT/launch_probe.exe"
rc=$RC
say "  waitforexitandrun exit $rc"
[ "$rc" = 1 ] || bad "the game verb exited $rc; a swap naming a missing file must still be refused"
want_log "FATAL: WINE_PPC64LE_RUN_EXE names $OUT/no-such-binary.exe, which does not exist"
TOOL_ENV=

# ---------------------------------------------------------------------------
# LAYER 4 -- the game keeps its dialogs.
# ---------------------------------------------------------------------------
say "layer 4: the game verb is not silenced"
run_tool waitforexitandrun "$COMPAT" "$RUN_TIMEOUT" "$OUT/launch_probe.exe"
rc=$RC
say "  tool exit $rc"
[ "$rc" = 0 ] || bad "waitforexitandrun exited $rc"
want_log "probe: no LAUNCH_PROBE_FILE, nothing executed"
want_no_log "background pre-step: message boxes answered"
want_no_log "WINE_PPC64LE_NO_DIALOGS"
# The debugger suppression is NOT scoped to run, and must still be in force
# here -- a crash on the game verb must not raise winedbg's window either.
want_log "winedbg suppressed"
# waitforexitandrun is the verb that waits for the server; run does not.
want_log "wineserver gone"

# ---------------------------------------------------------------------------
# LAYER 5 -- the bridge helper is stopped, checked after the tool has exited.
# ---------------------------------------------------------------------------
say "layer 5: the steam bridge helper is stopped on the way out"
hp=$(sed -n 's/.*steam bridge helper on 127\.0\.0\.1:\([0-9][0-9]*\) (pid \([0-9][0-9]*\).*/\1 \2/p' \
         "$TOOL_LOG" | head -1)
hport=${hp% *}
hpid=${hp#* }
if [ -z "$hp" ]; then
    if grep -qF "continuing without a steam bridge" "$TOOL_LOG"; then
        bad "the tool could not start the steam bridge helper, so teardown is untested;" \
            "build ppc64le/steamapi/helper/steamhelper and run again"
    else
        bad "no 'steam bridge helper on ...' line in $TOOL_LOG"
    fi
else
    say "  helper was pid $hpid on port $hport"
    want_log "stopping the steam bridge helper (pid $hpid)"
    if kill -0 "$hpid" 2>/dev/null; then
        bad "helper pid $hpid is still alive after the tool exited"
        kill "$hpid" 2>/dev/null
    else
        say "  ok: pid $hpid is gone"
    fi
    if ss -Htln "sport = :$hport" 2>/dev/null | grep -q .; then
        bad "something is still listening on 127.0.0.1:$hport"
    else
        say "  ok: nothing is listening on 127.0.0.1:$hport"
    fi
fi

# ---------------------------------------------------------------------------
# LAYER 6 -- run last, because it leaves a killed process behind.
# ---------------------------------------------------------------------------
SABOTAGE_COMPAT=$COMPAT
sabotage_layer

[ "$fail" = 0 ] && say "PASS"
exit "$fail"
