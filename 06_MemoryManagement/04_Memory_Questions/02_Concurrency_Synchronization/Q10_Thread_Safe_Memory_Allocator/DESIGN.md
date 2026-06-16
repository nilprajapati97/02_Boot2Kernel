# Q10 — Design a Thread-Safe Memory Allocator (SLUB/Slab Style)

---

## 1. Problem Statement

The kernel allocates and frees small objects (task_structs, inodes, skbuffs, dentries) millions of times per second. A general-purpose allocator like `malloc` is unsuitable because:
- `kmalloc` on `buddy allocator` has 4KB granularity — wastes memory for small objects.
- Every alloc/free from a global free list requires a global lock — scales poorly.
- Cache fragmentation: mixing differently-sized objects in the same page reduces spatial locality.

Design a thread-safe kernel object allocator (slab/SLUB style) that:
- Maintains **per-CPU caches** to avoid locking on the fast path.
- Groups **same-size objects** on the same pages (slab) for cache efficiency.
- Uses **partial slab lists** to reclaim memory when slabs are empty.
- Scales to hundreds of CPUs without contention.

---

## 2. Requirements

### 2.1 Functional Requirements
- Allocate/free fixed-size kernel objects with O(1) amortized time.
- Per-object type caches: `struct kmem_cache` for `task_struct`, `struct inode`, etc.
- Thread-safe: no global lock in the allocation fast path.
- Memory pressure: return empty slabs to the buddy allocator.
- Debugging: detect use-after-free, buffer overflow, uninitialized reads (KASAN integration).
- NUMA awareness: allocate from local NUMA node's pages.

### 2.2 Non-Functional Requirements
- Alloc fast path: < 50 ns (per-CPU freelist hit).
- Alloc slow path: < 500 ns (per-CPU magazine refill from partial slab).
- Allocator metadata overhead: < 5% of allocated memory.
- No false sharing between CPUs in the allocator's hot path.

---

## 3. Constraints & Assumptions

- Linux SLUB allocator (`CONFIG_SLUB=y`, default since kernel 2.6.28).
- Architecture: x86-64 with per-CPU variables (`this_cpu_ptr()`).
- Page size: 4KB; slab order: 0 (one page per slab) for small objects.
- Object size range: 8 bytes to ~8KB (larger → `kmalloc` calls `alloc_pages`).

---

## 4. Architecture Overview

```
  Allocation Fast Path (per-CPU freelist — no lock)
  ┌────────────────────────────────────────────────────────────────┐
  │  kmem_cache_alloc(cache)                                       │
  │       │                                                        │
  │       ▼                                                        │
  │  c = this_cpu_ptr(cache->cpu_slab)                            │
  │  if (c->freelist != NULL):                                     │
  │      obj = c->freelist                                         │
  │      c->freelist = obj->next     ← pop from per-CPU freelist  │
  │      return obj                  ← FAST PATH (< 50 ns)        │
  │                                                                │
  │  else: ──────────────────────────────────────────────────┐    │
  │                                                          │    │
  └──────────────────────────────────────────────────────────┘    │
                                                             │
  Slow Path (refill per-CPU freelist from partial slab)     │
  ┌──────────────────────────────────────────────────────────▼─── ┐
  │  __slab_alloc(cache, gfpflags, node)                          │
  │       │                                                        │
  │       ├──► get_partial(cache, node)   ← grab partial slab     │
  │       │         → move slab's freelist to c->freelist          │
  │       │                                                        │
  │       └──► if no partial: allocate_slab()                     │
  │                 → alloc_pages(order) from buddy allocator     │
  │                 → initialize all objects in new slab           │
  │                 → add to partial list                         │
  └────────────────────────────────────────────────────────────────┘
```

---

## 5. Core Data Structures

### 5.1 Cache Descriptor

```c
struct kmem_cache {
    /* Hot: accessed on every alloc/free */
    struct kmem_cache_cpu __percpu *cpu_slab;  /* per-CPU data */

    /* Object layout */
    unsigned int    size;           /* object size including metadata */
    unsigned int    object_size;    /* actual requested object size */
    unsigned int    offset;         /* offset of freelist pointer within object */
    slab_flags_t    flags;          /* SLAB_POISON, SLAB_RED_ZONE, etc. */

    /* Slab layout */
    unsigned int    oo;             /* order:objects packed in high/low bits */
    unsigned int    min;            /* min objects per slab (small order) */
    unsigned int    max;            /* max objects per slab */
    int             align;          /* object alignment requirement */

    /* NUMA node lists */
    struct kmem_cache_node *node[MAX_NUMNODES];

    /* Constructor (optional) */
    void (*ctor)(void *);

    /* Name, for /proc/slabinfo */
    const char     *name;
    struct list_head list;   /* in slab_caches list */
};
```

### 5.2 Per-CPU Slab Data

```c
struct kmem_cache_cpu {
    /* Hot path: freelist and current slab */
    void          **freelist;   /* pointer to next free object (NULL if empty) */
    unsigned long   tid;        /* transaction ID (prevents lost-free ABA) */
    struct slab    *slab;       /* current slab page we're allocating from */

#ifdef CONFIG_SLUB_CPU_PARTIAL
    struct slab    *partial;    /* per-CPU partial slab list */
    int             pobjects;   /* partial object count (for refill decision) */
#endif
} ____cacheline_aligned_in_smp;  /* each CPU's data on separate cache line */
```

### 5.3 Per-NUMA-Node Data

```c
struct kmem_cache_node {
    spinlock_t      list_lock;     /* protects partial list */

    unsigned long   nr_partial;   /* count of partial slabs */
    struct list_head partial;     /* partial slab list */

    atomic_long_t   nr_slabs;     /* total slab count */
    atomic_long_t   total_objects;
};
```

### 5.4 Slab (Page)

```c
struct slab {          /* overlaid on struct page via page->slab_cache / page flags */
    unsigned long   __page_flags;     /* PG_slab bit set */
    struct kmem_cache *slab_cache;    /* back-pointer to cache */
    union {
        struct {
            union {
                struct list_head slab_list;  /* node.partial list linkage */
                struct {
                    struct slab *next;       /* CPU partial list */
                    int pobjects;
                };
            };
            void        *freelist;  /* head of free object chain */
            union {
                unsigned long counters;
                struct { unsigned inuse:16; unsigned objects:15; unsigned frozen:1; };
            };
        };
    };
};
```

---

## 6. Key Algorithms & Design Decisions

### 6.1 Fast Path — Per-CPU Freelist (No Lock Required)

```c
static __always_inline void *slab_alloc_node(struct kmem_cache *s,
                                              gfp_t gfpflags, int node,
                                              unsigned long addr)
{
    struct kmem_cache_cpu *c;
    struct slab *slab;
    void *object;

redo:
    /* Disable preemption to pin to a CPU — no lock needed */
    c = raw_cpu_ptr(s->cpu_slab);
    object = c->freelist;

    if (unlikely(!object || !node_match(c, node)))
        goto __slab_alloc;   /* slow path */

    /* Pointer authentication: freelist pointer is encoded */
    next_object = get_freepointer_safe(s, object);

    /* CAS on tid prevents: CPU migration between reading freelist and updating it */
    if (unlikely(!this_cpu_cmpxchg_double(s->cpu_slab->freelist, s->cpu_slab->tid,
                                           object, tid,
                                           next_object, next_tid(tid))))
        goto redo;

    return object;

__slab_alloc:
    return __slab_alloc(s, gfpflags, node, addr, c);
}
```

**Why `cmpxchg_double`:** Between reading `freelist` and updating it, a preemption could move the task to another CPU, or an interrupt could run a free on the same freelist, creating an ABA. The `tid` (transaction ID) detects this: if `tid` changed, the CAS fails and we retry.

### 6.2 Freelist Pointer Encoding (Security)

SLUB encodes the freelist pointer to prevent heap spray attacks:
```c
/* Encode: freepointer = ptr XOR slab_address XOR secret */
static inline void *freelist_ptr_encode(const struct kmem_cache *s,
                                        void *ptr, const void *addr)
{
    return (void *)((unsigned long)ptr ^ s->random ^ swab((unsigned long)(addr)));
}
```
`s->random` is set at `kmem_cache_create()` from `get_random_long()`. An attacker who overwrites the freelist pointer cannot predict the correct encoding.

### 6.3 Slow Path — Per-CPU Partial Slab Refill

```c
__slab_alloc():
    /* Try per-CPU partial list first (no node lock) */
    if (c->partial):
        slab = c->partial
        c->partial = slab->next
        c->freelist = slab->freelist
        c->slab = slab
        return slab_alloc_node() fast path

    /* Try node partial list (requires node->list_lock) */
    slab = get_partial_node(s, node):
        spin_lock(&node->list_lock)
        slab = list_first_entry(&node->partial, ...)
        node->partial = slab->next
        slab->frozen = 1   ← mark as per-CPU owned
        spin_unlock(&node->list_lock)
        return slab

    /* No partial slabs: allocate new slab page */
    slab = allocate_slab(s, flags, node):
        page = alloc_pages_node(node, flags, order)
        slab = page_slab(page)
        setup_slab_object_freelist(s, slab)  ← chain all objects into freelist
        return slab
```

### 6.4 Free Path

```c
void kmem_cache_free(struct kmem_cache *s, void *x)
{
    struct slab *slab = virt_to_slab(x);      /* O(1) via page->slab_cache */
    struct kmem_cache_cpu *c = raw_cpu_ptr(s->cpu_slab);

    if (likely(slab == c->slab)) {
        /* Object belongs to current CPU's active slab: prepend to freelist */
        set_freepointer(s, x, c->freelist);
        c->freelist = x;
        return;   /* FAST PATH: no lock */
    }

    /* Slow path: object belongs to a different slab */
    __slab_free(s, slab, x, ...);
}
```

### 6.5 Slab Lifecycle

```
alloc_pages() → [NEW slab, frozen=0, freelist=all objects]
      ↓
first CPU acquires slab: frozen=1 (per-CPU owned, invisible to node list)
      ↓
objects allocated: slab.inuse increases
      ↓
slab.inuse == slab.objects: slab is FULL, CPU moves to new partial slab
      ↓
objects freed: slab.inuse decreases
      ↓
slab.inuse < threshold: return to node->partial list (frozen=0)
      ↓
slab.inuse == 0: return to buddy allocator via __free_pages()
```

### 6.6 Object Constructors and Destructors

```c
struct kmem_cache *inode_cachep = kmem_cache_create(
    "inode_cache",
    sizeof(struct inode),
    0,                       /* align: use natural alignment */
    SLAB_RECLAIM_ACCOUNT,    /* eligible for memory reclaim */
    inode_init_once           /* constructor: called on new objects */
);
```

Constructors are called **once** at slab allocation time, not on every alloc/free. This avoids re-initializing fields that don't change between uses (e.g., embedded mutexes, list heads).

---

## 7. Trade-off Analysis

| Decision | SLUB | SLAB (old) | SLOB (tiny) |
|---|---|---|---|
| Per-CPU freelist | Yes (single pointer) | Yes (per-CPU array magazine) | No |
| Metadata overhead | Minimal (in struct slab) | Heavy (separate slab header) | Minimal |
| Fragmentation | Low (per-cache pages) | Low | Medium |
| Debug features | KASAN, redzone, poison | Redzone, poison | None |
| Memory compaction | Yes (`compaction_suitable`) | No | N/A |
| Target system | Default (SMP, NUMA) | Legacy | Embedded (<32MB RAM) |

---

## 8. Real Linux Kernel References

| Component | Source | Symbol |
|---|---|---|
| SLUB core | `mm/slub.c` | `kmem_cache_alloc()`, `__slab_alloc()`, `slab_alloc_node()` |
| Cache create | `mm/slub.c` | `kmem_cache_create()`, `kmem_cache_destroy()` |
| Per-CPU struct | `include/linux/slub_def.h` | `struct kmem_cache_cpu` |
| Node struct | `mm/slab.h` | `struct kmem_cache_node` |
| Freelist encode | `mm/slub.c` | `freelist_ptr_encode()`, `get_freepointer_safe()` |
| KASAN integration | `mm/kasan/shadow.c` | `kasan_slab_alloc()`, `kasan_slab_free()` |
| Slab page | `include/linux/slab.h` | `virt_to_slab()`, `slab_to_page()` |
| /proc/slabinfo | `mm/slab_common.c` | `proc_slabinfo_show()` |

---

## 9. Failure Modes & Debug Strategies

### 9.1 Use-After-Free Detection
```bash
# KASAN (Kernel Address Sanitizer) — most effective:
# CONFIG_KASAN=y, CONFIG_KASAN_GENERIC=y
# On use-after-free: immediate BUG with full stack trace
# Or: SLUB_DEBUG=y sets freed objects to 0x6b (POISON_FREE) pattern
grep "slab_err\|Poison overwritten" /proc/kmsg
```

### 9.2 Memory Leak (Slab Cache Not Shrinking)
```bash
cat /proc/slabinfo | sort -k3 -n -r | head -20
# Columns: name active_objs num_objs objsize ...
# High num_objs for a cache = potential leak
# Or: /sys/kernel/slab/<cache>/total_objects
```

### 9.3 KASAN Shadow Corruption
```bash
# dmesg: "BUG: KASAN: slab-out-of-bounds in foo+0x42"
# Stack trace shows allocation site + access site
# Use kasan_report() to get full context
```

### 9.4 Debugging with slabdump
```bash
# CONFIG_SLUB_DEBUG=y
echo 1 > /sys/kernel/slab/<cachename>/validate
# Kernel walks all objects in all slabs, checks poison patterns and red zones
```

---

## 10. Performance Considerations

- **Per-CPU freelist is truly lock-free:** `raw_cpu_ptr()` + `cmpxchg_double` — no spinlock, no preempt_disable needed on x86 (TSO memory model provides sufficient ordering).
- **Cache-line alignment:** `kmem_cache_cpu` is `____cacheline_aligned_in_smp` — each CPU's hot data (freelist, tid) is on its own cache line. No false sharing between CPU 0 and CPU 1 allocating concurrently.
- **Object alignment:** Objects are aligned to their natural size (or a specified alignment) — misaligned objects cause split cache-line accesses.
- **kmalloc size classes:** `kmalloc-8`, `kmalloc-16`, ..., `kmalloc-8192` — 13 pre-created caches. Allocating 17 bytes uses `kmalloc-32` (next power-of-two) — up to 47% internal fragmentation for worst case.
- **NUMA allocation:** `kmem_cache_alloc_node(cache, flags, node)` for NUMA-local allocation — GPU driver DMA buffers should allocate from the GPU's NUMA node.

---

## 11. Interview Answer Strategy (NVIDIA 10-yr Level)

**What they want to hear:**
1. Per-CPU freelist eliminates global locking from the fast path — explain the cmpxchg_double TID trick.
2. Three-tier hierarchy: per-CPU freelist → per-CPU partial → node partial → buddy allocator.
3. `frozen` bit distinguishes per-CPU-owned slabs from node-partial slabs (prevents double-return).
4. Freelist pointer XOR encoding — security-relevant detail.
5. Difference between SLUB (current default), SLAB (legacy), SLOB (embedded).
6. KASAN integration — per-object shadow memory tracks allocation state.
7. NUMA allocation: `kmem_cache_alloc_node()` for GPU driver DMA structures.
8. `/proc/slabinfo` and `slabdump` for production debugging.
