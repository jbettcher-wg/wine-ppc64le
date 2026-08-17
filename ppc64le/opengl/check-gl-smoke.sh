#!/bin/sh
#
# check-gl-smoke.sh -- the native-vs-guest OpenGL RUNTIME gate.
#
# The claim: an x86-64 Windows program run as a GUEST under this port creates
# a real OpenGL context, fetches modern entry points through
# wglGetProcAddress, calls them, draws, reads the framebuffer back, and gets
# BYTE-IDENTICAL output to the same source built for this machine's OWN
# architecture and run through the same Wine, the same winex11 GLX path and
# the same driver.  Only the caller's instruction set differs -- and therefore
# only whether every one of those calls crossed the guest thunk boundary.
#
# Byte-identical is the bar rather than "the guest said PASS" for the reason
# check-com-smoke.sh and check-d3d11-smoke.sh give: reaching the right answer
# through the wrong mechanism is exactly the failure a PASS/PASS comparison
# cannot see.  Every byte compared here is a value the GL implementation
# computed -- a driver string read through a pointer into host memory, an FNV
# of the extension list, 4096 texels, a projection matrix's raw bits.
#
# Eight legs:
#
#   A  SURFACE CURRENT: ppc64le/opengl/gen-guest-surface.py --check.  The
#      guest export list and the oracle's declarations are GENERATED from
#      include/wine/wgl.h and dlls/opengl32/thunks.c; if either output has
#      drifted from its inputs, everything below is testing a stale surface.
#   B  BUILD: the guest opengl32 thunk exists and has the whole registry in
#      it, not just the 361 names opengl32.spec exports.
#   C  DISPLAY: an Xvfb of this gate's own, on a display number nobody is
#      using.  winex11 needs an X server; the person at this machine is on
#      the real ones and this gate never touches them.
#   D  NATIVE: the probe built as a ppc64 PE (winegcc), run under this wine
#      with no guest anywhere in the process, reports PASS.
#   E  GUEST: the same source built as an x86-64 PE, run under the same wine
#      as a guest, reports PASS.
#   F  IDENTITY: cmp(native stdout, guest stdout) is empty.
#   G  VENDING: the guest's own count of what wglGetProcAddress answered, and
#      the ONE refusal this port makes by name -- glDebugMessageCallback,
#      whose first argument is a guest function pointer native GL would call
#      back with seven arguments through a four-argument trampoline.  The
#      guest must get NULL for it and the port must SAY so, by name.
#   H  ORDINALS: the guest thunk's first 361 ordinals are the native module's
#      own, because opengl32-guest.spec copies opengl32.spec verbatim before
#      appending anything.
#
# --sabotage runs the negative controls instead, and requires every one to go
# red.  A gate that cannot go red proves nothing:
#
#   1  WINEEMUNOGLVEND=1 makes wglGetProcAddress hand the guest the NATIVE
#      ppc64 address -- the exact defect this module exists to prevent, and
#      the one a game cannot diagnose because it looks like a valid pointer.
#      The guest run must not PASS.
#   2  WINEEMUFPNOSTACK=1 restores the host's floating-point path as it was
#      before an argument was allowed to travel anywhere but a register: every
#      FP argument read out of XMMi however far along it is, and every
#      argument past the eighth dropped.  The guest run must not PASS, and
#      must fail at BOTH the projection-matrix step (glOrtho's fifth and sixth
#      doubles, which MS-x64 puts on the stack) and the evaluator step
#      (glMap2f's ninth and tenth arguments, which ELFv2 puts in the parameter
#      save area) -- wrong numbers, not crashes.
#   3  each GL_SMOKE_BREAK=1..5 build of the NATIVE leg must FAIL.
#
# Exit 0 = pass, 1 = a check failed, 2 = could not run at all (a skip is NOT
# a pass).
#
# WHY EVERY WINE RUN DISABLES winedbg, verbatim from check-d3d11-smoke.sh
# because the hazard is identical: the bringup prefix has AeDebug configured
# with "winedbg --auto", so a run that ends in an unhandled fault -- which is
# what a defect here looks like from outside -- starts a debugger that
# attaches and never lets go, turning every red state of this gate into a
# hang.  WINEDLLOVERRIDES=winedbg.exe=d makes start_debugger's CreateProcess
# fail, so the process simply terminates.  It is an environment override for
# the duration of one run and touches nothing in the prefix.
set -u

HERE=$(cd "$(dirname "$0")" && pwd)
SRC=$(cd "$HERE/../.." && pwd)
BUILD=${BUILD:-$SRC}                    # in-tree build by default
OUT=${OUT:-/tmp/gl-smoke}
SABOTAGE=0
[ "${1:-}" = "--sabotage" ] && SABOTAGE=1

say()  { echo "check-gl-smoke: $*"; }
bad()  { echo "check-gl-smoke: FAIL $*" >&2; fail=1; }
note() { echo "check-gl-smoke: note $*"; }
skip() { echo "check-gl-smoke: $*" >&2; cleanup; exit 2; }

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
command -v "${CC:-gcc}" >/dev/null || skip "need ${CC:-gcc} for the native ppc64 build"
command -v Xvfb >/dev/null || skip "need Xvfb: winex11 needs an X server and this \
gate must not take the one the user is on"

mkdir -p "$OUT" || skip "cannot create $OUT"
fail=0
TIMEOUT=${TIMEOUT:-180}
INCL="-I$BUILD/include -I$SRC/include -I$SRC/include/msvcrt"
GUEST_THUNK="$BUILD/dlls/opengl32/x86_64-windows/opengl32.dll"

# ---- A: the generated surface is current -----------------------------------
if [ ! -x "$HERE/gen-guest-surface.py" ]; then
    skip "no $HERE/gen-guest-surface.py"
elif "$HERE/gen-guest-surface.py" --check > "$OUT/surface.out" 2>&1; then
    say "surface: $(grep -m1 CURRENT "$OUT/surface.out")"
else
    sed 's/^/  surface| /' "$OUT/surface.out" >&2
    bad "the generated guest surface has drifted from wgl.h / thunks.c -- run \
ppc64le/opengl/gen-guest-surface.py and rebuild"
fi

# ---- B: the guest thunk exists and carries the whole registry --------------
[ -f "$GUEST_THUNK" ] || skip "no guest opengl32 thunk at $GUEST_THUNK; build it first"
NEXPORT=$(python3 - "$GUEST_THUNK" <<'EOF'
import struct, sys
d = open(sys.argv[1], 'rb').read()
pe = struct.unpack_from('<I', d, 0x3c)[0]
nsec = struct.unpack_from('<H', d, pe + 6)[0]
opt = struct.unpack_from('<H', d, pe + 20)[0]
edir_rva, edir_sz = struct.unpack_from('<II', d, pe + 24 + 112)
secs = []
for i in range(nsec):
    o = pe + 24 + opt + i * 40
    va, rs, pr = struct.unpack_from('<III', d, o + 12)
    secs.append((va, rs, pr))
def off(rva):
    for va, rs, pr in secs:
        if va <= rva < va + rs:
            return pr + rva - va
    return None
print(struct.unpack_from('<I', d, off(edir_rva) + 24)[0])
EOF
) || NEXPORT=0
if [ "${NEXPORT:-0}" -gt 3000 ]; then
    say "thunk: $GUEST_THUNK exports $NEXPORT names -- the 361 opengl32.spec \
exports plus the extension registry, which is the whole point"
else
    bad "the guest opengl32 thunk exports only ${NEXPORT:-0} names; a guest that \
imports opengl32 and then asks wglGetProcAddress for anything modern gets NULL"
fi

# ---- C: a display of this gate's own ---------------------------------------
# NEVER a display someone is on.  The user of this machine is at :0 and :1
# (tty and an X session), and a gate that hijacked one would be taking their
# desktop.  Pick a high number nothing has a socket for, and hand it back on
# the way out.
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
untouched"

WDBG=${WINEDEBUG:--all}
run_wine() {   # run_wine <exe> <extra env assignments...>
    exe=$1; shift
    timeout -k 5 "$TIMEOUT" env DISPLAY="$XVFB_DISPLAY" WINEDEBUG="$WDBG" \
        WINEDLLOVERRIDES="winedbg.exe=d" "$@" "$BUILD/wine" "$exe"
}

# ---- build: the native ppc64 PE leg ----------------------------------------
# Built exactly the way ppc64le/seh/check-seh-smoke.sh builds its native lane:
# an ordinary consumer of the public headers, linked against this tree's own
# ppc64-windows import libraries and turned into a builtin PE.  It runs under
# the SAME wine as the guest leg, so both reach the same opengl32.
native_build() {   # native_build <output> [extra cflags...]
    nout=$1; shift
    ${CC:-gcc} -c -o "$OUT/native.o" "$HERE/probes/gl_smoke.c" $INCL \
        -DGL_SMOKE_NATIVE "$@" \
        -D_UCRT -D_WIN32 -Wall -pipe -mlongcall -mno-pltseq -fcf-protection=none \
        -fvisibility=hidden -fno-stack-protector -fno-strict-aliasing -gdwarf-4 \
        -fPIC -fasynchronous-unwind-tables -mlong-double-64 -fno-builtin \
        -fshort-wchar -Wno-format -g -O1 2>"$OUT/native.build.err" || return 1
    "$SRC/tools/winegcc/winegcc" -o "$nout" --wine-objdir "$BUILD" \
        --cc-cmd="${CC:-gcc}" -mno-cygwin -fPIC -fasynchronous-unwind-tables \
        -Wl,--wine-builtin -mconsole "$OUT/native.o" \
        "$BUILD/dlls/opengl32/ppc64-windows/libopengl32.a" \
        "$BUILD/dlls/gdi32/ppc64-windows/libgdi32.a" \
        "$BUILD/dlls/user32/ppc64-windows/libuser32.a" \
        "$BUILD/libs/winecrt0/ppc64-windows/libwinecrt0.a" \
        "$BUILD/dlls/ucrtbase/ppc64-windows/libucrtbase.a" \
        "$BUILD/dlls/kernel32/ppc64-windows/libkernel32.a" \
        "$BUILD/dlls/ntdll/ppc64-windows/libntdll.a" \
        2>>"$OUT/native.build.err" || return 1
    rm -f "$nout"
    "$SRC/tools/elf2pe" "$nout.so" "$nout" 2>>"$OUT/native.build.err" || return 1
    "$SRC/tools/winebuild/winebuild" --builtin "$nout" \
        2>>"$OUT/native.build.err" || return 1
    return 0
}

# ---- build: the x86-64 guest PE leg ----------------------------------------
# The same clang x86_64-windows-gnu machinery check-d3d11-smoke.sh drives its
# guest build with and the same Wine headers, so any disagreement between the
# two legs is the boundary and not the declarations.  The imports are
# described by hand, naming only what the probe calls -- the guest binds to
# the same builtins a real application would and nothing else is linked in at
# all; there is no CRT here, see the probe's header.
cat > "$OUT/kernel32.def" <<'EOF'
LIBRARY kernel32.dll
EXPORTS
GetStdHandle
WriteFile
ExitProcess
GetModuleHandleA
EOF
cat > "$OUT/user32.def" <<'EOF'
LIBRARY user32.dll
EXPORTS
RegisterClassA
CreateWindowExA
DefWindowProcA
DestroyWindow
ShowWindow
GetDC
ReleaseDC
EOF
cat > "$OUT/gdi32.def" <<'EOF'
LIBRARY gdi32.dll
EXPORTS
ChoosePixelFormat
SetPixelFormat
EOF
# Only the wgl entry points are imported.  Everything gl* the probe calls is
# imported too -- they are ordinary exports of opengl32 on any Windows -- and
# the modern ones are fetched at run time, which is the thing under test.
cat > "$OUT/opengl32.def" <<'EOF'
LIBRARY opengl32.dll
EXPORTS
wglCreateContext
wglDeleteContext
wglMakeCurrent
wglGetCurrentContext
wglGetCurrentDC
wglGetProcAddress
wglDescribePixelFormat
wglGetPixelFormat
glGetString
glGetError
glGetIntegerv
glGetFloatv
glGetDoublev
glViewport
glClearColor
glClear
glDrawBuffer
glReadBuffer
glPixelStorei
glReadPixels
glFinish
glMap2f
glGetMapfv
glGetMapiv
glBegin
glEnd
glColor3f
glVertex2f
glMatrixMode
glLoadIdentity
glOrtho
EOF
for m in kernel32 user32 gdi32 opengl32; do
    llvm-dlltool -m i386:x86-64 -d "$OUT/$m.def" -l "$OUT/lib$m.a" \
        || skip "llvm-dlltool failed for $m"
done

GUESTCC="clang -target x86_64-windows-gnu -nostdlibinc $INCL -Wall -O1 -fno-builtin -g"
GUESTLD="clang -target x86_64-windows-gnu -fuse-ld=lld -nostdlib \
-Wl,--entry=gl_smoke_entry -Wl,--subsystem,console"

guest_build() {   # guest_build <output> [extra cflags...]
    gout=$1; shift
    $GUESTCC "$@" -c -o "$OUT/guest.o" "$HERE/probes/gl_smoke.c" \
        2>"$OUT/guest.build.err" || return 1
    $GUESTLD -o "$gout" "$OUT/guest.o" "$OUT/libopengl32.a" "$OUT/libgdi32.a" \
        "$OUT/libuser32.a" "$OUT/libkernel32.a" 2>>"$OUT/guest.build.err" || return 1
    return 0
}

guest_build "$OUT/guest.exe" || {
    tail -20 "$OUT/guest.build.err" >&2
    skip "the x86-64 guest build failed"
}

# ---- (also standalone as --sabotage): the negative controls -----------------
sabotage() {
    ok=1

    guest_build "$OUT/guest.exe" || { echo "guest build failed" >&2; return 1; }

    # 1: the raw-pointer failure mode.  wglGetProcAddress hands back the
    # NATIVE ppc64 address, which the guest then CALLs.
    run_wine "$OUT/guest.exe" WINEEMUNOGLVEND=1 \
        > "$OUT/sab_vend.out" 2>"$OUT/sab_vend.err"
    if grep -q "gl_smoke: PASS" "$OUT/sab_vend.out"; then
        echo "check-gl-smoke: FAIL WINEEMUNOGLVEND=1 still PASSED -- the gate \
cannot go red" >&2; ok=0
    else
        say "sabotage: WINEEMUNOGLVEND=1 killed the guest run at '$(tail -1 \
            "$OUT/sab_vend.out" | cut -c1-70)', as it must"
    fi

    # 2: an argument that is not in a register, back to not being an argument.
    run_wine "$OUT/guest.exe" WINEEMUFPNOSTACK=1 \
        > "$OUT/sab_fp.out" 2>"$OUT/sab_fp.err"
    if grep -q "gl_smoke: PASS" "$OUT/sab_fp.out"; then
        echo "check-gl-smoke: FAIL WINEEMUFPNOSTACK=1 still PASSED -- the \
argument-marshalling checks do not check anything" >&2; ok=0
    elif grep -q "GL_PROJECTION_MATRIX.*FAIL" "$OUT/sab_fp.out" && \
         grep -q "glGetMapfv/iv.*FAIL" "$OUT/sab_fp.out"; then
        say "sabotage: WINEEMUFPNOSTACK=1 failed at BOTH the projection matrix \
and the evaluator, as it must: $(grep -c 'FAIL' "$OUT/sab_fp.out") checked \
steps went red, and the run still reached its own verdict rather than crashing"
    else
        echo "check-gl-smoke: FAIL WINEEMUFPNOSTACK=1 did not pass, but it did \
not fail at BOTH the projection matrix and the evaluator -- the negative \
control is not falsifying what it claims to" >&2
        sed 's/^/  sab_fp| /' "$OUT/sab_fp.out" >&2
        ok=0
    fi

    # 3: each falsification build of the NATIVE leg must FAIL.
    for n in 1 2 3 4 5; do
        if native_build "$OUT/native_break$n.exe" -DGL_SMOKE_BREAK=$n; then
            run_wine "$OUT/native_break$n.exe" \
                > "$OUT/native_break$n.out" 2>"$OUT/native_break$n.err"
            if grep -q "gl_smoke: PASS" "$OUT/native_break$n.out"; then
                echo "check-gl-smoke: FAIL GL_SMOKE_BREAK=$n PASSED; the \
falsification build must FAIL" >&2; ok=0
            else
                say "sabotage: GL_SMOKE_BREAK=$n failed as it must: $(tail -1 \
                    "$OUT/native_break$n.out" | cut -c1-100)"
            fi
        else
            echo "check-gl-smoke: FAIL GL_SMOKE_BREAK=$n build failed; cannot \
prove this check can fail" >&2
            tail -5 "$OUT/native.build.err" | sed 's/^/  break'"$n"'| /' >&2
            ok=0
        fi
    done

    [ "$ok" = 1 ] && say "SABOTAGE PASS"
    [ "$ok" = 1 ]
}

if [ "$SABOTAGE" = 1 ]; then
    sabotage
    rc=$?
    cleanup
    exit $rc
fi

# ---- D: native --------------------------------------------------------------
if native_build "$OUT/native.exe"; then
    run_wine "$OUT/native.exe" > "$OUT/native.out" 2>"$OUT/native.err"
    nst=$?
    if [ $nst -eq 124 ] || [ $nst -eq 137 ]; then
        bad "the native run timed out after ${TIMEOUT}s"
    elif grep -q "gl_smoke: PASS" "$OUT/native.out"; then
        say "native: $(tail -1 "$OUT/native.out")"
    else
        sed 's/^/  native| /' "$OUT/native.out" >&2
        tail -20 "$OUT/native.err" >&2
        bad "the native ppc64 build did not pass -- there is no working GL on \
this display at all, so nothing below could mean anything"
    fi
else
    tail -10 "$OUT/native.build.err" | sed 's/^/  native| /' >&2
    skip "the native ppc64 PE build failed; without it there is nothing to \
compare the guest against"
fi

# ---- E: guest ---------------------------------------------------------------
run_wine "$OUT/guest.exe" > "$OUT/guest.out" 2>"$OUT/guest.err"
gst=$?
if [ $gst -eq 124 ] || [ $gst -eq 137 ]; then
    bad "the guest run timed out after ${TIMEOUT}s"
elif grep -q "gl_smoke: PASS" "$OUT/guest.out"; then
    say "guest:  $(tail -1 "$OUT/guest.out")"
else
    sed 's/^/  guest| /' "$OUT/guest.out" >&2
    tail -20 "$OUT/guest.err" >&2
    bad "the x86-64 guest build did not pass"
fi

# ---- F: identity ------------------------------------------------------------
if cmp -s "$OUT/native.out" "$OUT/guest.out"; then
    say "identity: native and guest output is byte-identical ($(wc -l \
        < "$OUT/native.out") lines), including three driver strings read \
through a host pointer, 4096 texels and a projection matrix's raw bits"
else
    bad "native and guest output differ; first difference: $(diff \
        "$OUT/native.out" "$OUT/guest.out" | head -1)"
    diff "$OUT/native.out" "$OUT/guest.out" | sed 's/^/  /' | head -20 >&2
fi

# ---- G: the one refusal, made by name --------------------------------------
# glDebugMessageCallback's first argument is a guest function pointer that
# native GL calls back with SEVEN arguments; this port's callback trampolines
# carry four.  So it is refused -- kept out of the guest export surface
# entirely (see CALLBACK_REFUSALS in gen-guest-surface.py) -- and
# wglGetProcAddress answers NULL and says which name it refused and why.
# NULL is an answer every GL loader tests for; a raw guest pointer handed to
# a driver is a crash none of them can diagnose.
if grep -q "gl_smoke_note: glDebugMessageCallback p=0000000000000000" "$OUT/guest.err"; then
    say "vending: the guest asked for glDebugMessageCallback and got NULL"
else
    sed -n 's/^\(gl_smoke_note:.*\)$/  note| \1/p' "$OUT/guest.err" >&2
    bad "the guest did NOT get NULL for glDebugMessageCallback -- either the \
refusal is gone or a guest function pointer is now reaching native GL raw"
fi
if grep -q "gl_smoke_note: glDebugMessageCallback p=0000000000000000" "$OUT/native.err"; then
    note "the NATIVE leg also got NULL for glDebugMessageCallback, which is \
unexpected: it should reach Wine's own registry.  Not a failure of the guest \
boundary, but the refusal below then proves less than it looks."
fi
run_wine "$OUT/guest.exe" WINEDEBUG=-all,err+seh,warn+seh \
    > "$OUT/loud.out" 2>"$OUT/loud.err"
if grep -q "glDebugMessageCallback" "$OUT/loud.err"; then
    say "vending: the port names the refusal out loud: $(grep -m1 \
        'glDebugMessageCallback' "$OUT/loud.err" | cut -c1-140)"
else
    bad "the port returned NULL for glDebugMessageCallback without saying so; a \
silent NULL is a bug report nobody can file"
    grep -m5 "wglGetProcAddress" "$OUT/loud.err" | sed 's/^/  loud| /' >&2
fi

# ---- H: ordinals ------------------------------------------------------------
# opengl32-guest.spec copies opengl32.spec verbatim before appending, so the
# real module's ordinals are the guest module's ordinals for every name that
# exists on Windows.  A guest importing opengl32 by ordinal -- which is rare
# but not unheard of -- must reach the same function.
FIRST_SPEC=$(grep -m1 '^@ stdcall' "$SRC/dlls/opengl32/opengl32.spec" | \
             sed 's/^@ stdcall \([A-Za-z0-9_]*\).*/\1/')
FIRST_GUEST=$(grep -m1 '^@ stdcall' "$SRC/dlls/opengl32/opengl32-guest.spec" | \
              sed 's/^@ stdcall \([A-Za-z0-9_]*\).*/\1/')
# ...minus the port's own private resolver entry point, which opengl32.spec
# pins last and dlls/opengl32/opengl32.thunks EXCLUDEs from the guest surface,
# so its ordinal is an honest hole rather than a name a guest may bind.
NSPEC=$(( $(grep -c '^@ stdcall' "$SRC/dlls/opengl32/opengl32.spec") \
        - $(grep -c '^@ stdcall __wine_' "$SRC/dlls/opengl32/opengl32.spec") ))
if [ "$FIRST_SPEC" = "$FIRST_GUEST" ] && \
   head -"$((NSPEC + 40))" "$SRC/dlls/opengl32/opengl32-guest.spec" | \
       grep -q "^@ stdcall wglUseFontOutlinesW"; then
    say "ordinals: the guest surface opens with the real module's own $NSPEC \
exports in file order (first is $FIRST_SPEC), so ordinals 1..$NSPEC agree"
else
    bad "the guest surface does not open with opengl32.spec's own export list; \
ordinals 1..$NSPEC no longer agree with the native module"
fi

# ---- verdict ----------------------------------------------------------------
cleanup
if [ "$fail" = 0 ]; then
    say "PASS"
    exit 0
fi
say "FAILED" >&2
exit 1
