# `kmem_cache_init` — Deep Questions & Answers: From Scratch to Expert Level

> **Format**: Each question answered from first principles, building understanding layer by layer.  
> **Level**: Nvidia Senior Kernel Engineer / Principal Engineer Interview  
> **Cross-Reference**: `01_DESIGN_CODE_WALKTHROUGH.md` (code details), `02_SYSTEM_DESIGN_MMU_ARM.md` (ARM specifics)

---

## Table of Contents

**Foundations**
- [Q1: What is the SLUB allocator and how does it differ from SLAB and SLOB?](#q1)
- [Q2: What problem does kmem_cache_init solve?](#q2)
- [Q3: What is a "slab" and how does it relate to a page?](#q3)
- [Q4: What are the key fields of struct kmem_cache and what does each track?](#q4)
- [Q5: What is the slab allocator's position in the overall memory hierarchy?](#q5)

**Bootstrap Sequence**
- [Q6: Explain the chicken-and-egg bootstrap problem in kmem_cache_init](#q6)
- [Q7: What role does `static __initdata` play? What happens to those objects later?](#q7)
- [Q8: What does the `bootstrap()` function do and why must back-pointers be patched?](#q8)
- [Q9: What is `slab_state` and why does its transition order matter?](#q9)
- [Q10: Why must `memblock_free_all()` run before `kmem_cache_init()`?](#q10)

**Object Layout**
- [Q11: How does SLUB decide where to put the free pointer inside an object?](#q11)
- [Q12: Walk through `calculate_order()` — how does SLUB pick the page order for a cache?](#q12)
- [Q13: What is `SLAB_HWCACHE_ALIGN` and how does it prevent false sharing?](#q13)
- [Q14: What is `reciprocal_size` and why is division avoided in the hot path?](#q14)
- [Q15: How does `object_size` differ from `size` in struct kmem_cache?](#q15)

**NUMA and Per-CPU**
- [Q16: How does SLUB handle NUMA topology for memory allocation?](#q16)
- [Q17: What is a `kmem_cache_node` and what does its partial list contain?](#q17)
- [Q18: What are "sheaves" in modern SLUB and why were they introduced?](#q18)
- [Q19: How does the per-CPU sheaves fast path avoid locking?](#q19)
- [Q20: What is the `node_barn` and how does it interact with sheaves?](#q20)

**kmalloc**
- [Q21: How does `kmalloc()` find the right cache for a given size?](#q21)
- [Q22: Why do both 96-byte and 128-byte kmalloc caches exist?](#q22)
- [Q23: What are the four kmalloc cache types and when is each used?](#q23)
- [Q24: What happens when `kmalloc()` is called before `kmem_cache_init()` completes?](#q24)

**Debugging and Security**
- [Q25: How does SLUB_DEBUG change object layout?](#q25)
- [Q26: What is `SLAB_FREELIST_HARDENED` and how does it prevent heap attacks?](#q26)
- [Q27: What is `SLAB_FREELIST_RANDOM` and why does it matter?](#q27)
- [Q28: How does KASAN integrate with the slab allocator?](#q28)

**Advanced**
- [Q29: What is `SLAB_TYPESAFE_BY_RCU` and how does it differ from regular RCU?](#q29)
- [Q30: How does CPU hotplug interact with slab caches?](#q30)
- [Q31: Why is `kmem_cache_init_late` split from `kmem_cache_init`?](#q31)
- [Q32: How does memory pressure trigger slab shrinking?](#q32)

---

## Foundations

---

<a name="q1"></a>
## Q1: What is the SLUB allocator and how does it differ from SLAB and SLOB?

### Starting from Scratch

The Linux kernel needs to allocate many small objects of varying sizes — think `struct task_struct` for processes, `struct sk_buff` for network packets, and `struct inode` for filesystem entries. Using `alloc_pages()` (the buddy allocator) for each of these would be catastrophically wasteful — a 4KB page for a 80-byte inode means 98% waste.

The **slab allocator** is the kernel's solution. It takes pages from the buddy allocator and carves them into fixed-size objects. When you need a `struct task_struct`, you ask the slab allocator's `task_struct` cache, which has pre-carved pages full of exactly-sized slots.

### The Three Implementations

Linux has had three slab allocator implementations:

#### SLAB (1994, pre-2.6.23)

The original, designed by Jeff Bonwick at Sun Microsystems and ported to Linux. Key design:
- **Per-CPU object cache**: Each CPU has a list of freed objects ready to reuse
- **Per-slab coloring**: Offsets objects in different slabs to reduce cache conflicts
- **Complex queuing**: Full/partial/free lists per NUMA node, with careful watermarks

```
SLAB structure:
  kmem_cache
  ├── array_cache[CPU0]  ← per-cpu freed objects
  ├── array_cache[CPU1]
  ├── node[0]
  │   ├── slabs_full   (doubly-linked list)
  │   ├── slabs_partial
  │   └── slabs_free
  └── node[1]...
```

**Problems**: Complex code (~5000 lines), large `struct kmem_cache` (many fields), memory overhead from per-cpu caches, hard to debug.

#### SLUB (2007, default since 2.6.23)

Designed by Christoph Lameter. Key insight: most SLAB complexity was there to handle fragmentation that SLUB avoids differently:
- **No free slab lists**: When a slab becomes empty, return immediately to buddy
- **Per-CPU slab page** (old): One active slab per CPU per cache; freed objects go right back
- **Per-CPU sheaves** (new): Batch of object pointers per CPU; no per-CPU slab ownership
- **Minimal locking**: Most allocations touch only per-cpu data (no spinlocks in fast path)

```
SLUB structure (modern with sheaves):
  kmem_cache
  ├── cpu_sheaves[CPU0]
  │   ├── main sheaf  [ptr0][ptr1][ptr2]...[ptrN]
  │   └── spare sheaf
  ├── node[0]
  │   ├── partial list  (some free, some used)
  │   └── barn (pool of sheaves)
  └── node[1]...
```

**Advantages**: Fewer lines of code, smaller memory overhead, better performance under high concurrency, easier debugging.

#### SLOB (Small allocator, for tiny embedded systems)

Designed for systems with very little RAM (< 8MB):
- Uses a **first-fit** allocation strategy over a flat free list
- No caches, no per-cpu structures, minimal code
- Terrible performance at scale (O(n) allocation)
- No NUMA awareness
- Deprecated and scheduled for removal in modern kernels

### Comparison Table

| Feature | SLAB | SLUB | SLOB |
|---|---|---|---|
| Default since | 2.4 era | 2.6.23 | Never (opt-in) |
| Lines of code | ~5000 | ~8000 (but cleaner) | ~600 |
| Per-CPU structure | array_cache | sheaves | none |
| Free slab lists | yes (3 lists) | no (only partial) | no |
| NUMA support | yes | yes (better) | no |
| Cache coloring | yes | no | no |
| Slab merging | no | yes | no |
| Debug support | ok | excellent (slabinfo) | minimal |
| Memory overhead | high (per-cpu caches) | low | minimal |
| Performance | good | excellent | poor |
| Target systems | general | all | tiny embedded |

---

<a name="q2"></a>
## Q2: What problem does `kmem_cache_init` solve?

### The Core Problem

Before `kmem_cache_init()` runs, the kernel has:
- Physical memory available (buddy allocator works)
- Page tables set up (MMU on)
- A handful of global variables initialized

What it does NOT have: any way to allocate a single byte of kernel memory dynamically. Not `kmalloc(8, GFP_KERNEL)`. Not `kzalloc(sizeof(struct foo), ...)`. Nothing.

This is catastrophic because essentially every kernel subsystem that initializes after `mm_core_init()` needs dynamic allocation:
- `kmemleak_init()` (just after) needs to allocate its tracking structures
- `vmalloc_init()` needs to allocate red-black tree nodes
- Workqueues need to allocate work items
- File systems need to allocate inodes, dentries, superblocks

### The Solution `kmem_cache_init` Provides

```
Before kmem_cache_init():              After kmem_cache_init():
  alloc_pages(GFP_KERNEL, 0) ✓          kmalloc(8..2MB, *) ✓
  kmalloc() ✗                           kzalloc() ✓
  kmem_cache_create() ✗                 kcalloc() ✓
  kzalloc() ✗                          kmem_cache_create() ✓
  kstrdup() ✗                           kstrdup() ✓
  vzalloc() ✗ (needs vmalloc_init)      kfree() ✓
```

Specifically, `kmem_cache_init()`:
1. Creates the two meta-caches (`kmem_cache` and `kmem_cache_node`) needed for any future cache creation
2. Creates 22 × 4 = 88 `kmalloc` caches covering sizes from 8 bytes to 2 MB in four types (normal, reclaim, cgroup, DMA)
3. Sets `slab_state = UP`, which is the gate checked by `kmalloc()` to know it's safe to proceed

Without it, even the very first `pr_info()` call that tries to allocate a formatted string buffer would crash.

---

<a name="q3"></a>
## Q3: What is a "slab" and how does it relate to a page?

### From First Principles

Imagine you need to repeatedly allocate and free `struct task_struct` (8KB on x86-64). The naive approach: for each allocation, ask the buddy allocator for 2 pages; for each free, return them. Problems:
- **Latency**: buddy allocation is not trivial (finding, splitting, merging order-1 blocks)
- **Fragmentation**: external fragmentation in buddy increases
- **Cache cold**: each new allocation starts with cold cache lines

The slab approach: pre-allocate a page, divide it into N `task_struct`-sized slots. Keep a free list of available slots. Allocation = pop from free list (O(1)). Free = push to free list (O(1)). When all slots are free, return the page to buddy. When all slots are used, allocate a new page and divide again.

### The "Slab" Terminology

The word "slab" is overloaded in Linux:

1. **The subsystem**: "the slab allocator" = SLAB, SLUB, or SLOB collectively
2. **The data structure** (`struct slab`): metadata describing one slab page (or compound multi-page group) carved into fixed-size objects
3. **Informally**: "a slab" = one page (or group of pages) dedicated to a particular cache

### `struct slab` vs `struct page`

In Linux, `struct slab` **physically overlays** `struct page` at the same memory address. This is the key design decision:

```c
// mem_map[PFN] is struct page
// page_slab(page) = (struct slab *)page  — same address!

struct slab {
    memdesc_flags_t flags;          // Overlays page->flags
    struct kmem_cache *slab_cache;  // Overlays page->compound_head
    struct list_head slab_list;     // Partial/full list link
    void *freelist;                 // Head of free object chain
    unsigned inuse:16;              // Objects currently allocated
    unsigned objects:15;            // Total objects in this slab
    unsigned frozen:1;              // Owned by a CPU's per-cpu sheaf
    atomic_t __page_refcount;       // Overlays page->_refcount
};
```

Why overlay? Because:
- Every slab is exactly one page or a compound group of pages
- The `struct page` for those pages must exist anyway (mem_map/vmemmap)
- Reusing the same memory means zero extra overhead per slab page
- Type safety: `folio_slab()` / `page_slab()` do the cast and validate

The `compound_head` overlap at `slab_cache` works because:
- For compound page heads: `compound_head` bit 0 = 0 (it's not a tail page)
- All `struct kmem_cache *` pointers are pointer-aligned (even address) → bit 0 = 0
- So `slab->slab_cache` (even pointer) never collides with a tail page marker

---

<a name="q4"></a>
## Q4: What are the key fields of `struct kmem_cache` and what does each track?

### Why the Cache Descriptor Exists

`struct kmem_cache` is the configuration + state of one slab cache. Think of it like a factory blueprint + inventory manager for one type of object.

### Field-by-Field Explanation

```c
struct kmem_cache {
```

**`cpu_sheaves`** — Per-CPU fast path
```
Points to a per-cpu array of slub_percpu_sheaves. This is the hot path:
most allocations read/write only this structure. Per-cpu means no cross-CPU
contention — each core has its own sheaf.
```

**`flags`** — Cache behavior bits
```
SLAB_HWCACHE_ALIGN: pad objects to cache line size
SLAB_TYPESAFE_BY_RCU: defer slab page reuse past RCU grace period
SLAB_POISON: fill freed objects with 0xa5 pattern (UAF detection)
SLAB_PANIC: panic instead of returning NULL on OOM
```

**`size` vs `object_size`**
```
object_size = what the caller asked for (e.g., sizeof(struct task_struct))
size = actual allocation size after:
  - alignment padding (to sizeof(void*))
  - optional freepointer appended (for RCU/poison/ctor caches)
  - KASAN shadow metadata
  - alignment to s->align (e.g., 64B for HWCACHE_ALIGN)

Example for a 72-byte struct with HWCACHE_ALIGN on ARM64 (64B lines):
  object_size = 72
  After align to sizeof(void*): 72 (already aligned)
  After HWCACHE_ALIGN: ALIGN(72, 64) = 128
  size = 128  (56 bytes wasted as padding)
```

**`offset`** — Freepointer location
```
When an object is FREE, a pointer to the next free object is stored
at byte offset s->offset within the object's memory.
When the object is ALLOCATED, those bytes are overwritten with user data.

Two strategies:
  Inline (default): offset = ALIGN_DOWN(object_size / 2, sizeof(void*))
    → freeptr in middle of object; survives heap sprays less predictably
  
  Appended (for RCU/poison/ctor): offset = size (beyond object_size)
    → freeptr after object; object memory untouched by freelist bookkeeping
    → needed when object memory must not be written while potentially accessible
```

**`oo` (order-objects)** — Packed encoding
```
Upper 16 bits: page order for this cache (0=4KB, 1=8KB, 2=16KB, 3=32KB)
Lower 16 bits: number of objects per slab at that order

oo_order(s->oo) = preferred page order
oo_objects(s->oo) = preferred objects per slab
s->min = fallback (always enough for at least 1 object)
```

**`allocflags`** — Page allocation flags
```
These GFP flags are OR'd into every buddy page allocation for this cache:
  __GFP_COMP: always (compound pages)
  GFP_DMA: for DMA caches
  __GFP_RECLAIMABLE: for KMALLOC_RECLAIM caches
```

**`node[MAX_NUMNODES]`** — Per-NUMA-node state
```
Variable-length array at the end of the struct.
node[0] = kmem_cache_node for NUMA node 0 (partial list, barn)
node[1] = kmem_cache_node for NUMA node 1
...
Allocated at create time: only nr_node_ids entries used.
```

---

<a name="q5"></a>
## Q5: What is the slab allocator's position in the overall memory hierarchy?

### The Full Stack

```
Hardware: DRAM chips (physical memory)
    ↓
Firmware/bootloader: sets up initial memory map (UEFI, ATF, U-Boot)
    ↓
Kernel: memblock — early allocator, simple bitmap of free regions
  Used by: paging_init, zone setup, vmemmap allocation
    ↓
Kernel: buddy allocator — manages pages (4KB units) in per-zone free lists
  Used by: vmalloc, CMA, DMA, high-level allocators
    ↓
Kernel: SLUB slab allocator ← kmem_cache_init creates this
  Uses: buddy allocator (new_slab → __alloc_pages_node)
  Provides: kmalloc, kmem_cache_alloc (sub-page, fixed-size objects)
    ↓
Kernel: vmalloc — virtually contiguous, physically scattered
  Uses: slab (for VMA descriptors, page lists)
  Uses: buddy (for backing pages)
  Provides: large virtually contiguous buffers
    ↓
Kernel subsystems: network, filesystem, device drivers
  Use: slab (most small objects), vmalloc (large buffers), buddy (DMA)
    ↓
User space: malloc, mmap
  Uses: kernel brk/mmap syscalls, which use buddy+vmalloc+slab
```

Slab is unique because it is **used by the systems that implement the layers around it**. vmalloc needs slab for its internal data structures. The page allocator needs slab for page extensions. Even the slab allocator itself uses slab (meta-caches `kmem_cache` and `kmem_cache_node`).

---

## Bootstrap Sequence

---

<a name="q6"></a>
## Q6: Explain the chicken-and-egg bootstrap problem in `kmem_cache_init`

### The Problem

To create a slab cache, SLUB needs to:
1. Allocate a `struct kmem_cache` object (from the `kmem_cache` slab cache)
2. Allocate `struct kmem_cache_node` objects (from the `kmem_cache_node` slab cache)

But to have those caches, they must first be created — which requires them to already exist. Classic chicken-and-egg.

### Three-Level Dependency

```
To create cache X:
  1. Need s->node[N] = alloc from kmem_cache_node cache     (dependency A)
  2. Need struct kmem_cache for X = alloc from kmem_cache cache  (dependency B)
  3. Need struct kmem_cache_node for kmem_cache = alloc from... (dependency A again!)
```

### The Solution: Three Tricks

**Trick 1: Static initdata objects**
```c
static __initdata struct kmem_cache      boot_kmem_cache;
static __initdata struct kmem_cache_node boot_kmem_cache_node;

// These exist in the .init.data section — no allocation needed.
// Zero-initialized by BSS clearing before start_kernel.
kmem_cache_node = &boot_kmem_cache_node;
kmem_cache      = &boot_kmem_cache;
```
Now `kmem_cache_node` and `kmem_cache` global pointers point somewhere valid (even though those descriptors aren't fully initialized yet).

**Trick 2: Direct slab page manipulation**

When `create_boot_cache(kmem_cache_node, ...)` calls `init_kmem_cache_nodes()`, the first `struct kmem_cache_node` needed cannot be allocated from the slab (it doesn't exist yet). So `early_kmem_cache_node_alloc()` bypasses normal allocation:

```c
// Get a raw page from buddy:
struct slab *slab = new_slab(kmem_cache_node, GFP_NOWAIT, node);

// The page already has a freelist (set up by new_slab).
// Take the FIRST object directly:
struct kmem_cache_node *n = slab->freelist;
slab->freelist = get_freepointer(kmem_cache_node, n);  // advance freelist
slab->inuse = 1;  // mark 1 object used

// Assign it as the node descriptor:
kmem_cache_node->node[node] = n;
init_kmem_cache_node(n, NULL);
```

This "steals" the first object from the first-ever slab page, using it as the `kmem_cache_node` descriptor itself. Beautiful and self-referential.

**Trick 3: The `bootstrap()` migration**

After `slab_state = PARTIAL`, real allocations from `kmem_cache_node` are possible. Now we can create `kmem_cache` similarly. But both caches still use the static `boot_kmem_cache*` objects as their descriptors. Those static objects will be freed when `free_initmem()` runs!

`bootstrap()` migrates them to real heap allocations:
```c
// Allocate a real struct kmem_cache from the now-working slab:
struct kmem_cache *s = kmem_cache_zalloc(kmem_cache, GFP_NOWAIT);

// Copy all fields from the static descriptor:
memcpy(s, static_cache, kmem_cache->object_size);

// CRITICAL: update all slab pages' back-pointers:
// Every slab page has slab->slab_cache = (still) pointing at static object.
// After bootstrap, they must point at the new heap object.
for_each_kmem_cache_node(s, node, n) {
    list_for_each_entry(p, &n->partial, slab_list)
        p->slab_cache = s;   // ← update back-pointer
}
list_add(&s->list, &slab_caches);
return s;
```

After `bootstrap()`: the global `kmem_cache` points at a heap-allocated descriptor managed by SLUB itself. The static object is abandoned (but harmlessly — `free_initmem()` reclaims `.init.data` later).

---

<a name="q7"></a>
## Q7: What role does `static __initdata` play? What happens to those objects later?

### `__initdata` Placement

```c
static __initdata struct kmem_cache boot_kmem_cache;
//           ↑
//    This expands to: __attribute__((__section__(".init.data")))
```

The `.init.data` ELF section:
- Is **not** in normal BSS or data section
- Is physically allocated in the kernel image (like any initialized data)
- Is **not released** during boot until `free_initmem()` is explicitly called
- Is **zeroed** as part of BSS initialization in `head.S` before `start_kernel()`

So `boot_kmem_cache` starts as all-zeros. `kmem_cache_init()` populates its fields via `create_boot_cache()` → `do_kmem_cache_create()` → `calculate_sizes()` etc.

### The Lifetime of `.init.data`

```
Boot timeline:
  start_kernel() begins
    ↓
  kmem_cache_init() — uses boot_kmem_cache* (in .init.data)
    ↓
  ... many subsystems init, still in .init.data phase ...
    ↓
  kernel_init() thread starts
    ↓
  free_initmem()  ← .init.text and .init.data freed to buddy!
    │
    ├── All __init functions' code freed
    ├── All __initdata variables freed (including boot_kmem_cache*!)
    └── Pages returned to buddy allocator
```

**The window of danger**: Between `kmem_cache_init()` using `boot_kmem_cache*` and `bootstrap()` migrating them, the static objects are LIVE. After `bootstrap()`, the global `kmem_cache` pointer is updated to the heap-allocated copy, and the static `boot_kmem_cache` is effectively dead (no pointers to it remain, except the stack variable `boot_kmem_cache` itself which goes out of scope when `kmem_cache_init()` returns).

When `free_initmem()` runs, it blankets the entire `.init.data` section — including the now-dead `boot_kmem_cache*` — back to the buddy allocator. This is safe because by then, no live references to the static objects exist.

---

<a name="q8"></a>
## Q8: What does `bootstrap()` do and why must back-pointers be patched?

### What `bootstrap()` Does

```c
static struct kmem_cache * __init bootstrap(struct kmem_cache *static_cache)
{
    int node;
    struct kmem_cache *s = kmem_cache_zalloc(kmem_cache, GFP_NOWAIT);
    struct kmem_cache_node *n;

    memcpy(s, static_cache, kmem_cache->object_size);

    for_each_kmem_cache_node(s, node, n) {
        struct slab *p;
        list_for_each_entry(p, &n->partial, slab_list)
            p->slab_cache = s;
#ifdef CONFIG_SLUB_DEBUG
        list_for_each_entry(p, &n->full, slab_list)
            p->slab_cache = s;
#endif
    }
    list_add(&s->list, &slab_caches);
    return s;
}
```

Steps:
1. **Allocate a real heap object**: `kmem_cache_zalloc(kmem_cache, GFP_NOWAIT)` — at this point, `slab_state = PARTIAL` so allocating from `kmem_cache` is possible (one slab page exists, was created in Phase 3)
2. **Copy all descriptor fields**: `memcpy(s, static_cache, kmem_cache->object_size)` — the heap object gets identical configuration
3. **Patch back-pointers on all existing slabs**: explained below
4. **Register in global list**: `list_add(&s->list, &slab_caches)` — makes the cache visible to `/proc/slabinfo`, debugfs, etc.
5. **Return new pointer**: caller updates the global (`kmem_cache = bootstrap(...)`)

### Why Back-Pointers Must Be Patched

Every slab page has a `slab->slab_cache` field pointing to its owning `struct kmem_cache`. This is the critical link used by `kfree()`:

```c
void kfree(const void *x) {
    struct folio *folio = virt_to_folio(x);
    struct slab *slab = folio_slab(folio);
    struct kmem_cache *s = slab->slab_cache;   // ← uses back-pointer
    // ... return x to cache s ...
}
```

**Before `bootstrap()`**: existing slab pages have `slab->slab_cache = &boot_kmem_cache` (the static object).

**After `bootstrap()` without patching**: `kmem_cache` global → heap object. But any `kfree()` call on an object from those existing slabs would still follow `slab->slab_cache` to the old static address. Since:
- We did `memcpy(s, static_cache)`, so the heap copy is a valid descriptor
- But subsequent modifications to the cache (via `kmem_cache` global) would update the heap copy, not the static object
- `kfree()` would use stale state from the static object → potential crash or memory corruption

After patching: all existing `slab->slab_cache` pointers → heap object → consistent state.

**How many slabs need patching?** At the time `bootstrap()` is called for `kmem_cache_node`, exactly one slab page exists (from `early_kmem_cache_node_alloc`). For `kmem_cache`, also exactly one (from `create_boot_cache(kmem_cache)`). So the inner loops run exactly once per NUMA node.

---

<a name="q9"></a>
## Q9: What is `slab_state` and why does its transition order matter?

### The State Machine

```c
// mm/slab.h:325
enum slab_state {
    DOWN,    // No slab functionality — static initdata only
    PARTIAL, // kmem_cache_node allocations possible (only!)
    UP,      // Full kmalloc/kmem_cache_create available
    FULL     // Everything including sysfs/debug initialized
};
extern enum slab_state slab_state;
```

### Why State Matters: The Guard in `init_kmem_cache_nodes`

```c
static int init_kmem_cache_nodes(struct kmem_cache *s)
{
    for_each_node_mask(node, slab_nodes) {
        if (slab_state == DOWN) {
            early_kmem_cache_node_alloc(node);  // bootstrap path
            continue;
        }
        // Normal path (only reached when slab_state >= PARTIAL):
        n = kmem_cache_alloc_node(kmem_cache_node, GFP_KERNEL, node);
        ...
    }
}
```

`slab_state == DOWN` is the gate. The first call to `init_kmem_cache_nodes()` (for `kmem_cache_node` itself) must use the bootstrap path. The second call (for `kmem_cache`) can use the normal path because `slab_state = PARTIAL` was set between the two `create_boot_cache` calls.

If `slab_state` were set to `PARTIAL` too early (before `early_kmem_cache_node_alloc` completes), the second call path would try to call `kmem_cache_alloc_node` when the `kmem_cache_node` cache isn't functional yet → crash.

If set too late (after both `create_boot_cache` calls), the second call for `kmem_cache` would still use `early_kmem_cache_node_alloc` — which bypasses normal allocation and would corrupt the existing slab page by "re-stealing" the first free object.

### The `UP` Transition and `kmalloc`

```c
// mm/slab_common.c:994
void __init create_kmalloc_caches(void)
{
    for (type = ...) {
        for (i = ...) new_kmalloc_cache(i, type);
    }
    slab_state = UP;   // ← AFTER all caches created
    ...
}
```

`slab_state = UP` signals that `kmalloc()` is safe. The guard:
```c
// include/linux/slab.h
static __always_inline void *kmalloc(size_t size, gfp_t flags)
{
    // If called before slab_state=UP: falls through to __kmalloc_large or panics
    // After: uses kmalloc_caches[] fast path
    ...
}
```

Setting `UP` before all caches are ready would cause `kmalloc()` to access a `NULL` entry in `kmalloc_caches[]` → NULL dereference. Setting it after ensures all 88 caches are populated.

---

<a name="q10"></a>
## Q10: Why must `memblock_free_all()` run before `kmem_cache_init()`?

### The Dependency Chain

```
memblock_free_all()
  └── All physical pages handed to buddy allocator
      Each page → free_list in its zone
      struct page initialized with PG_buddy flag

kmem_cache_init()
  └── create_boot_cache()
      └── do_kmem_cache_create()
          └── init_kmem_cache_nodes()
              └── early_kmem_cache_node_alloc(node)
                  └── new_slab()
                      └── alloc_slab_page()
                          └── __alloc_pages_node()   ← needs buddy to have pages!
```

**If `memblock_free_all()` hasn't run**: The buddy allocator has no free pages (or only some pages from early memblock_reserve releases). `__alloc_pages_node()` would return NULL → `new_slab()` returns NULL → `BUG_ON(!slab)` fires in `early_kmem_cache_node_alloc`.

### What `memblock_free_all()` Does

```c
// mm/memblock.c
unsigned long __init memblock_free_all(void)
{
    unsigned long count = 0;
    phys_addr_t start, end;
    
    // Iterate all free memblock regions
    for_each_free_mem_range(i, NUMA_NO_NODE, MEMBLOCK_NONE, &start, &end, NULL) {
        // For each physical page in [start, end):
        //   1. Initialize struct page (PG_buddy, count=0, etc.)
        //   2. __free_pages_core() → add to buddy free lists
        count += (end - start) >> PAGE_SHIFT;
    }
    return count;
}
```

After this, the buddy allocator has millions of pages ready to serve. `kmem_cache_init()` can then request its first slab page with confidence.

### `mem_init()` Also Must Run First

`mem_init()` (called just before `kmem_cache_init()`) handles architecture-specific finalization:
- ARM32: sets up `highmem` infrastructure (PKMap tables)
- ARM64: frees temporary mapping pages, sets up mem_map
- Both: updates `totalram_pages` counter

Without `mem_init()`, zone accounting may be incomplete, and zone-specific allocations (e.g., `GFP_DMA`) might fail or return incorrect results.

---

## Object Layout

---

<a name="q11"></a>
## Q11: How does SLUB decide where to put the free pointer inside an object?

### Why There Is a Free Pointer

When an object is on the free list, SLUB needs to store a pointer to the next free object. Instead of a separate allocation for this pointer, SLUB **writes it inside the object's own memory** (at offset `s->offset`). When the object is allocated to a caller, those bytes are overwritten with real data.

This "intrusive linked list" approach means zero overhead per free object — the freed memory serves double duty as list node storage.

### Two Placement Strategies

#### Strategy 1: Inline at `object_size / 2` (Default)

```c
// In calculate_sizes():
if (!((flags & SLAB_TYPESAFE_BY_RCU) || (flags & SLAB_POISON) || s->ctor)) {
    s->offset = ALIGN_DOWN(s->object_size / 2, sizeof(void *));
    // No size increase — freeptr is inside the existing object space
}
```

Example: `kmalloc-64` (object_size = 64):
```
offset = ALIGN_DOWN(64/2, 8) = ALIGN_DOWN(32, 8) = 32

Object layout when FREE:
  bytes 0..31:  available (not used by freelist)
  bytes 32..39: ← FREEPOINTER (pointer to next free object)
  bytes 40..63: available
```

Why the midpoint? Placing it at byte 0 would make heap overflows from a previous object immediately corrupt the freelist. The midpoint makes heap metadata slightly harder to target in exploits. (Security improvement without cost.)

**Prerequisite**: The object must be safe to write to when freed. This is true for most objects: once freed, no code should access them. But...

#### Strategy 2: Appended Beyond `object_size` (For Special Cases)

```c
if ((flags & SLAB_TYPESAFE_BY_RCU) || (flags & SLAB_POISON) || s->ctor) {
    s->offset = size;          // After the object
    size += sizeof(void *);    // Total size grows by 8 bytes
}
```

Example: `task_struct` cache with `SLAB_TYPESAFE_BY_RCU`:
```
object_size = 9216 (struct task_struct)
offset = 9216  (appended)
size = 9224  (9216 + 8)

Object layout when FREE:
  bytes 0..9215:  object memory (NOT written — RCU readers might access it!)
  bytes 9216..9223: ← FREEPOINTER
```

Three cases requiring appended freepointer:
1. **`SLAB_TYPESAFE_BY_RCU`**: Object was freed but RCU grace period hasn't expired. Old pointers to it might still be dereferenced. Must not corrupt object body.
2. **`SLAB_POISON`**: After freeing, object is filled with `0xa5` pattern. Writing a freepointer at offset would corrupt the poison pattern, defeating the use-after-free detector.
3. **`s->ctor`**: Constructor is called when object is first carved out of a slab. If freepointer was inline, it would overwrite constructor-initialized state on first free — bad for caches that rely on constructor invariants.

### FREELIST_HARDENED: Pointer Obfuscation

When `CONFIG_SLAB_FREELIST_HARDENED` is enabled:
```c
// The stored freepointer is NOT the raw next-object pointer.
// It's XOR'd with a per-cache secret AND the pointer's own address:
static inline void *freelist_ptr_decode(const struct kmem_cache *s,
                                         void *ptr, unsigned long ptr_addr)
{
    return (void *)(((unsigned long)ptr) ^ s->random ^ swab(ptr_addr));
}
```

This prevents an attacker who can write to a freed object from simply replacing the freepointer with an arbitrary address — they don't know `s->random` and the XOR target address.

---

<a name="q12"></a>
## Q12: Walk through `calculate_order()` — how does SLUB pick the page order for a cache?

### Goal

Find the smallest page order such that:
1. The slab holds at least `min_objects` objects
2. Internal waste (unused bytes due to packing mismatch) is below a threshold

### The Algorithm Step by Step

```c
static inline int calculate_order(unsigned int size)
{
    unsigned int order, min_order;
    unsigned int min_objects = slub_min_objects;
```

**Step 1: Compute `min_objects`**
```c
    if (!min_objects) {  // Auto-compute based on CPU count
        unsigned int nr_cpus = num_present_cpus();
        if (nr_cpus <= 1) nr_cpus = nr_cpu_ids;
        min_objects = 4 * (fls(nr_cpus) + 1);
    }
```

Rationale: More CPUs → more concurrent allocations → prefer larger slabs to reduce refill frequency:
- 1 CPU: `4*(1+1) = 8` objects minimum
- 4 CPUs: `4*(3+1) = 16` objects
- 8 CPUs: `4*(4+1) = 20` objects
- 128 CPUs: `4*(8+1) = 36` objects
- 1024 CPUs: `4*(11+1) = 48` objects

**Step 2: Compute `min_order`**
```c
    max_objects = max(order_objects(slub_max_order, size), 1U);
    min_objects = min(min_objects, max_objects);
    
    min_order = max_t(unsigned int, slub_min_order,
                      get_order(min_objects * size));
```

`min_order` = smallest order that can fit `min_objects` objects:
- `get_order(min_objects * size)` = ceil(log2(min_objects * size / PAGE_SIZE))

**Step 3: Try decreasing waste tolerances**
```c
    for (unsigned int fraction = 16; fraction > 1; fraction /= 2) {
        order = calc_slab_order(size, min_order, slub_max_order, fraction);
        if (order <= slub_max_order)
            return order;
    }
```

`calc_slab_order(size, min_order, max_order, frac)` finds the smallest order in [min_order, max_order] where:
```
wasted_bytes = (1 << (PAGE_SHIFT + order)) % size
waste_fraction = wasted_bytes / total_bytes
acceptable if: waste_fraction <= 1/frac
```

Fractions tried: 1/16 (6.25%), 1/8 (12.5%), 1/4 (25%), 1/2 (50%).

### Worked Example: `kmem_cache_node` (80 bytes, 8 CPUs)

```
size = 80, nr_cpus = 8
min_objects = 4 * (fls(8) + 1) = 4 * 5 = 20
min_order = get_order(20 * 80) = get_order(1600) = 1 (4096 > 1600, but get_order(1600) = 1 because 2^1*4096=8192 ≥ 1600)
Wait: get_order(1600) = ceil(log2(1600/4096)) — this would be 0 since 4096 > 1600.
Actually: get_order(1600) = 0 (4KB page can hold 1600B)

Try fraction=16 (max 6.25% waste):
  order=0: total = 4096, objects = 4096/80 = 51, used = 51*80=4080, waste=16
  waste fraction = 16/4096 = 0.39% < 6.25% ✓
  → calc_slab_order returns 0

Result: oo_make(0, 80) = (0 << 16) | 51 = 51
kmem_cache_node: order=0 (4KB page), 51 objects per slab, 16 bytes wasted
```

### Worked Example: `kmalloc-192` (192 bytes, 8 CPUs)

```
size = 192
min_objects = 20
min_order = get_order(20*192=3840) = 0 (4096 > 3840)

Try fraction=16:
  order=0: objects = 4096/192 = 21 (floor), used = 21*192=4032, waste=64
  waste fraction = 64/4096 = 1.56% < 6.25% ✓
  → order=0

Result: 21 objects per 4KB slab, 64 bytes wasted (1.56%)
```

---

<a name="q13"></a>
## Q13: What is `SLAB_HWCACHE_ALIGN` and how does it prevent false sharing?

### Cache Line Basics

Modern CPUs cache memory in "cache lines" (64 bytes on ARM64 and most x86). The cache is addressed by cache lines, not bytes. When a CPU writes one byte, the entire 64-byte line is marked dirty. When another CPU reads any byte from that same line, it must wait for the dirty line to be flushed.

**False sharing**: Two CPUs accessing different data that happens to reside on the same cache line. No logical sharing, but the cache thinks they're sharing.

### The Problem Without Alignment

```
Cache (64-byte lines), no alignment:
  Object A (24 bytes): bytes 0..23
  Object B (24 bytes): bytes 24..47  ← SAME CACHE LINE as A!
  Object C (24 bytes): bytes 48..71  ← SAME CACHE LINE as A and B!

CPU-0 modifies Object A → invalidates the 64-byte cache line
CPU-1 reads Object B → cache miss! Must re-fetch entire line
CPU-2 reads Object C → another cache miss!
```

This can cause 10-20x slowdowns on highly contended objects.

### The Fix: `SLAB_HWCACHE_ALIGN`

```c
// In create_kmalloc_caches() via calculate_alignment():
unsigned int ralign = cache_line_size();  // 64B on ARM64

while (size <= ralign / 2)
    ralign /= 2;   // Don't pad smaller than half cache line
                    // (avoids wasting memory for tiny objects)
align = max(align, ralign);

// Then: size = ALIGN(size, align);
```

Result:
```
Object A (24 bytes, HWCACHE_ALIGN, 64B lines):
  aligned_size = ALIGN(24, 64) = 64  (40 bytes padding added)

  bytes 0..23:   Object A data
  bytes 24..63:  PADDING (wasted)
                 ← cache line boundary ─────────────────────

  bytes 64..87:  Object B data
  bytes 88..127: PADDING
```

Now Object A and Object B are on different cache lines. CPU-0 modifying A doesn't affect CPU-1 reading B.

### When NOT Used

Not all caches use `SLAB_HWCACHE_ALIGN`:
- Small objects (< half cache line) get half-line alignment (reduces waste)
- Caches with `SLAB_TYPESAFE_BY_RCU` where objects are logically read-only after allocation
- Debug caches where memory efficiency is less important

The bootstrap caches (`kmem_cache_node`, `kmem_cache`) use `SLAB_HWCACHE_ALIGN` because they are accessed frequently from many CPUs.

---

<a name="q14"></a>
## Q14: What is `reciprocal_size` and why is division avoided in the hot path?

### Where Division Would Be Needed

When an object is freed via `kfree(ptr)`, SLUB needs to find its index within the slab (to update `slab->inuse` correctly and to locate the object for debugging):

```
index = (ptr - slab_address(slab)) / object_size
```

This integer division is on the critical free path — called millions of times per second on busy systems.

### The Problem with Division

On most architectures including ARM32 and some ARM64 configurations, integer division is:
- Not a single instruction (must call `__aeabi_uidiv` or `__udivdi3`)
- 10-30 CPU cycles vs 1-4 for multiply
- Not pipelined efficiently

### Reciprocal Multiplication

The trick: precompute a magic multiplier `m` and shift `sh` such that:
```
x / N  ≡  (x * m) >> sh   (for all x in valid range)
```

This is the "multiplication by reciprocal" technique from Hacker's Delight.

```c
// struct reciprocal_value precomputed from object_size:
struct reciprocal_value {
    u32 m;   // magic multiplier
    u8 sh1;  // first shift
    u8 sh2;  // second shift
};

// Usage in obj_to_index():
static inline unsigned int obj_to_index(const struct kmem_cache *cache,
                                         const struct slab *slab, void *obj)
{
    u32 offset = (u32)(obj - slab_address(slab));
    return reciprocal_divide(offset, cache->reciprocal_size);
}

// reciprocal_divide: expand to 2-3 instructions (shift+multiply+shift)
```

Example for `size = 64`:
- `reciprocal_value(64)` precomputes `m = 0x04000001`, `sh1 = 0`, `sh2 = 6`
- `reciprocal_divide(offset, rv)` = `offset >> 6` (for power-of-2, simplifies to shift)

For non-power-of-2 sizes (e.g., 96):
- More complex multiplier, but still 3-4 instructions vs 20+ for division

### Performance Impact

Benchmark (ARM64, A72):
```
kfree() hot path with integer division: ~25 ns
kfree() hot path with reciprocal_divide: ~18 ns
```
~30% improvement on the free path. Significant when millions of frees/second.

---

<a name="q15"></a>
## Q15: How does `object_size` differ from `size` in `struct kmem_cache`?

### The Two-Size Model

```
struct kmem_cache:
  unsigned int object_size;   // What the caller asked for (e.g., sizeof(struct task_struct))
  unsigned int size;          // What SLUB actually allocates per slot (always >= object_size)
```

`size` grows beyond `object_size` for several reasons:

**1. Pointer alignment**: Round up to `sizeof(void *)` (8 bytes on 64-bit):
```
object_size = 17 → aligned → 24
```

**2. Appended freepointer** (for RCU/poison/ctor caches):
```
object_size = 80 → size = 88  (+8 for appended freepointer)
```

**3. KASAN metadata**:
```
With KASAN SW tags: may add shadow bytes or metadata after object
object_size = 80 → size = 80 (shadow is separate, not in slab)
With KASAN generic: may add redzone bytes
object_size = 80 → size = 88
```

**4. SLAB_HWCACHE_ALIGN padding**:
```
object_size = 80 → ALIGN(80, 64) = 128 → size = 128  (+48 padding)
```

**5. SLUB_DEBUG redzones**:
```
With SLAB_RED_ZONE:
  left redzone (red_left_pad bytes) + object + right redzone
  size = red_left_pad + object_size + sizeof(RedZone) + alignment
```

### Why the Distinction Matters

`object_size` is used for:
- `kmem_cache->object_size` = what `kmalloc_size(cache)` returns (what caller can use)
- KASAN: track which bytes are valid for caller access
- `memcpy(s, static_cache, kmem_cache->object_size)` in `bootstrap()` — copies only valid state

`size` is used for:
- Object layout: where the freepointer lives at `s->offset` within `size` bytes
- Slab page partitioning: `nr_objects = slab_page_size / s->size`
- `reciprocal_size`: division by `size` for object index calculation
- Memory accounting: actual bytes used per allocation

```
Example: kmalloc-64 (64-byte objects) on ARM64 with SLAB_HWCACHE_ALIGN:
  object_size = 64
  After pointer align: 64 (no change)
  HWCACHE_ALIGN: ralign = 64; size(64) > 32 → ALIGN(64, 64) = 64
  size = 64 (equal in this case: 64 is already cache-line aligned)

Example: arbitrary 40-byte struct with HWCACHE_ALIGN:
  object_size = 40
  HWCACHE_ALIGN: ALIGN(40, 64) = 64
  size = 64
  → 24 bytes wasted per object for false-sharing prevention
```

---

## NUMA and Per-CPU

---

<a name="q16"></a>
## Q16: How does SLUB handle NUMA topology for memory allocation?

### The NUMA Performance Problem

On a 2-node NUMA system:
```
Node 0: DRAM bank 0 (local to CPUs 0..31), latency 10ns
Node 1: DRAM bank 1 (local to CPUs 32..63), latency 100ns

CPU-0 accessing memory on Node 1: 10× penalty
```

For a high-frequency allocator, allocating from the wrong node means every object access has 10× latency.

### SLUB's NUMA Solution

**Layered locality**:

1. **Layer 1 — Per-CPU sheaf** (fastest): Objects cached locally, tagged with the allocating CPU. Free objects return to the local sheaf regardless of which NUMA node they came from. No node affinity at this layer.

2. **Layer 2 — Per-NUMA barn**: When the sheaf needs refill, it exchanges with `node->barn`. The barn is specific to one NUMA node. `s->cpu_sheaves[cpu]->main` is refilled from the barn of `numa_mem_id()` — the CPU's preferred memory node.

3. **Layer 3 — Per-NUMA partial list**: When barn is empty, a new slab page is allocated via `alloc_pages_node(NUMA_node, ...)`. This page comes from physical DRAM on that NUMA node.

4. **Layer 4 — Remote node fallback**: If the local node's partial list is empty and allocation fails, try the next node (controlled by `remote_node_defrag_ratio`).

### `init_kmem_cache_nodes()` NUMA Setup

```c
for_each_node_mask(node, slab_nodes) {
    // Allocate struct kmem_cache_node ON that node:
    n = kmem_cache_alloc_node(kmem_cache_node, GFP_KERNEL, node);
    //                                                     ↑
    //                                         Force allocation on 'node'
    init_kmem_cache_node(n, barn);
    s->node[node] = n;
}
```

The `kmem_cache_node` for node N is physically located in node-N DRAM. CPUs on node N access it locally. CPUs on node M accessing a different cache's node-N state incur cross-node penalty — but this is rare (cross-node allocations are the exception).

---

<a name="q17"></a>
## Q17: What is a `kmem_cache_node` and what does its partial list contain?

### Structure and Purpose

```c
struct kmem_cache_node {
    spinlock_t list_lock;       // Protects everything below
    unsigned long nr_partial;   // Count of slabs in partial list
    struct list_head partial;   // The partial slab list
    struct node_barn *barn;     // Sheaf exchange pool
#ifdef CONFIG_SLUB_DEBUG
    atomic_long_t nr_slabs;
    atomic_long_t total_objects;
    struct list_head full;
#endif
};
```

One instance per (cache, NUMA-node) pair. `s->node[N]` gives you the `kmem_cache_node` for cache `s` on NUMA node N.

### The Partial Slab List

A "partial slab" is a slab page that has:
- At least 1 object in use (not returned to buddy)
- At least 1 object free (not given to a caller)

```
Partial slab state example (kmalloc-64, 64 objects per 4KB page):
  slab->objects = 64 (total slots)
  slab->inuse = 47  (objects given to callers)
  slab->freelist → 17 free objects chained together
```

Partial slabs live in `node->partial` until either:
- All objects are freed → slab is returned to buddy (if `nr_partial > min_partial`)
- All objects are allocated → slab moves to `full` list (DEBUG only, otherwise untracked)

### `min_partial` Watermark

```c
s->min_partial = clamp(ilog2(s->size) / 2, MIN_PARTIAL, MAX_PARTIAL);
```

Keeps at least `min_partial` slabs in `node->partial` even when idle. Prevents thrashing:
- Without: CPU frees all objects → slab returned to buddy → next alloc: buddy alloc again → slow
- With: Keep `min_partial=5` slabs in partial list → free+re-alloc stays in slab layer

---

<a name="q18"></a>
## Q18: What are "sheaves" in modern SLUB and why were they introduced?

### The Problem with the Old Per-CPU Slab

Old SLUB (pre-sheaves):
```c
struct kmem_cache_cpu {
    void **freelist;  // Pointer to first free object in the active slab
    struct slab *slab;    // The active slab page for this CPU
    ...
};
```

Each CPU "owned" one active slab page. Allocation = take from `freelist`. Free = put back on `freelist`.

**Problem 1 — Cross-CPU free (the bounce problem)**:
- CPU-A allocates object O from its active slab (freelist moves forward)
- CPU-B frees O (must put it on some slab's freelist)
- CPU-B doesn't own that slab → must acquire `slab->lock` → contention
- The slab metadata (freelist pointer, counter) bounces between CPU-A and CPU-B's L1 caches

**Problem 2 — Slab page "frozen" complexity**:
- When a CPU owns a slab, it's "frozen" (no other CPU can manipulate it)
- Complex state machine to freeze/unfreeze slabs
- Edge cases: CPU idle, CPU hotplug, memory pressure

### The Sheaves Solution

A **sheaf** is simply an array of object pointers:
```c
struct slab_sheaf {
    unsigned int size;      // Current count
    unsigned int capacity;  // Maximum count
    void *objects[];        // The pointers
};

struct slub_percpu_sheaves {
    local_trylock_t lock;
    struct slab_sheaf *main;   // Alloc from here (pop from tail)
    struct slab_sheaf *spare;  // Pre-loaded for quick swap
    struct slab_sheaf *rcu_free;  // Batch kfree_rcu
};
```

**Allocation**: `objects[--size]` — pure array indexing, no slab page involvement.
**Free**: `objects[size++] = ptr` — push to array.

**When sheaf is empty** (allocation):
- Try `spare` sheaf (if full)
- If spare empty too: exchange with `node->barn` (spinlock, but batched)
- If barn empty: allocate new slab page

**When sheaf is full** (free):
- Swap with `spare` (if spare is empty)
- If spare full too: deposit full sheaf in barn (spinlock, batched)
- Barn overloaded: return objects to slab pages (via slab's freelist update)

### Why Sheaves Are Better

**Amortization**: One spinlock acquisition handles N objects (N = sheaf capacity). Old model: one lock acquisition per cross-CPU free.

**Cache behavior**: The sheaf is a compact array. Accessing `objects[size-1]` is a single L1 cache read. Old model: following freelist pointers (pointer chasing — cache-unfriendly).

**Simpler invariants**: No "frozen slab" state machine. Slab pages are just memory pools; CPUs exchange batches of pointers via the barn.

---

<a name="q19"></a>
## Q19: How does the per-CPU sheaves fast path avoid locking?

### The Key: Local Trylock

```c
struct slub_percpu_sheaves {
    local_trylock_t lock;  // ← this is not a spinlock!
    ...
};
```

`local_trylock_t` is a CPU-local lock that:
1. **Disables preemption** (so the CPU won't be scheduled away mid-operation)
2. **Prevents NMI/softirq re-entrancy** (on the same CPU)
3. **Is NOT a cross-CPU lock** (no atomic operations affecting other CPUs)

```c
// Fast allocation:
local_trylock(&pcs->lock)  // Disable preemption on this CPU
if (pcs->main->size > 0) {
    obj = pcs->main->objects[--pcs->main->size];  // Pure array access
    local_tryunlock(&pcs->lock);   // Re-enable preemption
    return obj;
}
// If trylock fails (preemption/interrupt already has it): fall to slow path
```

### Why No Cross-CPU Synchronization Needed

The `cpu_sheaves` array is indexed by CPU ID. `cpu_sheaves[CPU_N]` is accessed exclusively by CPU N (until the CPU dies). No other CPU reads or writes CPU-N's sheaf while CPU-N is alive.

This is why a **trylock** (not a spinlock) suffices. The only contention is:
- CPU N executing kernel code vs an interrupt/NMI on CPU N trying to also call `kmalloc`
- The trylock handles this: if interrupted mid-alloc, the interrupt takes the slow path

**No memory barrier needed for the sheaf itself**: All accesses are on the same CPU (program order is sufficient for single-CPU visibility).

### The Slow Path

```c
// slow path (sheaf empty or trylock failed):
get_partial_node()     → grab from node->partial (list_lock spinlock)
deactivate_slab()      → return active slab to node (list_lock)
allocate_slab()        → alloc from buddy, initialize
```

The slow path uses real spinlocks (`node->list_lock`, `barn->lock`) but is called rarely (once per N allocations, where N = sheaf capacity).

---

<a name="q20"></a>
## Q20: What is the `node_barn` and how does it interact with sheaves?

### The Barn as a Depot

```c
struct node_barn {
    spinlock_t lock;
    struct list_head sheaves_full;    // Full sheaves (ready for CPU to alloc from)
    struct list_head sheaves_empty;   // Empty sheaves (ready for CPU to free to)
    unsigned int nr_full;
    unsigned int nr_empty;
};
```

The barn is a per-NUMA-node depot of sheaves. Think of it as a "magazine rack":
- Full sheaves (loaded with free object pointers) are deposited by CPUs that just freed many objects, and withdrawn by CPUs that need objects
- Empty sheaves are deposited by CPUs that just allocated all their objects, and withdrawn by CPUs that need somewhere to put freed objects

### Exchange Protocol

```
CPU needs objects (sheaf empty):
  1. Acquire barn->lock
  2. Pop a full sheaf from barn->sheaves_full → assign to pcs->main
  3. Release barn->lock
  4. Alloc from new main sheaf (no lock needed)

CPU has too many objects (sheaf full):
  1. Acquire barn->lock
  2. Push current pcs->main to barn->sheaves_full
  3. Pop an empty sheaf from barn->sheaves_empty → assign to pcs->main
  4. Release barn->lock
  5. Continue freeing into now-empty main sheaf

Barn empty (no full sheaves when CPU needs objects):
  1. Allocate a new slab page (slow: buddy allocator)
  2. Fill a new sheaf with objects from the slab page
  3. Use that sheaf as main

Barn full (no empty sheaves when CPU has full sheaf):
  1. Drain full sheaf: for each object, return to its slab's freelist
  2. Return any now-fully-empty slab pages to buddy
```

### Why NUMA-Local Barn Matters

The barn is allocated per NUMA node in `init_kmem_cache_nodes()`:
```c
barn = kmalloc_node(sizeof(*barn), GFP_KERNEL, node);
get_node(s, node)->barn = barn;
```

CPUs on node N exchange with node-N's barn. Objects in the barn came from slab pages on node N. So when a CPU on node N gets a full sheaf from the barn, the objects in it point to physical memory on node N. Local access guaranteed.

---

## kmalloc

---

<a name="q21"></a>
## Q21: How does `kmalloc()` find the right cache for a given size?

### The Size-to-Index Mapping

```c
// Fast path in __kmalloc():
static __always_inline void *kmalloc(size_t size, gfp_t flags)
{
    if (__builtin_constant_p(size) && size) {
        unsigned int index = __kmalloc_index(size, true);
        return kmalloc_caches[kmalloc_type(flags, _RET_IP_)][index];
    }
    return __kmalloc(size, flags);
}
```

`__kmalloc_index(size)`:
```c
static __always_inline unsigned int __kmalloc_index(size_t size, bool size_is_constant)
{
    if (!size)                   return 0;
    if (size <= 32)              return 1;  // 96B cache (special)
    if (size <= 64)              return 2;  // 192B cache (special)
    // Power-of-2 region:
    // size <= 8:   index 3
    // size <= 16:  index 4
    // ...
    // Actually: return ilog2(size - 1) + 1 for power-of-2 rounding
    return fls(size - 1);
}
```

Wait, the actual mapping:
```
size    → index → cache name
1-8     → 3     → kmalloc-8
9-16    → 4     → kmalloc-16
17-32   → 5     → kmalloc-32
33-64   → 6     → kmalloc-64
65-96   → 1     → kmalloc-96  (special non-power-of-2)
97-128  → 7     → kmalloc-128
129-192 → 2     → kmalloc-192 (special non-power-of-2)
193-256 → 8     → kmalloc-256
...
```

`setup_kmalloc_cache_index_table()` precomputes this mapping for all sizes from 1 to KMALLOC_MAX_SIZE.

### Runtime Fast Path

For a constant-size `kmalloc(64, GFP_KERNEL)`:
1. Compiler constant-folds `__kmalloc_index(64)` = 6 at compile time
2. Runtime: `kmalloc_caches[KMALLOC_NORMAL][6]->cpu_sheaves[cpu].main->objects[--size]`
3. Total: 2-3 pointer dereferences + array index + decrement
4. No function call, no lock (in fast path)

For variable-size `kmalloc(n, flags)`:
1. Runtime call to `__kmalloc(n, flags)`
2. `__kmalloc_index(n)` call (few instructions, O(1))
3. Same sheaf fast path

---

<a name="q22"></a>
## Q22: Why do both 96-byte and 128-byte kmalloc caches exist?

### The Internal Fragmentation Problem

Consider `kmalloc(80, GFP_KERNEL)`:
- Fits in `kmalloc-128` (128-byte objects): wastes 48 bytes (37.5%)
- With a `kmalloc-96` cache: wastes only 16 bytes (16.7%)

For `kmalloc(100, GFP_KERNEL)`:
- `kmalloc-128`: wastes 28 bytes (21.9%)
- `kmalloc-96`: fits in next = `kmalloc-128`: still 28 bytes
  (100 > 96, so still goes to 128)

The 96-byte cache helps when `size ≤ 96`. Many kernel objects are 64-96 bytes:
- `struct hlist_head` chains
- Small network protocol headers
- Small filesystem metadata

Without 96-byte cache, these would all use 128-byte slots with 25-50% waste.

### The 192-Byte Cache

Similarly, `kmalloc-192` fills the gap between 128B and 256B:
- Objects 129-192 bytes go to `kmalloc-192` (max 33% waste) instead of 256 (up to 50% waste)
- `struct bio` (block I/O), `struct nft_rule`, many module objects are in this range

### Memory Saved

On a system doing 100K concurrent 80-byte allocations:
- Without 96B cache: 100K × 128B = 12.8 MB
- With 96B cache: 100K × 96B = 9.6 MB
- Savings: 3.2 MB (25% reduction for this size class)

Scaled to a large server with millions of such objects: tens to hundreds of MB saved.

---

<a name="q23"></a>
## Q23: What are the four kmalloc cache types and when is each used?

| Type | Enum | Extra Flag | Purpose | Example Usage |
|---|---|---|---|---|
| `KMALLOC_NORMAL` | 0 | none | General kernel allocations | `kmalloc(n, GFP_KERNEL)` |
| `KMALLOC_RECLAIM` | 1 | `SLAB_RECLAIM_ACCOUNT` | Objects that CAN be freed under memory pressure | Page cache pages, dcache entries |
| `KMALLOC_CGROUP` | 2 | `SLAB_ACCOUNT` | Memory cgroup accounting | Container-aware code paths |
| `KMALLOC_DMA` | 3 | `SLAB_CACHE_DMA` | DMA-accessible memory | ISA DMA, legacy device drivers |

### How the Type is Selected

```c
// kmalloc_type() selects based on GFP flags:
static inline enum kmalloc_cache_type kmalloc_type(gfp_t flags, unsigned long caller)
{
    if (unlikely(flags & GFP_DMA))          return KMALLOC_DMA;
    if (unlikely(!IS_ENABLED(CONFIG_MEMCG)))return KMALLOC_NORMAL;
    if (flags & __GFP_RECLAIMABLE)          return KMALLOC_RECLAIM;
    if (kmem_cache_accounted(current))      return KMALLOC_CGROUP;
    return KMALLOC_NORMAL;
}
```

### `SLAB_RECLAIM_ACCOUNT` and kswapd

Pages in `KMALLOC_RECLAIM` caches are counted in `NR_SLAB_RECLAIMABLE`. The kernel's memory reclaim code (`kswapd`, `shrink_slab`) can request these pages back when memory is low. The caches must register a shrinker (`register_shrinker`) to actually free objects — SLUB doesn't do this automatically. Many filesystems register shrinkers for their inode/dentry caches.

### `SLAB_ACCOUNT` and Memory Cgroups

When `kmalloc(n, GFP_KERNEL)` is called from a process in a memory cgroup, using `KMALLOC_CGROUP` type ensures the allocation is charged to the cgroup. This enables per-container memory limits (used heavily by Docker/Kubernetes for container isolation).

---

<a name="q24"></a>
## Q24: What happens when `kmalloc()` is called before `kmem_cache_init()` completes?

### Early in Boot (Before UP)

If called with `slab_state < UP`:
```c
// In __kmalloc():
if (unlikely(slab_state < UP)) {
    // kmalloc_caches[] not fully populated
    // Some size indices may have NULL cache pointer
    // → Fall through to memblock allocator? → NO
    // → Actually: BUG() or memblock_alloc_raw() depending on context
}
```

In practice, no legitimate kernel code should call `kmalloc()` before `slab_state = UP`. The boot sequence is carefully ordered:
- `kmem_cache_init()` is called at line 2722 of `mm_init.c`
- `kmemleak_init()` (which needs kmalloc) is at line 2724 — AFTER

If accidentally called before (e.g., a buggy early init):
- With SLUB: NULL pointer dereference from `kmalloc_caches[type][index]` being NULL
- With KASAN: KASAN would catch the NULL deref → informative panic

### During Bootstrap (slab_state == PARTIAL)

Between `slab_state = PARTIAL` and `slab_state = UP`:
- `kmem_cache_alloc_node(kmem_cache_node, ...)`: WORKS (that's what PARTIAL enables)
- `kmalloc(n, GFP_KERNEL)`: FAILS (kmalloc_caches not populated yet)
- `kmem_cache_create(...)`: WORKS (uses `kmem_cache_alloc` internally)

The kernel specifically uses `kmem_cache_alloc_node` (not kmalloc) in the PARTIAL window.

---

## Debugging and Security

---

<a name="q25"></a>
## Q25: How does SLUB_DEBUG change object layout?

### Debug Flags Available

```
SLAB_RED_ZONE         Add guard bytes around objects
SLAB_POISON           Fill with pattern on free (0xa5) and on alloc (0x6b)
SLAB_STORE_USER       Store last alloc/free caller after each object
SLAB_CONSISTENCY_CHECKS Run expensive validation on alloc/free
```

### Layout Without Debug

```
Normal kmalloc-64 object layout (64 bytes):
  [0..31]: user data
  [32..39]: freepointer (when free) / user data (when allocated)
  [40..63]: user data
```

### Layout With SLAB_RED_ZONE + SLAB_POISON

```
Debug object layout:
  [0..7]:   LEFT REDZONE  (s->red_left_pad bytes, filled with RED_ZONE_MAGIC=0xbb)
  [8..71]:  ACTUAL OBJECT  (object_size bytes, user data when allocated)
  [72..79]: PADDING to align
  [80..87]: RIGHT REDZONE  (sizeof(long long), filled with 0xbb)
  [88..95]: STORE_USER: alloc/free caller PCs
  total size grows significantly

On free:
  Object bytes [8..71] filled with POISON_FREE = 0x6b
  
On alloc:
  Verify [8..71] still == 0x6b (if not → use-after-free detected!)
  Verify left and right redzones == 0xbb (if not → overflow detected!)
  Fill [8..71] with POISON_INUSE = 0x5a (so uninit reads are obvious)
```

### How Debug Affects `calculate_sizes()`

```c
#ifdef CONFIG_SLUB_DEBUG
if (flags & SLAB_RED_ZONE) {
    s->red_left_pad = sizeof(void *);   // Left redzone before object
    s->red_left_pad = ALIGN(s->red_left_pad, s->align);
    size += s->red_left_pad;
}
// ... more debug size additions ...
#endif
```

Debug builds have significantly larger object sizes, lower objects-per-slab, and much slower alloc/free paths. They are for development/testing, not production.

---

<a name="q26"></a>
## Q26: What is `SLAB_FREELIST_HARDENED` and how does it prevent heap attacks?

### The Attack Being Prevented

Classic heap overflow / use-after-free exploit:
1. Attacker frees a slab object they control
2. Object now has a freepointer at `s->offset` (plain pointer to next free object)
3. Attacker writes to the freed object (use-after-free)
4. Overwrites freepointer with target address (e.g., kernel function pointer)
5. Next `kmalloc()` returns the "target address" as a valid allocation
6. Caller writes to it → arbitrary kernel write → privilege escalation

### The Defense

```c
// CONFIG_SLAB_FREELIST_HARDENED

// In kmem_cache_open():
s->random = get_random_long();  // Per-cache secret, set once at cache creation

// When storing a freepointer (freeing an object):
static inline void set_freepointer(struct kmem_cache *s, void *object, void *fp)
{
    unsigned long freeptr_addr = (unsigned long)object + s->offset;
    // XOR with per-cache secret AND the pointer's own address:
    *(void **)freeptr_addr = freelist_ptr_encode(s, fp, freeptr_addr);
}

// freelist_ptr_encode:
static inline void *freelist_ptr_encode(const struct kmem_cache *s,
                                         void *ptr, unsigned long ptr_addr)
{
    return (void *)(((unsigned long)ptr) ^ s->random ^ swab(ptr_addr));
    //                                   ^^^^^^^^^^^  ^^^^^^^^^^^^^^^^^
    //                                   secret key   address of this pointer
    //                                                (different for each slot!)
}
```

### Why This Works

For an attacker to replace a freepointer with address X (after UAF write):
- They must write `X ^ s->random ^ swab(freeptr_addr)` to the freed object
- `s->random` is unknown (per-cache secret, initialized from PRNG at cache creation)
- `swab(freeptr_addr)` is known (pointer address is predictable), but `s->random` is not
- Without `s->random`, the attacker's forged pointer decodes to a garbage address → crash (controlled by attacker) but no arbitrary write

This is not a complete defense (kernel info leak can expose `s->random`), but it significantly raises the attack cost and eliminates naive "write to freed object" exploits.

---

<a name="q27"></a>
## Q27: What is `SLAB_FREELIST_RANDOM` and why does it matter?

### Heap Spray Attacks

A common exploit technique is "heap spray": allocate a large number of objects to ensure that the allocator predictably places an attacker-controlled object adjacent to or at a specific address. The predictability comes from the deterministic freelist ordering (objects are carved out in linear order: obj0, obj1, obj2...).

### The Randomization

```c
// CONFIG_SLAB_FREELIST_RANDOM

// At cache creation (do_kmem_cache_create()):
if (slab_state >= UP)
    init_cache_random_seq(s);

// init_cache_random_seq(): creates a random permutation of 0..nr_objects-1
// Stored in s->random_seq[]

// When a new slab page is initialized (setup_object_map()):
// Objects are linked into the freelist in RANDOM order instead of linear order:
// freelist → random_seq[0]→ random_seq[1] → ... → NULL
```

### Effect

Without randomization: first `kmalloc()` from a new slab page always returns `obj[0]`, then `obj[1]`, etc. Attacker knows "if I spray N objects, the N+1st will be at base + N * size".

With randomization: objects are returned in shuffled order. An attacker spraying N objects cannot predict which address they'll get, or where their controlled object will be placed relative to the target.

### `kmem_cache_init_late` Connection

```c
void __init kmem_cache_init_late(void)
{
    ...
#ifdef CONFIG_SLAB_FREELIST_RANDOM
    prandom_init_once(&slab_rnd_state);  // Initialize PRNG state for slab
#endif
}
```

The PRNG needs to be seeded from `/dev/urandom`-quality entropy, which is only available after early boot. `kmem_cache_init_late()` (called after IRQ enable) finalizes this.

Note: `random_seq` is computed per-cache at creation using the PRNG. Each cache gets its own shuffle, so the sequence is different even for caches of the same size.

---

<a name="q28"></a>
## Q28: How does KASAN integrate with the slab allocator?

### What KASAN Does

Kernel Address SANitizer detects:
1. **Out-of-bounds accesses**: access beyond the allocated object size
2. **Use-after-free**: access to a freed slab object

KASAN works by maintaining a **shadow memory** region: for every N bytes of kernel memory, there is 1 shadow byte indicating whether those bytes are accessible.

### KASAN Generic Integration

```c
// At cache creation: kasan_cache_create()
void kasan_cache_create(struct kmem_cache *cache, unsigned int *size,
                         slab_flags_t *flags)
{
    // May grow *size for KASAN metadata
    // May add SLAB_POISON to flags
    // Adds KASAN-specific redzone after each object
}

// At allocation: kasan_slab_alloc()
void *kasan_slab_alloc(struct kmem_cache *cache, void *object,
                        gfp_t flags, bool init)
{
    // Unpoison shadow bytes for object_size bytes of 'object'
    // (mark as accessible)
    kasan_unpoison_memory(object, cache->object_size, ...);
    return object;
}

// At free: kasan_slab_free()
bool kasan_slab_free(struct kmem_cache *cache, void *object, ...)
{
    // Poison shadow bytes for object_size bytes
    // (mark as inaccessible — "freed")
    kasan_poison_memory(object, cache->object_size, KASAN_FREE_PAGE);
    
    // For SLAB_TYPESAFE_BY_RCU: quarantine object instead of immediate return
    // (defer re-use until KASAN quarantine period expires)
    if (kasan_quarantine_put(cache, object))
        return true;  // Don't free — quarantined
    return false;
}
```

### KASAN HW Tags (ARM64 MTE)

```c
// arch/arm64/include/asm/kasan.h
// On allocation:
void *kasan_slab_alloc(struct kmem_cache *cache, void *object, ...)
{
    // Generate random 4-bit tag
    u8 tag = (u8)(prandom_u32() & 0xF);
    
    // Set tag in memory (MTE hardware instruction):
    // STG <ptr>, [ptr]   — store allocation tag
    for (int i = 0; i < cache->object_size; i += MTE_GRANULE_SIZE)
        mte_set_mem_tag_range(object + i, MTE_GRANULE_SIZE, tag);
    
    // Return tagged pointer: ptr[63:56] = tag
    return set_tag(object, tag);
}

// On free:
void kasan_slab_free(...)
{
    // Set memory tag to KASAN_TAG_INVALID (e.g., 0xFF)
    mte_set_mem_tag_range(object, cache->object_size, 0xFF);
    
    // The freed object's memory tag is now 0xFF
    // Any subsequent dereference via old pointer (tag ≠ 0xFF) → MTE fault!
}
```

**Zero runtime overhead for tagged accesses**: ARM64 MTE tag checking happens in hardware with no CPU instruction overhead (tag is checked in parallel with the memory access in the load/store pipeline).

---

## Advanced

---

<a name="q29"></a>
## Q29: What is `SLAB_TYPESAFE_BY_RCU` and how does it differ from regular RCU?

### Regular RCU and Slab

Normally, when you RCU-protect a pointer to a slab object:
```c
// Writer:
rcu_assign_pointer(global_ptr, new_obj);
synchronize_rcu();       // Wait for all readers to finish
kfree(old_obj);          // Safe: no readers holding old_obj

// Reader:
rcu_read_lock();
obj = rcu_dereference(global_ptr);
// access obj...
rcu_read_unlock();
```

After `synchronize_rcu()`, the old object is returned to the slab. Its memory *could* be immediately reallocated and overwritten. Any reader that somehow held a reference past the grace period (bug) would access corrupted data.

### `SLAB_TYPESAFE_BY_RCU` Guarantee

With this flag, the slab page backing the freed object is **not returned to the buddy allocator** until after an RCU grace period. Within the same slab page, the object slot *may* be reused for a new object of the same type. But the *physical memory* remains valid.

```c
// After kfree(obj) with SLAB_TYPESAFE_BY_RCU:
//   1. obj is added to the slab's freelist (may be reallocated immediately)
//   2. The slab PAGE is kept (not returned to buddy) for >= 1 RCU grace period
//      (Actually: the slab is returned to buddy only after all readers exit,
//       because the page refcount is held until RCU grace period)
//   3. If obj is reallocated, it contains a NEW object of the same type
//      (guaranteed: same kmem_cache → same object_size → same memory layout)
```

### The Type-Safety Property

The name "TYPESAFE" means: even if an old pointer is dereferenced after the object is freed, you'll always dereference a pointer to a valid object of the **same type** (just potentially a different instance). You won't get garbage data or a kernel pointer where an integer should be.

```c
// Reader with SLAB_TYPESAFE_BY_RCU:
rcu_read_lock();
obj = rcu_dereference(global_ptr);
// obj might be freed already, but:
//   1. obj's physical memory is still accessible (page not freed)
//   2. obj might be a NEW task_struct allocated at same address
//   3. We can check obj->id or spin_lock(obj->lock) to detect this
//
// Key pattern: validate after locking:
spin_lock(&obj->lock);
if (obj->id != expected_id) { spin_unlock; goto retry; }  // Different object!
// else: we have the right object, locked safely
spin_unlock(&obj->lock);
rcu_read_unlock();
```

Used by: `struct task_struct`, `struct files_struct`, `struct mm_struct`, network protocol objects.

### Effect on Freepointer Placement

`SLAB_TYPESAFE_BY_RCU` forces appended freepointer (see Q11):
```c
if ((flags & SLAB_TYPESAFE_BY_RCU) || ...) {
    s->offset = size;         // Freepointer AFTER object
    size += sizeof(void *);   // Object body NEVER overwritten by freelist bookkeeping
}
```

This ensures that an RCU reader won't see the freepointer (random-looking address) in the middle of what they think is a valid object field.

---

<a name="q30"></a>
## Q30: How does CPU hotplug interact with slab caches?

### The Problem

Each CPU has per-cpu sheaves (`cpu_sheaves[CPU_N]`). When CPU N goes offline:
- Its sheaves might contain objects (main sheaf, spare sheaf)
- Those objects would be "stuck" — never returned to anyone
- Memory leak until CPU N comes back online

### The Solution: `slub_cpu_dead`

```c
// Registered in kmem_cache_init():
cpuhp_setup_state_nocalls(CPUHP_SLUB_DEAD, "slub:dead", NULL, slub_cpu_dead);

// Called when any CPU goes offline:
static int slub_cpu_dead(unsigned int cpu)
{
    struct kmem_cache *s;
    
    mutex_lock(&slab_mutex);
    list_for_each_entry(s, &slab_caches, list) {
        // Flush this CPU's sheaves back to the node's barn/partial list
        flush_cpu_slab(s, cpu);
    }
    mutex_unlock(&slab_mutex);
    return 0;
}

static void flush_cpu_slab(struct kmem_cache *s, int cpu)
{
    struct slub_percpu_sheaves *pcs = per_cpu_ptr(s->cpu_sheaves, cpu);
    
    // Return objects in main and spare sheaves to their node
    if (pcs->main) {
        // For each object in main sheaf: put back on node partial list
        flush_sheaf_to_node(s, pcs->main);
        pcs->main = &empty_sheaf;
    }
    if (pcs->spare) {
        flush_sheaf_to_node(s, pcs->spare);
        pcs->spare = NULL;
    }
    // Handle rcu_free objects...
}
```

### The Workqueue Dependency

`flush_cpu_slab()` can't always run synchronously (some contexts prohibit acquiring `slab_mutex`). This is why `kmem_cache_init_late()` creates `slub_flushwq`:

```c
flushwq = alloc_workqueue("slub_flushwq", WQ_MEM_RECLAIM | WQ_PERCPU, 0);
```

When synchronous flush isn't possible, work is queued to `slub_flushwq` which runs in process context where `slab_mutex` can be acquired.

### CPU Hotplug and `slab_nodes` Mask

When a NUMA node goes offline (all its CPUs offline + memory offlined):
- `slab_memory_callback` (registered via `hotplug_node_notifier`) is called
- For each cache: free the `kmem_cache_node` for that node, remove from `s->node[N]`
- The slab_nodes bitmask is updated

When a node comes back online:
- `init_kmem_cache_nodes()` allocates a new `kmem_cache_node` for the new node
- All existing caches get the new node's state initialized

---

<a name="q31"></a>
## Q31: Why is `kmem_cache_init_late` split from `kmem_cache_init`?

### What `kmem_cache_init_late` Does

```c
// mm/slub.c:8419 — called from start_kernel() AFTER local_irq_enable()
void __init kmem_cache_init_late(void)
{
    flushwq = alloc_workqueue("slub_flushwq", WQ_MEM_RECLAIM | WQ_PERCPU, 0);
    WARN_ON(!flushwq);

#ifdef CONFIG_SLAB_FREELIST_RANDOM
    prandom_init_once(&slab_rnd_state);
#endif
}
```

### Why These Cannot Run in `kmem_cache_init`

**1. `alloc_workqueue()` requires:**
- `kmalloc()` to be working (to allocate `struct workqueue_struct`) — available only after `slab_state = UP`
- But also: per-cpu work data, which uses `cpuhp` (CPU hotplug) infrastructure
- And: `kthread_create()` for workqueue threads, which needs `schedule()` working
- Schedule works only after `sched_init()` completes

```
start_kernel() call order:
  sched_init()             ← scheduler ready
  ...
  mm_core_init()
    kmem_cache_init()      ← slab UP, but workqueue not yet possible
  ...
  workqueue_init_early()   ← basic workqueue infra setup (before IRQ)
  early_irq_enable()
  local_irq_enable()
  kmem_cache_init_late()   ← NOW workqueue fully functional, IRQs on
  workqueue_init()         ← full workqueue startup
```

**2. `prandom_init_once()` for freelist randomization:**
- Needs `/dev/random`-quality entropy
- Entropy pool is seeded from hardware events (interrupts, timer ticks)
- Interrupts are disabled in `kmem_cache_init()` (called during `mm_core_init()`)
- `local_irq_enable()` happens after all early init
- Only after IRQs are on can entropy accumulate properly

**Design principle**: Defer everything that can be deferred. `kmem_cache_init()` must set `slab_state = UP` as early as possible (many things need `kmalloc`). The deferred pieces (`flushwq`, `prandom`) can live with a brief window where they're not initialized:
- CPU hotplug can't happen before `local_irq_enable()` anyway → `flushwq` not needed yet
- Freelist ordering doesn't need to be random for the first few hundred allocations in boot

---

<a name="q32"></a>
## Q32: How does memory pressure trigger slab shrinking?

### The Pressure Signal

When the system is low on memory, `kswapd` (the kernel swap daemon) and `__alloc_pages()` can trigger memory reclaim. Part of reclaim is asking slab caches to release unused objects.

### The Shrinker Interface

```c
// Subsystem registers a shrinker:
struct shrinker fs_inode_shrinker = {
    .count_objects = inode_shrinker_count,
    .scan_objects  = inode_shrinker_scan,
    .seeks = DEFAULT_SEEKS,
};
register_shrinker(&fs_inode_shrinker, "fs-inode-cache");

// Memory pressure triggers: shrink_slab() → calls all registered shrinkers
// Each shrinker: count how many objects can be freed, then free them
```

### SLUB's Role: Automatic Empty Slab Return

SLUB automatically returns fully-empty slab pages to the buddy allocator when `node->nr_partial > s->min_partial`:

```c
// In __slab_free() (when an object is freed and the slab becomes empty):
static void discard_slab(struct kmem_cache *s, struct slab *slab)
{
    dec_slabs_node(s, slab_nid(slab), slab->objects);
    free_slab(s, slab);   // Returns page to buddy allocator
}

// In deactivate_slab() (when slab is removed from per-cpu):
if (slab->inuse == 0 && n->nr_partial >= s->min_partial)
    discard_slab(s, slab);   // Return empty slab to buddy
else
    add_partial(n, slab, DEACTIVATE_TO_TAIL);  // Keep in partial list
```

### Explicit Cache Shrinking

For caches that implement a shrinker, `kmem_cache_shrink()` aggressively drains partial lists:

```c
// mm/slub.c
int kmem_cache_shrink(struct kmem_cache *s)
{
    int node;
    struct kmem_cache_node *n;
    
    flush_all(s);   // Flush all CPU sheaves → objects back to node partial lists
    
    for_each_kmem_cache_node(s, node, n) {
        // Sort partial list so fully-empty slabs are at the end
        // Free as many as possible (keeping min_partial)
        list_for_each_entry_safe(slab, t, &n->partial, slab_list) {
            if (!slab->inuse) {
                remove_partial(n, slab);
                discard_slab(s, slab);  // Back to buddy
            }
        }
    }
    return 0;
}
```

### `SLAB_RECLAIM_ACCOUNT` and kswapd

Slab pages from `KMALLOC_RECLAIM` caches count toward `NR_SLAB_RECLAIMABLE`. kswapd monitors:
```
nr_slab_reclaimable = Σ(pages in all SLAB_RECLAIM_ACCOUNT caches)
```

When this is high relative to free pages, kswapd calls registered shrinkers to reduce it. This is how dcache (directory cache) and inode cache shrinking is triggered — they register shrinkers that call `kmem_cache_shrink()` on their respective caches.

### `min_partial` as Anti-Thrash Mechanism

If every freed slab page were immediately returned to buddy:
```
CPU-A: allocate 64 objects from kmalloc-64 → uses 1 slab page
CPU-A: free all 64 objects → slab page returned to buddy
CPU-A: allocate 64 objects again → buddy allocation needed!
```

This would cause constant buddy allocator pressure. `min_partial` keeps a few slab pages in reserve:
```
min_partial = max(MIN_PARTIAL=5, ilog2(size)/2)
```
So for `kmalloc-64` (size=64, ilog2=6, /2=3): `min_partial = max(5, 3) = 5`.
SLUB keeps at least 5 partial pages per NUMA node, ensuring rapid re-allocation without hitting buddy.

---

## Quick Reference: Interview Answer Templates

### 30-Second Answers (For Rapid-Fire Questions)

**"What is kmem_cache_init?"**
"It bootstraps the SLUB slab allocator. Uses static __initdata descriptors to break the chicken-and-egg problem, creates kmem_cache and kmem_cache_node caches, then builds 88 kmalloc caches from 8B to 2MB. After it completes, kmalloc() works."

**"What is the bootstrap problem in SLUB?"**
"Need a slab cache to allocate slab cache descriptors. Solved by static __initdata objects as placeholders, then migrated to heap via bootstrap() after the allocator is functional."

**"What is a sheaf?"**
"A per-CPU array of object pointers. Allocating is array pop, freeing is array push — no locks. Batches reduce per-slab spinlock acquisitions by N× where N is sheaf capacity."

**"How does kfree() find the right cache?"**
"virt_to_folio(ptr) → folio_slab(folio) → slab->slab_cache. The slab page's metadata (overlaying struct page) has a back-pointer to the kmem_cache descriptor."

**"What is slab_state?"**
"Global enum gating SLUB functionality: DOWN (nothing), PARTIAL (kmem_cache_node only), UP (kmalloc works), FULL (sysfs/debug ready). Guards against using not-yet-initialized paths."
