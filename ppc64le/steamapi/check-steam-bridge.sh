#!/bin/sh
#
# check-steam-bridge.sh -- the steam_api -> Steam-client bridge gate.
#
# WHAT IS BEING TESTED.  A Windows game reaches Steam through
# steam_api64.dll, which reads a registry value, loads the steamclient DLL it
# names, calls CreateInterface, and then calls C++ vtables.  On this port that
# DLL is dlls/steamclient64: Proton's lsteamclient PE side compiled as a REAL
# x86-64 Windows DLL (so the game's vtable calls are x86-64 calling x86-64,
# exactly as under Proton), whose one remaining boundary -- Proton's flat
# params-struct "unix call" -- is stretched over a socket to an x86-64 Linux
# helper that owns the real ~/.steam/sdk64/steamclient.so and runs under FEX.
#
# Nine layers, each removing one way of passing by accident:
#
#   1  HELPER: the cross-compiled helper really is x86-64, really runs under
#      FEX, really dlopens the native steamclient.so, and really finds all
#      seven entry points Proton's unix side binds.  dlsym only -- this layer
#      never calls into the library, so it cannot disturb a Steam client.
#   2  MARSHALLER: one synthetic call, __wine_steamrpc_selftest, exercising
#      every parameter class the generated descriptors use -- an in-string, an
#      out buffer with an explicit length, a fixed struct by pointer, a sized
#      caller buffer, an opaque 64-bit handle, and scalars travelling in the
#      params blob -- with values checked on BOTH ends, plus the check that
#      matters most: that the caller's own pointers come back unchanged rather
#      than holding helper addresses.  This layer needs no Steam client, which
#      is the point: a length or direction that is off by one is invisible in
#      a "did it connect" test and catastrophic in a game.
#   3  CHAIN: a guest x86-64 probe standing exactly where steam_api64.dll
#      stands -- LoadLibrary by bare name, GetProcAddress, CreateInterface,
#      and one call through the returned vtable -- against the REAL Steam
#      client library in the helper.
#   4  NO-CLIENT SEMANTICS: with no Steam client running, the whole chain must
#      produce the legible failure a Linux game gets (CreateSteamPipe returns
#      0, "no pipe"), and must RETURN rather than fault.  And with no helper
#      at all -- a game launched outside Steam -- CreateInterface must answer
#      NULL, which is what makes steam_api print its own SteamAPI_Init()
#      failed rather than crash.
#   5  NEGATIVE CONTROL: point the client at a port nothing is listening on.
#      The bridge must fail loudly and promptly, not hang and not succeed.  A
#      gate that cannot go red proves nothing.
#   6  CALLBACKS, with no Steam client.  Steamworks callbacks are pull-based,
#      so both channels can be proved end to end as long as something can put
#      a callback into the far end: the helper is asked to queue one
#      (STEAMRPC_CODE_INJECT), and everything after that is the shipped path
#      -- the wire, the generated descriptors, Proton's own PE-side
#      Steam_BGetCallback and execute_pending_callbacks, and a guest function
#      pointer the helper stored and never called.  Value-checked: the
#      callback id, its size, its payload BYTES, and -- the check this layer
#      mainly exists for -- that the payload pointer inside the message is
#      still the game's own allocation and not the helper's address.  The
#      same export carries SteamAPI_RunCallbacks and the modern
#      SteamAPI_ManualDispatch_GetNextCallback, both of which steam_api64.dll
#      implements on top of Steam_BGetCallback; there is no second entry
#      point to test.
#   7  PATHS, with no Steam client.  One DOS path converted to unix and back
#      through the helper's translation and Proton's own converters, checked
#      against a path this script computes from the prefix itself -- including
#      a wrong-case spelling, because Wine resolves components
#      case-insensitively and a game that spells C:\Users must still find
#      users/.  This is what a cloud save needs: ISteamRemoteStorage and
#      ISteamUGC hand the Steam client DOS paths, and ISteamApps hands the
#      game unix ones back.
#   8  THE RED ZONE.  A marshal descriptor smaller than what the Steam client
#      actually writes is not a wrong answer -- it is a heap overflow inside
#      the helper.  MEASURED: ISteamInput::GetConnectedControllers was
#      described as one 8-byte handle, the client wrote sixteen, and the
#      helper died of `free(): invalid size` at DOOM's title screen, taking
#      the game's Steam connection with it.  The descriptors are fixed (the
#      generator now reads Valve's own STEAM_*_COUNT annotations, and layer 8b
#      checks the four that were wrong), but the CLASS has to stop being
#      fatal, so the helper pads every blob with a red zone.  This asks it to
#      overrun one on purpose and checks the guard catches it, the call is
#      refused by name, and the helper is still answering afterwards.
#   9  HELPER KILLED MID-SESSION.  A Steam client can die under a running game
#      on Windows and games survive it.  The helper is this port's Steam
#      client, so it is killed here while the probe is mid-session, and every
#      later call must fail cleanly and promptly -- never hang, never fault,
#      never take the process down.
#
#   5b LIVE CLIENT (only if one is already running): SteamAppId 480 --
#      Spacewar, the SDK's own test appid, deliberately NOT the game list game's,
#      because using a real appid would flip the account's presence to
#      "playing" it.  A pipe, a user on it, and then the same callback channel
#      as layer 6 with nothing injected: the callbacks are the client's own,
#      which is the one thing a synthetic layer cannot prove.  A client that
#      is RUNNING BUT LOGGED OUT answers the pipe and has no global user, so
#      the drain half is skipped there rather than asserted -- measured: that
#      is the state a client sitting at its login screen is in.  This layer is
#      SKIPPED, never faked, when no client is running, and this script NEVER
#      starts, stops or configures one.
#
# --sabotage runs layer 5 alone.
#
# Exit 0 = pass, 1 = a check failed, 2 = could not run (not a pass).
#
#
# WHY winedbg IS DISABLED ON EVERY RUN.  Same reason check-seh-smoke.sh gives:
# the bringup prefix has AeDebug set to "winedbg --auto", so any unhandled
# fault starts a debugger that attaches and never lets go, turning a red state
# into a hang.  WINEDLLOVERRIDES=winedbg.exe=d makes that CreateProcess fail
# and the process dies promptly instead.  It is an environment override for
# the duration of one run and touches nothing in the prefix.
#
# WDBG follows the tree's append-don't-replace convention: an inherited
# WINEDEBUG wins, and the default only applies when the caller set none.
set -u

HERE=$(cd "$(dirname "$0")" && pwd)
SRC=$(cd "$HERE/../.." && pwd)
BUILD=${BUILD:-$SRC}
OUT=${OUT:-/tmp/steam-bridge}
TIMEOUT=${TIMEOUT:-180}
SABOTAGE=0
[ "${1:-}" = "--sabotage" ] && SABOTAGE=1

say()  { echo "check-steam-bridge: $*"; }
bad()  { echo "check-steam-bridge: FAIL $*" >&2; fail=1; }
skip() { echo "check-steam-bridge: $*" >&2; exit 2; }

fail=0
helper_pid=

cleanup() {
    [ -n "$helper_pid" ] && kill "$helper_pid" 2>/dev/null
    [ -n "$helper_pid" ] && wait "$helper_pid" 2>/dev/null
    helper_pid=
}
trap 'cleanup' EXIT INT TERM

[ -x "$BUILD/wine" ] || skip "no wine loader at $BUILD/wine"
[ -n "${WINEPREFIX:-}" ] || skip "set WINEPREFIX to a prefix wineboot has run in"
[ -n "${WINEFEXBRIDGE:-}" ] || skip "set WINEFEXBRIDGE to the emulator bridge"
command -v clang >/dev/null || skip "need clang for the guest build"

GUEST_DLL=$BUILD/dlls/steamclient64/x86_64-windows/steamclient64.dll
[ -f "$GUEST_DLL" ] || skip "no guest steamclient64.dll at $GUEST_DLL; build it first"

HELPER=$HERE/helper/steamhelper
if [ ! -x "$HELPER" ]; then
    say "building the helper"
    "$HERE/helper/build-helper.sh" >"${OUT:-/tmp}/helper-build.log" 2>&1 || {
        mkdir -p "$OUT"
        "$HERE/helper/build-helper.sh" >"$OUT/helper-build.log" 2>&1 || \
            skip "helper build failed; see $OUT/helper-build.log"
    }
fi
[ -x "$HELPER" ] || skip "no helper at $HELPER"

mkdir -p "$OUT" || skip "cannot create $OUT"

WDBG=${WINEDEBUG:--all}

# ---------------------------------------------------------------------------
# The guest probe.  Built here rather than by "make" for the same reason the
# other gates build theirs here: it is test scaffolding, and its imports are
# named by hand so that what it binds to is visible in this file.
# ---------------------------------------------------------------------------
INCL="-I$BUILD/include -I$SRC/include -I$SRC/include/msvcrt -I$SRC/dlls/steamclient64"
clang -target x86_64-windows-gnu -nostdlibinc $INCL -D_MSVCR_VER=0 \
    -Wall -O1 -fno-builtin -g -c -o "$OUT/probe.o" "$HERE/steam_bridge_probe.c" \
    || skip "guest probe compile failed"
clang -target x86_64-windows-gnu -fuse-ld=lld -nostdlib \
    -Wl,--entry=steam_bridge_probe_entry -Wl,--subsystem,console \
    -o "$OUT/steam_bridge_probe.exe" "$OUT/probe.o" \
    "$BUILD/dlls/kernel32/x86_64-windows/kernel32.dll" \
    || skip "guest probe link failed"

# NOTHING IS STAGED.  The probe loads steamclient64.dll by BARE NAME and the
# loader serves the builtin straight out of the build tree, exactly as it does
# for every other guest module -- which is why the module directory is named
# dlls/steamclient64 and not dlls/lsteamclient: find_builtin_dll() looks under
# dlls/<NAME>/x86_64-windows/<NAME>.dll, so the directory name and the DLL name
# have to be the same string.  Measured: with the DLL at the old path the same
# probe printed "LoadLibraryA(steamclient64.dll) FAILED".
#
# A bare name is also all a game can be given.  This port's guest loader
# refuses a path outright (load_guest_library in dlls/ntdll/signal_ppc64.c:
# "a guest namespace has no files, so a path means nothing"), so the prefix's
# SteamClientDll64 value is a bare name rather than the absolute path Windows
# would have there.

# CB and PATH_IN switch the probe's two optional steps on.  They are passed
# explicitly rather than exported, so that every run states which steps it
# asked for and the default -- both off -- is what the fixed-transcript
# scenarios below compare against.
CB=
PATH_IN=
GUARD=
KILL=

run_probe() {   # $1 = STEAM_BRIDGE_ADDR (may be empty), $2 = stdout file
    ( cd "$OUT" && timeout -k 5 "$TIMEOUT" \
        env WINEDEBUG="$WDBG" WINEDLLOVERRIDES="winedbg.exe=d" \
            STEAM_BRIDGE_ADDR="$1" STEAMBRIDGEDEBUG="${STEAMBRIDGEDEBUG:-err}" \
            STEAM_BRIDGE_CALLBACK="$CB" STEAM_BRIDGE_PATH_IN="$PATH_IN" \
            STEAM_BRIDGE_GUARD="$GUARD" STEAM_BRIDGE_KILL="$KILL" \
            "$BUILD/wine" "$OUT/steam_bridge_probe.exe" \
            >"$2" 2>"${2%.out}.err" )
    echo $?
}

started_helpers=

start_helper() {
    rm -f "$OUT/helper.log"
    "$HELPER" --serve --port 0 -v >"$OUT/helper.log" 2>&1 &
    helper_pid=$!
    started_helpers="$started_helpers $helper_pid"
    n=0
    while [ $n -lt 100 ]; do
        PORT=$(sed -n 's/.*listening on 127\.0\.0\.1:\([0-9][0-9]*\).*/\1/p' \
                   "$OUT/helper.log" 2>/dev/null | head -1)
        [ -n "$PORT" ] && return 0
        kill -0 "$helper_pid" 2>/dev/null || return 1
        n=$((n + 1))
        sleep 0.1
    done
    return 1
}

want_line() {   # $1 = file, $2 = exact expected line
    if grep -qxF "$2" "$1"; then
        say "  ok: $2"
    else
        bad "expected line \"$2\" in $1"
        sed -n '1,20p' "$1" | sed 's/^/      got: /' >&2
    fi
}

# ---------------------------------------------------------------------------
# LAYER 5 (negative control) -- also the whole of --sabotage.
#
# Nothing is listening on this port.  The client must report that it cannot
# reach the helper, CreateInterface must answer NULL, and the probe must exit
# promptly.  A bridge that hangs here would turn every red state of this gate
# into a timeout.
# ---------------------------------------------------------------------------
say "layer 5: negative control (nothing listening)"
DEAD_PORT=1        # port 1 is privileged and unbound: connect() refuses at once
t0=$(date +%s)
rc=$(STEAMBRIDGEDEBUG=err run_probe "127.0.0.1:$DEAD_PORT" "$OUT/dead.out")
t1=$(date +%s)
say "  probe exit $rc after $((t1 - t0))s"
[ "$rc" = 0 ] || bad "the no-helper run should still exit 0 (a legible failure, not a crash); got $rc"
[ $((t1 - t0)) -lt 60 ] || bad "the no-helper run took $((t1 - t0))s; it must fail promptly"
want_line "$OUT/dead.out" "bridge: loaded steamclient64.dll"
# 0x1 is STEAMRPC_SELFTEST_BAD_STATUS on its own: the call could not be sent.
# Any other value would mean the marshaller also disagreed about something,
# which "no helper" must not be able to cause.
want_line "$OUT/dead.out" "bridge: selftest=0x1"
want_line "$OUT/dead.out" "bridge: CreateInterface(SteamClient017)=NULL"
grep -q "cannot reach the Steam bridge helper" "$OUT/dead.err" || \
    bad "the failure was not named on stderr; a silent NULL is the defect this gate exists for"
[ "$SABOTAGE" = 1 ] && { [ "$fail" = 0 ] && say "sabotage PASS" || say "sabotage FAIL"; exit "$fail"; }

# ---------------------------------------------------------------------------
# LAYER 4b -- no helper configured at all, which is what a game launched
# outside Steam sees.  Same requirement: legible, prompt, non-fatal.
# ---------------------------------------------------------------------------
say "layer 4b: no bridge configured (a game launched outside Steam)"
rc=$(run_probe "" "$OUT/nobridge.out")
[ "$rc" = 0 ] || bad "the unconfigured run should exit 0; got $rc"
want_line "$OUT/nobridge.out" "bridge: selftest=0x1"
want_line "$OUT/nobridge.out" "bridge: CreateInterface(SteamClient017)=NULL"

# ---------------------------------------------------------------------------
# LAYER 1 -- the helper itself.
# ---------------------------------------------------------------------------
say "layer 1: the helper under FEX"
"$HELPER" --probe >"$OUT/probe.txt" 2>&1
prc=$?
sed 's/^/      /' "$OUT/probe.txt"
[ "$prc" = 0 ] || bad "helper --probe exited $prc"
want_line "$OUT/probe.txt" "helper: uname machine = x86_64"
want_line "$OUT/probe.txt" "helper: pointer size = 8"
for e in CreateInterface Steam_BGetCallback Steam_GetAPICallResult \
         Steam_FreeLastCallback Steam_ReleaseThreadLocalMemory \
         Steam_IsKnownInterface Steam_NotifyMissingInterface; do
    grep -q "^export $e = 0x" "$OUT/probe.txt" || \
        bad "steamclient.so does not export $e"
done
grep -q "= MISSING" "$OUT/probe.txt" && bad "an export was MISSING"

# ---------------------------------------------------------------------------
# LAYERS 2-4 -- the live chain.
# ---------------------------------------------------------------------------
say "layer 2-4: the chain, against the real steamclient.so"
start_helper || skip "the helper did not come up; see $OUT/helper.log"
say "  helper listening on 127.0.0.1:$PORT (pid $helper_pid)"

rc=$(run_probe "127.0.0.1:$PORT" "$OUT/live.out")
sed 's/^/      /' "$OUT/live.out"
[ "$rc" = 0 ] || bad "the live run exited $rc"

want_line "$OUT/live.out" "bridge: loaded steamclient64.dll"
want_line "$OUT/live.out" "bridge: exports resolved"
# layer 2: every parameter class round-tripped with the right value.  Any
# nonzero bit names which class disagreed -- see STEAMRPC_SELFTEST_BAD_* in
# dlls/steamclient64/steamrpc_wire.h.
want_line "$OUT/live.out" "bridge: selftest=0x0"
# layer 3: the interface object, and one call through its vtable.
want_line "$OUT/live.out" "bridge: CreateInterface(SteamClient017)=object"
want_line "$OUT/live.out" "bridge: DONE"

# ---------------------------------------------------------------------------
# LAYER 6 -- one callback through each PULL channel, with no Steam client.
#
# The helper is asked to queue a synthetic callback; everything downstream is
# the shipped path.  Both channels are exercised by ONE Steam_BGetCallback,
# because that is what Proton's PE side does: it drains the queue on the way
# in (which calls the guest function pointer the helper stored and never
# called) and answers the message on the way out.
#
# The values come out of the wire header rather than being spelled again here:
# a gate that carries its own copy of a constant can agree with itself while
# disagreeing with the code.
# ---------------------------------------------------------------------------
WIRE=$SRC/dlls/steamclient64/steamrpc_wire.h
CB_ID=$(sed -n 's/^#define STEAMRPC_INJECT_ID *\(0x[0-9a-f]*\).*/\1/p' "$WIRE")
CB_LEN=$(sed -n 's/^#define STEAMRPC_INJECT_LEN *\([0-9][0-9]*\).*/\1/p' "$WIRE")
[ -n "$CB_ID" ] && [ -n "$CB_LEN" ] || skip "cannot read the injection constants out of $WIRE"

say "layer 6: a callback delivered end to end, with no Steam client"
CB=1
rc=$(run_probe "127.0.0.1:$PORT" "$OUT/cb.out")
CB=
sed 's/^/      /' "$OUT/cb.out"
[ "$rc" = 0 ] || bad "the callback run exited $rc"
# the two injections were accepted
want_line "$OUT/cb.out" "bridge: cb_inject_cdecl=1"
want_line "$OUT/cb.out" "bridge: cb_inject_msg=1"
# channel 1: Steam_BGetCallback -- the export SteamAPI_RunCallbacks and
# SteamAPI_ManualDispatch_GetNextCallback are both built on
want_line "$OUT/cb.out" "bridge: cb_bget=1"
want_line "$OUT/cb.out" "bridge: cb_id=$CB_ID"
want_line "$OUT/cb.out" "bridge: cb_size=$CB_LEN"
# the nested payload pointer: still the game's allocation, and the right bytes
# in it.  A helper address here is the defect this whole class exists for, and
# it would surface as an unmapped read inside the game.
want_line "$OUT/cb.out" "bridge: cb_ptr=readable"
want_line "$OUT/cb.out" "bridge: cb_payload=ok"
# channel 2: the queue Proton drains into a guest function pointer -- the one
# SetWarningMessageHook, SetDebugOutputFunction and EnableActionEventCallbacks
# come back through
want_line "$OUT/cb.out" "bridge: cb_cdecl_calls=1"
want_line "$OUT/cb.out" "bridge: cb_cdecl_payload=ok"
want_line "$OUT/cb.out" "bridge: cb_free=1"

# ---------------------------------------------------------------------------
# LAYER 7 -- DOS <-> unix path translation, with no Steam client.
#
# Two paths, chosen for the two things that are easy to get wrong: a file that
# does not exist yet (every cloud save is one, and Wine answers it with a
# usable path rather than a failure), and a wrong-case spelling of one that
# does (Wine resolves each component case-insensitively, and games do spell
# C:\Users for a directory Wine created as users/).
# ---------------------------------------------------------------------------
say "layer 7: DOS<->unix paths, against this prefix"
C_ROOT=$(readlink -f "$WINEPREFIX/dosdevices/c:" 2>/dev/null)
if [ ! -d "$C_ROOT" ]; then
    bad "no C: drive in $WINEPREFIX; the path layer has nothing to check against"
else
    check_path() {   # $1 = DOS in, $2 = expected unix, $3 = expected DOS back
        PATH_IN=$1
        rc=$(run_probe "127.0.0.1:$PORT" "$OUT/path.out")
        PATH_IN=
        sed 's/^/      /' "$OUT/path.out"
        [ "$rc" = 0 ] || bad "the path run for $1 exited $rc"
        want_line "$OUT/path.out" "bridge: path_unix=$2"
        want_line "$OUT/path.out" "bridge: path_dos=$3"
        grep -qx "bridge: path_ret=0" "$OUT/path.out" && \
            bad "$1: the unix-to-DOS direction reported 0 bytes"
    }

    # a save file that does not exist yet, under a real user directory
    USERDIR=$(ls "$C_ROOT/users" 2>/dev/null | head -1)
    if [ -z "$USERDIR" ]; then
        bad "no C:\\users\\<user> in $WINEPREFIX to translate against"
    else
        check_path "C:\\users\\$USERDIR\\steam-bridge-save.dat" \
                   "$C_ROOT/users/$USERDIR/steam-bridge-save.dat" \
                   "C:\\users\\$USERDIR\\steam-bridge-save.dat"
    fi

    # a directory that exists, spelled in the wrong case on the way in and
    # answered in the real case on the way back
    check_path "C:\\WINDOWS\\SYSTEM32" \
               "$C_ROOT/windows/system32" \
               "C:\\windows\\system32"

    # the map itself: this prefix has at least C: and Z:, and the helper must
    # be holding them.  Reported by the helper, not assumed by the client.
    NDRIVES=$(sed -n 's/^bridge: path_drives=//p' "$OUT/path.out" | head -1)
    say "  helper is holding $NDRIVES drives"
    [ "${NDRIVES:-0}" -ge 2 ] || \
        bad "the helper holds $NDRIVES drives; this prefix has at least C: and Z:"
fi

# ---------------------------------------------------------------------------
# LAYER 8 -- the red zone.
# ---------------------------------------------------------------------------
say "layer 8: a too-small marshal descriptor is caught, not fatal"
GUARD=1
rc=$(run_probe "127.0.0.1:$PORT" "$OUT/guard.out")
GUARD=
sed 's/^/      /' "$OUT/guard.out"
[ "$rc" = 0 ] || bad "the guard run exited $rc"
# 1 = the helper noticed and refused the call by name.  0 would mean the
# overrun went through unnoticed, which is the entire defect.
want_line "$OUT/guard.out" "bridge: guard_caught_1=1"
want_line "$OUT/guard.out" "bridge: guard_caught_120=1"
# ...and it is still alive and still correct, which is what distinguishes a
# guard from a crash.
want_line "$OUT/guard.out" "bridge: guard_alive_selftest=0x0"
grep -q "past the end of the" "$OUT/helper.log" || \
    bad "the helper did not name the overrun on its own stderr"
grep -q "writing past the end of the buffer marshalled for buf" "${OUT}/guard.err" || \
    bad "the client did not name the parameter that overran"

# ---------------------------------------------------------------------------
# LAYER 8b -- the four descriptors that were wrong, checked against Valve.
#
# This is the DOOM regression itself, and it needs nothing running: the
# generated table either sizes these arrays by their SDK capacity or it does
# not.  The capacities are read out of the vendored SDK headers rather than
# written here, so a Valve change cannot leave this check agreeing with a
# stale copy of itself.
# ---------------------------------------------------------------------------
say "layer 8b: SDK fixed-capacity arrays are sized by Valve's own annotation"
SDK=$(ls -d "$SRC"/dlls/steamclient64/proton/steamworks_sdk_* 2>/dev/null | tail -1)
GEN=$SRC/dlls/steamclient64/steamrpc_generated.c
if [ -z "$SDK" ] || [ ! -f "$GEN" ]; then
    bad "cannot find the vendored SDK headers or the generated descriptors"
else
    cap_of() {   # $1 = macro name
        grep -rhoP "^#define\s+$1\s+\K[0-9]+" "$SDK"/*.h 2>/dev/null | head -1
    }
    check_cap() {   # $1 = descriptor field name, $2 = capacity macro, $3 = method
        cap=$(cap_of "$2")
        if [ -z "$cap" ]; then
            bad "layer 8b: $2 is not defined in $(basename "$SDK")"
            return
        fi
        # every descriptor for that field must multiply by the capacity
        bare=$(grep -c "{ \"$1\", offsetof(_P, $1), .*STEAMRPC_K_FIXED, sizeof" "$GEN")
        sized=$(grep -c "{ \"$1\", offsetof(_P, $1), .*STEAMRPC_K_FIXED, $cap \* sizeof" "$GEN")
        if [ "$bare" != 0 ]; then
            bad "layer 8b: $bare descriptor(s) still size $1 as ONE element; $3 writes $cap"
        elif [ "$sized" = 0 ]; then
            bad "layer 8b: no descriptor sizes $1 as $cap elements"
        else
            say "  ok: $sized descriptors size $1 as $2 ($cap) elements"
        fi
    }
    check_cap originsOut STEAM_INPUT_MAX_ORIGINS "GetDigital/AnalogActionOrigins"
    check_cap handlesOut STEAM_INPUT_MAX_COUNT "GetConnectedControllers/GetActiveActionSetLayers"
fi

# ---------------------------------------------------------------------------
# LAYER 4/5b -- what the answer must be depends on whether a client is up, and
# the two cases are asserted separately rather than accepting either.
# ---------------------------------------------------------------------------
if pgrep -x steam >/dev/null 2>&1 || pgrep -f 'steam/ubuntu.*steam$' >/dev/null 2>&1; then
    STEAM_UP=1
else
    STEAM_UP=0
fi

if [ "$STEAM_UP" = 0 ]; then
    say "layer 4: no Steam client is running -- the no-connection path"
    # steamclient.so's own answers with nothing to connect to.  Both are the
    # library's, reached through the bridge: "not known" for the interface
    # query and "no pipe" for the connection.  Asserted here rather than in
    # the shared section because a running client can legitimately change
    # them, and a gate that accepts either value checks nothing.
    want_line "$OUT/live.out" "bridge: IsKnownInterface(SteamClient017)=0"
    want_line "$OUT/live.out" "bridge: CreateSteamPipe=0"
    say "layer 5b: SKIPPED (no client running; this gate never starts one)"
else
    say "layer 5b: a Steam client is running -- appid 480 (Spacewar)"
    # Spacewar is the SDK's own test appid.  The game list game's appid is
    # deliberately NOT used: it would publish "playing" to the account.
    cleanup
    SteamAppId=480 SteamGameId=480 start_helper || \
        skip "the helper did not come up for the appid-480 run"
    # CB=2 is "poll, inject nothing": with a client on the far end the
    # callbacks are the client's own, which is the one thing the synthetic
    # layer above cannot prove.
    CB=2
    rc=$(SteamAppId=480 SteamGameId=480 run_probe "127.0.0.1:$PORT" "$OUT/app480.out")
    CB=
    sed 's/^/      /' "$OUT/app480.out"
    [ "$rc" = 0 ] || bad "the appid-480 run exited $rc"
    if grep -qx "bridge: CreateSteamPipe=0" "$OUT/app480.out"; then
        bad "a client is running but CreateSteamPipe still returned 0"
    else
        say "  ok: CreateSteamPipe returned a live pipe"
    fi
    # "a client is running" and "a user is logged in to it" are different
    # facts, and only the second one makes callbacks flow.  A client sitting
    # at its login screen answers IPC -- CreateSteamPipe succeeds -- and has
    # no global user to attach, which is Steam's own behaviour and not this
    # bridge's.  Asserting callbacks in that state would be a gate that goes
    # red for something it is not testing, so it is reported and skipped.
    if grep -qx "bridge: live_user=0" "$OUT/app480.out"; then
        say "  the client is running but no user is logged in to it, so it has"
        say "  no callbacks to deliver: that drain is SKIPPED, not faked"
    else
        LIVE_CB=$(sed -n 's/^bridge: live_cb_count=//p' "$OUT/app480.out" | head -1)
        say "  the live client delivered $LIVE_CB callbacks in the poll window"
        [ "${LIVE_CB:-0}" -ge 1 ] || \
            bad "a user is attached to a live client, but not one callback arrived"
    fi
fi

# ---------------------------------------------------------------------------
# LAYER 9 -- the helper killed out from under a running session.
#
# LAST, because it ends with no helper.  The probe reaches a point where it is
# mid-session and working, drops a marker file, and then polls; this script
# kills the helper the moment the marker appears and the probe's poll ends when
# its socket dies.  Nothing here is timed, so nothing here is flaky.
#
# SIGKILL rather than SIGTERM on purpose: the question is not whether the
# helper shuts down tidily, it is whether the GAME survives losing it the way
# a Windows game survives Steam being killed.
# ---------------------------------------------------------------------------
say "layer 9: the helper killed mid-session"
cleanup
rm -f "$OUT/kill-the-helper-now"
if ! start_helper; then
    bad "the helper did not come up for the mid-session kill"
else
    say "  helper listening on 127.0.0.1:$PORT (pid $helper_pid)"
    t0=$(date +%s)
    ( cd "$OUT" && timeout -k 5 "$TIMEOUT" \
        env WINEDEBUG="$WDBG" WINEDLLOVERRIDES="winedbg.exe=d" \
            STEAM_BRIDGE_ADDR="127.0.0.1:$PORT" \
            STEAMBRIDGEDEBUG="${STEAMBRIDGEDEBUG:-err}" \
            STEAM_BRIDGE_KILL=1 \
            "$BUILD/wine" "$OUT/steam_bridge_probe.exe" \
            >"$OUT/kill.out" 2>"$OUT/kill.err" ) &
    probe_job=$!

    n=0
    while [ $n -lt 1800 ]; do
        [ -f "$OUT/kill-the-helper-now" ] && break
        kill -0 "$probe_job" 2>/dev/null || break
        n=$((n + 1))
        sleep 0.1
    done

    if [ ! -f "$OUT/kill-the-helper-now" ]; then
        bad "the probe never reached the point where the helper is killed"
        kill "$probe_job" 2>/dev/null
    else
        say "  the probe is mid-session; killing the helper (pid $helper_pid) with SIGKILL"
        kill -9 "$helper_pid" 2>/dev/null
    fi
    wait "$probe_job"
    rc=$?
    helper_pid=
    t1=$(date +%s)
    sed 's/^/      /' "$OUT/kill.out"
    say "  probe exit $rc after $((t1 - t0))s"

    # The whole point: the game process outlives its Steam client.
    [ "$rc" = 0 ] || bad "losing the helper must not stop the process; the probe exited $rc"
    [ $((t1 - t0)) -lt 120 ] || bad "the probe took $((t1 - t0))s to notice; it must fail promptly"
    # It was working before the kill...
    want_line "$OUT/kill.out" "bridge: predeath_selftest=0x0"
    # ...and afterwards every call fails, with BAD_STATUS alone: a dead socket
    # must not be able to make the marshaller disagree about anything else.
    want_line "$OUT/kill.out" "bridge: dead_selftest=0x1"
    want_line "$OUT/kill.out" "bridge: dead_is_known=0"
    want_line "$OUT/kill.out" "bridge: dead_survived"
    want_line "$OUT/kill.out" "bridge: DONE"
    grep -q "steam bridge is down" "$OUT/kill.err" || \
        bad "the bridge did not say it was down; a silent stop is the defect this layer exists for"
fi
rm -f "$OUT/kill-the-helper-now"

cleanup

# Nothing of this gate's may outlive it, on a machine whose Steam client is the
# user's own.  Checked by the pids THIS script started and not by a pgrep for
# the helper's name: the build host is shared, another run's helper is none of
# this gate's business, and a name match would report someone else's live game
# as this gate's leak (measured: it did).
for p in $started_helpers; do
    if kill -0 "$p" 2>/dev/null; then
        bad "helper pid $p, started by this gate, is still running"
        kill -9 "$p" 2>/dev/null
    fi
done

if [ "$fail" = 0 ]; then
    say "PASS"
else
    say "FAILED"
fi
exit "$fail"
