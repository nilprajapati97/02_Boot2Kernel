# Q14 — Design a Block I/O Scheduler for SSD + GPU Workloads

---

## 1. Problem Statement

The Linux block I/O scheduler sits between the filesystem and the block device driver. It reorders and merges requests to maximize throughput and minimize latency. However, SSDs and NVMe expose different performance characteristics than HDDs:
- No rotational latency — seek time is near-zero.
- Parallel internal queues — NVMe supports up to 64K queues, each with 64K depth.
- QoS differentiation needed — GPU checkpoint writes shouldn't block model loading reads.

Design a block I/O scheduler that handles NVMe SSD + GPU I/O workloads optimally.

---

## 2. Requirements

### 2.1 Functional Requirements
- Submit I/O requests to NVMe device via hardware queues (blk-mq multi-queue).
- Merge adjacent requests to reduce I/O count.
- Prioritize read I/Os over write I/Os for GPU workloads (reads are latency-critical).
- I/O throttling via cgroup blkio controller.
- Write-back coalescing: batch dirty page writes into sequential streams.
- Real-time I/O deadlines: guarantee < 1 ms latency for critical reads.

### 2.2 Non-Functional Requirements
- NVMe throughput: achieve > 95% of device-rated IOPS (1M IOPS @ 4KB).
- P99.9 read latency: < 200 µs for 4KB random reads.
- CPU overhead of scheduler: < 1% per CPU core at 1M IOPS.
- Zero scheduler overhead for NVMe (pass-through `none` scheduler option).

---

## 3. Constraints & Assumptions

- NVMe SSD with 32 hardware queues, queue depth 1024.
- Linux blk-mq (multi-queue block layer), default since kernel 5.0.
- Two schedulers evaluated: `kyber` (for NVMe) and `bfq` (for HDDs + mixed).
- `io_uring` as the primary userspace I/O submission interface.

---

## 4. Architecture Overview

```
  User Space: io_uring / libaio / pread()
  │
  ▼
  VFS / Page Writeback
  │
  ▼
  ┌──────────────────────────────────────────────────────────────┐
  │              Block Layer (blk-mq)                           │
  │                                                              │
  │  ┌─────────────────┐  ┌────────────────────────────────┐   │
  │  │  Software Queue  │  │  I/O Scheduler (per-CPU)       │   │
  │  │  (ctx->rq_list) │  │  kyber / mq-deadline / bfq     │   │
  │  │  per-CPU submit  │  │  Merge, reorder, prioritize    │   │
  │  └─────────────────┘  └────────────────────────────────┘   │
  │                                │                             │
  │                                ▼                             │
  │  ┌─────────────────────────────────────────────────────┐   │
  │  │          Hardware Dispatch Queues (hctx)             │   │
  │  │  [HW Queue 0] [HW Queue 1] ... [HW Queue 31]        │   │
  │  │   depth=1024   depth=1024        depth=1024          │   │
  │  └─────────────────────────────────────────────────────┘   │
  └──────────────────────────────────────────────────────────────┘
  │
  ▼
  NVMe Driver → NVMe Submission Queue → SSD
```

---

## 5. Core Data Structures

### 5.1 Block Request

```c
struct request {
    struct request_queue  *q;
    struct blk_mq_ctx     *mq_ctx;   /* per-CPU software queue context */
    struct blk_mq_hw_ctx  *mq_hctx; /* hardware queue context */

    unsigned int           cmd_flags;  /* REQ_OP_READ, REQ_OP_WRITE, REQ_SYNC */
    unsigned int           __data_len; /* total data length */
    sector_t               __sector;   /* start sector */

    struct bio            *bio;        /* head of bio list */
    struct bio            *biotail;    /* tail of bio list */

    struct list_head       queuelist;  /* scheduler queue linkage */
    u64                    start_time_ns; /* for latency stats */

    /* I/O priority */
    unsigned short         ioprio;     /* IOPRIO_CLASS_RT, IOPRIO_CLASS_BE, etc. */
};
```

### 5.2 Bio (Block I/O Unit from Filesystem)

```c
struct bio {
    struct block_device  *bi_bdev;
    unsigned int          bi_opf;       /* op: REQ_OP_READ, REQ_OP_WRITE */
    unsigned short        bi_flags;
    sector_t              bi_iter.bi_sector;  /* start sector */
    unsigned int          bi_iter.bi_size;    /* total size */
    struct bvec_iter      bi_iter;
    bio_end_io_t         *bi_end_io;
    void                 *bi_private;
    struct bio_vec        bi_inline_vecs[];   /* scatter-gather segments */
};
```

### 5.3 Kyber Scheduler State

```c
struct kyber_queue_data {
    struct request_queue *q;

    /* Per-domain (read/write/discard) token buckets */
    struct sbitmap_queue domain_tokens[KYBER_NUM_DOMAINS];
    /* KYBER_READ_DOMAIN, KYBER_WRITE_DOMAIN, KYBER_DISCARD_DOMAIN */

    /* Dispatch queue per domain */
    struct list_head rqs[KYBER_NUM_DOMAINS];

    /* Latency targets */
    u64 read_lat_nsec;   /* target read latency (default 2ms) */
    u64 write_lat_nsec;  /* target write latency (default 10ms) */

    /* Adaptive token allocation based on measured latency */
    spinlock_t lock;
    unsigned int good_buckets[KYBER_LATENCY_BUCKETS];
};
```

---

## 6. Key Algorithms & Design Decisions

### 6.1 blk-mq — Multi-Queue Architecture

Traditional single-queue block layer had a global `request_queue->queue_lock` — bottleneck at > 200K IOPS.

blk-mq eliminates this with:
- **Software queues:** One per-CPU (`blk_mq_ctx`). Requests enqueued here without cross-CPU locking.
- **Hardware queues:** One per NVMe queue (`blk_mq_hw_ctx`). CPUs map many-to-one to hardware queues.
- **Dispatch:** Per-CPU software queue → scheduler → hardware queue → NVMe submission ring.

```c
blk_mq_submit_bio(bio):
    ctx = blk_mq_get_ctx(q)           /* per-CPU, no lock */
    hctx = blk_mq_map_queue(q, bio)   /* map CPU to HW queue */

    rq = blk_mq_get_request(q, bio, ...)
    blk_mq_bio_to_request(rq, bio)

    if (q->elevator):                  /* if scheduler attached */
        q->elevator->type->ops.insert_requests(hctx, &rq_list, ...)
    else:
        blk_mq_try_issue_directly(hctx, rq)  /* NVMe: skip scheduler */
```

### 6.2 Request Merging — Reducing I/O Count

Before inserting into the scheduler queue, try to merge with an existing request:
```c
blk_attempt_plug_merge(q, bio, &request_count):
    /* Search plug list (per-task list of recently submitted requests) */
    for each pending rq in current->plug->mq_list:
        if bio->sector == rq->sector + rq->nr_sectors:  /* front merge */
            bio_list_add_head(&rq->bio_list, bio)
            return true
        if bio->sector + bio_sectors(bio) == rq->sector:  /* back merge */
            bio_list_add(&rq->bio_list, bio)
            return true
    return false   /* no merge: insert as new request */
```

**Merge impact:** For sequential writes (ML checkpoint), merging 128 × 4KB writes into one 512KB request reduces NVMe submission overhead by 128×.

### 6.3 Kyber Scheduler — Token Bucket QoS

Kyber uses token buckets to ensure reads get low latency while writes get throughput:

```
Each domain (read/write) has a token count.
When a request is dispatched:
    if tokens[domain] > 0:
        dispatch request, decrement token
    else:
        wait (request stays in queue)

Token replenishment:
    Every dispatch_budget_timer fires:
        if measured_latency[READ] > read_lat_target:
            increase read tokens (give reads more priority)
            decrease write tokens
        else:
            rebalance toward write throughput
```

This is **adaptive**: under GPU model loading (read-heavy), Kyber automatically biases toward reads. During checkpoint writing (write-heavy), it shifts toward write throughput.

### 6.4 mq-deadline — Deadline-Based Scheduling

For mixed SSD + HDD environments:
```c
struct deadline_data {
    struct rb_root sort_list[2];    /* RB-tree sorted by sector [READ, WRITE] */
    struct list_head fifo_list[2];  /* FIFO sorted by deadline [READ, WRITE] */

    sector_t last_sector[2];        /* last dispatched sector per domain */
    unsigned int front_merges;

    /* Deadline tunables */
    int fifo_expire[2];   /* read: 500ms, write: 5000ms defaults */
    int fifo_batch;        /* max requests to dispatch in one batch */
    int writes_starved;    /* max write starvations before forced dispatch */
};
```

Dispatch policy:
1. Check FIFO list: if oldest request has expired its deadline → dispatch it (prevent starvation).
2. Otherwise: dispatch from `sort_list` in sector order (exploits SSD parallelism; reduces seek on HDD).

### 6.5 BFQ (Budget Fair Queuing) — For Desktops/Mixed Workloads

BFQ assigns a "budget" (number of sectors) to each process. A process with a large budget (background backup) is pre-empted when a process with a small budget + latency sensitivity (interactive app) needs I/O. This ensures interactive applications are never starved by bulk writers.

BFQ also tracks sequential access patterns per process — sequential processes get larger budgets (fewer preemptions).

### 6.6 NVMe Pass-Through (`none` scheduler)

For NVMe at 1M IOPS, any scheduler adds latency. For NVMe-native workloads (io_uring direct I/O):
```bash
echo none > /sys/block/nvme0n1/queue/scheduler
# No reordering, no merging — each bio goes directly to NVMe HW queue
# NVMe device's internal scheduler handles parallelism
```

This is appropriate when:
- I/O pattern is already random (no benefit from reordering).
- io_uring handles batching at application level.
- NVMe has multiple queues and can saturate them without help.

---

## 7. Trade-off Analysis

| Scheduler | Best For | Latency | Throughput | CPU Overhead |
|---|---|---|---|---|
| `none` | NVMe, io_uring, GPU checkpoint | Lowest | Highest | Lowest |
| `kyber` | NVMe with mixed read/write QoS | Low | High | Low |
| `mq-deadline` | NVMe + HDD mixed, deadline requirements | Medium | Medium | Medium |
| `bfq` | Desktop/laptop, interactive + bulk mix | Adaptive | Lower | Higher |

---

## 8. Real Linux Kernel References

| Component | Source | Symbol |
|---|---|---|
| blk-mq core | `block/blk-mq.c` | `blk_mq_submit_bio()`, `blk_mq_dispatch_rq_list()` |
| Request merging | `block/blk-merge.c` | `blk_attempt_plug_merge()`, `bio_attempt_back_merge()` |
| Kyber scheduler | `block/kyber-iosched.c` | `kyber_dispatch_requests()`, `kyber_latency_exceeded()` |
| mq-deadline | `block/mq-deadline.c` | `dd_dispatch_request()`, `dd_insert_request()` |
| BFQ | `block/bfq-iosched.c` | `bfq_dispatch_request()`, `bfq_queue_budget_left()` |
| bio structure | `include/linux/blk_types.h` | `struct bio`, `bio_for_each_segment()` |
| I/O priority | `include/uapi/linux/ioprio.h` | `IOPRIO_CLASS_RT`, `ioprio_set()` |
| blk-mq hw queue | `include/linux/blk-mq.h` | `struct blk_mq_hw_ctx`, `blk_mq_run_hw_queue()` |

---

## 9. Failure Modes & Debug Strategies

### 9.1 I/O Stall (Request Queue Full)
```bash
iostat -x 1   # look for %util = 100% and await > threshold
cat /sys/block/nvme0n1/queue/nr_requests  # current queue depth
cat /proc/diskstats | awk '{print $1,$2,$3,$13}'  # in-flight I/Os
```

### 9.2 Read Latency Spike Under Write Pressure
```bash
# Kyber adaptive rebalancing takes 100-200ms to converge
# Temporary fix: increase read token weight
echo 2 > /sys/block/nvme0n1/queue/iosched/read_lat_nsec   # 2ms target
# Or: switch to mq-deadline with read_expire=100 (100ms)
```

### 9.3 Write Starvation (writes never dispatched)
```bash
# mq-deadline: check writes_starved counter
cat /sys/block/nvme0n1/queue/iosched/writes_starved
# If == writes_starved_limit: writes are being deprioritized
# Fix: reduce writes_starved limit or increase write_expire
```

### 9.4 Debug: tracing I/O submission path
```bash
blktrace -d /dev/nvme0n1 -o trace
blkparse trace.blktrace.* | head -100
# Output: per-event I/O lifecycle (Q=queued, G=get request, M=merged, D=dispatched, C=completed)
```

---

## 10. Performance Considerations

- **CPU-to-HW-queue mapping:** Ensure submission CPUs are on the same NUMA node as the NVMe PCIe root. Cross-NUMA PCIe submission adds ~200 ns per I/O.
- **Poll queues:** NVMe supports completion polling (no interrupt). For < 10 µs latency, use `io_uring` with poll mode and a dedicated spinning CPU.
- **Write-back ordering:** Page writeback submits writes in order of `writeback_index` — sequential by design. The block layer should not reorder writes to the same file.
- **IO priority inheritance:** GPU checkpoint writes should use `IOPRIO_CLASS_BE` (best-effort); model loading reads use `IOPRIO_CLASS_RT` (real-time) so Kyber biases tokens toward reads.

---

## 11. Interview Answer Strategy (NVIDIA 10-yr Level)

**What they want to hear:**
1. blk-mq architecture — per-CPU software queues eliminate the global lock.
2. `none` scheduler for NVMe — knowing when NOT to use a scheduler is as important.
3. Kyber token bucket — adaptive read/write QoS for mixed GPU workloads.
4. Request merging: plug list + elevator merge to convert 4K random into 512K sequential.
5. io_uring poll mode for ultra-low latency NVMe.
6. CPU-to-HW-queue NUMA mapping — PCIe locality matters.
7. `blktrace` / `blkparse` for I/O debugging — production-ready tool knowledge.
