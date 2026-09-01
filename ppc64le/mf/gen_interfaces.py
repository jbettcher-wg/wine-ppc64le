#!/usr/bin/env python3
"""Extract the Media Foundation interface roster from Wine's OWN headers.

  ./gen_interfaces.py --json interfaces_mf.json          # write the roster
  ./gen_interfaces.py --check interfaces_mf.json         # regenerate and diff
  ./gen_interfaces.py --report                           # what was taken, what was dropped

OUTPUT is interfaces_mf.json -- the ONE roster for the Media Foundation
surface, and the same file three things read:

  * dlls/mfplat/mfplat.thunks, dlls/mf/mf.thunks and
    dlls/mfreadwrite/mfreadwrite.thunks name it as COM-JSON, so spec2thunk
    builds each guest module's per-interface trap-stub arrays from it;
  * ppc64le/mf/gen_winecom.py turns it into dlls/mfplat/mf_marshal.h, the
    marshal tables the native side dispatches with;
  * libs/winecom cross-checks every IID and slot count of every loaded guest
    module against those tables at attach.

Two generators reading one file is the whole safety argument: if the guest
module's interface indices and the native marshal table's disagreed by one, a
call would land on the neighbouring slot with the neighbour's argument types.
Both sort by interface name, and the attach-time cross-check is the last line
of defence.

WHY THE INPUT IS WINE'S GENERATED HEADERS rather than the .idl files.  The
implementation being served IS Wine's mfplat/mf/mfreadwrite, compiled from
those very headers, so the vtable layout in `include/mfobjects.h` is by
construction the layout of the objects the guest will be calling.  Parsing the
.idl would reintroduce widl's own inheritance flattening as something this
script had to reimplement and could get wrong.

WHAT IS DROPPED, AND WHY EACH IS A REFUSAL RATHER THAN AN OVERSIGHT.  An
interface is taken only if its ENTIRE base chain is present in the parsed
header set: a vtable whose base slots this script never saw has unknown slot
NUMBERS, and a table with the wrong slot numbers is exactly the silent
mis-dispatch the design exists to prevent.  So `IMFPMPHost` (base
`IUnknown`, fine) is taken and anything rooted in `IPropertyStore` or
`IPersistStream` is dropped by name, with the reason in --report.

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
SRCTREE = os.path.abspath(os.path.join(HERE, "..", ".."))

# The headers whose interfaces make up this surface.  mfobjects/mfidl are the
# platform and the pipeline, mfreadwrite is IMFSourceReader/IMFSinkWriter, and
# mftransform is IMFTransform -- the decoder object a game that drives an MFT
# by hand reaches.  Nothing else is included: an interface that is not here
# gets no proxy vtable, and QueryInterface for it is refused loudly rather
# than answered with a native vtable (libs/winecom.c proxy_qi).
HEADERS = ("mfobjects.h", "mfidl.h", "mfreadwrite.h", "mftransform.h",
           # The three modules beside mfplat that a game's media code reaches.
           # They join THIS roster rather than getting one each, because
           # libs/winecom's state is per-linkee and there is exactly one
           # instance for the whole surface (native mfplat.dll owns it, the
           # others forward __wine_com_dispatch into it) -- so a proxy minted
           # for an IMFMediaType by mfplat has to be usable as an argument to
           # an evr method, and it can only be that if both guest modules
           # publish the same vtable for it.  Splitting the roster would put
           # an IMFMediaType proxy from one surface into another's dispatch
           # loop, which is the cross-surface refusal ppc64le/mf/README.md
           # already documents for combase's IStream.
           #
           #   mfmediaengine  IMFMediaEngine and friends -- the HTML5-shaped
           #                  player a game's video plugin uses.
           #   evr            the enhanced video renderer's control surface:
           #                  IMFVideoDisplayControl is what a game calls to
           #                  place and letterbox a cutscene.
           #   wmsdkidl       wmvcore's IWMReader/IWMSyncReader/IWMProfile --
           #                  Windows Media, which older titles still ship.
           #   wmsbuffer      INSSBuffer and family -- the WMSDK sample
           #                  buffer every reader callback and allocator
           #                  traffics in.  Its absence refused seven slots
           #                  (GetNextSample, AllocateSample, the four
           #                  reader-callback allocators, AllocateDataUnit)
           #                  as "pointer-to-pointer whose pointee cannot be
           #                  proven" -- the pointee was an interface this
           #                  roster had simply never seen.
           "mfmediaengine.h", "evr.h", "evr9.h", "wmsdkidl.h", "wmsbuffer.h")

# Headers scanned for TYPES only -- the enum, scalar-typedef and struct
# spellings the interfaces above use by name.  mfapi.h holds MFTIME and the
# MFT_* descriptor structs; nothing in it declares an interface.
TYPE_HEADERS = ("mfapi.h",)

# `Name : public Base` in the C++ half of a widl header.
IFACE_RE = re.compile(
    r'MIDL_INTERFACE\("([0-9a-fA-F-]{36})"\)\s*\n(\w+)\s*:\s*public\s+(\w+)')

# the C half: the flattened vtable, in slot order, with widl's own owner
# comments delimiting the inherited runs.
VTBL_RE = re.compile(r'typedef struct (\w+)Vtbl \{(.*?)\n\} \1Vtbl;', re.DOTALL)
OWNER_RE = re.compile(r'/\*\*\* (\w+) methods \*\*\*/')
METHOD_RE = re.compile(
    r'(?P<ret>[\w]+(?:\s*\*+)?)\s*\(STDMETHODCALLTYPE\s*\*(?P<name>\w+)\)\(\s*'
    r'(?P<args>[^;]*?)\);', re.DOTALL)

# enum typedefs, which are int-class in both ABIs and so may cross by value.
ENUM_RE = re.compile(r'typedef enum \w*\s*\{.*?\}\s*(\w+)\s*;', re.DOTALL)
# `typedef IMFFoo *LPFOO;` -- a pointer to an interface without a `*` in the
# text.  spec2thunk's flat-surface audit needs these names or an export taking
# one would read as carrying no interface at all.
ALIAS_RE = re.compile(r'typedef\s+(I\w+)\s*\*\s*(\w+)\s*;')

# `typedef DWORD MediaEventType;` -- an integer under a Media Foundation name.
# Collected from the headers rather than listed, because a hand list is how
# `TOPOID` ends up refused for being "not provably integer-class" when it is a
# UINT64 two lines above the interface that takes one.
SCALAR_BASE = ("BYTE WORD DWORD QWORD UINT INT LONG ULONG SHORT USHORT "
               "UINT8 UINT16 UINT32 UINT64 INT8 INT16 INT32 INT64 "
               "LONGLONG ULONGLONG LONG64 ULONG64 BOOL HRESULT "
               "ULONG_PTR DWORD_PTR UINT_PTR LONG_PTR INT_PTR").split()
SCALAR_RE = re.compile(r'typedef\s+(?:unsigned\s+|signed\s+)?(\w+)\s+(\w+)\s*;')


def split_args(text):
    """Split a parameter list on top-level commas."""
    out, depth, cur = [], 0, ""
    for ch in text:
        if ch in "([": depth += 1
        elif ch in ")]": depth -= 1
        if ch == "," and depth == 0:
            out.append(cur)
            cur = ""
        else:
            cur += ch
    if cur.strip():
        out.append(cur)
    return [" ".join(a.split()) for a in out if a.strip()]


def parse_header(path):
    """-> (ifaces, enums, aliases, structs) from one widl-generated header."""
    with open(path, errors="replace") as fh:
        text = fh.read()

    uuid_of, base_of = {}, {}
    for uuid, name, base in IFACE_RE.findall(text):
        uuid_of[name] = uuid.lower()
        base_of[name] = base

    ifaces = {}
    for name, body in VTBL_RE.findall(text):
        if name not in uuid_of:
            continue                      # no MIDL_INTERFACE: not a real iface
        slots, owner = [], None
        pos = 0
        marks = [(m.start(), m.group(1)) for m in OWNER_RE.finditer(body)]
        for m in METHOD_RE.finditer(body):
            while marks and marks[0][0] < m.start():
                owner = marks.pop(0)[1]
            args = split_args(m.group("args"))
            if not args or "This" not in args[0]:
                sys.exit("%s: %s::%s has no `This` parameter -- the vtable "
                         "parse is wrong, not the header"
                         % (path, name, m.group("name")))
            slots.append(dict(slot=len(slots), owner=owner or name,
                              name=m.group("name"),
                              ret=" ".join(m.group("ret").split()),
                              params=args[1:]))
            pos = m.end()
        del pos
        ifaces[name] = dict(uuid=uuid_of[name], base=base_of[name],
                            header=os.path.basename(path), slots=slots)

    enums = set(ENUM_RE.findall(text))
    aliases = {a: i for i, a in ALIAS_RE.findall(text)}
    scalars = {name: base for base, name in SCALAR_RE.findall(text)}
    return ifaces, enums, aliases, scalars


def resolve_scalars(scalars, enums):
    """Names that resolve, through however many typedefs, to a scalar this
    generator will let cross by value.  Anything that does not resolve is
    simply absent, and gen_winecom.py refuses the slot that uses it."""
    known, changed = set(SCALAR_BASE) | enums, True
    while changed:
        changed = False
        for name, base in scalars.items():
            if name not in known and base in known:
                known.add(name)
                changed = True
    return known & set(scalars)


def build(srctree, build_dir):
    ifaces, enums, aliases, scalars = {}, set(), {}, {}
    seen = []
    for h in HEADERS + TYPE_HEADERS:
        for root in (build_dir, os.path.join(srctree, "include")):
            path = os.path.join(root, "include", h) if root == build_dir \
                else os.path.join(root, h)
            if os.path.exists(path):
                break
        else:
            sys.exit("gen_interfaces: cannot find %s -- widl has not run; "
                     "build the tree first" % h)
        if h in HEADERS:
            seen.append(path)
        i, e, a, s = parse_header(path)
        ifaces.update(i)
        enums |= e
        aliases.update(a)
        scalars.update(s)

    # IUnknown itself is ON the roster, and that is a decision worth stating.
    # It is not in the parsed headers (unknwn.h is not a widl output of this
    # surface), so it is written out here.  Three reasons it belongs:
    #
    #   * QueryInterface(IID_IUnknown) is COM's most-called question, and
    #     without a roster entry libs/winecom's proxy_qi answers E_NOINTERFACE
    #     to it -- on every object, forever;
    #   * `IUnknown **` out-parameters (IMFCollection::GetElement,
    #     IMFAsyncResult::GetState, IMFSourceResolver::CreateObjectFromURL)
    #     become a proxy the guest can QI, instead of a refusal;
    #   * spec2thunk's flat-surface audit builds its "does this signature
    #     carry an interface" token set out of THIS list, so without the entry
    #     an export taking a bare `IUnknown *` -- MFShutdownObject does -- is
    #     invisible to the audit and passes a guest proxy through unclassified.
    #
    # Its vtable is exactly the three slots libs/winecom serves from the proxy
    # table, so nothing ever crosses through it.
    ifaces["IUnknown"] = dict(
        uuid="00000000-0000-0000-c000-000000000046",
        base="IUnknown", header="unknwn.h",
        slots=[dict(slot=0, owner="IUnknown", name="QueryInterface", ret="HRESULT",
                    params=["REFIID riid", "void **ppvObject"]),
               dict(slot=1, owner="IUnknown", name="AddRef", ret="ULONG", params=[]),
               dict(slot=2, owner="IUnknown", name="Release", ret="ULONG", params=[])])

    # IDispatch joined 2026-09-01, for the PROPVARIANT completeness pass:
    # VT_DISPATCH is a served tag now, and a served tag must hand the guest a
    # proxy whose vtable really has IDispatch's seven slots -- an IUnknown-
    # typed proxy would send `Invoke` into slot 4 of a 3-slot table.  Written
    # out here exactly as IUnknown above (oaidl.h is not this surface's widl
    # output), with the declaration transcribed from include/oaidl.h.
    # Invoke is HAND-SERVED (the shared VARIANT discipline,
    # include/wine/winecom_variant.h); GetTypeInfo refuses by name (ITypeInfo
    # is not on this roster and nothing on this surface vends one).
    ifaces["IDispatch"] = dict(
        uuid="00020400-0000-0000-c000-000000000046",
        base="IUnknown", header="oaidl.h",
        slots=[dict(slot=0, owner="IUnknown", name="QueryInterface", ret="HRESULT",
                    params=["REFIID riid", "void **ppvObject"]),
               dict(slot=1, owner="IUnknown", name="AddRef", ret="ULONG", params=[]),
               dict(slot=2, owner="IUnknown", name="Release", ret="ULONG", params=[]),
               dict(slot=3, owner="IDispatch", name="GetTypeInfoCount", ret="HRESULT",
                    params=["UINT *pctinfo"]),
               dict(slot=4, owner="IDispatch", name="GetTypeInfo", ret="HRESULT",
                    params=["UINT iTInfo", "LCID lcid", "ITypeInfo **ppTInfo"]),
               dict(slot=5, owner="IDispatch", name="GetIDsOfNames", ret="HRESULT",
                    params=["REFIID riid", "LPOLESTR *rgszNames", "UINT cNames",
                            "LCID lcid", "DISPID *rgDispId"]),
               dict(slot=6, owner="IDispatch", name="Invoke", ret="HRESULT",
                    params=["DISPID dispIdMember", "REFIID riid", "LCID lcid",
                            "WORD wFlags", "DISPPARAMS *pDispParams",
                            "VARIANT *pVarResult", "EXCEPINFO *pExcepInfo",
                            "UINT *puArgErr"])])

    # IClassFactory is on the roster for a reason measured rather than
    # assumed: WITHOUT it, this surface's only door is welded shut.
    #
    # mfmediaengine has exactly one flat export a caller can use --
    # DllGetClassObject -- and dlls/mfmediaengine/mfcom.c wraps its result
    # with __wine_com_wrap_out_iface(hr, riid, out).  That helper looks the
    # riid up in THIS roster and, finding nothing, releases the object and
    # answers E_NOINTERFACE rather than hand a guest a native vtable.  So a
    # guest asking for IID_IClassFactory got E_NOINTERFACE and there was no
    # second way in: IMFMediaEngineClassFactory is reached only through a
    # class object, and evr's and mfplat's DllGetClassObject were shut for the
    # same reason.  Measured by ppc64le/mf/check-mf-modules.sh, which is the
    # first thing that ever asked one of these modules for an object.
    #
    # It is written out here rather than by adding unknwn.h to HEADERS,
    # exactly as IUnknown above is: that header also declares AsyncIUnknown,
    # whose Begin_/Finish_ split is a second vtable nothing on this surface
    # vends, and a roster grows by decisions rather than by whatever a header
    # happened to contain.
    #
    # Both crossing slots have complete plans and neither is a special case:
    # CreateInstance is (IUnknown *outer, REFIID, void **) -- the same
    # IFACE_IN/RIID/PPV_OUT triple IMFGetService::GetService already carries,
    # so an aggregation outer arriving as a guest proxy is translated in and
    # the vended object comes back wrapped by IID; LockServer takes a BOOL.
    ifaces["IClassFactory"] = dict(
        uuid="00000001-0000-0000-c000-000000000046",
        base="IUnknown", header="unknwn.h",
        slots=[dict(slot=0, owner="IUnknown", name="QueryInterface", ret="HRESULT",
                    params=["REFIID riid", "void **ppvObject"]),
               dict(slot=1, owner="IUnknown", name="AddRef", ret="ULONG", params=[]),
               dict(slot=2, owner="IUnknown", name="Release", ret="ULONG", params=[]),
               dict(slot=3, owner="IClassFactory", name="CreateInstance", ret="HRESULT",
                    params=["IUnknown *pUnkOuter", "REFIID riid", "void **ppvObject"]),
               dict(slot=4, owner="IClassFactory", name="LockServer", ret="HRESULT",
                    params=["BOOL fLock"])])

    # Base-chain closure: every interface's ENTIRE chain must be present, or
    # its inherited slot NUMBERS are unknown and a table with the wrong slot
    # numbers is the silent mis-dispatch this design exists to prevent.
    dropped = {}
    taken = {}
    for name in sorted(ifaces):
        chain, cur = [], name
        while cur != "IUnknown":
            if cur not in ifaces:
                dropped[name] = ("base chain reaches %s, which is not in the "
                                 "parsed header set, so this interface's "
                                 "inherited slot NUMBERS are unknown" % cur)
                break
            chain.append(cur)
            cur = ifaces[cur]["base"]
        else:
            taken[name] = ifaces[name]

    for name, itf in taken.items():
        want = list(range(len(itf["slots"])))
        got = [s["slot"] for s in itf["slots"]]
        if got != want or not itf["slots"]:
            sys.exit("gen_interfaces: %s slot numbers are not contiguous" % name)
        if [s["name"] for s in itf["slots"][:3]] != \
                ["QueryInterface", "AddRef", "Release"]:
            sys.exit("gen_interfaces: %s does not begin with IUnknown's three "
                     "slots -- the vtable parse is wrong" % name)

    by_uuid = {}
    for name in sorted(taken):
        u = taken[name]["uuid"]
        if u in by_uuid:
            sys.exit("gen_interfaces: %s and %s share IID %s"
                     % (by_uuid[u], name, u))
        by_uuid[u] = name

    roster = dict(
        surface="wine-mf",
        headers=[os.path.basename(p) for p in seen],
        type_headers=list(TYPE_HEADERS),
        iface_ptr_aliases={k: v for k, v in sorted(aliases.items())
                           if v in taken},
        integer_types=sorted(enums | resolve_scalars(scalars, enums)),
        interfaces={n: taken[n] for n in sorted(taken)},
    )
    return roster, dropped


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--srctree", default=SRCTREE)
    ap.add_argument("--build", default=SRCTREE,
                    help="build tree holding widl's generated headers")
    ap.add_argument("--json", metavar="FILE")
    ap.add_argument("--check", metavar="FILE")
    ap.add_argument("--report", action="store_true")
    args = ap.parse_args()

    roster, dropped = build(args.srctree, args.build)
    text = json.dumps(roster, indent=1, sort_keys=False) + "\n"

    n_slots = sum(len(i["slots"]) for i in roster["interfaces"].values())
    print("surface %s: %d interface(s), %d vtable slot(s), %d dropped"
          % (roster["surface"], len(roster["interfaces"]), n_slots, len(dropped)))

    if args.report:
        print("\ndropped, with the reason:")
        for n in sorted(dropped):
            print("  %-40s %s" % (n, dropped[n]))
        print("\ntaken, by header:")
        for h in roster["headers"]:
            names = [n for n in roster["interfaces"]
                     if roster["interfaces"][n]["header"] == h]
            print("  %-16s %d" % (h, len(names)))

    if args.check:
        with open(args.check) as fh:
            have = fh.read()
        if have == text:
            print("check passed: %s matches Wine's headers" % args.check)
            return 0
        sys.exit("gen_interfaces: %s has DRIFTED from Wine's headers. "
                 "Regenerate it (--json) and re-run every gate." % args.check)

    if args.json:
        with open(args.json, "w") as fh:
            fh.write(text)
        print("wrote %s" % args.json)
    return 0


if __name__ == "__main__":
    sys.exit(main())
