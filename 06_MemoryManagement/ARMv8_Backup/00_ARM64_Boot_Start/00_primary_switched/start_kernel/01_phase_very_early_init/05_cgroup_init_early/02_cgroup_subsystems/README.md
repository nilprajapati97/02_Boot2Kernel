# Cgroup Subsystems — Early vs Late Init

## What Are Cgroup Subsystems?

Cgroup **subsystems** (also called "controllers") are kernel components that integrate with the cgroup hierarchy to enforce resource limits:

| Subsystem | Kernel Component | Functionality |
|-----------|-----------------|---------------|
| `cpu`     | CFS bandwidth controller | CPU time limits, CPU shares |
| `cpuacct` | CPU accounting | CPU usage statistics |
| `memory`  | `mm/memcontrol.c` | Memory limits, OOM per-cgroup |
| `io`      | `block/blk-cgroup.c` | I/O bandwidth throttling |
| `pids`    | `kernel/cgroup/pids.c` | Max process count per cgroup |
| `devices` | (v1 only) | Allow/deny device access |
| `net_cls` | `net/core/netclassid_cgroup.c` | Tag packets with cgroup ID |
| `freezer` | `kernel/cgroup/freezer.c` | Freeze/thaw all tasks in cgroup |
| `cpuset`  | `kernel/cgroup/cpuset.c` | CPU and memory node affinity |
| `hugetlb` | `mm/hugetlb_cgroup.c` | Huge page limits |
| `rdma`    | `drivers/infiniband/...` | RDMA resource limits |
| `misc`    | `kernel/cgroup/misc.c` | Miscellaneous device limits |

---

## Early Init Subsystems

Some subsystems set `ss->early_init = true`. These run during `cgroup_init_early()` (Phase 1, before slab allocator). Currently:

- **`cpuset`** — needs to be ready before `sched_init()` because the scheduler reads cpuset data when placing tasks

The early init path calls `cgroup_init_subsys(ss, true)` which:
1. Calls `ss->css_alloc()` to create the root CSS
2. Calls `ss->css_online()` to bring the root CSS online
3. Assigns the root CSS to `init_css_set.subsys[ss->id]`

---

## Late Init Subsystems

All other subsystems initialize in `cgroup_init()` (Phase 13) after slab allocator, VFS, and procfs are ready. They need:
- `kmalloc()` for CSS allocation
- `kernfs` (procfs-like filesystem) for `/sys/fs/cgroup` mount
- `idr` (ID allocator) for cgroup IDs

---

## Subsystem Registration

Each subsystem is registered via the `SUBSYS()` macro:

```c
// include/linux/cgroup_subsys.h
#define SUBSYS(_x) [_x ## _cgrp_id] = &_x ## _cgrp_subsys,

// kernel/cgroup/cgroup.c
static struct cgroup_subsys *cgroup_subsys[] = {
#include <linux/cgroup_subsys.h>
};
```

And each subsystem defines:
```c
// Example: kernel/cgroup/pids.c
struct cgroup_subsys pids_cgrp_subsys = {
    .css_alloc      = pids_css_alloc,
    .css_free       = pids_css_free,
    .can_attach     = pids_can_attach,
    .cancel_attach  = pids_cancel_attach,
    .attach         = pids_attach,
    .dfl_cftypes    = pids_files,
    .legacy_cftypes = pids_files,
    .subsys_id      = pids_cgrp_id,
    .early_init     = false,
};
```

---

## Interview Q&A

### Q1: How does the memory cgroup enforce OOM (Out-of-Memory) killing per cgroup?
**A:** When a process tries to allocate memory and the cgroup's `memory.limit_in_bytes` would be exceeded, `mem_cgroup_charge()` calls `mem_cgroup_oom()`. The OOM killer (`oom_kill_process()`) is called with the cgroup as the scope — it selects the task with the highest `oom_score` within that cgroup's subtree to kill. This allows containers to have independent OOM behavior: one container killing its tasks doesn't affect other containers. This is fundamental to how Docker/Kubernetes implement memory limits.

### Q2: What is the difference between `memory.limit_in_bytes` (v1) and `memory.max` (v2)?
**A:** Both set hard memory limits, but they differ in behavior and hierarchy:
- **v1 `memory.limit_in_bytes`**: Independent per hierarchy. A cgroup can be limited even if its parent has no limit. Setting -1 removes the limit.
- **v2 `memory.max`**: Hierarchical by design. A child's effective limit is `min(child.max, parent.max)` all the way to root. Uses `max` terminology. Also adds `memory.high` (soft limit that triggers reclaim before hard kill) and `memory.swap.max` separately.

The key v2 improvement: `memory.high` allows gradual memory pressure (causes `kswapd` to reclaim) before hitting `memory.max` (causes OOM kill), giving applications a chance to reduce their footprint gracefully.

### Q3: How does cgroup v2 CPU bandwidth control work?
**A:** `cpu.max` in cgroup v2 takes two values: `$QUOTA $PERIOD` (e.g., `50000 100000` = 50% of one CPU). The CFS bandwidth controller maintains per-cgroup runtime quota. When a task in the cgroup exhausts its quota for the current period, it is **throttled** — moved off the runqueue until the next period begins. `cpu.weight` (1-10000, default 100) controls relative shares between cgroups within the same parent. This is how Kubernetes `resources.limits.cpu` and `resources.requests.cpu` are implemented.
