# vfree — Internals

> Linux 6.6 · ARM64. The interesting story here is the **lazy purge**
> machinery, not the immediate `vfree` call.

---

## 1. Top-level dispatch

```c
/* mm/vmalloc.c:2842 */
void vfree(const void *addr)
{
    struct vm_struct *vm;
    int i;

    if (!addr)
        return;

    BUG_ON(in_nmi());
    might_sleep_if(!in_interrupt());

    if (in_interrupt()) {
        vfree_atomic(addr);
        return;
    }

    vm = remove_vm_area(addr);     /* finds vmap_area, removes from busy tree */
    if (!vm) {
        WARN(1, KERN_ERR "Trying to vfree() nonexistent vm area (%p)\n", addr);
        return;
    }

    /* Free backing pages back to buddy. */
    if (!(vm->flags & VM_MAP))     /* VM_ALLOC: we own the pages */
        vfree_deferred_pages(vm);

    kfree(vm);   /* the descriptor; vmap_area was already kfree'd inside remove_vm_area */
}
```

(The actual code is split across `vfree`, `__vunmap`, and helpers; this is the conceptual flow.)

---

## 2. `remove_vm_area()` — the busy-side work

[`mm/vmalloc.c:remove_vm_area`](https://elixir.bootlin.com/linux/v6.6/source/mm/vmalloc.c#L2768)

```c
struct vm_struct *remove_vm_area(const void *addr)
{
    struct vmap_area *va;

    va = find_unlink_vmap_area((unsigned long)addr);
    if (!va || !va->vm)
        return NULL;

    vm = va->vm;
    vmap_remove_mapping(vm);                    /* clear PTEs */
    free_vmap_area_noflush(va);                 /* push onto lazy purge list */
    return vm;
}
```

Key: **`free_vmap_area_noflush`** — the word "noflush" is the headline. The PTEs were cleared via `vunmap_range_noflush`, but no TLBI is issued yet.

---

## 3. `vunmap_range_noflush()`

[`mm/vmalloc.c:vunmap_range_noflush`](https://elixir.bootlin.com/linux/v6.6/source/mm/vmalloc.c#L406)

Walks `init_mm` page tables and writes `0` to each PTE in the range:

```c
vunmap_p4d_range -> vunmap_pud_range -> vunmap_pmd_range -> vunmap_pte_range
  -> pte_clear(&init_mm, addr, pte);
```

No flush between PTE clears; a final `dsb ishst` is **not even issued here** — it's bundled with the eventual TLBI at purge time.

---

## 4. Lazy purge list

[`mm/vmalloc.c:free_vmap_area_noflush`](https://elixir.bootlin.com/linux/v6.6/source/mm/vmalloc.c#L1922)

```c
static void free_vmap_area_noflush(struct vmap_area *va)
{
    unsigned long nr_lazy;
    nr_lazy = atomic_long_add_return((va->va_end - va->va_start) >> PAGE_SHIFT,
                                     &vmap_lazy_nr);
    insert_vmap_purge_list(va);
    if (unlikely(nr_lazy > lazy_max_pages()))
        schedule_work(&drain_vmap_work);
}
```

- `vmap_lazy_nr` counts pages awaiting purge across all CPUs.
- `lazy_max_pages()` = `32 UL * num_online_cpus() * PAGE_SIZE >> PAGE_SHIFT` (roughly 128 KB per CPU on a 4 KB page system, but auto-tuned).
- When threshold crossed, `drain_vmap_work` (a workqueue) runs `__purge_vmap_area_lazy()`.

---

## 5. `__purge_vmap_area_lazy()` — the actual TLB flush

[`mm/vmalloc.c:__purge_vmap_area_lazy`](https://elixir.bootlin.com/linux/v6.6/source/mm/vmalloc.c#L1814)

```c
static bool __purge_vmap_area_lazy(unsigned long start, unsigned long end)
{
    LIST_HEAD(local_purge_list);
    ...
    /* Drain the global purge list into a local list under spin_lock */
    spin_lock(&purge_vmap_area_lock);
    list_replace_init(&purge_vmap_area_list, &local_purge_list);
    spin_unlock(&purge_vmap_area_lock);

    /* One TLB flush for the union of all ranges. */
    flush_tlb_kernel_range(start, end);

    /* Now merge each freed area back into the free rbtree. */
    list_for_each_entry_safe(va, n_va, &local_purge_list, list) {
        merge_or_add_vmap_area(va, ...);
        atomic_long_sub(nr, &vmap_lazy_nr);
    }
    return true;
}
```

**One** `flush_tlb_kernel_range` covers all queued ranges — huge win on a 64-core arm64 box where each `flush_tlb_kernel_range` would otherwise broadcast IPIs.

---

## 6. Freeing backing pages

After PTEs are cleared, the loop in `__vunmap`:

```c
if (deallocate_pages) {
    for (i = 0; i < area->nr_pages; i += 1U << area->page_order)
        __free_pages(area->pages[i], area->page_order);
    kvfree(area->pages);
}
```

Each `struct page *` from `area->pages[]` is returned to buddy via `__free_pages`. For huge-page mappings (`page_order > 0`), the entire `2^order` block is returned in one call.

The `area->pages` array itself was allocated via `kvmalloc_node` and is freed via `kvfree`.

---

## 7. `vfree_atomic()` — deferred path

[`mm/vmalloc.c:vfree_atomic`](https://elixir.bootlin.com/linux/v6.6/source/mm/vmalloc.c#L2810)

```c
void vfree_atomic(const void *addr)
{
    struct vfree_deferred *p;

    BUG_ON(in_nmi());
    if (!addr) return;
    p = this_cpu_ptr(&vfree_deferred);
    if (llist_add((struct llist_node *)addr, &p->list))
        schedule_work(&p->wq);
}
```

Pushes the pointer onto a per-CPU lockless list; a workqueue eventually drains it and calls `__vfree()` from process context. Result: O(1) atomic-safe free, at the cost of a workqueue wakeup. Used heavily by network stack and `kfree_rcu` fallback paths.

---

## 8. `vunmap()` vs `vfree()`

For memory obtained via `vmap()` (which maps an array of caller-provided pages into vmalloc area):

- `vunmap(addr)` — clears PTEs, returns the VA range to the free rbtree, but **does not** call `__free_pages` on the underlying pages (the caller still owns them).
- `vfree(addr)` on a `VM_MAP`-flagged area — same as `vunmap`; it checks `area->flags & VM_ALLOC` before freeing pages.

Use `vunmap` for clarity; `vfree` works but obscures intent.

---

## 9. Locking summary

| Phase                       | Lock                                                  |
|-----------------------------|-------------------------------------------------------|
| `find_unlink_vmap_area`     | per-CPU `vmap_node->busy.lock` or global `vmap_area_lock` |
| `vunmap_range_noflush`      | `init_mm.page_table_lock` (briefly, per intermediate alloc) |
| `free_vmap_area_noflush`    | `purge_vmap_area_lock` (only when adding to global list) |
| `__purge_vmap_area_lazy`    | `purge_vmap_area_lock` (under `mutex_trylock(&vmap_purge_lock)` to serialize) |

---

## 10. Source-walk cheat sheet

| File             | Function                          | Role                                  |
|------------------|-----------------------------------|---------------------------------------|
| `mm/vmalloc.c`   | `vfree()`                         | Entry, dispatch sleep vs atomic       |
| `mm/vmalloc.c`   | `vfree_atomic()`                  | Defer to per-CPU workqueue            |
| `mm/vmalloc.c`   | `__vunmap()` / `remove_vm_area()` | Clear PTEs, push to purge list        |
| `mm/vmalloc.c`   | `vunmap_range_noflush()`          | Walk pgtables, zero PTEs              |
| `mm/vmalloc.c`   | `free_vmap_area_noflush()`        | Append to lazy purge list             |
| `mm/vmalloc.c`   | `__purge_vmap_area_lazy()`        | Single TLBI for batched ranges        |
| `arch/arm64/include/asm/tlbflush.h` | `flush_tlb_kernel_range()` | Issues `TLBI VAE1IS` broadcast |
