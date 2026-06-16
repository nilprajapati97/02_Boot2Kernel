# System Call Interface in the Linux Kernel — Deep Dive

A **system call (syscall)** is the controlled doorway through which a user-space program requests a service from the kernel (read a file, allocate memory, create a process, send a packet, etc.). User code cannot touch hardware, page tables, or kernel data structures directly — the CPU enforces this through **privilege levels** (rings on x86, ELx on ARM). The syscall interface is the only legitimate way to cross that boundary.

Below is the complete journey, from a C function call in your program all the way down to kernel code and back.

---

## 1. The Big Picture (Layers)

```
┌─────────────────────────────────────────────┐
│ User Application (e.g., your C program)     │  ring 3 / EL0
│   printf("hi"); → write(1, "hi", 2);        │
├─────────────────────────────────────────────┤
│ glibc / libc wrapper (write())              │  ring 3
│   - puts syscall number in a register       │
│   - executes SYSCALL/SVC instruction        │
├─────────────────────────────────────────────┤
│ CPU mode switch (hardware)                  │  ← privilege transition
├─────────────────────────────────────────────┤
│ Kernel entry stub (arch/x86/entry/...)      │  ring 0 / EL1
│   - saves registers, switches stack         │
├─────────────────────────────────────────────┤
│ syscall dispatcher (sys_call_table[nr])     │
├─────────────────────────────────────────────┤
│ SYSCALL_DEFINEx() handler (e.g., sys_write) │
├─────────────────────────────────────────────┤
│ VFS → filesystem → block/device driver      │
├─────────────────────────────────────────────┤
│ Hardware                                    │
└─────────────────────────────────────────────┘
```

---

## 2. Why Syscalls Exist (Privilege Separation)

CPUs have at least two modes:

| Mode | x86 name | ARM name | Can do |
|------|----------|----------|--------|
| User | Ring 3 | EL0 | Run normal instructions, access own virtual memory |
| Kernel | Ring 0 | EL1 | Everything: I/O ports, page tables, MSRs, halt CPU |

If user code tries a privileged instruction (e.g., `HLT`, `IN`, `OUT`, writing CR3), the CPU raises a **General Protection Fault**. The only sanctioned way to "ask" the kernel to do something privileged is a **syscall instruction**, which atomically:

1. Switches CPU to kernel mode.
2. Jumps to a fixed, kernel-controlled entry address (not arbitrary user-chosen code).

This second property is critical — the user cannot pick *where* in the kernel to land.

---

## 3. The User-Space Side

### 3.1 The wrapper

When you write:

```c
ssize_t n = write(1, "hi\n", 3);
```

`write` is a thin **glibc wrapper**. It does roughly:

```asm
; x86_64 System V syscall ABI
mov  rax, 1          ; syscall number for write
mov  rdi, 1          ; arg1: fd
mov  rsi, msg        ; arg2: buffer
mov  rdx, 3          ; arg3: count
syscall              ; <-- the magic instruction
; on return: rax = result (or -errno)
```

### 3.2 The syscall ABI (x86_64)

| Register | Purpose |
|----------|---------|
| `rax` | Syscall number (input), return value (output) |
| `rdi, rsi, rdx, r10, r8, r9` | Args 1–6 (note: `r10`, not `rcx`) |
| `rcx` | Clobbered — CPU stores return RIP here |
| `r11` | Clobbered — CPU stores RFLAGS here |

Other architectures:

| Arch | Instruction | Number reg | Arg regs |
|------|-------------|------------|----------|
| x86_64 | `syscall` | rax | rdi, rsi, rdx, r10, r8, r9 |
| i386 (legacy) | `int 0x80` | eax | ebx, ecx, edx, esi, edi, ebp |
| aarch64 | `svc #0` | x8 | x0–x5 |
| arm32 | `swi 0` | r7 | r0–r5 |
| riscv | `ecall` | a7 | a0–a5 |

### 3.3 Syscall numbers

Each syscall has a stable number per-architecture, defined in tables like `arch/x86/entry/syscalls/syscall_64.tbl`:

```
0   common  read    sys_read
1   common  write   sys_write
2   common  open    sys_open
...
```

Numbers are **append-only** to preserve the ABI forever.

---

## 4. The Hardware Transition

When the CPU executes `syscall` (x86_64):

1. Reads `MSR_LSTAR` → gets kernel entry RIP (set at boot to `entry_SYSCALL_64`).
2. Reads `MSR_STAR` → loads kernel CS/SS selectors.
3. Saves user `RIP → RCX`, `RFLAGS → R11`.
4. Masks flags via `MSR_SYSCALL_MASK` (disables interrupts, clears direction flag, etc.).
5. CPL changes 3 → 0.
6. Jumps to the kernel entry point.

Note: **the stack pointer is NOT switched by hardware** on x86_64 `syscall`. The kernel must do it manually (next step).

---

## 5. The Kernel Entry Stub

File: `arch/x86/entry/entry_64.S` → `entry_SYSCALL_64`.

Simplified flow:

```
entry_SYSCALL_64:
    swapgs                       ; switch GS base to per-CPU kernel data
    mov  %rsp, PER_CPU(user_rsp) ; save user stack
    mov  PER_CPU(kernel_stack), %rsp  ; load kernel stack for this task
    ; build a "pt_regs" frame on the kernel stack (saves all GPRs)
    push ... (rax, rdi, rsi, rdx, r10, r8, r9, rcx, r11, etc.)
    call do_syscall_64           ; C function
    ; on return:
    ; restore regs, swapgs, sysretq → back to user mode
```

Why a kernel stack per task? Because each thread needs its own stack for kernel execution; you can't trust or share the user stack.

`swapgs` is an x86 trick: the GS segment base points to user TLS in user mode and to the per-CPU kernel data area in kernel mode; `swapgs` toggles between two MSRs.

There are also hardening shims around it:

- **KPTI (Kernel Page Table Isolation)** — Meltdown mitigation. User mode runs on a shadow page table that does NOT map the kernel. On entry, the kernel switches CR3 to the full page table.
- **Retpolines / IBRS / STIBP** — Spectre mitigations applied during entry/exit.

---

## 6. The Dispatcher

`do_syscall_64()` (in `arch/x86/entry/common.c`) does:

```c
__visible noinstr void do_syscall_64(struct pt_regs *regs, int nr)
{
    add_random_kstack_offset();      // stack ASLR per syscall
    nr = syscall_enter_from_user_mode(regs, nr);  // audit, seccomp, ptrace, etc.

    if (likely(nr < NR_syscalls)) {
        nr = array_index_nospec(nr, NR_syscalls); // Spectre-v1 guard
        regs->ax = sys_call_table[nr](regs);      // <-- the actual call
    } else {
        regs->ax = __x64_sys_ni_syscall(regs);    // -ENOSYS
    }

    syscall_exit_to_user_mode(regs);  // signals, resched, tracing
}
```

Key pieces:

- **`sys_call_table`** — an array of function pointers indexed by syscall number. Built at compile time from the `.tbl` file via `syscalltbl.sh`.
- **`array_index_nospec`** — branchless bounds clamp that defeats Spectre-v1 speculation past the size check.
- **`syscall_enter_from_user_mode`** — the audit/security choke point. Here the kernel runs:
  - **seccomp** BPF filter (can kill or `-EPERM` the syscall).
  - **ptrace** stop (debugger may inspect/modify args).
  - **audit subsystem** record.
  - **context tracking** for nohz/RCU.

---

## 7. The Syscall Handler — `SYSCALL_DEFINEx`

Real handlers are written with macros so the kernel can generate per-arch glue (wrapping the `pt_regs` into typed args, applying sign-extension, etc.).

Example: `fs/read_write.c`:

```c
SYSCALL_DEFINE3(write, unsigned int, fd,
                const char __user *, buf, size_t, count)
{
    return ksys_write(fd, buf, count);
}
```

The `SYSCALL_DEFINE3` macro expands (roughly) to:

```c
asmlinkage long __x64_sys_write(const struct pt_regs *regs)
{
    return __se_sys_write(regs->di, regs->si, regs->dx);
}

static long __se_sys_write(long fd, long buf, long count)
{
    return __do_sys_write((unsigned int)fd,
                          (const char __user *)buf,
                          (size_t)count);
}

static inline long __do_sys_write(unsigned int fd,
                                  const char __user *buf,
                                  size_t count)
{
    return ksys_write(fd, buf, count);  // your body
}
```

Why so many layers?

- `__x64_sys_*` takes only `pt_regs *` — uniform signature for the table.
- `__se_*` does **sign extension** (32-bit args zero/sign-extended into 64-bit slots) — important for compat and security.
- `__do_sys_*` is the real typed body.
- The `__user` annotation is checked by **sparse** to ensure pointer provenance is respected.

---

## 8. Touching User Memory Safely

Inside `sys_write`, the kernel must read `buf` from user space. It **cannot** just `memcpy` it, because:

- The address might be invalid → would oops the kernel.
- A malicious caller could pass a pointer into kernel memory → privilege escalation.
- The page might be swapped/unmapped → needs a page-fault-safe path.

So the kernel uses:

```c
copy_from_user(kbuf, ubuf, len);   // user → kernel
copy_to_user(ubuf, kbuf, len);     // kernel → user
get_user(x, ptr);                  // single value
put_user(x, ptr);                  // single value
```

These primitives:

1. Verify the address is in the user range (`access_ok`).
2. On x86, enforce **SMAP/SMEP** via `stac`/`clac` (Supervisor Mode Access Prevention).
3. Install an **exception table entry** (`_ASM_EXTABLE`) so a fault during the copy returns `-EFAULT` instead of crashing the kernel.

This is the heart of **defensive boundary handling** — never trust user pointers.

---

## 9. Doing the Real Work

For `write(fd, buf, count)`, the path is:

```
ksys_write
  └─ fdget_pos(fd)                  // look up struct file from fd table
       └─ vfs_write(file, buf, count, &pos)
            └─ file->f_op->write_iter(...)   // filesystem-specific
                 └─ e.g., ext4_file_write_iter
                      └─ generic_perform_write
                           └─ page cache → bio → block layer → driver → disk
```

The syscall layer itself is thin; most logic lives in subsystems (VFS, MM, net, sched). The syscall is just the entry shim.

---

## 10. Returning to User Space

After the handler returns, `syscall_exit_to_user_mode()` performs:

1. **Signal delivery** — if a signal is pending, build a signal frame on the user stack and arrange to enter the handler.
2. **Reschedule** — if `TIF_NEED_RESCHED` is set, call `schedule()`. Your task may sleep here and another may run.
3. **ptrace exit stop**, audit record finalize.
4. **Address-limit / FPU / context-tracking checks** in debug builds.

Then the kernel exits via `sysretq` (or `iretq` if state is "complicated"):

- Restores `RIP` from `RCX`, `RFLAGS` from `R11`, switches CR3 back to user (KPTI), `swapgs`, drops to ring 3.
- User code resumes immediately after the `syscall` instruction with the result in `rax`.

glibc then converts negative results in the `-1..-4095` range into `-1` + `errno`:

```c
if ((unsigned long)ret > -4096UL) {
    errno = -ret;
    ret = -1;
}
```

That is why kernel handlers return `-EFAULT`, `-EINVAL`, etc. — they are negative `errno` values.

---

## 11. The Fast Path: vDSO

Some syscalls (`gettimeofday`, `clock_gettime`, `getcpu`, `time`) are so frequent that crossing into the kernel would be wasteful. Linux maps a small shared object — the **vDSO** (`linux-vdso.so.1`) — into every process. It contains read-only code and a kernel-updated data page (clock counters, etc.), so these "syscalls" execute entirely in user mode. glibc transparently calls into the vDSO when available, falling back to a real syscall otherwise.

---

## 12. Compatibility Layers

- **32-bit on 64-bit kernel (`CONFIG_COMPAT`)** — a separate `ia32_sys_call_table` and `compat_SYSCALL_DEFINE*` handlers translate 32-bit structures (timevals, iovecs) into native ones.
- **x32 ABI** — 64-bit registers but 32-bit pointers; syscall numbers ORed with `0x40000000`.
- **`int 0x80`** — legacy fallback path; slower because it uses the IDT.

---

## 13. Security and Observability Hooks

Every syscall passes through these gates, configurable per-task:

| Mechanism | Purpose |
|-----------|---------|
| **seccomp-bpf** | BPF filter decides allow / errno / kill / trace. Used by sandboxes (Chrome, Docker, systemd). |
| **LSM (SELinux, AppArmor, Tomoyo)** | Hooks deep in subsystem code (e.g., `security_file_permission`). |
| **audit** | Records syscall args and result to userspace (`auditd`). |
| **ptrace `PTRACE_SYSCALL`** | Stops the task on entry/exit so a debugger (strace, gdb) can inspect. |
| **tracepoints** `sys_enter` / `sys_exit` | Used by `perf`, `bpftrace`, `ftrace`. |

This is why `strace ./prog` works: it attaches via `ptrace` and the syscall entry path calls into the tracer.

---

## 14. Adding a New Syscall (How It All Connects)

To add `sys_mything`:

1. Implement the body in some `.c` file:
   ```c
   SYSCALL_DEFINE2(mything, int, a, int, b) {
       return a + b;
   }
   ```
2. Reserve a number in every arch table, e.g.:
   ```
   arch/x86/entry/syscalls/syscall_64.tbl:
   462   common  mything   sys_mything
   ```
3. Declare in `include/linux/syscalls.h`:
   ```c
   asmlinkage long sys_mything(int a, int b);
   ```
4. Rebuild kernel. The build system regenerates `sys_call_table` so index 462 points to `__x64_sys_mything`.
5. Userspace invokes it via `syscall(462, 1, 2)` until glibc adds a wrapper.

---

## 15. End-to-End Trace of One Call

`write(1, "hi", 2)` on x86_64 Linux:

1. **glibc** loads `rax=1, rdi=1, rsi="hi", rdx=2`, executes `syscall`.
2. **CPU** swaps to ring 0, jumps to `entry_SYSCALL_64`.
3. **Entry stub** does `swapgs`, switches to kernel stack, saves regs into `pt_regs`, calls `do_syscall_64`.
4. **Dispatcher** runs seccomp / audit / ptrace-enter hooks, indexes `sys_call_table[1]` → `__x64_sys_write`.
5. **Macro glue** unpacks `pt_regs` → calls `ksys_write(1, "hi", 2)`.
6. **VFS** resolves fd 1 → `struct file *` (the terminal). Calls `vfs_write` → `tty_write` → line discipline → driver → device.
7. Returns bytes-written (or `-errno`) back up the stack into `regs->ax`.
8. **Exit path** checks signals, need_resched, etc.; executes `sysretq`.
9. **CPU** returns to ring 3 at the instruction after `syscall`, `rax` = 2.
10. **glibc** sees positive value, returns it to the caller.

---

## 16. Mental Model Summary

- A syscall is a **hardware-assisted controlled jump** from ring 3 to a fixed kernel entry point.
- The kernel maintains a **table of function pointers** indexed by syscall number; the dispatcher is just `tablenr`.
- All user data crossing the boundary goes through **copy_from/to_user** with fault fixups and SMAP.
- The path is wrapped by **security, audit, trace, and signal** logic, making the syscall the natural enforcement point.
- The ABI (numbers, register conventions, struct layouts) is **frozen forever** — this is the contract that lets a 1995 binary still run on a 2026 kernel.

If you want, I can drill deeper into any one layer: the entry assembly, seccomp internals, `copy_from_user` exception tables, the vDSO clock data page, KPTI page-table switching, or how `SYSCALL_DEFINEx` expands — just say which.