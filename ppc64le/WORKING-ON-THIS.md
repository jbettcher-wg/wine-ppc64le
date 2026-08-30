# Working on this port: the things that cost a session to learn

`README.md` says how to build; `PLAYING-GAMES.md` says how to launch a title
through Steam; `ppc64le/NEXT.md` is the work list and `ppc64le/games/STATUS.md`
the per-title board.  This file is the fourth thing: the operational knowledge
that is not derivable from the code, and that has cost real time more than
once.  Everything here is measured, and most of it was measured the hard way.

## Environment knobs that reach the emulator

`FEX_*` variables reach a native-lane guest.  Two pieces make that true and
both are recent:

* fastppcx86 `54df357cb` gave FexBridge a configuration layer, so the bridge
  reads the same `FEX_*` environment the standalone frontend does.  Before
  that every knob was silently inert for a native-lane guest.
* `ppc64le/steamtool/proton` stopped stripping `FEX_*` from the launch
  environment.  The strip predated the layer and made the whole class of knob
  disappear between the shell and the guest.  `stripped 0 variable(s)` in a
  run log is the tell that a launch kept them; `WINE_PPC64LE_STRIP_FEX_ENV=1`
  restores the old behaviour when a launch environment carries someone else's
  FEX settings.

What the bridge does NOT read is per-app `AppConfig` JSON: that is the
frontend's mechanism and the native lane's per-title mechanism is
`ppc64le/steamtool/appconfig/<appid>.env`.  Two per-title mechanisms with
different keys is how a setting gets lost, so there is deliberately one.

`FEX_HWTSO=1` works on the native lane as of fastppcx86 `d4168c1ec` plus this
tree's PROT_SAO wiring: the bridge probes SAO at process init, the JIT then
emits no TSO barriers, and every guest-reachable page carries the bit.
Native-machine images are exempt (`VPROT_NOSAO`) so only the emulation side
pays SAO's store-throughput cost.  To confirm it is live on a running guest:

```sh
grep -c ' ar ' /proc/<pid>/smaps      # nonzero = SAO pages present
```

powerpc reports VM_SAO as `ar` in `VmFlags` (it is VM_ARCH_1), and `strace`
prints the mmap/mprotect bit symbolically as `PROT_SAO`, not `0x10`.

## Measuring a change

Cyberpunk 2077 accepts `-benchmark`: a deterministic ~67-second flythrough
that writes `summary.json` and `frames.csv` under the prefix's
`Documents/CD Projekt Red/Cyberpunk 2077/benchmarkResults/`.  That is the A/B
harness — it beats eyeballing a scene, and it is how the depth-clear bug was
confirmed fixed and the performance matrix was built.

Measured on this box (POWER8, V620), one variable at a time:

| lever | effect |
|---|---|
| `ondemand` -> `performance` governor | +3% |
| NUMA unbound vs `WINE_PPC64LE_NUMA_NODE=0` | +4% at SMT4 |
| SMT4 -> SMT2, unbound | +9% more |
| node-0 bind **at SMT2** | **halves the frame rate — never combine** |

The frame rate is one thread: the game pulls ~15 cores while `GameThread`
sits pinned at ~92%, which is that thread's JIT throughput.  wineserver is not
the bottleneck (4.5% CPU, ~2k context switches/s).  The old "~2.5-core
ceiling" note in NEXT.md item 6 was measured on a much older tree and does not
reproduce.

## Traps

**`pgrep -f` and `pkill -f` match your own command line.**  Over one session
this produced: a monitor that reported a game running when it had exited, a
"cleanup" that killed the ssh session doing the cleaning, and a benchmark
batch that killed itself.  Match the exact binary (`pgrep -x witcher3.exe`) or
break the pattern (`pkill -f tmp/w3"loop".sh`).

**But `pgrep -x` silently matches nothing when the name exceeds 15
characters** — the kernel's `comm` is capped there.  `pgrep -x
quake2ex_steam.exe` (18) returns no match and prints its warning to *stderr*,
so in a script the empty result reads as "not running", which is the exact
wrong answer this trap was supposed to prevent.  Check the length first, and
for longer names match on the truncated comm (`pgrep -x quake2ex_steam`) or
anchor a `-f` pattern on the full path.

**`wine foo.exe` leaves two `wine-preloader` processes** whose command lines
both name the exe, and only one ever runs guest code.  A loop that keeps the
last match will sample the wrong one — this produced "zero SAO pages" from a
working build and an hour of kernel-side theorising.  Pick the pid whose
`/proc/N/smaps` actually shows what you are looking for.

**A Wine session outlives `flock`, and keeps the lock.**  Two agents share this
box through `flock /tmp/op4k-bench.lock <command>`, and a command that starts
Wine leaves `services.exe`, `winedevice.exe`, `rpcss.exe` and friends behind
when its wineserver goes -- orphans that INHERITED the lock's file descriptor.
The lock is held by whoever holds an fd on it, so the next `flock` blocks
behind processes whose command finished an hour ago, on an idle machine.
[MEASURED] 2026-08-27: 30 minutes of a gate sweep waiting on six of its own
predecessor's leftovers.  `fuser -v /tmp/op4k-bench.lock` names them, and
`ppc64le/reap-orphans.sh` clears them; run it after any sweep or benchmark
batch, not only when the load average complains.

**Screenshots on this box** need the Wayland compositor, not X: the rootless
Xwayland root window is black to `import` and `ffmpeg -f x11grab`.  What works:

```sh
DBUS_SESSION_BUS_ADDRESS=unix:path=/run/user/1000/bus \
XDG_RUNTIME_DIR=/run/user/1000 \
WAYLAND_DISPLAY=$(basename "$(ls /run/user/1000/wayland-[0-9] | head -1)") \
  spectacle -b -n -o shot.png
```

All three variables are required — `spectacle` dumps core without
`WAYLAND_DISPLAY`.  **Do not hardcode the socket name.**  This file said
`wayland-0` for a while and the live socket was `wayland-1`; the number is
assigned per compositor start, so a recipe that names one is a recipe that
core-dumps on the next boot.  Derive it, as above.

**Driving a game without a human**: `python3 ~/fex-scripts/sendkey.py KEY_ENTER
--delay 0.5` injects seat-wide through uinput.  Focus the game window first;
a tiny `_NET_ACTIVE_WINDOW` sender does that, and the gate suites' crash
probes raise real dialogs on the live desktop that steal focus mid-run.

**Do not run benchmark batches concurrently, and do not SIGKILL a game
mid-GPU-submission.**  Doing both wedged the GPU: amdgpu job timeout, ASIC
reset that did not come back, reboot required.  If a run must be stopped, stop
the launcher and let the game exit.

## The mouse wedge, and how to reproduce it in two minutes

**Mouselook works in BORDERLESS, then dies after a few window switches, and
never comes back for the rest of the session.**  [MEASURED 2026-08-29, DOOM
(2016) on the native lane.]  Fullscreen never worked at all; borderless works
until focus churns.

That asymmetry is the whole diagnosis.  Borderless does not take the
exclusive-fullscreen path, so it never does the ClipCursor + warp-recenter
dance -- which is why it works at first.  What kills it is focus churn:
cosmic-comp's periodic `update_pointer_focus` (src/shell/focus/mod.rs) yanks
pointer focus on any surface-stack change with no check for an ACTIVE pointer
constraint; the pinned smithay then deactivates the game's pointer lock on
`leave` and drops the pending cursor hint; a stale hint lands at the clip
corner (0,0); and from then on `new_constraint`'s `is_under` test can never
pass again.  Wedged for the session.

**The reproducer, which is the point of this entry:**

1. Launch a title borderless (DOOM: `+r_fullscreen 0`).
2. Confirm mouselook works.  It will.
3. Alt-tab away and back two or three times.
4. Mouselook is dead, and stays dead until the game is restarted.

Two minutes, no guesswork about the trigger.  Every earlier round of this cost
a game session just to find out what set it off.

**A patch exists and is INSUFFICIENT.**  cosmic-comp was rebuilt locally with a
two-hunk backport (guard `update_pointer_focus` against active constraints;
apply the pending hint before explicit deactivation) and the wedge still
happens -- because an explicit focus change still legitimately deactivates the
lock, and the guard only covers the PERIODIC path.  Upstream has no fix:
pop-os/cosmic-comp#2050 closed as a duplicate of cosmic-epoch#1817, still open.
Xwayland 24.1.13 does not implement `wp_pointer_warp_v1` (only the lock/hint
emulator) and the MR adding it is unmerged, so that route is closed too.

Wine is NOT the cause: every commit touching `winex11.drv/mouse.c`,
`win32u/rawinput.c`, `win32u/input.c` and `server/queue.c` in this fork's range
is upstream-authored, and upstream has not fixed the absolute-valuator gate
either (zero commits, checked to wine-11.16).

## Reading a failure

**A "frozen" game may be a CRASHED game whose crash path wedged.**  [MEASURED]
2026-08-27, Cyberpunk under a benchmark batch: the game raised its own fatal
(`RaiseException(c000001d)` -- REDengine's fatal idiom; the "exception
address" is the RETURN ADDRESS of that call, a `ret` after a `call` in the
image, not a faulting instruction) ~36 s into STARTUP, the unhandled-guest
re-raise engaged the native crash machinery, and that wedged on the process
heap critical section ("blocked by <tid>" retry spam every 60 s) -- a
15-minute "freeze" that was really a crash plus a stuck reporter.  Read the
log's `raise_pending_guest_exception` line and the RtlpWaitForCriticalSection
spam before theorising about hangs.  Also: `ps -eo comm` truncates
`Cyberpunk2077.exe` to `Cyberpunk2077.e`, and a `grep -iE "Cyberpunk"` over
`comm` output can STILL miss it depending on the field order -- confirm a
game's death with `ps --ppid <proton>` and `args`, not comm greps.  The one
occurrence sits at 1 fatal in 11 otherwise-clean runs of the same build; the
CS-fast-path bisection (6 legs) and soak (6 legs) both came back clean, and
the other open suspect is fex's documented lazy-SMC cross-thread hole
(fex-src a072a9122, "narrowed but does not close"), whose window every thunk
relayout reshuffles.

`refuse_once` prints **once per slot**, so one line in a log can mean a call
made sixty times a second.  When a title renders wrong, `grep -a refus` the
run log before suspecting anything deeper — the Cyberpunk "memory corruption"
that survived three sessions of DMA and shader theories was one refused
`ClearDepthStencilView`, and the whole scene rendered against stale depth
because of it.

A `+seh` Cyberpunk boot log is 1.6 GB; `WINEDEBUG=+winecom,+d3d12` is ~8 MB
and names every slot.  Start there.

## The gate suite

```sh
for g in ppc64le/*/check-*.sh; do "$g" --sabotage; done
```

Every gate's negative control must go red; the suite is 33 gates and the
sweep takes about half an hour.  It needs `WINEPREFIX` (a booted prefix) and
`WINEFEXBRIDGE`.  Several gates raise **real modal dialogs** on the live
desktop as their controls — run the sweep under its own Xvfb, or expect to
close boxes by hand and to have them steal focus from anything else running.
