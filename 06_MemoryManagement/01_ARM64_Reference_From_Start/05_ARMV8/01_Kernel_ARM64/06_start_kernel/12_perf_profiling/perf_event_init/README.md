# `perf_event_init()` — Performance Event Framework

## Purpose

Initializes the `perf_events` subsystem: sets up per-CPU performance monitoring contexts, registers the PMU (Performance Monitoring Unit) drivers, and initializes the software event counters.

## Source File

`kernel/events/core.c`

```c
void __init perf_event_init(void)
{
    int ret;
    
    idr_init(&pmu_idr);
    
    // Initialize per-CPU contexts
    perf_event_init_all_cpus();
    
    // Register built-in PMUs:
    perf_pmu_register(&perf_swevent, "software", PERF_TYPE_SOFTWARE);
    perf_pmu_register(&perf_cpu_clock, NULL, -1);
    perf_pmu_register(&perf_task_clock, NULL, -1);
    perf_pmu_register(&perf_tracepoint, "tracepoint", PERF_TYPE_TRACEPOINT);
#ifdef CONFIG_KPROBES
    perf_pmu_register(&perf_kprobe, "kprobe", -1);
#endif
    
    // Register breakpoint PMU
    perf_pmu_register(&perf_breakpoint, "breakpoint", PERF_TYPE_BREAKPOINT);
    
    // Set up the NMI watchdog (uses PMU)
    perf_event_init_cpu(smp_processor_id());
}
```

## PMU Types

### Hardware PMU (CPU-specific)

```c
// Example: Intel Skylake PMU
// Registered by arch/x86/events/intel/core.c

// Hardware events available:
PERF_COUNT_HW_CPU_CYCLES         // CPU cycles
PERF_COUNT_HW_INSTRUCTIONS       // Instructions retired
PERF_COUNT_HW_CACHE_REFERENCES   // Cache references
PERF_COUNT_HW_CACHE_MISSES       // Cache misses
PERF_COUNT_HW_BRANCH_INSTRUCTIONS// Branches taken
PERF_COUNT_HW_BRANCH_MISSES      // Branch mispredictions
PERF_COUNT_HW_BUS_CYCLES         // Bus cycles
PERF_COUNT_HW_STALLED_CYCLES_FRONTEND // Frontend stalls
PERF_COUNT_HW_STALLED_CYCLES_BACKEND  // Backend stalls
```

### Software PMU

```c
// Software events (kernel-generated):
PERF_COUNT_SW_CPU_CLOCK         // CPU time
PERF_COUNT_SW_TASK_CLOCK        // Task time
PERF_COUNT_SW_PAGE_FAULTS       // Page faults
PERF_COUNT_SW_CONTEXT_SWITCHES  // Context switches
PERF_COUNT_SW_CPU_MIGRATIONS    // Task migrations between CPUs
PERF_COUNT_SW_PAGE_FAULTS_MIN   // Minor page faults
PERF_COUNT_SW_PAGE_FAULTS_MAJ   // Major page faults (I/O)
PERF_COUNT_SW_ALIGNMENT_FAULTS  // Unaligned memory access
PERF_COUNT_SW_EMULATION_FAULTS  // Emulated instructions
```

## The `perf_event_open()` Syscall

```c
// Count CPU cycles for this process:
struct perf_event_attr attr = {
    .type = PERF_TYPE_HARDWARE,
    .config = PERF_COUNT_HW_CPU_CYCLES,
    .disabled = 1,
    .exclude_kernel = 1,
    .exclude_hv = 1,
};
int fd = syscall(__NR_perf_event_open, &attr, 0, -1, -1, 0);
ioctl(fd, PERF_EVENT_IOC_ENABLE, 0);
// ... do work ...
ioctl(fd, PERF_EVENT_IOC_DISABLE, 0);
read(fd, &count, sizeof(count));
printf("CPU cycles: %llu\n", count);
```

## perf_event_context

Each task and each CPU has a `perf_event_context` that tracks active events:

```c
struct perf_event_context {
    struct pmu             *pmu;
    raw_spinlock_t          lock;
    struct mutex            mutex;
    struct list_head        active_ctxs;
    struct list_head        pinned_groups;  // Events pinned to CPU
    struct list_head        flexible_groups;// Events that can be rotated
    struct task_struct     *task;
    u64                     time;
    u64                     timestamp;
    /* ... */
};
```

## Sampling Mode

perf can generate samples (call stacks, register state) on every N events:

```bash
# Sample every 1000 CPU cycles, record call stacks:
perf record -e cycles:u -c 1000 -g ./my_program
perf report
```

When the counter overflows, a PMI (Performance Monitoring Interrupt) fires, the kernel captures a sample, and the counter is reset.

## Cross-references

- [Phase overview](../README.md)
- ftrace: [../../05_tracing_debugging/ftrace_init/README.md](../../05_tracing_debugging/ftrace_init/README.md)
