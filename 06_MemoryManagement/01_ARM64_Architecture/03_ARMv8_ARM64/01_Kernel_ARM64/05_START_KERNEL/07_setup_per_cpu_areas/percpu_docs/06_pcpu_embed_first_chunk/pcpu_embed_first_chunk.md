# `pcpu_embed_first_chunk()` — Primary Per-CPU Allocator

## Source Reference
- `mm/percpu.c:3075` — function definition
- `mm/percpu.c:2864` — calls `pcpu_build_alloc_info()`
- `mm/percpu.c:2608` — calls `pcpu_setup_first_chunk()`

---

## Function Signature

```c
/* mm/percpu.c:3075 */
/**
 * pcpu_embed_first_chunk - embed the first percpu chunk into bootmem
 * @reserved_size: the size of reserved percpu area in bytes
 * @dyn_size: minimum free size for dynamic allocation in bytes
 * @atom_size: allocation atom size
 * @cpu_distance_fn: callback to determine distance between cpus, optional
 * @alloc_fn: function to allocate percpu page, optional
 *
 * This is a helper to ease setting up embedded first percpu chunk and
 * can be called where pcpu_setup_first_chunk() is expected.
 *
 * Returns 0 on success, -errno on failure.
 */
int __init pcpu_embed_first_chunk(size_t reserved_size, size_t dyn_size,
                                  size_t atom_size,
                                  pcpu_fc_cpu_distance_fn_t cpu_distance_fn,
                                  pcpu_fc_alloc_fn_t alloc_fn)
```

---

## 7-Stage Algorithm

### Stage 1: Build Allocation Info (`pcpu_build_alloc_info`)

```c
/* mm/percpu.c:~3090 */
ai = pcpu_build_alloc_info(reserved_size, dyn_size, atom_size, cpu_distance_fn);
if (IS_ERR(ai))
    return PTR_ERR(ai);
```

Produces a `struct pcpu_alloc_info` with:
- `static_size`: size of `.data..percpu` template
- `unit_size`: padded size of each CPU unit (page-aligned)
- `nr_groups`: number of NUMA groups
- `groups[]`: group info with CPU maps

**Interview point:** `pcpu_build_alloc_info()` is the intelligence of the allocator.
It decides how many groups, how many units per alloc, and which CPUs go together.

---

### Stage 2: Allocate Memory Per Group (memblock_alloc_try_nid)

```c
/* mm/percpu.c:~3105 */
for (group = 0; group < ai->nr_groups; group++) {
    struct pcpu_group_info *gi = &ai->groups[group];
    unsigned int cpu = NR_CPUS;

    /* Find any valid CPU in this group to get its node */
    for (i = 0; i < gi->nr_units; i++)
        if (gi->cpu_map[i] != NR_CPUS) {
            cpu = gi->cpu_map[i];
            break;
        }

    /* Allocate contiguous memory for the entire group */
    ptr = memblock_alloc_try_nid(
        gi->alloc_size,           /* size = nr_units * unit_size (rounded) */
        PAGE_SIZE,                /* alignment */
        __pa(MAX_DMA_ADDRESS),    /* min physical address */
        MEMBLOCK_ALLOC_ACCESSIBLE, /* max physical address */
        cpu_to_node(cpu));        /* NUMA node preference */

    if (!ptr) {
        rc = -ENOMEM;
        goto out_free_areas;
    }

    /* Store the group's base address */
    gi->base_addr = ptr;           /* virtual address */
}
```

**Interview point:** Each NUMA group gets its own `memblock_alloc_try_nid()` call,
preferring the local NUMA node. This ensures CPU N's per-CPU data is physically close
to CPU N's memory controller → lower latency for any cross-CPU access.

---

### Stage 3: Find Global Base Address

```c
/* mm/percpu.c:~3125 */
base = (void *)ULONG_MAX;
for (group = 0; group < ai->nr_groups; group++) {
    base = min_t(void *, base, ai->groups[group].base_addr);
}
/* base = lowest virtual address across all group allocations */
```

This finds `pcpu_base_addr` — the minimum address of all allocated per-CPU regions.
Using the minimum ensures `base_offset` for all groups is non-negative.

---

### Stage 4: Compute Group Base Offsets

```c
/* mm/percpu.c:~3132 */
for (group = 0; group < ai->nr_groups; group++) {
    ai->groups[group].base_offset =
        ai->groups[group].base_addr - base;
}
/* base_offset[0] = 0 (it's the minimum) */
/* base_offset[1] = group1.base_addr - base (positive, non-zero for NUMA) */
```

---

### Stage 5: Copy Template Into Each CPU Unit

```c
/* mm/percpu.c:~3140 */
for (group = 0; group < ai->nr_groups; group++) {
    struct pcpu_group_info *gi = &ai->groups[group];

    for (i = 0; i < gi->nr_units; i++) {
        void *unit_addr = gi->base_addr + i * ai->unit_size;

        if (gi->cpu_map[i] == NR_CPUS) {
            /* Padding unit — no real CPU; zero it out */
            memset(unit_addr, 0, ai->unit_size);
            continue;
        }

        /* Copy the .data..percpu template into this CPU's unit */
        memcpy(unit_addr, __per_cpu_load, ai->static_size);

        /* Zero the reserved and dynamic regions */
        memset(unit_addr + ai->static_size, 0,
               ai->unit_size - ai->static_size);
    }
}
```

**Critical detail:** This `memcpy` reads from `__per_cpu_load`, NOT `__per_cpu_start`.
On ARM32 with XIP kernels, these differ:
- `__per_cpu_load` = physical address where template was loaded (in RAM)
- `__per_cpu_start` = virtual address where template runs

On ARM64 and non-XIP ARM32: `__per_cpu_load == __per_cpu_start`.

---

### Stage 6: Free Tail Padding (memblock_free)

```c
/* mm/percpu.c:~3155 */
for (group = 0; group < ai->nr_groups; group++) {
    struct pcpu_group_info *gi = &ai->groups[group];

    /* The allocation may have been rounded up beyond actual units used */
    /* Free the unused tail */
    size_t used = gi->nr_units * ai->unit_size;
    size_t allocated = gi->alloc_size;

    if (allocated > used) {
        memblock_free(gi->base_addr + used,
                      allocated - used);
    }
}
```

This reclaims any over-allocated memory. For example, if `upa = 4` but the group has
only 3 CPUs, the 4th unit's memory is freed back to memblock.

---

### Stage 7: Setup First Chunk (`pcpu_setup_first_chunk`)

```c
/* mm/percpu.c:~3165 */
rc = pcpu_setup_first_chunk(ai, base);
if (rc)
    goto out_free_areas;
```

Passes the allocation info and base address to the internal chunk setup function,
which initializes the chunk management structures.

---

## Full Function Flow Diagram

```
pcpu_embed_first_chunk(reserved_size=8KB, dyn_size=20KB, atom_size=PAGE_SIZE)
│
├─ [Stage 1] pcpu_build_alloc_info()
│   → Returns ai (static_size, unit_size, nr_groups, groups[])
│
├─ [Stage 2] for each group: memblock_alloc_try_nid(alloc_size, PAGE_SIZE, nid)
│   → Each group has physically contiguous memory on its NUMA node
│
├─ [Stage 3] base = min(all group base_addrs)
│   → Global base address established
│
├─ [Stage 4] gi->base_offset = gi->base_addr - base
│   → Relative offsets within the total memory region
│
├─ [Stage 5] for each (group, unit): memcpy(unit_addr, __per_cpu_load, static_size)
│   → Template initialized in every CPU's unit
│   → Remaining space zeroed
│
├─ [Stage 6] memblock_free() tail padding
│   → Reclaim over-allocated memory
│
├─ [Stage 7] pcpu_setup_first_chunk(ai, base)
│   → Sets pcpu_base_addr, pcpu_unit_offsets[], chunk management
│
└─ pcpu_free_alloc_info(ai)
    → Free the pcpu_alloc_info struct (no longer needed)
```

---

## Error Handling

```c
/* If any memblock_alloc fails: */
out_free_areas:
    for (group--, ...) {
        memblock_free(gi->base_addr, gi->alloc_size);
    }
    pcpu_free_alloc_info(ai);
    return rc;
```

Failures here cause `panic()` in the caller (`setup_per_cpu_areas()`).

---

## Alternative: `pcpu_page_first_chunk()`

There's a rarely-used alternative: `pcpu_page_first_chunk()` which allocates individual
pages per CPU unit (not contiguous). It's used on architectures where contiguous
physical memory is scarce or where per-page control is needed. Neither ARM32 nor ARM64
uses it in the default configuration.

---

## Interview Quick Facts

| Question | Answer |
|---|---|
| How many stages? | 7: build_info, alloc-per-group, find-base, compute-offsets, memcpy, free-tail, setup_chunk |
| What is `atom_size`? | Allocation granularity, `PAGE_SIZE` for `pcpu_embed_first_chunk` |
| Why is NUMA grouping done? | Allocate each group from its local node's memory |
| What does Stage 5 (memcpy) copy? | `.data..percpu` template from `__per_cpu_load` |
| Why free tail padding (Stage 6)? | Over-allocation due to upa rounding — reclaim unused units |
| What happens on Stage 7 failure? | memblock_free all groups, return -errno, caller panics |
| Alternative to this function? | `pcpu_page_first_chunk()` — per-page allocation |
