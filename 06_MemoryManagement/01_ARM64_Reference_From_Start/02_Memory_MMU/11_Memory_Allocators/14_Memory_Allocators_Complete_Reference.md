# Memory Allocators: Complete Interview Reference

**Category**: Linux Kernel Memory Allocators — Synthesis  
**Platform**: ARM64 (AArch64)

---

## 1. Allocator Decision Tree

```
"I need to allocate N bytes of kernel memory. Which allocator?"

             START
               │
               ▼
     Is it for DMA with a device?
     ┌─────────YES──────────┐
     │                      NO
     ▼                      │
  Large (>PAGE_SIZE)?        │
  ┌──YES──┐  ┌──NO──┐       │
  │       │  │      │       │
  ▼       │  ▼      │       │
dma_alloc  │ dma_pool_alloc  │
_coherent()│ (small DMA     │
(large     │  objects)       │
 contiguous)│               │
            │               ▼
            │     Is it physically contiguous?
            │     ┌────YES────┐    ┌────NO────┐
            │     │           │    │          │
            │     ▼           │    ▼          │
            │   < 8KB?     Large?  vmalloc()  │
            │  kmalloc()  alloc_pages()       │
            │  GFP_KERNEL   (order N)         │
            │              GFP_KERNEL         │
            │                                 │
            └─────────────────────────────────┘

Specific cases:
  Device MMIO registers: ioremap()
  Per-CPU data: alloc_percpu() or DEFINE_PER_CPU()
  Early boot (before buddy): memblock_alloc()
  Large physically contiguous (camera/video): CMA via dma_alloc_from_contiguous()
  Kernel object (same type, many instances): kmem_cache_create() + kmem_cache_alloc()
```

---

## 2. Allocator Hierarchy

```
Physical Memory (RAM)
  │
  ▼
memblock (early boot, before buddy)
  │ memblock_free_all() at boot
  ▼
Buddy Allocator (zone-based, order 0-10)
  │ alloc_pages() / free_pages()
  ├── Per-CPU page cache (pcppage) — order 0 fast path
  ├── GFP zone selection: DMA / DMA32 / Normal / Movable
  └── Migrate type: Unmovable / Movable / Reclaimable / CMA / HighAtomic
  │
  ├─→ SLUB Allocator (small objects, kmalloc)
  │     kmem_cache_alloc() / kmalloc()
  │     Per-CPU freelist (lock-free fast path)
  │     Per-node partial slab list (spinlock slow path)
  │
  ├─→ vmalloc (virtually contiguous, physically scattered)
  │     vmalloc() / vfree()
  │     vmap_area rb-tree
  │
  ├─→ ioremap (device MMIO)
  │     ioremap() / iounmap()
  │     Device memory attributes (nGnRE)
  │
  └─→ DMA Allocator
        dma_alloc_coherent() → CMA or direct alloc_pages
        dma_pool_alloc() → small DMA objects
        dma_map_single() → streaming (existing buffer)
        swiotlb → bounce buffers (32-bit DMA devices)
```

---

## 3. GFP Flag Quick Reference

```
Context           → Correct GFP Flag
──────────────────────────────────────
Interrupt handler → GFP_ATOMIC
Softirq / NAPI    → GFP_ATOMIC
Spinlock held     → GFP_ATOMIC
Mutex held        → GFP_KERNEL
Block I/O path    → GFP_NOIO
Filesystem path   → GFP_NOFS
User page alloc   → GFP_HIGHUSER_MOVABLE
32-bit DMA        → GFP_DMA32
Huge page (THP)   → GFP_TRANSHUGE
Early boot        → (memblock, not GFP)

Can sleep? → GFP_KERNEL
Cannot sleep? → GFP_ATOMIC
Need zero? → GFP_KERNEL | __GFP_ZERO (or vzalloc/kzalloc)
Must succeed? → GFP_KERNEL | __GFP_NOFAIL (⚠️ careful!)
Can fail silently? → GFP_KERNEL | __GFP_NOWARN
```

---

## 4. SLUB Internals Summary

```
SLUB kmem_cache structure:
  ┌─────────────────────────────────────────────────┐
  │  kmem_cache                                      │
  │  ├── cpu_slab: __percpu *kmem_cache_cpu          │
  │  │     ├── freelist: void** (current free chain) │
  │  │     ├── slab: *slab (current active slab)     │
  │  │     └── partial: *slab (CPU partial list)     │
  │  │                                               │
  │  ├── node[N]: *kmem_cache_node                   │
  │  │     ├── partial: list_head (node partial list)│
  │  │     └── list_lock: spinlock                   │
  │  │                                               │
  │  └── size/object_size/align/flags               │
  └─────────────────────────────────────────────────┘

Allocation speed:
  CPU freelist hit: 3 instructions (MRS TPIDR + load + cmpxchg)
  CPU partial hit:  set new slab, reload freelist (~10 instructions)
  Node partial hit: acquire spinlock, ~50 instructions
  New slab:         alloc_pages() from buddy → ~100+ instructions

Free space (freelist linked list in free objects):
  Object[0] freed: object[0].next = NULL (end of list)
  Object[1] freed: object[1].next = &object[0]
  object[2] freed: object[2].next = &object[1]
  freelist → object[2] → object[1] → object[0] → NULL
  
  Allocate: take object[2], set freelist = object[1]
  Free obj[3]: set obj[3].next = freelist = object[1]
               set freelist = &obj[3]
```

---

## 5. OOM Sequence

```
Memory pressure sequence (from allocation failure to OOM kill):

1. Process page fault / kmalloc:
   alloc_pages(GFP_KERNEL, 0) → fails
   │
2. Direct reclaim (synchronous):
   try_to_free_pages():
     shrink_lruvec(): scan LRU lists, try to free page cache, swap anonymous
     drop_slab(): shrink SLAB caches
     Retry allocation
   │ Still fails?
3. Memory compaction:
   try_to_compact_pages():
     compact_zone(): migrate movable pages to defragment
     Retry allocation
   │ Still fails?
4. OOM:
   out_of_memory():
     select_bad_process(): find highest oom_score victim
     oom_kill_process(): send SIGKILL
     wake_oom_reaper(): immediately unmap anonymous pages
   │
5. Retry allocation:
   After OOM kill: reap frees memory
   Allocation can now succeed

Signs of OOM in kernel logs:
  "Out of memory: Kill process %d (%s) score %d or sacrifice child"
  Followed by: killed process name, score, and top memory users
  
ARM64 Android OOM (different):
  lmkd (userspace): monitors /proc/meminfo
  Proactively kills background/cached processes BEFORE kernel OOM
  More responsive than kernel OOM killer
```

---

## 6. Top 20 Interview Q&A

**Q1: What's the maximum physically contiguous allocation from the buddy allocator?**
MAX_ORDER-1 = order 10 = 2^10 × 4KB = 4MB. Beyond this, use vmalloc (non-contiguous) or CMA (contiguous but reserved at boot).

**Q2: What is the minimum overhead of a SLUB object vs. requested size?**
With SLUB_DEBUG disabled: zero overhead — object slots are tightly packed, freelist chain uses the object memory itself (no separate metadata). Alignment padding may add waste (e.g., 10-byte object → 16-byte slot = 6 bytes waste).

**Q3: What is a "slab" in the SLUB allocator?**
A slab is a single buddy page (or multi-page block) divided into fixed-size object slots. The term "slab" comes from the original SLAB allocator design. In SLUB, struct slab overlays the struct page (uses page->private and page->flags fields). A slab is either "active" (owned by a CPU's cpu_slab), "partial" (some free slots, on node's partial list), or "full" (no free slots, not on any list).

**Q4: Can vmalloc'd memory be passed to a hardware DMA engine?**
Generally no. DMA requires physical addresses, and vmalloc memory has scattered physical pages. The only safe approach: call `vmalloc_to_page(va)` for each 4KB chunk to get the struct page, then build a scatter-gather list (`sg_list`), and use `dma_map_sg()`. With an IOMMU, this can work. Without IOMMU: the device must support scatter-gather DMA.

**Q5: What is the difference between ioremap() and ioremap_wc()?**
`ioremap()`: Device nGnRE attributes — no gathering, no reordering. For control registers requiring strict ordering. `ioremap_wc()`: Normal Non-Cacheable or Device GRE — CPU can combine writes into larger transactions. For frame buffers where write throughput matters and ordering within the buffer doesn't.

**Q6: When should you use dma_pool_alloc() vs. dma_alloc_coherent()?**
`dma_pool_alloc()`: for many small DMA objects (< PAGE_SIZE, like 16–64 byte descriptors). Avoids wasting a full 4KB page per descriptor. `dma_alloc_coherent()`: for larger DMA buffers (multi-page). Both return coherent (always-valid) DMA memory.

**Q7: How does CMA prevent the reserved memory from being wasted?**
CMA marks its region pages as `MIGRATE_CMA`. These pages participate in normal allocation as MOVABLE pages. User processes can use them for anonymous or file-backed mappings. When `cma_alloc()` is called, the pages in the region are migrated to other physical locations, freeing the contiguous region for the device. After `cma_release()`, pages return to `MIGRATE_CMA` and are again available for normal use.

**Q8: Explain GFP_NOIO and when to use it.**
`GFP_NOIO` = `GFP_RECLAIM` without `__GFP_IO`. Memory reclaim can sleep and try to free memory, but cannot start I/O operations (no `writepage`, no swap I/O). Use in block I/O paths where starting I/O to reclaim memory would cause the same I/O path to be re-entered (deadlock).

**Q9: What is the OOM reaper and why was it added (Linux 4.6+)?**
Before the OOM reaper: after `SIGKILL` was sent to the victim, the system had to wait for the process to be scheduled, run signal handlers, and exit normally before memory was freed. Under severe memory pressure, this scheduling delay could leave the system stuck for seconds or longer. The OOM reaper thread immediately walks the victim's page tables and unmaps/frees all anonymous pages without waiting for the process to exit, freeing memory in milliseconds.

**Q10: How does ARM64 TPIDR_EL1 enable lock-free per-CPU access?**
`TPIDR_EL1` holds the per-CPU base offset (distance from the `.data..percpu` template to this CPU's actual data chunk). `this_cpu_ptr(var)` compiles to: `MRS X1, TPIDR_EL1; ADD X0, X_var_template, X1`. The result is a pointer to this CPU's instance. No lock needed because: 1) Each CPU has its own instance (no sharing), 2) The offset is constant (no concurrent modification), 3) Within non-preemptible context, "this CPU" cannot change.

---

## 7. ARM64-Specific Allocator Differences

| Feature | x86_64 | ARM64 |
|---|---|---|
| HIGHMEM | Yes (PAE with 4GB+ RAM) | No (64-bit VA: all RAM mapped) |
| GFP_HIGHMEM | Allocates ZONE_HIGHMEM | Same as GFP_KERNEL |
| Per-CPU register | fs/gs segment | TPIDR_EL1 |
| TLB shootdown | Send IPI | TLBI *IS broadcast |
| MTE support | No | Yes (ARMv8.5+) |
| WFE/WFI | MWAIT/HLT | WFE / WFI |
| Cache line size | 64 bytes | 64 bytes (typically) |
| Max page size | 4KB (x86 also 2MB/1GB huge) | 4KB/16KB/64KB base, 2MB/1GB huge |
| DMA mask default | 32-bit (many x86 devices) | 64-bit (most ARM64 devices) |
