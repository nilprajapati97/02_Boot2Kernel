# ARM64 (ARMv8) Linux Kernel — Boot-Time Memory Initialization

## From Scratch: How Linux Initializes Memory on Your ARMv8 Board

This document set explains — layer by layer, in boot order — exactly how the Linux
kernel's memory management subsystem comes to life on an ARMv8 (AArch64) board.
Every layer is a self-contained document. Read them in order (01 → 09) for the full
picture, or jump to any layer if you already understand the earlier stages.

---

## Reading Order & Document Map

```
  POWER ON
     │
     ▼
┌─────────────────────────────────────────────────────────────────────┐
│  Layer 1 — Hardware Entry & Bootloader                             │
│  ARMv8 basics, exception levels, bootloader (U-Boot/UEFI) loads    │
│  the kernel image + DTB, passes FDT pointer in x0, jumps to       │
│  kernel entry with MMU OFF.                                        │
│  File: 01_Hardware_Entry_and_Bootloader.md                         │
└──────────────────────────┬──────────────────────────────────────────┘
                           │
                           ▼
┌─────────────────────────────────────────────────────────────────────┐
│  Layer 2 — Early Assembly Boot (head.S)                            │
│  primary_entry: save FDT, create identity map (PA==VA), set up     │
│  MAIR/TCR/SCTLR, enable MMU. CPU now executes from identity map.   │
│  File: 02_Early_Assembly_Boot.md                                   │
└──────────────────────────┬──────────────────────────────────────────┘
                           │
                           ▼
┌─────────────────────────────────────────────────────────────────────┐
│  Layer 3 — Kernel Virtual Mapping & VA Layout                      │
│  early_map_kernel: map kernel at KIMAGE_VADDR, apply KASLR, switch │
│  TTBR1 to swapper_pg_dir, jump to virtual address. ARM64 VA layout.│
│  File: 03_Kernel_Virtual_Mapping.md                                │
└──────────────────────────┬──────────────────────────────────────────┘
                           │
                           ▼
┌─────────────────────────────────────────────────────────────────────┐
│  Layer 4 — Device Tree & Memory Discovery                          │
│  start_kernel → setup_arch: fixmap init, FDT parsing,              │
│  early_init_dt_scan_memory → memblock_add() for each /memory node. │
│  File: 04_Device_Tree_and_Memory_Discovery.md                      │
└──────────────────────────┬──────────────────────────────────────────┘
                           │
                           ▼
┌─────────────────────────────────────────────────────────────────────┐
│  Layer 5 — Memblock: The Early Boot Allocator                      │
│  memblock data structure, add/reserve/alloc operations,            │
│  arm64_memblock_init: clip to VA/PA limits, reserve kernel image.  │
│  File: 05_Memblock_Early_Allocator.md                              │
└──────────────────────────┬──────────────────────────────────────────┘
                           │
                           ▼
┌─────────────────────────────────────────────────────────────────────┐
│  Layer 6 — Page Tables & paging_init()                             │
│  map_mem: create linear mapping of ALL RAM in swapper_pg_dir using │
│  2MB/1GB block mappings. Create runtime identity map.              │
│  File: 06_Page_Tables_and_Paging_Init.md                           │
└──────────────────────────┬──────────────────────────────────────────┘
                           │
                           ▼
┌─────────────────────────────────────────────────────────────────────┐
│  Layer 7 — Zones, Nodes & struct page Initialization               │
│  bootmem_init, free_area_init: create NUMA nodes, memory zones     │
│  (DMA/DMA32/NORMAL/MOVABLE), initialize struct page for all RAM.   │
│  File: 07_Zones_Nodes_and_Page_Init.md                             │
└──────────────────────────┬──────────────────────────────────────────┘
                           │
                           ▼
┌─────────────────────────────────────────────────────────────────────┐
│  Layer 8 — The Great Handoff: memblock → Buddy Allocator           │
│  memblock_free_all: release all unreserved pages into buddy system.│
│  Buddy merging algorithm, free lists, alloc_pages() runtime flow.  │
│  File: 08_Buddy_Allocator_Handoff.md                               │
└──────────────────────────┬──────────────────────────────────────────┘
                           │
                           ▼
┌─────────────────────────────────────────────────────────────────────┐
│  Layer 9 — SLUB, vmalloc & Runtime Allocation                      │
│  SLUB slab allocator bootstrap, vmalloc init, post-boot cleanup,   │
│  how userspace processes get memory (mmap → page fault → buddy).   │
│  File: 09_Slab_Vmalloc_and_Runtime.md                              │
└──────────────────────────┬──────────────────────────────────────────┘
                           │
                           ▼
              SYSTEM FULLY OPERATIONAL
              Memory subsystem ready for
              kernel + userspace allocation
```

---

## Quick Reference

| Layer | File | Key Functions | Source Files |
|-------|------|--------------|-------------|
| 1 | [01_Hardware_Entry_and_Bootloader.md](01_Hardware_Entry_and_Bootloader.md) | — | — |
| 2 | [02_Early_Assembly_Boot.md](02_Early_Assembly_Boot.md) | `primary_entry`, `create_init_idmap`, `__cpu_setup`, `__enable_mmu` | `arch/arm64/kernel/head.S`, `arch/arm64/mm/proc.S` |
| 3 | [03_Kernel_Virtual_Mapping.md](03_Kernel_Virtual_Mapping.md) | `early_map_kernel`, `map_kernel`, `__primary_switched` | `arch/arm64/kernel/pi/map_kernel.c` |
| 4 | [04_Device_Tree_and_Memory_Discovery.md](04_Device_Tree_and_Memory_Discovery.md) | `setup_arch`, `early_init_dt_scan_memory` | `arch/arm64/kernel/setup.c`, `drivers/of/fdt.c` |
| 5 | [05_Memblock_Early_Allocator.md](05_Memblock_Early_Allocator.md) | `memblock_add`, `memblock_reserve`, `arm64_memblock_init` | `mm/memblock.c`, `arch/arm64/mm/init.c` |
| 6 | [06_Page_Tables_and_Paging_Init.md](06_Page_Tables_and_Paging_Init.md) | `paging_init`, `map_mem`, `__create_pgd_mapping` | `arch/arm64/mm/mmu.c` |
| 7 | [07_Zones_Nodes_and_Page_Init.md](07_Zones_Nodes_and_Page_Init.md) | `free_area_init`, `free_area_init_core`, `memmap_init` | `mm/mm_init.c` |
| 8 | [08_Buddy_Allocator_Handoff.md](08_Buddy_Allocator_Handoff.md) | `memblock_free_all`, `__free_one_page`, `alloc_pages` | `mm/memblock.c`, `mm/page_alloc.c` |
| 9 | [09_Slab_Vmalloc_and_Runtime.md](09_Slab_Vmalloc_and_Runtime.md) | `kmem_cache_init`, `vmalloc_init` | `mm/slub.c`, `mm/vmalloc.c` |
| Ref | [10_Appendix_Reference.md](10_Appendix_Reference.md) | — | — |

---

## High-Level Boot Memory Timeline

```
Time ──────────────────────────────────────────────────────────────────────►

│ Bootloader │ head.S  │ early_map │ setup_arch │ mm_core_init │ Runtime │
│ loads kern │ MMU ON  │ kernel VA │ FDT parse  │ buddy+slab   │ alloc   │
│ + DTB      │ ID map  │ mapping   │ memblock   │ vmalloc      │ ready   │
│            │         │ KASLR     │ paging_init│              │         │
├────────────┼─────────┼───────────┼────────────┼──────────────┼─────────┤
│   No MM    │ Phys==  │  Kernel   │  memblock  │ memblock →   │ buddy + │
│            │ Virt    │  at high  │  tracks    │ buddy handoff│ slab +  │
│            │         │  VA addr  │  all RAM   │ slab boot    │ vmalloc │
```

---

## Appendix & Reference

- [10_Appendix_Reference.md](10_Appendix_Reference.md) — Complete boot timeline diagram,
  key Kconfig options, data structure reference, source file table, glossary

---

## Assumptions

- **Board**: ARMv8-A (AArch64) — e.g., Raspberry Pi 4, i.MX8, Qualcomm Snapdragon
- **Bootloader**: U-Boot or UEFI (passes Flattened Device Tree in register x0)
- **Kernel config**: 4KB pages, 48-bit VA (most common), `CONFIG_SPARSEMEM`
- **Source**: Linux kernel source tree (mainline)

---

*Start reading: [Layer 1 — Hardware Entry & Bootloader](01_Hardware_Entry_and_Bootloader.md)*
