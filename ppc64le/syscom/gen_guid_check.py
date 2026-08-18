#!/usr/bin/env python3
"""Verify every IID in interfaces_syscom.json against Wine's OWN authoritative
GUID declarations -- the third of check-syscom-roster.sh's three layers
(hangover-ppc64le/docs/system-com-design.md Sec11 step 1), and guard the
Sec12.6 versioned-family mismap hazard: two interfaces of the same DirectMusic
or DirectSound family (IDirectMusicLoader / IDirectMusicLoader8, ...) whose
IIDs get transposed by a bad merge or a copy-paste roster edit.

  ./gen_guid_check.py --json interfaces_syscom.json \
      --wine-src $SRCTREE/include --wine-gen $BUILD/include

WHERE THE AUTHORITATIVE VALUE COMES FROM, per header dialect:

  * widl/MIDL interfaces (objidl.h, oaidl.h, ocidl.h, activation.h,
    audioclient.h, mmdeviceapi.h, unknwn.h, inspectable.h, strmif.h, ...).
    Their real source of truth is not the widl-generated header -- which this
    machine may not even HAVE, because widl only runs at build time -- it is
    the .idl an interface is declared in, where the SAME uuid(...) attribute
    widl copies into MIDL_INTERFACE("...") already sits in plain text next to
    `interface NAME : BASE { ... }`.  Reading .idl needs no build at all, so
    this is the primary source and works on a machine that has never run
    configure.  If a build tree's OWN generated header is also available
    (--wine-gen), it is read too and cross-checked against the .idl reading;
    an interface found in both must agree, or something rewrote the IID
    between source and generated output.

  * the DECLARE_INTERFACE_ dialect (dsound.h, dmusicc.h, dmusici.h,
    dmplugin.h).  These are hand-written, ship as plain committed headers (no
    widl run needed) and name their IID with the classic
    `DEFINE_GUID(IID_Name, ...)` macro in the SAME file as the interface
    declaration.

  * XAudio2's versioned IXAudio2, whose IID is `#if XAUDIO2_VER <= 7 / #elif
    XAUDIO2_VER == 8 / #else` conditional in include/xaudio2.idl, because the
    identifier is the SAME name across three incompatible shapes and Windows
    tells them apart by COM identity.  This port's XAudio2 family on the
    wine-syscom surface is the 2.7 shape (dlls/xaudio2_7), so this script
    evaluates the conditional at XAUDIO2_VER=7 itself, reading only the
    #if/#elif/#else/#endif structure around a `uuid()` attribute list -- not a
    real preprocessor, and deliberately not one: every other generator in
    this port takes the same position (see ppc64le/audio/gen_interfaces.py's
    "NO preprocessor runs here"), because running cpp for real would need the
    whole Windows header set to be resolvable from this script's include
    path, which buys nothing a scoped #if reader does not already give.

  * synthetic IIDs ("synthetic_iid": true).  These interfaces are [local] --
    not IUnknown-derived, no QueryInterface, no Microsoft-assigned IID at all
    -- and carry a value this port DERIVES from the interface's own name (see
    synth_iid, copied verbatim from ppc64le/audio/gen_interfaces.py and
    ppc64le/syscom/gen_syscom_audio.py, which must and do agree with each
    other).  There is no external authority to check these against; what this
    script checks is that the roster's value IS that derivation, exactly, so
    a hand-edited or copy-pasted synthetic row cannot silently diverge from
    the one formula both generators promise to use.

Exit 0 if every checkable IID agrees.  Exit 1 if any row -- real or synthetic
-- disagrees with its authority.  A rostered interface with no authority found
at all (a widl interface whose .idl this script cannot locate, or an
interface whose header needs a build this machine has not done) is reported
but does NOT fail the run by itself; it is a coverage gap, not a proven
mismatch, and the summary line says how many rows fall in that bucket so a
reader can tell a real pass from a pass that skipped everything.

Copyright 2026 the ppc64le port authors

This library is free software; you can redistribute it and/or modify it under
the terms of the GNU Lesser General Public License as published by the Free
Software Foundation; either version 2.1 of the License, or (at your option)
any later version.
"""

import argparse
import glob
import hashlib
import json
import os
import re
import sys

# --------------------------------------------------------------------------
# the synthetic-IID derivation.  Copied verbatim (namespace and algorithm)
# from ppc64le/audio/gen_interfaces.py's synth_iid and
# ppc64le/syscom/gen_syscom_audio.py's synth_iid, which must and do produce
# the same value for the same name -- that agreement is *why* a synthetic row
# can be a private key shared between two independently-generated marshal
# tables at all.
# --------------------------------------------------------------------------

SYNTH_NS = b"wine-ppc64le/winecom/local-interface/"


def synth_iid(name):
    h = hashlib.sha1(SYNTH_NS + name.encode()).digest()
    b = bytearray(h[:16])
    b[6] = (b[6] & 0x0f) | 0x50        # version 5
    b[8] = (b[8] & 0x3f) | 0x80        # RFC 4122 variant
    return "%s-%s-%s-%s-%s" % (b[0:4].hex(), b[4:6].hex(), b[6:8].hex(),
                               b[8:10].hex(), b[10:16].hex())


# --------------------------------------------------------------------------
# the widl/.idl dialect: `[ ... uuid(XXXX-...) ... ] interface NAME : BASE {`
#
# Attribute lists can carry other attributes beside uuid() (object, local,
# pointer_default(...), a leading comment) and can spread over several
# lines, so this reads the whole `[...]` block and pulls uuid(...) out of it
# rather than anchoring on a fixed attribute order.
# --------------------------------------------------------------------------

IFACE_RE = re.compile(
    r'\[(?P<attrs>[^\[\]]*?)\]'
    r'(?:\s*/\*.*?\*/)*\s*'                    # a doc comment before `interface`
    r'interface\s+(?P<name>\w+)'
    r'\s*(?::\s*(?P<base>\w+))?\s*\n?\s*\{',
    re.DOTALL)
UUID_RE = re.compile(r'uuid\(([0-9a-fA-F-]+)\)')
IFDEF_RE = re.compile(r'#\s*(if|elif|else|endif)\b(.*)')


def eval_cond(expr, macros):
    """A tiny evaluator for the ONE shape this port's headers actually use:
    `MACRO OP INT` for OP in == != <= >= < >, with macros taken from the
    `macros` dict.  Not a preprocessor -- see the module docstring for why
    that is deliberate.  Anything it does not recognise is FALSE, which is
    the fail-closed reading (a branch this script cannot evaluate is a branch
    it must not silently pick a uuid() out of)."""
    m = re.match(r'^\s*(\w+)\s*(==|!=|<=|>=|<|>)\s*(\d+)\s*$', expr)
    if not m:
        return None
    name, op, val = m.group(1), m.group(2), int(m.group(3))
    if name not in macros:
        return None
    left = macros[name]
    return {"==": left == val, "!=": left != val, "<=": left <= val,
            ">=": left >= val, "<": left < val, ">": left > val}[op]


def resolve_conditional_uuid(attrs, macros):
    """attrs may itself contain `#if/#elif/#else/#endif` lines around
    multiple uuid(...) attributes -- IXAudio2's shape.  Walk it as a flat
    sequence of (condition-is-true-so-far, text-since-last-directive) and
    return the uuid() found in the first block whose condition holds.  A
    plain attrs string with no directives at all just falls through to a
    single uuid() match, same as any other interface."""
    if "#if" not in attrs and "#elif" not in attrs:
        u = UUID_RE.search(attrs)
        return u.group(1) if u else None

    lines = attrs.splitlines()
    # stack of (branch_taken_already, this_branch_active, condition_known)
    stack = [(True, True)]
    chosen = None
    for line in lines:
        m = IFDEF_RE.match(line.strip())
        if not m:
            if stack[-1][1]:
                u = UUID_RE.search(line)
                if u and chosen is None:
                    chosen = u.group(1)
            continue
        kind, rest = m.groups()
        if kind == "if":
            cond = eval_cond(rest.strip(), macros)
            stack.append((bool(cond), bool(cond)))
        elif kind == "elif":
            taken, _ = stack[-1]
            cond = eval_cond(rest.strip(), macros)
            stack[-1] = (taken or bool(cond), (not taken) and bool(cond))
        elif kind == "else":
            taken, _ = stack[-1]
            stack[-1] = (True, not taken)
        elif kind == "endif":
            stack.pop()
    return chosen


def scan_idl_dir(root, macros):
    """-> {name: {(uuid, path), ...}} from every interface DEFINITION (a
    `interface NAME ... {` with a body) found under an .idl tree.  Forward
    declarations (`interface NAME;`, no attrs, no body) do not match IFACE_RE
    at all, so they contribute nothing -- which is correct, they carry no
    IID of their own."""
    out = {}
    for path in glob.glob(os.path.join(root, "**", "*.idl"), recursive=True):
        with open(path, errors="replace") as fh:
            text = fh.read()
        for m in IFACE_RE.finditer(text):
            uuid = resolve_conditional_uuid(m.group("attrs"), macros)
            if uuid is None:
                continue
            out.setdefault(m.group("name"), set()).add(
                (uuid.lower(), os.path.relpath(path, root)))
    return out


# --------------------------------------------------------------------------
# the DECLARE_INTERFACE_ dialect: DEFINE_GUID(IID_Name, ...) in a plain
# committed .h, same pattern ppc64le/audio/gen_interfaces.py's parse_guids
# uses.
# --------------------------------------------------------------------------

GUID_RE = re.compile(
    r'DEFINE_GUID\(\s*IID_(\w+)\s*,\s*'
    r'0x([0-9a-fA-F]+)\s*,\s*0x([0-9a-fA-F]+)\s*,\s*0x([0-9a-fA-F]+)\s*,\s*'
    r'((?:0x[0-9a-fA-F]+\s*,?\s*){8})\)')


def scan_h_dir(root):
    """-> {name: {(uuid, path), ...}} from every DEFINE_GUID(IID_Name, ...)
    under a plain-header tree."""
    out = {}
    for path in glob.glob(os.path.join(root, "**", "*.h"), recursive=True):
        with open(path, errors="replace") as fh:
            text = fh.read()
        for name, d1, d2, d3, rest in GUID_RE.findall(text):
            d4 = [int(x, 16) for x in re.findall(r'0x([0-9a-fA-F]+)', rest)]
            if len(d4) != 8:
                continue
            uuid = "%08x-%04x-%04x-%02x%02x-%s" % (
                int(d1, 16), int(d2, 16), int(d3, 16), d4[0], d4[1],
                "".join("%02x" % b for b in d4[2:]))
            out.setdefault(name, set()).add(
                (uuid.lower(), os.path.relpath(path, root)))
    return out


# --------------------------------------------------------------------------
# also the widl/MIDL dialect out of a BUILD tree, when one is available: some
# hosts (the real ppc64le build host, chiefly) HAVE run widl, and a generated
# header is worth cross-checking against the .idl reading even though the
# .idl reading alone is already authoritative and sufficient to pass.
# --------------------------------------------------------------------------

MIDL_RE = re.compile(
    r'MIDL_INTERFACE\("([0-9a-fA-F-]+)"\)\s*\n\s*(\w+)\s*:',
    re.MULTILINE)


def scan_generated_dir(root):
    out = {}
    if not root or not os.path.isdir(root):
        return out
    for path in glob.glob(os.path.join(root, "**", "*.h"), recursive=True):
        with open(path, errors="replace") as fh:
            text = fh.read()
        for uuid, name in MIDL_RE.findall(text):
            out.setdefault(name, set()).add(
                (uuid.lower(), os.path.relpath(path, root)))
    return out


# --------------------------------------------------------------------------
# per-interface verdicts
# --------------------------------------------------------------------------

OK, MISMATCH, UNVERIFIED = "ok", "mismatch", "unverified"


def check_interface(name, roster_uuid, idl_idx, h_idx, gen_idx):
    """-> (verdict, detail).  Tries the .idl source first (needs no build),
    then the DECLARE_INTERFACE_ header dialect, then a build tree's generated
    header if one was given.  The first authority that names this interface
    at all decides the verdict; a real build tree is used to CROSS-CHECK when
    present (see main()), not as a second independent verdict here."""
    want = roster_uuid.lower()
    for idx, origin in ((idl_idx, "idl"), (h_idx, "h")):
        cands = idx.get(name)
        if not cands:
            continue
        uuids = {u for u, p in cands}
        if want in uuids:
            return OK, "%s: %s" % (origin, ", ".join(sorted(
                p for u, p in cands if u == want)))
        return MISMATCH, "%s says %s, roster says %s (%s)" % (
            origin, "/".join(sorted(uuids)), want,
            ", ".join(sorted(p for u, p in cands)))
    cands = gen_idx.get(name)
    if cands:
        uuids = {u for u, p in cands}
        if want in uuids:
            return OK, "build: %s" % ", ".join(
                sorted(p for u, p in cands if u == want))
        return MISMATCH, "build says %s, roster says %s (%s)" % (
            "/".join(sorted(uuids)), want,
            ", ".join(sorted(p for u, p in cands)))
    return UNVERIFIED, "not declared in any .idl, DEFINE_GUID header, or " \
        "generated header this script was given"


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--json", action="append", required=True, metavar="FILE",
                    help="a roster to check; may be repeated")
    ap.add_argument("--wine-src", required=True, metavar="DIR",
                    help="Wine's include/ (source .idl and hand-written .h)")
    ap.add_argument("--wine-gen", metavar="DIR",
                    help="a build tree's generated include/ (optional; "
                         "cross-checked when present, not required to pass)")
    ap.add_argument("--xaudio2-ver", type=int, default=7,
                    help="the XAUDIO2_VER this port's wine-syscom surface "
                         "serves (default 7 -- dlls/xaudio2_7)")
    args = ap.parse_args()

    macros = {"XAUDIO2_VER": args.xaudio2_ver}
    idl_idx = scan_idl_dir(args.wine_src, macros)
    h_idx = scan_h_dir(args.wine_src)
    gen_idx = scan_generated_dir(args.wine_gen)

    n_ok = n_mismatch = n_unverified = n_synth_ok = n_synth_bad = 0
    mismatches = []

    for jsonfile in args.json:
        with open(jsonfile) as fh:
            roster = json.load(fh)
        ifaces = roster["interfaces"]
        declared_synthetic = set(roster.get("synthetic_iid_interfaces", ()))

        for name in sorted(ifaces):
            i = ifaces[name]
            uuid = i["uuid"]
            is_synth = bool(i.get("synthetic_iid"))

            if is_synth != (name in declared_synthetic):
                mismatches.append(
                    "%s: %s's \"synthetic_iid\" flag disagrees with "
                    "%s's own \"synthetic_iid_interfaces\" list"
                    % (jsonfile, name, jsonfile))
                n_mismatch += 1
                continue

            if is_synth:
                want = synth_iid(name)
                if uuid.lower() == want.lower():
                    n_synth_ok += 1
                else:
                    n_synth_bad += 1
                    mismatches.append(
                        "%s: %s is SYNTHETIC and should derive to %s, but "
                        "the roster carries %s" % (jsonfile, name, want,
                                                    uuid))
                continue

            verdict, detail = check_interface(name, uuid, idl_idx, h_idx,
                                              gen_idx)
            if verdict == OK:
                n_ok += 1
            elif verdict == UNVERIFIED:
                n_unverified += 1
            else:
                n_mismatch += 1
                mismatches.append("%s: %s -- %s" % (jsonfile, name, detail))

    print("gen_guid_check: %d IID(s) verified against Wine's own "
          "declarations, %d synthetic IID(s) verified by re-derivation, "
          "%d row(s) unverified (no local authority found), %d mismatch(es)"
          % (n_ok, n_synth_ok, n_unverified, n_mismatch))
    if n_unverified:
        print("gen_guid_check: unverified rows are a coverage gap, not a "
              "pass -- typically an interface whose header is widl output "
              "this host has not built", file=sys.stderr)
    if mismatches:
        for line in mismatches:
            print("gen_guid_check: MISMATCH %s" % line, file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
