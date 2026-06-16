# TG0 / TG1 Granule Encoding: The Asymmetry Trap

**Category**: Translation Control Register Deep Dive  
**Targeted**: ARM, Qualcomm

---

## 1. The Encoding Asymmetry

`TCR_EL1.TG0` and `TCR_EL1.TG1` use **different encoding tables** for the same granule sizes. This is one of the most notorious ARM64 gotchas in BSP and bootloader code.

```
TG0 (bits[15:14]) — for TTBR0 granule:
  0b00 = 4 KB
  0b01 = 64 KB
  0b10 = 16 KB
  0b11 = Reserved (UNPREDICTABLE)

TG1 (bits[31:30]) — for TTBR1 granule:
  0b00 = Reserved (UNPREDICTABLE)
  0b01 = 16 KB
  0b10 = 4 KB   ← NOTE: NOT the same as TG0!
  0b11 = 64 KB  ← NOTE: NOT the same as TG0!
```

### Side-by-side comparison

```
Granule │ TG0 value │ TG1 value │ Same?
────────┼───────────┼───────────┼───────
 4 KB   │ 0b00 (0)  │ 0b10 (2)  │ NO ✗
16 KB   │ 0b10 (2)  │ 0b01 (1)  │ NO ✗
64 KB   │ 0b01 (1)  │ 0b11 (3)  │ NO ✗
```

This asymmetry is intentional by ARM to preserve backward compatibility (the original encoding reserved TG1=0b00) but it is a **constant source of bugs**.

---

## 2. Common Bug Pattern

```c
// WRONG: Using TG0 encoding for TG1 (common bootloader bug):
#define MY_GRANULE_4KB   0b00
tcr |= (MY_GRANULE_4KB << 30);   // TG1 = 0b00 = RESERVED (UNPREDICTABLE!)

// WRONG: Using the same macro for both:
#define GRANULE_4KB  (0 << 14)   // correct for TG0
tcr |= (GRANULE_4KB << 30);      // incorrect for TG1! Uses 0b00 = RESERVED

// CORRECT:
#define TCR_TG0_4K  (0b00 << 14)  // 0b00 for TG0 field
#define TCR_TG1_4K  (0b10 << 30)  // 0b10 for TG1 field (different!)

tcr = TCR_TG0_4K | TCR_TG1_4K | ...;
```

---

## 3. Linux Kernel Constants

```c
// arch/arm64/include/asm/pgtable-hwdef.h

// TG0 granule constants (bits[15:14]):
#define TCR_TG0_4K      (UL(0) << 14)   // 0b00 = 4 KB
#define TCR_TG0_64K     (UL(1) << 14)   // 0b01 = 64 KB
#define TCR_TG0_16K     (UL(2) << 14)   // 0b10 = 16 KB

// TG1 granule constants (bits[31:30]) — DIFFERENT ENCODING:
#define TCR_TG1_16K     (UL(1) << 30)   // 0b01 = 16 KB
#define TCR_TG1_4K      (UL(2) << 30)   // 0b10 = 4 KB
#define TCR_TG1_64K     (UL(3) << 30)   // 0b11 = 64 KB
// Note: 0b00 for TG1 = Reserved (never use!)

// Selected at compile time based on CONFIG_ARM64_xK_PAGES:
#ifdef CONFIG_ARM64_4K_PAGES
#define TCR_TG_FLAGS    (TCR_TG0_4K | TCR_TG1_4K)
#elif defined(CONFIG_ARM64_8K_PAGES)
// Not a real ARM64 granule — hypothetical
#elif defined(CONFIG_ARM64_16K_PAGES)
#define TCR_TG_FLAGS    (TCR_TG0_16K | TCR_TG1_16K)
#elif defined(CONFIG_ARM64_64K_PAGES)
#define TCR_TG_FLAGS    (TCR_TG0_64K | TCR_TG1_64K)
#endif
```

---

## 4. Consequences of Wrong TG Encoding

```
Scenario: Bootloader sets TG1 = 0b00 (thinking it means 4KB, but it's RESERVED)

Result:
  Hardware behavior is UNPREDICTABLE:
  - May use a random granule (hardware interprets 0b00 differently per microarch)
  - May use 4KB (some implementations treat RESERVED as 4KB by default)
  - May generate fault on first kernel page table walk
  - System hangs immediately or corrupts page tables silently

Debugging hint:
  If system hangs with random "Translation Faults" or "synchronous abort" in EL1
  right after MMU enable → check TG1 encoding first!

Verification:
  Read back TCR_EL1 after writing and verify TG0/TG1 bits:
  u64 tcr_read = read_sysreg(tcr_el1);
  assert((tcr_read >> 30) & 3) == 2);  // TG1 must be 0b10 for 4KB
```

---

## 5. Can TG0 ≠ TG1?

Technically yes — TTBR0 and TTBR1 can use different granule sizes:

```
Example: TG0=0b00 (4KB user pages), TG1=0b11 (64KB kernel pages)

Implications:
  - User space: 4KB pages, 4-level table, TASK_SIZE = 256TB
  - Kernel space: 64KB pages, 3-level table, huge kernel efficiency

Why Linux doesn't do this:
  1. Single kernel config controls PAGE_SIZE for BOTH user and kernel
  2. Kernel data structures (struct page, page tables) are sized for one granule
  3. Cannot have different granules for user/kernel (file page cache uses PAGE_SIZE)
  4. Would require completely separate page allocators per granule

Theoretical use: A hypervisor at EL2 might configure different granules
for different guests (each guest uses a different VMID and separate TCR).
```

---

## 6. Hardware Checks TG vs ID Register

```
Hardware checks that TG0/TG1 values are consistent with the hardware's
supported granules, reported in ID_AA64MMFR0_EL1:
  TGran4  [31:28]: 0b0000 = 4KB supported; 0b1111 = not supported
  TGran16 [23:20]: 0b0001 = 16KB supported; 0b0000 = not supported
  TGran64 [27:24]: 0b0000 = 64KB supported; 0b1111 = not supported

If TCR_EL1 requests a granule not supported by hardware:
  → Behavior is CONSTRAINED UNPREDICTABLE
  → Hardware may use a default granule or fault

Linux boot check (arch/arm64/kernel/cpufeature.c):
  __read_sysreg_s(SYS_ID_AA64MMFR0_EL1);
  // Verify TGran4/TGran16/TGran64 match the compiled CONFIG_ARM64_xK_PAGES
  // If mismatch → kernel BUG() or panic at boot
```

---

## 7. Quick Reference Table

```
To configure 4KB granule:
  TCR_EL1[15:14] = 0b00   (TG0 = 4KB)
  TCR_EL1[31:30] = 0b10   (TG1 = 4KB)

To configure 16KB granule:
  TCR_EL1[15:14] = 0b10   (TG0 = 16KB)
  TCR_EL1[31:30] = 0b01   (TG1 = 16KB)

To configure 64KB granule:
  TCR_EL1[15:14] = 0b01   (TG0 = 64KB)
  TCR_EL1[31:30] = 0b11   (TG1 = 64KB)

Memory aid: "TG1 for 4KB = 2" (0b10 in decimal = 2)
            "TG0 for 4KB = 0" (0b00 in decimal = 0)
```

---

## 8. Interview Questions & Answers

**Q1: What value should TCR_EL1.TG1 be set to for 4KB pages, and why is it different from TG0?**

`TG1 = 0b10` for 4KB. The reason for the different encoding is historical: when ARM designed the LPAE (Long Physical Address Extension for ARM32), the `TG1` field reserved `0b00` as a compatibility value, shifting the encodings compared to `TG0`. ARM64 inherited this asymmetry. `TG0 = 0b00` means 4KB, but `TG1 = 0b00` is Reserved. For 4KB pages: `TG0 = 0b00`, `TG1 = 0b10`. This is the correct answer expected in interviews and a real source of BSP bugs.

**Q2: Can you configure TTBR0 to use 4KB pages and TTBR1 to use 64KB pages?**

Technically yes — `TG0` and `TG1` are independent fields. You could set `TG0=0b00` (4KB TTBR0) and `TG1=0b11` (64KB TTBR1). But Linux never does this because the kernel's page size (`PAGE_SIZE`) is a single compile-time constant used for both user and kernel memory. The slab allocator, page cache, buddy allocator, and DMA APIs all assume one consistent page size. Using different granules for TTBR0 and TTBR1 would require fundamentally different allocation and caching infrastructure for user vs kernel memory, which Linux doesn't implement.

---

## 9. Quick Reference

| Granule | TG0[15:14] | TG1[31:30] | Linux TG0 const | Linux TG1 const |
|---|---|---|---|---|
| 4 KB | 0b00 | 0b10 | `TCR_TG0_4K` | `TCR_TG1_4K` |
| 16 KB | 0b10 | 0b01 | `TCR_TG0_16K` | `TCR_TG1_16K` |
| 64 KB | 0b01 | 0b11 | `TCR_TG0_64K` | `TCR_TG1_64K` |
| Reserved | 0b11 | 0b00 | Never use | Never use |
