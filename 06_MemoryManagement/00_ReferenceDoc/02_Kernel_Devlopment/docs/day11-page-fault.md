# Day 11 — Page Faults & `copy_{from,to}_user`

> **Goal**: Route synchronous data-aborts to `do_page_fault()`; distinguish kernel-mode oops vs user-mode segfault (will deliver SIGSEGV once Day 19 lands signals); implement `copy_from_user` / `copy_to_user` with an exception fixup table so a bad user pointer **does not crash the kernel**.
>
> **Why today**: Syscalls (Day 17) and ELF loading (Day 18) must accept user pointers and tolerate invalid ones. Page faults on demand also unlock lazy stack growth and copy-on-write later.

---

## 1. Background

### 1.1 Data abort syndrome
`ESR_EL1` on data abort:
- `EC = 0x24` (lower EL) or `0x25` (same EL).
- `ISS[5:0] = FSC` (fault status code):
  - `0b00010x` translation fault levels 0..3
  - `0b00100x` access flag fault
  - `0b00110x` permission fault
- `ISS.WnR` (bit 6) = write (1) or read (0).
- `ISS.CM` (bit 8) = cache maint.
- `FAR_EL1` = faulting VA.

### 1.2 Kernel vs user fault decision
```
if (ELR_EL1 in kernel range && current EL == 1):
    if (FAR within current task's VMA && lazy fixup possible)
        install page, return
    else if (ELR ∈ __ex_table)
        // user-pointer-touching code; redirect to fixup
        ELR = fixup_addr; return
    else
        oops
else (user fault from EL0):
    if (FAR ∈ user vma && handleable)
        demand-allocate page
    else
        SIGSEGV / kill task
```

### 1.3 Exception fixup table
For each kernel instruction that might dereference a user pointer (`ldr xN, [user_ptr]`), emit a record:
```
struct exception_entry { u64 fault_ip; u64 fixup_ip; };
```
Stored in a `.ex_table` section. On kernel fault, binary-search for `fault_ip`; if found, set `ELR_EL1 = fixup_ip` and return; the fixup sets `x0 = -EFAULT`.

---

## 2. Design

### 2.1 Files
```
mm/page_fault.c
mm/uaccess.c
include/kernel/uaccess.h
arch/arm64/include/asm/uaccess.h    (asm helpers w/ .ex_table)
linker.ld                            (add .ex_table section)
```

### 2.2 Linker addition
```ld
.ex_table : ALIGN(8) {
    __ex_table_start = .;
    KEEP(*(.ex_table))
    __ex_table_end = .;
}
```

---

## 3. Implementation

### 3.1 `mm/page_fault.c`
```c
#include <kernel/printk.h>
#include <kernel/panic.h>
#include <asm-arm64/pt_regs.h>

extern struct exception_entry __ex_table_start[], __ex_table_end[];
struct exception_entry { u64 fault_ip; u64 fixup_ip; };

static u64 search_fixup(u64 ip)
{
    /* simple linear; switch to binsearch when table grows */
    for (struct exception_entry *e = __ex_table_start; e < __ex_table_end; e++)
        if (e->fault_ip == ip) return e->fixup_ip;
    return 0;
}

void do_page_fault(struct pt_regs *r, u64 esr, u64 far)
{
    u32 fsc = esr & 0x3f;
    int write = (esr >> 6) & 1;
    int from_el0 = ((esr >> 26) & 0x3f) == 0x24;

    printk(KERN_WARN "page fault: FAR=%p ELR=%p %s, FSC=0x%x, %s\n",
           (void *)far, (void *)r->pc,
           write ? "write" : "read", fsc,
           from_el0 ? "user" : "kernel");

    if (!from_el0) {
        u64 fix = search_fixup(r->pc);
        if (fix) {
            r->pc = fix;
            r->regs[0] = (u64)-14;        /* -EFAULT */
            return;
        }
        panic("kernel page fault unhandled");
    }

    /* From EL0: defer to current task's VMA handler (Day 15) */
    extern int handle_user_fault(u64 addr, int write);
    if (handle_user_fault(far, write) == 0) return;

    /* Unhandled → SIGSEGV (Day 19) → for now, kill */
    extern void do_exit(int code);
    do_exit(11 /* SIGSEGV */);
}
```

Update `do_sync` (Day 4) to dispatch:
```c
void do_sync(struct pt_regs *r) {
    u64 esr, far;
    asm volatile("mrs %0, esr_el1" : "=r"(esr));
    asm volatile("mrs %0, far_el1" : "=r"(far));
    u32 ec = esr >> 26;
    switch (ec) {
    case 0x24: case 0x25:   /* data abort */
    case 0x20: case 0x21:   /* inst abort */
        do_page_fault(r, esr, far); return;
    case 0x15:              /* SVC */
        extern void do_syscall(struct pt_regs *);
        do_syscall(r); return;
    default:
        panic("unhandled sync EC=%x", ec);
    }
}
```

### 3.2 `mm/uaccess.c` with fixup
```c
#include <kernel/uaccess.h>

long copy_from_user(void *dst, const void __user *src, u64 n)
{
    /* User pointer must be in TTBR0 range (high bit 0) */
    if ((u64)src >> 47) return -14;
    u8 *d = dst; const u8 *s = src;
    while (n--) {
        u8 v;
        asm volatile(
            "1: ldrb %w0, [%1]\n"
            "   .pushsection .ex_table, \"a\"\n"
            "   .balign 8\n"
            "   .quad   1b, 99f\n"
            "   .popsection\n"
            : "=&r"(v) : "r"(s) : "memory");
        *d++ = v; s++;
    }
    return 0;
99: return -14;                                /* -EFAULT */
}

long copy_to_user(void __user *dst, const void *src, u64 n)
{
    if ((u64)dst >> 47) return -14;
    u8 *d = dst; const u8 *s = src;
    while (n--) {
        asm volatile(
            "1: strb %w0, [%1]\n"
            "   .pushsection .ex_table, \"a\"\n"
            "   .quad   1b, 99f\n"
            "   .popsection\n"
            :: "r"((u32)*s), "r"(d) : "memory");
        d++; s++;
    }
    return 0;
99: return -14;
}
```

> The `99:` label inside C is a GCC extension via inline-asm; alternatively, write the whole routine in `.S` for clarity. A real implementation uses word-sized copies and an alignment prologue.

### 3.3 Demand zero-fill anonymous mapping (preview Day 15)
```c
int handle_user_fault(u64 va, int write)
{
    extern struct task *current;
    struct vma *v = find_vma(current->mm, va);
    if (!v) return -1;
    if (write && !(v->prot & PROT_WRITE)) return -1;
    /* allocate, zero, map */
    phys_addr_t pa = alloc_pages(0);
    if (!pa) return -1;
    map_4k(current->mm->pgd, va & PAGE_MASK, pa,
           PTE_PAGE | PTE_VALID | PTE_AF | PTE_SH_INNER |
           PTE_ATTR(MT_NORMAL) | PTE_AP_RW_ANY | PTE_NG | PTE_UXN);
    asm volatile("dsb sy; isb");
    return 0;
}
```

---

## 4. Pitfalls

1. **Fault recursion**: a fault inside `do_page_fault` (e.g., printk → some unmapped buffer) loops forever. Mask debug/`#define DEBUG_PFAULT 0`, route to emergency UART.
2. **Stack-pointer fault**: if `SP_EL1` itself is bad (kernel stack overflow), abort takes vector path which **uses** SP_EL1 → triple-fault. Use a separate IRQ stack later (Day 28).
3. **TLB stale**: after installing new PTE on fault return, ensure `dsb sy; isb` so the retry sees it. (No TLBI needed because no prior valid entry existed.)
4. **`.ex_table` ordering**: if you keep entries sorted by `fault_ip`, you can binsearch. We use linear for clarity.
5. **Endianness of inline asm output**: keep it AArch64 little-endian — never use `.long` for 64-bit address; use `.quad`.

---

## 5. Verification (Phase 2 Gate)

```c
/* Kernel-side smoke */
long r = copy_from_user(buf, (void __user *)0x123, 64);
printk("copy_from_user => %ld\n", r);   /* -14 */
```

Stress: alloc/free 100k pages via `kmalloc`/`alloc_pages`; deliberately access an unmapped user VA from a kthread set up with a tiny `mm` (Day 15 preview); expect graceful kill.

Phase 2 gate (full): `buddy_stats()` before and after stress is **byte-identical** → no leaks.

---

## 6. Stretch

- Decode FSC into strings ("translation fault L2", "permission fault L3"...).
- Add `get_user(x, p)` macro returning value + status; map onto a single `ldr`+fixup.
- Page-fault stat counters per type (translation/permission/access).

---

## 7. References

- ARM ARM §D13.2.40 (`ESR_EL1`), Table D13-43 (FSC values).
- Linux `arch/arm64/mm/fault.c`, `arch/arm64/lib/copy_from_user.S`.
- LWN: "The user-space fault model".
