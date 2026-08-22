# Wine on ppc64le (POWER8/POWER9)

**A fork of [Wine](https://www.winehq.org/) adding a native ppc64le host port.**
Upstream Wine has no PowerPC support; 32-bit PowerPC was removed years ago and
64-bit never existed. This branch adds it.

> **Want to play something?** `PLAYING-GAMES.md` is the practical guide —
> what to build, how to register the compatibility tool with Steam, the
> per-game settings, and what each failure means.  This file is the design
> record.
>
> **Want to work on it?**  `ppc64le/NEXT.md` is the ordered work list and
> where the port stands; `ppc64le/games/STATUS.md` is the per-title board;
> `ppc64le/WORKING-ON-THIS.md` is the operational knowledge that is not
> derivable from the code — which environment knobs actually reach the
> emulator, how to measure a change, and the traps that have each cost a
> session.

## Why

To run Windows software on POWER with **only the guest binary emulated**. Today,
a Windows game on a POWER9 workstation emulates every layer — the game, Wine,
and the graphics translation beneath it. A native Wine means the x86-64
emulator ([fastppcx86](https://github.com/daedalao/fastppcx86)) only has to
handle the application itself.

That matters more on POWER than elsewhere: guest x86-64 is total-store-ordered
and POWER is weakly ordered, so every emulated memory operation pays a barrier
tax that native code does not.

## Status

Honest, and in progress.

| | |
|---|---|
| `configure` and the build system | works |
| winebuild PowerPC64 codegen | **done** — import, delayed-import, relay and stub thunks, ELFv2 |
| Unix-side libraries | `wineserver` and the loader run |
| Windows-side modules | **built as real PEs**, machine `0x01f3`, via `tools/elf2pe` |
| PE-side `ntdll.dll` | stays an ELF builtin — see below |
| `wineboot -u` | **works** — full prefix, services registered, and the prefix runs programs |
| Wine's own test suite | runs **as x86-64 guests** — `mspatcha` 2145/0, `apphelp` 15/0, `advapi32:registry` 6367 executed |
| Running an x86-64 PE | **works** — real Windows programs run to completion |
| Guest imports → native code | **works**, generated at build time by `tools/spec2thunk` — 30+ modules |
| An application's **own** DLLs | **load and run** — not just Wine builtins |
| Guest `DllMain`, C++ static initializers | **run as guest code** |
| Floating point across the boundary | **works** — `probes/check-fp-marshal.sh`, 11/11 |
| Microsoft's real `msvcp140.dll` | **runs as an x86-64 guest module** |
| Guest threads, callbacks, TLS, SEH | **work** — see below |
| D3D12 → native vkd3d-proton | **works**, including **pixels on screen** |
| `vulkan-1` guest thunks | **252 exports, 0 refused** |
| System COM for guests | **works** — `ppc64le/syscom/check-com-smoke.sh`, 21/21, native and guest byte-identical |
| The Steam client for guests | **reaches it** — `ppc64le/steamapi/check-steam-bridge.sh`; a guest gets a real `ISteamClient` and the real client library answers |
| Launching **from** Steam | **has a gate** — `ppc64le/steamtool/check-launch-smoke.sh`, including the `legacycompat` pre-step that used to hang the launch |
| The guest thunk boundary | **lock-free** — the RIP→target cache answers without taking the loader lock |
| Debugging a guest | **works** — `winedbg` attaches, shows the guest's registers, stack, disassembly and backtrace; `ppc64le/winedbg/check-guest-debug.sh` |
| OpenGL on **both** drivers | **works** — X11 and Wayland legs, native and guest byte-identical on each; `ppc64le/opengl/check-gl-smoke.sh` |
| A swapchain on a **child** window | **works** — the child leg of `ppc64le/dxvk/check-present-smoke.sh` |
| Swapchain **resize** | **works** — on screen and in the back buffer; `ppc64le/dxvk/check-fullscreen-smoke.sh` |
| Exclusive **fullscreen** | **works** — the window becomes the screen, the display mode follows, and leaving puts both back; same gate |
| Media Foundation for guests | **works** — measured end to end on `mfplat`/`mf`/`mfreadwrite` |
| Running a **32-bit** PE | **works** — real Wine WoW64, the embedded emulator as the CPU backend; `ppc64le/wow64/check-wow64-smoke.sh` |
| Processor topology | **all cores** — sparse CPU and NUMA numbering, real processor groups; `ppc64le/cpu/check-cpu-topology.sh` |
| `mfmediaengine`, `evr`, `wmvcore` | **surface built, unexercised** — same roster, same instance; no title has driven one, and `ppc64le/mf/README.md` says so |
| **Commercial games** | **three play** — DOOM (2016), Cyberpunk 2077, The Witcher 3; `ppc64le/games/STATUS.md` is the per-title board |
| Graphics for a **32-bit** guest | **not yet** — the i386 half of the dxvk thunk surface builds, but a call traps and nothing answers; `ppc64le/NEXT.md` item 2 |

`ntdll` cannot be a PE and never will be: its TEB lives in an initial-exec
`__thread`, and a PE image has nowhere to put a static TLS block. It is built as
an ELF builtin deliberately, not as a stepping stone.

Nothing here is stubbed silently: anything incomplete is recorded as incomplete
in the design notes.

### Running x86-64 Windows binaries

The native port is only half the point. The other half is that an **x86-64 PE
runs as the main image**, with the x86-64 emulator embedded as a library rather
than the whole environment being emulated.

This does **not** go through WoW64. WoW64 is 32-on-64 by construction, but
`is_machine_64bit(AMD64)` is true, so `WowTebOffset` stays 0 and that path is
never entered. Instead the prefix advertises AMD64 in the server's supported
machines, and `RtlUserThreadStart` hands a guest entry point to the emulator.

A **32-bit** PE is different: that one does go through real WoW64, with the
embedded emulator as the CPU backend behind `BTCpuSimulate`. The server
advertises `IMAGE_FILE_MACHINE_I386`, `wineboot` stages the 32-bit builtins
into `C:\windows\syswow64`, and `ntdll` is itself the CPU DLL. See
`ppc64le/wow64/DESIGN.md`. The emulator bridge must be ABI 4 or no 32-bit
process starts at all.

Guest imports bind to **AMD64 thunk PEs** served from a per-machine system
directory (`C:\windows\sysx8664\`, alongside Wine's existing `syswow64` and
`sysarm32`). Each thunk stub traps into the embedded emulator's callback, which
marshals MS-x64 → ELFv2 and calls the real native ppc64 implementation. Nothing
about the mechanism is specific to `kernel32`.

Those modules are **ordinary build output**. `tools/spec2thunk` generates them
from each module's **own `.spec`** (the `.thunks` file carries `FROM-SPEC` plus
only the overrides a `.spec` cannot express: variadic v-variants, `.spec`-located
names, exclusions) — signatures come from clang parsing Wine's real headers,
never from the `.spec` file, which carries no return type and whose `ARG_LONG`
is 32 bits. Anything unrepresentable is refused at generation time with the
compiler's own reason rather than mis-marshalled; a guest import of a refused
or absent export binds to a per-symbol `0xdead0000+n` sentinel so the fault's
`si_addr` names the symbol that was missing. They are not staged into the
prefix: like every other Wine builtin, they resolve from the build or install
directory — and files copied into `sysx8664` are outranked by the builtin, so
there is exactly one serving path.

The proof it works end to end is Wine's own `winepath.exe`, built as an x86-64
PE and run under the port:

```
$ wine winepath.exe -u 'C:\windows'
/tmp/.../drive_c/windows
```

Byte-identical to what the native ppc64 build of the same program prints. That
run covers a real CRT startup, 42 imports across three modules, and
`GetProcAddress` returning a pointer the guest then calls — the last of which
only works because handles are translated into the caller's own namespace, so a
guest asking for a proc address gets the stub in *its* module rather than a
native ppc64 address it would crash on.

Calls in the **other** direction work too. Native code invoking guest code —
`atexit` destructors, thread start routines, TLS callbacks, and callbacks like
a `qsort` comparator or a progress callback — is intercepted at *registration*
rather than at invocation, because a native caller holding a function pointer
cannot classify it but the thunk that received it knows exactly what it is.
Callbacks needing distinct identities get a trampoline from a pool allocated
outside any guest image, so nothing mistakes a trampoline for guest code. A
callback whose pointer is carried *inside a struct* — a `WNDPROC` in a
`WNDCLASS` — needs the registration to know the struct's shape rather than an
argument's position, and a callback that returns more than 32 bits — a
`WNDPROC` again, returning `LRESULT` — needs the trampoline's return width to
be a property of the slot rather than of the pool.

The gate for that direction is `ppc64le/seh/check-guest-callbacks.sh`: one
guest process that registers window classes both ways, creates message-only
windows (`HWND_MESSAGE`, so it is headless), proves its own `WNDPROC` ran as
guest code for `WM_NCCREATE` and `WM_CREATE`, round-trips two `LRESULT`s with
bits above 31 set, swaps the procedure with `SetWindowLongPtrW` and calls both
forms of `CallWindowProcW` — 21 value-checking steps, a byte-exact transcript,
and a layer that requires the port's own `+seh` trace to show three trampolines,
all three tagged 64-bit return. The same process then asks `CreateThread` for a
16 MiB reserved stack, reads its own TEB from inside the thread, and recurses
past 12 MiB to prove the reserve is not just a number, with a `dwStackSize=0`
thread beside it as the control. `--sabotage` runs both of the port's levers:
`WINEEMUNOCBWRAP=1` (raw guest pointers to native code) and
`WINEEMUNOSTACKSIZE=1` (guest stacks sized from the image again).

Guest **exceptions** dispatch: a fault inside guest code is caught by Wine's own
handler, reconstructed as an `EXCEPTION_RECORD` carrying the guest RIP, and
dispatched to the guest's vectored and TEB-chain handlers, with an unhandled one
re-raised natively. That is what took `advapi32_test:registry` from dying at its
first guest fault to executing 6367 tests.

**Graphics.** A guest D3D12 program reaches native vkd3d-proton and the GPU, and
**presents to the screen** — verified texel-exact by reading the window back on
a live session. Presentation goes through Wine's own win32u client-surface
layer, the one `winevulkan` uses, so vkd3d-proton needs no changes at all. vkd3d
is built by this tree's `make`.

**Graphics, on more than one driver and more than one size.** That
client-surface argument used to be where the Wayland driver's coverage stopped:
"served by construction". It is now run rather than argued, and one of the three
things it was claimed to cover turned out not to be covered by it at all.

* **OpenGL on Wayland is a separate implementation, not the same one served
  twice.** Context creation, pixel formats and buffer swaps belong to the
  *driver* — winex11's GLX and winewayland's EGL — and `win32u`'s client surface
  says nothing about them. `ppc64le/opengl/check-gl-smoke.sh` now runs its whole
  native-vs-guest comparison a second time against a headless weston, and both
  legs pass 15/15 byte-identical. It is also the first **hardware** GL in that
  gate: an Xvfb has no DRI3, so the X11 leg is llvmpipe, while the Wayland leg
  reports `Radeon Pro V620`.
* **A Wine session serves one graphics driver**, and that is a fact anyone
  switching drivers has to know rather than a defect in either. [MEASURED] the
  identical binary in an identical environment: 15/15 in a session of its own,
  and 1/2 — dead at `CreateWindow`, `err:winediag:nodrv_CreateWindow ... The
  explorer process failed to start` — in a session the X11 legs had already
  started. The desktop window is per session and it has one owner. The gate ends
  the session between its two driver legs for that reason.
* **A swapchain on a CHILD window already worked**, which was worth proving
  rather than assuming: a launcher, an in-game UI panel and an embedded video
  view all present into a child of their own frame, and a child HWND is a
  different object to the driver. `check-present-smoke.sh` now creates a 128x96
  `WS_CHILD` inside a 256x256 parent and requires the compositor's own
  framebuffer to hold a rectangle of the CHILD's size — and, against the same
  photograph, requires it *not* to hold one of the parent's, which is the exact
  shape of "presented to the parent instead".
* **Resize works, exclusive fullscreen works, and the display mode changes —
  and the boundary was never the problem in any of the three.**
  `dlls/d3d11/d3d11_marshal.h` already carried complete plans for
  `SetFullscreenState`, `GetFullscreenState`, `ResizeBuffers`, `ResizeTarget`
  and `GetContainingOutput` on every `IDXGISwapChain` version the roster covers
  — ordinary integer slots the generator never refused. What was unproven was
  whether they *did* anything, since a window operation that never reaches the
  display server returns `S_OK` and leaves the picture exactly the size it was.

  `ppc64le/dxvk/check-fullscreen-smoke.sh` drives one swapchain through four
  phases and photographs the screen between them, checking DXGI's own
  description of the back buffer **and** the size of the rectangle on screen —
  because checking only the second would pass a resize that moved the window
  and left DXVK scaling the old buffer into it, which is the commonest way to
  get this wrong and is the gate's first negative control. Resize passes both
  ways: 256x256 → 192x144 → 192x144 again after leaving fullscreen.

  **Fullscreen did not, and the gate is what found it.** [MEASURED] 2026-08-18,
  before the fix: `SetFullscreenState(TRUE)` returned `S_OK`,
  `GetFullscreenState` agreed, `GetSystemMetrics` reported the whole screen —
  and the rectangle on screen was still the window's. The cause was one
  inherited method: `ForeignWsiDriver::enterFullscreenMode` is a deliberate
  no-op that reports success because *the window belongs to somebody else*,
  which is right for the foreign-X11 backend and wrong for the Win32u one,
  whose window is a Wine HWND Wine can move — and `Win32uWsiDriver` inherited
  it unchanged.

  The road it needed did not exist: DXVK's WSI runs in the unix library, and
  the five-entry callback table it reaches Wine through could not move a
  window. That table has **six more entries and an ABI bump** now
  (`ppc64le/dxvk/dxvk_win32u_wsi.h` at abi 2), a Wine half in
  `dlls/d3d11/unix_wsi_window.c` that lands in `NtUserSetWindowPos` and
  `NtUserChangeDisplaySettings`, and twelve overrides in `Win32uWsiDriver`
  (`dxvk-patches/0004-win32u-window-ops.patch`). [MEASURED] the same gate now:
  from 192x144 the window becomes `0,0,1024,768`, `GetFullscreenState` agrees,
  the back buffer follows to 1024x768, the compositor's own framebuffer holds
  524,970 pixels of the cleared colour where it held 27,648, and
  `SetFullscreenState(FALSE)` puts the rectangle, the buffer and the display
  mode back **without the probe touching the window**. Doing it in
  `dlls/d3d11`'s guest-facing shim instead would have been smaller and wrong:
  it would serve guests only, and it would paper over a premise that is false
  one layer up.

  `ChangeDisplaySettingsExW` was recorded as *unproven rather than broken*, and
  it is proven now. [MEASURED] why nobody could see it: `win32u` synthesises the
  mode list for a display whose driver reports one mode, and the smallest entry
  in its table is 640x480 — so on the 640x480 compositor this gate used to
  start, the whole list was three modes that were all 640x480 and no change
  could ever be asked for. At 1024x768 the same code offers four sizes, and a
  second probe build whose swapchain carries
  `DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH` drives a real one: 1024x768 →
  640x480 on the way into fullscreen, and back on the way out.

  Two crashes fell out of doing it, both the port's own and neither in the new
  code. `dlls/win32u/vulkan.c` had **two `pthread_once` controls for one
  initialiser** — upstream's, and the one this fork's HWND-surface seam added —
  so `vulkan_init_once` could run twice, and its second run reset
  `driver_funcs` back to the lazy Vulkan driver after that driver's own
  once-control had already been consumed, leaving every `lazydrv_*` entry point
  calling itself until the stack ran out. Any process that changed a display
  mode and then built a swapchain died on it, in either order; nothing in this
  tree changed a display mode until this gate did. The second was found by the
  gate's own `--sabotage` lever: a mutual recursion between
  `getDesktopCoordinates` and `getCurrentDisplayMode` in the new WSI driver,
  reachable only when Wine publishes the table without the new entries. That is
  the whole argument for a negative control that turns off the **code** rather
  than the gate's own assertions.

  What could **not** be shown on screen is the fullscreen window's POSITION,
  and it is not this port's to show: no Wayland client may place its own
  top-levels. This compositor put the window where it wanted when it was mapped
  and never moved it again, so a screen-sized frame is clipped by the screen's
  edges rather than starting at `0,0`. The gate reads the origin out of a
  windowed phase's own photograph and requires the fullscreen one to be exactly
  the on-screen part of a screen-sized rectangle there, completely filled —
  every number checked, none a tolerance. (Weston's own `weston-fullscreen`
  client does land at `0,0` on the same compositor, so this is a winewayland
  question rather than a DXVK one, and it is recorded rather than fixed.)

### What real games needed

**Three commercial games play**: DOOM (2016), Cyberpunk 2077 and The Witcher 3,
the last two through their own launchers and into live gameplay.
`ppc64le/games/STATUS.md` is the board — every Windows title installed on the
development machine, with the walls each one named in the order it hit them.

That is the method, and it is worth stating because it decided what got built:
one title finds a gap, the second title finds the same gap and proves it was
never per-title. Almost every wall in this record was decidable before anything
ran — an import table and an export table are both just tables, and
`ppc64le/thunks/import_chain.py` walks a binary's whole static chain against
this tree's guest thunk surface and names what would not bind.

What follows is the run of gaps that closing them exposed. They were structural
rather than per-title, and each is worth naming because each will recur:

* the loader refused the *whole* search path for a guest image, so an
  application's own DLLs — `SDL2.dll` sitting beside the `.exe` that imports it
  — were invisible. `open_dll_file()` now demands an exact machine and keeps
  walking the path, so guest modules resolve while a native module still cannot
  be spliced into a guest call;
* a guest `DllMain` was called natively, executing x86-64 bytes as ppc64;
* the C runtime's `_initterm` walks the *caller's* table of static
  initializers, and native code `bctrl`'d straight into guest ones;
* floating point had no path at all. MS-x64 indexes FP argument registers by
  **position**, ELFv2 by **order** — but ELFv2 indexes its GPRs by position
  even so, an FP argument taking its FPR and *skipping* its GPR. Packing the
  integers down instead put `ldexp(double,int)`'s exponent in the wrong
  register, and it returned its input unscaled: a **wrong number, not a
  crash**. `probes/check-fp-marshal.sh` compares against compile-time-known
  answers precisely because that class is invisible to "did it start";
* `LoadLibraryEx` was not intercepted, so a guest got a native `HMODULE` and
  then `NULL` from every `GetProcAddress` on it;
* apisets were not resolved on the runtime `LoadLibrary` path, so every probe
  for an `api-ms-win-*` set answered `NULL`;
* the TEB describes **one** stack, and a thread running guest code has two —
  the native ppc64 one Wine gave it and the guest stack the run loop
  allocates. Guest `__chkstk`, which MSVC puts in front of every function
  with a frame bigger than a page, loads `gs:[0x10]` and touches a byte per
  page from there down to the new RSP; with the *native* bounds in the TEB
  its very first probe landed on the *native* stack's guard page. That is the
  `STATUS_STACK_OVERFLOW` this file used to record as Steam's DRM declining
  to run. The TEB now describes whichever machine is executing — the guest
  stack strictly inside a run, the native stack in every trap out of one —
  which incidentally lets the guest stack **grow**, since Wine's own fault
  handler decides whether a guard-page hit is a stack from those same fields.
  The size is per **thread**, which is not the same as per image. The run loop
  asked for a fixed 8 MiB, which matched no PE at all; then for zero, the way
  every other thread stack in Wine asks for the main image's
  `SizeOfStackReserve`; and now for the size of the stack *this thread* was
  created with. A guest `CreateThread(..., dwStackSize, ...)` is an ordinary
  thunk call into native `kernel32`, so `STACK_SIZE_PARAM_IS_A_RESERVATION`,
  the image defaults, the one-megabyte floor and the granularity rounding have
  all already been applied — to the thread's *native* stack, where the result
  is readable as a pair of addresses in its own TEB. Mirroring that is the
  whole plumbing, and it keeps the arithmetic in the one place that owns it.
  Measured on DOOM: its main thread gets 2097152 bytes, which is its
  `0x200000`, and the worker it prints `Starting stack size in KB: 8388608`
  about gets 8388608 in the same run. (An earlier revision of this file read
  that line as the port being caught out here. It is not: 8388608 is the
  `dwStackSize` DOOM passes to `CreateThread`, printed from its own
  configuration — a separate gap, and now a closed one.) `WINEEMUNOSTACKSIZE=1`
  puts the image back in charge, which is what the gate has to go red against;
* a callback-taking API missing from the interception table is a `bctrl` into
  x86-64 bytes. Interception is at *registration*, so each API is a row, and
  the rows follow what the game list actually registers rather than API
  taxonomy: DOOM handing a `FONTENUMPROC` to `EnumFontFamiliesA` was one, and
  native `gdi32` called it once per font family. The next one could not be a
  row at all. A `WNDPROC` arrives *inside* a `WNDCLASS`, so there is no
  argument position to name it, and DOOM's window procedure reached `user32`'s
  callback dispatcher as a raw guest pointer, was `bctrl`'d into with
  `WM_NCCREATE`, and the `c000001d` that produced was **swallowed** by that
  dispatcher ("ignoring exception c000001d") — leaving a window that would
  never receive a message, and no error at all. Registration interception is
  now struct-aware: `RegisterClass(Ex)A/W` copies the struct and swaps the one
  field, `SetWindowLongPtrA/W(GWLP_WNDPROC)` swaps the value, and
  `CallWindowProcA/W` swaps its first argument — which has to accept all three
  things a guest can legitimately have there: one of our own trampolines, a raw
  guest procedure it never registered, and a `win32u` **winproc handle**
  (`0xffff00nn`), which is not a code pointer at all and which the general
  wrapper would have run as one. A `WNDPROC` also forced the trampoline pool's
  return width to become a per-slot property: it returns `LRESULT`, and the
  pool sign-extended every result from 32 bits — correct for the comparators
  and enumeration callbacks it carried until now, silently wrong for a message
  result carrying a pointer or a handle. Measured on DOOM: `RegisterClassA`,
  then `SetWindowLongPtrA(GWLP_WNDPROC)` — it subclasses its own window — then
  `CallWindowProcA` chaining to the *native* procedure it replaced, which
  passes through untouched.
* **an import does not have to carry a name.** A descriptor may name an
  *ordinal*, and then the number is the whole request. `tools/spec2thunk` wrote
  a `.def` listing names only and let `lld-link` number the table `1..N` in
  **name** order, which is not what a single one of these modules publishes —
  so every module that pins ordinals (`gdiplus`, `shlwapi`, `oleaut32`,
  `user32`, `ws2_32`, `wsock32`, `winspool.drv`, `uxtheme`, `gdi32`, `xinput`,
  `winmm`, `shell32`, `msimg32`, `d3d12`) published the wrong ones to guests
  and nobody could tell, because there was no name in the failure to go on.
  Steam's own `d3ddriverquery64.exe` imports `d3d12.dll` ordinal **101** and
  nothing else — where Microsoft's `d3d12.dll` has always exported
  `D3D12CreateDevice` — bound `0xdead0001`, and died calling it. Ordinals are
  now carried from each module's own `.spec` or `.def` into the thunk's export
  table, numbered exactly the way `tools/winebuild/parser.c`'s
  `assign_ordinals()` numbers the real module (base = the lowest *explicit*
  ordinal, not 1; entries this machine does not export consume nothing;
  everything the generator refuses keeps its number as a **hole**, so a guest
  importing one still gets a named sentinel rather than an unrelated
  function). Replicating that rule was checked against the export tables of
  every module this tree builds: 41,512 named exports, zero disagreements.
  `ppc64le/vkd3d/check-ordinal-imports.sh` is the gate — a guest that imports
  `d3d12` ordinal 101 and requires the pointer to be the *same address*
  `GetProcAddress("D3D12CreateDevice")` returns;
* **a C runtime destroys its per-thread state through an FLS callback**, and
  every MSVC toolchain does it, so this is not per-title either. `FlsAlloc` was
  not in the registration-interception table, so the raw guest pointer went
  into the native FLS slot and native `ntdll` `bctrl`'d into it at process
  exit. `d3ddriverquery64.exe` printed its whole answer and *then* died
  `c000001d` at `0x140002FEC`, which is the middle of an instruction because
  the first two words of UCRT's `__acrt_freefls` happen to decode as valid
  ppc64 and the third does not;
* **a crash reporter that faults while reporting must not restart the
  report.** DOOM's does. On a live Steam launch it called a missing-import
  sentinel, its own top-level filter faulted trying to describe it, and that
  fault was another unhandled guest exception on the same thread — which began
  the whole unhandled report again, ~4.8 KiB of native stack deeper each time,
  804,000 log lines, until the thread's 8 MiB was gone and it spun at 0%
  holding a critical section a second thread was blocked on. The process
  **hung**, and a hang names nothing. One unhandled guest exception is now
  reported per thread: a second one arriving while the first is still being
  reported names both and terminates, and a re-raise entered with less than
  64 KiB of native stack left terminates rather than faulting inside the fault
  handler. The marker is a stack address, not a counter, because a native
  `__EXCEPT_ALL` above the frame can swallow the exception and unwind straight
  past it — there is no epilogue to balance a counter in.

Rather than translate the MSVC C++ ABI, **Microsoft's own `msvcp140.dll` is
loaded as an x86-64 guest module** and runs under the emulator — which only
became possible once application DLLs resolved.

Where the first two titles stopped, and what that cost:

* **Quake II (2023 remaster)** — every DLL initialized and the game reached its
  own code, then dereferenced a global that its one writer never set. Not a
  thunk, sentinel or ABI problem. It has not been re-run since, because the
  library drive it lives on is not mounted on the development machine — so
  whether the gaps closed afterwards moved it is **unknown, not fixed**.
* **DOOM (2016, Vulkan build)** — every import resolves, including all 252
  `vulkan-1` exports. The `STATUS_STACK_OVERFLOW` in `steam_api64.dll` this
  file used to record, and excused as Steam's DRM shim with no client to talk
  to, was **the port's own bug**: the guest `__chkstk` above, probing the
  native stack's guard page. It was never Steam. It now **runs its whole
  engine startup and stops on its own error path**: launched outside Steam,
  `SteamAPI_Init()` cannot find a running client, and DOOM does what it does on
  Windows — prints `[S_API FAIL] SteamAPI_Init() failed`, then `FATAL ERROR:
  Steam failed to initialize`, and throws a C++ exception at its own top-level
  `catch`. That `catch` is a *consolidating* unwind, and it now works (see
  below), so the throw is **caught**, the error path runs to its end, and the
  process stops on the `int3` DOOM's own fatal-error handler executes. The
  whole run is nine lines of log and exit 3. Until that landed, the refusal
  turned the throw into an unhandled exception and DOOM's crash handler looped
  1730 times on `FATAL ERROR: Filesystem call made without initialization`
  before overflowing its stack. **The frontier is now Steam.** Nothing between
  the entry point and `SteamAPI_Init` belongs to this port any more — the two
  swallowed `c000001d`s that used to sit just before it were the window
  procedure, and they are gone too. Launched **from Steam** on 2026-08-17 it
  went past that line for the first time: the guest bound a real
  `ISteamClient017` through `dlls/steamclient64` and the client answered, with
  only the three callback-taking methods refused by name. What it died on next
  was one more missing import — `iphlpapi.GetAdaptersInfo`, reached as
  `0xdead001d` from `DOOMx64vk.exe+0x32a7c2`, because that whole module's
  exports had been refused for want of a header in the signature oracle's
  translation unit and the thunk emitted had none at all. Auditing the binary's
  entire import table against the built thunks rather than waiting for the next
  one found 94 imports in that state across nine modules; adding the headers
  they are declared in leaves **two**, both genuinely unrepresentable and both
  named: `SetConsoleScreenBufferSize` takes a `COORD` and `AlphaBlend` a
  `BLENDFUNCTION`, by value.

  What stood between the two was the thread-naming idiom, and it is worth
  keeping the diagnosis because the fix generalises. Naming a thread on
  Windows means raising `0x406D1388` inside a `__try/__except` that swallows
  it, and the `__except` never ran: the language handler in DOOM's `.xdata` is
  the copy of `__C_specific_handler` its **static** MSVC runtime linked into
  its own `.text` (`DOOMx64vk.exe+0x1eab2c8`, reading
  `DispatcherContext->HandlerData` as a scope table — it is one, byte for byte
  a different one), not the guest `ntdll`'s, which is the only
  `__C_specific_handler` this port can *prove* is one. `steam_api64.dll` names
  `__GSHandlerCheck` the same way. Nothing in a PE says which of
  `__C_specific_handler`, `__GSHandlerCheck` and `__CxxFrameHandler3/4` a
  handler RVA is, and they take incompatible handler data, so accepting one on
  resemblance would mean running an arbitrary address as a filter funclet. The
  port used to refuse, and the game died on an exception Windows discards. It
  now enters the handler **as guest code** instead, which needs no
  identification at all, and serves the cross-boundary `RtlUnwindEx` that such
  a handler calls — see the dispatch section below. Two further stops fell in
  the same run and neither was SEH: the `dbghelp.dll` guest thunk had **no
  exports at all** (every one of its 140 refused as "no declaration in any Wine
  header", so DOOM's crash-reporter setup called `0xdead0017`, the sentinel the
  loader hands a missing import — one `PROBE-EXTRA dbghelp.h` line), and every
  `GetSystemInfo` was answering `PROCESSOR_ARCHITECTURE_PPC64` to a program
  made entirely of x86-64. At the time of that run no `vulkan-1` entry point
  was reached at all: the game stopped before it got that far, on Steam rather
  than on graphics. **It plays now** — guest fibers and two more callback
  classes were the last walls, and `ppc64le/games/STATUS.md` carries the rest
  of that title's record, including the `FATAL ERROR: Memory corruption before
  block!` that was never corruption. Nothing was damaged: swept from outside a
  frozen process, ~19,600 live allocations had zero damaged canaries and all
  13,578 of the game's own blocks still validated. It was `pdh.dll` failing to
  load, and the game freeing a pointer its own allocator never returned.

**System COM** now crosses. `CoCreateInstance` hands a guest a **proxy** whose
vtable is the guest thunk module's own trap-stub array, so a guest calling a
COM method traps into `combase` and reaches Wine's real implementation, with
interface pointers translated in both directions at the boundary. The gate is
`ppc64le/syscom/check-com-smoke.sh`: one source built twice, run as a native
ppc64 PE and as an x86-64 guest, 21 checked steps across `IMoniker`,
`IClassFactory` and `IStream`, byte-identical output — and red under
`WINEEMUNOCOMWRAP=1`, which is the same run with the wrapping turned off.

**Table-based `.pdata` exception dispatch** now works, which is how a
`__try/__except` in any MSVC-compiled x86-64 binary is actually found: nothing
is registered at runtime, so dispatch means walking the guest's exception
directory, virtually unwinding an `AMD64_CONTEXT` frame by frame and asking
each frame's language handler. The unwinder is not new code — Wine already
compiles one architecture's unwinder under suffixed names to serve ARM64EC, and
`unwind.c`'s x86-64 block is now built on ppc64 the same way, against the
guest's `CONTEXT` shape. What is new is the boundary.

**A frame's language handler is entered as guest code.** That is the only
answer that scales, because the question "which handler is this" cannot be
asked of a PE: an image linked against the static MSVC runtime carries its own
`__C_specific_handler`, its own `__GSHandlerCheck` and its own
`__CxxFrameHandler*` in `.text`, and the `.xdata` names those. So the walk
builds a `DISPATCHER_CONTEXT_AMD64` — `ControlPc`, `ImageBase`,
`FunctionEntry`, `EstablisherFrame`, `TargetIp`, `ContextRecord`,
`LanguageHandler`, `HandlerData`, `ScopeIndex`, `HistoryTable`, all of it in
memory the guest can read, because on this port guest memory *is* host memory
— and calls `handler(rec, EstablisherFrame, ctx, dispatch)` through the same
nested-run primitive that already ran TLS callbacks and `DllMain`, honouring
the disposition it returns. The guest `ntdll`'s own `__C_specific_handler` is
still recognised by **address identity** and served natively, with only its
filter and `__finally` **funclets** entered as guest code; both paths are
handed the same `DISPATCHER_CONTEXT` built in the same place, so they cannot
drift apart structurally.

**`RtlUnwindEx` crosses the boundary**, which is what such a handler does the
moment its `__except` accepts. It cannot jump — the frames it would abandon
include the *native* frames of the emulator run that is running the handler —
so the request is recorded and the handler's run is **ended**, and the frame
walk that entered the handler performs the unwind against the faulting stack,
running every guest `__finally` in between as guest code. Guest code that calls
`RtlUnwindEx` while not inside such a handler is unwinding inside its own run
and is served in place. Either way, resuming at the target is a context write
rather than a stack switch: the unwound `AMD64_CONTEXT` gets `Rip` = TargetIp
and `Rax` = the unwind's return value, and the emulator continues from it.

**A C++ `catch` is a consolidating unwind**, and it crosses too. MSVC does not
unwind to a jump target for `catch`: `__CxxFrameHandler` calls `RtlUnwindEx`
with a record whose code is `STATUS_UNWIND_CONSOLIDATE` and whose
`ExceptionInformation[0]` is a *consolidation routine*. The unwind itself is
ordinary — every `__finally` between the throw and the catching frame runs,
which is where destructors live — but the resume is not: the routine is called
once the stack is unwound and the address it **returns** is where execution
continues, TargetIp being ignored. Here that routine is entered as guest code
like every other funclet, and the *whole record* reaches it: Wine's own
`__CxxFrameHandler` fills eleven `ExceptionInformation` slots and a real
`__CxxCallCatchBlock` reads six of them, so a port that carried slot 0 and
dropped the rest would run a catch block against a frame it invented. It runs
on the nested run's own guest stack rather than on the unwound one, which is
sound for exactly the reason the filter and `__finally` funclets are: a funclet
addresses its parent's locals through the establisher frame it is *handed* —
slot 1 — never through the stack it happens to be running on. The cost is named
in the source rather than left to be found: a `throw` that must escape the catch
block searches only as far as that nested run's entry frame. A consolidating
unwind that names **no** routine is still refused, by name; that one has no
right answer, and resuming at the TargetIp it was handed is the plausible wrong
one.

There are two gates, deliberately separate so that each has one owner for its
red state. `ppc64le/seh/check-seh-smoke.sh` covers the identity fast path: 14
value-checking steps over exception codes, `ExceptionInformation`, faulting
addresses, `__finally` call counts and ordering, and a two-frame unwind, plus a
layer that proves the built probe really carries `.pdata` and that its handler
binds to guest code and not to ppc64. `ppc64le/seh/check-seh-handlers.sh` covers
the guest-entered path, on a probe whose `.pdata` provably names a **private**
language handler — a hand-written `.seh_proc` with a `.seh_handler` directive,
because every `__try` clang compiles names `__C_specific_handler` and no flag
changes that. 26 value-checking steps, a byte-exact transcript, and a layer that
re-runs at `+seh` and requires the port's own trace to name the same handler
address the image's `.xdata` names, as many times as the probe counted. Steps 21
to 25 are the consolidating unwind, hand-built to the eleven-slot shape Wine's
own `__CxxFrameHandler` produces and checked field by field by the routine it
was built for — including that the resume came from the routine's return value
and not from the TargetIp, which the probe's frame can prove because it has two
landing pads, and driven down **both** of the port's unwind roads: a handler
asking for the unwind (deferred to the frame walk) and ordinary guest code
asking for it (served in place from the trap context). That lane is hand-built
because it cannot be compiled: clang
`-target x86_64-windows-gnu` gives C++ `try/catch` the `__gxx_personality_seh0`
personality — libstdc++'s, not MSVC's, and it does not use
`STATUS_UNWIND_CONSOLIDATE` at all — and linking one into the probe's `-nostdlib`
image fails on `__cxa_allocate_exception`, `__cxa_throw` and
`__cxa_begin_catch`; `-target x86_64-windows-msvc` emits `__CxxFrameHandler3`
and `_CxxThrowException`, neither of which this tree's guest `msvcrt` thunk
exports. Both measured with the toolchain the gate runs on.

**Known gaps.** System COM is not *finished*: the roster covers 58 interfaces
and 652 slots, but a guest-implemented object passed back into Wine still
needs reverse proxies, and most interface-bearing flat exports are refused
loudly rather than wrapped (105 of `oleaut32`'s 106, for one).
Exception dispatch runs any language handler now, and refuses what is left by
name rather than guessing: **collided unwinds** — an exception raised while an
unwind is already in progress, a handler that starts a second unwind from
inside the first, or one that returns `ExceptionCollidedUnwind` — an exit
unwind, which names no frame to resume in, and a consolidating unwind that
names no consolidation routine. Each terminates with its own message naming the
frame and the handler. A guest exception that must be caught
**below a nested run** — raised inside a guest callback that native code
invoked — is a second, untested limit: the frame walk ends at that run's entry
frame, and the record is re-raised natively where no guest handler can see it.
D3D11 on this path is still unbuilt. Swapchain resize, exclusive fullscreen and
the Wayland driver leg are no longer: see "Graphics, on more than one driver
and more than one size" below.

**`winedbg` can be pointed at a guest.** It could not, and for most of this
file's history every crash here was read off the port's own `+seh` trace, a
disassembler and the exception record's `ExceptionAddress` — which is why the
loader hands out a **distinct** `0xdead0000+n` per unresolved import instead of
one shared `0xdeadbeef`: post mortem, the faulting address was the only name a
symbol had left. Two separate things were wrong.

**The attach never landed, and the reason was an address rather than a
classification.** `DbgUiIssueRemoteBreakin` creates a thread in the target at
the *debugger's own* `DbgUiRemoteBreakin`, which is only meaningful because
`ntdll` normally sits at the same place in every process — it is a PE with a
fixed image base. Here `ntdll`'s PE side is the one module that cannot be a PE,
so it is an ELF builtin and the dynamic linker puts it wherever it likes:
[MEASURED] three concurrent processes of the same binary mapped
`dlls/ntdll/ntdll.dll.so` at `0x3fff881ed000`, `0x3fffb9a1d000` and
`0x3fff91a0d000`. The address handed across named nothing in the target, and
the thread-start classifier said exactly that and refused it — `thread start
00003FFFB7F11280 is in no loaded image; refusing to run it either way`, which
is the correct answer to the question it was asked. `winedbg` printed "attached
to pid" and then waited forever. (An earlier revision of this file read that as
the breakin routine living outside any PE the loader has a record of. It does
not: `ntdll`'s ELF text is inside `ntdll`'s own loader entry and
`RtlPcToFileHeader` finds it.) `DbgUiIssueRemoteBreakin` now resolves the
routine in the **target's** own `ntdll` — same file, same offset, different
base — reading the debuggee's loader data and falling back to today's behaviour
if anything about it fails, which is the identity on every build where `ntdll`
is a PE.

**The registers were the emulator's.** A guest thread's native ppc64 `CONTEXT`
describes the JIT, not the program. The guest register file is reconstructed at
every trap and at every fault, but in a stack frame that is gone by the time
anybody outside asks — and the guest **stack** is freed by the run loop before a
fatal fault is even reported, so a debugger with the registers would still have
had an RSP pointing at unmapped memory. The run loop now keeps a copy of the
guest `AMD64_CONTEXT` in `thread_data` with a state beside it, and serves it
through `NtQueryInformationThread(ThreadWow64Context)` — the information class
that already means "this thread's context in the machine it is really
executing" — over the second server context block this tree had already
reserved for the pair and left empty. A run that ends on an unhandled guest
exception keeps its guest stack mapped **while a debugger is attached**, so the
stack the registers point at is still there when the report is made.

`winedbg` picks its CPU backend per **thread** rather than per process, because
a guest process genuinely has threads of both machines in it — the game's are
x86-64 under the emulator, the debugger's own injected breakin thread and every
Wine service thread are native ppc64. Whichever answers becomes the backend
while that thread is current, and a thread with no guest context says so out
loud instead of being shown zeros. `dbghelp`'s x86-64 stack walker is built on
ppc64 the same way `unwind.c`'s x86-64 block already is, so a guest backtrace is
walked from the guest's own `.pdata`.

What that produces, on a guest that faults three calls deep: the exception
named at the guest RIP, every guest register, a stack dump at the guest RSP,
the faulting instruction disassembled as x86-64, and four guest frames in
order. `ppc64le/winedbg/check-guest-debug.sh` is the gate — a native ppc64
reader that checks ten sentinel registers, CS, the guest RIP and a marker read
out of the guest stack across a process boundary, beside `winedbg` itself
reading the same crash; `--sabotage` turns off the breakin translation, the
context publication and the stack retention in turn and requires each to go red.

**What is still refused, by name.** `set_thread_wow64_context` says no: writing
a guest register means writing into the emulator's own thread state, which is
safe only while the guest is stopped and is not checkable from outside. So a
guest can be **read and not steered** — no guest single-step, no resuming from
an edited RIP, and no breakpoint that needs the context adjusted past it. And a
guest thread that is executing inside the JIT *right now* reports no context at
all rather than the registers it last stopped with, because a debugger printing
a stale RIP as the current one is a wrong number, and this port treats a wrong
number as worse than a refusal.

`AeDebug` remains a hazard for gates rather than a help: every gate except the
debugger's own runs with `WINEDLLOVERRIDES=winedbg.exe=d`, because a red state
that starts a debugger is a **hang** if anything about the attach goes wrong,
and a hang is the one thing a gate must never be.

### Steam

**A guest now reaches the real Steam client.** `dlls/steamclient64` vendors
Proton's `lsteamclient` and splits it across the ISA boundary rather than
reimplementing it, because Proton has already done the hard part: every one of
the ~6500 Steamworks methods across the version matrix is reduced to a flat
params-struct call over a single `WINE_UNIX_CALL`. That boundary is kept and
stretched across a **process**.

The half a game touches is compiled **for x86-64**, unlike every other
guest-facing module here. That is deliberate and it is the whole design
decision. A COM vtable can be served by `libs/winecom`'s trap stubs because it
is a closed shape — three `IUnknown` slots, `HRESULT` returns, an argument
classification small enough to write down per slot. A Steamworks vtable is
not: no `IUnknown`, no IID, no `HRESULT`, and signatures with `float` and
`double` returns, `CSteamID` passed and returned by value, 136-byte
`SteamNetworkingIdentity` arguments that MS-x64 passes by hidden reference, and
hidden-sret returns. Serving those through a dispatcher that can only marshal
integers into `RAX` would mean generating a per-slot MS-x64 marshal descriptor
for every one of them — which is exactly the code a C compiler emits when it
compiles Proton's own PE-side wrappers, because those wrappers **are** the
marshaller. So they are compiled for the machine the game is running on, and
the game-to-`steamclient64.dll` boundary stays x86-64 calling x86-64, as it is
on Windows and under Proton. All 46 PE-side sources compile for
`x86_64-windows-gnu` against this tree's headers; the DLL is ordinary build
output.

The other half runs as an **x86-64 Linux** helper under FEX, because
`~/.steam/sdk64/steamclient.so` is an x86-64 SysV ELF that native ppc64le Wine
cannot load and that the embedded emulator — which runs x86-64 *Windows* code —
cannot host either. It is Proton's unix side unmodified: 219 translation
units, compiled for `x86_64-linux-gnu` against this tree's Wine headers with
zero errors, plus the eleven ntdll entry points a Wine unixlib gets for free
and a standalone program does not.

What is new is the marshalling, and it is generated rather than written:
`tools/steamrpc/gen-steamrpc` reads Proton's own params structs and classifies
every pointer field by its C type and, where the type is not enough, by the
next field's name — the same `count/len/size/num` idiom Proton's generator
asserts on. It emits **C**, so every length is a `sizeof` and every offset an
`offsetof` evaluated by the compiler that also compiles the structs; the
generator never needs to know a layout. **6234 of 6556 methods are bridged and
322 are refused by name** — callback function pointers, game-implemented
callback interfaces, pointer graphs the params struct does not describe, and
three types the SDK only forward-declares. A wrong length here would be a
silent memory bug in someone else's game, so the classifier fails closed.

The helper knows nothing about Steamworks: each frame carries its own pointer
map, so adding an interface version needs no change to it at all.

The gate is `ppc64le/steamapi/check-steam-bridge.sh`. Its second layer is the
one worth naming: a synthetic call exercising every parameter class — in-string,
out buffer with an explicit length, fixed struct by pointer, sized caller
buffer, opaque handle, in-place scalars — with values checked on **both** ends,
including that the caller's own pointers come back unchanged rather than
holding helper addresses. It needs no Steam client, which is the point: a
length or direction that is off by one is invisible in a "did it connect" test.
Measured cost of a round trip: **0.5 ms**, over 2000 calls.

With no client running, the chain produces the answer a Linux game gets —
`CreateSteamPipe` returns 0 — and with no helper at all, `CreateInterface`
returns NULL, which is what lets `steam_api` print its own `SteamAPI_Init()
failed` instead of crashing. Both are asserted, and so is the negative control.

**Not done.** No Steam client has been running on this machine while any of
this was built, so the layer that needs one — appid 480, Spacewar — has been
written and never executed. The callback path (`Steam_BGetCallback`) is
plumbed and untested for the same reason. DOS/unix path translation inside the
helper is refused by name rather than answered, because the helper has no Wine
prefix. And `steam_api64.dll` reads an absolute path out of the registry, which
the guest loader refuses outright, so the prefix's `SteamClientDll64` is a bare
name — that refusal wants relaxing before a real game can be pointed at this.

## The interesting part: r2 across unwound frames

On ppc64le ELFv2, **r2 holds the TOC pointer and every module has its own**, so
any unwind across a module boundary must restore it — and **GCC emits no CFI
rule for r2**. Get it wrong and execution continues silently against the wrong
module's data.

The mechanism, verified over 84 unwind steps against independent ground truth:
the ELFv2 CFA *is* the caller's r1, so the restored frame's TOC save slot is at
`*(cfa + 24)`; whether that slot is live is decided by reading one instruction
at the frame's **resume address** — `ld r2,24(r1)` (`0xE8410018`) means load it,
anything else means leave r2 alone. This is what libgcc, nongnu libunwind and
LLVM libunwind already do, and Wine bundles the LLVM version.

The trap: ppc64's return-address register is **65, which maps to `Lr`, not
`Iar`**. On x86-64 a naive implementation works by accident because the
return-address register *is* the pc register. Reading the pc field gets the
marker from the wrong frame and silently loads stack garbage into r2.

## Building

Build on a ppc64le host. There is no cross-build path.

```
./configure --enable-win64 --enable-archs=ppc64,i386
make -j 64
```

That is the whole thing — the PE modules are produced by `tools/elf2pe`, which
the build drives itself, so no extra toolchain or flag is needed for them.

**`--enable-archs=ppc64,i386` is not optional if you want 32-bit Windows
programs to run.** The i386 lane is real WoW64: Wine's own i386 PE builtins
run under the emulator and convert at the syscall boundary, so they have to be
built. With `--enable-win64` alone `PE_ARCHS` is `ppc64` and those builtins are
simply absent — and because a build tree keeps whatever an earlier configure
left behind, a tree that once had them can appear to work long after it stopped
building them. If `dlls/kernel32/i386-windows/kernel32.dll` is a couple of
megabytes you have them; if it is ~100 KB it is a guest thunk stub and
something is wrong.

After changing `tools/makedep.c`, run `./tools/makedep` yourself. `make`
rebuilds the tool but does not necessarily re-run it, so the generated
`Makefile` can silently keep the old rules and your change appears to do
nothing.

`-j` should match the machine; these are large builds and the developer machine
is a 176-thread AC922. Note that `ninja` — used by some subprojects — does
**not** read `MAKEFLAGS`, so pass it `-j` explicitly or it spawns roughly
core-count+2 jobs and is bounded by RAM rather than cores.

To play a Steam game with what you have just built, follow
`PLAYING-GAMES.md` from section 4.

Then run programs straight out of the build directory, as with upstream Wine:

```
./wine notepad
```

To run an **x86-64** binary, point `WINEFEXBRIDGE` at a built
[fastppcx86](https://github.com/daedalao/fastppcx86) bridge library, which the
loader dlopens on demand:

```
WINEFEXBRIDGE=/path/to/libfexbridge.so ./wine program.exe
```

Without it, native ppc64 Windows binaries still run normally — the emulator is
loaded only when a guest image is actually encountered.

## Design notes

Measured results, adversarial reviews, and the reasoning behind each decision
live outside this tree in the project handbook — including the r2/TOC analysis,
the winebuild codegen record, and a wall-by-wall build log.

## Licence

**Unchanged: LGPL 2.1 or later**, exactly as upstream. See `LICENSE` and
`COPYING.LIB`. This is a fork rather than a re-publication precisely so the
provenance and the modified-source obligation are self-evident.

---

*Upstream Wine's README follows.*

## INTRODUCTION

Wine is a program which allows running Microsoft Windows programs
(including DOS, Windows 3.x, Win32, and Win64 executables) on Unix.
It consists of a program loader which loads and executes a Microsoft
Windows binary, and a library (called Winelib) that implements Windows
API calls using their Unix, X11 or Mac equivalents.  The library may also
be used for porting Windows code into native Unix executables.

Wine is free software, released under the GNU LGPL; see the file
LICENSE for the details.


## QUICK START

From the top-level directory of the Wine source (which contains this file),
run:

```
./configure
make
```

Then either install Wine:

```
make install
```

Or run Wine directly from the build directory:

```
./wine notepad
```

Run programs as `wine program`. For more information and problem
resolution, read the rest of this file, the Wine man page, and
especially the wealth of information found at https://www.winehq.org.


## REQUIREMENTS

To compile and run Wine, you must have one of the following:

- Linux version 2.6.22 or later
- FreeBSD 12.4 or later
- Solaris x86 9 or later
- NetBSD-current
- macOS 10.15 or later

As Wine requires kernel-level thread support to run, only the operating
systems mentioned above are supported.  Other operating systems which
support kernel threads may be supported in the future.

**FreeBSD info**:
  See https://wiki.freebsd.org/Wine for more information.

**Solaris info**:
  You will most likely need to build Wine with the GNU toolchain
  (gcc, gas, etc.). Warning : installing gas does *not* ensure that it
  will be used by gcc. Recompiling gcc after installing gas or
  symlinking cc, as and ld to the gnu tools is said to be necessary.

**NetBSD info**:
  Make sure you have the USER_LDT, SYSVSHM, SYSVSEM, and SYSVMSG options
  turned on in your kernel.

**macOS info**:
  You need Xcode/Xcode Command Line Tools or Apple cctools.

**Supported file systems**:
  Wine should run on most file systems. A few compatibility problems
  have also been reported using files accessed through Samba. Also,
  NTFS does not provide all the file system features needed by some
  applications.  Using a native Unix file system is recommended.

**Basic requirements**:
  You need to have the X11 development include files installed
  (called xorg-dev in Debian and libX11-devel in Red Hat).
  Of course you also need make (most likely GNU make).
  You also need flex version 2.5.33 or later and bison.

**Optional support libraries**:
  Configure will display notices when optional libraries are not found
  on your system. See https://gitlab.winehq.org/wine/wine/-/wikis/Building-Wine
  for hints about the packages you should install. On 64-bit
  platforms, you have to make sure to install the 32-bit versions of
  these libraries.


## COMPILATION

To build Wine, do:

```
./configure
make
```

This will build the program "wine" and numerous support libraries/binaries.
The program "wine" will load and run Windows executables.
The library "libwine" ("Winelib") can be used to compile and link
Windows source code under Unix.

To see compile configuration options, do `./configure --help`.

For more information, see https://gitlab.winehq.org/wine/wine/-/wikis/Building-Wine


## SETUP

Once Wine has been built correctly, you can do `make install`; this
will install the wine executable and libraries, the Wine man page, and
other needed files.

Don't forget to uninstall any conflicting previous Wine installation
first.  Try either `dpkg -r wine` or `rpm -e wine` or `make uninstall`
before installing.

Once installed, you can run the `winecfg` configuration tool. See the
Support area at https://www.winehq.org/ for configuration hints.


## RUNNING PROGRAMS

When invoking Wine, you may specify the entire path to the executable,
or a filename only.

For example, to run Notepad:

```
wine notepad            (using the search Path as specified in
wine notepad.exe         the registry to locate the file)

wine c:\\windows\\notepad.exe      (using DOS filename syntax)

wine ~/.wine/drive_c/windows/notepad.exe  (using Unix filename syntax)

wine notepad.exe readme.txt          (calling program with parameters)
```

Wine is not perfect, so some programs may crash. If that happens you
will get a crash log that you should attach to your report when filing
a bug.


## GETTING MORE INFORMATION

- **WWW**: A great deal of information about Wine is available from WineHQ at
	https://www.winehq.org/ : various Wine Guides, application database,
	bug tracking. This is probably the best starting point.

- **FAQ**: The Wine FAQ is located at https://gitlab.winehq.org/wine/wine/-/wikis/FAQ

- **Wiki**: The Wine Wiki is located at https://gitlab.winehq.org/wine/wine/-/wikis/

- **Gitlab**: Wine development is hosted at https://gitlab.winehq.org

- **Mailing lists**:
	There are several mailing lists for Wine users and developers; see
	https://gitlab.winehq.org/wine/wine/-/wikis/Forums for more
	information.

- **Bugs**: Report bugs to Wine Bugzilla at https://bugs.winehq.org
	Please search the bugzilla database to check whether your
	problem is already known or fixed before posting a bug report.

- **IRC**: Online help is available at channel `#WineHQ` on irc.libera.chat.
