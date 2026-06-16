# Page Faults in Linux — Deep Dive (Interview Perspective)

## 1. What is a Page Fault?

A **page fault** is a **CPU exception** (synchronous trap) raised by the **MMU (Memory Management Unit)** when a process accesses a virtual address and the translation cannot be completed successfully via the page tables.

It does **not** always mean an error — most page faults in Linux are **normal and expected** (demand paging, COW, etc.).

---

## 2. Why Page Faults Happen — The 3 Root Causes

When the CPU walks the page table (via MMU) for a virtual address, the fault occurs if:

| Cause | Description |
|-------|-------------|
| **1. Page not present** | PTE has `Present` bit = 0 (page not in RAM — never allocated, or swapped out) |
| **2. Permission violation** | Write to read-only page, user accessing kernel page, exec on NX page |
| **3. Reserved bit / malformed PTE** | Hardware-level corruption (rare) |

---

## 3. Types of Page Faults in Linux

```
                    Page Fault
                        |
        ----------------------------------
        |                                |
   Valid Fault                    Invalid Fault
   (Handled OK)                   (SIGSEGV / OOPS)
        |
   --------------------------------------
   |          |          |              |
 Minor      Major       COW          Demand
 Fault      Fault      Fault         Paging
```

### 3.1 Minor Fault (Soft Fault)
- Page is **already in RAM**, but PTE is not set up for this process.
- Example: shared libraries already cached, `fork()` shared pages.
- **No disk I/O** → fast.

### 3.2 Major Fault (Hard Fault)
- Page **must be fetched from disk** (swap or file-backed mmap).
- Involves block I/O → slow.
- Counted in `/proc/<pid>/stat` (`majflt`).

### 3.3 Copy-on-Write (COW) Fault
- After `fork()`, parent & child share pages as **read-only**.
- On write → fault → kernel allocates new page, copies content, updates PTE writable.

### 3.4 Demand Paging Fault
- `malloc()` / `mmap()` only reserves VMA; no physical page allocated.
- First access → fault → kernel allocates a zeroed page (anonymous) or reads from file.

### 3.5 Invalid Fault → SIGSEGV
- Address not in any VMA, or permission mismatch → kernel sends `SIGSEGV` to user process.
- In kernel mode → **OOPS / kernel panic** (unless fixup exists, e.g., `copy_from_user`).

---

## 4. Hardware Side (ARM64 Example)

On ARM64 a memory access fault triggers a **synchronous exception** delivered to **EL1**:
- Exception class (`ESR_EL1.EC`) = `0x24` (data abort from lower EL) or `0x25` (same EL).
- Fault address stored in **`FAR_EL1`** (Fault Address Register).
- Fault status (cause) in **`ESR_EL1.ISS`** (e.g., translation fault, permission fault, access flag fault).

On x86: fault address in **`CR2`**, error code pushed on stack.

---

## 5. End-to-End Kernel Flow (ARM64)

```
User/Kernel access bad VA
        │
        ▼
   MMU walks page table → fault
        │
        ▼
   CPU raises Synchronous Exception → EL1
        │
        ▼
   vectors (arch/arm64/kernel/entry.S)
        │
        ▼
   el1_sync / el0_sync   →   do_mem_abort()
        │
        ▼
   fault_info[] table dispatch (by ESR_EL1.ISS)
        │
        ▼
   do_translation_fault() / do_page_fault()
        │
        ▼
   __do_page_fault()
        │
        ▼
   handle_mm_fault()        ◄── core MM entry (generic)
        │
        ▼
   handle_pte_fault()
        │
   ┌────┴───────────────────────────────┐
   │                                    │
do_anonymous_page()                do_swap_page()
do_fault() (file-backed)           do_wp_page() (COW)
   │
   ▼
Allocate page (alloc_pages) → update PTE → flush TLB → return
        │
        ▼
   Exception return (eret) → re-execute faulting instruction
```

---

## 6. Key Kernel Functions Explained

### `do_page_fault()` — arch entry point
Responsibilities:
1. Get faulting address (`FAR_EL1` / `CR2`).
2. Determine if fault from user or kernel mode.
3. Find the **VMA** covering the address using `find_vma()`.
4. Validate access permissions vs `vma->vm_flags`.
5. Call generic `handle_mm_fault()`.
6. Handle failures → `SIGSEGV` / `SIGBUS` / kernel OOPS / extable fixup.

### `find_vma(mm, addr)`
Searches the process's `mm_struct->mm_mt` (maple tree, formerly RB-tree) for a VMA where `addr < vm_end`. If `addr < vma->vm_start` and VMA is a growable stack → `expand_stack()`.

### `handle_mm_fault()` → `__handle_mm_fault()` → `handle_pte_fault()`
Walks through page table levels: **PGD → P4D → PUD → PMD → PTE**, allocating missing intermediate levels, then dispatches based on PTE state:

| PTE State | Handler |
|-----------|---------|
| Empty + anonymous VMA | `do_anonymous_page()` |
| Empty + file VMA | `do_fault()` → `do_read_fault()` / `do_cow_fault()` / `do_shared_fault()` |
| Non-present, swap entry | `do_swap_page()` |
| Present + write fault on RO | `do_wp_page()` (COW) |
| Present, no access flag | `handle_pte_fault()` sets young/access bit |

---

## 7. Key Data Structures

```c
struct mm_struct {
    struct maple_tree mm_mt;   // all VMAs
    pgd_t *pgd;                // top-level page table
    atomic_t mm_users, mm_count;
    unsigned long start_code, end_code, start_data, ...;
};

struct vm_area_struct {
    unsigned long vm_start, vm_end;
    unsigned long vm_flags;      // VM_READ | VM_WRITE | VM_EXEC | VM_SHARED
    struct file *vm_file;        // NULL for anonymous
    const struct vm_operations_struct *vm_ops; // ->fault() callback
    pgprot_t vm_page_prot;
};
```

`vm_ops->fault()` is the **driver/filesystem hook** the kernel calls to populate a page (e.g., filemap_fault for page cache).

---

## 8. Detailed Scenarios (Interview Favorites)

### A. `malloc()` + first access
1. `malloc` → `brk()`/`mmap()` syscall → only a **VMA** created, no PTE.
2. CPU writes to address → fault → `do_anonymous_page()` →
3. Allocates **zeroed page** (`alloc_zeroed_user_highpage_movable`) → installs PTE → returns.
4. Re-executes the instruction successfully.

### B. `fork()` + write by child (COW)
1. `fork()` → `copy_page_range()` marks all writable PTEs **read-only** in both parent & child; increments `_refcount` of physical pages.
2. Child writes → permission fault → `do_wp_page()`.
3. If page refcount > 1 → allocate new page, copy data, install writable PTE.
4. If refcount == 1 → just make PTE writable (reuse).

### C. File-backed mmap read
1. `mmap(file)` → VMA with `vm_file` set, `vm_ops = generic_file_vm_ops`.
2. Read access → fault → `do_read_fault()` → calls `vma->vm_ops->fault()` → `filemap_fault()`.
3. Looks up page in **page cache**; if miss → `readpage()` issues block I/O (**major fault**).
4. PTE installed pointing to page cache page (read-only for now).

### D. Swap-in
1. PTE has swap entry (not present, encoded with swap type+offset).
2. Fault → `do_swap_page()` → `swap_readpage()` → read from swap device → restore PTE.

### E. Invalid access (SIGSEGV)
1. `find_vma()` returns NULL or `addr < vma->vm_start` (not stack).
2. `bad_area()` → `force_sig_fault(SIGSEGV, SEGV_MAPERR, addr)`.

### F. Kernel mode fault on user address (e.g., `copy_from_user`)
1. Fault in kernel → check **exception tables** (`search_exception_tables()`).
2. If fixup exists → jump to fixup code → return `-EFAULT`.
3. Else → **OOPS** / kernel panic.

---

## 9. Return Value Flags from `handle_mm_fault()` — `vm_fault_t`

| Flag | Meaning |
|------|---------|
| `VM_FAULT_MAJOR` | Major fault occurred (count `majflt`) |
| `VM_FAULT_OOM` | Out of memory |
| `VM_FAULT_SIGBUS` | Send SIGBUS (file truncated past mmap) |
| `VM_FAULT_SIGSEGV` | Send SIGSEGV |
| `VM_FAULT_RETRY` | Released mmap_lock, retry needed |
| `VM_FAULT_NOPAGE` | Handler installed PTE directly |

---

## 10. Locking

- **`mm->mmap_lock`** (rwsem) — protects VMA list. Taken read-side in fault path.
- Modern kernels (≥6.0) support **per-VMA locks (SPF — Speculative Page Faults)** to reduce contention.
- **PTE lock** (`pte_offset_map_lock`) — fine-grained per page table page.

---

## 11. Performance Counters

- `/proc/<pid>/stat` → `minflt`, `majflt`
- `perf stat -e page-faults,minor-faults,major-faults ./app`
- `vmstat` → `pgfault`, `pgmajfault`

---

## 12. Common Interview Questions & Crisp Answers

**Q1. Difference between minor & major fault?**
Minor → page already in RAM, just fix PTE. Major → disk I/O needed.

**Q2. What happens on `*ptr = 5` where ptr from `malloc`?**
First access faults → `do_anonymous_page` allocates a zero page → PTE installed → instruction retried.

**Q3. How does COW work?**
After fork, all writable pages marked RO. Write triggers fault → `do_wp_page` copies the page if shared.

**Q4. What's `vm_fault_t`?**
Bitmask returned by `handle_mm_fault()` indicating fault outcome (retry, OOM, major, etc.).

**Q5. How does kernel know if an address is valid?**
`find_vma()` searches process VMAs in maple tree; checks `vm_flags` for permission.

**Q6. What happens if page fault occurs in interrupt context?**
`faulthandler_disabled()` true → kernel cannot sleep → fault treated as bug → OOPS.

**Q7. Difference SIGSEGV vs SIGBUS?**
SIGSEGV = invalid VA (no VMA / perm). SIGBUS = valid VMA but underlying object gone (e.g., mmap’d file truncated, or unaligned access).

**Q8. What is demand paging?**
Lazy allocation — physical page allocated only on first access via page fault.

**Q9. Why mark pages RO after fork instead of copying?**
Saves memory and CPU; copy only happens on write, often never (e.g., `exec` immediately).

**Q10. How does kernel handle fault for kernel virtual address?**
Vmalloc area faults sync per-process PGD with `init_mm` PGD (`vmalloc_fault()` on x86; on ARM64 not needed since kernel PGD is global via TTBR1).

---

## 13. One-Line Summary

> A page fault is an MMU-generated exception that Linux uses as a **mechanism**, not just an error — enabling demand paging, COW, swapping, mmap, and lazy allocation through a unified handler chain: **exception vector → `do_page_fault` → `handle_mm_fault` → PTE-specific handler → fix PTE → retry instruction.**

If you'd like, I can next explain any of these in more depth — e.g., **ARM64 ESR decoding**, **COW implementation in `do_wp_page`**, **swap path internals**, or **per-VMA locking (SPF)**.
