# `TCR_EL1` — Translation Control Register Deep Dive

**Written by:** `__cpu_setup` in `arch/arm64/mm/proc.S`  
**Purpose:** Controls the virtual address space split, granule sizes, cacheability, and shareability of both TTBR0 and TTBR1 translation regimes  
**When it matters:** TCR_EL1 is the configuration register that makes the MMU know HOW to walk the page tables that `reserved_pg_dir` and `__pi_init_idmap_pg_dir` define

---

## 0. The 64-Bit Address Space Split — The Core Concept

AArch64 provides a 64-bit virtual address space but divides it into two
independent halves using the **top bits** of the VA:

```
Virtual Address bit 63 / bit 55 / top T*SZ bits
──────────────────────────────────────────────────────────
0xFFFF_FFFF_FFFF_FFFF  ─┐
0xFFFF_0000_0000_0000  ─┤  Kernel VA space  → TTBR1_EL1
            ┊           │  (bits all 1s)
0xFFFF_0000_0000_0000  ─┘
     [VA HOLE / unmapped]
0x0000_FFFF_FFFF_FFFF  ─┐
0x0000_0000_0000_0000  ─┤  User VA space    → TTBR0_EL1
            ┊           │  (bits all 0s)
0x0000_0000_0000_0000  ─┘
──────────────────────────────────────────────────────────
```

The **split point** is determined by `TCR_EL1.T0SZ` and `TCR_EL1.T1SZ`:

- `T0SZ` = `64 - <number of bits used for TTBR0 VA>`
- `T1SZ` = `64 - <number of bits used for TTBR1 VA>`

For a 48-bit VA kernel (`CONFIG_ARM64_VA_BITS_48`):
- `T0SZ = 16` → TTBR0 covers `0x0000_0000_0000_0000` to `0x0000_FFFF_FFFF_FFFF`
- `T1SZ = 16` → TTBR1 covers `0xFFFF_0000_0000_0000` to `0xFFFF_FFFF_FFFF_FFFF`

---

## 1. TCR_EL1 Register Layout

```
Bits [63:60]  [59]  [58:56]  [55:52]  [51:48]  [47:46]  [45:44]  [43:42]
     RES0    TBI1  RES0     RES0     RES0     RES0    ORGN1    IRGN1

Bits [41:40]  [39:38]  [37:36]  [35:32]  [31]  [30:29]  [28:27]  [26]
     SH1      TG1      RES0     T1SZ     A     RES0     TBI0     AS

Bits [25:24]  [23:22]  [21:20]  [19:18]  [17:16]  [15:14]  [13:12]  [11:10]
     RES0     ORGN0    IRGN0    SH0      TG0      RES0     EPD1     EPD0

Bits [9:6]   [5:0]
     RES0    T0SZ
```

---

## 2. Field-by-Field Analysis

### 2.1 `T0SZ` — Bits [5:0] — TTBR0 VA Size Offset

```
T0SZ = 64 - VA_BITS
```

For `VA_BITS = 48`:
```
T0SZ = 64 - 48 = 16
```

This tells the hardware: "For TTBR0 translations, the virtual address uses bits
`[47:0]` (48 bits). Any VA with bits `[63:48]` non-zero in TTBR0 range is
invalid."

The hardware detects which TTBR to use by checking the **top bits** of the VA:
- VA bits `[63:64-T0SZ]` all zero → use TTBR0
- VA bits `[63:64-T1SZ]` all one  → use TTBR1
- Neither → fault (VA is in the "hole")

**VA Hole Example (48-bit VA):**

```
0xFFFF_0000_0000_0000  and above → TTBR1 (kernel)
0x0001_0000_0000_0000  to
0xFFFE_FFFF_FFFF_FFFF             → VA HOLE: Translation fault (no TTBR)
0x0000_0000_0000_0000  to
0x0000_FFFF_FFFF_FFFF             → TTBR0 (user)
```

Any code that generates a VA in the hole (e.g., corrupt pointer) immediately
faults, providing memory safety.

---

### 2.2 `T1SZ` — Bits [21:16] — TTBR1 VA Size Offset

Symmetric with T0SZ but for the kernel half. For 48-bit VA:
```
T1SZ = 16
```

This means: "For TTBR1 translations, the VA uses bits `[47:0]` counted from
the top of the address space."

After `__enable_mmu` and `__pi_early_map_kernel`, the kernel runs in the VA
range defined by TTBR1. `PAGE_OFFSET` is `0xFFFF_8000_0000_0000` for 48-bit VA:

```c
// arch/arm64/include/asm/memory.h
#define _PAGE_OFFSET(va)    (-(UL(1) << (va)))
#define PAGE_OFFSET         (_PAGE_OFFSET(VA_BITS))
// For VA_BITS=48: -(2^48) = 0xFFFF_0000_0000_0000
```

---

### 2.3 `EPD0` — Bit 7 — TTBR0 Walk Disabled

```
EPD0 = 0  → TTBR0 page table walk ENABLED
EPD0 = 1  → Any TTBR0 VA access causes Translation Fault
```

At boot time, `EPD0 = 0` because the identity map must remain accessible via
TTBR0. Later in the boot sequence (in `cpu_replace_ttbr1`), the kernel sets
`EPD1=1` temporarily during TTBR1 switching, then restores it.

**Important:** `EPD1=1` is used in `__cpu_switch_mm` and `cpu_replace_ttbr1`
as a trick to atomically disable TTBR1 access while updating it, preventing
speculation through stale translations.

---

### 2.4 `EPD1` — Bit 23 — TTBR1 Walk Disabled

Same as EPD0 but for TTBR1. `EPD1 = 0` at boot (TTBR1 walk enabled).

---

### 2.5 `TG0` — Bits [15:14] — TTBR0 Granule Size

```
TG0 = 0b00  → 4KB granule (CONFIG_ARM64_4K_PAGES)
TG0 = 0b01  → 64KB granule (CONFIG_ARM64_64K_PAGES)
TG0 = 0b10  → 16KB granule (CONFIG_ARM64_16K_PAGES)
```

**Granule = page size** used for the leaf-level page table entries (PTEs).
This defines the minimum allocation unit for virtual address to physical address
mapping and the size of physical pages in the allocator.

The value is selected at kernel compile time and encoded into `TCR_EL1.TG0`
by `__cpu_setup`.

---

### 2.6 `TG1` — Bits [31:30] — TTBR1 Granule Size

```
TG1 = 0b10  → 4KB granule (CONFIG_ARM64_4K_PAGES)   ← NOTE: encoding differs from TG0!
TG1 = 0b11  → 64KB granule (CONFIG_ARM64_64K_PAGES)
TG1 = 0b01  → 16KB granule (CONFIG_ARM64_16K_PAGES)
```

**Critical difference from TG0:** The encoding for TG1 uses different values.
For 4K pages, `TG0=0b00` but `TG1=0b10`. The ARM ARM specifies different
encodings for TG0 and TG1 as a historical artifact.

---

### 2.7 `IRGN0`, `ORGN0` — Bits [11:10], [9:8] — TTBR0 Cacheability

```
IRGN0 = 0b01  → Inner Write-Back, Read-Allocate, Write-Allocate Cacheable
ORGN0 = 0b01  → Outer Write-Back, Read-Allocate, Write-Allocate Cacheable
```

These control how the **hardware page table walker** treats the page table
memory pointed to by TTBR0. "Inner" = within the inner shareability domain
(typically all cores on the SoC). "Outer" = outer shareability domain (includes
I/O masters in some platforms).

Setting both to write-back cacheable means:
1. Page table entries are cached in L1/L2 after first walk.
2. Subsequent TLB misses that require page table walks hit the cache, not DRAM.
3. This dramatically speeds up kernel execution after the first mapping is walked.

---

### 2.8 `IRGN1`, `ORGN1` — Bits [43:42], [41:40] — TTBR1 Cacheability

Same as IRGN0/ORGN0 but for TTBR1. Both set to `0b01` (write-back cacheable).

---

### 2.9 `SH0` — Bits [13:12] — TTBR0 Shareability

```
SH0 = 0b11  → Inner Shareable (shared across all CPUs in Inner Shareability domain)
SH0 = 0b10  → Outer Shareable
SH0 = 0b00  → Non-Shareable
```

Setting `SH0 = 0b11` (Inner Shareable) means the page table walker on all
CPUs sees coherent page table data. If CPU0 updates a PTE, CPU1's page table
walker (on a TLB miss) sees the updated PTE without explicit cache maintenance.

This is essential for SMP correctness: `__pi_early_map_kernel` runs only on
CPU0, but secondary CPUs that come online later must see the same page tables.

---

### 2.10 `SH1` — Bits [29:28] — TTBR1 Shareability

Same as SH0 but for TTBR1. Set to `0b11` (Inner Shareable).

---

### 2.11 `IPS` — Bits [34:32] — Intermediate Physical Address Size

```
IPS = 0b000  → 32-bit PA (4 GB)
IPS = 0b001  → 36-bit PA (64 GB)
IPS = 0b010  → 40-bit PA (1 TB)
IPS = 0b011  → 42-bit PA (4 TB)
IPS = 0b100  → 44-bit PA (16 TB)
IPS = 0b101  → 48-bit PA (256 TB)  ← typical for server SoCs
IPS = 0b110  → 52-bit PA (4 PB)   ← requires FEAT_LPA
```

`__cpu_setup` reads `ID_AA64MMFR0_EL1.PARange` to discover the maximum PA
size this CPU supports, then encodes it into `TCR_EL1.IPS`:

```asm
// proc.S:
mrs     x10, ID_AA64MMFR0_EL1
ubfx    x10, x10, #ID_AA64MMFR0_EL1_PARANGE_SHIFT, 4
mov_q   x9, TCR_T0SZ(VA_BITS) | TCR_T1SZ(VA_BITS) | TCR_CACHE_FLAGS | \
            TCR_SMP_FLAGS | TCR_TG_FLAGS | TCR_KASLR_FLAGS | TCR_ASID16 | \
            TCR_TBI0 | TCR_A1 | TCR_KASAN_FLAGS
bfi     x9, x10, #TCR_IPS_SHIFT, #3     // embed PARange into IPS field
msr     tcr_el1, x9
```

**Why IPS matters:** If `IPS` claims a larger PA than the CPU supports, the
hardware behaviour is UNPREDICTABLE for PAs beyond the supported range. Setting
IPS to exactly the supported maximum is mandatory.

---

### 2.12 `AS` — Bit 36 — ASID Size

```
AS = 0  → 8-bit ASIDs (256 unique)
AS = 1  → 16-bit ASIDs (65536 unique)
```

Linux sets `AS = 1` when `ID_AA64MMFR0_EL1.ASIDBits != 0` (i.e., the CPU
supports 16-bit ASIDs). With 16-bit ASIDs, `TTBR0_EL1[63:48]` and
`TTBR1_EL1[63:48]` carry the ASID, enabling the TLB to hold entries from
multiple processes simultaneously without full TLB flushes on context switch.

---

### 2.13 `TBI0` / `TBI1` — Top Byte Ignore

```
TBI0 = 1  → TTBR0 VAs: top byte (bits [63:56]) ignored during address translation
TBI1 = 0  → TTBR1 VAs: all 64 bits participate in translation
```

`TBI0 = 1` enables **Tagged Pointers** (user space): Android and MTE use the
top byte to encode metadata while the pointer still translates correctly.

`TBI1 = 0` for kernel VA — the full VA must be used precisely. Kernel pointers
are not tagged.

---

## 3. TCR_EL1 and the 4-Level Page Table Walk (4KB granule, 48-bit VA)

With the above settings, the hardware PTW for a 48-bit VA uses exactly 4 levels:

```
VA[47:39]  = L0 index (PGD) — 9 bits → 512 entries × 8 bytes = 4KB per table
VA[38:30]  = L1 index (PUD) — 9 bits → 512 entries × 8 bytes = 4KB per table
VA[29:21]  = L2 index (PMD) — 9 bits → 512 entries × 8 bytes = 4KB per table
VA[20:12]  = L3 index (PTE) — 9 bits → 512 entries × 8 bytes = 4KB per table
VA[11:0]   = Page offset    — 12 bits → 4KB page
```

The PTW starts from the physical address in TTBR1_EL1 (bits[47:1] × 2, or 48
bits for 52-bit PA), adds `VA[47:39] × 8` to index into the PGD, fetches the
table descriptor, continues to PUD, PMD, and PTE.

---

## 4. What Happens If TCR_EL1 Is Configured Wrong

| Wrong setting | Consequence |
|---------------|-------------|
| `T1SZ` too large | Kernel VA space shrunk — `PAGE_OFFSET` shifts up, vmalloc shrinks, crash likely |
| `T1SZ` too small | Top bits of VA not checked properly — user VA might alias kernel VA — security disaster |
| `TG1` wrong | Page table walk uses wrong granule — every translation produces garbage PA |
| `IPS` too large | PA bits beyond hardware capability go through MMU — UNPREDICTABLE |
| `SH1 = non-shareable` | On SMP: secondary CPU PTWs are not cache-coherent — stale table entries on secondary CPUs |
| `IRGN1 = non-cacheable` | Every TLB miss requires DRAM read for page table — 100× slower kernel |
| `EPD1 = 1` | Kernel VA access immediately faults — instant crash |

---

## ARMv8 CPU / Kernel / Memory Context

### CPU Perspective (ARMv8-A)
TCR_EL1 (Translation Control Register, EL1) is a 64-bit system register that controls how the hardware page-table walker interprets addresses. Key fields:
- T0SZ (bits 5:0): number of bits subtracted from 64 to get the size of the TTBR0_EL1 VA region. Linux uses T0SZ=48 for 48-bit user VA (256 TB).
- T1SZ (bits 21:16): same for TTBR1_EL1 (kernel VA). Linux uses T1SZ=48.
- TG0/TG1 (bits 15:14, 31:30): translation granule (4 KB, 16 KB, 64 KB).
- IPS (bits 34:32): intermediate physical address size (physical address bits supported).
- EPD0/EPD1: disable TTBR0/TTBR1 walks (set EPD1=0 and EPD0=0 to enable both).
The hardware reads TCR_EL1 at every TLB miss to know how to parse the VA and how many levels of page tables to walk.

### Kernel Perspective (Linux ARM64)
Linux sets TCR_EL1 in __cpu_setup. The value is built from Kconfig options (VA_BITS, PAGE_SIZE) and CPU feature bits. The TCR value written at boot determines the kernel and user address space split for the entire lifecycle of the system. KASLR does not change TCR; it only changes TTBR1_EL1 to point to a randomly positioned kernel image.

### Memory Perspective (ARMv8 Memory Model)
TCR_EL1 establishes the VA split: addresses with the top T1SZ bits all-ones go through TTBR1_EL1 (kernel), addresses with the top T0SZ bits all-zeros go through TTBR0_EL1 (user). Addresses in between are unmapped (translation fault). This split is the hardware enforcement of the user/kernel address space boundary in the ARMv8 memory model. The TG0/TG1 fields also determine the granularity of permission boundaries (page size).