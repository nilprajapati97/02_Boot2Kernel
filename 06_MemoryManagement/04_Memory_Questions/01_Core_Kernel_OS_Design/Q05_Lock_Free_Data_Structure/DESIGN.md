# Q05 — Design a Lock-Free Data Structure for Kernel Usage (IO Queue)

---

## 1. Problem Statement

Design a lock-free MPMC (Multi-Producer Multi-Consumer) ring buffer suitable for kernel use — specifically as an I/O completion queue shared between hardware (DMA engine / GPU), softirq handlers, and user-space-facing kernel threads.

**Why lock-free in the kernel?**
- Spinlocks in interrupt context extend interrupt-disabled duration.
- Mutexes cannot be used in atomic (softirq/hardirq) context at all.
- A lock-free ring buffer used in the I/O path of a high-throughput device (NVMe, GPU DMA) eliminates the spinlock entirely from the fast path.

The design must handle: producers running in hardirq/softirq context, consumers running in process context, and arbitrary CPU count without ABA problem.

---

## 2. Requirements

### 2.1 Functional Requirements
- MPMC ring buffer: N producers (GPU DMA engines, NIC RX queues), M consumers (completion threads).
- Wait-free producers (never spin or block — critical for hardirq context).
- Lock-free consumers (progress guaranteed despite preemption/failure).
- Correct ordering: `WRITE_ONCE` / memory barriers per architecture.
- Configurable capacity (power-of-two for fast modulo via bitwise AND).
- Safe for use across CPUs without any spinlock in the enqueue/dequeue hot path.

### 2.2 Non-Functional Requirements
- Enqueue latency (producer): < 50 ns on the fast path.
- Dequeue throughput: > 50 million ops/sec per core (NVMe-class).
- Memory overhead: < 32 bytes per slot overhead (excluding payload).
- No ABA problem under any interleaving.

---

## 3. Constraints & Assumptions

- x86-64 architecture: TSO (Total Store Order) memory model.
- Slots hold a fixed-size entry (e.g., 64 bytes: completion descriptor).
- Ring size is a power of two (64 to 65536 entries).
- Linux kernel: `atomic64_t`, `READ_ONCE()`, `WRITE_ONCE()`, `smp_mb()`, `smp_wmb()`, `smp_rmb()`.
- No `cmpxchg128` required — design uses 64-bit CAS only.

---

## 4. Architecture Overview

```
  Producers (hardirq / softirq / DMA engine)
  ┌─────────┐  ┌─────────┐  ┌─────────┐
  │ GPU Eng0 │  │ GPU Eng1 │  │ NIC RX  │
  └────┬─────┘  └────┬─────┘  └────┬────┘
       │              │              │
       ▼              ▼              ▼
  ┌────────────────────────────────────────┐
  │        MPMC Lock-Free Ring Buffer      │
  │                                        │
  │  head (consumer index) ──► [slot 0]    │
  │                             [slot 1]   │
  │                             [slot 2]   │
  │  tail (producer index) ──► [slot N-1]  │
  │                                        │
  │  Each slot:                            │
  │    sequence_number (atomic64)          │
  │    payload[56 bytes]                   │
  └────────────────────────────────────────┘
       │              │              │
       ▼              ▼              ▼
  ┌─────────┐  ┌─────────┐  ┌─────────┐
  │ Comp Thr │  │ Comp Thr │  │ Comp Thr │
  └─────────┘  └─────────┘  └─────────┘
  Consumers (process context kernel threads)
```

---

## 5. Core Data Structures

### 5.1 Ring Buffer Slot

```c
/* Each slot is exactly one cache line (64 bytes) */
struct rb_slot {
    atomic64_t  sequence;          /* monotonically increasing sequence number */
    u8          payload[56];       /* completion descriptor (configurable) */
} ____cacheline_aligned;           /* __attribute__((aligned(64))) */
```

### 5.2 Ring Buffer Descriptor

```c
struct lockfree_ring {
    /* Producer state — separate cache line from consumer state */
    atomic64_t  enqueue_pos ____cacheline_aligned;  /* next slot to claim */

    /* Consumer state — separate cache line */
    atomic64_t  dequeue_pos ____cacheline_aligned;  /* next slot to read */

    /* Ring metadata — separate cache line */
    u32         capacity;      /* must be power of two */
    u32         mask;          /* capacity - 1, for fast modulo */

    /* Slot array (page-aligned, allocated with vmalloc) */
    struct rb_slot *slots;
};
```

**Critical:** `enqueue_pos` and `dequeue_pos` are on separate cache lines. On x86 TSO, false sharing between the producer increment and consumer increment would serialize them through the cache coherence protocol.

---

## 6. Key Algorithms & Design Decisions

### 6.1 Dmitry Vyukov MPMC Ring Buffer (Kernel Adaptation)

This is the canonical lock-free MPMC algorithm. Each slot has a `sequence` field that acts as a generation counter — eliminating the ABA problem without double-width CAS.

**Enqueue (producer — wait-free on fast path):**
```c
int lockfree_ring_enqueue(struct lockfree_ring *rb, const void *payload, size_t len)
{
    struct rb_slot *slot;
    u64             pos, seq, diff;

    pos = atomic64_read(&rb->enqueue_pos);

    for (;;) {
        slot = &rb->slots[pos & rb->mask];
        seq  = atomic64_read(&slot->sequence);
        diff = (s64)(seq - pos);

        if (diff == 0) {
            /* Slot is free: try to claim it */
            if (atomic64_cmpxchg(&rb->enqueue_pos, pos, pos + 1) == pos)
                break;            /* claimed */
            /* CAS failed — another producer claimed it, retry */
            pos = atomic64_read(&rb->enqueue_pos);
        } else if (diff < 0) {
            /* Ring is full (consumer hasn't caught up) */
            return -ENOBUFS;
        } else {
            /* Another producer just claimed this slot but hasn't published;
               advance pos and retry */
            pos = atomic64_read(&rb->enqueue_pos);
        }
    }

    /* We own the slot — write payload */
    memcpy(slot->payload, payload, len);

    /* Publish: advance sequence to pos+1, making it visible to consumers */
    smp_wmb();                              /* ensure payload written before seq */
    atomic64_set(&slot->sequence, pos + 1);

    return 0;
}
```

**Dequeue (consumer — lock-free):**
```c
int lockfree_ring_dequeue(struct lockfree_ring *rb, void *payload, size_t len)
{
    struct rb_slot *slot;
    u64             pos, seq, diff;

    pos = atomic64_read(&rb->dequeue_pos);

    for (;;) {
        slot = &rb->slots[pos & rb->mask];
        seq  = atomic64_read(&slot->sequence);
        diff = (s64)(seq - (pos + 1));

        if (diff == 0) {
            /* Slot is ready: try to claim it */
            if (atomic64_cmpxchg(&rb->dequeue_pos, pos, pos + 1) == pos)
                break;
            pos = atomic64_read(&rb->dequeue_pos);
        } else if (diff < 0) {
            /* Ring is empty */
            return -ENODATA;
        } else {
            pos = atomic64_read(&rb->dequeue_pos);
        }
    }

    smp_rmb();                              /* ensure seq read before payload */
    memcpy(payload, slot->payload, len);

    /* Release slot back to producers: advance sequence to pos + capacity */
    atomic64_set(&slot->sequence, pos + rb->capacity);

    return 0;
}
```

### 6.2 Why This Avoids the ABA Problem

The `sequence` field is a monotonically increasing counter. When a slot is freed by a consumer, its `sequence` is set to `pos + capacity` — not back to the original value. So a stale CAS will see `diff != 0` and retry, never mistaking a recycled slot for a free one.

### 6.3 Memory Ordering on x86 TSO vs ARM

| Architecture | Store ordering | Load ordering | Required barriers |
|---|---|---|---|
| x86-64 (TSO) | Stores are sequentially consistent w.r.t. other stores | Loads don't reorder w.r.t. earlier loads | Only compiler barriers needed (`WRITE_ONCE`/`READ_ONCE`) for most ops |
| ARM64 (weak) | Stores can reorder | Loads can reorder | `dmb ishst` (store barrier), `dmb ishld` (load barrier) needed |

Linux abstracts this with:
- `smp_wmb()` — store-store barrier (no-op on x86, `dmb ishst` on ARM)
- `smp_rmb()` — load-load barrier (no-op on x86, `dmb ishld` on ARM)
- `READ_ONCE()` / `WRITE_ONCE()` — prevent compiler reordering (always needed)

### 6.4 Producer Preemption Safety

In the kernel, producers may be in hardirq context. The enqueue function uses only `atomic64_cmpxchg` and `atomic64_set` — both are lock-free and safe in any context. No sleeping, no spinlock, no `preempt_disable()` needed.

However: if the producer is preempted **after** claiming the slot (CAS succeeded) but **before** writing the payload and updating `sequence`, other producers can still proceed (they won't touch this slot). Consumers will spin briefly on `diff == 0` for this specific slot — this is bounded by scheduler quantum. This is acceptable (lock-free, not wait-free for consumers).

### 6.5 Kernel's io_uring Ring Buffer (Real-World Analog)

`io_uring` uses two separate SPSC rings (submission queue SQ + completion queue CQ) shared between user and kernel. It uses `smp_load_acquire` / `smp_store_release` pairs for head/tail, and `READ_ONCE` / `WRITE_ONCE` for entries. Our MPMC design generalizes this to multiple producers/consumers.

### 6.6 Initialization

```c
struct lockfree_ring *lockfree_ring_alloc(u32 capacity)
{
    struct lockfree_ring *rb;
    u32 i;

    BUG_ON(!is_power_of_2(capacity));   /* enforce at boot, not runtime */
    rb = kzalloc(sizeof(*rb), GFP_KERNEL);
    rb->slots = vzalloc(capacity * sizeof(struct rb_slot));
    rb->capacity = capacity;
    rb->mask     = capacity - 1;

    for (i = 0; i < capacity; i++)
        atomic64_set(&rb->slots[i].sequence, i);   /* initial sequence = slot index */

    atomic64_set(&rb->enqueue_pos, 0);
    atomic64_set(&rb->dequeue_pos, 0);
    return rb;
}
```

---

## 7. Trade-off Analysis

| Decision | Chosen | Alternative | Reason |
|---|---|---|---|
| Sequence counter per slot | Yes | Head/tail bitmask | Per-slot sequence eliminates ABA without 128-bit CAS |
| Cache-line alignment per slot | Yes | Packed slots | False sharing at scale kills throughput; 64B aligned = one slot per cache line |
| enqueue/dequeue pos on separate lines | Yes | Shared struct | Producer/consumer CAS compete on different addresses |
| Wait-free producer | Full ring returns -ENOBUFS | Producer spins/waits | Spinning in hardirq context is unsafe |
| Power-of-two capacity | Yes | Arbitrary size | `pos & mask` replaces `pos % capacity` — eliminates division in hot path |
| `memcpy` payload | Yes | Pointer-based | Avoids payload lifetime issues in interrupt context |

---

## 8. Real Linux Kernel References

| Component | Source | Symbol |
|---|---|---|
| io_uring rings | `io_uring/io_uring.c` | `io_get_sqe()`, `io_cqring_add_event()` |
| Kernel SPSC kfifo | `include/linux/kfifo.h` | `kfifo_in()`, `kfifo_out()` |
| Atomic operations | `include/linux/atomic.h` | `atomic64_cmpxchg()`, `atomic64_set()` |
| Memory barriers | `include/asm-generic/barrier.h` | `smp_wmb()`, `smp_rmb()`, `smp_mb()` |
| Cache line macros | `include/linux/cache.h` | `____cacheline_aligned`, `L1_CACHE_BYTES` |
| DMA completion ring | `drivers/nvme/host/nvme.c` | NVMe CQ ring (MMIO-based SPSC) |
| Bpf ring buffer | `kernel/bpf/ringbuf.c` | `bpf_ringbuf_reserve()`, `bpf_ringbuf_commit()` |

---

## 9. Failure Modes & Debug Strategies

### 9.1 Ring Buffer Full (Producer Backpressure)
**Scenario:** Consumer threads are slow; producers fill the ring.
**Detection:** `lockfree_ring_enqueue()` returns `-ENOBUFS` more than N times/sec.
**Fix:** Increase ring size (power-of-two), add consumer threads, or implement producer backpressure via `ENOBUFS` count in a per-CPU counter.

### 9.2 Sequence Corruption (Bug: Missing `WRITE_ONCE`)
**Scenario:** Without `WRITE_ONCE`, compiler can cache the sequence in a register → consumer never sees update.
**Debug:** `CONFIG_KCSAN` (Kernel Concurrency Sanitizer) — detects data races without `READ_ONCE`/`WRITE_ONCE`.
```bash
echo "CONFIG_KCSAN=y" >> .config
# Run workload; KCSAN prints race reports to dmesg
```

### 9.3 Starvation of Consumer
**Scenario:** Producers continuously overwrite a consumer's claimed slot (bug).
**Verification:** This cannot happen in the design — once a consumer claims `dequeue_pos` via CAS, producers cannot touch that slot until the consumer releases it (sets `sequence = pos + capacity`).

### 9.4 False Sharing Verification
```bash
perf stat -e cache-misses,cache-references ./workload
# High cache-miss ratio on ring-buffer operations → check alignment
# perf c2c record ./workload; perf c2c report
```

---

## 10. Performance Considerations

- **Cache-line padding between enqueue/dequeue pos:** Without it, every enqueue invalidates the consumer's cache line and vice versa — a 10x throughput regression in benchmarks.
- **Batched dequeue:** Instead of dequeuing one item at a time, claim a range of positions with a single CAS (`dequeue_pos += batch_size`) — amortizes CAS overhead.
- **NUMA-local ring:** If producers are on NUMA node 0 and consumers on node 1, the ring buffer's `enqueue_pos` cache line bounces across nodes. Prefer producer and consumer on the same NUMA node; if impossible, use a per-NUMA-node intermediary ring.
- **Huge pages for slot array:** `vmalloc` allocates 4K pages; for large rings (65536 × 64B = 4MB), use `alloc_pages(GFP_KERNEL, get_order(size))` to get a contiguous physical allocation → fewer TLB entries.

---

## 11. Interview Answer Strategy (NVIDIA 10-yr Level)

**What they want to hear:**
1. The ABA problem and why per-slot sequence counters solve it without `cmpxchg16b`.
2. x86 TSO vs ARM weak memory model — and that `READ_ONCE`/`WRITE_ONCE` are always needed (compiler).
3. Why you cannot use a spinlock in hardirq context and what the actual alternatives are.
4. Cache-line alignment of head/tail — producer-consumer false sharing is the #1 perf mistake.
5. Reference to `io_uring`'s SQ/CQ ring as a production example in the Linux kernel.
6. BPF ring buffer (`kernel/bpf/ringbuf.c`) as another kernel SPSC example.
7. KCSAN for detecting missing `READ_ONCE`/`WRITE_ONCE` annotations.
