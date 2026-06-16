# `start_kernel()` — Linux Kernel Boot Entry Point

**Source file:** [`init/main.c`](../../init/main.c) · Line 874  
**Audience:** Kernel engineers learning the Linux boot path from scratch

---

## What Is `start_kernel()`?

`start_kernel()` is the first architecture-independent C function the Linux kernel runs. Every supported architecture (x86, ARM64, RISC-V, etc.) sets up the minimal CPU state in assembly, then jumps here. This function orchestrates the initialization of **every major kernel subsystem** — memory, scheduling, interrupts, timekeeping, filesystems, security, and more — before handing off to the first user-space process.

```c
asmlinkage __visible __init __no_sanitize_address __noreturn __no_stack_protector
void start_kernel(void)
```

| Attribute | Meaning |
|-----------|---------|
| `asmlinkage` | Use C calling convention; called from assembly |
| `__visible` | Prevent linker garbage collection of this symbol |
| `__init` | Place in `.init.text` section (freed after boot) |
| `__no_sanitize_address` | ASan not yet ready; skip instrumentation |
| `__noreturn` | Never returns — ends in `arch_call_rest_init()` → idle loop |
| `__no_stack_protector` | SSP canary not yet initialized at entry |

---

## System State at Entry

When `start_kernel()` is entered:

- **IRQs**: Disabled (must remain disabled until `local_irq_enable()`)
- **MMU**: Enabled (identity/early page tables set by arch code)
- **Memory allocator**: Only `memblock` is available
- **Printk**: Output via `earlycon` only (no console driver yet)
- **SMP**: Single CPU (boot CPU only)
- **Scheduler**: Not yet running (no preemption)
- **system_state**: `SYSTEM_BOOTING`

---

## Full Call Sequence (in source order)

```
start_kernel()
│
├── [Phase 1: Early CPU & Task Setup]
│   ├── set_task_stack_end_magic(&init_task)
│   ├── smp_setup_processor_id()
│   ├── debug_objects_early_init()
│   ├── init_vmlinux_build_id()
│   ├── cgroup_init_early()
│   ├── local_irq_disable()              ◄─ IRQs DISABLED
│   ├── boot_cpu_init()
│   └── page_address_init()
│
├── [Phase 2: Architecture Setup]
│   ├── early_security_init()
│   ├── setup_arch(&command_line)        ◄─ LARGEST single call
│   ├── setup_boot_config()
│   ├── setup_command_line(command_line)
│   ├── setup_nr_cpu_ids()
│   ├── setup_per_cpu_areas()
│   ├── smp_prepare_boot_cpu()
│   └── boot_cpu_hotplug_init()
│
├── [Phase 3: Parameter Parsing]
│   ├── jump_label_init()
│   ├── parse_early_param()
│   ├── parse_args("Booting kernel", ...)
│   ├── print_unknown_bootoptions()
│   ├── parse_args("Setting init args", ...)  [conditional]
│   ├── parse_args("Setting extra init args", ...) [conditional]
│   └── random_init_early(command_line)
│
├── [Phase 4: Core Memory & Exception Handling]
│   ├── setup_log_buf(0)
│   ├── vfs_caches_init_early()
│   ├── sort_main_extable()
│   ├── trap_init()
│   ├── mm_core_init()                   ◄─ memblock → page allocator
│   ├── poking_init()
│   └── ftrace_init()
│
├── [Phase 5: Tracing]
│   └── early_trace_init()
│
├── [Phase 6: Scheduler]
│   └── sched_init()
│
├── [Phase 7: Data Structures & Workqueues]
│   ├── radix_tree_init()
│   ├── maple_tree_init()
│   ├── housekeeping_init()
│   └── workqueue_init_early()
│
├── [Phase 8: RCU & Tracing]
│   ├── rcu_init()
│   ├── trace_init()
│   └── context_tracking_init()
│
├── [Phase 9: Interrupts & Timers]
│   ├── early_irq_init()
│   ├── init_IRQ()
│   ├── tick_init()
│   ├── rcu_init_nohz()
│   ├── init_timers()
│   ├── srcu_init()
│   ├── hrtimers_init()
│   ├── softirq_init()
│   ├── timekeeping_init()
│   └── time_init()
│
├── [Phase 10: Randomness & Memory Safety]
│   ├── random_init()
│   ├── kfence_init()
│   └── boot_init_stack_canary()
│
├── [Phase 11: Performance & Profiling]
│   ├── perf_event_init()
│   ├── profile_init()
│   └── call_function_init()
│
├── local_irq_enable()                   ◄─ IRQs ENABLED
│
├── [Phase 12: Post-IRQ Memory & Console]
│   ├── kmem_cache_init_late()
│   ├── console_init()                   ◄─ CONSOLE ONLINE
│   ├── lockdep_init()
│   └── locking_selftest()
│
├── [Phase 13: NUMA, ACPI, Clocks]
│   ├── setup_per_cpu_pageset()
│   ├── numa_policy_init()
│   ├── acpi_early_init()
│   ├── late_time_init()  [arch opt.]
│   ├── sched_clock_init()
│   ├── calibrate_delay()
│   └── arch_cpu_finalize_init()
│
├── [Phase 14: Process & Security Infrastructure]
│   ├── pid_idr_init()
│   ├── anon_vma_init()
│   ├── efi_enter_virtual_mode() [x86 only]
│   ├── thread_stack_cache_init()
│   ├── cred_init()
│   ├── fork_init()
│   ├── proc_caches_init()
│   ├── uts_ns_init()
│   ├── key_init()
│   ├── security_init()
│   ├── dbg_late_init()
│   └── net_ns_init()
│
├── [Phase 15: Filesystems & Namespaces]
│   ├── vfs_caches_init()
│   ├── pagecache_init()
│   ├── signals_init()
│   ├── seq_file_init()
│   ├── proc_root_init()
│   └── nsfs_init()
│
├── [Phase 16: Control Groups & Accounting]
│   ├── cpuset_init()
│   ├── cgroup_init()
│   ├── taskstats_init_early()
│   └── delayacct_init()
│
├── [Phase 17: Platform Finalization]
│   ├── acpi_subsystem_init()
│   ├── arch_post_acpi_subsys_init()
│   └── kcsan_init()
│
└── arch_call_rest_init()                ◄─ NEVER RETURNS → idle loop
```

---

## Key State Transitions

### IRQ State Timeline

```
Entry                    ~line 891        ~line 1030
  │                          │                │
  ▼                          ▼                ▼
[IRQs ON]──────► local_irq_disable() ──────► local_irq_enable()
                [IRQs DISABLED: ~139 calls]  [IRQs ENABLED: rest of boot]
```

### Memory Allocator Timeline

```
Entry              mm_core_init()      kmem_cache_init_late()
  │                    │                      │
  ▼                    ▼                      ▼
[memblock only] ──► [buddy allocator] ──► [slab/slub fully ready]
                  [kmalloc available]   [kmem_cache_create works]
```

### System State Transitions

```
start_kernel() entry      rest_init()             kernel_init()
       │                      │                        │
       ▼                      ▼                        ▼
SYSTEM_BOOTING ──────► SYSTEM_SCHEDULING ──► SYSTEM_FREEING_INITMEM
                                                        │
                                                        ▼
                                                SYSTEM_RUNNING
```

---

## Directory Index

| Directory | Phase | Key Subsystem |
|-----------|-------|---------------|
| [00_overview/](00_overview/README.md) | — | Context, full call flow, state diagrams |
| [01_early_cpu_task_setup/](01_early_cpu_task_setup/README.md) | 1 | Stack magic, CPU ID, cgroup early |
| [02_arch_setup/](02_arch_setup/README.md) | 2 | setup_arch, per-CPU, command line |
| [03_parameter_parsing/](03_parameter_parsing/README.md) | 3 | Jump labels, cmdline parsing |
| [04_memory_management/](04_memory_management/README.md) | 4 | trap_init, mm_core_init, VFS early |
| [05_tracing_debugging/](05_tracing_debugging/README.md) | 5 | ftrace, trace_init, context tracking |
| [06_scheduling/](06_scheduling/README.md) | 6 | sched_init, CFS, housekeeping |
| [07_data_structures/](07_data_structures/README.md) | 7 | Radix tree, maple tree |
| [08_rcu/](08_rcu/README.md) | 8 | RCU init, nohz |
| [09_interrupts_irq/](09_interrupts_irq/README.md) | 9 | IRQ descriptors, APIC, tick, softirq |
| [10_timekeeping_timers/](10_timekeeping_timers/README.md) | 9 | Timer wheel, hrtimer, timekeeping |
| [11_security_randomness/](11_security_randomness/README.md) | 10 | CRNG, KFENCE, LSM, keys |
| [12_perf_profiling/](12_perf_profiling/README.md) | 11 | PMU, perf_event, profiling |
| [13_console_locking/](13_console_locking/README.md) | 12 | console_init, lockdep |
| [14_acpi/](14_acpi/README.md) | 13 | ACPI, BogoMIPS, CPU finalize |
| [15_process_management/](15_process_management/README.md) | 14 | fork_init, PIDs, credentials |
| [16_networking/](16_networking/README.md) | 14 | net_ns_init |
| [17_vfs_filesystems/](17_vfs_filesystems/README.md) | 15 | VFS caches, proc, signals |
| [18_cgroups_control/](18_cgroups_control/README.md) | 16 | cgroup v1/v2, cpuset |
| [19_rest_init/](19_rest_init/README.md) | 17 | rest_init, kernel_init, kthreadd |

---

## How to Read This Documentation

1. Start with [00_overview/boot_phases.md](00_overview/boot_phases.md) for the narrative story
2. Follow [00_overview/call_flow.md](00_overview/call_flow.md) for the precise call sequence
3. Drill into each numbered phase directory in order
4. Within a phase, each function subdirectory has its own `README.md` with source references

Each function-level document follows this template:
- **Purpose** — What problem does this solve?
- **Source File** — Exact path in the kernel tree
- **Pre-conditions** — What must be true before this call
- **Post-conditions** — What becomes available after
- **Key Data Structures** — What is initialized
- **Call Graph** — Direct callees
- **IRQ State** — Enabled or disabled at call time
- **Kconfig Dependencies** — Build-time gates
