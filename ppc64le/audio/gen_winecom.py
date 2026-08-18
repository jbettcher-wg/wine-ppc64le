#!/usr/bin/env python3
"""Emit the winecom marshal tables for the AUDIO surfaces.

  ./gen_winecom.py --surface dsound    --out ../../dlls/dsound/dsound_marshal.h
  ./gen_winecom.py --surface xaudio2_9 --out ../../dlls/xaudio2_9/xaudio2_marshal.h
  ./gen_winecom.py --surface dsound    --report
  ./gen_winecom.py --surface dsound    --check ../../dlls/dsound/dsound_marshal.h

INPUT is ppc64le/audio/interfaces_<surface>.json -- the ONE roster, the same
file dlls/dsound/dsound.thunks hands spec2thunk to build the guest trap module.
The two generators must agree about interface order and slot counts or a call
lands on the neighbouring slot with the neighbour's argument types; both sort by
interface name, and libs/winecom cross-checks every IID and slot_count at attach
as the last line of defence.

This is the DXVK lane's gen_winecom.py (ppc64le/dxvk/gen_winecom.py) with three
differences that come from what is on the other side of the boundary, not from
taste:

  * NO WCHAR REFUSAL.  The DXVK lane refuses every WCHAR-bearing slot because
    DXVK's native headers typedef WCHAR to a 4-byte wchar_t while the guest
    PE's is 2.  Here the implementation is Wine's own PE-side dsound/xaudio2,
    compiled with -fshort-wchar like every other Wine PE, so a WCHAR is two
    bytes on both sides of the call and IXAudio2::CreateMasteringVoice's
    LPCWSTR DeviceId crosses as an address like any other pointer.

  * BY-VALUE FLOATS ARE SERVED, not refused.  The DXVK lane's invoker crosses
    a unixlib with an all-integer parameter block; this one calls the native
    PE vtable directly, so a float argument can be picked out of the trap
    CONTEXT's XMM save area and passed in a real prototype.  Each float-bearing
    slot is routed to a hand-written function by its ARGUMENT SHAPE (see
    FP_SHAPES) rather than by name, so a new slot with an existing shape is
    served automatically and a new shape is a named refusal rather than a
    silent integer-register call.

  * INTERFACES WITH NO IUnknown.  IXAudio2Voice and its three derivatives are
    `[local]`: slot 0 is GetVoiceDetails, not QueryInterface.  Their rows carry
    real marshal entries in slots 0..2 and the generated header marks them in
    <prefix>_iface_local[], because libs/winecom's dispatcher serves slots 0..2
    from the proxy table unconditionally -- so the CLIENT module's
    __wine_com_dispatch must claim those interfaces before winecom_dispatch
    ever sees them.  The generator refuses to emit a local interface's table
    unless it is told the client does that (--locals-claimed), because getting
    it wrong turns GetVoiceDetails into QueryInterface.

CLASSIFY OR REFUSE, never pass-and-hope: an interface pointer that crosses
unclassified is handed to the guest as a NATIVE vtable, and the guest's first
method call through it executes ppc64 bytes as x86-64.  A parameter shape the
classifier does not recognise stops generation rather than emitting a CA_PASS.

Copyright 2026 the ppc64le port authors

This library is free software; you can redistribute it and/or modify it under
the terms of the GNU Lesser General Public License as published by the Free
Software Foundation; either version 2.1 of the License, or (at your option) any
later version.
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
# both sides.  ANYTHING NOT LISTED STOPS GENERATION -- "it is probably an enum"
# is how a by-value aggregate silently becomes four bytes of garbage.  The
# roster's enum names are added at run time; an enum is int-class in both ABIs.
# --------------------------------------------------------------------------
BYVAL_INTEGER = frozenset("""
    UINT INT LONG ULONG DWORD WORD BYTE BOOL WINBOOL UINT8 UINT16 UINT32
    UINT64 INT8 INT16 INT32 INT64 SIZE_T SSIZE_T ULONG64 LONG64 HRESULT
    unsigned int short char long
    HWND HANDLE HMODULE HMONITOR HINSTANCE
""".split())

# By-value types that ARE integer-class but still may not cross, because the
# integer means something different on each side.  Refused by name.
#
# NOTE what is NOT here, and why: HWND and HANDLE.  On this surface both sides
# of the call are ordinary Wine PE code in the SAME process and the same Wine
# object namespace -- IDirectSound::SetCooperativeLevel's HWND is a Wine window
# handle produced by the guest's own user32 thunk and consumed by Wine's own
# dsound.  The DXVK lane refuses them because DXVK's native side has its own
# handle encodings; there is no second namespace here.
BYVAL_OPAQUE = {}

# Pointer spellings that are pointers without a `*` in the source text and
# that no header states as a typedef this generator can read: REFIID and its
# siblings are #defines in C mode, and LPCWSTR/LPSTR come from windef.h's
# CONST/NEAR/FAR macro layer.  Everything else -- and DirectSound's headers are
# nothing but LP-spellings -- is LEARNED from the headers by
# scan_pointer_typedefs, because a hand list of them is exactly the kind of
# thing that goes stale silently.
POINTER_TYPEDEFS = {
    "LPSTR": ("char", 1), "LPCSTR": ("char", 1),
    "LPWSTR": ("WCHAR", 1), "LPCWSTR": ("WCHAR", 1),
    "REFIID": ("GUID", 1), "REFGUID": ("GUID", 1), "REFCLSID": ("GUID", 1),
    "LPVOID": ("void", 1), "LPCVOID": ("void", 1),
    # unknwn.h is widl output and lives in the build tree, not include/
    "LPUNKNOWN": ("IUnknown", 1), "PUNKNOWN": ("IUnknown", 1),
}

COMMENT_RE = re.compile(r'/\*.*?\*/|//[^\n]*', re.DOTALL)
TYPEDEF_KW = re.compile(r'\btypedef\b')


def typedef_statements(text):
    """Yield each `typedef ... ;` statement with its {...} bodies removed, so
    `typedef struct _DSCAPS {...} DSCAPS,*LPDSCAPS;` and `typedef const
    DSBUFFERDESC *LPCDSBUFFERDESC;` arrive in the same shape.

    Scanned statement by statement rather than by stripping every brace in the
    file first: a Windows header is full of preprocessor branches whose braces
    do not balance in the raw text, and one unbalanced `{` would swallow the
    rest of the file."""
    text = COMMENT_RE.sub(" ", text)
    for m in TYPEDEF_KW.finditer(text):
        i, depth, out = m.end(), 0, []
        while i < len(text):
            ch = text[i]
            if ch == "{":
                depth += 1
            elif ch == "}":
                depth -= 1
                if depth < 0:
                    break            # ran out of the enclosing construct
            elif ch == ";" and not depth:
                yield "".join(out)
                break
            elif not depth:
                out.append(ch)
            i += 1


def scan_pointer_typedefs(texts):
    """-> {name: (base, extra_stars)}.

    Learned from the headers rather than listed here, because DirectSound's
    surface is written entirely in LP-spellings -- LPDIRECTSOUNDBUFFER,
    LPCDSBUFFERDESC, LPD3DVALUE, LPLPDIRECTSOUNDBUFFER -- and a classifier
    that did not resolve them would see `LPCDSBUFFERDESC` as an unknown
    BY-VALUE type and refuse the whole of CreateSoundBuffer, or, worse, see
    LPDIRECTSOUNDBUFFER as plain data and pass a native vtable to the guest."""
    out = {}
    for text in texts:
        for stmt in typedef_statements(text):
            parts = [p.strip() for p in stmt.split(",")]
            if not parts:
                continue
            head = parts[0].replace("*", " * ").split()
            head = [t for t in head if t not in ("const", "volatile",
                                                 "struct", "union", "enum",
                                                 "interface", "CONST")]
            if len(head) < 2:
                continue
            # the base type is everything before the first declarator token
            stars0 = 0
            i = 0
            while i < len(head) - 1 and head[i] != "*":
                i += 1
            base = " ".join(head[:i]) if i else head[0]
            if not base or not re.match(r'^[\w ]+$', base):
                continue
            base = base.split()[-1]
            decls = [" ".join(head[i:])] + parts[1:]
            for d in decls:
                toks = d.replace("*", " * ").split()
                stars = toks.count("*") + stars0
                names = [t for t in toks if t != "*"]
                if len(names) != 1 or not re.match(r'^\w+$', names[0]):
                    continue
                name = names[0]
                if name == base or name in out:
                    continue
                out[name] = (base, stars)
    return out

FLOAT_TOKENS = re.compile(r'\b(FLOAT|float|double|DOUBLE|D3DVALUE)\b')

# A COM interface name that survived typedef resolution without being in the
# roster.  The fail-closed rule: an `IUnknown *pUnkOuter` that reached the
# generic pointer class would be handed to Wine's dsound as a guest-side
# pointer it would then call.  IID/IUnknown-shaped NON-interface spellings
# resolve to their real base (IID -> GUID) before this is reached, so the
# pattern sees only what is genuinely an unrostered interface.
UNROSTERED_IFACE_RE = re.compile(r'^I[A-Z]\w*$')


# --------------------------------------------------------------------------
# FLOAT SHAPES.  Keyed by the argument-class string of the parameters AFTER
# `this` -- 'i' for anything that travels in an integer register (including
# every pointer) and 'f' for a by-value float -- plus '>' and the return class.
# The value is the hand function that implements exactly that prototype.
#
# Keying on the SHAPE rather than the method name is the point: every one of
# these is a different method on a different interface with the same ABI, and
# one hand function serves all of them.  A float-bearing slot whose shape is
# not here is REFUSED with the shape printed, which is a two-line addition
# here plus one hand function rather than a silent wrong-register call.
# --------------------------------------------------------------------------
FP_SHAPES = {
    # IDirectSound3DListener::SetDistanceFactor / SetDopplerFactor /
    # SetRolloffFactor, IDirectSound3DBuffer::SetMinDistance /
    # SetMaxDistance, IXAudio2Voice::SetVolume,
    # IXAudio2SourceVoice::SetFrequencyRatio
    "fi>i":     "hand_f_i",
    # IDirectSound3DListener::SetPosition / SetVelocity,
    # IDirectSound3DBuffer::SetPosition / SetVelocity / SetConeOrientation
    "fffi>i":   "hand_fff_i",
    # IDirectSound3DListener::SetOrientation -- six floats, of which the
    # fifth and sixth are past XMM3 and travel on the guest's stack
    "ffffffi>i": "hand_ffffff_i",
}


class Refused(Exception):
    pass


# --------------------------------------------------------------------------
# per-surface knobs
# --------------------------------------------------------------------------

SURFACES = {
    "dsound": dict(
        prefix="dsound",
        roster="interfaces_dsound.json",
        # dsound.h for the surface itself; the rest because DirectSound
        # spells almost every parameter with an LP-typedef declared elsewhere
        # (LPDWORD/LPLONG in windef.h, LPCGUID in guiddef.h, LPWAVEFORMATEX in
        # mmreg.h, LPD3DVALUE/LPD3DVECTOR in d3dtypes.h) and an unresolved one
        # would be classified as an unknown BY-VALUE type.
        headers=["dsound.h", "d3dtypes.h", "mmreg.h", "mmsystem.h",
                 "windef.h", "minwindef.h", "winnt.h", "guiddef.h",
                 "basetsd.h"],
        locals_claimed=False,
        # void** / LPVOID* out-parameters that are NOT untyped interface
        # pointers but blocks of mapped audio memory, checked one by one
        # against dsound.h.  Everything else with a bare void** is refused:
        # the default has to be the safe one, because an interface pointer
        # that crosses untyped gets no guest vtable at all.
        void_pp_is_memory=frozenset((
            "IDirectSoundBuffer::Lock",
            "IDirectSoundBuffer8::Lock",
            "IDirectSoundCaptureBuffer::Lock",
            "IDirectSoundCaptureBuffer8::Lock",
        )),
        hand_slots=[
            # DirectSound's three creators end in an aggregation pUnkOuter,
            # which is an IUnknown the CALLER implements.  A guest one would
            # need a reverse proxy; a NULL one -- which is what every caller
            # that is not writing an aggregate passes -- needs nothing.  So
            # these are hand-written rather than refused wholesale: NULL is
            # served, non-NULL is refused by name.  The two CreateSoundBuffer
            # slots share one function because IDirectSound8's vtable restates
            # IDirectSound's signature exactly.
            ("IDirectSound::CreateSoundBuffer",         "hand_create_sound_buffer"),
            ("IDirectSound8::CreateSoundBuffer",        "hand_create_sound_buffer"),
            ("IDirectSoundCapture::CreateCaptureBuffer", "hand_create_capture_buffer"),
        ],
        refusals={},
        flat_refusals={},
    ),
    "xaudio2_9": dict(
        prefix="xaudio2",
        roster="interfaces_xaudio2_9.json",
        headers=["dlls/xaudio2_9/xaudio_classes.h"],
        headers_from_build=True,
        extra_headers=["windef.h", "minwindef.h", "winnt.h", "guiddef.h", "mmreg.h",
                       "basetsd.h"],
        locals_claimed=True,
        void_pp_is_memory=frozenset(),
        hand_slots=[
            # Each of these takes a struct that reaches an interface pointer
            # (XAUDIO2_VOICE_SENDS -> IXAudio2Voice*, XAUDIO2_EFFECT_CHAIN ->
            # IUnknown*) or a guest-implemented callback.  A hand function can
            # do what a static class cannot: serve the NULL case, which is what
            # a game that does not build submix graphs or effect chains
            # actually passes, and refuse the non-NULL one BY NAME.
            ("IXAudio2::CreateSourceVoice",      "hand_create_source_voice"),
            ("IXAudio2::CreateSubmixVoice",      "hand_create_submix_voice"),
            ("IXAudio2::CreateMasteringVoice",   "hand_create_mastering_voice"),
            ("IXAudio2Voice::SetOutputVoices",   "hand_set_output_voices"),
            ("IXAudio2Voice::SetEffectChain",    "hand_set_effect_chain"),
        ],
        # Slots the CLIENT has to recognise by number rather than by name.
        # DestroyVoice is the one: the client keeps a registry of live voice
        # host pointers (see dlls/xaudio2_9/guestcom.c) and this is where an
        # entry leaves it.  Emitted as a #define, and the generator checks the
        # slot number is the same in every interface that inherits the method,
        # because a per-interface answer would need a per-interface test.
        notable_slots=["IXAudio2Voice::DestroyVoice"],
        # RegisterForCallbacks and UnregisterForCallbacks used to be refused
        # here, and the reason was real: they take an IXAudio2EngineCallback
        # the APPLICATION implements and XAudio2 calls it from its own threads.
        # libs/winecom/reverse.c builds the mirror now and both interfaces are
        # on the roster, so their CA_IFACE_IN rows carry the interface type the
        # reverse direction needs and the ordinary marshal path serves them.
        # Unregister works for the same reason Register does, and it works with
        # the SAME POINTER, because reverse proxies are interned by (guest
        # pointer, interface) -- which is what makes an unregister find the
        # registration.
        refusals={},
        flat_refusals={},
    ),
    # Mechanical repeat of the xaudio2_9 entry above (see xaudio2_9.thunks'
    # banner for why 2_8 needs its own roster and not a shared one): same
    # hand slots, same notable slot, same shape.  Reads
    # dlls/xaudio2_8/xaudio_classes.h, which is build output -- widl's
    # translation of xaudio2_7/xaudio_classes.idl (shared via PARENTSRC) at
    # -DXAUDIO2_VER=8 -- so this entry cannot run until that exists.
    "xaudio2_8": dict(
        prefix="xaudio2",
        roster="interfaces_xaudio2_8.json",
        headers=["dlls/xaudio2_8/xaudio_classes.h"],
        headers_from_build=True,
        extra_headers=["windef.h", "minwindef.h", "winnt.h", "guiddef.h", "mmreg.h",
                       "basetsd.h"],
        locals_claimed=True,
        void_pp_is_memory=frozenset(),
        hand_slots=[
            ("IXAudio2::CreateSourceVoice",      "hand_create_source_voice"),
            ("IXAudio2::CreateSubmixVoice",      "hand_create_submix_voice"),
            ("IXAudio2::CreateMasteringVoice",   "hand_create_mastering_voice"),
            ("IXAudio2Voice::SetOutputVoices",   "hand_set_output_voices"),
            ("IXAudio2Voice::SetEffectChain",    "hand_set_effect_chain"),
        ],
        notable_slots=["IXAudio2Voice::DestroyVoice"],
        refusals={},
        flat_refusals={},
    ),
}


# --------------------------------------------------------------------------
# struct bodies -- which ones carry an interface pointer, transitively
# --------------------------------------------------------------------------

STRUCT_RE = re.compile(
    r'typedef\s+(?:struct|union)\s*(?:\w+\s*)?\{(.*?)\}\s*(\w+)\s*;', re.DOTALL)


def scan_structs(texts, iface_names):
    """-> (bearing, why).  `bearing` is the set of struct type names that reach
    an INTERFACE pointer through any member chain.  Neither the fact nor the
    hazard is visible in a signature: XAUDIO2_VOICE_SENDS is a plain data
    struct as far as the parameter list is concerned, and carries the
    XAUDIO2_SEND_DESCRIPTOR array whose pOutputVoice members would arrive at
    FAudio as guest proxy pointers.

    Deliberately transitive and deliberately crude about what a "member type"
    is -- every identifier in the body counts.  Over-approximating costs a
    refusal; under-approximating hands the implementation a guest proxy."""
    members = {}
    for text in texts:
        for body, name in STRUCT_RE.findall(text):
            members.setdefault(name, set(re.findall(r'\b(\w+)\s*\*', body)) |
                               set(re.findall(r'\b(\w+)\s+\w+\s*(?:\[|;|:)', body)))
    hit, why = set(), {}
    for name in members:
        seen, stack = set(), [(name, [])]
        while stack:
            cur, path = stack.pop()
            if cur in seen:
                continue
            seen.add(cur)
            done = False
            for m in members.get(cur, ()):
                if m in iface_names:
                    hit.add(name)
                    why[name] = " -> ".join(path + [cur, m])
                    done = True
                    break
                if m in members:
                    stack.append((m, path + [cur]))
            if done:
                break
    return hit, why


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
        self.inner_const = bool(re.search(r'\*\s*const\s*\*', t))
        t = re.sub(r'\bconst\b', ' ', t)
        toks = t.replace('*', ' * ').split()
        self.stars = toks.count('*')
        toks = [x for x in toks if x != '*']
        self.base = toks[0] if toks else ''
        self.name = toks[-1] if len(toks) > 1 else ''
        if self.array:
            self.stars += 1

    def is_riid(self):
        return re.match(r'^(REFIID|REFGUID|REFCLSID)\b', self.raw) is not None

    def resolve(self, typedefs, ifaces):
        """-> (base, stars) with every pointer typedef followed.

        LPLPDIRECTSOUNDBUFFER is IDirectSoundBuffer** in one token; stopping at
        the first name would classify the out-parameter of CreateSoundBuffer as
        opaque data.  Stops the moment the name IS a rostered interface, so an
        `IDirectSoundNotify8` alias resolves to its interface and no further."""
        base, stars, n = self.base, self.stars, 0
        while base not in ifaces and base in typedefs and n < 16:
            base, extra = typedefs[base]
            stars += extra
            n += 1
        return base, stars


# --------------------------------------------------------------------------
# classification
# --------------------------------------------------------------------------

CA = dict(PASS=0, IFACE_IN=1, RIID=2, PPV_OUT=3, RET_PTR=4, EVENT=5,
          IFACE_ARR_IN=6, IFACE_OUT_STATIC=7, IFACE_ARR_OUT_STATIC=8)
CA_NAME = {v: "WINECOM_CA_" + k for k, v in CA.items()}


def classify(key, slot, ifaces, iface_index, typedefs, byval_ok, bearing,
             why_bearing, void_pp_memory):
    """-> (cls[], xaux[], aux, fp_shape, fpmask, fpwide, xmask) or raise
    Refused(reason).

    fp_shape is None when no parameter travels in a floating-point register;
    otherwise it is the FP_SHAPES key and the caller must route the FORWARD
    call to a hand function, because the generic invoker calls with integer
    registers only.  fpmask/fpwide say the same thing in the form
    libs/winecom/reverse.c reads, because the REVERSE dispatcher marshals its
    own registers and needs no hand function at all.

    xmask says WHICH parameters this generator actually wrote an xaux entry
    for.  Without it a zero it never wrote reads as roster index 0, which is a
    real interface -- and the reverse direction, which reads xaux for IN
    parameters too, would hand a guest method an object of the wrong type."""
    params = [Param(p) for p in slot["params"]]
    n = len(params)
    cls = [CA["PASS"]] * n
    xaux = [0] * n
    aux = 0
    shape = []
    fpmask = fpwide = xmask = 0

    if FLOAT_TOKENS.search(slot["ret"]) and "*" not in slot["ret"]:
        raise Refused(
            "returns a float by value; the dispatcher writes the result into "
            "ctx->Rax and MS-x64 returns a float in XMM0, so this slot needs a "
            "hand-written form that writes the XMM save area itself")

    for i, p in enumerate(params):
        base, stars = p.resolve(typedefs, ifaces)

        if stars == 0:
            if FLOAT_TOKENS.search(p.raw) or base in ("float", "double"):
                shape.append("f")
                if i < 7:
                    # Past position 7 there is no ELFv2 register for it and
                    # the reverse dispatcher refuses the slot anyway; the
                    # FORWARD hand function still reads it off the guest's
                    # stack, so this only narrows what reverse claims.
                    fpmask |= 1 << i
                    if base in ("double", "DOUBLE"):
                        fpwide |= 1 << i
                continue
            if base in BYVAL_OPAQUE:
                raise Refused(BYVAL_OPAQUE[base])
            if base not in byval_ok:
                raise Refused(
                    "by-value parameter `%s` is of a type this generator "
                    "cannot prove is integer-class on both ABIs; refusing "
                    "rather than assuming it is an enum" % p.raw)
            shape.append("i")
            cls[i] = CA["PASS"]
            continue

        shape.append("i")            # every pointer is an integer register

        if base == "void" and stars == 2:
            prev = params[i - 1] if i else None
            if prev is not None and prev.is_riid():
                cls[i] = CA["PPV_OUT"]
                cls[i - 1] = CA["RIID"]
                aux = i - 1
                continue
            if key in void_pp_memory:
                cls[i] = CA["PASS"]
                continue
            raise Refused(
                "has a void** out-parameter (`%s`) with no REFIID beside it to "
                "type the result; an untyped interface pointer cannot be given "
                "a guest vtable" % p.raw)

        if base in bearing:
            raise Refused(
                "takes %s, a struct that reaches an interface pointer through "
                "its own members (%s); the pointers inside it would arrive at "
                "the implementation as guest proxies.  Needs a hand-written "
                "walker, or a hand-written slot that serves the NULL case and "
                "refuses the rest by name" % (base, why_bearing.get(base, base)))

        if base in ifaces:
            if stars == 1:
                # The TYPE is recorded even though the forward direction
                # recognises an IN pointer by identity rather than by type:
                # the reverse direction has to give a NATIVE object arriving
                # at a guest method one of the rostered guest vtables, and
                # identity cannot say which.
                cls[i] = CA["IFACE_IN"]
                xaux[i] = iface_index[base]
                xmask |= 1 << i
                continue
            if stars != 2:
                raise Refused(
                    "takes `%s`: an interface pointer at a level of "
                    "indirection this generator has no class for" % p.raw)
            if p.inner_const:
                raise Refused(
                    "takes the input interface array `%s`; no slot on this "
                    "surface has one, so there is no measured count rule to "
                    "bound it with" % p.raw)
            cls[i] = CA["IFACE_OUT_STATIC"]
            xaux[i] = iface_index[base]
            xmask |= 1 << i
            continue

        if UNROSTERED_IFACE_RE.match(base):
            raise Refused(
                "takes `%s`, an interface pointer of a type this surface does "
                "not roster -- there is no guest stub vtable for it, so it can "
                "be neither wrapped on the way out nor recognised on the way "
                "in.  A NULL-only slot (aggregation's pUnkOuter is the usual "
                "case) belongs in hand_slots, where the NULL case is served "
                "and the rest is refused by name" % p.raw)

        # an ordinary pointer to plain data: crosses as an address.  Both
        # sides are Wine PE code in the same process, built from these very
        # declarations, so there is nothing to repack.
        cls[i] = CA["PASS"]

    fp = "".join(shape) + ">" + ("f" if FLOAT_TOKENS.search(slot["ret"]) else "i")
    return (cls, xaux, aux, (fp if "f" in "".join(shape) else None),
            fpmask, fpwide, xmask)


# --------------------------------------------------------------------------
# emission
# --------------------------------------------------------------------------

def c_guid(u):
    a, b, c, d, e = u.split("-")
    d4 = d + e
    return "{0x%s,0x%s,0x%s,{%s}}" % (
        a, b, c, ",".join("0x" + d4[i:i + 2] for i in range(0, 16, 2)))


def generate(roster, spec, header_texts):
    ifaces = roster["interfaces"]
    aliases = roster.get("iface_ptr_aliases", {})
    prefix = spec["prefix"]
    order = sorted(ifaces)
    iface_index = {n: i for i, n in enumerate(order)}
    byval_ok = set(BYVAL_INTEGER) | set(roster.get("enums", ()))
    typedefs = scan_pointer_typedefs(header_texts)
    typedefs.update(POINTER_TYPEDEFS)   # the ones no header states readably
    # the roster's own alias map is authoritative for interface spellings
    for k, v in aliases.items():
        if k not in typedefs:
            typedefs[k] = (v, 0)
    bearing, why_bearing = scan_structs(header_texts, set(ifaces) | set(aliases))
    is_local = {n: bool(ifaces[n].get("synthetic_iid")) for n in order}

    if any(is_local.values()) and not spec["locals_claimed"]:
        sys.exit("gen_winecom: %s has interface(s) that are not IUnknown-"
                 "derived (%s) but the surface is not marked locals_claimed.  "
                 "libs/winecom serves slots 0..2 of EVERY interface from the "
                 "proxy table, so the client's __wine_com_dispatch must claim "
                 "these before winecom_dispatch sees them -- otherwise slot 0 "
                 "is served as QueryInterface."
                 % (roster["surface"],
                    ", ".join(n for n in order if is_local[n])))

    hand_index, hand_order = {}, []

    def hand_slot(fn):
        if fn not in hand_order:
            hand_order.append(fn)
        return hand_order.index(fn)

    for key, fn in spec["hand_slots"]:
        hand_index[key] = hand_slot(fn)
    # the shape-keyed float hands come after the named ones, in a fixed order,
    # so adding a named hand cannot renumber a shape hand
    shape_index = {shape: hand_slot(fn) for shape, fn in sorted(FP_SHAPES.items())}

    out = []
    w = out.append
    stats = dict(marshalled=0, refused=0, hand=0, identity=0, iunknown=0,
                 fp=0, local=0)
    refusal_log = []

    w("""/* GENERATED by ppc64le/audio/gen_winecom.py -- do not edit.
 *
 * Marshal tables for the %s surface (%d interfaces, %d vtable slots),
 * generated from ppc64le/audio/%s -- the same roster the guest
 * thunk module's .thunks file hands spec2thunk.  Interface order is
 * sorted by name, which is the order spec2thunk COM mode gives the
 * guest stub arrays; libs/winecom cross-checks every IID and slot
 * count at attach, so the two generators cannot silently disagree.
 *
 * Slot/iface types and WINECOM_CA_* classes come from
 * include/wine/winecom.h, which must be included before this file.
 */
""" % (roster["surface"], len(order),
       sum(len(ifaces[n]["slots"]) for n in order), spec["roster"]))

    w("enum %s_iface_index\n{" % prefix)
    for n in order:
        w("    %s_IFACE_%s = %d," % (prefix.upper(), n, iface_index[n]))
    w("    %s_IFACE_COUNT = %d\n};\n" % (prefix.upper(), len(order)))
    w("#define %s_HAND_COUNT %d\n" % (prefix.upper(), len(hand_order)))
    for key in spec.get("notable_slots", ()):
        owner, method = key.split("::")
        seen = set()
        for n in order:
            for s2 in ifaces[n]["slots"]:
                if s2["owner"] == owner and s2["name"] == method:
                    seen.add(s2["slot"])
        if not seen:
            sys.exit("gen_winecom: notable slot %s is in no interface" % key)
        if len(seen) != 1:
            sys.exit("gen_winecom: notable slot %s is at slot numbers %s in "
                     "different interfaces; the client tests ONE number"
                     % (key, sorted(seen)))
        w("#define %s_SLOT_%s_%s %d\n"
          % (prefix.upper(), owner, method, seen.pop()))
    w("/* hand_funcs[] order in the client module:\n%s */\n"
      % "".join(" *   %d %s\n" % (i, f) for i, f in enumerate(hand_order)))

    tables = []
    for n in order:
        slots = ifaces[n]["slots"]
        rows, decls = [], []
        interesting = False
        for s in slots:
            key = "%s::%s" % (s["owner"], s["name"])
            argc = 1 + len(s["params"])
            if s["slot"] < 3 and not is_local[n]:
                rows.append('    { "%s", NULL, NULL, NULL, %d, 0, 0, 0, NULL,'
                            ' 0, 0, 0 },  /* runtime */'
                            % (key, 1 if s["slot"] else 3))
                stats["iunknown"] += 1
                continue
            flags = []
            if s["ret"] == "void":
                flags.append("WINECOM_F_RET_VOID")
            reason = spec["refusals"].get(key)
            hand = hand_index.get(key)
            cls = xaux = None
            aux = fpmask = fpwide = xmask = 0
            if reason is None and hand is None:
                try:
                    (cls, xaux, aux, fp, fpmask, fpwide, xmask) = classify(
                        key, s, ifaces, iface_index, typedefs, byval_ok,
                        bearing, why_bearing, spec["void_pp_is_memory"])
                    if fp is not None:
                        if fp not in shape_index:
                            raise Refused(
                                "passes a float by value in the shape `%s`, "
                                "which no hand-written form implements; the "
                                "generic invoker calls with integer registers "
                                "only, so serving it would put the value in "
                                "the wrong register file.  Add the shape to "
                                "FP_SHAPES in ppc64le/audio/gen_winecom.py and "
                                "the function beside its siblings" % fp)
                        if any(c != CA["PASS"] for c in cls):
                            raise Refused(
                                "passes a float by value AND carries an "
                                "interface pointer (shape `%s`); the shape-"
                                "keyed hand functions marshal arguments only, "
                                "so this one needs a named hand slot of its "
                                "own" % fp)
                        hand = shape_index[fp]
                        stats["fp"] += 1
                except Refused as e:
                    reason = str(e)
            if reason is not None:
                rows.append('    { "%s",\n      "%s: %s",\n'
                            '      NULL, NULL, %d, 0, 0, 0, NULL, 0, 0, 0 },'
                            % (key, key, reason.replace('"', "'"), argc))
                stats["refused"] += 1
                refusal_log.append((n, s["slot"], key, reason))
                interesting = True
                continue
            cname = xname = "NULL"
            if cls is not None and any(c != CA["PASS"] for c in cls):
                cname = "cls_%s_%d" % (n, s["slot"])
                decls.append("static const unsigned char %s[] = { %s };"
                             % (cname, ", ".join(CA_NAME[c] for c in cls)))
                interesting = True
            # Emitted whenever a parameter is interface-typed, NOT when the
            # values happen to be non-zero: roster index 0 is a real interface
            # and `any(xaux)` would drop the row that names it.
            if xmask:
                xname = "xaux_%s_%d" % (n, s["slot"])
                decls.append("static const unsigned char %s[] = { %s };"
                             % (xname, ", ".join(str(x) for x in xaux)))
            if hand is not None:
                # A hand-written FORWARD slot.  When the classifier proved the
                # whole signature -- which is every shape-keyed float hand, and
                # no named one -- the row also carries the plan and
                # WINECOM_F_REV, so libs/winecom/reverse.c serves the REVERSE
                # call from the table with no hand function at all.
                if cls is not None:
                    flags.append("WINECOM_F_REV")
                rows.append('    { "%s", NULL, %s, %s, %d, '
                            'WINECOM_F_HAND%s, %d, 0, NULL, 0x%02x, 0x%02x,'
                            ' 0x%02x },'
                            % (key, cname, xname, argc,
                               "".join("|" + f for f in flags), hand,
                               fpmask, fpwide, xmask))
                stats["hand"] += 1
                interesting = True
                continue
            rows.append('    { "%s", NULL, %s, %s, %d, %s, %d, 0, NULL, '
                        '0x%02x, 0x%02x, 0x%02x },'
                        % (key, cname, xname, argc, "|".join(flags) or "0",
                           aux, fpmask, fpwide, xmask))
            stats["marshalled"] += 1

        if is_local[n]:
            stats["local"] += 1
        if not interesting:
            # An all-CA_PASS table, and it is EMITTED rather than collapsed to
            # a winecom identity row.  The DXVK lane collapses because its
            # roster covers interfaces whose parameters were never examined;
            # here every parameter of every slot has been proved integer-class
            # or plain-data-pointer by the classifier above, so a table of
            # PASS rows is a checked statement and an identity row would
            # refuse IDirectSoundNotify::SetNotificationPositions at runtime
            # for no reason.
            stats["identity"] += 1
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
        # WINECOM_IF_LOCAL is the [local] fact in the form libs/winecom reads.
        # The client's own dispatcher claims these interfaces on the FORWARD
        # path (xaudio2_iface_local[] below); the REVERSE dispatcher has no
        # second dispatcher to claim them in, so it needs the flag on the row.
        w('    { "%s", %s,\n      %d, %s, %s },'
          % (n, c_guid(ifaces[n]["uuid"]), len(ifaces[n]["slots"]),
             "slots_" + n, "WINECOM_IF_LOCAL" if is_local[n] else "0"))
    w("};")

    if any(is_local.values()):
        # Emitted only when there is one, so a surface without any does not
        # carry an unused static and the build stays warning-free.
        w("""
/* Interfaces that are NOT IUnknown-derived: slot 0 is a real method, not
 * QueryInterface.  libs/winecom's dispatcher serves slots 0..2 from the proxy
 * table for every interface it is given, so the client's __wine_com_dispatch
 * MUST test this array and serve these itself before delegating. */""")
        w("static const unsigned char %s_iface_local[%s_IFACE_COUNT] =\n{"
          % (prefix, prefix.upper()))
        for n in order:
            w("    %d,  /* %s */" % (1 if is_local[n] else 0, n))
        w("};")

    w("\n/* %d slot(s) marshalled, %d hand-written (%d of them float-bearing,\n"
      " * routed by argument shape), %d refused with a named reason, %d\n"
      " * IUnknown slot(s) served by the runtime; %d interface(s) carry\n"
      " * only CA_PASS rows, %d are [local] and served by the client. */"
      % (stats["marshalled"], stats["hand"], stats["fp"], stats["refused"],
         stats["iunknown"], stats["identity"], stats["local"]))
    return "\n".join(out) + "\n", stats, refusal_log


def read_headers(spec, build):
    texts = []
    for h in spec["headers"]:
        path = os.path.join(build, h) if spec.get("headers_from_build") \
            else os.path.join(SRCTREE, "include", h)
        if not os.path.exists(path):
            sys.exit("gen_winecom: no header at %s" % path)
        with open(path, errors="replace") as fh:
            texts.append(fh.read())
    for h in spec.get("extra_headers", ()):
        path = os.path.join(SRCTREE, "include", h)
        if not os.path.exists(path):
            sys.exit("gen_winecom: no header at %s" % path)
        with open(path, errors="replace") as fh:
            texts.append(fh.read())
    return texts


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--surface", default="dsound", choices=sorted(SURFACES))
    ap.add_argument("--build", default=os.environ.get("BUILD", SRCTREE))
    ap.add_argument("--out")
    ap.add_argument("--check", metavar="FILE")
    ap.add_argument("--report", action="store_true")
    args = ap.parse_args()

    spec = SURFACES[args.surface]
    with open(os.path.join(HERE, spec["roster"])) as fh:
        roster = json.load(fh)
    text, stats, refusals = generate(roster, spec, read_headers(spec, args.build))

    print("surface %s: %d marshalled, %d hand-written (%d float-shaped), "
          "%d refused, %d IUnknown, %d all-pass, %d [local]"
          % (roster["surface"], stats["marshalled"], stats["hand"], stats["fp"],
             stats["refused"], stats["iunknown"], stats["identity"],
             stats["local"]))

    if args.report:
        print("\nrefused slots, by reason:")
        seen = {}
        for n, slot, key, reason in refusals:
            seen.setdefault(reason.split(';')[0][:76], []).append(key)
        for reason, keys in sorted(seen.items(), key=lambda kv: -len(kv[1])):
            uniq = sorted(set(keys))
            print("  %4d  %s" % (len(keys), reason))
            for k in uniq[:10]:
                print("          %s" % k)
            if len(uniq) > 10:
                print("          ... and %d more distinct method(s)"
                      % (len(uniq) - 10))
        if spec["flat_refusals"]:
            print("\nflat exports refused:")
            for k, v in sorted(spec["flat_refusals"].items()):
                print("  %s\n    %s" % (k, v))

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
