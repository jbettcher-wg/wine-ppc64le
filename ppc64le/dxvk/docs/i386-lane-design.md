# The i386 half of the dxvk thunk surface — design, what is done, what is not

Started 2026-08-19.  dexwin (Dex, PE32, Unity 5) is the canary: the i386 guest
boots through the ABI-4 bridge and Unity puts its window up, then dies with no
graphics because i386 `dxgi.dll` forwards to `d3d11.__wine_dxvk_*` and this
port's i386 d3d11 exports none of the dxvk surface.  Portal 2, Half-Life 2 and
the Win32 Styx wait behind the same wall.

## Why this module and not the whole Win32 surface

The 64-bit lane thunks everything: a guest's imports bind to trap stubs and
ntdll marshals MS-x64 to ELFv2, which works because guest and native are both
LLP64 and a struct is the same bytes on both sides.

The i386 lane is **real WoW64** (74f9b452924): Wine's own i386 builtins run
under the emulator and convert at the SYSCALL boundary, where wow64.dll and
wow64win.dll already carry ~1500 conversions.  Thunking the Win32 surface for
i386 was rejected there on purpose.  So a `.thunks` file gets **no i386 half
by default**; a module opts in with `GUEST-MACHINE i386`, and only three do —
d3d11, dxgi, d3d10core — because this tree REPLACED their implementation with
dxvk behind a unixlib, so their i386 build forwards to entries that exist only
in the native ppc64 module.  No amount of WoW64 helps with that.

## DONE (commit 9732a028c12), and verified

1. **spec2thunk emits either guest machine** (`--machine i386`).  Stub body is
   `int 0x80` (`CD 80`, trap_off 0) — the instruction this FEXCore build
   routes into the same OS_GENERIC sink `0F 05` reaches on the 64-bit side.
   i386 image base, `/safeseh:no` (a hand-written stub array carries no
   handlers), and the leading underscore an i386 PE puts on symbol names: the
   OBJECT is decorated, the `.def` is not, because lld-link decorates the
   names a `.def` mentions itself.
   Verified: PE32, `cd 80` at 16-byte stride, undecorated exports, COM stub
   arrays present, Wine builtin signature stamped.

2. **makedep: `GUEST-MACHINE`, and `thunk_owns_arch()`** so the ordinary
   from-source PE build is suppressed for a thunk-owned arch instead of both
   rules naming one target and make picking by luck.  Configure the tree
   `--enable-archs=ppc64,i386` for this lane.
   Verified: full build green, zero overriding-recipe warnings, i386 kernel32
   (2.1 MB) and user32 (6.2 MB) are Wine's own and untouched, beside the three
   thunks at ~55 KB.

3. **Generated struct repacks** (`ppc64le/thunks/gen_repack32.py`,
   `dlls/d3d11/d3d11_repack32.h`).  52 of 328 D3D11/DXGI aggregates lay out
   differently for an i386 guest.  The scoping pass proposed hand-writing a
   dozen walkers and refusing the rest as "no title of this era calls them" —
   per-title triage inside a Wine port, wrong twice over: the divergences are
   mechanical, and a fork of Wine owes every export it declares.  So the
   generator measures instead (clang's own record layouts for both targets,
   plus a sizeof pass for the widths the dump omits) and emits both directions
   for all 52, recursing into nested divergent aggregates and refusing at
   GENERATION time — never silently — anything it cannot decide.
   Verified: compiles clean for ppc64le, x86_64-windows and i386-windows.

## BUILT (2026-08-28) — the crux resolved to a per-process constant

Everything below this banner is the 2026-08-19 stopping point, kept for the
record.  The three pieces it names are now in the tree, and the deciding
insight was that "one proxy runtime parameterised by guest width, or a
separate 32-bit one" was a false choice: **a process has exactly one guest
machine for its whole life** (an i386 guest is a WoW64 process), so the
runtime is keyed once, at attach, and `struct com_proxy` never changed —
proxies and the 4-byte-slot vtable block are allocated below 4 GiB, and on
this little-endian host the guest's 4-byte load at offset 0 reads exactly
the low half of the field the native side reads as 8.

* **emu32 routing**: `EMU32_RUN_TRAP` (unix classifies an `int 0x80` at a
  non-bop Eip as a trap, the PE side resolves it), `emu32_dispatch_thunk` in
  signal_ppc64.c — 32-bit loader-list walk, shared RIP cache (`lane32`),
  flat stdcall dispatch from the version-8 **geom32** frame word spec2thunk
  now measures with a second oracle pass at the i386 target (an 8-byte-class
  x64 argument is a pointer=1 slot or an int64=2 there, and the width words
  could not tell them apart; no geometry ⇒ the frame cannot even be popped ⇒
  refuse).
* **dispatch32** (`winecom_dispatch32`): serves a row only under BOTH
  version stamps — `WINECOM_F_I386_GEOM` (decode + callee-pop) and
  `WINECOM_F_I386_STRUCTS_OK` (every pointer parameter audited against
  `gen_repack32.py --json`'s measured layout roster; divergent pointees are
  repacked through `reps[]`, out-direction repacks that would truncate a
  host pointer are refused as `refuse32`).  It owns the whole epilogue —
  Eax, Edx for EDX:EAX, the stdcall pop — because on i386 the pop is
  per-slot knowledge.  `hand32` walkers (matched by slot name at attach)
  serve what no rep can: the float slots, presentation, the texture creates
  (initial-data count = MipLevels×ArraySize out of the desc), and
  **Map/Unmap, which BOUNCE**: DXVK's mapped host pointer sits above 4 GiB,
  so the guest is served a guest-legal buffer sized from the resource's own
  description, copied in for the read modes and flushed back before Unmap.
* **The gate**: `check-d3d11-smoke32.sh` — the same d3d11_smoke.c probe,
  built PE32, must print stdout byte-identical to the native ppc64le run,
  and the +d3d11 trace must show Map served by its walker.  First full run:
  PASS 7/7, byte-identical, `Map(READ)` walking 4096 texels through the
  bounce.

## NOT DONE (2026-08-19 record) — and the real crux, which is not the struct layouts

An i386 guest now binds d3d11 and traps at a stub that nothing answers.  Three
pieces remain, and the third is much bigger than this document originally said.

* **emu32 routing.**  `unix_emu32_run` classifies an exit by EIP: the two bop
  addresses become SYSCALL/UNIXCALL and everything else becomes
  `EMU32_RUN_FAULT`.  A stub trap therefore arrives as a fault today — safe,
  and the honest failure — and needs a new reason plus a resolve step in
  `BTCpuSimulate`.  Small.

* **dispatch32 in libs/winecom.**  stdcall puts every argument in a 4-byte
  stack slot at ESP+4+4n; widen into the same `UINT64 args[]` the 64-bit path
  builds (the marshal table's existing `dwordmask`/`dwordsign` already say
  which are narrow and which are signed), apply the repacks above to struct
  parameters, and do the callee-pops return.  Medium, and well-bounded.

* **THE CRUX: a proxy is pointer-width.**  `struct com_proxy`'s first member
  IS the vtable pointer the guest dereferences, and `find_guest_module()`
  walks `NtCurrentTeb()->Peb->LdrData` — the 64-bit loader's list.  For an
  i386 guest every proxy handed out must be a 32-bit object with a 4-byte
  vtable pointer, materialised from the i386 module's stub arrays, interned
  separately, and reachable from the 32-bit loader namespace.  That is not a
  parameter conversion; it is a second instantiation of the whole proxy
  runtime, keyed by guest machine — comparable in size to the original 64-bit
  COM bring-up.  Anything less produces a COM path that looks plausible and
  hands a 32-bit guest a 64-bit vtable pointer, which is this codebase's most
  expensive bug class.

The stopping point was chosen there deliberately: everything above the crux is
complete, measured and independently useful (the repack generator serves any
surface, and the i386 emitter serves any module that opts in), and the crux is
a design decision — one proxy runtime parameterised by guest width, or a
separate 32-bit one — that deserves to be made on purpose rather than
discovered halfway through an implementation.
