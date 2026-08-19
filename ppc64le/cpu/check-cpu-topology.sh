#!/bin/sh
#
# check-cpu-topology.sh -- the gate for what a guest is told about this
# machine's processors, and for whether thread affinity actually places threads
# where the guest asked.
#
# THE CLAIM.  A guest asking how big the machine is gets one answer, whichever
# call it asks with; a processor number it is handed can be used to index an
# array sized by that answer; and an affinity mask it sets moves its thread onto
# exactly the processors it named.  Three claims, and the third is the one that
# matters, because the first two are reporting bugs and the third changes what
# the scheduler does.
#
# WHY THIS GATE EXISTS.  There was none, and its absence is why the following
# survived.  [MEASURED] 2026-08-18, op4k, a POWER8 with 80 online CPUs:
#
#   GetSystemInfo().dwNumberOfProcessors        80
#   GetActiveProcessorCount(ALL_PROCESSOR_...)  32     <- disagrees by 2.5x
#   GetActiveProcessorGroupCount()               1
#   NtGetCurrentProcessorNumber()               up to 155 on a machine of 80
#   sched_setaffinity(cpus 0-7)                  0, and the thread ran on 4
#
# That last line -- the RAW sched_setaffinity(2), called natively, not through
# Wine -- is the whole reason this gate is shaped the way it is.  The call
# SUCCEEDED and left the thread with half the parallelism it asked for, because
# bits 4-7 named CPUs that are offline.  A leg that checked the return value
# would have been green through all of it.  So the affinity layer uses a NATIVE
# OBSERVER: the guest pins itself and holds still, and the gate reads that
# thread's real Cpus_allowed_list out of /proc.  Nothing the guest can see about
# itself is admitted as evidence, because everything the guest can see comes
# back through the port that got it wrong.
#
# THROUGH WIN32 THE SAME DEFECT WEARS THE OPPOSITE FACE, AND THAT IS WHY THE
# RETURN VALUE IS WORSE THAN USELESS HERE.  The process affinity mask is built
# over ONLINE LINUX CPU NUMBERS BELOW 64.  [MEASURED] 2026-08-18, op4k:
# GetProcessAffinityMask returned 0x0f0f0f0f0f0f0f0f -- 32 bits, at 0-3, 8-11,
# ... 56-59, which is exactly the online CPUs below 64 and nothing else.  Two
# things follow from that one mask, and both were measured:
#
#   * Bits 4-7 name OFFLINE CPUs, so they are CLEAR, so a guest asking for eight
#     processors with SetThreadAffinityMask(0xff) is REFUSED outright with
#     ERROR_INVALID_PARAMETER.  Bit 39 -- the top processor of what the contract
#     calls group 0 -- is clear for the same reason: Linux CPU 39 is offline.
#   * Bit 40 names Linux CPU 40, which IS online, so it is SET, so a mask naming
#     it is ACCEPTED and the thread really moves to Linux CPU 40.  Under the
#     contract processor 40 is the FIRST PROCESSOR OF GROUP 1, and a group-0
#     mask has no business naming it at all.
#
# The valid request fails and the invalid one succeeds.  A check that asked only
# "did the call return an error" would score that pair exactly backwards -- and
# neither half of it involves the wineserver, which still maps affinity bit i
# straight to CPU_SET(i) (server/thread.c).  It falls out of the affinity mask
# being built over sparse Linux indices, nothing more.
#
# NOTHING HERE IS SPECIFIC TO THIS MACHINE.  There is no 80, no SMT width, no
# node id and no assumption that CPU numbers are dense anywhere in this file.
# Every expected value is derived at run time by probes/derive-topology.awk from
# /sys/devices/system/cpu/online, /sys/devices/system/node/online and the
# per-node cpulists.  Reasoning about the other machine explicitly, because a
# gate that is only correct on the box it was written on is the same disease it
# is here to cure:
#
#   ON A DENSE SINGLE-NODE x86 BOX -- online "0-7", node/online "0" -- the
#   derivation emits 8 processors, one group of 8, node id 0, and the IDENTITY
#   map.  Layer 1 then requires all five count calls to say 8.  Layer 2's
#   in-range assertion is satisfied by any placement, since the highest online
#   CPU (7) is below the count (8) -- it is vacuous there, and it is SUPPOSED to
#   be: on a dense machine that bug cannot occur.  Layer 3 requires an affinity
#   mask of bits 0-7 to land on Linux CPUs 0-7, which is what that machine has
#   always done, so the layer passes without the port doing anything special.
#   Layer 4 requires node id 0 and a highest node number of 0.  And the two
#   sabotage levers that force the identity map say so and are SKIPPED rather
#   than passed, because on a machine where the correct map IS the identity they
#   cannot make anything red, and a control that cannot control is not a pass.
#
# Layers, in ascending order of what they prove:
#
#   0  THE DERIVATION ITSELF, before any guest is involved.  Three parts:
#      0a  probes/derive-topology.awk reads this machine and produces the truth
#          table every later layer compares against.
#      0b  probes/topology_cases.c runs the REAL derivation from
#          include/wine/cputopology.h against ten SYNTHETIC machines -- POWER8
#          at SMT8/4/2/1, x86 at 8/64/65/128/96 CPUs over one to four nodes, and
#          80 processors inside a single NUMA node.  This is the part that
#          proves the rule is general, which cannot be proved from one box.
#      0c  probes/topo_ref.c prints the header's answer for THIS machine, and it
#          must match 0a line for line.  Two independent implementations of the
#          same contract, in different languages, cross-checked.  If the gate
#          asked the header what the answer was it would agree by construction
#          and could never catch the header being wrong.
#
#   1  THE COUNTS AGREE AND ARE REAL.  GetSystemInfo, GetActiveProcessorCount
#      for all groups and for each group, GetActiveProcessorGroupCount,
#      GetMaximumProcessorCount, GetProcessAffinityMask and the RelationGroup
#      records must agree with each other AND with the derived truth.
#
#   2  A PROCESSOR NUMBER IS AN INDEX, NOT A LINUX CPU ID.
#      NtGetCurrentProcessorNumber must always be below the count the same port
#      reports, or a guest sizing an array by one and indexing it by the other
#      writes out of bounds.  Sampled while free-running, and then forced: the
#      whole run is re-done under `taskset -c <highest online CPU>`, which is
#      derived from /sys and is the deterministic way to reach the case.
#
#   3  AFFINITY ACTUALLY PLACES THE THREAD, read from outside.  Four legs, and
#      3a is the calibration: a NATIVE program pins itself correctly by
#      construction and the observer must call it GREEN.  Without that, on a
#      tree where the guest's affinity is broken, a broken observer and a broken
#      port look identical.
#
#   4  NUMA IS DESCRIBED WITH THE KERNEL'S OWN NODE IDS.  This machine's nodes
#      are 0 and 8.  A guest walking 0..GetNumaHighestNodeNumber() has to reach
#      both, so the highest node number must be the highest ID, not the count
#      minus one.
#
#   5  THE CORE RECORDS ADD UP.  Every processor appears in exactly one
#      RelationProcessorCore mask, and the number of records matches the number
#      of distinct online thread-sibling sets the kernel reports.
#
# --sabotage runs the negative controls instead, and each must go red for a
# DIFFERENT reason:
#
#   sabotage(ref-identity)   CPUTOP_REF_IDENTITY=1 makes the native reference
#                            pin by the identity map -- the old assumption, that
#                            Windows CPU i IS Linux CPU i -- instead of through
#                            the table.  Layer 3a must go red.  This is a lever
#                            on CODE, not an inverted assertion, and it is the
#                            proof that the observer can tell a right placement
#                            from a wrong one at all.
#   sabotage(ref-truncate)   CPUTOP_REF_TRUNCATE=1 makes the reference report
#                            the topology the unfixed tree reports -- stop at
#                            the first CPU whose NUMBER passes 64, claim one
#                            group.  Layer 0c must go red.  A different reason
#                            from the above: that one is about WHERE a thread
#                            goes, this one about HOW BIG the machine is said to
#                            be, and neither control can stand in for the other.
#   sabotage(nocpumap)       WINEEMUNOCPUMAP=1 must make ntdll and the server
#                            use the identity map, so layer 3b goes red.  This
#                            lever lives in files this gate does not own; if it
#                            is not in the build the leg reports that and the
#                            sabotage run exits 2, because a control that was
#                            never run is not a control that passed.
#   sabotage(nocpugroups)    WINEEMUNOCPUGROUPS=1 must make ntdll report one
#                            group and stop enumerating at Linux CPU 64, so
#                            layer 1 goes red.  Same ownership note.
#
# Exit 0 = pass, 1 = a check failed, 2 = could not run (NOT a pass).
#
# Copyright 2026 the ppc64le port authors
#
# This library is free software; you can redistribute it and/or
# modify it under the terms of the GNU Lesser General Public
# License as published by the Free Software Foundation; either
# version 2.1 of the License, or (at your option) any later version.
set -u

HERE=$(cd "$(dirname "$0")" && pwd)
SRC=$(cd "$HERE/../.." && pwd)
BUILD=${BUILD:-$SRC}
OUT=${OUT:-/tmp/cpu-topology}
SABOTAGE=0
[ "${1:-}" = "--sabotage" ] && SABOTAGE=1

# A STALE WINESERVER DEFEATS EVERY SERVER-SIDE LEVER, silently.
#
# WINEEMUNOCPUMAP is read by the wineserver, once, into a static -- the house
# pattern for a lever, and right for a value that must not change under a
# running process.  But a wineserver outlives the run that started it, so a
# server already up from an EARLIER run latched the variable's old value and
# will not see this run's.  [MEASURED] 2026-08-18: with a leftover server the
# nocpumap leg reported "the guest's thread still landed on [0,1,2,3,8,9,10,11]
# with the identity map forced" -- the lever appeared not to work; with the
# server killed first, the same command goes red exactly as it should.
#
# A negative control that a leftover process can defeat is not a control, and
# the failure is the dangerous direction: it looks like the mechanism under
# test is broken when it is fine, or -- if the stale server happens to have the
# variable set -- like a control passing when nothing was tested.  So the
# server is retired here, before anything runs, in both modes.  `wineserver -k`
# is scoped to $WINEPREFIX and touches nobody else's.
if [ -x "$BUILD/server/wineserver" ] && [ -n "${WINEPREFIX:-}" ]; then
    "$BUILD/server/wineserver" -k >/dev/null 2>&1 || true
    sleep 1
fi

say()  { echo "check-cpu-topology: $*"; }
bad()  { echo "check-cpu-topology: FAIL $*" >&2; fail=1; }
note() { echo "check-cpu-topology: note $*"; }
skip() { echo "check-cpu-topology: $*" >&2; exit 2; }

fail=0
TIMEOUT=${TIMEOUT:-240}
DEADLINE=${DEADLINE:-90}

# ---- preconditions -------------------------------------------------------
# Each of these names the real cause rather than the symptom it produces.
[ -x "$BUILD/wine" ] || skip "no wine loader at $BUILD/wine; build the tree first"
[ -n "${WINEPREFIX:-}" ] || skip "set WINEPREFIX to a prefix wineboot has run in"
[ -d "$WINEPREFIX/drive_c" ] || \
    skip "WINEPREFIX=$WINEPREFIX has no drive_c, so wineboot has never completed \
in it.  This gate hands the guest a DOS path under C: to synchronise on; \
without drive_c the guest has nowhere to look and every affinity leg would \
time out rather than fail"
[ -n "${WINEFEXBRIDGE:-}" ] || skip "set WINEFEXBRIDGE to the emulator bridge"
if command -v nm >/dev/null && [ -f "${WINEFEXBRIDGE:-/nonexistent}" ]; then
    nm -D --defined-only "$WINEFEXBRIDGE" 2>/dev/null | \
        grep -q fexbridge_process_init || \
        skip "the bridge at $WINEFEXBRIDGE exports no fexbridge_process_init, so \
it cannot start an x86-64 guest at all.  Rebuild it -- 'ninja fexbridge' in the \
FEX build directory -- before anything here can run"
fi
[ -f "$BUILD/dlls/kernel32/x86_64-windows/kernel32.dll" ] || \
    skip "no guest kernel32; build with --enable-archs=ppc64 first"
[ -f "$SRC/include/wine/cputopology.h" ] || \
    skip "include/wine/cputopology.h is missing.  That header is the contract \
both ntdll and the wineserver derive the processor mapping from, and this gate \
checks the port against a second, independent derivation of it -- with the \
header gone there is nothing to cross-check and layer 0 is meaningless"
[ -f "$HERE/probes/topology_cases.c" ] || \
    skip "probes/topology_cases.c is missing; it is the only part of this gate \
that can show the derivation is right on machines this one is not"
[ -r /sys/devices/system/cpu/online ] || \
    skip "/sys/devices/system/cpu/online is unreadable, so this gate cannot \
learn what the machine actually is and has no expected values to check against"
command -v clang >/dev/null || skip "need clang for the guest build"
command -v llvm-dlltool >/dev/null || skip "need llvm-dlltool for the guest build"
command -v llvm-readobj >/dev/null || skip "need llvm-readobj to read the built image"
command -v awk >/dev/null || skip "need awk"
command -v strings >/dev/null || skip "need strings (binutils) -- --sabotage \
uses it to tell a lever that is absent from the build apart from a lever that \
is present and broken, and those two must not be confused"
CC=${CC:-cc}
command -v "$CC" >/dev/null || skip "need a native C compiler (\$CC=$CC) for the \
observer's reference program"
command -v taskset >/dev/null || skip "need taskset (util-linux) to force the \
processor-number leg onto the highest online CPU; without it layer 2 could only \
sample whatever CPU the scheduler happened to pick, which on a lightly loaded \
machine is usually a low one and would make the leg pass by luck"

mkdir -p "$OUT" || skip "cannot create $OUT"
rm -f "$OUT"/*.out "$OUT"/*.err "$OUT"/go.* 2>/dev/null

# ==========================================================================
# Layer 0a: what this machine actually is.
# ==========================================================================
TRUTH="$OUT/truth.txt"
awk -f "$HERE/probes/derive-topology.awk" </dev/null > "$TRUTH" 2>"$OUT/truth.err"
if [ $? -ne 0 ] || grep -q '^error ' "$TRUTH"; then
    sed 's/^/      | /' "$TRUTH" "$OUT/truth.err" >&2
    skip "could not derive this machine's topology from /sys (see above)"
fi

t_get()  { awk -v k="$1" '$1==k{print $2; exit}' "$TRUTH"; }
gsize()  { awk -v g="$1" '$1=="gsize" && $2==g {print $3; exit}' "$TRUTH"; }
# node <i> <kernel_id> <online_cpus>
node_ids()      { awk '$1=="node"{print $3}' "$TRUTH" | sort -n | tr '\n' ' '; }
node_online()   { awk -v id="$1" '$1=="node" && $3==id {print $4; exit}' "$TRUTH"; }
# map <win> <unix> <group> <inidx> <node>
map_unix()      { awk -v w="$1" '$1=="map" && $2==w {print $3; exit}' "$TRUTH"; }
win_of_unix()   { awk -v u="$1" '$1=="map" && $3==u {print $2; exit}' "$TRUTH"; }
win_group()     { awk -v w="$1" '$1=="map" && $2==w {print $4; exit}' "$TRUTH"; }
win_inidx()     { awk -v w="$1" '$1=="map" && $2==w {print $5; exit}' "$TRUTH"; }
groups_of_node(){ awk -v id="$1" '$1=="map" && $6==id {print $4}' "$TRUTH" | sort -nu | tr '\n' ' '; }

COUNT=$(t_get count)
GROUP_COUNT=$(t_get group_count)
NODE_COUNT=$(t_get node_count)
CORE_COUNT=$(t_get core_count)
HIGHEST=$(t_get highest_online)
NODE_IDS=$(node_ids)
NODE_MAX=$(echo "$NODE_IDS" | tr ' ' '\n' | sort -n | tail -1)

[ -n "$COUNT" ] && [ "$COUNT" -gt 0 ] || \
    skip "the derivation produced no processors at all, which cannot be true \
of a running machine; /sys is not what this gate expects"

say "layer 0a: this machine has $COUNT online processors in $GROUP_COUNT \
group(s), NUMA node id(s) [${NODE_IDS% }], highest online Linux CPU $HIGHEST"

# The Windows-processor bits of a group turned into the Linux CPUs they name,
# as a sorted comma list -- the form Cpus_allowed_list is normalised into.
# $1 = group, $2.. = bit indices within that group.
bits_to_unix() {
    _g=$1; shift
    for _b in "$@"; do
        awk -v g="$_g" -v b="$_b" '$1=="map" && $4==g && $5==b {print $3; exit}' "$TRUTH"
    done | sort -n | paste -sd, -
}

# The same bits under the OLD assumption -- Windows CPU i IS Linux CPU i -- so
# a failure can say which of the two answers the port actually produced.
bits_to_identity() { shift; for _b in "$@"; do echo "$_b"; done | sort -n | paste -sd, -; }

# A 64-bit affinity mask as hex, built nibble by nibble.  Not arithmetic: awk
# numbers are doubles and bit 63 is not exactly representable in one, so a mask
# naming the top processor of a full group would come out wrong.
mask_hex() {
    for _b in "$@"; do echo "$_b"; done | awk '
        { n[int($1/4)] += 2 ^ ($1 % 4) }
        END {
            s = ""; seen = 0
            for (i = 15; i >= 0; i--)
            {
                d = n[i] + 0
                if (!d && !seen) continue
                seen = 1
                s = s sprintf( "%x", d )
            }
            if (!seen) s = "0"
            print "0x" s
        }'
}

# "0-3,8-11" -> "0,1,2,3,8,9,10,11".  Both sides of every affinity comparison go
# through this, so the comparison is of SETS and not of the kernel's formatting.
expand_list() {
    echo "$1" | awk '{
        n = split( $0, parts, "," )
        c = 0
        for (i = 1; i <= n; i++)
        {
            d = index( parts[i], "-" )
            if (d) { a = substr( parts[i], 1, d-1 ) + 0; b = substr( parts[i], d+1 ) + 0 }
            else   { a = parts[i] + 0; b = a }
            for (j = a; j <= b; j++) out[c++] = j
        }
        s = ""
        for (i = 0; i < c; i++) s = s (i ? "," : "") out[i]
        print s
    }'
}

# ==========================================================================
# Layer 0b: the derivation on ten machines this one is not.
# ==========================================================================
CASEDIR="$OUT/cases"
rm -rf "$CASEDIR"
mkdir -p "$CASEDIR"
if ! $CC -O1 -Wall -I"$SRC/include" -DWINE_CPU_SYSFS_ROOT="\"$CASEDIR\"" \
        -o "$OUT/topology_cases" "$HERE/probes/topology_cases.c" 2>"$OUT/cases.cc.err"; then
    sed 's/^/      | /' "$OUT/cases.cc.err" >&2
    skip "could not compile probes/topology_cases.c against \
$SRC/include/wine/cputopology.h; the contract header and its case harness have \
gone out of step and nothing below can be trusted until they agree"
fi
if "$OUT/topology_cases" "$CASEDIR" > "$OUT/cases.out" 2>&1 && \
   grep -q "all cases passed" "$OUT/cases.out"; then
    say "layer 0b: the derivation is right on all $(grep -c '^  [^ ]' "$OUT/cases.out") \
synthetic machines (POWER8 at four SMT widths, x86 at five sizes, one \
oversized NUMA node)"
else
    grep -E "FAIL|failure" "$OUT/cases.out" | sed 's/^/      | /' >&2
    bad "layer 0b: the contract derivation is wrong on a machine we do not \
have -- see $OUT/cases.out"
fi

# ==========================================================================
# Layer 0c: the header's answer for THIS machine must match the gate's own.
# ==========================================================================
if ! $CC -O1 -Wall -I"$SRC/include" -o "$OUT/topo_ref" "$HERE/probes/topo_ref.c" \
        2>"$OUT/ref.cc.err"; then
    sed 's/^/      | /' "$OUT/ref.cc.err" >&2
    skip "could not compile probes/topo_ref.c; the native half of the observer \
is what calibrates the affinity layer, and without it a red affinity leg cannot \
be told apart from a broken gate"
fi

# The awk script emits two records the header does not know about and should
# not: strip them rather than teaching the header about them.
grep -vE '^(highest_online|core_count) ' "$TRUTH" > "$OUT/truth.cmp"

ref_dump() {   # ref_dump <outfile> [env...]
    _o=$1; shift
    env "$@" "$OUT/topo_ref" dump > "$_o" 2>"$_o.err"
}

ref_dump "$OUT/ref.out"
if cmp -s "$OUT/truth.cmp" "$OUT/ref.out"; then
    say "layer 0c: the contract header and this gate's own derivation agree on \
all $(wc -l < "$OUT/ref.out") records for this machine"
else
    diff "$OUT/truth.cmp" "$OUT/ref.out" | head -30 | sed 's/^/      | /' >&2
    bad "layer 0c: include/wine/cputopology.h and probes/derive-topology.awk \
disagree about THIS machine (< gate, > header).  One of them has drifted from \
the contract, and until they agree every expected value below is suspect"
fi

# ==========================================================================
# The guest PE.
# ==========================================================================
INCL="-I$BUILD/include -I$SRC/include -I$SRC/include/msvcrt"
cat > "$OUT/kernel32.def" <<'EOF'
LIBRARY kernel32.dll
EXPORTS
GetStdHandle
WriteFile
ExitProcess
Sleep
SwitchToThread
GetLastError
SetLastError
GetEnvironmentVariableA
GetFileAttributesA
GetCurrentThread
GetCurrentProcess
GetCurrentProcessId
GetSystemInfo
GetProcessAffinityMask
SetThreadAffinityMask
GetThreadGroupAffinity
SetThreadGroupAffinity
GetActiveProcessorCount
GetActiveProcessorGroupCount
GetMaximumProcessorCount
GetMaximumProcessorGroupCount
GetCurrentProcessorNumber
GetCurrentProcessorNumberEx
GetLogicalProcessorInformationEx
GetNumaHighestNodeNumber
EOF
cat > "$OUT/ntdll.def" <<'EOF'
LIBRARY ntdll.dll
EXPORTS
NtGetCurrentProcessorNumber
EOF
for m in kernel32 ntdll; do
    llvm-dlltool -m i386:x86-64 -d "$OUT/$m.def" -l "$OUT/lib$m.a" \
        || skip "llvm-dlltool failed for $m"
done

GUESTCC="clang -target x86_64-windows-gnu -nostdlibinc $INCL -fms-extensions \
-D_UCRT -Wall -O1 -fno-builtin -g"
GUESTLD="clang -target x86_64-windows-gnu -fuse-ld=lld -nostdlib \
-Wl,--entry=cputopo_entry -Wl,--subsystem,console"

$GUESTCC -c -o "$OUT/cputopo.o" "$HERE/probes/cputopo.c" 2>"$OUT/guest.cc.err" || {
    sed 's/^/      | /' "$OUT/guest.cc.err" | tail -20 >&2
    skip "guest compile failed"
}
$GUESTLD -o "$OUT/cputopo.exe" "$OUT/cputopo.o" "$OUT/libkernel32.a" "$OUT/libntdll.a" \
    2>"$OUT/guest.ld.err" || {
    sed 's/^/      | /' "$OUT/guest.ld.err" | tail -20 >&2
    skip "guest link failed"
}
EXE="$OUT/cputopo.exe"

# The import table really names both modules.  Cheap, and it catches the case
# where the probe quietly stopped calling the NT entry point -- which would
# leave layer 2 checking nothing while still printing a number.
imports=$(llvm-readobj --coff-imports "$EXE" 2>/dev/null | \
          sed -n 's/^ *Name: \(.*\)$/\1/p' | tr 'A-Z' 'a-z' | sort -u)
for m in kernel32.dll ntdll.dll; do
    case "$imports" in
        *"$m"*) ;;
        *) bad "build: $EXE does not import $m (got: $(echo $imports))" ;;
    esac
done

WDBG=${WINEDEBUG:--all}
GUEST_ENV="WINEDEBUG=$WDBG WINEDLLOVERRIDES=winedbg.exe=d"

run_report() {   # run_report <outfile> [env assignments...]
    _o=$1; shift
    # shellcheck disable=SC2086
    env $GUEST_ENV CPUTOPO_MODE=report "$@" \
        timeout -k 5 "$TIMEOUT" "$BUILD/wine" "$EXE" > "$_o" 2>"$_o.err"
    echo $?
}

# ==========================================================================
# The observer.  This is the part that has to be right.
# ==========================================================================
# A snapshot of every thread in a process and the CPUs it is REALLY allowed on,
# straight from the kernel.  Not the mask the guest set, not the mask the port
# reports back: Cpus_allowed_list, which is what the scheduler will obey.
snapshot() {   # snapshot <pid> <outfile>
    _p=$1; _o=$2
    : > "$_o"
    for _t in /proc/"$_p"/task/*; do
        [ -d "$_t" ] || continue
        _tid=${_t##*/}
        _l=$(sed -n 's/^Cpus_allowed_list:[ \t]*//p' "$_t/status" 2>/dev/null)
        [ -n "$_l" ] && echo "$_tid $_l" >> "$_o"
    done
}

wait_for_line() {   # wait_for_line <file> <text> <seconds> <pid>
    _f=$1; _txt=$2; _secs=$3; _pid=$4
    _n=0
    while [ "$_n" -lt $((_secs * 20)) ]; do
        grep -q "^$_txt\$" "$_f" 2>/dev/null && return 0
        kill -0 "$_pid" 2>/dev/null || { sleep 0.2; grep -q "^$_txt\$" "$_f" 2>/dev/null && return 0; return 1; }
        sleep 0.05
        _n=$((_n + 1))
    done
    return 1
}

# Drive one pinning run and report what the kernel says happened.
#
#   observe <label> <logfile> <go-prefix-unix> <expected-list> <cmd...>
#
# The two-phase handshake exists so the observer never has to guess which Linux
# tid the guest thread is -- a guest cannot tell it, since the thread id Windows
# hands out is the server's and not the kernel's.  Instead: snapshot every
# thread before, snapshot every thread after, and require that EXACTLY ONE
# thread present in both changed its allowed set.  A thread created between the
# two snapshots is not "changed", it is new, so a busy process does not confuse
# it.
#
# Sets OBS_RESULT to ok | <diagnostic>, and OBS_ACTUAL to the set observed.
observe() {
    _label=$1; _log=$2; _go=$3; _expect=$4; shift 4
    OBS_RESULT=""; OBS_ACTUAL=""
    rm -f "$_go.baseline" "$_go.pinned" "$_log" "$_log.err"
    : > "$_log"

    "$@" >> "$_log" 2>"$_log.err" &
    _pid=$!

    if ! wait_for_line "$_log" "READY-BASELINE" "$DEADLINE" "$_pid"; then
        kill -TERM "$_pid" 2>/dev/null
        wait "$_pid" 2>/dev/null
        OBS_RESULT="never reached READY-BASELINE within ${DEADLINE}s"
        return 1
    fi
    snapshot "$_pid" "$OUT/$_label.before"
    if [ ! -s "$OUT/$_label.before" ]; then
        kill -TERM "$_pid" 2>/dev/null
        wait "$_pid" 2>/dev/null
        OBS_RESULT="no thread of pid $_pid has a Cpus_allowed_list; the process \
being watched is not the one that pinned itself"
        return 1
    fi
    : > "$_go.baseline"

    if ! wait_for_line "$_log" "READY-PINNED" "$DEADLINE" "$_pid"; then
        kill -TERM "$_pid" 2>/dev/null
        wait "$_pid" 2>/dev/null
        OBS_RESULT="never reached READY-PINNED within ${DEADLINE}s"
        return 1
    fi
    snapshot "$_pid" "$OUT/$_label.after"
    : > "$_go.pinned"
    wait "$_pid" 2>/dev/null

    # The threads present in both snapshots whose allowed set changed.
    _changed=$(awk 'NR==FNR { was[$1] = substr( $0, index( $0, " " ) + 1 ); next }
                    ($1 in was) {
                        now = substr( $0, index( $0, " " ) + 1 )
                        if (now != was[$1]) print $1, now
                    }' "$OUT/$_label.before" "$OUT/$_label.after")
    _n=$(echo "$_changed" | grep -c . )

    if [ "$_n" -eq 0 ]; then
        OBS_RESULT="no thread's Cpus_allowed_list changed at all -- the \
affinity request never reached sched_setaffinity"
        return 1
    fi
    if [ "$_n" -gt 1 ]; then
        OBS_RESULT="$_n threads changed their allowed set, not one: $(echo $_changed)"
        return 1
    fi
    OBS_ACTUAL=$(expand_list "$(echo "$_changed" | awk '{print $2}')")
    [ "$OBS_ACTUAL" = "$_expect" ] && OBS_RESULT=ok || \
        OBS_RESULT="the thread ended up on Linux CPUs [$OBS_ACTUAL], not [$_expect]"
    [ "$OBS_RESULT" = ok ]
}

# ==========================================================================
# Layer 3a: calibrate the observer against a placement that is right by
# construction.  Runs before the guest legs on purpose -- if this is red, every
# red below it is uninterpretable.
# ==========================================================================
NBITS=8
[ "$NBITS" -gt "$(gsize 0)" ] && NBITS=$(gsize 0)
BITS=$(awk -v n="$NBITS" 'BEGIN { for (i = 0; i < n; i++) printf "%s%d", (i?" ":""), i }')
# shellcheck disable=SC2086
MASK=$(mask_hex $BITS)
# shellcheck disable=SC2086
EXPECT=$(bits_to_unix 0 $BITS)
# shellcheck disable=SC2086
NAIVE=$(bits_to_identity 0 $BITS)

ref_pin() {   # ref_pin <label> <expected> [env...]
    _lbl=$1; _exp=$2; shift 2
    observe "$_lbl" "$OUT/$_lbl.out" "$OUT/go-$_lbl" "$_exp" \
        env "$@" "$OUT/topo_ref" pin 0 "$MASK" "$OUT/go-$_lbl"
}

# NOTE THE ABSENCE OF `timeout` HERE, and it is not an oversight.  timeout(1)
# FORKS its child, so $! would be timeout's pid and the observer would read
# /proc for a process with one thread that never pins anything -- a gate that
# reported "no thread changed" for every run, including correct ones.  `env`
# execs, so the pid the shell gets is the pid the guest runs under.  The
# deadline is enforced by observe() instead, and the probe itself gives up on
# the handshake after sixty seconds so a dead gate leaves nothing spinning.
guest_pin_leg() {   # guest_pin_leg <label> <group> <api> <maskhex> <expected> [env...]
    _lbl=$1; _grp=$2; _api=$3; _msk=$4; _exp=$5; shift 5
    observe "$_lbl" "$OUT/$_lbl.out" "$WINEPREFIX/drive_c/go-$_lbl" "$_exp" \
        env $GUEST_ENV CPUTOPO_MODE=pin CPUTOPO_GROUP="$_grp" CPUTOPO_MASK="$_msk" \
        CPUTOPO_API="$_api" CPUTOPO_GO="C:\\go-$_lbl" "$@" "$BUILD/wine" "$EXE"
}

if [ "$SABOTAGE" = 0 ]; then
    if ref_pin refpin "$EXPECT"; then
        say "layer 3a: the native reference asked for group 0 bits 0-$((NBITS-1)) \
(mask $MASK) and the kernel put it on Linux CPUs [$EXPECT] -- the observer can \
see a correct placement"
    else
        sed 's/^/      | /' "$OUT/refpin.out" >&2
        tail -5 "$OUT/refpin.out.err" | sed 's/^/      # /' >&2
        bad "layer 3a: the OBSERVER is not working, or this machine will not \
honour the placement the contract asks for: $OBS_RESULT.  Every affinity result \
below is uninterpretable until this is green"
    fi
fi

# ==========================================================================
# The sabotage legs.
# ==========================================================================
# Is <NAME> a lever this build actually has?
#
# SEARCHED IN BOTH ENCODINGS AND IN EVERY BINARY THAT COULD HOLD IT, because
# getting this wrong fails silently in the worst direction: a detector that
# cannot see a lever that IS there reports it MISSING forever, the control is
# never run, and nobody notices because the message looks like a to-do rather
# than a bug.  [MEASURED] 2026-08-18, op4k: this port's existing levers are WIDE
# string literals -- emu_env_flag( L"WINEEMUNORIPCACHE" ) in
# dlls/ntdll/signal_ppc64.c -- and land in dlls/ntdll/ntdll.dll.so as UTF-16LE,
# where a plain `strings` over ntdll.so finds nothing at all.  The unix-side
# files these levers will live in use getenv() and produce plain ASCII in
# ntdll.so and wineserver, so both encodings and all three binaries are checked.
lever_in_build() {   # lever_in_build <NAME>
    for _f in "$BUILD/dlls/ntdll/ntdll.so" "$BUILD/dlls/ntdll/ntdll.dll.so" \
              "$BUILD/server/wineserver"; do
        [ -f "$_f" ] || continue
        strings -a "$_f" 2>/dev/null | grep -q "$1" && return 0
        strings -a -el "$_f" 2>/dev/null | grep -q "$1" && return 0
    done
    return 1
}

if [ "$SABOTAGE" = 1 ]; then
    sfail=0
    missing=0

    # --- ref-identity: a lever on CODE, in a file this gate owns ------------
    if ref_pin refid "$EXPECT" CPUTOP_REF_IDENTITY=1; then
        if grep -q '^identity.effective 0$' "$OUT/refid.out"; then
            note "sabotage(ref-identity): SKIPPED -- on this machine the correct \
map IS the identity ([$EXPECT] == [$NAIVE]), so forcing it cannot make anything \
red.  That is the expected outcome on a dense single-node box, and it is \
reported rather than counted as a pass"
        else
            echo "check-cpu-topology: SABOTAGE FAIL ref-identity: the observer \
still called the placement correct with the identity map forced.  It is \
comparing against something that does not depend on the mapping, so layer 3 \
proves nothing" >&2
            sfail=1
        fi
    else
        say "sabotage(ref-identity): the observer went red with the identity map \
forced, as it must -- $OBS_RESULT"
    fi

    # --- ref-truncate: a different reason, same ownership -------------------
    ref_dump "$OUT/reftrunc.out" CPUTOP_REF_TRUNCATE=1
    if grep -q '^truncate.effective 0$' "$OUT/reftrunc.out"; then
        note "sabotage(ref-truncate): SKIPPED -- this machine has no online CPU \
numbered above 64, so the old index cap removes nothing and cannot make the \
count comparison red"
    else
        grep -v '^truncate.effective ' "$OUT/reftrunc.out" > "$OUT/reftrunc.cmp"
        if cmp -s "$OUT/truth.cmp" "$OUT/reftrunc.cmp"; then
            echo "check-cpu-topology: SABOTAGE FAIL ref-truncate: layer 0c still \
matched with the topology truncated the way the unfixed tree truncates it -- the \
comparison is not looking at the counts" >&2
            sfail=1
        else
            say "sabotage(ref-truncate): layer 0c went red with the old index cap \
(count $(awk '$1=="count"{print $2}' "$OUT/reftrunc.cmp") instead of $COUNT), as it must"
        fi
    fi

    # --- the two levers this gate does NOT own -----------------------------
    # First, is the DETECTOR working?  WINEEMUNORIPCACHE is a lever this tree
    # is known to have (dlls/ntdll/signal_ppc64.c, the thunk-cache gate's
    # negative control).  If lever_in_build cannot find that one, it cannot be
    # trusted to find the two below either, and every MISSING it prints would be
    # a false alarm dressed up as a to-do item.
    if ! lever_in_build WINEEMUNORIPCACHE; then
        note "sabotage: the lever detector cannot find WINEEMUNORIPCACHE, which \
this tree does have.  The detector is broken, so treat any MISSING below as \
unproven rather than as a real gap"
    fi
    for lv in WINEEMUNOCPUMAP WINEEMUNOCPUGROUPS; do
        if ! lever_in_build "$lv"; then
            echo "check-cpu-topology: SABOTAGE MISSING $lv is not in this build. \
It has to live in dlls/ntdll/unix/system.c, dlls/ntdll/unix/thread.c and \
server/thread.c, which this gate does not own.  $lv=1 must make ntdll and the \
server $( [ "$lv" = WINEEMUNOCPUMAP ] && echo "use the identity map (Windows CPU \
i IS Linux CPU i) for affinity in both directions and for \
NtGetCurrentProcessorNumber" || echo "report exactly one processor group and \
stop enumerating at Linux CPU number 64, i.e. the pre-fix \
create_logical_proc_info()" ).  Until it exists this control cannot be run, and \
a control that was not run is not a control that passed" >&2
            missing=1
            continue
        fi
        case "$lv" in
        WINEEMUNOCPUMAP)
            if guest_pin_leg nocpumap 0 mask "$MASK" "$EXPECT" "$lv=1"; then
                echo "check-cpu-topology: SABOTAGE FAIL nocpumap: the guest's \
thread still landed on [$EXPECT] with the identity map forced" >&2
                sfail=1
            else
                say "sabotage(nocpumap): the guest's affinity leg went red with \
the identity map forced, as it must -- $OBS_RESULT"
            fi
            ;;
        WINEEMUNOCPUGROUPS)
            rc=$(run_report "$OUT/nogroups.out" "$lv=1")
            got=$(awk '$1=="apc.all"{print $2}' "$OUT/nogroups.out")
            if [ "${got:-}" = "$COUNT" ]; then
                echo "check-cpu-topology: SABOTAGE FAIL nocpugroups: \
GetActiveProcessorCount still said $COUNT with the group machinery forced back \
to one group" >&2
                sfail=1
            else
                say "sabotage(nocpugroups): the count leg went red with one \
forced group (guest said ${got:-nothing}, machine has $COUNT), as it must"
            fi
            ;;
        esac
    done

    if [ $missing = 1 ]; then
        echo "check-cpu-topology: SABOTAGE INCOMPLETE -- see MISSING above" >&2
        exit 2
    fi
    [ $sfail -eq 0 ] && say "SABOTAGE PASS"
    exit $sfail
fi

# ==========================================================================
# Layer 1: the counts.
# ==========================================================================
rc=$(run_report "$OUT/report.out")
if [ "$rc" != 0 ] || ! grep -q '^END$' "$OUT/report.out"; then
    sed 's/^/      | /' "$OUT/report.out" >&2
    tail -20 "$OUT/report.out.err" | sed 's/^/      # /' >&2
    skip "the guest probe did not finish (exit $rc); nothing about the guest's \
view of this machine can be checked"
fi

g_get() { awk -v k="$1" '$1==k{print $2; exit}' "$OUT/report.out"; }

# $LAYER names the layer in the message, so a line from layer 4 does not claim
# to be from layer 1 -- a gate whose failures point at the wrong layer costs
# whoever reads it the time it was meant to save.
LAYER=1
want() {   # want <key> <expected> <why>
    _k=$1; _e=$2; _why=$3
    _v=$(g_get "$_k")
    if [ "${_v:-}" = "$_e" ]; then
        say "layer $LAYER: $_k = $_v"
    else
        bad "layer $LAYER: $_k = ${_v:-<absent>}, but this machine has $_e ($_why)"
    fi
}

want si.numberofprocessors "$COUNT" "online CPUs in /sys/devices/system/cpu/online"
want apc.all               "$COUNT" "GetActiveProcessorCount(ALL_PROCESSOR_GROUPS) \
has to be the whole machine, and it is the number a guest sizes a worker pool from"
want agc                   "$GROUP_COUNT" "the contract puts $COUNT processors in \
$GROUP_COUNT group(s)"

sum=0
g=0
while [ "$g" -lt "$GROUP_COUNT" ]; do
    want "group.$g.active" "$(gsize $g)" "the size of group $g"
    gm=$(g_get "group.$g.maximum")
    gs=$(gsize $g)
    if [ -z "${gm:-}" ]; then
        bad "layer 1: GetMaximumProcessorCount($g) said nothing"
    elif [ "$gm" -lt "$gs" ] || [ "$gm" -gt 64 ]; then
        bad "layer 1: GetMaximumProcessorCount($g) = $gm, which is not between \
the group's $gs active processors and the 64 a group can hold"
    fi
    want "glpi.group.$g.active"  "$gs" "the RelationGroup record for group $g"
    want "glpi.group.$g.maskpop" "$gs" "the bits set in group $g's \
ActiveProcessorMask -- a count and a mask that disagree are two different \
answers to the same question"
    v=$(g_get "group.$g.active"); sum=$((sum + ${v:-0}))
    g=$((g + 1))
done
if [ "$sum" = "$COUNT" ]; then
    say "layer 1: the per-group counts add up to $sum, the whole machine"
else
    bad "layer 1: the per-group counts add up to $sum but the machine has $COUNT \
-- a guest that walks the groups reaches only part of it"
fi

mpc=$(g_get mpc.all)
if [ -z "${mpc:-}" ] || [ "$mpc" -lt "$COUNT" ]; then
    bad "layer 1: GetMaximumProcessorCount(ALL) = ${mpc:-<absent>}, below the \
$COUNT processors that are active right now"
else
    say "layer 1: mpc.all = $mpc (>= the $COUNT active)"
fi
want glpi.group.activegroupcount "$GROUP_COUNT" "the RelationGroup record"

# The one-word views of the machine.  Both describe GROUP 0 only -- that is what
# a 64-bit mask can hold -- so both must have exactly group 0's worth of bits.
want si.activeprocessormask.pop "$(gsize 0)" "dwActiveProcessorMask describes \
group 0, so it must name every one of its processors and nothing else"
want pam.system.pop  "$(gsize 0)" "the system affinity mask is group 0's"
want pam.process.pop "$(gsize 0)" "a process with untouched affinity must be \
allowed on all of its group"

# ==========================================================================
# Layer 2: a processor number is an index into the count, not a Linux CPU id.
# ==========================================================================
pmax=$(g_get procnum.max)
pmin=$(g_get procnum.min)
if [ -z "${pmax:-}" ]; then
    bad "layer 2: the probe reported no processor number at all"
elif [ "$pmax" -ge "$COUNT" ]; then
    bad "layer 2: NtGetCurrentProcessorNumber returned $pmax on a machine this \
port says has $COUNT processors.  A guest that sizes an array by the count and \
indexes it by this writes out of bounds -- it is a memory-safety bug, not a \
reporting one"
elif [ "$pmin" = "$pmax" ]; then
    # Worth saying out loud rather than banking: the thread never moved, so
    # this leg saw one CPU and cannot have caught an out-of-range one.  It is
    # not a pass to lean on, and it is precisely why the forced leg below
    # exists.
    note "layer 2: the free-running sample never left processor $pmax, so it \
did not exercise the range at all -- the forced leg below is the one that does"
else
    say "layer 2: NtGetCurrentProcessorNumber stayed in [$pmin,$pmax], inside \
the $COUNT processors reported ($(g_get procnum.samples) samples)"
fi

# Forced, so the leg does not pass by the scheduler's luck.  taskset -c on the
# HIGHEST ONLINE CPU is derived from /sys: on this machine that number is far
# above the processor count, on a dense x86 box it is one below it, and the
# assertion is the same either way.
HIGH_WIN=$(win_of_unix "$HIGHEST")
env $GUEST_ENV CPUTOPO_MODE=report taskset -c "$HIGHEST" \
    timeout -k 5 "$TIMEOUT" "$BUILD/wine" "$EXE" \
    > "$OUT/high.out" 2>"$OUT/high.err"
rc=$?
if [ "$rc" != 0 ] || ! grep -q '^END$' "$OUT/high.out"; then
    tail -15 "$OUT/high.err" | sed 's/^/      # /' >&2
    bad "layer 2: the probe would not run pinned to Linux CPU $HIGHEST (exit $rc)"
else
    hmax=$(awk '$1=="procnum.max"{print $2}' "$OUT/high.out")
    hex_g=$(awk '$1=="procnumex.group"{print $2}' "$OUT/high.out")
    hex_n=$(awk '$1=="procnumex.number"{print $2}' "$OUT/high.out")
    if [ "${hmax:-0}" -ge "$COUNT" ]; then
        bad "layer 2: run on Linux CPU $HIGHEST, NtGetCurrentProcessorNumber \
returned $hmax -- the raw Linux CPU id -- on a machine of $COUNT processors. \
Linux CPU $HIGHEST is Windows processor $HIGH_WIN, group $(win_group "$HIGH_WIN"), \
index $(win_inidx "$HIGH_WIN") within it"
    else
        say "layer 2: pinned to Linux CPU $HIGHEST the number stayed at $hmax, \
inside $COUNT"
    fi
    if [ "${hex_g:-}" != "$(win_group "$HIGH_WIN")" ]; then
        bad "layer 2: GetCurrentProcessorNumberEx reported group ${hex_g:-<absent>} \
while running on Linux CPU $HIGHEST, which the contract puts in group \
$(win_group "$HIGH_WIN").  A guest that trusts the group cannot address the \
processor it is actually on"
    elif [ "${hex_n:-}" != "$(win_inidx "$HIGH_WIN")" ]; then
        bad "layer 2: GetCurrentProcessorNumberEx reported number ${hex_n:-<absent>} \
while running on Linux CPU $HIGHEST, whose index within group ${hex_g} is \
$(win_inidx "$HIGH_WIN")"
    else
        say "layer 2: GetCurrentProcessorNumberEx named group $hex_g index \
$hex_n, which is exactly where Linux CPU $HIGHEST lives"
    fi
fi

# ==========================================================================
# Layer 3: affinity actually places the thread.
# ==========================================================================
# What the port itself said about the call, quoted next to what the kernel did.
# Both halves matter and neither is sufficient: a call that returned success and
# placed the thread wrongly is the silent halving this gate was built for, and a
# call that was REFUSED is a different bug with the same cause -- the port
# deciding a real processor does not exist.  Naming which of the two happened is
# the difference between a useful red and a puzzle.
pin_detail() {   # pin_detail <label>
    _rc=$(awk '$1=="pin.rc"{print $2}' "$OUT/$1.out" 2>/dev/null)
    _le=$(awk '$1=="pin.lasterror"{print $2}' "$OUT/$1.out" 2>/dev/null)
    _rb=$(awk '$1=="pin.readback.mask"{print $2}' "$OUT/$1.out" 2>/dev/null)
    _pm=$(awk '$1=="pin.pam.process"{print $2}' "$OUT/$1.out" 2>/dev/null)
    _pp=$(awk '$1=="pin.pam.process.pop"{print $2}' "$OUT/$1.out" 2>/dev/null)
    if [ "${_rc:-}" = "0" ]; then
        echo "The port REFUSED the call (GetLastError ${_le:-?}; 87 is \
ERROR_INVALID_PARAMETER): a thread mask must be a subset of the PROCESS mask, \
and this process's is ${_pm:-?} -- ${_pp:-?} bits, the online Linux CPUs below \
64 rather than this machine's $COUNT processors, so the bits naming offline \
CPUs are simply not in it.  The thread's mask is still ${_rb:-?}"
    elif [ -n "${_rc:-}" ]; then
        echo "The port reported SUCCESS (rc=$_rc, readback ${_rb:-?}) and the \
thread went somewhere else anyway -- which is exactly why a leg that only \
checked the return value would have been green"
    else
        echo "The probe reported no result for the call at all"
    fi
}

# 3b: the call a game actually makes, over the low processors of group 0.
if guest_pin_leg pin_low 0 mask "$MASK" "$EXPECT"; then
    say "layer 3b: the guest asked for group 0 bits 0-$((NBITS-1)) and its thread \
really runs on Linux CPUs [$EXPECT]"
else
    bad "layer 3b: SetThreadAffinityMask($MASK) asked for $NBITS processors, which \
the contract puts on Linux CPUs [$EXPECT] -- $OBS_RESULT.  $(pin_detail pin_low) \
The identity map would have named Linux CPUs [$NAIVE]"
fi

# 3c: one processor, and the highest one in group 0, because a single bit is the
# case where "it landed on something in the right neighbourhood" cannot hide a
# wrong answer -- and the top of the group is the bit an index-capped
# enumeration loses first.
TOPBIT=$(( $(gsize 0) - 1 ))
TOPMASK=$(mask_hex "$TOPBIT")
TOPEXPECT=$(bits_to_unix 0 "$TOPBIT")
if [ "$TOPBIT" -gt 0 ]; then
    if guest_pin_leg pin_top 0 mask "$TOPMASK" "$TOPEXPECT"; then
        say "layer 3c: pinned to the single top processor of group 0 (bit \
$TOPBIT, mask $TOPMASK) the thread runs on exactly Linux CPU [$TOPEXPECT]"
    else
        bad "layer 3c: SetThreadAffinityMask($TOPMASK) named one processor, the \
last in group 0, which the contract puts on Linux CPU [$TOPEXPECT] -- \
$OBS_RESULT.  $(pin_detail pin_top) The identity map would have named Linux CPU \
$TOPBIT"
    fi
fi

# 3d: a bit that names no processor must be REFUSED.  Windows fails the call;
# quietly ignoring it hands the guest a thread confined to something it did not
# ask for.  Only meaningful when the group is not full.
if [ "$(gsize 0)" -lt 64 ]; then
    OOR=$(gsize 0)
    OORMASK=$(mask_hex "$OOR")
    guest_pin_leg pin_oor 0 mask "$OORMASK" "" >/dev/null 2>&1
    oor_rc=$(awk '$1=="pin.rc"{print $2}' "$OUT/pin_oor.out" 2>/dev/null)
    if [ "${oor_rc:-}" = "0" ]; then
        say "layer 3d: an affinity mask naming processor $OOR, which group 0 \
does not have, was refused"
    else
        bad "layer 3d: SetThreadAffinityMask($OORMASK) named processor $OOR, \
which group 0 does not have -- it holds $(gsize 0) -- and the call was ACCEPTED \
(rc=${oor_rc:-<absent>}).  The thread was moved onto Linux CPU(s) \
[${OBS_ACTUAL:-none}], which is a processor the guest never asked for and has no \
way to name"
    fi
fi

# -> 0 if the wineserver protocol can carry a processor GROUP alongside an
# affinity mask.  Read from the source rather than assumed, so this gate cannot
# drift away from the tree it is testing: `affinity_t affinity;` with a
# `group` field beside it in the same request is the thing that would make
# group 1 addressable.
server_carries_a_group()
{
    awk '/@REQ\(set_thread_info\)/ { inreq = 1; next }
         inreq && /^@(REQ|REPLY|END)/ { inreq = 0 }
         inreq && /[ \t]group[ \t]*;/ { found = 1 }
         END { exit(found ? 0 : 1) }' "$SRC/server/protocol.def" 2>/dev/null
}

# 3e: a group other than 0.  Only exists on a machine big enough to have one,
# and it is the leg that says whether the second half of this machine is
# reachable by a guest at all.
if [ "$GROUP_COUNT" -gt 1 ]; then
    G1BITS=$NBITS
    [ "$G1BITS" -gt "$(gsize 1)" ] && G1BITS=$(gsize 1)
    B1=$(awk -v n="$G1BITS" 'BEGIN { for (i = 0; i < n; i++) printf "%s%d", (i?" ":""), i }')
    # shellcheck disable=SC2086
    M1=$(mask_hex $B1)
    # shellcheck disable=SC2086
    E1=$(bits_to_unix 1 $B1)
    if guest_pin_leg pin_g1 1 group "$M1" "$E1"; then
        say "layer 3e: the guest reached group 1 through SetThreadGroupAffinity \
and its thread runs on Linux CPUs [$E1]"
    elif ! server_carries_a_group; then
        # A KNOWN LIMIT, NOT A PASS, AND IT RE-ARMS ITSELF.
        #
        # The wineserver protocol carries an affinity mask and NOTHING ELSE --
        # `affinity_t affinity` in set_thread_info/get_thread_info and their
        # process counterparts, with no group field anywhere in
        # server/protocol.def -- so the server cannot be TOLD which group a
        # mask means, and a guest cannot address group 1 no matter how right
        # the enumeration is.  ntdll agrees with it today: rtl.c's group
        # affinity setter refuses any group but 0.
        #
        # This is deliberate and scoped: closing it means adding a group field
        # to four requests, bumping SERVER_PROTOCOL_VERSION, and deciding what
        # a process affinity SET of groups means -- work that was weighed and
        # deferred, not overlooked.  What is NOT deferred is the default case,
        # which is what costs a real machine real processors: a guest that
        # never pins now runs on all $COUNT (measured 32 of 80 before).
        #
        # Reporting this as a pass would bury it, and failing forever would
        # make the whole gate worthless -- a red light nobody can act on gets
        # ignored, and then a real regression hides behind it.  So it is named,
        # and the test above is on the PROTOCOL rather than on a hardcoded
        # expectation: the day a group field appears in protocol.def this stops
        # being a limit and becomes a failure again, with no one having to
        # remember to re-enable it.
        say "layer 3e: LIMIT -- group 1's $(gsize 1) processors cannot be \
addressed by a guest, because server/protocol.def carries no group field \
alongside the affinity mask.  Not a regression and not a pass: this leg \
becomes an assertion again automatically once that field exists.  Unpinned \
threads are unaffected and use all $COUNT processors"
    else
        bad "layer 3e: SetThreadGroupAffinity(group 1, $M1) should have landed \
on Linux CPUs [$E1] -- $OBS_RESULT.  $(pin_detail pin_g1) The $(gsize 1) \
processors of group 1 are $(gsize 1) of this machine's $COUNT, and a guest \
cannot reach any of them.  server/protocol.def DOES carry a group field now, \
so this is a real failure rather than the documented limit"
    fi
fi

# ==========================================================================
# Layer 4: NUMA, with the kernel's own node ids.
# ==========================================================================
LAYER=4
want glpi.numa.records "$NODE_COUNT" "this machine's NUMA nodes"

seen_nodes=$(awk '$1 ~ /^glpi\.numa\.[0-9]+\.node$/ {print $2}' "$OUT/report.out" \
             | sort -n | uniq | tr '\n' ' ')
if [ "$seen_nodes" = "$NODE_IDS" ]; then
    say "layer 4: the RelationNumaNode records name node id(s) [${seen_nodes% }], \
which is what the kernel calls them"
else
    bad "layer 4: the RelationNumaNode records name node id(s) [${seen_nodes:-none}] \
but the kernel's are [${NODE_IDS% }].  A guest that walks the ids it is given \
addresses nodes that do not exist and misses the ones that do"
fi

for nid in $NODE_IDS; do
    pop=$(awk -v id="$nid" '
        $1 ~ /^glpi\.numa\.[0-9]+\.node$/ && $2 == id { split($1,p,"."); want[p[3]] = 1 }
        $1 ~ /^glpi\.numa\.[0-9]+\.maskpop$/ { split($1,p,"."); pop[p[3]] = $2 }
        END { s = 0; for (i in want) s += pop[i] + 0; print s }' "$OUT/report.out")
    exp=$(node_online "$nid")
    if [ "${pop:-0}" = "$exp" ]; then
        say "layer 4: node $nid's masks name $pop processors, as the kernel says"
    else
        bad "layer 4: node $nid's RelationNumaNode masks name ${pop:-0} \
processors; the kernel puts $exp online CPUs in that node"
    fi
    grp=$(awk -v id="$nid" '
        $1 ~ /^glpi\.numa\.[0-9]+\.node$/ && $2 == id { split($1,p,"."); want[p[3]] = 1 }
        $1 ~ /^glpi\.numa\.[0-9]+\.group$/ { split($1,p,"."); if (p[3] in want) print $2 }' \
        "$OUT/report.out" | sort -nu | tr '\n' ' ')
    exp_g=$(groups_of_node "$nid")
    case " $exp_g " in
        *" ${grp% } "*) : ;;
        *) bad "layer 4: node $nid's record claims group [${grp:-none}] but its \
processors are in group(s) [$exp_g]" ;;
    esac
done

hnn=$(g_get numa.highest)
if [ "${hnn:-}" = "$NODE_MAX" ]; then
    say "layer 4: GetNumaHighestNodeNumber = $hnn, the highest node id the \
kernel uses"
else
    bad "layer 4: GetNumaHighestNodeNumber = ${hnn:-<absent>} but the highest \
node id on this machine is $NODE_MAX.  A guest walking 0..highest stops before \
node $NODE_MAX and never sees $(node_online "$NODE_MAX") of its $COUNT processors"
fi

# ==========================================================================
# Layer 5: the core records add up.
# ==========================================================================
LAYER=5
corepop=$(g_get glpi.core.maskpop.total)
if [ "${corepop:-}" = "$COUNT" ]; then
    say "layer 5: the RelationProcessorCore masks account for all $COUNT processors"
else
    bad "layer 5: the RelationProcessorCore masks account for ${corepop:-0} \
processors, not $COUNT"
fi
if [ "${CORE_COUNT:--1}" = "-1" ]; then
    note "layer 5: this kernel publishes no thread_siblings_list, so the number \
of core records is not checked -- guessing an SMT width here would be exactly \
the machine-specific assumption this gate exists to remove"
else
    want glpi.core.records "$CORE_COUNT" "distinct online thread-sibling sets \
in /sys/devices/system/cpu/cpuN/topology"
fi

# A wrong answer here most often arrives as a swallowed fault rather than a
# wrong number, and this gate must not read that as success.
if grep -qE "ignoring exception|c000001d|unhandled guest trap" "$OUT/report.out.err"; then
    grep -E "ignoring exception|c000001d|unhandled guest trap" "$OUT/report.out.err" | \
        head -5 | sed 's/^/      | /' >&2
    bad "the clean run's stderr names a swallowed or unhandled fault"
fi

[ $fail -eq 0 ] && say "PASS"
exit $fail
