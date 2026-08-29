#!/bin/sh
#
# check-seh-smoke.sh -- the table-based (.pdata/.xdata) SEH dispatch gate.
#
# The guest side of this port dispatches exceptions today by walking the TEB
# registration chain (dlls/ntdll/signal_ppc64.c, dispatch_guest_exception).
# That is the 32-bit-shaped mechanism; an x86-64 PE compiled by any real
# toolchain carries its handlers in an exception DIRECTORY instead -- .pdata
# RUNTIME_FUNCTIONs pointing at .xdata UNWIND_INFOs whose language handler is
# __C_specific_handler, driven by a scope table.  Until that is dispatched,
# every __try in every guest program is invisible.  This file is the gate for
# building it: it is expected to be RED until the dispatcher exists, and its
# job is to be red for exactly the right reasons and green for no others.
#
# Six layers, each removing one way of passing by accident:
#
#   1  VACUITY: the built guest .exe really carries an exception directory,
#      really imports __C_specific_handler from ntdll, and really has
#      UNWIND_INFOs with UNW_FLAG_EHANDLER set.  This layer exists because
#      clang will silently compile __try/__except into NOTHING when it thinks
#      the guarded body cannot unwind (see the long note under "the native
#      lane" below and the header comment of seh_smoke.c).  Without layer 1 a
#      probe that had quietly lost its handlers would pass layers 2-3 while
#      testing nothing at all.
#  1b  BINDING: the __C_specific_handler that .xdata names resolves to
#      something on the GUEST side of the machine boundary rather than to
#      native ppc64 code.  A name that binds to the wrong machine is not a
#      handler, and today it binds to the wrong machine SILENTLY.
#   2  GUEST: the guest PE runs under the emulator and reports PASS -- every
#      one of its fourteen steps checks a value: an exception code, an
#      ExceptionInformation pair, a faulting address inside the faulting
#      function, an ordered marker trace, a __finally call count, a local
#      variable's value after resumption.
#   3  TRANSCRIPT: the guest stdout is byte-identical to the expected
#      transcript embedded below.  "PASS" from a program that also printed
#      something new is not a pass.
#   4  NATIVE: the same source built as a native ppc64 Windows PE and run.
#      Semantic corroboration only, by a different mechanism -- see the
#      accounting below -- and it must match the transcript to the LINE,
#      including the one line that is measured to differ.
#   5  NEGATIVE CONTROL: a fault OUTSIDE any __try, built from the same source
#      under -DSEH_SMOKE_UNHANDLED, must reach the port's existing unhandled
#      path: a PROMPT death, naming the fault AND the guest PC, with a nonzero
#      status.  Not a hang, not a silent success.  Bounded by timeout,
#      asserted on the status, the wall clock and the stderr text.
#
# --sabotage runs layer 5 alone.
#
# Exit 0 = pass, 1 = a check failed, 2 = could not run (not a pass).
#
#
# THE NATIVE ppc64 LANE, MEASURED RATHER THAN ASSUMED.
#
# check-com-smoke.sh gets its strongest layer from byte-identical native-vs-
# guest output.  The equivalent here would be worth very little, and the
# measurement that says so is worth writing down rather than glossing:
#
#   - gcc on ppc64 has no MSVC structured exception handling at all: __try is
#     not a keyword and there is no .pdata/.xdata to dispatch.  The construct
#     under test cannot be compiled for the native lane, full stop.
#   - Wine's own __TRY/__EXCEPT/__FINALLY (include/wine/exception.h) DO
#     compile and DO run for the native ppc64 PE -- measured, not assumed;
#     seh_smoke.c is written in those macros precisely so one source serves
#     both lanes.  With USE_COMPILER_EXCEPTIONS defined (the guest build) they
#     expand to the compiler's __try/__except/__finally; without it (the
#     native build) they expand to Wine's setjmp-and-TEB-chain emulation.
#   - But that emulation is a DIFFERENT MECHANISM, not a second
#     implementation of the same one.  It registers frames by hand on the TEB
#     chain and leaves via __wine_longjmp; it has no unwind tables, no scope
#     table, no __C_specific_handler, and no frame-by-frame virtual unwind.
#     It agrees with the guest lane about SEMANTICS -- which code runs, in
#     which order, with which values -- while sharing none of the machinery
#     the guest lane exists to test.
#
# What the native lane therefore DOES cover: that the expected transcript is
# an achievable one.  Thirteen of its fourteen steps come back identical on
# ppc64 through a completely independent implementation, which is real
# evidence that the transcript describes correct SEH rather than describing
# whatever the guest happens to do.  What it CANNOT cover: anything about
# .pdata, the guest exception directory, or the cross-machine walk.  Layer 3
# is the value gate; layer 4 is corroboration.
#
# THE ONE MEASURED DIVERGENCE, step 6.  Native Wine's RtlRaiseException fills
# ExceptionAddress from its OWN captured PC, so a RaiseException reports an
# address inside ntdll, not inside the caller.  The guest path is different by
# construction: dlls/ntdll/signal_ppc64.c emu_RaiseException sets
# ExceptionAddress to the return address the guest's CALL pushed, i.e. inside
# the raising function, which is what an x86-64 program compiled for Windows
# expects.  So step 6 is expected to read in_window=yes on the guest and
# in_window=no natively.  That single difference is baked into the native
# expectation below rather than waved through, so that a SECOND native
# divergence -- which would mean the transcript is wrong about something --
# is loud instead of lost in a diff.
#
# The native lane is allowed to be UNBUILDABLE without failing the run (it
# reports "native: not expressible" and moves on), because its absence says
# nothing about the dispatcher.  What it may never do is silently disappear or
# silently drift.
#
#
# WHY EVERY RUN DISABLES winedbg.  The bringup prefix has AeDebug configured
# with "winedbg --auto", so an unhandled fault starts the debugger, which
# attaches, loads its GUI stack and never lets go -- measured: the negative
# control was killed at 300s with WineDbg still attached and idle.  That turns
# every red state of this gate into a hang, which is the one thing a gate must
# never be.  WINEDLLOVERRIDES=winedbg.exe=d makes start_debugger's
# CreateProcess fail, so UnhandledExceptionFilter falls straight through to
# terminating the process -- the port's own death path, timed at 0s.  This is
# an environment override for the duration of one run and touches nothing in
# the prefix.
set -u

HERE=$(cd "$(dirname "$0")" && pwd)
SRC=$(cd "$HERE/../.." && pwd)
BUILD=${BUILD:-$SRC}                    # in-tree build by default
OUT=${OUT:-/tmp/seh-smoke}
SABOTAGE=0
[ "${1:-}" = "--sabotage" ] && SABOTAGE=1

say()  { echo "check-seh-smoke: $*"; }
bad()  { echo "check-seh-smoke: FAIL $*" >&2; fail=1; }
note() { echo "check-seh-smoke: note $*"; }
skip() { echo "check-seh-smoke: $*" >&2; exit 2; }

[ -x "$BUILD/wine" ] || skip "no wine loader at $BUILD/wine"
[ -n "${WINEPREFIX:-}" ] || skip "set WINEPREFIX to a prefix wineboot has run in"
[ -n "${WINEFEXBRIDGE:-}" ] || skip "set WINEFEXBRIDGE to the emulator bridge"
[ -f "$BUILD/dlls/ntdll/x86_64-windows/ntdll.dll" ] || \
    skip "no guest ntdll thunk; build it first"
command -v clang >/dev/null || skip "need clang for the guest build"
command -v llvm-dlltool >/dev/null || skip "need llvm-dlltool for the guest build"
command -v python3 >/dev/null || skip "need python3 to read the exception directory"

mkdir -p "$OUT" || skip "cannot create $OUT"
fail=0

INCL="-I$BUILD/include -I$SRC/include -I$SRC/include/msvcrt"
TIMEOUT=${TIMEOUT:-120}

# ---- build: the x86-64 guest PE ------------------------------------------
# Same clang x86_64-windows-gnu machinery tools/spec2thunk drives its
# signature oracle with, and the same Wine headers, exactly as the COM gate
# does -- so any disagreement is the boundary, not the declarations.
#
# -fms-extensions is what makes __try a keyword for this target; without it
# clang rejects it as an undeclared identifier.  It is NOT enough on its own:
# clang only attaches a language handler to a __try whose body it believes can
# unwind, and for -windows-gnu there is no way to tell it that a memory fault
# can (-fasync-exceptions is accepted only for the -windows-msvc target and is
# warned-and-ignored here).  seh_smoke.c therefore calls through a volatile
# function pointer inside every __try; layer 1 below checks that this actually
# worked on the produced image rather than trusting it.
#
# The imports are described by hand rather than taken from a mingw sysroot:
# the point of naming the DLL for each symbol is that the guest binds to the
# same builtins a real guest application would.  __C_specific_handler is the
# language handler clang names in .xdata -- measured, not assumed: the .xdata
# of a __try compiled for this target carries an IMAGE_REL_AMD64_ADDR32NB
# relocation against the symbol __C_specific_handler, and mingw/lld resolve it
# through an ordinary import thunk.  It is an ntdll export on Windows and it
# must be one here.
cat > "$OUT/kernel32.def" <<'EOF'
LIBRARY kernel32.dll
EXPORTS
GetStdHandle
WriteFile
ExitProcess
RaiseException
EOF
cat > "$OUT/ntdll.def" <<'EOF'
LIBRARY ntdll.dll
EXPORTS
__C_specific_handler
EOF
for m in kernel32 ntdll; do
    llvm-dlltool -m i386:x86-64 -d "$OUT/$m.def" -l "$OUT/lib$m.a" \
        || skip "llvm-dlltool failed for $m"
done

GUESTCC="clang -target x86_64-windows-gnu -nostdlibinc $INCL -fms-extensions \
-DSEH_SMOKE_NO_CRT -DUSE_COMPILER_EXCEPTIONS -D_UCRT -Wall -O1 -fno-builtin -g"
GUESTLD="clang -target x86_64-windows-gnu -fuse-ld=lld -nostdlib \
-Wl,--entry=seh_smoke_entry -Wl,--subsystem,console"

$GUESTCC -c -o "$OUT/seh_smoke_guest.o" "$HERE/seh_smoke.c" \
    || skip "guest compile failed"
$GUESTLD -o "$OUT/seh_smoke_guest.exe" "$OUT/seh_smoke_guest.o" \
    "$OUT/libkernel32.a" "$OUT/libntdll.a" || skip "guest link failed"

$GUESTCC -DSEH_SMOKE_UNHANDLED -c -o "$OUT/seh_smoke_unhandled.o" "$HERE/seh_smoke.c" \
    || skip "guest unhandled-probe compile failed"
$GUESTLD -o "$OUT/seh_smoke_unhandled.exe" "$OUT/seh_smoke_unhandled.o" \
    "$OUT/libkernel32.a" "$OUT/libntdll.a" || skip "guest unhandled-probe link failed"

# ---- the PE reader used by layer 1 ---------------------------------------
# Reads DataDirectory[3] (IMAGE_DIRECTORY_ENTRY_EXCEPTION) straight out of the
# built image, walks the RUNTIME_FUNCTION array, and reports how many of the
# UNWIND_INFOs actually carry UNW_FLAG_EHANDLER (0x1) or UNW_FLAG_UHANDLER
# (0x2).  Those flags are the whole question: a __try that clang decided could
# not unwind still emits a RUNTIME_FUNCTION and an UNWIND_INFO, just with a
# flags field of zero and no handler -- which is the vacuous state this gate
# must never accept.
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
    name = data[s:s+8].rstrip(b'\0').decode('latin1')
    vsize, vaddr, rawsize, rawptr = struct.unpack_from('<IIII', data, s + 8)
    sections.append((name, vaddr, vsize, rawptr, rawsize))
    s += 40

def off(rva):
    for name, vaddr, vsize, rawptr, rawsize in sections:
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

print('exc_rva=0x%x exc_size=%u entries=%u ehandler=%u' %
      (exc_rva, exc_size, n, ehandler))
EOF

# ---- 1: vacuity ----------------------------------------------------------
PD=$(python3 "$OUT/pdata.py" "$OUT/seh_smoke_guest.exe" 2>&1) || \
    skip "could not read the guest exception directory: $PD"
say "image: $PD"
case "$PD" in
    *"exc_size=0 "*) bad "the guest exe has an EMPTY exception directory" ;;
esac
EH=$(echo "$PD" | sed 's/.*ehandler=//')
if [ "${EH:-0}" -lt 8 ]; then
    bad "only ${EH:-0} UNWIND_INFOs carry a language handler; the probe's \
__try blocks compiled away (expected at least 8)"
fi
llvm-readobj --coff-imports "$OUT/seh_smoke_guest.exe" > "$OUT/imports.txt" 2>&1
CSH_DLL=$(awk '/^Import \{/ { dll = "" }
               /Name: .*\.dll/ { dll = $2 }
               /__C_specific_handler/ { print dll; exit }' "$OUT/imports.txt")
if [ -n "$CSH_DLL" ]; then
    say "image: __C_specific_handler is imported from $CSH_DLL"
else
    bad "the guest exe does not import __C_specific_handler; its .xdata names \
no language handler and the whole gate would be vacuous"
fi

# ---- the transcript every correct implementation must print --------------
# Embedded rather than captured, because there is nothing to capture from yet:
# this is what a correct table-based dispatcher OWES, written down before it
# exists.  Every value here is either a constant of the ABI (0xc0000005, the
# write flag 1, the null address 0), a constant of this program
# (0xe5e40001 and its two parameters), or an ordering that MSVC SEH defines
# (filter in the search phase, __finally in the unwind phase, handler last).
cat > "$OUT/expected.txt" <<'EOF'
seh_smoke: start
step 1 null store: filter code: code=0xc0000005 calls=1 ok
step 2 null store: exception information: nparam=2 info0=0x0000000000000001 info1=0x0000000000000000 ok
step 3 null store: faulting address is inside the faulting function: in_window=yes ok
step 4 null store: trace: 'store avfilt avhandler' ok
step 5 RaiseException: filter code and parameters: code=0xe5e40001 nparam=2 info0=0x11223344 info1=0x55667788 ok
step 6 RaiseException: raising address is inside the raising function: in_window=yes ok
step 7 RaiseException: trace: 'raise raisefilt raisehandler' ok
step 8 unwind order: filter, then __finally, then the __except body: 'store orderfilt fin-abnormal orderhandler' ok
step 9 unwind order: the __finally ran exactly once, abnormally: calls=1 abnormal=1 ok
step 10 two frames: declining filter, intermediate __finally, outer handler: 'L1 L2 L3 store innerfilt outerfilt midfin-abnormal L1handler L1after' ok
step 11 two frames: the declining filter saw the code, the __finally ran once: innercode=0xc0000005 midfin=1 ok
step 12 continuation: the frame's local survives filter and handler: witness=0x222 ok
step 13 continuation: the code after the __except block runs: 'store witnessfilt witnesshandler witnessafter' ok
step 14 fall-through: __finally runs exactly once, normally: calls=1 trace='nofault fall-normal' ok
seh_smoke: PASS 14/14
EOF

# -all,err+seh: stdout stays clean enough to diff, while the port's own seh
# diagnostics still reach stderr -- a red gate that says nothing about why is
# only half a gate.
WDBG=${WINEDEBUG:--all,err+seh}
run_wine() { timeout -k 5 "$2" \
                 env WINEDEBUG="$WDBG" WINEDLLOVERRIDES="winedbg.exe=d" \
                 "$BUILD/wine" "$1"; }

# ---- 5 (also available standalone as --sabotage): the negative control ----
#
# The unhandled probe faults outside any __try.  Nothing may catch it, and the
# three things that must be true of its death are checked separately, because
# each of them has failed in this port at some point: it must be PROMPT (the
# emulator must not spin or wait), it must be a nonzero EXIT (not a silent
# success), and it must NAME the fault at the GUEST pc (the failure mode this
# port already fixed once was dying with "emulator bridge failed (1)", and the
# failure mode after that was naming an address inside the JIT rather than
# inside the guest image).
NEG_DEADLINE=${NEG_DEADLINE:-20}
neg_control() {
    started=$(date +%s)
    run_wine "$OUT/seh_smoke_unhandled.exe" "$NEG_DEADLINE" \
        >"$OUT/unhandled.out" 2>"$OUT/unhandled.err"
    st=$?
    elapsed=$(( $(date +%s) - started ))
    if [ $st -eq 124 ] || [ $st -eq 137 ]; then
        bad "the unhandled fault HUNG (killed after ${NEG_DEADLINE}s); the \
unhandled path must be prompt"
        tail -10 "$OUT/unhandled.err" | sed 's/^/  unhandled| /' >&2
        return
    fi
    if [ $st -eq 0 ]; then
        bad "the unhandled fault exited 0; a fault outside any __try must not \
be a silent success"
    else
        say "negative control: exited $st after ${elapsed}s"
    fi
    if grep -q "seh_smoke: FAIL the unhandled fault returned" "$OUT/unhandled.out"; then
        bad "the unhandled fault RESUMED; something swallowed it"
    fi
    if ! grep -q "seh_smoke: unhandled probe" "$OUT/unhandled.out"; then
        bad "the unhandled probe never reached its first marker; it died \
before the fault, so it proves nothing"
    fi
    # Named: the exception code has to appear.  Either spelling of the port's
    # report satisfies this, so a rewording of the message does not turn the
    # gate red for no reason -- but losing the code entirely does.
    if grep -qEi "c0000005|page fault on write access to 0000000000000000" \
            "$OUT/unhandled.err"; then
        say "negative control: $(grep -Eim1 \
            'c0000005|page fault on write access' "$OUT/unhandled.err" | cut -c1-110)"
    else
        sed 's/^/  unhandled| /' "$OUT/unhandled.err" >&2
        bad "the unhandled death names neither c0000005 nor the null write"
    fi
    # Named AT THE GUEST PC.  lld bases the guest image at 0x140000000, so a
    # report that carries the guest program counter says 00000001400xxxxx
    # somewhere.  A report that only carries the emulator's own JIT address
    # tells a user nothing about their program and is not a legible death.
    if grep -qE "00000001400[0-9a-fA-F]{5}" "$OUT/unhandled.err"; then
        say "negative control: the death names the guest pc: $(grep -Eom1 \
            '00000001400[0-9a-fA-F]{5}' "$OUT/unhandled.err")"
    else
        sed 's/^/  unhandled| /' "$OUT/unhandled.err" >&2
        bad "the unhandled death never names an address in the guest image \
(0x140000000-based); the guest pc was lost on the way out"
    fi
}

if [ "$SABOTAGE" = 1 ]; then
    neg_control
    [ $fail -eq 0 ] && say "SABOTAGE PASS"
    exit $fail
fi

# ---- 1b: binding ---------------------------------------------------------
# Layer 1 proves the .xdata NAMES a language handler.  This proves the name
# binds to something the GUEST can execute, which is a separate question and
# the one the whole port turns on: the handler RVA in .xdata points at the
# exe's import thunk, and if that thunk lands on native ppc64 code then the
# first thing a working dispatcher does is jump into ppc64 bytes as x86-64.
# It is a silent defect -- the loader satisfies the import either way and
# never says a word -- which is exactly why it gets a layer.
#
# The criterion is EXACT arithmetic, not an address window.  An address window
# was tried first and was wrong: the guest thunk modules are mapped in the same
# 0x00003fffxxxxxxxx region as Wine's native PE modules (measured -- the guest
# C:\windows\sysx8664\ntdll.dll loads at 0x00003fffffba0000), so "looks native"
# and "is native" are not the same test.  Instead:
#
#   * read the export RVA of __C_specific_handler out of the guest thunk PE
#     itself, and fail loudly if that module does not export it at all -- that
#     absence IS the defect this layer was invented for;
#   * take the guest ntdll's load base from the +loaddll trace, keyed on the
#     GUEST system directory (sysx8664) so the native ntdll cannot be mistaken
#     for it;
#   * require resolved_address == base + rva.
GUEST_NTDLL="$BUILD/dlls/ntdll/x86_64-windows/ntdll.dll"
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
CSH_RVA=$(python3 "$OUT/exportrva.py" "$GUEST_NTDLL" __C_specific_handler 2>&1)
case "$CSH_RVA" in
    0x*) say "binding: the guest ntdll thunk exports __C_specific_handler at RVA $CSH_RVA" ;;
    *)   bad "the guest ntdll thunk ($GUEST_NTDLL) does not export \
__C_specific_handler ($CSH_RVA), so the guest's .xdata handler binds to the \
NATIVE ntdll and jumps into ppc64 code"
         CSH_RVA="" ;;
esac

timeout -k 5 "$TIMEOUT" env WINEDEBUG=+imports,+loaddll WINEDLLOVERRIDES="winedbg.exe=d" \
    "$BUILD/wine" "$OUT/seh_smoke_guest.exe" >/dev/null 2>"$OUT/imports.trace"
# Both greps are keyed to the id of the process that loaded the GUEST ntdll,
# which is the probe by construction: only a guest image makes sysx8664 appear
# at all.  Taking the first matching line in the FILE instead was wrong, and
# wrong in a way that only shows on a cold prefix: starting the probe on a
# prefix with no wineserver also starts wineboot.exe and services.exe, which
# are NATIVE Wine processes whose own ntdll imports resolve -- correctly -- to
# the native ntdll, and whose traces are interleaved into the same stderr and
# come first.  Measured: cold, this layer read wineboot's 00003FFF9CCFF250 and
# called the port broken; warm, the same build and the same probe read its own
# 00003FFFFFBB41F0 and passed.  A layer that answers a different process's
# question is not red for the right reason.
GUEST_ID=$(grep -m1 'Loaded .*sysx8664..ntdll\.dll" at ' "$OUT/imports.trace" | cut -d: -f1)
NTDLL_BASE=$(grep -m1 'Loaded .*sysx8664..ntdll\.dll" at ' "$OUT/imports.trace" | \
             sed 's/.*" at \([0-9A-Fa-f]*\).*/\1/')
if [ -n "$GUEST_ID" ]; then
    CSH_ADDR=$(grep "^$GUEST_ID:" "$OUT/imports.trace" | \
               grep -m1 -- "--- __C_specific_handler ntdll.dll" | sed 's/.*= *//')
else
    CSH_ADDR=""
fi
if [ -z "$NTDLL_BASE" ]; then
    bad "the guest ntdll (sysx8664) never appears in the +loaddll trace, so \
where __C_specific_handler bound cannot be decided"
elif [ -z "$CSH_ADDR" ]; then
    bad "the loader never resolved __C_specific_handler for the guest exe; \
the import vanished instead of failing"
elif [ -n "$CSH_RVA" ]; then
    WANT=$(printf '%016X' $(( 0x$NTDLL_BASE + $CSH_RVA )))
    if [ "$(echo "$CSH_ADDR" | tr 'a-f' 'A-F')" = "$WANT" ]; then
        say "binding: __C_specific_handler resolved to $CSH_ADDR == guest ntdll \
$NTDLL_BASE + $CSH_RVA"
    else
        bad "__C_specific_handler resolved to $CSH_ADDR, not to the guest ntdll's \
own stub at $WANT ($NTDLL_BASE + $CSH_RVA): the .xdata handler does not land in \
guest code"
    fi
fi

# ---- 2: guest ------------------------------------------------------------
run_wine "$OUT/seh_smoke_guest.exe" "$TIMEOUT" > "$OUT/guest.out" 2>"$OUT/guest.err"
gst=$?
if [ $gst -eq 124 ] || [ $gst -eq 137 ]; then
    bad "the guest run timed out after ${TIMEOUT}s"
elif grep -q "seh_smoke: PASS" "$OUT/guest.out"; then
    say "guest: $(tail -1 "$OUT/guest.out")"
else
    sed 's/^/  guest| /' "$OUT/guest.out" >&2
    tail -20 "$OUT/guest.err" >&2
    bad "the x86-64 guest build did not pass"
fi

# ---- 3: transcript -------------------------------------------------------
if cmp -s "$OUT/expected.txt" "$OUT/guest.out"; then
    say "transcript: the guest printed exactly the expected $(wc -l \
        < "$OUT/expected.txt") lines"
else
    diff "$OUT/expected.txt" "$OUT/guest.out" | sed 's/^/  /' >&2
    bad "the guest transcript is not the expected one"
fi

# ---- 4: native (informational; see the header) ---------------------------
# Built exactly the way check-com-smoke.sh builds its native lane, minus
# -D__WINESRC__: an ordinary consumer of the public headers, which is the only
# form the guest build can also see.  USE_COMPILER_EXCEPTIONS is deliberately
# NOT defined here, so wine/exception.h expands to its setjmp form.
native_lane() {
    ${CC:-gcc} -c -o "$OUT/seh_smoke.o" "$HERE/seh_smoke.c" $INCL \
        -D_UCRT -D_WIN32 -Wall -pipe -mlongcall -mno-pltseq -fcf-protection=none \
        -fvisibility=hidden -fno-stack-protector -fno-strict-aliasing -gdwarf-4 \
        -fPIC -fasynchronous-unwind-tables -mlong-double-64 -fno-builtin \
        -fshort-wchar -Wno-format -g -O1 2>"$OUT/native.build.err" || return 1
    "$BUILD/tools/winegcc/winegcc" -o "$OUT/seh_smoke.exe" --wine-objdir "$BUILD" \
        --cc-cmd="${CC:-gcc}" -mno-cygwin -fPIC -fasynchronous-unwind-tables \
        -Wl,--wine-builtin -mconsole "$OUT/seh_smoke.o" \
        "$BUILD/libs/winecrt0/ppc64-windows/libwinecrt0.a" \
        "$BUILD/dlls/ucrtbase/ppc64-windows/libucrtbase.a" \
        "$BUILD/dlls/kernel32/ppc64-windows/libkernel32.a" \
        "$BUILD/dlls/ntdll/ppc64-windows/libntdll.a" \
        2>>"$OUT/native.build.err" || return 1
    rm -f "$OUT/seh_smoke.exe"
    "$SRC/tools/elf2pe" "$OUT/seh_smoke.exe.so" "$OUT/seh_smoke.exe" \
        2>>"$OUT/native.build.err" || return 1
    "$BUILD/tools/winebuild/winebuild" --builtin "$OUT/seh_smoke.exe" \
        2>>"$OUT/native.build.err" || return 1
    return 0
}

if native_lane; then
    # The native expectation is the guest transcript with exactly one line
    # rewritten: step 6, whose divergence is explained at the top of this file
    # and comes from native RtlRaiseException reporting its own PC.  Anything
    # else differing is a real signal and fails this layer.
    sed -e "s/^step 6 \(.*\): in_window=yes ok\$/step 6 \1: in_window=no FAIL (ExceptionAddress is not in raise_private_code)/" \
        -e "s/^seh_smoke: PASS 14\/14\$/seh_smoke: FAIL 13\/14/" \
        "$OUT/expected.txt" > "$OUT/expected.native.txt"
    timeout -k 5 "$TIMEOUT" env WINEDEBUG="$WDBG" WINEDLLOVERRIDES="winedbg.exe=d" \
        "$BUILD/wine" "$OUT/seh_smoke.exe" > "$OUT/native.out" 2>"$OUT/native.err"
    if cmp -s "$OUT/expected.native.txt" "$OUT/native.out"; then
        say "native: Wine's setjmp __TRY reproduces 13/14 of the transcript on \
ppc64, diverging only at step 6 as measured and explained -- the transcript is \
achievable semantics, by a mechanism that shares no .pdata with the guest lane"
    else
        diff "$OUT/expected.native.txt" "$OUT/native.out" | sed 's/^/  native| /' >&2
        bad "the native ppc64 lane diverges from the transcript somewhere OTHER \
than the one known and explained place; either the transcript is wrong or \
native SEH regressed"
    fi
else
    note "native: not expressible on this toolchain -- the build failed.  \
Last error:"
    tail -5 "$OUT/native.build.err" | sed 's/^/  native| /'
    note "native: this does NOT fail the gate; gcc on ppc64 has no MSVC SEH, \
so there is no native lane for the construct under test.  Layer 3 (the \
embedded transcript) is the value gate."
fi

# ---- 5: negative control -------------------------------------------------
neg_control

[ $fail -eq 0 ] && say "PASS"
exit $fail
