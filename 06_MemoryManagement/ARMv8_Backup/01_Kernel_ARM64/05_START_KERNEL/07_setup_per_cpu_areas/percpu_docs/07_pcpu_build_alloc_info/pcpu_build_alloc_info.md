# `pcpu_build_alloc_info()` — NUMA Group Construction

## Source Reference
- `mm/percpu.c:2864` — function definition
- Called by: `pcpu_embed_first_chunk()` at `mm/percpu.c:~3090`

---

## Function Signature

```c
/* mm/percpu.c:2864 */
/**
 * pcpu_build_alloc_info - build alloc_info considering distances between CPUs
 * @reserved_size: the size of reserved percpu area in bytes
 * @dyn_size: minimum free size for dynamic allocation in bytes
 * @atom_size: allocation atom size
 * @cpu_distance_fn: callback to determine distance between cpus, optional
 *
 * ...
 *
 * Returns pointer to the new allocation info on success, ERR_PTR values
 * on failure.
 */
static struct pcpu_alloc_info * __init pcpu_build_alloc_info(
                size_t reserved_size, size_t dyn_size,
                size_t atom_size,
                pcpu_fc_cpu_distance_fn_t cpu_distance_fn)
```

---

## Algorithm Overview

`pcpu_build_alloc_info()` solves two problems simultaneously:
1. **Sizing**: How large should each CPU unit be?
2. **Grouping**: Which CPUs should share a single contiguous memory allocation?

---

## Step 1: Compute `static_size` and `min_unit_size`

```c
/* mm/percpu.c:~2890 */
static_size = ALIGN(__per_cpu_end - __per_cpu_start, PCPU_MIN_ALLOC_SIZE);
/* PCPU_MIN_ALLOC_SIZE = 8 bytes */

/* Align reserved and dynamic sizes */
reserved_size = ALIGN(reserved_size, PCPU_MIN_ALLOC_SIZE);
dyn_size      = ALIGN(dyn_size,      PCPU_MIN_ALLOC_SIZE);

/*
 * Determine the minimum unit size.
 * Must hold: static + reserved + dynamic space
 * Must be at least PCPU_MIN_UNIT_SIZE (32KB) for runtime allocations
 */
min_unit_size = max_t(size_t, static_size + reserved_size + dyn_size,
                      PCPU_MIN_UNIT_SIZE);
```

**Example**:
```
__per_cpu_end - __per_cpu_start = 0x80000 (512KB)
static_size   = ALIGN(512KB, 8) = 512KB (already aligned)
reserved_size = ALIGN(8KB, 8)   = 8KB
dyn_size      = ALIGN(20KB, 8)  = 20KB
min_unit_size = max(512+8+20 KB, 32KB) = 540KB
```

---

## Step 2: Determine Optimal `upa` (Units Per Alloc)

```c
/* mm/percpu.c:~2910 */
/*
 * Determine optimal upa such that:
 *   (nr_cpus * unit_size) / (alloc_size)  >= 0.75
 * where alloc_size = upa * atom_size
 *
 * This ensures ≥75% memory utilization (wastes ≤25%)
 */

/* First, try upa = 1 (one unit per allocation) */
/* Then increase upa if the fill ratio is acceptable */

alloc_size = PAGE_SIZE;  /* start with atom_size */
upa = alloc_size / unit_size;
if (upa == 0) {
    /* unit_size > PAGE_SIZE — use multi-page allocation */
    alloc_size = ALIGN(unit_size, PAGE_SIZE);
    upa = 1;
}

/* Try to pack more units per allocation while maintaining >75% fill */
/* The fill ratio formula:
 *   fill = (nr_units_used * unit_size) / (nr_allocs * alloc_size)
 *        = nr_cpus * unit_size / (ALIGN(nr_cpus, upa) * unit_size)
 *        = nr_cpus / ALIGN(nr_cpus, upa)
 * We want: nr_cpus / ALIGN(nr_cpus, upa) >= 0.75
 */
```

**Why 75%?** The kernel accepts up to 25% memory waste from padding units. For a
4-CPU system with `upa = 4`, the allocation is exactly 4 units — 100% fill. For
a 3-CPU system with `upa = 4`, the allocation holds 4 units but only 3 are used —
75% fill, still acceptable.

---

## Step 3: Build CPU Groups by NUMA Distance

```c
/* mm/percpu.c:~2945 */

/* Arrays to track group membership */
int group_map[NR_CPUS];        /* group_map[cpu] = group index for this CPU */
int group_cnt[NR_CPUS];        /* group_cnt[g] = number of CPUs in group g */
int nr_groups = 1;             /* start with 1 group */

for_each_possible_cpu(cpu)
    group_map[cpu] = 0;        /* initially all CPUs in group 0 */

/* If cpu_distance_fn provided, try to merge distant CPUs into groups */
/* Two CPUs belong to the same group if they are LOCAL_DISTANCE apart */

for_each_possible_cpu(cpu) {
    for_each_possible_cpu(cpu2) {
        if (cpu >= cpu2)
            continue;
        dist = cpu_distance_fn ? cpu_distance_fn(cpu, cpu2) : LOCAL_DISTANCE;
        if (dist > LOCAL_DISTANCE && can_split_groups) {
            /* Put cpu and cpu2 in different groups if fill ratio allows */
            /* ... */
        }
    }
}
```

For ARM32 and ARM64 with `cpu_distance_fn = NULL` (default in `setup_per_cpu_areas()`):
```c
/* NULL distance function → single group containing all CPUs */
nr_groups = 1;
group_map[cpu] = 0;  /* all CPUs in group 0 */
```

---

## Step 4: Build the `pcpu_alloc_info` Struct

```c
/* mm/percpu.c:~2970 */

/* Allocate the info struct from memblock */
ai = pcpu_alloc_info_alloc(nr_groups, nr_cpu_ids);

/* Fill in sizes */
ai->static_size   = static_size;
ai->reserved_size = reserved_size;
ai->dyn_size      = dyn_size;
ai->unit_size     = alloc_size / upa;    /* per-unit size */
ai->atom_size     = atom_size;           /* PAGE_SIZE */
ai->alloc_size    = alloc_size;          /* upa * unit_size */

/* Fill in groups */
for (group = 0; group < nr_groups; group++) {
    gi = &ai->groups[group];
    gi->nr_units = roundup(group_cnt[group], upa);  /* round up to upa multiple */
    /* fill gi->cpu_map[] with CPUs in this group, then NR_CPUS for padding */
}

ai->nr_groups = nr_groups;
```

---

## The 75% Utilization Check — Detailed Example

Scenario: 6 CPUs, `unit_size = 32KB`, `atom_size = 4KB (PAGE_SIZE)`.

```
Try upa = 1: alloc_size = 32KB = unit_size
  Fill ratio = 1 (100% — all units used)
  Works but makes 6 separate allocations

Try upa = 2: alloc_size = 64KB
  6 CPUs / roundup(6, 2) = 6/6 = 100% → OK

Try upa = 4: alloc_size = 128KB
  6 CPUs / roundup(6, 4) = 6/8 = 75% → OK (exactly at threshold)

Try upa = 8: alloc_size = 256KB
  6 CPUs / roundup(6, 8) = 6/8 = 75% → OK
  But upa=8 means all 6 CPUs in one alloc_size=256KB, 6 units used, 2 padding

Kernel picks the upa that minimizes nr_allocs while staying above 75%.
For 6 CPUs: best_upa = 8 (one allocation of 256KB, 75% fill)
```

---

## `pcpu_alloc_info_alloc()` — Internal Allocator

```c
/* mm/percpu.c:~2850 */
static struct pcpu_alloc_info * __init pcpu_alloc_info_alloc(int nr_groups,
                                                              int nr_cpu_ids)
{
    /* Compute total size needed */
    size_t ai_size = sizeof(struct pcpu_alloc_info)
                   + nr_groups * sizeof(struct pcpu_group_info);
    size_t cpu_map_size = nr_cpu_ids * sizeof(int);

    /* The cpu_map[] arrays are packed after the group_info array */
    void *ptr = memblock_alloc(ai_size + nr_groups * cpu_map_size, SMP_CACHE_BYTES);
    /* ... set up pointers ... */
    return ptr;
}
```

---

## ARM32 / ARM64 Specifics

Both ARM32 and ARM64 call `pcpu_embed_first_chunk(NULL, NULL)` — no `cpu_distance_fn`.
Therefore `pcpu_build_alloc_info()` always produces:

```
nr_groups = 1
groups[0].nr_units = nr_possible_cpus (or padded to upa multiple)
groups[0].cpu_map[] = {0, 1, 2, 3, ..., NR_CPUS (padding)}
```

For NUMA ARM64 servers, the platform-specific `cpu_distance_fn` can be provided by
architecture code via a more specialized call to `pcpu_setup_first_chunk()`. But the
default ARM64 path via `mm/percpu.c:3383` uses `NULL` and gets single-group allocation.

---

## Interview Quick Facts

| Question | Answer |
|---|---|
| What is `upa`? | Units per alloc — how many CPU units fit in one memblock allocation |
| What is the fill ratio threshold? | ≥75% (allows ≤25% padding waste) |
| What does `cpu_distance_fn=NULL` mean? | Single group, no NUMA-awareness |
| What linker symbols determine static_size? | `__per_cpu_end - __per_cpu_start` |
| What is `PCPU_MIN_UNIT_SIZE`? | 32KB — minimum unit to ensure dynamic alloc headroom |
| How is the alloc_info freed? | `pcpu_free_alloc_info()` via `memblock_free()` after setup_first_chunk |
