# Native ppc64le D3D12 for emulated x86-64 games — feasibility

What it would take to do for **D3D12** what `dxvk-ppc64le` did for D3D11: run the
translation layer as native ppc64le ELF called from emulated x86-64 Windows
binaries under `fastppcx86`, instead of emulating the translation layer too.

Research only. Nothing was built, installed, or modified. Every number below
marked `[MEASURED]` was produced by a read-only script over a file in this tree
and is reproducible; scripts are in the scratchpad paths named at each site.

**Evidence markers used throughout:**

| | |
|---|---|
| `[MEASURED]` | I ran something read-only here and this is the output |
| `[VERIFIED-LOCAL]` | I read the actual file in this tree |
| `[VERIFIED-UPSTREAM]` | a subagent read the actual upstream file/commit, URL given |
| `[INFERRED]` | reasoning from the above, not directly observed |
| `[UNVERIFIED]` | reported, not confirmed — see §7 |

**In one screen:**

- The brief's central worry is **wrong in the favourable direction**.
  vkd3d-proton builds as a native Linux ELF `.so` today and three CI jobs compile
  that target on every push. Nothing needs to be un-Windows-ified: pthreads is its
  native path and Win32 is the `#ifdef _WIN32` shim. **§1**
- **Use vkd3d-proton, not Wine vkd3d.** Wine vkd3d also builds native, but it is
  FL 12_1 / SM 6.0 / no DXR / no mesh shaders / no VRS / no enhanced barriers —
  it cannot run modern D3D12 games at all. **§1, §2**
- The D3D12 surface is **the same size as the one already done**: 118 interfaces
  and 2,604 vtable slots versus D3D11's 111 and 2,593 — and 43 of those interfaces
  are the DXGI ones the D3D11 work already generated. **§4.5**
- The four D3D12-specific worries in the brief mostly **do not bite**: descriptor
  handles work because handles are host pointers and FEX shares an address space;
  `Map` is already answered; multithreaded recording needs no boundary barrier.
  **§4.1, §4.2, §4.4**
- Three things are **genuinely new work**: fences/`SetEventOnCompletion` (a hard
  FEX limit, with a designed way around it), by-value aggregate returns (which the
  generator correctly refuses today), and the DXR state-object graph. **§4.3,
  §4.7, §4.8**
- The **unixlib alternative is not reachable**: this Wine has no PE modules at all
  (`PE_ARCHS` is empty), and both routes to an emulated x86-64 guest were
  investigated by this project and got a "No" verdict. But **one property of it is
  worth stealing now** — the args-struct transport, which makes §4.7 disappear.
  **§5**
- **§7 is the section to read second.** It is where everything unproven lives.

---

## 1. Verdict on the native-build question

**The premise in the brief is refuted, and refuted in the favourable
direction.** The working belief was that vkd3d-proton is mingw/PE-only with the
native path removed. That is wrong.

### vkd3d-proton builds native Linux ELF today, and CI proves it cannot bitrot

`[VERIFIED-UPSTREAM]` `meson.build` treats `linux` as a peer of `windows`, not
as a legacy branch:

```meson
if vkd3d_platform == 'linux' or vkd3d_platform == 'android'
  lib_dl           = vkd3d_compiler.find_library('dl')
  vkd3d_extra_libs = [ lib_dl, threads_dep ]
elif vkd3d_platform == 'windows'
  ...
else
  error('Unknown platform')
```

<https://github.com/HansKristian-Work/vkd3d-proton/blob/master/meson.build>

- Native output is **`libvkd3d-proton-d3d12.so`** (`libs/d3d12/meson.build`,
  `d3d12_name_prefix = 'libvkd3d-proton-'` on non-Windows), plus
  `libvkd3d-proton-d3d12core.so`. Confirmed independently by
  `include/vkd3d_sonames.h`, which has an `#elif defined(__linux__)` arm naming
  exactly those sonames.
- The Linux arm *installs* a pkg-config file and public headers — it is a
  supported consumer surface, not a developer toy build.
- `package-release.sh --native` invokes `build_arch 64` with **no `--cross-file`
  at all**.
- `.github/workflows/test-build-linux.yml` runs `build-native-gcc-x86`,
  `build-native-gcc-x64` and `build-native-clang-x64` on every push, all plain
  `meson setup --buildtype release`. **This is the load-bearing fact**: a native
  target that three CI jobs compile on every push cannot silently rot, which is
  the failure mode that would have made "it builds" worthless.

The history that generated the mistaken belief is real but means the opposite of
what it looks like. `enable_standalone_d3d12` (added `cbebf9ef`, 2020-07-02)
never gated ELF-vs-PE; it gated whether to build the standalone `d3d12.dll`
front-end. It was deleted in
[`065f2fc6`](https://github.com/HansKristian-Work/vkd3d-proton/commit/065f2fc6484cfa49f6dd4f0b0c6f3eca58f3cc32)
(2022-11-14) — `vkd3d-utils: Remove it.` — whose own commit message is a move
*toward* the native `.so`:

> With recent improvements, vkd3d-utils is no longer meaningful and **we can
> export a single .so on Linux.** … On Linux, **libvkd3d-proton-d3d12.so** is
> exported rather than libd3d12.so as that would easily conflict with e.g. WSL.
> Follow similar logic as DXVK here.

What died in that commit was the *upstream-vkd3d-compatible C API*
(`vkd3d_create_instance`/`vkd3d_create_device`), not native linking. A search of
`git log --all -S"ENABLE_NATIVE"` returns nothing; no such option ever existed.

**The one caveat, stated by upstream itself** `[VERIFIED-UPSTREAM]`, README:
"A native Linux binary can be built, but it is not intended to be compatible
with upstream Wine. A native option is mostly relevant for development purposes
for the time being." So: it builds, it is CI-tested, it is not the shipping
artifact, and there is no ABI-stability promise. That is *the same position DXVK's
native path was in* when this project adopted it, and it worked.

### The Windows-dependency polarity is inverted from the brief's assumption

The brief asks how much Win32 removal would cost. There is nothing to remove —
**pthreads is the native path and Win32 is the shim** `[VERIFIED-UPSTREAM]`:

- `include/private/vkd3d_threads.h`: `#if defined(_WIN32)` *implements*
  `pthread_create`/`pthread_mutex_t`/`pthread_cond_t` on top of
  `CreateThread`/`SRWLOCK`/`CONDITION_VARIABLE`; `#else` includes `<pthread.h>`
  and uses it directly.
- `include/private/vkd3d_native_sync_handle.h`: Win32 events are the `_WIN32`
  branch; the native branch is **`eventfd`**.
- `__declspec`: exactly one occurrence, `vkd3d_common.h:313`, `_MSC_VER`-guarded
  with `#else #define VKD3D_THREAD_LOCAL __thread`.
- SEH (`__try`/`__except`/`RaiseException`): zero occurrences in `libs/` or
  `include/`.
- `CRITICAL_SECTION`: zero occurrences.
- `Interlocked*` is defined portably in `include/private/vkd3d_atomic.h` over
  `__atomic_*` GCC builtins — arch-neutral, no inline asm.
- `include/vkd3d_windows.h` is a `#if !defined(_WIN32)` shim supplying `HRESULT`,
  `GUID`, `REFIID`, `MIDL_INTERFACE`, `CONST_VTBL`, an empty `__stdcall`,
  `CONTAINING_RECORD` and a template `__uuidof` emulation.

Residual Windows coupling that *is* real: **`widl` is required even for native
builds** (`meson.build` `find_program('widl', 'widl-stable',
'widl-mingw-tools-fallback')`). Build-host tool only; Wine's `widl` builds
standalone.

### ppc64le specifics for vkd3d-proton

`[VERIFIED-UPSTREAM]`, and the aarch64 port is the template to copy:

- SSE2 non-temporal copies in `include/private/copy_utils.h` are `#ifdef __SSE2__`
  guarded with a generic `#else` fallback. No `_mm_*`/`immintrin` anywhere else.
- `__builtin_ia32_rdtsc()` (`vkd3d_common.h:344`) is guarded to i386/x86_64 with
  a `vkd3d_get_current_time_ns()` fallback.
- `-msse -msse2` go through `get_supported_arguments()` and are dropped silently
  off x86.
- **One known one-line papercut.** `include/vkd3d_windows.h:75`:
  ```c
  # if defined(__aarch64__) || defined(__x86_64__) || defined(__WIDL__)
  typedef long INT64;
  typedef unsigned long UINT64;
  # else
  typedef long long DECLSPEC_ALIGN(8) INT64;
  ```
  ppc64le falls to `long long` — same size, but produces the `-Wformat` storm
  that forced commit `5bc9ec14` for aarch64. CI compiles native with
  `-Wformat=2`. Add `defined(__powerpc64__)`.
- Commit `ad18f0cf` ("build instructions for native aarch64") added a
  `build-widl.txt` cross-file pointing at `x86_64-w64-mingw32-widl` — a directly
  reusable recipe for ppc64le.

### dxil-spirv: the best news in this document

`[VERIFIED-UPSTREAM]` vkd3d-proton consumes dxil-spirv as a meson subproject, and
**that path does not use LLVM at all** — it compiles a self-contained
hand-written bitcode reader (`-DHAVE_LLVMBC`, `bc/*.cpp`,
`third_party/bc-decoder/llvm_decoder.cpp`). The real-LLVM path is CMake-only and
`option(DXIL_SPIRV_NATIVE_LLVM ... OFF)`. There is no `dependency('llvm')` in the
meson build.

Portability: **zero** x86/SSE intrinsics and **zero** endian/byteswap handling in
dxil-spirv proper (the only `endian` hits are in vendored SPIRV-Tools *tools*,
not the library). Float bit-punning is `memcpy`-based. The bitstream reader
(`third_party/bc-decoder/llvm_bitreader.h:145-155`) fills a byte scratch buffer
then `memcpy`s into the integer — correct on little-endian, would byte-swap on
big-endian. **ppc64le is little-endian, so this is a non-issue for the target**,
and would be a genuine blocker for ppc64 BE. (The LSB-first fill order is
`[INFERRED]` from the `memcpy` idiom; `ReadBits`' full body was not traced.)

### Wine vkd3d also builds native — and is not a candidate anyway

`[VERIFIED-UPSTREAM]` (via <https://codeberg.org/vkd3d/vkd3d>, whose `main` tip
`9a673d0b` is byte-identical to `gitlab.winehq.org/wine/vkd3d` HEAD; the WineHQ
GitLab web UI is behind an anti-bot wall). Autotools, `lib_LTLIBRARIES =
libvkd3d-shader.la libvkd3d.la libvkd3d-utils.la`, version-scripted, `dlopen`s
Vulkan by soname. Native ELF is its *primary* mode.

It is nevertheless the wrong choice, for reasons that are not close:

| | Wine vkd3d 2.0 | vkd3d-proton |
|---|---|---|
| Max feature level | **12_1** (`device.c:1489-1508`, no path to 12_2) | 12_2 |
| Max shader model | **6.0** (`device.c:3691` clamps `HighestShaderModel`) | 6.6+ |
| DXR | `CreateStateObject` → `E_NOTIMPL`; `DispatchRays`, `BuildRaytracingAccelerationStructure`, `SetPipelineState1` all `FIXME(... "stub!")` | implemented |
| Mesh shaders | `MeshShaderTier = NOT_SUPPORTED`; `DispatchMesh` is a stub | implemented |
| VRS | `RSSetShadingRate`/`RSSetShadingRateImage` stubs | implemented |
| Enhanced barriers | `EnhancedBarriersSupported = FALSE` (`device.c:4074`) | implemented |
| Highest device iface | `ID3D12Device9` (QI, `device.c:3093`) | higher |
| Highest cmd list | `ID3D12GraphicsCommandList6` (`command.c:2516`) | higher |
| DXGI | **none at all** — README: "neither libvkd3d nor libvkd3d-utils implement any DXGI interfaces" | none, but designed to sit on DXVK's |

I confirmed the DXR/mesh stubs `[VERIFIED-LOCAL]` in the copy Wine vendors at
`wine-upstream/libs/vkd3d/libs/vkd3d/command.c:6406-6427` — five consecutive
`FIXME(... "stub!")` bodies for `DispatchRays`, `RSSetShadingRate`,
`RSSetShadingRateImage`, `DispatchMesh`, `SetPipelineState1` — and
`device.c:1850` `RaytracingTier = D3D12_RAYTRACING_TIER_NOT_SUPPORTED`,
`device.c:3948` `MeshShaderTier = D3D12_MESH_SHADER_TIER_NOT_SUPPORTED`.

vkd3d-proton's own README states the split in primary text: "a fork of VKD3D,
which aims to implement the full Direct3D 12 API… serves as the development
effort for Direct3D 12 support in Proton… Backwards compatibility with the vkd3d
standalone API is not a goal."

The one part of Wine vkd3d that is genuinely excellent and worth remembering
separately is **`libvkd3d-shader`**: a mature in-tree HLSL/DXBC/DXIL → SPIR-V
compiler (`dxil.c` alone is 11,933 lines `[MEASURED]`) with no external
`dxcompiler` dependency. It is not needed here — vkd3d-proton has dxil-spirv —
but it is a second independent native-buildable DXIL front end, which is a real
risk reducer.

---

## 2. Recommendation

**vkd3d-proton, native ELF, no fork.** Same shape as the DXVK work: pin a
commit, carry a patch series rather than a fork, build with
`meson setup --buildtype release` and no cross file.

Reasoning, in the order that actually decided it:

1. **It is the only one of the two that can run modern D3D12 games.** Wine vkd3d
   is FL 12_1 / SM 6.0 / no DXR / no mesh / no VRS / no enhanced barriers. Any
   title from roughly 2021 onward that this project would want to demonstrate is
   out of reach. That alone settles it.
2. **The native path is CI-tested on every push**, so the thing this project
   depends on (upstream keeping a non-Windows build alive) is defended by
   upstream's own infrastructure rather than by our patches.
3. **The porting delta is small and has a precedent to copy** — one `INT64`
   typedef line, a `build-widl.txt`, and the aarch64 commits as a worked
   example.
4. **The bottom of the stack is clean.** No LLVM, no SSE, no endian assumptions
   in dxil-spirv; guarded SSE and guarded `rdtsc` in vkd3d-proton itself.

The cost of that choice is the Vulkan floor: vkd3d-proton requires **Vulkan
1.3**, ≥1,000,000 UpdateAfterBind descriptors, `VK_EXT_robustness2` with
`nullDescriptor` (`device.c:3448` hard-errors without it — *"required for correct
operation"*), and `VK_KHR_push_descriptor`. Wine vkd3d asks for Vulkan 1.0 and
three device extensions. On this hardware that trade is free — see §6.

**This recommendation is about *which library*, and it is independent of *which
boundary*.** §5 evaluates putting D3D12 on Wine's unixlib boundary instead of a
FEX thunk. That option is real, it eliminates the residual ABI unknown in this
document, and it is blocked on work that has not happened — but it does not change
the answer here. Both boundaries want vkd3d-proton, and taking Wine's own vkd3d
along with Wine's boundary is the one combination that should be rejected
outright.

---

## 3. What transfers from the D3D11 work, and what does not

### Transfers essentially unchanged

**The whole architecture.** `game.exe → PE shim → guest ELF stub → FEX thunk →
native .so` is unaffected by which API sits at the far end.

**The three-function boundary.** `thunk/runtime/dxvk_thunk_abi.h:96-126` keeps the
crossing at `dxvk_thunk_call` / `dxvk_thunk_call_float` / `dxvk_thunk_call_entry`
precisely because FEX's host trampoline allocator is a bump allocator with no
free path (`Thunks.cpp:431-484`). That constraint is identical for D3D12 and the
solution transfers verbatim.

**The proxy design.** `thunk/runtime/dxvk_proxy.h:24-29` — one static vtable per
*interface type*, `Proxy{vtbl, host, refs, iface}` as plain data, `AddRef`/
`Release` served guest-side and never crossing. D3D12 has *more* per-frame object
churn than D3D11 (command allocators, lists, fences), so this matters more, not
less.

**The DXGI half is already done.** The D3D11 work's 111 interfaces / 2,593 slots
already include the full `dxgi.h`…`dxgi1_6.h` set — 43 interfaces and 819 vtable
slots by my count of the same surface in Wine's IDL `[MEASURED]`. D3D12 needs
DXGI 1.4+ for `IDXGISwapChain3::GetCurrentBackBufferIndex` and friends, and every
one of those interfaces is already generated, marshalled and swapchain-override'd.
The 29 swapchain overrides in `pe-shim/swapchain_slots.inc` and the HWND↔XID map
apply unchanged.

**The `ms_abi` forwarder machinery.** Defect A1's fix — separate SysV and MS-x64
vtable arrays per interface, `dxvk_thunk_vtable_for(iface, abi)`
(`dxvk_proxy.h:79-87`) — is API-independent and is the single most valuable thing
already built, since it is the part that would otherwise fail invisibly.

**The `Map`/`Unmap` argument, in full.** `docs/hazard-hunt.md:620-712` establishes
`[CODE]` that fastppcx86's thunk opcode lowers to a plain `bctrl` in
`FEXCore/.../PPC64LE/BranchOps.cpp` `DEF_OP(Thunk)` — an ordinary ELFv2 indirect
call on the same hardware thread. A core observes its own stores in program
order, so *no barrier is owed at the boundary at all*. This is the deepest result
in the D3D11 work and it transfers to D3D12 without modification. See §4.2 and
§4.4.

**Struct-pass-by-pointer.** `gen_layout_check.py` proved all 296 D3D11/DXGI
aggregates member-offset-identical. The method generalises; §4.6 has the D3D12
answer.

**`scan-isa.sh` and the POWER8 floor discipline.** Unchanged, and the
`libstdc++.a` finding in `QUEUE.md` §F applies identically — vkd3d-proton is C,
so if anything it is easier.

### Does not transfer

**`gen_interfaces.py` is wrong on D3D12 headers, and I proved it rather than
guessed.** `[MEASURED]` Running the unmodified generator against DXVK's own
vendored `src/include/native/directx/d3d12.h`:

```
ID3D12DescriptorHeap -- 14 vtable slots
   ...
   8   D3D12_DESCRIPTOR_HEAP_DESC* GetDesc(D3D12_DESCRIPTOR_HEAP_DESC *__ret)
   9   D3D12_DESCRIPTOR_HEAP_DESC  GetDesc(void)
  10   D3D12_CPU_DESCRIPTOR_HANDLE* GetCPUDescriptorHandleForHeapStart(D3D12_CPU_DESCRIPTOR_HANDLE *__ret)
  11   D3D12_CPU_DESCRIPTOR_HANDLE  GetCPUDescriptorHandleForHeapStart(void)
  12   D3D12_GPU_DESCRIPTOR_HANDLE* GetGPUDescriptorHandleForHeapStart(D3D12_GPU_DESCRIPTOR_HANDLE *__ret)
  13   D3D12_GPU_DESCRIPTOR_HANDLE  GetGPUDescriptorHandleForHeapStart(void)
```

The true vtable is **11 slots**. The generator emits **14**.

Cause `[VERIFIED-LOCAL]`, `d3d12.h:2682-2694`: widl wraps every struct-returning
method in `#ifdef WIDL_EXPLICIT_AGGREGATE_RETURNS` / `#else` / `#endif`, emitting
a `virtual … = 0;` declaration in *both* arms. `gen_interfaces.py` is a regex over
raw text — it does not preprocess — so `METHOD_RE` (`gen_interfaces.py:53-56`)
matches both and counts one slot twice. Every slot after the first such method in
that interface is off by one, and the script's own docstring names exactly this
failure: *"Getting it wrong does not fail to compile, it dispatches to the
neighbouring method with the neighbour's argument types."*

Scale `[MEASURED]`, over Wine's `d3d12.idl` + `d3d12sdklayers.idl` + `dxgi*.idl`:
**22 of 118 interfaces** are affected, **47 phantom slots** total. The worst are
not obscure: `ID3D12Device` through `Device10` (+2 to +4 each, from
`GetResourceAllocationInfo`/`GetCustomHeapProperties`/`…1`/`…2`),
`ID3D12DescriptorHeap` (+3), `ID3D12Resource`/`Resource1`/`Resource2`. Those are
the three hottest interfaces in the API.

The **fix** is mechanical — strip `#ifdef WIDL_EXPLICIT_AGGREGATE_RETURNS` blocks
(or preprocess properly) before matching. The **consequence** is not; see the next
item.

**`gen_thunk.py`'s transport does not carry aggregate returns, and it knows.**
`[VERIFIED-LOCAL]` `gen_thunk.py:14-21` states assumption 2 explicitly —
*"NOTHING in the surface returns an aggregate by value"* — and
`check_invariants()` at `gen_thunk.py:519-528` **fails generation** on any return
type not in `BYVAL_INTEGER`. It fails closed, which is the right behaviour and
means this cannot ship broken silently. But it means D3D12 stops the generator on
day one.

`[MEASURED]` The D3D12 surface has **15 method instances that return a struct by
value** (I separated these from the 7 that return enums, which widl does not treat
as aggregates and which need nothing):

| return type | methods |
|---|---|
| `D3D12_RESOURCE_ALLOCATION_INFO` (16 B) | `Device::GetResourceAllocationInfo`, `Device4::…1`, `Device8::…2` |
| `D3D12_CPU_DESCRIPTOR_HANDLE` (8 B) | `DescriptorHeap::GetCPUDescriptorHandleForHeapStart` |
| `D3D12_GPU_DESCRIPTOR_HANDLE` (8 B) | `DescriptorHeap::GetGPUDescriptorHandleForHeapStart` |
| `D3D12_RESOURCE_DESC` / `_DESC1` | `Resource::GetDesc`, `Resource2::GetDesc1` |
| `D3D12_DESCRIPTOR_HEAP_DESC` (16 B) | `DescriptorHeap::GetDesc` |
| `D3D12_COMMAND_QUEUE_DESC` (16 B) | `CommandQueue::GetDesc` |
| `D3D12_HEAP_DESC` | `Heap::GetDesc` |
| `D3D12_HEAP_PROPERTIES` (20 B) | `Device::GetCustomHeapProperties` |
| `D3D12_PROTECTED_RESOURCE_SESSION_DESC` / `…1` | ×2 |
| `D3D12_SHADER_CACHE_SESSION_DESC` | `ShaderCacheSession::GetDesc` |
| `D3D12_DEVICE_CONFIGURATION_DESC` | `DeviceConfiguration::GetDesc` |

This needs a **fourth boundary function** (`dxvk_thunk_call_sret` or equivalent),
in the style of `DXVK_FSHAPE_*` for the five float methods
(`dxvk_thunk_abi.h:32-46`) — but simpler than that, because §4.7 establishes from
Microsoft's ABI documentation that **all 15 use the same shape**: a COM method is
a non-static member function, so even the 8-byte descriptor handles go through a
caller-allocated buffer rather than `RAX`. One new transport, not a table of
shapes. Bounded work; new work.

`hazard-hunt.md:358-364` is worth quoting against itself here. Its argument for
why the clang-vs-MSVC ABI risk was small was a census: *"The shapes at risk in a
clang-vs-MSVC divergence are the ones this surface does not have: `__m128`/vector
arguments (none), by-value aggregates of size 3/5/6/7 or >8 (none), **by-value
aggregate returns (none)**, and variadics (none)."* **D3D12 has by-value
aggregate returns.** The residual-risk argument that made D3D11 safe does not
cover D3D12, and the ABI question it deferred now has to be answered rather than
sidestepped. See §4.7 and §7.

**The WCHAR shim becomes unnecessary — and this is the second-best news here.**
`[VERIFIED-LOCAL]` DXVK's native `include/native/windows/windows_base.h:30`
hardcodes `typedef wchar_t WCHAR;` — 4 bytes on Linux — which is why the D3D11
work needed `dxvk_wchar_shim.cpp` (516 lines) over 36 slots, installed
conditionally on caller ABI (defect A4).

vkd3d-proton makes the opposite choice. `[VERIFIED-UPSTREAM]`
<https://github.com/HansKristian-Work/vkd3d-proton/blob/master/include/vkd3d_windows.h>:

```c
typedef unsigned short WCHAR;
typedef const WCHAR* LPCWSTR;
```

**2 bytes, matching PE.** So the entire WCHAR conversion problem — 516 lines of
shim, 36 slots, `wchar_slots.inc`, and the ABI-conditional vtable patching that
defect A4 was about — **does not arise for D3D12 on vkd3d-proton**. Guest strings
pass through as pointers to memory the host reads with the same character width.

This matters far beyond `SetName`. `[MEASURED]` of 222 aggregates in `d3d12.idl`,
**11 contain `const WCHAR *` members**, and they are `D3D12_EXPORT_DESC`,
`D3D12_HIT_GROUP_DESC`, `D3D12_SUBOBJECT_TO_EXPORTS_ASSOCIATION`,
`D3D12_DXIL_SUBOBJECT_TO_EXPORTS_ASSOCIATION`, the meta-command descs, and the
DRED breadcrumb nodes — i.e. almost entirely the **DXR state-object graph**,
which is §4.8's problem and which this result substantially defuses.

---

## 4. The hazards

### 4.1 Descriptor heaps and raw pointer arithmetic — **NOT A PROBLEM**

The concern: applications compute `handle.ptr + i * GetDescriptorHandleIncrementSize(type)`
without any interceptable call. If handles are host pointers, does that work?

**Yes, and it is the design both implementations already use.**
`[VERIFIED-LOCAL]`, Wine vkd3d `libs/vkd3d/resource.c:4236` and `:4250`:

```c
descriptor->ptr = (SIZE_T)heap->descriptors;              /* CPU handle */
descriptor->ptr = (uint64_t)(intptr_t)heap->descriptors;  /* GPU handle */
```

and `device.c:4188-4209` returns `sizeof(struct d3d12_desc)` (16 bytes: a union
pointer + two `unsigned int`, `vkd3d_private.h:662-676`) as the increment. The
handle *is* a host pointer into a host array, and the increment is the honest
stride of that array.

Why this works under FEX, and it is the same fact `Map` rests on:
`thunk/runtime/dxvk_proxy.h:12-14` — *"FEX shares an address space with the
guest, so this is a real pointer."* The guest performs **arithmetic** on the
handle, never a dereference; it hands the computed value straight back through
`CreateShaderResourceView(desc, handle)` or `SetGraphicsRootDescriptorTable`. The
host receives a value it produced, in its own address space, and uses it directly.
`D3D12_CPU_DESCRIPTOR_HANDLE.ptr` is `SIZE_T` — 8 bytes on guest x86-64 and on
ppc64le alike.

Three things that would break it, none of which apply:

- A 32-bit guest. Host VAs on ppc64le live at `0x3fff'xxxx'xxxx` and would
  truncate — `Thunks.cpp:437-464` handles exactly this for trampolines. D3D12 is
  not formally 64-bit-only, but shipping D3D12 titles are x86-64 and this
  project's target is the x86-64 guest, so it does not arise. A 32-bit D3D12
  guest would break descriptor handles outright, not subtly.
- Reporting a fake increment size while the host array has a different stride.
  Neither implementation does this.
- The guest *dereferencing* the handle. That is undefined in D3D12 and no
  correct application does it.

Contrast this with `ID3D12Resource::GetGPUVirtualAddress`, which returns a
`D3D12_GPU_VIRTUAL_ADDRESS` (a `UINT64` typedef) that games *also* do arithmetic
on. Same analysis, same answer, and it is a plain scalar so it does not even need
the aggregate-return work.

### 4.2 `ID3D12Resource::Map` — **NOT A PROBLEM**, already answered

`[VERIFIED-LOCAL]` `docs/hazard-hunt.md:620-712` answers the D3D11 equivalent
(`QUEUE.md` B2) with `[CODE]` citations and the answer is **no barrier is owed**,
for a reason that is a property of the emulator rather than of D3D11: the FEX
thunk opcode lowers to `bctrl`, so the crossing never leaves the hardware thread
that executed the guest's stores.

The D3D11 analysis then enumerated the three consumers that *are* on another
thread and showed each is already ordered. For D3D12 the enumeration changes but
the conclusion does not:

1. **DXVK's CS thread** — does not exist in D3D12. D3D12 has no deferred context
   and no implicit CS thread; `Unmap` is essentially a no-op and the data is
   carried by the next `ExecuteCommandLists`. **Fewer** cross-thread consumers,
   not more.
2. **The GPU** — `vkQueueSubmit` performs the availability domain operation for
   everything that happens-before it, and the guest's stores happen-before the
   guest's own `ExecuteCommandLists` call on the same thread. Unchanged.
3. **Another guest thread** — fastppcx86's TSO obligation, not the thunk's
   (`DEF_OP(StoreMemTSO)` emits `lwsync` before every store). Unchanged, and it is
   the thing a fast-mode configuration with TSO disabled would break, silently,
   for far more than `Map`.

The one genuinely `Map`-specific residual from the D3D11 analysis carries over
verbatim and gets *worse* in D3D12: **unaligned atomic RMW inside a mapped
buffer.** `larx`/`stcx.` require natural alignment; x86 permits split locks. A
game doing a `lock`-prefixed op on an unaligned address inside memory we handed it
is a `SIGBUS` on POWER unless fastppcx86 emulates split locks. D3D12 games use
persistently-mapped upload heaps far more aggressively than D3D11 games and are
more likely to run lock-free ring buffers in them. Rare, but no native barrier can
fix it — it is an emulator-fidelity question.

### 4.3 Fences and `SetEventOnCompletion` — **REAL, and the hardest item here**

This is the one hazard with a genuine architectural obstacle rather than a
generator change.

`[VERIFIED-LOCAL]` `docs/fex-thunk-architecture.md:635-655`, quoting
`Thunks.cpp:223-225`:

```cpp
static void CallCallback(void* callback, void* arg0, void* arg1) {
  if (!ThreadObject) {
    ERROR_AND_DIE_FMT("Thunked library attempted to invoke guest callback asynchronously");
  }
```

`ThreadObject` is `static thread_local` (`Thunks.cpp:143`), set only on threads
FEX created and is currently running guest code on. FEX's own README says the same
in prose: host→guest *"is only possible while handling a Guest → Host call."*
The architecture document's verdict: *"A host library that stores a guest callback
and invokes it later from its own worker thread hits a fatal error, not a wrong
answer."*

Now the D3D12 shape. `ID3D12Fence::SetEventOnCompletion(value, hEvent)` takes a
**Win32 event HANDLE the game created itself**, and the runtime signals it from a
worker thread when the GPU reaches the value. `[VERIFIED-LOCAL]` Wine vkd3d
parameterises exactly this through a function pointer —
`include/vkd3d.h:112` `typedef HRESULT (*PFN_vkd3d_signal_event)(HANDLE event);`,
supplied by Wine's PE shim at `dlls/d3d12/d3d12_main.c` as a `SetEvent` wrapper —
and `libs/vkd3d/command.c:1126-1158` calls `fence->device->signal_event(event)`.
If that function pointer were a guest pointer and the caller were a host worker
thread, FEX would `ERROR_AND_DIE`.

So the naive translation of the design is a hard crash, not a slowdown.

**What it would have to be instead**, and this is a design, not a measurement:
invert the direction so every crossing is guest→host and synchronous.

- The PE shim implements `SetEventOnCompletion` itself: it records
  `(fence, value, hEvent)` and does **not** pass `hEvent` across.
- One **guest-side** service thread blocks in a thunked host call — a
  "wait until any registered fence reaches its value" primitive backed by
  vkd3d-proton's native sync handle (`eventfd`, per
  `include/private/vkd3d_native_sync_handle.h`).
- When that call returns, the service thread — a guest thread, running guest
  code, inside a guest→host call's return — calls Wine's `SetEvent(hEvent)`
  guest-side. No host→guest callback is ever made.

Cost: one guest thread parked in a blocking thunk call, plus one wake per fence
completion. Games typically have 2–3 frames in flight, so the registration table
is tiny. Risk: a blocking host call holds a FEX guest thread; whether that
interacts badly with FEX's thread management is **unknown** and is on the list in
§7.

I do **not** yet know what vkd3d-proton's native build expects `HANDLE` to be for
this method — whether it is an eventfd it owns or whether there is Win32 event
emulation. That is `[UNVERIFIED]` and named in §7.

### 4.4 Heavy multithreaded command-list recording — **NOT A PROBLEM for correctness**

The brief asks where the boundary needs barriers for correctness rather than
performance. The answer is **nowhere**, and it follows from the same `bctrl` fact
as §4.2 — but the reasoning has to be redone because D3D11's answer leaned on
DXVK's `LockContext()`, and D3D12 deliberately has no such lock.

Take the worst realistic case: guest thread A records a command list, hands it to
guest thread B, B calls `ExecuteCommandLists`.

- **A's recording.** Every `ID3D12GraphicsCommandList` method A calls is a
  synchronous `bctrl` on A's own core. vkd3d's stores into the command allocator
  are host stores on A's core. A core observes its own stores in program order.
  Nothing owed.
- **A → B.** This is the game's own synchronization — a mutex, a fence, an
  atomic. It is guest code, and fastppcx86 lowers it: `DEF_OP(StoreMemTSO)` emits
  `lwsync` before every store, `DEF_OP(LoadMemTSO)` after every load,
  `DEF_OP(Fence)` maps `LoadStore` to `hwsync`. A's release barrier runs on A's
  core and orders *everything* A's core did — including the host stores vkd3d made
  on A's behalf, because those are stores by the same core. B's acquire pairs with
  it. **This is the emulator's obligation and it discharges it for host stores as
  a side effect of discharging it for guest stores**, because they are the same
  core's stores.
- **B's submit.** `vkQueueSubmit` orders everything happens-before it for the GPU.

The one configuration that breaks this is fastppcx86 with TSO emission disabled —
and that breaks vastly more than D3D12, as `hazard-hunt.md:695-703` already notes.

**Performance is a different question and is not answered here.** D3D12's whole
point is many threads recording concurrently, and each recorded command is now a
thunk crossing. The D3D11 work never measured crossing cost under multi-thread
contention. Note in mitigation that D3D12 issues *fewer* API calls per frame than
D3D11 for the same scene — state is baked into PSOs and root signatures rather
than set per-draw — so total crossings may well go *down*. That is `[INFERRED]`
and unmeasured either way.

The falsification harness `hazard-hunt.md:688-698` specifies (an MP litmus across
the boundary, with a negative control that must fire before a null result means
anything) applies unchanged and is still unbuilt.

### 4.5 Interface surface size — **NOT A PROBLEM, and smaller than expected**

`[MEASURED]`, parsing Wine's IDL (`scratchpad/count_idl.py`; validated against
three independently-known vtable sizes — `ID3D12GraphicsCommandList` = 60,
`ID3D12Device` = 44, `ID3D12Resource` = 15 — all exact):

| surface | interfaces | own methods | total vtable slots |
|---|---|---|---|
| **D3D11 + DXGI (already done)** | 111 | — | **2,593** |
| `d3d12.idl` | 62 | 240 | 1,645 |
| `dxgi.idl` … `dxgi1_6.idl` | 43 | 151 | 819 |
| `d3d12sdklayers.idl` (debug layer, optional) | 12 | 56 | 140 |
| **D3D12 + DXGI + sdklayers** | **118** | **447** | **2,604** |

The D3D12 surface is **the same size as the one already done** — 118 vs 111
interfaces, 2,604 vs 2,593 slots — and 43 of those 118 interfaces are the DXGI
ones that already exist, generated and marshalled. The genuinely new work is 62
D3D12 interfaces / 1,645 slots, and the debug layer can be dropped.

Two further checks that came out clean `[MEASURED]`:

- **Max arity is 10**, hit by exactly two methods
  (`ID3D12Device10::CreateCommittedResource3`, `ID3D12CommandQueue::UpdateTileMappings`).
  `DXVK_MAX_ARGS` is 10 (`dxvk_thunk_abi.h:21`). Zero methods exceed it. That is
  a coincidence worth a `static_assert` rather than a comfort.
- **Float-class parameters: 2 methods** (`GraphicsCommandList::ClearDepthStencilView`,
  `GraphicsCommandList1::OMSetDepthBounds`) versus five in D3D11. The
  `DXVK_FSHAPE_*` mechanism transfers with a smaller table.

Would `gen_interfaces.py` handle the D3D12 headers as-is? **No** — see §3, it
emits 47 phantom slots across 22 interfaces. The fix is mechanical; what it
exposes downstream is not.

One more input problem: DXVK's vendored `d3d12.h` is **stale**. `[MEASURED]` it
stops at `ID3D12Device1` and `ID3D12GraphicsCommandList2` (21 `MIDL_INTERFACE`
declarations, 68 interfaces resolved once DXGI is included). Wine's `d3d12.idl`
reaches `ID3D12Device10` / `ID3D12GraphicsCommandList7`. Headers must come from
widl over Wine's IDL, from Microsoft's DirectX-Headers, or from vkd3d-proton's own
vendored IDL — not from the DXVK checkout.

### 4.6 Struct layout — **NOT A PROBLEM, with two named exceptions to check**

The D3D11 work proved all 296 aggregates member-offset-identical between x86-64
and ppc64le, so they pass by pointer unrepacked (`gen_layout_check.py:10-17`). The
reasoning given there — *"x86-64 SysV and ppc64le ELFv2 agree on the size and
alignment of every primitive these structs contain, and both are little-endian
LP64"* — applies to D3D12 for the same reason.

`[MEASURED]` I scanned all **222 aggregates in `d3d12.idl`** for the constructs
that could break that:

| construct | count | verdict |
|---|---|---|
| `long double` | **0** | the only primitive where the two ABIs genuinely differ |
| `wchar_t` / `LPCWSTR` / `LPWSTR` | **0** | (11 use `const WCHAR *`, which is a *pointer* — 8 bytes both; the pointee is §4.8's problem, not a layout problem) |
| `HANDLE` / `HWND` members | **0** | no handle-bearing descriptor structs |
| bitfields | **1** | `D3D12_RAYTRACING_INSTANCE_DESC` — see below |
| `SIZE_T` members | 7 | 8 bytes on both; fine |

Everything else is `UINT`, `UINT64`, `FLOAT`, `BOOL`, enums, and pointers.

**The one to actually check** is `D3D12_RAYTRACING_INSTANCE_DESC`
(`d3d12.idl:3961-3969`):

```c
FLOAT Transform[3][4];
UINT InstanceID : 24;
UINT InstanceMask : 8;
UINT InstanceContributionToHitGroupIndex : 24;
UINT Flags : 8;
D3D12_GPU_VIRTUAL_ADDRESS AccelerationStructure;
```

Both ABIs allocate bit-fields from the least-significant end on little-endian
targets, so this *should* be identical — but "should" is not a check, and this is
the exact class of silent field corruption the D3D11 work kept finding. It is also
special in a way that makes it worse: the application writes this struct **into a
GPU upload buffer**, not into an API call, so no marshaller ever sees it and no
`STRICT` mode can warn. If the layouts disagree, ray-tracing instances get wrong
masks and it looks like a driver bug. `gen_layout_check.py` as written dumps
`sizeof`/`alignof` only; catching a bitfield disagreement needs member offsets or
a bit-pattern round-trip, which is a small extension.

**The other one to check** is `D3D12_PIPELINE_STATE_STREAM_DESC`. It is a `SIZE_T
SizeInBytes; void *pPipelineStateSubobjectStream;` pair whose *contents* are a
packed sequence of `alignas(void*)`-tagged subobjects. Layout of the stream
depends on `alignof(void*)` = 8 on both, so it should be fine, but it is a blob
the generator cannot type-check and the failure mode is a misparsed PSO
description.

### 4.7 By-value aggregate returns — **REAL, new, and the top ABI risk**

Covered mechanically in §3; the ABI half belongs here.

`[VERIFIED-LOCAL]` MinGW's widl-generated `d3d12.h:2763-2765` declares the **C**
vtable entry unconditionally in the hidden-pointer form:

```c
D3D12_CPU_DESCRIPTOR_HANDLE * (STDMETHODCALLTYPE *GetCPUDescriptorHandleForHeapStart)(
    ID3D12DescriptorHeap *This,
    D3D12_CPU_DESCRIPTOR_HANDLE *__ret);
```

while the **C++** interface offers both forms behind
`WIDL_EXPLICIT_AGGREGATE_RETURNS` (`:2682-2694`). Wine's widl generates this from
`is_aggregate_return()` (`tools/widl/header.c:889-894`), which fires on
`TYPE_STRUCT`/`TYPE_UNION`/`TYPE_COCLASS`/`TYPE_INTERFACE`/`TYPE_RUNTIMECLASS` —
so enum returns are unaffected, which is why 7 of my 22 hits need nothing. Wine's
own vkd3d implements the hidden-pointer form:
`libs/vkd3d/resource.c:4229` `static D3D12_CPU_DESCRIPTOR_HANDLE * STDMETHODCALLTYPE
d3d12_descriptor_heap_GetCPUDescriptorHandleForHeapStart(...)`.

**Microsoft's own ABI documentation settles the half that mattered most**, and it
settles it in the direction that makes the generator simpler. `[VERIFIED-UPSTREAM]`
<https://learn.microsoft.com/en-us/cpp/build/x64-calling-convention>, *Return
values*, verbatim:

> User-defined types can be returned by value **from global functions and static
> member functions**. To return a user-defined type by value in `RAX`, it must
> have a length of 1, 2, 4, 8, 16, 32, or 64 bits. It must also have no
> user-defined constructor, destructor, or copy assignment operator. … Otherwise,
> the caller must allocate memory for the return value and pass a pointer to it as
> the first argument. The remaining arguments are then shifted one argument to the
> right. **The same pointer must be returned by the callee in `RAX`.**

Two consequences:

1. **A COM method is a non-static member function, so the `RAX` path is not
   available to it at all** — the permission is granted only to global and static
   member functions. So `GetCPUDescriptorHandleForHeapStart` does **not** return
   its 8-byte handle in `RAX` under MSVC; it uses the caller-allocated buffer, like
   every other aggregate return. **All 15 struct-returning slots therefore share
   one shape**, and the generator needs one new transport, not two.
2. This is also the **rationale for `WIDL_EXPLICIT_AGGREGATE_RETURNS`**, which I
   had listed as unknown. A plain C compiler seeing `struct8 f(void*)` would use
   `RAX`; widl forces the memory form so that C code built by GCC matches MSVC's
   member-function rule. The macro is not a workaround for a compiler bug, it is
   widl reconciling C-function rules with C++-member-function rules.

**What the document does *not* settle** is the register order: whether the hidden
pointer takes `RCX` with `this` displaced to `RDX`, or `this` keeps `RCX` and the
pointer takes `RDX`. "The first argument … remaining arguments shifted one to the
right" is ambiguous about whether the implicit `this` counts. The evidence
available here points to **`this` first, buffer second**: widl writes the C vtable
entry as `(This, __ret)` (above), Wine's vkd3d implements it that way
(`libs/vkd3d/resource.c:4229`), and that same widl-generated shape is what
vkd3d-proton's PE build presents to MSVC-compiled games in Proton in production.
That is an empirical argument, not a documentary one, and the residue is in §7.1.

The other thing not in doubt: this hazard exists, and the D3D11 work's argument for
why ABI risk was low explicitly does not cover it (`hazard-hunt.md:358-364`), which
listed "by-value aggregate returns (none)" as one of the shapes that made the
residual risk small.

Note that §5.4 makes this problem disappear entirely on the unixlib transport, and
that adopting the args-struct form on the FEX transport (§5.7 item 3) would do the
same without changing boundary.

### 4.8 The DXR state-object graph — **REAL**, and worse than any D3D11 marshalling case

`[VERIFIED-LOCAL]` `d3d12.idl:3782-3787` and `:3660-3664`:

```c
typedef struct D3D12_STATE_OBJECT_DESC {
    D3D12_STATE_OBJECT_TYPE Type;
    UINT NumSubobjects;
    const D3D12_STATE_SUBOBJECT *pSubobjects;
} D3D12_STATE_OBJECT_DESC;

typedef struct D3D12_STATE_SUBOBJECT {
    D3D12_STATE_SUBOBJECT_TYPE Type;
    const void *pDesc;          /* points at one of ~14 types, chosen by Type */
} D3D12_STATE_SUBOBJECT;
```

`ID3D12Device5::CreateStateObject` therefore takes a **recursive, tag-dispatched,
string-bearing object graph**: an array of type-tagged `void*`s, several of whose
targets (`D3D12_EXPORT_DESC`, `D3D12_HIT_GROUP_DESC`,
`D3D12_SUBOBJECT_TO_EXPORTS_ASSOCIATION`) carry `const WCHAR *` arrays and further
back-references to other subobjects.

`gen_thunk.py` marshals from **flat per-slot signatures** — `ARRAY_SPECS`,
`IFACE_BEARING_STRUCTS`, per-slot IN/OUT tables. It has no notion of a
discriminated union behind a `void*`. The D3D11 precedent is `GetPrivateData`
with an interface GUID (`QUEUE.md` B1a), which the project recorded as *not
statically detectable* and left unfixed.

**But it is much smaller than it first looks, for two reasons already
established.** Because vkd3d-proton's `WCHAR` is `unsigned short` (§3), the
strings need no conversion at all. And because FEX shares an address space, a
pointer graph the guest built is directly readable by the host. So most of
`D3D12_STATE_OBJECT_DESC` reduces to **a pointer passed through unrepacked** —
the same treatment every other descriptor struct gets — provided every struct in
the graph is layout-identical, which §4.6's scan says it is (the `WCHAR` members
are *pointers*, 8 bytes on both).

That downgrades this from "recursive marshaller required" to "verify the graph's
struct layouts and pass the pointer" — **except** where a subobject carries an
*interface* pointer, which must be proxy-unwrapped. `[MEASURED]` there are
**12 subobject types** (`D3D12_STATE_SUBOBJECT_TYPE`, `d3d12.idl:3643-3658`,
excluding `MAX_VALID`) and **3 of them carry interface pointers**
`[VERIFIED-LOCAL]`:

```c
d3d12.idl:3680   D3D12_GLOBAL_ROOT_SIGNATURE  { ID3D12RootSignature *pGlobalRootSignature; }
d3d12.idl:3685   D3D12_LOCAL_ROOT_SIGNATURE   { ID3D12RootSignature *pLocalRootSignature; }
d3d12.idl:3715   D3D12_EXISTING_COLLECTION_DESC {
                     ID3D12StateObject *pExistingCollection;
                     UINT NumExports; D3D12_EXPORT_DESC *pExports; }
```

Global and local root signatures are not exotic — **every** DXR pipeline has at
least one. So the walker is required, not a fallback. `IFACE_BEARING_STRUCTS` in
`gen_thunk.py` cannot reach these: it works by naming a struct type at a *typed*
parameter position, and `D3D12_STATE_SUBOBJECT::pDesc` is `const void *`.

The good news is the size: **12 tagged cases, 3 of which do anything**, and the
strings and everything else pass through untouched. That is a bounded
hand-written walker of perhaps 60 lines, not a general recursive marshaller.

The alternative, if DXR is deferred: gate `CreateStateObject` behind
`dxvk_thunk_refuse()` (`dxvk_proxy.h:124`), which already exists for
unmarshallable slots. Loses ray tracing; keeps everything else, and fails loudly.

A related item in the same area: `ID3D12StateObjectProperties::GetShaderIdentifier`
returns a raw `void*` into host memory that the application `memcpy`s into its
shader table. Under a shared address space that works, and it is the same class as
`Map`. It would not survive a design where host and guest addresses differ.

---

## 5. The unixlib option: expand the native Wine port instead of thunking

Evaluated as a first-class alternative, as asked: put D3D12 on Wine's
**`__wine_unix_call` / unixlib** boundary — the mechanism `ntdll` and
`winevulkan` already use — inside the native ppc64le Wine at
`hangover-ppc64le/wine-upstream` (branch `ppc64le-attempt`, `[MEASURED]` HEAD
`61c2c22`), instead of building a bespoke FEX thunk.

**Verdict: it is not a trap, and one of its properties is genuinely valuable —
but it is not currently reachable, and this project's own prior research already
established why.** Three independent blockers, any one of which is sufficient.
Stated up front so nothing important sits at the bottom of a long section. **B1 I
verified myself; B2 and B3 are quotations from handbook documents read on my
behalf and not opened by me** — see §7.5, because they carry the verdict.

> **B1 — There is no emulated x86-64 PE module in this Wine to *do* the calling.**
> `[MEASURED]`, read directly: `hangover-ppc64le/wine-build/Makefile:166-167` —
> **`PE_ARCHS = `** (empty) and `DLLEXT = .so`. Corroborated by the handbook's own
> measurement,
> `wine-ntdll-pe.md:88-92`: *"`PE_ARCHS` is empty and `DLLEXT = .so`
> `[MEASURED]` — this is the ELF-builtin route. So the 'PE half of ntdll' is
> `dlls/ntdll/ntdll.dll.so`, an ELF shared object built by winegcc."* Everything
> in this Wine, including the notional PE side, is a **native ppc64le ELF**. The
> crossing the option is built around does not exist here yet.
>
> **B2 — Both routes that would create such a module are documented CLOSED by
> this project.** Not "unimplemented" — investigated and rejected, with verdicts
> at the top of each document. `wow64-same-bitness.md:17`: *"**No. It is
> structurally 32-on-64, and the block is one predicate deep.**"* — the gate is
> `dlls/ntdll/unix/env.c:1847`, an AMD64 main image never gets `WowTebOffset`, so
> `wow64.dll` is never loaded and no `BTCpu*` DLL is consulted; and `:518`
> *"`wow64.dll` cannot host an amd64 guest, and making it do so is not a small
> patch."* `ec-without-hybrid-pe.md:22`: *"**No. And the blocker is on the
> guest→native side, which is where the hypothesis expected to be safe.**"* The
> only route with a finished shape is **i386 guests**, which is not the target.
>
> **B3 — No emulator is wired into this Wine, and the Wine itself does not yet
> boot.** `wine-forward-port-gap.md:548-554` sets the ordering explicitly:
> *"Writing a `BTCpu*`-conforming emulator DLL needs zero upstream Wine changes …
> The blocked half is the prerequisite: getting Wine itself to build and run
> natively on ppc64le … The emulator route does not route *around* §2.8; it sits
> on top of it."* Current status `wine-ntdll-pe.md:579-583`: the unix side builds
> **yes**; a Wine unit test ran **yes** (378 executed, 2 failures); `wineboot -u`
> on a fresh prefix **no** — *"it runs the whole install and then crashes"*, with
> `kernel32.dll` absent from `C:\windows\system32`.

Everything below is the assessment on its merits, which still matters — the
option becomes live the moment B1–B3 move, and one of its properties is worth
stealing regardless.

### 5.1 Wine's D3D12 does *not* ride the unixlib boundary — it crosses one layer lower

The premise to check first, and it comes out opposite to the assumption.
`[VERIFIED-LOCAL]`:

- `wine-upstream/libs/vkd3d/Makefile.in` line 1: **`STATICLIB = libvkd3d.a`**,
  with `EXTRAINCL = $(VKD3D_PE_CFLAGS)`. Wine compiles the whole of vkd3d —
  `vkd3d`, `vkd3d-shader` (including `dxil.c`), `vkd3d-common`, `vkd3d-utils` —
  into a **PE static library**.
- `wine-upstream/dlls/d3d12/Makefile.in` contains **no `UNIXLIB` line at all**
  `[MEASURED]` (`grep -c UNIXLIB` → 0). `d3d12.dll` is entirely PE-side.
- `dlls/d3d12/d3d12_main.c:41-58` reaches Vulkan with
  `LoadLibraryA("vulkan-1.dll")` + `GetProcAddress("vkGetInstanceProcAddr")` —
  through the **PE** Vulkan loader.
- The unixlib boundary sits at Vulkan: `dlls/winevulkan/Makefile.in` line 2 is
  **`UNIXLIB = winevulkan.so`**, and `dlls/winevulkan/loader.c` is wall-to-wall
  `UNIX_CALL(...)`.

So in Wine today **the entire D3D12 → Vulkan translation is on the PE side**, and
under emulation that means emulated — precisely the tax this project exists to
remove. "Expand the Wine work" is therefore not adoption of something that already
works; it is **moving Wine's boundary up a whole layer**, new work in Wine.

The project reached the same conclusion for D3D11 already, and wrote it down.
`wine-forward-port-gap.md:697-705`: *"**A native ppc64le Wine cannot load an
x86-64 PE DXVK.** Its only d3d path would be `wined3d` — precisely the slow path
Proton exists to bypass. Under x86-64 emulation DXVK works, and its hot path
(Vulkan calls) can be *thunked* to a native ppc64le Vulkan driver rather than
emulated — which is what `fastppcx86`'s Vulkan thunk already does in this
project."*

### 5.2 Would it force Wine's vkd3d? — only via the shortcut, and the shortcut is the downgrade

Two proposals hide behind one sentence, and they have different answers.

**(a) "Move Wine's `dlls/d3d12` + `libs/vkd3d` to the unix side."** This forces
Wine vkd3d, and it is a serious compatibility downgrade — §1's table: feature
level **12_1** max, shader model **6.0** max, `RaytracingTier = NOT_SUPPORTED`,
`MeshShaderTier = NOT_SUPPORTED`, `EnhancedBarriersSupported = FALSE`, and
`DispatchRays` / `DispatchMesh` / `RSSetShadingRate` / `SetPipelineState1` /
`BuildRaytracingAccelerationStructure` all literal `FIXME(… "stub!")`
`[VERIFIED-LOCAL]` `wine-upstream/libs/vkd3d/libs/vkd3d/command.c:6406-6427`. No
modern D3D12 title runs on that. **Said plainly, as asked: this option buys a
clean boundary by giving up the game coverage that is the entire point.**

There is a second, independent blocker on (a). vkd3d-proton **deleted** the
standalone `vkd3d_*` C API in commit `065f2fc6` (§1), and Wine's `d3d12.dll` is
written directly against exactly that API — `d3d12_main.c` calls
`vkd3d_create_instance` / `vkd3d_create_device` and supplies `pfn_signal_event`
`[VERIFIED-LOCAL]`. vkd3d-proton cannot be dropped into Wine's `d3d12.dll` at all;
you would be reviving a deleted API in a fork, against upstream's stated intent.

**(b) "Write a new PE-side `d3d12.dll` whose COM vtables forward over
`UNIX_CALL` to a native `.so`."** This does **not** force Wine vkd3d — the native
side can be `libvkd3d-proton-d3d12.so`. But it is not cheap: it still requires the
full 1,645-slot D3D12 vtable surface (§4.5), the slot table, interface-pointer
proxying, and every marshalling decision in this document. The transport changes;
the surface does not.

### 5.3 What one crossing costs — `[MEASURED]`, and it is not a plain call

The dispatcher is not a syscall — upstream stopped that in 2022
(`wine-forward-port-gap.md:302-303`, commit `ee147d9216`, *"Replace the
`__wine_unix_call()` syscall by a function calling through the dispatcher"*) —
but it is not a thunk either.

`[VERIFIED-LOCAL]` `wine-upstream/dlls/ntdll/unix/signal_ppc64.c:1703-1807`,
`__ASM_GLOBAL_FUNC( __wine_unix_call_dispatcher, … )` — **101 assembly statements
on the entry side alone** `[MEASURED]`, roughly 60 of them stores. In order: stash
the caller's TOC in the red zone; derive its own TOC from r12; a 4-instruction
initial-exec TLS load of `ppc64_unix_teb`; load the syscall frame from `TEB+0x378`;
spill **r13–r31** (19 GPRs), **f14–f31** (18 `stfd`), **v20–v31** (12 `stvx` with
pointer bumps), plus CR, XER, CTR, LR (twice) and FPSCR; switch to the
frame-relative kernel stack; and only then:

```
    "sldi 0, 4, 3\n\t"      /* code * 8    */
    "ldx 12, 3, 0\n\t"      /* funcs[code] */
    "mr 3, 5\n\t"           /* args        */
    ...
    "bctrl\n\t"
    "b " __ASM_LOCAL_LABEL("__wine_syscall_dispatcher_return")
```

returning through the shared syscall-return path, which restores it all.

Compare the FEX thunk, `[VERIFIED-LOCAL]` via `hazard-hunt.md:620-712` quoting
`FEXCore/.../PPC64LE/BranchOps.cpp` `DEF_OP(Thunk)` — **7 instructions**:

```
  mr(r3, GetReg(Op->ArgPtr));  InsertNamedThunkRelocation(TMP2, …);  mr(r(12), TMP2);
  std(r2, 24, r1);  mtctr(TMP2);  bctrl();  ld(r2, 24, r1);
```

**~7 versus ~150 instructions** counting both directions — `[INFERRED]` as a
ratio, since it is an instruction count, not a timing. **Nothing in the handbook
times either boundary** — the cost question is unmeasured on both sides, and that
is itself worth knowing.

And this is the *best* case: native ppc64 PE → native ppc64 unix, no emulator. An
emulated guest must first reach the dispatcher through the JIT. In the one route
with a finished shape, that is a patched trap opcode —
`fexcore-embeddability.md:543-560`, `*reinterpret_cast<uint32_t*>(Addr) =
0x2ecd2ecd; /* int 0x2e ; int 0x2e */`, decoded by FEXCore as a syscall and
bracketed by `UnlockJITContext`/`LockJITContext`. `qemu-to-fastppcx86.md:762-767`
is right that *"two patched words are the entire emulation boundary"* and that for
a JIT this is an exit, not a mode switch — but it is still an exit, and its cost is
documented nowhere.

In fairness: the register spill is not waste. It is what makes the boundary safe
for arbitrary PE code and why it is maintained upstream rather than by us.

### 5.4 What the unixlib boundary genuinely fixes — the one property worth stealing

This deserves saying without hedging, because it lands on the one ABI question
this document could not fully close.

`[VERIFIED-LOCAL]` `include/wine/unixlib.h:290-301` — `__wine_unix_call( handle,
code, args )` is **one 64-bit handle, one `unsigned int`, one `void *`**, and the
callee type is `typedef NTSTATUS (*unixlib_entry_t)( void *args )` (`:35`) — a
single pointer. No floats, no by-value structs, no varargs, one universal
signature. `signal_ppc64.c:1796-1806` passes `args` through verbatim (`mr 3, 5`).

That structurally eliminates:

- the entire `ms_abi` forwarder army — defect A1's whole class;
- `DXVK_FSHAPE_*` float shapes (`dxvk_thunk_abi.h:32-46`);
- the `DXVK_MAX_ARGS = 10` arity ceiling;
- **and §4.7's by-value aggregate returns entirely — including the register-order
  residue §7.1 could not close.** An aggregate return becomes a member of the args
  struct, so `GetCPUDescriptorHandleForHeapStart` and `GetResourceAllocationInfo`
  stop being an ABI question and become a struct-layout question, which §4.6
  already answers.

The scale of that is quantified elsewhere in the handbook: `ec-without-hybrid-pe.md`
quotes Microsoft that ARM64EC thunks are *"specifically tailored for each
individual function signature"* and counts **2,286 distinct (convention, arg-type)
tuples** across Wine's spec files (`:113-115`). A fixed three-integer boundary
needs exactly **one**.

**But the same document already establishes why that advantage does not extend to
D3D.** `ec-without-hybrid-pe.md:631` names the blocker as *"2,921 vtables, 2,721
interfaces `[MEASURED]`"* with no annotation point. COM dispatch has no
`(code, args)` chokepoint to exploit; you have to build one, per slot, which is
exactly the generated surface this document has been costing all along. The
unixlib boundary makes each slot's *transport* trivial; it does not make the slots
go away.

What it also does not eliminate: the requirement that the args structs be
layout-identical across the boundary — the same LP64 little-endian argument as
§4.6, now load-bearing for *every* call rather than only for descriptor structs.
No handbook document discusses that, and it is in §7.5.

### 5.5 Can it carry the specific hazards?

- **Descriptor-handle pointer arithmetic (§4.1) and `Map` (§4.2)** need one
  address space. `[VERIFIED-LOCAL]` PE ntdll and unix ntdll are two ELF mappings
  in one Unix process; `__wine_unix_call_dispatcher` is filled in with the unix
  ntdll's own address at `dlls/ntdll/unix/loader.c:1591-1606`, and unix libraries
  are `dlopen`'d into the process (`loader.c:795-803`). The `args` pointer passes
  numerically unchanged. For the emulated case the same holds:
  `qemu-to-fastppcx86.md:293-301` — *"**First, the address space is 1:1.** A guest
  pointer is numerically the host pointer; there is no softmmu and no base
  register … **no marshalling is needed at the boundary**."* **Carried.**
- **Fences / `SetEventOnCompletion` (§4.3)** — unixlib is *better* here. On the
  FEX route the obstacle is hard: a host worker thread invoking a guest callback
  is `ERROR_AND_DIE`, not a wrong answer (`fex-thunk-architecture.md:635-655`). On
  the unixlib route the PE side is real Wine code that can call `SetEvent` itself,
  and a PE thread parked in a blocking `UNIX_CALL` on an eventfd is an ordinary
  shape. The design in §4.3 is the *same* design, on a boundary where it is
  idiomatic rather than a workaround. `[INFERRED]` — I did not find a Wine unixlib
  that does precisely this and verify it.
- **Multithreaded recording (§4.4)** — unchanged. The ordering argument is about
  guest-thread-to-guest-thread handoff and the emulator's TSO obligation, and is
  independent of which boundary the call crosses.

### 5.6 Would it subsume the DXVK thunk too?

In principle yes, and that is a genuine attraction: one mechanism for D3D11 and
D3D12, maintained upstream. The accounting is less favourable than it looks.

**Discarded**: the FEX-specific transport — thunk registration
(`pe-shim/ThunksDB-dxvk_d3d11.json`, the unapplied registration patch,
`QUEUE.md` B4), the guest ELF stub, the `ms_abi` forwarders.
**Kept**: `gen_interfaces.py`, `interfaces.json`, the slot tables,
`gen_layout_check.py`, and all of `gen_thunk.py`'s marshalling classification —
the large majority of the effort and, per `QUEUE.md` B1, the part that took
longest to get right.

So the shared-mechanism argument is real but small: what the two boundaries would
share is already shared, because it is the generator, not the transport.

### 5.7 Recommendation on this option

1. **Reject 5.2(a) outright.** Moving Wine's existing `dlls/d3d12` + `libs/vkd3d`
   to the unix side means accepting FL 12_1 / SM 6.0 / no DXR / no mesh / no VRS.
   That is not a boundary choice, it is a decision not to run modern games, and it
   should not be made accidentally by choosing a transport.
2. **Do not wait for 5.2(b).** It is behind B1–B3, and B2 in particular is not a
   schedule item — two routes were investigated and both got a "No" verdict at the
   top of their own document. Nothing in this assessment should be sequenced after
   that.
3. **Steal the one good property now.** The args-struct transport is what makes
   §4.7 and its §7.1 residue disappear, and it costs nothing to adopt on
   the FEX boundary: pass a pointer to a generated per-slot params struct instead
   of `uint64_t[10]`. `gen_thunk.py` currently emits `dxvk_thunk_call(iface, slot,
   host, args)` inline at every stub; making the transport a back end means the
   boundary becomes a build-time switch rather than a rewrite, *and* the
   args-struct form is strictly more general than the register-shaped one.
   This is the concrete, actionable outcome of evaluating this option.
4. **Ship on FEX.** It works today, it is roughly an order of magnitude cheaper
   per crossing, and it is the only one of the two that does not depend on an
   emulator integration that this project's own research has twice concluded is
   closed.

### 5.8 This is a conditional rejection, not a closed door

Recorded because §5.7 reads more final than it should, and the distinction
matters for anyone picking this up later.

Wine vkd3d's feature gap is **a development target, not a fixed property of the
option**. A project already porting Wine to a new architecture is not entitled to
treat "this component lacks features" as disqualifying on its own — the gap could
be closed, and both codebases are **LGPL-2.1**, so implementations can be ported
across rather than reinvented.

What actually decides it today is that the gap buys nothing. The reason to prefer
Wine vkd3d was never features — it was riding Wine's maintained unixlib boundary
instead of a bespoke thunk. That boundary is unreachable for an unrelated reason
(§5.2, `PE_ARCHS` empty, no ppc64 PE target). So closing the capability gap now
means paying for DXR, mesh shaders, work graphs, enhanced barriers and SM 6.x —
thousands of lines of specialist work — **without** obtaining the architectural
benefit that motivated the choice. That is the argument, and it is narrower than
"Wine vkd3d is not good enough."

**Why it is underinvested is not why it is limited.** Wine vkd3d is stalled
because on x86-64 and ARM64 nobody needs it — vkd3d-proton's PE DLLs run natively
on those hosts, so the integrated path buys those platforms nothing. That
disincentive does not apply here: on POWER the integrated path is the only one
that avoids emulating the translation layer. So the feature gap reflects **upstream
priorities, not difficulty**, and treating it as evidence the path is unsound is a
category error. If the gap were the only objection, it would be a barrier to break
through rather than a reason to walk away.

**The objection that features do not answer** is §5.3: ~150 instructions per
crossing against the FEX thunk's ~7, because the dispatcher spills r13–r31,
f14–f31, v20–v31, CR, XER, CTR, LR and FPSCR to be safe for arbitrary PE code.
That cost lands per call, on a D3D12 workload making thousands of calls per frame,
and it is the specific overhead this project exists to remove. Note the honest
limit on that figure: it is an instruction count, **not a timing**, and neither
boundary has ever been timed here (§5.3).

**"We would become maintainers" is not an argument here, and was wrongly offered
as one.** This project already maintains a Wine fork, a DXVK patch series, a fork
of FEX (`fastppcx86`), and a thunk generator written from scratch. Every layer in
the stack is already carried locally. Maintenance burden therefore does not
distinguish the integrated path from the thunked one, and any reasoning that leans
on it is reasoning from a cost the project has already paid.

What remains is the per-crossing cost above — which is a property of the machine,
not of who owns the source.

**Revisit when:** a ppc64 PE target exists and `PE_ARCHS` is non-empty. That is
not fantasy — `winebuild` in this project's Wine tree already emits
`IMAGE_FILE_MACHINE_POWERPC64` (0x01f2). If that lands, the trade inverts: the
unixlib boundary becomes real, its args-struct transport comes for free rather
than being stolen (§5.7.3), and expanding Wine vkd3d's capabilities becomes a
question of cost rather than of pointless cost. Nothing here should be read as
foreclosing that.

---

## 6. Vulkan floor on this hardware

`[MEASURED]` on the AC922 (`ssh`, read-only): `mesa 26.1.2`, `vulkan-radeon
26.1.2`, `/usr/share/vulkan/icd.d/{radeon_icd.json, lvp_icd.json}`, Vulkan headers
present at `/usr/include/vulkan/`.

`[VERIFIED-LOCAL]` `dxvk-ppc64le/docs/present-proof-radv.md` records RADV driving
the real device repeatedly: `AMD Radeon RX 7900 XTX (RADV NAVI31)`, PCI
`1002:744c`, `radv 26.1.2`, with `vkQueuePresentKHR` returning 0.

vkd3d-proton's documented floor is RADV ≥ Mesa 22.0 `[VERIFIED-UPSTREAM]`; this is
26.1.2 on a fully-supported gfx11 part, so the requirement is met **by
documentation**. It is **not met by measurement here** — I did not enumerate
`VkPhysicalDeviceDescriptorIndexingProperties::maxUpdateAfterBindDescriptorsInAllPools`,
`VK_EXT_robustness2`'s `nullDescriptor`, or `apiVersion` on this device, because
`vulkaninfo` is not installed and installing is out of scope. Settling it costs
one command: install `vulkan-tools` and run `vulkaninfo --summary`. Until then it
is in §7.

---

## 7. Claims here that my research could not disprove

Everything below is either reported from a secondary route, inferred rather than
observed, or a primary artifact I did not read. Treated as the most important
section per the brief.

### 7.1 The MS-x64 aggregate-return convention — partly settled, residue named

Partly resolved after drafting, from Microsoft's own ABI page (quoted in §4.7).
**Settled:** a COM method is a non-static member function and therefore never
returns a user-defined type in `RAX`, so all 15 struct-returning slots use the
caller-allocated buffer and share one shape; and that is also why widl emits
`WIDL_EXPLICIT_AGGREGATE_RETURNS`.

**Still open, and this is the residue that matters:**

- **The register order.** Whether the hidden return-buffer pointer takes `RCX`
  with `this` displaced to `RDX`, or `this` keeps `RCX` and the buffer takes
  `RDX`. Microsoft's wording ("pass a pointer to it as the first argument. The
  remaining arguments are then shifted one argument to the right") does not say
  whether the implicit `this` is among the arguments being shifted. §4.7 argues
  for `this`-first from widl's `(This, __ret)` form and from vkd3d-proton's
  production interop with MSVC games — an **empirical** argument, not a
  documentary one, and one nobody in this project has tested.
- **Whether GCC and Clang `ms_abi` agree with MSVC on that order**, and with each
  other. Not checked in either compiler's source or documentation.
- **Whether MSVC's rule changes for a type that is not trivially copyable.** Moot
  for D3D12's plain-C structs, but it is the kind of assumption that gets carried
  into somewhere it is not moot.

I also want to flag a process point, because it is the sort of thing this section
exists for: while drafting I recalled a specific mechanism in clang
(`MicrosoftCXXABI::isSRetParameterAfterThis()`) that would settle the register
order in favour of `this`-first. **I did not read that source, and I am not
recording it as a finding** — only as a pointer for whoever settles this.

**What settles it, cheapest first:**

1. A differential disassembly: compile the same six prototypes
   (`GetCPUDescriptorHandleForHeapStart`, `GetDesc`, `GetResourceAllocationInfo`,
   plus a control) with MSVC on any Windows machine and diff the argument and
   return-buffer setup against `clang -target x86_64-pc-windows-msvc` and
   `g++ -mabi=ms`. Needs no ppc64le hardware. This is the same recommendation
   `hazard-hunt.md:366-376` already makes for the D3D11 residual, and it now has a
   second, larger reason to be done.
2. A cross-compiler agreement test in the style of `hazard-hunt.md:322-357` —
   GCC 15.2 `ms_abi` call sites against clang `ms_abi` forwarders, extended to
   aggregate returns, with the convention deliberately inverted as the negative
   control. That proves GCC ≡ clang, not GCC ≡ MSVC, but it is runnable here.
3. Any real MSVC-built D3D12 title reaching `Present`.

Note that this is *not* a blocker for deciding to proceed — it is a blocker for
believing the result once built, and the generator fails closed
(`gen_thunk.py:519-528`) rather than guessing, so it cannot ship wrong silently.

### 7.2 What the `WCHAR` result does *not* prove

`typedef unsigned short WCHAR;` is `[VERIFIED-UPSTREAM]` (§3), so the width claim
is settled. Two things riding on it are not:

- I read the typedef in isolation and did not confirm the `#if !defined(_WIN32)`
  guard around it in situ, nor that no other header redefines `WCHAR` for a
  native build. One `grep -rn "WCHAR" include/ libs/` over a checkout settles it.
- The conclusion in §4.8 that the DXR subobject graph can pass unrepacked is
  `[INFERRED]` from the WCHAR result plus §4.6's layout scan. Nobody has diffed
  the layouts of the ~14 subobject types across the two ABIs, and
  `D3D12_EXISTING_COLLECTION_DESC`'s embedded `ID3D12StateObject*` is a known
  counterexample that a pointer-passthrough cannot handle.

### 7.3 vkd3d-proton's DXGI requirement and `SetEventOnCompletion` on native

Two questions I raised and did not get answered before finishing. Both are cheap
to settle from a checkout and both should be settled before any design work.

- **What vkd3d-proton needs from DXGI**, at build time and at runtime — whether it
  hard-requires DXVK's `IDXGIVkInteropAdapter`, or can enumerate Vulkan physical
  devices directly, and hence whether `D3D12CreateDevice(NULL, …)` works in a
  native build with no DXVK present. Wine's own vkd3d README is explicit that
  *"neither libvkd3d nor libvkd3d-utils implement any DXGI interfaces"*
  `[VERIFIED-UPSTREAM]`, and vkd3d-proton is a fork of it, so it almost certainly
  ships no DXGI either — but "almost certainly" is not a check, and the *shape* of
  the dependency is what matters. This decides whether the D3D11 work's existing
  native `libdxvk_dxgi.so` becomes a **dependency** (good — already built and
  proven) or an **irrelevance**. Settle by reading `libs/d3d12/` and grepping
  `libs/vkd3d/device.c` for `IDXGI`.
- **What `HANDLE` means for `ID3D12Fence::SetEventOnCompletion` in a native
  build**, and whether a *host worker thread* signals it. §4.3's proposed
  inversion assumes it does, and §4.3 is the hardest hazard in this document, so
  this assumption is load-bearing. `include/private/vkd3d_native_sync_handle.h`
  using `eventfd` on the native branch `[VERIFIED-UPSTREAM]` suggests the native
  build expects an fd rather than a Win32 event, which would change the shim's
  shape — probably for the better, since it would mean the PE shim owns the event
  and never hands it across. Settle by reading `d3d12_fence_SetEventOnCompletion`
  and locating the fence worker thread.

### 7.4 Things I asserted from reasoning, not observation

- **`D3D12_RAYTRACING_INSTANCE_DESC` bitfield layout is identical** between x86-64
  SysV and ppc64le ELFv2. `[INFERRED]` from both being little-endian and both
  allocating bit-fields LSB-first. Not measured, and `gen_layout_check.py` as
  written could not have caught it (it dumps `sizeof`/`alignof`, not offsets or
  bit positions). Settle with a member-offset dump or a bit-pattern round-trip
  compiled for both targets.
- **D3D12 issues fewer API calls per frame than D3D11**, so thunk crossing volume
  may fall. `[INFERRED]` from how the APIs are designed. Completely unmeasured
  here, in either API.
- **A guest thread parked in a blocking thunk call is safe** under fastppcx86 —
  §4.3's fence design depends on it. Unverified; FEX's thread management under a
  long-blocking host call was not examined.
- **dxil-spirv's bitstream reader is LSB-first** — the subagent read the `memcpy`
  idiom and the zero-fill prologue but did not trace `ReadBits`' full body.
  Irrelevant on LE; would matter for ppc64 BE.
- **RADV on this 7900 XTX provides Vulkan 1.3, 1M UpdateAfterBind descriptors and
  `VK_EXT_robustness2` `nullDescriptor`.** Argued from Mesa 26.1.2 ≫ the
  documented 22.0 floor on a supported gfx11 part, and from RADV having driven
  this exact device in this project's own logs. Not enumerated on the device.
  One `vulkaninfo --summary` settles it; `vulkan-tools` would need installing.
- **My IDL parser's counts.** `[MEASURED]` and validated against three
  independently-known vtable sizes (60 / 44 / 15, all exact), which is a real
  control — but it is a regex over IDL, not widl, and a method form it fails to
  match would silently *under*count. The 2,604-slot figure should be regenerated
  from widl-produced headers before anyone plans against it.

### 7.5 The unixlib option (§5) — what I did not establish

**What I read myself** (`[VERIFIED-LOCAL]` / `[MEASURED]`): `PE_ARCHS` empty in
the live build Makefile; the ppc64 `__wine_unix_call_dispatcher` and its 101
statements (`signal_ppc64.c:1703-1807`); the three-argument unix-call signature
(`unixlib.h:35, 290-301`); `libs/vkd3d/Makefile.in`'s `STATICLIB`;
`dlls/d3d12/Makefile.in` having no `UNIXLIB`; `d3d12_main.c`'s
`LoadLibraryA("vulkan-1.dll")`; `dlls/winevulkan/Makefile.in`'s `UNIXLIB`;
Wine vkd3d's DXR/mesh stubs; `dlls/xtajit64/cpu.c` at 249 lines.

**What I am relaying from a subagent's reading of handbook documents**, not from a
document I opened myself — B2 and B3 in §5 rest on these, so they are the ones to
re-check before acting:

- the `wow64-same-bitness.md:17` and `:518` verdicts, and the
  `dlls/ntdll/unix/env.c:1847` predicate they turn on;
- the `ec-without-hybrid-pe.md:22` verdict, its `:113-115` count of 2,286
  (convention, arg-type) tuples, and its `:631` figure of 2,921 vtables / 2,721
  interfaces — that last one is load-bearing for §5.4's conclusion that the
  unixlib advantage does not extend to COM;
- `wine-ntdll-pe.md:88-92` (`PE_ARCHS` / ELF-builtin route — though I confirmed
  this independently from the Makefile), and `:579-600` (the status table, 378
  tests, `wineboot -u` failing);
- `wine-forward-port-gap.md:548-554` (ordering: emulator sits on top of the Wine
  port), `:697-705` (native ppc64le Wine cannot load an x86-64 PE DXVK),
  `:302-303` (`__wine_unix_call` stopped being a syscall in `ee147d9216`);
- `fexcore-embeddability.md:543-566` (the `int 0x2e` bop, and that a 64-bit guest
  needs a `KiUserExceptionDispatcher` hand-off instead);
- `qemu-to-fastppcx86.md:293-301` (1:1 address space) and `:762-767` (two patched
  words as the emulation boundary).

**Still not established by anyone:**

- **Any timing of either boundary.** The subagent's grep across seven handbook
  documents found no cost figure for a unixlib crossing *or* a FEX thunk crossing.
  §5.3's "~7 versus ~150" is an instruction count I derived, not a measurement,
  and it ignores the params-struct write on the unixlib side and the
  `uint64_t[10]` write on the FEX side. A microbenchmark of both would be cheap
  and is the obvious next measurement if anyone wants to argue transport.
- **Whether a PE thread parked in a blocking `UNIX_CALL` is a supported pattern.**
  §5.5's fence argument rests on it. Asserted from the shape of the API; no Wine
  unixlib doing it was found and verified.
- **Struct layout inside `*args` across the boundary.** x86-64 Windows is LLP64,
  ppc64le Linux is LP64; Wine handles this by convention in its params structs and
  no handbook document discusses it. For §4.6's types it is the same LP64-LE
  argument and should hold, but "should" is not a check.
- **Whether winebuild can produce a PE `d3d12.dll` for this target with the COM
  vtable surface intact.** `winebuild-ppc64-imports.md` and
  `winebuild-ppc64-relays-stubs.md` exist in the handbook; neither was read.

### 7.6 Not investigated at all

- Whether vkd3d-proton's native `.so` and DXVK's native `libdxvk_dxgi.so` can
  coexist in one process without symbol collisions (both provide Vulkan glue; both
  may vendor SPIRV headers).
- `D3D12` swapchain presentation. The D3D11 work's foreign-surface WSI, the X11
  escape, the blit fight with Wine's cached window surface, and the Wayland dead
  end (`QUEUE.md` §D) all presumably recur, but I did not check whether
  vkd3d-proton's presentation path differs from DXVK's in any way that matters.
- Licence review. `[VERIFIED-UPSTREAM]` vkd3d-proton's `LICENSE` is the **GNU
  Lesser General Public License, Version 2.1** — not zlib like DXVK. That is a
  materially different obligation for modified binaries, and it lands on the
  side of `dxvk-ppc64le/README.md`'s closing warning about Wine rather than the
  permissive side it describes for DXVK. I confirmed the licence and did **not**
  analyse what it requires of a patch-series-plus-thunk distribution.
- Any actual D3D12 title's requirements. Nothing in this document is grounded in a
  specific game.
