# Phase 4: Memory Management Initialization

## Overview

This phase establishes the full kernel memory subsystem — from the exception table and interrupt vectors to the slab allocator and NUMA policy. After this phase, the kernel has a fully functional memory allocator (`kmalloc`, `vmalloc`, slab caches) available.

## Execution Order

| # | Function | Source File | Description |
|---|----------|-------------|-------------|
| 1 | [`setup_log_buf()`](setup_log_buf/README.md) | `kernel/printk/printk.c` | Allocate persistent printk ring buffer |
| 2 | [`vfs_caches_init_early()`](vfs_caches_init_early/README.md) | `fs/dcache.c` | Early dentry/inode slab caches |
| 3 | [`sort_main_extable()`](sort_main_extable/README.md) | `lib/extable.c` | Sort exception table for binary search |
| 4 | [`trap_init()`](trap_init/README.md) | `arch/x86/kernel/traps.c` | Install IDT exception handlers |
| 5 | [`mm_core_init()`](mm_core_init/README.md) | `mm/mm_init.c` | Core memory management init |
| 6 | [`kmem_cache_init_late()`](kmem_cache_init_late/README.md) | `mm/slab.c` | Finalize slab allocator |
| 7 | [`setup_per_cpu_pageset()`](setup_per_cpu_pageset/README.md) | `mm/page_alloc.c` | Per-CPU page set caches |
| 8 | [`numa_policy_init()`](numa_policy_init/README.md) | `mm/mempolicy.c` | Default NUMA memory policy |

## IRQ State

- **Entry**: Disabled
- **Exit**: Disabled (IRQs still off until `local_irq_enable()` in Phase 9)

## Memory State Progression

```
Enter Phase 4:
  - memblock: available (from setup_arch())
  - buddy allocator: NOT yet available
  - slab/kmalloc: NOT available

After mm_core_init():
  - buddy allocator: FULLY available
  - memblock: still available (but will be freed later)

After kmem_cache_init_late():
  - kmalloc(): fully available
  - slab caches: fully available
  - vmalloc(): available
```

## Key Subsystems Initialized

- **printk ring buffer**: Persistent kernel log storage
- **Exception table**: Sorted for fast fault recovery (copy_from_user etc.)
- **IDT** (x86): All CPU exception and trap handlers installed
- **Buddy allocator**: Full page allocator operational
- **Slab allocator**: Object-level allocation (`kmalloc`, `kzalloc`, `kfree`)
- **Per-CPU page sets**: Per-CPU hot-page caches for allocation speedup
- **NUMA policy**: Default interleave/local-alloc policy

## Dependencies on Prior Phases

- `setup_arch()` (Phase 2): Initialized `memblock`, e820 map, per-CPU areas
- `setup_per_cpu_areas()` (Phase 2): Per-CPU memory allocated
- `parse_early_param()` (Phase 3): `mem=` cmdline restrictions applied

## Function Index

- [setup_log_buf/](setup_log_buf/README.md)
- [vfs_caches_init_early/](vfs_caches_init_early/README.md)
- [sort_main_extable/](sort_main_extable/README.md)
- [trap_init/](trap_init/README.md)
- [mm_core_init/](mm_core_init/README.md)
- [kmem_cache_init_late/](kmem_cache_init_late/README.md)
- [setup_per_cpu_pageset/](setup_per_cpu_pageset/README.md)
- [numa_policy_init/](numa_policy_init/README.md)
