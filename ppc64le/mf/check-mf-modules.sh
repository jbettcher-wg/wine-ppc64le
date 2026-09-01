#!/bin/sh
#
# check-mf-modules.sh -- the RUNTIME gate for mfmediaengine, wmvcore and evr.
#
# Sibling of check-mf-smoke.sh, which proves the mfplat/mf/mfreadwrite lane.
# This one proves the three modules that joined the roster afterwards and that
# ppc64le/mf/README.md recorded, correctly, as "surface built, unexercised":
# no guest had ever driven one.  It builds ppc64le/mf/probes/mf_modules.c
# twice from the one source -- once as a native ppc64 Windows PE, once as an
# x86-64 guest PE -- runs both, and requires their stdout to be
# BYTE-IDENTICAL.
#
# THE NUMBERS ARE ORACLES, NOT TIE-BREAKS.  What the two runs print is not
# "it started": it is network and ready states, error codes, stream and output
# numbers, a media duration in 100ns units, and a 2D video buffer's contiguous
# length and pitch.  Two of those are ARITHMETIC THIS SCRIPT DOES FOR ITSELF
# in python (layer 1) rather than numbers taken from either run, so "the guest
# agrees with the native run" and "both agree with what the media and the
# geometry imply" are separate claims and both are checked.  The rest are
# constants quoted from Wine's own implementation beside each check in the
# probe.
#
# WHAT THIS GATE FOUND THE FIRST TIME IT RAN, which is the argument for
# writing it: mfmediaengine's only door was welded shut.  Its one usable flat
# export is DllGetClassObject, whose wrapper wraps the result by IID, and
# IClassFactory was NOT on the Media Foundation roster -- so the wrapper
# released the class object and answered E_NOINTERFACE, and there was no
# second way in.  Layer 0 below is what keeps the roster and the marshal
# tables honest about that fix.
#
# WHAT IT IS ALSO THE GATE FOR NOW: the SERVED FLOATING-POINT RETURNS.  A COM
# method whose return value travels in XMM0 instead of RAX was served by
# PPC64EC step C and, honestly, disclosed as untested -- the machinery was
# built and named and nothing had ever put a number through it, because the
# only rows that carry one belong to IMFMediaEngine and the port has no
# MediaEngine title to drive.  It turns out not to need one: a fresh engine
# answers GetDefaultPlaybackRate, GetPlaybackRate, GetVolume and GetDuration
# with no media, no device and no audio endpoint, and this gate already
# creates one.  The probe now reads those, round-trips three distinctive bit
# patterns in through the FP argument and out through the FP return, and
# compares RAW BITS -- in the native ppc64 run and the x86-64 guest run both,
# which layer 4 then requires to be byte-identical.  Control d is the lever
# that proves the FP invoker is what carried them.
#
# Seven layers, each of which removes one way of passing by accident:
#
#   0  PROVENANCE: both generators reproduce their committed output.  The
#      roster still describes Wine's headers and dlls/mfplat/mf_marshal.h
#      still describes the roster, so the slot numbers the guest modules' stub
#      arrays imply and the ones the native tables assume cannot have drifted.
#      This matters more here than it did for the smaller surface: six guest
#      modules publish this one roster and an interface inserted in the middle
#      of it renumbers every one that sorts after it.
#   0b ONE INSTANCE: the built import table of all six modules on this surface
#      shows every __wine_com_*/__wine_mf_* helper coming from mfplat.dll and
#      nowhere else.  combase exports the same NAMES for its own winecom
#      instance, so a module that lists combase first in its IMPORTS binds the
#      wrong surface -- and the symptom is a guest holding a raw native vtable,
#      with nothing in the source to see.  wmvcore had exactly that.
#   1  ORACLE: python3 writes the WAV and computes, from the file and from the
#      geometry this script chose, the duration wmvcore must report and the
#      contiguous length and pitch evr's 2D buffer must have.  No decoder and
#      no Media Foundation is involved in producing any of those three.
#   2  NATIVE: the native ppc64 PE runs and reports PASS.  Establishes that
#      Wine's own mfmediaengine/wmvcore/evr behave this way with the guest
#      lane nowhere in the picture.
#   3  GUEST: the x86-64 guest PE runs under the emulator and reports PASS.
#   4  IDENTITY: diff(native, guest) is empty.
#   5  MECHANISM: the +winecom trace of the guest run shows the proxy path
#      being taken, module by module and method by method -- a class object
#      wrapped, a media engine wrapped, a sync reader wrapped, a video sample
#      allocator wrapped, and, in the REVERSE direction, a native proxy minted
#      for the guest's own IMFMediaEngineNotify with EventNotify entering
#      through it.  Without this a guest that somehow reached the right
#      answers natively would still pass layers 2-4.
#
# --sabotage runs the negative controls instead:
#
#   a  WINEEMUNOCOMWRAP=1 makes winecom_wrap hand the guest the RAW native
#      pointer -- and winecom_reverse_wrap hand native code the RAW guest
#      vtable, which is the direction this lane depends on for EventNotify --
#      so the guest run MUST fail.
#   b  a hand-corrupted expected DURATION MUST fail the wmvcore lane, proving
#      that number is compared rather than merely printed.
#   c  a hand-corrupted expected 2D LENGTH MUST fail the evr lane.  It is a
#      separate control from (b) on purpose: a gate that compared one lane's
#      oracle and printed the other's would pass (b) and still be blind.
#   d  WINEEMUNOCOMFP=1 turns every float-bearing slot back into a named
#      refusal, and a refused FP-RETURN row answers 0.0.  The media-engine
#      lane's FP checks are written against 1.0, a quiet NaN and three
#      distinctive bit patterns precisely so that none of them is reachable
#      that way, so the guest run MUST go red under it.  This is the control
#      that makes the FP-return claim a measurement rather than an assertion.
#
# Exit 0 = pass, 1 = a check failed, 2 = could not run (not a pass).
set -u

HERE=$(cd "$(dirname "$0")" && pwd)
SRC=$(cd "$HERE/../.." && pwd)
BUILD=${BUILD:-$SRC}                    # in-tree build by default
OUT=${OUT:-/tmp/mf-modules}
SABOTAGE=0
[ "${1:-}" = "--sabotage" ] && SABOTAGE=1

say()  { echo "check-mf-modules: $*"; }
bad()  { echo "check-mf-modules: FAIL $*" >&2; fail=1; }
skip() { echo "check-mf-modules: $*" >&2; exit 2; }

[ -x "$BUILD/wine" ] || skip "no wine loader at $BUILD/wine"
[ -n "${WINEPREFIX:-}" ] || skip "set WINEPREFIX to a prefix wineboot has run in"
[ -n "${WINEFEXBRIDGE:-}" ] || skip "set WINEFEXBRIDGE to the emulator bridge"
for m in mfplat mfmediaengine wmvcore evr; do
    [ -f "$BUILD/dlls/$m/x86_64-windows/$m.dll" ] || \
        skip "no guest $m thunk; build it first"
done
[ -f "$BUILD/dlls/winegstreamer/winegstreamer.so" ] || \
    skip "no winegstreamer; there is no demux backend for the wmvcore lane"
command -v clang >/dev/null || skip "need clang for the guest build"
command -v llvm-dlltool >/dev/null || skip "need llvm-dlltool for the guest build"
command -v python3 >/dev/null || skip "need python3 to write the media"

mkdir -p "$OUT" || skip "cannot create $OUT"
fail=0

INCL="-I$BUILD/include -I$SRC/include -I$SRC/include/msvcrt"

# ---- 0: provenance -------------------------------------------------------
# Two generators read one roster.  Six guest modules publish it.  If the
# committed roster and the committed marshal tables had drifted apart by one
# interface, a call would land on the neighbouring slot with the neighbour's
# argument types -- and libs/winecom's attach-time IID cross-check is the last
# line of defence, not the first.  This is the first.
if python3 "$HERE/gen_interfaces.py" --build "$BUILD" \
        --check "$HERE/interfaces_mf.json" > "$OUT/roster.log" 2>&1; then
    say "provenance: $(head -1 "$OUT/roster.log")"
else
    cat "$OUT/roster.log" >&2
    bad "the committed roster has drifted from Wine's headers"
fi
if python3 "$HERE/gen_winecom.py" --headers "$BUILD/include" \
        --check "$SRC/dlls/mfplat/mf_marshal.h" > "$OUT/marshal.log" 2>&1; then
    say "provenance: $(head -1 "$OUT/marshal.log")"
else
    cat "$OUT/marshal.log" >&2
    bad "dlls/mfplat/mf_marshal.h has drifted from the roster"
fi

# ---- 0b: one surface, one instance ---------------------------------------
# libs/winecom's proxy state is PER-LINKEE, so the six modules on this surface
# work only because exactly one of them (mfplat) links winecom and the other
# five reach that instance through mfplat's exported __wine_com_* helpers.
# combase exports symbols of the SAME NAMES for its own system-COM instance,
# and the linker binds an undefined reference to whichever import library it
# sees first -- so a module that lists combase before mfplat silently gets the
# wrong surface, and the failure is a guest holding a raw native vtable rather
# than anything that says so.  wmvcore had exactly that (dlls/wmvcore/Makefile.in
# records the measurement).  Reading the built import table is the only place
# this is checkable: it is a property of the LINK, invisible in the source.
python3 - "$BUILD" mfplat mf mfreadwrite mfmediaengine evr wmvcore \
        > "$OUT/imports.log" 2>&1 <<'EOF'
import struct, sys, os

build, modules = sys.argv[1], sys.argv[2:]

def imports(path):
    """{dll: [names]} from a PE's import directory."""
    d = open(path, "rb").read()
    e = struct.unpack_from("<I", d, 0x3c)[0]
    nsec = struct.unpack_from("<H", d, e + 6)[0]
    optsz = struct.unpack_from("<H", d, e + 20)[0]
    magic = struct.unpack_from("<H", d, e + 24)[0]
    dd = e + 24 + (112 if magic == 0x20b else 96)
    impva = struct.unpack_from("<I", d, dd + 8)[0]
    secs = []
    so = e + 24 + optsz
    for i in range(nsec):
        n, vs, va, rs, ro = struct.unpack_from("<8sIIII", d, so + i * 40)
        secs.append((va, vs, ro, rs))
    def r2o(r):
        for va, vs, ro, rs in secs:
            if va <= r < va + max(vs, rs):
                return ro + (r - va)
        return None
    out = {}
    if not impva:
        return out
    o, i = r2o(impva), 0
    while True:
        ilt, ts, fc, nm, iat = struct.unpack_from("<IIIII", d, o + i * 20)
        if not nm:
            break
        q = d[r2o(nm):]
        dll = q[:q.index(b"\x00")].decode()
        t, names = r2o(ilt or iat), []
        while True:
            v = struct.unpack_from("<Q", d, t)[0]
            if not v:
                break
            if not (v >> 63):                  # by name, not by ordinal
                r = d[r2o(v) + 2:]
                names.append(r[:r.index(b"\x00")].decode())
            t += 8
        out.setdefault(dll.lower(), []).extend(names)
        i += 1
    return out

bad = 0
for m in modules:
    path = os.path.join(build, "dlls", m, "ppc64-windows", m + ".dll")
    if not os.path.exists(path):
        print("MISSING %s" % path)
        bad = 1
        continue
    for dll, names in sorted(imports(path).items()):
        helpers = [n for n in names if n.startswith("__wine_com_")
                                    or n.startswith("__wine_mf_")]
        if not helpers:
            continue
        if dll != "mfplat.dll":
            print("WRONG SURFACE: %s.dll binds %s from %s, not mfplat.dll"
                  % (m, ", ".join(sorted(helpers)), dll))
            bad = 1
        else:
            print("ok  %-14s <- mfplat.dll  %s" % (m, " ".join(sorted(helpers))))
print("instance check: %s" % ("FAILED" if bad else "every helper comes from mfplat"))
sys.exit(1 if bad else 0)
EOF
if [ $? = 0 ]; then
    say "one instance: $(tail -1 "$OUT/imports.log")"
else
    grep -v '^ok ' "$OUT/imports.log" >&2
    bad "a module on this surface links a winecom helper from the wrong module"
fi

# ---- 1: the oracle -------------------------------------------------------
# The same 1-second 440 Hz 44100 Hz mono 16-bit sine check-mf-smoke.sh writes,
# for the same reason: it is byte-deterministic, it needs no codec package
# installed, and nothing is read out of the user's game library.  Two numbers
# are computed from it and from the frame geometry chosen here, and both are
# properties of things this script just decided rather than of anything Media
# Foundation reported:
#
#   DURATION  the media's length in 100ns units, straight out of the WAV's own
#             sample count and rate.  winegstreamer's demuxer must agree.
#   2D_LENGTH / 2D_PITCH
#             what dlls/mfplat/buffer.c's create_2d_buffer must produce for an
#             RGB32 frame of this size: the CONTIGUOUS length is the unpadded
#             stride times the height, while the PITCH is that stride rounded
#             up to MF_64_BYTE_ALIGNMENT.  The width is deliberately 50 rather
#             than a multiple of 16, so those two numbers DIFFER (8000 against
#             a 256-byte pitch) and an implementation that confused them would
#             be caught.  The alignment rule is mfplat's, quoted here:
#             ALIGN_SIZE(stride, MF_64_BYTE_ALIGNMENT) with
#             MF_64_BYTE_ALIGNMENT == 0x3f, and the RGB32 stride is
#             (width * 4 + 3) & ~3 (dlls/mfplat/mediatype.c
#             mf_get_stride_for_format, alignment 3 for a 32bpp format).
MEDIA="$OUT/sine1s.wav"
python3 - "$MEDIA" > "$OUT/oracle" <<'EOF' || skip "could not write the media"
import math, struct, sys

RATE, SECS, FREQ, AMP = 44100, 1, 440, 16000
data = b"".join(struct.pack("<h", int(AMP * math.sin(2 * math.pi * FREQ * i / RATE)))
                for i in range(RATE * SECS))
hdr = (b"RIFF" + struct.pack("<I", 36 + len(data)) + b"WAVEfmt " +
       struct.pack("<IHHIIHH", 16, 1, 1, RATE, RATE * 2, 2, 16) +
       b"data" + struct.pack("<I", len(data)))
open(sys.argv[1], "wb").write(hdr + data)

frames = len(data) // 2                       # mono, 16-bit
print("MF_MODULES_DURATION=%d" % (frames * 10000000 // RATE))

WIDTH, HEIGHT = 50, 40
stride = (WIDTH * 4 + 3) & ~3                 # RGB32, 32bpp, alignment 3
pitch  = (stride + 0x3f) & ~0x3f              # MF_64_BYTE_ALIGNMENT
print("MF_MODULES_WIDTH=%d"     % WIDTH)
print("MF_MODULES_HEIGHT=%d"    % HEIGHT)
print("MF_MODULES_2D_LENGTH=%d" % (stride * HEIGHT))
print("MF_MODULES_2D_PITCH=%d"  % pitch)
EOF
. "$OUT/oracle"
export MF_MODULES_DURATION MF_MODULES_WIDTH MF_MODULES_HEIGHT
export MF_MODULES_2D_LENGTH MF_MODULES_2D_PITCH
say "oracle: duration $MF_MODULES_DURATION (100ns) from the WAV; \
${MF_MODULES_WIDTH}x${MF_MODULES_HEIGHT} RGB32 is $MF_MODULES_2D_LENGTH \
contiguous bytes at pitch $MF_MODULES_2D_PITCH -- no decoder involved"

# The probe takes DOS paths; the prefix's Z: is the unix root.  The media
# engine is given a URL and the sync reader a plain path, which is the shape
# each API actually takes.
MF_MODULES_URL="Z:$(echo "$MEDIA" | tr '/' '\\')"
MF_MODULES_PATH="$MF_MODULES_URL"
export MF_MODULES_URL MF_MODULES_PATH

# ---- build 1: the native ppc64 Windows PE ---------------------------------
# The flags are the ones check-mf-smoke.sh uses, which are the ones the tree's
# own Makefile uses for programs/winepath minus -D__WINESRC__: this program is
# not Wine source, it is an ordinary consumer of the public headers, which is
# also the only way the guest build can see them.
#
# NOTE the import list: mfplat and nothing else.  The three modules under test
# are reached with LoadLibraryW/GetProcAddress in BOTH builds -- mfmediaengine
# has no import library at all, and using one path for all three is what keeps
# the two builds executing the same code.
${CC:-gcc} -c -o "$OUT/mf_modules.o" "$HERE/probes/mf_modules.c" $INCL \
    -D_UCRT -D_WIN32 -Wall -pipe -mlongcall -mno-pltseq -fcf-protection=none \
    -fvisibility=hidden -fno-stack-protector -fno-strict-aliasing -gdwarf-4 \
    -fPIC -fasynchronous-unwind-tables -mlong-double-64 -fno-builtin \
    -fshort-wchar -Wno-format -g -O2 || skip "native compile failed"

"$BUILD/tools/winegcc/winegcc" -o "$OUT/mf_modules.exe" --wine-objdir "$BUILD" \
    --cc-cmd="${CC:-gcc}" -mno-cygwin -fPIC -fasynchronous-unwind-tables \
    -Wl,--wine-builtin -mconsole "$OUT/mf_modules.o" \
    "$BUILD/libs/winecrt0/ppc64-windows/libwinecrt0.a" \
    "$BUILD/dlls/mfplat/ppc64-windows/libmfplat.a" \
    "$BUILD/dlls/ole32/ppc64-windows/libole32.a" \
    "$BUILD/dlls/ucrtbase/ppc64-windows/libucrtbase.a" \
    "$BUILD/dlls/kernel32/ppc64-windows/libkernel32.a" \
    "$BUILD/dlls/ntdll/ppc64-windows/libntdll.a" || skip "native link failed"
rm -f "$OUT/mf_modules.exe"
"$SRC/tools/elf2pe" "$OUT/mf_modules.exe.so" "$OUT/mf_modules.exe" \
    || skip "elf2pe failed"
"$BUILD/tools/winebuild/winebuild" --builtin "$OUT/mf_modules.exe" \
    || skip "winebuild --builtin failed"

# ---- build 2: the x86-64 guest PE ----------------------------------------
# The imports are described by hand rather than taken from a mingw sysroot:
# the point of naming the DLL for each symbol is that the guest binds to the
# same builtins a real guest application would, and nothing else is linked in
# at all (there is no CRT here -- see mf_modules.c).
cat > "$OUT/mfplat.def" <<'EOF'
LIBRARY mfplat.dll
EXPORTS
MFStartup
MFShutdown
MFCreateAttributes
MFCreateMediaType
EOF
cat > "$OUT/kernel32.def" <<'EOF'
LIBRARY kernel32.dll
EXPORTS
GetStdHandle
WriteFile
ExitProcess
GetEnvironmentVariableW
CreateEventW
WaitForSingleObject
SetEvent
CloseHandle
GetCurrentThreadId
LoadLibraryW
GetProcAddress
EOF
for m in mfplat kernel32; do
    llvm-dlltool -m i386:x86-64 -d "$OUT/$m.def" -l "$OUT/lib$m.a" \
        || skip "llvm-dlltool failed for $m"
done

clang -target x86_64-windows-gnu -nostdlibinc $INCL \
    -DMF_MODULES_NO_CRT -D_UCRT -Wall -O1 -fno-builtin -g \
    -c -o "$OUT/mf_modules_guest.o" "$HERE/probes/mf_modules.c" \
    || skip "guest compile failed"
clang -target x86_64-windows-gnu -fuse-ld=lld -nostdlib \
    -Wl,--entry=mf_modules_entry -Wl,--subsystem,console \
    -o "$OUT/mf_modules_guest.exe" "$OUT/mf_modules_guest.o" \
    "$OUT/libmfplat.a" "$OUT/libkernel32.a" || skip "guest link failed"

# Bounded, because a run that never returns is a result too: the sabotage
# control hands native code a raw guest vtable and it can spin instead of
# faulting, and an unbounded gate would hang there rather than report red.
# The probe's own waits are bounded well inside this.
TIMEOUT=${TIMEOUT:-300}
run_native() { timeout -k 5 "$TIMEOUT" \
                   env WINEDEBUG=-all "$BUILD/wine" "$OUT/mf_modules.exe" \
                   2>"$OUT/native.err"; }
run_guest()  { timeout -k 5 "$TIMEOUT" \
                   env WINEDEBUG=${1:--all} WINEEMUNOCOMWRAP=${2:-0} \
                   "$BUILD/wine" "$OUT/mf_modules_guest.exe" 2>"$OUT/guest.err"; }

if [ "$SABOTAGE" = 1 ]; then
    # ---- control a: raw interface pointers, both directions ---------------
    # WINEEMUNOCOMWRAP=1 hands the guest the raw native interface pointer AND
    # hands native code the raw guest vtable pointer.  This lane depends on
    # both: the media engine is reached through proxies, and native MF calls
    # the guest's own IMFMediaEngineNotify through a reverse proxy.  The run
    # MUST NOT reach "mf_modules: PASS".
    run_guest +winecom 1 > "$OUT/sabotage.out"
    if grep -q "mf_modules: PASS" "$OUT/sabotage.out"; then
        bad "WINEEMUNOCOMWRAP=1 still PASSED -- the gate cannot go red"
    else
        say "sabotage(rawptr): raw pointers crossing both ways failed the \
guest run at '$(tail -1 "$OUT/sabotage.out" | cut -c1-60)', as they must"
    fi

    # ---- control b: a corrupted duration oracle ---------------------------
    # If the duration were merely printed rather than compared, changing the
    # expected value would change nothing.
    MF_MODULES_DURATION=$(( MF_MODULES_DURATION + 1 )) run_native \
        > "$OUT/sabotage_duration.out"
    if grep -q "mf_modules: PASS" "$OUT/sabotage_duration.out"; then
        bad "a corrupted expected duration still PASSED -- the wmvcore lane's \
number is not actually being compared"
    else
        say "sabotage(duration): a one-unit change to the expected duration \
failed the run, as it must"
    fi

    # ---- control c: a corrupted 2D geometry oracle ------------------------
    # Separate from (b) because it targets a different lane: a gate that
    # compared the duration and printed the buffer geometry would pass (b).
    MF_MODULES_2D_LENGTH=$(( MF_MODULES_2D_LENGTH + 1 )) run_native \
        > "$OUT/sabotage_2d.out"
    if grep -q "mf_modules: PASS" "$OUT/sabotage_2d.out"; then
        bad "a corrupted expected 2D length still PASSED -- the evr lane's \
numbers are not actually being compared"
    else
        say "sabotage(2d): a one-byte change to the expected contiguous \
length failed the run, as it must"
    fi

    # ---- control d: the floating-point invoker off ------------------------
    # WINEEMUNOCOMFP=1 makes every float-bearing slot refuse (libs/winecom's
    # invoke_marshalled fails closed to E_NOTIMPL, the pre-step-C world), and
    # for a slot whose RETURN is floating point that refusal leaves fpret_bits
    # at zero -- so winecom_dispatch writes 0.0 into the whole of XMM0 and
    # every FP return in the media-engine lane answers 0.0.
    #
    # THAT IS EXACTLY WHY THE PROBE'S FP CHECKS ARE WRITTEN AGAINST NON-ZERO
    # VALUES.  1.0 for a fresh engine's rate and volume, a quiet NaN for its
    # duration, and three distinctive bit patterns through the round trips:
    # not one of them is reachable by a refusal, so this control turns the FP
    # RETURN direction red the way control d in check-mf-smoke.sh turns the FP
    # ARGUMENT direction red.  A probe that had settled for GetCurrentTime's
    # 0.0 would pass here and prove nothing, which is the trap this control
    # exists to keep the probe out of.
    timeout -k 5 "$TIMEOUT" env WINEDEBUG=-all WINEEMUNOCOMFP=1 "$BUILD/wine" \
        "$OUT/mf_modules_guest.exe" > "$OUT/sabotage_fp.out" 2>"$OUT/sabotage_fp.err"
    if grep -q "mf_modules: PASS" "$OUT/sabotage_fp.out"; then
        bad "WINEEMUNOCOMFP=1 still PASSED -- the media engine's by-value \
doubles are not crossing through the FP invoker"
    else
        say "sabotage(fp-off): the FP invoker lever failed the guest run at \
'$(grep -m1 FAIL "$OUT/sabotage_fp.out" | cut -c1-72)', as it must"
    fi

    [ $fail -eq 0 ] && say "SABOTAGE PASS (all controls red)"
    exit $fail
fi

# ---- 2: native -----------------------------------------------------------
run_native > "$OUT/native.out"
if grep -q "mf_modules: PASS" "$OUT/native.out"; then
    say "native: $(tail -1 "$OUT/native.out")"
else
    sed 's/^/  native| /' "$OUT/native.out" >&2
    tail -20 "$OUT/native.err" >&2
    bad "the native ppc64 build did not pass"
fi

# ---- 3: guest ------------------------------------------------------------
run_guest > "$OUT/guest.out"
if grep -q "mf_modules: PASS" "$OUT/guest.out"; then
    say "guest:  $(tail -1 "$OUT/guest.out")"
else
    sed 's/^/  guest| /' "$OUT/guest.out" >&2
    tail -20 "$OUT/guest.err" >&2
    bad "the x86-64 guest build did not pass"
fi

# ---- 4: identity ---------------------------------------------------------
if cmp -s "$OUT/native.out" "$OUT/guest.out"; then
    say "identity: native and guest output is byte-identical ($(wc -l \
        < "$OUT/native.out") lines)"
else
    diff "$OUT/native.out" "$OUT/guest.out" >&2
    bad "native and guest output differ"
fi

# ---- 5: mechanism --------------------------------------------------------
# The guest could in principle print the right numbers while calling native
# vtables (that IS the defect), so require the runtime's own trace to show the
# proxy path being taken -- once per module, and by method name.
run_guest +winecom > "$OUT/guest.trace.out"
cmp -s "$OUT/native.out" "$OUT/guest.trace.out" || \
    bad "the traced guest run did not reproduce the untraced one"
for want in "materialised .* guest vtable slots" \
            "wrapped IClassFactory host .* as proxy" \
            "wrapped IMFMediaEngineClassFactory host .* as proxy" \
            "wrapped IMFMediaEngine host .* as proxy" \
            "wrapped IMFMediaError host .* as proxy" \
            "wrapped IWMSyncReader host .* as proxy" \
            "wrapped IWMHeaderInfo host .* as proxy" \
            "wrapped IMFVideoSampleAllocator host .* as proxy" \
            "wrapped IMF2DBuffer host .* as proxy" \
            "winecom_dispatch IClassFactory::CreateInstance" \
            "winecom_dispatch IMFMediaEngine::GetNetworkState" \
            "winecom_dispatch IMFMediaEngine::GetReadyState" \
            "winecom_dispatch IMFMediaEngine::GetDuration" \
            "winecom_dispatch IMFMediaEngine::GetVolume" \
            "winecom_dispatch IMFMediaEngine::SetVolume" \
            "winecom_dispatch IMFMediaEngine::GetPlaybackRate" \
            "winecom_dispatch IMFMediaEngine::SetPlaybackRate" \
            "winecom_dispatch IMFMediaEngine::GetDefaultPlaybackRate" \
            "winecom_dispatch IMFMediaEngine::SetDefaultPlaybackRate" \
            "winecom_dispatch IMFMediaError::GetErrorCode" \
            "winecom_dispatch IMFAttributes::SetUnknown" \
            "winecom_dispatch IWMSyncReader::Open" \
            "winecom_dispatch IWMHeaderInfo::GetAttributeByName" \
            "winecom_dispatch IMFVideoSampleAllocator::AllocateSample" \
            "winecom_dispatch IMF2DBuffer::GetContiguousLength"; do
    if ! grep -qE "$want" "$OUT/guest.err"; then
        bad "no '$want' in the +winecom trace of the guest run"
    fi
done
# And the REVERSE direction, which is what makes a media engine a media engine
# rather than an object that never says anything: the guest's own
# IMFMediaEngineNotify must have been given a NATIVE vtable and native MF must
# have entered EventNotify through it.
for want in "reverse-wrapped guest IMFMediaEngineNotify .* as native proxy" \
            "winecom_reverse_dispatch IMFMediaEngineNotify::EventNotify \(iface"; do
    if ! grep -qE "$want" "$OUT/guest.err"; then
        bad "no '$want' in the +winecom trace of the guest run"
    fi
done
[ $fail -eq 0 ] && say "mechanism: class object, media engine, sync reader, \
header info, sample allocator and 2D buffer all wrapped; methods dispatched \
by name, the FP-return rows among them; and the guest's own notify object \
reverse-wrapped with EventNotify entering through it"

[ $fail -eq 0 ] && say "PASS"
exit $fail
