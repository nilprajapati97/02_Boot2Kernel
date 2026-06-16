# User Space VA Limit, Mmap, Stack, Heap Layout

**Category**: Virtual Address Space  
**Targeted**: ARM, Qualcomm, NVIDIA, AMD

---

## 1. Concept Foundation

Every Linux process has its own user-space VA layout, controlled by the kernel through Virtual Memory Areas (VMAs). The layout depends on:
1. **TASK_SIZE**: Maximum user VA (= 2^VA_BITS for AArch64)
2. **ASLR** (Address Space Layout Randomization): Randomizes stack, mmap, heap bases
3. **ELF load address**: Where the dynamic linker places the binary
4. **mmap_base**: The base address for anonymous and file-backed mmap regions

Understanding this layout is critical for:
- Security analysis (exploit mitigations)
- Memory debugging (valgrind, ASAN)
- Performance profiling (large mmap region fragmentation)
- Container/sandbox memory limits

---

## 2. TASK_SIZE — Maximum User VA

```c
// arch/arm64/include/asm/processor.h

// 64-bit process:
#define TASK_SIZE_64    (UL(1) << VA_BITS)
// For 48-bit VA: TASK_SIZE_64 = 2^48 = 256 TB

// 32-bit compat process (AArch32 under AArch64 kernel):
#define TASK_SIZE_32    UL(0x100000000)  // 4 GB

// Current task's TASK_SIZE:
#define TASK_SIZE       (test_thread_flag(TIF_32BIT) ? TASK_SIZE_32 : TASK_SIZE_64)

// Maximum user stack address:
#define STACK_TOP       TASK_SIZE
// User stack grows down from STACK_TOP (ASLR moves this)
```

---

## 3. Typical 64-bit Process VA Layout (48-bit VA, ASLR)

```
High VA (0x0000_FFFF_FFFF_FFFF)
│
├── [Stack top] ─ ASLR randomized ────────────────────────
│   Stack (grows downward)
│   Initial size: RLIMIT_STACK (default 8 MB)
│   Hard limit: 128 MB (configurable)
│   guard page (PROT_NONE, 1 page)
│
├── [Stack guard page]
│
├── [mmap region - top-down allocation]
│   Shared libraries (.so files)
│   Anonymous mmaps (large malloc, posix_memalign)
│   File-backed maps (mmap'd files)
│   mmap_base = TASK_SIZE/3 * 2 (ASLR adjusted)
│
├── [Heap - grows upward from brk]
│   brk() system call extends/shrinks heap
│   Initial brk = end of BSS + random ASLR gap
│
├── [BSS segment] (uninitialized data, zero-filled)
│
├── [Data segment] (initialized writable data)
│
├── [Text/Code segment] (read-only, executable)
│   Load address: 0x5555_5555_4000 (randomized by ASLR)
│   (actual value varies; ~0x4000 for PIE without ASLR)
│
└── 0x0000_0000_0000_0000 (NULL - unmapped)
```

---

## 4. ASLR on ARM64 Linux

ARM64 Linux uses full ASLR for all regions:

```c
// arch/arm64/mm/mmap.c

// ASLR entropy sources:
#define STACK_RND_MASK     (0x3ffff)  // 18-bit stack randomization
#define MMAP_RND_BITS      28         // 28-bit mmap randomization (max for 48-bit VA)
#define MMAP_RND_COMPAT_BITS 8        // 8-bit for 32-bit compat processes

// Stack randomization:
// arch/arm64/kernel/process.c
unsigned long arch_align_stack(unsigned long sp)
{
    if (!(current->personality & ADDR_NO_RANDOMIZE) && randomize_va_space)
        sp -= get_random_u32_below(STACK_RND_MASK << PAGE_SHIFT);
    return sp & ~0xf;   // 16-byte align
}

// mmap base randomization:
// arch/arm64/mm/mmap.c
static unsigned long mmap_base(unsigned long rnd, struct rlimit *rlim_stack)
{
    unsigned long gap = rlim_stack->rlim_cur;
    // ... gap calculations
    return PAGE_ALIGN(STACK_TOP - gap - rnd);
}
```

ASLR is controlled by `/proc/sys/kernel/randomize_va_space`:
```
0 = No ASLR
1 = Stack + mmap randomized; heap fixed
2 = Stack + mmap + heap randomized (full ASLR, default)
```

---

## 5. mmap Region — Bottom-Up vs Top-Down

Linux ARM64 supports two mmap allocation strategies:

### Top-Down (default, recommended)

```
mmap_base starts near the top of user VA space (below stack).
New mmap() calls are allocated downward from mmap_base.
Benefits:
  - Large mmap regions don't fragment the heap area
  - Stack and mmap regions grow toward each other; rlimit prevents collision
```

### Bottom-Up (legacy personality)

```
mmap_base starts above the heap.
New mmap() calls are allocated upward.
Used when: ADDR_COMPAT_LAYOUT personality flag is set.
```

```c
// mm/mmap.c
unsigned long arch_get_unmapped_area_topdown(...)
{
    // Start from mmap_base, search downward
    addr = mm->mmap_base - len;
    // Find free VMA slot
}
```

---

## 6. Stack Layout Details

The initial user stack is set up by `exec()` (`fs/exec.c`):

```
Initial stack layout (at program entry):
High address (STACK_TOP - random_offset)
┌─────────────────────────────┐
│  Argument strings (argv)    │
├─────────────────────────────┤
│  Environment strings (envp) │
├─────────────────────────────┤
│  Auxiliary vectors (auxv)   │ ← AT_PLATFORM, AT_HWCAP, etc.
├─────────────────────────────┤
│  NULL (end of envp)         │
│  envp[n-1]                  │
│  ...                        │
│  envp[0]                    │
├─────────────────────────────┤
│  NULL (end of argv)         │
│  argv[argc-1]               │
│  ...                        │
│  argv[0]                    │
├─────────────────────────────┤
│  argc                       │
└─────────────────────────────┘ ← initial SP
```

Stack grows downward. `ulimit -s` (RLIMIT_STACK) controls max stack size (default 8 MB).

---

## 7. Heap (brk) Layout

```c
// Initial heap state after exec:
mm->start_brk = mm->brk = PAGE_ALIGN(elf_data_end) + random_brk_offset

// Growing heap:
brk(new_brk):
  if (new_brk > mm->brk):
      Allocate anonymous pages for [mm->brk, new_brk)
      mm->brk = new_brk

// malloc() in glibc uses brk for small allocations (<128KB typically)
// and mmap(MAP_ANONYMOUS) for larger allocations
// The threshold is configurable via mallopt(M_MMAP_THRESHOLD, ...)
```

---

## 8. VMA Structure

Every mapped region is a `struct vm_area_struct` (VMA):

```c
// include/linux/mm_types.h
struct vm_area_struct {
    unsigned long vm_start;     // Start VA (inclusive)
    unsigned long vm_end;       // End VA (exclusive)
    struct mm_struct *vm_mm;    // Owning mm
    pgprot_t vm_page_prot;      // Page protection
    unsigned long vm_flags;     // VM_READ, VM_WRITE, VM_EXEC, VM_SHARED, ...
    struct rb_node vm_rb;       // Red-black tree node (for fast search)
    struct vm_operations_struct *vm_ops; // fault handler, etc.
    unsigned long vm_pgoff;     // Offset in file (for file-backed mappings)
    struct file *vm_file;       // Backing file (NULL for anonymous)
    void *vm_private_data;      // Private data
};
```

VMAs are stored in:
1. A sorted linked list (`mm->mmap` list)
2. A red-black tree (`mm->mm_rb`) for O(log N) VA lookups

Linux 6.1+ uses **maple tree** instead of rbtree for VMA storage.

---

## 9. /proc/PID/maps — Viewing the Layout

```bash
cat /proc/self/maps
```

Sample output:
```
5555555f4000-5555555f5000 r--p 00000000 08:01 12345  /bin/bash
5555555f5000-5555555f6000 r-xp 00001000 08:01 12345  /bin/bash
7ffff7fce000-7ffff7fd0000 rw-p 00000000 00:00 0      [stack]
7ffff7ff9000-7ffff7ffd000 r--p 00000000 00:00 0      [vvar]
7ffff7ffd000-7ffff7fff000 r-xp 00000000 00:00 0      [vdso]
7ffffffde000-7ffffffff000 rwxp 00000000 00:00 0      [stack]
```

Fields: `start-end  perms  offset  dev  inode  pathname`

`/proc/PID/smaps` gives per-VMA detailed statistics (RSS, PSS, swap, etc.).

---

## 10. Interview Questions & Answers

**Q1: Where does malloc() get memory from?**

For small allocations (typically < 128 KB), glibc's malloc uses `brk()`/`sbrk()` to extend the heap upward. For large allocations, it uses `mmap(MAP_ANONYMOUS, MAP_PRIVATE)` to get a new VA region in the mmap area, which can be returned to the OS with `munmap()` independently. The threshold is configurable via `mallopt(M_MMAP_THRESHOLD)`. The kernel never knows about individual malloc chunks — it only sees the VMA boundaries. This is why `valgrind` and `ASAN` must intercept malloc at the glibc level.

**Q2: What is ASLR and how much entropy does ARM64 Linux use?**

ASLR (Address Space Layout Randomization) randomizes the base addresses of stack, mmap region, and heap to make ROP/return-to-libc attacks harder. ARM64 with 48-bit VA uses 28 bits of entropy for the mmap base, 18 bits for the stack, and 13 bits for the heap. With 28-bit mmap entropy, there are 2^28 = 268 million possible mmap base positions. This makes brute-force guessing infeasible. The randomization is set during `exec()` and remains constant for the process lifetime.

**Q3: What is the virtual memory area (VMA) and what operations does it support?**

A VMA is a `struct vm_area_struct` representing a contiguous range of user VA with uniform permissions and backing. Key operations via `vm_ops`: `fault()` (called on page fault to provide a page), `map_pages()` (prefault surrounding pages), `open()`/`close()` (VMA lifecycle), `mmap()` (device-specific mapping). When a user accesses a VA, the kernel finds the VMA via the maple tree/rbtree, then calls `vm_ops->fault()` if no PTE exists yet. For anonymous memory, the fault handler allocates a zero page. For file-backed memory, it reads from the page cache.

---

## 11. Quick Reference

| Region | Layout Direction | ASLR Entropy | Controlled By |
|---|---|---|---|
| Kernel image | Fixed (KASLR at boot) | KASLR | Kernel ELF loader |
| Stack | Top-down (grows ↓) | 18 bits | execve + arch_align_stack |
| Shared libraries | Top-down (mmap area) | 28 bits | mmap(PROT_EXEC) |
| Anonymous mmap | Top-down | 28 bits | mmap() |
| Heap | Bottom-up (grows ↑) | 13 bits | brk() |
| Text/data | Fixed in mmap area | 28 bits (PIE) | ELF loader |

| VMA flag | Meaning |
|---|---|
| VM_READ | Readable |
| VM_WRITE | Writable |
| VM_EXEC | Executable |
| VM_SHARED | Shared (MAP_SHARED) |
| VM_GROWSDOWN | Stack grows down |
| VM_PFNMAP | PFN-mapped (device) |
| VM_IO | I/O memory (no swap) |
| VM_DONTEXPAND | Cannot mremap |
