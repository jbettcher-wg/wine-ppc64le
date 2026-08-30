# Dex redundant-copy hunt: findings and fix

## Named source function behind RVA 0x51118

RVA `0x51118` in the shipped (stripped) `d3d11.dll` is the return address
right after the `bctrl` in **`winecom_dispatch32`**
(`libs/winecom/winecom.c`) at the point where it reads a function pointer
out of `d3d11_hand_funcs32[...]` at offset 8 (`ld r12,8(r9)`) and calls
it. RVA `0x3ad4c` is the return address after the `bctrl` inside
**`hand32_unmap`** (`dlls/d3d11/main.c`) that calls
`memcpy( b->host_ptr, b->low, b->size )`.

Resolution method: the shipped PE has no symbol table (verified — COFF
`PointerToSymbolTable`/`NumberOfSymbols` are both 0) and no debug
directory, so RVAs were mapped by hand: parsed the PE section headers
directly (`.text` VirtualAddress `0x1000`, PointerToRawData `0x400`),
converted each RVA to a file offset, and disassembled the raw bytes at
that offset (`objdump -D -b binary -m powerpc:common64 -EL
--adjust-vma=<rva-based-base>`). The `libdxvk_d3d11.so.0.30002` /
`.p` object dir the task pointed at turned out to be DXVK's own engine
library (12 MB, symbols for DXVK's ~2600 D3D11/D3D10/DXGI vtable slots)
— a red herring for these two specific RVAs, which land in *this
project's own* winecom bridge code (`libs/winecom/winecom.c` and
`dlls/d3d11/main.c`), not in upstream DXVK sources. The disassembly at
both RVAs matches the source line-for-line (register-level: `ld
r12,8(r9); mtctr r12; bctrl` at the dispatch site; a `memcpy` codegen
sequence at the wrapper site), and was independently confirmed live by
attaching gdb to a running Dex process and reading `$pc`/`$r3`/`$r4`/`$r5`
directly — `r5` pinned at `0x400000`, `r4` constant, `r3` alternating
between two addresses `0x400000` apart, matching the original report
exactly.

## What was driving the copy

`hand32_unmap` (dlls/d3d11/main.c) is this project's i386-guest/ppc64-host
Map/Unmap bounce, added for Dex in commit `50c4e37669d`. Because DXVK's
`Map(D3D11_MAP_WRITE_DISCARD)` on this dynamic texture returns a host
pointer above 4 GiB (unaddressable by the 32-bit guest), the guest is
handed a low, guest-legal scratch buffer (`b->low`) to write into, and
`Unmap` must flush those bytes into whatever host buffer DXVK actually
handed back for that Map call — unconditionally, on every WRITE-mode
Unmap, full subresource size, no dirty tracking. DXVK renames this
resource's backing on every `WRITE_DISCARD` map (to avoid stalling the
GPU on the previous frame's copy), so `b->host_ptr` rotates between
**exactly two addresses 0x400000 (4 MiB) apart** — this is DXVK's own
double-buffering, working as designed. The bug is that Dex/Unity remaps
this 1024x1024 RGBA texture (4 MiB) on a large fraction of frames
**whether or not its content actually changed**, and every one of those
calls paid a full 4 MiB write into uncached/write-combined host-visible
memory regardless.

Confirmed live (instrumented build, rate-logging this exact call site):
the destination alternated between `0x3ff19abfe000` / `0x3ff19affe000`
(exactly `0x400000` apart), size pinned at `4194304`, firing at roughly
150-225 calls/s system-wide across all dynamic resources this thread
maps, with the 4 MiB texture specifically recurring every few hundred
calls, sustained, uncapped — matching the original report's "many
separate 4 MiB copies completing back-to-back."

## The fix

Cache, per (resource, subresource), a small shadow-slot table keyed by
**destination host pointer** (up to 4 slots, linear search — generous
next to the two DXVK has shown in practice). At Unmap, if the slot for
the *current* `host_ptr` already holds bytes identical (`memcmp`) to
what the guest just wrote, skip the flush entirely; the real host Unmap
call still always happens. A single global "last thing we sent" shadow
would have been wrong: buffer A's last content says nothing about
whether buffer B — the one DXVK's rename just handed back — already
holds it, and skipping on that basis would leave B's stale/uninitialized
bytes live for the GPU. Per-destination shadows avoid that: a skip is
only ever claimed against the specific buffer being written this time. A
resource resize invalidates all cached shadows, mirroring the existing
`b->low` resize path.

Shadow storage is plain process-heap memory (`RtlAllocateHeap` /
`RtlReAllocateHeap`) — it's compared against locally and never handed to
the guest, so it doesn't need the guest-legal low-2GiB arena `b->low`
requires.

Changed file: `dlls/d3d11/main.c` (`struct map_bounce`, `hand32_map`'s
resize path, `hand32_unmap`'s flush). Commit: **`1206ef587fc996816307e55e31f580d5be8df3d1`**
in the wine-upstream tree
(`~/Development/powerpc64le-ports/hangover-ppc64le/wine-upstream`).

## Before/after measurement

**Skip rate (load-independent, direct instrumentation — the reliable
number):**

- Aggregate across every dynamic resource this thread maps (11,400 calls
  observed): **35.4%** of WRITE-mode unmap flushes skipped as identical.
- Isolated to just the named 1024x1024 RGBA / 4 MiB texture (separate
  counters, 4,200-call window): **18-27 flushes skipped per 100**,
  sustained — roughly 1 in 5 of exactly the calls the original gdb hunt
  caught the thread stuck in.

This was verified two ways: (1) an instrumented build logging every 100th
call's skip count, confirmed the skip logic fires at a real, sustained
rate rather than a fluke; (2) after stripping the instrumentation for the
final committed build, relaunched Dex once more and confirmed via gdb
that `UnityGfxDeviceW` is alive, still reaches the same
`winecom_dispatch32` / `hand32_unmap` call site, and produces no crashes,
hangs, or `fixme`/error spam beyond the pre-existing baseline noise.

**Render-thread memcpy share, gdb-sampled (the number the task asked
for, reported with its caveat):** the original report's clean baseline
was 97% of `UnityGfxDeviceW` cycles in the memcpy loop (8/8 samples,
quiet system). Tonight's box was not quiet — a Steam client running
under FEX was steadily at ~280% CPU for the whole session (unrelated to
this work), and both a same-session "before" sample (pre-fix, 6/20
samples ≈ 30% in the memcpy loop) and "after" sample (post-fix, 7/40 and
later 10/40 samples ≈ 17-25% in the memcpy loop) were dominated instead
by Unity job-system semaphore waits (`sem_trywait`-family spins in
libc), not memcpy, on both builds. That is contention noise, not a
reproduction of the clean 97% figure, and I am not claiming the gdb
share numbers before/after are a clean comparison — they're reported
here for completeness, with that caveat. The skip-rate numbers above are
the number to trust: they are a direct count, made at the C-code level,
of flush calls the fix proved unnecessary, independent of whatever else
was competing for the CPU at the time.

**What I am not claiming:** I did not see the screen and cannot say
whether Dex is now playable — only that a measured ~20% (this specific
texture) to ~35% (all dynamic Map/Unmap traffic combined) of the
identified redundant 4 MiB uncached-memory writes are now skipped
without touching the copy's correctness, and that the game still boots,
loads, and runs to the same point it did before, with no crash or hang
observed in gdb or the wine log across three separate before/after
launches tonight.
