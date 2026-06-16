# TTBR Base Address Alignment Requirements per Granule

**Category**: Page Tables and Translation  
**Targeted**: ARM, Qualcomm

---

## 1. Concept Foundation

`TTBR0_EL1` and `TTBR1_EL1` store the physical base address of the top-level page table (PGD). The hardware page table walker uses this address to begin the translation walk. The base address must be **naturally aligned** to the size of the top-level table, because the walker uses it as a base for index-based lookups.

If the TTBR base address is misaligned, the hardware will read wrong memory as page table entries, leading to random Translation Faults, wrong mappings, or security holes.

---

## 2. Alignment Requirements by Granule

### 4KB Granule (4-level, 48-bit VA)

```
Top-level table size: 512 entries × 8 bytes = 4,096 bytes = 4 KB
Alignment requirement: 4 KB (PA[11:0] = 0)

TTBR0_EL1[47:12] << 12 = Base PA
PA must satisfy: PA & 0xFFF == 0

Example: Valid   = 0x4000_0000_0000 (PA bits[11:0] = 0x000 ✓)
         Invalid = 0x4000_0001_0100 (PA bits[11:0] = 0x100 ✗)
```

### 16KB Granule (4-level, 48-bit VA)

```
Top-level is concatenated:
  Level 0 consists of 2 concatenated tables (one per VA[47:47] bit)
  2 × 16KB = 32KB total
  Alignment requirement: 32 KB (PA[14:0] = 0)

Single 16KB table (one level): 16 KB alignment

For 3-level 16KB (36-bit VA):
  Top-level table: 2048 entries × 8B = 16,384 bytes = 16 KB
  Alignment: 16 KB (PA[13:0] = 0)
```

### 64KB Granule (3-level, 48-bit VA)

```
Top-level is concatenated:
  Level 0 has 64 entries (6-bit index)
  64 × 8B = 512 bytes (fits in a 64KB page, but occupies only 512 bytes)
  
  Actually: top-level table is a FULL 64KB page (for alignment)
  The walker reads L0 entries at offsets within the 64KB page
  Alignment requirement: 64 KB (PA[15:0] = 0)

For 2-level 64KB (42-bit VA):
  Top-level: 8192 entries × 8B = 65,536 bytes = 64 KB
  Alignment: 64 KB (PA[15:0] = 0)
```

---

## 3. Alignment Summary Table

```
Granule │ VA bits │ Levels │ Top-level size │ Required Alignment
────────┼─────────┼────────┼────────────────┼───────────────────
4 KB    │  48     │   4    │   4 KB         │   4 KB (12-bit align)
4 KB    │  39     │   3    │   4 KB         │   4 KB (12-bit align)
4 KB    │  52     │   5    │   4 KB         │   4 KB (12-bit align)
16 KB   │  48     │   4    │  32 KB (conc.) │  32 KB (15-bit align)
16 KB   │  36     │   2-3  │  16 KB         │  16 KB (14-bit align)
64 KB   │  48     │   3    │  64 KB (conc.) │  64 KB (16-bit align)
64 KB   │  42     │   2    │  64 KB         │  64 KB (16-bit align)
```

---

## 4. TTBR Register Format

### Standard (pre-ARMv8.2)

```
TTBR0_EL1 (64-bit):
  Bits[63:48] = ASID (8 or 16 bits, upper bits zero if 8-bit)
  Bits[47:0]  = Base physical address of top-level table
                (lower bits implicit zero per alignment)

Example (4KB granule):
  ASID = 42 (0x002A)
  PGD PA = 0x0000_4000_0000_0000
  TTBR0_EL1 = 0x002A_4000_0000_0000
  
  Hardware reads base address as: TTBR0_EL1[47:12] << 12
  = 0x4000_0000_0000 ✓
```

### With LPA (ARMv8.2, 52-bit PA, 64KB granule)

```
For 64KB granule + 52-bit PA, descriptor PA[51:48] is stored
in descriptor bits[15:12] (not bits[47:44]).

TTBR format is extended for 52-bit PA:
  Bits[5:2] = PA[51:48] of the top-level table (stored in low bits!)
  Bits[47:16] = PA[47:16]
  Bits[15:0] = Must be 0 (64KB aligned)

This is because the standard TTBR bits[47:0] field can only address 48 bits.
The "overflow" PA[51:48] is packed into unused bits[5:2].
```

---

## 5. Linux Page Table Allocation

```c
// mm/mmu.c / arch/arm64/mm/pgd.c

// PGD allocation for a process:
pgd_t *pgd_alloc(struct mm_struct *mm)
{
    // Allocate 1 page (4KB) for the PGD:
    pgd_t *new_pgd = (pgd_t *)__get_free_page(GFP_KERNEL);
    // __get_free_page always returns a page-aligned (4KB-aligned) address
    // → satisfies 4KB alignment requirement automatically
    
    if (!new_pgd)
        return NULL;
    
    // Copy kernel mappings from swapper_pg_dir (top half for TTBR1):
    // (For TTBR0, all entries start invalid)
    memset(new_pgd, 0, PGD_SIZE);
    pgd_populate_init(mm, new_pgd);
    
    return new_pgd;
}

// For 16KB or 64KB granules, Linux uses order-N page allocations:
// 16KB granule PGD = __get_free_pages(GFP_KERNEL, order=2) (4 pages = 16KB)
// 64KB granule PGD = __get_free_pages(GFP_KERNEL, order=4) (16 pages = 64KB)
// These allocations are naturally aligned to their size by the buddy allocator.
```

---

## 6. Checking Alignment (Debugging)

```c
// Debugging: verify PGD alignment
pgd_t *pgd = mm->pgd;
phys_addr_t pa = virt_to_phys(pgd);

// For 4KB granule:
WARN_ON(pa & 0xFFF);     // Must be 4KB-aligned

// For 16KB granule:
WARN_ON(pa & 0x3FFF);    // Must be 16KB-aligned

// For 64KB granule:
WARN_ON(pa & 0xFFFF);    // Must be 64KB-aligned

// At runtime, read TTBR0_EL1 and verify:
// (from EL1 kernel code)
u64 ttbr = read_sysreg(ttbr0_el1);
phys_addr_t base = ttbr & ~(u64)ASID_MASK;
// base[11:0] should be 0 for 4KB granule
```

---

## 7. What Happens on Misalignment

If a misaligned address is written to TTBR:

```
ARM64 architecture behavior:
  TTBR bits below the required alignment boundary are IGNORED or UNPREDICTABLE.

Example (4KB granule):
  Write TTBR0 = 0x0000_4000_0000_0100 (misaligned by 256 bytes)
  Hardware uses: 0x0000_4000_0000_0000 (lower 12 bits forced to 0)
  → Walker reads entries from 0x4000_0000_0000, not 0x4000_0000_0100
  → Entries at offset 0x100 are never reached
  → Process PGD is effectively wrong
  → All accesses fault (Translation Fault) because PGD entries are garbage

In practice: misaligned TTBR causes immediate kernel panic (invalid instruction 
fetch for kernel, process crash for user)
```

---

## 8. Interview Questions & Answers

**Q1: Why must TTBR0_EL1 be aligned to the top-level page table size?**

The hardware page table walker computes the address of each entry by using the TTBR base address as a starting point and adding `index × 8`. For this to work, the base address must be aligned so that the natural binary indexing is correct. If TTBR is misaligned, the computed addresses for entries would overlap with each other or fall on wrong cache lines. The ARM64 architecture defines that bits below the alignment boundary in TTBR are either ignored or produce UNPREDICTABLE behavior. Linux ensures proper alignment by using the kernel page allocator (`__get_free_page`/`__get_free_pages`), which returns naturally-aligned memory.

**Q2: What alignment does a 64KB-granule system require for TTBR?**

64KB alignment (`PA[15:0] = 0`). With 64KB granule, each page table is 64KB (8192 entries × 8 bytes). The PTW hardware reads the base address from TTBR and performs 64KB-aligned accesses. On Linux, this means the PGD must be allocated as 16 contiguous 4KB pages (`__get_free_pages(GFP_KERNEL, 4)`), which the buddy allocator guarantees to be 64KB-aligned.

**Q3: How does ARMv8.2 LPA (52-bit PA) affect the TTBR format for 64KB granule?**

In standard ARM64, `TTBR0_EL1[47:0]` holds the top-level table PA. With 52-bit PA and 64KB granule (LPA), the PA can have bits[51:48] set. Since bits[47:0] in the standard TTBR cannot hold 52-bit PA, the extra 4 bits (`PA[51:48]`) are stored in `TTBR0_EL1[5:2]` (otherwise unused/zero bits). The hardware reads `TTBR.PA = {TTBR[5:2], TTBR[47:16], 16'b0}` to form the 52-bit base address. Linux handles this in `arch/arm64/mm/pgd.c` by packing the upper 4 PA bits into TTBR[5:2] when LPA is enabled.

---

## 9. Quick Reference

| Granule | VA Width | Top-Level Entries | Table Size | TTBR Alignment |
|---|---|---|---|---|
| 4 KB | 48 bits | 512 | 4 KB | 4 KB |
| 4 KB | 39 bits | 512 | 4 KB | 4 KB |
| 16 KB | 48 bits | 2 × 2048 | 32 KB (conc.) | 32 KB |
| 16 KB | 36 bits | 2048 | 16 KB | 16 KB |
| 64 KB | 48 bits | 64 (conc.) | 64 KB | 64 KB |
| 64 KB | 42 bits | 8192 | 64 KB | 64 KB |

| TTBR field | Bits | Content |
|---|---|---|
| ASID | [63:48] | Process ASID (8 or 16 bits) |
| Base PA | [47:0] | Page table physical base address |
| CnP | [0] | Common-not-Private (ARMv8.2) |
