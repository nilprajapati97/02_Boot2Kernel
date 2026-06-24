# Part 2 — How It Works Internally, From Scratch

> This is the deep document. We start from the most basic premise ("everything is a file")
> and descend, layer by layer, all the way to page-table entries and CPU exception tables,
> naming the actual kernel functions and structs at every step. Pinned to **6.17**.

**Map of the descent**

1. "Everything is a file" and the VFS objects
2. Device nodes: what a `/dev` entry actually is
3. The character subsystem internals (`cdev_map`)
4. The `open()` path, end to end
5. The `read()`/`write()` path, end to end
6. `copy_to_user`/`copy_from_user` and exception tables
7. `f_pos` and `llseek` internals
8. Blocking I/O: wait queues, the hard way
9. `poll`/`select`/`epoll` internals
10. `ioctl` internals and `_IOC` decoding
11. `mmap` internals: VMAs and page tables
12. The device model, sysfs, uevents, and udev
13. Module lifecycle and refcounting
14. Memory: `kmalloc` vs `vmalloc` vs `kzalloc`
15. The whole life of the device, layer by layer

---

## 1. "Everything is a file" and the VFS objects

The Unix design principle is that most kernel objects — regular files, directories, pipes,
sockets, and **devices** — are reached through the same `open`/`read`/`write`/`close`
interface. The kernel layer that provides this uniform interface is the **Virtual File
System (VFS)**. Understanding four VFS objects is the prerequisite for everything else.

```text
   process                      kernel
   ───────                      ─────────────────────────────────────────────
   int fd  ───────────▶  struct files_struct (per-process fd table)
                              └─ fd_array[fd] ─▶ struct file   (an OPEN file)
                                                   ├─ f_op   ─▶ struct file_operations
                                                   ├─ f_pos    (cursor)
                                                   ├─ f_flags  (O_NONBLOCK, ...)
                                                   ├─ f_mode   (FMODE_READ/WRITE)
                                                   ├─ private_data (ours)
                                                   └─ f_inode ─▶ struct inode (the FILE ITSELF)
                                                                    ├─ i_rdev   (dev_t, if device)
                                                                    ├─ i_cdev   (struct cdev *)
                                                                    ├─ i_mode   (S_IFCHR | perms)
                                                                    └─ i_fop    (default fops)
```

- **`struct inode`** — represents *the file object itself* (metadata: type, permissions,
  owner; for a device, the `dev_t` in `i_rdev`). There is **one inode per file**, no matter
  how many times it is open. Defined in `include/linux/fs.h`.
- **`struct file`** (often called `filp`, "file pointer") — represents **one open
  instance**. `open()` the same node twice → two `struct file`, one shared `inode`. It
  holds the per-open cursor `f_pos`, the open flags `f_flags`, the mode `f_mode`, the
  operations vector `f_op`, and our `private_data`.
- **`struct dentry`** — a "directory entry," the cache that maps a *name* in a directory
  to an inode. Path lookup (`/dev/scullbuf0`) walks dentries. Not central to our driver but
  it is how the inode is found.
- **`struct files_struct` / `struct fdtable`** — the per-process array that turns the small
  integer `fd` into a `struct file *`. `fd` is just an index.

The single most important relationship for a driver writer:

> **`fd` → `struct file` (per open) → `f_op` (your `file_operations`) → your function.**
> The cursor and flags live in `struct file`; the device identity lives in `struct inode`.

This is why our `open()` caches the device pointer into `filp->private_data`: it attaches
per-open state to the *open instance*, reachable by every later call on that `fd`.

---

## 2. Device nodes: what a `/dev` entry actually is

A device node is a special inode. When you run `mknod /dev/scullbuf0 c 511 0`, the
filesystem creates an inode whose type is **`S_IFCHR`** (character special) and whose
**`i_rdev`** field stores `MKDEV(511, 0)`. It occupies no data blocks — the `(major,minor)`
*is* the content.

Crucially, the **file's own `i_fop` is not your driver's fops yet**. When such an inode is
instantiated, the VFS calls `init_special_inode()` (`fs/inode.c`):

```c
void init_special_inode(struct inode *inode, umode_t mode, dev_t rdev)
{
        inode->i_mode = mode;
        if (S_ISCHR(mode)) {
                inode->i_fop = &def_chr_fops;   /* the char "bootstrap" fops */
                inode->i_rdev = rdev;           /* remember (major,minor)   */
        } else if (S_ISBLK(mode)) {
                ...
        }
}
```

So a freshly looked-up char node points at **`def_chr_fops`** (`fs/char_dev.c`), a tiny
generic table whose only real member is `.open = chrdev_open`. Your driver is *not* yet in
the picture. The hand-off from `def_chr_fops` to your `scullbuf_fops` happens **during
`open()`** — that is the pivot the whole subsystem turns on, detailed in §4.

> Today most systems use **devtmpfs** mounted at `/dev`, so the kernel itself
> auto-populates these special inodes when drivers register (no manual `mknod`). The inode
> semantics above are identical; only *who* created it differs (§12).

---

## 3. The character subsystem internals: `cdev_map`

How does the kernel get from "`i_rdev` = `MKDEV(511,0)`" to "the `struct cdev` that owns
that number"? Through a dedicated lookup structure in `fs/char_dev.c`.

There are two registries:

1. **`chrdevs[]`** — a 255-bucket hash of `struct char_device_struct`, used by the
   **legacy** `register_chrdev()` path to record name/major ownership ranges. This is
   bookkeeping for "who owns major N."
2. **`cdev_map`** — a `struct kobj_map *`. This is the real runtime lookup that maps a
   `dev_t` *range* to a `struct cdev *`. It is what `cdev_add()` populates and what
   `chrdev_open()` queries.

`kobj_map` (in `drivers/base/map.c`) is a small hash of 255 buckets of "probe" records,
each describing `{ dev_t base, range, data=struct cdev*, get/lock callbacks }`:

```c
/* drivers/base/map.c, simplified */
struct kobj_map {
        struct probe {
                struct probe   *next;
                dev_t           dev;     /* first dev of this range  */
                unsigned long   range;   /* how many minors          */
                struct module  *owner;
                kobj_probe_t   *get;     /* returns the kobject      */
                int           (*lock)(dev_t, void *);
                void           *data;    /* the struct cdev *        */
        } *probes[255];
        struct mutex *lock;
};
```

`cdev_add()` ultimately calls `kobj_map(cdev_map, dev, count, ..., exact_match, exact_lock,
cdev)` to insert our `cdev` under its `(major, first-minor .. +count)` range.

`chrdev_open()` later calls `kobj_lookup(cdev_map, inode->i_rdev, &index)`, which hashes
the major, walks the bucket's probe list, finds the range containing `i_rdev`, runs the
`get` callback (`exact_match`) to return the `cdev`'s kobject, and `exact_lock` does a
`cdev_get()` (refcount + `try_module_get(owner)`). The result is recovered with
`container_of(kobj, struct cdev, kobj)`.

> **Mental model:** `cdev_map` is a *range map* keyed by `dev_t`. `cdev_add` writes a
> range; `open` reads it back. The 255 buckets are hashed on the **major** number.

---

## 4. The `open()` path, end to end

Now the pivot. Tracing `open("/dev/scullbuf0", O_RDWR)` from the syscall to your code:

```text
user: open() / openat()
  └─ syscall ─▶ sys_openat ─▶ do_sys_openat2()            (fs/open.c)
       └─ do_filp_open()                                   (fs/namei.c)
            └─ path_openat()
                 ├─ link_path_walk()      resolve "/dev/scullbuf0" via dentries
                 └─ do_open() ─▶ vfs_open() ─▶ do_dentry_open()   (fs/open.c)
                      ├─ f->f_op = fops_get(inode->i_fop)   // == &def_chr_fops
                      └─ if (f->f_op->open) f->f_op->open(inode, f)
                            └─ chrdev_open(inode, filp)     (fs/char_dev.c)
```

`do_dentry_open()` first sets `f->f_op` from `inode->i_fop`, which for our char node is
**`def_chr_fops`**. It then calls `def_chr_fops.open`, i.e. **`chrdev_open()`**. Here is
the heart of it (`fs/char_dev.c`, condensed and annotated):

```c
static int chrdev_open(struct inode *inode, struct file *filp)
{
        const struct file_operations *fops;
        struct cdev *p;
        struct cdev *new = NULL;
        int ret = 0;

        spin_lock(&cdev_lock);
        p = inode->i_cdev;                 /* cached from a previous open? */
        if (!p) {
                struct kobject *kobj;
                int idx;
                spin_unlock(&cdev_lock);
                /* (1) LOOK UP the cdev by device number */
                kobj = kobj_lookup(cdev_map, inode->i_rdev, &idx);
                if (!kobj)
                        return -ENXIO;     /* no driver owns this number   */
                new = container_of(kobj, struct cdev, kobj);
                spin_lock(&cdev_lock);
                p = inode->i_cdev;
                if (!p) {
                        inode->i_cdev = p = new;   /* (2) CACHE on the inode */
                        list_add(&inode->i_devices, &p->list);
                        new = NULL;
                } else if (!cdev_get(p))
                        ret = -ENXIO;
        } else if (!cdev_get(p))           /* (3) bump refcount + module    */
                ret = -ENXIO;
        spin_unlock(&cdev_lock);
        cdev_put(new);
        if (ret)
                return ret;

        ret = -ENXIO;
        fops = fops_get(p->ops);           /* (4) OUR scullbuf_fops         */
        if (!fops)
                goto out_cdev_put;

        replace_fops(filp, fops);          /* (5) filp->f_op = scullbuf_fops */
        if (filp->f_op->open) {
                ret = filp->f_op->open(inode, filp);   /* (6) scullbuf_open  */
                if (ret)
                        goto out_cdev_put;
        }
        return 0;
 out_cdev_put:
        cdev_put(p);
        return ret;
}
```

The six numbered moves are the entire magic:

1. **Look up** the `cdev` from `inode->i_rdev` via `kobj_lookup(cdev_map, ...)` (§3).
2. **Cache** it on `inode->i_cdev` and link the inode into `cdev->list`, so future opens of
   the same node skip the lookup. **This is the `i_cdev` our design's `container_of` relies
   on** (design §7).
3. **`cdev_get(p)`** bumps the cdev's kobject refcount **and** `try_module_get(cdev->owner)`
   — pinning our module while any fd is open (§13).
4. **`fops_get(p->ops)`** fetches *our* `scullbuf_fops` (and bumps the module ref again via
   the fops owner).
5. **`replace_fops(filp, fops)`** overwrites `filp->f_op`: from this instant the file's
   operations are **ours**, not `def_chr_fops`. Every later `read`/`write`/`ioctl` on this
   `fd` dispatches straight into `scullbuf_*`.
6. **Call `filp->f_op->open`** = **`scullbuf_open()`**, where we run `container_of(inode->
   i_cdev, ...)`, set `filp->private_data`, and do per-open setup.

> **The one-sentence summary of the entire char model:** *a device node bootstraps through
> `def_chr_fops.open = chrdev_open`, which looks the `cdev` up by `dev_t` in `cdev_map`,
> swaps `filp->f_op` to the driver's fops, and calls the driver's `open` — after which the
> file behaves exactly like the driver dictates.*

---

## 5. The `read()`/`write()` path, end to end

After open, `read(fd, buf, n)`:

```text
user: read()
  └─ syscall ─▶ ksys_read()                               (fs/read_write.c)
       ├─ fdget_pos(fd) ─▶ struct fd { struct file *file; } + take f_pos lock
       └─ vfs_read(file, buf, count, &file->f_pos)
            ├─ rw_verify_area()         // sanity: count, pos, limits, LSM hook
            └─ if (file->f_op->read)
                     return file->f_op->read(file, buf, count, pos);   // scullbuf_read
               else if (file->f_op->read_iter)
                     return new_sync_read(file, buf, count, pos);      // iterator wrap
```

Two dispatch shapes exist, and this matters for modern drivers:

- **Classic** `f_op->read(file, char __user *buf, size_t count, loff_t *pos)` — what
  `scullbuf` provides. The VFS calls it directly.
- **Iterator** `f_op->read_iter(struct kiocb *, struct iov_iter *)` — the modern form. If a
  driver provides only `read_iter`, a classic `read(2)` is serviced by **`new_sync_read()`**
  (`fs/read_write.c`), which builds a one-segment `struct iov_iter` and a synchronous
  `struct kiocb` on the stack and calls `read_iter`. `readv`/`preadv`/async io_uring use the
  iterator directly.

```c
/* fs/read_write.c — how a classic read() reaches a modern driver */
static ssize_t new_sync_read(struct file *filp, char __user *buf,
                             size_t len, loff_t *ppos)
{
        struct iovec    iov = { .iov_base = buf, .iov_len = len };
        struct kiocb    kiocb;
        struct iov_iter iter;
        ssize_t ret;

        init_sync_kiocb(&kiocb, filp);
        kiocb.ki_pos = (ppos ? *ppos : 0);
        iov_iter_init(&iter, ITER_DEST, &iov, 1, len);
        ret = filp->f_op->read_iter(&kiocb, &iter);
        if (ppos) *ppos = kiocb.ki_pos;
        return ret;
}
```

Either way, control reaches our function with three things: a **user pointer** (`buf`), a
**count**, and the **position** (`*pos` for classic; `kiocb.ki_pos` for iter). The
`fdget_pos()` in `ksys_read` already took the **`f_pos` lock** so concurrent reads on the
*same fd* don't corrupt the cursor (§7).

The driver cannot simply `memcpy(buf, dev->buffer, count)` — `buf` is a **user-space
virtual address** that is meaningless or dangerous to dereference directly from the kernel.
That is what §6 is about.

---

## 6. `copy_to_user`/`copy_from_user` and exception tables

User space and kernel space are **separate virtual address mappings** enforced by the MMU
and the page-table privilege (supervisor) bit. A user pointer `buf`:

- may be **unmapped** (the page isn't present — touching it must trigger demand paging, not
  an oops),
- may be **invalid/malicious** (pointing at kernel memory — must be rejected),
- may **fault** mid-copy (must be handled, returning a short count, never crashing).

So the kernel never dereferences user pointers directly. It uses **`copy_to_user()`** and
**`copy_from_user()`**, which do three things:

### (a) Range-check with `access_ok()`

`access_ok(ptr, len)` verifies the `[ptr, ptr+len)` range lies within the user portion of
the address space (below the user/kernel split). On 6.17 it lives in
`include/asm-generic/access_ok.h` (pulled in by `arch/x86/include/asm/uaccess.h`). If the
range is bogus, the copy returns "all bytes not copied" and the caller returns `-EFAULT`.

### (b) The historic `set_fs()` is gone

Older kernels used a per-thread `addr_limit` toggled by `set_fs(KERNEL_DS)`/`get_fs()` to
let kernel code reuse user-copy routines on kernel addresses. That mechanism was a security
footgun and was **removed**. On modern x86-64 there is **no `set_fs`/`get_fs`** (verified:
no matches in 6.17 x86 headers). User and kernel copies now use distinct, explicit
helpers. This is a favorite interview question: *"Why was `set_fs` removed?"* → it allowed
bugs/exploits to flip the address-limit and make the kernel treat kernel pointers as user
(or vice-versa), defeating `access_ok`; eliminating it closes that class entirely.

### (c) Fault-tolerant copy via the **exception table**

The actual byte movement (`raw_copy_to_user` → `copy_user_generic`) issues ordinary `mov`
instructions, but each such instruction is registered in a **kernel exception table**. The
assembly wraps the access with an `_ASM_EXTABLE(faulting_insn, fixup_label)` entry:

```text
   .text:   1:  mov  %rax, (%rdi)     ; store into the user address
            ...
   __ex_table:  .quad 1b, fixup       ; "if insn at 1b faults, jump to fixup"
```

If the user page is not present, the CPU raises a **page fault**. The page-fault handler
(`do_user_addr_fault` → `fixup_exception`, `arch/x86/mm/extable.c`) consults the exception
table. If the faulting RIP is listed, it either:

- lets demand paging bring the page in and **retries**, or
- if the address is truly bad, **diverts execution to the fixup label**, which makes the
  copy routine return the number of bytes that could *not* be copied.

That nonzero remainder is what turns into `-EFAULT` in our driver. This is why
`copy_*_user` **return a count, not an error code** — they can copy *part* of the buffer
before a fault.

```c
/* contract */
unsigned long copy_to_user(void __user *to, const void *from, unsigned long n);
/* returns the number of bytes that could NOT be copied (0 == full success). */
```

And this is why these functions **may sleep** (`might_fault()` / `might_sleep()`): servicing
a page fault can block on I/O. Hence our design uses a **mutex**, not a spinlock, around
them (design §9), and we **drop the lock before sleeping** in the blocking path (§8).

> **Putting §5 and §6 together:** the driver receives a user pointer it must not trust;
> `copy_to_user` validates it, attempts the copy under exception-table protection, and
> reports a remainder. Zero remainder → success; nonzero → `-EFAULT`.

---

## 7. `f_pos` and `llseek` internals

Each open file has a cursor, `file->f_pos` (a `loff_t`, 64-bit signed). The VFS — not the
driver — owns this field, but the driver advances it through the `*ppos` argument.

- On `read`/`write`, `ksys_read`/`ksys_write` pass **`&file->f_pos`** as the `pos`
  argument. Our function reads from `*pos` and writes back the new position. The VFS took
  `file->f_pos_lock` in `fdget_pos()` so two threads sharing the *same fd* serialize their
  cursor updates (threads with *separate* fds have separate `struct file`, separate
  cursors).
- `lseek(fd, off, whence)` → `ksys_lseek` → `vfs_llseek` → `file->f_op->llseek`. If a
  driver leaves `.llseek` NULL, behavior depends on flags; many drivers use the generic
  helpers:
  - **`default_llseek`** / **`generic_file_llseek`** implement `SEEK_SET` (absolute),
    `SEEK_CUR` (relative), `SEEK_END` (from end), updating `file->f_pos`.
  - **`no_llseek`/`nonseekable_open`** mark a stream unseekable (e.g., a pure FIFO).

Our `scullbuf_llseek` interprets `whence` against `buffersize` and assigns
`filp->f_pos`, returning the new offset. Because our device is RAM with a meaningful
position, seeking is well-defined (unlike a serial port).

```c
static loff_t scullbuf_llseek(struct file *filp, loff_t off, int whence)
{
        struct scullbuf_dev *dev = filp->private_data;
        loff_t newpos;
        switch (whence) {
        case SEEK_SET: newpos = off; break;
        case SEEK_CUR: newpos = filp->f_pos + off; break;
        case SEEK_END: newpos = dev->buffersize + off; break;
        default:       return -EINVAL;
        }
        if (newpos < 0 || newpos > dev->buffersize) return -EINVAL;
        filp->f_pos = newpos;
        return newpos;
}
```

---

## 8. Blocking I/O: wait queues, the hard way

This is the mechanism that lets a process *sleep* until a condition becomes true without
burning CPU. The naive version and why each piece exists:

A **wait queue** is a list of sleeping tasks plus a lock: `wait_queue_head_t`. To wait
*correctly* you must avoid the **lost-wakeup race**: if you (1) check the condition, (2) it
is false, (3) you decide to sleep — but a writer changes the condition and fires the wakeup
*between* steps (2) and (3) — you would sleep forever. The kernel solves this by changing
your task state to sleeping **before** the final condition re-check, so a concurrent wakeup
either finds you already queued or you observe the new condition and don't sleep.

The manual idiom (what the macro expands to):

```c
DEFINE_WAIT(wait);                                   /* a wait_queue_entry */
add_wait_queue(&dev->readq, &wait);
for (;;) {
        set_current_state(TASK_INTERRUPTIBLE);       /* (A) arm sleep FIRST */
        if (dev->end != 0)                           /* (B) THEN re-check   */
                break;                               /* condition true: go  */
        if (signal_pending(current)) {               /* killable            */
                ret = -ERESTARTSYS;
                break;
        }
        schedule();                                  /* (C) actually sleep  */
}
set_current_state(TASK_RUNNING);
remove_wait_queue(&dev->readq, &wait);
```

The ordering **(A) arm → (B) check → (C) schedule** is the crux. Between arming the state
and calling `schedule()`, a wakeup that sets `TASK_RUNNING` simply prevents the upcoming
`schedule()` from sleeping (or returns immediately), so the wakeup is never lost. There is
an implicit **memory barrier** in `set_current_state()` pairing with the one in
`wake_up()`, ensuring the condition write by the writer is visible.

`wait_event_interruptible(wq, cond)` packages exactly this. Its verified 6.17 expansion:

```c
#define wait_event_interruptible(wq_head, condition)            \
({                                                              \
        int __ret = 0;                                          \
        might_sleep();                                          \
        if (!(condition))                                       \
                __ret = __wait_event_interruptible(wq_head, condition); \
        __ret;                                                  \
})
```

`__wait_event_interruptible` expands (via `___wait_event`) into the `for(;;)` loop above
with `TASK_INTERRUPTIBLE`, `prepare_to_wait`/`finish_wait`, the `signal_pending` check, and
`schedule()`. Return value: `0` if the condition became true, `-ERESTARTSYS` if a signal
interrupted the sleep. That `-ERESTARTSYS` is special: on return to user space the kernel
either restarts the syscall transparently or delivers `-EINTR`, which is what keeps a
blocked `read` **killable** (Ctrl-C works).

The wake side:

```c
wake_up_interruptible(&dev->readq);
/* expands to __wake_up(&dev->readq, TASK_INTERRUPTIBLE, 1, NULL):
   walk the queue, for each entry call its wake function (default_wake_function
   -> try_to_wake_up), set those tasks TASK_RUNNING, enqueue on a runqueue. */
```

Why our design (§10) **drops the device mutex before `wait_event_interruptible`**: the
writer needs that same mutex to change `dev->end` and then call `wake_up`. If a reader slept
holding it, the writer could never run — classic self-deadlock. So: unlock → wait → on wake,
re-lock → re-verify → proceed. The `while` loop around the wait handles the **thundering
herd**: if `wake_up` wakes several readers but only one chunk of data exists, the losers
re-check, find `end == 0` again, and go back to sleep.

> **Interview gold:** "Walk me through the lost-wakeup race and how `wait_event` avoids it."
> → state-before-check ordering + the barrier in `set_current_state`/`wake_up`. "Why
> interruptible?" → so the task counts as sleeping-but-killable (`TASK_INTERRUPTIBLE`),
> not an unkillable `TASK_UNINTERRUPTIBLE` 'D' state.

---

## 9. `poll`/`select`/`epoll` internals

`poll` answers "which of these fds are ready, and block until at least one is." The driver
side is tiny (design §11); the interesting part is how the generic core uses it.

### The poll table

`poll(2)`/`select(2)` land in `do_sys_poll`/`core_sys_select` → `do_poll`/`do_select`
(`fs/select.c`). The core builds a **`struct poll_wqueues`** containing a
**`poll_table`** (a.k.a. `struct poll_table_struct`) whose `_qproc` is set to
`__pollwait`. It then calls each fd's `f_op->poll(file, &pt)`.

Inside our `scullbuf_poll`, **`poll_wait(filp, &dev->readq, pt)`** runs (verified 6.17
`include/linux/poll.h`):

```c
static inline void poll_wait(struct file *filp, wait_queue_head_t *wait_address,
                             poll_table *p)
{
        if (p && p->_qproc) {
                p->_qproc(filp, wait_address, p);   /* == __pollwait */
                smp_mb();   /* pair with wq_has_sleeper() in the wake path */
        }
}
```

`__pollwait` allocates a `struct poll_table_entry`, links a wait-queue entry onto
`dev->readq` with the callback `pollwake`, so that a future `wake_up_interruptible(&dev->
readq)` from our `write` path **wakes the polling task**. Then our `poll` returns the
**current** readiness mask (`EPOLLIN`, etc.).

### The wait/recheck loop

```text
do_poll():
  for (;;) {
      mask = 0;
      for each fd:  mask |= fd->f_op->poll(file, &pt);   // also registers waitqueues
      if (mask matched what caller asked || timeout || signal) break;
      pt._qproc = NULL;                  // only REGISTER on the first pass
      poll_schedule_timeout(...);        // sleep until a wake fires or timeout
  }
```

The first pass both **registers** wait queues (via `poll_wait`) and **samples** readiness.
On later passes `_qproc` is NULL'd so `poll_wait` becomes a no-op (no double registration)
and it only re-samples. The sleep is ended by exactly the same `wake_up_interruptible` our
blocking I/O already issues — that is why one set of wait queues serves both `read()`
blocking and `poll()` (design §11).

### `__poll_t` and `EPOLLIN`

The mask type is `__poll_t`, a bitmask. Verified value:
`#define EPOLLIN (__force __poll_t)0x00000001` (`include/uapi/linux/eventpoll.h`). We OR
together `EPOLLIN|EPOLLRDNORM` for readable and `EPOLLOUT|EPOLLWRNORM` for writable.

### epoll

`epoll` is edge/level-triggered and scalable: instead of re-polling every fd each call, it
registers **once** with each fd's wait queue using the callback **`ep_poll_callback`**
(`fs/eventpoll.c`). When our `wake_up` fires, `ep_poll_callback` moves the epoll item onto a
ready list and wakes any `epoll_wait`. The driver does nothing special — implementing
`.poll` correctly (registering the right wait queues, reporting the right mask) makes the fd
work with `select`, `poll`, **and** `epoll` automatically.

---

## 10. `ioctl` internals and `_IOC` decoding

`ioctl(fd, cmd, arg)` → `sys_ioctl` → `do_vfs_ioctl()`/`vfs_ioctl()` (`fs/ioctl.c`).
`vfs_ioctl` handles a few generic commands (`FIONBIO`, `FIOCLEX`, ...) and otherwise calls
**`file->f_op->unlocked_ioctl(file, cmd, arg)`** — our handler. There is no global lock;
"unlocked" literally means the old BKL is gone and locking is the driver's job.

For a 32-bit process on a 64-bit kernel, `compat_ioctl` is called instead; pointer and
`long` widths differ, so commands carrying such types may need translation.
**`compat_ptr_ioctl`** is a generic shim that sign-extends/zero-extends the pointer in `arg`
and forwards to `unlocked_ioctl`, suitable when your structures are already layout-compatible.

### The `_IOC` encoding

A command number is **not** an arbitrary integer; it is a 32-bit word with four fields, so
that commands are globally (mostly) unique and self-describing
(`include/uapi/asm-generic/ioctl.h`):

```text
 bits: 31    30 29 ............ 16 15 .......... 8 7 ............ 0
      ┌────────────┬─────────────────┬─────────────┬──────────────┐
      │  dir (2)   │     size (14)    │   type (8)  │    nr (8)     │
      └────────────┴─────────────────┴─────────────┴──────────────┘
        _IOC_DIR     _IOC_SIZE          _IOC_TYPE      _IOC_NR
```

- **dir** — `_IOC_NONE`, `_IOC_WRITE`, `_IOC_READ`, or read|write (direction is from the
  *application's* viewpoint).
- **type** — a "magic" byte identifying the subsystem/driver (we chose `'K'`). Convention
  reduces cross-driver collisions; the registry is `Documentation/userspace-api/ioctl/`.
- **nr** — the per-driver command ordinal.
- **size** — `sizeof` the argument type, captured automatically by the macros.

The construction macros:

```c
_IO(type,nr)            /* no data        : dir = _IOC_NONE             */
_IOR(type,nr,datatype)  /* read  into user: dir = _IOC_READ,  size set  */
_IOW(type,nr,datatype)  /* write from user: dir = _IOC_WRITE, size set  */
_IOWR(type,nr,datatype) /* bidirectional                                 */
```

The decode macros let the handler validate before acting:

```c
_IOC_DIR(cmd)   _IOC_TYPE(cmd)   _IOC_NR(cmd)   _IOC_SIZE(cmd)
```

Our handler rejects foreign commands with **`-ENOTTY`** ("inappropriate ioctl for device")
by checking `_IOC_TYPE(cmd) != SCULLBUF_IOC_MAGIC`. For pointer arguments we copy with
`copy_from_user`/`copy_to_user`; we never trust `arg` as a kernel address. Some robust
drivers also pre-validate using `_IOC_DIR`/`_IOC_SIZE` + `access_ok` before the switch.

> **Why magic numbers?** Two drivers might both define "command 0." Without a type byte, a
> wrong-device `ioctl` could be silently *accepted* and scribble memory. The type byte makes
> a mismatched command fail with `-ENOTTY` instead.

---

## 11. `mmap` internals: VMAs and page tables

`mmap` makes the device buffer appear directly in the process's address space. The flow:

```text
user: mmap(addr, len, prot, flags, fd, offset)
  └─ sys_mmap_pgoff ─▶ ksys_mmap_pgoff ─▶ vm_mmap_pgoff ─▶ do_mmap()   (mm/mmap.c)
       └─ mmap_region()
            ├─ allocate & insert a struct vm_area_struct (VMA) into the mm
            └─ call_mmap() ─▶ file->f_op->mmap(file, vma)   // scullbuf_mmap
```

A **`struct vm_area_struct` (VMA)** describes one contiguous region of a process's virtual
address space: `[vm_start, vm_end)`, protection bits `vm_page_prot`, flags `vm_flags`, a
file offset `vm_pgoff`, and an operations vector `vm_ops`. At `f_op->mmap` time the VMA
exists but **no physical pages are mapped yet** — the driver's job is to arrange that the
right physical pages back this virtual range.

### Two techniques, dictated by the allocator (design §13)

**A. `remap_pfn_range` — eager, for physically contiguous memory (`kmalloc`/reserved/IO):**

```c
int remap_pfn_range(struct vm_area_struct *vma, unsigned long addr,
                    unsigned long pfn, unsigned long size, pgprot_t prot);
```

It walks the page tables for `[addr, addr+size)` and installs PTEs pointing at physical
frames `pfn, pfn+1, ...` in one shot. Because it needs a *contiguous* PFN run, it suits
`kmalloc`'d buffers (which are physically contiguous) and device/IO memory. For IO memory
you also set `vma->vm_flags |= VM_IO | VM_PFNMAP` (these pages aren't normal RAM with a
`struct page` to refcount) and choose a `pgprot` (e.g., `pgprot_noncached`).

**B. Fault handler — lazy, for virtually-contiguous memory (`vmalloc`):**

`vmalloc` memory is contiguous in *virtual* space but its physical pages are **scattered**,
so a single PFN run does not exist. Instead you set `vma->vm_ops` to a table with a
`.fault` callback and map pages on demand:

```c
static vm_fault_t scullbuf_vm_fault(struct vm_fault *vmf)
{
        struct scullbuf_dev *dev = vmf->vma->vm_private_data;
        unsigned long offset = vmf->pgoff << PAGE_SHIFT;
        struct page *page = vmalloc_to_page(dev->buffer + offset);
        get_page(page);                 /* refcount the page          */
        vmf->page = page;               /* hand it to the fault core  */
        return 0;
}
```

The first access to each page traps into the page-fault handler, which calls our `.fault`,
which returns the correct `struct page` (found via `vmalloc_to_page`), and the core installs
the PTE. Subsequent accesses hit the now-present mapping with no trap.

### Why the allocator choice ripples into mmap

This is the canonical "show you understand it" point: **`kmalloc` ⇒ `remap_pfn_range`;
`vmalloc` ⇒ `vm_ops->fault`.** The buffer-allocation decision in §14 is not cosmetic — it
determines which mmap implementation is even *possible*, because it determines whether a
contiguous PFN range exists.

> **6.17 direction:** `file_operations` now also has `mmap_prepare` (verified present), a
> split "prepare then populate" path that lets the core set up the VMA more safely before
> the driver populates it. Classic `.mmap` remains fully supported and is what we use.

---

## 12. The device model, sysfs, uevents, and udev

This is how `/dev/scullbuf0` appears automatically. The foundation is the **kobject**.

### kobjects, ksets, classes, devices

- **`struct kobject`** — the base "object" with a name, a reference count
  (`struct kref`), a parent pointer, and a `sysfs` directory. Every `/sys` directory *is* a
  kobject. `kobject_get`/`kobject_put` manage the refcount; the object is freed only when it
  hits zero (this is why `struct cdev` embeds a kobject — its lifetime is refcounted, §13).
- **`struct kset`** — a collection of kobjects of the same kind (a `/sys` subdirectory that
  also defines uevent behavior).
- **`struct class`** — a higher-level grouping of devices that do the same job (e.g., all
  `tty`s). `class_create("scullbuf")` makes `/sys/class/scullbuf/`. **(6.4+: single arg.)**
- **`struct device`** — one device in the model. `device_create(class, parent, devno,
  drvdata, "scullbuf%d", i)` allocates a `struct device`, names it, sets its `dev` (the
  `dev_t`), parents it under the class, registers it (creating
  `/sys/class/scullbuf/scullbuf0/`), and — crucially — creates the **`dev` attribute** file
  containing `"MAJOR:MINOR"`.

### The uevent → udev pipeline

Registering the device emits a **`KOBJ_ADD` uevent**. The chain:

```text
device_create()
  └─ device_add()
       ├─ creates /sys/class/scullbuf/scullbuf0/  (+ "dev", "uevent" attrs)
       └─ kobject_uevent(&dev->kobj, KOBJ_ADD)
            └─ broadcasts an env blob over a NETLINK_KOBJECT_UEVENT socket
                 (MAJOR=511 MINOR=0 DEVNAME=scullbuf0 SUBSYSTEM=scullbuf ACTION=add)
                      │
                      ▼
                systemd-udevd receives it, matches rules in /lib/udev/rules.d,
                reads the "dev" attribute, and calls mknod() → /dev/scullbuf0
```

Two cooperating mechanisms actually create the node:

- **devtmpfs** — a tiny in-kernel tmpfs mounted at `/dev`. When a device with a `dev_t`
  registers, the kernel itself creates the node *immediately* (so `/dev/scullbuf0` exists
  even before udev runs). `CONFIG_DEVTMPFS_MOUNT`.
- **udev** — the userspace daemon that then applies policy: permissions, ownership,
  symlinks, group (e.g., make it mode 0660 owned by group `scull`), based on rules. It can
  also create the node if devtmpfs is absent.

So `device_create` is the single call that wires sysfs + the `dev` attribute + the uevent;
devtmpfs/udev turn that into the actual `/dev` inode (whose `i_rdev`/`def_chr_fops`
semantics are exactly §2). Teardown via `device_destroy` emits `KOBJ_REMOVE`, and the node
disappears.

> **Why not just `mknod` in the driver?** Because the major is dynamic (design §4) and
> policy (perms/owner/symlinks) belongs in userspace. The model cleanly separates
> *mechanism* (kernel emits "a device named scullbuf0 with dev 511:0 appeared") from
> *policy* (udev decides the node's name/permissions).

---

## 13. Module lifecycle and refcounting

A loadable module can be inserted (`insmod`/`modprobe`) and removed (`rmmod`) at runtime.
Two questions matter: **what runs when**, and **why can't the module vanish while a device
is open?**

### init/exit and section discarding

- `module_init(scullbuf_init)` registers the function run at load; `module_exit(
  scullbuf_exit)` at unload.
- `__init` places `scullbuf_init` in the `.init.text` section, which the kernel **frees
  after initialization** (it is never needed again). `__initdata` does the same for data.
- `__exit` code is **dropped entirely** if the kernel is built without module-unload
  support (there would be no way to call it).
- `MODULE_LICENSE("GPL")` sets the license; a non-GPL string **taints** the kernel and
  hides GPL-only exported symbols from the module. `MODULE_AUTHOR`/`MODULE_DESCRIPTION` are
  metadata shown by `modinfo`.

### Refcounting: the unload safety net

The danger: if `rmmod` freed our code while a process had `/dev/scullbuf0` open, the next
`read` would jump into freed memory. Prevention is layered:

- **`fops.owner = THIS_MODULE`** and **`cdev.owner = THIS_MODULE`**. During `chrdev_open`
  (§4 step 3–4), `cdev_get` and `fops_get` call **`try_module_get(owner)`**, incrementing
  the module's reference count. `__fput` (last close) calls `fops_put`/`cdev_put` →
  `module_put`. While the count is nonzero, **`rmmod` fails with `EBUSY`** (or blocks),
  because `delete_module` refuses to remove a module whose refcount is not zero.
- **The cdev kobject** is independently refcounted; `chrdev_open` did `cdev_get` (kobject
  ref +1), `__fput` does `cdev_put`. The `struct cdev`'s backing isn't released while opens
  exist.

```text
insmod ─▶ try_module_get on each open ─▶ refcount > 0 ─▶ rmmod blocked
                                                   │
                          last close (__fput) ─────┘ ─▶ module_put ─▶ refcount 0 ─▶ rmmod ok
```

> **Interview point:** "How does the kernel stop you `rmmod`-ing a busy driver?" → the
> module reference count, bumped via `try_module_get(cdev->owner)` on open and dropped via
> `module_put` on last `fput`; `rmmod` of a nonzero-refcount module returns `EBUSY`. This
> is exactly why forgetting `.owner = THIS_MODULE` is a dangerous bug.

---

## 14. Memory: `kmalloc` vs `vmalloc` vs `kzalloc`

The buffer's allocator determines physical layout, which (§11) determines the mmap
strategy and (cache/DMA) performance.

| Allocator | Virtual | Physical | Typical size | Can sleep? | Notes |
|-----------|---------|----------|--------------|------------|-------|
| `kmalloc(n, flags)` | contiguous | **contiguous** | small (≤ a few pages typical) | depends on flags | slab allocator; fast; suitable for DMA; backs `remap_pfn_range` mmap |
| `kzalloc(n, flags)` | contiguous | contiguous | small | depends | `kmalloc` + zeroing |
| `vmalloc(n)` | contiguous | **scattered** | large | yes (`GFP_KERNEL`) | builds fresh page-table mappings; not DMA-friendly; mmap needs a `.fault` handler |

### GFP flags and context

The `flags` argument to `kmalloc` controls *how hard* and *in what context* the allocator
may work:

- **`GFP_KERNEL`** — normal kernel allocation; **may sleep** (can trigger reclaim/swap). Use
  in process context (e.g., our `read`/`write`/`open`), **never** in atomic context.
- **`GFP_ATOMIC`** — will **not** sleep; for interrupt handlers, or while holding a
  spinlock. Smaller pool, can fail more easily.
- `__GFP_ZERO` (what `kzalloc` adds), `GFP_DMA`/`GFP_DMA32` (address constraints), etc.

> **The rule that ties it together:** in interrupt context or under a spinlock you must use
> `GFP_ATOMIC`; in ordinary process context use `GFP_KERNEL`. Our `scullbuf` allocates the
> buffer in `init`/`open` (process context) → `GFP_KERNEL`/`kzalloc` is correct. If we ever
> allocated from a softirq or while holding a spinlock, we'd be forced to `GFP_ATOMIC`.

Choosing `kmalloc` for `scullbuf` keeps the buffer physically contiguous so the simple
`remap_pfn_range` mmap (§11) works. Choosing `vmalloc` would allow a much larger buffer at
the cost of the `.fault`-based mmap and no DMA.

---

## 15. The whole life of the device, layer by layer

Putting all fifteen mechanisms into one timeline:

```text
1. insmod scullbuf.ko
   └─ module loader maps .ko, runs scullbuf_init (__init section)

2. alloc_chrdev_region(&base, 0, 4, "scullbuf")
   └─ kernel reserves a free major + minors 0..3        [§4 design / numbers]

3. class_create("scullbuf")                              [§12]
   └─ /sys/class/scullbuf/ kobject appears

4. per minor i in 0..3:
   a. init mutex, wait queues, kzalloc buffer (GFP_KERNEL)   [§14]
   b. cdev_init(&dev.cdev, &scullbuf_fops); cdev.owner=THIS_MODULE
   c. cdev_add(&dev.cdev, MKDEV(major,i), 1)             [§3]
        └─ kobj_map(cdev_map, ...) inserts the dev_t range  ← DEVICE NOW LIVE
   d. device_create(class, NULL, MKDEV(major,i), NULL, "scullbuf%d", i)  [§12]
        ├─ /sys/class/scullbuf/scullbufN/ + "dev"=major:N
        └─ KOBJ_ADD uevent ─netlink─▶ udev ; devtmpfs makes /dev/scullbufN

5. application: fd = open("/dev/scullbuf0", O_RDWR)      [§1,§2,§4]
   └─ path walk → inode (S_IFCHR, i_rdev=major:0, i_fop=def_chr_fops)
   └─ do_dentry_open → def_chr_fops.open = chrdev_open
        ├─ kobj_lookup(cdev_map, i_rdev) → our struct cdev
        ├─ inode->i_cdev = cdev ; cdev_get → try_module_get(owner)   [§13]
        ├─ replace_fops(filp, scullbuf_fops)
        └─ scullbuf_open: container_of(i_cdev) → dev ; filp->private_data = dev

6. application: read(fd, buf, n)                          [§5,§6,§7,§8]
   └─ vfs_read → scullbuf_read
        ├─ mutex_lock_interruptible(&dev->lock)
        ├─ while (dev->end == 0): unlock; wait_event_interruptible(readq, end!=0); relock
        ├─ copy_to_user(buf, dev->buffer, k)   (access_ok + exception table)
        ├─ advance *f_pos ; stats
        ├─ mutex_unlock ; wake_up_interruptible(&dev->writeq)
        └─ return k

7. another application: write(fd, buf, m)                 [§5,§6,§8]
   └─ scullbuf_write: copy_from_user; extend end; wake_up_interruptible(readq)
        └─ the sleeping reader in step 6 wakes, re-checks end!=0, proceeds

8. application: poll/select/epoll on fd                   [§9]
   └─ scullbuf_poll: poll_wait(readq), poll_wait(writeq); return EPOLLIN/EPOLLOUT
        └─ later wake_up on those queues readies the poll/epoll waiter

9. application: ioctl(fd, SCULLBUF_IOCSQUANTUM, &v)       [§10]
   └─ vfs_ioctl → scullbuf_ioctl: check _IOC_TYPE; copy_from_user(&v); set quantum

10. application: p = mmap(... fd ...)                     [§11]
   └─ do_mmap → scullbuf_mmap: remap_pfn_range(vma, ..., pfn, size, prot)
        └─ user now reads/writes the kernel buffer with plain loads/stores

11. application: close(fd)
   └─ __fput (last ref) → scullbuf_release → cdev_put/fops_put → module_put   [§13]

12. cat /proc/scullbuf                                    [§14 design]
   └─ seq_file shows per-minor size/used/reads/writes

13. rmmod scullbuf
   └─ blocked with EBUSY if any fd still open (refcount>0)   [§13]
   └─ else scullbuf_exit: proc_remove; per minor device_destroy + cdev_del + kfree;
        class_destroy; unregister_chrdev_region    (reverse order of init)   [§15 design]
```

Every arrow above is a function you can name, a struct you can point to, and a reason it
exists. That is the whole character-driver model, from the syscall instruction down to the
page-table entry and back up to udev creating the node.

---

### Where to go next

[03-traces-and-diagrams.md](03-traces-and-diagrams.md) renders these paths as mermaid
sequence diagrams and gives function-by-function call-stack traces you can read top-to-bottom.
[04-appendix.md](04-appendix.md) is the reference: API signatures, `_IOC` tables, version
notes, pitfalls, and an interview Q&A self-check.
