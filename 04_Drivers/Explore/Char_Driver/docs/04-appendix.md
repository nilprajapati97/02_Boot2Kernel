# Part 4 — Appendix: Reference, Version Notes, Pitfalls & Interview Q&A

Quick-reference material to back up [01-design.md](01-design.md) and
[02-internals.md](02-internals.md). All signatures verified against **kernel 6.17.0-35**
headers in `/lib/modules/$(uname -r)/build`.

---

## A. API quick reference

### Device numbers — `<linux/fs.h>`, `<linux/kdev_t.h>`

| Function / macro | Signature | Purpose |
|------------------|-----------|---------|
| `alloc_chrdev_region` | `int alloc_chrdev_region(dev_t *dev, unsigned baseminor, unsigned count, const char *name)` | Dynamically allocate a free major + `count` minors. |
| `register_chrdev_region` | `int register_chrdev_region(dev_t from, unsigned count, const char *name)` | Claim a *known* major/minor range. |
| `unregister_chrdev_region` | `void unregister_chrdev_region(dev_t from, unsigned count)` | Release the range. |
| `register_chrdev` | `int register_chrdev(unsigned major, const char *name, const struct file_operations *fops)` | Legacy all-in-one (256 minors). |
| `MKDEV` / `MAJOR` / `MINOR` | macros | Compose / decompose a `dev_t`. |

### `cdev` — `<linux/cdev.h>`

| Function | Signature | Purpose |
|----------|-----------|---------|
| `cdev_init` | `void cdev_init(struct cdev *, const struct file_operations *)` | Init embedded cdev + set ops. |
| `cdev_alloc` | `struct cdev *cdev_alloc(void)` | Allocate a standalone cdev. |
| `cdev_add` | `int cdev_add(struct cdev *, dev_t, unsigned count)` | **Publish**: device live after this. |
| `cdev_del` | `void cdev_del(struct cdev *)` | Remove from the system. |
| `cdev_device_add` | `int cdev_device_add(struct cdev *, struct device *)` | Combined cdev + device add (alternative API). |

### Driver model / nodes — `<linux/device/class.h>`, `<linux/device.h>`

| Function | Signature (6.17) | Purpose |
|----------|------------------|---------|
| `class_create` | `struct class *class_create(const char *name)` | **6.4+ single arg.** Make `/sys/class/<name>`. |
| `class_destroy` | `void class_destroy(const struct class *cls)` | Tear down the class. |
| `device_create` | `struct device *device_create(const struct class *, struct device *parent, dev_t, void *drvdata, const char *fmt, ...)` | Create `/sys` node + `dev` attr + uevent → udev. |
| `device_destroy` | `void device_destroy(const struct class *, dev_t)` | Remove the device/node. |
| `DEVICE_ATTR_RW` | macro | Define a readable/writable sysfs attribute. |

### User-memory access — `<linux/uaccess.h>`

| Function | Signature | Returns |
|----------|-----------|---------|
| `copy_to_user` | `unsigned long copy_to_user(void __user *to, const void *from, unsigned long n)` | **bytes NOT copied** (0 = success). May sleep. |
| `copy_from_user` | `unsigned long copy_from_user(void *to, const void __user *from, unsigned long n)` | bytes NOT copied. May sleep. |
| `access_ok` | `access_ok(ptr, size)` | true if range is valid user memory. |
| `get_user` / `put_user` | macros | Single scalar in/out, fast path. |

### Wait queues — `<linux/wait.h>`

| Function / macro | Purpose |
|------------------|---------|
| `init_waitqueue_head(&q)` | Initialize a `wait_queue_head_t`. |
| `wait_event_interruptible(q, cond)` | Sleep until `cond`; `0` on success, `-ERESTARTSYS` on signal. |
| `wait_event_interruptible_timeout(q, cond, t)` | As above with timeout. |
| `wake_up_interruptible(&q)` | Wake `TASK_INTERRUPTIBLE` sleepers. |
| `wake_up_interruptible_all(&q)` | Wake all of them. |

### poll — `<linux/poll.h>`, `<uapi/linux/eventpoll.h>`

| Symbol | Purpose |
|--------|---------|
| `poll_wait(filp, &q, pt)` | Register a wait queue with the poll core (does not sleep). |
| `EPOLLIN` (=`0x1`) `EPOLLRDNORM` | Readable. |
| `EPOLLOUT` `EPOLLWRNORM` | Writable. |
| `__poll_t` | The bitmask return type of `.poll`. |

### ioctl — `<uapi/asm-generic/ioctl.h>`

| Macro | Purpose |
|-------|---------|
| `_IO(type,nr)` | Command with no data. |
| `_IOR(type,nr,dtype)` | Reads data to user (dir = READ). |
| `_IOW(type,nr,dtype)` | Writes data from user (dir = WRITE). |
| `_IOWR(type,nr,dtype)` | Bidirectional. |
| `_IOC_DIR/TYPE/NR/SIZE(cmd)` | Decode the fields. |
| `compat_ptr_ioctl` | Generic `.compat_ioctl` for pointer args. |

### mmap / memory — `<linux/mm.h>`, `<linux/slab.h>`, `<linux/vmalloc.h>`

| Function | Signature / note |
|----------|------------------|
| `remap_pfn_range` | `int remap_pfn_range(struct vm_area_struct *, unsigned long addr, unsigned long pfn, unsigned long size, pgprot_t)` |
| `vmalloc_to_page` | `struct page *vmalloc_to_page(const void *addr)` — for `.fault` handlers. |
| `kmalloc` / `kzalloc` | `void *kmalloc(size_t, gfp_t)` — physically contiguous. |
| `vmalloc` | `void *vmalloc(unsigned long)` — virtually contiguous only. |
| `kfree` / `vfree` | Free the above. |

### procfs — `<linux/proc_fs.h>`, `<linux/seq_file.h>`

| Symbol | Note |
|--------|------|
| `proc_create` | `proc_create(name, mode, parent, const struct proc_ops *)` — **`proc_ops`, not `file_operations`, since 5.6.** |
| `struct proc_ops` | `.proc_open/.proc_read/.proc_lseek/.proc_release`. |
| `seq_printf` / `single_open` | Safe arbitrary-size `/proc` output. |
| `proc_remove` | Remove the entry. |

---

## B. `_IOC` encoding {#ioctl-encoding}

A command number is a 32-bit word:

```text
 bit  31 30 │ 29 ........... 16 │ 15 ........ 8 │ 7 ......... 0
      ┌─────┼──────────────────┼───────────────┼──────────────┐
      │ DIR │       SIZE        │     TYPE      │     NR       │
      │ (2) │       (14)        │     (8)       │     (8)      │
      └─────┴──────────────────┴───────────────┴──────────────┘
```

| Field | Bits | Built by | Decoded by | Meaning |
|-------|------|----------|------------|---------|
| DIR | 31–30 | `_IO/_IOR/_IOW/_IOWR` | `_IOC_DIR` | `_IOC_NONE`(0), `_IOC_WRITE`(1), `_IOC_READ`(2), R+W(3) — from app's view. |
| SIZE | 29–16 | `sizeof(dtype)` in the macro | `_IOC_SIZE` | Argument size in bytes (≤ 16383). |
| TYPE | 15–8 | you pick a magic byte | `_IOC_TYPE` | Driver/subsystem identity (we use `'K'`). |
| NR | 7–0 | you number commands | `_IOC_NR` | Per-driver command ordinal. |

### `scullbuf` command set

| Macro | Expansion | DIR | Arg | Effect |
|-------|-----------|-----|-----|--------|
| `SCULLBUF_IOCRESET` | `_IO('K', 0)` | NONE | — | Set `end = 0` (empty the buffer). |
| `SCULLBUF_IOCSQUANTUM` | `_IOW('K', 1, int)` | WRITE | `int *` | `copy_from_user` → `dev->quantum`. |
| `SCULLBUF_IOCGQUANTUM` | `_IOR('K', 2, int)` | READ | `int *` | `dev->quantum` → `copy_to_user`. |

Handler guard: reject when `_IOC_TYPE(cmd) != 'K'` or `_IOC_NR(cmd) > 2` with **`-ENOTTY`**.

---

## C. Kernel 6.x version notes (what bites when porting)

| Change | Since | Impact |
|--------|-------|--------|
| **`class_create(name)` single arg** | 6.4 | Old `class_create(THIS_MODULE, name)` **won't compile** on 6.17. The #1 build break. |
| **`proc_create` takes `struct proc_ops`** | 5.6 | Old code passing `file_operations` to `proc_create` won't compile. |
| **`set_fs()`/`get_fs()` removed** | ~5.10 (x86 earlier) | Cannot toggle address limit; use explicit `copy_*_user` / `kernel_read`. Verified absent on 6.17. |
| **`fop_flags` field in `file_operations`** | ~6.11 | New flags field after `.owner`; e.g. `FOP_UNSIGNED_OFFSET`. Initialize via designated initializers (we already do). |
| **`mmap_prepare` callback** | ~6.13 | New split-phase mmap setup in `file_operations` (verified present 6.17). Classic `.mmap` still supported. |
| **`unlocked_ioctl` (no BKL)** | 2.6.36 era | `.ioctl` member gone; you must lock yourself. |
| **`read_iter`/`write_iter` preferred** | ongoing | Classic `.read`/`.write` still wrapped by `new_sync_read/write`; iter forms needed for async/vectored/io_uring. |
| **`access_ok(ptr, size)` 2-arg** | 5.0 | Old 3-arg form (with type) is gone. |
| **`register_chrdev` legacy** | always | Fine for toys; prefer `alloc_chrdev_region` + `cdev` + `class/device_create`. |
| **`no_llseek` removed / default seek behavior** | 6.12 | `no_llseek` deleted; use `nonseekable_open` / leave `.llseek` per intent. |

> When in doubt on a target kernel, grep the headers:
> `grep -n "class_create" /lib/modules/$(uname -r)/build/include/linux/device/class.h`.

---

## D. Common pitfalls & race conditions

1. **`class_create` two-arg on 6.x** — won't compile. Drop `THIS_MODULE`.
2. **Forgetting `.owner = THIS_MODULE`** — module can be `rmmod`'d while an fd is open →
   use-after-free. Set it on both `fops` and `cdev`.
3. **`cdev_add` before the device is initialized** — a parallel `open` can run the instant
   `cdev_add` returns; init buffer/locks/wait-queues *first*.
4. **Sleeping while holding a spinlock** — `copy_*_user`, `kmalloc(GFP_KERNEL)`,
   `wait_event_*`, `mutex_lock` all may sleep; never under a spinlock. Use a mutex (design §9).
5. **Sleeping while holding the device mutex in the blocking path** — drop it before
   `wait_event_*`, or the waker can't make progress (deadlock, internals §8).
6. **`if` instead of `while` around the wait condition** — thundering-herd wakeups need a
   re-check loop; `wait_event_*` loops internally, but your outer re-lock must re-test.
7. **Trusting `ioctl`'s `arg` as a kernel pointer** — always `copy_*_user`; validate
   `_IOC_TYPE`/`_IOC_NR` and return `-ENOTTY` for foreign commands.
8. **Ignoring `copy_*_user`'s return value** — it returns *bytes not copied*; nonzero means
   `-EFAULT`. It is **not** a 0/-1 success flag.
9. **mmap past the buffer** — bound the VMA size to the allocation; mapping beyond it leaks
   kernel memory to userspace (security bug).
10. **`kmalloc` vs `vmalloc` mismatch with mmap** — `vmalloc` memory has no contiguous PFN
    run, so `remap_pfn_range` is wrong; use a `.fault` handler (internals §11).
11. **`GFP_KERNEL` in atomic context** — in IRQ/softirq or under a spinlock use `GFP_ATOMIC`.
12. **`proc_create` with `file_operations`** — must be `struct proc_ops` since 5.6.
13. **Wrong teardown order** — free in reverse of init; e.g. `device_destroy` before
    `cdev_del` before `kfree(buffer)` before `class_destroy` before `unregister_chrdev_region`.
14. **Not handling `-ERESTARTSYS`** — return it (don't swallow) so blocked syscalls stay
    killable and restartable.

---

## E. Glossary

| Term | Meaning |
|------|---------|
| **VFS** | Virtual File System: the uniform `open/read/write/close` layer. |
| **`dev_t`** | 32-bit device id = major (driver) + minor (instance). |
| **major / minor** | Which driver / which instance of it. |
| **`struct inode`** | The file object itself (one per file); device id in `i_rdev`. |
| **`struct file` (filp)** | One open instance; holds `f_pos`, `f_flags`, `f_op`, `private_data`. |
| **`file_operations`** | The driver's vtable of syscall callbacks. |
| **`struct cdev`** | Kernel object binding a `dev_t` range to a `file_operations`. |
| **`cdev_map`** | `kobj_map` that resolves a `dev_t` to its `cdev` at open time. |
| **`def_chr_fops`** | Bootstrap fops on a char inode; `.open = chrdev_open`. |
| **`container_of`** | Compile-time macro: member pointer → enclosing struct pointer. |
| **wait queue** | List of sleeping tasks woken on a condition (`wait_event_*`/`wake_up_*`). |
| **`-ERESTARTSYS`** | "Interrupted by signal; restart or report EINTR" — keeps syscalls killable. |
| **`poll_table`** | Structure the poll core hands to `.poll` so it can register wait queues. |
| **`__poll_t`** | Bitmask of `EPOLLIN`/`EPOLLOUT`/... readiness flags. |
| **`_IOC`** | The ioctl command-number encoding (dir/type/nr/size). |
| **VMA** (`vm_area_struct`) | One contiguous region of a process's virtual address space. |
| **PFN** | Page Frame Number = physical address >> `PAGE_SHIFT`. |
| **kobject** | Refcounted base object; every `/sys` directory is one. |
| **uevent** | Kernel→userspace event (over netlink) that drives udev. |
| **devtmpfs** | In-kernel `/dev` filesystem that auto-creates device nodes. |
| **udev** | Userspace daemon applying naming/permission policy to device nodes. |
| **GFP flags** | Allocation context/behavior flags (`GFP_KERNEL` may sleep; `GFP_ATOMIC` won't). |
| **`__init`/`__exit`** | Section markers; init code freed post-boot/load. |

---

## F. Interview Q&A self-check

Try to answer before expanding the reasoning. Each answer is grounded in the body docs.

1. **What is the difference between `struct inode` and `struct file`?**
   Inode = the file itself, one per file, holds `i_rdev`/`i_cdev`. File = one open instance,
   holds `f_pos`/`f_flags`/`f_op`/`private_data`. Two opens of one node → two files, one
   inode. (Internals §1.)

2. **From `open("/dev/x")` to your `->open`, what actually happens?**
   Path walk → char inode with `i_fop = def_chr_fops` → `do_dentry_open` calls
   `def_chr_fops.open = chrdev_open` → `kobj_lookup(cdev_map, i_rdev)` finds the `cdev` →
   caches `i_cdev`, `cdev_get`/`try_module_get` → `replace_fops(filp, yours)` → calls your
   `->open`. (Internals §4.)

3. **How does one `file_operations` serve many minors?**
   `inode->i_cdev` is set by `chrdev_open`; your `open` does `container_of(inode->i_cdev,
   struct mydev, cdev)` (or indexes by `MINOR(i_rdev)`), then caches it in
   `filp->private_data`. (Design §7.)

4. **Why can't you `memcpy` a user pointer in `read`?**
   User and kernel are separate MMU-enforced address spaces; the user page may be unmapped,
   invalid, or fault mid-copy. Use `copy_to_user`, which `access_ok`-validates and copies
   under exception-table protection. (Internals §6.)

5. **What do `copy_to_user`/`copy_from_user` return?**
   The number of bytes **not** copied (0 = full success). Nonzero → `-EFAULT`. They may
   sleep (page faults). (Internals §6, Appendix A.)

6. **Why was `set_fs()` removed?**
   It let code flip the user/kernel address limit, which bugs/exploits could abuse to make
   the kernel treat kernel pointers as user (bypassing `access_ok`). Removing it eliminates
   that vulnerability class. (Internals §6.)

7. **Explain the lost-wakeup race and how `wait_event_interruptible` avoids it.**
   If you check the condition, then sleep, a wakeup firing in between is lost. The fix:
   set `TASK_INTERRUPTIBLE` **before** the final condition re-check, with a barrier pairing
   `set_current_state` and `wake_up`; a concurrent wakeup then prevents `schedule()` from
   sleeping. (Internals §8.)

8. **Why drop the device mutex before sleeping in a blocking read?**
   The writer needs that mutex to produce data and fire the wakeup; sleeping while holding
   it deadlocks. Unlock → wait → relock → re-check. (Design §10, Internals §8.)

9. **Why a `while` (not `if`) around the wait condition?**
   Thundering herd: a wakeup may rouse several readers but only one can consume the data;
   the others must re-check and sleep again. (Internals §8.)

10. **How does `poll` cooperate with blocking I/O?**
    `.poll` calls `poll_wait` to register the *same* wait queues used by `read`/`write`,
    then returns a readiness mask. The existing `wake_up_interruptible` wakes both blocking
    readers and poll/epoll waiters. (Internals §9.)

11. **What's in an ioctl command number and why?**
    DIR(2)+SIZE(14)+TYPE(8)+NR(8). The TYPE magic byte makes commands driver-unique so a
    wrong-device ioctl fails with `-ENOTTY` instead of corrupting state. (Internals §10,
    Appendix B.)

12. **`unlocked_ioctl` vs `compat_ioctl`?**
    `unlocked_ioctl` runs with no BKL (you lock yourself). `compat_ioctl` handles a 32-bit
    process on a 64-bit kernel (pointer/long width); `compat_ptr_ioctl` is the generic shim.
    (Internals §10.)

13. **Why does the mmap implementation depend on `kmalloc` vs `vmalloc`?**
    `kmalloc` is physically contiguous → one PFN run → `remap_pfn_range` (eager). `vmalloc`
    is physically scattered → no PFN run → a `vm_ops->fault` handler resolving each page via
    `vmalloc_to_page` (lazy). (Internals §11.)

14. **How does `/dev/x` get created automatically?**
    `class_create` + `device_create` register a `struct device`, create
    `/sys/class/.../dev`, and emit a `KOBJ_ADD` uevent over netlink; devtmpfs makes the node
    immediately and udev applies permissions/policy. (Internals §12.)

15. **Why can't you `rmmod` a driver with an open fd?**
    `chrdev_open` did `try_module_get(cdev->owner)`; the module refcount is nonzero until
    last `fput` calls `module_put`. `delete_module` refuses a nonzero refcount → `EBUSY`.
    (Internals §13.)

16. **`GFP_KERNEL` vs `GFP_ATOMIC`?**
    `GFP_KERNEL` may sleep (reclaim) — process context only. `GFP_ATOMIC` never sleeps — for
    IRQ/softirq or under a spinlock; smaller pool, fails more easily. (Internals §14.)

17. **What's special about `cdev_add` timing?**
    The device is **live** the instant it returns — a concurrent `open` can call your fops
    immediately. All per-device state must be initialized before it. (Design §5, §15.)

18. **What changed in `class_create` and `proc_create` on modern kernels?**
    `class_create` is single-arg since 6.4 (no `THIS_MODULE`); `proc_create` takes
    `struct proc_ops` (not `file_operations`) since 5.6. Both are common build breaks.
    (Appendix C.)

19. **Char vs block vs network device — one line each.**
    Char = byte stream via `file_operations`, driver sees I/O directly. Block = fixed blocks
    behind the page cache + block layer, hosts filesystems. Network = no `/dev` node, socket
    API + `struct net_device`. (Design §1.)

20. **Classic `.read` vs `.read_iter` — which is called when?**
    If only `.read_iter` exists, a `read(2)` is wrapped by `new_sync_read` (builds a 1-seg
    `iov_iter` + `kiocb`). Vectored/async/io_uring use the iter form directly. Classic
    `.read` is called directly when present. (Internals §5.)

---

### Back to the top

- [README](../README.md) · [01 Design](01-design.md) · [02 Internals](02-internals.md) ·
  [03 Traces & Diagrams](03-traces-and-diagrams.md)
