# CPU Online/Possible/Active/Present State Machine

## State Definitions

| State | Bitmask | Meaning |
|-------|---------|---------|
| **possible** | `cpu_possible_mask` | CPU slot exists (hardware or firmware says it could) |
| **present** | `cpu_present_mask` | CPU physically present and detectable |
| **online** | `cpu_online_mask` | CPU is running, can receive IPIs, can run tasks |
| **active** | `cpu_active_mask` | CPU can receive new task migrations (scheduler active) |

## State Transition Diagram

```
[not possible]
      │ ACPI/DT parsing (setup_arch)
      ▼
[possible] → cpu_possible_mask
      │ ACPI/DT says CPU physically present
      ▼
[present] → cpu_present_mask
      │ cpu_up() → start_secondary() completes
      ▼
[online] → cpu_online_mask
      │ scheduler accepts migrations
      ▼
[active] → cpu_active_mask

Hotplug DOWN:
[active] ─► set_cpu_active(cpu, false)   ← stop new task migrations
[online] ─► set_cpu_online(cpu, false)   ← after cpu executes cpu_die()
[present] ─► set_cpu_present(cpu, false) ← if physically removed
```

## CPU Hotplug State Machine (kernel 4.10+)

Linux 4.10 replaced ad-hoc hotplug callbacks with a formal state machine in `cpuhp_state`:

```c
// include/linux/cpuhotplug.h
enum cpuhp_state {
    CPUHP_OFFLINE = 0,
    // ... many states ...
    CPUHP_AP_ONLINE,
    CPUHP_AP_ONLINE_IDLE,
    CPUHP_AP_SCHED_WAIT_EMPTY,
    // ...
    CPUHP_ONLINE,
};
```

Each subsystem registers callbacks for `startup` (going online) and `teardown` (going offline) at specific state positions. This replaces the old `register_cpu_notifier()` approach.

## Interview Q&A

### Q1: What is the CPUHP (CPU Hotplug) state machine and why was it introduced?
**A:** Before Linux 4.10, CPU hotplug used `register_cpu_notifier()` — any subsystem could register a callback called during CPU up/down. This led to: ordering bugs (subsystem B's callback depended on A's but no explicit ordering existed), crash-on-hotplug bugs, and hard-to-test code paths. The CPUHP state machine introduces **ordered states** — each subsystem picks a position in the ordered list (`CPUHP_AP_PERF_X86_UNCORE_ONLINE` happens before `CPUHP_AP_PERF_ONLINE`). This makes hotplug sequences deterministic and testable.

### Q2: How do you safely iterate over online CPUs while a CPU might be going offline?
**A:** Use `get_online_cpus()` / `put_online_cpus()` (now `cpus_read_lock()` / `cpus_read_unlock()`) to hold a reference count on the CPU online set. CPU hotplug is protected by the `cpu_hotplug_lock` rwsem. Code that iterates `for_each_online_cpu` while holding the read lock is safe — a CPU can't transition out of online state while the read lock is held. For performance-critical paths, per-CPU reference counts (`percpu_ref`) are used instead of a global lock.
