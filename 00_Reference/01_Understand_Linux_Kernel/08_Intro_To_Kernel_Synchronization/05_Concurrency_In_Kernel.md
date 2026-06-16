# 05 — Concurrency in the Kernel

## 1. Sources of Concurrency in Linux

The kernel is inherently concurrent due to multiple independent sources:

```mermaid
mindmap
  root((Kernel Concurrency Sources))
    Multiprocessor SMP
      Multiple CPUs running simultaneously
      Each CPU has its own pipeline
    Kernel Preemption
      PREEMPT config
      Another process can preempt kernel code
    Hardware Interrupts
      Can fire at any time
      Preempts current CPU execution
    Softirqs and Tasklets
      Run after interrupt returns
    Sleeping and Rescheduling
      Code can block waiting for I/O
      Another process runs
    User Space Preemption
      Portability to RT kernels
```

---

## 2. On a Single CPU (Uniprocessor)

Even without SMP, concurrency exists:

```mermaid
sequenceDiagram
    participant Process as Process (kernel mode)
    participant IRQ as Hardware IRQ

    Process->>Process: global_var = global_var + 1  (step 1: read)
    IRQ->>IRQ: global_var = 0  (interrupt fires between steps!)
    Process->>Process: global_var + 1 = 1           (step 2: compute)
    Process->>Process: global_var = 1               (step 3: write — WRONG!)
    Note over Process: Lost IRQ update!
```

**Key insight:** Even on UP (uniprocessor), IRQs create concurrency.

---

## 3. On SMP (Symmetric Multiprocessing)

```
CPU 0                CPU 1
──────────────       ──────────────
kernel_func()        kernel_func()
  access list ─────────── access list
  (simultaneous read/write → corruption)
```

Linux is a **fully preemptive SMP-aware kernel**. Every data structure accessible from multiple CPUs needs protection.

---

## 4. Kernel Preemption

Since Linux 2.6, the kernel is **preemptible** (`CONFIG_PREEMPT`):
- A higher-priority task can preempt kernel code mid-execution
- This means kernel code must protect itself even on UP

```c
/* Without CONFIG_PREEMPT: kernel code was "safe" on UP (though IRQs still matter) */
/* With CONFIG_PREEMPT: kernel code CAN be preempted at any point */

static int counter;
counter++;  /* Might be preempted between load and store! */

/* Fix: use spinlock (disables preemption) or atomic_inc() */
```

---

## 5. What Data Needs Protection

| Data | Protection Needed |
|------|-----------------|
| Global kernel variables | Spinlock or atomic_t |
| Per-CPU variables (`DEFINE_PER_CPU`) | Usually none (by definition per-CPU) |
| Stack variables (local to function) | None (not shared) |
| Heap objects accessed only by one task | None |
| Heap objects shared across tasks/CPUs | Spinlock, mutex, RCU |
| Hardware registers (MMIO) | Usually none (serialized by bus) |

---

## 6. Summary: When to Protect

```mermaid
flowchart TD
    A[Is data shared between execution paths?] --> B{No}
    A --> C{Yes}
    B --> D[No protection needed]
    C --> E{Could one of those paths be an IRQ?}
    E -- Yes --> F[spin_lock_irqsave]
    E -- No --> G{Can paths sleep?}
    G -- Yes --> H[Mutex]
    G -- No --> I[Spinlock]
```

---

## 7. Related Concepts
- [02_Race_Conditions.md](./02_Race_Conditions.md) — What goes wrong
- [../09_Kernel_Synchronization_Methods/](../09_Kernel_Synchronization_Methods/) — All available synchronization primitives
- [../03_Process_Scheduling/05_Preemption.md](../03_Process_Scheduling/05_Preemption.md) — Kernel preemption
