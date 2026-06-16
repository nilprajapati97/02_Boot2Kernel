# Phase 2: CPU & Architecture Initialization

## Context

After the very early setup (stack magic, CPU ID, debug objects, cgroup early, IRQs disabled), the kernel now performs the most critical and complex initialization: **architecture-specific hardware setup**.

This phase is where:
- The boot CPU is officially marked as "online" in the CPU bitmasks
- Physical memory is mapped and recorded
- Hardware features (CPU features, NUMA topology, memory banks) are detected
- The kernel's virtual address space is finalized
- Early security (LSM) framework is set up

---

## Functions in This Phase (in call order)

| # | Function | Source File | Purpose |
|---|----------|-------------|---------|
| 1 | `boot_cpu_init()` | `kernel/cpu.c` | Mark boot CPU as online/present/active |
| 2 | `page_address_init()` | `mm/page_alloc.c` | High memory page address tracking |
| 3 | `setup_arch()` | `arch/x86/kernel/setup.c` ★ | Architecture hardware setup (BIGGEST function) |
| 4 | `mm_core_init_early()` | `mm/mm_init.c` | Early zone list setup |
| 5 | `jump_label_init()` | `kernel/jump_label.c` | Static branch/jump label infrastructure |
| 6 | `static_call_init()` | `kernel/static_call.c` | Static call patching |
| 7 | `early_security_init()` | `security/lsm_init.c` | Early LSM framework |

---

## Key Achievement of Phase 2

After this phase:
- `memblock` allocator has a complete map of all physical RAM
- Kernel virtual address space is finalized (paging fully enabled)
- All CPU hardware features are detected and recorded in `boot_cpu_data`
- Static branch patching is ready (prerequisite for LSM hooks)
- Early security modules have initialized their data structures

---

## Navigation

- [01 boot_cpu_init](01_boot_cpu_init/README.md)
- [02 page_address_init](02_page_address_init/README.md)
- [03 setup_arch](03_setup_arch/README.md) ← Most complex
- [04 mm_core_init_early](04_mm_core_init_early/README.md)
- [05 jump_label_init](05_jump_label_init/README.md)
- [06 static_call_init](06_static_call_init/README.md)
- [07 early_security_init](07_early_security_init/README.md)
