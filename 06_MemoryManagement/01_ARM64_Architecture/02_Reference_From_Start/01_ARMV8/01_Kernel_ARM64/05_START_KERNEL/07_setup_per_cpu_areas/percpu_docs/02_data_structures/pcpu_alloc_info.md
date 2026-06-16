# `struct pcpu_alloc_info` — Per-CPU Allocation Descriptor

## Source Reference
- `include/linux/percpu.h` — struct definition
- `mm/percpu.c:2864` — used in `pcpu_build_alloc_info()`
- `mm/percpu.c:3075` — passed to `pcpu_embed_first_chunk()`

---

## Struct Definition

```c
/* include/linux/percpu.h */
struct pcpu_alloc_info {
    size_t          static_size;    /* size of kernel .data..percpu section */
    size_t          reserved_size;  /* PERCPU_MODULE_RESERVE (module dynamic) */
    size_t          dyn_size;       /* PERCPU_DYNAMIC_RESERVE (alloc_percpu) */
    size_t          unit_size;      /* size of each CPU's unit (page-aligned) */
    size_t          atom_size;      /* allocation atom, usually PAGE_SIZE */
    size_t          alloc_size;     /* size of each group's allocation */
    size_t          __ai_size;      /* internal: total struct allocation */
    int             nr_groups;      /* number of NUMA groups */
    struct pcpu_group_info  groups[]; /* flexible array of group descriptors */
};
```

---

## Field-by-Field Explanation

### `static_size`
```
static_size = ALIGN(__per_cpu_end - __per_cpu_start, cache_line_size)
```
- Computed in `pcpu_build_alloc_info()` as the difference between the linker symbols
  that bracket the `.data..percpu` section.
- Represents the size of the **master template** that must be copied into every CPU unit.
- Always aligned to cache line size to prevent false sharing at unit boundaries.

**Typical value:** 256KB – 1MB depending on kernel configuration and drivers compiled in.

### `reserved_size`
```c
// mm/percpu.c:3383
pcpu_embed_first_chunk(PERCPU_MODULE_RESERVE, PERCPU_DYNAMIC_RESERVE, ...)
//                     ^^^^^^^^^^^^^^^^^^^^ this becomes reserved_size
```
- Space at the end of each unit reserved for **kernel module** per-CPU variables.
- Kernel modules compiled as `.ko` files need per-CPU space allocated **after** bootmem
  is gone, using a dedicated reserved pool in each unit.
- Default: `PERCPU_MODULE_RESERVE = 8 * 1024` (8KB, configured in `mm/percpu.c`).

### `dyn_size`
- Space for runtime `alloc_percpu()` / `__alloc_percpu()` calls.
- Default: `PERCPU_DYNAMIC_RESERVE = 20 * 1024` (20KB).
- This is the minimum; the actual dynamic region may be larger depending on unit_size
  rounding.

### `unit_size`
```c
/* from pcpu_embed_first_chunk, mm/percpu.c ~line 3100 */
unit_size = ALIGN(static_size + reserved_size + dyn_size, atom_size);
unit_size = max(unit_size, PCPU_MIN_UNIT_SIZE);
```
- Total size of one CPU's private area, page-aligned.
- `PCPU_MIN_UNIT_SIZE = 32 * 1024` (32KB) — ensures reasonable dynamic allocation.
- **All CPUs have the same unit_size** even if they belong to different NUMA nodes.

### `atom_size`
- Minimum allocation granularity. For `pcpu_embed_first_chunk()`, this is `PAGE_SIZE`.
- Groups are allocated in multiples of `atom_size * units_per_alloc`.

### `alloc_size`
```c
alloc_size = atom_size * upa;  /* upa = units per alloc */
```
- Size of each individual bootmem allocation (one per group, rounded up to page multiples).
- `upa` (units per alloc) is chosen in `pcpu_build_alloc_info()` to achieve ≥75% memory
  utilization while fitting within NUMA node proximity constraints.

### `nr_groups`
- Number of NUMA distance groups. On non-NUMA systems: always 1.
- On NUMA systems: CPUs are grouped by `LOCAL_DISTANCE` — CPUs that are "local" to each
  other (same NUMA node) share one group.
- Maximum: `nr_cpu_ids` (degenerate case where every CPU is in its own group).

### `groups[]`
- Flexible array of `struct pcpu_group_info` (see `pcpu_group_info.md`).
- One entry per NUMA group, each describing which CPUs belong and where their memory is.

---

## Memory Allocation for the Struct Itself

The struct (including the flexible `groups[]` array) is allocated at the beginning of
`pcpu_build_alloc_info()`:

```c
/* mm/percpu.c ~2880 */
ai = pcpu_alloc_info_alloc(nr_groups, nr_cpu_ids);
/*  allocates: sizeof(pcpu_alloc_info)
              + nr_groups * sizeof(pcpu_group_info)
              + nr_cpu_ids * sizeof(int)  [cpu_map]
    using memblock_alloc_raw()
*/
```

It is freed after `pcpu_setup_first_chunk()` completes:
```c
/* mm/percpu.c ~3170 */
pcpu_free_alloc_info(ai);  /* memblock_free() */
```

---

## Concrete Example: 4-CPU, Non-NUMA System

```
static_size   = 512 * 1024  = 0x80000
reserved_size =   8 * 1024  = 0x02000
dyn_size      =  20 * 1024  = 0x05000
                             ---------
raw sum       = 540 * 1024  = 0x87000
unit_size     = ALIGN(0x87000, 0x1000)  ← PAGE_SIZE aligned
              = 0x87000  (already page aligned)
              but must be >= PCPU_MIN_UNIT_SIZE (0x8000)
              → unit_size = 0x87000

nr_groups     = 1
groups[0]:
    nr_units  = 4
    base_offset = 0  (set later after memblock_alloc)
    cpu_map[] = { 0, 1, 2, 3 }
    alloc_size = unit_size * upa = 0x87000 * 4 = 0x21C000
```

---

## Interview Quick Facts

| Question | Answer |
|---|---|
| Where is struct allocated? | `memblock_alloc()` in `pcpu_build_alloc_info()` |
| When is it freed? | After `pcpu_setup_first_chunk()` returns |
| What determines nr_groups? | NUMA LOCAL_DISTANCE grouping of CPUs |
| Why does unit_size matter? | All CPUs must have identical unit_size for offset arithmetic |
| What's PCPU_MIN_UNIT_SIZE? | 32KB — ensures minimal dynamic per-CPU space |
