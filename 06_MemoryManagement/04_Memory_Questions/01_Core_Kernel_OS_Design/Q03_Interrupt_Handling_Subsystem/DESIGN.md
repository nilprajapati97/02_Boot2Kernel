# Q03 — Design a High-Performance Interrupt Handling Subsystem

---

## 1. Problem Statement

Interrupt handling is the kernel's lowest-latency contract with hardware. A poorly designed interrupt subsystem introduces:
- Excessive interrupt latency (time from IRQ line assertion to handler execution)
- Long periods with interrupts disabled (missed events, cascading latency)
- Unbounded work in hardirq context (blocking system-wide preemption)
- Interrupt coalescing that reduces throughput OR increases latency (tuning conflict)

Design an interrupt handling subsystem that achieves < 1 µs hardirq-to-handler latency, properly defers non-critical work, supports MSI/MSI-X for multi-queue devices (GPUs, NICs), and integrates with the Linux IRQ domain/chip abstraction.

---

## 2. Requirements

### 2.1 Functional Requirements
- Handle hardware interrupts from PCI MSI-X vectors (per-queue, per-GPU-engine interrupts).
- Support threaded IRQ handlers (`IRQF_THREAD`) for long-running work.
- Implement top-half (hardirq) / bottom-half (softirq/tasklet/workqueue) split.
- CPU affinity assignment per interrupt (IRQ balancing across cores).
- Interrupt coalescing configuration per device (NAPI-style for NICs, fence interrupt batching for GPUs).
- Support `IRQ_WAKE_THREAD` for real-time (`CONFIG_PREEMPT_RT`) compatibility.

### 2.2 Non-Functional Requirements
- Hardirq handler duration: < 500 ns (measure at P99.9).
- IRQ-to-wakeup latency for threaded IRQ: < 5 µs.
- Zero spurious interrupt handling overhead.
- Scalable to 256 MSI-X vectors per device, 1024 interrupts system-wide.

---

## 3. Constraints & Assumptions

- x86-64 with APIC (Advanced Programmable Interrupt Controller) + IO-APIC + MSI-X.
- PCIe device (GPU or NIC) with MSI-X table in BAR0.
- `CONFIG_GENERIC_IRQ_CHIP`, `CONFIG_IRQ_DOMAIN` enabled.
- `CONFIG_PREEMPT_RT` considered for GPU driver compatibility.

---

## 4. Architecture Overview

```
  GPU Hardware (MSI-X)
  ┌──────────────────────────────────────────────────────────────┐
  │  Engine 0 IRQ ──► MSI-X Vector 0 → CPU 0 LAPIC             │
  │  Engine 1 IRQ ──► MSI-X Vector 1 → CPU 1 LAPIC             │
  │  ...                                                         │
  │  Engine N IRQ ──► MSI-X Vector N → CPU N LAPIC              │
  └──────────────────────────────────────────────────────────────┘
                          │
                          ▼
  ┌──────────────────────────────────────────────────────────────┐
  │                 IRQ Domain (PCI MSI-X)                       │
  │  irq_domain_add_linear() → maps hwirq → Linux virq           │
  │  irq_chip: mask/unmask/ack/eoi ops → MSI-X table write      │
  └──────────────────────────────────────────────────────────────┘
                          │
                          ▼
  ┌──────────────────────────────────────────────────────────────┐
  │              Interrupt Descriptor (struct irq_desc)          │
  │  [virq 32]  handler_data, action chain, irq_chip ptr        │
  │  [virq 33]  ...                                              │
  └──────────────────────────────────────────────────────────────┘
                          │
                ┌─────────┴──────────┐
                ▼                    ▼
  ┌─────────────────────┐  ┌─────────────────────────────────┐
  │   Hardirq Handler   │  │     Threaded IRQ Handler        │
  │   (top half)        │  │     (irq_thread kernel thread)  │
  │   < 500 ns          │  │     runs with irqs enabled      │
  │   Ack HW, wake thread│ │     does actual work            │
  └─────────────────────┘  └─────────────────────────────────┘
                │
     ┌──────────┴──────────┐
     ▼                     ▼
  Softirq              Workqueue
  (NET_RX, TASKLET)    (slow path, can sleep)
```

---

## 5. Core Data Structures

### 5.1 Interrupt Descriptor
```c
struct irq_desc {
    struct irq_data        irq_data;    /* per-irq chip/domain data */
    struct irqaction      *action;      /* handler chain */
    unsigned int           status_use_accessors; /* IRQ_DISABLED, etc. */
    raw_spinlock_t         lock;        /* protects action list */
    irq_flow_handler_t     handle_irq;  /* flow handler: edge/level */
    struct irq_affinity_notify *affinity_notify;
    /* stats */
    unsigned int           irqs_unhandled;
    unsigned long          last_unhandled;
    atomic_t               threads_active;
    wait_queue_head_t      wait_for_threads;
};
```

### 5.2 IRQ Action (handler registration)
```c
struct irqaction {
    irq_handler_t     handler;        /* hardirq handler function */
    void             *dev_id;         /* cookie passed to handler */
    struct irqaction *next;           /* shared IRQ chain */
    irq_handler_t     thread_fn;      /* threaded handler, if any */
    struct task_struct *thread;       /* irq_thread kernel thread */
    unsigned long     thread_flags;   /* IRQTF_RUNTHREAD, etc. */
    unsigned int      irq;
    unsigned int      flags;          /* IRQF_SHARED, IRQF_THREAD, etc. */
    char              name[32];
};
```

### 5.3 IRQ Chip (hardware abstraction)
```c
struct irq_chip {
    const char *name;
    void (*irq_mask)(struct irq_data *data);
    void (*irq_unmask)(struct irq_data *data);
    void (*irq_ack)(struct irq_data *data);
    void (*irq_eoi)(struct irq_data *data);      /* end-of-interrupt */
    int  (*irq_set_affinity)(struct irq_data *data,
                              const struct cpumask *dest, bool force);
    int  (*irq_set_type)(struct irq_data *data, unsigned int flow_type);
    /* MSI-X specific */
    void (*irq_compose_msi_msg)(struct irq_data *data, struct msi_msg *msg);
    void (*irq_write_msi_msg)(struct irq_data *data, struct msi_msg *msg);
};
```

### 5.4 Interrupt Coalescing Descriptor (custom, for GPU)
```c
struct gpu_irq_coalesce {
    u32     max_count;         /* max interrupts before forced delivery */
    u32     timeout_us;        /* max delay before interrupt fires */
    atomic_t pending_count;   /* current pending interrupt count */
    ktime_t  first_pending_ts; /* timestamp of first pending event */
    struct hrtimer coalesce_timer;  /* fires if timeout_us expires */
};
```

---

## 6. Key Algorithms & Design Decisions

### 6.1 Top-Half / Bottom-Half Split

The golden rule: **hardirq handler must be fast**. Only three things belong in the top half:
1. Acknowledge the interrupt at the hardware level (clear IRQ line, write MSI-X EOI).
2. Read the minimum state needed to determine what happened (e.g., which GPU engine fired).
3. Schedule bottom-half work (`__raise_softirq_irqoff(NET_RX_SOFTIRQ)` or `wake_up_process(thread)`).

Everything else — DMA buffer processing, fence signaling, command queue refill — goes to the bottom half.

### 6.2 Softirq vs Tasklet vs Workqueue

| Mechanism | Runs where | Can sleep | Priority | Use case |
|---|---|---|---|---|
| `SOFTIRQ` | Hardirq context (local CPU) | No | Highest | NET_RX, timer, block |
| `TASKLET` | Softirq context | No | Medium | Single-CPU serialized callbacks |
| `WORKQUEUE` | Kernel thread | Yes | Configurable | DMA completion, user notification |
| Threaded IRQ | Dedicated `irq_thread` | Yes | `SCHED_FIFO` | RT-compatible drivers |

**For GPU completion interrupts:** Use **threaded IRQ** (`IRQF_THREAD`). The hardirq reads the engine status register, the thread signals the DRM fence. This is RT-safe and eliminates `dma_fence_signal()` from hardirq context (which can indirectly call sleeping code via fence callbacks).

### 6.3 MSI-X Vector Allocation and Per-CPU Assignment

```c
/* Allocate N MSI-X vectors */
nvec = pci_alloc_irq_vectors(pdev, 1, max_engines,
                              PCI_IRQ_MSI | PCI_IRQ_MSIX);

for (i = 0; i < nvec; i++) {
    virq = pci_irq_vector(pdev, i);
    /* Pin each vector to a specific CPU for cache locality */
    irq_set_affinity_hint(virq, cpumask_of(cpu_for_engine[i]));

    request_threaded_irq(virq, gpu_hardirq_handler,
                         gpu_thread_handler,
                         IRQF_NO_SUSPEND, "gpu_eng%d", &eng[i]);
}
```

**Why per-CPU pinning matters:** When an MSI-X interrupt fires, the handler runs on the target CPU. If the GPU engine's completion ring buffer is in that CPU's L3 cache (because the submission thread also runs there), the handler sees a cache-hot ring buffer — reducing latency.

### 6.4 IRQ Affinity and NUMA Topology

For NUMA systems:
1. Find the NUMA node of the GPU's PCIe root: `dev_to_node(&pdev->dev)`.
2. Get the CPUs on that node: `cpumask_of_node(node)`.
3. Spread MSI-X vectors across those CPUs: use `irq_calc_affinity_vectors()`.
4. For each engine, assign IRQ affinity to the CPU that runs the engine's submission thread.

### 6.5 Interrupt Coalescing (GPU Fence Batching)

For high-frequency GPU workloads (thousands of small kernels/second):
- Each kernel completion fires an interrupt → excessive interrupt rate → interrupt storm.
- **Solution:** Coalesce completions: hardware holds IRQ for up to `timeout_us` OR until `max_count` completions.
- Software side: hrtimer fires if hardware coalescing expires → force-poll completion ring.
- Trade-off: latency ↑ by `timeout_us`, but interrupt rate ↓ by factor of `max_count`.

```c
static enum hrtimer_restart coalesce_hrtimer(struct hrtimer *timer)
{
    struct gpu_irq_coalesce *c = container_of(timer, ...);
    if (atomic_read(&c->pending_count) > 0)
        tasklet_schedule(&gpu->completion_tasklet);
    return HRTIMER_NORESTART;
}
```

### 6.6 Spurious Interrupt Detection

```c
irqreturn_t gpu_hardirq_handler(int irq, void *dev)
{
    u32 status = readl(gpu->regs + IRQ_STATUS);
    if (!status)
        return IRQ_NONE;    /* spurious — do not count as handled */

    writel(status, gpu->regs + IRQ_ACK);   /* clear at source */
    gpu->pending_engines |= status;
    return IRQ_WAKE_THREAD;               /* wake threaded handler */
}
```

After `IRQ_NONE` returned `irq_desc->irqs_unhandled` times, the kernel auto-disables the IRQ (`IRQ_DISABLED`) to protect the system from interrupt storms.

---

## 7. Trade-off Analysis

| Decision | Chosen | Alternative | Reason |
|---|---|---|---|
| Bottom-half mechanism | Threaded IRQ | Tasklet | RT-safe, can sleep, better debug (named thread) |
| MSI-X vs MSI vs INTx | MSI-X | MSI | MSI-X: per-vector CPU targeting; MSI: shared vector |
| IRQ coalescing | hrtimer + count threshold | Pure polling | Coalescing maintains low overhead at high frequency |
| CPU pinning | Per-engine CPU pin | IRQ balancing daemon | Pin avoids migration; irqbalance is too slow for GPU |
| EOI placement | After ring buffer read | Before ring buffer read | Before risks re-entry; after adds ~50 ns latency |

---

## 8. Real Linux Kernel References

| Component | Source | Symbol |
|---|---|---|
| IRQ descriptor | `include/linux/irqdesc.h` | `struct irq_desc` |
| IRQ chip | `include/linux/irq.h` | `struct irq_chip` |
| Threaded IRQ | `kernel/irq/manage.c` | `request_threaded_irq()`, `irq_thread()` |
| MSI-X alloc | `drivers/pci/msi/msi.c` | `pci_alloc_irq_vectors()` |
| Softirq | `kernel/softirq.c` | `raise_softirq()`, `__do_softirq()` |
| IRQ affinity | `kernel/irq/affinity.c` | `irq_set_affinity()`, `irq_calc_affinity_vectors()` |
| IRQ domain | `kernel/irq/irqdomain.c` | `irq_domain_add_linear()` |
| DRM GPU IRQ | `drivers/gpu/drm/scheduler/sched_main.c` | `drm_sched_fault()` |
| APIC | `arch/x86/kernel/apic/apic.c` | `apic_send_IPI()`, `apic_eoi()` |

---

## 9. Failure Modes & Debug Strategies

### 9.1 Interrupt Storm
**Symptom:** `top` shows `%si` (softirq) at 100%; `cat /proc/interrupts` shows one IRQ line incrementing millions/sec.
**Fix:** Verify IRQ ACK is written to hardware before returning from hardirq. Check for level-triggered interrupt where condition is never cleared.
```bash
watch -n1 "cat /proc/interrupts | grep -i gpu"
```

### 9.2 IRQ Stuck / Not Firing
**Symptom:** GPU appears idle; fences never signal.
**Debug:**
```bash
cat /proc/irq/<virq>/smp_affinity_list   # verify CPU pinning
cat /sys/bus/pci/devices/<bdf>/msi_irqs  # list MSI-X vectors
# Check if IRQ is masked:
cat /proc/irq/<virq>/spurious
```

### 9.3 Threaded IRQ Latency Spike
**Symptom:** `irq_thread` is runnable but not scheduled for > 10 ms.
**Debug:**
```bash
# irq_threads use SCHED_FIFO priority 50 by default
chrt -p $(pgrep irq/32-gpu_eng0)
# If system is overloaded, check for RT throttling:
cat /proc/sys/kernel/sched_rt_throttle_us
```

### 9.4 Debug Tools
```bash
# IRQ latency histogram
sudo bpftrace -e 'tracepoint:irq:irq_handler_entry { @ts[args->irq] = nsecs; }
                  tracepoint:irq:irq_handler_exit  { @lat = hist(nsecs - @ts[args->irq]); }'

# ftrace hardirq duration
echo function_graph > /sys/kernel/debug/tracing/current_tracer
echo gpu_hardirq_handler > /sys/kernel/debug/tracing/set_ftrace_filter
cat /sys/kernel/debug/tracing/trace_pipe
```

---

## 10. Performance Considerations

- **Cache-line alignment for `irq_desc`:** `irq_desc` is allocated with `SLAB_HWCACHE_ALIGN` to prevent false sharing between adjacent interrupt descriptors.
- **`local_irq_disable()` duration:** Every `local_irq_disable()/enable()` pair defines a latency bubble. Linux `CONFIG_PROVE_LOCKING` + `irqsoff` tracer measures worst-case duration.
- **APIC EOI:** Writing EOI to LAPIC (`apic->eoi()`) releases the interrupt controller to accept new interrupts at the same priority. Delay here causes missed interrupts.
- **Softirq vs process context:** `ksoftirqd` runs softirqs if they take > 2 ms or are re-raised too many times, preventing softirq starvation of processes.
- **Per-vector statistics:** `/proc/interrupts` is O(NR_CPUS × NR_IRQS) — for 512 CPUs × 256 vectors = 128k cells. Aggregate with `awk` instead of reading raw.

---

## 11. Interview Answer Strategy (NVIDIA 10-yr Level)

**What they want to hear:**
1. Clear hardirq/softirq/threaded split with reasoning — not just "use workqueue."
2. MSI-X vector per engine with per-CPU affinity pinning for cache locality.
3. `IRQF_THREAD` + `IRQ_WAKE_THREAD` interaction — and why it matters for `PREEMPT_RT`.
4. Interrupt coalescing trade-off: you cannot eliminate latency AND reduce interrupt rate — be explicit about the tuning knob.
5. Spurious interrupt handling — `IRQ_NONE` return and auto-disable mechanism.
6. Real kernel tools: `irqtop`, `bpftrace irq tracepoints`, `irqsoff tracer`.
7. For NVIDIA specifically: GPU engines have independent interrupt vectors — you'll use MSI-X and thread-per-engine for parallelism.
