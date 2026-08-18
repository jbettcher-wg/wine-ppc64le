#!/usr/bin/env python3
"""Emit the winecom marshal tables for the Media Foundation surface.

  ./gen_winecom.py --out ../../dlls/mfplat/mf_marshal.h
  ./gen_winecom.py --report        # what is marshalled, what is refused, why
  ./gen_winecom.py --check ../../dlls/mfplat/mf_marshal.h

INPUT is interfaces_mf.json -- the ONE roster, the same file
dlls/mfplat/mfplat.thunks, dlls/mf/mf.thunks and
dlls/mfreadwrite/mfreadwrite.thunks hand spec2thunk to build the guest trap
modules.  Both generators sort by interface name, and libs/winecom
cross-checks every IID and slot count at attach as the last line of defence.

OUTPUT is a header of `struct winecom_slot` / `struct winecom_iface` tables
(include/wine/winecom.h), consumed by dlls/mfplat/mfcom.c.  It is the
system-COM sibling of dlls/combase/syscom_marshal.h rather than of the DXVK
lane's d3d11_marshal.h, and the difference matters in exactly two places:

  * WCHAR crosses.  Wine's PE modules are built with -fshort-wchar and so is
    the guest, so both sides' WCHAR is two bytes.  (On the DXVK surface it is
    four on one side, which is why that generator refuses every WCHAR-bearing
    slot.)
  * HANDLE and HWND cross.  Both sides are the same Wine process: a guest's
    handles come from the same kernel32/user32 the native side calls, so the
    integer means the same object in both namespaces.

WHAT THIS GENERATOR IS FOR.  Every argument of every slot must be classified
before the call may cross: an interface pointer that crosses unclassified is
handed to the guest as a NATIVE vtable, and the guest's first method call
through it executes ppc64 bytes as x86-64.  The rule is CLASSIFY OR REFUSE,
never pass-and-hope; a parameter shape the classifier does not recognise
becomes a named refusal with the reason the runtime prints once, and is
counted in --report.

THE REFUSALS THAT MATTER MOST ON THIS SURFACE, because they are Media
Foundation's own shape rather than an accident of this port:

  * PROPVARIANT.  IMFAttributes::GetItem/SetItem and everything spelled
    REFPROPVARIANT carry a tagged union that can hold VT_UNKNOWN -- an
    interface pointer with no type in the signature at all.  Refused; the
    typed accessors (GetGUID/GetUINT32/GetUINT64/GetString/GetBlob/GetUnknown)
    are fully served and are what callers actually use.
  * nothing about IUnknown.  It is ON the roster (see
    ppc64le/mf/gen_interfaces.py for the three reasons), so `IUnknown *` in
    translates like any other proxy and `IUnknown **` out comes back as a
    three-slot proxy the guest QueryInterfaces from -- which is what a COM
    caller does with one anyway.
  * MFT_OUTPUT_DATA_BUFFER (IMFTransform::ProcessOutput) holds an IMFSample*
    and an IMFCollection* INSIDE a struct, which is why the struct-bearing
    scan below still refuses it for every OTHER slot that carries one; this
    one slot is routed to a hand-written walker instead (HAND_SLOTS,
    dlls/mfplat/mfcom.c's hand_process_output), the shape
    dlls/d3d12/main.c's hand_resource_barrier has.
  * by-value float (IMFRateControl::SetRate, IMFSimpleAudioVolume).  The
    native invoker (dlls/mfplat/mfcom.c mf_invoke) calls the host vtable slot
    with the widest INTEGER form, so a float argument would be placed in the
    wrong register file entirely.

A guest-IMPLEMENTED object handed back into native MF -- an IMFAsyncCallback,
an IMFByteStreamHandler, an IMFMediaSource the game wrote -- is a different
refusal and does NOT live here: it is a runtime property of the pointer, not
of the signature, so libs/winecom's winecom_translate_in refuses it at the
moment it arrives, with the interface named.  See ppc64le/mf/README.md.

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

# --------------------------------------------------------------------------
# by-value types that cross as an integer register and are the same width on
# both sides.  ANYTHING NOT LISTED STOPS GENERATION -- see the module banner.
# The roster's enum/typedef names are added to this set at run time.
# --------------------------------------------------------------------------
BYVAL_INTEGER = frozenset("""
    UINT INT LONG ULONG DWORD WORD BYTE BOOL WINBOOL UINT8 UINT16 UINT32
    UINT64 INT8 INT16 INT32 INT64 SIZE_T SSIZE_T LONGLONG ULONGLONG
    ULONG64 LONG64 QWORD HANDLE HWND HRESULT
    ULONG_PTR DWORD_PTR UINT_PTR LONG_PTR INT_PTR
    unsigned int short char long
""".split())

# 8-byte aggregates that are ONE integer register on both the MS-x64 and the
# ELFv2 side.  Each is here because its layout was checked.
BYVAL_AGGREGATE = {
    "MFRatio": "{ UINT32 Numerator; UINT32 Denominator; } -- 8 bytes, one "
               "register on both ABIs",
}

# By-value aggregates the two ABIs pass DIFFERENTLY, refused by name with the
# mechanism rather than with the generic "not provably integer-class".
BYVAL_ABI_SPLIT = {
    "GUID":
        "takes a GUID BY VALUE, and the two ABIs disagree about how.  MS-x64 "
        "passes any aggregate that is not 1/2/4/8 bytes by a hidden POINTER "
        "to a caller-allocated temporary, so the guest puts an address in the "
        "argument slot; ELFv2 passes a 16-byte struct in TWO GPRs, so the "
        "native callee reads that address as the first half of the GUID and "
        "whatever follows as the second.  Needs a hand-written slot that "
        "dereferences, not a marshal class",
    "PROPERTYKEY":
        "takes a PROPERTYKEY (a GUID plus a DWORD) by value; see the GUID "
        "case -- 20 bytes is a hidden pointer on one ABI and registers on the "
        "other",
}

# Pointer spellings that are pointers without a `*` in the source text.
POINTER_TYPEDEFS = {
    "LPSTR": "char", "LPCSTR": "char", "LPWSTR": "WCHAR", "LPCWSTR": "WCHAR",
    "PWSTR": "WCHAR", "PCWSTR": "WCHAR", "BSTR": "WCHAR", "LPOLESTR": "WCHAR",
    "LPCOLESTR": "WCHAR",
    "LPVOID": "void", "PVOID": "void", "LPCVOID": "void",
    "REFIID": "GUID", "REFGUID": "GUID", "REFCLSID": "GUID",
    "REFPROPVARIANT": "PROPVARIANT",
}

# Pointer-to-pointer parameters that really are blocks of plain memory, so
# CA_PASS is the right answer for them: the callee writes a HOST address into
# guest-visible storage, and on this port guest memory IS host memory.  The
# list is an ALLOW-list because the failure it guards is silent -- an
# `IFoo **` this generator did not recognise as an interface would otherwise
# hand the guest a native vtable -- and generation stops on anything else.
PLAIN_PP = {
    "BYTE":  "IMFMediaBuffer::Lock / IMF2DBuffer::Lock2D -- the mapped sample",
    "UINT8": "IMFAttributes::GetAllocatedBlob -- a CoTaskMemAlloc'd blob",
    "WCHAR": "IMFAttributes::GetAllocatedString -- a CoTaskMemAlloc'd string",
    "char":  "an ANSI string out-parameter",
}

FLOAT_TOKENS = re.compile(r'\b(FLOAT|float|double|DOUBLE)\b')

# Structs that carry an interface pointer (or an untyped tagged payload that
# can BE one) but whose body is not in the parsed MF headers, so the
# transitive scan below cannot find them.  Named here instead.
CARRIER_STRUCTS = {
    "PROPVARIANT": "a tagged union that can hold VT_UNKNOWN -- an interface "
                   "pointer with no type anywhere in the signature",
    "VARIANT": "a tagged union that can hold VT_UNKNOWN -- an interface "
               "pointer with no type anywhere in the signature",
    "PROPERTYKEY": "reaches a PROPVARIANT through the property store it keys",
}

# --------------------------------------------------------------------------
# Hand-written slots.  Keyed "Owner::Method" so an inherited slot gets the
# same hand function in every derived interface's vtable, which is the whole
# point of keying by owner.  The C functions live in dlls/mfplat/mfcom.c and
# the order here IS the hand_funcs[] order there.
#
# Both entries exist to rescue a PROPVARIANT slot that the table-driven
# classifier must refuse and a human can serve.  The refusal is right in
# general -- a PROPVARIANT's `vt` can name an interface pointer that no IID in
# the signature types -- but these three slots are the ones a cutscene player
# actually needs, so each is served with the tag AUDITED at run time instead:
# a VT_I8 seek position or a VT_UI8 duration passes, a VT_UNKNOWN is refused
# with the same loudness the blanket refusal had.
#
#   IMFSourceReader::SetCurrentPosition       seek -- replay a cutscene
#   IMFMediaSession::Start                    the same (this, GUID*, PROPVARIANT*)
#                                             shape, so the same function
#   IMFSourceReader::GetPresentationAttribute MF_PD_DURATION -- how long is it
# --------------------------------------------------------------------------
HAND_SLOTS = [
    ("IMFSourceReader::SetCurrentPosition",       "hand_propvariant_in"),
    ("IMFMediaSession::Start",                    "hand_propvariant_in"),
    ("IMFSourceReader::GetPresentationAttribute", "hand_propvariant_out"),
    # MFT_OUTPUT_DATA_BUFFER carries an IMFSample* and an IMFCollection*
    # inside a struct the classifier below refuses by name (see
    # CARRIER_STRUCTS/bearing); this hand slot is the walker dlls/d3d12/
    # main.c's hand_resource_barrier has, one field at a time instead of one
    # union arm at a time.  Bypasses classify() entirely, exactly like the
    # PROPVARIANT hands above.
    ("IMFTransform::ProcessOutput",               "hand_process_output"),
]

# Refusals decided here rather than derived, each with the reason the runtime
# prints once.  Keyed "Owner::Method" like HAND_SLOTS.
REFUSALS = {}

# void** out-parameters that are blocks of memory rather than untyped
# interface pointers, checked one by one against the headers.  Everything else
# with a bare void** is refused: the default has to be the safe one.
VOID_PP_IS_MEMORY = frozenset()


# --------------------------------------------------------------------------
# struct bodies -- which ones reach an interface pointer, transitively
# --------------------------------------------------------------------------

STRUCT_RE = re.compile(
    r'typedef\s+(?:struct|union)\s*(?:\w+\s*)?\{(.*?)\}\s*(\w+)\s*;', re.DOTALL)


def scan_structs(paths, iface_names):
    """-> (bearing, why).

    `bearing` is the set of struct type names that reach an interface pointer
    through any member chain.  MFT_OUTPUT_DATA_BUFFER is the case that matters
    and it is not visible in a signature: `MFT_OUTPUT_DATA_BUFFER *pSamples`
    reads as a plain data pointer while carrying an IMFSample* and an
    IMFCollection* the callee would receive as guest proxies.

    Deliberately transitive and deliberately crude about what a member type
    is -- every identifier in the body counts.  Over-approximating costs a
    refusal; under-approximating hands native MF a guest proxy pointer."""
    members = {}
    for path in paths:
        with open(path, errors="replace") as fh:
            text = fh.read()
        for body, name in STRUCT_RE.findall(text):
            if name in members:
                continue
            # SORTED, and that is load-bearing rather than tidy: the `why`
            # path below ends up in the refusal TEXT of the generated header,
            # so iterating a set here would make the committed file depend on
            # PYTHONHASHSEED and the --check gate go red at random.
            members[name] = sorted(
                set(re.findall(r'\b(\w+)\s*\*', body)) |
                set(re.findall(r'\b(\w+)\s+\w+\s*(?:\[|;|:)', body)))

    hit, why = {}, {}
    for name in members:
        seen, stack = set(), [(name, [])]
        while stack:
            cur, path = stack.pop()
            if cur in seen:
                continue
            seen.add(cur)
            for m in members.get(cur, ()):
                if m in iface_names:
                    hit[name] = True
                    why[name] = " -> ".join(path + [cur, m])
                    stack = []
                    break
                if m in members:
                    stack.append((m, path + [cur]))
    return set(hit), why


# --------------------------------------------------------------------------
# parameter parsing
# --------------------------------------------------------------------------

class Param:
    __slots__ = ("raw", "base", "stars", "inner_const", "array", "name")

    def __init__(self, raw):
        self.raw = " ".join(raw.split())
        t = self.raw
        self.array = bool(re.search(r'\[', t))
        t = re.sub(r'\[[^\]]*\]', '', t).strip()
        # `Iface *const *p`: the INNER pointer is const, which is how these
        # headers spell an INPUT array.
        self.inner_const = bool(re.search(r'\*\s*const\s*\*', t))
        t = re.sub(r'\bconst\b', ' ', t)
        toks = t.replace('*', ' * ').split()
        self.stars = toks.count('*')
        toks = [x for x in toks if x != '*']
        self.base = toks[0] if toks else ''
        self.name = toks[-1] if len(toks) > 1 else ''
        if self.base in POINTER_TYPEDEFS:
            self.stars += 1
            self.base = POINTER_TYPEDEFS[self.base]
        if self.array:
            self.stars += 1

    def is_riid(self):
        return re.match(r'^(REFIID|REFGUID|REFCLSID)\b', self.raw) is not None


COUNT_RE = re.compile(r'^(\w*[Cc]ount\w*|cElements|dwCount|cbSize)$')


def find_count(params, idx):
    """Index of the by-value count that governs the array at `idx`: nearest
    preceding, else nearest following.  None if there is none."""
    cands = [i for i, p in enumerate(params)
             if p.stars == 0 and p.base in ("UINT", "UINT32", "unsigned",
                                            "int", "DWORD", "ULONG")
             and COUNT_RE.match(p.name or '')]
    before = [i for i in cands if i < idx]
    after = [i for i in cands if i > idx]
    if before:
        return before[-1]
    if after:
        return after[0]
    return None


# --------------------------------------------------------------------------
# classification
# --------------------------------------------------------------------------

CA = dict(PASS=0, IFACE_IN=1, RIID=2, PPV_OUT=3, RET_PTR=4, EVENT=5,
          IFACE_ARR_IN=6, IFACE_OUT_STATIC=7, IFACE_ARR_OUT_STATIC=8)
CA_NAME = {v: "WINECOM_CA_" + k for k, v in CA.items()}


class Refused(Exception):
    pass


class Plan:
    """What classify() proved about one slot.

    `fp_reason` is the FORWARD refusal of a slot whose only problem is a
    by-value float: the plan is complete and the REVERSE direction can serve
    it (WINECOM_F_REV), while the forward invoker -- which calls the native
    vtable slot in its widest INTEGER form -- still cannot place the value and
    still refuses.  Every other refusal is about the signature, is raised as
    Refused, and stands in both directions."""

    __slots__ = ("cls", "xaux", "caux", "aux", "aux2", "fpmask", "fpwide",
                 "xmask", "fp_reason")

    def __init__(self):
        self.cls = self.xaux = self.caux = None
        self.aux = self.aux2 = 0
        self.fpmask = self.fpwide = self.xmask = 0
        self.fp_reason = None


def classify(key, slot, ifaces, iface_index, byval_ok, bearing, why_bearing):
    """-> Plan, or raise Refused(reason)."""
    params = [Param(p) for p in slot["params"]]
    n = len(params)
    cls = [CA["PASS"]] * n
    xaux = [0] * n
    caux = [0] * n
    aux = aux2 = 0
    plan = Plan()
    floats = []

    if FLOAT_TOKENS.search(slot["ret"]) and "*" not in slot["ret"]:
        raise Refused(
            "returns %s by value; the native invoker calls the host vtable "
            "slot with the widest INTEGER form, so the result would come back "
            "out of the wrong register file" % slot["ret"])
    if slot["ret"] not in ("void",) and Param(slot["ret"]).stars == 0 and \
            Param(slot["ret"]).base not in byval_ok:
        raise Refused(
            "return type %s is not provably integer-class on both ABIs; "
            "refusing rather than assuming" % slot["ret"])

    for i, p in enumerate(params):
        if p.stars == 0:
            if FLOAT_TOKENS.search(p.raw):
                # NOT a hard refusal: an ELFv2 float lives in f1-f13 and an
                # MS-x64 one in XMM0-3, and both are register files a
                # marshaller can read and write.  What cannot place one is the
                # FORWARD invoker, which calls the native vtable slot in its
                # widest integer form -- so the row is emitted complete, the
                # forward call refuses with the reason below, and
                # WINECOM_F_REV lets libs/winecom/reverse.c serve it.
                if i >= 7:
                    raise Refused(
                        "passes %s by value in argument position %d, past the "
                        "eight-argument register file either direction "
                        "marshals" % (p.base, i + 1))
                plan.fpmask |= 1 << i
                if p.base in ("double", "DOUBLE"):
                    plan.fpwide |= 1 << i
                floats.append(p.base)
                cls[i] = CA["PASS"]
                continue
            if p.base in BYVAL_AGGREGATE:
                cls[i] = CA["PASS"]
                continue
            if p.base in BYVAL_ABI_SPLIT:
                raise Refused(BYVAL_ABI_SPLIT[p.base])
            if p.base not in byval_ok:
                raise Refused(
                    "by-value %s is not provably integer-class on both ABIs; "
                    "refusing rather than assuming it is an enum" % p.base)
            cls[i] = CA["PASS"]
            continue

        # ---- pointers
        if p.base in CARRIER_STRUCTS:
            raise Refused("%s is %s" % (p.base, CARRIER_STRUCTS[p.base]))
        if p.base in bearing:
            raise Refused(
                "%s carries interface pointers inside a struct (%s) and has "
                "no hand-written walker; the pointers inside it would reach "
                "native MF as guest proxies"
                % (p.base, why_bearing.get(p.base, p.base)))

        if p.base == "void" and p.stars == 2:
            prev = params[i - 1] if i else None
            if prev is not None and prev.is_riid():
                cls[i] = CA["PPV_OUT"]
                cls[i - 1] = CA["RIID"]
                aux = i - 1
                continue
            if key in VOID_PP_IS_MEMORY:
                cls[i] = CA["PASS"]
                continue
            raise Refused(
                "has a void** out-parameter (`%s`) with no REFIID beside it "
                "to type the result; an untyped interface pointer cannot be "
                "given a guest vtable" % p.raw)

        if p.base in ifaces:
            if p.stars == 1:
                # Translate-in works for ANY of our proxies, IUnknown
                # included: it is a runtime identity test, not a typed one.
                # The TYPE is recorded anyway, because the reverse direction
                # needs it -- a native object arriving as the argument of a
                # guest method has to be given one of the rostered guest
                # vtables, and identity cannot say which.
                cls[i] = CA["IFACE_IN"]
                xaux[i] = iface_index[p.base]
                plan.xmask |= 1 << i
                continue
            if p.stars != 2:
                raise Refused(
                    "takes `%s`: an interface pointer at a level of "
                    "indirection this generator has no class for" % p.raw)
            if p.inner_const:
                c = find_count(params, i)
                if c is None:
                    raise Refused(
                        "takes the input interface array `%s` with no "
                        "by-value count parameter to bound it" % p.raw)
                cls[i] = CA["IFACE_ARR_IN"]
                aux2 = c
                continue
            c = find_count(params, i)
            plural = (p.name or '').startswith("pp") and \
                     (p.name or '').endswith("s")
            if c is not None and plural:
                cls[i] = CA["IFACE_ARR_OUT_STATIC"]
                xaux[i] = iface_index[p.base]
                plan.xmask |= 1 << i
                caux[i] = c
                continue
            if plural:
                raise Refused(
                    "writes the interface array `%s` with no count parameter "
                    "this generator can identify; refusing rather than "
                    "wrapping only its first element" % p.raw)
            cls[i] = CA["IFACE_OUT_STATIC"]
            xaux[i] = iface_index[p.base]
            plan.xmask |= 1 << i
            continue

        # A pointer to a pointer that is not an interface out-parameter has to
        # be named before it may pass: this is the position where an
        # unrecognised interface spelling would silently become a native
        # vtable in the guest's hands.
        if p.stars >= 2 and p.base not in PLAIN_PP:
            raise Refused(
                "writes `%s`, a pointer-to-pointer whose pointee this "
                "generator cannot prove is plain memory rather than an "
                "interface; add it to PLAIN_PP with the reason, or give it a "
                "marshal class" % p.raw)

        # an ordinary pointer to plain data: crosses as an address.  Guest
        # memory IS host memory on this port, and both sides compile these
        # structs from the SAME Wine headers with the same -fshort-wchar, so
        # there is nothing to repack.
        cls[i] = CA["PASS"]

    if floats:
        plan.fp_reason = (
            "passes %s by value.  The native invoker calls the host vtable "
            "slot with the widest INTEGER form, so a float argument would be "
            "placed in the wrong register file entirely; the FORWARD call is "
            "refused for that and the reverse one is served (WINECOM_F_REV), "
            "because a sink is only ever called in that direction"
            % " and ".join(sorted(set(floats))))
    plan.cls, plan.xaux, plan.caux = cls, xaux, caux
    plan.aux, plan.aux2 = aux, aux2
    return plan


# --------------------------------------------------------------------------
# emission
# --------------------------------------------------------------------------

def c_guid(u):
    a, b, c, d, e = u.split("-")
    d4 = d + e
    return "{0x%s,0x%s,0x%s,{%s}}" % (
        a, b, c, ",".join("0x" + d4[i:i + 2] for i in range(0, 16, 2)))


def generate(roster, prefix, header_dir):
    ifaces = roster["interfaces"]
    order = sorted(ifaces)
    iface_index = {n: i for i, n in enumerate(order)}
    byval_ok = set(BYVAL_INTEGER) | set(roster["integer_types"])
    # Only the headers this surface is made of, plus mfapi.h for the structs
    # its flat API shares with the vtables.  Scanning all of include/ would
    # over-approximate into unrelated trees and refuse slots for a reason
    # that had nothing to do with Media Foundation.
    paths = [os.path.join(header_dir, h)
             for h in list(roster["headers"]) + list(roster["type_headers"])
             if os.path.exists(os.path.join(header_dir, h))]
    bearing, why_bearing = scan_structs(paths, set(ifaces))
    hand_index, hand_order = {}, []
    for key, fn in HAND_SLOTS:
        if fn not in hand_order:
            hand_order.append(fn)
        hand_index[key] = hand_order.index(fn)

    out = []
    w = out.append
    stats = dict(marshalled=0, refused=0, hand=0, identity=0, iunknown=0,
                 reverse_only=0)
    refusal_log = []

    w("""/* GENERATED by ppc64le/mf/gen_winecom.py -- do not edit.
 *
 * Marshal tables for the %s surface (%d interfaces, %d vtable slots),
 * generated from ppc64le/mf/interfaces_mf.json -- the same roster
 * dlls/mfplat/mfplat.thunks, dlls/mf/mf.thunks and
 * dlls/mfreadwrite/mfreadwrite.thunks hand spec2thunk to build the guest
 * trap modules.  Interface order is sorted by name, which is the order
 * spec2thunk COM mode gives the guest stub arrays; libs/winecom
 * cross-checks every IID and slot count at attach, so the two generators
 * cannot silently disagree.
 *
 * Slot/iface types and WINECOM_CA_* classes come from
 * include/wine/winecom.h, which must be included before this file.
 */
""" % (roster["surface"], len(order),
       sum(len(ifaces[n]["slots"] or ()) for n in order)))

    w("enum %s_iface_index\n{" % prefix)
    for n in order:
        w("    %s_IFACE_%s = %d," % (prefix.upper(), n, iface_index[n]))
    w("    %s_IFACE_COUNT = %d\n};\n" % (prefix.upper(), len(order)))
    w("#define %s_HAND_COUNT %d\n" % (prefix.upper(), len(hand_order)))
    if hand_order:
        w("/* hand_funcs[] order in dlls/mfplat/mfcom.c:\n%s */\n"
          % "".join("     *   %d %s\n" % (i, f)
                    for i, f in enumerate(hand_order)))

    tables = []
    for n in order:
        slots = ifaces[n]["slots"]
        rows, decls = [], []
        for s in slots:
            key = "%s::%s" % (s["owner"], s["name"])
            if s["slot"] < 3:
                rows.append('    { "%s", NULL, NULL, NULL, %d, 0, 0, 0, NULL,'
                            ' 0, 0, 0 },  /* runtime */'
                            % (key, 1 if s["slot"] else 3))
                stats["iunknown"] += 1
                continue
            argc = 1 + len(s["params"])
            flags = []
            if s["ret"] == "void":
                flags.append("WINECOM_F_RET_VOID")
            if key in hand_index:
                rows.append('    { "%s", NULL, NULL, NULL, %d, WINECOM_F_HAND%s,'
                            ' %d, 0, NULL, 0, 0, 0 },'
                            % (key, argc, "".join("|" + f for f in flags),
                               hand_index[key]))
                stats["hand"] += 1
                continue
            plan = None
            reason = REFUSALS.get(key)
            if reason is None:
                try:
                    plan = classify(key, s, ifaces, iface_index, byval_ok,
                                    bearing, why_bearing)
                    reason = plan.fp_reason
                except Refused as e:
                    reason = str(e)
            if reason is not None and plan is None:
                # A refusal about the SIGNATURE: no plan, no row, and the
                # reverse direction refuses it with the same words.
                rows.append('    { "%s",\n      "%s: %s",\n'
                            '      NULL, NULL, %d, 0, 0, 0, NULL, 0, 0, 0 },'
                            % (key, key, reason.replace('"', "'"), argc))
                stats["refused"] += 1
                refusal_log.append((n, s["slot"], key, reason))
                continue
            cls, xaux, caux = plan.cls, plan.xaux, plan.caux
            cname = xname = kname = "NULL"
            if any(c != CA["PASS"] for c in cls):
                cname = "cls_%s_%d" % (n, s["slot"])
                decls.append("static const unsigned char %s[] = { %s };"
                             % (cname, ", ".join(CA_NAME[c] for c in cls)))
            # Emitted whenever a parameter is interface-typed, NOT when the
            # values happen to be non-zero: roster index 0 is a real interface
            # and `any(xaux)` would drop the row that names it.
            if plan.xmask:
                xname = "xaux_%s_%d" % (n, s["slot"])
                decls.append("static const unsigned char %s[] = { %s };"
                             % (xname, ", ".join(str(x) for x in xaux)))
            if any(caux):
                kname = "caux_%s_%d" % (n, s["slot"])
                decls.append("static const unsigned char %s[] = { %s };"
                             % (kname, ", ".join(str(x) for x in caux)))
            if reason is not None:
                # A by-value float: the plan is complete, the FORWARD call
                # refuses, and WINECOM_F_REV lets the reverse one through.
                flags.append("WINECOM_F_REV")
                rows.append('    { "%s",\n      "%s: %s",\n'
                            '      %s, %s, %d, %s, %d, %d, %s, 0x%02x, 0x%02x,'
                            ' 0x%02x },'
                            % (key, key, reason.replace('"', "'"), cname, xname,
                               argc, "|".join(flags), plan.aux, plan.aux2,
                               kname, plan.fpmask, plan.fpwide, plan.xmask))
                stats["refused"] += 1
                stats["reverse_only"] += 1
                refusal_log.append((n, s["slot"], key, reason))
                continue
            rows.append('    { "%s", NULL, %s, %s, %d, %s, %d, %d, %s, 0, 0,'
                        ' 0x%02x },'
                        % (key, cname, xname, argc,
                           "|".join(flags) or "0", plan.aux, plan.aux2, kname,
                           plan.xmask))
            stats["marshalled"] += 1

        if len(slots) <= 3:
            # An identity row -- and the ONLY interface that gets one is
            # IUnknown itself, whose three slots libs/winecom serves from the
            # proxy table without ever crossing.
            #
            # This is where this generator deliberately parts company with
            # ppc64le/dxvk/gen_winecom.py, which gives an identity row to any
            # interface whose every argument came out CA_PASS on the grounds
            # that "a table of nothing but CA_PASS rows would claim more than
            # we checked".  On THIS surface that reasoning inverts: a row is
            # CA_PASS only because classify() proved the parameter is
            # plain data, and libs/winecom refuses every slot of an interface
            # whose table is NULL.  IMFMediaBuffer is the case that made the
            # difference -- Lock(BYTE **, DWORD *, DWORD *), Unlock(),
            # GetCurrentLength(DWORD *) are all plain data, so the old rule
            # refused the single most important method on the surface with
            # "no marshal plan", MEASURED: the guest read zero bytes of PCM
            # out of a file the native run decoded exactly.
            stats["identity"] += 1
            tables.append((n, None))
            continue
        tables.append((n, (decls, rows)))

    for n, t in tables:
        if t is None:
            continue
        decls, rows = t
        w("")
        for d in decls:
            w(d)
        w("static const struct winecom_slot slots_%s[%d] =\n{" % (n, len(rows)))
        for r in rows:
            w(r)
        w("};")

    w("\nstatic const struct winecom_iface %s_com_ifaces[%s_IFACE_COUNT] =\n{"
      % (prefix, prefix.upper()))
    for n, t in tables:
        w('    { "%s", %s,\n      %d, %s, 0 },'
          % (n, c_guid(ifaces[n]["uuid"]), len(ifaces[n]["slots"]),
             "NULL" if t is None else "slots_" + n))
    w("};")

    w("\n/* %d slot(s) marshalled, %d hand-written, %d refused with a named\n"
      " * reason (%d of them refused FORWARD only -- a by-value float, whose\n"
      " * plan is complete and whose WINECOM_F_REV row libs/winecom's reverse\n"
      " * dispatcher serves), %d IUnknown slot(s) served by the runtime;\n"
      " * %d interface(s) (IUnknown itself) carry identity rows only. */"
      % (stats["marshalled"], stats["hand"], stats["refused"],
         stats["reverse_only"], stats["iunknown"], stats["identity"]))
    return "\n".join(out) + "\n", stats, refusal_log


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--roster",
                    default=os.path.join(HERE, "interfaces_mf.json"))
    ap.add_argument("--headers",
                    default=os.path.join(SRCTREE, "include"),
                    help="directory holding widl's generated MF headers")
    ap.add_argument("--prefix", default="mf")
    ap.add_argument("--out")
    ap.add_argument("--check", metavar="FILE")
    ap.add_argument("--report", action="store_true")
    args = ap.parse_args()

    with open(args.roster) as fh:
        roster = json.load(fh)
    text, stats, refusals = generate(roster, args.prefix, args.headers)

    print("surface %s: %d marshalled, %d hand-written, %d refused "
          "(%d reverse-servable), %d IUnknown, %d identity-only interface(s)"
          % (roster["surface"], stats["marshalled"], stats["hand"],
             stats["refused"], stats["reverse_only"], stats["iunknown"],
             stats["identity"]))

    if args.report:
        print("\nrefused slots, by reason:")
        seen = {}
        for n, slot, key, reason in refusals:
            seen.setdefault(reason.split(';')[0][:76], []).append(key)
        for reason, keys in sorted(seen.items(), key=lambda kv: -len(kv[1])):
            uniq = sorted(set(keys))
            print("  %4d  %s" % (len(keys), reason))
            for k in uniq[:8]:
                print("          %s" % k)
            if len(uniq) > 8:
                print("          ... and %d more distinct method(s)"
                      % (len(uniq) - 8))

    if args.check:
        with open(args.check) as fh:
            have = fh.read()
        if have == text:
            print("\ncheck passed: %s matches the roster" % args.check)
            return 0
        sys.exit("\ngen_winecom: %s has DRIFTED from the roster.  Regenerate "
                 "it (--out) and re-run every gate." % args.check)

    if args.out:
        with open(args.out, "w") as fh:
            fh.write(text)
        print("\nwrote %s" % args.out)
    return 0


if __name__ == "__main__":
    sys.exit(main())
