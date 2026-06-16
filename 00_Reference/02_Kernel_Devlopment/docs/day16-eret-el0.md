# Day 16 — Drop to EL0 (First Userspace Instruction) — Phase 3 Gate

> **Goal**: Hand-craft a tiny user "process" — text/stack VMAs, a hand-assembled blob that does `svc #0` (exit), then `eret` to EL0 to execute it. Bonus: handle the resulting syscall to print "hello".
>
> **Why today**: This is the *user/kernel boundary* coming alive. Day 17 builds the full syscall table on top of the EL0 entry path proven today.

---

## 1. Background

### 1.1 EL1 → EL0 transition
The exact same mechanism as EL2 → EL1 (Day 2):
1. `SPSR_EL1.M[3:0] = 0` (EL0t) — bit 0 = SP_EL0 selector (EL0 always uses SP_EL0).
2. `SPSR_EL1.DAIF` = 0 (unmask IRQs at EL0).
3. `ELR_EL1` = user PC (e.g., `0x40_0000`).
4. `SP_EL0` = user stack top.
5. `TTBR0_EL1` = user `pgd | ASID<<48`.
6. `eret`.

### 1.2 What user PC actually contains (Day 16 blob)
We hand-assemble a `mov x8, #93; svc #0` sequence (syscall 93 = `exit` in Linux AArch64). Encoding:
```
mov x8, #93   :  d2800ba8
svc #0        :  d4000001
```
(Confirm with `aarch64-linux-gnu-as` and `objdump -d`.)

Place this 8-byte blob at PA `0x4100_0000` (a free page), map at VA `0x40_0000` in user.

### 1.3 Returning from EL0
On `svc`, the existing exception vector at offset `0x400` (lower EL using AArch64, sync) executes `kernel_entry 0` (Day 4), then calls `do_sync` → which sees `EC=0x15` → `do_syscall(r)`.

For Day 16 we implement just enough `do_syscall` to recognize `x8 == 93` (exit) and `x8 == 64` (write to fd=1):
```c
void do_syscall(struct pt_regs *r) {
    long ret = 0;
    switch (r->regs[8]) {
    case 64: /* write(fd, buf, len) */
        if (r->regs[0] == 1) {
            char k[256]; long n = r->regs[2];
            if (n > 255) n = 255;
            copy_from_user(k, (void __user *)r->regs[1], n);
            k[n] = 0;
            uart_puts(k);
            ret = n;
        }
        break;
    case 93: /* exit */
        do_exit((int)r->regs[0]);
        break;
    default:
        ret = -38;       /* ENOSYS */
    }
    r->regs[0] = ret;
}
```

---

## 2. Design

### 2.1 Hand-assembled "hello" user blob
```asm
    .text
    .global _user_entry
_user_entry:
    /* write(1, msg, 6) */
    mov     x0, #1
    adr     x1, msg
    mov     x2, #6
    mov     x8, #64
    svc     #0
    /* exit(0) */
    mov     x0, #0
    mov     x8, #93
    svc     #0
msg:
    .ascii  "hello\n"
```
Compile separately to a flat binary, embed via `incbin` into the kernel image at link time:
```ld
.user_blob : ALIGN(4096) {
    __user_blob_start = .;
    KEEP(*(.user_blob))
    __user_blob_end = .;
}
```
And:
```c
asm(".pushsection .user_blob, \"a\"\n"
    ".incbin \"build/user_blob.bin\"\n"
    ".popsection\n");
```

### 2.2 Files
```
user/blob.S                       (hand-crafted blob source)
kernel/syscall.c                  (just the 2 syscalls today)
arch/arm64/kernel/userspace.c     (build mm, drop to EL0)
```

---

## 3. Implementation

### 3.1 Build the blob (Makefile addition)
```make
USER_AS := $(CROSS)as
USER_LD := $(CROSS)ld
USER_OBJCOPY := $(CROSS)objcopy

build/user_blob.bin: user/blob.S
	@mkdir -p $(@D)
	$(USER_AS) -o build/user_blob.o $<
	$(USER_LD) -Ttext=0x400000 -o build/user_blob.elf build/user_blob.o
	$(USER_OBJCOPY) -O binary build/user_blob.elf $@
```

### 3.2 Build the first user task
```c
#include <kernel/task.h>
#include <kernel/mm.h>
#include <kernel/sched.h>
extern char __user_blob_start[], __user_blob_end[];

void launch_first_user(void)
{
    struct task *t = task_create(NULL, NULL, "init");
    /* override entry: we don't go through ret_from_kthread; we go through ret_to_user. */

    struct mm_struct *mm = mm_alloc();
    t->mm = mm;

    /* Map text @ 0x400000 with the blob */
    u64 blob_sz = __user_blob_end - __user_blob_start;
    u64 npages = (blob_sz + PAGE_SIZE - 1) >> PAGE_SHIFT;
    for (u64 i = 0; i < npages; i++) {
        phys_addr_t pa = alloc_pages(0);
        memcpy((void *)(pa + 0xffff000000000000UL),
               __user_blob_start + i * PAGE_SIZE,
               (i == npages - 1) ? (blob_sz % PAGE_SIZE ?: PAGE_SIZE) : PAGE_SIZE);
        map_4k(mm->pgd, 0x400000 + i * PAGE_SIZE, pa,
               PTE_PAGE | PTE_VALID | PTE_AF | PTE_SH_INNER |
               PTE_ATTR(0) | PTE_AP_RO_ANY | PTE_NG /* exec OK: no UXN */);
    }
    struct vma *vt = kmalloc(sizeof *vt, 0);
    *vt = (struct vma){.start=0x400000, .end=0x400000 + npages*PAGE_SIZE, .prot=PROT_READ|PROT_EXEC};
    insert_vma(mm, vt);

    /* Map a one-page user stack at 0x7FFF_F000 */
    u64 ustack_top = 0x80000000UL;
    phys_addr_t spa = alloc_pages(0);
    map_4k(mm->pgd, ustack_top - PAGE_SIZE, spa,
           PTE_PAGE|PTE_VALID|PTE_AF|PTE_SH_INNER|PTE_ATTR(0)|
           PTE_AP_RW_ANY|PTE_NG|PTE_UXN);
    struct vma *vs = kmalloc(sizeof *vs, 0);
    *vs = (struct vma){.start=ustack_top - PAGE_SIZE, .end=ustack_top,
                       .prot=PROT_READ|PROT_WRITE};
    insert_vma(mm, vs);

    /* Build initial pt_regs on the kernel stack so kernel_exit el=0 returns to EL0 */
    struct pt_regs *r = (struct pt_regs *)((u8*)t->kstack + KSTACK_SIZE) - 1;
    memset(r, 0, sizeof *r);
    r->pc     = 0x400000;
    r->sp     = ustack_top;
    r->pstate = 0;                       /* EL0t, DAIF=0 */
    /* Point task's context so that on first switch, sp = r and pc = ret_to_user */
    t->ctx.sp = (u64)r;
    extern void ret_to_user(void);
    t->ctx.pc = (u64)ret_to_user;

    sched_add(t);
}
```

### 3.3 `ret_to_user` (in `entry.S`)
```asm
    .global ret_to_user
ret_to_user:
    kernel_exit 0
```
Because `sp` points to a fully-populated `pt_regs`, `kernel_exit 0` restores it and `eret` lands at EL0 PC = `0x400000`.

### 3.4 Wire it up
```c
void kmain_post_mmu(void) {
    /* ... mm/sched/timer init ... */
    launch_first_user();
    asm volatile("msr daifclr, #2");
    for (;;) asm volatile("wfi");
}
```

---

## 4. ARM64 Cheat-Sheet (Day 16)

```
SPSR_EL1.M[3:0]   EL0t = 0000  ; EL0 must be 'unprivileged thread'
ELR_EL1           user PC to resume
SP_EL0            user stack pointer  (selected by SPSR.M[0]=0)
TTBR0_EL1[63:48]  ASID
svc #imm          synchronous call -> EL1 sync vector (offset 0x400)
ESR_EL1.EC == 0x15  for SVC from AArch64
```

---

## 5. Pitfalls

1. **PSTATE.M wrong**: `EL0t` is `0b0000`; if you set `EL0t` but bit 0 = 1 ("h") you'd be selecting SP_EL0 in a privileged mode — UNPREDICTABLE.
2. **User text mapped W+X**: blob is R-X only (`PTE_AP_RO_ANY` + no UXN). Stack is RW+NX (UXN set).
3. **Kernel SP corruption on user fault**: each task uses **its own kernel stack** (we set it up in `task_create`). When user faults, `kernel_entry 0` switches to the kernel SP, then to the saved `pt_regs` area.
4. **Forgetting `dsb; isb` after `ttbr0` load**: subsequent user code fetched with stale ASID — random fetch faults.
5. **`copy_from_user` for `write` syscall**: must use fixup path (Day 11) in case user passes a bad pointer. Otherwise kernel oops.

---

## 6. Verification (Phase 3 Gate)

```
make run
# Serial:
hello
[INFO] task pid=2 (init) exited code=0
A B A B ...
```

GDB:
```
(gdb) b do_syscall
(gdb) c
(gdb) p/x ((struct pt_regs*)$x0)->regs[8]   # 64 (write)
(gdb) c
(gdb) p/x ((struct pt_regs*)$x0)->regs[8]   # 93 (exit)
```

---

## 7. Stretch

- Spawn 3 user tasks running the same blob to demonstrate ASID switching.
- Add `getpid` (syscall 172) so the blob can print its PID via write.
- Print full PSTATE+regs on user crash for debugging.

---

## 8. References

- ARM ARM §D1.7 (`PSTATE` and exception return).
- Linux UAPI `arch/arm64/include/uapi/asm/unistd.h` — syscall numbers.
- xv6 `proc.c` `forkret`, `userinit`.
