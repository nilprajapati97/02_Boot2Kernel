# T0SZ / T1SZ: VA Range Size Formula and Implications

**Category**: Translation Control Register Deep Dive  
**Targeted**: ARM, Qualcomm

---

## 1. The Core Formula

```
Virtual Address Width = 64 - TxSZ  (where x = 0 or 1)

T0SZ (bits[5:0]) controls TTBR0_EL1 VA range (user space)
T1SZ (bits[21:16]) controls TTBR1_EL1 VA range (kernel space)

Valid range: TxSZ ∈ [16, 39] for 4KB granule with 48-bit VA
(lower TxSZ = larger VA range; higher TxSZ = smaller VA range)
```

---

## 2. T0SZ / T1SZ Values and Their VA Ranges

```
T0SZ │ VA bits │ VA range              │ Linux use
──────┼─────────┼───────────────────────┼────────────────────────
  12  │   52    │ 0–4 PB (4 petabytes)  │ LPA2 (ARMv8.7)
  16  │   48    │ 0–256 TB              │ Standard ARM64 (default)
  22  │   42    │ 0–4 TB                │ 64KB granule 3-level
  25  │   39    │ 0–512 GB              │ CONFIG_ARM64_VA_BITS_39
  28  │   36    │ 0–64 GB               │ 16KB granule, embedded
  32  │   32    │ 0–4 GB                │ Very restricted

For TTBR1 (kernel):
  T1SZ=16 → VA range: 0xFFFF000000000000 – 0xFFFFFFFFFFFFFFFF (256 TB)
  T1SZ=25 → VA range: 0xFFFFFF8000000000 – 0xFFFFFFFFFFFFFFFF (512 GB)
```

---

## 3. VA Split with Symmetric T0SZ = T1SZ

```
With T0SZ = T1SZ = 16 (48-bit VA, standard Linux):

User space (TTBR0 region):
  VA range: 0x0000_0000_0000_0000 – 0x0000_FFFF_FFFF_FFFF
  Size: 256 TB
  Canonical: VA[63:48] must all be 0x0000

Kernel space (TTBR1 region):
  VA range: 0xFFFF_0000_0000_0000 – 0xFFFF_FFFF_FFFF_FFFF
  Size: 256 TB
  Canonical: VA[63:48] must all be 0xFFFF

VA Hole (non-canonical):
  0x0001_0000_0000_0000 – 0xFFFE_FFFF_FFFF_FFFF
  Any access to this range → Translation Fault
  Size of hole: (2^64 - 2 × 2^48) ≈ 18.4 EB - 512 TB ≈ enormous gap

ASCII layout (48-bit VA):
  ┌─────────────────────────────┐ 0xFFFF_FFFF_FFFF_FFFF
  │   Kernel Virtual Space      │
  │   (TTBR1 region)            │ ← 256 TB
  ├─────────────────────────────┤ 0xFFFF_0000_0000_0000
  │                             │
  │   VA Hole (non-canonical)   │ ← 16.7 million TB hole
  │                             │
  ├─────────────────────────────┤ 0x0001_0000_0000_0000
  │   User Virtual Space        │
  │   (TTBR0 region)            │ ← 256 TB
  └─────────────────────────────┘ 0x0000_0000_0000_0000
```

---

## 4. Number of Page Table Levels vs T0SZ

For 4KB granule, the number of levels is determined by T0SZ (VA bits):

```
VA bits │ T0SZ │ Levels │ Bits per level   │ CONFIG_PGTABLE_LEVELS
────────┼──────┼────────┼──────────────────┼───────────────────────
  52    │  12  │   5    │ 4+9+9+9+9+12=52  │ 5
  48    │  16  │   4    │ 9+9+9+9+12=48    │ 4
  39    │  25  │   3    │ 9+9+9+12=39      │ 3 (PUD folded)
  32    │  32  │   3    │ 5+9+9+12=35→32   │ varies

Formula for level count (4KB granule):
  levels = ceil((VA_bits - 12) / 9)
  
  (VA_bits - 12) = bits to decode with 9-bit-per-level chunks
  ceil(36/9) = ceil(4) = 4 levels for 48-bit VA (36 bits above offset)
  ceil(27/9) = 3 levels for 39-bit VA
  ceil(40/9) = ceil(4.44) = 5 levels for 52-bit VA
```

---

## 5. Asymmetric T0SZ ≠ T1SZ

While Linux always uses T0SZ = T1SZ, the hardware supports asymmetric values:

```
Example: T0SZ=25 (39-bit user), T1SZ=16 (48-bit kernel)

User space: 512 GB maximum → 3-level page table for user
Kernel space: 256 TB maximum → 4-level page table for kernel

Use cases:
  - Embedded systems where user processes are small (< 512 GB)
  - Save user PGD memory (3-level PGD = 4 KB, vs 4-level = still 4 KB but
    one fewer level of allocation per walk)
  - Microkernel environments with restricted user VA

Risk: If user process tries to access VA > 512 GB → Translation Fault
  Some libraries assume 48-bit VA (e.g., they use upper bits for metadata)
  → May crash if T0SZ > 16 on standard Linux

Android uses T0SZ=25 (39-bit VA) on some configurations to match iOS
and improve compatibility with apps that fit in 512 GB.
```

---

## 6. TASK_SIZE and T0SZ in Linux

```c
// arch/arm64/include/asm/memory.h

// TASK_SIZE: maximum VA available to user processes
#define TASK_SIZE_64    (UL(1) << VA_BITS)
// For VA_BITS=48: TASK_SIZE_64 = 0x0001_0000_0000_0000 = 256 TB

// VA_BITS is set by CONFIG_ARM64_VA_BITS (39, 48, or 52)
// Controlled at compile time; T0SZ is set to (64 - VA_BITS) at boot

// Computing T0SZ from VA_BITS:
#define TCR_T0SZ_OFFSET  0
#define TCR_T0SZ(x)      ((64UL - (x)) << TCR_T0SZ_OFFSET)

// For 48-bit VA: TCR_T0SZ(48) = 64-48 = 16

// Reading T0SZ back:
u64 tcr = read_sysreg(tcr_el1);
int t0sz = (tcr >> TCR_T0SZ_OFFSET) & 0x3F;  // bits[5:0]
int va_bits = 64 - t0sz;   // = 48 for standard Linux
```

---

## 7. T0SZ and Security (KASLR Entropy)

```
T0SZ directly affects KASLR (Kernel Address Space Layout Randomization) entropy:

For T1SZ=16 (48-bit kernel VA):
  Kernel image KASLR range: VA[47:21] = 27 bits of randomization
  → 128 possible 2MB-aligned positions for kernel image

Wider T1SZ (smaller kernel VA) → less KASLR entropy → easier to predict address
  T1SZ=25 (39-bit kernel): only VA[38:21] = 18 bits KASLR entropy → 262144 positions
  T1SZ=16 (48-bit kernel): VA[47:21] = 27 bits KASLR entropy → 134 million positions

Linux CONFIG_RANDOMIZE_BASE:
  Uses the full T1SZ range for randomization.
  Each kernel boot picks a random offset within the VA range.
  Memory-mapped areas (vmalloc, linear map) also randomized.
```

---

## 8. T0SZ Change at Runtime (e.g., for compat 32-bit tasks)

```c
// For 32-bit (AArch32) user processes (compat ABI):
// The 32-bit process only uses VA[31:0] (4 GB user space)
// T0SZ doesn't need to change; the 32-bit VA space fits within
// the 48-bit TTBR0 region (4 GB << 256 TB)

// Linux does NOT change T0SZ at context switch for 32-bit compat tasks.
// TTBR0 is changed (process-specific page tables), but TCR_EL1 stays.
// The 32-bit process simply uses only the bottom 4 GB of the 48-bit user space.

// However, TTBR0 ASID and page tables are still swapped on context switch.
```

---

## 9. Interview Questions & Answers

**Q1: What is the formula for VA width from TxSZ, and what is T0SZ for standard 48-bit Linux?**

`VA_width = 64 - TxSZ`. For 48-bit VA (standard Linux ARM64), `T0SZ = 64 - 48 = 16`. Both T0SZ and T1SZ are set to 16 in standard Linux. This gives 256 TB user space (TTBR0) and 256 TB kernel space (TTBR1), with a non-canonical hole between `0x0001_0000_0000_0000` and `0xFFFE_FFFF_FFFF_FFFF`.

**Q2: Can T0SZ and T1SZ have different values, and what are the implications?**

Yes — they are independent fields. T0SZ controls TTBR0 (user) VA range; T1SZ controls TTBR1 (kernel) VA range. Different values create an asymmetric VA split. For example, T0SZ=25 (39-bit user, 512 GB) and T1SZ=16 (48-bit kernel, 256 TB) would be valid hardware configuration. The user page table would only need 3 levels while the kernel needs 4. However, Linux always uses symmetric values to ensure compatibility with userspace code that may allocate near the maximum VA.

**Q3: How does T0SZ affect page table levels?**

For 4KB granule, the number of levels = `ceil((64 - T0SZ - 12) / 9)`. With T0SZ=16: `ceil(36/9) = 4` levels. With T0SZ=25: `ceil(27/9) = 3` levels. With T0SZ=12 (LPA2 52-bit): `ceil(40/9) = ceil(4.44) = 5` levels. The walk starting level changes accordingly — with T0SZ=25, the L0 (PGD) level is folded and the walk starts at L1 (PUD), saving one memory access per TLB miss.

---

## 10. Quick Reference

| T0SZ | VA bits | VA range | Levels (4KB) | Linux config |
|---|---|---|---|---|
| 12 | 52 | 0–4 PB | 5 | LPA2 (ARMv8.7) |
| 16 | 48 | 0–256 TB | 4 | Default (ARM64_VA_BITS_48) |
| 22 | 42 | 0–4 TB | 3 | 64KB granule |
| 25 | 39 | 0–512 GB | 3 | ARM64_VA_BITS_39 |
| 28 | 36 | 0–64 GB | 3 | 16KB granule embedded |

| TxSZ formula | Value |
|---|---|
| `VA_bits = 64 - TxSZ` | The VA width |
| `T0SZ = 64 - VA_BITS` | Set from kernel config |
| `TASK_SIZE = 1 << VA_BITS` | Max user VA |
| `PAGE_OFFSET = -1 << (VA_BITS - 1)` | Kernel VA start |
