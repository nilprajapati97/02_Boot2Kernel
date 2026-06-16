# `cgroup_init()` — Control Group Initialization

## Purpose

Completes cgroup initialization: registers all built-in cgroup controllers, sets up the default hierarchy, mounts cgroupfs, and creates the root cgroup that all processes start in.

## Source File

`kernel/cgroup/cgroup.c`

```c
int __init cgroup_init(void)
{
    struct cgroup_subsys *ss;
    int ssid;
    
    BUILD_BUG_ON(CGROUP_SUBSYS_COUNT > 16);
    
    // Register all subsystems (controllers):
    for_each_subsys(ss, ssid) {
        ss->id = ssid;
        ss->name = cgroup_subsys_name[ssid];
        if (!ss->legacy_name)
            ss->legacy_name = ss->name;
        
        // Initialize subsystem:
        if (ss->early_init) {
            // Already done in cgroup_init_early() (Phase 1)
        } else {
            cgroup_init_subsys(ss, false);
        }
    }
    
    // Set up default unified hierarchy (cgroup v2):
    cgrp_dfl_root.flags |= CGRP_ROOT_SUPPORT_DEFAULT_HIERARCHY;
    
    // Mount cgroup2 as the default hierarchy:
    cgroup_setup_root(&cgrp_dfl_root, 0);
    
    return 0;
}
```

## cgroup v1 vs cgroup v2

### cgroup v1 (Legacy)

Multiple hierarchies, each with different controllers:
```
/sys/fs/cgroup/memory/   ← memory controller
/sys/fs/cgroup/cpu/      ← cpu + cpuacct
/sys/fs/cgroup/cpuset/   ← cpuset
/sys/fs/cgroup/blkio/    ← block I/O
```

Problems: Complex, inconsistent, multiple trees to manage.

### cgroup v2 (Unified Hierarchy)

Single tree, all controllers in one hierarchy:
```
/sys/fs/cgroup/          ← unified hierarchy
├── cgroup.controllers   ← "cpu io memory ..."
├── cgroup.procs         ← all processes
├── system.slice/
│   └── cgroup.procs
└── user.slice/
    └── cgroup.procs
```

## Built-in Controllers

| Controller | Resource | cgroup v2 file |
|------------|----------|----------------|
| `cpu` | CPU bandwidth | `cpu.max`, `cpu.weight` |
| `cpuacct` | CPU accounting | `cpu.stat` |
| `memory` | RAM + swap | `memory.max`, `memory.current` |
| `io` | Block I/O | `io.max`, `io.weight` |
| `pids` | Process count | `pids.max`, `pids.current` |
| `cpuset` | CPU/memory nodes | `cpuset.cpus`, `cpuset.mems` |
| `freezer` | Suspend/resume | `cgroup.freeze` |
| `net_cls` | Network class | `net_cls.classid` |
| `devices` | Device access | (v1 only) |
| `perf_event` | perf events | (v1 only) |

## Sub-topic: Controllers Detail

See [controllers/README.md](controllers/README.md) for detailed documentation of each controller.

## Memory Controller Example

```bash
# Limit a process group to 512MB:
mkdir /sys/fs/cgroup/myapp
echo "536870912" > /sys/fs/cgroup/myapp/memory.max  # 512MB in bytes
echo $PID > /sys/fs/cgroup/myapp/cgroup.procs

# When memory exceeds limit → OOM killer invoked within cgroup
```

## CPU Controller Example

```bash
# Limit to 50% of one CPU:
echo "50000 100000" > /sys/fs/cgroup/myapp/cpu.max
# Format: quota period (microseconds)
# 50000us per 100000us period = 50%
```

## Cross-references

- [Phase overview](../README.md)
- `cgroup_init_early()`: [../../01_early_cpu_task_setup/cgroup_init_early/README.md](../../01_early_cpu_task_setup/cgroup_init_early/README.md)
