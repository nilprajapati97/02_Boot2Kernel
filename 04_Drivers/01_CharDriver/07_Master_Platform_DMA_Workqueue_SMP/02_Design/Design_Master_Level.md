# Design: Master Level — Platform + DMA Engine + Workqueue + SMP
## Level 07 | End-to-End Deep Design from Scratch

---

## 1. What Makes This "Master Level"

This level combines all real-world production driver techniques into one coherent system:

| Component | Purpose |
|-----------|---------|
| Platform driver + DT | Hardware abstraction and probing |
| DMA Engine API | Async hardware transfers via standard kernel DMA framework |
| Workqueue (ordered) | Deferred processing in process context with ordering guarantees |
| Threaded IRQ | Split interrupt handling for latency and safety |
| kfifo + spinlock | High-speed data path with correct SMP locking |
| `completion` | Single-use synchronization primitive for DMA wait |
| poll/select | Async I/O readiness for user-space |

---

## 2. DMA Engine API — The Right Way to Use DMA

### Why DMA Engine API vs Direct DMA?

Level 04 used `dma_alloc_coherent` directly — this is **bare DMA**: you program HW registers yourself. The **DMA Engine API** (`dmaengine`) abstracts multiple DMA controllers:

```
Your driver → dmaengine API → DMA controller driver → HW DMA engine
                                    ↑
                         PL330, QCOM BAM, EDMA, etc.
```

Benefits:
- Works across any DMA controller
- Handles IOMMU mapping automatically
- Provides async callback on completion
- Supports scatter-gather, cyclic, interleaved transfer modes

### DMA Channel Lifecycle

```c
/* 1. Request channel (from DT "dmas" property or by capability) */
d->dma_chan = dma_request_chan(&pdev->dev, "tx");
/* DT: dmas = <&dma_ctrl 5>;  dma-names = "tx"; */

/* 2. Prepare transfer descriptor */
desc = dmaengine_prep_dma_memcpy(chan, dst_pa, src_pa, len, flags);

/* 3. Set completion callback */
desc->callback       = my_dma_complete;
desc->callback_param = my_data;

/* 4. Submit to pending queue */
cookie = dmaengine_submit(desc);

/* 5. Issue all pending transfers */
dma_async_issue_pending(chan);

/* 6. Wait (option A: completion) */
wait_for_completion(&dma_done);

/* 7. Cleanup on remove */
dmaengine_terminate_sync(chan);
dma_release_channel(chan);
```

### Transfer Types

| Function | Use case |
|---------|----------|
| `dmaengine_prep_dma_memcpy` | Memory to memory copy |
| `dmaengine_prep_slave_sg` | Scatter-gather from/to peripheral |
| `dmaengine_prep_dma_cyclic` | Continuous circular DMA (audio) |
| `dmaengine_prep_interleaved_dma` | 2D image/video DMA |

---

## 3. `completion` — One-Shot Synchronization

```c
struct completion dma_done;
init_completion(&dma_done);

/* Waiter (driver read/write path) */
wait_for_completion_timeout(&dma_done, HZ);  /* HZ = 1 second */

/* Completer (DMA callback, IRQ) */
complete(&dma_done);
```

### How It Differs from Wait Queue

| | `completion` | `wait_queue` |
|--|------------|-------------|
| Use case | One-shot event | Recurring events |
| Edge vs level | Edge (completes once) | Level (condition checked) |
| API | `complete()` / `wait_for_completion()` | `wake_up()` / `wait_event()` |

Use `completion` for: DMA done, firmware load complete, probe synchronization.
Use `wait_queue` for: data available, buffer not full (recurring conditions).

---

## 4. Workqueue — Bottom-Half Architecture

### The Three Levels of Interrupt Deferral

```
Hardware IRQ fires
      │
      ▼
Top-half (hardirq) — absolutely minimal
│  • acknowledge interrupt
│  • return IRQ_WAKE_THREAD or IRQ_HANDLED
│
      ▼
Bottom-half — choose one:
│
├── Softirq    — very fast, preemptible by hardirq, no sleep, no mutex
├── Tasklet    — built on softirq, runs once, no sleep
└── Workqueue  — runs in kernel thread, CAN SLEEP, can use mutex/kmalloc
```

### Why Ordered Workqueue

```c
/* System workqueue — shared, can be starved by other drivers */
schedule_work(&work);

/* Dedicated ordered workqueue — guaranteed ordering, not shared */
d->wq = alloc_ordered_workqueue("%s_wq", WQ_MEM_RECLAIM, DRIVER_NAME);
queue_work(d->wq, &work);
```

`alloc_ordered_workqueue`: each submitted work runs one-at-a-time in submission order. Critical when data ordering matters (DMA packets must be processed in order).

### Work Item Pattern

```c
struct my_work {
    struct work_struct work;   /* must be first or use container_of */
    void   *data;
    size_t  len;
    struct  my_dev *dev;       /* back-pointer */
};

/* Initialization */
INIT_WORK(&item->work, my_work_handler);

/* Submission (from IRQ bottom-half) */
queue_work(d->wq, &item->work);

/* Handler (runs in workqueue thread) */
void my_work_handler(struct work_struct *work) {
    struct my_work *item = container_of(work, struct my_work, work);
    /* Can sleep, use mutex, allocate memory */
}
```

---

## 5. Full Data Flow: Write Path with DMA + Workqueue

```
User: write(fd, data, 16384)
         │
         ▼
master_write()
  ├─ copy_from_user(dma_src, ubuf, len)    ← user → DMA src buffer
  ├─ dmaengine_prep_dma_memcpy(...)        ← prepare descriptor
  ├─ dmaengine_submit(desc)               ← queue transfer
  ├─ dma_async_issue_pending(chan)         ← kick DMA engine
  ├─ wait_for_completion_timeout(...)     ← sleep until done
  │
  │   [DMA engine transfers: dma_src → dma_dst independently]
  │
  │   dma_complete_callback():
  │     complete(&dma_done)               ← wake writer
  │
  ├─ queue_work(wq, &work_item)           ← schedule heavy processing
  │
  │   [workqueue thread: dma_work_handler()]
  │     kfifo_in(fifo, dma_dst, len)
  │     wake_up(read_wq)
  │
  └─ return len                           ← user write() returns
```

---

## 6. IRQ Architecture: Top-Half + Bottom-Half + Workqueue

```
Hardware IRQ
      │
      ▼
master_irq_top() [hardirq context]
│  Constraints: no sleep, no mutex, no kmalloc
│  Just: return IRQ_WAKE_THREAD
      │
      ▼
master_irq_bottom() [kernel thread, IRQF_ONESHOT]
│  Constraints: can use spinlock, no mutex that sleeps
│  Action: queue_work(d->wq, &work_item)
│          return IRQ_HANDLED
      │
      ▼
dma_work_handler() [workqueue thread, fully preemptible]
│  Constraints: none — can sleep, use mutex, kmalloc
│  Action: kfifo_in(), wake_up_interruptible()
```

`IRQF_ONESHOT`: IRQ line stays masked until `master_irq_bottom()` returns. Prevents interrupt storm if HW keeps asserting IRQ before thread handles it.

---

## 7. Locking Map

```
Context          Can use              Cannot use
─────────────────────────────────────────────────────
hardirq top      spinlock (irqsave)   mutex, kmalloc(GFP_KERNEL)
threaded bottom  spinlock, mutex      (can use mutex if non-atomic)
workqueue        spinlock, mutex      nothing restricted
process read/write  spinlock, mutex   (normal process context)
```

In this driver:
- `fifo_lock` (spinlock): protects kfifo — used in IRQ bottom-half and process context
- No mutex in hardirq top-half — just return `IRQ_WAKE_THREAD`
- `dma_busy` (atomic): lock-free busy flag

---

## 8. `flush_workqueue` vs `drain_workqueue`

On `remove()`:
```c
flush_workqueue(d->wq);    /* wait for all pending work to complete */
destroy_workqueue(d->wq);  /* destroy after flush */
```

`flush` ensures no work is mid-execution when we free resources. Without `flush`, workqueue thread could be accessing freed memory after `kfree(d)`.

---

## 9. DMA Channel From Device Tree

```dts
peripheral@1000000 {
    compatible = "myvendor,master-device";
    reg = <0x0 0x1000000 0x0 0x1000>;
    dmas = <&dma_ctrl 5>, <&dma_ctrl 6>;
    dma-names = "tx", "rx";
    interrupts = <GIC_SPI 50 IRQ_TYPE_LEVEL_HIGH>;
};
```

```c
/* In probe: */
d->dma_tx = dma_request_chan(&pdev->dev, "tx");
d->dma_rx = dma_request_chan(&pdev->dev, "rx");
```

---

## 10. Summary Table

| Mechanism | Why Used | Key API |
|-----------|---------|---------|
| DMA Engine | Standard DMA abstraction | `dmaengine_prep_dma_memcpy` |
| `completion` | One-shot DMA done signal | `wait_for_completion_timeout` |
| Ordered workqueue | Deferred processing, in-order | `alloc_ordered_workqueue` |
| Threaded IRQ | Top+bottom split, can use mutex | `request_threaded_irq` |
| `IRQF_ONESHOT` | Prevent interrupt storm | Flag in `request_threaded_irq` |
| `flush_workqueue` | Safe shutdown order | Before `destroy_workqueue` |
| kfifo + spinlock | High-speed data path | `kfifo_in/out` |
