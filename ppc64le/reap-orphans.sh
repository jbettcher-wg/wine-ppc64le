#!/bin/bash
# Kill Wine processes that no wineserver owns any more.
#
# Copyright 2026 the ppc64le port authors
#
# This library is free software; you can redistribute it and/or modify it under
# the terms of the GNU Lesser General Public License as published by the Free
# Software Foundation; either version 2.1 of the License, or (at your option)
# any later version.
#
# ---------------------------------------------------------------------------
#
# WHY THIS EXISTS.  A Wine session leaves service processes behind -- services,
# winedevice, svchost, plugplay, rpcss, explorer -- and when its wineserver goes
# away they are reparented to init and never exit.  They do not go quietly
# either: [MEASURED] 2026-08-18, op4k, after a day of gate suites and agent
# runs, 54 orphaned winedevice.exe were resident, each having accumulated close
# to a full core of CPU time, on a machine with 80.  Another 72 orphaned
# svchost/plugplay processes sat idle alongside them.  Load average 16 with
# nothing running.
#
# The wedge is visible in wchan: `ntsync_schedule`, state S, blocked in the
# ntsync driver on a sync object whose server is gone and which nothing will
# ever signal.  That is also why they ignore SIGTERM -- the signal is delivered
# to a process whose Wine-side loop is never scheduled again -- and why some of
# them need SIGKILL.
#
# A gate suite starts and ends dozens of sessions, so this accumulates fast
# enough to matter within an afternoon.
#
# SAFETY.  This only touches a process that is ALL of:
#   * running a Windows image (its argv names a .exe under C:\ or Z:\),
#   * reparented to init (PPID 1), which is what "no session owns me" looks
#     like, and
#   * not covered by any running wineserver.
# and it kills by EXACT PID.  `pkill -f` is never used here: the pattern would
# also match an editor, a grep, or somebody's game.
#
# The wineserver check is deliberately whole-machine rather than per-prefix: a
# server for prefix A cannot own an orphan from prefix B, but working that out
# per process means reading every server's prefix, and refusing to act while
# ANY server runs is the conservative direction.  Pass --force to skip it when
# you know the running servers are unrelated.

set -u

FORCE=0
DRY=0
for a in "$@"; do
    case "$a" in
        --force) FORCE=1 ;;
        -n|--dry-run) DRY=1 ;;
        -h|--help)
            echo "usage: $0 [--force] [-n|--dry-run]"
            echo "  kills Wine processes reparented to init that no wineserver owns"
            exit 0 ;;
        *) echo "reap-orphans: unknown argument $a" >&2; exit 2 ;;
    esac
done

say() { echo "reap-orphans: $*"; }

if [ "$FORCE" = 0 ] && pgrep -x wineserver >/dev/null 2>&1; then
    say "a wineserver is running; refusing to act (use --force if it is unrelated)"
    pgrep -x wineserver | while read -r s; do
        say "  wineserver $s"
    done
    exit 2
fi

# Collect first, act second: the list must not change under us while we walk it.
orphans=""
count=0
while read -r pid rest; do
    [ -d "/proc/$pid" ] || continue
    case "$rest" in
        *'C:\'*.exe*|*'Z:\'*.exe*|*/wineserver) ;;
        *) continue ;;
    esac
    ppid=$(ps -o ppid= -p "$pid" 2>/dev/null | tr -d ' ')
    [ "$ppid" = "1" ] || continue
    orphans="$orphans $pid"
    count=$((count + 1))
done <<EOF
$(ps -eo pid= -o args=)
EOF

if [ "$count" = 0 ]; then
    say "nothing to do"
    exit 0
fi

say "$count orphaned Wine process(es)"
for pid in $orphans; do
    nm=$(ps -o args= -p "$pid" 2>/dev/null | grep -oE '[A-Za-z0-9_.-]+\.exe' | head -1)
    t=$(ps -o time= -p "$pid" 2>/dev/null | tr -d ' ')
    say "  $pid ${nm:-?} cpu=$t"
done

if [ "$DRY" = 1 ]; then
    say "dry run; nothing killed"
    exit 0
fi

for pid in $orphans; do kill "$pid" 2>/dev/null; done
sleep 3

# SIGKILL for the ones wedged in ntsync, which never run their handler.
left=0
for pid in $orphans; do
    [ -d "/proc/$pid" ] || continue
    kill -9 "$pid" 2>/dev/null
    left=$((left + 1))
done
[ "$left" -gt 0 ] && { sleep 2; say "$left needed SIGKILL (wedged in ntsync)"; }

still=0
for pid in $orphans; do [ -d "/proc/$pid" ] && still=$((still + 1)); done
if [ "$still" -gt 0 ]; then
    say "FAILED: $still process(es) survived SIGKILL"
    exit 1
fi
say "reaped $count"
exit 0
