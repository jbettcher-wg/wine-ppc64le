#!/bin/sh
#
# probe-interface.sh -- build and run interface_probe.c on BOTH guest machines,
#                       headless, without a game, a display or the game lock.
#
#   probe-interface.sh [--machine i386|x86_64] [--level 1..4] [--iface NAME]
#
# WHAT THIS IS FOR.  check-steam-bridge.sh's layer 3 drives "a guest x86-64
# probe standing exactly where steam_api64.dll stands -- LoadLibrary,
# GetProcAddress, CreateInterface, and one call through the returned vtable".
# This is that layer for BOTH widths at once, run as a diagnostic rather than
# a gate, and split into four escalating steps so a failure says WHICH step
# broke.  It exists because of a distinction this port turned out to need and
# had no way to make: "the transport works" is not the same claim as "the
# transport works AND interface calls work", and on i386 those two currently
# have different answers.
#
# NOT a check-*.sh, deliberately, for the same reasons probe-dllload.sh is
# not: nothing here raises a dialog, nothing needs the display, and it does
# not take the game lock.  It starts its OWN helper on an ephemeral port and
# stops it again, so it does not disturb a running game or a running gate.
#
# ===========================================================================
# WHAT IT MEASURED WHEN IT LANDED  [2026-08-30, AC922, both lanes, headless]
# ===========================================================================
#
#   step                                              i386          x86_64
#   ------------------------------------------------  ------------  --------
#   1  LoadLibrary                                    ok            ok
#   2  + CreateInterface                              ok            ok
#   3  + exception dispatch after CreateInterface     ok            ok
#   4  + one call through vtable slot 0               ACCESS VIOL   ok (30)
#
# Step 2 is not a formality on either lane: with a helper listening, the
# client connects and its drive-map frame really does cross -- the helper,
# run with -v, logs "accepted connection on fd 5" and both drives.  So on
# i386 the SCR3 transport carries real traffic and it is the first
# MARSHALLED METHOD CALL that fails, before its frame ever reaches the helper.
#
# On x86_64 step 4 returns non-zero: a frame crossed to the helper, into the
# real Steam client, and back.  That lane is end-to-end.
#
# The i386 failure, exactly:
#
#     err:seh:KiUserExceptionDispatcher code=c0000005
#     err:seh:KiUserExceptionDispatcher access violation at 000000007BDB0064:
#                                       reading 0000000004245C89
#     err:sync:RtlpWaitForCriticalSection section 7A0640B4 (invalid)
#                                       wait timed out ... retrying (60 sec)
#
# 0x7BDB0064 is ntdll.dll (loaded at 7BD90000) + 0x20064, which falls between
# the exported RtlSetUnhandledExceptionFilter (0x1FEE0) and
# RtlCaptureStackBackTrace (0x20130) -- i.e. in the vectored-handler and
# stack-walking region, in a static function that exports cannot name.  The
# probe then hangs on the invalid critical section rather than exiting, so
# runs are bounded by a timeout here.
#
# WHAT THIS PROBE DOES **NOT** COVER, and the next person must not assume it
# does.  Portal 2 fails with a DIFFERENT exception: an unhandled
# STATUS_INVALID_HANDLE (0xc0000008), raised from RtlRaiseStatus (ntdll+0x1FC26)
# immediately after steam_api.dll logs "[S_API] SteamAPI_Init(): Loaded
# '...steamclient.dll' OK."  Both failures strike at the first real interface
# call after this port's i386 steamclient.dll loads, which is why they are
# PLAUSIBLY the same defect -- but that is INFERRED AND NOT PROVEN.  The codes
# differ, and Portal 2 has breakpad plus its own SEH filter installed (its
# handler at 0x00402910 is called and declines), either of which could
# transform one into the other.  Making this probe green is therefore NOT by
# itself evidence that Portal 2 is fixed.  Check Portal 2.
#
# ===========================================================================
# DEAD ENDS -- do not spend the evening rediscovering these
# ===========================================================================
#
# * WINEDBG GIVES NO BACKTRACE ON THIS PORT.  WINE_PPC64LE_WINEDBG=1 does
#   restore the debugger and it does attach ("WineDbg attached to pid ..."),
#   and then it dies of its own c0000096 (STATUS_PRIVILEGED_INSTRUCTION)
#   before printing anything.  No call stack has ever been obtained this way.
#   Use WINEDEBUG=+seh,+debugstr and the escalating steps above instead.
#
# * These were each tested and are NOT the cause:
#     - OutputDebugStringA on its own.  A guest PE that does nothing but call
#       it twice survives on BOTH lanes.  The DBG_PRINTEXCEPTION_C in the logs
#       is normal and is handled.
#     - A bridge round-trip corrupting state.  Step 3 with a live helper --
#       so CreateInterface's frames really crossed -- passes on i386.
#     - Marshal descriptors baked at the wrong width.  steamrpc_generated.c
#       uses offsetof()/sizeof(), recomputed per compile, and slot 0's
#       descriptor has no pointer fields at all.
#     - A missing i386 thiscall vtable thunk.  Wine's DEFINE_THISCALL_WRAPPER
#       is correctly disabled for a -windows-gnu target, so the vtable holds
#       the raw __thiscall function; see interface_probe.c.
#     - The probe calling wrong.  Its i386 object really does put `this` in
#       ecx before the indirect call; disassembled, not assumed.
#
# ===========================================================================
# THE BEST UNEXPLORED LEAD  -- deliberately NOT chased on 2026-08-30
# ===========================================================================
#
# dlls/steamclient64/proton/steamclient_main.c skips BOTH RTTI initialisers in
# DllMain on i386:
#
#     #if defined(__x86_64__) || defined(__aarch64__)
#         init_type_info_rtti( (char *)instance );
#         init_rtti( (char *)instance );
#     #endif
#
# ...while proton/cxx.h still emits an RTTI pointer immediately BEFORE every
# vtable (__ASM_VTABLE lays down "<name>_rtti" then the vtable), and undefines
# RTTI_USE_RVA for i386 so that pointer is absolute rather than an RVA.  That
# is a real width asymmetry sitting exactly where the failure is: the object
# whose slot 0 we call is the object whose RTTI was never initialised.  It is
# most likely fine -- this is Proton's own vendored code and Proton ships i386
# -- but it is untested here and it is the first thing to check.
#
# ===========================================================================
# A CORRECTION TO THE RECORD
# ===========================================================================
#
# The message of commit 8970f3bbfd9 says of a Portal 2 run that "the bridge was
# never reached: both helpers sat idle with the game running."  The conclusion
# is true for that run -- Valve's own bin/steamclient.dll had been loaded and
# this port's was not -- but the REASONING given for it was unsound, and is
# repeated here so nobody reuses it: it was inferred from helper logs showing
# only "listening".  steamhelper_log() is gated on -v (see
# helper/steamhelper.c), so without -v those logs say nothing whatever about
# whether a connection was accepted.  Always pass -v before concluding a
# helper was idle.  The commit message is left as written rather than
# rewritten, because other work is committed on top of it.
#
# ---------------------------------------------------------------------------
set -u

here=$(cd "$(dirname "$0")" && pwd -P)
src=$(cd "$here/../.." && pwd -P)          # the wine source tree
tool=$src/ppc64le/steamtool
build=${WINE_PPC64LE_TREE:-$src/../wine-build}

machines="x86_64 i386"
level=4
iface=SteamClient020
timeout_s=${INTERFACE_PROBE_TIMEOUT:-90}

while [ $# -gt 0 ]; do
    case $1 in
    --machine) machines=${2:?--machine needs x86_64 or i386}; shift 2 ;;
    --level)   level=${2:?--level needs 1..4}; shift 2 ;;
    --iface)   iface=${2:?--iface needs an interface name}; shift 2 ;;
    -*) echo "probe-interface.sh: unknown option $1" >&2; exit 2 ;;
    *)  echo "probe-interface.sh: unexpected argument $1" >&2; exit 2 ;;
    esac
done

out=${PROBE_OUT:-$build/ppc64le-probes}
mkdir -p "$out" || { echo "probe-interface.sh: cannot create $out" >&2; exit 1; }

# Same split, same reason, as probe-dllload.sh: the x86-64 lane links straight
# against the kernel32 THUNK dll's export table, while i386 needs winebuild's
# import library because its kernel32 exports are undecorated and a stdcall
# call site wants _Name@N.
build_probe() {
    _m=$1
    case $_m in
    x86_64) _kern32=$build/dlls/kernel32/x86_64-windows/kernel32.dll ;;
    i386)   _kern32=$build/dlls/kernel32/i386-windows/libkernel32.a ;;
    esac
    clang -target $_m-windows-gnu -nostdlibinc \
        -I"$build/include" -I"$src/include" -I"$src/include/msvcrt" -D_MSVCR_VER=0 \
        -Wall -O1 -fno-builtin -g -c -o "$out/interface_probe_$_m.o" \
        "$here/interface_probe.c" || return 1
    clang -target $_m-windows-gnu -fuse-ld=lld -nostdlib \
        -Wl,--entry=interface_probe_entry -Wl,--subsystem,console \
        -o "$out/interface_probe_$_m.exe" "$out/interface_probe_$_m.o" "$_kern32" || return 1
    return 0
}

dospath() { printf 'Z:%s' "$(printf %s "$1" | tr / '\\')"; }

rc_meaning() {
    case $1 in
    20)  echo "reached the end of step $level, no fault" ;;
    30)  echo "vtable slot 0 returned NON-ZERO (a live Steam client answered)" ;;
    31)  echo "vtable slot 0 returned ZERO (no client; it RETURNED, which passes)" ;;
    10)  echo "INTERFACE_PROBE_DLL unset" ;;
    11)  echo "LoadLibrary FAILED" ;;
    12)  echo "no CreateInterface export" ;;
    13)  echo "CreateInterface returned NULL" ;;
    124) echo "TIMED OUT -- see the invalid-critical-section hang in the header" ;;
    *)   echo "died (see the log above)" ;;
    esac
}

overall=0
for m in $machines; do
    case $m in
    x86_64) helper=steamhelper;   var=STEAM_BRIDGE_ADDR
            dll=$build/dlls/steamclient64/x86_64-windows/steamclient64.dll ;;
    i386)   helper=steamhelper32; var=STEAM_BRIDGE_ADDR32
            dll=$build/dlls/steamclient/i386-windows/steamclient.dll ;;
    *) echo "probe-interface.sh: --machine must be x86_64 or i386" >&2; exit 2 ;;
    esac

    echo "=== $m ==="
    [ -f "$dll" ] || { echo "  no $dll -- build it first"; overall=1; continue; }
    build_probe "$m" || { echo "  probe build FAILED"; overall=1; continue; }

    hexe=$here/helper/$helper
    if [ ! -x "$hexe" ]; then
        echo "  building the $m helper"
        "$here/helper/build-helper.sh" --machine "$m" >/dev/null || {
            echo "  helper build FAILED"; overall=1; continue; }
    fi

    hlog=$out/interface_probe_$m.helper.log
    # -v, always.  Without it steamhelper_log() prints nothing and the log
    # cannot tell an accepted connection from an idle helper -- the exact
    # mistake recorded in the correction above.
    "$hexe" --serve --port 0 -v >"$hlog" 2>&1 &
    hpid=$!
    n=0
    while [ $n -lt 100 ]; do
        port=$(sed -n 's/.*listening on 127\.0\.0\.1:\([0-9][0-9]*\).*/\1/p' "$hlog" 2>/dev/null | head -1)
        [ -n "${port:-}" ] && break
        kill -0 "$hpid" 2>/dev/null || break
        n=$((n + 1)); sleep 0.1
    done
    if [ -z "${port:-}" ]; then
        echo "  the $m helper did not come up; see $hlog"
        kill "$hpid" 2>/dev/null; wait "$hpid" 2>/dev/null
        overall=1; continue
    fi

    plog=$out/interface_probe_$m.run.log
    env WINE_PPC64LE_NO_STEAM_BRIDGE=1 \
        "$var=127.0.0.1:$port" \
        INTERFACE_PROBE_DLL="$(dospath "$dll")" \
        INTERFACE_PROBE_IFACE="$iface" \
        INTERFACE_PROBE_LEVEL="$level" \
        WINEDEBUG="${WINEDEBUG:-+debugstr}" \
        timeout -s TERM "$timeout_s" "$tool/run-native" --name ifprobe \
        "$out/interface_probe_$m.exe" >"$plog" 2>&1
    rc=$?

    # SIGTERM, never SIGKILL: the helper may be inside the Steam client.
    kill "$hpid" 2>/dev/null; wait "$hpid" 2>/dev/null

    echo "  rc=$rc -- $(rc_meaning $rc)"
    echo "  helper: $(grep -c 'accepted connection' "$hlog") connection(s) accepted, $(grep -c 'drive ' "$hlog") drive(s) received"

    # run-native's own stdout only NAMES the guest log; the seh/debugstr lines
    # that matter are in that file, so follow it rather than grepping $plog.
    glog=$(sed -n 's/.*log -> \(.*\.log\).*/\1/p' "$plog" | head -1)
    grep -o 'interface_probe: [^\\"]*' "$plog" | sed 's/^/  /'
    if [ -n "${glog:-}" ] && [ -f "$glog" ]; then
        grep -o 'interface_probe: [^\\"]*' "$glog" | sed 's/^/  /'
        grep -E "access violation at|Unhandled exception|RtlpWaitForCriticalSection|report_native_pc" "$glog" \
            | sed 's/^/  /' | head -4
    fi
    echo "  logs: $plog  $hlog${glog:+  $glog}"

    case $rc in 20|30|31) ;; *) overall=1 ;; esac
done

exit $overall
