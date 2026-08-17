#!/bin/sh
#
# check-seh-handlers.sh -- the gate for a GUEST language handler entered as
# guest code, its DISPATCHER_CONTEXT, and RtlUnwindEx called from inside it.
#
# check-seh-smoke.sh gates the other half of table-based dispatch: a compiled
# __try whose .xdata names ntdll's __C_specific_handler, which the port
# recognises by exact address identity and serves with a NATIVE implementation
# of those semantics.  That is the fast path, and it is the ONLY path a probe
# written in C can reach -- clang names __C_specific_handler in the .xdata of
# every __try it compiles, for -windows-gnu and -windows-msvc alike, and there
# is no flag that changes it.
#
# A real application leaves that path immediately.  An image linked against the
# static MSVC runtime carries its own byte-identical copy of
# __C_specific_handler (DOOM (2016), at DOOMx64vk.exe+0x1eab2c8), its own
# __GSHandlerCheck (steam_api64.dll+0xed68) and its own __CxxFrameHandler*, and
# its .xdata names THOSE.  Nothing in a PE says which handler an RVA is, so the
# port must enter it as guest code -- handler( EXCEPTION_RECORD *, void
# *EstablisherFrame, CONTEXT *, DISPATCHER_CONTEXT * ), MS-x64, in a nested
# emulator run -- build it a DISPATCHER_CONTEXT it can read, honour the
# disposition it returns, and then serve the RtlUnwindEx it calls.  This file
# gates that, on a real x86-64 PE, with an image whose .xdata provably names a
# handler of the probe's own.
#
# Seven layers, each removing one way of passing by accident:
#
#   1  SHAPE: the built guest .exe carries an exception directory, UNWIND_INFOs
#      that really set UNW_FLAG_EHANDLER, and the imports the probe needs --
#      __C_specific_handler from ntdll.dll (the clang __try lane) and
#      RtlUnwindEx from BOTH kernel32.dll and ntdll.dll (the two routes stage C
#      exercises).  As in check-seh-smoke.sh this layer exists because clang
#      silently drops the language handler of a __try it believes cannot unwind.
#  1b  IDENTITY -- the layer without which this whole gate would be a second,
#      slower copy of check-seh-smoke.sh.  The RUNTIME_FUNCTION for the hand-
#      written frame pf_call must name pf_language_handler, the probe's OWN
#      function, in its UNWIND_INFO, and must NOT name the imported
#      __C_specific_handler; its handler data must decode as a well-formed
#      SCOPE_TABLE of one record whose JumpTarget is pf_landing.  Asserted twice
#      over, once by a PE reader written here and once by llvm-readobj --unwind,
#      because the assertion is the premise of everything below it.
#   2  GUEST: the probe runs under the emulator and reports PASS.  Every one of
#      its thirty-two steps checks a value or a relation: a DISPATCHER_CONTEXT
#      field against compile-time knowledge of the frame, an ordered marker
#      trace that crosses the machine boundary, a __finally call count, the
#      contents of RAX and of a nonvolatile register at an unwind's landing pad,
#      -- steps 21 to 25 -- an eleven-slot consolidating-unwind record read
#      back field by field by the routine it was built for, down BOTH of the
#      port's unwind roads (a handler asking for it, and a guest asking for it
#      itself), and -- steps 26 to 31 -- a COLLIDED UNWIND down both of those
#      roads as well: an unwind started from inside an unwind, where what is
#      measured is that the frame the outer unwind was heading for never runs
#      its __except, that the scope which collided does not run twice, that the
#      one __finally between the collision and the inner unwind's target runs
#      exactly once, and that the colliding frame's handler is re-entered at the
#      ScopeIndex the collision left it at, carrying the INNER unwind's record.
#   3  TRANSCRIPT: the guest stdout is byte-identical to the transcript embedded
#      below.  "PASS" from a program that also printed something new is not a
#      pass.
#   4  THE PORT'S OWN VIEW: a separate run with WINEDEBUG=+seh must contain the
#      port's "entering guest language handler" trace naming the EXACT addresses
#      the image's .xdata names, as many times as the probe's own witness
#      counters counted.  Layer 2 proves the handlers' code ran; this proves it
#      ran because the frame walk entered it, and not by some other road.  The
#      same layer asserts which DOOR the chaining handler's call to
#      __C_specific_handler came in by -- emu_C_specific_handler, the thunk
#      override, not the frame walk's identity check -- and that no err-level
#      diagnostic names __C_specific_handler at all, because serving that call
#      wrongly is quiet by nature: ExceptionContinueSearch to a frame entitled
#      to its __except looks from the outside like a frame with no handler.
#   5  NEGATIVE CONTROL: a fault under a private handler that DECLINES must
#      still reach the port's unhandled path -- a PROMPT death, naming the fault
#      AND the guest pc, with a nonzero status.  The same three assertions
#      check-seh-smoke.sh's neg_control makes, over a frame that has a language
#      handler rather than none.
#   6  REFUSALS: the three states this port refuses by name from the private
#      handler -- a collided unwind that cannot ADVANCE (a handler that returns
#      ExceptionCollidedUnwind and hands back the same dispatcher context every
#      time), an exit unwind (RtlUnwindEx with a null target frame), and a
#      consolidating unwind that names NO consolidation routine -- must each die
#      promptly, nonzero, with the refusal named in the port's own diagnostics.
#      A refusal that is silent, or that becomes a hang, is worse than a missing
#      feature.  Two of these used to be the FEATURE rather than its degenerate
#      case: the consolidating unwind is steps 21-25 of the passing transcript
#      and the collided unwind is steps 26-31, and what is left refused in each
#      is the form that has no right answer.  The collided control additionally
#      asserts that the old wording ("collided unwinds are not implemented") is
#      gone, because a port that went back to refusing the whole family would
#      otherwise satisfy this layer while failing layer 2.
#
# --sabotage runs layer 5 alone.
#
# Exit 0 = pass, 1 = a check failed, 2 = could not run (not a pass).
#
#
# THERE IS NO NATIVE ppc64 LANE HERE, AND THAT IS A MEASUREMENT RATHER THAN AN
# OMISSION.  check-seh-smoke.sh has one because its probe is written in Wine's
# __TRY macros, which have a setjmp-and-TEB-chain expansion that gcc on ppc64
# does compile -- a different mechanism agreeing about semantics.  This probe
# cannot have one: the construct under test is a hand-written x86-64 .seh_proc
# carrying a .seh_handler directive and an @IMGREL SCOPE_TABLE.  There is no
# ppc64 spelling of that; not a different spelling, none.  So layer 3 is the
# whole value gate, and what stands in for the native lane is inside the
# transcript instead: stage B raises the SAME exception through the private
# handler and through an ordinary clang __try, and requires the two independent
# handler implementations to have been handed the same record, field for field.
#
# WHY THIS DOES NOT ALSO RUN check-seh-smoke.sh.  They are separate gates and
# stay separate, deliberately.  A gate should have one owner for its red state:
# folding the identity-path gate into this one would mean a failure of the
# NATIVE __C_specific_handler implementation turning this file red, and a reader
# would then have to work out which of two mechanisms broke.  The overlap that
# matters is covered anyway -- stage B of this probe drives an ordinary clang
# __try, i.e. the identity fast path, in the same process and the same
# transcript as the private-handler path, so a regression that broke the
# identity path in a way this gate depends on cannot hide.  Run both; they are
# both fast, and they answer different questions.
#
# WHY EVERY RUN DISABLES winedbg, verbatim from check-seh-smoke.sh because the
# hazard is identical: the bringup prefix has AeDebug configured with
# "winedbg --auto", so an unhandled fault starts the debugger, which attaches,
# loads its GUI stack and never lets go.  That turns every red state of this
# gate -- and layers 5 and 6, which are red states BY CONSTRUCTION -- into a
# hang, which is the one thing a gate must never be.  WINEDLLOVERRIDES=
# winedbg.exe=d makes start_debugger's CreateProcess fail, so
# UnhandledExceptionFilter falls straight through to terminating the process.
# This is an environment override for the duration of one run and touches
# nothing in the prefix.
set -u

HERE=$(cd "$(dirname "$0")" && pwd)
SRC=$(cd "$HERE/../.." && pwd)
BUILD=${BUILD:-$SRC}                    # in-tree build by default
OUT=${OUT:-/tmp/seh-handlers}
SABOTAGE=0
[ "${1:-}" = "--sabotage" ] && SABOTAGE=1

say()  { echo "check-seh-handlers: $*"; }
bad()  { echo "check-seh-handlers: FAIL $*" >&2; fail=1; }
note() { echo "check-seh-handlers: note $*"; }
skip() { echo "check-seh-handlers: $*" >&2; exit 2; }

[ -x "$BUILD/wine" ] || skip "no wine loader at $BUILD/wine"
[ -n "${WINEPREFIX:-}" ] || skip "set WINEPREFIX to a prefix wineboot has run in"
# Required from the environment rather than defaulted to a path here: which FEX
# build the port is bridged to is a property of the machine, not of this gate,
# and a gate that silently picks a bridge is a gate that can pass against an
# emulator nobody is shipping.
[ -n "${WINEFEXBRIDGE:-}" ] || skip "set WINEFEXBRIDGE to the emulator bridge"
[ -f "$BUILD/dlls/ntdll/x86_64-windows/ntdll.dll" ] || \
    skip "no guest ntdll thunk; build it first"
[ -f "$BUILD/dlls/kernel32/x86_64-windows/kernel32.dll" ] || \
    skip "no guest kernel32 thunk; build it first"
command -v clang >/dev/null || skip "need clang for the guest build"
command -v llvm-dlltool >/dev/null || skip "need llvm-dlltool for the guest build"
command -v llvm-readobj >/dev/null || skip "need llvm-readobj to read the image"
command -v python3 >/dev/null || skip "need python3 to read the exception directory"

mkdir -p "$OUT" || skip "cannot create $OUT"
fail=0

INCL="-I$BUILD/include -I$SRC/include -I$SRC/include/msvcrt"
TIMEOUT=${TIMEOUT:-120}
IMAGEBASE=0x140000000                   # lld's default for an x86-64 PE; asserted below

# ---- build: the x86-64 guest PE ------------------------------------------
# The same clang x86_64-windows-gnu machinery check-seh-smoke.sh uses, with the
# same hand-written imports and the same no-CRT entry point, so that any
# disagreement between the two gates is about the code under test and not about
# how the probe was built.
#
# Two additions this gate needs:
#
#   * seh_handlers_asm.S, assembled by clang's INTEGRATED assembler for this
#     target.  The mingw-w64 SEH directives (.seh_proc, .seh_handler,
#     .seh_handlerdata) are what put a handler of the probe's OWN into a real
#     UNWIND_INFO; no C construct on this toolchain can.
#
#   * `RtlUnwindEx_ntdll == RtlUnwindEx` in ntdll.def.  That is the mingw import
#     -library aliasing form: the image gets a second import thunk, for the same
#     exported name, resolved through a DIFFERENT module.  It is how one image
#     can import RtlUnwindEx from kernel32.dll AND from ntdll.dll at once, which
#     is what stage C needs to exercise both of the port's override rows.  DOOM
#     (2016) takes RtlUnwindEx from KERNEL32 and imports nothing at all from
#     ntdll, so the kernel32 row is the one that matters in the field and the
#     ntdll row is the one that is easy to believe is being tested when it is
#     not.
cat > "$OUT/kernel32.def" <<'EOF'
LIBRARY kernel32.dll
EXPORTS
GetStdHandle
WriteFile
ExitProcess
RaiseException
GetCurrentThreadId
RtlUnwindEx
EOF
cat > "$OUT/ntdll.def" <<'EOF'
LIBRARY ntdll.dll
EXPORTS
__C_specific_handler
RtlUnwindEx_ntdll == RtlUnwindEx
EOF
for m in kernel32 ntdll; do
    llvm-dlltool -m i386:x86-64 -d "$OUT/$m.def" -l "$OUT/lib$m.a" \
        || skip "llvm-dlltool failed for $m"
done

GUESTCC="clang -target x86_64-windows-gnu -nostdlibinc $INCL -fms-extensions \
-DUSE_COMPILER_EXCEPTIONS -D_UCRT -Wall -O1 -fno-builtin -g"
GUESTAS="clang -target x86_64-windows-gnu -g"
GUESTLD="clang -target x86_64-windows-gnu -fuse-ld=lld -nostdlib \
-Wl,--entry=seh_handlers_entry -Wl,--subsystem,console"

$GUESTAS -c -o "$OUT/seh_handlers_asm.o" "$HERE/seh_handlers_asm.S" \
    || skip "the hand-written SEH frame did not assemble; without it this gate \
has nothing to test (clang cannot be made to name a private language handler)"

# One object per variant, one image per variant.  Each control ENDS its process,
# so they cannot be stages of one run, and a control that shares a process with
# the thing it controls is not a control.
build_variant() {   # $1 = name, $2... = extra defines
    name=$1; shift
    $GUESTCC "$@" -c -o "$OUT/seh_handlers_$name.o" "$HERE/seh_handlers.c" \
        || { skip "guest compile failed for variant $name"; }
    $GUESTLD -o "$OUT/seh_handlers_$name.exe" "$OUT/seh_handlers_$name.o" \
        "$OUT/seh_handlers_asm.o" "$OUT/libkernel32.a" "$OUT/libntdll.a" \
        -Wl,-Map,"$OUT/seh_handlers_$name.map" \
        || { skip "guest link failed for variant $name"; }
}
build_variant guest
build_variant unhandled  -DSEH_HANDLERS_UNHANDLED
build_variant collided   -DSEH_HANDLERS_COLLIDED
build_variant exitunwind -DSEH_HANDLERS_EXIT_UNWIND
build_variant noroutine  -DSEH_HANDLERS_CONS_NOROUTINE

EXE="$OUT/seh_handlers_guest.exe"
MAP="$OUT/seh_handlers_guest.map"

# The linker's own statement of where each symbol landed.  Read from the map
# rather than guessed, and rather than taken from the running program: the point
# of layer 1b is to make a claim about the FILE ON DISK that does not depend on
# the port being correct about anything.
sym_rva() {
    awk -v s="$1" '$NF == s { print $1; exit }' "$MAP"
}
PF_CALL_RVA=$(sym_rva pf_call)
PF_CALL_END_RVA=$(sym_rva pf_call_end)
PF_HANDLER_RVA=$(sym_rva pf_language_handler)
PF_LANDING_RVA=$(sym_rva pf_landing)
PF_CHAIN_CALL_RVA=$(sym_rva pf_chain_call)
PF_CHAIN_END_RVA=$(sym_rva pf_chain_call_end)
PF_CHAIN_HANDLER_RVA=$(sym_rva pf_chain_handler)
PF_CHAIN_LANDING_RVA=$(sym_rva pf_chain_landing)
for v in PF_CALL_RVA PF_CALL_END_RVA PF_HANDLER_RVA PF_LANDING_RVA \
         PF_CHAIN_CALL_RVA PF_CHAIN_END_RVA PF_CHAIN_HANDLER_RVA PF_CHAIN_LANDING_RVA; do
    eval "x=\$$v"
    [ -n "$x" ] || skip "the link map names no $v; cannot state what the image \
should contain"
done

# ---- the PE reader used by layers 1 and 1b -------------------------------
# Reads DataDirectory[3] (IMAGE_DIRECTORY_ENTRY_EXCEPTION) straight out of the
# built image, walks the RUNTIME_FUNCTION array, and then does the thing
# llvm-readobj cannot: decodes the handler DATA that follows the handler RVA in
# the UNWIND_INFO as a SCOPE_TABLE and reports its contents.  That block is what
# the port hands a guest handler as DISPATCHER_CONTEXT->HandlerData, and the
# probe reads it back at run time; this layer states what it is supposed to
# find, from the file, before anything runs.
cat > "$OUT/xdata.py" <<'EOF'
import struct, sys

exe, want_begin, want_end, want_handler, want_landing = sys.argv[1:6]
want_begin, want_end = int(want_begin, 16), int(want_end, 16)
want_handler, want_landing = int(want_handler, 16), int(want_landing, 16)

data = open(exe, 'rb').read()
pe = struct.unpack_from('<I', data, 0x3c)[0]
assert data[pe:pe+4] == b'PE\0\0', 'not a PE'
nsec, = struct.unpack_from('<H', data, pe + 6)
optsz, = struct.unpack_from('<H', data, pe + 20)
opt = pe + 24
magic, = struct.unpack_from('<H', data, opt)
imagebase, = struct.unpack_from('<Q', data, opt + 24)
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

print('imagebase=0x%x' % imagebase)
print('exc_rva=0x%x exc_size=%u' % (exc_rva, exc_size))

n = exc_size // 12
ehandler = 0
handlers = {}
mine = None
mine_exists = False
for i in range(n):
    o = off(exc_rva + i * 12)
    if o is None:
        continue
    begin, end, unw = struct.unpack_from('<III', data, o)
    uo = off(unw)
    if uo is None:
        continue
    verflags, prolog, ncodes, frame = data[uo:uo+4]
    flags = (verflags >> 3) & 0x1f
    if begin == want_begin:
        mine_exists = True
    if not (flags & 0x3):          # UNW_FLAG_EHANDLER | UNW_FLAG_UHANDLER
        continue
    ehandler += 1
    # the handler RVA sits after the unwind codes, which are padded to an even
    # count -- getting that padding wrong reads the handler out of the codes
    hoff = uo + 4 + 2 * ((ncodes + 1) & ~1)
    handler, = struct.unpack_from('<I', data, hoff)
    handlers[handler] = handlers.get(handler, 0) + 1
    if begin == want_begin:
        mine = (begin, end, flags, handler, hoff + 4)

print('entries=%u ehandler=%u distinct_handlers=%u' % (n, ehandler, len(handlers)))
if mine is None:
    # Told apart on purpose.  "no_handler" is what a lost .seh_handler directive
    # looks like -- the frame is still described, it just names nobody -- and it
    # is a different defect from a frame with no .pdata entry at all.
    print('pf_entry=%s' % ('no_handler' if mine_exists else 'absent'))
    raise SystemExit(0)

begin, end, flags, handler, sdata = mine
print('pf_entry=present begin_matches=%s end_matches=%s flags=0x%x' %
      ('yes' if begin == want_begin else 'no',
       'yes' if end == want_end else 'no', flags))
print('pf_handler_rva=0x%x handler_is_pf_language_handler=%s frames_sharing_it=%u' %
      (handler, 'yes' if handler == want_handler else 'no', handlers[handler]))

count, = struct.unpack_from('<I', data, sdata)
print('scope_count=%u' % count)
for i in range(min(count, 4)):
    b, e, h, j = struct.unpack_from('<IIII', data, sdata + 4 + 16 * i)
    print('scope%u_begin=0x%x scope%u_end=0x%x scope%u_handler=%u '
          'scope%u_jumptarget_is_pf_landing=%s' %
          (i, b, i, e, i, h, i, 'yes' if j == want_landing else 'no'))
    print('scope%u_range_inside_frame=%s' %
          (i, 'yes' if want_begin <= b < e <= want_end else 'no'))
EOF

# ---- 1: shape ------------------------------------------------------------
XD=$(python3 "$OUT/xdata.py" "$EXE" "$PF_CALL_RVA" "$PF_CALL_END_RVA" \
             "$PF_HANDLER_RVA" "$PF_LANDING_RVA" 2>&1) || \
    skip "could not read the guest exception directory: $XD"
xd() { echo "$XD" | tr ' ' '\n' | sed -n "s/^$1=//p" | head -1; }

say "image: $(echo "$XD" | sed -n 2p)"
[ "$(xd imagebase)" = "$IMAGEBASE" ] || \
    bad "the image is based at $(xd imagebase), not $IMAGEBASE; the guest-pc \
assertions below are written for lld's default base"
case "$(xd exc_size)" in
    0) bad "the guest exe has an EMPTY exception directory" ;;
esac
EH=$(xd ehandler)
if [ "${EH:-0}" -lt 6 ]; then
    bad "only ${EH:-0} UNWIND_INFOs carry a language handler; the probe's __try \
blocks compiled away (expected at least 6)"
else
    say "image: $EH UNWIND_INFOs carry a language handler, naming \
$(xd distinct_handlers) distinct handlers"
fi
# Exactly three, and it is worth naming them: pf_language_handler,
# pf_chain_handler, and the imported __C_specific_handler that clang names in
# the .xdata of every __try in this file.  A fourth would mean clang emitted a
# handler this gate does not know about; a second would mean one of the two
# hand-written frames lost its own.
[ "$(xd distinct_handlers)" = 3 ] || \
    bad "the image names $(xd distinct_handlers) distinct language handlers; \
it should name exactly three -- the two private ones and the imported \
__C_specific_handler"

llvm-readobj --coff-imports "$EXE" > "$OUT/imports.txt" 2>&1
imported_from() {   # $1 = symbol -> prints the DLL that provides it
    awk -v s="$1" '/Name: .*\.dll/ { dll = $2 }
                   $0 ~ ("Symbol: " s " ") { print dll }' "$OUT/imports.txt"
}
for want in "__C_specific_handler ntdll.dll" "RtlUnwindEx kernel32.dll" \
            "RtlUnwindEx ntdll.dll"; do
    sym=${want% *}; dll=${want#* }
    if imported_from "$sym" | grep -qx "$dll"; then
        say "image: $sym is imported from $dll"
    else
        bad "the guest exe does not import $sym from $dll; the lane that needs \
it would be silently untested (imports seen: $(imported_from "$sym" | tr '\n' ' '))"
    fi
done

# ---- 1b: identity --------------------------------------------------------
# The premise of the whole gate.  Without it, a probe whose hand-written frames
# had silently lost their .seh_handler directives -- or whose handler RVAs had
# been resolved to the imported __C_specific_handler -- would sail through every
# layer below while testing the fast path a second time.
#
# Applied to BOTH hand-written frames: pf_call, whose handler decides for
# itself, and pf_chain_call, whose handler chains to ntdll's
# __C_specific_handler.  Their .xdata must name two DIFFERENT private functions
# and neither may name the import.
frame_identity() {   # $1 = frame symbol, $2 = its RVA, $3 = handler symbol
    fsym=$1; frva=$2; hsym=$3
    if [ "$(xd pf_entry)" = "no_handler" ]; then
        bad "$fsym's RUNTIME_FUNCTION (RVA $frva) exists but its UNWIND_INFO \
carries neither UNW_FLAG_EHANDLER nor UNW_FLAG_UHANDLER: the frame names NO \
language handler, so the frame walk would step straight past it and this gate \
would test nothing"
        return
    fi
    if [ "$(xd pf_entry)" != "present" ]; then
        bad "the image has no RUNTIME_FUNCTION beginning at $fsym (RVA $frva); \
the hand-written frame produced no .pdata entry and there is nothing for the \
frame walk to find"
        return
    fi
    say "image: $fsym's RUNTIME_FUNCTION: begin_matches=$(xd begin_matches) \
end_matches=$(xd end_matches) flags=$(xd flags)"
    [ "$(xd begin_matches)" = yes ] && [ "$(xd end_matches)" = yes ] || \
        bad "$fsym's RUNTIME_FUNCTION does not span the function the link map \
describes"
    case "$(xd flags)" in
        0x3) : ;;
        *) bad "$fsym's UNWIND_INFO has flags $(xd flags); it must carry both \
UNW_FLAG_EHANDLER and UNW_FLAG_UHANDLER (0x3) or the frame is invisible to one \
of the two phases" ;;
    esac
    if [ "$(xd handler_is_pf_language_handler)" = yes ]; then
        say "image: $fsym's .xdata names $hsym (RVA $(xd pf_handler_rva)), a \
handler of the probe's OWN -- this gate is testing the guest-entered path and \
not the __C_specific_handler identity path"
    else
        bad "$fsym's .xdata names handler RVA $(xd pf_handler_rva), not $hsym; \
the frame under test does not name a private handler and this gate would be a \
slower copy of check-seh-smoke.sh"
    fi
    # The same handler RVA appearing on other frames would mean the map lied
    # about which symbol that is, or that clang emitted our handler for its own
    # __try frames.  Exactly one frame may name it.
    [ "$(xd frames_sharing_it)" = 1 ] || \
        bad "$(xd frames_sharing_it) frames name $fsym's handler; exactly one \
frame in this image should"
    [ "$(xd scope_count)" = 1 ] || \
        bad "$fsym's handler data decodes as $(xd scope_count) scope records, \
not the one seh_handlers_asm.S emits; either the SCOPE_TABLE or the padding \
before it is wrong"
    [ "$(xd scope0_jumptarget_is_pf_landing)" = yes ] || \
        bad "$fsym's scope record's JumpTarget is not its landing pad; an \
unwind to it would resume somewhere else entirely"
    [ "$(xd scope0_range_inside_frame)" = yes ] || \
        bad "$fsym's scope record's guarded range is not inside the frame"
    [ "$(xd scope0_handler)" = 1 ] || \
        bad "$fsym's scope record's HandlerAddress is $(xd scope0_handler), not \
the EXCEPTION_EXECUTE_HANDLER (1) encoding seh_handlers_asm.S emits"
}

# The second opinion.  llvm-readobj decodes the same UNWIND_INFO through an
# entirely different implementation and resolves the handler through the image's
# COFF symbol table, so it names the handler rather than numbering it.  If the
# reader above and llvm-readobj disagree, one of them is wrong and the premise
# is not established either way.
llvm-readobj --unwind "$EXE" > "$OUT/unwind.txt" 2>&1
readobj_agrees() {   # $1 = frame symbol, $2 = handler symbol
    if awk -v f="StartAddress: $1 (" -v h="Handler: $2" \
           'index($0, f) { inpf = 1; next }
            inpf && index($0, "RuntimeFunction {") { inpf = 0 }
            inpf && index($0, h) { found = 1 }
            END { exit !found }' "$OUT/unwind.txt"; then
        say "image: llvm-readobj agrees -- $1's UnwindInfo says 'Handler: $2'"
    else
        grep -A20 "StartAddress: $1 (" "$OUT/unwind.txt" | sed 's/^/  unwind| /' >&2
        bad "llvm-readobj does not report $2 as $1's handler, so the two readers \
disagree about the premise of this gate"
    fi
}

frame_identity pf_call "$PF_CALL_RVA" pf_language_handler
readobj_agrees pf_call pf_language_handler

XD=$(python3 "$OUT/xdata.py" "$EXE" "$PF_CHAIN_CALL_RVA" "$PF_CHAIN_END_RVA" \
             "$PF_CHAIN_HANDLER_RVA" "$PF_CHAIN_LANDING_RVA" 2>&1) || \
    skip "could not read the guest exception directory for pf_chain_call: $XD"
frame_identity pf_chain_call "$PF_CHAIN_CALL_RVA" pf_chain_handler
readobj_agrees pf_chain_call pf_chain_handler
[ "$PF_HANDLER_RVA" = "$PF_CHAIN_HANDLER_RVA" ] && \
    bad "the two hand-written frames name the SAME handler RVA; one of the two \
.seh_handler directives is pointing at the wrong function and the chaining lane \
is not being tested"

# ---- the transcript every correct implementation must print --------------
# Embedded rather than captured.  Every line here is either a constant of the
# ABI, a constant of this program, a relation the x64 SEH contract fixes
# (ScopeIndex starts at 0; TargetIp is 0 during the search and the unwind's
# target during the unwind; EXCEPTION_UNWINDING is set in the second phase and
# not the first), or an ordering MSVC SEH defines (the search runs inner to
# outer, the unwind runs every __finally between the exception and the target,
# exactly once and abnormally, before the target resumes).
#
# Two relations in step 4 deserve their own note, because they are the ones a
# reader is most likely to think are wrong:
#
#   contextrecord_rip_is_controlpc_in_unwind=yes and, in the search phase, the
#   same relation does NOT hold -- which is why only the unwind-phase form is
#   printed.  During an unwind DISPATCHER_CONTEXT->ContextRecord describes the
#   frame whose handler is running; during the search it describes the frame the
#   walk has already stepped to, one frame further out.  That one-frame lag is
#   not this port's invention: Wine's own x86-64 dispatch has it too
#   (dlls/ntdll/signal_x86_64.c, call_seh_handlers, which sets
#   dispatch.ContextRecord = &context and passes orig_context to the handler),
#   and no MSVC handler notices, because the only thing one ever does with the
#   field is hand it to RtlUnwindEx, which immediately overwrites it with
#   RtlCaptureContext.  Recorded here so that a future change to it is loud.
#
#   ctxarg_is_contextrecord_in_unwind=yes says the third argument and
#   DISPATCHER_CONTEXT->ContextRecord are the SAME pointer during an unwind,
#   which is what makes a handler's "unwind from the context I was given" and
#   "unwind from the context in the dispatcher context" mean the same thing.
cat > "$OUT/expected.txt" <<'EOF'
seh_handlers: start
step 1 private handler: it ran, as x86-64 guest code, in both phases: search_calls=1 unwind_calls=1 witness_delta=2 ok
step 2 private handler: DISPATCHER_CONTEXT names this frame: imagebase_matches=yes functionentry_begin_matches=yes functionentry_end_matches=yes controlpc_in_frame=yes languagehandler_is_self=yes establisherframe_matches_arg=yes ok
step 3 private handler: HandlerData is this frame's own scope table: handlerdata_scope_count=1 controlpc_in_scope=yes scope0_handler=1 scope0_jumptarget_is_landing=yes ok
step 4 private handler: the phase-dependent fields differ by phase: scopeindex=0 targetip_zero_in_search=yes targetip_nonzero_in_unwind=yes unwinding_flag_in_unwind=yes target_unwind_flag_in_unwind=no contextrecord_rip_is_controlpc_in_search=no contextrecord_rip_is_controlpc_in_unwind=yes ctxarg_is_contextrecord_in_search=no ctxarg_is_contextrecord_in_unwind=yes ok
step 5 private handler: the record it was handed: code=0xe5e50001 nparam=2 info0=0x0a0a0001 info1=0x0a0a0002 ok
step 6 private handler: declining hands the exception to the enclosing __try: trace='araise ph-search afilt ph-unwind ahandler' ok
step 7 thread name, private handler: the idiom's record arrived whole: code=0x406d1388 nparam=4 info0=0x1000 name_matches=yes tid_matches=yes info3=0x0 ok
step 8 thread name, private handler: accepting unwinds into the frame: landed=1 rax_is_returnvalue=yes rbx_is_frame_sentinel=yes trace='braise ph-search ph-target landed' ok
step 9 thread name, private handler: continuing resumes after the raise: landed=0 trace='braise ph-search braise-after returned' ok
step 10 thread name, clang __try: accepting runs the __except body: trace='braise bfilt bhandler' ok
step 11 thread name, clang __try: continuing resumes after the raise: trace='braise bfilt braise-after returned' ok
step 12 thread name: the private handler and __C_specific_handler agree: code=yes flags=yes nparam=yes info=yes address=yes ok
step 13 RtlUnwindEx from a guest handler, imported from KERNEL32.dll: order: trace='craise ph-search midfin-abnormal ph-target landed' ok
step 14 RtlUnwindEx from a guest handler, imported from KERNEL32.dll: the intermediate __finally: calls=1 abnormal=1 abnormal_arg_agrees=yes ok
step 15 RtlUnwindEx from a guest handler, imported from KERNEL32.dll: arrival: landed=1 rax_is_returnvalue=yes rbx_is_frame_sentinel=yes pf_call_returned_rax=yes ok
step 16 RtlUnwindEx from a guest handler, imported from ntdll.dll: order: trace='craise ph-search midfin-abnormal ph-target landed' ok
step 17 RtlUnwindEx from a guest handler, imported from ntdll.dll: the intermediate __finally: calls=1 abnormal=1 abnormal_arg_agrees=yes ok
step 18 RtlUnwindEx from a guest handler, imported from ntdll.dll: arrival: landed=1 rax_is_returnvalue=yes rbx_is_frame_sentinel=yes pf_call_returned_rax=yes ok
step 19 chained handler: a private handler that tail-calls __C_specific_handler: chain_calls=2 trace='draise chain-search chain-target landed' ok
step 20 chained handler: __C_specific_handler served the guest's own call: landed=1 rax_is_exception_code=yes r12_is_frame_sentinel=yes pf_chain_call_returned_rax=yes ok
step 21 consolidating unwind: the __finally runs before the routine does: order: trace='craise ph-search midfin-abnormal ph-target consolidate landed-consolidate' finally_calls=1 abnormal=1 ok
step 22 consolidating unwind: the record the routine was handed: calls=1 code=0x80000029 nparam=11 unwinding_flag=yes info0_is_the_routine=yes info1_is_the_frame=yes info2_to_10_intact=yes ok
step 23 consolidating unwind: it resumed where the ROUTINE said: consolidate_landed=1 plain_landed=0 rax_is_returnvalue=yes rbx_is_frame_sentinel=yes pf_call_returned_rax=yes routine_calls=1 ok
step 24 consolidating unwind in place: a guest calling RtlUnwindEx itself: order: trace='fcall midfin-abnormal ph-target consolidate landed-consolidate' finally_calls=1 abnormal=1 establisherframe_is_frames_own_rsp=yes ok
step 25 consolidating unwind in place: the record and the arrival: routine_calls=1 nparam=11 unwinding_flag=yes info0_is_the_routine=yes info1_is_the_frame=yes info2_to_10_intact=yes consolidate_landed=1 rax_is_returnvalue=yes pf_call_returned_rax=yes ok
step 26 collided unwind, deferred road: a guest handler unwinds from inside the unwind: order: trace='graise ph-search kh-search gfilt kh-collide kh-readopted gmidfin ph-target landed' collide_calls=1 outer_filter_calls=1 outer_except_calls=0 ok
step 27 collided unwind, deferred road: the colliding frame is re-entered where it was left: readopt_calls=1 same_frame=yes same_scopeindex=yes collided_flag=yes target_unwind_flag_absent=yes targetip_is_the_new_target=yes code_is_the_inner_record=yes ok
step 28 collided unwind, deferred road: the inner unwind's target is reached: mid_finally=1 abnormal=1 arrive_calls=1 code_is_inner=yes info_is_inner=yes target_unwind_flag=yes landed=1 rax_is_returnvalue=yes rbx_is_frame_sentinel=yes pf_call_returned_rax=yes ok
step 29 collided unwind, in-place road: a __finally unwinds from inside the unwind: order: trace='graise gfilt gkfin gmidfin ph-target landed' collide_calls=1 outer_filter_calls=1 outer_except_calls=0 ok
step 30 collided unwind, in-place road: the scope that collided does not run again: k_finally=1 abnormal=1 mid_finally=1 abnormal=1 ok
step 31 collided unwind, in-place road: the inner unwind's target is reached: arrive_calls=1 code_is_inner=yes info_is_inner=yes target_unwind_flag=yes landed=1 rax_is_returnvalue=yes rbx_is_frame_sentinel=yes pf_call_returned_rax=yes ok
step 32 the private handler was entered this many times in all: witness_delta=17 ok
seh_handlers: PASS 32/32
EOF

# -all,err+seh: stdout stays clean enough to diff, while the port's own seh
# diagnostics still reach stderr -- a red gate that says nothing about why is
# only half a gate.  err+seh is appended to any caller-supplied WINEDEBUG
# rather than replaced by it: several verdicts below grep for the refusal
# text, and a caller exporting -all would otherwise turn a passing port into
# a false FAIL.
WDBG=${WINEDEBUG:--all},err+seh
run_wine() { timeout -k 5 "$2" \
                 env WINEDEBUG="$WDBG" WINEDLLOVERRIDES="winedbg.exe=d" \
                 "$BUILD/wine" "$1"; }

# ---- a control that must die: the shared assertions ----------------------
# Layers 5 and 6 differ only in what they expect to see NAMED.  What they share
# -- promptness, a nonzero exit, having reached the probe's first marker, and
# not having silently resumed -- is asserted once here, because each of those
# four has been a real failure mode of this port at some point and none of them
# should have to be re-argued per control.
DEADLINE=${DEADLINE:-20}
dying_control() {   # $1 = variant, $2 = human name, $3.. = regexps that must appear
    variant=$1; what=$2; shift 2
    started=$(date +%s)
    run_wine "$OUT/seh_handlers_$variant.exe" "$DEADLINE" \
        >"$OUT/$variant.out" 2>"$OUT/$variant.err"
    st=$?
    elapsed=$(( $(date +%s) - started ))
    if [ $st -eq 124 ] || [ $st -eq 137 ]; then
        bad "$what HUNG (killed after ${DEADLINE}s); this path must be prompt"
        tail -10 "$OUT/$variant.err" | sed "s/^/  $variant| /" >&2
        return
    fi
    if [ $st -eq 0 ]; then
        bad "$what exited 0; it must not be a silent success"
    else
        say "$what: exited $st after ${elapsed}s"
    fi
    if ! grep -q "^seh_handlers: .* probe" "$OUT/$variant.out"; then
        bad "$what never reached the probe's first marker; it died before the \
thing under test and proves nothing"
    fi
    if grep -q "seh_handlers: FAIL" "$OUT/$variant.out"; then
        bad "$what RESUMED past the point that must not return: \
$(grep -m1 'seh_handlers: FAIL' "$OUT/$variant.out")"
    fi
    for re in "$@"; do
        if grep -qEi -- "$re" "$OUT/$variant.err"; then
            say "$what names /$re/: $(grep -Eim1 -- "$re" "$OUT/$variant.err" | cut -c1-110)"
        else
            sed "s/^/  $variant| /" "$OUT/$variant.err" >&2
            bad "$what does not name /$re/ anywhere in the port's diagnostics; \
a refusal nobody can read is not a refusal"
        fi
    done
}

# ---- 5 (also available standalone as --sabotage): the negative control ----
#
# A genuine machine fault, under a frame whose private language handler RAN and
# said ExceptionContinueSearch.  That is the arrangement that matters: declining
# is what a real __GSHandlerCheck answers for every exception it does not own,
# and a port that lost the exception at the declining frame -- or that treated
# "a handler ran" as "the exception was handled" -- would look identical from
# the outside to one that worked, except that nothing would ever die.  The three
# assertions are check-seh-smoke.sh's: prompt, nonzero, and naming the fault at
# the GUEST pc rather than at some address inside the JIT.
neg_control() {
    dying_control unhandled "the unhandled fault" \
        "c0000005|page fault on write access to 0000000000000000" \
        "00000001400[0-9a-fA-F]{5}"
}

if [ "$SABOTAGE" = 1 ]; then
    neg_control
    [ $fail -eq 0 ] && say "SABOTAGE PASS"
    exit $fail
fi

# ---- 2: guest ------------------------------------------------------------
run_wine "$EXE" "$TIMEOUT" > "$OUT/guest.out" 2>"$OUT/guest.err"
gst=$?
if [ $gst -eq 124 ] || [ $gst -eq 137 ]; then
    bad "the guest run timed out after ${TIMEOUT}s"
elif grep -q "seh_handlers: PASS" "$OUT/guest.out"; then
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

# ---- where check-seh-smoke.sh's native lane would be ----------------------
# Said out loud on every run, in the slot the sister gate uses for it, so that
# its absence is a statement rather than a hole somebody has to notice.
note "native: not expressible.  The construct under test is a hand-written \
x86-64 .seh_proc with a .seh_handler directive and an @IMGREL SCOPE_TABLE; gcc \
on ppc64 has no MSVC structured exception handling at all and Wine's setjmp \
__TRY macros, which give check-seh-smoke.sh its native lane, cannot express a \
PRIVATE language handler in any form.  There is no second implementation to \
corroborate against, so layer 3 is the whole value gate -- and steps 7-12 of \
the transcript stand in for the corroboration by driving the SAME exception \
through the private handler and through ntdll's __C_specific_handler and \
requiring the two to have been handed the same record."
note "check-seh-smoke.sh is a SEPARATE gate and is deliberately not run from \
here: a gate should have one owner for its red state, and folding the identity \
path in would mean a failure of the native __C_specific_handler implementation \
turning this file red.  Steps 10-12 above already drive an ordinary clang \
__try -- the identity fast path -- in the same process as the private one, so \
the overlap this gate depends on cannot hide.  Run both."

# ---- 4: the port's own view ----------------------------------------------
# The probe's own witness counter says "code inside pf_language_handler ran".
# It cannot say through WHICH door: guest memory is host memory on this port, so
# in principle something else could have called it.  The port's trace closes
# that: call_guest_language_handler() logs "entering guest language handler %p"
# immediately before it enters the address as guest code, and the address it
# logs must be the one the IMAGE's .xdata names -- which layer 1b read off the
# file on disk, from the link map, without running anything.  Two independent
# measurements of the same event, required to agree on the address.
#
# A separate run at +seh rather than reusing the layer-2 run, because +seh is
# very loud and layer 3 needs a stdout that is clean enough to diff.
# Matched case-insensitively and with the leading zeroes optional, because how
# many of them a %p carries is a property of the host's printf and not of the
# port; what must be exact is the ADDRESS, and the trailing colon keeps
# 0x140001000 from matching 0x1400010005.
PF_HANDLER_ADDR=$(printf '%x' $(( IMAGEBASE + 0x$PF_HANDLER_RVA )))
timeout -k 5 "$TIMEOUT" env WINEDEBUG=+seh WINEDLLOVERRIDES="winedbg.exe=d" \
    "$BUILD/wine" "$EXE" >/dev/null 2>"$OUT/seh.trace"
ENTER_RE="entering guest language handler (0x)?0*$PF_HANDLER_ADDR:"
ENTERED=$(grep -Eci -- "$ENTER_RE" "$OUT/seh.trace")
if [ "${ENTERED:-0}" -ge 1 ]; then
    say "port: the frame walk logged $ENTERED entries into guest language \
handler 0x$PF_HANDLER_ADDR, which is exactly the address pf_call's .xdata names"
else
    grep -m5 "entering guest language handler" "$OUT/seh.trace" | \
        sed 's/^/  trace| /' >&2
    bad "the port never logged entering the guest language handler at \
0x$PF_HANDLER_ADDR; the probe's witness counter says its code ran, but the \
port's own record does not say the frame walk is what ran it"
fi
# ...and it must be entered exactly as often as the probe counted, or the two
# instruments are measuring different things.
if [ "${ENTERED:-0}" -ne 17 ]; then
    bad "the port entered the private handler ${ENTERED:-0} times; the probe's \
own witness counted 17 (step 32).  Two instruments, one event, two answers"
fi

# The COLLIDED UNWIND, from the port's own side.  Steps 26-31 prove the guest
# observed one; this proves the port's unwind loop is what produced it, and
# names the two roads separately -- the trace that says a guest handler's
# RtlUnwindEx was recognised as a collision, and the trace that says a
# __finally's was.  Two of them, one per stage, because a port that served both
# stages down ONE road would satisfy every value check in the transcript.
# One ADOPTION per stage, in the unwind loop, and exactly one of the two
# arriving down the funclet road -- guest_request_unwind's own trace, which only
# the in-place road reaches.  A port that served both stages down one road would
# satisfy every value check in the transcript and fail here.
ADOPTED=$(grep -c "collided unwind: frame .* started a second unwind" "$OUT/seh.trace")
FINCOLL=$(grep -c "collided unwind: a __finally of the unwind to frame" "$OUT/seh.trace")
if [ "${ADOPTED:-0}" -eq 2 ] && [ "${FINCOLL:-0}" -eq 1 ]; then
    say "port: two collisions adopted, one per road -- a guest handler's own \
RtlUnwindEx (deferred) and a __finally funclet's (in place, recognised in \
guest_request_unwind)"
else
    grep -m4 "collided unwind" "$OUT/seh.trace" | sed 's/^/  trace| /' >&2
    bad "the port adopted ${ADOPTED:-0} collisions of which ${FINCOLL:-0} came \
down the funclet road; the probe drives exactly two, one per road (steps 26 and \
29).  Either a collision was served as something else, or one road served both"
fi
# THE SCOPE INDEX, from the port's own mouth.  The in-place stage's colliding
# frame is a scope table with one __finally, so the unwind that adopts it must
# resume at scope 1 -- the index the collision left it at.  An adoption that
# re-derived the dispatcher context would say "scope 0" here and would run that
# __finally a second time, which is exactly what step 30 counts from the other
# side.  Two instruments, one number.
SCOPE1=$(grep -c "started a second unwind .* adopting it at scope 1" "$OUT/seh.trace")
if [ "${SCOPE1:-0}" -eq 1 ]; then
    say "port: the funclet-road collision was adopted at scope 1 -- the index the \
unwind it collided with had already advanced to"
else
    grep -m4 "adopting it at scope" "$OUT/seh.trace" | sed 's/^/  trace| /' >&2
    bad "the port adopted ${SCOPE1:-0} collisions at scope 1; the in-place stage's \
colliding frame has one __finally and must be resumed at the scope AFTER it"
fi
# ...and the refusal that used to stand in for all of this must be GONE, in
# both of its wordings, from a run that passes.
if grep -qi "is not implemented" "$OUT/guest.err" || \
   grep -qi "is not implemented" "$OUT/seh.trace"; then
    grep -im3 "is not implemented" "$OUT/guest.err" "$OUT/seh.trace" | sed 's/^/  port| /' >&2
    bad "the port still refuses something by name during the PASSING run; the \
collided unwind is implemented and nothing in these stages may reach a refusal"
fi

# The consolidating unwind's own trace line, for the same reason: the probe's
# routine counter says its code ran, and only the port can say the FRAME WALK
# entered it as the last step of an unwind rather than something else calling
# it.  guest_consolidate_callback() logs the routine's address immediately
# before entering it as guest code.
PF_CONS_RVA=$(sym_rva pf_consolidate)
if [ -n "$PF_CONS_RVA" ]; then
    PF_CONS_ADDR=$(printf '%x' $(( IMAGEBASE + 0x$PF_CONS_RVA )))
    CONS=$(grep -Eci -- "consolidating unwind: entering routine (0x)?0*$PF_CONS_ADDR " \
                "$OUT/seh.trace")
    if [ "${CONS:-0}" -eq 2 ]; then
        say "port: the unwind logged entering consolidation routine \
0x$PF_CONS_ADDR twice -- once down each of the port's two unwind roads, \
deferred and in place -- at the address the probe put in \
ExceptionInformation[0]"
    else
        grep -m3 "consolidating unwind" "$OUT/seh.trace" | sed 's/^/  trace| /' >&2
        bad "the port logged entering the consolidation routine at \
0x$PF_CONS_ADDR ${CONS:-0} times; the probe drives it twice, once per unwind \
road (steps 23 and 25).  Two instruments, one event, two answers"
    fi
else
    bad "the link map names no pf_consolidate; cannot state which address the \
port should have entered"
fi

# The same pair of measurements for the CHAINING frame, plus the one that says
# which door its chain came in by.  emu_C_specific_handler() is the thunk
# override a guest CALL to ntdll's __C_specific_handler lands in -- a different
# entry point from the frame walk's identity fast path, which never fires here
# because pf_chain_call's .xdata names pf_chain_handler.  Its TRACE is the only
# place that distinguishes the two, so the count is asserted rather than the
# mere presence.
PF_CHAIN_HANDLER_ADDR=$(printf '%x' $(( IMAGEBASE + 0x$PF_CHAIN_HANDLER_RVA )))
CHAIN_ENTERED=$(grep -Eci -- \
    "entering guest language handler (0x)?0*$PF_CHAIN_HANDLER_ADDR:" "$OUT/seh.trace")
if [ "${CHAIN_ENTERED:-0}" -eq 2 ]; then
    say "port: the frame walk logged $CHAIN_ENTERED entries into the CHAINING \
guest language handler 0x$PF_CHAIN_HANDLER_ADDR, one per phase"
else
    bad "the port entered the chaining handler at 0x$PF_CHAIN_HANDLER_ADDR \
${CHAIN_ENTERED:-0} times; the probe counted 2 (step 19), one per phase"
fi
SERVED=$(grep -c "guest __C_specific_handler:" "$OUT/seh.trace")
if [ "${SERVED:-0}" -eq 2 ]; then
    say "port: emu_C_specific_handler served $SERVED direct guest calls to \
__C_specific_handler -- the chain reached the port through the thunk override, \
not through the frame walk's identity check"
else
    bad "emu_C_specific_handler served ${SERVED:-0} direct guest calls to \
__C_specific_handler; the chaining handler makes exactly 2 (one per phase).  \
Either the chain never reached the port, or something else is calling it"
fi
# ...and it must not have been REFUSED.  Serving it wrongly is quiet by nature:
# an ExceptionContinueSearch to a frame that is entitled to its __except looks
# from the outside exactly like a frame that had no handler.  So the assertion
# is on the err channel, which is where every refusal in this port lands.
if grep -q "err:.*__C_specific_handler" "$OUT/guest.err"; then
    grep -m3 "err:.*__C_specific_handler" "$OUT/guest.err" | sed 's/^/  guest| /' >&2
    bad "the port logged an ERROR about __C_specific_handler during the passing \
run; a guest calling it directly must be SERVED, not refused"
else
    say "port: no err-level diagnostic names __C_specific_handler -- the direct \
guest call was served rather than refused"
fi

# ---- 6: the refusals -----------------------------------------------------
# Two states the port refuses BY NAME rather than answering wrongly.  Each is
# reached from the private handler, which is the only place a guest can produce
# them, and each must die the same way an unhandled exception does.
#
# Said out loud rather than left to be noticed: the state that used to be the
# fourth refusal in this family -- an exception raised while an unwind is
# ALREADY in progress on the thread -- is no longer refused at all.  A funclet
# runs in a nested emulator run with a guest stack of its own, so a fault inside
# one is dispatched over THAT stack and cannot reach the frames the unwind has
# torn down; and a funclet that unwinds out past its own run is the collided
# unwind steps 26-31 gate.  What is still refused there is a fault on the very
# stack being unwound, which no guest code should be able to reach, and it has
# no layer here because producing it would mean corrupting the port on purpose.
refusals() {
    # NOT "collided unwinds are not implemented" any more -- that state is steps
    # 26 to 31 of the passing transcript.  What is left is the DEGENERATE form:
    # a handler that returns the disposition and hands back a dispatcher context
    # that never advances, which cannot be adopted twice without re-entering it
    # forever.  The third pattern asserts the old refusal's wording is gone, so
    # that a port which quietly went back to refusing the whole family cannot
    # pass this control by accident.
    dying_control collided "the non-advancing collided unwind" \
        "ExceptionCollidedUnwind" \
        "does not advance" \
        "00000001400[0-9a-fA-F]{5}"
    if grep -qi "collided unwind.*is not implemented" "$OUT/collided.err"; then
        bad "the port still says collided unwinds are not implemented; the \
control is supposed to be refused for NOT ADVANCING, not for the feature being \
missing"
    fi
    dying_control exitunwind "the exit-unwind refusal" \
        "EXIT unwind names no frame"
    dying_control noroutine "the routineless consolidating unwind" \
        "STATUS_UNWIND_CONSOLIDATE" \
        "names no consolidation routine"
}

neg_control
refusals

[ $fail -eq 0 ] && say "PASS"
exit $fail
