# ntdll.dll+64d14 null deref — Cyberpunk 2077 loading-screen crash

## Headline

**Function:** `RtlImageNtHeader()` — `dlls/ntdll/loader.c:4529` (in the tree,
before this fix; the guard added by this fix shifts later line numbers by
~19).

**Faulting instruction:** `dos->e_magic == IMAGE_DOS_SIGNATURE` at
`loader.c:4538`, compiled to `lhz r9,0(r31)` (load halfword at `r31+0`).

**The null pointer:** `r31`, which holds `RtlImageNtHeader`'s own `hModule`
parameter (`dos = (IMAGE_DOS_HEADER *)hModule`). `hModule == NULL` at the call
that crashed.

**Why it was null:** it wasn't a bug that it was null. `RtlImageNtHeader` is
explicitly written to tolerate a garbage/NULL `hModule` — the whole function
body is wrapped in `__TRY { ... } __EXCEPT_PAGE_FAULT { return NULL; }
__ENDTRY`, and five other files in this tree (`actctx.c`, `loader.c`,
`relay.c`, `resource.c`, `threadpool.c`) call it and check the return value
for NULL as routine, expected behaviour. The actual bug is that **this port's
`__TRY`/`__EXCEPT_PAGE_FAULT` did not catch the fault**: the SEH frame was
pushed onto `NtCurrentTeb()->Tib.ExceptionList` correctly (confirmed in the
disassembly, see below), the page fault happened inside the protected region,
and it still reached `KiUserExceptionDispatcher` as an unhandled access
violation and killed the process (log shows `rc=3`).

## Status of the fix

Applied and built: an explicit `if (!hModule) return NULL;` was added at the
top of `RtlImageNtHeader`, before the `__TRY` block, in
`dlls/ntdll/loader.c`. Confirmed present in the rebuilt `ntdll.dll.so` by
disassembly. Confirmed **zero regressions** by running the `rtl`,
`exception`, and `info` ntdll test binaries before and after the change —
identical exit codes and identical failure/test counts in both builds (these
tests have pre-existing, unrelated failures on this port; see "Verification"
below).

**Not fixed, and explicitly flagged rather than papered over:** the
underlying reason the ppc64 port's `__TRY`/`__EXCEPT_PAGE_FAULT` fails to
recover an in-process page fault here. That is a defect in the general SEH
delivery chain (`call_seh_handlers` → `virtual_unwind` → generic ELFv2
back-chain unwind → `__wine_rtl_unwind` → `__wine_longjmp`), not specific to
`RtlImageNtHeader`, and it can in principle bite *any* other `__TRY` block on
this architecture with a non-NULL but still-invalid pointer (this fix only
neutralises the single most common garbage value, NULL, by never asking the
fault-catching machinery to do anything). See "What's still open" below.

---

## How the RVA was resolved (double landmark)

The build is `~/Development/powerpc64le-ports/hangover-ppc64le/wine-build` on
the AC922. `dlls/ntdll/ntdll.so` is the *unix-side* module (functions under
`dlls/ntdll/unix/`); `dlls/ntdll/ntdll.dll.so` is the separate *PE-side*
module — the actual `ntdll.dll` (functions in `dlls/ntdll/*.c`, no `unix/`) —
loaded the classic Wine "builtin .so" way (`dlopen_dll`/`map_so_dll` in
`dlls/ntdll/unix/loader.c`). **The crash log's `"ntdll.dll"+RVA` refers to
`ntdll.dll.so`, not `ntdll.so`.** Naively treating the RVA as an `nm`/`objdump`
offset into `ntdll.so` "works" well enough to land inside a real function
(`fd_describe`/`fd_out` in `unix/signal_ppc64.c`) — but the specific
instruction at that offset (`add r27,r27,r3`, a register-only op) can never
raise a memory access violation, which is what caught the error: the RVA
math was wrong, exactly as `unix/signal_ppc64.c`'s own comment on
`map_so_dll()` warns ("an address printed by +seh ... cannot be turned into a
.so file offset by naive arithmetic, because those two views of a builtin
module do not share a base").

The correct mapping was derived and then cross-checked live, twice:

1. `map_so_dll()` computes a synthetic PE-view base (`addr`/`module`,
   what becomes `LDR_DATA_TABLE_ENTRY.DllBase`) that is **separate** from
   where the `.so` is actually `dlopen()`'d. `delta = nt_descr - addr`
   (`nt_descr` = the real runtime address of `__wine_spec_nt_header`, `addr`
   = the synthetic base), and all RVAs baked into the header are shifted by
   `delta` at load time.

2. Ran `ntdll_test.exe` (ppc64-windows) under `strace -f -e trace=mmap,openat`
   with `WINEDEBUG=+loaddll`, twice (two independent test processes). In both:
   - `ntdll.dll.so` is `dlopen()`'d and its code lands via a normal
     `PROT_READ|PROT_EXEC` `mmap(fd, offset=0)` at some real address
     (`so_base`), e.g. `0x3fff8a037000`.
   - A follow-up `mmap(addr, 4096, PROT_READ|PROT_WRITE, MAP_FIXED|ANON, ...)`
     lands exactly at the address `+loaddll` reports as `ntdll.dll`'s
     `DllBase` (e.g. `0x3fff8a050000`) — this is `map_so_dll()` overwriting
     part of the mapped image with the synthetic DOS/NT/section headers.
   - Both runs gave **the same delta**: `so_base - DllBase = -0x19000`,
     i.e. `RVA = link_address - 0x19000` (`link_address` = the address
     `nm`/`objdump` report for a symbol in `ntdll.dll.so`).
   - Cross-check: `AddressOfEntryPoint` embedded in the static
     `__wine_spec_nt_header` data (read via `objdump -s`, offset 0x30 into
     the header at link address `0x146028`) is `0xfc4f0` — exactly equal to
     `nm`'s address for `__wine_spec_dll_entry`. This confirms the header's
     *pre-fixup* fields are plain link addresses, consistent with the delta
     model above.
   - Self-consistency against the actual crash log: `DllBase = nip - RVA =
     0x3fffa7364d14 - 0x64d14 = 0x3fffa7300000`; per the formula,
     `link_address = RVA + 0x19000 = 0x64d14 + 0x19000 = 0x7dd14`.

3. `RVA 0x64d00 → link 0x7dd00`, `RVA 0x64d14 → link 0x7dd14`, both inside
   `RtlImageNtHeader` (`nm`: `0x7dca0`, next symbol `find_basename_module` at
   `0x7dda0`).

## Disassembly (confirms source line-for-line)

```
000000000007dca0 <RtlImageNtHeader>:            loader.c:4530 (prologue)
   7dcb8: mr      r31,r3                         r31 = hModule
   ...
   7dcf0: addis   r12,r2,-8                      wine/exception.h:265
   7dcf4: addi    r12,r12,752                       (call to NtCurrentTeb(),
   7dcf8: mtctr   r12                               inlined __wine_push_frame
   7dcfc: bctrl                                     body around it)
   7dd00: ld      r2,24(r1)             <- RVA 0x64d00 == LR after the call
   7dd04: addi    r9,r1,32                       exception.h:267 (&__f.frame)
   7dd08: ld      r10,0(r3)                      exception.h:266 (teb->ExceptionList)
   7dd0c: std     r10,32(r1)
   7dd10: std     r9,0(r3)                       teb->ExceptionList = &__f.frame
   7dd14: lhz     r9,0(r31)             <- RVA 0x64d14 == FAULT. loader.c:4538
                                            dos->e_magic, dos == r31 == hModule == 0
   7dd18: cmpwi   r9,23117 ("MZ")
   ...
```

This is a complete, self-consistent story: the `__TRY` frame push
(`NtCurrentTeb()->Tib.ExceptionList` update) executes fine, immediately
followed by the very first, and only, thing the protected block does before
faulting — read `hModule->e_magic`.

## Why this crash, specifically, escaped `__TRY`/`__EXCEPT_PAGE_FAULT`

Traced as far as time allowed, without reproducing live (see below):

- `dispatch_exception()` (`dlls/ntdll/exception.c:239`) is reached for *every*
  SIGSEGV that isn't shortcut by `handle_syscall_fault()`'s
  syscall-in-progress `jmp_buf` check (`unix/signal_ppc64.c:1451`) — there is
  no separate "try the local `__TRY` handler first, without full dispatch"
  path on this port for a fault outside a syscall.
- It calls `call_seh_handlers()` (`dlls/ntdll/signal_ppc64.c:525`), which is
  supposed to find and invoke exactly the frame that was just pushed, via the
  "hack: call wine handlers registered in the tib list" branch (line 587-617
  — this is stock Wine wording/logic, not ppc64-specific).
- Getting there requires `virtual_unwind()` (line 364) to first successfully
  unwind the *faulting* frame (`RtlImageNtHeader`'s own). The file's own
  header comment (lines 46-71) states plainly that `RtlLookupFunctionEntry()`
  **always** returns NULL on this port (no ppc64 `.pdata` producer exists),
  so every unwind — not just this one — falls back to the generic ELFv2
  back-chain walk (`0(r1)` / `16(r1)`). `RtlImageNtHeader`'s prologue
  (`std r0,16(r1)` before `stdu r1,-672(r1)`) is a completely standard
  ABI-conforming frame, so the back-chain walk *should* succeed here.
- After that, recovery is supposed to happen via
  `__wine_exception_handler_page_fault` → `unwind_frame` → `__wine_rtl_unwind`
  → `__wine_longjmp` (`libs/winecrt0/exception.c`). For ppc64 there is no
  assembly `__wine_rtl_unwind` (unlike i386/x86_64); it uses the generic C
  fallback: `RtlUnwind(frame, target, record, 0); for (;;) target();`.

**I could not pin down, with certainty, which single link in that chain is
actually defective** — the TEB-frame stack-pointer comparison in
`call_seh_handlers`'s while loop, something in the generic ELFv2 unwind of
the frames between the fault and `__f.frame`, or `__wine_setjmpex`/
`__wine_longjmp`'s ppc64 register/TOC (`r2`) save-restore. Given the task's
own warning that a wrong attribution here sends the next person down a dead
end, I am reporting this as **open and unresolved**, not guessing further.

## What I verified live, and what I could not

- **Verified live (double landmark):** the RVA→source mapping, via two
  independent `strace`d `ntdll_test.exe` processes (see above). This is not
  reasoning from source alone.
- **Verified live:** the fix builds, is present in the rebuilt
  `dlls/ntdll/ntdll.dll.so` (checked mtime and re-disassembled
  `RtlImageNtHeader`: `mr. r31,r3` / `beq` to the NULL-return path appears
  immediately, before the `__wine_push_frame`/`dos->e_magic` code).
- **Verified live:** no regression. Ran `ntdll_test.exe rtl`, `exception`,
  and `info` (all `ppc64-windows`, headless, no display, no game) against a
  build *without* the fix (`git stash`) and again *with* it. Exit codes and
  failure/pass counts were identical in both (`rtl` exit=3, `exception`
  exit=11 with 11/380 failures, `info` exit=255 with 365/4779 failures) —
  these are pre-existing, unrelated port gaps, not something this change
  touches or masks.
- **Not verified live:** that `RtlImageNtHeader(NULL)` called directly now
  returns cleanly instead of crashing, and the exact mechanism by which the
  original fault escaped `__TRY`. I attempted this with a scripted `gdb`
  session (`catch load` / breakpoint on `RtlImageNtHeader`, calling it
  directly) against a running `ntdll_test.exe`, but Wine's process tree here
  forks repeatedly (preloader → wineserver → per-subtest child processes),
  and I could not get a stable non-interactive breakpoint in the right
  process before it ran to completion or gdb's catchpoints were lost across
  a fork, inside the time budget for this task. This is exactly the kind of
  gap the task said is fine to report honestly: **the fix is reasoned from a
  disassembly-confirmed root cause and checked for zero regression, not
  confirmed by directly reproducing the original failure and re-running it
  green.**
- **Genuinely unverifiable without the game:** whether this was the only
  null/garbage `hModule` reaching `RtlImageNtHeader` during the Cyberpunk
  2077 loading screen, and whether `concrt140.dll not loaded` (the `fixme`
  immediately before the crash in the log) is related. I looked for a
  connection and found none: `concrt140` is a Visual C++ concurrency runtime
  DLL Wine doesn't implement (a `msvcp:init_cxx_funcs` stub warning), and
  nothing in `RtlImageNtHeader`, its callers, or the loader path touches
  `concrt140` or anything that fixme sets. Treating it as coincidental
  ordering in the log, not a cause, but this is not provable without the
  game.

## Files touched

- `dlls/ntdll/loader.c` — `RtlImageNtHeader()`: added `if (!hModule) return
  NULL;` before the `__TRY` block, with a comment recording the measurement,
  the reasoning for why NULL is legitimate input here, and explicitly
  flagging the still-open SEH-chain gap for whoever looks at this next.
