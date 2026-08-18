#!/usr/bin/env python3
"""Emit a C probe that proves interfaces_syscom.json's slot layout against
Wine's OWN compiled C vtbl structs -- the second of check-syscom-roster.sh's
three layers (hangover-ppc64le/docs/system-com-design.md Sec11 step 1).

  ./gen_vtbl_check.py --half wine-syscom --json interfaces_syscom.json > probe.c
  clang -target x86_64-windows-gnu -fsyntax-only -nostdlibinc \
      -Werror=incompatible-pointer-types \
      -Werror=incompatible-function-pointer-types \
      -I$BUILD/include -I$SRCTREE/include -I$SRCTREE/include/msvcrt probe.c

WHY A COMPILED PROBE RATHER THAN A SECOND PARSE OF THE HEADERS.  Parsing the
headers a second time here would only prove the roster agrees with THIS
script's reading of them -- exactly the mistake that let the roster and the
real vtables drift apart in the first place, since nothing would ever bounce
the drift off the compiler that actually lays the struct out.  So this script
does not classify a single type.  For every slot in the roster it emits:

  * an offsetof() _Static_assert -- the slot's declared index times
    sizeof(void*) must equal the struct member's real offset, which fails
    the moment two methods trade places or a slot is inserted/deleted upstream
    of one that was not renumbered here;
  * a typed assignment from the Vtbl member into a function-pointer variable
    declared with the roster's OWN return type and parameter types.  The
    gate's -Werror=incompatible-(function-)pointer-types turns any signature
    mismatch -- a changed parameter type, an extra argument, a different
    calling convention -- into a hard compile error.  Getting a slot's NAME
    right but its SIGNATURE wrong compiles fine as an offsetof assertion and
    fails here.

Both checks read the struct Wine's own widl output (or, for the two
DECLARE_INTERFACE_ dialect families, Wine's own macro expansion) actually
produced; nothing here re-derives what "correct" means.

WHICH HEADER PER INTERFACE.  Each roster row already carries the header it was
extracted from (interfaces_syscom.json's "header" field), because
ppc64le/audio/gen_interfaces.py and ppc64le/syscom/gen_syscom_audio.py record
it for exactly this reason. This script only adds the two facts a compiler
needs that a roster row does not carry on its own:

  * IUnknown's row says header "(builtin)" -- it predates every widl run and
    is declared by Wine's own unknwn.h, not extracted from an interface's own
    header;
  * xaudio_classes.h is build output that lands under dlls/xaudio2_7/, not
    under an include/ directory a plain -I finds; the gate script adds that
    one extra -I for this reason.

--half exists to catch the one mistake that would silently pass everything
else: running this generator against the wrong surface's JSON (say, an
xaudio2_9 audio-family roster fed to a slot check meant for wine-syscom). It
must equal the roster's own "surface" field or the probe is refused before a
single line of C is emitted.

Copyright 2026 the ppc64le port authors

This library is free software; you can redistribute it and/or modify it under
the terms of the GNU Lesser General Public License as published by the Free
Software Foundation; either version 2.1 of the License, or (at your option)
any later version.
"""

import argparse
import json
import re
import sys

# --------------------------------------------------------------------------
# headers that need an include path beyond the ordinary -I$SRCTREE/include /
# -I$BUILD/include the gate already passes.  Keyed by the roster's "header"
# field.  "(builtin)" is not a file at all -- see HEADER_FOR below.
# --------------------------------------------------------------------------

EXTRA_HEADER_FOR = {
    "(builtin)": "unknwn.h",
}

# PREREQUISITE ORDER.  This probe does something no real Wine consumer ever
# does: it #includes every family on the wine-syscom surface in ONE
# translation unit.  A real consumer never pays for that, because
# dlls/dmusic/dmusic_private.h reaches dmusicc.h (via dmusici.h) and never
# also reaches audioclient.h in the same file -- so it never notices that the
# two disagree about how much of REFERENCE_TIME to declare.
#
# THE COLLISION, measured on the ppc64le host: include/dmdls.h guards its
# typedef with `#ifndef REFERENCE_TIME_DEFINED` and provides BOTH REFERENCE_
# TIME and LPREFERENCE_TIME under it; include/ksmedia.h guards the SAME macro
# but provides only REFERENCE_TIME.  audioclient.h (widl output of
# audioclient.idl, which cpp_quotes `#include <ksmedia.h>`) therefore wins
# the guard race if it is #included before dmusicc.h, and every LPREFERENCE_
# TIME-typed slot in dmusicc.h -- TotalTime, GetNextEvent, GetStartTime --
# then fails with "unknown type name" instead of a slot mismatch.  Sorting
# headers alphabetically put audioclient.h before dmusicc.h and hit exactly
# this.
#
# THE FIX is a general rule, not a special case for REFERENCE_TIME: the hand-written DECLARE_INTERFACE_ family (dsound.h,
# dmusicc.h, dmusici.h, dmplugin.h) is SELF-CONTAINED -- each already
# #includes its own prerequisites, including each other, in the order a real
# consumer would -- so giving that whole family priority over every widl-
# dialect header reproduces a real consumer's ordering inside this probe's
# single TU: the full definition is always declared before anything that
# would settle for the partial one. A header not named here sorts after the
# family, in the widl dialect's own bucket, alphabetically.
# ... and the family is ITSELF order-sensitive: dmusici.h's packed structs
# use MUSIC_TIME and IDirectMusicTool from dmusicc.h, and dmplugin.h includes
# dmusici.h expecting the same, so the list below is DEPENDENCY order and the
# sort key preserves list position rather than sorting alphabetically.
DECLARE_INTERFACE_HEADERS = ["dsound.h", "dmusicc.h", "dmusici.h", "dmplugin.h"]


def header_sort_key(h):
    if h in DECLARE_INTERFACE_HEADERS:
        return (0, DECLARE_INTERFACE_HEADERS.index(h), h)
    return (1, 0, h)

# The struct/typedef name a header's C-dialect vtbl carries for interface
# NAME.  Every dialect this roster's headers use -- widl's MIDL_INTERFACE
# output and the hand-written DECLARE_INTERFACE_ family (dsound.h, dmusicc.h,
# dmusici.h, dmplugin.h) -- names it "<Name>Vtbl" in C mode (no __cplusplus,
# no CINTERFACE needed: DECLARE_INTERFACE_ and widl's C branch both emit that
# struct unconditionally).  If a future header dialect names it differently,
# this is the one place to teach the difference.
def vtbl_type(name):
    return "%sVtbl" % name


IDENT_RE = re.compile(r'^\w+$')


def strip_param_name(p):
    """A roster param string is `TYPE [*...] name[[N]]`.  Return just the
    type (with its stars), the way a function-pointer typedef needs it.  A
    trailing `[...]` is an array parameter, which decays to a pointer; the
    bracket and whatever is inside it is dropped and one more `*` is added
    the same way the C standard would."""
    p = " ".join(p.split())
    array = False
    m = re.match(r'^(.*?)\s*\[[^\]]*\]$', p)
    if m:
        p, array = m.group(1), True
    if p in ("void", "..."):
        return p
    m = re.match(r'^(.*[\*\s])(\w+)$', p)
    if not m:
        # No trailing identifier to strip -- either already a bare type, or a
        # shape this generator has not seen.  Emitting it unchanged is the
        # fail-open choice for offsetof, and the compiler is the backstop for
        # the typed check: an unresolvable fragment is a compile error, not a
        # silently accepted slot.
        t = p
    else:
        t = m.group(1).rstrip()
    if array:
        t += " *" if not t.endswith("*") else "*"
    return t


def fnptr_decl(ret, iface, params, varname):
    """A full C declaration of a function-pointer variable with the roster's
    own return type and parameter types -- not a `TYPE name` pair, because a
    function pointer's name goes INSIDE the declarator, between the `*` and
    the parameter list: `RET (CONV *name)(args)`.  Getting this wrong is a
    syntax error, not a signature mismatch, which is exactly the bug a first
    draft of this generator had and why this is spelled out."""
    ptypes = [strip_param_name(p) for p in params]
    args = ", ".join(["%s *" % iface] + ptypes) if ptypes else "%s *" % iface
    return "%s (STDMETHODCALLTYPE *%s)(%s)" % (ret, varname, args)


def build_probe(roster, half):
    if roster.get("surface") != half:
        sys.exit("gen_vtbl_check: --half %s does not match this roster's own "
                 "surface %r -- refusing to check the wrong surface's JSON"
                 % (half, roster.get("surface")))

    ifaces = roster["interfaces"]
    seen_headers = set()
    for name in sorted(ifaces):
        h = ifaces[name]["header"]
        h = EXTRA_HEADER_FOR.get(h, h)
        seen_headers.add(h)
    # Prerequisite family first (see header_sort_key), alphabetical within
    # each bucket for a deterministic, reviewable diff.
    headers = sorted(seen_headers, key=header_sort_key)

    out = []
    out.append("/* GENERATED by gen_vtbl_check.py -- do not edit.")
    out.append(" *")
    out.append(" * Slot-layout probe for the %s surface (%d interfaces).  Every"
              % (half, len(ifaces)))
    out.append(" * _Static_assert below checks one slot's offset; every")
    out.append(" * check_<Iface> function checks every slot's full typed")
    out.append(" * signature by assignment.  Compiling this file with")
    out.append(" * -Werror=incompatible-(function-)pointer-types IS the check:")
    out.append(" * a mismatch of order, count or signature is a compile error,")
    out.append(" * not a diagnostic this script prints itself. */")
    out.append("")
    out.append("#include <stddef.h>")
    for h in headers:
        out.append("#include <%s>" % h)
    out.append("")

    n_asserts = 0
    n_typed = 0
    for name in sorted(ifaces):
        i = ifaces[name]
        vt = vtbl_type(name)
        slots = i["slots"]
        out.append("/* %s : %s  {%s}%s */"
                  % (name, i["base"], i["uuid"],
                     "  SYNTHETIC IID, [local]" if i.get("synthetic_iid")
                     else ""))
        out.append('_Static_assert(sizeof(%s) == %d * sizeof(void *), '
                  '"%s: vtable has the wrong slot count");'
                  % (vt, len(slots), name))
        n_asserts += 1
        for s in slots:
            out.append(
                '_Static_assert(offsetof(%s, %s) == %d * sizeof(void *), '
                '"%s::%s is not at slot %d");'
                % (vt, s["name"], s["slot"], name, s["name"], s["slot"]))
            n_asserts += 1
        out.append("static void __syscom_vtbl_check_%s(void)" % name)
        out.append("{")
        out.append("    const %s *v = (const %s *)0;" % (vt, vt))
        for s in slots:
            varname = "p_%s" % s["name"]
            decl = fnptr_decl(s["ret"], name, s["params"], varname)
            out.append("    %s = v->%s; (void)%s;"
                      % (decl, s["name"], varname))
            n_typed += 1
        out.append("}")
        out.append("")

    out.append("int main(void) { return 0; }")
    return "\n".join(out) + "\n", n_asserts, n_typed


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--half", required=True,
                    help="the roster's own \"surface\" field, as a guard "
                         "against feeding this the wrong JSON")
    ap.add_argument("--json", required=True, metavar="FILE",
                    help="the roster to emit a probe for")
    args = ap.parse_args()

    with open(args.json) as fh:
        roster = json.load(fh)

    text, n_asserts, n_typed = build_probe(roster, args.half)
    sys.stdout.write(text)
    print("gen_vtbl_check: %d interface(s), %d _Static_assert(s), %d typed "
          "slot check(s)" % (len(roster["interfaces"]), n_asserts, n_typed),
          file=sys.stderr)
    return 0


if __name__ == "__main__":
    sys.exit(main())
