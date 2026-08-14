# vkd3d-ppc64le

vkd3d-proton for ppc64le, carried as a **patch series against a pinned upstream
commit** rather than a fork — the same arrangement as `dxvk-ppc64le`, so the
provenance of every changed line is `git diff` and the LGPL obligations below
stay cheap to meet.

```sh
./bootstrap.sh              # clone at the pinned commit, apply vkd3d-patches/
./bootstrap.sh --check      # verify an existing src/ matches, change nothing
./bootstrap.sh --force      # re-clone from scratch
```

`src/` is upstream's checkout and is gitignored. The series is applied to the
**working tree and never committed**, which is what lets the probe below
recover the pre-change behaviour with `git show HEAD:<file>`.

## What the series changes, and why

`vkd3d-patches/0001-tagged-native-event-handles.patch`

On the native (non-Windows) build, an event `HANDLE` crossing vkd3d's boundary
was a **bare fd cast** — `handle.fd = (int)(intptr_t)os_handle`. This port
replaces that with a **tagged** encoding:

```
HANDLE = 0x4556464400000000 | fd        /* 'EVFD' in bits 63..32 */
```

Two reasons, both demonstrated by the probe rather than argued:

1. **Collision.** A bare fd is indistinguishable from every other `HANDLE` the
   library is handed. Here a D3D12 caller can be an emulated x86-64 PE whose
   `HANDLE` crosses the FEX thunk untranslated, so what arrives is a *Wine*
   object — a small integer sitting squarely in the range of live fd numbers.
   `[MEASURED]` against pristine upstream: handing `SetEventOnCompletion` the
   number of a live pipe descriptor causes eight bytes to be written into that
   pipe. Tagging turns that into a refusal.
2. **fd 0.** Upstream drops an eventfd that lands on fd 0 and says so in a
   comment, then works around it with a `dup()` dance in
   `GetFrameLatencyEvent` and again in `tests/d3d12_crosstest.h`. The
   `GetFrameLatencyEvent` workaround was itself broken — `fd = dup(fd);
   close(fd);` returns the number of a descriptor it has just closed, and leaks
   fd 0 — so on the one occasion it ran, the caller got a `HANDLE` naming a
   closed fd. A tagged fd 0 is not `NULL`, so the hazard and all three
   workarounds are gone.

**The tag is the same one DXVK uses**, deliberately, so a handle minted by
either library is understood by the other:
`dxvk-ppc64le/src/include/native/windows/dxvk_native_event.h` (that project's
`dxvk-patches/0003`) and `dxvk-ppc64le/thunk/runtime/dxvk_guest_event.h`. The
constant is spelled three times because the three are built independently; that
is only safe because both projects' probes include two of the copies and assert
they agree, and both probes are in their gates. A coherent port beats matching
upstream, and we are the upstream for both of these here.

Files touched: `include/vkd3d_native_event_handle.h` (new, and **public** —
the demos are built with `vkd3d_public_includes` only, and so is every real
external consumer), `include/private/vkd3d_native_sync_handle.h`,
`libs/vkd3d/swapchain.c`, `tests/d3d12_crosstest.h`, `demos/demo_xcb.h`.

Behaviour that does **not** change: `_WIN32` is untouched; `NULL` is still
refused exactly as before, which is what the legitimate non-event callers pass
(`vkd3d_waiting_event_signal`'s latch path, and
`d3d12_device_SetEventOnMultipleFenceCompletion`'s `MULTI_ANY` case); and
`vkd3d_native_sync_handle_create`, which never went through `wrap()`, is
unchanged.

## Verifying it

```sh
./probes/tagged_handle_run.sh
```

Builds `probes/tagged_handle_probe.c` **twice** — against the patched tree, and
against `include/private/vkd3d_native_sync_handle.h` exactly as it is at the
pinned commit, extracted with `git show`. That second build is not a simulation
of the old behaviour; it is the old behaviour, compiled. The patched build must
pass all six cases and the upstream build must fail two of them (fd 0, and the
collision — including the eight bytes landing in the pipe). The script refuses
to claim an A/B if the "before" build turns out to have compiled the patched
header, which is a mistake it has already made once.

`[MEASURED] 2026-08-14, AC922, kernel 7.1.4` — patched `PASSED (0 failures)`,
upstream `FAILED (5 failures)`.

## Building

`[MEASURED] 2026-08-14` upstream vkd3d-proton at the pinned commit, with this
series applied, **builds for ppc64le at `-mcpu=power8`** — library, tests,
demos and programs, 282 targets, no errors. It needs `widl`
(`hangover-ppc64le/wine-build/tools/widl/widl` will do), meson, ninja and
glslang.

```sh
PATH=$HOME/Development/powerpc64le-ports/hangover-ppc64le/wine-build/tools/widl:$PATH \
  meson setup build-native src -Dbuildtype=release -Denable_tests=true \
        -Dc_args=-mcpu=power8 -Dcpp_args=-mcpu=power8
ninja -C build-native -j 176
```

That is a compile, not a port. Nothing here claims the result runs, and the
`d3d12` test binary has not been executed on this machine. Whether
vkd3d-proton *works* on ppc64le is a separate question that this series does
not answer.

## Layout

| Path | |
|---|---|
| `bootstrap.sh` | reconstructs `src/` at the pinned commit with the series applied |
| `vkd3d-patches/` | our changes to vkd3d-proton, as a revertible series |
| `probes/` | the before/after for the series |
| `docs/` | `feasibility.md`, `fence-callback.md` |
| `experiments/` | `fence-callback/` — the host→guest wakeup harness |

## Licence

**vkd3d-proton is LGPL-2.1-or-later** — © Hans-Kristian Arntzen and others, for
Valve. This is the one to watch: unlike DXVK's zlib, the LGPL places real
obligations on anyone distributing **modified binaries** — the modifications
stay LGPL and recipients must be able to relink against a changed library. Our
changes being a patch series against a pinned commit is what makes that
straightforward rather than an audit.

The checkout also vendors `dxil-spirv` and the Khronos SPIR-V and Vulkan
headers (Apache-2.0, which carries patent-grant terms). Worth reading before
redistributing binaries.
