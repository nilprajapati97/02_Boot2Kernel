# Layer 5 — Memblock: The Early Boot Allocator

[← Layer 4: Device Tree & Memory Discovery](04_Device_Tree_and_Memory_Discovery.md) | **Layer 5** | [Layer 6: Page Tables & Paging Init →](06_Page_Tables_and_Paging_Init.md)

---

## What This Layer Covers

After the Device Tree is parsed, the kernel knows about all physical RAM (stored in
`memblock.memory`). But the buddy allocator and slab allocator don't exist yet — they
need memory to initialize themselves! Memblock is the **first and simplest memory
allocator** in the Linux kernel. It manages physical memory during early boot using two
sorted arrays of regions. This layer explains memblock's data structures, operations,
and how `arm64_memblock_init()` configures it for the ARM64 platform.

---

## 1. Why Memblock Exists

The kernel faces a chicken-and-egg problem during boot:

```
┌──────────────────────────────────────────────────────────────────┐
│                  THE BOOTSTRAP PROBLEM                           │
│                                                                  │
│  To set up the buddy allocator, the kernel needs:                │
│    • struct page arrays (one per physical page frame)            │
│    • Zone structures (pg_data_t, struct zone)                    │
│    • Page table pages (for the linear map)                       │
│                                                                  │
│  But allocating these structures requires a memory allocator!    │
│                                                                  │
│  Solution: memblock                                              │
│    A dead-simple allocator that tracks memory as two sorted      │
│    arrays of (base, size) regions. No page tables needed,        │
│    no struct page needed — just raw physical address ranges.     │
│                                                                  │
│  Lifecycle:                                                      │
│    memblock is used during boot → hands off to buddy → discarded │
└──────────────────────────────────────────────────────────────────┘
```

---

## 2. Memblock Data Structures

**Source**: `include/linux/memblock.h`, `mm/memblock.c`

### 2.1 The Global `memblock` Structure

Memblock is a single global variable, **statically initialized** at compile time:

```
struct memblock memblock = {
    .memory = {
        .regions    = memblock_memory_init_regions,    // Static array [128]
        .max        = 128,
        .cnt        = 0,                              // No regions yet
        .total_size = 0,
    },
    .reserved = {
        .regions    = memblock_reserved_init_regions,  // Static array [128]
        .max        = 128,
        .cnt        = 0,
        .total_size = 0,
    },
    .bottom_up      = false,         // Allocate top-down by default
    .current_limit  = MEMBLOCK_ALLOC_ANYWHERE,
};
```

### 2.2 The Two Region Arrays

Memblock's core idea: physical memory is described by just **two sorted arrays**:

```
memblock.memory (what RAM exists)
═════════════════════════════════

Array of struct memblock_region, sorted by base address:

Index  Base Address           Size           Flags    NUMA Node
─────  ────────────           ────           ─────    ─────────
  0    0x0000_0000_4000_0000  0x8000_0000    NONE     0
  1    0x0000_0001_0000_0000  0x1_0000_0000  NONE     0

  cnt = 2, total_size = 6 GB


memblock.reserved (what's already allocated/reserved)
═════════════════════════════════════════════════════

Index  Base Address           Size           Flags    NUMA Node
─────  ────────────           ────           ─────    ─────────
  0    0x0000_0000_4080_0000  0x0200_0000    NONE     0   (kernel image)
  1    0x0000_0000_4800_0000  0x0000_8000    NONE     0   (FDT)

  cnt = 2, total_size = ~34 MB
```

### 2.3 `struct memblock_region`

Each region is simple:

```
struct memblock_region {
    phys_addr_t base;              // Physical start address
    phys_addr_t size;              // Size in bytes
    enum memblock_flags flags;     // HOTPLUG, MIRROR, NOMAP, etc.
    int nid;                       // NUMA node ID (MAX_NUMNODES if unknown)
};
```

**Flags**:
- `MEMBLOCK_NONE` — Normal memory
- `MEMBLOCK_HOTPLUG` — Memory that can be hot-removed
- `MEMBLOCK_MIRROR` — Mirrored (redundant) memory
- `MEMBLOCK_NOMAP` — Do NOT include in the kernel's linear map

### 2.4 Visual: How Memblock Sees Memory

```
Physical Address Space
═════════════════════════════════════════════════════════════

0x1_4000_0000 ┌─────────────────────────────┐
              │         Free RAM             │  ← memblock.memory[1]
              │    (part of 4 GB bank 2)     │     minus any reserved
              │                              │     portions
0x1_0000_0000 ├─────────────────────────────┤

              │    (hole — no RAM here)      │

0xC000_0000   ├─────────────────────────────┤
              │         Free RAM             │
              ├─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ┤
              │   initrd (reserved)          │  ← memblock.reserved
              ├─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ┤
              │         Free RAM             │
              ├─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ┤
              │      FDT (reserved)          │  ← memblock.reserved
              ├─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ┤
              │         Free RAM             │
              ├─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ┤  ← memblock.memory[0]
              │   Kernel Image (reserved)    │  ← memblock.reserved
              ├─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ┤
              │         Free RAM             │
0x4000_0000   └─────────────────────────────┘

"Free" = in memblock.memory BUT NOT in memblock.reserved
```

---

## 3. Memblock Operations

### 3.1 `memblock_add()` — Register Physical RAM

Called by `early_init_dt_scan_memory()` for each `/memory` DT node.

```
memblock_add(base, size)
    └─► memblock_add_range(&memblock.memory, base, size, MAX_NUMNODES, 0)
        │
        │   Two-pass algorithm:
        │
        ├── Pass 1 (insert=false):
        │       Walk existing regions, count how many new sub-regions
        │       are needed after handling overlaps.
        │       If the array is full, call memblock_double_array()
        │       to grow it (doubles the array, copies old entries).
        │
        ├── Pass 2 (insert=true):
        │       Actually insert new sub-regions into the sorted array.
        │       Handle overlaps by splitting/merging as needed.
        │
        └── memblock_merge_regions():
                Walk the array and merge adjacent regions that have
                the same flags and NUMA node.
```

**Example**: Adding 2 GB at 0x40000000:

```
Before: memblock.memory = []
After:  memblock.memory = [ {base=0x40000000, size=0x80000000} ]
```

### 3.2 `memblock_reserve()` — Mark Memory as Allocated

Called to protect critical regions from being overwritten.

```
memblock_reserve(base, size)
    └─► memblock_add_range(&memblock.reserved, base, size, ...)
            Same two-pass algorithm, but operates on the .reserved array.
```

**Who calls memblock_reserve()?**
- `setup_machine_fdt()` — reserves the FDT
- `arm64_memblock_init()` — reserves the kernel image (`_text` to `_end`)
- `early_init_fdt_scan_reserved_mem()` — reserves DT `/reserved-memory` entries
- Various subsystems — reserve initrd, crashkernel, CMA pool, etc.

### 3.3 `memblock_alloc()` — Allocate Memory

The most important operation: allocate physical memory during early boot.

```
memblock_alloc(size, align)
    │
    └─► memblock_alloc_internal(size, align, ...)
        │
        └─► memblock_alloc_range_nid(size, align, 0, limit, nid, ...)
            │
            ├── 1. memblock_find_in_range_node()
            │       Search for a free gap between memblock.memory
            │       and memblock.reserved regions.
            │
            │       The search direction depends on memblock.bottom_up:
            │       ┌─────────────────────────────────────────────┐
            │       │  bottom_up = false (DEFAULT):               │
            │       │    Search from HIGH addresses downward       │
            │       │    → Puts boot allocations at top of RAM    │
            │       │    → Leaves low addresses for DMA devices   │
            │       │                                             │
            │       │  bottom_up = true:                          │
            │       │    Search from LOW addresses upward          │
            │       │    → Used in some special configurations     │
            │       └─────────────────────────────────────────────┘
            │
            ├── 2. __memblock_reserve(found_base, size)
            │       Reserve the allocated region.
            │
            └── 3. Return phys_to_virt(found_base)
                    Convert physical address to virtual (via linear map).
                    NOTE: This only works AFTER the linear map exists!
                    Before paging_init(), memblock_alloc returns
                    addresses that will be valid once the linear map
                    is created.
```

### 3.4 Finding Free Memory — The Gap-Finding Algorithm

The key insight: "free" memory is any region in `memblock.memory` that is NOT covered
by `memblock.reserved`:

```
memblock.memory:     [=============================]
memblock.reserved:        [===]     [==]    [=====]
                     ─────────────────────────────────
Free (allocatable):  [====]    [===]   [==]

__memblock_find_range_bottom_up() or __memblock_find_range_top_down()
scans these free gaps looking for one that satisfies the size and
alignment requirements.
```

### 3.5 `memblock_remove()` and `memblock_free()`

- `memblock_remove(base, size)` — Remove a range from `memblock.memory` (rare)
- `memblock_free(base, size)` — Remove a range from `memblock.reserved` (un-reserve)
- `memblock_mark_nomap(base, size)` — Flag a memory region as NOMAP (not linear-mapped)

---

## 4. `arm64_memblock_init()` — ARM64-Specific Memblock Configuration

**Source**: `arch/arm64/mm/init.c`

After Device Tree parsing populates `memblock.memory` with raw physical RAM regions,
`arm64_memblock_init()` fine-tunes the regions for the ARM64 architecture:

```
arm64_memblock_init()
    │
    ├──► 1. Remove memory above PHYS_MASK
    │       memblock_remove(1ULL << PHYS_MASK_SHIFT, ULLONG_MAX)
    │       Any RAM above the CPU's physical address limit is removed.
    │       (e.g., if CPU supports 48-bit PA, remove anything above 2^48)
    │
    ├──► 2. Compute memstart_addr
    │       memstart_addr = round_down(memblock_start_of_DRAM(),
    │                                  ARM64_MEMSTART_ALIGN)
    │       This is PHYS_OFFSET — the base address used for PA↔VA
    │       conversion in the linear map.
    │       Aligned to a large boundary (e.g., 1 GB for 48-bit VA).
    │
    ├──► 3. Clip memory to fit in the linear map
    │       The linear map VA space has a finite size:
    │         linear_region_size = PAGE_END - PAGE_OFFSET
    │       Any physical memory above (memstart_addr + linear_region_size)
    │       is removed because it cannot be linearly mapped.
    │
    │       ┌───────────────────────────────────────────────────────┐
    │       │  Example (48-bit VA):                                 │
    │       │    PAGE_OFFSET    = 0xFFFF_0000_0000_0000             │
    │       │    PAGE_END       = 0xFFFF_8000_0000_0000             │
    │       │    linear_region  = 128 TB                            │
    │       │    Max mappable RAM = 128 TB (more than enough!)      │
    │       └───────────────────────────────────────────────────────┘
    │
    ├──► 4. Handle 52-bit VA fallback on 48-bit hardware
    │       If kernel was compiled with 52-bit VA but hardware only
    │       supports 48-bit: reduce memstart_addr to fit.
    │
    ├──► 5. Apply "mem=" command line limit
    │       If user passed "mem=512M", limit total usable memory.
    │       Excess memory is removed from memblock.memory.
    │
    ├──► 6. Handle initrd
    │       Check if initrd is within the linear-mappable range.
    │       Reserve initrd memory: memblock_reserve(initrd_start, size)
    │
    ├──► 7. Reserve the kernel image
    │       memblock_reserve(__pa_symbol(_text), _end - _text)
    │       Reserves the physical memory occupied by the kernel
    │       binary (text + rodata + data + bss + page tables).
    │
    └──► 8. Process /reserved-memory DT nodes
            early_init_fdt_scan_reserved_mem()
            │
            ├── For each /reserved-memory child node:
            │     memblock_reserve(base, size)    — or —
            │     memblock_mark_nomap(base, size) — if "no-map" property
            │
            └── Process "shared-dma-pool" / CMA regions
```

### 4.1 Memblock State After `arm64_memblock_init()`

```
memblock.memory (after arm64 adjustments):
══════════════════════════════════════════

Index  Base Address           Size          Flags     Node
─────  ────────────           ────          ─────     ────
  0    0x0000_0000_4000_0000  0x8000_0000   NONE      0     (2 GB, bank 1)
  1    0x0000_0001_0000_0000  0x1_0000_0000 NONE      0     (4 GB, bank 2)


memblock.reserved (after arm64 adjustments):
════════════════════════════════════════════

Index  Base Address           Size          What
─────  ────────────           ────          ────
  0    0x0000_0000_4080_0000  0x0200_0000   Kernel image (_text → _end)
  1    0x0000_0000_4800_0000  0x0000_8000   FDT (Device Tree Blob)
  2    0x0000_0000_4900_0000  0x0100_0000   initrd (initial ramdisk)
  3    0x0000_0000_5000_0000  0x1000_0000   /reserved-memory (GPU, no-map)
  4    ...                     ...          Other reservations


Free memory = memblock.memory MINUS memblock.reserved
             = all RAM except kernel, FDT, initrd, firmware reservations
```

---

## 5. How Memblock Allocations Are Used

During boot, memblock is the ONLY way to allocate memory. Here's what uses it:

```
Early Boot Allocations (all via memblock_alloc):
═════════════════════════════════════════════════

Caller                          What's Allocated            Size (typical)
──────                          ────────────────            ──────────────
paging_init → pgtable_alloc     Page table pages            Varies (many pages)
                                (for the linear map)

free_area_init → alloc_node_    struct page array           ~1.56% of RAM
  mem_map                       (one per 4KB page frame)    (64B per 4KB page)

free_area_init_core →           Pageblock flags bitmap      Small
  setup_usemap

setup_log_buf                   Kernel log buffer           256 KB - 2 MB

setup_per_cpu_areas             Per-CPU data regions        Varies

sparse_init                     Sparsemem section arrays    Varies

early_ioremap                   Temporary I/O mappings      Small
```

---

## 6. Memblock Lifecycle

```
Timeline:
═════════

  Boot Start                                                    Boot End
  ──────┼────────────────────────────────────────────────────────┼──────
        │                                                        │
        │  early_init_dt_scan_memory()                           │
        │  └► memblock_add() — register RAM           ┐          │
        │                                              │          │
        │  arm64_memblock_init()                       │          │
        │  └► memblock_reserve() — reserve regions     │          │
        │                                              │ memblock │
        │  paging_init()                               │ is the   │
        │  └► memblock_alloc() — page table pages      │ ONLY     │
        │                                              │ allocator│
        │  free_area_init()                            │          │
        │  └► memblock_alloc() — struct page arrays    │          │
        │                                              ┘          │
        │                                                        │
        │  ★ memblock_free_all() ★ ──── THE HANDOFF ────────────│
        │  All unreserved memblock memory is freed to             │
        │  the buddy allocator. memblock's job is DONE.           │
        │                                                        │
        │  page_alloc_init_late()                                │
        │  └► memblock_discard() — free memblock arrays           │
        │     memblock data structures themselves are freed.      │
        │                                                        │
       ─┴────────────────────────────────────────────────────────┘
```

---

## 7. State After Layer 5

```
┌────────────────────────────────────────────────────────────────┐
│              STATE AFTER arm64_memblock_init()                  │
│                                                                │
│  memblock.memory:                                              │
│    ✓ All physical RAM registered (from DT)                     │
│    ✓ Clipped to PA/VA limits                                   │
│    ✓ memstart_addr (PHYS_OFFSET) computed                      │
│                                                                │
│  memblock.reserved:                                            │
│    ✓ Kernel image reserved                                     │
│    ✓ FDT reserved                                              │
│    ✓ initrd reserved                                           │
│    ✓ DT /reserved-memory nodes processed                       │
│                                                                │
│  Memory Allocator:                                             │
│    ✓ memblock_alloc() works — finds free gaps, reserves them   │
│    ✗ Buddy allocator NOT ready (no struct page, no zones)      │
│    ✗ Slab allocator NOT ready                                  │
│                                                                │
│  Page Tables:                                                  │
│    ✓ swapper_pg_dir has kernel image mapping                   │
│    ✓ Fixmap is active (FDT accessible)                         │
│    ✗ Linear map NOT created yet                                │
│                                                                │
│  What Comes Next:                                              │
│    → paging_init() (Layer 6): create linear map using memblock │
│      to allocate page table pages                              │
└────────────────────────────────────────────────────────────────┘
```

---

## Summary: What Happened in Layer 5

| Aspect | Detail |
|--------|--------|
| **What is memblock** | A simple early boot allocator using two sorted arrays of physical address regions |
| **memory array** | Lists all physical RAM (from DT /memory nodes) |
| **reserved array** | Lists allocated/protected regions (kernel, FDT, initrd, firmware) |
| **Free memory** | Any region in `memory` NOT covered by `reserved` |
| **Allocation strategy** | Top-down by default (high addresses first → leaves low for DMA) |
| **arm64 adjustments** | Clip to PA/VA limits, compute memstart_addr, reserve kernel+initrd |
| **Array growth** | Starts with 128 slots, doubles if needed via memblock_double_array() |
| **Lifetime** | Used during boot only → discarded after buddy allocator takes over |

---

[← Layer 4: Device Tree & Memory Discovery](04_Device_Tree_and_Memory_Discovery.md) | **Layer 5** | [Layer 6: Page Tables & Paging Init →](06_Page_Tables_and_Paging_Init.md)
