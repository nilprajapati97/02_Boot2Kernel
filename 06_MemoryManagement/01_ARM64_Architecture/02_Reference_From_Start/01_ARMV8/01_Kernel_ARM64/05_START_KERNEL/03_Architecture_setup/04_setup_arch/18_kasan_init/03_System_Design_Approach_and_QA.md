# kasan_init() — System Design Approach and Q&A

## 1. Why KASAN Exists: The Memory Safety Problem in Kernel Code

The Linux kernel is written in C, which has no memory safety guarantees:
- No bounds checking on array accesses
- No use-after-free detection
- No double-free detection

A single memory corruption bug in the kernel can:
- Lead to arbitrary code execution (privilege escalation)
- Cause silent data corruption that's never reported
- Appear as a random crash far from the actual bug

KASAN makes memory violations **immediately visible** — instead of silently corrupting memory and crashing later at an unrelated point, KASAN detects the violation at the exact instruction where it occurs, with a stack trace.

---

## 2. Design Principle: Shadow Memory as a Side Channel

KASAN's shadow memory is a **metadata side channel** — a parallel data structure that tracks the state of every kernel memory byte without changing the kernel's own data structures.

```
Why shadow memory, not inline metadata?

Option A: Store validity bits inline with data
  [header: 8 bytes][data: N bytes][footer: 8 bytes] ← like glibc malloc
  Problem: Inline metadata changes object sizes → breaks existing kernel
           code that uses fixed sizes (e.g., struct page, kernel stacks).
           Not feasible for kernel code.

Option B: Separate shadow memory (KASAN's approach)
  Shadow is separate from kernel memory → no size changes
  Compiler instruments access sites to check shadow
  No changes to data structure layout
  ✓ Works for all kernel memory including fixed-size objects
```

The 1:8 ratio (1 shadow byte per 8 kernel bytes) is chosen because:
- Minimum allocation unit in kmalloc is 1 byte, but memory accesses are typically word-aligned
- 8-byte granularity with a 1-byte shadow value allows encoding: "first N bytes valid" (N = 0-7) or "all invalid" (negative values for error type)

---

## 3. The KASAN Report: What Happens on Detection

When KASAN detects a violation:

```
void kasan_report(unsigned long addr, size_t size, bool is_write,
                  unsigned long ip)
{
    /* 1. Print the violation type */
    pr_err("BUG: KASAN: use-after-free in %pS\n", ip);

    /* 2. Print the access details */
    pr_err("%s of size %zu at addr %p by task %s/%d\n",
           is_write ? "Write" : "Read", size, addr,
           current->comm, task_pid_nr(current));

    /* 3. Print kernel stack trace */
    dump_stack();

    /* 4. Print object information (where was it allocated/freed) */
    print_address_description(addr);

    /* 5. Optionally panic (if kasan_multi_shot=1, continue) */
    if (!test_and_set_bit(KASAN_BIT_REPORTED, &kasan_flags))
        panic("kasan: panic_on_warn set ...");
}
```

A typical KASAN report:
```
BUG: KASAN: use-after-free in do_something+0x48/0xb0
Read of size 4 at addr ffff888104e24900 by task swapper/0/1

CPU: 0 PID: 1 Comm: swapper/0
Call Trace:
 do_something+0x48/0xb0
 setup_arch+0x3c8/0x7f0
 start_kernel+0x54/0x5c0

Allocated by task 1:
 kasan_kmalloc+0x90/0xd0
 kmem_cache_alloc+0x1b8/0x3f0
 some_init_func+0x20/0x40

Freed by task 1:
 kfree+0x80/0x1f0
 another_func+0x18/0x30
```

This report immediately identifies: what happened (use-after-free), where the access was, and where the object was allocated and freed. This is vastly more useful than the typical "kernel NULL pointer dereference at random address" crash.

---

## 4. Dependency Graph

```
[paging_init()]
  └── permanent page tables built
  └── memblock_alloc() works for large allocations
        │
        ▼
[kasan_init()]
  ├── kasan_map_populate()
  │     Uses set_pte/set_pmd to map shadow region
  │     Uses memblock_alloc() for PTE pages
  ├── kasan_populate_zero_shadow()
  │     Maps unmapped shadow regions to read-only zero page
  └── init_task.kasan_depth = 0
        │
        ▼
[All subsequent kernel code instrumented with KASAN shadow checks]
  ├── kmalloc() / kfree() → poison shadow on free, unpoison on alloc
  ├── page_alloc → poison shadow on page_free_list
  └── compiler-inserted checks at every memory access
```

---

## 5. KASAN vs Other Memory Safety Tools

| Tool | Type | Detects | Overhead | Kernel support |
|------|------|---------|----------|----------------|
| KASAN | Shadow memory | UAF, overflow, UBI | 2-3x | Yes |
| KFENCE | Probabilistic | UAF, overflow | ~1% | Yes (production) |
| KMSAN | Shadow (undefined) | Uninitialized reads | 3-5x | Yes |
| AddressSanitizer (ASan) | User-space KASAN | UAF, overflow | 2x | Userspace |
| Valgrind | Binary instrumentation | UAF, leaks | 5-20x | Userspace |
| ARM MTE | Hardware | UAF, overflow | ~0% | ARM64 only |

KFENCE (Kernel Electric Fence) is the production-ready complement to KASAN:
- Randomly samples ~1% of kmalloc allocations for electric-fence protection
- Only ~1% overhead → suitable for production kernels
- KASAN: catches all bugs during development; KFENCE: catches some bugs in production

---

## 6. System Design Q&A

**Q: How does KASAN interact with CONFIG_KASAN_INLINE vs CONFIG_KASAN_OUTLINE?**
> These are two compiler instrumentation modes. `KASAN_OUTLINE` inserts function calls to `__asan_load4()`, `__asan_store4()`, etc. at each memory access — the check code is in the KASAN runtime library, not inlined. `KASAN_INLINE` inserts the shadow check code inline at every access site — faster (no function call overhead) but larger code size. `KASAN_INLINE` is recommended for performance. The difference is ~10-20% in KASAN overhead between the two modes. For the kernel, `KASAN_INLINE` is the default when KASAN is enabled.

**Q: What does "redzone" mean in KASAN and how does it work for kmalloc?**
> A redzone is extra memory added around an allocation that KASAN poisons with a specific shadow value. For `kmalloc(100, GFP_KERNEL)`, KASAN actually allocates, say, 128 bytes (power-of-2 rounding) but poisons the last 28 bytes with `KASAN_KMALLOC_REDZONE`. Any access to bytes 100-127 triggers a "kmalloc out-of-bounds" KASAN report. The redzone catches write-past-end bugs (heap buffer overflow). The size is determined by kmalloc's internal slab cache — the difference between the requested size and the actual slab object size becomes the redzone.

**Q: Can KASAN be used to find bugs in interrupt handlers?**
> Yes, KASAN works in interrupt context. The `kasan_depth` counter mechanism is per-task, but interrupts run on the current task's stack with the task's `kasan_depth`. When KASAN detects a violation in an interrupt handler, `kasan_report()` checks `in_interrupt()` and `in_nmi()` and adjusts the report format. The `dump_stack()` call in interrupt context shows the interrupt stack trace, including the interrupted task. The main limitation: NMI handlers that access memory before `kasan_init()` completes (e.g., early NMI handlers) can't be protected by KASAN.

**Q: How does KASAN handle the kernel's own code that intentionally accesses "invalid" memory?**
> Several kernel code paths intentionally access memory that KASAN would flag: `copy_from_user()` (user memory may be invalid), probing MMIO registers (might fault), error handlers accessing freed objects for debugging. KASAN provides `kasan_disable_current()` / `kasan_enable_current()` (increments/decrements `kasan_depth`) to bracket such accesses. When `kasan_depth > 0`, the compiler-instrumented check is a no-op. This allows intentional unsafe accesses without flooding KASAN reports. The `probe_kernel_read()` / `get_user()` macros use this mechanism internally.
