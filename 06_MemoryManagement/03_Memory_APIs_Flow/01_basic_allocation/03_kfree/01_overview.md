# kfree — Overview

> Linux **6.6 LTS** · ARM64 · SLUB allocator
> Header: `<linux/slab.h>` · Source: [`mm/slub.c:kfree`](https://elixir.bootlin.com/linux/v6.6/source/mm/slub.c#L4346)

---

## 1. One-line definition

`kfree(p)` returns a SLUB-allocated object (originally obtained via `kmalloc`/`kzalloc`/`kmem_cache_alloc_trace`) to its slab freelist, or — for objects above `KMALLOC_MAX_CACHE_SIZE` — releases the underlying pages directly to the buddy allocator.

---

## 2. Prototype

```c
/* include/linux/slab.h */
void kfree(const void *objp);
void kfree_sensitive(const void *objp);    /* memzero before free (was kzfree) */
void kvfree(const void *addr);             /* dispatches kfree() vs vfree() */
size_t ksize(const void *objp);            /* actual usable size of allocation */
```

`kfree()` itself is **not** an inline; it's a real function in `mm/slub.c`.

---

## 3. Behavior contract

| Input                                                      | Action                                                                                                                          |
| ---------------------------------------------------------- | ------------------------------------------------------------------------------------------------------------------------------- |
| `NULL`                                                   | No-op (`ZERO_OR_NULL_PTR` check at top).                                                                                      |
| `ZERO_SIZE_PTR` (= `(void *)16`)                       | No-op.                                                                                                                          |
| Pointer obtained from `kmalloc` ≤ 8 KB                  | Push back to the owning slab's freelist (fast path) or remote-free path.                                                        |
| Pointer obtained from `kmalloc` > 8 KB                   | `free_large_kmalloc()` → `__free_pages(folio_page(folio,0), order)`.                                                       |
| Pointer obtained from `vmalloc`                          | **WARNING** ("kfree of vmalloc address") — use `vfree()`.                                                              |
| Pointer obtained from `alloc_pages`/`__get_free_pages` | Undefined behavior —`kfree` looks up the slab metadata and may corrupt the buddy state.                                      |
| Already-freed pointer (double free)                        | Detected by SLUB freelist checks (loud splat with `CONFIG_SLUB_DEBUG`); silent corruption otherwise. KASAN catches it always. |

---

## 4. Context constraints

Same as `kmalloc` consumers:

| Context                    | Allowed?                                                                |
| -------------------------- | ----------------------------------------------------------------------- |
| Process context, sleepable | Yes                                                                     |
| IRQ / softirq              | Yes (slow path may take `n->list_lock` — short spinlock, OK in IRQ). |
| Spinlock held              | Yes                                                                     |
| NMI                        | **No** — same reasons as `kmalloc`.                            |
| RCU read-side              | Yes; for RCU-protected objects use `kfree_rcu(p, rcu)` instead.       |

---

## 5. Sibling APIs

| API                           | Use case                                                                                                                 |
| ----------------------------- | ------------------------------------------------------------------------------------------------------------------------ |
| `kfree(p)`                  | Default. Object was obtained from `kmalloc` family.                                                                    |
| `kfree_sensitive(p)`        | Zero the buffer with `memzero_explicit()` before freeing — for keys, credentials. Replaces the old `kzfree`.        |
| `kfree_rcu(p, member)`      | Defer free until next RCU grace period; safe for RCU-readable objects.                                                   |
| `kvfree(p)`                 | Pointer might be from `kmalloc` *or* `vmalloc` (e.g., `kvmalloc()` return); dispatches by `is_vmalloc_addr()`. |
| `kmem_cache_free(cache, p)` | When the object came from a dedicated `kmem_cache_create()` cache.                                                     |
| `free_pages(addr, order)`   | For `__get_free_pages` allocations.                                                                                    |
| `__free_pages(page, order)` | For `alloc_pages` allocations.                                                                                         |
| `vfree(p)`                  | For `vmalloc` allocations.                                                                                             |
| `dma_free_coherent(...)`    | For `dma_alloc_coherent` allocations.                                                                                  |

---

## 6. Failure modes / warning splats

| Splat                                                      | Meaning                                                                                                                                     |
| ---------------------------------------------------------- | ------------------------------------------------------------------------------------------------------------------------------------------- |
| `Trying to free already-free object`                     | Double free. Boot with `slub_debug=FZPU` to capture both call stacks.                                                                     |
| `Object 0x… size mismatch`                              | Pointer offset inside the slab is wrong — likely passing a pointer that was offset (e.g., into a struct's member) instead of the original. |
| `Cannot free … vmalloc address`                         | Caller used `vmalloc` but `kfree`. Replace with `vfree` or `kvfree`.                                                                |
| `BUG: Unable to handle kernel paging request` after free | UAF; KASAN catches via shadow at `KASAN_SHADOW_OFFSET`.                                                                                   |

---

## 7. RCU-deferred free

```c
struct foo *f = kzalloc(sizeof(*f), GFP_KERNEL);
...
/* after RCU readers might still be using f */
kfree_rcu(f, rcu);   /* needs `struct rcu_head rcu;` member */
```

This schedules the actual `kfree` via `call_rcu()` after the next RCU grace period. Internally [`include/linux/rcupdate.h:kfree_rcu`](https://elixir.bootlin.com/linux/v6.6/source/include/linux/rcupdate.h) batches frees via the `kfree_rcu_batched` infrastructure in [`kernel/rcu/tree.c`](https://elixir.bootlin.com/linux/v6.6/source/kernel/rcu/tree.c).

---

## 8. Minimal usage

```c
struct foo *f = kmalloc(sizeof(*f), GFP_KERNEL);
if (!f)
    return -ENOMEM;
/* ... use ... */
kfree(f);   /* safe even if f == NULL */
```

---

## 9. Cross-references

- Internals → [02_internals.md](02_internals.md)
- ARM64 call flow → [03_arm64_callflow.md](03_arm64_callflow.md)
- Memory map → [04_memory_map.md](04_memory_map.md)
- Interview Q&A → [05_interview_qna.md](05_interview_qna.md)
- Allocation side (kmalloc internals) → [`../kmalloc/02_internals.md`](../kmalloc/02_internals.md)
