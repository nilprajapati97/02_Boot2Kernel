# ARM64 MMU System Registers

## 1. Overview

Five system registers must be configured **before** `SCTLR_EL1.M` is set.
All are EL1-scoped, meaning they control the EL1 (kernel) translation regime.

```
Registers to configure:
  ┌──────────────┐   ┌───────────────┐   ┌──────────────┐
  │  TTBR0_EL1   │   │  TTBR1_EL1   │   │   TCR_EL1    │
  │ (user/idmap) │   │  (kernel)    │   │  (VA config) │
  └──────────────┘   └───────────────┘   └──────────────┘
           ┌───────────────────────┐
           │       MAIR_EL1        │
           │  (memory attributes)  │
           └───────────────────────┘
           ┌───────────────────────┐
           │       SCTLR_EL1       │
           │   (enable MMU here)   │
           └───────────────────────┘
```

---

## 2. `TTBR0_EL1` — Translation Table Base Register 0

### Purpose
Holds the **physical base address** of the page table for the **lower VA range**
(addresses where bits [63:48] == `0x0000`).

At boot time, `TTBR0_EL1` is loaded with `idmap_pg_dir` so the
identity map is active when the MMU turns on.

### Register Format

```
TTBR0_EL1 [63:0]:
  Bits [63:48] — ASID (Address Space Identifier, if TTBR0 carries it)
  Bits [47:x]  — Physical Base Address of PGD (aligned to table size)
  Bits [x-1:0] — Reserved / zero
```

### Assembly Write

```asm
adrp    x0, idmap_pg_dir    // load physical address of PGD
msr     TTBR0_EL1, x0
isb
```

### After Boot
Once the kernel is running, `TTBR0_EL1` is switched per-process to point
to each process's own page table (user space). This happens in `context_switch()`.

---

## 3. `TTBR1_EL1` — Translation Table Base Register 1

### Purpose
Holds the **physical base address** of the page table for the **upper VA range**
(addresses where bits [63:48] == `0xFFFF`).

This is the **kernel's page table**. It never changes after boot
(always points to `swapper_pg_dir`).

### Register Format

```
TTBR1_EL1 [63:0]:
  Bits [63:48] — ASID (not typically used for kernel)
  Bits [47:x]  — Physical Base Address of kernel PGD
  Bits [x-1:0] — Reserved / zero
```

### Assembly Write (boot)

```asm
adrp    x1, init_pg_dir     // load physical address of kernel PGD
msr     TTBR1_EL1, x1
isb
```

### VA Routing Rule (set by TCR_EL1)

```
VA[63] == 0   →  use TTBR0_EL1  (user space: 0x0000_0000_0000_0000)
VA[63] == 1   →  use TTBR1_EL1  (kernel: 0xFFFF_0000_0000_0000 and above)
```

---

## 4. `TCR_EL1` — Translation Control Register

### Purpose
Controls the **shape** of the translation regime: VA size, page granule,
shareability, cache attributes for the page table walks.

### Key Bit Fields

```
TCR_EL1 bit layout (64-bit register):

 [63:60]  Reserved
 [59]     DS   — 52-bit PA support (ARMv8.2-LPA)
 [38:37]  TBI1 — Top Byte Ignore for TTBR1 range
 [36:35]  TBI0 — Top Byte Ignore for TTBR0 range
 [34:32]  IPS  — Intermediate Physical Address Size
                  000 = 32-bit (4GB)
                  001 = 36-bit
                  010 = 40-bit
                  011 = 42-bit
                  100 = 44-bit
                  101 = 48-bit   ← typical
                  110 = 52-bit (LPA)
 [31:30]  TG1  — Granule size for TTBR1
                  01 = 16KB
                  10 = 4KB       ← typical
                  11 = 64KB
 [29:28]  SH1  — Shareability for TTBR1 walks
                  11 = Inner Shareable ← typical (SMP)
 [27:26]  ORGN1 — Outer cache for TTBR1 walks
                  01 = Write-Back, Read-Allocate, Write-Allocate
 [25:24]  IRGN1 — Inner cache for TTBR1 walks
                  01 = Write-Back, Read-Allocate, Write-Allocate
 [23]     EPD1 — Disable TTBR1 walks (0 = enabled)
 [22]     A1   — ASID select (0 = TTBR0, 1 = TTBR1)
 [21:16]  T1SZ — Size offset of TTBR1 region
                  16 → 48-bit VA for kernel  ← typical
 [15:14]  TG0  — Granule size for TTBR0
                  00 = 4KB  ← typical
 [13:12]  SH0  — Shareability for TTBR0 walks (11 = Inner Shareable)
 [11:10]  ORGN0 — Outer cache for TTBR0 walks
 [9:8]    IRGN0 — Inner cache for TTBR0 walks
 [7]      EPD0 — Disable TTBR0 walks (0 = enabled)
 [5:0]    T0SZ — Size offset of TTBR0 region
                  16 → 48-bit VA for user  ← typical
```

### Typical Boot Value (48-bit VA, 4KB pages)

```c
// arch/arm64/include/asm/pgtable-hwdef.h
#define TCR_T0SZ(x)     ((UL(64) - (x)) << TCR_T0SZ_OFFSET)
#define TCR_T1SZ(x)     ((UL(64) - (x)) << TCR_T1SZ_OFFSET)

// In proc.S / __cpu_setup:
tcr = TCR_T0SZ(VA_BITS)  |    // T0SZ = 16 for 48-bit
      TCR_T1SZ(VA_BITS)  |    // T1SZ = 16 for 48-bit
      TCR_TG0_4K         |    // 4KB pages for user
      TCR_TG1_4K         |    // 4KB pages for kernel
      TCR_IRGN0_WBWA     |    // inner WB-WA for TTBR0 walks
      TCR_IRGN1_WBWA     |    // inner WB-WA for TTBR1 walks
      TCR_ORGN0_WBWA     |    // outer WB-WA for TTBR0 walks
      TCR_ORGN1_WBWA     |    // outer WB-WA for TTBR1 walks
      TCR_SH0_INNER      |    // inner shareable for TTBR0
      TCR_SH1_INNER      |    // inner shareable for TTBR1
      TCR_IPS_48BIT;          // 48-bit physical address
```

### VA Ranges Resulting from T0SZ/T1SZ = 16

```
TTBR0 range:  0x0000_0000_0000_0000  to  0x0000_FFFF_FFFF_FFFF  (256TB)
TTBR1 range:  0xFFFF_0000_0000_0000  to  0xFFFF_FFFF_FFFF_FFFF  (256TB)
```

---

## 5. `MAIR_EL1` — Memory Attribute Indirection Register

### Purpose
Defines up to **8 memory attribute types** (indices 0–7).
Page table entries reference these by a 3-bit index (`AttrIndx[2:0]`).

### Why Needed Before MMU On
When the MMU is on, every memory access checks the page table descriptor's
`AttrIndx` field and uses `MAIR_EL1` to determine the actual memory type.
If `MAIR_EL1` is not set up, the CPU uses undefined/garbage attributes.

### Register Format

```
MAIR_EL1 [63:0]:
  Bits [7:0]   → Attr0
  Bits [15:8]  → Attr1
  Bits [23:16] → Attr2
  Bits [31:24] → Attr3
  Bits [39:32] → Attr4
  Bits [47:40] → Attr5
  Bits [55:48] → Attr6
  Bits [63:56] → Attr7
```

### Linux ARM64 Attribute Table

```c
// arch/arm64/include/asm/memory.h (MT_ = Memory Type)
#define MT_DEVICE_nGnRnE   0    // Device, non-Gathering, non-Reordering, no Early Write Ack
#define MT_DEVICE_nGnRE    1    // Device, non-Gathering, non-Reordering
#define MT_DEVICE_GRE      2    // Device, Gathering, Reordering
#define MT_NORMAL_NC       3    // Normal, Non-Cacheable
#define MT_NORMAL          4    // Normal, Write-Back, Write-Allocate (typical RAM)
#define MT_NORMAL_WT       5    // Normal, Write-Through
#define MT_NORMAL_TAGGED   6    // Normal, MTE-tagged (ARMv8.5-MTE)
```

### Assembly Write (in `__cpu_setup`)

```asm
// Build MAIR value:
// Attr0 = 0x00 (Device nGnRnE)
// Attr1 = 0x04 (Device nGnRE)
// Attr2 = 0x0C (Device GRE)
// Attr3 = 0x44 (Normal NC)
// Attr4 = 0xFF (Normal WB WA)
// Attr5 = 0xBB (Normal WT)
mov_q   x5, MAIR_EL1_SET       // precomputed constant
msr     MAIR_EL1, x5
isb
```

---

## 6. `SCTLR_EL1` — System Control Register

### Purpose
The master control register for EL1 operation.
**Bit 0 (M) is what actually enables the MMU.**

### Key Bits for MMU Enable

```
SCTLR_EL1 bit layout (relevant bits):

  Bit 0   M    — MMU enable (0 = off, 1 = on)  ← THE key bit
  Bit 1   A    — Alignment fault enable
  Bit 2   C    — Data cache enable
  Bit 3   SA   — Stack alignment check (EL1)
  Bit 4   SA0  — Stack alignment check (EL0)
  Bit 12  I    — Instruction cache enable
  Bit 19  WXN  — Write implies Execute-Never
  Bit 23  SPAN — Set PAN (Privileged Access Never) on exception
  Bit 25  EE   — Exception Endianness (0 = little-endian)
  Bit 26  UCI  — User Cache Instructions enable
```

### Assembly Pattern at Boot

```asm
// arch/arm64/kernel/head.S — __enable_mmu
mrs     x0, SCTLR_EL1

// Clear bits that should be 0 (from ARM spec RES0 and desired state)
mov_q   x1, SCTLR_EL1_SET_CLEAR_MASK
bic     x0, x0, x1

// Set bits: M (MMU), C (D-cache), I (I-cache)
mov_q   x1, (SCTLR_ELx_M  | SCTLR_ELx_C | SCTLR_ELx_I)
orr     x0, x0, x1

msr     SCTLR_EL1, x0
isb                         // ← MANDATORY instruction synchronization barrier
```

### Why `isb` is Mandatory After Writing `SCTLR_EL1`

The ARM Architecture Reference Manual requires an **ISB** after writing `SCTLR_EL1`
to ensure the change is visible to subsequent instructions.

Without `isb`:
- The CPU may speculatively fetch instructions with the old translation state
- Instruction cache may contain stale lines from before MMU was on
- Results are **UNPREDICTABLE** (hardware-specific but often catastrophic)

---

## 7. Register Configuration Sequence

The **order matters**. ARM strongly recommends:

```
1. Configure MAIR_EL1   (memory attributes)
2. Configure TCR_EL1    (translation control)
3. isb                  (ensure MAIR and TCR are visible)
4. Load TTBR0_EL1       (user/idmap page table)
5. Load TTBR1_EL1       (kernel page table)
6. isb                  (ensure TTBRs are visible)
7. Write SCTLR_EL1.M=1  (enable MMU)
8. isb                  (ensure MMU is active before next instruction)
```

In `head.S`, this is split between `__cpu_setup` (steps 1–3) and
`__enable_mmu` (steps 4–8).

---

## 8. Reading Current Register Values (Debugging)

To inspect these registers from a running kernel (requires EL1 access):

```c
// From kernel code:
u64 ttbr1, tcr, mair, sctlr;
asm("mrs %0, ttbr1_el1" : "=r" (ttbr1));
asm("mrs %0, tcr_el1"   : "=r" (tcr));
asm("mrs %0, mair_el1"  : "=r" (mair));
asm("mrs %0, sctlr_el1" : "=r" (sctlr));
```

Or via the kernel's `sysreg_show()` debug paths and crash dump tools.
