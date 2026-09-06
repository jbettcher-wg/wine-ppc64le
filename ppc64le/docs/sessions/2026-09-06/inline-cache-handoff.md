# Inline caching of COM slot call sites -- a spec for the fastppcx86 tree

Written 2026-09-06 by the wine side.  Third of the reviewer's three picks
(the other two shipped wine-side the same day: batch replay and dead-record
pruning of the context journal, `crossing-asm-op4k.md` section 15).  This
is a JIT change, so it is specified here and not built here.

## What the guest does today

A COM call in the guest is `mov rax, [rcx]; call [rax + 8*slot]`: `rcx` is
one of winecom's proxies, `[rcx]` its guest vtable (a row of the
materialised block `guest_vtbl_block`, one block per surface, bounds known
to the bridge as the digest's `vt_lo`/`vt_size`), and the slot entry is one
of: a registered stub RIP (compiles to `EcTransition`, section 14), a
journal snippet (install_journal repoints it), or a const-getter snippet.
The call is indirect, so every one of them is a count-cache prediction and,
in the bridge lane with block linking off, a dispatcher lookup on the way
in and on the `ret` out.

## The cache

At a call site the JIT has already seen resolve to one target, emit the
monomorphic inline cache:

    load  vt   = [rcx]                 ; the proxy's vtable (guest load)
    load  tgt  = [vt + 8*slot]         ; the slot entry
    cmp   tgt, CACHED_TARGET           ; the guard
    bne   slow                         ; polymorphic or repointed: today's path
    bl    CACHED_NATIVE                ; a DIRECT branch to the compiled block
                                       ;   for CACHED_TARGET (the EcTransition
                                       ;   block, or the snippet's block)
    ...return lands here, no dispatcher

The guard compares the SLOT ENTRY, not the vtable pointer: two proxies of
different interfaces share nothing, but two proxies of the SAME interface
share the vtable row, so one cache serves every object of that type, and a
slot that install_journal or install_const_getters repointed after the
cache was filled fails the guard by construction (the entry changed).
Nothing on the wine side has to invalidate anything.

What the wine side guarantees, so the guard is sufficient:

- A slot entry in `guest_vtbl_block` is written exactly twice at most:
  once at attach (the stub RIP) and once by an install pass that runs
  BEFORE the first guest call through the surface (`winecom_attach`,
  under `wc_cs`).  It never changes afterwards; a lever (WINEEMUNOCOM*)
  changes what is installed, never a live entry.
- The block is never freed or moved for the process's life.
- `vt_lo`/`vt_size` in the digest bound the block; a `vt` outside it is
  a guest-implemented object (or garbage) and must take the slow path.

## What is worth caching, in order

1. `EcTransition` targets (the trap stubs): the `bl` replaces the
   dispatcher entry AND the `ret` dispatch; section 14 measured the
   direct COM slot at 70 ns with ~7 mispredicts per crossing in the
   dispatcher's `bctr`.  Expect the mispredicts to be the gain.
2. Journal snippet targets: the snippet's own block, called directly.
   The record is 69 ns now (section 15), and the call/ret dispatch pair
   is a real share of it.
3. Const-getter snippets: same shape, cheaper still.

Do not cache a target outside the guest vtable block (an app's own COM
object): its slots can be rewritten by the app at any time.

## Register-direct arguments -- only PASS-class, as the reviewer said

For a cached `EcTransition` the block still spills the guest file and the
digest's `ext[]` widths still apply; the win here is the branch, not the
marshal.  Do not move the classifier into the JIT.

## How to measure

`ppc64le/cpu/bench-com-crossing.sh` prints, per run:

    com_gettype_ns_per_call             a direct-served trap slot (83 ns today)
    com_journaled_topology_ns_per_call  a journaled slot (69 ns today, LWW on;
                                        WINEEMUNOCOMLWW=1 for the 125 ns
                                        every-record-replays form)

`PERF_STAT=1` adds a `PERFSTAT <loop> cycles= insns= branches=
mispredicts=` line per loop (each loop run at N and at 0 under `perf
stat`, deltas divided by N): mispredicts are the claim, so the A/B record
is the timing line AND the counter line.  Interleave rounds A/B with the
cache off (a FEX_ lever, please -- every port change has one -- and it
belongs in the CodeCache config hash like every other codegen toggle, so
a cached block from the other arm is never served); the box's governor
moves the numbers by 20%, so same governor on both arms.

Two wine-side facts the reviewer asked about, so they are on record: the
intern table grows with the proxy count and the dispatch once-guard reads
before it swaps (80091e5a732, 2026-09-04), and `proxy_from_pointer` takes
no lock at all -- a live tag on never-recycled proxy memory plus an O(1)
vtable check (c450c940876, same day).  The replay loop uses that same
lookup once per record, so the remaining per-record cost is not a chain
walk; the counters say what it is (crossing-asm-op4k.md section 15): a
direct row is 303 instructions / 3.0 mispredicts per call, a journaled
record alone 287 / 2.0, a batched replay of one record adds ~330 / 5.4
on top -- branchy per-record C, wine-side work, not yours.  The 3.0
mispredicts of the direct row are the ones the inline cache is after.  The gates that must stay green:
`ppc64le/cpu/check-ec-direct.sh`, `check-ec-leaf.sh`, `check-com-fastpath.sh`,
`ppc64le/winecom/check-ctx-journal.sh` (and `--sabotage`),
`ppc64le/dxvk/check-d3d11-smoke.sh`.

## Two smaller JIT-side notes from the same day

- `FEX_TSOENABLED=0` took the journaled record from 75 to 54 ns: the
  per-store fence is ~3.5 ns on POWER8 and the port's own snippets
  (journal records, const getters) write single-writer rings that the
  drain reads with an acquire on `pos`.  A way to mark the snippet block
  as unfenced -- it is wine-emitted code at a known address range, the
  `PAGE_EXECUTE_READWRITE` block install_journal allocates -- would be
  worth ~15 ns per record with no wine-side change.
- The remaining per-record replay cost (~55 ns) is PE-side C, not JIT;
  nothing to do there from your side.
