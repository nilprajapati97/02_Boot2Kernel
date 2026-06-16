# Q09 — Design a Deadlock Detection & Prevention System Inside the Kernel

---

## 1. Problem Statement

Deadlocks in the kernel are catastrophic — the system freezes, often silently, with no crash dump and no recovery path. Unlike userspace deadlocks (detectable via `SIGKILL`), a kernel deadlock may hold a spinlock with IRQs disabled, making even the NMI watchdog unreliable.

Design a kernel-level deadlock detection and prevention system that:
- Detects lock dependency cycles at lock-acquisition time (not post-mortem).
- Handles all kernel locking primitives: spinlocks, mutexes, rwlocks, rwsems, RCU.
- Reports the cycle with full call stacks.
- Adds zero overhead in production builds.

This maps directly to Linux's `lockdep` subsystem — a premier example of runtime verification in kernel engineering.

---

## 2. Requirements

### 2.1 Functional Requirements
- Build a directed graph of lock dependency (lock A → lock B = "A held while B acquired").
- Detect cycles in the dependency graph at `lock_acquire()` time.
- Detect invalid lock contexts (e.g., mutex acquired in interrupt context).
- Detect lock ordering inversions across different execution contexts.
- Report: full dependency chain, stack traces at each acquisition point.
- Cover `spinlock_t`, `mutex`, `rwlock_t`, `rwsem`, `semaphore`.

### 2.2 Non-Functional Requirements
- Zero overhead when `CONFIG_LOCKDEP=off` (compile-time elimination).
- When enabled: < 5 µs overhead per lock acquisition (debug builds only).
- Track up to 8192 distinct lock classes.
- Track up to 65536 distinct lock dependency edges.

---

## 3. Constraints & Assumptions

- `CONFIG_LOCKDEP=y` and `CONFIG_PROVE_LOCKING=y` in debug builds.
- Production kernels compile out all lockdep instrumentation.
- Lock classes are identified by `(file, line, name)` — not by pointer address.
- Per-task lock stack: tracks currently held locks (up to 48 nesting depth).

---

## 4. Architecture Overview

```
  Lock Acquisition Path (instrumented)
  ┌──────────────────────────────────────────────────────────────┐
  │  spin_lock(&lock)                                            │
  │       │                                                      │
  │       ▼                                                      │
  │  lock_acquire(&lock->dep_map, ...)                          │
  │       │                                                      │
  │       ├──► validate_chain()                                 │
  │       │         │                                            │
  │       │         ├──► check_deadlock()    ← cycle detection  │
  │       │         ├──► check_irq_usage()   ← context check    │
  │       │         └──► add_lock_dependency() ← graph update   │
  │       │                                                      │
  │       ▼                                                      │
  │  (hardware lock acquisition)                                 │
  └──────────────────────────────────────────────────────────────┘

  Lock Dependency Graph (global)
  ┌──────────────────────────────────────────────────────────────┐
  │  lock_classes[]   ← static array of known lock classes       │
  │  classhash_table  ← hash(file+line) → lock_class            │
  │                                                              │
  │  lock_class A ──► lock_class B  (A held when B acquired)    │
  │  lock_class B ──► lock_class C                               │
  │  lock_class C ──► lock_class A  ← CYCLE DETECTED!           │
  └──────────────────────────────────────────────────────────────┘

  Per-Task Lock Stack (task_struct::lockdep_depth, held_locks[])
  ┌──────────────────────────────────────────────────────────────┐
  │  held_locks[0]: &rq->lock                                    │
  │  held_locks[1]: &task->pi_lock                               │
  │  held_locks[2]: ... (currently being acquired)              │
  └──────────────────────────────────────────────────────────────┘
```

---

## 5. Core Data Structures

### 5.1 Lock Class

```c
struct lock_class {
    struct hlist_node   hash_entry;   /* in classhash_table */
    struct list_head    lock_entry;   /* in all_lock_classes */

    /* Identity */
    const char         *name;         /* "rq->lock", "task->pi_lock" */
    const char         *name_version; /* disambiguates anonymous locks */
    short               subclass;     /* for nested locks of same type */

    /* Dependency edges */
    struct list_head    locks_after;  /* classes we've locked while holding this */
    struct list_head    locks_before; /* classes that held us while acquiring others */

    /* IRQ context usage */
    unsigned long       usage_mask;
    /* Bits: LOCKF_USED_IN_HARDIRQ, LOCKF_ENABLED_HARDIRQ, etc. */

    int                 name_version;
    unsigned long       acquire_ip;   /* instruction pointer of first acquire */
};
```

### 5.2 Lock Dependency Edge

```c
struct lock_list {
    struct list_head    entry;
    struct lock_class  *class;         /* the lock at the other end */
    struct lock_trace  *trace;         /* call stack at time of first edge creation */
    int                 distance;      /* BFS distance for cycle reporting */
    union {
        /* For BFS traversal */
        struct lock_list *parent;
    };
};
```

### 5.3 Per-Task Lock Stack

```c
/* In task_struct: */
struct held_lock {
    u64                 acquire_ip;    /* instruction pointer */
    struct lockdep_map *instance;      /* the specific lock instance */
    struct lock_class  *class;         /* the lock class */
    unsigned int        irq_context;   /* hardirq/softirq/process? */
    unsigned int        trylock:1;
    unsigned int        read:2;        /* 0=write, 1=read, 2=recursive-read */
    unsigned int        check:1;       /* should we validate? */
    unsigned int        hardirqs_off:1;
};

/* Fields in task_struct: */
struct task_struct {
    /* ... */
    unsigned int    lockdep_depth;                /* nesting depth */
    struct held_lock held_locks[MAX_LOCK_DEPTH];  /* MAX_LOCK_DEPTH = 48 */
    u64             curr_chain_key;               /* hash of current lock chain */
    /* ... */
};
```

### 5.4 Chain Cache (optimization)

```c
/*
 * Lock chain = ordered sequence of lock classes currently held.
 * Hash the chain; if seen before, skip full graph validation.
 */
struct lock_chain {
    unsigned int    irq_context:2;
    unsigned int    depth:6;      /* chain length */
    unsigned int    base:24;      /* index into chain_hlocks[] */
    struct hlist_node entry;
    u64             chain_key;
};

/* Global chain cache */
struct hlist_head chainhash_table[CHAINHASH_SIZE];
u16 chain_hlocks[MAX_LOCKDEP_CHAIN_HLOCKS];  /* compact storage */
```

---

## 6. Key Algorithms & Design Decisions

### 6.1 Dependency Graph Construction

On every `lock_acquire(&lock)`:
1. Find (or create) `lock_class` for this lock.
2. For each lock `L` in current task's `held_locks[]`:
   - Check if edge `L → lock` already exists in `L.locks_after`.
   - If not: add new `lock_list` node to `L.locks_after` and `lock.locks_before`.
3. Store acquisition in `held_locks[lockdep_depth++]`.

On every `lock_release(&lock)`:
1. Remove from `held_locks[]` (pop stack).

### 6.2 Cycle Detection — Forward BFS

When adding edge `A → B`, check for cycle: can we reach `A` starting from `B`?

```c
check_noncircular(B, A):
    BFS from B, following locks_after edges
    visited = {}
    queue = [B]
    while queue:
        node = queue.pop()
        if node == A:
            return CYCLE_DETECTED  ← report
        for each dep in node.locks_after:
            if dep.class not in visited:
                queue.append(dep.class)
                dep.parent = node  ← for path reconstruction
    return OK
```

BFS is bounded by `MAX_LOCKDEP_CHAIN_HLOCKS` — if the graph grows too large, lockdep disables itself (`lockdep_enabled = 0`) rather than running out of memory.

### 6.3 IRQ Context Validation

Each lock class tracks **how it's been used** across IRQ contexts:
```
LOCK_USED_IN_HARDIRQ:   this lock has been acquired in hardirq context
LOCK_ENABLED_HARDIRQ:   hardirqs were enabled when this lock was held
```

Violation: Lock A is held with `LOCK_ENABLED_HARDIRQ` AND Lock A has `LOCK_USED_IN_HARDIRQ`:
- This means: task holds A with irqs enabled → hardirq fires → hardirq tries to take A → DEADLOCK.
- Lockdep catches this immediately, even if the hardirq hasn't fired yet.

```c
check_usage(curr, prev, new_bit, msg):
    if (prev->usage_mask & new_bit):
        return print_irq_inversion_bug(curr, prev, msg)
```

### 6.4 Lock Ordering Inversion Detection

Classic ABBA deadlock:
```
CPU 0: lock(A); lock(B)    ← creates edge A→B
CPU 1: lock(B); lock(A)    ← tries to create edge B→A
                              BFS from A following B→A finds A is reachable
                              CYCLE: A→B→A → DEADLOCK REPORT
```

Lockdep reports this the moment CPU 1 tries to acquire A while holding B — **before** the actual deadlock manifests.

### 6.5 Subclasses for Nested Locks of Same Type

A common pattern: acquiring two locks of the same class (e.g., two `struct inode.i_lock`) in nesting order:

```c
/* Wrong: both are class "inode::i_lock" → lockdep sees A→A cycle */
lock(inode1->i_lock);
lock(inode2->i_lock);   ← lockdep reports: circular dep on same class

/* Correct: use subclass annotation */
lock_nested(&inode1->i_lock, I_MUTEX_PARENT);
lock_nested(&inode2->i_lock, I_MUTEX_CHILD);
/* lockdep treats PARENT and CHILD as different classes */
```

### 6.6 Chain Cache Optimization

Full BFS on every lock acquisition would be O(V+E) — too slow even for debug kernels. The **chain cache** memoizes the result:

1. Hash the current sequence of `(lock_class*, subclass)` in `held_locks[]`.
2. If this exact chain has been validated before: skip BFS (O(1) hash lookup).
3. Only run BFS for new chains (new combinations of lock orderings).

In practice, most kernel code takes the same lock orderings repeatedly → cache hit rate > 99% after warmup.

---

## 7. Trade-off Analysis

| Decision | Chosen | Alternative | Reason |
|---|---|---|---|
| Compile-time removal | Yes (`CONFIG_LOCKDEP`) | Runtime flag | Zero overhead in production — compile-time is cleaner |
| Class-based graph | Yes (file+line identity) | Instance-based graph | Class-based: O(1000s classes), not O(millions instances) |
| BFS for cycle detection | Yes | DFS | BFS gives shortest path → cleaner cycle report |
| Chain cache | Yes | Re-validate every time | 99% cache hit; avoids O(V+E) on hot paths |
| Self-disable on overflow | Yes | Crash/panic | Overflow in debug builds should not break production |

---

## 8. Real Linux Kernel References

| Component | Source | Symbol |
|---|---|---|
| Lockdep core | `kernel/locking/lockdep.c` | `lock_acquire()`, `lock_release()`, `validate_chain()` |
| IRQ usage | `kernel/locking/lockdep.c` | `check_usage()`, `mark_lock()` |
| Cycle detection | `kernel/locking/lockdep.c` | `check_noncircular()` |
| Held lock stack | `include/linux/lockdep.h` | `struct held_lock`, `MAX_LOCK_DEPTH` |
| Chain cache | `kernel/locking/lockdep.c` | `lookup_chain_cache()` |
| Spinlock integration | `include/linux/spinlock.h` | `spin_lock()` → `_raw_spin_lock()` → `lock_acquire()` |
| Subclasses | `include/linux/lockdep.h` | `lock_acquire_exclusive()`, `lockdep_set_class_and_subclass()` |
| Lockdep printing | `kernel/locking/lockdep.c` | `print_circular_bug()` |

---

## 9. Failure Modes & Debug Strategies

### 9.1 Lockdep Disables Itself
```bash
dmesg | grep "lockdep is turned off"
# Cause: MAX_LOCKDEP_ENTRIES or MAX_LOCKDEP_CHAINS exceeded
# Fix: increase limits in include/linux/lockdep.h or reduce lock classes
```

### 9.2 False Positive — Same Class, Different Usage Patterns
```bash
# lockdep reports cycle but it's logically impossible
# Fix: use lock_nested() with distinct subclasses
# Or: lockdep_set_class(&lock, &my_custom_class_key)
```

### 9.3 Real Deadlock in Production (no lockdep)
```bash
# Use hung_task watchdog:
cat /proc/sys/kernel/hung_task_timeout_secs
# Sysrq-T (Alt+SysRq+T) dumps all task stacks — shows who's waiting on what
echo t > /proc/sysrq-trigger

# Magic SysRq + W: dumps blocked tasks
echo w > /proc/sysrq-trigger
```

### 9.4 Soft Lockup (spinlock held too long)
```bash
# "BUG: soft lockup - CPU#X stuck for XXs!"
# NMI watchdog detects CPU not scheduling for > watchdog_thresh seconds
# dmesg backtrace shows the stuck CPU's call stack
cat /proc/sys/kernel/watchdog_thresh  # default 10s
```

---

## 10. Performance Considerations

- **Production overhead:** Exactly zero — all `lock_acquire()` calls are `static_branch_unlikely()` gated on lockdep being enabled. When disabled, the branch is patched to a NOP.
- **Debug overhead:** Chain cache hit: ~100 ns. Chain cache miss (new ordering): ~5 µs (BFS). Acceptable for debug/CI kernels.
- **Memory footprint:** `lock_classes[]` array: 8192 entries × 256 bytes = 2 MB static allocation. Lockdep is not suitable for embedded kernels.
- **Boot time:** Lockdep initializes all held-lock stacks per task at `task_struct` allocation — zero-cost in production.

---

## 11. Interview Answer Strategy (NVIDIA 10-yr Level)

**What they want to hear:**
1. Class-based (not instance-based) graph — explains scalability.
2. IRQ context annotation — `LOCK_USED_IN_HARDIRQ` vs `LOCK_ENABLED_HARDIRQ` — the most subtle part.
3. Chain cache as the optimization that makes lockdep practical.
4. Subclass annotations for nested same-type locks — `lock_nested()`.
5. How lockdep detects future deadlocks before they happen — ordering validation.
6. Production deployment: `CONFIG_LOCKDEP` in CI/test kernels only; zero overhead in shipping kernels.
7. For NVIDIA: GPU driver has deep lock nesting (channel lock → device lock → bar lock) — lockdep annotation discipline is critical.
