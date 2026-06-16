# Design: Beyond Elite — Multi-Queue + Zero-Copy + Epoll
## Level 11 | End-to-End Deep Design from Scratch

---

## 1. Why Multi-Queue?

At Level 10, we achieved per-CPU rings. Level 11 exposes **multiple independent queues to user-space**:
- Each queue has its own file descriptor (via `ioctl QUEUE_BIND`)
- Each thread `epoll_wait()`s on its own fd → no lock contention at all
- Fan-out: data broadcast to all queues, each thread consumes independently

This mirrors how **Linux network drivers** work (multi-queue NICs, e.g., `ethtool -L eth0 combined 4`).

---

## 2. Multi-Queue Architecture

```
write() → fanout_to_queues()
                │
        ┌───────┼───────┐
        ▼       ▼       ▼
    Queue[0] Queue[1] Queue[2]
    wq[0]    wq[1]    wq[2]
        │       │       │
    epoll   epoll   epoll
    fd[0]   fd[1]   fd[2]
        │       │       │
   Thread0  Thread1  Thread2
   CPU0     CPU1     CPU2
```

---

## 3. epoll Deep Internals

### How epoll Works (kernel side)

```c
/* Kernel: poll() implementation */
static __poll_t bey_poll(struct file *filp, poll_table *wait) {
    poll_wait(filp, &q->wq, wait);   /* register on wait queue */

    if (queue_depth(q) > 0)
        return EPOLLIN;               /* data available */
    return 0;
}
```

```
epoll_ctl(epfd, EPOLL_CTL_ADD, fd, ...)
    → calls poll() with poll_table → registers on driver's wait queue

epoll_wait(epfd, ...)
    → sleeps until wake_up(&q->wq) is called from driver

wake_up_interruptible(&q->wq)
    → wakes all tasks sleeping in epoll_wait on this queue
    → epoll callback fires → event returned to user-space
```

### Edge Trigger vs Level Trigger

| | Level Trigger (default) | Edge Trigger (`EPOLLET`) |
|--|------------------------|------------------------|
| When fired | Every `epoll_wait` if data present | Only when new data arrives |
| Use case | Simple, safe | High performance, needs drain loop |
| Risk | None | If not fully drained: miss events |

```c
/* Edge trigger requires full drain loop */
while (1) {
    ret = read(fd, buf, sizeof(buf));
    if (ret == -1 && errno == EAGAIN) break;  /* fully drained */
    process(buf, ret);
}
```

---

## 4. Zero-Copy with `mmap()`

### Standard read() Path — 2 copies

```
Kernel ring[tail].data
    │
    ├── [Copy 1] copy_to_user → user buffer
    │
    └── user processes data
```

### mmap() Zero-Copy Path — 0 copies

```
Kernel allocates shared_buf (physical pages)
    │
    ├── mmap() → maps same physical pages into user VMA
    │
    └── user reads directly from mapped address (0 copies)
```

### Kernel Side

```c
static int bey_mmap(struct file *filp, struct vm_area_struct *vma) {
    unsigned long pfn = virt_to_phys(q->shared_buf) >> PAGE_SHIFT;

    /* Map kernel physical pages into user VMA */
    return remap_pfn_range(vma, vma->vm_start, pfn, size,
                           vma->vm_page_prot);
}
```

### User Side

```c
/* Map queue 0's shared buffer */
void *shm = mmap(NULL, SHARED_BUF_SIZE,
                 PROT_READ | PROT_WRITE, MAP_SHARED,
                 fd,
                 0 * SHARED_BUF_PAGES);  /* pgoff = queue_id */

/* Read directly without syscall */
struct entry *e = (struct entry *)shm;
while (e->seq != last_seq + 1) ;  /* spin or poll */
process(e->data, e->len);
```

### `remap_pfn_range` Parameters

```c
remap_pfn_range(
    vma,           /* user VMA */
    vma->vm_start, /* user virtual start address */
    pfn,           /* kernel physical page frame number */
    size,          /* mapping size */
    vma->vm_page_prot   /* page protection flags */
);
```

---

## 5. CPU Affinity for Zero-Contention

### Binding IRQ to CPU

```bash
# Find queue 0's IRQ
cat /proc/interrupts | grep beyond_drv

# Pin IRQ 47 to CPU 0
echo 1 > /proc/irq/47/smp_affinity

# Pin IRQ 48 to CPU 1
echo 2 > /proc/irq/48/smp_affinity
```

### Binding Thread to CPU

```c
/* User space: pin thread to CPU matching queue */
cpu_set_t cpuset;
CPU_ZERO(&cpuset);
CPU_SET(queue_id, &cpuset);
pthread_setaffinity_np(pthread_self(), sizeof(cpuset), &cpuset);
```

Result: **thread `N` runs on CPU `N`**, where **queue `N`'s IRQ fires on CPU `N`** → producer and consumer on same CPU → cache-hot ring buffer access.

---

## 6. Sequence Number — Loss Detection

```c
/* Driver stamps each entry with sequence number */
entry->seq = atomic_inc_return(&q->seq_counter);

/* User-space detects drops */
if (entry->seq != last_seq + 1)
    dropped += entry->seq - last_seq - 1;
last_seq = entry->seq;
```

This allows user-space to detect queue overflows without additional syscalls.

---

## 7. Multi-Threaded User-Space Application

```c
/* Launch N threads, each bound to queue N */
for (int i = 0; i < num_queues; i++) {
    int fd = open("/dev/MyAnilDev11", O_RDONLY | O_NONBLOCK);
    
    /* Bind to queue i */
    ioctl(fd, BEYOND_IOC_QUEUE_BIND, &i);
    
    /* Pin thread to CPU i */
    pthread_create(&threads[i], NULL, reader_thread, (void*)(long)fd);
}

void *reader_thread(void *arg) {
    int fd = (int)(long)arg;
    
    /* epoll setup */
    int epfd = epoll_create1(0);
    struct epoll_event ev = { .events = EPOLLIN | EPOLLET, .data.fd = fd };
    epoll_ctl(epfd, EPOLL_CTL_ADD, fd, &ev);
    
    while (1) {
        epoll_wait(epfd, &ev, 1, -1);
        /* Drain with zero-copy via mmap or read() */
        drain_queue(fd);
    }
}
```

---

## 8. io_uring Concept (Next Evolution)

io_uring pushes the zero-copy model further:
- **Submission Ring** (SQ): user writes requests → kernel reads (no syscall)
- **Completion Ring** (CQ): kernel writes results → user reads (no syscall)
- Single `io_uring_enter()` syscall batches many I/O operations

Our `mmap()` shared buffer is the conceptual predecessor: shared memory between kernel and user with sequence numbers as the communication protocol.

---

## 9. Fan-Out Patterns

| Pattern | Use Case | Implementation |
|---------|---------|---------------|
| Broadcast | All queues receive every event | `for_each_queue: push()` |
| Round-robin | Load balance across queues | `queue[event_count % N]` |
| Hash-based | Same source → same queue | `queue[hash(key) % N]` |
| Flow-based | Network flows to same queue | `queue[5tuple_hash % N]` |

Our implementation uses **broadcast** (all queues get all data), suitable for monitoring/logging.

---

## 10. Build and Test Guide

```bash
# Compile
make -C /lib/modules/$(uname -r)/build M=$PWD modules

# Load
sudo insmod beyond_elite_multiqueue_driver.ko

# Check queues (should be num_online_cpus)
cat /sys/kernel/debug/beyond_drv/queue_info

# Single-queue test
echo "hello" > /dev/MyAnilDev11
cat /dev/MyAnilDev11

# Multi-queue test (C program)
./test_multiqueue   # opens N fds, bind each to queue N, epoll

# Sequence loss detection
./test_loss_rate    # flood write, check seq gaps

# Zero-copy test
./test_mmap         # mmap shared buffer, compare with read()

# eBPF monitor (drop rate per queue)
bpftrace -e '
kprobe:queue_push {
    @[arg0] = count();
}
kprobe:queue_pop {
    @deq[arg0] = count();
}'

# Cleanup
sudo rmmod beyond_elite_multiqueue_driver
```

---

## 11. Architecture Summary

```
┌─────────────────────────────────────────────────────────┐
│                    User Space                           │
│  Thread0     Thread1     Thread2     Thread3            │
│  epoll(fd0)  epoll(fd1)  epoll(fd2)  epoll(fd3)         │
│  CPU0        CPU1        CPU2        CPU3               │
│  mmap(q0)    mmap(q1)    mmap(q2)    mmap(q3)           │
└────────────────────────────────┬────────────────────────┘
                                 │ VFS / char device
┌────────────────────────────────▼────────────────────────┐
│              Kernel: /dev/MyAnilDev11                   │
│  write() → fanout_to_queues()                           │
│  ┌─────────┬─────────┬─────────┬─────────┐              │
│  │Queue[0] │Queue[1] │Queue[2] │Queue[3] │              │
│  │SPSC ring│SPSC ring│SPSC ring│SPSC ring│              │
│  │wq[0]   │wq[1]   │wq[2]   │wq[3]   │              │
│  └─────────┴─────────┴─────────┴─────────┘              │
│                      │                                  │
│  ioctl(QUEUE_BIND)   │ poll()/epoll                     │
│  mmap(pgoff=queue_id)│ wake_up_interruptible_all()      │
└─────────────────────────────────────────────────────────┘

Debugfs: /sys/kernel/debug/beyond_drv/queue_info
```

---

## 12. Summary

| Mechanism | API | Benefit |
|-----------|-----|---------|
| Multi-queue | `struct bey_queue[N]` | Per-queue isolation |
| epoll | `poll_wait()`, `wake_up()` | Scalable event notification |
| Zero-copy mmap | `remap_pfn_range()` | Eliminate copy_to_user |
| CPU affinity | `smp_affinity`, `pthread_setaffinity` | Cache-hot access |
| Sequence numbers | `atomic_inc_return` | Loss detection |
| IOCTL queue bind | `_IOW(BEYOND_MAGIC,1,int)` | Per-fd queue selection |
| Fan-out | `for_each_queue: push()` | Broadcast distribution |
