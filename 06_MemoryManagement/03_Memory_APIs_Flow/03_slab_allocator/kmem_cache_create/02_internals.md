# kmem_cache_create — Internals

> Linux 6.6 · ARM64.

---

## 1. Entry funnel

In 6.6, both `kmem_cache_create` and `kmem_cache_create_usercopy` are inline wrappers that build a `struct kmem_cache_args` and call `__kmem_cache_create_args()` ([`mm/slab_common.c`](https://elixir.bootlin.com/linux/v6.6/source/mm/slab_common.c#L227)).

```c
struct kmem_cache *__kmem_cache_create_args(const char *name, unsigned int size,
                                            struct kmem_cache_args *args,
                                            slab_flags_t flags)
{
    struct kmem_cache *s = NULL;
    int err;

    mutex_lock(&slab_mutex);
    err = kmem_cache_sanity_check(name, size);
    if (err) { goto out; }

    /* Try to merge with an existing compatible cache. */
    s = __kmem_cache_alias(name, size, args->align, flags, args->ctor);
    if (s) {
        s->refcount++;
        goto out;
    }

    s = create_cache(name, size, args, flags);
    ...
out:
    mutex_unlock(&slab_mutex);
    return s;
}
```

The two interesting branches: **cache merging** and **fresh creation**.

---

## 2. Cache merging — `__kmem_cache_alias`

[`mm/slub.c:__kmem_cache_alias`](https://elixir.bootlin.com/linux/v6.6/source/mm/slub.c#L5530)

For every existing cache `c`:

```c
if (slab_unmergeable(s) || slab_unmergeable(c)) continue;
if (size > c->size) continue;
if ((flags & SLAB_MERGE_SAME) != (c->flags & SLAB_MERGE_SAME)) continue;
if (c->size - size >= sizeof(void *)) continue;   /* too much padding */
if (!IS_ALIGNED(c->size, align)) continue;
```

If a match is found, the new "cache" is just an **alias** — the returned `kmem_cache *` points to the existing one, with `refcount++`. Visible in `/sys/kernel/slab/<canonical>/aliases`.

**Why merge?** Reduces total number of caches (saves per-cache metadata and per-CPU state), reduces TLB pressure on slab pages. Drawback: an attacker abusing one merged cache could probe state for another, motivating `SLAB_NO_MERGE`.

---

## 3. Fresh creation — `create_cache`

[`mm/slab_common.c:create_cache`](https://elixir.bootlin.com/linux/v6.6/source/mm/slab_common.c#L227)

```c
static struct kmem_cache *create_cache(const char *name, unsigned int object_size,
                                       struct kmem_cache_args *args, slab_flags_t flags)
{
    struct kmem_cache *s;

    s = kmem_cache_zalloc(kmem_cache, GFP_KERNEL);   /* allocator-of-allocators */
    if (!s) return ERR_PTR(-ENOMEM);

    s->name        = name;
    s->size        = s->object_size = object_size;
    s->align       = args->align;
    s->ctor        = args->ctor;
    s->useroffset  = args->useroffset;
    s->usersize    = args->usersize;

    err = __kmem_cache_create(s, flags);    /* SLUB-specific setup */
    if (err) { kmem_cache_free(kmem_cache, s); return ERR_PTR(err); }

    list_add(&s->list, &slab_caches);
    s->refcount = 1;
    return s;
}
```

The **bootstrap chicken-and-egg** is solved by the global `struct kmem_cache *kmem_cache` — a cache of `kmem_cache` descriptors, hand-bootstrapped in `kmem_cache_init()`.

---

## 4. SLUB-specific setup — `__kmem_cache_create`

[`mm/slub.c:__kmem_cache_create`](https://elixir.bootlin.com/linux/v6.6/source/mm/slub.c#L5466)

Key steps:

### 4.1 `calculate_sizes()`

Determines:

- `s->object_size` — user-requested size.
- `s->inuse` — size + offset for free pointer (if `SLAB_POISON` etc., the freelist pointer lives **outside** the object).
- `s->size` — final stride between objects, aligned to `s->align` and `ARCH_SLAB_MINALIGN`.
- `s->offset` — where in each object to store the freelist pointer (`0` = at start — unsafe for caller's data; chosen carefully).
- `s->oo` and `s->min` — `kmem_cache_order_objects` packed values: high order (preferred) and minimum order that fits at least one object.

Order selection ([`mm/slub.c:calculate_order`](https://elixir.bootlin.com/linux/v6.6/source/mm/slub.c#L4719)) tries to fit between `slub_min_objects` and `slub_max_order` objects per slab, balancing internal fragmentation vs. allocator overhead.

### 4.2 `init_kmem_cache_nodes()`

Allocates one `struct kmem_cache_node` per NUMA node ([`mm/slub.c:init_kmem_cache_nodes`](https://elixir.bootlin.com/linux/v6.6/source/mm/slub.c#L4961)). Each holds:

- `n->list_lock` — spinlock.
- `n->partial` — list of partially-used slabs.
- `n->nr_partial` — count.
- (debug) `n->full` — list of full slabs.

### 4.3 `alloc_kmem_cache_cpus()`

Allocates per-CPU `struct kmem_cache_cpu` via `__alloc_percpu` ([`mm/slub.c:alloc_kmem_cache_cpus`](https://elixir.bootlin.com/linux/v6.6/source/mm/slub.c#L4878)). Each per-CPU struct:

```c
struct kmem_cache_cpu {
    void   **freelist;     /* head of lockless freelist */
    unsigned long tid;     /* monotonic, used in cmpxchg_double */
    struct slab *slab;     /* current active slab */
    struct slab *partial;  /* per-CPU partial list head (CONFIG_SLUB_CPU_PARTIAL) */
};
```

### 4.4 `sysfs_slab_add()`

If `slab_state >= FULL`, exposes `/sys/kernel/slab/<name>/` with tunables and stats. During early boot, sysfs is deferred until `slab_sysfs_init()` later.

---

## 5. The `struct kmem_cache` walk

[`include/linux/slub_def.h`](https://elixir.bootlin.com/linux/v6.6/source/include/linux/slub_def.h)

```c
struct kmem_cache {
    struct kmem_cache_cpu __percpu *cpu_slab;
    slab_flags_t          flags;
    unsigned long         min_partial;
    unsigned int          size;         /* stride */
    unsigned int          object_size;
    unsigned int          offset;       /* freelist pointer offset */
    unsigned int          cpu_partial;  /* objects to keep on cpu partial chain */
    struct kmem_cache_order_objects oo;
    struct kmem_cache_order_objects min;
    gfp_t                 allocflags;
    int                   refcount;
    void (*ctor)(void *);
    unsigned int          inuse;
    unsigned int          align;
    unsigned int          red_left_pad;
    const char           *name;
    struct list_head      list;         /* in slab_caches */
    struct kmem_cache_node *node[MAX_NUMNODES];
    /* + randomization, usercopy region, kasan, debug fields */
};
```

---

## 6. Slab layout

For a cache with `size=128`, `oo.order=1` (2 pages = 8 KB), each slab holds `(8192 - sizeof(struct slab metadata)) / 128 ≈ 63` objects. SLUB stores the freelist pointer **inside** the object at `s->offset` (default: start of object). With `SLAB_FREELIST_HARDENED`, the pointer is XORed with `s->random ^ &freeptr`.

```
  +-------------------------------+
  | obj 0: [freeptr | data ...]   |  freeptr -> obj 4
  | obj 1: [freeptr | data ...]   |  freeptr -> obj 7
  | obj 2: in-use (data only)     |
  | obj 3: [freeptr | data ...]   |  freeptr -> obj 1
  | ...                           |
  +-------------------------------+
  The active per-CPU freelist head points to obj 3.
```

---

## 7. Boot bootstrap

The absolute first caches — `kmem_cache` (cache of caches) and `kmem_cache_node` (cache of node descriptors) — cannot use `kmem_cache_alloc` because there's no slab system yet. They are bootstrapped in [`mm/slub.c:kmem_cache_init`](https://elixir.bootlin.com/linux/v6.6/source/mm/slub.c#L5285) using static `boot_kmem_cache` and `boot_kmem_cache_node` structs, then later copied via `bootstrap()` into proper slab-allocated descriptors.

---

## 8. Hardening hooks

| Option                       | Effect                                                                  |
|------------------------------|-------------------------------------------------------------------------|
| `CONFIG_SLAB_FREELIST_HARDENED` | XOR-encode freelist pointers; attacker corrupting an object cannot easily redirect the next alloc. |
| `CONFIG_SLAB_FREELIST_RANDOM`| Randomize the order in which objects are placed on the freelist at slab creation. |
| `CONFIG_HARDENED_USERCOPY`   | `useroffset`/`usersize` define a permitted user-copy window in each object; `copy_from_user`/`copy_to_user` validate against it. |
| `CONFIG_KASAN`               | Per-object shadow; redzones between `object_size` and `size`.            |
| `CONFIG_KFENCE`              | Probabilistic guard-page allocator; intercepts a fraction of allocations. |

---

## 9. Source-walk cheat sheet

| File                | Function                            | Role                                  |
|---------------------|-------------------------------------|---------------------------------------|
| `mm/slab_common.c`  | `kmem_cache_create()`               | Entry (inline)                         |
| `mm/slab_common.c`  | `__kmem_cache_create_args()`        | Merge-or-create dispatcher             |
| `mm/slub.c`         | `__kmem_cache_alias()`              | Cache-merging search                   |
| `mm/slab_common.c`  | `create_cache()`                    | Allocate descriptor + bootstrap link   |
| `mm/slub.c`         | `__kmem_cache_create()`             | SLUB-side setup                        |
| `mm/slub.c`         | `calculate_sizes()` / `calculate_order()` | Geometry                         |
| `mm/slub.c`         | `init_kmem_cache_nodes()`           | Per-node state                         |
| `mm/slub.c`         | `alloc_kmem_cache_cpus()`           | Per-CPU state                          |
| `mm/slub.c`         | `bootstrap()` / `kmem_cache_init()` | Boot chicken-and-egg                   |
| `mm/slub.c`         | `sysfs_slab_add()`                  | `/sys/kernel/slab/<name>/`             |
