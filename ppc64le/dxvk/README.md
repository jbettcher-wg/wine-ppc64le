# dxvk-ppc64le — the native D3D9/D3D10/D3D11 lane

DXVK (D3D9/D3D10/D3D11/DXGI → Vulkan) for ppc64le, carried as a **patch series
against a pinned upstream commit** rather than a fork — the same arrangement as
`ppc64le/vkd3d`, so the provenance of every changed line is `git diff` and
DXVK's zlib notice travels with the source it belongs to.

**Authority.** This project exists in two places: this fold, inside the Wine
repo, and the standalone `dxvk-ppc64le/` working copy. **The fold is
authoritative** — the Wine build drives its meson build (`build-for-wine.sh`),
reads `interfaces_dxvk.json` at build time (three `.thunks` files name it), and
versions all of it in the Wine repo. The standalone copy is a mirror for work
outside the Wine tree; when the two disagree, the fold wins. That is the same
sentence `ppc64le/vkd3d/README.md` opens with, for the same reason.

`interfaces_dxvk.json` deliberately has **one copy anywhere**. The guest and
native halves of the COM boundary are emitted by different generators from it,
and a second copy that drifted would dispatch a call into the neighbouring
slot with the neighbour's argument types — caught only at attach time, by the
runtime IID cross-check, if at all.

```sh
./bootstrap.sh              # clone at the pinned commit, apply dxvk-patches/
./bootstrap.sh --check      # verify an existing src/ matches, change nothing
./bootstrap.sh --force      # re-clone from scratch
```

**The series is a stack now, and `bootstrap.sh` had to learn that.** It used to
ask each patch in turn whether a reverse-apply would succeed and call that
already-applied. That question stopped having an answer the moment two patches
touched the same lines, which `0003` does — it adds a line to the WSI driver
table `0001` created, so `0001`'s own hunks no longer describe the file. `0004`
stacks on `0003` the same way, adding overrides to the class `0003` declared.
`[MEASURED]` `--check` on a correctly patched tree reported `not applied:
0001-foreign-wsi-backend.patch`, exactly backwards. Nor can the stack be checked
in one go: `git apply --check a b c` re-reads each file from disk for every
patch instead of chaining them, so a reverse check of the whole series fails on
everything below the top, while *forward* application chains fine because each
`git apply` is a separate process. So the applied state is recorded in
`src/.dxvk-patch-stamp` (the pinned commit plus the md5 of every patch file in
series order) and checked together with a reverse-apply of the **top** patch,
which by construction must always reverse cleanly.

`src/` is upstream's checkout and is gitignored. The series is applied to the
**working tree and never committed**, which is what lets a probe recover the
pre-change behaviour with `git show HEAD:<file>`.

## The shape of the lane

```
guest x86-64 PE  -->  C:\windows\sysx8664\{d3d11,dxgi,d3d10core}.dll
   |                  (spec2thunk COM mode from the three .thunks files:
   |                   pure trap surface, no marshalling knowledge)
   |  trap; ntdll maps RIP -> (iface, slot), calls the NATIVE namesake's
   |  __wine_com_dispatch
   v
native d3d11.dll (dlls/d3d11/main.c)
   |  = libs/winecom's dispatch loop over dlls/d3d11/d3d11_marshal.h
   v
d3d11.so (dlls/d3d11/unix.c)  -->  libdxvk_d3d11.so + libdxvk_dxgi.so
                                     + libdxvk_d3d10core.so  -->  Vulkan/RADV
```

**One winecom instance for three DLLs.** `libs/winecom`'s proxy state is
per-linkee. If native `dxgi.dll` linked its own copy, the `IDXGIAdapter` proxy
it minted would not be one of `d3d11`'s proxies, and the very first
`D3D11CreateDevice(adapter, ...)` a real game makes would be refused as a
guest-implemented object. So native `d3d11.dll` owns the only instance and
`dlls/dxgi/dxgi.spec` and `dlls/d3d10core/d3d10core.spec` forward **both** their
flat exports and `__wine_com_dispatch` into it. All three GUEST modules publish
the same roster, which is what makes a proxy's guest vtable interchangeable
between them; `winecom_attach` cross-checks every IID and slot count of every
loaded one.

**Two names per interface-bearing flat export.** `__wine_guest_<Name>` is what
the guest reaches (the `GUEST-IMPL` rows); the plain name is what a native
ppc64 PE would reach, and it refuses loudly, because a proxy's vtable is the
guest module's array of x86-64 trap stubs and a native caller would execute
them as ppc64. `dlls/d3d12` predates this and exports one name for both.

**Four guest modules publish this roster now**, not three: `d3d10.dll` joined
`d3d11.dll`, `dxgi.dll` and `d3d10core.dll`. A D3D10 application imports
`d3d10.dll` and nothing else on this surface, so it is the only module in the
process publishing the roster -- and `winecom_attach` materialises the proxy
vtables from a publisher. `[MEASURED]` without it, `D3D10CreateDevice` from
such a guest returned `E_FAIL` with `winecom: no guest thunk module in this
process; COM dispatch cannot work`.

## The roster and the marshal tables

| | |
|---|---|
| `gen_interfaces.py` | DXVK's vendored MinGW/widl headers → `interfaces_dxvk.json` |
| `gen_winecom.py` | that JSON → `dlls/d3d11/d3d11_marshal.h`; `--prefix d3d9` for the other surface |
| `dxvk_flat_surface.h` | the nine flat exports no Wine header declares, so the oracle can type them |
| `dxvk_flat_surface_d3d9.h` | the four D3D9 ones, likewise |

`gen_winecom.py` carries a table per surface -- hand-written slots, named
refusals, audited `void **`, refused flat exports -- selected by `--prefix`.
An unknown prefix stops generation rather than emitting a surface with nothing
hand-written and nothing refused, which reads exactly like a clean run.

`[MEASURED] 2026-08-17, the test machine (POWER9, V620/RADV)` — the D3D11+DXGI subset
reproduces the standalone project's measured baseline exactly, **111
interfaces / 2,593 slots**, and D3D10 adds **+26 / +429** for a roster of
**137 interfaces / 3,022 slots**. (`ID3D10Multithread` is dropped: it shares
its IID *and* its vtable with `ID3D11Multithread`, and two roster rows for one
IID is a thing spec2thunk refuses outright — correctly.)

Of those 3,022 slots: **2,060 marshalled**, **424 hand-written**, **411 served
by the runtime** (IUnknown), **127 refused with a named reason**. Every
refusal is a `FIXME` the runtime prints once, carrying the reason below; none
of them is silent, and none of them is a pass-through.

`[MEASURED] 2026-08-17` — that is 27 fewer refusals than before presentation
existed. **39 window-handle slots came back** (26 by-value `HWND`, 13
`DXGI_SWAP_CHAIN_DESC`), four of them as hand-written slots rather than
generated ones; **12 new refusals** replaced them, on the two swapchain routes
that have no window at all.

### What is refused, and why each is a real hazard

| n | why |
|---:|---|
| 47 | **by-value `HANDLE`.** A Wine `HANDLE` is a Wine object; DXVK's native side encodes an event as the tagged eventfd `0x4556464400000000 \| fd` and a shared resource as its own key. Handing one namespace's integer to the other is the collision `ppc64le/vkd3d`'s tagged-handle series exists to prevent — measured there as eight bytes written into a live pipe. **This is the one that will never come back**, which is why `check-d3d11-smoke.sh`'s refusal control was moved onto it. |
| 30 | **`ID3D11ClassInstance **ppClassInstances`** on the six `XSGetShader` slots: an out-array whose element count arrives through a `UINT *`, which winecom has no class for. Wrapping only its first element would be worse than refusing. |
| 15 | **`WCHAR`.** DXVK's native headers say `typedef wchar_t WCHAR` (4 bytes here); the guest PE's is 2. Includes `ID3DUserDefinedAnnotation::BeginEvent`/`SetMarker`, which is why a debug-annotating game will see `E_NOTIMPL` there. |
| 6 | **`IDXGIFactory2::CreateSwapChainForCoreWindow`** — a WinRT CoreWindow has no `HWND` at all. |
| 6 | **`IDXGIFactory2::CreateSwapChainForComposition`** — a windowless composition swapchain. DXVK serves it by fabricating a dummy window through its WSI backend (`DxgiSurfaceFactory::CreateDummyWindow`); this lane's backend owns no windows, and a swapchain that renders correctly and is visible nowhere is worse than a refusal. |
| 6 | **by-value `float`** in `VideoProcessorSetStreamAlpha`/`SetStreamLumaKey`. |
| 5 | **`DXGI_SHARED_RESOURCE`**, a handle-bearing struct. |
| 3+3+3 | **the video path's raw pointers**: `ID3D11VideoContext::GetDecoderBuffer`, `DecoderExtension`, `VideoProcessorBlt`. Found by walking the headers' own struct bodies, and exactly the nine `dxvk-ppc64le/docs/hazard-hunt.md` §3.1 measured by hand. |
| 3 | **`D3D11_AUTHENTICATED_CONFIGURE_OUTPUT`**, another handle-bearing struct. |

**`HWND` is no longer on that list**, and the reason it could leave without
becoming a lie is worth stating: the value never needed converting. The guest
PE calls Wine's own user32, so there is exactly ONE window-handle namespace in
the process and the same integer names the same window on both sides. `HANDLE`
is the opposite case — DXVK's native side has its own encoding for the same
things — which is why it stays.

And one flat export: **`D3D11On12CreateDevice`**, refused because it needs a
live `ID3D12Device` from the d3d12 lane and the two lanes hold separate winecom
instances — a d3d12 proxy handed to this runtime is not one of its proxies and
would be refused a frame later, in the middle of a resource wrap, where the
reason would be illegible.

### What is hand-written rather than generated

`GetPrivateData` / `SetPrivateData` / `SetPrivateDataInterface` (401 slots
across the roster, one implementation each), the three float-bearing
`ID3D11DeviceContext` slots, and the four presentation slots
(`IDXGIFactory::CreateSwapChain`, `IDXGIFactory2::CreateSwapChainForHwnd`,
`IDXGISwapChain::Present`, `IDXGISwapChain1::Present1` — 23 slots once
inherited into every derived vtable).

The presentation four are not hand-written because their arguments are hard.
Every one of them would marshal correctly on its own now. What the generator
cannot express is the **order of operations**: win32u wants its client surface
updated before a present and marked presented after, and both calls must
happen on a Wine thread — which the application's call into `Present` is, and
DXVK's submission thread, where the real `vkQueuePresentKHR` happens, is not.

`GetPrivateData` is the interesting one. Its out-parameter is `void *pData`,
not `void **` — no slot flag could mark it, and the old FEX stack's
`DXVK_THUNK_STRICT` could not warn about it either. On a GUID the application
registered through `SetPrivateDataInterface` it hands back a **raw native
pointer**. It is the one hazard on this surface invisible to static
classification, so it is answered dynamically: a side table remembers what the
guest stored and gives the guest back what it stored. The entry holds a
reference on the container object, deliberately — the table is keyed by host
address, and an address freed while an entry named it could be reissued to a
new object whose first `GetPrivateData` would return a dangling proxy.

The float slots exist because the unixlib boundary calls with the widest
**integer** form, and on ELFv2 a `float` argument lives in the floating-point
register file, which that form never writes. `ClearDepthStencilView`'s depth
arrives in the guest's XMM3 and is read out of the trap CONTEXT's
`FltSave.XmmRegisters[3]` — the same register, by the same rule, that
`dlls/ntdll/signal_ppc64.c`'s flat FP path uses.

## Presentation

**It presents, on both APIs.** `[MEASURED] 2026-08-17, the test machine` — an x86-64 guest
creates a real Wine window, `D3D11CreateDeviceAndSwapChain`s on it, clears to
`(0.00, 0.25, 0.50, 1.00)` and presents; a separate native ppc64le process with
no Wine in it reads the compositor's own framebuffer and finds **all 65,536
pixels of a 256×256 rectangle holding exactly RGB `00 40 80`**, in a bounding
box of exactly the window's size. Then the same gate does it again through
**D3D9** — `IDirect3D9::CreateDevice` on the same kind of window, `Clear`
through the hand-written by-value-float slot, `Present` on the device — and
finds the same 65,536 pixels of the same colour in the same sized rectangle.
`check-present-smoke.sh` is both.

The two are not variations on one path. D3D9 has no DXGI and no swapchain
object: its surface is built inside `CreateDevice` itself, before anything a
D3D11 application would recognise as a swapchain exists, and `Present` is a
method on the device with a per-call destination-window override. They meet
only at win32u.

```
guest CreateWindowExA                     a real Wine HWND
guest D3D11CreateDeviceAndSwapChain(desc.OutputWindow = it)
   |   the HWND crosses UNCONVERTED -- one window-handle namespace
   v
native d3d11.dll   dlls/d3d11/main.c, four hand-written swapchain slots
   |               push the client size down, remember hwnd per swapchain,
   |               run win32u's two hooks around Present on THIS thread
   v
DXVK's "Win32u" WSI driver     dxvk-patches/0003-win32u-wsi-backend.patch
   |   every question about the window forwarded to the table Wine registered
   v
dlls/d3d11/unix_win32u.c  ==  dlls/d3d12/unix_win32u.c, compiled twice
   v
win32u client surface  ->  winex11 child window / winewayland wl_subsurface
```

**The DXVK side is a fifth WSI backend, not a fork of one.** DXVK's WSI is a
compile-time backend selection — `src/wsi/wsi_platform.cpp` picks one
`WsiBootstrap` from a static array by name — so the patch series adds one more,
`Win32u`, that owns nothing at all: no window, no display connection, no
geometry. Every answer it gives comes from a C callback table Wine registers
at load time (`dxvk_win32u_wsi.h`, the one copy of that ABI). It derives from
the existing foreign driver, so the monitor answers and the deliberate no-ops
on somebody else's window are inherited rather than repeated.

Three things had to be added to DXVK beyond the driver itself:

* `WsiDriver::destroySurface`, called from `Presenter::destroySurface`. The
  `VkSurfaceKHR` was always DXVK's to destroy; what DXVK could not know is that
  this backend's surfaces sit on a Wine **client surface** which nothing else
  would ever release — so the client window outlived every swapchain that used
  it and stayed on screen. Every other backend inherits a no-op.
* `dxvk_wsi_win32u_register`, exported from `libdxvk_dxgi.so`,
  `libdxvk_d3d11.so` and `libdxvk_d3d9.so` **separately**. `src/wsi` is a
  static library, so each of them carries its own `WsiDriver` — and the D3D11
  swapchain's surface is created by DXGI's copy, not d3d11's. The Wine side
  registers into every DXVK library it loads and logs the count, because
  "registered in some of them" presents to nothing and looks exactly like
  success. (`libdxvk_d3d10core.so` links `libdxvk_d3d11.so` rather than the
  WSI static library and has no copy to register into; that is three of four,
  deliberately.)
* `protected:` in place of `private:` on the foreign driver, so the new one can
  derive from it.

**The threading rule is the whole design.** Every win32u call must come from a
Wine thread; DXVK owns a CS thread and a submission thread and neither has a
TEB. So nothing calls win32u from inside DXVK's own threads:

* the **surface** is created inside `IDXGIFactory::CreateSwapChain` and friends,
  which DXVK calls synchronously on the application's thread, from inside our
  unixlib call;
* the **present hooks** are driven from the PE side around the `Present` slot,
  on the caller's own thread — exactly as `dlls/d3d12/unix_present.c` drives
  them for vkd3d;
* and it is **asserted rather than assumed**: every unixlib entry point sets a
  thread-local flag, and a WSI callback arriving without it is refused by name
  (surface creation) or deferred to the next Wine-thread entry (surface
  destruction, which can genuinely happen on the submission thread when the
  last `Rc<Presenter>` reference dies there).

The window's client size is **pushed down, not pulled**: the unix side cannot
reach user32, and the PE side is holding the `HWND` at every point where DXVK
is about to ask. `NtUserGetClientRect`/`NtUserIsWindow` at swapchain creation
and at every present. No `WINE_HWND_SURFACE_VERSION` bump was needed — the
seam the d3d12 lane built answers everything this needs.

### What still refuses, and why

`CreateSwapChainForCoreWindow` and `CreateSwapChainForComposition`: neither has
an `HWND`. DXVK serves them by fabricating a dummy window through its WSI
backend, and this backend owns no windows — it presents to windows the
application asked Wine for. `D3D11On12CreateDevice` is unchanged and unrelated:
it needs the d3d12 lane's winecom instance.

### What could not be proven here, and how that was measured

The gate presents to a **headless Weston with the GL renderer**, not to an
Xvfb, and that is not a preference. `[MEASURED] 2026-08-17, the test machine`: an Xvfb has
no DRI3, and RADV refuses to present to an X server without it — with no Wine
anywhere in the process, `DISPLAY=:73 vkcube` on an Xvfb prints `MESA: info:
vulkan: No DRI3 support detected - required for presentation` and dumps core.
Driven through this lane the same limit appears one layer up: the win32u client
surface is created, DXVK builds a 256×256 `B8G8R8A8_UNORM` swapchain on it, and
the first `vkAcquireNextImageKHR` returns `VK_ERROR_SURFACE_LOST_KHR`. So the
**winex11 half of this path is built and reaches swapchain creation, and the
last step of it is unproven on this machine** for want of a display server that
can present at all. The winewayland half is proven to the pixel.

## The D3D10 front end, and what it does not serve

`d3d10core.dll` has been served since the D3D10 extension went into the roster:
+26 interfaces, +429 slots, the same winecom instance, and the guest thunk that
Wine never had. What was still missing was **`d3d10.dll`** — the module a D3D10
application actually imports — so a guest that asked for it died on a missing
library before anything could tell it anything.

It now loads, and **two of its exports work**: `D3D10CreateDevice` and
`D3D10CreateDeviceAndSwapChain`, implemented in `dlls/d3d11/main.c` the way the
real runtime implements them — make a DXGI factory, take its first adapter,
hand both to `D3D10CoreCreateDevice` — with **host-side calls only**, so no
proxy is ever minted for the factory or the adapter. They exist for the length
of the call and the application never sees them, which is what the real runtime
does too. The swapchain variant goes through the same window bookkeeping the
hand-written `CreateSwapChain` slot does, so a D3D10 title presents through
exactly the path D3D11 does.

`[MEASURED] 2026-08-17` — a guest that imports only `d3d10.dll` gets
`D3D10CreateDevice hr=0x00000000`, a working `ID3D10Device` proxy,
`CreateTexture2D hr=0x00000000` on it, and `D3D10GetPixelShaderProfile ->
ps_4_0`. Two things had to be right for that and neither was at first: the
guest `d3d10.dll` had to be added to the roster's publisher list (it is the
ONLY module such an application loads, so without it the proxy vtables had
nowhere to come from — the runtime said so, by name), and the feature level had
to be `D3D_FEATURE_LEVEL_10_0` rather than the SDK version, which occupies the
same argument position one function along. Passing the SDK version through
produced a device anyway and DXVK logged `Using feature level 29`; 29 is not a
feature level.

**The rest of `d3d10.dll` does not cross**, and the reason is structural rather
than provisional: DXVK ships no `d3d10.dll` at all, so the effects framework,
the state-block builders and the shader reflection are **Wine's own C**,
written against `ID3D10Device`. On this lane an `ID3D10Device` is a guest proxy
whose vtable is an array of x86-64 trap stubs, and native C driving one would
execute those bytes as ppc64 on its first call. Serving it needs either a
reverse-proxy runtime (so a native caller can hold a guest object) or a native
D3D10 effects implementation, and neither exists.

The audit splits them three ways, and the split is a finding:

| | |
|---|---|
| **3 refused** | `D3D10CreateEffectFromMemory`, `D3D10CreateEffectPoolFromMemory`, `D3D10CreateStateBlock` — each names a ROSTERED interface, so the flat-surface audit sees them and `GUEST-REFUSE` answers `E_NOTIMPL` by name. |
| **9 excluded** | every export that vends an `ID3D10Blob` or takes an `ID3D10Include`. Those are d3dcommon's interfaces and are **not in this roster**, so the audit calls a `GUEST-REFUSE` on them STALE and would let them pass through — handing the guest a native vtable. The same blind spot `dlls/xaudio2_9/xaudio2_9.thunks` documents for its XAPO factories, and the same answer: `EXCLUDE` binds the guest's import to the per-symbol `0xdead0000` sentinel, so an application that wants to compile a shader faults with the symbol's own name instead of crashing inside a native blob. |
| **3 audited pass-throughs** | the profile getters. Each takes an `ID3D10Device` and **never dereferences it**: Wine's implementations are three-line stubs that FIXME the pointer and return a static ASCII string. `GUEST-PASS` with the reason spelled out, and a note that this must become an `EXCLUDE` the day they grow a real `GetFeatureLevel` call. |

The `D3D10StateBlockMask*` family carries no interface at all — bit arithmetic
over a caller-owned struct — and passes cleanly.

`D3D10_DRIVER_TYPE_REFERENCE`, `_NULL` and `_SOFTWARE` are refused: all three
want a software adapter made from a rasteriser DLL, DXVK implements neither,
and answering with the hardware device would give an application that asked to
compare against the reference the opposite of what it asked for.

**Most D3D10-era titles use the effects framework**, so this is a smaller
practical unlock than the D3D9 lane — and that is worth saying plainly rather
than counting the two working exports as the API.

## The D3D9 lane

`[MEASURED] 2026-08-17` — **21 interfaces, 497 vtable slots**, of which **413
marshalled, 19 hand-written, 63 served by the runtime (IUnknown) and 2 refused
with a named reason.** Two. The whole of D3D9 crosses this boundary except
`IDirect3DSurface9::GetDC` and `ReleaseDC`, which hand out and take back a GDI
device context.

That ratio is not luck, and the contrast with D3D11's 127 refusals is worth
reading: D3D9 predates shared handles, predates `ID3D11ClassInstance` arrays,
and has no video decode surface at all — the three things that account for
almost all of D3D11's. What it has instead is **floats by value in the middle
of the API** and **no DXGI**, and those are what the D3D9 lane had to answer.

| | |
|---|---|
| `interfaces_d3d9.json` | the roster, from DXVK's vendored `d3d9.h` |
| `dlls/d3d9/d3d9_marshal.h` | the marshal tables, `--prefix d3d9` |
| `dlls/d3d9/main.c` | a SECOND winecom instance, 11 hand-written slots |
| `dlls/d3d9/unix.c` | `libdxvk_d3d9.so`, its own WSI registration |
| `check-d3d9-smoke.sh` | native vs guest, byte-identical, texel-exact |

**A second header dialect.** `d3d9.h` is a `DECLARE_INTERFACE_` header, not a
`MIDL_INTERFACE` one: methods are `STDMETHOD_(Ret,Name)(THIS_ args) PURE`, and
each interface body **re-declares every inherited method**. `gen_interfaces.py`
learned that dialect and verifies, per interface, that the re-declared prefix
matches the base's vtable name for name and in order — because getting it wrong
compiles fine and dispatches `Clear` into `Present`. All 20 interfaces check
out; the presentation-critical slots are `IDirect3DDevice9::Reset` at 16 and
`::Present` at 17.

**A second winecom instance, and that is correct.** `dlls/d3d11` holds the only
instance for d3d11+dxgi+d3d10core because those three share objects — one
`D3D11CreateDevice(adapter, ...)` spans two of them. D3D9 shares none with
them: no D3D9 method takes a DXGI interface and no DXGI method takes a D3D9
one, and `libdxvk_d3d9.so` has no `DT_NEEDED` on `libdxvk_dxgi.so` at all,
which is the same statement in the linker's words.

**Both float directions are value-checked, and one is not.** `SetNPatchMode`
takes a float in XMM1 and `GetNPatchMode` returns one in XMM0; DXVK stores and
returns the value verbatim, so `check-d3d9-smoke.sh` round-trips `0x40600000`
(3.5f) through both and compares the BITS -- printing the number instead would
hide a low-mantissa difference behind the probe's own formatting. `Clear`'s
depth is exercised on every frame of the presentation gate's D3D9 leg but its
VALUE is not asserted anywhere, because nothing on this surface reads a depth
buffer back; that is a gap and is named as one rather than counted as covered.

**`Clear`'s float is on the stack, not in XMM.** `IDirect3DDevice9::Clear(count,
rects, flags, colour, float z, stencil)` — `z` is the FIFTH argument counting
`this`, and MS-x64 puts arguments past the fourth on the stack, where a float
is four bytes in an eight-byte slot. So it is read out of the trap CONTEXT's
**stack image** and not out of `FltSave`, which is the opposite of D3D11's
`ClearDepthStencilView` (fourth argument, XMM3). Both end up in the same place
for the same reason: on ELFv2 a float argument lives in `f1..f13`, which the
unixlib's widest-integer call form never writes. Refusing `Clear` would have
refused the API — every D3D9 title calls it every frame.

**There is no offscreen D3D9 device.** `IDirect3D9::CreateDevice` always
creates one implicit swapchain tied to the `HWND` it is passed, and DXVK builds
its `Presenter` — and its `VkSurfaceKHR` — inside it. `[MEASURED]`: headless,
with `DXVK_WSI_DRIVER=Headless`, `CreateDevice` returns `0x8876086A`
(`D3DERR_NOTAVAILABLE`) and DXVK says `Foreign WSI: headless driver cannot
create a surface`. With upstream DXVK's own `d3d9.deferSurfaceCreation` option
the Presenter constructor skips surface creation and the whole offscreen
pipeline comes up. `check-d3d9-smoke.sh` sets it on **both** legs and carries
that measurement as a negative control of its own: without the option, the
identical binary must still fail at device creation, or the gate's premise has
quietly stopped being true.

## Building

**The Wine build does this for you.** `./configure && make` in a Wine build
tree runs `build-for-wine.sh` (via the "ppc64le native D3D11 lane" rule in
`configure.ac`), which bootstraps `src/` if absent, builds the native libraries
with meson/ninja into `$(top_builddir)/ppc64le/dxvk-build/`, and symlinks them
into `dlls/d3d11/` next to the d3d11 unixlib, which dlopens them there — no
environment variable, no install step.

**`build-for-wine.sh` now builds ninja's default target rather than a list of
library names, and that is a fix rather than a widening.** It used to name the
four libraries by their unversioned symlinks, on the reasoning that the
versioned names would stop naming anything the day the pinned commit's version
changed. The reasoning was right and the conclusion was wrong: **meson does not
emit a ninja target for the unversioned name** — it creates that symlink as a
side effect of the link rule for the versioned library, so
`ninja src/d3d11/libdxvk_d3d11.so` names a file with no rule, and ninja treats
a file with no rule as a *source*. `[MEASURED] 2026-08-17, the test machine`: with a new
file added to `src/wsi` and `build.ninja` correctly regenerated to compile it,
that command printed `ninja: no work to do` and exited zero, twice, while
`ninja src/wsi/libwsi.a` on the same tree had twelve objects to compile. **Every
incremental DXVK build this lane had ever done was a no-op that reported
success** — invisible until the day a patch to DXVK had to change behaviour,
which is the day this was found. The existence check could never have caught
it: the files were there, because the first build had built them.

The default target also builds `d3d9` and `d3d8`, which is now wanted: `d3d9`
is what `ppc64le/dxvk/interfaces_d3d9.json` describes and `dlls/d3d9` will
serve. Upstream's `-Wpsabi` notes on d3d9's small-aggregate returns are quieted
at the source (`-Wno-psabi`) rather than by not compiling the files that emit
them, because a Wine build with upstream diagnostics in it is a build whose
warnings nobody reads.

The libraries are consumed from the meson **build** layout on purpose: meson
bakes `DT_RUNPATH=$ORIGIN/../dxgi` into `libdxvk_d3d11.so` there and strips it
on install. `dlls/d3d11/unix.c` **realpaths the symlink before the dlopen**,
because glibc expands `$ORIGIN` from the path an object was loaded by and not
from its realpath — loading through the symlink in `dlls/d3d11/` would look for
the dxgi half in `dlls/dxgi/`. `DXVK_LIB_DIR` survives as an override for
pointing at a scratch build.

### What the build needs

Beyond Wine's own dependencies: `meson`, `ninja`, `glslang`, and **SDL2 or
GLFW development files**. Upstream's `meson.build` errors out without one of
them even though this lane presents through win32u and uses neither; the patch
series adds the foreign/headless WSI backend but does not remove upstream's
check. Worth a third patch if this ever bites someone.

### The POWER8 floor

Everything is built `-mcpu=power8`, so the objects run on POWER8 and POWER9
alike. `scan-isa.sh` audits built objects for instructions a POWER8 cannot
execute, and refuses to report a result unless its own five-case self-test
passes first — the assembler-based version it replaced scored 1 of 5 on that
control and still exited zero.

`[MEASURED] 2026-08-17, the test machine` — `scan-isa.sh` over the 273 objects of a
`-mcpu=power8` build: **CLEAN**, no word decoding above the floor. 43,572 words
decode at no ISA level (alignment padding, literal pools, switch tables the
compiler placed in `.text`) and 30 words in 13 runs decode above the floor but
are walled inside those, so they are data; both are counted in the output
rather than dropped quietly.

## The gates

```sh
./check-d3d11-smoke.sh              # 0 pass, 1 a check failed, 2 could not run
./check-d3d11-smoke.sh --sabotage   # prove the checks can go red
./check-present-smoke.sh            # and that a frame reaches the SCREEN
./check-present-smoke.sh --sabotage
./check-d3d9-smoke.sh               # the same standard, one API back
./check-d3d9-smoke.sh --sabotage
./check-fullscreen-smoke.sh         # ...that the frame can CHANGE SIZE, go
                                    # FULLSCREEN, and change the display MODE
./check-fullscreen-smoke.sh --sabotage
```

`probes/d3d11_smoke.c` is ONE source compiled twice — natively for ppc64le
against DXVK directly, and as an x86-64 PE run as a guest under this port —
and the claim is that their **stdout is byte-identical**. Both create a device
at feature level 11_0, clear a 64×64 render target to a colour whose 8-bit
encoding is exact, `CopyResource` it to a staging texture, `Map` it and
value-check all 4,096 texels. That is the d3d12 leg's texel-exact standard.

`check-present-smoke.sh` applies the same standard to the one claim that probe
cannot make, because no application can: that the frame is **visible**. It
starts a headless Weston of its own, runs `probes/present_smoke.c` as a guest PE
that owns a real window and presents to it, and then reads the compositor's own
framebuffer from a native ppc64le process that shares no code, no ABI and no
process with the guest. The two observers must agree on the byte. Its negative
controls include the one that matters most here — a build that presents **zero**
frames, whose own back-buffer readback still passes and whose capture must
fail — because a gate that proves rendering and calls it presentation is the
specific mistake available in this area.

No gate here ever touches a display it did not create: `$DISPLAY`,
`$WAYLAND_DISPLAY` and `$XDG_RUNTIME_DIR` are unset for every process the
present and fullscreen gates start, and replaced by a runtime directory of
their own. That matters most for `check-fullscreen-smoke.sh`, which is the one
gate in this tree that asks a program to go fullscreen and to change a display
mode — neither of which is a thing to do to somebody's desktop.

`check-present-smoke.sh` also owns the **child-window** claim. A launcher, an
in-game UI panel and an embedded video view all present into a child of their
own frame, and a child HWND is a different object to the driver — winex11 gives
it its own X window and winewayland a subsurface. The leg creates a 128x96
`WS_CHILD` inside a 256x256 parent and asks the same photograph twice: the
child's size must be there and the parent's must not, which is the exact shape
of "presented to the parent instead". [MEASURED] 2026-08-18: it already worked;
the leg exists so that it keeps working.

### Resize works, exclusive fullscreen works, and the display mode changes

`check-fullscreen-smoke.sh` drives one swapchain through four phases against a
headless weston and photographs the screen between them. All four are green now:
256x256 windowed, 192x144 after `SetWindowPos` + `ResizeBuffers`, **the whole
1024x768 screen** after `SetFullscreenState(TRUE)`, and 192x144 again after
leaving — checked BOTH on screen and in DXGI's own description of the back
buffer, because checking only the screen would pass a resize that moved the
window and left DXVK scaling the old buffer into it.

**Fullscreen was a named limitation until 2026-08-18, and this is what it was.**
[MEASURED] before the fix: `SetFullscreenState(TRUE)` returned `S_OK`,
`GetFullscreenState` agreed, `GetSystemMetrics` reported the whole screen — and
the rectangle on screen was still 192x144. The transition reached DXGI and
stopped there.

The cause was one inherited method.
`dxvk-patches/0001-foreign-wsi-backend.patch` makes
`ForeignWsiDriver::enterFullscreenMode`, `leaveFullscreenMode`, `setWindowMode`
and `resizeWindow` no-ops that report success, and says why in a comment: *the
window belongs to somebody else*, so its owner decides its geometry and DXVK
follows through `VkSurfaceCapabilitiesKHR::currentExtent`. That premise is
correct for the foreign-X11 backend, which is handed a raw XID belonging to
another process. **It is not correct for the Win32u backend**, whose window is a
Wine HWND that Wine can move — and `Win32uWsiDriver`
(`dxvk-patches/0003-win32u-wsi-backend.patch`) inherited all four unchanged. So
nothing ever asked Wine to resize the window.

**The road that did not exist has been built.**
`dxvk-patches/0004-win32u-window-ops.patch` overrides twelve methods in
`Win32uWsiDriver`, and the callback ABI it reaches Wine through
(`dxvk_win32u_wsi.h`) is at **abi 2**: six new entries beside the original five.

| new entry | lands in | what it is for |
|---|---|---|
| `window_rect` | `NtUserGetWindowRect` | what `saveWindowState` remembers and `restoreWindowState` puts back — the OUTER rect, so a decorated window does not walk down the screen by a caption's height per toggle |
| `window_set_pos` | `NtUserSetWindowPos` | the one the whole bump is for |
| `window_style` | `NtUserSetWindowLong` / `GetWindowLongW` | strip `WS_OVERLAPPEDWINDOW` on the way in and put it back on the way out; without it a fullscreen window keeps a title bar and its CLIENT area is smaller than the screen by the frame |
| `display_mode` | `NtUserEnumDisplaySettings` | the output's mode LIST, which is what `FindClosestMatchingMode1` searches |
| `display_set_mode` | `NtUserChangeDisplaySettings` | `setWindowMode` |
| `display_restore_mode` | `NtUserChangeDisplaySettings(NULL)` | `restoreDisplayMode`, on the way out of fullscreen |

**The Wine half is `dlls/d3d11/unix_wsi_window.c`**, compiled by both DXVK lanes
(`dlls/d3d9/unix_wsi_window.c` is the one-line include, the same arrangement as
`unix_win32u.c` and for the same reason). It resolves win32u's entry points with
`dlsym` on the handle a process that owns a window already has open, rather than
linking `-lwin32u` — the identical measured reason `dlls/d3d12/unix_win32u.c`
gives, a DT_NEEDED the build-tree layout cannot satisfy in a headless process.
These are the same entry points `winex11.drv` and `winewayland.drv` call from
their own unix halves.

Two things are worth knowing about that seam.

* **`NtUserSetWindowPos` sends messages**, so win32u calls back out to a window
  procedure — which on this port may be GUEST x86-64 code behind an interception
  trampoline. That road already existed: it is the one winex11 takes on every X
  event that moves a window, and the one `dlls/user32`'s callback dispatcher
  already serves for guests. What is new is entering it from inside a unixlib
  call rather than from inside a message wait, so the Wine-thread rule is
  asserted rather than assumed, by the same flag surface creation already used.
* **`w32u_window_size` now ASKS win32u and falls back to the pushed size**, where
  it used to be pushed-only. It had to change the moment DXVK could move a window
  itself: `ResizeBuffers(0, 0, ...)` right after a transition — which is what an
  application actually does there — asks DXVK for the window size, and the PE
  side has no reason to push a new one until the next present. The back buffer
  would have come out at the window's PREVIOUS size, and DXVK would have scaled
  it into the fullscreen window: right size on screen, wrong size in the buffer,
  which is exactly the half-done resize this gate's first negative control is
  about.

Doing any of this in `dlls/d3d11`'s guest-facing shim instead would have been
smaller and wrong: it would serve guests only, and it would paper over a premise
that is false one layer up rather than correcting it.

**What the ABI bump costs.** The registration entry point checks `abi` for EXACT
equality, so a `libdxvk` built from the three-patch series refuses a table
stamped 2 and this tree's DXVK refuses one stamped 1 — in both cases by name,
with the `bootstrap.sh` line to run. `>=` would be friendlier and would be
wrong: an older library accepting a newer table cannot know that the entries it
does not read are the ones the caller is relying on, and the failure would then
be a swapchain that reports fullscreen and does not go there, which is the exact
bug the bump exists to fix. In this tree both halves are built by the same
`make`, so the cost is paid only by an out-of-tree DXVK.

`ChangeDisplaySettingsExW` was a third case, recorded as **unproven rather than
broken**, and it is proven now. Why nobody could see it work is worth keeping:
[MEASURED] `win32u` synthesises a virtual mode list for any display whose driver
reports a single mode (`dlls/win32u/sysparams.c` `get_virtual_modes`), and the
smallest entry in its table is 640x480 — so on the **640x480** weston this gate
used to start, the whole list was three modes that were all 640x480 and no mode
change could ever be requested. The gate starts a **1024x768** one now, the same
code offers 640x480, 800x600, 960x540 and 1024x768, and a second probe built
with `-DFS_MODE_SWITCH=1` (so its swapchain carries
`DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH`, without which DXGI deliberately asks
for the mode the display is already in) drives a real change through DXGI and
requires the screen metrics to follow it in and come back out.

That leg takes no photograph, deliberately. Wine EMULATES a mode change on a
display whose driver has one real mode, so the compositor's own framebuffer
looks the same either way and the screen METRICS are the only place the change
is unambiguous. Splitting the two claims across two runs is what keeps each of
them a single fact.

**The other display driver was checked by hand, once, and it says something
different about the mode.** The gate has a compositor of its own and never
touches a display it did not create, so this is a confirmation rather than a
leg. [MEASURED] 2026-08-18, the same `-DFS_MODE_SWITCH=1` probe against the
workstation's real X11 session (`winex11`, Xwayland, 1920x1080, **28** modes):
fullscreen works there too — `winrect=0,0,1920,1080`, back buffer 1920x1080,
`GetFullscreenState` agreeing, and `SetFullscreenState(FALSE)` putting the
window back to `32,32,224,176` on its own. The **mode** does not change:
`asked=320x200 rc=0 after=1920x1080`. So `ChangeDisplaySettingsExW` crossed the
boundary, winex11 reported `DISP_CHANGE_SUCCESSFUL`, and the X screen stayed
the size it was — a display answer rather than a boundary one, and an Xwayland
one specifically, since the root window's size belongs to the Wayland
compositor rather than to the X client asking. The probe correctly failed its
own mode assertion there, which is what a strict assertion is for. The display
was left at 1920x1080, as `FINAL screen=` records on every exit path.

**The negative control is a lever on the fix, not on the gate.**
`WINEDXVKNOWINDOWOPS=1` makes the Wine side publish the callback table with its
six abi-2 entries NULL. DXVK's driver checks every one of them before calling it
and falls back to the no-op it used to inherit, so the port behaves exactly as it
did before this work — and `--sabotage` requires the main leg to go red on the
rectangle and the mode leg to go red on the display mode. A gate whose only
negative control switched off its own assertions would prove nothing about the
code.

## Layout

| Path | |
|---|---|
| `bootstrap.sh` | reconstructs `src/` at the pinned commit with the series applied |
| `build-for-wine.sh` | the make↔meson seam, called by the generated Makefile |
| `gen_interfaces.py` | DXVK's headers → `interfaces_dxvk.json`, the ONE roster |
| `gen_winecom.py` | that roster → `dlls/d3d11/d3d11_marshal.h`; `--report` explains every refusal |
| `dxvk_flat_surface.h` | declarations for the flat exports Wine ships but does not declare |
| `scan-isa.sh` | audits built objects for post-ISA-2.07 instructions |
| `check-d3d11-smoke.sh` | the offscreen gate |
| `check-present-smoke.sh` | the on-screen gate: two observers, one colour |
| `check-d3d9-smoke.sh` | the D3D9 offscreen gate |
| `probes/present_smoke9.c` | the D3D9 on-screen probe, the present gate's second leg |
| `dxvk_flat_surface_d3d9.h` | the four D3D9 flat exports Wine declares nowhere |
| `dxvk_win32u_wsi.h` | the ONE copy of the WSI callback ABI (abi 2), compiled by both sides |
| `check-fullscreen-smoke.sh` | resize, exclusive fullscreen and the display mode |
| `probes/fullscreen_smoke.c` | one source, two builds: the plain leg and `-DFS_MODE_SWITCH=1` |
| `interfaces_d3d9.json` | the D3D9 roster: 21 interfaces, 497 slots |
| `dxvk-patches/` | our changes to DXVK, as a revertible series |
| `probes/` | the gates' probes, native and guest from one source |

## Licence

**DXVK is zlib/libpng** — © Philip Rebohle, Joshua Ashton, Robin Kertels,
Jeffrey Ellison. Permissive: use, modify and redistribute freely, provided you
don't misrepresent the origin, mark altered versions as altered, and keep the
notice. See `src/LICENSE` after bootstrapping. Our changes are a **patch series
against a pinned upstream commit** rather than a fork, so the origin is
unambiguous and the licence travels with the source.

A DXVK checkout also vendors other licences — `dxbc-spirv`, `libdisplay-info`,
OpenVR, and the SPIR-V and Vulkan headers (Apache-2.0, which unlike zlib
carries patent-grant terms). Worth reviewing if you redistribute **binaries**
rather than source.

The Wine side of this work is **LGPL-2.1+**, which places real obligations on
anyone distributing modified binaries. That is the one to watch, not DXVK's.
