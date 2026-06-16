# Day 06 — MMU Theory & Page Table Builders

> **Goal**: Understand ARMv8-A stage-1 translation; implement C helpers to build L0–L3 page-table entries (block and table descriptors); precompute the page tables in BSS but **don't enable MMU yet** (Day 7).
>
> **Why today**: Separating "build the tables" from "switch them on" keeps each step debuggable. Today: print the table contents from `printk`; tomorrow: flip `SCTLR_EL1.M`.

---

## 1. Background

### 1.1 ARMv8 Stage-1 (48-bit VA, 4 KiB granule)
```
 VA [47:39] L0 idx (512 entries)
    [38:30] L1 idx
    [29:21] L2 idx
    [20:12] L3 idx
    [11:0]  page offset
```
- 4 KiB granule → 9 bits per level, 4 levels = 36 + 12 = 48-bit VA.
- Two TTBRs: `TTBR0_EL1` (user, VA[63]=0), `TTBR1_EL1` (kernel, VA[63]=1).
- A **block** at L1 maps 1 GiB; at L2 maps 2 MiB; at L3 it's always a 4 KiB **page**.

### 1.2 Descriptor formats (lower 12 bits)
```
Bits      L0/L1/L2 Table          Block/Page
[1:0]     11  Table descriptor    01 Block / 11 Page (L3)
[4:2]     -                       AttrIndx (MAIR index 0..7)
[5]       -                       NS (Non-secure)
[7:6]     -                       AP[2:1]  (00=RW EL1, 01=RW any, 10=RO EL1, 11=RO any)
[9:8]     -                       SH       (00 non-share, 10 outer, 11 inner)
[10]      -                       AF       (Access flag — must be 1 or fault)
[11]      -                       nG       (not Global = ASID-tagged)
[47:12]   PA of next table        Output address (frame phys >> 12)
[51:48]   -                       Res0 / Reserved
[53]      PXNTable                PXN (Privileged Execute-Never)
[54]      UXNTable                UXN/XN
[59:55]   Software-defined
[63]      NSTable                 -
```

### 1.3 `MAIR_EL1` (Memory Attribute Indirection Register)
Eight 8-bit slots; `AttrIndx` (bits [4:2] of descriptor) selects one.
We define:
| Idx | Encoding | Meaning |
|---|---|---|
| 0 | `0xff` | Normal, WB cacheable inner+outer |
| 1 | `0x44` | Normal, non-cacheable |
| 2 | `0x00` | Device-nGnRnE (strongly ordered) |
| 3 | `0x04` | Device-nGnRE (for MMIO) |

### 1.4 `TCR_EL1` (Translation Control Register)
Key fields we set:
| Bits | Field | Value | Meaning |
|---|---|---|---|
| 5:0 | T0SZ | 16 | 64−16 = 48-bit VA for TTBR0 |
| 21:16 | T1SZ | 16 | same for TTBR1 |
| 9:8 | IRGN0 | 01 | WB WA inner cache |
| 11:10 | ORGN0 | 01 | WB WA outer cache |
| 13:12 | SH0   | 11 | Inner shareable |
| 15:14 | TG0 | 00 | 4 KiB granule TTBR0 |
| 25:24 | IRGN1 | 01 | |
| 27:26 | ORGN1 | 01 | |
| 29:28 | SH1   | 11 | |
| 31:30 | TG1 | 10 | 4 KiB granule TTBR1 (encoding differs!) |
| 34:32 | IPS | 010 | 40-bit phys (cortex-a72) |

---

## 2. Design

### 2.1 New files
```
arch/arm64/mm/mmu.c          (table builders, MAIR/TCR helpers)
arch/arm64/mm/pgtable.S      (TLBI ops, future)
include/asm-arm64/pgtable.h  (constants, PTE_* flags)
include/asm-arm64/mmu.h
```

### 2.2 Static page tables (BSS-allocated)
```c
__attribute__((aligned(4096))) static u64 ttbr0_l0[512];
__attribute__((aligned(4096))) static u64 ttbr1_l0[512];
__attribute__((aligned(4096))) static u64 l1_id[512];
__attribute__((aligned(4096))) static u64 l1_kern[512];
```
(Day 7 will write into them and load TTBR{0,1}.)

---

## 3. Implementation

### 3.1 `include/asm-arm64/pgtable.h`
```c
#ifndef _ASM_ARM64_PGTABLE_H
#define _ASM_ARM64_PGTABLE_H

#define PAGE_SHIFT      12
#define PAGE_SIZE       (1UL << PAGE_SHIFT)
#define PAGE_MASK       (~(PAGE_SIZE - 1))

#define PTE_VALID       (1UL << 0)
#define PTE_TABLE       (1UL << 1)
#define PTE_BLOCK       (0UL << 1)
#define PTE_PAGE        (1UL << 1)            /* L3 only */

#define PTE_ATTR(idx)   (((u64)(idx)) << 2)
#define PTE_AP_RW_EL1   (0UL << 6)
#define PTE_AP_RW_ANY   (1UL << 6)
#define PTE_AP_RO_EL1   (2UL << 6)
#define PTE_AP_RO_ANY   (3UL << 6)
#define PTE_SH_INNER    (3UL << 8)
#define PTE_AF          (1UL << 10)
#define PTE_NG          (1UL << 11)
#define PTE_PXN         (1UL << 53)
#define PTE_UXN         (1UL << 54)

#define MT_NORMAL       0
#define MT_NORMAL_NC    1
#define MT_DEVICE_nGnRnE 2
#define MT_DEVICE_nGnRE 3

#define PTE_NORMAL_RWX  (PTE_VALID | PTE_AF | PTE_SH_INNER | PTE_ATTR(MT_NORMAL) | PTE_AP_RW_EL1)
#define PTE_DEVICE_RW   (PTE_VALID | PTE_AF | PTE_ATTR(MT_DEVICE_nGnRE) | PTE_AP_RW_EL1 | PTE_PXN | PTE_UXN)

#endif
```

### 3.2 `arch/arm64/mm/mmu.c`
```c
#include <asm-arm64/pgtable.h>
#include <kernel/printk.h>
#include <kernel/types.h>

#define PTRS_PER_TABLE 512
#define IDX(va, lvl)   (((va) >> (39 - 9 * (lvl))) & 0x1ffUL)

__attribute__((aligned(4096))) u64 ttbr0_l0[PTRS_PER_TABLE];
__attribute__((aligned(4096))) u64 ttbr1_l0[PTRS_PER_TABLE];
__attribute__((aligned(4096))) u64 ttbr0_l1[PTRS_PER_TABLE];
__attribute__((aligned(4096))) u64 ttbr1_l1[PTRS_PER_TABLE];

void map_block_1g(u64 *l1, u64 va, u64 pa, u64 attrs)
{
    u32 idx = IDX(va, 1);
    l1[idx] = (pa & ~0x3fffffffUL) | attrs | PTE_BLOCK | PTE_VALID;
}

void mmu_build_tables(void)
{
    /* L0[0] -> L1 identity */
    ttbr0_l0[0] = ((u64)ttbr0_l1) | PTE_TABLE | PTE_VALID;
    /* L0[256] -> L1 kernel (VA bit 47 set means TTBR1; we use the matching low half) */
    ttbr1_l0[0] = ((u64)ttbr1_l1) | PTE_TABLE | PTE_VALID;

    /* Identity-map first 1 GiB as Normal memory (covers RAM + low MMIO) */
    map_block_1g(ttbr0_l1, 0x00000000, 0x00000000, PTE_NORMAL_RWX);
    /* MMIO 1 GiB block: 0x00000000..0x40000000 actually has both — refine in Day 7
       by using L2 blocks at 2 MiB so we can mark MMIO Device. */

    /* Higher-half: VA 0xFFFF_0000_4000_0000 → PA 0x4000_0000, 1 GiB block */
    map_block_1g(ttbr1_l1, 0x40000000, 0x40000000, PTE_NORMAL_RWX);

    printk(KERN_INFO "mmu: tables built, ttbr0_l0=%p ttbr1_l0=%p\n",
           ttbr0_l0, ttbr1_l0);
    printk(KERN_INFO "mmu: l1_id[1]=%p l1_kern[1]=%p\n",
           (void *)ttbr0_l1[1], (void *)ttbr1_l1[1]);
}
```

> Note: at L1, `IDX(0x40000000, 1) = 1`, so we populate `ttbr*_l1[1]`. Printing it sanity-checks the math.

### 3.3 MAIR/TCR helpers (used Day 7)
```c
u64 build_mair(void)
{
    return (0xffUL << (MT_NORMAL * 8))
         | (0x44UL << (MT_NORMAL_NC * 8))
         | (0x00UL << (MT_DEVICE_nGnRnE * 8))
         | (0x04UL << (MT_DEVICE_nGnRE * 8));
}

u64 build_tcr(void)
{
    u64 tcr = 0;
    tcr |= 16;                        // T0SZ
    tcr |= 16UL << 16;                // T1SZ
    tcr |= (1UL << 8)  | (1UL << 10) | (3UL << 12);   // TTBR0 cacheable, inner-share
    tcr |= (1UL << 24) | (1UL << 26) | (3UL << 28);   // TTBR1
    /* TG0=00 (4K), TG1=10 (4K) */
    tcr |= (2UL << 30);
    tcr |= (2UL << 32);               // IPS=40-bit
    return tcr;
}
```

---

## 4. ARM64 Cheat-Sheet (Day 6)

```
TTBR0_EL1   user/low VA root  (VA[47]=0 selects)
TTBR1_EL1   kernel/high VA    (VA[47]=1 selects)
MAIR_EL1    memory attribute LUT
TCR_EL1     translation control
ASID        in TTBR*_EL1 high 16 bits — Day 15
```

Memory types quick rule:
- RAM → MT_NORMAL (WB cacheable, inner-shareable)
- MMIO → MT_DEVICE_nGnRE
- Boot-time scratch → MT_NORMAL is fine

---

## 5. Pitfalls

1. **Forgetting `PTE_AF`**: first access faults — modern ARMv8 doesn't auto-set AF unless `TCR.HA=1`.
2. **Mixing block bit at L3**: at L3 the bit pattern `01` means *Reserved (invalid)*; you must use `11` (PTE_PAGE).
3. **Alignment**: all tables 4 KiB aligned; blocks at L1 require PA aligned to 1 GiB.
4. **Outer/inner shareability mismatch**: inconsistent SH between page tables and `TCR` causes UNPREDICTABLE on multi-core.
5. **Marking MMIO as Normal**: silently corrupts UART (writes coalesced/reordered) — always Device for MMIO.

---

## 6. Verification

Today MMU stays off; we only print the constructed tables. Expected:
```
[INFO] mmu: tables built, ttbr0_l0=0x... ttbr1_l0=0x...
[INFO] mmu: l1_id[1]=0x...0000_0701 l1_kern[1]=0x...0000_0701
```
(The `0x701` suffix = valid + block + AF + SH_inner + Normal attr — sanity check by hand.)

Unit test: write a small C helper that walks a VA through the static tables and returns the PA; assert `walk(0x40080000) == 0x40080000`.

---

## 7. Stretch

- Generate tables via L2 (2 MiB blocks) so MMIO holes (`0x0900_0000` UART) can be marked Device while RAM stays Normal.
- Add a `mmu_dump()` that prints all valid entries indented.
- Pre-compute kernel `_text`/`_rodata`/`_data`/`_bss` ranges from linker symbols for fine-grained perms on Day 29 hardening.

---

## 8. References

- ARM ARM §D5 (VMSAv8-64), §D5.3 (translation descriptors), §D13 (`MAIR_EL1`, `TCR_EL1`).
- Linux `arch/arm64/mm/proc.S`, `arch/arm64/include/asm/pgtable-hwdef.h`.
