# Design: Full Integrated Production Char Driver
## Level 03 | End-to-End Deep Design from Scratch

---

## 1. Architecture Overview

Level 03 combines every kernel driver mechanism into a single, production-grade driver. The key design shift from Level 02: **no bare global variables** — everything is encapsulated in a `struct my_dev` per-device structure.

```
┌─────────────────────────────────────────────────────────┐
│                      User Space                         │
│   open()  read()  write()  ioctl()  poll()  close()     │
└────────────┬────────────────────────────────────────────┘
             │ syscall
┌────────────▼────────────────────────────────────────────┐
│                   VFS Layer                             │
│   vfs_read() → file->f_op->read = my_read()             │
└────────────┬────────────────────────────────────────────┘
             │
┌────────────▼────────────────────────────────────────────┐
│              Driver (struct my_dev)                     │
│                                                         │
│  ┌──────────┐   ┌──────────┐   ┌──────────────────────┐ │
│  │  cdev    │   │ io_mutex │   │    ring[RING_SIZE]   │ │
│  │ fops     │   │ ring_lock│   │ head/tail/count      │ │
│  └──────────┘   └──────────┘   └──────────────────────┘ │
│                                                         │
│  ┌──────────┐   ┌──────────────────────────────────────┐│
│  │ read_wq  │   │  IRQ top-half → IRQ bottom-half      ││
│  │ write_wq │   │  (request_threaded_irq)              ││
│  └──────────┘   └──────────────────────────────────────┘│
└─────────────────────────────────────────────────────────┘
             │
┌────────────▼────────────────────────────────────────────┐
│                    Hardware / IRQ                       │
└─────────────────────────────────────────────────────────┘
```

---

## 2. Per-Device Structure Pattern

### Why?

Real drivers support **multiple instances** (e.g., 4 serial ports, 8 SPI channels). Bare globals break with multiple instances.

```c
struct my_dev {
    dev_t           devno;       // major:minor
    struct cdev     cdev;        // kernel char device
    struct class   *cls;         // sysfs class
    struct device  *dev;         // /dev entry

    char            ring[...];   // data buffer
    spinlock_t      ring_lock;   // IRQ-safe lock
    struct mutex    io_mutex;    // process context lock

    wait_queue_head_t read_wq;   // block readers here
    wait_queue_head_t write_wq;  // block writers here

    atomic_t        count;       // bytes available
    atomic_t        total_reads; // statistics
    unsigned int    threshold;   // min bytes to wake reader
};
```

### `container_of` Pattern

```c
/* In open(), recover per-device pointer from inode */
struct my_dev *d = container_of(inode->i_cdev, struct my_dev, cdev);
filp->private_data = d;

/* In read/write/ioctl, use private_data */
struct my_dev *d = filp->private_data;
```

`container_of(ptr, type, member)` — given a pointer to a struct member, returns pointer to the enclosing struct. Zero overhead, pure pointer arithmetic.

---

## 3. Spinlock vs Mutex: When to Use Which

| | Spinlock | Mutex |
|---|---|---|
| **Context** | IRQ handlers, atomic context | Process context only |
| **Blocking** | No — busy-waits (spin) | Yes — can sleep |
| **Use when** | Lock held for < 1 µs, IRQ context | Lock held longer, can sleep |
| **API** | `spin_lock_irqsave()` | `mutex_lock()` |

In this driver:
- `ring_lock` (spinlock) — used in IRQ bottom-half for ring buffer access
- `io_mutex` — used in `read()`/`write()` process context for broader protection

```c
/* IRQ-safe spinlock usage */
unsigned long flags;
spin_lock_irqsave(&d->ring_lock, flags);
/* ... critical section ... */
spin_unlock_irqrestore(&d->ring_lock, flags);
/* flags saves/restores IRQ enable state */
```

`irqsave` variant: disables local CPU interrupts while holding lock, preventing deadlock if IRQ fires while lock is held.

---

## 4. Threaded IRQ — `request_threaded_irq`

### Classic IRQ Problem

Top-half runs with interrupts disabled — cannot:
- Call `mutex_lock()` (might sleep)
- Call `schedule()`
- Do DMA operations

**Solution**: Split into two halves.

```c
request_threaded_irq(
    IRQ_NUM,
    my_irq_top,     // top-half: runs in hardirq context
    my_irq_bottom,  // bottom-half: runs in kernel thread (can sleep)
    IRQF_SHARED | IRQF_ONESHOT,
    DRIVER_NAME,
    device_ptr
);
```

### IRQF_ONESHOT

Required for threaded IRQ: **keeps IRQ line masked** until bottom-half completes. Without it, the IRQ would re-fire before the thread handles the previous one.

### Execution Timeline

```
Hardware IRQ fires
      │
      ▼
my_irq_top() runs [hardirq, no preemption]
├─ return IRQ_WAKE_THREAD
      │
      ▼
Kernel IRQ thread wakes up [process context, preemptible]
      │
      ▼
my_irq_bottom() runs
├─ can use spin_lock / mutex
├─ push data to ring buffer
└─ wake_up_interruptible(&d->read_wq)
```

---

## 5. Threshold-Based Read Wakeup

A configurable threshold allows batching:

```c
/* Only wake reader when N bytes are ready */
wake_up_interruptible(&d->read_wq);

/* Reader waits for threshold, not just any byte */
wait_event_interruptible(d->read_wq,
    ring_avail(d) >= d->threshold);
```

This reduces unnecessary context switches — critical for high-throughput I/O (e.g., modem data channels).

---

## 6. Bidirectional Flow Control

```
Writers  ──writes──►  ring buffer  ──reads──►  Readers
                          │
              ring_free()─┤─ring_avail()
                          │
         write_wq ◄── wake when space ──► read_wq
```

- When ring is **full**: writers sleep on `write_wq`
- When ring is **empty**: readers sleep on `read_wq`
- After read: `wake_up(write_wq)` — space freed
- After write: `wake_up(read_wq)` — data available

---

## 7. IOCTL: Status + Config

### `dev_status` struct shared between kernel and user-space

```c
struct dev_status {
    unsigned int bytes_in_buf;
    unsigned int total_reads;
    unsigned int total_writes;
    unsigned int total_irqs;
};
```

**Warning**: struct must be identical in kernel and user-space. Use exact same definition in a shared header. Avoid padding differences between 32/64-bit with `__attribute__((packed))` if needed.

---

## 8. Memory Allocation: `kzalloc`

```c
g_dev = kzalloc(sizeof(*g_dev), GFP_KERNEL);
```

- `kzalloc` = `kmalloc` + `memset(0)` — zero-initializes all fields
- `GFP_KERNEL` — may sleep to reclaim memory (OK in init context)
- `GFP_ATOMIC` — used in IRQ/atomic context, no sleep, may fail

Always check return: `if (!g_dev) return -ENOMEM;`

---

## 9. Production Checklist

| Item | Status | Notes |
|------|--------|-------|
| No bare globals | ✅ | All state in `struct my_dev` |
| IRQ top/bottom split | ✅ | `request_threaded_irq` |
| Spinlock for IRQ context | ✅ | `spin_lock_irqsave` |
| Mutex for process context | ✅ | `mutex_lock` |
| Wait queues for both sides | ✅ | `read_wq` + `write_wq` |
| O_NONBLOCK support | ✅ | `EAGAIN` path |
| ERESTARTSYS handling | ✅ | Signal-safe sleep |
| Statistics via ioctl | ✅ | `IOCTL_GET_STATUS` |
| goto-based error cleanup | ✅ | Init error paths |
| kfree on exit | ✅ | No memory leak |

---

## 10. Test Script

```bash
#!/bin/bash
DEV="/dev/MyAnilDev3"

# Load
sudo insmod integrated_driver.ko

# Non-blocking read (should get EAGAIN)
sudo dd if=$DEV of=/dev/null bs=1 count=1 iflag=nonblock 2>&1

# Write then read
echo "Production Test" | sudo tee $DEV
sudo cat $DEV

# IOCTL status
cat > query.c << 'EOF'
#include <stdio.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#define MY_MAGIC 'A'
struct dev_status { unsigned int b, r, w, i; };
#define IOCTL_GET_STATUS _IOR(MY_MAGIC, 1, struct dev_status)
int main() {
    int fd = open("/dev/MyAnilDev3", O_RDWR);
    struct dev_status s;
    ioctl(fd, IOCTL_GET_STATUS, &s);
    printf("buf=%u reads=%u writes=%u irqs=%u\n", s.b, s.r, s.w, s.i);
    return 0;
}
EOF
gcc query.c -o query ; sudo ./query

sudo rmmod integrated_driver
```
