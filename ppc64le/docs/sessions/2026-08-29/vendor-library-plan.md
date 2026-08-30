# Vendor-library strategy: the GPU-vendor middleware AAA titles ship

Research pass, 2026-08-29. No source files modified, nothing built, nothing
launched. All PE facts below were read directly from the files on the AC922
with small Python readers (`/tmp/vendorplan/` on the AC922: `peimports.py`,
`pesyms.py`, `peexports.py`) plus `ppc64le/thunks/import_chain.py` (run from a
patched `/tmp` snapshot after the live copy grew a mid-edit call-site bug —
see "coordination notes" at the end; the tree copy was not touched).

Tree: `~/Development/powerpc64le-ports/hangover-ppc64le/wine-upstream` (src)
and `.../wine-build` (build) on the AC922; the Pi's
`~/Development/power9_development/` mount maps to the AC922's `~/Development/`.

Labels: **[M]** = MEASURED (read from a file / tool output today),
**[I]** = INFERRED (reasoned, needs a run or an owner to confirm).

---

## 0. The one mechanism that explains the current wall [M]

Today's Steam-copy crash record
(`.../Cyberpunk 2077/bin/x64/Cyberpunk2077.exe-20260829-133848-304-308.txt`):
`EXCEPTION_ACCESS_VIOLATION`, "DEP violation at **0xDEAD0009**", uptime 9 s —
the per-symbol sentinel class, exactly as briefed.

The chain, every link read from a file:

1. The prefix in use is `~/.local/share/wine-ppc64le/cp2077/pfx` (run-native;
   Steam's `compatdata/1091500` has **no** sysx8664 — today's runs go through
   run-native). Its `sysx8664` holds exactly: `msvcp140.dll`,
   `vcruntime140.dll`, `vcruntime140_1.dll` (the hand-staged Aftermath fix,
   all "Wine builtin DLL" x86-64) and the hand-built `sl.interposer.dll`
   stub (6656 bytes, 23 exports). [M]
2. The staged vcruntime140 **forward-exports**
   `_CxxThrowException -> ucrtbase._CxxThrowException` (and the whole
   `__CxxFrameHandler*` family, and setjmp/longjmp). [M — export table dump]
3. This tree's ucrtbase guest thunk **deliberately refuses**
   `_CxxThrowException` / `__CxxFrameHandler3` / `__unDNameEx`
   (`dlls/ucrtbase/ucrtbase.thunks` ~line 231 says so and points at the
   reasoning). So the forward lands in a hole → sentinel → the crash. [M]
4. Piquant asymmetry: the tree's **own** `dlls/vcruntime140/vcruntime140.thunks`
   *does* carry a `_CxxThrowException` row (0x00000B01, from the DOOM triage),
   while `__CxxFrameHandler3` is a documented deliberate hole. So the staged
   Proton vcruntime140 and the native thunk do **not** resolve
   `_CxxThrowException` identically — the very parity the setjmp FORWARD
   comment in that file says the design wants. Either way a real guest throw
   cannot complete without a guest-side EH personality (the deliberate-hole
   comment explains why), so this is a note **for the EH agent**, not a bug to
   fix here. [M for the tables, I for "neither mix can complete a throw"]

Consequence for this plan: **satisfying** the XeSS family is EH-gated;
**stubbing** it is not. The stubs are the parallel lane.

---

## 1. Inventory

### Cyberpunk 2077, Steam copy (`.../Cyberpunk 2077/bin/x64/`, patch 2.31) [M]

Exe static import table read directly; delay-load table is **empty**. Static =
cannot be removed, the loader binds it before entry. `import_chain.py` walked
the full static chain: 4701 imports across 25 modules, 495 holes — nearly all
`msvcp140`/`vcruntime140[_1]` MSVC C++ symbols.

| Library | What it is | Linkage | Unresolved imports (against tree thunks, before prefix staging) | Verdict |
|---|---|---|---|---|
| `sl.interposer.dll` | NVIDIA Streamline (DLSS/Reflex plugin loader; game routes D3D12/DXGI creation through it) | **static** | 83 (78 msvcp140, 4 vcruntime140, 1 vcruntime140_1) — moot: stubbed | **STUB** (done; promote). Stub is missing `slEvaluateFeature`, which the exe imports — see §2 |
| `libxess.dll` (77 MB, XeSS 2.x) | Intel XeSS super-resolution | **static** | 129: 122 msvcp140, 5 vcruntime140 (`__CxxFrameHandler4` via _1), `SETUPAPI.SetupGetInfDriverStoreLocationW`; +10 apiset NO-THUNKs (see §6 — static-analysis blind spot) | **STUB** |
| `libxess_fg.dll` (45 MB) | Intel XeSS Frame Generation | **static** | 120: 114 msvcp140, 4+1 vcruntime, `SETUPAPI.SetupGetInfDriverStoreLocationW`; +10 apiset NO-THUNKs | **STUB** |
| `libxell.dll` | Intel XeLL latency reduction | **static** | 91: 86 msvcp140, 3+1 vcruntime, `SETUPAPI.SetupGetInfDriverStoreLocationW`; +5 apiset NO-THUNKs | **STUB** |
| `libxess_dx11.dll` | XeSS DX11 variant | dynamic (not in exe table) | not walked; same family | STUB (same module set; only if it ever loads) |
| `amd_ags_x64.dll` | AMD GPU Services (driver extension markers) | **static** | **0 holes** — imports only KERNEL32+USER32 | **LEAVE** (satisfied). `agsInit` fails gracefully without AMD's ADL DLLs [I — no wall recorded] |
| `ffx_fsr3_x64.dll`, `ffx_backend_dx12_x64.dll` (+static deps `ffx_frameinterpolation/fsr3upscaler/opticalflow_x64.dll`) | AMD FidelityFX FSR3 upscaling + frame interpolation | **static** | **0 holes** | **LEAVE — never stub.** This is the one vendor stack that is *relevant* on this GPU: it is the game's AMD upscaler/frame-gen path and the honest replacement for DLSS/XeSS |
| `amd_fidelityfx_dx12.dll` | FidelityFX runtime (other features) | dynamic | not walked | LEAVE |
| `GFSDK_Aftermath_Lib.x64.dll` | NVIDIA Aftermath GPU crash dumps | **dynamic** (NOT in exe static table — but measured earlier to load at startup and block it) | was 91, all MSVC CRT — **already resolved** by the msvcp140/vcruntime140 staging | **SATISFY** (done by staging; promote staging into tool). API fails cleanly on non-NVIDIA [I] |
| `GfnRuntimeSdk.dll` | NVIDIA GeForce NOW detection | dynamic | 0 vendor holes (plain system imports) | LEAVE (its C++ throw already earned the unwinder fixes, GOG session) |
| `nvngx_dlss.dll`, `nvngx_dlssd.dll`, `nvngx_dlssg.dll` | DLSS model payloads | dynamic, loaded **only by Streamline plugins** | n/a | NO ACTION — with the interposer stubbed they can never load |
| `sl.common/sl.dlss/sl.dlss_d/sl.dlss_g/sl.nis/sl.pcl/sl.reflex.dll` | Streamline feature plugins | dynamic, loaded only by the real interposer | n/a | NO ACTION — same |
| `nvToolsExt64_1.dll` | NVIDIA profiling markers | dynamic | imports KERNEL32 only | LEAVE |
| `WinPixEventRuntime.dll` | MS PIX markers | dynamic | CRT only, covered by staging | LEAVE |
| `CChromaEditorLibrary64.dll` | Razer Chroma | dynamic | needs `mfc140u.dll` (the DOOM precedent — the tool's existing MFC block covers it when a foreign prefix has it) | LEAVE |
| `bink2w64.dll`, `oo2ext_7_win64.dll` | Bink video, Oodle compression | static / dynamic-by-name | 0 holes | LEAVE (essential middleware, works) |
| `dbghelp.dll` + `dbgcore.dll`, `symsrv.dll` | game-shipped MS debug help | **static** (dbghelp) | 7 ucrtbase `_o__*`/`__unDNameEx`/EH holes; apiset NO-THUNKs | LEAVE; the crash-txt files prove the reporter pipeline already works [M]. `_o__*` rows are cheap thunk work **only if measured to be called** |
| `icuuc.dll`, `icuin.dll` | game-built ICU (appconfig already forces `=n`) | **static** | 23+7, all MSVC C++/EH class — covered by staging except EH-on-throw | SATISFY (staging, done) |
| `libcurl.dll` | curl | **static** | `wldap32.#301`, 1 ucrtbase | LEAVE until named in a log |
| `redlexer_native.dll` | REDengine script lexer | **static** | 1: `VCRUNTIME140.__CxxFrameHandler3` | SATISFY via staging (binds through the staged forward); scripts load today per crash txt [M]. Real throw = EH-gated |
| `REDGalaxy64.dll` | GOG Galaxy glue | **static** | 0 holes (secur32 work already landed) | LEAVE |
| Anti-tamper / Denuvo | — | — | none present: CP2077 ships no Denuvo; DRM surface is `steam_api64.dll` + `GameServicesSteam.dll` (dynamic), both already served by the port's Steam helper | NO ACTION |

### The Witcher 3, Steam copy (`.../The Witcher 3/bin/x64_dx12/`, next-gen DX12 build) [M]

Chain walk: 2859 imports across 12 modules, 508 holes.

| Library | Linkage | Unresolved imports | Verdict |
|---|---|---|---|
| `sl.interposer.dll` (older SL 1.x API) | **static** | 61 msvcp140 + 4 vcruntime — moot with stub. **Exe imports 5 names the current stub lacks**: `slGetFeatureSettings`, `slSetFeatureConstants`, `CreateDXGIFactory1`, `DXGIGetDebugInterface1` (+ CP's `slEvaluateFeature`) | **STUB** — after widening the export surface. The two DXGI creators are likely called unconditionally [I] |
| `libxess.dll` (15 MB, XeSS 1.x; statically imports `XeFX.dll` + `XeFX_Loader.dll`) | **static** | 146 msvcp140, 4+1 vcruntime, setupapi, 11 apiset NO-THUNKs | **STUB** (7-name surface, smaller than CP's) |
| `XeFX.dll`, `XeFX_Loader.dll` | static deps **of libxess only** | 51+18 msvcp140 etc. | NO ACTION — never load once libxess is stubbed [M: only libxess imports them] |
| `GFSDK_Aftermath_Lib.x64.dll` | **static** (unlike CP) | 106 msvcp140 + 5+1 vcruntime | **SATISFY** via the same CRT staging (promoting it makes this title's fix automatic) |
| `GFSDK_SSAO_D3D12.win64.dll` (HBAO+) | **static** (`GFSDK_SSAO_CreateContext_D3D12`) | **0 holes** | **LEAVE — do not stub.** Plain D3D12 compute, works on AMD; stubbing would break real AO [I on "works", M on clean binding] |
| `NvCameraSDK64.dll` (Ansel) | **static** (`updateCamera`, `setConfiguration`) | 4 msvcr120 EH/context symbols + NO-THUNK msvcp120 | **SATISFY**: stage Proton msvcp120 (the GOG-W3 precedent, currently hand-work) ; the 4 EH holes fire only on throw [I] |
| `dxcompiler.dll` + `dxil.dll` | **static** | 73 msvcp140, 5 ucrtbase `_o__*`, 4+1 vcruntime | **SATISFY** (essential — it compiles the game's shaders; staging covers the msvcp surface) |
| `GFSDK_HairWorks`, `NVHair*`, `PhysX3Gpu`, `PhysXDevice64`, `APEX_ClothingGPU`, `cudart64_50_35` | dynamic (none in exe table) | not walked | LEAVE — NVIDIA/CUDA GPU-physics features; the engine's CPU fallback is the Windows-on-AMD behavior [I; act only on a measured wall] |
| `RedTelemetryLib.dll`, `Galaxy64.dll`, `GRB_1_1_api3_x64.dll` | dynamic | not walked | LEAVE |

### The GOG-vs-Steam question [I]

The GOG copy is **not on this disk** (searched; today's `cp2077` prefix logs
show the Steam path). The co-devs' GOG notes mention only `sl.interposer.dll`.
Given the Steam exe here is patch **2.31** (built 2025-08-27) and XeSS-FG/XeLL
arrived in the 2.2x line, the likeliest story is *version* skew shipping as
*store* skew — but that is unverifiable from here. Either way the Steam copy's
static vendor set is the superset, so serving it serves both.

---

## 2. Stub versus satisfy, per library, with the reasoning

The honest-failure test from the brief: a stub is right only when the feature
is genuinely unavailable on this hardware/port **and** the game has a fallback
it actually takes. A stub that hides a feature the game then calls into is
worse than a named sentinel.

**STUB (fail honestly):**

- **`sl.interposer.dll`** — settled (README + STATUS record the measured wild
  pointer from detour-hooked trap stubs; every Streamline feature is
  NVIDIA-only). The game's own SL error handling turns failure into "feature
  unavailable", and the D3D/DXGI creators pass through. Remaining work is the
  **export surface**: the staged stub exports 23 names [M]; union with both
  exes' import lists adds `slEvaluateFeature` (CP — sentinel today, unreached
  only because `slInit` fails first [I]) and W3's `slGetFeatureSettings`,
  `slSetFeatureConstants`, plus FWD lines for `CreateDXGIFactory1` and
  `DXGIGetDebugInterface1` (real dxgi entry points a game may call
  unconditionally — these must forward, not fail).
- **`libxess.dll` / `libxess_fg.dll` / `libxell.dll`** — stub, three reasons:
  1. *The satisfy path is EH-gated.* The msvcp140 surface staging covers the
     bindings, but libxess **throws during early init** (measured: today's
     0xDEAD0009 at 9 s uptime), and a guest C++ throw cannot complete until
     the guest-side personality lands (deliberate holes in
     ucrtbase/vcruntime140 thunks, documented in-file). Another agent owns
     that; this class should not wait on it.
  2. *The game has the fallback on this exact hardware.* CP2077 statically
     imports **AMD's FSR3** (binds with zero holes) — upscaling and frame
     generation on an RX 7900 XTX is FSR3's job. XeSS-FG and XeLL genuinely
     require the Intel driver stack (XeFX); plain XeSS has a DP4a fallback
     that runs on AMD *on Windows*, so the stub slightly under-promises there
     — but "unsupported driver" is the truthful answer for RADV/vkd3d today,
     and CP2077/W3 both grey the option out exactly as on a non-dp4a Windows
     box [I — the one behavioral assumption a launch must confirm].
  3. *Cost.* 77 MB + 45 MB of MSVC C++ running under emulation, for a feature
     the settings menu duplicates natively.
  Stub shape: every `xess*`/`xefg*`/`xell*` export returns nonzero
  (`XESS_RESULT_ERROR_UNSUPPORTED_DRIVER` = -3 class; any stable nonzero
  works, like the SL stub's `1`), **and out-params are zeroed** for the
  getters the exes import (`xessGetVersion`, `xessGetInputResolution`,
  `xefgSwapChainD3D12GetSwapChainPtr`, `xellD3D12CreateContext`) so a caller
  that reads before checking gets NULL/zeros, not stack garbage — one notch
  more careful than the SL stub's zero-arity trick, and cheap since each
  export list is 6–11 names [M].

**SATISFY (the library is harmless; feed it its dependencies):**

- **`GFSDK_Aftermath_Lib.x64.dll`** — pure CRT hunger, already proven by the
  hand staging. On AMD its API returns not-available and the game moves on
  (it has for both titles' GOG/hand-staged runs). Promote the staging.
- **`dxcompiler.dll`** (W3) — not vendor-optional, it is the shader compiler;
  must bind for real. CRT staging covers it.
- **`NvCameraSDK64.dll`** (W3) — 2 flat imports; the GOG-W3 fix (msvcr120
  flat rows + staged msvcp120) already carries it. Promote msvcp120 staging.
- **`icuuc/icuin`, `redlexer_native`, `dbghelp`** — engine essentials, bind
  through the staged CRT; their EH-class holes only fire on an actual throw.

**LEAVE ALONE (functional on this hardware — the inventory's job is to make
sure nobody ever stubs these):** `ffx_*` (FSR3 — the actively wanted path),
`amd_ags_x64`, `GFSDK_SSAO_D3D12` (HBAO+ is GPU-agnostic compute),
`bink2w64`, `oo2ext_7_win64`, `REDGalaxy64`, `GfnRuntimeSdk`,
`nvToolsExt64_1`, `WinPixEventRuntime`, and every dynamically-loaded
NVIDIA-physics module until a log names one.

---

## 3. Staging design for `ppc64le/steamtool/proton`

Two new blocks, each following the letter of the existing comments' hard-won
rules (`msvcp100` block ~line 460, `mfc140u` block ~line 360):

**Block A — promote the CRT staging (the "satisfy" half).**
Extend the existing `for dll in msvcp100.dll` loop's list to:
`msvcp100.dll msvcp120.dll msvcp140.dll vcruntime140.dll vcruntime140_1.dll`.
- Source: `$foreign` (the app's real Proton prefix), exactly as now — nothing
  redistributed, the user's own files.
- Dest: **`sysx8664`, not system32** — every one of these has a competing
  Wine builtin, so the load path never reaches a system32 copy (the msvcp100
  comment records this).
- **`pe_machine` = 8664 gate** on every copy — the 2026-08-22 self-copy bug
  is why the helper exists; keep using it.
- **NO load-order override** — `LO_NATIVE` refuses a Wine builtin outright
  (`load_builtin`); the staged file wins by directory search order, not by
  override. The msvcp100 comment records the rc=53 scar.
- Idempotent, checked every run, `say` on both success and absence, with the
  "provision a prefix for this app" hint mirroring the existing text.
- One consequence to note in the block comment: staging vcruntime140 changes
  `_CxxThrowException` from the native thunk's served row to a forward into
  ucrtbase's deliberate hole (§0.4) — cite it so the EH agent's landing can
  revisit.

**Block B — vendor fail-honestly stubs (the "stub" half), new.**
```
for dll in sl.interposer.dll libxess.dll libxess_fg.dll libxell.dll; do ...
```
- Source: **the build tree, not a foreign prefix** —
  `$tree/dlls/<name>/x86_64-windows/<name>.dll` (the guestpe output-directory
  contract steamclient64/guestcrt already document; `$tree` is the build dir,
  the top-of-file comment's out-of-tree scar applies). Fall back to
  `$(libdir)/wine/x86_64-windows` if the tool ever runs from an install.
- Dest: `sysx8664` — **mandatory here, not just preferred**: the game ships
  its own real copies beside the exe, and only find_dll_file()'s
  guest-branch "machine's own system dir FIRST" rule lets the stub shadow
  them (the streamline README documents this as the mechanism).
- `pe_machine` = 8664 gate, same helper.
- NO load-order override (none needed — no builtin competes, and the rule
  stands on principle).
- **Refresh, don't skip**: unlike the borrowed-CRT block, these are our own
  build products; `cmp -s || cp` so a stub fix reaches existing prefixes.
  (The msvcp100 block skips-if-present because overwriting a user's
  provisioned runtime would be rude; that reasoning doesn't apply to our own
  artifacts.)
- Stage unconditionally into every prefix: the four names only ever shadow a
  game's own copies of libraries that cannot work on this port, so there is
  nothing per-title about it — no manifest needed.

---

## 4. Should the stubs become guestpe modules? **Yes.**

- The infrastructure is exactly for this: `tools/guestpe/guestpe` +
  `output_source_guestpe` in `tools/makedep.c` build real x86-64 PEs as
  ordinary tracked build output, with `.def`-driven exports and `IMPORT`
  lines against guest thunk DLLs. `dlls/guestcrt/` (2 exports, 1 source, a
  5-line `.guestpe`) proves the fixed cost is tiny [M].
- The hand-recipe status quo already failed its own bar twice [M]: a fresh
  tree has **no** stub at all, and the one staged copy is missing five
  exports the second title statically imports (`slGetFeatureSettings`,
  `slSetFeatureConstants`, `CreateDXGIFactory1`, `DXGIGetDebugInterface1`,
  `slEvaluateFeature`). A `.def` is an auditable export surface; the current
  recipe relies on MinGW-lld auto-export of whatever is global.
- Staging (Block B) then has a real make-tracked artifact to copy, with the
  `pe_machine` gate as belt-and-braces.

Concrete shape: four module directories —
`dlls/sl.interposer/`, `dlls/libxess/`, `dlls/libxess_fg/`, `dlls/libxell/`
(dotted directory names are established Wine practice, cf. `dlls/api-ms-*`),
each with `Makefile.in` (`SOURCES = <name>.guestpe`), the `.guestpe`
(`ENTRY DllMain`, `DEF <name>.def`, `IMPORT kernel32` for the interposer's
lazy-forwarding; the xess/xell stubs likely need no imports at all, like
guestcrt), one small `.c`, and a `WINE_CONFIG_MAKEFILE(dlls/<name>)` line in
`configure.ac` (guestcrt precedent at line 2937). `sl_interposer_stub.c`
moves to `dlls/sl.interposer/`; `ppc64le/streamline/README.md` shrinks to a
pointer plus the design history, its build-recipe section retired.

---

## 5. The general shape — three patterns, not one framework

Deliberately **not** proposing a common stub skeleton library, a stub
generator, or a per-title manifest. The measured reality is three small
classes with three different answers:

- **Pattern A — fail-honestly stub** (Streamline, XeSS family, XeLL): tiny
  C-ABI export set returning nonzero, out-params zeroed, creators forwarded
  where the library is an *interposer*. Four ~80-line guestpe modules. The
  only shared conventions worth writing down: exports come from the measured
  union of shipping titles' import tables; staged in `sysx8664`; never stub
  a library whose feature works on this hardware. A shared macro header is
  optional sugar — with four files this small, duplication is cheaper than
  abstraction.
- **Pattern B — satisfy the CRT** (Aftermath, dxcompiler, NvCameraSDK, icu,
  dbghelp): one staging list in one existing block. No stubs, no new code.
- **Pattern C — leave alone** (FSR3, AGS, HBAO+, Bink, Oodle, and all the
  dynamic NVIDIA physics): the inventory table *is* the artifact — it names
  what must never be stubbed.

The reusable *method* is the audit itself: exe static-import table +
`import_chain.py` walk before any run — STATUS.md already states this as the
house rule; this pass is that rule applied to the vendor layer.

---

## 6. Ordered plan, sizing, and what each step unblocks

| # | Step | Size | Unblocks | EH-dependent? |
|---|---|---|---|---|
| 1 | Four guestpe stub modules (`dlls/sl.interposer` with the widened 28-name surface, `dlls/libxess` 9+dx11 names, `dlls/libxess_fg` 11, `dlls/libxell` 6; out-params zeroed) + `configure.ac` lines | ~1 day | CP-Steam past today's 0xDEAD0009 wall and past libxell (next in the chain); W3-Steam-DX12's entire Intel/NVIDIA-optional layer; fresh-tree reproducibility of the existing GOG fix | **No** — this is the parallel lane |
| 2 | `steamtool/proton` Block B (stage stubs from `$tree`, pe_machine-gated, cmp-refresh, sysx8664, no overrides) | ~½ day | any new prefix / any user gets the layer automatically; retires the README's "not yet a proton-tool staging rule" debt | No |
| 3 | `steamtool/proton` Block A (add msvcp120/msvcp140/vcruntime140/vcruntime140_1 to the msvcp100 list) | ~½ day | Aftermath + dxcompiler + NvCameraSDK + icu on *any* fresh prefix — currently hand-staged in exactly one prefix on one machine | No (binding); throws through it remain EH-gated |
| 4 | Docs: streamline README pointer, STATUS.md Steam-copy row for both titles | ~1 h | co-devs on GOG can see why their copy never showed this class | No |
| 5 | Verification, by the owner (not this pass): re-run `import_chain.py` on both exes with stubs beside the walk, then a launch gate on CP-Steam — expect title screen with XeSS/frame-gen greyed and FSR3 offered | ~½ day | confirms the two [I] behavioral assumptions in §2 | No |
| 6 | Optional flat thunk rows, each only on a measured call: `setupapi.SetupGetInfDriverStoreLocationW` (only if the XeSS *satisfy* path is ever attempted), ucrtbase `_o__seh_filter_dll`/`_o__calloc_base`/`_o___stdio_common_vfprintf` (dbghelp/dxcompiler), `wldap32.#301` (libcurl) | hours each | removes the last non-CRT named holes in both walks | No |
| 7 | Revisit libxess as a *real* guest module (DP4a path) | — | XeSS image-quality option | **Yes — after the EH agent lands**, and only if someone wants it; FSR3 makes it optional forever |

Steps 1–3 are independent of each other in code but land best in that order
(2 stages what 1 builds). Everything above the last row proceeds without
waiting on C++ exception handling — that is the point of doing this now.

---

## 7. What I could not determine, and what settles it

- **[I] The two behavioral gambles of the stubs**: (a) CP2077 2.31 tolerates
  every `xess*`/`xefg*`/`xell*` call failing (expected — it is the shipping
  behavior on non-dp4a Windows hardware); (b) no caller reads an out-param
  before checking the result (mitigated by zeroing). One gated launch with
  the stubs staged settles both. I did not launch anything.
- **[I] W3's `CreateDXGIFactory1`/`DXGIGetDebugInterface1` are called
  unconditionally** — near-certain, which is why they are FWD lines, not
  fail-stubs. Same launch settles it.
- **GOG-copy vendor set** — not on this disk; version-skew vs store-skew is
  unresolved. Settled by a co-dev running the two `/tmp/vendorplan` readers
  (or `winedump`-equivalents) on their `bin/x64`.
- **Whether CP loads Aftermath before or after the vendor-static layer** —
  it is absent from the exe's static table [M] yet blocked startup earlier
  [project record]; some engine module LoadLibrary's it unconditionally.
  Doesn't change the verdict (satisfy, already done).
- **`amd_ags_x64` runtime behavior** (`agsInit` on RADV with no ADL) — no
  wall recorded across many runs; left alone, watch logs.
- **The apiset NO-THUNKs are (very likely) a static-analysis blind spot, not
  runtime walls**: libxess reached its own throw today, which means its whole
  import table — including `api-ms-win-core-debug-l1-1-0.dll` — bound at
  load [M ⇒ I]. So the walker's apiset map misses entries the real loader
  resolves. That belongs to the `import_chain.py` owner.

### Coordination notes (read-only observations, other agents' territory)

- `ppc64le/thunks/import_chain.py` changed *between my two runs today* and
  currently has a mid-edit inconsistency: the `serve =
  guest_thunk_path(src, build, target)` call site lacks the new `mdir`
  argument, and `exports_cache` lost its initialization (renamed toward
  `served_cache`). My `/tmp/vendorplan/import_chain_snap.py` patch (add
  `"x86_64-windows"`, re-add the dict) confirms the walk itself is healthy.
  Left for its owner.
- The `_CxxThrowException` parity asymmetry (§0.4) and the deliberate EH
  holes are the EH agent's file comments; nothing here touches them, and
  nothing in steps 1–5 depends on them.
- `dlls/guestcrt/` and the guestpe tool were read for pattern only.

### Raw data locations (AC922, `/tmp/vendorplan/`)

`cp_exe_holes.txt` (full CP walk, sorted/uniq-counted), `w3_exe_holes.txt`
(full W3 walk), `cp_vendor_syms.txt` / `w3_vendor_syms.txt` (per-vendor-DLL
symbols each exe imports — the stub `.def` contents), `peimports.py`,
`pesyms.py`, `peexports.py`, `import_chain_snap.py`.
