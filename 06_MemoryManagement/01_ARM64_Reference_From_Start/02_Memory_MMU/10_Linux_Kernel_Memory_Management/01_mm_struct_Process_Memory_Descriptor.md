# mm_struct: Process Memory Descriptor

**Category**: Linux Kernel Memory Management  
**Platform**: ARM64 (AArch64)

---

## 1. Concept Foundation

```
mm_struct: the central data structure describing a process's ENTIRE virtual address space.
  Every user-space process has exactly ONE mm_struct.
  Kernel threads: share swapper_mm (init_mm) or have mm = NULL.

Location in kernel: include/linux/mm_types.h

mm_struct is created:
  fork() → copy_mm() → dup_mm() → allocates new mm_struct, copies page tables
  exec() → exec_mmap() → allocates new mm_struct, discards old (COW cleanup)
  
mm_struct is destroyed:
  exit() → mmput() → decrement mm_users; if 0: mmput_slowpath() → mmdrop()
  mmdrop(): decrements mm_count; if 0: __mmdrop() → frees page tables + mm_struct

Reference counting:
  mm_users: number of PROCESSES sharing this mm (fork shares until exec)
  mm_count: number of references from kernel code (including kernel threads
            that might briefly hold a reference via mmget())

Threads and mm_struct:
  Threads (CLONE_VM): share the SAME mm_struct!
  thread_info→task_struct→mm → same mm_struct pointer for all threads in a process
  This is why threads see each other's memory: they share one mm_struct
```

---

## 2. mm_struct Structure Key Fields

```c
struct mm_struct {
    /* VMA management (virtual memory areas) */
    struct maple_tree       mm_mt;          // VMA tree (since Linux 6.1+)
    // Previously: struct rb_root mm_rb;   // red-black tree of VMAs (pre-6.1)
    
    /* Page table root */
    pgd_t                   *pgd;           // ARM64: physical address of PGD
    
    /* Memory counters */
    atomic_t                mm_users;       // number of user-space tasks
    atomic_t                mm_count;       // number of kernel references
    
    /* Virtual address space limits */
    unsigned long           mmap_base;      // base of mmap region (grows down)
    unsigned long           mmap_legacy_base; // for legacy mmap layout
    
    unsigned long           task_size;      // size of task virtual address space
                                            // ARM64: 512GB (USER_DS on 4-level)
    
    /* Code/data segment limits */
    unsigned long           start_code, end_code;   // .text section
    unsigned long           start_data, end_data;   // .data section
    unsigned long           start_brk, brk;          // heap: [start_brk, brk)
    unsigned long           start_stack;             // stack VMA start
    
    /* Statistics */
    unsigned long           total_vm;    // total mapped pages
    unsigned long           locked_vm;   // pages locked in RAM (mlock)
    unsigned long           pinned_vm;   // pages pinned (DMA, uio)
    unsigned long           data_vm;     // VM_WRITE, VM_EXEC cleared
    unsigned long           exec_vm;     // VM_EXEC set
    unsigned long           stack_vm;    // VM_GROWSDOWN set
    
    /* Huge pages */
    unsigned long           def_flags;   // default VMA flags (e.g., VM_HUGEPAGE)
    
    /* ASLR randomization */
    unsigned long           mmap_base;   // randomized base for mmap
    
    /* rss (resident set size) counters: */
    struct mm_rss_stat      rss_stat;    // per-mm RSS: anon, file, shm, swap
    
    /* CPU context: */
    mm_context_t            context;     // ARM64: ASID, ttbr0, etc.
    
    /* Spinlock for VMA modifications: */
    struct rw_semaphore     mmap_lock;   // protects VMA list/tree
    
    /* Code section for exec: */
    unsigned long           arg_start, arg_end;  // argv
    unsigned long           env_start, env_end;  // environment
};
```

---

## 3. ARM64-Specific mm_context_t

```c
/* arch/arm64/include/asm/mmu.h */
typedef struct {
    atomic64_t      id;          // ASID: [bits 63:16] = generation, [bits 15:0] = ASID value
    void            *vdso;       // virtual DSO mapping pointer
    unsigned long   flags;       // MM_CTX_HAS_ASID, etc.
#ifdef CONFIG_COMPAT
    void            *sigpage;    // 32-bit signal return trampoline
#endif
} mm_context_t;

ASID (Address Space Identifier) in mm_context_t.id:
  Upper 48 bits: ASID generation (epoch)
  Lower 16 bits: actual ASID value (8-bit or 16-bit ASID hardware)
  
  ARM64 8-bit ASID: 256 possible values
  ARM64 16-bit ASID: 65536 possible values (FEAT_ASID16)
  
  ASID assignment:
    new_context() in arch/arm64/mm/context.c
    TTBR0_EL1[63:48] = ASID (for ASID-tagged userspace TLB entries)

pgd pointer in mm_struct:
  mm->pgd: kernel virtual address of the process's PGD (page global directory)
  Physical address: __pa(mm->pgd) → used for TTBR0_EL1
  
  switch_mm() in ARM64:
    void switch_mm(struct mm_struct *prev, struct mm_struct *next, struct task_struct *tsk)
      → check_and_switch_context(next)  // allocate/check ASID
      → cpu_switch_mm(next->pgd, next)   // update TTBR0_EL1
      
  cpu_switch_mm() (arch/arm64/include/asm/mmu_context.h):
    ASID = (next->context.id >> USER_ASID_BIT) & ~ASID_MASK
    TTBR0 = __pa(next->pgd) | (ASID << 48)
    MSR TTBR0_EL1, TTBR0
    ISB
```

---

## 4. mm_struct Lifecycle

```
Process creation (fork):
  sys_fork() → kernel_clone() → copy_mm() → dup_mm()
  
  dup_mm():
    1. mm_alloc(): allocate new mm_struct, zero-initialize
    2. Copy mm fields (stack limits, code/data ranges)
    3. Copy page tables: copy_page_range()
       - For each VMA: copy PTEs with COW flag
       - Set PTE as read-only (wp = write-protect)
       - Child and parent share physical pages (COW)
    4. Allocate new PGDIR: pgd_alloc()
       - Copy kernel page tables from swapper_pg_dir
       - Set mm->pgd = new PGD
    5. Increment mm_users, mm_count
    
  After fork: parent and child share physical pages (COW)
  Write fault on shared page → do_wp_page() → copy page, set writable

execve():
  exec_mmap():
    1. Create NEW mm_struct (mm_alloc)
    2. Activate new mm: activate_mm(old_mm, new_mm)
    3. mmdrop(old_mm): drop reference to old mm
    4. New mm: map ELF sections, stack, vdso, etc.

exit():
  do_exit() → exit_mm() → mmput()
    mm_users-- → if 0: __mmput() → exit_mmap() → unmap_vmas() → TLB flush
    mm_count-- → if 0: __mmdrop() → free page tables + pgd + mm_struct itself
```

---

## 5. Memory Layout on ARM64

```
ARM64 48-bit user VA layout (typical, with ASLR):

0x0000_0000_0000 – 0x0000_0000_FFFF: first 64KB (null pointer guard, usually unmapped)
0x0000_0000_8000: typical ELF load address (PIE executables: randomized)
...
0x0000_7FFF_FFFF_FFFF: end of user address space (512 TB, 4-level, 48-bit VA)

Sections (from /proc/<pid>/maps):
  [text]:    start_code → end_code     (ELF PT_LOAD, r-xp)
  [data]:    start_data → end_data     (ELF PT_LOAD, rw-p)
  [heap]:    start_brk → brk           (grows up via brk() syscall)
  [mmap]:    mmap_base down            (shared libs, anonymous mmaps, file mappings)
             (Flexible layout: mmap grows DOWN from mmap_base)
  [stack]:   TASK_SIZE - 8MB upward    (grows down, guarded by stack_vm)
  [vdso]:    VDSO_BASE                 (virtual dynamic shared object: time, etc.)

ARM64 mm_struct.task_size:
  48-bit VA: 0x0001_0000_0000_0000 (256 TB)
  52-bit VA: 0x0010_0000_0000_0000 (4 PB) [FEAT_LVA]
  
  task_size = TASK_SIZE = (user_va_size)
  All VMAs must have: vma->vm_end <= task_size

Kernel virtual address space (TTBR1_EL1):
  0xFFFF_0000_0000_0000 and above
  Separate page table: swapper_pg_dir (not in mm_struct)
```

---

## 6. Interview Questions & Answers

**Q1: A thread group has 4 threads. How many mm_structs exist? What happens to the mm_struct when one thread exits vs when the entire process exits?**

**4 threads → 1 mm_struct.** All threads created with `CLONE_VM` share the SAME `mm_struct`. Each thread has its own `task_struct`, but `task_struct->mm` points to the SAME `mm_struct`. The reference count `mm_users` = 4 (one per thread).

**When one thread exits**: `do_exit()` calls `exit_mm()` → `mmput()`. This decrements `mm_users` to 3. Since `mm_users > 0`, the `mm_struct` is NOT freed — the other threads still use it. The exiting thread's `task_struct->mm` is set to NULL (task becomes a "zombie" briefly).

**When the last thread exits**: `mm_users` drops to 0. `__mmput()` is called → `exit_mmap()` unmaps all VMAs, does TLB flush, frees page tables. Then `mmdrop()` is called → `mm_count` drops; when 0, `__mmdrop()` frees the `mm_struct` itself. The ASID is also released back to the ASID pool.

**mm_count vs mm_users**: `mm_users` counts threads that have the mm active. `mm_count` counts all holders including kernel code that may have grabbed a reference via `mmget()` (e.g., the OOM killer inspecting the mm). This two-level reference counting ensures the `mm_struct` isn't freed while kernel code is examining it.

---

## 7. Quick Reference

| Field | Type | Purpose |
|---|---|---|
| mm->pgd | pgd_t * | Root of page table (→ TTBR0_EL1) |
| mm->mm_users | atomic_t | User-space reference count |
| mm->mm_count | atomic_t | Total reference count |
| mm->mmap_lock | rw_semaphore | Protects VMA operations |
| mm->context.id | atomic64_t | ASID (generation:value) |
| mm->brk | unsigned long | Current heap top |
| mm->task_size | unsigned long | VA space limit |
| mm->rss_stat | mm_rss_stat | Resident pages counter |

| Event | mm_users change | mm_count change |
|---|---|---|
| fork() | +1 (new process) | +1 |
| Thread create (CLONE_VM) | +1 | +1 |
| Thread exit | -1 | -1 |
| mmget() (kernel ref) | 0 | +1 |
| mmdrop() | 0 | -1 |
