#!/bin/sh
#
# check-com-levers.sh -- the gate on the COM wave kill switches.
#
# WHY THIS GATE EXISTS.  The completeness landings (74591109c3f..c199f79caf9)
# newly SERVE hundreds of COM rows that used to refuse, and they shipped with
# no runtime way to put any of them back.  When the Witcher 3 stopped loading
# afterwards, bisecting that stretch cost seven seat runs of swapping built PE
# halves in and out of a tree -- see ppc64le/docs/sessions/2026-09-01/
# w3-load-regression-bisect.md, whose closing lesson is the reason for the
# three levers this file tests:
#
#   WINEEMUNOCOMROWS   force named `Iface::Slot` rows back to refusing
#   WINEEMUNOCOMIIDS   treat named IIDs as unrostered where interfaces are
#                      handed out (release, NULL, E_NOINTERFACE)
#   WINEEMUNOCOMWAVE   whole landings, by the names in
#                      libs/winecom/winecom_waves.h, whose membership
#                      ppc64le/winecom/derive-wave-rows.py derived from git
#
# A LEVER NOBODY CAN PROVE IS WORSE THAN NO LEVER: a bisect leg run under a
# lever that silently did nothing is recorded as "tested, clean", and the
# conclusion drawn from it is wrong in the most expensive direction.  So every
# arm here is a PAIR -- the lever armed and the lever absent -- and the two
# must print different things.
#
# The observable is ppc64le/winecom/probes/com_lever_smoke.c's `omget_out=`,
# which has exactly three values and each one names a different world:
#
#   rtv       ID3D11DeviceContext::OMGetRenderTargets SERVED and interning
#             handed the same proxy back
#   null      the row REFUSED and the refusal SCRUBBED the out-param --
#             refused means INERT, the Witcher 3 GetShader lesson
#   sentinel  the row REFUSED and did NOT scrub, which is correct ONLY under
#             WINEEMUNOREFUSESCRUB=1 and is the crash class everywhere else
#
# Legs:
#
#   A  BUILD: the guest probe compiles and links.
#   B  BASELINE: no lever -> omget_out=rtv, and the DXGI factory comes back.
#      (This is also the sabotage arm: if the row refuses with no lever set,
#      every positive arm below is measuring something else.)
#   C  ROWS: WINEEMUNOCOMROWS naming the row -> omget_out=null, and the
#      +winecom trace names the method AND the lever.
#   D  SCRUB: C plus WINEEMUNOREFUSESCRUB=1 -> omget_out=sentinel.  This is
#      what proves leg C's "null" was the SCRUB and not DXVK writing NULL.
#   E  @FILE: the same row named through `@path` -> omget_out=null.
#   F  TYPO: a row name that matches nothing -> omget_out=rtv AND a warning
#      naming it.  A typo in a bisect leg must never pass as "tested".
#   G  WAVE: WINEEMUNOCOMWAVE=getfamily -> omget_out=null (the row is in that
#      wave); an unknown wave name -> omget_out=rtv AND a warning.
#   H  IIDS: WINEEMUNOCOMIIDS naming IID_IDXGIFactory1, in both the full and
#      the bare 8-hex-digit spellings -> factory_hr=0x80004002 with the out
#      pointer NULLED.  Absent the lever, leg B already showed it serving.
#   I  DERIVATION: derive-wave-rows.py --check -- the checked-in wave list and
#      the generated header still match what git says.
#
# --sabotage runs the negative controls alone (legs B, F, and G's unknown-wave
# half) and requires each to show the lever NOT firing.  A gate that cannot go
# red proves nothing.
#
# Exit 0 = pass, 1 = a check failed, 2 = could not run at all (a skip is NOT a
# pass).
#
# WINEDLLOVERRIDES=winedbg.exe=d on every run, verbatim from
# check-d3d11-smoke.sh and for the identical reason: the bringup prefix has
# AeDebug pointed at "winedbg --auto", so a run that ends in an unhandled
# fault -- what a defect in this boundary looks like from outside -- would
# start the debugger, which attaches and never lets go, turning every red
# state of this gate into a hang.
set -u

HERE=$(cd "$(dirname "$0")" && pwd)
SRC=$(cd "$HERE/../.." && pwd)
BUILD=${BUILD:-$SRC}
OUT=${OUT:-/tmp/com-levers}
TIMEOUT=${TIMEOUT:-120}
SABOTAGE=0
[ "${1:-}" = "--sabotage" ] && SABOTAGE=1

say()  { echo "check-com-levers: $*"; }
bad()  { echo "check-com-levers: FAIL $*" >&2; fail=1; }
note() { echo "check-com-levers: note $*"; }
skip() { echo "check-com-levers: $*" >&2; exit 2; }

[ -x "$BUILD/wine" ] || skip "no wine loader at $BUILD/wine"
[ -n "${WINEPREFIX:-}" ] || skip "set WINEPREFIX to a prefix wineboot has run in"
[ -n "${WINEFEXBRIDGE:-}" ] || skip "set WINEFEXBRIDGE to the emulator bridge"
[ -f "$BUILD/dlls/d3d11/x86_64-windows/d3d11.dll" ] || \
    skip "no guest d3d11 thunk; build it first"
command -v clang >/dev/null || skip "need clang for the guest build"
command -v llvm-dlltool >/dev/null || skip "need llvm-dlltool for the guest build"

mkdir -p "$OUT" || skip "cannot create $OUT"
# DXVK drops <appname>_d3d11.log next to the CURRENT directory otherwise, and a
# gate is normally run from the top of the checkout.
DXVK_LOG_PATH=$OUT
export DXVK_LOG_PATH
fail=0

INCL="-I$BUILD/include -I$SRC/include -I$SRC/include/msvcrt"

# ---- A: build the guest probe ----------------------------------------------
# Same clang x86_64-windows-gnu machinery, the same hand-written .def imports
# and the same "no CRT" rule as ppc64le/dxvk/check-d3d11-smoke.sh's guest leg:
# the probe binds to the builtins a real guest application binds to, and
# nothing else is linked in at all.
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
cat > "$OUT/dxgi.def" <<'EOF'
LIBRARY dxgi.dll
EXPORTS
CreateDXGIFactory1
EOF
for m in kernel32 d3d11 dxgi; do
    llvm-dlltool -m i386:x86-64 -d "$OUT/$m.def" -l "$OUT/lib$m.a" \
        || skip "llvm-dlltool failed for $m"
done

clang -target x86_64-windows-gnu -nostdlibinc $INCL -Wall -O1 -fno-builtin -g \
    -c -o "$OUT/probe.o" "$HERE/probes/com_lever_smoke.c" \
    2>"$OUT/build.err" || { sed 's/^/  build| /' "$OUT/build.err" >&2;
                            skip "the guest probe did not compile"; }
clang -target x86_64-windows-gnu -fuse-ld=lld -nostdlib \
    -Wl,--entry=com_lever_entry -Wl,--subsystem,console \
    -o "$OUT/probe.exe" "$OUT/probe.o" \
    "$OUT/libd3d11.a" "$OUT/libdxgi.a" "$OUT/libkernel32.a" \
    2>>"$OUT/build.err" || { sed 's/^/  build| /' "$OUT/build.err" >&2;
                             skip "the guest probe did not link"; }
say "build: $OUT/probe.exe"

# run <tag> [VAR=VAL ...] -- always +winecom, because the refusal lines the
# arms assert on are the PORT's own trace, not the probe's output.
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
    grep -q "com_lever_smoke: done" "$OUT/$tag.out" && return 0
    sed 's/^/  '"$tag"'| /' "$OUT/$tag.out" >&2
    tail -15 "$OUT/$tag.err" | sed 's/^/  '"$tag"'| /' >&2
    bad "the $tag run never reached its own last line"
    return 1
}

field() { grep -o "$2=[a-zA-Z0-9]*" "$OUT/$1.out" | head -1 | cut -d= -f2; }

# want <tag> <field> <expected> <why>
want() {
    got=$(field "$1" "$2")
    if [ "$got" = "$3" ]; then
        say "$1: $2=$3 -- $4"
    else
        bad "$1: wanted $2=$3, got '${got:-<absent>}' -- $4"
    fi
}

ROW=ID3D11DeviceContext::OMGetRenderTargets
# IID_IDXGIFactory1, from include/dxgi1_2.idl; the probe compiles the same
# bytes into its own literal, so the two cannot drift apart silently.
FACTORY_IID='{770aae78-f26f-4dba-a829-253c83d1b387}'

# ---- the negative controls, also available standalone as --sabotage --------
sabotage() {
    ok=1

    # B: with NO lever the row must SERVE.  If it refuses here, every positive
    # arm below is measuring something other than the lever.
    if run baseline; then
        if [ "$(field baseline omget_out)" = "rtv" ]; then
            say "sabotage: with no lever the row SERVES (omget_out=rtv), so the \
armed arms below can mean something"
        else
            bad "with NO lever set the row did not serve (omget_out=\
$(field baseline omget_out)); the lever arms prove nothing"; ok=0
        fi
    else ok=0; fi

    # F: a row name that matches nothing must force NOTHING and must SAY SO.
    if run typo WINEEMUNOCOMROWS="ID3D11DeviceContext::NoSuchSlotHere"; then
        if [ "$(field typo omget_out)" != "rtv" ]; then
            bad "a row name matching no row still changed behaviour \
(omget_out=$(field typo omget_out))"; ok=0
        elif ! grep -q "NoSuchSlotHere" "$OUT/typo.err"; then
            sed 's/^/  typo| /' "$OUT/typo.err" >&2
            bad "a row name matching no row passed SILENTLY; a typo in a bisect \
leg would be recorded as 'tested, clean'"; ok=0
        else
            say "sabotage: $(grep -m1 'NoSuchSlotHere' "$OUT/typo.err" | cut -c1-140)"
        fi
    else ok=0; fi

    # G(b): an unknown wave name, same rule.
    if run nowave WINEEMUNOCOMWAVE=nosuchwave; then
        if [ "$(field nowave omget_out)" != "rtv" ]; then
            bad "an unknown wave name still forced something \
(omget_out=$(field nowave omget_out))"; ok=0
        elif ! grep -q "nosuchwave" "$OUT/nowave.err"; then
            bad "an unknown wave name passed silently"; ok=0
        else
            say "sabotage: $(grep -m1 'nosuchwave' "$OUT/nowave.err" | cut -c1-140)"
        fi
    else ok=0; fi

    [ "$ok" = 1 ] && say "SABOTAGE PASS"
    [ "$ok" = 1 ]
}

if [ "$SABOTAGE" = 1 ]; then
    sabotage
    exit $?
fi

# ---- B: baseline ------------------------------------------------------------
if run baseline; then
    want baseline omget_out rtv "the row serves and interning hands the same proxy back"
    want baseline factory_out object "a riid-typed handout serves"
    want baseline factory_hr 0x00000000 "CreateDXGIFactory1 succeeded"
fi

# ---- C: the row lever ------------------------------------------------------
if run rows WINEEMUNOCOMROWS="$ROW"; then
    want rows omget_out null "the row refused AND the refusal scrubbed the out-param"
    if grep -qi "refusing" "$OUT/rows.err" && \
       grep -q "OMGetRenderTargets" "$OUT/rows.err" && \
       grep -q "WINEEMUNOCOMROWS" "$OUT/rows.err"; then
        say "rows: $(grep -im1 'refusing' "$OUT/rows.err" | cut -c1-140)"
    else
        sed 's/^/  rows| /' "$OUT/rows.err" >&2
        bad "the +winecom trace does not name 'refusing', the method AND the \
lever; the refusal is either missing or unattributed"
    fi
fi

# ---- D: and the scrub inside it -------------------------------------------
# Proves leg C's "null" was the SCRUB and not DXVK writing NULL: same lever,
# scrub disabled, and the sentinel must SURVIVE.
if run rows_noscrub WINEEMUNOCOMROWS="$ROW" WINEEMUNOREFUSESCRUB=1; then
    want rows_noscrub omget_out sentinel \
        "the forced refusal is what wrote NULL in leg C, not the callee"
fi

# ---- E: the @file spelling -------------------------------------------------
printf '# a bisect leg, one name per line\n%s\n' "$ROW" > "$OUT/rows.list"
if run rows_file WINEEMUNOCOMROWS="@$OUT/rows.list"; then
    want rows_file omget_out null "the @file spelling resolves the same row"
fi

# ---- F: the typo warning ---------------------------------------------------
if run typo WINEEMUNOCOMROWS="ID3D11DeviceContext::NoSuchSlotHere"; then
    want typo omget_out rtv "a name that matches nothing forces nothing"
    if grep -q "NoSuchSlotHere" "$OUT/typo.err"; then
        say "typo: $(grep -m1 'NoSuchSlotHere' "$OUT/typo.err" | cut -c1-140)"
    else
        bad "a row name matching no row passed SILENTLY"
    fi
fi

# ---- G: the wave aliases ---------------------------------------------------
if run wave WINEEMUNOCOMWAVE=getfamily; then
    want wave omget_out null "the getfamily wave contains this row"
    if grep -q "getfamily" "$OUT/wave.err"; then
        say "wave: $(grep -m1 'getfamily' "$OUT/wave.err" | cut -c1-140)"
    else
        bad "WINEEMUNOCOMWAVE=getfamily armed without saying so"
    fi
fi
if run nowave WINEEMUNOCOMWAVE=nosuchwave; then
    want nowave omget_out rtv "an unknown wave forces nothing"
    grep -q "nosuchwave" "$OUT/nowave.err" || \
        bad "an unknown wave name passed silently"
fi

# ---- H: the IID lever ------------------------------------------------------
# 0x80004002 is E_NOINTERFACE.  Both accepted spellings, because the bare form
# is what the port's own notes use ("{77aa99a0} IAudioSessionManager2").
if run iid_full WINEEMUNOCOMIIDS="$FACTORY_IID"; then
    want iid_full factory_hr 0x80004002 "a blocked IID answers E_NOINTERFACE"
    want iid_full factory_out null "and the out pointer is NULLED, not left as residue"
    want iid_full omget_out rtv "and nothing else on the surface changed"
fi
if run iid_short WINEEMUNOCOMIIDS=770aae78; then
    want iid_short factory_hr 0x80004002 "the bare 8-hex-digit spelling works too"
    want iid_short factory_out null "and NULLs the out pointer the same way"
fi

# ---- I: the derivation is still what git says ------------------------------
if command -v python3 >/dev/null && [ -d "$SRC/.git" ]; then
    if python3 "$HERE/derive-wave-rows.py" --check 2>"$OUT/derive.err"; then
        say "derivation: wave-rows.list and winecom_waves.h still match git"
    else
        sed 's/^/  derive| /' "$OUT/derive.err" >&2
        bad "the checked-in wave membership drifted from what \
derive-wave-rows.py derives; re-run it"
    fi
else
    note "no python3 or no git checkout here; skipping the derivation check"
fi

[ $fail -eq 0 ] && say "PASS"
exit $fail
