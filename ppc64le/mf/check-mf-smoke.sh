#!/bin/sh
#
# check-mf-smoke.sh -- the Media Foundation RUNTIME gate.
#
# It builds ppc64le/mf/probes/mf_smoke.c twice from the one source -- once as a
# native ppc64 Windows PE, once as an x86-64 guest PE -- runs both against a
# WAV this script writes itself, and requires their stdout to be
# BYTE-IDENTICAL.  Everything either run prints is a number Wine's own
# mfreadwrite/winegstreamer pipeline computed: sample rate, channel count,
# sample width, decoded byte count, presentation timestamps, and an FNV-1a
# hash of every decoded PCM byte.
#
# THE HASH IS AN ORACLE, NOT A TIE-BREAK.  The media is a 1-second 44.1kHz
# mono 16-bit sine this script generates deterministically with python3 (no
# ffmpeg, so the bytes do not move when a codec package updates, and nothing
# is read out of the user's Steam library), and layer 1 below computes the
# same FNV-1a over the WAV's own data chunk.  So "the guest agrees with the
# native run" and "both agree with the file" are separate claims and both are
# checked -- two runs of the same wrong decoder would still fail.
#
# Seven layers, each of which removes one way of passing by accident:
#
#   0  PROVENANCE: both generators reproduce their committed output.  The
#      roster still describes Wine's headers and dlls/mfplat/mf_marshal.h
#      still describes the roster -- so the slot numbers the guest module's
#      stub arrays imply and the ones the native tables assume cannot have
#      drifted apart since either was last written.
#   1  ORACLE: python3 writes the WAV and computes the expected byte count and
#      hash from its data chunk.  No decoder is involved.
#   2  NATIVE: the native ppc64 PE runs and reports PASS.  Establishes that
#      Wine's MF decodes this file correctly with the guest lane nowhere in
#      the picture.
#   3  GUEST: the x86-64 guest PE runs under the emulator and reports PASS.
#   4  IDENTITY: diff(native, guest) is empty.
#   5  MECHANISM: the +winecom trace of the guest run shows the guest vtables
#      being MATERIALISED, the source reader and the samples being WRAPPED,
#      and the individual methods arriving in winecom_dispatch BY NAME.
#      Without this a guest that somehow reached the right answers natively
#      would still pass layers 2-4.
#   6  REVERSE-PROXY MECHANISM: mf_async_probe.exe hands native MF a
#      guest-implemented IMFAsyncCallback for real -- MFPutWorkItem, an
#      IMFAsyncResult::GetState() identity round trip, and the same round
#      trip again through IMFAttributes::SetUnknown/GetUnknown -- and the
#      probe's own value checks require all of it to not merely return S_OK
#      but to come back as the GUEST'S OWN POINTER.  This layer additionally
#      requires the +winecom trace to show the MECHANISM behind those
#      answers: a reverse proxy actually minted for the guest callback,
#      winecom_reverse_dispatch actually entering Invoke (and GetParameters,
#      if native MF calls it) BY NAME, the round-trip recognising its own
#      reverse proxy rather than minting a fresh wrapper, and the proxy being
#      destroyed on the way out (refcount balance visible in the trace).
#      Without this a probe that got the right answers some other way would
#      still pass on its own say-so.  Guest-only by construction: it measures
#      a boundary a native run does not have.
#
# --sabotage runs the negative controls instead:
#
#   a  WINEEMUNOCOMWRAP=1 makes winecom_wrap hand the guest the RAW native
#      pointer -- the exact defect this runtime exists to fix -- and the guest
#      run MUST then fail.
#   b  a hand-corrupted expected hash MUST fail the value check, proving the
#      decoded bytes are actually compared rather than merely printed.
#   c  WINEEMUNOCOMWRAP=1 also applies to the reverse direction (it hands
#      native code the RAW guest vtable pointer both ways), so mf_async.exe
#      MUST NOT print "mf_async: PASS" under it either -- if it did, the
#      round-trip identity checks above would be passing by accident, because
#      raw pointers trivially compare equal to themselves without any proxy
#      involved at all.
#
# Exit 0 = pass, 1 = a check failed, 2 = could not run (not a pass).
set -u

HERE=$(cd "$(dirname "$0")" && pwd)
SRC=$(cd "$HERE/../.." && pwd)
BUILD=${BUILD:-$SRC}                    # in-tree build by default
OUT=${OUT:-/tmp/mf-smoke}
SABOTAGE=0
[ "${1:-}" = "--sabotage" ] && SABOTAGE=1

say()  { echo "check-mf-smoke: $*"; }
bad()  { echo "check-mf-smoke: FAIL $*" >&2; fail=1; }
skip() { echo "check-mf-smoke: $*" >&2; exit 2; }

[ -x "$BUILD/wine" ] || skip "no wine loader at $BUILD/wine"
[ -n "${WINEPREFIX:-}" ] || skip "set WINEPREFIX to a prefix wineboot has run in"
[ -n "${WINEFEXBRIDGE:-}" ] || skip "set WINEFEXBRIDGE to the emulator bridge"
[ -f "$BUILD/dlls/mfplat/x86_64-windows/mfplat.dll" ] || \
    skip "no guest mfplat thunk; build it first"
[ -f "$BUILD/dlls/mfreadwrite/x86_64-windows/mfreadwrite.dll" ] || \
    skip "no guest mfreadwrite thunk; build it first"
[ -f "$BUILD/dlls/winegstreamer/winegstreamer.so" ] || \
    skip "no winegstreamer; there is no decode backend to reach"
command -v clang >/dev/null || skip "need clang for the guest build"
command -v llvm-dlltool >/dev/null || skip "need llvm-dlltool for the guest build"
command -v python3 >/dev/null || skip "need python3 to write the media"

mkdir -p "$OUT" || skip "cannot create $OUT"
fail=0

INCL="-I$BUILD/include -I$SRC/include -I$SRC/include/msvcrt"

# ---- 0: provenance -------------------------------------------------------
# Two generators read one roster, and if the guest module's stub arrays and
# the native marshal tables disagreed by one interface a call would land on
# the neighbouring slot with the neighbour's argument types.  libs/winecom's
# attach-time IID cross-check is the last line of defence; this is the first.
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

# ---- 1: the oracle -------------------------------------------------------
# A 1-second 440 Hz sine, 44100 Hz, mono, 16-bit PCM -- written here rather
# than taken from anywhere, so the gate owns its own media and the expected
# numbers are properties of a file this script just produced.  Nothing is read
# from the user's game library.
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

h = 2166136261
for b in data:
    h = ((h ^ b) * 16777619) & 0xffffffff
print("MF_SMOKE_RATE=%d"     % RATE)
print("MF_SMOKE_CHANNELS=1")
print("MF_SMOKE_BITS=16")
print("MF_SMOKE_BYTES=%d"    % len(data))
print("MF_SMOKE_FNV=%d"      % h)
EOF
. "$OUT/oracle"
export MF_SMOKE_RATE MF_SMOKE_CHANNELS MF_SMOKE_BITS MF_SMOKE_BYTES MF_SMOKE_FNV
say "oracle: $MF_SMOKE_BYTES bytes of PCM in $MEDIA, FNV-1a $(printf '0x%08X' \
    "$MF_SMOKE_FNV") computed from the file, no decoder involved"

# The probe takes a DOS path; the prefix's Z: is the unix root.
MF_SMOKE_URL="Z:$(echo "$MEDIA" | tr '/' '\\')"
export MF_SMOKE_URL

# ---- build 1: the native ppc64 Windows PE ---------------------------------
# The flags are the ones the tree's own Makefile uses for programs/winepath,
# minus -D__WINESRC__: this program is not Wine source, it is an ordinary
# consumer of the public headers, which is also the only way the guest build
# can see them.
${CC:-gcc} -c -o "$OUT/mf_smoke.o" "$HERE/probes/mf_smoke.c" $INCL \
    -D_UCRT -D_WIN32 -Wall -pipe -mlongcall -mno-pltseq -fcf-protection=none \
    -fvisibility=hidden -fno-stack-protector -fno-strict-aliasing -gdwarf-4 \
    -fPIC -fasynchronous-unwind-tables -mlong-double-64 -fno-builtin \
    -fshort-wchar -Wno-format -g -O2 || skip "native compile failed"

"$BUILD/tools/winegcc/winegcc" -o "$OUT/mf_smoke.exe" --wine-objdir "$BUILD" \
    --cc-cmd="${CC:-gcc}" -mno-cygwin -fPIC -fasynchronous-unwind-tables \
    -Wl,--wine-builtin -mconsole "$OUT/mf_smoke.o" \
    "$BUILD/libs/winecrt0/ppc64-windows/libwinecrt0.a" \
    "$BUILD/dlls/mfplat/ppc64-windows/libmfplat.a" \
    "$BUILD/dlls/mfreadwrite/ppc64-windows/libmfreadwrite.a" \
    "$BUILD/dlls/ole32/ppc64-windows/libole32.a" \
    "$BUILD/dlls/ucrtbase/ppc64-windows/libucrtbase.a" \
    "$BUILD/dlls/kernel32/ppc64-windows/libkernel32.a" \
    "$BUILD/dlls/ntdll/ppc64-windows/libntdll.a" || skip "native link failed"
rm -f "$OUT/mf_smoke.exe"
"$SRC/tools/elf2pe" "$OUT/mf_smoke.exe.so" "$OUT/mf_smoke.exe" \
    || skip "elf2pe failed"
"$BUILD/tools/winebuild/winebuild" --builtin "$OUT/mf_smoke.exe" \
    || skip "winebuild --builtin failed"

# ---- build 2: the x86-64 guest PEs ---------------------------------------
# The imports are described by hand rather than taken from a mingw sysroot:
# the point of naming the DLL for each symbol is that the guest binds to the
# same builtins a real guest application would, and nothing else is linked in
# at all (there is no CRT here -- see mf_smoke.c).
cat > "$OUT/mfplat.def" <<'EOF'
LIBRARY mfplat.dll
EXPORTS
MFStartup
MFShutdown
MFCreateMediaType
MFCreateAttributes
MFPutWorkItem
MFCreateAsyncResult
EOF
cat > "$OUT/mfreadwrite.def" <<'EOF'
LIBRARY mfreadwrite.dll
EXPORTS
MFCreateSourceReaderFromURL
EOF
cat > "$OUT/ole32.def" <<'EOF'
LIBRARY ole32.dll
EXPORTS
PropVariantClear
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
EOF
for m in mfplat mfreadwrite kernel32 ole32; do
    llvm-dlltool -m i386:x86-64 -d "$OUT/$m.def" -l "$OUT/lib$m.a" \
        || skip "llvm-dlltool failed for $m"
done

guest_build() {   # <source> <entry> <exe>
    clang -target x86_64-windows-gnu -nostdlibinc $INCL \
        -DMF_SMOKE_NO_CRT -D_UCRT -Wall -O1 -fno-builtin -g \
        -c -o "$OUT/$2.o" "$1" || return 1
    clang -target x86_64-windows-gnu -fuse-ld=lld -nostdlib \
        -Wl,--entry=$2 -Wl,--subsystem,console \
        -o "$3" "$OUT/$2.o" \
        "$OUT/libmfplat.a" "$OUT/libmfreadwrite.a" "$OUT/libkernel32.a" "$OUT/libole32.a"
}

guest_build "$HERE/probes/mf_smoke.c" mf_smoke_entry "$OUT/mf_smoke_guest.exe" \
    || skip "guest build of mf_smoke.c failed"
guest_build "$HERE/probes/mf_async_probe.c" mf_async_entry "$OUT/mf_async.exe" \
    || skip "guest build of mf_async_probe.c failed"

# Bounded, because a run that never returns is a result too: the sabotage
# control hands the guest a native code pointer and it can spin instead of
# faulting, and an unbounded gate would hang there rather than report red.
TIMEOUT=${TIMEOUT:-180}
run_native() { timeout -k 5 "$TIMEOUT" \
                   env WINEDEBUG=-all "$BUILD/wine" "$OUT/mf_smoke.exe" \
                   2>"$OUT/native.err"; }
run_guest()  { timeout -k 5 "$TIMEOUT" \
                   env WINEDEBUG=${1:--all} WINEEMUNOCOMWRAP=${2:-0} \
                   "$BUILD/wine" "$OUT/mf_smoke_guest.exe" 2>"$OUT/guest.err"; }

if [ "$SABOTAGE" = 1 ]; then
    # ---- control a: raw interface pointers --------------------------------
    # WINEEMUNOCOMWRAP=1 hands the guest the raw native interface pointer, so
    # its first method call executes ppc64 bytes as x86-64.  The run MUST NOT
    # reach "mf_smoke: PASS".
    run_guest +winecom 1 > "$OUT/sabotage.out"
    if grep -q "mf_smoke: PASS" "$OUT/sabotage.out"; then
        bad "WINEEMUNOCOMWRAP=1 still PASSED -- the gate cannot go red"
    else
        say "sabotage(rawptr): raw interface pointers failed the guest run at \
'$(tail -1 "$OUT/sabotage.out" | cut -c1-60)', as they must"
    fi

    # ---- control b: a corrupted oracle ------------------------------------
    # If the decoded bytes were merely printed rather than compared, flipping
    # the expected hash would change nothing.
    MF_SMOKE_FNV=$(( (MF_SMOKE_FNV + 1) & 0xffffffff )) run_native \
        > "$OUT/sabotage_hash.out"
    if grep -q "mf_smoke: PASS" "$OUT/sabotage_hash.out"; then
        bad "a corrupted expected hash still PASSED -- the decoded bytes are \
not actually being compared"
    else
        say "sabotage(oracle): a one-bit change to the expected FNV failed the \
run, as it must"
    fi

    # ---- control c: raw pointers across the REVERSE direction too ---------
    # WINEEMUNOCOMWRAP=1 disables wrapping in BOTH directions: winecom_wrap
    # hands the guest a raw native pointer (control a) and winecom_reverse_wrap
    # hands native code the raw guest vtable pointer (ERR "WINEEMUNOCOMWRAP:
    # handing raw guest %s %p to native code", libs/winecom/reverse.c).  Under
    # that control the GetState/GetUnknown identity checks in mf_async_probe.c
    # would trivially "pass" for the wrong reason -- a raw pointer compares
    # equal to itself with no proxy involved at all -- so the run as a whole
    # MUST NOT reach "mf_async: PASS", or the round-trip checks above are not
    # actually proving the reverse proxy did the work.
    timeout -k 5 "$TIMEOUT" env WINEDEBUG=-all WINEEMUNOCOMWRAP=1 "$BUILD/wine" \
        "$OUT/mf_async.exe" > "$OUT/sabotage_async.out" 2>"$OUT/sabotage_async.err"
    if grep -q "mf_async: PASS" "$OUT/sabotage_async.out"; then
        bad "WINEEMUNOCOMWRAP=1 still PASSED mf_async -- the round-trip \
identity checks are not exercising the reverse proxy"
    else
        say "sabotage(rawptr-reverse): raw guest/native pointers crossing both \
ways failed the async run at \
'$(tail -1 "$OUT/sabotage_async.out" | cut -c1-60)', as they must"
    fi

    # ---- control d: the floating-point invoker off ------------------------
    # WINEEMUNOCOMFP=1 makes every float-bearing slot refuse (fail closed to
    # E_NOTIMPL, the pre-step-C world), so the SetDouble/GetDouble round trip
    # in mf_smoke.c MUST fail its value check and the run MUST NOT reach
    # PASS.  This is what proves the FP invoker is load-bearing rather than
    # the value arriving some other way.
    #
    # THE ARGUMENT DIRECTION ONLY, and it is worth saying which half this is.
    # SetDouble takes a by-value double in XMM1 and answers an HRESULT in RAX;
    # no slot on this gate's surface RETURNS a float, so nothing here touches
    # winecom_dispatch's XMM0 write-back at all.  That half is measured by the
    # sibling gate, which owns the only module with reachable .fpret rows:
    # ppc64le/mf/check-mf-modules.sh drives IMFMediaEngine's on a fresh engine
    # and carries its own control d for this same lever.
    timeout -k 5 "$TIMEOUT" env WINEDEBUG=-all WINEEMUNOCOMFP=1 "$BUILD/wine" \
        "$OUT/mf_smoke_guest.exe" > "$OUT/sabotage_fp.out" 2>"$OUT/sabotage_fp.err"
    if grep -q "mf_smoke: PASS" "$OUT/sabotage_fp.out"; then
        bad "WINEEMUNOCOMFP=1 still PASSED -- the by-value double is not \
crossing through the FP invoker"
    else
        say "sabotage(fp-off): the FP invoker lever failed the guest run at \
'$(tail -1 "$OUT/sabotage_fp.out" | cut -c1-60)', as it must"
    fi

    [ $fail -eq 0 ] && say "SABOTAGE PASS (all controls red)"
    exit $fail
fi

# ---- 2: native -----------------------------------------------------------
run_native > "$OUT/native.out"
if grep -q "mf_smoke: PASS" "$OUT/native.out"; then
    say "native: $(tail -1 "$OUT/native.out")"
else
    sed 's/^/  native| /' "$OUT/native.out" >&2
    tail -20 "$OUT/native.err" >&2
    bad "the native ppc64 build did not pass"
fi

# ---- 3: guest ------------------------------------------------------------
run_guest > "$OUT/guest.out"
if grep -q "mf_smoke: PASS" "$OUT/guest.out"; then
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
# The guest could in principle print the right bytes while calling a native
# vtable (that IS the defect), so require the runtime's own trace to show the
# proxy path being taken, method by method.
run_guest +winecom > "$OUT/guest.trace.out"
cmp -s "$OUT/native.out" "$OUT/guest.trace.out" || \
    bad "the traced guest run did not reproduce the untraced one"
for want in "materialised .* guest vtable slots" \
            "wrapped IMFSourceReader host .* as proxy" \
            "wrapped IMFMediaType host .* as proxy" \
            "wrapped IMFSample host .* as proxy" \
            "wrapped IMFMediaBuffer host .* as proxy" \
            "winecom_dispatch IMFSourceReader::SetStreamSelection" \
            "winecom_dispatch IMFSourceReader::GetNativeMediaType" \
            "winecom_dispatch IMFAttributes::GetGUID" \
            "winecom_dispatch IMFAttributes::GetUINT32" \
            "winecom_dispatch IMFAttributes::SetDouble" \
            "winecom_dispatch IMFAttributes::SetItem" \
            "winecom_dispatch IMFAttributes::GetItem" \
            "winecom_dispatch IMFSourceReader::SetCurrentMediaType" \
            "winecom_dispatch IMFSourceReader::ReadSample" \
            "winecom_dispatch IMFSample::ConvertToContiguousBuffer" \
            "winecom_dispatch IMFMediaBuffer::Lock" \
            "winecom_dispatch IMFSourceReader::GetPresentationAttribute" \
            "winecom_dispatch IMFSourceReader::SetCurrentPosition" \
            "destroying proxy .*\(IMFSourceReader host"; do
    if ! grep -qE "$want" "$OUT/guest.err"; then
        bad "no '$want' in the +winecom trace of the guest run"
    fi
done
[ $fail -eq 0 ] && say "mechanism: guest vtables materialised, reader/type/\
sample/buffer wrapped, methods dispatched by name"

# ---- 6: the reverse-proxy mechanism ---------------------------------------
timeout -k 5 "$TIMEOUT" env WINEDEBUG=+mfplat,+winecom "$BUILD/wine" \
    "$OUT/mf_async.exe" > "$OUT/async.out" 2>"$OUT/async.err"
if grep -q "mf_async: PASS" "$OUT/async.out"; then
    say "async:  $(tail -1 "$OUT/async.out")"
else
    sed 's/^/  async| /' "$OUT/async.out" >&2
    tail -20 "$OUT/async.err" >&2
    bad "the reverse-proxy positive control did not pass"
fi
# The probe's own value checks (S_OK, the GetState/GetUnknown identity round
# trips, the refcount balance) could in principle be satisfied by ANY
# mechanism that happens to hand back the same pointer it was given -- so
# require the runtime's own trace to show the REVERSE PROXY actually doing
# the work, not just the probe agreeing with itself.
for want in "reverse-wrapped guest IMFAsyncCallback .* as native proxy" \
            "winecom_reverse_dispatch IMFAsyncCallback::Invoke \(iface" \
            "wrapped IMFAsyncResult host .* as proxy" \
            "is our reverse proxy for guest .*; returning the guest.s own pointer" \
            "destroying reverse proxy .* guest"; do
    if ! grep -qE "$want" "$OUT/async.err"; then
        bad "no '$want' in the +winecom trace of the reverse-proxy run"
    fi
done
# GetParameters is COUNTED by the probe, not asserted, because whether native
# MF calls it on this path is a fact about MF's own implementation rather
# than about the reverse-proxy mechanism; report what the trace shows rather
# than gating on it either way.
if grep -qE "winecom_reverse_dispatch IMFAsyncCallback::GetParameters \(iface" "$OUT/async.err"; then
    say "async:  native MF also called GetParameters through the reverse proxy"
else
    say "async:  native MF did not call GetParameters on this path (trace-confirmed)"
fi
[ $fail -eq 0 ] && say "async:  the +winecom trace shows a reverse proxy \
minted for the guest callback, Invoke entered through it, a forward proxy \
minted for the IMFAsyncResult inside that same call, the round trip \
recognising its own reverse proxy, and the proxy torn down again"

[ $fail -eq 0 ] && say "PASS"
exit $fail
