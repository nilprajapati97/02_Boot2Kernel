Here are **20 advanced Linux Character Driver interview questions** commonly asked in **AMD, NVIDIA, Qualcomm, Intel, Samsung, Broadcom, and Linux Kernel teams**. These questions focus on internals rather than API memorization.

---

# 1. How does `open()` reach your character driver's `open()` callback?

### Expected Answer

```text
sys_open()
  → do_sys_open()
  → path_openat()
  → do_dentry_open()
  → chrdev_open()
  → file_operations->open()
```

Kernel uses:

```c
inode->i_cdev
```

to locate the registered `cdev` and then invokes the driver's callback.

---

# 2. What is the difference between `inode` and `file`?

### Expected Answer

`inode`

```text
Represents the device itself.
Shared by all opens.
```

Contains:

```c
inode->i_rdev
inode->i_cdev
```

`file`

```text
Represents one open instance.
Created per open().
```

Contains:

```c
file->private_data
f_flags
f_pos
```

---

# 3. Why is `file->private_data` used?

### Expected Answer

To store device-specific context.

Example:

```c
file->private_data = dev;
```

Later:

```c
dev = file->private_data;
```

avoids repeated lookups.

---

# 4. How does one driver support multiple device instances?

### Expected Answer

Using:

```text
Major Number
Multiple Minor Numbers
```

Example:

```text
240:0
240:1
240:2
240:3
```

Each minor maps to a separate device structure.

---

# 5. Explain `container_of()` internally.

### Expected Answer

Suppose:

```c
struct mydev {
    int id;
    struct cdev cdev;
};
```

Kernel provides:

```c
inode->i_cdev
```

Need:

```c
struct mydev *
```

Using:

```c
container_of(ptr, type, member)
```

Internally:

```c
(type *)((char *)ptr - offsetof(type, member))
```

---

# 6. Difference between static and dynamic major number allocation?

### Expected Answer

Static:

```c
register_chrdev_region()
```

Driver chooses major.

Dynamic:

```c
alloc_chrdev_region()
```

Kernel chooses available major.

Dynamic allocation is preferred.

---

# 7. What happens internally during `cdev_add()`?

### Expected Answer

Kernel inserts `cdev` into:

```text
Character Device Hash Table
```

mapping:

```text
major/minor
    →
cdev
    →
file_operations
```

Later used by `chrdev_open()`.

---

# 8. How does blocking read work?

### Expected Answer

Process sleeps on waitqueue.

```c
wait_event_interruptible()
```

Scheduler removes task from run queue.

When data arrives:

```c
wake_up_interruptible()
```

Task becomes runnable.

---

# 9. Difference between busy wait and waitqueue?

### Expected Answer

Busy wait:

```c
while (!data)
    ;
```

Consumes CPU.

Waitqueue:

```c
wait_event_interruptible()
```

Puts task to sleep.

Preferred solution.

---

# 10. Explain race condition between read and write.

### Expected Answer

Scenario:

```text
Writer updates buffer
Reader reads simultaneously
```

May get corrupted data.

Protection:

```c
mutex
spinlock
rwlock
```

depending on context.

---

# 11. Why can't mutex be used in interrupt context?

### Expected Answer

Mutex may sleep.

Interrupt handlers cannot sleep.

Therefore:

```c
spin_lock()
```

must be used.

---

# 12. Difference between spinlock and mutex?

| Spinlock                | Mutex              |
| ----------------------- | ------------------ |
| Busy waits              | Sleeps             |
| Interrupt safe          | Not interrupt safe |
| Small critical sections | Long sections      |
| Atomic context          | Process context    |

---

# 13. What happens if user passes invalid pointer to read/write?

### Expected Answer

Direct access causes:

```text
Kernel Oops
```

Use:

```c
copy_to_user()
copy_from_user()
```

which validate access.

---

# 14. Why should driver never dereference user pointer directly?

### Expected Answer

User memory:

```text
May not exist
May be swapped
May be invalid
```

Must use:

```c
access_ok()
copy_to_user()
copy_from_user()
```

---

# 15. How does `poll()` work internally?

### Expected Answer

Userspace:

```c
select()
poll()
epoll()
```

Driver:

```c
poll_wait()
```

registers waitqueue.

When event arrives:

```c
wake_up()
```

Kernel notifies userspace.

---

# 16. Difference between synchronous and asynchronous notification?

### Expected Answer

Synchronous:

```text
read()
blocks
```

Asynchronous:

```c
SIGIO
fasync
```

Kernel sends signal when data arrives.

---

# 17. Explain `mmap()` in character drivers.

### Expected Answer

Maps kernel/device memory directly into userspace.

Flow:

```text
mmap()
 → driver mmap()
 → remap_pfn_range()
```

avoids data copy.

Used in:

```text
GPU
DMA
Framebuffers
NIC
```

---

# 18. What is zero-copy in character drivers?

### Expected Answer

Traditional:

```text
Device
  ↓
Kernel Buffer
  ↓
User Buffer
```

Zero-copy:

```text
Device
  ↓
Mapped User Memory
```

Implemented via:

```c
mmap()
DMA
```

---

# 19. How does reference counting work for a character device?

### Expected Answer

Open:

```text
ref++
```

Release:

```text
ref--
```

Module cannot unload while references exist.

Kernel uses:

```c
try_module_get()
module_put()
```

---

# 20. Explain complete path of a character driver read.

### Expected Answer

```text
User read()

    ↓

glibc read()

    ↓

sys_read()

    ↓

ksys_read()

    ↓

vfs_read()

    ↓

file->f_op->read()

    ↓

Driver read()

    ↓

copy_to_user()

    ↓

Userspace buffer
```

---

# Very Tough Follow-up Questions (Often Asked in NVIDIA/Qualcomm)

### Q21

Why does `open()` get `inode` but `read()` doesn't?

**Answer:**
`inode` is needed only during open to identify the device instance. After that, the instance pointer is stored in `file->private_data`.

---

### Q22

Can two processes share the same `file` structure?

**Answer:**
Yes.

Example:

```c
fork();
```

Parent and child share the same open file description.

---

### Q23

When is `release()` called?

**Answer:**

Not on every `close()`.

Only when:

```text
file reference count reaches zero
```

---

### Q24

Can `read()` be called without `open()`?

**Answer:**

No.

VFS guarantees:

```text
open()
   →
read/write/ioctl
   →
release()
```

---

### Q25

Why is `struct cdev` embedded inside driver private structure?

**Answer:**

Allows:

```c
container_of(inode->i_cdev,
             struct mydev,
             cdev);
```

to recover device context efficiently.

These are the kinds of questions that move beyond basic driver coding and test whether you understand VFS, cdev internals, synchronization, wait queues, poll/select/epoll, mmap, DMA, and multi-instance driver architecture.


If you're targeting **AMD, NVIDIA, Qualcomm, Intel, Google, Meta Kernel, or Linux BSP teams**, the interviewer will often move beyond APIs and ask **"why" and "how internally"** questions.

Here are **30 more advanced character driver questions**.

---

# 1. What happens if two threads call `read()` simultaneously on the same file descriptor?

### Discussion Points

```c
fd = open("/dev/mydev", O_RDWR);

Thread1 -> read(fd)
Thread2 -> read(fd)
```

Questions:

* Is `file->f_pos` shared?
* Is locking required?
* Can data corruption occur?
* How does VFS protect it?

---

# 2. Why is `copy_to_user()` allowed to sleep?

### Expected Answer

Page fault may occur.

```text
copy_to_user()
    ↓
User page not present
    ↓
Page fault handler
    ↓
Page brought into memory
```

Therefore:

```text
copy_to_user()
can sleep
```

Cannot call while holding spinlocks.

---

# 3. Why does `copy_to_user()` sometimes return a non-zero value?

### Expected Answer

Returns:

```text
Number of bytes NOT copied
```

Example:

```c
ret = copy_to_user(buf, kbuf, 100);
```

returns:

```text
20
```

Means only 80 bytes copied.

---

# 4. What happens if a driver calls `copy_to_user()` inside an IRQ handler?

### Expected Answer

BUG.

Reason:

```text
copy_to_user()
can fault
can sleep
```

IRQ context cannot sleep.

---

# 5. Why does Linux maintain both `inode` and `cdev`?

### Expected Answer

`inode`

```text
VFS object
```

`cdev`

```text
Character-device specific object
```

VFS remains filesystem-independent.

---

# 6. How does kernel find `file_operations` from major/minor?

Internal chain:

```text
major/minor
      ↓
cdev_map
      ↓
struct cdev
      ↓
file_operations
```

---

# 7. What is inside `struct cdev`?

Important fields:

```c
struct cdev {
    struct kobject kobj;
    struct module *owner;
    const struct file_operations *ops;
};
```

Interviewer may ask:

```text
Why does cdev contain kobject?
```

Answer:

```text
Reference counting
sysfs integration
lifetime management
```

---

# 8. Explain lifetime issues in character drivers.

Scenario:

```text
Thread A:
    open()

Thread B:
    rmmod
```

Question:

```text
How does kernel prevent module unload?
```

Answer:

```c
.owner = THIS_MODULE
```

Module refcount increases automatically.

---

# 9. Why is `.owner = THIS_MODULE` important?

Without it:

```text
open()
   ↓
rmmod
   ↓
driver memory removed
   ↓
read()
   ↓
crash
```

---

# 10. What happens internally during `wait_event_interruptible()`?

### Internally

```text
prepare_to_wait()
      ↓
TASK_INTERRUPTIBLE
      ↓
schedule()
      ↓
context switch
```

Wakeup:

```text
wake_up()
      ↓
try_to_wake_up()
      ↓
TASK_RUNNING
```

---

# 11. Explain the difference between TASK_INTERRUPTIBLE and TASK_UNINTERRUPTIBLE.

### TASK_INTERRUPTIBLE

```text
Can wake via signal
```

Example:

```text
Ctrl+C
kill -9
```

### TASK_UNINTERRUPTIBLE

```text
Cannot wake via signal
```

Used for critical waits.

---

# 12. Why is TASK_UNINTERRUPTIBLE dangerous?

Process may appear stuck forever:

```text
D state
```

Classic interview question.

---

# 13. Why should driver avoid long spinlock hold times?

Because:

```text
CPU spins
interrupt latency increases
scheduler delayed
```

In SMP systems:

```text
other CPUs stall
```

---

# 14. Explain lock ordering deadlock.

Example:

```text
Thread A
Lock1
Lock2

Thread B
Lock2
Lock1
```

Result:

```text
deadlock
```

---

# 15. How would you debug deadlocks in a character driver?

Tools:

```text
lockdep
sysrq-w
ftrace
crash utility
```

---

# 16. Difference between mutex and semaphore?

### Mutex

```text
single owner
priority inheritance
```

### Semaphore

```text
counting resource
multiple holders
```

Modern drivers mostly use mutex.

---

# 17. Why is `down()` deprecated in many new drivers?

Because:

```text
mutex gives ownership tracking
lockdep support
priority inheritance
```

---

# 18. What is a sleeping lock?

Example:

```c
mutex_lock()
```

Task sleeps if lock unavailable.

---

# 19. What is an atomic context?

Examples:

```text
Interrupt
Softirq
Tasklet
Spinlock-held section
```

Cannot:

```text
sleep
schedule
mutex_lock
copy_to_user
```

---

# 20. Why can't scheduler run inside spinlock?

Scheduler may sleep.

Spinlock implies:

```text
atomic context
```

Would deadlock.

---

# 21. Explain poll/select/epoll interaction with a driver.

Userspace:

```text
epoll_wait()
```

Driver:

```c
poll_wait()
```

Kernel links:

```text
waitqueue
```

When event arrives:

```text
wake_up()
```

epoll gets notified.

---

# 22. Why is epoll faster than select?

select:

```text
O(n)
scan all fds
```

epoll:

```text
event-driven
O(active events)
```

---

# 23. Explain race between poll and wakeup.

Classic bug:

```text
poll checks condition
      ↓
interrupt occurs
      ↓
wake_up()
      ↓
poll sleeps
```

Lost wakeup.

Need:

```text
check condition
register waitqueue
recheck condition
```

---

# 24. Why do we need memory barriers in drivers?

Multi-core issue:

CPU0:

```c
flag = 1;
data = 100;
```

CPU1:

```c
if(flag)
   read(data);
```

Compiler/CPU may reorder.

Need:

```c
smp_wmb();
smp_rmb();
```

---

# 25. Difference between `spin_lock()` and `spin_lock_irqsave()`?

### spin_lock()

Only lock.

### spin_lock_irqsave()

```text
Disable local interrupts
Acquire lock
```

Used when IRQ handler uses same lock.

---

# 26. What race occurs if interrupt handler and read() use same buffer?

Example:

```text
IRQ updates buffer
read() consumes buffer
```

Need:

```c
spin_lock_irqsave()
```

Otherwise corruption.

---

# 27. Explain top-half and bottom-half processing.

### Top Half

IRQ handler.

```text
Fast
No sleeping
```

### Bottom Half

```text
tasklet
workqueue
threaded irq
```

Deferred work.

---

# 28. Why are workqueues preferred over tasklets in modern kernels?

Tasklets:

```text
Atomic context
Cannot sleep
```

Workqueues:

```text
Process context
Can sleep
Can allocate memory
```

---

# 29. What happens if device disappears while fd remains open?

Example:

```text
USB unplug
fd still open
```

Need:

```text
reference counting
disconnect handling
state machine
```

Otherwise use-after-free.

---

# 30. Explain a use-after-free bug in character drivers.

Example:

```c
open()
{
    file->private_data = dev;
}

remove()
{
    kfree(dev);
}

read()
{
    dev = file->private_data;
}
```

After removal:

```text
private_data points to freed memory
```

Kernel crash.

Need:

```text
kref
get_device()
put_device()
reference counting
```

---

# Questions Frequently Asked at NVIDIA/AMD

### Q31

Why is `read()` not guaranteed to return requested bytes?

Example:

```c
read(fd, buf, 4096);
```

May return:

```text
128
512
1024
4096
```

Driver decides.

---

### Q32

What is the difference between returning:

```c
0
```

and

```c
-EAGAIN
```

from read?

**0**

```text
EOF
```

**-EAGAIN**

```text
No data now
Try later
```

---

### Q33

What is the difference between:

```c
-ERESTARTSYS
```

and

```c
-EINTR
```

Very advanced signal-handling question.

---

### Q34

Why does Linux use waitqueues instead of condition variables?

Because waitqueues are integrated with:

```text
scheduler
task states
signals
wakeups
```

---

### Q35

How would you design a character driver supporting:

```text
1000 devices
10000 opens/sec
Multiple CPUs
DMA
poll/epoll
hotplug
```

This is often an architecture discussion rather than a coding question.

These questions test kernel internals, VFS, scheduler interaction, SMP synchronization, memory ordering, reference counting, hotplug handling, and race-condition analysis—the areas where senior kernel-driver interviews are usually focused.
