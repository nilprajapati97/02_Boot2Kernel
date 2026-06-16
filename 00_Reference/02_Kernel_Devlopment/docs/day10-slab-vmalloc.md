# Day 10 — Slab/Slob + `kmalloc` + `vmalloc`

> **Goal**: Build a small-object allocator (slob-style free-list-of-blocks) over the buddy allocator, expose `kmalloc(size, flags)` / `kfree(ptr)`, and add `vmalloc()` that returns virtually contiguous, physically non-contiguous memory in a dedicated VA region.
>
> **Why today**: `task_struct` (Day 12), `inode`/`dentry` (Day 20) need arbitrary-size allocations smaller than a page. `vmalloc` is needed for kernel module-style large buffers (Day 23 ext2 superblock copy) without needing 16 contiguous frames.

---

## 1. Background

### 1.1 Slab vs Slob vs Slub
- **Slab** (Bonwick'94): per-object caches with constructor/destructor. Complex; needed for performance.
- **Slob**: simple free-list of variable-size blocks within a page. Tiny code, fragmentation acceptable for our scale.
- **Slub**: Linux's current default — per-CPU lockless fast path. Too complex for Day 10.

We pick **slob with named caches** (`kmem_cache`) — best learning trade-off.

### 1.2 vmalloc region
Reserve `0xFFFF_8000_0000_0000..0xFFFF_C000_0000_0000` (256 TiB nominal, in practice we'll cap at 256 MiB). Each `vmalloc(n)` page is:
1. Reserve a VA range from a freelist.
2. For each 4 KiB chunk, `alloc_pages(0)` and install a PTE pointing it at the VA.
3. Return base VA.

`vfree` walks PTEs, frees frames, unmaps.

---

## 2. Design

### 2.1 Files
```
mm/slab.c
mm/vmalloc.c
include/kernel/slab.h
include/kernel/vmalloc.h
```

### 2.2 Slab data structures
```c
struct slab {
    struct slab *next;
    u32 free_bytes;
    u8  data[];           /* PAGE_SIZE - sizeof(header) bytes */
};

struct kmem_cache {
    const char *name;
    u32 obj_size;
    u32 align;
    struct slab *partial;
    u32 nr_allocated;
};
```

A free block within a slab is:
```c
struct slob_block { u32 size; struct slob_block *next; };
```

---

## 3. Implementation

### 3.1 `mm/slab.c` (core)
```c
#include <kernel/slab.h>
#include <kernel/page.h>
#include <kernel/printk.h>
#include <kernel/string.h>

#define PAGE_SIZE 4096
#define ALIGN_UP(x, a) (((x) + (a) - 1) & ~((a) - 1))

struct slob_block { u32 size; struct slob_block *next; };
struct slab_hdr   { struct slab_hdr *next; u32 free; struct slob_block *flist; };

static struct slab_hdr *small_slabs;   /* for kmalloc */

static struct slab_hdr *new_slab(void)
{
    phys_addr_t pa = alloc_pages(0);
    if (!pa) return NULL;
    struct slab_hdr *s = (void *)(pa + 0xffff000000000000UL);
    s->next  = NULL;
    s->free  = PAGE_SIZE - sizeof(*s);
    s->flist = (struct slob_block *)(s + 1);
    s->flist->size = s->free;
    s->flist->next = NULL;
    return s;
}

static void *slab_alloc_in(struct slab_hdr *s, u32 sz)
{
    struct slob_block **pp = &s->flist;
    while (*pp) {
        if ((*pp)->size >= sz) {
            struct slob_block *blk = *pp;
            if (blk->size >= sz + 16) {       /* split */
                struct slob_block *rest = (void *)((u8*)blk + sz);
                rest->size = blk->size - sz;
                rest->next = blk->next;
                *pp = rest;
            } else {
                sz = blk->size;
                *pp = blk->next;
            }
            s->free -= sz;
            *(u32 *)blk = sz;                 /* header for kfree */
            return (u8 *)blk + sizeof(u32);
        }
        pp = &(*pp)->next;
    }
    return NULL;
}

void *kmalloc(u32 size, u32 flags)
{
    (void)flags;
    u32 sz = ALIGN_UP(size + sizeof(u32), 16);

    if (sz >= PAGE_SIZE - sizeof(struct slab_hdr)) {
        /* large alloc: round to page, use buddy directly */
        unsigned order = 0; u32 need = (sz + PAGE_SIZE - 1) >> 12;
        while ((1u << order) < need) order++;
        phys_addr_t pa = alloc_pages(order);
        return pa ? (void *)(pa + 0xffff000000000000UL) : NULL;
    }

    for (struct slab_hdr *s = small_slabs; s; s = s->next) {
        void *p = slab_alloc_in(s, sz);
        if (p) return p;
    }
    struct slab_hdr *ns = new_slab();
    if (!ns) return NULL;
    ns->next = small_slabs; small_slabs = ns;
    return slab_alloc_in(ns, sz);
}

void kfree(void *p)
{
    if (!p) return;
    u32 *hdr = (u32 *)p - 1;
    u32 sz = *hdr;
    /* find slab containing this address */
    u64 addr = (u64)hdr;
    u64 slab_va = addr & ~(PAGE_SIZE - 1UL);
    struct slab_hdr *s = (struct slab_hdr *)slab_va;
    struct slob_block *blk = (void *)hdr;
    blk->size = sz;
    blk->next = s->flist;
    s->flist  = blk;        /* simple free; merging deferred */
    s->free  += sz;
    /* TODO: coalesce; release fully-free slab back to buddy */
}
```

### 3.2 `kmem_cache_create/alloc/free` (thin wrappers)
```c
struct kmem_cache {
    const char *name; u32 size; u32 align; u32 nr_alloc;
};

struct kmem_cache *kmem_cache_create(const char *n, u32 sz, u32 align)
{
    struct kmem_cache *c = kmalloc(sizeof *c, 0);
    *c = (struct kmem_cache){.name=n, .size=sz, .align=align};
    return c;
}
void *kmem_cache_alloc(struct kmem_cache *c){ c->nr_alloc++; return kmalloc(c->size, 0); }
void  kmem_cache_free(struct kmem_cache *c, void *p){ c->nr_alloc--; kfree(p); }
```

### 3.3 `mm/vmalloc.c`
```c
#include <kernel/vmalloc.h>
#include <kernel/page.h>
#include <asm-arm64/pgtable.h>

#define VMALLOC_START 0xFFFF800000000000UL
#define VMALLOC_END   0xFFFF800010000000UL    /* 256 MiB */

static u64 vmalloc_next = VMALLOC_START;       /* simple bump for now */

extern u64 ttbr1_l0[512];                      /* from mmu.c */
extern void map_4k(u64 *root, u64 va, u64 pa, u64 attrs);
extern void unmap_4k(u64 *root, u64 va);

void *vmalloc(u32 size)
{
    u32 npages = (size + PAGE_SIZE - 1) >> PAGE_SHIFT;
    u64 va = vmalloc_next;
    vmalloc_next += npages * PAGE_SIZE;
    if (vmalloc_next > VMALLOC_END) return NULL;

    for (u32 i = 0; i < npages; i++) {
        phys_addr_t pa = alloc_pages(0);
        if (!pa) return NULL;     /* leak partial — TODO unwind */
        map_4k(ttbr1_l0, va + i * PAGE_SIZE, pa, PTE_NORMAL_RWX | PTE_PAGE);
    }
    asm volatile("dsb sy; isb");
    return (void *)va;
}

void vfree(void *p, u32 size)
{
    u32 npages = (size + PAGE_SIZE - 1) >> PAGE_SHIFT;
    u64 va = (u64)p;
    for (u32 i = 0; i < npages; i++) {
        /* walk PTE, recover pa, free */
        extern phys_addr_t walk_pa(u64 *root, u64 va);
        phys_addr_t pa = walk_pa(ttbr1_l0, va + i * PAGE_SIZE);
        unmap_4k(ttbr1_l0, va + i * PAGE_SIZE);
        free_pages(pa, 0);
    }
    asm volatile("dsb sy; tlbi vmalle1is; dsb sy; isb");
}
```

`map_4k` walks/creates L1/L2/L3 tables on demand using `alloc_pages(0)`.

---

## 4. Pitfalls

1. **Header before pointer**: `kfree` finds the slab from `addr & ~(PAGE_SIZE-1)`. Works ONLY if every slab is page-aligned and contains its header at offset 0 — which our `new_slab` guarantees.
2. **Large kmalloc + kfree mismatch**: free path doesn't distinguish; track large allocs in a separate list with size, or store size at `pa - 8`.
3. **vmalloc holes**: bump pointer leaks VA on `vfree`; replace with a free-extent rbtree on Day 24.
4. **Forgetting TLBI**: stale TLB entry after unmap → use-after-free read returns valid data. Always `tlbi vaae1is, x_va; dsb; isb`.
5. **Allocation in interrupt context**: today single-CPU, IRQs disabled briefly during slab ops will be fine; with SMP, slab needs locks.

---

## 5. Verification

```c
char *a = kmalloc(13, 0); memcpy(a, "hello world!", 13); a[12]=0;
printk("%s\n", a);
kfree(a);

void *big = vmalloc(64 * 1024);
memset(big, 0xa5, 64 * 1024);
vfree(big, 64 * 1024);
buddy_stats();   /* should report free_count restored */
```

Stress: 10000 kmalloc/kfree with random sizes 1..2048; assert no leak after `printk(buddy_stats)`.

---

## 6. Stretch

- Implement `kmalloc` size classes (8, 16, 32, 64, … up to 2048) — Slub-lite.
- Add poisoning (write `0x5a` on free, check on alloc) → Day 29 KASAN-lite.
- Coalesce adjacent free blocks within a slab; release fully-free slabs to buddy.

---

## 7. References

- Jeff Bonwick, "The Slab Allocator" (USENIX 1994).
- Linux `mm/slob.c` (~600 LOC, very readable).
- Linux `mm/vmalloc.c` `__vmalloc_node_range`.
