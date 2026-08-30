# Upstream Wine e99fc2f7587..mirror/master (265 commits, 2026-08-14..2026-08-26) — relevance to flap-standalone fork

All findings below are from reading the actual diffs over ssh on the AC922 tree
(`~/Development/powerpc64le-ports/hangover-ppc64le/wine-upstream`), not from commit subjects.
`wine-11.16` (8da89f8493b) is inside this range. Labels: MEASURED = verified in the diff/blob;
INFERRED = judgment about impact on the fork.

## 1. Mouse raw-input motion — UPSTREAM HAS NOT FIXED IT

**MEASURED:** At `mirror/master`, `update_relative_valuators()` in `dlls/winex11.drv/mouse.c`
still contains the exact gate in the diagnosis:

```c
if (valuator->number == 0 && valuator->mode == XIModeRelative) thread_data->x_valuator = *valuator;
if (valuator->number == 1 && valuator->mode == XIModeRelative) thread_data->y_valuator = *valuator;
...
WARN( "X/Y axis valuators not found, ignoring RawMotion events\n" );
```

and `map_raw_event_coords()` still returns a zero point immediately when
`x->number < 0 || y->number < 0`. On a master pointer whose valuators sit in
`XIModeAbsolute` (this box's Xwayland 24.1.13 under cosmic-comp), every raw frame is still
discarded. **Nothing in the 265 commits touches that gate. The fix has to be ours.**

Commits that DO touch the area (all read in full):

- **9306b8e8de4** "winex11: Preserve fractional raw mouse motion" (Rüdiger Lenz, 2026-08-13).
  Adds `double raw_x, raw_y` accumulators to `struct x11drv_thread_data` (x11drv.h) and changes
  `map_raw_event_coords()` from `raw->x = *raw_values` to accumulate-and-round with fractional
  carry; resets the accumulators in `update_relative_valuators()`.
  - MEASURED: does not change the relative-mode gate; runs only after the gate has passed.
  - INFERRED: irrelevant to our symptom (we never reach this code), but **directly collides
    textually with any fix we write in `update_relative_valuators` / `map_raw_event_coords`** —
    both hunks land within lines 241-260 and ~1594-1640 of mouse.c. If we patch the gate to
    accept absolute-mode valuators, we should write the patch against the post-9306b8e8de4
    shape (and keep the fractional carry — it is a good change for high-DPI mice).

- **f245eb3c812** "win32u: Flush mouse input motion when hitting the clipping rect edges"
  (Rémi Bernon, 2026-08-25, Wine-Bug 60218). In `dlls/win32u/input.c`: moves `get_clip_cursor()`
  up, adds `is_clipped_motion()`, and forces `send_mouse_motion()` before an absolute host-clipped
  position at the clip-rect edge overwrites accumulated relative motion; needed because dinput's
  mouse relies on LL-hooks seeing motion beyond the clip rect.
  - INFERRED: fixes a *different* motion-loss bug (relative deltas eaten at ClipCursor edges when
    legacy events are the source). Our games use RIDEV_NOLEGACY WM_INPUT, so this does not fix
    our symptom, but it is exactly the class of bug that co-occurs in FPS games — worth
    cherry-picking with the merge so we don't chase it later as a second "mouse eats deltas at
    screen edge" report.

- **c28e3a549fc / 7c0575e36d0 / d006e2e91fa / c8fc7559487** — server/queue.c
  `AttachThreadInput` refcounting/focus/keystate refactor series. MEASURED: no rawinput or
  motion-delivery change; queue.c churn only matters as merge-conflict surface if the fork
  edited queue.c.
- **58e1111075f** — set_window_rect_visible uses receiving window's thread; unrelated.

`dlls/win32u/rawinput.c`: **zero commits in range** (MEASURED). `server/queue.c` rawinput paths:
untouched apart from the attach series above. winewayland.drv: only mechanically touched by two
win32u client-surface commits — its relative-pointer code is unchanged.

**Bottom line: no upstream commit addresses absolute-mode master-pointer valuators; merging
11.16 will not restore mouse motion. Our fix (accept XIModeAbsolute valuators or derive deltas
from raw_values regardless of mode) remains necessary, and must be rebased over 9306b8e8de4.**

## 2. Vulkan / WSI present

**MEASURED:** `dlls/win32u/vulkan.c` is touched by exactly one commit, **6e07c6c1c67**, and only
one line of it: `get_unused_client_surface( surface->hwnd, 0 )` gains a third `FALSE` argument
("raw physical coordinates") in `win32u_vkCreateWin32SurfaceKHR`. No swapchain-creation,
present-mode, or image-count logic changes anywhere in the range (grep for
swapchain/present mode/image count/WSI finds only a d3d10 test sync). `dlls/winevulkan` is
untouched.

**INFERRED:** our `create_host_swapchain()` retry in win32u/vulkan.c survives untouched
semantically; the only merge friction is the trivial signature change at the surface-creation
call site if our diff context overlaps it. Nothing upstream subsumes the
win32u-swapchain-image-count.diff rationale.

Note: the surrounding win32u OpenGL/client-surface area is being heavily reworked (14+ commits:
framebuffer surface, gamma-ramp emulation, client surface coordinates). If the fork carries any
patches in win32u/opengl.c or the drivers' present paths, expect real conflicts there.

## 3. Exception dispatch / unwinding

**MEASURED:** Pickaxe over the whole range for `_CxxThrowException` and `__CxxFrameHandler`:
**zero hits**. `dlls/msvcrt/cpp.c`, `except*.c`, `handler4.c`, `vcruntime140`, `vcruntime140_1`:
**zero commits**. The C++ EH contract our guest-side `_CxxThrowException` was written against is
unchanged — nothing invalidates that work.

What did change nearby:

- **8c284041138** `dlls/ntdll/unwind.h`: `ctx_flags_x64_to_arm`/`ctx_flags_arm_to_x64` now
  preserve `CONTEXT_EXCEPTION_ACTIVE|SERVICE_ACTIVE|EXCEPTION_REQUEST|EXCEPTION_REPORTING`
  via a new `exception_flags_mask`. arm64/arm64ec-only code, but INFERRED: if our ppc64le CPU
  backend has analogous x64<->host context-flag converters copied from this header, we should
  replicate the fix — losing CONTEXT_EXCEPTION_REQUEST breaks `test_context_exception_request`
  and debugger-style Get/SetThreadContext users.
- **0b8d0addf6a** "Don't use entry thunk context in RtlRaiseException" — signal_arm64ec.c only.
- **KiUserEmulationDispatcher series** (see section 4) — changes *when* the emulation dispatcher
  is entered after NtSetContextThread; contract-relevant if our backend mirrors arm64ec.
- `dlls/ntdll/signal_x86_64.c` (PE side) and unix/signal_x86_64.c: no commits in range.

## 4. WoW64 / i386 lane

**MEASURED commits:**

- **9d7c45f0179** "wow64: Enforce a valid low limit in extended memory parameters" —
  `dlls/wow64/virtual.c` now rejects `LowestStartingAddress > highest_user_address` in
  `mem_extended_parameters_32to64`, and ntdll/unix/virtual.c goes back to validating against
  the full `user_space_limit` (reverts 235e3e522e8).
- **4ac0555e55c** "ntdll: Validate virtual memory ranges against the user address limit" —
  companion change, `dlls/wow64/virtual.c` + unix/virtual.c; plus test churn.
- **a081733d481** 32-bit limit on cross-process allocations in old wow64 mode;
  **ce72727a654** NtQueryVirtualMemory overflow fix in old wow64 mode.
- **KiUserEmulationDispatcher rework** (Jacek Caban): **f12bd89a4bd** moves dispatcher setup
  out of `signal_set_full_context` into syscall-dispatcher exit (new
  `RESTORE_FLAGS_EMULATION 0x00010000` in restore_flags, set in NtSetContextThread when the
  target pc is not EC code) so an emulated context can be set *while the thread is in a
  syscall*; **d3b41a854a8** does the same setup in usr1_handler; **3b6b0cedd98** sets
  `InSimulation` before returning to KiUserEmulationDispatcher from the kernel side.
  All in `dlls/ntdll/unix/signal_arm64.c` / `signal_arm64ec.c`.
- **fc2ba3ffce7 / 6ddac4544f0** ARM64 syscall dispatcher hygiene; **2f69c014dc2**
  EcCodeBitMap bounds check.

**MEASURED:** `dlls/ntdll/unix/loader.c`: zero commits. `dlls/wow64win`, `dlls/wow64cpu`,
`ntdll.spec`, `win32u.spec`: zero commits. No i386 builtin-set changes surfaced in any area log.

**INFERRED:** none of this conflicts with a ppc64le backend textually (it is all arm64 files),
but the KiUserEmulationDispatcher timing change and RESTORE_FLAGS_EMULATION pattern are the
current upstream contract for "set guest context during a syscall" — if our WoW64 CPU backend
copied the older `signal_set_full_context` hook, we inherit the bug upstream just fixed
(NtSetContextThread on a thread parked in a syscall not entering the emulator). Worth an audit
of our backend against f12bd89a4bd.

## 5. server/protocol.def — two changes, regeneration guaranteed

**MEASURED:**

1. **4423e8ed9aa** (2026-08-16, in 11.16): `struct context_data` layout change —
   `arm64_regs` split `x[31]` -> `x0[18], x19[12]` (x18 removed from INTEGER), new
   `union { unsigned __int64 arm64_x18; } tls` selected by new **SERVER_CTX_TLS 0x0100**, and
   all SERVER_CTX_* constants widened to 4 hex digits. Touches server/thread.c, trace.c,
   request_trace.h, unix/signal_arm64.c, unix/thread.c.
   - INFERRED: protocol-version bump + struct layout change. Our x86_64 guest contexts use
     `x86_64_regs`, which is untouched, but any fork code that memcpy's or hand-marshals
     `struct context_data` (a ppc64le backend almost certainly does) must be re-checked; and if
     the fork added its own SERVER_CTX_ bit, 0x0100 is now taken.
2. **178f11f1c57** (2026-08-17): `get_window_rectangles` reply gains
   `struct rectangle visible;`. Reply-size change; regeneration handles it; semantic risk nil
   unless the fork issues that request via hand-rolled marshalling.

Both bump the protocol version, so the fork's own protocol.def extension will conflict at
minimum on the version line and must be renumbered/regenerated on merge. No other protocol.def
commits exist in the range (MEASURED).

## 6. Bugs we independently found / worked around

- **msacm32:** **zero commits** in range (MEASURED). Upstream did not touch the
  driver-registration path; the stale-JIT-cache workaround stands on its own.
- **psapi:** **zero commits** to `dlls/psapi`, `include/psapi.h`, or `dlls/kernelbase/debug.c`
  (MEASURED). Our 27-export thunk-generator fix is not invalidated and nothing upstream changed
  the spec it was generated from.
- **setupapi:** `SetupGetInfDriverStoreLocationW` — pickaxe over the range: **zero hits**
  (MEASURED). The XeSS unresolved import is still unimplemented upstream; if we stub it, no
  collision. However `dlls/setupapi/devinst.c` got a 4-commit rework of device enumeration
  (**4a01ceea685, f9b51de6234, 2857fec3c24, af16996e29c**: `SetupDiGetClassDevs` reimplemented
  on top of `CM_Get_Device_ID_List` / `CM_Get_Device_Interface_List`, `remove_all_device_ifaces`
  via cfgmgr32, plus tests 87ad0c29d12/586119e6ea1). INFERRED: since devinst.c is in our
  conflict set, this materially enlarges the merge conflict there and changes device/interface
  enumeration ordering — retest anything that enumerates devices (controller detection, HID)
  after merging.
- Bonus finding, msvcrt: **738aac8c6ed** adds a hand-written **x86_64 asm body for `_isatty`**
  whose literal instruction bytes exist so the Ruby runtime can *disassemble the builtin* to
  find `__pioinfo`. INFERRED: a reminder that some apps sniff builtin code bytes — on our lane
  the guest-facing msvcrt must present real x86_64 bytes for this to work; if our msvcrt is a
  native ppc64le builtin behind thunks, Ruby-style pioinfo discovery will fail there. Not a
  regression from the merge, but a known-sharp edge to log.

## Would anything invalidate our recent work?

- **_CxxThrowException / guest SEH: NO** (MEASURED — zero commits to any C++ EH file, zero
  pickaxe hits, x86_64 signal/unwind files untouched; only arm64/arm64ec changed).
- **Swapchain retry: NO** (MEASURED — one-line signature drift at the surface-creation call
  site is the only vulkan.c change; no WSI semantics touched).

## Cherry-pick shortlist (if the full merge waits)

1. **f245eb3c812** — clip-rect-edge motion flush (win32u/input.c). Real dinput/FPS mouse fix,
   small, self-contained.
2. **9306b8e8de4** — fractional raw motion accumulation. Take it *together with* our
   absolute-valuator fix so the fix is written against upstream's final shape of
   map_raw_event_coords.
3. **8c284041138** — pattern-fix for exception-flag preservation; port the mask idea into our
   context converters if they have the same loss.
4. **9d7c45f0179 + 4ac0555e55c** — wow64 extended-parameter/address-limit validation, directly
   applicable to the new 32-bit lane.
5. **f68eb9e43d0** — winex11 GL_EXTENSIONS use-after-free; trivial and a real crash fix.

## Could not determine

- Whether the fork's own edits overlap server/queue.c or win32u/opengl.c (I did not inspect the
  fork tree; another agent owns merge mechanics). The upstream churn there is large either way.
- Whether our WoW64 backend actually mirrors the arm64ec KiUserEmulationDispatcher hook point
  (needs a look at our signal backend to know if f12bd89a4bd's bug applies).
- Runtime behavior differences from the setupapi enumeration-order change (tests changed
  ordering expectations; only a retest on our lane can confirm impact).
