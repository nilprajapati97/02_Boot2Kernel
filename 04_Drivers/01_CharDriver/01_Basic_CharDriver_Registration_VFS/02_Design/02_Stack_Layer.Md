## Deep Dive: `open("/dev/MyAnilDev")` End-to-End

## 2. The Big Picture — Full Stack Flow

```
User Space                     Kernel Space
─────────────────────────────────────────────────────
  open("/dev/MyAnilDev")
       │
       ▼
  C library (glibc)  ──→  sys_open()  [syscall table]
                               │
                               ▼
                          vfs_open()  [Virtual File System]
                               │
                        Looks up inode in /dev/
                        Inode holds: major, minor, type
                               │
                               ▼
                          chrdev_open()  [fs/char_dev.c]
                          Finds cdev via chrdevs[] map
                               │
                               ▼
                          my_open()   ← YOUR DRIVER CODE
```

Every `read()`, `write()`, `ioctl()` follows the same path through VFS → your `file_operations`.


### Step 1 — User Space: `open()` C library call

```c
int fd = open("/dev/MyAnilDev", O_RDWR);
```

This is a **POSIX libc wrapper** (glibc). It does nothing more than:
1. Place the syscall number (`__NR_openat` on modern kernels) in a register
2. Execute the `SVC #0` (ARM64) or `SYSCALL` (x86-64) instruction — this **traps into kernel mode**

The CPU switches privilege level (EL0 → EL1 on ARM64). The kernel's **syscall table** dispatches to `sys_openat()`.

---

### Step 2 — Syscall Entry: `sys_open()` / `sys_openat()`

```
arch/arm64/kernel/entry.S  →  do_el0_svc()  →  sys_openat()
```

`sys_openat()` is defined in `fs/open.c`. It:
- Copies the filename string from **user address space** into kernel memory (safe copy)
- Calls `do_filp_open()` which eventually reaches `vfs_open()`

At this point you're fully inside the kernel, running in **process context** (the calling process's task_struct is still `current`).

---

### Step 3 — VFS Layer: `vfs_open()`

The **Virtual File System** is an abstraction layer that makes ALL filesystems look the same to the kernel. It doesn't know if you're opening a file on ext4, tmpfs, or `/dev/`.

```
vfs_open()
    └─ do_dentry_open()
           └─ inode->i_fop->open()   ← dispatches based on file type
```

**What is an inode?**

Every file/device in Linux has an `inode` — a kernel data structure holding metadata. For `/dev/MyAnilDev`:

```c
struct inode {
    umode_t         i_mode;   // S_IFCHR — this is a char device!
    dev_t           i_rdev;   // encoded major:minor  (e.g., 245:0)
    struct cdev    *i_cdev;   // pointer filled in by chrdev_open
    // ...
};
```

The inode for `/dev/MyAnilDev` lives on **devtmpfs** (the pseudo-filesystem backing `/dev/`). When `udev` ran `mknod` (or `device_create()` triggered a uevent), this inode was created with `i_mode = S_IFCHR` and `i_rdev` holding the `major:minor` you registered.

VFS sees `S_IFCHR` → calls `chrdev_open()` via `def_chr_fops`.

---

### Step 4 — `chrdev_open()` in `fs/char_dev.c`

This is the **bridge between VFS and your driver**. Its job is to find your `cdev` and swap in your `file_operations`.

```c
// fs/char_dev.c (simplified)
static int chrdev_open(struct inode *inode, struct file *filp)
{
    struct cdev *p;
    dev_t dev = inode->i_rdev;

    // Look up the cdev registered for this major:minor
    p = cdev_get(inode);          // searches kobj_map (chrdevs[])
    if (!p)
        return -ENXIO;            // No driver registered → "No such device"

    inode->i_cdev = p;            // cache it on the inode

    // *** KEY MOMENT: replace generic fops with YOUR driver's fops ***
    filp->f_op = fops_get(p->ops);

    // Now call YOUR open() if it exists
    if (filp->f_op->open)
        return filp->f_op->open(inode, filp);

    return 0;
}
```

**The `chrdevs[]` map** is a hash table (actually `kobj_map`) keyed by `dev_t`. When you called `cdev_add()` during module init, your `struct cdev` was inserted here. `chrdev_open()` extracts `major:minor` from the inode and does a lookup — this is how the kernel knows which of potentially hundreds of registered drivers to call.

**Critical detail — `filp->f_op` swap**: After `chrdev_open()` runs, the `struct file`'s function table is permanently replaced with YOUR `file_operations`. All subsequent `read()`, `write()`, `ioctl()` calls on this fd bypass `chrdev_open` entirely and go **directly to your driver**.

---

### Step 5 — Your Driver: `my_open()`

```c
static int my_open(struct inode *inode, struct file *filp)
{
    pr_info("MyAnilDev: opened\n");
    // optionally store per-open state:
    // filp->private_data = my_per_open_state;
    return 0;
}
```

**What the two parameters mean:**

| Parameter | Type | What it represents |
|-----------|------|--------------------|
| `inode` | `struct inode *` | The **file itself** — shared across all opens of the same device. Contains `i_rdev` (major:minor), `i_cdev` pointer |
| `filp` | `struct file *` | **This specific open instance** — unique per `open()` call. Holds flags (`O_RDWR`), current position (`f_pos`), and `private_data` for your per-fd state |

If you have multiple minor numbers (e.g., `/dev/MyAnilDev0`, `/dev/MyAnilDev1`), you extract which instance was opened via:

```c
int minor = iminor(inode);   // which minor number
```

---

### The Complete Picture — Objects in Play

```
open("/dev/MyAnilDev")
         │
         │  [EL0 → EL1 trap]
         ▼
  sys_openat()                    // fs/open.c
  allocates struct file           // one per open() call
         │
         ▼
  vfs_open()                      // fs/namei.c
  resolves path → dentry → inode
  inode.i_mode = S_IFCHR
  inode.i_rdev = MKDEV(245, 0)
         │
         ▼
  chrdev_open()                   // fs/char_dev.c
  kobj_map_lookup(dev_t=245:0)
  → finds your struct cdev
  filp->f_op = &my_fops           // ← THE KEY SWAP
         │
         ▼
  my_open(inode, filp)            // YOUR CODE
  return 0
         │
         ▼
  fd = 3  returned to user space  // index into process fd table
                                  // fd table[3] → struct file → my_fops
```

After this, every `read(fd, ...)` from user space goes: `sys_read()` → looks up `fd` → gets `struct file` → calls `filp->f_op->read` → **your `my_read()` directly**. No VFS lookup needed again.

---

### Why `-ENXIO` and not `-ENOENT`?

If `/dev/MyAnilDev` exists as a device node (inode is present) but no driver registered that `major:minor`, `chrdev_open()` returns `-ENXIO` ("No such device"). The file *exists* — the driver just isn't loaded. This is why `insmod` must come before the first `open()`.