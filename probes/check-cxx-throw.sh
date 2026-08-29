#!/bin/sh
#
# check-cxx-throw.sh -- the gate for guestcrt's _CxxThrowException
# (guest-cxx-eh-plan.md, Session A).
#
# Shape copied from ppc64le/seh/check-seh-smoke.sh and check-seh-handlers.sh:
# build a freestanding x86-64 guest PE against hand-written import libs, run
# it under the port, require every compile-time-known check, --sabotage as a
# negative control on the CHECKS themselves.  This file lives under probes/
# (not ppc64le/*/check-*.sh) on purpose -- it never opens a live desktop or
# an AeDebug-hooked prefix's modal dialog the way the ppc64le/*/check-*.sh
# gates can, so it is safe to run directly; WINEDLLOVERRIDES=winedbg.exe=d
# is still set on every run anyway, belt and braces, exactly as those gates
# do.
#
# What this proves, layer by layer (see probes/guest/cxx_throw.c's own
# banner for the per-step detail):
#
#   1  SHAPE: the built guest .exe really imports __C_specific_handler from
#      VCRUNTIME140.dll (not ntdll -- a hand-written vcruntime140.def forces
#      the .xdata's relocation there), and its exception directory carries a
#      real language-handler UNWIND_INFO rather than the vacuous "clang
#      decided this __try can't unwind" shape check-seh-smoke.sh's layer 1
#      exists to catch.
#   2  GUEST: cxx_throw.exe runs under the port and reports PASS 13/13 -- the
#      two forwarder-chain resolutions, their agreement, and every field of
#      the exception record for both a normal throw and the `throw;`
#      spelling, each against a value known before the probe ran.
#   3  TRANSCRIPT: the guest stdout is byte-identical to the transcript
#      embedded below (no raw addresses are ever printed -- see the source
#      comment at steps 1/2/11 -- so this is not a flaky compare).
#   4  NEGATIVE CONTROL: a throw with NO handler at all must still reach the
#      port's existing unhandled path and die promptly, with a nonzero exit,
#      naming e06d7363 and saying "unhandled at guest level" -- the
#      dispatcher's own attribution of the pc to guest code, not the
#      emulator's JIT.  A hang or a silent exit 0 here is worse than a
#      missing feature.
#
# --sabotage rebuilds probes/guest/cxx_throw.c with -DSABOTAGE, which changes
# ONLY the value step 8 compares the magic against (the real throw is
# untouched) -- and requires that build to FAIL.  A gate that cannot fail is
# not a gate; this is the same discipline check-fp-marshal.sh and the seh
# gates already hold this tree to.
#
# Exit 0 = pass, 1 = a check failed, 2 = could not run (not a pass).
set -u

HERE=$(cd "$(dirname "$0")" && pwd)
SRC=$(cd "$HERE/.." && pwd)
BUILD=${BUILD:-$SRC}
OUT=${OUT:-/tmp/cxx-throw}
SABOTAGE=0
[ "${1:-}" = "--sabotage" ] && SABOTAGE=1

say()  { echo "check-cxx-throw: $*"; }
bad()  { echo "check-cxx-throw: FAIL $*" >&2; fail=1; }
skip() { echo "check-cxx-throw: $*" >&2; exit 2; }

[ -x "$BUILD/wine" ] || skip "no wine loader at $BUILD/wine"
[ -f "$BUILD/dlls/guestcrt/x86_64-windows/guestcrt.dll" ] || \
    skip "no guestcrt.dll; build it first"
[ -f "$BUILD/dlls/vcruntime140/x86_64-windows/vcruntime140.dll" ] || \
    skip "no guest vcruntime140 thunk; build it first"
[ -f "$BUILD/dlls/ucrtbase/x86_64-windows/ucrtbase.dll" ] || \
    skip "no guest ucrtbase thunk; build it first"
command -v clang >/dev/null || skip "need clang for the guest build"
command -v llvm-dlltool >/dev/null || skip "need llvm-dlltool for the guest build"
command -v llvm-readobj >/dev/null || skip "need llvm-readobj to read the image"
command -v python3 >/dev/null || skip "need python3 to read the exception directory"

# WINEPREFIX and the FEX bridge variables are properties of the machine, not
# of this gate; a prefix that has run wineboot is required (WINEPREFIX
# defaults to the "gate" bringup prefix this tree already keeps for exactly
# this purpose), and the four FEX_* variables are required verbatim from the
# environment note this session was briefed with -- without them FEX asserts
# and core-dumps in a way that looks like a code bug, not an environment one.
WINEPREFIX=${WINEPREFIX:-$HOME/Development/wine-prefixes/gate}
export WINEPREFIX
[ -d "$WINEPREFIX/dosdevices" ] || skip "WINEPREFIX=$WINEPREFIX has not run wineboot"
FEX_APP_DATA_LOCATION=${FEX_APP_DATA_LOCATION:-$HOME/Development/fexrootfs/}
FEX_ROOTFS=${FEX_ROOTFS:-$HOME/Development/fexrootfs/RootFS/Ubuntu_24_04}
FEX_THUNKGUESTLIBS=${FEX_THUNKGUESTLIBS:-$HOME/Development/fastppcx86/build-thunks/Guest}
FEX_THUNKHOSTLIBS=${FEX_THUNKHOSTLIBS:-$HOME/Development/fastppcx86/build-thunks/HostLibs_64}
export FEX_APP_DATA_LOCATION FEX_ROOTFS FEX_THUNKGUESTLIBS FEX_THUNKHOSTLIBS

mkdir -p "$OUT" || skip "cannot create $OUT"
fail=0

INCL="-I$BUILD/include -I$SRC/include -I$SRC/include/msvcrt"
TIMEOUT=${TIMEOUT:-60}

# ---- build: the x86-64 guest PE ------------------------------------------
# Same clang x86_64-windows-gnu machinery the seh gates use.  The imports are
# described by hand, as check-seh-smoke.sh's are: naming __C_specific_handler
# in vcruntime140.def (NOT ntdll.def) is what forces the compiled __try's
# .xdata relocation to resolve through VCRUNTIME140.dll, exactly the way
# game_x64.dll and libxess*.dll import it -- the shape guest-cxx-eh-plan.md
# section 1 item 2 says was mis-served before the FORWARD fix landed.
cat > "$OUT/kernel32.def" <<'EOF'
LIBRARY kernel32.dll
EXPORTS
GetStdHandle
WriteFile
ExitProcess
LoadLibraryA
GetProcAddress
GetModuleHandleA
EOF
cat > "$OUT/vcruntime140.def" <<'EOF'
LIBRARY vcruntime140.dll
EXPORTS
__C_specific_handler
EOF
for m in kernel32 vcruntime140; do
    llvm-dlltool -m i386:x86-64 -d "$OUT/$m.def" -l "$OUT/lib$m.a" \
        || skip "llvm-dlltool failed for $m"
done

GUESTCC="clang -target x86_64-windows-gnu -nostdlibinc $INCL -fms-extensions \
-DUSE_COMPILER_EXCEPTIONS -D_UCRT -Wall -O1 -fno-builtin -g"
GUESTLD="clang -target x86_64-windows-gnu -fuse-ld=lld -nostdlib \
-Wl,--entry=cxx_throw_entry -Wl,--subsystem,console"

build_variant() {   # $1 = name, $2... = extra defines
    name=$1; shift
    $GUESTCC "$@" -c -o "$OUT/cxx_throw_$name.o" "$HERE/guest/cxx_throw.c" \
        || skip "guest compile failed for variant $name"
    $GUESTLD -o "$OUT/cxx_throw_$name.exe" "$OUT/cxx_throw_$name.o" \
        "$OUT/libkernel32.a" "$OUT/libvcruntime140.a" \
        -Wl,-Map,"$OUT/cxx_throw_$name.map" \
        || skip "guest link failed for variant $name"
}
build_variant guest
build_variant sabotage  -DSABOTAGE
build_variant unhandled -DCXX_THROW_UNHANDLED

EXE="$OUT/cxx_throw_guest.exe"

# ---- layer 1: shape -------------------------------------------------------
llvm-readobj --coff-imports "$EXE" > "$OUT/imports.txt" 2>&1
CSH_DLL=$(awk '/^Import \{/ { dll = "" }
               /Name: .*\.dll/ { dll = $2 }
               /__C_specific_handler/ { print dll; exit }' "$OUT/imports.txt")
case "$CSH_DLL" in
    *[Vv][Cc][Rr][Uu][Nn][Tt][Ii][Mm][Ee]140*)
        say "image: __C_specific_handler is imported from $CSH_DLL, as a real \
/MD title imports it" ;;
    "")
        bad "the guest exe does not import __C_specific_handler at all; the \
__try compiled away and this gate would be vacuous" ;;
    *)
        bad "__C_specific_handler is imported from $CSH_DLL, not vcruntime140.dll; \
the identity-path fix (guest-cxx-eh-plan.md section 1 item 2) is untested" ;;
esac

cat > "$OUT/pdata.py" <<'EOF'
import struct, sys

data = open(sys.argv[1], 'rb').read()
pe = struct.unpack_from('<I', data, 0x3c)[0]
assert data[pe:pe+4] == b'PE\0\0', 'not a PE'
nsec, = struct.unpack_from('<H', data, pe + 6)
optsz, = struct.unpack_from('<H', data, pe + 20)
opt = pe + 24
magic, = struct.unpack_from('<H', data, opt)
ddir = opt + (112 if magic == 0x20b else 96)
exc_rva, exc_size = struct.unpack_from('<II', data, ddir + 3 * 8)

sections = []
s = opt + optsz
for i in range(nsec):
    vsize, vaddr, rawsize, rawptr = struct.unpack_from('<IIII', data, s + 8)
    sections.append((vaddr, vsize, rawptr, rawsize))
    s += 40

def off(rva):
    for vaddr, vsize, rawptr, rawsize in sections:
        if vaddr <= rva < vaddr + max(vsize, rawsize):
            return rawptr + (rva - vaddr)
    return None

n = exc_size // 12
ehandler = 0
for i in range(n):
    o = off(exc_rva + i * 12)
    if o is None:
        continue
    begin, end, unw = struct.unpack_from('<III', data, o)
    uo = off(unw)
    if uo is None:
        continue
    verflags = data[uo]
    if (verflags >> 3) & 0x3:
        ehandler += 1

print('exc_size=%u entries=%u ehandler=%u' % (exc_size, n, ehandler))
EOF
PD=$(python3 "$OUT/pdata.py" "$EXE" 2>&1) || skip "could not read the guest \
exception directory: $PD"
say "image: $PD"
# Measured: at -O1 clang inlines call_normal_throw()/call_rethrow_spelling()
# into cxx_throw_run(), so BOTH __try blocks end up sharing one function-
# level UNWIND_INFO/scope table rather than one each -- MSVC SEH is a
# per-FUNCTION handler with a per-try-block scope table entry underneath it,
# not one physical handler per __try.  So the bound here is >= 1, and it is
# the guest transcript (steps 4 and 12, both filters actually entered) that
# proves BOTH try blocks really ran, not this count.
EH=$(echo "$PD" | sed 's/.*ehandler=//')
case "$PD" in *"exc_size=0"*) bad "the guest exe has an EMPTY exception \
directory; the __try compiled away" ;; esac
if [ "${EH:-0}" -lt 1 ]; then
    bad "no UNWIND_INFO carries a language handler; the __try blocks \
compiled away (expected >= 1)"
fi

# ---- run helper -------------------------------------------------------
WDBG=${WINEDEBUG:--all},err+seh
run_wine() { timeout -k 5 "$2" \
                 env WINEDEBUG="$WDBG" WINEDLLOVERRIDES="winedbg.exe=d" \
                 "$BUILD/wine" "$1"; }

# ---- --sabotage: the checks-can-fail control ------------------------------
if [ "$SABOTAGE" = 1 ]; then
    run_wine "$OUT/cxx_throw_sabotage.exe" "$TIMEOUT" \
        >"$OUT/sabotage.out" 2>"$OUT/sabotage.err"
    st=$?
    if [ $st -eq 124 ] || [ $st -eq 137 ]; then
        bad "the sabotaged build HUNG; a failing check must still exit promptly"
    elif [ $st -eq 0 ]; then
        bad "the sabotaged build exited 0; flipping the expected magic must \
make step 8 FAIL"
    elif grep -q "^cxx_throw: PASS" "$OUT/sabotage.out"; then
        bad "the sabotaged build still printed PASS; the flipped constant \
never got compared"
    elif grep -q "^cxx_throw: FAIL" "$OUT/sabotage.out"; then
        say "sabotage: exited $st and printed FAIL, as a wrong magic must"
    else
        sed 's/^/  sabotage| /' "$OUT/sabotage.out" >&2
        bad "the sabotaged build neither PASSed nor FAILed by name; it died \
before printing its verdict"
    fi
    [ $fail -eq 0 ] && say "SABOTAGE PASS"
    exit $fail
fi

# ---- layer 2/3: the guest run and its transcript --------------------------
cat > "$OUT/expected.txt" <<'EOF'
step 1 resolve _CxxThrowException via vcruntime140.dll:  ok
step 2 resolve _CxxThrowException via ucrtbase.dll:  ok
step 3 both forwarder chains resolve to the same address:  ok
step 4 normal throw: the private filter actually ran:  ok
step 5 normal throw: ExceptionCode: e06d7363 ok
step 6 normal throw: EXCEPTION_NONCONTINUABLE is set:  ok
step 7 normal throw: NumberParameters: 4 ok
step 8 normal throw: ExceptionInformation[0] (CXX_FRAME_MAGIC_VC6): 19930520 ok
step 9 normal throw: ExceptionInformation[1] == &g_object:  ok
step 10 normal throw: ExceptionInformation[2] == &g_throwinfo:  ok
step 11 normal throw: ExceptionInformation[3] == this module's base:  ok
step 12 rethrow spelling: the private filter actually ran:  ok
step 13 rethrow spelling: ExceptionInformation[1] == [2] == 0:  ok
cxx_throw: PASS 13/13
EOF
run_wine "$EXE" "$TIMEOUT" >"$OUT/guest.out" 2>"$OUT/guest.err"
st=$?
if [ $st -eq 124 ] || [ $st -eq 137 ]; then
    bad "the guest probe HUNG (killed after ${TIMEOUT}s)"
elif [ $st -ne 0 ]; then
    sed 's/^/  guest| /' "$OUT/guest.out" >&2
    tail -20 "$OUT/guest.err" | sed 's/^/  guest.err| /' >&2
    bad "the guest probe exited $st; expected 0 (all checks pass)"
elif diff -u "$OUT/expected.txt" "$OUT/guest.out" >"$OUT/diff.txt" 2>&1; then
    say "transcript: byte-identical to the expected 13/13 PASS"
else
    cat "$OUT/diff.txt" >&2
    bad "the guest transcript does not match the expected 13/13 PASS byte for byte"
fi

# ---- layer 4: negative control ---------------------------------------
NEG_DEADLINE=${NEG_DEADLINE:-20}
run_wine "$OUT/cxx_throw_unhandled.exe" "$NEG_DEADLINE" \
    >"$OUT/unhandled.out" 2>"$OUT/unhandled.err"
st=$?
if [ $st -eq 124 ] || [ $st -eq 137 ]; then
    bad "the unhandled throw HUNG (killed after ${NEG_DEADLINE}s); an \
unhandled exception must be prompt"
    tail -10 "$OUT/unhandled.err" | sed 's/^/  unhandled| /' >&2
elif [ $st -eq 0 ]; then
    bad "the unhandled throw exited 0; a throw with no handler must not be \
a silent success"
else
    say "negative control: exited $st"
fi
if grep -q "cxx_throw: FAIL the unhandled throw returned" "$OUT/unhandled.out"; then
    bad "the unhandled throw RESUMED; something swallowed it"
fi
if ! grep -q "cxx_throw: unhandled probe" "$OUT/unhandled.out"; then
    bad "the unhandled probe never reached its first marker; it died before \
the throw, so it proves nothing"
fi
if grep -qi "e06d7363" "$OUT/unhandled.err"; then
    say "negative control: the death names e06d7363: $(grep -im1 e06d7363 \
        "$OUT/unhandled.err" | cut -c1-110)"
else
    sed 's/^/  unhandled| /' "$OUT/unhandled.err" >&2
    bad "the unhandled death never names the exception code e06d7363"
fi
# NOT a "starts with 0x140000000" address-window check, unlike
# ppc64le/seh/check-seh-smoke.sh's DLL probe: an EXE built exactly the same
# way as that gate's guest PE is, MEASURED here, does not load at its
# preferred base -- this port maps it (like the guest thunk DLLs
# check-seh-smoke.sh's own comment already notes) somewhere in the
# 0x00003fffxxxxxxxx range instead, so a base-relative window would be
# asserting a number this port never promised.  What the phrase "unhandled
# at guest level" asserts instead is the thing that actually matters: the
# dispatcher's own signal_ppc64.c code identified the pc as belonging to
# GUEST code (not the emulator's own JIT) before giving up on it.
if grep -q "unhandled at guest level" "$OUT/unhandled.err"; then
    say "negative control: the death names guest level: $(grep -m1 \
        'unhandled at guest level' "$OUT/unhandled.err" | cut -c1-120)"
else
    sed 's/^/  unhandled| /' "$OUT/unhandled.err" >&2
    bad "the unhandled death never says 'unhandled at guest level'; the \
guest pc was lost on the way out, or attributed to the emulator instead"
fi

if [ $fail -eq 0 ]; then
    say "PASS"
else
    say "FAIL -- see above"
fi
exit $fail
