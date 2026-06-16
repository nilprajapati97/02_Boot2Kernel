# Phase 1: Very Early Initialization

## Context

This is the **absolute first phase** of `start_kernel()`. The CPU is running in 64-bit mode, paging is active, but the system is in an extremely fragile state:

- **Interrupts are disabled** (ensured by arch assembly prologue and `local_irq_disable()`)
- **Only one CPU is active** (the boot CPU — all others are halted in INIT/SIPI wait loop)
- **No dynamic memory allocation** — slab/buddy allocators not yet set up
- **No console output** — `printk()` buffers output but nothing displays yet

---

## Functions in This Phase (in call order)

| # | Function | Source File | Purpose |
|---|----------|-------------|---------|
| 1 | `set_task_stack_end_magic()` | `kernel/fork.c` | Stack overflow canary for init_task |
| 2 | `smp_setup_processor_id()` | arch-specific | Set logical/physical CPU ID |
| 3 | `debug_objects_early_init()` | `lib/debugobjects.c` | Object lifecycle debugging |
| 4 | `init_vmlinux_build_id()` | `kernel/buildid.c` | Embed build ID in kernel |
| 5 | `cgroup_init_early()` | `kernel/cgroup/cgroup.c` | Cgroup subsystem early bootstrap |
| 6 | `local_irq_disable()` | arch inline / `include/linux/irqflags.h` | Explicitly disable all local IRQs |

---

## Why These Run First

These functions share a common property: they **require nothing** — no memory allocator, no scheduler, no IRQ controller. They only touch:
- **Statically allocated** kernel data structures
- **CPU registers** (flags register for IRQ disable)
- **Fixed-size hash tables** initialized from BSS (debug objects)

Any failure at this stage means an immediate boot failure with no recovery possible.

---

## State After Phase 1

- Boot CPU's `init_task.stack` has a stack end magic value (overflow detection armed)
- Boot CPU has a valid logical processor ID
- Debug object subsystem can track object state
- Early cgroup root created
- IRQs explicitly disabled, `early_boot_irqs_disabled = true`

---

## Navigation

- [01 set_task_stack_end_magic](01_set_task_stack_end_magic/README.md)
- [02 smp_setup_processor_id](02_smp_setup_processor_id/README.md)
- [03 debug_objects_early_init](03_debug_objects_early_init/README.md)
- [04 init_vmlinux_build_id](04_init_vmlinux_build_id/README.md)
- [05 cgroup_init_early](05_cgroup_init_early/README.md)
- [06 local_irq_disable](06_local_irq_disable/README.md)
