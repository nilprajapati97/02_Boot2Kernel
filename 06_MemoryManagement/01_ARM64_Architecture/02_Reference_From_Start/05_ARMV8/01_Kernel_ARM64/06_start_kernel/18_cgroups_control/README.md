# Phase 18: Control Groups and Task Accounting

## Overview

Initializes the cgroup (control group) subsystem, which provides resource management and accounting for groups of processes. This phase sets up CPU sets, the full cgroup hierarchy, task statistics, and delay accounting.

## Execution Order

| # | Function | Source File | Description |
|---|----------|-------------|-------------|
| 1 | [`cpuset_init()`](cpuset_init/README.md) | `kernel/cpuset.c` | CPU and memory node assignment |
| 2 | [`cgroup_init()`](cgroup_init/README.md) | `kernel/cgroup/cgroup.c` | Full cgroup v1/v2 initialization |
| 3 | [`taskstats_init_early()`](taskstats_init_early/README.md) | `kernel/taskstats.c` | Per-task statistics (netlink) |
| 4 | [`delayacct_init()`](delayacct_init/README.md) | `kernel/delayacct.c` | Per-task delay accounting |

## IRQ State

- **Entry**: Enabled
- **Exit**: Enabled

## cgroup Overview

cgroups organize processes into a tree and apply resource limits/policies to each group:

```
/ (root cgroup)
├── system.slice
│   ├── sshd.service
│   └── NetworkManager.service
├── user.slice
│   └── user-1000.slice
│       └── session-1.scope
│           ├── bash (PID 5000)
│           └── vim  (PID 5001)
└── docker
    └── container_id
        └── nginx (PID 6000)
```

## Function Index

- [cpuset_init/](cpuset_init/README.md)
- [cgroup_init/](cgroup_init/README.md)
- [taskstats_init_early/](taskstats_init_early/README.md)
- [delayacct_init/](delayacct_init/README.md)
