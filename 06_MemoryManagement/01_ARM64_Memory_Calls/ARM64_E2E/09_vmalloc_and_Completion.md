# 09 — vmalloc Init & Memory Subsystem Completion

[← Doc 08: SLUB Bootstrap](08_SLUB_Bootstrap_Code.md) | **Doc 09** | [Doc 10: Master Call Graph →](10_Master_Call_Graph.md)

---

## Overview

This final functional document covers `vmalloc_init()` — the last major allocator to
come online — and the completion sequence that makes the memory subsystem fully
operational. After this, all kernel memory APIs are available.

---

## 1. `vmalloc_init()` — Virtual Contiguous Allocator

**Source**: `mm/vmalloc.c`  
**Called by**: `mm_core_init()` (after `kmem_cache_init()`)

### 1.1 The vmalloc Address Space

ARM64 dedicates a large VA range for vmalloc:

```
VMALLOC_START = MODULES_END
VMALLOC_END   = VMEMMAP_START - 256MB

Typical layout (48-bit VA):
  0xFFFF_0000_0000_0000 .. 0xFFFF_7BFF_BFFF_FFFF  → vmalloc space (~124 TB)
  0xFFFF_7BFF_C000_0000 .. 0xFFFF_7FFF_FFFF_FFFF  → vmemmap
  0xFFFF_8000_0000_0000 .. 0xFFFF_FFFF_FFFF_FFFF  → linear map (PAGE_OFFSET)
```

### 1.2 `vmalloc_init()` Code

```c
void __init vmalloc_init(void)                       // mm/vmalloc.c:5425
{
    struct vmap_area *va;
    struct vm_struct *tmp;
    int i;

    /* ─── Step 1: Initialize per-CPU vfree deferred lists ─── */
    for_each_possible_cpu(i) {
        struct vmap_area *va;
        struct vfree_deferred *p;

        p = &per_cpu(vfree_deferred, i);
        init_llist_head(&p->list);
        INIT_WORK(&p->wq, delayed_vfree_work);
    }

    /* ─── Step 2: Register existing vm_struct areas ─── */
    for (tmp = vmlist; tmp; tmp = tmp->next) {
        //   vmlist = list of boot-time ioremap/fixmap areas
        //   These were allocated before vmalloc_init

        va = kmalloc(sizeof(*va), GFP_NOWAIT);
        //   ★ First kmalloc usage in vmalloc init!
        //   Works because kmem_cache_init() already ran

        if (WARN_ON_ONCE(!va))
            continue;

        va->va_start = (unsigned long)tmp->addr;
        va->va_end = va->va_start + tmp->size;
        va->vm = tmp;

        insert_vmap_area(va, &vmap_area_root, &vmap_area_list);
        //   Insert into the red-black tree that tracks vmalloc VA usage
    }

    /* ─── Step 3: Create the free VA space ─── */
    vmap_init_free_space();
    //   Insert the entire [VMALLOC_START, VMALLOC_END] as free VA
    //   Minus any areas already registered above
    //   Uses a separate red-black tree: free_vmap_area_root

    /* ─── Step 4: Mark vmalloc as initialized ─── */
    vmap_initialized = true;
    //   After this: vmalloc(), vmap(), ioremap() all work
}
```

---

## 2. `__vmalloc_node_range()` — The Core vmalloc Allocator

After init, vmalloc allocations use this function:

**Source**: `mm/vmalloc.c`

```c
void *__vmalloc_node_range_noprof(
    unsigned long size,         // Requested size
    unsigned long align,        // Alignment
    unsigned long start,        // VA range start (VMALLOC_START)
    unsigned long end,          // VA range end (VMALLOC_END)
    gfp_t gfp_mask,            // Allocation flags
    pgprot_t prot,              // Page protection
    unsigned long vm_flags,     // VM_* flags
    int node,                   // NUMA node
    const void *caller)         // Return address for debugging
{
    struct vm_struct *area;
    struct page **pages;
    unsigned int nr_pages, array_size;
    void *addr;

    size = PAGE_ALIGN(size);
    nr_pages = size >> PAGE_SHIFT;

    /* ─── Step 1: Allocate VM area (find free VA range) ─── */
    area = __get_vm_area_node(size, align, VM_ALLOC | vm_flags,
                              start, end, node, gfp_mask, caller);
    //   Finds a free VA range in the vmalloc space
    //   Uses red-black tree of free areas
    //   Returns vm_struct with addr, size fields

    if (!area)
        return NULL;

    /* ─── Step 2: Allocate backing physical pages ─── */
    addr = __vmalloc_area_node(area, gfp_mask, prot, node);
    //   Allocates nr_pages individual pages from buddy
    //   Maps each page into the VA range via page tables
    //   (See below)

    return addr;    // Return vmalloc virtual address
}
```

### 2.1 `__vmalloc_area_node()` — Allocate and Map Pages

```c
static void *__vmalloc_area_node(struct vm_struct *area, gfp_t gfp_mask,
                                 pgprot_t prot, int node)    // mm/vmalloc.c:3829
{
    unsigned int nr_pages = get_vm_area_size(area) >> PAGE_SHIFT;
    struct page **pages;
    unsigned int i;

    /* Allocate page pointer array */
    if (nr_pages > PAGE_SIZE / sizeof(struct page *)) {
        pages = __vmalloc_node(array_size, 1, gfp_mask | __GFP_HIGHMEM,
                               node, area->caller);
        //   Array itself might be vmalloc'd if large enough!
    } else {
        pages = kmalloc_node(array_size, gfp_mask, node);
    }
    area->pages = pages;
    area->nr_pages = nr_pages;

    /* ─── Allocate individual pages ─── */
    for (i = 0; i < nr_pages; i++) {
        struct page *page;

        page = alloc_pages_node(node, gfp_mask, 0);
        //   Allocate ONE page (order 0) from buddy for each
        //   Pages do NOT need to be physically contiguous!

        if (!page)
            goto fail;

        area->pages[i] = page;
    }

    /* ─── Map pages into vmalloc VA space ─── */
    if (vmap_pages_range(addr, addr + size, prot, pages, PAGE_SHIFT) < 0)
        goto fail;
    //   Create page table entries:
    //     For each page: VA[i] → PA of pages[i]
    //     Uses swapper_pg_dir (kernel page tables)
    //     Creates PGD→PUD→PMD→PTE chain for the VA range

    return area->addr;
}
```

**Key insight**: vmalloc pages are physically scattered but virtually contiguous.
The page tables map each virtual page to a different physical page.

```
vmalloc VA space:                Physical memory:
  VA+0x0000 → PTE → page_A         page_A at PA 0x5230_1000
  VA+0x1000 → PTE → page_B         page_B at PA 0x4180_5000
  VA+0x2000 → PTE → page_C         page_C at PA 0x7FF0_3000
  VA+0x3000 → PTE → page_D         page_D at PA 0x4000_2000
```

---

## 3. Completion: `page_alloc_init_late()`

**Source**: `mm/mm_init.c`  
**Called by**: `kernel_init_freeable()` (after `start_kernel()` proper)

```c
void __init page_alloc_init_late(void)              // mm/mm_init.c:2323
{
    /* ─── Mark CMA pageblocks ─── */
    set_pageblock_migratetype_for_cma();

    /* ─── Complete deferred struct page init ─── */
    struct_pages_for_free_area_init();
    //   On large memory systems (>1GB), struct page init was deferred
    //   This completes it (may run in parallel on multiple CPUs)

    /* ─── FREE ALL REMAINING MEMBLOCK MEMORY ─── */
    memblock_free_all();                            // ★ Buddy takes over
    after_bootmem = 1;

    /* ─── System administration ─── */
    page_alloc_sysctl_init();
    report_meminit();                               // Print final memory info
}
```

---

## 4. `memblock_discard()` — Discarding Memblock Data

**Source**: `mm/memblock.c`  
**Called by**: `free_initmem()` (when init sections are freed)

```c
void __init memblock_discard(void)
{
    phys_addr_t addr, size;

    /* Free the memblock.memory.regions array if it was dynamically allocated */
    if (memblock.memory.regions != memblock_memory_init_regions) {
        addr = __pa(memblock.memory.regions);
        size = PAGE_ALIGN(sizeof(struct memblock_region) * memblock.memory.max);
        memblock_free_late(addr, size);
        //   → __free_pages_core() → buddy
    }

    /* Free the memblock.reserved.regions array */
    if (memblock.reserved.regions != memblock_reserved_init_regions) {
        addr = __pa(memblock.reserved.regions);
        size = PAGE_ALIGN(sizeof(struct memblock_region) * memblock.reserved.max);
        memblock_free_late(addr, size);
    }
}
```

---

## 5. Final Memory API Availability

After all initialization is complete:

| API | Backing | Available Since |
|-----|---------|----------------|
| `memblock_alloc()` | memblock | `setup_arch()` (very early) |
| `alloc_pages()` | buddy | `page_alloc_init_late()` (memblock_free_all) |
| `__get_free_pages()` | buddy | Same as alloc_pages |
| `kmalloc()` | SLUB → buddy | `kmem_cache_init()` → `create_kmalloc_caches()` |
| `kmem_cache_alloc()` | SLUB → buddy | `kmem_cache_init()` (slab_state = PARTIAL) |
| `vmalloc()` | vmalloc → buddy | `vmalloc_init()` |
| `ioremap()` | vmalloc VA space | `vmalloc_init()` |
| `kzalloc()` | SLUB | Same as kmalloc |
| `kvmalloc()` | kmalloc OR vmalloc | After vmalloc_init |

---

## 6. Complete Boot Timeline

```
Time ──────────────────────────────────────────────────────────────→

head.S:
│ _text                    ──── MZ header + branch
│ primary_entry            ──── save FDT, set stack
│ create_init_idmap        ──── identity map (VA=PA)
│ __cpu_setup              ──── MAIR, TCR, SCTLR
│ __primary_switch         ──── enable MMU
│   __enable_mmu           ──── TTBR0/TTBR1, set SCTLR.M
│   early_map_kernel       ──── kernel map at KIMAGE_VADDR
│   __primary_switched     ──── switch to kernel VA
│     start_kernel()       ──── ★ ENTER C CODE

start_kernel():
│ setup_arch()
│   early_fixmap_init()    ──── fixmap page tables
│   setup_machine_fdt()    ──── parse DTB
│     early_init_dt_scan_memory()
│       memblock_add()     ──── ★ RAM KNOWN
│   arm64_memblock_init()  ──── reservations (kernel, initrd, DTB)
│   paging_init()          ──── ★ LINEAR MAP BUILT
│     map_mem()            ──── map all RAM into linear map
│   bootmem_init()         ──── NUMA, zone boundaries, CMA
│     dma_limits_init()    ──── ZONE_DMA/DMA32 limits
│
│ mm_core_init_early()
│   free_area_init()       ──── ★ ZONES + STRUCT PAGE INIT
│     sparse_init()        ──── sparsemem sections + vmemmap
│     free_area_init_node()
│       free_area_init_core()
│         zone_init_free_lists() ── empty free lists
│         memmap_init()    ──── init struct page (PG_reserved)
│
│ mm_core_init()
│   build_all_zonelists()  ──── zone fallback lists
│   kmem_cache_init()      ──── ★ SLUB BOOTSTRAP
│     create_boot_cache()  ──── static kmem_cache + kmem_cache_node
│     bootstrap()          ──── static → dynamic replacement
│     create_kmalloc_caches() ── kmalloc-8 through kmalloc-8k
│                              ── slab_state = UP
│                              ── ★ KMALLOC WORKS
│   vmalloc_init()         ──── ★ VMALLOC WORKS
│     vmap_init_free_space() ── free VA tree
│
│ (later, in kernel_init_freeable):
│ page_alloc_init_late()
│   memblock_free_all()    ──── ★ BUDDY TAKES OVER
│     free_low_memory_core_early()
│       __free_pages_memory() ── decompose + free to buddy
│         __free_pages_core() ── clear PG_reserved, free
│           __free_one_page() ── buddy merge algorithm
│   after_bootmem = 1      ──── memblock era ends
│
│ ★ ALL MEMORY APIS OPERATIONAL ★
```

---

## State After Document 09

```
┌──────────────────────────────────────────────────────────┐
│  Memory Subsystem: FULLY OPERATIONAL                     │
│                                                          │
│  Allocators (bottom → top):                              │
│    Buddy: manages all physical pages                     │
│      └─ free_area[order].free_list[migratetype]          │
│    SLUB: object-level allocation                         │
│      └─ Per-CPU sheaves → partial slab lists → buddy     │
│    vmalloc: virtually contiguous allocations              │
│      └─ VA range management → individual page alloc      │
│                                                          │
│  Page tables:                                            │
│    swapper_pg_dir (TTBR1):                               │
│      • Kernel image (ROX/RW segments)                    │
│      • Linear map (all RAM, NX)                          │
│      • vmemmap (struct page arrays)                      │
│      • vmalloc space (on-demand)                         │
│      • fixmap                                            │
│                                                          │
│  Memory accounting:                                      │
│    totalram_pages = all pages freed to buddy              │
│    zone->managed_pages = per-zone accounting              │
│    memblock → discarded after boot                       │
│                                                          │
│  Next: Doc 10 — Complete call graph with file:line refs  │
└──────────────────────────────────────────────────────────┘
```

---

[← Doc 08: SLUB Bootstrap](08_SLUB_Bootstrap_Code.md) | **Doc 09** | [Doc 10: Master Call Graph →](10_Master_Call_Graph.md)
