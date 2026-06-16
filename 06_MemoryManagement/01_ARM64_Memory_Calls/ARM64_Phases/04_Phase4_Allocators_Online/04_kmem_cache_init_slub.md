# `kmem_cache_init()` — SLUB Slab Allocator Bootstrap

**Source:** `mm/slub.c`
**Phase:** Allocators Online — Bootstrapped before `memblock_free_all()`
**Memory Allocator:** SLUB (Small/Large Unified allocator, Linux's default)
**Called by:** `mm_core_init()`
**Purpose:** Enable `kmalloc()`, `kmem_cache_create()`, and `kfree()`

---

## What This Function Does

Bootstraps the SLUB slab allocator — which provides efficient allocation of small objects (8 bytes to several KB). SLUB sits **on top of** the buddy allocator, taking whole pages from buddy and subdividing them into fixed-size objects.

```
Application: kmalloc(128, GFP_KERNEL)
    │
    ▼
SLUB: Returns 128-byte object from kmalloc-128 cache
    │ (If no free objects, gets new page from buddy)
    ▼
Buddy: Provides 4KB page (order 0) to SLUB
    │
    ▼
Physical: 4KB page at some PFN
```

---

## The Chicken-and-Egg Problem

SLUB needs to allocate `struct kmem_cache` to track each cache. But `struct kmem_cache` can only be allocated from... a slab cache. How do you create the first slab cache?

**Solution:** Use a **static bootstrap cache** in the `.data` section.

```c
// mm/slub.c
static struct kmem_cache boot_kmem_cache __initdata;
static struct kmem_cache boot_kmem_cache_node __initdata;
```

These are statically allocated in the kernel image — no dynamic allocation needed.

---

## Step-by-Step Bootstrap

### Step 1: Create Boot Caches Using Static Storage

```c
void __init kmem_cache_init(void)
{
    // State: SLUB doesn't exist yet, no slab caches

    // Create the cache that manages kmem_cache objects themselves
    create_boot_cache(&boot_kmem_cache,
                      "kmem_cache",
                      sizeof(struct kmem_cache),
                      SLAB_HWCACHE_ALIGN);

    // Create the cache for kmem_cache_node (per-node slab lists)
    create_boot_cache(&boot_kmem_cache_node,
                      "kmem_cache_node",
                      sizeof(struct kmem_cache_node),
                      SLAB_HWCACHE_ALIGN);
```

**`create_boot_cache()`** initializes the static `kmem_cache` structure:

```c
void __init create_boot_cache(struct kmem_cache *s, const char *name,
                               unsigned int size, slab_flags_t flags)
{
    // Set cache properties
    s->name = name;
    s->object_size = size;
    s->size = ALIGN(size, sizeof(void *));    // Aligned object size
    s->flags = flags;

    // Calculate slab layout
    __kmem_cache_create(s, flags);

    // Add to global slab cache list
    list_add(&s->list, &slab_caches);

    s->refcount = -1;  // Special: never destroy this cache
}
```

---

### Step 2: Register Boot Caches

```c
    // Register these as the active caches for kmem_cache management
    kmem_cache = &boot_kmem_cache;
    kmem_cache_node = &boot_kmem_cache_node;

    // Now slab_state advances
    slab_state = PARTIAL;
```

### Slab State Machine

```c
enum slab_state {
    DOWN,           // SLUB not initialized at all
    PARTIAL,        // boot_kmem_cache available, limited functionality
    PARTIAL_NODE,   // kmem_cache_node available
    UP,             // All boot caches created, kmalloc works
    FULL            // After sysfs registration (post-boot)
};
```

---

### Step 3: Create kmalloc Caches

```c
    // Now that basic infrastructure exists, create kmalloc caches
    create_kmalloc_caches(KMALLOC_MAX_SIZE);
```

**`create_kmalloc_caches()`** creates caches for standard sizes:

```c
void __init create_kmalloc_caches(unsigned int limit)
{
    // For each kmalloc size class:
    // 8, 16, 32, 64, 96, 128, 192, 256, 512, 1024, 2048, 4096, 8192

    for (i = KMALLOC_SHIFT_LOW; i <= KMALLOC_SHIFT_HIGH; i++) {
        if (!kmalloc_caches[KMALLOC_NORMAL][i]) {
            kmalloc_caches[KMALLOC_NORMAL][i] =
                create_kmalloc_cache(kmalloc_info[i].name,
                                     kmalloc_info[i].size, 0);
        }
    }

    // Also create DMA and reclaimable variants
    for (type = KMALLOC_DMA; type <= KMALLOC_RECLAIM; type++) {
        for (i = KMALLOC_SHIFT_LOW; i <= KMALLOC_SHIFT_HIGH; i++) {
            // Create DMA and reclaimable variants
        }
    }

    slab_state = UP;   // kmalloc is fully operational!
}
```

### kmalloc Size Classes

```
kmalloc-8:     8-byte objects      (512 per 4KB slab)
kmalloc-16:    16-byte objects     (256 per slab)
kmalloc-32:    32-byte objects     (128 per slab)
kmalloc-64:    64-byte objects     (64 per slab)
kmalloc-96:    96-byte objects     (42 per slab)
kmalloc-128:   128-byte objects    (32 per slab)
kmalloc-192:   192-byte objects    (21 per slab)
kmalloc-256:   256-byte objects    (16 per slab)
kmalloc-512:   512-byte objects    (8 per slab)
kmalloc-1k:    1024-byte objects   (4 per slab)
kmalloc-2k:    2048-byte objects   (2 per slab)
kmalloc-4k:    4096-byte objects   (1 per slab)
kmalloc-8k:    8192-byte objects   (1 per 2-page slab)
```

---

### Step 4: Bootstrap Self-Replacement

```c
    bootstrap(&boot_kmem_cache);
    bootstrap(&boot_kmem_cache_node);
}
```

**`bootstrap()`** replaces the static `.data` caches with dynamically allocated ones:

```c
static void __init bootstrap(struct kmem_cache *static_cache)
{
    // Allocate a NEW kmem_cache from the now-working SLUB
    struct kmem_cache *s = kmem_cache_zalloc(kmem_cache, GFP_NOWAIT);

    // Copy all data from static boot cache to dynamic cache
    memcpy(s, static_cache, sizeof(*s));

    // Update all slab pages to point to the new cache
    // (each slab page has a backpointer to its owning cache)
    list_for_each_entry(p, &s->full, slab_list)
        p->slab_cache = s;
    list_for_each_entry(p, &s->partial, slab_list)
        p->slab_cache = s;

    // Replace the global pointer
    if (static_cache == &boot_kmem_cache)
        kmem_cache = s;
    else
        kmem_cache_node = s;

    // static_cache (in .data) is now unused
}
```

**After bootstrap:**
- `kmem_cache` points to a dynamically allocated `struct kmem_cache`
- `boot_kmem_cache` (static) is abandoned
- SLUB is fully self-hosting

---

## SLUB Slab Structure

When SLUB needs memory for a cache, it gets whole pages from the buddy allocator and divides them into objects:

```
One Slab Page (4 KB page for kmalloc-64):

┌────────┬────────┬────────┬────────┬────────┐
│ Obj 0  │ Obj 1  │ Obj 2  │ Obj 3  │  ...   │  64 objects total
│ 64 B   │ 64 B   │ 64 B   │ 64 B   │        │
└────────┴────────┴────────┴────────┴────────┘

struct page (for this slab page):
  .slab_cache = &kmalloc_caches[NORMAL][6]  // kmalloc-64
  .freelist = &obj_3                         // First free object
  .inuse = 61                                // 61 objects in use
  .objects = 64                              // 64 objects total
  .frozen = 1                                // CPU-local (fast path)
```

### Free Object Linked List

Free objects are linked through a **freelist** pointer embedded at a configurable offset within each free object:

```
Slab page with some objects allocated:

Object 0: [ALLOCATED DATA............]
Object 1: [ALLOCATED DATA............]
Object 2: [freelist_ptr → Object 5   ]  ← Free
Object 3: [ALLOCATED DATA............]
Object 4: [ALLOCATED DATA............]
Object 5: [freelist_ptr → Object 9   ]  ← Free
...
Object 9: [freelist_ptr → NULL       ]  ← Free (last)

page->freelist = &Object 2  (first free)
Chain: Object 2 → Object 5 → Object 9 → NULL
```

---

## Per-CPU Slab Caching

SLUB uses per-CPU caching to minimize lock contention:

```c
struct kmem_cache_cpu {
    union {
        struct {
            void **freelist;       // Freelist of current slab
            unsigned long tid;     // Transaction ID (for lockless CAS)
        };
    };
    struct slab *slab;             // Current CPU-local slab
    struct slab *partial;          // CPU-local partial slab list
};
```

### Allocation Fast Path (Lockless!)

```c
// kmalloc() → slab_alloc() → slab_alloc_node() → ___slab_alloc()

// FAST PATH (no lock, no atomic):
object = c->freelist;              // Read freelist pointer
if (likely(object)) {
    next = get_freepointer(s, object);
    // Compare-and-swap: update freelist atomically
    if (this_cpu_cmpxchg_double(s->cpu_slab->freelist, s->cpu_slab->tid,
                                 object, tid, next, next_tid(tid))) {
        return object;             // Got object from CPU cache!
    }
}

// SLOW PATH (if CPU cache empty):
// 1. Check CPU partial slabs
// 2. Check node partial slabs (takes node lock)
// 3. Allocate new slab from buddy allocator
```

---

## `kmalloc()` — How It Picks a Cache

```c
static __always_inline void *kmalloc(size_t size, gfp_t flags)
{
    if (__builtin_constant(size)) {
        // Compile-time size: resolved to direct cache access
        unsigned int index = kmalloc_index(size);
        return kmem_cache_alloc(kmalloc_caches[kmalloc_type(flags)][index],
                                flags);
    }

    // Runtime size
    return __kmalloc(size, flags);
}
```

**`kmalloc_index()`** maps sizes to cache indices:

```
Size → Index → Cache
1-8     3      kmalloc-8
9-16    4      kmalloc-16
17-32   5      kmalloc-32
33-64   6      kmalloc-64
65-96   1      kmalloc-96
97-128  7      kmalloc-128
129-192 2      kmalloc-192
193-256 8      kmalloc-256
257-512 9      kmalloc-512
...
```

Note: `kmalloc(33)` allocates from `kmalloc-64`, wasting 31 bytes. This internal fragmentation is the trade-off for O(1) allocation.

---

## SLUB Lifecycle During Boot

```
slab_state = DOWN
│
├── create_boot_cache(boot_kmem_cache)       ← Static cache
├── create_boot_cache(boot_kmem_cache_node)  ← Static cache
│
│ slab_state = PARTIAL
│
├── create_kmalloc_caches()                   ← kmalloc-8 through kmalloc-8k
│
│ slab_state = UP
│
├── bootstrap(boot_kmem_cache)                ← Self-replacement
├── bootstrap(boot_kmem_cache_node)           ← Self-replacement
│
│ (After memblock_free_all and sysfs init)
│
│ slab_state = FULL
│
└── SLUB fully operational
```

---

## Key Takeaways

1. **Chicken-and-egg solved by static caches** — `boot_kmem_cache` in `.data` section
2. **Self-hosting after bootstrap** — SLUB replaces its own static caches with dynamically allocated ones
3. **Per-CPU lockless fast path** — `cmpxchg_double` enables allocation without locks
4. **Size classes waste some memory** — `kmalloc(33)` → 64-byte object (48% waste in worst case)
5. **Buddy provides the pages** — SLUB manages objects within buddy-allocated pages
6. **GFP flags flow through** — `GFP_DMA`, `GFP_ATOMIC` etc. are passed to `alloc_pages()` when SLUB needs new slabs
