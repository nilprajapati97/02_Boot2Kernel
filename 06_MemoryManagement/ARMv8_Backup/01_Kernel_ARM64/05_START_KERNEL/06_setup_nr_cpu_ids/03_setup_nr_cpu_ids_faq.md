# 03. setup_nr_cpu_ids FAQ

## Q1: What does `setup_nr_cpu_ids()` do?

**A:** It sets `nr_cpu_ids` to the smallest runtime CPU ID limit based on the highest set bit in `cpu_possible_mask`.

In code, it is:

```c
set_nr_cpu_ids(find_last_bit(cpumask_bits(cpu_possible_mask), NR_CPUS) + 1);
```

This means if the highest possible CPU ID is `3`, `nr_cpu_ids` becomes `4`.

## Q2: Why is `nr_cpu_ids` different from `NR_CPUS`?

**A:** `NR_CPUS` is a compile-time upper bound. `nr_cpu_ids` is the runtime limit for the current boot.

`NR_CPUS` defines the size of static data structures, while `nr_cpu_ids` defines how many CPU IDs are actually valid.

## Q3: What is `cpu_possible_mask`?

**A:** It is the boot-time bitmap of CPU IDs that may ever be used on this system. It is architecture/platform-specific and set before `setup_nr_cpu_ids()`.

If bit `0` and bit `3` are set, it means logical CPUs `0` and `3` are possible; the resulting `nr_cpu_ids` becomes at least `4`.

## Q4: When is `setup_nr_cpu_ids()` called?

**A:** It runs early in `start_kernel()`, before `setup_per_cpu_areas()` and before `smp_prepare_boot_cpu()`.

This ensures later per-CPU allocations have the correct CPU limit.

## Q5: Can `setup_nr_cpu_ids()` fail?

**A:** Not directly. It assumes `cpu_possible_mask` is valid. If that mask is malformed, later CPU setup code will fail or behave incorrectly.

The function itself is a simple calculation and update.

## Q6: How does `maxcpus=` affect this function?

**A:** It usually does not directly affect `setup_nr_cpu_ids()`. `maxcpus=` sets `setup_max_cpus`, which limits how many CPUs will actually be activated during SMP boot.

`setup_nr_cpu_ids()` determines the valid ID range, while `setup_max_cpus` determines how many of those IDs are brought online.

## Q7: How does `nr_cpus=` affect this function?

**A:** `nr_cpus=` can reduce the value of `nr_cpu_ids` if the boot-time limit is smaller than the architecture-detected possible CPUs.

That parameter is handled by early boot parsing and `set_nr_cpu_ids()` before or around `setup_nr_cpu_ids()`.

## Q8: Why is there a `+ 1` in the code?

**A:** `find_last_bit()` returns the highest set bit index. CPU IDs are zero-based, so the count/limit is the index plus one.

Example:

- highest set bit = 0 -> `nr_cpu_ids = 1`
- highest set bit = 1 -> `nr_cpu_ids = 2`
- highest set bit = 3 -> `nr_cpu_ids = 4`

## Q9: What does `set_nr_cpu_ids()` do?

**A:** It updates `nr_cpu_ids`, but only when the kernel build supports dynamic runtime CPU limits.

On systems with `NR_CPUS == 1` or `CONFIG_FORCE_NR_CPUS`, `set_nr_cpu_ids()` is a no-op or warns if the requested value differs.

## Q10: What is the difference between `possible`, `present`, and `online`?

- `possible`: CPUs that may ever exist on this boot (`cpu_possible_mask`)
- `present`: CPUs that are currently populated or plugged in (`cpu_present_mask`)
- `online`: CPUs that are currently enabled for scheduling (`cpu_online_mask`)

`setup_nr_cpu_ids()` depends on `possible`; it does not care about `present` or `online`.

## Q11: Is `setup_nr_cpu_ids()` architecture-specific?

**A:** No. The function is generic and lives in `linux/kernel/smp.c`. The architecture-specific part is building `cpu_possible_mask`.

ARM32 and ARM64 both use the same function once the possible CPU bitmap is ready.

## Q12: How does ARM32 differ from ARM64 in this flow?

**A:** ARM32 often makes a writable copy of the boot command line and may use legacy ATAG/DT paths for CPU enumeration.

ARM64 typically uses `boot_command_line` directly and enumerates CPUs through Device Tree or ACPI MADT.

## Q13: What happens on uniprocessor builds?

**A:** On UP builds (`CONFIG_SMP` disabled), `setup_nr_cpu_ids()` is often defined as a no-op because `nr_cpu_ids` is fixed at compile time.

The kernel does not need a dynamic runtime limit when only one CPU exists.

## Q14: Why does the kernel need `nr_cpu_ids` before `smp_init()`?

**A:** Many core boot structures are sized with `nr_cpu_ids` before SMP bring-up. That includes per-CPU memory, CPU masks, scheduler structures, and core topology data.

If the runtime CPU ID limit were unknown, the kernel could not correctly allocate or validate these structures.

## Q15: Can `nr_cpu_ids` be smaller than the number of CPUs physically present?

**A:** Yes. Boot options like `nr_cpus=` can deliberately reduce the usable CPU ID range. This is useful for testing or for workarounds on buggy hardware.

However, if the runtime limit is too small for the actual hardware, some CPUs may never be brought online.

## Q16: What is the relationship with `smp_prepare_cpus()`?

**A:** `setup_nr_cpu_ids()` is about the ID range. `smp_prepare_cpus()` is about preparing the boot CPU and eventually enabling additional CPUs.

The kernel calls:

1. `setup_nr_cpu_ids()`
2. `setup_per_cpu_areas()`
3. `smp_prepare_boot_cpu()`
4. `boot_cpu_hotplug_init()`

Then later, in `kernel_init_freeable()`, it calls `smp_init()` to bring up secondary CPUs.

## Q17: How does `cpu_possible_mask` get populated?

**A:** Platform/arch code sets it based on firmware, device tree, ACPI tables, or fixed platform knowledge.

ARM64 may populate it from DT or ACPI. ARM32 may populate it from ATAGs, DT, or legacy platform setup.

## Q18: Why does Linux use a logical CPU ID range instead of hardware IDs?

**A:** Logical CPU IDs simplify Linux scheduling and per-CPU data access. They provide a compact contiguous range for kernel data structures while allowing hardware numbering to be arbitrary.

This abstraction is critical for supporting heterogeneous systems, hotplug, virtualization, and platform-specific ID schemes.

## Q19: What if the highest possible CPU ID is not densely packed?

**A:** `nr_cpu_ids` still becomes the maximum ID plus one. Linux can handle sparse IDs in `cpu_possible_mask` because masks are bitmaps, but `nr_cpu_ids` must cover the highest possible index.

Example: if CPUs `0` and `5` are possible, `nr_cpu_ids = 6`.

## Q20: Where should I look for the exact implementation?

- `linux/kernel/smp.c` for `setup_nr_cpu_ids()` and SMP boot logic
- `linux/include/linux/cpumask.h` for `set_nr_cpu_ids()` and CPU mask semantics
- `arch/arm/kernel/setup.c` and `arch/arm64/kernel/setup.c` for architecture setup details
- `linux/init/main.c` for the `start_kernel()` sequence
