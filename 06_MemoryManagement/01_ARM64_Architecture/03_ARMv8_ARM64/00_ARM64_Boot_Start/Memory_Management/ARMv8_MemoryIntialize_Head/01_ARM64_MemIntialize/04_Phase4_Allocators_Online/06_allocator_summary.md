# Allocator Summary — Complete Memory Initialization Reference

**Purpose:** Unified reference for the entire ARM64 Linux kernel memory initialization
**Scope:** From power-on to fully operational memory management

---

## Allocator Timeline

```
Time ──────────────────────────────────────────────────────────────►

Phase 1: Assembly Boot
│ No allocator
│ Bump allocator in create_init_idmap (page table pages only)
│ All data in BSS or registers
│
├── MMU ON ──────────────────────────────────────────────────────────

Phase 2: setup_arch() / Memblock
│ MEMBLOCK becomes active
│ ├── memblock_add() — register RAM from DTB
│ ├── memblock_reserve() — reserve kernel, DTB, initrd
│ ├── memblock_alloc() — allocate page table pages
│ └── memblock_alloc() — allocate various boot structures
│
├── Linear map created (all RAM accessible) ─────────────────────────

Phase 3: Zone & Node Init
│ MEMBLOCK still active
│ ├── memblock_alloc() — struct page arrays (largest allocation!)
│ ├── memblock_alloc() — zone/node structures
│ └── memblock_alloc() — pageblock bitmaps
│
├── Zone structures ready (but empty) ───────────────────────────────

Phase 4: Allocators Online
│ SLUB bootstrap (kmem_cache_init)
│ ├── Static boot caches → create kmalloc caches
│ │
│ memblock_free_all() ← THE TRANSITION
│ ├── All free memblock pages → buddy free lists
│ │
│ BUDDY ALLOCATOR active
│ ├── alloc_pages() works
│ │
│ SLUB fully online
│ ├── kmalloc() works
│ │
│ vmalloc_init()
│ ├── vmalloc() works
│ │
│ ALL ALLOCATORS OPERATIONAL ────────────────────────────────────────
```

---

## Allocator Hierarchy (Runtime)

```
User/Kernel Request
       │
       ├── Small object (8B - 8KB)
       │   └── kmalloc() / kmem_cache_alloc()
       │       └── SLUB Slab Allocator
       │           └── alloc_pages() when slab needs new page
       │               └── Buddy Allocator
       │
       ├── Full pages (4KB - 4MB)
       │   └── alloc_pages() / __get_free_pages()
       │       └── Buddy Allocator
       │           ├── Per-CPU cache (order 0-3, lockless)
       │           └── Zone free_area[] (higher orders, zone lock)
       │
       ├── Large virtually-contiguous
       │   └── vmalloc()
       │       ├── alloc_vmap_area() — find VA range (RB-tree)
       │       ├── alloc_pages() × N — get physical pages (buddy)
       │       └── vmap_pages_range() — create page table entries
       │
       └── Contiguous physical (DMA)
           └── cma_alloc() / dma_alloc_coherent()
               └── Migrate movable pages out of CMA region
                   └── Buddy allocator (MIGRATE_CMA type)
```

---

## Address Space Layout (ARM64, 48-bit VA)

```
0xFFFF_FFFF_FFFF_FFFF ┐
                       │ Fixmap                            (~8 MB)
0xFFFF_FFFE_0000_0000 ┤
                       │ PCI I/O space                     (~16 MB)
0xFFFF_FFFD_0000_0000 ┤
                       │ vmemmap (struct page arrays)      (~variable)
0xFFFF_FFFC_0000_0000 ┤
                       │ vmalloc / vmap / ioremap          (~127 TB)
0xFFFF_FF80_0800_0000 ┤
                       │ Modules                           (~128 MB)
0xFFFF_FF80_0000_0000 ┤
                       │ Kernel image (text, data, BSS)    (~32 MB)
0xFFFF_FF80_0000_0000 ┤
                       │
                       │ ╔════════════════════════════════╗
                       │ ║     LINEAR MAP                 ║
                       │ ║     (all physical RAM)         ║  (~128 TB)
                       │ ║                                ║
                       │ ║  __va(phys) = phys - memstart  ║
                       │ ║              + PAGE_OFFSET     ║
                       │ ╚════════════════════════════════╝
                       │
0xFFFF_0000_0000_0000 ┘ PAGE_OFFSET

─── User/Kernel boundary ───

0x0000_FFFF_FFFF_FFFF ┐
                       │ User space                        (256 TB)
0x0000_0000_0000_0000 ┘
```

---

## Key Data Structures Summary

| Structure | Count | Size | Purpose |
|-----------|-------|------|---------|
| `struct page` | 1 per 4KB physical page | 64 bytes | Page metadata (flags, refcount, lru, etc.) |
| `pg_data_t` | 1 per NUMA node | ~4 KB | Node descriptor (zones, kswapd, stats) |
| `struct zone` | 3-4 per node | ~2 KB each | Zone allocator state (free lists, watermarks) |
| `struct free_area` | 11 per zone | ~100 bytes | Per-order free lists (orders 0-10) |
| `struct kmem_cache` | ~200 at runtime | ~512 bytes | Slab cache descriptor |
| `struct vmap_area` | 1 per vmalloc | ~64 bytes | vmalloc VA range tracking |
| `struct memblock` | 1 global | ~32 bytes | Boot allocator (retired after Phase 4) |

---

## GFP Flags Quick Reference

| Flag | Zone | Behavior |
|------|------|----------|
| `GFP_KERNEL` | NORMAL→DMA32→DMA | May sleep, may reclaim, standard allocation |
| `GFP_ATOMIC` | NORMAL→DMA32→DMA | Cannot sleep, uses reserves, interrupt-safe |
| `GFP_DMA` | DMA only | For devices with limited DMA addressing |
| `GFP_DMA32` | DMA32→DMA | For 32-bit DMA devices |
| `GFP_HIGHUSER_MOVABLE` | MOVABLE→NORMAL→... | User pages, can be migrated/compacted |
| `GFP_NOWAIT` | NORMAL→... | Cannot sleep, no reclaim, fails quickly |
| `__GFP_NOFAIL` | (modifier) | Never return NULL, loops until success |
| `__GFP_ZERO` | (modifier) | Zero-fill allocated memory |
| `__GFP_COMP` | (modifier) | Create compound page (for huge pages) |

---

## Key Source Files

| File | What |
|------|------|
| `arch/arm64/kernel/head.S` | Assembly boot, initial page tables, MMU enable |
| `arch/arm64/kernel/setup.c` | `setup_arch()` orchestrator |
| `arch/arm64/mm/mmu.c` | `paging_init()`, `map_mem()`, page table creation |
| `arch/arm64/mm/init.c` | `arm64_memblock_init()`, `bootmem_init()` |
| `mm/memblock.c` | Memblock allocator, `memblock_free_all()` |
| `mm/mm_init.c` | `mm_core_init()`, `free_area_init()` |
| `mm/page_alloc.c` | Buddy allocator: `__alloc_pages()`, `__free_pages()` |
| `mm/slub.c` | SLUB slab allocator: `kmem_cache_init()`, `kmalloc()` |
| `mm/vmalloc.c` | vmalloc: `vmalloc_init()`, `vmalloc()`, `vfree()` |
| `mm/sparse.c` | SPARSEMEM: `sparse_init()`, vmemmap |
| `include/linux/mmzone.h` | Zone/node/page data structures |
| `include/linux/gfp.h` | GFP flags and zone selection |

---

## Memory Initialization Checklist

```
□ Phase 1: Assembly Boot
  □ BSS zeroed
  □ Identity map created (id_pg_dir)
  □ init_pg_dir created (kernel image mapped)
  □ MMU enabled
  □ Virtual addressing active (TTBR1 = init_pg_dir)
  □ Stack active (init_task.stack)
  □ Jump to start_kernel()

□ Phase 2: setup_arch / Memblock
  □ Fixmap initialized (for DTB access)
  □ DTB parsed → memblock_add() for each RAM bank
  □ DTB, kernel, initrd reserved in memblock
  □ memstart_addr calculated (linear map base)
  □ Memory trimmed to VA/PA limits
  □ Linear map created in swapper_pg_dir
  □ TTBR1 switched: init_pg_dir → swapper_pg_dir
  □ Zone boundaries calculated
  □ CMA reserved
  □ NUMA topology determined

□ Phase 3: Zone & Node Init
  □ HugeTLB pre-allocated
  □ sparse_init(): struct page arrays allocated, vmemmap mapped
  □ free_area_init(): zones initialized, free lists empty
  □ Pageblock bitmaps allocated
  □ All struct pages marked PageReserved

□ Phase 4: Allocators Online
  □ SLUB bootstrapped (boot_kmem_cache → self-hosting)
  □ kmalloc caches created (kmalloc-8 through kmalloc-8k)
  □ memblock_free_all() → all free pages to buddy
  □ Zone watermarks calculated
  □ Per-CPU page caches initialized
  □ vmalloc_init() → VA space management ready
  □ ALL ALLOCATORS OPERATIONAL
```

---

## Quick API Reference

```c
// Page allocator (buddy)
struct page *alloc_pages(gfp_t gfp, unsigned int order);
void __free_pages(struct page *page, unsigned int order);
unsigned long __get_free_pages(gfp_t gfp, unsigned int order);
void free_pages(unsigned long addr, unsigned int order);

// Slab allocator (SLUB)
void *kmalloc(size_t size, gfp_t flags);
void kfree(const void *objp);
struct kmem_cache *kmem_cache_create(const char *name, size_t size, ...);
void *kmem_cache_alloc(struct kmem_cache *s, gfp_t flags);
void kmem_cache_free(struct kmem_cache *s, void *x);

// vmalloc
void *vmalloc(unsigned long size);
void vfree(const void *addr);
void *vmap(struct page **pages, unsigned int count, ...);
void vunmap(const void *addr);

// Conversion
struct page *pfn_to_page(unsigned long pfn);
unsigned long page_to_pfn(struct page *page);
void *page_address(struct page *page);  // Linear map VA
phys_addr_t page_to_phys(struct page *page);
```

---

## Key Takeaways

1. **Four phases, four capabilities**: boot mapping → memblock → zones → full allocators
2. **Memblock is temporary** — it exists only to bootstrap the permanent allocators
3. **The linear map is fundamental** — `__va()` and `__pa()` are used thousands of times per second
4. **SLUB sits on buddy** — objects within pages (SLUB) vs. whole pages (buddy)
5. **vmalloc adds virtual flexibility** — when physical contiguity isn't needed
6. **struct page is everywhere** — 64 bytes per 4KB page, ~1.5% of RAM, accessed constantly
7. **GFP flags are the API** — they encode zone preference, sleeping ability, and reclaim behavior
