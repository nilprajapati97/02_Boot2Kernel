# 04 — Page Faults

## 1. What is a Page Fault?

A **page fault** is an exception triggered by the MMU when a virtual address cannot be translated:

| Type | Cause | Action |
|------|-------|--------|
| Minor (soft) | Page mapped but not in memory (demand paging, swap) | Load from swap/file |
| Major (hard) | Page not mapped at all | Load from disk |
| Spurious | Race condition, resolved without I/O | Return immediately |
| Invalid/Bad | Access violation, NULL deref, stack overflow | SIGSEGV |

---

## 2. Page Fault Handler Flow

```mermaid
flowchart TD
    A["MMU: Cannot translate VA"] --> B["#PF exception → do_page_fault()"]
    B --> C{Address in user or kernel space?}
    C -- Kernel --> D{Valid kernel mapping?}
    D -- No --> E[oops: BUG / kernel panic]
    D -- Yes --> F[fixup_exception — handle known faults]
    C -- User --> G["find_vma(mm, address)"]
    G --> H{VMA found?}
    H -- No --> I[SIGSEGV — bad address]
    H -- Yes --> J{Access matches VMA permissions?}
    J -- No --> K[SIGSEGV — access violation]
    J -- Yes --> L["handle_mm_fault()"]
    L --> M{PTE present?}
    M -- No + anonymous --> N["alloc_zeroed_page()— demand alloc"]
    M -- No + file --> O["Read page from disk\n(major fault)"]
    M -- Present + COW --> P["Copy-on-write: copy page"]
    N & O & P --> Q["install PTE, return to user"]
```

---

## 3. Key Functions

```c
/* arch/x86/mm/fault.c */
DEFINE_IDTENTRY_RAISIRF(page_fault)
{
    do_page_fault(regs, error_code, address);
}

/* mm/memory.c */
vm_fault_t handle_mm_fault(struct vm_area_struct *vma,
                            unsigned long address,
                            unsigned int flags,
                            struct pt_regs *regs);
```

---

## 4. Demand Paging

```mermaid
sequenceDiagram
    participant Proc as Process
    participant MMU
    participant PF as Page Fault Handler
    participant MM as Memory Manager
    participant PA as Physical Memory

    Proc->>MMU: Access virtual address 0x500000
    MMU->>Proc: #PF — PTE not present
    Proc->>PF: do_page_fault(0x500000)
    PF->>MM: handle_mm_fault()
    MM->>PA: alloc_zeroed_user_highpage()
    PA-->>MM: struct page
    MM->>MM: set_pte() — install PTE
    MM-->>PF: VM_FAULT_MINOR
    PF-->>Proc: Return (retry instruction)
    Proc->>MMU: Access virtual address 0x500000
    MMU-->>Proc: PA mapped — success
```

---

## 5. Copy-on-Write (COW) Fault

```c
/*
 * Fork: parent and child share same pages, both read-only.
 * On first write by child → COW page fault:
 */
static vm_fault_t do_cow_fault(struct vm_fault *vmf)
{
    struct page *old_page = vm_normal_page(...);
    struct page *new_page = alloc_page_vma(GFP_HIGHUSER_MOVABLE, ...);
    
    copy_user_highpage(new_page, old_page, ...);
    /* Install new writable PTE for new_page */
    /* unmap old_page from this process */
}
```

---

## 6. vm_fault Structure

```c
struct vm_fault {
    const struct {
        struct vm_area_struct *vma;  /* Faulting VMA */
        gfp_t           gfp_mask;
        pgoff_t         pgoff;       /* Logical page offset */
        unsigned long   address;     /* Faulting virtual address */
        unsigned long   real_address;
    };
    enum fault_flag     flags;   /* FAULT_FLAG_WRITE, FAULT_FLAG_USER */
    pmd_t               *pmd;
    pud_t               *pud;
    pte_t               orig_pte;
    struct page         *cow_page;  /* Pre-allocated COW page */
    struct page         *page;      /* Resulting page */
    pte_t               *pte;
    spinlock_t          *ptl;       /* PT lock */
};
```

---

## 7. Source Files

| File | Description |
|------|-------------|
| `arch/x86/mm/fault.c` | x86 page fault entry |
| `mm/memory.c` | `handle_mm_fault()`, COW, demand alloc |
| `mm/swapin.c` | Swap page faults |
| `mm/filemap.c` | File-backed page faults |

---

## 8. Related Topics
- [03_Page_Tables.md](./03_Page_Tables.md)
- [../02_Process_Management/04_Copy_On_Write.md](../02_Process_Management/04_Copy_On_Write.md)
- [05_mmap.md](./05_mmap.md)
