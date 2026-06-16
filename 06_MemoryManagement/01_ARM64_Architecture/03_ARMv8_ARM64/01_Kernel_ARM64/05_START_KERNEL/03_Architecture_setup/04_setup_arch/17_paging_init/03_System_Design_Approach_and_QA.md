# paging_init() — System Design Approach and Q&A

## 1. Why paging_init() Is a Major Architectural Milestone

Before `paging_init()`:
- CPU uses minimal page tables from head.S (identity mapping only)
- No struct page arrays (can't track individual pages)
- No memory zones (ZONE_NORMAL, ZONE_HIGHMEM don't exist yet)
- Only memblock is available for allocation
- Fixmap uses static early PTE tables

After `paging_init()`:
- Full permanent page tables in place
- All lowmem mapped to kernel VA
- Exception vectors at correct location
- struct page array allocated and initialized
- Memory zones exist
- Buddy allocator structures exist (but pages not yet in free lists)

Everything downstream of `paging_init()` in `setup_arch()` (and beyond) operates in the fully-initialized virtual memory environment.

---

## 2. Design Principle: Progressive Memory Initialization

Linux uses a **three-phase memory initialization**:

```
Phase 1: Memblock (available during all of setup_arch)
  Simple: just two arrays (memory, reserved)
  Can only alloc contiguous physical ranges
  No struct page, no zones, no buddy

Phase 2: Page table setup (paging_init)
  Maps physical memory to virtual
  Allocates struct page array (mem_map)
  Creates zone structures
  Buddy allocator STRUCTURES exist but empty (no free pages)

Phase 3: Buddy allocator initialized (mem_init, after setup_arch)
  Memblock free regions released to buddy allocator
  struct pages transitioned from "reserved" to "free"
  kmalloc, vmalloc, page_alloc all become available
```

This progressive approach means each phase can use the previous phase's allocator while setting up the next.

---

## 3. The mem_map: struct page Array

`free_area_init()` (called via `bootmem_init()`) allocates the `mem_map` array using `memblock_alloc()`:

```c
struct page *mem_map;  /* global pointer to page array */

/* Size: one struct page per physical page frame */
/* struct page size: ~64 bytes (varies with config) */
/* For 1GB lowmem with 4KB pages: 1GB / 4KB = 262144 pages × 64 bytes = 16MB */
```

The `mem_map` array is itself allocated from physical memory (via memblock). This creates a slight inefficiency: 16MB of RAM is used to track the remaining ~1008MB. The ratio is about 1.5% overhead.

Struct page contains:
```c
struct page {
    unsigned long flags;          /* PG_locked, PG_uptodate, etc. */
    atomic_t _refcount;           /* page reference count */
    atomic_t _mapcount;           /* number of PTEs mapping this page */
    struct list_head lru;         /* free list link (when free) */
    struct address_space *mapping; /* file cache mapping (when used) */
    pgoff_t index;                /* offset in mapping */
    /* ... more fields ... */
};
```

---

## 4. Dependency Graph

```
[early_ioremap_reset()] ← prerequisite
        │
        ▼
paging_init()
  ├── [prepare_page_table]
  │     └── [memblock state] — determines which PMDs to clear
  │
  ├── [map_lowmem]
  │     └── [arm_lowmem_limit] ← from adjust_lowmem_bounds #2
  │           └── [mem_types[]] ← from build_mem_type_table (early_mm_init)
  │
  ├── [devicemaps_init]
  │     └── [mdesc->map_io()] ← machine-specific MMIO mappings
  │
  └── [bootmem_init]
        ├── [sparse_init]
        │     └── [mem_section[] array] — tracks 128MB memory sections
        │
        └── [zone_sizes_init]
              └── [free_area_init]
                    └── [mem_map] — struct page array for all PFNs
                          │
                          ▼
        ALL DOWNSTREAM CODE:
        pfn_to_page(), page_to_pfn(), get_page(), put_page()
        /proc/meminfo, kswapd, page reclaim
        file cache, anonymous memory, kernel allocators
```

---

## 5. Security: W^X Enforcement

ARM32's `map_lowmem()` sets up initial permissions, and later `mark_rodata_ro()` enforces W^X:

```
Stage 1 (paging_init → map_lowmem):
  Kernel text section:          RWX  (needed for self-modifying init code)
  Kernel data/BSS/init section: RW   (data)
  lowmem (non-kernel):          RW   (general RAM)

Stage 2 (mark_rodata_ro — called from kernel_init after module loading):
  Kernel text section:          R-X  (read + execute, no write)
  Kernel rodata section:        R--  (read only)
  Kernel data section:          RW-  (read/write, no execute)
  Kernel init section:          freed (init code reclaimed after use)
```

ARM64 enforces this more strictly via `CONFIG_ARM64_SW_TTBR0_PAN` and `CONFIG_RODATA_FULL_DEFAULT_ENABLED`.

---

## 6. System Design Q&A

**Q: Why does paging_init() call both bootmem_init() and the buddy allocator isn't ready until later?**
> `bootmem_init()` initializes the STRUCTURES of the buddy allocator — it creates `struct zone`, allocates `mem_map` (struct page array), and sets up the per-zone per-order free lists. But all pages are marked reserved (not added to the free lists). The buddy allocator's free lists are empty. Pages are added to the free lists by `mem_init()`, which is called from `start_kernel()` after `setup_arch()` returns. `mem_init()` walks the memblock free regions and calls `__free_pages()` for each free page — which adds them to the buddy allocator. This two-step approach ensures the zone structures are ready before any code tries to use them, even though allocations aren't possible until `mem_init()`.

**Q: What is sparse memory model and why does ARM use it?**
> The sparse memory model handles physical memory that is not contiguous — systems with memory holes or hot-pluggable memory. Instead of one giant `mem_map` array covering all PFNs from 0 to max_pfn (which would waste memory for holes), sparse memory divides physical memory into sections (128MB each). Each present section has its own `struct page` array. `pfn_to_page(pfn)` in the sparse model does: `mem_section[pfn_to_section_nr(pfn)]->section_mem_map[pfn & section_mask]`. ARM uses sparse because ARM SoCs often have memory holes (firmware-reserved regions at fixed addresses), and sparse avoids allocating struct pages for those holes.

**Q: What happens to the identity mapping from head.S after paging_init()?**
> head.S sets up a section mapping (1MB PMD entry) for the physical address range containing the kernel. This is an identity mapping (PA = VA) that lets the CPU continue running after MMU enable. During `paging_init()`, `prepare_page_table()` clears PMD entries for all VAs below `PAGE_OFFSET`. Since the identity mapping VA = PA (a physical address like 0x00008000) is far below `PAGE_OFFSET` (0xC0000000), `prepare_page_table()` clears it. The kernel survives this because by the time `prepare_page_table()` runs, it's executing from its PAGE_OFFSET VA (set by the branch in head.S to virtual address). No code runs from the identity mapping at that point.

**Q: What is TCM (Tightly-Coupled Memory) and when is tcm_init() used?**
> TCM is a small SRAM block (typically 4-64KB) embedded in some ARM processors, directly connected to the CPU pipeline without cache latency. It provides deterministic (zero-wait) access time — useful for real-time critical code (interrupt handlers, spin-wait loops). `tcm_init()` maps TCM into kernel virtual address space if `CONFIG_HAVE_TCM` is set and the hardware has TCM. ARM Cortex-M (microcontroller) series commonly has TCM. Most Cortex-A (application processor) SoCs do not have TCM, making `tcm_init()` a no-op.

**Q: How does paging_init() ensure the CPU's TLB is flushed after building new page tables?**
> `create_mapping()` (called by `map_lowmem()` and `devicemaps_init()`) operates on the page table data structure in memory. After all mappings are built, a TLB flush is needed so the CPU uses the new mappings. ARM32 uses `local_flush_tlb_all()` which executes a `mcr p15, 0, r0, c8, c7, 0` instruction (write to CP15 TLBIALL — TLB invalidate all). On ARM32 single-core systems during boot, only one core is running, so local flush is sufficient. On SMP ARM32, after secondary CPUs start, each secondary CPU calls `cpu_init()` which includes a TLB flush. The full TLB flush ensures the new page tables from `paging_init()` are active.
