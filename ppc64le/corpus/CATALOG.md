# The corpus

Every Windows game installed on this machine, run against the port, with the
walls each one names written down in the order it hits them.

## Why this file exists

A port is finished when programs run, and programs are the only thing that can
say which gap matters next. One title finds a gap; the second title finds the
same gap and proves it was never per-title. That is the whole method, and it
has a shape worth naming:

**Nearly every wall in this pass was decidable before anything ran.** An import
table and an export table are both just tables. `ppc64le/thunks/import_chain.py`
walks a third-party PE's whole static chain against this tree's guest thunk
surface and names every import that would not bind — and for the two titles
that had never been run here, its static answer and the run's answer were the
same list, in the same order. The audit is the cheap first move on any new
title. Run it before you run the game.

The second thing worth naming is the difference between two failure shapes that
look alike in a log and are nothing alike in consequence:

* **A missing EXPORT is survivable.** The loader binds ntdll's per-symbol
  sentinel, `0xDEAD00nn`, and the guest dies only if it actually *calls* it —
  by name, with the module and symbol in the message.
* **A missing MODULE is fatal, immediately, and before any guest code runs.**
  With no guest thunk the loader finds the *native ppc64* module, correctly
  refuses to splice it into a guest call (`c000007b`), and because an ordinary
  non-delay-load import failure fails the whole image, `loader_init` gives up
  with `c0000135`. The process is gone before its entry point.

Two of the three runnable titles here died the second way, on modules nobody
would have guessed: `Normaliz.dll`, `dxva2.dll`, `faultrep.dll`, `mscoree.dll`.
None of them are graphics, audio, or Steam. They are the ordinary furniture of
a Windows build, and the fix for all fifteen was one `.thunks` file each.

## Status board

| Title | appid | Machine | Before this pass | After |
|---|---|---|---|---|
| Warhammer 40,000: Boltgun | 2005010 | PE32+ | dead in `loader_init`, `c0000135`, 0 guest instructions | past the loader, and now **past the DRM stub** too |
| Styx: Master of Shadows | 242640 | PE32+ | dead in `loader_init`, `c0000135`, 0 guest instructions | past the loader — see below |
| The Elder Scrolls V: Skyrim SE | 489830 | PE32+ | loader completes, then `c0000005` at the image entry | **past the DRM stub** — see the 2026-08-18 re-run below |
| Half-Life 2 | 220 | **PE32** | refused — no 32-bit guest | unchanged, and correctly so |
| FreeInfantry | 2830720 | **PE32** | refused — no 32-bit guest | unchanged, and correctly so |
| Styx: Master of Shadows (Win32) | 242640 | **PE32** | refused — no 32-bit guest | unchanged, and correctly so |
| Styx: Shards of Darkness | — | — | not installed (empty directory, no appmanifest) | — |
| DOOM (2016) | 379720 | PE32+ | owned by the audio/`winecom` work; not touched here | — |

Run recipe used throughout, deliberately **without** `SteamAppId` so that no
account presence is signalled — `steam_api64` failing its init is the expected
wall for these runs, and what is being measured is everything before and
around it:

```sh
env -u DISPLAY \
    STEAM_COMPAT_DATA_PATH=/mnt/caution/steamapps/compatdata/<appid> \
    WINEFEXBRIDGE=$HOME/projects/fex-emu-ppc64le/src/build-smc/Source/Tools/FexBridge/libfexbridge.so \
    WINEDLLOVERRIDES=winedbg.exe=d \
    timeout -k 5 240 ~/.local/share/Steam/compatibilitytools.d/wine-ppc64le-native/proton \
    waitforexitandrun <exe>
```

The compat tool writes the whole run to
`$STEAM_COMPAT_DATA_PATH/wine-ppc64le-native-<ts>-<pid>.log` and uses its own
prefix, `$STEAM_COMPAT_DATA_PATH/pfx-ppc64le-native`; it never touches the
app's own `pfx`. The Steam library itself is read-only and stayed byte-identical.

---

## What this pass changed

Fifteen modules had **no guest thunk at all**. Each now has a `.thunks` file
and a line in its `Makefile.in`'s `SOURCES` — the same one-line fix
`dlls/dbghelp/dbghelp.thunks` and `dlls/iphlpapi/iphlpapi.thunks` already
carry, and for the same reason. Nothing in them is hand-written: the export
list comes from each module's own `.spec`.

| Module | Wanted by | `PROBE-EXTRA` | Emitted / eligible |
|---|---|---|---|
| `d3dcompiler_43` | Boltgun, Styx | — | 11 / 12 |
| `dwmapi` | Boltgun | `dwmapi.h` | 24 / 24 |
| `normaliz` | Boltgun | — | 5 / 5 |
| `dxva2` | Boltgun | `d3d9.h`, `dxva2api.h` | 2 / 37 |
| `uiautomationcore` | Boltgun | `uiautomationcore.h`, `uiautomationcoreapi.h` | 34 / 37 |
| `wldap32` | Boltgun | `winldap.h` | 169 / 244 |
| `xapofx1_5` | Boltgun, Styx | — | 0 / 1 |
| `d3dx11_43` | Styx | `d3dx11.h` | 24 / 25 |
| `psapi` | Styx, its `EasyHook64.dll` | — | 0 / 27 |
| `msvcr100` | Styx + 12 of its own DLLs | `locale.h`, `setjmp.h`, `sys/utime.h`, `sys/timeb.h`, `mbstring.h`, `mtdll.h` | 766 / 1185 |
| `msvcp100` | Styx | — | 0 / 46 |
| `rpcrt4` | Styx's `wxmsw28u_core…dll` | — | 260 / 317 |
| `mscoree` | Styx | — | 2 / 48 |
| `faultrep` | Styx | — | 0 / 14 |
| `msacm32` | Styx's `fmodex64.dll` | `mmreg.h`, `msacm.h` | 42 / 43 |

A module that emits **zero** exports is still the fix: the module *loads*, the
image gets past `loader_init`, and each import binds a sentinel that faults by
name. That is the whole distance between "never started" and "stopped
somewhere you can read".

Ordering matters in the `PROBE-EXTRA` lines and three of these prove it:
`dxva2api.h` names `D3DFORMAT` and needs `d3d9.h` first; `uiautomationcoreapi.h`
names `PROPERTYID` and needs the widl-generated `uiautomationcore.h` first;
`msacm.h` names `PWAVEFILTER` and needs `mmreg.h` first. Without the
predecessor the header does not compile at all and *every* export is refused.

### A finding: the signature oracle resolves a NAME, and headers rename

`tools/spec2thunk`'s oracle looks up the export name a `.spec` publishes and
asks Wine's headers for a declaration *under that name*. A header is entitled
to declare the implementation under a different one, and two modules in this
pass do exactly that — for different reasons, which is what makes it a class
rather than a quirk:

* **`psapi.dll` — all 27 exports.** `include/psapi.h` sets `PSAPI_VERSION 2`
  itself and then `#define`s every one of its 27 names to a `K32*` equivalent
  *before* declaring them. What the translation unit ends up declaring is
  `K32GetProcessMemoryInfo`; the oracle asks for `GetProcessMemoryInfo` and
  finds nothing. Adding `psapi.h` as a `PROBE-EXTRA` changes not one verdict.
* **`wldap32.dll` — 75 of 244, including ten of the sixteen ordinals Boltgun
  imports.** `include/winldap.h` declares `ldap_initA`/`ldap_initW` and
  `#define`s the undecorated `ldap_init` through Wine's own
  `WINELIB_NAME_AW()`, ~120 times over.

Neither is a shape the descriptor cannot express — both are ordinary integer
arguments and integer returns. The fix belongs in the oracle (follow a
declaration the header aliases the export name to), not in fifteen `.thunks`
files asserting signatures by hand. Until then each keeps its ordinal as a
named sentinel, which is the honest failure. **Not filed as a game wall: no
run has called one yet.**

### Honest refusals that are NOT the oracle's fault

Worth separating, because they will not go away when the above is fixed:

* `uiautomationcore.UiaRaiseAutomationPropertyChangedEvent` takes two
  `VARIANT`s **by value** — 16 bytes, wider than the one 64-bit slot this
  descriptor can carry. Named sentinel, by design, exactly like the documented
  `oleaut32.#113`.
* `msvcr100`'s 77 varargs (the printf/scanf family) need the `variadic=`
  column and a per-name v-variant.
* `msvcr100`'s `div`/`ldiv`/`lldiv`/`_cabs` return or take a struct by value —
  the same rule that makes `ucrtbase.ldiv` a documented hole.
* `msvcp100`'s 46 eligible exports are MSVC-mangled C++ members. No Wine
  header declares them as flat prototypes because they are not flat
  prototypes; they are a C++ ABI. See the handoff below.

---

## Per-title ledger

### Warhammer 40,000: Boltgun (2005010)

* **Exe:** `/mnt/caution/steamapps/common/Boltgun/Boltgun/Binaries/Win64/Boltgun-Win64-Shipping.exe`
  (Unreal Engine). The top-level `Warhammer 40,000 Boltgun.exe` is a 243 KB
  launcher shim with 84 imports and zero holes; the shipping exe is the real
  binary and is what was run.
* **Static audit:** 824 imports, 103 holes, **7 modules with no thunk at all**.
* **Before:** 144 modules loaded, then seven `c000007b` module refusals in
  import-table order — `XAPOFX1_5`, `D3DCOMPILER_43`, `WLDAP32`, `Normaliz`,
  `dwmapi`, `UIAutomationCore`, `dxva2` — and
  `err:module:loader_init Importing dlls for L"…Boltgun-Win64-Shipping.exe"
  failed, status c0000135`, exit rc=53. All seven are ordinary static imports
  of the exe itself, none delay-load, so one was enough. No `Saved/Logs`
  directory was ever created: the UE4 engine's own logging never initialises,
  because no engine code runs. Earlier than any wall this port had recorded.
* **The 96 sentinel-bound holes in `msvcp140`/`vcruntime140`/`ucrtbase`/
  `kernel32`/`setupapi` were never called** — the process died first. They are
  the *next* run's question, not this one's.

**After — the loader is gone as a wall.** Zero `c000007b` in the run. All
seven modules load, 40 modules initialise, every `DllMain` runs, and the
process reaches its own entry point at `0x14542F310` and executes it. It then
dies inside the **Steam DRM stub** — the same bug as Skyrim, root-caused
below; see *One bug, two titles* — with

    err:seh:RtlUserThreadStart failed to emulate AMD64 entry point 000000014542F310, status c0000005

exit rc=5. The distance travelled is the whole loader plus all of DLL
initialisation: from "no guest instruction ever ran" to "the image's first
instruction sequence ran and got four API calls into the DRM preamble".

**What Boltgun imports from those seven modules, and what is now real.** 29
exports in total; 17 are emitted and 12 are named sentinels:

| Module | imported | served | still a sentinel |
|---|---|---|---|
| `dwmapi` | 4 | 4 | — |
| `normaliz` | 1 | 1 | — |
| `dxva2` | 1 | 1 | — |
| `d3dcompiler_43` | 1 | 1 | — |
| `uiautomationcore` | 5 | 4 | `UiaRaiseAutomationPropertyChangedEvent` (VARIANT by value) |
| `wldap32` | 16 | 6 | 10, all of them the A/W macro-rename above |
| `xapofx1_5` | 1 | 0 | `CreateFX` (no Wine header declares it) |

**Not one of the twelve has been called**, because the DRM stub stops the
process before any of this code runs. That is why none of them was promoted
with a `.spec`-location override: on this project's rule a downgrade is earned
by a measured call, not by anticipation.

### Styx: Master of Shadows (242640)

* **Exe:** `/mnt/caution/steamapps/common/Styx/Binaries/Win64/StyxGame.exe`
  (Unreal Engine 3, 59 MB).
* **Static audit:** 13,993 imports across 23 modules, 42 holes, **10 modules
  with no thunk at all**.
* **Before:** 30 modules loaded — including six of the game's own guest x86-64
  DLLs running as real guest code under the emulator (`steam_api64.dll`,
  `binkw64.dll`, `ApexFrameworkCHECKED_x64.dll`, `libresample_x64.dll`,
  `tbbmalloc.dll`) — then the import walk hit all ten missing modules and
  `loader_init` gave up with `c0000135`, exit rc=53. No `StyxGame/Logs/*.log`
  was ever created: UE3's own log never opens.
* **Every one of the ten was load-bearing.** Eight are direct imports of the
  exe (`D3DCOMPILER_43`, `d3dx11_43`, `XAPOFX1_5`, `MSVCP100`, `MSVCR100`,
  `PSAPI`, `faultrep`, `mscoree`); `RPCRT4` arrives through the bundled
  `wxmsw28u_core_vc_custom_64.dll` and `MSACM32` through `fmodex64.dll`.
  Nothing could be skipped — a failed import of the main image is fatal
  wholesale, not per-feature.
* **Three named-export holes were reached and bound to sentinels** during that
  walk (imported, not yet called): `KERNEL32.SetConsoleScreenBufferSize`
  (takes a `COORD` by value — a real shape refusal), `d3dx9_43.D3DXUVAtlasPack`
  and `D3DXUVAtlasPartition`, and `WININET.InternetSetStatusCallbackW`. The
  last is worth flagging: it takes an `INTERNET_STATUS_CALLBACK`, so if Styx
  ever *calls* it the answer is registration interception (the `FlsAlloc` /
  `WNDPROC` treatment in README), not a plain thunk.
* **The 15 `msvcrt.dll` holes the audit named are irrelevant to this title.**
  `msvcrt.dll` never appears in the run at all: Styx's CRT is `MSVCR100`. The
  operator-new/delete, `_CxxThrowException`, `__CxxFrameHandler` and the data
  exports `__badioinfo`/`__pioinfo`/`_iob`/`__mb_cur_max` are a different
  title's problem. (They remain a genuine design item when one arrives: a trap
  stub cannot serve a *data* export, because there is no call to trap on.)

**After — three walls knocked down in sequence, each one revealing the next.**
This is the flywheel working exactly as intended, and each step was measured
rather than predicted:

1. **The loader.** Zero `c000007b`. Modules loaded went 30 -> 59. The image
   gets past `loader_init` for the first time and begins `PROCESS_ATTACH`.
2. **`MSVCR100._initterm_e` `bctrl`'d into guest code.** Its own bundled
   `wxmsw28u_vc_custom_64.dll` reached `DllMain`, called `_initterm_e` with an
   initializer table at `0x1001CCBC0..0x1001CCBD0`, and *native* msvcr100
   walked it and jumped into the first entry:

       dispatch_exception code=c0000005 addr=00000001001BE2E4
         iar=00000001001be2e4  ctr=00000001001be2e4  lr=00003fffff1cc900

   `ctr` and `iar` are the same **guest** address; `lr` is inside native
   `msvcr100._initterm_e`. This is precisely the `_initterm` failure README
   already records for `msvcrt` and `ucrtbase` — in the one C runtime that had
   never had a guest thunk, and so had never been given the same treatment.
   **Fixed**: `GUEST-IMPL _initterm/_initterm_e -> __wine_guest__initterm{,_e}`
   in `msvcr100.thunks`, plus the two `-arch=ppc64` forwards in
   `msvcr100.spec` that `msvcrt.spec` and `ucrtbase.spec` already carry.
3. **`MSVCR100._malloc_crt` sentinel, called.** With the initializers running
   as guest code, the next thing that happened was the port's own designed,
   legible failure:

       err:seh:dispatch_guest_exception guest called through a wild pointer:
         00000000DEAD0008 is in no guest image; the call was made from
         00000001001BE2F5 = L"wxmsw28u_vc_custom_64.dll"+ae2f5

   **Fixed**: one `.spec`-location override. Nothing was guessed —
   `msvcr100.spec:1243` is `@ cdecl _malloc_crt(long) malloc`, so it *is*
   `malloc`, and `dlls/msvcrt/heap.c:430` is the implementation the forward
   names.

**Where it stops now: `MSVCR100.__dllonexit`, and this one was deliberately
NOT fixed.** `__dllonexit` registers a function pointer that native code will
`bctrl` later, at detach. Serving it with a plain thunk would take a sentinel
that faults *by name, now* and turn it into a raw guest pointer stored in a
native table that crashes *illegibly, later* — the exact `FlsAlloc` failure
README documents. The right fix is a row in the registration-interception
table at `dlls/ntdll/signal_ppc64.c:4777`, which today reads

    { L"msvcrt.dll",   "qsort",   4, NULL, 1u << 3 },
    { L"ucrtbase.dll", "qsort",   4, NULL, 1u << 3 },

and has no `msvcr100.dll` row at all. That file is not this change's to edit.
See the handoff below — it is the highest-value single item Styx produced.

Also raised in passing: a hundred exports of `msvcr100` were named sentinels
purely because six headers were not in the signature oracle's translation unit
(`locale.h`, `setjmp.h`, `sys/utime.h`, `sys/timeb.h`, `mbstring.h`, and
`mtdll.h` via `INCLUDE-DIR ../msvcrt`). Emitted went 666 -> 766, recovering
`setlocale`, `localeconv`, `_configthreadlocale`, `longjmp`, `_ftime64`,
`_ismbblead` and — the ones that matter most — the CRT's own `_lock`/`_unlock`.

### The Elder Scrolls V: Skyrim Special Edition (489830)

* **Exe:** `/mnt/caution/steamapps/common/Skyrim Special Edition/SkyrimSE.exe`
  (37 MB). 29 modules mapped, 25 of them AMD64 guest images; the loader
  completes, every guest `DllMain` runs, CRT init runs, `steam_api64.dll` and
  `bink2w64.dll` load.
* **Static audit:** 798 imports, 106 holes, **no module missing a thunk**. None
  of the 15 modules this pass added is needed by Skyrim, and **none of the 106
  holes is reached** — the process dies before the CRT entry point runs. No
  `PROBE-EXTRA` line and no `.thunks` change helps this title at all.
* **It stops at:** `err:seh:RtlUserThreadStart failed to emulate AMD64 entry
  point 000000014383D310, status c0000005`, exit 5.

#### One bug, two titles — the Steam DRM stub calls a NULL pointer

`0x14383D310` **is** `SkyrimSE.exe`'s `AddressOfEntryPoint` (`ImageBase
0x140000000` + RVA `0x383D310`) — so this was never a fresh thread, a TLS
callback, or a sentinel. But it is **not** `mainCRTStartup`: it is the entry of
the **SteamStub v3.1 DRM wrapper**, in a 9th section named `.bind`
(va `0x0383D000`, vsize `0x32918`, characteristics `0x60000000` — read+execute,
no `CNT_CODE`), fully initialised on disk. Its encrypted header decrypts to,
among other fields, `Signature 0xC0DEC0DF`, `SteamAppID 0x00077966` (= 489830)
and `OriginalEntryPoint 0x0153BC64` — so Skyrim's real CRT entry is
`0x140153BC64` and not one instruction of the game itself ever runs.

The stub's first act is the canonical SteamStub anti-debug block:

```c
IsDebuggerPresent();
h  = GetModuleHandleA("ntdll.dll");                       /* -> NULL here */
fn = GetProcAddress(h, "NtSetInformationThread");         /* -> NULL, refused */
fn(GetCurrentThread(), ThreadHideFromDebugger, NULL, 0);  /* -> execute fault at 0 */
```

and the port's own log shows every step of it, including the refusal it is
right to make and the name it was asked for (recovered at runtime from
`emu_GetProcAddress`'s refusal branch):

    find_guest_thunk_target L"KERNEL32.dll".IsDebuggerPresent
    find_guest_thunk_target L"KERNEL32.dll".GetModuleHandleA
    find_guest_thunk_target L"KERNEL32.dll".GetProcAddress
    warn:seh:emu_GetProcAddress GetProcAddress(0000000000000000) is not a guest module, refusing
    ### REFUSED-GPA mod=(nil) name=NtSetInformationThread
    find_guest_thunk_target L"KERNEL32.dll".GetCurrentThread
    handle_syscall_fault code=c0000005 flags=0 addr=(nil) pc=0
    handle_syscall_fault  info[0]=0000000000000008  info[1]=0000000000000000

`info[0] = 8` is `EXCEPTION_EXECUTE_FAULT` and `info[1] = 0`: an **instruction
fetch at address 0**, not a bad data access. The stub does what the
DOOM/`CChromaEditorLibrary` case in README does — discards the failure and
calls anyway.

**THE ROOT CAUSE: this port's guest module namespace never contains
`ntdll.dll`.** `emu_GetModuleHandleA` (`dlls/ntdll/signal_ppc64.c:1220`)
delegates to `find_guest_module` (`:1186`), which walks
`Peb->LdrData->InMemoryOrderModuleList` for `Machine == IMAGE_FILE_MACHINE_AMD64`
and **never loads** — correctly, because `GetModuleHandle` must not load. So
the guest namespace holds only what static imports dragged in, which for
Skyrim is 25 images and `grep -c 'sysx8664.*ntdll'` = **0**. On Windows
`ntdll.dll` is mapped into every process before anything executes;
`GetModuleHandleA("ntdll.dll")` *cannot* fail there, and a great deal of real
software relies on that.

The guest module already exists and is already correct:
`dlls/ntdll/x86_64-windows/ntdll.dll`, machine `0x8664`, **960 named exports**,
`NtSetInformationThread` among them as a genuine trap stub. It is simply never
loaded.

**And it is the same bug in Boltgun, not merely the same message.** Boltgun's
entry `0x14542F310` also lands in a `.bind` section (va `0x0542F000`, vsize
`0x32518`, characteristics `0x60000000`), and **the first 64 bytes at its entry
are byte-identical to Skyrim's**. The guest stack geometry matches to the byte
across the two titles: `guest rsp=…DD08`, module-name argument `…DD50`,
proc-name argument `…DD60`. The `0x310` both entry RVAs end in is the stub
header's `BindSectionOffset` field, not a coincidence.

Two titles, one cause, and it will recur on **every Steam-DRM-wrapped title**
in any corpus. That makes it the highest-value single item this pass produced.

**Ruled out, with evidence, so nobody re-treads it:** TLS callbacks — Skyrim's
TLS callback array at `0x141766000` has a zero first entry, i.e. no callbacks.
Missing-import sentinels — no `DEAD00` and no `wild pointer` appears anywhere
in any Skyrim run log. A "poisoned" DRM thread — the stub is intact and
behaving exactly as designed; it is the port that answers it wrongly.

### The 32-bit refusals — Half-Life 2, FreeInfantry, Styx Win32

All three are PE32/i386 and all three produce the **byte-identical** refusal.
This port has no 32-bit guest and cannot acquire one by accident; the entry
here is evidence for that work item, not a wall anyone can knock down.

Verbatim, for each of `hl2.exe`, `FreeInfantry.exe` and
`Styx/Binaries/Win32/StyxGame.exe`:

```
Application could not be started, or no application associated with the specified file.
ShellExecuteEx failed: Bad format.
```

with `game exited rc=1`.

**Which layer refuses, traced through source rather than guessed:**

1. `dlls/ntdll/unix/loader.c:load_main_exe()` → `open_main_image()` →
   `virtual_map_module()`.
2. The status comes from the **wineserver**: `server/mapping.c:get_image_info()`
   returns `STATUS_INVALID_IMAGE_FORMAT` (`c000007b`) because
   `is_machine_supported(IMAGE_FILE_MACHINE_I386)` is false.
3. `server/registry.c:init_supported_machines()`, `__powerpc64__` branch, lists
   `{ POWERPC64, AMD64 }` and says why in a comment: *I386 is deliberately not
   here… Add I386 when there is a 32-bit guest story, not before.*
4. `load_main_exe()` then falls back to stock Wine's `programs/start/start.exe`,
   whose `GetBinaryTypeW()` hits the same `c000007b`, skips `CreateProcessW`,
   and lands on `ShellExecuteExW` — which produces the two lines above,
   verbatim from `programs/start/start.rc`.

Everything from step 4 on is **stock upstream Wine, unmodified**, faithfully
reproducing what Windows says when nothing can open a file.

**The finding for the 32-bit work item is the mismatch between the two halves
of that.** The *status* trail is loud and named — `supported_machines[]` states
the policy and the reason in a code comment. The *user-visible* text is
Windows' generic "no application associated" message and never says
"32-bit", "i386" or "unsupported architecture" anywhere. Someone reading only
the terminal cannot tell an architecture refusal from a missing file
association. By this project's own standard — loud, named refusals — that is a
gap, and a one-line one: the fallback path knows the machine it rejected and
does not say so.

### Styx: Shards of Darkness

Not installed. `/mnt/caution/steamapps/common/Styx Shards of Darkness/` is an
empty directory and there is no `appmanifest_*.acf` for it. Its compatdata
(`355790`) survives from an earlier install. No action.

---

## Handoffs

Ordered by value. The first two are the ones worth doing next.

### 1. To the guest loader: `ntdll.dll` must always be in the guest namespace

> **CLOSED 2026-08-18, by commit `39f8835393e`** — `dlls/ntdll/loader.c:4902` seeds
> every AMD64 guest process's namespace with `ntdll.dll` once, exactly as this
> handoff asked. Nobody re-ran the titles afterwards, so here is the measurement,
> headless, with this file's own recipe and a 180-second bound:
>
> | | before | after |
> |---|---|---|
> | Skyrim SE (489830) | `c0000005` at the image entry, 0 guest instructions | **rc=124** — past the stub, past `loader_init`, through display-driver and keyboard-layout setup, still alive when the bound fired |
> | Boltgun (2005010) | same fault, byte-identical stub | **rc=124** — same, plus the Common-Controls manifest `fixme` |
>
> Neither run shows `handle_syscall_fault pc=0`, and neither shows the wild-pointer
> fault any more. The expected next wall named below — the stub's Steam checks —
> was not reached inside the bound; what either title is doing after keyboard-layout
> setup is not established, and that is the next measurement rather than a claim.

**This was the single highest-value item in that pass: two titles, one cause,
and it would have recurred on every Steam-DRM-wrapped title.** Full evidence in the
Skyrim entry above.

`find_guest_module()` (`dlls/ntdll/signal_ppc64.c:1186`) walks
`Peb->LdrData->InMemoryOrderModuleList` for AMD64 images and never loads —
which is correct, because `GetModuleHandle` must not load. The consequence is
that the guest namespace contains only what static imports dragged in, and
**`ntdll.dll` is never among them**. On Windows `ntdll.dll` is mapped into
every process before anything executes, so `GetModuleHandleA("ntdll.dll")`
cannot fail; software relies on that, and SteamStub is only the first example.

The fix is not in a fenced file and does not touch `find_guest_module`: seed
the guest namespace with `ntdll.dll` once, when the guest machine is first
brought up, through the existing
`load_guest_dll( L"ntdll.dll", IMAGE_FILE_MACHINE_AMD64, &mod )`
(`dlls/ntdll/loader.c:3792`). The guest module is already built and already
correct — `dlls/ntdll/x86_64-windows/ntdll.dll`, machine `0x8664`, 960 named
exports including `NtSetInformationThread` as a real trap stub. Special-casing
ntdll *inside* `find_guest_module` is the worse alternative twice over: it
lands in a fenced file, and it would make `GetModuleHandle` load, which it
must never do.

Expected next wall after the fix: the stub's Steam checks
(`Local\SteamStart_SharedMemFile`, `Local\SteamStart_SharedMemLock`,
`"Steam Error"`, `"Application load error X:XXXXXXXXXX"` — all recovered from
its decrypted string table), i.e. DRM-shaped rather than structural.

### 2. To the callback-registration table: `msvcr100.dll` has no rows

`dlls/ntdll/signal_ppc64.c:4777` is the per-module, per-API interception table
that stops a raw guest function pointer from reaching native code that will
`bctrl` it. It has rows for `msvcrt.dll` and `ucrtbase.dll` and **none for
`msvcr100.dll`**, which did not exist as a guest module until this pass.

Styx: Master of Shadows is blocked on it right now. Its bundled
`wxmsw28u_vc_custom_64.dll` calls `MSVCR100.__dllonexit` during
`PROCESS_ATTACH`, and Styx additionally imports `qsort`, `bsearch`,
`_beginthread`, `_beginthreadex`, `_onexit`, `_set_invalid_parameter_handler`,
`_set_purecall_handler` and `__setusermatherr` from the same module. Every one
of them hands over a guest pointer.

`msvcr100.thunks` deliberately leaves `__dllonexit` as a named sentinel rather
than serving it: a plain thunk here would convert a fault that names itself
*now* into a native table full of raw guest pointers that crashes *later*,
which is the `FlsAlloc` failure README already documents. The fix is a handful
of rows beside the existing two, and it belongs to whoever owns that file.

### 3. To the port's own legibility: name the fault, not the run's start

> **CLOSED 2026-08-18, by commit `9fc52010e0c`.**

`dlls/ntdll/signal_ppc64.c:5625` prints

    ERR( "failed to emulate AMD64 entry point %p, status %08x\n", entry, status );

where `entry` is the address the run loop was *entered* with, not where the
guest died. It reads as "the entry point is bad" when what happened is "the
guest ran from here and faulted somewhere else entirely". It cost this
investigation its opening premise and a chunk of a day: the brief for Skyrim
began from "a fresh thread whose entry fails", which was a reasonable reading
of the message and wrong in every particular. It should name the faulting
guest RIP, or say plainly that it is naming the run's start.

Related, and worse: a guest call through a NULL pointer arrives as a host
`SIGSEGV` with NIP `0`, so `p_fexbridge_fault_is_jit()` says no,
`emu_handle_fault()` (`dlls/ntdll/unix/loader.c:1332`) stashes no record, and
the guest gets **no `EXCEPTION_RECORD` at all**. A guest that wraps a null call
in `__try`, or installs a vectored handler, loses the whole process instead of
handling it. The comment inside `emu_handle_fault` asserts that "a guest jump
to unfetchable memory never gets here at all: the bridge turns it into
RUN_FAULT internally without any host signal" — that is precisely the case
that failed here, twice, in two titles.

### 4. To the compat tool: the VC++ 2010 C++ runtime

> **ATTEMPTED 2026-08-18 AND NOT SHIPPED — but the blocker is now named, and it is
> not the one below.** Everything this handoff proposed turns out to work; what
> stops it is one step further in, in the port's own guest loader.
>
> The provisioning itself is right, with one correction: the file must go to
> `C:\windows\sysx8664`, **not** `system32` as the `mfc140u.dll` block does.
> `find_dll_file`'s guest branch searches the machine's own system directory
> FIRST, then the tree's builtins, then the ordinary load path — and `msvcp100`
> HAS a builtin, so a copy on the load path is outranked and never opened, while
> `mfc140u` has none and so is fine where it is.
>
> Staging alone is still not enough. Wine's load order for a DLL it implements is
> builtin-first, and the traced result is a loader naming the right path while
> mapping the wrong file:
>
> ```
> trace:module:get_load_order got hardcoded default for L"...\MSVCP100.dll"
> trace:module:find_builtin_dll looking for "msvcp100.dll" for file L"...\MSVCP100.dll"
> trace:module:map_image_into_view mapping PE file L"...\MSVCP100.dll" at ...-0x...3000
>   section .rdata ... section .reloc ...
> ```
>
> — a three-page image with a `.rdata` and a `.reloc` and nothing else, which is
> this tree's exports-less builtin wearing the staged file's name. So the tool has
> to ask for the file as well as copy it: `msvcp100=n`.
>
> With BOTH, the imports resolve. The hundred-odd
> `No implementation for MSVCP100.dll.?_Lockit_ctor@...` warnings drop to **zero**,
> and the file is found and opened —
> `get_nt_and_unix_names ... -> ret 0 ... unix ".../sysx8664/msvcp100.dll"`,
> `get_load_order_value got environment n for L"MSVCP100"`.
>
> **And then the load fails anyway**: `load_dll Failed to load module
> L"MSVCP100.dll"; status=c0000135`, so `import_dll` reports the library not found
> and the image never starts — rc=53 where the sentinel build reached rc=5. That is
> worse, not better, which is why nothing was committed.
>
> So the next step is not the copy list. It is why the guest-machine load path
> cannot complete a NATIVE-order file it has already found, opened and
> machine-checked in the machine system directory. `load_native_dll`
> (`dlls/ntdll/loader.c:3004`) has no guest guard, and both callers of
> `load_builtin` (`dlls/ntdll/unix/virtual.c:3546` and `:3863`) do handle
> `STATUS_IMAGE_ALREADY_LOADED` by mapping the real image — so the refusal is
> inside `load_builtin` itself (`dlls/ntdll/unix/loader.c:2092`). **Traced, with
> a temporary WARN at the top of that function:**
>
> ```
> load_builtin L"\??\C:\windows\sysx8664\MSVCP100.dll"
>              order=2 builtin=1 fakedll=0 machine=0000 search=8664 sysdir=1/8664
> load_dll Failed to load module L"MSVCP100.dll"; status=c0000135
> ```
>
> `order=2` is `LO_NATIVE` and `builtin=1`, so it takes the first of the two
> exits — `if (pe_mapping->image.wine_builtin) { if (loadorder == LO_NATIVE)
> return STATUS_DLL_NOT_FOUND; }`. The open question the next pass starts from is
> why `image.wine_builtin` is set at all for a 1.7 MB Microsoft DLL: either the
> flag describes the NAME rather than the file, or the section under examination
> is already the builtin by the time `load_builtin` sees it (`machine=0000` on
> that line is a hint that this call is not the one carrying the guest's demanded
> machine). Answer that and the C++ runtime loads.


`msvcp100.dll` is a C++ ABI, and translating the MSVC C++ ABI is exactly what
this port decided **not** to do — `dlls/msvcp140` already takes the other road,
and Microsoft's own `msvcp140.dll` runs as an x86-64 guest module under the
emulator. Styx needs the 2010 vintage of the same thing.

Measured: `pfx-ppc64le-native/drive_c/windows/system32/msvcr100.dll` and
`msvcp100.dll` exist but are `Unknown processor 0x01f3` — Wine's own ppc64
builtins. Styx bundles no copies of its own. So there is nothing to load today
and this is not a bug, it is an unprovisioned prefix: the same situation
`ppc64le/steamtool/proton` already handles for `mfc140u.dll`, where it copies
the user's own file across from the app's Proton prefix if they provisioned one
(`protontricks 242640 vcrun2010`). Extending that copy list to the VC++ 2010
redistributable is the fix, and nothing needs to be downloaded or redistributed
to make it work.

### 5. To `tools/spec2thunk`: follow a header's alias of an export name

See "the signature oracle resolves a NAME, and headers rename" above. Two
modules, 102 exports, two different macro mechanisms (`PSAPI_VERSION` and
`WINELIB_NAME_AW`), one cause. Everything needed to fix it is already in the
translation unit the oracle compiles.

### 6. To the 32-bit work item

The refusal is correct and consistent, and the user-visible message does not
name the reason. See above.

---

## Files this pass touched

Nothing is committed; everything below is uncommitted working state.

* **15 new** `dlls/<m>/<m>.thunks` — `d3dcompiler_43`, `d3dx11_43`, `dwmapi`,
  `dxva2`, `faultrep`, `msacm32`, `mscoree`, `msvcp100`, `msvcr100`,
  `normaliz`, `psapi`, `rpcrt4`, `uiautomationcore`, `wldap32`, `xapofx1_5`.
* **15** `dlls/<m>/Makefile.in` — one `<m>.thunks` line added to `SOURCES`
  (`psapi` had no `SOURCES` block at all and gained one).
* **1** `dlls/msvcr100/msvcr100.spec` — two `-arch=ppc64` forwards added
  beside `_initterm_e`, byte-for-byte the pair `msvcrt.spec:563-564` and
  `ucrtbase.spec:429-430` already carry. `GUEST-IMPL` cannot work without
  them. `msvcr100.spec` pins no explicit ordinals, so nothing renumbers.
* **this file**.

## Gates

Green after these changes, on the tree that carries them, re-run after the
`msvcr100` change as well as after the fifteen:

* `ppc64le/thunks/check-import-chain.sh` — PASS; holes still exactly the three
  documented ones, runtime leg PASS.
* `ppc64le/vkd3d/check-ordinal-imports.sh` — PASS; 1388 pinned ordinals across
  21 modules agree with their `.spec`.
* `ppc64le/syscom/check-com-smoke.sh` — PASS 21/21 native and guest,
  byte-identical.
* guest `winepath` — PASS.

The build is warning-free.
