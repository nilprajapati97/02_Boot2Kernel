# 02 — Identity Map & Kernel Mapping: Code Walkthrough

[← Doc 01: Image Entry](01_Image_Entry_head_S.md) | **Doc 02** | [Doc 03: start_kernel → setup_arch →](03_start_kernel_to_setup_arch.md)

---

## Overview

Before the MMU can be enabled, the kernel needs page tables that map its own code at
the physical address where it's loaded (identity map: VA = PA). After MMU is on, a
second mapping creates the kernel at its high virtual address (link address). This
document traces the exact code that builds both mappings.

---

## 1. `create_init_idmap()` — Building the Identity Map

**Source**: `arch/arm64/kernel/pi/map_range.c`  
**Called by**: `primary_entry` in head.S as `bl __pi_create_init_idmap`  
**Prototype**:

```c
asmlinkage phys_addr_t __init create_init_idmap(pgd_t *pg_dir, ptdesc_t clrmask)
```

**Arguments at call site**:
- `x0` (`pg_dir`) = physical address of `init_idmap_pg_dir` (from linker script)
- `x1` (`clrmask`) = 0 (no bits to clear from protection attributes)

**Code**:

```c
asmlinkage phys_addr_t __init create_init_idmap(pgd_t *pg_dir, ptdesc_t clrmask)
{
    /* ptep starts right after the PGD — bump allocator for sub-tables */
    phys_addr_t ptep = (phys_addr_t)pg_dir + PAGE_SIZE;

    pgprot_t text_prot = PAGE_KERNEL_ROX;   // Read-Only + eXecute
    pgprot_t data_prot = PAGE_KERNEL;       // Read-Write, No-eXecute

    pgprot_val(text_prot) &= ~clrmask;      // Apply clrmask (0 = no change)
    pgprot_val(data_prot) &= ~clrmask;

    /*
     * Identity mapping #1: kernel text [_stext, __initdata_begin)
     * VA == PA, read-only executable
     */
    map_range(&ptep,
              (u64)_stext,              // start VA (= PA, identity map)
              (u64)__initdata_begin,    // end VA
              (phys_addr_t)_stext,      // PA
              text_prot,                // PAGE_KERNEL_ROX
              IDMAP_ROOT_LEVEL,         // root level (0 for 4-level)
              (pte_t *)pg_dir,          // page table base
              false,                    // may_use_cont = false
              0);                       // va_offset = 0 (identity: VA=PA)

    /*
     * Identity mapping #2: kernel data [__initdata_begin, _end)
     * VA == PA, read-write non-executable
     */
    map_range(&ptep,
              (u64)__initdata_begin,    // start VA
              (u64)_end,                // end VA
              (phys_addr_t)__initdata_begin,  // PA
              data_prot,                // PAGE_KERNEL
              IDMAP_ROOT_LEVEL,
              (pte_t *)pg_dir,
              false,
              0);

    return ptep;    // Return end of used page table memory (in x0)
}
```

**What this creates**:
```
init_idmap_pg_dir:
  PGD[idx] → PUD → PMD → PTE entries mapping:
    VA 0x4008_0000 → PA 0x4008_0000  (text, ROX)  ← identity map
    VA 0x40xx_xxxx → PA 0x40xx_xxxx  (data, RW)   ← identity map
```

---

## 2. `map_range()` — The Recursive Page Table Builder

**Source**: `arch/arm64/kernel/pi/map_range.c`  
**Prototype**:

```c
void __init map_range(phys_addr_t *pte,     // bump allocator: next free page
                      u64 start,             // virtual address start
                      u64 end,               // virtual address end (exclusive)
                      phys_addr_t pa,         // physical address of start
                      pgprot_t prot,          // protection attributes
                      int level,              // current page table level (0-3)
                      pte_t *tbl,             // page table array at this level
                      bool may_use_cont,      // allow PTE_CONT attribute
                      u64 va_offset)          // phys→VA offset for child table access
```

### 2.1 Setup

```c
{
    u64 cmask = (level == 3) ? CONT_PTE_SIZE - 1 : U64_MAX;
    ptdesc_t protval = pgprot_val(prot) & ~PTE_TYPE_MASK;
    int lshift = (3 - level) * PTDESC_TABLE_SHIFT;  // shift for this level
    u64 lmask = (PAGE_SIZE << lshift) - 1;           // mask for this level's block size

    /* Add block/page type bits to protection value */
    if (level == 2)
        protval |= PMD_TYPE_SECT;     // 2MB section descriptor
    else if (level == 3)
        protval |= PTE_TYPE_PAGE;     // 4KB page descriptor
```

Level-to-size mapping (4KB granule):

| Level | `lshift` | Block Size | Descriptor Type |
|-------|----------|-----------|-----------------|
| 0 (PGD) | 27 | 512 GB | Table only |
| 1 (PUD) | 18 | 1 GB | Table or Block |
| 2 (PMD) | 9 | 2 MB | Table or Block (section) |
| 3 (PTE) | 0 | 4 KB | Page only |

### 2.2 Index Calculation and Walk

```c
    /* Advance tbl pointer to the entry covering 'start' */
    tbl += (start >> (lshift + PAGE_SHIFT)) % PTRS_PER_PTE;
```

Each page table has 512 entries (9 bits of VA index). This computes which entry
in the table corresponds to `start`.

### 2.3 The Main Loop — Recursive Descent

```c
    while (start < end) {
        u64 next = min((start | lmask) + 1, PAGE_ALIGN(end));
        //   next = end of this entry's VA range, clamped to 'end'
        //   Example: level 2, start=0x4020_0000
        //            lmask = 0x1F_FFFF (2MB-1)
        //            next = (0x4020_0000 | 0x1F_FFFF) + 1 = 0x4040_0000

        if (level < 2 || (level == 2 && (start | next | pa) & lmask)) {
            /*
             * NEED FINER MAPPING:
             * - Levels 0,1: always need subtable (no block at PGD)
             * - Level 2: start/end/PA not 2MB-aligned → need 4KB pages
             */
            if (pte_none(*tbl)) {
                /* Allocate a new sub-table page from bump allocator */
                *tbl = __pte(__phys_to_pte_val(*pte) |
                             PMD_TYPE_TABLE | PMD_TABLE_UXN);
                *pte += PTRS_PER_PTE * sizeof(pte_t);  // advance allocator
                //   *pte += 512 * 8 = 4096 bytes = 1 page
            }
            /* RECURSE into next level */
            map_range(pte, start, next, pa, prot, level + 1,
                      (pte_t *)(__pte_to_phys(*tbl) + va_offset),
                      may_use_cont, va_offset);

        } else {
            /*
             * CAN PLACE BLOCK/PAGE MAPPING:
             * - Level 2: 2MB section (start, next, PA all 2MB-aligned)
             * - Level 3: 4KB page
             */
            /* Check if contiguous hint is applicable */
            if (((start | pa) & cmask) == 0 && may_use_cont)
                protval |= PTE_CONT;
            if ((end & ~cmask) <= start)
                protval &= ~PTE_CONT;

            /* Write the page table entry */
            *tbl = __pte(__phys_to_pte_val(pa) | protval);
        }

        pa += next - start;   // Advance PA
        start = next;         // Advance VA
        tbl++;                // Next entry in this table
    }
}
```

### 2.4 Recursion Example

For a kernel loaded at PA `0x4008_0000` (not 1GB-aligned):

```
map_range(level=0, PGD)         ← 512GB entries
  │  Entry covers 0x0..0x7F_FFFF_FFFF
  │  Allocate PUD sub-table, recurse
  │
  └─► map_range(level=1, PUD)   ← 1GB entries
        │  Entry covers 0x4000_0000..0x7FFF_FFFF
        │  Not 1GB-aligned → allocate PMD, recurse
        │
        └─► map_range(level=2, PMD)  ← 2MB entries
              │  Entry at 0x4000_0000: 2MB-aligned → BLOCK MAPPING
              │  *tbl = PA 0x4000_0000 | PMD_TYPE_SECT | protval
              │
              │  Entry at 0x4020_0000: 2MB-aligned → BLOCK MAPPING
              │  *tbl = PA 0x4020_0000 | PMD_TYPE_SECT | protval
              │
              │  (continues for each 2MB chunk of kernel...)
```

---

## 3. `early_map_kernel()` — Mapping the Kernel at Virtual Address

**Source**: `arch/arm64/kernel/pi/map_kernel.c`  
**Called by**: `__primary_switch` as `bl __pi_early_map_kernel`  
**Prototype**:

```c
asmlinkage void __init early_map_kernel(u64 boot_status, phys_addr_t fdt)
```

**Arguments at call site**:
- `x0` (`boot_status`) = `x20` = boot CPU mode from `init_kernel_el`
- `x1` (`fdt`) = `x21` = FDT physical address

### 3.1 Map FDT into identity map

```c
{
    u64 va_base, pa_base = (u64)&_text;   // PA of kernel start
    u64 kaslr_offset = pa_base % MIN_KIMG_ALIGN;
    int root_level = 4 - CONFIG_PGTABLE_LEVELS;  // 0 for 4-level
    int va_bits = VA_BITS;                        // 48

    void *fdt_mapped = map_fdt(fdt);
    //   Identity-maps the FDT into init_idmap_pg_dir using map_range()
    //   Returns pointer to FDT in identity-mapped VA (= PA since identity)
```

### 3.2 Clear BSS and Page Tables

```c
    memset(__bss_start, 0, (char *)init_pg_end - (char *)__bss_start);
    //   Zeros BSS + init_pg_dir + swapper_pg_dir pages
    //   Critical: page tables must be zero before populating
```

### 3.3 Parse CPU Features from DT

```c
    chosen = fdt_path_offset(fdt_mapped, "/chosen");
    init_feature_override(boot_status, fdt_mapped, chosen);
    //   Applies "arm64,feature-override" from DT /chosen node
    //   Can disable features like PAuth, MTE, etc.
```

### 3.4 VA Bits and LPA2 Adjustments

```c
    if (IS_ENABLED(CONFIG_ARM64_64K_PAGES) && !cpu_has_lva())
        va_bits = VA_BITS_MIN;
    else if (IS_ENABLED(CONFIG_ARM64_LPA2) && !cpu_has_lpa2()) {
        va_bits = VA_BITS_MIN;
        root_level++;
    }

    if (va_bits > VA_BITS_MIN)
        sysreg_clear_set(tcr_el1, TCR_EL1_T1SZ_MASK, TCR_T1SZ(va_bits));
        //   Expand TTBR1 VA size from VA_BITS_MIN to full VA_BITS
```

### 3.5 KASLR Seed

```c
    if (IS_ENABLED(CONFIG_RANDOMIZE_BASE)) {
        u64 kaslr_seed = kaslr_early_init(fdt_mapped, chosen);
        //   Read "kaslr-seed" property from /chosen
        //   Returns randomized offset
        if (kaslr_seed && kaslr_requires_kpti())
            arm64_use_ng_mappings = ng_mappings_allowed();
        kaslr_offset |= kaslr_seed & ~(MIN_KIMG_ALIGN - 1);
    }
```

### 3.6 Build Kernel Mapping

```c
    va_base = KIMAGE_VADDR + kaslr_offset;
    //   KIMAGE_VADDR = the kernel image link-address base
    //   Example: 0xFFFF_8000_1000_0000 + kaslr_offset

    map_kernel(kaslr_offset, va_base - pa_base, root_level);
    //   va_offset = VA - PA: used to translate symbol addresses to VA
}
```

---

## 4. `map_kernel()` — Creating the 6 Kernel Segments

**Source**: `arch/arm64/kernel/pi/map_kernel.c`  
**Prototype**:

```c
static void __init map_kernel(u64 kaslr_offset, u64 va_offset, int root_level)
```

**Arguments**:
- `kaslr_offset` = KASLR randomization offset (0 if KASLR disabled)
- `va_offset` = `KIMAGE_VADDR + kaslr_offset - pa_base` (the VA-PA difference)
- `root_level` = 0 (4-level page tables)

### 4.1 `map_segment()` Helper

```c
static void __init map_segment(pgd_t *pg_dir, phys_addr_t *pgd, u64 va_offset,
                               void *start, void *end, pgprot_t prot,
                               bool may_use_cont, int root_level)
{
    map_range(pgd,
              ((u64)start + va_offset) & ~PAGE_OFFSET,  // VA (masked)
              ((u64)end + va_offset) & ~PAGE_OFFSET,    // VA end (masked)
              (u64)start,                                // PA (symbol addr)
              prot,
              root_level,
              (pte_t *)pg_dir,
              may_use_cont,
              0);
}
```

### 4.2 The 6 Segments

```c
static void __init map_kernel(u64 kaslr_offset, u64 va_offset, int root_level)
{
    pgd_t *pg_dir = (void *)init_pg_dir + va_offset;
    phys_addr_t pgd = (phys_addr_t)init_pg_dir + PAGE_SIZE;

    pgprot_t text_prot = PAGE_KERNEL_ROX;    // executable, read-only
    pgprot_t data_prot = PAGE_KERNEL;        // read-write, no-execute
    bool enable_scs = IS_ENABLED(CONFIG_UNWIND_PATCH_PAC_INTO_SCS);

    /* If SCS patching needed → map text as RW first, then re-map as ROX */
    if (enable_scs)
        text_prot = PAGE_KERNEL;             // Temporarily writable

    /* Segment 1: pre-text data [_text, _stext) */
    map_segment(pg_dir, &pgd, va_offset, _text, _stext,
                data_prot, false, root_level);

    /* Segment 2: kernel text [_stext, _etext) */
    map_segment(pg_dir, &pgd, va_offset, _stext, _etext,
                text_prot, !enable_scs, root_level);

    /* Segment 3: rodata [__start_rodata, __inittext_begin) */
    map_segment(pg_dir, &pgd, va_offset, __start_rodata, __inittext_begin,
                data_prot, false, root_level);

    /* Segment 4: init text [__inittext_begin, __inittext_end) */
    map_segment(pg_dir, &pgd, va_offset, __inittext_begin, __inittext_end,
                text_prot, false, root_level);

    /* Segment 5: init data [__initdata_begin, __initdata_end) */
    map_segment(pg_dir, &pgd, va_offset, __initdata_begin, __initdata_end,
                data_prot, false, root_level);

    /* Segment 6: data + BSS [_data, _end) */
    map_segment(pg_dir, &pgd, va_offset, _data, _end,
                data_prot, true, root_level);
```

### 4.3 Switch to init_pg_dir

```c
    /* Ensure page table writes are visible */
    dsb(ishst);
    idmap_cpu_replace_ttbr1((phys_addr_t)init_pg_dir);
    //   msr ttbr1_el1, init_pg_dir   ← TTBR1 now points to kernel mapping
    //   isb
```

### 4.4 SCS/Relocatable Fix-up (Two-Pass)

```c
    if (IS_ENABLED(CONFIG_RELOCATABLE)) {
        relocate_kernel(kaslr_offset);
        //   Patch relocations in kernel image for KASLR offset
    }
    if (enable_scs) {
        scs_patch(__pi_scs_patch_phys_start + va_offset,
                  __pi_scs_patch_phys_end + va_offset);
    }

    /* If text was temporarily writable, re-map as read-only executable */
    if (enable_scs || IS_ENABLED(CONFIG_RELOCATABLE)) {
        /* Unmap text from init_pg_dir (set entries to zero) */
        unmap_segment(init_pg_dir, va_offset, _stext, _etext, root_level);
        dsb(ishst);
        tlbi(vmalle1);    // Invalidate TLB
        isb();

        /* Re-map text as PAGE_KERNEL_ROX */
        map_segment(init_pg_dir, &pgd, va_offset, _stext, _etext,
                    PAGE_KERNEL_ROX, true, root_level);
        dsb(ishst);
        isb();
    }
```

### 4.5 Copy to swapper_pg_dir and Final Switch

```c
    /* Copy PGD root entries from init_pg_dir → swapper_pg_dir */
    memcpy((void *)swapper_pg_dir + va_offset, init_pg_dir, PAGE_SIZE);
    dsb(ishst);

    /* Switch TTBR1 from init_pg_dir → swapper_pg_dir */
    idmap_cpu_replace_ttbr1((phys_addr_t)swapper_pg_dir);
    //   This is the FINAL TTBR1 value for the rest of boot
}
```

**After `map_kernel()`**:
```
TTBR1 = swapper_pg_dir
  Maps kernel image at virtual addresses:
    0xFFFF_8000_1xxx_xxxx  →  PA where kernel is loaded
    (6 segments with appropriate RO/RW/X/NX permissions)

TTBR0 = init_idmap_pg_dir  (still identity map)
```

---

## 5. Page Table Memory Layout in BSS

The page table arrays are defined by the linker script and live in BSS:

```
Defined in arch/arm64/kernel/vmlinux.lds.S:

  init_idmap_pg_dir     [INIT_IDMAP_DIR_SIZE pages]    ← Identity map (TTBR0)
  init_idmap_pg_end

  init_pg_dir           [INIT_DIR_SIZE pages]           ← Initial kernel map
  init_pg_end

  swapper_pg_dir        [PAGE_SIZE]                     ← Final kernel PGD (TTBR1)
                        (sub-tables allocated from init_pg_dir region)
```

The bump allocator (`ptep`) starts at `pg_dir + PAGE_SIZE` and allocates
sub-level tables sequentially. Each new table consumes one page (4096 bytes,
512 entries × 8 bytes each).

---

## State After Document 02

```
┌──────────────────────────────────────────────────────────┐
│  Page Tables Built:                                      │
│                                                          │
│  TTBR0 = init_idmap_pg_dir:                              │
│    Identity map (VA = PA) for:                           │
│    • Kernel text [_stext .. __initdata_begin) → ROX      │
│    • Kernel data [__initdata_begin .. _end) → RW         │
│    • FDT (mapped by map_fdt)                             │
│                                                          │
│  TTBR1 = swapper_pg_dir:                                 │
│    Kernel virtual map at KIMAGE_VADDR:                   │
│    • _text → _stext:           data (RW)                 │
│    • _stext → _etext:          text (ROX)                │
│    • __start_rodata → __inittext_begin: rodata (RW/NX)   │
│    • __inittext_begin → __inittext_end: init text (ROX)  │
│    • __initdata_begin → __initdata_end: init data (RW)   │
│    • _data → _end:             data+BSS (RW)             │
│                                                          │
│  map_range() built tables using a bump allocator:        │
│    Level 0 (PGD) → Level 1 (PUD) → Level 2 (PMD)        │
│    2MB section mappings where aligned                    │
│    4KB page mappings at unaligned boundaries              │
│                                                          │
│  NOT YET MAPPED:                                         │
│    ✗ Physical RAM (linear map) — done in paging_init()   │
│    ✗ No memblock, no allocator                           │
│    ✗ No DTB parsing for memory banks                     │
│                                                          │
│  Next: start_kernel() → setup_arch() → memory discovery  │
└──────────────────────────────────────────────────────────┘
```

---

[← Doc 01: Image Entry](01_Image_Entry_head_S.md) | **Doc 02** | [Doc 03: start_kernel → setup_arch →](03_start_kernel_to_setup_arch.md)
