# 03 — `start_kernel()` → `setup_arch()`: Memory Discovery

[← Doc 02: Identity Map & Kernel Map](02_Identity_Map_and_Kernel_Map.md) | **Doc 03** | [Doc 04: Memblock Operations →](04_Memblock_Operations.md)

---

## 1. `start_kernel()` — The C Entry Point

**Source**: `init/main.c`  
**Called by**: `__primary_switched` in head.S (`bl start_kernel`)

The memory-related calls in exact order:

```c
void start_kernel(void)                              // init/main.c:1017
{
    // ... early init (printk, params, etc.) ...

    setup_arch(&command_line);                       // line ~1038 ★
    mm_core_init_early();                            // line ~1039 ★
    // ...
    setup_per_cpu_areas();                           // line ~1048
    // ...
    mm_core_init();                                  // line ~1082 ★
    // ...
    kmem_cache_init_late();                          // line ~1151 ★
    // ...
    setup_per_cpu_pageset();                         // line ~1173 ★
    // ... (later, in kernel_init_freeable()):
    page_alloc_init_late();                          // line ~1701 ★
}
```

This document covers the first call: `setup_arch()`.

---

## 2. `setup_arch()` — Architecture-Specific Setup

**Source**: `arch/arm64/kernel/setup.c`  
**Prototype**: `void __init __no_sanitize_address setup_arch(char **cmdline_p)`

### 2.1 Memory-Related Call Sequence

```c
void __init __no_sanitize_address setup_arch(char **cmdline_p)
{
    setup_initial_init_mm(_text, _etext, _edata, _end);
    //   init_mm.start_code = _text
    //   init_mm.end_code = _etext
    //   init_mm.end_data = _edata
    //   init_mm.brk = _end

    *cmdline_p = boot_command_line;
    kaslr_init();                              // Finalize KASLR state

    early_fixmap_init();                       // ★ Step 1
    early_ioremap_init();                      // ★ Step 2

    setup_machine_fdt(__fdt_pointer);          // ★ Step 3: FDT → memblock
    // ...
    cpu_uninstall_idmap();                     // Remove identity map from TTBR0
    // ...
    efi_init();                                // EFI memory map (if UEFI boot)

    arm64_memblock_init();                     // ★ Step 4: memblock reservations
    paging_init();                             // ★ Step 5: linear map
    // ...
    bootmem_init();                            // ★ Step 6: NUMA, zones, CMA
    // ...
}
```

---

## 3. `early_fixmap_init()` — Fixed Virtual Address Mappings

**Source**: `arch/arm64/mm/fixmap.c`  
**Purpose**: Set up the fixmap page table hierarchy so fixed VAs can be mapped later.

```c
void __init early_fixmap_init(void)
{
    // Walk swapper_pg_dir to create PGD→P4D→PUD→PMD chain for FIXADDR_START
    pgd_t *pgdp = pgd_offset_k(FIXADDR_START);
    p4d_t *p4dp = p4d_offset(pgdp, FIXADDR_START);
    pud_t *pudp = ...;    // Allocate from bm_pud[]
    pmd_t *pmdp = ...;    // Allocate from bm_pmd[]

    // The PMD entry for fixmap is set up but PTE table allocated later
    // This allows early_ioremap and FDT mapping to use fixmap slots
}
```

**Fixmap slots** include:
- `FIX_FDT` — for mapping the Device Tree at a known VA
- `FIX_EARLYCON_MEM_BASE` — for early console MMIO

---

## 4. `setup_machine_fdt()` — Parse Device Tree, Discover Memory

**Source**: `arch/arm64/kernel/setup.c`  
**Prototype**: `static void __init setup_machine_fdt(phys_addr_t dt_phys)`  
**Argument**: `__fdt_pointer` = physical address of DTB (set by head.S)

```c
static void __init setup_machine_fdt(phys_addr_t dt_phys)
{
    int size;
    void *dt_virt = fixmap_remap_fdt(dt_phys, &size, PAGE_KERNEL);
    //   Map DTB into fixmap at FIX_FDT virtual address
    //   Returns virtual address of DTB
    //   size = FDT total size
    //   Internally: __fixmap_remap_fdt() → create_mapping_noalloc()

    if (!dt_virt || !early_init_dt_scan(dt_virt)) {
        // ...error handling...
    }

    // After scanning, the DTB data is available at dt_virt
    dump_stack_set_arch_desc("%s (DT)", of_flat_dt_get_machine_name());
}
```

### 4.1 `early_init_dt_scan()` — DT Scanning

```c
bool __init early_init_dt_scan(void *dt_virt)
{
    // Validate FDT magic, version
    if (!dt_virt || fdt_check_header(dt_virt))
        return false;

    initial_boot_params = dt_virt;    // Global FDT pointer

    // Scan /chosen node: bootargs, initrd, stdout-path
    early_init_dt_scan_chosen(boot_command_line);

    // Scan /memory nodes → DISCOVER RAM
    early_init_dt_scan_memory();      // ★ THE KEY CALL
    return true;
}
```

---

## 5. `early_init_dt_scan_memory()` — RAM Discovery from DT

**Source**: `drivers/of/fdt.c`  
**Called by**: `early_init_dt_scan()` → `setup_machine_fdt()`  
**This is where the kernel first learns how much RAM exists.**

```c
int __init early_init_dt_scan_memory(void)
{
    int node, found_memory = 0;
    const void *fdt = initial_boot_params;

    /* Walk ALL subnodes of the root "/" node */
    fdt_for_each_subnode(node, fdt, 0) {
        const char *type = of_get_flat_dt_prop(node, "device_type", NULL);
        const __be32 *reg;
        int i, l;
        bool hotpluggable;

        /* Skip nodes that are not device_type = "memory" */
        if (type == NULL || strcmp(type, "memory") != 0)
            continue;

        /* Skip disabled nodes */
        if (!of_fdt_device_is_available(fdt, node))
            continue;

        /* Try "linux,usable-memory" first, fall back to "reg" */
        reg = of_flat_dt_get_addr_size_prop(node, "linux,usable-memory", &l);
        if (reg == NULL)
            reg = of_flat_dt_get_addr_size_prop(node, "reg", &l);
        if (reg == NULL)
            continue;

        hotpluggable = of_get_flat_dt_prop(node, "hotpluggable", NULL);

        /* Parse each (base, size) pair from the reg property */
        for (i = 0; i < l; i++) {
            u64 base, size;

            of_flat_dt_read_addr_size(reg, i, &base, &size);
            //   Reads #address-cells and #size-cells worth of data
            //   Converts from big-endian DT format to native u64

            if (size == 0)
                continue;

            found_memory = 1;

            early_init_dt_add_memory_arch(base, size);   // ★ → memblock_add()

            if (hotpluggable)
                memblock_mark_hotplug(base, size);
        }
    }
    return found_memory;
}
```

### 5.1 `early_init_dt_add_memory_arch()` — Validated memblock_add

```c
void __init __weak early_init_dt_add_memory_arch(u64 base, u64 size)
{
    const u64 phys_offset = MIN_MEMBLOCK_ADDR;    // 0

    /* Page-align base (round up) and size (round down) */
    if (!PAGE_ALIGNED(base)) {
        size -= PAGE_SIZE - (base & ~PAGE_MASK);
        base = PAGE_ALIGN(base);
    }
    size &= PAGE_MASK;

    /* Clamp to valid physical address range */
    if (base > MAX_MEMBLOCK_ADDR)
        return;
    if (base + size - 1 > MAX_MEMBLOCK_ADDR)
        size = MAX_MEMBLOCK_ADDR - base + 1;
    if (base + size < phys_offset)
        return;
    if (base < phys_offset) {
        size -= phys_offset - base;
        base = phys_offset;
    }

    memblock_add(base, size);    // ★ ADD RAM TO MEMBLOCK
}
```

### 5.2 Example DT Memory Node

```dts
// Example: Board with 2GB DRAM starting at 0x4000_0000
memory@40000000 {
    device_type = "memory";
    reg = <0x0 0x40000000 0x0 0x80000000>;   // base=0x4000_0000, size=2GB
};
```

After `early_init_dt_scan_memory()`:
```
memblock.memory:
  regions[0] = { base: 0x40000000, size: 0x80000000, nid: 0 }
  total_size = 0x80000000 (2 GB)
  cnt = 1

memblock.reserved:
  (empty — reservations come next)
```

---

## 6. `arm64_memblock_init()` — Reservations and Adjustments

**Source**: `arch/arm64/mm/init.c`  
**Called by**: `setup_arch()`

This function trims the memory map and reserves regions:

```c
void __init arm64_memblock_init(void)
{
    s64 linear_region_size = PAGE_END - _PAGE_OFFSET(vabits_actual);
    //   = size of kernel linear map region
    //   48-bit VA: 0xFFFF_FFFF_FFFF - 0xFFFF_8000_0000_0000 = 128 TB

    /* ─── 1. Remove memory above physical address limit ─── */
    memblock_remove(1ULL << PHYS_MASK_SHIFT, ULLONG_MAX);
    //   PHYS_MASK_SHIFT = 48 → remove everything above 256 TB PA

    /* ─── 2. Set DRAM base address ─── */
    memstart_addr = round_down(memblock_start_of_DRAM(), ARM64_MEMSTART_ALIGN);
    //   memstart_addr = aligned start of first memblock region
    //   Example: 0x4000_0000

    /* ─── 3. Remove memory that doesn't fit in linear map ─── */
    memblock_remove(
        max_t(u64, memstart_addr + linear_region_size, __pa_symbol(_end)),
        ULLONG_MAX);
    //   Clip off anything above the linear map range

    if (memstart_addr + linear_region_size < memblock_end_of_DRAM()) {
        /* RAM extends below and above linear map — trim the bottom */
        memstart_addr = round_up(memblock_end_of_DRAM() - linear_region_size,
                                 ARM64_MEMSTART_ALIGN);
        memblock_remove(0, memstart_addr);
    }

    /* ─── 4. 52-bit VA adjustment ─── */
    if (IS_ENABLED(CONFIG_ARM64_VA_BITS_52) && (vabits_actual != 52))
        memstart_addr -= _PAGE_OFFSET(vabits_actual) - _PAGE_OFFSET(52);

    /* ─── 5. Apply mem= cmdline limit ─── */
    if (memory_limit != PHYS_ADDR_MAX) {
        memblock_mem_limit_remove_map(memory_limit);
        memblock_add(__pa_symbol(_text), (resource_size_t)(_end - _text));
    }

    /* ─── 6. Handle initrd ─── */
    if (IS_ENABLED(CONFIG_BLK_DEV_INITRD) && phys_initrd_size) {
        phys_addr_t base = phys_initrd_start & PAGE_MASK;
        resource_size_t size = PAGE_ALIGN(phys_initrd_start + phys_initrd_size) - base;

        memblock_add(base, size);
        memblock_clear_nomap(base, size);
        memblock_reserve(base, size);           // ★ Reserve initrd
    }

    /* ─── 7. Reserve kernel image ─── */
    memblock_reserve(__pa_symbol(_text), _end - _text);
    //   Reserve [PA(_text), PA(_end)) — the kernel itself
    //   Example: reserve 0x4008_0000 .. 0x4180_0000 (~24 MB kernel)

    /* ─── 8. initrd virtual addresses ─── */
    if (IS_ENABLED(CONFIG_BLK_DEV_INITRD) && phys_initrd_size) {
        initrd_start = __phys_to_virt(phys_initrd_start);
        initrd_end = initrd_start + phys_initrd_size;
    }

    /* ─── 9. DT reserved-memory ─── */
    early_init_fdt_scan_reserved_mem();
    //   Scan /reserved-memory DT nodes
    //   Each reserved-memory child → memblock_reserve()
    //   Also handles CMA regions defined in DT
}
```

After `arm64_memblock_init()`:
```
memblock.memory:
  regions[0] = { base: 0x40000000, size: 0x80000000 }   ← 2GB RAM
  total = 2 GB

memblock.reserved:
  regions[0] = { base: 0x40080000, size: 0x01780000 }   ← kernel image
  regions[1] = { base: 0x4C000000, size: 0x00800000 }   ← initrd (8MB)
  regions[2] = { base: 0x4A000000, size: 0x00080000 }   ← DTB (512KB)
  (+ any /reserved-memory nodes)
```

---

## 7. `paging_init()` — Create the Linear Map

**Source**: `arch/arm64/mm/mmu.c`  
**Called by**: `setup_arch()`

```c
void __init paging_init(void)
{
    map_mem(swapper_pg_dir);            // ★ Map ALL RAM into linear map
    memblock_allow_resize();            // Allow memblock arrays to grow
    create_idmap();                     // Rebuild identity map (refined)
    declare_kernel_vmas();              // Register kernel VA regions
}
```

### 7.1 `map_mem()` — Linear Map All RAM

```c
static void __init map_mem(pgd_t *pgdp)
{
    phys_addr_t kernel_start = __pa_symbol(_text);
    phys_addr_t kernel_end = __pa_symbol(__init_begin);
    phys_addr_t start, end;
    int flags = NO_EXEC_MAPPINGS;       // Linear map is NX (no-execute)
    u64 i;

    /* Allocate KFENCE pool if configured */
    early_kfence_pool = arm64_kfence_alloc_pool();

    if (force_pte_mapping())
        flags |= NO_BLOCK_MAPPINGS | NO_CONT_MAPPINGS;

    /* Temporarily mark kernel text as NOMAP to avoid writable alias */
    memblock_mark_nomap(kernel_start, kernel_end - kernel_start);
    //   Kernel text is already mapped via map_kernel() as ROX.
    //   We don't want a SECOND writable mapping in the linear map.

    /* Map ALL non-NOMAP memory regions */
    for_each_mem_range(i, &start, &end) {
        if (start >= end)
            break;

        __map_memblock(pgdp, start, end,
                       pgprot_tagged(PAGE_KERNEL),  // RW, NX, cacheable
                       flags);
        //   For each memblock region:
        //     VA = __phys_to_virt(start) = start - memstart_addr + PAGE_OFFSET
        //     PA = start
        //     Creates block mappings (2MB/1GB) where possible
    }

    /* Re-map kernel range into linear map as non-executable */
    __map_memblock(pgdp, kernel_start, kernel_end,
                   PAGE_KERNEL,                     // RW, NX (no tagged)
                   NO_CONT_MAPPINGS);
    memblock_clear_nomap(kernel_start, kernel_end - kernel_start);
}
```

### 7.2 `__map_memblock()` → `__create_pgd_mapping()`

```c
static void __init __map_memblock(pgd_t *pgdp, phys_addr_t start,
                                  phys_addr_t end, pgprot_t prot, int flags)
{
    early_create_pgd_mapping(pgdp,
                             start,                    // PA
                             __phys_to_virt(start),    // VA (linear map)
                             end - start,              // size
                             prot,                     // protection
                             early_pgtable_alloc,      // allocator function
                             flags);                   // NO_EXEC, block flags
}
```

The call chain for page table creation is detailed in [Doc 05](05_Page_Table_Chain.md).

After `paging_init()`:
```
swapper_pg_dir now contains:
  1. Kernel image mapping (from map_kernel, ROX/RW segments)
  2. Linear map of ALL RAM:
     VA = PA - memstart_addr + PAGE_OFFSET
     Using 2MB section mappings for aligned regions
     Using 1GB block mappings for 1GB-aligned regions
```

---

## 8. `bootmem_init()` — NUMA Topology and Zone Boundaries

**Source**: `arch/arm64/mm/init.c`  
**Called by**: `setup_arch()`

```c
void __init bootmem_init(void)
{
    unsigned long min, max;

    min = PFN_UP(memblock_start_of_DRAM());     // First usable PFN
    max = PFN_DOWN(memblock_end_of_DRAM());     // Last usable PFN
    //   PFN = Page Frame Number = PA / PAGE_SIZE
    //   Example: min = 0x40000 (PA 0x4000_0000 / 4096)
    //            max = 0xC0000 (PA 0xC000_0000 / 4096)

    early_memtest(min << PAGE_SHIFT, max << PAGE_SHIFT);
    //   Optional: test memory patterns (if "memtest" on cmdline)

    /* Set global PFN limits */
    max_pfn = max_low_pfn = max;                // ★ max PFN
    min_low_pfn = min;                          // ★ min PFN
    //   Used throughout the kernel for memory bounds

    arch_numa_init();
    //   Detect NUMA topology:
    //     1. Try DT-based NUMA (distance-map + numa node properties)
    //     2. Try ACPI SRAT table
    //     3. Fallback: single node 0 (UMA)
    //   Assigns each memblock region a NUMA node ID

    kvm_hyp_reserve();
    //   Reserve memory for KVM hypervisor (if configured)

    dma_limits_init();
    //   Compute DMA zone boundaries (see below)

    dma_contiguous_reserve(arm64_dma_phys_limit);
    //   Reserve CMA pool from memblock (default ~64 MB)
    //   memblock_reserve() + memblock_mark_nomap() for CMA

    arch_reserve_crashkernel();
    //   If "crashkernel=256M" on cmdline → memblock_reserve()

    memblock_dump_all();
    //   Print all memblock regions to dmesg (if memblock_debug or memblock=debug)
}
```

### 8.1 `dma_limits_init()` — Zone DMA Boundaries

```c
static void __init dma_limits_init(void)
{
    phys_addr_t dma32_phys_limit = max_zone_phys(DMA_BIT_MASK(32));
    //   max_zone_phys(0xFFFF_FFFF) → min(4GB, end_of_DRAM)

#ifdef CONFIG_ZONE_DMA
    /* Get DMA limit from ACPI IORT and DT */
    acpi_zone_dma_limit = acpi_iort_dma_get_max_cpu_address();
    dt_zone_dma_limit = of_dma_get_max_cpu_address(NULL);
    zone_dma_limit = min(dt_zone_dma_limit, acpi_zone_dma_limit);

    if (memblock_start_of_DRAM() < U32_MAX)
        zone_dma_limit = min(zone_dma_limit, U32_MAX);

    arm64_dma_phys_limit = max_zone_phys(zone_dma_limit);
    //   Example: zone_dma_limit = 0x3FFF_FFFF (1GB)
    //            arm64_dma_phys_limit = 0x4000_0000
#endif

#ifdef CONFIG_ZONE_DMA32
    if (!arm64_dma_phys_limit)
        arm64_dma_phys_limit = dma32_phys_limit;
#endif
    if (!arm64_dma_phys_limit)
        arm64_dma_phys_limit = PHYS_MASK + 1;
}
```

---

## State After Document 03

```
┌──────────────────────────────────────────────────────────┐
│  After setup_arch() completes:                           │
│                                                          │
│  Memory Discovery:                                       │
│    ✓ DTB parsed → memblock knows all RAM regions         │
│    ✓ memblock.memory has all physical RAM                │
│    ✓ memblock.reserved has kernel, initrd, DTB, etc.     │
│                                                          │
│  Page Tables:                                            │
│    ✓ swapper_pg_dir has kernel + linear map               │
│    ✓ All RAM accessible via __phys_to_virt() / __va()    │
│    ✓ Identity map removed from TTBR0                     │
│                                                          │
│  PFN Limits:                                             │
│    ✓ min_low_pfn, max_low_pfn, max_pfn set               │
│                                                          │
│  DMA Zone Limits:                                        │
│    ✓ arm64_dma_phys_limit computed                       │
│    ✓ Zone boundaries ready for free_area_init()           │
│                                                          │
│  Reservations Complete:                                  │
│    ✓ Kernel image reserved                               │
│    ✓ Initrd reserved                                     │
│    ✓ DTB reserved                                        │
│    ✓ CMA reserved                                        │
│    ✓ crashkernel reserved (if configured)                │
│    ✓ DT /reserved-memory nodes reserved                  │
│                                                          │
│  Allocator State:                                        │
│    ✓ memblock works (alloc + reserve)                    │
│    ✗ Buddy allocator NOT initialized                     │
│    ✗ Slab NOT initialized                                │
│    ✗ vmalloc NOT initialized                             │
│                                                          │
│  Next: mm_core_init_early() → free_area_init()           │
│        (zone, node, struct page initialization)           │
└──────────────────────────────────────────────────────────┘
```

---

[← Doc 02: Identity Map & Kernel Map](02_Identity_Map_and_Kernel_Map.md) | **Doc 03** | [Doc 04: Memblock Operations →](04_Memblock_Operations.md)
