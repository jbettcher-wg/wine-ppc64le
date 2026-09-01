# CoCreateInstance(CLSID_DirectInput8) — the cross-surface routing decision

Status: DESIGN, not built (2026-09-01).  Filed by the completeness pass;
the syscom pass identified {bf798031-…} = IID_IDirectInput8A/W as the
second long-unknown IID in the wrap_out_iface choke point.

## The problem

Games reach DirectInput two ways.  `DirectInput8Create` goes through
dinput8's own flat wrapper and its own winecom instance — served since the
shell wave.  `CoCreateInstance(CLSID_DirectInput8, …, IID_IDirectInput8A)`
goes through COMBASE: syscom's `wrap_out_iface` looks IID_IDirectInput8A up
in the SYSCOM roster, finds nothing, releases the object and answers
E_NOINTERFACE.  Honest, loud — and a refusal of a path real games walk.

## Why syscom cannot simply roster IDirectInput8

libs/winecom state is **per-linkee by design** (system-com-design.md §4.2):
a proxy minted by syscom's instance is recognized only by syscom's runtime,
and its guest vtable comes from the stub arrays of the guest module the
SURFACE names (combase.dll's thunk).  A syscom-minted IDirectInput8A proxy
would work — until it vends an IDirectInputDevice8A, whose marshal rows,
hand walkers (EnumDevices' trampoline swaps, the 2026-09-01 shims) and
per-slot knowledge all live in DINPUT8's surface.  Duplicating the tables
into syscom means every dinput fix lands twice or drifts.

## The design that fits the architecture

Route the CREATION, not the interface: combase's guest-facing
`CoCreateInstance` wrapper special-cases CLSID_DirectInput8 (the same shape
as its existing per-class routing for XAudio2's CLSIDs) and forwards to
**dinput8's own exported guest entry** — a small
`__wine_guest_dinput8_cocreate( REFIID, void ** )` exported from
dinput8.dll, which calls DirectInput8Create(GetModuleHandle, 0x0800, …)
and wraps through DINPUT8's instance.  The guest receives a dinput8-surface
proxy, exactly as if it had called DirectInput8Create — one surface owns
the type end to end, and CoCreateInstance's contract (an
IDirectInput8A/W* out) is met.

Loose ends the builder must close:
* aggregation (pUnkOuter) answers DIERR_NOAGGREGATION as the flat wrapper
  does;
* CoCreateInstanceEx / class-factory paths for this CLSID either route the
  same way or keep a named refusal;
* combase must load dinput8.dll on demand (LoadLibrary at the routing
  point — the guest asked for the object, so pulling the module in is the
  contract, not a surprise);
* the refusal text in syscom's wrap_out_iface should name this file once
  the route exists, so the next unknown-IID hunt ends here.
