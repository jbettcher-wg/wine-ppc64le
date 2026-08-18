#!/bin/sh
#
# check-wow64-smoke.sh -- the gate for the 32-bit (i386) guest lane:
# real Wine WoW64 with the embedded emulator as the CPU backend
# (ppc64le/wow64/DESIGN.md; dlls/ntdll/wow64cpu_ppc64.c and the emu32
# unixcalls in dlls/ntdll/unix/loader.c own the mechanism).
#
# Layers, in ascending order of what they prove:
#
#   1  BUILD: the i386 guest PE compiles and imports exactly the functions
#      wow_smoke.c claims from kernel32 -- the layer that catches "linked the
#      wrong import library" passing everything below for the wrong reason.
#   2  EXIT CODE: the same PE run with an "exit" argument terminates with
#      ExitProcess(123) and the loader hands 123 back.  The cheapest whole
#      value the lane can move: server accepts the PE32, wow64.dll comes up,
#      the CPU backend runs guest code to a syscall, and the status crosses
#      back out of the process.
#   3  GUEST + TRANSCRIPT: the full probe prints PASS 8/8 and its stdout is
#      byte-identical to the transcript embedded below.  Every step checks a
#      value or a relation -- the TIB through FS, the PEB against the loader,
#      GetProcAddress against the import, a 64-bit OUT parameter, an SEH
#      round trip through the 32-bit dispatcher, a second guest thread with
#      a distinct TIB.  See wow_smoke.c's header for what each step pins.
#   4  THE CANONICAL LADDER RUNG: Wine's own winepath.exe, built as an i386
#      PE by this tree's own build, prints byte-identical output to the
#      native ppc64 winepath for the same query.  This is the same proof the
#      AMD64 lane's README leads with, one machine word narrower.
#   5  THE PORT'S OWN VIEW: a WINEDEBUG=trace+module run of the exit-code leg
#      must show unixcall_emu32_init's "32-bit lane up" TRACE exactly once --
#      the 32-bit emulator lane started, once, and nothing started it twice.
#
# --sabotage (also the standalone flag) runs BOTH of this lane's negative
# controls, one per mechanism, exactly as check-guest-callbacks.sh does:
#
#   WINEEMUNOWOW32=1 (dlls/ntdll/unix/loader.c, unixcall_emu32_init) refuses
#   to start the 32-bit emulator lane at all.  The run MUST fail: prompt,
#   nonzero, never printing PASS, and the port must SAY the lever's name in
#   its diagnostics.  This is the whole-lane lever: with it on, an i386 image
#   is accepted by the server and wow64.dll loads, but the CPU backend's
#   process init fails and the process dies before one guest instruction.
#
#   WINEEMUNOFSBASE32=1 (same file, unixcall_emu32_thread) starts the lane
#   but leaves every thread's FS base unset -- the TIB-reachability defect,
#   switched on deliberately.  The guest's very first fs: access (the 32-bit
#   ntdll touches its TEB long before wow_smoke's step 1) dereferences its
#   TIB offsets against base 0.  The run must fail promptly without PASS,
#   and the port must name the lever.
#
# WHY EVERY RUN DISABLES winedbg, verbatim from check-guest-callbacks.sh: the
# bringup prefix has AeDebug configured with "winedbg --auto", so an unhandled
# guest fault -- which both sabotage legs deliberately produce -- would start
# the debugger, which attaches and never lets go, turning every red state this
# gate is SUPPOSED to reach into a hang instead of a result.
#
# Exit 0 = pass, 1 = a check failed, 2 = could not run (not a pass).
set -u

HERE=$(cd "$(dirname "$0")" && pwd)
SRC=$(cd "$HERE/../.." && pwd)
BUILD=${BUILD:-$SRC}
OUT=${OUT:-/tmp/wow64-smoke}
SABOTAGE=0
[ "${1:-}" = "--sabotage" ] && SABOTAGE=1

say()  { echo "check-wow64-smoke: $*"; }
bad()  { echo "check-wow64-smoke: FAIL $*" >&2; fail=1; }
skip() { echo "check-wow64-smoke: $*" >&2; exit 2; }

[ -x "$BUILD/wine" ] || skip "no wine loader at $BUILD/wine"
[ -n "${WINEPREFIX:-}" ] || skip "set WINEPREFIX to a prefix wineboot has run in"
[ -n "${WINEFEXBRIDGE:-}" ] || skip "set WINEFEXBRIDGE to the emulator bridge"
[ -f "$BUILD/dlls/ntdll/i386-windows/ntdll.dll" ] || \
    skip "no i386 ntdll; build with --enable-archs=ppc64,i386 first"
[ -f "$BUILD/programs/winepath/i386-windows/winepath.exe" ] || \
    skip "no i386 winepath.exe; build with --enable-archs=ppc64,i386 first"
# THE BRIDGE IS CHECKED BEFORE THE PREFIX, because getting this order wrong
# costs an afternoon.  A bridge without 32-bit support does not fail loudly at
# wineboot time -- wineboot's own Wow64Install pass starts
# syswow64\rundll32.exe as an i386 process, that process dies in
# BTCpuProcessInit with c0000139, and wineboot carries on and reports success.
# What you are left with is an EMPTY syswow64 and a gate that says "run
# wineboot -u", which is a symptom pointing at the wrong thing: running it
# again produces exactly the same empty directory.
#
# [MEASURED] 2026-08-18, op4k: the bridge built beside the binfmt-registered
# FEX was ABI 3 and had no fexbridge_process_init32, so four Wow64Install
# passes ran and staged nothing.  Rebuilt at ABI 4, one wineboot -u staged 890
# entries and this gate went green first try.
if command -v nm >/dev/null && [ -f "$WINEFEXBRIDGE" ]; then
    nm -D --defined-only "$WINEFEXBRIDGE" 2>/dev/null |
        grep -q fexbridge_process_init32 || \
        skip "the bridge at $WINEFEXBRIDGE has no fexbridge_process_init32, so \
it cannot run a 32-bit guest (needs ABI 4).  Rebuild it -- 'ninja fexbridge' in \
the FEX build directory -- and boot the prefix again; nothing downstream of this \
will work until that symbol is there"
fi

# The prefix must have been updated since the server learned I386: syswow64
# and the Wow6432Node only appear on a wineboot -u against the new server, AND
# that wineboot must have had a 32-bit-capable bridge (see above).
[ -d "$WINEPREFIX/drive_c/windows/syswow64" ] || \
    skip "prefix has no syswow64; run 'wineboot -u' against this build first"
[ -n "$(ls -A "$WINEPREFIX/drive_c/windows/syswow64" 2>/dev/null)" ] || \
    skip "prefix has an EMPTY syswow64, which means wineboot's Wow64Install \
pass ran and staged nothing -- almost always a bridge that cannot start a \
32-bit process.  Check the bridge first, then re-run 'wineboot -u'"
command -v clang >/dev/null || skip "need clang for the guest build"
command -v llvm-readobj >/dev/null || skip "need llvm-readobj to read the built image"

mkdir -p "$OUT" || skip "cannot create $OUT"
fail=0
TIMEOUT=${TIMEOUT:-120}

# ---- build: the i386 guest PE --------------------------------------------
# Linked against the BUILD TREE'S OWN winebuild import library rather than an
# llvm-dlltool one: i386 stdcall decoration (_Name@N in the linker, plain Name
# in the import table) is exactly the kind of detail a hand-made .def gets
# subtly wrong, and the tree's import library IS the ABI this probe is meant
# to exercise.
INCL="-I$BUILD/include -I$SRC/include -I$SRC/include/msvcrt"
clang -target i686-windows-gnu -nostdlibinc $INCL -fms-extensions -D_UCRT \
    -Wall -O1 -fno-builtin -g -c -o "$OUT/wow_smoke.o" "$HERE/wow_smoke.c" \
    || skip "guest compile failed"
clang -target i686-windows-gnu -fuse-ld=lld -nostdlib \
    -Wl,--entry=_wow_smoke_entry -Wl,--subsystem,console \
    -o "$OUT/wow_smoke.exe" "$OUT/wow_smoke.o" \
    "$BUILD/dlls/kernel32/i386-windows/libkernel32.a" \
    || skip "guest link failed"
EXE="$OUT/wow_smoke.exe"

# ---- 1: build/shape ------------------------------------------------------
llvm-readobj --coff-imports "$EXE" > "$OUT/imports.txt" 2>&1
imported_from() {
    awk -v s="$1" '/Name: .*\.dll/ { dll = $2 }
                   $0 ~ ("Symbol: .*" s " ") { print dll }' "$OUT/imports.txt"
}
for sym in GetStdHandle WriteFile ExitProcess GetModuleHandleW VirtualAlloc \
           GetProcAddress QueryPerformanceFrequency CreateThread
do
    if imported_from "$sym" | grep -qx "kernel32.dll"; then
        say "image: $sym is imported from kernel32.dll"
    else
        bad "the guest exe does not import $sym from kernel32.dll (seen: \
$(imported_from "$sym" | tr '\n' ' '))"
    fi
done
MACHINE=$(llvm-readobj --file-headers "$EXE" | sed -n 's/.*Machine: \(IMAGE_FILE_MACHINE_[A-Z0-9]*\).*/\1/p')
if [ "$MACHINE" = "IMAGE_FILE_MACHINE_I386" ]; then
    say "image: machine is I386 -- this probe is the 32-bit lane's, not the AMD64 lane's"
else
    bad "the guest exe's machine is '$MACHINE', not IMAGE_FILE_MACHINE_I386"
fi

# -all keeps stdout clean enough to diff the transcript; the sabotage runs
# instead keep the err CLASS on every channel (-all,err+all), because what
# they must SEE is the port's own ERR naming the disabled lever -- a plain
# -all would silence exactly the diagnostic the negative control exists to
# read, and a silent red is the failure mode this gate is built to catch.
WDBG=${WINEDEBUG:--all}
WDBG_ERR=${WINEDEBUG:--all,err+all}

# ---- --sabotage levers ---------------------------------------------------
sabotage_lever() {   # $1 = env var name
    lever=$1
    started=$(date +%s)
    timeout -k 5 "${DEADLINE:-30}" \
        env WINEDEBUG="$WDBG_ERR" WINEDLLOVERRIDES="winedbg.exe=d" "$lever=1" \
        "$BUILD/wine" "$EXE" >"$OUT/$lever.out" 2>"$OUT/$lever.err"
    st=$?
    elapsed=$(( $(date +%s) - started ))
    if [ $st -eq 124 ] || [ $st -eq 137 ]; then
        bad "$lever=1 HUNG (killed after ${DEADLINE:-30}s); the disabled \
mechanism must fail promptly, not hang"
        tail -10 "$OUT/$lever.err" | sed "s/^/  $lever| /" >&2
        return
    fi
    if [ $st -eq 0 ]; then
        bad "$lever=1 exited 0; the sabotaged lane must not be a silent success"
    else
        say "$lever=1: exited $st after ${elapsed}s"
    fi
    if grep -q "wow_smoke: PASS" "$OUT/$lever.out"; then
        bad "$lever=1 still printed PASS; the lever did not reach anything \
this probe checks"
    fi
    if grep -q "$lever" "$OUT/$lever.err"; then
        say "$lever=1: the port named the lever: $(grep -m1 "$lever" \
            "$OUT/$lever.err" | cut -c1-110)"
    else
        sed "s/^/  $lever| /" "$OUT/$lever.err" | tail -15 >&2
        bad "$lever=1 died without the port naming the lever anywhere in its \
diagnostics; a silent death is exactly what this gate exists to make loud"
    fi
}

if [ "$SABOTAGE" = 1 ]; then
    sabotage_lever WINEEMUNOWOW32
    sabotage_lever WINEEMUNOFSBASE32
    [ $fail -eq 0 ] && say "SABOTAGE PASS"
    exit $fail
fi

# ---- 2: exit code --------------------------------------------------------
timeout -k 5 "$TIMEOUT" env WINEDEBUG="$WDBG" WINEDLLOVERRIDES="winedbg.exe=d" \
    "$BUILD/wine" "$EXE" exit >"$OUT/exitcode.out" 2>"$OUT/exitcode.err"
st=$?
if [ $st -eq 123 ]; then
    say "exit code: ExitProcess(123) came back as 123"
else
    tail -20 "$OUT/exitcode.err" | sed 's/^/  exit| /' >&2
    bad "the exit-code leg returned $st, not 123"
fi

# ---- 3: guest + transcript ----------------------------------------------
timeout -k 5 "$TIMEOUT" env WINEDEBUG="$WDBG" WINEDLLOVERRIDES="winedbg.exe=d" \
    "$BUILD/wine" "$EXE" >"$OUT/guest.out" 2>"$OUT/guest.err"
gst=$?
if [ $gst -eq 124 ] || [ $gst -eq 137 ]; then
    bad "the guest run timed out after ${TIMEOUT}s"
elif grep -q "wow_smoke: PASS" "$OUT/guest.out"; then
    say "guest: $(tail -1 "$OUT/guest.out")"
else
    sed 's/^/  guest| /' "$OUT/guest.out" >&2
    tail -30 "$OUT/guest.err" >&2
    bad "the i386 guest probe did not pass (exit $gst)"
fi

cat > "$OUT/expected.txt" <<'EOF'
wow_smoke: start
step 1 fs TIB self-pointer and stack bound: self=nonzero stackbase>local=yes ok
step 2 fs:[0x30] PEB names the image the loader names: peb_base==module=yes ok
step 3 GetModuleHandleW(NULL) is an MZ header: mz=yes ok
step 4 VirtualAlloc memory is writable and stable: pattern=held ok
step 5 GetProcAddress agrees with the import: same_pid=yes ok
step 6 QueryPerformanceFrequency fills 64 bits: nonzero=yes ok
step 7 SEH caught the NULL write and resumed: code=c0000005 ok
step 8 second thread ran with its own TIB: exit=1234 value=beef01 tib=distinct ok
wow_smoke: PASS 8/8
EOF
if cmp -s "$OUT/expected.txt" "$OUT/guest.out"; then
    say "transcript: the guest printed exactly the expected $(wc -l < "$OUT/expected.txt") lines"
else
    diff "$OUT/expected.txt" "$OUT/guest.out" | sed 's/^/  /' >&2
    bad "the guest transcript is not the expected one"
fi

# ---- 4: winepath, the canonical rung -------------------------------------
timeout -k 5 "$TIMEOUT" env WINEDEBUG="$WDBG" WINEDLLOVERRIDES="winedbg.exe=d" \
    "$BUILD/wine" winepath -u 'C:\windows' >"$OUT/winepath-native.out" 2>/dev/null
timeout -k 5 "$TIMEOUT" env WINEDEBUG="$WDBG" WINEDLLOVERRIDES="winedbg.exe=d" \
    "$BUILD/wine" "$BUILD/programs/winepath/i386-windows/winepath.exe" -u 'C:\windows' \
    >"$OUT/winepath-i386.out" 2>"$OUT/winepath-i386.err"
if [ ! -s "$OUT/winepath-native.out" ]; then
    bad "native winepath printed nothing; cannot compare"
elif cmp -s "$OUT/winepath-native.out" "$OUT/winepath-i386.out"; then
    say "winepath: the i386 PE printed byte-identical output to the native \
ppc64 build: $(cat "$OUT/winepath-native.out")"
else
    echo "  native: $(cat "$OUT/winepath-native.out")" >&2
    echo "  i386:   $(cat "$OUT/winepath-i386.out")" >&2
    tail -15 "$OUT/winepath-i386.err" | sed 's/^/  winepath| /' >&2
    bad "the i386 winepath's output differs from the native build's"
fi

# ---- 5: the port's own view ----------------------------------------------
timeout -k 5 "$TIMEOUT" env WINEDEBUG=trace+module WINEDLLOVERRIDES="winedbg.exe=d" \
    "$BUILD/wine" "$EXE" exit >/dev/null 2>"$OUT/trace.log"
LANE_UP=$(grep -c "32-bit lane up" "$OUT/trace.log")
if [ "${LANE_UP:-0}" -eq 1 ]; then
    say "port: the module trace shows the 32-bit emulator lane starting \
exactly once: $(grep -m1 '32-bit lane up' "$OUT/trace.log" | sed 's/.*trace://' | cut -c1-90)"
else
    grep "32-bit lane" "$OUT/trace.log" | sed 's/^/  trace| /' >&2
    bad "the module trace shows the 32-bit lane starting ${LANE_UP:-0} times, not 1"
fi

sabotage_lever WINEEMUNOWOW32
sabotage_lever WINEEMUNOFSBASE32
[ $fail -eq 0 ] && say "PASS"
exit $fail
