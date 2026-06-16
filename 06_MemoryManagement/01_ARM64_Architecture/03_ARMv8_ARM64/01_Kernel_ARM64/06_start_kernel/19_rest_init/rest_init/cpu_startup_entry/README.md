# `cpu_startup_entry()` — CPU Idle Loop

## Purpose

Transforms the calling CPU (CPU 0 in this context, via `rest_init()`) into the idle task for that CPU. The CPU enters the idle loop and runs `do_idle()` whenever no runnable task is available.

## Source File

`kernel/sched/idle.c`

```c
void cpu_startup_entry(enum cpuhp_state state)
{
    arch_cpu_idle_prepare();
    cpuhp_online_idle(state);
    while (1)
        do_idle();
}
```

## `do_idle()` — The Idle Loop

```c
static void do_idle(void)
{
    int cpu = smp_processor_id();
    
    // Check if we should schedule before going idle:
    if (need_resched()) {
        __schedule(SM_NONE);
        return;
    }
    
    // Handle pending RCU callbacks, timers, etc.:
    rcu_idle_enter();
    
    // Check for pending work one more time:
    if (!need_resched()) {
        stop_critical_timings();
        
        // Enter CPU-specific idle state:
        arch_cpu_idle();        // ← HLT on x86
        
        start_critical_timings();
    }
    
    // We've been woken up (interrupt occurred):
    rcu_idle_exit();
    
    // Check for need_resched again and schedule if needed:
    schedule_idle();
}
```

## `arch_cpu_idle()` — x86 HLT

On x86, the idle state is entered with the `HLT` (Halt) instruction:

```c
// arch/x86/kernel/process.c:
void arch_cpu_idle(void)
{
    x86_idle();  // Calls mwait_idle() or hlt_play_dead()
}

static void hlt_idle(void)
{
    local_irq_enable();
    asm volatile("hlt");  // ← CPU halts, resumes on interrupt
    // After HLT returns (due to interrupt):
    // IRQ handler runs, then execution continues here
}
```

## Power States

Modern CPUs have deeper idle states (C-states) for better power savings:

| C-State | Name | Wake Latency | Power Savings |
|---------|------|-------------|---------------|
| C0 | Active | 0 | 0% |
| C1/C1E | Halt | <1µs | ~50% |
| C2 | Stop-Clock | ~10µs | ~60% |
| C3 | Sleep | ~50µs | ~80% |
| C6 | Deep Power Down | ~100µs | ~95% |
| C10 | Enhanced Deep | ~300µs | ~99% |

The `intel_idle` or `acpi_idle` driver selects the appropriate C-state via `MWAIT` instruction or ACPI `_CST` methods.

## Idle Load Balancing

While a CPU is idle, it can steal tasks from busy CPUs:

```c
// In do_idle():
cpuidle_idle_call()
    → newidle_balance()  // Pull tasks from busy CPUs
```

## CPU Hotplug Integration

`cpu_startup_entry()` is also used when bringing up secondary CPUs during SMP init (Phase 15's `smp_init()`). Each new CPU calls `cpu_startup_entry()` after completing its own initialization.

## Cross-references

- [Parent: rest_init](../README.md)
- Secondary CPUs: [../../19_rest_init/kernel_init/kernel_init_freeable/smp_init/README.md](../../19_rest_init/kernel_init/kernel_init_freeable/smp_init/README.md)
