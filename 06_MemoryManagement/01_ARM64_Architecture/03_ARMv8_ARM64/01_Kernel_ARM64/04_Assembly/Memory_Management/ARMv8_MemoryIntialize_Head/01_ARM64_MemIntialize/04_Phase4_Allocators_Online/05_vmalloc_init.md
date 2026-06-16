# `vmalloc_init()` — Virtual Memory Allocator Initialization

**Source:** `mm/vmalloc.c` lines 5425–5486
**Phase:** Allocators Online
**Memory Allocator:** Buddy (for physical pages) + page tables (for virtual mapping)
**Called by:** `mm_core_init()`
**Purpose:** Enable `vmalloc()`, `vmap()`, `ioremap()` for virtually contiguous allocations

---

## What vmalloc Is

vmalloc allocates **virtually contiguous** but **physically discontiguous** memory. Unlike `kmalloc()` (which returns physically contiguous memory), `vmalloc()` can map arbitrary physical pages into a contiguous virtual address range.

```
kmalloc(16384):
  Physical: [page_A][page_B][page_C][page_D]  ← Must be contiguous!
  Virtual:  [page_A][page_B][page_C][page_D]  ← Contiguous (linear map)

vmalloc(16384):
  Physical: [page_X]...[page_Y]...[page_Z]...[page_W]  ← Can be scattered
  Virtual:  [page_X][page_Y][page_Z][page_W]            ← Contiguous (vmalloc map)
```

**Use cases:**
- Large allocations where physical contiguity isn't needed
- Module loading (code needs contiguous VA but not PA)
- Network buffers, large data structures
- `ioremap()` for memory-mapped I/O

---

## vmalloc Address Space

```
ARM64 kernel virtual address space (48-bit VA):

0xFFFF_FF80_0800_0000  MODULES_END = VMALLOC_START
    │
    │ ┌──────────────────────────────────────────┐
    │ │         VMALLOC / VMAP region             │
    │ │                                           │
    │ │  vmalloc(4096) → 0xFFFF_FF80_0800_0000    │
    │ │  vmalloc(8192) → 0xFFFF_FF80_0800_2000    │
    │ │  (guard page)  → 0xFFFF_FF80_0800_4000    │
    │ │  vmalloc(4096) → 0xFFFF_FF80_0800_5000    │
    │ │  ...                                      │
    │ │                                           │
    │ │  ioremap(mmio_addr) → somewhere here      │
    │ │                                           │
    │ └──────────────────────────────────────────┘
    │
0xFFFF_FFFC_0000_0000  VMALLOC_END
```

Each vmalloc allocation has a **guard page** (unmapped page) after it to detect buffer overflows.

---

## How vmalloc_init() Works

### Step 1: Create vmap_area Slab Cache

```c
void __init vmalloc_init(void)
{
    // Create slab cache for vmap_area structs
    vmap_area_cachep = KMEM_CACHE(vmap_area, SLAB_PANIC);
```

`struct vmap_area` tracks each vmalloc allocation:

```c
struct vmap_area {
    unsigned long va_start;        // Virtual start address
    unsigned long va_end;          // Virtual end address
    struct rb_node rb_node;        // Red-black tree node (for VA space management)
    struct list_head list;         // Linked list node
    union {
        unsigned long subtree_max_size;  // For free VA management
        struct vm_struct *vm;            // Associated vm_struct (for allocated areas)
    };
    unsigned long flags;           // VA flags
};
```

---

### Step 2: Import Existing Kernel VMAs

```c
    // Import kernel VMAs that were registered during paging_init()
    // (kernel text, rodata, inittext, initdata, data sections)

    for (tmp = vmlist; tmp; tmp = tmp->next) {
        va = kmem_cache_zalloc(vmap_area_cachep, GFP_NOWAIT);

        va->va_start = (unsigned long)tmp->addr;
        va->va_end = va->va_start + tmp->size;
        va->vm = tmp;

        insert_vmap_area(va, &vmap_area_root, &vmap_area_list);
    }
```

`vmlist` was populated by `declare_kernel_vmas()` during `paging_init()`. This import tells vmalloc "these VA ranges are already in use — don't allocate over them."

---

### Step 3: Initialize Free VA Space Management

```c
    // Set up the free vmap_area tree for tracking available VA ranges
    vmap_init_free_space();
}
```

**`vmap_init_free_space()`** creates `vmap_area` entries for all **unused** VA ranges in the vmalloc region:

```c
static void __init vmap_init_free_space(void)
{
    unsigned long vmap_start = VMALLOC_START;
    unsigned long vmap_end = VMALLOC_END;
    struct vmap_area *busy, *free;

    // Walk all busy areas (sorted by VA)
    // Create free areas in the gaps

    list_for_each_entry(busy, &vmap_area_list, list) {
        if (vmap_start < busy->va_start) {
            // Gap before this busy area = free space
            free = kmem_cache_zalloc(vmap_area_cachep, GFP_NOWAIT);
            free->va_start = vmap_start;
            free->va_end = busy->va_start;
            insert_vmap_area_free(free, ...);
        }
        vmap_start = busy->va_end;
    }

    // Final gap after last busy area
    if (vmap_start < vmap_end) {
        free = kmem_cache_zalloc(vmap_area_cachep, GFP_NOWAIT);
        free->va_start = vmap_start;
        free->va_end = vmap_end;
        insert_vmap_area_free(free, ...);
    }
}
```

---

## How `vmalloc()` Works (Post-Init)

```c
void *vmalloc(unsigned long size)
{
    return __vmalloc_node(size, 1, GFP_KERNEL, NUMA_NO_NODE,
                          __builtin_return_address(0));
}
```

### Allocation Steps

```
vmalloc(16384):  // Request 4 pages

Step 1: Find free VA range
  alloc_vmap_area(size=16384+PAGE_SIZE, align=PAGE_SIZE,
                  vstart=VMALLOC_START, vend=VMALLOC_END)

  Search free VA red-black tree for a gap ≥ 20480 bytes (16KB + 4KB guard)
  Result: VA range [0xFFFF_FF80_1000_0000, 0xFFFF_FF80_1000_5000)

  The extra 4KB at the end is the guard page (unmapped → catches overflows)

Step 2: Allocate physical pages
  For each page needed (4 pages):
    page[i] = alloc_pages(GFP_KERNEL, 0)  // From buddy allocator

  Pages may be scattered across physical memory!

Step 3: Map pages into vmalloc VA range
  vmap_pages_range(va_start, va_end, prot, pages, count)

  For each page:
    Create PTE entry: vmalloc_VA → physical_page | PAGE_KERNEL

  This modifies swapper_pg_dir page tables.

Step 4: Return virtual address
  return (void *)va_start;  // 0xFFFF_FF80_1000_0000
```

### VA Space Management with Red-Black Trees

```
Free VA Tree:                    Busy VA Tree:
        ┌───────┐                        ┌───────┐
        │ 32 TB │                        │kernel │
        │ free  │                        │text   │
       ╱        ╲                       ╱        ╲
  ┌───────┐  ┌───────┐          ┌───────┐  ┌───────┐
  │ 8 TB  │  │16 TB  │          │vmalloc│  │vmalloc│
  │ free  │  │ free  │          │alloc#1│  │alloc#2│
  └───────┘  └───────┘          └───────┘  └───────┘

  O(log n) search for free VA ranges
  O(log n) insertion of new allocations
```

---

## vmalloc vs. kmalloc Comparison

| Feature | `kmalloc()` | `vmalloc()` |
|---------|-----------|------------|
| Physical contiguous | Yes | No |
| Virtual contiguous | Yes (linear map) | Yes (vmalloc region) |
| Max size | ~4 MB (order 10) | Limited by VA space |
| Speed | Fast (slab cache) | Slower (page tables, TLB flushes) |
| Guard pages | No | Yes (buffer overflow detection) |
| DMA-safe | Yes (for DMA zones) | No (physically scattered) |
| Where mapped | Linear map (PAGE_OFFSET+) | VMALLOC_START..VMALLOC_END |

---

## vfree() — Freeing vmalloc Memory

```c
void vfree(const void *addr)
{
    // 1. Find vmap_area by VA address (RB-tree lookup)
    va = find_vmap_area((unsigned long)addr);

    // 2. Unmap: clear PTE entries
    vunmap_range(va->va_start, va->va_end);

    // 3. TLB flush (other CPUs may have TLB entries)
    flush_tlb_kernel_range(va->va_start, va->va_end);

    // 4. Free physical pages back to buddy
    for each page:
        __free_pages(page, 0);

    // 5. Return VA range to free tree
    free_vmap_area(va);
}
```

**Lazy TLB flushing:** For performance, `vfree()` may defer TLB flushes and batch them. The VA range is added to a "purge" list and flushed when the free VA space runs low.

---

## Key Takeaways

1. **vmalloc = virtual contiguity without physical contiguity** — ideal for large allocations
2. **Guard pages catch overflows** — every vmalloc has an unmapped trailing page
3. **Red-black tree for VA management** — O(log n) allocation and free
4. **Page tables are modified** — each vmalloc creates PTE entries in `swapper_pg_dir`
5. **TLB cost is real** — vmalloc memory requires page table walks (no block mappings)
6. **Imports existing VMAs** — kernel text/data regions from `paging_init()` are registered
7. **vmap_area slab cache** — metadata allocated from SLUB (which is why SLUB must init first)
