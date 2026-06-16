# TTBR Physical Address Format and `phys_to_ttbr` Macro

**Source:** `arch/arm64/include/asm/assembler.h`  
**Used in:** `__enable_mmu` — `phys_to_ttbr x2, x2` before `msr ttbr0_el1, x2`  
**Prerequisite:** Understanding that TTBR registers do NOT simply take a raw physical address

---

## 0. The Problem: PA Doesn't Fit Directly in TTBR

On a 64-bit architecture supporting up to 52-bit physical addresses (FEAT_LPA2),
the TTBR registers have a specific encoding. You cannot simply write a physical
address directly — the bits must be placed in the correct positions.

The `phys_to_ttbr` macro handles this translation from raw PA to the correctly
encoded TTBR value.

---

## 1. TTBR0_EL1 / TTBR1_EL1 Register Format (Standard 48-bit PA)

For systems without `CONFIG_ARM64_PA_BITS_52` (standard 48-bit PA):

```
TTBR0_EL1 / TTBR1_EL1 [63:0]:

Bits [63:48] = ASID[15:0]    ← Address Space Identifier (16-bit if TCR_EL1.AS=1)
Bits [47:1]  = BADDR[47:1]   ← Table base physical address, bits [47:1]
Bit  [0]     = CnP           ← Common not Private
```

**For standard 48-bit PA:**

```
phys_to_ttbr macro (CONFIG_ARM64_PA_BITS_52 not set):
    // No encoding needed — PA bits [47:1] fit directly in TTBR[47:1]
    // The macro is a no-op
    .macro phys_to_ttbr, ttbr, phys
    // nop — identity transformation
    .endm
```

So for the normal case, `x2 = physical address of __pi_init_idmap_pg_dir`
and writing `x2` directly to `TTBR0_EL1` works because:
- PA bits [47:12] fit in TTBR bits [47:12]
- PA bits [11:1] are always 0 (page-aligned address)
- PA bit [0] = 0 (CnP = 0 at boot)
- TTBR bits [63:48] (ASID) = 0 by default

---

## 2. TTBR Format for 52-bit PA (FEAT_LPA2, `CONFIG_ARM64_PA_BITS_52`)

### 2.1 The Problem with 52-bit PA

TTBR registers only have 48 bits allocated for the physical address
(bits[47:1]). A 52-bit PA has 4 extra bits (bits[51:48]) that don't fit.

The ARM architecture's solution for LPA2 (`FEAT_LPA2`) places the extra PA
bits [51:48] in **TTBR bits[5:2]**:

```
TTBR0_EL1 [63:0] with FEAT_LPA2:

Bits [63:48] = ASID[15:0]
Bits [47:6]  = PA[47:6]    ← main PA bits
Bits [5:2]   = PA[51:48]   ← upper PA bits in non-obvious location!
Bit  [1]     = 0 (RES0)
Bit  [0]     = CnP
```

### 2.2 The `phys_to_ttbr` Macro for 52-bit PA

```asm
// arch/arm64/include/asm/assembler.h
.macro phys_to_ttbr, ttbr, phys
#ifdef CONFIG_ARM64_PA_BITS_52
    orr  \ttbr, \phys, \phys, lsr #46    // Copy bits [51:48] of PA into bits [5:2] of TTBR
    and  \ttbr, \ttbr, #TTBR_BADDR_MASK_52
    // TTBR_BADDR_MASK_52 = GENMASK_ULL(47, 2) = 0x0000_FFFF_FFFF_FFFC
#endif
.endm
```

**Step-by-step for PA = `0xF_0000_8000_0000`** (52-bit example):

```
phys = 0x000F_0000_8000_0000   (PA bits[51:48] = 0xF, bits[47:0] = 0x0000_8000_0000)

Step 1: phys >> 46
    0x000F_0000_8000_0000 >> 46
  = 0x0000_0000_0000_003C   (bits [51:48] shifted to positions [5:2])

Step 2: OR with original phys
    0x000F_0000_8000_0000
  | 0x0000_0000_0000_003C
  = 0x000F_0000_8000_003C

Step 3: AND with TTBR_BADDR_MASK_52 (0x0000_FFFF_FFFF_FFFC)
    0x000F_0000_8000_003C
  & 0x0000_FFFF_FFFF_FFFC
  = 0x0000_0000_8000_003C

Result in TTBR:
  Bits [47:6]  = 0x0000_8000_0000 >> 6 = PA[47:6]
  Bits [5:2]   = 0xF (PA[51:48])
  Bits [1:0]   = 0x0 (CnP=0, bit1=0)
```

When the hardware reads TTBR, it reconstructs the 52-bit PA by reversing this
encoding:

```
PA[47:6]  = TTBR[47:6]
PA[51:48] = TTBR[5:2]
PA[5:0]   = 0 (page-aligned, always)
```

---

## 3. ASID Field in TTBR

### 3.1 Placement

```
TTBR0_EL1[63:48] = ASID

(8-bit ASID if TCR_EL1.AS=0: only bits[55:48] are used, [63:56] ignored)
(16-bit ASID if TCR_EL1.AS=1: bits[63:48] all used)
```

Linux uses 16-bit ASIDs when the CPU supports them
(`ID_AA64MMFR0_EL1.ASIDBits != 0`).

### 3.2 At Boot

At boot, `TTBR0_EL1` and `TTBR1_EL1` are written with ASID = 0:

```asm
// __enable_mmu:
msr     ttbr0_el1, x2    // x2 = phys_to_ttbr(PA) — ASID bits[63:48] = 0
load_ttbr1 x1, x1, x3   // ASID = 0 in TTBR1 as well
```

All TLB entries installed during boot have ASID = 0. The first user process
(PID 1, `init`) will use ASID 1.

### 3.3 ASID 0 is Special

ASID 0 in TTBR0 means: "The entries using TTBR0 are associated with ASID 0."
However, kernel entries (via TTBR1) also use ASID 0 at boot. Since kernel
entries are **global** (`nG=0` in PTE), they are not tagged with any specific
ASID and match regardless of the current TTBR ASID.

---

## 4. CnP — Common not Private (bit 0)

### 4.1 What CnP Does

`CnP` (bit 0 of TTBR) was introduced in ARMv8.2 (`FEAT_TTCNP`).

```
CnP = 0  → Private: Each CPU may have different TLB entries for this regime
CnP = 1  → Common: All CPUs in the same Inner Shareable domain are guaranteed
            to use the same page table for this TTBR; TLB is coherent
```

### 4.2 Performance Implication

When `CnP = 1`:
- The hardware can share TLB entries across CPUs without broadcasting invalidations
- Context switch TLB flushes need not broadcast to all CPUs — the hardware
  guarantees consistency through `CnP`
- This reduces `TLBI IS` (Inner Shareable broadcast) overhead in SMP systems

### 4.3 At Boot

```
CnP = 0 at boot
```

The kernel sets CnP=1 after boot in `secondary_start_kernel` for secondary
CPUs that share the kernel page tables (`swapper_pg_dir`). Before boot
completes, CnP=0 is the safe choice since page tables are still being modified.

---

## 5. `load_ttbr1` Macro — Writing TTBR1 Safely

`TTBR1_EL1` is written via the `load_ttbr1` macro, not a simple `msr`:

```asm
// arch/arm64/include/asm/assembler.h
.macro load_ttbr1, ttbr1, tmp1, tmp2
#ifdef CONFIG_ARM64_PA_BITS_52
    phys_to_ttbr \ttbr1, \ttbr1
#endif
    adrp    \tmp1, empty_zero_page
    phys_to_ttbr \tmp1, \tmp1
    msr     ttbr1_el1, \tmp1         // Write a safe empty page table first
    isb                               // ISB to make it take effect
    msr     ttbr1_el1, \ttbr1        // Now write the real value
    isb
.endm
```

Wait — **why write twice?** This is an erratum workaround for some early
Cortex-A55/A76 parts where writing TTBR1_EL1 directly while the MMU is off
(or being enabled) could cause TLB coherency issues. Writing an empty page
table first, then the real one, ensures the hardware state machine transitions
correctly.

---

## 6. Physical Address to TTBR Encoding — Complete Decision Tree

```
Is CONFIG_ARM64_PA_BITS_52 set?
    │
    ├── NO (48-bit PA or less)
    │       TTBR value = PA (identity)
    │       PA[47:12] maps directly to TTBR[47:12]
    │       PA[11:0] must be 0 (page-aligned — guaranteed by linker)
    │       TTBR[63:48] = ASID (0 at boot)
    │       TTBR[0] = CnP (0 at boot)
    │
    └── YES (52-bit PA, FEAT_LPA2)
            TTBR value = phys_to_ttbr(PA)
            PA[47:6]  → TTBR[47:6]
            PA[51:48] → TTBR[5:2]
            TTBR[63:48] = ASID (0 at boot)
            TTBR[1] = 0 (RES0)
            TTBR[0] = CnP (0 at boot)
```

---

## 7. `__enable_mmu` Line-by-Line Revisited With This Knowledge

```asm
phys_to_ttbr x2, x2          // Convert __pi_init_idmap_pg_dir PA to TTBR encoding
msr     ttbr0_el1, x2        // TTBR0 = identity map root (no ISB needed here —
                              // MMU is off, write will take effect before MMU on)
load_ttbr1 x1, x1, x3        // TTBR1 = reserved_pg_dir PA encoded appropriately
                              // (includes workaround double-write + ISB)
set_sctlr_el1 x0             // MMU ON — at this point TTBR0 and TTBR1 are valid
ret                           // fetch uses VA via TTBR0 identity map → same PA
```

The ordering guarantee: `msr ttbr0_el1, x2` followed by `set_sctlr_el1`
(which includes an `ISB`) ensures the TTBR write is architecturally complete
before the MMU enable takes effect.

---

## ARMv8 CPU / Kernel / Memory Context

### CPU Perspective (ARMv8-A)
ARMv8-A uses a multi-level page-table walk starting at the physical address stored in TTBR0_EL1 (low VA, user) or TTBR1_EL1 (high VA, kernel). With 4 KB granule and 48-bit VA (T0SZ/T1SZ=16): the walk has 4 levels: L0 (PGD), L1 (PUD), L2 (PMD), L3 (PTE). Each table entry is 8 bytes. A leaf entry (block or page descriptor) contains the PA, memory attributes (AttrIdx into MAIR_EL1), access permissions (AP bits), shareability, and XN/PXN bits. The hardware page-table walker is fully autonomous: on a TLB miss, it reads TTBR, walks the tables in memory, and populates the TLB entry without software intervention.

### Kernel Perspective (Linux ARM64)
Linux organizes the early boot page tables into:
- __idmap_pg_dir: identity map (VA==PA) covering the .idmap.text section, used during MMU enable.
- init_pg_dir / swapper_pg_dir: kernel page table covering the TTBR1_EL1 region.
__pi_early_map_kernel (arch/arm64/mm/mmu.c) allocates and populates these tables using fixmap and the early pgtable allocator before __enable_mmu is called. After start_kernel, the definitive kernel page tables are built by paging_init().

### Memory Perspective (ARMv8 Memory Model)
Each page-table entry encodes both the physical address and the memory type for that region. The ARMv8 memory model treats adjacent pages independently: two adjacent 4 KB pages can have different memory attributes (e.g., one Normal cacheable, one Device). Block entries (2 MB at L2, 1 GB at L1) allow the walker to terminate early, reducing TLB fill latency and the number of memory accesses per walk. The hardware guarantees that table walks are coherent with the data cache if the inner-shareable domain includes the page-table memory (which is true for all Linux ARM64 configurations).