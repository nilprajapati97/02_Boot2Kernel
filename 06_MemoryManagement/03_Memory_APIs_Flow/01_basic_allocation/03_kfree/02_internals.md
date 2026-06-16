# kfree — Internals

> Linux 6.6 · ARM64. Mirror image of the kmalloc allocation walk;
> see [`../kmalloc/02_internals.md`](../kmalloc/02_internals.md) for the
> SLUB data structures referenced here.

---

## 1. Top-level dispatch

```c
/* mm/slub.c:4346 */
void kfree(const void *object)
{
    struct folio *folio;
    struct slab  *slab;
    struct kmem_cache *s;

    trace_kfree(_RET_IP_, object);
    if (unlikely(ZERO_OR_NULL_PTR(object)))
        return;

    folio = virt_to_folio(object);
    if (unlikely(!folio_test_slab(folio))) {
        free_large_kmalloc(folio, (void *)object);
        return;
    }

    slab = folio_slab(folio);
    s    = slab->slab_cache;
    __kmem_cache_free(s, (void *)object, _RET_IP_);
}
```

Two branches:

- **Large-kmalloc** (folio not tagged as slab) → `free_large_kmalloc()` → `__free_pages()`.
- **Normal SLUB** → `__kmem_cache_free()` → `slab_free()` → fast or slow free.

---

## 2. Fast free path — `do_slab_free()`

[`mm/slub.c:do_slab_free`](https://elixir.bootlin.com/linux/v6.6/source/mm/slub.c#L3737)

```c
redo:
    c   = raw_cpu_ptr(s->cpu_slab);
    tid = READ_ONCE(c->tid);
    barrier();

    if (likely(slab == c->slab)) {                   /* object's slab is c->slab */
        void **freelist = READ_ONCE(c->freelist);
        set_freepointer(s, tail_obj, freelist);      /* link old head as next */
        if (!this_cpu_cmpxchg_double(s->cpu_slab->freelist, s->cpu_slab->tid,
                                     freelist, tid,
                                     head,      next_tid(tid)))
            goto redo;
        stat(s, FREE_FASTPATH);
    } else {
        __slab_free(s, slab, head, tail, cnt, addr); /* SLOW PATH */
    }
```

Conditions for the fast path:

1. The object's slab is **`c->slab`** (i.e., the current per-CPU active slab).
2. The `cmpxchg_double` on `(freelist, tid)` succeeds atomically — same primitive used by `kmalloc`'s fast path.

If both hold, the free is **lockless** and **O(1)** — just a freelist push.

---

## 3. Slow free path — `__slab_free()`

[`mm/slub.c:__slab_free`](https://elixir.bootlin.com/linux/v6.6/source/mm/slub.c#L3796)

Handles the cases the fast path can't:

| Case                                          | Action |
|-----------------------------------------------|--------|
| Object's slab is **not** `c->slab` (remote free) | `cmpxchg_double` directly on `slab->freelist` + `slab->counters`; updates `inuse`. |
| Slab transitions from full → partial          | Move to `n->partial` under `n->list_lock`. |
| Slab transitions from partial → empty *and* `n->nr_partial > s->min_partial` | `discard_slab()` → `__free_pages()` — returns to buddy. |
| Slab is frozen (owned by a CPU's `c->partial`) | Just update counters; CPU will pick it up later. |
| `SLAB_TYPESAFE_BY_RCU`                        | Defer page release via `call_rcu(&slab->rcu_head, rcu_free_slab)`. |

---

## 4. Large-kmalloc free — `free_large_kmalloc()`

[`mm/slub.c:free_large_kmalloc`](https://elixir.bootlin.com/linux/v6.6/source/mm/slub.c#L4321)

```c
static void free_large_kmalloc(struct folio *folio, void *object)
{
    unsigned int order = folio_order(folio);

    if (WARN_ON_ONCE(order == 0))   /* must be > KMALLOC_MAX_CACHE_SIZE */
        return;

    kasan_kfree_large(object);
    kmsan_kfree_large(object);
    mod_lruvec_page_state(folio_page(folio, 0),
                          NR_SLAB_UNRECLAIMABLE_B, -(PAGE_SIZE << order));
    __free_pages(folio_page(folio, 0), order);
}
```

Returns `2^order` pages directly to buddy via `__free_pages` — no slab bookkeeping.

---

## 5. `ksize()` and how `kfree` finds the size

`kfree` doesn't take a size argument. It recovers it from:

- `slab->slab_cache->size` for normal SLUB objects.
- `folio_size(folio)` for large-kmalloc.

`ksize(p)` ([`mm/slub.c:ksize`](https://elixir.bootlin.com/linux/v6.6/source/mm/slub.c#L4570)) exposes the **actual** usable size (which is `s->size` for SLUB, rounded up). Useful when caller wants to know how much padding it can safely use — but **note**: callers must not write past `s->object_size` if they rely on KASAN catching overflows — KASAN's redzone lives between `object_size` and `size`.

---

## 6. `kfree_sensitive()`

[`mm/slab_common.c:kfree_sensitive`](https://elixir.bootlin.com/linux/v6.6/source/mm/slab_common.c#L1186)

```c
void kfree_sensitive(const void *p)
{
    size_t ks;
    void *mem = (void *)p;
    ks = ksize(mem);
    if (ks)
        memzero_explicit(mem, ks);   /* DC ZVA, not optimizable away */
    kfree(mem);
}
```

`memzero_explicit` ([`lib/string.c`](https://elixir.bootlin.com/linux/v6.6/source/lib/string.c#L932)) calls `barrier_data()` to defeat the compiler's dead-store elimination. Used for cryptographic key material, kernel keyring entries.

---

## 7. `kvfree()` and `kvfree_call_rcu()`

```c
/* mm/util.c:kvfree */
void kvfree(const void *addr)
{
    if (is_vmalloc_addr(addr))
        vfree(addr);
    else
        kfree(addr);
}
```

`is_vmalloc_addr()` checks whether the pointer is within `VMALLOC_START..VMALLOC_END` — see [`mm/vmalloc.c`](https://elixir.bootlin.com/linux/v6.6/source/mm/vmalloc.c). Cheap range check; no page-table walk.

Used when the allocation came from `kvmalloc()` which may transparently fall back to `vmalloc` when `kmalloc` cannot satisfy a large request.

---

## 8. KASAN / KFENCE / poisoning on free

| Stage                          | Action |
|--------------------------------|--------|
| Top of `slab_free_freelist_hook`| `kasan_slab_free()` — poison object in shadow, optionally quarantine. |
| If `init_on_free` is set       | `memset(object, 0, s->object_size)` before the freelist push (clears UAF leakable data). |
| Free-pointer hardened          | `set_freepointer()` XORs the linked pointer with `s->random ^ addr_of_ptr`. |
| KFENCE-served object           | `kfence_free()` poisons the guard page and marks the slot for re-use after a delay; future deref faults. |
| KMSAN                          | `kmsan_kfree_large` / `kmsan_slab_free` updates origin/shadow. |

---

## 9. Source-walk cheat sheet

| File                  | Function                          | Role |
|-----------------------|-----------------------------------|------|
| `mm/slub.c`           | `kfree()`                         | Entry, NULL/large dispatch |
| `mm/slub.c`           | `__kmem_cache_free()` / `slab_free()` | Fast/slow free orchestration |
| `mm/slub.c`           | `do_slab_free()`                  | Lockless fast path           |
| `mm/slub.c`           | `__slab_free()`                   | Slow / remote free           |
| `mm/slub.c`           | `free_large_kmalloc()`            | > 8 KB direct-to-buddy       |
| `mm/slab_common.c`    | `kfree_sensitive()`               | Zero-then-free               |
| `mm/util.c`           | `kvfree()` / `kvfree_sensitive()` | Auto-dispatch vmalloc/slub   |
| `kernel/rcu/tree.c`   | `kfree_rcu_*`                     | Deferred free batching       |
