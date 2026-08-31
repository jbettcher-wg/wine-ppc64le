# Where the syscalls go: 91% of every ioctl is ntsync

**[MEASURED 2026-08-31, Cyberpunk 2077 benchmark, native lane, AC922 POWER9.]**

This is the first measurement on this port whose *magnitude matches the thing it
is trying to explain*. Twelve tuning experiments the previous day moved nothing;
this one says why they could not.

## The census

`perf record -e syscalls:sys_enter_ioctl -a` for 10 s while the built-in
benchmark rendered. 882,236 samples, **88,224 ioctls/sec**. Idle floor measured
before launch: 4,725/sec.

The ioctl magic byte is bits 8-15 — `0x4e` is `'N'` (ntsync,
`include/linux/ntsync.h`), `0x64` is `'d'` (DRM).

| count | cmd | operation | share |
|---:|---|---|---:|
| 313,046 | `0xc0284e82` | `NTSYNC_IOC_WAIT_ANY` | 35.5% |
| 304,072 | `0xc0044e81` | `NTSYNC_IOC_SEM_RELEASE` | 34.5% |
| 91,946 | `0x40044e88` | `NTSYNC_IOC_EVENT_SET` | 10.4% |
| 89,025 | `0x40044e89` | `NTSYNC_IOC_EVENT_RESET` | 10.1% |
| 3,189 | `0x80084e87` | `NTSYNC_IOC_CREATE_EVENT` | 0.4% |
| | | **ntsync total** | **90.8%** |
| 32,788 | `0x80206445` | DRM | 3.7% |
| ~42,700 | `0x…64…` | DRM, remainder | ~4.9% |
| | | **GPU total** | **~8.6%** |

**Graphics submission is under 9% of all ioctls. Synchronisation is 91%.**

At the run's ~16.9 fps that is **~4,740 ntsync ioctls per frame**, and
`WAIT_ANY` alone is ~1,850 per frame.

## Why this matters

The co-developers' proposal is a userspace fast path: an already-signalled
semaphore acquire should be a shared-memory count and an atomic decrement, with
the ioctl only when the count is empty. **The two operations that dominate this
census — `WAIT_ANY` and `SEM_RELEASE`, 70% of all ioctls between them — are
exactly the pair that fast path removes.**

Against the measured 34-38 ms frame floor and the ~17 ms gap to the emulated
lane:

    half of WAIT_ANY already signalled   ~925 syscalls/frame   1.9-2.8 ms
    most of WAIT_ANY + SEM_RELEASE     ~2,500-3,600/frame      5-11 ms

That is the largest identified lever on this port, and it is their patch rather
than a new idea.

## What is NOT established

- **The fast path's hit rate.** What fraction of `WAIT_ANY` calls find the
  object already signalled is unmeasured, and it is what turns the range above
  into a single number. Answerable by instrumenting the kernel module or
  `dlls/ntdll/unix/sync.c`.
- **The per-ioctl cost on this hardware.** The 2-3 us figure above is assumed,
  not measured. A syscall-latency microbenchmark would tighten every number here.
- **That eliminating them recovers frame time proportionally.** Removing a
  syscall from a thread that then simply waits elsewhere buys nothing.

## Context: enabling ntsync alone did nothing

`/dev/ntsync` did not exist on this box until 2026-08-30. The module was built
out-of-tree from `drivers/misc/ntsync.c` against `~/Development/linux-7.2.2`
and inserted live, no reboot. The tree was **already** ntsync-ready
(`HAVE_LINUX_NTSYNC_H 1`, `server/inproc_sync.c:50` opens the device), so every
prior run had that `open()` failing `ENOENT` and silently falling back to
server-side sync.

With it enabled the path is **provably hot** — a live wineserver holds
~1,160 ntsync fds per run — and the benchmark did not move: three legs at
16.355 / 16.747 / 17.526 fps, floors 35.49 / 36.63 / 34.03, inside a same-day
baseline band of 15.90-17.47 and 34.03-38.15. The legs climb monotonically,
which is warm cache rather than a mechanism.

That is the strongest negative available: **the mechanism was verified active
rather than assumed.** It says swapping wineserver round trips for one ioctl
per acquire is a wash, which leaves only the narrower claim — that the cost is
entering the kernel *at all* — standing. This census sizes that claim.
