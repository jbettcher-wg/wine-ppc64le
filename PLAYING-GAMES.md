# Playing a Steam game on ppc64le

This is the practical guide: what to install, what to click, and what to do
when a game does not start.  It assumes nothing about the port's internals.
If you want to know *why* any of it is shaped this way, `README.md` and the
`ppc64le/*/README.md` files are the design record; this file is the recipe.

**What this port changes.** Steam itself is an x86-64 program and stays one —
it runs under FEX, emulated, exactly as it did before.  What changes is what
runs the *game*: instead of an emulated Wine inside an emulated container,
Steam hands the game to a native ppc64le Wine, and only the game's own x86-64
code is emulated.  Graphics, audio, filesystem, sockets and the Steam client
API are native the whole way down.

---

## 1. What you need first

| | |
|---|---|
| Machine | ppc64le, POWER8 or POWER9 |
| Kernel page size | **4 KiB** — check with `getconf PAGESIZE`, it must print `4096` |
| GPU | anything with a working Vulkan driver (`vulkaninfo` runs); DXVK and vkd3d-proton render through it |
| Steam | an ordinary x86-64 Steam client already running under FEX via `binfmt_misc` |
| Disk | a Steam library the account running Steam can read |

**The 4 KiB page size is not negotiable.**  A PE image's sections are laid out
on 4 KiB boundaries and a 64 KiB-page kernel cannot map them.  Many distro
kernels for POWER ship 64 KiB pages; if `getconf PAGESIZE` says `65536`, nothing
below will work and the failure will look like random loader errors rather than
like a page-size problem.

This guide does **not** cover getting Steam itself running under FEX — that is
the same setup an emulated Proton stack needs, and it has to be working before
this port has anything to attach to.

---

## 2. Build the port

On the ppc64le machine.  There is no cross-build path.

```sh
./configure --enable-win64 --enable-archs=ppc64,i386
make -j32
```

Notes that cost people time:

* `--enable-archs=ppc64,i386` builds the 32-bit lane as well.  Leave `i386` out
  and 64-bit games still work, but Steam's own `SteamService.exe` pre-step is a
  32-bit binary — see the bridge section below.
* Match `-j` to the machine, but do not simply use the thread count: these are
  large links and two of them at once can exhaust RAM on a shared box.
* `ninja`, used by some subprojects, ignores `MAKEFLAGS`.  Pass it `-j`
  explicitly or it spawns core-count+2 jobs.
* **Adding a source file needs `./configure` re-run**, or the file is silently
  absent from the generated Makefile.

You do not need to `make install`.  Everything below runs the tree in place.

---

## 3. Build the emulator bridge

The port loads a bridge library, `libfexbridge.so`, to execute the guest's
x86-64 code.  It comes from the [fastppcx86](https://github.com/daedalao/fastppcx86)
tree — a FEX fork — and it must be built with **clang**; that build rejects GCC
outright.  Only the one target is needed, not the whole emulator:

```sh
cd /path/to/fastppcx86
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release \
      -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++
ninja -C build fexbridge
```

The result is `build/Source/Tools/FexBridge/libfexbridge.so`.  It links against
the system `libfmt` and `libxxhash`, so those have to be installed.

**Check it can start 32-bit processes:**

```sh
grep -c fexbridge_process_init32 build/Source/Tools/FexBridge/libfexbridge.so
```

If that prints `0`, the bridge is an older ABI that cannot run a 32-bit guest
at all, and the way it fails is nasty rather than obvious: Steam runs a 32-bit
`legacycompat/SteamService.exe` before your game, that process dies, and
`shell32` puts up a modal **"Bad format."** box in the middle of the launch that
nobody can answer.  A prefix booted with such a bridge also ends up with an
empty `syswow64`, and a later `wineboot -u` will **not** repair it — it compares
the prefix's `.update-timestamp` against `wine.inf`, decides the prefix is
current, and runs no install pass at all.  Delete the prefix and let the tool
make a new one.

The compatibility tool finds a bridge by itself, in this order: whatever
`WINE_PPC64LE_FEXBRIDGE` names, then the bridge beside the FEX registered in
`binfmt_misc`, then the one in the tree next door.  A candidate that can do
32-bit wins regardless of order.

---

## 4. Install the compatibility tool

```sh
./ppc64le/steamtool/install.sh
```

That symlinks `ppc64le/steamtool` into
`~/.local/share/Steam/compatibilitytools.d/wine-ppc64le-native`.  Use
`--copy` instead if the account Steam runs as cannot read your build tree, and
`--uninstall` to remove it.  A symlink is worth preferring: it means a rebuilt
tree is picked up with no reinstall.

Then, because **Steam only scans `compatibilitytools.d` at startup**:

1. Restart the Steam client.
2. Right-click the game → **Properties → Compatibility**.
3. Tick **Force the use of a specific Steam Play compatibility tool**.
4. Choose **Wine ppc64le (native)**.

---

## 5. Press Play

The first launch of a game creates its prefix and runs `wineboot`, which takes
a minute or two and looks like nothing is happening.  Later launches skip it.

**Where things are.**  Everything the tool does is written to a log under the
game's compat directory:

```
~/.local/share/Steam/steamapps/compatdata/<appid>/wine-ppc64le-native-<date>-<pid>.log
```

(or under whichever library the game is installed in).  The `<appid>` is the
number in the game's store URL.

Two things about that log surprise everyone once:

* **Steam runs the tool twice per launch** — once with the verb `run` for a
  background pre-step, then with `waitforexitandrun` for the game itself.  Two
  logs per launch is normal; it is not two launches.
* The last line is `game exited rc=<n>`.  `rc=0` is a clean exit.  Anything
  else, read upwards.

The prefix is `compatdata/<appid>/pfx`, or `pfx-ppc64le-native` beside it if
something else — usually Proton — already owns `pfx`.  **The tool never touches
a prefix it did not create**, and refuses `destroyprefix` on one, so an existing
Proton prefix for the same game is safe.

---

## 6. Per-game settings

Two ways, and they compose: a launch option wins over the file.

**Steam launch options** (Properties → General → Launch Options).  The tool's
environment filter is a strip-*list* — it removes only variables that belong to
the Steam runtime or to another Wine — so anything you set here survives:

```
MANGOHUD=1 %command%
```

**A settings file in the tool**, `ppc64le/steamtool/appconfig/<appid>.env`, one
`NAME=VALUE` per line, `#` for comments.  It is applied only on the verb that
launches the game, and only where you have not already set the variable
yourself.  The one shipped for DOOM (2016), appid 379720, looks like this:

```sh
# Steam's launch config runs the OpenGL DOOMx64.exe; this port serves the
# Vulkan binary.
WINE_PPC64LE_RUN_EXE=DOOMx64vk.exe

# Bind the game to one NUMA node.
WINE_PPC64LE_NUMA_NODE=0
```

### The knobs

| variable | what it does |
|---|---|
| `WINE_PPC64LE_RUN_EXE` | Run a different binary from the one Steam's launch config picked.  A bare name resolves beside that binary; an absolute path is taken as-is.  Refused loudly if the file is not there. |
| `WINE_PPC64LE_NUMA_NODE` | Bind the game to one NUMA node — see below.  `off` (the default) leaves placement to Linux. |
| `WINE_PPC64LE_NUMA_MEM` | How strict the memory half of that is: `membind` (default), `preferred`, or `none` for CPUs only. |
| `WINE_PPC64LE_FEXBRIDGE` | Use this bridge library, whatever else is on the machine. |
| `WINE_PPC64LE_NO_STEAM_BRIDGE` | `1` disables the helper that lets the game reach the real Steam client.  Games that need Steam will then fail their own way, which is sometimes what you want while debugging. |
| `WINE_PPC64LE_KEEP_FEX_ENV` | `1` keeps the outer `FEX_*` variables instead of letting the inner emulator start from its own defaults. |
| `WINE_PPC64LE_NO_PREREQ_SEED` | `1` stops the tool claiming .NET 4 is present in a new prefix.  Off by default because a game's own prerequisite installer otherwise fails its .NET stage and never starts the game. |
| `WINE_PPC64LE_NO_DIALOGS` | `0` restores message boxes on the pre-step, which are suppressed by default; the game's own boxes are never suppressed. |
| `WINE_PPC64LE_TREE` | Point the tool at a different build tree. |
| `WINEDEBUG` | Wine's own channels, e.g. `WINEDEBUG=+seh`.  Useful and *enormous* — a full `+seh` trace of one game launch can be tens of gigabytes. |

### NUMA binding, and why you might want it

A POWER8 or POWER9 box is usually two or more NUMA nodes.  Run `numactl
--hardware`: if the `node distances` matrix shows something like `10` local and
`40` remote, a memory access on the wrong node costs roughly four times as much
as one on the right node.

By default this port does not confine a game — an unrestricted affinity mask
means every processor on the machine, so Linux spreads the game's threads over
every node.  Measured on DOOM (2016) here: 38 threads on one node and 35 on the
other, with 3.5 GB of the game split 2.1/1.4 between them.  Windows would have
kept that process inside a single processor group.

Whether confining it helps is a property of the game and the machine, so it is a
switch rather than a default.  Set `WINE_PPC64LE_NUMA_NODE=0` and the log will
say so:

```
[wine-ppc64le-native] NUMA: numactl --cpunodebind=0 --membind=0
```

To see what you got, with the game running:

```sh
pid=$(pgrep -x -f /path/to/Game.exe)
ps -L -o psr=,pcpu= -p $pid   # which CPU each thread is on
numastat -p $pid              # where its memory is
```

Compare frame times with and without.  If it is worse, set the value to `off`.

---

### When a game installs its own prerequisites

Unreal Engine titles run `UE4PrereqSetup_x64.exe` from their launcher, and
Steam runs redistributable installers from a game's install script.  You will
see those windows before the game, and that is normal — they run under this
port and their Visual C++ stages work: Boltgun installed the 2015-2019 x86 and
x64 redistributables into its own prefix and recorded them in the registry.

The **.NET Framework 4** stage cannot work: its MSI refuses the platform (error
**1633**), the chain reports failure, and the launcher exits without starting
the game.  On Windows that stage is skipped, because .NET is part of the
operating system.  So the tool makes a new prefix look the same way — it writes
the .NET 4 detection keys and the installer skips the stage.  That is a claim
made to installers, not an implementation: managed code still will not run, and
a game that genuinely executes .NET assemblies needs wine-mono in its prefix.

## 7. Before running a title for the first time

Most first failures are decidable without running anything, because an import
table and an export table are both just tables.  Point the static audit at the
game's binary:

```sh
python3 ppc64le/thunks/import_chain.py --src . --build . \
    "/path/to/steamapps/common/Game/Game.exe"
```

It walks the whole static import chain — the game, and every DLL it ships —
against this tree's guest thunk surface, and names every import that would not
bind.  For the two titles here that had never been run, its answer and the run's
answer were the same list in the same order.  It costs seconds; a run costs
minutes.

---

## 8. When it does not work

The port's own `err:` lines in the tool log are the instrument.  Read those
first — and specifically **do not trust `winedbg` or the game's own crash
reporter for addresses** on this port: winedbg decodes ppc64 as x86-64, and
several games write crash reports with an all-zero register block.

| what you see | what it means | what to do |
|---|---|---|
| The process dies instantly, log has `c0000135` | A whole DLL the game imports has no guest thunk.  Fatal before any of the game's code runs. | Run the import audit above; the module needs a `.thunks` file.  Open an issue with the audit's output. |
| `0xDEAD00nn` in a crash, or "not defined" | A single missing *export*.  The loader binds a per-symbol sentinel; the game dies only if it actually calls it. | Same audit — it names the export. |
| Modal **"Bad format."** during launch | 32-bit could not start: bridge without `fexbridge_process_init32`, or a tree built without the `i386` arch. | Section 3. |
| `err:seh:...` followed by `rc=3` | A genuine port bug in the game's path. | Report the log; the `err:` lines carry real values. |
| `NATIVE ppc64 execution has branched INTO GUEST CODE` | A native module was handed one of the game's function pointers and called it directly — a missing wrapping row. | Report it **with the `called from lr=...` line**: that names the exact call site, and the fix is usually one table entry. |
| `guest called through a wild pointer` | The game called through a pointer that is in no image — usually a `GetProcAddress` that returned NULL earlier. | The message names the caller; look upstream for the failed lookup. |
| A **UE4 Prerequisites** or Steam redistributable installer appears instead of the game, then it exits (`rc=44`) | The chain reached its **.NET Framework 4** stage, whose MSI refuses the platform — `Install_I_Silent_Error`, result **1633**.  The Visual C++ stages of the same installer do work. | Nothing to do: the tool seeds the .NET detection keys into a new prefix so the stage is skipped, exactly as it is skipped on Windows where .NET is part of the OS.  For a prefix made before that existed, delete it and launch again, or set the keys yourself.  `WINE_PPC64LE_NO_PREREQ_SEED=1` turns the seeding off. |
| Game runs but is slow, log full of `Pump was not called` | The known concurrency ceiling: this port is latency-bound at roughly 2.5 cores today. | Try the NUMA binding above.  Otherwise it is a known limit, not your setup. |
| Nothing on screen, no error | Check `vulkaninfo` works for the user Steam runs as, and that the game is not picking an OpenGL binary (`WINE_PPC64LE_RUN_EXE`). |

Two rules that will save you a bad evening:

* **Never point this at `~/.wine`.**  Use the prefix the tool makes.
* When killing a stuck game, kill it **by exact pid**.  A `pkill -f` on a game
  path also matches Steam's own launcher chain — `reaper`,
  `steam-runtime-launch-client`, the tool script — because the path is in their
  arguments, and you will take out more than you meant to.

---

## 9. What actually runs today

`ppc64le/corpus/CATALOG.md` is the honest status board: every Windows game
tested here, in the order each one hits its wall, with what was fixed and what
is still open.  Read it before assuming a title should work.

Known-not-working classes, so you do not waste an evening:

* **Kernel-level anti-cheat** (EAC, BattlEye) — no.
* **32-bit-only titles** run through Wine's WoW64 with the emulator as the CPU
  backend, and need an ABI-4 bridge and an `i386` build.
* Titles whose DRM prologue depends on Windows-only behaviour beyond the
  SteamStub family have not been surveyed.

## 10. Reporting something

The useful bug report for this port is small: the tool log (it is a few
thousand lines, not a trace), the output of the import audit for the title, and
`getconf PAGESIZE` plus `numactl --hardware` for the machine.  If the failure
produced any of the port's own `err:seh:` lines, those alone usually identify
it.
