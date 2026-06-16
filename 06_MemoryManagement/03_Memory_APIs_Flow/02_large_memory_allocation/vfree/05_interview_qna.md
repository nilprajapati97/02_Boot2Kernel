# vfree — Interview Q&A (Nvidia / Google / Qualcomm)

---

### Q1. Why does `vfree` defer the TLB flush?  `[Nvidia]` `[Google]`

Every arm64 `TLBI ...IS` is an **inner-shareable broadcast** that stalls the issuing CPU until all other CPUs in the inner-shareable domain ACK. On a 64-core system that's an expensive synchronization. By batching many `vfree`s on a lazy purge list and issuing **one** `flush_tlb_kernel_range()` when the queued size crosses `lazy_max_pages()`, the kernel amortizes the broadcast cost across many frees. Frequently the union range is so large that `flush_tlb_kernel_range` falls back to `flush_tlb_all`, paying one full-TLB flush instead of N range flushes.

---

### Q2. Walk through `vfree(p)` step-by-step on ARM64.  `[Qualcomm]`

1. NULL/in_interrupt checks; if in IRQ, delegate to `vfree_atomic`.
2. `remove_vm_area(p)` → `find_unlink_vmap_area` locates and removes the `vmap_area` from the busy rbtree.
3. `vunmap_range_noflush` walks `init_mm` page tables and writes `0` to each PTE in the range — no TLBI yet.
4. `__free_pages` returns each backing page to buddy immediately.
5. `free_vmap_area_noflush` appends the `vmap_area` to the global lazy purge list and increments `vmap_lazy_nr`.
6. If `vmap_lazy_nr > lazy_max_pages()`, `schedule_work(&drain_vmap_work)` triggers the deferred purge.
7. `kfree(area->pages)` and `kfree(vm)` clean up descriptors.
8. Return. The TLBI happens later in `__purge_vmap_area_lazy` → `flush_tlb_kernel_range` → `TLBI VAALE1IS` + `DSB ISH` + `ISB`.

---

### Q3. Why is `vfree` unsafe in IRQ context, and what's the alternative?  `[Google]`

`vfree` takes the `vmap_area_lock` (sleepable in some paths via `down_read`), walks page tables (may need `init_mm.page_table_lock`), and calls `kfree` which may sleep if `init_on_free` is set. None of those are IRQ-safe. The alternative is `vfree_atomic(p)`: pushes the pointer onto a per-CPU lockless list (`llist_add` via `CASAL` on arm64 with LSE) and schedules a workqueue. The actual `vfree` happens later in process context.

---

### Q4. After `vfree(p)`, is the VA `p` immediately reusable?  `[Nvidia]`

No. The PTEs are cleared but the `vmap_area` is sitting on the lazy purge list, **outside** the free rbtree. Another `vmalloc` won't allocate that range until purge runs and merges the freed area back into the free tree. From the allocator's perspective the VA is in limbo.

The backing **physical pages**, however, are immediately on buddy and can be re-allocated for any other purpose right away.

---

### Q5. Difference between `vunmap()` and `vfree()`?  `[Qualcomm]`

- `vunmap(p)` — designed for areas created by `vmap()` (which maps caller-owned pages into vmalloc area). It clears PTEs and frees the `vm_struct`/`vmap_area`, but **does not free the pages**.
- `vfree(p)` — designed for areas created by `vmalloc()` family. It also frees the backing pages via `__free_pages`. Internally `vfree` checks `area->flags & VM_ALLOC` to decide whether to free pages.

Using `vfree` on a `vmap()`'d area accidentally also frees pages that the caller still owns — prefer `vunmap` for clarity.

---

### Q6. What's `flush_tlb_kernel_range` doing differently for very large ranges?  `[Google]`

If `(end - start) > (MAX_TLBI_OPS << PAGE_SHIFT)` (defaults to ~4 MB on 4K pages), the per-page `TLBI VAALE1IS` loop becomes too expensive, so it falls back to `flush_tlb_all` — a single `TLBI VMALLE1IS` invalidates the entire EL1 TLB on all CPUs in inner-shareable domain. Trade-off: one giant invalidation that takes everyone's working set with it, versus thousands of broadcasts.

---

### Q7. A driver vfree's a 1 MB buffer in a tight loop. dmesg shows TLB-related soft-lockups. Diagnose.  `[Nvidia]` `[Qualcomm]`

Unlikely with lazy purge — the cost should be amortized. Possible causes:

1. `CONFIG_DEBUG_PAGEALLOC=y` disables lazy purge — every `vfree` synchronously broadcasts.
2. `CONFIG_KASAN_VMALLOC=y` does extra shadow work per free.
3. The driver also calls `vmalloc` in the same loop and the purge worker is constantly running.

Fix:

- Allocate once, reuse.
- Use a kmem_cache or slab for repeated small alloc/free.
- If repeated large buffers truly needed, use `kvmalloc`/`kvfree` so it stays in slab when small.

---

### Q8. UAF after vfree: explain why dereferences may sometimes work and sometimes fault.  `[Google]`

Because TLB invalidation is lazy. After `vfree(p)`:

- PTEs in `init_mm` are zero.
- Backing pages are on buddy (possibly re-allocated by another subsystem already).
- A CPU that previously accessed `p` may have the translation cached in its TLB.

If the access hits a cached TLB entry, the CPU silently reads the now-stale physical page (which may belong to someone else — info leak or data corruption). If the access misses, the MMU walks the now-zero PTE and faults. The behavior is non-deterministic — the classic "works on my machine" UAF.

---

### Q9. Why does `vfree` free backing pages immediately but defer VA reuse?  `[Qualcomm]`

Freeing pages to buddy is **local** — just bookkeeping on free-area lists. No TLB work needed (linear-map PTEs remain valid). Returning the VA range to the free rbtree, however, allows re-allocation that depends on the TLB being clean for that VA. So the kernel does the cheap thing immediately and defers the expensive TLB-dependent thing.

---

### Q10. Implement `kvfree` from scratch.  `[Google]`

```c
void my_kvfree(const void *p)
{
    if (!p)
        return;
    if (is_vmalloc_addr(p))
        vfree(p);
    else
        kfree(p);
}
```

`is_vmalloc_addr(p)` is a cheap range check: `(unsigned long)p >= VMALLOC_START && p < VMALLOC_END`. No page-table walk. Pairs with `kvmalloc()` which may transparently fall back to vmalloc on large allocations.

---

### Q11. `kvfree_sensitive(p, len)` — how is it different from `kvfree`?  `[Qualcomm]`

It calls `memzero_explicit(p, len)` before dispatching to `kfree` or `vfree`. The `_explicit` variant uses `barrier_data()` to defeat compiler dead-store elimination, so the wipe always happens. Use for keys/credentials stored in `kvmalloc`'d buffers.

---

### Q12. Staff-level: design a per-CPU vmalloc-free fast path with no global locking.  `[Nvidia]` `[Google]`

Observation: in 6.6 the global `vmap_area_lock` was already replaced by per-CPU vmap nodes for both alloc and free. To go further:

- Each CPU maintains its own purge list; `__purge_vmap_area_lazy` drains them in parallel.
- TLB flush still must be broadcast (architectural), but use ARM v8.4 `TLBI RVAE1IS` (range-based TLBI) to coalesce.
- Per-CPU `vfree_deferred` workqueue avoids cross-CPU wakeups.

This is essentially what the kernel does today; further wins require new arch hooks (e.g., async TLB invalidate hardware).

---

### Q13. Why doesn't `vfree` unmap from the linear map?  `[Qualcomm]`

The pages backing a vmalloc area were never in the vmalloc VA range *via the linear map* — they're in both: the linear map (PAGE_OFFSET-based) and the vmalloc area (the new mapping). `vfree` only tears down the vmalloc-side mapping. The linear-map mapping survives because every page of RAM is permanently linear-mapped at boot; tearing it down would require splitting large block mappings and broadcasting TLBs — too expensive for every free.

---

### Q14. ABBA risk: `vfree` inside an RCU read-side critical section?  `[Google]`

Direct `vfree` from inside `rcu_read_lock()`/`rcu_read_unlock()` is technically allowed but you must not sleep — and `vfree` may sleep (`might_sleep_if(!in_interrupt())`). Use `vfree_atomic` or defer via `call_rcu` to a callback that runs in softirq/workqueue. For pointers protected by RCU, the canonical pattern is `kfree_rcu`-like: schedule the free for after a grace period.

---

### Q15. What changes if `vfree` is called on a `vmalloc_huge`'d area?  `[Nvidia]`

`vunmap_range_noflush` recognizes PMD-block entries and clears them with `pmd_clear()` — a single 8-byte store collapses 2 MB of mapping. `__free_pages(area->pages[i], area->page_order)` returns each 2 MB block to buddy as `order=9`. The TLBI loop on arm64 currently still iterates per-page; on cores with ARMv8.4 range TLBI (`TLBI RVAE1IS`), the kernel can issue a single instruction for the whole 2 MB — a future optimization area.

---

## Common pitfalls

| Pitfall                                          | Fix |
|--------------------------------------------------|-----|
| `vfree` from IRQ                                 | Use `vfree_atomic`. |
| Touching `p` after `vfree(p)`                    | Don't. KASAN-VMALLOC + DEBUG_PAGEALLOC during CI. |
| Using `vunmap` when you meant `vfree`            | `vunmap` leaks the pages. |
| Assuming `vfree` flushes TLB immediately         | It doesn't — lazy purge. Plan accordingly for security boundaries. |
| Mixing `vmap`'d and `vmalloc`'d cleanup          | Match: `vmap` ↔ `vunmap`, `vmalloc` ↔ `vfree`. |
