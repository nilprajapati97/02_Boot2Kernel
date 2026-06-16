# `cpuset_init()` — CPU and Memory Set Control

## Purpose

Initializes the cpuset subsystem, which allows binding a group of processes to specific CPUs and NUMA memory nodes. cpuset is both a cgroup controller and a standalone mechanism used by NUMA-aware workloads.

## Source File

`kernel/cpuset.c`

```c
int __init cpuset_init(void)
{
    BUG_ON(percpu_rwsem_init(&cpuset_rwsem));
    return 0;
}
```

The heavier initialization runs later as a cgroup subsystem via `cgroup_init()`.

## cpuset Functionality

### CPU Binding

A cpuset defines which CPUs a process may run on:

```bash
# Create a cpuset for real-time workload:
mkdir /sys/fs/cgroup/cpuset/realtime
echo "2-7" > /sys/fs/cgroup/cpuset/realtime/cpuset.cpus
echo "0" > /sys/fs/cgroup/cpuset/realtime/cpuset.mems

# Assign a process:
echo $PID > /sys/fs/cgroup/cpuset/realtime/cgroup.procs
```

### Memory Node Binding

On NUMA systems, cpuset controls which memory nodes a process can allocate from:

```bash
# Restrict process to NUMA node 0 memory only:
echo "0" > /sys/fs/cgroup/cpuset/mygroup/cpuset.mems
```

## cpuset Hierarchy

cpusets form a tree. A child cpuset's allowed CPUs/nodes must be a subset of its parent's:

```
root_cpuset: cpus=0-15, mems=0-3
    │
    ├── web_servers: cpus=0-7, mems=0-1
    │       └── php_workers: cpus=0-3, mems=0
    │
    └── database: cpus=8-15, mems=2-3
```

## Key Files in cpuset cgroup

| File | Description |
|------|-------------|
| `cpuset.cpus` | Allowed CPU list (e.g., "0-3,7") |
| `cpuset.mems` | Allowed memory nodes |
| `cpuset.cpu_exclusive` | Exclusive CPU use (no sharing) |
| `cpuset.mem_exclusive` | Exclusive memory use |
| `cpuset.mem_hardwall` | Hard memory boundary |
| `cpuset.memory_migrate` | Migrate pages when changing mems |
| `cpuset.sched_load_balance` | Enable load balancing within cpuset |

## Integration with Scheduler

When a task is assigned to a cpuset, the scheduler's `select_task_rq()` only considers CPUs in the cpuset's `cpus_allowed` mask:

```c
// In kernel/sched/core.c:
int select_task_rq(struct task_struct *p, int prev_cpu, int wake_flags)
{
    // task->cpus_ptr is AND of cpuset allowed + task affinity
    if (cpumask_test_cpu(cpu, p->cpus_ptr))
        // ... select this CPU
}
```

## Cross-references

- [Phase overview](../README.md)
- `cgroup_init()`: [../cgroup_init/README.md](../cgroup_init/README.md)
- `sched_init()`: [../../06_scheduling/sched_init/README.md](../../06_scheduling/sched_init/README.md)
