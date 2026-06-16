# 14 — Priority Inversion and RT Mutexes

> **Scope**: Priority inversion problem, Mars Pathfinder incident, priority inheritance protocol, priority ceiling, rt_mutex in Linux, PREEMPT_RT, and PI futexes.

---

## 1. What is Priority Inversion?

Priority inversion occurs when a **high-priority task** is blocked waiting for a **low-priority task** that holds a resource, while a **medium-priority task** runs instead of the low-priority task, effectively delaying the high-priority task indefinitely.

```mermaid
sequenceDiagram
    participant L as Low Priority Task
    participant M as Medium Priority Task
    participant H as High Priority Task
    participant LOCK as Mutex

    L->>LOCK: lock - acquired
    L->>L: In critical section...
    
    Note over H: High priority task wakes up
    H->>LOCK: lock - BLOCKED waiting for L
    Note over H: H must wait for L to unlock

    Note over M: Medium priority task wakes up
    M->>M: Running - preempts L
    Note over L: L is preempted by M<br/>Cannot run to release lock
    
    M->>M: Still running 100ms...
    Note over H: H is stuck waiting for L<br/>L is stuck waiting for M<br/>PRIORITY INVERSION<br/>H effectively runs at L priority
    
    M->>M: Finishes
    L->>L: Resumes critical section
    L->>LOCK: unlock
    H->>LOCK: lock - acquired, finally runs
```

---

## 2. The Mars Pathfinder Bug (1997)

```mermaid
flowchart TD
    subgraph Mars["Mars Pathfinder Priority Inversion"]
        T1["LOW: Meteorological task<br/>Held shared memory bus mutex"]
        T2["MEDIUM: Communications task<br/>Ran for long time, preempted LOW"]
        T3["HIGH: Information bus task<br/>Needed the mutex, BLOCKED"]
        T4["Watchdog timer expired<br/>System RESET"]
        T5["Spacecraft kept rebooting<br/>Fix: enable priority inheritance<br/>in VxWorks RTOS"]
        
        T1 --> T2 --> T3 --> T4 --> T5
    end
    
    style T4 fill:#e74c3c,stroke:#c0392b,color:#fff
    style T5 fill:#2ecc71,stroke:#27ae60,color:#fff
```

---

## 3. Solutions to Priority Inversion

```mermaid
flowchart TD
    subgraph Solutions["Priority Inversion Solutions"]
        PI["Priority Inheritance<br/>Low-priority task INHERITS<br/>high-priority waiter priority<br/>Runs at high priority to release lock fast"]
        
        PC["Priority Ceiling<br/>Lock has a ceiling priority<br/>Any task holding it runs at ceiling<br/>Prevents medium from preempting"]
        
        LF["Lock-Free Design<br/>Use RCU, atomics, per-CPU<br/>Avoid the problem entirely"]
    end
    
    subgraph Linux["Linux Uses"]
        L1["rt_mutex: Priority Inheritance"]
        L2["PI futex: Userspace priority inheritance"]
        L3["PREEMPT_RT: all spinlocks get PI"]
    end
    
    PI --> L1
    PI --> L2
    PI --> L3

    style PI fill:#2ecc71,stroke:#27ae60,color:#fff
    style PC fill:#3498db,stroke:#2980b9,color:#fff
    style LF fill:#f39c12,stroke:#e67e22,color:#fff
```

---

## 4. Priority Inheritance in Action

```mermaid
sequenceDiagram
    participant L as Low Prio=10
    participant M as Medium Prio=50
    participant H as High Prio=90
    participant LOCK as rt_mutex

    L->>LOCK: rt_mutex_lock - acquired
    L->>L: In critical section prio=10

    H->>LOCK: rt_mutex_lock - BLOCKED
    Note over L: Priority Inheritance kicks in<br/>L priority boosted 10 to 90

    M->>M: Wakes up prio=50
    Note over M: Cannot preempt L<br/>L running at prio=90 now
    
    L->>L: Critical section at prio=90
    L->>LOCK: rt_mutex_unlock
    Note over L: Priority restored to 10
    
    H->>LOCK: Acquires lock, runs at prio=90
    Note over M: M runs after H finishes
```

---

## 5. rt_mutex API

```c
#include <linux/rtmutex.h>

struct rt_mutex my_rt_mutex;
rt_mutex_init(&my_rt_mutex);

/* Lock — supports priority inheritance */
void rt_mutex_lock(struct rt_mutex *lock);

/* Interruptible */
int rt_mutex_lock_interruptible(struct rt_mutex *lock);

/* Killable */
int rt_mutex_lock_killable(struct rt_mutex *lock);

/* Try lock */
int rt_mutex_trylock(struct rt_mutex *lock);

/* Unlock */
void rt_mutex_unlock(struct rt_mutex *lock);
```

---

## 6. rt_mutex Internals

```c
struct rt_mutex_base {
    raw_spinlock_t wait_lock;  /* Protects waiter tree */
    struct rb_root_cached waiters; /* RB-tree of waiters, 
                                      sorted by priority */
    struct task_struct *owner;     /* Current owner */
};

/* PI Chain:
 * When H blocks on rt_mutex held by L:
 * 1. H is added to lock's waiter tree
 * 2. L's priority is boosted to H's priority
 * 3. If L is blocked on ANOTHER rt_mutex,
 *    the boost propagates through the chain
 * 4. PI chain depth is limited (prevent infinite loops) */
```

```mermaid
flowchart TD
    subgraph PIChain["Priority Inheritance Chain"]
        H["Task H prio=90<br/>wants Lock 1"]
        L1["Lock 1<br/>held by Task M"]
        M["Task M prio=50<br/>boosted to 90<br/>wants Lock 2"]
        L2["Lock 2<br/>held by Task L"]
        L["Task L prio=10<br/>boosted to 90"]
        
        H -->|"blocked on"| L1
        L1 -->|"owned by"| M
        M -->|"blocked on"| L2
        L2 -->|"owned by"| L
    end
    
    subgraph Result["Result"]
        R1["L runs at prio=90"]
        R2["L unlocks Lock 2"]
        R3["M runs at prio=90, unlocks Lock 1"]
        R4["H runs"]
        R1 --> R2 --> R3 --> R4
    end
    
    style H fill:#2ecc71,stroke:#27ae60,color:#fff
    style M fill:#f39c12,stroke:#e67e22,color:#fff
    style L fill:#e74c3c,stroke:#c0392b,color:#fff
```

---

## 7. PREEMPT_RT and Spinlock Conversion

```c
/* Under PREEMPT_RT:
 * spinlock_t is internally converted to rt_mutex
 * This means EVERY spinlock gets priority inheritance!
 *
 * Benefits:
 * - No priority inversion with spinlocks
 * - Spinlock holders can be preempted (deterministic latency)
 * - All IRQ handlers run as kernel threads (preemptible)
 *
 * Only raw_spinlock_t remains as a true spinning lock.
 */

/* Standard kernel:
 *   spinlock_t     → actual spinlock (busy-wait)
 *   mutex          → sleeping lock with PI
 *   rt_mutex       → sleeping lock with PI
 *
 * PREEMPT_RT kernel:
 *   spinlock_t     → rt_mutex internally (sleeping, PI)
 *   mutex          → rt_mutex internally (sleeping, PI)
 *   raw_spinlock_t → actual spinlock (busy-wait)
 */
```

---

## 8. PI Futex — Userspace Priority Inheritance

```c
/* Linux extends priority inheritance to userspace via PI futexes */

/* Userspace: pthread_mutex with PTHREAD_PRIO_INHERIT */
#include <pthread.h>

pthread_mutex_t mutex;
pthread_mutexattr_t attr;

pthread_mutexattr_init(&attr);
pthread_mutexattr_setprotocol(&attr, PTHREAD_PRIO_INHERIT);
pthread_mutex_init(&mutex, &attr);

/* Now pthread_mutex_lock uses FUTEX_LOCK_PI syscall
 * Kernel creates an rt_mutex shadow for this userspace mutex
 * Priority inheritance works across user/kernel boundary */
```

```mermaid
flowchart TD
    subgraph UserKernel["PI Futex: User-Kernel Bridge"]
        subgraph User["Userspace"]
            U1["pthread_mutex_lock<br/>with PRIO_INHERIT"]
            U2["Contended: syscall<br/>FUTEX_LOCK_PI"]
        end
        
        subgraph Kernel["Kernel"]
            K1["Create rt_mutex proxy"]
            K2["Apply priority inheritance"]
            K3["Boost owner task priority"]
            K4["Wake and transfer on unlock"]
        end
        
        U1 --> U2 --> K1 --> K2 --> K3
    end
    
    style K2 fill:#2ecc71,stroke:#27ae60,color:#fff
    style K3 fill:#2ecc71,stroke:#27ae60,color:#fff
```

---

## 9. When to Use rt_mutex vs mutex

```mermaid
flowchart TD
    Q1{"Real-time requirements?<br/>Strict priority scheduling?"}
    Q1 -- No --> MUTEX["Use regular mutex<br/>Simpler, faster for non-RT"]
    Q1 -- Yes --> Q2{"PREEMPT_RT kernel?"}
    Q2 -- Yes --> SPIN["spinlock_t already has PI<br/>Use normally"]
    Q2 -- No --> Q3{"Need PI for specific lock?"}
    Q3 -- Yes --> RT["Use rt_mutex<br/>Explicit PI support"]
    Q3 -- No --> MUTEX
    
    style MUTEX fill:#3498db,stroke:#2980b9,color:#fff
    style RT fill:#2ecc71,stroke:#27ae60,color:#fff
    style SPIN fill:#f39c12,stroke:#e67e22,color:#fff
```

---

## 10. Deep Q&A

### Q1: Why doesn't regular mutex have priority inheritance?

**A:** Regular `struct mutex` is designed for maximum throughput with features like adaptive spinning and fast-path atomic acquisition. PI adds overhead: maintaining a waiter tree sorted by priority, propagating priority changes through chains, and restoring priority on unlock. For most kernel code, deadlines are not strict enough to justify this cost. `rt_mutex` exists for the cases that need it.

### Q2: What is the priority ceiling protocol and why doesn't Linux use it?

**A:** Priority ceiling assigns each lock a ceiling priority (the highest priority of any task that might use it). Any task holding the lock runs at ceiling priority. This prevents priority inversion and deadlock. Linux doesn't use it because: (1) determining the ceiling requires knowing ALL possible users at design time — impractical for a general-purpose OS, (2) it boosts priority even when no high-priority waiter exists — wasteful.

### Q3: Can priority inheritance chains cause problems?

**A:** Yes. PI chains can be arbitrarily deep: Task A→Lock1→Task B→Lock2→Task C→Lock3→... Propagating priority through a long chain is expensive and adds latency to the lock operation. Linux limits PI chain depth (`MAX_LOCK_DEPTH = 48`). If exceeded, the kernel prints a warning and stops propagating. In practice, chains longer than 3-4 are extremely rare and indicate a design problem.

### Q4: How does the kernel handle priority de-boosting on unlock?

**A:** When `rt_mutex_unlock()` is called: (1) Remove the highest-priority waiter from the tree. (2) Wake that waiter. (3) Recalculate the owner's effective priority: it's the maximum of the task's base priority and the highest-priority waiter on ANY OTHER rt_mutex the task holds. (4) If the task holds no more contested rt_mutexes, restore to base priority. This is called "de-boosting."

---

[← Previous: 13 — Lockdep Debugging](13_Lockdep_Debugging.md) | [Next: 15 — Lock Ordering and Deadlock →](15_Lock_Ordering_Deadlock.md)
