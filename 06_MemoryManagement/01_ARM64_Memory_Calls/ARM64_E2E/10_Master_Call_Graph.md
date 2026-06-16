# 10 — Master Call Graph: Complete Function Tree with Source References

[← Doc 09: vmalloc & Completion](09_vmalloc_and_Completion.md) | **Doc 10** | [Back to Index →](00_Index.md)

---

## Complete Call Tree: Boot to Memory Subsystem Operational

Every function in the memory initialization path, with source file and approximate
line numbers (Linux ~6.x mainline). Indentation shows call depth.

```
_text                                               arch/arm64/kernel/head.S
  b  primary_entry
│
primary_entry                                       arch/arm64/kernel/head.S
├── bl  record_mmu_state                            arch/arm64/kernel/head.S
│       → x19 = SCTLR_ELx_M or 0
├── bl  preserve_boot_args                          arch/arm64/kernel/head.S
│       → x21 = FDT PA, boot_args[] filled
│       └── dcache_inval_poc                        arch/arm64/kernel/cache.S
├── (set SP = early_init_stack)
├── bl  __pi_create_init_idmap                      arch/arm64/kernel/pi/map_kernel.c
│   ├── map_range(text, _stext→__initdata_begin)   arch/arm64/kernel/pi/map_range.c
│   │   └── (recursive PGD→PUD→PMD→PTE descent)
│   └── map_range(data, __initdata_begin→_end)      arch/arm64/kernel/pi/map_range.c
├── dcache_inval_poc / dcache_clean_poc             arch/arm64/kernel/cache.S
├── bl  init_kernel_el                              arch/arm64/kernel/head.S
│       → x20 = BOOT_CPU_MODE_EL1/EL2
│       └── (EL2→EL1 transition via eret)
├── bl  __cpu_setup                                 arch/arm64/mm/proc.S
│       → MAIR_EL1, TCR_EL1 configured
│       → x0 = INIT_SCTLR_EL1_MMU_ON
└── b   __primary_switch                            arch/arm64/kernel/head.S
    ├── bl  __enable_mmu                            arch/arm64/kernel/head.S
    │       msr ttbr0_el1, idmap_pg_dir
    │       msr ttbr1_el1, reserved_pg_dir
    │       msr sctlr_el1, x0          ← ★ MMU ON
    ├── (set SP = early_init_stack)
    ├── bl  __pi_early_map_kernel                   arch/arm64/kernel/pi/map_kernel.c
    │   ├── map_fdt()                               arch/arm64/kernel/pi/map_kernel.c
    │   ├── memset(__bss_start, 0, ...)
    │   ├── init_feature_override()
    │   ├── kaslr_early_init()                      arch/arm64/kernel/pi/kaslr_early.c
    │   └── map_kernel()                            arch/arm64/kernel/pi/map_kernel.c
    │       ├── map_segment(text)                   (6 segments via map_range)
    │       ├── map_segment(rodata)
    │       ├── map_segment(init text)
    │       ├── map_segment(init data)
    │       ├── map_segment(data+BSS)
    │       ├── idmap_cpu_replace_ttbr1(init_pg_dir)
    │       ├── relocate_kernel()                   (if KASLR)
    │       ├── memcpy → swapper_pg_dir
    │       └── idmap_cpu_replace_ttbr1(swapper_pg_dir)  ← ★ FINAL TTBR1
    └── br  __primary_switched (at kernel VA)       arch/arm64/kernel/head.S
        ├── init_cpu_task(init_task)
        │       current = &init_task, SP = kernel stack
        ├── msr vbar_el1, vectors
        ├── str __fdt_pointer                       (FDT PA saved)
        ├── str kimage_voffset                      (VA-PA offset saved)
        ├── bl  set_cpu_boot_mode_flag
        ├── bl  kasan_early_init                    (if CONFIG_KASAN)
        ├── bl  finalise_el2
        └── bl  start_kernel                        init/main.c:1017
```

---

## `start_kernel()` Call Tree (Memory-Related Paths)

```
start_kernel()                                      init/main.c:1017
├── setup_arch(&command_line)                       arch/arm64/kernel/setup.c:281
│   ├── setup_initial_init_mm(_text, _etext, _edata, _end)
│   ├── kaslr_init()
│   ├── early_fixmap_init()                         arch/arm64/mm/fixmap.c
│   ├── early_ioremap_init()                        mm/early_ioremap.c
│   ├── setup_machine_fdt(__fdt_pointer)            arch/arm64/kernel/setup.c
│   │   ├── fixmap_remap_fdt()                      arch/arm64/mm/mmu.c
│   │   └── early_init_dt_scan()                    drivers/of/fdt.c
│   │       ├── early_init_dt_scan_chosen()         drivers/of/fdt.c
│   │       └── early_init_dt_scan_memory()         drivers/of/fdt.c:1049
│   │           └── early_init_dt_add_memory_arch() drivers/of/fdt.c:1170
│   │               └── memblock_add(base, size)    mm/memblock.c
│   │                   └── memblock_add_range()    mm/memblock.c:609
│   │                       ├── memblock_insert_region()
│   │                       └── memblock_merge_regions()
│   ├── cpu_uninstall_idmap()
│   ├── efi_init()                                  (if UEFI)
│   ├── arm64_memblock_init()                       arch/arm64/mm/init.c:193
│   │   ├── memblock_remove(1ULL<<48, MAX)          (clip PA range)
│   │   ├── memblock_reserve(kernel_pa, size)       (reserve kernel)
│   │   ├── memblock_reserve(initrd_pa, size)       (reserve initrd)
│   │   └── early_init_fdt_scan_reserved_mem()      (DT reserved-memory)
│   │       └── memblock_reserve() × N
│   ├── paging_init()                               arch/arm64/mm/mmu.c:1420
│   │   ├── map_mem(swapper_pg_dir)                 arch/arm64/mm/mmu.c:1136
│   │   │   ├── memblock_mark_nomap(kernel_text)
│   │   │   ├── for_each_mem_range → __map_memblock()  arch/arm64/mm/mmu.c:1038
│   │   │   │   └── __create_pgd_mapping()          arch/arm64/mm/mmu.c:493
│   │   │   │       └── alloc_init_pud()            arch/arm64/mm/mmu.c:343
│   │   │   │           ├── pgtable_alloc()         → memblock_phys_alloc()
│   │   │   │           ├── (1GB block if aligned)
│   │   │   │           └── alloc_init_cont_pmd()
│   │   │   │               └── init_pmd()          arch/arm64/mm/mmu.c:246
│   │   │   │                   ├── (2MB section if aligned)
│   │   │   │                   └── alloc_init_cont_pte()
│   │   │   │                       └── init_pte()  arch/arm64/mm/mmu.c:167
│   │   │   │                           └── set_pte(pfn_pte(phys, prot))
│   │   │   └── memblock_clear_nomap(kernel_text)
│   │   ├── memblock_allow_resize()
│   │   ├── create_idmap()
│   │   └── declare_kernel_vmas()
│   └── bootmem_init()                              arch/arm64/mm/init.c:300
│       ├── min = PFN_UP(memblock_start_of_DRAM())
│       ├── max = PFN_DOWN(memblock_end_of_DRAM())
│       ├── arch_numa_init()
│       ├── dma_limits_init()                       arch/arm64/mm/init.c:135
│       │   └── max_zone_phys()                     arch/arm64/mm/init.c:121
│       ├── dma_contiguous_reserve()                (CMA reservation)
│       └── memblock_dump_all()
│
├── mm_core_init_early()                            mm/mm_init.c:2710
│   └── free_area_init()                            mm/mm_init.c:1831
│       ├── arch_zone_limits_init(max_zone_pfns)
│       ├── free_area_init_max_zone_pfns()
│       ├── sparse_init()                           mm/sparse.c
│       │   └── sparse_init_nid()
│       │       └── vmemmap_populate()              (map struct page arrays)
│       │           └── memblock_alloc() for backing pages
│       └── for_each_node → free_area_init_node()   mm/mm_init.c:1725
│           ├── get_pfn_range_for_nid()
│           ├── calculate_node_totalpages()
│           └── free_area_init_core()               mm/mm_init.c:1604
│               ├── calc_memmap_size()
│               ├── setup_usemap()                  → memblock_alloc()
│               ├── zone_init_internals()           mm/mm_init.c:1432
│               ├── zone_init_free_lists()          mm/mm_init.c:1444
│               │   └── INIT_LIST_HEAD(free_list[order][migratetype])
│               ├── memmap_init()                   mm/mm_init.c:977
│               │   └── memmap_init_range()
│               │       └── __init_single_page()
│               │           ├── set_page_zone()
│               │           ├── set_page_node()
│               │           ├── init_page_count()   → _refcount = 1
│               │           └── SetPageReserved()   → PG_reserved SET
│               └── pgdat_init_internals()
│
├── mm_core_init()                                  mm/mm_init.c:2721
│   ├── build_all_zonelists(NULL)                   mm/page_alloc.c
│   ├── page_alloc_init_cpuhp()
│   ├── kmem_cache_init()                           mm/slub.c:8460
│   │   ├── create_boot_cache(kmem_cache, ...)      mm/slab_common.c:695
│   │   │   └── __kmem_cache_create()               mm/slub.c
│   │   │       └── kmem_cache_open()
│   │   ├── create_boot_cache(kmem_cache_node, ...)
│   │   ├── slab_state = PARTIAL
│   │   ├── bootstrap(&boot_kmem_cache)             mm/slub.c:8369
│   │   │   ├── kmem_cache_zalloc(kmem_cache, ...)  → first slab alloc!
│   │   │   ├── memcpy(dynamic, static, ...)
│   │   │   └── fix slab_cache pointers
│   │   ├── bootstrap(&boot_kmem_cache_node)
│   │   └── create_kmalloc_caches()                 mm/slab_common.c:994
│   │       ├── new_kmalloc_cache(size) × 13
│   │       └── slab_state = UP                     ← ★ KMALLOC WORKS
│   ├── vmalloc_init()                              mm/vmalloc.c:5425
│   │   ├── init per-CPU vfree_deferred
│   │   ├── register vmlist entries (boot ioremaps)
│   │   ├── vmap_init_free_space()
│   │   └── vmap_initialized = true                 ← ★ VMALLOC WORKS
│   ├── page_alloc_init()
│   └── execmem_init()
│
└── (later, kernel_init_freeable):
    └── page_alloc_init_late()                      mm/mm_init.c:2323
        ├── set_pageblock_migratetype_for_cma()
        ├── struct_pages_for_free_area_init()
        └── memblock_free_all()                     mm/memblock.c:2355
            ├── reset_all_zones_managed_pages()
            ├── memmap_init_reserved_pages()
            └── free_low_memory_core_early()        mm/memblock.c:2308
                └── for_each_free_mem_range →
                    __free_memory_core()             mm/memblock.c:2241
                    └── __free_pages_memory()        mm/memblock.c:2215
                        └── memblock_free_pages()    mm/mm_init.c:2492
                            └── __free_pages_core()  mm/mm_init.c:1589
                                ├── ClearPageReserved()
                                ├── set_page_count(0)
                                ├── adjust_managed_page_count()
                                └── __free_pages()
                                    └── free_one_page()      mm/page_alloc.c:1543
                                        └── __free_one_page()  mm/page_alloc.c:944
                                            ├── __find_buddy_pfn()  (XOR)
                                            ├── page_is_buddy()
                                            ├── __del_page_from_free_list()
                                            └── __add_to_free_list()  mm/page_alloc.c:798
```

---

## Allocator Transition Diagram

```
                    memblock era                    buddy era
                ◄──────────────────►    ◄────────────────────────►

Allocation:     memblock_alloc()        alloc_pages()
                    │                       │
                    ▼                       ▼
                memblock_find_in_range  rmqueue_buddy()
                memblock_reserve()      __rmqueue_smallest()
                                        expand() (split)

Object alloc:   (not available)         kmalloc() / kmem_cache_alloc()
                                            │
                                            ▼
                                        slab_alloc_node()
                                        alloc_from_pcs()     ← per-CPU fast
                                        ___slab_alloc()      ← slow path
                                        allocate_slab()      ← new slab from buddy

Virtual alloc:  (not available)         vmalloc()
                                            │
                                            ▼
                                        __vmalloc_node_range()
                                        __get_vm_area_node()  ← VA allocation
                                        __vmalloc_area_node() ← page alloc + map

Transition:     ────────── memblock_free_all() ──────────►
                           __free_pages_core()
                           __free_one_page() (buddy merge)
```

---

## Quick Reference: Key Source Files

| File | Content |
|------|---------|
| `arch/arm64/kernel/head.S` | Boot entry, MMU enable, primary_switched |
| `arch/arm64/mm/proc.S` | __cpu_setup (MAIR, TCR, SCTLR) |
| `arch/arm64/kernel/pi/map_range.c` | map_range() recursive page table builder |
| `arch/arm64/kernel/pi/map_kernel.c` | create_init_idmap, early_map_kernel, map_kernel |
| `arch/arm64/kernel/setup.c` | setup_arch, setup_machine_fdt |
| `arch/arm64/mm/init.c` | arm64_memblock_init, bootmem_init |
| `arch/arm64/mm/mmu.c` | paging_init, map_mem, __create_pgd_mapping chain |
| `drivers/of/fdt.c` | early_init_dt_scan_memory, DT parsing |
| `mm/memblock.c` | memblock_add/reserve/alloc/free_all |
| `mm/mm_init.c` | free_area_init, __free_pages_core |
| `mm/page_alloc.c` | __free_one_page, __rmqueue_smallest, alloc_pages |
| `mm/slub.c` | kmem_cache_init, bootstrap, slab_alloc_node |
| `mm/slab_common.c` | create_boot_cache, create_kmalloc_caches |
| `mm/vmalloc.c` | vmalloc_init, __vmalloc_node_range |
| `init/main.c` | start_kernel, mm_core_init, page_alloc_init_late |

---

[← Doc 09: vmalloc & Completion](09_vmalloc_and_Completion.md) | **Doc 10** | [Back to Index →](00_Index.md)
