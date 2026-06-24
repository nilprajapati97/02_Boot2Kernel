# The Linux Character Driver Model — Design & From-Scratch Internals

> A deep, interview-grade reference for how Linux character device drivers work, built
> around a **scull-style in-memory buffer device** with multiple minors. It pins every
> API and behavior to a concrete, modern kernel so nothing is hand-wavy.

**Target kernel:** `6.17.0-35-generic` (Ubuntu 24.04 LTS, x86-64).
Every struct layout, function signature, and macro in these documents was verified
against the headers shipped in `/lib/modules/6.17.0-35-generic/build`. Where an API
changed across kernel versions, the change is called out explicitly.

This is a **documentation set**, not a buildable module. It contains illustrative C
fragments and real kernel-source excerpts because you cannot honestly explain the
internals at this depth without them — but there is no `Makefile` or complete `.ko`
project here.

---

## How to read this

The material is split into four documents that go from "what we are building" to
"what the silicon and the kernel actually do, line by line."

| # | Document                                                      | What it answers                                                                                                                                                                                                                                                                                                                                                           |
| - | ------------------------------------------------------------- | ------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| 1 | [docs/01-design.md](docs/01-design.md)                           | **The model & the design.** What a char device *is*, the data structures we choose, device numbering, `cdev` registration, the `file_operations` table, multiple minors, auto node creation, and the design of locking / blocking I/O / poll / ioctl / mmap / procfs-sysfs.                                                                                   |
| 2 | [docs/02-internals.md](docs/02-internals.md)                     | **From scratch, how it works inside the kernel.** "Everything is a file," VFS objects, device nodes, the char subsystem's `cdev_map`, the full `open()`/`read()`/`write()` call paths, `copy_*_user` and exception tables, wait queues, poll/epoll, ioctl decoding, mmap page tables, the device model & udev, module refcounting, and memory allocators. |
| 3 | [docs/03-traces-and-diagrams.md](docs/03-traces-and-diagrams.md) | **End-to-end traces & diagrams.** Mermaid architecture and per-operation sequence diagrams, plus annotated kernel call-stack traces for `open` and a blocking `read`.                                                                                                                                                                                           |
| 4 | [docs/04-appendix.md](docs/04-appendix.md)                       | **Reference.** API quick-reference, `_IOC` encoding tables and our command set, kernel-version notes, a glossary, common pitfalls / race conditions, and an interview Q&A self-check.                                                                                                                                                                             |
| 5 | [docs/05-i2c-boot-debugging.md](docs/05-i2c-boot-debugging.md)   | **A real-bus case study: debugging I2C failures at boot (ARM/ARM64 + Device Tree).** The four-layer model (controller → core → DT → client + the electrical bus), deferred probe, the wire protocol, a failure taxonomy + errno decoder, registration/transfer call paths, a layered methodology, software/hardware tooling, bus recovery, diagrams, and worked case studies. |

If you are preparing for an interview, read 1 → 2 → 3, then test yourself with the
Q&A in 4. If you just need a refresher on a single mechanism (say, blocking reads),
jump straight to the relevant section in document 2. Document 5 is a standalone,
applied case study — debugging why a **real I2C bus/device fails to come up at boot**
on an ARM/ARM64 Device-Tree system — and reads independently of the `scullbuf` model.

---

## The device we are designing: `scullbuf`

A classic teaching device modeled on Rubini & Corbet's *scull* ("Simple Character
Utility for Loading Localities"). It backs a `/dev` node with a chunk of kernel memory:

- **`write(2)`** copies bytes from user space into a kernel buffer.
- **`read(2)`** copies bytes back out. A reader **blocks** when the buffer is empty
  (unless `O_NONBLOCK`); a writer wakes waiting readers.
- **`llseek(2)`** repositions within the buffer.
- **`ioctl(2)`** resets the device and gets/sets the buffer quantum size.
- **`poll(2)`/`select(2)`/`epoll`** report readability/writability.
- **`mmap(2)`** maps the kernel buffer directly into a process's address space.
- It exposes **multiple minor devices** (independent buffers) behind one major number.
- It publishes **`/proc`** statistics and a **sysfs** attribute.

This single device exercises essentially every mechanism in the character-driver
contract, which is exactly why it is used to *teach* the contract.

---

## The 10,000-foot view

```text
                        ┌───────────────────────────────────────────────┐
        USER SPACE      │   application:  fd = open("/dev/scullbuf0");  │
                        │                 read(fd, buf, n);             │
                        └───────────────┬───────────────────────────────┘
                                        │  glibc wrapper -> syscall instruction
  ─────────────────────────────────────-│---------------------------------------  user/kernel boundary
                                        ▼
                        ┌───────────────────────────────────────────────┐
                        │  System call entry (arch) -> sys_read()       │
                        └───────────────┬───────────────────────────────┘
                                        ▼
                        ┌───────────────────────────────────────────────────┐
        VFS LAYER       │  fdtable -> struct file -> file->f_op             │
                        │  vfs_read() validates, calls f_op->read/read_iter │
                        └───────────────┬───────────────────────────────────┘
                                        ▼
                        ┌───────────────────────────────────────────────────┐
   CHAR DEV SUBSYSTEM   │  At open(): def_chr_fops.open = chrdev_open()     │
                        │  i_rdev --kobj_lookup(cdev_map)--> struct cdev    │
                        │  filp->f_op is swapped to OUR file_operations     │
                        └───────────────┬───────────────────────────────────┘
                                        ▼
                        ┌───────────────────────────────────────────────────┐
   OUR DRIVER           │  scullbuf_read(): mutex_lock; if empty &&         │
   (scullbuf)           │  blocking -> wait_event_interruptible(...);       │
                        │  copy_to_user(); update f_pos; wake writers       │
                        └───────────────┬───────────────────────────────────┘
                                        ▼
                        ┌───────────────────────────────────────────────────┐
   KERNEL MEMORY        │  per-minor buffer (kmalloc/vmalloc), size,        │
                        │  struct mutex, wait queues, struct cdev           │
                        └───────────────────────────────────────────────────┘
```

Each of those boxes is unpacked, function by function, in
[docs/02-internals.md](docs/02-internals.md).

---

## Conventions used in these docs

- Kernel functions are written like `chrdev_open()`; structs like `struct file`.
- Source citations look like `fs/char_dev.c : chrdev_open()` and refer to the
  mainline tree at the `6.17` series unless stated otherwise.
- "User space" / "kernel space" refer to the two halves of the virtual address space
  separated by the page-table privilege bit; crossing the boundary safely is a
  recurring theme (see `copy_to_user` in document 2).
- Code fragments are **illustrative**. They compile in spirit, but the focus is on the
  contract and the mechanism, not on being a drop-in module.
