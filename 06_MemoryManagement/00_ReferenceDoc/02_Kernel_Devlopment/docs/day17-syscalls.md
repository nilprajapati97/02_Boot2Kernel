# Day 17 — Syscall Infrastructure

> **Goal**: Build a syscall dispatcher driven by a static table indexed by `x8` (Linux AArch64 convention). Implement: `read`, `write`, `exit`, `getpid`, `brk`, `mmap` (anon), `munmap`, `nanosleep`, `gettimeofday`, `uname`.
>
> **Why today**: Day 18's ELF loader runs user code that immediately calls `write`, `brk`, `mmap`. Day 26's `ksh` shell relies on `read`, `write`, `wait4`, `fork`, `execve`.

---

## 1. Background

### 1.1 Linux AArch64 syscall ABI
| Reg | Use |
|---|---|
| `x8` | syscall number |
| `x0..x5` | args 0..5 |
| `x0` (after `svc`) | return value (negative = `-errno`) |

Selected syscall numbers (matches `<asm-generic/unistd.h>`):
| # | Name |
|---|---|
| 17 | `getcwd` |
| 35 | `unlinkat` |
| 56 | `openat` |
| 57 | `close` |
| 63 | `read` |
| 64 | `write` |
| 78 | `readlinkat` |
| 80 | `fstat` |
| 93 | `exit` |
| 96 | `set_tid_address` |
| 101 | `nanosleep` |
| 113 | `clock_gettime` |
| 124 | `sched_yield` |
| 160 | `uname` |
| 172 | `getpid` |
| 173 | `getppid` |
| 214 | `brk` |
| 215 | `munmap` |
| 220 | `clone` |
| 221 | `execve` |
| 222 | `mmap` |
| 226 | `mprotect` |
| 260 | `wait4` |

### 1.2 Dispatch through a table
```c
typedef long (*syscall_fn)(struct pt_regs *);
static const syscall_fn syscall_table[NR_SYSCALLS] = {
    [63]  = sys_read,
    [64]  = sys_write,
    [93]  = sys_exit,
    [101] = sys_nanosleep,
    [113] = sys_clock_gettime,
    [160] = sys_uname,
    [172] = sys_getpid,
    [214] = sys_brk,
    [222] = sys_mmap,
    [215] = sys_munmap,
    [124] = sys_sched_yield,
    ...
};
```

---

## 2. Implementation

### 2.1 Dispatcher
```c
void do_syscall(struct pt_regs *r)
{
    unsigned nr = r->regs[8];
    long ret;
    if (nr < NR_SYSCALLS && syscall_table[nr])
        ret = syscall_table[nr](r);
    else {
        printk(KERN_WARN "ENOSYS x8=%u pid=%d\n", nr, current()->pid);
        ret = -38;     /* ENOSYS */
    }
    r->regs[0] = (u64)ret;
}
```

### 2.2 Sample syscalls
```c
long sys_getpid(struct pt_regs *r)   { (void)r; return current()->pid; }
long sys_sched_yield(struct pt_regs *r) { (void)r; schedule(); return 0; }

long sys_write(struct pt_regs *r)
{
    int fd = r->regs[0];
    const char __user *buf = (const char __user *)r->regs[1];
    size_t n = r->regs[2];
    if (fd != 1 && fd != 2) return -9;     /* EBADF (real fd Day 20) */
    char k[256]; if (n > 255) n = 255;
    if (copy_from_user(k, buf, n)) return -14;
    k[n] = 0; uart_puts(k); return n;
}

long sys_read(struct pt_regs *r)
{
    int fd = r->regs[0];
    char __user *buf = (char __user *)r->regs[1];
    size_t n = r->regs[2];
    if (fd != 0) return -9;
    /* simple poll-based UART read */
    extern int uart_getc(void);
    size_t i = 0;
    while (i < n) {
        int c = uart_getc();
        if (c < 0) break;
        if (copy_to_user(buf + i, &c, 1)) return -14;
        i++;
        if (c == '\n') break;
    }
    return i;
}

long sys_exit(struct pt_regs *r) { do_exit((int)r->regs[0]); for(;;); }

long sys_brk(struct pt_regs *r)
{
    struct mm_struct *mm = current()->mm;
    u64 new_brk = r->regs[0];
    if (new_brk < mm->start_brk) return mm->brk;
    /* grow: allocate pages 4 KiB at a time */
    u64 old = (mm->brk + PAGE_SIZE - 1) & PAGE_MASK;
    u64 end = (new_brk + PAGE_SIZE - 1) & PAGE_MASK;
    for (u64 va = old; va < end; va += PAGE_SIZE) {
        phys_addr_t pa = alloc_pages(0);
        if (!pa) return mm->brk;
        map_4k(mm->pgd, va, pa,
               PTE_PAGE|PTE_VALID|PTE_AF|PTE_SH_INNER|
               PTE_ATTR(0)|PTE_AP_RW_ANY|PTE_NG|PTE_UXN);
    }
    /* shrink: unmap+free  (omitted for brevity) */
    mm->brk = new_brk;
    return new_brk;
}

long sys_mmap(struct pt_regs *r)
{
    u64 addr   = r->regs[0];
    u64 length = r->regs[1];
    u32 prot   = r->regs[2];
    u32 flags  = r->regs[3];
    int fd     = r->regs[4];
    u64 off    = r->regs[5];
    (void)fd; (void)off;            /* file-backed Day 20+ */

    if (!(flags & MAP_ANONYMOUS)) return -22;
    length = (length + PAGE_SIZE - 1) & PAGE_MASK;

    /* simple: hand-pick a region in the user free space */
    static u64 mmap_next = 0x600000000UL;
    if (!addr) addr = mmap_next;
    mmap_next += length;

    for (u64 va = addr; va < addr + length; va += PAGE_SIZE) {
        phys_addr_t pa = alloc_pages(0);
        if (!pa) return -12;
        u64 attrs = PTE_PAGE|PTE_VALID|PTE_AF|PTE_SH_INNER|PTE_ATTR(0)|PTE_NG;
        attrs |= (prot & PROT_WRITE) ? PTE_AP_RW_ANY : PTE_AP_RO_ANY;
        if (!(prot & PROT_EXEC)) attrs |= PTE_UXN;
        map_4k(current()->mm->pgd, va, pa, attrs);
    }

    struct vma *v = kmalloc(sizeof *v, 0);
    *v = (struct vma){.start=addr, .end=addr+length, .prot=prot, .flags=flags};
    insert_vma(current()->mm, v);

    return addr;
}

long sys_nanosleep(struct pt_regs *r)
{
    struct timespec { long sec; long nsec; } __user *req = (void *)r->regs[0];
    struct timespec ts;
    if (copy_from_user(&ts, req, sizeof ts)) return -14;
    u64 deadline = get_jiffies() + ts.sec * HZ + ts.nsec / (1000000000UL / HZ);
    while (get_jiffies() < deadline) { schedule(); }
    return 0;
}

long sys_uname(struct pt_regs *r)
{
    struct utsname { char sysname[65], nodename[65], release[65], version[65], machine[65], domainname[65]; };
    struct utsname k = {
        .sysname  = "nkernel",
        .nodename = "qemu",
        .release  = "0.1",
        .version  = "#1 SMP " __DATE__,
        .machine  = "aarch64",
    };
    return copy_to_user((void __user *)r->regs[0], &k, sizeof k);
}
```

### 2.3 `asm-offsets.h` generator
To keep `entry.S` `S_FRAME_SIZE` consistent:
```c
/* arch/arm64/kernel/asm-offsets.c */
#include <asm-arm64/pt_regs.h>
#include <stddef.h>
#define DEFINE(sym, val) asm volatile("#define " #sym " %0" :: "i"(val))
int main(void) {
    DEFINE(S_FRAME_SIZE, sizeof(struct pt_regs));
    DEFINE(S_REGS,       offsetof(struct pt_regs, regs));
    DEFINE(S_SP,         offsetof(struct pt_regs, sp));
    DEFINE(S_PC,         offsetof(struct pt_regs, pc));
    DEFINE(S_PSTATE,     offsetof(struct pt_regs, pstate));
    return 0;
}
```
A makefile rule preprocesses this and pipes into `asm-offsets.h`.

---

## 3. Pitfalls

1. **User pointer not validated**: always `copy_from_user`/`copy_to_user`. Direct `*ptr` from kernel for a user VA = OOPS if unmapped.
2. **Negative-return ABI confusion**: returning `-EFAULT` (which is `0xFFFFFFFFFFFFFFF2` in `u64`) appears as a valid pointer to the user. Use signed return in syscall and cast at the end.
3. **`brk` shrink leaks**: implement page-free on shrink, or document as TODO.
4. **`mmap` placement clashes**: bump pointer may overlap with future `mmap`; track allocated extents.
5. **Re-entrancy from sleep**: `sys_nanosleep` loops calling `schedule`. Use proper timer wait queue Day 24.

---

## 4. Verification

Compile a tiny user program (`hello.c`):
```c
int main() {
    const char *s = "hello from nkernel\n";
    long n = 0; while (s[n]) n++;
    asm volatile("mov x0, #1; mov x1, %0; mov x2, %1; mov x8, #64; svc #0" :: "r"(s), "r"(n) : "x0","x1","x2","x8");
    asm volatile("mov x0, #0; mov x8, #93; svc #0" ::: "x0","x8");
    return 0;
}
```
Build static: `aarch64-linux-gnu-gcc -static -nostartfiles -nostdlib -e main hello.c -o hello.elf`.

Plug into Day 16's loader (replace blob): expect serial line `hello from nkernel`.

---

## 5. Stretch

- Generate `syscall_table` from a `.tbl` file via `awk` (Linux-style).
- Add per-syscall trace (`strace -k`) by printing name + args.
- `vDSO`-style page for fast `clock_gettime` (skip `svc`).

---

## 6. References

- Linux `include/uapi/asm-generic/unistd.h` — canonical syscall numbers.
- Linux `arch/arm64/kernel/syscall.c`.
