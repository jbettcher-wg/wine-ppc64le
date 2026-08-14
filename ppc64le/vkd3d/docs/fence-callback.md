# Can a native host thread signal an emulated guest thread across the FEX boundary?

**Date:** 2026-08-13
**Emulator under test:** `fastppcx86` @ `e3589066d` ("ThunkLibs: stop common/Host.h dragging FEXCore into every consumer", 2026-08-13)
**Machine:** AC922 `witherspoon-arkamedes`, kernel 7.2.0-rc7, ppc64le
**Experiment:** `powerpc64le-ports/vkd3d-ppc64le/experiments/fence-callback/`

---

## Verdict

**Not a blocker. Not even a design constraint on the fence path — it is a non-problem, provided the crossing is a kernel wakeup rather than a control transfer.**

The hypothesis in the brief is **confirmed by measurement**. A native ppc64le
pthread living inside the FEX process — one FEX has never heard of, holding no
`ThreadObject`, not inside any guest→host call — woke emulated x86-64 guest
threads **224,480 times across 27 runs with zero lost, zero duplicated, and zero
misdirected wakeups**, at a mean latency of **5.6–6.9 µs** and a sustained rate
of **1.12 million wakeups/second**. Nothing died. This held through `read(2)`,
`poll(2)`, `epoll_wait(2)`, private and shared `FUTEX_WAIT`, and — the surprise —
`tgkill(2)` delivering a signal that **ran the guest's own signal handler**.

The `ERROR_AND_DIE` on record is **accurate but far narrower than the concern
assumed**. It forbids exactly one thing: a host thread **entering JIT-compiled
guest code** through FEX's trampoline machinery. It has nothing to say about a
host thread doing a `write()` on an fd, and it never executes on that path.

This means `vkd3d_fence_worker_main` **can stay exactly as upstream wrote it**.
`vkd3d_native_sync_handle_signal()` is a `write()` on the app's fd; that is the
operation measured here, at the same rate and in the same process shape, and it
works. §4.3's proposed inversion is **not required for the eventfd path** and
should not be built for that reason. It is still required for one narrow reason —
see §5 — but that reason is Wine's, not FEX's.

| Question | Answer | Basis |
|---|---|---|
| Can a native host thread signal a parked emulated guest thread? | **Yes** | `[MEASURED]` §2 |
| Is the `ERROR_AND_DIE` claim accurate? | **Yes, and it is narrow** | `[CODE]` §1 |
| Does it forbid the fence path? | **No — the fence path never reaches it** | `[CODE]` §1 + `[MEASURED]` §2 |
| Can a host thread signal a *Wine* event directly? | **No, not on this box** | `[CODE]` §3 |
| Does DXVK already solve this in-tree? | **No — D3D11's `SetEvent` is a silent stub** | `[CODE]` §4 |
| Is a genuine host→guest *call* unavoidable? | **No** | §5 |

---

## 1. The `ERROR_AND_DIE` claim `[CODE]`

**Exact site.** `fastppcx86/Source/Tools/LinuxEmulation/Thunks.cpp:229-231`:

```cpp
static void CallCallback(void* callback, void* arg0, void* arg1) {
  if (!ThreadObject) {
    ERROR_AND_DIE_FMT("Thunked library attempted to invoke guest callback asynchronously");
  }
```

**What `ThreadObject` is.** `Thunks.cpp:149` — `static thread_local FEX::HLE::ThreadStateObject* ThreadObject {};`.
It is assigned in exactly **one** place, `Thunks.cpp:268-270` (`RegisterTLSState`),
whose only callers are guest-thread creation paths:
`LinuxSyscalls/Syscalls/Thread.cpp:129` and `:327` (guest `clone`), and
`FEXInterpreter/FEXInterpreter.cpp:629` (the initial guest thread), via
`LinuxSyscalls/Syscalls.cpp:1347-1349`.

**Therefore, precisely:** any thread that FEX did not create as a guest thread has
`ThreadObject == nullptr` forever. A native pthread spawned by a host library is
such a thread.

**What it forbids.** `CallCallback` is reached only through a host trampoline
(`Thunks.cpp:486` installs `&ThunkHandler_impl::CallCallback` into
`TrampolineInstanceInfo`; `:529` asserts it). Its body ends at `:252`
`CTX->HandleCallback(ThreadObject->Thread, (uintptr_t)callback)` — it sets guest
GPRs and enters the JIT. **So the prohibition is on a host thread executing guest
code, and nothing else.** The same `!ThreadObject` guard appears on
`GetGuestStack`/`MoveGuestStack` (`Thunks.cpp:720-721`, `:728-729`) — again, only
on operations that presuppose a guest register/stack context.

`write(2)` on an eventfd touches none of this. There is no path from a host
library's `write()` to `CallCallback`. The concern on record conflated
"host thread notifies guest thread" with "host thread calls guest code"; only the
latter is forbidden.

`[MEASURED]` §7's second control demonstrates the forbidden thing really is fatal,
so this is not an argument that the guard is toothless.

---

## 2. The experiment `[MEASURED]`

### Shape

Two artifacts, in the same process, mirroring the real fence path:

- **`host_worker.c`** → `libfencehost.so`, **native ppc64le**, `LD_PRELOAD`ed into
  the `FEX` binary. Spawns pthreads that are ordinary native threads: FEX never
  registers them, they never enter guest code, they never make a thunk call.
  Their signalling operation is `write(fd, &one, 8)` — byte-for-byte
  `vkd3d_native_sync_handle_signal()`.
- **`guest_waiter.c`** → `guest_waiter`, **x86-64**, built by
  `x86_64-linux-gnu-gcc 15.2.0`, run under FEX. It plays the D3D12 app: it calls
  `eventfd(2)` itself and owns the fds, exactly as an app does when it casts an
  eventfd to a `HANDLE` for `SetEventOnCompletion`, then parks threads on them.

Rendezvous is a file carrying the guest's pid and the address of a control block
in guest anonymous memory. The host thread then reads and writes that guest
memory directly — which itself established a load-bearing fact:

> `[MEASURED]` **The host thread read the guest's control block and validated its
> magic, and its stores into guest memory were observed by the guest**
> (`host_probe=deadbeefcafe` read back by guest code every run). FEX's guest
> address space is the host address space; no translation, no copy. The eventfd
> **numbers are identical on both sides** — the host wrote to fds 4, 7, 8, 9 as
> reported by the guest and the correct guest threads woke. FEX does not
> renumber fds.

### Results

All runs: `env -i` with a private `HOME`, RootFS `Ubuntu_24_04`, no thunks
configured, `timeout -s KILL 90`.

| Phase | Guest wait primitive | Waiters × wakes | Result | Notes |
|---|---|---|---|---|
| `pingpong` | blocking `read(2)` | 1 × 1000 | **PASS** | mean **6.9 µs**, min 4.8, max 641.5 |
| `pingpong` +6 load threads | blocking `read(2)` | 1 × 1000 | **PASS** | mean **5.6 µs**, max 100.7 |
| `fanout` | blocking `read(2)` | 32 × 200 = 6400 | **PASS** | 30.5 ms, 210 k/s |
| `fanout` | blocking `read(2)` | 64 × 500 = 32000 | **PASS** | 42.9 ms, 746 k/s |
| `load` | `read(2)` + 8 JIT-churn threads | 64 × 500 = 32000 | **PASS** | 28.6 ms, **1.12 M/s** |
| `poll` | `poll(2)` | 8 × 100 = 800 | **PASS** | |
| `epoll` | `epoll_wait(2)` | 8 × 100 = 800 | **PASS** | |
| `futex` | `FUTEX_WAIT_PRIVATE` on guest memory | 1 | **PASS** | host store + `FUTEX_WAKE_PRIVATE` returned **1 woken**; 892 µs from idle |
| `futexsh` | `FUTEX_WAIT` (shared) | 1 | **PASS** | 880 µs from idle |
| `signal` | blocking `read(2)`, host `tgkill` | 1 | **PASS** | **guest SIGUSR1 handler ran**; `read` returned `EINTR`; 350 µs |
| soak | `load`, 15 consecutive runs | 32 × 300 × 15 = **144 000** | **15/15 PASS** | zero failures |

**Total: 224,480 host→guest wakeups, zero lost.**

The no-lost-wakeup claim is not a liveness check. eventfd has counter semantics,
so the guest sums the `uint64` values it reads and asserts the total equals the
number of `write()`s the host performed, **per fd**. §7's third control shows this
assertion detects a shortfall of 3 in 20.

### Two findings worth calling out beyond the hypothesis

1. **`[MEASURED]` Futex wakes cross the boundary.** The guest's `FUTEX_WAIT`
   (x86-64 `SYS_futex` 202, emulated) and the host's `FUTEX_WAKE` (ppc64le
   `SYS_futex` 221, native) met on the same address in the same kernel futex
   queue, and `FUTEX_WAKE` reported **1 thread woken**. FEX does not virtualize
   futexes. Both private and shared variants worked. This is the fsync-style
   shape, and it is available if it is ever wanted.

2. **`[MEASURED]` A native thread *can* cause guest code to run asynchronously —
   via a signal.** `tgkill()` from the native worker to a guest thread's tid
   caused FEX to deliver SIGUSR1 to the guest, **the guest's own x86-64 handler
   executed**, and the blocked `read(2)` returned `EINTR`. This does not
   contradict §1: the signal is *received* on a thread FEX registered
   (`SignalDelegator::RegisterTLSState`, `LinuxSyscalls/SignalDelegator.cpp:1852`),
   so full TLS context exists at the point guest code is entered. It is a real,
   working async host→guest *code entry* — just not one that runs on the host
   thread. It is a fallback if a callback is ever genuinely needed, at ~350 µs and
   with all the usual async-signal-safety constraints.

---

## 3. How Wine fits — the one place the answer is "no" `[CODE]`

Investigated in `hangover-ppc64le/wine-upstream` (Wine **11.15**, upstream, the
source for `wine-build` per `wine-build/config.status:398`).

**There is no esync and no fsync in this tree.** No `esync.c`, no `fsync.c`, no
`WINEESYNC`/`WINEFSYNC` anywhere. Only two backends exist:

1. **`/dev/ntsync` in-process sync.** `dlls/ntdll/unix/sync.c:63-64`, `:311-450`.
   Waiting is `ioctl(device, NTSYNC_IOC_WAIT_ANY, ...)` (`sync.c:431`); signalling
   is `ioctl(event_fd, NTSYNC_IOC_EVENT_SET, &prev)` (`sync.c:333-339`). Compiled
   in — `wine-build/include/config.h:205` `#define HAVE_LINUX_NTSYNC_H 1`.
   This **is** a pure kernel operation a raw native pthread could perform.
2. **wineserver.** The waiter blocks in `read(wait_fd[0], ...)`
   (`dlls/ntdll/unix/server.c:357`); the only writer is the *wineserver process*
   (`server/thread.c:1175`). Reaching it requires `wine_server_call`, which
   dereferences `pthread_getspecific(thread_data_key)`
   (`dlls/ntdll/unix/unix_private.h:128-131`) — **NULL on a thread Wine never
   created**, i.e. a segfault. **A native pthread cannot signal a
   wineserver-backed event at all.**

Which backend is live is decided by whether the wineserver can
`open("/dev/ntsync")` (`server/inproc_sync.c:47-52`, `server/event.c:76-87`).

> `[MEASURED]` **On the AC922 today, `/dev/ntsync` does not exist.** The module is
> built (`/lib/modules/7.2.0-rc7/kernel/drivers/misc/ntsync.ko`) but not loaded.
> Loading it is `sudo modprobe ntsync` — **not run**, needs root.

Even with ntsync loaded, the per-handle fd lives in a `static` cache private to
`sync.c` (`struct inproc_sync`, `sync.c:536-556`; fill via a server round-trip at
`:667-688`) with no exported accessor. Extracting it means either replicating
internal index math or adding a wine unixcall.

**Conclusion for Wine:** signalling a Wine event from a raw native thread is
either impossible (wineserver) or requires a kernel module that is not loaded plus
poking at Wine internals (ntsync). **Do not design for it.** The corollary is that
§4.3's inversion survives, but for a Wine reason rather than a FEX reason: the
thread that finally calls `SetEvent(hEvent)` must be a *Wine* thread. It does not
have to be a *guest* thread — Wine's `ntdll` is native ppc64le in this build
(`--without-mingw`, `PE_ARCHS =` empty; `current_machine = IMAGE_FILE_MACHINE_POWERPC64`,
`dlls/ntdll/unix/unix_private.h:52-53`), so a *native* thread created through
`RtlCreateUserThread` (thus owning a TEB) can call `NtSetEvent` directly. That is
strictly cheaper than parking a guest thread.

> ### `[MEASURED, 2026-08-14]` Correction: it is thread *data*, not a TEB
>
> §3's "thus owning a TEB" is the wrong property, and a guard written against it
> refuses the arrangement that works. Two corrections, both from running the
> thing rather than reading it:
>
> 1. **`RtlCreateUserThread` is not reachable from a host library.** It lives in
>    the PE-side ntdll; `ntdll.so` — the unix side, which is what a `dlopen`'d
>    host thunk library can `dlsym` — does not export it. It exports
>    **`PsCreateSystemThread`** (`dlls/ntdll/unix/thread.c:1615`), which is the
>    unix-side "make me a native thread Wine has registered" entry point and is
>    what this port uses.
> 2. **That thread has no TEB, and does not need one.**
>    `server_init_thread` (`dlls/ntdll/unix/server.c:1785-1815`) calls
>    `pthread_setspecific(thread_data_key, data)` for *every* thread it starts,
>    but a system thread is created with `data->teb == NULL` and takes the branch
>    at `:1814` that calls the entry point directly. `NtCurrentTeb()` is
>    `data ? data->teb : NULL` (`thread.c:1806-1811`), so it returns **NULL** on
>    such a thread — while `wine_server_call`'s
>    `pthread_getspecific(thread_data_key)` is populated. The property
>    `NtSetEvent` requires is the second one.
>
> `[MEASURED]` `dxvk-ppc64le/probes/wine_event_relay.cpp`, inside a real
> `wine-build` process on the AC922: a Wine event created with `NtCreateEvent`
> (handle `0x70`), bound by the host translator, written from an ordinary
> pthread, and relayed by the `PsCreateSystemThread` waiter →
> `NtSetEvent` `STATUS_SUCCESS`, `NtWaitForSingleObject` `STATUS_SUCCESS`, with
> `NtCurrentTeb() == NULL` on the relay thread throughout. With
> `DXVK_THUNK_NO_GUEST_EVENTS=1` the same run gives `STATUS_TIMEOUT`.
>
> The first version of that translator guarded the waiter on `NtCurrentTeb()`
> being non-NULL, exactly as §3's wording implies, and refused to signal
> anything. `NtCurrentTeb()` is still used, but only on the *caller* — where it
> is a sufficient (not necessary) test that the thread is one Wine created, and
> the only such test `ntdll.so` exports.

---

## 4. DXVK's existing thunk — no precedent, and a warning `[CODE]`

There is **no** host→guest notification anywhere in the working D3D11 path. The
entire FEX-visible surface is four scalar-only guest→host functions
(`dxvk-ppc64le/pe-shim/libdxvk_d3d11_interface.cpp:32-38`). Zero uses of
`MakeHostTrampolineForGuestFunction`/`CallbackUnpack`/`RegisterCallbackUnpacker`;
zero `PFN_` parameters in the 2593 modelled vtable slots; zero `pthread_create` in
any thunk (`fastppcx86/ThunkLibs/`, `dxvk-ppc64le/thunk/runtime/`, `pe-shim/`).
Waits work because the guest thread blocks *inside* a synchronous thunk call
(`dxvk_fence.cpp:100-111`, `vkWaitSemaphores`) and never leaves it.

**The warning.** D3D11's one async-notification API is not solved — it is
**silently broken**. `d3d11_fence.cpp:118-129` enqueues `[hEvent]{ SetEvent(hEvent); }`
onto a real host fence thread, and on this build `SetEvent` resolves to
`util_win32_compat.h:43-46`:

```cpp
inline BOOL SetEvent(HANDLE hEvent) {
  dxvk::Logger::warn("SetEvent not implemented.");
  return FALSE;
}
```

`d3d11_fence.cpp.o` is in the native build, so this is live code. D3D11 apps
mostly tolerate it. **D3D12 will not** — `SetEventOnCompletion` is the primary
frame-pacing mechanism. If the D3D12 shim is built on the same compat header
without checking, it will inherit a stub that reports success-shaped nothing.
Given this project's history with checks that test nothing, that stub is the most
likely way this hazard actually bites, and it has nothing to do with FEX.

FEX's own Vulkan thunk **refuses** rather than solves: it overwrites debug
callbacks with a host dummy (`ThunkLibs/libvulkan/Host.cpp:492-503`), strips
`VkDebugUtilsMessengerCreateInfoEXT` from `pNext` chains (`:314-336`, with the
comment *"if native libvulkan invokes them, it runs guest code as host code and
SEGVs"*), and forces `VkAllocationCallbacks` to `nullptr` (`:505-512`).

---

## 5. Is a genuine callback unavoidable? **No.**

Nowhere in the fence path does a host thread need to enter guest code.

Recommended shape, in ascending cost — pick the first that fits:

1. **Guest owns the eventfd, host writes it.** Upstream vkd3d unchanged. The PE
   shim creates an `eventfd`, hands it to `SetEventOnCompletion` as the `HANDLE`,
   and the fence worker `write()`s it. Guest waits with `read`/`poll`/`epoll`.
   **`[MEASURED]` works, 5.6 µs, 1.12 M/s.** Use this whenever the shim controls
   the object being waited on.
2. **App-supplied Win32 event.** The app hands a real Wine `HANDLE`. Then the shim
   must translate: register `(fence, value, hEvent)` shim-side, do **not** pass
   `hEvent` down, and have a thread that owns a TEB call `SetEvent(hEvent)` on
   completion. Per §3 that thread can be a **native Wine thread**
   (`RtlCreateUserThread`), which is cheaper than §4.3's parked *guest* thread —
   `ntdll` is native here. §4.3's design also works; it is just heavier.
3. **Signal.** `tgkill` a guest thread. **`[MEASURED]` works**, ~350 µs. Listed
   only as evidence that even the hard case has an escape hatch. Do not build on it.

The residual risk in §4.3 — *"a blocking host call holds a FEX guest thread;
whether that interacts badly with FEX's thread management is unknown"* — was **not
tested here** and remains open, because options 1 and 2 make it moot. If option 2
is implemented with a guest service thread rather than a Wine native thread, that
risk needs its own experiment.

---

## 6. Falsifiability: the harness catches the bad outcome `[MEASURED]`

Three controls, each producing a *different* failure signature.

**Control A — an fd that is never signalled.** The host is told to skip one fd.

```
RESULT: FAIL phase=control-nosignal reason=watchdog-timeout secs=8
  fd[0]=4 acked=1 counted=10      fd[1]=7 acked=1 counted=10
  fd[2]=8 acked=0 counted=0       fd[3]=9 acked=1 counted=10
---- exit=2
```

Exactly the skipped fd is identified; the other three pass in the same run. A hang
is converted to a loud, attributable failure by a guest watchdog thread.

**Control B — the forbidden operation, for real.** The host thread branches
directly to a guest code address (`0x401680`, guest `main`) — a native ppc64le
thread executing x86-64 bytes, which is what an unmediated host→guest callback is:

```
HOST: NEGATIVE CONTROL: native thread 3118613 branching to guest code at 0x401680
timeout: the monitored command dumped core
... Illegal instruction   ---- exit=132
```

SIGILL, core dumped. The direction that is forbidden **is** fatal, and the harness
reports it as such rather than passing.

**Control C — lost wakeups, the assertion that matters.** `FENCE_HOST_DROP=3`
makes each host writer silently skip 3 of its 20 writes:

```
FENCE_NFD=4 FENCE_ROUNDS=20 FENCE_HOST_DROP=3 → RESULT: FAIL   counted=17 on all 4 fds
FENCE_NFD=4 FENCE_ROUNDS=20                   → RESULT: PASS   wakes=80
```

Identical parameters, one bit changed, opposite verdicts. The "every write is
accounted for" claim is a real measurement sensitive to a shortfall of 3 in 20,
not a liveness check that any wakeup would satisfy.

---

## 7. Reproducing

```bash
# on the AC922
R=~/Development/powerpc64le-ports/vkd3d-ppc64le/experiments/fence-callback/run.sh
$R build          # x86-64 guest + native ppc64le host lib; runs the POWER8 check
$R all            # every phase, both controls
$R pingpong       # one phase

# stress / controls
FENCE_NFD=64 FENCE_ROUNDS=500 FENCE_LOAD=8 $R load
FENCE_NFD=4 FENCE_ROUNDS=20 FENCE_HOST_DROP=3 FENCE_WATCHDOG=8 $R fanout   # must FAIL
```

`run.sh` writes a private `HOME` with its own `Config.json`, so the live
`~/.config/fex-emu/Config.json` (which names Vulkan/GL thunk libs and will
`SIGTRAP` every FEX invocation if they are absent) cannot affect results.

**POWER8 floor.** `objdump -d -Mpower8 libfencehost.so` → no `(bad)` encodings.
The build script performs this check and prints `POWER8 CHECK: pass`. Note this
covers the experiment's own host code only; it says nothing about vkd3d.

---

## 8. Claims here that your own testing could not disprove

These are the places where a re-run of my harness would agree with me and still be
wrong.

1. **The injection is `LD_PRELOAD`, not a thunk-`dlopen`.** My native threads are
   in the FEX process and unregistered — the property §1 turns on — but they were
   not created by a library FEX's thunk handler loaded, and the process never made
   a single thunk call. If loading a host library *through the thunk path* changes
   FEX's state in a way that matters (it should not; `write()` reaches no FEX
   code), my harness cannot see it. **The first real vkd3d thunk bring-up retests
   this for free; nothing needs to be built to close it.**

2. **vkd3d-proton's source is not in this tree.** `find` over the whole workspace
   returns no `vkd3d_native_sync_handle.h` and no vkd3d-proton checkout — only
   `vkd3d-ppc64le/docs/`. I modelled `vkd3d_native_sync_handle_signal()` as
   `write(fd, &one, 8)` **from the brief's description, not from its source.** If
   it is `eventfd_write` with different flags, a semaphore, or `EFD_SEMAPHORE`,
   the shape is close but not verified identical. Also unverified: that
   vkd3d actually accepts an arbitrary caller-supplied fd as `HANDLE` rather than
   one it minted itself.

   > `[RESOLVED, 2026-08-14]` The source is now at `vkd3d-ppc64le/src`
   > (`238f157e`). Both halves of the guess were right:
   > `vkd3d_native_sync_handle_release` is `write(handle.fd, &value, 8)` with
   > `value = count` (`vkd3d_native_sync_handle.h:99-107`), and `wrap()` did
   > accept an arbitrary caller-supplied fd with a bare cast — which is the
   > hazard `vkd3d-patches/0001` removes. One thing the model missed:
   > `_create` uses `EFD_SEMAPHORE` for the SEMAPHORE type, though only for
   > handles vkd3d mints itself.

3. **No Wine was in the loop, at all.** §3 is entirely `[CODE]`. I never ran a
   Wine process, never created a Wine event, and never confirmed that a guest PE
   calling `WaitForSingleObject` behaves as the source implies — the wine tree is
   owned by another agent and `make` there was off-limits. The claim "a native
   pthread segfaults in `server_call_unlocked`" is read, not observed.

4. **`/dev/ntsync` was never exercised.** The module exists and is unloaded. Every
   statement about the ntsync path is from source. If ntsync is loaded later, Wine
   silently switches backends and §3's conclusions shift — the ioctl path becomes
   signallable by a raw pthread. That flips a "no" to a "maybe" and is untested
   either way.

5. **Latency was measured on an idle machine over ~10 s bursts.** Real fence
   traffic is bursty, contends with three other agents' load, and runs for hours.
   I did not test: thermal/SMT contention, a host thread signalling *during* an
   SMC invalidation or a FEX thread-pause, guest `fork`, guest thread exit while a
   host write is in flight, or fd reuse after `close()`. The soak is 15 runs of
   ~30 ms each — that is a smoke test for stability, not a soak in any real sense.

6. **Every wakeup here was a wakeup the guest was already waiting for.** I never
   tested the race that actually bites in production: the host signalling in the
   window *between* the guest registering the wait and the guest entering the
   blocking call. eventfd's counter semantics should make that safe by
   construction, and my counter-sum assertion would catch a loss if it were
   provoked — but I did not provoke it. **This is the gap I would close first.**

7. **`ERROR_AND_DIE` was never actually observed firing.** §1 is a source
   argument, and Control B demonstrates the *underlying* fatality (SIGILL) rather
   than FEX's guard. Triggering the guard itself needs a thunk guest/host pair
   with an async callback, which meant building `ThunkLibs` in a shared tree — out
   of scope under the load constraints. I claim the guard is narrow; I did not
   watch it refuse anything.
