# kmalloc — Interview Q&A (Nvidia / Google / Qualcomm)

> Tags reflect the *typical* focus area of each company's kernel teams:
> **[Nvidia]** = GPU drivers, DMA coherency, large contiguous buffers.
> **[Google]** = Android GKI, scalability, memcg, scheduler integration.
> **[Qualcomm]** = SoC bring-up, power, IOMMU/SMMU, atomic context, BSP.
> Many questions are relevant to all three; the tag highlights the angle.

---

### Q1. `kmalloc` vs `vmalloc` — give a one-paragraph answer and three concrete cases where you'd choose one over the other.  `[Nvidia]` `[Google]` `[Qualcomm]`

**Answer.** `kmalloc` returns a buffer in the **kernel linear map**, backed by **physically contiguous** pages from the buddy allocator (via SLUB). It is fast (per-CPU lockless fast path), supports small sizes (≤ `KMALLOC_MAX_SIZE` ≈ 4 MB), and the pointer satisfies `virt_to_phys()`. `vmalloc` allocates from the **vmalloc area** with **virtually contiguous** but possibly **physically discontiguous** pages, populated into kernel PTEs on demand; it survives fragmentation but costs page-table updates, a TLB flush window, and the pointer is **not** valid for DMA without `vmalloc_to_page()`.

| Pick `kmalloc` when | Pick `vmalloc` when |
|---------------------|---------------------|
| You need DMA / `virt_to_phys` | You need a multi-MB virt-contig buffer and don't need phys contiguity |
| You're on a hot path and want O(1) alloc | You're in init/probe and rare alloc is OK |
| Size is small (a few KB) | Size is large (hundreds of KB to many MB) under fragmented memory |

---

### Q2. Why is `kmalloc` memory physically contiguous?  `[Nvidia]`

Because every `kmalloc` bucket is a SLUB cache whose **slab is one (or `2^order`) contiguous physical pages** obtained from the buddy allocator. The pointer it returns is just an offset inside that contiguous slab → contiguous in physical RAM too. This matters for devices that do DMA without an IOMMU and require contiguous physical buffers (legacy or where IOMMU bypass is enabled).

---

### Q3. What's the maximum size you can request from `kmalloc`?  `[Google]` `[Qualcomm]`

`KMALLOC_MAX_SIZE = 1 << (MAX_ORDER + PAGE_SHIFT - 1)`. On a 6.6 arm64 defconfig with `MAX_ORDER=10`, `PAGE_SHIFT=12` → `1 << 21 = 2 MB`. (Some configs raise `MAX_ORDER` to 11, giving 4 MB.) Above `KMALLOC_MAX_CACHE_SIZE` (8 KB), `__kmalloc_large_node()` bypasses SLUB and calls `alloc_pages` directly. A request larger than the limit triggers a `WARN_ON_ONCE` and returns `NULL`.

---

### Q4. Difference between `GFP_KERNEL` and `GFP_ATOMIC`. When does each one fail?  `[Qualcomm]`

`GFP_KERNEL = __GFP_RECLAIM | __GFP_IO | __GFP_FS` → may sleep, may invoke direct reclaim and even the OOM killer. Allowed only in process context with no spinlock held.

`GFP_ATOMIC = __GFP_HIGH | __GFP_KSWAPD_RECLAIM` → never sleeps, taps the emergency reserve (`min_free_kbytes` watermark + `ALLOC_HIGH`). Used from IRQ/softirq/spinlock. **Failure modes:**

- `GFP_KERNEL` rarely returns `NULL` for small sizes — it'll reclaim or OOM-kill first.
- `GFP_ATOMIC` returns `NULL` quickly under memory pressure; callers must handle it.

If you ever do `kmalloc(…, GFP_KERNEL)` while holding a spinlock, lockdep will splat: `BUG: sleeping function called from invalid context`.

---

### Q5. Walk me through the SLUB fast path. Why is it lockless?  `[Google]` `[Nvidia]`

```c
c   = raw_cpu_ptr(s->cpu_slab);
tid = c->tid;                       /* per-CPU transaction id */
obj = c->freelist;
if (!obj || !node_match(...)) goto slow;
next = *(void**)(obj + s->offset);  /* freelist link inside object */
if (!this_cpu_cmpxchg_double(&c->freelist, &c->tid,
                              obj, tid, next, tid+1)) goto again;
return obj;
```

It's lockless because the only atomic operation is a `cmpxchg_double` on the per-CPU `(freelist, tid)` pair. On ARM64 with LSE this becomes a single `CASPAL` instruction. The `tid` (transaction id) catches the case where preemption migrated us to another CPU between the load and the CAS — the CAS will fail on the new CPU's mismatching `tid` and we just retry. No spinlock, no IRQ disable, no IPI.

---

### Q6. Why does SLUB store the freelist pointer *inside* the free object? Isn't that a security risk?  `[Google]`

Storing the next-free pointer inside the free object means **no metadata overhead per object**, which is why SLUB has excellent memory density. The risk (free-list corruption via UAF/heap overflow) is mitigated by `CONFIG_SLAB_FREELIST_HARDENED`: each pointer is XORed with `s->random ^ (unsigned long)ptr` in `freelist_ptr()` ([`mm/slub.c`](https://elixir.bootlin.com/linux/v6.6/source/mm/slub.c#L339)). An attacker who corrupts the freelist no longer controls the next allocation address directly. `CONFIG_SLAB_FREELIST_RANDOM` additionally randomizes the initial order so consecutive `kmalloc`s of the same size don't return adjacent objects.

---

### Q7. On ARM64, what synchronization primitive does SLUB use for the fast path, and which instruction does it become?  `[Qualcomm]`

`this_cpu_cmpxchg_double()`. On ARMv8.1+ with `ARM64_HAS_LSE_ATOMICS`, alternatives patching swaps in `CASP`/`CASPA`/`CASPAL` (the 16-byte CAS pair). On older v8.0 cores it falls back to an `LDXP`/`STXP` loop using the local exclusive monitor. Both operate on a 16-byte aligned pair `(freelist, tid)` — `struct kmem_cache_cpu` is laid out to guarantee that alignment.

---

### Q8. A driver calls `kmalloc(4096, GFP_KERNEL)` and does DMA from a device. Two bugs are possible — what are they?  `[Nvidia]` `[Qualcomm]`

1. **Cache coherency / alignment** — on a non-coherent ARM64 platform, the buffer is allocated as **Normal cacheable** memory. The CPU writes sit in L1/L2 until flushed, so the device reads stale RAM. Even with `dma_map_single()` doing CPU cache maintenance, the buffer must be at least `ARCH_DMA_MINALIGN` (often 128 B on arm64) to avoid sharing a cache line with unrelated data. A 4 KB allocation is page-aligned and safe in size, but only if `ARCH_KMALLOC_MINALIGN >= ARCH_DMA_MINALIGN` — which is **not** universally true in 6.6 (since 5.19 they can differ). Safer to use `dma_alloc_coherent` or `kmalloc(ARCH_DMA_MINALIGN-aligned)` after checking `dma_kmalloc_needs_bounce()`.
2. **DMA mask not honored** — if the device has a 32-bit DMA mask, the kmalloc with plain `GFP_KERNEL` may return a buffer above 4 GB → DMA programming truncates the address and silently corrupts memory. Must use `GFP_DMA32` (or rely on the DMA API + SMMU/IOMMU + IOVA, which is what `dma_alloc_coherent` does).

---

### Q9. What happens on `kfree(NULL)`?  `[Google]`

It's an explicit no-op:

```c
/* mm/slub.c:kfree */
if (unlikely(ZERO_OR_NULL_PTR(x))) return;
```

So freeing `NULL` is safe and idiomatic in error paths. Similarly `ZERO_SIZE_PTR` (returned for `kmalloc(0)`) is recognized.

---

### Q10. `kmalloc(0, …)` — what's returned and why?  `[Google]`

It returns `ZERO_SIZE_PTR` = `((void *)16)` — a non-NULL but non-dereferenceable sentinel. This lets callers distinguish "alloc failed" (NULL) from "alloc succeeded with zero bytes". `kfree(ZERO_SIZE_PTR)` is a no-op. See [`include/linux/slab.h`](https://elixir.bootlin.com/linux/v6.6/source/include/linux/slab.h#L83) and the `ZERO_OR_NULL_PTR` macro.

---

### Q11. What's `__GFP_ACCOUNT` and the `kmalloc-cg-N` caches?  `[Google]`

`__GFP_ACCOUNT` tells SLUB to charge the allocation to the current task's memory cgroup (`memcg`). To avoid mixing accounted and non-accounted objects in the same slab (which would complicate per-slab page accounting), SLUB maintains a parallel set of `kmalloc-cg-<size>` caches. Selection happens in `kmalloc_type(flags)`. Android (GKI) and Google's container infrastructure rely on this to attribute kernel memory back to user-visible cgroups.

---

### Q12. Two CPUs on a 64-core ARM64 server are hammering `kmalloc(128, GFP_KERNEL)`. Where is the contention?  `[Nvidia]` `[Google]`

There is **none on the fast path** — each CPU has its own `kmem_cache_cpu`. Contention shows up only when:

- A CPU exhausts its `cpu_slab` *and* its `c->partial` chain → must grab `n->list_lock` on `kmem_cache_node[nid]`.
- Or buddy itself is contended (`zone->lock`) when SLUB asks for a fresh slab.

To diagnose: `perf lock`, `perf c2c`, `/sys/kernel/slab/kmalloc-128/cpu_slabs` (per-CPU active count), and watch `ALLOC_SLOWPATH` counters in `/sys/kernel/slab/kmalloc-128/alloc_slowpath`.

---

### Q13. Walk through what happens after a `kmalloc` slab becomes fully empty.  `[Qualcomm]`

When the last object in a slab is freed via `__slab_free()`, SLUB:

1. Atomically updates `slab->counters` (decrement `inuse`).
2. If the slab is on `n->partial` and `n->nr_partial > s->min_partial`, calls `discard_slab()`.
3. `discard_slab()` → `free_slab()` → `__free_slab()` → `__free_pages(folio_page(folio, 0), order)`.
4. Buddy returns the pages to `zone->free_area[order]`. The linear-map mapping stays — only the page bookkeeping changes.

Result: physical pages return to the free pool. The kernel VA range remains validly mapped (it always is for the linear map).

---

### Q14. How does KFENCE interact with `kmalloc`?  `[Google]`

KFENCE intercepts a sampled subset (1-in-`CONFIG_KFENCE_SAMPLE_INTERVAL`) of `kmalloc` calls and serves them from a small pool of **guarded pages**: every allocation gets its own page, with surrounding pages marked unmapped (`set_memory_valid(.., 0)` on arm64). Buffer overflows immediately fault → translation fault handler routes to `kfence_handle_page_fault()` which prints a UAF/OOB report. Cost is bounded (a fixed pool), unlike KASAN's per-byte shadow.

---

### Q15. You see this in dmesg: `kmalloc: Trying to free already-free object … in kmalloc-256`. What does it mean and how do you debug?  `[Qualcomm]`

Classic **double-free**. SLUB detected that the object being freed was already on the freelist (via `on_freelist()` check when `slub_debug` is enabled). Debug:

1. Boot with `slub_debug=FZPU,kmalloc-256` to enable Free-list, Zap, Poison, User tracking for that specific bucket.
2. Reproduce — SLUB now stores `alloc`/`free` call stacks per object; the splat will print both the first and the duplicate free's backtrace.
3. Or use KASAN (`CONFIG_KASAN_GENERIC`) which catches double-free immediately via shadow memory.
4. On production kernels without debug, capture `dmesg` + `/proc/slabinfo` and run with `slub_debug=-` to disable merging (`CONFIG_SLAB_MERGE_DEFAULT=n`) to isolate the cache.

---

### Q16. Why is `kmalloc` not allowed from NMI context?  `[Nvidia]`

NMIs can preempt code that holds the SLUB fast-path's per-CPU state (e.g., halfway through the `cmpxchg_double` retry loop, or holding `n->list_lock` in the slow path). Re-entering SLUB from an NMI could observe an inconsistent `tid`/`freelist` pair, or worse, recursively try to take `n->list_lock` and deadlock. The kernel offers NMI-safe alternatives like `nmi_panic_self_stop()`-style local buffers or pre-allocated emergency pools. For perf events, the ring buffer is pre-allocated explicitly to avoid `kmalloc` in NMI.

---

### Q17. Bonus, staff-level: how would you reduce p99 `kmalloc` latency on a 128-core ARM64 server running a memcached-like workload?  `[Google]` `[Nvidia]`

Levers in order of impact:

1. **Pin caches to NUMA nodes** — ensure objects stay on the local node; check `/sys/kernel/slab/<cache>/remote_node_defrag_ratio`.
2. **Tune `cpu_partial_slabs`** — increase via `/sys/kernel/slab/<cache>/cpu_partial` to absorb bursts without hitting `n->list_lock`.
3. **Switch hot sizes to a dedicated `kmem_cache_create()`** with `SLAB_HWCACHE_ALIGN | SLAB_ACCOUNT` and a sensible `align` — prevents merging with unrelated caches (`SLAB_MERGE`) so misses don't pollute your hot slabs.
4. **Lower `MIGRATE_UNMOVABLE` fragmentation** — boot with `defrag_mode=1` / use `kcompactd` tuning so high-order slab pages are easier to obtain → fewer slow paths.
5. **Audit `GFP_ATOMIC` usage** — atomic allocations drain the emergency reserve; convert to deferred work + `GFP_KERNEL` where possible.
6. **Enable LSE atomics** — `CONFIG_ARM64_LSE_ATOMICS=y` so the fast path uses `CASPAL` instead of `LDXP/STXP` retry loops; on heavily contended slow paths the difference is real.
7. **Watch PSI** — `/proc/pressure/memory` `some avg10` rising correlates with slab slow paths; pair with `bpftrace` on `___slab_alloc` to confirm.

---

## Common pitfalls (quick reference)

| Pitfall                                                    | Fix |
|------------------------------------------------------------|-----|
| `GFP_KERNEL` under spinlock                                | Switch to `GFP_ATOMIC`, or drop lock and retry. |
| Assuming `kmalloc` zero-fills                              | Use `kzalloc` or pass `__GFP_ZERO`. |
| Mixing accounted/non-accounted objects in one cache        | Use `__GFP_ACCOUNT` consistently or a dedicated `kmem_cache` with `SLAB_ACCOUNT`. |
| `kmalloc` for DMA on non-coherent ARM64                    | Use `dma_alloc_coherent` or `dma_alloc_noncoherent` + explicit sync. |
| Allocating `>` KMALLOC_MAX_SIZE                            | Use `vmalloc` (virt-contig) or `alloc_pages_exact`. |
| `kfree` on a `vmalloc` pointer (or vice versa)             | Always match the alloc API; SLUB/KASAN will warn loudly. |
| Pointer arithmetic past `ksize(p)` boundary                | Use `ksize(p)` to query the *actual* allocated size; SLUB rounds up. |

---

## Further reading

- [`Documentation/core-api/memory-allocation.rst`](https://elixir.bootlin.com/linux/v6.6/source/Documentation/core-api/memory-allocation.rst)
- [`Documentation/mm/slub.rst`](https://elixir.bootlin.com/linux/v6.6/source/Documentation/mm/slub.rst)
- [`Documentation/arch/arm64/memory.rst`](https://elixir.bootlin.com/linux/v6.6/source/Documentation/arch/arm64/memory.rst)
- LWN: "The SLUB allocator" (Christoph Lameter)
- LPC talks on hardened SLUB / KFENCE
