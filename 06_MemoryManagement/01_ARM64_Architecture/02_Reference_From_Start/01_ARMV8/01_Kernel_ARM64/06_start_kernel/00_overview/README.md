# `start_kernel()` — Overview

## Purpose

`start_kernel()` is the kernel's C-language entry point — the bridge between architecture-specific assembly bootstrap code and the fully generic kernel initialization sequence. It is called exactly once, by the boot CPU, with interrupts disabled. It never returns.

## Function Signature

```c
asmlinkage __visible __init __no_sanitize_address __noreturn __no_stack_protector
void start_kernel(void)
```

Located in: `init/main.c`, line 874

## What Comes Before `start_kernel()`?

On x86-64, the boot sequence leading to `start_kernel()` is:

```
BIOS/UEFI firmware
    └─► bootloader (GRUB/systemd-boot)
            └─► arch/x86/boot/header.S   (16-bit entry)
                    └─► arch/x86/boot/main.c (16-bit C)
                            └─► arch/x86/boot/compressed/head_64.S
                                    └─► arch/x86/kernel/head_64.S
                                            └─► arch/x86/kernel/head64.c
                                                    └─► start_kernel()
```

By the time `start_kernel()` is reached, the architecture code has:
- Switched to protected/long mode (x86)
- Set up early page tables (identity + kernel virtual mapping)
- Enabled the MMU
- Set up a temporary stack for the boot CPU
- Initialized early CPU exception handlers (enough to not crash)
- Zeroed the `.bss` section
- Copied boot parameters from bootloader into `boot_params`

## What `start_kernel()` Does — 17-Phase Overview

| Phase | IRQ State | Memory | Highlights |
|-------|-----------|--------|------------|
| 1. Early CPU & Task | Disabled | memblock | Stack guard, CPU ID, cgroup early |
| 2. Architecture | Disabled | memblock | Memory map, CPU features, per-CPU |
| 3. Parameter Parsing | Disabled | memblock | cmdline, jump labels, early RNG |
| 4. Core Memory | Disabled | memblock→buddy | trap_init, mm_core_init |
| 5. Tracing | Disabled | buddy+slab | ftrace, early trace |
| 6. Scheduler | Disabled | buddy+slab | CFS/RT/DL runqueues |
| 7. Data Structures | Disabled | buddy+slab | Radix/maple trees, workqueues |
| 8. RCU | Disabled | buddy+slab | Tree RCU grace period daemon |
| 9. IRQ & Timers | Disabled | buddy+slab | APIC, tick, hrtimer, timekeeping |
| 10. Security/RNG | Disabled | buddy+slab | Full CRNG, KFENCE, stack canary |
| 11. Perf | Disabled | buddy+slab | PMU hardware counters |
| **IRQ ENABLE** | **→ Enabled** | buddy+slab | `local_irq_enable()` |
| 12. Console/Locks | Enabled | slab late | console_init, lockdep |
| 13. NUMA/ACPI/Clocks | Enabled | full | BogoMIPS, CPU finalize |
| 14. Process/Security | Enabled | full | fork, PIDs, creds, LSM |
| 15. VFS/Filesystems | Enabled | full | dcache, proc, signals |
| 16. Cgroups | Enabled | full | cgroup v1/v2, cpuset |
| 17. Rest init | Enabled | full | arch_call_rest_init() → idle |

## Local Variables

```c
char *command_line;    // Points to arch-provided cmdline after setup_arch()
char *after_dashes;    // Points to "--" separator in cmdline, if any
```

These are the only two local variables. Everything else is in global/static storage, reflecting the boot CPU single-threaded nature of this phase.

## The `--` Separator in Command Line

```
kernel_param1 kernel_param2 -- init_arg1 init_arg2
```

- Parameters before `--` are parsed by the kernel
- Parameters after `--` are passed directly to the `init` process (PID 1)
- `after_dashes` captures the pointer to arguments after `--`

## See Also

- [call_flow.md](call_flow.md) — Numbered call sequence with line numbers
- [boot_phases.md](boot_phases.md) — Phase narrative with subsystem interactions
- [../README.md](../README.md) — Root documentation with full ASCII call tree
