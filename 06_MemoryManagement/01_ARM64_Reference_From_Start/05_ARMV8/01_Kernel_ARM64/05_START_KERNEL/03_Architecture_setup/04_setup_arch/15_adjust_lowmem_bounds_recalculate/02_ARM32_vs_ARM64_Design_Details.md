# adjust_lowmem_bounds() Second Call — ARM32 vs ARM64 Design Details

## 1. ARM64 Has No Equivalent Recalculation

As established in directory `13_adjust_lowmem_bounds_first_call`, ARM64 has no `adjust_lowmem_bounds()` at all — no lowmem/highmem split exists on ARM64.

This document focuses on the contrast between:
- **ARM32**: Two-phase recalculation pattern (before and after reservations)
- **ARM64**: Single-pass `arm64_memblock_init()` that handles all reservations at once

---

## 2. Why ARM32 Needs Two-Phase Recalculation

The root cause is the **circular dependency** in ARM32's design:

```
arm_lowmem_limit  ←── adjust_lowmem_bounds()
      │
      └──used by──→  arm_memblock_init()
                           │
                           └──changes──→  memblock state
                                               │
                                               └──affects──→  arm_lowmem_limit
```

ARM32 resolves this circular dependency with two calls: bootstrap approximation first, then correction.

### ARM64 Avoids the Circular Dependency

ARM64's `arm64_memblock_init()` does not use any equivalent of `arm_lowmem_limit`:

```c
/* ARM64 arm64_memblock_init() — no arm_lowmem_limit dependency */
void __init arm64_memblock_init(void)
{
    /* All reservations in one function, no pre-existing limit needed */
    memblock_reserve(__pa(KERNEL_START), _end - _text);
    reserve_initrd_mem();
    early_init_fdt_scan_reserved_mem();
    dma_contiguous_reserve(arm64_dma_phys_limit);
    /* arm64_dma_phys_limit is computed from ARM64 IOMMU/DMA capabilities,
       not from a VA-space-limited lowmem calculation */
}
```

`arm64_dma_phys_limit` on ARM64 comes from the DMA mask of devices (via IOMMU or device DMA address constraints), not from a VA crunch calculation. No circular dependency → no two-phase pattern needed.

---

## 3. ARM32 Two-Phase Pattern vs ARM64 Single-Phase

### ARM32 Boot Sequence (Virtual Address Crunch Resolution)

```
Phase 1: adjust_lowmem_bounds()
         vmalloc_limit = VMALLOC_END - vmalloc_size - VMALLOC_OFFSET - PAGE_OFFSET + PHYS_OFFSET
         arm_lowmem_limit = highest available byte < vmalloc_limit
         [APPROXIMATE — based on pre-reservation memblock]

Phase 2: arm_memblock_init(mdesc)
         Reservations change memblock
         Uses arm_lowmem_limit from Phase 1 (needed for dma_contiguous_reserve)

Phase 3: adjust_lowmem_bounds()  ← SECOND CALL
         arm_lowmem_limit = recalculated with post-reservation memblock
         [ACCURATE — what paging_init() will actually use]
```

### ARM64 Boot Sequence (No VA Crunch)

```
Phase 1: arm64_memblock_init()
         All reservations in one function
         arm64_dma_phys_limit computed independently
         No recalculation needed after reservations

[No Phase 2 needed — single pass is sufficient]
```

---

## 4. Recalculation Impact: Concrete Numbers

### Example: ARM32 System with 1GB RAM, GPU Takes Top 128MB

**After first adjust_lowmem_bounds():**
```
Available memory:   0x00000000 – 0x3FFFFFFF (1GB)
vmalloc_limit:      0x40000000 (1GB)
arm_lowmem_limit:   0x3FFFFFFF
high_memory:        0xC0000000 + 0x3FFFFFFF = 0xFFFFFFFF (near end of kernel VA)
```

**After arm_memblock_init() (GPU reserves top 128MB):**
```
memblock.reserved:  0x38000000 – 0x3FFFFFFF (128MB)
Available memory:   0x00000000 – 0x37FFFFFF (0.875GB)
```

**After second adjust_lowmem_bounds():**
```
for_each_mem_range sees available top at 0x37FFFFFF
arm_lowmem_limit:   0x37FFFFFF  ← 128MB lower than before
high_memory:        0xC0000000 + 0x37FFFFFF = 0xF7FFFFFF
```

`paging_init()` now calls `map_lowmem()` up to `arm_lowmem_limit = 0x37FFFFFF`. The GPU region (0x38000000–0x3FFFFFFF) is NOT in the direct map. GPU driver uses `ioremap()` if it needs to access GPU-side registers.

---

## 5. The Role of for_each_mem_range in Recalculation

The key to understanding what changes between calls:

```c
/* mm/memblock.c */
#define for_each_mem_range(i, p_start, p_end) \
    __for_each_mem_range(i, &memblock.memory, &memblock.reserved, \
                         NUMA_NO_NODE, MEMBLOCK_NONE, p_start, p_end, NULL)
```

`for_each_mem_range` iterates the **intersection** of memory[] minus reserved[]. After `arm_memblock_init()`, the reserved[] array has grown significantly. The iterator skips all reserved regions. So the second `adjust_lowmem_bounds()` sees only truly available memory.

Contrast with `for_each_mem_range`:
- Before arm_memblock_init: returns large ranges
- After arm_memblock_init: returns same or smaller/fragmented ranges

---

## 6. When Second Call Produces No Change

In many simple systems, the second call produces identical output to the first:

```
Simple 256MB system, no board reservations, no /reserved-memory nodes:

After first call:
  arm_lowmem_limit = 0x0FFFFFFF

After arm_memblock_init():
  Reserved: kernel image (e.g., 8MB at 0x00008000)
  Reserved: initrd (e.g., 16MB at 0x00800000)
  These are within the middle of RAM, not at the top

After second call:
  for_each_mem_range still returns range up to 0x0FFFFFFF
  arm_lowmem_limit = 0x0FFFFFFF  ← UNCHANGED
```

The double-call is a **precautionary correctness measure**, not always necessary in practice. But it is always correct.

---

## 7. Comparison Table

| Aspect | ARM32 First Call | ARM32 Second Call | ARM64 |
|--------|-----------------|------------------|-------|
| When called | Before arm_memblock_init | After arm_memblock_init | N/A |
| memblock state | Pre-reservation | Post-reservation | N/A |
| Purpose | Bootstrap approximation | Final accurate value | N/A |
| Used by | arm_memblock_init (for dma_contiguous_reserve) | paging_init (for map_lowmem) | N/A |
| arm_lowmem_limit | Set (approximate) | Set (accurate) | Not used |
| high_memory | Set | Updated | Not used |
| memblock current limit | Set | Updated | N/A |
| Same function | Yes | Yes | No equivalent |
| Result changes? | Depends on reservations | May be same or lower | N/A |

## 8. ARM64 Memory Limit Equivalent

ARM64 does have a concept analogous to constraining memory allocations — but it's simpler:

```c
/* ARM64 does not use arm_lowmem_limit, but uses: */
memblock_set_current_limit(memblock_end_of_DRAM());

/* And for DMA: */
arm64_dma_phys_limit = get_arm64_dma_phys_limit();
/* Which is: min(memblock_end_of_DRAM(), DMA_BIT_MASK(32)) for 32-bit DMA devices */
/*           or full 64-bit for 64-bit DMA devices with IOMMU */
```

ARM64 never needs a recalculation because its DMA limit is not derived from VA space constraints. It's purely a physical address constraint from device capabilities.
