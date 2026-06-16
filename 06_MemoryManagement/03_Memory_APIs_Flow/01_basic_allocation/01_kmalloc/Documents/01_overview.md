# kmalloc — Overview

> **Scope:** Linux **6.6 LTS** · Arch **ARM64 (AArch64, 4 KB pages, 48‑bit VA)**
> **Subsystem:** SLUB allocator (default since 2.6.23, only allocator in 6.6 after SLAB removal)
> **Header:** `<linux/slab.h>` · **Source:** [`mm/slub.c`](https://elixir.bootlin.com/linux/v6.6/source/mm/slub.c), [`include/linux/slab.h`](https://elixir.bootlin.com/linux/v6.6/source/include/linux/slab.h)

---

## 1. One‑line definition

`kmalloc()` returns a **physically contiguous**, kernel-virtual buffer of the requested size, drawn from a pre-sized SLUB cache (`kmalloc-8`, `kmalloc-16`, …, `kmalloc-8k`) and ultimately backed by pages from the buddy allocator.

---

## 2. Prototype

```c
/* include/linux/slab.h */
void *kmalloc(size_t size, gfp_t flags);
void *kmalloc_node(size_t size, gfp_t flags, int node);
void *kmalloc_array(size_t n, size_t size, gfp_t flags);    /* overflow-checked */
void *kcalloc(size_t n, size_t size, gfp_t flags);          /* + __GFP_ZERO    */
void  kfree(const void *objp);
```

In 6.6 `kmalloc()` is a `static __always_inline` wrapper in [`include/linux/slab.h`](https://elixir.bootlin.com/linux/v6.6/source/include/linux/slab.h#L580) that:

1. If `size` is a **compile-time constant** and `≤ KMALLOC_MAX_CACHE_SIZE`, picks the bucket at compile time via `kmalloc_index(size)` and tail-calls `kmem_cache_alloc_trace(kmalloc_caches[type][idx], …)`.
2. Otherwise tail-calls `__kmalloc(size, flags)` ([`mm/slub.c:__kmalloc`](https://elixir.bootlin.com/linux/v6.6/source/mm/slub.c#L4404)).

---

## 3. Parameters & return value

| Field    | Meaning |
|----------|---------|
| `size`   | Bytes requested. **Rounded up** to the nearest `kmalloc-N` bucket (next power of 2, with `-96` and `-192` extras). |
| `flags`  | GFP mask — controls context, zone, reclaim, zeroing, accounting. |
| **ret**  | Pointer to a buffer aligned to at least `ARCH_KMALLOC_MINALIGN` (8 on ARM64), or `NULL` on failure (unless `__GFP_NOFAIL`). |

Returned pointer lives in the **kernel linear (direct) map** — `virt_to_phys()` works on it, unlike `vmalloc()`.

---

## 4. GFP flags that matter for `kmalloc`

| Flag | Effect | Context |
|------|--------|---------|
| `GFP_KERNEL`   | May sleep, may reclaim, may invoke OOM. **Default for process context.** | Process only |
| `GFP_ATOMIC`   | Never sleeps, dips into emergency pools (`ALLOC_HIGH`). Smaller success rate. | IRQ, softirq, spinlock held |
| `GFP_NOWAIT`   | Like `GFP_ATOMIC` but **no** emergency pools. | Latency-sensitive |
| `GFP_NOIO`     | No filesystem/block reclaim — for I/O paths to avoid recursion. | Block/FS writeback |
| `GFP_NOFS`     | No filesystem reclaim. | FS code holding fs locks |
| `GFP_DMA`      | Force `ZONE_DMA` (≤ 1 MB on most arm64 platforms; rarely needed). | Legacy 24‑bit DMA |
| `GFP_DMA32`    | Force `ZONE_DMA32` (≤ 4 GB phys). Common for 32‑bit-addressable devices. | Devices with 32‑bit DMA mask |
| `__GFP_ZERO`   | Zero the buffer (same as `kzalloc`). | Any |
| `__GFP_NOFAIL` | Loop forever instead of returning NULL. **Discouraged.** | Rare |
| `__GFP_ACCOUNT`| Charge to memcg (`kmalloc-cg-*` caches, see [`mm/memcontrol.c`](https://elixir.bootlin.com/linux/v6.6/source/mm/memcontrol.c)). | cgroup-aware code |

`GFP_KERNEL = __GFP_RECLAIM | __GFP_IO | __GFP_FS` — see [`include/linux/gfp_types.h`](https://elixir.bootlin.com/linux/v6.6/source/include/linux/gfp_types.h#L262).

---

## 5. Size buckets in 6.6 (ARM64, 4 KB pages)

```
kmalloc-8       kmalloc-16      kmalloc-32      kmalloc-64
kmalloc-96   (3*32 — fills the gap)
kmalloc-128     kmalloc-192  (3*64)
kmalloc-256     kmalloc-512     kmalloc-1k      kmalloc-2k
kmalloc-4k      kmalloc-8k                       ← KMALLOC_MAX_CACHE_SIZE = 8 KB
   │
   └── above 8 KB → falls through to the page allocator (see §7).
```

Each bucket exists in **three flavors** (`enum kmalloc_cache_type` in [`include/linux/slab.h`](https://elixir.bootlin.com/linux/v6.6/source/include/linux/slab.h#L322)):

| Type                  | Used when                                        | Cache name        |
|-----------------------|--------------------------------------------------|-------------------|
| `KMALLOC_NORMAL`      | default                                          | `kmalloc-N`       |
| `KMALLOC_RECLAIM`     | `__GFP_RECLAIMABLE` (reclaimable objects)        | `kmalloc-rcl-N`   |
| `KMALLOC_DMA`         | `GFP_DMA`                                        | `dma-kmalloc-N`   |
| `KMALLOC_CGROUP`      | `__GFP_ACCOUNT` (memcg charging)                 | `kmalloc-cg-N`    |

Selection is done by `kmalloc_type(flags)` ([`include/linux/slab.h:354`](https://elixir.bootlin.com/linux/v6.6/source/include/linux/slab.h#L354)).

---

## 6. Limits & alignment

| Quantity                       | Value on ARM64 (defconfig, 4K) |
|--------------------------------|---------------------------------|
| `ARCH_KMALLOC_MINALIGN`        | **8 bytes** (arm64 cache-line aware via `cache_line_size()`; 128 on systems with 128-byte L1 lines via `ARCH_DMA_MINALIGN`) |
| `KMALLOC_MIN_SIZE`             | `8` (or `ARCH_KMALLOC_MINALIGN`) |
| `KMALLOC_MAX_CACHE_SIZE`       | `8 KB`  (largest SLUB bucket) |
| `KMALLOC_MAX_SIZE`             | `1 << (MAX_ORDER + PAGE_SHIFT - 1) = 4 MB` (MAX_ORDER=10, PAGE_SHIFT=12) — **hard upper limit** for `kmalloc()` |
| `KMALLOC_MAX_ORDER`            | `MAX_ORDER` = 10 (i.e. up to 1024 pages = 4 MB contig.) |

> **Note (DMA alignment):** since v5.19 the arm64 `ARCH_DMA_MINALIGN` can be larger than `ARCH_KMALLOC_MINALIGN`. Use `dma_kmalloc_needs_bounce()` / dedicated DMA APIs for DMA-safe buffers; do **not** rely on `kmalloc` alignment for DMA on all platforms.

---

## 7. What happens above 8 KB?

`__kmalloc_large_node()` in [`mm/slub.c:3922`](https://elixir.bootlin.com/linux/v6.6/source/mm/slub.c#L3922) routes the request directly to the **buddy allocator** via `alloc_pages(flags, get_order(size))`. The page is then folio-tagged so `kfree()` can recognize it and route to `free_large_kmalloc()`. The buffer is still physically contiguous.

---

## 8. Context constraints

| Context                       | Allowed flag(s)                  |
|-------------------------------|----------------------------------|
| Process, can sleep            | `GFP_KERNEL`, `GFP_NOIO`, `GFP_NOFS` |
| IRQ handler                   | `GFP_ATOMIC` only                |
| Softirq / tasklet             | `GFP_ATOMIC` (or `GFP_NOWAIT`)   |
| Spinlock held                 | `GFP_ATOMIC`                     |
| RCU read-side critical section| `GFP_ATOMIC`                     |
| NMI                           | **never** (`kmalloc` not NMI-safe) |

`might_sleep()` is invoked inside the slow path when `__GFP_DIRECT_RECLAIM` is set — using `GFP_KERNEL` from atomic context will splat in lockdep/kernel-debug builds.

---

## 9. Failure modes

| Cause                                   | Symptom |
|-----------------------------------------|---------|
| Atomic allocation under memory pressure | `NULL` returned; check & propagate `-ENOMEM`. |
| `size > KMALLOC_MAX_SIZE` (> 4 MB)      | `WARN_ON_ONCE` + `NULL` ([`mm/slub.c`](https://elixir.bootlin.com/linux/v6.6/source/mm/slub.c#L3935)). |
| OOM with `GFP_KERNEL`                   | OOM killer fires; eventual success or task killed. |
| `__GFP_NOFAIL` + size > 8 KB            | `WARN_ON` — `NOFAIL` is unsupported for the page-alloc fallback. |

---

## 10. When to use `kmalloc` vs. siblings

| Need                                | Use                          | Why |
|-------------------------------------|------------------------------|-----|
| Small (< 4 MB), phys-contig, fast   | **`kmalloc`**                | SLUB fast path is per-CPU, lockless. |
| Same as above + zeroed              | `kzalloc` / `kcalloc`        | Just `kmalloc + __GFP_ZERO`. |
| Many objects of identical size      | `kmem_cache_create` + `kmem_cache_alloc` | Tight packing, ctor support, dedicated stats. |
| Very large (≥ several MB), virt OK  | `vmalloc`                    | Virt-contig only; survives fragmentation. |
| Need raw pages / order-N buffer     | `alloc_pages` / `__get_free_pages` | Skip the slab, get pages directly. |
| DMA-coherent buffer for a device    | `dma_alloc_coherent`         | Proper cacheability + IOVA via SMMU. |

---

## 11. Minimal usage

```c
#include <linux/slab.h>

struct foo *f;

f = kmalloc(sizeof(*f), GFP_KERNEL);
if (!f)
    return -ENOMEM;

/* ... use f ... */

kfree(f);
```

---

## 12. Cross-references

- Internals & SLUB walk → [02_internals.md](02_internals.md)
- ARM64 call flow (Mermaid) → [03_arm64_callflow.md](03_arm64_callflow.md)
- Memory map: where the pointer lives → [04_memory_map.md](04_memory_map.md)
- Interview Q&A (Nvidia/Google/Qualcomm) → [05_interview_qna.md](05_interview_qna.md)
