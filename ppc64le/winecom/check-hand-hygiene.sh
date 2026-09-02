#!/bin/sh
#
# check-hand-hygiene.sh -- the gate on refusal hygiene in HAND walkers and
# flat GUEST-IMPL wrappers, the half check-com-levers.sh cannot see.
#
# WHY THIS GATE EXISTS.  Refused must mean INERT.  libs/winecom's
# scrub_refused_outs() enforces that for WINECOM_F_TABLE rows off the
# generated scrubptr/scrubdw/scrubq masks, and check-com-levers.sh leg D
# proves it.  Those masks reach no WINECOM_F_HAND row and no flat
# __wine_guest_* wrapper: each of those owns its out-params itself.  Six of
# them in dlls/combase/syscom.c owned them and did not write them, and one --
# IMMDevice::Activate refusing a non-NULL pActivationParams -- was the Witcher
# 3's load regression.  The game read the never-written *ppv back off its own
# stack, called through the residue, and the emulator decoded wined3d.dll's
# ppc64le bytes as x86 until they faulted.  Days.  See 690567d9d8e's message
# and ppc64le/docs/sessions/2026-09-01/w3-load-regression-bisect.md.
#
# The fix landed with no gate, which is how the same class ships again.  This
# is that gate.
#
# THE OBSERVABLE, from the caller's chair, is the guest's own out cell after a
# refusal it seeded with the residue-shaped sentinel first
# (ppc64le/winecom/probes/hand_hygiene_probe.c):
#
#   null      the refusal SCRUBBED the cell -- refused means INERT
#   sentinel  the refusal did NOT scrub, which is correct ONLY under
#             WINEEMUNOREFUSESCRUB=1 and is the crash class everywhere else
#   object    it SERVED, which means the refusal the arm measures did not
#             happen at all and the arm is void, not passing
#
# Legs:
#
#   A  BUILD: the guest probe compiles and links.
#   B  POSITIVE (walker): IMMDevice::Activate with a non-NULL PROPVARIANT
#      pActivationParams -> activate_out=null and activate_hr=0x80004001
#      (E_NOTIMPL).  This is the live site, driven live: a real
#      MMDeviceEnumerator, a real endpoint out of the prefix, a real Activate.
#   C  POSITIVE (flat wrapper, a DIFFERENT FILE): DirectInput8Create with a
#      non-NULL aggregation punkOuter -> dinput_out=null.  dlls/dinput8/
#      guestcom.c, and the flat path rather than the walker path -- one file's
#      scrub proving load-bearing does not prove the shared helper reaches the
#      others.  Its HRESULT is DIERR_NOAGGREGATION (0x80040110), dinput8's own
#      answer, which is deliberately NOT E_NOTIMPL: the hygiene rule is about
#      the out-param, not about which error the refusal chose.
#   D  SABOTAGE: B and C again under WINEEMUNOREFUSESCRUB=1.  BOTH sentinels
#      must SURVIVE.  This is what proves legs B and C measured the SCRUB and
#      not mmdevapi or dinput8 happening to write NULL on their own -- and it
#      is the whole reason the scrubs go through winecom_refused_scrub_*
#      rather than an inline `*out = NULL` the lever cannot reach.
#
# --sabotage runs leg D alone.  A gate that cannot go red proves nothing.
#
# Exit 0 = pass, 1 = a check failed, 2 = could not run at all (a skip is NOT a
# pass).
#
# WINEDLLOVERRIDES=winedbg.exe=d on every run, verbatim from
# check-com-levers.sh and for the identical reason: the bringup prefix has
# AeDebug pointed at "winedbg --auto", so a run that ends in an unhandled
# fault -- what a defect in this boundary looks like from outside -- would
# start the debugger, which attaches and never lets go, turning every red
# state of this gate into a hang.
set -u

HERE=$(cd "$(dirname "$0")" && pwd)
SRC=$(cd "$HERE/../.." && pwd)
BUILD=${BUILD:-$SRC}
OUT=${OUT:-/tmp/hand-hygiene}
TIMEOUT=${TIMEOUT:-120}
SABOTAGE=0
[ "${1:-}" = "--sabotage" ] && SABOTAGE=1

say()  { echo "check-hand-hygiene: $*"; }
bad()  { echo "check-hand-hygiene: FAIL $*" >&2; fail=1; }
note() { echo "check-hand-hygiene: note $*"; }
skip() { echo "check-hand-hygiene: $*" >&2; exit 2; }

[ -x "$BUILD/wine" ] || skip "no wine loader at $BUILD/wine"
[ -n "${WINEPREFIX:-}" ] || skip "set WINEPREFIX to a prefix wineboot has run in"
[ -n "${WINEFEXBRIDGE:-}" ] || skip "set WINEFEXBRIDGE to the emulator bridge"
[ -f "$BUILD/dlls/combase/x86_64-windows/combase.dll" ] || \
    skip "no guest combase thunk; build it first"
[ -f "$BUILD/dlls/dinput8/x86_64-windows/dinput8.dll" ] || \
    skip "no guest dinput8 thunk; build it first"
command -v clang >/dev/null || skip "need clang for the guest build"
command -v llvm-dlltool >/dev/null || skip "need llvm-dlltool for the guest build"

mkdir -p "$OUT" || skip "cannot create $OUT"
fail=0

INCL="-I$BUILD/include -I$SRC/include -I$SRC/include/msvcrt"

# ---- A: build the guest probe ----------------------------------------------
# The same clang x86_64-windows-gnu machinery, hand-written .def imports and
# "no CRT" rule as the com_lever_smoke probe beside it: the probe binds to the
# builtins a real guest application binds to, and nothing else is linked in.
cat > "$OUT/kernel32.def" <<'EOF'
LIBRARY kernel32.dll
EXPORTS
GetStdHandle
WriteFile
ExitProcess
GetModuleHandleW
EOF
cat > "$OUT/ole32.def" <<'EOF'
LIBRARY ole32.dll
EXPORTS
CoInitializeEx
CoUninitialize
CoCreateInstance
EOF
cat > "$OUT/dinput8.def" <<'EOF'
LIBRARY dinput8.dll
EXPORTS
DirectInput8Create
EOF
for m in kernel32 ole32 dinput8; do
    llvm-dlltool -m i386:x86-64 -d "$OUT/$m.def" -l "$OUT/lib$m.a" \
        || skip "llvm-dlltool failed for $m"
done

clang -target x86_64-windows-gnu -nostdlibinc $INCL -Wall -O1 -fno-builtin -g \
    -c -o "$OUT/probe.o" "$HERE/probes/hand_hygiene_probe.c" \
    2>"$OUT/build.err" || { sed 's/^/  build| /' "$OUT/build.err" >&2;
                            skip "the guest probe did not compile"; }
clang -target x86_64-windows-gnu -fuse-ld=lld -nostdlib \
    -Wl,--entry=hand_hygiene_entry -Wl,--subsystem,console \
    -o "$OUT/probe.exe" "$OUT/probe.o" \
    "$OUT/libole32.a" "$OUT/libdinput8.a" "$OUT/libkernel32.a" \
    2>>"$OUT/build.err" || { sed 's/^/  build| /' "$OUT/build.err" >&2;
                             skip "the guest probe did not link"; }
say "build: $OUT/probe.exe"

# run <tag> [VAR=VAL ...] -- always +winecom, because the refusal lines the
# arms lean on are the PORT's own trace, not the probe's output.
run() {
    tag=$1; shift
    timeout -k 5 "$TIMEOUT" \
        env WINEDEBUG=+winecom WINEDLLOVERRIDES="winedbg.exe=d" "$@" \
        "$BUILD/wine" "$OUT/probe.exe" \
        > "$OUT/$tag.out" 2>"$OUT/$tag.err"
    rc=$?
    if [ $rc -eq 124 ] || [ $rc -eq 137 ]; then
        bad "the $tag run HUNG (killed after ${TIMEOUT}s)"
        return 1
    fi
    grep -q "hand_hygiene_probe: done" "$OUT/$tag.out" && return 0
    sed 's/^/  '"$tag"'| /' "$OUT/$tag.out" >&2
    tail -15 "$OUT/$tag.err" | sed 's/^/  '"$tag"'| /' >&2
    bad "the $tag run never reached its own last line"
    return 1
}

field() { grep -o "$2=[a-zA-Z0-9]*" "$OUT/$1.out" | head -1 | cut -d= -f2; }

# want <tag> <field> <expected> <why>
want() {
    got=$(field "$1" "$2")
    if [ "$got" = "$3" ]; then
        say "$1: $2=$3 -- $4"
    else
        bad "$1: wanted $2=$3, got '${got:-<absent>}' -- $4"
    fi
}

E_NOTIMPL=80004001
DIERR_NOAGGREGATION=80040110

# ---- D: the negative control, also available standalone as --sabotage ------
# With the lever armed, BOTH refusals must leave the sentinel standing.  If a
# sentinel dies here, the corresponding positive arm's "null" was somebody
# else's NULL and proved nothing about our scrub.
sabotage() {
    ok=1

    if run noscrub WINEEMUNOREFUSESCRUB=1; then
        got=$(field noscrub activate_out)
        if [ "$got" = "sentinel" ]; then
            say "sabotage: WINEEMUNOREFUSESCRUB=1 left the sentinel standing in \
IMMDevice::Activate's out cell -- the walker scrub is load-bearing"
        elif [ "$got" = "object" ] || [ "$got" = "noenum" ] || [ "$got" = "nodevice" ]; then
            bad "the Activate arm never reached its refusal (activate_out=$got); \
the walker leg measures nothing"; ok=0
        else
            bad "WINEEMUNOREFUSESCRUB=1 did not stop the walker refusal scrub \
(activate_out=$got); the lever is not the negative control it claims to be, \
and check-hand-hygiene's positive arm proves nothing"; ok=0
        fi

        got=$(field noscrub dinput_out)
        if [ "$got" = "sentinel" ]; then
            say "sabotage: WINEEMUNOREFUSESCRUB=1 left the sentinel standing in \
DirectInput8Create's out cell -- the flat-wrapper scrub is load-bearing too"
        elif [ "$got" = "object" ]; then
            bad "DirectInput8Create SERVED an aggregation outer; the flat-wrapper \
leg measures nothing"; ok=0
        else
            bad "WINEEMUNOREFUSESCRUB=1 did not stop the flat-wrapper refusal \
scrub (dinput_out=$got)"; ok=0
        fi
    else ok=0; fi

    [ "$ok" = 1 ] && say "SABOTAGE PASS"
    [ "$ok" = 1 ]
}

if [ "$SABOTAGE" = 1 ]; then
    sabotage || exit 1
    exit 0
fi

# ---- B: the walker, live ---------------------------------------------------
if run scrub; then
    want scrub activate_out null \
        "IMMDevice::Activate refused its PROPVARIANT and scrubbed *ppv"
    got=$(field scrub activate_hr)
    if [ "$got" = "0x$E_NOTIMPL" ]; then
        say "scrub: activate_hr=0x$E_NOTIMPL -- E_NOTIMPL, the refusal's own answer"
    else
        bad "scrub: wanted activate_hr=0x$E_NOTIMPL (E_NOTIMPL), got '${got:-<absent>}'"
    fi
    if grep -q "IMMDevice::Activate" "$OUT/scrub.err"; then
        say "scrub: $(grep -m1 'IMMDevice::Activate' "$OUT/scrub.err" | cut -c1-140)"
    else
        note "the port did not name IMMDevice::Activate in the trace (the \
refuse-once flag may already be set for this process); the out cell is the \
gate's real observable and it is checked above"
    fi
else
    fail=1
fi

# ---- C: the flat wrapper, a different file ---------------------------------
if [ -f "$OUT/scrub.out" ]; then
    want scrub dinput_out null \
        "DirectInput8Create refused a non-NULL punkOuter and scrubbed *out"
    got=$(field scrub dinput_hr)
    if [ "$got" = "0x$DIERR_NOAGGREGATION" ]; then
        say "scrub: dinput_hr=0x$DIERR_NOAGGREGATION -- DIERR_NOAGGREGATION, \
dinput8's own answer for the case it cannot serve"
    else
        bad "scrub: wanted dinput_hr=0x$DIERR_NOAGGREGATION \
(DIERR_NOAGGREGATION), got '${got:-<absent>}'"
    fi
fi

# ---- D --------------------------------------------------------------------
sabotage || fail=1

[ "$fail" = 0 ] && say "PASS"
exit $fail
