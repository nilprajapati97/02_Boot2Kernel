# vfree — Memory Map (ARM64)

> Linux 6.6 · ARM64.
> See [`../vmalloc/04_memory_map.md`](../vmalloc/04_memory_map.md) for the
> baseline VMALLOC area layout. This document focuses on **what changes**
> when `vfree` runs.

---

## 1. Headline

> `vfree` clears the PTEs **immediately** but does **not** invalidate the
> TLB or merge the VA range back into the free pool until a later purge.
> Backing physical pages are returned to buddy immediately. The VA is
> reusable only after purge.

---

## 2. State transitions

```
Step 0: vmalloc'd state

   VMALLOC area
   +----+----+----+----+----+----+
   | G  | P0 | P1 | P2 | P3 | G  |     G = guard, Px = PTE pointing to buddy page x
   +----+----+----+----+----+----+
     ^  ^                        ^
     |  |                        |
   guard  data range          guard

   busy_rbtree: contains vmap_area{va_start, va_end}
   init_mm pgtables: PTEs P0..P3 valid
   TLBs: may contain entries for P0..P3 on any CPU

Step 1: vfree(p)  ->  remove_vm_area  ->  vunmap_range_noflush

   VMALLOC area
   +----+----+----+----+----+----+
   | G  | -- | -- | -- | -- | G  |     PTEs cleared (== 0)
   +----+----+----+----+----+----+

   busy_rbtree: vmap_area removed
   purge_vmap_area_list: vmap_area appended  <-- waiting
   init_mm pgtables: PTEs zero
   TLBs: STALE entries still cached (no TLBI yet)
   pages: returned to buddy via __free_pages

Step 2: threshold hit  ->  __purge_vmap_area_lazy

   VMALLOC area: VA range now free for reuse
   free_rbtree: gap merged with neighbors
   TLBs: invalidated (TLBI VAALE1IS broadcast + DSB ISH)
   purge_vmap_area_list: empty
```

---

## 3. What happens to the backing pages

```
   Before vfree:
        page array area->pages = [pfn_47, pfn_91, pfn_3, pfn_220]
        Each page has page_count = 1, owned by vmap subsystem

   During vfree (after PTE clear):
        for each page: __free_pages(page, area->page_order)
           -> page_count = 0
           -> returned to buddy zone->free_area[page_order]

   After vfree:
        pages are immediately reusable by *any* subsystem (slab, anon, page cache).
        Linear-map mapping of those pages remains valid (linear map never unmaps).
```

Note the **asymmetry**: pages return to buddy *immediately*, but the **VA range** in vmalloc area is reclaimed *later* (at purge). A reuse-after-free of the VA pointer might land on a totally different page now backing some unrelated allocation — explosive bug class. KASAN-VMALLOC catches it.

---

## 4. Guard pages stay reserved (briefly)

The `vmap_area` includes the guard pages in `va_start..va_end`. When merged back into the free tree, the guard slots become available for the next allocator to claim as its own data + guards. No special handling needed.

---

## 5. TLB state across CPUs

```
                                  CPU0 TLB    CPU1 TLB    CPU2 TLB
   Initially (after vmalloc):     [P0]        [P1]        []
   After vfree, before purge:     [P0]        [P1]        []        <- STALE
   During purge (TLBI VAALE1IS):  CPU0,1,2 invalidate matching entries
   After purge:                   []          []          []
```

Until purge, `P0`/`P1` may still be readable from CPU0/1 — but the PTE has been zeroed in memory. On a TLB miss, the walk now hits a zero PTE and faults. So:

- **Cached translation hits** → silent read of orphaned/now-reused data.
- **TLB miss** → page fault.

Both are bugs in the caller; the kernel is consistent.

---

## 6. `DEBUG_PAGEALLOC` mode

`CONFIG_DEBUG_PAGEALLOC=y` forces:

- Lazy purge disabled — every `vfree` flushes the TLB synchronously.
- Freed pages are unmapped from the linear map too (using `__kernel_map_pages`).
- Any read/write to a freed VA faults immediately.

Great for catching UAF in CI, prohibitively expensive in production.

---

## 7. KASAN-VMALLOC shadow state

```
   Before vfree:  shadow bytes for [P0..P3] = 0x00 (valid)
   After vfree:   shadow bytes = 0xFA (KASAN_VMALLOC_INVALID)
                  Later, if entire shadow page is invalid:
                      shadow page itself freed via kasan_release_vmalloc.
```

---

## 8. Verify on a live system

```text
# Before:
$ cat /proc/vmallocinfo | wc -l
12345

# Drop caches, observe shrinkage:
$ echo 3 > /proc/sys/vm/drop_caches
$ cat /proc/vmallocinfo | wc -l
11000   # many freed; purge will follow

# Force purge (debug):
$ echo 1 > /proc/sys/vm/drop_vmap_area_caches   # if exposed

# Watch lazy pages pending purge (debug builds):
$ cat /sys/kernel/debug/vmap_purge_count
```

---

## 9. Cross-references

- vmalloc memory map → [`../vmalloc/04_memory_map.md`](../vmalloc/04_memory_map.md)
- vfree internals → [02_internals.md](02_internals.md)
- vfree call flow → [03_arm64_callflow.md](03_arm64_callflow.md)
- ARM64 TLB ops reference → `arch/arm64/include/asm/tlbflush.h`
