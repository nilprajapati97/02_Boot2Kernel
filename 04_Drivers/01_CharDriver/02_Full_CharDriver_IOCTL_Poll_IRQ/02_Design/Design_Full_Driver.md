# Design: Full Char Driver — IOCTL + Poll + IRQ + Ring Buffer
## Level 02 | End-to-End Deep Design from Scratch

---

## 1. Why This Level Exists

Level 01 gave us a basic char driver. Real hardware drivers need:
- **ioctl** — configuration and control without data read/write
- **poll/select/epoll** — efficient async I/O monitoring
- **Interrupt handling** — hardware-triggered events
- **Ring buffer** — efficient, lock-free data pipe between IRQ and reader

---

## 2. IOCTL — Kernel Control Interface

### What is ioctl?

`ioctl` (I/O control) is a syscall for **out-of-band** communication with a device — operations that don't fit the read/write data model.

```
user: ioctl(fd, IOCTL_CLEAR_BUFFER, 0)
         │
         ▼
   sys_ioctl() → vfs_ioctl() → my_ioctl()
```

### Command Encoding (Linux Standard)

```c
_IO(magic, nr)           // no data transfer
_IOR(magic, nr, type)    // kernel → user (Read)
_IOW(magic, nr, type)    // user → kernel (Write)
_IOWR(magic, nr, type)   // bidirectional
```

The 32-bit ioctl number encodes:
- bits [7:0]  — command number
- bits [15:8] — magic (driver identifier)
- bits [29:16] — data size
- bits [31:30] — direction (00=none, 10=read, 01=write, 11=both)

This prevents two drivers from accidentally using the same command code.

### Why `unlocked_ioctl`?

The old `ioctl` took the **Big Kernel Lock (BKL)**. `unlocked_ioctl` is the modern replacement — **you own the locking** using mutex/spinlock. This gives better concurrency.

---

## 3. Wait Queues — The Blocking I/O Engine

A **wait queue** is a kernel list of sleeping tasks associated with a condition.

```c
DECLARE_WAIT_QUEUE_HEAD(read_wq);

/* Producer side (IRQ/writer) */
wake_up_interruptible(&read_wq);    // wake sleepers

/* Consumer side (reader) */
wait_event_interruptible(read_wq, condition);  // sleep until condition true
```

### Internal Mechanism

```
wait_event_interruptible():
  1. Evaluate condition → true? Return immediately
  2. Add current task to read_wq list
  3. Set task state = TASK_INTERRUPTIBLE
  4. Call schedule() → CPU given to another task
  5. Signal arrives? Return -ERESTARTSYS
  6. wake_up called? Remove from list, re-check condition
```

`ERESTARTSYS` tells the syscall layer to restart the syscall if the signal disposition allows it.

---

## 4. Interrupt Handler

### Top-Half vs Bottom-Half

| Layer | When | Constraints |
|-------|------|-------------|
| Top-half (hardirq) | Immediately at interrupt | No sleep, no blocking, minimal work |
| Bottom-half (softirq/workqueue/tasklet) | Deferred, scheduled | Can sleep (workqueue only) |

```
Hardware triggers IRQ
        │
        ▼
CPU jumps to interrupt vector
        │
        ▼
my_irq_handler()     ← TOP HALF
├─ read HW register
├─ push data to ring buffer
├─ wake_up_interruptible(&read_wq)
└─ return IRQ_HANDLED

        │ (scheduled by kernel)
        ▼
Worker/Tasklet       ← BOTTOM HALF (if needed)
└─ heavy processing
```

### `request_irq()` parameters

```c
request_irq(
    irq,           // IRQ line number
    handler,       // your handler function
    IRQF_SHARED,   // share IRQ line with other devices (real HW common)
    name,          // appears in /proc/interrupts
    dev_id         // unique cookie for IRQF_SHARED (used to free)
);
```

`IRQF_SHARED` allows multiple devices on same IRQ line. Your handler is called; return `IRQ_HANDLED` if your device caused it, `IRQ_NONE` otherwise.

---

## 5. Ring Buffer (Circular Buffer) — Data Path Design

### Why Ring Buffer?

Between IRQ (producer) and `read()` (consumer):
- IRQ cannot block or sleep
- Reader may be slow (user space)
- Need a **lock-free** or **minimally locked** buffer

### Structure

```
RING_SIZE = 512 (must be power-of-2)

     tail                    head
      │                       │
      ▼                       ▼
  [  |T|D|A|T|A|_|_|_|_|_|_|H|  ]
       └───────────────────┘
              count = 5

head = write index
tail = read index
count = atomic (readable by both IRQ and process context safely)
```

### Power-of-2 Trick

```c
/* Avoids expensive modulo % */
index = ptr & (RING_SIZE - 1);   /* same as ptr % RING_SIZE */
```

### Lock Strategy

- `atomic_t count` — read by IRQ without lock for fast empty check
- `mutex_lock` — taken for actual push/pop to prevent head/tail race

---

## 6. poll() — Enabling select/epoll

### How epoll works with a driver

```
epoll_wait(epfd)
      │
      ▼
sys_epoll_wait()
      │
      ▼
For each fd: call file->f_op->poll()
      │
      ▼
my_poll():
  poll_wait(filp, &read_wq, wait)  ← registers wait queue
  if (!ring_empty) return EPOLLIN
  return 0   ← not ready, epoll will sleep

      │ (later: IRQ fires, wake_up called)
      ▼
epoll_wait() returns, fd is readable
```

### Return Mask

```c
EPOLLIN   | EPOLLRDNORM   // data available for read
EPOLLOUT  | EPOLLWRNORM   // space available for write
EPOLLERR                  // error condition
EPOLLHUP                  // hang-up
```

---

## 7. O_NONBLOCK Behavior

Drivers must respect `O_NONBLOCK`:

```c
if (ring_empty(&ring)) {
    if (filp->f_flags & O_NONBLOCK)
        return -EAGAIN;         // return immediately
    wait_event_interruptible(read_wq, !ring_empty(&ring));
}
```

User-space handles `EAGAIN` by checking later or using poll.

---

## 8. Concurrency Analysis

| Scenario | Problem | Solution |
|----------|---------|----------|
| Two processes read simultaneously | Both pop same byte | `mutex_lock` around pop |
| IRQ writes while process reads | Ring state corruption | `mutex_lock` in IRQ handler |
| Multiple writers | Head pointer race | `mutex_lock` around push |
| Atomic count check | Out-of-sync | `atomic_t` for quick empty/full check |

---

## 9. Full Data Flow

```
[User write("hello")]
       │
       ▼
my_write() → copy_from_user() → ring_push() → wake_up_interruptible()

[IRQ fires]
       │
       ▼
my_irq_handler() → ring_push("IRQ_DATA") → wake_up_interruptible()

[User read()]
       │
       ▼
my_read() → wait_event_interruptible() → ring_pop() → copy_to_user()

[User select()/epoll_wait()]
       │
       ▼
my_poll() → poll_wait() → returns EPOLLIN when ring non-empty
```

---

## 10. Build and Test

```bash
# Build
make -C /lib/modules/$(uname -r)/build M=$PWD modules

# Load
sudo insmod full_driver.ko

# Test IOCTL
cat > test_ioctl.c << 'EOF'
#include <fcntl.h>
#include <sys/ioctl.h>
#define MY_IOCTL_MAGIC 'k'
#define IOCTL_CLEAR_BUFFER _IO(MY_IOCTL_MAGIC, 0)
#define IOCTL_GET_BUF_LEN  _IOR(MY_IOCTL_MAGIC, 1, int)
int main() {
    int fd = open("/dev/MyAnilDev2", O_RDWR);
    int len;
    ioctl(fd, IOCTL_GET_BUF_LEN, &len);
    printf("buf len = %d\n", len);
    ioctl(fd, IOCTL_CLEAR_BUFFER);
    return 0;
}
EOF
gcc test_ioctl.c -o test_ioctl ; sudo ./test_ioctl

# Test poll
sudo cat /dev/MyAnilDev2 &    # blocks in wait queue
echo "wake" | sudo tee /dev/MyAnilDev2  # unblocks reader
```

---

## Summary

| Feature | Mechanism | Key Function |
|---------|-----------|-------------|
| Control interface | ioctl | `unlocked_ioctl` |
| Blocking read | wait queue | `wait_event_interruptible` |
| Interrupt data | IRQ handler | `request_irq` + `wake_up` |
| Async I/O | poll | `poll_wait` + bitmask |
| Data pipe | ring buffer | `ring_push` / `ring_pop` |
| Concurrency | mutex + atomic | `mutex_lock`, `atomic_inc` |
