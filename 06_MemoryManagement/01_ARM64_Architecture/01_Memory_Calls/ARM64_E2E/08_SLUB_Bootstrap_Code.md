# 08 — SLUB Bootstrap: `kmem_cache_init()` Code Walkthrough

[← Doc 07: Buddy Handoff](07_Buddy_Handoff_Code.md) | **Doc 08** | [Doc 09: vmalloc & Completion →](09_vmalloc_and_Completion.md)

---

## Overview

SLUB is the default slab allocator. It faces a chicken-and-egg problem: `kmem_cache`
structures are themselves allocated from a slab cache. The bootstrap uses static
variables, then replaces them with dynamically allocated ones.

---

## 1. Pre-Bootstrap State

Before `kmem_cache_init()`:
- Buddy allocator is active (`alloc_pages()` works)
- `kmalloc()` does NOT work
- `kmem_cache_create()` does NOT work

---

## 2. `kmem_cache_init()` — The Complete Bootstrap

**Source**: `mm/slub.c`  
**Called by**: `mm_core_init()` in `mm/mm_init.c`

```c
void __init kmem_cache_init(void)                  // mm/slub.c:8460
{
    /* ─── Step 1: Static initial caches ─── */
    static struct kmem_cache boot_kmem_cache, boot_kmem_cache_node;
    //   These two static variables live in .bss
    //   They bootstrap the first two slab caches WITHOUT kmalloc

    /* ─── Step 2: Initialize CPU slab state ─── */
    cpuhp_setup_state_nocalls(CPUHP_SLUB_DEAD, "slub:dead", NULL, slub_cpu_dead);

    /* ─── Step 3: Create the cache-of-caches ─── */
    kmem_cache = &boot_kmem_cache;
    //   Global kmem_cache pointer → static variable

    create_boot_cache(kmem_cache, "kmem_cache",
                      offsetof(struct kmem_cache, node) +
                          nr_node_ids * sizeof(struct kmem_cache_node *),
                      SLAB_HWCACHE_ALIGN, 0, 0);
    //   Initialize the static kmem_cache to allocate kmem_cache objects
    //   size = sizeof(kmem_cache) up to the node[] array
    //   ★ This cache can allocate future kmem_cache structs

    /* ─── Step 4: Create the kmem_cache_node cache ─── */
    kmem_cache_node = &boot_kmem_cache_node;
    //   Global kmem_cache_node pointer → static variable

    create_boot_cache(kmem_cache_node, "kmem_cache_node",
                      sizeof(struct kmem_cache_node),
                      SLAB_HWCACHE_ALIGN, 0, 0);
    //   size = sizeof(struct kmem_cache_node) ≈ 64-128 bytes
    //   ★ This cache can allocate kmem_cache_node structs

    /* ─── Step 5: Mark slab system as partially up ─── */
    slab_state = PARTIAL;
    //   States: DOWN → PARTIAL → PARTIAL_NODE → UP → FULL
    //   PARTIAL: kmem_cache_alloc works for the two boot caches

    /* ─── Step 6: Bootstrap — replace static with dynamic ─── */
    bootstrap(&boot_kmem_cache);
    //   Allocate a REAL kmem_cache from the boot cache
    //   Copy boot_kmem_cache → dynamic one
    //   Update global pointer + all slab references

    bootstrap(&boot_kmem_cache_node);
    //   Same for kmem_cache_node

    /* ─── Step 7: Create kmalloc caches ─── */
    init_freelist_randomization();
    //   Random freelist order for SLAB_FREELIST_RANDOM

    create_kmalloc_caches();
    //   Create caches for sizes: 8, 16, 32, 64, 96, 128, 192, 256,
    //   512, 1024, 2048, 4096, 8192 bytes
    //   ★ After this, kmalloc() works!

    /* ─── Step 8: Final state ─── */
    // slab_state is set to UP inside create_kmalloc_caches()
    // After: slab_state = UP → full kmalloc available
}
```

---

## 3. `create_boot_cache()` — Initialize a Static kmem_cache

**Source**: `mm/slab_common.c`

```c
void __init create_boot_cache(struct kmem_cache *s, const char *name,
                              unsigned int size, slab_flags_t flags,
                              unsigned int useroffset, unsigned int usersize)
{
    s->name = name;
    s->size = s->object_size = size;
    s->align = calculate_alignment(flags, ARCH_KMALLOC_MINALIGN, size);
    //   Alignment: max(ARCH_KMALLOC_MINALIGN, natural alignment, HWCACHE_ALIGN)
    //   ARM64: ARCH_KMALLOC_MINALIGN = 8

    s->useroffset = useroffset;
    s->usersize = usersize;

    __kmem_cache_create(s, flags);
    //   ★ SLUB-specific initialization (see below)

    s->refcount = -1;                  // Mark as boot cache (never freed)

    list_add(&s->list, &slab_caches);
    //   Add to global list of all slab caches

    memcg_link_cache(s);
    //   Memory cgroup integration
}
```

### 3.1 `__kmem_cache_create()` — SLUB-Specific Setup

```c
int __kmem_cache_create(struct kmem_cache *s, slab_flags_t flags)
{
    int err = kmem_cache_open(s, flags);
    //   Calculate slab layout:
    //     s->oo = optimal objects-per-slab (order, objects)
    //     s->min = minimum objects-per-slab
    //     s->inuse = actual used bytes per object (with metadata)
    //     s->offset = freelist pointer offset within object
    //     s->red_left_pad = KASAN/debug padding
    //
    //   Set up per-CPU state:
    //     s->cpu_slab → per-CPU slab pointer (initially NULL)
    //
    //   For boot: allocates per-cpu structs from percpu area

    if (!err)
        list_add(&s->list, &slab_caches);
    return err;
}
```

---

## 4. `bootstrap()` — The Self-Referential Trick

**Source**: `mm/slub.c`  
**This is the most clever part of SLUB init.**

```c
static struct kmem_cache * __init bootstrap(struct kmem_cache *static_cache)
{
    struct kmem_cache *s = kmem_cache_zalloc(kmem_cache, GFP_NOWAIT);
    //   Allocate a NEW kmem_cache struct from the boot kmem_cache
    //   This is the first "real" slab allocation!
    //   Works because slab_state == PARTIAL

    memcpy(s, static_cache, kmem_cache->object_size);
    //   Copy all fields from static boot cache → dynamically allocated one

    /* ─── Fix up all slabs that reference the static cache ─── */
    __flush_cpu_slab(s, smp_processor_id());

    list_for_each_entry(slab, &s->full, slab_list)
        slab->slab_cache = s;           // Point to new dynamic cache
    list_for_each_entry(slab, &s->partial, slab_list)
        slab->slab_cache = s;           // Point to new dynamic cache

    list_add(&s->list, &slab_caches);   // Add to global cache list

    /* ─── Update global pointer ─── */
    if (static_cache == kmem_cache)
        kmem_cache = s;                 // Global → dynamic
    else
        kmem_cache_node = s;            // Global → dynamic

    return s;
    //   The static_cache in .bss is now abandoned
    //   All slab pages point to the heap-allocated kmem_cache
}
```

**The trick**:
1. `kmem_cache` (static) can allocate `kmem_cache` objects
2. Allocate a new `kmem_cache` from itself
3. Copy static → dynamic
4. Fix up all slab page pointers
5. Replace global pointer

```
Before bootstrap():
  kmem_cache → &boot_kmem_cache (static, in BSS)
  All slabs → slab_cache = &boot_kmem_cache

After bootstrap():
  kmem_cache → heap-allocated kmem_cache (from slab)
  All slabs → slab_cache = heap-allocated
  &boot_kmem_cache → abandoned (BSS reclaimed later)
```

---

## 5. `create_kmalloc_caches()` — kmalloc Size Classes

**Source**: `mm/slab_common.c`

```c
void __init create_kmalloc_caches(void)             // mm/slab_common.c:994
{
    int i;
    enum kmalloc_cache_type type;

    /* ─── Create caches for each kmalloc size class ─── */
    for (type = KMALLOC_NORMAL; type < NR_KMALLOC_TYPES; type++) {
        for (i = KMALLOC_SHIFT_LOW; i <= KMALLOC_SHIFT_HIGH; i++) {
            //   KMALLOC_SHIFT_LOW = 3 (8 bytes)
            //   KMALLOC_SHIFT_HIGH = 13 (8192 bytes)

            if (!kmalloc_caches[type][i])
                new_kmalloc_cache(i, type);
            //   Creates cache: "kmalloc-8", "kmalloc-16", ..., "kmalloc-8k"
        }
    }

    /* Special sizes: 96, 192 */
    if (!kmalloc_caches[KMALLOC_NORMAL][1]) {
        new_kmalloc_cache(1, KMALLOC_NORMAL);   // kmalloc-96
    }
    if (!kmalloc_caches[KMALLOC_NORMAL][2]) {
        new_kmalloc_cache(2, KMALLOC_NORMAL);   // kmalloc-192
    }

    slab_state = UP;
    //   ★ FULL KMALLOC NOW AVAILABLE
}
```

**kmalloc size classes** (index → size):

| Index | Size | Cache Name |
|-------|------|-----------|
| 1 | 96 | kmalloc-96 |
| 2 | 192 | kmalloc-192 |
| 3 | 8 | kmalloc-8 |
| 4 | 16 | kmalloc-16 |
| 5 | 32 | kmalloc-32 |
| 6 | 64 | kmalloc-64 |
| 7 | 128 | kmalloc-128 |
| 8 | 256 | kmalloc-256 |
| 9 | 512 | kmalloc-512 |
| 10 | 1024 | kmalloc-1k |
| 11 | 2048 | kmalloc-2k |
| 12 | 4096 | kmalloc-4k |
| 13 | 8192 | kmalloc-8k |

---

## 6. Slab Allocation Path: `kmalloc()` → `slab_alloc_node()`

After `create_kmalloc_caches()`, a `kmalloc(128, GFP_KERNEL)` call follows:

```c
void *kmalloc(size_t size, gfp_t flags)
{
    struct kmem_cache *s = kmalloc_slab(size, flags);
    //   Look up cache for this size:
    //     128 bytes → index 7 → kmalloc_caches[NORMAL][7]
    //     = "kmalloc-128" cache

    return kmem_cache_alloc(s, flags);
    //   → slab_alloc_node()
}
```

### 6.1 `slab_alloc_node()` — Fast Path (Per-CPU)

```c
static __always_inline void *slab_alloc_node(struct kmem_cache *s,
                                             struct list_lru *lru,
                                             gfp_t gfpflags, int node,
                                             unsigned long addr,
                                             size_t orig_size)  // mm/slub.c:4869
{
    void *object;
    bool init = false;
    struct slub_percpu_cache *pcs;
    unsigned long irqflags;

    /* ─── Fast path: per-CPU sheaf ─── */
    irqflags = local_save_flags();
    local_irq_disable();

    pcs = slub_get_percpu_cache(s);
    //   s->cpu_slab → per-CPU slab cache pointer

    object = alloc_from_pcs(s, pcs);
    //   Try to get object from per-CPU freelist (no locking!)
    //   pcs->freelist is a LIFO stack of free objects

    if (likely(object)) {
        local_irq_restore(irqflags);
        goto out;
        //   ★ FAST PATH: got object without any locking
    }

    /* ─── Slow path: refill per-CPU cache ─── */
    local_irq_restore(irqflags);
    object = ___slab_alloc(s, gfpflags, node, addr, NULL);
out:
    return object;
}
```

### 6.2 `___slab_alloc()` — Slow Path

```c
static void *___slab_alloc(struct kmem_cache *s, gfp_t gfpflags,
                           int node, unsigned long addr,
                           struct slab *slab)            // mm/slub.c:4405
{
    struct slab *newslab;
    void *freelist;

    /* ─── Try 1: Get from partial slab list for this node ─── */
    newslab = get_partial(s, node, &freelist);
    if (newslab) {
        /* Found a partially-used slab → use its freelist */
        goto populate_pcs;
    }

    /* ─── Try 2: Allocate a brand new slab ─── */
    newslab = new_slab(s, gfpflags, node);
    //   → allocate_slab()
    if (unlikely(!newslab))
        return NULL;    // Out of memory

    freelist = newslab->freelist;
    newslab->freelist = NULL;

populate_pcs:
    /* Populate per-CPU sheaf from the slab's freelist */
    // Move objects from slab freelist → per-CPU cache
    return object;   // Return one object to caller
}
```

### 6.3 `allocate_slab()` — New Slab from Buddy

```c
static struct slab *allocate_slab(struct kmem_cache *s, gfp_t flags, int node)
{
    struct slab *slab;
    struct page *page;
    unsigned int order = oo_order(s->oo);
    //   s->oo encodes optimal order and object count
    //   Example: "kmalloc-128" → order 0 (1 page = 4096 / 128 = 32 objects)

    /* Allocate compound page(s) from buddy */
    page = alloc_slab_page(s, flags, node, order);
    //   → alloc_pages_node(node, flags, order)
    //   → __alloc_pages(gfp, order, ...)
    //   Returns a (possibly compound) page from buddy allocator

    if (!page)
        return NULL;

    slab = page_slab(page);

    /* ─── Build freelist chain within the slab ─── */
    slab->objects = oo_objects(s->oo);    // e.g., 32
    slab->inuse = 0;
    slab->freelist = fixup_red_left(s, start);

    /* Chain all objects via embedded freelist pointers */
    for (idx = 0; idx < slab->objects; idx++) {
        void *object = start + idx * s->size;
        void *next = (idx + 1 < slab->objects) ?
                     start + (idx + 1) * s->size : NULL;

        set_freepointer(s, object, next);
        //   *(void **)(object + s->offset) = next
        //   Each object points to the next free one
    }

    slab->slab_cache = s;
    return slab;
}
```

**Slab layout** (example: kmalloc-128, order 0):
```
Page (4096 bytes):
  [obj0|fp→obj1] [obj1|fp→obj2] [obj2|fp→obj3] ... [obj31|fp→NULL]
   128 bytes       128 bytes      128 bytes          128 bytes

  slab->freelist → obj0
  slab->objects = 32
  slab->inuse = 0
```

---

## 7. Bootstrap Timeline Summary

```
slab_state:  DOWN ──────────────────────────────────────────→ FULL
              │                                                │
              │  create_boot_cache(kmem_cache, ...)            │
              │  create_boot_cache(kmem_cache_node, ...)       │
              │           │                                    │
              │     slab_state = PARTIAL                       │
              │           │                                    │
              │  bootstrap(&boot_kmem_cache)                   │
              │  bootstrap(&boot_kmem_cache_node)              │
              │           │                                    │
              │  create_kmalloc_caches()                       │
              │           │                                    │
              │     slab_state = UP                            │
              │           │                                    │
              │     kmalloc() now works!                       │
              ▼                                                ▼
```

---

## State After Document 08

```
┌──────────────────────────────────────────────────────────┐
│  SLUB Slab Allocator Operational:                        │
│                                                          │
│  kmem_cache_init() complete:                             │
│    ✓ kmem_cache (cache-of-caches) bootstrapped           │
│    ✓ kmem_cache_node bootstrapped                        │
│    ✓ Static boot caches replaced by dynamic ones         │
│    ✓ kmalloc size classes created (8B to 8KB)            │
│    ✓ slab_state = UP                                     │
│                                                          │
│  Available APIs:                                         │
│    ✓ kmalloc(size, gfp) / kfree(ptr)                     │
│    ✓ kmem_cache_create(name, size, ...)                   │
│    ✓ kmem_cache_alloc(cache, gfp) / kmem_cache_free()    │
│                                                          │
│  Allocation path:                                        │
│    kmalloc → slab_alloc_node                             │
│      Fast: per-CPU sheaf (alloc_from_pcs)                │
│      Slow: get_partial → get slab from partial list       │
│      Slowest: allocate_slab → alloc_pages (buddy)         │
│                                                          │
│  NOT YET:                                                │
│    ✗ vmalloc (virtual contiguous, physical scattered)     │
│                                                          │
│  Next: vmalloc_init and completion                       │
└──────────────────────────────────────────────────────────┘
```

---

[← Doc 07: Buddy Handoff](07_Buddy_Handoff_Code.md) | **Doc 08** | [Doc 09: vmalloc & Completion →](09_vmalloc_and_Completion.md)
