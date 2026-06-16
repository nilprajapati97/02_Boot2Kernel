`cgroup_init_early()` is an **early Linux boot initializer** for cgroups. It runs before normal userspace, before normal process creation, and before the full cgroup filesystem setup.

At a high level: **it creates the root cgroup world so that the first task, `init_task`, already belongs to a valid resource-control hierarchy before the rest of the kernel starts creating tasks.**

Linux cgroups organize processes hierarchically and let controllers such as CPU, memory, I/O, cpuset, freezer, etc. limit, account, or control resources. Kernel docs define cgroup as a mechanism to organize processes hierarchically and distribute resources through controllers. ([Kernel Documentation][1])

## Where it runs in boot

On ARM64, boot roughly looks like:

```text
Firmware / bootloader
   ↓
ARM64 kernel entry
   ↓
head.S: MMU setup, exception level setup, early page tables
   ↓
start_kernel()
   ↓
cgroup_init_early()
   ↓
scheduler, memory, IRQ, SMP, initcalls, userspace
```

So this function is not about parsing `/sys/fs/cgroup` yet. It is about **kernel-internal cgroup data structures**.

The current Linux source says:

```c
int __init cgroup_init_early(void)
{
        static struct cgroup_fs_context __initdata ctx;
        struct cgroup_subsys *ss;
        int i;

        ctx.root = &cgrp_dfl_root;
        init_cgroup_root(&ctx);
        cgrp_dfl_root.cgrp.self.flags |= CSS_NO_REF;

        RCU_INIT_POINTER(init_task.cgroups, &init_css_set);

        for_each_subsys(ss, i) {
                ...
                ss->id = i;
                ss->name = cgroup_subsys_name[i];
                if (!ss->legacy_name)
                        ss->legacy_name = cgroup_subsys_name[i];

                if (ss->early_init)
                        cgroup_init_subsys(ss, true);
        }
        return 0;
}
```

This matches the upstream kernel source around `cgroup_init_early()` lines 6301–6337. ([Code Browser][2])

## From scratch: what problem is this solving?

Before cgroups, a process was just a task scheduled by the kernel. But modern systems need answers like:

```text
How much CPU can this container use?
How much memory can this Android app use?
Which CPUs may this workload run on?
How much I/O did this workload generate?
Can I freeze or kill this workload group?
```

A single process-level view is not enough. You need a **hierarchical resource domain**.

Example:

```text
root cgroup
 ├── system.slice
 ├── user.slice
 └── kubepods.slice
      ├── pod A
      │    ├── container 1
      │    └── container 2
      └── pod B
```

Every task belongs to exactly one cgroup in a hierarchy; controllers apply limits and accounting down the tree. Kernel documentation says every process belongs to one and only one cgroup, and controllers restrict resources hierarchically. ([Kernel Documentation][1])

## What `cgroup_init_early()` initializes

### 1. It creates the default root cgroup

```c
ctx.root = &cgrp_dfl_root;
init_cgroup_root(&ctx);
```

`cgrp_dfl_root` is the **default unified cgroup root**, mainly associated with cgroup v2.

Think of it as:

```text
/
└── root cgroup object
```

At this point, it is not yet a mounted filesystem visible at `/sys/fs/cgroup`. It is an internal kernel object.

Interview phrasing:

> `cgroup_init_early()` creates the root resource-control domain before tasks and controllers need it. It gives the kernel a valid cgroup anchor from the beginning of process lifetime.

### 2. It marks the root cgroup state as no-refcount

```c
cgrp_dfl_root.cgrp.self.flags |= CSS_NO_REF;
```

`CSS` means **cgroup subsystem state**.

Each controller has a per-cgroup state object. For example:

```text
root cgroup
 ├── cpu css
 ├── memory css
 ├── cpuset css
 └── io css
```

`CSS_NO_REF` means this object does not need normal lifetime reference counting.

Why?

Because the root cgroup exists for the life of the kernel. It is not dynamically freed like a user-created cgroup. Avoiding unnecessary refcounting here is both safe and cheaper.

### 3. It attaches the boot task to the initial css_set

```c
RCU_INIT_POINTER(init_task.cgroups, &init_css_set);
```

This is very important.

`init_task` is the first kernel task, PID 0 / swapper. Later it creates kernel threads and eventually userspace PID 1.

`init_css_set` is the initial set of cgroup subsystem states.

Conceptually:

```text
init_task.cgroups ───► init_css_set
                         ├── cpu root state
                         ├── memory root state
                         ├── cpuset root state
                         └── ...
```

Why RCU?

Because cgroup membership is read frequently on hot paths: scheduler, memory charging, task migration, accounting. The kernel wants readers to be fast and mostly lockless. RCU lets readers safely dereference `task->cgroups` while writers update it carefully.

Interview framing:

> `task_struct` points to a `css_set`, not directly to every cgroup. This allows tasks sharing the same controller-state combination to share one object, reducing memory overhead and making membership lookup fast.

### 4. It assigns subsystem IDs and names

```c
for_each_subsys(ss, i) {
        ss->id = i;
        ss->name = cgroup_subsys_name[i];
        if (!ss->legacy_name)
                ss->legacy_name = cgroup_subsys_name[i];
}
```

Each cgroup controller gets a stable ID:

```text
cpu     → id N
memory  → id M
cpuset  → id K
io      → id X
```

Why IDs matter:

```c
init_css_set.subsys[ss->id]
```

The kernel can index into arrays instead of doing string lookup. That matters for performance.

### 5. It initializes only early-init controllers

```c
if (ss->early_init)
        cgroup_init_subsys(ss, true);
```

Not every controller is safe or necessary this early. Some controllers depend on memory allocators, workqueues, filesystem code, percpu infrastructure, or scheduler state that may not be fully ready.

So the design is split:

```text
cgroup_init_early()
    Initialize minimum root + early controllers

cgroup_init()
    Later initialize full cgroup filesystem and remaining controllers
```

The source comment says `cgroup_init_early()` initializes cgroups at system boot and initializes subsystems that request early init. ([Code Browser][2])

## ARMv8 / ARM64 perspective

On ARM64, the important thing is not that cgroups are ARM-specific. They are mostly architecture-independent kernel code. But ARM64 affects the environment in which this code runs.

### CPU state

When `cgroup_init_early()` runs:

```text
Usually only boot CPU is active
Secondary CPUs may not yet be fully online
SMP setup may not be complete
Interrupts and scheduler are still early
Kernel memory mapping exists
Normal userspace does not exist
```

On ARMv8, each CPU core has its own registers, exception levels, caches, TLBs, and per-CPU areas. But cgroup initialization is preparing **logical resource accounting**, not programming ARM registers directly.

The relationship is indirect:

```text
ARM64 CPU core
   ↓ runs kernel code
scheduler
   ↓ asks cgroup CPU controller / cpuset rules
cgroup subsystem state
   ↓ affects task placement, accounting, throttling
```

### Memory perspective

`cgroup_init_early()` uses static boot-time objects:

```c
static struct cgroup_fs_context __initdata ctx;
```

`__initdata` means the data is only needed during initialization. After boot, the kernel can free the `.init.data` memory region.

That is important in kernel design:

```text
Boot-only data should not permanently occupy RAM.
```

On ARM64, kernel memory is mapped through page tables created early in boot. By this stage, the kernel has enough virtual memory support to access global/static kernel objects. But the full dynamic memory ecosystem may not be ready for every subsystem, which is why some cgroup setup is delayed.

The source also shows that early subsystem initialization avoids normal ID allocation because allocation may not be safe during early init. In nearby code, early init uses a fixed CSS ID instead of dynamic ID allocation. ([Code Browser][2])

### Cache and RCU perspective

This line matters:

```c
RCU_INIT_POINTER(init_task.cgroups, &init_css_set);
```

On ARM64, memory ordering is weaker than x86. That means the kernel cannot casually publish shared pointers without ordering rules.

RCU APIs provide the right publication semantics so that when another CPU later reads `task->cgroups`, it sees a valid initialized object.

Interview-grade answer:

> On ARM64, because the memory model is weakly ordered, publishing shared kernel pointers must use primitives such as RCU or release/acquire operations. `RCU_INIT_POINTER()` documents and enforces safe initialization of `init_task.cgroups` before later lockless readers access it.

## Kernel object model

Important objects:

```text
task_struct
    └── cgroups pointer → css_set

css_set
    ├── array of cgroup_subsys_state pointers
    └── shared by tasks with same cgroup membership

cgroup
    ├── hierarchy node
    ├── parent/children links
    └── files exposed later through cgroupfs

cgroup_subsys
    ├── controller definition: cpu, memory, cpuset, io...
    ├── callbacks: css_alloc, css_free, fork, exit...
    └── early_init flag

cgroup_subsys_state
    └── per-controller state for one cgroup
```

Visual:

```text
task_struct(init_task)
   |
   v
init_css_set
   |
   +-- subsys[cpu]    ---> root cpu css
   +-- subsys[memory] ---> root memory css
   +-- subsys[cpuset] ---> root cpuset css
   +-- subsys[io]     ---> root io css
```

## Why this must happen early

Because task creation depends on cgroup state.

When a new task is forked, the kernel needs to know:

```text
Which cgroup does the child inherit?
Should controller fork callbacks run?
Should memory/cpu/accounting state be attached?
```

If `init_task` had no cgroup pointer, early fork paths would need special cases. Instead, the kernel establishes the invariant early:

```text
Every task always has a valid cgroup membership.
```

That is excellent kernel design.

## What happens later in `cgroup_init()`

Later, `cgroup_init()` does heavier work:

```text
register cgroup filesystem
initialize cgroup control files
initialize remaining controllers
set up root hierarchy fully
make cgroups visible to userspace
```

The source after `cgroup_init_early()` shows `cgroup_init()` registering cgroup files, initializing cgroup rstat, setting up the root, and initializing non-early subsystems. ([Code Browser][2])

So:

```text
cgroup_init_early()
    internal boot invariant

cgroup_init()
    full runtime cgroup system
```

## NVIDIA / Google system-design interview angle

A strong answer should connect this tiny function to large-scale resource isolation.

### In a Google/Kubernetes context

Kubernetes relies heavily on cgroups:

```text
Pod CPU limit       → cpu controller
Pod memory limit    → memory controller
Pod cpuset policy   → cpuset controller
Pod I/O behavior    → io / blkio controller
OOM isolation       → memory cgroup
```

Without cgroups, containers would be mostly namespaces without reliable resource control.

In interview language:

> Namespaces answer “what can the process see?” Cgroups answer “how much can the process consume?”

### In an NVIDIA/GPU-system context

GPU workloads still rely on CPU and memory isolation:

```text
GPU training job
 ├── CPU dataloader threads
 ├── pinned host memory
 ├── page cache
 ├── DMA buffers
 ├── NUMA locality
 └── scheduler placement
```

Even though Linux cgroups do not fully virtualize every GPU resource by default, they are still central for:

```text
CPU quota for data preprocessing
memory limit for host RAM
cpuset binding near PCIe/NVLink NUMA node
I/O throttling for checkpoint writes
process containment for multi-tenant systems
```

Interview answer:

> On an NVIDIA multi-tenant training node, cgroups are part of the isolation layer around GPU jobs. The GPU plugin may assign devices, but CPU, RAM, cpuset locality, and process accounting still flow through cgroup infrastructure.

## Final mental model

`cgroup_init_early()` is not “creating containers.”

It is doing this:

```text
Before the kernel creates real process trees,
before userspace mounts cgroupfs,
before all controllers are initialized,

create the root cgroup,
attach init_task to the initial css_set,
name and index all cgroup controllers,
initialize only controllers safe for early boot.
```

Best one-line interview answer:

> `cgroup_init_early()` establishes the kernel’s initial resource-control universe: it creates the default root cgroup, attaches the boot task to the initial cgroup state using RCU-safe publication, assigns IDs/names to cgroup controllers, and initializes only the controllers that must exist before normal task creation and full cgroup filesystem setup.

