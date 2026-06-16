# Boot Memory Setup: Complete Reference

**Category**: KASLR and Kernel Boot Memory Setup  
**Platform**: ARM64 (AArch64)

---

## 1. ARM64 Boot Sequence Summary

```
ARM64 kernel boot flow (4KB pages, 48-bit VA, UEFI boot):

[Reset Vector / EFI Stub Entry]
        │
        ▼
arch/arm64/kernel/head.S:
  _head: kernel Image header (PE/COFF for UEFI)
  el2_setup: if at EL2, configure HCR_EL2, enter EL1
  set_cpu_boot_mode_flag: save exception level
  
        │
        ▼
__create_page_tables: (still MMU off)
  build early identity map (idmap_pg_dir): VA=PA for MMU enable code
  build early kernel map (init_pg_dir): VA=0xFFFF_8001_0000_0000+
  
        │
        ▼
__cpu_setup:
  MAIR_EL1 = memory attribute table
  TCR_EL1  = VA size, granule, cacheability, ASID size
  SCTLR_EL1 |= SCTLR_ELx_M (enable MMU)
  ISB
  → CPU now uses virtual addresses via init_pg_dir
  
        │
        ▼
__primary_switch: (now with MMU on, but identity map)
  Load TTBR1_EL1 = init_pg_dir (kernel VA range)
  Load TTBR0_EL1 = reserved_pg_dir (no user mapping yet)
  branch to start_kernel() in virtual address space
  
        │
        ▼
start_kernel():
  setup_arch():
    arm64_memblock_init():
      early_init_dt_scan(): parse device tree → memory, reserved-memory
      efi_init(): parse EFI memory map (if UEFI)
      memblock_add() all RAM regions
      memblock_reserve() kernel, DTB, initrd, EFI tables, TZASC regions
    
    paging_init():
      map_mem(): map all RAM in swapper_pg_dir (TTBR1)
      pgd/pud/pmd allocation from memblock
      1GB, 2MB, or 4KB mappings (based on alignment)
      load swapper_pg_dir: MSR TTBR1_EL1, x0
      
    setup_machine_fdt():
      unflatten_device_tree(): convert DTB to in-memory tree
      
  mm_init():
    sparse_init(): allocate vmemmap (struct page array)
    zone_sizes_init(): set up ZONE_DMA32, ZONE_NORMAL, ZONE_MOVABLE
    memblock_free_all(): donate all free memblock memory to buddy
    
  Then: slab, vmalloc, vmap etc. become available
```

---

## 2. fixmap Virtual Addresses

```
fixmap: compile-time allocated virtual addresses for early use

Purpose: some virtual addresses needed before vmalloc is ready
  Examples: early console (UART), DTB access, KPTI trampoline, APIC equivalent

ARM64 fixmap layout:
  FIXADDR_TOP:    0xFFFFFF7FFBFFF000 (just below vmalloc area)
  FIXADDR_START:  FIXADDR_TOP - (FIXMAP_NR × PAGE_SIZE)
  
  Each fixmap slot: one page (4KB), compile-time constant virtual address
  
  Fixed mapping indices (enum fixed_addresses):
    FIX_FDT_END / FIX_FDT:   DTB (device tree blob) early mapping
    FIX_EARLYCON_MEM_BASE:    early console UART register map
    FIX_BTMAP_END:            early ioremap area (32 slots × 256KB each)
    FIX_BTMAP_BEGIN:          (for temporary early ioremap)
    FIX_KPTI_TRAMP:           KPTI trampoline page
  
  Using fixmap:
    set_fixmap(idx, phys_addr): map fixmap slot idx to physical address
    set_fixmap_io(idx, phys_addr): NC (non-cacheable) mapping
    clear_fixmap(idx): unmap
    fix_to_virt(idx): returns virtual address of slot idx
    virt_to_fix(vaddr): returns index for virtual address
  
  Early ioremap (early_ioremap/early_memremap):
    Uses fixmap BTMAP slots (temporary, limited life)
    For: accessing DTB, ACPI tables before full ioremap is ready
    early_ioremap_init(): sets up the BTMAP slot management
    early_memremap(phys, size): map up to 256KB at a time
    early_memunmap(vaddr, size): release
    
    Example: DTB early mapping:
      fdt = early_memremap(params->dtb_base, total_size)
      // parse DTB
      early_memunmap(fdt, total_size)
```

---

## 3. Linear Map and vmemmap

```
Linear map (direct map): ALL physical memory mapped at fixed offset

ARM64 linear map:
  BASE: PAGE_OFFSET = 0xFFFF000000000000 (with 48-bit VA, 4KB pages)
  All physical RAM: contiguous mapping from PAGE_OFFSET
  
  Physical address PA:
    VA (in linear map) = PA + PAGE_OFFSET - PHYS_OFFSET
    = PA + PAGE_OFFSET (when PHYS_OFFSET=0, common case)
  
  __pa(vaddr): vaddr - PAGE_OFFSET + PHYS_OFFSET = physical address
  __va(paddr): paddr + PAGE_OFFSET - PHYS_OFFSET = virtual address
  
  Linear map creation (map_mem()):
    For each memblock.memory region:
      Call: __map_memblock(pgdp, start, end, prot, flags)
      Try 1GB blocks (PUD level) if aligned + large enough
      Try 2MB blocks (PMD level) if aligned
      Fall back to 4KB pages (PTE level)
    Result: swapper_pg_dir covers all RAM
  
  Linear map attributes:
    Normal RAM: MT_NORMAL (write-back, inner+outer sharable)
    Memory-mapped IO: NOT in linear map (use ioremap())
    NOMAP regions: excluded from linear map (OP-TEE, CMA, etc.)

vmemmap (virtual memory map):
  Problem: struct page is 64 bytes per page. For 512GB RAM: 8GB of struct pages
  vmemmap: contiguous virtual array of ALL struct page objects
  
  ARM64 vmemmap:
    vmemmap_base: 0xFFFFC00000000000 (typical)
    vmemmap[pfn]: struct page for physical page frame pfn
    virt_to_page(addr): → pfn_to_page(virt_to_pfn(addr))
                        → vmemmap + pfn (directly indexed array!)
    page_to_virt(page): → pfn_to_virt(page_to_pfn(page))
  
  sparse_vmemmap_alloc_and_verify():
    Allocates struct page backing for each memory section
    Maps into vmemmap_base + (pfn × sizeof(struct page))
    Only allocates for populated memory (not holes)
    Holes in physical memory: no vmemmap allocation
  
  vmemmap_populate():
    Called by sparse_init() → sparse_init_one_section()
    Allocates: one physical page of struct pages at a time
    Maps: into vmemmap virtual range
    Uses: memblock_alloc() during early boot
```

---

## 4. Top 10 Interview Questions

**Q1: What is the purpose of the ARM64 identity map (idmap)?**
The identity map creates VA=PA mappings for the code that enables/disables the MMU. When `SCTLR_EL1.M=1` is set (MMU enable), the very next PC-relative fetch uses the new page tables. If those tables don't map the running code, instant prefetch abort. `idmap_pg_dir` maps the `cpu_idmap` section at its physical address. After MMU enable, the code immediately branches to the real kernel virtual address. `idmap_pg_dir` is also used for CPU hotplug (secondary CPUs enter via the identity map).

**Q2: How does arm64_memblock_init() discover available RAM?**
1. Parse `/memory` device tree nodes via `early_init_dt_scan_memory()` → `memblock_add()`. 2. If UEFI boot: parse EFI memory map → add `EfiConventionalMemory` + `EfiLoaderCode/Data` (free after ExitBootServices). 3. Reserve: kernel text/data (already loaded), DTB, initrd, EFI tables, ACPI tables. 4. Process `/reserved-memory` DT nodes → `memblock_reserve()` or `memblock_mark_nomap()`. 5. TZASC-protected regions: TF-A passes them as `no-map` reserved DT nodes → excluded from Linux.

**Q3: What is the difference between NOMAP and reserved memory regions?**
`memblock_reserve()`: region is tracked as "reserved" but still MAPPED in the linear map. Kernel cannot allocate from it via buddy/slab, but can access it via `__va()`. Example: kernel text/data — reserved so buddy won't reuse those pages. `memblock_mark_nomap()`: region is excluded from the linear map entirely. Cannot access via `__va()`. Example: OP-TEE memory (Secure world), device firmware tables, CMA. Access to NOMAP regions requires explicit `ioremap()` or DMA map.

**Q4: How does the kernel switch from init_pg_dir to swapper_pg_dir?**
1. `head.S` creates `init_pg_dir` (minimal: just enough to map kernel text). 2. MMU enabled with `init_pg_dir` in `TTBR1_EL1`. 3. `start_kernel()` → `setup_arch()` → `paging_init()`. 4. `paging_init()` builds complete `swapper_pg_dir`: maps all RAM from memblock. 5. `cpu_replace_ttbr1(lm_alias(swapper_pg_dir))`: atomically switches `TTBR1_EL1`. 6. `ISB + DSB`: ensure transition is complete. 7. Free `init_pg_dir` pages back to buddy (they are no longer needed).

**Q5: How does KASLR affect the linear map and vmemmap?**
KASLR randomizes only the kernel text/data virtual address. The linear map base (`PAGE_OFFSET`) and vmemmap base are fixed per-build but also randomized in some configurations. ARM64 linear map: `PAGE_OFFSET` itself is not randomized in standard KASLR (only kimage_vaddr). However, some hardened configurations also randomize `PAGE_OFFSET`. vmemmap base: fixed relative to linear map. KASLR entropy focuses on kernel text because that's where gadgets are — randomizing stack layout, heap base (ASLR) provides additional protection for other regions.

**Q6: What is early_ioremap and why is it needed?**
`early_ioremap()` provides temporary virtual mappings for physical addresses before `ioremap()` is ready (before `vmalloc` area is set up). Uses fixmap BTMAP slots (32 slots, ~256KB each). Typical use: map the DTB (`early_memremap`), ACPI tables, UART during early console setup. Lifetime: temporary — must call `early_memunmap()` after use (slot recycled). Limitations: maximum 256KB per mapping, limited number of simultaneous mappings. Replaced by full `ioremap()` once the vmalloc area is initialized.

**Q7: How does the kernel handle physical memory gaps (holes)?**
`memblock`: only adds actual memory ranges (from DT or EFI). Holes between ranges are simply not in `memblock.memory`. During vmemmap setup: `sparse_init()` only allocates `struct page` objects for populated memory sections (64MB sections). Holes don't get `struct page` backing — no vmemmap allocation. In buddy allocator: `zone_sizes_init()` marks absent pages as holes in the zone's `present_pages`. `pfn_valid(pfn)` returns false for holes: kernel won't dereference `vmemmap[pfn]` for invalid pfns.

**Q8: What is the fixmap FIX_BTMAP area and when is it used?**
`FIX_BTMAP` (Boot-Time Map) is a set of fixmap slots reserved for early `ioremap`. Slots: 32 entries × 256KB = 8MB total virtual space. `early_ioremap_setup()`: initializes the BTMAP area management. When `early_ioremap(phys, size)` is called: finds a free BTMAP slot, calls `set_fixmap_io(slot, phys+offset)` for each page, returns virtual address. After use: `early_memunmap()` removes the mappings. This provides temporary I/O access before `vmalloc` is ready. Example: UART for early console, DTB parsing.

---

## 5. Quick Reference

| Region | Virtual Base | Content |
|---|---|---|
| Linear map | PAGE_OFFSET | All physical RAM |
| vmemmap | vmemmap_base | Array of struct page |
| vmalloc | VMALLOC_START | vmalloc/ioremap/modules |
| fixmap | FIXADDR_START | Early fixed-VA mappings |
| Kernel text | ~0xFFFF8001... + KASLR | Kernel code/data/BSS |

| Boot Allocator | When Active | What It Does |
|---|---|---|
| memblock | Before mm_init | Track physical memory |
| page_alloc (buddy) | After memblock_free_all | Allocate pages |
| slab/slub | After slab_init | Allocate small objects |
| vmalloc | After paging_init | Non-contiguous VA alloc |
