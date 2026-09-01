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
  * by-value floats in parameter positions past the eighth, where the fp
    masks cannot name them.  Positions 1..8 and float returns are SERVED
    since PPC64EC step C through the surface's floating-point invoker
    (unix_vtbl_call_fp over wine/winecom_fpcall.h), driven by the row's
    fpmask/fpwide/fpret; the hand-written float slots stay hand-written.
    The 32-BIT lane has not adopted the invoker, so served fp rows carry
    refuse32.
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


# Sub-word by-value integer types.  Not used to DESCRIBE anything on this
# surface -- it has none -- but to RECOGNISE one if a roster entry ever adds
# it, so the refusal below can be by name.  Kept spelled the same as
# ppc64le/mf/gen_winecom.py's copy so the two can be compared at a glance.
NARROW_BYVAL = {
    "WORD":    (2, False), "USHORT": (2, False), "UINT16":  (2, False),
    "SHORT":   (2, True),  "INT16":  (2, True),
    "BYTE":    (1, False), "UCHAR":  (1, False), "UINT8":   (1, False),
    "BOOLEAN": (1, False),
    "CHAR":    (1, True),  "INT8":   (1, True),
}

# The same widths spelled as plain C, matched against the whole declaration
# because Param.base keeps only the FIRST token.
NARROW_RAW = (
    (r'\bunsigned\s+short\b',  (2, False)),
    (r'\bsigned\s+short\b',    (2, True)),
    (r'\bunsigned\s+char\b',   (1, False)),
    (r'\bsigned\s+char\b',     (1, True)),
    (r'\bshort\b',              (2, True)),
    (r'\bchar\b',               (1, True)),
)


def narrow_of(p):
    """(bytes, signed) for a by-value parameter narrower than 32 bits, else
    None.  Ported from ppc64le/mf/gen_winecom.py exactly as the old refusal
    text promised, the day the i386 lane needed every width published."""
    if p.base in NARROW_BYVAL:
        return NARROW_BYVAL[p.base]
    for pat, w in NARROW_RAW:
        if re.search(pat, p.raw):
            return w
    return None


# --------------------------------------------------------------------------
# the i386 width oracle -- what the OTHER guest's ABI says each by-value
# type measures.
#
# The 64-bit masks above answer "how wide is this argument on the x86-64
# guest", and they can afford to be name lists because every name was checked
# against that one ABI.  The i386 guest gets no such shortcut: the 8-byte
# class SPLITS over there.  HANDLE, HWND, SIZE_T, ULONG_PTR and every pointer
# shrink to 4 bytes -- ONE stdcall stack slot -- while UINT64 and
# D3D12_GPU_VIRTUAL_ADDRESS stay 8 bytes and TWO, so ID3D11Fence::Signal's
# value sits at a different stack offset than any pointer-taking neighbour,
# and a lane that guessed from the 64-bit tables would read half fence-value,
# half whatever came next: plausible, wrong, silent.  A name list is exactly
# how those two widths ended up in one bucket, so the widths are asked of
# clang itself, per declared type, for BOTH guest targets -- the
# ppc64le/dxvk/layout32.py mechanism: compile a sizeof array with -S, read
# the .long values back out of the assembly, never execute anything.  The
# x86-64 answers double as a lie detector for the name lists above: any
# disagreement stops generation, because it would mean the committed 64-bit
# tables were already wrong.
#
# The oracle publishes into `struct winecom_slot::qwordmask` (bit i: that
# parameter is TWO 4-byte slots) and WINECOM_F_RET_QWORD (the return is
# EDX:EAX, not EAX), both guarded by WINECOM_F_I386_GEOM -- the xmask rule: a
# table that predates the field reads as "no geometry", never as "all
# narrow".  And the emitted geometry is CHECKED against a second opinion
# before it may be written: every covered slot is re-declared
# __attribute__((stdcall)) verbatim from the roster and clang's own @N symbol
# decoration -- the callee-popped byte count, computed by clang laying out
# the whole frame -- must equal what argc and qwordmask predict.
# --------------------------------------------------------------------------

I386_TARGET = "i386-windows-gnu"
AMD64_TARGET = "x86_64-windows-gnu"

# Tokens that can end a multi-word builtin type, so the name stripper below
# knows "unsigned int" carries no parameter name while "UINT64 value" does.
C_TYPE_TAIL = frozenset(
    "int char short long unsigned signed float double".split())


def byval_spelling(raw):
    """The C type expression of a star-free, bracket-free declaration: the
    declared text with qualifiers and the parameter NAME stripped.  The
    result is handed to clang, which ERRORS on anything this got wrong
    rather than mis-measuring it -- the stripper cannot silently lie."""
    t = re.sub(r'\b(const|volatile)\b', ' ', " ".join(raw.split()))
    toks = t.split()
    if len(toks) > 1 and toks[-1] not in C_TYPE_TAIL:
        toks = toks[:-1]
    return " ".join(toks)


class I386Oracle:
    """sizeof/signedness/floating-point-ness per type spelling, per guest
    target, from clang.

    `sign_names`: single-token spellings signedness may be asked of (the
    integer-class roster); a struct cannot be cast from -1, so everything
    else gets a constant 0 in the sign column and nobody reads it.  The
    FP column has no such gate: it is a _Generic over an unevaluated
    dereference, which compiles for any object type and answers 1 only for
    float/double/long double -- the types i386 returns in x87 ST(0) rather
    than EAX, which is why slot_geometry refuses to publish a return
    register class for them.

    `fallback_prelude`: a SECOND header world for names the surface's own
    headers never declare.  The d3d12 roster's DXGI rows carry enums
    (DXGI_MEMORY_SEGMENT_GROUP and friends) that exist in Wine's dxgi
    headers but in no vkd3d header; the two worlds cannot share one
    translation unit (both define the interfaces), so a name the first TU
    reports UNDECLARED is retried in the second, and a name neither world
    declares stops generation.  Nothing is ever guessed on the way."""

    def __init__(self, clang, prelude, incdirs, sign_names, tag,
                 fallback_prelude="", fallback_incdirs=None):
        self.clang = clang
        self.prelude = prelude
        self.fallback_prelude = fallback_prelude
        self.incdirs = [d for d in incdirs if d]
        self.fallback_incdirs = ([d for d in fallback_incdirs if d]
                                 if fallback_incdirs else self.incdirs)
        self.sign_names = set(sign_names)
        self.tag = tag
        self.cache = {}          # spelling -> (w32, w64, signed, fp)
        self.fallback_ids = set()   # identifiers only the fallback declares

    def _sign_ok(self, s):
        toks = s.split()
        return (all(t in C_TYPE_TAIL for t in toks)
                or (len(toks) == 1 and toks[0] in self.sign_names))

    UNDECLARED = re.compile(
        r"(?:use of undeclared identifier|unknown type name)\s+'(\w+)'")

    def _run(self, src_text, target, what, fatal=True, incdirs=None):
        """-> assembly text, or (None, stderr) with fatal=False."""
        import subprocess
        import tempfile
        with tempfile.TemporaryDirectory() as td:
            c = os.path.join(td, "winecom_%s.c" % what)
            with open(c, "w") as fh:
                fh.write(src_text)
            asm = c[:-2] + ".s"
            cmd = ([self.clang, "-target", target, "-O0", "-S", "-o", asm,
                    "-nostdlibinc"]
                   + ["-I" + d for d in (incdirs or self.incdirs)] + [c])
            r = subprocess.run(cmd, capture_output=True, text=True)
            if r.returncode:
                if not fatal:
                    return None, r.stderr
                sys.exit("gen_winecom: the %s %s probe does not compile for "
                         "%s, and a width that cannot be measured must not "
                         "be guessed.\ncommand: %s\n%s"
                         % (self.tag, what, target, " ".join(cmd),
                            r.stderr[:4000]))
            with open(asm) as fh:
                return fh.read(), None

    def _width_src(self, spellings, prelude):
        src = prelude + "const unsigned int winecom_probe[] = {\n"
        for s in spellings:
            sign = ("(unsigned int)((%s)-1 < (%s)0)" % (s, s)
                    if self._sign_ok(s) else "0u")
            # The dereference is UNEVALUATED (_Generic reads only the type),
            # so this compiles for any object type -- structs included --
            # and cannot be fooled by a typedef the way a name list can.
            fp = ("(unsigned int)_Generic(*(%s *)0, float: 1, double: 1, "
                  "long double: 1, default: 0)" % s)
            src += "    (unsigned int)sizeof(%s), %s, %s,\n" % (s, sign, fp)
        src += "};\n"
        return src

    def _measure_batch(self, todo, prelude, incdirs=None):
        vals = {}
        for target in (I386_TARGET, AMD64_TARGET):
            text, _ = self._run(self._width_src(todo, prelude), target,
                                "width", incdirs=incdirs)
            m = re.search(r'^_?winecom_probe:\n((?:\s+\.long\s+\d+[^\n]*\n)+)',
                          text, re.M)
            got = ([int(x) for x in re.findall(r'\.long\s+(\d+)', m.group(1))]
                   if m else [])
            if len(got) != 3 * len(todo):
                sys.exit("gen_winecom: read %d width-probe values for %d "
                         "types from the %s %s assembly; the compiler did "
                         "not answer what the source asked"
                         % (len(got), len(todo), self.tag, target))
            vals[target] = got
        for i, s in enumerate(todo):
            s32, g32, f32 = vals[I386_TARGET][3 * i:3 * i + 3]
            s64, g64, f64 = vals[AMD64_TARGET][3 * i:3 * i + 3]
            if g32 != g64:
                sys.exit("gen_winecom: %s is signed on one guest and "
                         "unsigned on the other; no marshal class expresses "
                         "that and this generator will not pick a side" % s)
            if f32 != f64:
                sys.exit("gen_winecom: %s is floating-point on one guest "
                         "and not the other, which cannot happen for a type "
                         "both targets parse; the probe is broken, not the "
                         "type" % s)
            self.cache[s] = (s32, s64, bool(g32), bool(f32))

    def measure(self, spellings):
        todo = sorted(set(spellings) - set(self.cache))
        if not todo:
            return
        # Partition: names the surface headers do not declare move to the
        # fallback world.  The trial compile is i386-only and non-fatal; the
        # real measurements below are fatal, so nothing unmeasured survives.
        main_set = list(todo)
        fb_set = []
        for _ in range(8):
            _, err = self._run(self._width_src(main_set, self.prelude),
                               I386_TARGET, "width", fatal=False)
            if err is None:
                break
            missing = set(self.UNDECLARED.findall(err))
            moved = [s for s in main_set
                     if missing & set(re.findall(r'\w+', s))]
            if not missing or not moved or not self.fallback_prelude:
                sys.exit("gen_winecom: the %s width probe does not compile "
                         "and no fallback header world declares what is "
                         "missing:\n%s" % (self.tag, err[:4000]))
            self.fallback_ids |= missing
            fb_set += moved
            main_set = [s for s in main_set if s not in moved]
        if main_set:
            self._measure_batch(main_set, self.prelude)
        if fb_set:
            self._measure_batch(sorted(fb_set), self.fallback_prelude,
                                self.fallback_incdirs)

    def width32(self, s):
        return self.cache[s][0]

    def width64(self, s):
        return self.cache[s][1]

    def is_signed(self, s):
        return self.cache[s][2]

    def is_fp(self, s):
        return self.cache[s][3]

    def check_stdcall_frames(self, entries, alias_map):
        """entries: (key, params, argc, qwordmask) for every slot whose
        geometry will be published.  Declares each one stdcall, verbatim
        from the roster, and requires clang's @N callee-pop decoration to
        equal 4*argc + 4*popcount(qwordmask).  Any disagreement stops
        generation: two independent derivations of the same frame may not
        differ and both still be trusted."""
        if not entries:
            return 0

        def decl_lines(batch, prelude):
            lines = [prelude]
            for n, (key, params, argc, qm) in enumerate(batch):
                ptext = ""
                for praw in params:
                    praw = " ".join(praw.split())
                    for old, new in alias_map.items():
                        praw = re.sub(r'\b%s\b' % re.escape(old), new, praw)
                    ptext += ", " + praw
                lines.append("void __attribute__((stdcall)) winecom_chk_%d"
                             "(void *wcthis%s);" % (n, ptext))
            lines.append("const void *winecom_chk_refs[] = {")
            lines += ["    (const void *)&winecom_chk_%d," % n
                      for n in range(len(batch))]
            lines.append("};")
            return "\n".join(lines) + "\n"

        # Slots spelled with names only the fallback world declares are
        # checked THERE -- same ABI, different header copy.  The partition
        # is discovered the same way measure() discovers it: clang says
        # which identifiers the main world lacks, and the entries naming
        # them move over; nothing unmoved is left unchecked.
        main_e = list(entries)
        fb_e = [e for e in main_e
                if set(re.findall(r'\w+', " ".join(e[1])))
                & self.fallback_ids]
        main_e = [e for e in main_e if e not in fb_e]
        text = None
        for _ in range(8):
            if not main_e:
                text = ""
                break
            text, err = self._run(decl_lines(main_e, self.prelude),
                                  I386_TARGET, "frame", fatal=False)
            if err is None:
                break
            missing = set(self.UNDECLARED.findall(err))
            moved = [e for e in main_e
                     if missing & set(re.findall(r'\w+', " ".join(e[1])))]
            if not missing or not moved or not self.fallback_prelude:
                sys.exit("gen_winecom: the %s frame probe does not compile "
                         "and no fallback header world declares what is "
                         "missing:\n%s" % (self.tag, err[:4000]))
            fb_e += moved
            main_e = [e for e in main_e if e not in moved]
        found = {}
        for batch, text_ready, prelude, what, incs in (
                (main_e, text, self.prelude, "frame", None),
                (fb_e, None, self.fallback_prelude, "frame_fb",
                 self.fallback_incdirs)):
            if not batch:
                continue
            if text_ready is None:
                text_ready, _ = self._run(decl_lines(batch, prelude),
                                          I386_TARGET, what, incdirs=incs)
            got = {int(a): int(b)
                   for a, b in re.findall(r'winecom_chk_(\d+)@(\d+)',
                                          text_ready)}
            for n, e in enumerate(batch):
                found[id(e)] = got.get(n)
        bad = []
        for e in entries:
            key, params, argc, qm = e
            want = 4 * argc + 4 * bin(qm).count("1")
            got = found.get(id(e))
            if got != want:
                bad.append("  %s: table geometry says the callee pops %d "
                           "bytes, clang's stdcall decoration says %s"
                           % (key, want, got))
        if bad:
            sys.exit("gen_winecom: the emitted i386 geometry DISAGREES with "
                     "clang's own frame layout for %d slot(s); refusing to "
                     "write a table either half of the toolchain would "
                     "contradict:\n%s" % (len(bad), "\n".join(bad)))
        return len(entries)


def slot_geometry(slot, oracle):
    """-> (qwordmask, ret_qword, unproven_reason).  The i386 stdcall frame of
    one slot, independent of its marshal classification: EVERY slot needs
    geometry -- a refused slot still has to pop the right number of bytes
    before it can so much as answer E_NOTIMPL.  An 8-byte parameter past bit
    15 of qwordmask stops generation outright: the alternative is a silently
    truncated mask, the exact bug class this field exists to prevent."""
    qm = 0
    unproven = None
    for i, praw in enumerate(slot["params"]):
        p = Param(praw)
        if p.stars:              # pointers and decayed arrays: one 4-byte slot
            continue
        w = oracle.width32(byval_spelling(p.raw))
        if w == 8:
            if i >= 16:
                sys.exit("gen_winecom: %s::%s has an 8-byte by-value "
                         "parameter in argument position %d, past the "
                         "sixteen bits qwordmask covers.  Widen the mask; "
                         "truncating it silently is not an option."
                         % (slot["owner"], slot["name"], i + 1))
            qm |= 1 << i
        elif w not in (1, 2, 4):
            unproven = ("by-value %s measures %d bytes on the i386 guest, "
                        "which qwordmask cannot spell (one or two 4-byte "
                        "slots)" % (byval_spelling(p.raw), w))
    ret_q = False
    rraw = slot["ret"]
    if rraw != "void" and not slot.get("aggregate_return"):
        rp = Param(rraw)
        if rp.stars == 0:
            rsp = byval_spelling(rraw)
            if oracle.is_fp(rsp):
                # i386 returns float and double in x87 ST(0).  The contract
                # these fields publish can spell EAX and EDX:EAX and nothing
                # else, so an FP return gets NO geometry and a 32-bit lane
                # fails closed on it -- the alternative is a consumer that
                # reads garbage out of EAX and leaves ST(0) unpopped, NaN
                # after eight calls (GetResourceMinLOD / GetNPatchMode are
                # the live cases on these rosters).  Asked of clang (the
                # _Generic probe), not of a token list: a typedef spelling
                # of float must not slip through as "EAX".
                unproven = ("the %s return value is floating-point, which "
                            "i386 returns in x87 ST(0); neither EAX nor "
                            "EDX:EAX describes it, so no geometry is "
                            "published and a 32-bit lane must fail closed"
                            % rsp)
            else:
                rw = oracle.width32(rsp)
                if rw == 8:
                    ret_q = True
                elif rw not in (1, 2, 4):
                    unproven = ("the %s return value measures %d bytes on "
                                "the i386 guest, which is neither EAX nor "
                                "EDX:EAX" % (rsp, rw))
    return qm, ret_q, unproven


def collect_spellings(ifaces):
    """Every by-value parameter and by-value return spelling in the roster,
    so the oracle can measure them in one batch."""
    sp = set()
    for iface in ifaces.values():
        for s in iface["slots"] or ():
            for praw in s["params"]:
                p = Param(praw)
                if not p.stars:
                    sp.add(byval_spelling(p.raw))
            rraw = s["ret"]
            if rraw != "void" and not s.get("aggregate_return"):
                if Param(rraw).stars == 0:
                    sp.add(byval_spelling(rraw))
    return sp





# --------------------------------------------------------------------------
# the i386 struct audit -- WINECOM_F_I386_STRUCTS_OK and the reps tables
# --------------------------------------------------------------------------
# The geometry above answers how the i386 FRAME is decoded; this answers what
# the frame's POINTERS point at.  An i386 guest lays pointer members out in
# four bytes and its structs move fields; the repack roster
# (gen_repack32.py --json) records, for every aggregate on the surface,
# whether the two layouts agree.  Every CA_PASS/CA_RET_PTR pointer parameter
# of a served row must be accounted for -- identical, repacked (a reps[]
# entry naming the generated repack pair), or the row refuses on the 32-bit
# lane with a named reason -- before the row may carry
# WINECOM_F_I386_STRUCTS_OK.  A row without the flag is refused by the
# 32-bit dispatcher wholesale, so nothing unaudited can pass a divergent
# struct through raw.

# Pointee types that lay out identically on both guests without appearing in
# the repack roster: scalars, GUIDs, and the plain-data Win32 aggregates the
# roster's scan regex does not cover.  Pointer-width scalars (SIZE_T and
# friends) are deliberately ABSENT: a SIZE_T out-parameter is a 4-byte cell
# on i386 and would need a widening rep of its own.
SAFE_POINTEE = frozenset(
    """void char BYTE UINT8 UCHAR USHORT UINT16 SHORT INT16 UINT INT
       DWORD ULONG LONG BOOL WINBOOL FLOAT float GUID IID UINT64 INT64
       ULONGLONG LONGLONG DOUBLE double LARGE_INTEGER RECT POINT SIZE
       LUID SECURITY_ATTRIBUTES D3D11_RECT D3D10_RECT
       PALETTEENTRY""".split())

# Divergent-struct parameters that are ARRAYS, with the by-value parameter
# holding the element count.  Keyed by (Interface::Method, parameter name);
# per-method API knowledge, the same honesty rule as HAND_SLOTS.
STRUCT_ARRAYS = {
    ("ID3D11Device::CreateInputLayout", "pInputElementDescs"): "NumElements",
    ("ID3D11Device::CreateGeometryShaderWithStreamOutput", "pSODeclaration"): "NumEntries",
    ("ID3D10Device::CreateInputLayout", "pInputElementDescs"): "NumElements",
    ("ID3D10Device::CreateGeometryShaderWithStreamOutput", "pSODeclaration"): "NumEntries",
}

# Divergent-struct array parameters whose element count is NOT a by-value
# parameter (CreateTexture*'s initial data is MipLevels x ArraySize out of
# the desc): no mechanical rep can serve them, so the ROW refuses on the
# 32-bit lane until a hand32 walker takes it over -- which dlls/d3d11's
# hand32 table does for the texture creates.
STRUCT_SPECIAL = {
    ("ID3D11Device::CreateTexture1D", "pInitialData"),
    ("ID3D11Device::CreateTexture2D", "pInitialData"),
    ("ID3D11Device::CreateTexture3D", "pInitialData"),
    ("ID3D10Device::CreateTexture1D", "pInitialData"),
    ("ID3D10Device::CreateTexture2D", "pInitialData"),
    ("ID3D10Device::CreateTexture3D", "pInitialData"),
}

# Divergent-struct pointer parameters whose name LOOKS plural but which the
# API defines as a single struct -- the plural-name tripwire below demands
# each one be confirmed here rather than silently defaulted to scalar.
CONFIRMED_SCALAR = {
    ("IDXGISwapChain1::Present1", "pPresentParameters"),

    # D3D9.  Plural by spelling, singular by API: each of these writes ONE
    # record.  Read out of d3d9.h's own declarations rather than inferred
    # from the name.  Note what is deliberately NOT here --
    # IDirect3D9::CreateDevice's pPresentationParameters really IS an array,
    # one element per adapter in a multi-head group, and it needs no ruling
    # because that row is hand-written on both lanes.
    ("IDirect3DDevice9::GetCreationParameters", "pParameters"),
    ("IDirect3DDevice9Ex::GetCreationParameters", "pParameters"),
    ("IDirect3DSwapChain9::GetPresentParameters", "pPresentationParameters"),
    ("IDirect3DSwapChain9Ex::GetPresentParameters", "pPresentationParameters"),
    ("IDirect3DSwapChain9Ex::GetPresentStats", "stats"),
}

# Pointee types that are POINTER-WIDTH SCALARS: four guest bytes, eight
# native ones.  These are not aggregates, so no layout roster measures them
# and no generated struct repack exists -- but the divergence is real and it
# is the same one, so they get the fixed widening rep gen_repack32.py always
# emits (wine_repack32_PTRWIDTH / wine_repack64_PTRWIDTH).  Read that
# function's banner for what the OUT direction does when a value will not
# fit, and why it reports none rather than a truncated one.
#
# HANDLE is what forced this: `HANDLE *pSharedHandle` closes the parameter
# list of every D3D9 resource creator, nineteen rows, so without a rep for it
# a 32-bit D3D9 title cannot allocate a texture, a vertex buffer, an index
# buffer or a render target.  [MEASURED 2026-08-30, gen_winecom --report on
# the d3d9 surface: 19 of 33 i386-only refusals were this one type.]
#
# SIGNED pointer-width scalars are deliberately absent, for the reason
# gen_repack32.py's SIGNED_PTR_TYPES gives: the widening rep zero-extends,
# and a sign-extending one is a different function that nothing needs yet.
PTRWIDTH_REP = "@PTRWIDTH"
PTRWIDTH_POINTEE = frozenset(
    """HANDLE HGLOBAL HLOCAL HMODULE HINSTANCE
       SIZE_T ULONG_PTR UINT_PTR DWORD_PTR""".split())

_audit_tripwire = []


def audit_i386(key, s, cls, layouts, byval_ok):
    """-> (reps, refuse32).  reps: [(param_idx, count_idx_or_None, dir,
    typename)] for the divergent pointees; refuse32: a reason string when
    the row cannot be served on the 32-bit lane; (None, None) when no audit
    roster exists at all (the row then carries no STRUCTS_OK and fails
    closed at dispatch)."""
    if layouts is None:
        return None, None
    params = [Param(p) for p in s["params"]]
    reps = []
    for i, p in enumerate(params):
        if p.stars == 0:
            continue
        if cls[i] not in (CA["PASS"], CA["RET_PTR"]):
            continue        # interface/riid/event machinery: width-safe
        base = p.base
        # A DOUBLE pointer that reached here is not an interface -- classify()
        # already took IFACE_IN, PPV_OUT and RIID out of the running -- so it
        # is a cell the host reads or writes a POINTER through.  That cell is
        # eight bytes native and FOUR in the guest, so the native call reads
        # four bytes of neighbouring guest memory, or writes four bytes over
        # it, and the guest then dereferences half an address.  No amount of
        # struct auditing sees this, because the pointee's own layout is fine.
        #
        # [MEASURED 2026-08-30] The hole was live: `void **ppbData`, the
        # answer to IDirect3DVertexBuffer9::Lock and IDirect3DIndexBuffer9::
        # Lock, is CA_PASS by way of VOID_PP_IS_MEMORY, and `void` is in
        # SAFE_POINTEE -- so both rows published I386_STRUCTS_OK and would
        # have let DXVK write eight bytes into a four-byte guest cell on the
        # first vertex buffer any 32-bit D3D9 title locked.  ID3D10Buffer::Map
        # has the same shape one API up.  Refusing by name is the honest
        # answer until a hand32 walker hands back a BELOW-4-GIB address, which
        # is the same bounce D3DLOCKED_RECT needs and the same one
        # dlls/d3d11/main.c already built for ID3D11DeviceContext::Map.
        if p.stars > 1:
            return None, ("parameter `%s` is a cell the host fills with a "
                          "POINTER, and a 32-bit guest's cell is four bytes "
                          "wide -- the native side would read or write eight. "
                          "A hand32 walker with a below-4GiB answer must "
                          "serve this row" % p.raw)
        if base in SAFE_POINTEE or base in byval_ok:
            continue        # scalar or enum pointee: identical bytes
        if base in layouts["identical"]:
            continue
        if base in PTRWIDTH_POINTEE:
            if p.stars > 1:
                return None, ("parameter `%s` is a pointer to pointers of the "
                              "pointer-width scalar %s; no rep describes that "
                              "shape" % (p.raw, base))
            db = 1 if "const" in p.raw.split() else 2
            if cls[i] == CA["RET_PTR"]:
                db = 2
            reps.append((i, None, db, PTRWIDTH_REP))
            continue
        if base not in layouts["divergent"]:
            return None, ("parameter `%s` points at %s, which the i386 "
                          "layout roster never audited" % (p.raw, base))
        # (the old "pointer to pointers of the divergent X" refusal lived
        # here; every stars > 1 parameter is now refused further up, for a
        # reason that does not depend on the pointee diverging at all)
        mkey = (key, p.name)
        if mkey in STRUCT_SPECIAL:
            return None, ("`%s` is an array of the divergent %s whose "
                          "element count is not a by-value parameter; a "
                          "hand32 walker must serve this row" % (p.name, base))
        count_idx = None
        if mkey in STRUCT_ARRAYS:
            cname = STRUCT_ARRAYS[mkey]
            count_idx = next((j for j, q in enumerate(params)
                              if q.stars == 0 and q.name == cname), None)
            if count_idx is None:
                sys.exit("gen_winecom: STRUCT_ARRAYS names count parameter "
                         "%r for %s, which the roster's %s does not have"
                         % (cname, mkey[1], key))
        elif p.name.endswith("s") and mkey not in CONFIRMED_SCALAR:
            _audit_tripwire.append((key, p.raw, base))
            continue
        dir_bits = 1 if "const" in p.raw.split() else 2
        if cls[i] == CA["RET_PTR"]:
            dir_bits = 2
        if (dir_bits & 2) and layouts["divergent"][base].get("out_unsafe"):
            return None, ("writes %s through `%s`, and that struct carries a "
                          "pointer field a 4-byte guest cell cannot hold -- "
                          "repacking OUT would silently truncate it (a hand32 "
                          "walker with a below-4GiB answer must serve this "
                          "row)" % (base, p.raw))
        reps.append((i, count_idx, dir_bits, base))
    return reps, None


def classify(key, slot, ifaces, iface_index, byval_ok, bearing,
             why_bearing, opaque, why_opaque, void_pp_is_memory, oracle):
    """-> (cls[], xaux[], caux[], aux, aux2, masks) or raise
    Refused(reason).  `masks` is (narrowmask, narrowwide, narrowsign,
    dwordmask, dwordsign) -- the widths libs/winecom extends by, ported from
    the mf/vkd3d siblings the day the width had to be published rather than
    merely recognised."""
    params = [Param(p) for p in slot["params"]]
    n = len(params)
    cls = [CA["PASS"]] * n
    xaux = [0] * n
    caux = [0] * n
    aux = aux2 = 0
    narrowmask = narrowwide = narrowsign = 0
    dwordmask = dwordsign = 0

    joined = " | ".join(p.raw for p in params) + " | " + slot["ret"]
    if WCHAR_TOKENS.search(joined):
        raise Refused(
            "carries WCHAR: DXVK's native headers typedef WCHAR to wchar_t "
            "(4 bytes here), the guest PE's WCHAR is 2 -- a string crossing "
            "unconverted is silent, so this slot waits for the converting "
            "hand-written form")
    fpmask = fpwide = fpret = 0
    if FLOAT_TOKENS.search(slot["ret"]) and "*" not in slot["ret"]:
        # PPC64EC step C: served, not refused.  The surface's floating-point
        # invoker (unix_vtbl_call_fp -> wine/winecom_fpcall.h) returns f1's
        # bits beside the integer result; fpret says the width, the flat
        # lane's THUNK_FP_RET encoding.
        fpret = 1 if Param(slot["ret"]).base in ("double", "DOUBLE") else 2

    for i, p in enumerate(params):
        if p.stars == 0:
            if FLOAT_TOKENS.search(p.raw):
                # PPC64EC step C: a by-value float is a SERVED position now
                # (fpmask/fpwide drive the surface's floating-point invoker),
                # not a refusal -- unless it sits past the eighth parameter,
                # where the masks cannot name it and either register file
                # would have run out anyway.
                if i >= 7:
                    raise Refused(
                        "passes %s by value in parameter position %d, past "
                        "the eight-parameter register window the fp masks "
                        "name" % (p.base, i + 1))
                fpmask |= 1 << i
                if p.base in ("double", "DOUBLE"):
                    fpwide |= 1 << i
                cls[i] = CA["PASS"]
                continue
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
            # EVERY by-value integer's width is now PUBLISHED, not merely
            # recognised -- the port the old refusal text here promised
            # (narrow_of() and the three mask fields, from the mf copy),
            # done the day the i386 lane needed a width for every slot.
            #
            # A 1- or 2-byte by-value integer arrives with UNDEFINED upper
            # bits (MS-x64 lets the caller write only the declared width;
            # ELFv2 makes extending it the CALLER's job) -- narrowmask.
            # A 4-byte one is clean in a register but stale-topped on the
            # stack -- dwordmask, [MEASURED] on the d3d12 surface as
            # CopyDescriptors' argument seven.  The 4-vs-8 split itself is
            # asked of clang's x86_64-windows-gnu layout, never of a name
            # list, and the SIGNEDNESS comes from the same probe: ELFv2's
            # rule is "signed types sign-extend", applied to the C type as
            # clang parses it (so BOOL -- int underneath -- sign-extends,
            # which is identical for every value a BOOL may hold).
            narrow = narrow_of(p)
            if narrow:
                if i >= 8:
                    raise Refused(
                        "passes a %d-byte %s by value in argument position "
                        "%d, past the eight parameters the narrowing masks "
                        "cover" % (narrow[0], p.base, i + 1))
                narrowmask |= 1 << i
                if narrow[0] == 2:
                    narrowwide |= 1 << i
                if narrow[1]:
                    narrowsign |= 1 << i
            else:
                sp = byval_spelling(p.raw)
                w64 = oracle.width64(sp)
                if w64 == 4:
                    if i >= 16:
                        raise Refused(
                            "passes a 4-byte %s by value in argument "
                            "position %d, past the sixteen parameters "
                            "dwordmask covers" % (p.base, i + 1))
                    dwordmask |= 1 << i
                    if oracle.is_signed(sp):
                        dwordsign |= 1 << i
                elif w64 != 8:
                    raise Refused(
                        "by-value %s measures %d byte(s) on the x86-64 "
                        "guest, which is not a width the masks can spell; "
                        "refusing rather than guessing where its bytes land"
                        % (p.base, w64))
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

    return (cls, xaux, caux, aux, aux2,
            (narrowmask, narrowwide, narrowsign, dwordmask, dwordsign,
             fpmask, fpwide, fpret))


# --------------------------------------------------------------------------
# emission
# --------------------------------------------------------------------------

def c_guid(u):
    a, b, c, d, e = u.split("-")
    d4 = d + e
    return "{0x%s,0x%s,0x%s,{%s}}" % (
        a, b, c, ",".join("0x" + d4[i:i + 2] for i in range(0, 16, 2)))


def generate(roster, prefix, header_dir=HEADERS, surface=None,
             roster_name="interfaces_dxvk.json", oracle=None, layouts=None):
    ifaces = roster["interfaces"]
    order = sorted(ifaces)
    iface_index = {n: i for i, n in enumerate(order)}
    byval_ok = set(BYVAL_INTEGER) | set(roster["integer_types"])
    oracle.sign_names |= byval_ok
    oracle.measure(collect_spellings(ifaces))
    # The lie detector for the width claims this file still spells by name:
    # NARROW_BYVAL and BYVAL_AGGREGATE must measure under clang's
    # x86_64-windows-gnu exactly what they claim, or generation stops --
    # the committed 64-bit behaviour would already have been wrong.
    bad = []
    for iface in ifaces.values():
        for slt in iface["slots"] or ():
            for i, praw in enumerate(slt["params"]):
                pp = Param(praw)
                if pp.stars:
                    continue
                narrow = narrow_of(pp)
                if narrow:
                    expected = narrow[0]
                elif pp.base in BYVAL_AGGREGATE:
                    expected = 8
                else:
                    continue
                got = oracle.width64(byval_spelling(pp.raw))
                if got != expected:
                    bad.append("  %s::%s arg %d (%s): claimed %d byte(s), "
                               "clang's x86-64 layout says %d"
                               % (slt["owner"], slt["name"], i + 1, praw,
                                  expected, got))
    if bad:
        sys.exit("gen_winecom: width name-claims disagree with clang for "
                 "%d parameter(s):\n%s" % (len(bad),
                                            "\n".join(sorted(set(bad)))))
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
    stats = dict(marshalled=0, refused=0, hand=0, identity=0, iunknown=0,
                 geom=0, geom_unproven=0, qword_slots=0, ret_qword=0,
                 refused32=0, fp_served=0)
    refusal_log = []
    qword_log = []
    unproven_log = []
    chk_entries = []
    chk_seen = set()

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
        # Counted PER INTERFACE and folded into the file-wide stats only if
        # the table is actually emitted: an identity-elided interface's rows
        # are dropped wholesale below, and a banner that counted them would
        # claim geometry (and marshalled rows) this file does not publish --
        # the d3d11 banner said "2611 rows carry WINECOM_F_I386_GEOM" while
        # only 2593 existed, the 18 others living in elided tables.
        istats = dict(marshalled=0, refused=0, hand=0, refused32=0,
                      geom=0, geom_unproven=0, qword_slots=0, ret_qword=0,
                      fp_served=0)
        iqword_log = []
        iunproven_log = []
        ichk = []
        for s in slots:
            key = "%s::%s" % (s["owner"], s["name"])
            label = key
            if s["slot"] < 3:
                rows.append('    { "%s", NULL, NULL, NULL, %d, 0, 0, 0, NULL },'
                            '  /* runtime */' % (label, 1 if s["slot"] else 3))
                stats["iunknown"] += 1
                continue
            argc = 1 + len(s["params"])
            # The i386 frame geometry is computed for EVERY row -- hand and
            # refused included: a refused slot still has to pop the right
            # number of bytes before it can so much as answer E_NOTIMPL.
            qm, retq, geo_unproven = slot_geometry(s, oracle)
            gflags = []
            if geo_unproven is None:
                gflags.append("WINECOM_F_I386_GEOM")
                if retq:
                    gflags.append("WINECOM_F_RET_QWORD")
                istats["geom"] += 1
                ichk.append((key, s["params"], argc, qm))
                if qm:
                    istats["qword_slots"] += 1
                if retq:
                    istats["ret_qword"] += 1
                if qm or retq:
                    iqword_log.append((key, qm, retq))
            else:
                istats["geom_unproven"] += 1
                iunproven_log.append((key, geo_unproven))
            flags = []
            if s["ret"] == "void":
                flags.append("WINECOM_F_RET_VOID")
            if key in hand_index:
                rows.append('    { "%s", NULL, NULL, NULL, %d, WINECOM_F_HAND%s,'
                            ' %d, 0, NULL, 0, 0, 0, 0, 0, 0, 0, 0, 0x%04x },'
                            % (label, argc,
                               "".join("|" + f for f in flags + gflags),
                               hand_index[key], qm))
                istats["hand"] += 1
                interesting = True
                continue
            reason = surface["refuse"].get(key)
            if reason is None:
                try:
                    cls, xaux, caux, aux, aux2, masks = classify(
                        key, s, ifaces, iface_index, byval_ok, bearing,
                        why_bearing, opaque, why_opaque, surface["void_pp"],
                        oracle)
                except Refused as e:
                    reason = str(e)
            if reason is not None:
                rows.append('    { "%s",\n      "%s::%s: %s",\n'
                            '      NULL, NULL, %d, %s, 0, 0, NULL, 0, 0, 0,'
                            ' 0, 0, 0, 0, 0, 0x%04x },'
                            % (label, s["owner"], s["name"],
                               reason.replace('"', "'"), argc,
                               "|".join(gflags) or "0", qm))
                istats["refused"] += 1
                refusal_log.append((n, s["slot"], key, reason))
                interesting = True
                continue
            # The i386 struct audit.  A row it refuses (refuse32) is STILL a
            # fully marshalled row for the 64-bit lane -- the first cut here
            # emitted refuse32 rows with NULL cls/xaux, so the 64-bit
            # dispatcher treated every parameter as CA_PASS and handed the
            # guest a RAW HOST POINTER from CreateTexture2D's out parameter
            # [MEASURED: check-d3d11-smoke went red at step 3, the texture
            # unrecognizable one call later].  Only the LAST field differs.
            reps, refuse32 = audit_i386(key, s, cls, layouts, byval_ok)
            r32 = "NULL"
            if refuse32 is not None:
                r32 = '\n      "%s::%s: %s"' % (s["owner"], s["name"],
                                                 refuse32.replace('"', "'"))
                istats["refused32"] += 1
                refusal_log.append((n, s["slot"], key, "[i386 only] " + refuse32))
                interesting = True
            rname = "NULL"
            repn = 0
            if refuse32 is None and reps is not None and layouts is not None:
                gflags.append("WINECOM_F_I386_STRUCTS_OK")
            if refuse32 is None and reps:
                rname = "reps_%s_%d" % (n, s["slot"])
                ents = []
                for pi, ci, db, ty in reps:
                    if ty == PTRWIDTH_REP:
                        s64, s32, fn = 8, 4, "PTRWIDTH"
                    else:
                        s64 = layouts["divergent"][ty]["size64"]
                        s32 = layouts["divergent"][ty]["size32"]
                        fn = ty
                    ents.append("{ %d, %s, %d, %d, %d, wine_repack32_%s,"
                                " wine_repack64_%s }"
                                % (pi, "0xff" if ci is None else str(ci), db,
                                   s64, s32, fn, fn))
                decls.append("static const struct winecom_rep %s[] =\n"
                             "    { %s };" % (rname, ",\n      ".join(ents)))
                repn = len(reps)
                interesting = True
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
            nm, nw, ns, dm, ds, fpm, fpw, fpr = masks
            if fpm or fpr:
                # A float-bearing row, served forward through the surface's
                # invoke_fp (PPC64EC step C).  The 32-BIT lane has not
                # adopted the FP invoker, so unless the i386 audit already
                # refused this row it refuses here by name -- served on the
                # 64-bit lane, honest on the other, the refuse32 discipline.
                if r32 == "NULL":
                    r32 = ('"the 32-bit lane has not adopted the FP invoker; '
                           "a hand32 walker or the lane's own FP path comes "
                           'first"')
                rows.append('    { "%s", NULL, %s, %s, %d, %s, %d, %d, %s,'
                            ' 0x%02x, 0x%02x,'
                            ' 0x%02x, 0x%02x, 0x%02x, 0x%02x, 0x%04x, 0x%04x,'
                            ' 0x%04x, %s, %d, %s, %d },'
                            % (label, cname, xname, argc,
                               "|".join(flags + gflags) or "0", aux, aux2, kname,
                               fpm, fpw,
                               0, nm, nw, ns, dm, ds, qm, rname, repn, r32, fpr))
                istats["marshalled"] += 1
                istats["fp_served"] += 1
                interesting = True
            else:
                rows.append('    { "%s", NULL, %s, %s, %d, %s, %d, %d, %s, 0, 0,'
                            ' 0x%02x, 0x%02x, 0x%02x, 0x%02x, 0x%04x, 0x%04x,'
                            ' 0x%04x, %s, %d, %s },'
                            % (label, cname, xname, argc,
                               "|".join(flags + gflags) or "0", aux, aux2, kname,
                               0, nm, nw, ns, dm, ds, qm, rname, repn, r32))
                istats["marshalled"] += 1

        if not interesting:
            # An identity row: IUnknown's three slots are served by the
            # runtime from the proxy table and everything else is refused
            # loudly.  Emitting a table of nothing but CA_PASS rows would
            # claim more than we checked.  istats/ichk and the per-interface
            # logs are DISCARDED with the rows: the banner counts what this
            # file publishes, the frame checker checks what the banner
            # counts, and an elided table publishes nothing.  (A NULL table
            # refuses every slot at runtime, so nothing uncounted can run.)
            stats["identity"] += 1
            tables.append((n, None))
            continue
        for k in istats:
            stats[k] += istats[k]
        qword_log += iqword_log
        unproven_log += iunproven_log
        for e in ichk:
            dedup = (e[0], e[2], e[3])
            if dedup not in chk_seen:
                chk_seen.add(dedup)
                chk_entries.append(e)
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

    if _audit_tripwire:
        sys.exit("gen_winecom: %d divergent-struct pointer parameter(s) have "
                 "plural-looking names and no plurality ruling; add each to "
                 "STRUCT_ARRAYS, STRUCT_SPECIAL or CONFIRMED_SCALAR:\n  %s"
                 % (len(_audit_tripwire),
                    "\n  ".join("%s: %s (-> %s)" % t
                                 for t in sorted(set(_audit_tripwire)))))

    # The second opinion on every published frame: clang's own stdcall @N.
    checked = oracle.check_stdcall_frames(chk_entries, {})

    w("\n/* %d slot(s) marshalled, %d hand-written, %d refused with a named\n"
      " * reason, %d IUnknown slot(s) served by the runtime; %d interface(s)\n"
      " * carry identity rows only.\n"
      " * i386 geometry: %d row(s) carry WINECOM_F_I386_GEOM (%d distinct\n"
      " * frames re-checked against clang's stdcall @N decoration), %d with\n"
      " * a non-zero qwordmask, %d returning EDX:EAX; %d row(s) publish no\n"
      " * i386 geometry and a 32-bit lane must fail closed on them; %d\n"
      " * row(s) refuse on the 32-bit lane only (refuse32). */"
      % (stats["marshalled"], stats["hand"], stats["refused"],
         stats["iunknown"], stats["identity"],
         stats["geom"], checked, stats["qword_slots"], stats["ret_qword"],
         stats["geom_unproven"], stats["refused32"]))
    return ("\n".join(out) + "\n", stats, refusal_log, qword_log,
            unproven_log)


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
    ap.add_argument("--clang", default="clang",
                    help="the compiler the i386 width oracle asks")
    ap.add_argument("--repack-json", default=None,
                    help="the layout roster gen_repack32.py --json wrote; "
                         "defaults to repack32_<prefix>.json beside this "
                         "script when that exists.  Without one, no row gets "
                         "WINECOM_F_I386_STRUCTS_OK and the 32-bit lane "
                         "fails closed on every marshalled row.")
    ap.add_argument("--build-include", default=None,
                    help="directory holding Wine's widl-GENERATED headers "
                         "(wtypes.h and friends); defaults to the sibling "
                         "wine-build/include when it exists")
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
    # The i386 width oracle reads the SAME vendored MinGW headers the
    # roster was generated from, over Wine's base includes.  WINBOOL is the
    # one base type those headers leave to MinGW's windef.h (their own copy
    # sits under `#if 0`, widl's convention); mingw-w64 spells it
    # `typedef int WINBOOL;` and so does this shim.
    wine_root = os.path.abspath(os.path.join(HERE, "..", ".."))
    build_inc = args.build_include
    if build_inc is None:
        cand = os.path.abspath(os.path.join(wine_root, "..",
                                            "wine-build", "include"))
        build_inc = cand if os.path.isdir(cand) else None
    probes = {
        "d3d11": "#include <d3d10_1.h>\n#include <d3d11_4.h>\n"
                 "#include <dxgi1_6.h>\n",
        "d3d9":  "#include <d3d9.h>\n",
    }
    oracle = I386Oracle(args.clang,
                        "typedef int WINBOOL;\n" + probes[args.prefix],
                        [args.headers, build_inc,
                         os.path.join(wine_root, "include"),
                         os.path.join(wine_root, "include", "msvcrt")],
                        set(), args.prefix)

    repack_json = args.repack_json
    if repack_json is None:
        cand = os.path.join(HERE, "repack32_%s.json" % args.prefix)
        repack_json = cand if os.path.exists(cand) else None
    layouts = None
    if repack_json:
        with open(repack_json) as fh:
            layouts = json.load(fh)
        print("i386 struct audit: %d divergent, %d identical aggregates "
              "from %s" % (len(layouts["divergent"]),
                           len(layouts["identical"]), repack_json))
    else:
        print("i386 struct audit: NO layout roster; every marshalled row "
              "will fail closed on the 32-bit lane")

    text, stats, refusals, qwords, unproven = generate(
        roster, args.prefix, args.headers, surface,
        os.path.basename(args.roster), oracle, layouts)

    print("surface %s: %d marshalled, %d hand-written, %d refused, "
          "%d IUnknown, %d identity-only interface(s)"
          % (roster["surface"], stats["marshalled"], stats["hand"],
             stats["refused"], stats["iunknown"], stats["identity"]))
    print("i386 geometry: %d row(s) published, %d qword-marked, %d EDX:EAX "
          "return(s), %d without geometry (fail closed)"
          % (stats["geom"], stats["qword_slots"], stats["ret_qword"],
             stats["geom_unproven"]))

    if args.report:
        print("\nslots whose i386 frame differs from all-dwords "
              "(qwordmask / EDX:EAX return):")
        for key, qmv, retq in sorted(set(qwords)):
            print("  %-64s qwordmask 0x%04x%s"
                  % (key, qmv, "  ret EDX:EAX" if retq else ""))
        if unproven:
            print("\nslots publishing NO i386 geometry, and why:")
            for key, why in sorted(set(unproven)):
                print("  %-64s %s" % (key, why))
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
        print("\nflat exports refused in dlls/%s/main.c:" % args.prefix)
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
