# 50 Advanced Linux Kernel / Device Driver Interview Q\&A (with tricky real-world scenarios & debugging tips)

Nice — here’s a compact, interview-ready set of 50 advanced questions and answers targeted at kernel and device-driver engineers. Each answer is focused, practical, and includes debugging tips or real-world gotchas where appropriate. Use this to rehearse and to build deeper follow-ups for any item you want to master.

---

1. **Q: Explain the sequence when `insmod` loads a `.ko`.**
   **A:** `insmod` (or `finit_module`) syscall → kernel `load_module()` → validate ELF & vermagic & signature → `struct module` creation → `module_alloc()` (vmalloc) for sections → copy sections & zero `.bss` → resolve symbols (kallsyms, EXPORT\_SYMBOL/\_GPL, try\_to\_request\_module) → apply relocations → set page permissions & flush icache → insert into `modules` list & sysfs → parse params → call `init` → mark LIVE or rollback on failure.
   **Debug tip:** check `dmesg` for vermagic/relocation errors and `/sys/module/<m>/sections`.

2. **Q: How are kernel symbols exported and looked up?**
   **A:** `EXPORT_SYMBOL()` / `EXPORT_SYMBOL_GPL()` add symbols to exported table. Loader uses kernel symbol hash (kallsyms) to resolve relocations. `EXPORT_SYMBOL_GPL` restricts to GPL modules. Versioning (`CONFIG_MODVERSIONS`) adds CRC checks.
   **Gotcha:** referencing non-exported symbols fails; unresolved symbols may trigger `request_module()`.

3. **Q: Difference between `kmalloc`, `vmalloc`, `alloc_pages`?**
   **A:** `kmalloc` — small, physically contiguous, slab caches; `vmalloc` — virtually contiguous, physically fragmented, uses page tables; `alloc_pages`/`__get_free_pages` — raw page-granular allocations from buddy. Use `kmalloc` for DMA only if physically contiguous requirement satisfied.
   **Debug tip:** ENOMEM due to fragmentation — try `vmalloc` or higher-order alloc.

4. **Q: How does `mmap()` work for drivers?**
   **A:** Userspace `mmap()` triggers driver `.mmap()` callback. Driver sets `vma->vm_page_prot` and calls `remap_pfn_range()` (or `vm_insert_page`) to map physical/DMA/device pages into process page table. Manage `vm_ops` for fault handlers if needed.
   **Gotcha:** caching attributes (write-combining vs uncached) must be correct for correctness/performance.

5. **Q: Explain DMA coherent vs streaming APIs.**
   **A:** `dma_alloc_coherent()` returns a CPU & device-consistent buffer (no explicit sync); `dma_map_single()`/`dma_unmap_single()` for streaming buffers requires explicit sync, suitable for large or transient transfers. Choose mapping based on device cacheability and direction.
   **Debug tip:** incorrect mapping leads to stale data; use `dma_sync_single_for_cpu/device()` when appropriate.

6. **Q: What causes a page fault in IRQ context and why is it bad?**
   **A:** Page faults occur when code in IRQ context accesses unmapped user memory or vmalloc not mapped. IRQ context cannot sleep or handle page faults; kernel BUG occurs.
   **Fix:** avoid sleeping in IRQ handlers; use `copy_from_user()` only in process context; for deferred work use workqueue/tasklet.

7. **Q: How does Copy-On-Write (COW) work across `fork()`?**
   **A:** Parent and child share PTEs marked read-only; on a write fault, `handle_mm_fault()` allocates new page, copies data, updates PTE and reference counts. `anon_vma` tracks shared anonymous mappings.
   **Debug tip:** heavy forking + writes → page copying cost; use `vfork()` or `posix_spawn()` if suitable.

8. **Q: Explain kernel page allocator (buddy) basics.**
   **A:** Pages tracked per-order buckets (2^order contiguous pages). Free lists per zone (NORMAL/HIGHMEM/DMA). `alloc_pages()` gets pages; on failure, allocator tries compaction, reclaim, or returns NULL. GFP flags control blocking/atomic behavior.
   **Gotcha:** high-order allocations may fail due to fragmentation.

9. **Q: What are `struct mm_struct` and `vm_area_struct`?**
   **A:** `mm_struct` describes a process's address space (page tables, VMA tree). `vm_area_struct` represents a contiguous virtual range (start/end, flags, file pointer). The kernel uses an RB-tree and linked list for VMAs.
   **Debug tip:** view `/proc/<pid>/maps` to inspect VMAs.

10. **Q: How to implement scatter-gather DMA?**
    **A:** Use `sg_alloc_table`/`sg_init_table` or DMA mapping helpers returning `struct scatterlist` with `dma_map_sg()` to map scatterlist for hardware. Hardware reads descriptors/SG list.
    **Gotcha:** CPU cache coherency and IOMMU considerations.

11. **Q: How does IOMMU change DMA model?**
    **A:** IOMMU provides I/O address translation; drivers use DMA APIs which map through IOMMU domain. IOMMU enables device memory isolation and supports passthrough; mapping is via iommu ops.
    **Debug:** missing iommu mappings cause device faults (e.g., IOVA errors).

12. **Q: How is `copy_to_user()` implemented?**
    **A:** Performs safe memory accesses with validation; copies page-by-page and handles faults using `get_user_pages()`/`access_ok()` depending on path. It can fault and returns bytes not copied on error. Always check return.
    **Gotcha:** copy\_to\_user in atomic context may sleep/fault — avoid.

13. **Q: Explain `get_user_pages()` and its semantics.**
    **A:** Pins user pages and returns `struct page` refs for kernel use (DMA, io). With GUP, you must `put_page()` later. GUP flags control write intent and no-fault behavior.
    **Gotcha:** long pinning blocks memory reclaim; use with care.

14. **Q: What causes race conditions in driver probe/unbind?**
    **A:** Concurrency between hotplug (remove/probe), sysfs ops, interrupts, and userspace interactions. Not holding appropriate locks or using improper reference counting leads to use-after-free.
    **Fix:** use device model reference (`get_device/put_device`), `devm_*` helpers for cleanup, and proper locking (mutex/RCU).

15. **Q: How to handle multi-threaded interrupts (MSI-X) in drivers?**
    **A:** Use `pci_enable_msix()` to allocate vectors, `request_irq()` per vector, and per-CPU or per-queue affinities. Ensure handlers are short and schedule bottom halves for processing.
    **Debug tip:** check affinity, IRQ balancing, and masking order to avoid lost interrupts.

16. **Q: Describe race in `rmmod` vs active users — how to prevent?**
    **A:** Module unload requires `usecount==0`. Use `try_module_get()`/`module_put()` or `get_device()`/`put_device()` around accesses. For sysfs, use `driver_unregister()` sequence and proper refcounting.
    **Gotcha:** forgetting a `module_get()` around threaded work leads to Oops on rmmod.

17. **Q: What is uaccess vs kernel access?**
    **A:** `uaccess` APIs (`copy_to_user`, `get_user`, `put_user`) validate and safely access user memory. Kernel must not dereference user pointers directly.
    **Debug tip:** use `access_ok()` before copying.

18. **Q: Explain `wait_queue` vs `completion`.**
    **A:** `wait_queue` is for blocking/sleep wake-up semantics (many waiters), `completion` is a one-shot primitive (init/wait/complete) often used for request/ack flows. `completion` is simpler for single event sync.
    **Use:** I/O completion or start/stop sequences.

19. **Q: How does `workqueue` differ from `tasklet`?**
    **A:** `tasklet` runs in softirq context (no sleep), `workqueue` runs in process context (can sleep). Use workqueues when sleeping or locking needed. Tasklets are deprecated in favor of threaded IRQs & workqueues.
    **Gotcha:** using sleeping calls inside tasklet causes BUG.

20. **Q: Kernel memory leak detection strategies.**
    **A:** Use `kmemleak` (compile-time option), `slabinfo`/`slabtop`, dynamic `tracepoints`, check `dmesg` for `kmalloc` failures, and review reference counts. For DMA resources, track `dma_unmap`/`scatterlist` teardown.
    **Interview tip:** demonstrate how to reproduce and collect heap leaks (stress test + snapshot diffs).

21. **Q: How to debug a page fault inside kernel module?**
    **A:** Inspect oops stack trace via `dmesg`, use `addr2line` on `vmlinux` to map PC to source, check for illegal user pointer access in atomic/IRQ context. Use `lockdep` and KASAN if available.
    **Fix:** ensure no user memory access from atomic context; pin pages properly.

22. **Q: Explain `vmalloc` vs `ioremap`.**
    **A:** `vmalloc` maps physical pages into contiguous kernel virtual space (for ordinary RAM). `ioremap` (or `pci_iomap`) maps MMIO/device physical addresses into kernel virtual space. `ioremap` uses non-cacheable or device-specific attributes.
    **Gotcha:** using `memcpy` on ioremap'd regions is invalid; use `readl/writel` or `memcpy_fromio`.

23. **Q: How do you perform zero-copy I/O with network drivers?**
    **A:** Use `get_user_pages()` to pin user pages and map them to NIC DMA (or use XDP/AF\_XDP for kernel-bypass). Use scatter-gather DMA and avoid extra copies; ensure cache coherency and unmap after use.
    **Debug:** pinned pages can block OOM — ensure put\_page on completion.

24. **Q: What's the difference between `pci_iomap` and `ioremap`?**
    **A:** `pci_iomap` is a convenience wrapper that reads PCI BAR and sets up `ioremap`. `ioremap` maps arbitrary physical IO memory into kernel VA with device memory attributes. Use `pci_iomap()` in PCI drivers.

25. **Q: How to handle device DMA in the presence of IOMMU and VFIO?**
    **A:** Use DMA mapping APIs which route through IOMMU. For passthrough VMs use VFIO: user-space manages device with VFIO, IOMMU groups enforced, and DMA mappings created via VFIO ioctls.
    **Gotcha:** IOMMU domain attach/detach and incorrect IOVA ranges cause device fault.

26. **Q: Explain `devm_` resource management and its pros/cons.**
    **A:** `devm_*` helpers auto-free resources on driver detach (`device` lifecycle). Pros: reduces leaks and simplifies error paths. Cons: less control; not suitable for long-lived allocations beyond device lifetime.
    **Tip:** Use devm for simple probe-time allocations.

27. **Q: What is KMSAN and KASAN? When to use each?**
    **A:** KASAN detects out-of-bounds and use-after-free in kernel memory. KMSAN detects uninitialized memory usage in kernel. Use KASAN for UAF/bounds bugs; use KMSAN when encountering uninitialized data propagation. Both are heavy-weight instrumentation builds.

28. **Q: How to safely perform copy to/from user in interrupt bottom halves?**
    **A:** You can’t access user memory in interrupt/softirq context. Instead, copy user data in process context (syscall) or pin pages earlier (GUP) and use a safe reference (struct page) in bottom half; convert to mapping if necessary.

29. **Q: Explain `poll`/`select` implementation in drivers.**
    **A:** Driver implements `.poll()` VFS method to return bitmask (POLLIN/POLLOUT). It registers `wait_queue_head_t` and uses `poll_wait()` to add the wait queue; on data arrival invoke `wake_up_interruptible()`.
    **Debug:** missing `poll_wait()` causes userspace hang.

30. **Q: How to design a safe ioctl interface?**
    **A:** Use `copy_from_user()`/`copy_to_user()` with validated size & versioning; use `compat_ioctl` for 32/64-bit compatibility; avoid passing pointers into kernel; prefer `read/write` or `mmap` for bulk transfers. Document ABI and semver.
    **Real-world tip:** add IOCTL range checks and magic numbers.

31. **Q: How do you handle memory barriers for device register access?**
    **A:** Use `writel()`/`readl()` which include required barriers on many arches, or explicit `mb()`, `rmb()`, `wmb()` as needed. Ensure ordering around DMA descriptors and doorbells.
    **Debug:** missing barriers cause subtle lost or reordered writes.

32. **Q: Why would `dma_map_single()`/`unmap` mismatch cause issues?**
    **A:** Mismatching direction or not unmapping causes stale caches or leaks; double-unmap could corrupt IOMMU state. Always pair map/unmap with correct direction and for each transaction.
    **Tip:** enable IOMMU logging to catch errors.

33. **Q: How to debug a PCIe BAR size/placement problem?**
    **A:** Check `lspci -v` for BAR ranges, use `dmesg` for resource allocation logs, ensure `pci_enable_device()` succeeds, verify `resource_size()` and `pci_resource_start()`. For BAR aliasing, check BIOS/ACPI resource conflicts.
    **Gotcha:** BAR not requested/no space leads to device not mapped.

34. **Q: What are `request_irq()` flags important to set?**
    **A:** IRQF\_SHARED (for shared lines), IRQF\_TRIGGER\_\* for edge/level, IRQF\_ONESHOT for threaded IRQs, IRQF\_NO\_SUSPEND for suspend behavior. Ensure correct flags to prevent lost interrupts and deadlocks.

35. **Q: Explain how `splice()`/`vmsplice()` provide zero-copy paths.**
    **A:** `splice()` moves data between file descriptors in kernel without copying to user space. `vmsplice()` maps user pages into a pipe for zero-copy transfers. Use cases: high-performance I/O or network forwarding.
    **Security:** ensure checks to prevent pinning user pages too long.

36. **Q: How to handle hotplug/unbind race with sysfs attribute callbacks?**
    **A:** Sysfs callbacks run in process context and need to check device state & take device reference. Use `sysfs_notify` and lock (`device_lock`) and ensure removal waits for in-flight sysfs operations. `device_del()`/`device_unregister()` sequences must coordinate.

37. **Q: How to handle scatter-gather lists with SG table reuse?**
    **A:** Carefully allocate/free `sg_table` per request or maintain a pool. After `dma_map_sg()`, the SG list has DMA addresses; ensure `dma_unmap_sg()` before freeing SG table. Reuse requires reinitializing lengths and addresses.

38. **Q: Explain driver/firmware interaction model (request\_firmware).**
    **A:** `request_firmware()` runs in process context (may sleep) and loads firmware blob via userspace helper if not in kernel. Drivers must handle failures and avoid blocking critical paths. Use `firmware_class` and `devm_request_firmware()`.

39. **Q: How to instrument performance inside kernel driver?**
    **A:** Use `tracepoints`, `ftrace`, `trace_printk`, `perf` events, BPF (kprobes/uprobes), and `debugfs` counters. Prefer dynamic tracepoints to avoid runtime overhead in production.

40. **Q: What is `rcu` and when to use it?**
    **A:** Read-Copy-Update: low-overhead reader paths without locks; updaters create new copy and call `synchronize_rcu()` or `call_rcu()` to defer reclamation after grace period. Use for read-mostly data structures (e.g., routing tables).
    **Debug:** use `rcu_read_lock()`/`rcu_read_unlock()` around readers.

41. **Q: Explain `get_user_pages_remote()` vs `get_user_pages`.**
    **A:** `get_user_pages_remote()` pins pages of a process from another task or address space (requires `mm` and lock). Use when pinning pages for RDMA or cross-process DMA. Must handle race/permissions.

42. **Q: How to debug kernel OOPS and translate addresses to source?**
    **A:** Copy oops trace from `dmesg`; use `ksymoops`/`addr2line -e vmlinux <addr>` to map addresses. Ensure `CONFIG_KALLSYMS` and unstripped vmlinux available. Use `crash` utility for core dumps.

43. **Q: What is `doorbell` in device drivers and how to handle it?**
    **A:** Doorbell is a register write to notify device of new descriptors. Writes must be ordered and often need `wmb()` before writing doorbell to ensure descriptor memory is visible to device.

44. **Q: How to design for suspend/resume in drivers?**
    **A:** Implement `pm_ops` (`suspend`, `resume`, `freeze`, `thaw`) to quiesce device, save state, and reinitialize on resume. Consider runtime PM (autosuspend), device wakeup sources, and power domain ordering. Test with `echo freeze > /sys/power/state`.

45. **Q: Why does `copy_from_user` sometimes return fewer bytes?**
    **A:** Because user memory may fault or be partially invalid; `copy_from_user` returns bytes not copied on failure. Always check return value. Use GUP if you need pinned pages.

46. **Q: How to safely implement a character device that supports both blocking and non-blocking IO?**
    **A:** Implement `.read`/`.write` to return `-EAGAIN` if non-blocking and data not ready; use `poll()` to waiters via `poll_wait()` and `wake_up()`. Properly handle O\_NONBLOCK flag in `open`.

47. **Q: Explain device model reference counting (device\_get/put).**
    **A:** `get_device()/put_device()` increment/decrement the device's refcount to prevent detach while in use. Drivers and clients use these around asynchronous work or long-lived references.

48. **Q: How to prevent priority inversion with kernel locks?**
    **A:** Prefer sleeping locks (mutex) in process context; for real-time systems use priority inheritance mutexes (`CONFIG_PREEMPT_RT`) or avoid long-held locks in high-priority contexts. Use `rwsem` for reader-heavy workloads.

49. **Q: Real-world example: DMA mapping leak & how to find/fix it.**
    **A:** Symptom: IOMMU mappings grow and eventually fail; dmesg shows mapping warnings. Debug: audit map/unmap call sites, enable DMA debug (`dynamic_debug` or driver logs), check error paths where `dma_unmap_*()` is skipped. Fix by ensuring unmap on all error/return paths, review scatterlist lifetimes.

50. **Q: Real-world example: page fault in ISR (how it happens & fix).**
    **A:** Cause: ISR calls code that accesses pageable memory or calls `copy_from_user()`/`kmalloc` with GFP\_KERNEL that can sleep. Result: BUG or Oops. Fix: move faulting work to workqueue/threaded IRQ; use `kmalloc` with `GFP_ATOMIC` or preallocate buffers; avoid user-copy in ISR. Use `lockdep`/KASAN to detect.

---

If you want, I can now:

* Expand any of these into a 2–4 slide mini-deck with diagrams and kernel function pointers; or
* Provide **detailed example code** for tricky Qs (e.g., safe `.mmap()` implementation with `fault` handler, an example of `dma_map_sg` flow, or a correct probe/remove skeleton that avoids races).

Which specific question(s) should we expand first?
