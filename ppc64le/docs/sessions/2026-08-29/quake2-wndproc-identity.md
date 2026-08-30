# Quake II rerelease: the 224-deep wndproc recursion, root-caused and fixed — 2026-08-30

Symptom: `quake2ex_steam.exe` exits rc=3 in under a minute.  The crossing-depth
diagnostic showed a perfect two-element cycle — `REVERSE guest rip=0x6c0000` /
`TRAP guest rip=0x3fffffba01b3` (user32 stub #27, `CallWindowProcW`) — 224
crossings deep until the kernel-stack floor check killed the callback
(`c0000001`), 113 times, with stub #352 (`GetPropW`) innermost at exhaustion.

Everything below is [MEASURED] on 2026-08-30 unless labeled otherwise.
Artifacts: `/tmp/peekgate/q2/` on the AC922 (captured page, maps, +seh log,
before/after logs).

## 1. What the RWX page at 0x6c0000 actually is

Captured live from the running game (`/proc/<pid>/mem`, the mapping is a
single anonymous `rwxp` page `006c0000-006c1000`).  Its entire content is 21
bytes of x86-64:

```
48 8b 01   48 8b 51 10   4c 8b 41 18   4c 8b 49 20   48 8b 49 08   ff e0
mov rax,[rcx]; mov rdx,[rcx+10]; mov r8,[rcx+18]; mov r9,[rcx+20]; mov rcx,[rcx+8]; jmp rax
```

— a byte-for-byte match for `thunk_code[]` in `call_guest_function_args`
(`dlls/ntdll/signal_ppc64.c`).  **The page is the port's own native→guest
argument-unpacking trampoline**, written once by ntdll itself; the log's
"guest callback 00000000006C0000 failed" names the transport, not the culprit.
The "subclassing thunk written by the app" reading was wrong: the app wrote
nothing executable.  Every `REVERSE rip=0x6c0000` is simply "native called
some guest function with 4 arguments" — the dump cannot show which one,
because the target travels in the parameter block (see §5).

## 2. Who recurses, and why

One `WINEDEBUG=+seh` leg (524 MB, the game is short) reduced the whole
wndproc story to four lines:

```
RegisterClass  (0x7DFD80): wndproc 0x3FFFFFBA08B0 -> 0x942698
RegisterClassEx(0x7BFD80F0): wndproc 0x7BF63700   -> 0x942700
RegisterClassEx(0x7DF920):   wndproc 0x7BF67D30   -> 0x942838
SetWindowLongPtr(0x1005E, GWLP_WNDPROC): 0x7BF67D30 -> 0x942838
```

The game is Nightdive's Kex engine hosting its own `SDL2.dll` (in the game
directory; `kexPlatformAppSDL` in its log).  `0x7BF67D30` is SDL's
`WIN_WindowProc`; `0x942838` the pool stub the port mints for it.  The last
line is the smoking gun: **the window is subclassed with the same procedure
its class already carries** — the wrap dedup even hands back the same stub.

That is SDL's standard `WIN_SetupWindowData` idiom:

```c
data->wndproc = (WNDPROC)GetWindowLongPtr( hwnd, GWLP_WNDPROC );
if (data->wndproc == WIN_WindowProc) data->wndproc = NULL;   /* my class */
else SetWindowLongPtr( hwnd, GWLP_WNDPROC, (LONG_PTR)WIN_WindowProc );
```

On Windows the read-back is the raw pointer SDL registered, the comparison
matches, and no subclassing happens.  On this port the read-back was the pool
stub, the comparison missed, and SDL stored the stub as "previous proc" and
subclassed.  First message: stub → `WIN_WindowProc` → `GetPropW(hwnd,
"SDL_WindowData")` (the per-cycle GetPropW) → `CallWindowProcW(stub)` → stub →
`WIN_WindowProc` → … 224 crossings, floor check, dead callback, and the
engine's window never worked; it fell into its error path and exited rc=3.

The marshal layer's own comment had declared the trampoline-visible read-back
"the accepted answer" and the compare-against-own-address idiom one "no
correct program does."  SDL does it, in every program that links it, and it is
correct on Windows whenever registration and read are the same flavor.  The
corpus now contains the counterexample.

## 3. The fix: the trampoline is invisible to the guest

`dlls/ntdll/signal_ppc64.c` (the PE-side marshal layer — NOT the unix side
another agent is working in):

* `guest_cb_target()` — exact-base reverse lookup, stub → guest fn.
* `unwrap_guest_wndproc()` — pool stubs unwrap to their guest function;
  native procs, win32u winproc handles (`0xffff00nn`) and unknown values pass
  through untouched.  Idempotent against `wrap_guest_wndproc` in both
  directions (re-wrapping finds the same stub in the pool by dedup).
* Applied where a WNDPROC travels INTO the guest: a new
  `GetWindowLongPtrA/W` override row (GWLP_WNDPROC only), and
  `SetWindowLongPtr`'s previous-value return.  `GetClassLongPtr(GCLP_WNDPROC)`
  still returns the trampoline: nothing in the corpus reads it; the row
  belongs beside these when something does.
* The stale comment rewritten to carry the measured story.

Result: **the recursion is gone** — the fixed leg has zero stack exhaustions,
zero failed callbacks, zero `c0000001`.  The game now creates its window,
initializes Vulkan, sound, the map DB (5 episodes / 232 maps), ImGui, and
runs full server initialization before dying much later in its own game logic
(§4).  Regression: `peek_fastpath.exe` 5/5 PASS and `peek_shapes.exe` 15/15
PASS against the fixed ntdll (both exercise `RegisterClassExW` + guest
wndproc dispatch + delivery).

## 4. The NEXT blocker, for whoever picks it up

With the window fixed the game dies at:

```
12:33:49: Standard exception caught in kexPlatformApp::Main: index
```

(engine stdout, `Saved Games/Nightdive Studios/Quake II/stdout.txt`) — a C++
`std::out_of_range("index")`-shaped exception after `==== InitGame ====`
completes, caught by Kex's top-level handler → rc=3.  Unrelated to windowing.
Noted but not chased: candidates include the vulkan-1 thunk module returning
NULL for `vkGetPhysicalDeviceProperties2KHR` (a promoted-extension alias the
generator refused; an engine that saw the extension advertised may treat the
NULL as fatal or take a degraded path), and the fresh-prefix absence of
`system.cfg`/`autoexec.cfg` (normal first-run noise on Windows too).  The
`+seh` leg predates the fix, so a fresh capture is needed for this one.

## 5. The depth-cap question (raised by another agent, view formed)

Should the dispatcher recurse 224 deep at all?  My view: **the existing
kernel-stack floor check already is the depth cap, in the correct currency**
— bytes of stack left, not an arbitrary count.  It fired exactly as designed,
failed the callback loudly, and produced the dump that made this diagnosis
possible.  A fixed crossing-depth limit would be a second, arbitrary limit
with a false-positive risk against legitimately deep nesting (SendMessage
chains, nested dialogs, COM reentrancy), and it would have added nothing
here.  Not implemented, deliberately.

One diagnostic improvement IS worth someone's time: the crossing record for a
REVERSE entered through `call_guest_function_args` records the trampoline's
rip (0x6c0000) — the one address that identifies nothing.  Recording the
parameter block's target alongside would have named `WIN_WindowProc` in the
first dump and saved the live memory capture.  That record lives in
`dlls/ntdll/unix/signal_ppc64.c`, which is under active work by another agent
— left for them or for later, noted here.
