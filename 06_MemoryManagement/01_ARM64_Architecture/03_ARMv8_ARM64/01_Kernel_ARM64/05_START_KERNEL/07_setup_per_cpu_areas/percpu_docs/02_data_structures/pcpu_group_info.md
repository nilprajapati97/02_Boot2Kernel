# `struct pcpu_group_info` — NUMA Group Descriptor

## Source Reference
- `include/linux/percpu.h` — struct definition
- `mm/percpu.c:2864` — built by `pcpu_build_alloc_info()`
- `mm/percpu.c:3075` — used by `pcpu_embed_first_chunk()` for memblock alloc

---

## Struct Definition

```c
/* include/linux/percpu.h */
struct pcpu_group_info {
    int         nr_units;       /* number of CPU units in this group */
    unsigned long base_offset;  /* byte offset from pcpu_base_addr to this group */
    unsigned int *cpu_map;      /* array: cpu_map[unit_index] = cpu_id */
};
```

---

## Role in the Per-CPU System

`pcpu_group_info` represents **one NUMA node's worth of CPUs** in the per-CPU layout.
The entire per-CPU memory area is divided into groups, where each group's memory is
allocated from memblock on the NUMA node that the group's CPUs are "local to".

This optimization ensures:
- CPU 0's per-CPU data is on NUMA node 0's memory (local access, fast)
- CPU 4's per-CPU data is on NUMA node 1's memory (if it's a node-1 CPU)

On non-NUMA systems, there is **always exactly 1 group** containing all CPUs.

---

## Field-by-Field Explanation

### `nr_units`

Number of CPU "slots" allocated for this group. Note: `nr_units` may be slightly larger
than the actual number of CPUs in the group due to alignment requirements:

```c
/* pcpu_build_alloc_info() ensures: */
nr_units = ALIGN(actual_cpu_count, upa);  /* upa = units per alloc */
```

If `upa = 4` but the group has 3 CPUs, `nr_units = 4`. The extra unit is still
allocated (wastes memory) but its `cpu_map` entry is set to `NR_CPUS` (invalid),
indicating it has no associated CPU.

### `base_offset`

After `pcpu_embed_first_chunk()` calls `memblock_alloc()` for each group, it finds
the minimum base address across all groups:
```c
/* mm/percpu.c ~3130 */
base = min(base, (void *)ai->groups[i].base_offset);
```

Then for each group:
```c
ai->groups[i].base_offset = groups[i].alloc_addr - base;
```

So `base_offset` is the byte offset from `pcpu_base_addr` (the global base) to the
start of this group's allocated memory. It is used in `pcpu_setup_first_chunk()` to
compute `pcpu_unit_offsets[cpu]`.

### `cpu_map`

Maps from unit index (within this group) to global CPU ID:

```c
/* Example: Group 0 has CPUs 0,1,2,3 */
cpu_map[0] = 0;
cpu_map[1] = 1;
cpu_map[2] = 2;
cpu_map[3] = 3;

/* Example: Group 1 has CPUs 4,5 (with upa=4, padding with NR_CPUS) */
cpu_map[0] = 4;
cpu_map[1] = 5;
cpu_map[2] = NR_CPUS;  /* padding unit, no CPU */
cpu_map[3] = NR_CPUS;  /* padding unit, no CPU */
```

The `cpu_map` array is allocated contiguously after the `pcpu_group_info` array within
the `pcpu_alloc_info` struct's allocation.

---

## How Groups Are Built

`pcpu_build_alloc_info()` at `mm/percpu.c:2864` builds the group structure:

### Step 1: Try single group

First attempt: put all CPUs in one group with `upa` chosen to hit ≥75% utilization.

```c
/* mm/percpu.c ~2910 */
static_size = ALIGN(__per_cpu_end - __per_cpu_start, PCPU_MIN_ALLOC_SIZE);
reserved_size = ALIGN(reserved_size, PCPU_MIN_ALLOC_SIZE);
dyn_size = ALIGN(dyn_size, PCPU_MIN_ALLOC_SIZE);
min_unit_size = max_t(size_t, static_size + reserved_size + dyn_size,
                      PCPU_MIN_UNIT_SIZE);
```

### Step 2: Determine units per alloc (upa)

```c
/* Find upa such that (nr_cpus * unit_size) / (upa * alloc_size) >= 0.75 */
/* i.e., fill factor >= 75% to avoid excessive memory waste */
```

### Step 3: Group by NUMA distance

If a single group produces poor NUMA locality (CPUs getting remote memory), the
algorithm splits into multiple groups:

```c
/* Pseudo-code of pcpu_build_alloc_info */
for_each_possible_cpu(cpu) {
    nid = cpu_to_node(cpu);
    /* Find existing group for this nid, or create new group */
    group = find_or_create_group(nid);
    group->cpu_map[group->nr_units++] = cpu;
}
```

---

## Memory Layout Diagram

### Single-group system (4 CPUs, non-NUMA):

```
pcpu_base_addr
     │
     ▼
┌────────────────────────────────────────────────────────────────┐
│  GROUP 0  (base_offset = 0)                                    │
│  ┌──────────┬──────────┬──────────┬──────────┐                │
│  │  CPU 0   │  CPU 1   │  CPU 2   │  CPU 3   │                │
│  │  unit    │  unit    │  unit    │  unit    │                │
│  │ (static) │ (static) │ (static) │ (static) │                │
│  │ (reserv) │ (reserv) │ (reserv) │ (reserv) │                │
│  │  (dyn)   │  (dyn)   │  (dyn)   │  (dyn)   │                │
│  └──────────┴──────────┴──────────┴──────────┘                │
└────────────────────────────────────────────────────────────────┘
  ←─── alloc_size = unit_size * upa ───────────────────────────→
```

### Two-group system (4+4 CPUs, NUMA nodes 0 and 1):

```
                     ← Node 0 memory →        ← Node 1 memory →
pcpu_base_addr
     │
     ▼
┌──────────────────────────┐  ┌──────────────────────────┐
│  GROUP 0 (base_off=0)    │  │  GROUP 1 (base_off=X)    │
│  CPUs: 0,1,2,3           │  │  CPUs: 4,5,6,7           │
│  Allocated from Node 0   │  │  Allocated from Node 1   │
└──────────────────────────┘  └──────────────────────────┘
                                ^
                                │
                       base_offset = group1.alloc_addr - pcpu_base_addr
```

---

## Interview Quick Facts

| Question | Answer |
|---|---|
| What is a group? | Set of CPUs with the same NUMA node locality |
| How many groups on non-NUMA? | Always 1 |
| What is `cpu_map`? | Array mapping unit index to global CPU ID |
| What is `NR_CPUS` in cpu_map? | Padding unit — no real CPU assigned |
| Why split into groups? | Allocate each group from its NUMA node's memory |
| When is base_offset set? | After memblock_alloc in pcpu_embed_first_chunk |
