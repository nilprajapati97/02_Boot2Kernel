# ARM32 vs ARM64 MMU Enable — Side-by-Side Comparison

## 1. Overview

Both ARM32 and ARM64 enable the MMU in assembly early during boot, but the
mechanisms differ significantly due to architectural changes in AArch64.

---

## 2. Quick Comparison Table

| Aspect | ARM32 | ARM64 |
|---|---|---|
| **Control register** | `SCTLR` (CP15 c1) | `SCTLR_EL1` (system register) |
| **MMU enable bit** | `SCTLR.M` (bit 0) | `SCTLR_EL1.M` (bit 0) |
| **Page table base** | `TTBR0` / `TTBR1` (CP15) | `TTBR0_EL1` / `TTBR1_EL1` |
| **Translation control** | `TTBCR` (CP15) | `TCR_EL1` |
| **Memory attributes** | `MAIR0` / `MAIR1` (CP15) | `MAIR_EL1` |
| **Register access** | `mcr/mrc p15,...` | `msr/mrs` |
| **Page table levels** | 2 (no LPAE) / 3 (LPAE) | 3 or 4 |
| **VA split** | `TTBCR.N` field | `TCR_EL1.T0SZ`/`T1SZ` |
| **Max PA** | 32-bit (4GB) / 40-bit (LPAE) | 48-bit / 52-bit (LPA) |
| **Max VA** | 32-bit | 48-bit / 52-bit |
| **Identity map** | Yes (`idmap_pg_dir`) | Yes (`idmap_pg_dir`) |
| **Kernel page table** | `swapper_pg_dir` | `swapper_pg_dir` |
| **Temp boot table** | None (direct to swapper) | `init_pg_dir` |
| **Huge pages** | 1MB sections (ARM32) | 2MB (PMD), 1GB (PUD) |
| **ISB required after SCTLR write** | Yes (CP15 ISB) | Yes |
| **KASLR** | No | Yes (`CONFIG_RANDOMIZE_BASE`) |
| **W^X enforcement** | Partial | Full (WXN bit) |

---

## 3. Register Access Syntax

### ARM32 — Coprocessor 15 Access

```asm
// ARM32: MCR/MRC instruction with CP15
// Enable MMU: set bit 0 of SCTLR
mrc     p15, 0, r0, c1, c0, 0   // read SCTLR into r0
orr     r0, r0, #CR_M            // set M bit (MMU enable)
mcr     p15, 0, r0, c1, c0, 0   // write back to SCTLR
isb                              // instruction sync barrier

// Write TTBR0
mcr     p15, 0, r4, c2, c0, 0   // TTBR0 = r4

// Write TTBCR
mcr     p15, 0, r8, c2, c0, 2   // TTBCR = r8
```

### ARM64 — `msr`/`mrs` System Registers

```asm
// ARM64: direct MSR/MRS instructions
// Enable MMU: set bit 0 of SCTLR_EL1
mrs     x0, SCTLR_EL1            // read SCTLR_EL1 into x0
orr     x0, x0, #SCTLR_ELx_M    // set M bit (MMU enable)
msr     SCTLR_EL1, x0            // write back to SCTLR_EL1
isb                               // instruction sync barrier

// Write TTBR1_EL1
msr     TTBR1_EL1, x26           // direct write

// Write TCR_EL1
msr     TCR_EL1, x9              // direct write
```

ARM64 replaced the awkward `mcr p15, ...` with clean `msr <sysreg>` syntax.

---

## 4. Page Table Architecture

### ARM32 (no LPAE) — 2-Level

```
TTBR0 ──► L1 Table (PGD)
             │  4096 entries × 4 bytes = 16KB
             │  Each entry: 1MB section (block) or table pointer
             ▼
           L2 Table (PTE)
             │  256 entries × 4 bytes = 1KB
             │  Each entry: 4KB page descriptor
             ▼
           Physical Page (4KB)
```

Kernel uses **1MB sections** for most of its own mappings (no PTE needed).

### ARM32 (with LPAE) — 3-Level

```
TTBR0/1 ──► PGD (few entries)
                │
                ▼
              PMD (512 entries, 4KB)   or 2MB hugepage
                │
                ▼
              PTE (512 entries, 4KB)
                │
                ▼
              Physical Page (4KB)
```

LPAE extends physical addressing to 40-bit for >4GB systems.

### ARM64 — 4-Level (48-bit VA, 4KB pages)

```
TTBR1_EL1 ──► PGD (512 entries, 4KB)
                  │
                  ▼
                PUD (512 entries, 4KB)   or 1GB hugepage
                  │
                  ▼
                PMD (512 entries, 4KB)   or 2MB hugepage
                  │
                  ▼
                PTE (512 entries, 4KB)
                  │
                  ▼
                Physical Page (4KB)
```

---

## 5. VA Range Design

### ARM32 VA Split

```
ARM32 (no LPAE):

  0xFFFF_FFFF ─────────────────────────────────
               kernel space (1GB typical)
  0xC000_0000  ← PAGE_OFFSET (configurable: 3G/1G or 2G/2G)
  0xBFFF_FFFF
               user space (3GB typical)
  0x0000_0000 ─────────────────────────────────

TTBCR.N = 0  → full 4GB via TTBR0 (no kernel split)
TTBCR.N > 0  → lower (4GB >> N) via TTBR0, rest via TTBR1
```

### ARM64 VA Split

```
ARM64 (48-bit, T0SZ = T1SZ = 16):

  0xFFFF_FFFF_FFFF_FFFF ──────────────────────
                          kernel (256TB)
  0xFFFF_0000_0000_0000 ──────────────────────

          (hole — unmapped, 16 million TB)

  0x0000_FFFF_FFFF_FFFF ──────────────────────
                          user (256TB)
  0x0000_0000_0000_0000 ──────────────────────
```

ARM64 gives **vastly more** VA space to both kernel and user, eliminating
the 3GB/1GB tension that ARM32 had.

---

## 6. `head.S` MMU Enable Sequence Comparison

### ARM32 `head.S` Sequence

```
_stext:
    safe_svcmode_maskall r9     // set SVC mode, disable IRQs
    mrc p15,0,r9,c0,c0,0       // read processor ID
    bl  __lookup_processor_type // find proc_info
    bl  __vet_atags / __fixup_smp
    bl  __create_page_tables    // build swapper_pg_dir (L1 only!)
    ldr r13, =__mmap_switched   // virtual address to jump to
    __enable_mmu:
        mcr p15,0,r0,c2,c0,0   // write TTBR0
        mcr p15,0,r4,c2,c0,2   // write TTBCR
        mcr p15,0,r10,c3,c0,0  // write DACR (domain access control)
        b   __turn_mmu_on
    __turn_mmu_on:
        mcr p15,0,r0,c1,c0,0   // write SCTLR (set M bit)
        mrc p15,0,r3,c0,c0,0   // read processor ID (pipeline flush trick)
        mov r3, r13             // r13 = virtual address
        mov pc, r3              // jump to VA
```

### ARM64 `head.S` Sequence

```
primary_entry:
    bl  preserve_boot_args
    bl  init_kernel_el         // handle EL2→EL1 if needed
    bl  __create_page_tables   // build init_pg_dir + idmap_pg_dir
    bl  __cpu_setup            // configure MAIR, TCR (proc.S)
    b   __primary_switch

__primary_switch:
    adrp x1, idmap_pg_dir
    adrp x2, init_pg_dir
    bl  __enable_mmu           // write TTBRs, set SCTLR_EL1.M, isb
    // __enable_mmu returns to __primary_switched via lr
```

**Key difference:** ARM64 uses `lr` (link register) loaded with the VA
of `__primary_switched` to jump to virtual address after `ret`.
ARM32 loads the VA into `r13`/`pc` directly.

---

## 7. `paging_init()` Comparison

### ARM32 `paging_init()`
- Called from `setup_arch()` with `mdesc` (machine descriptor)
- Calls `prepare_page_table()` to clear boot-time L1 entries
- Calls `map_lowmem()` to map all lowmem (contiguous)
- Calls `devicemaps_init(mdesc)` to map device I/O
- Calls `bootmem_init()` → `zone_sizes_init()` → buddy allocator
- ARM32 **uses the same `swapper_pg_dir`** from `head.S` (no temp table)

### ARM64 `paging_init()`
- Called from `setup_arch()` (no mdesc)
- Calls `map_kernel()` — maps kernel image with proper permissions
- Calls `map_mem()` — maps all DRAM as linear map
- Calls `cpu_replace_ttbr1(swapper_pg_dir)` — **replaces `init_pg_dir`**
- Calls `memblock_free(init_pg_dir)` — frees the temporary table
- Calls `sparse_init()` → `free_area_init()` → buddy allocator

ARM64 has an **extra stage** because of KASLR: the kernel image location
is randomized at runtime, so `head.S` cannot know the final VA layout at
compile time. `init_pg_dir` is the minimal "guess" that gets replaced.

---

## 8. KASLR Difference

| Feature | ARM32 | ARM64 |
|---|---|---|
| KASLR | Not supported | `CONFIG_RANDOMIZE_BASE` |
| Kernel load address | Fixed at link time | Random offset within DRAM |
| `KIMAGE_VADDR` | Fixed | `KIMAGE_VADDR + kaslr_offset` |
| `init_pg_dir` needed | No (static layout) | Yes (must survive random VA) |

Because ARM64 randomizes the kernel's VA, the temporary `init_pg_dir` is
needed as a bridge until `paging_init()` can build the final layout
after discovering the actual KASLR offset.

---

## 9. Summary: What ARM64 Improved Over ARM32

| Area | ARM32 Issue | ARM64 Solution |
|---|---|---|
| **Register access** | Complex `mcr/mrc p15` syntax | Clean `msr/mrs sysreg` |
| **VA space** | 3GB/1GB cramped split | 256TB/256TB per half |
| **PA size** | Max 40-bit (LPAE) | Up to 52-bit (LPA) |
| **Page table depth** | 2 or 3 levels | Consistent 4 levels |
| **Security (W^X)** | Optional | `WXN` bit in `SCTLR_EL1` |
| **KASLR** | Not available | Full randomization support |
| **Exception levels** | Modes (SVC, ABT, IRQ...) | Clean EL0/EL1/EL2/EL3 |
| **ASID width** | 8-bit | 8 or 16-bit |
| **TBI** | Not available | Top Byte Ignore for pointer tagging |
