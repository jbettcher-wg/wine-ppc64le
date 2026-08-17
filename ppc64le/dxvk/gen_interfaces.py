#!/usr/bin/env python3
"""Extract the COM vtable layout of the D3D11/D3D10/DXGI surface -- and,
with --surface d3d9, the D3D9 surface -- from DXVK's own native headers, as
the ONE table both halves of each boundary are built from.

WHY IT IS ONE FILE.  The guest x86-64 thunk module (spec2thunk COM mode, from
dlls/d3d11/d3d11.thunks) and the native module's marshal tables
(gen_winecom.py -> dlls/d3d11/d3d11_marshal.h) are emitted by two different
generators.  If they read two copies of the roster and one drifted, the
symptom would be a call dispatched to the NEIGHBOURING slot with the
neighbour's argument types -- silently, at runtime.  So there is exactly one
`interfaces_dxvk.json`, both generators read it, and libs/winecom cross-checks
the IIDs and slot counts at attach as the last line of defence.  Same
arrangement, same reason, as ppc64le/vkd3d/interfaces_d3d12.json.

WHY DXVK'S HEADERS AND NOT WINE'S.  The declarations must be the ones the
implementation was compiled from.  DXVK's native build vendors MinGW-w64/widl
output at `src/include/native/directx`; the ppc64le libdxvk_d3d11.so this port
calls was compiled against exactly those, so its vtable layout IS theirs.
Wine's own d3d11.h describes wined3d's implementation, which this lane
replaces.

SLOT ORDER IS NOT A FREE CHOICE.  It is base-interface methods first, in
declaration order, then the derived interface's own, recursively.  Getting it
wrong compiles fine and dispatches to the neighbour, so it is computed here
from the headers rather than written by hand.

TWO DIALECTS, ONE SCHEMA.  d3d11.h/dxgi.h/d3d10.h use widl's C++ dialect:
`MIDL_INTERFACE("uuid") \n Name : public Base { virtual RET STDMETHODCALLTYPE
Method(...) = 0; ... };`.  d3d9.h predates widl's MIDL_INTERFACE and uses the
older `DECLARE_INTERFACE_(Name,Base) { STDMETHOD(Method)(THIS_ args) PURE;
... };` macro dialect, with the IID given separately by a `DEFINE_GUID`
rather than an attribute on the declaration.  The two are parsed by
completely separate regexes (see IFACE_RE/METHOD_RE for the first,
IFACE_D3D9_RE/METHOD_D3D9_RE/DEFINE_GUID_RE for the second), because the
d3d9.h dialect ALSO differs in a way that changes the slot-order algorithm:
every DECLARE_INTERFACE_ body re-declares its base's methods in full (see
vtable_d3d9's docstring), where the MIDL_INTERFACE dialect declares only
each interface's own additions and relies on `: public Base` for the rest.
Both dialects still emit the same {surface, integer_types, interfaces} JSON
schema, so gen_winecom.py never has to know which one produced its input.

  ./gen_interfaces.py --json interfaces_dxvk.json     # the table
  ./gen_interfaces.py                                 # summary only
  ./gen_interfaces.py --iface ID3D11DeviceContext     # one vtable, with slots
  ./gen_interfaces.py --check interfaces_dxvk.json    # regenerate and diff
  ./gen_interfaces.py --surface d3d9 --json interfaces_d3d9.json  # D3D9

Derived from dxvk-ppc64le/thunk/gen_interfaces.py, which produced the measured
2,593-slot / 111-interface D3D11+DXGI baseline; this adds the `surface` and
`integer_types` keys the winecom generator needs, the D3D10 headers d3d10core.dll's
surface lives in, a --check mode so a stale checked-in JSON is a build
failure rather than a runtime mystery, and (see above) the D3D9 dialect for
d3d9.dll's own surface.

Copyright 2026 the ppc64le port authors

This library is free software; you can redistribute it and/or modify it under
the terms of the GNU Lesser General Public License as published by the Free
Software Foundation; either version 2.1 of the License, or (at your option)
any later version.
"""

import argparse
import json
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
HEADERS = os.path.join(HERE, "src", "include", "native", "directx")

# The D3D11 + DXGI surface, exactly the file set that produced the measured
# 2,593-slot baseline.  Order matters only for "first definition wins".
FILES_D3D11 = [
    "d3d11.h", "d3d11_1.h", "d3d11_2.h", "d3d11_3.h", "d3d11_4.h",
    "dxgi.h", "dxgi1_2.h", "dxgi1_3.h", "dxgi1_4.h", "dxgi1_5.h", "dxgi1_6.h",
]

# d3d10core.dll's surface.  DXVK's d3d10 is a thin layer over its own d3d11,
# and its interfaces are declared in these two.  Kept as a SEPARATE list so
# the delta against the D3D11 baseline is visible in the summary rather than
# folded into it.
FILES_D3D10 = ["d3d10.h", "d3d10_1.h"]

# d3d9.dll's surface.  All twenty interfaces -- including the three *Ex ones,
# which live behind a `#if !defined(D3D_DISABLE_9EX)` this generator does not
# evaluate, the same way it does not evaluate any other #ifdef -- are
# DECLARE_INTERFACE_'d in this one file.  d3d9caps.h and d3d9types.h supply
# only the enums/structs the method signatures reference, not any interface
# body, and those are already covered by scan_integer_types's sweep of every
# .h in the directory, so they do not belong in this list.
FILES_D3D9 = ["d3d9.h"]

# MIDL_INTERFACE("uuid") \n Name : public Base \n {
IFACE_RE = re.compile(
    r'MIDL_INTERFACE\("([0-9a-fA-F-]+)"\)\s*\n\s*(\w+)\s*:\s*public\s+(\w+)\s*\n\s*\{',
    re.MULTILINE,
)
# virtual RET STDMETHODCALLTYPE Name( ... ) = 0;
METHOD_RE = re.compile(
    r'virtual\s+(.+?)\s+STDMETHODCALLTYPE\s+(\w+)\s*\((.*?)\)\s*=\s*0\s*;',
    re.DOTALL,
)

# DECLARE_INTERFACE_(Name,Base) \n {   -- the d3d9.h dialect.  Bound with \b
# rather than matched bare so it cannot be fooled by DECLARE_INTERFACE(Name)
# (no base, no trailing underscore), which d3d9.h happens not to use but
# which is a real macro in the same family; a header that used it here would
# need a base to recurse into and this dialect has no way to invent one.
IFACE_D3D9_RE = re.compile(
    r'\bDECLARE_INTERFACE_\(\s*(\w+)\s*,\s*(\w+)\s*\)\s*\n\s*\{',
    re.MULTILINE,
)
# STDMETHOD(Name)(THIS_ args) PURE;            -- HRESULT-returning
# STDMETHOD_(Ret,Name)(THIS_ args) PURE;       -- Ret-returning
# STDMETHOD_(Ret,Name)(THIS) PURE;             -- no arguments
# The first capture group is "Ret,Name" or bare "Name"; a bare name means
# STDMETHOD (no trailing underscore) was used, which this dialect's own
# macro header defines to always return HRESULT.  THIS/THIS_ are the
# implicit `this` the macro inserts and are stripped here, never emitted as
# a parameter.
METHOD_D3D9_RE = re.compile(
    r'STDMETHOD_?\(([^()]*)\)\s*\(\s*THIS_?\s*(.*?)\)\s*PURE\s*;',
    re.DOTALL,
)
# DEFINE_GUID(IID_Name, l, w1, w2, b1, b2, b3, b4, b5, b6, b7, b8);  -- this
# dialect has no MIDL_INTERFACE("uuid") attribute, so the IID is wherever the
# header's author put a DEFINE_GUID for it: usually in the predeclaration
# block near the top of the file, but the three *Ex interfaces' are declared
# right next to their DECLARE_INTERFACE_.  Scanned across the WHOLE file
# before any interface body is parsed, so declaration order does not matter.
DEFINE_GUID_RE = re.compile(r'DEFINE_GUID\(IID_(\w+)\s*,\s*([^)]+)\)\s*;')

# typedef enum NAME { ... } NAME;  and  enum NAME { ... };
ENUM_RE = re.compile(r'\benum\s+(\w+)\s*\{', re.MULTILINE)
# typedef enum [TAG] { ... } NAME;  -- the typedef name, which is what the
# parameter lists actually spell; TAG (when present) is usually _NAME and is
# NOT what a signature says, so ENUM_RE alone misses these entirely.
ENUM_ANON_RE = re.compile(r'\btypedef\s+enum\s*(?:\w+\s*)?\{[^{}]*\}\s*(\w+)\s*;',
                          re.MULTILINE | re.DOTALL)
# typedef <existing> NAME;  -- one-line aliases, resolved transitively below so
# that `typedef D3D_PRIMITIVE_TOPOLOGY D3D11_PRIMITIVE_TOPOLOGY;` and
# `typedef UINT DXGI_USAGE;` both end up integer-class.
TYPEDEF_RE = re.compile(
    r'^\s*typedef\s+((?:unsigned\s+|signed\s+|const\s+)?\w+)\s+(\w+)\s*;',
    re.MULTILINE)

# The seed set of integer-class spellings a typedef chain may bottom out in.
# Kept in step with gen_winecom.py's BYVAL_INTEGER, which is the consumer.
INTEGER_SEED = set("""
    UINT INT LONG ULONG DWORD WORD BYTE BOOL WINBOOL UINT8 UINT16 UINT32
    UINT64 INT8 INT16 INT32 INT64 SIZE_T SSIZE_T ULONG64 LONG64
    unsigned int short char long
""".split())

# IUnknown is not declared in these headers; its layout is fixed and is the
# root of every vtable in the surface.
IUNKNOWN = {
    "name": "IUnknown",
    "base": None,
    "uuid": "00000000-0000-0000-c000-000000000046",
    "header": "(builtin)",
    "methods": [
        {"name": "QueryInterface", "ret": "HRESULT",
         "params": ["REFIID riid", "void **ppvObject"]},
        {"name": "AddRef", "ret": "ULONG", "params": []},
        {"name": "Release", "ret": "ULONG", "params": []},
    ],
}


def split_params(text):
    """Split a parameter list on top-level commas only."""
    text = " ".join(text.split())
    if not text or text == "void":
        return []
    out, depth, cur = [], 0, ""
    for ch in text:
        if ch in "(<[":
            depth += 1
        elif ch in ")>]":
            depth -= 1
        if ch == "," and depth == 0:
            out.append(cur.strip())
            cur = ""
        else:
            cur += ch
    if cur.strip():
        out.append(cur.strip())
    return out


def find_body_end(text, brace_pos):
    """Return the index of the matching close brace."""
    depth = 0
    for i in range(brace_pos, len(text)):
        if text[i] == "{":
            depth += 1
        elif text[i] == "}":
            depth -= 1
            if depth == 0:
                return i
    return len(text)


def parse_header(path):
    with open(path, "r", errors="replace") as fh:
        text = fh.read()

    found = []
    for m in IFACE_RE.finditer(text):
        uuid, name, base = m.group(1), m.group(2), m.group(3)
        brace = text.index("{", m.end() - 1)
        body = text[brace: find_body_end(text, brace)]

        methods = []
        for mm in METHOD_RE.finditer(body):
            ret = " ".join(mm.group(1).split())
            methods.append({
                "name": mm.group(2),
                "ret": ret,
                "params": split_params(mm.group(3)),
            })

        found.append({
            "name": name,
            "base": base,
            "uuid": uuid.lower(),
            "header": os.path.basename(path),
            "methods": methods,
        })
    enums = set(ENUM_RE.findall(text)) | set(ENUM_ANON_RE.findall(text))
    return found, enums, TYPEDEF_RE.findall(text)


def format_uuid(fields):
    """DEFINE_GUID's eleven integer fields (Data1, Data2, Data3, Data4[0..7])
    rendered in the same lowercase 8-4-4-4-12 form MIDL_INTERFACE's string
    attribute already gives every other interface in the roster, so nothing
    downstream -- dedup_iids, gen_winecom.py, libs/winecom's attach-time
    cross-check -- ever needs to know which dialect an IID came from."""
    data1, data2, data3 = fields[0], fields[1], fields[2]
    data4 = "".join("%02x" % b for b in fields[3:11])
    return "%08x-%04x-%04x-%s-%s" % (data1, data2, data3, data4[:4], data4[4:])


def parse_header_d3d9(path):
    """The DECLARE_INTERFACE_ dialect: one interface body per match, IIDs
    resolved from a separate DEFINE_GUID sweep of the whole file rather than
    from an attribute on the declaration itself (see DEFINE_GUID_RE).

    Every method in the body is kept, including the ones that re-declare an
    inherited method -- STRIPPING them here would be guessing which prefix
    is the inherited one instead of proving it, and vtable_d3d9 needs the
    full re-declared list to check the header actually did re-declare the
    base's methods unchanged, not just started an unrelated method at the
    same name."""
    with open(path, "r", errors="replace") as fh:
        text = fh.read()

    uuids = {}
    for m in DEFINE_GUID_RE.finditer(text):
        try:
            fields = [int(x.strip(), 16) for x in m.group(2).split(",")]
        except ValueError:
            continue                    # not eleven plain hex/decimal literals
        if len(fields) == 11:
            uuids[m.group(1)] = format_uuid(fields)

    found = []
    for m in IFACE_D3D9_RE.finditer(text):
        name, base = m.group(1), m.group(2)
        brace = text.index("{", m.end() - 1)
        body = text[brace: find_body_end(text, brace)]

        if name not in uuids:
            sys.exit(
                "gen_interfaces: %s has no DEFINE_GUID(IID_%s, ...) anywhere "
                "in %s -- the DECLARE_INTERFACE_ dialect has no MIDL_INTERFACE "
                "attribute to fall back on, so an interface with no "
                "DEFINE_GUID has no IID at all" % (name, name, os.path.basename(path)))

        methods = []
        for mm in METHOD_D3D9_RE.finditer(body):
            head, args = mm.group(1), mm.group(2)
            if "," in head:
                ret, mname = head.split(",", 1)
            else:
                ret, mname = "HRESULT", head
            # `struct IDirect3DDevice9 **` -- this dialect predates the
            # widl output being consistently forward-declared without the
            # tag, so parameter types may carry a leading `struct` widl's
            # dialect never does; stripped so both dialects' interface
            # pointers match the same bare name downstream.
            args = re.sub(r'\bstruct\s+', '', args)
            methods.append({
                "name": mname.strip(),
                "ret": " ".join(ret.split()),
                "params": split_params(args),
            })

        found.append({
            "name": name,
            "base": base,
            "uuid": uuids[name],
            "header": os.path.basename(path),
            "methods": methods,
        })
    return found


def scan_integer_types(header_dir):
    """Every enum and integer typedef in the whole vendored header set.

    NOT just the interface headers: `DXGI_FORMAT` is declared in
    dxgiformat.h, `DXGI_USAGE` is `typedef UINT DXGI_USAGE` in dxgitype.h,
    and `D3D11_PRIMITIVE_TOPOLOGY` is an alias of an enum in d3dcommon.h --
    all three appear as by-value parameters in d3d11.h.  A generator that
    only read the interface headers would refuse a third of the surface for
    being 'not provably integer-class', which is a true statement about the
    generator and a false one about the type.

    Typedef aliases are resolved to a fixed point, so a chain of them still
    lands on the seed set."""
    enums, aliases = set(), []
    for fn in sorted(os.listdir(header_dir)):
        if not fn.endswith(".h"):
            continue
        with open(os.path.join(header_dir, fn), errors="replace") as fh:
            text = fh.read()
        enums |= set(ENUM_RE.findall(text)) | set(ENUM_ANON_RE.findall(text))
        aliases += TYPEDEF_RE.findall(text)

    integer = set(INTEGER_SEED) | enums
    changed = True
    while changed:
        changed = False
        for src, dst in aliases:
            src = src.replace("const ", "").strip()
            if dst not in integer and src.split()[-1] in integer:
                integer.add(dst)
                changed = True
    return sorted(integer)


def build_table(header_dir, files):
    table = {"IUnknown": IUNKNOWN}
    order = ["IUnknown"]
    missing = []

    for fn in files:
        path = os.path.join(header_dir, fn)
        if not os.path.exists(path):
            missing.append(fn)
            continue
        ifaces, _, _ = parse_header(path)
        for iface in ifaces:
            if iface["name"] in table:
                continue        # first definition wins; later headers redeclare
            table[iface["name"]] = iface
            order.append(iface["name"])

    enums = scan_integer_types(header_dir) if not missing else []
    return table, order, enums, missing


def vtable(table, name, _seen=None):
    """Full slot-ordered method list: base methods first, then own."""
    _seen = _seen or set()
    if name in _seen:                       # defensive: cyclic bases
        return []
    _seen.add(name)

    iface = table.get(name)
    if iface is None:
        return None                          # base defined outside our set
    if iface["base"] is None:
        return [(name, m) for m in iface["methods"]]

    inherited = vtable(table, iface["base"], _seen)
    if inherited is None:
        return None
    return inherited + [(name, m) for m in iface["methods"]]


def dedup_iids(table, order, resolved):
    """Two spellings of one interface must not become two roster entries.

    `ID3D11Multithread` (d3d11_4.h) and `ID3D10Multithread` (d3d10.h) carry the
    SAME IID, 9b7e4e00-342c-4106-a19f-4f2704f689f0, because they are the same
    interface declared twice.  Two entries would give the guest module two stub
    arrays for it and give winecom_iface_from_iid an arbitrary winner, and
    spec2thunk refuses a roster with duplicate IIDs outright -- correctly.

    So the later declaration is dropped, but ONLY after proving the two really
    are the same vtable.  A duplicate IID over differing method lists is a
    header bug or a parse bug, and either way it stops generation: silently
    picking one would mean dispatching one interface's calls into the other's
    slots."""
    seen, dropped = {}, []
    for n in list(order):
        uuid = table[n]["uuid"]
        first = seen.get(uuid)
        if first is None:
            seen[uuid] = n
            continue
        a = [(o, m["name"]) for o, m in (resolved[first] or ())]
        b = [(o, m["name"]) for o, m in (resolved[n] or ())]
        if [x[1] for x in a] != [x[1] for x in b]:
            sys.exit("gen_interfaces: %s and %s share IID %s but declare "
                     "different vtables (%d vs %d slots).  One of them would "
                     "have to dispatch into the other's slots; refusing."
                     % (first, n, uuid, len(a), len(b)))
        dropped.append((n, first))
        order.remove(n)
        del resolved[n]
    return dropped


def build_surface(header_dir, files):
    table, order, enums, missing = build_table(header_dir, files)
    resolved = {n: vtable(table, n) for n in order}
    dropped = dedup_iids(table, order, resolved)
    return table, order, enums, missing, resolved, dropped


def build_table_d3d9(header_dir, files):
    table = {"IUnknown": IUNKNOWN}
    order = ["IUnknown"]
    missing = []

    for fn in files:
        path = os.path.join(header_dir, fn)
        if not os.path.exists(path):
            missing.append(fn)
            continue
        for iface in parse_header_d3d9(path):
            if iface["name"] in table:
                continue        # first definition wins, same rule as build_table
            table[iface["name"]] = iface
            order.append(iface["name"])

    # scan_integer_types sweeps every .h in header_dir regardless of which
    # `files` list is passed to it -- d3d9caps.h and d3d9types.h are already
    # in there whenever --headers points at the full vendored set, which is
    # why the D3D9 surface's integer_types count comes out identical to the
    # D3D11+DXGI surface's: it is the same directory-wide scan, not a
    # per-surface one.
    enums = scan_integer_types(header_dir) if not missing else []
    return table, order, enums, missing


def vtable_d3d9(table, name, _seen=None):
    """Full slot-ordered method list for the DECLARE_INTERFACE_ dialect.

    Unlike vtable() above, the body ALREADY contains the base's methods
    re-declared verbatim -- checked by hand against d3d9.h's
    IDirect3DSurface9 (: IDirect3DResource9), IDirect3DDevice9Ex
    (: IDirect3DDevice9), and IDirect3DCubeTexture9 (: IDirect3DBaseTexture9)
    bodies, all three of which open with their base's own methods in the
    base's own order before adding their own -- so the derived interface's
    own slots are exactly its body, not its base's vtable plus its body.

    What this function adds over just returning the body verbatim is
    PROOF, not convenience: it recurses into the base's own resolved
    vtable and requires the derived body's first len(base) methods to name
    the same methods in the same order.  A header that re-declared them
    out of order, dropped one, or renamed one silently would compile fine
    -- STDMETHOD does not check against the base at all -- and dispatch
    every later slot in the derived interface to its neighbour.  Owners
    are copied from the base's own resolution rather than reset to `name`,
    so a HAND_SLOTS-style "Owner::Method" key (gen_winecom.py) still
    reaches every interface that inherits the method, exactly as it does
    for the MIDL_INTERFACE dialect's vtable()."""
    _seen = _seen or set()
    if name in _seen:                       # defensive: cyclic bases
        return []
    _seen.add(name)

    iface = table.get(name)
    if iface is None:
        return None                          # base defined outside our set
    own = iface["methods"]
    if iface["base"] is None:
        return [(name, m) for m in own]

    base_full = vtable_d3d9(table, iface["base"], _seen)
    if base_full is None:
        return None
    n = len(base_full)
    if len(own) < n:
        sys.exit(
            "gen_interfaces: %s's body has only %d method(s) but its base "
            "%s has %d -- it does not fully re-declare its base's vtable, "
            "and this dialect has no `: public Base` to fall back on for "
            "the rest.  Refusing to guess which slots are missing."
            % (name, len(own), iface["base"], n))
    for i in range(n):
        if own[i]["name"] != base_full[i][1]["name"]:
            sys.exit(
                "gen_interfaces: %s re-declares slot %d as `%s` but its "
                "base %s declares `%s` there -- the header's re-declaration "
                "order does not match its base, so the rest of %s's vtable "
                "cannot be trusted to line up either.  Refusing to guess."
                % (name, i, own[i]["name"], iface["base"],
                   base_full[i][1]["name"], name))
    return base_full + [(name, m) for m in own[n:]]


def build_surface_d3d9(header_dir, files):
    table, order, enums, missing = build_table_d3d9(header_dir, files)
    resolved = {n: vtable_d3d9(table, n) for n in order}
    dropped = dedup_iids(table, order, resolved)
    return table, order, enums, missing, resolved, dropped


def emit(table, order, enums, resolved, surface):
    """The JSON both generators read.  Interfaces whose base is outside the
    parsed set carry slots=null; spec2thunk refuses those outright, and the
    summary names them, so an unresolved base can never become a silently
    short vtable."""
    return {
        "surface": surface,
        "integer_types": enums,
        "interfaces": {
            n: {
                "uuid": table[n]["uuid"],
                "base": table[n]["base"],
                "header": table[n]["header"],
                "slots": [
                    {"slot": i, "owner": o, **m}
                    for i, (o, m) in enumerate(resolved[n])
                ] if resolved[n] else None,
            }
            for n in order
        },
    }


def print_iface(name, slots):
    print("%s -- %d vtable slots" % (name, len(slots)))
    for i, (owner, m) in enumerate(slots):
        params = ", ".join(m["params"]) or "void"
        mark = "  " if owner == name else " ^"
        print("%4d%s %-10s %s(%s)" % (i, mark, m["ret"], m["name"], params))


def finish(args, out):
    """--check / --json handling, identical for every surface: the JSON
    schema is the same regardless of which dialect produced it, so the two
    gates that consume it (a stale checked-in file, or writing a fresh one)
    do not need to know either."""
    if args.check:
        with open(args.check) as fh:
            have = json.load(fh)
        if have == out:
            print("\ncheck passed: %s matches the headers" % args.check)
            return 0
        sys.exit("\ngen_interfaces: %s has DRIFTED from the headers at the "
                 "pinned commit.\n  Regenerate it (--json) and re-run every "
                 "gate -- a roster the two generators disagree about\n  "
                 "dispatches to the neighbouring slot, which nothing else "
                 "catches until attach." % args.check)

    if args.json:
        with open(args.json, "w") as fh:
            json.dump(out, fh, indent=2)
            fh.write("\n")
        print("\nwrote %s" % args.json)
    return 0


def main_d3d9(args):
    table, order, enums, missing, resolved, dropped = build_surface_d3d9(
        args.headers, FILES_D3D9)

    if missing:
        sys.exit("gen_interfaces: %d header(s) not found under %s: %s\n"
                 "  Run ppc64le/dxvk/bootstrap.sh -- src/ is the pinned "
                 "upstream checkout and is gitignored."
                 % (len(missing), args.headers, ", ".join(missing)))

    if args.iface:
        slots = vtable_d3d9(table, args.iface)
        if slots is None:
            sys.exit("%s: not found, or inherits from an interface outside "
                     "the parsed set" % args.iface)
        print_iface(args.iface, slots)
        return 0

    unresolved = sorted(n for n, v in resolved.items() if v is None)
    total = sum(len(v) for v in resolved.values() if v)

    print("surface           : %s" % args.surface)
    print("interfaces parsed : %d (%d with a resolved vtable)"
          % (len(order), len(order) - len(unresolved)))
    print("vtable slots      : %d" % total)
    print("integer types     : %d (enums + resolved typedef aliases)"
          % len(enums))
    if unresolved:
        print("unresolved bases  : %d (%s)"
              % (len(unresolved), ", ".join(unresolved)))
    for n, first in dropped:
        print("duplicate IID     : %s dropped -- same IID and same vtable as %s"
              % (n, first))

    return finish(args, emit(table, order, enums, resolved, args.surface))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--headers", default=HEADERS)
    ap.add_argument("--json", metavar="FILE", help="write the table")
    ap.add_argument("--check", metavar="FILE",
                    help="regenerate and compare against FILE; nonzero if it "
                         "has drifted from the headers")
    ap.add_argument("--iface", metavar="NAME",
                    help="print one interface's full vtable")
    ap.add_argument("--surface", default="dxvk",
                    help="dxvk (default, D3D11+DXGI[+D3D10]) or d3d9")
    ap.add_argument("--no-d3d10", action="store_true",
                    help="D3D11+DXGI only -- the 2,593-slot baseline set")
    args = ap.parse_args()

    if args.surface == "d3d9":
        return main_d3d9(args)

    files = FILES_D3D11 if args.no_d3d10 else FILES_D3D11 + FILES_D3D10
    table, order, enums, missing, resolved, dropped = build_surface(
        args.headers, files)

    if missing:
        sys.exit("gen_interfaces: %d header(s) not found under %s: %s\n"
                 "  Run ppc64le/dxvk/bootstrap.sh -- src/ is the pinned "
                 "upstream checkout and is gitignored."
                 % (len(missing), args.headers, ", ".join(missing)))

    if args.iface:
        slots = vtable(table, args.iface)
        if slots is None:
            sys.exit("%s: not found, or inherits from an interface outside "
                     "the parsed set" % args.iface)
        print_iface(args.iface, slots)
        return 0

    unresolved = sorted(n for n, v in resolved.items() if v is None)
    total = sum(len(v) for v in resolved.values() if v)

    # The measured baseline this surface must not silently shrink below.
    base_table, base_order, _, _, base_resolved, _ = build_surface(
        args.headers, FILES_D3D11)
    base_slots = sum(len(v) for v in base_resolved.values() if v)
    base_ifaces = sum(1 for v in base_resolved.values() if v)

    print("surface           : %s" % args.surface)
    print("interfaces parsed : %d (%d with a resolved vtable)"
          % (len(order), len(order) - len(unresolved)))
    print("vtable slots      : %d" % total)
    print("integer types     : %d (enums + resolved typedef aliases)"
          % len(enums))
    print("D3D11+DXGI subset : %d interface(s), %d slot(s)   "
          "[baseline: 111 / 2593]" % (base_ifaces, base_slots))
    if (base_ifaces, base_slots) != (111, 2593):
        print("  NOTE: the D3D11+DXGI subset has moved off the measured "
              "dxvk-ppc64le baseline of 111 interfaces / 2593 slots.")
    if not args.no_d3d10:
        print("D3D10 extension   : +%d interface(s), +%d slot(s)"
              % (len(order) - len(base_order), total - base_slots))
    if unresolved:
        print("unresolved bases  : %d (%s)"
              % (len(unresolved), ", ".join(unresolved)))
    for n, first in dropped:
        print("duplicate IID     : %s dropped -- same IID and same vtable as %s"
              % (n, first))

    return finish(args, emit(table, order, enums, resolved, args.surface))


if __name__ == "__main__":
    sys.exit(main())
