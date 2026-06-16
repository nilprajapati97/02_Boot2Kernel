# kzalloc — Internals

> Linux 6.6 · ARM64. `kzalloc()` is a thin inline over `kmalloc()` — this
> document focuses on the parts that **differ** from
> [`../kmalloc/02_internals.md`](../kmalloc/02_internals.md).

---

## 1. Source location

```c
/* include/linux/slab.h:726 */
static inline __alloc_size(1) void *kzalloc(size_t size, gfp_t flags)
{
    return kmalloc(size, flags | __GFP_ZERO);
}
```

There is **no `kzalloc` symbol** in `mm/slub.c` — every call site is inlined into a `kmalloc` call with the `__GFP_ZERO` bit added at compile time. `perf record` will attribute all samples to `__kmalloc` / `kmem_cache_alloc_trace`.

---

## 2. `__GFP_ZERO` propagation

```
caller --kzalloc--> kmalloc(size, flags|__GFP_ZERO)
                       |
                       v
                  __kmalloc(size, gfpflags)
                       |
                       v
                  __do_kmalloc_node()
                       |
                       v
                  slab_alloc_node(s, NULL, gfpflags, …)
                       |
                       v
                  fast/slow path returns `object`
                       |
                       v
   if (slab_want_init_on_alloc(gfpflags, s))
        memset(object, 0, s->object_size);   <-- the zero pass
```

Key call site: `slab_post_alloc_hook()` ([`mm/slab.h:slab_post_alloc_hook`](https://elixir.bootlin.com/linux/v6.6/source/mm/slab.h#L767)), invoked at the end of both fast and slow paths.

---

## 3. `slab_want_init_on_alloc()` decision

```c
static inline bool slab_want_init_on_alloc(gfp_t flags, struct kmem_cache *c)
{
    if (static_branch_maybe(CONFIG_INIT_ON_ALLOC_DEFAULT_ON,
                            &init_on_alloc))
        return !(c->flags & SLAB_NO_OBJ_EXT) &&
               !(c->flags & __GFP_SKIP_ZERO);
    return flags & __GFP_ZERO;
}
```

So `kzalloc` zeroes whenever:

- caller passed `__GFP_ZERO`, **or**
- `init_on_alloc` static key is enabled (boot arg / Kconfig).

Result: on a hardened kernel even plain `kmalloc` produces zeroed memory; `kzalloc` becomes a *style* statement of intent rather than a performance differentiator.

---

## 4. Zero size vs. object size

The memset is `s->object_size` bytes, **not** the bucket size (`s->size`):

```
   slab bucket:        |   object_size   | red-zone | freelist ptr | padding |
                       |<--- zeroed ---->|
                       <----------------- s->size ---------------->
```

Hence padding between user data and the next bucket boundary is *not* cleared.
`memzero_explicit()` or manual `memset(p, 0, ksize(p))` is needed if you intend to expose the padding to userspace.

---

## 5. Large-kmalloc (> 8 KB) zeroing

`__kmalloc_large_node()` ([`mm/slub.c:3922`](https://elixir.bootlin.com/linux/v6.6/source/mm/slub.c#L3922)) honors `__GFP_ZERO` by calling `alloc_pages(__GFP_ZERO, order)` — the **page allocator** clears the pages via `clear_highpage()` / `prep_new_page()` in [`mm/page_alloc.c:prep_new_page`](https://elixir.bootlin.com/linux/v6.6/source/mm/page_alloc.c#L1546). On ARM64 this uses `clear_page()` in [`arch/arm64/lib/clear_page.S`](https://elixir.bootlin.com/linux/v6.6/source/arch/arm64/lib/clear_page.S), which is a `DC ZVA` loop.

---

## 6. Interaction with `ctor`

`kmem_cache_create()` allows a `ctor` callback that runs once when an object is first taken from the slab page. SLUB refuses to mix `__GFP_ZERO` with a constructor:

```c
/* mm/slub.c:slab_alloc_node */
if (unlikely(slab_want_init_on_alloc(gfpflags, s))) {
    if (WARN_ON_ONCE(c->ctor))
        ;  /* both can't apply */
    else
        memset(object, 0, s->object_size);
}
```

So `kzalloc` on a cache that has a `ctor` will WARN — use plain `kmem_cache_zalloc()` is also disallowed in that case.

---

## 7. KASAN / KFENCE behavior

- **KASAN**: object is unpoisoned **before** the zero pass, so `memset` doesn't trip shadow checks. Quarantine on free is unchanged.
- **KFENCE**: when a sampled allocation is served from KFENCE's pool, `__GFP_ZERO` is honored inside `kfence_alloc()` ([`mm/kfence/core.c`](https://elixir.bootlin.com/linux/v6.6/source/mm/kfence/core.c)). The whole guarded page is zeroed regardless of object size (since each KFENCE object owns its own page).
- **`CONFIG_INIT_ON_FREE_DEFAULT_ON`** complements: with both alloc-init and free-init on, freed memory is wiped immediately *and* re-wiped on next alloc — defense in depth against UAF info leaks.

---

## 8. Performance notes

| Cost on ARM64 (Neoverse N1, 64-B cache lines) |
|------------------------------------------------|
| 64 B object zero: 1 × `STP xzr, xzr, [x0]`, `STP xzr, xzr, [x0,#16]` × 2 — handful of cycles. |
| 256 B object zero: 4 × `DC ZVA` cache lines — bypasses read, ~equal to one cacheline store. |
| 4 KB page zero (large path): `clear_page` = 64 × `DC ZVA` — memory-bandwidth bound, not CPU. |

`DC ZVA` requires the line to be in cacheable memory and not user-locked; both true for slab pages.

---

## 9. Source-walk cheat sheet

| File                       | Function                          | Role                        |
|----------------------------|-----------------------------------|-----------------------------|
| `include/linux/slab.h:726` | `kzalloc()`                       | Inline wrapper              |
| `mm/slab.h:slab_want_init_on_alloc` | gate function            | Decides whether to memset   |
| `mm/slab.h:slab_post_alloc_hook`    | post-alloc work          | Runs the memset             |
| `arch/arm64/lib/memset.S`  | `__memset`                        | The actual zeroing          |
| `arch/arm64/lib/clear_page.S` | `clear_page`                   | Page-granular zero (large path) |
| `mm/page_alloc.c:prep_new_page`     | page prep                | Honors `__GFP_ZERO`         |
