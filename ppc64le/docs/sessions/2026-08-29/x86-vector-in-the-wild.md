# x86 vector code in the wild: what shipped games actually contain

**Session 2026-08-29/30 — research only, no code changes.**

A characterisation of x86/x86-64 SIMD as it exists in shipped third-party game
binaries, built from static analysis of 22 binaries across 11 titles
(1997–2025), the hand-tuned SIMD source of open engines, a native ppc64le
profile of Bullet physics, and same-source dual-target codegen experiments.
None of this project's performance reports, JIT lowerings, or benchmarks were
used as inputs. All game binaries analysed are MSVC (and some clang-cl)
output from CD Projekt, Valve, id, 11 bit, Firaxis, Nightdive, Bethesda/Virtuos,
NVIDIA, AMD and Intel — none contain this project's opinions.

---

## 1. Headline: the instruction mix, and how it varies by engine and era

**Measured** (static disassembly, `llvm-objdump -d`, full `.text` sweep;
method and limits in §2). Percentages are shares of the *SIMD-class*
instructions in each binary; `simd%` is SIMD as a share of all instructions.

| binary | year / engine | format | simd% | moves | scalar FP | packed FP | shuffle | logic | compare | convert | packed int |
|---|---|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| quake2.exe | 1997 / id Tech 2 | PE32 | 0.1 (x87: 3.0) | 11 | 1 | 11 | 14 | 10 | 8 | 4 | 37 |
| gamex86.dll (Q2) | 1997 / id Tech 2 | PE32 | 0.0 (x87: 9.6) | — | — | — | — | — | — | — | — |
| HL2 ep. client.dll | 2004→2011 / Source | PE32 | 8.4 | 54 | 32 | 2.2 | 0.8 | 2.4 | 4.6 | 3.5 | 0.5 |
| HL2 ep. server.dll | 2004→2011 / Source | PE32 | 8.3 | 56 | 36 | 0.1 | 0.1 | 1.8 | 5.5 | 1.3 | 0.1 |
| Portal 2 client.dll | 2011 / Source | PE32 | 9.2 | 60 | 24 | 2.1 | 0.7 | 5.1 | 4.1 | 4.0 | 0.1 |
| Portal 2 server.dll | 2011 / Source | PE32 | 9.7 | 62 | 24 | 0.5 | 0.2 | 6.4 | 5.3 | 2.2 | 0.1 |
| Dex.exe | ~2015 / Unity (32-bit player) | PE32 | 17.5 | 48 | 15 | 8.0 | 6.0 | 6.6 | 2.8 | 11.4 | 1.7 |
| witcher3.exe (x64) | 2015 / REDengine 3 | PE32+ | 8.5 | 60 | 23 | 3.6 | 3.1 | 4.2 | 3.2 | 2.1 | 0.3 |
| witcher3.exe (dx12) | 2022 next-gen / REDengine 3 | PE32+ | 8.4 | 61 | 23 | 3.5 | 3.1 | 4.3 | 3.2 | 2.1 | 0.3 |
| DOOMx64vk.exe | 2016 / id Tech 6 | PE32+ | 10.2 | 57 | 26 | 3.3 | 1.6 | 4.3 | 3.0 | 2.8 | 2.3 |
| CivilizationVI.exe | 2016 / Firaxis | PE32+ | 6.2 | 66 | 17 | 1.3 | 0.9 | 3.9 | 2.5 | 7.2 | 0.5 |
| Cyberpunk2077.exe | 2020–23 / REDengine 4 | PE32+ | 5.7 | 67 | 14 | 2.8 | 2.9 | 6.1 | 3.0 | 2.3 | 1.1 |
| quake2ex_steam.exe | 2023 / Kex (Q2 rerelease) | PE32+ | 8.5 | 60 | 19 | 1.0 | 1.4 | 9.5 | 3.7 | 4.5 | 0.6 |
| Frostpunk2 shipping | 2024 / UE5 | PE32+ | 12.9 | 61 | 8 | 10.2 | 7.7 | 4.6 | 2.2 | 3.2 | 2.0 |
| Oblivion Remastered | 2025 / UE5 | PE32+ | 12.5 | 60 | 9 | 9.4 | 7.2 | 4.8 | 2.5 | 3.3 | 2.5 |
| PhysX3_x64.dll | middleware (CP77) | PE32+ | 15.6 | 52 | 21 | 10.4 | 7.2 | 5.4 | 2.2 | 0.5 | 0.6 |
| PhysX3Common_x64.dll | middleware (CP77) | PE32+ | **48.5** | 48 | 20 | 13.6 | 8.4 | 4.8 | 3.2 | 0.5 | 0.1 |
| d3dcompiler_47.dll | middleware (W3) | PE32+ | 2.0 | 77 | 8 | 0.6 | 0.3 | 6.3 | 5.3 | 1.7 | 1.4 |
| amd_fidelityfx_dx12 | middleware (CP77) | PE32+ | 13.5 | 83 | 3 | 2.5 | 1.4 | 4.7 | 1.0 | 2.0 | 2.3 |
| OpenImageDenoise.dll | middleware (Oblivion) | PE32+ | 7.4 | 54 | 9 | 7.7 | 2.6 | 7.3 | 3.5 | 9.1 | 6.2 |

("moves" = full-width 16/32-byte loads/stores/reg-moves + scalar `movss`/`movsd`
+ element inserts/extracts/broadcasts. "packed FP" includes FMA. All rows
are full-`.text` linear sweeps. The 1997 rows are included as era markers —
Quake 2's game code is x87, and the ~175 SIMD-class instructions in
quake2.exe are MMX in the software renderer.)

**The five findings that hold across every column:**

1. **Data movement dominates everything.** 48–83% of SIMD-class instructions
   are moves. The single most common SIMD instruction in nearly every binary
   is `movss` or `movaps`. Memory-direction full-width moves outnumber
   reg-reg moves 4–7× — a large part of this is Win64 ABI traffic (xmm6–xmm15
   are callee-saved, so every non-leaf function that touches vectors spills
   them to the home area) plus working-set spills forced by 16 architectural
   registers.

2. **"SIMD" in game executables is mostly scalar.** In every pre-UE5 game
   executable, scalar SSE arithmetic (`mulss`/`addss`/`subss`…) is the
   biggest non-move category — 14–36% of SIMD instructions — because on
   x86-64 SSE *is* the FPU: every C++ `float` operation compiles to a scalar
   SSE instruction. True packed FP arithmetic is **0.5–4% of SIMD
   instructions (≈0.1–0.4% of all instructions)** in the 2004–2023 game
   executables. The vast majority of what an x86-on-VSX translator will see
   in the SIMD encoding space is scalar math and 16-byte block moves.

3. **The real vector code lives in middleware and, recently, UE5.**
   PhysX3Common is 48.5% SIMD-dense with 13.6% packed FP and 8.4% shuffle —
   that's what a hand-vectorised math library looks like. The UE5 titles
   (Frostpunk 2, Oblivion Remastered) are the only *game executables* that
   look like that: ~10% packed FP, ~7.5% shuffle, and — uniquely — heavy
   **packed double** (`mulpd`/`addpd`/`shufpd`, plus scalar `movsd`/`mulsd`),
   which is UE5's Large World Coordinates double-precision math. Nothing
   older uses packed doubles in any quantity.

4. **AVX presence is recent and mostly latent.** VEX-encoded share of SIMD:
   Source/Portal 0%, Witcher 3 / Cyberpunk ~0.5%, DOOM 2016 7.4%, Q2 Kex
   1.3%, **Frostpunk 2 27%, Oblivion Remastered 22%** (16.9% / 13.4% of
   their SIMD instructions touch a ymm register — ISPC modules and
   AVX2-compiled engine units). All binaries carry `cpuid` dispatch sites
   (OpenImageDenoise has 356 and ships SSE4/AVX2/AVX-512 function
   multiversions, the only AVX-512 in the corpus). **Presence is not
   execution**: these are runtime-dispatched paths, and a host that doesn't
   advertise AVX will see the SSE variants execute instead. Static counts
   here measure what's on disk, not what runs.

5. **Loop weighting does not change the ranking.** Weighting instructions
   inside backward-branch spans (crude loop detection, ≤4KB span) makes
   moves *more* dominant, not less, in every game executable. The only
   loop-specific shift is in the UE5 pair, where in-loop code shows a surge
   of lane-management traffic (`vextractf128`/`vinsertf128`, broadcasts,
   `shufpd`) — 256-bit code paying its cross-lane tax — and in the 1997/2015
   codec-ish binaries where packed-int pack/unpack rises.

**Era summary** (the spread is the finding):

- **1997**: x87 + a little MMX. No SSE.
- **1999** (ioquake3 source, cross-checked): still no SIMD in game code.
- **2004–2011, 32-bit MSVC** (Source): SSE used as a scalar FPU replacement
  plus `movss`-heavy data plumbing; packed FP ≤2%, shuffles ≈0. Hand-SIMD
  exists but is a tiny island (`rsqrtss` for fast inverse sqrt: 787
  occurrences in HL2 server.dll).
- **2015–2023, x64 MSVC** (REDengine, id Tech 6, Firaxis, Kex): same shape.
  SSE2 baseline scalar FP everywhere; packed FP 1–4%; SSE4.1 idioms appear
  (`dpps` 1,185× in Cyberpunk only, `haddps` 2,198× in Witcher 3 — both are
  DirectXMath-style dot products); DOOM adds a modest AVX path.
- **2024–2025, UE5 era**: a step change. Twice the SIMD density, a quarter
  VEX-encoded, packed doubles from LWC, ISPC-generated AVX2 kernels, real
  shuffle pressure. This is the direction of travel for future titles.

---

## 2. Method, and exactly what it can't tell you

**Measured.** `llvm-objdump -d --x86-asm-syntax=intel` linear sweep of each
binary's executable sections on the AC922 (llvm 22), piped through a
classifier (`/tmp/simdmix.py` on the AC922) that buckets every SIMD-class
mnemonic (SSE…AVX-512, VEX-aware, `movsd`/`cmpsd` string-instruction
ambiguity handled by requiring an xmm operand), tracks x87 separately,
detects the `xorps reg,reg` zeroing idiom, counts ymm/zmm operands and
`cpuid` sites, and applies crude loop weighting: any backward branch whose
target is within 4,096 bytes marks its span as a loop body, and SIMD
instructions inside such spans are counted again in the in-loop columns.
Raw JSON per binary: `/tmp/simdmix/out/*.json` on the AC922.

**Limits, stated plainly:**

- Static counts weight cold init code and hot loops equally; the loop
  weighting is a crude corrective, not a profile. Nothing here is an
  execution frequency.
- Linear-sweep disassembly can misparse data embedded in code (MSVC mostly
  keeps jump tables in `.rdata`, but not always). Error is diffuse noise,
  worst in the 1997 binaries.
- Runtime dispatch is invisible: a binary's AVX2 path inflates its static
  AVX share even on machines where it never executes. Counted `cpuid` sites
  indicate dispatch density but not selection. (Context, not an input: on
  this port AVX is not advertised, so guests take their SSE paths.)
- Win64 callee-save convention inflates the "moves" bucket relative to what
  a Linux/SysV build of the same code would show. This is real translated
  work all the same — the translator must execute those spills.
- The dual-target codegen experiments (§5) use Linux ABI clang/GCC, which is
  a strong instrument for the *mapping* question (what does this idiom
  become on VSX) but a weak one for "what do shipped Windows binaries
  contain" — the table above is the authority on the latter.

---

## 3. The demand signal from engine authors (read, not measured)

The most concentrated evidence about which operations *matter* is the set of
operations engine authors chose to hand-write. Read from upstream sources
(dhewm3 = id Tech 4 GPL, Bullet, ioquake3; clones under `/tmp` on the AC922).

**id Tech 4's `idSIMD` hierarchy** is a literal demand list: a virtual
interface of ~80 operations with Generic/MMX/SSE/SSE2/SSE3/AltiVec
implementations (`neo/idlib/math/Simd*.{h,cpp}`, 38k lines). The operations
that earned hand-tuning:

- elementwise float-array ops (add/sub/mul/div, mul-add) and the aligned
  "16-byte" variants used by the physics LCP solver;
- **dot products in seven AoS flavours** (vec3·array, plane·array,
  drawvert·array…), min/max reductions over vertex arrays, clamp;
- **compare-against-constant producing packed byte masks** (`CmpLT` et al.
  with a `bitNum` variant — cull-bit generation);
- dense matrix × vector / matrix × matrix, lower-triangular solves,
  LDLT factorisation (rigid-body solver);
- **skeletal animation**: quat↔matrix conversion, joint transform chains,
  vertex skinning (`TransformVerts`), blend of joint quats;
- geometry derivation: tri planes, tangents, point-vs-plane culling
  (`TracePointCull` — `cmpps` + `movmskps` + bit packing), shadow volume
  cache construction;
- **audio**: PCM up-sampling, speaker mixing (2/6 channel), and
  float→int16 conversion with saturation (`MixedSoundToSamples`).

Three structural lessons from the same tree:

1. **SSE1 captured essentially all the value.** The SSE override set is ~50
   operations; SSE2 re-overrides only 4 (integer/double corners: `CmpLT`
   bit-packing, triangular solves, float→int16); SSE3 exactly 1
   (`TransformVerts`, via `haddps` for AoS dots). 4×float32 with mul/add,
   shuffle, compare and min/max is the workload.
2. **The complete op set shipped on 128-bit PPC in 2004**: `Simd_AltiVec.cpp`
   implements the whole list. Notably it replaces the `movmskps` idiom with
   per-lane shifts + OR into a vector accumulator (no scalar mask round-trip).
   VSX-128 is a proven-sufficient target for this entire class of code.
3. **MMX was only ever worth it for `Memcpy`/`Memset`.**

**Bullet's intrinsic census** (grep across `src/`): the most-used intrinsic
in a hand-vectorised physics library is not arithmetic —

```
121 _mm_shuffle_ps   105 _mm_mul_ps    75 _mm_add_ps   48 _mm_movehl_ps
 46 _mm_movelh_ps     40 _mm_and_ps    27 _mm_sub_ps   25 _mm_set1_ps
 17 _mm_min/max_ps    16 _mm_load_ps   14 _mm_xor_ps   13 _mm_cmpeq_ps
 11 _mm_movemask_ps    8 _mm_shuffle_epi32           6 _mm_blendv_ps
```

Shuffle/permute + splat + half-register moves outweigh multiplies. Sign
manipulation is `and/xor` with constant masks. Branching on vector compares
goes through `movemask`. No horizontal-add instruction is used at all —
Bullet reduces via `shuffle+add`, and id only adopted `haddps` in its one
SSE3 function. **ioquake3 (1999) contains no game-code SIMD** beyond scalar
converts — confirming the era table from source.

**Godot (cross-check, read):** core `Vector3` is deliberately a bare
3-float struct with no SIMD — the maintainers' own proposal discussion
(godot-proposals #4544) documents why (API exposes fields; AoS memory
layout). A modern, actively-maintained engine choosing scalar math is
independent confirmation that engine-side game code is not where dense
vector work lives.

---

## 4. The recurring shapes (what the counts are made of)

From the corpus counts, the dense-region reads, and the sources — ranked
roughly by how much of the translated stream they will occupy:

1. **Scalar SSE float chains** (measured: the top mnemonics nearly
   everywhere). Compiled C++ float math: `movss` load → `mulss/addss/subss`
   chain → `comiss` + branch or `movss` store. DOOM's densest SIMD block is
   47 `mulss` + 37 `movaps` + 12 `addss` — a matrix/animation kernel the
   compiler never vectorised. Translation-relevant properties: long
   dependency chains, frequent reg-reg `movaps` (SSE's destructive 2-operand
   encoding forces copies), `xorps` zeroing, sign flips via `xorps` with
   `0x80000000` masks.

2. **16-byte block moves** (measured: `movaps`/`movups`/`movdqu` at 30–83%).
   Three distinct sources: inlined memcpy/struct copies (`movups` pairs),
   Win64 xmm callee-save spills (`movaps [rsp+disp]`), and value passing of
   4-float vectors kept in memory (AoS engine style). `movups` vs `movaps`
   split shows modern compilers default to unaligned loads — Civ 6 and
   FidelityFX are `movups`-dominated; alignment faults are not a thing
   compilers rely on any more (but `movaps` store alignment #GP semantics
   still exist in older code).

3. **4-wide AoS vector math** (measured in PhysX/UE5/Unity; source-confirmed
   in id/Bullet): dot, cross, normalize, lerp, min/max, all on `x,y,z,w` in
   one register. Built from `mulps/addps/subps` + **splat** (`shufps imm 0`
   or `vbroadcastss`) + **swizzle** (`shufps`, `unpcklps`, `movehl/movelh`).
   The 4×4 matrix transform and the AoS→SoA transpose (the
   `movlps/movhps/unpcklps/shufps` block, hand-commented in id's
   `Simd_SSE.cpp` for strided `idDrawVert` loads) are the canonical
   composites.

4. **Compare → mask → decide** (measured: ~37k `movmsk`-family sites
   corpus-wide — `movmskps` 7.5k + `vmovmskps` 1.2k + `pmovmskb` 0.5k in
   the pre-UE5 set, heavily in PhysX at 1,220 and Unity at 1,255, plus
   ~14.9k/12.7k in Frostpunk 2/Oblivion including `movmskpd` on their
   double math): `cmpps` then either (a) `movmskps eax` + scalar
   test/branch — culling, early-out, containment tests — or (b) branchless
   select via `andps/andnps/orps` (pre-SSE4) / `blendvps` (measured: ~4.8k
   blendv total, mostly DOOM and OIDN). id's cull functions pack four
   plane-test masks into cull bytes this way.

5. **Horizontal reductions** (measured: `haddps` 5.0k, `dpps` 1.2k —
   concentrated in Witcher 3 and Cyberpunk, i.e. DirectXMath-era MSVC;
   absent in UE5): AoS dot products compiled to `dpps` (SSE4.1) or
   `haddps×2`. Hand-written code avoids both (Bullet: zero occurrences) in
   favour of shuffle+add.

6. **Truncating float→int converts** (measured: `cvttss2si` 24.7k +
   `cvttsd2si` 6.7k + packed forms ~2.5k — the most numerous "awkward" op
   in every single binary): quantisation, grid indices, fixed-point audio,
   `(int)` casts. C++ truncation semantics force the `tt` forms.

7. **Byte shuffles and pack/saturate** (measured: `pshufb`-family small in
   game exes but dominant in dense codec/crypto regions; `packuswb/ssdw`
   ~2k, concentrated in the 32-bit Unity player = software image paths):
   pixel format conversion, audio float→s16 (`packssdw`+`packuswb` after
   `cvttps2dq` — the exact shape of id's `MixedSoundToSamples`), and
   crypto. Frostpunk 2's single densest SIMD block is a ChaCha-style hash:
   `vpaddd/vpxor/vpshufb/vpslld/vpsrld` — i.e. **the densest AVX2 code in a
   shipped AAA title is a hash function, not game math.**

8. **Approximate-math + refinement** (measured: `rsqrtss` 4.1k, `rcpps`
   0.8k, `rsqrtps` 0.6k): normalisation and divides via estimate +
   Newton-Raphson. Old Source code leans on `rsqrtss` hard; DOOM 2016 still
   carries 2,948 of them.

9. **256-bit lane management and element traffic** (measured, UE5 + DOOM
   only; per-UE5-binary counts: `vextractf128`+`vinsertf128` ≈74–96k,
   `vinsertps`/`vextractps`/`insertps`/`extractps` ≈60–120k, `vpermps`
   7.2–7.7k, `vperm2f128` up to 7k, `vzeroupper` 1.2–1.5k): AVX's split
   personality — cross-lane shuffles are restricted, so compilers emit
   explicit 128-bit half extracts/inserts around them, and AoS↔SoA element
   insertion shows up as dense `insertps`/`extractps` streams. This is the
   part of AVX that does *not* look like "two SSE ops glued together".
   **AVX2 gathers exist only here**: `vgatherdps`/`vgatherdpd`/`vpgatherdd`
   total ≈10.5k in Frostpunk 2 and ≈7.7k in Oblivion Remastered (ISPC/AVX2
   modules), and zero in every other binary in the corpus.

10. **Environment fiddling** (measured: `ldmxcsr` 575 + `stmxcsr` 428,
    spread thinly everywhere): FTZ/DAZ setup, rounding-mode saves around
    converts, and the odd `stmxcsr`-based exception poll. Rare but
    semantically load-bearing (denormal behaviour differs across hosts).

**Notably absent** (measured): AVX2 gathers in anything before 2024 — zero
across every pre-UE5 binary, and even in UE5 confined to CPUID-gated AVX2
paths (see item 9); AVX-512 outside OpenImageDenoise;
x87 in anything after 2011 beyond vestigial `fld/fstp` in long-double
library corners; `pcmpistri`-class string ops (a handful, all in zlib/CRT
corners); MMX after 1997.

**Native confirmation lane** (measured, ppc64le, no emulation): Bullet
compiled `-O2 -g -mcpu=power9` and profiled under `perf` on a headless 3,000
falling-box scene (its own benchmark #1 shape). Hot symbols:
`btDbvtBroadphase::setAabb` 11.4% (AABB min/max/compare), constraint solver
rows 18.6% combined (`gResolveSingleConstraintRow*` — dot products + FMA
chains on 4-float rows; note the profile lands in `_scalar_reference`
because Bullet's SIMD path is `#ifdef SSE` x86-only — a measured example of
the portable-fallback gap a VSX path closes), `dBoxBox2` SAT test 8.2%
(dot/cross/compare), then sorting and hashing. The hot shapes in a real
physics tick are exactly the short-vector idioms of items 3–5 — physics
loops, not long-array kernels.

---

## 5. Implications for a 128-bit VSX target (reasoned + codegen-verified)

Method for this section: the same eight representative kernels (4×4 matmul,
AoS dot array, horizontal sum, cmp→byte mask, AABB min/max, float→s16 with
saturation, BGRA swizzle, vec4 lerp) compiled with the same clang 22 for
x86-64 (SSE2 and AVX2+FMA) and ppc64le (`-mcpu=power8` and `-mcpu=power9`),
plus GCC 15 cross-checks (`/tmp/k_*.s` on the AC922). Inferences from the
ISA are labelled as such.

### What translates cleanly (the bulk, by the §1 counts)

- **Scalar SSE FP → VSX scalar ops.** `addss/mulss/…` map 1:1 to
  `xsaddsp/xsmulsp/…`; `comiss/ucomiss` to `fcmpu`/`xscmpudp` + CR read.
  The dominant instruction class in every game binary is the easy class.
  Bonus, verified in the kernel output: POWER has scalar and vector **FMA**
  (`xsmaddasp/xvmaddasp`); SSE has none, so every x86 `mulss`+`addss` pair
  is a fusion opportunity, not a cost.
- **16-byte moves → `lxv/stxv`.** On POWER9 unaligned vector loads are
  first-class, one instruction, no penalty; `movups`≡`movaps` collapses to
  the same `lxv`. (POWER8 caveat, verified: `-mcpu=power8` emits
  `lxvd2x + xxswapd` for every LE vector load/store — a real per-access tax
  that ISA 3.0 removed. On P8, move-dominated code pays ~2× instructions on
  its largest bucket; on P9 it pays nothing.)
- **Packed FP arith, min/max, compare, select, logicals** → `xvaddsp`,
  `xvminsp/xvmaxsp`, `xvcmpgtsp` (verified: produces the same all-ones lane
  masks), `xxsel`, `xxland/xxlxor`. The compare-mask-select branchless idiom
  is native. `blendvps` ≡ `xxsel` exactly.
- **Splat**: better than x86. Verified: `movss+shufps 0` from memory
  becomes a single `lxvwsx` (load-and-splat); register splat is `xxspltw`;
  `vbroadcastss` maps directly. UE5's broadcast-heavy loops get cheaper.
- **Zeroing idiom**: `xorps reg,reg` (measured at 1.5–8% of all SIMD
  instructions!) → `xxlxor vN,vN,vN`, same trick, and with 64 registers a
  translator can often keep a pinned zero register instead.
- **Approximate math**: `rsqrtss/rcpps` → `xsrsqrtesp/xvresp` estimates
  exist. (Inferred: precision differs — x86 guarantees ~12 bits, POWER
  similar but not bit-identical; NR-refined results converge, raw estimate
  comparisons can diverge. Fidelity risk, not performance risk.)
- **Pack/saturate + converts**: `cvttps2dq` → `xvcvspsxws` (also
  round-toward-zero); `packssdw/packuswb` → `vpkswss/vpkshus`. The audio
  float→s16 shape is native on both ends (AltiVec had saturating packs
  before SSE2 existed).

### Where the register file changes the game

Measured: memory-direction full-width moves outnumber reg-reg 4–7× in every
title, and reg-reg `movaps` (the destructive-encoding copy tax) is another
5–12% of moves. Both exist *because* x86 has 16 architectural vector
registers and 2-operand SSE encodings. VSX has **64 registers and
3-operand non-destructive encodings**: a translator holds all 16 guest xmm
in a quarter of the file and still has ~40 registers for temporaries,
pinned constants (zero, sign masks, permute controls), and fused-idiom
scratch. Guest spill/fill traffic must still be performed (it's
architecturally visible memory), but every *internal* copy the guest did
for encoding reasons folds away, and translated sequences never spill.

### What is genuinely awkward (the tail worth engineering)

Ranked by measured frequency × per-instance cost:

1. **`movmskps`/`movmskpd`/`pmovmskb` (~37k sites corpus-wide, ~9.2k
   outside UE5, hot in physics).** No
   single VSX instruction extracts sign bits to a GPR. Options: `vbpermq`
   with a sign-bit permute control + `mfvsrd` (2–3 instructions, POWER8+),
   or compare→shift→OR-reduce in-vector as id's AltiVec port did. The
   *pattern* `cmpps → movmskps → test/jcc` should be recognised whole and
   lowered to `xvcmp*.` setting CR6 (`vcmpgtfp.`-style dot forms give
   all/none for free) — the branch usually only tests all-zero/all-ones,
   and CR6 answers that in one instruction. Translating the mask literally
   is the slow path; translating the *question* is native.
2. **Truncating converts with sentinel semantics (`cvttss2si`, 31k+ sites).**
   The conversion itself maps (`xscvdpsxws` truncates), but x86 returns
   `0x80000000` ("integer indefinite") on NaN/overflow where POWER
   saturates. Bit-exact translation needs a NaN/range check around every
   convert unless relaxed. Given 31k static sites, this is the single
   biggest fidelity-vs-speed lever in the whole SIMD space. (Same issue
   family: DAZ/FTZ via MXCSR — POWER has no DAZ; denormal-sensitive code
   is rare but the ~1k `ldmxcsr/stmxcsr` sites show engines do set FTZ.)
3. **Horizontal ops (`haddps` 5k, `dpps` 1.2k, MSVC-era only).** No VSX
   analogue; lower to `xxsldwi`/`vsldoi` shuffle + `xvaddsp` chains (2 ops
   per `haddps`, ~4–6 for `dpps` including its mask immediate). Cost is
   modest and these are already rare in hand-tuned and post-2023 code —
   compilers moved away from them too.
4. **Immediate shuffle controls.** `shufps/pshufd/blendps` encode their
   pattern in an imm8; VSX's general `vperm/xxperm` needs the control in a
   register, loaded from the constant pool (verified in every ppc kernel:
   `.LCPI` loads via TOC). Fixed-function cases (`xxmrghw/xxmrglw/xxpermdi/
   xxsldwi/xxspltw`) cover the common two-source word patterns without
   constants; for the rest, 64 registers mean a translator can cache hot
   permute controls in pinned registers per trace — amortising what looks
   like a per-shuffle load down to near zero. `pshufb` maps to `vperm`
   almost 1:1 (same byte-table semantics, modulo the high-bit-zeroes rule
   needing one extra compare/select — and LE lane-order care).
5. **256-bit AVX (UE5-era).** Splitting `vaddps ymm` into two `xvaddsp` is
   mechanical and cheap ALU-wise — with 64 registers the pair-allocation
   pressure that kills 16-register hosts doesn't exist. The real cost
   concentrates in the measured lane-management ops (`vperm2f128`,
   `vextractf128`, cross-lane `vpermd/vpermps`) whose semantics span the
   halves — each becomes an `xxpermdi`/`xxperm` composition — in the
   **AVX2 gathers** (≈8–10k sites per UE5 binary, no VSX analogue at all:
   each lowers to 4–8 scalar loads plus element inserts, the one place a
   256-bit op costs an order of magnitude, softened only by VSX
   element-insert ops and spare registers) — and in `vzeroupper`, which is
   pure x86 microarchitectural hygiene and free to drop.
   Measured reality check: ymm-touching instructions are ≤0.3% of
   SIMD in everything before 2024, 13–17% in the UE5 pair — but every one
   of those binaries carries CPUID dispatch and an SSE fallback, so a host
   that doesn't advertise AVX never meets them. 128-bit translation quality
   is the whole game today; clean ymm splitting matters only if AVX
   advertisement is ever turned on.
6. **`maskmovps`-family masked memory (1.3k, DOOM's AVX path).** VSX has no
   masked store; needs select-and-store or per-lane branches. Rare enough
   to take the slow path.

### The POWER8 vs POWER9 delta (verified on real codegen)

For this exact workload the ISA 3.0 wins, in measured-frequency order:
`lxv/stxv` (kills the `xxswapd` tax on the #1 instruction class — a
P8-targeted translator pays two instructions on roughly half of all SIMD
sites), `lxvwsx` load-splat (every AoS broadcast), `lxssp/stxssp`
(single-float vector-register loads without conversion games),
`xscvdpspn`-based scalar/vector moves, and word-element
insert/extract (`vinsertw/vextractuw` — element traffic that P8 does via
permute). Nothing in the corpus needs P9's fancier additions (`vpermr`,
`darn`, half-precision converts appear only in OIDN's `cvtph2ps`, which
P9 has as `xvcvhpsp`).

---

## 6. External cross-checks (consulted after the numbers above)

- **MSVC x64 codegen**: SSE2 is the architectural baseline; all scalar FP
  is SSE by definition, and auto-vectorisation is conservative at default
  `/arch` ([Microsoft /arch:x64 docs](https://learn.microsoft.com/en-us/cpp/build/reference/arch-x64),
  [AVX-512 auto-vectorization in MSVC](https://devblogs.microsoft.com/cppblog/avx-512-auto-vectorization-in-msvc/)).
  Matches the measured scalar dominance and the thin packed share.
- **DirectXMath** ships SSE2-baseline with optional SSE3/SSE4 paths
  ([walbourn: DirectXMath SSE/NEON](https://walbourn.github.io/directxmath-sse-sse2-and-arm-neon/)) —
  explains `dpps`/`haddps` clustering in exactly the MSVC-era titles.
- **UE5 Large World Coordinates** doubles
  ([Epic LWC docs](https://dev.epicgames.com/documentation/en-us/unreal-engine/large-world-coordinates-in-unreal-engine-5)) —
  explains the packed-double signature unique to the UE5 pair.
- **Godot's scalar-math stance**
  ([godot-proposals #4544](https://github.com/godotengine/godot-proposals/discussions/4544)).
- **Other binary translators** (checked last, as agreed): Box64's RISC-V
  work found that efficiently translating **on the order of a hundred SSE
  instructions** covers hot code, with the pathological cases being
  hand-written SSSE3 codec kernels (dav1d), i.e. `pshufb`-dense byte code —
  and reports ~70% of native once vector translation replaced scalarisation
  ([Box64 + RVV](https://riscv.org/blog/box64-adds-initial-support-for-risc-v-vector-1-0-rvv-extension-achieves-up-to-300-performance-boost-code-now-open-source-and-merged-upstream/),
  [Box64 and RISC-V 2024](https://box86.org/2024/08/box64-and-risc-v-in-2024/)).
  Consistent with this corpus: the engine-side mix is small and boring; the
  dense tails are codec/crypto middleware. QEMU's scalarising approach is
  the known anti-pattern
  ([Improving SIMD parallelism via DBT](https://dl.acm.org/doi/pdf/10.1145/3173456)).

---

## 7. Bottom line

The honest finding is close to the "boring" hypothesis, with a precise tail:

1. **By volume, shipped-game SIMD is moves + scalar float math**, and both
   translate to VSX one-for-one or better (FMA fusion, load-splat, no
   unaligned penalty on P9, 64 registers absorbing the spill/copy churn
   that constitutes x86's biggest SIMD bucket). For everything from Source
   through REDengine 4, a translator that does nothing clever beyond clean
   1:1 lowering of ~40 mnemonics covers >95% of static SIMD sites.
2. **The transferable pattern knowledge**: recognise five composites rather
   than optimising mnemonics in isolation — (a) `cmpps→movmskps→test`
   lower to `xvcmp*.`/CR6; (b) splat-from-memory to `lxvwsx`; (c)
   `mul+add` chains to FMA; (d) `haddps`/`dpps` to shuffle-add trees; (e)
   `cvttss2si`'s sentinel semantics as a deliberate fidelity/speed policy
   point (31k sites). The hard ops are few, known, and enumerable — and
   AVX2 gathers, the classic translator nightmare, are *absent from every
   binary before 2024* and confined to CPUID-gated AVX2 paths in the two
   UE5 titles.
3. **Where dense vector code exists it is middleware** (physics, denoise,
   codec, crypto) and, from 2024 onward, **UE5 executables** — packed
   double, AVX2/ISPC, real shuffle pressure, 256-bit lane management.
   That is the era signature to plan for; it is also fully covered by
   CPUID-dispatched SSE fallbacks today.
4. **The 2004 AltiVec existence proof stands**: id shipped the entire
   hand-tuned SIMD demand list of a AAA engine on 128-bit PPC vectors.
   Nothing measured in the 2025 binaries changes the conclusion that
   128-bit VSX with 64 registers is a comfortable superset target for this
   workload — the deltas are all in semantics corners (mask extraction,
   convert sentinels, denormals), not in width or operation coverage.

**Artifacts** (on the AC922, `/tmp`): `simdmix.py` (classifier),
`simdmix/out/*.json` + `*.hard` (per-binary raw counts), `k_*.s` (dual-target
kernel codegen), `bulletbench` + `bullet.perf` (native profile),
`/tmp/dhewm3`, `/tmp/bullet3`, `/tmp/ioq3` (source clones).
