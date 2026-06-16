# arm_memblock_init() — System Design Approach and Q&A

## 1. Why arm_memblock_init() Exists: The Memory Map Finalization Problem

At the point `arm_memblock_init()` is called, the kernel has:
- A raw memory map from FDT/ATAG memory nodes (what RAM exists)
- EFI reservations applied (if UEFI boot)
- An initial lowmem/highmem boundary (from first `adjust_lowmem_bounds()`)

But the memory map is **incomplete**. Several critical regions are not yet protected:
1. The kernel image itself (code, data, BSS)
2. The initrd (initial RAM disk)
3. Page table physical memory
4. Board-specific hardware-reserved regions
5. DMA contiguous pool

`arm_memblock_init()` performs the **reservation phase** — transforming the raw memory map into a map that accurately reflects what memory the kernel can freely use.

---

## 2. Design Principle: Early Reservation Before Allocation

The fundamental invariant of early memory management:

```
RESERVE → ALLOCATE (NEVER allocate first, then try to reserve)

Violated order:
  paging_init() allocates page tables
  arm_memblock_init() tries to reserve kernel image
  Problem: kernel image is in an unprotected range;
           paging_init() might have allocated it for page tables

Correct order:
  arm_memblock_init() reserves kernel image, initrd, etc.
  paging_init() allocates page tables (memblock_alloc finds free memory only)
```

This is enforced by the position of `arm_memblock_init()` in `setup_arch()`: **before** `paging_init()`, **before** `early_ioremap_reset()`.

---

## 3. The memblock Lifecycle

```
Phase 1: Population (FDT scan, before setup_arch)
  head.S → start_kernel → setup_arch
  early_init_dt_scan_memory() adds all RAM to memblock.memory[]

Phase 2: Adjustment (adjust_lowmem_bounds #1)
  Computes vmalloc_limit, sets memblock current limit

Phase 3: Reservation (arm_memblock_init) ← THIS FUNCTION
  All "must-not-allocate" regions added to memblock.reserved[]
  arm_memblock_steal_permitted = false (lock)

Phase 4: Correction (adjust_lowmem_bounds #2)
  Recalculates after reservations changed available memory

Phase 5: Page table setup (paging_init)
  memblock_alloc() used for page tables (within reserved bounds)
  Direct map built for lowmem

Phase 6: Handoff (mem_init, called from paging_init)
  All free memblock regions released to buddy allocator
  memblock tables freed (usually freed to page allocator)
```

After Phase 6, memblock is largely unused. The buddy allocator takes over.

---

## 4. CMA Design: Why Reserve During memblock Phase

CMA (Contiguous Memory Allocator) must be reserved during the memblock phase because:

```
Why early reservation is critical for CMA:

T=0 (memblock phase):
  Physical memory: [0x0....0x3FFFFFFF] — all free
  Reserve CMA at top: [0x38000000 - 0x3FFFFFFF] (128MB)

T=1 (buddy allocator initialized):
  CMA region is marked "movable" in the buddy allocator
  Pages within CMA are given to page allocator as movable pages
  Kernel uses these pages for cached file data, anon pages

T=2 (camera driver requests 64MB contiguous buffer):
  CMA migrates all movable pages OUT of the target 64MB range
  (migrates: moves their content to other free pages, updates PTEs)
  Returns the 64MB contiguous physical range to the driver

Without early reservation:
  Some pages in 0x38000000-0x3FFFFFFF range would be allocated
  for non-movable data (e.g., page table pages, slab caches)
  CMA can't migrate non-movable pages → can't provide contiguous range
  Camera driver fails
```

---

## 5. Dependency Graph

```
[FDT /memory nodes]            → memblock populated (before setup_arch)
[ATAG_MEM]                     → memblock populated (legacy)
[EFI memory map]               → memblock populated (arm_efi_init)
          │
          ▼
[arm_lowmem_limit]             (from adjust_lowmem_bounds #1)
[arm_dma_limit]                (from setup_dma_zone)
          │
          ▼
[arm_memblock_init(mdesc)]
  ├── memblock.reserved[] ←─── kernel image, initrd, page tables
  ├── memblock.reserved[] ←─── mdesc->reserve() output
  ├── memblock.reserved[] ←─── FDT /reserved-memory nodes
  ├── memblock.reserved[] ←─── CMA pool
  └── arm_memblock_steal_permitted = false
          │
          ▼
[adjust_lowmem_bounds #2]      → corrected arm_lowmem_limit
          │
          ▼
[paging_init]                  → maps lowmem, allocates page tables
          │
          ▼
[mem_init]                     → frees free memblock to buddy allocator
          │
          ▼
[/proc/meminfo]                → MemTotal = sum of freed memblock regions
```

---

## 6. Board Reservation Patterns (mdesc->reserve)

Different board types have different reservation patterns:

### Pattern 1: Framebuffer Pre-allocated by Bootloader

Some bootloaders allocate a framebuffer before calling the kernel. The kernel must not free this region (the display would go blank):

```c
/* Example board reserve() */
static void __init myboard_reserve(void)
{
    /* Bootloader pre-allocated 8MB framebuffer at fixed address */
    memblock_reserve(0x1F800000, SZ_8M);
    /* Kernel will remap this as write-combine for the display driver */
}
```

### Pattern 2: Secure World (TrustZone) Reservation

ARM TrustZone assigns memory to the secure world. The non-secure kernel must not touch it:

```c
static void __init myboard_reserve(void)
{
    /* TrustZone secure world: 16MB at top of RAM */
    memblock_reserve(0x3F000000, SZ_16M);
    /* If kernel tries to map this: may trigger secure abort */
}
```

### Pattern 3: DSP/Coprocessor Memory

Systems with a DSP alongside ARM:

```c
static void __init myboard_reserve(void)
{
    /* DSP firmware loaded here by primary bootloader */
    memblock_reserve(DSP_MEM_BASE, DSP_MEM_SIZE);
}
```

Modern boards express all these as `/reserved-memory` nodes in the DTB, making `mdesc->reserve = NULL` possible.

---

## 7. System Design Q&A

**Q: What is the difference between memblock_reserve() and memblock_mark_nomap()?**
> `memblock_reserve()` marks a region as "in use" — the allocator won't give it out. The region is still mapped by `paging_init()` and accessible by the kernel. Used for kernel image, initrd, CMA (until CMA initializes and frees it). `memblock_mark_nomap()` marks a region that exists in hardware but should neither be allocated NOR mapped. Used for hardware-owned regions (TrustZone, GPU firmwares, EFI reserved firmware). Accessing a NOMAP region generates a page fault because no PTE exists. NOMAP is stronger than reserve.

**Q: How does memblock know the kernel image physical address if KASLR changes the load address?**
> The kernel is linked to run at a virtual address (e.g., 0xC0008000 on ARM32). `__pa(KERNEL_START)` subtracts `PAGE_OFFSET` and adds `PHYS_OFFSET` to convert to physical. On ARM32 without KASLR, `PHYS_OFFSET` is fixed and the conversion is deterministic. The kernel was loaded by the bootloader at exactly `PHYS_OFFSET + (KERNEL_START - PAGE_OFFSET)`. On ARM64 with KASLR, `memstart_addr` is the randomized physical start, and `__pa()` uses `memstart_addr` which is set during head.S execution before reaching `arm64_memblock_init()`.

**Q: What happens if the initrd overlaps with the kernel image?**
> `reserve_initrd_mem()` checks `memblock_is_region_memory(start, size)` but does not explicitly check for overlap with the kernel reservation. However, since both `memblock_reserve(KERNEL_START, ...)` and `memblock_reserve(initrd_start, ...)` operate on disjoint address ranges (the bootloader places them separately), overlap is unlikely. If overlap somehow occurred (buggy bootloader), `memblock_reserve()` would create two overlapping entries in `.reserved[]` — memblock handles this by merging them. The kernel would reserve the union. The initrd region within the kernel image would remain protected (but the initrd content would be overwritten by BSS zeroing or relocation, causing a corrupt rootfs mount).

**Q: What is arm_memblock_steal() and when would a driver use it?**
> `arm_memblock_steal()` permanently removes physical memory from memblock — it's neither reserved nor available; it simply disappears from the system. This is used for memory that must be physically contiguous, permanently allocated, and not tracked by any allocator (e.g., initial page table memory, the zero page). Unlike `memblock_alloc()` which reserves memory that will later be freed to the buddy allocator, stolen memory is gone forever. `arm_memblock_steal_permitted` being false after `arm_memblock_init()` prevents late callers from permanently removing memory.

**Q: How does the /reserved-memory "no-map" property affect driver access later?**
> A `no-map` region is never mapped in the kernel's page tables. If a driver later needs to access it (e.g., a camera driver needs to write to the GPU framebuffer), it must use `ioremap()` to create a new mapping for that physical address range. `ioremap()` creates a PTE in the vmalloc region pointing to the physical address. The driver accesses the memory via the ioremap VA. When the driver is done, `iounmap()` removes the mapping. This is the correct pattern for hardware-reserved memory: no default mapping, explicit ioremap per-user.
