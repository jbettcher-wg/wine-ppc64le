# Wine on ppc64le (POWER8/POWER9)

**A fork of [Wine](https://www.winehq.org/) adding a native ppc64le host port.**
Upstream Wine has no PowerPC support; 32-bit PowerPC was removed years ago and
64-bit never existed. This branch adds it.

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
| **A commercial game** | **no** — see "Where real games stop" |

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
outside any guest image, so nothing mistakes a trampoline for guest code.

Guest **exceptions** dispatch: a fault inside guest code is caught by Wine's own
handler, reconstructed as an `EXCEPTION_RECORD` carrying the guest RIP, and
dispatched to the guest's vectored and TEB-chain handlers, with an unhandled one
re-raised natively. That is what took `advapi32_test:registry` from dying at its
first guest fault to executing 6367 tests.

**Graphics.** A guest D3D12 program reaches native vkd3d-proton and the GPU, and
**presents to the screen** — verified texel-exact by reading the window back on
a live session. Presentation goes through Wine's own win32u client-surface
layer, the one `winevulkan` uses, so both the X11 and Wayland drivers are served
by construction and vkd3d-proton needs no changes at all. vkd3d is built by this
tree's `make`.

### Where real games stop

**No commercial game runs yet.** Two were pointed at the port, and both got far
enough to be useful rather than far enough to play.

Getting there closed a run of gaps that were structural rather than per-title,
and each is worth naming because each will recur:

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
  for an `api-ms-win-*` set answered `NULL`.

Rather than translate the MSVC C++ ABI, **Microsoft's own `msvcp140.dll` is
loaded as an x86-64 guest module** and runs under the emulator — which only
became possible once application DLLs resolved.

Where the two stop today:

* **Quake II (2023 remaster)** — every DLL initializes and the game reaches its
  own code, then dereferences a global that its one writer never set. Not a
  thunk, sentinel or ABI problem.
* **DOOM (2016, Vulkan build)** — every import resolves, including all 252
  `vulkan-1` exports, and it dies in `steam_api64.dll` with `STATUS_STACK_OVERFLOW`.
  That is Steam's DRM shim with no Steam client to talk to, so it is not
  obviously a porting failure at all.

**Known gaps.** System COM is not finished: `CoCreateInstance` still hands a
guest a native vtable, so ole32-dependent tests reach `main` and stop there.
Table-based `.pdata` exception dispatch is not implemented (no corpus binary
needs it yet, but real-world binaries will). D3D11 on this path, swapchain
resize and fullscreen, and the Wayland driver leg are all unbuilt.

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
./configure --enable-win64
make -j 64
```

That is the whole thing — the PE modules are produced by `tools/elf2pe`, which
the build drives itself, so no extra toolchain or flag is needed for them.

`-j` should match the machine; these are large builds and the developer machine
is a 176-thread AC922. Note that `ninja` — used by some subprojects — does
**not** read `MAKEFLAGS`, so pass it `-j` explicitly or it spawns roughly
core-count+2 jobs and is bounded by RAM rather than cores.

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
