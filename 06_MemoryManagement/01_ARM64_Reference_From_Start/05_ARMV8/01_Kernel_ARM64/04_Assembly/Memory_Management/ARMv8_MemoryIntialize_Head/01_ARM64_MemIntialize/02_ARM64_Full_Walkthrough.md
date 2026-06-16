

---

# ARMv8 (arm64) Linux Memory Subsystem Initialization — Full Walkthrough

## Phase 0: Pre-Kernel (Bootloader)

Before Linux runs, the bootloader (U-Boot, UEFI, etc.) must:
1. **Initialize physical RAM** (DRAM controller setup)
2. **Prepare a Device Tree Blob (DTB)** describing memory layout
3. **Load the kernel image** into RAM
4. **Jump to the kernel** with: MMU=off, D-cache=off, `x0` = physical address of DTB

As documented in booting.rst:
> The boot loader is expected to find and initialise all RAM that the kernel will use for volatile data storage.

---

## Phase 1: Assembly Entry — MMU Off, No Allocator (Physical Addresses Only)

### 1.1 `primary_entry` — head.S

This is the very first kernel code to execute. The CPU is running with **MMU off**, **D-cache off**, all memory accesses are physical.

```
SYM_CODE_START(primary_entry)
    bl  record_mmu_state         // Check if MMU was on (x19 records state)
    bl  preserve_boot_args       // Save x0-x3 (FDT pointer in x21)
    ...
    bl  __pi_create_init_idmap   // Create identity-mapped page tables
    bl  init_kernel_el           // Configure exception level (EL2→EL1 drop)
    bl  __cpu_setup              // Configure TCR, MAIR, cache attributes
    b   __primary_switch         // Enable MMU and switch to virtual addresses
```

**Memory type at this point:** Raw physical memory. No allocator exists. Page tables are statically allocated in BSS (`.bss` section) — `init_idmap_pg_dir`.

### 1.2 `__cpu_setup` — proc.S

Configures the MMU hardware registers while MMU is still off:

- **MAIR_EL1** — Memory Attribute Indirection Register: defines attribute encodings (Device-nGnRnE, Normal-Cacheable, Normal-Non-Cacheable, etc.)
- **TCR_EL1** — Translation Control Register: sets page granule size (4K/16K/64K), VA bits (48 or 52), IPA size, cacheability of page table walks
- Enables hardware Access Flag updates if supported (`TCR_HA`)

### 1.3 `__primary_switch` — head.S

```
SYM_FUNC_START_LOCAL(__primary_switch)
    adrp  x1, reserved_pg_dir
    adrp  x2, __pi_init_idmap_pg_dir
    bl    __enable_mmu              // TTBR0 = id map, TTBR1 = reserved, set SCTLR.M=1
    ...
    bl    __pi_early_map_kernel     // Map kernel image at its link address
    ldr   x8, =__primary_switched
    br    x8                        // Jump to virtual address!
```

### 1.4 `__enable_mmu` — head.S

- Loads **TTBR0_EL1** (identity map page tables)
- Loads **TTBR1_EL1** (kernel page tables)
- Sets **SCTLR_EL1.M=1** → **MMU is now ON**
- From this point, all addresses go through the translation tables

### 1.5 `__primary_switched` — head.S

Now running at **virtual addresses**. Sets up:
- `init_task` stack pointer (`sp`)
- `VBAR_EL1` (exception vectors)
- `kimage_voffset` (virtual-to-physical offset for kernel image)
- Calls `kasan_early_init` if KASAN enabled
- **Calls `start_kernel()`** — transitions to C code

---

## Phase 2: `start_kernel()` → `setup_arch()` — Memblock Allocator Era

### 2.1 `start_kernel()` — main.c

The master init function. Memory-relevant calls in order:

```c
start_kernel()
├── setup_arch(&command_line)         // ARM64-specific memory discovery
├── mm_core_init_early()              // Zone/node setup, sparse mem init
├── setup_per_cpu_areas()             // Per-CPU memory regions
├── mm_core_init()                    // Buddy allocator, slab, vmalloc init
```

### 2.2 `setup_arch()` — setup.c

This is where the kernel **discovers what physical memory exists**:

```c
void __init setup_arch(char **cmdline_p)
{
    setup_initial_init_mm(_text, _etext, _edata, _end);  // Record kernel boundaries
    early_fixmap_init();         // Initialize fixmap (compile-time fixed virtual addresses)
    early_ioremap_init();        // Boot-time I/O remapping
    setup_machine_fdt(__fdt_pointer);  // *** PARSE DTB FOR MEMORY ***
    ...
    arm64_memblock_init();       // *** CONFIGURE MEMBLOCK ***
    paging_init();               // *** CREATE FINAL PAGE TABLES ***
    bootmem_init();              // *** SETUP ZONES, NUMA, CMA ***
}
```

### 2.3 `setup_machine_fdt()` — setup.c

- Maps the DTB via **fixmap** (a pre-allocated virtual address range for early mapping needs)
- Calls `early_init_dt_scan()` which parses `/memory` nodes from DTB
- Each `/memory` node triggers `memblock_add(base, size)` — registering physical RAM regions in the **memblock allocator**
- Reserves the DTB itself: `memblock_reserve(dt_phys, size)`

**Memblock** is the boot-time memory allocator. It maintains two arrays:
- `memblock.memory` — all physical RAM regions
- `memblock.reserved` — regions claimed by kernel/DTB/initrd/etc.

### 2.4 `arm64_memblock_init()` — init.c

Trims and adjusts memblock based on hardware constraints:

```c
void __init arm64_memblock_init(void)
{
    // Remove memory above physical address limit
    memblock_remove(1ULL << PHYS_MASK_SHIFT, ULLONG_MAX);

    // Set memstart_addr = base of DRAM (aligned to PUD/PMD boundary)
    memstart_addr = round_down(memblock_start_of_DRAM(), ARM64_MEMSTART_ALIGN);

    // Remove memory that doesn't fit in the linear mapping (VA_BITS constraint)
    memblock_remove(max_t(u64, memstart_addr + linear_region_size, __pa(_end)), ...);

    // Handle "mem=" command line limit
    if (memory_limit != PHYS_ADDR_MAX)
        memblock_mem_limit_remove_map(memory_limit);

    // Reserve initrd
    memblock_reserve(base, size);  // for initrd

    // Reserve the kernel image itself
    memblock_reserve(__pa_symbol(_text), _end - _text);

    // Scan DTB for reserved-memory nodes
    early_init_fdt_scan_reserved_mem();
}
```

**Key concept: `memstart_addr`** — This is the physical address that corresponds to `PAGE_OFFSET` (the start of the kernel's linear/direct map). The linear map formula is: `virtual = physical - memstart_addr + PAGE_OFFSET`.

### 2.5 `paging_init()` — mmu.c

Creates the **final kernel page tables** in `swapper_pg_dir`:

```c
void __init paging_init(void)
{
    map_mem(swapper_pg_dir);       // Map all physical RAM into linear map
    memblock_allow_resize();       // Allow memblock arrays to grow
    create_idmap();                // Recreate identity map
    declare_kernel_vmas();         // Register kernel text/data as vm_struct
}
```

#### `map_mem()` — mmu.c

This creates the **linear mapping** (direct map) of all physical memory:

```c
static void __init map_mem(pgd_t *pgdp)
{
    // Temporarily mark kernel text as NOMAP to avoid writable aliases
    memblock_mark_nomap(kernel_start, kernel_end - kernel_start);

    // Map all memory banks from memblock
    for_each_mem_range(i, &start, &end) {
        __map_memblock(pgdp, start, end, pgprot_tagged(PAGE_KERNEL), flags);
    }

    // Map kernel text separately with different permissions (non-executable in linear map)
    __map_memblock(pgdp, kernel_start, kernel_end, PAGE_KERNEL, NO_CONT_MAPPINGS);
}
```

**Page table attributes used:**
| Region | Protection | Notes |
|--------|-----------|-------|
| Normal RAM | `PAGE_KERNEL` | RW, executable, cacheable, shared |
| Kernel text (linear alias) | `PAGE_KERNEL` + no-exec | Prevents execution from linear map |
| Device memory | Not mapped here | Done via `ioremap()` later |

Uses **block mappings** (2MB with 4K pages, or 1GB with PUD) for efficiency when `NO_BLOCK_MAPPINGS` is not set.

### 2.6 `bootmem_init()` — init.c

```c
void __init bootmem_init(void)
{
    min = PFN_UP(memblock_start_of_DRAM());
    max = PFN_DOWN(memblock_end_of_DRAM());

    max_pfn = max_low_pfn = max;
    min_low_pfn = min;

    arch_numa_init();              // NUMA topology from DTB/ACPI
    dma_limits_init();             // Determine DMA zone boundaries
    dma_contiguous_reserve(...);   // Reserve CMA (Contiguous Memory Allocator) regions
    arch_reserve_crashkernel();    // Reserve crashkernel memory if configured
    memblock_dump_all();           // Print final memblock state to dmesg
}
```

**DMA zone setup** — init.c:

| Zone | Address Range | Purpose |
|------|--------------|---------|
| `ZONE_DMA` | 0 to `zone_dma_limit` (often 0-1GB or 0-4GB) | For devices with limited DMA addressing (e.g., RPi4: 30-bit) |
| `ZONE_DMA32` | Up to 4GB | For 32-bit DMA-capable devices |
| `ZONE_NORMAL` | Above 4GB to `max_pfn` | General purpose memory |

---

## Phase 3: `mm_core_init_early()` — Zone & Node Data Structures

Called from main.c:

```c
void __init mm_core_init_early(void)
{
    hugetlb_cma_reserve();     // Reserve CMA for huge pages
    hugetlb_bootmem_alloc();   // Allocate huge page pools from memblock
    free_area_init();          // *** Initialize zones and struct page array ***
}
```

### `free_area_init()` — mm_init.c

This is a critical function that:

1. Calls `arch_zone_limits_init()` to get zone PFN boundaries
2. Calls `sparse_init()` — allocates `struct page` arrays (the `mem_map`) using **memblock**. With `SPARSEMEM_VMEMMAP`, these are mapped into the vmemmap region.
3. For each NUMA node, calls `free_area_init_node()` → `free_area_init_core()` which initializes:
   - `struct pglist_data` (per-node data)
   - `struct zone` for each zone (with `zone->managed_pages = 0` initially)
   - Free lists (buddy system lists) — empty at this point

**Memory used:** All `struct page` arrays and zone structures are allocated from **memblock** at this stage. No page allocator exists yet.

---

## Phase 4: `mm_core_init()` — The Big Transition

Called from main.c. This is where **all major allocators come online**:

```c
void __init mm_core_init(void)
{
    // ---- Step 1: Pre-buddy setup ----
    arch_mm_preinit();              // swiotlb init for DMA bounce buffers
    build_all_zonelists(NULL);      // Build fallback zone lists for allocation
    page_alloc_init_cpuhp();        // CPU hotplug for page allocator

    // ---- Step 2: MEMBLOCK → BUDDY TRANSITION ----
    memblock_free_all();            // *** FREE ALL UNRESERVED MEMBLOCK PAGES TO BUDDY ***
    mem_init();                     // Mark page allocator as available

    // ---- Step 3: SLAB/SLUB allocator ----
    kmem_cache_init();              // *** SLUB allocator initialization ***

    // ---- Step 4: vmalloc ----
    vmalloc_init();                 // *** Virtual memory allocator ***

    // ---- Step 5: Remaining subsystems ----
    kmemleak_init();
    pgtable_cache_init();
    mm_cache_init();
    execmem_init();
}
```

### 4.1 `memblock_free_all()` — memblock.c

**THE critical transition**: Walks all memblock.memory regions, and for every page that is NOT in `memblock.reserved`, calls `__free_pages_core()` to hand it to the **buddy allocator**:

```c
void __init memblock_free_all(void)
{
    free_unused_memmap();
    reset_all_zones_managed_pages();   // Reset managed_pages counters
    pages = free_low_memory_core_early();  // Walk memblock, free pages to buddy
    totalram_pages_add(pages);
}
```

After this call:
- The **buddy allocator** owns all free memory
- `memblock` is no longer used for new allocations
- `zone->managed_pages` reflects actual free page counts

### 4.2 `kmem_cache_init()` — slub.c

Bootstraps the SLUB slab allocator (the default on arm64):

```c
void __init kmem_cache_init(void)
{
    // Bootstrap: create caches for cache metadata itself
    kmem_cache_node = &boot_kmem_cache_node;   // Static bootstrap
    kmem_cache = &boot_kmem_cache;

    // Create the first real slab caches
    create_boot_cache(kmem_cache_node, "kmem_cache_node", ...);
    slab_state = PARTIAL;

    create_boot_cache(kmem_cache, "kmem_cache", ...);

    // Replace bootstrap caches with real slab-allocated ones
    kmem_cache = bootstrap(&boot_kmem_cache);
    kmem_cache_node = bootstrap(&boot_kmem_cache_node);

    // Create the kmalloc size-based caches (kmalloc-8, -16, ..., -8192, etc.)
    create_kmalloc_caches();
}
```

After this: `kmalloc()` / `kfree()` are functional. SLUB gets pages from the buddy allocator via `alloc_pages()`.

### 4.3 `vmalloc_init()` — vmalloc.c

Initializes the virtual memory allocator for non-contiguous kernel virtual mappings:

```c
void __init vmalloc_init(void)
{
    vmap_area_cachep = KMEM_CACHE(vmap_area, SLAB_PANIC);  // Slab cache for VMAs
    vmap_init_nodes();           // Per-node vmap management
    // Import early vmlist entries (kernel text/data VMAs from declare_kernel_vmas)
    vmap_init_free_space();      // Initialize free VA space tracking
    vmap_initialized = true;
}
```

After this: `vmalloc()`, `vmap()`, `ioremap()` are functional.

---

## Summary: Allocator Timeline

| Phase | Allocator Available | Backing Source | Functions |
|-------|-------------------|----------------|-----------|
| **Assembly boot** (MMU off) | **None** | Static BSS page tables | — |
| **MMU on, pre-setup_arch** | **Fixmap** only | Compile-time VA slots | `set_fixmap()` |
| **setup_arch → mm_core_init** | **Memblock** | Physical RAM tracked by DTB | `memblock_alloc()`, `memblock_reserve()` |
| **After `memblock_free_all()`** | **Buddy (page allocator)** | Freed memblock pages | `alloc_pages()`, `__get_free_pages()` |
| **After `kmem_cache_init()`** | **SLUB slab allocator** | Buddy allocator pages | `kmalloc()`, `kmem_cache_alloc()` |
| **After `vmalloc_init()`** | **vmalloc** | Buddy pages + kernel page tables | `vmalloc()`, `ioremap()`, `vmap()` |

## Address Space Layout (typical 48-bit VA, 4K pages)

```
0xFFFF_FFFF_FFFF_FFFF  ┐
                        │ Fixmap, FDT, early console
0xFFFF_FFFE_0000_0000  ┤
                        │ PCI I/O space
0xFFFF_FFFD_0000_0000  ┤
                        │ vmemmap (struct page array)
0xFFFF_FFFC_0000_0000  ┤
                        │ vmalloc / ioremap space
0xFFFF_FF80_0800_0000  ┤
                        │ Kernel image (text, data, BSS)
0xFFFF_FF80_0000_0000  ┤
                        │ Linear map (PAGE_OFFSET → all physical RAM)
0xFFFF_0000_0000_0000  ┘ PAGE_OFFSET
```

## Key Source Files for Reference

| File | Purpose |
|------|---------|
| head.S | Assembly entry, MMU enable |
| proc.S | TCR/MAIR/SCTLR setup |
| setup.c | `setup_arch()` — arch init |
| init.c | `arm64_memblock_init()`, `bootmem_init()`, `mem_init()` |
| mmu.c | `paging_init()`, `map_mem()` — page table creation |
| memblock.c | Memblock allocator, `memblock_free_all()` |
| mm_init.c | `free_area_init()`, `mm_core_init()` |
| slub.c | SLUB slab allocator init |
| vmalloc.c | vmalloc subsystem init |
| main.c | `start_kernel()` — orchestrator |
