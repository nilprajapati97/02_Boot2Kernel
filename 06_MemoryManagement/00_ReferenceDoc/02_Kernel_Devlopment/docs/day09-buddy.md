# Day 09 — Buddy Allocator

> **Goal**: Build an 11-order buddy allocator (orders 0..10 → 4 KiB..4 MiB) on top of the Day-8 bitmap. Expose `alloc_pages(order)` / `free_pages(pa, order)` and a `struct page` array.
>
> **Why today**: Slab (Day 10) needs efficient power-of-2 allocations. Buddy gives O(log N) split/merge with low fragmentation and is what Linux uses for the page allocator.

---

## 1. Background

### 1.1 Buddy basics
- All frames grouped into power-of-2 blocks. Order *k* block = `2^k` frames.
- Each order has its own **free list**.
- **Split**: requesting order *k* when only order *k+1* is free → halve the block, insert one half on order *k* free list.
- **Merge** on free: check if the buddy at the same order is also free → coalesce upward.

### 1.2 Buddy address math
For a block starting at PFN *p* of order *k*:
- Buddy PFN = *p* XOR `(1 << k)`.
- Parent PFN = *p* AND `~(1 << k)`.

### 1.3 `struct page`
Per-frame metadata (modeled after Linux but minimal):
```c
struct page {
    u32 flags;          /* PG_RESERVED, PG_BUDDY, PG_SLAB */
    u8  order;          /* if PG_BUDDY: order of free block */
    u8  ref;            /* simple refcount (Day 24 for COW-like uses) */
    u16 pad;
    struct page *next;  /* free-list linkage */
};
```
Array of `nr_frames` × 16 bytes. For 512 MiB / 4 KiB = 131072 frames → 2 MiB metadata. Allocate from boot reserve.

---

## 2. Design

### 2.1 Files
```
mm/buddy.c
include/kernel/page.h
```

### 2.2 Data structures
```c
#define MAX_ORDER 10                          /* up to 4 MiB block */

static struct page *page_array;
static struct page *free_lists[MAX_ORDER + 1];
static u64 free_pages_by_order[MAX_ORDER + 1];
```

---

## 3. Implementation

### 3.1 `include/kernel/page.h`
```c
#ifndef _KERNEL_PAGE_H
#define _KERNEL_PAGE_H
#include <kernel/types.h>

#define PG_RESERVED  (1u << 0)
#define PG_BUDDY     (1u << 1)
#define PG_SLAB      (1u << 2)
#define PG_VMALLOC   (1u << 3)

struct page {
    u32 flags;
    u8  order;
    u8  ref;
    u16 pad;
    struct page *next;
};

void buddy_init(u64 ram_base, u64 nr_frames);
phys_addr_t alloc_pages(unsigned order);
void        free_pages(phys_addr_t pa, unsigned order);
struct page *pa_to_page(phys_addr_t pa);
phys_addr_t  page_to_pa(struct page *p);

#endif
```

### 3.2 `mm/buddy.c`
```c
#include <kernel/page.h>
#include <kernel/printk.h>
#include <kernel/mm.h>
#include <asm-arm64/pgtable.h>

#define MAX_ORDER 10
static struct page *page_array;
static u64 ram_base_pa, nr_frames;
static struct page *free_lists[MAX_ORDER + 1];
static u64 free_count[MAX_ORDER + 1];

static u64 pfn_of(struct page *p) { return p - page_array; }
phys_addr_t page_to_pa(struct page *p) { return ram_base_pa + (pfn_of(p) << PAGE_SHIFT); }
struct page *pa_to_page(phys_addr_t pa) { return &page_array[(pa - ram_base_pa) >> PAGE_SHIFT]; }

static void push(unsigned o, struct page *p)
{
    p->next = free_lists[o];
    free_lists[o] = p;
    p->flags |= PG_BUDDY;
    p->order = o;
    free_count[o]++;
}

static struct page *pop(unsigned o)
{
    struct page *p = free_lists[o];
    if (p) {
        free_lists[o] = p->next;
        p->flags &= ~PG_BUDDY;
        free_count[o]--;
    }
    return p;
}

static void remove_block(unsigned o, struct page *target)
{
    struct page **pp = &free_lists[o];
    while (*pp && *pp != target) pp = &(*pp)->next;
    if (*pp) { *pp = target->next; target->flags &= ~PG_BUDDY; free_count[o]--; }
}

void buddy_init(u64 base, u64 frames)
{
    ram_base_pa = base; nr_frames = frames;

    /* Carve struct page array from the bitmap allocator */
    u64 meta_bytes = frames * sizeof(struct page);
    u64 meta_pages = (meta_bytes + PAGE_SIZE - 1) >> PAGE_SHIFT;
    phys_addr_t meta_pa = alloc_pages_contig(meta_pages);
    if (!meta_pa) { printk(KERN_ERR "buddy: no meta\n"); return; }
    page_array = (struct page *)(meta_pa + 0xffff000000000000UL);
    for (u64 i = 0; i < frames; i++) page_array[i] = (struct page){0};

    /* Walk the bitmap allocator: for every still-free PFN, free it into buddy */
    for (u64 i = 0; i < frames; i++) {
        phys_addr_t pa = ram_base_pa + (i << PAGE_SHIFT);
        if (bitmap_is_free(i)) {        /* expose helper in page_alloc.c */
            bitmap_mark_used(i);        /* prevent double-tracking */
            free_pages(pa, 0);
        }
    }

    for (unsigned o = 0; o <= MAX_ORDER; o++)
        printk(KERN_INFO "buddy: order %u -> %lu blocks\n", o, free_count[o]);
}

phys_addr_t alloc_pages(unsigned order)
{
    if (order > MAX_ORDER) return 0;
    /* find smallest available */
    unsigned o = order;
    while (o <= MAX_ORDER && !free_lists[o]) o++;
    if (o > MAX_ORDER) return 0;

    struct page *p = pop(o);
    /* split down */
    while (o > order) {
        o--;
        struct page *buddy = p + (1UL << o);
        push(o, buddy);
    }
    p->order = order;
    return page_to_pa(p);
}

void free_pages(phys_addr_t pa, unsigned order)
{
    struct page *p = pa_to_page(pa);
    u64 pfn = pfn_of(p);

    while (order < MAX_ORDER) {
        u64 buddy_pfn = pfn ^ (1UL << order);
        if (buddy_pfn >= nr_frames) break;
        struct page *buddy = &page_array[buddy_pfn];
        if (!(buddy->flags & PG_BUDDY) || buddy->order != order) break;
        remove_block(order, buddy);
        pfn &= ~(1UL << order);
        p = &page_array[pfn];
        order++;
    }
    push(order, p);
}

void buddy_stats(void)
{
    u64 total = 0;
    for (unsigned o = 0; o <= MAX_ORDER; o++) {
        printk(KERN_INFO "  order %2u: %6lu blocks (%lu pages)\n",
               o, free_count[o], free_count[o] << o);
        total += free_count[o] << o;
    }
    printk(KERN_INFO "buddy: %lu free pages\n", total);
}
```

### 3.3 Wiring
After `mm_init` + reservations:
```c
buddy_init(ram_base, nr_frames);
buddy_stats();
```

Replace all later uses of `alloc_page()` with `alloc_pages(0)`. Keep `alloc_page` as `static inline` wrapper for clarity.

---

## 4. Pitfalls

1. **Buddy PFN out of range**: at the upper boundary, `pfn ^ (1<<order)` can exceed `nr_frames`. Always range-check.
2. **Reserved frame in middle**: if reserved area straddles an order-k boundary, you can never coalesce across it — fine, just less efficient.
3. **Double free**: check `PG_BUDDY` flag on entry to `free_pages`; panic if already set.
4. **Concurrent access**: today we're UP. Day 14 will add a global `buddy_lock` (later per-zone).
5. **`struct page` size drift**: keep it ≤ 16 bytes or you double the metadata cost; use bitfields if needed.

---

## 5. Verification

```
[INFO] buddy: order  0 ->  10 blocks
[INFO] buddy: order  1 ->   5 blocks
…
[INFO] buddy: order 10 ->  124 blocks
[INFO] buddy: 130936 free pages
```

Stress test in-kernel: allocate 1000 random-order blocks into an array, free in shuffled order, assert `buddy_stats` total restored.

---

## 6. Stretch

- Per-CPU "magazine" caches of single pages for fast path (Day 14 once locks exist).
- `alloc_pages_node()` API stub for future NUMA (we won't use, but interface-shape matters).
- `dump_buddy()` printing free-list head PAs for debugging.

---

## 7. References

- Knuth TAOCP Vol 1, §2.5 "Dynamic Storage Allocation".
- Linux `mm/page_alloc.c` `__alloc_pages_core`, `__free_one_page`.
