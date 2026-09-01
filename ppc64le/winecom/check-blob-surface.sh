#!/bin/sh
#
# check-blob-surface.sh -- the d3dcompiler blob surface, end to end.
#
# WHY THIS GATE EXISTS.  dlls/d3dcompiler_47 was FROM-SPEC auto with no
# COM-JSON from 2026-08-15 to 2026-09-01, so D3DCompile's `ID3DBlob **`
# out-cells crossed as plain scalars: the guest received a NATIVE ppc64 blob
# pointer and its first vtable call executed ppc64 bytes as x86-64.  The
# Witcher 3 compiles shaders while loading a save and crashed there on every
# Continue; DOOM and Cyberpunk ship precompiled DXBC, which is why no earlier
# title found it.  The fix is the d3dcompiler winecom surface (guestcom.c,
# interfaces_d3dcompiler.json) and spec2thunk's flat audit now FAILS the build
# on an unclassified interface-bearing export in this module.  This gate is
# the RUNTIME half of that guarantee.
#
# Legs:
#   A  BUILD: the guest probe compiles and links.
#   B  BASELINE: D3DCompile succeeds, the code blob's bytes read "DXBC"
#      through the proxy's own vtable (GetBufferPointer + GetBufferSize, the
#      exact calls that used to execute ppc64 bytes), a broken compile hands
#      back a WRAPPED error blob (the wrap must not be gated on success), and
#      every blob Releases without a fault.
#   C  PROVENANCE (the sabotage arm): WINEEMUNOCOMROWS forcing
#      ID3D10Blob::GetBufferSize proves the calls dispatch through THIS
#      surface's marshal -- the forced refusal answers E_NOTIMPL (0x80004001)
#      as the size, deterministically, and the armed line names the row.  A
#      raw host blob would never consult winecom, so this arm CANNOT pass on
#      the pre-fix behavior; it also re-proves the lever reaches a brand-new
#      surface.
#
# Exit 0 = pass, 1 = a check failed, 2 = could not run (a skip is NOT a pass).
set -u

HERE=$(cd "$(dirname "$0")" && pwd)
SRC=$(cd "$HERE/../.." && pwd)
BUILD=${BUILD:-$SRC}
OUT=${OUT:-/tmp/blob-surface}
TIMEOUT=${TIMEOUT:-120}

say()  { echo "check-blob-surface: $*"; }
bad()  { echo "check-blob-surface: FAIL $*" >&2; fail=1; }
skip() { echo "check-blob-surface: $*" >&2; exit 2; }

[ -x "$BUILD/wine" ] || skip "no wine loader at $BUILD/wine"
[ -n "${WINEPREFIX:-}" ] || skip "set WINEPREFIX to a prefix wineboot has run in"
[ -n "${WINEFEXBRIDGE:-}" ] || skip "set WINEFEXBRIDGE to the emulator bridge"
[ -f "$BUILD/dlls/d3dcompiler_47/x86_64-windows/d3dcompiler_47.dll" ] || \
    skip "no guest d3dcompiler_47 thunk; build it first"
command -v clang >/dev/null || skip "need clang for the guest build"
command -v llvm-dlltool >/dev/null || skip "need llvm-dlltool for the guest build"

mkdir -p "$OUT" || skip "cannot create $OUT"
fail=0

INCL=""   # the probe is freestanding; no headers wanted

# ---- A: build the guest probe ----------------------------------------------
cat > "$OUT/kernel32.def" <<'EOF'
LIBRARY kernel32.dll
EXPORTS
GetStdHandle
WriteFile
ExitProcess
EOF
cat > "$OUT/d3dcompiler_47.def" <<'EOF'
LIBRARY d3dcompiler_47.dll
EXPORTS
D3DCompile
EOF
for m in kernel32 d3dcompiler_47; do
    llvm-dlltool -m i386:x86-64 -d "$OUT/$m.def" -l "$OUT/lib$m.a" \
        || skip "llvm-dlltool failed for $m"
done

clang -target x86_64-windows-gnu -nostdlibinc -Wall -O1 -fno-builtin -g \
    -c -o "$OUT/probe.o" "$HERE/probes/blob_surface_smoke.c" \
    2>"$OUT/build.err" || { sed 's/^/  build| /' "$OUT/build.err" >&2;
                            skip "the guest probe did not compile"; }
clang -target x86_64-windows-gnu -fuse-ld=lld -nostdlib \
    -Wl,--entry=com_lever_entry -Wl,--subsystem,console \
    -o "$OUT/probe.exe" "$OUT/probe.o" \
    "$OUT/libd3dcompiler_47.a" "$OUT/libkernel32.a" \
    2>>"$OUT/build.err" || { sed 's/^/  build| /' "$OUT/build.err" >&2;
                             skip "the guest probe did not link"; }
say "build: $OUT/probe.exe"

run() {
    tag=$1; shift
    timeout -k 5 "$TIMEOUT" \
        env WINEDEBUG=+winecom WINEDLLOVERRIDES="winedbg.exe=d" "$@" \
        "$BUILD/wine" "$OUT/probe.exe" \
        > "$OUT/$tag.out" 2>"$OUT/$tag.err"
    rc=$?
    if [ $rc -eq 124 ] || [ $rc -eq 137 ]; then
        bad "the $tag run HUNG (killed after ${TIMEOUT}s)"
        return 1
    fi
    grep -q "blob_surface_smoke: done" "$OUT/$tag.out" && return 0
    sed 's/^/  '"$tag"'| /' "$OUT/$tag.out" >&2
    tail -15 "$OUT/$tag.err" | sed 's/^/  '"$tag"'| /' >&2
    bad "the $tag run never reached its own last line (the pre-fix behavior \
is a FAULT exactly here -- the guest executing a native blob vtable)"
    return 1
}

field() { grep -o "$2=[a-zA-Z0-9]*" "$OUT/$1.out" | head -1 | cut -d= -f2; }

want() {
    got=$(field "$1" "$2")
    if [ "$got" = "$3" ]; then say "$1: $2=$got -- $4"
    else bad "$1: $2=$got, wanted $3 ($4)"; fi
}

# ---- B: baseline ------------------------------------------------------------
if run baseline; then
    want baseline compile_hr 0x00000000 "D3DCompile succeeded through the wrapper"
    want baseline code_magic DXBC "the blob's bytes read back through the PROXY vtable"
    want baseline err_hr 0x80004005 "the broken compile failed as it must"
    want baseline err_blob text "and its ERROR blob came back wrapped and readable"
    # 2, not 3: the clean compile writes NULL to its error cell (no warnings
    # from a one-liner), so only the code blob and leg 2's error blob exist.
    want baseline released 2 "every blob Released without a fault"
fi

# ---- C: provenance / sabotage ----------------------------------------------
if run forced WINEEMUNOCOMROWS="ID3D10Blob::GetBufferSize"; then
    want forced code_magic DXBC "GetBufferPointer still serves"
    want forced code_size 2147500033 \
        "the forced refusal's E_NOTIMPL came back as the size -- the call \
dispatches through THIS surface's marshal, which a raw host blob never would"
    if grep -q "d3dcompiler: WINEEMUNOCOMROWS/WAVE armed" "$OUT/forced.err"; then
        say "forced: $(grep -m1 'armed' "$OUT/forced.err" | cut -c1-140)"
    else
        bad "the lever never armed on the d3dcompiler surface"
    fi
fi

[ $fail -eq 0 ] && say "PASS"
exit $fail
