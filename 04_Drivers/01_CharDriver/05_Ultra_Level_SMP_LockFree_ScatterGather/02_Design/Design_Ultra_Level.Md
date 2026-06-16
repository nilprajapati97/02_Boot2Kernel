# Design: Ultra Level — SMP + Lock-Free + Scatter-Gather DMA + RCU
## Level 05 | End-to-End Deep Design from Scratch

---

## 1. Why Ultra Level?

Levels 01–04 use mutexes and simple buffers. On a **Qualcomm SoC with 8 big.LITTLE cores at GHz speeds**, a mutex in the data path means:
- Cache line contention across cores
- Scheduler overhead from sleeping/waking
- Latency spikes in real-time data paths

Ultra level eliminates these with:
- **Memory barriers** instead of locks for ordering
- **Lock-free data structures** for IRQ↔process communication
- **Scatter-Gather DMA** for real hardware I/O
- **RCU** for zero-overhead config reads
- **Per-CPU** counters for zero-contention stats

---

## 2. SMP Memory Ordering — The Core Problem

On a multi-core system, **CPUs can reorder memory operations** for performance. This breaks assumptions in concurrent code.

### Example: Bug Without Barrier

```c
// CPU0 (writer/IRQ):
data[0] = 'X';          // write 1
head = 1;               // write 2

// CPU1 (reader):
if (head == 1)          // sees write 2
    c = data[0];        // might see OLD data[0]!
```

CPU1 could see `head = 1` before `data[0] = 'X'` because CPUs reorder stores.

### Fix: Memory Barriers

```c
// CPU0 (writer):
data[0] = 'X';
smp_wmb();    // WRITE barrier: all stores before this are visible before stores after
head = 1;

// CPU1 (reader):
if (head == 1) {
    smp_rmb();    // READ barrier: read head before reading data
    c = data[0];  // guaranteed: sees 'X'
}
```

### Barrier Types

| Barrier | Direction | Meaning |
|---------|-----------|---------|
| `smp_wmb()` | Store→Store | Prevent store reordering |
| `smp_rmb()` | Load→Load | Prevent load reordering |
| `smp_mb()` | Full | Prevent any reordering |
| `smp_store_release()` | Store + wmb | Combined store+barrier |
| `smp_load_acquire()` | Load + rmb | Combined load+barrier |

On x86 (TSO model), most barriers are no-ops (compiler fence only). On ARM64 (weakly ordered), they emit `DMB ISH` instructions.

---

## 3. Lock-Free Ring Buffer

### Design

```
head (producer)                tail (consumer)
    │                              │
    ▼                              ▼
[0][1][2][3][4][5][6][7][8][9][10][11]...
         ↑_______data here_______↑
         tail                   head
```

### Push (Producer — IRQ context)

```c
int h = atomic_read(&r->head);
int t = atomic_read(&r->tail);

if full: return false

r->data[h & MASK] = c;   // write data
smp_wmb();                // BARRIER: data before head update
atomic_set(&r->head, (h+1) & MASK);  // publish
```

### Pop (Consumer — process context)

```c
int h = atomic_read(&r->head);
int t = atomic_read(&r->tail);

if empty: return false

smp_rmb();                // BARRIER: read head before data
c = r->data[t & MASK];   // read data
smp_wmb();
atomic_set(&r->tail, (t+1) & MASK);
```

### Why No Lock?

Single-producer-single-consumer (SPSC) ring buffer is lock-free by design:
- Only producer touches `head`
- Only consumer touches `tail`
- Memory barriers ensure ordering
- Works correctly on all SMP architectures

For multi-producer: need CAS (`atomic_cmpxchg`) or lock.

---

## 4. `kfifo` — Kernel's Built-In FIFO

Linux provides `kfifo` — a production-tested, lock-free FIFO for SPSC use:

```c
DECLARE_KFIFO(kfifo_buf, char, 4096);  // declare + embed in struct
INIT_KFIFO(kfifo_buf);                 // initialize

/* Producer */
kfifo_in(&buf, data, len);

/* Consumer */
kfifo_out(&buf, data, len);

/* Check */
kfifo_len(&buf);       // bytes available
kfifo_is_empty(&buf);
kfifo_is_full(&buf);
```

For multi-producer/consumer: wrap with `spinlock_t fifo_lock`.

---

## 5. Scatter-Gather DMA

### Why SG-DMA?

Real data is often fragmented (network packets, file pages):
```
Page 0 [4KB] ─┐
Page 1 [4KB]  ├─ Logically contiguous, physically scattered
Page 2 [4KB] ─┘
```

Without SG: must allocate `3 * 4KB` physically contiguous buffer (impossible at high loads).

With SG-DMA:
```
DMA engine walks the scatter-gather list:
  Entry 0: paddr=0x1000000, len=4096
  Entry 1: paddr=0x3000000, len=4096  ← not contiguous!
  Entry 2: paddr=0x5000000, len=4096
```

### Setup Sequence

```c
sg_init_table(sgl, nents);          // initialize SG array

for each page:
    sg_set_buf(&sgl[i], vaddr, len) // bind virtual buffer to SG entry

dma_map_sg(dev, sgl, nents, dir)    // IOMMU maps all pages → DMA addresses

/* Program HW with SG list */
for each mapped entry:
    hw_addr = sg_dma_address(&sgl[i]);
    hw_len  = sg_dma_len(&sgl[i]);
    program_hw_dma_entry(hw_addr, hw_len);
```

### Cleanup

```c
dma_unmap_sg(dev, sgl, nents, dir);  // release IOMMU mappings
/* Then free individual pages */
```

---

## 6. RCU — Read-Copy-Update

### Problem

Config struct read millions of times/second in data path, updated rarely (ioctl):
- Mutex → data path slows down
- RCU → reads have **zero overhead**

### How RCU Works

```
Reader (data path):
    rcu_read_lock();           // tells kernel "I'm reading"
    cfg = rcu_dereference(ptr); // safely dereference RCU pointer
    use cfg->max_transfer;
    rcu_read_unlock();          // "done reading"

Writer (ioctl):
    new_cfg = kmalloc(...);     // allocate new version
    /* fill new_cfg */
    old_cfg = rcu_dereference_protected(ptr, ...);
    rcu_assign_pointer(ptr, new_cfg);  // atomic publish
    call_rcu(&old_cfg->rcu, free_fn);  // free OLD after all readers done
```

### The Grace Period

```
Readers existing at rcu_assign_pointer time:
    They still use old_cfg → safe
    They finish → rcu_read_unlock()

call_rcu defers free_fn until ALL pre-existing readers have called rcu_read_unlock()
    → free_fn runs → old_cfg freed

No reader ever sees a freed pointer.
```

### When to Use RCU

- Data read frequently, written rarely
- Readers cannot block (IRQ context, hot paths)
- Stale reads for brief period acceptable

---

## 7. Per-CPU Variables — Zero Contention Stats

```c
DEFINE_PER_CPU(struct ultra_cpu_stats, cpu_stats);

/* In driver (no lock needed) */
this_cpu_inc(cpu_stats.reads);     // increments THIS CPU's copy
this_cpu_add(cpu_stats.bytes_rx, n);

/* Read all CPUs (for ioctl status) */
unsigned long total_reads = 0;
int cpu;
for_each_possible_cpu(cpu)
    total_reads += per_cpu(cpu_stats, cpu).reads;
```

**Why no contention?** Each CPU has its own cache-line-aligned copy. No false sharing, no atomic instruction needed for increment.

---

## 8. Architecture Comparison

| Mechanism | Lock-based | Lock-free (this level) |
|-----------|-----------|----------------------|
| Ring buffer | mutex + memcpy | atomic + smp_wmb |
| Config reads | mutex_lock | rcu_read_lock (free) |
| Stats | atomic_inc (cache ping-pong) | per_cpu (zero contention) |
| DMA | single coherent buffer | scatter-gather list |
| Overhead | ~100ns per lock | ~1-5ns |

---

## 9. Key APIs Summary

| Category | API | Purpose |
|----------|-----|---------|
| Barriers | `smp_wmb/rmb/mb` | SMP memory ordering |
| Lock-free | `atomic_read/set` | Non-blocking counter ops |
| Kernel FIFO | `kfifo_in/out` | Lock-free SPSC queue |
| SG DMA | `sg_init_table`, `dma_map_sg` | Scatter-gather I/O |
| RCU | `rcu_read_lock`, `rcu_assign_pointer` | Lock-free config |
| Per-CPU | `this_cpu_inc`, `per_cpu` | Zero-contention stats |
