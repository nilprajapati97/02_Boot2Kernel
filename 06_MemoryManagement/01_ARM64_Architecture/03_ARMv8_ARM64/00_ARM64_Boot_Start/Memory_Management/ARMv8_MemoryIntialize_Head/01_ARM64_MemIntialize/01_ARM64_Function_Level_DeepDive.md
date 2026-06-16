# ARM64 Linux Kernel Memory Initialization — Function-Level Deep Dive

## Overview

This directory contains **in-depth, function-by-function documentation** of the entire ARM64 Linux kernel memory subsystem initialization path. Each document explains:

- **What** the function does
- **How** it works internally (algorithm, logic flow)
- **Which memory** it allocates, reserves, or maps
- **Data structures** it creates or modifies
- **Source file** and line references (latest kernel source)

## Document Map

### Entry Point

| Document | Description |
|----------|-------------|
| [01_MemoryIntializeFlow.md](01_MemoryIntializeFlow.md) | High-level overview of the entire memory init sequence |

---

### Phase 1 — Assembly Boot (MMU Off → MMU On)

**Directory:** [02_Phase1_Assembly_Boot/](02_Phase1_Assembly_Boot/)

The CPU starts with **MMU off**, running at physical addresses. This phase creates identity-mapped page tables, configures MMU hardware registers, enables the MMU, and transitions to virtual addresses and C code.

| # | Document | Function(s) Covered |
|---|----------|---------------------|
| 01 | [primary_entry](02_Phase1_Assembly_Boot/01_primary_entry.md) | `primary_entry()` — head.S entry, register setup, stack |
| 02 | [create_init_idmap](02_Phase1_Assembly_Boot/02_create_init_idmap.md) | `__pi_create_init_idmap()` — identity page table creation |
| 03 | [cpu_setup](02_Phase1_Assembly_Boot/03_cpu_setup.md) | `__cpu_setup()` — MAIR, TCR, SCTLR register configuration |
| 04 | [enable_mmu_primary_switch](02_Phase1_Assembly_Boot/04_enable_mmu_primary_switch.md) | `__primary_switch()`, `__enable_mmu()` — MMU enable |
| 05 | [primary_switched](02_Phase1_Assembly_Boot/05_primary_switched.md) | `__primary_switched()` — transition to C code |

**Allocator Available:** None. Memory comes from static BSS sections only.

---

### Phase 2 — setup_arch() & Memblock Era

**Directory:** [03_Phase2_SetupArch_Memblock/](03_Phase2_SetupArch_Memblock/)

The kernel discovers physical memory from the Device Tree, configures the memblock allocator, creates final kernel page tables, and sets up memory zones.

| # | Document | Function(s) Covered |
|---|----------|---------------------|
| 01 | [setup_arch](03_Phase2_SetupArch_Memblock/01_setup_arch.md) | `setup_arch()` — master orchestrator |
| 02 | [setup_machine_fdt](03_Phase2_SetupArch_Memblock/02_setup_machine_fdt.md) | `setup_machine_fdt()` — DTB parsing, memory discovery |
| 03 | [arm64_memblock_init](03_Phase2_SetupArch_Memblock/03_arm64_memblock_init.md) | `arm64_memblock_init()` — memblock trimming & reservations |
| 04 | [memblock_internals](03_Phase2_SetupArch_Memblock/04_memblock_internals.md) | Memblock allocator algorithm deep-dive |
| 05 | [paging_init](03_Phase2_SetupArch_Memblock/05_paging_init.md) | `paging_init()` — final page table creation |
| 06 | [map_mem](03_Phase2_SetupArch_Memblock/06_map_mem.md) | `map_mem()` — linear mapping of all physical RAM |
| 07 | [bootmem_init](03_Phase2_SetupArch_Memblock/07_bootmem_init.md) | `bootmem_init()` — zone discovery, DMA limits, CMA |

**Allocator Available:** Memblock (`memblock_alloc()`, `memblock_reserve()`).

---

### Phase 3 — Zone & Node Initialization

**Directory:** [04_Phase3_Zone_Node_Init/](04_Phase3_Zone_Node_Init/)

The kernel initializes per-node and per-zone data structures, allocates `struct page` arrays for all physical memory, and prepares the buddy allocator's free lists (empty at this point).

| # | Document | Function(s) Covered |
|---|----------|---------------------|
| 01 | [mm_core_init_early](04_Phase3_Zone_Node_Init/01_mm_core_init_early.md) | `mm_core_init_early()` — orchestrator |
| 02 | [free_area_init](04_Phase3_Zone_Node_Init/02_free_area_init.md) | `free_area_init()` — zone PFN calculation, page init |
| 03 | [sparse_init](04_Phase3_Zone_Node_Init/03_sparse_init.md) | `sparse_init()` — SPARSEMEM vmemmap setup |
| 04 | [zone_and_node_structures](04_Phase3_Zone_Node_Init/04_zone_and_node_structures.md) | Data structures: `struct zone`, `pglist_data`, `free_area`, `page` |

**Allocator Available:** Memblock (struct page arrays allocated from memblock).

---

### Phase 4 — Allocators Come Online

**Directory:** [05_Phase4_Allocators_Online/](05_Phase4_Allocators_Online/)

The kernel transitions from memblock to the buddy page allocator, then bootstraps SLUB slab allocator and vmalloc. After this phase, all runtime allocators are functional.

| # | Document | Function(s) Covered |
|---|----------|---------------------|
| 01 | [mm_core_init](05_Phase4_Allocators_Online/01_mm_core_init.md) | `mm_core_init()` — master orchestrator |
| 02 | [memblock_free_all](05_Phase4_Allocators_Online/02_memblock_free_all.md) | `memblock_free_all()` — memblock → buddy transition |
| 03 | [buddy_allocator](05_Phase4_Allocators_Online/03_buddy_allocator.md) | Buddy system: merging, splitting, migration types |
| 04 | [kmem_cache_init_slub](05_Phase4_Allocators_Online/04_kmem_cache_init_slub.md) | `kmem_cache_init()` — SLUB bootstrap |
| 05 | [vmalloc_init](05_Phase4_Allocators_Online/05_vmalloc_init.md) | `vmalloc_init()` — VA space management |
| 06 | [allocator_summary](05_Phase4_Allocators_Online/06_allocator_summary.md) | Timeline, address space layout, reference |

**Allocator Available:** All — buddy, SLUB (`kmalloc`), vmalloc, ioremap.

---

## Allocator Availability Timeline

```
Boot ──────────────────────────────────────────────────────► Runtime

Phase 1          Phase 2              Phase 3         Phase 4
Assembly Boot    setup_arch()         Zone/Node Init  Allocators Online
│                │                    │               │
│  None          │  Memblock          │  Memblock     │  Buddy + SLUB + vmalloc
│  (static BSS)  │  memblock_alloc()  │  (struct page │  kmalloc(), vmalloc(),
│                │  memblock_reserve() │   from mblk)  │  alloc_pages()
```

## Source Reference

All function references point to the kernel source tree at:
```
/home/nilprajapti/Workspace/nilprajapati97/KernelRepo/linux/
```
