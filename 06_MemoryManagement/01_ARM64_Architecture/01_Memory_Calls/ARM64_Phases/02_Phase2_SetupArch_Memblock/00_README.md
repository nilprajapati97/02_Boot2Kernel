# Phase 2 — setup_arch() & Memblock Era

## Overview

With the MMU enabled and C code running, the kernel now needs to:

1. **Discover** what physical memory exists (from the Device Tree)
2. **Register** all RAM with the memblock allocator
3. **Reserve** regions that must not be used (kernel image, DTB, initrd)
4. **Create final page tables** mapping all physical RAM into the kernel's linear map
5. **Set up memory zones** (DMA, DMA32, NORMAL) for the upcoming buddy allocator

The **memblock allocator** is the primary memory management system during this phase. It maintains two simple arrays:
- `memblock.memory` — all physical RAM regions
- `memblock.reserved` — regions claimed by kernel/DTB/firmware

## Call Graph

```
start_kernel()                                    ← init/main.c
└── setup_arch(&command_line)                     ← arch/arm64/kernel/setup.c
    │
    ├── setup_initial_init_mm()                   ← Record kernel VA boundaries
    ├── early_fixmap_init()                       ← Fixed virtual address mappings
    ├── early_ioremap_init()                      ← Boot-time I/O remapping
    │
    ├── setup_machine_fdt(__fdt_pointer)           ← Parse DTB
    │   ├── fixmap_remap_fdt()                    ← Map DTB via fixmap
    │   └── early_init_dt_scan()                  ← Parse /memory nodes
    │       └── memblock_add(base, size)          ← Register RAM with memblock
    │
    ├── arm64_memblock_init()                     ← Configure memblock
    │   ├── Calculate memstart_addr               ← Base of linear map
    │   ├── memblock_remove(...)                  ← Trim unmappable memory
    │   ├── memblock_reserve(kernel)              ← Reserve kernel image
    │   ├── memblock_reserve(initrd)              ← Reserve initrd
    │   └── early_init_fdt_scan_reserved_mem()    ← DTB reserved regions
    │
    ├── paging_init()                             ← Create final page tables
    │   ├── map_mem(swapper_pg_dir)               ← Map all RAM into linear map
    │   │   ├── memblock_mark_nomap(kernel_text)  ← Avoid writable alias
    │   │   ├── for_each_mem_range()              ← Walk memblock regions
    │   │   │   └── __map_memblock()              ← Create PGD→PTE mappings
    │   │   └── Re-map kernel text (no-exec)
    │   ├── memblock_allow_resize()               ← Allow memblock arrays to grow
    │   ├── create_idmap()                        ← Recreate identity map
    │   └── declare_kernel_vmas()                 ← Register kernel VMAs
    │
    └── bootmem_init()                            ← Zone & NUMA setup
        ├── PFN range: min_low_pfn, max_pfn
        ├── arch_numa_init()                      ← NUMA topology
        ├── dma_limits_init()                     ← DMA zone boundaries
        ├── dma_contiguous_reserve()              ← CMA regions
        └── memblock_dump_all()                   ← Print final state
```

## Memory Operations Summary

| Function | memblock ops | What it does |
|----------|-------------|--------------|
| `setup_machine_fdt()` | `add` × N, `reserve` × 1 | Adds RAM banks, reserves DTB |
| `arm64_memblock_init()` | `remove` × 5, `reserve` × 2 | Trims + reserves kernel/initrd |
| `paging_init()` | `mark_nomap` × 1, reads regions | Creates page tables from memblock data |
| `bootmem_init()` | reads only | Calculates zone boundaries from memblock |

## Key Concept: Linear Mapping

The **linear map** (also called "direct map") is a 1:1 mapping of all physical RAM into kernel virtual address space:

```
Virtual Address = Physical Address - memstart_addr + PAGE_OFFSET

Where:
  PAGE_OFFSET = 0xFFFF_0000_0000_0000  (48-bit VA)
  memstart_addr = base of DRAM (aligned)
```

Every byte of physical RAM has a corresponding virtual address in this region, enabling the kernel to access any physical page via simple pointer arithmetic.

## Documents

| # | Document | Covers |
|---|----------|--------|
| 01 | [setup_arch.md](01_setup_arch.md) | Master orchestrator — full call sequence |
| 02 | [setup_machine_fdt.md](02_setup_machine_fdt.md) | DTB parsing, memory bank discovery |
| 03 | [arm64_memblock_init.md](03_arm64_memblock_init.md) | Memblock trimming, reservations |
| 04 | [memblock_internals.md](04_memblock_internals.md) | Memblock allocator algorithm deep-dive |
| 05 | [paging_init.md](05_paging_init.md) | Final page table creation |
| 06 | [map_mem.md](06_map_mem.md) | Linear mapping of all physical RAM |
| 07 | [bootmem_init.md](07_bootmem_init.md) | Zone discovery, DMA limits, CMA |

## Zone Layout After This Phase

| Zone | Physical Range | Purpose |
|------|---------------|---------|
| `ZONE_DMA` | 0 → `zone_dma_limit` | Devices with limited DMA addressing |
| `ZONE_DMA32` | 0 → 4 GB | 32-bit DMA-capable devices |
| `ZONE_NORMAL` | 4 GB → `max_pfn` | General purpose memory |
