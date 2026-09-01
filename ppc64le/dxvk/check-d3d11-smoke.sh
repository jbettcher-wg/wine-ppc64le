#!/bin/sh
#
# check-d3d11-smoke.sh -- the native-vs-guest D3D11 RUNTIME gate.
#
# ppc64le/dxvk/build-for-wine.sh proves DXVK builds clean for this machine.
# ppc64le/dxvk/scan-isa.sh proves what it builds stays on the -mcpu=power8
# floor.  Neither says the result actually WORKS -- that a guest D3D11
# call reaches the same DXVK, through whatever plumbing carries it there,
# and gets back the same answer a process with no guest in the loop at all
# would get.  That is what this file proves, the same way
# check-com-smoke.sh proved it for the COM boundary: build probes/
# d3d11_smoke.c TWICE from the one source -- once as a native ppc64le ELF
# binary that dlopens DXVK directly (no Wine, no emulation anywhere), once
# as an x86-64 guest PE that imports d3d11.dll and runs under this port's
# wine -- and require the two runs to print BYTE-IDENTICAL stdout.  Every
# line either run prints is a value the real GPU pipeline computed (a
# feature level, an FNV-1a checksum, a per-texel mismatch count), so
# identical output means the guest reached the same implementation with
# nothing lost, swapped, or silently defaulted on the way.  It is the same
# texel-exact standard dlls/d3d12/main.c's guest surface is held to
# downstream of this file, applied one layer closer to the metal.
#
# Seven legs:
#
#   A  BUILD: the three DXVK libraries (dxgi, d3d11, d3d10core) exist under
#      the meson build tree.  Not built here -- see build-for-wine.sh.
#   B  ISA FLOOR: scan-isa.sh reports the build tree CLEAN against the
#      -mcpu=power8 floor.
#   C  NATIVE: the probe, built for ppc64le and run headless with no Wine
#      and no guest in the process, reports PASS.  Establishes the expected
#      bytes with the boundary out of the picture entirely.
#   D  GUEST: the same probe, built as an x86-64 PE, runs under the
#      emulator and reports PASS.
#   E  IDENTITY: cmp(native stdout, guest stdout) is empty.
#   F  REFUSAL: a SEPARATE guest build (D3D11_SMOKE_REFUSAL -- see the
#      probe's header) drives ID3D11Device::OpenSharedResource with a
#      fabricated HANDLE, and the port's own +winecom trace must name both
#      the method and the word "refusing" -- and the process must still
#      reach its own next line rather than crash.  A defect refused loudly
#      is not this gate's problem; a defect served silently would be.
#
# --sabotage runs a negative control instead, in two parts, and requires
# BOTH to go red:
#
#   1  WINEEMUNOCOMWRAP=1 hands the guest raw host pointers -- the exact
#      defect this port's proxy runtime exists to fix -- and the guest run
#      MUST NOT print "d3d11_smoke: PASS".
#   2  The native probe rebuilt with -DSMOKE_BREAK=1, =2, and =3 (see the
#      probe's header for what each one falsifies) MUST each FAIL.
#
# A gate that cannot go red proves nothing; this is how it proves it can.
#
# Exit 0 = pass, 1 = a check failed, 2 = could not run at all (a skip is
# NOT a pass).
#
#
# WHY EVERY WINE RUN DISABLES winedbg, verbatim from check-seh-smoke.sh and
# check-seh-handlers.sh because the hazard is identical here: the bringup
# prefix has AeDebug configured with "winedbg --auto", so any run that ends
# in an unhandled fault -- which is exactly what a defect in this boundary
# looks like from the outside -- starts the debugger, which attaches, loads
# its GUI stack and never lets go.  That turns every red state of this gate
# into a hang, which is the one thing a gate must never be.
# WINEDLLOVERRIDES=winedbg.exe=d makes start_debugger's CreateProcess fail,
# so UnhandledExceptionFilter falls straight through to terminating the
# process instead.  This is an environment override for the duration of one
# run and touches nothing in the prefix.
set -u

HERE=$(cd "$(dirname "$0")" && pwd)
SRC=$(cd "$HERE/../.." && pwd)
BUILD=${BUILD:-$SRC}                    # in-tree build by default
OUT=${OUT:-/tmp/d3d11-smoke}
SABOTAGE=0
[ "${1:-}" = "--sabotage" ] && SABOTAGE=1

say()  { echo "check-d3d11-smoke: $*"; }
bad()  { echo "check-d3d11-smoke: FAIL $*" >&2; fail=1; }
note() { echo "check-d3d11-smoke: note $*"; }
skip() { echo "check-d3d11-smoke: $*" >&2; exit 2; }

[ -x "$BUILD/wine" ] || skip "no wine loader at $BUILD/wine"
[ -n "${WINEPREFIX:-}" ] || skip "set WINEPREFIX to a prefix wineboot has run in"
[ -n "${WINEFEXBRIDGE:-}" ] || skip "set WINEFEXBRIDGE to the emulator bridge"
[ -f "$BUILD/dlls/d3d11/x86_64-windows/d3d11.dll" ] || \
    skip "no guest d3d11 thunk; build it first"
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

# ---- A: build check --------------------------------------------------------
# meson bakes DT_RUNPATH $ORIGIN/../dxgi (etc.) into these exact locations
# in the BUILD tree -- see build-for-wine.sh's own note on why the libraries
# are consumed from here rather than from an install prefix.  The native
# leg below dlopens libdxvk_d3d11.so from this literal path for that reason:
# $ORIGIN resolution depends on it.
DXVKBUILD="$BUILD/ppc64le/dxvk-build"
D3D11_SO="$DXVKBUILD/src/d3d11/libdxvk_d3d11.so"
DXGI_SO="$DXVKBUILD/src/dxgi/libdxvk_dxgi.so"
D3D10_SO="$DXVKBUILD/src/d3d10/libdxvk_d3d10core.so"
for f in "$D3D11_SO" "$DXGI_SO" "$D3D10_SO"; do
    [ -e "$f" ] || \
        skip "missing $f -- run ppc64le/dxvk/build-for-wine.sh $DXVKBUILD first"
done
say "build: all three DXVK libraries are present under $DXVKBUILD/src"

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
# An ordinary Linux program, not a PE and not run under wine: DXVK's own
# headers, dlopen, no emulation anywhere in the process.  Same include
# layout dxvk-ppc64le/probes/native_d3d11_smoke.cpp documents building
# against (N=src/include/native, plus its windows/ and directx/
# subdirectories), because that vendored MinGW-w64 header set is what makes
# the SAME probe source's COBJMACROS calls compile on both legs.
NATIVE_INC_BASE="$HERE/src/include/native"
[ -d "$NATIVE_INC_BASE" ] || \
    skip "no DXVK native headers at $NATIVE_INC_BASE -- run \
ppc64le/dxvk/build-for-wine.sh (which runs bootstrap.sh if needed) first"
NATIVE_INC="-I$NATIVE_INC_BASE -I$NATIVE_INC_BASE/windows -I$NATIVE_INC_BASE/directx"
NATIVECC="${CC:-gcc} -std=c11 -O2 -mcpu=power8 $NATIVE_INC -Wall -fno-builtin"

$NATIVECC -DD3D11_SMOKE_NATIVE -c -o "$OUT/native.o" "$HERE/probes/d3d11_smoke.c" \
    || skip "native compile failed"
${CC:-gcc} -o "$OUT/native" "$OUT/native.o" -ldl || skip "native link failed"

# ---- build: the x86-64 guest PE leg -----------------------------------------
# Same clang x86_64-windows-gnu machinery check-com-smoke.sh drives its
# guest build with, and the same Wine headers, so any disagreement between
# the native and guest legs is the boundary, not the declarations.  The
# import is described by hand, naming only what the probe actually calls
# (D3D11CreateDevice; CreateDXGIFactory1 only in the separate REFUSAL
# build below), for the same reason check-com-smoke.sh's ole32.def does:
# the guest binds to the same builtins a real guest application would, and
# nothing else is linked in at all -- there is no CRT here, see the
# probe's header.
cat > "$OUT/kernel32.def" <<'EOF'
LIBRARY kernel32.dll
EXPORTS
GetStdHandle
WriteFile
ExitProcess
EOF
cat > "$OUT/d3d11.def" <<'EOF'
LIBRARY d3d11.dll
EXPORTS
D3D11CreateDevice
EOF
for m in kernel32 d3d11; do
    llvm-dlltool -m i386:x86-64 -d "$OUT/$m.def" -l "$OUT/lib$m.a" \
        || skip "llvm-dlltool failed for $m"
done

GUESTCC="clang -target x86_64-windows-gnu -nostdlibinc $INCL -Wall -O1 -fno-builtin -g"
GUESTLD="clang -target x86_64-windows-gnu -fuse-ld=lld -nostdlib \
-Wl,--entry=d3d11_smoke_entry -Wl,--subsystem,console"

$GUESTCC -c -o "$OUT/guest.o" "$HERE/probes/d3d11_smoke.c" || skip "guest compile failed"
$GUESTLD -o "$OUT/guest.exe" "$OUT/guest.o" "$OUT/libd3d11.a" "$OUT/libkernel32.a" \
    || skip "guest link failed"

WDBG=${WINEDEBUG:--all}
run_wine() { timeout -k 5 "$2" \
                 env WINEDEBUG="$WDBG" WINEDLLOVERRIDES="winedbg.exe=d" \
                 "$BUILD/wine" "$1"; }

# ---- (also available standalone as --sabotage): the negative controls -----
sabotage() {
    ok=1

    # part 1: WINEEMUNOCOMWRAP=1 hands the guest raw host pointers.
    timeout -k 5 "$TIMEOUT" \
        env WINEDEBUG="$WDBG" WINEDLLOVERRIDES="winedbg.exe=d" WINEEMUNOCOMWRAP=1 \
        "$BUILD/wine" "$OUT/guest.exe" \
        > "$OUT/sabotage_wrap.out" 2>"$OUT/sabotage_wrap.err"
    if grep -q "d3d11_smoke: PASS" "$OUT/sabotage_wrap.out"; then
        bad "WINEEMUNOCOMWRAP=1 still PASSED -- the gate cannot go red"; ok=0
    else
        say "sabotage: WINEEMUNOCOMWRAP=1 failed the guest run at '$(tail -1 \
            "$OUT/sabotage_wrap.out" | cut -c1-60)', as it must"
    fi

    # part 2: each SMOKE_BREAK variant, native, must FAIL.
    for n in 1 2 3; do
        if $NATIVECC -DD3D11_SMOKE_NATIVE -DSMOKE_BREAK=$n \
                -c -o "$OUT/native_break$n.o" "$HERE/probes/d3d11_smoke.c" \
                2>"$OUT/native_break$n.build.err" \
           && ${CC:-gcc} -o "$OUT/native_break$n" "$OUT/native_break$n.o" -ldl \
                2>>"$OUT/native_break$n.build.err"; then
            timeout -k 5 "$TIMEOUT" \
                env -u DISPLAY -u WAYLAND_DISPLAY -u XDG_RUNTIME_DIR \
                DXVK_WSI_DRIVER=Headless \
                "$OUT/native_break$n" "$D3D11_SO" \
                > "$OUT/native_break$n.out" 2>"$OUT/native_break$n.err"
            if grep -q "d3d11_smoke: PASS" "$OUT/native_break$n.out"; then
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

    # part 3: refusal hygiene's own negative control.  The refusal probe's
    # OpenSharedResource out-param is seeded with a sentinel; the normal leg F
    # requires osr_scrubbed=yes (a refused slot WRITES NULL before answering
    # -- the Witcher 3 GetShader lesson).  Here the scrub is disabled by its
    # lever, and the sentinel MUST survive: if osr_scrubbed stays "yes" with
    # the lever armed, the scrub the positive leg observed was not the thing
    # this lever controls, and neither leg proves anything.
    if [ -x "$OUT/refusal.exe" ] || [ -f "$OUT/refusal.exe" ]; then
        timeout -k 5 "$TIMEOUT" \
            env WINEDEBUG="$WDBG" WINEDLLOVERRIDES="winedbg.exe=d" \
            WINEEMUNOREFUSESCRUB=1 \
            "$BUILD/wine" "$OUT/refusal.exe" \
            > "$OUT/sabotage_scrub.out" 2>"$OUT/sabotage_scrub.err"
        if grep -q "osr_scrubbed=no" "$OUT/sabotage_scrub.out"; then
            say "sabotage: WINEEMUNOREFUSESCRUB=1 left the sentinel in the \
refused out-param, as it must (the scrub is load-bearing)"
        else
            bad "WINEEMUNOREFUSESCRUB=1 did not stop the refusal scrub \
(wanted osr_scrubbed=no): $(grep -o 'osr_scrubbed=[a-zA-Z]*' \
"$OUT/sabotage_scrub.out" | head -1)"; ok=0
        fi
    else
        note "sabotage: no refusal.exe from a prior full run; skipping the \
refusal-hygiene control (run the gate without --sabotage first)"
    fi

    [ "$ok" = 1 ] && say "SABOTAGE PASS"
    [ "$ok" = 1 ]
}

if [ "$SABOTAGE" = 1 ]; then
    sabotage
    exit $?
fi

# ---- C: native ---------------------------------------------------------
# Headless, exactly as dxvk-ppc64le/probes/native_d3d11_smoke.cpp's own run
# recipe requires: no DISPLAY, no WAYLAND_DISPLAY, no XDG_RUNTIME_DIR, and
# DXVK_WSI_DRIVER=Headless so no WSI toolkit is touched at all.  This probe
# never creates a swapchain (see its header), so headless is not a
# workaround here, it is simply correct.
timeout -k 5 "$TIMEOUT" \
    env -u DISPLAY -u WAYLAND_DISPLAY -u XDG_RUNTIME_DIR DXVK_WSI_DRIVER=Headless \
    "$OUT/native" "$D3D11_SO" > "$OUT/native.out" 2>"$OUT/native.err"
nst=$?
if [ $nst -eq 124 ] || [ $nst -eq 137 ]; then
    bad "the native run timed out after ${TIMEOUT}s"
elif grep -q "d3d11_smoke: PASS" "$OUT/native.out"; then
    say "native: $(tail -1 "$OUT/native.out")"
else
    sed 's/^/  native| /' "$OUT/native.out" >&2
    tail -20 "$OUT/native.err" >&2
    bad "the native ppc64le build did not pass"
fi

# ---- D: guest -----------------------------------------------------------
run_wine "$OUT/guest.exe" "$TIMEOUT" > "$OUT/guest.out" 2>"$OUT/guest.err"
gst=$?
if [ $gst -eq 124 ] || [ $gst -eq 137 ]; then
    bad "the guest run timed out after ${TIMEOUT}s"
elif grep -q "d3d11_smoke: PASS" "$OUT/guest.out"; then
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

# ---- F: refusal negative control -------------------------------------------
# A SEPARATE build (D3D11_SMOKE_REFUSAL) from the one just compared for
# identity above -- see the probe's header for why: this variant's
# transcript is not the one native and guest are diffed against, and must
# never be.  It drives one more call after everything else has already
# passed or failed on its own: a second D3D11CreateDevice, then
# ID3D11Device::OpenSharedResource with a fabricated HANDLE that names no
# shared resource anywhere.
#
# THIS LEG USED TO DRIVE IDXGIFactory::MakeWindowAssociation, on the
# reasoning that an HWND is a value the boundary cannot make sense of.  It
# now can -- this lane presents through win32u's client-surface layer and
# every window-handle slot on the surface marshals -- so that control would
# have quietly started PASSING, which is worse than not having one.  A
# by-value HANDLE never will: it names a Wine object on one side and DXVK's
# tagged-eventfd encoding on the other, two namespaces over one integer, and
# no amount of presentation work changes that.  The control moved to a
# refusal that is structural rather than provisional.
#
# Guarded: if the probe cannot be built, this leg is skipped with a clear
# note rather than failing the whole gate -- legs A-E already established
# the boundary this file exists to test; leg F is one additional negative
# control on top of that, not the load-bearing check.  It needs no import
# beyond d3d11.dll now, because the refused call is a method on a device
# D3D11CreateDevice hands back.
refusal_ok=1
if ! $GUESTCC -DD3D11_SMOKE_REFUSAL -c -o "$OUT/refusal.o" \
        "$HERE/probes/d3d11_smoke.c" 2>"$OUT/refusal.build.err" \
     || ! $GUESTLD -o "$OUT/refusal.exe" "$OUT/refusal.o" \
        "$OUT/libd3d11.a" "$OUT/libkernel32.a" \
        2>>"$OUT/refusal.build.err"; then
    refusal_ok=0
    note "leg F: the D3D11_SMOKE_REFUSAL build failed; skipping the \
refusal negative control (the probe was not built with that call). Last error:"
    tail -5 "$OUT/refusal.build.err" | sed 's/^/  refusal| /'
fi

if [ "$refusal_ok" = 1 ]; then
    timeout -k 5 "$TIMEOUT" \
        env WINEDEBUG=+winecom WINEDLLOVERRIDES="winedbg.exe=d" \
        "$BUILD/wine" "$OUT/refusal.exe" \
        > "$OUT/refusal.out" 2>"$OUT/refusal.err"
    rst=$?
    if [ $rst -eq 124 ] || [ $rst -eq 137 ]; then
        bad "leg F: the refusal probe HUNG (killed after ${TIMEOUT}s) instead \
of exiting cleanly"
        tail -10 "$OUT/refusal.err" | sed 's/^/  refusal| /' >&2
    elif grep -q " reached$" "$OUT/refusal.out"; then
        say "leg F: the refusal probe exited $rst and reached its own next \
line after the refused call"
    else
        sed 's/^/  refusal| /' "$OUT/refusal.out" >&2
        tail -10 "$OUT/refusal.err" | sed 's/^/  refusal| /' >&2
        bad "leg F: the refusal probe never reached its own line after the \
refused call; it crashed instead of being refused cleanly"
    fi
    if grep -qi "refusing" "$OUT/refusal.err" && \
       grep -q "OpenSharedResource" "$OUT/refusal.err"; then
        say "leg F: $(grep -im1 'refusing' "$OUT/refusal.err" | cut -c1-140)"
    else
        sed 's/^/  refusal| /' "$OUT/refusal.err" >&2
        bad "leg F: the port's +winecom trace does not name both 'refusing' \
and 'OpenSharedResource'; the refusal is either missing or unnamed"
    fi
    # refusal hygiene: refused means INERT.  The probe seeds the out-param
    # with a sentinel; the dispatcher must have scrubbed it to NULL before
    # answering E_NOTIMPL, or an unchecked caller reads stack residue -- the
    # Witcher 3 GetShader crash class.  --sabotage part 3 proves the other
    # direction with WINEEMUNOREFUSESCRUB=1.
    if grep -q "osr_scrubbed=yes" "$OUT/refusal.out"; then
        say "leg F: the refused out-param came back scrubbed (NULL)"
    else
        bad "leg F: the refused OpenSharedResource left its out-param \
unscrubbed: $(grep -o 'osr_scrubbed=[a-zA-Z]*' "$OUT/refusal.out" | head -1)"
    fi
fi

[ $fail -eq 0 ] && say "PASS"
exit $fail
