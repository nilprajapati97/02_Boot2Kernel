# `kmem_cache_init` — Detailed Code Design & Nvidia Kernel Engineer Interview Guide

> **Source**: `mm/slub.c:8362`  
> **Called from**: `mm/mm_init.c:2722` inside `mm_core_init()`  
> **Purpose**: Bootstrap the SLUB allocator — the foundation of all dynamic kernel memory allocation

---

## Table of Contents

1. [Executive Summary](#1-executive-summary)
2. [Position in the Kernel Boot Timeline](#2-position-in-the-kernel-boot-timeline)
3. [The Bootstrap Problem — Chicken-and-Egg](#3-the-bootstrap-problem--chicken-and-egg)
4. [Four-Phase Internal Sequence](#4-four-phase-internal-sequence)
5. [Data Structure Deep Dive](#5-data-structure-deep-dive)
6. [Object Layout Engine](#6-object-layout-engine)
7. [NUMA Node Initialization](#7-numa-node-initialization)
8. [Per-CPU Sheaves Fast Path](#8-per-cpu-sheaves-fast-path)
9. [kmalloc Cache Table](#9-kmalloc-cache-table)
10. [SLAB Flags Reference Table](#10-slab-flags-reference-table)
11. [Lock Hierarchy](#11-lock-hierarchy)
12. [Slab State Machine](#12-slab-state-machine)
13. [kmem_cache_init_late](#13-kmem_cache_init_late)
14. [Complete Call Graph](#14-complete-call-graph)
15. [SLUB Tuning Parameters](#15-slub-tuning-parameters)
16. [Key Terminology Glossary](#16-key-terminology-glossary)
17. [Nvidia Interview Talking Points](#17-nvidia-interview-talking-points)

---

## 1. Executive Summary

`kmem_cache_init()` is the single most important initialization function for the Linux kernel memory subsystem. It bootstraps the **SLUB allocator** — the component responsible for all fine-grained kernel memory allocations (`kmalloc`, `kzalloc`, `kmem_cache_alloc`, and virtually every kernel data structure allocation).

**What it does**: Creates the two fundamental slab caches (`kmem_cache` and `kmem_cache_node`) using a clever static-bootstrap technique, then builds the full `kmalloc` size table (8 bytes to 2 MB, across 4 memory types), and finally sets up the per-CPU fast-path structures (sheaves).

**When it runs**: During early kernel initialization, inside `mm_core_init()` at `mm/mm_init.c:2722`. At this point, the MMU is on, the buddy allocator is live, and memory zones have been populated. However, no dynamic memory allocation exists yet — not even `kmalloc(4, GFP_KERNEL)`.

**Why it matters**: After `kmem_cache_init()` returns, the kernel can call `kmalloc()` and `kmem_cache_create()` freely. Every subsequent subsystem initialization (networking, filesystems, device drivers, workqueues) depends on this working. Without it, the kernel cannot proceed.

**Key insight**: The function must create a slab cache without a slab cache existing. It solves this using `static __initdata` objects in the kernel's `.init.data` section, then migrating them to heap-allocated objects once the allocator is functional — a technique known as *bootstrapping*.

---

## 2. Position in the Kernel Boot Timeline

### 2.1 From Power-On to `kmem_cache_init`

```
┌─────────────────────────────────────────────────────────────────────────┐
│  STAGE                  │  WHAT HAPPENS                                  │
├─────────────────────────┼────────────────────────────────────────────────┤
│  CPU Reset              │  PC = reset vector; MMU off; SRAM only         │
│  head.S                 │  Enable MMU; setup page tables (idmap + swapper)│
│  __primary_switched     │  Zero BSS; set stack; jump to start_kernel     │
│  start_kernel (main.c)  │  Architecture setup, console, IRQ, CPU info    │
│  setup_arch()           │  ARM64: paging_init, memory map, zones built   │
│  build_all_zonelists()  │  NUMA zone ordering for allocation fallback     │
│  memblock_free_all()    │  Hand memblock memory to buddy allocator        │
│  mem_init()             │  vmalloc space init, highmem (ARM32 only)      │
│  ► kmem_cache_init()    │  *** SLUB ALLOCATOR BORN HERE ***              │
│  page_ext_init_flatmem_late()  │  Page extension metadata               │
│  kmemleak_init()        │  Memory leak detector (needs kmalloc)          │
│  vmalloc_init()         │  vmalloc red-black tree (needs kmalloc)        │
│  kmem_cache_init_late() │  Workqueue for slub flush (after IRQ enable)   │
│  rest_init()            │  kernel_init thread, idle thread               │
└─────────────────────────────────────────────────────────────────────────┘
```

### 2.2 Pre-Conditions (What Must Be Ready)

| Requirement | Provided By | Why Needed |
|---|---|---|
| MMU enabled, kernel mapped | `head.S` / `paging_init()` | `slab_address()` does VA arithmetic |
| Buddy allocator live | `memblock_free_all()` | `new_slab()` calls `__alloc_pages_node()` |
| Memory zones built | `build_all_zonelists()` | Zone-based allocation (DMA, NORMAL) |
| `nr_node_ids` set | `setup_arch()` | Size of `kmem_cache->node[]` array |
| `nr_cpu_ids` set | early CPU detection | `min_objects` calculation in `calculate_order()` |
| BSS zeroed | `head.S` | Static `boot_kmem_cache*` start as zero |

### 2.3 Post-Conditions (What Becomes Available)

| Capability | Enabled At |
|---|---|
| `kmem_cache_alloc_node()` | After `slab_state = PARTIAL` |
| `kmalloc()` / `kzalloc()` | After `slab_state = UP` (inside `create_kmalloc_caches()`) |
| `kmem_cache_create()` | After `slab_state = UP` |
| `kfree()` | After `slab_state = UP` |
| Async slab flush | After `kmem_cache_init_late()` |

---

## 3. The Bootstrap Problem — Chicken-and-Egg

### 3.1 The Problem Statement

SLUB requires a `struct kmem_cache` object to describe each slab cache. To allocate that `struct kmem_cache`, you need... a slab cache. Specifically:

- `kmem_cache` is a slab cache that allocates `struct kmem_cache` objects
- `kmem_cache_node` is a slab cache that allocates `struct kmem_cache_node` objects
- But creating either cache requires both to exist first

Additionally, per-NUMA-node state (`kmem_cache_node`) must be allocated *on the correct NUMA node*, requiring an already-functional slab allocator with node awareness.

### 3.2 The Solution: Static Initdata Objects

```c
// mm/slub.c:8362
void __init kmem_cache_init(void)
{
    // Allocated in .init.data section (BSS-zeroed, no heap needed)
    static __initdata struct kmem_cache      boot_kmem_cache;
    static __initdata struct kmem_cache_node boot_kmem_cache_node;
    ...
    // Point the global pointers at these static objects
    kmem_cache_node = &boot_kmem_cache_node;
    kmem_cache      = &boot_kmem_cache;
    ...
}
```

The `__initdata` qualifier places these in the `.init.data` ELF section, which:
- Is physically allocated at link time (no allocation needed)
- Is zeroed before `start_kernel()` (BSS rules)
- Is **freed** after `free_initmem()` later in boot — so objects must be migrated to heap before that

### 3.3 The `bootstrap()` Migration

After creating the static-based caches, `bootstrap()` migrates them to proper heap-allocated objects:

```c
// mm/slub.c:8271
static struct kmem_cache * __init bootstrap(struct kmem_cache *static_cache)
{
    int node;
    // Allocate a real kmem_cache object from the newly functional slab
    struct kmem_cache *s = kmem_cache_zalloc(kmem_cache, GFP_NOWAIT);
    struct kmem_cache_node *n;

    // Copy all fields from the static object to the heap object
    memcpy(s, static_cache, kmem_cache->object_size);

    // CRITICAL: patch slab->slab_cache back-pointers on every existing slab
    // (they still point to the static object address, which is about to be abandoned)
    for_each_kmem_cache_node(s, node, n) {
        struct slab *p;
        list_for_each_entry(p, &n->partial, slab_list)
            p->slab_cache = s;       // update back-pointer
#ifdef CONFIG_SLUB_DEBUG
        list_for_each_entry(p, &n->full, slab_list)
            p->slab_cache = s;
#endif
    }

    // Register in the global slab_caches list (for /proc/slabinfo, etc.)
    list_add(&s->list, &slab_caches);
    return s;
}
```

**Why back-pointer patching is critical**: Every slab page (struct slab) has a `slab_cache` field pointing to its owning `struct kmem_cache`. When an object is freed (`kfree(ptr)`), the kernel does: `virt_to_slab(ptr)->slab_cache` to find which cache to return it to. If `slab_cache` still pointed at the freed static object, the free would corrupt memory or crash.

---

## 4. Four-Phase Internal Sequence

```
slab_state:  DOWN           PARTIAL             PARTIAL             UP
             │               │                   │                   │
Phase:       1               2                   3                   4
             │               │                   │                   │
             ▼               ▼                   ▼                   ▼
         [Point globals]  [Bootstrap         [Bootstrap          [Create all
         [at static objs] [kmem_cache_node]  [kmem_cache]        [kmalloc caches]
                          [First slab page   [Migrate both        [setup_kmalloc
                          [from buddy]       [static→heap]        [_cache_index
                                                                   [_table]
```

### Phase 1 — Slab State: DOWN (Pre-initialization)

```c
// Point global pointers at static initdata (no allocation yet)
kmem_cache_node = &boot_kmem_cache_node;
kmem_cache      = &boot_kmem_cache;

// Build slab_nodes bitmask: which NUMA nodes have memory
for_each_node_state(node, N_MEMORY)
    node_set(node, slab_nodes);

// Guard page debugging: force order=0 slabs if enabled
if (debug_guardpage_minorder())
    slub_max_order = 0;

// Finalize pointer hash choice based on debug state
hash_pointers_finalize(__slub_debug_enabled());
```

At this point `slab_state == DOWN`. The `kmem_cache_node` global pointer exists but points at a zeroed static buffer. No slab pages exist.

### Phase 2 — Create `kmem_cache_node` Cache (→ PARTIAL)

```c
// Create the kmem_cache_node slab cache using the static object as descriptor
create_boot_cache(kmem_cache_node, "kmem_cache_node",
        sizeof(struct kmem_cache_node),
        SLAB_HWCACHE_ALIGN | SLAB_NO_OBJ_EXT, 0, 0);
```

Inside `create_boot_cache` → `do_kmem_cache_create`:
1. `calculate_sizes()` — determine object layout, page order
2. `init_kmem_cache_nodes()` — for each NUMA node, call `early_kmem_cache_node_alloc(node)`:
   - Calls `new_slab(kmem_cache_node, GFP_NOWAIT, node)` — **first ever** `__alloc_pages_node()` call!
   - Takes the first object from the slab's freelist as the `kmem_cache_node` for that NUMA node
   - Patches `slab->freelist` forward (first object is no longer free)
3. `init_percpu_sheaves()` — sets up per-CPU sheaf pointers

```c
// Register for CPU hotplug events (node add/remove)
hotplug_node_notifier(slab_memory_callback, SLAB_CALLBACK_PRI);

// NOW: kmem_cache_alloc_node() works for kmem_cache_node cache
slab_state = PARTIAL;
```

### Phase 3 — Create `kmem_cache` Cache and Migrate Both

```c
// Create the kmem_cache slab cache
// Note: size includes variable-length node[] array at end
create_boot_cache(kmem_cache, "kmem_cache",
        offsetof(struct kmem_cache, node) +
            nr_node_ids * sizeof(struct kmem_cache_node *),
        SLAB_HWCACHE_ALIGN | SLAB_NO_OBJ_EXT, 0, 0);

// Migrate: allocate real heap objects, copy from static, patch back-pointers
kmem_cache      = bootstrap(&boot_kmem_cache);
kmem_cache_node = bootstrap(&boot_kmem_cache_node);
```

After `bootstrap()` returns:
- `kmem_cache` global → heap-allocated `struct kmem_cache` (managed by SLUB)
- `kmem_cache_node` global → heap-allocated `struct kmem_cache` (managed by SLUB)
- All `slab->slab_cache` back-pointers updated
- Both caches added to `slab_caches` list
- Static `boot_kmem_cache*` objects are now dead (but won't be freed until `free_initmem()`)

### Phase 4 — Build kmalloc Cache Table (→ UP)

```c
// Build the O(1) size-to-index lookup table
setup_kmalloc_cache_index_table();

// Create all kmalloc caches (this sets slab_state = UP internally)
create_kmalloc_caches();

// Set up sheaves (batch allocators) for kmalloc caches
bootstrap_kmalloc_sheaves();

// Randomize freelist ordering (security hardening)
init_freelist_randomization();

// Register CPU hotplug callback for slab CPU teardown
cpuhp_setup_state_nocalls(CPUHP_SLUB_DEAD, "slub:dead", NULL, slub_cpu_dead);
```

---

## 5. Data Structure Deep Dive

### 5.1 `struct kmem_cache` — The Cache Descriptor

```c
// mm/slab.h:197
struct kmem_cache {
    // ── Fast-path data (hot cache line) ──────────────────────────────
    struct slub_percpu_sheaves __percpu *cpu_sheaves;
                            // Per-CPU allocation fast-path; pointer to
                            // per-cpu array of slab_sheaf structs
    slab_flags_t flags;     // SLAB_HWCACHE_ALIGN, SLAB_PANIC, etc.
    unsigned long min_partial;
                            // Min # of partial slabs to keep per node
                            // (watermark to avoid thrashing)

    // ── Size accounting ───────────────────────────────────────────────
    unsigned int size;      // Actual allocation size (object_size + metadata)
    unsigned int object_size;
                            // Pure user-visible object size (what caller asked for)
    struct reciprocal_value reciprocal_size;
                            // Precomputed: used for division-free slab index calc
                            // index = (offset * reciprocal_size.m) >> reciprocal_size.sh
    unsigned int offset;    // Offset of free-pointer within object
                            // (inline freepointer for SLUB "embedded" mode)

    // ── Slab order/count ─────────────────────────────────────────────
    unsigned int sheaf_capacity;
                            // Objects per sheaf (0 = debug/tiny caches, no sheaves)
    struct kmem_cache_order_objects oo;
                            // Preferred: packed (page_order << OO_SHIFT | obj_count)
    struct kmem_cache_order_objects min;
                            // Fallback (order=0 or minimum viable)

    // ── Allocation flags ─────────────────────────────────────────────
    gfp_t allocflags;       // Added to every alloc: __GFP_COMP, GFP_DMA, etc.
    int refcount;           // -1 = exempt from merging; >0 = merge count
    void (*ctor)(void *);   // Optional constructor called after alloc

    // ── Metadata offsets ─────────────────────────────────────────────
    unsigned int inuse;     // Usable bytes (size before any debug padding)
    unsigned int align;     // Final alignment (max of object, arch, cache-line)
    unsigned int red_left_pad; // Left redzone size (SLUB_DEBUG only)
    const char *name;       // Cache name (e.g., "kmalloc-64", "task_struct")
    struct list_head list;  // Entry in global slab_caches list

    // ── Optional/config-gated fields ─────────────────────────────────
#ifdef CONFIG_SYSFS
    struct kobject kobj;    // /sys/kernel/slab/<name>
#endif
#ifdef CONFIG_SLAB_FREELIST_HARDENED
    unsigned long random;   // Per-cache XOR key for freelist pointer obfuscation
#endif
#ifdef CONFIG_SLAB_FREELIST_RANDOM
    unsigned int *random_seq; // Randomized object order within a slab
#endif
#ifdef CONFIG_KASAN_GENERIC
    struct kasan_cache kasan_info;  // KASAN shadow metadata
#endif
#ifdef CONFIG_HARDENED_USERCOPY
    unsigned int useroffset;  // Start of user-copyable region
    unsigned int usersize;    // Length of user-copyable region
#endif
#ifdef CONFIG_NUMA
    unsigned int remote_node_defrag_ratio;
                            // Threshold for cross-node defrag (0=never, 100=always)
#endif

    // ── Per-NUMA-node state (variable length!) ────────────────────────
    struct kmem_cache_node *node[MAX_NUMNODES];
                            // Pointer to per-node state; array length = nr_node_ids
                            // This is why create_boot_cache uses offsetof()+nr_node_ids*sizeof(ptr)
};
```

**Key insight for interviews**: The `node[]` array at the end is what makes `struct kmem_cache` *variable-length*. The size passed to `create_boot_cache` is:
```c
offsetof(struct kmem_cache, node) + nr_node_ids * sizeof(struct kmem_cache_node *)
```
This is why `kmem_cache` must be the *second* cache created — it needs `nr_node_ids` which is set up during `setup_arch()`.

### 5.2 `struct kmem_cache_node` — Per-NUMA-Node State

```c
// mm/slub.c:430
struct kmem_cache_node {
    spinlock_t list_lock;       // Protects partial and full lists
    unsigned long nr_partial;   // Count of partial slabs (for watermark checks)
    struct list_head partial;   // List of partially-used slab pages
                                // (has free objects but also used objects)
#ifdef CONFIG_SLUB_DEBUG
    atomic_long_t nr_slabs;     // Total slab count (full + partial)
    atomic_long_t total_objects;// Total object slots
    struct list_head full;      // Fully-used slabs (DEBUG only; tracked for leak detection)
#endif
    struct node_barn *barn;     // Per-node sheaf pool (new in recent kernels)
};
```

**Why per-node?**: On NUMA systems, allocating from a remote node is expensive (100+ ns vs 10 ns). Each cache has a `kmem_cache_node` per NUMA node so that local allocations stay local. `kmem_cache_alloc_node(cache, flags, node)` hits only the target node's partial list.

### 5.3 `struct slab` — Page-to-Slab Bridge

```c
// mm/slab.h:74
struct slab {
    memdesc_flags_t flags;          // PG_slab set; NUMA/zone bits
    struct kmem_cache *slab_cache;  // ← back-pointer to owning cache
                                    // Overlays struct page::compound_head
                                    // bit 0 MUST be 0 (not a tail page)
    union {
        struct {
            struct list_head slab_list;  // Link in node->partial or node->full
            union {
                struct {
                    void *freelist;      // Linked list of free objects
                    union {
                        unsigned long counters;
                        struct {
                            unsigned inuse:16;   // Objects in use
                            unsigned objects:15; // Total objects in slab
                            unsigned frozen:1;   // CPU owns this slab (no contention)
                        };
                    };
                };
            };
        };
        struct rcu_head rcu_head;    // For deferred slab freeing
    };
    unsigned int __page_type;       // Encodes PGTY_slab
    atomic_t __page_refcount;       // Page refcount
#ifdef CONFIG_SLAB_OBJ_EXT
    unsigned long obj_exts;         // Extended per-object metadata pointer
#endif
};

// Compile-time assertions that struct slab overlays struct page correctly:
SLAB_MATCH(flags, flags);
SLAB_MATCH(compound_head, slab_cache);   // Must be at same offset
SLAB_MATCH(_refcount, __page_refcount);
static_assert(sizeof(struct slab) <= sizeof(struct page));
```

**The overlay trick**: `struct slab` and `struct page` start at the same physical address. The kernel casts between them via:
```c
static inline struct slab *page_slab(const struct page *page) {
    return (struct slab *)page;
}
static inline struct folio *slab_folio(const struct slab *slab) {
    return (struct folio *)slab;
}
static inline void *slab_address(const struct slab *slab) {
    return folio_address(slab_folio(slab));  // VA of first byte of slab data
}
```

### 5.4 `struct slub_percpu_sheaves` — Per-CPU Fast Path

```c
// mm/slub.c:420
struct slub_percpu_sheaves {
    local_trylock_t lock;     // Trylock: if CPU is preempted mid-alloc, fall through
    struct slab_sheaf *main;  // Active sheaf: pop objects from tail; NEVER NULL
    struct slab_sheaf *spare; // Pre-loaded empty (for alloc) or full (for free) sheaf
    struct slab_sheaf *rcu_free; // Accumulates kfree_rcu() pointers for batched free
};
```

### 5.5 `struct slab_sheaf` — Object Batch Container

```c
// mm/slub.c:404
struct slab_sheaf {
    union {
        struct rcu_head rcu_head;    // For sheaf-level deferred free
        struct list_head barn_list;  // Entry in node_barn->sheaves_full/empty
        struct {
            unsigned int capacity;   // Max objects this sheaf can hold
            bool pfmemalloc;         // Came from PF_MEMALLOC reservation
        };
    };
    struct kmem_cache *cache;   // Owning cache
    unsigned int size;          // Current object count (0..capacity)
    int node;                   // NUMA node affinity
    void *objects[];            // Flexible array: pointers to free objects
};
```

A sheaf is simply an **array of pointers to free objects**. Allocation = `sheaf->objects[--sheaf->size]`. Free = `sheaf->objects[sheaf->size++] = ptr`. No lock needed on the fast path because each CPU has its own sheaf and the local_trylock prevents concurrent access from the same CPU.

### 5.6 `struct node_barn` — Per-Node Sheaf Pool

```c
// mm/slub.c:396
struct node_barn {
    spinlock_t lock;
    struct list_head sheaves_full;   // Full sheaves (ready to hand to CPU for alloc)
    struct list_head sheaves_empty;  // Empty sheaves (ready to hand to CPU for free)
    unsigned int nr_full;
    unsigned int nr_empty;
};
```

The barn is the exchange point between CPUs and the node's slab pages. When a CPU's main sheaf is empty, it checks the barn for a full one. When full, it deposits it in the barn.

### 5.7 `struct kmem_cache_order_objects` — Packed Encoding

```c
// mm/slab.h:190
struct kmem_cache_order_objects { unsigned int x; };

// Encoding: upper (32-OO_SHIFT) bits = page order; lower OO_SHIFT bits = object count
#define OO_SHIFT   16
#define OO_MASK    ((1 << OO_SHIFT) - 1)

static inline struct kmem_cache_order_objects oo_make(unsigned int order, unsigned int size) {
    struct kmem_cache_order_objects x = {
        (order << OO_SHIFT) + order_objects(order, size)
    };
    return x;
}
static inline unsigned int oo_order(struct kmem_cache_order_objects x) { return x.x >> OO_SHIFT; }
static inline unsigned int oo_objects(struct kmem_cache_order_objects x) { return x.x & OO_MASK; }
```

This packs both "page order" and "objects per slab" into a single `u32`, which fits in one cache line word. Saves memory and improves cache performance in the hot allocation path.

---

## 6. Object Layout Engine

### 6.1 `calculate_sizes()` — How Object Layout is Determined

```c
// mm/slub.c:7661  (simplified)
static int calculate_sizes(struct kmem_cache_args *args, struct kmem_cache *s)
{
    slab_flags_t flags = s->flags;
    unsigned int size = s->object_size;

    // Step 1: Align object to pointer size (ensures freepointer is accessible)
    size = ALIGN(size, sizeof(void *));

#ifdef CONFIG_SLUB_DEBUG
    // Step 2 (DEBUG): Add right redzone for overflow detection
    if (flags & SLAB_RED_ZONE)
        size = ALIGN(size, s->align);
    // s->red_left_pad and right_pad sizes computed here
#endif

    // Step 3: Record "inuse" — bytes before debug metadata (= what user can access)
    s->inuse = size;

    // Step 4: Place the free pointer
    if ((flags & SLAB_TYPESAFE_BY_RCU) || (flags & SLAB_POISON) || s->ctor) {
        // Cannot reuse object memory for freeptr (object may still be accessed!)
        s->offset = size;         // Freeptr appended AFTER object
        size += sizeof(void *);   // Grows total size
    } else {
        // Safe to embed freeptr inside object body (object is zeroed/unused when free)
        // Place at halfway point to maximize distance from start (reduces false-sharing)
        s->offset = ALIGN_DOWN(s->object_size / 2, sizeof(void *));
    }

    // Step 5: KASAN may add metadata/shadow bytes
    kasan_cache_create(s, &size, &s->flags);

    // Step 6: Align to final alignment (max of type, arch, cache-line if HWCACHE_ALIGN)
    size = ALIGN(size, s->align);
    s->size = size;

    // Step 7: Precompute reciprocal for division-free address-to-index calculation
    s->reciprocal_size = reciprocal_value(size);

    // Step 8: Determine page order for this cache
    if (s->oo.x)   // Already set (e.g., by userspace via sysfs)
        goto verify;
    s->oo = oo_make(calculate_order(size), size);
    s->min = oo_make(get_order(size), size);   // Minimum: one object

    // Step 9: Set GFP flags for page allocation
    s->allocflags = __GFP_COMP;                  // Always compound pages
    if (s->flags & SLAB_CACHE_DMA)   s->allocflags |= GFP_DMA;
    if (s->flags & SLAB_CACHE_DMA32) s->allocflags |= GFP_DMA32;
    if (s->flags & SLAB_RECLAIM_ACCOUNT) s->allocflags |= __GFP_RECLAIMABLE;
    ...
}
```

### 6.2 `calculate_order()` — Choosing the Page Order

```c
// mm/slub.c:7366  (simplified)
static inline int calculate_order(unsigned int size)
{
    unsigned int order, min_order, max_objects;
    unsigned int min_objects = slub_min_objects;

    // Auto-compute min_objects based on CPU count:
    // More CPUs → more contention → bigger slabs → fewer refills needed
    if (!min_objects) {
        unsigned int nr_cpus = num_present_cpus();
        if (nr_cpus <= 1) nr_cpus = nr_cpu_ids;
        min_objects = 4 * (fls(nr_cpus) + 1);
        // fls(8 CPUs) = 4 → min_objects = 20
        // fls(128 CPUs) = 8 → min_objects = 36
    }

    max_objects = max(order_objects(slub_max_order, size), 1U);
    min_objects = min(min_objects, max_objects);

    min_order = max_t(unsigned int, slub_min_order,
                      get_order(min_objects * size));

    // Try decreasing waste tolerance: 1/16 first (tight packing), then 1/8, 1/4, 1/2
    for (unsigned int fraction = 16; fraction > 1; fraction /= 2) {
        order = calc_slab_order(size, min_order, slub_max_order, fraction);
        if (order <= slub_max_order)
            return order;
    }

    // Last resort: just enough for one object
    order = get_order(size);
    if (order <= MAX_PAGE_ORDER) return order;
    return -ENOSYS;  // Object too large
}
```

**The waste fraction algorithm**: `calc_slab_order` finds the smallest order where wasted bytes (due to packing mismatch) are ≤ 1/fraction of total slab size. Starting at 1/16 (6.25% waste) ensures tight packing. Relaxing to 1/2 (50% waste) is a last resort for odd-sized objects.

### 6.3 Object Layout Diagram

```
Slab Page (order=0, 4KB example, kmalloc-64):
┌─────────────────────────────────────────────────────────────┐
│  struct slab (overlays struct page header — NOT in slab data)│
│  [lives in mem_map[], NOT in the slab page itself]           │
└─────────────────────────────────────────────────────────────┘

Physical slab page data (4096 bytes):
┌──────────────────────────────────────────────────────────────┐
│ Object 0 [64 bytes]                                          │
│  ├─ bytes 0..31: user data                                   │
│  ├─ bytes 32..39: ◄── freepointer (when object is FREE)      │
│  │                    (overwritten with user data when USED)  │
│  └─ bytes 40..63: user data                                  │
├──────────────────────────────────────────────────────────────┤
│ Object 1 [64 bytes]   (freepointer → Object 3)              │
├──────────────────────────────────────────────────────────────┤
│ Object 2 [64 bytes]   (in use — no freepointer)             │
├──────────────────────────────────────────────────────────────┤
│  ...  (64 objects total in 4KB page)                        │
└──────────────────────────────────────────────────────────────┘

Freelist (slab->freelist → obj1 → obj3 → obj7 → ... → NULL):
  slab->freelist ──► [obj1.freeptr] ──► [obj3.freeptr] ──► NULL

With SLAB_TYPESAFE_BY_RCU or SLAB_POISON (appended freepointer):
┌──────────────────────────────────────────────────────────────┐
│ Object 0 [object_size bytes | freepointer [8 bytes]]         │
│ Object 1 [object_size bytes | freepointer [8 bytes]]         │
│  ...                                                         │
└──────────────────────────────────────────────────────────────┘
```

### 6.4 `reciprocal_size` — Division-Free Index Calculation

When freeing an object, SLUB needs to find which slab it belongs to (to update `slab->inuse`). It computes `(ptr - slab_start) / object_size` to get the object index. Division is expensive on some architectures. `reciprocal_size` precomputes a magic multiplier so this becomes a multiply-and-shift:

```c
// Fast: index = obj_to_index(s, slab, obj)
//   = (unsigned)(obj - slab_address(slab)) * reciprocal_size.m >> reciprocal_size.sh
```

---

## 7. NUMA Node Initialization

### 7.1 The `slab_nodes` Bitmask

```c
// mm/slub.c  — global
static nodemask_t slab_nodes;

// In kmem_cache_init():
for_each_node_state(node, N_MEMORY)
    node_set(node, slab_nodes);
```

`N_MEMORY` nodes are those with actual RAM (as opposed to `N_POSSIBLE` which includes hotplug slots with no memory yet). This determines which nodes get a `kmem_cache_node` per cache.

### 7.2 `early_kmem_cache_node_alloc()` — Bootstrap Path

```c
// mm/slub.c:7514 — called when slab_state == DOWN (FIRST create_boot_cache only)
static void early_kmem_cache_node_alloc(int node)
{
    // Step 1: Allocate a raw slab page from the buddy allocator
    struct slab *slab = new_slab(kmem_cache_node, GFP_NOWAIT, node);
    struct kmem_cache_node *n;

    BUG_ON(!slab);

    // Step 2: Take the FIRST object from the freelist as our kmem_cache_node
    n = slab->freelist;
    BUG_ON(!n);
    n = kasan_slab_alloc(kmem_cache_node, n, GFP_KERNEL, false);

    // Step 3: Advance freelist past the consumed object
    slab->freelist = get_freepointer(kmem_cache_node, n);
    slab->inuse = 1;

    // Step 4: Point this NUMA node's cache state at our new struct
    kmem_cache_node->node[node] = n;
    init_kmem_cache_node(n, NULL);

    // Step 5: Account for slab allocation; put the slab in the partial list
    inc_slabs_node(kmem_cache_node, node, slab->objects);
    __add_partial(n, slab, ADD_TO_HEAD);
}
```

This is the **most delicate** part of boot: allocating a `struct kmem_cache_node` without a `kmem_cache_node` cache. It works by directly manipulating the slab's freelist, bypassing all normal allocation paths.

### 7.3 Normal Node Allocation Path (Post-PARTIAL)

```c
// mm/slub.c:7576 — after slab_state >= PARTIAL
static int init_kmem_cache_nodes(struct kmem_cache *s)
{
    int node;
    for_each_node_mask(node, slab_nodes) {
        struct kmem_cache_node *n;

        if (slab_state == DOWN) {
            early_kmem_cache_node_alloc(node);  // Special bootstrap path
            continue;
        }
        // Normal path: use the now-functional slab allocator
        n = kmem_cache_alloc_node(kmem_cache_node, GFP_KERNEL, node);
        if (!n) return 0;
        init_kmem_cache_node(n, NULL);
        s->node[node] = n;
    }
    return 1;
}
```

### 7.4 `min_partial` Watermark

```c
// In do_kmem_cache_create():
s->min_partial = min_t(unsigned long, MAX_PARTIAL, ilog2(s->size) / 2);
s->min_partial = max_t(unsigned long, MIN_PARTIAL, s->min_partial);
```

- `min_partial` controls how many partial slabs to keep in `node->partial` even when idle
- Too low → frequent slab alloc/free thrashing (buddy pressure)
- Too high → memory waste
- Formula: scales with `log2(object_size)` — larger objects → fewer partial slabs needed

---

## 8. Per-CPU Sheaves Fast Path

### 8.1 Why Per-CPU Allocation

Before sheaves (old per-cpu slab), SLUB had a per-cpu "slab page" that each CPU directly allocated from. This caused:
- **Cache line pingpong**: When CPU-A frees an object that CPU-B allocated, the slab page metadata (freelist, counters) bounces between CPU caches
- **Lock contention**: Any modification to a shared slab needs locking

Sheaves solve this by batching: each CPU holds a **sheaf** (array of pointers), so N consecutive allocations only touch the local sheaf, not any shared state.

### 8.2 Fast-Path Allocation Flow

```
kmem_cache_alloc(s, GFP_KERNEL)
│
├─ get_cpu_ptr(s->cpu_sheaves)  [disable preemption]
│
├─ local_trylock(&pcs->lock)    [acquire trylock]
│   └─ if failed → slow path
│
├─ if pcs->main->size > 0:      [sheaf has objects?]
│   └─ obj = pcs->main->objects[--pcs->main->size]  [POP — no other locks!]
│   └─ local_tryunlock; put_cpu_ptr; return obj      [FAST PATH — done]
│
├─ else (main sheaf empty):
│   └─ swap main ↔ spare        [try spare sheaf first]
│   └─ if spare was full → same fast pop → done
│
└─ slow path: refill from node->barn or new slab page
```

### 8.3 Bootstrap Sheaf Setup

During `kmem_cache_init`, sheaves cannot be allocated yet (too early). A static `bootstrap_sheaf` with `capacity=0` is used:

```c
// mm/slub.c:7461
static int init_percpu_sheaves(struct kmem_cache *s)
{
    static struct slab_sheaf bootstrap_sheaf = {};   // capacity=0, size=0

    for_each_possible_cpu(cpu) {
        struct slub_percpu_sheaves *pcs = per_cpu_ptr(s->cpu_sheaves, cpu);
        local_trylock_init(&pcs->lock);
        if (!s->sheaf_capacity)
            pcs->main = &bootstrap_sheaf;   // Debug/early caches: no real sheaf
        else
            pcs->main = alloc_empty_sheaf(s, GFP_KERNEL);
        if (!pcs->main) return -ENOMEM;
    }
    return 0;
}
```

After `create_kmalloc_caches()`, `bootstrap_kmalloc_sheaves()` replaces `bootstrap_sheaf` with real per-cpu sheaves for all kmalloc caches.

---

## 9. kmalloc Cache Table

### 9.1 `kmalloc_info[]` — Size Table

```c
// mm/slab_common.c:861
// Index 0=0B, 1=96B, 2=192B, 3=8B, 4=16B, ..., 21=2M
const struct kmalloc_info_struct kmalloc_info[] __initconst = {
    INIT_KMALLOC_INFO(0, 0),      // [0]  — special: 0-byte allocations
    INIT_KMALLOC_INFO(96, 96),    // [1]  — 96 bytes
    INIT_KMALLOC_INFO(192, 192),  // [2]  — 192 bytes
    INIT_KMALLOC_INFO(8, 8),      // [3]  — 8 bytes
    INIT_KMALLOC_INFO(16, 16),    // [4]  — 16 bytes
    INIT_KMALLOC_INFO(32, 32),    // [5]  — 32 bytes
    INIT_KMALLOC_INFO(64, 64),    // [6]  — 64 bytes
    INIT_KMALLOC_INFO(128, 128),  // [7]  — 128 bytes
    INIT_KMALLOC_INFO(256, 256),  // [8]  — 256 bytes
    INIT_KMALLOC_INFO(512, 512),  // [9]  — 512 bytes
    INIT_KMALLOC_INFO(1024, 1k),  // [10] — 1 KB
    INIT_KMALLOC_INFO(2048, 2k),  // [11] — 2 KB
    INIT_KMALLOC_INFO(4096, 4k),  // [12] — 4 KB
    INIT_KMALLOC_INFO(8192, 8k),  // [13] — 8 KB
    INIT_KMALLOC_INFO(16384, 16k),// [14] — 16 KB
    INIT_KMALLOC_INFO(32768, 32k),// [15] — 32 KB
    INIT_KMALLOC_INFO(65536, 64k),// [16] — 64 KB
    INIT_KMALLOC_INFO(131072, 128k),// [17] — 128 KB
    INIT_KMALLOC_INFO(262144, 256k),// [18] — 256 KB
    INIT_KMALLOC_INFO(524288, 512k),// [19] — 512 KB
    INIT_KMALLOC_INFO(1048576, 1M), // [20] — 1 MB
    INIT_KMALLOC_INFO(2097152, 2M), // [21] — 2 MB
};
```

Note: Indices 1 (96B) and 2 (192B) exist to reduce waste for objects that don't fit neatly in 64B or 128B buckets. A 96-byte `struct` would waste 25% in a 128B bucket; the 96B cache wastes only 0%.

### 9.2 Four kmalloc Cache Types

```c
// mm/slab_common.c:994
void __init create_kmalloc_caches(void)
{
    enum kmalloc_cache_type type;

    for (type = KMALLOC_NORMAL; type < NR_KMALLOC_TYPES; type++) {
        // Create 96B and 192B caches only if KMALLOC_MIN_SIZE allows
        if (KMALLOC_MIN_SIZE <= 32)  new_kmalloc_cache(1, type);  // 96B
        if (KMALLOC_MIN_SIZE <= 64)  new_kmalloc_cache(2, type);  // 192B

        // Create power-of-2 caches from KMALLOC_SHIFT_LOW to KMALLOC_SHIFT_HIGH
        for (int i = KMALLOC_SHIFT_LOW; i <= KMALLOC_SHIFT_HIGH; i++)
            new_kmalloc_cache(i, type);
    }
    slab_state = UP;    // ← kmalloc() becomes usable HERE
    ...
}
```

| Type | Enum Value | Extra Flag | Use Case |
|---|---|---|---|
| `KMALLOC_NORMAL` | 0 | none | Most kernel allocations |
| `KMALLOC_RECLAIM` | 1 | `SLAB_RECLAIM_ACCOUNT` | Pages counted as reclaimable by kswapd |
| `KMALLOC_CGROUP` | 2 | `SLAB_ACCOUNT` | Memory cgroup accounting (memcg) |
| `KMALLOC_DMA` | 3 | `SLAB_CACHE_DMA` | Physically contiguous, DMA-accessible |

### 9.3 `setup_kmalloc_cache_index_table()` — O(1) Size Lookup

```c
// Returns the kmalloc_caches index for a given allocation size:
// kmalloc_index(size) = fls(size - 1) when size > 192
// kmalloc_index(size) = special table entry for small sizes
// Runtime: kmalloc(N, gfp) → kmalloc_caches[type][kmalloc_index(N)]
```

### 9.4 Random kmalloc Caches (Security)

```c
#ifdef CONFIG_RANDOM_KMALLOC_CACHES
    random_kmalloc_seed = get_random_u64();
#endif
```

When enabled, `kmalloc()` randomly selects among multiple caches of the same size, making heap spray attacks harder (attacker cannot reliably predict where objects land).

---

## 10. SLAB Flags Reference Table

| Flag | Bit | Set In kmem_cache_init | Effect |
|---|---|---|---|
| `SLAB_HWCACHE_ALIGN` | `_SLAB_HWCACHE_ALIGN` | `create_boot_cache()` | Align objects to cache line (64B on ARM64); reduces false sharing |
| `SLAB_NO_OBJ_EXT` | `_SLAB_NO_OBJ_EXT` | `create_boot_cache()` | No per-object extension metadata (used for bootstrap caches) |
| `SLAB_PANIC` | `_SLAB_PANIC` | Many driver caches | Panic instead of returning NULL on failure |
| `SLAB_TYPESAFE_BY_RCU` | `_SLAB_TYPESAFE_BY_RCU` | RCU-safe caches | Defer slab page free until RCU grace period; freepointer appended |
| `SLAB_RECLAIM_ACCOUNT` | `_SLAB_RECLAIM_ACCOUNT` | KMALLOC_RECLAIM | Pages counted in NR_SLAB_RECLAIMABLE; kswapd can reclaim |
| `SLAB_ACCOUNT` | `_SLAB_ACCOUNT` | KMALLOC_CGROUP | Per-cgroup memory accounting |
| `SLAB_CACHE_DMA` | `_SLAB_CACHE_DMA` | KMALLOC_DMA | `GFP_DMA` added to allocflags |
| `SLAB_CACHE_DMA32` | `_SLAB_CACHE_DMA32` | Some arch caches | `GFP_DMA32` added (first 4GB) |
| `SLAB_POISON` | `_SLAB_POISON` | Debug builds | Fill with 0x6b after alloc, 0xa5 after free; detect use-after-free |
| `SLAB_RED_ZONE` | `_SLAB_RED_ZONE` | Debug builds | Add guard bytes around objects; detect buffer overflow |
| `SLAB_STORE_USER` | `_SLAB_STORE_USER` | Debug builds | Store last alloc/free caller at end of object |
| `SLAB_NO_MERGE` | `_SLAB_NO_MERGE` | Driver caches | Prevent cache merging with compatible caches |
| `SLAB_KMALLOC` | `_SLAB_KMALLOC` | kmalloc caches | Internal: marks cache as a kmalloc cache |
| `SLAB_CONSISTENCY_CHECKS` | `_SLAB_CONSISTENCY_CHECKS` | Debug builds | Run expensive consistency checks on alloc/free |

---

## 11. Lock Hierarchy

SLUB has a carefully documented 5-level lock hierarchy (from `mm/slub.c:58`):

```
Level 0: cpu_hotplug_lock (rwsem)
│  Protects: CPU online/offline transitions
│  Acquired by: cpu_up(), cpu_down(), kmem_cache_create/destroy
│
Level 1: slab_mutex (mutex)
│  Protects: slab_caches linked list; kmem_cache_create/destroy
│  Acquired: only from process context, never in alloc/free hot path
│
Level 2a: cpu_sheaves->lock (local_trylock)
│  Protects: per-cpu sheaf pointers (main, spare, rcu_free)
│  Property: TRYLOCK — if preempted, fall to slow path (no deadlock risk)
│  Scope: per-cpu, so no cross-CPU contention
│
Level 2b: node->barn->lock (spinlock)
│  Protects: per-node sheaf pool (sheaves_full, sheaves_empty lists)
│  Contention: multiple CPUs exchanging sheaves
│
Level 2c: node->list_lock (spinlock)
│  Protects: node->partial list (partial slab pages)
│  Acquired: when refilling from/depositing to NUMA-node slab list
│
Level 3: slab_lock(slab) (bit spinlock — only on arches without cmpxchg_double)
│  Protects: individual slab's freelist + counters (frozen bit)
│  On x86/ARM64: cmpxchg_double() used instead (lockless!)
│
Level 4: object_map_lock (spinlock — SLUB_DEBUG only)
   Protects: per-object debugging state bitmap
```

**Key invariant**: A lock at level N can only be acquired while holding locks at levels < N. Never acquire `list_lock` while holding `slab_mutex` and never acquire `slab_mutex` while in an interrupt context.

**The fast path is nearly lock-free**: allocation/free on the same CPU goes: `local_trylock(cpu_sheaves->lock)` → pop/push pointer → `local_tryunlock`. No spinlocks, no atomic operations on shared data.

---

## 12. Slab State Machine

```c
// mm/slab.h:325
enum slab_state {
    DOWN,    // No slab functionality
    PARTIAL, // kmem_cache_node allocations possible (only!)
    UP,      // Full kmalloc/kmem_cache_create available
    FULL     // Everything including sysfs/debug initialized
};
```

| State | Transition Trigger | What Becomes Possible |
|---|---|---|
| `DOWN` | Initial | Nothing — static initdata only |
| `PARTIAL` | After `create_boot_cache(kmem_cache_node)` + `hotplug_node_notifier` | `kmem_cache_alloc_node(kmem_cache_node, ...)` |
| `UP` | Inside `create_kmalloc_caches()` after all sizes created | `kmalloc()`, `kzalloc()`, `kmem_cache_create()`, `kfree()` |
| `FULL` | After `slab_init_debugfs()` / late init | `/sys/kernel/slab/`, debug stats, RCU sheaf free |

**Why the ordering matters**: `init_kmem_cache_nodes()` checks `slab_state == DOWN` to decide between the bootstrap path (`early_kmem_cache_node_alloc`) and normal `kmem_cache_alloc_node`. The state must be set **after** the bootstrap path completes and **before** any code tries to use the normal path.

---

## 13. `kmem_cache_init_late`

```c
// mm/slub.c:8419
// Called from start_kernel() AFTER local_irq_enable()
void __init kmem_cache_init_late(void)
{
    // Create async workqueue for deferred slab flush
    // (needed when sheaves are stolen from dead CPUs during CPU hotplug)
    flushwq = alloc_workqueue("slub_flushwq", WQ_MEM_RECLAIM | WQ_PERCPU, 0);
    WARN_ON(!flushwq);

#ifdef CONFIG_SLAB_FREELIST_RANDOM
    // Initialize PRNG state for freelist randomization
    prandom_init_once(&slab_rnd_state);
#endif
}
```

**Why deferred?**: `alloc_workqueue()` itself needs `kmalloc()` to allocate the workqueue structure — it cannot be called before `slab_state = UP`. More importantly, workqueue init requires interrupt handlers and per-cpu storage to be ready, which only happens after `local_irq_enable()`.

---

## 14. Complete Call Graph

```
start_kernel()                                     [init/main.c:1008]
└── mm_core_init()                                 [mm/mm_init.c:2694]
    ├── build_all_zonelists()
    ├── memblock_free_all()
    ├── mem_init()
    └── kmem_cache_init()                          [mm/slub.c:8362]
        │
        ├── [Phase 1: DOWN]
        │   ├── debug_guardpage_minorder()
        │   ├── hash_pointers_finalize()
        │   ├── kmem_cache_node = &boot_kmem_cache_node
        │   ├── kmem_cache = &boot_kmem_cache
        │   └── for_each_node_state → node_set(slab_nodes)
        │
        ├── [Phase 2: →PARTIAL]
        │   ├── create_boot_cache(kmem_cache_node, ...)    [mm/slab_common.c:695]
        │   │   └── do_kmem_cache_create()                 [mm/slub.c:8429]
        │   │       ├── calculate_sizes()                  [mm/slub.c:7661]
        │   │       │   └── calculate_order()              [mm/slub.c:7366]
        │   │       │       └── calc_slab_order()
        │   │       ├── init_kmem_cache_nodes()            [mm/slub.c:7576]
        │   │       │   └── early_kmem_cache_node_alloc()  [mm/slub.c:7514]
        │   │       │       ├── new_slab()                 [mm/slub.c — first buddy alloc!]
        │   │       │       ├── init_kmem_cache_node()     [mm/slub.c:7430]
        │   │       │       └── __add_partial()
        │   │       └── init_percpu_sheaves()              [mm/slub.c:7461]
        │   ├── hotplug_node_notifier()
        │   └── slab_state = PARTIAL
        │
        ├── [Phase 3: Bootstrap migration]
        │   ├── create_boot_cache(kmem_cache, ...)         [same as above]
        │   │   └── do_kmem_cache_create()
        │   │       └── init_kmem_cache_nodes()            [now uses kmem_cache_alloc_node]
        │   ├── kmem_cache = bootstrap(&boot_kmem_cache)   [mm/slub.c:8271]
        │   │   ├── kmem_cache_zalloc()
        │   │   ├── memcpy()
        │   │   └── patch slab->slab_cache backptrs
        │   └── kmem_cache_node = bootstrap(&boot_kmem_cache_node)
        │
        └── [Phase 4: →UP]
            ├── setup_kmalloc_cache_index_table()          [mm/slab_common.c:897]
            ├── create_kmalloc_caches()                    [mm/slab_common.c:994]
            │   └── new_kmalloc_cache() × 22 × 4_types    [mm/slab_common.c:945]
            │       └── create_kmalloc_cache()             [mm/slab_common.c:726]
            │           └── create_boot_cache()
            │               └── do_kmem_cache_create()
            ├── [slab_state = UP]  ← inside create_kmalloc_caches
            ├── bootstrap_kmalloc_sheaves()                [mm/slub.c:8350]
            │   └── bootstrap_cache_sheaves() × each cache [mm/slub.c:8299]
            │       ├── alloc barn per node
            │       └── alloc_empty_sheaf() per cpu
            ├── init_freelist_randomization()              [mm/slub.c:3334]
            └── cpuhp_setup_state_nocalls(CPUHP_SLUB_DEAD)

start_kernel() (later, after local_irq_enable())
└── kmem_cache_init_late()                         [mm/slub.c:8419]
    ├── alloc_workqueue("slub_flushwq", ...)
    └── prandom_init_once(&slab_rnd_state)
```

---

## 15. SLUB Tuning Parameters

| Parameter | Default | Cmdline | Effect |
|---|---|---|---|
| `slub_min_order` | `0` | `slub_min_order=N` | Minimum page order for all slabs; increases slab size floor |
| `slub_max_order` | `3` (normal) or `1` (SLUB_TINY) | `slub_max_order=N` | Maximum page order; limits memory wasted per slab |
| `slub_min_objects` | `0` (auto) | `slub_min_objects=N` | Override minimum objects; `0` = `4*(fls(nr_cpus)+1)` |

**Auto-tuning example** (8-CPU system):
```
nr_cpus = 8
fls(8) = 4 (position of highest set bit)
min_objects = 4 * (4 + 1) = 20
```
For `kmalloc-64`: need at least `20 × 64 = 1280` bytes → order-1 (8KB) slab, holds 128 objects. Waste = 0 bytes (8192 / 64 = 128 exact). `oo = oo_make(1, 64) = (1<<16)|128 = 0x00010080`.

**Debug path**: `if (debug_guardpage_minorder()) slub_max_order = 0;` — when guard pages are enabled for debugging, every slab is exactly one page to ensure guard pages can surround it.

---

## 16. Key Terminology Glossary

| Term | Definition |
|---|---|
| **slab** | A contiguous group of pages carved into fixed-size objects. Also: the name of the overall subsystem (SLAB, SLUB, SLOB). |
| **SLUB** | The default kernel slab allocator since Linux 2.6.23. "SL" + "UB" = Simplified sLab with Unqueued Buffers. Avoids the complex per-slab queuing of SLAB. |
| **kmem_cache** | A slab cache descriptor: knows object size, alignment, per-node state, and per-cpu fast path. Created by `kmem_cache_create()`. |
| **kmem_cache_node** | Per-NUMA-node state for a cache: partial slab list, full slab list (debug), barn. |
| **freelist** | A linked list of free objects within a slab, threaded through the objects themselves using an embedded pointer at `s->offset`. |
| **sheaf** | A small array of object pointers, one per CPU. Provides a lock-free fast path for alloc/free. Replaces the older per-cpu page concept. |
| **barn** | Per-NUMA-node pool of full and empty sheaves. CPUs exchange sheaves with the barn under a spinlock. |
| **bootstrapping** | The process of initializing a system that depends on itself. Here: creating slab caches using statically allocated descriptors. |
| **slab_state** | Global enum (DOWN/PARTIAL/UP/FULL) tracking how functional the allocator is. Guards against using not-yet-initialized features. |
| **oo encoding** | Packed `u32`: `order << 16 \| objects`. Stores both page order and objects-per-slab in one word. |
| **reciprocal_size** | Precomputed multiplier for division-free object index calculation. `index = ptr_offset * m >> sh`. |
| **inuse** | Bytes of object actually usable by the caller (before debug redzones/poison padding). |
| **offset** | Byte offset within an object where the freepointer lives (when object is on freelist). |
| **GFP flags** | "Get Free Page" flags: `GFP_KERNEL`, `GFP_ATOMIC`, `GFP_NOWAIT`, `__GFP_COMP`, `GFP_DMA`, etc. Control how and from where pages are allocated. |
| **compound page** | Multiple physically contiguous pages treated as a single unit. Slab pages are always compound (`__GFP_COMP`). |
| **folio** | New kernel type: large page abstraction. `struct slab` cast to `struct folio *` for interaction with the page allocator. |
| **NUMA** | Non-Uniform Memory Access. Multi-socket systems where memory latency depends on which node holds the memory. |
| **KASAN** | Kernel Address SANitizer. Detects out-of-bounds and use-after-free by poisoning shadow memory around allocations. |
| **RCU** | Read-Copy-Update. A synchronization primitive. `SLAB_TYPESAFE_BY_RCU` defers slab page reuse until all RCU readers exit. |
| **HWCACHE_ALIGN** | `SLAB_HWCACHE_ALIGN`: align object start to CPU cache line (64B on ARM64/x86). Prevents two objects sharing a cache line (false sharing). |
| **redzone** | Debug: guard bytes added before and after each object. If corrupted → overflow detected on free. |
| **poison** | Debug: fill free objects with a known pattern (0x6b). If pattern changed when object is next allocated → use-after-free detected. |
| **frozen** | `slab->frozen=1`: the slab is "owned" by a CPU's per-cpu sheaf. While frozen, the slab cannot be moved to/from the partial list. |
| **partial slab** | A slab with at least one free and at least one used object. Lives in `kmem_cache_node->partial`. |
| **OOM** | Out Of Memory. `__alloc_pages_node()` can fail; slab handles this by returning NULL (or panicking if `SLAB_PANIC` set). |

---

## 17. Nvidia Interview Talking Points

### 17.1 Five-Minute Verbal Answer Structure

When asked "Walk me through `kmem_cache_init`":

1. **(30s) Context**: "It's called from `mm_core_init()` during early boot, after the buddy allocator is live but before any dynamic memory exists. Its job is to bootstrap the SLUB allocator."

2. **(45s) The Bootstrap Problem**: "The chicken-and-egg problem: SLUB needs a `kmem_cache` descriptor to create a slab cache, but that descriptor must come from a slab cache. The solution is two static `__initdata` objects in BSS that act as placeholders."

3. **(60s) Four Phases**: "Phase 1: point globals at static objects. Phase 2: create the `kmem_cache_node` cache using `early_kmem_cache_node_alloc` which directly manipulates the first slab page from the buddy allocator. Phase 3: create `kmem_cache` and then `bootstrap()` both — migrating the static objects to heap-allocated ones, patching all `slab->slab_cache` back-pointers. Phase 4: build the kmalloc size table — 22 sizes from 8B to 2MB, four types."

4. **(45s) The Infrastructure**: "Three things make the fast path fast: per-cpu sheaves (lock-free array of pointers), per-node barns (spinlock-protected exchange pool), and reciprocal_size (division-free object indexing). The lock hierarchy is 5 levels deep, and the hot path touches only the local trylock."

5. **(30s) For GPU/driver work**: "In NVIDIA driver context, this matters because every `kmem_cache_create('my_driver_obj', sizeof(struct gpu_obj), ...)` call creates a cache built on top of what `kmem_cache_init` established. DMA pools, per-device object caches, and per-channel descriptor allocators all ultimately rely on SLUB."

### 17.2 GPU/Driver Relevance

```c
// Example: NVIDIA driver-style usage of what kmem_cache_init enables
static struct kmem_cache *gpu_channel_cache;
static struct kmem_cache *pushbuf_desc_cache;

void nvidia_init_caches(void)
{
    // kmem_cache_init made kmem_cache_create() possible
    gpu_channel_cache = kmem_cache_create(
        "nvidia_gpu_channel",
        sizeof(struct gpu_channel),
        __alignof__(struct gpu_channel),
        SLAB_HWCACHE_ALIGN | SLAB_PANIC,
        NULL);

    // DMA-accessible descriptor pool (uses SLAB_CACHE_DMA internally)
    pushbuf_desc_cache = kmem_cache_create_usercopy(
        "nvidia_pushbuf",
        sizeof(struct pushbuf_descriptor),
        64,   // align to cache line
        SLAB_HWCACHE_ALIGN,
        0, sizeof(struct pushbuf_descriptor),
        NULL);
}
```

Key points:
- Per-object constructors (`ctor`) are called on fresh allocation — useful for pre-initializing GPU object state
- `SLAB_TYPESAFE_BY_RCU` useful for GPU channel objects that RCU-readers may hold references to
- NUMA awareness: `kmem_cache_alloc_node(cache, GFP_KERNEL, gpu_numa_node)` keeps GPU DMA buffers on the right NUMA node

### 17.3 Common Follow-Up Questions

**Q: Why are there two separate boot phases (`kmem_cache_init` and `kmem_cache_init_late`)?**  
A: `alloc_workqueue()` needs `kmalloc()` and also needs interrupt handlers ready (for per-cpu work). These conditions are only met after `local_irq_enable()`. The split allows the allocator to become functional for all non-workqueue callers as early as possible.

**Q: What happens if `create_boot_cache` fails during boot?**  
A: It calls `panic()` directly. There is no recovery path — if the slab allocator can't initialize, the kernel cannot function.

**Q: How does `kfree()` know which cache an object belongs to?**  
A: `kfree(ptr)` → `virt_to_folio(ptr)` → `folio_slab(folio)` → `slab->slab_cache`. The `struct slab` overlay on the page's metadata gives O(1) lookup of the owning `kmem_cache`.

**Q: Can SLUB merge caches with identical parameters?**  
A: Yes, unless `SLAB_NO_MERGE` or `SLAB_NO_OBJ_EXT` is set, or the cache has a destructor. Merging saves memory (fewer slab pages) but makes debugging harder. The `boot_kmem_cache*` uses `SLAB_NO_OBJ_EXT` to prevent early merging.

**Q: What is `SLAB_TYPESAFE_BY_RCU` and when would an Nvidia driver use it?**  
A: Objects in an RCU-safe slab can be freed, but their backing page cannot be returned to the buddy allocator until after an RCU grace period. This allows RCU readers to safely dereference the pointer even after the object is "freed" (the memory is still valid, just may have been reallocated). Useful for per-channel state that RCU-protected lookup tables point to.

**Q: How does SLUB handle memory pressure?**  
A: Through the shrinker interface: `register_shrinker()` → `kmem_cache_shrink()` → drains each node's partial list by freeing fully-empty slabs back to buddy. The `min_partial` watermark prevents draining below a threshold, avoiding thrashing.

**Q: Explain `SLAB_FREELIST_HARDENED` in the context of exploit mitigation.**  
A: The freepointer stored in each free object is XOR'd with `s->random` (a per-cache secret) and the pointer's own address. This means an attacker who can write to a free object cannot simply forge a freepointer to redirect the next allocation to an arbitrary address — they need to know both the secret and the current pointer value.

**Q: On an 8-CPU ARM64 system, what page order does `kmem_cache_init` choose for `kmem_cache_node` (32 bytes)?**  
A: `min_objects = 4*(fls(8)+1) = 20`. Need `20 × 32 = 640` bytes → `get_order(640)` = 0 (4KB fits). `calc_slab_order(32, 0, 3, 16)`: at order=0 (4096B), objects=128, waste=0 bytes. 0/4096 < 1/16. → order=0. So `oo = oo_make(0, 32)` = 128 objects on a single 4KB page.

**Q: What does `bootstrap()` do if there are no partial slabs yet?**  
A: `for_each_kmem_cache_node` iterates over `s->node[]`. After `early_kmem_cache_node_alloc`, there is exactly one partial slab per node. `list_for_each_entry` on that partial list updates `slab->slab_cache` on each slab. Since only one slab exists at this point, the loop runs exactly once per node.

**Q: Why does `struct kmem_cache` use `MAX_NUMNODES` for the `node[]` array but the actual allocation uses `nr_node_ids`?**  
A: `MAX_NUMNODES` is a compile-time constant (e.g., 1 for UMA, 64 for NUMA builds). The struct definition uses it to ensure the array is large enough. But the actual memory allocated for the struct uses `nr_node_ids` (the runtime count of real nodes) to avoid wasting BSS space on unused entries. This is why `create_boot_cache` uses `offsetof(struct kmem_cache, node) + nr_node_ids * sizeof(ptr)`.

**Q: How does the sheaves design compare to the old per-cpu slab design?**  
A: The old design had a per-cpu pointer to a single slab page (`c->page`). Alloc/free went directly to/from the slab page's freelist. Problem: freeing an object from CPU-B that was allocated on CPU-A required acquiring the slab's lock, causing cross-CPU cache line bouncing. Sheaves batch objects: a CPU holds N pointers (the sheaf), so N consecutive allocs/frees are completely local. Only when the sheaf is exhausted/full does any cross-CPU or spinlock activity occur.

**Q: What is `GFP_NOWAIT` and why is it used in `early_kmem_cache_node_alloc`?**  
A: `GFP_NOWAIT` means "allocate without waiting — return NULL immediately if no free pages." During early boot: (1) there is no scheduler running to sleep, (2) no memory reclaim is possible (no kswapd, no shrinkers), and (3) there should be plenty of free pages (memblock just handed everything to buddy). Using `GFP_KERNEL` here would be wrong because it might try to sleep, which is not possible in `__init` context before the scheduler is up.
