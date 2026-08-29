#!/bin/sh
#
# check-d3d9-smoke.sh -- the native-vs-guest D3D9 RUNTIME gate.
#
# The D3D9 sibling of check-d3d11-smoke.sh, and everything structural is that
# file's: build probes/d3d9_smoke.c TWICE from ONE source -- once as a native
# ppc64le ELF binary that dlopens DXVK's libdxvk_d3d9.so directly, with no Wine
# and no emulation anywhere in the process, and once as an x86-64 guest PE that
# imports d3d9.dll and runs under this port's wine -- and require the two runs
# to print BYTE-IDENTICAL stdout.  Every line either run prints is a value the
# real GPU pipeline computed, so identical output means the guest reached the
# same implementation with nothing lost, swapped or silently defaulted on the
# way.
#
# WHAT IS DIFFERENT FROM THE D3D11 GATE, AND IT IS ONE THING.  D3D11 has no
# implicit swapchain: D3D11CreateDevice returns a bare device with no window,
# so the offscreen gate simply never asks for one.  D3D9 has no such option --
# IDirect3D9::CreateDevice ALWAYS creates one implicit swapchain tied to the
# HWND it is passed, whether the application ever presents or not, and DXVK
# builds its Presenter (and its VkSurfaceKHR) inside CreateDevice.
#
# [MEASURED] 2026-08-17, the test machine: with `DXVK_WSI_DRIVER=Headless` and no display
# variables at all, CreateDevice returns 0x8876086A (D3DERR_NOTAVAILABLE) and
# DXVK says `Foreign WSI: headless driver cannot create a surface` followed by
# `Presenter: Failed to create Vulkan surface`.  With
# `DXVK_CONFIG="d3d9.deferSurfaceCreation = True"` -- which is upstream DXVK's
# own option, defaulted to false in d3d9_options.cpp and set for many titles in
# its application profiles -- the Presenter constructor skips surface creation
# entirely and the device, its render targets and the whole offscreen pipeline
# come up.  So BOTH legs set it, and neither leg presents.  What this gate
# proves is the COM boundary; ppc64le/dxvk/check-present-smoke.sh is where a
# frame reaching a screen is proved, and it does that on D3D11.
#
# Six legs:
#
#   A  BUILD: libdxvk_d3d9.so exists under the meson build tree.
#   B  ISA FLOOR: scan-isa.sh reports the build tree CLEAN against the
#      -mcpu=power8 floor.
#   C  NATIVE: the probe, built for ppc64le and run headless with no Wine and
#      no guest in the process, reports PASS.  Steps 9-11 are the DEPTH
#      claim: a D3DFMT_D32F_LOCKABLE depth surface is cleared to two
#      compile-time-known Z values in turn and every one of its 4096
#      texels is read back and compared bit-exactly, the same way step 7
#      walks the colour target.  Depth is the only value on this surface
#      that reaches DXVK through a floating-point argument register, and
#      a Z that arrives wrong is a silently wrong buffer, not a crash.
#   D  GUEST: the same probe, built as an x86-64 PE, runs under the emulator
#      and reports PASS.
#   E  IDENTITY: cmp(native stdout, guest stdout) is empty.
#   F  REFUSAL: the port's own +winecom trace must name a refusal by method
#      when the guest drives one -- see below; on this surface there are only
#      two refused slots and both are the GDI device-context pair.
#
# --sabotage runs the negative controls instead and requires them to go red:
#
#   1  WINEEMUNOCOMWRAP=1 hands the guest raw host pointers -- the exact defect
#      this port's proxy runtime exists to fix -- and the guest run MUST NOT
#      print "d3d9_smoke: PASS".
#   2  The native probe rebuilt with -DSMOKE_BREAK=1..5 MUST each FAIL.  1-3
#      are the colour cases; 4 and 5 are the depth pair added with steps
#      9-11 -- skipping the second depth Clear, and checking the first
#      depth readback against a value nothing ever wrote.
#   3  The native probe WITHOUT the deferSurfaceCreation option must fail at
#      device creation, because that is the measurement the whole recipe above
#      rests on and a gate should not carry an unfalsifiable premise.
#
# Exit 0 = pass, 1 = a check failed, 2 = could not run at all (a skip is NOT a
# pass).
#
# WHY EVERY WINE RUN DISABLES winedbg: verbatim from check-d3d11-smoke.sh,
# because the hazard is identical -- see that file.
set -u

HERE=$(cd "$(dirname "$0")" && pwd)
SRC=$(cd "$HERE/../.." && pwd)
BUILD=${BUILD:-$SRC}
OUT=${OUT:-/tmp/d3d9-smoke}
SABOTAGE=0
[ "${1:-}" = "--sabotage" ] && SABOTAGE=1

say()  { echo "check-d3d9-smoke: $*"; }
bad()  { echo "check-d3d9-smoke: FAIL $*" >&2; fail=1; }
note() { echo "check-d3d9-smoke: note $*"; }
skip() { echo "check-d3d9-smoke: $*" >&2; exit 2; }

[ -x "$BUILD/wine" ] || skip "no wine loader at $BUILD/wine"
[ -n "${WINEPREFIX:-}" ] || skip "set WINEPREFIX to a prefix wineboot has run in"
[ -n "${WINEFEXBRIDGE:-}" ] || skip "set WINEFEXBRIDGE to the emulator bridge"
[ -f "$BUILD/dlls/d3d9/x86_64-windows/d3d9.dll" ] || \
    skip "no guest d3d9 thunk at $BUILD/dlls/d3d9/x86_64-windows/d3d9.dll; \
build it first"
command -v clang >/dev/null || skip "need clang for the guest build"
command -v llvm-dlltool >/dev/null || skip "need llvm-dlltool for the guest build"
command -v "${CC:-gcc}" >/dev/null || skip "need ${CC:-gcc} for the native ppc64le build"

mkdir -p "$OUT" || skip "cannot create $OUT"

# DXVK writes <appname>_d3d11.log next to the CURRENT DIRECTORY by default, and
# a gate is normally run from the top of the source tree -- so a plain run left
# native_d3d11.log, native_dxgi.log and a wine-preloader_* pair lying in the
# checkout.  Point them at this gate's own work directory instead; DXVK_LOG_PATH
# takes a directory, and "none" would suppress the logs entirely, which is worse
# when a leg fails and the log is the evidence.
DXVK_LOG_PATH=$OUT
export DXVK_LOG_PATH
fail=0

INCL="-I$BUILD/include -I$SRC/include -I$SRC/include/msvcrt"
TIMEOUT=${TIMEOUT:-120}

# The option the measurement above says both legs need, and the one variable
# that must be identical on both sides or the two transcripts are not
# comparable.  Named once, here.
DEFER='d3d9.deferSurfaceCreation = True'

# ---- A: build check --------------------------------------------------------
DXVKBUILD="$BUILD/ppc64le/dxvk-build"
D3D9_SO="$DXVKBUILD/src/d3d9/libdxvk_d3d9.so"
[ -e "$D3D9_SO" ] || \
    skip "missing $D3D9_SO -- run ppc64le/dxvk/build-for-wine.sh $DXVKBUILD first"
say "build: $D3D9_SO is present"

# ---- B: ISA floor -----------------------------------------------------------
[ -x "$HERE/scan-isa.sh" ] || skip "no $HERE/scan-isa.sh"
SCANOUT="$OUT/scan-isa.out"
"$HERE/scan-isa.sh" "$DXVKBUILD" >"$SCANOUT" 2>&1
scan_rc=$?
case $scan_rc in
    0) say "isa: $(grep -m1 'CLEAN' "$SCANOUT")" ;;
    1) sed 's/^/  scan-isa| /' "$SCANOUT" >&2
       bad "the dxvk build tree contains instructions above the -mcpu=power8 floor" ;;
    *) sed 's/^/  scan-isa| /' "$SCANOUT" >&2
       skip "scan-isa.sh could not run (exit $scan_rc); the ISA floor cannot be asserted" ;;
esac

# ---- build: the native ppc64le ELF leg --------------------------------------
NATIVE_INC_BASE="$HERE/src/include/native"
[ -d "$NATIVE_INC_BASE" ] || \
    skip "no DXVK native headers at $NATIVE_INC_BASE -- run \
ppc64le/dxvk/build-for-wine.sh (which runs bootstrap.sh if needed) first"
NATIVE_INC="-I$NATIVE_INC_BASE -I$NATIVE_INC_BASE/windows -I$NATIVE_INC_BASE/directx"
NATIVECC="${CC:-gcc} -std=c11 -O2 -mcpu=power8 $NATIVE_INC -Wall -fno-builtin"

$NATIVECC -DD3D9_SMOKE_NATIVE -c -o "$OUT/native.o" "$HERE/probes/d3d9_smoke.c" \
    || skip "native compile failed"
${CC:-gcc} -o "$OUT/native" "$OUT/native.o" -ldl || skip "native link failed"

# ---- build: the x86-64 guest PE leg -----------------------------------------
cat > "$OUT/kernel32.def" <<'EOF'
LIBRARY kernel32.dll
EXPORTS
GetStdHandle
WriteFile
ExitProcess
EOF
cat > "$OUT/d3d9.def" <<'EOF'
LIBRARY d3d9.dll
EXPORTS
Direct3DCreate9
EOF
for m in kernel32 d3d9; do
    llvm-dlltool -m i386:x86-64 -d "$OUT/$m.def" -l "$OUT/lib$m.a" \
        || skip "llvm-dlltool failed for $m"
done

GUESTCC="clang -target x86_64-windows-gnu -nostdlibinc $INCL -Wall -O1 -fno-builtin -g"
GUESTLD="clang -target x86_64-windows-gnu -fuse-ld=lld -nostdlib \
-Wl,--entry=d3d9_smoke_entry -Wl,--subsystem,console"

$GUESTCC -c -o "$OUT/guest.o" "$HERE/probes/d3d9_smoke.c" 2>"$OUT/guest.build.err" \
    || { sed 's/^/  guest| /' "$OUT/guest.build.err" >&2; skip "guest compile failed"; }
$GUESTLD -o "$OUT/guest.exe" "$OUT/guest.o" "$OUT/libd3d9.a" "$OUT/libkernel32.a" \
    2>>"$OUT/guest.build.err" \
    || { sed 's/^/  guest| /' "$OUT/guest.build.err" >&2; skip "guest link failed"; }

WDBG=${WINEDEBUG:--all}

# Both legs run with the SAME DXVK options and no display of any kind.
# XDG_RUNTIME_DIR is unset as well as DISPLAY and WAYLAND_DISPLAY: a Wayland
# client finds a compositor through the default socket name in that directory
# even with WAYLAND_DISPLAY unset, which is how a headless test quietly stops
# being headless -- and, worse, ends up talking to the caller's own session.
run_native() {   # $@ = extra env assignments
    env -u DISPLAY -u WAYLAND_DISPLAY -u XDG_RUNTIME_DIR \
        DXVK_WSI_DRIVER=Headless DXVK_CONFIG="$DEFER" "$@" \
        timeout -k 5 "$TIMEOUT" "$1" >/dev/null 2>&1
}

# ---- (also available standalone as --sabotage): the negative controls -----
sabotage() {
    ok=1

    # part 1: WINEEMUNOCOMWRAP=1 hands the guest raw host pointers.
    timeout -k 5 "$TIMEOUT" \
        env -u DISPLAY -u WAYLAND_DISPLAY -u XDG_RUNTIME_DIR \
        WINEDEBUG="$WDBG" WINEDLLOVERRIDES="winedbg.exe=d" WINEEMUNOCOMWRAP=1 \
        DXVK_CONFIG="$DEFER" \
        "$BUILD/wine" "$OUT/guest.exe" \
        > "$OUT/sabotage_wrap.out" 2>"$OUT/sabotage_wrap.err"
    if grep -q "d3d9_smoke: PASS" "$OUT/sabotage_wrap.out"; then
        bad "WINEEMUNOCOMWRAP=1 still PASSED -- the gate cannot go red"; ok=0
    else
        say "sabotage: WINEEMUNOCOMWRAP=1 failed the guest run at '$(tail -1 \
            "$OUT/sabotage_wrap.out" | cut -c1-60)', as it must"
    fi

    # part 2: each SMOKE_BREAK variant, native, must FAIL.  Cases 4 and 5
    # are the depth pair: 4 skips the second depth Clear so the surface
    # still holds the first value, 5 checks the first readback against a
    # depth nothing ever wrote.  A depth assertion that cannot go red for
    # both reasons is not asserting on the depth VALUES.
    for n in 1 2 3 4 5; do
        if $NATIVECC -DD3D9_SMOKE_NATIVE -DSMOKE_BREAK=$n \
                -c -o "$OUT/native_break$n.o" "$HERE/probes/d3d9_smoke.c" \
                2>"$OUT/native_break$n.build.err" \
           && ${CC:-gcc} -o "$OUT/native_break$n" "$OUT/native_break$n.o" -ldl \
                2>>"$OUT/native_break$n.build.err"; then
            timeout -k 5 "$TIMEOUT" \
                env -u DISPLAY -u WAYLAND_DISPLAY -u XDG_RUNTIME_DIR \
                DXVK_WSI_DRIVER=Headless DXVK_CONFIG="$DEFER" \
                "$OUT/native_break$n" "$D3D9_SO" \
                > "$OUT/native_break$n.out" 2>"$OUT/native_break$n.err"
            if grep -q "d3d9_smoke: PASS" "$OUT/native_break$n.out"; then
                bad "SMOKE_BREAK=$n PASSED; the falsification build must FAIL"; ok=0
            else
                say "sabotage: SMOKE_BREAK=$n failed as it must: $(tail -1 \
                    "$OUT/native_break$n.out" | cut -c1-100)"
            fi
        else
            bad "SMOKE_BREAK=$n native build failed; cannot prove this check can fail"
            tail -5 "$OUT/native_break$n.build.err" | sed 's/^/  break'"$n"'| /' >&2
            ok=0
        fi
    done

    # part 3: the premise this gate's whole recipe rests on.  Without the
    # deferSurfaceCreation option the identical binary must fail at device
    # creation -- if it ever stops failing, the comment at the top of this
    # file has become false and somebody should find out from here rather
    # than from a green run that means something else.
    timeout -k 5 "$TIMEOUT" \
        env -u DISPLAY -u WAYLAND_DISPLAY -u XDG_RUNTIME_DIR \
        DXVK_WSI_DRIVER=Headless \
        "$OUT/native" "$D3D9_SO" \
        > "$OUT/nodefer.out" 2>"$OUT/nodefer.err"
    if grep -q "d3d9_smoke: PASS" "$OUT/nodefer.out"; then
        bad "without d3d9.deferSurfaceCreation the probe still PASSED -- the \
measurement this gate's recipe rests on no longer holds, and both legs are \
now setting an option they do not need"; ok=0
    else
        say "sabotage: without deferSurfaceCreation the native run failed at \
'$(grep -m1 'step 2' "$OUT/nodefer.out" | cut -c1-90)', as the measurement says"
    fi

    [ "$ok" = 1 ] && say "SABOTAGE PASS"
    [ "$ok" = 1 ]
}

if [ "$SABOTAGE" = 1 ]; then
    sabotage
    exit $?
fi

# ---- C: native ---------------------------------------------------------
timeout -k 5 "$TIMEOUT" \
    env -u DISPLAY -u WAYLAND_DISPLAY -u XDG_RUNTIME_DIR \
    DXVK_WSI_DRIVER=Headless DXVK_CONFIG="$DEFER" \
    "$OUT/native" "$D3D9_SO" > "$OUT/native.out" 2>"$OUT/native.err"
nst=$?
if [ $nst -eq 124 ] || [ $nst -eq 137 ]; then
    bad "the native run timed out after ${TIMEOUT}s"
elif grep -q "d3d9_smoke: PASS" "$OUT/native.out"; then
    say "native: $(tail -1 "$OUT/native.out")"
else
    sed 's/^/  native| /' "$OUT/native.out" >&2
    tail -20 "$OUT/native.err" >&2
    bad "the native ppc64le build did not pass"
fi

# ---- D: guest -----------------------------------------------------------
timeout -k 5 "$TIMEOUT" \
    env -u DISPLAY -u WAYLAND_DISPLAY -u XDG_RUNTIME_DIR \
    WINEDEBUG="$WDBG" WINEDLLOVERRIDES="winedbg.exe=d" DXVK_CONFIG="$DEFER" \
    "$BUILD/wine" "$OUT/guest.exe" > "$OUT/guest.out" 2>"$OUT/guest.err"
gst=$?
if [ $gst -eq 124 ] || [ $gst -eq 137 ]; then
    bad "the guest run timed out after ${TIMEOUT}s"
elif grep -q "d3d9_smoke: PASS" "$OUT/guest.out"; then
    say "guest:  $(tail -1 "$OUT/guest.out")"
else
    sed 's/^/  guest| /' "$OUT/guest.out" >&2
    tail -20 "$OUT/guest.err" >&2
    bad "the x86-64 guest build did not pass"
fi

# ---- E: identity ----------------------------------------------------------
if cmp -s "$OUT/native.out" "$OUT/guest.out"; then
    say "identity: native and guest output is byte-identical ($(wc -l \
        < "$OUT/native.out") lines)"
else
    FIRST=$(diff "$OUT/native.out" "$OUT/guest.out" | head -1)
    bad "native and guest output differ; first difference: $FIRST"
    diff "$OUT/native.out" "$OUT/guest.out" | sed 's/^/  /' | head -20 >&2
fi

# ---- F: the refusals are loud ----------------------------------------------
# Only two slots on this whole surface are refused, and both are the GDI
# device-context pair -- so unlike the D3D11 gate there is no separate probe
# build for this.  What is checked instead is that the generator and the
# runtime agree about how many there are: a surface whose refusal count
# silently went to zero would mean the marshal tables were regenerated with
# something switched off, and a call that should refuse would be served.
if command -v python3 >/dev/null && [ -f "$HERE/interfaces_d3d9.json" ]; then
    refused=$(grep -c 'IDirect3DSurface9::\(Get\|Release\)DC:' \
              "$SRC/dlls/d3d9/d3d9_marshal.h" 2>/dev/null || echo 0)
    if [ "$refused" -ge 2 ]; then
        say "refusals: $refused named refusal(s) in the marshal tables \
(IDirect3DSurface9::GetDC/ReleaseDC, the GDI device-context pair)"
    else
        bad "the D3D9 marshal tables carry $refused named refusal(s); the two \
GDI device-context slots must be refused and are not"
    fi
else
    note "leg F: no python3 or no roster; the refusal count was not checked"
fi

[ $fail -eq 0 ] && say "PASS"
exit $fail
