#!/usr/bin/env python3
"""Extract the COM vtable layout of the AUDIO surfaces from Wine's OWN headers,
as the ONE table both halves of each boundary are built from.

  ./gen_interfaces.py --surface dsound   --json interfaces_dsound.json
  ./gen_interfaces.py --surface xaudio2_9 --json interfaces_xaudio2_9.json
  ./gen_interfaces.py --surface dsound   --check interfaces_dsound.json
  ./gen_interfaces.py --surface dsound   --iface IDirectSoundBuffer8

WHY IT IS ONE FILE, same reason as ppc64le/dxvk/gen_interfaces.py: the guest
x86-64 thunk module (spec2thunk COM mode, from dlls/dsound/dsound.thunks) and
the native module's marshal tables (gen_winecom.py -> dlls/dsound/
dsound_marshal.h) are emitted by two different generators.  If they read two
copies of the roster and one drifted, the symptom would be a call dispatched to
the NEIGHBOURING slot with the neighbour's argument types -- silently, at
runtime.  So there is exactly one JSON per surface, both generators read it, and
libs/winecom cross-checks every IID and slot count at attach as the last line of
defence.

WHY WINE'S HEADERS.  Unlike the D3D11 lane, the implementation being reached IS
Wine's own (dlls/dsound, dlls/xaudio2_*), compiled from exactly these
declarations.  There is no second vendor's copy to disagree with.

TWO HEADER DIALECTS, because DirectX predates widl:

  * dsound.h is hand-written in the old DirectX style -- `DECLARE_INTERFACE_(
    Name, Base) { STDMETHOD(M)(THIS_ ...) PURE; }` -- and RE-DECLARES every
    inherited method inline, so the block IS the flattened vtable in order.
    The `/*** X methods ***/` comments name the owning interface, which is what
    the hand-slot/refusal keys in gen_winecom.py are keyed on.
  * xaudio2.h is widl output from include/xaudio2.idl, in the ordinary
    MIDL_INTERFACE C++ dialect, where the vtable is base-first and has to be
    flattened here.  It must be read from the BUILD tree (widl runs at build
    time) and with the right -DXAUDIO2_VER, because the IIDs and two of the
    method lists are version-conditional.

INTERFACES WITH NO IID.  IXAudio2Voice and the three interfaces derived from it
are `[local]` and are NOT IUnknown-derived: they have no QueryInterface, no
AddRef, no Release and no IID at all.  winecom's roster is keyed by IID for the
attach cross-check between the two generators, so those rows carry a
DETERMINISTIC SYNTHETIC IID derived from the interface name (see synth_iid).
It is a private key for this port's own two tables and is never compared
against anything outside them; the roster records `synthetic_iid: true` so a
reader cannot mistake it for a Microsoft IID.  gen_winecom.py refuses to emit a
riid-typed lookup that could reach one.

Copyright 2026 the ppc64le port authors

This library is free software; you can redistribute it and/or modify it under
the terms of the GNU Lesser General Public License as published by the Free
Software Foundation; either version 2.1 of the License, or (at your option) any
later version.
"""

import argparse
import hashlib
import json
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
SRCTREE = os.path.abspath(os.path.join(HERE, "..", ".."))


# --------------------------------------------------------------------------
# surfaces
# --------------------------------------------------------------------------

SURFACES = {
    # name: (dialect, [headers], extra)
    "dsound": dict(
        dialect="declare_interface",
        headers=["dsound.h"],
        from_build=False,
    ),
    "xaudio2_9": dict(
        dialect="midl",
        # NOT include/xaudio2.h.  xaudio2.idl is version-conditional and
        # include/'s copy is generated at ONE version for everybody; each
        # module gets its own widl run through xaudio_classes.idl, and
        # dlls/xaudio2_9/xaudio_classes.h is the header xaudio2_9.dll was
        # actually compiled from -- IIDs, IXAudio2's first three methods and
        # IXAudio2SourceVoice::GetState all differ by version.
        headers=["dlls/xaudio2_9/xaudio_classes.h", "include/xapo.h"],
        from_build=True,
        # What a guest can be handed, AND what a guest can HAND OVER.  The two
        # callback interfaces are the second kind: IXAudio2EngineCallback and
        # IXAudio2VoiceCallback are implemented BY the application and passed
        # INTO XAudio2, which calls them from its own threads -- the voice one
        # from the REALTIME mixer thread, on every buffer boundary.
        #
        # They were deliberately absent while this port had no reverse proxies,
        # so that a guest handing one over was refused by name rather than
        # wrapped into something that would run x86-64 bytes on an audio
        # thread.  libs/winecom/reverse.c builds the mirror now, so they are on
        # the roster and their rows are what it marshals by.  Neither is
        # IUnknown-derived -- slot 0 is a real method and there is no reference
        # count -- so both come out SYNTHETIC-IID and [local], like the voices.
        keep=["IXAudio2", "IXAudio2Extension", "IXAudio2Voice",
              "IXAudio2SourceVoice", "IXAudio2SubmixVoice",
              "IXAudio2MasteringVoice",
              "IXAudio2EngineCallback", "IXAudio2VoiceCallback",
              # AND the two XAPO effect interfaces, which come from a
              # DIFFERENT header: include/xapo.h, widl's translation of
              # include/xapo.idl.  They are not version-conditional the way
              # IXAudio2 is -- xapo.idl has no XAUDIO2_VER in it -- so the
              # same declarations serve 2_8 and 2_9, and unlike the voices
              # they are REAL IUnknown-derived interfaces with real IIDs in
              # the header (a410b984-... and 26d95c66-...), so nothing here
              # is synthetic.  They are rostered because the three XAPO
              # factories vend one: dlls/xaudio2_7/xapo.c hands
              # CreateAudioReverb's and CreateAudioVolumeMeter's caller
              # `&object->IXAPO_iface` cast to IUnknown*, and that object's
              # QueryInterface answers IXAPOParameters from the same
              # allocation.  Without both on the roster the factories had to
              # be EXCLUDEd and a guest asking for a reverb got a sentinel.
              "IXAPO", "IXAPOParameters"],
    ),
    # The mechanical repeat xaudio2_9.thunks describes: SAME roster shape as
    # 2_9 (IXAudio2's IID and the first three methods are version-conditional,
    # which is exactly why this needs its OWN widl run rather than sharing
    # 2_9's table), built from dlls/xaudio2_8/xaudio_classes.h -- the header
    # widl emits when dlls/xaudio2_8 compiles xaudio2_7/xaudio_classes.idl
    # (shared via PARENTSRC) with -DXAUDIO2_VER=8.  Requires a build tree.
    "xaudio2_8": dict(
        dialect="midl",
        headers=["dlls/xaudio2_8/xaudio_classes.h", "include/xapo.h"],
        from_build=True,
        keep=["IXAudio2", "IXAudio2Extension", "IXAudio2Voice",
              "IXAudio2SourceVoice", "IXAudio2SubmixVoice",
              "IXAudio2MasteringVoice",
              "IXAudio2EngineCallback", "IXAudio2VoiceCallback",
              # Same two XAPO interfaces, same header, same reason -- see the
              # 2_9 entry above.
              "IXAPO", "IXAPOParameters"],
    ),
    # The WHOLE system-COM surface -- the roster libs/winecom, combase's
    # syscom.c and every guest COM module are built from.  This is the surface
    # ppc64le/syscom/check-syscom-roster.sh layer 1 regenerates and compares
    # against ppc64le/syscom/interfaces_syscom.json, and it is the reason
    # `headers` may name a dialect and a source per file: this one surface
    # spans BOTH dialects and both trees.  Nine widl-generated headers come out
    # of the BUILD tree (they are .idl in include/), four hand-written DirectX
    # headers come out of the SOURCE tree, and XAudio2's classes come from the
    # 2_9 module's own widl run -- the same file the xaudio2_9 surface above
    # reads, and for the same reason.
    #
    # IUnknown is not read from a header at all.  unknwn.h does declare it, but
    # the roster records it as `(builtin)` with no base, because it is the
    # bottom of every chain and its three slots are the definition rather than
    # a thing to be parsed.  Kept that way here so the two agree.
    "syscom": dict(
        dialect="midl",
        from_build=True,
        headers=[
            ("include/unknwn.h",       "midl",              True),
            ("include/objidl.h",       "midl",              True),
            ("include/oaidl.h",        "midl",              True),
            ("include/ocidl.h",        "midl",              True),
            ("include/strmif.h",       "midl",              True),
            ("include/activation.h",   "midl",              True),
            ("include/inspectable.h",  "midl",              True),
            ("include/mmdeviceapi.h",  "midl",              True),
            ("include/audioclient.h",  "midl",              True),
            # 2_7, not 2_9: the system-COM surface serves the XAudio2 2.7
            # family (the 2010 DirectX redistributable everything in the
            # corpus links against), and IXAudio2's IID and its first three
            # methods are version-conditional -- reading 2_9's header here
            # produces a roster whose IXAudio2 has a different IID and three
            # fewer slots, which is exactly what it did the first time.
            ("dlls/xaudio2_7/xaudio_classes.h", "midl",     True),
            # The system-information family: Cyberpunk 2077's boot walks WMI
            # and the NetworkListManager through CoCreateInstance before it
            # loads d3d12, and calls INetworkListManager::GetNetworks without
            # checking the HRESULT -- an unrostered refusal is a guest NULL
            # deref there, not a graceful degrade.  Same family, both
            # generators, one roster: gen_syscom_audio.py owns the rows and
            # this extractor must regenerate them for the drift gate.
            ("include/wbemcli.h",      "midl",              True),
            ("include/netlistmgr.h",   "midl",              True),
            # The device property store: Cyberpunk 2077's audio boot calls
            # IMMDevice::OpenPropertyStore without checking the HRESULT, so
            # the unrostered refusal left its IPropertyStore* uninitialised
            # and the game called through stack garbage (run 30, 2026-08-19).
            # gen_syscom_audio.py owns the rows; this extractor must
            # regenerate them for the drift gate.
            ("include/propsys.h",      "midl",              True),
            # The session-volume chain: The Witcher 3 asks
            # IAudioClient::GetService for IAudioSessionManager2 (the
            # unknown-IID release in the 2026-08-31 run log) on its way to
            # ISimpleAudioVolume.  gen_syscom_audio.py owns the rows; this
            # extractor must regenerate them for the drift gate.
            ("include/audiopolicy.h",  "midl",              True),
            ("dsound.h",               "declare_interface", False),
            ("dmusicc.h",              "declare_interface", False),
            ("dmusici.h",              "declare_interface", False),
            ("dmplugin.h",             "declare_interface", False),
        ],
        builtin=["IUnknown"],
        keep=[
            "IActivationFactory", "IAgileObject", "IAudioClient",
            "IAudioRenderClient",
            "IAudioSessionControl", "IAudioSessionManager",
            "IAudioSessionManager2", "ISimpleAudioVolume",
            # 2026-09-01, the completeness pass: the session sinks + the
            # enumerator (reverse-licensed in gen_syscom_audio.py) --
            # the drift gate's mirror of the same growth.
            "IAudioSessionEnumerator", "IAudioSessionEvents",
            "IAudioSessionNotification", "IAudioVolumeDuckNotification",
            "IBindCtx", "IClassFactory",
            "IConnectionPoint", "IConnectionPointContainer", "ICreateErrorInfo",
            "IDirectMusic", "IDirectMusicAudioPath", "IDirectMusicBand",
            "IDirectMusicBuffer", "IDirectMusicDownloadedInstrument",
            "IDirectMusicGetLoader", "IDirectMusicGraph",
            "IDirectMusicInstrument", "IDirectMusicLoader",
            "IDirectMusicLoader8", "IDirectMusicObject",
            "IDirectMusicPerformance", "IDirectMusicPerformance8",
            "IDirectMusicPort", "IDirectMusicSegment", "IDirectMusicSegment8",
            "IDirectMusicSegmentState", "IDirectMusicTool", "IDirectMusicTrack",
            "IDirectSound", "IDirectSoundBuffer", "IDispatch",
            "IEnumConnectionPoints", "IEnumConnections", "IEnumMoniker",
            "IEnumSTATSTG", "IEnumString", "IEnumUnknown", "IErrorInfo",
            "IGlobalInterfaceTable", "IInspectable", "ILockBytes", "IMMDevice",
            "IMMDeviceCollection", "IMMDeviceEnumerator", "IMMNotificationClient",
            "IMalloc", "IMarshal", "IMoniker", "IMultiQI", "IPersist",
            "IPersistFile", "IPersistStream", "IPersistStreamInit",
            "IRecordInfo", "IReferenceClock", "IRunningObjectTable",
            "ISequentialStream", "IStorage", "IStream", "ISupportErrorInfo",
            "ITypeComp", "ITypeInfo", "ITypeLib", "IUnknown", "IXAudio2",
            "IXAudio2EngineCallback", "IXAudio2MasteringVoice",
            "IXAudio2SourceVoice", "IXAudio2SubmixVoice", "IXAudio2Voice",
            "IXAudio2VoiceCallback",
            "IPropertyStore",
            "IWbemLocator", "IWbemServices", "IEnumWbemClassObject",
            "IWbemClassObject", "IWbemContext",
            # 2026-09-01: the WMI sync out-types + the async sink.
            "IWbemCallResult", "IWbemQualifierSet", "IWbemObjectSink",
            # oaidl's IEnumVARIANT, for the NLM __NewEnum chain.
            "IEnumVARIANT",
            "INetworkListManager", "IEnumNetworks", "INetwork",
            "IEnumNetworkConnections", "INetworkConnection",
        ],
    ),
}


# --------------------------------------------------------------------------
# the DirectX `DECLARE_INTERFACE_` dialect (dsound.h)
# --------------------------------------------------------------------------

DECL_RE = re.compile(
    r'DECLARE_INTERFACE_?\(\s*(\w+)\s*(?:,\s*(\w+)\s*)?\)\s*\{(.*?)\n\};',
    re.DOTALL)
# STDMETHOD(Name)(THIS_ a, b) PURE;   /   STDMETHOD_(RET,Name)(THIS) PURE;
METHOD_RE = re.compile(
    r'STDMETHOD(_)?\(\s*(?:([^,()]+?)\s*,\s*)?(\w+)\s*\)\s*\((.*?)\)\s*PURE\s*;',
    re.DOTALL)
OWNER_RE = re.compile(r'/\*\*\*\s*(\w+)\s+methods\s*\*\*\*/')
GUID_RE = re.compile(
    r'DEFINE_GUID\(\s*IID_(\w+)\s*,\s*'
    r'0x([0-9a-fA-F]+)\s*,\s*0x([0-9a-fA-F]+)\s*,\s*0x([0-9a-fA-F]+)\s*,\s*'
    r'((?:0x[0-9a-fA-F]+\s*,?\s*){8})\)')
# typedef struct IDirectSound *LPDIRECTSOUND,**LPLPDIRECTSOUND;
# typedef struct IDirectSoundNotify IDirectSoundNotify8,*LPDIRECTSOUNDNOTIFY8;
ALIAS_RE = re.compile(r'typedef\s+struct\s+(\w+)\s+([^;]+);')


def parse_declare_interface(text, path):
    """-> (interfaces, owners_by_iface).  Each DECLARE_INTERFACE_ body IS the
    flattened vtable: the DirectX dialect re-states every inherited method in
    order, which is why there is nothing to flatten here and why the
    `/*** X methods ***/` comments are the only source of the owner."""
    ifaces = {}
    for name, base, body in DECL_RE.findall(text):
        # walk the body once, tracking the most recent owner comment
        owner = base or name
        slots = []
        pos = 0
        for m in METHOD_RE.finditer(body):
            for om in OWNER_RE.finditer(body, pos, m.start()):
                owner = om.group(1)
            pos = m.end()
            underscore, ret, mname, args = m.groups()
            ret = (ret or "HRESULT").strip()
            args = " ".join(args.split())
            if args.startswith("THIS_"):
                args = args[len("THIS_"):]
            elif args.strip() == "THIS":
                args = ""
            params = [p.strip() for p in split_params(args) if p.strip()]
            slots.append(dict(slot=len(slots), owner=owner, name=mname,
                              ret=ret, params=params))
        if not slots:
            continue
        if name in ifaces:
            continue
        ifaces[name] = dict(base=base or "IUnknown",
                            header=os.path.basename(path), slots=slots)
    return ifaces


def split_params(s):
    """Split a parameter list on commas that are not inside brackets."""
    out, depth, cur = [], 0, ""
    for ch in s:
        if ch in "([{":
            depth += 1
        elif ch in ")]}":
            depth -= 1
        if ch == "," and depth == 0:
            out.append(cur)
            cur = ""
        else:
            cur += ch
    out.append(cur)
    return out


def parse_guids(text):
    out = {}
    for name, d1, d2, d3, rest in GUID_RE.findall(text):
        d4 = [int(x, 16) for x in re.findall(r'0x([0-9a-fA-F]+)', rest)]
        out[name] = "%08x-%04x-%04x-%02x%02x-%s" % (
            int(d1, 16), int(d2, 16), int(d3, 16), d4[0], d4[1],
            "".join("%02x" % b for b in d4[2:]))
    return out


def parse_aliases(text, iface_names):
    """Pointer typedefs and second spellings, so the flat-surface audit in
    spec2thunk can see that `LPDIRECTSOUND8 *` carries an interface."""
    out = {}
    for target, names in ALIAS_RE.findall(text):
        if target not in iface_names:
            continue
        for tok in names.split(","):
            tok = tok.strip()
            stars = tok.count("*")
            ident = tok.replace("*", "").strip()
            if not ident or ident == target:
                continue
            # A plain (no-star) typedef is a second NAME for the interface --
            # IDirectSoundNotify8 -- and a starred one is a pointer spelling.
            # Both must be recognised by the audit; neither becomes a roster
            # row of its own, because it has no vtable of its own.
            out[ident] = target
            del stars
    return out


# --------------------------------------------------------------------------
# the widl/MIDL dialect (xaudio2.h out of the build tree)
# --------------------------------------------------------------------------

MIDL_RE = re.compile(
    r'MIDL_INTERFACE\("([0-9a-fA-F-]+)"\)\s*\n\s*(\w+)\s*:\s*public\s+(\w+)\s*\n\s*\{',
    re.MULTILINE)
# widl also emits non-IUnknown `[local]` interfaces WITHOUT MIDL_INTERFACE:
#   interface IXAudio2Voice
#   {
IFACE_PLAIN_RE = re.compile(
    r'^\s*(?:struct|interface)\s+(\w+)(?:\s*:\s*public\s+(\w+))?\s*\n\s*\{',
    re.MULTILINE)
VIRTUAL_RE = re.compile(
    r'virtual\s+(.+?)\s+STDMETHODCALLTYPE\s+(\w+)\s*\((.*?)\)\s*=\s*0\s*;',
    re.DOTALL)


def brace_body(text, open_idx):
    """Text between the brace at open_idx and its match."""
    depth = 0
    for i in range(open_idx, len(text)):
        if text[i] == "{":
            depth += 1
        elif text[i] == "}":
            depth -= 1
            if not depth:
                return text[open_idx + 1:i]
    return ""


def parse_midl(text, path):
    """-> {name: dict(uuid, base, own_slots)}.  Only the interface's OWN
    methods; flattening happens later, because that is where the base chain is
    known."""
    ifaces = {}
    seen_at = {}
    for m in MIDL_RE.finditer(text):
        uuid, name, base = m.groups()
        seen_at[name] = m.end() - 1
        if name in ifaces:
            continue
        ifaces[name] = dict(uuid=uuid.lower(), base=base,
                            header=os.path.basename(path))
    for m in IFACE_PLAIN_RE.finditer(text):
        name, base = m.groups()
        if name in ifaces or not name.startswith("I"):
            continue
        idx = text.index("{", m.start())
        seen_at[name] = idx
        ifaces[name] = dict(uuid=None, base=base or None,
                            header=os.path.basename(path))
    for name, i in ifaces.items():
        body = brace_body(text, seen_at[name])
        own = []
        for ret, mname, args in VIRTUAL_RE.findall(body):
            args = " ".join(args.split())
            # widl writes the IDL's defaultvalue() as a C++ default argument;
            # it is not part of the ABI and must not reach the classifier.
            params = [re.sub(r'\s*=.*$', '', p).strip()
                      for p in split_params(args) if p.strip()]
            own.append(dict(owner=name, name=mname,
                            ret=" ".join(ret.split()), params=params))
        i["own"] = own
    return ifaces


def flatten_midl(ifaces, name, seen=None):
    """Base-interface methods first, in declaration order, then this
    interface's own, recursively.  Getting this wrong compiles fine and
    dispatches to the neighbour, so it is computed rather than written."""
    seen = seen or set()
    if name in seen:
        sys.exit("gen_interfaces: %s inherits from itself" % name)
    i = ifaces[name]
    base = i.get("base")
    if base == "IUnknown":
        out = [dict(owner="IUnknown", name="QueryInterface", ret="HRESULT",
                    params=["REFIID riid", "void **ppvObject"]),
               dict(owner="IUnknown", name="AddRef", ret="ULONG", params=[]),
               dict(owner="IUnknown", name="Release", ret="ULONG", params=[])]
    elif base and base in ifaces:
        out = list(flatten_midl(ifaces, base, seen | {name}))
    elif base:
        sys.exit("gen_interfaces: %s's base %s is outside the parsed header "
                 "set, so its slot numbers are unknown" % (name, base))
    else:
        out = []                       # a [local] non-IUnknown interface
    out = [dict(s) for s in out] + [dict(s) for s in i["own"]]
    for n, s in enumerate(out):
        s["slot"] = n
    return out


# --------------------------------------------------------------------------
# synthetic IIDs for the IID-less [local] interfaces
# --------------------------------------------------------------------------

# A fixed namespace so the value is reproducible from the name alone and
# collides with nothing: the port's own UUID space, version 5 (SHA-1) form.
SYNTH_NS = b"wine-ppc64le/winecom/local-interface/"


def synth_iid(name):
    h = hashlib.sha1(SYNTH_NS + name.encode()).digest()
    b = bytearray(h[:16])
    b[6] = (b[6] & 0x0f) | 0x50        # version 5
    b[8] = (b[8] & 0x3f) | 0x80        # RFC 4122 variant
    return "%s-%s-%s-%s-%s" % (b[0:4].hex(), b[4:6].hex(), b[6:8].hex(),
                               b[8:10].hex(), b[10:16].hex())


# --------------------------------------------------------------------------
# enums, so gen_winecom.py can prove a by-value parameter is integer-class
# --------------------------------------------------------------------------

ENUM_RE = re.compile(r'\benum\s+(\w+)\s*\{')
TYPEDEF_ENUM_RE = re.compile(r'\}\s*(\w+)\s*;')


def scan_enums(texts):
    out = set()
    for text in texts:
        for m in ENUM_RE.finditer(text):
            out.add(m.group(1))
            tail = text[m.end():m.end() + 4000]
            close = tail.find("}")
            if close >= 0:
                t = TYPEDEF_ENUM_RE.match(tail[close:])
                if t:
                    out.add(t.group(1))
    return sorted(out)


# --------------------------------------------------------------------------
# driver
# --------------------------------------------------------------------------

def header_spec(entry, spec):
    """A `headers` entry is either a bare name -- taking the surface's own
    dialect and tree -- or a (name, dialect, from_build) triple.  The triple
    form exists for the one surface that spans both dialects and both trees;
    see the syscom entry above."""
    if isinstance(entry, tuple):
        return entry
    return (entry, spec["dialect"], spec["from_build"])


def read_header(name, from_build, build):
    """Return (text, path), read verbatim.

    NO preprocessor runs here, deliberately.  widl has already resolved the
    version conditionals into the generated header, and the only #if left in
    it is __cplusplus/CINTERFACE -- both branches are present in the text and
    the C++ one, which is the vtable in declaration order, is the one taken
    (first definition wins).  Running cpp instead would need the whole Windows
    header set to be resolvable from this script, which buys nothing and
    breaks the moment an include path moves."""
    root = build if from_build else SRCTREE
    path = os.path.join(root, name if from_build else os.path.join("include", name))
    if not os.path.exists(path):
        sys.exit("gen_interfaces: %s is not at %s%s" % (
            name, path, " -- it is build output, so build first or pass --build"
            if from_build else ""))
    with open(path, errors="replace") as fh:
        return fh.read(), path


# The three slots every IUnknown-derived vtable starts with.  Written out
# rather than parsed: IUnknown is the bottom of every chain, its layout is the
# definition rather than a thing a header gets to vary, and a surface that
# rosters IUnknown itself records it as `(builtin)` for the same reason.
IUNKNOWN_SLOTS = [
    dict(owner="IUnknown", name="QueryInterface", ret="HRESULT",
         params=["REFIID riid", "void **ppvObject"]),
    dict(owner="IUnknown", name="AddRef", ret="ULONG", params=[]),
    dict(owner="IUnknown", name="Release", ret="ULONG", params=[]),
]
IUNKNOWN_IID = "00000000-0000-0000-c000-000000000046"


def build_roster(surface, build):
    spec = SURFACES[surface]

    # ONE PASS OVER EVERY HEADER FIRST, then flatten.  A base interface
    # routinely lives in a different file from the one that derives from it --
    # IStream's base ISequentialStream shares objidl.h, but IPersistStreamInit
    # in ocidl.h derives from IPersistStream in objidl.h -- so flattening
    # cannot start until every header has been read.
    raw = []
    midl_pool, decl_ifaces, aliases, guids = {}, {}, {}, {}
    for entry in spec["headers"]:
        name, dialect, from_build = header_spec(entry, spec)
        text, path = read_header(name, from_build, build)
        raw.append(text)
        if dialect == "declare_interface":
            got = parse_declare_interface(text, path)
            guids.update(parse_guids(text))
            for iname, i in got.items():
                decl_ifaces.setdefault(iname, i)
            aliases.update(parse_aliases(text, set(got)))
        else:
            for iname, i in parse_midl(text, path).items():
                midl_pool.setdefault(iname, i)

    builtin = set(spec.get("builtin") or ())
    keep = spec.get("keep") or sorted(set(decl_ifaces) | set(midl_pool))
    ifaces, synthetic = {}, []
    for name in keep:
        if name in builtin:
            ifaces[name] = dict(uuid=IUNKNOWN_IID, base=None, header="(builtin)",
                                slots=[dict(s) for s in IUNKNOWN_SLOTS])
            for n, s in enumerate(ifaces[name]["slots"]):
                s["slot"] = n
            continue
        if name in decl_ifaces:
            i = decl_ifaces[name]
            if name not in guids:
                # No IID in the header means nothing can ask for it and nothing
                # can vend it; a row with no key is worse than no row, so it is
                # dropped and named here.
                print("  note: %s has no IID_ in %s -- not rostered"
                      % (name, i["header"]), file=sys.stderr)
                continue
            i["uuid"] = guids[name]
            ifaces[name] = i
            continue
        if name not in midl_pool:
            sys.exit("gen_interfaces: %s asked for %s, which is in none of its "
                     "headers" % (surface, name))
        i = midl_pool[name]
        slots = flatten_midl(midl_pool, name)
        uuid = i["uuid"]
        if uuid is None:
            uuid = synth_iid(name)
            synthetic.append(name)
        ifaces[name] = dict(uuid=uuid, base=i.get("base") or "(none)",
                            header=i["header"], slots=slots)
        if uuid == synth_iid(name):
            ifaces[name]["synthetic_iid"] = True

    out = dict(surface="wine-" + surface,
               iface_ptr_aliases=dict(sorted(aliases.items())),
               enums=scan_enums(raw),
               interfaces={k: ifaces[k] for k in sorted(ifaces)})
    if synthetic:
        out["synthetic_iid_interfaces"] = sorted(synthetic)
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--surface", default="dsound", choices=sorted(SURFACES))
    ap.add_argument("--build", default=os.environ.get("BUILD", SRCTREE),
                    help="build tree holding widl output (default: in-tree)")
    ap.add_argument("--json", metavar="FILE", help="write the table")
    ap.add_argument("--check", metavar="FILE",
                    help="regenerate and diff against FILE")
    ap.add_argument("--iface", metavar="NAME", help="print one vtable")
    args = ap.parse_args()

    roster = build_roster(args.surface, args.build)
    ifaces = roster["interfaces"]
    total = sum(len(i["slots"]) for i in ifaces.values())
    print("%s: %d interface(s), %d vtable slot(s), %d pointer alias(es)"
          % (roster["surface"], len(ifaces), total,
             len(roster["iface_ptr_aliases"])))
    if roster.get("synthetic_iid_interfaces"):
        print("  synthetic IIDs (no IID in the header, not IUnknown-derived): %s"
              % ", ".join(roster["synthetic_iid_interfaces"]))

    if args.iface:
        i = ifaces.get(args.iface)
        if not i:
            sys.exit("no such interface: %s" % args.iface)
        print("\n%s : %s  {%s}%s" % (args.iface, i["base"], i["uuid"],
                                     "  SYNTHETIC" if i.get("synthetic_iid")
                                     else ""))
        for s in i["slots"]:
            print("  %3d  %-28s %-8s (%s)"
                  % (s["slot"], "%s::%s" % (s["owner"], s["name"]), s["ret"],
                     ", ".join(s["params"])))

    text = json.dumps(roster, indent=2, sort_keys=False) + "\n"
    if args.check:
        with open(args.check) as fh:
            have = fh.read()
        if have == text:
            print("check passed: %s is byte-identical to a regeneration"
                  % args.check)
            return 0
        sys.exit("gen_interfaces: %s has DRIFTED from the headers.  "
                 "Regenerate it (--json) and re-run every gate." % args.check)
    if args.json:
        with open(args.json, "w") as fh:
            fh.write(text)
        print("wrote %s" % args.json)
    return 0


if __name__ == "__main__":
    sys.exit(main())
