# The game list

Every Windows game installed on this machine, run against the port, with the
walls each one names written down in the order it hits them.

## Why this file exists

A port is finished when programs run, and programs are the only thing that can
say which gap matters next. One title finds a gap; the second title finds the
same gap and proves it was never per-title. That is the whole method, and it
has a shape worth naming:

**Nearly every wall in this pass was decidable before anything ran.** An import
table and an export table are both just tables. `ppc64le/thunks/import_chain.py`
walks a third-party PE's whole static chain against this tree's guest thunk
surface and names every import that would not bind — and for the two titles
that had never been run here, its static answer and the run's answer were the
same list, in the same order. The audit is the cheap first move on any new
title. Run it before you run the game.

The second thing worth naming is the difference between two failure shapes that
look alike in a log and are nothing alike in consequence:

* **A missing EXPORT is survivable.** The loader binds ntdll's per-symbol
  sentinel, `0xDEAD00nn`, and the guest dies only if it actually *calls* it —
  by name, with the module and symbol in the message.
* **A missing MODULE is fatal, immediately, and before any guest code runs.**
  With no guest thunk the loader finds the *native ppc64* module, correctly
  refuses to splice it into a guest call (`c000007b`), and because an ordinary
  non-delay-load import failure fails the whole image, `loader_init` gives up
  with `c0000135`. The process is gone before its entry point.

Two of the three runnable titles here died the second way, on modules nobody
would have guessed: `Normaliz.dll`, `dxva2.dll`, `faultrep.dll`, `mscoree.dll`.
None of them are graphics, audio, or Steam. They are the ordinary furniture of
a Windows build, and the fix for all fifteen was one `.thunks` file each.

## Status board

| Title | appid | Machine | Before this pass | After |
|---|---|---|---|---|
| Warhammer 40,000: Boltgun | 2005010 | PE32+ | dead in `loader_init`, `c0000135`, 0 guest instructions | past the loader, and now **past the DRM stub** too |
| Styx: Master of Shadows | 242640 | PE32+ | dead in `loader_init`, `c0000135`, 0 guest instructions | **past `loader_init`** — 58 modules, every DllMain runs; now stops in its own code on `mscoree._CorExeMain` (handoff #4) |
| The Elder Scrolls V: Skyrim SE | 489830 | PE32+ | loader completes, then `c0000005` at the image entry | **past the DRM stub** — see the 2026-08-18 re-run below |
| Portal 2 | 620 | **PE32** | refused — no 32-bit guest | **attempted 2026-08-18 with the WoW64 lane and did not launch — not yet investigated**; logs in `steamapps/compatdata/620/` |
| Half-Life 2 | 220 | **PE32** | refused — no 32-bit guest | 32-bit is served now; not attempted since |
| The Witcher 3 GOTY | 292030 (GOG copy) | PE32+ | **PLAYS** — in-world, mounted, HUD/minimap/weather live | Four walls fell in one night (2026-08-19, `fex nw-witcher3`), each measured: (1) MSVCP120/MSVCR120 had no AMD64 thunk — `dlls/msvcr120/msvcr120.thunks` (936 exports) + Proton's x86-64 msvcp120 staged in sysx8664, the msvcr100/msvcp100 split; (2) the SSE3 requirements box — ppc64's `RtlIsProcessorFeaturePresent` returned FALSE unconditionally and the shared-data seeding never named x86 features (both fixed in ntdll; a guest probe reads MMX→SSE4.2 all present now); (3) NvCameraSDK64 (Ansel) took the DEAD sentinel through `__clean_type_info_names_internal` at CRT teardown — six flat rows added to the msvcr120 thunks (the C++ EH personality stays refused by design); (4) run-native never set CWD to the game dir — REDengine 3 resolves content from CWD and exited rc=0 politely (fixed; Steam semantics now).  After that: intro cinematic at a locked 60 fps, main menu (both expansions detected), Hearts of Stone start driven by injected keys to LIVE GAMEPLAY — Geralt mounted near Oxenfurt, rain, minimap, journal pop-ups.  In-world fps ~8-15, menus 60 — the same CPU ceiling as every title (NEXT item 6). |
| Dex (Windows build) | 269650 | **PE32** | **PLAYS** — menu 55 fps, in-game (opening apartment, dialogue, HUD) | **the 32-bit lane's first real title, SERVED** (2026-08-28, `fex nw-dexwin`): the full i386 dxvk lane — v8 thunk geometry, emu32 trap dispatch, winecom's 32-bit runtime, struct repacks, the Map bounce.  The canary earned four fixes on the way: native namesakes load from system32 (Dex ships its own i386 d3d11/dxgi beside the .exe), find_dll_file threads the demanded machine, winecom skips table-less namesakes, and wrap_concrete interns resources by GetType (Unity static_casts GetResource's answer to ID3D11Texture2D and calls GetDesc — a proxy vtable sized to the declared interface ran off its stub array).  Remaining: run-native's proton prefix path skips wineboot's 32-bit registration pass (audio COM 'class not registered' until a manual `wineboot -u`; fixed by hand in nw-dexwin, root cause unowned).  Gate: check-d3d11-smoke32.sh, byte-identical to native. |
| FreeInfantry | 2830720 | **PE32** | refused — no 32-bit guest | 32-bit is served now; not attempted since |
| Styx: Master of Shadows (Win32) | 242640 | **PE32** | refused — no 32-bit guest | 32-bit is served now; not attempted since |
| Styx: Shards of Darkness | — | — | not installed (empty directory, no appmanifest) | — |
| Cyberpunk 2077 | 1091500 (GOG copy) | PE32+ | never tried | **creates its D3D12 device** (2026-08-19, `fex nw-cp2077`).  Seven walls fell in one session, in order: the eleven missing exports (all emitted — see the alias-fallback and PROBE-EXTRA commits); the bridge's sticky-NoExec class (ESRCH probe failures cached forever — fixed bridge-side and with 64-bit invalidation hooks); the 2×40 processor-group view REDengine refuses (`WINE_PPC64LE_CPU_LIMIT=64` in its appconfig reproduces the emulated lane's 1×64); a missing guest `secur32` (REDGalaxy throws `gog::RuntimeError` without SSPI — new thunk module, `InitSecurityInterfaceW` GUEST-IMPL'd to a guest-stub table); two guest-unwinder defects GfnRuntimeSdk's C++ throw exposed (a local unwind misread as collided; RtlUnwindEx's ContextRecord never written back; issuer run stacks freed while their records were still referenced); the unrostered WMI + NetworkListManager COM families (game derefs NLM without checking — both served by the syscom roster now); dxvk-native's 4-byte `WCHAR` skewing every wide-bearing struct a guest reads (`GetDesc` answered "A" and zeros — dxvk-patches/0005).  NVIDIA Streamline detour-hooks trap stubs and dies, useless on AMD anyway — a stub `sl.interposer.dll` (ppc64le/streamline/) staged in sysx8664 forwards the D3D creators and reports every feature unsupported.  **RENDERS ITS TITLE SCREEN** (2026-08-19, second session; user-confirmed over Moonlight at 245 fps, and past five minutes of loading after that).  Six more walls fell, in order: the whole derived-interface half of the d3d12 marshal table was NULL — Device1..15, GraphicsCommandList1..10 all refused every slot — and `ppc64le/vkd3d/gen_winecom.py` (the lane's lost generator, rewritten into the family) now serves all 117 interfaces, 2661 slots; `CopyDescriptors` passed its descriptor heap type in a 32-bit STACK slot whose stale upper half vkd3d's 64-bit switch trusted (`winecom_slot::dwordmask` — the flat lane's version-6 width fix, ported to the COM lane); `IMMDevice::OpenPropertyStore`'s honest refusal left an unchecked out-pointer uninitialised and the game called through stack garbage (IPropertyStore is rostered now, read path a diffed gate step); the game's swapchain call lands on DXVK's factory with the vkd3d surface's queue proxy — cross-lane by design — and DXVK's hand slot now forwards to `__wine_d3d12_create_swapchain_for_hwnd`, which serves it entirely inside the d3d12 surface; REDengine QIs the new swapchain for `IDXGISwapChain4` and releases it on E_NOINTERFACE ("Failed to initialize viewport") — the phase-(a) swapchain now answers, with SetHDRMetaData accepted-and-ignored; and with RADV exposing real ray tracing the game built RT pipelines through the DXR state-object refusal and force-closed blaming GPU drivers — `VKD3D_CONFIG=nodxr` in the appconfig hides the tier until the `D3D12_STATE_OBJECT_DESC` walker exists.  **Remaining named refusals it survives**: the PipelineLibrary Load pair (falls back to fresh PSO creation), DXR CreateStateObject (hidden by nodxr), and the syscom flat exports thread 0160/0134 probe at boot.  **GAMEPLAY REACHED** (prologue dialogue, user-driven), **with live rendering corruption that is improved but NOT fixed**: the dense speckle tracked `VKD3D_CONFIG=no_upload_hvv` exactly (removed; mesh shaders A/B'd and cleared), the copy-pattern probe proved this tree's upload/copy path byte-clean in both memory placements — and the game still garbles in ordinary play; the user's read of what remains is memory corruption.  **THE CORRUPTION IS SOLVED** (2026-08-19, the `-benchmark` sessions): it was `ID3D12GraphicsCommandList::ClearDepthStencilView` — FLOAT by value, a named refusal, so no frame ever cleared depth and every 3D pass tested against stale depth/HTILE.  Served by `hand_clear_dsv` + the unixlib typed-float call; the built-in benchmark now renders clean end to end (avg 15.6 fps at 1080p High, CPU-bound), verified against the emulated lane's reference frames.  NEXT.md item 3 carries the full elimination record (both storms, the cross-ISA shader byte-compare, this vkd3d cross-built into the emulated lane).  After that: the frame-latency waitable (`GetFrameLatencyWaitableObject` returns NULL; the eventfd→NT relay is the known lift). |
| DOOM (2016) | 379720 | PE32+ | reached ~99% of startup, then `FATAL ERROR: Memory corruption before block!` and `rc=5`, every run | **past it** — the message was a misdiagnosis, not damage; see handoff #7 below |

> ### Handoff #7 — DOOM's "memory corruption" was `pdh.dll` not loading
>
> **Nothing was corrupt.** Under `GlobalFlag=0x10` Wine paints a 16-byte `0xab`
> canary after every allocation. Swept from outside a frozen process every two
> seconds for whole runs: **~19,600 live allocations, zero damaged canaries**,
> every subheap chain clean, and all **13,578** of the game's own blocks still
> validating against their own cookie right up to the instant the message
> printed. An overrun reaching a neighbour has to destroy a canary first, and
> none was.
>
> Patching DOOM's `FatalError` entry to `jmp rdi` in the live process — `rdi` is
> callee-saved and the free path sets it to `rcx-0x10` — made the emulator print
> the guest register file at the failure, byte-identical across runs:
>
> ```
> R10=0x66 ('f')   R9=0x736C ("ls")   R8=0x8075000500200008
> ```
>
> The "tag" is the letter `f`, the "offset" is `ls`, and the "size" is
> `08 00 20 00 05 00 75 80` little-endian — **Wine's own `struct block`** (128
> bytes, LFH, USED). The sixteen bytes DOOM read as its header were the tail of
> a neighbouring string plus that block header. It was freeing a pointer its own
> allocator never returned.
>
> `DOOMx64vk.exe+0x19f4d40` allocates 88 bytes with `operator new`, calls
> `LoadLibraryW(L"Pdh.dll")` and binds five entry points. **The load returned
> NULL** — this tree built `dlls/pdh` as a native ppc64 module with no AMD64
> thunk — so the game took a cleanup path that releases that object through the
> engine's `Mem_Free` rather than the matching `operator delete`. Latent on
> Windows, where the load never fails.
>
> Fixed by `dlls/pdh/pdh.thunks` plus one line of `Makefile.in`. **qconsole
> 14,272 → 34,495 bytes, fatal lines 1 → 0, three of three runs.**
>
> **Stated as a negative:** thunk info version 7 is neither cause nor cure. The
> fatal error was already absent at `045dc35e10a`, and `WINEEMUNOARGWIDTH=1` did
> not bring it back.
>
> **The new wall**, and it is a different problem: DOOM now reaches weapon
> loading (`idHands::Init - animweb player/fp_hands ... pistol_9mm`) and stops on
> `err:seh:call_seh_handlers invalid frame ... Exception frame is not in stack
> limits` — an SEH registration frame outside the thread's stack bounds,
> plausibly the guest/native stack duality.
>
> **The class, which is the part worth carrying forward:** a module reached only
> through `LoadLibrary` appears in **no import table**, so
> `ppc64le/thunks/check-import-chain.sh` is structurally blind to it. That is why
> this cost weeks of looking at the heap.
> `ppc64le/thunks/check-optional-module.sh` covers the runtime-probed surface.

Run recipe used throughout, deliberately **without** `SteamAppId` so that no
account presence is signalled — `steam_api64` failing its init is the expected
wall for these runs, and what is being measured is everything before and
around it:

```sh
env -u DISPLAY \
    STEAM_COMPAT_DATA_PATH=/mnt/caution/steamapps/compatdata/<appid> \
    WINEFEXBRIDGE=$HOME/projects/fex-emu-ppc64le/src/build-smc/Source/Tools/FexBridge/libfexbridge.so \
    WINEDLLOVERRIDES=winedbg.exe=d \
    timeout -k 5 240 ~/.local/share/Steam/compatibilitytools.d/wine-ppc64le-native/proton \
    waitforexitandrun <exe>
```

The compat tool writes the whole run to
`$STEAM_COMPAT_DATA_PATH/wine-ppc64le-native-<ts>-<pid>.log` and uses its own
prefix, `$STEAM_COMPAT_DATA_PATH/pfx-ppc64le-native`; it never touches the
app's own `pfx`. The Steam library itself is read-only and stayed byte-identical.

---

## What this pass changed

Fifteen modules had **no guest thunk at all**. Each now has a `.thunks` file
and a line in its `Makefile.in`'s `SOURCES` — the same one-line fix
`dlls/dbghelp/dbghelp.thunks` and `dlls/iphlpapi/iphlpapi.thunks` already
carry, and for the same reason. Nothing in them is hand-written: the export
list comes from each module's own `.spec`.

| Module | Wanted by | `PROBE-EXTRA` | Emitted / eligible |
|---|---|---|---|
| `d3dcompiler_43` | Boltgun, Styx | — | 11 / 12 |
| `dwmapi` | Boltgun | `dwmapi.h` | 24 / 24 |
| `normaliz` | Boltgun | — | 5 / 5 |
| `dxva2` | Boltgun | `d3d9.h`, `dxva2api.h` | 2 / 37 |
| `uiautomationcore` | Boltgun | `uiautomationcore.h`, `uiautomationcoreapi.h` | 34 / 37 |
| `wldap32` | Boltgun | `winldap.h` | 169 / 244 |
| `xapofx1_5` | Boltgun, Styx | — | 0 / 1 |
| `d3dx11_43` | Styx | `d3dx11.h` | 24 / 25 |
| `psapi` | Styx, its `EasyHook64.dll` | — | 0 / 27 |
| `msvcr100` | Styx + 12 of its own DLLs | `locale.h`, `setjmp.h`, `sys/utime.h`, `sys/timeb.h`, `mbstring.h`, `mtdll.h` | 766 / 1185 |
| `msvcp100` | Styx | — | 0 / 46 |
| `rpcrt4` | Styx's `wxmsw28u_core…dll` | — | 260 / 317 |
| `mscoree` | Styx | — | 2 / 48 |
| `faultrep` | Styx | — | 0 / 14 |
| `msacm32` | Styx's `fmodex64.dll` | `mmreg.h`, `msacm.h` | 42 / 43 |

A module that emits **zero** exports is still the fix: the module *loads*, the
image gets past `loader_init`, and each import binds a sentinel that faults by
name. That is the whole distance between "never started" and "stopped
somewhere you can read".

Ordering matters in the `PROBE-EXTRA` lines and three of these prove it:
`dxva2api.h` names `D3DFORMAT` and needs `d3d9.h` first; `uiautomationcoreapi.h`
names `PROPERTYID` and needs the widl-generated `uiautomationcore.h` first;
`msacm.h` names `PWAVEFILTER` and needs `mmreg.h` first. Without the
predecessor the header does not compile at all and *every* export is refused.

### A finding: the signature oracle resolves a NAME, and headers rename

`tools/spec2thunk`'s oracle looks up the export name a `.spec` publishes and
asks Wine's headers for a declaration *under that name*. A header is entitled
to declare the implementation under a different one, and two modules in this
pass do exactly that — for different reasons, which is what makes it a class
rather than a quirk:

* **`psapi.dll` — all 27 exports.** `include/psapi.h` sets `PSAPI_VERSION 2`
  itself and then `#define`s every one of its 27 names to a `K32*` equivalent
  *before* declaring them. What the translation unit ends up declaring is
  `K32GetProcessMemoryInfo`; the oracle asks for `GetProcessMemoryInfo` and
  finds nothing. Adding `psapi.h` as a `PROBE-EXTRA` changes not one verdict.
* **`wldap32.dll` — 75 of 244, including ten of the sixteen ordinals Boltgun
  imports.** `include/winldap.h` declares `ldap_initA`/`ldap_initW` and
  `#define`s the undecorated `ldap_init` through Wine's own
  `WINELIB_NAME_AW()`, ~120 times over.

Neither is a shape the descriptor cannot express — both are ordinary integer
arguments and integer returns. The fix belongs in the oracle (follow a
declaration the header aliases the export name to), not in fifteen `.thunks`
files asserting signatures by hand. Until then each keeps its ordinal as a
named sentinel, which is the honest failure. **Not filed as a game wall: no
run has called one yet.**

### Honest refusals that are NOT the oracle's fault

Worth separating, because they will not go away when the above is fixed:

* `uiautomationcore.UiaRaiseAutomationPropertyChangedEvent` takes two
  `VARIANT`s **by value** — 16 bytes, wider than the one 64-bit slot this
  descriptor can carry. Named sentinel, by design, exactly like the documented
  `oleaut32.#113`.
* `msvcr100`'s 77 varargs (the printf/scanf family) need the `variadic=`
  column and a per-name v-variant.
* `msvcr100`'s `div`/`ldiv`/`lldiv`/`_cabs` return or take a struct by value —
  the same rule that makes `ucrtbase.ldiv` a documented hole.
* `msvcp100`'s 46 eligible exports are MSVC-mangled C++ members. No Wine
  header declares them as flat prototypes because they are not flat
  prototypes; they are a C++ ABI. See the handoff below.

---

## Per-title ledger

### Warhammer 40,000: Boltgun (2005010)

* **Exe:** `/mnt/caution/steamapps/common/Boltgun/Boltgun/Binaries/Win64/Boltgun-Win64-Shipping.exe`
  (Unreal Engine). The top-level `Warhammer 40,000 Boltgun.exe` is a 243 KB
  launcher shim with 84 imports and zero holes; the shipping exe is the real
  binary and is what was run.
* **Static audit:** 824 imports, 103 holes, **7 modules with no thunk at all**.
* **Before:** 144 modules loaded, then seven `c000007b` module refusals in
  import-table order — `XAPOFX1_5`, `D3DCOMPILER_43`, `WLDAP32`, `Normaliz`,
  `dwmapi`, `UIAutomationCore`, `dxva2` — and
  `err:module:loader_init Importing dlls for L"…Boltgun-Win64-Shipping.exe"
  failed, status c0000135`, exit rc=53. All seven are ordinary static imports
  of the exe itself, none delay-load, so one was enough. No `Saved/Logs`
  directory was ever created: the UE4 engine's own logging never initialises,
  because no engine code runs. Earlier than any wall this port had recorded.
* **The 96 sentinel-bound holes in `msvcp140`/`vcruntime140`/`ucrtbase`/
  `kernel32`/`setupapi` were never called** — the process died first. They are
  the *next* run's question, not this one's.

**After — the loader is gone as a wall.** Zero `c000007b` in the run. All
seven modules load, 40 modules initialise, every `DllMain` runs, and the
process reaches its own entry point at `0x14542F310` and executes it. It then
dies inside the **Steam DRM stub** — the same bug as Skyrim, root-caused
below; see *One bug, two titles* — with

    err:seh:RtlUserThreadStart failed to emulate AMD64 entry point 000000014542F310, status c0000005

exit rc=5. The distance travelled is the whole loader plus all of DLL
initialisation: from "no guest instruction ever ran" to "the image's first
instruction sequence ran and got four API calls into the DRM preamble".

**What Boltgun imports from those seven modules, and what is now real.** 29
exports in total; 17 are emitted and 12 are named sentinels:

| Module | imported | served | still a sentinel |
|---|---|---|---|
| `dwmapi` | 4 | 4 | — |
| `normaliz` | 1 | 1 | — |
| `dxva2` | 1 | 1 | — |
| `d3dcompiler_43` | 1 | 1 | — |
| `uiautomationcore` | 5 | 4 | `UiaRaiseAutomationPropertyChangedEvent` (VARIANT by value) |
| `wldap32` | 16 | 6 | 10, all of them the A/W macro-rename above |
| `xapofx1_5` | 1 | 0 | `CreateFX` (no Wine header declares it) |

**Not one of the twelve has been called**, because the DRM stub stops the
process before any of this code runs. That is why none of them was promoted
with a `.spec`-location override: on this project's rule a downgrade is earned
by a measured call, not by anticipation.

### Styx: Master of Shadows (242640)

* **Exe:** `/mnt/caution/steamapps/common/Styx/Binaries/Win64/StyxGame.exe`
  (Unreal Engine 3, 59 MB).
* **Static audit:** 13,993 imports across 23 modules, 42 holes, **10 modules
  with no thunk at all**.
* **Before:** 30 modules loaded — including six of the game's own guest x86-64
  DLLs running as real guest code under the emulator (`steam_api64.dll`,
  `binkw64.dll`, `ApexFrameworkCHECKED_x64.dll`, `libresample_x64.dll`,
  `tbbmalloc.dll`) — then the import walk hit all ten missing modules and
  `loader_init` gave up with `c0000135`, exit rc=53. No `StyxGame/Logs/*.log`
  was ever created: UE3's own log never opens.
* **Every one of the ten was load-bearing.** Eight are direct imports of the
  exe (`D3DCOMPILER_43`, `d3dx11_43`, `XAPOFX1_5`, `MSVCP100`, `MSVCR100`,
  `PSAPI`, `faultrep`, `mscoree`); `RPCRT4` arrives through the bundled
  `wxmsw28u_core_vc_custom_64.dll` and `MSACM32` through `fmodex64.dll`.
  Nothing could be skipped — a failed import of the main image is fatal
  wholesale, not per-feature.
* **Three named-export holes were reached and bound to sentinels** during that
  walk (imported, not yet called): `KERNEL32.SetConsoleScreenBufferSize`
  (takes a `COORD` by value — a real shape refusal), `d3dx9_43.D3DXUVAtlasPack`
  and `D3DXUVAtlasPartition`, and `WININET.InternetSetStatusCallbackW`. The
  last is worth flagging: it takes an `INTERNET_STATUS_CALLBACK`, so if Styx
  ever *calls* it the answer is registration interception (the `FlsAlloc` /
  `WNDPROC` treatment in README), not a plain thunk.
* **The 15 `msvcrt.dll` holes the audit named are irrelevant to this title.**
  `msvcrt.dll` never appears in the run at all: Styx's CRT is `MSVCR100`. The
  operator-new/delete, `_CxxThrowException`, `__CxxFrameHandler` and the data
  exports `__badioinfo`/`__pioinfo`/`_iob`/`__mb_cur_max` are a different
  title's problem. (They remain a genuine design item when one arrives: a trap
  stub cannot serve a *data* export, because there is no call to trap on.)

**After — three walls knocked down in sequence, each one revealing the next.**
This is the flywheel working exactly as intended, and each step was measured
rather than predicted:

1. **The loader.** Zero `c000007b`. Modules loaded went 30 -> 59. The image
   gets past `loader_init` for the first time and begins `PROCESS_ATTACH`.
2. **`MSVCR100._initterm_e` `bctrl`'d into guest code.** Its own bundled
   `wxmsw28u_vc_custom_64.dll` reached `DllMain`, called `_initterm_e` with an
   initializer table at `0x1001CCBC0..0x1001CCBD0`, and *native* msvcr100
   walked it and jumped into the first entry:

       dispatch_exception code=c0000005 addr=00000001001BE2E4
         iar=00000001001be2e4  ctr=00000001001be2e4  lr=00003fffff1cc900

   `ctr` and `iar` are the same **guest** address; `lr` is inside native
   `msvcr100._initterm_e`. This is precisely the `_initterm` failure README
   already records for `msvcrt` and `ucrtbase` — in the one C runtime that had
   never had a guest thunk, and so had never been given the same treatment.
   **Fixed**: `GUEST-IMPL _initterm/_initterm_e -> __wine_guest__initterm{,_e}`
   in `msvcr100.thunks`, plus the two `-arch=ppc64` forwards in
   `msvcr100.spec` that `msvcrt.spec` and `ucrtbase.spec` already carry.
3. **`MSVCR100._malloc_crt` sentinel, called.** With the initializers running
   as guest code, the next thing that happened was the port's own designed,
   legible failure:

       err:seh:dispatch_guest_exception guest called through a wild pointer:
         00000000DEAD0008 is in no guest image; the call was made from
         00000001001BE2F5 = L"wxmsw28u_vc_custom_64.dll"+ae2f5

   **Fixed**: one `.spec`-location override. Nothing was guessed —
   `msvcr100.spec:1243` is `@ cdecl _malloc_crt(long) malloc`, so it *is*
   `malloc`, and `dlls/msvcrt/heap.c:430` is the implementation the forward
   names.

**Where it stops now: `MSVCR100.__dllonexit`, and this one was deliberately
NOT fixed.** `__dllonexit` registers a function pointer that native code will
`bctrl` later, at detach. Serving it with a plain thunk would take a sentinel
that faults *by name, now* and turn it into a raw guest pointer stored in a
native table that crashes *illegibly, later* — the exact `FlsAlloc` failure
README documents. The right fix is a row in the registration-interception
table at `dlls/ntdll/signal_ppc64.c:4777`, which today reads

    { L"msvcrt.dll",   "qsort",   4, NULL, 1u << 3 },
    { L"ucrtbase.dll", "qsort",   4, NULL, 1u << 3 },

and has no `msvcr100.dll` row at all. That file is not this change's to edit.
See the handoff below — it is the highest-value single item Styx produced.

Also raised in passing: a hundred exports of `msvcr100` were named sentinels
purely because six headers were not in the signature oracle's translation unit
(`locale.h`, `setjmp.h`, `sys/utime.h`, `sys/timeb.h`, `mbstring.h`, and
`mtdll.h` via `INCLUDE-DIR ../msvcrt`). Emitted went 666 -> 766, recovering
`setlocale`, `localeconv`, `_configthreadlocale`, `longjmp`, `_ftime64`,
`_ismbblead` and — the ones that matter most — the CRT's own `_lock`/`_unlock`.

### The Elder Scrolls V: Skyrim Special Edition (489830)

* **Exe:** `/mnt/caution/steamapps/common/Skyrim Special Edition/SkyrimSE.exe`
  (37 MB). 29 modules mapped, 25 of them AMD64 guest images; the loader
  completes, every guest `DllMain` runs, CRT init runs, `steam_api64.dll` and
  `bink2w64.dll` load.
* **Static audit:** 798 imports, 106 holes, **no module missing a thunk**. None
  of the 15 modules this pass added is needed by Skyrim, and **none of the 106
  holes is reached** — the process dies before the CRT entry point runs. No
  `PROBE-EXTRA` line and no `.thunks` change helps this title at all.
* **It stops at:** `err:seh:RtlUserThreadStart failed to emulate AMD64 entry
  point 000000014383D310, status c0000005`, exit 5.

#### One bug, two titles — the Steam DRM stub calls a NULL pointer

`0x14383D310` **is** `SkyrimSE.exe`'s `AddressOfEntryPoint` (`ImageBase
0x140000000` + RVA `0x383D310`) — so this was never a fresh thread, a TLS
callback, or a sentinel. But it is **not** `mainCRTStartup`: it is the entry of
the **SteamStub v3.1 DRM wrapper**, in a 9th section named `.bind`
(va `0x0383D000`, vsize `0x32918`, characteristics `0x60000000` — read+execute,
no `CNT_CODE`), fully initialised on disk. Its encrypted header decrypts to,
among other fields, `Signature 0xC0DEC0DF`, `SteamAppID 0x00077966` (= 489830)
and `OriginalEntryPoint 0x0153BC64` — so Skyrim's real CRT entry is
`0x140153BC64` and not one instruction of the game itself ever runs.

The stub's first act is the canonical SteamStub anti-debug block:

```c
IsDebuggerPresent();
h  = GetModuleHandleA("ntdll.dll");                       /* -> NULL here */
fn = GetProcAddress(h, "NtSetInformationThread");         /* -> NULL, refused */
fn(GetCurrentThread(), ThreadHideFromDebugger, NULL, 0);  /* -> execute fault at 0 */
```

and the port's own log shows every step of it, including the refusal it is
right to make and the name it was asked for (recovered at runtime from
`emu_GetProcAddress`'s refusal branch):

    find_guest_thunk_target L"KERNEL32.dll".IsDebuggerPresent
    find_guest_thunk_target L"KERNEL32.dll".GetModuleHandleA
    find_guest_thunk_target L"KERNEL32.dll".GetProcAddress
    warn:seh:emu_GetProcAddress GetProcAddress(0000000000000000) is not a guest module, refusing
    ### REFUSED-GPA mod=(nil) name=NtSetInformationThread
    find_guest_thunk_target L"KERNEL32.dll".GetCurrentThread
    handle_syscall_fault code=c0000005 flags=0 addr=(nil) pc=0
    handle_syscall_fault  info[0]=0000000000000008  info[1]=0000000000000000

`info[0] = 8` is `EXCEPTION_EXECUTE_FAULT` and `info[1] = 0`: an **instruction
fetch at address 0**, not a bad data access. The stub does what the
DOOM/`CChromaEditorLibrary` case in README does — discards the failure and
calls anyway.

**THE ROOT CAUSE: this port's guest module namespace never contains
`ntdll.dll`.** `emu_GetModuleHandleA` (`dlls/ntdll/signal_ppc64.c:1220`)
delegates to `find_guest_module` (`:1186`), which walks
`Peb->LdrData->InMemoryOrderModuleList` for `Machine == IMAGE_FILE_MACHINE_AMD64`
and **never loads** — correctly, because `GetModuleHandle` must not load. So
the guest namespace holds only what static imports dragged in, which for
Skyrim is 25 images and `grep -c 'sysx8664.*ntdll'` = **0**. On Windows
`ntdll.dll` is mapped into every process before anything executes;
`GetModuleHandleA("ntdll.dll")` *cannot* fail there, and a great deal of real
software relies on that.

The guest module already exists and is already correct:
`dlls/ntdll/x86_64-windows/ntdll.dll`, machine `0x8664`, **960 named exports**,
`NtSetInformationThread` among them as a genuine trap stub. It is simply never
loaded.

**And it is the same bug in Boltgun, not merely the same message.** Boltgun's
entry `0x14542F310` also lands in a `.bind` section (va `0x0542F000`, vsize
`0x32518`, characteristics `0x60000000`), and **the first 64 bytes at its entry
are byte-identical to Skyrim's**. The guest stack geometry matches to the byte
across the two titles: `guest rsp=…DD08`, module-name argument `…DD50`,
proc-name argument `…DD60`. The `0x310` both entry RVAs end in is the stub
header's `BindSectionOffset` field, not a coincidence.

Two titles, one cause, and it will recur on **every Steam-DRM-wrapped title**
in any game list. That makes it the highest-value single item this pass produced.

**Ruled out, with evidence, so nobody re-treads it:** TLS callbacks — Skyrim's
TLS callback array at `0x141766000` has a zero first entry, i.e. no callbacks.
Missing-import sentinels — no `DEAD00` and no `wild pointer` appears anywhere
in any Skyrim run log. A "poisoned" DRM thread — the stub is intact and
behaving exactly as designed; it is the port that answers it wrongly.

### The 32-bit refusals — Half-Life 2, FreeInfantry, Styx Win32

> **[SUPERSEDED IN PART, 2026-08-30.] Two things below are no longer true.**
>
> First, **this port does have a 32-bit guest now** — Dex reached real gameplay
> on it on 2026-08-28. The i386 lane is not a work item any more; specific
> marshal surfaces on it are.
>
> Second, and more importantly for anyone planning work from this section:
> **the Half-Life 2 family and Portal are not Windows builds on this machine.**
> Checked with `file(1)` on 2026-08-30 — appids 220, 340, 380, 420 and 400 all
> install `hl2_linux`, an **ELF 32-bit i386** binary, and ship their own
> `bin/libdxvk_d3d9.so`. There is no `.exe` anywhere in those directories. They
> never reach Wine's `d3d9.dll` at all; they would run through the emulator's
> ELF path against Valve's own bundled DXVK. The `hl2.exe` cited below was a
> Windows install that is no longer what Steam has put on disk.
>
> **Portal 2 (appid 620) is the exception** and the only installed PE32 Source
> title: `portal2.exe`, with a Linux build beside it. So D3D9 marshal work on
> the i386 lane gates exactly ONE installed title, not six. Plan accordingly —
> an earlier framing of this as a six-title unlock was wrong, and was corrected
> only because someone ran `file` instead of counting directories.

All three were PE32/i386 and all three produced the **byte-identical** refusal
at the time this was written.

Verbatim, for each of `hl2.exe`, `FreeInfantry.exe` and
`Styx/Binaries/Win32/StyxGame.exe`:

```
Application could not be started, or no application associated with the specified file.
ShellExecuteEx failed: Bad format.
```

with `game exited rc=1`.

**Which layer refuses, traced through source rather than guessed:**

1. `dlls/ntdll/unix/loader.c:load_main_exe()` → `open_main_image()` →
   `virtual_map_module()`.
2. The status comes from the **wineserver**: `server/mapping.c:get_image_info()`
   returns `STATUS_INVALID_IMAGE_FORMAT` (`c000007b`) because
   `is_machine_supported(IMAGE_FILE_MACHINE_I386)` is false.
3. `server/registry.c:init_supported_machines()`, `__powerpc64__` branch, lists
   `{ POWERPC64, AMD64 }` and says why in a comment: *I386 is deliberately not
   here… Add I386 when there is a 32-bit guest story, not before.*
4. `load_main_exe()` then falls back to stock Wine's `programs/start/start.exe`,
   whose `GetBinaryTypeW()` hits the same `c000007b`, skips `CreateProcessW`,
   and lands on `ShellExecuteExW` — which produces the two lines above,
   verbatim from `programs/start/start.rc`.

Everything from step 4 on is **stock upstream Wine, unmodified**, faithfully
reproducing what Windows says when nothing can open a file.

**The finding for the 32-bit work item is the mismatch between the two halves
of that.** The *status* trail is loud and named — `supported_machines[]` states
the policy and the reason in a code comment. The *user-visible* text is
Windows' generic "no application associated" message and never says
"32-bit", "i386" or "unsupported architecture" anywhere. Someone reading only
the terminal cannot tell an architecture refusal from a missing file
association. By this project's own standard — loud, named refusals — that is a
gap, and a one-line one: the fallback path knows the machine it rejected and
does not say so.

### Styx: Shards of Darkness

Not installed. `/mnt/caution/steamapps/common/Styx Shards of Darkness/` is an
empty directory and there is no `appmanifest_*.acf` for it. Its compatdata
(`355790`) survives from an earlier install. No action.

### Quake II (2023 remaster) — installed, and the wall is NOT this port

**This entry previously said the title was not installed because the drive
holding it was absent.** That was true on 2026-08-18 and is no longer true:
`appmanifest_2320.acf` is present, the game is 5.3 GB under
`~/.local/share/Steam/steamapps/common/Quake 2/`, and it has been launched
repeatedly since. The old text is kept only in git history — do not act on it.

Current wall, reproducible, `rerelease/quake2ex_steam.exe`:

```
==== InitGame ====
------- Server Initialization -------
0 entities inhibited
0 teams repaired
0 teams with 0 entities
Standard exception caught in kexPlatformApp::Main: index
```

**[MEASURED 2026-08-30] The fully emulated lane fails IDENTICALLY.** The
game's own `stdout.txt` from the Proton prefix
(`steamapps/compatdata/2320/pfx/.../Saved Games/Nightdive Studios/Quake II/`)
ends on the same line, at the same point, after the same three
server-init messages. Real x86-64 Wine under emulation hits it too. **That
rules out this port as the cause** — no amount of thunk, marshalling or
COM work will move it, and it should not be triaged as a port defect.

Diffing the two lanes' `stdout.txt` is the cheapest tool here and is worth
repeating for other titles. It surfaced three native-lane gaps that are
**real but are NOT this failure**, since the emulated lane has none of them and
dies anyway:

| symptom | native | emulated |
|---|---|---|
| `kexAudioXA2::Init` | **fails**, empty reason string | `Sound System Initialized` |
| video displays | 1 | 2 |
| joystick | `not supported on this machine` | (no message) |
| extra missing `vkGetInstanceProcAddr` entries | 9 more | — |

Those are separate work items. Recording them here so they are not
rediscovered as "the Quake II bug".

**Hypotheses eliminated so far** — each measured, not argued:

1. **Processor count / processor groups.** Both lanes report
   `Max Worker Threads: 175` and `Max Logical CPU Cores: 176`, and the
   exception message is literally `index`, so a fixed per-core array was the
   obvious suspect — the same shape as the 2×40 view Cyberpunk 2077 refuses.
   `WINE_PPC64LE_CPU_LIMIT=64` in an appconfig **did** take effect (the game
   then reported `Max Logical CPU Cores: 64`, `RelationNumaNode count: 1`) and
   the game **still threw the identical exception**. The appconfig was deleted
   rather than left behind as a knob that does nothing.
2. **Steam client presence.** A Steam client is running on this box — Linux
   Steam under the emulator — and the emulated-lane run had full working
   Steam integration. It failed anyway. Note for other titles: Linux Steam
   publishes **no Win32 named kernel objects**, so anything probing for
   `Local\SteamStart_SharedMemFile` can never be satisfied by starting the
   client.

**[MEASURED 2026-08-30] The `+seh` trace was taken** (4.3M lines). What it
established, and what it did not:

The throw is a single C++ exception — `code=e06d7363`, and there is exactly
**one** in the whole run, so nothing is being thrown and swallowed earlier. It
is caught by handlers inside the game's own image (`ImageBase 140000000`, i.e.
`quake2ex_steam.exe`) at RVA `0x315c22`, with an outer frame at `0x11e78b`.

The trace also surfaced two lines that `stdout.txt` alone does not show, and
**both appear on the emulated lane too**, which is the bar any candidate cause
has to clear:

```
kexEngineLocal::InitCPUUtilization - No performance data available
playfabUser_s: userState_e::Nill -> userState_e::DoingSignIn
```

On the first of those: the game really does use PDH. The trace shows it binding
`PdhOpenQueryW`, `PdhAddEnglishCounterW` and `PdhCollectQueryData`. Wine's
`dlls/pdh` is a hardcoded table of exactly **two** counters —
`\Processor(_Total)\% Processor Time`, whose collector returns a literal
`500000 /* FIXME */`, and `\System\System Up Time`. There are no
per-processor instances, so an `AddEnglishCounter` for anything else returns
`PDH_CSTATUS_NO_COUNTER`. That is consistent with the engine's message.

**A guess of mine that the evidence killed, recorded so nobody repeats it:** I
expected the engine to ask for `\Processor(*)\...` and be defeated by
`PdhExpandWildCardPathW` being a stub. **It never calls that function.** The
stub is real but irrelevant here.

**Two live hypotheses. The first is now MEASURED at the PDH layer:**

1. *PDH — the gap is real and fully characterised.* `WINEDEBUG=+pdh`, run
   `20260830-160905`: the engine opens one query and then asks for **176
   per-processor counters**, `\Processor(0)\% Processor Time` through
   `\Processor(175)\% Processor Time` — one per logical CPU — and **never
   asks for `_Total`**.

   Wine's `dlls/pdh` counter table contains exactly two entries, and the only
   processor one is `\Processor(_Total)\% Processor Time` (whose collector
   returns a literal `500000 /* FIXME */`). **There are no per-instance
   processor counters at all**, so all 176 adds fail with
   `PDH_CSTATUS_NO_COUNTER` and the engine logs "No performance data
   available".

   This retroactively explains why the processor-group cap did nothing: at 64
   cores the game asks for 64 counters instead of 176, and **every one of them
   fails either way**. The cap changed the count, not the outcome.

   **The fix is a Wine fix, not a port fix**, and it is well scoped: add
   per-instance `\Processor(N)\% Processor Time` sources backed by real data.
   `NtQuerySystemInformation(SystemProcessorPerformanceInformation)` is already
   implemented and carries exactly the per-CPU times needed. This would benefit
   any title that reads per-core utilisation, not only this one.

   **Still NOT established:** that the empty array is what throws. The run that
   captured the PDH evidence exited `rc=3` before reaching the exception, so
   the last link — empty container, later indexed — remains inference.
2. *PlayFab.* Sign-in enters `DoingSignIn` and the log records no subsequent
   state transition; the throw follows about 20 seconds later, which has the
   shape of a network timeout leaving an empty user/entitlement container.
   Test: compare behaviour with the network unreachable, and look for a state
   change that never arrives.

**Honest status: characterised, not solved.** The single most valuable thing
established is the elimination — this is a Wine-level gap present in both
lanes, not a defect in this port, so it should not consume port-side effort
until someone wants this specific title.

## The Steam overlay — what would have to happen, and what does today

**Nothing today. The overlay is not injected, and one of the two roads to it is
closed deliberately.** This section is the mechanism written down, because it
crosses two worlds and neither half is obvious.

There are two overlays, not one, and Steam picks between them by what kind of
process the game is:

* **`gameoverlayrenderer.so`** — x86-64 **Linux** ELF
  (`~/.local/share/Steam/ubuntu12_64/`, also under `steamrt64/`). The Steam
  client `LD_PRELOAD`s it into the whole launched process tree. It hooks the
  **host** GL/EGL/Vulkan entry points and draws the overlay after the game's
  own frame. This is the road Proton uses for a Windows game, because under
  Proton the process that talks to the GPU *is* a Linux process.
* **`GameOverlayRenderer64.dll`** — x86-64 **PE**
  (`~/.local/share/Steam/legacycompat/`, and Proton copies it into
  `drive_c/Program Files (x86)/Steam/` in every prefix it builds — see
  `proton`'s `filestocopy` list). This is the Windows-side overlay, loaded
  *inside* the game process, hooking D3D/DXGI at the application's own level.

**Road one is closed by this port, on purpose, and the reason is in the code.**
`ppc64le/steamtool/proton` strips `LD_PRELOAD` (with `LD_LIBRARY_PATH`,
`LD_AUDIT` and the rest) out of the environment Steam hands it, because the
value Steam sets names **x86-64** shared objects and the processes this port
starts are **native ppc64le**: leaving it in produces a stream of "wrong ELF
class" errors from the host `ld.so` on every process the port starts, including
`wineserver`. That is not a decision that can be reversed by keeping the
variable — the object is the wrong machine for the process that would load it,
and there is no ppc64le build of it. **The host-side overlay cannot work here at
all, ever, unless Valve ships a ppc64le `gameoverlayrenderer.so`.**

**Road two is untried, and it is the one that could work.** The Windows-side
overlay is an x86-64 PE, which is exactly the machine this port already runs as
a guest: `msvcp140.dll` and every game's own DLLs load and execute the same way.
So `GameOverlayRenderer64.dll` is not categorically out of reach the way its
Linux sibling is. Three things stand between here and there, and only the first
is small:

1. **It is never staged into the prefix.** Proton copies it (and
   `steamclient.dll`, `Steam.dll`, `SteamService.exe`) into
   `drive_c/Program Files (x86)/Steam/`; `ppc64le/steamtool/proton` copies none
   of them, because this port serves `steamclient64.dll` from its own builtin
   (`dlls/steamclient64`) and never needed a file on disk. Adding a copy is a
   handful of lines in the same place the MFC-runtime copy already lives.
2. **Nothing loads it.** On Windows the Steam client injects it; under Proton
   the `steam.exe` helper `LoadLibrary`s it. This port has no helper `.exe` —
   `dlls/steamclient64`'s `DllMain` does the job the helper used to do (it
   writes `ActiveProcess\PID` from inside the game, which is Proton's trick
   minus the helper). A `LoadLibraryW( L"GameOverlayRenderer64.dll" )` from that
   same `DllMain` is the natural place, and it would go through the guest
   loader like any other guest module.
3. **Its imports have to bind, and they very nearly all do.**
   [MEASURED] 2026-08-18, `ppc64le/thunks/import_chain.py` on
   `~/.local/share/Steam/GameOverlayRenderer64.dll`: a PE32+ x86-64 DLL with
   **280 imports across ten modules** — `kernel32` (166), `user32` (59),
   `imm32` (16), `advapi32` (15), `gdi32` (10), `ole32` (6), `cfgmgr32` (3),
   `oleaut32` (2), `psapi` (2), `winmm` (1).

   Nine of the ten already had a guest thunk. **`cfgmgr32` had no `.thunks`
   file at all**, which on this port is the fatal shape rather than the
   survivable one — a missing MODULE fails the whole import walk before any
   guest code runs, while a missing EXPORT only binds a sentinel. That was a
   one-line fix of exactly the kind the fifteen modules above got, and it is
   made: `dlls/cfgmgr32/cfgmgr32.thunks`. (cfgmgr32 is ordinary furniture, not
   a Steam thing: any guest DLL that enumerates devices imports it.)

   With that in, the audit is down to **two holes, both in `psapi`** —
   `GetModuleInformation` and `GetModuleBaseNameA`. Both are the
   `PSAPI_VERSION`/`K32*` macro-rename documented under "the signature oracle
   resolves a NAME, and headers rename" above: `include/psapi.h` `#define`s
   every one of its 27 names to a `K32*` equivalent before declaring it, so the
   oracle asks for `GetModuleInformation` and the translation unit only has
   `K32GetModuleInformation`. They are named sentinels, and an overlay is
   exactly the kind of code that calls them — it walks the loaded modules to
   find the graphics API to hook. So handoff 5 below ("follow a header's alias
   of an export name") is not an abstract tidy-up any more: it is the last
   thing between this DLL and loading.

**What it would then do, and why nobody should assume it works.** The overlay
hooks the graphics API *inside* the game. On this port the game's D3D11 calls
leave the guest through a thunk stub and land in native `d3d11.dll`, so a
detour installed on the **guest** side of that boundary — an IAT patch on
`d3d11.dll`, or a vtable patch on the swapchain proxy — still sees every call,
because the proxy vtable the guest holds *is* guest code
(`libs/winecom`'s trap-stub array). A detour installed by writing x86-64 jump
bytes over the entry of what it believes is `IDXGISwapChain::Present` would land
on a trap stub, which is five bytes long. That is the interesting question and
it is genuinely open: the overlay's hooking style decides whether it composes
with this port's proxies or corrupts them, and nothing here has measured it.

**Recorded as INCOMPLETE.** Not "not needed yet": the overlay is what tells a
player their game is running under Steam at all, and the first two steps above
are small. The third is a real investigation and it has not been done.

---

## Handoffs

Ordered by value. The first two are the ones worth doing next.

### 1. To the guest loader: `ntdll.dll` must always be in the guest namespace

> **CLOSED 2026-08-18, by commit `39f8835393e`** — `dlls/ntdll/loader.c:4902` seeds
> every AMD64 guest process's namespace with `ntdll.dll` once, exactly as this
> handoff asked. Nobody re-ran the titles afterwards, so here is the measurement,
> headless, with this file's own recipe and a 180-second bound:
>
> | | before | after |
> |---|---|---|
> | Skyrim SE (489830) | `c0000005` at the image entry, 0 guest instructions | **rc=124** — past the stub, past `loader_init`, through display-driver and keyboard-layout setup, still alive when the bound fired |
> | Boltgun (2005010) | same fault, byte-identical stub | **rc=124** — same, plus the Common-Controls manifest `fixme` |
>
> Neither run shows `handle_syscall_fault pc=0`, and neither shows the wild-pointer
> fault any more. The expected next wall named below — the stub's Steam checks —
> was not reached inside the bound; what either title is doing after keyboard-layout
> setup is not established, and that is the next measurement rather than a claim.

**This was the single highest-value item in that pass: two titles, one cause,
and it would have recurred on every Steam-DRM-wrapped title.** Full evidence in the
Skyrim entry above.

`find_guest_module()` (`dlls/ntdll/signal_ppc64.c:1186`) walks
`Peb->LdrData->InMemoryOrderModuleList` for AMD64 images and never loads —
which is correct, because `GetModuleHandle` must not load. The consequence is
that the guest namespace contains only what static imports dragged in, and
**`ntdll.dll` is never among them**. On Windows `ntdll.dll` is mapped into
every process before anything executes, so `GetModuleHandleA("ntdll.dll")`
cannot fail; software relies on that, and SteamStub is only the first example.

The fix is not in a fenced file and does not touch `find_guest_module`: seed
the guest namespace with `ntdll.dll` once, when the guest machine is first
brought up, through the existing
`load_guest_dll( L"ntdll.dll", IMAGE_FILE_MACHINE_AMD64, &mod )`
(`dlls/ntdll/loader.c:3792`). The guest module is already built and already
correct — `dlls/ntdll/x86_64-windows/ntdll.dll`, machine `0x8664`, 960 named
exports including `NtSetInformationThread` as a real trap stub. Special-casing
ntdll *inside* `find_guest_module` is the worse alternative twice over: it
lands in a fenced file, and it would make `GetModuleHandle` load, which it
must never do.

Expected next wall after the fix: the stub's Steam checks
(`Local\SteamStart_SharedMemFile`, `Local\SteamStart_SharedMemLock`,
`"Steam Error"`, `"Application load error X:XXXXXXXXXX"` — all recovered from
its decrypted string table), i.e. DRM-shaped rather than structural.

### 2. To the callback-registration table: `msvcr100.dll` has no rows

`dlls/ntdll/signal_ppc64.c:4777` is the per-module, per-API interception table
that stops a raw guest function pointer from reaching native code that will
`bctrl` it. It has rows for `msvcrt.dll` and `ucrtbase.dll` and **none for
`msvcr100.dll`**, which did not exist as a guest module until this pass.

Styx: Master of Shadows is blocked on it right now. Its bundled
`wxmsw28u_vc_custom_64.dll` calls `MSVCR100.__dllonexit` during
`PROCESS_ATTACH`, and Styx additionally imports `qsort`, `bsearch`,
`_beginthread`, `_beginthreadex`, `_onexit`, `_set_invalid_parameter_handler`,
`_set_purecall_handler` and `__setusermatherr` from the same module. Every one
of them hands over a guest pointer.

`msvcr100.thunks` deliberately leaves `__dllonexit` as a named sentinel rather
than serving it: a plain thunk here would convert a fault that names itself
*now* into a native table full of raw guest pointers that crashes *later*,
which is the `FlsAlloc` failure README already documents.

**CLOSED.** All eight now have rows. `qsort`, `bsearch` and `_onexit` landed
first; the remaining five — `_beginthread`, `_beginthreadex`,
`_set_invalid_parameter_handler`, `_set_purecall_handler` and
`__setusermatherr` — are in the table now for all three CRT modules that
actually export each one, which is not the same set for each: `msvcrt.dll`
exports neither handler setter (it has `_invalid_parameter` and `_purecall`,
which are the *call* sites), and `ucrtbase.dll` additionally publishes `_o_*`
forwarders that a name-keyed row would have missed. Two of the five could
not be got right by inspection alone:

* `_beginthread`/`_beginthreadex` are **not** reached by the invocation-time
  thread-start interception that covers `CreateThread`. `dlls/msvcrt/thread.c`
  hands `CreateThread` its own *native* trampoline and calls the guest routine
  from inside it, so the entry point the port classifies is native and the
  guest pointer is never inspected;
* `_set_invalid_parameter_handler`'s callback takes **five** arguments, and
  the trampoline pool puts the guest target in the register one past the last
  real one — r7 at the default four-argument arity, which is exactly where
  native `_invalid_parameter` has already put `pReserved`. A default-arity row
  would not have faulted; it would have handed the handler a pointer where a
  reserved value belongs. That is what the `cb_argc` column added for it
  prevents.

The gate is `ppc64le/seh/check-crt-callbacks.sh`: one source built twice, run
as a native ppc64 PE and as an x86-64 guest, nine value-checking steps plus a
separate pure-virtual run, transcripts byte-identical, and red under
`WINEEMUNOCBWRAP=1`.

### 3. To the port's own legibility: name the fault, not the run's start

> **CLOSED 2026-08-18, by commit `9fc52010e0c`.**

`dlls/ntdll/signal_ppc64.c:5625` prints

    ERR( "failed to emulate AMD64 entry point %p, status %08x\n", entry, status );

where `entry` is the address the run loop was *entered* with, not where the
guest died. It reads as "the entry point is bad" when what happened is "the
guest ran from here and faulted somewhere else entirely". It cost this
investigation its opening premise and a chunk of a day: the brief for Skyrim
began from "a fresh thread whose entry fails", which was a reasonable reading
of the message and wrong in every particular. It should name the faulting
guest RIP, or say plainly that it is naming the run's start.

Related, and worse: a guest call through a NULL pointer arrives as a host
`SIGSEGV` with NIP `0`, so `p_fexbridge_fault_is_jit()` says no,
`emu_handle_fault()` (`dlls/ntdll/unix/loader.c:1332`) stashes no record, and
the guest gets **no `EXCEPTION_RECORD` at all**. A guest that wraps a null call
in `__try`, or installs a vectored handler, loses the whole process instead of
handling it. The comment inside `emu_handle_fault` asserts that "a guest jump
to unfetchable memory never gets here at all: the bridge turns it into
RUN_FAULT internally without any host signal" — that is precisely the case
that failed here, twice, in two titles.

### 4. To the compat tool: the VC++ 2010 C++ runtime

> **CLOSED 2026-08-18. Styx: Master of Shadows is past `loader_init`.** It now
> loads 58 modules, every DllMain runs — including the
> `wxmsw28u_vc_custom_64.dll` one this handoff was written about — and the game
> stops somewhere else entirely (below). Four things had to be true, and the
> first is that **this handoff's central premise was false**.
>
> **The file is not Microsoft's.** `compatdata/242640/pfx/drive_c/windows/
> system32/msvcp100.dll` is 1,747,524 bytes and machine `0x8664`, both as
> recorded — and its DOS stub reads **`Wine builtin DLL`**, with 1628 exports.
> It is WINE'S OWN msvcp100 built for x86-64 by Proton. So the open question
> this handoff ended on — "why is `image.wine_builtin` set at all for a 1.7 MB
> Microsoft DLL" — dissolves: the flag describes the FILE, the file is a
> builtin, and the flag was right. Nothing was wrong in `load_builtin`'s
> reading of it.
>
> **Which makes `msvcp100=n` the actual defect.** `LO_NATIVE` refuses a builtin
> outright, by the line this handoff quoted. Adding the override could only ever
> turn "a module with no useful exports" into "a module that will not load",
> which is exactly the rc=53 that made the first attempt worse than doing
> nothing. There is no override in the shipped block.
>
> **The real bug was one layer up, and it was measured rather than reasoned
> about.** With the file staged in `C:\windows\sysx8664` and no override at all:
>
> ```
> find_builtin_dll looking for "msvcp100.dll" for file
>   L"\??\C:\windows\sysx8664\msvcp100.dll"
> map_image_into_view mapping PE file
>   L"\??\C:\windows\sysx8664\msvcp100.dll" at ...-0x...3000
>   section .rdata ... section .reloc ...
> ```
>
> — a three-page image wearing the staged file's name. `load_builtin` re-resolves
> the NAME through `find_builtin_dll`, which searches the build and install trees
> and never the prefix, so the 1.7 MB module was opened, machine-checked and then
> thrown away for this tree's exports-less msvcp100 thunk. `dlls/ntdll/unix/
> loader.c` now keeps a file that is already in a GUEST machine's own system
> directory instead of re-resolving it: `find_dll_file`'s guest branch searched
> that directory FIRST on purpose, and `load_builtin` was undoing the decision.
> The discriminator is the mapping's own nt_name (the SERVER's record of the file
> the section came from), and it was checked both ways — an ordinary thunk
> arrives as `L"\??\Z:\home\...\dlls\msvcr100\x86_64-windows\msvcr100.dll"` and
> does not match; only a staged file arrives as its `sysx8664` path.
>
> **And the sentinel this handoff blamed on msvcp100 was msvcr100's.** With
> msvcp100 staged, all 65 of Styx's std:: imports bind and the
> "No implementation for MSVCP100.dll…" warnings go to **zero** — and the game
> died in the same place, because `0xDEAD000B` was never msvcp100's number:
>
> ```
> No implementation for MSVCR100.dll.??2@YAPEAX_K@Z imported from
>   wxmsw28u_vc_custom_64.dll, setting to 00000000DEAD000B
> guest called through a wild pointer: 00000000DEAD000B ... from
>   L"wxmsw28u_vc_custom_64.dll"+5d580
> ```
>
> `operator new`. Sentinel indices are per IMPORTING module, so the number says
> nothing about which DLL owns the name — worth remembering, because reading it
> as msvcp100's is what pointed this whole handoff at the wrong module.
> `??2@YAPEAX_K@Z`, `??3@YAXPEAX@Z` and their array forms are flat prototypes
> with a mangled NAME — one integer-class argument, a pointer or nothing back,
> no `this` and no EH state — so they are four rows in
> `dlls/msvcr100/msvcr100.thunks` citing `msvcr100.spec`, not an ABI problem.
> Two tooling gaps stood in the way and both are fixed in `tools/spec2thunk`:
> the "plain C identifier" filter ran before overrides were consulted (it
> protects the header ORACLE, which a `.spec` location does not use), and the
> `-arch=` refusal in `wine_sig.py` was unconditional (it guards against citing
> the arm form of a name that also has a win64 form, so it now accepts a line
> the tool's own `_spec_cpus` filter already proved applicable).
>
> **msvcr100 deliberately stays Wine's own**, and this is now measured rather
> than assumed. The C runtime is the half this port CAN translate — 766 of 1185
> exports cross — and `msvcr100.thunks` carries the callback-registration rows
> `ppc64le/seh/check-crt-callbacks.sh` gates; staging a guest msvcr100 would put
> all of that behind the emulator for nothing. It also does not work: Proton's
> x86-64 msvcr100.dll is a Wine builtin that imports Wine's INTERNAL ntdll
> helpers, which this port's guest ntdll thunk does not publish, and it dies at
> its own PROCESS_ATTACH on
> `guest called through a wild pointer: 00000000DEAD0005 … from msvcr100.dll+7ef80`
> (= `__wine_dbg_header`).
>
> **Where Styx stops now**, and it is a different wall in a different place:
>
> ```
> No implementation for mscoree.dll._CorExeMain imported from
>   Z:\…\Styx\Binaries\Win64\StyxGame.exe, setting to 00000000DEAD0099
> guest called through a wild pointer: 00000000DEAD0099 is in no guest image,
>   and the return address on its stack (…) is in none either
> ```
>
> The .NET hosting entry point, imported and CALLED by StyxGame.exe itself.
> That is not a marshalling or a C++ ABI problem: Wine's own `mscoree` needs
> Mono to do anything with it. `StyxGame/Logs/` is still never created, so UE3's
> own log has not opened yet. What this pass proved is that the whole C++
> runtime wall — the one this handoff was about — is behind us.
>
> The compat-tool half is `ppc64le/steamtool/proton`, beside the `mfc140u`
> block, and it stages msvcp100 only.


### 5. To `tools/spec2thunk`: follow a header's alias of an export name

See "the signature oracle resolves a NAME, and headers rename" above. Two
modules, 102 exports, two different macro mechanisms (`PSAPI_VERSION` and
`WINELIB_NAME_AW`), one cause. Everything needed to fix it is already in the
translation unit the oracle compiles.

### 6. To the 32-bit work item

The refusal is correct and consistent, and the user-visible message does not
name the reason. See above.

---

## Files this pass touched

Nothing is committed; everything below is uncommitted working state.

* **15 new** `dlls/<m>/<m>.thunks` — `d3dcompiler_43`, `d3dx11_43`, `dwmapi`,
  `dxva2`, `faultrep`, `msacm32`, `mscoree`, `msvcp100`, `msvcr100`,
  `normaliz`, `psapi`, `rpcrt4`, `uiautomationcore`, `wldap32`, `xapofx1_5`.
* **15** `dlls/<m>/Makefile.in` — one `<m>.thunks` line added to `SOURCES`
  (`psapi` had no `SOURCES` block at all and gained one).
* **1** `dlls/msvcr100/msvcr100.spec` — two `-arch=ppc64` forwards added
  beside `_initterm_e`, byte-for-byte the pair `msvcrt.spec:563-564` and
  `ucrtbase.spec:429-430` already carry. `GUEST-IMPL` cannot work without
  them. `msvcr100.spec` pins no explicit ordinals, so nothing renumbers.
* **this file**.

## Gates

Green after these changes, on the tree that carries them, re-run after the
`msvcr100` change as well as after the fifteen:

* `ppc64le/thunks/check-import-chain.sh` — PASS; holes still exactly the three
  documented ones, runtime leg PASS.
* `ppc64le/vkd3d/check-ordinal-imports.sh` — PASS; 1388 pinned ordinals across
  21 modules agree with their `.spec`.
* `ppc64le/syscom/check-com-smoke.sh` — PASS 21/21 native and guest,
  byte-identical.
* guest `winepath` — PASS.

The build is warning-free.

---

> ### Handoff #8 — Civilization VI's loader death was MSVCP140 + four UCRT holes,
> ### and the sentinel WAS named all along
>
> **The premise this started from was false, and the false part is the useful
> part.** Civ VI (appid 289070) aborts in `loader_init`:
>
> ```
> err:seh:dispatch_guest_exception guest called through a wild pointer:
>   00000000DEAD0047 ... from EOSSDK-Win64-Shipping.dll+35af79
> err:module:loader_init "EOSSDK-Win64-Shipping.dll" failed to initialize
> ```
>
> A run with `WINEDEBUG=warn+module` had produced **zero** `allocating stub`
> lines, which read like the launcher discarding `WINEDEBUG`. It is not.
> `run-native` and `proton` never touch `WINEDEBUG` (`run-native:32` says so),
> the variable was in that run's own environment dump, and the log already held
> **258** naming lines. **The string is different.**
> `dlls/ntdll/loader.c` has four `allocate_stub()` call sites and all four warn,
> but in two different texts:
>
> * **1298/1304**, inside the `if (!exports)` branch — a module with no export
>   directory **at all** — end on the shared line at **1307**, `" imported from
>   %s, allocating stub %p"`.
> * **1327/1343**, the ordinary per-symbol path every real miss takes, say
>   `"No implementation for %s.%s imported from %s, **setting to** %p"`.
>
> So there is no silent path and no diagnosability gap in the code — only a
> grep that matched the branch nothing hits. **Grep for `setting to`**, or for
> `No implementation for`, which covers all four. The fault and the naming WARN
> also carried the same `0130:` thread prefix, so the sentinel index was
> counting in the process that faulted and `0x47` meant what it looked like:
> the 71st binding, `api-ms-win-crt-heap-l1-1-0.dll._get_heap_handle`.
>
> **What was actually missing.** 258 sentinels over 144 distinct names, and
> only twelve of the 144 were fixable in a `.thunks` file:
>
> | where | what | how |
> |---|---|---|
> | ucrtbase | `_get_heap_handle`, `_heapchk`, `_ftime64` (+`_s`/`_o_` siblings) | `PROBE-EXTRA malloc.h`, `PROBE-EXTRA sys/timeb.h` — real declarations, just not in the oracle's TU |
> | ucrtbase | `tmpnam_s`, `_wtmpnam_s`, `_seh_filter_exe` | the `.spec`-location downgrade, each justified against the implementation it forwards to |
> | vcruntime140 | `__RTtypeid`, `__RTDynamicCast`, `__std_type_info_name`/`_compare`, `__std_exception_copy`/`_destroy`, `__uncaught_exception` | `FORWARD` to the ucrtbase namesakes, which is what `vcruntime140.spec:30-51` already says they are |
> | advapi32 | `CredReadW`/`WriteW`/`DeleteW`/`Free` | `PROBE-EXTRA wincred.h` |
> | wldap32 | ordinal **301** = `ber_free` (+9 `ber_*` siblings) | `PROBE-EXTRA winber.h` |
>
> The other 132 are **MSVCP140**, and no row can serve them: mangled member
> functions, **vtable data** (`??_7ios_base@std@@6B@`) and a **data constant**
> (`?_BADOFF@std@@3_JB`) are not a flat C surface. That takes the msvcp100 /
> msvcp120 answer — a real x86-64 Wine build staged into the prefix's
> `sysx8664` — now generalised in **`ppc64le/steamtool/stage-guest-runtime`**,
> which sources the file from any **Proton dist tree** instead of from an app
> prefix most titles (Civ VI included) do not have. `concrt140` is staged with
> it, because msvcp140's own `DllMain` `LoadLibrary`s it.
>
> **vcruntime140 is deliberately NOT staged.** This tree's own thunk forwards
> the entire C++ exception personality into `dlls/guestcrt`'s guest x86-64
> code; replacing the module to gain six RTTI entry points would swap out a
> load-bearing subsystem. The six were served properly instead.
>
> **Measured, headlessly** — `ppc64le/games/probe-dllload.sh`, a new guest
> probe that does one `LoadLibrary` and needs no display, no GPU and no game
> lock, which is what made this loop affordable with four agents queued for the
> foreground:
>
> * `EOSSDK-Win64-Shipping.dll` — **loads, `DllMain` completes,
>   `EOS_Initialize` resolves.** The module the title died in.
> * `DatabaseDLL_FinalRelease.dll`, `Localization_FinalRelease.dll`,
>   `HavokScript_FinalRelease.dll` — load, **zero** unresolved imports.
> * `GameCore_Base_FinalRelease.dll` — faults `c0000005` in its own `DllMain`
>   with a guest↔native recursion the bridge's `SPINSENTINEL` names. **Not
>   established as the next wall**: `civilizationvi.exe` does not statically
>   import it, so loading it alone is out of the game's own order.
>
> **Still open, both non-fatal here and both recorded where they live:**
> `ucrtbase._invoke_watson` (`@ stub` — Wine has no implementation; a terminal
> path anyway) and `ucrtbase._open` (a true variadic with no v-variant; the
> `variadic=` guard correctly refuses the `.spec` fallback, and closing it means
> adding a flat v-form). `ucrtbase._set_new_handler` stays absent **on purpose**
> — its argument is a guest function pointer handed to native code that will
> later call it, so a row without a matching callback-interception entry would
> be worse than the hole.
>
> **THE TITLE ITSELF, MEASURED** (run `20260830-155652`, the full launcher path
> through `with-game-lock`): **`loader_init` completes.** No sentinel fault, no
> wild pointer, no `c0000135`/`c000007b` anywhere in the run.
> `civilizationvi.exe`, `bink2w64.dll` and `EOSSDK-Win64-Shipping.dll` all map
> and initialise; the guest JIT starts (the `fexbridge` banner lines); the main
> thread gets through COM, `NtQuerySystemInformation` and WS2_32 init, and a
> second thread (`0138`) spawns and probes display drivers.  Exactly **two**
> imports in the whole run bind sentinels -- `_invoke_watson` and `_open`, each
> bound twice (once for the exe, once for EOSSDK) -- and **neither is called**.
>
> `rc=143` is SIGTERM from this run's own `timeout -k 10 300`, **not a crash**:
> the game was still alive when the budget expired.  Compare the previous state,
> `rc=5` inside two minutes with the process dead in the loader.
>
> **THE NEW WALL, NAMED** (runs `160951` and `161227`, the second with
> `+loaddll`).  **All seven of the title's own modules load, every one of them
> as a real x86-64 guest PE:**
>
> ```
> civilizationvi.exe   steam_api64.dll   EOSSDK-Win64-Shipping.dll
> DatabaseDLL_FinalRelease.dll   Localization_FinalRelease.dll
> HavokScript_FinalRelease.dll   bink2w64.dll
> ```
>
> The main thread completes COM (`combase`/`coml2`/`ole32`/`oleaut32`/
> `actxprxy`), a second thread loads **`winex11.drv` and `uxtheme.dll`** -- so
> display init is fine; the `winemac.drv` `c0000135` in the log is Wine probing
> the macOS driver on Linux and is benign -- and **the game then exits `rc=51`
> of its own accord, about 60 seconds in**, with no fault, no sentinel and no
> unhandled exception anywhere in the run.
>
> It never creates `Documents\My Games\Sid Meier's Civilization VI\Logs`, so
> it dies before its own logging starts and has nothing to say for itself.
>
> **The reading, stated as a hypothesis and not as a result:** `steam_api64.dll`
> is loaded and the only thing between "every module initialised" and "clean
> early exit with a nonzero code" is Steamworks.  Civ VI exits rather than
> continues when `SteamAPI_Init` fails, and this port has no real Steam client
> behind the bridge.  That is the **Steam liveness** work another agent owns
> (`ppc64le/steamapi`), which is why this pass stopped here rather than
> guessing at it.  What would settle it in one run: `+loaddll` plus a channel
> that shows the Steamworks entry points being called, or a breakpoint on
> `steam_api64.SteamAPI_Init`'s return.
>
> **STATED AS A NEGATIVE, because it removes the most obvious suspect:** the
> SteamStub liveness objects are **neither cause nor cure** here.  Runs
> `160951` and `161227` had the presence publisher ACTIVE; run `161516` ran
> with it OFF (`steam presence publisher off` in the log, after that mechanism
> became opt-in).  **Byte for byte the same outcome** -- `rc=51`, the same
> seven modules, the same stop point after `winex11.drv`/`uxtheme.dll`.  So
> whatever Civ VI is refusing, publishing or withholding
> `Local\SteamStart_SharedMem*` does not move it, and the Portal 2 regression
> that made those objects opt-in does not reproduce on this title either.
>
> Two earlier observations that a reader should NOT carry forward: the 300s run
> ended `rc=143`, which was that run's own `timeout` and not the game; and the
> log going quiet for four minutes looked like a hang but was not -- with a
> shorter budget the same build exits on its own in a minute.

> **The class, carried forward:** this is handoff #7's lesson one level up.
> #7 was a module reached only through `LoadLibrary` and therefore invisible to
> `check-import-chain.sh`. This one was visible the whole time — the log named
> it — and cost the same because the *documented* WARN string only fires on a
> branch real misses never take.
