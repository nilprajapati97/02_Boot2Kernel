# Template Copy: Per-CPU Data Initialization via `memcpy`

## Source Reference
- `mm/percpu.c:~3140` — template copy loop in `pcpu_embed_first_chunk()`
- `include/asm-generic/vmlinux.lds.h` — `__per_cpu_load` / `__per_cpu_start` definitions

---

## The Core Concept: Master Template

The Linux kernel's linker collects all `DEFINE_PER_CPU()` variables into a single
ELF section `.data..percpu`. This section, bounded by `__per_cpu_start` and
`__per_cpu_end`, is the **master template**.

```
Kernel image in memory:
  _text            0xC0008000 ← kernel code starts
  .text            ...
  .rodata          ...
  .data            ...
  __per_cpu_load = 0xC0800000 ← where template is loaded into RAM
  __per_cpu_start  0xC0800000 ← virtual start of template (= __per_cpu_load on ARM64/non-XIP)
  .data..percpu    {
      .first       ← highest-priority per-CPU vars (cpu_number, etc.)
      .page_aligned← page-aligned per-CPU vars
      .read_mostly ← read-mostly per-CPU vars (separate cache line)
      (default)    ← all other DEFINE_PER_CPU() vars
  }
  __per_cpu_end    0xC0880000 ← virtual end of template
```

`static_size = __per_cpu_end - __per_cpu_start = 512KB` (in this example)

---

## Template Copy Code

```c
/* mm/percpu.c:~3140 */
for (group = 0; group < ai->nr_groups; group++) {
    struct pcpu_group_info *gi = &ai->groups[group];

    for (i = 0; i < gi->nr_units; i++) {
        /* Virtual address of this CPU's unit start */
        void *unit_addr = gi->base_addr + (unsigned long)(i * ai->unit_size);

        if (gi->cpu_map[i] == NR_CPUS) {
            /* Padding unit — no real CPU assigned */
            /* Zero the entire unit to avoid stale data */
            memset(unit_addr, 0, ai->unit_size);
            continue;
        }

        /* CRITICAL: Copy from __per_cpu_load, not __per_cpu_start */
        memcpy(unit_addr,               /* destination: CPU's private unit */
               __per_cpu_load,          /* source: template physical load address */
               ai->static_size);        /* length: size of .data..percpu section */

        /* Zero the rest of the unit (reserved + dynamic regions) */
        memset(unit_addr + ai->static_size, 0,
               ai->unit_size - ai->static_size);
    }
}
```

---

## Why `__per_cpu_load` vs `__per_cpu_start`?

### On ARM64 and non-XIP ARM32: They're the same

```
__per_cpu_load  == __per_cpu_start
```

Both refer to the same virtual address in the kernel image loaded into RAM.
The `memcpy` source and virtual address are identical.

### On XIP (Execute-In-Place) ARM32: They differ

XIP kernels execute their code **directly from flash memory** (read-only storage).
The `.data..percpu` section cannot run from flash because it's mutable data.
Instead:
```
Flash (read-only, not RAM):
  __per_cpu_start  0xC0800000 (virtual) ← linked as if it runs here
  .data..percpu    ← actually stored in flash at this virtual address

RAM:
  __per_cpu_load   0x40800000 (physical), or corresponding virtual address
  .data..percpu    ← bootloader copies from flash to this RAM address at startup

memcpy(unit_addr, __per_cpu_load, static_size)
  reads from the RAM copy at __per_cpu_load
  (NOT from flash at __per_cpu_start, which might not be physically accessible
   the same way, and is the "virtual" run address, not the load address)
```

For ARM64: XIP is not supported; `__per_cpu_load = __per_cpu_start` always.

---

## What Gets Copied? — Per-CPU Variable Categories

### `.data..percpu..first` — cpu_number and critical early vars

```c
/* include/linux/percpu-defs.h */
#define DEFINE_PER_CPU_FIRST(type, name)  \
    DEFINE_PER_CPU_SECTION(type, name, PER_CPU_FIRST_SECTION)

/* Example: */
DEFINE_PER_CPU_FIRST(int, cpu_number);  /* CPU ID, initialized before percpu setup */
```

These are placed first in the template to ensure they're at a known offset from
the unit base address.

### `.data..percpu..page_aligned` — per-CPU page structures

```c
/* Example in mm/: */
DEFINE_PER_CPU_PAGE_ALIGNED(struct per_cpu_pageset, boot_pageset);
/* Placed at page boundary for huge-page mapping optimization */
```

### `.data..percpu..read_mostly` — mostly-read data

```c
/* Example: */
DEFINE_PER_CPU_READ_MOSTLY(struct task_struct *, current_task);
/* Placed in separate cache lines to avoid write invalidation from writes
   to adjacent per-CPU variables */
```

### `.data..percpu` (default) — all other DEFINE_PER_CPU vars

This includes thousands of per-CPU variables from:
- Scheduler (`struct rq runqueues`, `nr_switches`)
- Memory allocator (`struct kmem_cache_cpu`)
- IRQ handling (`irq_stat`, `softirq_work_list`)
- Networking (`struct softnet_data`)
- Block I/O (`struct blk_plug`)
- RCU (`struct rcu_data`)
- Timer (`struct timer_base`)

---

## Memory Layout After Template Copy

For CPU 0 (unit at `pcpu_base_addr + 0`):

```
pcpu_base_addr + 0x00000: [STATIC REGION — memcpy from __per_cpu_load]
  + 0x00000: cpu_number = 0  (DEFINE_PER_CPU_FIRST: initialized during copy)
  + 0x01000: boot_pageset (page-aligned)
  + 0x05000: current_task (read-mostly)
  + 0x10000: struct rq (scheduler runqueue)
  + 0x20000: softnet_data
  + ...
  + 0x7FFFF: (end of static region at static_size = 512KB)

pcpu_base_addr + 0x80000: [RESERVED REGION — zeroed]
  + 0x80000: (8KB for kernel module per-CPU vars)
  + 0x82000: (end of reserved region)

pcpu_base_addr + 0x82000: [DYNAMIC REGION — zeroed]
  + 0x82000: (20KB+ for alloc_percpu() allocations)
  + 0x87000: (end of unit at unit_size)
```

---

## What Happens to `cpu_number` After Copy?

The per-CPU variable `cpu_number` is defined as `DEFINE_PER_CPU_FIRST(int, cpu_number)`.

After the template copy, EVERY CPU unit has `cpu_number = 0` (the template's value).
This is wrong — CPU 1 should have `cpu_number = 1`, etc.

The correction happens in `pcpu_setup_first_chunk()`:
```c
/* mm/percpu.c:~2700 */
for_each_possible_cpu(cpu) {
    per_cpu(cpu_number, cpu) = cpu;  /* fix cpu_number for each unit */
}
```

This illustrates that the template copy sets **initial values** which may be
subsequently overridden by setup code.

---

## Interview Quick Facts

| Question | Answer |
|---|---|
| What is the template? | The `.data..percpu` section in the kernel image |
| What linker symbols bound it? | `__per_cpu_start` and `__per_cpu_end` |
| What is `__per_cpu_load`? | Physical load address of template (may differ from `__per_cpu_start` on XIP) |
| Does ARM64 use XIP? | No — `__per_cpu_load == __per_cpu_start` on ARM64 |
| What is copied? | `ai->static_size` bytes from `__per_cpu_load` |
| What is zeroed after copy? | Reserved + dynamic regions within each unit |
| What is `.data..percpu..first`? | Highest-priority per-CPU section (cpu_number, etc.) |
| Why might copied values need fixing? | Template has CPU 0's values; other CPUs need their own (e.g., cpu_number) |
