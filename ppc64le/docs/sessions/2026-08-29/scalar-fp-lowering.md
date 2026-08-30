# Scalar SSE floating point through the PPC64LE JIT: forms, registers, spills

**Session 2026-08-30 — analysis and measurement of emitted code only. No
benchmarks, no game launches. Companion to
[x86-vector-in-the-wild.md](x86-vector-in-the-wild.md), which established that
the dominant translated "SIMD" workload is scalar SSE FP plus register-pressure
memory traffic.**

## Summary

**Instruction forms (measured, from source and from live translated blocks):**
the JIT emits neither classic FP (`fadds`/`lfd` on FPR0-31) nor VSX *scalar*
forms (`xsaddsp`/`xsadddp`). It emits a deliberate third thing: **splat-domain
VSX *vector* ops** — `xvaddsp`/`xvadddp`/`xvmulsp`/... with both operands
splatted to all lanes and a 2-instruction lane-0 merge back into the
destination. Guest `movss`/`movsd` memory traffic uses the VSX scalar indexed
loads/stores (`lxsiwzx`/`lxsdx` + `xxswapd`, `xxswapd` + `stxsiwx`/`stxsdx`),
which address the full 64-register VSR file.

**Register budget (measured):** the backend deploys essentially the whole
64-entry VSR file, and all 16 guest xmm registers are **statically pinned** —
they live in host vector registers permanently, across blocks, and are never
spilled by the JIT in steady state:

| VSRs | role |
|---|---|
| vs32-47 (v0-15) | **SRA: guest xmm0-15, statically allocated** (x64 mode) |
| vs48-61 (v16-29) | dynamic FPR allocation pool, 14 registers |
| vs62-63 (v30-31) | VTMP1/VTMP2 (lowering scratch) |
| vs16-31 | AVX ymm-high bank (guest ymm upper halves) |
| vs12, vs14 | VTMP3_VSX, VZERO_VSX (pinned zero, dw0 contract) |
| vs0-11, 13, 15 | host-call ABI / x87 / cvt scratch (f0-f13 classic-FP aliased) |

**Spill traffic that survives in real translated blocks (measured):**
JIT-added FPR spill/fill is **0.02% of emitted instructions** — 450 spill
stores + 94 fills in 2.33M host instructions across 2,782 translated CPython
blocks, and **zero** in a synthetic block that holds all 16 guest xmm live
simultaneously with dynamic temporaries on top. The guest binary's own
compiler-inserted spills (the survey's dominant traffic) translate 1:1 at 3 host
instructions per `movss` store/load and are architecturally non-elidable —
they are stores to addressable guest memory, not JIT decisions.

**Conclusion up front: the 64-register question is closed.** The full VSR file
is already exploited; guest xmm state is fully resident by construction; the
register allocator does not thrash. There is no meaningful headroom on the
"use more registers" axis. The headroom that *does* exist is per-op sequence
length, and one item dwarfs the rest: `cvttss2si` costs ~26 host instructions
per site because its x86 sentinel semantics are implemented **twice** (§6.2).

---

## 1. What is emitted for scalar SSE arithmetic, exactly

Source: `FEXCore/Source/Interface/Core/JIT/PPC64LE/VectorOps.cpp`,
`DEF_SCALAR_INSERT` (~line 3795) and `DEF_FMA_SCALAR_INSERT` (~line 4280).

Guest `addss xmm0, xmm1` becomes (verified in a live dump, block at guest
0x401013 → host 0x10031002060):

    xxspltw vs62, vs32, 3      # splat xmm0 elem0 to all lanes (VTMP1)
    xxspltw vs63, vs33, 3      # splat xmm1 elem0            (VTMP2)
    xvaddsp vs62, vs62, vs63   # 4-lane add, all lanes identical
    xxsldwi vs63, vs62, vs32, 3  # ┐ rotate result word into
    xxsldwi vs32, vs63, vs63, 1  # ┘ xmm0's element 0, upper 3 words kept

5 instructions for f32; f64 is 4 (`xxpermdi` splats, one `xxpermdi` merge).
FMA (`vfmadd231ss` etc.) is 6-7 with three-operand splats via VTMP3_VSX, with a
load-and-splat fusion (`lxvdsx`) that deletes the splat when the operand is a
single-use f64 load feeding the FMA.

**Why vector forms and not scalar forms.** The block comment records the
measured reason: on POWER8, `xs*`/`f*` scalar float ops take a ~22.8x denormal
assist penalty (measured on op4k, 2026-08-05, notes/denormal_bench.c) while
`xv*` vector ops run denormal-flat; guest audio DSP decays into denormals by
design and x86 games mask that with MXCSR.FTZ, which is not emulated. The
POWER9 UM (§4.3.2.1) states denormal operands/results are full-speed on
POWER9 (soft-patch assist removed, §4.3.6), so the cliff is POWER8-only — but
the vector-domain choice also wins on pure instruction count (5 vs the
predecessor's 9 for f32, §5) and keeps one code path correct on both the
POWER8 co-developer box and this POWER9. Splatting *both* operands (rather
than operating lanewise on real neighbor data) is a deliberate FPSCR-fidelity
choice: all lanes compute the same value, so sticky bits match the one real
computation.

**Loads/stores.** `LoadFPRSized`/`StoreFPRSized`
(`ArchHelpers/PPC64Emitter.cpp:595,672`): guest `movss [mem]` load is
`lxsiwzx` + `xxswapd` (2 insns; ISA 3.0 path, zero-extends and positions into
the LE-element-0 register image); `movsd` is `lxsdx` + `xxswapd`. Stores are
`xxswapd`-into-vs12 + `stxsiwx`/`stxsdx` (2 insns; never clobbers a VMX reg).
Aligned 16-byte guest accesses (`movaps`) are single `lvx`/`stvx`. RA spills,
when they happen at all, are full-width `stvx`/`lvx` against r1
(`MemoryOps.cpp` DEF_OP(SpillRegister/FillRegister)).

**Splat-chain elision exists but rarely fires (measured).** The
`ScalarSplatChain` IR pass (`IR/Passes/ScalarSplatChain.cpp`) can prove a
chain's upper elements unobserved and collapse a chain-internal scalar op to a
single `xv*`. But its own rule (d) makes the *last* write of a guest register
ineligible, and a compiler leaves results live-out in xmm registers at block
boundaries — so a `movss; subss; mulss; addss; mulss; subss; movss-store`
chain in the dumped block emitted the full 5-insn form for every link
(host 0x10031002614 ff.). The elision only pays when the guest reuses the
register later in the same block. Root constraint: `Core.cpp:851` flushes the
register cache before every guest instruction, so every scalar result is
StoreRegister'd to architectural state; the pass models that but cannot cross
block ends.

## 2. Per-instruction measured costs (live translated block dump)

Method: hand-assembled static x86-64 ELF (four scalar-SSE blocks: a typical
8-xmm compiler chain, an all-16-xmm + stack-spill block, the two named idioms,
a splat-chain shape), run under `build/Bin/FEX` with `FEX_BLOCKJITNAMING=1`,
host code dumped from the live process via gdb at the perf-map address and
disassembled with binutils. Whole loop: ~130 guest instructions → 421 host
instructions (3.2x static expansion).

| guest | host insns | sequence |
|---|---:|---|
| `movss xmm, [rbx+disp]` | 3 (2 if disp=0) | addi; lxsiwzx; xxswapd |
| `movss [rsp+disp], xmm` | 3 | addi; xxswapd→vs12; stxsiwx |
| `movsd xmm, [mem]` | 3 | addi; lxsdx; xxswapd |
| `addss/subss/mulss/divss r,r` | 5 | 2 splat + xv op + 2 merge |
| `addsd/mulsd r,r` | 4 | 2 splat + xv op + 1 merge |
| stack reload + `addss` | 8 | the two rows above composed |
| `movss xmm, xmm` (reg-reg) | **14** | vperm + inline 2×64-bit control build (§6.3) |
| `movsd xmm, xmm` (reg-reg) | 1 | xxpermdi |
| `cvttss2si r32, xmm` | **26** | §6.2: 12 backend + 14 redundant IR fixup |
| `cvttsd2si r32, xmm` | **21** | same shape |
| `movmskps r, xmm` | 7 | li; lvsl; vspltisb; vslb; vbpermq; mfvrd; clrldi |
| `pmovmskb r, xmm` | 6 | same minus clrldi |

All guest values stayed in their pinned SRA registers throughout; the
16-xmm-live block used vs32-47 concurrently plus VTMP1/2 and emitted **zero**
JIT spills. Latency note (inferred from POWER9 UM §4.3: ~6-cycle dependent FP
issue-to-issue, ~3-cycle permutes): the 5-insn form turns one dependent FP op
into permute→FP→permute→permute, roughly doubling dependency-chain latency for
serial scalar code — a plausible contributor to the GameThread's 0.61 IPC that
register-residency work would NOT have touched.

## 3. Aggregate spill measurement over a real binary (measured)

CPython 3.12 (x86-64, rootfs) running a float workload under FEX with block
naming; all 2,782 translated blocks (9.6 MB of host code) dumped live and
categorized:

  * total host instructions in named blocks: 2,325,286
  * **FPR spills (`stvx` to r1): 450 — 0.019%. FPR fills (`lvx` from r1): 94**
  * GPR r1-relative spill/fill: 5,009 — 0.2%
  * `vor` (SRA reg-reg moves the RA could not coalesce): 79 — negligible;
    LoadRegister/StoreRegister coalescing onto the pinned registers works.
  * 81,231 `stvx` + 9,627 `lvx` against r27 (STATE) are **not** block-body
    spills: they are SpillStaticRegs/FillStaticRegs at dispatcher/syscall
    crossings (5,005 exit sites × 16 xmm each, static count). Cold in linked
    steady state; the cost of a crossing, not of translation.

CPython is not FP-heavy; the synthetic block covers the FP-pressure case
(zero spills at 16 live xmm + temps). Between the two, JIT-added FP spill
traffic is not a contributor to the measured stall profile.

## 4. The register allocator's actual budget

`ArchHelpers/PPC64Emitter.h` namespace `x64` (x32 differs: 8 static + 22
dynamic): `SRAFPR` = 16 (v0-15), `RAFPR` = 14 (v16-29), wired verbatim into
the RA in `JIT.cpp:2493`. With VTMP1/2 that accounts for all 32 VMX-addressable
registers; the AVX-high bank (vs16-31) and the low-bank scratch/zero registers
account for the rest of the 64. Nothing models "only 32"; nothing reserves
most of the file. The one structural scarcity: **no free VMX register exists
for pinned constants** — every vperm/vbpermq control must be rebuilt inline or
loaded from the vconst pool, because v0-v31 are all assigned. (Low-bank VSRs
are free but only VSX-form-addressable, and their dw1 is not preserved across
host calls — the VZERO_VSX dw0-only contract documents that trap.)

## 5. The fex-ppc64le predecessor: did the merge lose anything? No.

Both trees share the 2026-05-11 POWER8 snapshot commit (`e1f83d4c4`, identical
in both repos). The same 16+14 SRAFPR/RAFPR banking exists in the predecessor's
`PPC64Emitter.h` — the static-registration design predates the split, and the
merge preserved it. On the scalar-FP lowering itself the two branches solved
the same problem a day apart and the current tree's answer is strictly newer
and better:

  * `fex-ppc64le` `62efc1b1f` (2026-08-04): "S4 — scalar SSE off the stack,
    VSX in-register" — replaced the stack round-trip with **`xs*` scalar
    forms** plus extract/insert permutes: 9 insns for f32, 4 for f64, subject
    to the POWER8 scalar denormal cliff.
  * `fastppcx86` `eaaa8fb1a` (2026-08-05): the vector-domain lowering — 5/4
    insns, denormal-flat, and the base for everything since (ScalarSplatChain,
    FMA load-splat fusion, the two-xxsldwi merge replacing pooled xxsel masks:
    "120 lvx mask loads inside ONE Wwise mixer block" per the block comment).

The predecessor's remaining differences are all *older*, not lost work: its
`Float_ToGPR_ZS` still bounces through JITScratch (`stvx` + `lfs`,
store-hit-load) where the current tree is register-only; its FMA scalar insert
is lanewise (2 insns cheaper than the current splat form but computes junk in
upper lanes — a weaker FPSCR-sticky fidelity the current tree explicitly
declined). The `vbpermq` movmsk path exists in both, and the current tree's
dispatcher carries an explicit MERGE NOTE (OpcodeDispatcher/Vector.cpp:859)
showing it was consciously re-homed during the merge so both callers get it.
**Verdict: the merge improved things; nothing to recover. Question closed.**

## 6. The two named idioms

### 6.1 movmsk family (~37k sites in the survey corpus): already near-optimal

`DEF_OP(VExtractSignBits)` (VectorOps.cpp:2541): one `vbpermq` does the whole
extraction; the 7-insn total is 1 gather + `mfvrd` (mandatory VSU→GPR
crossing) + `clrldi` + a 4-insn control-vector build (`li; lvsl; vspltisb;
vslb`) that touches no memory. The build is rebuilt per site; pooling it would
trade 3 ALU/permute insns for a load with load-use latency — the tree already
tried pooled masks for the scalar merge and measured against them. Possible
micro-win, not a lever. Compare the pre-vbpermq chain it replaced ("well over
a hundred host instructions"): this idiom is done.

### 6.2 cvttss2si sentinel semantics (~31k sites): the lever, confirmed — it is implemented twice

Measured per site (live dump, guest `cvttss2si eax, xmm2`): **26 host
instructions**, in two halves:

  1. Backend `DEF_OP(Float_ToGPR_ZS)` (ALUOps.cpp:4200) — 12 insns and
     **already fully x86-exact**: position elem0, `xscvspdp`, materialize the
     2^31/2^63 bound (li+sldi+mtvrd, 3 insns), `xscmpudp`, `xscvdpsxws`
     (truncates and saturates like fctiwz), `mfvrd`+`clrldi`, then a branch
     that overwrites with 0x80000000 on overflow/NaN. Output = x86's integer
     indefinite in every case.
  2. Dispatcher `CVTFPR_To_GPRImpl` (OpcodeDispatcher/Vector.cpp:2217) — the
     inherited `!SupportsFRINTTS` ARM-shaped fallback then emits a **second**
     sentinel fixup around the already-exact op: `LoadAndCacheNamedVectorConstant`
     (CVTMAX from STATE — a dependent load), an FGT scalar compare
     (`xxmrghd`/`xxsldwi`/`xscvspdp`×2/`xscmpudp`), and a GPR `_Select`
     (isel + two constant builds) — 14 more instructions that can never
     change the result.

**Proposal (build-ready):** in `CVTFPR_To_GPRImpl`, on this backend, return
`_Float_ToGPR_ZS` directly (the `HostRoundingMode` variant likewise maps to
`Float_ToGPR_S`, whose backend op carries the same internal fixup) — an
`#ifdef ARCHITECTURE_ppc64le` short-circuit in the dispatcher, deleting the
MaxF/MaxI/Select emission. 26 → 12 insns per site. Secondary (backend): the
bound is a per-site 3-insn rebuild; the LastConstantCache pattern or a
NamedVectorConstant slot would shave 2 more. Sizing the win (inferred, static
mix): convert-class is 2.1-7.2% of SIMD-class in the x64 survey rows — at
Cyberpunk's mix (5.7% SIMD, 2.3% convert), cvtt sites are ~0.13% of guest
instructions but ~3.4% of emitted host instructions at 26x; the dispatcher fix
alone halves that. Against the survey's "~31,000 sites, the main
fidelity/speed lever": confirmed, and the fidelity half is free — the backend
op is already exact, so this is pure deletion of dead belt-and-suspenders.

### 6.3 Bonus finding: reg-reg `movss` is 14 instructions

`movss xmm, xmm` lowers to `VInsElement` f32, which takes the generic
byte-perm path: `LoadPermCtrl` builds a 16-byte vperm control **inline** (two
5-insn 64-bit immediates + 2 `mtvrd`) + `vperm`. The i64 case (reg-reg
`movsd`) already has a 1-insn `xxpermdi` special case; the f32
DestIdx=0/SrcIdx=0 case (exactly reg-reg `movss`) has a 2-insn form sitting a
page away in the same file — the scalar-insert merge pair:
`xxsldwi(VTMP, Src, Dst, 3); xxsldwi(Dst, VTMP, VTMP, 1)`. On ISA 3.0,
`xxinsertw` may do it in one. 14 → 2. The survey puts reg-reg moves at a
4-7x minority of the move class, but the move class is half of all SIMD —
this is the cheapest fix per line of code in this report.

## 7. What NOT to do (explicitly closed)

  * **Do not port the lowering to `xsaddsp`/`xsadddp` scalar forms.** Same
    VSU pipes, no count win (the e0-positioning permutes just move), and on
    the POWER8 co-dev box it re-opens the 22.8x denormal cliff. The
    predecessor's version of exactly that was superseded within a day.
  * **Do not chase register-file exploitation.** 16 pinned + 14 dynamic + the
    AVX bank already consume the file; measured residual spill is 0.02%.
  * **Guest stack-spill promotion** (holding `[rsp+N]` slots in the 34 spare
    dynamic registers) is the only way to touch the survey's dominant traffic,
    and it is unsafe without aliasing proofs FEX cannot make — every slot is
    addressable guest memory. Not proposed.

If one change is built from this report, it is §6.2's dispatcher
short-circuit; the second is §6.3.
