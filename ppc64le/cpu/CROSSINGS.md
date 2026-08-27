# Counting guest/native crossings

`ppc64le/NEXT.md` item 6 measured what the GameThread's time is spent on and
found that only ~37% of it is JIT'd guest code; the rest is the price of
crossing the guest/native boundary.  A perf profile can say what a crossing
COSTS.  It cannot say how often each named call crosses, because it attributes
samples to the callee and never counts the crossings that reached it.  This is
the other half: **WINE_PPC64LE_TRAP_STATS**, a per-call-site crossing counter.

```sh
WINE_PPC64LE_TRAP_STATS=/tmp/xstat/cp2077 <launch the game as usual>
```

Off unless that variable names a path.  Every process in the session writes
`<path>.<pid>` — a game launch is a whole Wine session and the game is never
the last process to exit, so a shared path measured a service.  Take the
biggest file.

## What is counted

| class | one row per | counted at |
|---|---|---|
| `flat` | spec2thunk flat export, `module.Export` | `find_guest_thunk_target`, both cache paths |
| `com` | winecom vtable slot, `Interface::Method` | the same place; the name comes from the owning module's `__wine_com_slot_name` |
| `syscall` | syscall number, resolved to its name at dump | `__wine_syscall_dispatcher`, into the service table's own `CounterTable` |
| `callback` | guest callback target, `module+offset` | `guest_callback_run*`, the native→guest direction |
| `event` | guest faults, nested runs, unresolved traps | where each happens |

Two things are deliberately NOT rows, and both would be double counting:

* **The user-mode callback pair.**  `call_user_mode_callback` /
  `NtCallbackReturn` is the mechanism `emu_trap_dispatch` is entered and left
  by, so it is exactly 1:1 with `flat`+`com`.  It shows up anyway, as the
  `NtCallbackReturn` syscall row, and the measured run confirms the identity
  to the crossing: 99,787,100 NtCallbackReturns against 99,786,639 flat+com
  traps, the 461 difference being the guest faults and genuine user-mode
  callbacks that leave through the same door.
* **Bridge state sync and TLS resolution**, the ~14% and ~7% of NEXT.md item
  6.  Those are per-crossing work inside fastppcx86's bridge and glibc, not
  crossings of their own; the counts here are what they are per-crossing work
  FOR.

The native->guest direction is complete rather than sampled: every entry into
guest code goes through `call_guest_function` (DllMain, TLS callbacks, SEH
filters and handlers, callback trampolines, winecom's reverse-proxy slots --
which use the same trampoline pool), so the `nested guest run` event row is
the total and the `callback` class is its named subset.

The 32-bit lane's `int 0x80` sink is uncounted because no title reaches it yet
(NEXT.md item 2).

## What it costs

One `__ATOMIC_RELAXED` add per counted event, against a row index the site
resolved once — flat and COM sites carry theirs in the thunk RIP cache entry beside the
answers they already cache.  No locks, no strings, no allocation on any
counting path.  Interning a new row takes a spin lock, which happens once per
distinct call site.  Syscalls count into `SYSTEM_SERVICE_TABLE::CounterTable`,
a field Wine already has and always left NULL, so the dispatcher pays one load,
one compare and one not-taken branch when the sink is off.

**It is not free, and the number is here rather than a claim that it is.**
[MEASURED] 2026-08-27, Cyberpunk `-benchmark`, armed and unarmed runs
interleaved to control for the pipeline cache warming across a session:

| armed | 18.47 | 18.74 | 18.25 | 18.37 | 18.47 | mean **18.46** |
|---|---|---|---|---|---|---|
| **off** | 20.00 | 19.54 | 19.48 | | | mean **19.67** |

**6.2% of frame rate while armed**, well outside this title's
run-to-run spread, so it is a real cost and not noise.  It buys two relaxed
atomic adds per guest call at 3.5M counted events/s — the trap's own row, and
the dispatcher counter for the syscall that call makes — on cache lines
several threads write.

That cost does **not** bias the ranking, which is what the sink is for: every
counted event pays the same one add, so the shape of the table and every
ratio in it are unaffected.  Only the absolute per-second column is conservative, by
about the same 6.2%.  The way to remove it, if someone ever needs the
absolute rates exact, is per-thread row arrays summed at dump time; that
trades 128 KB a thread and an aggregation pass for the atomics.

## Reading a run

The counter runs from a process's first crossing to its last, so a
`-benchmark` process's whole-life rates are diluted by ~2.5x of loading.  The
sink rewrites its file every ~1M crossings per thread (and on `SIGUSR2`), so
two dumps subtract:

```sh
ppc64le/cpu/xstat_window.py EARLY LATE [-n TOP]
```

`crossings-cp2077-benchmark.txt` beside this file is that subtraction over the
flythrough of a 2026-08-27 run, and is the table NEXT.md item 6's second lever
asked for.

## What the first measurement said

Over the 66.6 s flythrough at 18.25 fps: **235.6M crossings, 3.54M/s**, in
217 named rows — **196,516 crossings per frame**.

| crossing | per second | per frame | note |
|---|---:|---:|---|
| `QueryPerformanceCounter` | 256,638 | 14,061 | one trap AND one `NtQueryPerformanceCounter` each |
| `SetGraphicsRootDescriptorTable` | 176,021 | 9,644 | |
| `CopyDescriptors` | 164,319 | 9,003 | |
| `PeekMessageW` | 100,526 | 5,508 | one trap AND one `NtUserPeekMessage` each |
| `CreateConstantBufferView` | 95,340 | 5,224 | |
| `GetGPUVirtualAddress` | 94,618 | 5,184 | a getter on a resource the guest already holds |
| `DrawIndexedInstanced` | 78,318 | 4,291 | |
| `EnterCriticalSection`/`LeaveCriticalSection` | 69,862 / 69,861 | 3,828 each | no syscall behind them when uncontended |

* **QPC is the single hottest named crossing** — 14,061 calls per frame, each
  a full trap plus a full syscall.  Windows serves it from
  KUSER_SHARED_DATA in user space.  **This row is now gone**; see below.
* **PeekMessage is second among flat exports**, 5,508 per frame, likewise a
  trap plus a syscall.
* **GetTickCount is NOT hot**: 377/s over the whole process, 0 in the
  flythrough window.  The `win32u get_tick_count` in the perf profile is
  inside `NtUserPeekMessage`, not a crossing of its own.  That half of the
  working hypothesis is dead.
* **Callbacks are not a hot class at all**: ~18/s.  One row carries almost all
  of it (`Cyberpunk2077.exe+0x7cb6bc`, a window procedure).
* **The COM class is 55M crossings, 825k/s** — larger than the flat class, and
  the top four rows are d3d12 descriptor and draw-state calls.

## What was done about the top row (2026-08-27)

QPC is gone.  `KERNEL32.QueryPerformanceCounter`, `NtQueryPerformanceCounter`
and `QueryPerformanceFrequency` do not appear in the flythrough window at all
any more, because a guest calling them no longer crosses anything: the export
in the guest's kernel32 is real x86-64 that reads the POWER timebase with
`rdtsc` and returns.  `ppc64le/cpu/crossings-cp2077-qpc.txt` is the A/B, one
binary and one env var apart.

| | before | after |
|---|---:|---:|
| `KERNEL32.dll.QueryPerformanceCounter` | 198,735/s | — |
| `NtQueryPerformanceCounter` | 198,735/s | — |
| all crossings, per frame | 187,633 | 152,718 |

The mechanism is three pieces and each is documented where it lives:

* `include/wine/emu_qpc.h` — the clock.  Why the timebase, why the guest and
  native answers are the same expression rather than two clocks that agree,
  what the emulator's TSC scale is and how the host measures it without asking
  the emulator, and the 39.6 ppm the timebase drifts from CLOCK_BOOTTIME
  (which is why `server_monotonic_time()` now exists).
* `tools/spec2thunk` — `FAST_PATH_EXPORTS`, the mechanism for giving one
  export real guest code beside its stub array while the stub, the stride and
  the trap offset stay exactly what the dispatcher expects.
* `dlls/ntdll/signal_ppc64.c` — `qpc_arm_module`, which fills the guest's
  block on the first call.

`ppc64le/cpu/check-qpc-fastpath.sh` is the gate, and its negative control
breaks the seeding two different ways.

**What this does not touch.**  `PeekMessageW` is now the hottest flat row at
120,708/s and it is not shared-page servable — it has real message-queue
semantics.  `GetSystemTimePreciseAsFileTime` is 2,489/s and reads the realtime
clock, which NTP can step; the timebase cannot serve it without a published
realtime epoch that moves under a reader.  The COM class did not move and is
now the largest by far; those rows want a batching design, not a fast path.
