# `sparse_init()` — SPARSEMEM & vmemmap Initialization

**Source:** `mm/sparse.c`
**Phase:** Zone & Node Init
**Memory Allocator:** Memblock (allocates struct page arrays + page tables)
**Called by:** `free_area_init()`
**Purpose:** Create the `struct page` array for all physical memory

---

## What This Function Does

Creates the mapping from **Page Frame Number (PFN)** to **`struct page`** pointer. After this function, `pfn_to_page(pfn)` works for any valid PFN.

On ARM64 with `SPARSEMEM_VMEMMAP` (the default), the `struct page` arrays are mapped into a dedicated virtual address region called **vmemmap**.

---

## The Problem: How to Map PFN → struct page?

The kernel needs a `struct page` (64 bytes) for **every 4KB physical page** in the system. For a 4GB system:

```
Total pages = 4 GB / 4 KB = 1,048,576 pages
struct page size = 64 bytes
Total memory = 1,048,576 × 64 = 64 MB
```

**Three approaches:**

| Model | How | ARM64? |
|-------|-----|--------|
| FLATMEM | Single contiguous array | No (too inflexible) |
| SPARSEMEM | Sectioned array with indirection | Base model |
| SPARSEMEM_VMEMMAP | Sectioned array mapped to fixed VA | **Default on ARM64** |

---

## SPARSEMEM Model

Physical memory is divided into **sections**. Each section covers a fixed-size range of PFNs:

```
ARM64 section size: 1 GB (SECTION_SIZE_BITS = 30)

Section 0:  PFN [0x00000 — 0x3FFFF]    = PA [0x0000_0000 — 0x3FFF_FFFF]
Section 1:  PFN [0x40000 — 0x7FFFF]    = PA [0x4000_0000 — 0x7FFF_FFFF]
Section 2:  PFN [0x80000 — 0xBFFFF]    = PA [0x8000_0000 — 0xBFFF_FFFF]
...
```

The `mem_section[]` array maps section numbers to their `struct page` arrays:

```c
struct mem_section {
    unsigned long section_mem_map;    // Encoded pointer to struct page array
    unsigned long usage;             // Pointer to section_usage (for hotplug)
};

// Global array (or 2D for large systems)
struct mem_section *mem_section[NR_SECTION_ROOTS];
```

---

## VMEMMAP: Virtual Memory Map

Instead of storing actual pointers in `mem_section`, ARM64 uses **vmemmap** — a fixed virtual address region where all `struct page` arrays are laid out contiguously:

```
Virtual Address Space:

VMEMMAP_START (0xFFFF_FFFC_0000_0000 for 48-bit VA)
│
├── struct page for PFN 0      ← 64 bytes
├── struct page for PFN 1      ← 64 bytes
├── struct page for PFN 2      ← 64 bytes
├── ...
├── struct page for PFN N      ← 64 bytes
│
VMEMMAP_END
```

### pfn_to_page() with VMEMMAP

```c
#define pfn_to_page(pfn) (vmemmap + (pfn))

// vmemmap = (struct page *)VMEMMAP_START
// Each struct page is 64 bytes
// pfn_to_page(0x40000) = vmemmap + 0x40000 = VMEMMAP_START + 0x40000 * 64
```

**O(1) lookup!** Just pointer arithmetic — no array traversal, no indirection.

### page_to_pfn() with VMEMMAP

```c
#define page_to_pfn(page) ((unsigned long)((page) - vmemmap))
```

Also O(1) — just pointer subtraction.

---

## How sparse_init() Works

### Step 1: Allocate mem_section Root Array

```c
void __init sparse_init(void)
{
    // Allocate root array if using 2-level section lookup
    // (for systems with very large PA space)

    unsigned long pnum_end = memblock_end_of_DRAM() >> SECTION_SIZE_BITS;

    // For each present section:
    for (pnum = 0; pnum < pnum_end; pnum++) {
        if (!present_section_nr(pnum))
            continue;
        // Mark section as present
    }
```

### Step 2: Allocate struct page Arrays Per Section

```c
    // For each NUMA node:
    for_each_node_state(nid, N_MEMORY) {
        sparse_init_nid(nid);
    }
}
```

### `sparse_init_nid()` — Per-Node Section Setup

```c
static void __init sparse_init_nid(int nid)
{
    // For each section on this node:
    for_each_present_section_nr(pnum) {
        if (section_to_node_table[pnum] != nid)
            continue;

        // Step A: Allocate struct page array for this section
        // Section = 1GB = 262,144 pages = 262,144 × 64 = 16 MB
        struct page *map = sparse_buffer_alloc(section_map_size());

        // Step B: Map into vmemmap
        __populate_section_memmap(pfn, map, nid);

        // Step C: Record in mem_section
        sparse_init_one_section(ms, pnum, map, usage, SECTION_IS_EARLY);
    }
}
```

### Step A: Allocate struct page Array

```c
static void *sparse_buffer_alloc(unsigned long size)
{
    // Allocate from memblock, aligned to section size
    void *ptr = memblock_alloc(size, PAGE_SIZE);
    return ptr;
}
```

**For a 1GB section:**
- 262,144 pages × 64 bytes = 16 MB from memblock
- This is the actual `struct page` array storage

### Step B: Map into vmemmap

```c
void __init __populate_section_memmap(unsigned long pfn,
                                      struct page *map, int nid)
{
    unsigned long start = (unsigned long)pfn_to_page(pfn);
    unsigned long end = start + section_map_size();

    // Create page table entries mapping:
    //   vmemmap VA [start..end) → physical address of 'map'
    vmemmap_populate(start, end, nid, NULL);
}
```

**`vmemmap_populate()`** creates page table entries in `swapper_pg_dir` for the vmemmap region:

```
vmemmap_populate(vmemmap_VA_start, vmemmap_VA_end, nid):
  For each PGD entry needed:
    Allocate PUD table from memblock (if not exists)
    For each PUD entry:
      Option A: Use 1GB block mapping (maps 16M struct pages at once)
      Option B: Allocate PMD table from memblock
        For each PMD entry:
          Option A: Use 2MB block mapping
          Option B: Allocate PTE table from memblock
            For each PTE entry:
              Map 4KB page of struct page array
```

**Block mappings in vmemmap** are used when possible:
- A 2MB PMD entry covers 2MB / 64 bytes = 32,768 struct pages = 128 MB of physical RAM
- A 1GB PUD entry covers 1GB / 64 bytes = 16,777,216 struct pages = 64 GB of physical RAM

---

## Memory Layout After sparse_init()

```
Virtual Address Space:

0xFFFF_FFFC_0000_0000  VMEMMAP_START
│
│ Section 0 (PA 0x0000_0000):
│   Unmapped (no RAM here typically)
│
│ Section 1 (PA 0x4000_0000):
│   struct page[0x40000] to struct page[0x7FFFF]
│   = 16 MB of struct page data
│   Backed by 16 MB memblock allocation
│   Mapped via 2MB PMD block entries (8 entries)
│
│ Section 2 (PA 0x8000_0000):
│   struct page[0x80000] to struct page[0xBFFFF]
│   ...
│
│ (gaps for non-present sections: unmapped, will fault if accessed)
│
0xFFFF_FFFD_0000_0000  (approximate end, depends on PA range)


Physical Memory:
┌─────────────────────────────────────┐
│ struct page arrays                  │ ← 16 MB per 1GB section
│ (allocated from memblock)           │
│ Total: ~1.5% of RAM                │
├─────────────────────────────────────┤
│ vmemmap page tables                 │ ← PGD/PUD/PMD/PTE pages
│ (also from memblock)                │
│ Total: ~64 KB for 4GB system       │
├─────────────────────────────────────┤
│ mem_section[] root array            │ ← Small
├─────────────────────────────────────┤
│ Rest of RAM (available for buddy)   │
└─────────────────────────────────────┘
```

---

## struct page Initial State

After `sparse_init()`, every `struct page` is initialized to a minimal state:

```c
// Set during memmap_init_range() called from free_area_init_core
for each page in section:
    init_page_count(page);           // _refcount = 1
    page_mapcount_reset(page);       // _mapcount = -1
    SetPageReserved(page);           // Mark as reserved (not free)
    set_page_node(page, nid);        // Set NUMA node
    set_page_zone(page, zone_idx);   // Set zone
    set_page_links(page, zone, nid, pfn); // Combined link setup
    INIT_LIST_HEAD(&page->lru);      // Initialize LRU list
    page_cpupid_reset_last(page);
```

**`PageReserved` flag:** All pages start as reserved. When `memblock_free_all()` runs (Phase 4), it clears this flag for free pages and adds them to the buddy allocator.

---

## Hot-Remove Support (Memory Hotplug)

SPARSEMEM exists primarily to support **memory hotplug**:

```
Before hotplug: Section 5 present, struct page[] exists
After hot-remove:
  1. Migrate all pages out of section 5
  2. Offline section 5
  3. Free struct page[] for section 5
  4. Unmap vmemmap entries
  5. Physically remove DIMM
```

FLATMEM can't do this because the struct page array is contiguous — you can't free a piece from the middle.

---

## Key Takeaways

1. **vmemmap makes pfn_to_page() O(1)** — just `vmemmap + pfn`, no lookup tables
2. **~1.5% overhead** — every 4KB physical page needs a 64-byte struct page
3. **Block mappings reduce overhead** — 2MB vmemmap entries cover 128MB of RAM each
4. **Sections enable hotplug** — each 1GB section has independent struct page arrays
5. **All struct pages start as PageReserved** — freed to buddy in Phase 4
6. **vmemmap is sparse** — only present sections are mapped, holes cause faults
