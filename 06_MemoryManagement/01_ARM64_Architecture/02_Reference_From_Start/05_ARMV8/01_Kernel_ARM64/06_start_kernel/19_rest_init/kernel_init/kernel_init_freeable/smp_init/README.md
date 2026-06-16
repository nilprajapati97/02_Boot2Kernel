# `smp_init()` — Bring Up Secondary CPUs

## Purpose

Brings all secondary CPUs (CPU 1, 2, ..., N-1) from their firmware-halted state into full Linux operation. After `smp_init()`, all CPUs are online and running their own idle loops.

## Source File

`kernel/smp.c`

```c
void __init smp_init(void)
{
    int num_nodes, num_cpus;
    
    idle_threads_init();   // Create idle tasks for all CPUs
    cpuhp_threads_init();  // CPU hotplug kthreads
    
    pr_info("Bringing up secondary CPUs ...\n");
    
    bringup_nonboot_cpus(setup_max_cpus);
    
    num_nodes = num_online_nodes();
    num_cpus = num_online_cpus();
    pr_info("Brought up %d node%s, %d CPU%s\n", ...);
    
    /* Final SMP callbacks: */
    smp_cpus_done(setup_max_cpus);
}
```

## CPU Bring-Up Sequence

For each secondary CPU:

```
Boot CPU (CPU 0)                    Secondary CPU (CPU N)
───────────────────                 ──────────────────────
1. Allocate idle task for CPU N
2. Set CPU N's initial_code pointer
3. Send INIT IPI to CPU N          ← CPU N: Execute BIOS reset vector
4. Wait 10ms
5. Send STARTUP IPI × 2            ← CPU N: Jump to trampoline code
                                    ← CPU N: Enable protected mode
                                    ← CPU N: Enable paging
                                    ← CPU N: Set up GDT/IDT
                                    ← CPU N: Call start_secondary()
6. Wait for cpu_online(N)          ← CPU N: Calls cpu_startup_entry()
7. Continue to next CPU            ← CPU N: Enters idle loop
```

## x86 AP (Application Processor) Startup

The x86 SMP startup uses the APIC IPI (Inter-Processor Interrupt) protocol:

```
1. INIT IPI → AP resets to real mode at 0xFFFF0
2. Wait 10ms (spec requirement)
3. STARTUP IPI (address = trampoline page >> 12)
   → AP jumps to arch/x86/realmode/rm/trampoline_64.S
4. AP transitions: real mode → protected mode → long mode
5. AP calls start_secondary() in arch/x86/kernel/smpboot.c
```

## `start_secondary()` on Each AP

```c
static void notrace start_secondary(void *unused)
{
    cpu_init();
    x86_cpuinit.early_percpu_clock_init();
    preempt_disable();
    smp_callin();
    
    // ...
    
    // All initialization complete:
    set_cpu_online(smp_processor_id(), true);
    
    // Enter the idle loop (same as CPU 0):
    cpu_startup_entry(CPUHP_AP_ONLINE_IDLE);
    // NEVER RETURNS
}
```

## Idle Tasks for APs

Before APs are started, `idle_threads_init()` creates idle tasks:

```c
void __init idle_threads_init(void)
{
    unsigned cpu, boot_cpu;
    boot_cpu = smp_processor_id();
    
    for_each_possible_cpu(cpu) {
        if (cpu != boot_cpu)
            idle_init(cpu);  // fork_idle(cpu) creates a new idle task
    }
}
```

## `setup_max_cpus`

The number of CPUs to bring up is controlled by:

```bash
# Limit to 4 CPUs at boot:
maxcpus=4

# Disable SMP entirely:
nosmp
maxcpus=0
```

## NUMA Topology

During AP bring-up, NUMA affinity is established. Each CPU is assigned its NUMA node, and the scheduler's domain hierarchy (discussed in `sched_init_smp()`) is configured for optimal NUMA-aware scheduling.

## Cross-references

- [Parent: kernel_init_freeable](../README.md)
- `sched_init()`: [../../../../06_scheduling/sched_init/README.md](../../../../06_scheduling/sched_init/README.md)
- `cpu_startup_entry()`: [../../../rest_init/cpu_startup_entry/README.md](../../../rest_init/cpu_startup_entry/README.md)
