# The Steam compatibility tool

`proton` is the script Steam runs instead of Proton. It sets up an environment
for the native ppc64 wine and hands the game to it.

This file is how the tool works. **If you are trying to get a game running,
read `PLAYING-GAMES.md` at the top of the tree instead** — installation, the
Steam settings, the per-game knobs and what each failure means.

Installed by `install.sh` into
`~/.local/share/Steam/compatibilitytools.d/wine-ppc64le-native/`.

## What it does, in order

| step | what |
|---|---|
| bridge | Picks the emulator bridge and exports `WINEFEXBRIDGE`. |
| environment | Strips variables belonging to the Steam runtime and to Proton's wine. |
| prefix | Chooses `$STEAM_COMPAT_DATA_PATH/pfx`, or `pfx-ppc64le-native` if something else already owns `pfx`. |
| first run | `wineboot --init`. |
| 32-bit | If `syswow64` is empty and the bridge can do 32-bit, forces a `wineboot -u`. |
| staging | Copies an x86-64 `msvcp100.dll` into `C:\windows\sysx8664` if one is available. |
| steam bridge | Starts the helper that lets a guest reach the real Steam client. |
| verb | `run` for pre-steps, `waitforexitandrun` for the game. |

## Bridge selection

Order: `WINE_PPC64LE_FEXBRIDGE`, then the bridge beside the binfmt-registered
FEX, then the in-tree build.

A candidate carrying `fexbridge_process_init32` **wins** regardless of order.
A bridge without it cannot start a 32-bit process at all, and the way that
surfaces is not an error about the bridge: Steam's own
`legacycompat/SteamService.exe` is a PE32, so the launch fails
`ERROR_BAD_EXE_FORMAT` and shell32 puts up a modal "Bad format." box in the
middle of somebody's game.

The symbol is looked for with `grep`, not `nm`, because this runs inside the
FEX environment where binutils may not be.

If nothing 32-bit-capable is readable the first readable candidate is still
used, with a warning — a 64-bit-only guest works fine on it.

## Environment

The filter is a strip-**list**; anything not named is kept. So `MANGOHUD=1` and
other launch-option variables pass through untouched.

Stripped: the Steam runtime's loader and driver paths, `FEX_*` (the inner
emulator must start from its own defaults, not the launcher's —
`WINE_PPC64LE_KEEP_FEX_ENV=1` keeps them), and another wine's `WINEPREFIX`,
`WINELOADER`, `WINESERVER`, `WINEDLLPATH`, `WINEARCH`, `WINEESYNC`, `WINEFSYNC`.

## NUMA

`WINE_PPC64LE_NUMA_NODE=<id>` wraps the game in `numactl --cpunodebind=<id>
--membind=<id>`; unset or `off` changes nothing. `WINE_PPC64LE_NUMA_MEM` picks
`membind` (default), `preferred` or `none` (cpus only). The wineserver the game
starts inherits it; the steam bridge helper deliberately does not, being an
x86-64 process under the outer FEX rather than part of the game's working set.

It exists because this port does not confine a guest: `ppc64le/cpu/TOPOLOGY.md`'s
rule is that an unrestricted affinity mask means every processor in every group,
so Linux spreads a game across every NUMA node. [MEASURED] DOOM (2016)
mid-session, unbound: 38 threads on node 0 and 35 on node 8, 3.5 GB split
2.1/1.4. Bound to node 0: 74 threads and 2.0 GB of 2.0 GB on that node.

Whether it helps is a per-title, per-machine question — two nodes is twice the
bandwidth, but this port's ceiling is latency-bound and the test machine's remote distance
is 40 against a local 10 — so it is a lever with no default, set per game in
`appconfig/<appid>.env`.

## Dialogs

Steam inserts `iscriptevaluator.exe` as a `run` pre-step after a compat-tool
change. It shells a 32-bit binary; when that failed, shell32 raised a modal box
nobody could answer and the launch hung.

`WINE_PPC64LE_NO_DIALOGS` answers message boxes automatically — **on the `run`
verb only**. The game keeps every box it raises. Set it to `0` to restore the
dialogs; that is what the gate's negative control does.

## Prerequisite markers

A new prefix gets the .NET 4 detection keys (`NDP\v4\Full` and `\Client`,
plus the Wow6432Node copies) written into it.  A prefix that already answers
those keys -- wine-mono, or a real install -- is left alone, and a stamp file
means it happens once.

Why: a game's own prerequisite installer chains .NET Framework 4, whose MSI
refuses the platform (`Install_I_Silent_Error`, MSI **1633**), and the failure
takes the whole chain with it -- Boltgun's UE4PrereqSetup exits and its launcher
never starts the game, rc=44, with nothing naming .NET as the cause.  The Visual
C++ stages of the same installer succeed and leave their own keys behind.

On Windows the .NET stage is a no-op because .NET is part of the OS.  This makes
the prefix look the same.  It is a claim to installers, not an implementation.
`WINE_PPC64LE_NO_PREREQ_SEED=1` turns it off.

## Prefixes

A prefix Proton already owns is never touched. If `pfx` exists and was not
created by this tool, `pfx-ppc64le-native` is used beside it.

`destroyprefix` refuses to delete a prefix this tool did not create.

## Staged modules

`msvcp100.dll` is copied into `C:\windows\sysx8664` when the app has a Proton
prefix with one. That directory is searched before the builtins, so the staged
copy wins.

It is staged because msvcp100's exports are MSVC-mangled C++ members that this
port does not translate, so the tree's own thunk has no exports at all. The
file that gets staged is Wine's own x86-64 build, not Microsoft's.

`msvcr100` is deliberately **not** staged: it is the half this port can
translate, 766 of 1185 exports cross, and its `.thunks` carries the
callback-registration rows the CRT gate depends on.

## Gate

`check-launch-smoke.sh` — prefix selection, environment stripping, the exe
swap, dialog suppression scoped to `run`, and the bridge helper being stopped
on the way out.

It raises a real modal dialog as its negative control, so it starts its own
Xvfb. Pointed at a live session those boxes land on somebody's desktop and
outlive the run.

## Logs

`$STEAM_COMPAT_DATA_PATH/wine-ppc64le-native-<date>-<pid>.log`. Steam runs the
tool **twice** per launch — `run` then `waitforexitandrun` — so two logs per
launch is normal, not two launches.
