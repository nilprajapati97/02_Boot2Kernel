# Day 15 — `kthread_create`, `do_fork` Skeleton, `mm_struct`

> **Goal**: Generalize task creation into a `do_fork` that can produce either a kernel thread or a user process (sharing or duplicating `mm_struct`); model address spaces with VMAs.
>
> **Why today**: Day 16 drops to EL0 — that requires a `task` with a user `mm` containing at least one VMA for code. Day 17 syscalls (`fork`, `brk`, `mmap`) all manipulate the `mm`.

---

## 1. Background

### 1.1 `mm_struct` and VMAs
Linux model (simplified for us):
```c
struct vm_area_struct {
    u64 start, end;            /* VA range, page-aligned */
    u32 prot;                  /* PROT_READ|WRITE|EXEC */
    u32 flags;                 /* MAP_ANONYMOUS, MAP_FIXED, etc. */
    struct file *file;         /* file-backed mapping (Day 20+) */
    u64 file_off;
    struct vma *next;
};

struct mm_struct {
    u64 *pgd;                  /* page-table root (L0) for this user */
    u16 asid;
    struct vma *vma_list;      /* sorted by start */
    u64 start_code, end_code;
    u64 start_data, end_data;
    u64 start_brk,  brk;
    u64 start_stack;
    int refcount;
};
```

### 1.2 Clone flags (subset; align with Linux UAPI)
| Flag | Value | Meaning |
|---|---|---|
| CLONE_VM | 0x100 | share mm |
| CLONE_FS | 0x200 | share fs root |
| CLONE_FILES | 0x400 | share file table |
| CLONE_SIGHAND | 0x800 | share signals |
| CLONE_THREAD | 0x10000 | same TGID |

For now we implement two paths:
- **kthread**: `CLONE_VM` (use kernel mm — actually NULL).
- **user fork**: full copy of mm, file table, etc. **No COW** yet — slow but simple.

### 1.3 ASID (Address Space ID)
- 16-bit field in `TTBR0_EL1[63:48]`.
- Tags TLB entries so we can switch user MMs without `tlbi vmalle`.
- Allocate ASIDs from a small bitmap; rollover handled with global TLB flush.

---

## 2. Design

### 2.1 Files
```
kernel/fork.c
kernel/mm.c              (mm_alloc/free, find_vma, insert_vma)
include/kernel/mm.h      (extended with mm_struct + vma)
arch/arm64/mm/asid.c
```

### 2.2 `do_fork()` skeleton
```c
int do_fork(u32 flags, u64 user_sp, void (*entry)(void *), void *arg);
```
Returns child pid in parent, 0 in child (mirrors Linux semantics).

---

## 3. Implementation

### 3.1 `kernel/mm.c`
```c
#include <kernel/mm.h>
#include <kernel/slab.h>
#include <kernel/page.h>
#include <asm-arm64/pgtable.h>

struct mm_struct *mm_alloc(void)
{
    struct mm_struct *mm = kmalloc(sizeof *mm, 0);
    memset(mm, 0, sizeof *mm);
    phys_addr_t pgd_pa = alloc_pages(0);
    mm->pgd = (u64 *)(pgd_pa + 0xffff000000000000UL);
    mm->refcount = 1;
    mm->asid = asid_alloc();
    return mm;
}

void mm_free(struct mm_struct *mm)
{
    if (--mm->refcount > 0) return;
    /* unmap all VMAs */
    for (struct vma *v = mm->vma_list; v; ) {
        struct vma *n = v->next;
        for (u64 va = v->start; va < v->end; va += PAGE_SIZE) {
            phys_addr_t pa = walk_pa(mm->pgd, va);
            if (pa) { unmap_4k(mm->pgd, va); free_pages(pa, 0); }
        }
        kfree(v);
        v = n;
    }
    free_pages((u64)mm->pgd - 0xffff000000000000UL, 0);
    asid_free(mm->asid);
    kfree(mm);
}

struct vma *find_vma(struct mm_struct *mm, u64 va)
{
    for (struct vma *v = mm->vma_list; v; v = v->next)
        if (va >= v->start && va < v->end) return v;
    return NULL;
}

void insert_vma(struct mm_struct *mm, struct vma *v)
{
    struct vma **pp = &mm->vma_list;
    while (*pp && (*pp)->start < v->start) pp = &(*pp)->next;
    v->next = *pp;
    *pp = v;
}
```

### 3.2 `arch/arm64/mm/asid.c`
```c
#define NR_ASIDS 256
static u64 asid_bitmap[NR_ASIDS / 64];
static spinlock_t asid_lock;

u16 asid_alloc(void)
{
    unsigned long f; spin_lock_irqsave(&asid_lock, f);
    for (int i = 1; i < NR_ASIDS; i++) {
        if (!(asid_bitmap[i>>6] & (1UL << (i & 63)))) {
            asid_bitmap[i>>6] |= 1UL << (i & 63);
            spin_unlock_irqrestore(&asid_lock, f);
            return i;
        }
    }
    spin_unlock_irqrestore(&asid_lock, f);
    /* TODO rollover: flush all TLBs, reset bitmap, restart */
    return 0;
}
void asid_free(u16 a) { /* clear bit */ }
```

### 3.3 `kernel/fork.c`
```c
#include <kernel/sched.h>
#include <kernel/task.h>
#include <kernel/mm.h>
#include <kernel/string.h>

extern struct task *task_create(void (*)(void *), void *, const char *);

static struct mm_struct *dup_mm(struct mm_struct *src)
{
    if (!src) return NULL;
    struct mm_struct *new = mm_alloc();
    for (struct vma *v = src->vma_list; v; v = v->next) {
        struct vma *nv = kmalloc(sizeof *nv, 0);
        *nv = *v; nv->next = NULL;
        insert_vma(new, nv);
        /* Eagerly copy pages (no COW). */
        for (u64 va = v->start; va < v->end; va += PAGE_SIZE) {
            phys_addr_t spa = walk_pa(src->pgd, va);
            if (!spa) continue;
            phys_addr_t dpa = alloc_pages(0);
            memcpy((void *)(dpa + 0xffff000000000000UL),
                   (void *)(spa + 0xffff000000000000UL),
                   PAGE_SIZE);
            map_4k(new->pgd, va, dpa, /* user attrs */ PTE_PAGE | PTE_VALID | PTE_AF | PTE_SH_INNER | PTE_ATTR(0) | PTE_AP_RW_ANY | PTE_NG | PTE_UXN);
        }
    }
    new->start_code = src->start_code; new->end_code = src->end_code;
    new->start_brk  = src->start_brk;  new->brk      = src->brk;
    new->start_stack = src->start_stack;
    return new;
}

int do_fork(u32 flags, u64 user_sp, void (*entry)(void *), void *arg)
{
    struct task *parent = current();
    struct task *child  = task_create(entry, arg, parent->comm);
    if (!child) return -1;

    if (flags & CLONE_VM) child->mm = parent->mm;     /* share */
    else if (parent->mm)  child->mm = dup_mm(parent->mm);
    else                  child->mm = NULL;            /* pure kthread */

    if (parent->mm) parent->mm->refcount++;

    /* If user fork, child returns 0 from syscall; arrange via pt_regs in Day 17. */

    sched_add(child);
    return child->pid;
}
```

### 3.4 `kthread_create` wrapper
```c
struct task *kthread_create(void (*fn)(void *), void *arg, const char *name)
{
    struct task *t = task_create(fn, arg, name);
    sched_add(t);
    return t;
}
```

### 3.5 Switching TTBR0 on context switch (user procs)
Update `cpu_switch_to` (or surrounding C wrapper) to load `TTBR0_EL1`:
```c
void __switch_to(struct task *prev, struct task *next)
{
    if (next->mm) {
        u64 ttbr = ((u64)next->mm->pgd - 0xffff000000000000UL) | ((u64)next->mm->asid << 48);
        asm volatile("msr ttbr0_el1, %0; isb" :: "r"(ttbr));
    }
    cpu_switch_to(prev, next);
}
```
Now `schedule()` calls `__switch_to` instead of raw `cpu_switch_to`.

---

## 4. Pitfalls

1. **Sharing mm without incrementing refcount** → use-after-free on first exit. Always `mm->refcount++`.
2. **Forgetting to flush user TLB on `mm_free`**: invalid leftover entries cause faults on next ASID reuse. `tlbi aside1is, x_asid`.
3. **VMA list not sorted**: `find_vma` becomes O(N) anyway, but inserting unsorted breaks merge later. Keep sorted.
4. **Copying PTE attributes blindly**: child must NOT inherit `PG_BUDDY` semantics; copy only relevant attrs (R/W/X, user).
5. **Race between fork and scheduler**: `sched_add` after `child->state = TASK_RUNNING` only. Otherwise scheduler picks half-baked task.

---

## 5. Verification

```c
struct task *a = kthread_create(thr_a, NULL, "A");
struct task *b = kthread_create(thr_b, NULL, "B");
printk("forked pid=%d %d\n", a->pid, b->pid);
```

Build a tiny user mm manually:
```c
struct mm_struct *mm = mm_alloc();
struct vma *v = kmalloc(sizeof *v, 0);
v->start = 0x400000; v->end = 0x401000;
v->prot = PROT_READ|PROT_EXEC; v->flags = MAP_ANONYMOUS;
insert_vma(mm, v);
/* later allocate a page, copy hand-assembled svc blob into it, map at 0x400000 */
```
Day 16 uses this.

---

## 6. Stretch

- COW fork: clone PTEs read-only, on write fault clone the page. ~150 LOC.
- `vfork()` semantics (parent blocks until exec/exit).
- `clone_thread` flag bundle to share signal handlers too (for future).

---

## 7. References

- Linux `kernel/fork.c` `copy_process`.
- Linux `arch/arm64/mm/context.c` (ASID rollover).
- "What Every OS Should Know About TTBR" — Will Deacon, LWN.
