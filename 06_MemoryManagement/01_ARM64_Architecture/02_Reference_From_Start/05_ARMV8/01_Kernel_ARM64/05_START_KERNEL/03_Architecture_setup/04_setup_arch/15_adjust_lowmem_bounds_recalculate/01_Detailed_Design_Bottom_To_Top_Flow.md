# adjust_lowmem_bounds() Second Call — Detailed Design

## 1. Context: Why the Second Call Is Needed

```
setup_arch():
  adjust_lowmem_bounds()     ← FIRST CALL — based on full FDT memory
  arm_memblock_init(mdesc)   ← Reservations applied:
        ├── mdesc->reserve() → may remove top-of-memory regions
        ├── FDT /reserved-memory → may fragment available memory
        └── CMA reserve → removes a contiguous chunk
  adjust_lowmem_bounds()     ← SECOND CALL — recalculate with updated memblock
```

The same function `adjust_lowmem_bounds()` in `arch/arm/mm/mmu.c` is called twice. No code change — but the **state of memblock** has changed between the two calls.

---

## 2. The Problem: Reservations Change the Memory Topology

### Scenario A: Top-of-Memory Reservation

Before `arm_memblock_init()`:
```
memblock.memory:   [0x00000000 - 0x3FFFFFFF]  (1GB continuous)
arm_lowmem_limit:  0x3FFFFFFF  (all 1GB is lowmem)
```

`mdesc->reserve()` removes 128MB from the top for GPU:
```
memblock.memory:   [0x00000000 - 0x37FFFFFF]  (0.875GB)
memblock.reserved: [0x38000000 - 0x3FFFFFFF]  (GPU — 128MB)
```

After second `adjust_lowmem_bounds()`:
```
arm_lowmem_limit:  0x37FFFFFF  (recalculated — 0.875GB is the new top of usable memory)
high_memory:       __va(0x38000000)
```

Without the second call, `paging_init()` would try to map virtual addresses for physical range 0x38000000–0x3FFFFFFF as lowmem, but that physical memory is now GPU reserved — kernel would create PTEs for GPU memory in the direct map, which is wrong and potentially dangerous.

### Scenario B: Memory Fragmentation

Before `arm_memblock_init()`:
```
memblock.memory:   [0x00000000 - 0x1FFFFFFF]  Block A (512MB)
                   [0x20000000 - 0x3FFFFFFF]  Block B (512MB)
arm_lowmem_limit:  0x3FFFFFFF  (all 1GB)
```

`early_init_fdt_scan_reserved_mem()` reserves a hole in the middle:
```
memblock.reserved: [0x1C000000 - 0x1FFFFFFF]  (no-map region, 64MB)
```

After second `adjust_lowmem_bounds()`:
```
arm_lowmem_limit:  recalculated stopping at last contiguous free region
memblock_limit:    possibly lowered to 0x1C000000 (can't page-map through a gap)
```

---

## 3. Source Code: Same Function, Different Context

```c
/* arch/arm/mm/mmu.c:1196 — called twice */
void __init adjust_lowmem_bounds(void)
{
    phys_addr_t block_start, block_end, memblock_limit = 0;
    u64 vmalloc_limit, i;
    phys_addr_t lowmem_limit = 0;

    /* Step 1: Compute vmalloc_limit (same formula both times) */
    vmalloc_limit = (u64)VMALLOC_END - vmalloc_size - VMALLOC_OFFSET -
                    PAGE_OFFSET + PHYS_OFFSET;

    /* Step 2: Scan memblock — this time with reservations in effect */
    for_each_mem_range(i, &block_start, &block_end) {
        /* for_each_mem_range returns AVAILABLE regions (not reserved) */
        if (block_start < vmalloc_limit) {
            if (block_end > lowmem_limit)
                lowmem_limit = min_t(u64, vmalloc_limit, block_end);
            ...
        }
    }

    /* Step 3: Update globals (OVERWRITES first call's values) */
    arm_lowmem_limit = lowmem_limit;
    high_memory = __va(arm_lowmem_limit - 1) + 1;

    ...
    memblock_set_current_limit(memblock_limit);
}
```

**Key insight**: `for_each_mem_range()` iterates **available** (non-reserved) memory ranges. Between the two calls, reservations have reduced what's available. So the second call computes `lowmem_limit` over a more accurate available-memory set.

---

## 4. What Changes Between Call 1 and Call 2

| State | After Call 1 | After Call 2 |
|-------|-------------|-------------|
| memblock reservations | Only EFI reservations | + kernel image + initrd + board + CMA |
| arm_lowmem_limit | Conservative estimate | Accurate final value |
| high_memory | Based on full RAM | Based on actually usable RAM |
| memblock current limit | Permissive | Corrected for final topology |

The second call's output is the value that `paging_init()` uses. The first call's output is an approximation sufficient for `arm_memblock_init()` to do its work.

---

## 5. CMA Interaction with Second adjust_lowmem_bounds()

CMA reserves a contiguous region, usually at the **top of available memory**:

```
Before arm_memblock_init():
  Available: [0x00000000 - 0x3FFFFFFF] (1GB)
  arm_lowmem_limit: 0x3FFFFFFF

After arm_memblock_init() (CMA reserve 128MB at top):
  Reserved: [0x38000000 - 0x3FFFFFFF] (CMA — 128MB)
  Available: [0x00000000 - 0x37FFFFFF] (0.875GB)

After second adjust_lowmem_bounds():
  arm_lowmem_limit: 0x3FFFFFFF (CMA is RESERVED not removed — still in lowmem VA range)
```

Wait — CMA is reserved, not removed from memory. Does it affect `adjust_lowmem_bounds()`?

**Answer**: `for_each_mem_range()` iterates the **memory** array, not the **reserved** array. It returns all physical memory, including reserved regions. But `memblock_limit` (the PMD-alignment check) is affected by CMA being non-contiguous with non-reserved memory.

The CMA region is mapped in the direct kernel map — its pages are given to the buddy allocator as MIGRATE_CMA type. `arm_lowmem_limit` covers the CMA region. So the second call doesn't dramatically change `arm_lowmem_limit` due to CMA, but it does update `memblock_limit` based on PMD-alignment of the first contiguous block.

---

## 6. Difference from First Call: Why Not Always Call Twice?

Could the kernel just call `adjust_lowmem_bounds()` once, after `arm_memblock_init()`?

**No**, because `arm_memblock_init()` itself needs `arm_lowmem_limit`:

```c
/* arm_memblock_init() calls: */
dma_contiguous_reserve(arm_dma_limit);

/* dma_contiguous_reserve eventually calls: */
cma_declare_contiguous_nid(base, size, limit, alignment, ...);
  /* 'limit' here is arm_dma_limit which is <= arm_lowmem_limit */
  /* Without arm_lowmem_limit being set, CMA reserve has no upper bound */
```

Also, `mdesc->reserve()` implementations sometimes use `arm_lowmem_limit` in their logic:
```c
/* hypothetical mdesc->reserve() that limits reservations to lowmem */
static void __init myboard_reserve(void)
{
    if (arm_lowmem_limit > SOME_THRESHOLD)
        memblock_reserve(EXTRA_REGION, EXTRA_SIZE);
}
```

The two-call pattern bootstraps the dependency cycle: Call 1 sets `arm_lowmem_limit` so `arm_memblock_init()` can work; Call 2 corrects it after `arm_memblock_init()` finishes.

---

## 7. Interview Q&A

**Q1: Is the second call to adjust_lowmem_bounds() always necessary?**
> It depends on the platform. If `mdesc->reserve()` and FDT `/reserved-memory` nodes don't remove any memory from the top of the physical address space, the second call produces the same result as the first. However, the kernel always makes the second call because it cannot know in advance whether reservations changed the topology. The cost is negligible (a single iteration over memblock regions). Skipping it would be an optimization that trades correctness for trivial performance gain — clearly wrong.

**Q2: What happens to paging_init() if arm_lowmem_limit is incorrect (too high)?**
> If `arm_lowmem_limit` is too high (e.g., includes a GPU-reserved region), `map_lowmem()` in `paging_init()` creates page table entries mapping the GPU's physical memory into the kernel's direct map. Kernel code can now dereference virtual addresses that point to GPU memory. Read operations return undefined data (GPU memory may not be coherent with CPU cache). Write operations corrupt GPU memory (GPU may interpret corrupted data as commands, causing display corruption or GPU lockup). This is a hard-to-debug memory corruption class.

**Q3: Why does the comment in setup_arch() say adjust_lowmem_bounds() must run before any memory reservation?**
> The comment refers specifically to the FIRST call. The reason: `dma_contiguous_reserve()` (inside `arm_memblock_init()`) calls `memblock_alloc_range_nid()` which needs a valid upper bound for DMA-safe memory. This bound comes from `arm_dma_limit`, which was set by `setup_dma_zone()`. But `memblock_set_current_limit()` (called by `adjust_lowmem_bounds()`) also constrains allocations. Without the first `adjust_lowmem_bounds()`, `memblock_set_current_limit()` is at its default (all of physical memory), and CMA could be placed above the safe zone for early allocations.
