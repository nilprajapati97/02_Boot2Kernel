# Design: Architect Level — CFS Scheduler + eBPF + vmcore + Shrinker
## Level 09 | End-to-End Deep Design from Scratch

---

## 1. Architect-Level Thinking

At Architect level, the driver is not just code — it's a **system component** integrated with:
- Kernel memory management (shrinker)
- Power and lifecycle events (reboot notifier)
- Kernel crash analysis (vmcore)
- Live kernel tracing (eBPF / tracepoints)
- Scheduler subsystem (CFS vruntime, priorities)

---

## 2. CFS Scheduler Deep Internals

### What is CFS?

**Completely Fair Scheduler** — Linux's default scheduler. Goal: give every task a "fair" share of CPU, proportional to priority.

### Virtual Runtime (vruntime)

$$v_{runtime} = \frac{exec\_time_{ns}}{weight}$$

where $weight$ is derived from nice value:
- nice=0  → weight=1024
- nice=-20 → weight=88761 (highest)
- nice=+19 → weight=15 (lowest)

### CFS Red-Black Tree

All runnable tasks stored in a **red-black tree** ordered by `vruntime`. CFS always picks the **leftmost node** (lowest vruntime = least recently run).

```
vruntime:  100  200  300  400  500
           [A]  [B]  [C]  [D]  [E]
            ↑
         picked next (leftmost)
```

### Implication for Driver Design

When `wake_up_interruptible()` is called:
1. Task moves from TASK_INTERRUPTIBLE wait queue → runqueue
2. Its `vruntime` is set to `min_vruntime` of runqueue (fair restart)
3. If woken task has lower vruntime than current → **preemption check**
4. If preemption: current task is descheduled, woken task runs

```
Driver IRQ fires → wake_up() → reader task added to runqueue
                               │
                               ▼
        reader.vruntime << current.vruntime?
                   YES → preempt → reader runs NOW (~5-10 µs)
                   NO  → reader queued → runs later (~50-200 µs)
```

### `cond_resched()` — Voluntary Preemption

In long kernel loops:
```c
for (i = 0; i < 1000000; i++) {
    process_item(i);
    cond_resched();   /* if need_resched flag set, yield to scheduler */
}
```

Without `cond_resched()`: loop holds CPU for entire duration. Other tasks (including RT) may be delayed on non-preemptible kernels.

---

## 3. eBPF — Live Kernel Tracing

### What is eBPF?

eBPF (extended Berkeley Packet Filter) allows verified programs to run in the kernel at:
- **Tracepoints** — static instrumentation points
- **kprobes** — dynamic function entry/return
- **perf events** — hardware counter events

### Attaching eBPF to Driver Functions

```bash
# bpftrace: trace every call to arch_read()
bpftrace -e '
kprobe:arch_read {
    printf("read: count=%d pid=%d comm=%s\n",
           arg2, pid, comm);
}'

# Trace tracepoint (trace_printk output)
bpftrace -e '
tracepoint:ftrace:kernel_stack {
    printf("%s\n", kstack);
}'
```

### `trace_printk` vs Tracepoints

```c
/* trace_printk — simple, intercepted by ftrace ring buffer */
trace_printk("event: len=%zu\n", len);

/* Proper tracepoint — recommended for production */
/* Defined in trace/events/my_driver.h */
DEFINE_EVENT(template, my_event,
    TP_PROTO(int len, pid_t pid),
    TP_ARGS(len, pid));
```

### BCC (BPF Compiler Collection) Example

```python
from bcc import BPF

prog = """
int kprobe__arch_read(struct pt_regs *ctx, struct file *f,
                       char *buf, size_t count) {
    bpf_trace_printk("arch_read: count=%lu\\n", count);
    return 0;
}
"""
b = BPF(text=prog)
b.trace_print()
```

---

## 4. vmcore — Crash Dump Analysis

### What Happens on Kernel Panic

```
Kernel panic
      │
      ▼
kdump: save physical memory snapshot → vmcore file
      │
      ▼
crash tool: analyze vmcore offline
```

### Making Driver State Analyzable

```c
struct arch_dev_state {
    u32     magic;      /* 0xA1B2C3D4 — use as anchor in vmcore */
    u64     bytes_rx;
    u32     irq_count;
    pid_t   last_reader_pid;
    char    last_reader_comm[TASK_COMM_LEN];
};
```

### Analysis with `crash` Tool

```bash
# Load vmcore
crash /usr/lib/debug/boot/vmlinux /var/crash/vmcore

# Find structure by magic value
crash> search 0xA1B2C3D4

# Once address found (e.g. 0xffff888001234560):
crash> struct arch_dev_state 0xffff888001234560
arch_dev_state {
  magic = 0xa1b2c3d4,
  bytes_rx = 1048576,
  irq_count = 1024,
  last_reader_pid = 1234,
  last_reader_comm = "cat",
}

# Examine kfifo at crash time
crash> struct kfifo 0xffff888001234600
```

### GDB with vmcore

```bash
gdb /usr/lib/debug/vmlinux /var/crash/vmcore

(gdb) x/4wx &g_dev->state.magic
(gdb) p g_dev->state.total_bytes_rx
```

---

## 5. Memory Shrinker

### Why Shrinker?

Driver caches pages for performance. Under memory pressure (OOM killer about to run), driver must release them voluntarily.

```
System RAM pressure high
      │
      ▼
mm/vmscan.c: calls all registered shrinkers
      │
      ▼
arch_shrinker_count() → returns number of freeable pages
arch_shrinker_scan()  → frees up to sc->nr_to_scan pages
```

### Registration

```c
d->pool.shrinker.count_objects = arch_shrinker_count;
d->pool.shrinker.scan_objects  = arch_shrinker_scan;
d->pool.shrinker.seeks         = DEFAULT_SEEKS;  /* relative cost to free */
register_shrinker(&d->pool.shrinker);
```

`DEFAULT_SEEKS = 2` — standard scanning cost. Higher = kernel less likely to ask us to shrink.

---

## 6. Reboot Notifier

```c
d->reboot_nb.notifier_call = arch_reboot_notify;
d->reboot_nb.priority      = 0;  /* higher priority = called first */
register_reboot_notifier(&d->reboot_nb);

static int arch_reboot_notify(struct notifier_block *nb,
                               unsigned long action, void *data) {
    if (action == SYS_RESTART || action == SYS_HALT) {
        /* Flush pending hardware operations */
        /* Wait for DMA to complete */
        /* Disable HW interrupts */
    }
    return NOTIFY_OK;
}
```

Without reboot notifier: driver might leave HW in mid-DMA state during reboot → hardware corruption or next boot failure.

---

## 7. CPU Hotplug Awareness

On systems where CPUs can be hot-removed (servers, ARM big.LITTLE):

```c
/* Register per-CPU setup/teardown callbacks */
ret = cpuhp_setup_state(CPUHP_AP_ONLINE_DYN,
    "arch_drv:online",
    arch_cpu_online,   /* called when CPU comes online */
    arch_cpu_offline   /* called when CPU goes offline */
);

static int arch_cpu_online(unsigned int cpu) {
    /* Initialize per-CPU data structures */
    per_cpu(cpu_stats, cpu).enabled = true;
    return 0;
}

static int arch_cpu_offline(unsigned int cpu) {
    /* Migrate work from going-offline CPU */
    per_cpu(cpu_stats, cpu).enabled = false;
    return 0;
}
```

---

## 8. CFS Priority Impact on Driver

| Reader priority | Wake latency | Use case |
|----------------|-------------|---------|
| RT (SCHED_FIFO) | < 10 µs | Real-time sensor data |
| Normal nice=-10 | 10-50 µs | High-priority service |
| Normal nice=0 | 50-200 µs | Normal user app |
| Normal nice=+10 | 200-500 µs | Background service |

```c
/* Set reader thread to high-priority normal */
struct sched_param param = { .sched_priority = 0 };
sched_setscheduler(d->reader_task, SCHED_NORMAL, &param);
set_user_nice(d->reader_task, -10);  /* nice -10 */
```

---

## 9. Full Architecture Diagram

```
User App (bpftrace / HAL)
     │
     ▼
eBPF kprobe → arch_read()     ← live tracing
     │
     ▼
arch_fops → arch_read()
     │ wait_event_interruptible
     │
IRQ → wake_up
     │
     ▼
CFS Scheduler
├─ vruntime comparison
├─ preempt if reader.vruntime < current.vruntime
└─ context switch to reader

     │
     ▼
copy_to_user → user data

[Memory pressure]
     │
     ▼
mm/vmscan → arch_shrinker_scan() → free cache pages

[System reboot]
     │
     ▼
register_reboot_notifier → arch_reboot_notify() → flush HW

[Kernel crash]
     │
     ▼
kdump → vmcore → crash tool → arch_dev_state (magic=0xA1B2C3D4)
```

---

## 10. Summary

| Mechanism | Purpose | API |
|-----------|---------|-----|
| CFS vruntime | Understand wake latency | `sched_setscheduler`, `set_user_nice` |
| `cond_resched` | Voluntary CPU yield | In long loops |
| eBPF kprobe | Live function tracing | `kprobe:function_name` |
| `trace_printk` | ftrace ring buffer output | Available in all kernel contexts |
| vmcore magic | Crash analysis anchor | Fixed magic value in state struct |
| `register_shrinker` | Memory pressure response | Release caches under OOM |
| `register_reboot_notifier` | Clean shutdown | Flush HW before reboot |
| CPU hotplug | Multi-core awareness | `cpuhp_setup_state` |
