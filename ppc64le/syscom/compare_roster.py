#!/usr/bin/env python3
"""Compare two wine-syscom rosters for the differences that can dispatch a
call to the wrong place, and count the ones that cannot.

    ./compare_roster.py committed.json regenerated.json

Exit 0 when they agree, 1 when they do not, and print every disagreement.

WHAT IS COMPARED, AND WHY THAT LIST.  A roster row is a vtable: an IID, a
number of slots, and for each slot a method and a return type.  Get any of
those wrong between the two generators that read this file -- the guest stub
array and the native marshal table -- and a call lands on the NEIGHBOURING
slot with the neighbour's argument types, silently, at runtime.  That is the
whole failure mode this gate exists for, so that is the list:

  * the set of interfaces;
  * each interface's IID (and whether it is one of the synthetic ones);
  * each interface's slot COUNT;
  * each slot's method NAME and RETURN TYPE, in slot order.

WHAT IS NOT COMPARED, AND WHY NOT.  Parameter spelling and the `owner` field
are reported as counts rather than failures.  The committed roster was written
by a generator that no longer exists, and it spells both differently from the
one in this tree: it resolved LP-typedefs, so it writes `DMUS_PORTCAPS
*pPortCaps` where dmusicc.h says `LPDMUS_PORTCAPS pPortCaps`, and it names
each interface as the owner of its own inherited IUnknown slots where this
tree's parser reads the `/*** IUnknown methods ***/` comment above them.
Those are the same types and the same methods written two ways.  Demanding one
spelling would make this a test of a dead tool's habits.

Parameter LAYOUT is not going unchecked: layer 2 of check-syscom-roster.sh
compiles 884 static assertions about it against Wine's own headers, which is a
stronger statement than string equality between two spellings would be.  The
counts are printed anyway, because a spelling difference that suddenly grows is
worth a human's attention even when it is not worth failing over.

Copyright 2026 the ppc64le port authors

This library is free software; you can redistribute it and/or modify it under
the terms of the GNU Lesser General Public License as published by the Free
Software Foundation; either version 2.1 of the License, or (at your option)
any later version.
"""

import json
import sys


def load(path):
    with open(path) as fh:
        return json.load(fh)


def main():
    if len(sys.argv) != 3:
        sys.exit("usage: compare_roster.py COMMITTED REGENERATED")
    a = load(sys.argv[1])["interfaces"]
    b = load(sys.argv[2])["interfaces"]
    bad = []
    owner_diffs = params_diffs = 0

    only_a = sorted(set(a) - set(b))
    only_b = sorted(set(b) - set(a))
    for n in only_a:
        bad.append("%s is in the committed roster and not in a regeneration" % n)
    for n in only_b:
        bad.append("%s is in a regeneration and not in the committed roster" % n)

    for n in sorted(set(a) & set(b)):
        x, y = a[n], b[n]
        if x["uuid"] != y["uuid"]:
            bad.append("%s: IID %s committed, %s regenerated" % (n, x["uuid"], y["uuid"]))
        if bool(x.get("synthetic_iid")) != bool(y.get("synthetic_iid")):
            bad.append("%s: synthetic_iid %r committed, %r regenerated"
                       % (n, bool(x.get("synthetic_iid")), bool(y.get("synthetic_iid"))))
        if len(x["slots"]) != len(y["slots"]):
            bad.append("%s: %d slots committed, %d regenerated"
                       % (n, len(x["slots"]), len(y["slots"])))
            continue
        for xs, ys in zip(x["slots"], y["slots"]):
            if xs["name"] != ys["name"]:
                bad.append("%s slot %d: %s committed, %s regenerated"
                           % (n, xs["slot"], xs["name"], ys["name"]))
            if xs["ret"] != ys["ret"]:
                bad.append("%s::%s: returns %s committed, %s regenerated"
                           % (n, xs["name"], xs["ret"], ys["ret"]))
            if xs["owner"] != ys["owner"]:
                owner_diffs += 1
            if xs["params"] != ys["params"]:
                params_diffs += 1

    print("compare_roster: %d interface(s), %d slot(s) compared by name and "
          "return type" % (len(set(a) & set(b)),
                           sum(len(a[n]["slots"]) for n in set(a) & set(b))))
    print("compare_roster: %d slot(s) differ only in the owner field and %d "
          "only in parameter spelling -- not compared, see this file's "
          "docstring" % (owner_diffs, params_diffs))
    for line in bad:
        print("compare_roster: DRIFT %s" % line, file=sys.stderr)
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
