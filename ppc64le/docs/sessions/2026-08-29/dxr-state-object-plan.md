# DXR across the guest/native boundary — design study, 2026-08-29

Tree read: `wine-upstream` on the AC922 (sshfs), HEAD `2431d35e7a1`.
Every claim below is labeled MEASURED (read from the tree / git) or INFERRED.

## 0. The premise is stale — the walker already exists

**MEASURED.** The task premise ("CreateStateObject / AddToStateObject are named
refusals; nobody has written a hand walker") describes the tree as of
2026-08-19. Commit **`477b103fb76`** (2026-08-26, "vkd3d,d3d12: nine refused
slots become hand walkers; the named holes drop to four") *implemented* the
`D3D12_STATE_OBJECT_DESC` walker; it is not merely precedent. In HEAD:

* `dlls/d3d12/d3d12_marshal.h`: both rows carry `WINECOM_F_HAND` — hand
  index 14 (`ID3D12Device5::CreateStateObject`) and 15
  (`ID3D12Device7::AddToStateObject`), registered in every derived device
  interface's table (Device5..15 for Create, Device7..15 for Add).
* `dlls/d3d12/main.c`: `state_object_desc_unwrap()` (~line 812),
  `hand_create_state_object()` (917), `hand_add_to_state_object()` (941),
  both in `d3d12_hand_funcs[]`, order pinned by `gen_winecom.py`'s
  `HAND_SLOTS` list and `C_ASSERT`ed against `D3D12_HAND_COUNT`.
* Banner: **2661 marshalled / 133 hand-written / 4 refused** — and none of the
  four refusals (DRED `GetAutoBreadcrumbsOutput`, the two
  `ID3D12WorkGraphProperties` by-value `D3D12_NODE_ID` getters,
  `ID3DDestructionNotifier::RegisterDestructionCallback`) is on the DXR path.
* NEXT.md item 3 records it: "nodxr CAN come out of the appconfig now, but
  whether the V620 should be offered RT is a performance decision, not a
  marshalling one." Also: "None of the nine new walkers has been driven by a
  real title yet; the next Cyberpunk boot is the live test."

Two things ARE stale and worth fixing as documentation hygiene (edit, don't
guess — the owner may want to do this): the `nodxr` comment block in
`ppc64le/steamtool/appconfig/1091500.env` still says "no hand-written walker
yet", and this study's own tasking inherited that comment.

So the study's real question is not "how do we write the walker" but **"what
does the walker not cover, is the rest of the DXR path served, and what does
it take to prove the whole thing live"**. That is what the rest answers.

## 1. The subobject enumeration (core deliverable)

Source of truth: `ppc64le/vkd3d/generated-headers/vkd3d_d3d12.h` (the pinned
vkd3d-proton 3.1.0 headers this surface is generated from), enum
`D3D12_STATE_SUBOBJECT_TYPE` at line 6685, values 0–30 (4 and 25 are
reserved/skipped upstream). Walker column is MEASURED from
`state_object_desc_unwrap()` in `dlls/d3d12/main.c`.

| # | Subobject type | `pDesc` points to | Interface ptrs | Other pointers | Counted arrays | Walker today |
|---|----------------|-------------------|----------------|----------------|----------------|--------------|
| 0 | STATE_OBJECT_CONFIG | `D3D12_STATE_OBJECT_CONFIG { Flags }` | none | none | none | pass-through |
| 1 | GLOBAL_ROOT_SIGNATURE | `D3D12_GLOBAL_ROOT_SIGNATURE { ID3D12RootSignature* }` | **yes** | none | none | **copied + `com_unwrap`** |
| 2 | LOCAL_ROOT_SIGNATURE | `D3D12_LOCAL_ROOT_SIGNATURE { ID3D12RootSignature* }` | **yes** | none | none | **copied + `com_unwrap`** |
| 3 | NODE_MASK | `D3D12_NODE_MASK { UINT }` | none | none | none | pass-through |
| 5 | DXIL_LIBRARY | `D3D12_DXIL_LIBRARY_DESC { SHADER_BYTECODE; NumExports; D3D12_EXPORT_DESC* }` | none | bytecode blob ptr; `D3D12_EXPORT_DESC { LPCWSTR Name; LPCWSTR ExportToRename; Flags }` | `pExports[NumExports]` | pass-through (guest ptrs; vkd3d deep-copies at create) |
| 6 | EXISTING_COLLECTION | `D3D12_EXISTING_COLLECTION_DESC { ID3D12StateObject*; NumExports; D3D12_EXPORT_DESC* }` | **yes** | export descs (strings) | `pExports[NumExports]` | **copied + `com_unwrap`** (shallow: `pExports` stays guest — interface-free) |
| 7 | SUBOBJECT_TO_EXPORTS_ASSOCIATION | `D3D12_SUBOBJECT_TO_EXPORTS_ASSOCIATION { const D3D12_STATE_SUBOBJECT*; NumExports; LPCWSTR* }` | indirectly — points at a **sibling subobject** | `LPCWSTR` array | `pExports[NumExports]` | **copied + pointer REMAPPED into the walker's copy** (index-validated; out-of-array refuses `E_INVALIDARG`) |
| 8 | DXIL_SUBOBJECT_TO_EXPORTS_ASSOCIATION | `{ LPCWSTR SubobjectToAssociate; NumExports; LPCWSTR* }` | none (association by *name*) | strings | `pExports[NumExports]` | pass-through |
| 9 | RAYTRACING_SHADER_CONFIG | `{ UINT MaxPayloadSize; UINT MaxAttributeSize }` | none | none | none | pass-through |
| 10 | RAYTRACING_PIPELINE_CONFIG | `{ UINT MaxTraceRecursionDepth }` | none | none | none | pass-through |
| 11 | HIT_GROUP | `D3D12_HIT_GROUP_DESC { LPCWSTR x4 ; Type }` | none | 4 strings | none | pass-through |
| 12 | RAYTRACING_PIPELINE_CONFIG1 | `{ UINT MaxTraceRecursionDepth; Flags }` | none | none | none | pass-through |
| 13 | WORK_GRAPH | `D3D12_WORK_GRAPH_DESC { LPCWSTR; Flags; NumEntrypoints; D3D12_NODE_ID*; NumNodes; D3D12_NODE* }` | none in the header structs | nested node descs | two counted arrays | **fails closed** (`E_INVALIDARG` + named ERR) |
| 14–24, 26–28, 30 | STREAM_OUTPUT, BLEND, SAMPLE_MASK, RASTERIZER, DEPTH_STENCIL, INPUT_LAYOUT, IB_STRIP_CUT_VALUE, PRIMITIVE_TOPOLOGY, RENDER_TARGET_FORMATS, DEPTH_STENCIL_FORMAT, SAMPLE_DESC, FLAGS, DEPTH_STENCIL1, VIEW_INSTANCING, DEPTH_STENCIL2 | the classic PSO sub-descs (graphics-state-in-state-object, SM 6.8 generic programs era) | none | input-layout element array etc. | some | **fail closed** |
| 29 | GENERIC_PROGRAM | `D3D12_GENERIC_PROGRAM_DESC { LPCWSTR; NumExports; LPCWSTR*; NumSubobjects; const D3D12_STATE_SUBOBJECT* const* }` | indirectly (nested subobject ptr array) | yes | two counted arrays | **fails closed** |

Walker mechanics (MEASURED, `state_object_desc_unwrap`): one sizing pass that
also refuses unknown types up front, one allocation holding the copied
subobject array plus payload copies, unwrap/remap in the copy, call, free.
`AddToStateObject` = same walker + `com_unwrap(grow_from)` for the
`ID3D12StateObject*` argument. Both return through
`winecom_wrap_out_iface(hr, riid, ppv)` (`libs/winecom/winecom.c:2171`). No
float-by-value anywhere in either frame, so no new `FP_SHAPE_*` typed call is
needed — `unixlib.h`'s three shapes (CLEAR_DSV, DEPTH_BOUNDS, DEPTH_BIAS)
stay as they are.

## 2. Minimum viable subset — already chosen correctly

A DXR 1.0/1.1 pipeline is built from exactly types 0–12: DXIL libraries, hit
groups, shader/pipeline configs, root signatures (global+local), exports
associations, optionally existing collections and node mask. **All twelve are
served.** What fails closed — WORK_GRAPH (13) and the graphics-in-state-object
/ GENERIC_PROGRAM family (14–30) — is SM 6.8 / Agility-SDK-era machinery no
DXR 1.x title touches. INFERRED (strongly): Cyberpunk's REDengine RT path is
DXR 1.0/1.1 and uses nothing past type 12; the walker's fail-closed ERR line
("state object subobject %u has type %u this walker cannot prove
interface-free") is the tripwire if that inference is wrong, and it prints to
the run log where `grep -a refus`-style triage already looks (this one is an
ERR, not a `refuse` row — grep for "cannot prove interface-free" too).

v1 refuse-by-name list is therefore: **13, 14–24, 26–30**. This matches the
codebase's normal shape and needs no change.

## 3. AddToStateObject

MEASURED: implemented (hand 15), same desc walker plus one unwrapped
interface in-arg, registered on Device7..Device15. Nothing deferred — it is
already a first-class citizen. INFERRED: CP2077 (DXR 1.1, incremental PSO
growth is an RTX-era optimization) may or may not call it; either way it is
covered. The only semantic risk is vkd3d-proton's own AddToStateObject
support level (upstream 3.1.0 supports it), not the marshalling.

## 4. The return path

* `ID3D12StateObject` (iface index 65), `ID3D12StateObjectProperties` (66),
  `ID3D12StateObjectProperties1` (67) are all in the roster
  (`interfaces_d3d12.json` → generated enum) with full slot tables. MEASURED.
* `CreateStateObject`'s `REFIID/void**` goes through `winecom_wrap_out_iface`
  — the same choke point every create in the surface uses. MEASURED.
* `GetShaderIdentifier` returns a raw `void*` into vkd3d's state-object data.
  Its row is a plain 2-arg marshal (no classes, RAX passed back raw).
  **This crosses safely**: the native lane is one process, one address space —
  the guest x86-64 code runs under FEX in the same process as native vkd3d,
  which is the same property every pass-through desc pointer in this surface
  already relies on. The guest memcpys `D3D12_SHADER_IDENTIFIER_SIZE_IN_BYTES`
  (32) bytes into its shader table; lifetime is tied to the state object,
  which the guest holds a proxy ref on. MEASURED (row) + INFERRED (lifetime
  semantics, per D3D12 spec and vkd3d behavior).
* `GetShaderStackSize`/`GetPipelineStackSize` return UINT64
  (`WINECOM_F_RET_QWORD` rows), `SetPipelineStackSize` served,
  `GetProgramIdentifier` (Properties1) is an sret row with
  `WINECOM_CA_RET_PTR`. MEASURED.
* One 32-bit-lane note: the two hand walkers have no `hand32` twins, so an
  i386 guest gets fail-closed behavior there — irrelevant for CP2077 (PE32+),
  worth a line in the table comment only.

## 5. The full DXR path today (MEASURED, from `d3d12_marshal.h`)

| Call | Status |
|------|--------|
| `ID3D12Device5::CreateStateObject` | **hand walker 14** |
| `ID3D12Device7::AddToStateObject` | **hand walker 15** |
| `ID3D12Device5::GetRaytracingAccelerationStructurePrebuildInfo` | marshalled (void-ret; desc + info ptrs pass through, no interfaces) |
| `ID3D12Device5::CheckDriverMatchingIdentifier` | marshalled |
| `ID3D12GraphicsCommandList4::BuildRaytracingAccelerationStructure` | marshalled (desc is GPU VAs + guest geometry-desc array; vkd3d converts at record time inside the call — same-address-space read, safe) |
| `ID3D12GraphicsCommandList4::EmitRaytracingAccelerationStructurePostbuildInfo` | marshalled |
| `ID3D12GraphicsCommandList4::CopyRaytracingAccelerationStructure` | marshalled |
| `ID3D12GraphicsCommandList4::SetPipelineState1` | marshalled with `WINECOM_CA_IFACE_IN` — the state-object proxy is unwrapped |
| `ID3D12GraphicsCommandList4::DispatchRays` | marshalled (desc is all GPU VAs + dims — no CPU pointers at all) |
| `ID3D12StateObjectProperties::*` | all served (section 4) |
| Shader binding tables | guest-written GPU buffers via the already-proven upload path (the elimination record in NEXT.md item 3: storms + cross-ISA shader byte-compare) — no new marshalling exists or is needed |
| CheckFeatureSupport OPTIONS5 (RaytracingTier) | marshalled long ago; the tier is what `nodxr` currently hides |

Native side: in-house vkd3d-proton **3.1.0** (`ppc64le/vkd3d/src`,
pinned + `vkd3d-patches/0001` which is event-handle work, RT-unrelated).
`nodxr` maps to `VKD3D_CONFIG_FLAG_NO_DXR`, which disables
KHR_ray_tracing_pipeline / acceleration_structure / ray_query /
deferred_host_operations / RT-maintenance1 / opacity micromap /
pipeline-library-group-handles at the extension table (`device.c:71-161`);
with the flag absent, DXR 1.1 is offered whenever RADV exposes the
extensions. MEASURED.

**Conclusion: there is no missing marshalling on the DXR path.** The whole
path is served; what it lacks is *live fire* — NEXT.md's own words: none of
the nine walkers has been driven by a real title yet.

## 6. Ordered implementation plan, with sizing

Nothing here is a new walker. The order is verification-first, and each step
names its machine. (POWER9 = this AC922, RX 7900 XTX/RDNA3, Vega card
available; POWER8 = co-developers' box, V620/RDNA2.)

1. **Refresh the stale comments** (bounded, minutes). `1091500.env`'s nodxr
   block still claims the walker doesn't exist; rewrite it to say the walker
   landed in `477b103fb76` and the flag now guards a *performance/validation*
   decision. Any machine.

2. **A/B probe run without `nodxr`** (bounded, one session; POWER9 + 7900
   XTX, coordinated with the build/run owner — this study must not launch).
   The appconfig is a **default, not an override** (`steamtool/proton:245` —
   a caller-set value is kept), so no file edit is needed: launch CP2077
   `-benchmark` with `VKD3D_CONFIG` pre-set to any benign non-empty value and
   the nodxr default yields. Watch for, in order: (a) the game surviving RT
   pipeline creation (the 2026-08-19 run 35 failure mode was force-close at
   title screen), (b) zero "cannot prove interface-free" ERRs and no new
   `refus` lines, (c) the benchmark completing, (d) fps and frame correctness
   vs the raster baseline (~15.6 avg, CPU-bound — INFERRED: RT will drop
   this further; the point of the first run is correctness, not speed).
   In-game RT settings should be forced on explicitly for one leg — the
   menu exposes RT toggles only when the tier is visible.

3. **A standalone state-object probe** (bounded, ~1 day; follows
   `ppc64le/vkd3d/probes/` precedent). A small C probe that creates a device,
   builds a minimal DXR pipeline — one DXIL library (raygen+miss), global
   root signature, shader config, pipeline config, hit group, one
   SUBOBJECT_TO_EXPORTS_ASSOCIATION (to exercise the sibling-pointer remap),
   one EXISTING_COLLECTION grow (to exercise `com_unwrap` of a state-object
   payload and AddToStateObject) — then QIs Properties, reads a shader
   identifier, builds a trivial BLAS/TLAS, DispatchRays 1x1, reads back.
   vkd3d's own `tests/d3d12_crosstest.h` shows the shape. Run it in the guest
   lane (cross-built x86-64 PE, like the copy-storm probe) so it drives the
   *walker*, not just vkd3d. Negative control for a `--sabotage` gate: a
   desc containing type 13 (WORK_GRAPH) must fail closed with the named ERR,
   and an association pointer outside the array must return E_INVALIDARG.
   POWER9 (needs RT hardware); also runnable on POWER8/V620.

4. **Vega control leg** (bounded, hours; POWER9 with the Vega card). Same
   probe + game boot with the tier honestly absent (RADV exposes no RT on
   Vega, so vkd3d reports TIER_NOT_SUPPORTED with no config flag at all).
   Proves the "absent tier" path is clean *without* nodxr — i.e. that nodxr
   is not load-bearing for non-RT hardware — and gives the correctness
   contrast: absent-and-honest vs present-and-served.

5. **The per-machine appconfig decision** (design decision, small). One
   shared `1091500.env` serves both machines today. If the 7900 XTX leg says
   RT is correct and worth offering while the V620 leg says it is not, the
   mechanism question is whether nodxr stays the shared default (callers on
   the XTX opt out per-launch, cheap, zero code) or steamtool grows a
   per-machine overlay. Owner's call; the caller-wins semantics make the
   zero-code answer viable today.

6. **Only if step 2/3 exposes a gap** (open-ended, but currently
   evidence-free): candidates in likelihood order are vkd3d-proton 3.1.0 DXR
   behavior on RADV/RDNA3 under this Mesa (native-side, now fixable in-tree
   — that is what bringing vkd3d in-house bought), dxil-spirv on the game's
   RT shaders cross-ISA (the 2176-shader byte-compare predates RT being
   reachable; RT shaders were likely never compiled — re-run the compare
   with RT pipelines built), and RT pipeline compile *time* amplifying the
   PipelineLibrary path (the Load trio walkers from the same commit are also
   untested live; watch their rows in the same run).

Explicitly NOT needed: new hand walkers, new typed FP shapes, roster changes,
generator changes, vkd3d patches.

## 7. Verification plan

| Step | What proves it | Machine |
|------|----------------|---------|
| Probe (plan step 3) as `check-state-object.sh` gate with sabotage legs | walker correctness incl. remap + fail-closed paths, repeatable without a game | POWER9 (RT); POWER8 later |
| CP2077 `-benchmark`, nodxr overridden, RT forced on | end-to-end frame out; the project's A/B harness; JSON results under the prefix's Documents | POWER9 + 7900 XTX |
| Same benchmark, RT off vs raster baseline | no regression when tier visible but unused | POWER9 |
| Vega leg | honest-absent-tier control | POWER9 + Vega |
| V620 leg, later | RDNA2 perf decision for whether nodxr leaves the shared appconfig | POWER8 + V620 |
| `grep -a "refus\|cannot prove interface-free"` on every run log | the tripwires for the exotic-subobject inference | all |

## 8. What I could not determine, and what settles it

* **Whether CP2077's RT actually stays within types 0–12** — INFERRED from
  the DXR 1.1 era; settled by the first no-nodxr boot's log (the fail-closed
  ERR names the type if not).
* **Whether it calls AddToStateObject / EXISTING_COLLECTION at all** —
  settled the same way (add a one-shot TRACE if desired).
* **RT correctness and performance of vkd3d-proton 3.1.0 on RADV/RDNA3 under
  this machine's Mesa** — unmeasurable without running, which the run owner
  controls; settled by plan steps 2–3. Same for RDNA2/V620.
* **Whether dxil-spirv's cross-ISA byte-identity extends to RT shaders** —
  the existing 2176-shader compare was done under nodxr, so RT shaders were
  plausibly absent; settled by re-running the compare harness after an RT
  boot.
* **Where the Vega card currently sits (installed or on the shelf)** — given
  by the tasking as "available"; settled by asking the owner before step 4.
