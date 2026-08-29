#!/bin/sh
#
# check-guest-ntdll-seed.sh -- the guest ntdll namespace-seed gate.
#
# CATALOG.md's Skyrim Special Edition entry and Handoff #1 root-cause the
# wall every SteamStub v3.1-wrapped title in this game list hits at its own
# entry point: the anti-debug prologue calls
#
#     h  = GetModuleHandleA("ntdll.dll");
#     fn = GetProcAddress(h, "NtSetInformationThread");
#     fn(GetCurrentThread(), ThreadHideFromDebugger, NULL, 0);
#
# without checking either return value, which on real Windows is always safe
# -- ntdll.dll is mapped into every process before any user code runs -- and
# before this fix was never safe here, because this port's guest module
# namespace held only what a guest image's own static imports dragged in,
# and nothing imports ntdll.dll.  h was NULL, fn was NULL, and the call
# through fn was a c0000005 EXECUTE_FAULT at address 0.  The fix,
# loader_init() in dlls/ntdll/loader.c, seeds the guest namespace with
# ntdll.dll once at guest-process bringup, before any guest instruction runs
# -- GetModuleHandle itself still never loads anything.
#
# ppc64le/games/ntdll_seed_probe.c carries the full defect/fix writeup and
# is built TWICE from one source:
#
#   checked   verifies each return value before using it; never crashes on
#             purpose.  Built as both the x86-64 guest PE and a native ppc64
#             PE, so the native run corroborates the transcript describes
#             real, achievable Windows semantics rather than something
#             guest-specific.
#   blind (-DNTDLL_SEED_PROBE_BLIND)
#             the LITERAL SteamStub sequence: no NULL check before the final
#             call.  Guest-only -- it is the faithful reproduction of the
#             actual DRM stub, and it is what the sabotage layer runs.
#
# Six layers, each removing one way of passing by accident:
#
#   1  PREREQ: the guest ntdll thunk exists and really exports
#      NtSetInformationThread by name -- otherwise every later layer would be
#      testing a probe that can never succeed for a reason unrelated to the
#      seed.
#   2  GUEST: the checked guest probe passes all three steps -- ntdll found,
#      the export resolved, and the stub actually called and returned
#      STATUS_SUCCESS (dlls/ntdll/unix/thread.c's ThreadHideFromDebugger
#      case), not merely "did not crash".
#   3  BINDING: the handle GetModuleHandleA returned is the REAL guest
#      ntdll's load base, read independently from the +loaddll trace -- not
#      a coincidental non-NULL value that happens to satisfy step 2's check.
#   4  NATIVE: the same checked probe, built as a native ppc64 PE, passes
#      too -- corroboration only, by a mechanism this fix never touches
#      (native Wine has always loaded its own ntdll for every process).
#   5  FAITHFUL: the BLIND probe -- the literal, unchecked SteamStub
#      sequence -- passes un-sabotaged.  This is the closest this gate comes
#      to running the actual DRM stub.
#   6  SABOTAGE (also standalone as --sabotage): WINEEMUNOGUESTNTDLLSEED=1
#      (dlls/ntdll/loader.c) skips the seed, reproducing the pre-fix
#      namespace exactly, and the blind probe MUST then reproduce the
#      documented crash byte-for-byte: c0000005, EXECUTE_FAULT
#      (info[0]=...008, info[1]=...000), at address 0.  A gate that cannot
#      go red proves nothing, and a gate that goes red for some OTHER
#      reason proves the wrong thing.
#
# Exit 0 = pass, 1 = a check failed, 2 = could not run (not a pass).
set -u

HERE=$(cd "$(dirname "$0")" && pwd)
SRC=$(cd "$HERE/../.." && pwd)
BUILD=${BUILD:-$SRC}                    # in-tree build by default
OUT=${OUT:-/tmp/guest-ntdll-seed}
SABOTAGE=0
[ "${1:-}" = "--sabotage" ] && SABOTAGE=1

say()  { echo "check-guest-ntdll-seed: $*"; }
bad()  { echo "check-guest-ntdll-seed: FAIL $*" >&2; fail=1; }
skip() { echo "check-guest-ntdll-seed: $*" >&2; exit 2; }

[ -x "$BUILD/wine" ] || skip "no wine loader at $BUILD/wine"
[ -n "${WINEPREFIX:-}" ] || skip "set WINEPREFIX to a prefix wineboot has run in"
[ -n "${WINEFEXBRIDGE:-}" ] || skip "set WINEFEXBRIDGE to the emulator bridge"
GUEST_NTDLL="$BUILD/dlls/ntdll/x86_64-windows/ntdll.dll"
[ -f "$GUEST_NTDLL" ] || skip "no guest ntdll thunk at $GUEST_NTDLL; build it first"
command -v clang >/dev/null || skip "need clang for the guest build"
command -v llvm-dlltool >/dev/null || skip "need llvm-dlltool for the guest build"
command -v python3 >/dev/null || skip "need python3 to read the guest ntdll's export table"

mkdir -p "$OUT" || skip "cannot create $OUT"
fail=0

INCL="-I$BUILD/include -I$SRC/include -I$SRC/include/msvcrt"
TIMEOUT=${TIMEOUT:-120}

# ---- 1: prereq -------------------------------------------------------------
# Same export-table reader check-seh-smoke.sh's binding layer uses: walk the
# guest ntdll thunk's own export directory and fail loudly if the export
# named by NtSetInformationThread's spec entry is not there at all.  That
# absence would make every later layer fail for a reason this fix does not
# own.
cat > "$OUT/exportrva.py" <<'EOF'
import struct, sys

data = open(sys.argv[1], 'rb').read()
pe = struct.unpack_from('<I', data, 0x3c)[0]
nsec, = struct.unpack_from('<H', data, pe + 6)
optsz, = struct.unpack_from('<H', data, pe + 20)
opt = pe + 24
magic, = struct.unpack_from('<H', data, opt)
exp_rva, = struct.unpack_from('<I', data, opt + (112 if magic == 0x20b else 96))

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
    raise SystemExit('rva 0x%x is in no section' % rva)

eo = off(exp_rva)
nfunc, nname = struct.unpack_from('<II', data, eo + 20)
afunc, aname, aord = struct.unpack_from('<III', data, eo + 28)
for i in range(nname):
    no = off(struct.unpack_from('<I', data, off(aname) + 4 * i)[0])
    if data[no:data.index(b'\0', no)].decode('latin1') != sys.argv[2]:
        continue
    idx, = struct.unpack_from('<H', data, off(aord) + 2 * i)
    print('0x%x' % struct.unpack_from('<I', data, off(afunc) + 4 * idx)[0])
    break
else:
    raise SystemExit('%s is not exported' % sys.argv[2])
EOF
NSIT_RVA=$(python3 "$OUT/exportrva.py" "$GUEST_NTDLL" NtSetInformationThread 2>&1)
case "$NSIT_RVA" in
    0x*) say "prereq: the guest ntdll thunk exports NtSetInformationThread at RVA $NSIT_RVA" ;;
    *)   skip "the guest ntdll thunk ($GUEST_NTDLL) does not export \
NtSetInformationThread ($NSIT_RVA); nothing below can pass" ;;
esac

# ---- build: the x86-64 guest PE, checked and blind -------------------------
# No ntdll.def anywhere in this file: the whole point under test is that the
# probe does NOT import ntdll.dll, exactly like the real SteamStub stub it
# reproduces.  A static import would seed the namespace on its own and every
# layer below would pass whether or not the fix exists.
cat > "$OUT/kernel32.def" <<'EOF'
LIBRARY kernel32.dll
EXPORTS
GetStdHandle
WriteFile
ExitProcess
GetModuleHandleA
GetProcAddress
GetCurrentThread
EOF
llvm-dlltool -m i386:x86-64 -d "$OUT/kernel32.def" -l "$OUT/libkernel32guest.a" \
    || skip "llvm-dlltool failed for kernel32"

GUESTCC="clang -target x86_64-windows-gnu -nostdlibinc $INCL \
-DNTDLL_SEED_PROBE_NO_CRT -D_UCRT -Wall -O1 -fno-builtin -g"
GUESTLD="clang -target x86_64-windows-gnu -fuse-ld=lld -nostdlib \
-Wl,--entry=ntdll_seed_probe_entry -Wl,--subsystem,console"

$GUESTCC -c -o "$OUT/probe_guest.o" "$HERE/ntdll_seed_probe.c" \
    || skip "guest checked compile failed"
$GUESTLD -o "$OUT/probe_guest.exe" "$OUT/probe_guest.o" "$OUT/libkernel32guest.a" \
    || skip "guest checked link failed"

$GUESTCC -DNTDLL_SEED_PROBE_BLIND -c -o "$OUT/probe_blind.o" "$HERE/ntdll_seed_probe.c" \
    || skip "guest blind compile failed"
$GUESTLD -o "$OUT/probe_blind.exe" "$OUT/probe_blind.o" "$OUT/libkernel32guest.a" \
    || skip "guest blind link failed"

# -all,+seh,+loaddll,+module: stdout stays clean enough to grep for the
# transcript, while the port's own fault diagnostics and module-load trace
# still reach stderr.  +seh (not err+seh) is deliberate: handle_syscall_fault's
# info[0]/info[1]/addr breakdown the sabotage layer keys on is logged at
# TRACE, not ERR, so a plain "+seh" (all classes for the channel) is what
# actually surfaces it -- measured against a real game list log, not assumed.
# +module is the seed itself: dlls/ntdll/loader.c declares
# WINE_DEFAULT_DEBUG_CHANNEL(module), so the WINEEMUNOGUESTNTDLLSEED WARN and
# the seed-failure ERR both live there, not under seh.
WDBG=${WINEDEBUG:--all,+seh,+loaddll,+module}
run_guest() { timeout -k 5 "$TIMEOUT" \
                  env WINEDEBUG="$WDBG" WINEDLLOVERRIDES="winedbg.exe=d" \
                  WINEEMUNOGUESTNTDLLSEED=${1:-0} \
                  "$BUILD/wine" "$2"; }

# ---- 6 (also available standalone as --sabotage): the negative control ----
neg_control() {
    started=$(date +%s)
    run_guest 1 "$OUT/probe_blind.exe" >"$OUT/sabotage.out" 2>"$OUT/sabotage.err"
    st=$?
    elapsed=$(( $(date +%s) - started ))
    if [ $st -eq 124 ] || [ $st -eq 137 ]; then
        bad "the sabotaged blind run HUNG (killed after ${TIMEOUT}s); a call \
through address 0 must fault promptly, not spin"
        tail -10 "$OUT/sabotage.err" | sed 's/^/  sabotage| /' >&2
        return
    fi
    if grep -q "ntdll_seed_probe: PASS" "$OUT/sabotage.out"; then
        bad "WINEEMUNOGUESTNTDLLSEED=1 still PASSED -- the gate cannot go red"
        return
    fi
    if [ $st -eq 0 ]; then
        bad "the sabotaged blind run exited 0 without printing PASS; a \
silent non-crash is not the documented failure"
    else
        say "sabotage: exited $st after ${elapsed}s, no PASS printed"
    fi
    if grep -q "WINEEMUNOGUESTNTDLLSEED: deliberately not seeding" "$OUT/sabotage.err"; then
        say "sabotage: loader_init confirms it skipped the seed"
    else
        bad "loader_init never logged WINEEMUNOGUESTNTDLLSEED taking effect; \
this run may not have exercised the knob at all"
    fi
    # The documented signature, verbatim from CATALOG.md's Skyrim entry:
    # code=c0000005, EXECUTE_FAULT (info[0]=8), at address 0 (info[1]=0, and
    # the fault address itself nil).  Each piece is checked separately so a
    # crash for some unrelated reason cannot masquerade as this one.
    if grep -qi "c0000005" "$OUT/sabotage.err"; then
        say "sabotage: names c0000005, as documented"
    else
        sed 's/^/  sabotage| /' "$OUT/sabotage.err" >&2
        bad "the sabotaged run does not name c0000005; this is not the \
documented NULL-call crash"
    fi
    if grep -qE "info\[0\]=0*8\b" "$OUT/sabotage.err"; then
        say "sabotage: info[0]=8 (EXECUTE_FAULT), as documented"
    else
        bad "the sabotaged run does not show info[0]=8 (EXECUTE_FAULT); \
the fault shape does not match the documented one"
    fi
    if grep -qE "addr=\(nil\)|pc=0\b|pc=0x0+\b" "$OUT/sabotage.err"; then
        say "sabotage: the fault is at address 0, as documented"
    else
        bad "the sabotaged run does not name address 0 as the fault site"
    fi
}

if [ "$SABOTAGE" = 1 ]; then
    neg_control
    [ $fail -eq 0 ] && say "SABOTAGE PASS"
    exit $fail
fi

# ---- 2: guest (checked) ----------------------------------------------------
run_guest 0 "$OUT/probe_guest.exe" > "$OUT/guest.out" 2>"$OUT/guest.err"
gst=$?
if [ $gst -eq 124 ] || [ $gst -eq 137 ]; then
    bad "the checked guest run timed out after ${TIMEOUT}s"
elif grep -q "ntdll_seed_probe: PASS 3/3" "$OUT/guest.out"; then
    say "guest: $(tail -1 "$OUT/guest.out")"
else
    sed 's/^/  guest| /' "$OUT/guest.out" >&2
    tail -20 "$OUT/guest.err" >&2
    bad "the checked x86-64 guest build did not pass 3/3"
fi

# ---- 3: binding -------------------------------------------------------------
# Step 1's printed handle must be the guest ntdll's REAL load base, taken
# independently from the +loaddll trace -- same discipline as
# check-seh-smoke.sh's binding layer, and for the same reason: a non-NULL
# value that happened to satisfy step 2's check is not proof it is the right
# module.
GOT_HANDLE=$(grep -m1 '^step 1 ' "$OUT/guest.out" | sed -n 's/.*handle=0x\([0-9A-Fa-f]*\).*/\1/p')
NTDLL_BASE=$(grep -m1 'Loaded .*sysx8664..ntdll\.dll" at ' "$OUT/guest.err" | \
             sed 's/.*" at \([0-9A-Fa-f]*\).*/\1/')
if [ -z "$NTDLL_BASE" ]; then
    bad "the guest ntdll (sysx8664) never appears in the +loaddll trace, so \
the seed cannot be confirmed to have loaded the real module"
elif [ -z "$GOT_HANDLE" ]; then
    bad "could not read the handle GetModuleHandleA printed in step 1"
elif [ "$(echo "$GOT_HANDLE" | tr 'a-f' 'A-F' | sed 's/^0*//')" = \
        "$(echo "$NTDLL_BASE" | tr 'a-f' 'A-F' | sed 's/^0*//')" ]; then
    say "binding: GetModuleHandleA returned the real guest ntdll's load base ($NTDLL_BASE)"
else
    bad "GetModuleHandleA returned 0x$GOT_HANDLE, not the guest ntdll's own \
load base 0x$NTDLL_BASE -- it found something, but not the seeded module"
fi

# ---- 4: native (corroboration) ---------------------------------------------
# Same recipe check-seh-smoke.sh's native lane uses: an ordinary consumer of
# the public headers, built with Wine's own ppc64 CRT.  Native Wine has
# always loaded its own ntdll for every process, so this is not a test of the
# fix -- it is evidence the transcript describes real, achievable Windows
# semantics rather than a guest-only artifact.
native_lane() {
    ${CC:-gcc} -c -o "$OUT/probe_native.o" "$HERE/ntdll_seed_probe.c" $INCL \
        -D_UCRT -D_WIN32 -Wall -pipe -mlongcall -mno-pltseq -fcf-protection=none \
        -fvisibility=hidden -fno-stack-protector -fno-strict-aliasing -gdwarf-4 \
        -fPIC -fasynchronous-unwind-tables -mlong-double-64 -fno-builtin \
        -fshort-wchar -Wno-format -g -O1 2>"$OUT/native.build.err" || return 1
    "$BUILD/tools/winegcc/winegcc" -o "$OUT/probe_native.exe" --wine-objdir "$BUILD" \
        --cc-cmd="${CC:-gcc}" -mno-cygwin -fPIC -fasynchronous-unwind-tables \
        -Wl,--wine-builtin -mconsole "$OUT/probe_native.o" \
        "$BUILD/libs/winecrt0/ppc64-windows/libwinecrt0.a" \
        "$BUILD/dlls/ucrtbase/ppc64-windows/libucrtbase.a" \
        "$BUILD/dlls/kernel32/ppc64-windows/libkernel32.a" \
        "$BUILD/dlls/ntdll/ppc64-windows/libntdll.a" \
        2>>"$OUT/native.build.err" || return 1
    rm -f "$OUT/probe_native.exe"
    "$SRC/tools/elf2pe" "$OUT/probe_native.exe.so" "$OUT/probe_native.exe" \
        2>>"$OUT/native.build.err" || return 1
    "$BUILD/tools/winebuild/winebuild" --builtin "$OUT/probe_native.exe" \
        2>>"$OUT/native.build.err" || return 1
    return 0
}

if native_lane; then
    timeout -k 5 "$TIMEOUT" env WINEDEBUG=-all WINEDLLOVERRIDES="winedbg.exe=d" \
        "$BUILD/wine" "$OUT/probe_native.exe" > "$OUT/native.out" 2>"$OUT/native.err"
    if grep -q "ntdll_seed_probe: PASS 3/3" "$OUT/native.out"; then
        say "native: $(tail -1 "$OUT/native.out") (corroboration only -- native \
Wine has always loaded its own ntdll)"
    else
        sed 's/^/  native| /' "$OUT/native.out" >&2
        bad "the native ppc64 build did not pass 3/3"
    fi
else
    say "native: not expressible on this toolchain -- build failed, not fatal \
to this gate.  Last error:"
    tail -5 "$OUT/native.build.err" | sed 's/^/  native| /'
fi

# ---- 5: faithful (blind, un-sabotaged) --------------------------------------
run_guest 0 "$OUT/probe_blind.exe" > "$OUT/blind.out" 2>"$OUT/blind.err"
bst=$?
if [ $bst -eq 124 ] || [ $bst -eq 137 ]; then
    bad "the un-sabotaged blind run timed out after ${TIMEOUT}s"
elif grep -q "ntdll_seed_probe: PASS 3/3" "$OUT/blind.out"; then
    say "faithful: the literal, unchecked SteamStub sequence passes: \
$(tail -1 "$OUT/blind.out")"
else
    sed 's/^/  blind| /' "$OUT/blind.out" >&2
    tail -20 "$OUT/blind.err" >&2
    bad "the un-sabotaged blind (literal SteamStub) run did not pass 3/3 -- \
the exact sequence real Steam builds run is still unsafe"
fi

# ---- 6: negative control ----------------------------------------------------
neg_control

[ $fail -eq 0 ] && say "PASS"
exit $fail
