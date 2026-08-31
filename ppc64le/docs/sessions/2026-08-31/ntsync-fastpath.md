# The ntsync fast path works, is taken, and buys nothing -- because these
# semaphores are a handoff, not a counter

**[MEASURED 2026-08-31, Cyberpunk 2077 benchmark, native lane, AC922 POWER9.]**

Yesterday's census (`2026-08-30/ntsync-ioctl-census.md`) found that 91% of every
ioctl this port issues is ntsync, and that `WAIT_ANY` + `SEM_RELEASE` are 70% of
all ioctls between them. The proposed lever was a userspace fast path: an
already-signalled acquire becomes an atomic decrement on a shared count, and a
release with nobody queued becomes an atomic add. Both halves are now built,
both are verifiably taken, and **the frame floor does not move.** The reason is
specific and is the useful part of this result.

## The measurement that explains it

One leg with the fast path in statistics mode:

    acquire   242,940 hit / 2,514,839 miss     -- 8.8% hit rate
    release 2,691,268 fast, of which 2,677,417 still needed a wake ioctl -- 99.5%

- **91% of acquires find the semaphore empty.** The premise -- "the object is
  usually already signalled" -- is false here. The wait has to block.
- **99.5% of releases have someone queued.** So the release cannot skip the
  syscall either; only the kernel can wake a sleeping task, and one is almost
  always sleeping.

Syscalls actually removed: 242,940 (acquires) + 13,851 (releases that found
nobody queued) = **~257k, against ~5.19M still made on those same paths. Under
5%**, not the 70% the census suggested was addressable.

**These semaphores are a handoff, not a counter.** A producer releases and the
consumer is already blocked waiting for it. Every release must wake somebody;
every acquire must sleep. That is exactly the case a userspace fast path cannot
remove, because the syscall is not bookkeeping -- it is the scheduling
operation itself. A fast path removes syscalls that were doing nothing. These
are doing something.

There is a second, independent limiter. Sampling `/proc/<pid>/maps` against
`/proc/<pid>/fd` on the live game: **95 ntsync objects, of which 4 are
semaphores** -- 4%. The fast path is semaphore-only, so most of the 313k
`WAIT_ANY` calls are on events it never sees. Extending it to events would
raise coverage, but the hit-rate numbers above say the acquires would miss for
the same reason.

## The benchmark, for completeness

Interleaved single legs, alternating, same session, fast path off then on:

| leg | floor_ms | avg fps |
|---|---:|---:|
| off r1 | 31.77 | 17.462 |
| off r2 | (no result) | -- |
| off r3 | 32.91 | 16.919 |
| on r1  | 34.28 | 17.829 |
| on r2  | 31.13 | 18.428 |
| on r3  | 34.18 | 17.166 |

The on arm straddles the off arm. The predicted effect was 5-11 ms, which is
several times the same-day spread (~2-4 ms) and would have been unmistakable at
three legs a side. It is absent, and the counters above say it is absent
because the syscalls were never removed -- not because removing syscalls fails
to pay.

## What was built

`ppc64le/kernel/ntsync-fastpath/ntsync.c` -- the stock module plus `mmap()` on
the object fd, exposing one page per object. `dlls/ntdll/unix/sync.c` -- the
userspace half, behind `WINE_PPC64LE_NTSYNC_FASTPATH`, default off.

The whole protocol is **one 64-bit word**:

    bits  0..31  count       the semaphore count
    bits 32..60  waiters     tasks queued on this object in the kernel
    bit  61      HOLD        the kernel has a unit on loan
    bit  62      WAIT_ALL    a wait-all is queued -- userspace keep out
    bit  63      NO_FASTPATH permanent; set on mutexes and events

Putting count and waiters in the same location is the entire safety argument.
Every ordering question between a releaser and a waiter is answered by cache
coherence -- the single total order that all atomic RMWs of one location have --
rather than by a barrier. The two-location form of this protocol is a
store-buffer pattern that POWER9 needs a full `sync` on both sides to make
safe, and which x86 gets for free from its LOCK-prefixed RMWs. A naive port of
an x86 fast path would have been silently wrong here. There is no barrier in
the protocol because there is nothing to order.

**A bug this design caught, in the machine, before shipping.** The first build
let userspace release up to `max`, and `try_wake_any_sem()` takes a unit out of
the count before it has found a waiter to hand it to, putting it back if it
finds none. Userspace could fill the semaphore during that window and the
put-back then pushed the count to `max + 1` -- an over-signal. The self test hit
it within seconds and the kernel `WARN_ON_ONCE` fired. The fix is `NTSYNC_F_HOLD`,
set in the *same CAS* as the take: userspace declines to fast release while the
loan is outstanding, so during the window the count can only fall, never rise,
and the put-back provably fits. This is precisely the failure mode the whole
exercise was meant to avoid -- rare, silent, and it would have been blamed on
something else entirely months later.

`ppc64le/kernel/ntsync-fastpath/fastpath-test.c` is the test that caught it.
It checks conservation (every unit released is acquired exactly once), mutual
exclusion (a max-1 semaphore never hands the unit to two threads), lost wakeups,
and the wait-all lockout. All pass against the fixed module: 3.2M units through
32 threads with zero loss, 1.45M mutex rounds with peak holders 1, 300k releases
with zero lost wakeups.

## Verified

- **Default-off is not merely unused, it is unreachable.** With the variable
  unset the live game had 90 open ntsync fds and **zero** mapped pages; with it
  set, pages appear. Nothing is mapped, so `sync->shm` is NULL and every call
  site falls through to the ioctl path that was there before.
- **ABI is additive in both directions.** Old userspace never calls `mmap()`.
  New userspace against the stock module gets `ENODEV` and stays on ioctls --
  confirmed by running the test suite against the stock module. No existing
  ioctl changed number, argument or behaviour. The "wake only" operation the
  release path needs is `NTSYNC_IOC_SEM_RELEASE` with a count of zero, which
  already existed.
- **POWER8 legality.** The module compiles to `ldarx`/`stdcx.`, `lwarx`/`stwcx.`,
  `hwsync`, `isync` -- all ISA 2.07. No ISA 3.0 instruction is emitted.

## Not verified

- **No perf census with the fast path on.** `perf` needs root. The userspace
  counters above are a direct substitute and are better targeted, but they count
  only the paths the fast path touches, not total system ioctls.
- **Long-run stability.** The correctness argument is written out and the stress
  tests pass, but nothing has run for hours. The failure mode this design guards
  against is by construction one that short runs do not show.
- **Events.** 20.5% of ioctls are `EVENT_SET`/`EVENT_RESET` and are untouched.
  Given the 8.8% acquire hit rate, extending to events is unlikely to pay, and
  auto-reset events and `PULSE` carry spurious-wakeup hazards that semaphores
  do not.
- **Fairness.** The acquire fast path lets a running thread take a unit ahead of
  a thread already queued in the kernel. NT does not promise wakeup order and no
  unit is lost, but the wakeup discipline is genuinely different from the ioctl
  path, which hands queued waiters strict priority.
- **Memory footprint.** One page per ntsync object, allocated eagerly. At ~1,160
  objects per run that is ~4.6 MB on this 4K-page kernel; it would be ~74 MB on a
  64K-page kernel.

## Addendum (same day, other seat): a wait-order bug fixed, and the spin the negative result does not kill

Review of the merged fast path found one real defect, latent while default-off:
`fast_acquire_semaphore()` returned plain FALSE for two states that are not
interchangeable.  Count-zero is proof the object was unsignalled at the CAS's
point in the word's coherence order -- a wait-any may walk past it.
NTSYNC_F_NO_TOUCH is not proof of anything: the object may be signalled and
merely off limits while a wait-all is queued, and walking past it let
`inproc_wait()` return index 1 while object 0 was signalled, which
NtWaitForMultipleObjects documents never happens.  Fixed by a three-way result
(TOOK / EMPTY / KEEPOUT); KEEPOUT and an unmapped object now stop the walk and
fall to the kernel, which checks in order under the lock.  Needs a concurrent
wait-all on the same semaphore to trigger, which is why the stress suite --
all single-guarantee runs -- never saw it.

Second: the one lever this file's own negative result leaves standing.  91% of
acquires find the count empty *at the instant of the call*, but in a handoff
the release is often only microseconds behind, and sleeping then costs two
scheduler round trips.  `inproc_wait()` now spins on the mapped word(s) for a
bounded budget before the wait ioctl -- `WINE_PPC64LE_NTSYNC_SPIN=<us>`,
default 5, 0 disables, only armed when the fast path itself is on.  Spinning
runs at low SMT priority (`or 1,1,1`), only when every object in the wait is a
mapped semaphore that just read empty (so nothing signalled is kept waiting),
takes only through the same CAS as the walk, and bails to the kernel on any
NO_TOUCH sighting.  Zero timeouts stay polls; nonzero timeouts stretch by at
most the budget.  Statistics mode grew a fifth counter: acquires taken inside
the spin, printed as "(N taken in spin)" -- that number against the acquire
miss count is the whole verdict on whether the release-to-wait gap is really
microseconds.  Whether it pays is judged by feel on the live titles, per the
owner's standing preference; the counters are there if the answer needs a why.

## What this says about the next lever

The census sized the *opportunity* correctly and the *mechanism* wrongly. 91% of
ntsync ioctls being contended handoffs means the cost is not entering the kernel
-- it is the scheduler round trip, the wakeup latency, and whatever the woken
thread then waits on next. Removing the syscall around a real block buys
nothing, which is the narrower claim the previous session's negative result had
already left standing. This one closes it: it is not that syscalls are cheap,
it is that these particular syscalls are not redundant.
