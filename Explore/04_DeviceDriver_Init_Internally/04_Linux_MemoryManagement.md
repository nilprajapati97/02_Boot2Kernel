# Linux Kernel Memory Allocation – Complete Deep Dive

## 1. Introduction

This document provides a deep, structured explanation of how memory allocation works inside the Linux kernel. It is designed for advanced learners, kernel developers, and interview preparation at a senior level.

---

## 2. High-Level Architecture

Memory allocation flow:

User / Kernel Request
→ kmalloc / vmalloc / alloc_pages
→ SLUB Allocator
→ Buddy Allocator
→ Physical Memory (RAM)

---

## 3. Memory Fundamentals

### 3.1 Pages

* Basic unit: 4KB (architecture dependent)
* Represented by struct page

### 3.2 Zones

* ZONE_DMA
* ZONE_NORMAL
* ZONE_HIGHMEM

### 3.3 Virtual vs Physical Memory

* Kernel uses virtual addressing
* Page tables map to physical memory

---

## 4. Kernel Allocation APIs

### 4.1 kmalloc()

* Physically contiguous
* Fast
* Uses SLUB

### 4.2 vmalloc()

* Virtually contiguous
* Physically non-contiguous
* Slower

### 4.3 alloc_pages()

* Direct buddy allocator interface

---

## 5. kmalloc Code Walkthrough

### 5.1 kmalloc()

```
static __always_inline void *kmalloc(size_t size, gfp_t flags)
{
    if (__builtin_constant_p(size)) {
        if (size > KMALLOC_MAX_CACHE_SIZE)
            return kmalloc_large(size, flags);

        return kmalloc_index(size);
    }
    return __kmalloc(size, flags);
}
```

Key Points:

* Optimized for compile-time constants
* Routes to slab or page allocator

---

### 5.2 __kmalloc()

```
void *__kmalloc(size_t size, gfp_t flags)
{
    struct kmem_cache *s;
    s = kmalloc_slab(size, flags);

    if (unlikely(!s))
        return ZERO_SIZE_PTR;

    return slab_alloc(s, flags, _RET_IP_);
}
```

---

### 5.3 Cache Selection

```
struct kmem_cache *kmalloc_slab(size_t size, gfp_t flags)
{
    if (size <= KMALLOC_MAX_CACHE_SIZE)
        return kmalloc_caches[kmalloc_index(size)];

    return NULL;
}
```

---

## 6. SLUB Allocator Internals

### 6.1 Architecture

* kmem_cache
* slab (collection of pages)
* objects

### 6.2 Fast Path

```
object = c->freelist;
if (object) {
    c->freelist = get_freepointer(s, object);
    return object;
}
```

* Lockless
* Per-CPU

### 6.3 Slow Path

```
return __slab_alloc(...);
```

---

### 6.4 __slab_alloc()

```
page = get_partial(s, node);
if (!page)
    page = new_slab(s, gfpflags, node);
```

---

## 7. Slab Creation

### 7.1 new_slab()

```
page = alloc_slab_page(s, flags, node);
setup_slab(page, s);
```

---

### 7.2 alloc_slab_page()

```
return alloc_pages_node(node, flags, s->order);
```

---

## 8. Buddy Allocator Internals

### 8.1 Entry

```
alloc_pages_node()
→ __alloc_pages()
```

---

### 8.2 __alloc_pages()

```
page = get_page_from_freelist(...);
if (!page)
    page = __alloc_pages_slowpath(...);
```

---

### 8.3 Free Area Structure

* Orders: 0 → n
* Each order = 2^n pages

---

### 8.4 Allocation Logic

* Find matching block
* Else split higher order

---

### 8.5 Freeing Logic

* Check buddy
* Merge if possible

---

## 9. GFP Flags

* GFP_KERNEL
* GFP_ATOMIC
* GFP_DMA

Control:

* Blocking behavior
* Reclaim permissions

---

## 10. vmalloc Internals

Flow:

* Allocate pages
* Map into virtual space
* Update page tables

---

## 11. Memory Fragmentation

### Internal

* Wasted inside block

### External

* Non-contiguous free memory

---

## 12. Free Path

```
kfree()
→ slab_free()
→ freelist
→ buddy merge
```

---

## 13. Key Data Structures

### struct page

* flags
* refcount
* mapping

### struct kmem_cache

* size
* slab order

### struct kmem_cache_cpu

* freelist
* per-CPU data

---

## 14. Debugging Tools

* /proc/meminfo
* /proc/slabinfo
* slabtop
* kmemleak

---

## 15. Interview Summary

* kmalloc uses SLUB
* SLUB uses per-CPU freelist
* Buddy allocator manages physical pages
* vmalloc maps virtual memory

---

## 16. Full Flow Summary

kmalloc
→ SLUB
→ Buddy
→ Physical Memory

---

## 17. Conclusion

Linux kernel memory allocation is a layered system optimized for performance, scalability, and efficient memory usage. Understanding both SLUB and the buddy allocator is critical for kernel development and debugging.

---

---

# 18. 3-Hour Teaching Script (Storytelling + Diagrams)

This section is designed as a **live teaching / interview explanation flow**. You can use it to explain memory allocation end-to-end on a whiteboard.

---

## 🕒 Hour 1: Foundations + Big Picture

### 🎯 Goal

Build intuition: *What problem are we solving?*

---

### 🔰 Step 1: Start with a Story

"Imagine you are the Linux kernel. Multiple drivers, processes, interrupts are asking you for memory. Some need 64 bytes, some need 1MB, some need it instantly (interrupt), some can wait. How do you manage this efficiently without fragmentation and without slowing down the system?"

---

### 📊 Diagram: Big Picture Flow

```
[Driver / Kernel Code]
        ↓
   kmalloc / vmalloc
        ↓
      SLUB
        ↓
   Buddy Allocator
        ↓
   Physical Memory (RAM)
```

---

### 🔑 Concepts to Explain

#### 1. Pages

* Smallest unit = 4KB
* Kernel never allocates bytes directly → always pages underneath

#### 2. Why Not Direct Allocation?

"If kernel gives full 4KB page for 100 bytes → waste"

👉 Leads to need for SLAB/SLUB

---

### 📊 Diagram: Page Waste Problem

```
Request: 100 bytes
Allocated: 4096 bytes
Waste: 3996 bytes ❌
```

---

### 🔑 Zones

Explain with real-world analogy:

* ZONE_DMA → reserved parking
* ZONE_NORMAL → regular parking
* ZONE_HIGHMEM → far parking (extra mapping needed)

---

### 🔑 APIs Overview

| API         | Use Case                |
| ----------- | ----------------------- |
| kmalloc     | small, fast, contiguous |
| vmalloc     | large, flexible         |
| alloc_pages | low-level               |

---

### 🎤 End of Hour 1 Summary Line

"Kernel uses layered allocation: API → SLUB → Buddy → Physical memory"

---

## 🕒 Hour 2: SLUB Allocator Deep Dive

### 🎯 Goal

Explain **how small allocations are fast**

---

### 📊 Diagram: SLUB Architecture

```
kmem_cache (size class)
      ↓
   Slabs (pages)
      ↓
   Objects
```

---

### 🔑 Step 1: Size-Based Caches

```
kmalloc-32
kmalloc-64
kmalloc-128
...
```

Explain:
"Kernel pre-creates buckets to avoid fragmentation"

---

### 🔥 Step 2: FAST PATH (Most Important)

```
CPU freelist → return object
```

Explain like this:

"Each CPU keeps its own pocket of memory objects. No locking. Just pick and go."

---

### 📊 Diagram: Per-CPU Cache

```
CPU0 → freelist → objects
CPU1 → freelist → objects
```

---

### 🔥 Step 3: SLOW PATH

```
No free object → get partial slab → else create new slab
```

---

### 🔧 Step 4: Slab Creation Flow

```
new_slab()
   ↓
alloc_pages()
   ↓
Buddy Allocator
```

---

### 🧠 Key Teaching Insight

"SLUB is not a memory allocator itself. It is a memory *manager* sitting on top of the buddy allocator."

---

### 🔬 Internal Structures to Explain

#### struct kmem_cache

* size
* object layout

#### struct kmem_cache_cpu

* freelist
* current slab

---

### 🎤 End of Hour 2 Summary Line

"SLUB makes allocation fast using per-CPU freelists and size-based caches"

---

## 🕒 Hour 3: Buddy Allocator + Advanced Topics

### 🎯 Goal

Explain **how physical memory is actually managed**

---

### 📊 Diagram: Buddy System

```
Order 3 → 8 pages
Order 2 → 4 pages
Order 1 → 2 pages
Order 0 → 1 page
```

---

### 🔥 Step 1: Allocation Logic

"Request 3 pages → round to 4 pages (order 2)"

---

### 🔪 Step 2: Splitting

```
8 pages
 ↓ split
4 + 4
```

---

### 🔗 Step 3: Merging

```
free block + buddy free → merge
```

---

### 📊 Diagram: Split & Merge

```
[8]
 ↓
[4][4]
 ↓
[2][2][2][2]
```

---

### 🔑 GFP Flags Teaching

Explain with scenario:

* Interrupt → GFP_ATOMIC (no sleep)
* Driver init → GFP_KERNEL (can sleep)

---

### ⚠️ Slow Path (Important for interviews)

```
__alloc_pages_slowpath()
 → reclaim
 → compaction
 → OOM killer
```

---

### 🧠 vmalloc Explanation

```
Non-contiguous physical
Contiguous virtual
```

Use analogy:
"Different houses, but same street address mapping"

---

### 🔍 Debugging Section

Explain real usage:

* slabtop → slab usage
* /proc/slabinfo → cache details
* kmemleak → leak detection

---

### 🎤 Final Summary Statement

"Linux memory allocation is a layered system where SLUB optimizes small allocations and the buddy allocator manages physical memory using power-of-two blocks with splitting and merging."

---

## 🎯 Teaching Tips

* Always draw diagrams
* Emphasize FAST PATH vs SLOW PATH
* Relate to real driver use cases
* Repeat flow multiple times

---

## 🚀 Final Flow to Memorize

```
kmalloc
 ↓
SLUB (fast path → freelist)
 ↓
slow path → new slab
 ↓
alloc_pages
 ↓
buddy allocator
 ↓
RAM
```

---


