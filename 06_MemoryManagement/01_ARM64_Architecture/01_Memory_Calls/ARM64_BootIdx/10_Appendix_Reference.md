# Layer 10 — Appendix & Reference

[← Layer 9: Slab, vmalloc & Runtime](09_Slab_Vmalloc_and_Runtime.md) | **Appendix**

---

## A. Complete Boot Timeline — Memory Initialization

```
═══════════════════════════════════════════════════════════════════════════
                    ARM64 LINUX BOOT MEMORY TIMELINE
═══════════════════════════════════════════════════════════════════════════

BOOTLOADER (U-Boot / UEFI)
─────────────────────────────
  Load kernel Image to DRAM (e.g., 0x4008_0000)
  Load DTB to DRAM (e.g., 0x4A00_0000)
  Load initrd to DRAM (e.g., 0x4C00_0000)      [optional]
  Set x0 = DTB physical address
  Jump to Image entry point at EL2 (or EL1)

LAYER 1-2: EARLY ASSEMBLY (arch/arm64/kernel/head.S)
─────────────────────────────────────────────────────
  primary_entry:
    preserve_boot_args               Save x0-x3 to boot_args[]
    init_kernel_el                   Detect EL, drop to EL1 if at EL2
    create_init_idmap                Build identity map page tables:
      adrp x0, init_idmap_pg_dir      Base of ID map tables
      Create 1:1 VA=PA mapping for kernel Image
      Create 1:1 VA=PA mapping for DTB
    __cpu_setup:                     Configure CPU:
      Set MAIR_EL1                    Memory attribute indirection
      Set TCR_EL1                     Translation control (4KB, 48-bit VA)
    __primary_switch:
      __enable_mmu                   Enable MMU with identity map
        msr ttbr0_el1, x25            Load identity map in TTBR0
        msr sctlr_el1, x20            Set M bit → MMU ON
      clear_page_tables               Zero out init_pg_dir
      create_kernel_mapping           Map kernel at high VA (0xFFFF8...)

LAYER 3: KERNEL VIRTUAL MAPPING (arch/arm64/mm/mmu.c)
───────────────────────────────────────────────────────
    __primary_switched:
      early_fdt_map                  Fixmap-map the DTB
      init_feature_override          Parse DT for CPU features
      switch_to_swapper              Switch TTBR1: init_pg_dir → swapper_pg_dir
      start_kernel()                 Jump to C code!

LAYER 4: start_kernel() — EARLY C CODE (init/main.c)
───────────────────────────────────────────────────────
    setup_arch(&command_line):
      early_fixmap_init()            Set up fixmap page tables
      early_ioremap_init()           Early I/O remapping
      setup_machine_fdt(__fdt_pointer):
        fixmap_remap_fdt()            Map DTB via fixmap
        early_init_dt_scan()          Parse /chosen, /memory nodes
        early_init_dt_scan_memory():  ★ MEMORY DISCOVERY ★
          For each /memory node:
            memblock_add(base, size)   Register RAM with memblock
      parse_early_param()            Parse "mem=", "memmap=" etc.
      arm64_memblock_init():
        memblock_remove()             Clip to supported PA range
        memblock_add()                Add reserved regions
        memblock_reserve(kernel)       Reserve kernel Image
        memblock_reserve(initrd)       Reserve initramfs
        fdt_enforce_memory_region()
        reserve_crashkernel()
        reserve_elfcorehdr()

LAYER 5: MEMBLOCK FULLY OPERATIONAL
────────────────────────────────────
      early_mem_init():
        paging_init()                See Layer 6 below

LAYER 6: PAGE TABLE CONSTRUCTION (arch/arm64/mm/mmu.c)
────────────────────────────────────────────────────────
      paging_init():
        map_mem()                    Linear map ALL RAM (block mappings)
          __create_pgd_mapping()      Walk PGD→P4D→PUD→PMD→PTE
            For each 1GB aligned:     PUD block entry (if possible)
            For each 2MB aligned:     PMD block entry (section mapping)
            Otherwise:                PTE entries (4KB pages)
        create_idmap()               Rebuild identity map (refined)
        declare_kernel_vmas()        Register kernel VA regions

LAYER 7: ZONE & NODE SETUP
───────────────────────────
      bootmem_init():
        arch_numa_init()             Detect NUMA topology (DT/ACPI)
        dma_limits_init()            Compute DMA zone boundaries
        dma_contiguous_reserve()     Reserve CMA pool

    mm_core_init_early():
      free_area_init():              ★ ZONE/NODE/PAGE INIT ★
        sparse_init()                Sparsemem + vmemmap setup
        free_area_init_node():
          calculate_node_totalpages() Compute spanned/present/managed
          free_area_init_core():      Per-zone init
            zone_init_free_lists()    Init empty buddy free lists
        memmap_init():               Init every struct page
          __init_single_page()        flags, refcount=1, zone/node

LAYER 8: THE GREAT HANDOFF
──────────────────────────
    mm_core_init():
      build_all_zonelists()          Zone fallback ordering
      memblock_free_all():           ★ MEMBLOCK → BUDDY ★
        reset_all_zones_managed_pages()  Zero managed_pages
        free_low_memory_core_early():
          memmap_init_reserved_pages() Mark reserved pages
          For each free memblock range:
            __free_pages_core():
              ClearPageReserved()       Un-reserve pages
              set_page_count(0)         Mark as free
              __free_one_page():        ★ BUDDY MERGE ★
                XOR buddy finding
                Merge up the order ladder
                Add to zone->free_area[]
        totalram_pages_add()           Set total RAM counter

LAYER 9: SLAB & VMALLOC
────────────────────────
      kmem_cache_init():             ★ SLUB BOOTSTRAP ★
        create_boot_cache(node)       Static kmem_cache_node cache
        slab_state = PARTIAL
        create_boot_cache(cache)      Static kmem_cache cache
        bootstrap()                   Replace static with dynamic
        create_kmalloc_caches()       kmalloc-8 through kmalloc-8k
        slab_state = UP               kmalloc() WORKS!

      vmalloc_init():                ★ VMALLOC READY ★
        Create vmap_area_cachep
        vmap_init_free_space()
        vmap_initialized = true       vmalloc()/ioremap() WORK!

POST-INIT:
──────────
      kmem_cache_init_late()         slab_state = FULL
      page_alloc_init_late():
        deferred struct page init    Parallel per-node init
        memblock_discard()           Free memblock arrays themselves
      setup_per_cpu_pageset()        Tune PCP batch/high
      proc_caches_init()             Slab caches for processes
      vfs_caches_init()              Slab caches for VFS

═══════════════════════════════════════════════════════════════════════════
    ★ MEMORY SUBSYSTEM FULLY OPERATIONAL — READY FOR USERSPACE ★
═══════════════════════════════════════════════════════════════════════════
```

---

## B. Key Kconfig Options

| Option | Default | Description |
|--------|---------|-------------|
| `CONFIG_ARM64` | y | ARM64 architecture |
| `CONFIG_ARM64_4K_PAGES` | y | 4 KB page size (vs 16K/64K) |
| `CONFIG_ARM64_VA_BITS_48` | y | 48-bit virtual address space |
| `CONFIG_ARM64_PA_BITS_48` | y | 48-bit physical address space |
| `CONFIG_PGTABLE_LEVELS` | 4 | 4-level page tables (PGD→PUD→PMD→PTE) |
| `CONFIG_SPARSEMEM_VMEMMAP` | y | Virtual mem_map for sparse memory |
| `CONFIG_ZONE_DMA` | y | Enable ZONE_DMA (< 1 GB) |
| `CONFIG_ZONE_DMA32` | y | Enable ZONE_DMA32 (< 4 GB) |
| `CONFIG_CMA` | y | Contiguous Memory Allocator |
| `CONFIG_SLUB` | y | SLUB slab allocator |
| `CONFIG_NUMA` | n/y | NUMA support (y for servers) |
| `CONFIG_DEFERRED_STRUCT_PAGE_INIT` | y | Defer struct page init for fast boot |
| `CONFIG_RANDOMIZE_BASE` | y | KASLR — randomize kernel base |
| `CONFIG_KFENCE` | y | Kernel Electric Fence (debug) |
| `CONFIG_MEMCG` | y | Memory cgroups |
| `CONFIG_TRANSPARENT_HUGEPAGE` | y | THP for userspace |
| `CONFIG_MEMORY_HOTPLUG` | n/y | Memory hotplug support |

---

## C. Data Structure Reference

### C.1 `struct memblock` (`include/linux/memblock.h`)

```c
struct memblock {
    bool bottom_up;                     /* Direction of allocation */
    phys_addr_t current_limit;          /* Max allocatable address */
    struct memblock_type memory;        /* Known physical memory */
    struct memblock_type reserved;      /* Reserved regions */
};

struct memblock_type {
    unsigned long cnt;                  /* Number of regions */
    unsigned long max;                  /* Array capacity */
    phys_addr_t total_size;             /* Sum of all region sizes */
    struct memblock_region *regions;    /* Array of regions */
    char *name;                         /* "memory" or "reserved" */
};

struct memblock_region {
    phys_addr_t base;                   /* Start physical address */
    phys_addr_t size;                   /* Size in bytes */
    enum memblock_flags flags;          /* HOTPLUG, MIRROR, NOMAP */
    int nid;                            /* NUMA node ID */
};
```

### C.2 `pg_data_t` / `struct pglist_data` (`include/linux/mmzone.h`)

```c
typedef struct pglist_data {
    struct zone node_zones[MAX_NR_ZONES];  /* Zone array */
    struct zonelist node_zonelists[MAX_ZONELISTS]; /* Fallback lists */
    int nr_zones;                          /* Populated zones */
    unsigned long node_start_pfn;          /* First PFN */
    unsigned long node_present_pages;      /* Physical pages (no holes) */
    unsigned long node_spanned_pages;      /* Total span (with holes) */
    int node_id;                           /* NUMA node number */
    struct lruvec __lruvec;                /* LRU lists for reclaim */
    unsigned long totalreserve_pages;      /* Unreclaimable reserve */
    struct task_struct *kswapd;            /* Page reclaim daemon */
} pg_data_t;
```

### C.3 `struct zone` (`include/linux/mmzone.h`)

```c
struct zone {
    unsigned long _watermark[NR_WMARK];    /* min, low, high */
    unsigned long watermark_boost;
    unsigned long lowmem_reserve[MAX_NR_ZONES]; /* Reserve for fallback */
    struct pglist_data *zone_pgdat;        /* Parent node */
    struct per_cpu_pages __percpu *per_cpu_pageset; /* PCP */
    unsigned long zone_start_pfn;          /* First PFN */
    atomic_long_t managed_pages;           /* Buddy-managed pages */
    unsigned long spanned_pages;           /* Total span */
    unsigned long present_pages;           /* Actual RAM */
    const char *name;                      /* "DMA", "Normal", etc. */
    struct free_area free_area[NR_PAGE_ORDERS]; /* Buddy free lists */
    spinlock_t lock;                       /* Zone lock */
};
```

### C.4 `struct free_area` (`include/linux/mmzone.h`)

```c
struct free_area {
    struct list_head free_list[MIGRATE_TYPES]; /* Per-migratetype lists */
    unsigned long nr_free;                      /* Total free blocks */
};
```

### C.5 `struct page` (`include/linux/mm_types.h`)

```c
struct page {       /* Simplified — actual struct uses unions extensively */
    unsigned long flags;         /* Zone, node, PG_locked, PG_dirty, etc. */
    atomic_t _refcount;          /* Reference count */
    atomic_t _mapcount;          /* Page table mapping count */
    struct list_head lru;        /* LRU or buddy list linkage */
    struct address_space *mapping; /* File mapping or anon_vma */
    pgoff_t index;               /* Offset within mapping */
    unsigned long private;       /* Filesystem/swap private data */
    /* ... many more fields via unions ... */
};
/* sizeof(struct page) ≈ 64 bytes */
```

### C.6 `struct kmem_cache` (`include/linux/slub_def.h`)

```c
struct kmem_cache {
    unsigned int size;              /* Object size (with metadata) */
    unsigned int object_size;       /* Actual usable object size */
    unsigned int offset;            /* Freelist pointer offset */
    struct kmem_cache_cpu __percpu *cpu_slab; /* Per-CPU slab data */
    unsigned long min_partial;      /* Min partial slabs to keep */
    gfp_t allocflags;              /* Allocation flags for pages */
    int refcount;                   /* Cache reference count */
    const char *name;               /* "kmalloc-128", "inode_cache", etc. */
    struct list_head list;          /* Global cache list */
    struct kmem_cache_node *node[MAX_NUMNODES]; /* Per-node data */
    struct kmem_cache_order_objects oo; /* Optimal slab order+objects */
};
```

---

## D. Source File Reference

### D.1 Architecture-Specific (ARM64)

| File | Layer | Purpose |
|------|-------|---------|
| `arch/arm64/kernel/head.S` | 2 | Primary boot entry, MMU enable |
| `arch/arm64/kernel/pi/map_range.c` | 2 | Early page table builder |
| `arch/arm64/kernel/pi/map_kernel.c` | 2 | Early kernel mapping |
| `arch/arm64/mm/proc.S` | 2 | `__cpu_setup` (MAIR/TCR/SCTLR) |
| `arch/arm64/mm/mmu.c` | 3,6 | `paging_init`, `map_mem`, kernel VA map |
| `arch/arm64/mm/init.c` | 4,7 | `arm64_memblock_init`, `bootmem_init` |
| `arch/arm64/mm/fixmap.c` | 4 | `early_fixmap_init` |
| `arch/arm64/include/asm/memory.h` | All | VA layout constants, PA↔VA macros |
| `arch/arm64/include/asm/pgtable-hwdef.h` | 2,6 | HW page table bit definitions |
| `arch/arm64/include/asm/pgtable.h` | 6 | SW page table manipulation macros |
| `arch/arm64/include/asm/kernel-pgtable.h` | 2 | Kernel page table size macros |

### D.2 Core Memory Management

| File | Layer | Purpose |
|------|-------|---------|
| `mm/memblock.c` | 5,8 | Early boot allocator, memblock_free_all |
| `mm/mm_init.c` | 7,8,9 | `free_area_init`, `mm_core_init` |
| `mm/page_alloc.c` | 8 | Buddy allocator, `__free_one_page`, `alloc_pages` |
| `mm/slub.c` | 9 | SLUB slab allocator |
| `mm/vmalloc.c` | 9 | vmalloc subsystem |
| `mm/sparse.c` | 7 | Sparsemem initialization |
| `mm/sparse-vmemmap.c` | 7 | Virtual mem_map setup |
| `mm/memory.c` | 9 | Page fault handler |
| `mm/mmap.c` | 9 | VMA management, mmap |
| `mm/vmscan.c` | — | Page reclaim (kswapd, direct reclaim) |
| `mm/compaction.c` | — | Memory compaction |

### D.3 Device Tree

| File | Layer | Purpose |
|------|-------|---------|
| `drivers/of/fdt.c` | 4 | `early_init_dt_scan_memory` |
| `drivers/of/address.c` | 4 | DT address translation |

### D.4 Init

| File | Layer | Purpose |
|------|-------|---------|
| `init/main.c` | 4-9 | `start_kernel()` orchestrator |

---

## E. ARM64 Virtual Address Space Layout

```
ARM64 VA Layout (48-bit, 4K pages, CONFIG_ARM64_VA_BITS=48)
═══════════════════════════════════════════════════════════

  TTBR0 (User Space)                TTBR1 (Kernel Space)
  ═══════════════════                ═══════════════════

  0x0000_0000_0000_0000             0xFFFF_0000_0000_0000
  ┌──────────────────┐              ┌──────────────────────┐
  │                  │              │ Linear Map           │
  │  User virtual    │              │ PAGE_OFFSET =        │
  │  address space   │              │   0xFFFF_8000_0000_0000│
  │                  │              │ Maps ALL physical RAM │
  │  256 TB          │              │ VA = PA + PAGE_OFFSET │
  │                  │              │                      │
  │  Per-process     │              ├──────────────────────┤
  │  (TTBR0_EL1)     │              │ VMALLOC region       │
  │                  │              │ VMALLOC_START =       │
  │                  │              │   0xFFFF_0000_0000_0000│
  │                  │              │                      │
  │                  │              │ vmalloc(), ioremap(), │
  │                  │              │ module loading        │
  │                  │              ├──────────────────────┤
  │                  │              │ VMEMMAP region        │
  │                  │              │ Virtual struct page   │
  │                  │              │ array                 │
  │                  │              ├──────────────────────┤
  │                  │              │ PCI I/O space         │
  │                  │              ├──────────────────────┤
  │                  │              │ Fixmap region         │
  │                  │              │ Fixed virtual address │
  │                  │              │ mappings (FDT, early  │
  │                  │              │ console, etc.)        │
  │                  │              ├──────────────────────┤
  │                  │              │ Kernel Image          │
  │                  │              │ _text to _end         │
  │                  │              │ (KIMAGE_VADDR)        │
  │                  │              ├──────────────────────┤
  │                  │              │ Modules region        │
  │                  │              │ (128 MB for modules)  │
  └──────────────────┘              └──────────────────────┘
  0x0000_FFFF_FFFF_FFFF             0xFFFF_FFFF_FFFF_FFFF

  ◄── 16-bit gap ("VA hole") between TTBR0 and TTBR1 ──►
  Any access to addresses in this gap causes a fault.
```

---

## F. Page Table Format (AArch64, 4KB granule)

```
4-Level Page Table Walk (48-bit VA):
═══════════════════════════════════

  Virtual Address (48-bit):
  ┌──────┬──────┬──────┬──────┬────────────┐
  │63..48│47..39│38..30│29..21│ 20..12 │11..0│
  │ sign │ PGD  │ PUD  │ PMD  │  PTE   │offset│
  │extend│ index│ index│ index│  index │     │
  │ (16) │ (9)  │ (9)  │ (9)  │  (9)   │(12) │
  └──────┴──┬───┴──┬───┴──┬───┴───┬────┴──┬──┘
            │      │      │       │       │
            ▼      ▼      ▼       ▼       ▼
         ┌─────┐ ┌─────┐ ┌─────┐ ┌─────┐
  TTBR──►│PGD  │►│PUD  │►│PMD  │►│PTE  │──► Physical Page + offset
         │table│ │table│ │table│ │table│
         │512  │ │512  │ │512  │ │512  │
         │entry│ │entry│ │entry│ │entry│
         └─────┘ └─────┘ └─────┘ └─────┘

Each table: 512 entries × 8 bytes = 4096 bytes = 1 page

Entry format (descriptor):
  ┌────────────────────────────────────────────────────────────┐
  │ Bit 0:   Valid (1 = valid entry)                            │
  │ Bit 1:   Type (0 = block, 1 = table/page)                  │
  │ Bits 47:12: Physical address of next table or page          │
  │ Bits 54-58: Upper attributes (UXN, PXN, etc.)              │
  │ Bits 2-11:  Lower attributes (AP, SH, AF, AttrIndx)       │
  └────────────────────────────────────────────────────────────┘

Block mappings (used in linear map):
  PUD block = 1 GB   (bits 29:0 from VA used as offset)
  PMD block = 2 MB   (bits 20:0 from VA used as offset)
  PTE page  = 4 KB   (bits 11:0 from VA used as offset)
```

---

## G. Glossary

| Term | Definition |
|------|-----------|
| **BL** | Bootloader (U-Boot, UEFI, etc.) |
| **BSS** | Block Started by Symbol — uninitialized global data section |
| **CMA** | Contiguous Memory Allocator — reserves for large contiguous allocs |
| **DMA** | Direct Memory Access — hardware copies data without CPU |
| **DTB** | Device Tree Blob — binary hardware description |
| **DTS** | Device Tree Source — human-readable hardware description |
| **EL** | Exception Level — ARM privilege level (EL0=user, EL1=kernel, EL2=hypervisor) |
| **FDT** | Flattened Device Tree (= DTB) |
| **Fixmap** | Compile-time-fixed virtual addresses for boot-time mappings |
| **GFP** | Get Free Pages — allocation flags (GFP_KERNEL, GFP_DMA, etc.) |
| **Identity Map** | VA = PA mapping, used during MMU enable/disable |
| **KASLR** | Kernel Address Space Layout Randomization |
| **Linear Map** | 1:1 mapping of all physical RAM at a fixed VA offset |
| **LRU** | Least Recently Used — page reclaim list ordering |
| **MAIR** | Memory Attribute Indirection Register — defines memory types |
| **memblock** | Early boot memory allocator (before buddy) |
| **MMU** | Memory Management Unit — hardware address translation |
| **NID** | Node ID — NUMA node identifier |
| **NUMA** | Non-Uniform Memory Access — multi-node memory architecture |
| **OOM** | Out Of Memory — condition triggering process killing |
| **PA** | Physical Address |
| **PCP** | Per-CPU Pages — local page cache to avoid zone lock |
| **PFN** | Page Frame Number = PA / PAGE_SIZE |
| **PGD** | Page Global Directory — top-level page table |
| **PMD** | Page Middle Directory — 3rd-level page table |
| **PTE** | Page Table Entry — lowest-level page table entry |
| **PUD** | Page Upper Directory — 2nd-level page table |
| **SCTLR** | System Control Register — enables MMU, caches, etc. |
| **SLUB** | Slab allocator implementation (successor to SLAB/SLOB) |
| **Sparsemem** | Memory model for systems with large PA holes |
| **TCR** | Translation Control Register — page table config |
| **TLB** | Translation Lookaside Buffer — page table cache in CPU |
| **TTBR** | Translation Table Base Register — points to page table base |
| **TTBR0** | User-space page table base (lower VA half) |
| **TTBR1** | Kernel-space page table base (upper VA half) |
| **UMA** | Uniform Memory Access — single-node memory (most ARM64 boards) |
| **VA** | Virtual Address |
| **VMA** | Virtual Memory Area — process address range descriptor |
| **vmalloc** | Virtual contiguous allocator (physically scattered) |
| **vmemmap** | Virtual memory map — struct page array via virtual mapping |
| **Watermark** | Free page thresholds controlling reclaim behavior |
| **Zone** | Physical memory region with specific DMA/allocation properties |

---

## H. Command Line Parameters Affecting Memory

| Parameter | Example | Effect |
|-----------|---------|--------|
| `mem=` | `mem=2G` | Limit usable memory |
| `memmap=` | `memmap=256M$0x100000000` | Reserve/define memory regions |
| `crashkernel=` | `crashkernel=256M` | Reserve for kdump |
| `kernelcore=` | `kernelcore=1G` | ZONE_MOVABLE sizing |
| `movablecore=` | `movablecore=1G` | ZONE_MOVABLE sizing |
| `cma=` | `cma=64M` | CMA pool size |
| `nokaslr` | | Disable KASLR |
| `arm64.nopauth` | | Disable pointer authentication |
| `numa=off` | | Disable NUMA |
| `init_on_alloc=1` | | Zero pages on allocation |
| `init_on_free=1` | | Zero pages on free |
| `page_poison=1` | | Poison freed pages (debug) |
| `slub_debug=` | `slub_debug=FZPU` | Enable SLUB debug checks |
| `transparent_hugepage=` | `=never` | THP policy |

---

## I. Reading Order Recommendation

For **first-time readers**:
```
00_Index.md                         ← Start here (overview)
01_Hardware_Entry_and_Bootloader.md ← Hardware context
02_Early_Assembly_Boot.md           ← head.S assembly walkthrough
03_Kernel_Virtual_Mapping.md        ← VA space setup
04_Device_Tree_and_Memory_Discovery.md ← How RAM is discovered
05_Memblock_Early_Allocator.md      ← Early allocator deep dive
06_Page_Tables_and_Paging_Init.md   ← Final page table construction
07_Zones_Nodes_and_Page_Init.md     ← Memory organization
08_Buddy_Allocator_Handoff.md       ← The critical transition ★
09_Slab_Vmalloc_and_Runtime.md      ← Runtime allocators
10_Appendix_Reference.md            ← This file (reference)
```

For **experienced developers** wanting specific topics:
- **How does the MMU get enabled?** → Layers 2-3
- **Where does the kernel find RAM?** → Layer 4
- **How does memblock work?** → Layer 5
- **How are page tables built?** → Layer 6
- **What are zones and nodes?** → Layer 7
- **How does buddy allocation start?** → Layer 8
- **How does kmalloc work?** → Layer 9

---

## J. Key dmesg Messages to Watch

```
# Early boot — DTB and memblock
[    0.000000] BIOS-provided physical RAM map:              ← x86 only
[    0.000000] OF: fdt: Machine model: ...                  ← DTB model
[    0.000000] earlycon: ... at MMIO ...                    ← Early console

# Memory regions
[    0.000000] Reserved memory: ...                         ← DT reserved-memory
[    0.000000] cma: Reserved 64 MiB ...                     ← CMA pool

# Zone initialization
[    0.000000] Zone ranges:                                 ← Zone boundaries
[    0.000000]   DMA      [mem 0x...]
[    0.000000]   DMA32    [mem 0x...]
[    0.000000]   Normal   [mem 0x...]
[    0.000000] Movable zone start for each node
[    0.000000] Early memory node ranges
[    0.000000]   node   0: [mem 0x...]                      ← Per-node ranges

# After memblock_free_all
[    0.000000] Memory: 5765432K/6291456K available          ← ★ THE SUMMARY ★
[    0.000000]   (12345K kernel code, 1234K rwdata, ...

# SLUB
[    0.000000] SLUB: HWalign=64, Order=0-3, MinObjects=0   ← SLUB config

# vmalloc
[    0.000000] vmalloc : 0xffff... - 0xffff...              ← vmalloc VA range

# Buddy allocator state
[    0.000000] Freeing unused kernel memory: ...K           ← __init sections freed
[    0.000000] Run /sbin/init ...                           ← Userspace starts!
```

---

[← Layer 9: Slab, vmalloc & Runtime](09_Slab_Vmalloc_and_Runtime.md) | **Appendix**
