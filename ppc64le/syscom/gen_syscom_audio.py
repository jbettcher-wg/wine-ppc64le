#!/usr/bin/env python3
"""The AUDIO family of the wine-syscom surface: extract it from Wine's own
headers into ppc64le/syscom/interfaces_syscom.json, and re-emit
dlls/combase/syscom_marshal.h carrying it.

  ./gen_syscom_audio.py --roster    --json  ../../ppc64le/syscom/interfaces_syscom.json
  ./gen_syscom_audio.py --roster    --check ../../ppc64le/syscom/interfaces_syscom.json
  ./gen_syscom_audio.py --marshal   --out   ../../dlls/combase/syscom_marshal.h
  ./gen_syscom_audio.py --marshal   --check ../../dlls/combase/syscom_marshal.h
  ./gen_syscom_audio.py --selfcheck ../../dlls/combase/syscom_marshal.h
  ./gen_syscom_audio.py --report

WHY THIS FAMILY IS ON THE SYSTEM-COM SURFACE AT ALL.  XAudio2 2.7 -- which is
what DOOM (2016) and everything else built against the 2010-era DirectX
redistributable uses -- exports NO creator function.  A 2.7 application reaches
the engine only through CoCreateInstance( CLSID_XAudio2 ), which is served by
combase, so the object it vends must be wrapped by COMBASE's winecom instance,
from COMBASE's roster.  dlls/xaudio2_9's instance cannot do it and must not try:

  * its roster is the 2.9 SHAPE.  IXAudio2's first three methods
    (GetDeviceCount / GetDeviceDetails / Initialize) exist only for
    XAUDIO2_VER <= 7, IXAudio2::CreateMasteringVoice takes a device INDEX
    rather than a device id and has no AUDIO_STREAM_CATEGORY, and
    IXAudio2SourceVoice::GetState takes one argument rather than two.  A 2.7
    object called through 2.9 tables dispatches Initialize to CreateSourceVoice;
  * a winecom instance is per-linkee (include/wine/winecom.h).  A proxy's
    vtable is the GUEST module's stub array for that instance, and its trap
    lands in that module's __wine_com_dispatch.  Handing a combase-created
    object to xaudio2_9's instance would need xaudio2_9 to export a wrapper and
    would still leave the object interned in a table combase cannot see.

So the answer to "serve it here or hand it to the audio module" is SERVE IT
HERE, and this file is the price: the [local]-interface arrangement
dlls/xaudio2_9/guestcom.c explains at length is reproduced for the 2.7 shape in
dlls/combase/syscom.c, against tables generated from the SAME kind of roster.
The refusal texts are copied verbatim from ppc64le/audio/gen_winecom.py and
dlls/xaudio2_9/guestcom.c so that the agent building reverse proxies can grep
one string and find both insertion points.

WHAT ELSE IS IN THE FAMILY.  The WASAPI device chain -- IMMDeviceEnumerator,
IMMDeviceCollection, IMMDevice, IAudioClient, IAudioRenderClient -- for the same
reason: CoCreateInstance( CLSID_MMDeviceEnumerator ) is the ONLY way to reach it
and there is no flat creator anywhere.  Only the five interfaces a game actually
walks (enumerate, default endpoint, activate a client, get a render buffer) are
rostered; everything else an IMMDevice or an IAudioClient can vend is refused by
name, because an interface with no guest stub vtable handed to the guest is a
native vtable the guest would call as x86-64.

TWO GENERATORS, ONE ROSTER, as everywhere else in this port: spec2thunk COM
mode builds the guest stub arrays from interfaces_syscom.json and this file
builds the native marshal tables from the same file, both sorted by interface
name, and libs/winecom cross-checks every IID and slot count at attach.

WHAT THIS FILE DOES *NOT* CLAIM.  The other 58 interfaces of the wine-syscom
roster were extracted and classified by a generator that is not in this tree
(the header says dxvk-ppc64le/thunk/gen_winecom.py).  This file does not
re-derive them: it REUSES their emitted blocks verbatim and only renumbers the
roster indices inside their xaux[] arrays, which is forced because interface
order is sorted by name and the new rows land in the middle of it.  --selfcheck
proves the parse/re-emit round trip is byte-exact, so "verbatim" is a checked
statement rather than an intention.

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
ROSTER = os.path.join(HERE, "interfaces_syscom.json")
MARSHAL = os.path.join(SRCTREE, "dlls", "combase", "syscom_marshal.h")

PREFIX = "SYSCOM"

BANNER = """/* GENERATED -- do not edit.
 *
 * Marshal tables for the wine-syscom surface (%d interfaces, %d vtable
 * slots).  Interface order is sorted by name -- the same order spec2thunk
 * COM mode gives the guest module's stub arrays, and the runtime
 * cross-checks the IIDs at attach so the two cannot silently disagree.
 * Slot/iface types and WINECOM_CA_* classes come from
 * include/wine/winecom.h, which must be included before this file.
 *
 * The %d audio interfaces -- the XAudio2 2.7 family and the WASAPI device
 * chain -- are generated from interfaces_syscom.json by
 * ppc64le/syscom/gen_syscom_audio.py, which also owns the enum, the
 * interface array and the roster indices in every xaux[] here.  The other
 * %d rows are the emitted output of the earlier generator named in the
 * git history, reused verbatim EXCEPT where the reclassification pass
 * lifted a refusal (rows marked "upgraded from a legacy refusal": the
 * offline generator's type prover could not resolve pointer/integer
 * typedefs such as LPOLESTR or LCID; gen_syscom_audio.py's can, with
 * per-typedef provenance, and it reclassifies ONLY the reason families
 * its UPGRADE_LICENSED_RE licenses -- semantic refusals are never
 * touched); ppc64le/syscom/check-syscom-audio.sh
 * proves the reuse-plus-pass is deterministic.
 *
 * WHICH IS WHY THE ROWS ARE NOT ALL THE SAME LENGTH, and it is a
 * statement rather than an oversight: this generator's rows carry the
 * fpmask/fpwide/xmask the REVERSE direction reads, and the reused rows
 * stop at aux2 because the generator that wrote them predates those
 * members.  C zero-fills the rest, which is the right value for a row
 * whose float parameters were never classified and whose interface
 * IN-parameters this port declines to reverse-proxy -- but a reader must
 * not take a 0 on a reused row as a measurement.
 *
 * The reused rows' missing xmask is a POLICY as well as an accident.
 * gen_syscom_audio.py re-derives every one of them from the roster's own
 * parameter text and cross-checks it against the block as emitted, so the
 * information is present and checked; the bit is then withheld, because
 * every reused interface is IUnknown-derived and a reverse proxy of one
 * can be QueryInterface'd into any other -- including the DirectMusic and
 * moniker tables that refuse in both directions.  --report names every
 * withheld parameter.
 */"""

# --------------------------------------------------------------------------
# THE FAMILY.  Which interfaces this generator owns, where their declarations
# come from, and -- for the XAudio2 half -- WHICH widl run.
#
# dlls/xaudio2_7/xaudio_classes.h rather than include/xaudio2.h: include/'s
# copy is generated once at XAUDIO2_VER=9 for everybody, and every version-
# conditional in xaudio2.idl resolves the wrong way in it.  The 2.7 module gets
# its own widl run through its own xaudio_classes.idl with -DXAUDIO2_VER=7, and
# that IS the header the xaudio2_7.dll a guest CoCreateInstance reaches was
# compiled from.
# --------------------------------------------------------------------------

XAUDIO2_HEADER = os.path.join("dlls", "xaudio2_7", "xaudio_classes.h")
MMDEV_HEADERS = [os.path.join("include", "mmdeviceapi.h"),
                 os.path.join("include", "audioclient.h")]

# Read for their enums, typedefs and struct bodies only -- no interface is
# taken from them, but AUDCLNT_SHAREMODE is declared here and not in the widl
# output, and a by-value parameter whose type this generator cannot see is a
# refusal rather than a guess.  Source headers, not build output.
CONTEXT_HEADERS = [os.path.join("include", "audiosessiontypes.h"),
                   os.path.join("include", "mmreg.h")]

XAUDIO2_IFACES = ["IXAudio2", "IXAudio2Voice", "IXAudio2SourceVoice",
                  "IXAudio2SubmixVoice", "IXAudio2MasteringVoice",
                  "IXAudio2EngineCallback", "IXAudio2VoiceCallback"]
# IXAudio2EngineCallback is here BECAUSE the reverse direction now exists.  It
# is implemented BY the application and passed INTO XAudio2, which calls it from
# the mixer thread; libs/winecom/reverse.c builds the native vtable that enters
# guest code, and it needs this interface's slot table to marshal by.  Same
# addition, same reason, as ppc64le/audio/interfaces_xaudio2_9.json's.
#
# IXAudio2VoiceCallback is here for the same reason, and its arrival is what
# lets dlls/combase/syscom.c's hand_create_source_voice stop refusing.  It is
# the OTHER application-implemented interface: XAudio2 calls it per BUFFER
# rather than per engine pass, which is how a streaming loader learns that the
# chunk it queued has been consumed.  A 2.7 application that never gets
# OnBufferEnd waits forever for a buffer it already owns.
#
# The comment that stood here said this interface had to stay off the roster
# until the hand slot was rewritten, because CreateSourceVoice is hand-written
# for the XAUDIO2_VOICE_SENDS and XAUDIO2_EFFECT_CHAIN beside pCallback and a
# hand slot has no table row for a generator to write a callback type into.
# That was true of the ORDER of the work, not of the roster: the hand slot has
# now been rewritten to call winecom_to_native() with this interface's index
# explicitly, exactly as dlls/xaudio2_9/guestcom.c has done since the reverse
# direction landed, so what it needs from here is the SLOT TABLE and not a
# generated argument class.  The sends/chain refusals are untouched and stay
# refused in both directions -- they are a signature fact, not a direction one.
#
# NOT here, and for a reason rather than an omission:
#   IXAudio2Extension is 2.8+ only and absent from the 2.7 header.
MMDEV_IFACES = ["IMMDeviceEnumerator", "IMMDeviceCollection", "IMMDevice",
                "IMMEndpoint",
                "IAudioClient", "IAudioRenderClient", "IMMNotificationClient"]
# IMMEndpoint: RimWorld (Unity 2022.3, FMOD) took it on, 2026-09-04.  Its
# audio boot QIs every IMMDevice for IMMEndpoint and calls GetDataFlow
# through the result WITHOUT checking the HRESULT -- the honest refusal
# left NULL in rcx and the game called slot 3 of address zero
# (UnityPlayer.dll+176adfa, rax still E_NOINTERFACE).  One slot,
# GetDataFlow(EDataFlow*), an out-DWORD: the same lesson as IPropertyStore
# below.
# IMMNotificationClient is here for the same reason IXAudio2EngineCallback is:
# it is implemented BY the application, mmdevapi keeps the pointer and calls it
# from its own notification thread, and the reverse direction needs its slot
# table to marshal by.
#
# NOT here: IAudioClient2/3 / IAudioClock / the volume interfaces (nothing
# on the measured path asks for them, and an unrostered IID is refused
# loudly by winecom rather than served wrongly).
#
# IPropertyStore WAS on that list, and Cyberpunk 2077 took it off.  Its audio
# boot calls IMMDevice::OpenPropertyStore and does not check the HRESULT --
# the same manner it treats every COM call -- so the honest refusal left the
# game's IPropertyStore* stack variable UNINITIALISED, and thread 01b0 called
# through it: guest fault at rip 0x710c232, which is the top half of a stale
# stack quadword read as a vtable slot (run 30, 2026-08-19).  Same lesson as
# the NetworkListManager below: a refusal is only survivable when the caller
# looks at it.  The store's read path is served (GetCount / GetAt / GetValue
# -- see the VARIANT paragraph below for why a PROPVARIANT out is data on
# this path); SetValue stays refused by name (REFUSALS).
PROPSTORE_HEADERS = [os.path.join("include", "propsys.h")]
PROPSTORE_IFACES = ["IPropertyStore"]

# The session-volume chain, and the measurement that put it here: The Witcher
# 3's audio boot calls IAudioClient::GetService( IID_IAudioSessionManager2 )
# and the unknown-IID choke point released the answer --
# `winecom_wrap_out_iface: an interface for unknown IID
# {77aa99a0-1bd6-484f-8bc7-2c654c9a9b6f}` in the 2026-08-31 run log, which
# include/audiopolicy.idl names IAudioSessionManager2.  The game wants it for
# ISimpleAudioVolume (in-game volume sliders).  Same lesson as IPropertyStore
# and the NetworkListManager above: the path a shipped game actually walks
# must be SERVED, and partial slot coverage is the design working --
# GetSessionEnumerator, the notification registrations and every other slot
# naming an unrostered interface refuses BY SLOT.
# ISimpleAudioVolume::SetMasterVolume is float-bearing in exactly the
# existing "fi>i" hand shape (IXAudio2Voice::SetVolume's), so it is served by
# the same hand_f_i with no new code.
SESSION_HEADERS = [os.path.join("include", "audiopolicy.h")]
SESSION_IFACES = ["IAudioSessionManager", "IAudioSessionManager2",
                  "IAudioSessionControl", "ISimpleAudioVolume",
                  # The 2026-09-01 completeness pass: "nothing on the
                  # measured path needs them" is not a refusal reason on
                  # this port any more (the user's rule, after the GetShader
                  # class), so the session family is now WHOLE.  The three
                  # sink interfaces are application-implemented and carry
                  # the reverse-proxy licence below -- each is either a leaf
                  # or takes only NATIVE-minted interface arguments, which
                  # the relaxed licence check proves per slot.
                  "IAudioSessionEnumerator", "IAudioSessionEvents",
                  "IAudioSessionNotification",
                  "IAudioVolumeDuckNotification"]
# IAudioSessionManager is here because Manager2 derives from it and its two
# slots (GetAudioSessionControl / GetSimpleAudioVolume) are the ones the
# volume path actually calls.  NOT here: IAudioSessionControl2 (derives from
# Control; its three extra slots are session-identity strings a QI can grow
# later without unbalancing the roster).

AUDIO_IFACES = XAUDIO2_IFACES + MMDEV_IFACES + PROPSTORE_IFACES + SESSION_IFACES

# --------------------------------------------------------------------------
# THE SYSTEM-INFORMATION FAMILY: WMI and the NetworkListManager.
#
# Cyberpunk 2077's boot asks combase for both before it ever loads d3d12:
# CoCreateInstance( WbemLocator ) for its hardware survey and
# CoCreateInstance( NetworkListManager ) for connectivity -- and it calls
# INetworkListManager::GetNetworks on the result WITHOUT checking the HRESULT,
# because on Windows that creation cannot fail.  With neither family on the
# roster, winecom's fail-closed choke point answered E_NOINTERFACE and NULLed
# the out-pointer, and the game dereferenced it (measured: c0000005 at
# Cyberpunk2077.exe+7e5fd4, `mov rax,[rcx]; call [rax+0x38]` with rcx = 0 --
# slot 7 of an IDispatch-based vtable, which is GetNetworks).  So the honest
# refusal is not survivable here; the family must be SERVED.
#
# Slot coverage is deliberately partial, and that is the design working:
# every method whose parameter types this generator cannot license refuses BY
# SLOT (IWbemServices' IWbemObjectSink asynchrony, IWbemCallResult
# semisynchrony), while the walked path -- ConnectServer, ExecQuery,
# IEnumWbemClassObject::Next, IWbemClassObject::Get, GetNetworks,
# IEnumNetworks::Next, INetwork's property reads -- is plain pointers, BSTRs
# and enums in shared memory.  VARIANT out-parameters pass as plain pointers:
# a VARIANT holding a BSTR or an integer is data in the one address space both
# sides share.  A VARIANT holding VT_UNKNOWN would carry a native vtable; WMI
# property reads on the game's path (strings and integers about the machine)
# do not produce one.
#
# oaidl.h joins the PARSE set only for base-chain flattening: the NLM
# interfaces derive from IDispatch, whose seven slots must be numbered from
# its own declaration rather than assumed.  IDispatch itself stays a legacy
# roster row.
# --------------------------------------------------------------------------

SYSINFO_HEADERS = [os.path.join("include", "wbemcli.h"),
                   os.path.join("include", "netlistmgr.h")]
# oaidl.h for IDispatch's slot numbering; wtypes.h for the POINTER TYPEDEFS
# the classifier licenses BSTR by -- `const BSTR` is a by-value parameter of
# unprovable class until `typedef OLECHAR *BSTR` is in the scanned texts.
SYSINFO_BASE_HEADERS = [os.path.join("include", "oaidl.h"),
                        os.path.join("include", "wtypes.h")]

WBEM_IFACES = ["IWbemLocator", "IWbemServices", "IEnumWbemClassObject",
               "IWbemClassObject", "IWbemContext",
               # 2026-09-01: the WMI completeness pass.  IWbemCallResult and
               # IWbemQualifierSet were the unrostered types behind eleven
               # refused rows on the SYNCHRONOUS surface; IWbemObjectSink is
               # the application-implemented sink behind the thirteen ASYNC
               # rows, licensed as a reverse sink below (its interface
               # arguments all flow NATIVE->guest, the safe direction).
               "IWbemCallResult", "IWbemQualifierSet", "IWbemObjectSink"]
# IWbemContext is rostered for TYPE visibility: ConnectServer and ExecQuery
# take an IWbemContext* in-parameter (games pass NULL), and a slot whose
# parameter names an unrostered interface refuses.  Its own methods can all
# refuse; nothing on the measured path calls them.
NLM_IFACES = ["INetworkListManager", "IEnumNetworks", "INetwork",
              "IEnumNetworkConnections", "INetworkConnection",
              # 2026-09-01: get__NewEnum on both NLM enumerators returns an
              # IEnumVARIANT, whose Next fills VARIANTs that on THIS surface
              # hold VT_UNKNOWN/VT_DISPATCH INetwork objects -- so the
              # interface is rostered (oaidl.h joins the extraction set for
              # it) and its Next is a HAND slot that post-wraps the
              # interface-bearing elements.  See hand_enum_next_variant.
              "IEnumVARIANT"]

SYSINFO_IFACES = WBEM_IFACES + NLM_IFACES

# Everything THIS generator owns and derives; the legacy blocks are the rest.
OWNED_IFACES = AUDIO_IFACES + SYSINFO_IFACES

# Enums these headers declare that the classifier must be able to prove are
# integer-class by value.  Collected from the headers themselves (scan_enums),
# merged into the roster's own list.

# --------------------------------------------------------------------------
# classification knobs -- the same shape as ppc64le/audio/gen_winecom.py's,
# because the boundary is the same one: both sides are ordinary Wine PE code in
# one process, built from these very declarations, so a pointer crosses as an
# address and a WCHAR is two bytes on both sides.
# --------------------------------------------------------------------------

BYVAL_INTEGER = frozenset("""
    UINT INT LONG ULONG DWORD WORD BYTE BOOL WINBOOL UINT8 UINT16 UINT32
    UINT64 INT8 INT16 INT32 INT64 SIZE_T SSIZE_T ULONG64 LONG64 LONGLONG
    ULONGLONG HRESULT REFERENCE_TIME
    unsigned int short char long
    HWND HANDLE HMODULE HMONITOR HINSTANCE
""".split())

POINTER_TYPEDEFS = {
    "LPSTR": ("char", 1), "LPCSTR": ("char", 1),
    "LPWSTR": ("WCHAR", 1), "LPCWSTR": ("WCHAR", 1),
    "LPOLESTR": ("WCHAR", 1), "LPCOLESTR": ("WCHAR", 1),
    "REFIID": ("GUID", 1), "REFGUID": ("GUID", 1), "REFCLSID": ("GUID", 1),
    "LPGUID": ("GUID", 1), "LPCGUID": ("GUID", 1),
    "LPVOID": ("void", 1), "LPCVOID": ("void", 1),
    "LPUNKNOWN": ("IUnknown", 1), "PUNKNOWN": ("IUnknown", 1),
    # The LEGACY families' spellings, added for the upgrade pass (see
    # upgrade_legacy below).  Every entry is an ABI fact read out of the named
    # header, not an assumption -- these are the typedefs the legacy blocks'
    # refusal texts name as "not provably integer-class", and each one IS a
    # pointer, provable at the cited line:
    "LPDWORD": ("DWORD", 1),          # include/windef.h: typedef DWORD *LPDWORD
    "LPLONG": ("LONG", 1),            # include/windef.h: typedef LONG *LPLONG
    "LPBYTE": ("BYTE", 1),            # include/windef.h: typedef BYTE *LPBYTE
    "PVOID": ("void", 1),             # include/winnt.h: typedef void *PVOID
    "LPWAVEFORMATEX": ("WAVEFORMATEX", 1),    # include/mmreg.h
    "LPCWAVEFORMATEX": ("WAVEFORMATEX", 1),   # include/mmreg.h (const)
    "LPCDSBUFFERDESC": ("DSBUFFERDESC", 1),   # include/dsound.h (const)
    "LPREFERENCE_TIME": ("REFERENCE_TIME", 1),  # include/dmusicc.h
    # include/dsound.h: typedef IDirectSoundBuffer *LPDIRECTSOUNDBUFFER,
    # *LPLPDIRECTSOUNDBUFFER -- two stars from the interface, so it resolves
    # to IDirectSoundBuffer** and classifies as CA_IFACE_OUT_STATIC (the
    # rostered type), not merely a plain pointer.
    "LPLPDIRECTSOUNDBUFFER": ("IDirectSoundBuffer", 2),
    # Three more the upgrade pass's own STAYS log diagnosed (2026-09-01):
    "LPOVERLAPPED": ("OVERLAPPED", 1),   # include/minwinbase.h
    # include/wtypes.h (widl): typedef LPOLESTR *SNB; -- the wire_marshal
    # comment in the widl output defeats the typedef scanner's regex
    "SNB": ("WCHAR", 2),
}

# By-value INTEGER typedefs the same way: each resolves to a BYVAL_INTEGER
# member at the cited line.  classify() consults this after BYVAL_INTEGER
# itself, so the licence is one hop, spelled here, never guessed.
BYVAL_INTEGER_TYPEDEFS = {
    "LCID": "include/winnt.h: typedef DWORD LCID",
    "DISPID": "include/oaidl.h (widl): typedef LONG DISPID",
    "CIMTYPE": "include/wbemcli.h (widl): typedef long CIMTYPE",
    "MUSIC_TIME": "include/dmusici.h: typedef long MUSIC_TIME",
    "MEMBERID": "include/oaidl.h (widl): typedef DISPID MEMBERID (-> LONG)",
    # The DirectShow reference-clock handles (2026-09-01, the completeness
    # pass).  These LOOK like objects and are the reason AdviseTime and
    # AdvisePeriodic were refused -- but the header says what they are:
    # a DWORD_PTR is integer-class on both ABIs, and on THIS surface the
    # native side is Wine's own quartz in the same process, so the guest's
    # event HANDLE names exactly the object quartz expects (there is no
    # foreign handle namespace here -- contrast dlls/d3d11's DXVK rows).
    "HEVENT": "include/axcore.idl: typedef DWORD_PTR HEVENT",
    "HSEMAPHORE": "include/axcore.idl: typedef DWORD_PTR HSEMAPHORE",
}

# --------------------------------------------------------------------------
# void** OUT-parameters that are PLAIN MEMORY, not interfaces (2026-09-01,
# the completeness pass).  classify() refuses a void** with no REFIID beside
# it because an UNTYPED INTERFACE cannot be given a guest vtable -- but that
# reasoning only bites when the pointee IS an interface.  Each entry below
# licenses one slot's void** as raw data, with the header/doc fact that
# proves it; the key is "Iface::Method", the value maps the parameter index
# (after `this`, 0-based) to the proof.  A licence here turns the class into
# CA_PASS -- the address crosses, both sides read the same memory.
# --------------------------------------------------------------------------
RAW_VOID_OUT = {
    "IDirectSoundBuffer::Lock": {
        2: "include/dsound.h: ppvAudioPtr1 receives a pointer INTO the "
           "sound buffer's own memory -- bytes, not an object",
        4: "ppvAudioPtr2 likewise (the wrap half of the circular buffer)",
    },
    "IDirectMusicTrack::InitPlay": {
        2: "include/dmusici.h: ppStateData receives the track's own opaque "
           "per-play state, defined and consumed only by the same track "
           "object -- it round-trips through Play/EndPlay untouched",
    },
    "IRecordInfo::RecordCreateCopy": {
        1: "include/oaidl.h: ppvNew receives a freshly allocated RECORD "
           "(field data described by this IRecordInfo), not an object",
    },
    "IRecordInfo::GetFieldNoCopy": {
        3: "include/oaidl.h: ppvDataCArray receives a pointer into the "
           "record's own storage (the no-copy in the name) -- bytes the "
           "caller reads through GetFieldNoCopy's contract, not an object",
    },
}

# `Iface **` OUT-parameters the offline extractor refused as ARRAY-CAPABLE
# because a count-looking argument sits in the same signature.  Each entry
# licenses one slot's Iface** as the SINGULAAR out-cell it really is, with
# the header fact; classify() then serves it as an ordinary
# CA_IFACE_OUT_STATIC.  (Spelled per slot, never as a heuristic -- the
# heuristic is what refused these.  classify() cannot re-detect array-ness,
# so the upgrade pass admits this family ONLY for keys listed here.)
SINGULAR_IFACE_OUT = {
    # CreateStandardAudioPath(DWORD dwType, DWORD dwPChannelCount,
    #                         BOOL fActivate, IDirectMusicAudioPath **ppNewPath)
    "IDirectMusicPerformance8::CreateStandardAudioPath":
        "include/dmusici.h: dwPChannelCount is the CHANNEL count of the new "
        "path, not an element count; ppNewPath receives exactly one object",
}

# void** OUT-parameters whose typing REFIID exists but is NOT the adjacent
# parameter.  classify() pairs a void** with the parameter directly before
# it; these slots put a filename or index between the two.  The value is
# (ppv_index, riid_index), both 0-based after `this`, checked against the
# signature at classification time.
PPV_RIID_AT = {
    # LoadObjectFromFile(REFGUID rguidClassID, REFIID iidInterfaceID,
    #                    WCHAR *pwzFilePath, void **ppObject)
    "IDirectMusicLoader8::LoadObjectFromFile": (3, 1),
}

# Interface ARRAYS spelled `Iface **` that classify() would read as a single
# OUT.  The classifier cannot tell an array from an out-cell -- the
# signature text is identical -- so array-ness is licensed per slot, with the
# count parameter's index (0-based after `this`, count BY VALUE there).
# Today's one consumer is the reverse direction: IWbemObjectSink::Indicate is
# the WMI async sink's delivery slot, its array arrives NATIVE->guest, and
# libs/winecom/reverse.c's CA_IFACE_ARR_IN arm forward-mints each element.
IFACE_ARR_IN_AT = {
    # Indicate(long lObjectCount, IWbemClassObject **apObjArray)
    "IWbemObjectSink::Indicate": {1: 0},
}

FLOAT_TOKENS = re.compile(r'\b(FLOAT|float|double|DOUBLE)\b')
UNROSTERED_IFACE_RE = re.compile(r'^I[A-Z]\w*$')

# Float shapes, keyed by the argument classes AFTER `this` ('i' = integer
# register, including every pointer; 'f' = by-value float) plus '>' and the
# return class.  Same rule and same reason as ppc64le/audio/gen_winecom.py: the
# generic invoker calls with integer registers only, so a float-bearing slot
# must be routed to a hand-written prototype that reads the trap CONTEXT's XMM
# save area.  A shape with no hand form is a NAMED refusal, never a silent
# wrong-register call.
FP_SHAPES = {
    # IXAudio2Voice::SetVolume, IXAudio2SourceVoice::SetFrequencyRatio
    "fi>i": "hand_f_i",
    # IAudioSessionEvents::OnSimpleVolumeChanged (float, BOOL, LPCGUID) --
    # a REVERSE sink slot; the forward hand exists so the row carries a
    # complete plan (cls + fpmask + F_REV), which is what the reverse
    # direction marshals the delivery by.
    "fii>i": "hand_f_i_i",
}

# Slots served by a hand-written function in dlls/combase/syscom.c.  Each takes
# something a static argument class cannot express -- a struct that reaches an
# interface pointer, or a callback the APPLICATION implements -- and each is
# hand-written rather than refused outright so that the NULL case, which is what
# a game that just plays sound actually passes, is fully served and only the
# rest is refused BY NAME.  The order here is hand_funcs[] order.
HAND_SLOTS = [
    ("IXAudio2::CreateSourceVoice",     "hand_create_source_voice"),
    ("IXAudio2::CreateSubmixVoice",     "hand_create_submix_voice"),
    ("IXAudio2::CreateMasteringVoice",  "hand_create_mastering_voice"),
    ("IXAudio2Voice::SetOutputVoices",  "hand_set_output_voices"),
    ("IXAudio2Voice::SetEffectChain",   "hand_set_effect_chain"),
    ("IMMDevice::Activate",             "hand_mmdevice_activate"),
    # The two endpoint-notification slots.  Their IMMNotificationClient IS
    # reverse-proxied -- that is the whole point of rostering it -- but the
    # translation cannot be done blind, because mmdevapi never dereferences the
    # pointer it is given: it stores it on register and COMPARES it on
    # unregister, so `unregister something you never registered` is a
    # well-defined no-op everywhere except here, where translating the pointer
    # would enter its guest AddRef.  These two keep a registry of what this
    # surface really registered and refuse to touch anything else.
    ("IMMDeviceEnumerator::RegisterEndpointNotificationCallback",
                                        "hand_mmdev_register_notify"),
    ("IMMDeviceEnumerator::UnregisterEndpointNotificationCallback",
                                        "hand_mmdev_unregister_notify"),
    # ---- the 2026-09-01 completeness pass, owned families ----------------
    # The two NLM 16-byte GUID-by-value slots: MS-x64 passes any aggregate
    # that is not 1/2/4/8 bytes by hidden pointer, ELFv2 passes 16 bytes in
    # two GPRs -- the walker dereferences the guest's hidden pointer and
    # calls a real by-value prototype (the mf lane's worked shape).
    ("INetworkListManager::GetNetwork",  "hand_nlm_get_network"),
    ("INetworkListManager::GetNetworkConnection",
                                        "hand_nlm_get_network_connection"),
    # PROPERTYKEY (GUID + DWORD, 20 bytes -> also hidden-pointer on MS-x64)
    # by value, forward direction.
    ("IMMNotificationClient::OnPropertyValueChanged",
                                        "hand_propkey_byval"),
    # The device property store's WRITE path: the guest authors the
    # PROPVARIANT, so the walker translates the tagged union (VT_UNKNOWN
    # unwraps through winecom_to_native) instead of refusing the slot.
    ("IPropertyStore::SetValue",        "hand_propstore_setvalue"),
    # IEnumVARIANT::Next fills VARIANTs that hold INetwork objects on this
    # surface: post-wrap the interface-bearing elements, pass the rest.
    ("IEnumVARIANT::Next",              "hand_enum_next_variant"),
]

# LEGACY rows served by hand functions: the reused blocks' refusals whose
# reasons are SEMANTIC (interface-bearing structs, tagged unions, enum-Next
# arrays) and whose service is a walker in dlls/combase/syscom.c rather than
# a class the table could carry.  The rewrite pass swaps the refusal row for
# a WINECOM_F_HAND row with the function's index; everything about the
# signature lives in the C function, exactly as HAND_SLOTS above.  Keys must
# be refused rows in the reused corpus -- rewriting a SERVED row from here is
# fatal, because it would hide a disagreement about what the row is.
LEGACY_HAND = [
    ("IEnumUnknown::Next",              "hand_enum_next_unknown"),
    ("IEnumMoniker::Next",              "hand_enum_next_moniker"),
    ("IEnumConnectionPoints::Next",     "hand_enum_next_cp"),
    ("IEnumConnections::Next",          "hand_enum_next_connectdata"),
    ("IMultiQI::QueryMultipleInterfaces", "hand_multi_qi"),
    ("IDispatch::Invoke",               "hand_dispatch_invoke"),
    ("ITypeComp::Bind",                 "hand_typecomp_bind"),
    # DirectMusic: the PMSG family (interface members at fixed offsets in a
    # struct whose ownership TRANSFERS on send/free), the OBJECTDESC family
    # (an optional IStream member behind a validity flag), and the
    # tag-dispatched GetParam/SetParam payloads (known tags served, unknown
    # tags refused AT RUNTIME naming the GUID).
    ("IDirectMusicGraph::StampPMsg",    "hand_dmus_stamp_pmsg"),
    ("IDirectMusicPerformance::SendPMsg", "hand_dmus_send_pmsg"),
    ("IDirectMusicPerformance::AllocPMsg", "hand_dmus_alloc_pmsg"),
    ("IDirectMusicPerformance::FreePMsg", "hand_dmus_free_pmsg"),
    ("IDirectMusicPerformance8::SendPMsg", "hand_dmus_send_pmsg"),
    ("IDirectMusicPerformance8::AllocPMsg", "hand_dmus_alloc_pmsg"),
    ("IDirectMusicPerformance8::FreePMsg", "hand_dmus_free_pmsg"),
    ("IDirectMusicPerformance8::ClonePMsg", "hand_dmus_clone_pmsg"),
    ("IDirectMusicTool::ProcessPMsg",   "hand_dmus_process_pmsg"),
    ("IDirectMusicTool::Flush",         "hand_dmus_flush"),
    ("IDirectMusicPerformance::GetNotificationPMsg",
                                        "hand_dmus_get_notification_pmsg"),
    ("IDirectMusicPerformance8::GetNotificationPMsg",
                                        "hand_dmus_get_notification_pmsg"),
    ("IDirectMusicLoader::GetObject",   "hand_dmus_loader_getobject"),
    ("IDirectMusicLoader::SetObject",   "hand_dmus_objdesc_in"),
    ("IDirectMusicLoader::EnumObject",  "hand_dmus_enum_object"),
    ("IDirectMusicLoader8::GetObject",  "hand_dmus_loader_getobject"),
    ("IDirectMusicLoader8::SetObject",  "hand_dmus_objdesc_in"),
    ("IDirectMusicLoader8::EnumObject", "hand_dmus_enum_object"),
    ("IDirectMusicObject::GetDescriptor", "hand_dmus_objdesc_out"),
    ("IDirectMusicObject::SetDescriptor", "hand_dmus_objdesc_in"),
    ("IDirectMusicObject::ParseDescriptor", "hand_dmus_parse_descriptor"),
    # The param positions differ between the performance/segment shape and
    # the track shape, and a hand function knows its signature by BEING
    # slot-specific (it receives no argc) -- so one variant per shape:
    #   _p6: GetParam(REFGUID, DWORD, DWORD, MUSIC_TIME, MUSIC_TIME*, void*)
    #   _t4: GetParam(REFGUID, MUSIC_TIME, MUSIC_TIME*, void*)
    #   _p5: SetParam(REFGUID, DWORD, DWORD, MUSIC_TIME, void*)
    #   _t3: SetParam(REFGUID, MUSIC_TIME, void*)
    ("IDirectMusicPerformance::GetParam", "hand_dmus_getparam_p6"),
    ("IDirectMusicPerformance::SetParam", "hand_dmus_setparam_p5"),
    ("IDirectMusicPerformance8::GetParam", "hand_dmus_getparam_p6"),
    ("IDirectMusicPerformance8::SetParam", "hand_dmus_setparam_p5"),
    ("IDirectMusicPerformance8::GetParamEx", "hand_dmus_getparamex"),
    ("IDirectMusicPerformance8::InitAudio", "hand_dmus_init_audio"),
    ("IDirectMusicSegment::GetParam",   "hand_dmus_getparam_p6"),
    ("IDirectMusicSegment::SetParam",   "hand_dmus_setparam_p5"),
    ("IDirectMusicSegment8::GetParam",  "hand_dmus_getparam_p6"),
    ("IDirectMusicSegment8::SetParam",  "hand_dmus_setparam_p5"),
    ("IDirectMusicTrack::GetParam",     "hand_dmus_getparam_t4"),
    ("IDirectMusicTrack::SetParam",     "hand_dmus_setparam_t3"),
]

# --------------------------------------------------------------------------
# THE REVERSE-PROXY LICENCE.  Which interfaces this generator will write an
# xmask bit for on a CA_IFACE_IN parameter -- i.e. which guest-implemented
# objects this surface will accept and hand to native code as a reverse proxy.
#
# The bit is not free and it is not symmetric with the forward direction.  A
# CA_IFACE_IN parameter with NO xmask still serves a proxy the guest got FROM
# us (winecom_to_native recognises a forward proxy by identity, not by type);
# what the bit adds is the right to build a NATIVE vtable around a GUEST object
# and let Wine call it.  So the question the licence answers is not "what type
# is this parameter" -- the roster says that, and the derivation below proves
# it -- but "can this port serve every method the native side will call on an
# object of that type".
#
# For the two entries below the answer is yes and it is CHECKED, not asserted:
# each must be a LEAF, meaning no slot of it takes or returns an interface
# pointer at all (assert_sinks_are_leaves).  A leaf cannot cascade: accepting
# one cannot pull a second guest object across behind it.
#
# For every OTHER interface on this roster the answer is no, and for a reason
# that is a property of COM rather than of any one signature: every one of them
# is IUnknown-derived, and libs/winecom/reverse.c's rev_query_interface asks the
# GUEST for any IID the roster carries and returns a reverse proxy of whatever
# comes back.  So a licence on any one of them is a licence on all of them --
# including the DirectMusic and moniker interfaces whose own tables refuse in
# BOTH directions (a DMUS_PMSG or a DMUS_OBJECTDESC reaches an interface
# pointer through its own members; a tag-dispatched void* payload has no type
# at all).  A reverse proxy of one of those is an object that answers E_NOTIMPL
# from inside Wine's own implementation, at a moment the guest cannot see and
# cannot handle.  Refusing at the boundary, by name, is the port's rule.
#
# IXAudio2EngineCallback is additionally [local]: it has no QueryInterface at
# all, so the cascade above cannot even be spelled for it.
REVERSE_SINKS = {
    "IXAudio2EngineCallback":
        "XAudio2's engine-wide reporting sink.  [local], so it has no "
        "QueryInterface and no reference count; XAudio2 keeps the pointer and "
        "calls three void methods on its mixer thread.",
    "IMMNotificationClient":
        "mmdevapi's device-notification sink.  mmdevapi stores the pointer in "
        "a list, calls the five On* methods from its own notification thread, "
        "and never asks it for another interface.",
    # 2026-09-01, the completeness pass.  The leaf rule below was relaxed
    # from "no interface anywhere in any slot" to "no interface may flow
    # guest->native through a sink slot" -- an interface IN-parameter of a
    # sink method arrives NATIVE->guest and is FORWARD-minted by
    # libs/winecom/reverse.c's CA_IFACE_IN arm, which cannot pull a second
    # guest object across.  What the licence still forbids, and the check
    # still proves, is a sink slot that RETURNS an interface to native: that
    # is a guest object crossing INTO Wine at a moment nothing marshals it.
    "IAudioSessionEvents":
        "the per-session notification sink.  audiopolicy's session manager "
        "stores the pointer and calls the seven On* methods from its own "
        "thread; every parameter is plain data (floats, GUIDs, LPCWSTR).",
    "IAudioSessionNotification":
        "the session-created sink.  OnSessionCreated's one parameter is an "
        "IAudioSessionControl the SERVER minted -- native->guest, "
        "forward-wrapped by the reverse path's own CA_IFACE_IN arm.",
    "IAudioVolumeDuckNotification":
        "the ducking sink.  Two On* methods, LPCWSTR and UINT32 parameters, "
        "nothing crossing back.",
    "IWbemObjectSink":
        "WMI's asynchronous result sink -- the whole async half of "
        "IWbemServices is 'you implement this, wbemprox calls it'.  "
        "Indicate hands the guest an ARRAY of IWbemClassObject the SERVER "
        "minted (native->guest, each element forward-wrapped by the reverse "
        "path's CA_IFACE_ARR_IN arm); SetStatus hands one more the same "
        "way.  Nothing flows guest->native through any slot.",
}

# Slots the client recognises by NUMBER rather than by name, emitted as
# #defines.  DestroyVoice is the one: dlls/combase/syscom.c keeps a registry of
# live voice hosts and this is where an entry leaves it.
NOTABLE_SLOTS = ["IXAudio2Voice::DestroyVoice"]

# Named refusals.  EMPTY, and that emptiness is the change this generator most
# needs a reader to understand.
#
# It used to carry four: IXAudio2::RegisterForCallbacks / UnregisterForCallbacks
# and IMMDeviceEnumerator::RegisterEndpointNotificationCallback /
# UnregisterEndpointNotificationCallback.  The reason was real -- each takes a
# COM object the APPLICATION implements and hands it to native code, which then
# calls it from its own mixer or notification thread, and faking that with a
# native pointer would run ppc64 bytes as x86-64 on an audio thread.
#
# libs/winecom/reverse.c builds the mirror now.  IXAudio2EngineCallback and
# IMMNotificationClient are on the roster above, so their CA_IFACE_IN rows carry
# the interface type the reverse direction needs (xaux) and the xmask bit that
# says the generator wrote it.  The XAudio2 pair is served straight off that
# ordinary marshal path; the MMDevice pair needs one extra guard and is
# described below.  Unregister works for the same reason Register does and with
# the SAME POINTER, because reverse proxies are interned by (guest pointer,
# interface) -- which is exactly what makes an unregister find its
# registration.  That last sentence is ppc64le/audio/gen_winecom.py's, about
# the same two slots of the 2.9 shape.
#
# The MMDevice pair is additionally routed through hand-written slots (see
# HAND_SLOTS): mmdevapi does NOT AddRef a registered client and unregisters it
# by POINTER COMPARISON without ever dereferencing it, so an application that
# unregisters something it never registered is well-defined on Windows and on
# Wine -- while translating that pointer would enter its guest AddRef, which for
# a pointer that is not a COM object at all is a jump into whatever it points
# at.  The hand slots keep a registry of what this surface actually registered
# and answer E_NOTFOUND for anything else, WITHOUT touching it.  That is the
# same shape as the live-voice registry in dlls/combase/syscom.c and it exists
# for the same kind of reason.
# EMPTY again (2026-09-01): IPropertyStore::SetValue was the one entry, and
# the completeness pass moved it to HAND_SLOTS -- the walker translates the
# guest-authored PROPVARIANT per tag (VT_UNKNOWN unwraps through
# winecom_to_native, unknown interface arms refuse AT RUNTIME naming the VT)
# instead of refusing the whole slot.  "Read-only on every measured path" was
# half the old reason, and that half is not a refusal reason on this port any
# more.
REFUSALS = {}


class Refused(Exception):
    pass


# --------------------------------------------------------------------------
# header reading -- the widl/MIDL dialect, same as ppc64le/audio's extractor
#
# NO preprocessor runs here: widl has already resolved every version
# conditional into the generated header, and the only #if left is
# __cplusplus/CINTERFACE.  Both branches are in the text and the C++ one -- the
# vtable in declaration order -- is the one these patterns read.
# --------------------------------------------------------------------------

MIDL_RE = re.compile(
    r'MIDL_INTERFACE\("([0-9a-fA-F-]+)"\)\s*\n\s*(\w+)\s*:\s*public\s+(\w+)\s*\n\s*\{',
    re.MULTILINE)
IFACE_PLAIN_RE = re.compile(
    r'^\s*(?:struct|interface)\s+(\w+)(?:\s*:\s*public\s+(\w+))?\s*\n\s*\{',
    re.MULTILINE)
VIRTUAL_RE = re.compile(
    r'virtual\s+(.+?)\s+STDMETHODCALLTYPE\s+(\w+)\s*\((.*?)\)\s*=\s*0\s*;',
    re.DOTALL)
ENUM_RE = re.compile(r'\benum\s+(\w+)\s*\{')
# `} A, B;` -- widl writes the enum's tag and every typedef spelling of it in
# one list, and XAUDIO2_PROCESSOR is the second name of
# XAUDIO2_WINDOWS_PROCESSOR_SPECIFIER.  A pattern that took only the first
# would refuse IXAudio2::Initialize for a by-value type it can see the
# definition of.
TYPEDEF_ENUM_RE = re.compile(r'\}\s*([\w\s,]+?)\s*;')
# `typedef enum X Y;` -- the other spelling of the same fact
TYPEDEF_ENUM_ALIAS_RE = re.compile(r'\btypedef\s+enum\s+(\w+)\s+(\w+)\s*;')
STRUCT_RE = re.compile(
    r'typedef\s+(?:struct|union)\s*(?:\w+\s*)?\{(.*?)\}\s*(\w+)\s*;', re.DOTALL)


def split_params(s):
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


def brace_body(text, open_idx):
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
    """-> {name: dict(uuid, base, header, own)} with each interface's OWN
    methods only; flattening needs the base chain and happens below."""
    ifaces, seen_at = {}, {}
    for m in MIDL_RE.finditer(text):
        uuid, name, base = m.groups()
        if name in ifaces:
            continue
        seen_at[name] = m.end() - 1
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
        own = []
        for ret, mname, args in VIRTUAL_RE.findall(brace_body(text, seen_at[name])):
            args = " ".join(args.split())
            # widl writes the IDL's defaultvalue() as a C++ default argument;
            # it is not part of the ABI and must not reach the classifier.
            params = [re.sub(r'\s*=.*$', '', p).strip()
                      for p in split_params(args) if p.strip()]
            own.append(dict(owner=name, name=mname,
                            ret=" ".join(ret.split()), params=params))
        i["own"] = own
    return ifaces


def flatten(ifaces, name, seen=None):
    """Base methods first, in declaration order, then this interface's own.
    Computed rather than written down: getting it wrong compiles fine and
    dispatches to the neighbouring slot."""
    seen = seen or set()
    if name in seen:
        sys.exit("gen_syscom_audio: %s inherits from itself" % name)
    i = ifaces[name]
    base = i.get("base")
    if base == "IUnknown":
        out = [dict(owner="IUnknown", name="QueryInterface", ret="HRESULT",
                    params=["REFIID riid", "void **ppvObject"]),
               dict(owner="IUnknown", name="AddRef", ret="ULONG", params=[]),
               dict(owner="IUnknown", name="Release", ret="ULONG", params=[])]
    elif base and base in ifaces:
        out = list(flatten(ifaces, base, seen | {name}))
    elif base:
        sys.exit("gen_syscom_audio: %s's base %s is outside the parsed header "
                 "set, so its slot numbers are unknown" % (name, base))
    else:
        out = []                       # a [local] non-IUnknown interface
    out = out + i["own"]
    # key order matches the committed roster's existing rows: slot first
    return [dict(slot=n, owner=s["owner"], name=s["name"], ret=s["ret"],
                 params=list(s["params"])) for n, s in enumerate(out)]


# The synthetic-IID namespace and derivation are ppc64le/audio/gen_interfaces.py's
# verbatim, so the same [local] interface gets the same private key on either
# surface and a reader comparing the two rosters sees one value, not two.
SYNTH_NS = b"wine-ppc64le/winecom/local-interface/"


def synth_iid(name):
    h = hashlib.sha1(SYNTH_NS + name.encode()).digest()
    b = bytearray(h[:16])
    b[6] = (b[6] & 0x0f) | 0x50        # version 5
    b[8] = (b[8] & 0x3f) | 0x80        # RFC 4122 variant
    return "%s-%s-%s-%s-%s" % (b[0:4].hex(), b[4:6].hex(), b[6:8].hex(),
                               b[8:10].hex(), b[10:16].hex())


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
                    out.update(n.strip() for n in t.group(1).split(",")
                               if n.strip())
        for m in TYPEDEF_ENUM_ALIAS_RE.finditer(text):
            out.update(m.groups())
    return out


def scan_structs(texts, iface_names):
    """-> (bearing, why): struct type names that reach an INTERFACE pointer
    through any member chain.  Neither the fact nor the hazard is visible in a
    signature -- XAUDIO2_VOICE_SENDS is plain data as far as the parameter list
    is concerned and carries XAUDIO2_SEND_DESCRIPTORs whose pOutputVoice members
    would reach FAudio as guest proxy pointers.  Deliberately transitive and
    deliberately crude: over-approximating costs a refusal, under-approximating
    hands the implementation a guest pointer."""
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


def scan_pointer_typedefs(texts):
    """-> {name: (base, extra_stars)} learned from the headers, so a spelling
    that hides a `*` is resolved rather than mistaken for a by-value type."""
    out = {}
    for text in texts:
        for m in re.finditer(r'typedef\s+([\w ]+?)\s*(\**)\s*(\w+)\s*;', text):
            base, stars, name = m.group(1).split()[-1], m.group(2), m.group(3)
            if not stars or name in out or name == base:
                continue
            out[name] = (base, len(stars))
    return out


# --------------------------------------------------------------------------
# roster construction
# --------------------------------------------------------------------------

def read_header(path, build):
    full = os.path.join(build, path)
    if not os.path.exists(full):
        sys.exit("gen_syscom_audio: no header at %s -- it is build output "
                 "(widl), so build first or pass --build" % full)
    with open(full, errors="replace") as fh:
        return fh.read()


def build_audio_rows(build):
    """-> (rows, enums, header_texts).  rows is {name: roster row}."""
    xa_text = read_header(XAUDIO2_HEADER, build)
    mm_texts = [read_header(h, build) for h in MMDEV_HEADERS]
    ps_texts = [read_header(h, build) for h in PROPSTORE_HEADERS]
    se_texts = [read_header(h, build) for h in SESSION_HEADERS]
    ctx_texts = [read_header(h, SRCTREE) for h in CONTEXT_HEADERS]
    si_texts = [read_header(h, build) for h in SYSINFO_HEADERS]
    si_base_texts = [read_header(h, build) for h in SYSINFO_BASE_HEADERS]

    # Parsed per header so each row records the header it was read from; no
    # interface in the AUDIO family inherits across the two files.  (The
    # session chain inherits WITHIN audiopolicy.h -- IAudioSessionManager2
    # derives from IAudioSessionManager -- which per-header parsing serves.)
    parsed = {XAUDIO2_HEADER: parse_midl(xa_text, XAUDIO2_HEADER)}
    for path, text in zip(MMDEV_HEADERS + PROPSTORE_HEADERS + SESSION_HEADERS,
                          mm_texts + ps_texts + se_texts):
        parsed[path] = parse_midl(text, path)

    # The SYSINFO family DOES inherit across files -- INetworkListManager's
    # base chain runs through oaidl.h's IDispatch -- so its headers parse into
    # one merged view for flatten() to resolve bases in.  Each interface still
    # records the header it was declared in.
    si_parsed = {}
    for path, text in zip(SYSINFO_HEADERS + SYSINFO_BASE_HEADERS,
                          si_texts + si_base_texts):
        for k, v in parse_midl(text, path).items():
            si_parsed.setdefault(k, v)
    parsed["<sysinfo>"] = si_parsed

    rows, synthetic = {}, []
    for want in (XAUDIO2_IFACES, MMDEV_IFACES, PROPSTORE_IFACES,
                 SESSION_IFACES, SYSINFO_IFACES):
        for name in want:
            got = next((g for g in parsed.values() if name in g), None)
            if got is None:
                sys.exit("gen_syscom_audio: %s is declared in none of %s"
                         % (name, ", ".join([XAUDIO2_HEADER] + MMDEV_HEADERS
                                            + PROPSTORE_HEADERS
                                            + SESSION_HEADERS
                                            + SYSINFO_HEADERS)))
            i = got[name]
            uuid = i["uuid"]
            row = dict(uuid=uuid or synth_iid(name),
                       base=i.get("base") or "(none)",
                       header=i["header"],
                       slots=flatten(got, name))
            if uuid is None:
                row["synthetic_iid"] = True
                synthetic.append(name)
            rows[name] = row

    texts = ([xa_text] + mm_texts + ps_texts + se_texts + ctx_texts + si_texts
             + si_base_texts)
    return rows, scan_enums(texts), texts, sorted(synthetic)


def merge_roster(build, base):
    """The committed roster with this generator's rows replaced by a fresh
    extraction.  Everything else -- the other 58 interfaces, the pointer
    aliases, the enums that were already there -- is carried through
    untouched, because their extractor is not in this tree."""
    rows, enums, texts, synthetic = build_audio_rows(build)

    out = dict(base)
    ifaces = {k: v for k, v in base["interfaces"].items() if k not in OWNED_IFACES}
    ifaces.update(rows)
    out["enums"] = sorted(set(base.get("enums", ())) | enums)
    out["interfaces"] = {k: ifaces[k] for k in sorted(ifaces)}
    out["synthetic_iid_interfaces"] = synthetic
    # key order: surface, iface_ptr_aliases, enums, [unresolved_params],
    # synthetic_iid_interfaces, interfaces
    order = [k for k in ("surface", "iface_ptr_aliases", "enums",
                         "unresolved_params", "synthetic_iid_interfaces",
                         "interfaces") if k in out]
    return {k: out[k] for k in order}, texts


# --------------------------------------------------------------------------
# parameter classification
# --------------------------------------------------------------------------

CA = dict(PASS=0, IFACE_IN=1, RIID=2, PPV_OUT=3, RET_PTR=4, EVENT=5,
          IFACE_ARR_IN=6, IFACE_OUT_STATIC=7, IFACE_ARR_OUT_STATIC=8)
CA_NAME = {v: "WINECOM_CA_" + k for k, v in CA.items()}


class Param:
    def __init__(self, raw):
        self.raw = " ".join(raw.split())
        t = re.sub(r'\[[^\]]*\]', '', self.raw).strip()
        self.array = "[" in self.raw
        t = re.sub(r'\bconst\b', ' ', t)
        toks = t.replace('*', ' * ').split()
        self.stars = toks.count('*') + (1 if self.array else 0)
        toks = [x for x in toks if x != '*']
        self.base = toks[0] if toks else ''

    def is_riid(self):
        return re.match(r'^(REFIID|REFGUID|REFCLSID)\b', self.raw) is not None

    def resolve(self, typedefs, ifaces):
        base, stars, n = self.base, self.stars, 0
        while base not in ifaces and base in typedefs and n < 16:
            base, extra = typedefs[base]
            stars += extra
            n += 1
        return base, stars


def classify(key, slot, ifaces, iface_index, typedefs, byval_ok, bearing,
             why_bearing):
    """-> (cls[], xaux[], aux, fp_shape, fpmask, fpwide, want_xaux) or raise
    Refused(reason).

    fp_shape keys the hand-written FORWARD form (the generic invoker calls with
    integer registers only); fpmask/fpwide say the same thing in the form
    libs/winecom's REVERSE direction reads, which marshals its own registers
    and needs to know which parameter is a float and how wide.  want_xaux is
    TRUE when any parameter is interface-typed -- the row must then be emitted
    even if every index in it is zero, because roster index 0 is a real
    interface and `any(xaux)` would drop the row that names it."""
    params = [Param(p) for p in slot["params"]]
    cls = [CA["PASS"]] * len(params)
    xaux = [0] * len(params)
    caux = [0] * len(params)
    aux = 0
    fpmask = fpwide = 0
    want_xaux = want_caux = False
    shape = []

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
                fpmask |= 1 << i
                if base in ("double", "DOUBLE"):
                    fpwide |= 1 << i
                continue
            if base not in byval_ok:
                raise Refused(
                    "by-value parameter `%s` is of a type this generator "
                    "cannot prove is integer-class on both ABIs; refusing "
                    "rather than assuming it is an enum" % p.raw)
            shape.append("i")
            continue

        shape.append("i")            # every pointer is an integer register

        if base == "void" and stars == 2:
            if i in RAW_VOID_OUT.get(key, ()):
                # licensed plain memory; the proof is in the table above
                cls[i] = CA["PASS"]
                continue
            if key in PPV_RIID_AT:
                ppv_i, riid_i = PPV_RIID_AT[key]
                if ppv_i != i or not params[riid_i].is_riid():
                    raise Refused(
                        "has a PPV_RIID_AT licence that no longer matches its "
                        "signature (the parameter list moved); re-derive it")
                cls[i] = CA["PPV_OUT"]
                cls[riid_i] = CA["RIID"]
                aux = riid_i
                continue
            prev = params[i - 1] if i else None
            if prev is not None and prev.is_riid():
                cls[i] = CA["PPV_OUT"]
                cls[i - 1] = CA["RIID"]
                aux = i - 1
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
            if i in IFACE_ARR_IN_AT.get(key, ()):
                cnt = IFACE_ARR_IN_AT[key][i]
                if stars != 2:
                    raise Refused(
                        "carries an IFACE_ARR_IN_AT licence but `%s` is not "
                        "an Iface** any more; re-derive the licence" % p.raw)
                cls[i] = CA["IFACE_ARR_IN"]
                xaux[i] = iface_index[base]
                caux[i] = cnt
                want_xaux = want_caux = True
                continue
            if stars == 1:
                cls[i] = CA["IFACE_IN"]
                # The roster index goes in xaux even though the FORWARD
                # dispatcher does not read it for an IN parameter (a proxy is
                # recognised by identity, not by type).  The REVERSE direction
                # does read it -- include/wine/winecom.h says so -- because a
                # guest-implemented object arriving as an argument has to be
                # given one of the rostered guest vtables, and a table without
                # the row can only fail closed.
                xaux[i] = iface_index[base]
                want_xaux = True
                continue
            if stars != 2:
                raise Refused(
                    "takes `%s`: an interface pointer at a level of "
                    "indirection this generator has no class for" % p.raw)
            cls[i] = CA["IFACE_OUT_STATIC"]
            xaux[i] = iface_index[base]
            want_xaux = True
            continue

        if base in byval_ok or base in ("GUID", "IID", "CLSID", "FMTID"):
            # a POINTER to a scalar or a GUID-family struct is plain memory;
            # without this, `INT *` and `IID *` fall into the interface
            # regex below (^I[A-Z] matches INT and IID) -- [MEASURED]
            # 2026-09-01, IAudioSessionEnumerator::GetCount and
            # IStorage::CopyTo both refused on exactly that.
            cls[i] = CA["PASS"]
            continue

        if UNROSTERED_IFACE_RE.match(base):
            raise Refused(
                "takes `%s`, an interface pointer of a type the wine-syscom "
                "roster does not carry -- there is no guest stub vtable for "
                "it, so it can be neither wrapped on the way out nor "
                "recognised on the way in" % p.raw)

        # an ordinary pointer to plain data: crosses as an address.  Both sides
        # are Wine PE code in one process built from these very declarations.
        cls[i] = CA["PASS"]

    fp = "".join(shape) + ">" + ("f" if FLOAT_TOKENS.search(slot["ret"]) else "i")
    return (cls, xaux, aux, (fp if "f" in "".join(shape) else None),
            fpmask, fpwide, want_xaux,
            (caux if want_caux else None))


# --------------------------------------------------------------------------
# marshal-header emission
# --------------------------------------------------------------------------

def c_guid(u):
    a, b, c, d, e = u.split("-")
    d4 = d + e
    return "{0x%s,0x%s,0x%s,{%s}}" % (
        a, b, c, ",".join("0x" + d4[i:i + 2] for i in range(0, 16, 2)))


BLOCK_NAME_RE = re.compile(r'static const struct winecom_slot slots_(\w+)\[')
XAUX_RE = re.compile(r'(static const unsigned char xaux_\w+\[\] = \{ )([^}]*?)\s*(\})')


def parse_marshal(text):
    """-> dict(head, enum, defines, blocks{name: paragraph}, order, array,
    trailing).

    The emitted file is paragraph-structured -- one blank line between the
    banner, the enum, the defines, each interface's decls+table, the interface
    array and whatever follows it -- and no paragraph contains a blank line.
    So a split on "\\n\\n" is exact, and --selfcheck proves it."""
    paras = text.split("\n\n")
    head, enum, array = paras[0], None, None
    defines, blocks, order, trailing = [], {}, [], []
    state = "pre-enum"
    for p in paras[1:]:
        m = BLOCK_NAME_RE.search(p)
        if state == "pre-enum":
            if not p.startswith("enum "):
                sys.exit("gen_syscom_audio: paragraph 2 is not the enum")
            enum, state = p, "defines"
            continue
        if state == "defines":
            if m is None:
                defines.append(p)
                continue
            state = "blocks"
        if state == "blocks":
            if m is not None:
                blocks[m.group(1)] = p
                order.append(m.group(1))
                continue
            state = "array"
        if state == "array":
            if not p.startswith("static const struct winecom_iface "):
                sys.exit("gen_syscom_audio: expected the interface array, got "
                         "%r" % p[:60])
            array, state = p, "trailing"
            continue
        trailing.append(p)
    return dict(head=head, enum=enum, defines=defines, blocks=blocks,
                order=order, array=array, trailing=trailing)


def render(head, order, blocks, iface_meta, defines, trailing):
    paras = [head]
    e = ["enum syscom_iface_index", "{"]
    for n, name in enumerate(order):
        e.append("    %s_IFACE_%s = %d," % (PREFIX, name, n))
    e.append("    %s_IFACE_COUNT = %d\n};" % (PREFIX, len(order)))
    paras.append("\n".join(e))
    paras.extend(defines)
    paras.extend(blocks[name] for name in order)
    a = ["static const struct winecom_iface syscom_com_ifaces[%s_IFACE_COUNT] ="
         % PREFIX, "{"]
    for name in order:
        uuid, count, iflags = iface_meta[name]
        a.append('    { "%s", %s,\n      %d, slots_%s%s },'
                 % (name, uuid, count, name,
                    ", " + iflags if iflags is not None else ""))
    a.append("};")
    paras.append("\n".join(a))
    paras.extend(trailing)
    return "\n\n".join(paras).rstrip("\n") + "\n"


def local_paragraph(order, is_local):
    return "\n".join(
        ["/* Interfaces that are NOT IUnknown-derived: slot 0 is a real method,"
         " not\n * QueryInterface.  libs/winecom's dispatcher serves slots 0..2"
         " from the proxy\n * table for every interface it is given, so"
         " combase's __wine_com_dispatch MUST\n * test this array and serve"
         " these itself before delegating. */",
         "static const unsigned char syscom_iface_local[%s_IFACE_COUNT] =" % PREFIX,
         "{"]
        + ["    %d,  /* %s */" % (1 if is_local[n] else 0, n) for n in order]
        + ["};"])


def selfcheck(path):
    """Parse the committed header and re-emit it from its own parts.  Proves the
    paragraph parse is exact, which is what licenses reusing the 58 blocks this
    generator did not write."""
    with open(path) as fh:
        have = fh.read()
    p = parse_marshal(have)
    meta = {}
    for m in re.finditer(r'\{ "(\w+)", (\{[^\n]*\}),\n      (\d+), slots_\w+(?:, ([\w|]+))? \},',
                         p["array"]):
        meta[m.group(1)] = (m.group(2), int(m.group(3)), m.group(4))
    missing = [n for n in p["order"] if n not in meta]
    if missing:
        sys.exit("selfcheck: the interface array does not list %s"
                 % ", ".join(missing))
    text = render(p["head"], p["order"], p["blocks"], meta, p["defines"],
                  p["trailing"])
    if text == have:
        print("selfcheck passed: %s parses into %d paragraph(s) -- %d interface "
              "block(s) -- and re-emits byte-identically"
              % (path, len(have.split("\n\n")), len(p["order"])))
        return 0
    for i, (a, b) in enumerate(zip(have.split("\n"), text.split("\n"))):
        if a != b:
            sys.exit("selfcheck FAILED at line %d:\n  have %r\n  emit %r"
                     % (i + 1, a, b))
    sys.exit("selfcheck FAILED: length differs (%d vs %d)" % (len(have), len(text)))


# --------------------------------------------------------------------------
# THE LEGACY BLOCKS: deriving their xmask from the roster, and withholding it
#
# The 58 interface blocks this generator did not write stop at aux2, so every
# CA_IFACE_IN in them has xmask 0 and fails closed -- which is the only safe
# reading of "no information", and is why include/wine/winecom.h spells the
# field the way it does.  The information is NOT actually missing, though: the
# roster carries the parameter TEXT those blocks were classified from, and this
# is where it is read back.
#
# What the derivation is for is proof, not convenience.  It re-derives every
# reused row's argument classes and interface indices from interfaces_syscom.json
# alone and CROSS-CHECKS them against the block as emitted; a single
# disagreement stops the generator, because it would mean the roster and the
# reused tables describe different functions and nothing downstream could be
# trusted.  Only then is the licence applied -- and for the legacy corpus the
# licence says no (see REVERSE_SINKS), so the bits stay clear and every one of
# them is NAMED in --report rather than silently absent.
# --------------------------------------------------------------------------

LEGACY_ROW_RE = re.compile(
    r'\{ "([\w:~]+)",\s*(NULL|"(?:[^"\\]|\\.)*")\s*,\s*(NULL|\w+)\s*,'
    r'\s*(NULL|\w+)\s*,\s*(\d+)', re.DOTALL)
DECL_RE = re.compile(
    r'static const unsigned char (\w+)\[\] = \{ ([^}]*) \};')


def derive_row(params, ifaces, iface_index, typedefs, key=None):
    """The roster's parameter text -> (cls[], xaux[]) for ONE slot, using the
    same rules classify() uses, but from the roster alone: no header text and
    no refusal, because a reused row's refusal was already decided by the
    generator that wrote it.  Returns None when the text does not determine the
    row (an untyped void**, an unrostered interface, an indirection with no
    class), which is itself a fail-closed answer.  `key` brings the per-slot
    licences (RAW_VOID_OUT, PPV_RIID_AT) along, so this independent prover
    reaches the same answer classify() does for a licensed slot -- the
    cross-check must compare like with like or every licensed upgrade dies
    at the derive fence."""
    P = [Param(x) for x in params]
    cls = [CA["PASS"]] * len(P)
    xaux = [0] * len(P)
    for i, prm in enumerate(P):
        base, stars = prm.resolve(typedefs, ifaces)
        if stars == 0:
            continue
        if base == "void" and stars == 2:
            if key is not None and i in RAW_VOID_OUT.get(key, ()):
                continue                       # licensed plain memory
            if key is not None and key in PPV_RIID_AT:
                ppv_i, riid_i = PPV_RIID_AT[key]
                if ppv_i == i and P[riid_i].is_riid():
                    cls[i] = CA["PPV_OUT"]
                    cls[riid_i] = CA["RIID"]
                    continue
            if i and P[i - 1].is_riid():
                cls[i] = CA["PPV_OUT"]
                cls[i - 1] = CA["RIID"]
                continue
            return None
        if base in ifaces:
            if stars == 1:
                cls[i] = CA["IFACE_IN"]
            elif stars == 2:
                cls[i] = CA["IFACE_OUT_STATIC"]
            else:
                return None
            xaux[i] = iface_index[base]
            continue
        if (base in BYVAL_INTEGER or base in BYVAL_INTEGER_TYPEDEFS or
                base in ("GUID", "IID", "CLSID", "FMTID")):
            continue          # a pointer to a scalar/GUID: plain memory --
                              # same fact classify() carries, same [MEASURED]
                              # INT/IID regex trap
        if UNROSTERED_IFACE_RE.match(base):
            return None
    return cls, xaux


def derive_legacy(old, roster, order, iface_index, typedefs):
    """The reused blocks' xaux entries are in the OLD roster's numbering (the
    block loop renumbers them on the way out), so every comparison below reads
    them through old["order"] first."""
    """-> (checked, withheld).  `checked` counts reused rows whose classes and
    interface indices re-derive EXACTLY from the roster; `withheld` names every
    CA_IFACE_IN in them, because none of their interfaces is licensed."""
    ifaces = roster["interfaces"]
    checked, withheld = 0, []
    for name, para in sorted(old["blocks"].items()):
        if name in OWNED_IFACES:
            continue
        decls = {m.group(1): [x.strip() for x in m.group(2).split(",")]
                 for m in DECL_RE.finditer(para)}
        for slot, m in enumerate(LEGACY_ROW_RE.finditer(para)):
            mname, refuse, cname, xname, argc = m.groups()
            if slot < 3:
                continue                    # IUnknown, served by the runtime
            if refuse != "NULL":
                continue                    # already closed in both directions
            if slot >= len(ifaces[name]["slots"]):
                sys.exit("gen_syscom_audio: reused block %s has more rows than "
                         "the roster has slots" % name)
            s = ifaces[name]["slots"][slot]
            if int(argc) != 1 + len(s["params"]):
                sys.exit("gen_syscom_audio: %s slot %d says argc %s and the "
                         "roster says %d" % (name, slot, argc,
                                             1 + len(s["params"])))
            got = derive_row(s["params"], ifaces, iface_index, typedefs,
                             key="%s::%s" % (s["owner"], s["name"]))
            emit_cls = ([CA[v.replace("WINECOM_CA_", "")] for v in decls[cname]]
                        if cname != "NULL" else [CA["PASS"]] * len(s["params"]))
            emit_x = ([iface_index[old["order"][int(v)]] if int(v) else 0
                       for v in decls[xname]] if xname != "NULL"
                      else [0] * len(s["params"]))
            if got is None:
                # The roster text does not determine this row.  That is only
                # consistent if the emitted row claims nothing either.
                if any(c != CA["PASS"] for c in emit_cls):
                    sys.exit("gen_syscom_audio: %s slot %d (%s) carries a "
                             "marshal plan the roster's own parameter text "
                             "cannot re-derive" % (name, slot, mname))
                continue
            dcls, dx = got
            if dcls != emit_cls:
                # A STRICT REFINEMENT -- every disagreeing position is one the
                # reused block left PASS and today's typedef knowledge proves
                # is really RIID/PPV_OUT/IFACE_* -- is not a description of a
                # different function; it is the offline prover's gap showing
                # up in a SERVED row, i.e. a typed out-interface passing RAW
                # (the GetShader leak class, on the served side).  The upgrade
                # pass rewrites these rows; anything else is still fatal.
                if all(e == CA["PASS"] for d, e in zip(dcls, emit_cls) if d != e):
                    checked += 1
                    continue
                sys.exit("gen_syscom_audio: %s slot %d (%s) re-derives as %s "
                         "from the roster and the reused block says %s -- the "
                         "roster and the reused tables describe different "
                         "functions" % (name, slot, mname, dcls, emit_cls))
            # The indices are compared POSITION BY POSITION and asymmetrically,
            # because the generator that wrote these blocks filled xaux in only
            # for the OUT classes -- the forward direction recognises an IN
            # parameter by identity and never needed its type.  So an OUT
            # position must agree exactly, and an IN position is expected to be
            # the zero nobody wrote.  THAT ZERO IS THE WHOLE PROBLEM: roster
            # index 0 is a real interface, which is why the reverse direction
            # reads xmask before xaux and why these rows fail closed.
            for i, c in enumerate(dcls):
                if c == CA["IFACE_OUT_STATIC"] and emit_x[i] != dx[i]:
                    sys.exit("gen_syscom_audio: %s slot %d (%s) re-derives "
                             "parameter %d as %s and the reused block's xaux "
                             "says index %d" % (name, slot, mname, i,
                                                order[dx[i]], emit_x[i]))
                if c == CA["IFACE_IN"] and emit_x[i] not in (0, dx[i]):
                    sys.exit("gen_syscom_audio: %s slot %d (%s) re-derives "
                             "IN-parameter %d as %s and the reused block's xaux "
                             "says index %d" % (name, slot, mname, i,
                                                order[dx[i]], emit_x[i]))
            checked += 1
            for i, c in enumerate(dcls):
                if c != CA["IFACE_IN"]:
                    continue
                tname = order[dx[i]]
                if tname in REVERSE_SINKS:
                    sys.exit("gen_syscom_audio: %s slot %d takes a licensed "
                             "reverse sink (%s) in a REUSED block, whose rows "
                             "have no xmask field to write the bit into"
                             % (name, slot, tname))
                withheld.append((name, slot, "%s::%s" % (s["owner"], s["name"]),
                                 i, tname))
    return checked, withheld


# --------------------------------------------------------------------------
# THE LEGACY UPGRADE PASS (2026-09-01).  The reused blocks carry refusals
# their offline generator decided with a weaker type prover than classify()'s
# -- `by-value LPOLESTR is not provably integer-class` names a POINTER
# typedef it could not resolve -- and the Witcher 3 GetShader crash class
# made the cost of a stale refusal concrete: a refused slot leaves out-params
# holding stack residue for a caller that never checks.
#
# The pass is DOUBLE-GATED, and both gates are the point:
#   1. the row's ORIGINAL refusal reason must match a LICENSED family below
#      -- the mechanical prover gaps, nothing else.  Reasons that encode a
#      semantic judgment (array-capable Next, tag-dispatched payloads,
#      interface-bearing structs, void** with no REFIID, unrostered types)
#      are NEVER upgraded by this pass, whatever classify() would say,
#      because classify() cannot re-detect what those reasons detected.
#   2. classify() must then prove the WHOLE signature under today's rules
#      (typedefs with provenance above, the roster, the enums).  One
#      unprovable parameter and the row stays byte-identical to the reuse.
#
# An upgraded row is emitted in this generator's own full-row form (it
# carries fpmask/fpwide/xmask like every owned row) with a provenance marker,
# and --report lists old reason against new plan for every one.  Rows that
# STAY refused keep their reason text byte-for-byte -- the pass never
# rewrites a refusal it does not lift.
UPGRADE_LICENSED_RE = re.compile(
    r': by-value (LPOLESTR|LPCOLESTR|PVOID|LPVOID|LPDWORD|LPLONG|LPBYTE|'
    r'LPGUID|LPCGUID|LPREFERENCE_TIME|LPWAVEFORMATEX|LPCWAVEFORMATEX|'
    r'LPCDSBUFFERDESC|LPLPDIRECTSOUNDBUFFER|SNB|short|HEVENT|HSEMAPHORE) '
    r'is not provably integer-class$|'
    r': by-value parameter `(?:LCID|DISPID|CIMTYPE|MUSIC_TIME|short)'
    r'[^`]*` is of a type this generator cannot prove is integer-class|'
    # A POINTER return is integer-class by the same one-hop licence as the
    # parameter form: it travels whole in RAX on MS-x64 and in r3 on ELFv2,
    # and both sides are Wine PE code in one address space, so the address
    # means the same thing.  classify() only ever refuses FLOAT returns, so
    # gate 2 holds these to the same whole-signature proof as everything
    # else.  (2026-09-01: IMalloc::Alloc, IRecordInfo::RecordCreate.)
    r': return type (?:LPVOID|PVOID) is not provably integer-class$|'
    # The void**-with-no-REFIID family joined the licence 2026-09-01 with a
    # narrower claim than the doc-comment above assumed: classify() CAN
    # re-detect this one -- its own void** branch refuses exactly when no
    # typing REFIID exists -- so gate 2 does the real work, and the
    # RAW_VOID_OUT / PPV_RIID_AT licences (each entry with its proof) are
    # what let a specific slot through it.  An entry-less slot stays refused
    # by classify() itself, byte-identical.
    r' arg \d+ is a void\*\* with no preceding REFIID|'
    # array-capable Iface** rows: admitted ONLY when the key sits in
    # SINGULAR_IFACE_OUT (checked in try_upgrade -- classify() cannot
    # re-detect array-ness, so the per-slot proof is the whole gate here).
    r' is array-capable and the method declares a count argument')

# the licence families whose SECOND gate is a membership table rather than
# classify() itself; try_upgrade consults these before calling classify.
ARRAY_CAPABLE_RE = re.compile(
    r' is array-capable and the method declares a count argument')

LEGACY_SERVED_ROW_RE = re.compile(
    r'    \{ "([\w:~]+)", (NULL), (NULL|\w+), (NULL|\w+), (\d+), 0, 0, 0 \},(?!\s*/\* runtime \*/)')

LEGACY_REFUSED_ROW_RE = re.compile(
    r'    \{ "([\w:~]+)",\n      "((?:[^"\\]|\\.)*)",\n'
    r'      NULL, NULL, (\d+), 0, 0, 0 \},')


def upgrade_legacy_block(name, para, ifaces, order, iface_index, typedefs,
                         byval_ok, bearing, why_bearing, stats, upgrade_log,
                         withheld, legacy_hand, legacy_hand_hits):
    """One reused block -> the same block with its LICENSED refusals
    reclassified.  Returns (paragraph, decls_to_prepend)."""
    slots = ifaces[name]["slots"]
    key_to_slot = {"%s::%s" % (s["owner"], s["name"]): s for s in slots}
    slot_no = {"%s::%s" % (s["owner"], s["name"]): s["slot"] for s in slots}
    new_decls = []

    def try_upgrade(m):
        key, reason, argc = m.group(1), m.group(2), int(m.group(3))
        # LEGACY_HAND first: these rows' reasons are the SEMANTIC families
        # the licence regex deliberately excludes -- their service is a
        # walker in dlls/combase/syscom.c, and the row only has to say so.
        # RET_VOID comes from the roster, checked the same way argc is.
        if key in legacy_hand:
            s = key_to_slot.get(key)
            if s is None or 1 + len(s["params"]) != argc:
                sys.exit("gen_syscom_audio: LEGACY_HAND cannot find %s "
                         "(argc %d) in the roster" % (key, argc))
            legacy_hand_hits.add(key)
            stats["run_upgraded"] += 1
            upgrade_log.append((name, key, reason,
                                "HAND-SERVED: %s" % legacy_hand[key][1]))
            flags = "WINECOM_F_HAND"
            if s["ret"] == "void":
                flags += "|WINECOM_F_RET_VOID"
            return ('    /* hand-served: was a legacy refusal -- the walker '
                    'is %s */\n'
                    '    { "%s", NULL, NULL, NULL, %d, %s, %d, 0, NULL, '
                    '0, 0, 0 },'
                    % (legacy_hand[key][1], key, argc, flags,
                       legacy_hand[key][0]))
        if not UPGRADE_LICENSED_RE.search(reason):
            return m.group(0)
        if ARRAY_CAPABLE_RE.search(reason) and key not in SINGULAR_IFACE_OUT:
            upgrade_log.append((name, key, reason,
                                "STAYS: array-capable and not licensed "
                                "SINGULAR_IFACE_OUT"))
            return m.group(0)
        s = key_to_slot.get(key)
        if s is None or 1 + len(s["params"]) != argc:
            sys.exit("gen_syscom_audio: upgrade pass cannot find %s (argc %d) "
                     "in the roster" % (key, argc))
        try:
            # the legacy corpus licenses no interface arrays, so the eighth
            # element (arr_caux) can only be None here; asserted below.
            (cls, xaux, aux, fp, fpmask, fpwide, want_xaux,
             arr_caux) = classify(
                key, s, ifaces, iface_index, typedefs, byval_ok,
                bearing, why_bearing)
            assert arr_caux is None, key
        except Refused as e:
            upgrade_log.append((name, key, reason, "STAYS: %s" % e))
            return m.group(0)
        if fp is not None:
            # a float-bearing legacy slot would need a hand shape;
            # none is licensed for the reused corpus in this pass
            upgrade_log.append((name, key, reason,
                                "STAYS: float-bearing (shape %s)" % fp))
            return m.group(0)
        n = slot_no[key]
        xmask = 0
        for i, c in enumerate(cls):
            if c in (CA["IFACE_OUT_STATIC"], CA["IFACE_ARR_OUT_STATIC"]):
                xmask |= 1 << i
            elif c == CA["IFACE_IN"]:
                tname = order[xaux[i]]
                if tname in REVERSE_SINKS:
                    xmask |= 1 << i
                else:
                    withheld.append((name, n, key, i, tname))
        cname = xname = "NULL"
        if any(c != CA["PASS"] for c in cls):
            cname = "cls_%s_%d" % (name, n)
            new_decls.append("static const unsigned char %s[] = { %s };"
                             % (cname, ", ".join(CA_NAME[c] for c in cls)))
        if want_xaux:
            xname = "xaux_%s_%d" % (name, n)
            new_decls.append("static const unsigned char %s[] = { %s };"
                             % (xname, ", ".join(str(x) for x in xaux)))
        stats["run_upgraded"] += 1
        upgrade_log.append((name, key, reason,
                            "SERVED: cls=[%s]" % ",".join(
                                CA_NAME[c].replace("WINECOM_CA_", "")
                                for c in cls)))
        return ('    /* upgraded from a legacy refusal -- see the banner */\n'
                '    { "%s", NULL, %s, %s, %d, 0, %d, 0, NULL, '
                '0x%02x, 0x%02x, 0x%02x },'
                % (key, cname, xname, argc, aux, fpmask, fpwide, xmask))

    para = LEGACY_REFUSED_ROW_RE.sub(try_upgrade, para)

    # ---- the SERVED-row refinement pass ---------------------------------
    # derive_legacy found rows the offline prover classified WEAKER than the
    # roster text proves -- a typed out-interface (PVOID* behind a REFIID,
    # an interface the roster now carries) passing RAW.  That is the
    # GetShader leak class on the served side: native writes a native
    # vtable into guest memory and the guest calls it as x86-64.  Rewrite
    # every such row to the derived classes; any disagreement that is NOT a
    # strict refinement is fatal in derive_legacy already.
    decl_of = {m.group(1): m for m in DECL_RE.finditer(para)}

    def try_refine(m):
        key, refuse, cname, xname, argc = m.groups()
        if refuse != "NULL":
            return m.group(0)          # refused rows: the pass above owns them
        s = key_to_slot.get(key)
        if s is None:
            return m.group(0)
        n = slot_no[key]
        if n < 3:
            return m.group(0)          # IUnknown, served by the runtime
        got = derive_row(s["params"], ifaces, iface_index, typedefs, key=key)
        if got is None:
            return m.group(0)
        dcls, dx = got
        emit_cls = ([CA[v.strip().replace("WINECOM_CA_", "")]
                     for v in decl_of[cname].group(2).split(",")]
                    if cname != "NULL" else [CA["PASS"]] * len(s["params"]))
        if dcls == emit_cls:
            return m.group(0)
        if not all(e == CA["PASS"] for d, e in zip(dcls, emit_cls) if d != e):
            return m.group(0)          # derive_legacy already died on these
        aux = 0
        for i, c in enumerate(dcls):
            if c == CA["RIID"]:
                aux = i
        xmask = 0
        for i, c in enumerate(dcls):
            if c in (CA["IFACE_OUT_STATIC"], CA["IFACE_ARR_OUT_STATIC"]):
                xmask |= 1 << i
            elif c == CA["IFACE_IN"]:
                tname = order[dx[i]]
                if tname in REVERSE_SINKS:
                    xmask |= 1 << i
                else:
                    withheld.append((name, n, key, i, tname))
        ncname = nxname = "NULL"
        if any(c != CA["PASS"] for c in dcls):
            ncname = "cls_up_%s_%d" % (name, n)
            new_decls.append("static const unsigned char %s[] = { %s };"
                             % (ncname, ", ".join(CA_NAME[c] for c in dcls)))
        if any(dx) or any(c in (CA["IFACE_IN"], CA["IFACE_OUT_STATIC"])
                          for c in dcls):
            nxname = "xaux_up_%s_%d" % (name, n)
            new_decls.append("static const unsigned char %s[] = { %s };"
                             % (nxname, ", ".join(str(x) for x in dx)))
        stats["run_refined"] += 1
        upgrade_log.append((name, key,
                            "SERVED but underclassified: %s" %
                            [CA_NAME[c].replace("WINECOM_CA_", "") for c in emit_cls],
                            "REFINED: cls=[%s]" % ",".join(
                                CA_NAME[c].replace("WINECOM_CA_", "")
                                for c in dcls)))
        return ('    /* reclassified: a typed out-interface was passing RAW -- '
                'see the banner */\n'
                '    { "%s", NULL, %s, %s, %s, 0, %d, 0, NULL, '
                '0x00, 0x00, 0x%02x },'
                % (key, ncname, nxname, argc, aux, xmask))

    para = LEGACY_SERVED_ROW_RE.sub(try_refine, para)
    if new_decls:
        marker = "static const struct winecom_slot slots_%s" % name
        idx = para.index(marker)
        para = para[:idx] + "\n".join(new_decls) + "\n" + para[idx:]
    return para


def assert_sinks_are_leaves(ifaces, typedefs):
    """A licensed reverse sink must not let a GUEST object flow INTO native
    code through any of its slots.  The original spelling of that rule was
    'leaf' -- no interface anywhere in any slot -- which was sufficient but
    stronger than the danger: an interface IN-parameter of a sink method
    arrives NATIVE->guest, and libs/winecom/reverse.c forward-mints it (the
    CA_IFACE_IN and CA_IFACE_ARR_IN arms), which cannot pull a guest object
    across.  So the check now proves, per slot:

      * a single-star interface parameter must resolve to a ROSTERED type
        (the reverse table needs its xaux index to mint the proxy) -- an
        unrostered one still stops generation;
      * a double-star interface parameter (an OUT, a guest object crossing
        into Wine) is still fatal, as is an interface RETURN;
      * everything else is plain data and crosses as before.

    Bounded is still bounded: accepting one guest sink can only ever mint
    FORWARD proxies at the guest, never a second reverse proxy."""
    for name in sorted(REVERSE_SINKS):
        if name not in ifaces:
            sys.exit("gen_syscom_audio: REVERSE_SINKS names %s, which is not "
                     "on the roster" % name)
        for s in ifaces[name]["slots"]:
            if s["ret"] not in ("void",):
                rbase, rstars = Param(s["ret"] + " r").resolve(typedefs, ifaces)
                if rbase in ifaces or UNROSTERED_IFACE_RE.match(rbase):
                    sys.exit("gen_syscom_audio: %s::%s returns `%s` -- a guest "
                             "object would flow into native code, which the "
                             "reverse licence forbids"
                             % (name, s["name"], s["ret"]))
            for pi, prm in enumerate(s["params"]):
                base, stars = Param(prm).resolve(typedefs, ifaces)
                if base in ifaces:
                    if stars >= 2:
                        key = "%s::%s" % (s["owner"], s["name"])
                        if pi in IFACE_ARR_IN_AT.get(key, ()):
                            continue  # a licensed IN-array: forward-minted
                        sys.exit("gen_syscom_audio: %s::%s takes `%s` -- an "
                                 "interface OUT on a sink slot is a guest "
                                 "object flowing into native code, which the "
                                 "reverse licence forbids"
                                 % (name, s["name"], prm))
                    continue          # rostered IN: forward-minted, safe
                if UNROSTERED_IFACE_RE.match(base):
                    sys.exit("gen_syscom_audio: %s::%s takes `%s`, an "
                             "UNROSTERED interface -- the reverse table would "
                             "have no xaux index to mint the argument with"
                             % (name, s["name"], prm))


def generate(roster, texts, old):
    ifaces = roster["interfaces"]
    order = sorted(ifaces)
    iface_index = {n: i for i, n in enumerate(order)}
    old_index = {n: i for i, n in enumerate(old["order"])}
    byval_ok = (set(BYVAL_INTEGER) | set(roster.get("enums", ()))
                | set(BYVAL_INTEGER_TYPEDEFS))
    typedefs = scan_pointer_typedefs(texts)
    typedefs.update(POINTER_TYPEDEFS)
    for k, v in roster.get("iface_ptr_aliases", {}).items():
        typedefs.setdefault(k, (v, 0))
    bearing, why_bearing = scan_structs(texts, set(ifaces))
    is_local = {n: bool(ifaces[n].get("synthetic_iid")) for n in order}

    hand_order = []

    def hand_slot(fn):
        if fn not in hand_order:
            hand_order.append(fn)
        return hand_order.index(fn)

    hand_index = {key: hand_slot(fn) for key, fn in HAND_SLOTS}
    shape_index = {shape: hand_slot(fn) for shape, fn in sorted(FP_SHAPES.items())}
    legacy_hand = {key: (hand_slot(fn), fn) for key, fn in LEGACY_HAND}
    legacy_hand_hits = set()

    stats = dict(marshalled=0, refused=0, hand=0, iunknown=0, fp=0,
                 legacy_marshalled=0, legacy_refused=0, legacy_iunknown=0,
                 legacy_checked=0, legacy_upgraded=0, legacy_refined=0,
                 run_upgraded=0, run_refined=0)
    refusal_log = []
    upgrade_log = []
    withheld = []

    # The licence is bounded because its members are leaves, and that is
    # checked before a single bit is written.
    assert_sinks_are_leaves(ifaces, typedefs)

    # ---- the legacy blocks: reused verbatim, xaux[] renumbered -------------
    blocks = {}
    for name, para in old["blocks"].items():
        if name in OWNED_IFACES:
            continue

        def renumber(m):
            vals = [old["order"][int(v.strip())] for v in m.group(2).split(",")]
            return (m.group(1) + ", ".join(str(iface_index[v]) for v in vals)
                    + " " + m.group(3))

        para = XAUX_RE.sub(renumber, para)
        # The upgrade pass runs AFTER the renumber: every xaux it writes is
        # already in the new order, and the renumber regex never sees them.
        para = upgrade_legacy_block(name, para, ifaces, order, iface_index,
                                    typedefs, byval_ok, bearing, why_bearing,
                                    stats, upgrade_log, withheld,
                                    legacy_hand, legacy_hand_hits)
        blocks[name] = para
        # Counted rather than carried over, so the closing statistics describe
        # the file that is actually being written.  A row that opens with the
        # method name and closes with the runtime comment is an IUnknown slot;
        # one whose SECOND field is a string is a refusal (its reason begins on
        # the next line, which is what the '",\n      "' looks for).
        for row in para.split("\n"):
            if not row.startswith('    { "'):
                continue
            if row.rstrip().endswith("/* runtime */"):
                stats["legacy_iunknown"] += 1
            else:
                stats["legacy_marshalled"] += 1
        stats["legacy_refused"] += para.count('",\n      "')
        # The pass is a RATCHET: a row upgraded by an earlier regeneration is
        # already served in `old` and this run's pass does nothing to it, so
        # the per-run counters above would decay to zero and the tail text
        # would never be stable.  The MARKERS are cumulative; count those.
        stats["legacy_upgraded"] += para.count(
            "/* upgraded from a legacy refusal")
        stats["legacy_refined"] += para.count(
            "/* reclassified: a typed out-interface was passing RAW")

    stats["legacy_marshalled"] -= stats["legacy_refused"]

    # ...and now the derivation the reused rows have no field to carry: every
    # one of their interface IN-parameters, re-derived from the roster's own
    # parameter text, cross-checked against the block as emitted, and WITHHELD.
    stats["legacy_checked"], legacy_withheld = derive_legacy(
        old, roster, order, iface_index, typedefs)
    withheld.extend(legacy_withheld)

    # Every LEGACY_HAND key must have rewritten a refused row THIS run or a
    # previous one (the marker comment survives regeneration): a miss means
    # the table and the walker list disagree about what exists, which is
    # exactly the drift this check exists to stop.
    for key in legacy_hand:
        if key in legacy_hand_hits:
            continue
        if any(('{ "%s", NULL, NULL, NULL,' % key) in p
               for p in blocks.values()):
            continue                    # rewritten by an earlier regeneration
        sys.exit("gen_syscom_audio: LEGACY_HAND names %s but no refused (or "
                 "previously hand-served) legacy row carries it" % key)

    # ---- this generator's own blocks --------------------------------------
    for name in OWNED_IFACES:
        if name not in ifaces:
            sys.exit("gen_syscom_audio: %s is missing from the roster" % name)
        rows, decls = [], []
        for s in ifaces[name]["slots"]:
            key = "%s::%s" % (s["owner"], s["name"])
            argc = 1 + len(s["params"])
            if s["slot"] < 3 and not is_local[name]:
                rows.append('    { "%s", NULL, NULL, NULL, %d, 0, 0, 0 },'
                            '  /* runtime */' % (key, 1 if s["slot"] else 3))
                stats["iunknown"] += 1
                continue
            flags = ["WINECOM_F_RET_VOID"] if s["ret"] == "void" else []
            reason = REFUSALS.get(key)
            hand = hand_index.get(key)
            cls = xaux = None
            aux = fpmask = fpwide = 0
            want_xaux = False
            arr_caux = None
            if reason is None and hand is None:
                try:
                    (cls, xaux, aux, fp, fpmask, fpwide,
                     want_xaux, arr_caux) = classify(
                        key, s, ifaces, iface_index, typedefs, byval_ok,
                        bearing, why_bearing)
                    if fp is not None:
                        if fp not in shape_index:
                            raise Refused(
                                "passes a float by value in the shape `%s`, "
                                "which no hand-written form implements; the "
                                "generic invoker calls with integer registers "
                                "only, so serving it would put the value in "
                                "the wrong register file.  Add the shape to "
                                "FP_SHAPES in ppc64le/syscom/gen_syscom_audio.py "
                                "and the function beside its siblings" % fp)
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
                            '      NULL, NULL, %d, 0, 0, 0 },'
                            % (key, key, reason.replace('"', "'"), argc))
                stats["refused"] += 1
                refusal_log.append((name, s["slot"], key, reason))
                continue
            # THE XMASK, and the licence that governs it.  A bit says "xaux[i]
            # is a roster index this generator wrote", which the REVERSE
            # direction reads and the forward direction does not need.  It is
            # written for every position the classifier filled -- except a
            # CA_IFACE_IN whose interface is not in REVERSE_SINKS, where the
            # bit is WITHHELD so the parameter fails closed and is refused by
            # name.  Withholding costs a guest-implemented object of that type
            # a refusal; granting it would let this surface build a native
            # vtable it cannot serve every method of.
            xmask = 0
            if cls is not None:
                for i, c in enumerate(cls):
                    if c == CA["IFACE_OUT_STATIC"] or c == CA["IFACE_ARR_OUT_STATIC"]:
                        xmask |= 1 << i
                    elif c == CA["IFACE_IN"]:
                        tname = order[xaux[i]]
                        if tname in REVERSE_SINKS:
                            xmask |= 1 << i
                        else:
                            withheld.append((name, s["slot"], key, i, tname))
            cname = xname = "NULL"
            if cls is not None and any(c != CA["PASS"] for c in cls):
                cname = "cls_%s_%d" % (name, s["slot"])
                decls.append("static const unsigned char %s[] = { %s };"
                             % (cname, ", ".join(CA_NAME[c] for c in cls)))
            if want_xaux:
                xname = "xaux_%s_%d" % (name, s["slot"])
                decls.append("static const unsigned char %s[] = { %s };"
                             % (xname, ", ".join(str(x) for x in xaux)))
            if hand is not None:
                # A hand-written FORWARD slot.  When the classifier proved the
                # WHOLE signature -- which is every shape-keyed float hand and
                # no named one -- the row also carries the plan and
                # WINECOM_F_REV, so libs/winecom/reverse.c can serve the reverse
                # call from the table with no hand function at all.  Same rule,
                # same words, as ppc64le/audio/gen_winecom.py.
                if cls is not None:
                    flags.append("WINECOM_F_REV")
                rows.append('    { "%s", NULL, %s, %s, %d, '
                            'WINECOM_F_HAND%s, %d, 0, NULL, 0x%02x, 0x%02x, '
                            '0x%02x },'
                            % (key, cname, xname, argc,
                               "".join("|" + f for f in flags), hand,
                               fpmask, fpwide, xmask))
                stats["hand"] += 1
                continue
            # aux2 carries CA_IFACE_ARR_IN's count-parameter index (the field
            # comment in winecom.h); every other row leaves it zero.  There is
            # at most one licensed array per slot (IFACE_ARR_IN_AT).
            aux2 = 0
            if arr_caux is not None and cls is not None:
                for i, c in enumerate(cls):
                    if c == CA["IFACE_ARR_IN"]:
                        aux2 = arr_caux[i]
                        # the arm mints elements at the guest; the withheld
                        # rule does not apply (nothing flows guest->native),
                        # so the xmask bit is granted for the element type.
                        xmask |= 1 << i
            rows.append('    { "%s", NULL, %s, %s, %d, %s, %d, %d, NULL, '
                        '0x%02x, 0x%02x, 0x%02x },'
                        % (key, cname, xname, argc, "|".join(flags) or "0",
                           aux, aux2, fpmask, fpwide, xmask))
            stats["marshalled"] += 1
        blocks[name] = "\n".join(
            decls + ["static const struct winecom_slot slots_%s[%d] =\n{" % (name, len(rows))]
            + rows + ["};"])

    # ---- the defines paragraph -------------------------------------------
    defines = ["#define %s_HAND_COUNT %d" % (PREFIX, len(hand_order))]
    for key in NOTABLE_SLOTS:
        owner, method = key.split("::")
        seen = set()
        for n in order:
            for s in ifaces[n]["slots"]:
                if s["owner"] == owner and s["name"] == method:
                    seen.add(s["slot"])
        if len(seen) != 1:
            sys.exit("gen_syscom_audio: notable slot %s is at slot number(s) %s; "
                     "the client tests ONE number" % (key, sorted(seen)))
        defines.append("#define %s_SLOT_%s_%s %d"
                       % (PREFIX, owner, method, seen.pop()))
    defines.append("/* hand_funcs[] order in dlls/combase/syscom.c:\n%s */"
                   % "".join(" *   %d %s\n" % (i, f) for i, f in enumerate(hand_order)))

    # WINECOM_IF_LOCAL is the [local] fact in the form libs/winecom reads.
    # combase's own dispatcher claims these on the FORWARD path
    # (syscom_iface_local[] below); the REVERSE dispatcher has no second
    # dispatcher to claim them in, so it needs the flag on the row.
    meta = {n: (c_guid(ifaces[n]["uuid"]), len(ifaces[n]["slots"]),
                "WINECOM_IF_LOCAL" if is_local[n] else "0") for n in order}
    total = sum(len(ifaces[n]["slots"]) for n in order)
    n_local = sum(1 for n in order if is_local[n])
    tail = ("/* wine-syscom: %d interface(s), %d vtable slot(s).\n"
            " * The %d audio row(s) generated here: %d slot(s) marshalled, %d "
            "hand-written\n * (%d of them float-bearing, routed by argument "
            "shape), %d refused with a\n * named reason, %d IUnknown slot(s) "
            "served by the runtime, %d interface(s)\n * [local] and served by "
            "combase's own dispatcher.  The %d reused row(s):\n * %d "
            "marshalled (%d of those upgraded from legacy refusals by the\n"
            " * reclassification pass -- see gen_syscom_audio.py's "
            "UPGRADE_LICENSED_RE --\n * and %d REFINED where a typed "
            "out-interface was passing RAW),\n * %d refused, %d IUnknown; "
            "%d of them re-derived from\n"
            " * the roster and cross-checked against this file.\n"
            " * Reverse-proxy licence: %s.  %d interface IN-parameter(s)\n"
            " * withheld, each of which fails closed. */"
            % (len(order), total, len(OWNED_IFACES), stats["marshalled"],
               stats["hand"], stats["fp"], stats["refused"], stats["iunknown"],
               n_local, len(order) - len(OWNED_IFACES),
               stats["legacy_marshalled"], stats["legacy_upgraded"],
               stats["legacy_refined"], stats["legacy_refused"],
               stats["legacy_iunknown"], stats["legacy_checked"],
               ", ".join(sorted(REVERSE_SINKS)), len(withheld)))

    head = BANNER % (len(order), total, len(OWNED_IFACES),
                     len(order) - len(OWNED_IFACES))
    text = render(head, order, blocks, meta, defines,
                  [local_paragraph(order, is_local), tail])
    return text, stats, refusal_log, hand_order, withheld, upgrade_log


# --------------------------------------------------------------------------
# driver
# --------------------------------------------------------------------------

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--build", default=os.environ.get("BUILD", SRCTREE),
                    help="tree holding the widl output (default: in-tree)")
    ap.add_argument("--roster", action="store_true")
    ap.add_argument("--marshal", action="store_true")
    ap.add_argument("--selfcheck", metavar="FILE")
    ap.add_argument("--report", action="store_true")
    ap.add_argument("--json", metavar="FILE")
    ap.add_argument("--out", metavar="FILE")
    ap.add_argument("--check", metavar="FILE")
    args = ap.parse_args()

    if args.selfcheck:
        return selfcheck(args.selfcheck)

    with open(ROSTER) as fh:
        base = json.load(fh)
    roster, texts = merge_roster(args.build, base)

    if args.roster:
        n = len(OWNED_IFACES)
        slots = sum(len(roster["interfaces"][k]["slots"]) for k in OWNED_IFACES)
        print("wine-syscom audio family: %d interface(s), %d vtable slot(s); "
              "roster total %d / %d"
              % (n, slots, len(roster["interfaces"]),
                 sum(len(i["slots"]) for i in roster["interfaces"].values())))
        print("  synthetic IIDs (not IUnknown-derived, no IID in the header): %s"
              % ", ".join(roster["synthetic_iid_interfaces"]))
        text = json.dumps(roster, indent=2, sort_keys=False) + "\n"
        if args.check:
            with open(args.check) as fh:
                have = fh.read()
            if have == text:
                print("check passed: %s is byte-identical to a regeneration"
                      % args.check)
                return 0
            sys.exit("gen_syscom_audio: %s has DRIFTED from Wine's headers.  "
                     "Regenerate it (--roster --json) and re-run every gate."
                     % args.check)
        if args.json:
            with open(args.json, "w") as fh:
                fh.write(text)
            print("wrote %s" % args.json)
        return 0

    # --marshal / --report
    target = args.check or args.out or MARSHAL
    with open(MARSHAL) as fh:
        old = parse_marshal(fh.read())
    text, stats, refusals, hand_order, withheld, upgrades = generate(roster, texts, old)

    print("wine-syscom: %d interface(s), %d vtable slot(s); audio family "
          "%d marshalled, %d hand-written (%d float-shaped), %d refused, "
          "%d IUnknown; %d hand function(s)"
          % (len(roster["interfaces"]),
             sum(len(i["slots"]) for i in roster["interfaces"].values()),
             stats["marshalled"], stats["hand"], stats["fp"], stats["refused"],
             stats["iunknown"], len(hand_order)))
    print("legacy corpus: %d marshalled (%d UPGRADED from refusals by the "
          "reclassification pass, %d refined from raw pass-through), "
          "%d still refused, %d IUnknown"
          % (stats["legacy_marshalled"], stats["legacy_upgraded"],
             stats["legacy_refined"],
             stats["legacy_refused"], stats["legacy_iunknown"]))

    print("reverse-proxy licence: %s; %d interface IN-parameter(s) WITHHELD "
          "across %d slot(s), each fails closed and is refused by name"
          % (", ".join(sorted(REVERSE_SINKS)), len(withheld),
             len({(w[0], w[1]) for w in withheld})))

    if args.report:
        print("\nreverse-proxy licence WITHHELD (no xmask bit; a "
              "guest-implemented object at this position is refused):")
        for iname, slot, key, pidx, tname in withheld:
            print("  %-34s slot %-3d param %d  %-32s %s"
                  % (iname, slot, pidx, tname, key))
        print("\nhand-written slots (hand_funcs[] order):")
        for i, f in enumerate(hand_order):
            keys = [k for k, fn in HAND_SLOTS if fn == f] or \
                   ["<shape %s>" % s for s, fn in FP_SHAPES.items() if fn == f]
            print("  %d  %-30s %s" % (i, f, ", ".join(keys)))
        print("\nlegacy rows visited by the upgrade pass (SERVED = reclassified,"
              " STAYS = still refused):")
        for iname, key, reason, verdict in upgrades:
            print("  %-30s %s\n      was: %s\n      now: %s"
                  % (iname, key, reason.split(".")[0][:100], verdict))
        print("\nrefused slots, by reason:")
        for name, slot, key, reason in refusals:
            print("  %s slot %d\n      %s\n      %s"
                  % (name, slot, key, reason.split(".")[0]))

    if args.check:
        with open(args.check) as fh:
            have = fh.read()
        if have == text:
            print("\ncheck passed: %s matches the roster" % args.check)
            return 0
        for i, (a, b) in enumerate(zip(have.split("\n"), text.split("\n"))):
            if a != b:
                print("\nfirst difference at line %d:\n  have %r\n  emit %r"
                      % (i + 1, a, b), file=sys.stderr)
                break
        sys.exit("gen_syscom_audio: %s has DRIFTED from the roster.  "
                 "Regenerate it (--marshal --out) and re-run every gate."
                 % args.check)
    if args.out:
        # The upgrade pass is a ratchet whose bookkeeping (legacy_checked)
        # reads the PREVIOUS file, so one write may not be the fixed point.
        # Iterate until re-generation reproduces its own output; two rounds
        # in practice, three tolerated, more is a bug.
        for round_ in range(3):
            with open(args.out, "w") as fh:
                fh.write(text)
            old2 = parse_marshal(text)
            text2 = generate(roster, texts, old2)[0]
            if text2 == text:
                break
            text = text2
        else:
            sys.exit("gen_syscom_audio: --out did not reach a fixed point "
                     "in 3 rounds")
        print("\nwrote %s (fixed point after %d regeneration(s))"
              % (args.out, round_ + 1))
    return 0


if __name__ == "__main__":
    sys.exit(main())
