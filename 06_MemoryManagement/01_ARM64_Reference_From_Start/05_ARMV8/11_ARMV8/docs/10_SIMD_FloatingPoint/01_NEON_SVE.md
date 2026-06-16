# NEON, SVE & SVE2

## 1. NEON (Advanced SIMD)

NEON processes multiple data elements in parallel using 128-bit vector registers.

```
SIMD concept: Single Instruction, Multiple Data

  Scalar add: ADD X0, X1, X2     → 1 addition
  
  NEON add:   ADD V0.4S, V1.4S, V2.4S  → 4 additions simultaneously!

  V1.4S:  [ A3 | A2 | A1 | A0 ]    (four 32-bit integers)
     +    
  V2.4S:  [ B3 | B2 | B1 | B0 ]
     =
  V0.4S:  [A3+B3|A2+B2|A1+B1|A0+B0]  (four results at once)

NEON data types and lane arrangements:
┌──────────────────────────────────────────────────────────────┐
│ Suffix │ Meaning               │ Elements per 128-bit reg   │
├────────┼───────────────────────┼────────────────────────────┤
│ .16B   │ 16 × 8-bit bytes     │ 16 lanes                   │
│ .8H    │ 8 × 16-bit halfwords │ 8 lanes                    │
│ .4S    │ 4 × 32-bit words     │ 4 lanes (int32 or float32) │
│ .2D    │ 2 × 64-bit doubles   │ 2 lanes (int64 or float64) │
│ .8B    │ 8 × 8-bit (low 64b)  │ 8 lanes (half register)    │
│ .4H    │ 4 × 16-bit (low 64b) │ 4 lanes                    │
│ .2S    │ 2 × 32-bit (low 64b) │ 2 lanes                    │
└────────┴───────────────────────┴────────────────────────────┘
```

---

## 2. Common NEON Instructions

```
Arithmetic:
  ADD   V0.4S, V1.4S, V2.4S    // Integer add (4 × 32-bit)
  FADD  V0.4S, V1.4S, V2.4S    // Float add (4 × single)
  FMUL  V0.2D, V1.2D, V2.2D    // Float multiply (2 × double)
  FMLA  V0.4S, V1.4S, V2.4S    // Fused multiply-accumulate
                                 // V0 = V0 + V1 × V2
  MUL   V0.8H, V1.8H, V2.8H    // Integer multiply (8 × 16-bit)
  SADDL V0.4S, V1.4H, V2.4H    // Signed add long (widen result)

Comparison:
  CMGT  V0.4S, V1.4S, V2.4S    // Compare greater: 0xFFFFFFFF or 0
  FCMGT V0.4S, V1.4S, V2.4S    // Float compare greater

Logical:
  AND   V0.16B, V1.16B, V2.16B // Bitwise AND
  ORR   V0.16B, V1.16B, V2.16B // Bitwise OR
  EOR   V0.16B, V1.16B, V2.16B // Bitwise XOR
  BIF   V0.16B, V1.16B, V2.16B // Bit insert if false
  BSL   V0.16B, V1.16B, V2.16B // Bitwise select

Load/Store:
  LD1   {V0.4S}, [X0]           // Load 4 floats contiguous
  LD2   {V0.4S, V1.4S}, [X0]    // Load + deinterleave (AoS→SoA)
  LD3   {V0.4S, V1.4S, V2.4S}, [X0]  // 3-way deinterleave
  ST1   {V0.4S}, [X0]           // Store 4 floats
  LD1R  {V0.4S}, [X0]           // Load + replicate to all lanes

Permute/Shuffle:
  TRN1  V0.4S, V1.4S, V2.4S    // Transpose (even elements)
  TRN2  V0.4S, V1.4S, V2.4S    // Transpose (odd elements)
  ZIP1  V0.4S, V1.4S, V2.4S    // Interleave lower halves
  UZP1  V0.4S, V1.4S, V2.4S    // Deinterleave even elements
  TBL   V0.16B, {V1.16B}, V2.16B  // Table lookup (permute)
  EXT   V0.16B, V1.16B, V2.16B, #4 // Extract (concat + shift)

Reduction:
  ADDV  S0, V1.4S               // Sum all 4 lanes → scalar
  FMAXV S0, V1.4S               // Max across all lanes
  SADDLV D0, V1.4S              // Sum all lanes, widen result
```

---

## 3. NEON Use Cases

```
1. Memcpy/Memset optimization:
   // Copy 64 bytes per iteration using NEON
   LDP Q0, Q1, [X1], #32      // Load 32 bytes
   LDP Q2, Q3, [X1], #32      // Load 32 more
   STP Q0, Q1, [X0], #32      // Store 32 bytes
   STP Q2, Q3, [X0], #32      // Store 32 more
   // 4x wider than GPR-based copy

2. Image processing (RGBA → grayscale):
   // Y = 0.299*R + 0.587*G + 0.114*B
   LD4 {V0.8B, V1.8B, V2.8B, V3.8B}, [X0]  // Load 8 RGBA pixels
   // V0=R, V1=G, V2=B, V3=A (deinterleaved!)
   UMULL V4.8H, V0.8B, V_coeffR.8B   // R * 77
   UMLAL V4.8H, V1.8B, V_coeffG.8B   // + G * 150
   UMLAL V4.8H, V2.8B, V_coeffB.8B   // + B * 29
   SHRN  V5.8B, V4.8H, #8            // >> 8 → 8 grayscale pixels

3. Matrix multiply (4×4 float):
   // Column 0 of result = mat_A × col0_of_B
   FMUL  V0.4S, V4.4S, V8.S[0]    // col0 * B[0][0]
   FMLA  V0.4S, V5.4S, V8.S[1]    // + col1 * B[1][0]
   FMLA  V0.4S, V6.4S, V8.S[2]    // + col2 * B[2][0]
   FMLA  V0.4S, V7.4S, V8.S[3]    // + col3 * B[3][0]

4. String operations (strlen):
   // Check 16 bytes for null terminator at once
   LD1   {V0.16B}, [X0], #16
   CMEQ  V1.16B, V0.16B, #0      // Compare each byte with 0
   SHRN  V1.8B, V1.8H, #4        // Narrow to get bitmask
   FMOV  X1, D1                   // Move to GPR
   RBIT  X1, X1                   // Reverse bits
   CLZ   X1, X1                   // Count leading zeros → position
```

---

## 4. Floating-Point

```
ARMv8 FP is IEEE 754 compliant:

Supported formats:
  ┌──────────────────┬──────┬──────┬───────┬────────────────────┐
  │ Format           │ Bits │ Exp  │ Mant  │ Range              │
  ├──────────────────┼──────┼──────┼───────┼────────────────────┤
  │ Half (FP16)      │ 16   │ 5    │ 10    │ ±65504             │
  │ BFloat16 (BF16)  │ 16   │ 8    │ 7     │ ±3.4×10³⁸          │
  │ Single (FP32)    │ 32   │ 8    │ 23    │ ±3.4×10³⁸          │
  │ Double (FP64)    │ 64   │ 11   │ 52    │ ±1.8×10³⁰⁸         │
  └──────────────────┴──────┴──────┴───────┴────────────────────┘

  FP16: useful for ML inference (lower precision, higher throughput)
  BF16: Machine Learning (same range as FP32, less precision)

FP Control/Status:
  FPCR (FP Control Register):
    • Rounding mode (RN/RP/RM/RZ)
    • Exception trap enables (Invalid, DivByZero, Overflow, ...)
    • FZ (Flush to Zero) — denormals become zero
    • AH, FIZ: ARMv8.7 alternate FP behaviors

  FPSR (FP Status Register):
    • Exception flags (cumulative: set by FP ops)
    • QC: saturation occurred (NEON saturating ops)

Common FP instructions:
  FADD  S0, S1, S2       // Single-precision add
  FMUL  D0, D1, D2       // Double-precision multiply
  FMADD S0, S1, S2, S3   // Fused multiply-add: S0 = S3 + S1*S2
  FCVTZS W0, S1           // Float → signed int (truncate)
  SCVTF  S0, W1           // Signed int → float
  FRINTX S0, S1           // Round to integral (exact)
  FSQRT  D0, D1           // Square root
  FDIV   S0, S1, S2       // Division
```

---

## 5. SVE — Scalable Vector Extension (ARMv8.2)

```
Problem with NEON: Fixed 128-bit vectors
  • Code compiled for 128-bit won't use wider hardware
  • Different chips want different widths (128, 256, 512...)
  • Recompilation needed for each vector width

SVE solution: Vector Length Agnostic (VLA) programming
  • Vector width: 128 to 2048 bits (in increments of 128)
  • SAME binary runs on ALL SVE implementations
  • Hardware determines width at runtime

  ┌──────────────────────────────────────────────────────────────┐
  │  Implementation       │ SVE Vector Length  │ 32-bit elements │
  ├───────────────────────┼────────────────────┼─────────────────┤
  │  Fujitsu A64FX        │ 512 bits           │ 16 elements    │
  │  AWS Graviton3        │ 256 bits           │ 8 elements     │
  │  ARM Neoverse V1      │ 256 bits           │ 8 elements     │
  │  ARM Neoverse N2      │ 128 bits (SVE2)    │ 4 elements     │
  │  Cortex-A510 (SVE2)   │ 128 bits           │ 4 elements     │
  └───────────────────────┴────────────────────┴─────────────────┘

  Same code → different performance → no recompilation!
```

---

## 6. SVE Programming Model

```
SVE registers:
  Z0-Z31:  Scalable vector registers (128-2048 bits)
  P0-P15:  Predicate registers (1 bit per byte of vector length)
  FFR:     First Fault Register (speculative loads)
  ZCR_EL1: SVE Control Register (set vector length)

  Z registers OVERLAY NEON V registers:
    Z0[127:0] = V0 (NEON view)
    Z0[VL-1:128] = SVE extension

Predication (key SVE feature):
  Predicates mask which lanes are active:
  
  VL=256 bits, 8 × 32-bit elements:
  P0 = [1, 1, 0, 1, 1, 0, 1, 1]
  
  ADD Z0.S, P0/M, Z1.S, Z2.S
  → Only performs add for lanes where P0=1
  → Lanes with P0=0 are unchanged (Merge) or zeroed (Zero)
  
  This eliminates branch divergence in SIMD code!

Loop structure (VLA):
  // Process array of N floats
  MOV   X1, #0                    // i = 0
  WHILELT P0.S, X1, X_N           // P0 = (i < N) per lane
loop:
  LD1W  {Z0.S}, P0/Z, [X_src, X1, LSL #2]  // Load active elements
  LD1W  {Z1.S}, P0/Z, [X_dst, X1, LSL #2]
  FADD  Z1.S, P0/M, Z1.S, Z0.S             // Add
  ST1W  {Z1.S}, P0, [X_dst, X1, LSL #2]    // Store active elements
  INCW  X1                                   // i += VL/32
  WHILELT P0.S, X1, X_N                     // Update predicate
  B.FIRST loop                               // Any active lanes?

  Key: INCW increments by VL/32 (vector length / element size)
  On 256-bit SVE: INCW adds 8
  On 512-bit SVE: INCW adds 16
  Same binary, different throughput!
```

---

## 7. SVE Key Instructions

```
SVE instruction categories:

Vector arithmetic:
  ADD    Z0.S, Z1.S, Z2.S          // Unpredicated add
  FADD   Z0.S, P0/M, Z1.S, Z2.S   // Predicated FP add
  FMLA   Z0.S, P0/M, Z1.S, Z2.S   // Fused multiply-accumulate
  MUL    Z0.S, P0/M, Z0.S, Z1.S   // Integer multiply
  SDOT   Z0.S, Z1.B, Z2.B         // Signed dot product (int8→int32)
  FSCALE Z0.S, P0/M, Z0.S, Z1.S   // Fast floating-point scaling (2^n)

Predicate generation:
  WHILELT P0.S, X0, X1             // P0 = (X0+lane < X1) per lane
  PTRUE   P0.S                     // All lanes active
  PFALSE  P0.B                     // All lanes inactive
  CMPEQ   P0.S, P1/Z, Z0.S, Z1.S  // Compare equal → predicate
  CMPGT   P0.S, P1/Z, Z0.S, #5    // Compare > immediate

Gather/Scatter:
  LD1W  {Z0.S}, P0/Z, [X0, Z1.S, UXTW #2]  // Gather load
  ST1W  {Z0.S}, P0, [X0, Z1.S, UXTW #2]    // Scatter store
  // Load/store from non-contiguous addresses (Z1 = indices)

Reductions:
  FADDV  S0, P0, Z1.S    // Sum all active elements → scalar
  FMAXV  S0, P0, Z1.S    // Max of active elements
  UADDV  D0, P0, Z1.S    // Unsigned sum → 64-bit scalar

First-fault loads (speculative):
  LDFF1W {Z0.S}, P0/Z, [X0, X1, LSL #2]
  // Load speculatively — if a later element faults,
  // just set FFR to mark which elements are valid.
  // Used for: processing until end-of-buffer without
  // knowing exact length.
  RDFFR  P1.B              // Read first-fault register
```

---

## 8. SVE2 (ARMv9)

```
SVE2 extends SVE with instructions from NEON that were missing:

SVE2 additions:
┌────────────────────────────────────────────────────────────────┐
│ Category              │ Instructions                           │
├───────────────────────┼────────────────────────────────────────┤
│ Narrowing operations  │ SQSHRNB, SQSHRNT, UQSHRNB            │
│ Widening operations   │ SADDLB, SADDLT, SMULLB, SMULLT       │
│ Complex arithmetic    │ CADD, CMLA, SQCADD (complex rotation) │
│ Polynomial multiply   │ PMULLB, PMULLT (crypto/CRC)           │
│ Bitwise permutation   │ BGRP, BDEP, BEXT (bit manipulation)   │
│ Histogram             │ HISTCNT, HISTSEG (image processing)   │
│ Match                 │ MATCH, NMATCH (string search)          │
│ Table lookup          │ TBL, TBX (extended lanes)              │
│ Saturating arithmetic │ SQADD, UQADD, SQSUB (DSP)            │
│ Crypto (optional)     │ SM4E, AESE, RAX1 (in SVE2 width)     │
└────────────────────────────────────────────────────────────────┘

SVE2 makes SVE a complete superset of NEON:
  NEON: 128-bit, fixed width
  SVE:  128-2048 bit, scalable, VLA loops + predicates
  SVE2: SVE + all NEON DSP/crypto operations in scalable form

ARMv9 baseline: SVE2 at minimum 128-bit (same as NEON width)
  Every ARMv9 core supports SVE2 → portable vector code
```

---

## 9. SME — Scalable Matrix Extension (ARMv9.2)

```
SME adds hardware-accelerated matrix operations:

  ┌────────────────────────────────────────────────────────────┐
  │  SME key features:                                          │
  │  • ZA: 2D matrix tile register (up to SVL × SVL bits)     │
  │  • Outer product accumulate instructions                    │
  │  • Streaming SVE mode (separate SVE pipeline for matrix)   │
  │                                                              │
  │  FMOPA ZA0.S, P0/M, P1/M, Z0.S, Z1.S                     │
  │    → Matrix outer product: ZA += Z0 ⊗ Z1                  │
  │    → In one instruction: updates VL/32 × VL/32 elements!  │
  │                                                              │
  │  On 256-bit SVE: 8×8 = 64 FP32 multiply-adds per cycle!  │
  │  On 512-bit SVE: 16×16 = 256 FP32 multiply-adds per cycle!│
  │                                                              │
  │  Use cases:                                                  │
  │  • Neural network matrix multiplications (LLM inference)   │
  │  • Scientific computing (BLAS GEMM)                        │
  │  • Signal processing                                        │
  │                                                              │
  │  SME2 (ARMv9.4): Multi-vector instructions, more types     │
  └────────────────────────────────────────────────────────────┘
```

---

## 10. FP16, BF16 & ML Extensions

```
Half-precision and ML-focused formats:

  FP16 (IEEE 754):  1|5 exponent|10 mantissa
  BF16 (Brain FP):  1|8 exponent|7 mantissa

  BF16 advantage: same exponent range as FP32
    → Direct truncation of FP32 → BF16 (just drop lower mantissa)
    → Good for neural network training (where range matters more)

ML instructions added progressively:
┌──────────────┬───────────────────────────────────────────────┐
│ Extension    │ Instructions                                   │
├──────────────┼───────────────────────────────────────────────┤
│ FEAT_FP16    │ FADD, FMUL etc. on FP16 vectors (8×half)     │
│ (ARMv8.2)   │ 2× throughput vs FP32                         │
├──────────────┼───────────────────────────────────────────────┤
│ FEAT_DotProd │ SDOT, UDOT (int8 dot product → int32)         │
│ (ARMv8.2)   │ 4 int8 muls + accumulate per lane            │
│              │ Key for: int8 quantized ML inference          │
├──────────────┼───────────────────────────────────────────────┤
│ FEAT_BF16    │ BFMMLA, BFDOT, BFCVT                        │
│ (ARMv8.6)   │ BF16 matrix multiply + dot product           │
├──────────────┼───────────────────────────────────────────────┤
│ FEAT_I8MM    │ SMMLA, UMMLA, USMMLA                         │
│ (ARMv8.6)   │ Int8 matrix multiply (2×8 × 8×2 → 2×2 int32)│
│              │ Key for: int8 quantized ML                    │
├──────────────┼───────────────────────────────────────────────┤
│ FEAT_FAMINMAX│ FAMIN, FAMAX (absolute min/max)              │
│ (ARMv9.4)   │ For activation functions                      │
└──────────────┴───────────────────────────────────────────────┘

ML pipeline on ARM:
  Model weights (INT8) × Activations (INT8)
    → SMMLA/SDOT (hardware accelerated)
    → INT32 accumulation
    → Requantize to INT8 or convert to FP16/FP32
    → Next layer

  Performance: Cortex-X4 @ 3.4GHz SVE2:
    ~50 TOPS (INT8) — competitive with dedicated NPU for small models
```

---

Next: Back to [SIMD & FP Overview](./README.md) | Return to [Main Index](../README.md)
