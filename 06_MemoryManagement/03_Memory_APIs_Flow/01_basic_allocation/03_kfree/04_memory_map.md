# kfree — Memory Map

> What `kfree` does to the memory layout — and crucially, what it **doesn't**.
> ARM64 48-bit VA, 4 KB pages. See [`../kmalloc/04_memory_map.md`](../kmalloc/04_memory_map.md)
> for the baseline layout the freed object lived in.

---

## 1. Headline

> `kfree` **never unmaps** the kernel virtual address. The linear map is
> pre-populated for all RAM at boot and stays put forever. `kfree` only
> changes the **bookkeeping**: the object goes back onto a freelist, and
> when an entire slab page becomes empty it may be returned to the buddy
> allocator — but the *page-table entry* for that page in the linear map
> still exists. Re-allocation reuses the same VA range.

---

## 2. Before/after diagram

```
BEFORE kfree(p):

  Linear map page (in PAGE_OFFSET region)
  +---------------------------------------------+
  | obj0 | obj1 | obj2(p)  | obj3 | ... | objN | <-- p points here, in use
  +---------------------------------------------+
     used   used   IN-USE      used        used

  slab->inuse = N
  c->freelist -> obj1 -> obj4 -> ...

AFTER kfree(p) (fast path, slab stays partial):

  Linear map page (UNCHANGED in MMU/TLB)
  +---------------------------------------------+
  | obj0 | obj1 | obj2(p)  | obj3 | ... | objN | <-- p still mapped, contents may be poisoned
  +---------------------------------------------+
     used   free   FREE        used        used

  slab->inuse = N-1
  c->freelist -> obj2 -> obj1 -> obj4 -> ...   (p pushed to head)
```

If this was the last object in the slab AND `n->nr_partial > s->min_partial`:

```
AFTER kfree(p) (slab discarded to buddy):

  Linear map page (STILL MAPPED — linear map is permanent)
  +---------------------------------------------+
  | (free)| (free)| (free) | (free)| ... |(free)| <-- contents stale; page on buddy
  +---------------------------------------------+

  buddy: zone->free_area[order] += 1 page
  struct page: page_count() = 0, no slab_cache
```

The physical page can now be re-allocated by **any** subsystem (slab, anonymous pages, page cache) — but its kernel VA in the linear map remains the same and remains mapped.

---

## 3. Why the linear map isn't torn down

Unmapping a 4 KB page from the linear map would require:

1. Walking the 4-level page table down to the PTE.
2. Possibly splitting a 2 MB or 1 GB block mapping into 4 KB entries.
3. `flush_tlb_kernel_range()` → broadcast TLBI Inner-Shareable.
4. `dsb ishst; isb`.

For every `kfree` that's prohibitively expensive. Trade-off chosen by Linux: **keep all RAM mapped at EL1 forever; bookkeeping in `struct page` tracks ownership**. The only kernel mappings that get torn down on free are vmalloc/ioremap.

---

## 4. Large-kmalloc free path

For objects from `__kmalloc_large_node()` (> 8 KB):

```
Before: 2^order pages contiguous in linear map, folio tagged "large kmalloc"
After:  __free_pages(page, order) — returns to buddy free_area[order]
        Linear-map PTEs untouched.
```

---

## 5. KASAN view

KASAN shadow region: `KASAN_SHADOW_START..KASAN_SHADOW_END` (1/8 of linear map). On `kfree`:

```
shadow byte for object: 0x00 (allocated) ──► 0xFB (KASAN_KMALLOC_FREE)
```

[`mm/kasan/common.c:__kasan_slab_free`](https://elixir.bootlin.com/linux/v6.6/source/mm/kasan/common.c) writes the poison; subsequent dereferences load the shadow byte via `KASAN_SHADOW_OFFSET + (va >> 3)` and trap into `kasan_report()`.

---

## 6. Quarantine (with `CONFIG_KASAN_GENERIC`)

Freed objects are not immediately returned to the slab freelist; they go onto a per-CPU **quarantine** in [`mm/kasan/quarantine.c`](https://elixir.bootlin.com/linux/v6.6/source/mm/kasan/quarantine.c). Only when the quarantine grows beyond its budget (proportional to total slab pages) are old entries actually freed back into SLUB. This extends the UAF detection window at the cost of slightly increased slab footprint.

---

## 7. RCU-deferred free

For `kfree_rcu(p, rcu)` the memory is still mapped, still in the slab page, but the freelist push happens later. Concretely:

```
  T0:  caller calls kfree_rcu(p, rcu)
       p is added to per-CPU krc->bkvhead[]
       p still has its contents; readers in RCU read-side may still deref

  T1:  RCU grace period passes
       kfree_rcu workqueue runs kvfree_call_rcu_arg2() -> kfree(p)
       NOW p is poisoned (KASAN) and pushed to freelist
```

Until T1, `p` is **valid from the MMU's perspective** — it lives in the linear map. Only the *contents* are racy.

---

## 8. Verify on a live system

```text
# Watch a slab cache shrink as kfree's accumulate:
$ watch -n1 'grep -E "^kmalloc-(64|128|256)" /proc/slabinfo'

# See per-CPU partial/active slab counts:
$ ls /sys/kernel/slab/kmalloc-256/
$ cat /sys/kernel/slab/kmalloc-256/cpu_slabs

# Confirm linear map is still mapped for a former slab page:
# (CONFIG_PTDUMP_DEBUGFS=y)
$ cat /sys/kernel/debug/kernel_page_tables | head
```
