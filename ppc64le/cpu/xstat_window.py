#!/usr/bin/env python3
"""xstat_window.py -- the crossing table for ONE WINDOW of a run.

WINE_PPC64LE_TRAP_STATS counts from a process's first guest crossing to its
last, so its per-second column is diluted by everything that is not the part
being measured: a Cyberpunk `-benchmark` process lives ~168 s and flies the
65 s route inside it.  Rates over the whole life understate the flythrough by
about 2.5x and, worse, understate it UNEVENLY -- loading is heavy on file and
descriptor traffic, the flythrough on draw calls and time queries.

So the sink rewrites its file periodically (and on SIGUSR2), and this
subtracts one dump from a later one: counts and per-second rates for exactly
the interval between them.  Rows that appear only in the later file are new
call sites and count in full; rows that stop being crossed drop out.

    xstat_window.py EARLY LATE [-n TOP]
"""

import re
import sys

HDR = re.compile(r"^# .*?, ([0-9.]+) s of process life")


def read(path):
    rows = {}
    secs = None
    for line in open(path):
        m = HDR.match(line)
        if m:
            secs = float(m.group(1))
            continue
        if line.startswith("#") or not line.strip():
            continue
        f = line.split(None, 3)
        if len(f) < 4 or f[0] == "class":
            continue
        rows[(f[0], f[3].rstrip("\n"))] = int(f[1])
    if secs is None:
        raise SystemExit("%s: no header line with an elapsed time" % path)
    return secs, rows


def main():
    argv = sys.argv[1:]
    top = 40
    if "-n" in argv:
        i = argv.index("-n")
        top = int(argv[i + 1])
        del argv[i:i + 2]
    args = [a for a in argv if not a.startswith("-")]
    if len(args) != 2:
        raise SystemExit(__doc__)

    t0, early = read(args[0])
    t1, late = read(args[1])
    span = t1 - t0
    if span <= 0:
        raise SystemExit("the second file must be the later dump (%.2f -> %.2f)" % (t0, t1))

    delta = {}
    for key, n in late.items():
        d = n - early.get(key, 0)
        if d > 0:
            delta[key] = d

    per_class = {}
    for (cls, _), d in delta.items():
        per_class[cls] = per_class.get(cls, 0) + d
    total = sum(delta.values())

    print("# crossing frequency over a %.2f s window (%.2f s -> %.2f s of process life)"
          % (span, t0, t1))
    print("# %d crossings in %d named rows" % (total, len(delta)))
    for cls in sorted(per_class, key=lambda c: -per_class[c]):
        print("# class %-8s %14d  %12.0f/s" % (cls, per_class[cls], per_class[cls] / span))
    print()
    print("%-8s %14s %12s  %s" % ("class", "count", "per-sec", "name"))
    for (cls, name), d in sorted(delta.items(), key=lambda kv: -kv[1])[:top]:
        print("%-8s %14d %12.0f  %s" % (cls, d, d / span, name))


main()
