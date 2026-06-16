# Cgroup Root Structure Deep Dive

## Hierarchy of Cgroup Data Structures

```
cgroup_root (cgrp_dfl_root)
│   ├── nr_cgrps              ← total cgroups in this root
│   ├── flags                 ← CGRP_ROOT_* flags
│   └── cgrp  ─────────────► cgroup (root cgroup, id=1)
│                               │
│                               ├── self ────────────► cgroup_subsys_state
│                               │                        (base CSS for root)
│                               ├── children          ← list of child cgroups
│                               ├── id = 1            ← idr-allocated ID
│                               ├── level = 0         ← root is level 0
│                               └── kn ──────────────► kernfs_node ("/")
│
│   (child cgroup example: /system.slice)
└── cgroup (id=2, level=1)
        ├── self ───────────► cgroup_subsys_state (per-subsys)
        ├── parent ─────────► cgrp_dfl_root.cgrp
        ├── children         ← list
        └── subsys[] ───────► one css per subsystem
```

## `cgroup_subsys_state` (CSS)

```c
struct cgroup_subsys_state {
    struct cgroup       *cgroup;      // cgroup this CSS belongs to
    struct cgroup_subsys *ss;         // which subsystem
    struct percpu_ref    refcnt;      // reference counting
    struct list_head     sibling;     // siblings at same level
    struct list_head     children;    // child CSSes
    int                  id;          // unique ID
    unsigned int         flags;       // CSS_ONLINE, CSS_DYING, etc.
};
```

Every cgroup subsystem (cpu, memory, io, pids, ...) has exactly one `cgroup_subsys_state` per cgroup node. When you put a task in `/cgroup/A/B`, that task gets CSS pointers for each subsystem at the `/A/B` level.

## Memory Layout

```
cgroup_root [1 per hierarchy]
    └── cgroup [N per root, tree structure]
            └── cgroup_subsys_state [M per cgroup, one per subsystem]
                        │
task_struct.cgroups ──► css_set [shared if same cgroup path for all subsystems]
                            └── subsys[] → css pointers (one per subsystem)
```

## Interview Q&A

### Q1: How does cgroup deletion work when tasks are still in it?
**A:** Deleting a cgroup requires moving all tasks to the parent cgroup first. The `rmdir` on the cgroup's kernfs node triggers `cgroup_rmdir()`. If the cgroup has tasks, `rmdir` fails with `EBUSY`. Once all tasks are migrated (via writing PIDs to `cgroup.procs` of the parent), the cgroup transitions to `dying` state — new tasks cannot be added. The actual memory is freed when the last reference (`percpu_ref`) drops to zero, which happens after all `css_tryget()` references are released. This can be delayed if BPF programs or perf events hold CSS references.

### Q2: What is a `css_task_iter` and when is it used?
**A:** `css_task_iter` iterates over all tasks in a cgroup hierarchy. It is used by memory pressure (OOM killer picks tasks in the cgroup), CPU bandwidth enforcement (CFS bandwidth), and management tools that list tasks per cgroup. The iterator holds a lock-free snapshot using RCU to avoid long lock holds during iteration over potentially thousands of tasks.
