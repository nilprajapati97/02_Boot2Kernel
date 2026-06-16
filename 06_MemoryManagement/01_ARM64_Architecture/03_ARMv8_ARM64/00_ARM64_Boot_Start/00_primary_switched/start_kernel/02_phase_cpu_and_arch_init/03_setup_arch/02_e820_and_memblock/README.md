# E820 Table and memblock Allocator — Deep Dive

## E820 Table Processing Flow

```
BIOS INT 15h E820 call (done by bootloader or EFI stub)
        │
        ▼
boot_params.e820_table[]      ← raw entries copied from real-mode
        │
        ▼
e820__memory_setup()          ← sanitize, merge overlapping entries
        │
        ▼
e820_table                    ← canonical E820 table (max 128 entries)
        │
        ▼
memblock_add()  (for RAM entries)
memblock_reserve() (for reserved entries)
        │
        ▼
memblock.memory    ← list of available physical address ranges
memblock.reserved  ← list of reserved ranges
```

## memblock Internal Structure

```
memblock.memory (available RAM):
  [0x00001000 - 0x0009EFFF]  608KB    (low memory below BIOS hole)
  [0x00100000 - 0x7FFFFFFF]  2047MB   (main RAM)
  [0x80000000 - 0xBFFFFFFF]  1024MB   (more RAM)

memblock.reserved (kernel + initrd + ACPI):
  [0x01000000 - 0x0200FFFF]  16MB     (kernel image: _text to _end)
  [0x10000000 - 0x12FFFFFF]  48MB     (initrd)
  [0x7FE00000 - 0x7FFFFFFF]  2MB      (ACPI tables)
```

## Zone Boundaries After memblock

After E820 processing, `setup_arch` calls `free_area_init()` to create memory zones:

```
ZONE_DMA:     [0 .. 16MB]      DMA-capable devices (ISA legacy)
ZONE_DMA32:   [16MB .. 4GB]    32-bit DMA devices
ZONE_NORMAL:  [4GB .. RAM end] Regular allocation
(ZONE_HIGHMEM: only on 32-bit kernels)
```

## Interview Q&A

### Q1: Why does ZONE_DMA exist in 2024 kernels?
**A:** Legacy ISA DMA controllers can only address 24-bit (16MB) physical addresses. Modern PCIe devices don't have this limitation, but old sound cards, floppy controllers, and some embedded SoC DMA engines do. The kernel reserves `ZONE_DMA` to satisfy `GFP_DMA` allocations — `kmalloc(size, GFP_DMA)` guarantees a physical address below 16MB. Most modern allocations use `GFP_KERNEL` which can use any zone.

### Q2: What is the `min_low_pfn`, `max_low_pfn`, `max_pfn` hierarchy?
**A:**
- `min_low_pfn`: Lowest usable PFN (page frame number), typically 1 (skip PFN 0 which is BIOS data area)
- `max_low_pfn`: Highest PFN in ZONE_NORMAL (32-bit: 896MB boundary; 64-bit: not meaningful)
- `max_pfn`: Highest PFN in the system (top of RAM)
- `max_pfn_mapped`: Highest PFN mapped in the direct mapping

These define the boundaries used by `free_area_init()` to size the zones.

### Q3: How does memblock handle fragmented physical memory (e.g., holes from MMIO)?
**A:** `memblock` maintains separate `memory` and `reserved` type arrays. The `available` ranges are computed as `memory - reserved`. When initializing the buddy allocator, the kernel iterates available memblock regions and calls `__free_pages()` only for physically contiguous segments within each zone. Holes (MMIO gaps, ACPI reserved) are naturally skipped. The `struct page` array (vmemmap) is sparse — `pfn_valid(pfn)` checks if a PFN has a valid `struct page` before accessing it.
