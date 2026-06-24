# Part 1 — The Character Driver Model & The Design of `scullbuf`

This document defines **what** we are building and **why** each design decision is made.
The next document ([02-internals.md](02-internals.md)) explains **how** the kernel makes
it all work underneath.

---

## 1. Two kinds of device, and where char fits

The kernel historically exposes hardware through device special files in `/dev`.
There are two classic flavors:

- **Character (char) devices** — accessed as a **stream of bytes**, like a file you can
  only (by default) move through sequentially. `read`/`write` transfer arbitrary numbers
  of bytes. Examples: `/dev/ttyS0`, `/dev/null`, `/dev/random`, sound, most "simple"
  peripherals. There is *no* required block size and *no* page-cache between the app and
  the driver — the driver sees the I/O directly.
- **Block devices** — accessed in **fixed-size blocks**, sit behind the block layer and
  the page cache, can host filesystems, and are optimized for random access with request
  queues and I/O schedulers. Examples: `/dev/sda`, `/dev/nvme0n1`.

A third category, **network devices**, is not represented by `/dev` nodes at all; they
use the socket API and `struct net_device`.

We are building a **char device** because our contract is "a stream of bytes into and out
of a kernel buffer," which is the canonical char-device shape. The key practical
consequence: a char driver supplies a `struct file_operations` table and the kernel calls
our functions **directly and synchronously** in the context of the calling process —
there is no page cache or request queue interposed.

### Design goal

`scullbuf` is a RAM-backed "disk that isn't a disk": several independent in-memory
buffers, each reachable through its own `/dev/scullbufN` node, supporting the full char
contract (open/release/read/write/llseek/ioctl/poll/mmap) with correct concurrency and
blocking semantics. It is deliberately hardware-free so that 100% of the complexity is
the *kernel-interface* complexity we want to study.

---

## 2. Architecture of the design

```text
   ┌────────────────────────────────────────────────────────────────────┐
   │ module-global state                                                │
   │   dev_t        scullbuf_devno_base;   // major+first minor         │
   │   struct class *scullbuf_class;       // for /sys/class + udev     │
   │   int          scullbuf_nr_devs = 4;  // number of minors          │
   │   struct scullbuf_dev *scullbuf_devices;   // array, one per minor │
   │   struct proc_dir_entry *scullbuf_proc;                            │
   └───────────────────────────────┬────────────────────────────────────┘
                                    │ array indexed by MINOR()
        ┌───────────────────────────┼───────────────────────────┐
        ▼                           ▼                           ▼
 ┌──────────────┐           ┌──────────────┐            ┌──────────────┐
 │ scullbuf_dev │           │ scullbuf_dev │            │ scullbuf_dev │  ...
 │  [minor 0]   │           │  [minor 1]   │            │  [minor 2]   │
 ├──────────────┤           ├──────────────┤            ├──────────────┤
 │ buffer*      │           │ buffer*      │            │ buffer*      │
 │ buffersize   │           │ ...          │            │ ...          │
 │ end (valid)  │           │              │            │              │
 │ struct mutex │           │              │            │              │
 │ wait_queue rq│           │              │            │              │
 │ wait_queue wq│           │              │            │              │
 │ struct cdev  │ <───── one embedded cdev per device, all share fops  │
 └──────────────┘           └──────────────┘            └──────────────┘
```

Every minor is a fully independent device with its own buffer, its own lock, and its own
wait queues. They **share one `struct file_operations`** because the *code* is identical;
only the *data* differs per minor. The trick that connects "which file was opened" to
"which `scullbuf_dev`" is covered in §7.

---

## 3. The per-device data structure

```c
struct scullbuf_dev {
        char              *buffer;     /* the storage (kmalloc or vmalloc) */
        size_t             buffersize; /* total capacity in bytes         */
        size_t             end;        /* number of valid bytes [0..size] */

        struct mutex       lock;       /* serializes all access to fields */
        wait_queue_head_t  readq;      /* readers wait here when empty    */
        wait_queue_head_t  writeq;     /* writers wait here when full     */

        unsigned int       quantum;    /* tunable via ioctl               */
        unsigned long      n_readers;  /* open-for-read count (stats)     */
        unsigned long      bytes_written, bytes_read; /* /proc stats      */

        struct cdev        cdev;       /* kernel's char-device object     */
        struct device     *device;     /* the /sys + /dev node handle     */
};
```

Design rationale for each field:

- **`buffer` / `buffersize` / `end`** — the actual model state. `end` is the high-water
  mark of valid data; `read` returns up to `end - f_pos` bytes, `write` extends `end`.
- **`lock` (a `struct mutex`)** — a *sleeping* lock. We choose a mutex, not a spinlock,
  because our critical sections call `copy_to_user`/`copy_from_user`, which **may sleep**
  (they can take a page fault on the user buffer). You may not sleep while holding a
  spinlock; you may while holding a mutex. (See §9 and internals §6–7.)
- **`readq` / `writeq`** — wait queues that turn a busy device into a *blocking* device:
  a reader with no data to read goes to sleep on `readq` instead of spinning; a writer
  wakes `readq` after producing data. Two queues (not one) let us wake exactly the side
  that can now make progress.
- **`cdev`** — the kernel's representation of "a char device that owns a range of
  `(major,minor)` numbers." Embedding it (rather than `cdev_alloc()`) lets us reach the
  containing `scullbuf_dev` with `container_of()` — but see §7 for the subtlety that we
  actually recover the device from `inode->i_cdev`.
- **`device`** — the handle returned by `device_create()`, kept so we can tear the node
  down in the reverse order at unload.

> **Interview point:** "Why a mutex and not a spinlock here?" → Because the critical
> section can sleep (user-memory access can fault and block). Spinlocks forbid sleeping
> because the CPU holding them is busy-waiting with preemption/IRQs affected; sleeping
> would risk deadlock and unbounded latency.

---

## 4. Device numbers: `dev_t`, major, minor

A device node is identified by a **`dev_t`**, a 32-bit value split into a **major**
(which driver) and a **minor** (which instance). The kernel encodes this with macros from
`<linux/kdev_t.h>`:

```c
dev_t dev = MKDEV(major, minor);   /* compose            */
unsigned mj = MAJOR(dev);          /* extract major (12 bits in modern kernels) */
unsigned mi = MINOR(dev);          /* extract minor (20 bits)                   */
```

There are three ways to claim numbers; we choose **dynamic allocation**:

| API                                    | When to use                                                        | Signature                                                                                     |
| -------------------------------------- | ------------------------------------------------------------------ | --------------------------------------------------------------------------------------------- |
| `register_chrdev_region()`           | You know the major you want (static).                              | `int register_chrdev_region(dev_t from, unsigned count, const char *name)`                  |
| **`alloc_chrdev_region()`** ✅ | Let the kernel pick a free major (avoids collisions).              | `int alloc_chrdev_region(dev_t *dev, unsigned baseminor, unsigned count, const char *name)` |
| `register_chrdev()` (legacy)         | One-call old API; registers all 256 minors and wires fops for you. | `int register_chrdev(unsigned major, const char *name, const struct file_operations *fops)` |

Our init does:

```c
ret = alloc_chrdev_region(&scullbuf_devno_base, 0 /*baseminor*/,
                          scullbuf_nr_devs, "scullbuf");
/* scullbuf_devno_base now holds MKDEV(<dynamic major>, 0) */
```

We reserve `scullbuf_nr_devs` consecutive minors starting at 0. The matching teardown is
`unregister_chrdev_region(scullbuf_devno_base, scullbuf_nr_devs)`.

> **Why dynamic?** Hard-coding a major risks colliding with another driver and is not
> portable across systems. The cost is that the major isn't known until load time —
> which is exactly why udev/`device_create` (see §8) exists to create the nodes for us.

---

## 5. Registering with the char subsystem: `struct cdev`

Claiming numbers does **not** connect them to our code. The object that says "these
`(major,minor)`s are handled by *this* `file_operations`" is `struct cdev`. Verified
layout on 6.17 (`include/linux/cdev.h`):

```c
struct cdev {
        struct kobject              kobj;   /* refcount + sysfs identity */
        struct module              *owner;  /* THIS_MODULE: pins module  */
        const struct file_operations *ops;  /* our callbacks             */
        struct list_head            list;   /* inodes referring to it    */
        dev_t                       dev;    /* first dev number          */
        unsigned int                count;  /* how many minors           */
} __randomize_layout;
```

For each minor we run the standard two-step:

```c
void scullbuf_setup_cdev(struct scullbuf_dev *d, int index)
{
        dev_t devno = MKDEV(MAJOR(scullbuf_devno_base),
                            MINOR(scullbuf_devno_base) + index);

        cdev_init(&d->cdev, &scullbuf_fops); /* wires ops, inits kobj    */
        d->cdev.owner = THIS_MODULE;         /* refcount our module      */
        cdev_add(&d->cdev, devno, 1);        /* PUBLISH: live after this */
}
```

- `cdev_init()` initializes the embedded `kobject` and sets `cdev.ops`.
- Setting `.owner = THIS_MODULE` is what lets the kernel bump our module's refcount
  whenever a file is open, so we can't be unloaded out from under an open fd (internals
  §13).
- **`cdev_add()` is the moment the device goes live.** The instant it returns, a parallel
  `open()` can already be calling our `->open`. Therefore everything the device needs
  (buffer, mutex, wait queues) must be fully initialized *before* `cdev_add()`.

> **Embedded `cdev` vs `cdev_alloc()`:** We embed `struct cdev` inside `scullbuf_dev`. The
> alternative, `cdev_alloc()`, returns a standalone, kernel-managed `cdev`. Embedding is
> idiomatic for per-instance devices and keeps the lifetime tied to our own structure.
> Teardown uses `cdev_del(&d->cdev)`.

---

## 6. The contract: `struct file_operations`

`file_operations` is the **vtable** of a char driver — a struct of function pointers, one
per syscall the device can service. Unset members are `NULL`, and the VFS has sane
defaults or returns errors for the unset ones. The **verified 6.17 layout** adds two
modern members worth knowing (`fop_flags` and `mmap_prepare`); our design table:

```c
static const struct file_operations scullbuf_fops = {
        .owner          = THIS_MODULE,        /* module refcount glue          */
        .llseek         = scullbuf_llseek,    /* reposition f_pos              */
        .read           = scullbuf_read,      /* copy_to_user                  */
        .write          = scullbuf_write,     /* copy_from_user                */
        .unlocked_ioctl = scullbuf_ioctl,     /* control ops (modern, no BKL)  */
        .compat_ioctl   = compat_ptr_ioctl,   /* 32-bit app on 64-bit kernel   */
        .poll           = scullbuf_poll,      /* select/poll/epoll readiness   */
        .mmap           = scullbuf_mmap,      /* map buffer into user space    */
        .open           = scullbuf_open,      /* per-open setup                */
        .release        = scullbuf_release,   /* last close cleanup            */
        .fasync         = scullbuf_fasync,    /* optional SIGIO async notify   */
};
```

What each callback must do in our design:

| Member              | Prototype (essentials)                                                | Responsibility in`scullbuf`                                                                                                                                                              |
| ------------------- | --------------------------------------------------------------------- | ------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------ |
| `.owner`          | `struct module *`                                                   | Always`THIS_MODULE`. Prevents unload while open.                                                                                                                                         |
| `.open`           | `int (*)(struct inode *, struct file *)`                            | Recover our`scullbuf_dev` from `inode->i_cdev` via `container_of`, stash it in `filp->private_data`, bump reader counts, honor `O_TRUNC`/`O_APPEND`-like semantics if desired. |
| `.release`        | `int (*)(struct inode *, struct file *)`                            | Called on**last** close of this `struct file`; decrement counts, drop `fasync`.                                                                                                  |
| `.read`           | `ssize_t (*)(struct file *, char __user *, size_t, loff_t *)`       | Block if empty (unless`O_NONBLOCK`), `copy_to_user`, advance `*f_pos`, wake writers.                                                                                                 |
| `.write`          | `ssize_t (*)(struct file *, const char __user *, size_t, loff_t *)` | `copy_from_user`, extend `end`, advance `*f_pos`, wake readers.                                                                                                                      |
| `.llseek`         | `loff_t (*)(struct file *, loff_t, int)`                            | Implement`SEEK_SET/CUR/END` against `buffersize`; set `filp->f_pos`.                                                                                                                 |
| `.unlocked_ioctl` | `long (*)(struct file *, unsigned int cmd, unsigned long arg)`      | Decode`_IOC` command, validate, reset / get / set quantum.                                                                                                                               |
| `.compat_ioctl`   | same                                                                  | `compat_ptr_ioctl` for our pointer-free or pointer-compatible commands.                                                                                                                  |
| `.poll`           | `__poll_t (*)(struct file *, struct poll_table_struct *)`           | `poll_wait` on `readq`/`writeq`, return `EPOLLIN`/`EPOLLOUT` mask.                                                                                                               |
| `.mmap`           | `int (*)(struct file *, struct vm_area_struct *)`                   | Map the kernel buffer pages into the caller's VMA.                                                                                                                                         |
| `.fasync`         | `int (*)(int, struct file *, int)`                                  | Maintain the`fasync_struct` list for `SIGIO`.                                                                                                                                          |

> **`read` vs `read_iter`:** Modern kernels favor the iterator forms `read_iter`/
> `write_iter` (which take a `struct kiocb` + `struct iov_iter` and natively handle
> vectored and async I/O). The classic `.read`/`.write` still work — the VFS wraps them
> via `new_sync_read`/`new_sync_write`. We use the classic forms because they are clearer
> for teaching; internals §5 explains the wrapping and when you'd choose the iter forms.

> **`unlocked_ioctl` vs `ioctl`:** The old `.ioctl` ran under the Big Kernel Lock and no
> longer exists. `unlocked_ioctl` is called **without** any global lock; *we* are
> responsible for our own locking. `compat_ioctl` handles a 32-bit userspace process on a
> 64-bit kernel (pointer/`long` width differences).

---

## 7. Multiple minors: mapping an open file back to its device

We register N minors that all share `scullbuf_fops`. When `open()` happens on
`/dev/scullbuf2`, every callback receives a `struct file *` and `struct inode *` — but how
does the *code* know it is minor 2's buffer it should touch?

Two idiomatic mechanisms, used together:

1. **`container_of(inode->i_cdev, struct scullbuf_dev, cdev)`** — at `open()` time, the
   kernel has already set `inode->i_cdev` to *our* embedded `cdev` (it found it by looking
   up `inode->i_rdev` in the char subsystem; internals §3–4). Because the `cdev` is a
   member of `scullbuf_dev`, `container_of` walks back to the enclosing struct:

   ```c
   static int scullbuf_open(struct inode *inode, struct file *filp)
   {
           struct scullbuf_dev *dev =
                   container_of(inode->i_cdev, struct scullbuf_dev, cdev);

           filp->private_data = dev;        /* cache for read/write/ioctl */
           /* ...per-open setup... */
           return 0;
   }
   ```
2. **`filp->private_data`** — we stash the resolved `dev` pointer in the `struct file`, so
   every *subsequent* call (`read`, `write`, `ioctl`, `poll`, `mmap`) just does
   `struct scullbuf_dev *dev = filp->private_data;` with no recomputation.

> An equally valid alternative is `MINOR(inode->i_rdev)` used as an index into our
> `scullbuf_devices[]` array. `container_of` is preferred because it does not assume the
> array layout and works even if minors are sparse.

`container_of` itself is a compile-time pointer-arithmetic macro:

```c
#define container_of(ptr, type, member) \
        ((type *)((char *)(ptr) - offsetof(type, member)))
```

It is zero-cost at runtime — just "subtract the offset of `cdev` within `scullbuf_dev`."

---

## 8. Auto-creating the `/dev` node: class + device + udev

`cdev_add()` makes the device *functional*, but it does **not** create a `/dev` entry.
Historically you ran `mknod /dev/scullbuf0 c <major> 0` by hand. Modern systems automate
this through the **driver model + udev/devtmpfs**. Our design:

```c
/* once, in module init, after alloc_chrdev_region: */
scullbuf_class = class_create("scullbuf");      /* 6.4+ : single arg!  */

/* per minor, after cdev_add: */
d->device = device_create(scullbuf_class, NULL /*parent*/,
                          devno, NULL /*drvdata*/, "scullbuf%d", index);
```

The chain of events (fully traced in internals §12):

```text
class_create("scullbuf")
      └─ creates /sys/class/scullbuf/

device_create(..., devno, "scullbuf0")
      ├─ creates /sys/class/scullbuf/scullbuf0/
      ├─ writes the "dev" attribute = "MAJOR:MINOR"
      └─ emits a KOBJ_ADD uevent  ──netlink──▶  systemd-udevd
                                                     │
                                                     ▼
                                        reads "dev" attr, applies rules,
                                        calls mknod  →  /dev/scullbuf0
                  (or devtmpfs creates the node immediately in-kernel)
```

> **Critical 6.x detail:** Since kernel **6.4**, `class_create()` takes **one argument**
> (the name). Older code passed `class_create(THIS_MODULE, name)` — that **will not
> compile** on 6.17. This is the single most common "my old driver won't build" gotcha on
> modern kernels, so it is flagged again in the appendix.

Teardown is the **strict reverse**: `device_destroy(class, devno)` for each minor, then
`class_destroy(class)`.

---

## 9. Concurrency design: what each lock protects

Multiple processes can hold the same device open and call into it simultaneously on
different CPUs. Our invariants:

- **One `struct mutex` per device** guards *all* mutable fields of that `scullbuf_dev`
  (`buffer` contents, `end`, `quantum`, stats). Different minors never contend with each
  other because they have separate mutexes.
- We take the mutex with **`mutex_lock_interruptible(&dev->lock)`** so a blocked process
  can still be killed by a signal (returns `-ERESTARTSYS`).
- **We may sleep while holding this mutex** — both `copy_*_user` (page faults) and
  `wait_event_interruptible` can sleep. That is legal for a mutex. The wait-queue macros
  are even designed to *drop and re-acquire* around sleeping in our blocking design (§10).
- **Lock ordering:** there is only one lock per device and we never nest device locks, so
  there is no ordering hazard. If a future feature needed two devices' locks at once, we
  would establish a fixed order (e.g., by ascending minor) to prevent ABBA deadlock.

| Data                                      | Protected by                                           | Lock type rationale                                                   |
| ----------------------------------------- | ------------------------------------------------------ | --------------------------------------------------------------------- |
| `buffer[]`, `end`, `quantum`, stats | `dev->lock` (mutex)                                  | Critical section sleeps (user copy), so a sleeping lock is mandatory. |
| `fasync` list                           | handled internally by`fasync_helper`/`kill_fasync` | Library manages its own locking.                                      |
| module refcount                           | kernel (`try_module_get`)                            | Not our concern; the VFS does it via`cdev.owner`.                   |

> **Why not a spinlock or an atomic?** A spinlock would forbid the `copy_*_user` and
> `wait_event_*` calls inside the critical section. Atomics can't protect a multi-field
> invariant (buffer + `end` must change together). A per-device mutex is the right tool.

---

## 10. Blocking I/O design: wait queues

A read on an empty buffer must not return 0 forever or spin the CPU. It must **sleep**
until a writer provides data, then wake and proceed. We use a `wait_queue_head_t` per
direction. The read side, in design pseudo-code:

```c
ssize_t scullbuf_read(struct file *filp, char __user *ubuf,
                      size_t count, loff_t *f_pos)
{
        struct scullbuf_dev *dev = filp->private_data;

        if (mutex_lock_interruptible(&dev->lock))
                return -ERESTARTSYS;

        while (dev->end == 0) {                 /* nothing to read */
                mutex_unlock(&dev->lock);       /* don't sleep holding the lock */
                if (filp->f_flags & O_NONBLOCK)
                        return -EAGAIN;
                /* sleep until a writer makes end > 0, killable by signal */
                if (wait_event_interruptible(dev->readq, dev->end != 0))
                        return -ERESTARTSYS;    /* signal arrived */
                if (mutex_lock_interruptible(&dev->lock))
                        return -ERESTARTSYS;
        }

        /* data is present and lock is held: serve it */
        count = min(count, dev->end /* - offset */);
        if (copy_to_user(ubuf, dev->buffer /* + offset */, count)) {
                mutex_unlock(&dev->lock);
                return -EFAULT;
        }
        /* ...advance, update stats... */
        mutex_unlock(&dev->lock);

        wake_up_interruptible(&dev->writeq);    /* room may exist now */
        return count;
}
```

Design rules embodied here (the *why* is in internals §8):

- **Drop the lock before sleeping.** Sleeping with the device mutex held would block every
  other caller — including the writer who is supposed to wake us — a self-deadlock.
- **Re-check the condition after waking** (the `while`, not `if`). Wakeups can be spurious
  or another thread may have consumed the data first ("thundering herd"); `wait_event_*`
  already loops internally, but our outer re-lock/re-check keeps the invariant honest.
- **`O_NONBLOCK` returns `-EAGAIN`** instead of sleeping.
- **Interruptible sleep** returns `-ERESTARTSYS` on signal so the process stays killable.
- The writer mirrors this: after extending `end`, it calls
  `wake_up_interruptible(&dev->readq)`.

---

## 11. `poll`/`select`/`epoll` design

`poll` lets an application ask "is this fd readable/writable *without blocking*?" and wait
on many fds at once. Our `.poll` does two things: **register** our wait queues with the
poll machinery, then **report** current readiness.

```c
static __poll_t scullbuf_poll(struct file *filp, poll_table *wait)
{
        struct scullbuf_dev *dev = filp->private_data;
        __poll_t mask = 0;

        mutex_lock(&dev->lock);
        poll_wait(filp, &dev->readq, wait);     /* register read side  */
        poll_wait(filp, &dev->writeq, wait);    /* register write side */

        if (dev->end != 0)
                mask |= EPOLLIN  | EPOLLRDNORM;  /* readable            */
        if (dev->end != dev->buffersize)
                mask |= EPOLLOUT | EPOLLWRNORM;  /* writable            */
        mutex_unlock(&dev->lock);

        return mask;
}
```

- `poll_wait()` does **not** sleep. It records `dev->readq`/`writeq` into the caller's
  poll table so the generic `select`/`poll`/`epoll` core can sleep on them and be woken by
  the **same** `wake_up_interruptible` calls our read/write paths already issue.
- We return a bitmask of `__poll_t` flags: `EPOLLIN|EPOLLRDNORM` when data is available,
  `EPOLLOUT|EPOLLWRNORM` when space remains.
- Because readiness changes are signalled by the existing wakeups, **blocking I/O and poll
  share the same wait queues** — no extra plumbing. Internals §9 shows how `do_poll` and
  `ep_poll_callback` consume this.

---

## 12. `ioctl` design: control operations

`read`/`write` move data; `ioctl` does everything else (configuration, reset, queries).
Each command is an integer built by the `_IOC` macros so it self-describes its
**direction**, an owning **magic byte**, a **number**, and the **size** of its argument.

```c
#define SCULLBUF_IOC_MAGIC  'K'      /* pick a byte not used by others */

#define SCULLBUF_IOCRESET     _IO(SCULLBUF_IOC_MAGIC, 0)            /* no arg     */
#define SCULLBUF_IOCSQUANTUM  _IOW(SCULLBUF_IOC_MAGIC, 1, int)     /* set: W     */
#define SCULLBUF_IOCGQUANTUM  _IOR(SCULLBUF_IOC_MAGIC, 2, int)     /* get: R     */
#define SCULLBUF_IOC_MAXNR    2
```

Handler design:

```c
static long scullbuf_ioctl(struct file *filp, unsigned int cmd, unsigned long arg)
{
        struct scullbuf_dev *dev = filp->private_data;
        int tmp, ret = 0;

        if (_IOC_TYPE(cmd) != SCULLBUF_IOC_MAGIC) return -ENOTTY;
        if (_IOC_NR(cmd)   >  SCULLBUF_IOC_MAXNR) return -ENOTTY;

        switch (cmd) {
        case SCULLBUF_IOCRESET:
                mutex_lock(&dev->lock); dev->end = 0; mutex_unlock(&dev->lock);
                break;
        case SCULLBUF_IOCSQUANTUM:
                if (copy_from_user(&tmp, (int __user *)arg, sizeof(tmp)))
                        return -EFAULT;
                dev->quantum = tmp;
                break;
        case SCULLBUF_IOCGQUANTUM:
                tmp = dev->quantum;
                if (copy_to_user((int __user *)arg, &tmp, sizeof(tmp)))
                        return -EFAULT;
                break;
        default:
                return -ENOTTY;          /* "inappropriate ioctl for device" */
        }
        return ret;
}
```

Design rules:

- **Validate the magic and number first**, returning `-ENOTTY` for anything not ours —
  this is what makes `ioctl` on the wrong device fail cleanly rather than corrupting state.
- **`arg` is opaque.** For pointer commands we *must* `copy_from_user`/`copy_to_user`; we
  never dereference `arg` directly. The `_IOC_DIR` bits tell us which way data flows.
- The encoding tables and the bit layout of `_IOC` are in the appendix and internals §10.

---

## 13. `mmap` design: exposing the buffer directly

`mmap` lets a process map the device's kernel buffer into its own address space and then
access it with plain loads/stores — no `read`/`write` syscalls per access. The design
hinges on **how the buffer was allocated**, because that dictates how to build the page
tables:

| Buffer allocation                       | How pages are laid out    | mmap technique                                                               |
| --------------------------------------- | ------------------------- | ---------------------------------------------------------------------------- |
| `kmalloc` (physically contiguous)     | one run of physical pages | `remap_pfn_range()` over the buffer's PFN range                            |
| `vmalloc` (virtually contiguous only) | scattered physical pages  | a`vm_ops->fault` handler that resolves each page via `vmalloc_to_page()` |

For a `kmalloc`-backed buffer:

```c
static int scullbuf_mmap(struct file *filp, struct vm_area_struct *vma)
{
        struct scullbuf_dev *dev = filp->private_data;
        unsigned long pfn  = virt_to_phys(dev->buffer) >> PAGE_SHIFT;
        unsigned long size = vma->vm_end - vma->vm_start;

        if (size > PAGE_ALIGN(dev->buffersize))
                return -EINVAL;

        return remap_pfn_range(vma, vma->vm_start, pfn + vma->vm_pgoff,
                               size, vma->vm_page_prot);
}
```

Design rules:

- **Bound the mapping** to the buffer size — never let userspace map past the allocation
  (that would expose arbitrary kernel memory: a serious security bug).
- For DMA-able or device memory you would also set `VM_IO | VM_PFNMAP` and an appropriate
  `pgprot` (e.g., uncached). Internals §11 walks the page-table mechanics and the
  `kmalloc`-vs-`vmalloc` consequence in full.

> **Modern note (6.13+):** the kernel introduced `mmap_prepare` in `file_operations`
> (verified present in 6.17) as a safer, split-phase mmap setup path. The classic `.mmap`
> still works and is what we use; the appendix notes the direction of travel.

---

## 14. `procfs` and `sysfs` design

Two different "show me info" surfaces, by different mechanisms:

**`/proc/scullbuf` — a read-only stats file via `seq_file`:**

```c
static int scullbuf_proc_show(struct seq_file *m, void *v)
{
        int i;
        for (i = 0; i < scullbuf_nr_devs; i++) {
                struct scullbuf_dev *d = &scullbuf_devices[i];
                mutex_lock(&d->lock);
                seq_printf(m, "scullbuf%d: size=%zu used=%zu reads=%lu writes=%lu\n",
                           i, d->buffersize, d->end, d->bytes_read, d->bytes_written);
                mutex_unlock(&d->lock);
        }
        return 0;
}
/* registered with proc_create("scullbuf", 0, NULL, &scullbuf_proc_ops); */
```

> **Critical 5.6+ detail:** `/proc` files now register a **`struct proc_ops`**, not a
> `struct file_operations`. Verified present on 6.17. Old code using `file_operations`
> for `proc_create` won't compile. We pair `proc_create` with a `seq_file` to safely
> handle arbitrary output sizes and paging.

**A sysfs attribute via `DEVICE_ATTR`** (attached to the `struct device` from
`device_create`), exposing/tuning `quantum`:

```c
static ssize_t quantum_show(struct device *d, struct device_attribute *a, char *buf)
{ /* return scnprintf(buf, PAGE_SIZE, "%u\n", dev->quantum); */ }
static ssize_t quantum_store(struct device *d, struct device_attribute *a,
                             const char *buf, size_t n)
{ /* kstrtouint(buf, ...); set dev->quantum; return n; */ }
static DEVICE_ATTR_RW(quantum);
```

- **`/proc`** is the right place for free-form, human-readable, multi-value status.
- **`sysfs`** is the right place for the "one value per file" model and integrates with
  the device's `/sys/class/scullbuf/scullbufN/` directory automatically.

---

## 15. Init / exit and cleanup ordering

Initialization acquires resources in an order; teardown **must release them in the exact
reverse order**, and partial-failure paths must unwind only what succeeded. We use the
classic `goto` ladder:

```c
static int __init scullbuf_init(void)
{
        int i, ret;

        ret = alloc_chrdev_region(&scullbuf_devno_base, 0,
                                  scullbuf_nr_devs, "scullbuf");
        if (ret) goto fail_region;

        scullbuf_class = class_create("scullbuf");
        if (IS_ERR(scullbuf_class)) { ret = PTR_ERR(scullbuf_class); goto fail_class; }

        scullbuf_devices = kcalloc(scullbuf_nr_devs,
                                   sizeof(*scullbuf_devices), GFP_KERNEL);
        if (!scullbuf_devices) { ret = -ENOMEM; goto fail_alloc; }

        for (i = 0; i < scullbuf_nr_devs; i++) {
                struct scullbuf_dev *d = &scullbuf_devices[i];
                mutex_init(&d->lock);
                init_waitqueue_head(&d->readq);
                init_waitqueue_head(&d->writeq);
                d->buffer = kzalloc(SCULLBUF_SIZE, GFP_KERNEL);
                if (!d->buffer) { ret = -ENOMEM; goto fail_devs; }
                d->buffersize = SCULLBUF_SIZE;

                scullbuf_setup_cdev(d, i);          /* cdev_init + cdev_add  */
                d->device = device_create(scullbuf_class, NULL,
                                          MKDEV(MAJOR(scullbuf_devno_base), i),
                                          NULL, "scullbuf%d", i);
        }
        scullbuf_proc = proc_create("scullbuf", 0, NULL, &scullbuf_proc_ops);
        return 0;

fail_devs:
        /* unwind the devices created so far, reverse order */
        while (--i >= 0) { /* device_destroy, cdev_del, kfree(buffer) */ }
        kfree(scullbuf_devices);
fail_alloc:
        class_destroy(scullbuf_class);
fail_class:
        unregister_chrdev_region(scullbuf_devno_base, scullbuf_nr_devs);
fail_region:
        return ret;
}

static void __exit scullbuf_exit(void)
{
        int i;
        if (scullbuf_proc) proc_remove(scullbuf_proc);
        for (i = 0; i < scullbuf_nr_devs; i++) {
                struct scullbuf_dev *d = &scullbuf_devices[i];
                device_destroy(scullbuf_class, MKDEV(MAJOR(scullbuf_devno_base), i));
                cdev_del(&d->cdev);
                kfree(d->buffer);
        }
        kfree(scullbuf_devices);
        class_destroy(scullbuf_class);
        unregister_chrdev_region(scullbuf_devno_base, scullbuf_nr_devs);
}
module_init(scullbuf_init);
module_exit(scullbuf_exit);
MODULE_LICENSE("GPL");
```

Design rules:

- **Reverse-order teardown:** node → cdev → buffer → array → class → region. Mirror of init.
- **`goto` unwinding:** each label undoes exactly one acquisition; on failure you jump to
  the label for "the last thing that succeeded," so you never free what you didn't alloc.
- **`cdev_add` ordering hazard:** the device must be fully usable *before* `cdev_add`. In
  the loop, `device_create` (which can trigger udev → a near-instant `open`) comes *after*
  `cdev_add`, which is correct because the device is already serviceable by then.
- `__init` / `__exit` place these functions in discardable sections (internals §13).

---

### Where to go next

The design above repeatedly says "*the kernel does X for us.*" Document
[02-internals.md](02-internals.md) removes every such hand-wave and shows the actual
kernel functions, structs, and call paths that implement X — from the syscall entry down
to the page tables.
