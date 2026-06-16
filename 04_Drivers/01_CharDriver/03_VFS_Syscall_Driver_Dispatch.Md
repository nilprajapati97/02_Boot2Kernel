# VFS Syscall to CharDriver Dispatch

The Virtual File System (VFS) is the abstraction layer that routes user-space syscalls to the correct kernel driver via `struct file_operations`.

---

## Why VFS Exists

Linux supports many filesystems (ext4, NFS, procfs, sysfs, devtmpfs). VFS provides a uniform syscall API (`open`, `read`, `write`, `ioctl`, `close`) regardless of the underlying filesystem or device driver. Drivers only need to fill a `file_operations` table — VFS handles the rest.

---

## Core VFS Structures

| Structure | Role |
|---|---|
| `struct file_operations` | Driver-supplied function pointer table; VFS dispatches syscalls through it |
| `struct inode` | Kernel representation of a file/device node; holds `dev_t` for char devices |
| `struct file` | Per-open-instance state; holds `f_op`, `f_pos`, `f_flags`, `private_data` |

---

## Dispatch Flow

```
Userspace App
  open() / read() / write() / ioctl()
        │
        ▼
  Syscall Table (sys_open / sys_read / ...)
        │
        ▼
  VFS Layer
  ├─ Resolves path → struct inode
  ├─ Allocates struct file, sets file->f_op = inode->i_fop
  └─ Calls file->f_op-><operation>()
        │
        ▼
  CharDriver file_operations
  (.open / .read / .write / .ioctl)
        │
        ▼
  Kernel Buffers / Hardware
```

---

## Per-Syscall VFS Path

### open()

```c
fd = open("/dev/mychardev", O_RDWR);
```

1. `sys_open()` → `do_sys_open()` → `do_filp_open()`
2. VFS resolves path → finds `struct inode` with `dev_t`
3. Allocates `struct file`, sets `file->f_op = inode->i_fop`
4. Calls `file->f_op->open(inode, file)`
5. Driver stores per-open state in `file->private_data`

---

### read()

```c
read(fd, buf, size);
```

1. `sys_read()` → `vfs_read()` → `file->f_op->read(file, ubuf, size, &pos)`
2. Driver copies kernel data to user:

```c
copy_to_user(ubuf, kbuf, size);
```

---

### write()

```c
write(fd, buf, size);
```

1. `sys_write()` → `vfs_write()` → `file->f_op->write(file, ubuf, size, &pos)`
2. Driver copies user data to kernel:

```c
copy_from_user(kbuf, ubuf, size);
```

---

### ioctl()

```c
ioctl(fd, CMD, arg);
```

1. VFS → `file->f_op->unlocked_ioctl(file, cmd, arg)`
2. Driver decodes `cmd` and transfers structured data via `copy_from_user` / `copy_to_user`

---

### close()

```c
close(fd);
```

1. VFS decrements file refcount; on last close calls `file->f_op->release(inode, file)`
2. Driver frees `file->private_data` and releases hardware resources

---

## inode vs file

| | `struct inode` | `struct file` |
|---|---|---|
| Lifetime | Persistent per filesystem node | Per `open()` call (per fd) |
| Holds | `dev_t`, permissions, timestamps | `f_op`, `f_pos`, `f_flags`, `private_data` |
| Purpose | Identifies the device | Carries per-open state |

Multiple `open()` calls on the same device node → multiple `struct file` instances, one shared `struct inode`.

---

## copy_to_user / copy_from_user

Kernel must never directly dereference user-space pointers — the page may not be mapped, or the pointer may be malicious.

```c
// Kernel → User (read)
if (copy_to_user(user_buf, kernel_buf, len))
    return -EFAULT;

// User → Kernel (write)
if (copy_from_user(kernel_buf, user_buf, len))
    return -EFAULT;
```

Both return the number of bytes that could NOT be copied (0 = success).

---

## Summary

VFS is the bridge between user-space syscalls and kernel driver implementations. It uses `struct inode` to identify the device, `struct file` to track per-open state, and `struct file_operations` to dispatch each syscall to the correct driver function.
