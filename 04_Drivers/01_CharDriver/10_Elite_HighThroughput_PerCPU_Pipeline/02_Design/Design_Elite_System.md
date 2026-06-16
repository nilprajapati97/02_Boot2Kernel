# Design: Elite High-Throughput Per-CPU Pipeline
## Level 10 | End-to-End Deep Design from Scratch

---

## 1. The 10Gbps Problem

10Gbps = **1.25 GB/s** sustained throughput. A driver that:
- Uses a single spinlock → 100ns per lock → **10M locks/sec max** → bottleneck at 10Gbps
- Wakes reader on every entry → **context switches dominate**
- Copies byte-by-byte → **memcpy stalls**

**Solution**: Per-CPU data paths. No shared state on the critical path.

---

## 2. Per-CPU Ring Buffers

### Core Idea

Instead of one global ring buffer (with one lock), use **one ring buffer per CPU**.

```
CPU 0 → cpu_ring[0] → reader thread (CPU 0)
CPU 1 → cpu_ring[1] → reader thread (CPU 1)
CPU 2 → cpu_ring[2] → reader thread (CPU 2)
CPU 3 → cpu_ring[3] → reader thread (CPU 3)
```

### `alloc_percpu` / `free_percpu`

```c
/* Allocate one struct per CPU, cache-aligned automatically */
rings = alloc_percpu(struct cpu_ring);

/* Access from current CPU */
struct cpu_ring *r = this_cpu_ptr(rings);

/* Access specific CPU (when holding CPU offline) */
struct cpu_ring *r = per_cpu_ptr(rings, cpu);
```

### Cache Alignment is Critical

```c
struct cpu_ring {
    /* ... */
} ____cacheline_aligned;
```

Without alignment: two CPUs' `cpu_ring` structs on same cache line → **false sharing** → cache invalidation storm.

With alignment: each CPU's ring on its own cache line → **no contention**.

---

## 3. Lock-Free SPSC Ring Buffer

### The SPSC Model

**Single Producer, Single Consumer** per ring:
- **Producer**: IRQ handler pinned to CPU X → writes to `cpu_ring[X]`
- **Consumer**: reader thread → reads from `cpu_ring[X]`

No locks needed — only memory barriers.

### Producer (IRQ context)

```c
int head = atomic_read(&r->head);
int next = (head + 1) & RING_MASK;

if (next == atomic_read(&r->tail)) {
    /* Full — drop or block */
    return false;
}

/* Write data to slot */
r->entries[head] = entry;

/* CRITICAL: ensure data visible before updating head */
smp_wmb();  /* store barrier */

atomic_set(&r->head, next);
```

### Consumer (reader context)

```c
int tail = atomic_read(&r->tail);

/* CRITICAL: ensure tail read before reading head */
smp_rmb();  /* load barrier */

if (tail == atomic_read(&r->head)) return false; /* empty */

entry = r->entries[tail];

smp_wmb();
atomic_set(&r->tail, (tail + 1) & RING_MASK);
```

### Why `smp_wmb/rmb`?

Without barriers, out-of-order CPUs (ARM, POWER) can reorder:
- Producer: CPU writes data AFTER updating head → consumer reads garbage
- Consumer: CPU reads head BEFORE reading data → stale data

`smp_wmb()` = **write memory barrier** — all writes before this are visible before writes after.
`smp_rmb()` = **read memory barrier** — all reads after this see effects of writes before.

---

## 4. NAPI-Style Polling

### What is NAPI?

Network NAPI (New API) — instead of one interrupt per packet:
1. **Interrupt fires** → disable IRQ → schedule NAPI poll
2. **NAPI poll** → process up to BUDGET packets → re-enable IRQ
3. If BUDGET exhausted before ring drained → reschedule poll (no IRQ)

### Driver Adaptation

```c
#define NAPI_BUDGET 64   /* max entries per poll */

int poll(struct elite_dev *d, ...) {
    int budget = NAPI_BUDGET;
    for_each_online_cpu(cpu) {
        while (budget-- > 0) {
            if (!cpu_ring_pop(r, ...)) break;
            copy_to_user(...);
        }
    }
    return consumed;
}
```

### Throughput Impact

| Approach | Context switches/sec | Throughput |
|----------|---------------------|-----------|
| Wake on every entry | 10M/sec | < 1Gbps |
| NAPI BUDGET=64 | 156K/sec | > 10Gbps |
| NAPI BUDGET=256 | 39K/sec | > 40Gbps |

---

## 5. Interrupt Coalescing (Interrupt Moderation)

### Problem with Per-Entry Wakeup

```
10M entries/sec
→ 10M wake_up() calls/sec
→ 10M context switches/sec
→ CPU completely occupied switching tasks
→ 0% CPU for actual data processing
```

### Solution: Batch Wakeups

```c
/* Wake on COUNT threshold OR timer expiry */
if (++pending >= IRQ_COALESCE_COUNT) {
    wake_up(&wq);
    pending = 0;
} else {
    hrtimer_start(&timer, 100µs, HRTIMER_MODE_REL);
}
```

This is exactly what **NIC interrupt moderation** does (e.g., `ethtool -C eth0 rx-usecs 50`).

### Latency vs Throughput Tradeoff

| Coalescing | Wakeups/sec | Latency | Throughput |
|-----------|------------|---------|-----------|
| None | 10M | 1-5µs | Poor |
| COUNT=32 | 312K | 100-500µs | Excellent |
| 100µs timer | ~10K | ≤100µs | Excellent |
| COUNT=32 + 100µs timer | Adaptive | ≤100µs | Excellent |

---

## 6. hrtimer — High-Resolution Timer

```c
struct hrtimer timer;
ktime_t delay = ktime_set(0, 100000);  /* 100µs = 100,000 ns */

hrtimer_init(&timer, CLOCK_MONOTONIC, HRTIMER_MODE_REL);
timer.function = my_callback;

/* Start (one-shot) */
hrtimer_start(&timer, delay, HRTIMER_MODE_REL);

/* Cancel (on exit) */
hrtimer_cancel(&timer);
```

Resolution: typically **~50ns on x86**, **~1µs on ARM A-class**. Far better than `mod_timer` (jiffies, ~1ms resolution).

---

## 7. Batch `copy_to_user`

### Bad Pattern (one syscall per entry)

```c
for (entry in ring) {
    copy_to_user(ubuf + offset, entry.data, entry.len);
    offset += entry.len;
}
```

Each `copy_to_user` call crosses user/kernel boundary (page table walk, fault handling).

### Good Pattern (batch into kernel buffer, one large copy)

```c
char kbuf[4096];  /* kernel staging buffer */
int  staged = 0;

for (entry in ring) {
    memcpy(kbuf + staged, entry.data, entry.len);
    staged += entry.len;
    if (staged + ENTRY_SIZE > sizeof(kbuf)) break;
}

copy_to_user(ubuf, kbuf, staged);  /* single kernel→user copy */
```

---

## 8. DMA Engine Chaining

```
Write 1MB:  DMA src→staging buffer (async)
                  │
                  ├── DMA callback → elite_dma_callback()
                  │
                  └── complete(&dma_done) → wakes writer
```

```c
desc = dmaengine_prep_dma_memcpy(chan, dst_phys, src_phys, len,
                                  DMA_CTRL_ACK | DMA_PREP_INTERRUPT);
desc->callback       = elite_dma_callback;
desc->callback_param = &dma_work;

cookie = dmaengine_submit(desc);  /* queue */
dma_async_issue_pending(chan);    /* kick HW */
```

---

## 9. eBPF Hooks via `trace_printk`

```c
trace_printk("elite: poll consumed %zu bytes on cpu%d\n", total, cpu);
```

Monitor live from terminal:

```bash
# Enable trace
echo 1 > /sys/kernel/debug/tracing/tracing_on
cat /sys/kernel/debug/tracing/trace_pipe &

# Watch throughput
cat /sys/kernel/debug/elite_drv/ring_stats

# bpftrace on write path
bpftrace -e 'kprobe:elite_write { @bytes = hist(arg2); }'
```

---

## 10. Performance Analysis

```bash
# Throughput benchmark
dd if=/dev/zero bs=256 count=100000 > /dev/MyAnilDev10
dd if=/dev/MyAnilDev10 of=/dev/null bs=4096 count=10000

# Latency histogram via perf
sudo perf record -e sched:sched_switch -ag sleep 5
sudo perf report

# CPU usage per-CPU
sudo perf stat -e cpu-cycles,instructions \
    -a --per-cpu sleep 5

# Ring buffer stats
watch -n1 cat /sys/kernel/debug/elite_drv/ring_stats
```

---

## 11. Architecture Summary

```
Hardware Event (DMA Complete / IRQ)
         │
    CPU-pinned ISR
         │
    cpu_ring_push()  ← no lock, memory barrier
         │
    pending++ ≥ COALESCE? or 100µs timer?
         │YES
    wake_up_interruptible()
         │
    reader: elite_poll()
         │  NAPI budget loop
         │  cpu_ring_pop() × 64
         │  batch → copy_to_user (1 call)
         ▼
    User application
    
Debugfs: /sys/kernel/debug/elite_drv/ring_stats
eBPF:    bpftrace kprobe:elite_read
```

---

## 12. Summary Table

| Mechanism | API | Impact |
|-----------|-----|--------|
| Per-CPU rings | `alloc_percpu` / `this_cpu_ptr` | Zero inter-CPU contention |
| Lock-free SPSC | `smp_wmb/rmb` | No spinlock in hot path |
| NAPI polling | Budget loop | 64× fewer context switches |
| hrtimer coalescing | `hrtimer_start` 100µs | Bounded latency + high throughput |
| Batch copy_to_user | Single call per batch | 64× fewer page table walks |
| DMA Engine | `dmaengine_prep_dma_memcpy` | CPU-free bulk transfers |
| trace_printk | ftrace ring buffer | Zero-overhead eBPF tracing |
