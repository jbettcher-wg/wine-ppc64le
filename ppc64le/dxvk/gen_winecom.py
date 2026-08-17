#!/usr/bin/env python3
"""Emit the winecom marshal tables for the DXVK surface.

  ./gen_winecom.py --surface dxvk --out ../../dlls/d3d11/d3d11_marshal.h
  ./gen_winecom.py --report          # what is marshalled, what is refused, why
  ./gen_winecom.py --check ../../dlls/d3d11/d3d11_marshal.h

INPUT is interfaces_dxvk.json -- the ONE roster, the same file
dlls/d3d11/d3d11.thunks hands spec2thunk to build the guest trap module.  The
two generators must agree about interface order and slot counts or a call
lands on the neighbouring slot with the neighbour's argument types; both sort
by interface name, and libs/winecom cross-checks every IID and slot_count at
attach as the last line of defence.

OUTPUT is a header of `struct winecom_slot` / `struct winecom_iface` tables
(include/wine/winecom.h), consumed by dlls/d3d11/main.c.  This is the D3D11
sibling of the d3d12 lane's d3d12_marshal.h.

WHAT THIS GENERATOR IS FOR.  Every argument of every slot must be classified
before the call may cross: an interface pointer that crosses unclassified is
handed to the guest as a NATIVE vtable, and the guest's first method call
executes ppc64 bytes as x86-64.  So the rule here is CLASSIFY OR REFUSE, never
pass-and-hope, and a parameter shape the classifier does not recognise stops
generation rather than emitting a CA_PASS.  The refusals are all named, carry
their reason into the FIXME the runtime prints once, and are counted in
--report.

WHAT IT REFUSES, AND WHY EACH ONE IS A REAL HAZARD RATHER THAN LAZINESS -- see
REFUSALS below for the exact texts:

  * structs that carry interface pointers.  Computed here by walking the
    header's own struct bodies transitively, not from a hand list: a
    D3D11_VIDEO_PROCESSOR_STREAM passed by pointer contains
    ID3D11VideoProcessorInputView* members that would reach DXVK as guest
    proxy pointers.  This independently rediscovers the nine
    ID3D11VideoContext slots dxvk-ppc64le/docs/hazard-hunt.md §3.1 measured.
  * raw void** out-parameters with no REFIID to type them.
  * WCHAR-bearing signatures.  DXVK's native headers say
    `typedef wchar_t WCHAR` (src/include/native/windows/windows_base.h:30),
    which is FOUR bytes here; the guest PE's WCHAR is two.  A string crossing
    unconverted is not a subtle defect, but it is a silent one.
  * by-value floats, unless a hand-written slot carries them.  The unixlib
    boundary calls with the widest INTEGER form, so a float argument would
    arrive in the wrong register file entirely.
  * by-value types that are not integer-class.  Fail-closed: an unknown
    by-value spelling stops generation, because "it is probably an enum" is
    how a by-value aggregate silently becomes four bytes of garbage.
  * array out-parameters whose element count arrives through a UINT*.

Copyright 2026 the ppc64le port authors

This library is free software; you can redistribute it and/or modify it under
the terms of the GNU Lesser General Public License as published by the GNU
Free Software Foundation; either version 2.1 of the License, or (at your
option) any later version.
"""

import argparse
import json
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
HEADERS = os.path.join(HERE, "src", "include", "native", "directx")

# --------------------------------------------------------------------------
# by-value types that cross as an integer register, and are the same width on
# both sides.  ANYTHING NOT LISTED STOPS GENERATION -- see the module banner.
# The enum names in the roster JSON are added to this set at run time; an enum
# is `int`-class in both ABIs.
# --------------------------------------------------------------------------
BYVAL_INTEGER = frozenset("""
    UINT INT LONG ULONG DWORD WORD BYTE BOOL WINBOOL UINT8 UINT16 UINT32
    UINT64 INT8 INT16 INT32 INT64 SIZE_T SSIZE_T HMODULE HMONITOR HWND
    ULONG64 LONG64 unsigned int short char long
""".split())

# HWND IS ON THAT LIST, AND IT WAS NOT ALWAYS.
#
# It used to be refused by name in BYVAL_OPAQUE below, on the true statement
# that a Wine window handle means nothing to DXVK's native WSI.  What changed
# is that DXVK now has a WSI backend that hands the handle straight back to
# Wine (ppc64le/dxvk/dxvk-patches/0003-win32u-wsi-backend.patch, driver name
# "Win32u") and asks win32u's client-surface layer for a VkSurfaceKHR on it.
# The value itself never needed converting: the guest PE calls Wine's own
# user32, so there is exactly ONE window-handle namespace in the process and
# the same integer names the same window on both sides.  That is the whole
# reason this could stop being a refusal without becoming a lie -- unlike
# HANDLE, where DXVK's native side has its own encoding (a tagged eventfd) and
# the two namespaces genuinely collide.
#
# 26 slots and 13 DXGI_SWAP_CHAIN_DESC-bearing slots came back with it; four of
# them are hand-written below because presentation needs an order of
# operations, not just a marshalled argument list.

# By-value types that ARE integer-class but still may not cross, because the
# integer means something different on each side of the boundary.  Refused by
# name with the reason, never passed.
BYVAL_OPAQUE = {
    "HANDLE":
        "takes a by-value HANDLE.  A Wine HANDLE is a Wine object; DXVK's "
        "native side encodes an event as the tagged eventfd "
        "0x4556464400000000|fd (src/include/native/windows/"
        "dxvk_native_event.h) and a shared resource as its own key.  Handing "
        "one namespace's integer to the other is the exact collision "
        "ppc64le/vkd3d's tagged-handle series was written to prevent -- "
        "MEASURED there as eight bytes written into a live pipe",
    "HDC":
        "takes an HDC, a GDI object with no meaning on DXVK's native side",
}

# 8-byte aggregates that are ONE integer register on both the MS-x64 and the
# ELFv2 side, so they cross by value with no repacking.  Each is here because
# its layout was checked, not because it looked small.
BYVAL_AGGREGATE = {
    "LUID": "{ DWORD LowPart; LONG HighPart; } -- 8 bytes, one register on "
            "both ABIs",
    "SIZE": "{ LONG cx; LONG cy; } -- 8 bytes, one register on both ABIs",
}

# Pointer spellings that are pointers without a `*` in the source text.
POINTER_TYPEDEFS = {
    "LPSTR": "char", "LPCSTR": "char", "LPWSTR": "WCHAR", "LPCWSTR": "WCHAR",
    "REFIID": "GUID", "REFGUID": "GUID", "REFCLSID": "GUID",
}

WCHAR_TOKENS = re.compile(r'\b(WCHAR|LPWSTR|LPCWSTR|BSTR|PWSTR|OLECHAR)\b')
FLOAT_TOKENS = re.compile(r'\b(FLOAT|float|double|DOUBLE|D3DVALUE)\b')

# --------------------------------------------------------------------------
# Hand-written slots.  Keyed by "Owner::Method", so an inherited slot gets the
# same hand function in every derived interface's vtable -- which is the whole
# point: ID3D11DeviceChild::GetPrivateData appears in 60-odd vtables and there
# is exactly one right answer for it.  The C functions live in
# dlls/d3d11/main.c and the order here IS the hand_funcs[] order there.
# --------------------------------------------------------------------------
HAND_SLOTS_DXVK = [
    ("ID3D11DeviceChild::GetPrivateData",        "hand_get_private_data"),
    ("ID3D11Device::GetPrivateData",             "hand_get_private_data"),
    ("ID3D10DeviceChild::GetPrivateData",        "hand_get_private_data"),
    ("ID3D10Device::GetPrivateData",             "hand_get_private_data"),
    ("IDXGIObject::GetPrivateData",              "hand_get_private_data"),
    ("ID3D11DeviceChild::SetPrivateData",        "hand_set_private_data"),
    ("ID3D11Device::SetPrivateData",             "hand_set_private_data"),
    ("ID3D10DeviceChild::SetPrivateData",        "hand_set_private_data"),
    ("ID3D10Device::SetPrivateData",             "hand_set_private_data"),
    ("IDXGIObject::SetPrivateData",              "hand_set_private_data"),
    ("ID3D11DeviceChild::SetPrivateDataInterface", "hand_set_private_data_iface"),
    ("ID3D11Device::SetPrivateDataInterface",      "hand_set_private_data_iface"),
    ("ID3D10DeviceChild::SetPrivateDataInterface", "hand_set_private_data_iface"),
    ("ID3D10Device::SetPrivateDataInterface",      "hand_set_private_data_iface"),
    ("IDXGIObject::SetPrivateDataInterface",       "hand_set_private_data_iface"),
    ("ID3D11DeviceContext::ClearDepthStencilView", "hand_clear_depth_stencil_view"),
    ("ID3D11DeviceContext::SetResourceMinLOD",     "hand_set_resource_min_lod"),
    ("ID3D11DeviceContext::GetResourceMinLOD",     "hand_get_resource_min_lod"),
    ("ID3D10Device::ClearDepthStencilView",        "hand_d3d10_clear_depth_stencil_view"),
    # Presentation.  Every argument of these four would marshal correctly on
    # its own now that an HWND may cross; what the generator cannot express is
    # the ORDER OF OPERATIONS around them.  win32u wants its client surface
    # updated before a present and marked presented after, on a Wine thread --
    # which the application's call into Present is and DXVK's submission
    # thread, where the real vkQueuePresentKHR happens, is not.  The creation
    # slots have to hand the unixlib the window's client size before DXVK asks
    # for it, and record which window a swapchain presents to, because
    # Present's own signature does not carry one.
    ("IDXGIFactory::CreateSwapChain",              "hand_create_swapchain"),
    ("IDXGIFactory2::CreateSwapChainForHwnd",      "hand_create_swapchain_for_hwnd"),
    ("IDXGISwapChain::Present",                    "hand_swapchain_present"),
    ("IDXGISwapChain1::Present1",                  "hand_swapchain_present1"),
]

# --------------------------------------------------------------------------
# Refusals decided here rather than derived, each with the reason the runtime
# prints once.  Keyed "Owner::Method" like HAND_SLOTS.
# --------------------------------------------------------------------------
REFUSALS_DXVK = {
    # The two swapchain routes that have no window at all.  Upstream's
    # backends answer these by creating a window of their own -- DXVK's DXGI
    # calls DxgiSurfaceFactory::CreateDummyWindow for exactly that -- and this
    # lane's backend owns no windows: it presents to windows Wine created, on
    # behalf of an application that asked for one.  Refused here rather than
    # left to fail inside DXVK, where the reason would arrive as
    # VK_ERROR_INITIALIZATION_FAILED with no mention of windows.
    "IDXGIFactory2::CreateSwapChainForCoreWindow":
        "creates a swapchain for a WinRT CoreWindow, which has no HWND.  This "
        "lane presents through win32u's client-surface layer, which is a layer "
        "over a Wine window handle; there is no window here to attach a "
        "surface to, and DXVK's own answer would be to fabricate one",
    "IDXGIFactory2::CreateSwapChainForComposition":
        "creates a windowless composition swapchain.  DXVK serves it by "
        "fabricating a dummy window through its WSI backend "
        "(DxgiSurfaceFactory::CreateDummyWindow), and this lane's backend owns "
        "no windows -- it presents to windows the application asked Wine for. "
        " A composition swapchain would render correctly and be visible "
        "nowhere, which is worse than a refusal",
}

# void** out-parameters that are NOT untyped interface pointers but blocks of
# mapped memory, checked one by one against the headers.  Everything else with
# a bare void** is refused: the default has to be the safe one, because an
# interface pointer that crosses untyped gets no guest vtable at all.
VOID_PP_IS_MEMORY_DXVK = frozenset((
    "ID3D10Buffer::Map",        # void **ppData -- the mapped range
    "ID3D10Texture1D::Map",     # void **ppData -- the mapped range
))

# Flat-export refusals, documented here so --report names the whole surface in
# one place; enforced in dlls/d3d11/main.c and dlls/d3d11/d3d11.thunks.
FLAT_REFUSALS_DXVK = {
    "D3D11On12CreateDevice":
        "D3D11On12 needs a live ID3D12Device from the d3d12 lane, and the two "
        "lanes hold SEPARATE winecom instances (libs/winecom state is "
        "per-linkee) -- a d3d12 proxy handed to this module's runtime is not "
        "one of its proxies and would be refused one frame later, in the "
        "middle of a resource wrap.  Refused here, where the reason is legible",
}


# ==========================================================================
# The D3D9 surface -- a SECOND winecom instance, in dlls/d3d9.
#
# It shares this generator and nothing else.  D3D9 objects and D3D11 objects
# never meet: no D3D9 method takes a DXGI interface and no DXGI method takes a
# D3D9 one, so two runtimes with two proxy tables is not the hazard it would be
# between d3d11/dxgi/d3d10core (where one D3D11CreateDevice(adapter) call spans
# both and forces a single instance).  DXVK agrees at the library level --
# libdxvk_d3d9.so has NO DT_NEEDED on libdxvk_dxgi.so, which is the same
# statement in the linker's words.
#
# The refusals below are D3D9's own, and the shape of the surface is different
# enough from D3D11's to be worth naming: D3D9 has no DXGI, so presentation is
# on the device itself; and it carries floats by value in the middle of its
# hottest path, which D3D11 does only in three places.
# ==========================================================================

HAND_SLOTS_D3D9 = [
    # Presentation.  D3D9 has no DXGI: the window is an argument of
    # CreateDevice and a member of D3DPRESENT_PARAMETERS, the swapchain is
    # implicit in the device, and Reset re-creates it.  Same three duties as
    # the D3D11 side -- push the window's client size down before DXVK asks,
    # remember which window an object presents to, and run win32u's two hooks
    # around the present on the caller's own Wine thread.
    ("IDirect3D9::CreateDevice",                   "hand_d3d9_create_device"),
    ("IDirect3D9Ex::CreateDeviceEx",               "hand_d3d9_create_device_ex"),
    ("IDirect3DDevice9::CreateAdditionalSwapChain", "hand_d3d9_create_swapchain"),
    ("IDirect3DDevice9::Reset",                    "hand_d3d9_reset"),
    ("IDirect3DDevice9Ex::ResetEx",                "hand_d3d9_reset_ex"),
    ("IDirect3DDevice9::Present",                  "hand_d3d9_present"),
    ("IDirect3DDevice9Ex::PresentEx",              "hand_d3d9_present_ex"),
    ("IDirect3DSwapChain9::Present",               "hand_d3d9_swapchain_present"),

    # By-value floats.  The unixlib boundary calls with the widest INTEGER
    # form and cannot express them, exactly as on the D3D11 side -- but here
    # one of them is Clear, which every D3D9 title calls every frame, so
    # refusing it would refuse the whole API.
    #
    # Clear's `float z` is the FIFTH argument counting `this`, which MS-x64
    # puts on the STACK rather than in a register: past the four register
    # slots, a float is spilled as four bytes in an eight-byte stack slot.  So
    # it is read out of the trap CONTEXT's stack image and not out of an XMM
    # register, which is what makes it different from
    # ID3D11DeviceContext::ClearDepthStencilView (fourth argument, XMM3).
    ("IDirect3DDevice9::Clear",                    "hand_d3d9_clear"),
    ("IDirect3DDevice9::SetNPatchMode",            "hand_d3d9_set_npatch_mode"),
    ("IDirect3DDevice9::GetNPatchMode",            "hand_d3d9_get_npatch_mode"),
]

REFUSALS_D3D9 = {
    "IDirect3DSurface9::GetDC":
        "hands back an HDC over the surface's contents.  A GDI device context "
        "is a Wine object with no counterpart on DXVK's native side, and "
        "DXVK's own d3d9 answers this from its D3D9Surface GDI path -- which "
        "this lane does not reach.  Refused rather than returning a handle "
        "that names nothing",
    "IDirect3DSurface9::ReleaseDC":
        "takes by value the HDC IDirect3DSurface9::GetDC would have returned, "
        "which this lane refuses to produce in the first place",
}

VOID_PP_IS_MEMORY_D3D9 = frozenset((
    # Both are the mapped range of a buffer, checked against d3d9.h: the
    # `void **ppbData` that Lock writes is memory the application writes
    # vertices into, not an interface.  Same shape and same reasoning as
    # ID3D10Buffer::Map on the D3D11 surface.
    "IDirect3DVertexBuffer9::Lock",
    "IDirect3DIndexBuffer9::Lock",
))

FLAT_REFUSALS_D3D9 = {
    "Direct3DCreate9On12":
        "D3D9On12 needs a live ID3D12Device from the d3d12 lane, and the two "
        "lanes hold SEPARATE winecom instances (libs/winecom state is "
        "per-linkee) -- the same reason D3D11On12CreateDevice is refused on "
        "the D3D11 surface, one API down",
    "Direct3DCreate9On12Ex":
        "as Direct3DCreate9On12",
}


# The tables, by surface prefix.  --prefix selects one; anything else is a
# typo and stops generation rather than silently producing a surface with no
# hand-written slots and no refusals, which would look like a clean run.
SURFACES = {
    "d3d11": dict(hand=HAND_SLOTS_DXVK, refuse=REFUSALS_DXVK,
                  void_pp=VOID_PP_IS_MEMORY_DXVK, flat=FLAT_REFUSALS_DXVK),
    "d3d9":  dict(hand=HAND_SLOTS_D3D9, refuse=REFUSALS_D3D9,
                  void_pp=VOID_PP_IS_MEMORY_D3D9, flat=FLAT_REFUSALS_D3D9),
}


# --------------------------------------------------------------------------
# struct bodies -- which ones carry an interface pointer, transitively
# --------------------------------------------------------------------------

STRUCT_RE = re.compile(
    r'typedef\s+(?:struct|union)\s*(?:\w+\s*)?\{(.*?)\}\s*(\w+)\s*;',
    re.DOTALL)


def scan_structs(header_dir, iface_names, opaque_names):
    """-> (bearing, why, opaque, why_opaque).

    `bearing` is the set of struct type names that reach an INTERFACE pointer
    through any member chain; `opaque` the set that reach an HWND/HANDLE/HDC.
    Both matter and neither is visible in a signature: DXGI_SWAP_CHAIN_DESC is
    a plain data struct as far as the parameter list is concerned, and carries
    the `HWND OutputWindow` that IDXGIFactory::CreateSwapChain presents into.

    Deliberately transitive and deliberately crude about what a "member type"
    is -- every identifier in the body counts.  Over-approximating here costs
    a refusal; under-approximating hands DXVK a guest proxy pointer or a Wine
    window handle."""
    members = {}
    for fn in sorted(os.listdir(header_dir)):
        if not fn.endswith(".h"):
            continue
        with open(os.path.join(header_dir, fn), errors="replace") as fh:
            text = fh.read()
        for body, name in STRUCT_RE.findall(text):
            if name in members:
                continue
            members[name] = set(re.findall(r'\b(\w+)\s*\*', body)) | \
                set(re.findall(r'\b(\w+)\s+\w+\s*(?:\[|;|:)', body))

    def reach(targets):
        hit, why = {}, {}
        for name in members:
            # iterative reachability, so a cycle terminates
            seen, stack = set(), [(name, [])]
            while stack:
                cur, path = stack.pop()
                if cur in seen:
                    continue
                seen.add(cur)
                for m in members.get(cur, ()):
                    if m in targets:
                        hit[name] = True
                        why[name] = " -> ".join(path + [cur, m])
                        stack = []
                        break
                    if m in members:
                        stack.append((m, path + [cur]))
        return set(hit), why

    bearing, why = reach(iface_names)
    opaque, why_opaque = reach(opaque_names)
    return bearing, why, opaque - bearing, why_opaque


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
        # headers spell an INPUT array.  `Iface **p` with no inner const is an
        # out-parameter.  This distinction is the whole IN/OUT signal here.
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


COUNT_RE = re.compile(r'^(Num\w*|\w*[Cc]ount|adapter_idx|output_idx)$')


def find_count(params, idx):
    """Index of the by-value UINT count that governs the array at `idx`:
    nearest preceding, else nearest following.  None if there is none."""
    cands = [i for i, p in enumerate(params)
             if p.stars == 0 and p.base in ("UINT", "unsigned", "int", "DWORD")
             and COUNT_RE.match(p.name or '')]
    before = [i for i in cands if i < idx]
    after = [i for i in cands if i > idx]
    if before:
        return before[-1]
    if after:
        return after[0]
    return None


def ptr_count(params, idx):
    """True if some parameter is a UINT* whose name reads like the element
    count for the array at `idx` -- the XSGetShader(ppClassInstances,
    UINT *pNumClassInstances) shape, which has no winecom class."""
    return any(p.stars == 1 and p.base == "UINT" and
               re.match(r'^p?Num\w*$', p.name or '')
               for i, p in enumerate(params) if i != idx)


# --------------------------------------------------------------------------
# classification
# --------------------------------------------------------------------------

CA = dict(PASS=0, IFACE_IN=1, RIID=2, PPV_OUT=3, RET_PTR=4, EVENT=5,
          IFACE_ARR_IN=6, IFACE_OUT_STATIC=7, IFACE_ARR_OUT_STATIC=8)
CA_NAME = {v: "WINECOM_CA_" + k for k, v in CA.items()}


class Refused(Exception):
    pass


def classify(key, slot, ifaces, iface_index, byval_ok, bearing,
             why_bearing, opaque, why_opaque, void_pp_is_memory):
    """-> (cls[], xaux[], caux[], aux, aux2) or raise Refused(reason)."""
    params = [Param(p) for p in slot["params"]]
    n = len(params)
    cls = [CA["PASS"]] * n
    xaux = [0] * n
    caux = [0] * n
    aux = aux2 = 0

    joined = " | ".join(p.raw for p in params) + " | " + slot["ret"]
    if WCHAR_TOKENS.search(joined):
        raise Refused(
            "carries WCHAR: DXVK's native headers typedef WCHAR to wchar_t "
            "(4 bytes here), the guest PE's WCHAR is 2 -- a string crossing "
            "unconverted is silent, so this slot waits for the converting "
            "hand-written form")
    if FLOAT_TOKENS.search(slot["ret"]) and "*" not in slot["ret"]:
        raise Refused(
            "returns a float by value; the unixlib boundary calls with the "
            "widest INTEGER form, so the result would come back out of the "
            "wrong register file")

    for i, p in enumerate(params):
        if p.stars == 0:
            if FLOAT_TOKENS.search(p.raw):
                raise Refused(
                    "passes %s by value; the unixlib boundary calls with the "
                    "widest INTEGER form, so a float argument would be placed "
                    "in the wrong register file entirely" % p.base)
            if p.base in BYVAL_OPAQUE:
                raise Refused(BYVAL_OPAQUE[p.base])
            if p.base in BYVAL_AGGREGATE:
                cls[i] = CA["PASS"]
                continue
            if p.base not in byval_ok:
                raise Refused(
                    "by-value parameter `%s` is of a type this generator "
                    "cannot prove is integer-class on both ABIs; refusing "
                    "rather than assuming it is an enum" % p.raw)
            cls[i] = CA["PASS"]
            continue

        # ---- pointers
        if p.base == "void" and p.stars == 2:
            prev = params[i - 1] if i else None
            if prev is not None and prev.is_riid():
                cls[i] = CA["PPV_OUT"]
                cls[i - 1] = CA["RIID"]
                aux = i - 1
                continue
            if key in void_pp_is_memory:
                cls[i] = CA["PASS"]
                continue
            raise Refused(
                "has a void** out-parameter (`%s`) with no REFIID beside it "
                "to type the result; an untyped interface pointer cannot be "
                "given a guest vtable" % p.raw)

        if p.base in opaque:
            raise Refused(
                "takes %s, a struct that reaches a kernel or GDI handle "
                "through its own members (%s).  Those integers name Wine "
                "objects and DXVK's native side has its own encoding for the "
                "same things, so one namespace's integer handed to the other "
                "names a different object rather than none.  Window handles "
                "are NOT in this set any more -- there is one HWND namespace "
                "in the process and this lane presents through it"
                % (p.base, why_opaque.get(p.base, p.base)))
        if p.base in bearing:
            raise Refused(
                "takes %s, a struct that reaches an interface pointer "
                "through its own members (%s); the pointers inside it would "
                "arrive at DXVK as guest proxies.  Needs a hand-written "
                "walker, the shape dlls/d3d12/main.c's hand_resource_barrier "
                "has" % (p.base, why_bearing.get(p.base, p.base)))

        if p.base in ifaces:
            if p.stars == 1:
                cls[i] = CA["IFACE_IN"]
                continue
            if p.stars != 2:
                raise Refused(
                    "takes `%s`: an interface pointer at a level of "
                    "indirection this generator has no class for" % p.raw)
            if p.inner_const:
                c = find_count(params, i)
                if c is None:
                    raise Refused(
                        "takes the input interface array `%s` with no by-value "
                        "count parameter to bound it" % p.raw)
                cls[i] = CA["IFACE_ARR_IN"]
                aux2 = c
                continue
            # `Iface **` -- an out-parameter, single or array
            c = find_count(params, i)
            plural = (p.name or '').startswith("pp") and (p.name or '').endswith("s")
            if c is not None and plural:
                cls[i] = CA["IFACE_ARR_OUT_STATIC"]
                xaux[i] = iface_index[p.base]
                caux[i] = c
                continue
            if plural and ptr_count(params, i):
                raise Refused(
                    "writes the interface array `%s` whose element count "
                    "arrives through a UINT* rather than a by-value count; "
                    "winecom has no class for a count it must read back "
                    "through a pointer" % p.raw)
            if plural:
                raise Refused(
                    "writes the interface array `%s` with no count parameter "
                    "this generator can identify; refusing rather than "
                    "wrapping only its first element" % p.raw)
            cls[i] = CA["IFACE_OUT_STATIC"]
            xaux[i] = iface_index[p.base]
            continue

        # an ordinary pointer to plain data: crosses as an address.  Both
        # sides are the same process and the 296 descriptor structs are
        # member-offset-identical (dxvk-ppc64le/thunk/layout_check.cpp), so
        # there is nothing to repack.
        cls[i] = CA["PASS"]

    return cls, xaux, caux, aux, aux2


# --------------------------------------------------------------------------
# emission
# --------------------------------------------------------------------------

def c_guid(u):
    a, b, c, d, e = u.split("-")
    d4 = d + e
    return "{0x%s,0x%s,0x%s,{%s}}" % (
        a, b, c, ",".join("0x" + d4[i:i + 2] for i in range(0, 16, 2)))


def generate(roster, prefix, header_dir=HEADERS, surface=None,
             roster_name="interfaces_dxvk.json"):
    ifaces = roster["interfaces"]
    order = sorted(ifaces)
    iface_index = {n: i for i, n in enumerate(order)}
    byval_ok = set(BYVAL_INTEGER) | set(roster["integer_types"])
    bearing, why_bearing, opaque, why_opaque = scan_structs(
        header_dir, set(ifaces), set(BYVAL_OPAQUE))
    surface = surface or SURFACES["d3d11"]
    hand_index, hand_order = {}, []
    for key, fn in surface["hand"]:
        if fn not in hand_order:
            hand_order.append(fn)
        hand_index[key] = hand_order.index(fn)

    out = []
    w = out.append
    stats = dict(marshalled=0, refused=0, hand=0, identity=0, iunknown=0)
    refusal_log = []

    w("""/* GENERATED by ppc64le/dxvk/gen_winecom.py -- do not edit.
 *
 * Marshal tables for the %s surface (%d interfaces, %d vtable slots),
 * generated from ppc64le/dxvk/%s -- the same roster the matching
 * .thunks file hands spec2thunk to build the guest trap modules.
 * Interface order is sorted by name, which is the order spec2thunk
 * COM mode gives the guest stub arrays; libs/winecom cross-checks
 * every IID and slot count at attach, so the two generators cannot
 * silently disagree.
 *
 * Slot/iface types and WINECOM_CA_* classes come from
 * include/wine/winecom.h, which must be included before this file.
 */
""" % (roster["surface"], len(order),
       sum(len(ifaces[n]["slots"] or ()) for n in order), roster_name))

    w("enum %s_iface_index\n{" % prefix)
    for n in order:
        w("    %s_IFACE_%s = %d," % (prefix.upper(), n, iface_index[n]))
    w("    %s_IFACE_COUNT = %d\n};\n" % (prefix.upper(), len(order)))
    w("#define %s_HAND_COUNT %d\n" % (prefix.upper(), len(hand_order)))
    w("/* hand_funcs[] order in dlls/%s/main.c:\n%s */\n"
      % (prefix, "".join("     *   %d %s\n" % (i, f) for i, f in enumerate(hand_order))))


    tables = []
    for n in order:
        slots = ifaces[n]["slots"]
        if slots is None:
            sys.exit("gen_winecom: %s has no resolved vtable -- its base is "
                     "outside the parsed header set, so its slot numbers are "
                     "unknown and it must not get a table" % n)
        rows, decls = [], []
        interesting = False
        for s in slots:
            key = "%s::%s" % (s["owner"], s["name"])
            label = key
            if s["slot"] < 3:
                rows.append('    { "%s", NULL, NULL, NULL, %d, 0, 0, 0, NULL },'
                            '  /* runtime */' % (label, 1 if s["slot"] else 3))
                stats["iunknown"] += 1
                continue
            argc = 1 + len(s["params"])
            flags = []
            if s["ret"] == "void":
                flags.append("WINECOM_F_RET_VOID")
            if key in hand_index:
                rows.append('    { "%s", NULL, NULL, NULL, %d, WINECOM_F_HAND%s,'
                            ' %d, 0, NULL },'
                            % (label, argc,
                               "".join("|" + f for f in flags),
                               hand_index[key]))
                stats["hand"] += 1
                interesting = True
                continue
            reason = surface["refuse"].get(key)
            if reason is None:
                try:
                    cls, xaux, caux, aux, aux2 = classify(
                        key, s, ifaces, iface_index, byval_ok, bearing,
                        why_bearing, opaque, why_opaque, surface["void_pp"])
                except Refused as e:
                    reason = str(e)
            if reason is not None:
                rows.append('    { "%s",\n      "%s::%s: %s",\n'
                            '      NULL, NULL, %d, 0, 0, 0, NULL },'
                            % (label, s["owner"], s["name"],
                               reason.replace('"', "'"), argc))
                stats["refused"] += 1
                refusal_log.append((n, s["slot"], key, reason))
                interesting = True
                continue
            cname = xname = kname = "NULL"
            if any(c != CA["PASS"] for c in cls):
                cname = "cls_%s_%d" % (n, s["slot"])
                decls.append("static const unsigned char %s[] = { %s };"
                             % (cname, ", ".join(CA_NAME[c] for c in cls)))
                interesting = True
            if any(xaux):
                xname = "xaux_%s_%d" % (n, s["slot"])
                decls.append("static const unsigned char %s[] = { %s };"
                             % (xname, ", ".join(str(x) for x in xaux)))
            if any(caux):
                kname = "caux_%s_%d" % (n, s["slot"])
                decls.append("static const unsigned char %s[] = { %s };"
                             % (kname, ", ".join(str(x) for x in caux)))
            rows.append('    { "%s", NULL, %s, %s, %d, %s, %d, %d, %s },'
                        % (label, cname, xname, argc,
                           "|".join(flags) or "0", aux, aux2, kname))
            stats["marshalled"] += 1

        if not interesting:
            # An identity row: IUnknown's three slots are served by the
            # runtime from the proxy table and everything else is refused
            # loudly.  Emitting a table of nothing but CA_PASS rows would
            # claim more than we checked.
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
        w('    { "%s", %s,\n      %d, %s },'
          % (n, c_guid(ifaces[n]["uuid"]), len(ifaces[n]["slots"]),
             "NULL" if t is None else "slots_" + n))
    w("};")

    w("\n/* %d slot(s) marshalled, %d hand-written, %d refused with a named\n"
      " * reason, %d IUnknown slot(s) served by the runtime; %d interface(s)\n"
      " * carry identity rows only. */"
      % (stats["marshalled"], stats["hand"], stats["refused"],
         stats["iunknown"], stats["identity"]))
    return "\n".join(out) + "\n", stats, refusal_log


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--roster",
                    default=os.path.join(HERE, "interfaces_dxvk.json"))
    # The struct bodies are read from DXVK's vendored headers, which live in
    # the gitignored bootstrap checkout.  Overridable so the generator can be
    # run against a copy of them on a machine that has no src/ -- the roster
    # JSON is versioned, the headers are not.
    ap.add_argument("--headers", default=HEADERS)
    ap.add_argument("--prefix", default="d3d11")
    ap.add_argument("--out")
    ap.add_argument("--check", metavar="FILE")
    ap.add_argument("--report", action="store_true")
    args = ap.parse_args()

    with open(args.roster) as fh:
        roster = json.load(fh)
    surface = SURFACES.get(args.prefix)
    if surface is None:
        sys.exit("gen_winecom: no hand-written slots or refusals are declared "
                 "for surface prefix %r.  Generating one anyway would emit a "
                 "surface with nothing hand-written and nothing refused, which "
                 "reads exactly like a clean run; known prefixes are %s."
                 % (args.prefix, ", ".join(sorted(SURFACES))))
    text, stats, refusals = generate(roster, args.prefix, args.headers, surface,
                                     os.path.basename(args.roster))

    print("surface %s: %d marshalled, %d hand-written, %d refused, "
          "%d IUnknown, %d identity-only interface(s)"
          % (roster["surface"], stats["marshalled"], stats["hand"],
             stats["refused"], stats["iunknown"], stats["identity"]))

    if args.report:
        print("\nrefused slots, by reason:")
        seen = {}
        for n, slot, key, reason in refusals:
            seen.setdefault(reason.split(';')[0][:70], []).append(key)
        for reason, keys in sorted(seen.items(), key=lambda kv: -len(kv[1])):
            uniq = sorted(set(keys))
            print("  %4d  %s" % (len(keys), reason))
            for k in uniq[:8]:
                print("          %s" % k)
            if len(uniq) > 8:
                print("          ... and %d more distinct method(s)"
                      % (len(uniq) - 8))
        print("\nflat exports refused in dlls/%s/main.c:" % prefix)
        for k, v in sorted(surface["flat"].items()):
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
