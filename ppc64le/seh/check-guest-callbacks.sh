#!/bin/sh
#
# check-guest-callbacks.sh -- the gate for two guest/native boundary contracts
# that dlls/ntdll/signal_ppc64.c and dlls/ntdll/unix/loader.c own:
#
#   A. a guest function pointer handed to native code through a STRUCT FIELD
#      (WNDCLASSEXW.lpfnWndProc, WNDCLASSW.lpfnWndProc) or through a return
#      value native code hands BACK to the guest (SetWindowLongPtrW's and
#      GetWindowLongPtrW's previous/current WNDPROC) must be swapped for a
#      native trampoline exactly as reliably as the ARGUMENT-shaped callbacks
#      thunk_overrides[]'s cb_mask already covers.  DOOM (2016) died on
#      exactly this gap: its WNDPROC went through RAW inside WNDCLASSEXW,
#      native user32 stored the guest address as an ordinary function
#      pointer, and the first WM_NCCREATE was a ppc64 bctrl into x86-64
#      bytes.  Worse, KiUserCallbackDispatcher's own dispatch_user_callback()
#      (dlls/ntdll/exception.c) wraps every callback in __TRY/__EXCEPT_ALL
#      and SWALLOWS what came out --
#      "err:seh:dispatch_user_callback ignoring exception c000001d" -- so the
#      game got a window that never received a message and no error at all.
#      A second, narrower defect rides along: an LRESULT is a genuine 64-bit
#      value, and the trampoline pool's return path used to always
#      sign-extend the low 32 bits.
#
#      The mechanism, as landed: wrap_guest_callback_ex(fn, wide) is the
#      trampoline pool, now with a PER-SLOT return width (one stub per
#      (target, width) pair) -- wrap_guest_wndproc() is the WNDPROC-specific
#      entry point that always asks for the wide (64-bit) slot.  New
#      thunk_overrides[] rows drive it from every registration site this
#      probe exercises: emu_RegisterClassEx / emu_RegisterClass (copy the
#      WNDCLASSEXW/WNDCLASSW, swap lpfnWndProc, hand the copy to native) and
#      emu_SetWindowLongPtr (swaps only when the index is GWLP_WNDPROC),
#      plus emu_CallWindowProc, which wraps its OWN first argument -- the one
#      entry point that cannot be caught at any earlier registration, because
#      a guest program handing CallWindowProcW its own already-known function
#      pointer was never "registered" anywhere.  wrap_guest_wndproc() also
#      passes two shapes through untouched and quietly (TRACE, not WARN,
#      because neither is a defect): a win32u WINPROC handle
#      (value>>16 == 0xffff) and an ordinary native window procedure.  Which
#      of the three shapes a readback turns out to be is therefore NOT
#      asserted by this gate -- only that calling it works.
#
#   B. CreateThread's dwStackSize must reach the SEPARATE guest stack the
#      embedded emulator allocates for a thread's x86-64 side -- not just the
#      native ppc64 stack Wine has always sized correctly.  Until fixed,
#      emu_run_loop() (dlls/ntdll/unix/loader.c) always sized the guest stack
#      from the image's own SizeOfStackReserve, regardless of what
#      CreateThread was asked for.  DOOM (2016) asks its worker threads for
#      8 MiB with STACK_SIZE_PARAM_IS_A_RESERVATION and says so in its own
#      log -- "Starting stack size in KB: 8388608" -- and got the image's
#      2 MiB default instead, silently.  The mechanism, as landed: the guest
#      stack is now sized by MIRRORING the thread's own native stack, which
#      is where Wine already applies dwStackSize and
#      STACK_SIZE_PARAM_IS_A_RESERVATION -- so no new plumbing carries the
#      value across the guest/native boundary a second time, the existing
#      native thread-stack sizing IS the source of truth.  Traced on the
#      MODULE channel (not seh): "emu_run_loop guest stack for <entry>: ...
#      (<N> bytes, from this thread's own stack)", or "... from the image"
#      when no native thread stack was captured, or under the sabotage below.
#
# Both are gated from ONE guest process, guest_callbacks.c, built the same way
# check-seh-handlers.sh builds its probe: clang -target x86_64-windows-gnu, no
# CRT, the image entry point IS the program.  See that file's own header
# comment for the constants and the reasoning behind each one; this script
# only builds it, runs it, and reads its transcript and the port's own trace.
#
# Layers:
#
#   1  BUILD: the guest PE compiles, links, and imports exactly the functions
#      this probe calls from exactly the DLLs it claims to import them from.
#      Cheap, and it is the layer that catches "linked the wrong import
#      library" silently passing every layer below for the wrong reason.
#   2  GUEST: the probe runs under the emulator and prints PASS.  Every step
#      checks a value or a relation -- see guest_callbacks.c.
#   3  TRANSCRIPT: stdout is byte-identical to the transcript embedded below.
#   4  THE PORT'S OWN VIEW, part A: a separate WINEDEBUG=+seh run must show
#      wrap_guest_callback_ex()'s "guest callback %p -> trampoline %p (%u
#      total, 64-bit return)" TRACE exactly three times -- one per DISTINCT
#      guest WNDPROC this probe registers (gc_wndproc_a via RegisterClassExW,
#      gc_wndproc_b via RegisterClassW, gc_wndproc_c via SetWindowLongPtrW;
#      trampolines are per-target and idempotent, so re-use of an
#      already-wrapped pointer, which this probe also exercises, does NOT
#      grow this count) -- each tagged "64-bit return", proving the WIDE slot
#      is the one a WNDPROC actually gets, not the sign-extending one every
#      other callback class uses.  emu_RegisterClassEx, emu_RegisterClass,
#      emu_SetWindowLongPtr and emu_CallWindowProc must each show exactly as
#      many entries as this probe made calls of that shape (1, 1, 1, 3 --
#      fully controlled by this probe, unlike the message counts below, and
#      so asserted as exact constants).  guest_callback_run()'s "calling
#      guest callback %p ..." TRACE must appear exactly as many times as this
#      probe's own total-dispatch witness counted, READ FROM THE PROBE'S OWN
#      TRANSCRIPT rather than hard-coded here: unlike the four counts above,
#      which this probe fully controls, the exact number of messages
#      CreateWindowExW privately sends around WM_NCCREATE/WM_CREATE is
#      user32's own business and not this script's to predict.  The same
#      run's stderr must contain NO err-level line naming "ignoring
#      exception" or "c000001d": the failure this gate exists to catch is
#      SILENT by construction, so a passing run that quietly swallowed one
#      anyway must not be mistaken for success.
#   5  THE PORT'S OWN VIEW, part B: a WINEDEBUG=trace+module run must show
#      "emu_run_loop guest stack for ...: ... (16777216 bytes, from this
#      thread's own stack)" exactly once -- the big-stack thread's own
#      request, and the ONLY 16 MiB request anywhere in this probe.
#
# --sabotage (also the standalone flag) runs BOTH of this port's existing
# negative controls, one per mechanism, because either could be fixed while
# the other stays broken and a gate that only tries one lever would not know:
#
#   WINEEMUNOCBWRAP=1 (dlls/ntdll/signal_ppc64.c, wrap_guest_callback_ex) hands
#   the RAW guest pointer to native code on every registration -- the WNDPROC
#   defect itself, switched back on.  Under it the run MUST fail: prompt (not
#   a hang -- see the WINEDLLOVERRIDES note below), nonzero, never printing
#   PASS, and the port must SAY something (the ERR wrap_guest_callback_ex
#   itself prints, naming WINEEMUNOCBWRAP, or the port's own crash/exception
#   report, or both).  Expected to die inside PART A, before Part B's threads
#   are even created -- either at the first WM_NCCREATE a raw guest pointer
#   cannot survive, or earlier if dispatch_user_callback's own
#   swallow-and-continue leaves the window subsystem in a state
#   CreateWindowExW itself refuses.  So this lever alone says nothing about
#   Part B, which is exactly why there is a second one.
#
#   WINEEMUNOSTACKSIZE=1 (dlls/ntdll/unix/loader.c, emu_run_loop) sizes every
#   guest stack from the image, ignoring the thread's own -- the stack-size
#   defect itself, switched back on.  Unlike the lever above, this one does
#   NOT crash the process: guest_callbacks.c's own defensive design (see its
#   header comment on "refuse loudly rather than guess") notices the TEB bound
#   check has failed and skips the dangerous deep recursion rather than
#   attempting it against a stack that was never actually widened, so the run
#   ends with a clean, prompt, nonzero exit and an ordinary "guest_callbacks:
#   FAIL" rather than a signal.  Asserted here: Part A (steps 1-14) is
#   UNAFFECTED (this lever touches only Part B, proving the two mechanisms
#   are independent of one another in the port as well as in this gate), the
#   big-stack thread's own bound check (step 17) is the one that fails, and
#   it fails specifically by reporting the 2 MiB image default rather than
#   some other wrong number.
#
# WHY EVERY RUN DISABLES winedbg, verbatim from check-seh-handlers.sh: the
# bringup prefix has AeDebug configured with "winedbg --auto", so an unhandled
# guest fault -- and Part A's whole point, before the fix, is to produce one --
# would otherwise start the debugger, which attaches and never lets go, turning
# every red state this gate is SUPPOSED to reach into a hang instead of a
# result.  WINEDLLOVERRIDES=winedbg.exe=d makes start_debugger's CreateProcess
# fail, so UnhandledExceptionFilter falls straight through to terminating the
# process.  An environment override for one run; nothing in the prefix changes.
#
# Exit 0 = pass, 1 = a check failed, 2 = could not run (not a pass).
set -u

HERE=$(cd "$(dirname "$0")" && pwd)
SRC=$(cd "$HERE/../.." && pwd)
BUILD=${BUILD:-$SRC}
OUT=${OUT:-/tmp/guest-callbacks}
SABOTAGE=0
[ "${1:-}" = "--sabotage" ] && SABOTAGE=1

say()  { echo "check-guest-callbacks: $*"; }
bad()  { echo "check-guest-callbacks: FAIL $*" >&2; fail=1; }
note() { echo "check-guest-callbacks: note $*"; }
skip() { echo "check-guest-callbacks: $*" >&2; exit 2; }

[ -x "$BUILD/wine" ] || skip "no wine loader at $BUILD/wine"
[ -n "${WINEPREFIX:-}" ] || skip "set WINEPREFIX to a prefix wineboot has run in"
# Required from the environment rather than defaulted: which FEX build the
# port is bridged to is a property of the machine, not of this gate.
[ -n "${WINEFEXBRIDGE:-}" ] || skip "set WINEFEXBRIDGE to the emulator bridge"
[ -f "$BUILD/dlls/kernel32/x86_64-windows/kernel32.dll" ] || \
    skip "no guest kernel32 thunk; build it first"
[ -f "$BUILD/dlls/user32/x86_64-windows/user32.dll" ] || \
    skip "no guest user32 thunk; build it first"
command -v clang >/dev/null || skip "need clang for the guest build"
command -v llvm-dlltool >/dev/null || skip "need llvm-dlltool for the guest build"
command -v llvm-readobj >/dev/null || skip "need llvm-readobj to read the built image"

mkdir -p "$OUT" || skip "cannot create $OUT"
fail=0

INCL="-I$BUILD/include -I$SRC/include -I$SRC/include/msvcrt"
TIMEOUT=${TIMEOUT:-120}

# ---- build: the x86-64 guest PE ------------------------------------------
# One image, no variants: unlike check-seh-handlers.sh's controls, this
# probe's negative control (--sabotage) is an environment variable the port
# itself already reads (WINEEMUNOCBWRAP), not a different compiled shape, so
# there is nothing here that needs a second build.
cat > "$OUT/kernel32.def" <<'EOF'
LIBRARY kernel32.dll
EXPORTS
GetStdHandle
WriteFile
ExitProcess
GetModuleHandleW
CreateThread
WaitForSingleObject
EOF
cat > "$OUT/user32.def" <<'EOF'
LIBRARY user32.dll
EXPORTS
RegisterClassExW
RegisterClassW
CreateWindowExW
SendMessageW
SetWindowLongPtrW
GetWindowLongPtrW
CallWindowProcW
DefWindowProcW
EOF
for m in kernel32 user32; do
    llvm-dlltool -m i386:x86-64 -d "$OUT/$m.def" -l "$OUT/lib$m.a" \
        || skip "llvm-dlltool failed for $m"
done

GUESTCC="clang -target x86_64-windows-gnu -nostdlibinc $INCL -fms-extensions \
-D_UCRT -Wall -O1 -fno-builtin -g"
# --stack,reserve,commit sets this image's OWN default guest-stack size to
# 2 MiB -- chosen to echo DOOM's own 2 MiB default rather than lld's arbitrary
# 1 MiB, and load-bearing: guest_callbacks.c's control-thread step asserts its
# reserve equals EXACTLY 2 MiB, which only means something because this link
# line is what makes that number true rather than incidental.
# -Xlinker rather than -Wl, for --stack specifically: clang's -Wl, splits on
# EVERY comma before handing tokens to the linker, which would turn
# "--stack,0x200000,0x200000" into three separate argv entries -- "--stack",
# "0x200000", "0x200000" -- and GNU ld's (and lld's GNU-compatible driver's)
# --stack takes ONE value that may itself contain a comma (reserve[,commit]),
# so the split second "0x200000" gets read as a stray input FILE instead
# ("ld.lld: error: could not open '0x200000'").  -Xlinker passes its argument
# through untouched, so "0x200000,0x200000" survives as a single token.
GUESTLD="clang -target x86_64-windows-gnu -fuse-ld=lld -nostdlib \
-Wl,--entry=guest_callbacks_entry -Wl,--subsystem,console \
-Xlinker --stack -Xlinker 0x200000,0x200000"

$GUESTCC -c -o "$OUT/guest_callbacks.o" "$HERE/guest_callbacks.c" \
    || skip "guest compile failed"
$GUESTLD -o "$OUT/guest_callbacks.exe" "$OUT/guest_callbacks.o" \
    "$OUT/libkernel32.a" "$OUT/libuser32.a" \
    -Wl,-Map,"$OUT/guest_callbacks.map" \
    || skip "guest link failed"

EXE="$OUT/guest_callbacks.exe"

# ---- 1: build/shape --------------------------------------------------
# The lightweight cousin of check-seh-handlers.sh's layer 1: no unwind-data
# decoding is needed here (nothing in this probe carries a language handler),
# but a probe that silently linked against the wrong import library -- or
# that clang decided to resolve one of these calls to something other than an
# import thunk -- would pass every layer below for the wrong reason.
llvm-readobj --coff-imports "$EXE" > "$OUT/imports.txt" 2>&1
imported_from() {   # $1 = symbol -> prints the DLL that provides it
    awk -v s="$1" '/Name: .*\.dll/ { dll = $2 }
                   $0 ~ ("Symbol: " s " ") { print dll }' "$OUT/imports.txt"
}
for want in \
    "RegisterClassExW user32.dll" "RegisterClassW user32.dll" \
    "CreateWindowExW user32.dll" "SendMessageW user32.dll" \
    "SetWindowLongPtrW user32.dll" "GetWindowLongPtrW user32.dll" \
    "CallWindowProcW user32.dll" "DefWindowProcW user32.dll" \
    "CreateThread kernel32.dll" "WaitForSingleObject kernel32.dll"
do
    sym=${want% *}; dll=${want#* }
    if imported_from "$sym" | grep -qx "$dll"; then
        say "image: $sym is imported from $dll"
    else
        bad "the guest exe does not import $sym from $dll; the layer that \
needs it would be silently untested (imports seen: $(imported_from "$sym" | tr '\n' ' '))"
    fi
done

# -all,err+seh: stdout stays clean enough to diff, while the port's own seh
# and callback-wrapping diagnostics still reach stderr.  Appended to any
# caller-supplied WINEDEBUG rather than replacing it, exactly as in
# check-seh-handlers.sh, for the same reason: a caller exporting -all should
# not turn a passing port into a false FAIL here either.
WDBG=${WINEDEBUG:--all},err+seh
run_wine() { timeout -k 5 "$2" \
                 env WINEDEBUG="$WDBG" WINEDLLOVERRIDES="winedbg.exe=d" \
                 "$BUILD/wine" "$1"; }

# ---- --sabotage lever 1: the WNDPROC defect, switched back on -------------
# (also runnable standalone via --sabotage)
sabotage_cbwrap() {
    started=$(date +%s)
    timeout -k 5 "${DEADLINE:-20}" \
        env WINEDEBUG="$WDBG" WINEDLLOVERRIDES="winedbg.exe=d" WINEEMUNOCBWRAP=1 \
        "$BUILD/wine" "$EXE" >"$OUT/cbwrap.out" 2>"$OUT/cbwrap.err"
    st=$?
    elapsed=$(( $(date +%s) - started ))
    if [ $st -eq 124 ] || [ $st -eq 137 ]; then
        bad "WINEEMUNOCBWRAP=1 HUNG (killed after ${DEADLINE:-20}s); the raw-pointer \
path must fail promptly, not hang"
        tail -10 "$OUT/cbwrap.err" | sed 's/^/  cbwrap| /' >&2
        return
    fi
    if [ $st -eq 0 ]; then
        bad "WINEEMUNOCBWRAP=1 exited 0; the raw-pointer path must not be a silent success"
    else
        say "WINEEMUNOCBWRAP=1: exited $st after ${elapsed}s"
    fi
    if ! grep -q "^guest_callbacks: start" "$OUT/cbwrap.out"; then
        bad "WINEEMUNOCBWRAP=1 never reached the probe's first marker; it \
died before the thing under test and proves nothing"
    fi
    if grep -q "guest_callbacks: PASS" "$OUT/cbwrap.out"; then
        bad "WINEEMUNOCBWRAP=1 still printed PASS; the raw-pointer defect did not \
reach anything this probe checks"
    fi
    if grep -qE "WINEEMUNOCBWRAP|c000001d|ignoring exception|illegal instruction" \
            "$OUT/cbwrap.err"; then
        say "WINEEMUNOCBWRAP=1: the port said something: $(grep -Eim1 \
            'WINEEMUNOCBWRAP|c000001d|ignoring exception|illegal instruction' \
            "$OUT/cbwrap.err" | cut -c1-120)"
    else
        sed 's/^/  cbwrap| /' "$OUT/cbwrap.err" >&2
        bad "WINEEMUNOCBWRAP=1 died without the port naming why anywhere in its \
diagnostics; a silent death here is exactly the failure mode this \
whole gate exists to make loud"
    fi
}

# ---- --sabotage lever 2: the stack-size defect, switched back on ----------
# (also runnable standalone via --sabotage)
#
# Unlike lever 1, this one is NOT expected to crash -- see the header comment.
# guest_callbacks.c's own step 17/18 guard notices the reservation bound
# failed and skips the dangerous recursion, so the process ends promptly and
# gracefully with a nonzero exit and an ordinary FAIL line, not a signal.
# trace+module (rather than the +seh this script uses elsewhere) is what
# shows the port's own acknowledgement: emu_run_loop's guest-stack TRACE
# switches from "from this thread's own stack" to "from the image" under this
# lever, for every guest run in the process, not only the sabotaged thread's.
sabotage_stacksize() {
    started=$(date +%s)
    timeout -k 5 "${DEADLINE:-20}" \
        env WINEDEBUG="${WINEDEBUG:--all},trace+module" \
            WINEDLLOVERRIDES="winedbg.exe=d" WINEEMUNOSTACKSIZE=1 \
        "$BUILD/wine" "$EXE" >"$OUT/stacksize.out" 2>"$OUT/stacksize.err"
    st=$?
    elapsed=$(( $(date +%s) - started ))
    if [ $st -eq 124 ] || [ $st -eq 137 ]; then
        bad "WINEEMUNOSTACKSIZE=1 HUNG (killed after ${DEADLINE:-20}s); this probe's \
own defensive skip (see its header comment) is specifically what should keep \
this path from ever needing to hang"
        tail -10 "$OUT/stacksize.err" | sed 's/^/  stacksize| /' >&2
        return
    fi
    if [ $st -eq 0 ]; then
        bad "WINEEMUNOSTACKSIZE=1 exited 0; the image-default-only path must \
not be a silent success"
    else
        say "WINEEMUNOSTACKSIZE=1: exited $st after ${elapsed}s"
    fi
    if grep -q "guest_callbacks: PASS" "$OUT/stacksize.out"; then
        bad "WINEEMUNOSTACKSIZE=1 still printed PASS; ignoring dwStackSize did \
not reach anything this probe checks"
    fi
    # Part A must be UNAFFECTED: this lever is Part B's own, and the two
    # mechanisms being independent in the port is exactly what makes it
    # meaningful to gate them from one shared process.  Steps 1-14 are Part A;
    # step 15 is the first line of Part B.
    if sed -n '1,/^step 15 /p' "$OUT/stacksize.out" | grep -q FAIL; then
        sed -n '1,/^step 15 /p' "$OUT/stacksize.out" | grep FAIL | sed 's/^/  stacksize| /' >&2
        bad "WINEEMUNOSTACKSIZE=1 made a PART A step fail; this lever should \
touch only Part B, and a Part A regression under it means the two mechanisms \
are not as independent as this gate assumes"
    fi
    if grep -q "^step 17 .*reserve_bytes=2097152 FAIL" "$OUT/stacksize.out"; then
        say "WINEEMUNOSTACKSIZE=1: step 17 failed reporting exactly the 2 MiB \
image default, not some other wrong number"
    else
        grep "^step 17 " "$OUT/stacksize.out" | sed 's/^/  stacksize| /' >&2
        bad "WINEEMUNOSTACKSIZE=1 did not make step 17 fail by reporting the \
2 MiB image default; either the bound check did not go red at all, or it \
went red for a different reason than dwStackSize being ignored"
    fi
    if grep -q '(16777216 bytes, from this thread' "$OUT/stacksize.err"; then
        grep -m1 '(16777216 bytes' "$OUT/stacksize.err" | sed 's/^/  stacksize| /' >&2
        bad "WINEEMUNOSTACKSIZE=1 still logged a 16 MiB guest stack sized \
from the thread; the sabotage did not take effect"
    elif grep -q 'guest stack for .*from the image' "$OUT/stacksize.err"; then
        say "WINEEMUNOSTACKSIZE=1: the port's own module-channel trace says \
'from the image' -- the port acknowledges the sabotage, not just this \
probe's own bound check"
    else
        sed 's/^/  stacksize| /' "$OUT/stacksize.err" >&2
        bad "WINEEMUNOSTACKSIZE=1 died without the port's module trace naming \
'from the image' anywhere"
    fi
}

if [ "$SABOTAGE" = 1 ]; then
    sabotage_cbwrap
    sabotage_stacksize
    [ $fail -eq 0 ] && say "SABOTAGE PASS"
    exit $fail
fi

# ---- 2: guest --------------------------------------------------------
run_wine "$EXE" "$TIMEOUT" > "$OUT/guest.out" 2>"$OUT/guest.err"
gst=$?
if [ $gst -eq 124 ] || [ $gst -eq 137 ]; then
    bad "the guest run timed out after ${TIMEOUT}s"
elif grep -q "guest_callbacks: PASS" "$OUT/guest.out"; then
    say "guest: $(tail -1 "$OUT/guest.out")"
else
    sed 's/^/  guest| /' "$OUT/guest.out" >&2
    tail -30 "$OUT/guest.err" >&2
    bad "the x86-64 guest build did not pass"
fi

# ---- the transcript every correct implementation must print --------------
#
# Captured from an actual passing run against the landed fix (both parts A
# and B) on this exact toolchain and this exact prefix, not typed from the
# contract by hand -- unlike check-seh-handlers.sh's transcript, which its
# probe fully determines from constants of its own choosing, two lines here
# depend on facts THIS PROBE does not control: total= (step 14) depends on
# exactly which messages user32's CreateWindowExW privately sends around
# WM_NCCREATE/WM_CREATE for a message-only window on this port, and
# depth_bytes= (step 18) depends on this clang's exact per-frame call
# overhead at -O1 for gc_recurse.  Both were confirmed byte-for-byte
# reproducible across repeated runs on op4k before being pinned here; a
# change to either is worth noticing (a different message sequence, a
# different compiler), which is the entire reason this diffs byte for byte
# instead of only pattern-matching.
cat > "$OUT/expected.txt" <<'EOF'
guest_callbacks: start
step 1 RegisterClassExW registers a class naming a guest WNDPROC: atom_nonzero=yes ok
step 2 CreateWindowExW creates a window of class A: used_hwnd_message=yes created=yes ok
step 3 class A's guest WNDPROC ran for WM_NCCREATE then WM_CREATE: nccreate_calls=1 create_calls=1 trace='a-ncc a-create' ok
step 4 RegisterClassW (non-Ex) registers a second guest WNDPROC: atom_nonzero=yes ok
step 5 CreateWindowExW creates a window of class B: created=yes ok
step 6 class B's guest WNDPROC ran WM_NCCREATE/WM_CREATE through DefWindowProcW: nccreate_calls=1 create_calls=1 trace='b-ncc b-create' ok
step 7 SendMessageW: LRESULT with bit 31 of the low half SET survives: got=0x00c0ffeedeadbeef want=0x00c0ffeedeadbeef ok
step 8 SendMessageW: LRESULT with bit 31 of the low half CLEAR survives: got=0x00c0ffee12345678 want=0x00c0ffee12345678 ok
step 9 SetWindowLongPtrW(GWLP_WNDPROC) installs a second guest WNDPROC: prev_nonzero=yes ok
step 10 after the swap, SendMessageW reaches ONLY the new WNDPROC: got=0x00c0ffeecafef00d trace='c-ret3' ok
step 11 CallWindowProcW with the value SetWindowLongPtrW returned: prev_is_plain_pointer=yes prev_matches_winproc_handle_shape=no got=0x00c0ffeedeadbeef ok
step 12 CallWindowProcW with the RAW guest WNDPROC address: got=0x00c0ffeedeadbeef ok
step 13 GetWindowLongPtrW(GWLP_WNDPROC) readback is usable: cur_is_plain_pointer=yes cur_matches_winproc_handle_shape=no readback_equals_raw_guest_pointer=no callwindowproc_got=0x00c0ffeecafef00d ok
step 14 total guest WNDPROC dispatches witnessed in this process: total=16 ok
step 15 CreateThread(dwStackSize=16MiB, STACK_SIZE_PARAM_IS_A_RESERVATION): created=yes ok
step 16 the big-stack thread ran and reported back: joined=yes started=yes ok
step 17 the big-stack thread's OWN TEB describes a >=16MiB guest stack: reserve_mib=16 reserve_bytes=16777216 ok
step 18 usable depth: the big-stack thread actually descended >12MiB: depth_mib=12 depth_bytes=12585232 ok
step 19 CreateThread(dwStackSize=0) is the control: created=yes ok
step 20 the control thread ran and reported back: joined=yes started=yes ok
step 21 the control thread's guest stack is the image default, not 16MiB: reserve_mib=2 reserve_smaller_than_16mib=yes ok
guest_callbacks: PASS 21/21
EOF

if cmp -s "$OUT/expected.txt" "$OUT/guest.out"; then
    say "transcript: the guest printed exactly the expected $(wc -l < "$OUT/expected.txt") lines"
else
    diff "$OUT/expected.txt" "$OUT/guest.out" | sed 's/^/  /' >&2
    bad "the guest transcript is not the expected one (see the note above \
this block about which numbers are environment-dependent best-effort \
guesses pending a real passing run)"
fi

# ---- 4: the port's own view, part A ---------------------------------
# +seh,trace+module in ONE extra run rather than two: this run supplies both
# this layer's cross-checks and layer 5's below, so the WNDPROC and
# stack-size mechanisms are corroborated from a single execution's trace
# instead of doubling how many times this probe has to be run to gate it.
timeout -k 5 "$TIMEOUT" env WINEDEBUG=+seh,trace+module WINEDLLOVERRIDES="winedbg.exe=d" \
    "$BUILD/wine" "$EXE" >/dev/null 2>"$OUT/trace.log"

WRAPPED=$(grep -c -- '-> trampoline ' "$OUT/trace.log")
if [ "${WRAPPED:-0}" -eq 3 ]; then
    say "port: wrap_guest_callback_ex() logged $WRAPPED distinct trampolines -- \
exactly the three guest WNDPROCs this probe registers (gc_wndproc_a, \
gc_wndproc_b, gc_wndproc_c); re-use of an already-wrapped pointer must \
not grow this count, and it did not"
else
    grep -- '-> trampoline ' "$OUT/trace.log" | sed 's/^/  trace| /' >&2
    bad "wrap_guest_callback_ex() logged ${WRAPPED:-0} distinct trampolines, not \
3; either a WNDPROC never got wrapped at registration (the defect this \
gate exists to catch) or something wrapped more targets than this probe \
registers"
fi
WIDE=$(grep -c -- '-> trampoline .*64-bit return' "$OUT/trace.log")
if [ "${WIDE:-0}" -eq 3 ]; then
    say "port: all $WIDE of those trampolines were tagged 64-bit return -- a \
WNDPROC gets the WIDE slot, not the sign-extending one every other \
callback class uses"
else
    bad "only ${WIDE:-0} of the 3 trampolines were tagged 64-bit return; a \
WNDPROC trampoline that truncates its LRESULT to a sign-extended 32 bits \
would still pass this count check while failing steps 7-8 for the wrong \
reason if this sub-check were missing"
fi

# Each of these four is fully controlled by this probe -- one call, one
# trace line, an exact constant -- unlike the dispatch count below, which
# depends on how many messages user32 privately sends.
check_exact_count() {   # $1 = human name, $2 = grep pattern, $3 = expected count
    n=$(grep -c -- "$2" "$OUT/trace.log")
    if [ "${n:-0}" -eq "$3" ]; then
        say "port: $1 logged $n time(s), exactly what this probe called"
    else
        grep -- "$2" "$OUT/trace.log" | sed 's/^/  trace| /' >&2
        bad "$1 logged ${n:-0} time(s), not $3 -- this probe calls the API \
$1 traces exactly $3 time(s)"
    fi
}
check_exact_count "emu_RegisterClassEx"  "emu_RegisterClassEx RegisterClassEx(" 1
check_exact_count "emu_RegisterClass (non-Ex)" "emu_RegisterClass RegisterClass(" 1
check_exact_count "emu_SetWindowLongPtr" "emu_SetWindowLongPtr SetWindowLongPtr(" 1
check_exact_count "emu_CallWindowProc"   "emu_CallWindowProc CallWindowProc(" 3

DISPATCHED=$(grep -c "calling guest callback " "$OUT/trace.log")
PROBE_TOTAL=$(sed -n 's/.*total guest WNDPROC dispatches witnessed in this process: total=\([0-9]*\).*/\1/p' \
    "$OUT/guest.out" | head -1)
if [ -n "$PROBE_TOTAL" ] && [ "${DISPATCHED:-0}" = "$PROBE_TOTAL" ]; then
    say "port: guest_callback_run() logged $DISPATCHED calls, exactly \
what the probe's own witness counted (total=$PROBE_TOTAL); two \
independent instruments, one event, one answer"
else
    bad "guest_callback_run() logged ${DISPATCHED:-0} calls but the \
probe's own witness counted ${PROBE_TOTAL:-<none printed>}; the port's \
trace and the probe's in-process counter disagree about how many times \
a wrapped WNDPROC actually ran"
fi

# The failure this gate exists to catch is SILENT by construction --
# dispatch_user_callback's __EXCEPT_ALL swallows a guest fault and logs
# nothing but this ERR -- so a PASSING run must show none of it.
if grep -qE 'err:.*(ignoring exception|c000001d)' "$OUT/guest.err"; then
    grep -Em3 'err:.*(ignoring exception|c000001d)' "$OUT/guest.err" | sed 's/^/  guest| /' >&2
    bad "the passing run's own diagnostics contain an 'ignoring exception' or \
c000001d line -- something was silently swallowed even though the \
probe reported PASS, which is precisely the failure mode DOOM (2016) hit"
else
    say "port: no err-level diagnostic names 'ignoring exception' or \
c000001d -- nothing was silently swallowed during a passing run"
fi

# ---- 5: the port's own view, part B ----------------------------------
# The ONLY 16 MiB request anywhere in this probe is the big-stack thread's
# own; the control thread and every nested callback run all ask for the
# 2 MiB image default (see guest_callbacks.c's header comment on why that
# default is 2 MiB and not lld's arbitrary 1 MiB).  So a single exact count
# is a clean, low-noise cross-check: this probe's own witness (step 17) and
# the port's module trace, required to agree that exactly one 16 MiB guest
# stack, sized from a thread rather than the image, was ever allocated.
BIGSTACK=$(grep -c -- '(16777216 bytes, from this thread' "$OUT/trace.log")
if [ "${BIGSTACK:-0}" -eq 1 ]; then
    say "port: emu_run_loop() logged exactly one 16 MiB guest stack sized \
from a thread's own stack -- the big-stack thread's, and only its"
else
    grep -- '16777216 bytes' "$OUT/trace.log" | sed 's/^/  trace| /' >&2
    bad "emu_run_loop() logged ${BIGSTACK:-0} guest stacks of 16 MiB sized \
from a thread's own stack, not exactly 1; either dwStackSize never reached \
the guest stack allocator (this probe's own step 17 already checked the \
TEB side of that), or something ELSE in this process is also asking for \
16 MiB"
fi

sabotage_cbwrap
sabotage_stacksize
[ $fail -eq 0 ] && say "PASS"
exit $fail
