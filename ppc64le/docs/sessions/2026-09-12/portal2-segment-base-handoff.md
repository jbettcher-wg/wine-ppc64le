# Portal 2 (620) on op64k: the render-thread "wow64 SEH storm" is a ppc64 JIT ALU bug

Handoff to the fastppcx86 (fex-ppc64le) agent, 2026-09-12.  No FEX edits were
made from the wine side; this names the defect, the trigger, a standalone
repro and the fix shape.

## What the logs actually show

Run log: `~/.local/share/Steam/steamapps/compatdata/620/wine-ppc64le-native-20260911-234150-1794798.log`
(and the 23:28 run with +module,+seh, same shape).

Every guest fault in the storm has the SAME arithmetic:

    fexbridge-fault #1  rip=7875209c  dar=f312dba4  rdi=0012dba4  rcx=7  rax=0   (rep stosd, ES:EDI)
    fexbridge-fault #53 rip=781d4d9f  dar=f3111620  rdi=00111620  rcx=13 rax=0   (rep stosd, ES:EDI)
    23:28 run  #1       rip=7bcd28f0  dar=f312e1bc  rsi=0012e1bc  (ntdll32 memcpy movsb, DS:ESI, read)

    fault address = guest register + 0xF3000000, every time, stores AND loads.

The guest registers are fine.  The JIT is adding 0xF3000000 as the ES base
(stos/movs destination) and the DS base (movs source).  Ordinary
`mov [edi]` never consults those bases in 32-bit mode (GetSegment returns
nothing for an unprefixed access), so the thread runs for minutes; only
string instructions die.  The SEH handlers that then run use `rep stos`
themselves, so each dispatch re-faults and nests until the 64-bit stack is
gone ("virtual_setup_exception stack overflow").  The storm is the symptom,
not the bug.  The "native fault at ntdll.dll+d2a5c" lines are the 64-bit
dispatcher's own memcpy of the record (r5=0x98) being reported by
report_native_pc_in_guest_image; a red herring.

Not 64K-specific, not wow64, not ntsync.  4K Portal 2 never reached this
point, which is why it was never seen.

## Where 0xF3000000 comes from

FexBridge.cpp installs the Windows flat data descriptor for selector 0x2B:

    Base=0 Limit=0xFFFFF Type=3 S=1 DPL=3 P=1 L=0 D=1 G=1
    qword = 0x00CF_F300_0000_FFFF        byte 5 (access byte) = 0xF3

`OpDispatchBuilder::UpdatePrefixFromSegment` (any `pop es`, `mov es,ax`,
`pop ds`, ...) rebuilds the cached base with

    Upper32 = Lshr64(desc, 32)
    Masked  = And32(Upper32, 0xFF000000)             ; Base2 << 24
    Merged  = Orlshr(i32Bit, Masked, desc, 16)       ; | (low 32 of desc) >> 16 = Base0
    base    = Bfi(i32Bit, 8, 16, Merged, Upper32)    ; insert Base1 at bit 16

`Orlshr` at OpSize::i32Bit must shift the 32-BIT VIEW of its source (the
ARM64 backend gets this for free: `orr w, w, w, lsr #16`).  The ppc64
backend does not:

    FEXCore/Source/Interface/Core/JIT/PPC64LE/ALUOps.cpp:922
    DEF_OP(Orlshr) {
      srdi(TMP4, S2, Op->BitShift);     // 64-bit shift: bits 32..47 of desc land in bits 16..31
      or_(Dst, S1, TMP4);
      if (IROp->Size == IR::OpSize::i32Bit) Mask32Tail(Dst, Node);   // too late
    }

    0x00CFF3000000FFFF >> 16 = 0x0000_00CF_F300_0000 -> low 32 = 0xF300_0000
    Bfi then repairs bits 16..23 (Base1) and leaves the access byte in 24..31.
    Cached ES/DS base = 0xF3000000.

Why the ASM suite is green: the 32Bit_ASM harness prepends `mov es,ax`
(selector 0x17) to every test, but the Linux frontend's descriptors carry
base bytes only (SetGDTBase), byte 5 is 0, the leak is 0.  Only the bridge's
fully-formed descriptors expose it.

## Trigger in Portal 2

Miles Sound System.  `mss32.dll` has 125 real ES/DS reload sites
(`pop %es`, `pop %ds`, `mov %ax,%es`), `mssvoice.asi` 3, `msseax.flt` 2;
`engine.dll` reports 90 with the same objdump filter.  The 23:28 run faults
immediately after `msssrs.flt`'s process_attach.  Any 32-bit title that
touches ES/DS will hit this; Dex simply never reloads a segment register.

## Standalone repro (already run on op64k, reproduces)

`segreload32.c` next to this file.  Build and run:

    i686-w64-mingw32-gcc -O1 -o segreload32.exe segreload32.c
    export WINEPREFIX=$HOME/scratch-64k/segprefix-claude      # exists, wineboot done
    export WINEFEXBRIDGE=$HOME/projects/fex-emu-ppc64le/src/build-smc/Source/Tools/FexBridge/libfexbridge.so
    export FEX_HOSTPAGEMODE=force DISPLAY=:1 WINEDLLOVERRIDES=winedbg.exe=d
    cd ~/Projects/power8/wine-ppc64le && ./wine ./segreload32.exe

Observed (2026-09-12 00:25 box clock): step 1 (`rep stosl`, no reload)
passes silently; step 2 (`push es; pop es; rep stosl`) faults:

    fexbridge-fault #1 rip=00401670 dar=f3406040 rdi=00406040 rax=42424242 rcx=4

Expected after the fix: four "ok" lines, no fault.  (The probe's VEH-resume
path wedges after the fault under the current bridge, so it does not print
the FAULT line itself; the bridge's fault record is the signal.)

## Fix shape (FEX side, not done here)

- `Orlshr`: at i32Bit shift the 32-bit view, e.g.
  `rlwinm(TMP4, S2, (32 - sh) & 31, sh, 31)` (srwi) with sh==0 handled as
  `rldicl(TMP4, S2, 0, 32)`, exactly as `Lshr`, `AndShift`, `XornShift`
  already do.
- `Ornror` (ALUOps.cpp:932) has the same shape: `rldicl` 64-bit rotate at
  every size.  At i32Bit it needs `rotlwi`/`rlwinm` on the 32-bit view.
  Unverified whether any 32-bit path emits it; fix while there.
- Audit any other combined-shift op that still uses `srdi/sradi/rldicl`
  without an `Is32` arm.  `Lshr`, `Ashr`, `Ror`, `AndShift`, `XorShift`,
  `XornShift` were checked and are correct.
- Add a bridge-side regression: BridgeSmoke (or a 32Bit_ASM test whose
  descriptor carries the access byte) that does `push es; pop es; rep stosd`.

## Secondary observation, not blocking

A nested-dispatch storm exhausts the 64-bit stack before the guest's own
32-bit stack overflows.  Real Windows would raise STATUS_STACK_OVERFLOW on
the 32-bit stack; here the nesting is bounded only by the ppc64 stack.  Not
needed for Portal 2 once the base is right.
