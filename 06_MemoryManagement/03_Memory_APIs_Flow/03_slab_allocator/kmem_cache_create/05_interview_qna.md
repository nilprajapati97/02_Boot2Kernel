# kmem_cache_create — Interview Q&A (Nvidia / Google / Qualcomm)

---

### Q1. When should you use `kmem_cache_create` instead of `kmalloc`?  `[Google]` `[Nvidia]`

When you need any of: a constructor, `SLAB_TYPESAFE_BY_RCU`, exact-size packing (no rounding to a kmalloc bucket), named visibility in `/proc/slabinfo` for tracking, the ability to call `kmem_cache_shrink()`, or `SLAB_NO_MERGE` to avoid sharing freelist with unrelated allocations (security/isolation). Typical examples: `inode_cache`, `dentry`, `task_struct`, `mm_struct`, driver-specific request structures.

---

### Q2. What is cache merging and why is it on by default?  `[Qualcomm]`

SLUB scans existing caches at `kmem_cache_create` time and, if it finds one with compatible (size, align, flags) and neither cache has a constructor or `SLAB_TYPESAFE_BY_RCU`, it returns an **alias** to the existing cache (refcount++). This reduces per-cache metadata, per-CPU state, and TLB pressure on slab pages. Drawback: an attacker abusing one merged cache may probe state for another; opt out via `SLAB_NO_MERGE` or boot arg `slab_nomerge`.

---

### Q3. Walk through the bootstrap chicken-and-egg problem.  `[Google]`

The slab allocator needs `struct kmem_cache` and `struct kmem_cache_node` allocations to function, but allocating them requires the slab allocator. Solution in `kmem_cache_init()` ([`mm/slub.c`](https://elixir.bootlin.com/linux/v6.6/source/mm/slub.c#L5285)):

1. Statically declare `boot_kmem_cache` and `boot_kmem_cache_node` (as `static struct kmem_cache`).
2. Hand-initialize them via `create_boot_cache()` — directly write fields, allocate slab pages.
3. Once functional, call `bootstrap()` to allocate proper descriptors from the now-working cache-of-caches and copy the boot structs into them.
4. Patch up cross-references in `slab_caches` list.

---

### Q4. Explain `SLAB_TYPESAFE_BY_RCU` and when you need it.  `[Nvidia]` `[Google]`

With this flag, the cache **never returns an object to the buddy allocator while RCU readers might still hold pointers**. Freed objects go on the slab freelist and may be re-allocated to a **new use** — but always with the same **type** and layout. RCU readers must therefore re-validate (e.g., check a generation counter) but can be sure that field offsets and types are unchanged. Used for `vm_area_struct`, `kmem_cache`, network sockets — anywhere lockless lookups race with frees. Cache merging is disabled for such caches (because the "type" guarantee would be violated).

---

### Q5. Why does `SLAB_HWCACHE_ALIGN` matter on ARM64?  `[Qualcomm]`

It rounds `s->align` up to `cache_line_size()` (typically 64 B on most ARM cores, 128 B on Apple M). Prevents false sharing: two objects from the same cache that happen to be touched by different CPUs will land on different cache lines, avoiding ping-ponging via the coherency protocol. Critical for per-CPU counters, lock-protected structures, and any contended hot data.

---

### Q6. A driver creates a cache with size=144. Why might `s->size` end up as 192?  `[Google]`

If `CONFIG_SLAB_FREELIST_HARDENED` or debug flags force the freelist pointer **outside** the object, and the alignment is rounded up, the effective stride may grow. Without those, SLUB chooses the smallest stride that fits 144 bytes + needed metadata aligned to `s->align` (default 8 on arm64 = `ARCH_KMALLOC_MINALIGN`). With `SLAB_HWCACHE_ALIGN`, it would jump to 192 (multiple of 64). Inspect via `/sys/kernel/slab/<name>/slab_size`.

---

### Q7. Why does `kmem_cache_create` take `slab_mutex` for the whole creation?  `[Qualcomm]`

Several globally-shared resources must be inspected and mutated atomically: the `slab_caches` list (for merging), the sysfs hierarchy, the cache descriptor allocation. A single mutex is simple and uncontended (cache creation is rare). The mutex also serializes against `kmem_cache_destroy`, which walks the same list. Performance is not a concern since this path runs once per cache lifetime.

---

### Q8. What does `calculate_order()` actually compute?  `[Nvidia]`

It picks the **page allocation order** for each slab page, balancing internal fragmentation (waste at the tail of a slab) vs. allocator overhead (more frequent buddy calls for small orders). Heuristics: start from `slub_min_order` (usually 0), increase until `min_objects` objects fit while fragmentation is below ~1/16 of slab size, capped at `slub_max_order` (default 3, i.e., 8 pages = 32 KB). The chosen order is packed into `s->oo` with the object count; `s->min` holds the minimum order that can fit at least one object (the fallback).

---

### Q9. You create a cache with a `ctor`, then `kmem_cache_alloc` returns objects without re-running it. Why?  `[Google]`

The constructor runs **on slab creation** (when a new slab page is allocated), not on every `kmem_cache_alloc`. It initializes every object in the slab once. When an object is freed and later re-allocated, it carries the **last state the previous user left it in** — not the constructor's state. Thus constructors are only safe for *idempotent* initialization (e.g., embedded spinlocks/lists that the user re-uses without reinit). Modern kernel code mostly skips constructors and initializes after `kmem_cache_alloc` to avoid this confusion.

---

### Q10. What's `SLAB_RECLAIM_ACCOUNT` and why does it matter for OOM scoring?  `[Qualcomm]`

It accounts the cache's slab pages under `NR_SLAB_RECLAIMABLE_B` (vs. `NR_SLAB_UNRECLAIMABLE_B`) and contributes to `SReclaimable` in `/proc/meminfo`. The shrinker infrastructure can target reclaimable slabs under memory pressure. Mark a cache reclaimable when it holds **cacheable** state (e.g., dentries, inodes) that the kernel can drop and rebuild on demand. Don't mark it for caches whose contents are user-visible state (e.g., open file descriptors).

---

### Q11. Implement a per-driver cache with hardening: hardened freelist + no merge + usercopy region for offset 0..32.  `[Nvidia]` `[Google]`

```c
cache = kmem_cache_create_usercopy("my_req",
                                   sizeof(struct my_req),
                                   __alignof__(struct my_req),
                                   SLAB_HWCACHE_ALIGN |
                                   SLAB_RECLAIM_ACCOUNT |
                                   SLAB_NO_MERGE,
                                   0,            /* useroffset */
                                   32,           /* usersize  */
                                   NULL);
```

`SLAB_NO_MERGE` keeps this cache isolated; `_usercopy` variant restricts safe `copy_*_user` to bytes `[0..32)` of each object — `__check_heap_object` enforces it. Combine with `CONFIG_SLAB_FREELIST_HARDENED=y` (build-time) for XOR-encoded freelist pointers.

---

### Q12. What happens if two modules call `kmem_cache_create("my_name", ...)` with identical name?  `[Qualcomm]`

SLUB allows it but emits `WARN()` from `kmem_cache_sanity_check()` because the name is supposed to be globally unique (used for sysfs). The second create will fail to add a sysfs entry (kobject name collision). Use cache-merging-friendly names or namespace with module prefix.

---

### Q13. NUMA: how does `kmem_cache_create` handle per-node state?  `[Nvidia]`

`init_kmem_cache_nodes()` allocates one `struct kmem_cache_node` per online NUMA node, each on its node's memory (via `kmem_cache_alloc_node` on the `kmem_cache_node` cache). At alloc time, the per-CPU fast path uses local node memory; the slow path consults `n->partial` for the CPU's preferred node first and falls back to remote nodes. On arm64 servers with multiple memory controllers / chiplets (Ampere Altra, Grace, AmpereOne), this directly affects allocation latency.

---

### Q14. KFENCE interaction.  `[Google]`

KFENCE ([`mm/kfence/`](https://elixir.bootlin.com/linux/v6.6/source/mm/kfence/)) probabilistically diverts a small fraction of allocations from any slab cache to its own pool of guard-paged objects. `kmem_cache_create` doesn't need special opt-in — KFENCE registers a hook in `slab_alloc_node` that intercepts based on a per-CPU sample interval. Caches can opt out with `SLAB_SKIP_KFENCE`. Overhead is < 1% with `kfence.sample_interval=100` (ms).

---

### Q15. Staff-level: design choices when adding a new cache for a high-rate path (e.g., 1M alloc/sec).  `[Nvidia]` `[Google]`

Design considerations:

1. **Size**: keep `<= 256 B` so multiple per-CPU partial slabs fit comfortably.
2. **Alignment**: `SLAB_HWCACHE_ALIGN` to prevent false sharing between concurrent producers.
3. **`SLAB_NO_MERGE`**: avoid cross-cache contention; isolates hot path from unrelated callers.
4. **`cpu_partial`**: bump via `/sys/kernel/slab/<name>/cpu_partial` to absorb bursts without slow-path entry.
5. **Constructor**: avoid — explicit init lets compiler keep fields in registers.
6. **Memcg**: opt out (`!SLAB_ACCOUNT`) if not user-attributable; saves the objcg dereference on every alloc.
7. **NUMA**: callers should use `kmem_cache_alloc_node(s, gfp, numa_node_id())` on multi-socket arm64 to stay local.
8. **Telemetry**: enable `CONFIG_SLUB_STATS=y` only for benchmarking; in production rely on `/proc/slabinfo` and bpftrace.

---

## Common pitfalls

| Pitfall                                                | Fix |
|--------------------------------------------------------|-----|
| Calling from atomic context                            | Initialize at module load only. |
| Reusing a name already in `/sys/kernel/slab/`          | Pick a unique, namespaced name. |
| Relying on the constructor to re-zero on each alloc   | Use `kmem_cache_zalloc` or `memset` after alloc. |
| Forgetting `kmem_cache_destroy` in error paths         | Use devm/managed wrappers where possible. |
| Using `kfree` on objects from a custom cache           | Use `kmem_cache_free(s, p)`. |
