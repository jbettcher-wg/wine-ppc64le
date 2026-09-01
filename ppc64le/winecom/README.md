# ppc64le/winecom — reverse proxies, and the gate for the mechanism itself

`libs/winecom` has always had one direction. A **native** interface pointer
handed to an x86-64 guest is given a **guest** vtable — the trap-stub array a
`spec2thunk` COM module publishes — so the guest's `call [rax+0x18]` lands on a
trap that ntdll routes into the dispatch loop, which marshals and calls the
real native method. That is what makes `IMFSourceReader`, `IDirectSoundBuffer`,
`IDirectInputDevice8` and `IMoniker` work at all.

The other direction did not exist, and six subsystems filed the same need in
the same sentence: **the API's whole contract is "you implement it, we call
it."** Media Foundation's async model *is* `IMFAsyncCallback`. XAudio2 reports
buffer completion through `IXAudio2VoiceCallback`, from its mixer thread.
DirectSound aggregates through `pUnkOuter`. Every one of those is a
guest-implemented COM object — an x86-64 vtable at a guest address — handed to
native ppc64 code that will `bctrl` through it.

`libs/winecom/reverse.c` is the mirror: a **native vtable** wrapping a guest
interface pointer, whose per-slot stubs marshal ELFv2 arguments into MS-x64 by
the **same generated slot tables** the forward direction reads, enter the guest
method through the emulator, and marshal the result back.

```
native ppc64 caller                                 guest x86-64 object
   |  bctrl through slot n of a reverse proxy
   v
rev_stub_n  (libs/winecom/reverse.c)
   |  reads r3-r10 and f1-f13 whole; the slot's cls/xaux/fpmask says
   |  which registers are real arguments and what each one is
   v
winecom_reverse_dispatch
   |  builds an MS-x64 argument block; interface arguments become forward
   |  proxies, interface out-parameters come back as native pointers
   v
ntdll's guest-callback trampoline  ->  a 109-byte x86-64 shim
   |  unpacks the block into RCX/RDX/R8/R9, XMM0-3 and four stack slots
   v                                                  ---> the guest method
```

## The three pieces

**One stub per SLOT NUMBER, not per method.** `rev_stub_0..rev_stub_63` are
ordinary C functions whose prototype is a deliberate over-declaration: eight
integer parameters and thirteen doubles, which under ELFv2 is exactly `r3-r10`
and `f1-f13` — the whole argument register file, with nothing spilling to the
parameter save area. A native caller calling slot *n* with its own real
signature fills some subset; the stub reads all of them and the generated class
table says which are real and which register each came from. The interface is
carried by `this`, so slot 7 of one interface and slot 7 of another want the
same stub.

A float is what makes reading the register file *whole* worth doing. ELFv2 puts
a floating-point argument in the next FPR and **reserves** its integer slot, so
a method `(float, int)` leaves the int in `r5` and not `r4` — which is exactly
what walking the class table reproduces.

**One trampoline for the whole mechanism.** ntdll's guest-callback pool carries
four integer arguments into guest code, which is not enough for a COM method.
So the one argument it does carry is a parameter block, and the guest end is a
small x86-64 shim written into anonymous executable memory — the same thing
`call_guest_function_args` does for its own thunk. The per-call target lives in
the block, so the hot path takes no lock and does no lookup. That matters
because one of the consumers calls this from a realtime audio thread.

**Identity, both ways.** Reverse proxies are interned by (guest pointer,
interface) exactly as forward proxies are interned by (host pointer,
interface), and the two halves recognise each other:

| | |
|---|---|
| `winecom_wrap(` a reverse proxy `)` | the original **guest** pointer |
| `winecom_to_native(` a forward proxy `)` | the original **host** pointer |
| `winecom_to_guest(` a reverse proxy `)` | the original **guest** pointer |

This is not a nicety. COM callers compare interface pointers for identity,
aggregate on the assumption that `QueryInterface(IID_IUnknown)` is canonical,
and unregister a sink by handing back the pointer they registered — which is
literally how `IXAudio2::UnregisterForCallbacks` finds a registration. A
mechanism that minted a fresh wrapper each way would break all of that quietly.

## What is refused, and why each is real

Refusal discipline survives the new direction unchanged. A slot is refused
here, once, by name, when:

* its forward row carries a `refuse` string — the tables' judgement about the
  **signature** (a `PROPVARIANT`, a by-value `GUID`, a struct that reaches an
  interface pointer, an untyped `void**`) stands in both directions;
* it is served forward by a hand-written slot, whose signature lives in a C
  function rather than in the table, so there is nothing here to marshal by;
* it returns an aggregate through a hidden first argument, takes an interface
  array or a completion-event handle, or is wider than the eight-argument
  register file;
* its interface-typed IN parameter has no `xmask` bit — see below.

The **one** exception is spelled by the generator rather than assumed by the
runtime. `WINECOM_F_REV` on a row means "`cls`/`xaux`/`fpmask` describe this
signature completely", and the generators set it on exactly one shape: a slot
whose only problem is a **by-value float**. The forward invoker calls a native
vtable slot in its widest integer form and cannot place one; this dispatcher
marshals its own registers and can. `IMFClockStateSink::OnClockSetRate` is the
method that shape is named after, and it is a sink — it is only ever called in
this direction anyway.

### `xmask`, and the fail-open it closes

The reverse direction needs the interface **type** of a `CA_IFACE_IN`
parameter, because a native object arriving as an argument of a guest method
has to be given one of the rostered guest vtables and identity cannot say
which. The type goes in `xaux`, which the forward direction already used for
the OUT classes.

That is not enough on its own, and the hole is worth stating plainly: a row's
`xaux[]` is **one array shared by every parameter**, an untouched slot reads
`0`, and roster index 0 is a *real interface* (the rosters are sorted by name).
So "the generator wrote interface 0 here" and "the generator wrote nothing
here" were the same value. Tables written before this work — the 58 legacy
system-COM blocks in `dlls/combase/syscom_marshal.h`, where nineteen slots have
an IFACE_IN parameter sharing a non-NULL `xaux[]` with an OUT_STATIC one —
would have handed a guest method an object of whatever sorts first.

`xmask` is one bit per parameter saying **the generator filled this in**. A
table that predates the field has `xmask == 0`, so every such parameter fails
**closed** and is refused by name, which is the only safe reading of "no
information".

## The lifetime edge

The object's reference count is the **guest's**. A reverse proxy holds exactly
one guest reference for its whole life and hands it back when its own
native-visible count reaches zero. If native code is still holding the proxy
when the guest tears its object down anyway — a game that frees its callback
while an XAudio2 voice still points at it — the guest has broken the COM rules
and the next reverse call enters freed guest memory. That is the same failure a
native-only program would have, at the same moment, for the same reason; this
layer neither adds it nor can remove it. What it can do is make it legible, so
the destruction is traced with **both** pointers.

A `[local]` interface is the one case with no reference count at all —
`IXAudio2VoiceCallback`'s slot 1 is `OnVoiceProcessingPassEnd`, not `AddRef`.
XAudio2 stores such a pointer for a voice's whole life without ever taking a
reference, because there is nothing to take. So a reverse proxy for a `[local]`
interface is **permanent**: interned once, never freed. Freeing one instead
would hand the mixer thread a dangling native vtable at the next buffer
boundary, on the one thread where a fault is least recoverable.

## Files

| | |
|---|---|
| `check-reverse-proxy.sh` | the mechanism gate (5 layers); `--sabotage` runs three negative controls |
| `probes/reverse_probe.c` | guest-only: two COM objects in its own image, handed to a native hook |
| `check-com-levers.sh` | the gate on the three wave kill switches; `--sabotage` runs the three unarmed controls |
| `probes/com_lever_smoke.c` | guest-only: one served row and one riid-typed handout, watched from the caller's chair |
| `derive-wave-rows.py` | derives the wave membership FROM GIT and regenerates both files below |
| `wave-rows.list` | the derived membership, with its provenance in the header |
| `../../libs/winecom/winecom_waves.h` | the runtime's generated copy of that list |

## The wave kill switches

The completeness landings `74591109c3f..c199f79caf9` turned hundreds of
refused rows into served ones in one stretch and shipped no way to put any of
them back. Bisecting the Witcher 3 load regression that followed cost **seven
seat runs** of swapping built PE halves in and out of a tree
(`ppc64le/docs/sessions/2026-09-01/w3-load-regression-bisect.md`). Three
levers, read once at attach, make the same legs one environment variable:

| lever | what it does |
|---|---|
| `WINEEMUNOCOMROWS` | comma-separated `Iface::Slot` names, or `@/path/file`. Each named row takes the **generated-refusal path**: refuse once by name, `E_NOTIMPL`, and `scrub_refused_outs()` — refused means INERT. |
| `WINEEMUNOCOMIIDS` | comma-separated IIDs, `{xxxxxxxx-…}` or a bare leading 8 hex digits; `@file` too. A listed IID is treated as **unrostered** where interfaces are handed out: release the object, NULL the out pointer, `E_NOINTERFACE`. |
| `WINEEMUNOCOMWAVE` | `getfamily`, `syscom`, `dinput8`, `rest` — whole landings, expanded to the row and IID sets `derive-wave-rows.py` derived from git. The four **partition** the landing (466 rows), so all four is the entire stretch off. |

An `@file` takes one name per line with `#` comments — **and reads
`wave-rows.list`'s own dialect too**: `[section]` headers skipped, a
`row `/`iid ` prefix stripped, each lever taking only its own kind of line.
That is what lets the checked-in list, or any excerpt of it, go straight to
either lever. Without it the documented example would have matched nothing —
loudly, but a leg that forces nothing and reads as "tested, clean" is the
exact failure the levers exist to prevent.

Two rules the derivation is built on, both in the script's own banner: a row
whose `refuse` went from a reason string to `NULL`, and a row whose `caux`
went from `NULL` to a real count-parameter array. The second rule exists
because `OMGetRenderTargets` — the bisect's own theory 1 — never carried a
refuse string at all: it was refused **at runtime** by a dispatcher that could
not find its count parameter, so the first rule cannot see it.

**A name that matches nothing is loud.** A typo in a bisect leg that passed
silently would be recorded as "tested, clean", and the conclusion drawn from
it would be wrong in the most expensive direction. Every unmatched target and
every unknown wave name gets its own line.

**The hot path pays nothing when they are unset**: the row lever's result is a
per-`(iface, slot)` byte array that is only allocated when something matched,
so the unarmed test is one `NULL` pointer check. Resolution happens once, at
attach, against the surface's own `const` tables — which are never written.

```sh
./check-com-levers.sh              # every leg an armed/unarmed pair
./check-com-levers.sh --sabotage   # the three unarmed controls must show the
                                   # lever NOT firing
./derive-wave-rows.py              # re-derive both files from git
./derive-wave-rows.py --check      # and fail if either drifted
```

The hook is `__wine_winecom_reverse_selftest`, declared in
`include/wine/winecom_selftest.h` and implemented in `dlls/mfplat/mfcom.c`.
It lives on the Media Foundation surface because that surface already rosters
an interface with one method of every class this mechanism marshals —
`IMFAttributes`: `SetUINT32`, `SetUINT64`, `SetDouble` (a by-value double the
forward direction refuses), `SetString`, `SetUnknown` (an interface IN),
`GetUnknown` (an interface OUT through a `REFIID`), `GetCount` (an integer OUT)
and `SetItem` (a `PROPVARIANT`, refused in **both** directions and present to
prove the refusal discipline survived) — plus `IMFSimpleAudioVolume` for the
one class `IMFAttributes` has no method of, a by-value single-precision float.

Nothing in Wine calls the hook and nothing but the gate ever will. It is
exported so the probe reaches it through the ordinary `spec2thunk` `GUEST-IMPL`
path, which is the point: the hook is entered the same way every other flat
export is, so the gate measures the real boundary and not a private back door
into it.

**Both sides check.** The hook checks what came back to it — return values,
both round trips, `QueryInterface` identity, the refusal, reference balance —
and the probe checks what arrived, every argument value, and the floating-point
ones as **raw bit patterns**, because a double that lost its low bits crossing
between an FPR and an XMM register is still "about right" as a decimal.

```sh
./check-reverse-proxy.sh              # 23/23 probe checks + 12 hook checks
./check-reverse-proxy.sh --sabotage   # all three controls must go red
```

The three controls, and what each one would catch:

1. `WINEEMUNOCOMWRAP=1` hands pointers across **raw** in both directions — the
   guest object reaches native code as x86-64 bytes to call.
2. `WINEEMUNOCBWRAP=1` takes the guest-callback trampoline pool away, so the
   raw x86-64 shim address comes back and there is nothing to enter guest code
   with. The mechanism must refuse to arm itself and say so. This control is
   specific to this gate: it is the one that proves the crossing really goes
   through the emulator rather than through some accident.
3. A probe built expecting one different constant must fail its own value
   check, which proves the arguments are **compared** and not merely printed.

## Consumers

The mechanism is measured through its consumers too, because a gate for the
machinery and a gate for the thing the machinery was built for are different
claims:

* `ppc64le/mf` — `IMFAsyncCallback` invoked from a Media Foundation work-queue
  thread, with the `IMFAsyncResult` arriving as a forward proxy minted inside
  the reverse call and the guest's own state object coming back as itself;
* `ppc64le/audio` — `IXAudio2VoiceCallback::OnBufferEnd` from XAudio2's
  **mixer** thread, value-checked against the `pContext` the guest submitted;
* `ppc64le/shell` — the one consumer that is **not** a reverse proxy.
  `IDirectInput8::EnumDevices` hands native dinput a bare guest FUNCTION
  POINTER, and a reverse proxy is a vtable; that one is served by swapping the
  pointer for one of ntdll's guest-callback trampolines at the moment it
  arrives, which is the older mechanism and the right one for a callback with
  no vtable to build.
