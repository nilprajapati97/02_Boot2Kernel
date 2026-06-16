# ARM64 Memory Subsystem Initialization — Kernel Code Walkthrough

## What This Is

A **line-by-line code trace** through the Linux kernel source, following every function
call from the moment the bootloader jumps to the kernel image through the complete
initialization of the memory management subsystem. Every function signature, every
argument, every return value is shown as it appears in the actual source.

> **Difference from ARM64_Boot_Memory/**: Those documents explain *concepts*.
> These documents show the *exact code path* with real function prototypes, register
> values, and data flow — a guided walk through the kernel source tree.

---

## Document Map

```
ARM64_MemInit_CodeWalk/
│
├── 00_Index.md                ← YOU ARE HERE
│
├── 01_Image_Entry_head_S.md   ← First instruction → MMU enable
│   Traces: _text, primary_entry, preserve_boot_args,
│   init_kernel_el, create_init_idmap, __cpu_setup,
│   __enable_mmu, __primary_switch, __primary_switched
│
├── 02_Identity_Map_and_Kernel_Map.md  ← Page table construction in assembly context
│   Traces: map_range(), create_init_idmap(),
│   early_map_kernel(), map_segment(), map_kernel()
│
├── 03_start_kernel_to_setup_arch.md   ← C entry → arch-specific setup
│   Traces: start_kernel(), setup_arch(),
│   setup_machine_fdt(), early_init_dt_scan_memory(),
│   arm64_memblock_init(), paging_init()
│
├── 04_Memblock_Operations.md  ← The early allocator internals
│   Traces: memblock_add_range(), memblock_reserve(),
│   memblock_alloc_range_nid(), memblock_find_in_range_node()
│
├── 05_Page_Table_Chain.md     ← Full __create_pgd_mapping → PTE chain
│   Traces: __create_pgd_mapping(), alloc_init_p4d/pud/pmd/pte(),
│   map_mem(), block vs page mapping decisions
│
├── 06_Zone_Node_Page_Init.md  ← free_area_init code path
│   Traces: bootmem_init(), free_area_init(),
│   free_area_init_node(), free_area_init_core(),
│   zone_init_free_lists(), memmap_init(), __init_single_page()
│
├── 07_Buddy_Handoff_Code.md   ← memblock_free_all → buddy merging
│   Traces: memblock_free_all(), free_low_memory_core_early(),
│   __free_pages_core(), __free_one_page() XOR buddy merge,
│   alloc_pages() → rmqueue → __rmqueue_smallest → expand()
│
├── 08_SLUB_Bootstrap_Code.md  ← Slab allocator self-bootstrap
│   Traces: kmem_cache_init(), create_boot_cache(),
│   bootstrap(), create_kmalloc_caches(),
│   slab_alloc_node() fast/slow path, allocate_slab()
│
├── 09_vmalloc_and_Completion.md ← vmalloc init + final state
│   Traces: vmalloc_init(), __vmalloc_node_range(),
│   page_alloc_init_late(), memblock_discard(),
│   complete end-to-end call graph
│
└── 10_Master_Call_Graph.md    ← Complete function call tree with file:line
```

---

## How to Read These Documents

Each document follows a consistent format:

```
┌─────────────────────────────────────────────────────────┐
│  FUNCTION NAME                                          │
│  Source: file path : line number                         │
│                                                         │
│  Prototype:                                              │
│    return_type function_name(arg1_type arg1, ...)        │
│                                                         │
│  Called by: parent_function()                            │
│  Calls: child1(), child2(), ...                         │
│                                                         │
│  Arguments at call site:                                 │
│    arg1 = <actual value at this point in execution>     │
│    arg2 = <actual value>                                │
│                                                         │
│  Code: (key excerpt from actual source)                  │
│                                                         │
│  What happens:                                           │
│    Step-by-step description of the logic                │
│                                                         │
│  Returns: <what the function returns>                    │
│  Side effects: <global state changes>                    │
└─────────────────────────────────────────────────────────┘
```

---

## Complete Execution Flow (Summary)

```
BOOTLOADER
  │  x0 = FDT phys addr
  ▼
_text → primary_entry                          [arch/arm64/kernel/head.S]
  ├── record_mmu_state()                       → x19 = MMU state
  ├── preserve_boot_args()                     → x21 = FDT, boot_args[] saved
  ├── create_init_idmap()                      → identity map page tables
  │     └── map_range() × 2                    [arch/arm64/kernel/pi/map_range.c]
  ├── init_kernel_el()                         → x20 = EL1/EL2, drop to EL1
  ├── __cpu_setup()                            → MAIR, TCR configured; x0 = SCTLR
  │                                            [arch/arm64/mm/proc.S]
  └── __primary_switch()
        ├── __enable_mmu()                     ★ MMU ON (TTBR0=idmap, TTBR1=reserved)
        ├── early_map_kernel()                 [arch/arm64/kernel/pi/map_kernel.c]
        │     ├── map_fdt()
        │     ├── init_feature_override()
        │     ├── map_kernel()                 → 6 segments into init_pg_dir
        │     │     └── map_segment() × 6
        │     │           └── map_range()      (recursive)
        │     └── TTBR1 → swapper_pg_dir
        └── __primary_switched()               ★ Running at kernel VA
              ├── init_cpu_task(init_task)      → SP = kernel stack
              ├── __fdt_pointer = x21
              ├── kimage_voffset = VA-PA
              └── start_kernel()               ★ ENTER C CODE
                    │                          [init/main.c]
                    ├── setup_arch()           [arch/arm64/kernel/setup.c]
                    │     ├── early_fixmap_init()
                    │     ├── setup_machine_fdt(__fdt_pointer)
                    │     │     └── early_init_dt_scan_memory()
                    │     │           └── memblock_add(base, size) × N
                    │     ├── arm64_memblock_init()
                    │     │     ├── memblock_remove(above PA range)
                    │     │     ├── memblock_reserve(kernel image)
                    │     │     ├── memblock_reserve(initrd)
                    │     │     └── early_init_fdt_scan_reserved_mem()
                    │     ├── paging_init()
                    │     │     ├── map_mem(swapper_pg_dir)
                    │     │     │     └── __map_memblock() per region
                    │     │     │           └── __create_pgd_mapping()
                    │     │     │                 └── alloc_init_p4d/pud/pmd/pte()
                    │     │     ├── create_idmap()
                    │     │     └── declare_kernel_vmas()
                    │     └── bootmem_init()
                    │           ├── arch_numa_init()
                    │           ├── dma_limits_init()
                    │           └── dma_contiguous_reserve()
                    │
                    ├── mm_core_init_early()    [mm/mm_init.c]
                    │     └── free_area_init()
                    │           ├── arch_zone_limits_init()
                    │           ├── sparse_init()
                    │           ├── free_area_init_node() per node
                    │           │     ├── calculate_node_totalpages()
                    │           │     └── free_area_init_core()
                    │           │           └── zone_init_free_lists()
                    │           └── memmap_init()
                    │                 └── __init_single_page() per PFN
                    │
                    ├── mm_core_init()          [mm/mm_init.c]
                    │     ├── build_all_zonelists()
                    │     ├── memblock_free_all()    ★ THE HANDOFF
                    │     │     ├── reset_all_zones_managed_pages()
                    │     │     ├── free_low_memory_core_early()
                    │     │     │     └── __free_pages_core() per range
                    │     │     │           └── __free_one_page() buddy merge
                    │     │     └── totalram_pages_add()
                    │     ├── kmem_cache_init()      ★ SLUB BOOTSTRAP
                    │     │     ├── create_boot_cache(kmem_cache_node)
                    │     │     ├── slab_state = PARTIAL
                    │     │     ├── create_boot_cache(kmem_cache)
                    │     │     ├── bootstrap() × 2
                    │     │     ├── create_kmalloc_caches()
                    │     │     └── slab_state = UP
                    │     └── vmalloc_init()         ★ VMALLOC READY
                    │
                    ├── kmem_cache_init_late()  → slab_state = FULL
                    └── page_alloc_init_late()
                          ├── deferred struct page init
                          ├── memblock_discard()     ★ MEMBLOCK GONE
                          └── mem_init_print_info()
```

---

## Assumptions

- **Architecture**: AArch64 (ARMv8-A or later)
- **Page size**: 4 KB (`CONFIG_ARM64_4K_PAGES=y`)
- **VA bits**: 48 (`CONFIG_ARM64_VA_BITS=48`)
- **Page table levels**: 4 (`CONFIG_PGTABLE_LEVELS=4`)
- **Memory model**: Sparsemem VMEMMAP (`CONFIG_SPARSEMEM_VMEMMAP=y`)
- **Slab allocator**: SLUB (`CONFIG_SLUB=y`)
- **Kernel source**: Latest mainline (6.x)

---

[**Start Reading → Document 01: Image Entry (head.S)**](01_Image_Entry_head_S.md)
