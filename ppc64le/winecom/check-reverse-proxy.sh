#!/bin/sh
#
# check-reverse-proxy.sh -- the REVERSE-PROXY MECHANISM gate.
#
# libs/winecom/reverse.c gives a guest-implemented COM object -- an x86-64
# vtable at a guest address -- a NATIVE vtable whose slots marshal ELFv2
# arguments into MS-x64 and enter the guest method through the emulator.  Every
# other gate on this port measures that mechanism through a consumer:
# ppc64le/mf measures it as Media Foundation's async model, ppc64le/audio as an
# XAudio2 buffer callback.  This one measures THE MECHANISM, because a consumer
# only exercises the argument classes it happens to use, and the machinery has
# to be right for the classes it CARRIES.
#
# ppc64le/winecom/probes/reverse_probe.c builds two COM objects in its own
# guest image and hands them to __wine_winecom_reverse_selftest (a native hook,
# dlls/mfplat/mfcom.c; see include/wine/winecom_selftest.h for why a test hook
# lives in a shipping module).  The hook calls one method of every argument
# class: an integer, a wide integer, a BY-VALUE DOUBLE, a BY-VALUE FLOAT, a
# string, an interface IN, an interface OUT through a REFIID, an integer OUT,
# and -- deliberately -- one method the marshal tables REFUSE.  Both sides
# check: the hook checks what came back to it, the probe checks what arrived.
#
# Five layers, each removing one way of passing by accident:
#
#   0  PROVENANCE.  dlls/mfplat/mf_marshal.h still reproduces from the roster,
#      and the specific rows this gate depends on still say what it assumes:
#      SetDouble and SetMasterVolume carry WINECOM_F_REV with the right fpmask
#      and fpwide, SetUnknown carries an xaux interface type AND the xmask bit
#      that says the generator wrote it, SetItem is HAND-SERVED (the
#      2026-09-01 PROPVARIANT pass).  Without
#      this the gate could pass while measuring rows that had quietly changed
#      shape underneath it.
#   1  MECHANISM.  The probe runs and reports PASS -- every value checked on
#      both sides, both round trips, identity, refusal, reference balance.
#   2  TRACE.  The +winecom trace shows the reverse path actually being taken,
#      method by method BY NAME, so a run that somehow reached the right
#      answers without crossing the boundary would still fail.  This includes
#      the forward proxy minted INSIDE a reverse call, which is the one place
#      the two directions meet.
#   3  REFUSAL.  The trace shows the refused row refused, in the reverse
#      direction, with its reason.
#   4  BALANCE.  The trace shows the reverse proxy destroyed and the guest's
#      Release entered, which is the reference contract closing.
#
# --sabotage runs three negative controls instead, and all three must go red:
#
#   a  WINEEMUNOCOMWRAP=1 hands pointers across RAW in BOTH directions -- the
#      guest object reaches native code as x86-64 bytes to call.  The probe
#      must not PASS.
#   b  WINEEMUNOCBWRAP=1 makes the guest-callback trampoline pool hand back the
#      raw x86-64 shim address, so there is nothing to enter guest code with.
#      The mechanism must refuse to arm itself, loudly, and the probe must not
#      PASS.  This control is specific to this gate: it is the one that proves
#      the crossing really goes through the emulator.
#   c  a probe compiled with a corrupted expected value must fail its own value
#      check, proving the arguments are COMPARED and not merely printed.
#
# Exit 0 = pass, 1 = a check failed, 2 = could not run (not a pass).
set -u

HERE=$(cd "$(dirname "$0")" && pwd)
SRC=$(cd "$HERE/../.." && pwd)
BUILD=${BUILD:-$SRC}                    # in-tree build by default
OUT=${OUT:-/tmp/winecom-reverse}
SABOTAGE=0
[ "${1:-}" = "--sabotage" ] && SABOTAGE=1

say()  { echo "check-reverse-proxy: $*"; }
bad()  { echo "check-reverse-proxy: FAIL $*" >&2; fail=1; }
skip() { echo "check-reverse-proxy: $*" >&2; exit 2; }

[ -x "$BUILD/wine" ] || skip "no wine loader at $BUILD/wine"
[ -n "${WINEPREFIX:-}" ] || skip "set WINEPREFIX to a prefix wineboot has run in"
[ -n "${WINEFEXBRIDGE:-}" ] || skip "set WINEFEXBRIDGE to the emulator bridge"
[ -f "$BUILD/dlls/mfplat/x86_64-windows/mfplat.dll" ] || \
    skip "no guest mfplat thunk; build it first"
[ -f "$BUILD/dlls/mfplat/ppc64-windows/mfplat.dll" ] || \
    skip "no native mfplat; build it first"
command -v clang >/dev/null || skip "need clang for the guest build"
command -v llvm-dlltool >/dev/null || skip "need llvm-dlltool for the guest build"
command -v python3 >/dev/null || skip "need python3 for the provenance layer"

mkdir -p "$OUT" || skip "cannot create $OUT"
fail=0

INCL="-I$BUILD/include -I$SRC/include -I$SRC/include/msvcrt"
MARSHAL="$SRC/dlls/mfplat/mf_marshal.h"

# ---- 0: provenance -------------------------------------------------------
if python3 "$SRC/ppc64le/mf/gen_winecom.py" --headers "$BUILD/include" \
        --check "$MARSHAL" > "$OUT/marshal.log" 2>&1; then
    say "provenance: $(head -1 "$OUT/marshal.log")"
else
    cat "$OUT/marshal.log" >&2
    bad "dlls/mfplat/mf_marshal.h has drifted from the roster"
fi

# The rows this gate stands on.  Each is a one-line assertion about a shape the
# probe depends on, and each would otherwise be able to change underneath it.
#
# The expected text now runs to the NARROWING masks that close each row, and
# that is deliberate rather than incidental.  struct winecom_slot grew
# narrowmask/narrowwide/narrowsign when a WORD argument was found crossing with
# its upper bits undefined (ppc64le/mf/README.md records the measurement), and
# every row below is a slot whose by-value parameter is a float or a double --
# never a narrow integer -- so all three masks MUST be zero on all three.  A
# generator that ever set one here would be claiming a double is 8 or 16 bits
# wide, so pinning the zeros is a real assertion and not padding.
row_is() {   # <grep-pattern> <what it must contain> <description>
    if grep -A3 -F "$1" "$MARSHAL" | grep -qF "$2"; then
        say "row: $3"
    else
        bad "$3 -- expected '$2' near '$1' in mf_marshal.h"
    fi
}
# Since PPC64EC step C these two rows are FORWARD-served too (refuse NULL,
# the same fpmask/fpwide, an appended .fpret) -- the reverse contract this
# gate proves is unchanged, and the masks it pins are the same masks.
row_is '{ "IMFAttributes::SetDouble"' \
       'WINECOM_F_REV, 0, 0, NULL, 0x02, 0x02, 0x00, 0x00, 0x00, 0x00, .fpret = 0 }' \
       "IMFAttributes::SetDouble is reverse-servable AND forward-served, its \
double in the second parameter position"
row_is '{ "IMFSimpleAudioVolume::SetMasterVolume"' \
       'WINECOM_F_REV, 0, 0, NULL, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, .fpret = 0 }' \
       "IMFSimpleAudioVolume::SetMasterVolume is reverse-servable AND \
forward-served, its SINGLE in the first"
row_is '{ "IMFAttributes::SetUnknown", NULL, cls_IMFAttributes_27' \
       'xaux_IMFAttributes_27, 3, 0, 0, 0, NULL, 0, 0, 0x02, 0x00, 0x00, 0x00 }' \
       "IMFAttributes::SetUnknown records the interface TYPE of its IN \
parameter AND the xmask bit that says the generator wrote it -- without the \
bit an untouched zero would read as roster index 0, a real interface"
row_is '{ "IMFAttributes::SetItem",' \
       'WINECOM_F_HAND' \
       "IMFAttributes::SetItem is HAND-SERVED (the 2026-09-01 PROPVARIANT \
completeness pass -- the per-tag walker in dlls/mfplat/mfcom.c); a row that \
reads any other way means the mf tables changed shape under this gate"

# ---- build: the x86-64 guest probe ---------------------------------------
# The imports are described by hand rather than taken from a mingw sysroot: the
# point of naming the DLL for each symbol is that the guest binds to the same
# builtins a real guest application would, and nothing else is linked in at all
# (there is no CRT here -- see reverse_probe.c).
cat > "$OUT/mfplat.def" <<'EOF'
LIBRARY mfplat.dll
EXPORTS
MFStartup
MFShutdown
MFCreateAttributes
__wine_winecom_reverse_selftest
__wine_winecom_reverse_nest
EOF
cat > "$OUT/kernel32.def" <<'EOF'
LIBRARY kernel32.dll
EXPORTS
GetStdHandle
WriteFile
ExitProcess
GetCurrentThreadId
GetTickCount
EOF
for m in mfplat kernel32; do
    llvm-dlltool -m i386:x86-64 -d "$OUT/$m.def" -l "$OUT/lib$m.a" \
        || skip "llvm-dlltool failed for $m"
done

guest_build() {   # <exe> [extra cflags...]
    exe=$1; shift
    clang -target x86_64-windows-gnu -nostdlibinc $INCL \
        -DREVERSE_PROBE_NO_CRT -D_UCRT -Wall -O1 -fno-builtin -g "$@" \
        -c -o "$OUT/probe.o" "$HERE/probes/reverse_probe.c" || return 1
    clang -target x86_64-windows-gnu -fuse-ld=lld -nostdlib \
        -Wl,--entry=reverse_probe_entry -Wl,--subsystem,console \
        -o "$exe" "$OUT/probe.o" "$OUT/libmfplat.a" "$OUT/libkernel32.a"
}

guest_build "$OUT/reverse_probe.exe" || skip "guest build of reverse_probe.c failed"

# Bounded, because a run that never returns is a result too: the sabotage
# controls hand raw code pointers across and a run can spin instead of faulting.
TIMEOUT=${TIMEOUT:-180}
run_probe() {   # <exe> <WINEDEBUG> <WINEEMUNOCOMWRAP> <WINEEMUNOCBWRAP> <errfile>
    timeout -k 5 "$TIMEOUT" env WINEDEBUG="$2" WINEEMUNOCOMWRAP="$3" \
        WINEEMUNOCBWRAP="$4" "$BUILD/wine" "$1" 2>"$5"
}

if [ "$SABOTAGE" = 1 ]; then
    # ---- control a: raw pointers, both directions -------------------------
    run_probe "$OUT/reverse_probe.exe" +winecom 1 0 "$OUT/sab_a.err" \
        > "$OUT/sab_a.out"
    if grep -q "reverse_probe: PASS" "$OUT/sab_a.out"; then
        bad "WINEEMUNOCOMWRAP=1 still PASSED -- the gate cannot go red"
    else
        say "sabotage(rawptr): raw pointers failed the run at \
'$(tail -1 "$OUT/sab_a.out" | cut -c1-60)', as they must"
    fi

    # ---- control b: no guest-callback trampoline --------------------------
    # The mechanism enters guest code through the trampoline pool.  Take the
    # pool away and there is nothing to enter with; arming must fail rather
    # than call the x86-64 shim as ppc64.
    run_probe "$OUT/reverse_probe.exe" +winecom 0 1 "$OUT/sab_b.err" \
        > "$OUT/sab_b.out"
    if grep -q "reverse_probe: PASS" "$OUT/sab_b.out"; then
        bad "WINEEMUNOCBWRAP=1 still PASSED -- the crossing is not going \
through the emulator's guest-callback pool"
    elif grep -q "returned the raw shim address" "$OUT/sab_b.err"; then
        say "sabotage(nocbwrap): the mechanism refused to arm itself and said \
why, as it must"
    else
        say "sabotage(nocbwrap): the run failed without PASSing, but did not \
name the reason; check $OUT/sab_b.err"
    fi

    # ---- control c: a corrupted expected value ----------------------------
    # If the arguments were merely printed rather than compared, changing what
    # the guest expects would change nothing.
    guest_build "$OUT/reverse_probe_bad.exe" \
        -DWINECOM_ST_UINT32=0xDEADBEEFu || skip "sabotage build failed"
    run_probe "$OUT/reverse_probe_bad.exe" -all 0 0 "$OUT/sab_c.err" \
        > "$OUT/sab_c.out"
    if grep -q "reverse_probe: PASS" "$OUT/sab_c.out"; then
        bad "a corrupted expected value still PASSED -- the arguments are not \
actually being compared"
    else
        say "sabotage(oracle): a one-constant change failed the value check, \
as it must"
    fi

    [ $fail -eq 0 ] && say "SABOTAGE PASS (all three controls red)"
    exit $fail
fi

# ---- 1: the mechanism ----------------------------------------------------
run_probe "$OUT/reverse_probe.exe" -all 0 0 "$OUT/probe.err" > "$OUT/probe.out"
if grep -q "reverse_probe: PASS" "$OUT/probe.out"; then
    sed 's/^/  probe| /' "$OUT/probe.out"
else
    sed 's/^/  probe| /' "$OUT/probe.out" >&2
    tail -20 "$OUT/probe.err" >&2
    bad "the reverse-proxy mechanism probe did not pass"
fi

# ---- 2: the trace --------------------------------------------------------
# The probe could in principle print the right values while never crossing the
# boundary, so require the runtime's own trace to show the reverse path being
# taken, method by method.
run_probe "$OUT/reverse_probe.exe" +winecom 0 0 "$OUT/trace.err" \
    > "$OUT/trace.out"
cmp -s "$OUT/probe.out" "$OUT/trace.out" || \
    bad "the traced run did not reproduce the untraced one"
for want in "reverse proxies armed" \
            "reverse-wrapped guest IMFAttributes .* as native proxy" \
            "reverse-wrapped guest IMFSimpleAudioVolume .* as native proxy" \
            "winecom_reverse_dispatch IMFAttributes::SetUINT32" \
            "winecom_reverse_dispatch IMFAttributes::SetUINT64" \
            "winecom_reverse_dispatch IMFAttributes::SetDouble" \
            "winecom_reverse_dispatch IMFSimpleAudioVolume::SetMasterVolume" \
            "winecom_reverse_dispatch IMFAttributes::SetString" \
            "winecom_reverse_dispatch IMFAttributes::SetUnknown" \
            "winecom_reverse_dispatch IMFAttributes::GetUnknown" \
            "winecom_reverse_dispatch IMFAttributes::GetCount" \
            "rev_enter_guest entering guest method" \
            "wrapped IUnknown host .* as proxy"; do
    if ! grep -qE "$want" "$OUT/trace.err"; then
        bad "no '$want' in the +winecom trace"
    fi
done
[ $fail -eq 0 ] && say "trace: the reverse path was taken for every argument \
class, and a FORWARD proxy was minted inside a reverse call"

# The COST, which is an observation and not a check: it is a different number
# every run, so the probe writes it to stderr and it never enters the
# transcript layer 2 compares against itself.  Reported here because a number
# nobody prints is a number nobody knows -- and this one is what an XAudio2
# mixer thread pays at every buffer boundary.
cost=$(grep -m1 "^note: one call costs" "$OUT/probe.err")
[ -n "$cost" ] && say "cost: ${cost#note: }"

# ---- 3: the refusal discipline -------------------------------------------
if grep -qE "refusing the REVERSE call IMFAttributes::SetItem" "$OUT/trace.err"; then
    say "refusal: SetItem is refused in the reverse direction too, by name"
else
    bad "no reverse refusal of IMFAttributes::SetItem in the trace -- a row \
the tables refuse for what its signature IS must be refused both ways"
fi

# ---- 4: the reference contract -------------------------------------------
if grep -qE "destroying reverse proxy .*IMFAttributes guest" "$OUT/trace.err"; then
    say "balance: the reverse proxy was destroyed and the guest's Release \
entered"
else
    bad "the reverse proxy was never destroyed -- its one guest reference is \
still held"
fi

[ $fail -eq 0 ] && say "PASS"
exit $fail
