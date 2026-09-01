#!/usr/bin/env python3
"""Extract the DirectInput8 COM vtable layout from Wine's own dinput.h, and
emit the two files the two halves of the boundary are built from.

  ./gen_dinput_surface.py --json interfaces_dinput.json \
                          --marshal ../../dlls/dinput8/dinput8_marshal.h
  ./gen_dinput_surface.py --check          # regenerate both and diff
  ./gen_dinput_surface.py --report         # what is marshalled, what refused

WHY ONE FILE PRODUCES BOTH.  The guest x86-64 thunk module (tools/spec2thunk
COM mode, from dlls/dinput8/dinput8.thunks) and the native module's marshal
tables (dlls/dinput8/dinput8_marshal.h, consumed by dlls/dinput8/guestcom.c)
describe the SAME vtables.  If the two drifted by one slot, the symptom would
be a call dispatched to the neighbouring method with the neighbour's argument
types -- silently, at runtime.  So the roster JSON is written once and read by
both, libs/winecom cross-checks every IID and slot_count at attach, and this
script's --check mode makes a stale checked-in file a build failure rather
than a runtime mystery.  Same arrangement, same reason, as
ppc64le/dxvk/gen_interfaces.py + gen_winecom.py.

WHY WINE'S HEADER AND NOT A VENDORED ONE.  Unlike the DXVK lane, the
implementation on the other side of this boundary IS Wine's own dinput8 --
dlls/dinput8/dinput_main.c and device.c, compiled for ppc64 from these very
declarations.  include/dinput.h therefore is the layout, not a description of
it.

SLOT ORDER IS NOT A FREE CHOICE, and here it is not even a computation:
Wine's dinput.h uses DECLARE_INTERFACE_ and RESTATES every inherited method in
order inside each derived interface's block, so the block IS the flattened
vtable.  The `/*** IDirectInputDeviceA methods ***/` comments that separate
the groups give each slot its owning interface, which is what winecom keys
hand-written and refused slots on.

CLASSIFY OR REFUSE.  Every parameter of every slot is classified before the
call may cross.  An interface pointer that crossed unclassified would reach
the guest as a NATIVE ppc64 vtable and the guest's first method call on it
would execute ppc64 bytes as x86-64.  So a parameter spelling this script does
not recognise STOPS GENERATION rather than becoming a silent CA_PASS.

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
WINE = os.path.normpath(os.path.join(HERE, "..", ".."))
HEADER = os.path.join(WINE, "include", "dinput.h")

DEFAULT_JSON = os.path.join(HERE, "interfaces_dinput.json")
DEFAULT_MARSHAL = os.path.join(WINE, "dlls", "dinput8", "dinput8_marshal.h")

# --------------------------------------------------------------------------
# THE SURFACE.  dinput.h declares twenty interfaces -- the whole DirectInput
# 3/5/7/8 lineage in both character widths -- and this roster deliberately
# names only what dinput8.dll can actually vend to a guest:
#
#   IDirectInput8A/W        what DirectInput8Create returns
#   IDirectInputDevice8A/W  what IDirectInput8::CreateDevice returns
#   IDirectInputEffect      what IDirectInputDevice8::CreateEffect returns
#
# The DirectInput 7 and earlier interfaces belong to dinput.dll, which is a
# separate module with its own guest thunk and no roster (see
# dlls/dinput/dinput.thunks): a guest that wants them is running a
# pre-2001 title, and adding them here would publish vtables no dinput8
# entry point can ever hand out.  IUnknown is added because it is the root of
# every vtable and winecom serves its three slots itself.
# --------------------------------------------------------------------------
SURFACE = [
    "IDirectInput8A", "IDirectInput8W",
    "IDirectInputDevice8A", "IDirectInputDevice8W",
    "IDirectInputEffect",
]

IUNKNOWN = {
    "uuid": "00000000-0000-0000-c000-000000000046",
    "base": None,
    "header": "(builtin)",
    "slots": [
        {"slot": 0, "owner": "IUnknown", "name": "QueryInterface",
         "ret": "HRESULT", "params": ["REFIID riid", "void **ppvObject"]},
        {"slot": 1, "owner": "IUnknown", "name": "AddRef",
         "ret": "ULONG", "params": []},
        {"slot": 2, "owner": "IUnknown", "name": "Release",
         "ret": "ULONG", "params": []},
    ],
}

# --------------------------------------------------------------------------
# parsing dinput.h
# --------------------------------------------------------------------------
IFACE_RE = re.compile(
    r'^DECLARE_INTERFACE_\((\w+)\s*,\s*(\w+)\)\s*$\n\{(.*?)^\};',
    re.MULTILINE | re.DOTALL)
# STDMETHOD(Name)(THIS_ args) PURE;  -- HRESULT return, implicit
# STDMETHOD_(RET,Name)(THIS_ args) PURE;
METHOD_RE = re.compile(
    r'STDMETHOD(_)?\(\s*(?:(\w+)\s*,\s*)?(\w+)\s*\)\s*\(\s*THIS(_)?\s*(.*?)\)\s*PURE\s*;',
    re.DOTALL)
GROUP_RE = re.compile(r'/\*\*\*\s*(\w+)\s+methods\s*\*\*\*/')
GUID_RE = re.compile(
    r'^DEFINE_GUID\(\s*IID_(\w+)\s*,\s*(0x[0-9a-fA-F]+)\s*,\s*(0x[0-9a-fA-F]+)\s*,'
    r'\s*(0x[0-9a-fA-F]+)\s*,\s*((?:0x[0-9a-fA-F]+\s*,\s*){7}0x[0-9a-fA-F]+)\s*\)',
    re.MULTILINE)


def parse_header(path):
    with open(path, errors="replace") as fh:
        text = fh.read()

    uuids = {}
    for m in GUID_RE.finditer(text):
        d4 = [int(x, 16) for x in m.group(5).replace(" ", "").split(",")]
        uuids[m.group(1)] = "%08x-%04x-%04x-%02x%02x-%s" % (
            int(m.group(2), 16), int(m.group(3), 16), int(m.group(4), 16),
            d4[0], d4[1], "".join("%02x" % b for b in d4[2:]))

    ifaces = {}
    for m in IFACE_RE.finditer(text):
        name, base, body = m.group(1), m.group(2), m.group(3)
        # Walk the body in source order so a `/*** X methods ***/` comment
        # claims every method that follows it until the next one -- that
        # comment is how this header records which interface in the lineage
        # OWNS each slot, and winecom keys refusals on owner::method so that an
        # inherited slot carries the same verdict in every derived vtable.
        # A method before any group comment is the interface's own; that does
        # not happen in dinput.h, but the fallback must not be silent.
        slots, cur = [], name
        events = sorted(
            [(g.start(), 0, g.group(1)) for g in GROUP_RE.finditer(body)] +
            [(x.start(), 1, x) for x in METHOD_RE.finditer(body)])
        for _off, kind, val in events:
            if kind == 0:
                cur = val
                continue
            slots.append({"slot": len(slots), "owner": cur,
                          "name": val.group(3),
                          "ret": (val.group(2) or "HRESULT").strip(),
                          "params": split_params(
                              " ".join((val.group(5) or "").split()))})
        ifaces[name] = {"uuid": uuids.get(name), "base": base,
                        "header": "dinput.h", "slots": slots}
    return ifaces


def split_params(text):
    """Split a parameter list on top-level commas; '' and 'void' -> []."""
    text = text.strip()
    if not text or text == "void":
        return []
    out, depth, cur = [], 0, ""
    for ch in text:
        if ch in "([":
            depth += 1
        elif ch in ")]":
            depth -= 1
        if ch == "," and depth == 0:
            out.append(cur.strip())
            cur = ""
        else:
            cur += ch
    if cur.strip():
        out.append(cur.strip())
    return out


# --------------------------------------------------------------------------
# classification -- fail closed
# --------------------------------------------------------------------------
CA = dict(PASS=0, IFACE_IN=1, RIID=2, PPV_OUT=3, RET_PTR=4, EVENT=5,
          IFACE_ARR_IN=6, IFACE_OUT_STATIC=7, IFACE_ARR_OUT_STATIC=8)
CA_NAME = {v: "WINECOM_CA_" + k for k, v in CA.items()}

# By-value spellings that cross as one integer register and mean the same
# thing on both sides.  ANYTHING NOT LISTED STOPS GENERATION.
#
# HWND and HANDLE are here, and that is the one place this surface differs
# from the DXVK one, where both are refused by name.  The reason is concrete:
# on this lane the implementation is WINE's dinput8, in the same Win32 world
# and the same process as the guest.  A guest's HWND came from user32 through
# a flat thunk and IS a Wine window; the HANDLE SetEventNotification takes came
# from kernel32 and IS a Wine event.  There is no second namespace to collide
# with -- which is exactly what made DXVK's tagged eventfd a hazard there.
BYVAL_INTEGER = frozenset("""
    DWORD UINT INT LONG ULONG BOOL WORD BYTE HRESULT
    HWND HINSTANCE HANDLE LPARAM WPARAM UINT_PTR DWORD_PTR
""".split())

# Pointer spellings with no `*` in the source text.  Every one of these is a
# pointer to PLAIN DATA -- GUIDs, DWORDs, fixed CHAR/WCHAR arrays, and arrays
# of those -- checked member by member against dinput.h.  None reaches an
# interface pointer or a function pointer, which is why they may pass.
#
# WCHAR IS NOT A HAZARD ON THIS PORT, and it is worth saying because it is on
# the DXVK one: Wine's PE-side build is -fshort-wchar, so a WCHAR is two bytes
# on both sides of this boundary.  The DIDEVICEINSTANCEW/DIEFFECTINFOW/
# DIDEVICEOBJECTINSTANCEW string fields therefore cross byte for byte.
PLAIN_POINTER_TYPEDEFS = frozenset("""
    REFGUID REFIID LPGUID LPCGUID LPVOID LPDWORD LPCSTR LPCWSTR LPSTR LPWSTR
    LPDIDEVCAPS LPDIPROPHEADER LPCDIPROPHEADER LPCDIDATAFORMAT
    LPDIDEVICEOBJECTDATA LPCDIDEVICEOBJECTDATA
    LPDIDEVICEOBJECTINSTANCEA LPDIDEVICEOBJECTINSTANCEW
    LPDIDEVICEINSTANCEA LPDIDEVICEINSTANCEW
    LPDIEFFECT LPCDIEFFECT LPDIEFFECTINFOA LPDIEFFECTINFOW LPDIEFFESCAPE
    LPDIACTIONFORMATA LPDIACTIONFORMATW LPDIFILEEFFECT
    LPDIDEVICEIMAGEINFOHEADERA LPDIDEVICEIMAGEINFOHEADERW
    LPDICONFIGUREDEVICESPARAMSA LPDICONFIGUREDEVICESPARAMSW
""".split())

# Statically-typed out-parameters: the spelling of the pointed-to interface
# pointer -> the roster name whose guest vtable the returned host object gets.
IFACE_OUT_STATIC = {
    "LPDIRECTINPUTDEVICE8A": "IDirectInputDevice8A",
    "LPDIRECTINPUTDEVICE8W": "IDirectInputDevice8W",
    "LPDIRECTINPUTEFFECT":   "IDirectInputEffect",
}

# Interface pointers arriving from the guest.  Every one of these in this
# surface is an aggregation pUnkOuter, which must be NULL -- a non-NULL one is
# a GUEST-implemented IUnknown handed to native code, the reverse-proxy
# direction winecom does not have.  winecom_translate_in answers FALSE for it
# and the slot is refused at that point, not here.
IFACE_IN = frozenset(("LPUNKNOWN", "IUnknown"))

# Callback typedefs.  Native dinput retains the pointer for the duration of
# the enumeration (or, for ConfigureDevices, of a modal dialog) and calls it
# once per item from a NATIVE frame.  There is no winecom argument class for a
# callback and no way for this module to make one -- see REFUSALS below.
CALLBACK_TYPEDEFS = frozenset("""
    LPDIENUMDEVICESCALLBACKA LPDIENUMDEVICESCALLBACKW
    LPDIENUMDEVICEOBJECTSCALLBACKA LPDIENUMDEVICEOBJECTSCALLBACKW
    LPDIENUMEFFECTSCALLBACKA LPDIENUMEFFECTSCALLBACKW
    LPDIENUMCREATEDEFFECTOBJECTSCALLBACK
    LPDIENUMEFFECTSINFILECALLBACK
    LPDIENUMDEVICESBYSEMANTICSCBA LPDIENUMDEVICESBYSEMANTICSCBW
    LPDICONFIGUREDEVICESCALLBACK
""".split())

CALLBACK_REFUSAL = (
    "takes a guest callback WHOSE OWN FIRST ARGUMENT IS AN INTERFACE POINTER "
    "-- a device, an effect, or an array of them inside a struct.  The port "
    "wraps a guest function pointer at registration with ntdll's trampoline "
    "pool (__wine_guest_wrap_callback), and that is what the served Enum* "
    "slots below do; but a trampoline carries its arguments through "
    "UNTRANSLATED, so the native pointer dinput passes would reach the guest "
    "as a ppc64 vtable.  Serving one needs a per-callback shim that wraps that "
    "argument as a proxy first, which is a hand-written slot of its own rather "
    "than a table entry.  Refused by name until something needs it"
)

# --------------------------------------------------------------------------
# HAND-WRITTEN SLOTS.  Keyed "Owner::Method" like the other surfaces'
# generators, and the order here IS the hand_funcs[] order in
# dlls/dinput8/guestcom.c.
#
# Every one is an enumeration whose callback receives PLAIN DATA and the
# caller's own pvRef -- a DIDEVICEINSTANCE, a DIDEVICEOBJECTINSTANCE, a
# DIEFFECTINFO, a DIFILEEFFECT -- so the guest function pointer can be swapped
# for one of ntdll's trampolines at the moment it arrives and everything else
# crosses as an address.  That is the WHOLE mechanism for a bare callback, and
# it is deliberately NOT a reverse proxy: a reverse proxy is a vtable, and a
# DIENUMDEVICESCALLBACK has no vtable to build.
#
# The value is the (hand function, callback argument index counting AFTER
# `this`), and the index is checked against the signature below so a parameter
# list that changed shape stops generation instead of wrapping the wrong
# argument.
# Named by SHAPE rather than by method, the way ppc64le/audio's float hands
# are, because that is what they actually are: "the callback is argument N".
# EnumDevices and EnumEffectsInFile marshal identically and share one function.
HAND_SLOTS = {
    "IDirectInput8A::EnumDevices":             ("hand_enum_cb2", 1),
    "IDirectInput8W::EnumDevices":             ("hand_enum_cb2", 1),
    "IDirectInputDevice7A::EnumEffectsInFile": ("hand_enum_cb2", 1),
    "IDirectInputDevice7W::EnumEffectsInFile": ("hand_enum_cb2", 1),
    "IDirectInputDeviceA::EnumObjects":        ("hand_enum_cb1", 0),
    "IDirectInputDeviceW::EnumObjects":        ("hand_enum_cb1", 0),
    "IDirectInputDevice2A::EnumEffects":       ("hand_enum_cb1", 0),
    "IDirectInputDevice2W::EnumEffects":       ("hand_enum_cb1", 0),
    # 2026-09-01, the completeness pass: the three callbacks whose OWN first
    # (or second) argument is an interface pointer.  Each is served by a
    # per-callback SHIM in dlls/dinput8/guestcom.c that wraps that argument
    # as a proxy BEFORE entering the guest trampoline -- exactly what
    # CALLBACK_REFUSAL below says serving one needs.  The refusal text
    # stays, for any FUTURE callback typedef that arrives without a shim.
    "IDirectInput8A::EnumDevicesBySemantics":  ("hand_enum_semantics_a", 2),
    "IDirectInput8W::EnumDevicesBySemantics":  ("hand_enum_semantics_w", 2),
    "IDirectInput8A::ConfigureDevices":        ("hand_configure_devices", 0),
    "IDirectInput8W::ConfigureDevices":        ("hand_configure_devices", 0),
    "IDirectInputDevice2A::EnumCreatedEffectObjects":
                                               ("hand_enum_created_fx", 0),
    "IDirectInputDevice2W::EnumCreatedEffectObjects":
                                               ("hand_enum_created_fx", 0),
}


class Refused(Exception):
    pass


def base_type(raw):
    """-> (base spelling, number of '*')."""
    t = " ".join(raw.split())
    t = re.sub(r'\bconst\b', ' ', t)
    stars = t.count("*")
    toks = [x for x in t.replace("*", " ").split() if x]
    if not toks:
        raise Refused("empty parameter")
    return toks[0], stars


def classify_slot(iface_order, owner, method, params):
    """-> (classes, xaux, aux, refusal-or-None).  Raises Refused on a shape
    this generator does not recognise -- never a silent CA_PASS."""
    cls = [CA["PASS"]] * len(params)
    xaux = [0] * len(params)
    xmask = 0
    aux = 0

    # A callback anywhere in the list refuses the WHOLE slot before anything
    # else is classified: there is no partial version of this call that is
    # safe, and classifying the other arguments first would only produce a row
    # that looks servable.
    key = "%s::%s" % (owner, method)
    for i, p in enumerate(params):
        if base_type(p)[0] not in CALLBACK_TYPEDEFS:
            continue
        if key in HAND_SLOTS:
            # Served by hand: the generator's job is to prove the callback is
            # where the hand function believes it is, and then get out of the
            # way.  A parameter list that moved stops generation rather than
            # letting the hand function wrap the wrong argument.
            if HAND_SLOTS[key][1] != i:
                raise Refused(
                    "%s: HAND_SLOTS says its callback is argument %d, but the "
                    "signature has it at %d"
                    % (key, HAND_SLOTS[key][1], i))
            return cls, xaux, xmask, aux, None
        return cls, xaux, xmask, aux, "%s::%s %s" % (owner, method, CALLBACK_REFUSAL)

    for i, p in enumerate(params):
        base, stars = base_type(p)
        if base in ("REFIID", "REFCLSID") and i + 1 < len(params):
            nb, ns = base_type(params[i + 1])
            if nb == "void" and ns == 2:
                cls[i] = CA["RIID"]
                cls[i + 1] = CA["PPV_OUT"]
                aux = i
                continue
        if cls[i] != CA["PASS"]:
            continue                      # already claimed by the pair above
        if base in IFACE_OUT_STATIC and stars == 1:
            name = IFACE_OUT_STATIC[base]
            if name not in iface_order:
                raise Refused("%s::%s vends %s, which is not in the roster"
                              % (owner, method, name))
            cls[i] = CA["IFACE_OUT_STATIC"]
            xaux[i] = iface_order.index(name)
            xmask |= 1 << i
            continue
        if base in IFACE_IN and stars == 0:
            # The TYPE goes in xaux even though the forward direction
            # recognises an IN pointer by identity: libs/winecom's reverse
            # dispatcher has to give a NATIVE object arriving at a guest
            # method one of the rostered guest vtables, and identity cannot
            # say which.  Aggregation's pUnkOuter is the only one here.
            if "IUnknown" not in iface_order:
                raise Refused("%s::%s takes an IUnknown and the roster has no "
                              "IUnknown to type it with" % (owner, method))
            cls[i] = CA["IFACE_IN"]
            xaux[i] = iface_order.index("IUnknown")
            xmask |= 1 << i
            continue
        if base in PLAIN_POINTER_TYPEDEFS or base in BYVAL_INTEGER:
            continue                      # CA_PASS, and the reason is checked
        if base == "void" and stars:
            raise Refused(
                "%s::%s argument %d is a bare `%s` with no REFIID to type it; "
                "an untyped out-pointer is refused because an interface that "
                "crossed through one would get no guest vtable at all"
                % (owner, method, i, p))
        raise Refused("%s::%s argument %d (%s): unrecognised type `%s`"
                      % (owner, method, i, p, base))
    return cls, xaux, xmask, aux, None


# --------------------------------------------------------------------------
# emitting
# --------------------------------------------------------------------------
def build_roster(ifaces):
    out = {}
    for name in SURFACE:
        if name not in ifaces:
            sys.exit("gen_dinput_surface: %s is not declared in %s"
                     % (name, HEADER))
        i = ifaces[name]
        if not i["uuid"]:
            sys.exit("gen_dinput_surface: no DEFINE_GUID(IID_%s, ...) in %s"
                     % (name, HEADER))
        out[name] = i
    out["IUnknown"] = IUNKNOWN
    return {
        "surface": "wine-dinput8",
        "iface_ptr_aliases": {alias: name
                              for alias, name in sorted(IFACE_OUT_STATIC.items())},
        "enums": [],
        "interfaces": out,
    }


def c_string(text, indent, width=76):
    """A C string literal wrapped across lines by concatenation, so a long
    refusal reason does not become a 500-column source line."""
    words, lines, cur = text.split(), [], ""
    for w in words:
        cand = (cur + " " + w) if cur else w
        if len(cand) + indent + 3 > width and cur:
            lines.append(cur + " ")
            cur = w
        else:
            cur = cand
    if cur:
        lines.append(cur)
    pad = " " * indent
    return ("\n" + pad).join('"%s"' % l.replace('\\', '\\\\').replace('"', r'\"')
                             for l in lines)


def wrap_comment(text, prefix=" * ", width=76):
    words, lines, cur = text.split(), [], ""
    for w in words:
        cand = (cur + " " + w) if cur else w
        if len(cand) + len(prefix) > width and cur:
            lines.append(cur)
            cur = w
        else:
            cur = cand
    if cur:
        lines.append(cur)
    return "\n".join(prefix + l for l in lines)


def guid_c(uuid):
    a, b, c, d, e = uuid.split("-")
    tail = ",".join("0x%s" % (d + e)[i:i + 2] for i in range(0, 16, 2))
    return "{0x%s,0x%s,0x%s,{%s}}" % (a, b, c, tail)


def emit_marshal(roster, prefix="dinput8"):
    order = sorted(roster["interfaces"])
    stats = dict(marshalled=0, refused=0, iunknown=0)
    body = []

    up = prefix.upper()
    head = ["""/* GENERATED by ppc64le/shell/gen_dinput_surface.py -- do not edit.
 *
 * Marshal tables for the %s surface (%d interfaces, %d vtable slots),
 * generated from ppc64le/shell/interfaces_dinput.json -- the same roster
 * dlls/dinput8/dinput8.thunks hands spec2thunk to build the guest trap
 * module.  Interface order is sorted by name, which is the order spec2thunk
 * COM mode gives the guest stub arrays; libs/winecom cross-checks every IID
 * and slot count at attach, so the two generators cannot silently disagree.
 *
 * Slot/iface types and WINECOM_CA_* classes come from include/wine/winecom.h,
 * which must be included before this file.
 */

enum %s_iface_index
{""" % (roster["surface"], len(order),
        sum(len(roster["interfaces"][n]["slots"]) for n in order),
        prefix)]
    for n, name in enumerate(order):
        head.append("    %s_IFACE_%s = %d," % (up, name, n))
    head.append("    %s_IFACE_COUNT = %d" % (up, len(order)))
    head.append("};")
    head.append("")
    # hand_funcs[] order, assigned in HAND_SLOTS iteration order so that
    # adding a slot that reuses an existing function renumbers nothing.
    hand_order, hand_index = [], {}
    for k, (fn, _idx) in HAND_SLOTS.items():
        if fn not in hand_order:
            hand_order.append(fn)
        hand_index[k] = hand_order.index(fn)
    head.append("#define %s_HAND_COUNT %d" % (up, len(hand_order)))
    head.append("/* hand_funcs[] order in dlls/dinput8/guestcom.c:")
    for i, fn in enumerate(hand_order):
        head.append(" *   %d %s" % (i, fn))
    head.append(" */")
    head.append("")

    refusals, hands = [], []
    for name in order:
        info = roster["interfaces"][name]
        slots = info["slots"]
        if name == "IUnknown":
            stats["iunknown"] += len(slots)
            continue
        rows = []
        for s in slots:
            argc = 1 + len(s["params"])
            if s["owner"] == "IUnknown":
                # Served by the runtime itself: winecom answers
                # QueryInterface/AddRef/Release out of the proxy.  The row
                # still has to exist and be the right width.
                rows.append('    { "%s::%s", NULL, NULL, NULL, %d, 0, 0, 0,'
                            ' NULL, 0, 0, 0 },'
                            % (s["owner"], s["name"], argc))
                stats["iunknown"] += 1
                continue
            try:
                cls, xaux, xmask, aux, refuse = classify_slot(
                    order, s["owner"], s["name"], s["params"])
            except Refused as e:
                sys.exit("gen_dinput_surface: FATAL, generation stopped: %s" % e)
            if refuse:
                rows.append('    { "%s::%s",\n      %s,\n'
                            '      NULL, NULL, %d, 0, 0, 0, NULL, 0, 0, 0 },'
                            % (s["owner"], s["name"], c_string(refuse, 6), argc))
                stats["refused"] += 1
                refusals.append("%s::%s" % (s["owner"], s["name"]))
                continue
            key = "%s::%s" % (s["owner"], s["name"])
            if key in hand_index:
                rows.append('    { "%s", NULL, NULL, NULL, %d, WINECOM_F_HAND,'
                            ' %d, 0, NULL, 0, 0, 0 },'
                            % (key, argc, hand_index[key]))
                stats["hand"] = stats.get("hand", 0) + 1
                hands.append(key)
                continue
            stats["marshalled"] += 1
            if all(c == CA["PASS"] for c in cls):
                rows.append('    { "%s::%s", NULL, NULL, NULL, %d, 0, 0, 0,'
                            ' NULL, 0, 0, 0 },'
                            % (s["owner"], s["name"], argc))
                continue
            cname = "cls_%s_%s" % (name, s["name"])
            body.append("static const unsigned char %s[%d] = { %s };"
                        % (cname, len(cls),
                           ", ".join(CA_NAME[c] for c in cls)))
            xname = "NULL"
            if xmask:
                xname = "xaux_%s_%s" % (name, s["name"])
                body.append("static const unsigned char %s[%d] = { %s };"
                            % (xname, len(xaux), ", ".join(str(x) for x in xaux)))
            rows.append('    { "%s::%s", NULL, %s, %s, %d, 0, %d, 0, NULL,'
                        ' 0, 0, 0x%02x },'
                        % (s["owner"], s["name"], cname, xname, argc, aux,
                           xmask))
        body.append("")
        body.append("static const struct winecom_slot slots_%s[%d] =\n{"
                    % (name, len(slots)))
        body.extend(rows)
        body.append("};")

    tail = ["", "static const struct winecom_iface %s_com_ifaces[%s_IFACE_COUNT] ="
            % (prefix, up), "{"]
    for name in order:
        info = roster["interfaces"][name]
        n = len(info["slots"])
        arr = "NULL" if name == "IUnknown" else "slots_%s" % name
        tail.append('    { "%s", %s,\n      %d, %s, 0 },'
                    % (name, guid_c(info["uuid"]), n, arr))
    tail.append("};")
    tail.append("")
    summary = ("/*\n" + wrap_comment(
        "%d slot(s) marshalled, %d hand-written, %d refused with a named "
        "reason, %d IUnknown slot(s) served by the runtime."
        % (stats["marshalled"], stats.get("hand", 0), stats["refused"],
           stats["iunknown"])))
    if hands:
        summary += ("\n *\n" + wrap_comment(
            "Hand-written (a guest callback swapped for one of ntdll's "
            "trampolines at the moment it arrives): " + ", ".join(hands)))
    if refusals:
        summary += ("\n *\n" + wrap_comment("Refused: " + ", ".join(refusals)
                                            + "."))
    tail.append(summary + "\n */")
    return "\n".join(head + body + tail) + "\n", stats, refusals


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--json", default=None)
    ap.add_argument("--marshal", default=None)
    ap.add_argument("--check", action="store_true",
                    help="regenerate both and diff against what is checked in")
    ap.add_argument("--report", action="store_true")
    args = ap.parse_args()

    ifaces = parse_header(HEADER)
    roster = build_roster(ifaces)
    text, stats, refusals = emit_marshal(roster)
    js = json.dumps(roster, indent=2, sort_keys=True) + "\n"

    if args.check:
        bad = 0
        for path, want in ((DEFAULT_JSON, js), (DEFAULT_MARSHAL, text)):
            try:
                got = open(path).read()
            except OSError as e:
                print("gen_dinput_surface: %s" % e)
                bad = 1
                continue
            if got != want:
                print("gen_dinput_surface: %s is STALE against include/dinput.h"
                      % os.path.relpath(path, WINE))
                bad = 1
            else:
                print("gen_dinput_surface: %s is current"
                      % os.path.relpath(path, WINE))
        return bad

    if args.json:
        with open(args.json, "w") as fh:
            fh.write(js)
        print("gen_dinput_surface: wrote %s" % args.json)
    if args.marshal:
        with open(args.marshal, "w") as fh:
            fh.write(text)
        print("gen_dinput_surface: wrote %s" % args.marshal)

    print("surface %s: %d interface(s), %d slot(s) -- %d marshalled, %d "
          "refused, %d IUnknown served by the runtime"
          % (roster["surface"], len(roster["interfaces"]),
             sum(len(i["slots"]) for i in roster["interfaces"].values()),
             stats["marshalled"], stats["refused"], stats["iunknown"]))
    if args.report:
        for r in refusals:
            print("  refused  %s" % r)
    return 0


if __name__ == "__main__":
    sys.exit(main())
