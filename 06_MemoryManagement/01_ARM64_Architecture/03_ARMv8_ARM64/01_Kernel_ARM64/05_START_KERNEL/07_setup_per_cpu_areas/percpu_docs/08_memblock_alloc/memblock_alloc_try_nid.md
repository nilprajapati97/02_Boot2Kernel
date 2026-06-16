# `memblock_alloc_try_nid()` — Bootmem Allocation for Per-CPU

## Source Reference
- `mm/memblock.c` — implementation
- `include/linux/memblock.h` — declaration
- Called from: `pcpu_embed_first_chunk()` at `mm/percpu.c:~3110`

---

## What is memblock?

`memblock` is Linux's **early boot memory allocator** — it manages physical memory
before the full page allocator (`buddy system` / `kmalloc`) is initialized.

```
Boot memory timeline:
  Bootloader → head.S → setup_arch()
     │
     ├─ memblock_add()          ← Register physical memory banks
     ├─ memblock_reserve()      ← Reserve kernel image, DTB, initrd, etc.
     │
     ├─ paging_init()           ← Page tables created, virtual memory active
     │
     ├─ setup_per_cpu_areas()   ← Uses memblock to allocate per-CPU areas
     │   └─ memblock_alloc_try_nid()  ← WE ARE HERE
     │
     └─ free_all_bootmem()      ← Converts memblock to buddy allocator
        (After this, memblock is no longer used)
```

Per-CPU areas are allocated from `memblock` because:
- This happens before `kmalloc()` / `vmalloc()` work
- `memblock` can allocate from specific NUMA nodes
- Physically contiguous allocation is guaranteed

---

## Function Signature

```c
/* include/linux/memblock.h */
phys_addr_t __init memblock_alloc_try_nid(
    phys_addr_t size,
    phys_addr_t align,
    phys_addr_t min_addr,
    phys_addr_t max_addr,
    int nid);

/* Returns: physical address on success, 0 on failure */
```

The virtual address equivalent (used by pcpu_embed_first_chunk):
```c
void *memblock_alloc_try_nid(size_t size, size_t align,
                              phys_addr_t min_addr, phys_addr_t max_addr,
                              int nid);
/* Returns: virtual address (kernel-mapped), NULL on failure */
```

---

## How pcpu_embed_first_chunk Calls It

```c
/* mm/percpu.c:~3110 */
for (group = 0; group < ai->nr_groups; group++) {
    gi = &ai->groups[group];

    ptr = memblock_alloc_try_nid(
        gi->alloc_size,               /* WHAT: size of entire group's units */
        PAGE_SIZE,                     /* HOW: page-aligned */
        __pa(MAX_DMA_ADDRESS),        /* MIN: avoid DMA zone if possible */
        MEMBLOCK_ALLOC_ACCESSIBLE,    /* MAX: any accessible memory */
        cpu_to_node(group_cpu)        /* WHERE: prefer this NUMA node */
    );
```

### Parameter Breakdown

| Parameter | Value | Meaning |
|---|---|---|
| `size` | `gi->alloc_size` | `unit_size * upa` — entire group's memory at once |
| `align` | `PAGE_SIZE` (4KB) | Allocation must start at page boundary |
| `min_addr` | `__pa(MAX_DMA_ADDRESS)` | Try to avoid DMA zone (kernel data above DMA) |
| `max_addr` | `MEMBLOCK_ALLOC_ACCESSIBLE` | Any accessible memory (fallback) |
| `nid` | `cpu_to_node(group_cpu)` | Prefer NUMA node local to this group's CPUs |

### What `try_nid` Means

The `try_nid` variant:
1. First attempts to allocate from node `nid` (local NUMA node for this group)
2. If that fails (not enough contiguous memory on that node), falls back to `NUMA_NO_NODE`
   (any available memory)

This "try" semantics ensures:
- **Best case**: Local NUMA allocation → lowest access latency for per-CPU data
- **Fallback**: Cross-NUMA allocation → works correctly but slightly higher latency

---

## What Happens to the Allocated Memory

```
Before memcpy (template copy):
  Allocated memory contains: garbage / zeroed (memblock zeroes by default)
  Physical address range: [P, P + alloc_size)
  Virtual address: kernel direct mapping (kimage voffset translation)

After memcpy (Stage 5 in pcpu_embed_first_chunk):
  For each real CPU unit:
    [unit_base, unit_base + static_size) = copy of .data..percpu template
    [unit_base + static_size, unit_base + unit_size) = zeroed

  For padding units (no real CPU):
    [unit_base, unit_base + unit_size) = zeroed
```

---

## ARM32 Physical Memory Constraints

ARM32 without LPAE (Large Physical Address Extension):
- Physical memory limited to 4GB (32-bit physical addresses)
- Per-CPU areas allocated from first 4GB
- `MAX_DMA_ADDRESS = PHYS_OFFSET + 0x10000000` (often 256MB from start)
- Per-CPU memory preferably above DMA zone to avoid competing with DMA allocations

ARM32 with LPAE:
- Physical memory can extend to 40 bits (1TB)
- `phys_addr_t` is 64-bit
- Per-CPU still allocated in first accessible physical region for simplicity

---

## ARM64 Physical Memory Considerations

ARM64 systems may have:
- Scattered physical memory ranges (memblock holes)
- NUMA nodes with different physical address ranges
- Memory above 4GB (all ARM64 uses 64-bit physical addresses)

`memblock_alloc_try_nid` on ARM64:
```c
/* Max address is system-defined, typically full physical address space */
/* min_addr = __pa(MAX_DMA_ADDRESS) ensures per-CPU is not in DMA zone */
/* For large ARM64 servers: per-CPU may be allocated at very high addresses */
```

---

## memblock Internal Operation

```
memblock.memory: list of available physical memory regions
  [0x40000000, 0x80000000): 1GB DRAM (Node 0)
  [0x100000000, 0x140000000): 1GB DRAM (Node 1)

memblock.reserved: list of already-reserved regions
  [0x40008000, 0x40800000): kernel image
  [0x40800000, 0x40880000): .data..percpu template

memblock_alloc_try_nid(alloc_size=256KB, PAGE_SIZE, ..., nid=0):
  → Scans memblock.memory for a free region on Node 0
  → Finds: 0x42000000 - 0x42040000 (256KB free on Node 0, above DMA)
  → Marks it in memblock.reserved
  → Returns: 0xC2000000 (virtual = physical + PAGE_OFFSET on ARM32)
             or phys_to_virt(0x42000000)
```

---

## Failure Handling

```c
/* pcpu_embed_first_chunk handles memblock failure: */
if (!ptr) {
    rc = -ENOMEM;
    goto out_free_areas;  /* free already-allocated groups */
}
```

In practice, memblock allocation at early boot almost never fails because:
- Bootmem is not yet fragmented (no page allocator operations yet)
- The per-CPU footprint (typically a few MB total) is small vs. system RAM

If it does fail → `setup_per_cpu_areas()` calls `panic()` → kernel halts.

---

## Interview Quick Facts

| Question | Answer |
|---|---|
| What is memblock? | Boot-time physical memory allocator, precedes buddy system |
| Why use memblock for per-CPU? | kmalloc not yet available; need contiguous physical pages |
| What does `try_nid` mean? | Try specified NUMA node first, fall back to any node |
| What alignment is used? | `PAGE_SIZE` (4KB on ARM32/ARM64) |
| When is memblock freed? | `free_all_bootmem()` after `start_kernel()` completes |
| What if allocation fails? | `pcpu_embed_first_chunk` returns `-ENOMEM` → caller panics |
| Is allocated memory zeroed? | Yes — memblock zeros allocations by default |
