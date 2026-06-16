# Memory Map Detection — E820, UEFI, and memblock

## Physical Memory Discovery

The kernel must know where physical RAM is before it can do anything. Sources differ by platform:

| Platform | Memory Map Source | Mechanism |
|---------|-------------------|-----------|
| Legacy x86 BIOS | E820 table | INT 15h, AX=E820h |
| UEFI x86/ARM64 | EFI Memory Map | `GetMemoryMap()` service |
| ARM64 embedded | Device Tree | `/memory` node |
| RISC-V | Device Tree or ACPI | `/memory` node |

---

## E820 Table (Legacy BIOS)

The BIOS E820 interface returns an array of descriptors:

```c
struct e820_entry {
    u64 addr;       // physical base address
    u64 size;       // length in bytes
    u32 type;       // E820_TYPE_*
};
```

Example E820 table for a 8GB system:
```
E820:  [0x00000000000 - 0x0009FFFF]  640KB  RAM
E820:  [0x000A0000000 - 0x000FFFFF]  384KB  RESERVED  (BIOS ROM, VGA)
E820:  [0x00100000000 - 0x09EFFFFF]  ~8GB   RAM
E820:  [0x09F000000 - 0x09FFFFFF]    1MB    RESERVED  (ACPI)
E820:  [0x0FEC00000 - 0x0FEC0FFFF]   64KB   RESERVED  (I/O APIC MMIO)
E820:  [0xFFFFF0000 - 0xFFFFFFFFF]   64KB   RESERVED  (BIOS ROM)
```

## UEFI Memory Map

UEFI provides richer information:
```c
typedef struct {
    UINT32  Type;           // EfiConventionalMemory, EfiMemoryMappedIO, etc.
    UINT64  PhysicalStart;
    UINT64  VirtualStart;   // (for SetVirtualAddressMap)
    UINT64  NumberOfPages;
    UINT64  Attribute;      // EFI_MEMORY_WB, UC, WC, etc.
} EFI_MEMORY_DESCRIPTOR;
```

## memblock Allocator

```c
// include/linux/memblock.h
struct memblock {
    bool            bottom_up;   // allocation direction
    phys_addr_t     current_limit;
    struct memblock_type memory;   // available regions
    struct memblock_type reserved; // reserved regions
};

struct memblock_type {
    unsigned long     cnt;       // number of regions
    unsigned long     max;       // max regions (before resizing)
    phys_addr_t       total_size;
    struct memblock_region *regions;
};
```

### memblock API

```c
// Add available RAM
memblock_add(base, size);

// Reserve (prevent allocation)
memblock_reserve(base, size);

// Allocate from available memory
ptr = memblock_alloc(size, align);

// Free (only valid before buddy allocator starts)
memblock_free(base, size);

// Iterate all available regions
for_each_mem_range(i, &start, &end) { ... }
```

## Interview Q&A

### Q1: What happens to memblock after the buddy allocator is set up?
**A:** `mm_core_init()` calls `memblock_free_all()` which iterates all memblock regions that are `available` (not reserved) and calls `__free_pages()` on each — handing them to the buddy allocator. After this, `memblock_alloc()` is no longer valid. The memblock structure itself (arrays of `memblock_region`) was allocated from early memblock and is now freed to the buddy allocator as well. From this point, all memory allocation goes through `kmalloc()` / `alloc_pages()`.

### Q2: How does the kernel handle "bad RAM" (hardware memory errors)?
**A:** The BIOS E820 table marks bad pages as `E820_TYPE_UNUSABLE`. Alternatively, ACPI MCE (Machine Check Exceptions) reports corrected memory errors. The kernel's `mce-inject` mechanism and EDAC (Error Detection And Correction) subsystem track bad pages and poison them (set `PG_hwpoison` flag). `hwpoison_filter` allows excluding certain pages. Poisoned pages are never allocated — they are removed from the buddy allocator's free lists. NVIDIA GPUs do similar ECC memory management at the GPU VRAM level.

### Q3: What is the kernel's direct mapping and how large is it?
**A:** The direct mapping (also called `linear map` or `physmap`) is a contiguous virtual address region that maps all physical RAM. On x86-64, it starts at `PAGE_OFFSET = 0xffff888000000000` (for 4-level paging). For a server with 1TB RAM, the direct map covers 1TB of virtual address space. The macro `__va(pa) = (void *)(pa + PAGE_OFFSET)` converts physical to virtual. This is why `virt_to_phys()` and `phys_to_virt()` are just additions on 64-bit — no TLB walk needed for kernel virtual addresses.
