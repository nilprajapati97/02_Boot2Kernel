# `init_task` and `task_struct` — The Idle Thread

## Overview

`init_task` is the **statically allocated `task_struct`** representing the kernel's idle thread (PID 0, also called `swapper/0`). It is the only task in the entire kernel that is never created by `fork()` — it exists before any code runs and persists forever as the idle thread.

---

## Static Allocation

Unlike all other tasks which are allocated via `dup_task_struct()` → `kmem_cache_alloc()`, `init_task` is defined in the BSS/data section at compile time:

```c
// init/init_task.c
struct task_struct init_task
#ifdef CONFIG_ARCH_TASK_STRUCT_ON_STACK
    __init_task_data
#endif
    __aligned(L1_CACHE_BYTES)
= {
    .__state            = 0,            // TASK_RUNNING
    .stack              = init_stack,   // statically allocated stack
    .usage              = REFCOUNT_INIT(2),
    .flags              = PF_TASK_WORK,
    .prio               = MAX_PRIO - 20,// DEFAULT_PRIO
    .static_prio        = MAX_PRIO - 20,
    .normal_prio        = MAX_PRIO - 20,
    .policy             = SCHED_NORMAL,
    .cpus_ptr           = &init_task.cpus_mask,
    .user_cpus_ptr      = NULL,
    .cpus_mask          = CPU_MASK_ALL,
    .nr_cpus_allowed    = NR_CPUS,
    .mm                 = NULL,         // kernel thread: no user mm
    .active_mm          = &init_mm,     // borrows kernel mm
    .restart_block      = { .fn = do_no_restart_syscall },
    .se                 = { ... },      // sched entity
    .rt                 = { ... },      // RT sched entity
    .tasks              = LIST_HEAD_INIT(init_task.tasks),
    .mm_tasks           = LIST_HEAD_INIT(init_task.mm_tasks),
    .children           = LIST_HEAD_INIT(init_task.children),
    .sibling            = LIST_HEAD_INIT(init_task.sibling),
    .group_leader       = &init_task,
    .thread_group       = LIST_HEAD_INIT(init_task.thread_group),
    .thread_node        = LIST_HEAD_INIT(init_threads.thread_head),
    .real_parent        = &init_task,
    .parent             = &init_task,   // parent is itself
    .pid                = 0,
    .tgid               = 0,
    .comm               = INIT_TASK_COMM, // "swapper"
    .files              = &init_files,
    .fs                 = &init_fs,
    .signal             = &init_signals,
    .sighand            = &init_sighand,
    .nsproxy            = &init_nsproxy,
    .cred               = &init_cred,
    .thread             = INIT_THREAD,  // arch-specific thread state
    // ... 100+ more fields
};
EXPORT_SYMBOL(init_task);
```

---

## Key `task_struct` Fields

```c
struct task_struct {
    // ── State ──────────────────────────────────────────────────
    unsigned int        __state;        // TASK_RUNNING, TASK_INTERRUPTIBLE, etc.
    void               *stack;          // ptr to kernel stack allocation

    // ── Scheduler ──────────────────────────────────────────────
    int                 prio;           // dynamic priority (0-139)
    int                 static_prio;    // nice-based static priority
    int                 normal_prio;    // normalized priority
    unsigned int        rt_priority;    // RT priority (1-99)
    unsigned int        policy;         // SCHED_NORMAL/FIFO/RR/BATCH/IDLE/DEADLINE
    struct sched_entity se;             // CFS scheduling entity
    struct sched_rt_entity rt;          // RT scheduling entity
    struct sched_dl_entity dl;          // Deadline scheduling entity

    // ── Identification ─────────────────────────────────────────
    pid_t               pid;            // process ID
    pid_t               tgid;           // thread group ID
    char                comm[16];       // name (truncated at 15 chars)

    // ── Memory ─────────────────────────────────────────────────
    struct mm_struct   *mm;             // user address space (NULL for kernel threads)
    struct mm_struct   *active_mm;      // borrowed mm for kernel threads

    // ── Relationships ──────────────────────────────────────────
    struct task_struct *real_parent;    // biological parent
    struct task_struct *parent;         // adoptive parent (for ptrace)
    struct list_head    children;       // list of children
    struct list_head    sibling;        // sibling in parent's children list
    struct task_struct *group_leader;   // thread group leader

    // ── Files & FS ─────────────────────────────────────────────
    struct files_struct *files;         // open file descriptors
    struct fs_struct    *fs;            // filesystem info (cwd, root)

    // ── Credentials ────────────────────────────────────────────
    const struct cred  *ptracer_cred;
    const struct cred  *real_cred;      // objective UID/GID
    const struct cred  *cred;           // effective UID/GID/capabilities

    // ── Signal Handling ────────────────────────────────────────
    struct signal_struct   *signal;
    struct sighand_struct  *sighand;
    sigset_t                blocked;
    sigset_t                real_blocked;
    struct sigpending       pending;

    // ── Namespaces ─────────────────────────────────────────────
    struct nsproxy     *nsproxy;        // all namespace pointers

    // ── Arch-specific ──────────────────────────────────────────
    struct thread_struct thread;        // FPU state, TLS, debug registers
};
```

---

## Stack Allocation

```c
// arch/x86/include/asm/page_64_types.h
#define THREAD_SIZE_ORDER   (2 + KASAN_STACK_ORDER)  // typically 2 for 16KB
#define THREAD_SIZE         (PAGE_SIZE << THREAD_SIZE_ORDER)  // 16384 bytes

// init/init_task.c
unsigned long init_stack[THREAD_SIZE / sizeof(unsigned long)]
    __used __aligned(THREAD_SIZE)
    __attribute__((__section__(".data..init_task")));
```

The initial RSP points near the top of `init_stack`. As `start_kernel()` calls functions, RSP decreases.

---

## Interview Q&A

### Q1: What is PID 0 and how is `init_task` different from all other processes?
**A:** PID 0 is the idle thread (`swapper/0`), represented by `init_task`. It is **statically allocated at compile time** in the kernel image's data section, not created by `fork()`. Every other task is created by `copy_process()` → `dup_task_struct()` which allocates a new `task_struct` via slab cache. `init_task` is unique in that its `parent` and `real_parent` both point to itself, its `mm` is NULL (kernel thread), and it never exits. There is one idle thread per CPU — after SMP bringup, each secondary CPU gets its own idle thread cloned from `init_task`.

### Q2: Why does `init_task.mm = NULL` but `init_task.active_mm = &init_mm`?
**A:** In the Linux kernel, `task->mm == NULL` means the task is a kernel thread — it has no user address space. However, `active_mm` is the mm the task is currently executing with. Kernel threads "borrow" `active_mm` from the last user process that ran on the CPU, to avoid expensive TLB flushes when switching between kernel threads. `init_task` specifically borrows `init_mm`, the kernel's own mm_struct that maps the kernel virtual address space.

### Q3: How does the scheduler handle the idle task?
**A:** The idle task is the **lowest priority task possible** and runs only when no other task is runnable. It is represented by the `SCHED_IDLE` class (or more precisely, it's handled specially in `pick_next_task()`). When `schedule()` finds no runnable tasks, it calls `pick_next_task_idle()` which returns the per-CPU idle thread. The idle thread runs `cpu_idle_loop()` → `arch_cpu_idle()` → `HLT` or `MWAIT` instruction to save power.
