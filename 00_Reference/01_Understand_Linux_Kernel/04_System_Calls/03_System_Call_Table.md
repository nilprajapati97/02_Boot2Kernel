# 03 — System Call Table

## 1. Definition

The **system call table** (`sys_call_table[]`) is an array of function pointers. Each entry maps a syscall number to its kernel handler function. It is the dispatch table that `do_syscall_64()` uses.

---

## 2. sys_call_table Structure

```c
/* arch/x86/entry/syscall_64.c (auto-generated) */
asmlinkage const sys_call_ptr_t sys_call_table[] = {
    [0]  = __x64_sys_read,
    [1]  = __x64_sys_write,
    [2]  = __x64_sys_open,
    [3]  = __x64_sys_close,
    [4]  = __x64_sys_stat,
    [5]  = __x64_sys_fstat,
    [9]  = __x64_sys_mmap,
    [39] = __x64_sys_getpid,
    [56] = __x64_sys_clone,
    [57] = __x64_sys_fork,
    [59] = __x64_sys_execve,
    [60] = __x64_sys_exit,
    /* etc... */
    [NR_syscalls] = NULL,
};
```

---

## 3. How the Table is Generated

```mermaid
flowchart TD
    Tbl[arch/x86/entry/syscalls/syscall_64.tbl\nHuman-readable table] --> Script[scripts/syscalltbl.sh]
    Script --> H[arch/x86/include/generated/asm/syscalls_64.h]
    H --> Included[arch/x86/entry/syscall_64.c\nincludes the header]
    Included --> Table[sys_call_table\[\] array]
```

### syscall_64.tbl format:
```
# <number> <abi> <name> <entry point>
0   common  read                    sys_read
1   common  write                   sys_write
2   common  open                    sys_open
3   common  close                   sys_close
57  common  fork                    sys_fork
59  common  execve                  sys_execve
60  common  exit                    sys_exit
```

---

## 4. SYSCALL_DEFINE Macros

Kernel syscall handlers are defined with `SYSCALL_DEFINE` macros:

```c
/* include/linux/syscalls.h */
/* SYSCALL_DEFINEn where n = number of arguments */

SYSCALL_DEFINE0(getpid)
{
    return task_tgid_vnr(current);
}

SYSCALL_DEFINE3(write, unsigned int, fd, const char __user *, buf, size_t, count)
{
    return ksys_write(fd, buf, count);
}

SYSCALL_DEFINE4(openat, int, dfd, const char __user *, filename,
                int, flags, umode_t, mode)
{
    return do_sys_openat2(dfd, filename, &how);
}
```

### What SYSCALL_DEFINE expands to:
```c
/* SYSCALL_DEFINE3(write, unsigned int, fd, ...) expands to: */
asmlinkage long __x64_sys_write(const struct pt_regs *regs);
static long __se_sys_write(unsigned int fd, const char __user *buf, size_t count);
static inline long __do_sys_write(unsigned int fd, const char __user *buf, size_t count);

asmlinkage long __x64_sys_write(const struct pt_regs *regs) {
    return __se_sys_write(regs->di, regs->si, regs->dx);
}
/* The __do_sys_write is the actual implementation */
```

---

## 5. Important Syscalls and Their Kernel Functions

| Syscall | Number (x64) | Kernel Function | File |
|---------|-------------|----------------|------|
| `read` | 0 | `sys_read` → `ksys_read` | `fs/read_write.c` |
| `write` | 1 | `sys_write` → `ksys_write` | `fs/read_write.c` |
| `open` / `openat` | 2 / 257 | `do_sys_openat2` | `fs/open.c` |
| `close` | 3 | `__close_fd` | `fs/open.c` |
| `fork` | 57 | `kernel_clone` | `kernel/fork.c` |
| `execve` | 59 | `do_execveat_common` | `fs/exec.c` |
| `exit` | 60 | `do_exit` | `kernel/exit.c` |
| `mmap` | 9 | `ksys_mmap_pgoff` | `mm/mmap.c` |
| `brk` | 12 | `sys_brk` | `mm/mmap.c` |
| `getpid` | 39 | `task_tgid_vnr` | `kernel/sys.c` |
| `kill` | 62 | `sys_kill` → `__send_signal` | `kernel/signal.c` |
| `socket` | 41 | `__sys_socket` | `net/socket.c` |
| `clock_gettime` | 228 | `do_clock_gettime` | `kernel/time/posix-timers.c` |

---

## 6. Unimplemented Syscalls

```c
/* Not-implemented syscall handler */
asmlinkage long sys_ni_syscall(void)
{
    return -ENOSYS;    /* Function not implemented */
}
```

When a syscall slot is reserved but not implemented, it points to `sys_ni_syscall`.

---

## 7. 32-bit compat on 64-bit Kernel

```c
/* 32-bit processes on 64-bit kernel use int 0x80 or SYSCALL */
/* Handled by compat_sys_call_table[] */
/* arch/x86/entry/syscalls/syscall_32.tbl */
/* Different table! 32-bit and 64-bit have different numbers */

/* Example: compat_sys_write handles 32-bit write */
```

---

## 8. Viewing Syscall Table at Runtime

```bash
# Read syscall table from running kernel via /proc/kallsyms
sudo cat /proc/kallsyms | grep "sys_call_table"

# Use ausyscall to list all syscalls
ausyscall --dump | head -20

# Use strace to trace a program
strace -e trace=write echo hello
```

---

## 9. Related Concepts
- [02_System_Call_Handler.md](./02_System_Call_Handler.md) — How table is invoked
- [04_Adding_A_New_System_Call.md](./04_Adding_A_New_System_Call.md) — Adding an entry to the table
