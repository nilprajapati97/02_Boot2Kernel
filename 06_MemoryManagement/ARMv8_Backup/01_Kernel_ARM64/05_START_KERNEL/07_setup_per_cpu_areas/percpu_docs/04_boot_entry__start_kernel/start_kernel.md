# `start_kernel()` — Boot Entry Point and Per-CPU Context

## Source Reference
- `init/main.c:854` — `start_kernel()` definition
- `init/main.c:900` — `setup_nr_cpu_ids()` call
- `init/main.c:901` — `setup_per_cpu_areas()` call
- `init/main.c:902` — `smp_prepare_boot_cpu()` call

---

## Position of `setup_per_cpu_areas()` in Boot Sequence

```c
/* init/main.c — simplified, showing surrounding calls */
asmlinkage __visible void __init __no_sanitize_address start_kernel(void)
{
    char *command_line;
    char *after_dashes;

    /* Very early setup — no per-CPU access yet */
    set_task_stack_end_magic(&init_task);
    smp_setup_processor_id();        /* sets __smp_processor_id for boot CPU */
    debug_objects_early_init();
    init_vmlinux_build_id();

    cgroup_init_early();
    local_irq_disable();
    early_boot_irqs_disabled = true;

    boot_cpu_init();                 /* marks CPU0 as present/active/online */
    page_address_init();
    pr_notice("%s", linux_banner);
    early_security_init();

    setup_arch(&command_line);       /* ARCH-SPECIFIC: mem map, page tables */
    setup_boot_config();
    setup_command_line(command_line);
    setup_nr_cpu_ids();              /* LINE ~900: set nr_cpu_ids from cpumask */

    setup_per_cpu_areas();           /* LINE  901: *** PER-CPU SETUP *** */

    smp_prepare_boot_cpu();          /* LINE  902: write hw register for CPU0 */

    boot_cpu_hotplug_init();
    build_all_zonelists(NULL);
    page_alloc_init();
    /* ... many more subsystems ... */
    rest_init();                     /* spawns init thread, starts scheduler */
}
```

---

## Why `setup_per_cpu_areas()` Comes At This Specific Point

### Must come AFTER:
- `setup_arch()` — required because:
  - `memblock_init()` must have run (bootmem allocator ready)
  - Physical memory map must be built (`e820__memblock_setup()` on x86,
    `arm_memblock_init()` on ARM, `arm64_memblock_init()` on ARM64)
  - Page tables must be set up (virtual addresses usable)
  - `__per_cpu_load`, `__per_cpu_start`, `__per_cpu_end` linker symbols accessible

- `setup_nr_cpu_ids()` — required because:
  - `nr_cpu_ids` is used in `pcpu_build_alloc_info()` to allocate the `cpu_map` arrays
  - `for_each_possible_cpu()` iterates over `cpu_possible_mask` which must be finalized

### Must come BEFORE:
- `smp_prepare_boot_cpu()` — this function writes the hardware register (`TPIDRPRW` /
  `tpidr_el1`), which requires `__per_cpu_offset[0]` to be valid. That value is set
  inside `setup_per_cpu_areas()`.

- Most of the rest of `start_kernel()` — many kernel subsystems use per-CPU variables.
  For example:
  - `build_all_zonelists()` uses `per_cpu(boot_pageset, cpu)`
  - `page_alloc_init()` uses per-CPU page allocator state
  - `rcu_init()` uses per-CPU RCU state
  - Scheduler initialization uses per-CPU runqueues

---

## Pre-`setup_per_cpu_areas()` Per-CPU State

Before `setup_per_cpu_areas()` runs, any access to `this_cpu_*` variables is **undefined
behavior** (the hardware register holds garbage or 0). However, some early boot code does
carefully reference `__per_cpu_offset[0]` knowing the template is at offset 0:

```
On ARM32 & ARM64:
  Before setup_per_cpu_areas():
    __per_cpu_offset[0] = 0  (BSS zero-initialized)
    TPIDRPRW / tpidr_el1 = undefined (not written yet)

  After setup_per_cpu_areas(), before smp_prepare_boot_cpu():
    __per_cpu_offset[0] = correct value
    TPIDRPRW / tpidr_el1 = still undefined!

  After smp_prepare_boot_cpu():
    TPIDRPRW / tpidr_el1 = __per_cpu_offset[0]  ← correct
    this_cpu_*() now works correctly on CPU0
```

### Special case: PowerPC
PowerPC differs here — it has an arch-specific `setup_per_cpu_areas()` at
`arch/powerpc/kernel/setup_64.c` that also writes `paca_ptrs[cpu]->data_offset`.
The PACA (Processor Area Control Area) serves as PowerPC's per-CPU base structure.
This is **not** the ARM pattern.

---

## `setup_nr_cpu_ids()` — The Call Just Before

```c
/* kernel/smp.c */
void __init setup_nr_cpu_ids(void)
{
    nr_cpu_ids = find_last_bit(cpumask_bits(cpu_possible_mask),
                               NR_CPUS) + 1;
}
```

This sets `nr_cpu_ids` to the highest possible CPU ID + 1.
`pcpu_embed_first_chunk()` uses `nr_cpu_ids` to size the `cpu_map` arrays and
`__per_cpu_offset[]` array.

On ARM with DT (Device Tree): `cpu_possible_mask` is set by `arm_dt_init_cpu_maps()`
during `setup_arch()`.

On ARM64 with DT: set by `smp_init_cpus()` early in `setup_arch()`.

---

## `smp_prepare_boot_cpu()` — The Call Just After

### ARM32 (`arch/arm/kernel/smp.c:500`):
```c
void __init smp_prepare_boot_cpu(void)
{
    set_my_cpu_offset(per_cpu_offset(smp_processor_id()));
    /* writes TPIDRPRW = __per_cpu_offset[0] */
}
```

### ARM64 (`arch/arm64/kernel/smp.c:456`):
```c
void __init smp_prepare_boot_cpu(void)
{
    /*
     * The runtime per-cpu areas have been allocated by
     * setup_per_cpu_areas(), and CPU0's boot time per-cpu area will
     * be freed shortly, so we must move over to the runtime per-cpu
     * area.
     */
    set_my_cpu_offset(per_cpu_offset(smp_processor_id()));
    /* writes tpidr_el1 = __per_cpu_offset[0] */
    /* (or tpidr_el2 if VHE patching was applied) */
}
```

The comment "boot time per-cpu area will be freed" is important: during very early boot,
the kernel's per-CPU variables point to the **static image** (the template itself at
`__per_cpu_start`). After `setup_per_cpu_areas()`, the real runtime areas are allocated.
`smp_prepare_boot_cpu()` switches CPU0 from using the static image to using the newly
allocated runtime area.

---

## Interview Quick Facts

| Question | Answer |
|---|---|
| Line number of `setup_per_cpu_areas()` call? | `init/main.c:901` |
| What comes immediately before it? | `setup_nr_cpu_ids()` at line 900 |
| What comes immediately after it? | `smp_prepare_boot_cpu()` at line 902 |
| Why must `setup_arch()` come first? | memblock must be ready for bootmem allocation |
| Why must `setup_nr_cpu_ids()` come first? | nr_cpu_ids needed for alloc_info sizing |
| Can `this_cpu_*` be called before `smp_prepare_boot_cpu`? | No — hardware register not written yet |
| What does `smp_prepare_boot_cpu` do for ARM? | Calls `set_my_cpu_offset()` → writes hardware register |
