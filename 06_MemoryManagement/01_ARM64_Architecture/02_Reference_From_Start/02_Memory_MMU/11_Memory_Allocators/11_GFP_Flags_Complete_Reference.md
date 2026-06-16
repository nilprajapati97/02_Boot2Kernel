# GFP Flags: Complete Allocator Context Reference

**Category**: Linux Kernel Memory Allocators  
**Platform**: ARM64 (AArch64)

---

## 1. Concept Foundation

```
GFP (Get Free Pages) flags: pass context + constraints to memory allocator

  Why do allocation flags matter?
    The allocator needs to know:
    1. CAN it sleep to wait for memory? (interrupt handlers: NO)
    2. CAN it trigger I/O to reclaim? (inside FS code: maybe no)
    3. WHERE must memory come from? (DMA zone, NUMA node)
    4. HOW hard should it try? (retry forever? fail fast?)
    5. WHAT type of memory? (zeroed? high-memory? movable?)
  
  Passing wrong GFP flags = kernel deadlock or panic
  Example: calling kmalloc(GFP_KERNEL) in an interrupt handler:
    GFP_KERNEL → may sleep → interrupt handlers CANNOT sleep
    → BUG: scheduling while atomic

GFP flags are 32-bit values:
  Lower bits: zone selection
  Middle bits: behavioral modifiers
  Upper bits: type flags

ARM64 zone layout:
  ZONE_DMA:     0 – ~1GB  (some ARM64 SoCs need DMA in low memory)
  ZONE_DMA32:   0 – 4GB   (for 32-bit DMA devices)
  ZONE_NORMAL:  rest of RAM
  ZONE_MOVABLE: (optional, for hotplug or migration)
  No ZONE_HIGHMEM on ARM64! (64-bit VA: all physical memory directly mapped)
```

---

## 2. GFP Flags Bit Layout

```
GFP flags (include/linux/gfp.h):

Zone modifiers (bits 0-3):
  ___GFP_DMA        = 0x01  // must be in ZONE_DMA
  ___GFP_HIGHMEM    = 0x02  // prefer ZONE_HIGHMEM (ARM64: no-op, no HIGHMEM)
  ___GFP_DMA32      = 0x04  // must be below 4GB (for 32-bit DMA)
  ___GFP_MOVABLE    = 0x08  // allocate from ZONE_MOVABLE

Memory policy:
  ___GFP_RECLAIMABLE = 0x10  // page is reclaimable (page cache, slab-reclaimable)
  ___GFP_HIGH        = 0x20  // use emergency memory reserves
  ___GFP_IO          = 0x40  // can start physical I/O (for reclaim)
  ___GFP_FS          = 0x80  // can call filesystem code (for reclaim)
  ___GFP_ZERO        = 0x100 // zero the allocated memory
  ___GFP_ATOMIC      = 0x200 // atomic: don't sleep, use emergency reserves
  ___GFP_DIRECT_RECLAIM = 0x400  // can call direct memory reclaim
  ___GFP_KSWAPD_RECLAIM = 0x800 // can wake kswapd for background reclaim
  ___GFP_WRITE       = 0x1000 // allocation is for dirty writeable data
  ___GFP_NOWARN      = 0x2000 // suppress allocation failure warning
  ___GFP_RETRY_MAYFAIL = 0x4000 // retry but allow failure
  ___GFP_NOFAIL      = 0x8000  // never fail (WARNING: use sparingly!)
  ___GFP_NORETRY     = 0x10000 // don't retry on failure
  ___GFP_MEMALLOC    = 0x20000 // use all memory including reserves
  ___GFP_COMP        = 0x40000 // compound page (for huge pages, slab)
  ___GFP_NOMEMALLOC  = 0x80000 // don't use memory reserves
  ___GFP_HARDWALL    = 0x100000 // enforce NUMA hardwall policy
  ___GFP_THISNODE    = 0x200000 // allocate only from current NUMA node
  ___GFP_ACCOUNT     = 0x400000 // account against memory cgroup
  ___GFP_ZEROTAGS    = 0x800000 // ARM64 MTE: zero memory tags
  ___GFP_SKIP_ZERO   = 0x1000000 // don't zero (for pages that will be overwritten)
```

---

## 3. Common GFP Combinations

```
Composite GFP flags (pre-defined combinations):

GFP_ATOMIC:
  = ___GFP_HIGH | ___GFP_ATOMIC | ___GFP_KSWAPD_RECLAIM
  Use: interrupt handlers, spinlock holders, softirq context
  Properties: CANNOT sleep, can use emergency reserves
  Failure: may fail (return NULL) → always check!
  ARM64 example: kmalloc(size, GFP_ATOMIC) in network IRQ handler

GFP_KERNEL:
  = ___GFP_RECLAIM | ___GFP_IO | ___GFP_FS
  where: ___GFP_RECLAIM = ___GFP_DIRECT_RECLAIM | ___GFP_KSWAPD_RECLAIM
  Use: normal kernel context, can sleep
  Properties: CAN sleep, CAN do I/O, CAN call FS code
  Most common GFP flag for general kernel allocations
  ARM64 example: kmalloc(size, GFP_KERNEL) in process context

GFP_KERNEL_ACCOUNT:
  = GFP_KERNEL | ___GFP_ACCOUNT
  Use: memory that should be charged to a memcg (containers)
  kmalloc for user-visible kernel data (files, sockets, etc.)

GFP_NOWAIT:
  = ___GFP_KSWAPD_RECLAIM
  Use: allocations that should try but not block
  Won't sleep, won't try hard to reclaim, can wake kswapd
  Returns NULL immediately if no memory available
  Use when: holding a mutex that might be needed by reclaim

GFP_NOIO:
  = ___GFP_RECLAIM
  = GFP_KERNEL without ___GFP_IO and ___GFP_FS
  Use: in I/O paths (block driver, SCSI) — can reclaim but NOT start I/O
  (Starting I/O while processing I/O → deadlock)
  ARM64 example: kmalloc in SCSI command handler

GFP_NOFS:
  = ___GFP_RECLAIM | ___GFP_IO
  = GFP_KERNEL without ___GFP_FS
  Use: in FS paths — can reclaim and do I/O but NOT call FS code
  (Calling FS code while holding FS locks → deadlock)
  ARM64 example: buffer allocation inside ext4 write path

GFP_USER:
  = ___GFP_RECLAIM | ___GFP_IO | ___GFP_FS | ___GFP_HARDWALL
  Use: memory for user-space (page cache, page table entries)
  HARDWALL: respects NUMA memory policy

GFP_HIGHUSER:
  = GFP_USER | ___GFP_HIGHMEM
  ARM64: same as GFP_USER (no HIGHMEM on 64-bit)

GFP_HIGHUSER_MOVABLE:
  = GFP_HIGHUSER | ___GFP_MOVABLE
  Use: user anonymous pages (movable for migration/CMA)
  Most common for anonymous memory (do_anonymous_page)

GFP_DMA:
  = GFP_ATOMIC | ___GFP_DMA
  Force ZONE_DMA allocation

GFP_DMA32:
  = GFP_KERNEL | ___GFP_DMA32
  Force below-4GB allocation (for 32-bit DMA devices)

GFP_TRANSHUGE:
  = GFP_HIGHUSER_MOVABLE | ___GFP_COMP | ___GFP_NOMEMALLOC | ___GFP_NORETRY
    | ___GFP_NOWARN & ~___GFP_RECLAIM
  Use: transparent huge page allocation (order-9 = 2MB)
  NO RECLAIM: don't stall; if not available, fall back to 4KB pages

GFP_TRANSHUGE_LIGHT:
  = GFP_TRANSHUGE | ___GFP_KSWAPD_RECLAIM
  Slightly more persistent than GFP_TRANSHUGE
```

---

## 4. GFP Context Rules

```
RULES for choosing GFP flags:

1. In interrupt context (in_interrupt() == true):
   MUST use GFP_ATOMIC (or GFP_NOWAIT for non-critical)
   NEVER use GFP_KERNEL (may sleep → BUG: scheduling while atomic)

2. Holding a spinlock:
   MUST use GFP_ATOMIC
   Spinlocks: IRQ disabled, preemption disabled → CANNOT sleep

3. Holding a mutex (sleepable):
   CAN use GFP_KERNEL (mutex allows sleep)
   But careful: if mutex is held by reclaim path → GFP_NOIO or GFP_NOFS

4. In block I/O path (bio_submit, etc.):
   Use GFP_NOIO to prevent I/O from triggering more I/O (deadlock)

5. In filesystem path (holding inode/page locks):
   Use GFP_NOFS to prevent re-entering filesystem

6. For DMA buffers (device can't reach high memory):
   32-bit device: GFP_DMA or GFP_DMA32
   64-bit device with IOMMU: GFP_KERNEL (IOMMU handles translation)

7. For user-space anonymous/file pages:
   GFP_HIGHUSER_MOVABLE: standard for user pages
   Movable: supports migration (NUMA, CMA)

8. Large kernel allocations (vmalloc, module loading):
   GFP_KERNEL | __GFP_NORETRY | __GFP_NOWARN: try once, fail gracefully

Debugging wrong GFP usage:
  /proc/sys/kernel/debug_pagealloc = 1: enable page alloc debugging
  KASAN: detects GFP_ATOMIC in wrong context
  lockdep: tracks sleep-in-atomic via might_sleep()
  ARM64: kernel warnings: "BUG: sleeping function called from invalid context"
```

---

## 5. Interview Questions & Answers

**Q1: What happens if you use GFP_KERNEL in a softirq (NAPI) handler?**

Linux will print a kernel warning and potentially BUG:
```
BUG: sleeping function called from invalid context at mm/slub.c:NNN
in_softirq(): 1, irqs_disabled(): 0, pid: 0, name: "swapper/0"
```

This happens because `GFP_KERNEL` includes `__GFP_DIRECT_RECLAIM`, which allows direct memory reclaim. Reclaim involves:
- Waiting for pages to be written to swap/disk (sleeping wait)
- Calling filesystem code (which may block on I/O)
- Acquiring mutexes

Softirqs run with `in_softirq() == true`, which means they cannot sleep. Linux's `might_sleep()` macro (called internally by the allocator when `__GFP_DIRECT_RECLAIM` is set) detects this and warns/panics.

**Fix**: Use `GFP_ATOMIC` in NAPI/softirq handlers. If memory is not available, GFP_ATOMIC can use emergency reserves and fails quickly (returns NULL) rather than sleeping.

**Q2: When would you use `__GFP_NOFAIL`? What are the risks?**

`__GFP_NOFAIL` tells the allocator "this allocation MUST succeed, retry indefinitely." Use only when:
- Failure would cause immediate kernel panic anyway (no error path available)
- The allocation is for very small amount of truly critical data
- You have verified the allocator CAN theoretically succeed eventually

**Risks**:
1. **System hang**: if the system is genuinely out of memory (swap full, OOM kill not freeing enough), the system HANGS, spinning in the allocator forever. Users see an unresponsive system with no crash dump or useful diagnostic.
2. **Deadlock**: if the thread holding memory needed for reclaim is the SAME thread waiting for memory, the system deadlocks permanently.
3. **Hard to debug**: hung system with no output is harder to diagnose than a clear OOM kill.

**Better alternatives**:
- `__GFP_RETRY_MAYFAIL`: retry several times but eventually allow failure
- Pre-allocate at initialization time (before memory pressure)
- Use reserved emergency memory pool

---

## 6. Quick Reference

| Context | Required GFP |
|---|---|
| Interrupt handler | GFP_ATOMIC |
| Softirq / NAPI | GFP_ATOMIC |
| Spinlock holder | GFP_ATOMIC |
| Mutex holder | GFP_KERNEL (usually) |
| Block I/O path | GFP_NOIO |
| Filesystem path | GFP_NOFS |
| User page alloc | GFP_HIGHUSER_MOVABLE |
| 32-bit DMA | GFP_DMA32 |
| Huge page (THP) | GFP_TRANSHUGE |

| GFP Flag | Can Sleep? | Uses Emergency Reserve? | Can Fail? |
|---|---|---|---|
| GFP_KERNEL | YES | NO | YES |
| GFP_ATOMIC | NO | YES | YES |
| GFP_NOWAIT | NO | NO | YES |
| GFP_NOIO | YES (no I/O) | NO | YES |
| GFP_NOFS | YES (no FS) | NO | YES |
| +__GFP_NOFAIL | YES | YES | NO (blocks) |
| +__GFP_RETRY_MAYFAIL | YES | NO | YES (after retry) |
