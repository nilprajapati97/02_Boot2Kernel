# Design: Debug + Scheduler + ftrace + Debugfs
## Level 08 | End-to-End Deep Design from Scratch

---

## 1. Why Production Drivers Need Debug Infrastructure

A driver in production hits issues that:
- Never appear in development
- Only happen under load (100K events/sec)
- Only occur at specific temperatures / power states
- Require sub-microsecond timing to diagnose

The answer is **in-kernel debug infrastructure** — not `printk` everywhere.

---

## 2. Debugfs — Runtime Stats Interface

`debugfs` is a **RAM-based virtual filesystem** at `/sys/kernel/debug/` specifically for debugging. No stable ABI commitment — pure debug use.

### Creating a Debugfs Interface

```c
/* Create directory: /sys/kernel/debug/debug_drv/ */
d->dbg_dir = debugfs_create_dir("debug_drv", NULL);

/* Create read-only stats file */
debugfs_create_file("stats", 0444, d->dbg_dir, d, &stats_fops);

/* Create write-only trigger */
debugfs_create_file("trigger", 0200, d->dbg_dir, d, &trigger_fops);
```

### `seq_file` Pattern for Multi-Line Output

`seq_file` is the standard way to output multiple lines from a kernel file:

```c
static int stats_show(struct seq_file *sf, void *v) {
    struct my_dev *d = sf->private;
    seq_printf(sf, "bytes_rx: %lld\n", atomic64_read(&d->bytes_rx));
    seq_printf(sf, "irqs    : %d\n",   atomic_read(&d->irq_count));
    return 0;
}

static int stats_open(struct inode *inode, struct file *file) {
    return single_open(file, stats_show, inode->i_private);
}

static const struct file_operations stats_fops = {
    .open    = stats_open,
    .read    = seq_read,
    .llseek  = seq_lseek,
    .release = single_release,
};
```

### Usage

```bash
# Read stats
cat /sys/kernel/debug/debug_drv/stats

# Simulate IRQ
echo 1 > /sys/kernel/debug/debug_drv/trigger

# Read latency histogram
cat /sys/kernel/debug/debug_drv/latency
```

---

## 3. Latency Instrumentation

### `ktime_t` — Nanosecond Resolution Timestamps

```c
ktime_t start = ktime_get();        /* monotonic clock, ns precision */
/* ... operation ... */
ktime_t end = ktime_get();

s64 duration_ns = ktime_to_ns(ktime_sub(end, start));
```

### Latency Histogram

```c
/* Instead of storing every sample (memory), use buckets */
u64 buckets[8];  /* < 1µs, < 5µs, < 10µs, ... */

void record(u64 ns) {
    if      (ns < 1000)    buckets[0]++;
    else if (ns < 5000)    buckets[1]++;
    else if (ns < 10000)   buckets[2]++;
    /* ... */
}
```

### IRQ → Userspace Budget

```
Hardware event
    │ [~50-200 ns hardware latency]
IRQ fires
    │ [~100-500 ns: GIC, handler]
wake_up_interruptible()
    │ [~1-5 µs: scheduler wakeup]
Task scheduled on CPU
    │ [~5-20 µs: scheduler dispatch, context switch]
copy_to_user() completes
    │ [~100-500 ns]
User app reads data
```

Total typical: **10-50 µs**. Budget violation at `WARN()`.

---

## 4. `pr_debug` and Dynamic Debug

### The Problem with `printk`

`printk` in every code path:
- Slows down hot paths (printk is slow)
- Clutters dmesg in production

### `pr_debug` — Conditionally Compiled

```c
pr_debug("%s: IRQ handled\n", DRIVER_NAME);
```

- With `CONFIG_DYNAMIC_DEBUG=y`: compiled in but **off by default**
- Enable at runtime without recompiling:

```bash
# Enable debug for this driver
echo 'module debug_drv +p' > /sys/kernel/debug/dynamic_debug/control

# Enable for specific file
echo 'file debug_instrumented_driver.c +p' > /sys/kernel/debug/dynamic_debug/control

# Enable with timestamps
echo 'module debug_drv +pt' > /sys/kernel/debug/dynamic_debug/control

# Disable
echo 'module debug_drv -p' > /sys/kernel/debug/dynamic_debug/control
```

---

## 5. ftrace — Function Call Tracing

### Enable Function Tracing for Driver

```bash
# Mount debugfs
mount -t debugfs none /sys/kernel/debug

# Trace specific function
echo function > /sys/kernel/debug/tracing/current_tracer
echo 'debug_read debug_write debug_irq' > /sys/kernel/debug/tracing/set_ftrace_filter
echo 1 > /sys/kernel/debug/tracing/tracing_on

# Run your test
echo "test" > /dev/MyAnilDev8
cat /dev/MyAnilDev8

echo 0 > /sys/kernel/debug/tracing/tracing_on
cat /sys/kernel/debug/tracing/trace
```

### Function Graph Tracing (Shows call tree + timing)

```bash
echo function_graph > /sys/kernel/debug/tracing/current_tracer
echo 'debug_read' > /sys/kernel/debug/tracing/set_graph_function
```

Output:
```
 0)               |  debug_read() {
 0)   0.150 us    |    _raw_spin_lock_irqsave();
 0)   0.050 us    |    kfifo_out();
 0)   0.100 us    |    _raw_spin_unlock_irqrestore();
 0)   0.350 us    |    copy_to_user();
 0)   1.200 us    |  }
```

---

## 6. perf — Hardware Performance Counter Profiling

```bash
# Profile driver functions
sudo perf record -e cache-misses,instructions \
    -g dd if=/dev/MyAnilDev8 of=/dev/null bs=4096 count=1000

sudo perf report

# Count specific events
sudo perf stat -e cache-references,cache-misses \
    dd if=/dev/MyAnilDev8 of=/dev/null bs=4096 count=1000
```

### What to Look For

| Counter | High value means |
|---------|----------------|
| `cache-misses` | Structure not cache-aligned, false sharing |
| `branch-misses` | Unpredictable conditionals in hot path |
| `context-switches` | Excessive sleeping/waking |
| `cpu-cycles` | CPU time spent in function |

---

## 7. Scheduler-Aware Driver Design

### Understanding Task Priority

```c
/* In open() or read() */
d->reader_task = current;
d->reader_prio = current->prio;

pr_debug("reader: %s pid=%d prio=%d nice=%d\n",
         current->comm, current->pid,
         current->prio,            /* 0=highest RT, 139=lowest normal */
         task_nice(current));      /* -20 to +19 */
```

### Priority Ranges

```
RT priorities:   0–99   (SCHED_FIFO, SCHED_RR)
Normal:         100–139 (SCHED_NORMAL, nice -20 to +19)
Idle:           140     (SCHED_IDLE)
```

### Making Driver Scheduler-Aware

1. **Use `wake_up_interruptible`** not `wake_up` — don't wake non-interruptible sleep
2. **RT drivers**: use `wake_up` with `TASK_NORMAL` to wake RT tasks immediately
3. **High-priority reader**: if reader is RT, it will be scheduled immediately after wake_up
4. **Low-priority reader**: may wait 50–200 µs before scheduler dispatches it

```c
/* For RT-critical paths: boost priority of workqueue thread */
struct task_struct *t = d->wq->rescuer->task;
sched_setscheduler_nocheck(t, SCHED_FIFO, &(struct sched_param){.sched_priority = 50});
```

---

## 8. WARN_ON / BUG_ON — Defensive Programming

```c
/* BUG_ON: fatal kernel bug, triggers oops + stack trace */
BUG_ON(d == NULL);

/* WARN_ON: non-fatal, prints stack trace and continues */
WARN_ON(count > MAX_SIZE);

/* WARN with message */
WARN(latency_ns > 10000000ULL,
     "latency budget exceeded: %llu ns\n", latency_ns);
```

Use `WARN_ON` in production for unexpected but survivable states. Use `BUG_ON` only for truly impossible conditions (null pointer in validated path).

---

## 9. Debugfs vs Sysfs vs procfs

| | debugfs | sysfs | procfs |
|--|---------|-------|--------|
| Path | `/sys/kernel/debug/` | `/sys/` | `/proc/` |
| Purpose | Debug only, no ABI | Driver attributes (stable ABI) | Process info |
| Use for | Stats, histograms, triggers | Config, state | Process debugging |
| Stable ABI? | No | Yes | Mixed |

---

## 10. Real-World Debug Workflow

```bash
# 1. Check dmesg for errors
dmesg | grep debug_drv | tail -20

# 2. Enable dynamic debug
echo 'module debug_drv +p' > /sys/kernel/debug/dynamic_debug/control

# 3. Run test workload
dd if=/dev/urandom | sudo tee /dev/MyAnilDev8 > /dev/null &
sudo cat /dev/MyAnilDev8 > /dev/null &

# 4. Check stats
cat /sys/kernel/debug/debug_drv/stats

# 5. Check latency histogram
cat /sys/kernel/debug/debug_drv/latency

# 6. ftrace function graph
echo function_graph > /sys/kernel/debug/tracing/current_tracer
echo 1 > /sys/kernel/debug/tracing/tracing_on
sleep 2
echo 0 > /sys/kernel/debug/tracing/tracing_on
cat /sys/kernel/debug/tracing/trace | grep debug

# 7. perf analysis
sudo perf top -p $(pgrep cat)
```

---

## 11. Summary

| Tool | Location | Use Case |
|------|----------|----------|
| `debugfs` | `/sys/kernel/debug/` | Runtime stats, trigger injection |
| `pr_debug` + dynamic_debug | `/sys/kernel/debug/dynamic_debug/` | Conditional log enable |
| `ktime_t` | In code | µs-precision timing |
| Latency histogram | `debugfs/latency` | Latency distribution |
| `WARN` / `BUG_ON` | In code | Defensive assertions |
| ftrace | `/sys/kernel/debug/tracing/` | Call tracing + timing |
| perf | `perf record/report` | Hardware counter profiling |
| Scheduler info | `current->prio` | Priority-aware debugging |
