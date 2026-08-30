# D3D12 command-list recording batching — the lever already exists, was measured, and does not move frame time — 2026-08-30

Assignment: design and implement guest-side batching of D3D12 command-list
recording so thousands of deferred calls collapse into one crossing, gated on
an elasticity check, with the instruction to STOP AND REPORT if the check says
the saving would not become frame time.

**Verdict: STOP.  The elasticity check is already answered — not by a proxy
experiment but by the real intervention, built and measured in this tree on
2026-08-27 (`52e07e6fe15`, "winecom: the call journal — hot command-list
methods record guest-side, one crossing replays them").  It removed ~545k
crossings/s (~35% of ALL crossings in the process) and scene fps was FLAT,
because command-list recording runs on the ~25 redDispatcher worker threads,
which idle-spin at ~50%, not on the GameThread that binds the frame rate.
The assignment's premise that this "has never been attempted" is stale by
three days.  No new implementation was built today and no benchmark legs were
burned re-proving a measured null.**

This is the third time the check-before-build discipline has paid: the peek
fast path (f ≈ 1.0, λ ≈ 0, verdict DO NOT BUILD, `peek-fastpath-impl.md`),
the i386 call journal (killed by microbench arithmetic before construction,
NEXT.md "Crossings do not explain 13 fps.  Do not build the i386 call journal
on that theory"), and now this one — except here the full implementation
exists, shipped, and serves; only the *frame-time hope* for it is dead.

---

## 1. The elasticity check result (the thing the assignment gated on)

The assignment proposed injecting artificial per-call latency into recording
calls and measuring the frametime slope.  The tree contains something
strictly stronger: the actual removal, A/B'd against the actual baseline.

[MEASURED 2026-08-27, commit `52e07e6fe15`, Cyberpunk `-benchmark`, SMT4-era
config — the same SMT the box runs today]:

* Crossings removed: `SetGraphicsRootDescriptorTable` 176,243/s → 10/s;
  `DrawIndexedInstanced` 66k/s → 10/s; `IASetIndexBuffer` 51k/s → 4/s;
  `IASetVertexBuffers` 55k/s → 73/s (the residue is >8-views and ring-full
  fallbacks, by design); `SetPipelineState` 12.6k/s → 0.2/s.  COM class
  780k/s → 359k/s.  **ALL crossings 2.9M/s → 2.18M/s.**
* Scene fps: **FLAT — 23.5–24.6 across three journal legs vs 23.95 before.**
* Mechanism, not conjecture [MEASURED, NEXT.md thread profile]: the game
  pulls ~15 cores; **GameThread is pinned at 92%** and the frame rate is that
  one thread's throughput; **~25 redDispatcher workers idle-spin at ~50%
  each**.  Recording — and therefore every crossing the journal removes —
  happens on the workers.  Giving idle threads their cycles back cannot
  shorten a frame bounded by a different thread.

Two independent later measurements corroborate:

* `frame-cost-budget.md` (2026-08-30): with the journal serving, the top
  remaining COM rows are *device* methods (`CopyDescriptors` 102k/s,
  `CreateConstantBufferView` 56k/s), worker-side; vkd3d+RADV is **0.9%** of
  the GameThread; the GameThread's own crossings are ~75–85% `PeekMessageW`.
  §5 item 4 already records: "Batching COM further does not move the floor."
* `peek-fastpath-impl.md` (2026-08-30): the GameThread's dominant crossing
  load is an **elastic idle spin** — the thread polls while "blocked on
  dependencies elsewhere."  Injected peek delays up to ~30 ms/frame moved the
  floor by ~0.  Whatever the GameThread waits FOR, the journal A/B shows it
  is not recording-worker throughput: half a million crossings/s of worker
  relief produced zero fps.

λ for command-list recording cost, at the GameThread frame floor ≈ **0**, by
direct intervention.  The assignment's stop clause applies.

One caveat kept honest: NEXT.md notes "whether that headroom turns into fps
at SMT2 or under the performance governor is unmeasured."  The box is back at
SMT4 today (`ppc64_cpu --smt` = 4, the configuration the null was measured
in), so the caveat is moot for the current setup.  The ready-made re-check if
the config ever changes again is one A/B pair — `WINEEMUNOCOMJOURNAL=1` (the
journal's negative control) vs default — comparing floors from `frames.csv`
with repeats; no code needed.

## 2. The method classification, as it stands implemented

The recording surface the assignment asked to enumerate is enumerated in
`libs/winecom/winecom.c` (the `journal_slots[]` table and the wall-by-wall
correctness comment above `install_journal`), plus the device journal and the
named refusals.  Current state:

**Batched guest-side, shipping, default-on** (curated by name, void-returning,
per-command-list ring, replay in order through `invoke_marshalled` at the
object's next real trap):

| slot | shape |
|---|---|
| `ID3D12GraphicsCommandList::SetGraphicsRootDescriptorTable` | 2 reg args |
| `::SetComputeRootDescriptorTable` | 2 reg args |
| `::SetPipelineState` | 1 reg arg (proxy, unwrapped at drain) |
| `::SetGraphicsRoot32BitConstant` | 3 reg args |
| `::DrawIndexedInstanced` | 5 args |
| `::DrawInstanced` | 4 args |
| `::IASetIndexBuffer` | 16-byte view copied INTO the record |
| `::IASetVertexBuffers` | ≤8 views × 24 bytes copied in; >8 falls back to the trap |

The lifetime/ordering/flush walls the assignment worried about are each
addressed in that comment: single-recorder-per-list (D3D12's own rule);
every NON-journaled call on the list traps and drains first, so
interleavings replay as issued; `Close` is not journaled, so the ring is
empty by `ExecuteCommandLists` (and `wc_forward_host` drains an object
passed as an argument anyway); everything recorded is by-value or copied
into the record; a full ring falls back to the slot's own trap stub; a
curated row whose vtable shape stops matching is REFUSED at install —
fail closed to trapping, never to a wrong record.

**Round-trips by design**: `ResourceBarrier` (9.5k/s, struct arrays —
unbatched, the largest remaining list-side row and still small);
`CopyDescriptors` (traps deliberately: its trap is the device journal's
ordering point); `Close`, `ExecuteCommandLists`, everything returning a
value or carrying unclear lifetime.

**Built but OPT-IN**: the device journal (`9f18510f03c`) — free-threaded
`CreateConstantBufferView` into per-thread TEB-anchored rings, RDTSC-stamped,
k-way-merged at every dispatch.  Hardened three times over and gated by
`check-dev-journal.sh`, but with it on, Cyberpunk raises one amdgpu gfx-ring
timeout per leg (one escalated to a MODE1 crashloop); the replay stream was
ruled out on data (2.08M traced replays, zero inversions, double-apply runs
clean).  `frame-cost-budget.md` §5 already deprioritizes chasing that hang
for fps — the calls it would batch are also worker-side.

## 3. Correctness verification, as it stands

Not re-run today; on record from the landing sessions: mid-flythrough
screenshot with every draw present, zero journal errors in the run log, the
full 34-gate sweep green in both directions, and the negative-control /
sabotage / double-apply env levers (`WINEEMUNOCOMJOURNAL`,
`WINEEMUCOMDEVSABOTAGE`, `WINEEMUCOMDEVDOUBLE`) kept in the tree.  The one
live bug the first leg found (r11 clobber in the IASetVertexBuffers copy
loop → "journal pos beyond cap", dropped rings, LOUD) failed exactly the way
the project's discipline demands — the drain validates structurally and
falls back to trapping rather than replaying garbage.

## 4. Where the frame time actually is

The floor's addressable mass is on the GameThread, and the two largest named
items are both now measured elastic (peeks) or worker-side (COM).  The open
lever, named by `peek-fastpath-impl.md` §3 and still untouched: **off-CPU /
wakeup analysis of what the GameThread's elastic spin waits for** (`perf
sched` on its dependencies — fences, semaphore chains, the present path),
plus the JIT-quality half (37–40% of the thread is the game's own code under
the JIT).  Batching more COM buys worker CPU and power (~175 ms/s of worker
CPU at the budget doc's rates), which matters at SMT2 or on power budgets —
the parked device journal and its hang hunt are that work, if it is ever
worth doing.

## 5. Provenance

* `52e07e6fe15` (2026-08-27 16:19) — the call journal, implementation + A/B.
* `9f18510f03c` (2026-08-27 23:23) — the device journal, opt-in + hang record.
* `ppc64le/NEXT.md` (thread profile, lever matrix, journal ledger) and
  `ppc64le/cpu/CROSSINGS.md` (rates before/after, unbatched rows).
* `ppc64le/docs/sessions/2026-08-29/frame-cost-budget.md` §4–§5.
* `ppc64le/docs/sessions/2026-08-29/peek-fastpath-impl.md` §2–§3.
* Today: verified the journal is in the shipped artifacts (the
  `WINEEMUNOCOMJOURNAL` gate string is present in
  `wine-build/dlls/{d3d12,d3d11,combase,…}/ppc64-windows/*.dll.so`, built
  2026-08-29 20:02, after the last `libs/winecom/winecom.c` commit), that no
  steamtool/appconfig sets the kill switch, and SMT=4.  No game was launched
  and no code was changed.
