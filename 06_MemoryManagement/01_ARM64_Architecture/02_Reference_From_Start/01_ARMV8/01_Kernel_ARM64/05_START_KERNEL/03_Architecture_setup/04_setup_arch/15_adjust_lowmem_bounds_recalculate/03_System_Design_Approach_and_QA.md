# adjust_lowmem_bounds() Second Call — System Design Approach and Q&A

## 1. The Precautionary Correction Pattern

The two-call design of `adjust_lowmem_bounds()` is an example of the **precautionary correction pattern** in systems programming:

```
Problem: A → B → changes A (circular dependency)

Naive solution (wrong):
  Compute A once, compute B using A, done.
  Bug: B changes the environment that A was computed from.

Correct solution: Correction pass
  Step 1: Compute A (approximate)
  Step 2: Compute B using A (B changes the environment)
  Step 3: Recompute A (accurate, now accounts for B's changes)
```

This pattern appears in many systems:
- Linker: computes symbol addresses (approx), emits code, recomputes (link-time optimization)
- Compiler: builds symbol table (pass 1), generates code (pass 2), fixes up addresses (pass 3)
- Database VACUUM: estimates live rows, scans, corrects statistics

---

## 2. Invariant Maintained by the Two-Call Design

The invariant `paging_init()` requires:

> `arm_lowmem_limit` must point to the highest byte of **continuously available** physical memory below `vmalloc_limit`.

This invariant holds only when `adjust_lowmem_bounds()` has been called **after all reservations are complete**. The second call ensures this invariant.

If only one call were made (before `arm_memblock_init()`), the invariant would be violated for any system where `mdesc->reserve()` or FDT nodes remove memory from the top of the range.

---

## 3. Design Alternatives

### Alternative A: Pass arm_lowmem_limit to arm_memblock_init(), recompute inside

```c
/* Hypothetical redesign: */
void __init arm_memblock_init(const struct machine_desc *mdesc)
{
    ...all reservations...
    adjust_lowmem_bounds_internal();  /* compute final value here */
}
```

This avoids the two-call pattern. Problem: `arm_lowmem_limit` must be visible to `dma_contiguous_reserve()` which is called inside `arm_memblock_init()`. The first call to `adjust_lowmem_bounds()` is needed as a precondition. Making it internal to `arm_memblock_init()` would require calling it twice internally, achieving the same result.

### Alternative B: Use a sentinel value for first call, skip constraint in arm_memblock_init()

```c
/* Hypothetical: skip arm_lowmem_limit for first dma_contiguous_reserve() */
dma_contiguous_reserve(arm_dma_limit);  /* arm_dma_limit independent of arm_lowmem_limit */
```

Actually, this is close to what happens. `arm_dma_limit` from `setup_dma_zone()` is used as the constraint for `dma_contiguous_reserve()`, not `arm_lowmem_limit` directly. `arm_lowmem_limit` affects `memblock_set_current_limit()`, which constrains general memblock allocations. Removing the first call would make general allocations during `arm_memblock_init()` unconstrained, potentially placing data structures above the future paging limit.

### Alternative C: Delay all reservations until after paging_init()

Not feasible: `paging_init()` needs `arm_lowmem_limit` to know how much to map. And page table allocations use `memblock_alloc()` — if reservations haven't been applied, memblock might allocate within a GPU-reserved region for page tables.

---

## 4. Dependency Graph for Second Call

```
[arm_memblock_init()]
  ├── kernel image reserved
  ├── initrd reserved
  ├── page tables reserved
  ├── mdesc->reserve() applied  ← KEY: may remove top-of-memory
  ├── FDT /reserved-memory applied ← KEY: may fragment memory
  └── CMA reserved
        │
        ▼
[adjust_lowmem_bounds() — SECOND CALL]
        │
        ├── for_each_mem_range (post-reservation → accurate topology)
        ├── arm_lowmem_limit ← UPDATED (final value)
        ├── high_memory ← UPDATED
        └── memblock_set_current_limit() ← UPDATED
              │
              ▼
[early_ioremap_reset()]  — needs stable memblock (next step)
              │
              ▼
[paging_init()]
  └── map_lowmem() maps 0..arm_lowmem_limit into kernel VA
```

---

## 5. System Design Q&A

**Q: Is there a risk that the two-call approach still gets the wrong answer if reservations change iteratively?**
> No — the second call runs after ALL reservations are complete. `arm_memblock_init()` does all reservations in one function: kernel image, initrd, page tables, `mdesc->reserve()`, FDT nodes, and CMA. There is no code path that reserves memory between the second `adjust_lowmem_bounds()` call and `paging_init()`. The second call sees the final, complete reservation state. The only subsequent memory operations are `early_ioremap_reset()` (which just resets a function pointer, no memblock changes) and `paging_init()` itself (which uses `arm_lowmem_limit` as input, doesn't change reservations before calling `map_lowmem()`).

**Q: Why don't ARM64 systems need this two-phase recalculation?**
> ARM64's 48-bit virtual address space decouples DMA constraints from VA crunch constraints. On ARM64: (1) The DMA limit is determined by device DMA masks and IOMMU capabilities, not by VA space. (2) There is no lowmem/highmem split — all physical RAM is in the direct linear map. (3) `arm64_memblock_init()` doesn't need a pre-existing `arm_lowmem_limit` to compute its DMA constraints. The circular dependency that forces two calls on ARM32 doesn't exist on ARM64.

**Q: What would happen at runtime if arm_lowmem_limit were permanently set too high after board reservations?**
> `paging_init()` would call `map_lowmem()` up to the incorrect (too-high) `arm_lowmem_limit`. This creates PTEs in the direct map for physical addresses that belong to the GPU/firmware/TrustZone. When the kernel accesses a struct page for these PFNs (e.g., via `pfn_to_page()`), it either reads garbage (non-coherent GPU memory) or triggers a secure fault (TrustZone access violation). The secure fault would manifest as an imprecise abort exception, crashing the kernel with a cryptic "Unhandled fault: external abort on non-linefetch at 0xCxxxxxxx". Extremely hard to diagnose without knowing about the missing second `adjust_lowmem_bounds()` call.

**Q: Can user space detect whether the second call changed arm_lowmem_limit?**
> Indirectly yes, through `/proc/meminfo`. The `LowTotal` field shows memory in ZONE_NORMAL (lowmem). If `arm_lowmem_limit` was corrected down by the second call (because some RAM was reserved), `LowTotal` is smaller than it would be with only one call. Also, `dmesg` shows the board reservations: `memblock_reserve: [0x38000000-0x3FFFFFFF] board_reserve_memory()`. And `cat /proc/iomem` shows reserved regions. These together show what was removed from the kernel's view of memory.
