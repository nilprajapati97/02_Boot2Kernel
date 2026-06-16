# `cgroup_init_early()` — Early Cgroup Bootstrap

## Overview

| Attribute    | Value                                           |
|-------------|--------------------------------------------------|
| **Function** | `cgroup_init_early(void)`                       |
| **Source**   | `kernel/cgroup/cgroup.c`                        |
| **Purpose**  | Initialize the cgroup root structure and bootstrap early subsystems that require cgroup setup before memory allocation is available |

---

## Why It Exists

Cgroups (Control Groups) provide resource isolation — every process in the system belongs to exactly one cgroup hierarchy. The initial process (`init_task`, PID 0) must belong to a valid cgroup before any scheduling or resource management can begin. Therefore, at minimum, the **root cgroup** must be created before `sched_init()` and before any tasks are scheduled.

Additionally, some cgroup subsystems need to perform initialization actions before the memory allocator is ready (`is_early_init` flag):
- These run their `early_init()` method during `cgroup_init_early()`
- Later, full initialization runs in `cgroup_init()`

---

## Prerequisites

- `init_task` must be statically initialized (`set_task_stack_end_magic` ensures the task is valid)
- No dynamic memory allocation needed — root cgroup is statically allocated

---

## Internal Deep Dive

```c
// kernel/cgroup/cgroup.c
int __init cgroup_init_early(void)
{
    static struct cgroup_fs_context __initdata ctx;
    struct cgroup_subsys *ss;
    int i;

    ctx.root = &cgrp_dfl_root;      // default cgroup root (cgroup v2)
    init_cgroup_root(&ctx);          // init root cgroup node
    cgrp_dfl_root.cgrp.self.flags |= CSS_NO_REF;

    RCU_INIT_POINTER(init_task.cgroups, &init_css_set);  // assign init_task to root

    for_each_subsys(ss, i) {
        if (ss->early_init)
            cgroup_init_subsys(ss, true);   // run early_init for this subsys

        if (ss->implicit_on_dfl)
            cgrp_dfl_implicit_ss_mask |= 1 << i;
        else if (ss->dfl_cftypes)
            cgrp_dfl_opted_out_ss_mask |= 1 << i;
    }
    return 0;
}
```

### Key Data Structures Initialized

#### `cgrp_dfl_root` — Default Root (cgroup v2)
```c
// kernel/cgroup/cgroup-internal.h
struct cgroup_root cgrp_dfl_root = {
    .cgrp = {
        .self = { /* css = cgroup_subsys_state */ },
        .flags = 0,
        .id = 1,
    },
    .flags = CGRP_ROOT_NS_DELEGATE,
    .name = "",
};
```

#### `init_css_set` — Initial CSS Set
```c
// kernel/cgroup/cgroup.c
struct css_set init_css_set = {
    .refcount        = REFCOUNT_INIT(1),
    .dom_cset        = &init_css_set,
    .tasks           = LIST_HEAD_INIT(init_css_set.tasks),
    .mg_tasks        = LIST_HEAD_INIT(init_css_set.mg_tasks),
    .dying_tasks     = LIST_HEAD_INIT(init_css_set.dying_tasks),
    .task_iters      = LIST_HEAD_INIT(init_css_set.task_iters),
    .threaded_csets  = LIST_HEAD_INIT(init_css_set.threaded_csets),
    // one css per subsystem all pointing to root cgroup
    .subsys = { &cgrp_dfl_root.cgrp.self, ... },
};
```

Every task has a `css_set` that points to the cgroup_subsys_state for each enabled subsystem. `init_task.cgroups` points to `init_css_set`, and every task created before `cgroup_init()` will inherit this.

---

## Sub-Topics (Deep Dive)

- [01_cgroup_root_structure](01_cgroup_root_structure/README.md) — `cgroup_root`, `cgroup`, `css` structure hierarchy
- [02_cgroup_subsystems](02_cgroup_subsystems/README.md) — What are cgroup subsystems? Early init vs. late init

---

## Interview Q&A

### Q1: Why does `cgroup_init_early()` run before `sched_init()`?
**A:** The scheduler's `sched_init()` sets up the root task group and per-CPU runqueues. The task group (`task_group`) concept in the scheduler is integrated with the `cpu` cgroup subsystem — which uses the `css` (cgroup subsystem state) structure. To create the root `task_group`, the cgroup infrastructure (specifically `init_css_set`) must already exist. If `cgroup_init_early()` ran after `sched_init()`, `sched_init()` would find `init_task.cgroups` pointing to garbage.

### Q2: What is `css_set` and why does each task need one?
**A:** A `css_set` (cgroup subsystem state set) is a **per-task pointer array** holding a `cgroup_subsys_state` pointer for each cgroup subsystem. For a task in cgroup `/system.slice/ssh.service`, the `css_set` has pointers to the memory controller state, CPU controller state, etc. for that specific cgroup path. The key insight: if two tasks are in **exactly the same cgroup for all subsystems**, they share the same `css_set` (reference counted). This optimization reduces memory usage dramatically in systems with thousands of tasks in a small number of cgroups.

### Q3: What is the difference between cgroup v1 and cgroup v2 at the root structure level?
**A:** In **cgroup v1**, each subsystem has its own independent hierarchy root — you could mount `cpu` at `/sys/fs/cgroup/cpu` and `memory` at `/sys/fs/cgroup/memory` with completely different tree structures. This led to inconsistencies (a process could be in `/A/B` in the cpu hierarchy but `/C` in the memory hierarchy). In **cgroup v2** (unified hierarchy), all subsystems share one tree under `/sys/fs/cgroup`. `cgrp_dfl_root` represents this unified root. A task has one cgroup path like `/system.slice/app.service` that applies to all enabled subsystems.
