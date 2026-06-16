# TCR_EL1 — Translation Control Register

**Source:** `arch/arm64/mm/proc.S`, `arch/arm64/include/asm/pgtable-hwdef.h`

## Purpose

`TCR_EL1` controls **how** the MMU translates virtual addresses to physical addresses. It defines the page size, VA space size, cacheability of page table walks, and which physical address range the CPU supports.

## Register Layout (Key Fields)

```
TCR_EL1 (64 bits)
┌────┬────┬─────┬─────┬─────┬─────┬────┬────┬─────┬─────┬────┬────┐
│ DS │ .. │ IPS │ TG1 │ SH1 │ORG1 │IRG1│ .. │ T1SZ│ TG0 │ .. │T0SZ│
│[59]│    │[34:32]│[31:30]│[29:28]│[27:26]│[25:24]│ │[21:16]│[15:14]│ │[5:0]│
└────┴────┴─────┴─────┴─────┴─────┴────┴────┴─────┴─────┴────┴────┘
```

## Key Fields Explained

### T0SZ / T1SZ — Translation Size

```
T0SZ = 64 - IDMAP_VA_BITS     (for TTBR0, identity map)
T1SZ = 64 - VA_BITS           (for TTBR1, kernel VA space)
```

These define how many bits of the VA are used for translation:
- `T0SZ = 16` → 48-bit VA space for TTBR0 (identity map covers up to 256TB)
- `T1SZ = 16` → 48-bit VA space for TTBR1 (kernel space 0xFFFF_0000_0000_0000 → 0xFFFF_FFFF_FFFF_FFFF)

The formula: **VA bits = 64 - TxSZ**

### TG0 / TG1 — Translation Granule (Page Size)

| Value | TG0 (TTBR0) | TG1 (TTBR1) |
|-------|-------------|-------------|
| 0b00 | 4KB | — |
| 0b01 | 64KB | 16KB |
| 0b10 | 16KB | 4KB |
| 0b11 | — | 64KB |

Most ARM64 Linux systems use **4KB pages**.

### IPS — Intermediate Physical Address Size

Defines the maximum physical address the CPU can output:

| IPS Value | PA Bits | Physical Range |
|-----------|---------|----------------|
| 0b000 | 32 | 4GB |
| 0b001 | 36 | 64GB |
| 0b010 | 40 | 1TB |
| 0b011 | 42 | 4TB |
| 0b100 | 44 | 16TB |
| 0b101 | 48 | 256TB |
| 0b110 | 52 | 4PB (LPA2) |

Set by reading `ID_AA64MMFR0_EL1.PARange` — the CPU reports what it supports.

### SH1 / ORGN1 / IRGN1 — Cacheability of Page Table Walks

These control whether the MMU caches page table entries during translation:

```
SH1   = Inner Shareable   (TCR_SHARED)
ORGN1 = Write-Back, Write-Allocate (outer cache)
IRGN1 = Write-Back, Write-Allocate (inner cache)
```

Making page walks cacheable is a **huge performance win** — without it, every TLB miss requires 4 uncached memory reads (one per page table level).

### HA — Hardware Access Flag

```asm
mrs   x9, ID_AA64MMFR1_EL1
ubfx  x9, x9, ID_AA64MMFR1_EL1_HAFDBS_SHIFT, #4
cbz   x9, 1f
orr   tcr, tcr, #TCR_EL1_HA    ; enable HW Access flag update
```

If the CPU supports HAFDBS (Hardware Access Flag and Dirty Bit):
- **HA bit**: The CPU automatically sets the Access Flag in PTEs when a page is first accessed (instead of taking a fault)
- This avoids expensive software AF faults on every first access

### DS — Descriptor Size (LPA2)

For 52-bit physical addresses (LPA2), the `DS` bit changes how certain PTE bits are interpreted to accommodate the extra address bits.

## Value Set in `__cpu_setup`

```asm
mov_q tcr, TCR_T0SZ(IDMAP_VA_BITS) | TCR_T1SZ(VA_BITS_MIN) | \
           TCR_CACHE_FLAGS | TCR_SHARED | TCR_TG_FLAGS | \
           TCR_KASLR_FLAGS | TCR_EL1_AS | TCR_EL1_TBI0 | \
           TCR_EL1_A1 | TCR_KASAN_SW_FLAGS | TCR_MTE_FLAGS
```

## Two Translation Regions

TCR_EL1 controls **two independent** translation regions:

```
Virtual Address Space (64-bit):
0x0000_0000_0000_0000 ──────────────── 0xFFFF_FFFF_FFFF_FFFF

├─── TTBR0 region ────┤  (gap)  ├──── TTBR1 region ─────┤
     (user space)                     (kernel space)

Controlled by T0SZ,         Controlled by T1SZ,
TG0, ORGN0, IRGN0          TG1, ORGN1, IRGN1
```

- **TTBR0** (lower half): User-space address translation. Points to the identity map during boot.
- **TTBR1** (upper half): Kernel address translation. Points to `swapper_pg_dir` after boot.

## Key Takeaway

TCR_EL1 is the **master control knob** for the MMU. It defines the size of the VA space, the page granule size, how deep the page table walk goes, and whether page walks hit cache. Getting any field wrong causes immediate translation faults or, worse, silent corruption.
