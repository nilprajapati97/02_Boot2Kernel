# memblock: Early Boot Memory Allocator

**Category**: Linux Kernel Memory Allocators  
**Platform**: ARM64 (AArch64)

---

## 1. Concept Foundation

```
Boot memory problem:
  Linux kernel must allocate memory during early boot
  BUT: the buddy allocator, SLUB, vmalloc — none are initialized yet!
  Page tables don't exist, struct page array doesn't exist, zone data structures don't exist
  
  Chicken-and-egg: need memory to set up memory management!
  
  Solution: memblock — a minimal, simple allocator for early boot
  
  memblock philosophy:
    Simple arrays of [start, end] physical address ranges
    Two arrays: memory (available RAM) + reserved (in use)
    Allocation = find first gap in reserved array that fits
    Free = remove from reserved array
    No complex data structures, no locks required (single-threaded boot)
  
  memblock lifecycle:
    1. Kernel starts executing (EL2 → EL1 handoff, MMU off or basic mapping)
    2. arm64_memblock_init(): populate memblock from firmware (DTB/UEFI)
    3. Kernel allocates early data structures via memblock_alloc()
    4. setup_arch() / paging_init() / bootmem_init()
    5. memblock_free_all(): hand all free memblock memory to buddy allocator
    6. memblock data structures no longer used (buddy takes over)
  
  After memblock_free_all(): memblock is essentially dead
  But: memblock still tracks reserved regions for reference
  /sys/kernel/debug/memblock/: shows final state
```

---

## 2. memblock Data Structures

```c
/* include/linux/memblock.h */

struct memblock_region {
    phys_addr_t base;    // start physical address
    phys_addr_t size;    // size in bytes
    enum memblock_flags flags;  // MEMBLOCK_NONE, MEMBLOCK_HOTPLUG, MEMBLOCK_MIRROR, MEMBLOCK_NOMAP
    int nid;             // NUMA node ID
};

struct memblock_type {
    unsigned long cnt;           // number of regions
    unsigned long max;           // allocated size of regions array
    phys_addr_t total_size;     // total size of all regions
    struct memblock_region *regions;  // array of regions
    char *name;                  // "memory" or "reserved"
};

struct memblock {
    bool bottom_up;              // allocation direction (bottom = low PA first)
    phys_addr_t current_limit;  // upper limit for allocations
    struct memblock_type memory;   // available physical memory
    struct memblock_type reserved; // reserved (allocated or in use) memory
};

// Global instance:
extern struct memblock memblock;
```

---

## 3. ARM64 memblock Initialization

```
ARM64 boot sequence with memblock:

1. __primary_switched() [arch/arm64/kernel/head.S]:
   After kernel page table setup, calls start_kernel()

2. setup_arch() [arch/arm64/kernel/setup.c]:
   → arm64_memblock_init():
     
     a. Parse firmware memory map:
        UEFI: efi_memmap_walk() → memblock_add() for each RAM region
        DT:   early_init_dt_scan_memory() → memblock_add()
        Example: memblock_add(0x40000000, 0x100000000)  // 4GB at 1GB offset
     
     b. Reserve kernel image:
        memblock_reserve(__pa_symbol(_text), __pa_symbol(_end) - __pa_symbol(_text))
        Prevents kmalloc from reusing kernel code/data pages
     
     c. Reserve initrd (initial ramdisk):
        if (initrd_start): memblock_reserve(initrd_start, initrd_end - initrd_start)
     
     d. Reserve device tree:
        memblock_reserve(__pa(initial_boot_params), fdt_totalsize)
     
     e. Reserve swiotlb bounce buffer:
        swiotlb_init(1): memblock_alloc(io_tlb_nslabs * IO_TLB_SIZE, ...)
     
     f. Reserve CPU-local data, vectors, etc.
     
     g. NOMAP regions: device tree "no-map" reserved-memory regions:
        memblock_mark_nomap(base, size):
          Sets MEMBLOCK_NOMAP flag — never given to buddy
          Used for CMA carveouts, firmware-reserved, EFI runtime services

3. paging_init() → bootmem_init():
   → sparse_init(): creates vmemmap array (struct page per PFN)
   → zone_sizes_init(): sets up zone boundaries

4. mem_init() → memblock_free_all():
   Iterates memblock.memory regions
   For each FREE region (not in memblock.reserved, not MEMBLOCK_NOMAP):
     __free_pages_memory(pfn_start, pfn_end):
       Adds pages to buddy allocator as free pages
   
   After this: buddy allocator owns all free physical memory
   memblock is "retired" (data structures remain for reference/debugging)
```

---

## 4. memblock API Reference

```c
// Add physical memory to memblock:
int memblock_add(phys_addr_t base, phys_addr_t size);
int memblock_add_node(phys_addr_t base, phys_addr_t size, int nid, unsigned long flags);

// Reserve (mark as in use):
int memblock_reserve(phys_addr_t base, phys_addr_t size);

// Mark as no-map (never given to buddy):
int memblock_mark_nomap(phys_addr_t base, phys_addr_t size);

// Allocate memory:
void *memblock_alloc(phys_addr_t size, phys_addr_t align);
void *memblock_alloc_low(phys_addr_t size, phys_addr_t align);  // below 4GB
void *memblock_alloc_node(phys_addr_t size, phys_addr_t align, int nid);

// Free memory:
void memblock_free(void *ptr, phys_addr_t size);
// (just removes from reserved, back to free for next alloc)

// Query:
bool memblock_is_memory(phys_addr_t addr);    // is this addr in a RAM region?
bool memblock_is_reserved(phys_addr_t addr);  // is this addr reserved?
phys_addr_t memblock_start_of_DRAM(void);
phys_addr_t memblock_end_of_DRAM(void);

// Iterators:
for_each_mem_range(i, &start, &end):    // iterate free memory regions
for_each_reserved_mem_region(rgn):      // iterate reserved regions
for_each_mem_pfn_range(i, MAX_NUMNODES, &start_pfn, &end_pfn, &nid)

// Hand off to buddy:
unsigned long memblock_free_all(void);  // returns number of pages freed
```

---

## 5. Interview Questions & Answers

**Q1: Why can't the kernel use kmalloc() or alloc_pages() during early boot?**

kmalloc() and alloc_pages() require the following infrastructure to exist:
- `struct zone` (zone data structures): initialized later in `zone_sizes_init()`
- `struct page` (vmemmap array): created in `sparse_init()` during `paging_init()`
- Per-CPU data: per-CPU page sets need `boot_cpu_init()` and SMP setup
- SLUB caches: created in `kmem_cache_init()` which runs after mm_init()

All of these depend on **knowing the physical memory layout**, which is what memblock provides. There's a strict ordering:

1. **memblock**: "here is physical RAM" (from DTB/UEFI)
2. **paging_init**: creates kernel page tables over all RAM, creates vmemmap (struct page for each PFN)
3. **zone_init**: initializes zone structures using memblock info
4. **memblock_free_all()**: hands free pages to buddy
5. **kmem_cache_init()**: creates SLUB infrastructure
6. Now: kmalloc/alloc_pages work

memblock is deliberately kept simple (no complex dependencies) so it can work with just the DTB/UEFI data and the running kernel image — nothing else.

**Q2: What happens to memblock after memblock_free_all() is called?**

The memblock data structures themselves REMAIN in memory (they're not freed). The `memblock` global variable still exists and still accurately reflects which physical addresses are reserved (kernel image, device tree, etc.).

After `memblock_free_all()`:
- **Free regions**: owned by buddy allocator, memblock has no knowledge of individual allocations
- **Reserved regions**: still tracked in `memblock.reserved` for reference
- The buddy allocator is the AUTHORITATIVE source of free/used pages

Remaining uses after boot:
1. `/sys/kernel/debug/memblock/` shows the boot-time memory map (useful for debugging)
2. `memblock_is_memory()` can still be used to check if a PA is in RAM
3. `memblock_is_reserved()` still accurate for reserved regions
4. Memory hotplug: when adding new memory, `memblock_add()` is called again to register new RAM ranges before adding them to the buddy allocator

---

## 6. Quick Reference

| memblock Function | Effect |
|---|---|
| memblock_add() | Register available RAM |
| memblock_reserve() | Mark region as in use |
| memblock_mark_nomap() | Exclude from buddy forever |
| memblock_alloc() | Allocate early boot memory |
| memblock_free_all() | Hand free pages to buddy |

| ARM64 memblock Sources | Data Provider |
|---|---|
| RAM regions | Device tree /memory nodes, UEFI memmap |
| Kernel image | __pa(_text) to __pa(_end) |
| DTB | Passed by bootloader (UEFI/u-boot) |
| CMA carveouts | Device tree /reserved-memory |
| UEFI runtime | UEFI memory map type EfiRuntimeServicesData |
