# vfree — Overview

> Linux **6.6 LTS** · ARM64 · vmalloc subsystem
> Header: `<linux/vmalloc.h>` · Source: [`mm/vmalloc.c:vfree`](https://elixir.bootlin.com/linux/v6.6/source/mm/vmalloc.c#L2842)

---

## 1. One-line definition

`vfree(addr)` releases a kernel virtual range that was obtained from the
vmalloc family (`vmalloc`, `vzalloc`, `vmalloc_node`, `vmap`, etc.). The
release is **asynchronous from a TLB standpoint**: the VA range is put on a
lazy-purge list, the backing pages are freed back to the buddy allocator,
and a single TLB invalidation is issued later when the purge threshold is hit.

---

## 2. Prototype family

```c
/* include/linux/vmalloc.h */
void vfree(const void *addr);
void vfree_atomic(const void *addr);    /* safe in IRQ/atomic context */
void vunmap(const void *addr);          /* for vmap(): unmap only, don't free pages */
void kvfree(const void *addr);          /* dispatches kfree / vfree */
void kvfree_sensitive(const void *addr, size_t len);
```

---

## 3. Behavior contract

| Input                                | Action                                                       |
|--------------------------------------|--------------------------------------------------------------|
| `NULL`                               | No-op.                                                       |
| Pointer from `vmalloc`/`vzalloc`/etc.| Unmap PTEs (lazy), free backing pages, free `vm_struct`/`vmap_area`. |
| Pointer from `vmap()`                | Unmap PTEs (lazy), free `vm_struct`/`vmap_area`. **Does not free the pages** — the caller owns them. (`vfree` actually *does* call `__free_pages` for `vmap`'d areas — prefer `vunmap` if pages are externally owned.) |
| Pointer from `ioremap`               | Wrong API — use `iounmap`.                                   |
| Pointer from `kmalloc`/buddy         | Corruption: `vfree` will treat the address as inside the vmalloc rbtree and not find it (WARN + return). |
| Already-freed                        | `__vunmap` WARN "Trying to vfree() nonexistent vm area".     |

---

## 4. Context constraints

| Context                       | `vfree` | `vfree_atomic` |
|-------------------------------|---------|----------------|
| Process context, sleepable    | yes     | yes            |
| IRQ / softirq                 | **no**  | yes            |
| NMI                           | no      | no             |
| Spinlock held                 | no      | yes            |

`vfree` may sleep because it can take `vmap_area_lock` and walks `init_mm` page tables. `vfree_atomic` ([`mm/vmalloc.c:vfree_atomic`](https://elixir.bootlin.com/linux/v6.6/source/mm/vmalloc.c#L2810)) just defers everything to a workqueue (`vfree_deferred`) and is safe from IRQ.

---

## 5. Sibling APIs

| API                              | Use case                                                       |
|----------------------------------|----------------------------------------------------------------|
| `vfree(p)`                       | Default. Memory from `vmalloc` family.                         |
| `vfree_atomic(p)`                | Same memory, but you're in IRQ/atomic context.                 |
| `vunmap(p)`                      | Unmap only — used with `vmap()` where caller owns the pages.    |
| `kvfree(p)`                      | `p` might be `kmalloc` or `vmalloc`; auto-dispatches.          |
| `kvfree_sensitive(p, len)`       | Zero before free (for keys); dispatches by allocator.          |
| `iounmap(p)`                     | For `ioremap` mappings (MMIO).                                  |

---

## 6. Failure modes / splats

| Splat                                                          | Meaning |
|----------------------------------------------------------------|---------|
| `Trying to vfree() nonexistent vm area (…)`                    | Address not in vmalloc area, or already freed. |
| `Trying to vfree() bad address (…)`                            | Address inside vmalloc area but no matching `vmap_area` — corruption. |
| `vfree_atomic` called from NMI                                 | Use a different mechanism (ring buffer, deferred work). |
| `BUG: scheduling while atomic`                                 | Called plain `vfree` from atomic context — use `vfree_atomic`. |

---

## 7. Lazy TLB invalidation — the headline

> `vfree` returns quickly. The PTEs are cleared and recorded on a purge
> list; the actual `flush_tlb_kernel_range()` happens **later**, when
> the list grows beyond `lazy_max_pages()`. Until purge, the VA may
> still resolve in some CPU TLBs — but the `vmap_area` and `vm_struct`
> are already gone, so re-deref is **undefined**.

This design amortizes TLB-broadcast IPI cost across many frees — essential for large-core arm64 systems.

---

## 8. Minimal usage

```c
void *buf = vmalloc(SZ_4M);
if (!buf) return -ENOMEM;
/* ... */
vfree(buf);   /* safe on NULL */
```

From an IRQ handler:

```c
vfree_atomic(buf);
```

---

## 9. Cross-references

- Internals → [02_internals.md](02_internals.md)
- ARM64 call flow → [03_arm64_callflow.md](03_arm64_callflow.md)
- Memory map → [04_memory_map.md](04_memory_map.md)
- Interview Q&A → [05_interview_qna.md](05_interview_qna.md)
- Sibling → [`../vmalloc/`](../vmalloc/)
