# vmalloc — Internals

> Linux 6.6 · ARM64. The vmalloc subsystem is fundamentally different from
> SLUB: it (a) reserves a virtual range from the vmalloc area, (b)
> allocates physical pages one at a time (or as huge blocks where possible),
> (c) populates kernel page tables to stitch them together.

---

## 1. Subsystem map

```
  caller ─► vmalloc()
              │
              ▼
         __vmalloc_node()
              │
              ▼
         __vmalloc_node_range()                          ┌───────────────┐
              │                                          │ vmalloc area│
   1. ┌──────┤                                          │ freelist /  │
      │  get_vm_area_node()──► alloc_vmap_area()───────►│ red-black   │
      │                                                  │ tree (per-CPU│
      │                                                  │ vmap-cache)  │
      │                                                  └───────────────┘
      │                          (returns one vm_struct with .addr/.size)
      ▼
   2. __vmalloc_area_node()                              ┌───────────┐
              │                                          │  buddy    │
              ├── alloc_pages_bulk_array_node() ─────►│ (one page │
              │                                          │  per slot)│
              │                                          └───────────┘
              ▼
   3. vmap_pages_range()                                 ┌──────────────┐
              │                                          │ kernel pgd  │
              ├── walk PGD→PUD→PMD→PTE, install PTEs ─► │ (init_mm)   │
              │                                          │ init_pg_dir │
              ▼                                          └──────────────┘
   4. arch_sync_kernel_mappings() + (deferred) TLB flush
              │
              ▼
          return area->addr
```

---

## 2. Key data structures

### 2.1 `struct vm_struct` — the high-level descriptor

[`include/linux/vmalloc.h`](https://elixir.bootlin.com/linux/v6.6/source/include/linux/vmalloc.h#L40)

```c
struct vm_struct {
    struct vm_struct *next;
    void             *addr;       /* kernel VA */
    unsigned long     size;       /* incl. guard page */
    unsigned long     flags;      /* VM_ALLOC | VM_MAP | VM_IOREMAP | ... */
    struct page     **pages;      /* array of struct page* for each 4K slot */
    unsigned int      page_order; /* >0 for huge-page mappings */
    unsigned int      nr_pages;
    phys_addr_t       phys_addr;  /* for ioremap */
    const void       *caller;     /* __builtin_return_address(0) */
};
```

### 2.2 `struct vmap_area` — the low-level VA reservation

[`include/linux/vmalloc.h`](https://elixir.bootlin.com/linux/v6.6/source/include/linux/vmalloc.h#L82)

```c
struct vmap_area {
    unsigned long     va_start, va_end;   /* the reserved VA range */
    struct rb_node    rb_node;            /* in free or busy tree */
    struct list_head  list;
    union {
        unsigned long subtree_max_size;   /* free area aug-rbtree */
        struct vm_struct *vm;             /* back-pointer when busy */
    };
    unsigned long flags;
};
```

Two augmented red-black trees per CPU (`vmap_node`):

- **busy tree**: currently allocated areas, indexed by `va_start`.
- **free tree**: free gaps, augmented with `subtree_max_size` for best-fit search.

### 2.3 Per-CPU vmap-cache (`vmap_block`)

For small vmaps (≤ a few pages) there is a per-CPU `vmap_block_queue` that pre-reserves a 2 MB chunk and hands out sub-allocations without taking the global rbtree lock. See [`mm/vmalloc.c:vb_alloc`](https://elixir.bootlin.com/linux/v6.6/source/mm/vmalloc.c#L2110).

---

## 3. Algorithm — `__vmalloc_node_range()` walk

[`mm/vmalloc.c:__vmalloc_node_range`](https://elixir.bootlin.com/linux/v6.6/source/mm/vmalloc.c#L3322)

### Step 1: Reserve VA range

```c
area = __get_vm_area_node(real_size, align, shift, VM_ALLOC | vm_flags,
                          start, end, node, gfp_mask, caller);
```

Under the hood `alloc_vmap_area()` ([`mm/vmalloc.c:1635`](https://elixir.bootlin.com/linux/v6.6/source/mm/vmalloc.c#L1635)) searches the per-CPU free rbtree for a slot ≥ `size + 2 * PAGE_SIZE` (guard pages). If none, falls back to the global tree under `vmap_area_lock`.

### Step 2: Allocate physical pages

```c
area->nr_pages = nr_small_pages;
area->pages    = kvmalloc_node(array_size, page_array_gfp, node);
area->page_order = page_shift - PAGE_SHIFT;

nr_allocated = alloc_pages_bulk_array_node(gfp_mask, node, nr_small_pages,
                                            area->pages);
```

`alloc_pages_bulk_array_node()` is the **bulk** page allocator ([`mm/page_alloc.c`](https://elixir.bootlin.com/linux/v6.6/source/mm/page_alloc.c)) — it grabs the per-CPU pcp list and fills the array in one shot, drastically faster than `nr_pages` individual `alloc_page()` calls.

If `vmalloc_huge()` was called, the loop instead grabs `2^page_order` pages at a time and `vmap_pages_range()` installs them as PMD-block entries.

### Step 3: Map into kernel page tables

```c
ret = vmap_pages_range(addr, addr + size, prot, area->pages, page_shift);
```

`vmap_pages_range()` → `vmap_pages_range_noflush()` ([`mm/vmalloc.c:566`](https://elixir.bootlin.com/linux/v6.6/source/mm/vmalloc.c#L566)) walks the **init_mm** page tables, allocating intermediate PUD/PMD/PTE pages as needed, and installs a PTE per page (or PMD entry per huge slot).

After `vmap_pages_range`, **`arch_sync_kernel_mappings(start, end)`** is called (on ARM64 currently a no-op — the kernel pgd is shared via `init_pg_dir` and TTBR1 already points to it).

### Step 4: TLB management

No TLB invalidation is needed for newly **added** entries on ARM64 — the architecture allows lazy fill (`The TLB is allowed to cache valid translations only`). The vmalloc area was previously **unmapped**, so any speculative walk would have faulted; once the PTE is in place, subsequent accesses populate the TLB on demand.

However a `dsb ishst` is issued at the end of the pgtable update to make the writes globally visible:

```
   stp     xnew, xattr, [pte_ptr]
   dsb     ishst                ; ensure visible to other CPUs
   ; no TLBI — entry transitions invalid → valid
```

That's much cheaper than the `vfree` case which must invalidate.

---

## 4. Huge-page (PMD-block) mappings

When `vmalloc_huge()` is used and `arch_vmap_pmd_supported(prot)` returns true on arm64 (it does for `PAGE_KERNEL`), `vmap_pages_range` installs **2 MB block PMD entries** instead of leaf PTEs. Benefits:

- 1 TLB entry covers 2 MB — huge ITLB/DTLB win for big buffers.
- One PMD write instead of 512 PTE writes — faster allocation.

Requirements: `addr`, `phys`, and `size` aligned to `PMD_SIZE` (2 MB), and contiguous physical pages must be available.

---

## 5. Locking

| Resource                       | Lock                                |
|--------------------------------|-------------------------------------|
| Global free/busy rbtrees       | `vmap_area_lock` (spinlock)         |
| Per-CPU free rbtree            | per-CPU, no lock                    |
| Per-CPU `vmap_block_queue`     | `vbq->lock`                         |
| `init_mm.page_table_lock`      | held during pgtable installation    |
| Lazy purge list                | `purge_vmap_area_lock`              |

---

## 6. Lazy TLB purge

The complement of fast allocation: `vfree` does **not** flush the TLB synchronously. Instead it pushes the freed area to a **lazy purge list** ([`mm/vmalloc.c:__purge_vmap_area_lazy`](https://elixir.bootlin.com/linux/v6.6/source/mm/vmalloc.c#L1814)). When the list exceeds a threshold (`lazy_max_pages`), a single TLB flush covers the union of all pending ranges — amortizing IPI cost across many frees.

See [`../vfree/02_internals.md`](../vfree/02_internals.md) for the free side.

---

## 7. NUMA & node placement

`__vmalloc_node()` accepts an `int node` hint. Pages are allocated via `alloc_pages_bulk_array_node()` which prefers `node`'s zones. The VA reservation itself is node-agnostic (it's all one global vmalloc area), but the *backing physical pages* are NUMA-local where possible — important on large arm64 servers (Ampere Altra, Graviton, etc.).

---

## 8. Interaction with hardening / debug

| Option                       | Effect                                                                 |
|------------------------------|------------------------------------------------------------------------|
| `CONFIG_DEBUG_VM`            | Many sanity checks; will WARN on misuse (e.g., `kfree` on vmalloc ptr). |
| `CONFIG_KASAN_VMALLOC`       | Shadow memory tracks every page in the vmalloc area. Required for KASAN to catch UAF inside vmalloc'd buffers. |
| `CONFIG_DEBUG_PAGEALLOC`     | Each free returns pages to buddy with poisoning + unmap; defeats lazy purge.|
| `CONFIG_HARDENED_USERCOPY`   | `__check_object_size()` validates `copy_*_user` ranges against vmalloc area. |
| `CONFIG_RANDOMIZE_BASE` (KASLR) | `VMALLOC_START` shifted by a random multiple of `MIN_KIMG_ALIGN`.   |

---

## 9. Source-walk cheat sheet

| File                  | Function                                | Role                                  |
|-----------------------|-----------------------------------------|---------------------------------------|
| `mm/vmalloc.c`        | `vmalloc()`                             | thin inline                           |
| `mm/vmalloc.c`        | `__vmalloc_node()` / `__vmalloc_node_range()` | the real entry                  |
| `mm/vmalloc.c`        | `__get_vm_area_node()` / `alloc_vmap_area()` | VA reservation (rbtree)            |
| `mm/vmalloc.c`        | `__vmalloc_area_node()`                 | page allocation + map                 |
| `mm/vmalloc.c`        | `vmap_pages_range_noflush()`            | install PTEs                          |
| `mm/page_alloc.c`     | `alloc_pages_bulk_array_node()`         | bulk page acquisition                 |
| `arch/arm64/mm/mmu.c` | `arch_sync_kernel_mappings()` (no-op)   | arch hook                             |
| `arch/arm64/include/asm/pgtable-prot.h` | `PAGE_KERNEL`         | default vmalloc PTE attrs             |
