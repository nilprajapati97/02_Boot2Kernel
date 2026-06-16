# Q06 — Design a Scalable Locking Mechanism (Spinlocks vs Mutex vs RCU)

---

## 1. Problem Statement

Designing the right locking primitive is one of the highest-leverage decisions in kernel engineering. The wrong choice causes either:
- **Correctness failures:** sleeping in atomic context, priority inversion, deadlock.
- **Performance failures:** cache-line bouncing, contention collapse, false sharing.

Design a locking framework that covers all kernel access patterns — from NMI-safe per-CPU counters to long-held read-dominated data structure locks — and provides a clear decision matrix for choosing the right primitive.

---

## 2. Requirements

### 2.1 Functional Requirements
- Mutual exclusion for short critical sections in any context (hardirq, softirq, process).
- Read-write differentiation: readers should never block writers spuriously.
- Support for NMI/hardirq/softirq context constraints.
- Priority inheritance for RT tasks (`CONFIG_PREEMPT_RT`).
- Deadlock detection in debug builds.
- Per-CPU lock-free patterns for statistics and per-CPU state.

### 2.2 Non-Functional Requirements
- Spinlock acquisition: < 20 ns on uncontended fast path.
- Read-side RCU critical section overhead: ~0 ns (no lock/unlock).
- Mutex acquisition: allowed to sleep, O(1) wakeup on release.
- Correct behavior under `CONFIG_PREEMPT_RT` (spinlocks become sleepable mutexes).

---

## 3. Constraints & Assumptions

- Linux kernel with `CONFIG_SMP=y`, `CONFIG_PREEMPT=y`.
- x86-64: `cmpxchg`, `xchg`, `lock; decl` for atomic ops.
- `CONFIG_LOCKDEP=y` in debug builds.
- `CONFIG_PREEMPT_RT` compatibility considered for driver code.

---

## 4. Architecture Overview — Locking Decision Tree

```
  Where does the lock need to be acquired?
  │
  ├──► NMI / perf interrupt context
  │         → Per-CPU variable (no lock)
  │         → atomic_t / atomic64_t (lock-free)
  │
  ├──► Hardirq context (interrupt handler)
  │         → spinlock_t with irq disabled
  │           spin_lock_irqsave(&lock, flags)
  │
  ├──► Softirq / tasklet context
  │         → spinlock_t + bh disabled
  │           spin_lock_bh(&lock)
  │
  ├──► Process context — short critical section (< 1 µs)
  │         → spinlock_t (preemption disabled)
  │           spin_lock(&lock)
  │
  ├──► Process context — long critical section (can sleep)
  │         → mutex_t
  │           mutex_lock(&mutex)
  │
  ├──► Read-mostly data (rare writes, frequent reads)
  │    ├── Writes are infrequent, reads are performance-critical
  │    │       → RCU (read-copy-update)
  │    │         rcu_read_lock() / rcu_read_unlock()
  │    └── Readers need to sleep (file operations, etc.)
  │            → rwsem (read-write semaphore)
  │              down_read(&rw_sem)
  │
  └──► Per-CPU statistics / counters (no contention needed)
            → this_cpu_add(), per_cpu_ptr()
              (lock-free by design — only one CPU accesses each instance)
```

---

## 5. Core Data Structures

### 5.1 Spinlock

```c
typedef struct spinlock {
    union {
        struct raw_spinlock rlock;
        /* Under PREEMPT_RT: a sleeping lock */
    };
} spinlock_t;

struct raw_spinlock {
    arch_spinlock_t raw_lock;
    /* Under CONFIG_DEBUG_SPINLOCK: owner, owner_cpu, magic */
};

/* x86-64 queued spinlock (MCS-based) */
typedef struct qspinlock {
    union {
        atomic_t val;
        struct {
            u8  locked;   /* 0 = free, 1 = locked */
            u8  pending;  /* next-in-line waiter */
            u16 tail;     /* queue tail (CPU index) */
        };
    };
} arch_spinlock_t;
```

### 5.2 Mutex

```c
struct mutex {
    atomic_long_t   owner;    /* task_struct * of current owner, or flags */
    raw_spinlock_t  wait_lock; /* protects wait_list */
    struct list_head wait_list; /* queued waiters */
    /* Debug fields under CONFIG_DEBUG_MUTEXES */
};
```

### 5.3 RCU Internal State (simplified)

```c
struct rcu_state {
    struct rcu_node     node[NUM_RCU_NODES];  /* quiescent-state tracking tree */
    struct rcu_data __percpu *data;           /* per-CPU callbacks/state */
    unsigned long        gp_seq;             /* grace period sequence number */
    struct task_struct  *gp_kthread;         /* kthread driving GP */
};

struct rcu_data {
    unsigned long   gp_seq_needed;    /* GP this CPU needs to complete */
    struct rcu_head *nxtlist;         /* pending callbacks */
    long            qlen;             /* callback queue length */
    bool            core_needs_qs;   /* needs quiescent state reported */
};
```

---

## 6. Key Algorithms & Design Decisions

### 6.1 x86-64 Queued Spinlock (qspinlock) — MCS Lock Variant

Linux replaced the old ticket spinlock with a **queued spinlock** (qspinlock) in kernel 4.2. On uncontended acquisition:
```c
/* Fast path: CAS 0 → 1 on locked byte */
if (likely(atomic_try_cmpxchg_acquire(&lock->val, &val, _Q_LOCKED_VAL)))
    return;  /* acquired in ~4 cycles */

/* Slow path: join the MCS queue */
pv_queued_spin_lock_slowpath(lock, val);
```

**Why MCS over ticket spinlock:**
- Ticket spinlock: all waiters spin on the same cache line → O(N) cache invalidations on release.
- MCS / qspinlock: each CPU spins on its own per-CPU node → O(1) cache invalidations on release.
- On a 512-core NUMA system, this is a 512x improvement in bus traffic on a heavily contended lock.

### 6.2 Spinlock Variants and Context Rules

```c
spin_lock(&lock)            /* disables preemption, NOT irqs */
spin_lock_bh(&lock)         /* disables preemption + softirqs */
spin_lock_irq(&lock)        /* disables preemption + hardirqs (assume irqs were enabled) */
spin_lock_irqsave(&lock, f) /* disables preemption + hardirqs + saves irq state */
```

**Rule:** If a spinlock can be acquired in both hardirq context AND process context, you MUST use `spin_lock_irqsave` in the process context path — otherwise a hardirq fires while you hold the lock, tries to acquire it, and deadlocks.

**`lockdep` catches this:** annotates each lock with its acquisition context; reports invalid context switches.

### 6.3 Mutex — Priority Inheritance Under PREEMPT_RT

Under `CONFIG_PREEMPT_RT`, all spinlocks become `sleeping spinlocks` (backed by `rt_mutex`). `rt_mutex` implements **priority inheritance (PI)**:

```
Task A (prio 5) holds lock L
Task B (prio 90) waits for lock L
  → PI: boost Task A to prio 90 while it holds L
  → Task A completes, releases L, drops back to prio 5
  → Task B acquires L at prio 90
```

This prevents priority inversion that would otherwise occur on RT systems.

```c
struct rt_mutex {
    raw_spinlock_t  wait_lock;
    struct rb_root_cached waiters;  /* rb-tree ordered by priority */
    struct task_struct *owner;
};
```

### 6.4 RCU (Read-Copy-Update) Deep Dive

RCU is the most important synchronization primitive for read-mostly kernel data structures (routing tables, inode caches, module lists, network device lists).

**Core invariant:** Readers never acquire a lock. After a writer updates a data structure, it waits for a **grace period** — a time when all CPUs have passed through a **quiescent state** (exited the RCU read-side critical section, scheduled, gone idle, etc.). Only then does the writer free the old data.

**Read side:**
```c
rcu_read_lock();          /* disables preemption (TREE RCU), marks start of RS */
    p = rcu_dereference(gbl_ptr);  /* READ_ONCE + dependency barrier */
    use(p);
rcu_read_unlock();        /* re-enables preemption */
/* p cannot be dereferenced after rcu_read_unlock! */
```

**Write side:**
```c
new_obj = kmalloc(...);
/* initialize new_obj */
old_obj = rcu_dereference_protected(gbl_ptr, lockdep_is_held(&my_lock));
rcu_assign_pointer(gbl_ptr, new_obj);  /* WRITE_ONCE + smp_mb */
synchronize_rcu();         /* wait for all readers of old_obj to finish */
kfree(old_obj);            /* safe now — no readers hold reference */
```

**Why `synchronize_rcu()` is not a lock:** It doesn't prevent new readers from starting — it only waits for *existing* readers to finish. New readers of `new_obj` can start immediately.

### 6.5 Choosing Between RCU and rwlock

| Criterion | RCU | rwlock |
|---|---|---|
| Reader overhead | ~0 (preempt_disable/enable only) | Cache-line ping-pong on reader count |
| Writer overhead | `synchronize_rcu()` can take ms | `write_lock()` is fast but blocks all readers |
| Reader can sleep | No (TREE RCU) | Yes (rwsem) |
| Multiple simultaneous writers | Only one (protected by separate lock) | Only one (same) |
| Memory reclaim | Deferred to grace period | Immediate on write_unlock |

**Use RCU for:** network routing table, netdev list, filesystem dentry cache, module list, SELinux AVC.
**Use rwsem for:** file system VMA list (`mmap_lock`), inode metadata that readers need to hold across sleeps.

### 6.6 Per-CPU Variables — Lock-Free by Architecture

```c
DEFINE_PER_CPU(u64, bytes_received);

/* Increment without any lock (per-CPU, only this CPU touches it) */
this_cpu_add(bytes_received, len);

/* Read sum across all CPUs */
u64 total = 0;
for_each_possible_cpu(cpu)
    total += per_cpu(bytes_received, cpu);
```

Per-CPU variables are lock-free because each CPU has its own copy. The cost is aggregation (O(NR_CPUS)) for reads, which is acceptable for counters read infrequently.

---

## 7. Trade-off Analysis

| Primitive | Overhead | Context | Read performance | Write performance | Use case |
|---|---|---|---|---|---|
| spinlock | Low (fast path) | Any (atomic) | Equal to write | Equal to read | Short CS in any context |
| mutex | Medium (syscall) | Process only | No differentiation | No differentiation | Long CS, can sleep |
| rwlock | Medium | Atomic | Low (shared lock) | High (exclusive) | Read-heavy, short sections |
| rwsem | Medium | Process | Low (shared) | High | Read-heavy, can sleep |
| RCU | ~0 read | Any | **Zero** | High (deferred free) | Read-dominated, pointer-based |
| per-CPU | ~0 | Any | ~0 (local only) | ~0 (local only) | Stats, per-CPU state |

---

## 8. Real Linux Kernel References

| Component | Source | Symbol |
|---|---|---|
| Queued spinlock | `kernel/locking/qspinlock.c` | `queued_spin_lock_slowpath()` |
| Mutex | `kernel/locking/mutex.c` | `mutex_lock()`, `__mutex_lock()` |
| RT mutex (PI) | `kernel/locking/rtmutex.c` | `rt_mutex_lock()`, `rt_mutex_adjust_prio()` |
| RCU tree | `kernel/rcu/tree.c` | `synchronize_rcu()`, `rcu_gp_kthread()` |
| RCU read side | `include/linux/rcupdate.h` | `rcu_read_lock()`, `rcu_dereference()` |
| rwsem | `kernel/locking/rwsem.c` | `down_read()`, `up_write()` |
| lockdep | `kernel/locking/lockdep.c` | `lock_acquire()`, `lock_release()` |
| Per-CPU | `include/linux/percpu-defs.h` | `this_cpu_add()`, `DEFINE_PER_CPU()` |
| Seqlock | `include/linux/seqlock.h` | `read_seqbegin()`, `write_seqlock()` |

---

## 9. Failure Modes & Debug Strategies

### 9.1 Deadlock — Detected by lockdep
```bash
# Enable lockdep:
# CONFIG_LOCKDEP=y, CONFIG_PROVE_LOCKING=y
# Deadlock report in dmesg:
# "possible circular locking dependency"
# Shows: lock acquisition order that creates a cycle
```

### 9.2 Priority Inversion — Detected by RT Throttling
```bash
# Under PREEMPT_RT, spinlocks become rt_mutex
# Priority inversion: high-prio task waits for low-prio task holding lock
# Check: /proc/<pid>/status | grep "voluntary_ctxt"
# Fix: pi_mutex or ensure critical section holders have elevated priority
```

### 9.3 RCU Usage-After-Free
```bash
# Use CONFIG_PROVE_RCU and CONFIG_RCU_STRICT_GRACE_PERIOD
# KASAN detects the access after kfree
# Typical bug: dereferencing rcu pointer after rcu_read_unlock()
```

### 9.4 Spinlock Held Too Long
```bash
# lockstat shows contention:
echo 1 > /proc/sys/kernel/lock_stat
cat /proc/lock_stat | head -20
# Shows: lock name, contentions, wait times
```

---

## 10. Performance Considerations

- **False sharing on lock structure:** Keep `struct mutex` / `spinlock_t` on its own cache line, separate from protected data, to avoid evicting the data when the lock is contested.
- **RCU batching:** `call_rcu()` (asynchronous) is better than `synchronize_rcu()` in write-heavy paths — callbacks are batched and run after the grace period.
- **Seqlock for stats:** `seqlock` allows O(1) read-side with retry (no lock at all for reads); ideal for `jiffies`, `timespec` updates.
- **Lock elision (TSX):** Intel TSX hardware transactional memory can speculatively elide spinlocks — aborts fall back to normal locking. Disabled in most kernels due to TAA vulnerability.

---

## 11. Interview Answer Strategy (NVIDIA 10-yr Level)

**What they want to hear:**
1. Queued spinlock vs ticket spinlock — MCS principle, O(1) cache invalidation on unlock.
2. Context rules for spinlocks — `irqsave` vs `bh` vs plain, and why.
3. RCU grace period mechanics — quiescent state, not a lock, deferred free.
4. `synchronize_rcu()` vs `call_rcu()` — synchronous vs async callback registration.
5. Priority inheritance in `rt_mutex` — essential for `PREEMPT_RT` driver compatibility.
6. `lockdep` for correctness validation — mention actual debug workflow.
7. RCU vs rwlock decision criteria — NVIDIA driver code uses both; know when to switch.
