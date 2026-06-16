# kmalloc — Internals (SLUB on Linux 6.6)

> All source paths and line numbers below are pinned to **Linux 6.6 LTS** on
> [elixir.bootlin.com/linux/v6.6](https://elixir.bootlin.com/linux/v6.6/source).
> Architecture assumed: **ARM64**, 4 KB pages, `CONFIG_SLUB=y`,
> `CONFIG_SLUB_CPU_PARTIAL=y`, `CONFIG_NUMA` may or may not be set.

---

## 1. Subsystem map

```
                    ┌────────────────────┐
   caller ────►     │     kmalloc()      │  inline, picks bucket
                    └─────────┬──────────┘
                              ▼
                  ┌─────────────────────────┐
                  │  kmem_cache_alloc_trace │  generic SLUB entry
                  └─────────┬───────────────┘
                            ▼
                ┌───────────────────────────┐  fast path:
                │  slab_alloc_node()        │  lockless, per-CPU
                └─────┬─────────────┬───────┘
                fast  │             │ slow (cpu freelist empty)
                      ▼             ▼
              ┌───────────┐   ┌──────────────────┐
              │c->freelist│   │  ___slab_alloc() │  slow path
              └───────────┘   └─────────┬────────┘
                                        ▼
                               ┌──────────────────┐
                               │   new_slab()     │  no partial slabs
                               └─────────┬────────┘
                                         ▼
                               ┌──────────────────┐
                               │ alloc_slab_page()│
                               └─────────┬────────┘
                                         ▼
                               ┌──────────────────┐
                               │ __alloc_pages()  │  buddy allocator
                               └──────────────────┘
```

---

## 2. Key data structures (field-level)

### 2.1 `struct kmem_cache` — the cache descriptor

[`include/linux/slub_def.h`](https://elixir.bootlin.com/linux/v6.6/source/include/linux/slub_def.h#L88)

```c
struct kmem_cache {
    struct kmem_cache_cpu __percpu *cpu_slab; /* per-CPU fast path */
    slab_flags_t        flags;
    unsigned long       min_partial;     /* min slabs to keep on node */
    unsigned int        size;            /* object size + metadata (red zone, etc.) */
    unsigned int        object_size;     /* user-visible object size */
    struct reciprocal_value reciprocal_size;
    unsigned int        offset;          /* freelist pointer offset within object */
#ifdef CONFIG_SLUB_CPU_PARTIAL
    unsigned int        cpu_partial;     /* objects in per-cpu partial slabs */
    unsigned int        cpu_partial_slabs;
#endif
    struct kmem_cache_order_objects oo;  /* (order << 16) | objects-per-slab */
    struct kmem_cache_order_objects min;
    gfp_t               allocflags;
    int                 refcount;
    void                (*ctor)(void *);
    unsigned int        inuse;
    unsigned int        align;
    unsigned int        red_left_pad;
    const char         *name;
    struct list_head    list;
    struct kmem_cache_node *node[MAX_NUMNODES];
};
```

### 2.2 `struct kmem_cache_cpu` — per-CPU freelist (the fast path)

[`include/linux/slub_def.h`](https://elixir.bootlin.com/linux/v6.6/source/include/linux/slub_def.h#L48)

```c
struct kmem_cache_cpu {
    void          **freelist;   /* head of per-cpu free list */
    unsigned long   tid;        /* transaction id (cmpxchg_double seqno) */
    struct slab    *slab;       /* current slab from which we allocate */
#ifdef CONFIG_SLUB_CPU_PARTIAL
    struct slab    *partial;    /* per-cpu partial slab list head */
#endif
};
```

`tid` is updated on every fast-path operation; it lets `this_cpu_cmpxchg_double()` detect that we were preempted to another CPU and retry without locking.

### 2.3 `struct slab` — the page-sized container (was `struct page` overlay pre-5.17)

[`mm/slab.h`](https://elixir.bootlin.com/linux/v6.6/source/mm/slab.h#L11)

```c
struct slab {
    unsigned long      __page_flags;
    struct kmem_cache *slab_cache;
    union {
        struct { struct list_head slab_list;        /* node->partial / full */
                 union { struct slab *next;          /* cpu partial */
                         int           slabs; }; };
        struct rcu_head rcu_head;                    /* SLAB_TYPESAFE_BY_RCU */
    };
    void              *freelist;     /* in-slab free list head */
    union { unsigned long counters;
            struct { unsigned inuse:16, objects:15, frozen:1; }; };
    unsigned int       __unused;
    atomic_t           __page_refcount;
};
```

`frozen=1` ⇒ slab is "owned" by a specific CPU (currently in `c->slab`), so no node-list lock is needed.

### 2.4 `kmalloc_caches[type][idx]` — the bucket table

[`mm/slab_common.c`](https://elixir.bootlin.com/linux/v6.6/source/mm/slab_common.c#L675)

```c
struct kmem_cache *
kmalloc_caches[NR_KMALLOC_TYPES][KMALLOC_SHIFT_HIGH + 1] __ro_after_init;
```

Indexed by `(type, fls(size-1))`. Populated during `create_kmalloc_caches()` at boot ([`mm/slab_common.c:907`](https://elixir.bootlin.com/linux/v6.6/source/mm/slab_common.c#L907)).

---

## 3. Allocation algorithm

### 3.1 Top-level dispatch — `__kmalloc`

[`mm/slub.c:__kmalloc`](https://elixir.bootlin.com/linux/v6.6/source/mm/slub.c#L4404)

```c
void *__kmalloc(size_t size, gfp_t flags)
{
    return __do_kmalloc_node(size, flags, NUMA_NO_NODE, _RET_IP_);
}
```

`__do_kmalloc_node()` picks the right cache or falls through to the page allocator if `size > KMALLOC_MAX_CACHE_SIZE` (8 KB):

```c
if (unlikely(size > KMALLOC_MAX_CACHE_SIZE))
    return __kmalloc_large_node(size, flags, node);

s = kmalloc_slab(size, flags, caller);      /* picks kmalloc_caches[type][idx] */
ret = slab_alloc_node(s, NULL, flags, node, caller, size);
```

### 3.2 Fast path — `slab_alloc_node()` → `__slab_alloc_node()`

[`mm/slub.c:slab_alloc_node`](https://elixir.bootlin.com/linux/v6.6/source/mm/slub.c#L3458)

Pseudocode of the hot path:

```c
again:
    c   = raw_cpu_ptr(s->cpu_slab);
    tid = READ_ONCE(c->tid);
    barrier();

    object = c->freelist;
    slab   = c->slab;

    if (unlikely(!object || !node_match(slab, node))) {
        object = __slab_alloc(s, flags, node, addr, c);   /* SLOW PATH */
        goto out;
    }

    next = get_freepointer_safe(s, object);
    if (!this_cpu_cmpxchg_double(s->cpu_slab->freelist, s->cpu_slab->tid,
                                 object, tid,
                                 next,   next_tid(tid)))
        goto again;                              /* preempted / migrated */

    prefetch_freepointer(s, next);
    stat(s, ALLOC_FASTPATH);
out:
    maybe_wipe_obj_freeptr(s, object);
    if (flags & __GFP_ZERO) memset(object, 0, s->object_size);
    return object;
```

Properties of the fast path:

- **No locks**, no atomic RMWs except a single `cmpxchg_double` on `(freelist, tid)`.
- Survives migration: if we move CPUs mid-allocation, `tid` mismatch makes cmpxchg fail and we retry on the new CPU.
- Inlined into many call sites for cache-locality.

### 3.3 Slow path — `___slab_alloc()`

[`mm/slub.c:___slab_alloc`](https://elixir.bootlin.com/linux/v6.6/source/mm/slub.c#L3186)

In order:

1. **Reload** `c->slab` and try `get_freelist()` — slabs can be refilled by remote frees via the slab's freelist (cmpxchg).
2. **CPU partial list** (`c->partial`): take the next slab, unfreeze the previous one.
3. **Node partial list** (`s->node[nid]->partial`): grab a partial slab under `n->list_lock`. Refill `c->partial` with up to `s->cpu_partial_slabs` slabs.
4. **`new_slab()`** ([`mm/slub.c:1959`](https://elixir.bootlin.com/linux/v6.6/source/mm/slub.c#L1959)): no slab anywhere — allocate one fresh.

### 3.4 Fresh slab — `new_slab()` → `allocate_slab()` → buddy

`allocate_slab()` calls `alloc_slab_page()` which calls `alloc_pages_node(node, alloc_gfp, oo_order(s->oo))`. The order is chosen at cache creation by `calculate_order()` ([`mm/slub.c:4108`](https://elixir.bootlin.com/linux/v6.6/source/mm/slub.c#L4108)) to balance internal fragmentation and per-allocation cost — typically order 0–3 for kmalloc caches.

Buddy returns `struct page *`, which is then turned into `struct slab *` by `folio_slab()` and threaded with the in-slab freelist by `setup_object()`.

---

## 4. Free path (preview)

`kfree()` mirrors the above:

1. `virt_to_folio()` → identify the slab (or large-kmalloc page).
2. **Fast path** (`do_slab_free`): if the object belongs to `c->slab`, push it onto `c->freelist` via `cmpxchg_double`.
3. **Slow path** (`__slab_free`): atomic update of slab counters, possibly move slab between node `partial`/`full` lists, or free it back to buddy when fully empty and `n->nr_partial > min_partial`.

Detailed walk lives in [`../kfree/02_internals.md`](../kfree/02_internals.md).

---

## 5. Locking & concurrency

| Path                       | Synchronization                                |
|----------------------------|------------------------------------------------|
| Per-CPU fast path          | `this_cpu_cmpxchg_double` (lockless)           |
| CPU partial list           | local, frozen slabs (no lock)                  |
| Node partial/full lists    | `spinlock_t n->list_lock`                      |
| Remote free into a slab    | `cmpxchg_double(slab->freelist, slab->counters)` |
| `kmem_cache_create/destroy`| `slab_mutex` (global)                          |

ARM64-specific: `cmpxchg_double` lowers to **`CASP`** (Compare-And-Swap-Pair) when `ARM64_HAS_LSE_ATOMICS` is detected at boot; otherwise to an `LDXP/STXP` pair. Both are single-copy atomic for a 16-byte aligned pair — see [`arch/arm64/include/asm/cmpxchg.h`](https://elixir.bootlin.com/linux/v6.6/source/arch/arm64/include/asm/cmpxchg.h).

---

## 6. NUMA behavior

- `kmalloc()` defaults to `NUMA_NO_NODE` → SLUB allocates on the **current** CPU's node.
- `kmalloc_node(size, flags, nid)` honors the node hint; `___slab_alloc` prefers `s->node[nid]->partial` first.
- On UMA ARM64 SoCs (typical mobile/automotive) `MAX_NUMNODES = 1` and the node-id checks compile away.

---

## 7. Per-CPU partial slabs (`CONFIG_SLUB_CPU_PARTIAL`)

To reduce node-lock contention, each CPU keeps a small chain of frozen partial slabs (`c->partial`). Tuned via `s->cpu_partial_slabs`, sized by `set_cpu_partial()` based on object size:

| Object size       | `cpu_partial` (approx. objects) |
|-------------------|---------------------------------|
| ≤ 256 B           | 30                              |
| ≤ 1 KB            | 30                              |
| ≤ 2 KB            | 13                              |
| ≤ 4 KB            | 6                               |
| larger            | 2                               |

---

## 8. Hardened / safety features (compile-time)

| Option                       | Effect on kmalloc                                            |
|------------------------------|--------------------------------------------------------------|
| `CONFIG_SLUB_DEBUG`          | Red zones, poisoning, owner tracking; slows fast path.       |
| `CONFIG_SLAB_FREELIST_HARDENED` | XOR-obfuscated free pointers — `freelist_ptr()` ([`mm/slub.c:339`](https://elixir.bootlin.com/linux/v6.6/source/mm/slub.c#L339)). |
| `CONFIG_SLAB_FREELIST_RANDOM`| Randomize new-slab object order. |
| `CONFIG_HARDENED_USERCOPY`   | `__check_heap_object()` validates `copy_{to,from}_user` ranges against slab metadata. |
| `CONFIG_KASAN`               | Quarantine on free, shadow-memory checks on every access.    |
| `CONFIG_KFENCE`              | Sample 1 in N kmallocs into a guarded page-pool for UAF/OOB detection. |
| `CONFIG_INIT_ON_ALLOC_DEFAULT_ON` | Implicit `__GFP_ZERO` on every kmalloc.                |

---

## 9. Boot-time cache creation

`kmem_cache_init()` ([`mm/slub.c:5057`](https://elixir.bootlin.com/linux/v6.6/source/mm/slub.c#L5057)) is called very early from `mm_core_init()` ([`init/main.c`](https://elixir.bootlin.com/linux/v6.6/source/init/main.c#L935)). It bootstraps the SLUB metadata caches in a special two-pass dance (`bootstrap()`), then `create_kmalloc_caches()` fills `kmalloc_caches[]`. After this completes, `slab_state == FULL` and `kmalloc` is fully usable.

Before this point, the kernel uses the `memblock` allocator — that's why very early init code uses `memblock_alloc()` instead of `kmalloc()`.

---

## 10. Interaction with reclaim & OOM

If a fast/slow-path allocation cannot find a slab, `new_slab()` calls `alloc_pages` with the caller's GFP. When `__GFP_DIRECT_RECLAIM` is set, the buddy allocator may invoke `try_to_free_pages()` ([`mm/vmscan.c`](https://elixir.bootlin.com/linux/v6.6/source/mm/vmscan.c)) — including shrinking the dcache/icache, swapping anonymous pages, etc. If reclaim fails and `__GFP_FS` is set, the OOM killer (`out_of_memory()` in [`mm/oom_kill.c`](https://elixir.bootlin.com/linux/v6.6/source/mm/oom_kill.c)) may run. PSI counters in `kernel/sched/psi.c` are bumped during these stalls.

---

## 11. Tracing & debugging

- **`trace_kmem_cache_alloc`** / `trace_kmem_cache_free` — see `/sys/kernel/tracing/events/kmem/`.
- `/proc/slabinfo` — per-cache stats.
- `/sys/kernel/slab/<cache>/` — sysfs knobs (`order`, `objs_per_slab`, `slabs`, `partial`, `cpu_slabs`).
- `slub_debug=FZPU,kmalloc-128` boot arg — enable debug for one bucket.
- `slabtop(1)` — top-like summary.

---

## 12. Source-walk cheat sheet (Linux 6.6)

| File                     | Function                          | Role                                    |
|--------------------------|-----------------------------------|-----------------------------------------|
| `include/linux/slab.h`   | `kmalloc()` (inline)              | Compile-time bucket pick                |
| `mm/slub.c`              | `__kmalloc()`                     | Runtime entry                           |
| `mm/slub.c`              | `slab_alloc_node()` / `__slab_alloc_node()` | Fast path                       |
| `mm/slub.c`              | `___slab_alloc()`                 | Slow path                               |
| `mm/slub.c`              | `new_slab()` → `allocate_slab()`  | Page acquisition                        |
| `mm/slab_common.c`       | `kmalloc_slab()`                  | Bucket lookup                           |
| `mm/slab_common.c`       | `create_kmalloc_caches()`         | Boot-time table fill                    |
| `mm/page_alloc.c`        | `__alloc_pages()`                 | Buddy allocator                         |
| `arch/arm64/mm/init.c`   | `arm64_memblock_init()`           | Sets up linear map limits               |

---

Next: [03_arm64_callflow.md](03_arm64_callflow.md) — full Mermaid call flow with ARM64 architectural details (TTBR1, ASID, DSB/ISB, CASP).
