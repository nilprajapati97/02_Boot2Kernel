# `struct pcpu_chunk` — Per-CPU Chunk Descriptor

## Source Reference
- `mm/percpu-internal.h` — struct definition
- `mm/percpu.c:1345` — `pcpu_alloc_first_chunk()`
- `mm/percpu.c:555`  — `pcpu_chunk_relocate()`
- `mm/percpu.c:2608` — `pcpu_setup_first_chunk()` creates chunk pointers

---

## Struct Definition

```c
/* mm/percpu-internal.h */
struct pcpu_chunk {
    struct list_head    list;           /* linked into pcpu_slot[] free lists */
    int                 free_bytes;     /* bytes of free space in this chunk */
    struct pcpu_block_md chunk_md;      /* chunk-level metadata (largest free block) */
    unsigned long       *bound_map;     /* bitmap: allocation boundaries */
    unsigned long       *alloc_map;     /* bitmap: allocation state (free/used) */

    void                *base_addr;     /* virtual address of this chunk's data */
                                        /* base_addr + pcpu_unit_offsets[cpu] = cpu's data */

    int                 nr_pages;       /* number of populated pages in this chunk */
    int                 nr_populated;   /* number of pages currently mapped/populated */
    int                 nr_empty_pop_pages; /* populated but free pages (reclaimable) */
    unsigned long       *populated;     /* bitmap: which pages are populated */
    unsigned long       *vm_populated;  /* bitmap: which pages are vmalloc-mapped */

    int                 start_offset;   /* byte offset where actual data starts */
                                        /* (for alignment of static section) */
    int                 end_offset;     /* byte offset where actual data ends */

    struct pcpu_memblock_reserved *reserved;  /* NULL for dynamic chunks */
    bool                immutable;      /* if true: alloc_map cannot be modified */
                                        /* true only for pcpu_first_chunk */
    bool                isolated;       /* true if removed from active slot */
    int                 nr_alloc;       /* number of active allocations */
    struct obj_cgroup   **obj_cgroup;   /* cgroup accounting per allocation slot */
    struct list_head    map_extend_list;/* for deferred map extension */

    void                *data;          /* vmalloc-mapped backing data (non-first chunks) */
    bool                has_reserved;   /* reserved region present? */
    struct work_struct  work;           /* for async depopulation */
    int                 __percpu_ref_flags; /* reclaim/free state flags */
};
```

---

## The Two Chunk Types

### 1. First Chunk (`pcpu_first_chunk` / `pcpu_reserved_chunk`)

Created by `pcpu_alloc_first_chunk()` at `mm/percpu.c:1345`.

```
[pcpu_reserved_chunk] covers: static + reserved regions
[pcpu_first_chunk]    covers: dynamic region

Both are carved from the single contiguous per-CPU allocation made during boot.
```

Properties:
- `base_addr = pcpu_base_addr` — the start of the entire per-CPU memory area
- `immutable = true` — the static and reserved regions cannot be freed
- `data` is NULL — memory is physically contiguous bootmem (not vmalloc)
- Allocated via `memblock_alloc()`, never freed until the end of the system

### 2. Dynamic Chunks (added later by `pcpu_create_chunk()`)

Created on-demand when `alloc_percpu()` cannot satisfy a request from existing chunks.

Properties:
- `immutable = false`
- `data` points to `vmalloc()`-mapped backing pages
- Can be freed when all allocations within are released
- `nr_pages` is populated incrementally as allocations are made

---

## Key Fields Deep Dive

### `alloc_map` and `bound_map`

Per-CPU chunk space is managed by two bitmaps operating on units of `PCPU_MIN_ALLOC_SIZE`
bytes (typically 8 bytes — minimum allocation granularity):

```
Number of bits = (chunk_size / PCPU_MIN_ALLOC_SIZE)

alloc_map: 1 = used, 0 = free
           Tracks which "slots" are allocated

bound_map: 1 = start or end of an allocation (boundary)
           Used for efficient free/coalesce operations
```

**Example**: For a 4096-byte chunk with 8-byte minimum allocation:
- Each bitmap has 512 bits = 8 × 64-bit words

### `base_addr`

```c
/*
 * For CPU N:
 *   cpu_data_addr = chunk->base_addr + pcpu_unit_offsets[cpu_N]
 *
 * For a specific per-CPU variable 'var':
 *   var_for_cpu_N = cpu_data_addr + (offset of 'var' within unit)
 */
```

After `pcpu_setup_first_chunk()`:
```c
pcpu_base_addr = chunk->base_addr;  /* global, used for __per_cpu_offset computation */
```

### `free_bytes` and Slot Lists

Chunks are organized in `pcpu_slot[]` — an array of `struct list_head` where the index
corresponds to the amount of free space:

```c
/* mm/percpu.c */
#define PCPU_SLOT_BASE_SHIFT    5   /* 1<<5 = 32 bytes minimum slot increment */

/* Chunk with X free bytes goes into slot: */
slot = max(0, ilog2(free_bytes) - PCPU_SLOT_BASE_SHIFT + 1)
```

`pcpu_chunk_relocate()` is called after every alloc/free to move a chunk to the
correct slot. This allows `pcpu_alloc()` to quickly find a chunk with enough space
via an O(1) slot lookup.

---

## Lifecycle of a Chunk

```
pcpu_alloc_first_chunk() [mm/percpu.c:1345]
        │
        │  Allocates struct pcpu_chunk + alloc_map + bound_map + md_blocks
        │  via memblock_alloc() (bootmem)
        │  Sets immutable = true
        ▼
pcpu_setup_first_chunk() [mm/percpu.c:2608]
        │
        │  Splits: pcpu_reserved_chunk (static+reserved)
        │          pcpu_first_chunk (dynamic)
        │  Sets base_addr, free_bytes for pcpu_first_chunk
        ▼
pcpu_chunk_relocate() [mm/percpu.c:555]
        │
        │  Places pcpu_first_chunk in correct pcpu_slot[] based on free_bytes
        ▼
Runtime: pcpu_alloc() / pcpu_free()
        │
        │  alloc: find chunk in slots, update alloc_map/bound_map, relocate
        │  free:  clear bits in alloc_map, coalesce with neighbors, relocate
        ▼
(No deallocation for first chunk — lives for system lifetime)
```

---

## Interview Quick Facts

| Question | Answer |
|---|---|
| What bitmap manages free space? | `alloc_map` (used/free), `bound_map` (boundaries) |
| What is `immutable`? | Flag on first chunk preventing deallocation of static/reserved |
| How are chunks indexed for allocation? | `pcpu_slot[]` array indexed by free_bytes log2 |
| Where is `base_addr` used globally? | `pcpu_base_addr` → `__per_cpu_offset` computation |
| How does first chunk differ from dynamic? | Bootmem-backed, immutable, no vmalloc mapping |
| What is minimum allocation size? | `PCPU_MIN_ALLOC_SIZE` = 8 bytes |
