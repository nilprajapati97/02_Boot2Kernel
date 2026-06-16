# `pcpu_setup_first_chunk()` — Chunk Infrastructure Initialization

## Source Reference
- `mm/percpu.c:2608` — function definition
- Called by: `pcpu_embed_first_chunk()` at `mm/percpu.c:~3165`

---

## Function Signature

```c
/* mm/percpu.c:2608 */
/**
 * pcpu_setup_first_chunk - initialize the first percpu chunk
 * @ai: pcpu_alloc_info describing how the first chunk should be set up
 * @base_addr: mapped address of the first chunk
 *
 * Initialize the first percpu chunk which contains the kernel static
 * percpu area. This function is to be called from arch percpu area
 * setup path.
 *
 * @ai contains all information necessary to initialize the first
 * chunk and its layout.
 *
 * Returns 0 on success, -errno on failure.
 */
int __init pcpu_setup_first_chunk(const struct pcpu_alloc_info *ai,
                                   void *base_addr)
```

---

## What This Function Does

`pcpu_setup_first_chunk()` is the **accounting and bookkeeping** layer. After
`pcpu_embed_first_chunk()` has:
1. Allocated physical memory
2. Copied the template into each CPU's unit

...`pcpu_setup_first_chunk()` must:
1. Compute per-CPU unit offsets
2. Set the global `pcpu_base_addr`
3. Allocate and initialize chunk management structures
4. Set up the free-list slot arrays
5. Create the `pcpu_reserved_chunk` (static + reserved)
6. Create the `pcpu_first_chunk` (dynamic region)

---

## Step 1: Compute `pcpu_unit_offsets[]`

```c
/* mm/percpu.c:~2650 */
pcpu_unit_offsets = memblock_alloc(nr_cpu_ids * sizeof(pcpu_unit_offsets[0]),
                                    SMP_CACHE_BYTES);

for_each_possible_cpu(cpu) {
    /* Find which group and unit_index this CPU belongs to */
    int group = cpu_to_group(ai, cpu);
    int unit  = cpu_to_unit_index(ai, group, cpu);

    pcpu_unit_offsets[cpu] = ai->groups[group].base_offset
                           + unit * ai->unit_size;
    /*
     * pcpu_unit_offsets[cpu] = offset from pcpu_base_addr to this CPU's unit
     *
     * Example for 4-CPU system with unit_size = 0x90000:
     *   pcpu_unit_offsets[0] = 0x000000  (group0, unit 0)
     *   pcpu_unit_offsets[1] = 0x090000  (group0, unit 1)
     *   pcpu_unit_offsets[2] = 0x120000  (group0, unit 2)
     *   pcpu_unit_offsets[3] = 0x1B0000  (group0, unit 3)
     */
}
```

---

## Step 2: Set `pcpu_base_addr` and Global Variables

```c
/* mm/percpu.c:~2660 */
pcpu_base_addr     = base_addr;   /* = min(all group alloc addresses) */
pcpu_unit_size     = ai->unit_size;
pcpu_nr_units      = ai->nr_groups * max_nr_units_per_group;
pcpu_atom_size     = ai->atom_size;
pcpu_chunk_struct_size = ...;     /* sizeof struct pcpu_chunk + bitmap sizes */
```

`pcpu_base_addr` is the crucial global — it's used in `setup_per_cpu_areas()` to
compute the delta for `__per_cpu_offset[]`.

---

## Step 3: Initialize `pcpu_slot[]` — The Free-List Array

```c
/* mm/percpu.c:~2700 */
/*
 * Chunks are organized in a slot array where each slot holds chunks
 * with similar amounts of free space.
 *
 * Slot index = floor(log2(free_bytes)) - PCPU_SLOT_BASE_SHIFT + 1
 * where PCPU_SLOT_BASE_SHIFT = 5
 *
 * Slot 0: chunks with 0 free bytes
 * Slot 1: chunks with 1-31 free bytes
 * Slot 2: chunks with 32-63 free bytes
 * ...
 * Slot N: chunks with >= 2^(N+4) free bytes
 */
pcpu_nr_slots = __pcpu_size_to_slot(pcpu_unit_size) + 2;
pcpu_slot = memblock_alloc(pcpu_nr_slots * sizeof(pcpu_slot[0]),
                            SMP_CACHE_BYTES);

for (i = 0; i < pcpu_nr_slots; i++)
    INIT_LIST_HEAD(&pcpu_slot[i]);
```

---

## Step 4: Fix `cpu_number` Per-CPU Variable

```c
/* mm/percpu.c:~2710 */
for_each_possible_cpu(cpu) {
    per_cpu(cpu_number, cpu) = cpu;
}
/*
 * After template copy, all units have cpu_number = 0 (template value).
 * This loop corrects each unit's cpu_number to match its actual CPU ID.
 */
```

---

## Step 5: Create `pcpu_alloc_first_chunk()` — Chunk Struct

```c
/* mm/percpu.c:~2750 */
chunk = pcpu_alloc_first_chunk(ai, base_addr);
```

See `pcpu_alloc_first_chunk.md` for details. This creates the `struct pcpu_chunk`
that manages the allocated per-CPU memory.

---

## Step 6: Split Into Reserved and First Chunks

```c
/* mm/percpu.c:~2760 */

/*
 * The static and reserved regions are managed by pcpu_reserved_chunk.
 * Allocations from the reserved region are possible for modules.
 *
 * The dynamic region is managed by pcpu_first_chunk.
 * This is the primary source for alloc_percpu() requests.
 */

/* Reserved chunk covers: [0, static_size + reserved_size) */
pcpu_reserved_chunk = chunk;
pcpu_reserved_chunk->end_offset = static_size + reserved_size;

/* First chunk covers: [static_size + reserved_size, unit_size) */
/* (the dynamic region) */
pcpu_first_chunk = chunk;  /* actually this is more complex — see below */
```

In the actual implementation, there's one `struct pcpu_chunk` but it's configured
with `start_offset` and `end_offset` to represent the correct region for each pointer.
The `pcpu_reserved_chunk` points to the reserved region's management, and
`pcpu_first_chunk` points to the dynamic region.

---

## Step 7: Place First Chunk in Slot List

```c
/* mm/percpu.c:~2780 */
pcpu_chunk_relocate(pcpu_first_chunk, -1);
/*
 * -1 as old_slot means "not currently in any slot"
 * This places pcpu_first_chunk in the correct pcpu_slot[] based on free_bytes
 *
 * free_bytes = unit_size - static_size - reserved_size
 *            = dynamic region size (e.g., 20KB+)
 */
```

---

## Global State After `pcpu_setup_first_chunk()` Returns

```
pcpu_base_addr          = base_addr (lowest address of per-CPU memory)
pcpu_unit_offsets[cpu]  = offset of CPU N's unit from pcpu_base_addr
pcpu_unit_size          = unit_size (all units equal size)
pcpu_nr_units           = total number of units (includes padding)
pcpu_slot[]             = initialized free-list array
pcpu_reserved_chunk     = chunk managing static + reserved regions
pcpu_first_chunk        = chunk managing dynamic region

cpu_number[cpu]         = cpu  (per-CPU variable, corrected in all units)
```

---

## Data Flow Summary

```
Input (from pcpu_embed_first_chunk):
  ai->static_size, ai->unit_size, ai->nr_groups, ai->groups[]
  base_addr = lowest address of all group allocations

Processing in pcpu_setup_first_chunk:
  → pcpu_unit_offsets[cpu] = group[g].base_offset + unit_index * unit_size
  → pcpu_base_addr = base_addr
  → pcpu_slot[] initialized
  → pcpu_alloc_first_chunk() creates struct pcpu_chunk
  → pcpu_chunk_relocate() places chunk in slot list

Output (consumed by setup_per_cpu_areas):
  pcpu_base_addr          ← used to compute delta
  pcpu_unit_offsets[cpu]  ← added to delta to get __per_cpu_offset[cpu]
```

---

## Interview Quick Facts

| Question | Answer |
|---|---|
| What does pcpu_setup_first_chunk do? | Initializes all chunk management infrastructure |
| What is pcpu_base_addr? | Lowest virtual address of per-CPU memory (global) |
| What is pcpu_unit_offsets[cpu]? | Offset of CPU N's unit from pcpu_base_addr |
| What is pcpu_slot[]? | Array of free-list heads indexed by free-size log2 |
| What are the two chunk pointers set? | pcpu_reserved_chunk, pcpu_first_chunk |
| Why is cpu_number fixed here? | Template copy gives all units cpu_number=0; must set each CPU's value |
| When is this function called? | At end of pcpu_embed_first_chunk, before free_alloc_info |
