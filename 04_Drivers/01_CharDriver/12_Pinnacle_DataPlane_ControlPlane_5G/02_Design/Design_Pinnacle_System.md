# Design: Pinnacle — Data Plane + Control Plane + 5G Modem Style
## Level 12 | End-to-End Deep Design from Scratch

---

## 1. The Pinnacle Philosophy

At the Pinnacle level, the driver is designed like a **5G modem data path**:
- Millions of PDUs (Protocol Data Units) per second
- Predictable latency (< 100µs end-to-end)
- Zero-overhead control plane changes (RCU)
- Survives: memory pressure, reboots, CPU hotplug, eBPF probing

The design separates **data plane** (ultra-fast, never sleeps, no locks) from **control plane** (configures the data plane, can sleep, uses workqueue).

---

## 2. Data Plane vs Control Plane

```
┌───────────────────────────────────────┐
│            DATA PLANE                 │
│  (runs in IRQ / NAPI / reader context) │
│  ✅ Per-CPU rings (no contention)      │
│  ✅ Lock-free SPSC with smp_wmb/rmb   │
│  ✅ Batch 32 entries per dequeue      │
│  ✅ eventfd (no wake_up overhead)     │
│  ✅ hrtimer coalescing (50µs)         │
│  ❌ NO mutex/spinlock in hot path     │
│  ❌ NO sleep (wait_event)             │
│  ❌ NO kmalloc                        │
└───────────────────────────────────────┘
          │  rcu_dereference (read config)
          ▼
┌───────────────────────────────────────┐
│           CONTROL PLANE               │
│  (runs in workqueue / sysfs / ioctl)  │
│  ✅ RCU-protected config (zero lock)  │
│  ✅ ordered workqueue for changes     │
│  ✅ debugfs telemetry                 │
│  ✅ eventfd registration              │
│  ✅ reboot notifier                   │
│  ✅ can sleep, can allocate memory    │
└───────────────────────────────────────┘
```

---

## 3. RCU-Protected Configuration

### Problem

Data plane reads config on every packet (rate limit, burst size). Control plane updates config occasionally. How to read config safely without locks?

### Solution: RCU (Read-Copy-Update)

```c
/* Reader (data plane — runs millions of times/sec) */
rcu_read_lock();
cfg = rcu_dereference(d->config);   /* safe pointer dereference */
if (cfg->telemetry_enabled) log();
rcu_read_unlock();
/* No lock held → zero contention */

/* Writer (control plane — runs rarely) */
new_cfg = kmalloc(sizeof(*new_cfg), GFP_KERNEL);
*new_cfg = *old_cfg;               /* copy */
new_cfg->rate_limit_hz = new_rate; /* update */

rcu_assign_pointer(d->config, new_cfg);  /* atomic swap */
synchronize_rcu();   /* wait for all readers to finish with old_cfg */
kfree(old_cfg);      /* safe to free: no more readers */
```

### RCU Grace Period

```
Reader1:  rcu_read_lock() ─────────── rcu_read_unlock()
Reader2:         rcu_read_lock() ──────────── rcu_read_unlock()
Writer:                    rcu_assign_pointer()
                                    synchronize_rcu() waits here
                                                       │
                                                    kfree(old)
```

`synchronize_rcu()` returns after **all pre-existing readers have finished**. Zero contention for readers — ideal for high-frequency config reads.

---

## 4. Batch Processing

### Why Batch?

Without batching:
```
32 entries → 32 × [pop, copy_to_user, signal] = 32 syscalls
```

With batch IOCTL:
```
32 entries → 1 × [pop 32, memcpy 32, copy_to_user once] = 1 syscall
```

### IOCTL Batch Read Design

```c
struct batch_request {
    void __user *buf;   /* user-allocated buffer */
    int count;          /* requested count (max BATCH_SIZE) */
    int recvd;          /* actual count received */
    u64 latency_ns;     /* max latency in batch */
};

ioctl(fd, PIN_IOC_BATCH_READ, &req);
```

Benefits:
- **1 syscall** overhead for 32 entries
- **1 copy_to_user** for entire batch (TLB reuse)
- **max latency** returned per batch (SLA monitoring)

---

## 5. eventfd — Zero-Syscall Notification

### Standard `wake_up` path

```
Driver calls wake_up() → scheduler wakes task → context switch → task reads
```

Cost: 1 context switch per notification.

### eventfd path

```c
/* Control plane: register eventfd */
int evtfd = eventfd(0, EFD_NONBLOCK);
ioctl(fd, PIN_IOC_SET_EVTFD, &evtfd);

/* Data plane: signal eventfd */
eventfd_signal(ctx, 1);   /* increments counter by 1, no context switch */

/* User-space: epoll on eventfd */
epoll_ctl(epfd, EPOLL_CTL_ADD, evtfd, &ev_in);
epoll_wait(epfd, ...);    /* wakes when eventfd > 0 */
```

Benefits:
- **Single epoll fd** can monitor both char device AND eventfd
- eventfd counter accumulates → **coalescing for free**
- Kernel side: `eventfd_signal()` just increments atomic counter

---

## 6. Latency Histogram

### Design

Instead of recording every latency (requires huge memory), use fixed buckets:

| Bucket | Threshold | Meaning |
|--------|-----------|---------|
| 0 | < 1µs | Kernel hot cache |
| 1 | < 5µs | Normal kernel path |
| 2 | < 10µs | Scheduler wakeup |
| 3 | < 50µs | Normal |
| 4 | < 100µs | Acceptable |
| 5 | < 500µs | Warning |
| 6 | < 1ms | Problem |
| 7 | ≥ 1ms | Critical |

### Why Per-Entry Timestamps?

```c
entry->enqueue_ts = ktime_get();   /* at push */
/* ... entry travels through ring ... */
lat_ns = ktime_to_ns(ktime_sub(ktime_get(), entry->enqueue_ts));  /* at pop */
```

This measures **queue occupancy latency** — how long the entry waited in the ring. Includes scheduler dispatch delay.

---

## 7. 5G Modem Data Path Analogy

```
5G Radio Layer 1 (PHY) ──────────── Hardware interrupt
      │
5G Layer 2 (MAC)         ────────── Driver RX queue
      │                              dp_push() per PDU
      │                              Per-queue, per-subframe
5G Layer 3 (RLC/PDCP)   ────────── User-space processing
      │                              Batch dequeue
      │                              eventfd for wakeup
      ▼
Application (voice/data)
```

### Timing Requirements

| 5G parameter | Requirement | Driver analog |
|-------------|-------------|--------------|
| Subframe interval | 1ms | hrtimer period |
| Slot processing | 0.5ms | BATCH_SIZE dequeue |
| TTI latency | < 1ms | histogram bucket 6 |
| L1 IRQ → L2 | < 100µs | bucket 4 |

---

## 8. Ordered Workqueue for Control Plane

```c
g_dev->cp_wq = alloc_ordered_workqueue("pinnacle_cp", WQ_MEM_RECLAIM);
```

**Ordered**: work items execute sequentially (FIFO). Safe for config updates — no concurrent reconfigurations.

**`WQ_MEM_RECLAIM`**: workqueue has a rescue thread that runs even under memory pressure. Critical for flush operations during OOM.

```c
/* Queue a config update (non-blocking, from ioctl) */
struct cp_work *cpw = kmalloc(sizeof(*cpw), GFP_KERNEL);
INIT_WORK(&cpw->work, cp_reconfigure);
cpw->new_rate = new_rate;
queue_work(g_dev->cp_wq, &cpw->work);
/* Returns immediately, reconfiguration happens asynchronously */
```

---

## 9. Production Build and Test

### Makefile

```makefile
obj-m := pinnacle_dataplane_driver.o

all:
	make -C /lib/modules/$(shell uname -r)/build M=$(PWD) modules

clean:
	make -C /lib/modules/$(shell uname -r)/build M=$(PWD) clean
```

### Module Load

```bash
sudo insmod pinnacle_dataplane_driver.ko
ls /dev/MyAnilDev12
cat /sys/kernel/debug/pinnacle/telemetry
```

### Throughput Test

```bash
# Write 100K entries
for i in $(seq 1 100000); do echo "packet_$i" > /dev/MyAnilDev12; done

# Parallel reader
cat /dev/MyAnilDev12 > /dev/null &

# Check stats
cat /sys/kernel/debug/pinnacle/telemetry
```

### Batch Read Test (C)

```c
int fd = open("/dev/MyAnilDev12", O_RDONLY);

char user_buf[32 * 512];
struct batch_request req = {
    .buf   = user_buf,
    .count = 32,
};

ioctl(fd, PIN_IOC_BATCH_READ, &req);
printf("Received: %d entries, max_lat=%lu ns\n",
       req.recvd, req.latency_ns);
```

### eventfd Notification

```c
int evtfd = eventfd(0, EFD_NONBLOCK);
int drv_fd = open("/dev/MyAnilDev12", O_RDONLY);
ioctl(drv_fd, PIN_IOC_SET_EVTFD, &evtfd);

int epfd = epoll_create1(0);
struct epoll_event ev = { .events = EPOLLIN, .data.fd = evtfd };
epoll_ctl(epfd, EPOLL_CTL_ADD, evtfd, &ev);

/* Block until data ready */
epoll_wait(epfd, &ev, 1, -1);
/* Drain eventfd counter */
uint64_t count;
read(evtfd, &count, sizeof(count));
/* Now do batch read */
ioctl(drv_fd, PIN_IOC_BATCH_READ, &req);
```

### eBPF Monitoring

```bash
# Monitor fanout calls
bpftrace -e '
kprobe:dp_fanout {
    @fanout_count = count();
    @fanout_bytes = hist(arg2);
}'

# Monitor latency histogram
bpftrace -e '
kprobe:dp_pop_batch {
    @[tid] = nsecs;
}
kretprobe:dp_pop_batch {
    @lat_hist = hist(nsecs - @[tid]);
    delete(@[tid]);
}'

# ftrace: function graph on write path
echo function_graph > /sys/kernel/debug/tracing/current_tracer
echo 'pin_write dp_fanout dp_push' > /sys/kernel/debug/tracing/set_graph_function
echo 1 > /sys/kernel/debug/tracing/tracing_on
```

---

## 10. Full Architecture

```
┌────────────────────────────────────────────────────────────────┐
│                        User Space                              │
│                                                                │
│  ┌──────────┐  ┌──────────┐  ┌──────────┐  ┌──────────┐      │
│  │ Thread 0 │  │ Thread 1 │  │ Thread 2 │  │ Thread N │      │
│  │ CPU0     │  │ CPU1     │  │ CPU2     │  │ CPUN     │      │
│  │ epoll(ev)│  │ epoll(ev)│  │ epoll(ev)│  │ epoll(ev)│      │
│  │ batch_rd │  │ batch_rd │  │ batch_rd │  │ batch_rd │      │
│  └────┬─────┘  └────┬─────┘  └────┬─────┘  └────┬─────┘      │
└───────┼──────────────┼──────────────┼──────────────┼──────────┘
        │              │              │              │
        ▼              ▼              ▼              ▼
   eventfd[0]     eventfd[1]     eventfd[2]     eventfd[N]
        │              │              │              │
┌───────┼──────────────┼──────────────┼──────────────┼──────────┐
│  Kernel: /dev/MyAnilDev12           │                          │
│                                     │                          │
│  write() → dp_fanout()                                         │
│       │                                                        │
│  ┌────▼─────┐  ┌──────────┐  ┌──────────┐  ┌──────────┐      │
│  │ dp_queue │  │ dp_queue │  │ dp_queue │  │ dp_queue │      │
│  │   [0]   │  │   [1]   │  │   [2]   │  │   [N]   │      │
│  │ SPSC    │  │ SPSC    │  │ SPSC    │  │ SPSC    │      │
│  │ lat_hist│  │ lat_hist│  │ lat_hist│  │ lat_hist│      │
│  └──────────┘  └──────────┘  └──────────┘  └──────────┘      │
│                                                                │
│  hrtimer (50µs) → coalesce → eventfd_signal()                  │
│  RCU config ← cp_wq workqueue (async reconfigure)              │
│  reboot_notifier → flush queues                                │
│  debugfs/telemetry → per-queue + lat_hist                      │
└────────────────────────────────────────────────────────────────┘
```

---

## 11. Summary

| Mechanism | API | Role |
|-----------|-----|------|
| Multi-queue SPSC | `atomic_set/smp_wmb` | Data plane zero-contention |
| Batch dequeue | `dp_pop_batch(BATCH_SIZE=32)` | Amortize syscall overhead |
| eventfd | `eventfd_signal()` | Zero-copy notification |
| RCU config | `rcu_assign_pointer / rcu_dereference` | Lock-free config reads |
| hrtimer 50µs | `hrtimer_start` | Bounded notification latency |
| Ordered workqueue | `alloc_ordered_workqueue` | Sequential config updates |
| Latency histogram | `atomic64 bucket[8]` | Per-queue SLA monitoring |
| eBPF trace_printk | ftrace ring buffer | Zero-overhead observability |
| Reboot notifier | `register_reboot_notifier` | Clean shutdown |
| debugfs telemetry | `seq_file` | Production monitoring |
