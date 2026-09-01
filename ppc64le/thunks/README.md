# The guest thunk boundary

A guest x86-64 PE calls native ppc64 code by trapping. `tools/spec2thunk`
generates an AMD64 thunk PE per module; each exported stub traps into the
embedded emulator's callback, and `emu_trap_dispatch` in
`dlls/ntdll/signal_ppc64.c` marshals MS-x64 → ELFv2 and calls the real
implementation.

## What the generator publishes

Each thunk PE exports `__wine_thunk_info`, a versioned descriptor. The host
checks the version for **exact equality** — a module either matches or is
rejected, so there is no mixed build to be compatible with.

| field | added | what it carries |
|---|---|---|
| `names_rva`, `sigs_rva`, `stubs_rva`, `stride`, `trap_off` | 1 | export names, arity/flags, where the stubs are |
| `impl_names_rva` | 4 | the name to resolve natively, when it differs |
| `fp_rva` | 5 | which arguments and return travel in FP registers, and their width |
| `widths_rva` | 6 | argument width, 2 bits each: 8 / 4 / 2 / 1 bytes |
| `signs_rva` | 7 | which sub-word arguments are **signed** |
| `geom32_rva` | 8 | the i386 stdcall frame: total 4-byte slots, which arguments take two, whether the return is `EDX:EAX` |

Current version: **8**.

The `sigs_rva` word also carries the row's **signature tier** in bit 10
(`THUNK_SIG_SRCTIER`): set means the signature came from the module's own
implementing C definition rather than from a Wine header declaration. It
changes no marshalling — both tiers measure widths, signs and fp by the same
rules, in different translation units — and exists only so the
`WINEEMUNOFLAT*` levers below can put a chosen subset of the source tier back
to its pre-tier refusal without a rebuild.
`ppc64le/thunks/flat-tier-rows.py` reads that bit straight out of a built
thunk PE, which is the only place the tier is recorded beside the artifact a
run actually loads.

## Why widths and signs exist

The two ABIs disagree about who extends an integer narrower than a register,
and the disagreement produces **wrong numbers, not crashes** — so nothing that
only asks whether a call returned can see it.

- MS-x64 lets the caller write only the declared width (clang emits
  `movw $0x1, %dx`) and makes ignoring the rest the callee's job.
- ELFv2 makes extension the **caller's** job, and ties the kind of extension to
  the declared type — sign for `SHORT`, zero for `WORD`. A ppc64 callee at
  `-O2` may not re-extend.

Three live bugs came from this, each looking like something else:

| call | answered | should have |
|---|---|---|
| `GetActiveProcessorCount(ALL_PROCESSOR_GROUPS)` | 0 | the processor count |
| `IWMSyncReader::GetStreamSelected(1)` | read `0x40000001` | 1 |
| `VarI4FromBool(VARIANT_TRUE)` | 65535 | −1 |

`VARIANT_TRUE` is −1, which is why signedness and not just width was needed.

Measured on the built tables: **805 of 15,456** exported thunks take a 1- or
2-byte by-value argument; **131** take a signed one, across `kernel32`,
`kernelbase`, `oleaut32`, `opengl32`, `shlwapi` and `user32` — including the
whole `glVertex2s` / `glColor3b` family.

## How the widths are measured

`tools/spec2thunk/wine_sig.py` compiles one translation unit of
`char x[sizeof(T)];` globals with the **same clang, target and headers** that
verified the signature, and reads the array bounds back out of the LLVM IR. No
parsing of type spellings, no table of typedef widths. Signedness uses the same
trick with `char x[1 + ((T)-1 < (T)0)];`, asked only about types already
measured at 1 or 2 bytes — which is also what keeps the probe compilable.

A type that will not compile drops out and the export publishes nothing, which
the host treats as "no information", never as a guess.

## The COM half

`libs/winecom` carries the same thing per vtable slot:
`narrowmask` / `narrowwide` / `narrowsign` in `struct winecom_slot`. The two
surfaces agree by construction rather than by accident.

Generators on lanes with no sub-word parameters (`ppc64le/dxvk`,
`ppc64le/audio`, measured zero) do not carry the masks. They **refuse** such a
parameter by name instead, so the day a roster entry adds one it fails closed.

## The RIP cache

`emu_trap_dispatch` resolves a trapping RIP to its target through a lock-free
direct-mapped cache with a per-slot seqlock. Before it, every crossing took the
loader lock; that was the boundary serialization. COM vtable slots are cached
the same way.

## Gates

| gate | covers |
|---|---|
| `check-rip-cache.sh` | Values across both crossing kinds, from four threads. 17 crossings per pass. Four sabotage legs. |
| `check-import-chain.sh` | Every import of every tested game's modules resolves, or is a named hole. |
| `check-optional-module.sh` | Modules reached only by `LoadLibrary` — invisible to import-table checking. |
| `check-source-tier.sh` | The source-definition signature tier really reads definitions, and refuses what it must. |
| `check-flat-levers.sh` | The `WINEEMUNOFLAT*` kill switches fire, fire only where told, and are loud when they miss. |

## Levers

| variable | effect |
|---|---|
| `WINEEMUNORIPCACHE=1` | Cache off. |
| `WINEEMURIPCACHEBLIND=1` | Cache answers for the wrong address. |
| `WINEEMUNOARGWIDTH=1` | Sub-word arguments cut to 32 bits (pre-version-6 behaviour). |
| `WINEEMUNOARGSIGN=1` | Widths kept, signed arguments zero-extended. |
| `WINEEMUNOCBWRAP=1` | Raw guest callback pointers handed to native code. |
| `WINEEMUNOFLATTIER=source` | Every **source-tier** row in every module back to its pre-tier state: the export goes absent, so an import binds to a `0xdead0000+n` sentinel and `GetProcAddress` answers NULL. |
| `WINEEMUNOFLATMODS=a,b` | The same, for the named guest DLLs only. |
| `WINEEMUNOFLATROWS=m!E` | The same, per export. `module!Export`, a bare `Export`, or `@/path/file`. |

The three `WINEEMUNOFLAT*` levers reach **only** the source tier — the header
tier is not what landed, so it is not a restore target, and naming a
header-tier row forces nothing and says so. They route to the pre-tier
refusal that already exists (`find_ordinal_export` answering NULL for an
export-table hole) rather than to a refusal of their own; see the banner in
`dlls/ntdll/signal_ppc64.c` and
`ppc64le/docs/sessions/2026-09-01/w3-load-regression-bisect.md`.

`WINEEMUNOARGSIGN` exists separately from `WINEEMUNOARGWIDTH` because turning
narrowing off entirely would hide a signedness bug behind a width bug.

## Known limits

- 32-bit arguments stay zero-extended whatever their type. The PE side is LP64
  and a negative 32-bit value has always reached an LP64 `long` zero-extended;
  sub-word arguments have no such history, so the ABI's own rule is followed
  there.
- The width word is 2 bits × 16 arguments and the sign word 1 bit × 32;
  `THUNK_MAX_ARGS` is 16, so both bound the same set.
- MSVC-mangled C++ members are refused — the signature oracle cannot declare
  them. Six flat-shaped exceptions in `dlls/msvcr100/msvcr100.thunks` carry an
  explicit `.spec` citation.
- `__CxxFrameHandler3` and `_CxxThrowException` stay refused by design: they are
  the guest image's own language personality.
