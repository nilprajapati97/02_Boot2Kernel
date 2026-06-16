# kzalloc — Interview Q&A (Nvidia / Google / Qualcomm)

---

### Q1. What's the difference between `kmalloc + memset` and `kzalloc`?  `[Google]`

Functionally none — `kzalloc` is `kmalloc(size, flags | __GFP_ZERO)`. The difference is:

- **Readability**: `kzalloc` makes the zero-initialization explicit.
- **Locality**: the zero pass happens inside `slab_post_alloc_hook`, while the object is hot in L1; a separate `memset` after `kmalloc` is also hot, so perf is roughly equal.
- **Hardening**: with `CONFIG_INIT_ON_ALLOC_DEFAULT_ON`, every `kmalloc` is effectively a `kzalloc`. The distinction becomes documentation-only.

---

### Q2. How is the memset implemented on ARM64?  `[Nvidia]` `[Qualcomm]`

[`arch/arm64/lib/memset.S`](https://elixir.bootlin.com/linux/v6.6/source/arch/arm64/lib/memset.S) uses the **`DC ZVA`** instruction for sizes ≥ one cache-line block (typically 64 B). `DC ZVA` zeroes a whole cache line without first reading it from memory, halving memory traffic vs. `STP xzr, xzr` loops. The block size is queried from `DCZID_EL0` once at boot. For tiny sizes the path falls back to `STP xzr, xzr`.

For page-granular zeroing (large kzalloc > 8 KB), `clear_page()` in [`arch/arm64/lib/clear_page.S`](https://elixir.bootlin.com/linux/v6.6/source/arch/arm64/lib/clear_page.S) is a tight `DC ZVA` loop over a whole page.

---

### Q3. Does `kzalloc` zero the padding between `object_size` and the slab bucket size?  `[Google]` (info-leak angle)

**No.** It zeroes `s->object_size` bytes only. If you `copy_to_user(buf, p, ksize(p))` you can leak previous slab contents in the padding. Mitigations:

- `memzero_explicit(p, ksize(p))` after the allocation, **or**
- Create a dedicated `kmem_cache` where `object_size == size`, **or**
- Only copy `sizeof(struct)` to userspace, never `ksize`.

This is a recurring CVE pattern; the kernel has a `struct_size()` helper to prevent the related overflow class.

---

### Q4. A driver allocates a 256-byte struct with `kzalloc(GFP_KERNEL)` and DMAs it to a device. Two correctness concerns on ARM64?  `[Nvidia]` `[Qualcomm]`

Same as plain `kmalloc`:

1. **Cache coherency** on non-coherent platforms — `dma_map_single()` must do cache maintenance (clean for DMA-to-device, invalidate for DMA-from-device).
2. **DMA mask / alignment** — `ARCH_DMA_MINALIGN` on arm64 can be larger than `ARCH_KMALLOC_MINALIGN` (since 5.19). Adjacent allocations may share a cache line; a CPU writeback of a neighbor object could clobber DMA-in-flight data.

Use `dma_alloc_coherent` for DMA buffers — `kzalloc` is for the *control* path.

---

### Q5. Why can't you use a `ctor` together with `__GFP_ZERO`?  `[Qualcomm]`

A constructor is meant to leave the object in a non-zero "ready" state (e.g., initialize a spinlock). Memsetting to zero after the ctor would undo its work; running the ctor after a memset would defeat the zero invariant. SLUB resolves this by `WARN_ON_ONCE(c->ctor)` in the alloc-init path and skipping the memset — the warning catches the design conflict at runtime.

---

### Q6. What's `get_zeroed_page(flags)` and when is it preferable to `kzalloc(PAGE_SIZE, flags)`?  `[Nvidia]`

`get_zeroed_page()` is `__get_free_pages(flags | __GFP_ZERO, 0)` — it bypasses SLUB and returns a fresh page directly from the buddy allocator. Pros:

- No slab metadata overhead — full `PAGE_SIZE` is usable.
- Page-aligned VA (slab returns may not be page-aligned for small buckets, though for `PAGE_SIZE` kmalloc cache they happen to be).
- Faster for the page-sized case (skips slab housekeeping).

Use `kzalloc(PAGE_SIZE, …)` only if you need slab accounting / tracing or if size may vary.

---

### Q7. Is `kzalloc(0, GFP_KERNEL)` valid?  `[Google]`

Yes — it returns `ZERO_SIZE_PTR` (= `((void *)16)`), same as `kmalloc(0, …)`. There's nothing to zero. `kfree(ZERO_SIZE_PTR)` is a no-op.

---

### Q8. Imagine `init_on_alloc=1` is set globally. Does `kzalloc` still have a purpose?  `[Google]`

Yes, three:

1. **Backwards compatibility** — distros may not enable `init_on_alloc` for performance reasons; `kzalloc` works regardless.
2. **Documentation of intent** — code review tool can flag a `kmalloc` followed by `memset` and suggest `kzalloc`; tells future readers "this struct must start zero".
3. **Explicit hardening discipline** — security audits look for `kmalloc` of user-visible structs; `kzalloc` is the convention.

---

### Q9. Performance: is the zeroing pass measurable on a hot path?  `[Nvidia]`

For a 64 B object on a Neoverse N1, the zero is 2 × `STP` — under 5 cycles, completely hidden behind the allocation. For 4 KB pages, `clear_page` is memory-bandwidth bound (~6 GB/s on DDR4) — roughly 700 ns per page. At millions of kzallocs/sec it's a percent or two of a CPU core. Use `kmem_cache_alloc` + manual init if you need to avoid it on a critical hot path.

---

### Q10. Subtle one: a struct contains a flexible array member; `kzalloc(struct_size(p, items, n), GFP_KERNEL)`. What does `struct_size` guard against, and is the array zeroed?  `[Google]`

`struct_size(p, items, n)` ([`include/linux/overflow.h`](https://elixir.bootlin.com/linux/v6.6/source/include/linux/overflow.h)) computes `sizeof(*p) + n * sizeof(p->items[0])` with overflow detection — returning `SIZE_MAX` on overflow so the subsequent `kzalloc` fails safely (allocation of `SIZE_MAX` returns NULL after WARN). Yes, the entire requested size (including the trailing array) is `s->object_size` and gets memset to zero by `kzalloc`. This is the recommended pattern for variable-length kernel objects.

---

### Q11. KASAN + kzalloc — does KASAN see the memset?  `[Google]`

The KASAN unpoison happens **before** the memset (`kasan_slab_alloc()` is called at the top of `slab_post_alloc_hook`). The memset then writes into already-unpoisoned memory, so no shadow check fires. On free, `kasan_slab_free()` re-poisons and may quarantine. Net effect: `kzalloc` is fully KASAN-compatible with no special handling.

---

### Q12. KFENCE allocation served — what does the buffer look like?  `[Qualcomm]`

KFENCE allocates an entire page from its dedicated pool for the object. The page is unmapped on its sides (guard pages). When `__GFP_ZERO` is set, KFENCE's `kfence_guarded_alloc()` ([`mm/kfence/core.c`](https://elixir.bootlin.com/linux/v6.6/source/mm/kfence/core.c)) zeroes the entire allocated chunk before returning. Any overflow off either end faults immediately into `kfence_handle_page_fault`.

---

### Q13. You're auditing a driver. You see hundreds of `kmalloc + memset(..., 0, ...)` patterns. What's your recommendation and why?  `[Google]`

Replace with `kzalloc`. Reasons:

- Single-call atomicity — no chance of using the buffer between alloc and memset (would observe garbage; rare but real for racy init code).
- Smaller code (one call instead of two).
- Static analyzers (Coccinelle has a semantic patch for this) and Smatch flag it.
- Plays nice with `init_on_alloc` hardening — the memset becomes provably redundant and can be elided by the compiler if it sees the same flag set.

---

### Q14. Bonus, kernel-trivia: where in the source tree does the `kzalloc` definition actually live?  `[Qualcomm]`

[`include/linux/slab.h`](https://elixir.bootlin.com/linux/v6.6/source/include/linux/slab.h#L726) — it's a `static inline` of about 4 lines. There is no `kzalloc` symbol in any `.o`; every invocation inlines into `kmalloc` (with `__GFP_ZERO` ORed in) at the call site. `nm vmlinux | grep kzalloc` returns nothing.

---

## Common pitfalls

| Pitfall                                                  | Fix |
|----------------------------------------------------------|-----|
| Expecting padding bytes to be zero before `copy_to_user` | Use `memzero_explicit(p, ksize(p))` or a tight cache. |
| Using `kzalloc` for a cache that has a `ctor`            | Use plain `kmem_cache_alloc` and let the ctor run.    |
| Calling `kzalloc(huge_size, GFP_ATOMIC)` from IRQ        | The zero pass still has to run — may stretch IRQ latency for multi-KB allocations. |
| Forgetting that `kzalloc(0, …)` returns `ZERO_SIZE_PTR`  | Always check for `ZERO_OR_NULL_PTR(p)` before deref.  |
