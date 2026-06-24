# VFS, Syscalls, and Driver Dispatch

The **Virtual File System (VFS)** is one of the most important abstractions in the Linux kernel. It provides a **uniform interface** between user-space programs and the many different filesystems and devices they access. When a process calls `read()` or `write()`, the VFS decides **which implementation** actually runs and **dispatches** the call to it — an ext4 filesystem, an NFS mount, or a character driver.

---

## How the VFS Dispatches a Call

The phrase *"dispatches the request to the appropriate filesystem"* means the VFS forwards the operation to the filesystem (or driver) that owns the file.

For example, suppose your system has multiple filesystems mounted:

```text
/
├── home      → ext4
├── media     → FAT32 USB drive
├── network   → NFS
└── dev       → Device files
```

When an application executes:

```c
read(fd, buf, 100);
```

the VFS first determines **which object** `fd` refers to, then dispatches the request accordingly.

### Case 1: Regular file on ext4

```text
User
  │
read()
  │
  ▼
VFS
  │
  ▼
ext4_read()
  │
  ▼
Disk
```

The VFS calls the read implementation provided by the **ext4 filesystem**.

---

### Case 2: File on an NFS mount

```text
User
  │
read()
  │
  ▼
VFS
  │
  ▼
NFS read()
  │
  ▼
Network Server
```

The VFS dispatches the request to the **NFS filesystem**, which retrieves the data over the network.

---

### Case 3: Character device (`/dev/mydevice`)

```text
User
  │
read()
  │
  ▼
VFS
  │
  ▼
my_driver_read()
  │
  ▼
Hardware
```

Here, the VFS dispatches the request to the **device driver's** `read` callback rather than a disk filesystem.

---

### What "Appropriate" Means

"Appropriate" means **the implementation associated with the object being accessed**:

* If it's an **ext4 file**, call ext4's file operations.
* If it's an **XFS file**, call XFS's file operations.
* If it's an **NFS file**, call NFS's file operations.
* If it's a **character device**, call the character driver's `struct file_operations`.
* If it's a **block device**, call the block device's operations.

The application doesn't need to know which one it is—it always uses the same API:

```c
open();
read();
write();
close();
```

The VFS determines the correct implementation and dispatches the operation automatically.

---

## The Problem Before VFS

Imagine Linux had **no VFS**.

Suppose your computer supports:

* ext4
* XFS
* FAT32
* NTFS
* NFS
* USB devices
* Character devices
* procfs
* sysfs

Every application would have to know exactly what kind of object it is accessing.

For example:

```text
if ext4
    ext4_read()

else if xfs
    xfs_read()

else if nfs
    nfs_read()

else if char device
    driver_read()

else if procfs
    proc_read()
```

Every program would become extremely complicated.

Adding a new filesystem would require changing every application.

That is clearly impractical.

---

## What the VFS Does

Instead, Linux inserts another layer.

```text
Application
      │
read()
      │
      ▼
     VFS
      │
      ├── ext4
      ├── XFS
      ├── FAT
      ├── NFS
      ├── procfs
      ├── sysfs
      └── Character Driver
```

Applications never communicate directly with ext4 or a device driver.

They always communicate with the VFS.

---

## Uniform API

Every program uses the same functions.

```c
open();
read();
write();
close();
lseek();
ioctl();
```

Regardless of whether the object is:

* a text file
* a USB device
* a network filesystem
* `/proc/cpuinfo`
* `/dev/tty`
* `/sys/class/gpio`

the application uses exactly the same API.

Example:

```c
int fd = open(path, O_RDONLY);
read(fd, buffer, 100);
close(fd);
```

The code doesn't change.

---

## Filesystems Are Interchangeable

Suppose your home directory is stored on ext4.

```
/home/user/file.txt
```

Application:

```c
read(fd, buf, 100);
```

Later you reformat the partition to XFS.

Application:

```c
read(fd, buf, 100);
```

Nothing changes.

The application doesn't know or care.

Only the VFS dispatches to a different filesystem implementation.

---

## Every Filesystem Implements the Same Interface

The VFS defines operations like

```
open
read
write
create
unlink
mkdir
```

Each filesystem supplies its own implementation.

Example:

```
VFS read()
   │
   ▼
ext4_read()
```

or

```
VFS read()
   │
   ▼
xfs_read()
```

Same interface.

Different implementation.

---

## Driver Independence

Character devices are treated like files.

```
/dev/tty
/dev/null
/dev/random
/dev/mydriver
```

Application:

```c
write(fd, data, len);
```

VFS calls

```
driver->write()
```

The application never needs to know which driver exists.

---

## Network Files Look Local

Suppose a file is actually on another computer.

```
NFS Server
remote.txt
```

Application:

```c
read(fd, buf, 100);
```

The application thinks it's reading a local file.

Actually,

```
Application
   ↓
VFS
   ↓
NFS Driver
   ↓
Network
   ↓
Remote Machine
```

Without VFS, every application would need networking code.

---

## Pseudo Filesystems

Linux exposes kernel information as files.

```
/proc
/sys
```

Example:

```bash
cat /proc/meminfo
```

`cat` doesn't know it's reading kernel memory.

It simply executes

```c
open();
read();
close();
```

The VFS dispatches the request to procfs, which generates the file contents dynamically.

---

## Easy Addition of New Filesystems

Suppose someone develops a filesystem called

```
ABCFS
```

Without VFS:

```
Every application must learn ABCFS.
```

Impossible.

With VFS:

```
ABCFS implements VFS operations.
```

Immediately,

```
cat
cp
mv
vim
gcc
python
bash
```

all work automatically.

No application changes.

---

## Object Abstraction

The VFS represents everything using common kernel objects.

```
inode
file
dentry
super_block
```

Regardless of filesystem.

Applications don't know whether the inode belongs to

```
ext4
NFS
tmpfs
procfs
```

The VFS hides those details.

---

## Polymorphism in C

This is one of the biggest design ideas.

Every filesystem has different code.

But every filesystem provides function pointers.

Example:

```c
struct file_operations {
    int     (*open)(...);
    ssize_t (*read)(...);
    ssize_t (*write)(...);
};
```

The VFS only knows

```c
file->f_op->read(...);
```

It doesn't know whether

```
ext4_read()
xfs_read()
proc_read()
driver_read()
```

will execute.

This is **runtime polymorphism** implemented with function pointers in C, similar to virtual methods in object-oriented languages.

---

## Real Execution Flow

```
Application
      │
      ▼
read()
      │
      ▼
sys_read()
      │
      ▼
VFS
      │
      ├──────────────┐
      │              │
      ▼              ▼
Regular File     Character Device
      │              │
      ▼              ▼
ext4_read()     my_driver_read()
      │              │
      ▼              ▼
Disk          Hardware Register
```

The application is unaware of which path is taken.

---

## Key Benefits of VFS

| Benefit                     | Explanation                                                                                                                                                                  |
| --------------------------- | ---------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| **Uniform API**             | Programs always use `open()`, `read()`, `write()`, etc., regardless of the underlying object.                                                                                |
| **Filesystem independence** | Applications work with ext4, XFS, NFS, FAT, tmpfs, and others without modification.                                                                                          |
| **Device-as-file model**    | Character and block devices can be accessed using the same file API.                                                                                                         |
| **Extensibility**           | New filesystems or drivers only need to implement the VFS interfaces; existing applications continue to work.                                                                |
| **Code reuse**              | Common path lookup, permission checks, file descriptor management, and caching logic are centralized in the VFS instead of being duplicated by every filesystem.             |
| **Abstraction**             | The VFS hides implementation details, allowing user-space programs to focus on *what* they want to do (read or write data), while the kernel determines *how* to perform it. |

In essence, the VFS acts as a **universal interface and dispatcher** that decouples user applications from the specific implementation details of filesystems and devices. This separation is what makes Linux flexible, extensible, and capable of treating many different resources uniformly as files.
