# ppc64le/mf — Media Foundation for x86-64 guests

Wine's own `mfplat`/`mf`/`mfreadwrite` and the `winegstreamer` pipeline behind
them are **already correct on ppc64**: a native ppc64 PE that calls
`MFCreateSourceReaderFromURL` and loops on `ReadSample` gets PCM out of
GStreamer that is byte-identical to the source file. Nothing in this fold
replaces any of that. What is wrong at the boundary is **interface pointers**,
and this is the layer that fixes them — the same shape as `dlls/combase`'s
system COM, not a second decoder.

```
guest x86-64 PE  -->  C:\windows\sysx8664\{mfplat,mf,mfreadwrite}.dll
   |                  (spec2thunk COM mode from the three .thunks files:
   |                   pure trap surface, no marshalling knowledge)
   |  trap; ntdll maps RIP -> (iface, slot), calls the NATIVE namesake's
   |  __wine_com_dispatch
   v
native mfplat.dll (dlls/mfplat/mfcom.c)
   |  = libs/winecom's dispatch loop over dlls/mfplat/mf_marshal.h
   v
Wine's own mfplat/mf/mfreadwrite  -->  winegstreamer  -->  GStreamer 1.28
```

**One winecom instance for three DLLs.** `libs/winecom`'s proxy state is
per-linkee, so if native `mf.dll` linked its own copy the `IMFMediaType` proxy
it minted would not be one of `mfplat`'s and the first `SetCurrentMediaType`
would be refused as a guest-implemented object. Native `mfplat.dll` owns the
only instance; `dlls/mf/mf.spec` and `dlls/mfreadwrite/mfreadwrite.spec`
forward `__wine_com_dispatch` into it and reach it through the exported
`__wine_com_*` helpers. All three GUEST modules publish the same roster, which
is what makes a proxy's guest vtable interchangeable between them.

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
| `interfaces_mf.json` | the ONE roster: 93 interfaces, 1139 vtable slots |
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

Of the roster's **860 non-`IUnknown` vtable slots, 773 (90%) are served in the
FORWARD direction** — 768 by the generated marshal tables and 5 by hand-written
slots — and 87 are refused with a named reason. Nineteen of those 87 are
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

All **104 interface-bearing flat exports** across the three modules are
classified — 67 in `mfplat`, 31 in `mf`, 6 in `mfreadwrite` — because
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
| 1 | `MFT_OUTPUT_DATA_BUFFER` (`IMFTransform::ProcessOutput`) holds an `IMFSample *` and an `IMFCollection *` **inside a struct**; it needs a hand-written walker, the shape `dlls/d3d12/main.c`'s `hand_resource_barrier` has. This is the one refusal that blocks a real workflow — driving a decoder MFT by hand — and it is a day's work, not a design problem. |

Two flat exports return a `PROPVARIANT` legitimately (`MFGetSupportedMimeTypes`,
`MFGetSupportedSchemes`, plus `MFCreateSequencerSegmentOffset`). Those are
served and then **audited**: a string vector passes, a `VT_UNKNOWN` is cleared
and refused. There is no such audit for vtable slots because there is no
wrapper to put one in.

## Not done

* **`mfmediaengine`** — the HTML5-style `IMFMediaEngine` is callback-driven
  (`IMFMediaEngineNotify`) end to end. That is no longer a blocker in
  principle, since reverse proxies exist; it is a separate roster and a
  separate module, and nothing in the corpus has asked for it yet.
* **`wmvcore`** — no corpus title has been observed asking for it. It is a
  separate roster (`IWMReader` and friends) and adding it before something
  needs it would be surface with no gate behind it.
* **`evr`** — the enhanced video renderer presents through DirectX, which is a
  different winecom surface (see the cross-surface gap above).
