# SIMD / Floating-Point — Questions & Answers

---

## Q1. [L1] What is NEON (Advanced SIMD)? How does it differ from scalar operations?

**Answer:**

```
NEON is ARM's SIMD (Single Instruction, Multiple Data) extension
for parallel processing of multiple data elements simultaneously.

Scalar vs SIMD:
  ┌──────────────────────────────────────────────────────────┐
  │ Scalar (one element at a time):                         │
  │   ADD W0, W1, W2        // 1 addition per cycle         │
  │                                                          │
  │ NEON (multiple elements at once):                       │
  │   ADD V0.4S, V1.4S, V2.4S  // 4 additions in 1 cycle! │
  │   (4 × 32-bit integers added simultaneously)            │
  │                                                          │
  │   ┌────────┬────────┬────────┬────────┐                 │
  │   │ V1[3]  │ V1[2]  │ V1[1]  │ V1[0]  │  128-bit V1   │
  │   │ 10     │ 20     │ 30     │ 40     │                 │
  │   └───┬────┴───┬────┴───┬────┴───┬────┘                 │
  │       +        +        +        +      (parallel ADD)  │
  │   ┌───┴────┬───┴────┬───┴────┬───┴────┐                 │
  │   │ V2[3]  │ V2[2]  │ V2[1]  │ V2[0]  │  128-bit V2   │
  │   │ 1      │ 2      │ 3      │ 4      │                 │
  │   └───┬────┴───┬────┴───┬────┴───┬────┘                 │
  │       =        =        =        =                      │
  │   ┌───┴────┬───┴────┬───┴────┬───┴────┐                 │
  │   │ V0[3]  │ V0[2]  │ V0[1]  │ V0[0]  │  128-bit V0   │
  │   │ 11     │ 22     │ 33     │ 44     │                 │
  │   └────────┴────────┴────────┴────────┘                 │
  └──────────────────────────────────────────────────────────┘

NEON register file:
  32 × 128-bit registers: V0-V31
  
  Access as different element sizes:
    V0.16B  → 16 × 8-bit  (byte)
    V0.8H   → 8 × 16-bit  (halfword)
    V0.4S   → 4 × 32-bit  (single/word)
    V0.2D   → 2 × 64-bit  (double/doubleword)
  
  Also addressable as:
    B0-B31: 8-bit (bottom byte of V register)
    H0-H31: 16-bit
    S0-S31: 32-bit (also used for scalar float)
    D0-D31: 64-bit (also used for scalar double)
    Q0-Q31: 128-bit (full register)

Key NEON instructions:
  Arithmetic: ADD, SUB, MUL, MLA (multiply-accumulate)
  Comparison: CMEQ, CMGT, CMGE
  Logical:    AND, ORR, EOR, BIC, BIF, BIT, BSL
  Shift:      SHL, USHR, SSHR, USRA
  Convert:    FCVTAS, UCVTF, SCVTF
  Load/Store: LD1 {V0.4S}, [X0]       (load 4 floats)
              LD2 {V0.4S, V1.4S}, [X0]  (deinterleave)
              ST1, ST2, ST3, ST4       (store/interleave)
  Permute:    TBL, TBX, ZIP, UZP, TRN
  Reduce:     ADDV, SMAXV, SMINV, FMAXV
```

---

## Q2. [L2] What is SVE (Scalable Vector Extension)? How does it differ from NEON?

**Answer:**

```
SVE (ARMv8.2+, mandatory in ARMv9) provides vector-length
agnostic SIMD with predication and gather/scatter.

NEON vs SVE:
  ┌──────────────────────────────────────────────────────────┐
  │ Feature       │ NEON              │ SVE                  │
  ├───────────────┼───────────────────┼──────────────────────┤
  │ Vector width  │ Fixed 128-bit     │ 128-2048 bit (VLA)  │
  │ Predication   │ No (mask+merge)   │ Yes (per-lane pred) │
  │ Gather/Scatter│ No                │ Yes                  │
  │ First-fault   │ No                │ Yes (speculative)   │
  │ Loop control  │ Manual            │ WHILE* instructions │
  │ Registers     │ V0-V31 (128-bit)  │ Z0-Z31 (128-2048b) │
  │ Predicates    │ None              │ P0-P15 (1 bit/byte) │
  │ Compatibility │ Recompile for     │ Same binary, any    │
  │               │ different width   │ vector length!       │
  └───────────────┴───────────────────┴──────────────────────┘

Vector Length Agnostic (VLA):
  ┌──────────────────────────────────────────────────────────┐
  │ SVE code doesn't know vector length at COMPILE time!    │
  │                                                          │
  │ Same binary runs on:                                     │
  │   CPU A: 128-bit SVE (Cortex-A510) → 4 floats/vector  │
  │   CPU B: 256-bit SVE (Neoverse V1)  → 8 floats/vector │
  │   CPU C: 512-bit SVE (Fugaku A64FX) → 16 floats/vector│
  │   CPU D: 2048-bit SVE (hypothetical) → 64 floats/vector│
  │                                                          │
  │ Code uses VL-agnostic loops:                            │
  │   WHILELT P0.S, X0, X1    // Set predicate for active  │
  │   loop:                     // elements (X0 < X1)       │
  │     LD1W Z0.S, P0/Z, [X2, X0, LSL #2]  // Load        │
  │     FADD Z0.S, P0/M, Z0.S, Z1.S        // Compute     │
  │     ST1W Z0.S, P0, [X3, X0, LSL #2]    // Store       │
  │     INCW X0               // Increment by VL/32        │
  │     WHILELT P0.S, X0, X1  // Update predicate          │
  │     B.FIRST loop           // Continue if any active    │
  │                                                          │
  │ INCW: adds vector_length/32 to X0                      │
  │   128-bit SVE: INCW adds 4                              │
  │   256-bit SVE: INCW adds 8                              │
  │   512-bit SVE: INCW adds 16                             │
  │                                                          │
  │ Same code, different throughput!                         │
  └──────────────────────────────────────────────────────────┘

Predication:
  P0-P15: predicate registers (1 bit per byte of vector)
  
  256-bit SVE with .S (32-bit) elements: 8 predicate bits
  
  // Process only elements where condition is true:
  FCMGT P1.S, P0/Z, Z0.S, Z1.S   // P1 = where Z0 > Z1
  FADD Z2.S, P1/M, Z2.S, Z3.S    // Add only where P1 is set
  
  /Z = zeroing (inactive elements = 0)
  /M = merging (inactive elements unchanged)

Gather / Scatter:
  // Load from non-contiguous addresses (indirect):
  LD1W Z0.S, P0/Z, [X0, Z1.S, UXTW #2]
  // Z1 contains indices, X0 is base address
  // Each lane: load from X0 + Z1[lane] * 4
  
  // Essential for: sparse matrix, hash tables, indirect arrays
```

---

## Q3. [L3] What is SVE2 and SME (Scalable Matrix Extension)?

**Answer:**

```
SVE2 (ARMv9): extends SVE with more instructions for general
purpose SIMD (replaces NEON use cases).

SME (ARMv9.2): adds matrix operations for AI/ML workloads.

SVE2 additions over SVE:
  ┌──────────────────────────────────────────────────────────┐
  │ • Widening/narrowing integer operations (like NEON)     │
  │   SADDLB, SADDLT: signed add long (bottom/top half)    │
  │   SQSHRUNB: saturating shift right narrow unsigned      │
  │                                                          │
  │ • Polynomial multiply (crypto):                         │
  │   PMULLB, PMULLT: polynomial multiply                   │
  │   → Replaces NEON PMULL for AES-GCM                    │
  │                                                          │
  │ • Complex number operations:                            │
  │   CADD, CMLA: complex add, multiply-accumulate          │
  │   → DSP and 5G signal processing                       │
  │                                                          │
  │ • Bitwise permutation:                                   │
  │   BDEP, BEXT, BGRP: bit deposit, extract, group        │
  │   → Compression, cryptography                          │
  │                                                          │
  │ • Histogram:                                             │
  │   HISTCNT, HISTSEG: count element occurrences           │
  │   → Database, analytics                                │
  │                                                          │
  │ • Cross-lane operations:                                │
  │   MATCH, NMATCH: character matching                     │
  │   → String processing, regex                           │
  └──────────────────────────────────────────────────────────┘

SME (Scalable Matrix Extension):
  ┌──────────────────────────────────────────────────────────┐
  │ New ZA register: 2D matrix (SVL × SVL bits)             │
  │                                                          │
  │ If SVL = 256 bits:                                       │
  │   ZA = 256 × 256 = 65536 bits = 8KB matrix             │
  │   For FP32: 8×8 matrix of single-precision floats      │
  │                                                          │
  │ If SVL = 512 bits:                                       │
  │   ZA = 512 × 512 = 262144 bits = 32KB matrix           │
  │   For FP32: 16×16 matrix                                │
  │                                                          │
  │ Key instruction:                                        │
  │   FMOPA ZA0.S, P0/M, P1/M, Z0.S, Z1.S                │
  │   // ZA0 += outer_product(Z0, Z1)                      │
  │   // Accumulates rank-1 updates into matrix             │
  │                                                          │
  │ GEMM (General Matrix Multiply) with SME:               │
  │   C[i][j] += A[i][k] * B[k][j]                        │
  │                                                          │
  │   For each k:                                            │
  │     Load A column into Z0                               │
  │     Load B row into Z1                                  │
  │     FMOPA ZA, Z0, Z1  // Outer product accumulate      │
  │                                                          │
  │ Performance:                                             │
  │   Without SME: matrix multiply = O(n³) scalar ops      │
  │   With SME: can compute n² results per FMOPA!          │
  │   → Orders of magnitude faster for ML inference        │
  └──────────────────────────────────────────────────────────┘

Streaming SVE mode (SSVE):
  SME introduces a new mode: PSTATE.SM (Streaming Mode)
  In streaming mode: SVE instructions use streaming VL
  (may be different from normal SVE VL)
  
  SMSTART: enter streaming mode
  SMSTOP:  exit streaming mode
  
  ZA register only accessible in streaming mode or via
  SMSTART ZA.
```

---

## Q4. [L2] How does ARM handle floating-point precision and rounding?

**Answer:**

```
ARM FP conforms to IEEE 754 standard with configurable rounding
and exception handling.

Supported formats:
  ┌──────────────────────────────────────────────────────────┐
  │ Format    │ Bits │ Sign│Exponent│Mantissa│ Range         │
  ├───────────┼──────┼─────┼────────┼────────┼───────────────┤
  │ FP16      │ 16   │ 1   │ 5      │ 10     │ ±65504       │
  │ BF16      │ 16   │ 1   │ 8      │ 7      │ ±3.4×10³⁸   │
  │ FP32      │ 32   │ 1   │ 8      │ 23     │ ±3.4×10³⁸   │
  │ FP64      │ 64   │ 1   │ 11     │ 52     │ ±1.8×10³⁰⁸  │
  └───────────┴──────┴─────┴────────┴────────┴───────────────┘
  
  FP16 vs BF16:
    FP16: more precision (10-bit mantissa), smaller range
    BF16: less precision (7-bit mantissa), same range as FP32
    BF16 advantage: truncation to/from FP32 is trivial
    → ML training: BF16 matches FP32 dynamic range, halves memory

FPCR (Floating-Point Control Register):
  ┌──────────────────────────────────────────────────────────┐
  │ RMode [23:22]: Rounding mode                            │
  │   00 = Round to Nearest, ties to Even (RNE) — default  │
  │   01 = Round towards Plus Infinity (RP)                 │
  │   10 = Round towards Minus Infinity (RM)                │
  │   11 = Round towards Zero (RZ)                          │
  │                                                          │
  │ FZ [24]: Flush-to-Zero mode                             │
  │   0 = IEEE compliant denormals (slow path)              │
  │   1 = Denormals flushed to zero (fast, less precise)   │
  │   Many ARM cores: denormal handling is 10-100x slower!  │
  │   → FZ=1 critical for performance in signal processing │
  │                                                          │
  │ AH [1]: Alternate Handling (ARMv8.7)                    │
  │ FIZ [0]: Flush Inputs to Zero                           │
  │ DN [25]: Default NaN mode                               │
  │ IDE [15]: Input Denormal exception enable               │
  │ IXE [12]: Inexact exception enable                      │
  │ UFE [11]: Underflow exception enable                    │
  │ OFE [10]: Overflow exception enable                     │
  │ DZE [9]:  Division by Zero exception enable             │
  │ IOE [8]:  Invalid Operation exception enable            │
  └──────────────────────────────────────────────────────────┘

FPSR (Floating-Point Status Register):
  Sticky flags (set by HW, cleared by SW):
    IOC: Invalid Operation (0/0, sqrt(-1))
    DZC: Division by Zero
    OFC: Overflow
    UFC: Underflow
    IXC: Inexact (rounding occurred)
    IDC: Input Denormal

FP performance considerations:
  FP64 (double): often 2x slower than FP32 on NEON/SVE
    128-bit NEON: 2 doubles vs 4 floats per register
  
  FP16 (ARMv8.2 FEAT_FP16):
    128-bit NEON: 8 half-precision per register
    → 2x throughput vs FP32, 4x vs FP64
    Used in: ML inference, image processing
  
  FMLA (Fused Multiply-Add):
    FMLA V0.4S, V1.4S, V2.4S  // V0 += V1 * V2
    One rounding (FMA) vs two (separate MUL + ADD)
    → More precise AND faster (1 instruction vs 2)
```

---

## Q5. [L2] How does ARM support ML/AI workloads? (DOT product, FP16, BF16, INT8)

**Answer:**

```
ARM added multiple features specifically for neural network
inference and training acceleration.

ML-specific instructions:
  ┌──────────────────────────────────────────────────────────┐
  │                                                          │
  │ 1. DOT Product (ARMv8.2 FEAT_DotProd):                 │
  │    SDOT V0.4S, V1.16B, V2.16B                          │
  │    // 16 INT8 multiplies + 4 INT32 accumulates          │
  │    // V0[0] += V1[0]*V2[0] + V1[1]*V2[1] +            │
  │    //          V1[2]*V2[2] + V1[3]*V2[3]               │
  │                                                          │
  │    Performance: 16 multiply-accumulates per instruction!│
  │    Perfect for: INT8 quantized neural networks          │
  │                                                          │
  │ 2. FP16 (ARMv8.2 FEAT_FP16):                           │
  │    FMLA V0.8H, V1.8H, V2.8H                            │
  │    // 8 FP16 fused multiply-adds per instruction       │
  │    // 2x throughput vs FP32                             │
  │                                                          │
  │ 3. BF16 (ARMv8.6 FEAT_BF16):                           │
  │    BFMMLA V0.4S, V1.8H, V2.8H                          │
  │    // BF16 matrix multiply-accumulate                   │
  │    // 2×4 BF16 matrix × 4×2 BF16 matrix → 2×2 FP32   │
  │    // Combines BF16 precision with FP32 accumulation    │
  │    → Training-friendly: FP32 range, half the memory     │
  │                                                          │
  │ 4. INT8 Matrix Multiply (ARMv8.6 FEAT_I8MM):           │
  │    SMMLA V0.4S, V1.16B, V2.16B                         │
  │    // INT8 matrix multiply-accumulate into INT32        │
  │    // 2×8 INT8 × 8×2 INT8 → 2×2 INT32                 │
  │    → Fastest path for quantized inference               │
  │                                                          │
  │ 5. SVE2 for ML:                                         │
  │    SQDMLALB, SQDMLALT: saturating doubling multiply-add│
  │    → Wider accumulator prevents overflow                │
  └──────────────────────────────────────────────────────────┘

Performance scaling for inference (per clock per core):
  ┌──────────────────────────────────────────────────────────┐
  │ Operation      │ NEON (128-bit)  │ SVE (256-bit)       │
  ├────────────────┼─────────────────┼─────────────────────┤
  │ FP32 FMLA      │ 8 FLOPs         │ 16 FLOPs            │
  │ FP16 FMLA      │ 16 FLOPs        │ 32 FLOPs            │
  │ BF16 BFMMLA    │ 32 FLOPs*       │ 64 FLOPs            │
  │ INT8 DOT       │ 32 OPs          │ 64 OPs              │
  │ INT8 MMLA      │ 64 OPs          │ 128 OPs             │
  └────────────────┴─────────────────┴─────────────────────┘
  * With 2 BFMMLA units
  
  INT8 quantized = 8-16x more throughput than FP32!
  → Quantized mobile models (MobileNet, EfficientNet) run
    at hundreds of FPS on Cortex-A78/X2

Compiler auto-vectorization:
  // C code:
  for (int i = 0; i < N; i++)
      sum += a[i] * b[i];
  
  gcc -O3 -march=armv8.2-a+dotprod:
    → Auto-generates SDOT instructions for INT8 arrays
    → 16x speedup vs scalar loop

  Frameworks: TFLite, ONNX Runtime, PyTorch Mobile
  All use NEON/SVE intrinsics for ARM acceleration.
```

---

## Q6. [L2] How do NEON/SVE load and store operations handle data layout?

**Answer:**

```
Structure load/store instructions handle interleaved data
formats commonly found in multimedia and signal processing.

Structure loads (deinterleave):
  ┌──────────────────────────────────────────────────────────┐
  │ LD2 — Load 2-element structures (deinterleave)         │
  │                                                          │
  │ Memory (interleaved RGB pairs):                         │
  │ [R0 G0 R1 G1 R2 G2 R3 G3]                             │
  │                                                          │
  │ LD2 {V0.4H, V1.4H}, [X0]                               │
  │                                                          │
  │ V0: [R0 R1 R2 R3]  (all R values)                      │
  │ V1: [G0 G1 G2 G3]  (all G values)                      │
  │                                                          │
  │ Now you can process all R and G values independently!   │
  │                                                          │
  │ LD3 — 3-element (RGB pixels):                           │
  │ Memory: [R0 G0 B0 R1 G1 B1 R2 G2 B2 R3 G3 B3]        │
  │ LD3 {V0.4S, V1.4S, V2.4S}, [X0]                       │
  │ V0: [R0 R1 R2 R3]                                      │
  │ V1: [G0 G1 G2 G3]                                      │
  │ V2: [B0 B1 B2 B3]                                      │
  │                                                          │
  │ LD4 — 4-element (RGBA pixels):                          │
  │ LD4 {V0.4S, V1.4S, V2.4S, V3.4S}, [X0]               │
  │ → 4 registers, each with one channel                   │
  └──────────────────────────────────────────────────────────┘

Structure stores (interleave):
  ST2, ST3, ST4: reverse of LD2/LD3/LD4
  Takes separate channel registers → interleaves to memory
  
  V0: [R0 R1 R2 R3]
  V1: [G0 G1 G2 G3]
  ST2 {V0.4H, V1.4H}, [X0]
  Memory: [R0 G0 R1 G1 R2 G2 R3 G3]

SVE equivalents with predication:
  LD2W {Z0.S, Z1.S}, P0/Z, [X0, X1, LSL #2]
  // Load pairs, but only where P0 is active
  // Handles non-aligned loop tails gracefully
  
  LD1RQW Z0.S, P0/Z, [X0]  // Load and replicate quadword
  // Broadcast 128-bit chunk across entire SVE vector
  // Useful for coefficient broadcast

Gather (SVE only):
  ┌──────────────────────────────────────────────────────────┐
  │ // Load from scattered addresses:                       │
  │ // indices[] = {3, 7, 1, 5, ...}                       │
  │                                                          │
  │ LD1W Z1.S, P0/Z, [X0]               // Load indices   │
  │ LD1W Z0.S, P0/Z, [X1, Z1.S, UXTW #2] // Gather       │
  │                                                          │
  │ // Z0[0] = memory[X1 + indices[0] * 4]                 │
  │ // Z0[1] = memory[X1 + indices[1] * 4]                 │
  │ // ...                                                   │
  │                                                          │
  │ Essential for:                                           │
  │   Sparse matrix operations                              │
  │   Histogram computation                                 │
  │   Hash table lookups                                    │
  │   Graph algorithms                                      │
  └──────────────────────────────────────────────────────────┘

First-fault loads (SVE):
  LDFF1W Z0.S, P0/Z, [X0, X1, LSL #2]
  // Load speculatively — if first element succeeds,
  // continue; if later elements fault (unmapped page),
  // silently truncate (set remaining predicate bits to 0)
  // → Safely loop without checking bounds!
```

---

## Q7. [L2] How does the compiler auto-vectorize code for ARM NEON/SVE?

**Answer:**

```
Compilers (GCC, Clang) can automatically generate SIMD code
from scalar C/C++ loops.

Compiler flags:
  NEON (always available in AArch64):
    gcc -O2 -ftree-vectorize (enabled by default at -O2)
    
  SVE:
    gcc -O2 -march=armv8.2-a+sve
    
  SVE2:
    gcc -O2 -march=armv9-a+sve2
  
  Additional ML features:
    -march=armv8.2-a+dotprod+fp16+bf16+i8mm

Auto-vectorization example:
  // C source:
  void add_arrays(float *a, float *b, float *c, int n) {
      for (int i = 0; i < n; i++)
          c[i] = a[i] + b[i];
  }
  
  // NEON output (-O2):
  .loop:
      LDP Q0, Q1, [X1], #32     // Load 8 floats from b
      LDP Q2, Q3, [X0], #32     // Load 8 floats from a
      FADD V0.4S, V2.4S, V0.4S  // Add 4 floats
      FADD V1.4S, V3.4S, V1.4S  // Add 4 floats
      STP Q0, Q1, [X2], #32      // Store 8 floats to c
      SUBS X3, X3, #8            // Decrement counter
      B.GT .loop
  
  // SVE output (-march=armv8.2-a+sve):
  .loop:
      WHILELT P0.S, X3, X4
      LD1W Z0.S, P0/Z, [X1, X3, LSL #2]
      LD1W Z1.S, P0/Z, [X0, X3, LSL #2]
      FADD Z0.S, P0/M, Z1.S, Z0.S
      ST1W Z0.S, P0, [X2, X3, LSL #2]
      INCW X3
      WHILELT P0.S, X3, X4
      B.FIRST .loop
  
  SVE advantage: same code works for ANY vector length!

Intrinsics (when compiler can't auto-vectorize):
  // NEON intrinsics (arm_neon.h):
  #include <arm_neon.h>
  float32x4_t va = vld1q_f32(a);
  float32x4_t vb = vld1q_f32(b);
  float32x4_t vc = vaddq_f32(va, vb);
  vst1q_f32(c, vc);
  
  // SVE intrinsics (arm_sve.h):
  #include <arm_sve.h>
  svfloat32_t va = svld1_f32(pg, a);
  svfloat32_t vb = svld1_f32(pg, b);
  svfloat32_t vc = svadd_f32_m(pg, va, vb);
  svst1_f32(pg, c, vc);

Vectorization hints:
  #pragma omp simd               // OpenMP SIMD directive
  __attribute__((optimize("tree-vectorize")))
  __restrict pointers             // No aliasing
  __builtin_assume_aligned(p, 16) // Alignment hint
```

---

Back to [Question & Answers Index](./README.md)
