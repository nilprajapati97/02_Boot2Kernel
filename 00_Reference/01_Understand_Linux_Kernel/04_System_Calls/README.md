# Chapter 04 — System Calls

> **Book:** Linux Kernel Development — Robert Love (3rd Edition)  
> **Goal:** Understand how user-space programs communicate with the kernel through system calls — the mechanism, the table, parameter passing, and how to add a new system call.

---

## Topic Index

| File | Description |
|------|-------------|
| [01_What_Are_System_Calls.md](./01_What_Are_System_Calls.md) | Definition, POSIX, libc wrappers |
| [02_System_Call_Handler.md](./02_System_Call_Handler.md) | syscall instruction, entry_64.S, privilege switch |
| [03_System_Call_Table.md](./03_System_Call_Table.md) | sys_call_table[], syscall numbers |
| [04_Adding_A_New_System_Call.md](./04_Adding_A_New_System_Call.md) | How to implement a new syscall |
| [05_Parameter_Passing.md](./05_Parameter_Passing.md) | Registers, copy_from_user, copy_to_user |

---

## Chapter Flow

```mermaid
flowchart TD
    A[User calls write\(\)] --> B[libc: glibc wrapper\nsets rax=1]
    B --> C[syscall instruction\nRing3 → Ring0]
    C --> D[entry_64.S: save_regs]
    D --> E[sys_call_table\[1\] = sys_write]
    E --> F[sys_write\(\) runs]
    F --> G[VFS layer → device]
    G --> H[return to user space\nRing0 → Ring3]
```
