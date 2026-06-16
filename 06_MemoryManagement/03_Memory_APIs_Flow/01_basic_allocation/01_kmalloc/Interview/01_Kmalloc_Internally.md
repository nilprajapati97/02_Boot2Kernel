# How `kmalloc()` Works Internally in the Linux Kernel on ARM64

`kmalloc()` is the kernel's general-purpose allocator for **small, physically contiguous** memory chunks. On ARM64, it sits on top of the **SLUB allocator** (default), which in turn sits on top of the **buddy (page) allocator**, which manages physical RAM described by the **memblock → zones → pages** hierarchy.

Let me walk through the full path, layer by layer.

---

## 1. The Big Picture (Layered View)

```
   Driver / Subsystem code
            │
            ▼
        kmalloc(size, flags)
            │
            ├── size ≤ KMALLOC_MAX_CACHE_SIZE (8 KB typical) ──► SLUB path
            │                                                     │
            │                                                     ▼
            │                                          kmalloc_caches[type][index]
            │                                          (kmem_cache_alloc)
            │
            └── size  > KMALLOC_MAX_CACHE_SIZE      ──► kmalloc_large
                                                          │
                                                          ▼
                                                 alloc_pages (buddy allocator)
                                                          │
                                                          ▼
                                                    Physical page frames
                                                    (linear-mapped region)
```

On ARM64, every page returned ultimately lives in the **linear map** (`PAGE_OFFSET`-based virtual region), so `kmalloc()`'d memory is:
- **Virtually contiguous**
- **Physically contiguous**
- **Already mapped** in the kernel page tables (no fault, no TLB miss penalty for setup)
- Suitable for **DMA** (you can call `virt_to_phys()` / `virt_to_dma()` safely)

---

## 2. The Size Decision — Which Path Does `kmalloc` Take?

When you call `kmalloc(size, GFP_KERNEL)`, the very first thing that happens is a **size classification**:

1. **Compile-time constant size** (most common in drivers):
   The compiler folds `kmalloc_index(size)` into a constant. The call collapses into a direct `kmem_cache_alloc(kmalloc_caches[type][index], flags)`.

2. **Runtime size**:
   `__kmalloc()` is invoked, which computes the index at runtime via `kmalloc_index()` / `kmalloc_slab()`.

3. **Large size** (greater than `KMALLOC_MAX_CACHE_SIZE`, typically 8 KB on ARM64 with 4 KB pages):
   Falls through to `kmalloc_large()` → `__get_free_pages()` → buddy allocator.

### The `kmalloc_caches[]` Table

The kernel pre-creates an array of slab caches at boot:

| Index | Size   | Cache name        |
|-------|--------|-------------------|
| 3     | 8 B    | kmalloc-8         |
| 4     | 16 B   | kmalloc-16        |
| 5     | 32 B   | kmalloc-32        |
| 6     | 64 B   | kmalloc-64        |
| 7     | 128 B  | kmalloc-128       |
| 8     | 256 B  | kmalloc-256       |
| 9     | 512 B  | kmalloc-512       |
| 10    | 1 KB   | kmalloc-1k        |
| 11    | 2 KB   | kmalloc-2k        |
| 12    | 4 KB   | kmalloc-4k        |
| 13    | 8 KB   | kmalloc-8k        |

There are also "odd" sizes 96 B and 192 B for better fit, and parallel tables for different **types**:
- `KMALLOC_NORMAL`     — default
- `KMALLOC_DMA`        — `GFP_DMA` (ZONE_DMA, low addresses for restricted devices)
- `KMALLOC_RECLAIM`    — reclaimable slabs
- `KMALLOC_CGROUP`     — memcg-accounted

Your `size + gfp_flags` together pick exactly one cache.

---

## 3. The SLUB Fast Path — Where ~99% of `kmalloc()` Calls Live

SLUB is the default slab allocator on modern ARM64 kernels. Each `kmem_cache` keeps **per-CPU pools** so allocation is mostly lock-free.

### Per-CPU Structure (`kmem_cache_cpu`)
For each CPU, the cache holds:
- **`freelist`** — pointer to the first free object in the currently active slab.
- **`page` (or `slab`)** — the current slab being allocated from.
- **`tid`** — a transaction ID used for lockless `cmpxchg_double` arbitration.

### Fast-Path Steps
1. Disable preemption (or use `this_cpu_ptr` + `cmpxchg_double`).
2. Read `c->freelist`. If non-NULL:
   - The next free object pointer is stored **inside the object itself** (the "free pointer" — at a known offset).
   - Atomically advance `freelist = *(void **)object` via `cmpxchg_double` using `tid` to detect preemption/migration.
   - Return the object.
3. Re-enable preemption.

This is **one cache line touch, no locks, no atomic RMW on a contended variable** — extremely fast (tens of cycles).

### Slow Path (`___slab_alloc`)
Triggered when the per-CPU freelist is empty:
1. Try the per-CPU **partial slab list** (`c->partial`) — promote one of those slabs to active.
2. If empty, go to the **per-node partial list** (`n->partial` under `n->list_lock`).
3. If still empty, call `new_slab()` → **buddy allocator** to grab one or more pages, format them into objects, install as the new active slab.

The new slab's pages come from `alloc_pages_node()`, which is the buddy entry point.

---

## 4. The Buddy Allocator (Page-Level Backing)

SLUB requests pages in **orders** (`2^order` contiguous pages). Each kmalloc cache has a configured `oo` (order + objects-per-slab) chosen at boot to balance internal fragmentation vs. atomicity.

The buddy allocator on ARM64:
- Manages each **zone** (`ZONE_DMA`, `ZONE_DMA32`, `ZONE_NORMAL`, sometimes `ZONE_MOVABLE`) per NUMA node.
- Maintains **11 free-lists** (orders 0 … `MAX_ORDER-1`, typically up to 10 → 4 MB blocks).
- Each free-list is further split by **migrate type** (`UNMOVABLE`, `MOVABLE`, `RECLAIMABLE`, …) to fight fragmentation.

### Allocation Flow Inside Buddy
1. **Per-CPU page cache (`pcp`)** — order-0 fast path; like SLUB's per-CPU pool but for single pages.
2. **Free-list of the requested order** — pop the first block.
3. **Higher-order split** — if the exact order has no block, find a larger one, split it, place the buddy halves back on lower-order lists.
4. **Watermark check** — `min`, `low`, `high` watermarks decide whether to wake `kswapd` or invoke direct reclaim.
5. **Direct reclaim / compaction / OOM** — only if `GFP_KERNEL` allows sleeping.

On success, the buddy returns a `struct page *`. SLUB then computes the virtual address from it.

---

## 5. From `struct page` to a Virtual Address (ARM64 Specific)

This is the part where ARM64 hardware/MMU specifics matter.

### The Linear (Direct) Map
On ARM64, all of system RAM is permanently mapped into the kernel virtual address space at a fixed offset — the **linear map**, starting at `PAGE_OFFSET`. With 48-bit VA and 4 KB pages, that's typically `0xFFFF_0000_0000_0000` (exact value depends on `CONFIG_ARM64_VA_BITS` and KASLR).

```
   phys_addr = page_to_pfn(page) << PAGE_SHIFT
   virt_addr = phys_to_virt(phys_addr)
             = __va(phys_addr)
             = (phys_addr - PHYS_OFFSET) + PAGE_OFFSET
```

So `kmalloc()` never has to **create** a mapping — it just hands you a pointer into the existing linear map. That's why it's fast and DMA-safe, and why it's limited to "small" sizes that can be physically contiguous.

### Page-Table Properties of the Linear Map
The linear map is built early in boot (`paging_init` → `map_mem`) using:
- **Block descriptors** (2 MB or 1 GB) where possible — fewer TLB entries, better performance.
- **Normal, Cacheable, Inner/Outer Write-Back, Shareable** memory attributes (via MAIR_EL1 indexes).
- **PXN = 1** (Privileged Execute-Never on the linear map for security; `CONFIG_RODATA_FULL_DEFAULT_ENABLED` may also down-map sections.).
- **AP = kernel R/W**, user no-access.

With features like `rodata=full`, the linear map can be **broken down to PTE granularity** so individual pages can have their permissions changed (used by `set_memory_ro()`, BPF JIT, modules, etc.).

### MMU / TLB Behavior on Access
When a driver dereferences the returned `kmalloc` pointer:
1. CPU issues a virtual access in EL1.
2. **TTBR1_EL1** is consulted (kernel VA range).
3. Walks PGD → PUD → PMD → PTE (or stops at PMD for a 2 MB block).
4. MAIR_EL1 + the descriptor's AttrIndx give the cacheability — normal cacheable for kmalloc memory.
5. Result cached in TLB; subsequent accesses are direct.

Because the linear map is huge-page-mapped, TLB pressure from `kmalloc` is minimal compared to `vmalloc` (which uses 4 KB PTEs).

---

## 6. GFP Flags — What They Actually Influence

The `gfp_t flags` argument controls three orthogonal things:

| Concern              | Common flags                                       |
|----------------------|---------------------------------------------------|
| **Where** (zone)     | `GFP_DMA`, `GFP_DMA32`, `GFP_HIGHUSER`            |
| **How** (behavior)   | `__GFP_NOWAIT`, `__GFP_ATOMIC`, `__GFP_DIRECT_RECLAIM`, `__GFP_IO`, `__GFP_FS` |
| **Accounting**       | `__GFP_ACCOUNT` (memcg), `__GFP_ZERO` (`kzalloc`) |

Composites you see in drivers:
- `GFP_KERNEL` — can sleep, can do I/O, normal zone. Default.
- `GFP_ATOMIC` — never sleeps; use in IRQ/softirq/spinlock context. Higher chance of failure under pressure.
- `GFP_NOIO`, `GFP_NOFS` — for code paths inside the I/O / FS stack to avoid recursion deadlocks.
- `GFP_DMA` — restricted to ZONE_DMA (devices with narrow addressing).

These flags propagate all the way down to the buddy allocator's watermark and reclaim decisions.

---

## 7. Object Layout Inside a SLUB Slab

A slab is one or more contiguous pages divided into equal-size objects. SLUB stores the **free-pointer inside the free object** (not in a separate metadata array, unlike SLAB).

```
 ┌────────────────────────────────────────────────────┐  <- slab page(s), order N
 │ obj 0 │ obj 1 │ obj 2 │ ... │ obj K-1 │ padding   │
 └────────────────────────────────────────────────────┘
   ▲       ▲       ▲
   │       │       └── free-ptr inside object → next free, or NULL
   │       └────────── allocated
   └────────────────── allocated
```

Additional metadata (red zones, poison patterns, allocation tracking) is inserted only when **`SLUB_DEBUG`** is enabled — useful for catching use-after-free, out-of-bounds writes. KASAN extends this with a shadow memory region.

The `struct page` (or in newer kernels `struct slab`) for a slab page carries:
- pointer back to its `kmem_cache`
- in-use object count
- the per-slab freelist head (when slab is on a partial list)

---

## 8. Boot-Time Setup on ARM64 (How `kmalloc` Becomes Available)

Order of operations during kernel boot:

1. **EL2 → EL1 drop**, MMU off.
2. `__create_page_tables` builds **identity map** + **early kernel map**.
3. MMU turned on; running in TTBR1.
4. `setup_arch()` parses DT/ACPI, calls `arm64_memblock_init()` → populates **memblock** (early allocator).
5. `paging_init()` → `map_mem()` builds the full **linear map** of all RAM with block mappings.
6. `bootmem_init()` → `zone_sizes_init()` initializes **zones** and `mem_map[]` (the `struct page` array).
7. `mm_init()` → `mem_init()` releases free memblock pages into the **buddy allocator**.
8. `kmem_cache_init()` → creates the **kmalloc_caches[]** array of slab caches.
9. `kmem_cache_init_late()` enables per-CPU partial lists.

After step 8, `kmalloc()` is fully usable. Before that, the kernel uses `memblock_alloc()`.

---

## 9. End-to-End Example: `kmalloc(128, GFP_KERNEL)` on ARM64

1. Compiler resolves `kmalloc_index(128) = 7` and `type = KMALLOC_NORMAL`.
2. Call becomes `kmem_cache_alloc(kmalloc_caches[NORMAL][7], GFP_KERNEL)` — the `kmalloc-128` cache.
3. Fast path: read this CPU's `kmem_cache_cpu`. `freelist` is non-NULL → take object, advance freelist with `cmpxchg_double`. Return.
4. Caller gets a pointer like `0xFFFF_8000_1234_5680` — somewhere in the linear map.
5. Dereferencing walks TTBR1 page tables; the access hits a cached 2 MB block descriptor → 1 TLB entry covers many such objects.
6. Memory is normal-cacheable, write-back, inner-shareable — full coherency with other CPUs via the CCI/CMN interconnect.
7. `kfree(ptr)` later pushes the object back onto the per-CPU freelist (again lockless).

If, instead, the per-CPU freelist had been empty:
- Slow path grabs a partial slab from the node list, or
- Calls `alloc_pages(GFP_KERNEL, oo_order)` to get fresh pages.
- The new pages already exist in the linear map; SLUB just **formats** them: walk the page, write each object's free-pointer to point at the next, set the last to NULL, mark the `struct page` as slab.

---

## 10. Key Properties to Remember (Interview-Worthy Summary)

| Property                      | `kmalloc` behavior on ARM64                                  |
|-------------------------------|--------------------------------------------------------------|
| Virtual contiguity            | Yes                                                          |
| Physical contiguity           | Yes                                                          |
| Max practical size            | `KMALLOC_MAX_CACHE_SIZE` (8 KB SLUB) for fast path; up to `MAX_ORDER` pages via large path |
| Backing region                | Kernel linear map (PAGE_OFFSET-based)                        |
| Page-table cost on allocation | Zero — mapping already exists                                |
| TLB-friendliness              | High — linear map uses block (2 MB/1 GB) descriptors         |
| DMA usability                 | Yes (with appropriate zone / `GFP_DMA*`)                     |
| Sleeping                      | Only if GFP flags allow (`GFP_KERNEL` may, `GFP_ATOMIC` not) |
| Underlying allocator          | SLUB (default) → buddy                                       |
| Lock contention               | Almost none on fast path (per-CPU, `cmpxchg_double`)         |
| Debugging aids                | `SLUB_DEBUG`, KASAN, KFENCE, kmemleak                        |

---

### Quick Contrast With Siblings

- **`kzalloc`** — `kmalloc` + `memset(0)`.
- **`vmalloc`** — virtually contiguous, physically scattered, separate vmalloc area, **uses PTE-granularity mappings** (heavy TLB, slower), can allocate huge sizes, **not DMA-safe**.
- **`alloc_pages`** — direct buddy, returns `struct page *`, page-granular only.
- **`dma_alloc_coherent`** — DMA-API; may use CMA, may set non-cacheable attributes, returns both a CPU VA and a DMA bus address.

---

This is the full conceptual picture. Your workspace already has a 01_basic_allocation/01_kmalloc folder — when you're ready, the same structure used for `kzalloc` (02_internals.md, 03_arm64_callflow.md, 04_memory_map.md) would be a natural place to capture this for `kmalloc` itself.
