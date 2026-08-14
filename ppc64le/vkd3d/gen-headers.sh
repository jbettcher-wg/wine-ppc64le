#!/usr/bin/env bash
# Generate vkd3d-proton's C/C++ headers from its .idl sources.
#
# WHY THIS EXISTS. vkd3d-proton ships interfaces as `.idl` and runs widl at
# build time (`src/meson.build:79`), so a checkout contains no `d3d12.h`. The
# thunk generator (`dxvk-ppc64le/thunk/gen_interfaces.py`) derives vtable slots
# from `MIDL_INTERFACE` declarations, and the slots it must agree with are
# *these* — the host it dispatches into is vkd3d-proton, and vkd3d-proton's
# vtables are built from these idl files, not from anyone's copy of d3d12.h.
#
# That distinction is not academic. DXVK vendors a MinGW-w64 `d3d12.h`
# (`dxvk-ppc64le/src/include/native/directx/d3d12.h`) which is an old snapshot:
# it stops at ID3D12Device1 and ID3D12GraphicsCommandList2 and contains 21
# D3D12 interfaces with no DXR, no mesh shaders and no ID3D12Device5. vkd3d's
# own idl declares 71. Generating a D3D12 thunk from the DXVK copy would
# produce a surface that cannot express most of what vkd3d-proton implements,
# and — worse — would be a slot table derived from a different header than the
# one the callee was compiled from.
#
# The DXGI half is the opposite way round: vkd3d-proton declares only the 13
# DXGI interfaces it consumes, because on Linux it does not implement DXGI at
# all. DXVK's DXGI is the real one, and the D3D11 thunk already generates it.
# So the D3D12 surface is vkd3d's D3D12 headers PLUS DXVK's DXGI headers, which
# is why gen_interfaces.py takes a header search path rather than one directory.
#
#   ./gen-headers.sh            # generate into generated-headers/
#   WIDL=/path/to/widl ./gen-headers.sh
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
SRC="$HERE/src/include"
OUT="$HERE/generated-headers"

[ -d "$SRC" ] || {
    echo "gen-headers: $SRC absent; run ./bootstrap.sh first" >&2; exit 1; }

# widl, in order of preference. There is no widl package installed on either of
# this project's machines, so the fallback is Wine's own build of it — widl is
# a host tool built by any Wine tree, and the hangover port has one. Named
# explicitly rather than searched for, because a widl found at random is a widl
# whose version nobody recorded.
find_widl() {
    if [ -n "${WIDL:-}" ]; then printf '%s' "$WIDL"; return; fi
    for c in widl widl-stable; do
        if command -v "$c" >/dev/null 2>&1; then command -v "$c"; return; fi
    done
    local w="$HERE/../hangover-ppc64le/wine-build/tools/widl/widl"
    if [ -x "$w" ]; then printf '%s' "$w"; return; fi
    return 1
}

widl="$(find_widl)" || {
    cat >&2 <<'EOF'
gen-headers: no widl found.
  Tried $WIDL, `widl`, `widl-stable`, and ../hangover-ppc64le/wine-build/tools/widl/widl.
  Wine builds widl as a host tool; point $WIDL at one rather than installing a
  package, so the version in use is the one this tree already has.
EOF
    exit 1
}
echo "  widl: $widl"

# Copied out of the Wine tree before use: that tree belongs to another workflow
# and may be rebuilt underneath a long run.
tmp="$(mktemp -d)"
trap 'rm -rf "$tmp"' EXIT
cp "$widl" "$tmp/widl"

mkdir -p "$OUT"
n=0 skipped=0
for f in "$SRC"/*.idl; do
    b="$(basename "$f" .idl)"
    # vkd3d_unknown.idl is not standalone (it uses IID before vkd3d_windows.h
    # has defined it) and is only ever #included; nothing declares an interface
    # in it that the others do not re-declare.
    if ! (cd "$SRC" && "$tmp/widl" -h -o "$OUT/$b.h" "$b.idl") 2>"$tmp/err"; then
        echo "  skip $b.idl: $(head -1 "$tmp/err")"
        rm -f "$OUT/$b.h"
        skipped=$((skipped + 1))
        continue
    fi
    n=$((n + 1))
done

echo "gen-headers: wrote $n header(s) to generated-headers/ ($skipped skipped)"
grep -c MIDL_INTERFACE "$OUT"/vkd3d_d3d12.h | sed 's/^/  vkd3d_d3d12.h interfaces: /'
