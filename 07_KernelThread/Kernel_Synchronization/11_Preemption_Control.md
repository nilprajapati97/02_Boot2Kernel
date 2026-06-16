# 11 — Preemption Control

> **Scope**: preempt_disable/enable, preempt_count, CONFIG_PREEMPT variants, preemption and synchronization interactions, preemptible vs non-preemptible kernels, and PREEMPT_RT.

---

## 1. What is Kernel Preemption?

Kernel preemption means a higher-priority task can **interrupt** a currently running kernel-mode task and take over the CPU, even in the middle of kernel code.

```mermaid
flowchart TD
    subgraph NoPreempt["Non-Preemptible Kernel"]
        NP1["Task A in kernel mode"]
        NP2["Higher-priority Task B becomes runnable"]
        NP3["Task B waits until A returns to userspace<br/>or voluntarily calls schedule"]
        NP1 --> NP2 --> NP3
    end
    
    subgraph Preempt["Preemptible Kernel"]
        P1["Task A in kernel mode"]
        P2["Higher-priority Task B becomes runnable"]
        P3["Task A is preempted immediately<br/>Task B runs right now"]
        P1 --> P2 --> P3
    end
    
    style NP3 fill:#e74c3c,stroke:#c0392b,color:#fff
    style P3 fill:#2ecc71,stroke:#27ae60,color:#fff
```

---

## 2. CONFIG_PREEMPT Variants

```mermaid
flowchart TD
    subgraph Variants["Kernel Preemption Models"]
        NONE["CONFIG_PREEMPT_NONE<br/>No preemption in kernel<br/>Server workloads<br/>Best throughput"]
        
        VOLUNTARY["CONFIG_PREEMPT_VOLUNTARY<br/>Preemption at explicit check points<br/>might_sleep, cond_resched<br/>Desktop default"]
        
        FULL["CONFIG_PREEMPT<br/>Full preemption anywhere<br/>except when explicitly disabled<br/>Low latency"]
        
        RT["CONFIG_PREEMPT_RT<br/>Real-Time patch<br/>Even spinlocks become preemptible<br/>Deterministic latency"]
    end
    
    NONE -->|"More preemptible"| VOLUNTARY
    VOLUNTARY -->|"More preemptible"| FULL
    FULL -->|"Most preemptible"| RT

    style NONE fill:#3498db,stroke:#2980b9,color:#fff
    style VOLUNTARY fill:#f39c12,stroke:#e67e22,color:#fff
    style FULL fill:#e74c3c,stroke:#c0392b,color:#fff
    style RT fill:#9b59b6,stroke:#8e44ad,color:#fff
```

| Model | Preemption Points | Latency | Throughput | Use Case |
|-------|-------------------|---------|------------|----------|
| PREEMPT_NONE | Return to userspace only | High (ms) | Best | Servers, HPC |
| PREEMPT_VOLUNTARY | cond_resched + return | Medium | Good | Desktop |
| PREEMPT | Everywhere except atomic | Low (us) | Good | Embedded, audio |
| PREEMPT_RT | Even spinlock holders | Very low (us) | Lower | Industrial RT |

---

## 3. preempt_count — The Master Counter

```c
/* preempt_count is per-task, stored in thread_info */
/* It encodes MULTIPLE nesting counters in one integer: */

/*  Bits:
 *  [0:7]   = preemption count (preempt_disable nesting)
 *  [8:15]  = softirq count (in_softirq nesting)
 *  [16:19] = hardirq count (in_interrupt nesting)
 *  [20]    = NMI flag
 *  [21]    = PREEMPT_NEED_RESCHED
 */

/* Preemption is DISABLED when preempt_count != 0 
 * i.e., in any of: disabled, softirq, hardirq, NMI */
```

```mermaid
flowchart TD
    subgraph PreemptCount["preempt_count Fields"]
        PC["preempt_count 32-bit integer"]
        
        F1["Bits 0-7: Preemption nesting<br/>preempt_disable increments<br/>preempt_enable decrements"]
        F2["Bits 8-15: Softirq count<br/>local_bh_disable increments<br/>Nonzero = in softirq context"]
        F3["Bits 16-19: Hardirq count<br/>Set on IRQ entry<br/>Nonzero = in IRQ context"]
        F4["Bit 20: NMI<br/>Set during NMI handler"]
        
        PC --> F1
        PC --> F2
        PC --> F3
        PC --> F4
    end
    
    subgraph Check["Context Checks"]
        C1["in_interrupt = hardirq + softirq != 0"]
        C2["in_softirq = softirq bits != 0"]
        C3["in_irq = hardirq bits != 0"]
        C4["in_nmi = NMI bit set"]
        C5["preemptible = preempt_count == 0<br/>AND irqs enabled"]
    end
    
    style F1 fill:#2ecc71,stroke:#27ae60,color:#fff
    style F2 fill:#3498db,stroke:#2980b9,color:#fff
    style F3 fill:#e74c3c,stroke:#c0392b,color:#fff
    style F4 fill:#9b59b6,stroke:#8e44ad,color:#fff
```

---

## 4. preempt_disable / preempt_enable API

```c
#include <linux/preempt.h>

preempt_disable();    /* Increment preempt_count */
/* Critical section: task stays on THIS CPU */
preempt_enable();     /* Decrement; may reschedule if needed */

/* Nestable: */
preempt_disable();       /* count = 1 */
  preempt_disable();     /* count = 2 */
  preempt_enable();      /* count = 1, still disabled */
preempt_enable();        /* count = 0, preemption re-enabled */

/* No-reschedule variant (used inside lock implementations) */
preempt_enable_no_resched();  /* Decrement but don't check TIF_NEED_RESCHED */

/* Conditional reschedule for long loops */
cond_resched();  /* If preempt_count==0 and reschedule pending, yield CPU */
/* Use in long loops to keep system responsive */
```

---

## 5. When is Preemption Disabled?

```mermaid
flowchart TD
    subgraph Disabled["Preemption Automatically Disabled"]
        D1["spin_lock -- preempt_disable inside"]
        D2["IRQ handler -- hardirq count > 0"]
        D3["Softirq handler -- softirq count > 0"]
        D4["preempt_disable -- explicit"]
        D5["RCU read section -- on non-PREEMPT_RCU"]
    end
    
    subgraph Enabled["Preemption Remains Enabled"]
        E1["mutex_lock -- may sleep"]
        E2["Normal process context code"]
        E3["User space -- always preemptible"]
    end
    
    subgraph Check["Is Preemption Enabled?"]
        Q1["preempt_count == 0?"]
        Q2["irqs_disabled == false?"]
        Q3["Both yes = Preemptible"]
    end
    
    style D1 fill:#e74c3c,stroke:#c0392b,color:#fff
    style D2 fill:#e74c3c,stroke:#c0392b,color:#fff
    style E1 fill:#2ecc71,stroke:#27ae60,color:#fff
    style Q3 fill:#2ecc71,stroke:#27ae60,color:#fff
```

---

## 6. Preemption and Synchronization Interaction

```mermaid
sequenceDiagram
    participant T1 as Task A prio=10
    participant T2 as Task B prio=20
    participant SCHED as Scheduler

    T1->>T1: preempt_disable
    T1->>T1: Access per-CPU data
    
    Note over SCHED: Task B becomes runnable prio=20 > 10
    SCHED->>SCHED: Set TIF_NEED_RESCHED on Task A
    Note over T1: Cannot preempt, preempt_count > 0

    T1->>T1: Finish per-CPU access
    T1->>T1: preempt_enable
    Note over T1: preempt_enable checks<br/>TIF_NEED_RESCHED flag

    T1->>SCHED: Reschedule now
    SCHED->>T2: Switch to Task B
```

---

## 7. preempt_disable as a Synchronization Primitive

```c
/* preempt_disable creates a "soft" critical section:
 * - Task stays on current CPU
 * - Other tasks on THIS CPU cannot run
 * - But IRQs and softirqs CAN still fire!
 * - Other CPUs are NOT affected at all */

/* Use case: accessing per-CPU data safely */
preempt_disable();
struct my_data *p = this_cpu_ptr(&my_percpu);
p->counter++;
p->timestamp = ktime_get();
preempt_enable();

/* NOT sufficient for: */
/* - Protection against IRQ access (need local_irq_save) */
/* - SMP protection (need spinlock) */
/* - Protection against softirq (need local_bh_disable) */
```

---

## 8. PREEMPT_RT Impact on Synchronization

```c
/* Under PREEMPT_RT, many locking primitives change: */

/* spinlock_t → becomes a sleeping rt_mutex internally
 * Can be preempted while "spinning"
 * Supports priority inheritance */

/* raw_spinlock_t → remains a true spinlock
 * Only used in scheduler, timer, printk
 * Very few in the kernel (~30) */

/* local_irq_disable → only prevents IRQs, not preemption
 * Under PREEMPT_RT, IRQ handlers are threaded (run as tasks)
 * So disabling IRQs is less meaningful */

/* Implications: 
 * - spin_lock critical sections CAN sleep under RT
 * - Must use raw_spin_lock for truly non-preemptible sections
 * - All IRQ handlers are preemptible kernel threads */
```

```mermaid
flowchart TD
    subgraph Standard["Standard Kernel"]
        S_SL["spinlock_t<br/>True spinlock<br/>Preemption disabled"]
        S_ML["mutex<br/>Sleeping lock"]
        S_IRQ["IRQ handlers<br/>Run in interrupt context"]
    end
    
    subgraph RT["PREEMPT_RT Kernel"]
        R_SL["spinlock_t<br/>Becomes rt_mutex<br/>Can be preempted!"]
        R_RAW["raw_spinlock_t<br/>True spinlock<br/>Non-preemptible"]
        R_ML["mutex<br/>Sleeping lock unchanged"]
        R_IRQ["IRQ handlers<br/>Threaded kernel tasks<br/>Preemptible"]
    end
    
    style S_SL fill:#e74c3c,stroke:#c0392b,color:#fff
    style R_SL fill:#f39c12,stroke:#e67e22,color:#fff
    style R_RAW fill:#e74c3c,stroke:#c0392b,color:#fff
    style R_IRQ fill:#3498db,stroke:#2980b9,color:#fff
```

---

## 9. Deep Q&A

### Q1: Why doesn't preempt_disable protect against SMP races?

**A:** `preempt_disable()` only affects the LOCAL CPU — it prevents rescheduling of the current task. Other CPUs run independently and can access shared data concurrently. For SMP protection, you need actual locks (spinlock/mutex) or atomic operations. `preempt_disable()` is only sufficient for per-CPU data where each CPU accesses its own copy.

### Q2: What happens if you call schedule() with preemption disabled?

**A:** The kernel detects this and prints a BUG: "scheduling while atomic" with a stack trace. The `preempt_count` is non-zero, meaning you're in an atomic context (spinlock held, IRQ disabled, etc.). Sleeping here would mean the lock is never released (or IRQs never re-enabled), leading to deadlocks. This check is in `schedule()` → `__schedule()` → `schedule_debug()`.

### Q3: How does cond_resched() work and when should you use it?

**A:** `cond_resched()` checks if a reschedule is pending (TIF_NEED_RESCHED set) AND preemption is not disabled (preempt_count == 0). If both are true, it calls `schedule()`. Use it in long kernel loops that don't disable preemption: file system operations, memory reclaim, driver initialization. Without it, on a PREEMPT_NONE kernel, these loops would run for hundreds of milliseconds without letting higher-priority tasks run.

### Q4: Explain the preempt_enable → reschedule path.

**A:** `preempt_enable()` decrements `preempt_count`. If it reaches 0, it checks `TIF_NEED_RESCHED` flag (set by the scheduler when a higher-priority task became runnable during the critical section). If the flag is set, `preempt_enable()` calls `preempt_schedule()` → `__schedule()` to switch to the higher-priority task. This is how preemption is "deferred" — the reschedule happens at the exact moment it becomes safe.

---

[← Previous: 10 — Per-CPU Variables](10_Per_CPU_Variables.md) | [Next: 12 — Waitqueues →](12_Waitqueues.md)
