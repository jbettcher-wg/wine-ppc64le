# ppc64le/mf — Media Foundation for x86-64 guests

Wine's own `mfplat`/`mf`/`mfreadwrite` and the `winegstreamer` pipeline behind
them are **already correct on ppc64**: a native ppc64 PE that calls
`MFCreateSourceReaderFromURL` and loops on `ReadSample` gets PCM out of
GStreamer that is byte-identical to the source file. Nothing in this fold
replaces any of that. What is wrong at the boundary is **interface pointers**,
and this is the layer that fixes them — the same shape as `dlls/combase`'s
system COM, not a second decoder.

```
guest x86-64 PE  -->  C:\windows\sysx8664\{mfplat,mf,mfreadwrite,
   |                                       mfmediaengine,evr,wmvcore}.dll
   |                  (spec2thunk COM mode from the six .thunks files:
   |                   pure trap surface, no marshalling knowledge)
   |  trap; ntdll maps RIP -> (iface, slot), calls the NATIVE namesake's
   |  __wine_com_dispatch
   v
native mfplat.dll (dlls/mfplat/mfcom.c)
   |  = libs/winecom's dispatch loop over dlls/mfplat/mf_marshal.h
   v
Wine's own mfplat/mf/mfreadwrite  -->  winegstreamer  -->  GStreamer 1.28
```

**One winecom instance for six DLLs.** `libs/winecom`'s proxy state is
per-linkee, so if native `mf.dll` linked its own copy the `IMFMediaType` proxy
it minted would not be one of `mfplat`'s and the first `SetCurrentMediaType`
would be refused as a guest-implemented object. Native `mfplat.dll` owns the
only instance; `mf`, `mfreadwrite`, `mfmediaengine`, `evr` and `wmvcore` each
forward `__wine_com_dispatch` into it in their `.spec` and reach it through the
exported `__wine_com_*` helpers. All six GUEST modules publish the same roster,
which is what makes a proxy's guest vtable interchangeable between them.

`interfaces_mf.json` deliberately has **one copy**. Two generators read it —
`spec2thunk` COM mode for the guest stub arrays, `gen_winecom.py` for the
native marshal tables — and a second copy that drifted would dispatch a call
into the neighbouring slot with the neighbour's argument types.
`winecom_attach` cross-checks every IID and slot count of every loaded guest
module as the last line of defence.

## Files

| | |
|---|---|
| `gen_interfaces.py` | Wine's widl-generated MF headers → `interfaces_mf.json` |
| `gen_winecom.py` | that roster → `dlls/mfplat/mf_marshal.h`; `--report` explains every refusal |
| `interfaces_mf.json` | the ONE roster: 184 interfaces, 2373 vtable slots, across `mfplat`, `mf`, `mfreadwrite`, `mfmediaengine`, `evr` and `wmvcore` |
| `check-mf-smoke.sh` | the runtime gate (7 layers); `--sabotage` runs all three negative controls |
| `probes/mf_smoke.c` | one source, built as a native ppc64 PE and as an x86-64 guest PE |
| `probes/mf_async_probe.c` | guest-only: the reverse-proxy async path, measured |

```sh
./gen_interfaces.py --check interfaces_mf.json          # roster vs Wine's headers
./gen_winecom.py    --check ../../dlls/mfplat/mf_marshal.h
./gen_winecom.py    --report                            # what is refused, and why
```

## What is served

**The synchronous `IMFSourceReader` path, completely.** Create the reader,
select streams, read and set media types, `ReadSample` in a blocking loop,
`ConvertToContiguousBuffer`, `Lock`, read the bytes, `Unlock`, `Release`. Every
object involved is a proxy whose vtable is the guest module's own trap-stub
array. `check-mf-smoke.sh` runs exactly that loop in both a native ppc64 PE and
an x86-64 guest PE and requires byte-identical output, with the decoded PCM
checked against an FNV-1a hash computed from the WAV's data chunk by python3 —
so "the two runs agree" and "both agree with the file" are separate claims.

**Video too.** The gate uses a WAV because a sine wave is byte-deterministic
and needs no codec package installed. Measured separately on the same lane
with a 25-frame 320x240 H.264 mp4: the guest run and the native run both
decode 25 frames, 2,880,000 bytes of I420, FNV-1a `0x4D03A2AE`, timestamps
800000..10400000 — identical. (`MFVideoFormat_RGB32` as an output type is
refused with `MF_E_INVALIDMEDIATYPE` on *both* sides; that is a
winegstreamer limitation, not a boundary one, and I420/NV12 is what a game
uploads to a shader anyway.)

**The ASYNCHRONOUS path too**, which used to be this surface's headline
refusal; see the reverse-proxy section below.

Of the roster's **1821 non-`IUnknown` vtable slots, 1647 (90%) are served in
the FORWARD direction** — 1641 by the generated marshal tables and 6 by
hand-written slots — and 174 are refused with a named reason. Fifty-five of
those 174 are
refused *only forward*: they pass a float by value, which the forward invoker
(a widest-integer vtable call) cannot place and the reverse dispatcher can, so
their rows carry a complete plan and `WINECOM_F_REV`.
`IMFClockStateSink::OnClockSetRate` is the shape, and it is a sink — it is only
ever called in the reverse direction anyway. The whole platform object graph is
covered: `IMFAttributes` (every typed accessor), `IMFMediaType`, `IMFSample`,
`IMFMediaBuffer`, `IMF2DBuffer`, `IMFByteStream`, `IMFSourceResolver`,
`IMFMediaSource`, `IMFPresentationDescriptor`, `IMFStreamDescriptor`,
`IMFMediaTypeHandler`, `IMFTopology`, `IMFTopologyNode`, `IMFMediaSession`,
`IMFTransform` (all but `ProcessOutput`), `IMFSinkWriter`, and
**`IMFSourceReader` in full** — no refused slots at all, which took two
hand-written ones (below).

**Seek and duration, by hand.** The blanket `PROPVARIANT` refusal is right for
a table and wrong for three particular slots, because they are the ones a
cutscene player needs: `IMFSourceReader::SetCurrentPosition` (replay the
scene), `IMFSourceReader::GetPresentationAttribute` (`MF_PD_DURATION` — how
long is it) and `IMFMediaSession::Start`, which shares the first's
`(this, GUID *, PROPVARIANT *)` shape exactly. Two hand-written slots in
`dlls/mfplat/mfcom.c` serve them and **audit the tag at run time** instead: a
`VT_I8` position or a `VT_UI8` duration crosses, a `VT_UNKNOWN` is refused as
loudly as the blanket rule was. The gate proves the seek actually rewound
rather than merely returning `S_OK`, by decoding the whole file a second time
and requiring the same hash.

All **118 interface-bearing flat exports** across the six modules are
classified — 67 in `mfplat`, 31 in `mf`, 6 in `mfreadwrite`, 1 in
`mfmediaengine`, 7 in `evr` and 6 in `wmvcore` — because
`spec2thunk`'s flat-surface audit fails the build otherwise. Every one is
`GUEST-IMPL` with its own wrapper, including the ones that only refuse: the
shared `__wine_com_refuse` stub cannot say which export trapped, and on this
surface the refusals *are* the interesting half of the answer.

**`IUnknown` is on the roster**, which is a decision rather than an accident,
and it closed three holes at once: `QueryInterface(IID_IUnknown)` used to be
answered `E_NOINTERFACE` on every object; `IUnknown **` out-parameters
(`IMFCollection::GetElement`, `IMFAsyncResult::GetState`,
`IMFSourceResolver::CreateObjectFromURL`) used to be refused; and — the one
that mattered — `spec2thunk`'s flat audit builds its "does this signature carry
an interface" token set out of the roster, so a flat export taking a bare
`IUnknown *` was **invisible to the audit and passed a guest proxy straight
into native code**. `MFShutdownObject` is exactly that shape.

## What is refused, and why each is real

### The reverse-proxy direction — the central problem, and it is now served

`IMFAsyncCallback` is implemented **by the application** and Media Foundation
calls it back, from a work-queue thread the application never created. So are
`IMFSourceReaderCallback`, `IMFSinkWriterCallback`, `IMFClockStateSink`,
`IMFSampleGrabberSinkCallback`, `IMFByteStreamHandler` and `IMFSchemeHandler`.
A guest-implemented COM object handed **into** native MF means native ppc64
code jumping to an x86-64 vtable, which needs a **reverse proxy** — a native
vtable whose slots marshal ELFv2 arguments into MS-x64 and enter the guest
method through the emulator, the exact mirror of what `libs/winecom` builds in
the other direction.

`libs/winecom/reverse.c` builds it, and this surface turns it on
(`WINECOM_SF_REVERSE` in `dlls/mfplat/mfcom.c`). What that changed here:

* **the sixteen async flat exports are real wrappers** rather than named
  refusals — `MFPutWorkItem`, `MFPutWorkItem2`, `MFPutWorkItemEx`,
  `MFPutWorkItemEx2`, `MFPutWaitingWorkItem`, `MFScheduleWorkItem`,
  `MFScheduleWorkItemEx`, `MFInvokeCallback`, `MFCreateAsyncResult`,
  `MFBeginCreateFile`/`MFEndCreateFile`/`MFCancelCreateFile`, and the four
  MMCSS work-queue calls;
* **`IMFMediaEventGenerator::BeginGetEvent` needed no code at all.** Its row
  was already a `CA_IFACE_IN`; the generator now records the interface TYPE of
  such a parameter in `xaux` (with an `xmask` bit saying it did), which is what
  the reverse direction needs to know which of the 93 rostered vtables to
  build. So a guest can drive an `IMFMediaSession` by callback and not only by
  polling;
* **`MF_SOURCE_READER_ASYNC_CALLBACK` works**, for the same reason:
  `IMFAttributes::SetUnknown` puts the guest's callback into the store as a
  reverse proxy, and `dlls/mfreadwrite/mfcom.c`'s refusal became a trace.

**What it costs, measured.** `check-mf-smoke.sh` layer 6 hands native MF a
guest-implemented `IMFAsyncCallback`, requires `MFPutWorkItem` to return
`S_OK`, waits for `Invoke` on MF's own work-queue thread, and then checks the
things that are easy to get wrong and invisible when you do:

* the `IMFAsyncResult` the callback receives is a **forward proxy minted inside
  a reverse call** — the one place the two directions meet;
* `IMFAsyncResult::GetState` returns **the guest's own state object**, the same
  pointer it passed to `MFPutWorkItem`, rather than a wrapper around a wrapper.
  That round trip is what makes identity comparison and sink unregistration
  work at all;
* the callback's reference count is back where it started after `MFShutdown`;
* `Invoke` ran on a thread the guest had never run code on. The emulator's
  run-entry primitive adopts whatever thread calls it, so a native worker
  thread needs no thread-specific code anywhere — that is a property of
  `unix_emu_run_entry`, and this is where it is checked rather than assumed.

**What is still refused, and it is a wrapper's worth of work rather than a
design gap.** `MFCreateSourceReaderFromMediaSource` takes an `IMFMediaSource`
through the shared untyped `__wine_mf_translate_in`, which carries no roster
index — so a game that implements its own media source still gets a named
refusal, because there is no type to build a slot table from. Giving that
export a typed `__wine_mf_translate_in_iface` call is all it needs. The same is
true of `IMFByteStreamHandler`, `IMFSchemeHandler` and
`MFCreateSampleGrabberSinkActivate`.

`MFAddPeriodicCallback` is a different shape and a reverse proxy does not help
it: its argument is a bare guest FUNCTION POINTER with no vtable at all. The
port intercepts those at registration with ntdll's trampoline pool (which is
how `dlls/dinput8`'s enumerations are served); this export needs a callback
slot declared to `spec2thunk`, not a reverse proxy.

### The cross-surface gap

`libs/winecom`'s state is per-linkee **by design**, so an `IStream` proxy
minted by combase's system-COM instance is not one of this surface's proxies.
`MFCreateMFByteStreamOnStream`, `MFSerializeAttributesToStream`,
`MFCreateDXGISurfaceBuffer`, `MFCreateDXGIDeviceManager` and friends are
refused with the owning surface named. The same boundary means
`CoCreateInstance(CLSID_…, IID_IMFSourceReader)` fails with `E_NOINTERFACE`
from combase rather than returning an MF proxy — the `MFCreate*` entry points
are the served path.

### Signature-level refusals (`gen_winecom.py --report`)

| count | reason |
|---|---|
| 60 | `PROPVARIANT` — a tagged union that can hold `VT_UNKNOWN`, an interface pointer with no type anywhere in the signature. `IMFAttributes::GetItem`/`SetItem` and every `REFPROPVARIANT`. The **typed** accessors (`GetGUID`, `GetUINT32`, `GetUINT64`, `GetString`, `GetBlob`, `GetUnknown`) are fully served and are what callers use, and the three slots that really needed a `PROPVARIANT` have hand-written forms. |
| 11 | by-value `double` (`IMFAttributes::SetDouble`) — **forward only**: the row carries a complete plan and `WINECOM_F_REV`, so the reverse direction serves it. |
| 8 | by-value `float` (`IMFRateControl::SetRate`, `IMFSimpleAudioVolume`, `IMFAudioStreamVolume`, `IMFClockStateSink::OnClockSetRate`) — the native invoker calls the host vtable slot in its widest **integer** form, so a float argument would arrive in the wrong register file entirely. **Forward only**, for the same reason as the doubles: the reverse dispatcher marshals its own registers, and `ppc64le/winecom/check-reverse-proxy.sh` drives both widths through a guest object and compares the raw bits. |
| 7 | by-value `GUID` (`IMFMediaType::GetRepresentation`) — and this one is not a shrug. MS-x64 passes any aggregate that is not 1/2/4/8 bytes by a **hidden pointer**; ELFv2 passes a 16-byte struct in **two GPRs**. The guest puts an address in the argument slot and the native callee reads that address as the first half of the GUID. |
| 0 | `MFT_OUTPUT_DATA_BUFFER` (`IMFTransform::ProcessOutput`) held an `IMFSample *` and an `IMFCollection *` **inside a struct**; it now has the hand-written walker, `hand_process_output` in `dlls/mfplat/mfcom.c`, the shape `dlls/d3d12/main.c`'s `hand_resource_barrier` has (shallow-copy the `count`-sized array, translate `pSample` in exactly like an ordinary `CA_IFACE_IN`, leave `pEvents` `NULL` going in since it is OUT-only, wrap both back into guest proxies on the way out). Needs `ppc64le/mf/gen_winecom.py --out ../../dlls/mfplat/mf_marshal.h` re-run to pick up the `HAND_SLOTS` entry before the gate is green. |

Two flat exports return a `PROPVARIANT` legitimately (`MFGetSupportedMimeTypes`,
`MFGetSupportedSchemes`, plus `MFCreateSequencerSegmentOffset`). Those are
served and then **audited**: a string vector passes, a `VT_UNKNOWN` is cleared
and refused. There is no such audit for vtable slots because there is no
wrapper to put one in.

## The other three modules: `mfmediaengine`, `wmvcore`, `evr`

They were listed here as not done, each for a reason that turned out to be
about the ROSTER rather than about the module. All three are now on this one,
built by the same two generators, dispatched by the same single winecom
instance in native `mfplat.dll`.

**They share this roster; they do not each get one.** `libs/winecom`'s state is
per-linkee, so a second roster means a second instance and an object minted by
one is refused by the other as guest-implemented. A media engine is handed
`IMFAttributes` and `IMFMediaType` objects mfplat minted; an EVR display
control is reached with `MFGetService` on a sink `mf` minted; and Wine's
`wmvcore` sits on the same winegstreamer pipeline mfplat's source reader does,
so a title playing a `.wmv` through `IWMSyncReader` and its cutscenes through
`IMFSourceReader` is one process with both. One roster is what stops that being
two worlds. Each module's `.spec` forwards `__wine_com_dispatch` into mfplat,
exactly as `mf` and `mfreadwrite` already did.

| | before | after |
|---|---|---|
| interfaces | 93 | **184** |
| vtable slots | 1139 | **2373** |
| dropped by base-chain closure | 0 | **0** |

`mfmediaengine.h` contributes 20 interfaces, `evr.h`/`evr9.h` 13, `wmsdkidl.h`
58. Nothing was hand-picked: the roster is generated from the headers and all
three header sets close under `IUnknown`, so a curated subset would only have
been a second place to get slot numbers wrong.

Of the roster's non-`IUnknown` slots, **1641 are marshalled and 6 hand-written
against 174 refused** — the same 90% the smaller surface had, on a surface
more than twice the size.

**A finding on the way in, and it is the kind this fold exists to catch.**
`gen_winecom.py`'s `BYVAL_INTEGER` did not list `SHORT`/`USHORT`, while
`gen_interfaces.py`'s `SCALAR_BASE` has carried them all along. Two lists that
disagree is how a tooling gap gets reported as an ABI fact. It cost eight
slots, and they were not obscure ones: `IMFMediaEngine::GetNetworkState` and
`::GetReadyState` are what anything driving a media engine polls on every
frame, and `IMFMediaError::GetErrorCode` is how it finds out why playback
stopped. All are `unsigned short` — 2 bytes, one integer register, identical on
MS-x64 and ELFv2, never unrepresentable. `COLORREF` (a `DWORD`) went in beside
them for the same reason.

**What each module gets, and what it refuses.**

* **`mfmediaengine`** — one flat export that matters, `DllGetClassObject`, and
  the rest is interfaces. The notify callback that makes an engine useful
  (`IMFMediaEngineNotify`, called from MF's own thread) needs no code here: it
  is a guest-implemented object reaching native MF through the creation
  attribute store, i.e. through `IMFAttributes::SetUnknown`, whose `CA_IFACE_IN`
  row carries the interface type the reverse direction needs — the same road
  `MF_SOURCE_READER_ASYNC_CALLBACK` already travels.
* **`evr`** — the half a game actually calls is served and the half that is
  DirectX is refused by name. `IMFVideoDisplayControl` (place the video,
  letterbox it, grab a frame) is rectangles and enums and crosses;
  `MFCreateVideoMixer`, `MFCreateVideoPresenter` and
  `MFCreateVideoSampleFromSurface` take an `IDirect3DDeviceManager9` or an
  `IDirect3DSurface9`, which belong to the DXVK surface's winecom instance and
  not to this one. Those refuse the ARGUMENT rather than the export: `NULL`,
  which is legal and is what the EVR's own default path passes, is served.
  Three of evr's exports are mfplat's implementation re-exported with
  `-import`, and their `.spec` forwards point at mfplat's own wrapper so that
  one implementation does not get two opinions about its roster.
* **`wmvcore`** — six creation exports wrapped. `IWMSyncReader` is the path
  that needs no callback at all and is the analogue of the synchronous
  `IMFSourceReader` this fold's gate measures. `IWMReader` is asynchronous and
  its `IWMReaderCallback` is a vtable-method argument rather than a flat one,
  so the generated tables carry it.

**Refused, and each for a reason of its own rather than a shrug.** The four
`Priv` exports (`WMCreateReaderPriv`, `WMCreateSyncReaderPriv`,
`WMCreateWriterPriv`, `WMCreateBackupRestorerPrivate`) are implemented by Wine
and declared by no Wine header, so the signature oracle — which resolves a
NAME against the translation unit it compiles — cannot type them. So does
`MFCreateVideoMediaTypeFromVideoInfoHeader`, whose declaration sits behind
`#ifdef _KSMEDIA_`. Each keeps its ordinal as a named sentinel. That is the
same class as the `psapi`/`wldap32` renames in `ppc64le/corpus/CATALOG.md`: a
tooling gap, and the fix belongs in the oracle rather than in a `.thunks` file
asserting a signature by hand.

**NOT MEASURED, and this is the honest part.** No corpus title has created a
media engine, opened a Windows Media file or built an EVR sink on this port.
`ppc64le/mf/check-mf-smoke.sh` proves the surface these three joined still
works — the same 7 layers, the same byte-identical native-vs-guest transcript,
against a roster more than twice the size — and that is the whole of what has
been proven. What these three modules do when a program actually drives one is
unmeasured, and a gate for it needs a program that drives one. Written down
here rather than left to be inferred from the absence of a gate.

## Not done

* **A guest-implemented `IMFMediaSource` or `IWMStatusCallback`** still reaches
  the shared untyped `__wine_mf_translate_in`, which carries no roster index
  and therefore has no slot table to build a reverse proxy from. Named refusals
  in `MFCreateSourceReaderFromMediaSource` and `WMCreateBackupRestorer`; the
  fix is the typed `__wine_mf_translate_in_iface` call, which already exists.
* **The DirectX cross-surface boundary.** `libs/winecom`'s state is per-linkee
  by design, so a `IDirect3DSurface9` from the DXVK surface cannot be
  translated by this instance. `evr`'s three D3D-taking exports, plus
  `MFCreateDXGISurfaceBuffer` and `MFCreateMFByteStreamOnStream`, all refuse
  with the owning surface named.
