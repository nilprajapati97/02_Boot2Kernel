# `boot_cpu_init()` — Marking the Boot CPU Online

## Overview

| Attribute    | Value                                       |
|-------------|----------------------------------------------|
| **Function** | `boot_cpu_init(void)`                        |
| **Source**   | `kernel/cpu.c`                               |
| **Purpose**  | Mark the boot CPU as present, online, active, and possible in all CPU state bitmasks |

---

## Why It Exists

The Linux SMP infrastructure maintains four CPU state bitmasks:

```c
cpumask_t cpu_possible_mask;    // CPUs that could ever exist
cpumask_t cpu_present_mask;     // CPUs physically present
cpumask_t cpu_online_mask;      // CPUs currently running (receiving IRQs)
cpumask_t cpu_active_mask;      // CPUs that can run tasks
```

Before `boot_cpu_init()`, none of these bitmasks have any bits set — not even for the boot CPU. Many kernel subsystems (scheduler runqueue init, per-CPU allocations, IRQ affinity) check `cpu_online_mask` to know which CPUs exist. They would malfunction or skip the boot CPU if it's not marked online.

---

## Implementation

```c
// kernel/cpu.c
static void __init boot_cpu_init(void)
{
    int cpu = smp_processor_id();   // = 0 for boot CPU

    set_cpu_online(cpu, true);      // cpu_online_mask: bit 0 = 1
    set_cpu_active(cpu, true);      // cpu_active_mask: bit 0 = 1
    set_cpu_present(cpu, true);     // cpu_present_mask: bit 0 = 1
    set_cpu_possible(cpu, true);    // cpu_possible_mask: bit 0 = 1
}
```

After this, the boot CPU (CPU 0) is visible to all subsystems as a valid, running CPU.

---

## CPU State Transitions During Full Boot

```
boot_cpu_init()
    CPU 0: possible=1, present=1, online=1, active=1

setup_arch() → ACPI/DT parsing → smp_boot_cpus()
    CPU 1..N: possible=1, present=1  (not yet online)

smp_init() → start_secondary() → cpu_up()
    CPU 1..N: online=1, active=1   (secondary CPUs come online)
```

---

## Sub-Topics

- [01_cpumask_operations](01_cpumask_operations/README.md) — `cpumask_t`, `for_each_online_cpu()`, atomic set/test operations
- [02_cpu_online_possible_active](02_cpu_online_possible_active/README.md) — State machine for CPU hotplug transitions

---

## Interview Q&A

### Q1: What is the difference between `cpu_present_mask` and `cpu_online_mask`?
**A:** `cpu_present_mask` marks CPUs that are physically present (hardware exists, detectable via ACPI/DT). `cpu_online_mask` marks CPUs that are currently running and can receive interrupts and execute tasks. In a hotplug-capable system, a CPU can be present but not online (physically installed but software-disabled). `smp_processor_id()` is only valid for CPUs in `cpu_online_mask`. Per-CPU data is allocated for all `cpu_possible` CPUs, but runqueues only exist for `cpu_online` CPUs.

### Q2: Why is `cpu_possible_mask` separate from `cpu_present_mask`?
**A:** `cpu_possible_mask` is set at boot time to the maximum number of CPUs the system could ever have (from ACPI tables or `CONFIG_NR_CPUS`). Per-CPU memory allocations use `cpu_possible_mask` to pre-allocate space for all potential CPUs. `cpu_present_mask` contains CPUs actually discovered. This separation allows memory to be pre-allocated for CPUs that may be hot-plugged later, without needing to dynamically resize per-CPU arrays.

### Q3: What happens when a CPU is hot-unplugged?
**A:** The CPU transitions: `active=0` → `online=0` → `present=0` (if physically removed). The scheduler migrates all tasks off the CPU first, then the CPU executes `cpu_die()` which calls the CPU hotplug notifier chain, drains softirq queues, and finally executes `play_dead()` — entering a halt or MWAIT loop. The CPU's runqueue is emptied and its tasks are migrated to other online CPUs via `migrate_tasks()`.
