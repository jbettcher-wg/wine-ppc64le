#!/bin/sh
#
# probe-dllload.sh -- build and run ppc64le/games/dllload_probe.c against one
#                     guest DLL, headless, without taking the game lock.
#
#   probe-dllload.sh [--dir GAMEDIR] [--proc EXPORT] <module>
#
# NOT a check-*.sh, deliberately: nothing here raises a dialog, nothing needs
# the display, and it is a diagnostic rather than a gate.  It runs a guest
# x86-64 process that does one LoadLibrary and exits, which is enough to
# reproduce -- and to prove fixed -- the whole class of "the title dies in
# loader_init because a bundled DLL bound a 0xDEAD.... sentinel at
# PROCESS_ATTACH".  That is a two-minute launch otherwise, and a launch needs
# the foreground on a box where several agents are queueing for it.
#
# It uses the compat tool's own prefix and environment (via ./run-native), so
# the module loads against exactly the guest thunk surface a game would see.
# The prefix is named after the title, so per-title staging (sysx8664) and
# registry state are the ones under test.
#
# WINEDEBUG defaults to warn+module here, because the whole point is to see
# the "No implementation for <dll>.<symbol> ... setting to 00000000DEAD00nn"
# lines that name each sentinel.  Override it if you want something else.
set -u

here=$(cd "$(dirname "$0")" && pwd -P)
src=$here/../..                       # the wine source tree
tool=$here/../steamtool
build=${WINE_PPC64LE_TREE:-$src/../wine-build}

dir=
proc=
name=probe

while [ $# -gt 0 ]; do
    case $1 in
    --dir)  dir=${2:?--dir needs a directory}; shift 2 ;;
    --proc) proc=${2:?--proc needs an export name}; shift 2 ;;
    --name) name=${2:?--name needs a value}; shift 2 ;;
    --) shift; break ;;
    -*) echo "probe-dllload.sh: unknown option $1" >&2; exit 2 ;;
    *) break ;;
    esac
done

module=${1:-}
[ -n "$module" ] || { echo "usage: probe-dllload.sh [--dir GAMEDIR] [--proc EXPORT] [--name PREFIX] <module>" >&2; exit 2; }

out=${PROBE_OUT:-$build/ppc64le-probes}
mkdir -p "$out" || { echo "probe-dllload.sh: cannot create $out" >&2; exit 1; }

INCL="-I$build/include -I$src/include -I$src/include/msvcrt"
clang -target x86_64-windows-gnu -nostdlibinc $INCL -D_MSVCR_VER=0 \
    -Wall -O1 -fno-builtin -g -c -o "$out/dllload_probe.o" "$here/dllload_probe.c" \
    || { echo "probe-dllload.sh: guest probe compile failed" >&2; exit 1; }
clang -target x86_64-windows-gnu -fuse-ld=lld -nostdlib \
    -Wl,--entry=dllload_probe_entry -Wl,--subsystem,console \
    -o "$out/dllload_probe.exe" "$out/dllload_probe.o" \
    "$build/dlls/kernel32/x86_64-windows/kernel32.dll" \
    || { echo "probe-dllload.sh: guest probe link failed" >&2; exit 1; }

DLLLOAD_PROBE_FILE=$module
export DLLLOAD_PROBE_FILE
[ -n "$dir" ]  && { DLLLOAD_PROBE_DIR=$dir;   export DLLLOAD_PROBE_DIR; }
[ -n "$proc" ] && { DLLLOAD_PROBE_PROC=$proc; export DLLLOAD_PROBE_PROC; }
WINEDEBUG=${WINEDEBUG:-warn+module}
export WINEDEBUG

echo "probe-dllload.sh: $out/dllload_probe.exe -> $module"
exec "$tool/run-native" --name "$name" "$out/dllload_probe.exe"
