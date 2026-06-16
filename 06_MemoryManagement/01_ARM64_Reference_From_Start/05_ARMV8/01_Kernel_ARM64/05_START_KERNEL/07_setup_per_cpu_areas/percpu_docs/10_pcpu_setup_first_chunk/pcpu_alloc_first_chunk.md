# `pcpu_alloc_first_chunk()` — Chunk Structure Allocation

## Source Reference
- `mm/percpu.c:1345` — function definition
- Called by: `pcpu_setup_first_chunk()` at `mm/percpu.c:~2750`

---

## Function Signature

```c
/* mm/percpu.c:1345 */
/**
 * pcpu_alloc_first_chunk - allocate the first percpu chunk
 * @tmp_addr: the temporary chunk address
 * @map_size: the size of temp chunk
 *
 * Allocate the first percpu chunk backed by @tmp_addr. The caller
 * is responsible for copying data to/from @tmp_addr during early
 * boot.
 *
 * Returns: the first percpu chunk
 */
static struct pcpu_chunk * __init
pcpu_alloc_first_chunk(unsigned long tmp_addr, int map_size)
```

---

## Purpose

`pcpu_alloc_first_chunk()` allocates the **management metadata** for the first chunk.
Note: it does NOT allocate the per-CPU data itself (that was done by `memblock_alloc`
in `pcpu_embed_first_chunk()`). It allocates:

1. The `struct pcpu_chunk` itself
2. The `alloc_map` bitmap (tracks allocation state of each slot)
3. The `bound_map` bitmap (tracks allocation boundaries)
4. The `md_blocks` array (per-page metadata blocks)

All allocated from `memblock_alloc()` (bootmem) since this runs during early boot.

---

## Step-by-Step Allocation

### Compute Sizes

```c
/* mm/percpu.c:~1360 */

/* Number of allocation slots in the chunk */
/* Each slot = PCPU_MIN_ALLOC_SIZE (8) bytes of per-CPU data */
int map_size = pcpu_chunk_map_bits(chunk_size);
/*           = chunk_size / PCPU_MIN_ALLOC_SIZE */
/*           = dynamic_region_size / 8 */

/* alloc_map: 1 bit per 8-byte slot (1=used, 0=free) */
size_t alloc_map_size  = BITS_TO_LONGS(map_size) * sizeof(long);

/* bound_map: 1 bit per 8-byte slot (1=boundary) */
size_t bound_map_size  = BITS_TO_LONGS(map_size + 1) * sizeof(long);
/* +1 for sentinel bit at the end */

/* md_blocks: one struct pcpu_block_md per PAGE_SIZE of chunk */
int nr_pages           = PAGE_ALIGN(chunk_size) >> PAGE_SHIFT;
size_t md_blocks_size  = nr_pages * sizeof(struct pcpu_block_md);
```

### Allocate from memblock

```c
/* mm/percpu.c:~1380 */

/* Allocate the chunk struct itself */
chunk = memblock_alloc(sizeof(*chunk), SMP_CACHE_BYTES);
if (!chunk)
    panic("Failed to allocate first percpu chunk");

/* Allocate alloc_map */
chunk->alloc_map = memblock_alloc(alloc_map_size, SMP_CACHE_BYTES);

/* Allocate bound_map */
chunk->bound_map = memblock_alloc(bound_map_size, SMP_CACHE_BYTES);

/* Allocate md_blocks */
chunk->md_blocks = memblock_alloc(md_blocks_size, SMP_CACHE_BYTES);
```

### Initialize the Chunk

```c
/* mm/percpu.c:~1410 */

/* Set base address */
chunk->base_addr = (void *)(tmp_addr - pcpu_chunk_base_offset(ai));
/*
 * tmp_addr = pcpu_base_addr (the allocated memory base)
 * pcpu_chunk_base_offset = start_offset (where static section starts)
 * base_addr adjusted so that:
 *   base_addr + pcpu_unit_offsets[cpu] + var_offset = cpu's var address
 */

/* Mark chunk as immutable (static region cannot be freed) */
chunk->immutable = true;

/* Initialize alloc_map: mark static+reserved as allocated */
pcpu_bitmap_range_set_mask(chunk->alloc_map,
                           0,
                           (static_size + reserved_size) / PCPU_MIN_ALLOC_SIZE);

/* Initialize bound_map: set boundaries */
/* ... */

/* Set free_bytes to only the dynamic region */
chunk->free_bytes = dynamic_region_size;

/* Initialize md_blocks */
pcpu_init_md_blocks(chunk);

/* Set start/end offsets */
chunk->start_offset = 0;
chunk->end_offset   = 0;  /* or dynamic_start for first_chunk */

/* Not yet in any slot */
INIT_LIST_HEAD(&chunk->list);
```

---

## The `alloc_map` Bitmap in Detail

```
Chunk layout with unit_size = 548KB:
  [0     .. 512KB-1] = static region (immutable)
  [512KB .. 520KB-1] = reserved region (immutable for first chunk)
  [520KB .. 548KB-1] = dynamic region (28KB free)

alloc_map bits (1 bit per 8 bytes):
  bit 0  .. bit 65535: STATIC REGION    (all 1 = allocated/immutable)
  bit 65536 .. bit 66559: RESERVED       (all 1 = allocated)
  bit 66560 .. bit 70143: DYNAMIC        (all 0 = free)

  Total bits = 548KB / 8 = 70144 bits = 1096 × 64-bit longs
```

The `immutable` flag means the static and reserved bits are **never cleared**,
even if theoretically `pcpu_free()` is called on them.

---

## `struct pcpu_block_md` — Page Metadata

```c
struct pcpu_block_md {
    int     scan_hint;          /* scan hint for allocation search */
    int     scan_hint_start;    /* start position of scan_hint */
    int     contig_hint;        /* largest contiguous free range */
    int     contig_hint_start;  /* start of contig_hint */
    int     left_free;          /* free bits at the start of block */
    int     right_free;         /* free bits at the end of block */
    int     first_free;         /* index of the first free bit */
    int     nr_bits;            /* total bits in this block */
};
```

One `pcpu_block_md` per page (4KB) of the chunk. These are used to accelerate
allocation searches — instead of scanning all 70000+ bits, the allocator can
quickly find a page with enough contiguous free space.

---

## Why Use memblock (Not kmalloc)?

At the time `pcpu_alloc_first_chunk()` runs:
- `kmalloc()` is not yet initialized
- `slab_allocator_init()` hasn't been called
- The only available allocator is `memblock`

These chunk management structures are permanent — they're never freed (per-CPU
first chunk lives for the system's lifetime).

After `free_all_bootmem()` converts memblock to the buddy allocator, new dynamic
chunks use `kzalloc()` for their management structures.

---

## Interview Quick Facts

| Question | Answer |
|---|---|
| What does pcpu_alloc_first_chunk allocate? | struct pcpu_chunk, alloc_map, bound_map, md_blocks |
| Why from memblock? | kmalloc not available during early boot |
| What is immutable=true? | Static/reserved region bits cannot be cleared |
| What is alloc_map? | Bitmap: 1 bit per 8-byte slot, 1=used, 0=free |
| What is bound_map? | Bitmap marking allocation boundaries for merge/split |
| What is md_blocks? | Per-page metadata for fast contiguous free-space search |
| Is this where per-CPU DATA is allocated? | No — data was allocated by memblock in pcpu_embed_first_chunk |
