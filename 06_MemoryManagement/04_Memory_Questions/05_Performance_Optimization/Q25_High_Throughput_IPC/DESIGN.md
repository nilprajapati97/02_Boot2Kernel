# Q25 — Design a High-Throughput IPC Mechanism

---

## 1. Problem Statement

Inter-Process Communication (IPC) is the backbone of microservice, plugin-based, and multi-process architectures. The challenge is achieving high throughput (GB/s range) with low latency (sub-microsecond) while maintaining correctness, security, and scalability.

Traditional IPC (pipes, UNIX sockets) copies data through the kernel — expensive for large transfers. Modern high-throughput IPC must minimize copies, avoid blocking system calls, and leverage hardware features (shared memory, RDMA, io_uring).

Design a high-throughput IPC system covering the spectrum from latency-optimized small messages to throughput-optimized bulk transfers.

---

## 2. Requirements

### 2.1 Functional Requirements
- Small message IPC (< 256B): < 1 µs latency.
- Large message IPC (> 1MB): > 10 GB/s throughput.
- Multi-producer, multi-consumer (MPMC) queues with flow control.
- Ordering guarantee: in-order per sender.
- Security: processes cannot access each other's private memory.
- Fault isolation: sender crash does not corrupt receiver state.

### 2.2 Non-Functional Requirements
- Zero-copy path for large messages.
- No kernel involvement for steady-state data transfer.
- Graceful degradation under overload (backpressure, not silent drop).

---

## 3. Constraints & Assumptions

- Linux 6.x: io_uring, memfd, POSIX shared memory.
- Same-host IPC (not cross-machine RDMA — though we'll mention it).
- Trusted-environment shared memory (both processes must opt in).
- C language, POSIX API.

---

## 4. Architecture Overview

```
  ┌──────────────────────────────────────────────────────────────┐
  │  IPC Mechanism Selection                                     │
  │                                                              │
  │  Message Size    Latency Target    Mechanism                 │
  │  ──────────────────────────────────────────────             │
  │  < 8B            < 100 ns          Shared memory + futex    │
  │  8B – 64KB       < 1 µs            Shared ring buffer       │
  │                                    (no kernel after setup)  │
  │  64KB – 1MB      < 10 µs           io_uring + splice/tee    │
  │  > 1MB           Throughput        memfd + mmap (zero-copy) │
  │  Cross-host      < 1 µs            RDMA (verbs / RoCE)      │
  └──────────────────────────────────────────────────────────────┘

  Shared Memory Ring Buffer Architecture:
  ┌──────────────────────────────────────────────────────────────┐
  │  Process A (Producer)         Process B (Consumer)          │
  │  ┌─────────────────────┐      ┌─────────────────────┐      │
  │  │  mmap shared region │◄────►│  mmap shared region │      │
  │  │  [ring buffer]      │      │  [ring buffer]      │      │
  │  │  head: atomic_t     │      │  tail: atomic_t     │      │
  │  └─────────────────────┘      └─────────────────────┘      │
  │  write msg, advance head      read msg, advance tail        │
  │  if consumer slow: futex      if empty: futex_wait          │
  │  (no syscall in fast path)    (no syscall in fast path)     │
  └──────────────────────────────────────────────────────────────┘
```

---

## 5. Core Data Structures

### 5.1 Shared Memory Ring Buffer (Lock-Free SPSC)

```c
/* Shared between producer and consumer via mmap */
struct shm_ring {
    /* Control block — written by both sides, in same cache line */
    _Atomic(uint32_t)  head;       /* producer writes here */
    char               _pad1[60]; /* prevent false sharing */
    _Atomic(uint32_t)  tail;       /* consumer writes here */
    char               _pad2[60];

    /* Metadata */
    uint32_t           capacity;   /* ring size in entries (power-of-two) */
    uint32_t           entry_size; /* fixed per entry in bytes */
    uint32_t           mask;       /* capacity - 1 */
    uint32_t           _futex;     /* futex word for blocking wait */

    /* Data slots: capacity × entry_size bytes */
    uint8_t            data[];
};

/* Create shared ring: */
int fd = memfd_create("ipc_ring", MFD_CLOEXEC);
ftruncate(fd, sizeof(struct shm_ring) + RING_CAPACITY * ENTRY_SIZE);
struct shm_ring *ring = mmap(NULL, total_size,
                             PROT_READ|PROT_WRITE, MAP_SHARED, fd, 0);

/* Share fd to peer via Unix socket sendmsg(SCM_RIGHTS) */
```

### 5.2 Large Message: memfd Zero-Copy Transfer

```c
/* Producer: allocate memfd, write data, send fd */
int data_fd = memfd_create("large_msg", MFD_CLOEXEC | MFD_ALLOW_SEALING);
ftruncate(data_fd, message_size);
void *data = mmap(NULL, message_size, PROT_READ|PROT_WRITE, MAP_SHARED, data_fd, 0);
memcpy(data, payload, message_size);   /* or use DMA to write directly */

/* Seal: prevent further resize (integrity guarantee) */
fcntl(data_fd, F_ADD_SEALS, F_SEAL_SHRINK | F_SEAL_GROW | F_SEAL_WRITE);

/* Send fd to consumer via Unix socket */
send_fd(control_socket, data_fd);

/* Consumer: receives fd, mmap-reads without copy */
int recv_fd = recv_fd(control_socket);
void *consumer_view = mmap(NULL, message_size, PROT_READ, MAP_SHARED, recv_fd, 0);
/* Consumer accesses data directly — zero copy, no kernel involvement */
```

### 5.3 io_uring-based IPC Channel

```c
/* For kernel-mediated reliable IPC with ordering guarantees */
struct io_uring ring;
io_uring_queue_init(QUEUE_DEPTH, &ring, IORING_SETUP_SQPOLL);

/* SQPOLL: kernel background thread polls SQ — no syscall for submission */
/* Userspace writes to SQ, kernel thread picks up without io_uring_enter() */

/* Submit splice: move data from pipe to pipe (zero-copy in kernel) */
struct io_uring_sqe *sqe = io_uring_get_sqe(&ring);
io_uring_prep_splice(sqe, src_pipe_fd, -1, dst_pipe_fd, -1,
                     msg_size, SPLICE_F_MOVE);
io_uring_submit(&ring);  /* or with SQPOLL: no submit needed */
```

---

## 6. Key Algorithms & Design Decisions

### 6.1 Lock-Free SPSC Ring — Fast Path

```c
/* Producer: enqueue (no lock, no syscall in fast path) */
bool ring_enqueue(struct shm_ring *ring, const void *msg)
{
    uint32_t head = atomic_load_explicit(&ring->head, memory_order_relaxed);
    uint32_t next_head = (head + 1) & ring->mask;
    uint32_t tail = atomic_load_explicit(&ring->tail, memory_order_acquire);

    if (next_head == tail)
        return false;  /* full: caller should spin or wait */

    memcpy(&ring->data[head * ring->entry_size], msg, ring->entry_size);

    /* Store-release: ensure data is visible before head update */
    atomic_store_explicit(&ring->head, next_head, memory_order_release);

    return true;
}

/* Consumer: dequeue */
bool ring_dequeue(struct shm_ring *ring, void *msg)
{
    uint32_t tail = atomic_load_explicit(&ring->tail, memory_order_relaxed);
    uint32_t head = atomic_load_explicit(&ring->head, memory_order_acquire);

    if (tail == head)
        return false;  /* empty */

    memcpy(msg, &ring->data[tail * ring->entry_size], ring->entry_size);

    /* Store-release: marks slot as reusable */
    atomic_store_explicit(&ring->tail, (tail + 1) & ring->mask,
                          memory_order_release);
    return true;
}
```

**Memory ordering rationale:** store-release on head/tail ensures message data is visible to consumer before the head pointer update. load-acquire on the other side ensures the pointer read is ordered after the data read.

### 6.2 Blocking Wait — Futex-Based Slow Path

When the ring is empty (consumer) or full (producer), avoid busy-spinning forever:

```c
/* Consumer: wait for data */
void ring_wait_data(struct shm_ring *ring)
{
    /* Spin for 1000 iterations (covers typical < 1 µs producer latency) */
    for (int i = 0; i < 1000; i++) {
        if (atomic_load(&ring->head) != atomic_load(&ring->tail))
            return;  /* data available: no syscall */
        cpu_relax();
    }

    /* Still empty: sleep via futex */
    uint32_t current_head = atomic_load(&ring->head);
    syscall(SYS_futex, &ring->_futex, FUTEX_WAIT,
            current_head, NULL, NULL, 0);
    /* Kernel: put consumer to sleep; producer calls FUTEX_WAKE after enqueue */
}

/* Producer: after enqueue, wake sleeping consumer */
void ring_notify(struct shm_ring *ring)
{
    uint32_t new_head = atomic_load(&ring->head);
    atomic_store(&ring->_futex, new_head);
    syscall(SYS_futex, &ring->_futex, FUTEX_WAKE, 1, NULL, NULL, 0);
}
```

### 6.3 MPMC Ring — Multi-Producer, Multi-Consumer

SPSC requires exactly one producer and one consumer. For MPMC:

```c
/* Use per-slot sequence counter (Dmitry Vyukov MPMC queue) */
struct mpmc_slot {
    _Atomic(uint32_t)  sequence;  /* monotonically increasing */
    uint8_t            data[ENTRY_SIZE];
};

struct mpmc_ring {
    _Atomic(uint32_t)  head;  /* producers compete for this */
    char               _pad1[60];
    _Atomic(uint32_t)  tail;  /* consumers compete for this */
    char               _pad2[60];
    uint32_t           capacity;
    uint32_t           mask;
    struct mpmc_slot   slots[];
};

/* Producer: claim a slot */
bool mpmc_enqueue(struct mpmc_ring *ring, const void *msg)
{
    uint32_t head, seq;
    struct mpmc_slot *slot;

    head = atomic_load_explicit(&ring->head, memory_order_relaxed);
    for (;;) {
        slot = &ring->slots[head & ring->mask];
        seq = atomic_load_explicit(&slot->sequence, memory_order_acquire);
        int diff = (int)(seq - head);

        if (diff == 0) {
            /* Slot is free: try to claim it */
            if (atomic_compare_exchange_weak_explicit(
                    &ring->head, &head, head + 1,
                    memory_order_relaxed, memory_order_relaxed))
                break;  /* claimed */
        } else if (diff < 0) {
            return false;  /* ring full */
        } else {
            head = atomic_load_explicit(&ring->head, memory_order_relaxed);
        }
    }
    memcpy(slot->data, msg, ENTRY_SIZE);
    atomic_store_explicit(&slot->sequence, head + 1, memory_order_release);
    return true;
}
```

### 6.4 Zero-Copy Large Transfer — Page Remapping

For transfers > 1 MB, avoid even memcpy by remapping pages:

```c
/* Kernel technique: remap_file_pages() / process_vm_writev() */

/* Producer: allocate buffer via mmap */
void *buf = mmap(NULL, size, PROT_READ|PROT_WRITE,
                  MAP_SHARED|MAP_ANONYMOUS, -1, 0);
write_payload(buf);

/* Consumer: attach same physical pages to its address space */
/* Via memfd + fd passing: consumer mmap's same fd → same physical pages */
/* No data copy: CPU writes once, consumer reads from same cache lines */
```

### 6.5 Unix Socket for Control Path + Shared Memory for Data

Best practice for production IPC:
```
Control plane (Unix socket, datagram):
    - Connection establishment
    - Capability passing: sendmsg(SCM_RIGHTS) to share fds
    - Flow control signals: notify peer about ring state
    - Error reporting

Data plane (shared memory ring):
    - Actual payload transfer
    - Zero kernel involvement in steady state
    - Both sides map the same physical pages

Setup:
    1. Producer creates memfd, maps it
    2. Producer sends fd to consumer via Unix socket (SCM_RIGHTS)
    3. Consumer maps same fd → both share physical memory
    4. Data flows through ring: no syscalls
    5. Blocking: futex (shared across processes via MAP_SHARED)
```

---

## 7. Trade-off Analysis

| Mechanism | Latency | Throughput | Kernel Involvement | Complexity |
|---|---|---|---|---|
| Pipe (kernel copy) | 5–50 µs | 2–5 GB/s | Per transfer | Low |
| Unix socket (copy) | 5–50 µs | 2–5 GB/s | Per message | Low |
| POSIX mq | 10–100 µs | 1–3 GB/s | Per message | Low |
| Shared memory ring | 100–500 ns | 20+ GB/s | None (steady state) | Medium |
| memfd + mmap | 1–5 µs (setup) | Memory bandwidth | Only for fd passing | Medium |
| io_uring + splice | 2–10 µs | 10+ GB/s | SQPOLL: near-zero | High |
| RDMA (cross-host) | 1–2 µs | 100+ GB/s | None (RDMA verbs) | Very High |

---

## 8. Real Linux Kernel References

| Component | Source | Symbol |
|---|---|---|
| futex | `kernel/futex/futex.c` | `do_futex()`, `futex_wait()`, `futex_wake()` |
| memfd | `mm/memfd.c` | `memfd_create()`, `memfd_add_seals()` |
| Unix socket SCM_RIGHTS | `net/unix/scm.c` | `unix_attach_fds()`, `scm_detach_fds()` |
| splice/pipe | `fs/splice.c` | `do_splice()`, `splice_direct_to_actor()` |
| io_uring splice | `io_uring/splice.c` | `io_splice()`, `io_tee()` |
| process_vm_readv/writev | `mm/process_vm_access.c` | `process_vm_rw()` |
| POSIX mq | `ipc/mqueue.c` | `mq_open()`, `mq_send()` |
| shm_open | `mm/shmem.c` | `shmem_file_setup()` |

---

## 9. Failure Modes & Debug Strategies

### 9.1 Ring Full — Producer Blocked
```c
/* Monitor: track ring fullness */
uint32_t used = (ring->head - ring->tail) & ring->mask;
if (used > ring->capacity * 0.9)
    /* Alert: consumer is falling behind */
/* Fix: increase ring capacity, add consumer threads, apply backpressure */
```

### 9.2 Memory Ordering Bug — Data Corruption
```bash
# Tools:
ThreadSanitizer (TSAN): detects data races in shared memory
# Compile: clang -fsanitize=thread -g
# Symptoms: sporadic corruption, works under gdb (Heisenbug)
# Always use store-release / load-acquire, not relaxed, for ring head/tail
```

### 9.3 Futex Missed Wakeup
```c
/* Classic TOCTOU bug: */
/* 1. Consumer: ring_empty() → true */
/* 2. Producer: enqueue() → FUTEX_WAKE (nobody sleeping yet) */
/* 3. Consumer: FUTEX_WAIT → sleeps forever */
/* Fix: use seqlock or ensure FUTEX_WAIT is called BEFORE the check */
/* Linux futex semantics: FUTEX_WAIT atomically compares futex value and sleeps */
/* → if value changed by producer, FUTEX_WAIT returns EAGAIN immediately */
```

---

## 10. Performance Considerations

- **Cache-line alignment:** Ring head and tail on separate cache lines (64-byte padding). False sharing between head and tail causes unnecessary cache coherency traffic between producer and consumer CPUs.
- **Batch processing:** Dequeue multiple messages per NAPI-style poll iteration. Reading 64 messages per iteration amortizes cache misses.
- **Pinned CPUs:** If producer and consumer are on the same NUMA node and same L3 cache, cache coherency is fast (~10 ns). Across sockets: 50–100 ns for coherency.
- **Huge page backing:** Back shared memory with 2MB huge pages for ring > 2MB. Reduces TLB pressure from 512 entries to 1 per 2MB.
- **Copy vs no-copy threshold:** For messages < L1 cache size (32KB), copying is often faster than the overhead of fd passing + mmap (which has syscall + page table cost). Tune threshold empirically.

---

## 11. Interview Answer Strategy (NVIDIA 10-yr Level)

**What they want to hear:**
1. Zero-copy design: memfd + SCM_RIGHTS passes fd, consumer mmap's same physical pages — no data copy.
2. Lock-free SPSC ring: store-release on head, load-acquire on tail — explain why (ensure data visible before pointer advance).
3. MPMC: per-slot sequence counter solves ABA problem — same approach as io_uring SQ/CQ.
4. Futex for blocking: `FUTEX_WAIT` is atomic compare-and-sleep — avoids missed-wakeup race.
5. Unix socket control plane + shared memory data plane: standard production architecture.
6. io_uring SQPOLL: kernel background thread polls SQ — submission without syscall.
7. NUMA-awareness: place shared memory on node between producer and consumer, or replicate per-node.
8. Splice/tee: kernel-space buffer hand-off — no user-space copy, but still one kernel copy.
