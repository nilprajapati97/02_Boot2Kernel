# Memblock Internals — The Boot-Time Memory Allocator

**Source:** `mm/memblock.c`, `include/linux/memblock.h`
**Phase:** Memblock Era (used from DTB parsing until `memblock_free_all()`)
**Purpose:** Track and allocate physical memory before the buddy allocator exists

---

## What Memblock Is

Memblock is the **simplest possible memory allocator** — it maintains two sorted arrays of memory regions:

1. **`memblock.memory`** — all physical RAM in the system
2. **`memblock.reserved`** — regions that are "in use" (kernel, DTB, page tables, etc.)

**Free memory = memory regions that are NOT in reserved.**

There are no free lists, no buddy system, no slab — just two arrays and simple linear scans.

---

## Data Structures

### `struct memblock` — The Global State

```c
struct memblock {
    bool bottom_up;                    // Allocation direction (default: top-down)
    phys_addr_t current_limit;         // Max allocatable address
    struct memblock_type memory;       // All RAM regions
    struct memblock_type reserved;     // Reserved/allocated regions
};

// Global instance (statically allocated)
struct memblock memblock __initdata_memblock = {
    .memory.regions   = memblock_memory_init_regions,
    .memory.cnt       = 1,    // One dummy region initially
    .memory.max       = INIT_MEMBLOCK_MEMORY_REGIONS,  // 128
    .reserved.regions = memblock_reserved_init_regions,
    .reserved.cnt     = 1,
    .reserved.max     = INIT_MEMBLOCK_RESERVED_REGIONS, // 128
    .bottom_up        = false,         // Top-down allocation
    .current_limit    = MEMBLOCK_ALLOC_ANYWHERE,
};
```

### `struct memblock_type` — A List of Regions

```c
struct memblock_type {
    unsigned long cnt;                  // Number of regions
    unsigned long max;                  // Max array capacity
    phys_addr_t total_size;            // Sum of all region sizes
    struct memblock_region *regions;    // Sorted array of regions
    char *name;                        // "memory" or "reserved"
};
```

### `struct memblock_region` — A Single Region

```c
struct memblock_region {
    phys_addr_t base;                  // Physical start address
    phys_addr_t size;                  // Size in bytes
    enum memblock_flags flags;         // MEMBLOCK_NONE, MEMBLOCK_HOTPLUG, MEMBLOCK_NOMAP, etc.
    int nid;                           // NUMA node ID
};
```

### Visual: Memblock Arrays

```
memblock.memory.regions[]:
┌────────────────────┬────────────────────┬─────────┐
│ base=0x4000_0000   │ base=0x1_0000_0000 │ (empty) │
│ size=0x8000_0000   │ size=0x8000_0000   │         │
│ flags=NONE         │ flags=NONE         │         │
│ nid=0              │ nid=0              │         │
├────────────────────┼────────────────────┼─────────┤
   Region 0              Region 1           ...
   (2GB @ 1GB)           (2GB @ 4GB)

memblock.reserved.regions[]:
┌────────────────────┬────────────────────┬────────────────────┐
│ base=0x3000_0000   │ base=0x4080_0000   │ base=0x4508_0000   │
│ size=0x100_0000    │ size=0x200_0000    │ size=0x1_0000      │
├────────────────────┼────────────────────┼────────────────────┤
   OP-TEE               kernel image          DTB

ALWAYS SORTED BY BASE ADDRESS
```

---

## Core Operations

### 1. `memblock_add(base, size)` — Register a RAM Region

```c
int __init memblock_add(phys_addr_t base, phys_addr_t size)
{
    return memblock_add_range(&memblock.memory, base, size,
                              MAX_NUMNODES, MEMBLOCK_NONE);
}
```

**Algorithm:**

```
memblock_add_range(type, base, size, nid, flags):

1. Find insertion point (regions are sorted by base address)
2. Check for overlap with existing regions:

   Case A: No overlap → Insert new region, shift subsequent regions right

   Case B: Partial overlap → Merge with adjacent/overlapping regions

   Case C: Exact duplicate → No-op (already registered)

   Case D: Adjacent → Merge into single region

3. After insertion, call memblock_merge_regions() to coalesce
   adjacent regions with same nid and flags
```

**Merge Example:**

```
Before: regions = [{0x4000, 0x2000}, {0x8000, 0x1000}]
Add:    memblock_add(0x6000, 0x2000)

Step 1: Insert → [{0x4000, 0x2000}, {0x6000, 0x2000}, {0x8000, 0x1000}]
Step 2: Merge adjacent → [{0x4000, 0x5000}]  // All three merge!
```

**Array Growth:**
- Initial capacity: 128 regions (INIT_MEMBLOCK_MEMORY_REGIONS)
- If full, `memblock_double_array()` doubles the array capacity
- New array is allocated from memblock itself (after `memblock_allow_resize()`)
- Before resize is allowed, trying to exceed 128 regions triggers a panic

---

### 2. `memblock_reserve(base, size)` — Mark Region as Used

```c
int __init memblock_reserve(phys_addr_t base, phys_addr_t size)
{
    return memblock_add_range(&memblock.reserved, base, size,
                              MAX_NUMNODES, MEMBLOCK_NONE);
}
```

**Same algorithm as `memblock_add()`**, but operates on the `reserved` array instead of `memory`.

Reserved regions are "in use" — `memblock_alloc()` will not return memory from reserved regions.

---

### 3. `memblock_remove(base, size)` — Remove a Memory Region

```c
int __init memblock_remove(phys_addr_t base, phys_addr_t size)
{
    return memblock_remove_range(&memblock.memory, base, size);
}
```

**Algorithm:**

```
memblock_remove_range(type, base, size):

1. Find all regions that overlap [base, base+size)
2. For each overlapping region:

   Case A: Region fully contained → Delete entire region

   Case B: Overlap at start → Trim region start

   Case C: Overlap at end → Trim region end

   Case D: Overlap in middle → Split into two regions
```

**Split Example:**

```
Before: [{0x4000_0000, size=0x8000_0000}]  // 2GB starting at 1GB
Remove: memblock_remove(0x6000_0000, 0x2000_0000)  // Remove 512MB at 1.5GB

After: [{0x4000_0000, size=0x2000_0000},    // 512MB at 1GB
        {0x8000_0000, size=0x4000_0000}]    // 1GB at 2GB
```

---

### 4. `memblock_alloc(size, align)` — Allocate Memory

This is the main allocation function, used to allocate memory for page tables, struct page arrays, and other early data structures.

```c
void *__init memblock_alloc(phys_addr_t size, phys_addr_t align)
{
    return memblock_alloc_try_nid(size, align, MEMBLOCK_LOW_LIMIT,
                                  MEMBLOCK_ALLOC_ACCESSIBLE, NUMA_NO_NODE);
}
```

**Full call chain:**

```
memblock_alloc(size, align)
└── memblock_alloc_try_nid(size, align, min, max, nid)
    └── memblock_alloc_internal(size, align, min, max, nid, exact_nid)
        └── memblock_alloc_range_nid(size, align, start, end, nid, exact_nid)
            ├── memblock_find_in_range(size, align, start, end, nid)
            │   └── Find free region (not in reserved)
            └── memblock_reserve(found_addr, size)
                └── Mark as reserved
```

### The Allocation Algorithm

```c
phys_addr_t __init memblock_find_in_range(phys_addr_t size, phys_addr_t align,
                                           phys_addr_t start, phys_addr_t end,
                                           int nid)
{
    if (memblock.bottom_up)
        return __memblock_find_range_bottom_up(start, end, size, align, nid);
    else
        return __memblock_find_range_top_down(start, end, size, align, nid);
}
```

### Top-Down Allocation (Default)

```
__memblock_find_range_top_down():

1. Start from the END of memory, scan backward
2. For each memory region (reversed iteration):

   for_each_free_mem_range_reverse(i, nid, &this_start, &this_end):
     // this_start..this_end = free range (memory minus reserved)

     candidate = round_down(min(this_end, end) - size, align);

     if (candidate >= max(this_start, start)):
       return candidate;   // Found free, aligned memory!

3. If no fit found, return 0 (failure)
```

**Why top-down?**
- Kernel and DTB are typically at lower addresses
- Allocating from the top avoids fragmenting the lower region
- DMA-capable memory (low addresses) is preserved for devices that need it

### Bottom-Up Allocation (Optional)

```
Set via: memblock.bottom_up = true;

Scans from the START of memory, scan forward.
Used when KASLR wants to randomize allocation order.
```

### Free Range Iteration

The key function `for_each_free_mem_range()` computes free ranges by **subtracting reserved from memory**:

```
Memory:   [████████████████████████████████████]
Reserved: [  ██████  ]    [████]    [████████  ]
Free:     [██        ████       ████          ██]

Algorithm:
  Walk memory[] and reserved[] arrays simultaneously (both sorted)
  At each step, output the gap between reserved regions
```

This is done without creating a separate "free" array — it's computed on-the-fly by iterating both sorted arrays.

---

### 5. `memblock_free(base, size)` — Release a Reserved Region

```c
void __init memblock_free(phys_addr_t base, phys_addr_t size)
{
    memblock_remove_range(&memblock.reserved, base, size);
}
```

Removes a region from the `reserved` array (opposite of `memblock_reserve()`). The memory becomes available for future allocations.

---

### 6. `memblock_mark_nomap(base, size)` — Exclude from Linear Map

```c
int __init memblock_mark_nomap(phys_addr_t base, phys_addr_t size)
{
    return memblock_setclr_flag(&memblock.memory, base, size,
                                1, MEMBLOCK_NOMAP);
}
```

Sets the `MEMBLOCK_NOMAP` flag on memory regions. These regions:
- Are still tracked in `memblock.memory`
- Are **NOT** mapped into the kernel's linear map by `paging_init()`
- Are **NOT** available for allocation
- Used for: GPU memory, TrustZone memory, firmware regions

---

## NUMA-Aware Allocation

When allocating on NUMA systems, memblock tries to allocate from the requested node first:

```c
memblock_alloc_try_nid(size, align, min, max, nid=1):

1. Try to find free memory on node 1
   → for_each_free_mem_range(nid=1, ...)

2. If failed, fall back to any node
   → for_each_free_mem_range(nid=ANY, ...)
```

Each `memblock_region` has a `nid` field recording which NUMA node it belongs to. This is set during `arch_numa_init()` which reads NUMA topology from DTB or ACPI.

---

## Memory Layout of Memblock Itself

```
Static arrays (in kernel .init.data section):

memblock_memory_init_regions[INIT_MEMBLOCK_MEMORY_REGIONS]:
  128 × sizeof(struct memblock_region) = 128 × 32 = 4 KB

memblock_reserved_init_regions[INIT_MEMBLOCK_RESERVED_REGIONS]:
  128 × sizeof(struct memblock_region) = 128 × 32 = 4 KB

After memblock_allow_resize() (called in paging_init):
  If >128 regions needed, array is doubled:
  - New array allocated from memblock itself
  - Old static array abandoned
  - This is a one-time bootstrap operation
```

---

## Memblock Lifecycle

```
Phase 2: setup_machine_fdt()
  │ memblock_add()        ← Register RAM from DTB
  │ memblock_reserve()    ← Reserve DTB
  │
  ▼
Phase 2: arm64_memblock_init()
  │ memblock_remove()     ← Trim unmappable memory
  │ memblock_reserve()    ← Reserve kernel, initrd
  │
  ▼
Phase 2: paging_init()
  │ memblock_alloc()      ← Allocate page table pages
  │ for_each_mem_range()  ← Read regions for mapping
  │
  ▼
Phase 3: free_area_init()
  │ memblock_alloc()      ← Allocate struct page arrays
  │ memblock_alloc()      ← Allocate zone/node structures
  │
  ▼
Phase 4: memblock_free_all()
  │ for_each_free_mem_range()
  │   └── __free_pages_core()  ← Free to buddy allocator
  │
  ▼
  MEMBLOCK IS RETIRED — buddy allocator takes over
```

---

## Key Takeaways

1. **Two sorted arrays** — that's the entire allocator. Memory and reserved, both sorted by base address.
2. **O(n) everything** — add, remove, find are all linear scans. Fine for boot (n < 100 regions), terrible for runtime.
3. **Top-down by default** — allocates from high addresses to preserve low DMA-capable memory.
4. **Free = memory minus reserved** — computed on-the-fly, no separate free list.
5. **Merge aggressively** — adjacent regions with same attributes are always coalesced.
6. **Self-hosting** — when the region arrays need to grow, memblock allocates from itself.
7. **`__initdata`** — memblock code and data are in `.init` sections, freed after boot completes.
