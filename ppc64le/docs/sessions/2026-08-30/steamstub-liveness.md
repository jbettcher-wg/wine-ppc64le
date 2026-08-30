# The SteamStub liveness wall — what actually kills Oblivion Remastered and Frostpunk 2

**Measured 2026-08-30 on the AC922.** One mechanism, two titles.

This supersedes the "next wall" section of
[`../2026-08-29/ue5-msvcp140.md`](../2026-08-29/ue5-msvcp140.md), which named the
right two objects for the wrong reason and attributed them to the wrong caller.

## The corrections first

That write-up is right that Oblivion dies at rc=51 (exit `0x33`) long before any
of the 138 `MSVCP140` sentinels is touched, and right that
`Local\SteamStart_SharedMemFile` / `Local\SteamStart_SharedMemLock` are what is
missing. Two things in it are wrong and cost the next reader time:

* **It attributes the probe to `steam_api64.dll`.** It does not come from there.
  Neither title's `steam_api64.dll` imports `OpenEventA` or `OpenFileMappingA` —
  not Oblivion's v1.53 copy, not Frostpunk 2's v1.57 copy. Their whole
  KERNEL32 import list (87 names each) contains `OpenProcess`, `CreateEventW`,
  `SetEvent`, `ResetEvent`, `TerminateProcess` and no named-object *open* at
  all. Nor does either game's shipping `.exe` import them. Nor does any DLL
  anywhere in either install: a scan of every PE in the Oblivion tree finds
  `OpenEventW`/`OpenFileMappingW` only in `libcef.dll` and
  `CrashReportClient.exe`, neither of which is loaded here, and the ANSI forms
  nowhere.
* **It says `SteamAPI_Init` fails.** The observable evidence is that it gets as
  far as `[S_API] SteamAPI_Init(): Loaded 'steamclient64.dll' OK.` and the
  bridge RPC answers. Whether it ultimately returned true was never measured
  and is not what kills the process.

## What is actually there: SteamStub

Both shipping executables carry a **`.bind` section, and their PE entry point is
inside it**. That is Valve's SteamStub wrapper — the stub runs first, and jumps
to the real OEP only if it is satisfied.

    Oblivion Remastered-Win64-Shipping.exe   imagebase 0x140000000
        .bind  va=0x09de5000  vsz=0x38410   EP rva=0x09de5310  (EP = .bind + 0x310)
    Frostpunk2-Win64-Shipping.exe            imagebase 0x140000000
        .bind  va=0x0ae8e000  vsz=0x39248   EP rva=0x0ae8e310  (EP = .bind + 0x310)

Same stub, same entry offset into it, sizes 0xe38 bytes apart. `Frostpunk2.exe`
in the game root is a plain launcher shim with no `.bind` — the stub is on the
shipping binary, which is what a launch actually runs.

The stub self-decrypts: disassembling from the entry point gives a
register-save trampoline, a `sub rdx, imm / mov edx,[rdx] / xor edx, imm32`
relocation-decode helper, and then a byte-copy loop into a stack buffer. Its
strings are not in the file; it builds them onto its own stack at run time.
That is why nothing imports the APIs it calls and why `strings` finds none of
the names in the game binaries — and it is why the earlier grep for
`SteamStart_SharedMem` across the tree and the game legitimately found nothing.

## The probe, from the trace

From `~/.local/share/wine-ppc64le/oblivion/wine-ppc64le-native-20260830-090730-1379865.log`
(`WINEDEBUG=warn+module,+seh`), reading the `find_guest_thunk_target` /
`emu_trap_dispatch` pairs. `0x3FFFFDAF0000` is the guest `steamclient64.dll`
(it is the module whose entry point runs at :1157 and whose `DllMain` does the
`RegCreateKeyExW`/`RegSetValueExW`); `0x3FFFFFB40000` is the guest `USER32.dll`.

    GetModuleHandleExA( 0x6 /* UNCHANGED_REFCOUNT|FROM_ADDRESS */, 0x3FFFFDAF1950, out )
    GetModuleFileNameA( 0x3FFFFDAF0000, buf, 0x400 )
    CreateFileW( buf, GENERIC_READ, FILE_SHARE_READ, ... )
    LoadLibraryA( <stack string> )
    GetProcAddress( ... )
    OpenEventA( 0x00100000 /* SYNCHRONIZE */,  FALSE, <stack string @ +0x1F> )
    OpenFileMappingA( 0x2 /* FILE_MAP_WRITE */, FALSE, <stack string> )
    LoadLibraryA( <stack string> )   -> 0x3FFFFFB40000 (USER32)
    GetProcAddress( 0x3FFFFFB40000, <stack string, 12 bytes> )  /* "MessageBoxA" */
    MessageBoxA( NULL, text, caption, 0x10 /* MB_ICONERROR */ )
    TerminateProcess( -1, 0x33 )

The two name pointers are exactly `0x1F` = 31 bytes apart, i.e. one 30-character
string plus its NUL — the length of both `Local\SteamStart_SharedMemLock` and
`Local\SteamStart_SharedMemFile`. The event is opened first, then the mapping,
which is the `_SharedMemLock` / `_SharedMemFile` pair in that order.

**`MapViewOfFile` is never called.** The stub opens the mapping and stops. So
nothing is known about what it would read, and nothing here pretends to.

## Where the names come from

Not from a web page and not from an emulator. They are in the Steam install on
this machine, in Valve's own Windows client library:

    $ strings ~/.local/share/Steam/legacycompat/steamclient.dll | grep SharedMem
    %s%s_SharedMemFile
    %s%s_SharedMemLock
    Local\SteamStart_SharedMemFile
    Local\SteamStart_SharedMemLock
    ...CSharedMemStream::Put() / m_pSharedMemDRM->m_nLockState / k_EMasterStateAvailable...

`CSharedMemStream` builds the pair from a prefix (`Local\`) and a base name
(`SteamStart`). On Windows `steam.exe` publishes them; under Proton, Proton's
`steam.exe` stands in the same place.

## Why no amount of "start Steam first" fixes it

The Steam client on this box is **Linux Steam under `fastppcx86`**. It is not a
Windows process, it is not in any Wine prefix, and it publishes no Win32 named
objects anywhere. There is no configuration of a running client that satisfies
this probe. The objects have to be published on the Wine side or not at all.

## What was implemented

`ppc64le/steamapi/presence/steampresence.c` — a guest x86-64 PE, no CRT, built
by `build-presence.sh` with the same `clang -target x86_64-windows-gnu` recipe
`check-steam-bridge.sh` uses for its probe. It creates

    Local\SteamStart_SharedMemLock   event,   manual-reset, initially signalled
    Local\SteamStart_SharedMemFile   section, 0x1000, zeroed

and holds them until it is killed. `--probe` opens the same two objects with the
same access masks in the same order the stub uses, so the mechanism can be
tested without a game.

`ppc64le/steamtool/proton` starts it from `presence_start()`, called at the end
of `bridge_start()` — after the `ActiveProcess` registry writes and before the
game. `presence_stop()` is called both from the EXIT trap and, importantly,
*before* `wineserver -w`: the publisher is a process in the prefix and
`wineserver -w` waits for every one of them.

**A separate process, not `steamclient64.dll`'s `DllMain`.** The stub runs at
the entry point and chooses when to load `steam_api64.dll`, so anything
published from inside the game process is published at an ordering the game
controls. A process the launcher starts first cannot lose that race. It is also
what Proton and Windows both do.

**Gated on the truth of what it says.** `presence_start()` runs only once
`bridge_start()` has the helper up, and the helper only comes up when it has
loaded the user's real, running Steam client library; `steampresence.exe`
independently refuses to publish anything when `STEAM_BRIDGE_ADDR` is unset. No
client, no presence. Nothing speaks to a Valve service, fabricates an account,
a licence or a ticket, or touches the game's own code — and the stub does not
decrypt the game's `.text` (both binaries' imports, strings and section sizes
are plain), so there is nothing here that a licence gates.

**The section is left zeroed on purpose.** No `CSharedMemStream` header is
fabricated. The stub has never been seen to map a view; if a title is ever seen
to `MapViewOfFile` it and then fail, that measurement earns a layout.

### Verified without a game

    $ steampresence.exe --probe          # no publisher
    probe: OpenEventA(SYNCHRONIZE, Local\SteamStart_SharedMemLock) FAILED
      last error 0x2
    probe: OpenFileMappingA(FILE_MAP_WRITE, Local\SteamStart_SharedMemFile) FAILED
      last error 0x2
    probe: ABSENT

    $ steampresence.exe &                # publisher held in another process
    $ steampresence.exe --probe
    probe: OpenEventA(SYNCHRONIZE, Local\SteamStart_SharedMemLock) OK
    probe: OpenFileMappingA(FILE_MAP_WRITE, Local\SteamStart_SharedMemFile) OK
    probe: MapViewOfFile(FILE_MAP_WRITE) OK
    probe: PRESENT

`0x2` is `ERROR_FILE_NOT_FOUND` — what the stub was getting.

## Loose end, recorded before it is measured

`GetModuleFileNameA` on a guest builtin reports `C:\windows\sysx8664\<name>.dll`
(see the `LdrGetProcedureAddress ... in L"C:\windows\sysx8664\ADVAPI32.dll"`
warnings in the same log), and **that directory does not exist in the prefix** —
guest builtins are served straight out of the build tree. So the stub's
`CreateFileW` on `steamclient64.dll`'s reported path fails. In the measured run
the stub carried on to the object probe regardless, so it is not obviously
fatal, but it is the next thing to look at if the objects turn out not to be
enough.

## Who else this is in front of

Scanning every `.exe` under `steamapps/common` for a PE entry point that lands
in a `.bind` section — the SteamStub tell — finds five binaries across four
installed titles:

    Quake 2/quake2.exe
    Frostpunk2/Frostpunk2/Binaries/Win64/Frostpunk2-Win64-Shipping.exe
    Oblivion Remastered/OblivionRemastered/Binaries/Win64/OblivionRemastered-Win64-Shipping.exe
    Sid Meier's Civilization VI/steamassets/base/binaries/win64steam/civilizationvi.exe
    Sid Meier's Civilization VI/steamassets/base/binaries/win64steam/civilizationvi_dx12.exe

So this is not a two-title fix by construction. Two caveats, both measured:

* **Quake 2 is launched here through `rerelease/quake2ex_steam.exe`, which is
  not stubbed**, so the port has never met the stub on that title. `quake2.exe`
  in the game root is.
* **Civ VI is not dying at this wall.** Its recent runs exit rc=0 and rc=5, not
  rc=51, so whatever blocks it (the EOSSDK import work) is somewhere else. The
  stub being present does not mean the stub is the current wall — only that a
  title cannot get past it without these objects.
