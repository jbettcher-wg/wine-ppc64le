#!/bin/sh
#
# check-audio-smoke.sh -- the native-vs-guest AUDIO runtime gate.
#
# The claim: an x86-64 Windows program run as a GUEST under this port creates a
# real DirectSound device and an XAudio2 engine, fills a buffer with PCM, plays
# it against a real audio backend, watches the cursor move, reads every piece
# of state back, and gets BYTE-IDENTICAL output to the same source built for
# this machine's OWN architecture and run through the same Wine, the same
# dsound/xaudio2, the same mmdevapi and the same host device.  Only the
# caller's instruction set differs -- and therefore only whether every one of
# those calls crossed the guest COM boundary.
#
# Byte-identical is the bar rather than "the guest said PASS" for the reason
# check-com-smoke.sh and check-gl-smoke.sh give: reaching the right answer
# through the wrong mechanism is exactly the failure a PASS/PASS comparison
# cannot see.  Every byte compared here is a value the implementation
# computed -- a device capability, 176400 bytes of PCM checksummed after a
# round trip through Lock, a play cursor's movement, the raw bits of six
# floats of which three travelled on the guest's stack.
#
# NOTHING IS PLAYED AT ANYONE.  This machine has no sound card at all
# (`aplay -l` finds none); every sink on it is a virtual one belonging to the
# person using it.  So the gate loads a null sink OF ITS OWN, points
# PULSE_SINK at it for the duration, and unloads it on the way out.  The
# default sink is never changed and no existing sink is ever opened -- leg C
# says which sink was used, by name, in the transcript.
#
# Legs:
#
#   A  ROSTER CURRENT: gen_interfaces.py --check and gen_winecom.py --check for
#      both surfaces.  The guest thunk's stub arrays and the native module's
#      marshal tables are emitted by two generators from ONE roster each; if a
#      roster has drifted from the headers, or a marshal header from its
#      roster, everything below is testing a surface that no longer exists.
#   B  BUILD: the guest thunk modules exist, are COM-mode, and publish the
#      interface and slot counts the marshal tables expect.  Plus the one-line
#      regression: x3daudio1_7.dll's guest module must export its two entry
#      points, because it exported NONE until x3daudio.h reached the oracle.
#   C  SINK: a null sink of this gate's own.
#   D  DISPLAY: an Xvfb of this gate's own, on a display number nobody is
#      using.  DirectSound's SetCooperativeLevel wants a window, and the person
#      at this machine is on the real displays.
#   E  NATIVE ds: the probe built as a ppc64 PE, run under this wine with no
#      guest anywhere in the process, reports PASS.
#   F  GUEST ds: the same source built as an x86-64 PE, run as a guest, PASS.
#   G  IDENTITY: cmp(native stdout, guest stdout) is empty.
#   H  REFUSAL: the guest's own stderr must show DirectSound's one
#      reverse-proxy refusal -- CreateSoundBuffer with a non-NULL aggregation
#      pUnkOuter -- answering DSERR_NOAGGREGATION, where the native leg
#      succeeds.  A port that quietly served it would be handing Wine's dsound
#      a guest-implemented IUnknown.
#   I-K the same three legs for XAudio2, and again for winmm -- a FLAT surface
#      with no COM at all, whose 187-export guest thunk predates this work and
#      had never been checked to produce sound rather than a return code.
#   L  SERVED, THROUGH THE REVERSE PROXY: XAudio2's two reverse-proxy entry
#      points -- RegisterForCallbacks and CreateSourceVoice with a pCallback --
#      answered E_NOTIMPL on the guest leg until libs/winecom/reverse.c built a
#      NATIVE vtable for a GUEST-implemented object; now xa_smoke.c's steps
#      13-19 diff both as S_OK like everything else, and what leg L checks
#      instead is that the SERVING went through the mechanism and not around
#      it: the guest run's own +winecom trace (leg M's trace, re-examined)
#      must show a reverse wrap of IXAudio2VoiceCallback, a reverse wrap of
#      IXAudio2EngineCallback, and winecom_reverse_dispatch dispatching
#      IXAudio2VoiceCallback::OnBufferEnd BY NAME -- with no reverse call for
#      either interface refused.  Identical output with no such trace would
#      mean the guest was calling a native callback pointer directly, which is
#      the one thing that must be impossible.
#   M  MECHANISM: the port's own +winecom trace must show each surface's guest
#      vtables materialised with the exact interface and slot counts the
#      rosters state.  Identical output through a mechanism that never
#      attached would mean the guest was calling native vtables directly.
#
# --sabotage runs the negative controls instead, and requires every one to go
# red.  A gate that cannot go red proves nothing:
#
#   1  WINEEMUNOCOMWRAP=1 hands the guest RAW host pointers -- the exact defect
#      libs/winecom exists to fix, in EITHER direction now -- and neither
#      guest leg may PASS.
#   2  each DS_SMOKE_BREAK=1..3 and XA_SMOKE_BREAK=1..5 build of the NATIVE leg
#      must FAIL, so the checks are shown to be checks.
#
# Exit 0 = pass, 1 = a check failed, 2 = could not run at all (a skip is NOT a
# pass).
#
# WHY EVERY WINE RUN DISABLES winedbg, verbatim from check-gl-smoke.sh because
# the hazard is identical: the bringup prefix has AeDebug configured with
# "winedbg --auto", so a run that ends in an unhandled fault -- which is what a
# defect here looks like from outside -- starts a debugger that attaches and
# never lets go, turning every red state of this gate into a hang.
set -u

HERE=$(cd "$(dirname "$0")" && pwd)
SRC=$(cd "$HERE/../.." && pwd)
BUILD=${BUILD:-$SRC}                    # in-tree build by default
OUT=${OUT:-/tmp/audio-smoke}
SABOTAGE=0
[ "${1:-}" = "--sabotage" ] && SABOTAGE=1

say()  { echo "check-audio-smoke: $*"; }
bad()  { echo "check-audio-smoke: FAIL $*" >&2; fail=1; }
skip() { echo "check-audio-smoke: $*" >&2; cleanup; exit 2; }

XVFB_PID=
XVFB_DISPLAY=
SINK_MODULE=
SINK_NAME=wine_audio_gate_$$
DEFAULT_SINK_SAVED=

# Never trust the captured module id alone: look it up and refuse to unload
# unless its own arguments name OUR sink.  This is what keeps a gate from ever
# unloading a module -- null sink or otherwise -- that belongs to the person
# using the machine (a real regression: a gate once tore down the user's own
# Sunshine streaming sink this way).
unload_own_sink() {
    [ -n "$SINK_MODULE" ] || return 0
    args=$(pactl list short modules 2>/dev/null \
        | awk -F'\t' -v id="$SINK_MODULE" '$1==id{print $3}')
    case "$args" in
        *"sink_name=$SINK_NAME"*) pactl unload-module "$SINK_MODULE" 2>/dev/null ;;
        "") ;;  # already gone -- nothing to unload
        *) echo "check-audio-smoke: refusing to unload module $SINK_MODULE: its \
arguments do not name sink $SINK_NAME (got: $args)" >&2 ;;
    esac
    SINK_MODULE=
}

# The default sink is never meant to change here, but restore it defensively
# in case some other layer (pipewire-pulse's own default-sink policy, e.g.)
# moved it out from under this gate when the null sink appeared.
restore_default_sink() {
    [ -n "$DEFAULT_SINK_SAVED" ] || return 0
    cur=$(pactl get-default-sink 2>/dev/null)
    [ "$cur" = "$DEFAULT_SINK_SAVED" ] && return 0
    pactl set-default-sink "$DEFAULT_SINK_SAVED" 2>/dev/null
}

cleanup() {
    [ -n "$XVFB_PID" ] && kill "$XVFB_PID" 2>/dev/null
    XVFB_PID=
    unload_own_sink
    restore_default_sink
}
trap 'cleanup' EXIT INT TERM
DEFAULT_SINK_SAVED=$(pactl get-default-sink 2>/dev/null)

[ -x "$BUILD/wine" ] || skip "no wine loader at $BUILD/wine"
[ -n "${WINEPREFIX:-}" ] || skip "set WINEPREFIX to a prefix wineboot has run in"
[ -n "${WINEFEXBRIDGE:-}" ] || skip "set WINEFEXBRIDGE to the emulator bridge"
command -v clang >/dev/null || skip "need clang for the guest build"
command -v llvm-dlltool >/dev/null || skip "need llvm-dlltool for the guest build"
command -v "${CC:-gcc}" >/dev/null || skip "need ${CC:-gcc} for the native ppc64 build"
command -v python3 >/dev/null || skip "need python3 for the roster checks"
command -v Xvfb >/dev/null || skip "need Xvfb: DirectSound wants a window and \
this gate must not take the display the user is on"
command -v pactl >/dev/null || skip "need pactl: this gate plays into a null \
sink of its own and will not open a sink it did not create"

mkdir -p "$OUT" || skip "cannot create $OUT"
fail=0
TIMEOUT=${TIMEOUT:-240}
INCL="-I$BUILD/include -I$SRC/include -I$SRC/include/msvcrt"
DS_GUEST="$BUILD/dlls/dsound/x86_64-windows/dsound.dll"
XA_GUEST="$BUILD/dlls/xaudio2_9/x86_64-windows/xaudio2_9.dll"
XA8_GUEST="$BUILD/dlls/xaudio2_8/x86_64-windows/xaudio2_8.dll"
X3D_GUEST="$BUILD/dlls/x3daudio1_7/x86_64-windows/x3daudio1_7.dll"

# ---- A: the rosters and the marshal tables are current ---------------------
roster_check() {   # roster_check <surface> <marshal header>
    s=$1; h=$2
    if python3 "$HERE/gen_interfaces.py" --surface "$s" --build "$BUILD" \
            --check "$HERE/interfaces_$s.json" > "$OUT/roster_$s.out" 2>&1; then
        say "roster($s): $(grep -m1 'interface(s)' "$OUT/roster_$s.out")"
    else
        sed 's/^/  roster| /' "$OUT/roster_$s.out" >&2
        bad "ppc64le/audio/interfaces_$s.json has drifted from the headers"
    fi
    if python3 "$HERE/gen_winecom.py" --surface "$s" --build "$BUILD" \
            --check "$h" > "$OUT/marshal_$s.out" 2>&1; then
        say "marshal($s): $(grep -m1 'marshalled' "$OUT/marshal_$s.out")"
    else
        sed 's/^/  marshal| /' "$OUT/marshal_$s.out" >&2
        bad "$h has drifted from interfaces_$s.json"
    fi
}
roster_check dsound    "$SRC/dlls/dsound/dsound_marshal.h"
roster_check xaudio2_9 "$SRC/dlls/xaudio2_9/xaudio2_marshal.h"
roster_check xaudio2_8 "$SRC/dlls/xaudio2_8/xaudio2_marshal.h"

# ---- B: the guest thunk modules -------------------------------------------
# Reads the module's own __wine_com_thunk_info the way libs/winecom does, so
# the number checked here is the number the runtime will check at attach.
com_counts() {   # com_counts <guest dll> -> "<ifaces> <slots>"
    python3 - "$1" <<'EOF'
import struct, sys
d = open(sys.argv[1], 'rb').read()
pe = struct.unpack_from('<I', d, 0x3c)[0]
nsec = struct.unpack_from('<H', d, pe + 6)[0]
opt = struct.unpack_from('<H', d, pe + 20)[0]
edir_rva = struct.unpack_from('<I', d, pe + 24 + 112)[0]
secs = []
for i in range(nsec):
    o = pe + 24 + opt + i * 40
    va, rs, pr = struct.unpack_from('<III', d, o + 12)
    secs.append((va, rs, pr))
def off(rva):
    for va, rs, pr in secs:
        if va <= rva < va + rs:
            return pr + rva - va
    return None
e = off(edir_rva)
n = struct.unpack_from('<I', d, e + 24)[0]
fn_rva, name_rva, ord_rva = struct.unpack_from('<III', d, e + 28)
info_rva = None
names = []
for i in range(n):
    r = struct.unpack_from('<I', d, off(name_rva) + 4 * i)[0]
    o = off(r)
    nm = d[o:d.index(b'\0', o)].decode()
    names.append(nm)
    if nm == '__wine_com_thunk_info':
        idx = struct.unpack_from('<H', d, off(ord_rva) + 2 * i)[0]
        info_rva = struct.unpack_from('<I', d, off(fn_rva) + 4 * idx)[0]
if info_rva is None:
    print("0 0 %d" % len(names)); raise SystemExit
io = off(info_rva)
ver, ic, stride, trap, ifr = struct.unpack_from('<IIIII', d, io)
slots = 0
for i in range(ic):
    slots += struct.unpack_from('<I', d, off(ifr) + i * 24 + 16)[0]
print("%d %d %d" % (ic, slots, len(names)))
EOF
}

check_guest_module() {   # <label> <dll> <want ifaces> <want slots>
    lbl=$1; dll=$2; wi=$3; ws=$4
    [ -f "$dll" ] || { bad "no guest thunk at $dll; build it first"; return; }
    set -- $(com_counts "$dll") || { bad "cannot read $dll"; return; }
    if [ "$1" = "$wi" ] && [ "$2" = "$ws" ]; then
        say "thunk($lbl): $1 interface(s), $2 vtable slot(s), $3 export(s)"
    else
        bad "$lbl publishes $1 interfaces / $2 slots, the roster says $wi / $ws"
    fi
}
check_guest_module dsound    "$DS_GUEST" 21 202
# 10/133 rather than 8/115 since IXAPO and IXAPOParameters joined the roster,
# which is what lets the three XAPO factories be served instead of excluded.
check_guest_module xaudio2_9 "$XA_GUEST" 10 133
# xaudio2_8 is the SAME roster shape as 2_9 -- 10 interfaces, 133 slots -- and
# that is the point of it having its own generated pair rather than sharing
# 2_9's: what differs between the versions is IXAudio2's IID and three of its
# method signatures, neither of which changes the counts.  Equal numbers here
# are therefore a real check that both were generated, not a tautology.
check_guest_module xaudio2_8 "$XA8_GUEST" 10 133

# The one-line regression: x3daudio1_7 exported NOTHING until x3daudio.h
# reached the thunk generator's signature oracle, so every SkyrimSE import of
# it bound to a 0xdead0000 sentinel.
if [ -f "$X3D_GUEST" ]; then
    set -- $(com_counts "$X3D_GUEST")
    if [ "${3:-0}" -ge 3 ]; then
        say "thunk(x3daudio1_7): $3 export(s) -- X3DAudioInitialize and \
X3DAudioCalculate are present"
    else
        bad "the guest x3daudio1_7 exports only ${3:-0} name(s); it must carry \
X3DAudioInitialize and X3DAudioCalculate as well as __wine_thunk_info"
    fi
else
    bad "no guest thunk at $X3D_GUEST"
fi

# ---- C: a null sink of this gate's own -------------------------------------
SINK_MODULE=$(pactl load-module module-null-sink sink_name="$SINK_NAME" \
    sink_properties=device.description="$SINK_NAME" 2>"$OUT/sink.err") \
    || { sed 's/^/  pactl| /' "$OUT/sink.err" >&2
         skip "cannot create a null sink; this gate will not open one it did \
not create" ; }
say "sink: $SINK_NAME (module $SINK_MODULE), unloaded on the way out; the \
default sink is untouched and no existing sink is opened"

# ---- D: a display of this gate's own ---------------------------------------
for n in 81 82 83 84 85 86 87 88 89; do
    [ -e "/tmp/.X11-unix/X$n" ] && continue
    XVFB_DISPLAY=":$n"
    break
done
[ -n "$XVFB_DISPLAY" ] || skip "no free display number in :81..:89"
Xvfb "$XVFB_DISPLAY" -screen 0 320x240x24 > "$OUT/xvfb.log" 2>&1 &
XVFB_PID=$!
n=0
while [ ! -e "/tmp/.X11-unix/X${XVFB_DISPLAY#:}" ]; do
    n=$((n + 1))
    [ "$n" -gt 100 ] && break
    sleep 0.1
done
[ -e "/tmp/.X11-unix/X${XVFB_DISPLAY#:}" ] || {
    sed 's/^/  xvfb| /' "$OUT/xvfb.log" >&2
    skip "Xvfb did not come up on $XVFB_DISPLAY"
}
say "display: Xvfb on $XVFB_DISPLAY (pid $XVFB_PID); the user's own are untouched"

WDBG=${WINEDEBUG:--all}
run_wine() {   # run_wine <exe> <stdout> <stderr> [env assignments...]
    exe=$1; o=$2; e=$3; shift 3
    timeout -k 5 "$TIMEOUT" env DISPLAY="$XVFB_DISPLAY" WINEDEBUG="$WDBG" \
        WINEDLLOVERRIDES="winedbg.exe=d" PULSE_SINK="$SINK_NAME" \
        "$@" "$BUILD/wine" "$exe" > "$o" 2> "$e"
}

# ---- build: the native ppc64 PE leg ----------------------------------------
# Built exactly the way ppc64le/opengl/check-gl-smoke.sh builds its native
# lane: an ordinary consumer of the public headers, linked against this tree's
# own ppc64-windows import libraries and turned into a builtin PE.  It runs
# under the SAME wine as the guest leg, so both reach the same dsound.
native_build() {   # native_build <probe> <define> <output> [extra cflags...]
    probe=$1; def=$2; nout=$3; shift 3
    ${CC:-gcc} -c -o "$OUT/native.o" "$HERE/probes/$probe" $INCL \
        -D"$def" "$@" \
        -D_UCRT -D_WIN32 -Wall -pipe -mlongcall -mno-pltseq -fcf-protection=none \
        -fvisibility=hidden -fno-stack-protector -fno-strict-aliasing -gdwarf-4 \
        -fPIC -fasynchronous-unwind-tables -mlong-double-64 -fno-builtin \
        -fshort-wchar -Wno-format -g -O1 2>"$OUT/native.build.err" || return 1
    "$BUILD/tools/winegcc/winegcc" -o "$nout" --wine-objdir "$BUILD" \
        --cc-cmd="${CC:-gcc}" -mno-cygwin -fPIC -fasynchronous-unwind-tables \
        -Wl,--wine-builtin -mconsole "$OUT/native.o" \
        "$BUILD/dlls/dsound/ppc64-windows/libdsound.a" \
        "$BUILD/dlls/${XA_MODULE:-xaudio2_9}/ppc64-windows/lib${XA_MODULE:-xaudio2_9}.a" \
        "$BUILD/dlls/winmm/ppc64-windows/libwinmm.a" \
        "$BUILD/dlls/user32/ppc64-windows/libuser32.a" \
        "$BUILD/libs/winecrt0/ppc64-windows/libwinecrt0.a" \
        "$BUILD/dlls/ucrtbase/ppc64-windows/libucrtbase.a" \
        "$BUILD/dlls/kernel32/ppc64-windows/libkernel32.a" \
        "$BUILD/dlls/ntdll/ppc64-windows/libntdll.a" \
        2>>"$OUT/native.build.err" || return 1
    rm -f "$nout"
    "$SRC/tools/elf2pe" "$nout.so" "$nout" 2>>"$OUT/native.build.err" || return 1
    "$BUILD/tools/winebuild/winebuild" --builtin "$nout" \
        2>>"$OUT/native.build.err" || return 1
    return 0
}

# ---- build: the x86-64 guest PE leg ----------------------------------------
# The same clang x86_64-windows-gnu machinery check-gl-smoke.sh drives its
# guest build with, and the same Wine headers, so any disagreement between the
# two legs is the boundary and not the declarations.  The imports are described
# by hand, naming only what the probes call: the guest binds to the same
# builtins a real application would and there is no CRT at all.
cat > "$OUT/kernel32.def" <<'EOF'
LIBRARY kernel32.dll
EXPORTS
GetStdHandle
WriteFile
ExitProcess
CreateEventW
WaitForSingleObject
SetEvent
CloseHandle
GetTickCount
Sleep
QueryPerformanceCounter
QueryPerformanceFrequency
EOF
cat > "$OUT/user32.def" <<'EOF'
LIBRARY user32.dll
EXPORTS
GetDesktopWindow
EOF
cat > "$OUT/dsound.def" <<'EOF'
LIBRARY dsound.dll
EXPORTS
DirectSoundCreate8
EOF
cat > "$OUT/xaudio2_9.def" <<'EOF'
LIBRARY xaudio2_9.dll
EXPORTS
XAudio2Create
; The XAPO factory xa_smoke.c steps 20-22 drive.  It used to be EXCLUDEd
; from the guest thunk, so importing it would have bound a sentinel.
; (a .def comment is ";", not "#" -- llvm-dlltool rejects the latter)
CreateAudioReverb
EOF
cat > "$OUT/xaudio2_8.def" <<'EOF'
LIBRARY xaudio2_8.dll
EXPORTS
XAudio2Create
CreateAudioReverb
EOF
cat > "$OUT/winmm.def" <<'EOF'
LIBRARY winmm.dll
EXPORTS
waveOutGetNumDevs
waveOutGetDevCapsW
waveOutOpen
waveOutPrepareHeader
waveOutWrite
waveOutUnprepareHeader
waveOutReset
waveOutClose
waveOutGetPosition
PlaySoundW
EOF
for m in kernel32 user32 dsound xaudio2_9 xaudio2_8 winmm; do
    llvm-dlltool -m i386:x86-64 -d "$OUT/$m.def" -l "$OUT/lib$m.a" \
        || skip "llvm-dlltool failed for $m"
done

GUESTCC="clang -target x86_64-windows-gnu -nostdlibinc $INCL -Wall -O1 \
-fno-builtin -g"
GUESTLD="clang -target x86_64-windows-gnu -fuse-ld=lld -nostdlib \
-Wl,--subsystem,console"

guest_build() {   # guest_build <probe> <entry> <output> <extra libs...>
    probe=$1; entry=$2; gout=$3; shift 3
    # GUEST_EXTRA is how the xaudio2_8 leg points the same source at that
    # module's own widl header; empty for every other caller.
    $GUESTCC ${GUEST_EXTRA:-} -c -o "$OUT/guest.o" "$HERE/probes/$probe" \
        2>"$OUT/guest.build.err" || return 1
    $GUESTLD -Wl,--entry="$entry" -o "$gout" "$OUT/guest.o" "$@" \
        "$OUT/libuser32.a" "$OUT/libkernel32.a" 2>>"$OUT/guest.build.err" || return 1
    return 0
}

# ---- (also standalone as --sabotage): the negative controls -----------------
sabotage() {
    ok=1

    guest_build ds_smoke.c ds_smoke_entry "$OUT/ds_guest.exe" "$OUT/libdsound.a" \
        || { echo "guest ds build failed" >&2; return 1; }
    guest_build xa_smoke.c xa_smoke_entry "$OUT/xa_guest.exe" "$OUT/libxaudio2_9.a" \
        || { echo "guest xa build failed" >&2; return 1; }

    # 1: the raw-pointer failure mode.  Every interface pointer crosses to the
    # guest as the NATIVE host pointer, so the guest's first method call runs
    # ppc64 bytes as x86-64.  Neither probe may PASS.
    for p in ds xa; do
        run_wine "$OUT/${p}_guest.exe" "$OUT/sab_$p.out" "$OUT/sab_$p.err" \
            WINEEMUNOCOMWRAP=1
        if grep -q "${p}_smoke: PASS" "$OUT/sab_$p.out"; then
            echo "check-audio-smoke: FAIL WINEEMUNOCOMWRAP=1 still PASSED for \
$p -- the gate cannot go red" >&2; ok=0
        else
            echo "check-audio-smoke: sabotage(nocomwrap,$p): did not pass, as \
it must"
        fi
    done

    # 2: each falsification build of the NATIVE leg must fail.
    for b in 1 2 3; do
        native_build ds_smoke.c DS_SMOKE_NATIVE "$OUT/ds_break$b.exe" \
            -DDS_SMOKE_BREAK=$b || {
            echo "check-audio-smoke: FAIL DS_SMOKE_BREAK=$b did not build" >&2
            ok=0; continue; }
        run_wine "$OUT/ds_break$b.exe" "$OUT/ds_break$b.out" "$OUT/ds_break$b.err"
        if grep -q "ds_smoke: PASS" "$OUT/ds_break$b.out"; then
            echo "check-audio-smoke: FAIL DS_SMOKE_BREAK=$b still PASSED" >&2
            ok=0
        else
            echo "check-audio-smoke: sabotage(ds_break=$b): failed, as it must"
        fi
    done
    for b in 1 2 3 4 5; do
        native_build xa_smoke.c XA_SMOKE_NATIVE "$OUT/xa_break$b.exe" \
            -DXA_SMOKE_BREAK=$b || {
            echo "check-audio-smoke: FAIL XA_SMOKE_BREAK=$b did not build" >&2
            ok=0; continue; }
        run_wine "$OUT/xa_break$b.exe" "$OUT/xa_break$b.out" "$OUT/xa_break$b.err"
        if grep -q "xa_smoke: PASS" "$OUT/xa_break$b.out"; then
            echo "check-audio-smoke: FAIL XA_SMOKE_BREAK=$b still PASSED" >&2
            ok=0
        else
            echo "check-audio-smoke: sabotage(xa_break=$b): failed, as it must"
        fi
    done
    for b in 1 2; do
        native_build mm_smoke.c MM_SMOKE_NATIVE "$OUT/mm_break$b.exe" \
            -DMM_SMOKE_BREAK=$b || {
            echo "check-audio-smoke: FAIL MM_SMOKE_BREAK=$b did not build" >&2
            ok=0; continue; }
        run_wine "$OUT/mm_break$b.exe" "$OUT/mm_break$b.out" "$OUT/mm_break$b.err"
        if grep -q "mm_smoke: PASS" "$OUT/mm_break$b.out"; then
            echo "check-audio-smoke: FAIL MM_SMOKE_BREAK=$b still PASSED" >&2
            ok=0
        else
            echo "check-audio-smoke: sabotage(mm_break=$b): failed, as it must"
        fi
    done

    [ "$ok" = 1 ] && echo "check-audio-smoke: SABOTAGE PASS (every control red)"
    return $((1 - ok))
}

if [ "$SABOTAGE" = 1 ]; then
    sabotage || fail=1
    cleanup
    exit $fail
fi

# ---- E-H: DirectSound ------------------------------------------------------
run_leg() {   # run_leg <label> <probe> <entry> <native define> <guest lib> <tag>
    lbl=$1; probe=$2; entry=$3; ndef=$4; glib=$5; tag=$6

    native_build "$probe" "$ndef" "$OUT/${lbl}_native.exe" || {
        tail -20 "$OUT/native.build.err" >&2
        skip "the native ppc64 build of $probe failed"; }
    guest_build "$probe" "$entry" "$OUT/${lbl}_guest.exe" "$glib" || {
        tail -20 "$OUT/guest.build.err" >&2
        skip "the x86-64 guest build of $probe failed"; }

    run_wine "$OUT/${lbl}_native.exe" "$OUT/${lbl}_native.out" "$OUT/${lbl}_native.err"
    if grep -q "$tag: PASS" "$OUT/${lbl}_native.out"; then
        say "native($lbl): $(grep -m1 "$tag:" "$OUT/${lbl}_native.out")"
    else
        sed 's/^/  native| /' "$OUT/${lbl}_native.out" >&2
        tail -10 "$OUT/${lbl}_native.err" | sed 's/^/  native!| /' >&2
        bad "the NATIVE ppc64 $lbl leg did not pass -- nothing below is about \
the guest boundary"
    fi

    run_wine "$OUT/${lbl}_guest.exe" "$OUT/${lbl}_guest.out" "$OUT/${lbl}_guest.err"
    if grep -q "$tag: PASS" "$OUT/${lbl}_guest.out"; then
        say "guest($lbl): $(grep -m1 "$tag:" "$OUT/${lbl}_guest.out")"
    else
        sed 's/^/  guest| /' "$OUT/${lbl}_guest.out" >&2
        tail -20 "$OUT/${lbl}_guest.err" | sed 's/^/  guest!| /' >&2
        bad "the x86-64 GUEST $lbl leg did not pass"
    fi

    if cmp -s "$OUT/${lbl}_native.out" "$OUT/${lbl}_guest.out"; then
        say "identity($lbl): the two transcripts are byte-identical \
($(wc -c < "$OUT/${lbl}_native.out") bytes, \
$(grep -c '^step ' "$OUT/${lbl}_native.out") checked steps)"
    else
        diff "$OUT/${lbl}_native.out" "$OUT/${lbl}_guest.out" | head -20 >&2
        bad "the native and guest $lbl transcripts differ"
    fi
}

run_leg ds ds_smoke.c ds_smoke_entry DS_SMOKE_NATIVE "$OUT/libdsound.a" ds_smoke
run_leg xa xa_smoke.c xa_smoke_entry XA_SMOKE_NATIVE "$OUT/libxaudio2_9.a" xa_smoke
# The SAME probe, the same 23 steps, driven through xaudio2_8 instead.  This is
# not a duplicate of the leg above: 2_8 is a different roster, a different
# IXAudio2 IID and three different method signatures, all reached through a
# separately generated marshal table and a separately generated guest thunk.
# The two legs' transcripts are each diffed against their OWN native control
# rather than against each other, because what is being proved is that each
# version's boundary carries its own version's values -- not that the two
# versions agree, which is a claim about XAudio2 and not about this port.
#
# XA_MODULE swaps the ppc64 import library the native leg links; GUEST_EXTRA
# points the guest compile at dlls/xaudio2_8/xaudio_classes.h, that module's
# own widl output.  Both are unset again afterwards so nothing below inherits
# them.
XA_MODULE=xaudio2_8 \
GUEST_EXTRA="-DXA_SMOKE_V8 -I$BUILD/dlls/xaudio2_8" \
run_leg xa8 xa_smoke.c xa_smoke_entry XA_SMOKE_NATIVE "$OUT/libxaudio2_8.a" xa_smoke
unset XA_MODULE GUEST_EXTRA
run_leg mm mm_smoke.c mm_smoke_entry MM_SMOKE_NATIVE "$OUT/libwinmm.a" mm_smoke

# ---- H: the DirectSound refusal, by name -----------------------------------
# CreateSoundBuffer's aggregation pUnkOuter is a GUEST-implemented IUnknown
# handed to native code, and dsound has no reverse proxy for it -- the native
# leg SUCCEEDS, which is why this is checked here rather than being a diffed
# step: what is under test is that the port says no, by name, instead of
# handing Wine's dsound a pointer into an x86-64 image.
refusal() {   # refusal <leg> <needle> <want hex> <what>
    lg=$1; needle=$2; want=$3; what=$4
    got=$(grep -m1 -- "$needle" "$OUT/${lg}_guest.err" | sed 's/.* //')
    nat=$(grep -m1 -- "$needle" "$OUT/${lg}_native.err" | sed 's/.* //')
    if [ "$got" = "$want" ]; then
        say "refusal($what): guest $got, native ${nat:-none} -- refused by name"
    else
        bad "$what: the guest answered '${got:-nothing}', expected $want \
(native answered '${nat:-none}')"
    fi
}
refusal ds "note: CreateSoundBuffer with a non-NULL pUnkOuter" 0x80040110 \
    "IDirectSound8::CreateSoundBuffer aggregation pUnkOuter"

# ---- M: the mechanism actually attached ------------------------------------
# Identical output through a mechanism that never ran would mean the guest was
# calling native vtables directly, which is the one thing that must be
# impossible.  libs/winecom TRACEs the materialisation with its own counts.
mechanism() {   # mechanism <leg> <exe> <ifaces> <slots>
    lg=$1; exe=$2; wi=$3; ws=$4
    run_wine "$OUT/$exe" "$OUT/${lg}_trace.out" "$OUT/${lg}_trace.err" \
        WINEDEBUG=+winecom
    if grep -q "materialised $ws guest vtable slots across $wi interfaces" \
            "$OUT/${lg}_trace.err"; then
        say "mechanism($lg): $(grep -m1 -o 'materialised .*' "$OUT/${lg}_trace.err")"
    else
        grep -m5 -i "winecom" "$OUT/${lg}_trace.err" | sed 's/^/  trace| /' >&2
        bad "the $lg guest run never materialised $wi interfaces / $ws slots \
of guest vtables -- the proxies did not come from libs/winecom"
    fi
}
mechanism ds ds_guest.exe 21 202
mechanism xa xa_guest.exe 10 133

# ---- L: the reverse-proxy path is SERVED, and the trace proves it ---------
# xa_smoke.c's steps 13-19 already diffed RegisterForCallbacks and
# CreateSourceVoice(pCallback) as ordinary S_OK steps, which proves the CALLS
# succeed but not that they went through libs/winecom/reverse.c rather than
# around it.  The +winecom trace mechanism() just captured (xa_trace.err, from
# the SAME guest run that made the two callbacks fire) is re-examined here for
# the runtime facts only that machinery can produce: a reverse wrap of each
# `[local]` callback interface, and winecom_reverse_dispatch actually
# dispatching IXAudio2VoiceCallback::OnBufferEnd BY NAME -- the one call the
# delivery test depends on.  Identical stdout with none of this in the trace
# would mean the guest was calling a native callback pointer directly.
reverse_served() {   # reverse_served <trace stderr file>
    tr=$1
    ok=1

    if ! grep -q "winecom_reverse_wrap reverse-wrapped guest IXAudio2VoiceCallback" "$tr"
    then
        bad "the guest xa run never reverse-wrapped an IXAudio2VoiceCallback \
-- CreateSourceVoice's pCallback did not take the reverse-proxy path"
        ok=0
    fi
    if ! grep -q "winecom_reverse_wrap reverse-wrapped guest IXAudio2EngineCallback" "$tr"
    then
        bad "the guest xa run never reverse-wrapped an IXAudio2EngineCallback \
-- RegisterForCallbacks did not take the reverse-proxy path"
        ok=0
    fi
    if ! grep -q "winecom_reverse_dispatch IXAudio2VoiceCallback::OnBufferEnd" "$tr"
    then
        bad "the guest xa run's trace never shows \
winecom_reverse_dispatch IXAudio2VoiceCallback::OnBufferEnd -- the delivery \
test's OnBufferEnd was not entered through the reverse dispatcher"
        ok=0
    fi
    if grep -E "refusing the REVERSE call .*(IXAudio2VoiceCallback|IXAudio2EngineCallback)" \
            "$tr" > "$OUT/xa_reverse_refused.err"
    then
        sed 's/^/  refused| /' "$OUT/xa_reverse_refused.err" >&2
        bad "the guest xa run refused a reverse call for a callback \
interface that must be served"
        ok=0
    fi
    # NO REFERENCE MANAGEMENT ON A [local] INTERFACE, EVER.  A voice has no
    # AddRef and no Release -- it is destroyed by DestroyVoice, and its slot 2
    # is SetEffectChain -- so a layer that "drops a surplus reference" on one
    # tears its effect chain off instead.  This module keeps a registry and a
    # guard against exactly that (dlls/xaudio2_9/guestcom.c xaudio2_invoke),
    # and libs/winecom now knows not to ask (host_release_iface, keyed on
    # WINECOM_IF_LOCAL).
    #
    # MEASURED ON DOOM (2016): the combase surface's identical guard fired
    # THREE TIMES on one voice during audio setup, because the intern-hit path
    # in winecom_wrap asked without knowing what it held.  Nothing in any gate
    # forces that intern hit -- every voice this probe creates is wrapped
    # once -- so this check is a REGRESSION GUARD rather than a positive test:
    # it cannot make the path happen, but if the path happens and the answer is
    # wrong, the guard's message is in this trace and this turns it red.
    if grep -q "refusing a reference-drop on voice" "$tr"
    then
        grep "refusing a reference-drop on voice" "$tr" | sed 's/^/  voice| /' >&2
        bad "libs/winecom asked for a reference-drop on a [local] voice \
interface; host_release_iface should never have let that through"
        ok=0
    fi
    [ "$ok" = 1 ] && say "reverse(xa): IXAudio2VoiceCallback and \
IXAudio2EngineCallback both reverse-wrapped, OnBufferEnd dispatched by name, \
no reverse call refused, and no reference-drop attempted on a [local] voice"
}
reverse_served "$OUT/xa_trace.err"

cleanup
[ $fail -eq 0 ] && say "PASS"
exit $fail
