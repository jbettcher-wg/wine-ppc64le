#!/bin/sh
#
# check-shell-smoke.sh -- the everyday-DLL guest surface gate.
#
# The claim: an x86-64 Windows program run as a GUEST under this port opens
# DirectInput 8 through a COM interface whose vtable is on the wrong machine,
# creates a keyboard device, sets its data format and cooperative level,
# acquires it and POLLS IT -- and gets byte-identical results to the same
# source built for this machine's OWN architecture and run through the same
# Wine.  Plus the three flat surfaces this pass filled: d3dx9's floating-point
# math, hid's class GUID, and comctl32's control-class registration.
#
# Byte-identical is the bar rather than "the guest said PASS" for the reason
# ppc64le/syscom/check-com-smoke.sh gives: reaching the right answer through
# the wrong mechanism is exactly the failure a PASS/PASS comparison cannot
# see.  Every byte compared here is a value Wine computed or a constant the
# probe checked against arithmetic it did itself.
#
# Nine legs:
#
#   A  SURFACE CURRENT: ppc64le/shell/gen_dinput_surface.py --check.  The
#      roster and the marshal tables are GENERATED from include/dinput.h; if
#      either has drifted, everything below is testing a stale vtable map.
#   B  BUILD: the guest thunk modules exist and export what this pass claims.
#      Counted from the PE export table, not from a build log -- and the
#      REFUSALS are checked too: a name this port excluded on purpose must be
#      ABSENT, because an export that came back silently is the failure this
#      whole exercise is about.
#   C  DISPLAY: an Xvfb of this gate's own, on a display number nobody is
#      using.  user32 needs an X server for RegisterClassEx/CreateWindowEx;
#      the person at this machine is on the real ones and this gate never
#      touches them.
#   D  NATIVE: the probe built as a ppc64 PE (winegcc), run under this wine
#      with no guest anywhere in the process, reports PASS.
#   E  GUEST: the same source built as an x86-64 PE, run as a guest under the
#      same wine, reports PASS.
#   F  IDENTITY: cmp(native stdout, guest stdout) is empty.
#   G  MECHANISM: the +winecom,+guestcb trace of the guest run shows the guest
#      vtables being MATERIALISED, the returned IDirectInput8A and the device
#      being WRAPPED as proxies, the individual methods arriving in
#      winecom_dispatch BY NAME, and -- the half this pass added -- each guest
#      COMPARATOR and each PROPERTY SHEET PROCEDURE being swapped for a
#      trampoline BY NAME as it crosses.  A later pass added SetWindowSubclass/
#      RemoveWindowSubclass's SUBCLASSPROC to this same trace requirement: it
#      is a SIX-argument callback, the first shape this port's callback
#      trampolines could not originally carry in full, and the probe checks
#      that uIdSubclass and dwRefData -- arguments five and six, passed on the
#      Microsoft-x64 stack -- arrive as the full 64-bit patterns it sent and
#      not truncated or swapped.  Without this a guest that somehow reached
#      the right answers natively would still pass D-F.
#   H  REFUSAL IS THE BOUNDARY'S, NOT THE API'S: a second pair of builds
#      (-DSHELL_SMOKE_ENUM) calls IDirectInput8A::EnumDevices, which hands
#      native dinput a bare GUEST FUNCTION POINTER.  Both runs must now agree
#      -- same HRESULT, same device count, callback entered with a well-formed
#      DIDEVICEINSTANCEA -- and the guest run's trace must show ntdll's
#      trampoline pool minting a stub for that callback.  This leg used to
#      require the opposite (E_NOTIMPL on the guest); dlls/dinput8/guestcom.c
#      serves it now.  The callback VALUE-CHECKS its DIDEVICEINSTANCEA and
#      folds every device's guids, type and name into a digest the two runs
#      must agree on, so "the callback ran N times" becomes "the callback saw
#      the same N devices, field for field".
#   I  A HOOK INSIDE A STRUCT: a third pair of builds (-DSHELL_SMOKE_HOOK)
#      calls GetOpenFileNameA with OFN_ENABLEHOOK, which is the callback shape
#      no argument-position mask can name -- it is a field in the caller's
#      OPENFILENAME.  The hook must be ENTERED, must be handed THE CALLER'S OWN
#      struct (it reads the probe's cookie back out of lCustData), and cancels
#      the dialog itself so that a machine with nobody at it can run this.  The
#      two runs must agree byte for byte, and the guest run's +guestcb trace
#      must show the hook being swapped.  This used to be a refusal by name.
#
# --sabotage runs the negative controls instead, and every one must go red.
# A gate that cannot go red proves nothing:
#
#   1  WINEEMUNOCOMWRAP=1 makes winecom_wrap hand the guest the RAW native
#      pointer -- the exact defect the runtime exists to fix -- so the guest's
#      first vtable call executes ppc64 bytes as x86-64.  The guest run must
#      not PASS.
#   2  WINEEMUNOCBWRAP=1 makes the trampoline pool hand native code the RAW
#      GUEST pointer, which is the defect THIS pass's machinery exists to fix:
#      native comctl32 then calls a guest comparator and a guest dialog
#      procedure directly.  The guest run must not PASS.  Without this control
#      every callback leg below could be passing by accident.
#   3  each SHELL_SMOKE_BREAK=1..5 build of the NATIVE leg must FAIL: a
#      wrong-sized data format dinput must reject, a HID GUID off by one byte,
#      a matrix element off by one, a DPA_Sort order off by one, a
#      property-sheet callback mask off by one.  These prove the VALUE CHECKS
#      are checks, on the leg where no boundary is involved at all.
#
# Exit 0 = pass, 1 = a check failed, 2 = could not run at all (a skip is NOT
# a pass).
#
# WHY EVERY WINE RUN DISABLES winedbg, verbatim from check-gl-smoke.sh because
# the hazard is identical: the bringup prefix has AeDebug configured with
# "winedbg --auto", so a run that ends in an unhandled fault -- which is what
# a defect here looks like from outside -- starts a debugger that attaches and
# never lets go, turning every red state of this gate into a hang.
# WINEDLLOVERRIDES=winedbg.exe=d makes start_debugger's CreateProcess fail, so
# the process simply terminates.
set -u

HERE=$(cd "$(dirname "$0")" && pwd)
SRC=$(cd "$HERE/../.." && pwd)
BUILD=${BUILD:-$SRC}                    # in-tree build by default
OUT=${OUT:-/tmp/shell-smoke}
SABOTAGE=0
[ "${1:-}" = "--sabotage" ] && SABOTAGE=1

say()  { echo "check-shell-smoke: $*"; }
bad()  { echo "check-shell-smoke: FAIL $*" >&2; fail=1; }
note() { echo "check-shell-smoke: note $*"; }
skip() { echo "check-shell-smoke: $*" >&2; cleanup; exit 2; }

XVFB_PID=
XVFB_DISPLAY=
cleanup() {
    [ -n "$XVFB_PID" ] && kill "$XVFB_PID" 2>/dev/null
    XVFB_PID=
}
trap 'cleanup' EXIT INT TERM

[ -x "$BUILD/wine" ] || skip "no wine loader at $BUILD/wine"
[ -n "${WINEPREFIX:-}" ] || skip "set WINEPREFIX to a prefix wineboot has run in"
[ -n "${WINEFEXBRIDGE:-}" ] || skip "set WINEFEXBRIDGE to the emulator bridge"
command -v clang >/dev/null || skip "need clang for the guest build"
command -v llvm-dlltool >/dev/null || skip "need llvm-dlltool for the guest build"
command -v llvm-readobj >/dev/null || skip "need llvm-readobj to read a PE export table"
command -v "${CC:-gcc}" >/dev/null || skip "need ${CC:-gcc} for the native ppc64 build"
command -v Xvfb >/dev/null || skip "need Xvfb: user32 needs an X server and this \
gate must not take the one the user is on"
command -v python3 >/dev/null || skip "need python3 for the surface check"

mkdir -p "$OUT" || skip "cannot create $OUT"
fail=0
TIMEOUT=${TIMEOUT:-180}
INCL="-I$BUILD/include -I$SRC/include -I$SRC/include/msvcrt"
GUESTDIR="$BUILD/dlls"

# ---- A: the generated surface is current -----------------------------------
if python3 "$HERE/gen_dinput_surface.py" --check > "$OUT/surface.out" 2>&1; then
    say "surface: $(tr '\n' ';' < "$OUT/surface.out" | sed 's/;$//')"
else
    sed 's/^/  surface| /' "$OUT/surface.out" >&2
    bad "the dinput8 roster or marshal tables have drifted from include/dinput.h \
-- run ppc64le/shell/gen_dinput_surface.py --json ppc64le/shell/interfaces_dinput.json \
--marshal dlls/dinput8/dinput8_marshal.h and rebuild"
fi

# ---- B: the guest thunk modules, and what they do and do not export --------
# Read straight out of each PE's export table.  The counts are floors, not
# equalities: a later pass that serves MORE must not have to edit this gate,
# but one that quietly serves less has to answer for it.
exports_of() {   # exports_of <dll path> -> one name per line
    llvm-readobj --coff-exports "$1" 2>/dev/null |
        sed -n 's/^ *Name: \(.*\)$/\1/p'
}

check_module() {   # check_module <module> <floor> [must-be-absent...]
    mod=$1; floor=$2; shift 2
    dll="$GUESTDIR/$mod/x86_64-windows/$mod.dll"
    if [ ! -f "$dll" ]; then
        bad "no guest thunk at $dll -- build it first"
        return
    fi
    exports_of "$dll" > "$OUT/$mod.exports"
    n=$(grep -c . < "$OUT/$mod.exports")
    if [ "$n" -lt "$floor" ]; then
        bad "guest $mod.dll exports $n name(s), fewer than the $floor this \
pass measured"
        return
    fi
    for absent in "$@"; do
        if grep -qx "$absent" "$OUT/$mod.exports"; then
            bad "guest $mod.dll exports $absent, which this port refuses BY \
NAME -- a silent pass-through is exactly the defect the refusal exists to \
prevent"
            return
        fi
    done
    say "surface: $mod.dll exports $n name(s)$([ $# -gt 0 ] && \
        echo ", and none of the $# refused by name")"
}

# The floors are the export-table counts measured on 2026-08-17, which are the
# emitted surface plus __wine_thunk_info (plus __wine_com_thunk_info for a COM
# module).  They are floors and not equalities so that a later pass serving
# MORE does not have to edit this gate; one that quietly serves less has to
# answer for it.  The absent lists are this pass's own refusals, by name.
#
# AN ABSENT NAME IS AN `EXCLUDE` LINE IN THE .thunks FILE, AND NOTHING ELSE.
# `GUEST-IMPL X __wine_guest_X` does not remove X -- it emits X and points its
# native resolution at a wrapper, which is the port SERVING it.  Six comctl32
# names were on this list as refusals while comctl32.thunks had already grown
# GUEST-IMPL lines for them (PropertySheetA/W, CreatePropertySheetPageA/W,
# DPA_Sort, DPA_Search), and the gate stayed green because the guest module on
# disk was STALE: nothing in the build graph rebuilds a guest thunk module when
# its .thunks changes, so the gate had been reading an artifact older than the
# directives it was checking.  Both halves of that are worth remembering -- the
# list is derived from EXCLUDE, and a green gate over a stale artifact is not a
# green gate.
check_module dinput8  5
check_module hid      38
check_module d3dx9_42 264 D3DXSHEvalHemisphereLight D3DXComputeTangentFrameEx
check_module comctl32 90  HIMAGELIST_QueryInterface ImageList_Read ImageList_Write
check_module comdlg32 22  DllGetClassObject
check_module wininet  161
check_module wintrust 45
check_module usp10    36  ScriptPlaceOpenType
check_module d2d1     9   D2D1MakeRotateMatrix D2D1MakeSkewMatrix
check_module dwrite   2
check_module dinput   5   DllGetClassObject

# The COM half of the dinput8 module is not in the flat export count above: it
# is the per-interface stub arrays and the __wine_com_thunk_info table
# spec2thunk COM mode appends.  A module built WITHOUT COM-JSON would pass the
# count above unchanged, so the table is required by name -- that one export is
# what ntdll's COM trap dispatcher finds the vtables through.
if grep -qx "__wine_com_thunk_info" "$OUT/dinput8.exports"; then
    say "surface: guest dinput8.dll publishes __wine_com_thunk_info, so the \
102-slot COM stub block is really in it"
else
    bad "guest dinput8.dll has no __wine_com_thunk_info -- it was built without \
COM-JSON, so there are no guest vtables for ntdll to dispatch through"
fi

# ---- C: a display of this gate's own ---------------------------------------
for n in 91 92 93 94 95 96 97 98 99; do
    [ -e "/tmp/.X11-unix/X$n" ] && continue
    XVFB_DISPLAY=":$n"
    break
done
[ -n "$XVFB_DISPLAY" ] || skip "no free display number in :91..:99"
Xvfb "$XVFB_DISPLAY" -screen 0 320x240x24 > "$OUT/xvfb.log" 2>&1 &
XVFB_PID=$!
n=0
while [ ! -e "/tmp/.X11-unix/X${XVFB_DISPLAY#:}" ]; do
    n=$((n + 1))
    [ "$n" -gt 100 ] && break
    sleep 0.1
done
if [ ! -e "/tmp/.X11-unix/X${XVFB_DISPLAY#:}" ]; then
    sed 's/^/  xvfb| /' "$OUT/xvfb.log" >&2
    skip "Xvfb did not come up on $XVFB_DISPLAY"
fi
say "display: Xvfb on $XVFB_DISPLAY (pid $XVFB_PID); the user's :0 and :1 are \
not touched"

WDBG=${WINEDEBUG:--all}
run_wine() {   # run_wine <exe> [extra env assignments...]
    exe=$1; shift
    timeout -k 5 "$TIMEOUT" env DISPLAY="$XVFB_DISPLAY" WINEDEBUG="$WDBG" \
        WINEDLLOVERRIDES="winedbg.exe=d" "$@" "$BUILD/wine" "$exe"
}

# ---- build: the native ppc64 PE leg ----------------------------------------
# An ordinary consumer of the public headers, linked against this tree's own
# ppc64-windows import libraries and turned into a builtin PE -- the same
# recipe check-gl-smoke.sh and check-com-smoke.sh use, minus -D__WINESRC__.
native_build() {   # native_build <output> [extra cflags...]
    nout=$1; shift
    ${CC:-gcc} -c -o "$OUT/native.o" "$HERE/probes/shell_smoke.c" $INCL "$@" \
        -D_UCRT -D_WIN32 -Wall -pipe -mlongcall -mno-pltseq -fcf-protection=none \
        -fvisibility=hidden -fno-stack-protector -fno-strict-aliasing -gdwarf-4 \
        -fPIC -fasynchronous-unwind-tables -mlong-double-64 -fno-builtin \
        -fshort-wchar -Wno-format -g -O1 2>"$OUT/native.build.err" || return 1
    "$BUILD/tools/winegcc/winegcc" -o "$nout" --wine-objdir "$BUILD" \
        --cc-cmd="${CC:-gcc}" -mno-cygwin -fPIC -fasynchronous-unwind-tables \
        -Wl,--wine-builtin -mconsole "$OUT/native.o" \
        "$BUILD/dlls/dinput8/ppc64-windows/libdinput8.a" \
        "$BUILD/dlls/d3dx9_42/ppc64-windows/libd3dx9_42.a" \
        "$BUILD/dlls/hid/ppc64-windows/libhid.a" \
        "$BUILD/dlls/comctl32/ppc64-windows/libcomctl32.a" \
        "$BUILD/dlls/comdlg32/ppc64-windows/libcomdlg32.a" \
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
# The imports are described by hand rather than taken from a mingw sysroot:
# the point of naming the DLL for each symbol is that the guest binds to the
# same builtins a real application would, and nothing else is linked in at all
# (there is no CRT here -- see the probe's banner).  Note what is NOT in these
# lists: every DirectInput call after the first goes through a COM vtable, not
# an import.
cat > "$OUT/kernel32.def" <<'EOF'
LIBRARY kernel32.dll
EXPORTS
GetStdHandle
WriteFile
ExitProcess
GetModuleHandleA
GetCurrentThreadId
GetTickCount
EOF
cat > "$OUT/user32.def" <<'EOF'
LIBRARY user32.dll
EXPORTS
RegisterClassExA
CreateWindowExA
DefWindowProcA
DestroyWindow
GetClassInfoExA
GetParent
PostMessageA
SendMessageA
EOF
cat > "$OUT/dinput8.def" <<'EOF'
LIBRARY dinput8.dll
EXPORTS
DirectInput8Create
EOF
cat > "$OUT/d3dx9_42.def" <<'EOF'
LIBRARY d3dx9_42.dll
EXPORTS
D3DXMatrixMultiply
D3DXVec3Normalize
EOF
cat > "$OUT/hid.def" <<'EOF'
LIBRARY hid.dll
EXPORTS
HidD_GetHidGuid
EOF
cat > "$OUT/comctl32.def" <<'EOF'
LIBRARY comctl32.dll
EXPORTS
InitCommonControlsEx
DPA_Create
DPA_InsertPtr
DPA_GetPtr
DPA_Sort
DPA_Search
DPA_EnumCallback
DPA_DestroyCallback
DSA_Create
DSA_InsertItem
DSA_DestroyCallback
CreatePropertySheetPageA
DestroyPropertySheetPage
PropertySheetA
SetWindowSubclass
RemoveWindowSubclass
DefSubclassProc
EOF
# Only the -DSHELL_SMOKE_HOOK build imports these, but the import library is
# built unconditionally: an unreferenced one contributes nothing to a PE.
cat > "$OUT/comdlg32.def" <<'EOF'
LIBRARY comdlg32.dll
EXPORTS
GetOpenFileNameA
CommDlgExtendedError
EOF
for m in kernel32 user32 dinput8 d3dx9_42 hid comctl32 comdlg32; do
    llvm-dlltool -m i386:x86-64 -d "$OUT/$m.def" -l "$OUT/lib$m.a" \
        || skip "llvm-dlltool failed for $m"
done

GUESTCC="clang -target x86_64-windows-gnu -nostdlibinc $INCL -DSHELL_SMOKE_NO_CRT \
-D_UCRT -Wall -O1 -fno-builtin -g"
GUESTLD="clang -target x86_64-windows-gnu -fuse-ld=lld -nostdlib \
-Wl,--entry=shell_smoke_entry -Wl,--subsystem,console"

guest_build() {   # guest_build <output> [extra cflags...]
    gout=$1; shift
    $GUESTCC "$@" -c -o "$OUT/guest.o" "$HERE/probes/shell_smoke.c" \
        2>"$OUT/guest.build.err" || return 1
    $GUESTLD -o "$gout" "$OUT/guest.o" "$OUT/libdinput8.a" "$OUT/libd3dx9_42.a" \
        "$OUT/libhid.a" "$OUT/libcomctl32.a" "$OUT/libcomdlg32.a" \
        "$OUT/libuser32.a" "$OUT/libkernel32.a" \
        2>>"$OUT/guest.build.err" || return 1
    return 0
}

native_build "$OUT/shell_smoke.exe" || {
    sed 's/^/  native| /' "$OUT/native.build.err" >&2
    skip "the native ppc64 build failed"
}
guest_build "$OUT/shell_smoke_guest.exe" || {
    sed 's/^/  guest| /' "$OUT/guest.build.err" >&2
    skip "the x86-64 guest build failed"
}
# Zero-warning discipline: a warning from either build is a defect in the
# probe, and a probe with a defect is not evidence.
for w in "$OUT/native.build.err" "$OUT/guest.build.err"; do
    if grep -q 'warning:' "$w"; then
        sed 's/^/  warn| /' "$w" >&2
        bad "the probe built with warnings"
    fi
done

# ---- sabotage --------------------------------------------------------------
if [ "$SABOTAGE" = 1 ]; then
    # 1: raw interface pointers.
    run_wine "$OUT/shell_smoke_guest.exe" WINEEMUNOCOMWRAP=1 \
        > "$OUT/sab1.out" 2>"$OUT/sab1.err"
    if grep -q "shell_smoke: PASS" "$OUT/sab1.out"; then
        bad "WINEEMUNOCOMWRAP=1 still PASSED -- the COM leg cannot go red"
    else
        say "sabotage(nocomwrap): the guest run stopped at '$(tail -1 \
            "$OUT/sab1.out" | cut -c1-70)', as it must"
    fi
    # 2: raw guest callbacks.  Everything this pass added goes through the
    #    trampoline pool, so a run with the pool disabled must not reach PASS.
    run_wine "$OUT/shell_smoke_guest.exe" WINEEMUNOCBWRAP=1 \
        > "$OUT/sab2.out" 2>"$OUT/sab2.err"
    if grep -q "shell_smoke: PASS" "$OUT/sab2.out"; then
        bad "WINEEMUNOCBWRAP=1 still PASSED -- the guest-callback legs cannot \
go red, so none of them is testing the trampoline pool"
    else
        say "sabotage(nocbwrap): the guest run stopped at '$(tail -1 \
            "$OUT/sab2.out" | cut -c1-70)', as it must"
    fi
    # 3: the value checks themselves, on the native leg where no boundary is
    #    involved at all -- so a red here is the CHECK going red and nothing
    #    else.
    for n in 1 2 3 4 5; do
        if ! native_build "$OUT/break$n.exe" "-DSHELL_SMOKE_BREAK=$n"; then
            sed 's/^/  break| /' "$OUT/native.build.err" >&2
            bad "SHELL_SMOKE_BREAK=$n did not build"
            continue
        fi
        run_wine "$OUT/break$n.exe" > "$OUT/break$n.out" 2>&1
        if grep -q "shell_smoke: PASS" "$OUT/break$n.out"; then
            bad "SHELL_SMOKE_BREAK=$n PASSED -- that value check cannot go red"
        else
            say "sabotage(break$n): $(grep -m1 FAIL "$OUT/break$n.out" | \
                cut -c1-78)"
        fi
    done
    [ $fail -eq 0 ] && say "SABOTAGE PASS (every control red)"
    cleanup
    exit $fail
fi

# ---- D: native -------------------------------------------------------------
run_wine "$OUT/shell_smoke.exe" > "$OUT/native.out" 2>"$OUT/native.err"
if grep -q "shell_smoke: PASS" "$OUT/native.out"; then
    say "native: $(tail -1 "$OUT/native.out")"
else
    sed 's/^/  native| /' "$OUT/native.out" >&2
    tail -20 "$OUT/native.err" >&2
    bad "the native ppc64 build did not pass"
fi

# ---- E: guest --------------------------------------------------------------
run_wine "$OUT/shell_smoke_guest.exe" > "$OUT/guest.out" 2>"$OUT/guest.err"
if grep -q "shell_smoke: PASS" "$OUT/guest.out"; then
    say "guest:  $(tail -1 "$OUT/guest.out")"
else
    sed 's/^/  guest| /' "$OUT/guest.out" >&2
    tail -30 "$OUT/guest.err" >&2
    bad "the x86-64 guest build did not pass"
fi

# ---- F: identity -----------------------------------------------------------
if cmp -s "$OUT/native.out" "$OUT/guest.out"; then
    say "identity: native and guest output is byte-identical ($(grep -c . \
        < "$OUT/native.out") lines)"
else
    diff "$OUT/native.out" "$OUT/guest.out" >&2
    bad "native and guest output differ"
fi

# ---- G: mechanism ----------------------------------------------------------
# The guest could in principle print the right bytes while calling a native
# vtable, or while native comctl32 called its raw x86-64 comparator and got
# away with it -- those ARE the defects -- so require the runtime's own trace
# to show both mechanisms being taken, by name.
#
# +guestcb is the channel dlls/comctl32/guestthunk.c and
# dlls/comdlg32/guestthunk.c declare, and it prints exactly one line per swap:
# what was wrapped, the guest pointer, the trampoline.  A channel of their own
# rather than +seh so that this can be required precisely and cheaply -- the
# pool's own trace in ntdll would bury it.
run_wine "$OUT/shell_smoke_guest.exe" WINEDEBUG=+winecom,+guestcb \
    > "$OUT/guest.trace.out" 2>"$OUT/guest.trace.err"
cmp -s "$OUT/native.out" "$OUT/guest.trace.out" || \
    bad "the traced guest run did not reproduce the untraced one"
for want in "materialised .* guest vtable slots" \
            "wrapped IDirectInput8A host .* as proxy" \
            "wrapped IDirectInputDevice8A host .* as proxy" \
            "winecom_dispatch IDirectInput8A::CreateDevice" \
            "winecom_dispatch IDirectInputDeviceA::SetDataFormat" \
            "winecom_dispatch IDirectInputDeviceA::SetCooperativeLevel" \
            "winecom_dispatch IDirectInputDeviceA::Acquire" \
            "winecom_dispatch IDirectInputDeviceA::GetDeviceState" \
            "winecom_dispatch IDirectInputDevice2A::Poll" \
            "destroying proxy" \
            "DPA_Sort comparator .* -> trampoline" \
            "DPA_Search comparator .* -> trampoline" \
            "DPA_EnumCallback callback .* -> trampoline" \
            "DPA_DestroyCallback callback .* -> trampoline" \
            "DSA_DestroyCallback callback .* -> trampoline" \
            "property sheet pfnDlgProc .* -> trampoline" \
            "property sheet pfnCallback .* -> trampoline" \
            "property sheet header pfnCallback .* -> trampoline" \
            "SetWindowSubclass SUBCLASSPROC .* -> six-argument trampoline" \
            "RemoveWindowSubclass SUBCLASSPROC .* -> six-argument trampoline"; do
    if ! grep -qE "$want" "$OUT/guest.trace.err"; then
        bad "no '$want' in the +winecom,+guestcb trace of the guest run"
    fi
done
[ $fail -eq 0 ] && say "mechanism: guest vtables materialised, IDirectInput8A \
and its device wrapped, every method dispatched by name, and every comparator, \
property-sheet procedure and the six-argument SUBCLASSPROC swapped for a \
trampoline as it crossed"

# ---- H: the enumeration crosses, and the two runs agree --------------------
# EnumDevices hands native dinput a bare GUEST FUNCTION POINTER that dinput
# retains and calls once per device from a native frame.  It used to be refused
# by name; dlls/dinput8/guestcom.c now swaps the guest pointer for one of
# ntdll's guest-callback trampolines at the moment it arrives, so both runs
# must now produce the SAME answer -- the same HRESULT, the same device count,
# and a callback that actually saw a well-formed DIDEVICEINSTANCEA.
#
# Four claims, and each removes a different way of passing:
#   the HRESULTs agree            -- the call was served, not refused
#   the device COUNTS agree       -- the callback was entered, once per device,
#                                    with the caller's own pvRef intact
#   the DIGESTS agree             -- and it saw the same devices FIELD FOR
#                                    FIELD: dwSize, both guids, the device type
#                                    and the instance name of every one of them,
#                                    folded into one word.  A count alone cannot
#                                    tell "saw N devices" from "saw N of
#                                    something"
#   the trace shows a trampoline  -- it was entered THROUGH the pool rather
#                                    than by native code calling x86-64 bytes,
#                                    which is the defect this exists to prevent
native_build "$OUT/enum.exe" -DSHELL_SMOKE_ENUM || {
    sed 's/^/  enum-native| /' "$OUT/native.build.err" >&2
    skip "the -DSHELL_SMOKE_ENUM native build failed"
}
guest_build "$OUT/enum_guest.exe" -DSHELL_SMOKE_ENUM || {
    sed 's/^/  enum-guest| /' "$OUT/guest.build.err" >&2
    skip "the -DSHELL_SMOKE_ENUM guest build failed"
}
run_wine "$OUT/enum.exe" > "$OUT/enum.native.out" 2>"$OUT/enum.native.err"
# +seh as well as +winecom: the trampoline pool lives in
# dlls/ntdll/signal_ppc64.c, whose channel is seh, and its one-line trace is
# the only place the swap is visible from outside.
run_wine "$OUT/enum_guest.exe" WINEDEBUG=+winecom,+seh \
    > "$OUT/enum.guest.out" 2>"$OUT/enum.guest.err"

enum_line() { grep -m1 "EnumDevices" "$1" | sed 's/.*hr=/hr=/'; }
g_hr=$(enum_line "$OUT/enum.guest.out")
n_hr=$(enum_line "$OUT/enum.native.out")
if [ -z "$g_hr" ] || [ -z "$n_hr" ]; then
    sed 's/^/  enum-guest| /' "$OUT/enum.guest.out" >&2
    sed 's/^/  enum-native| /' "$OUT/enum.native.out" >&2
    bad "one of the EnumDevices runs printed nothing for the leg to compare"
elif [ "$g_hr" != "$n_hr" ]; then
    bad "the guest's EnumDevices answered '$g_hr' and the native one '$n_hr'; \
this port used to refuse it and now serves it, so they must agree"
elif ! echo "$g_hr" | grep -q "^hr=0x00000000"; then
    bad "both runs agreed on '$g_hr', which is not S_OK -- the two legs agreeing \
on a failure is not the same as the enumeration working"
elif ! echo "$g_hr" | grep -qE "devices=[1-9][0-9]*,"; then
    bad "EnumDevices returned S_OK but the callback was never entered ('$g_hr'); \
a served call that never calls back is exactly what a swallowed trampoline \
looks like"
elif ! echo "$g_hr" | grep -q "cb_arg_ok=1"; then
    bad "the enumeration callback was entered but its DIDEVICEINSTANCEA did not \
arrive well-formed ('$g_hr')"
elif ! echo "$g_hr" | grep -q "cb_call_ok=1"; then
    bad "the enumeration callback was entered but could not CALL BACK into the \
API from inside itself ('$g_hr') -- which is what every real callback does"
elif ! grep -qE "guest callback .* -> trampoline" "$OUT/enum.guest.err"; then
    bad "the guest's EnumDevices worked but no trampoline was minted for its \
callback -- native dinput must have been handed the raw guest pointer"
elif grep -q "refusing IDirectInput8A::EnumDevices" "$OUT/enum.guest.err"; then
    bad "winecom still refuses IDirectInput8A::EnumDevices; the hand slot is \
not being reached"
else
    say "enumeration: guest $g_hr, native $n_hr, and the callback was entered \
through $(grep -m1 -oE 'guest callback .* -> trampoline [0-9A-F]*' \
    "$OUT/enum.guest.err")"
fi

# ---- I: a hook inside a struct crosses, and the two runs agree -------------
# GetOpenFileNameA with OFN_ENABLEHOOK is the callback shape no
# argument-position mask can name: the procedure is a FIELD in the caller's
# OPENFILENAME, so nothing in ntdll's override table can see it and
# dlls/comdlg32/guestthunk.c has to swap it itself.  This used to be refused by
# name with CDERR_INITIALIZATION.
#
# ITS OWN BUILD, and its own run, for the same reason the enumeration has one:
# it puts a real file dialog up.  Nobody is at this machine, so the HOOK
# cancels the dialog -- which is also the strongest possible evidence that it
# ran, since a hook that was never entered leaves a dialog nobody closes and
# this leg times out instead of quietly passing.
#
# Four claims:
#   both runs agree, byte for byte  -- the guest saw what a native caller sees
#   the hook was ENTERED            -- printed, and implied by the run ending
#   cust_ok=1                       -- the OPENFILENAME the hook was handed is
#                                      THE CALLER'S OWN: it read the probe's
#                                      cookie back out of lCustData.  That is
#                                      what makes the in-place swap in
#                                      guestthunk.c a checked decision rather
#                                      than a preference
#   the trace shows the swap        -- by name, on the +guestcb channel
native_build "$OUT/hook.exe" -DSHELL_SMOKE_HOOK || {
    sed 's/^/  hook-native| /' "$OUT/native.build.err" >&2
    skip "the -DSHELL_SMOKE_HOOK native build failed"
}
guest_build "$OUT/hook_guest.exe" -DSHELL_SMOKE_HOOK || {
    sed 's/^/  hook-guest| /' "$OUT/guest.build.err" >&2
    skip "the -DSHELL_SMOKE_HOOK guest build failed"
}
run_wine "$OUT/hook.exe" > "$OUT/hook.native.out" 2>"$OUT/hook.native.err"
run_wine "$OUT/hook_guest.exe" WINEDEBUG=+guestcb \
    > "$OUT/hook.guest.out" 2>"$OUT/hook.guest.err"

hook_line() { grep -m1 "GetOpenFileNameA" "$1" | sed 's/.*: //'; }
g_hook=$(hook_line "$OUT/hook.guest.out")
n_hook=$(hook_line "$OUT/hook.native.out")
if [ -z "$g_hook" ] || [ -z "$n_hook" ]; then
    sed 's/^/  hook-guest| /' "$OUT/hook.guest.out" >&2
    sed 's/^/  hook-native| /' "$OUT/hook.native.out" >&2
    tail -20 "$OUT/hook.guest.err" >&2
    bad "one of the GetOpenFileName runs printed nothing for the leg to compare"
elif ! cmp -s "$OUT/hook.native.out" "$OUT/hook.guest.out"; then
    diff "$OUT/hook.native.out" "$OUT/hook.guest.out" >&2
    bad "the native and guest GetOpenFileName-with-a-hook runs differ"
elif ! echo "$g_hook" | grep -q "entered=1"; then
    bad "the dialog hook was never entered ('$g_hook') -- comdlg32 either \
refused the call or installed nothing"
elif ! echo "$g_hook" | grep -q "cust_ok=1"; then
    bad "the hook ran but was not handed the caller's own OPENFILENAME \
('$g_hook'); the swap must be IN PLACE, not into a copy"
elif ! grep -qE "GetOpenFileNameA: hook .* -> trampoline" "$OUT/hook.guest.err"; then
    bad "the guest's hook ran but comdlg32 never swapped it for a trampoline \
-- native comdlg32 must have been handed the raw guest pointer"
elif grep -q "refusing GetOpenFileNameA" "$OUT/hook.guest.err"; then
    bad "comdlg32 still refuses GetOpenFileNameA with OFN_ENABLEHOOK"
else
    say "dialog hook: guest and native both $g_hook, and the hook crossed \
through $(grep -m1 -oE 'hook [^ ]+ -> trampoline [^ ]+' "$OUT/hook.guest.err")"
fi

[ $fail -eq 0 ] && say "PASS"
cleanup
exit $fail
