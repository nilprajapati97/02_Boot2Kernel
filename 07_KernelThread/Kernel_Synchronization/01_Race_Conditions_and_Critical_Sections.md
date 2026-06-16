# 01 — Race Conditions and Critical Sections

> **Scope**: What races are, why they happen in the kernel, how critical sections protect shared data, and every source of concurrency in Linux.

---

## 1. What is a Race Condition?

A race condition occurs when two or more execution contexts access **shared data concurrently** and at least one of them **writes**, producing results that depend on the unpredictable order of execution.

```mermaid
sequenceDiagram
    participant CPU0 as CPU 0
    participant MEM as Shared Variable count=5
    participant CPU1 as CPU 1

    Note over CPU0,CPU1: Both want to do count++

    CPU0->>MEM: READ count (gets 5)
    CPU1->>MEM: READ count (gets 5)
    CPU0->>CPU0: Compute 5 + 1 = 6
    CPU1->>CPU1: Compute 5 + 1 = 6
    CPU0->>MEM: WRITE count = 6
    CPU1->>MEM: WRITE count = 6

    Note over MEM: count = 6 WRONG<br/>Expected: 7<br/>One increment LOST
```

---

## 2. Sources of Concurrency in the Linux Kernel

```mermaid
flowchart TD
    subgraph Sources["Sources of Concurrent Execution"]
        SMP["SMP - True Parallelism<br/>Multiple CPUs execute<br/>kernel code simultaneously"]
        PREEMPT["Preemption<br/>CONFIG_PREEMPT=y<br/>Kernel code preempted by scheduler<br/>Another task runs on SAME CPU"]
        IRQ["Hardware Interrupts<br/>Interrupt handler runs<br/>on SAME CPU preempting<br/>current kernel code"]
        SOFTIRQ["Softirqs and Tasklets<br/>Bottom halves run after<br/>interrupt on same or<br/>different CPU"]
        WORKQ["Workqueues<br/>Kernel threads that<br/>process deferred work"]
    end

    subgraph Danger["When does race happen?"]
        D1["Two CPUs access same struct"]
        D2["Process context vs IRQ on same CPU"]
        D3["Softirq runs on two CPUs simultaneously"]
        D4["Preempted task and new task share data"]
    end

    SMP --> D1
    IRQ --> D2
    SOFTIRQ --> D3
    PREEMPT --> D4

    style SMP fill:#e74c3c,stroke:#c0392b,color:#fff
    style PREEMPT fill:#f39c12,stroke:#e67e22,color:#fff
    style IRQ fill:#3498db,stroke:#2980b9,color:#fff
    style SOFTIRQ fill:#9b59b6,stroke:#8e44ad,color:#fff
    style WORKQ fill:#2ecc71,stroke:#27ae60,color:#fff
```

---

## 3. What is a Critical Section?

A critical section is a region of code that accesses shared resources and must **not** be executed by more than one thread of execution at a time.

```mermaid
flowchart TD
    subgraph Before["Without Protection"]
        B1["Thread A: lock = NONE"] --> B2["Read shared_list"]
        B3["Thread B: lock = NONE"] --> B4["Modify shared_list"]
        B2 --> CRASH["Corrupted data or crash"]
        B4 --> CRASH
    end

    subgraph After["With Critical Section"]
        A1["Thread A: spin_lock"] --> A2["READ shared_list<br/>CRITICAL SECTION"]
        A2 --> A3["spin_unlock"]
        A4["Thread B: spin_lock<br/>SPINS waiting"] --> A5["Modify shared_list<br/>CRITICAL SECTION"]
        A5 --> A6["spin_unlock"]
        A3 --> A4
    end

    style CRASH fill:#e74c3c,stroke:#c0392b,color:#fff
    style A2 fill:#2ecc71,stroke:#27ae60,color:#fff
    style A5 fill:#2ecc71,stroke:#27ae60,color:#fff
```

---

## 4. Classic Race Condition Examples in the Kernel

### 4.1 Linked List Race

```c
/* BROKEN: No locking around list manipulation */
struct my_device {
    struct list_head list;
    int data;
};

/* CPU 0: add to list */
void add_device(struct my_device *dev)
{
    list_add(&dev->list, &device_list);  /* NOT SAFE */
}

/* CPU 1: iterate and read */
void scan_devices(void)
{
    struct my_device *dev;
    list_for_each_entry(dev, &device_list, list) {
        process(dev->data);  /* may read half-updated pointers */
    }
}

/* FIXED: Protect with spinlock */
static DEFINE_SPINLOCK(dev_lock);

void add_device_safe(struct my_device *dev)
{
    spin_lock(&dev_lock);
    list_add(&dev->list, &device_list);
    spin_unlock(&dev_lock);
}

void scan_devices_safe(void)
{
    struct my_device *dev;
    spin_lock(&dev_lock);
    list_for_each_entry(dev, &device_list, list) {
        process(dev->data);
    }
    spin_unlock(&dev_lock);
}
```

### 4.2 Check-Then-Act Race (TOCTOU)

```c
/* BROKEN: Time-of-check-to-time-of-use race */
if (resource_available) {       /* CHECK */
    /* Another CPU sets resource_available = false HERE */
    use_resource();              /* ACT — resource no longer available! */
    resource_available = false;
}

/* FIXED: Atomic check-and-act */
spin_lock(&res_lock);
if (resource_available) {
    resource_available = false;  /* check + act is atomic now */
    spin_unlock(&res_lock);
    use_resource();
} else {
    spin_unlock(&res_lock);
}
```

---

## 5. Identifying Shared Data

```mermaid
flowchart TD
    subgraph Question["Ask These Questions"]
        Q1["Is this data global?<br/>YES = needs protection"]
        Q2["Is this data in a struct<br/>accessed by multiple threads?<br/>YES = needs protection"]
        Q3["Is this data passed to<br/>another context via pointer?<br/>YES = needs protection"]
        Q4["Is this a per-CPU variable?<br/>YES = safe IF preempt disabled"]
        Q5["Is this a local variable<br/>on the stack?<br/>YES = inherently safe"]
    end

    Q1 -->|Global| LOCK["Must use sync primitive"]
    Q2 -->|Shared struct| LOCK
    Q3 -->|Pointer shared| LOCK
    Q4 -->|per-CPU| PERCPU["Disable preemption"]
    Q5 -->|Stack local| SAFE["No protection needed"]

    style LOCK fill:#e74c3c,stroke:#c0392b,color:#fff
    style PERCPU fill:#f39c12,stroke:#e67e22,color:#fff
    style SAFE fill:#2ecc71,stroke:#27ae60,color:#fff
```

---

## 6. Critical Section Rules in the Linux Kernel

| Rule | Reason |
|------|--------|
| Keep critical sections SHORT | Reduces contention, latency |
| Never sleep while holding a spinlock | Spinlock disables preemption, sleeping = deadlock |
| Always use the SAME lock for the same data | Two different locks = no protection |
| Lock ordering must be consistent | Prevents deadlock: always lock A before B |
| Minimize nested locks | Each nesting level increases deadlock risk |
| Use the lightest primitive that works | atomic > spinlock > mutex > semaphore |

---

## 7. Concurrency Scenarios and Required Protection

```mermaid
flowchart TD
    subgraph Scenarios["Scenario Decision Tree"]
        S1{"Where is the<br/>shared data accessed?"}
        
        S1 -->|"Process context only<br/>Can sleep"| M["Use mutex"]
        S1 -->|"Process + softirq"| SBH["spin_lock_bh"]
        S1 -->|"Process + IRQ handler"| SIRQ["spin_lock_irqsave"]
        S1 -->|"IRQ + IRQ<br/>different IRQs"| SIRQ2["spin_lock_irqsave"]
        S1 -->|"Softirq + softirq<br/>same type"| SPIN["spin_lock<br/>same softirq serialized<br/>per-CPU"]
        S1 -->|"Softirq + softirq<br/>different types"| SPIN2["spin_lock"]
        S1 -->|"Mostly readers<br/>rare writers"| RCU_RW["RCU or rwlock"]
    end

    style M fill:#2ecc71,stroke:#27ae60,color:#fff
    style SBH fill:#3498db,stroke:#2980b9,color:#fff
    style SIRQ fill:#e74c3c,stroke:#c0392b,color:#fff
    style RCU_RW fill:#9b59b6,stroke:#8e44ad,color:#fff
```

---

## 8. Deep Q&A

### Q1: What is a data race vs a race condition?

**A:** A **data race** is a specific type: two threads access the same memory location concurrently, at least one writes, and there is no synchronization. A **race condition** is the broader concept — the behavior depends on timing. You can have a race condition without a data race (e.g., TOCTOU with atomics) and a data race that is benign (e.g., statistics counter).

### Q2: Can a race condition occur on a single-CPU system?

**A:** Yes, if `CONFIG_PREEMPT=y`. A kernel task reading shared data can be preempted, another task scheduled on the SAME CPU modifies the data, then the first task resumes with stale values. Also, interrupt handlers on the same CPU create races.

### Q3: Why is `i++` not safe in the kernel?

**A:** On most architectures, `i++` compiles to: `load i → increment → store i` (three separate operations). Between load and store, another CPU or interrupt can modify `i`. Use `atomic_inc()` or protect with a lock.

### Q4: What is the difference between mutual exclusion and serialization?

**A:** Mutual exclusion means only ONE thread in the critical section at a time. Serialization means enforcing a specific ORDER of operations. Locks provide mutual exclusion. Memory barriers and completion mechanisms provide serialization. Both are forms of synchronization.

---

[Next: 02 — Atomic Operations →](02_Atomic_Operations.md)
