# arm64_memblock_init and Kernel Boot Memory Setup

**Category**: KASLR and Kernel Boot Memory Setup  
**Platform**: ARM64 (AArch64)

---

## 1. Concept Foundation

```
Boot-time memory problem:
  At kernel entry: MMU may be on (with early/stub page tables) or off
  Page allocator (buddy system): NOT yet initialized
  slab/vmalloc: NOT yet available
  
  But kernel needs memory for:
    Page tables (pgd, pud, pmd, pte pages)
    struct page array (vmemmap)
    Early kernel data structures
    IRQ descriptors, zone structs, etc.
  
  Solution: memblock allocator
    A simple, static-array-based physical memory manager
    Active from: very start of kernel boot
    Until: memblock_free_all() hands all free memory to buddy
    
  Two key phases:
    1. Discovery: find all available physical RAM
    2. Setup: build proper page tables, initialize struct page

Timeline:
  [bootloader] → [EFI stub / head.S] → [start_kernel()] 
  → [setup_arch()] → [arm64_memblock_init()] → [paging_init()]
  → [mm_init()] → [memblock_free_all()] → [buddy available]
```

---

## 2. arm64_memblock_init() Deep Dive

```
arch/arm64/mm/init.c:

void __init arm64_memblock_init(void)
{
    // Step 1: Remove non-linear map regions from memblock
    // NOMAP regions: OP-TEE, firmware, CMA reservations
    const s64 linear_region_size = BIT(vabits_actual - 1);
    
    // Step 2: Process device tree memory nodes
    // early_init_dt_scan_memory() already called before this:
    //   → parsed /memory { reg = <...>; } nodes
    //   → called memblock_add() for each range
    
    // Step 3: Limit memblock to what can fit in linear map
    memblock_remove(linear_region_size, ULLONG_MAX);
    // Any RAM above the linear map VA size is removed
    
    // Step 4: Reserve kernel image
    memblock_reserve(__pa_symbol(_stext),
                     __pa_symbol(_end) - __pa_symbol(_stext));
    
    // Step 5: Reserve initrd (if present)
    if (initrd_start) {
        memblock_reserve(__pa(initrd_start),
                         initrd_end - initrd_start);
    }
    
    // Step 6: Reserve device tree blob (DTB)
    fdt_enforce_memory_region();
    // Scans DT for reserved-memory nodes:
    //   /reserved-memory/op-tee: memblock_reserve() + NOMAP
    //   /reserved-memory/linux,cma: memblock_reserve()
    
    // Step 7: EFI memory map processing
    if (efi_enabled(EFI_MEMMAP))
        efi_find_and_save_memmap();  // parse EFI memmap
    
    // Step 8: Reserve EFI runtime services
    efi_memblock_x86_reserve_range();  // name misleading, ARM64 also calls
    
    // Step 9: Reserve memory for crash kernel (kdump)
    reserve_crashkernel();
    
    // Step 10: Process ACPI SRAT for NUMA
    acpi_numa_init();  // sets up NUMA node → pfn mapping
    
    // Step 11: CMA (Contiguous Memory Allocator) reservation
    dma_contiguous_reserve(arm64_dma_phys_limit);
    
    // Step 12: memblock_dump_all() in verbose mode (for debugging)
}
```

### 2.1 memblock Data Structures

```c
// include/linux/memblock.h

struct memblock_region {
    phys_addr_t base;     // physical start address
    phys_addr_t size;     // size in bytes
    enum memblock_flags flags;  // MEMBLOCK_NONE/HOTPLUG/MIRROR/NOMAP
#ifdef CONFIG_NUMA
    int nid;              // NUMA node ID
#endif
};

struct memblock_type {
    unsigned long cnt;      // number of regions
    unsigned long max;      // max capacity (array size)
    phys_addr_t total_size; // total bytes
    struct memblock_region *regions;  // array of regions
    char *name;             // "memory" or "reserved"
};

struct memblock {
    bool bottom_up;                  // allocation direction
    phys_addr_t current_limit;       // max PA for allocations
    struct memblock_type memory;     // available regions
    struct memblock_type reserved;   // reserved regions
};

extern struct memblock memblock;   // global instance

// MEMBLOCK_NONE    = 0: normal allocatable region
// MEMBLOCK_HOTPLUG = BIT(0): hot-pluggable (ZONE_MOVABLE candidate)
// MEMBLOCK_MIRROR  = BIT(1): mirrored memory (high-availability)
// MEMBLOCK_NOMAP   = BIT(2): do NOT map in linear map
```

### 2.2 Key memblock Operations

```c
// Adding memory to memblock:
memblock_add(base, size):
    Adds [base, base+size) to memblock.memory
    Merges overlapping/adjacent regions automatically
    Called from: early_init_dt_scan_memory() for DT /memory nodes
                 efi_memblock_add() for EFI ConventionalMemory

memblock_add_node(base, size, nid, flags):
    Like memblock_add() but sets NUMA node ID

// Reserving memory (keep in memory but don't allocate):
memblock_reserve(base, size):
    Adds [base, base+size) to memblock.reserved
    Memory IS still mapped in linear map
    Buddy allocator will NOT use it
    
memblock_mark_nomap(base, size):
    Sets MEMBLOCK_NOMAP flag on region
    Region will NOT be mapped in linear map
    __va() will not work; ioremap() needed to access

// Removing memory:
memblock_remove(base, size):
    Removes from memblock.memory entirely
    Memory does not appear to Linux at all
    Used for: memory above linear map limit, TZASC-protected regions

// Allocation:
phys_addr_t memblock_phys_alloc(size, align):
    Finds free region in memblock.memory that is NOT in memblock.reserved
    Allocates (adds to memblock.reserved to mark as used)
    Returns physical address
    
void *memblock_alloc(size, align):
    = phys_to_virt(memblock_phys_alloc(size, align))
    Returns virtual address via linear map

// Freeing:
memblock_free(base, size):
    Removes from memblock.reserved (frees the "allocation")
    Memory goes back to available pool
```

---

## 3. paging_init() — Building the Real Kernel Page Tables

```c
// arch/arm64/mm/mmap.c + arch/arm64/mm/init.c

void __init paging_init(void)
{
    pgd_t *pgdp = pgd_set_fixmap(__pa_symbol(swapper_pg_dir));
    
    // Step 1: Map all RAM regions in swapper_pg_dir
    map_mem(pgdp);
    
    // Step 2: Map kernel text as read-only + executable
    // map_kernel(pgdp); (actually done as part of map_mem)
    
    // Step 3: Switch to swapper_pg_dir
    cpu_replace_ttbr1(lm_alias(swapper_pg_dir));
    
    // Step 4: Initialize vmemmap (struct page array)
    memmap_init();
    
    // After this:
    //   TTBR1_EL1 = swapper_pg_dir (full linear map + kernel text)
    //   vmemmap_base → struct page for every PFN
}

// map_mem(): the main RAM mapping function
static void __init map_mem(pgd_t *pgdp)
{
    phys_addr_t kernel_start = __pa_symbol(_stext);
    phys_addr_t kernel_end = __pa_symbol(_end);
    
    // Iterate all memblock.memory regions:
    for_each_mem_range(i, &start, &end) {
        if (start >= end)
            continue;
        
        // Choose mapping type:
        // - Normal RAM: MT_NORMAL (write-back, inner shareable)
        // - Kernel text: MT_NORMAL with PXN=0 (executable at EL1)
        // - Kernel data: MT_NORMAL with PXN=1 UXN=1 (not executable)
        
        __map_memblock(pgdp, start, end, pgprot_tagged(PAGE_KERNEL),
                       NO_CONT_MAPPINGS);
    }
    
    // Mark kernel text as read-only after mapping:
    // (actually done in mark_linear_text_alias_ro())
}
```

### 3.1 Huge Page Mappings in map_mem

```
arm64 map_mem() uses largest possible block descriptors:

For 4KB granule:
  If region is 1GB aligned + size ≥ 1GB:
    Use L1 block descriptor (PUD level) → 1GB mapping
    PUD entry bits[1:0] = 0b01 (block)
    PUD[47:30] = PA[47:30] (1GB-aligned PA)
  
  Elif region is 2MB aligned + size ≥ 2MB:
    Use L2 block descriptor (PMD level) → 2MB mapping
    PMD bits[1:0] = 0b01 (block)
    PMD[47:21] = PA[47:21] (2MB-aligned PA)
  
  Else:
    Use L3 page descriptor → 4KB mapping
    PTE bits[1:0] = 0b11 (page)

Most of RAM: mapped as 2MB blocks (typical alignment)
  Result: fewer page table pages needed
          fewer TLB entries needed for kernel linear map

Contiguous hint (CONT bit in PTE/PMD):
  16 consecutive 4KB pages (64KB total) → set CONT bit in all 16
  16 consecutive 2MB blocks (32MB total) → set CONT bit in all 16
  CONT hint: TLB may merge these into a single larger TLB entry
  ARM64 hardware: CONT hint = more efficient TLB usage
```

---

## 4. Memory Layout After paging_init()

```
ARM64 virtual memory layout (48-bit VA, 4KB granule) after paging_init():

VA                    Contents                    TTBR
────────────────────────────────────────────────────────
0x0000_0000_0000_0000  User space start            TTBR0_EL1
...
0x0000_FFFF_FFFF_FFFF  User space end

────────── Gap (canonical hole) ──────────

0xFFFF_0000_0000_0000  Linear map start (PAGE_OFFSET) TTBR1_EL1
...linear map: all physical memory (RAM) mapped here...
... 2MB blocks typically, 1GB for large aligned regions ...
0xFFFF_8000_0000_0000  Linear map end (max 128TB)

0xFFFFA000_0000_0000   vmalloc start               TTBR1_EL1
...vmalloc/ioremap/module text...
0xFFFFC000_0000_0000   vmalloc end

0xFFFFC000_0000_0000   vmemmap start               TTBR1_EL1
...struct page array for all PFNs...
0xFFFFD000_0000_0000   vmemmap end

0xFFFFFF7F_FBF0_0000   fixmap area                 TTBR1_EL1
...fixed virtual addresses for early use...
0xFFFFFF7F_FC00_0000   fixmap end

0xFFFF8001_0000_0000 + KASLR offset: kernel text    TTBR1_EL1
...kernel .text, .rodata, .data, .bss...

After mm_init() / memblock_free_all():
  All free memblock.memory pages → buddy allocator
  memblock: no longer the primary allocator
  Buddy: now manages all free physical pages
```

---

## 5. Interview Q&A

**Q1: What does arm64_memblock_init() do at a high level?**
It builds the initial picture of physical memory available to Linux. Steps: (1) Parse device tree `/memory` nodes (already done by `early_init_dt_scan_memory()`) — adds all RAM to `memblock.memory`. (2) Parse EFI memory map if UEFI boot. (3) Reserve critical regions: kernel text/data, initrd, DTB, EFI runtime tables. (4) Process `/reserved-memory` DT nodes: mark OP-TEE, CMA, etc. as reserved/NOMAP. (5) Reserve crash kernel region, NUMA setup, CMA reservation. After this function: `memblock` has a complete picture of what RAM exists and what is available.

**Q2: What is the difference between memblock_reserve() and memblock_mark_nomap()?**
`memblock_reserve(base, size)`: marks the region as used/reserved. Buddy won't allocate it. BUT the region IS still mapped in the kernel linear map (accessible via `__va()`). Example: kernel text — reserved so buddy doesn't reuse it, but `__pa(kernel_symbol)` works. `memblock_mark_nomap(base, size)`: sets the NOMAP flag. Region is excluded from the linear map entirely. `__va()` won't work. Example: OP-TEE memory — not mapped at all (accessing it would cause a fault). Requires `ioremap()` or explicit mapping for access.

**Q3: How does paging_init() decide between 4KB, 2MB, and 1GB mappings?**
`map_mem()` iterates memblock regions and calls `__map_memblock()`. For each range: (1) If region is 1GB-aligned AND size ≥ 1GB → use L1 block descriptor (1GB). (2) If 2MB-aligned AND size ≥ 2MB → use L2 block descriptor (2MB). (3) Otherwise → L3 page descriptors (4KB). The "contiguous hint" optimization further combines 16 consecutive entries. Most ARM64 server RAM: continuous, well-aligned → mostly 1GB and 2MB mappings → very efficient TLB usage for the kernel linear map.

**Q4: When does memblock stop being used and buddy take over?**
`memblock_free_all()` is called from `mm_init()` in `start_kernel()`. At this point: all zones and `struct page` objects are initialized. `memblock_free_all()` iterates all non-reserved memblock regions and calls `__free_pages_core()` on each range → donates them to the buddy allocator. After this: `memblock_alloc()` still works (it's still functional) but buddy is preferred. Eventually all early allocations migrate to buddy/slab.

**Q5: What is the CONT bit optimization and how does it improve TLB performance?**
The CONT bit in ARM64 PTEs/PMDs hints to the TLB that 16 consecutive entries (all with CONT=1) can be merged into a single TLB entry covering 16× the normal range. For 4KB pages: 16 × 4KB = 64KB covered by one TLB entry. For 2MB PMD: 16 × 2MB = 32MB covered by one TLB entry. The TLB hardware may or may not merge them (it's a hint, not a requirement). For the kernel linear map (which maps GBs of RAM), CONT-mapped regions allow the same RAM to be covered by far fewer TLB entries, reducing TLB pressure for kernel code that scans memory.

---

## 6. Quick Reference

| memblock Function | What It Does |
|---|---|
| `memblock_add(base, size)` | Add RAM region as available |
| `memblock_reserve(base, size)` | Mark as used (still mapped) |
| `memblock_mark_nomap(base, size)` | Exclude from linear map |
| `memblock_remove(base, size)` | Remove from memory entirely |
| `memblock_phys_alloc(size, align)` | Allocate physical memory |
| `memblock_free_all()` | Hand free memory to buddy |

| Boot Phase | What Happens |
|---|---|
| head.S / EFI stub | Early page tables, MMU enable |
| `arm64_memblock_init()` | Discover + reserve physical memory |
| `paging_init()` | Build swapper_pg_dir with all RAM |
| `sparse_init()` | Allocate vmemmap (struct page) |
| `mm_init()` | Initialize zones, buddy |
| `memblock_free_all()` | Hand remaining pages to buddy |
