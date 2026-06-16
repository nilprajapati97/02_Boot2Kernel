# kfree — Interview Q&A (Nvidia / Google / Qualcomm)

---

### Q1. What does `kfree(NULL)` do?  `[Google]`

It's a no-op. The first line of `kfree()` is `if (unlikely(ZERO_OR_NULL_PTR(object))) return;`. This lets error paths uniformly call `kfree(ptr)` without NULL-checking, and is idiomatic across the kernel.

---

### Q2. Walk through the fast free path on ARM64.  `[Nvidia]` `[Google]`

1. `virt_to_folio(p)` → `struct folio *` (cheap, just arithmetic on the linear-map VA).
2. Check `folio_test_slab(folio)`; if not, it's large-kmalloc → page allocator path.
3. `s = slab->slab_cache`; `slab_free_freelist_hook()` runs KASAN poison and `init_on_free` zeroing.
4. Fast path: if `slab == c->slab`, do `set_freepointer(p, c->freelist)` then `this_cpu_cmpxchg_double()` to push `p`. On ARM64 with LSE this is a single `CASPAL` instruction.
5. Done — no spinlock, no TLB work.

---

### Q3. What happens when you free a pointer obtained from `vmalloc` via `kfree`?  `[Qualcomm]`

`virt_to_folio()` on a vmalloc address points to a `struct page` whose folio is **not** tagged `PG_slab`. SLUB then treats it as large-kmalloc and calls `__free_pages()` on it — but the folio's `order` is bogus (it's a per-page mapping into vmalloc area; there is no contiguous physical order). Result: corruption of buddy's free area, often delayed kernel panic. Distros enable `CONFIG_DEBUG_VM` to catch this with a clear splat. Always use `vfree` or `kvfree`.

---

### Q4. Double-free detection — how does SLUB catch it?  `[Google]`

For each free, `slab_free_freelist_hook` calls `slab_free_freelist_hook` which (with `CONFIG_SLUB_DEBUG_ON` or boot-arg `slub_debug=F`) walks the slab's freelist to ensure the object isn't already on it. KASAN does it via shadow memory in O(1) and catches the second free instantly. Without debug, double-free corrupts the freelist pointer; the next allocation may return the same object to two callers — silent UAF.

---

### Q5. Explain `kfree_rcu` and when you need it.  `[Google]` `[Nvidia]`

`kfree_rcu(p, rcu_head_member)` schedules the free for after the next RCU grace period via `call_rcu`. Use it whenever readers might still hold a pointer to `p` under `rcu_read_lock()`. Pattern:

```c
old = rcu_dereference_protected(*pp, lock_held);
rcu_assign_pointer(*pp, new);
kfree_rcu(old, rcu);   /* old->rcu is a struct rcu_head member */
```

Direct `kfree(old)` would risk dereferencing freed memory in a concurrent reader.

---

### Q6. Difference between `kfree` and `kfree_sensitive`?  `[Qualcomm]`

`kfree_sensitive(p)` (formerly `kzfree`) calls `memzero_explicit(p, ksize(p))` before the regular `kfree`. `memzero_explicit` uses `barrier_data()` to prevent the compiler from eliding the memset as dead-store. Use it for credentials, crypto keys, kernel keyring entries — anything that would be an info-leak if observed via a UAF.

---

### Q7. When does `kfree` cause pages to go back to the buddy allocator?  `[Nvidia]`

Two cases:

1. **Large-kmalloc** (> 8 KB): always direct `__free_pages()` of the whole `2^order` block.
2. **Normal SLUB**: when the last object on a slab is freed *and* `n->nr_partial > s->min_partial`. `discard_slab()` removes the slab from `n->partial` (under `n->list_lock`) and calls `__free_pages()`. Tunable via `/sys/kernel/slab/<cache>/min_partial`.

---

### Q8. The linear map keeps every page mapped forever. So what does `kfree` actually "release"?  `[Qualcomm]`

It releases **ownership accounting**, not the mapping. The bytes of the object remain accessible (in the MMU sense) but are now either (a) on a slab freelist for reuse by the same cache, or (b) on a buddy free-area for reuse by anyone — including future page-cache pages, anonymous pages, slabs for *other* caches, etc. The next user gets the same VA pattern.

---

### Q9. NMI-safety of `kfree`?  `[Nvidia]`

Not safe. The fast path relies on per-CPU state that may be inconsistent if an NMI interrupts a `kmalloc`/`kfree` in flight. The slow path may take `n->list_lock` which an interrupted thread could already hold, deadlocking. For NMI contexts use pre-allocated buffers (e.g., perf ring buffers) or NMI-safe primitives like `memblock` reserves.

---

### Q10. A 64-core ARM64 system shows hot spinlock contention on `n->list_lock` for `kmalloc-128`. What's happening and how do you mitigate?  `[Google]`

This usually means the slow path is being taken often:

- Per-CPU active slabs and `c->partial` chains are exhausted frequently → forcing trips to the node partial list.
- Or: many remote-free transitions are causing slabs to bounce between full/partial.

Mitigations:

1. Bump `/sys/kernel/slab/kmalloc-128/cpu_partial` (number of objects buffered per CPU) — absorbs bursts.
2. Investigate the workload — maybe a dedicated `kmem_cache_create()` with `SLAB_HWCACHE_ALIGN` is warranted to remove this cache from the SLAB-merge fold (`CONFIG_SLAB_MERGE_DEFAULT=n`).
3. Reduce remote frees by pinning producers/consumers (NUMA, IRQ affinity).
4. Profile with `perf lock contention` and `bpftrace -e 'kfunc:__slab_free { @[kstack] = count(); }'`.

---

### Q11. KASAN quarantine — what is it and why?  `[Google]`

After `kfree`, KASAN's quarantine ([`mm/kasan/quarantine.c`](https://elixir.bootlin.com/linux/v6.6/source/mm/kasan/quarantine.c)) holds the object for a while before letting it return to the freelist. Goal: extend the UAF detection window so a stale pointer dereference still hits a poisoned (`0xFB`) shadow byte instead of valid re-allocated data. Cost: increased memory footprint proportional to slab usage; tuned via `KASAN_QUARANTINE_PERCPU_SIZE`.

---

### Q12. `ksize(p)` vs `sizeof(struct foo)` — when to use which?  `[Qualcomm]`

`sizeof(struct foo)` is the **C-level** struct size; `ksize(p)` is the **actual** allocation size (the bucket size, e.g., 128 for a `sizeof(foo)=100` allocation in `kmalloc-128`). Use `ksize` when you legitimately need to know how much you can scribble — e.g., extending a buffer in place. Do **not** use it to bound `copy_to_user` unless you've explicitly zeroed the padding — the padding bytes are uninitialized.

---

### Q13. Walk through `kfree` of a 1 MB allocation.  `[Nvidia]`

```
kfree(p)
  -> virt_to_folio(p)         // get folio*
  -> folio_test_slab() == false (large-kmalloc)
  -> free_large_kmalloc(folio, p)
      -> order = folio_order(folio)        // log2(1MB/4KB) = 8
      -> kasan_kfree_large(p)              // shadow poison
      -> mod_lruvec_page_state(..., NR_SLAB_UNRECLAIMABLE_B, -1MB)
      -> __free_pages(folio_page(folio,0), 8)
          -> buddy returns 256 pages to zone->free_area[8]
```

No slab housekeeping at all; the 256-page contiguous block is reusable by any order-≤8 future allocation.

---

### Q14. What's the relationship between `kfree` and the per-CPU `kmem_cache_cpu.tid` field?  `[Qualcomm]`

`tid` is a per-CPU counter incremented on every fast-path alloc and free. When `cmpxchg_double(freelist, tid, ...)` fires, it verifies *both* (a) we still hold the same freelist head we observed, and (b) `tid` hasn't changed — guaranteeing no other op was interleaved (e.g., we weren't preempted to another CPU mid-sequence). Without `tid`, ABA would let us push to a stale freelist.

---

### Q15. Imagine `kfree(p)` is called but `p` is a pointer to the *middle* of an object (e.g., `p = orig + 8`). What happens?  `[Google]`

`virt_to_folio(p)` still resolves to the same folio (page-granular). `slab = folio_slab(folio)` gives the right slab. SLUB then computes the object index from the slab base: `(p - slab_address) / s->size` — which yields a bogus index, and `set_freepointer(p, ...)` writes the freelist pointer at `p + s->offset` — corrupting whatever lives there. With `CONFIG_SLUB_DEBUG`, `check_valid_pointer()` detects misalignment and splats `Object pointer 0x… is not aligned`. Without debug, silent corruption; KASAN catches the subsequent UAF.

---

## Common pitfalls

| Pitfall                                              | Fix |
|------------------------------------------------------|-----|
| `kfree` on `vmalloc` pointer                         | Use `vfree` or `kvfree`. |
| Storing keys/secrets and `kfree`'ing without wiping  | Use `kfree_sensitive`.   |
| Direct `kfree` when readers still hold the pointer   | Use `kfree_rcu` or `synchronize_rcu` first. |
| Freeing the address of a struct member               | Free the original pointer, not `&obj->field`. |
| Assuming pages return to the OS                      | They return to buddy — still mapped via linear map, still in the kernel's footprint. |
