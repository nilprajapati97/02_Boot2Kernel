# SLUB Allocator: Slab Cache and kmalloc

**Category**: Linux Kernel Memory Allocators  
**Platform**: ARM64 (AArch64)

---

## 1. Concept Foundation

```
SLUB: the default Linux slab allocator (since kernel 2.6.23)
  Purpose: efficiently allocate small kernel objects (<= PAGE_SIZE)
  Strategy: maintain caches of same-sized, same-type objects
  Avoid: fragmentation from repeated alloc/free of small objects
  
  Why not just use buddy allocator for small objects?
    Buddy minimum: 1 page (4KB)
    A struct task_struct = ~7KB → 2 pages wasted per task (56% waste!)
    SLUB: pack multiple objects per page → ~0 internal waste
  
  Key concept: kmem_cache (slab cache)
    Each distinct object type has its own cache
    Examples:
      mm_struct: mm_cachep = kmem_cache_create("mm_struct", sizeof(mm_struct), ...)
      vm_area_struct: vm_area_cachep
      struct file: filp_cachep
      struct inode: (ext4_inode_cachep, etc.)
      task_struct: task_struct_cachep
      Generic kmalloc caches: kmalloc-8, kmalloc-16, ..., kmalloc-8192
  
  SLUB vs SLAB vs SLOB:
    SLAB:  original, complex, full per-CPU lists (CONFIG_SLAB, legacy)
    SLUB:  simplified, uses buddy pages directly, default since 2.6.23
    SLOB:  tiny, for embedded systems with <10MB RAM (being removed)
    
    SLUB advantages: simpler code, better debugging, NUMA-aware,
                     lower memory overhead than SLAB
```

---

## 2. SLUB Data Structures

```c
/* include/linux/slub_def.h */

struct kmem_cache {
    // Per-CPU slab (hot path — no locking):
    struct kmem_cache_cpu __percpu *cpu_slab;
    
    // Cache properties:
    slab_flags_t     flags;      // SLAB_POISON, SLAB_RED_ZONE, etc.
    unsigned long    min_partial; // min partial slabs to keep
    unsigned int     size;        // object size with metadata
    unsigned int     object_size; // object size without metadata
    unsigned int     offset;      // offset to next pointer within free object
    unsigned int     cpu_partial;  // number of objects on per-CPU partial list
    
    // Alignment:
    unsigned int     align;       // alignment requirement
    unsigned int     red_left_pad; // for red zone debugging
    const char       *name;       // cache name (for /proc/slabinfo)
    
    // Per-NUMA-node partial slabs:
    struct kmem_cache_node *node[MAX_NUMNODES];
    
    // Callbacks:
    void (*ctor)(void *);   // constructor called on new object
};

struct kmem_cache_cpu {
    void        **freelist;   // pointer to first free object (next free object embedded in each free object)
    unsigned long tid;        // transaction ID (for lock-free freelist)
    struct slab  *slab;       // currently active slab page
    struct slab  *partial;    // per-CPU partial slab list head
};

struct kmem_cache_node {
    spinlock_t    list_lock;        // protects partial list
    unsigned long nr_partial;       // count of partial slabs
    struct list_head partial;       // list of partial slabs
    // (full slabs: not tracked — freed directly when all objects returned)
};

struct slab {
    // A slab is backed by a buddy page (order = kmem_cache->order)
    // The struct slab is stored in the struct page's union:
    struct kmem_cache   *slab_cache;  // which cache owns this slab
    void                *freelist;    // first free object in this slab
    union {
        unsigned long   counters;     // packs: inuse + objects + frozen
        struct {
            unsigned inuse:16;        // objects currently allocated
            unsigned objects:15;      // total objects in this slab
            unsigned frozen:1;        // slab is frozen (owned by CPU)
        };
    };
    // The actual object data follows (uses the PAGE itself as storage)
};
```

---

## 3. SLUB Allocation Path

```
kmalloc(size, gfp_flags):
  → __kmalloc(size, gfp_flags)
  → kmalloc_slab(size, gfp_flags): find the right kmem_cache
      // size <= 192: use kmalloc-N cache (N = 8, 16, 32, 64, 96, 128, 192)
      // size <= 8192: use kmalloc-N cache (256, 512, 1024, 2048, 4096, 8192)
      // size > 8192: use kmalloc_large() → alloc_pages() directly
  → kmem_cache_alloc(cache, gfp_flags):
  
  FAST PATH (per-CPU, NO locking):
    1. cpu_slab = this_cpu_ptr(s->cpu_slab)
    2. object = cpu_slab->freelist
    3. if (object && cpu_slab->tid == READ_ONCE(cpu_slab->tid)):
         // Update freelist: set to next free object (embedded pointer at object->offset)
         next_obj = *(void **)(object + s->offset)
         if (cmpxchg_double_relaxed(&cpu_slab->freelist, &cpu_slab->tid,
                                     object, tid, next_obj, next_tid)):
           return object;  // SUCCESS with no lock!
    
    // ARM64 cmpxchg_double_relaxed:
    //   LDXP x0, x1, [x2]   (load-exclusive pair)
    //   CMP  / check match
    //   STXP x3, x4, x5, [x2]  (store-exclusive pair)
    //   if STXP failed: retry
    
  SLOW PATH (when CPU's freelist is empty):
    __slab_alloc(s, gfp_flags, node):
      1. Try cpu_slab->partial (per-CPU partial slab):
           slab = cpu_slab->partial
           if (slab): set as new cpu_slab, set freelist from slab->freelist
           continue with fast path
      
      2. Try node partial list (kmem_cache_node->partial):
           Take spinlock (list_lock)
           Find a partial slab
           Move to cpu_slab, "freeze" it (set slab->frozen=1)
           Release spinlock
      
      3. Allocate new slab from buddy allocator:
           allocate_slab(s, gfp_flags, node):
             alloc_pages(s->allocflags, s->oo.order)
             // Divides page into `objects` slots
             // Sets up freelist: each free slot contains pointer to next
             // First slot: slab->freelist, last slot: NULL
```

---

## 4. SLUB Free Path

```
kfree(object):
  → __kmem_cache_free(cache, object, ip)
  → slab_free(s, slab, object, head, tail, cnt, addr)

  FAST PATH (object to same CPU slab):
    cpu_slab = this_cpu_ptr(s->cpu_slab)
    if (slab == cpu_slab->slab):
      // Return to per-CPU freelist (cmpxchg_double):
      // Set object->next = old_freelist
      // cmpxchg_double: freelist = object, tid = next_tid
      return;  // NO LOCK!
  
  SLOW PATH:
    __slab_free(s, slab, object, head, cnt):
      do_slab_free():
        lock = acquire spinlock (slab->lock or node->list_lock)
        object->next = slab->freelist
        slab->freelist = object
        slab->inuse--
        
        if (slab->inuse == 0 && !on_cpu_partial):
          // Slab is empty: return to buddy allocator
          if (n->nr_partial >= s->min_partial):
            discard_slab(s, slab): __free_pages(slab_page, s->oo.order)
          else:
            add_partial(n, slab, DEACTIVATE_EMPTY)
        
        elif (was_full): // slab was full before this free
          add_partial(n, slab, DEACTIVATE_TO_TAIL)

Free object layout:
  When freed, object is overwritten with:
    object + s->offset: pointer to next free object in freelist chain
  
  Freelist is a SINGLY LINKED LIST embedded in the free objects themselves
  No separate metadata page needed (unlike old SLAB)
  
  DEBUG mode (CONFIG_SLUB_DEBUG):
    object filled with POISON pattern (0x6b when free, 0x5a when allocated)
    Red zones: guard bytes before/after object
    Track: allocation stack trace stored with freed object
```

---

## 5. kmalloc and kfree

```
kmalloc size → cache mapping:
  Size   → Cache           → Object size
  8      → kmalloc-8       → 8 bytes
  16     → kmalloc-16      → 16 bytes
  32     → kmalloc-32      → 32 bytes
  64     → kmalloc-64      → 64 bytes
  96     → kmalloc-96      → 96 bytes
  128    → kmalloc-128     → 128 bytes
  192    → kmalloc-192     → 192 bytes
  256    → kmalloc-256     → 256 bytes
  ...
  8192   → kmalloc-8192    → 8192 bytes (2 pages!)
  > 8192 → kmalloc_large() → alloc_pages() (order ceil(log2(size/PAGE_SIZE)))

ARM64 alignment:
  kmalloc returns ALIGN(size, cache_line_size()) aligned objects by default
  Cache line = 64 bytes on ARM64 (Cortex-A57+, Neoverse)
  KMALLOC_MIN_SIZE: 8 bytes (or arch-specific ARCH_KMALLOC_MINALIGN)
  DMA allocations: may need PAGE_SIZE alignment (use dma_alloc_coherent)

kfree(ptr):
  → virt_to_head_page(ptr): get struct page from virtual address
    = pfn_to_page(__pa(ptr) >> PAGE_SHIFT)
  → if PageCompound: pointer is to a compound page (large kmalloc)
  → page->slab_cache: get kmem_cache*
  → slab_free(cache, slab, ptr, ...)

ksize(ptr):
  Returns actual allocated size (may be larger than requested due to rounding)
  Useful for: krealloc, or knowing actual usable buffer size

krealloc(ptr, new_size, gfp):
  If new_size fits in old cache: just return ptr (shrink in place)
  If new_size needs larger cache: kmalloc(new_size) + memcpy + kfree(ptr)
```

---

## 6. SLUB Debugging and /proc/slabinfo

```
/proc/slabinfo (read slab statistics):
  # name            <active_objs> <num_objs> <objsize> <objperslab> <pagesperslab>
  kmalloc-8            1234          2048      8          512           1
  mm_struct              234          256    1128           16           4
  vm_area_struct        5678         6144      88           46           1
  
  /sys/kernel/slab/<name>/: per-cache controls (SLUB)
    alloc_calls: allocation count
    free_calls: free count
    destroy: destroy this cache
    min_partial: min partial slabs

SLUB debug options (boot kernel with slub_debug=FZP):
  F: sanity checks (SLAB_CONSISTENCY_CHECKS)
  Z: red zones (detect overflows)
  P: poisoning (detect use-after-free)
  U: user tracking (store allocation stack trace)
  T: trace allocations
  
  Kernel parameter: slub_debug=ZP,kmalloc-64
    Enable Z+P only for kmalloc-64 cache

KASAN (Kernel Address Sanitizer):
  ARM64 fully supports KASAN (CONFIG_KASAN)
  Shadow memory: 1 byte per 8 bytes of kernel memory
  ARM64 shadow: at specific VA range, checked on every access
  Detects: heap OOB, use-after-free, stack OOB, global OOB
  Use: CONFIG_KASAN=y for development/debugging builds
```

---

## 7. Interview Questions & Answers

**Q1: Why does SLUB use an embedded freelist instead of a separate free-object bitmap?**

An embedded freelist (each free object contains a pointer to the next free object) has several advantages over a bitmap:

1. **No extra metadata page**: SLAB (old allocator) needed a separate "slab management" page header to track free/used objects. SLUB embeds the freelist in the objects themselves, eliminating this overhead.

2. **Cache-friendly pop**: allocating from the head of the freelist touches only the allocated object itself. No separate bitmap or array to update.

3. **NUMA-friendly**: no metadata page means the slab management data stays with the data, not on a separate page that might be on a different NUMA node.

4. **Simpler reclaim**: when all objects are freed (slab empty), `slab->inuse == 0` is easily checked, and the page can be returned to buddy with just `__free_pages()`.

The trade-off: security — the freelist is embedded in freed objects, which makes use-after-free attacks easier (attacker can corrupt the freelist by writing to a freed object). Linux addresses this with `SLAB_FREELIST_OBFUSCATION` (encodes freelist pointers) and `SLAB_FREELIST_RANDOM` (randomizes freelist order).

---

## 8. Quick Reference

| kmem_cache Layer | Data Structure | Lock Required |
|---|---|---|
| Per-CPU hot path | kmem_cache_cpu.freelist | None (cmpxchg) |
| Per-CPU partial | kmem_cache_cpu.partial | None |
| Per-node partial | kmem_cache_node.partial | spinlock |
| Buddy allocator | zone->free_area | zone->lock |

| SLUB Object State | Location |
|---|---|
| Allocated | In use by kernel code |
| On CPU freelist | cpu_slab->freelist (locked to CPU) |
| On slab's freelist | slab->freelist (partial slab in node list) |
| Page freed to buddy | Order-N page returned to zone |
