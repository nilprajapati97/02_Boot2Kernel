# 04 — Memblock Operations: The Early Allocator Internals

[← Doc 03: start_kernel → setup_arch](03_start_kernel_to_setup_arch.md) | **Doc 04** | [Doc 05: Page Table Chain →](05_Page_Table_Chain.md)

---

## Overview

Before the buddy allocator exists, all memory operations go through **memblock**. This
document traces the exact code path of every memblock operation used during boot:
`memblock_add`, `memblock_reserve`, `memblock_alloc`, and `memblock_find_in_range`.

---

## 1. Data Structures

**Source**: `include/linux/memblock.h`

```c
struct memblock {
    bool bottom_up;                    // Allocation direction (default: top-down)
    phys_addr_t current_limit;         // Max address for allocation
    struct memblock_type memory;       // Known physical memory regions
    struct memblock_type reserved;     // Reserved (allocated/protected) regions
};

struct memblock_type {
    unsigned long cnt;                 // Number of active regions
    unsigned long max;                 // Array capacity
    phys_addr_t total_size;            // Sum of all region sizes
    struct memblock_region *regions;   // Array of regions
    char *name;                        // "memory" or "reserved"
};

struct memblock_region {
    phys_addr_t base;                  // Start physical address
    phys_addr_t size;                  // Size in bytes
    enum memblock_flags flags;         // HOTPLUG, MIRROR, NOMAP, etc.
    int nid;                           // NUMA node ID
};
```

**Initial state** (static, in BSS):
```c
// mm/memblock.c
static struct memblock_region memblock_memory_init_regions[INIT_MEMBLOCK_MEMORY_REGIONS];
static struct memblock_region memblock_reserved_init_regions[INIT_MEMBLOCK_RESERVED_REGIONS];
//   INIT_MEMBLOCK_MEMORY_REGIONS = 128 (default)
//   INIT_MEMBLOCK_RESERVED_REGIONS = 128 (default)

struct memblock memblock = {
    .memory.regions     = memblock_memory_init_regions,
    .memory.cnt         = 1,     // one dummy region with size=0
    .memory.max         = INIT_MEMBLOCK_MEMORY_REGIONS,
    .memory.name        = "memory",
    .reserved.regions   = memblock_reserved_init_regions,
    .reserved.cnt       = 1,
    .reserved.max       = INIT_MEMBLOCK_RESERVED_REGIONS,
    .reserved.name      = "reserved",
    .bottom_up          = false,
    .current_limit      = MEMBLOCK_ALLOC_ANYWHERE,
};
```

---

## 2. `memblock_add()` — Register a Memory Region

**Source**: `include/linux/memblock.h`, `mm/memblock.c`

```c
// Inline wrapper:
int memblock_add(phys_addr_t base, phys_addr_t size)
{
    return memblock_add_range(&memblock.memory, base, size, MAX_NUMNODES, 0);
}
```

### 2.1 `memblock_add_range()` — The Core Insertion Algorithm

**Source**: `mm/memblock.c`  
**Prototype**:

```c
static int __init_memblock memblock_add_range(
    struct memblock_type *type,    // &memblock.memory or &memblock.reserved
    phys_addr_t base,              // region start PA
    phys_addr_t size,              // region size
    int nid,                       // NUMA node ID
    enum memblock_flags flags)     // region flags
```

**Algorithm**: Two-pass insertion with overlap handling:

```c
{
    bool insert = false;
    phys_addr_t obase = base;
    phys_addr_t end = base + memblock_cap_size(base, &size);
    int idx, nr_new, start_rgn = -1, end_rgn;
    struct memblock_region *rgn;

    if (!size)
        return 0;

    /* ─── FAST PATH: empty array ─── */
    if (type->regions[0].size == 0) {
        type->regions[0].base = base;
        type->regions[0].size = size;
        type->regions[0].flags = flags;
        memblock_set_region_node(&type->regions[0], nid);
        type->total_size = size;
        type->cnt = 1;
        return 0;
    }

    /* Can we insert directly? (enough room in array) */
    if (type->cnt * 2 + 1 <= type->max)
        insert = true;

repeat:
    base = obase;
    nr_new = 0;

    for_each_memblock_type(idx, type, rgn) {
        phys_addr_t rbase = rgn->base;
        phys_addr_t rend = rbase + rgn->size;

        if (rbase >= end)
            break;              // Past our range — done
        if (rend <= base)
            continue;           // Before our range — skip

        /* GAP before this existing region */
        if (rbase > base) {
            nr_new++;
            if (insert) {
                memblock_insert_region(type, idx++, base, rbase - base, nid, flags);
                //   Shifts all entries right, inserts new region
            }
        }
        /* Skip past the overlap */
        base = min(rend, end);
    }

    /* TRAILING portion after all existing regions */
    if (base < end) {
        nr_new++;
        if (insert)
            memblock_insert_region(type, idx, base, end - base, nid, flags);
    }

    if (!nr_new)
        return 0;               // Fully overlapped — nothing to add

    if (!insert) {
        /* PASS 1 done — resize array if needed, then re-run with insertion */
        while (type->cnt + nr_new > type->max)
            memblock_double_array(type, obase, size);
        insert = true;
        goto repeat;
    } else {
        /* PASS 2 done — merge adjacent regions with same attributes */
        memblock_merge_regions(type, start_rgn, end_rgn);
        return 0;
    }
}
```

### 2.2 Example: Adding 2GB RAM

```
Call: memblock_add(0x4000_0000, 0x8000_0000)

Before:
  memblock.memory.regions[0] = { base: 0, size: 0 }  (dummy)

Fast path: array is empty →
  regions[0] = { base: 0x4000_0000, size: 0x8000_0000 }
  total_size = 0x8000_0000 (2 GB)
  cnt = 1
```

---

## 3. `memblock_reserve()` — Mark Memory as Reserved

**Source**: `include/linux/memblock.h`

```c
static __always_inline int memblock_reserve(phys_addr_t base, phys_addr_t size)
{
    return __memblock_reserve(base, size, NUMA_NO_NODE, 0);
}

int __init_memblock __memblock_reserve(phys_addr_t base, phys_addr_t size,
                                       int nid, enum memblock_flags flags)
{
    return memblock_add_range(&memblock.reserved, base, size, nid, flags);
    //   Same algorithm as memblock_add, but operates on the RESERVED array
}
```

### 3.1 Example: Reserving Kernel Image

```
Call: memblock_reserve(__pa_symbol(_text), _end - _text)
      → memblock_reserve(0x4008_0000, 0x0178_0000)   // ~24MB kernel

After:
  memblock.reserved:
    regions[0] = { base: 0x4008_0000, size: 0x0178_0000 }
    cnt = 1
```

---

## 4. `memblock_alloc_range_nid()` — Allocating Memory from Memblock

**Source**: `mm/memblock.c`  
**Prototype**:

```c
phys_addr_t __init memblock_alloc_range_nid(
    phys_addr_t size,          // Requested allocation size
    phys_addr_t align,         // Alignment requirement
    phys_addr_t start,         // Search range start
    phys_addr_t end,           // Search range end
    int nid,                   // Preferred NUMA node
    bool exact_nid)            // Must be on this node?
```

**Code**:

```c
{
    enum memblock_flags flags = choose_memblock_flags();
    phys_addr_t found;

    if (!align) {
        dump_stack();
        align = SMP_CACHE_BYTES;
    }

again:
    /* ─── Step 1: Find free range on preferred node ─── */
    found = memblock_find_in_range_node(size, align, start, end, nid, flags);
    if (found && !__memblock_reserve(found, size, nid, MEMBLOCK_RSRV_KERN))
        goto done;

    /* ─── Step 2: Fallback — any node ─── */
    if (numa_valid_node(nid) && !exact_nid) {
        found = memblock_find_in_range_node(size, align, start, end,
                                            NUMA_NO_NODE, flags);
        if (found && !memblock_reserve_kern(found, size))
            goto done;
    }

    /* ─── Step 3: Fallback — non-mirrored memory ─── */
    if (flags & MEMBLOCK_MIRROR) {
        flags &= ~MEMBLOCK_MIRROR;
        goto again;
    }
    return 0;    // Allocation failed

done:
    kmemleak_alloc_phys(found, size, 0);
    accept_memory(found, size);
    return found;   // Return PHYSICAL ADDRESS of allocated block
}
```

**The allocation sequence**:
1. Find a gap in memblock that fits `size` with `align`
2. Reserve it (add to `memblock.reserved`)
3. Return the physical address

---

## 5. `memblock_find_in_range_node()` — Finding Free Gaps

**Source**: `mm/memblock.c`  
**Prototype**:

```c
static phys_addr_t __init_memblock memblock_find_in_range_node(
    phys_addr_t size,
    phys_addr_t align,
    phys_addr_t start,
    phys_addr_t end,
    int nid,
    enum memblock_flags flags)
```

**Code**:

```c
{
    if (end == MEMBLOCK_ALLOC_ACCESSIBLE || end == MEMBLOCK_ALLOC_NOLEAKTRACE)
        end = memblock.current_limit;

    start = max_t(phys_addr_t, start, PAGE_SIZE);  // Skip page 0
    end = max(start, end);

    /* Default: TOP-DOWN search (allocate from high addresses first) */
    if (memblock_bottom_up())
        return __memblock_find_range_bottom_up(start, end, size, align, nid, flags);
    else
        return __memblock_find_range_top_down(start, end, size, align, nid, flags);
}
```

### 5.1 Top-Down Search (Default)

```c
static phys_addr_t __init_memblock
__memblock_find_range_top_down(phys_addr_t start, phys_addr_t end,
                               phys_addr_t size, phys_addr_t align,
                               int nid, enum memblock_flags flags)
{
    phys_addr_t this_start, this_end, cand;
    u64 i;

    /* Walk free ranges in REVERSE (high to low) */
    for_each_free_mem_range_reverse(i, nid, flags, &this_start, &this_end, NULL) {
        //   "free" = in memblock.memory AND NOT in memblock.reserved
        //   Computed by __next_mem_range_rev() which walks both arrays

        this_start = clamp(this_start, start, end);
        this_end = clamp(this_end, start, end);

        if (this_end < size)
            continue;

        /* Round down to alignment */
        cand = round_down(this_end - size, align);
        if (cand >= this_start)
            return cand;   // Found! Return the aligned address
    }
    return 0;  // No suitable gap
}
```

### 5.2 How "Free Ranges" Are Computed

`for_each_free_mem_range_reverse` iterates ranges that are in `memblock.memory`
but NOT in `memblock.reserved`:

```
memblock.memory:     [=========A=========]     [====B====]
memblock.reserved:        [rr]      [rr]             [r]

Free ranges:         [===]    [==]  [===]       [===]  [=]
                     ↑              ↑           ↑
                     These are iterated (in reverse for top-down)
```

---

## 6. `memblock_remove()` — Remove Memory from Map

**Source**: `mm/memblock.c`

```c
int __init_memblock memblock_remove(phys_addr_t base, phys_addr_t size)
{
    return memblock_remove_range(&memblock.memory, base, size);
}
```

Uses `memblock_isolate_range()` to split regions at the boundaries, then removes
the isolated regions. Used by `arm64_memblock_init()` to clip memory to supported
address ranges.

---

## 7. Common Memblock API Calls During Boot

| Caller | Function | Arguments (example) | Purpose |
|--------|----------|---------------------|---------|
| `early_init_dt_add_memory_arch` | `memblock_add(base, size)` | `(0x4000_0000, 0x8000_0000)` | Register 2GB RAM |
| `arm64_memblock_init` | `memblock_remove(1<<48, MAX)` | `(0x1_0000_0000_0000, ...)` | Remove > 48-bit PA |
| `arm64_memblock_init` | `memblock_reserve(kernel_pa, size)` | `(0x4008_0000, 0x178_0000)` | Reserve kernel |
| `arm64_memblock_init` | `memblock_reserve(initrd_pa, size)` | `(0x4C00_0000, 0x80_0000)` | Reserve initrd |
| `early_pgtable_alloc` | `memblock_phys_alloc(PAGE_SIZE, PAGE_SIZE)` | `(4096, 4096)` | Page table page |
| `sparse_init` | `memblock_alloc(section_map_size, ...)` | varies | Section maps |
| `free_area_init_core` | `memblock_alloc(usemap_size, ...)` | varies | Pageblock bitmap |

---

## 8. How `memblock_phys_alloc` Works End-to-End

Tracing a single allocation during page table creation:

```
early_pgtable_alloc()                        [arch/arm64/mm/mmu.c]
│
├── phys = memblock_phys_alloc(PAGE_SIZE, PAGE_SIZE)
│   │                                       [include/linux/memblock.h]
│   ├── memblock_phys_alloc_range(4096, 4096, 0, MEMBLOCK_ALLOC_ACCESSIBLE)
│   │   │                                   [mm/memblock.c]
│   │   └── memblock_alloc_range_nid(4096, 4096, 0, current_limit, NUMA_NO_NODE, false)
│   │       │
│   │       ├── memblock_find_in_range_node(4096, 4096, 0, current_limit, -1, flags)
│   │       │   │
│   │       │   └── __memblock_find_range_top_down(...)
│   │       │       │
│   │       │       │   Walk free ranges in reverse:
│   │       │       │   Found gap at 0xBFFF_F000 (top of 2GB RAM, minus reserved)
│   │       │       │   Aligned to 4096: candidate = 0xBFFF_F000
│   │       │       │   Return 0xBFFF_F000
│   │       │       │
│   │       ├── __memblock_reserve(0xBFFF_F000, 4096, -1, MEMBLOCK_RSRV_KERN)
│   │       │       Add to memblock.reserved
│   │       │
│   │       └── Return 0xBFFF_F000 (physical address)
│   │
│   └── Return 0xBFFF_F000
│
├── ptr = __va(phys)    // Convert to virtual address via linear map
│   //   = 0xBFFF_F000 - 0x4000_0000 + PAGE_OFFSET
│
├── memset(ptr, 0, PAGE_SIZE)    // Zero the new page
│
└── Return phys                  // Return PA for page table entry
```

---

## State After Document 04

```
┌──────────────────────────────────────────────────────────┐
│  Memblock is the active allocator:                       │
│                                                          │
│  memblock_add_range():                                   │
│    • Two-pass algorithm (count, then insert)             │
│    • Handles overlaps, merges adjacent regions            │
│    • Auto-doubles array if full                          │
│                                                          │
│  memblock_alloc_range_nid():                             │
│    • Top-down search by default (high addresses first)    │
│    • Finds gaps between memory and reserved arrays       │
│    • Reserves the found range before returning           │
│    • Fallback: other nodes, then non-mirrored            │
│                                                          │
│  Used extensively for:                                   │
│    • Page table pages (early_pgtable_alloc)              │
│    • Sparsemem section maps                              │
│    • Struct page arrays (FLATMEM)                        │
│    • Pageblock bitmaps                                   │
│    • CMA regions                                         │
│    • Per-CPU areas                                        │
│                                                          │
│  Memblock will be replaced by buddy in Doc 07            │
│  (memblock_free_all)                                     │
└──────────────────────────────────────────────────────────┘
```

---

[← Doc 03: start_kernel → setup_arch](03_start_kernel_to_setup_arch.md) | **Doc 04** | [Doc 05: Page Table Chain →](05_Page_Table_Chain.md)
