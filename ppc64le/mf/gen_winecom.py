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
    interface pointer with no type in the signature at all.  SERVED since
    2026-09-01, by hand and per tag (see the HAND_SLOTS banner): plain tags
    pass, VT_UNKNOWN translates through the proxy machinery, and only the
    arms nothing can serve (VT_DISPATCH, interface VECTOR/ARRAY/BYREF)
    refuse at runtime naming the VT.  The blanket signature-level refusal
    this paragraph used to describe is gone.
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
  * by-value float (IMFRateControl::SetRate, IMFSimpleAudioVolume) -- SERVED
    since PPC64EC step C: the surface's floating-point invoker
    (dlls/mfplat/mfcom.c mf_invoke_fp, over wine/winecom_fpcall.h) places the
    value in the other register file, driven by the row's
    fpmask/fpwide/fpret.  Positions past the eighth parameter still refuse.

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
    SHORT USHORT CHAR UCHAR BOOLEAN COLORREF
    unsigned int short char long
""".split())
# SHORT/USHORT/CHAR/UCHAR/BOOLEAN/COLORREF were absent from the line above
# while ppc64le/gen_interfaces.py's SCALAR_BASE has carried the first two all
# along, and two lists that disagree is how a tooling gap gets reported as an
# ABI fact -- which is the one thing this generator must never do.  [MEASURED]
# it cost IMFMediaEngine::GetNetworkState and ::GetReadyState, which are the
# two methods anything driving a media engine polls on every frame, plus
# IMFMediaError::GetErrorCode, IMFMediaKeySessionNotify::KeyError and
# IMFVideoDisplayControl::SetBorderColor.  All six are `unsigned short` or
# `DWORD` under another name: 2 or 4 bytes, one integer register, identical on
# MS-x64 and ELFv2, and nothing about them was ever unrepresentable.

# By-value integers NARROWER THAN 32 BITS, and what they are: (bytes, signed).
#
# These need a marshalling step the wider ones do not, and the reason is a
# straight disagreement between the two ABIs about whose job it is.  MS-x64
# leaves the upper bits of a register holding a narrow argument UNDEFINED and
# makes ignoring them the CALLEE's job -- clang emits `movw $0x1, %dx`, which
# writes 16 bits and leaves RDX's top 48 alone.  ELFv2 says the opposite:
# arguments smaller than a doubleword are extended by the CALLER, and a ppc64
# callee is compiled to trust it.
#
# [MEASURED] IWMSyncReader::GetStreamSelected(WORD) and
# ::GetOutputNumberForStream(WORD) crossed with a guest-supplied 1 and reached
# Wine's own winegstreamer code as 0x40000001 -- E_INVALIDARG from one and
# output number 0x40000000 from the other.  A wrong number, not a crash.  The
# runtime does the extension (struct winecom_slot::narrowmask); this table is
# what tells it the width and the signedness.
#
# 32-bit types are deliberately absent: x86-64 zero-extends every 32-bit
# register write to 64 bits by hardware rule, so a DWORD is already clean.
#
# THE RESIDUAL, stated rather than left to be discovered: a typedef in the
# roster's `integer_types` that resolves to one of these is NOT caught here,
# because the roster records those names without their widths.  No interface
# on this surface has one -- every narrow by-value parameter in
# interfaces_mf.json spells WORD, USHORT, SHORT, BYTE or BOOLEAN outright --
# and the --report output prints the narrow parameters it found so a new one
# arriving under an alias is visible rather than silent.
NARROW_BYVAL = {
    "WORD":    (2, False), "USHORT": (2, False), "UINT16":  (2, False),
    "SHORT":   (2, True),  "INT16":  (2, True),
    "BYTE":    (1, False), "UCHAR":  (1, False), "UINT8":   (1, False),
    "BOOLEAN": (1, False),
    "CHAR":    (1, True),  "INT8":   (1, True),
}

# The same widths spelled as plain C, which is how a header that did not use a
# Windows typedef writes them.  Matched against the parameter's whole
# declaration text because Param.base keeps only the FIRST token, so
# `unsigned short x` has base "unsigned" and the width is only in the raw text.
NARROW_RAW = (
    (r'\bunsigned\s+short\b',  (2, False)),
    (r'\bsigned\s+short\b',    (2, True)),
    (r'\bunsigned\s+char\b',   (1, False)),
    (r'\bsigned\s+char\b',     (1, True)),
    (r'\bshort\b',              (2, True)),
    (r'\bchar\b',               (1, True)),
)


def narrow_of(p):
    """(bytes, signed) for a by-value parameter narrower than 32 bits, else None."""
    if p.base in NARROW_BYVAL:
        return NARROW_BYVAL[p.base]
    for pat, w in NARROW_RAW:
        if re.search(pat, p.raw):
            return w
    return None


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
    # include/wmsdkidl.h:671 `typedef LPCWSTR LPCWSTR_WMSDK_TYPE_SAFE;` -- a
    # WCHAR string under a WMSDK guard name.  Ten rows (IWMHeaderInfo's
    # AddMarker/AddScript/AddCodecInfo, IWMLanguageList, IWMStreamConfig's
    # two setters) refused as "not provably integer-class" for want of this
    # line: the string itself is guest memory both sides read with 2-byte
    # WCHAR (this surface's banner, point one).
    "LPCWSTR_WMSDK_TYPE_SAFE": "WCHAR",
    # include/wingdi.h:1866 -- a plain-data struct pointer.  Its one row
    # (IMFVideoDisplayControl::GetCurrentImage) then classifies on its own
    # merits: the BYTE** beside it is the PLAIN_PP DIB blob.
    "LPBITMAPINFOHEADER": "BITMAPINFOHEADER",
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
    "GUID":  "IMFVideoProcessor::GetAvailableVideoProcessorModes / "
             "IWMDRMReader3::GetInclusionList -- the callee CoTaskMemAllocs "
             "an array of plain GUIDs and writes its address; sixteen bytes "
             "of data per element, no vtable anywhere",
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
#
# THE PROPVARIANT FAMILY, 2026-09-01: every PROPVARIANT-bearing slot on the
# surface is hand-served through dlls/mfplat/mfcom.c's mf_pv_in/mf_pv_out
# helpers, which TRANSLATE the tag instead of merely auditing it: VT_UNKNOWN
# crosses (unwrapped in, wrapped as an IUnknown proxy out -- QueryInterface
# from there is what a COM caller does with one anyway), plain-memory tags
# pass through, and only the genuinely untranslatable tags (VT_DISPATCH --
# IDispatch is not on this roster -- and VECTOR/ARRAY/BYREF of interfaces)
# refuse AT RUNTIME, per tag, naming the VT.  That is what complete means
# for a tagged union: the refusal moved from the whole slot to the one arm
# nothing can serve.  Each entry names its pv position(s) so the C function
# choice is an audited statement about the signature, not an inference.
#
# The WM_MEDIA_TYPE family crosses with its pUnk member AUDITED: the only
# native producer zeroes it (dlls/mfplat/mediatype.c:4351 memset in
# MFInitAMMediaTypeFromMFMediaType) and no assignment site exists anywhere
# in mfplat/wmvcore/winegstreamer non-test code, so a non-NULL pUnk in
# either direction is refused loudly as the impossible-today case rather
# than translated speculatively.
#
# The GetRepresentation trio takes a GUID BY VALUE: MS-x64 passes a 16-byte
# aggregate by hidden pointer, so the hand slot dereferences the guest's
# slot and calls the native method with a real by-value GUID prototype.
# The representation blob is always an AM_MEDIA_TYPE from
# MFCreateAMMediaTypeFromMFMediaType (mediatype.c GetRepresentation routes
# all five supported GUIDs there), pUnk audited as above;
# FreeRepresentation frees pbFormat+blob and never reads pUnk (mediatype.c
# FreeRepresentation), so it is a plain dereference-and-call.
HAND_SLOTS = [
    ("IMFSourceReader::SetCurrentPosition",       "hand_propvariant_in"),
    ("IMFMediaSession::Start",                    "hand_propvariant_in"),
    ("IMFAttributes::SetItem",                    "hand_propvariant_in"),
    ("IMFMetadata::SetProperty",                  "hand_propvariant_in"),
    ("IMFSourceReader::GetPresentationAttribute", "hand_propvariant_out"),
    ("IMFAttributes::GetItemByIndex",             "hand_propvariant_out"),
    ("IMFMediaEngineEx::GetStreamAttribute",      "hand_propvariant_out"),
    # MFT_OUTPUT_DATA_BUFFER carries an IMFSample* and an IMFCollection*
    # inside a struct the classifier below refuses by name (see
    # CARRIER_STRUCTS/bearing); this hand slot is the walker dlls/d3d12/
    # main.c's hand_resource_barrier has, one field at a time instead of one
    # union arm at a time.  Bypasses classify() entirely, exactly like the
    # PROPVARIANT hands above.
    ("IMFTransform::ProcessOutput",               "hand_process_output"),
    # (this, PROPVARIANT *out)
    ("IMFMediaEvent::GetValue",                   "hand_pv_out_1"),
    ("IMFMetadata::GetAllLanguages",              "hand_pv_out_1"),
    ("IMFMetadata::GetAllPropertyNames",          "hand_pv_out_1"),
    # (this, <one integer/pointer>, PROPVARIANT *out)
    ("IMFAttributes::GetItem",                    "hand_pv_out_2"),
    ("IMFMediaEngineEx::GetPresentationAttribute", "hand_pv_out_2"),
    ("IMFMediaEngineEx::GetStatistics",           "hand_pv_out_2"),
    ("IMFMetadata::GetProperty",                  "hand_pv_out_2"),
    # (this, REFGUID, REFPROPVARIANT in, BOOL *out)
    ("IMFAttributes::CompareItem",                "hand_pv_in_mid"),
    # (this, <three plain>, const PROPVARIANT *in)
    ("IMFMediaEventGenerator::QueueEvent",        "hand_pv_in_4"),
    ("IMFMediaEventQueue::QueueEventParamVar",    "hand_pv_in_4"),
    # (this, <one plain>, const PROPVARIANT *in, const PROPVARIANT *in)
    ("IMFStreamSink::PlaceMarker",                "hand_pv_in_2_3"),
    # (this, IMFPresentationDescriptor *in, const GUID *, const PROPVARIANT *in)
    ("IMFMediaSource::Start",                     "hand_source_start"),
    # (this, const GUID *, const PROPVARIANT *in, PROPVARIANT *out, PROPVARIANT *out)
    ("IMFSeekInfo::GetNearestKeyFrames",          "hand_pv_keyframes"),
    # WM_MEDIA_TYPE, pUnk audited (see above)
    ("IWMMediaProps::GetMediaType",               "hand_wmt_out"),
    ("IWMMediaProps::SetMediaType",               "hand_wmt_in_1"),
    ("IWMReaderAccelerator::Notify",              "hand_wmt_in_2"),
    ("IWMReaderCallbackAdvanced::OnOutputPropsChanged", "hand_wmt_in_2_ctx"),
    # GUID by value via the hidden pointer (see above)
    ("IMFMediaType::GetRepresentation",           "hand_get_representation"),
    ("IMFMediaType::FreeRepresentation",          "hand_free_representation"),
    ("IMFVideoMediaType::GetVideoRepresentation", "hand_get_video_representation"),
    # The two rows the INSSBuffer roster growth itself added: the WMSDK
    # buffer's property pair takes the property GUID by value, and the
    # payload is a plain void*/size pair.  Same hidden-pointer dereference.
    ("INSSBuffer3::GetProperty",                  "hand_nss_get_property"),
    ("INSSBuffer3::SetProperty",                  "hand_nss_set_property"),
]

# Refusals decided here rather than derived, each with the reason the runtime
# prints once.  Keyed "Owner::Method" like HAND_SLOTS.
REFUSALS = {}

# void** out-parameters that are blocks of memory rather than untyped
# interface pointers, checked one by one against the headers.  Everything else
# with a bare void** is refused: the default has to be the safe one.
VOID_PP_IS_MEMORY = frozenset()

# Slots whose interface out-parameter LOOKS plural to the pp...s name
# heuristic but is a single pointer, audited against the header: the `s` in
# `ppProps` is the s of "Props", not of a plural.  Both GetOutputFormat
# overloads write exactly one IWMOutputMediaProps* (wmsdkidl.h -- one
# out-param, format picked by the two by-value indices before it), and the
# heuristic's refusal ("interface array with no count") was a misreading,
# not a safety catch.  Keyed Owner::Method like every other audited list.
SINGULAR_IFACE_OUT = frozenset((
    "IWMReader::GetOutputFormat",
    "IWMSyncReader::GetOutputFormat",
))


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
                 "fpret", "xmask", "fp_reason", "narrowmask", "narrowwide",
                 "narrowsign", "narrow_params")

    def __init__(self):
        self.cls = self.xaux = self.caux = None
        self.aux = self.aux2 = 0
        self.fpmask = self.fpwide = self.fpret = self.xmask = 0
        self.narrowmask = self.narrowwide = self.narrowsign = 0
        self.narrow_params = []
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
        # PPC64EC step C: no longer a refusal.  The surface's FLOATING-POINT
        # invoker (winecom_surface::invoke_fp, the one shared implementation
        # in wine/winecom_fpcall.h) returns f1's bits alongside the integer
        # result, and the dispatcher writes XMM0 back whole -- the flat
        # lane's THUNK_FP_RET encoding, kept identical on purpose.
        plan.fpret = 1 if Param(slot["ret"]).base in ("double", "DOUBLE") else 2
    if not plan.fpret and slot["ret"] not in ("void",) and \
            Param(slot["ret"]).stars == 0 and \
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
            # An integer narrower than 32 bits crosses with UNDEFINED upper
            # bits (MS-x64) into a callee that trusts them (ELFv2).  Record the
            # width and signedness so libs/winecom can extend it; see
            # NARROW_BYVAL above for the measurement that made this necessary.
            narrow = narrow_of(p)
            if narrow:
                if i >= 8:
                    raise Refused(
                        "passes a %d-byte %s by value in argument position %d, "
                        "past the eight parameters the narrowing masks cover"
                        % (narrow[0], p.base, i + 1))
                plan.narrowmask |= 1 << i
                if narrow[0] == 2:
                    plan.narrowwide |= 1 << i
                if narrow[1]:
                    plan.narrowsign |= 1 << i
                plan.narrow_params.append((i, p.base, narrow[0], narrow[1]))
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
                     (p.name or '').endswith("s") and \
                     key not in SINGULAR_IFACE_OUT
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

    # PPC64EC step C: by-value floats no longer refuse the FORWARD call --
    # the surface's invoke_fp places them (fpmask/fpwide say where, fpret
    # says what comes back).  The rows keep WINECOM_F_REV exactly as before:
    # the reverse plan was always complete, and the reverse gate asserts the
    # flag with the right masks.  fp_reason survives as a Plan slot so a
    # table regenerated by an OLDER generator still reads correctly, but
    # nothing sets it any more.
    if floats:
        plan.fp_reason = None
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
    # A rostered header that is not on disk is a HARD ERROR here, never a
    # quietly smaller scan.  scan_structs is what decides which structs reach
    # an interface pointer through a member chain, and a struct whose
    # definition it never saw is not refused -- it is PASSED THROUGH, which is
    # the single outcome this surface exists to prevent.
    #
    # [MEASURED] Generated in an unbuilt tree, where include/wmsdkidl.h does
    # not exist yet because widl makes it, IWMMediaProps::GetMediaType and
    # ::SetMediaType lost their refusals and came out as ordinary marshalled
    # slots -- handing native winegstreamer a WM_MEDIA_TYPE whose embedded
    # IUnknown* is a guest proxy.  Two rows out of 2378, no warning, and the
    # table still compiles and still passes every gate that only asks whether
    # a call returned.
    #
    # The roster NAMES every header it needs, so a missing one always means
    # the tree is not built -- it never means the surface got smaller.
    paths, missing = [], []
    for h in list(roster["headers"]) + list(roster["type_headers"]):
        full = os.path.join(header_dir, h)
        if os.path.exists(full):
            paths.append(full)
        else:
            missing.append(h)
    if missing:
        sys.exit("gen_winecom: %d rostered header(s) are not in %s: %s\n"
                 "  They are widl-generated -- build the tree first.  "
                 "Generating without them silently drops refusals, because a "
                 "struct whose definition was never seen is passed through "
                 "rather than refused."
                 % (len(missing), header_dir, ", ".join(sorted(missing))))
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
            if plan.fpmask or plan.fpret:
                # A by-value float, SERVED forward through the surface's
                # invoke_fp (PPC64EC step C) -- fpmask/fpwide name the
                # positions, fpret the return, appended-last so every elder
                # reader sees exactly the row it always saw.  WINECOM_F_REV
                # stays: the reverse plan was always complete, and the
                # reverse gate asserts the flag with the right masks.
                flags.append("WINECOM_F_REV")
                rows.append('    { "%s", NULL, %s, %s, %d, %s, %d, %d, %s,'
                            ' 0x%02x, 0x%02x, 0x%02x, 0x%02x, 0x%02x, 0x%02x,'
                            ' .fpret = %d },'
                            % (key, cname, xname, argc,
                               "|".join(flags), plan.aux, plan.aux2, kname,
                               plan.fpmask, plan.fpwide, plan.xmask,
                               plan.narrowmask, plan.narrowwide,
                               plan.narrowsign, plan.fpret))
                stats["marshalled"] += 1
                stats["fp_served"] = stats.get("fp_served", 0) + 1
                continue
            rows.append('    { "%s", NULL, %s, %s, %d, %s, %d, %d, %s, 0, 0,'
                        ' 0x%02x, 0x%02x, 0x%02x, 0x%02x },'
                        % (key, cname, xname, argc,
                           "|".join(flags) or "0", plan.aux, plan.aux2, kname,
                           plan.xmask, plan.narrowmask, plan.narrowwide,
                           plan.narrowsign))
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
