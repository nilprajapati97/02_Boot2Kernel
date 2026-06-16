# Layer 9 — SLUB, vmalloc & Runtime Allocation

[← Layer 8: Buddy Allocator Handoff](08_Buddy_Allocator_Handoff.md) | **Layer 9** | [Appendix & Reference →](10_Appendix_Reference.md)

---

## What This Layer Covers

The buddy allocator is alive — `alloc_pages()` works. But the buddy system only deals
in whole pages (4 KB minimum). The kernel needs to allocate objects of arbitrary sizes
(32 bytes for a `struct dentry`, 640 bytes for a `struct inode`, etc.). This layer
explains the final two allocators that bootstrap on top of the buddy system: the **SLUB
slab allocator** (for small objects) and **vmalloc** (for virtually contiguous multi-page
allocations). After this layer, the kernel's memory subsystem is fully operational and
ready for both kernel and userspace use.

---

## 1. The Allocator Stack

```
┌─────────────────────────────────────────────────────────────────┐
│                    USER SPACE                                    │
│  malloc() / mmap() / brk()                                      │
│  └─► syscall → kernel handles via page fault                    │
├─────────────────────────────────────────────────────────────────┤
│                                                                 │
│  ┌─────────────┐  ┌─────────────┐  ┌──────────────────────┐    │
│  │   kmalloc()  │  │   vmalloc() │  │   alloc_pages()      │    │
│  │   kfree()    │  │   vfree()   │  │   __free_pages()     │    │
│  │             │  │             │  │                      │    │
│  │  Small obj: │  │  Virtually  │  │  Physical pages      │    │
│  │  8B - 8KB+  │  │  contiguous │  │  (power-of-2 blocks) │    │
│  │             │  │  Multi-page │  │                      │    │
│  └──────┬──────┘  └──────┬──────┘  └──────────┬───────────┘    │
│         │                │                     │                │
│         ▼                ▼                     │                │
│  ┌─────────────┐  ┌─────────────┐              │                │
│  │ SLUB Slab   │  │   vmap      │              │                │
│  │ Allocator   │  │   (map pages│              │                │
│  │             │  │    into VA)  │              │                │
│  └──────┬──────┘  └──────┬──────┘              │                │
│         │                │                     │                │
│         ▼                ▼                     ▼                │
│  ┌──────────────────────────────────────────────────────────┐   │
│  │              BUDDY PAGE ALLOCATOR                        │   │
│  │     alloc_pages() → zone free lists → physical pages     │   │
│  └──────────────────────────────────────────────────────────┘   │
│                                                                 │
│                    KERNEL SPACE                                  │
└─────────────────────────────────────────────────────────────────┘
```

---

## 2. SLUB Slab Allocator Bootstrap — `kmem_cache_init()`

**Source**: `mm/slub.c`

### 2.1 The Chicken-and-Egg Problem

```
┌──────────────────────────────────────────────────────────────────┐
│  SLUB needs to allocate struct kmem_cache to describe a cache.  │
│  But allocating struct kmem_cache requires... SLUB!              │
│                                                                  │
│  SLUB also needs struct kmem_cache_node (per-NUMA-node slab      │
│  metadata). Allocating that also requires SLUB!                  │
│                                                                  │
│  Solution: Bootstrap with STATIC structures on the stack,        │
│  then replace them with dynamically allocated ones.              │
└──────────────────────────────────────────────────────────────────┘
```

### 2.2 Slab State Machine

SLUB bootstraps through four states:

```
slab_state transitions:
═══════════════════════

  DOWN     → No slab functionality at all.
             Only memblock_alloc() and alloc_pages() work.

  PARTIAL  → kmem_cache_node cache is ready.
             Per-node slab lists can be allocated.

  UP       → All kmalloc caches are ready.
             kmalloc() works for all sizes.

  FULL     → Everything working, including flush workqueues
             and debug features.
```

### 2.3 The Bootstrap Sequence

```
kmem_cache_init()                                  [mm/slub.c]
    │
    │   slab_state = DOWN
    │
    ├──► 1. Create static boot caches ON THE STACK
    │       static struct kmem_cache boot_kmem_cache;
    │       static struct kmem_cache boot_kmem_cache_node;
    │       kmem_cache      = &boot_kmem_cache;
    │       kmem_cache_node = &boot_kmem_cache_node;
    │
    ├──► 2. Initialize NUMA node masks
    │
    ├──► 3. create_boot_cache(kmem_cache_node,
    │                         "kmem_cache_node",
    │                         sizeof(struct kmem_cache_node), ...)
    │       Create the cache that allocates kmem_cache_node objects.
    │       This uses alloc_pages() directly from the buddy allocator
    │       (since slab isn't ready yet, the cache bootstraps itself).
    │
    ├──► 4. slab_state = PARTIAL ★
    │       Now kmem_cache_node objects can be allocated via slab!
    │       Per-node slab metadata works.
    │
    ├──► 5. create_boot_cache(kmem_cache,
    │                         "kmem_cache",
    │                         sizeof(struct kmem_cache), ...)
    │       Create the cache that allocates kmem_cache objects.
    │
    ├──► 6. bootstrap(&boot_kmem_cache)
    │       The self-referential bootstrap:
    │       ├── Allocate a new struct kmem_cache FROM the cache itself
    │       │   (the cache for kmem_cache can now allocate its own descriptor!)
    │       ├── Copy boot_kmem_cache → new dynamically allocated one
    │       ├── Update all slab pages to point to the new cache
    │       └── kmem_cache = new_cache (replace the static one)
    │
    ├──► 7. bootstrap(&boot_kmem_cache_node)
    │       Same self-referential bootstrap for kmem_cache_node.
    │
    ├──► 8. create_kmalloc_caches()
    │       Create all the standard kmalloc caches:
    │       ┌────────────────────────────────────────────┐
    │       │  kmalloc-8        (8 bytes)                │
    │       │  kmalloc-16       (16 bytes)               │
    │       │  kmalloc-32       (32 bytes)               │
    │       │  kmalloc-64       (64 bytes)               │
    │       │  kmalloc-96       (96 bytes)               │
    │       │  kmalloc-128      (128 bytes)              │
    │       │  kmalloc-192      (192 bytes)              │
    │       │  kmalloc-256      (256 bytes)              │
    │       │  kmalloc-512      (512 bytes)              │
    │       │  kmalloc-1k       (1024 bytes)             │
    │       │  kmalloc-2k       (2048 bytes)             │
    │       │  kmalloc-4k       (4096 bytes)             │
    │       │  kmalloc-8k       (8192 bytes)             │
    │       │  ... and variants: kmalloc-cg-*, kmalloc-rcl-* │
    │       └────────────────────────────────────────────┘
    │
    │       slab_state = UP ★
    │       kmalloc() now works for ANY size!
    │
    └──► 9. init_freelist_randomization()
            Randomize slab freelists for security
            (makes use-after-free exploitation harder).
```

### 2.4 How SLUB Works (Runtime)

```
kmalloc(128, GFP_KERNEL)
    │
    ├── Find the right cache: kmalloc_caches[7] = "kmalloc-128"
    │
    └── slab_alloc(cache, gfp, ...)
        │
        ├── FAST PATH (most common):
        │     The current CPU has a "freelist" pointer:
        │     cpu_slab->freelist → object → object → object → NULL
        │     Take the first object. No locks needed!
        │
        ├── SLOW PATH (freelist empty):
        │     ├── Try the current slab page's freelist
        │     ├── Try partial slabs on this CPU's partial list
        │     ├── Try partial slabs on the node's partial list
        │     └── Allocate a NEW slab page:
        │           alloc_pages(gfp, cache->oo.order)
        │           → Gets a page from buddy allocator
        │           → Carve into (PAGE_SIZE/128) = 32 objects
        │           → Set up freelist linking all 32 objects
        │           → Return first object
        │
        └── Return pointer to 128-byte object


SLUB Slab Page Layout (for kmalloc-128):
═════════════════════════════════════════

  ┌──────┬──────┬──────┬──────┬──────┬──────┬──────┬──────┐
  │ obj0 │ obj1 │ obj2 │ obj3 │ ...  │obj30 │obj31 │unused│
  │128 B │128 B │128 B │128 B │      │128 B │128 B │      │
  └──┬───┴──┬───┴──┬───┴──────┴──────┴──────┴──────┴──────┘
     │      │      │
  freelist chain: obj0 → obj1 → obj2 → ... → NULL
  (each object's first word points to the next free object)
```

---

## 3. `vmalloc_init()` — Virtual Contiguous Allocator

**Source**: `mm/vmalloc.c`

### 3.1 Why vmalloc?

```
┌──────────────────────────────────────────────────────────────────┐
│  alloc_pages() gives you PHYSICALLY contiguous pages.            │
│  But sometimes physical contiguity isn't needed — only VIRTUAL   │
│  contiguity matters (e.g., loading a kernel module into memory). │
│                                                                  │
│  vmalloc() allocates pages from any physical location and maps   │
│  them into a contiguous virtual address range in the VMALLOC     │
│  region of the kernel VA space.                                  │
│                                                                  │
│  Advantage: can allocate large regions even when RAM is           │
│  fragmented. No need for physically contiguous memory.           │
│                                                                  │
│  Disadvantage: slower than kmalloc (needs page table setup),     │
│  TLB pressure (no block mappings).                               │
└──────────────────────────────────────────────────────────────────┘
```

### 3.2 The Initialization

```
vmalloc_init()                                    [mm/vmalloc.c]
    │
    ├──► Create vmap_area_cachep
    │       KMEM_CACHE(vmap_area, SLAB_PANIC)
    │       Slab cache for struct vmap_area descriptors.
    │       (Needs SLUB → called after kmem_cache_init)
    │
    ├──► Initialize per-CPU vmap_block_queue structures
    │       Per-CPU queues for small vmalloc allocations.
    │
    ├──► vmap_init_nodes()
    │       Set up vmap node data structures for managing
    │       the virtual address space tree.
    │
    ├──► Import existing vmlist entries
    │       Early ioremap areas (from before vmalloc_init) are
    │       imported into the vmap red-black tree.
    │
    ├──► vmap_init_free_space()
    │       Initialize free virtual address space tracking.
    │       The entire VMALLOC_START → VMALLOC_END range is
    │       registered as free VA space.
    │
    ├──► vmap_initialized = true ★
    │       vmalloc() and ioremap() now work!
    │
    └──► Register vmap shrinker
            For reclaiming unused vmap areas under memory pressure.
```

### 3.3 How vmalloc Works (Runtime)

```
vmalloc(65536)    // Allocate 64 KB (16 pages)
    │
    ├── 1. Find free VA range in VMALLOC space
    │       Allocate a struct vmap_area describing the range.
    │       Search the free space tree for 16 contiguous pages.
    │       → VA = 0xFFFF_xxxx_xxxx_xxxx (somewhere in VMALLOC)
    │
    ├── 2. Allocate 16 physical pages (NOT contiguous)
    │       for (i = 0; i < 16; i++)
    │           pages[i] = alloc_page(GFP_KERNEL)
    │       These pages may come from completely different PFNs!
    │
    ├── 3. Map pages into the VA range
    │       For each page, create a PTE in swapper_pg_dir:
    │         VA + 0*PAGE_SIZE → pages[0]
    │         VA + 1*PAGE_SIZE → pages[1]
    │         ...
    │         VA + 15*PAGE_SIZE → pages[15]
    │
    └── 4. Return the virtual address
            Caller sees a contiguous 64 KB region at VA.


Physical Memory:          Kernel Virtual Address Space:

  pages[0] at PFN 0x50000    VMALLOC region:
  pages[1] at PFN 0x82000    ┌──────────────────┐
  pages[2] at PFN 0x41000    │  VA + 0:  PFN 50 │
  ...                        │  VA + 4K: PFN 82 │
  pages[15] at PFN 0x93000   │  VA + 8K: PFN 41 │
                             │  ...              │
  Physically scattered!      │  VA + 60K:PFN 93 │
                             └──────────────────┘
                             Virtually contiguous!
```

---

## 4. Post-Initialization: Final Boot Steps

### 4.1 `kmem_cache_init_late()`

```
kmem_cache_init_late()
    ├── Set up the flush workqueue (for asynchronous cache flushing)
    ├── slab_state = FULL ★
    └── Slab allocator is now fully operational.
```

### 4.2 `page_alloc_init_late()` — Deferred Init & Cleanup

```
page_alloc_init_late()                            [mm/mm_init.c]
    │
    ├──► Deferred struct page initialization
    │       If CONFIG_DEFERRED_STRUCT_PAGE_INIT:
    │       Spawn parallel kthreads (one per NUMA node) to
    │       initialize remaining struct pages that were deferred
    │       during boot for speed. Wait for completion.
    │
    ├──► mem_init_print_info()
    │       Print the famous memory summary to dmesg:
    │       "Memory: xxxxK/yyyyK available (zK code, wK data...)"
    │
    ├──► memblock_discard() ★
    │       FREE the memblock data structures themselves.
    │       The memblock region arrays are no longer needed —
    │       all memory management is now through the buddy system.
    │       The arrays go back to the buddy allocator as free pages.
    │
    ├──► shuffle_free_memory()
    │       Randomize the order of free pages for security.
    │
    └──► set_zone_contiguous()
            Mark zones as contiguous if all pageblocks are present.
```

### 4.3 Other Memory-Related Init in `start_kernel()`

```
After mm_core_init():
    │
    ├── setup_per_cpu_pageset()     Tune per-CPU page cache batch/high
    ├── numa_policy_init()          Default NUMA memory policy
    ├── anon_vma_init()             Slab cache for anonymous VMAs
    ├── fork_init()                 Slab cache for task_struct
    ├── proc_caches_init()          Slab caches for process structures
    │   ├── sighand_cache           signal handlers
    │   ├── signal_cache            signal struct
    │   ├── files_cache             file descriptor table
    │   ├── fs_cache                filesystem struct
    │   ├── mm_cache                mm_struct
    │   └── vm_area_cache           vm_area_struct
    ├── vfs_caches_init()           Slab caches for VFS
    │   ├── dentry cache            directory entries
    │   └── inode cache             inodes
    └── pagecache_init()            Page cache initialization
```

---

## 5. How Userspace Gets Memory

Once the system is fully booted and userspace processes are running:

### 5.1 The `mmap()` → Page Fault → Buddy Flow

```
User process calls malloc(4096):
    │
    ├── C library (glibc) calls mmap() or brk() syscall
    │
    ├── Kernel: create a vm_area_struct (VMA)
    │       Describes the virtual address range.
    │       NO physical memory allocated yet!
    │       (Lazy allocation — "demand paging")
    │
    ├── User accesses the allocated address
    │       → CPU raises a PAGE FAULT (no PTE exists)
    │
    ├── Kernel: handle_mm_fault()
    │   ├── Walk process page table (TTBR0)
    │   ├── Reach empty PTE → need a physical page
    │   │
    │   └── alloc_pages(GFP_HIGHUSER_MOVABLE, 0)
    │       └── Buddy allocator: take a page from
    │           ZONE_NORMAL, MIGRATE_MOVABLE free list
    │
    ├── Kernel: set PTE in process page table
    │       TTBR0 page table: VA → PA of the new page
    │
    └── User process resumes
        The access succeeds. The page is now mapped.
```

### 5.2 Kernel Object Allocation Flow

```
Kernel needs a struct inode (say, 600 bytes):
    │
    ├── kmem_cache_alloc(inode_cache, GFP_KERNEL)
    │   └── SLUB: take an object from the inode slab cache
    │       ├── Fast: return from per-CPU freelist
    │       └── Slow: allocate new slab page from buddy
    │
    └── Returns pointer to 600-byte object in kernel VA space
        (within the linear map, since slab pages are linear-mapped)


Kernel needs 256 KB for a module:
    │
    ├── vmalloc(256 * 1024)
    │   ├── Find free VA in VMALLOC region
    │   ├── Allocate 64 pages from buddy (any PFN)
    │   ├── Map into VMALLOC VA via page tables
    │   └── Return VMALLOC VA pointer
    │
    └── Module code loaded at this virtually contiguous address
```

---

## 6. Complete Allocator Summary

| Allocator | API | Size Range | Physically Contiguous? | When Ready |
|-----------|-----|-----------|----------------------|-----------|
| **memblock** | `memblock_alloc()` | Any | Yes | Before buddy (boot only) |
| **Buddy** | `alloc_pages()` | 4 KB – 4 MB (order 0-10) | Yes | After `memblock_free_all()` |
| **SLUB** | `kmalloc()`, `kmem_cache_alloc()` | 8 B – 8+ KB | Yes (within slab page) | After `kmem_cache_init()` |
| **vmalloc** | `vmalloc()` | Any (multi-page) | No (virtually contiguous) | After `vmalloc_init()` |
| **CMA** | `dma_alloc_coherent()` | Large contiguous | Yes | After CMA setup |
| **Per-CPU** | `alloc_percpu()` | Any | N/A (per-CPU copy) | After `setup_per_cpu_areas()` |

---

## 7. State After Layer 9

```
┌────────────────────────────────────────────────────────────────┐
│           ★ MEMORY SUBSYSTEM FULLY OPERATIONAL ★               │
│                                                                │
│  Page Allocator (Buddy):                                       │
│    ✓ alloc_pages() / __get_free_pages() — works                │
│    ✓ Per-CPU page caches (PCP) — active                        │
│    ✓ Zone watermarks computed and kswapd scheduled              │
│    ✓ Compaction available for large allocations                 │
│                                                                │
│  Slab Allocator (SLUB):                                        │
│    ✓ kmalloc() / kfree() — works for all sizes                 │
│    ✓ kmem_cache_create() — custom caches can be created         │
│    ✓ slab_state = FULL                                         │
│                                                                │
│  vmalloc:                                                      │
│    ✓ vmalloc() / vfree() — works                               │
│    ✓ ioremap() — works (uses vmalloc VA space)                 │
│    ✓ vmap() — works (map existing pages into vmalloc VA)       │
│                                                                │
│  Virtual Memory:                                               │
│    ✓ swapper_pg_dir complete (linear map + kernel image)        │
│    ✓ Per-process page tables (TTBR0) managed via mm_struct      │
│    ✓ Page fault handler active (handle_mm_fault)                │
│    ✓ Demand paging for userspace                                │
│                                                                │
│  memblock:                                                     │
│    ✗ DISCARDED — data structures freed back to buddy            │
│                                                                │
│  The kernel can now:                                            │
│    • Allocate any size of memory (kmalloc, vmalloc, alloc_pages)│
│    • Create processes with their own VA space (fork)            │
│    • Load kernel modules (vmalloc)                              │
│    • Map device MMIO (ioremap)                                  │
│    • Handle page faults and demand paging                       │
│    • Reclaim memory under pressure (kswapd, direct reclaim)     │
│    • Everything needed for a running Linux system               │
└────────────────────────────────────────────────────────────────┘
```

---

## Summary: What Happened in Layer 9

| Step | Function | What It Did |
|------|----------|-------------|
| 1 | `kmem_cache_init()` | Bootstrapped SLUB slab allocator (DOWN → PARTIAL → UP) |
| 2 | `vmalloc_init()` | Initialized vmalloc VA space management |
| 3 | `kmem_cache_init_late()` | Finalized slab (UP → FULL) |
| 4 | `page_alloc_init_late()` | Deferred page init, memblock discard |
| 5 | Process cache creation | Slab caches for task_struct, mm_struct, VMA, etc. |
| 6 | VFS cache creation | Slab caches for dentries, inodes, file objects |

**The memory subsystem is now fully operational**. From this point forward, all memory
allocation uses the buddy → SLUB → vmalloc stack. The system is ready to run userspace.

---

[← Layer 8: Buddy Allocator Handoff](08_Buddy_Allocator_Handoff.md) | **Layer 9** | [Appendix & Reference →](10_Appendix_Reference.md)
