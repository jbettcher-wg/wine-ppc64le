# Oblivion Remastered (UE5) — where the wall actually is

**Date measured: 2026-08-30, on the AC922 (192.168.2.24).**
Binary: `OblivionRemastered/Binaries/Win64/OblivionRemastered-Win64-Shipping.exe`, appid 2623190.
Runs analysed:
- `~/.local/share/wine-ppc64le/oblivion/wine-ppc64le-native-20260830-090033-1377953.log` (warn+module; the run the task was written from)
- `~/.local/share/wine-ppc64le/oblivion/wine-ppc64le-native-20260830-090730-1379865.log` (my re-run, `WINEDEBUG=warn+module,+seh` — the decisive one)

## HEADLINE: the task premise is wrong about the current wall

**Oblivion does NOT reach msvcp140. Not one of the 138 MSVCP140 sentinels — nor
any VCRUNTIME140 / VCRUNTIME140_1 sentinel — is ever called.** The `+seh` run
contains zero `DEAD00xx` faults, zero `wild pointer`, zero `handle_syscall_fault`,
and no `c0000005`. The 138 imports are *bound* to sentinels at load (that is the
port working as designed — an unresolved import does not stop loading) and then
the process dies long before touching any of them.

**How it actually dies (rc=51 = exit code 0x33):** the game reaches
`SteamAPI_Init` inside the tree's guest `steamclient64.dll`, and that fails. The
game then pops a `MessageBoxA` (arg r9 = 0x10 = MB_ICONERROR) and calls
`KERNEL32.TerminateProcess(-1, 0x33)`. That 0x33 is the rc=51. This is the
classic "Steam must be running to play this game" error path, not a C++-runtime
crash.

### The exact sequence (from the +seh `find_guest_thunk_target` trace, thread 012c)

The game's own steam_api64 emits (captured as an OutputDebugString in the log):

    [S_API] SteamAPI_Init(): Loaded 'steamclient64.dll' OK.

then inside `steamclient64.dll`'s `SteamAPI_Init` → `rpc_connect`
(`dlls/steamclient64/steamrpc.c`):

    GetEnvironmentVariableW (STEAM_BRIDGE_ADDR) → strrchr → atoi
    WSAStartup → socket → memset → htons → inet_addr → connect → setsockopt(TCP_NODELAY)
    wine_get_unix_file_name → memcpy → send → recv → strcmp/strncmp (interface-version reply)

**The bridge socket CONNECTS and completes a send/recv handshake** — `setsockopt`
only runs on the success path *after* `connect()` in `rpc_connect`, and the
`send`/`recv`/`strcmp` show the RPC interface negotiation actually happened. So
the bridge helper (`ppc64le/steamapi/helper`, listening on 127.0.0.1:38281 for
this run) is reachable and answered.

Then steam_api64 finishes Init and does its **legacy "is Steam running" probe**:

    GetModuleHandleExA → CreateFileW → OpenEventA → OpenFileMappingA → MessageBoxA → TerminateProcess(0x33)

`OpenEventA` + `OpenFileMappingA` here is the canonical
`Local\SteamStart_SharedMemFile` / `Local\SteamStart_SharedMemLock` check that
Windows steam_api uses to confirm a live local Steam client. **Nothing in this
tree backs those named objects** (grep of `dlls/steamclient64/`,
`ppc64le/steamapi/`, `ppc64le/steamtool/` for `SteamStart_SharedMem` /
`SharedMemFile` / `IsSteamRunning` → no host-side implementation). So the probe
fails → error box → terminate.

This is exactly the wall the tree *predicted* for Steam-DRM titles:
`ppc64le/games/STATUS.md:644` already names `Local\SteamStart_SharedMemFile` /
`Local\SteamStart_SharedMemLock` as the "next wall after the ntdll-seed fix" for
Skyrim/Boltgun. Oblivion reaches it by a different road (steam_api64 directly,
no SteamStub DRM wrapper on the shipping exe) but it is the same missing piece.

**Steam itself was up and logged in during the run** (client user "Akad"
[U:1:910372], `~/.local/share/Steam/logs/connection_log.txt` heartbeats through
09:01, my run 09:07). So this is NOT "start Steam first" — the real Linux client
is running and the bridge talks to it; the gap is the *Windows-side shared-memory
liveness object* that steam_api64 insists on and the bridge does not synthesise.

## Boot-critical msvcp140 subset: currently EMPTY

The subset of the 138 that is on the path to first frame **cannot be measured
yet**, because the game dies at Steam init before engine start. Implementing any
of the 138 today changes nothing for Oblivion. This is the "do not bulk-implement
138 exports before establishing which matter" case, taken to its limit: right now
*zero* of them matter for this title. They will start to matter only once the
Steam wall falls and the engine actually spins up — at which point the call-order
triage the task describes becomes the right method.

## Why all 138 bind to sentinels (the FROM-SPEC gap — real, and worth fixing for OTHER titles)

Independently confirmed and worth recording, because it blocks every title that
*does* reach msvcp140 (Quake II remaster, etc.), even though it is moot for
Oblivion today:

- `winedump -j export` on the built `dlls/msvcp140/x86_64-windows/msvcp140.dll`:
  **104 ordinal slots, but only 2 named exports.** So a guest import of
  `MSVCP140._Cnd_signal` (or any of the other 136) finds no matching name in the
  builtin thunk and falls through to ntdll's per-symbol sentinel. That is the
  whole reason the log shows `_Cnd_signal → DEAD0018` instead of a trap thunk.
- `dlls/msvcp140/msvcp140.thunks` is `FROM-SPEC auto` with **no `INCLUDE-DIR` /
  `PROBE-EXTRA`** and exactly one hand line (`_Mtx_init_in_situ`, via the
  `.spec`-location downgrade). Running the generator directly
  (`tools/spec2thunk/spec2thunk --from-spec dlls/msvcp140/msvcp140.spec
  --body=trap --wine-generated <build>/include`) reports:
  `REFUSED EXPORTS … 100.0% no declaration in any Wine header`, e.g.
  `_Cnd_broadcast`, `_Close_dir`. The signature oracle (clang reading Wine's
  *headers*) can find no declaration, so it refuses rather than guess — correct
  behaviour, but it means the module ships empty.

## Mechanism each family needs (static analysis — for the post-Steam work)

1. **Thread / mutex / condvar / timing C family** — `_Thrd_*`, `_Mtx_lock/unlock/
   destroy_in_situ`, `_Cnd_*`, `_Query_perf_counter/frequency`, `_Xtime_get_ticks`.
   Wine implements every one of these natively in `dlls/msvcp90/misc.c` (e.g.
   `_Cnd_signal` at :896, `_Mtx_lock` at :774, `_Query_perf_counter` at :1629,
   `_Thrd_join` at :1308). They are thin, all-scalar/pointer, representable
   shapes → **trap thunks that marshal to the native implementation**, exactly as
   the task predicted. The catch: **there is no header** — the prototypes and the
   `_Mtx_t`/`_Cnd_t` structs live *inside misc.c* (lines 725/840), and
   `_Mtx_arg_t` is arch-conditional (pointer on win64). So a clean `PROBE-EXTRA`
   is not available; each needs a **per-symbol `.spec`-location downgrade line**
   in `msvcp140.thunks` (the documented mechanism, same as the existing
   `_Mtx_init_in_situ 0x00000E01 2 void msvcp140.spec:3682`), the arity/return
   asserted against misc.c. Almost mechanical, but per-symbol, not a one-line
   probe. (Alternatively: add a real internal header declaring the family and
   PROBE it — a bigger, cleaner change touching msvcp90.)

2. **Error/throw helpers** — `_Xbad_alloc`, `_Xlength_error`, `_Xout_of_range`,
   `_Xbad_function_call`, `_Throw_C_error`, `_Throw_Cpp_error`, `_Syserror_map`.
   Also native in Wine (msvcp90). Same `.spec`-location mechanism. But note they
   *throw* — so they are only correct once the FH3/FH4 catch side (below) works;
   a throw with no working personality faults loudly, which is acceptable.

3. **Mangled C++ methods & the streams/locale/codecvt objects** —
   `basic_streambuf::xsputn/sputc`, `basic_ostream::write`, the `basic_streambuf`
   / `basic_ios` / `ios_base` ctors/dtors, `codecvt::in/out/unshift`,
   `locale`/`_Locimp`. A method that takes `this` + scalars and returns a scalar
   IS trap-thunkable — Wine implements the same MSVC C++ ABI, so the trap marshals
   `this`+args and calls the native msvcp140 method. But the **vtable data exports**
   in this set — `??_7ios_base@std@@6B@`, `??_7?$basic_ios@…@6B@` — are NOT
   functions; they are vtable *pointers* the guest reads and calls through. A trap
   thunk is meaningless for a data export, and handing the guest a vtable full of
   native ppc64 method pointers is precisely the "silent wrong answer" landmine.
   These need real guest-side objects or a guestpe vtable, not an export line —
   **decide per-symbol; the vtables specifically are NOT a mechanical thunk.**

## FH3 / FH4 and Quake II — the one-fix-two-titles question, answered

Read `dlls/vcruntime140/vcruntime140.thunks` (the landmine doc) and
`dlls/guestcrt/{cxxthrow.c,exceptobj.c}`. The situation as of the 2026-08-29
Cyberpunk work:

- **FH4 is already served, and it is the SAME shape Quake II needs.** The tree's
  own note (vcruntime140.thunks, 2026-08-29 block) records that Quake II's
  `game_x64.dll` and Cyberpunk's XeSS **both import `VCRUNTIME140_1.__CxxFrameHandler4`
  and neither imports `__CxxFrameHandler3`.** The prefix-staged `vcruntime140_1.dll`
  is a Wine builtin carrying **real guest-side x86-64 FH4 code** (winedump: exactly
  `__CxxFrameHandler4` + `__NLG_Dispatch2`/`__NLG_Return2`), and the per-thread
  exception state it needs (`__CxxRegisterExceptionObject`,
  `__current_exception`, `__processing_throw`, …) now lives as real guest code in
  `dlls/guestcrt/exceptobj.c`, forwarded through ucrtbase/vcruntime140. `_CxxThrowException`
  and `__C_specific_handler` are now FORWARDs to that guest code, not traps.
  **So the FH4 chain that unblocked Cyberpunk already unblocks Quake II's EH — it
  is the same imports and the same fix; that work is done, not pending.** The
  remaining Quake II gap (if any) is elsewhere, not FH4.

- **Oblivion imports BOTH** `VCRUNTIME140.__CxxFrameHandler3` (DEAD0092) **and**
  `VCRUNTIME140_1.__CxxFrameHandler4` (DEAD0097). Its FH4 is served the same way
  as Quake II (once the prefix has the staged runtimes). Its **FH3 is still a
  deliberate, unserved hole** — nobody has written a guest-side FH3 personality
  (`guest-cxx-eh-plan.md` Session B/C). FH3 and FH4 are different table formats;
  FH4's guest personality does not cover FH3. Whether Oblivion's *boot path* ever
  dispatches through an FH3 frame is unknown and, again, moot until the Steam wall
  falls. (The reason Oblivion shows FH4 as a sentinel at all is that its prefix
  has not had Proton's msvcp140/vcruntime140/vcruntime140_1 staged into
  `sysx8664` — the log's "no x86-64 VC++ 2010 runtime to stage" family — so even
  the served chain is not present in *this* prefix yet.)

**Net:** FH3/FH4 is NOT what to work on for Oblivion right now, and the FH4→Quake II
overlap the task hoped for is real but already banked (2026-08-29). No new EH work
is unblocked by, or unblocks, Oblivion at this wall.

## What I refused to do, and why

- **I did not implement any of the 138 msvcp140 exports.** They are provably off
  the path to Oblivion's death (zero sentinel calls), so implementing them now
  would be exactly the bulk-implementation the task warns against, with no
  measurable effect on this title. When the Steam wall falls, triage in call
  order per the log.
- **I did not fabricate a Steam shared-memory shim.** Backing
  `Local\SteamStart_SharedMemFile`/`_SharedMemLock` well enough to satisfy
  steam_api64 is Steam-bridge subsystem work, it depends on Steamworks-internal
  layout I would be guessing at, and a wrong shim is a silent-wrong-answer of the
  kind this tree refuses. I named the wall precisely instead.

## The next wall, named

To get Oblivion (and any steam_api64-gated Steam title launched via `run-native`)
past startup, the work is in the **Steam bridge**, not the C++ runtime: make the
guest side present a live `Local\SteamStart_SharedMemFile` /
`Local\SteamStart_SharedMemLock` (the Windows-side "Steam is running" liveness
objects) so steam_api64's post-Init probe succeeds. The socket RPC to the helper
already works; only this legacy shared-memory liveness signal is missing. Owner
of `dlls/steamclient64` / `ppc64le/steamapi` should take it — it was already on
the tree's own radar (STATUS.md:644).

Only after that will msvcp140/FH become measurable for Oblivion, and the analysis
above becomes the plan for that pass.
