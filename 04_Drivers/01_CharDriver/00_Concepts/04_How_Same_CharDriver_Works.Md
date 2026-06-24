When people say **"same character driver supports multiple instances"**, they mean that **one driver code** can manage **multiple devices** (multiple hardware blocks or multiple logical devices) using the same set of `file_operations`.

For example:

```text
Driver: my_uart_driver

/dev/myuart0
/dev/myuart1
/dev/myuart2
/dev/myuart3
```

All devices use the same driver code, but each device has its own private data.

---

# 1. Basic Character Driver Architecture

A character driver is represented by:

```c
struct cdev
```

which is registered with:

```c
cdev_add()
```

Kernel internally maintains:

```text
Major Number
    |
    +---- Minor 0 -> Device Instance 0
    |
    +---- Minor 1 -> Device Instance 1
    |
    +---- Minor 2 -> Device Instance 2
```

Example:

```c
alloc_chrdev_region(&dev, 0, 4, "mydrv");
```

Kernel allocates:

```text
Major = 240

240:0
240:1
240:2
240:3
```

Each minor represents one instance.

---

# 2. Driver Private Structure

Typically:

```c
struct my_device {
        int id;
        struct cdev cdev;
        spinlock_t lock;
        char buffer[1024];
};
```

Create multiple objects:

```c
struct my_device devices[4];
```

Memory:

```text
devices[0]
devices[1]
devices[2]
devices[3]
```

Each represents one device instance.

---

# 3. Registration Flow

During module init:

```c
for (i = 0; i < 4; i++) {

        cdev_init(&devices[i].cdev, &my_fops);

        cdev_add(
                &devices[i].cdev,
                MKDEV(major, i),
                1);
}
```

Kernel now has:

```text
Minor 0 -> devices[0]
Minor 1 -> devices[1]
Minor 2 -> devices[2]
Minor 3 -> devices[3]
```

---

# 4. What Happens During open()

User:

```c
fd = open("/dev/mydrv2", O_RDWR);
```

Kernel path:

```text
sys_open()
   |
do_sys_open()
   |
path_lookup()
   |
inode found
   |
chrdev_open()
```

Inside inode:

```c
inode->i_rdev
```

contains:

```text
Major = 240
Minor = 2
```

Kernel locates:

```text
240:2
```

and finds:

```text
devices[2].cdev
```

---

# 5. How Kernel Finds Correct Instance

Kernel internally does:

```c
struct cdev *p = inode->i_cdev;
```

This points to:

```text
devices[2].cdev
```

Now driver needs:

```text
devices[2]
```

not just cdev.

Kernel uses:

```c
container_of()
```

---

# 6. container_of()

Driver:

```c
struct my_device {
        int id;
        struct cdev cdev;
};
```

Inside open:

```c
struct my_device *dev;

dev = container_of(inode->i_cdev,
                   struct my_device,
                   cdev);
```

Result:

```text
inode->i_cdev
      |
      V
devices[2].cdev
      |
container_of()
      |
      V
devices[2]
```

Now driver knows:

```c
dev->id = 2
```

---

# 7. Store Instance in file->private_data

Usually:

```c
static int my_open(struct inode *inode,
                   struct file *file)
{
        struct my_device *dev;

        dev = container_of(inode->i_cdev,
                           struct my_device,
                           cdev);

        file->private_data = dev;

        return 0;
}
```

Very important step.

Now every subsequent operation gets the device.

---

# 8. Read Flow

User:

```c
read(fd, buf, 100);
```

Kernel:

```text
vfs_read()
    |
my_read()
```

Driver:

```c
static ssize_t my_read(
                struct file *file,
                char __user *buf,
                size_t len,
                loff_t *off)
{
        struct my_device *dev;

        dev = file->private_data;

        ...
}
```

Now:

```text
fd for mydrv2
      |
private_data
      |
devices[2]
```

Driver accesses correct instance.

---

# 9. Multiple Processes Scenario

Process A:

```c
open("/dev/mydrv0")
```

Process B:

```c
open("/dev/mydrv2")
```

Memory:

```text
Process A
file->private_data
      |
devices[0]

Process B
file->private_data
      |
devices[2]
```

Same driver code.

Different device contexts.

---

# 10. Internal Kernel Data Structures

### inode

```c
struct inode {
        dev_t i_rdev;
        struct cdev *i_cdev;
};
```

Contains:

```text
major/minor
cdev pointer
```

---

### file

```c
struct file {
        void *private_data;
};
```

Contains:

```text
device instance pointer
```

---

### cdev

```c
struct cdev {
        const struct file_operations *ops;
};
```

Contains:

```text
driver callbacks
```

---

# 11. Platform Driver Case

Suppose Device Tree:

```dts
uart0 {
};

uart1 {
};

uart2 {
};
```

Probe called three times.

```c
my_probe()
{
        devm_kzalloc();
}
```

Each probe creates:

```c
struct my_device
```

instance.

```text
Probe #1 -> dev0
Probe #2 -> dev1
Probe #3 -> dev2
```

All use same:

```c
file_operations
```

but different private data.

---

# 12. Internal Flow Diagram

```text
open("/dev/mydrv2")
       |
       V
sys_open()
       |
       V
chrdev_open()
       |
       V
inode->i_rdev
       |
       V
Major=240 Minor=2
       |
       V
cdev lookup
       |
       V
devices[2].cdev
       |
       V
container_of()
       |
       V
devices[2]
       |
       V
file->private_data
       |
       V
read/write/ioctl
       |
       V
devices[2]
```

---

# Common Interview Question (AMD/NVIDIA/Qualcomm)

### Q1: How does a single character driver support multiple devices?

**Answer:**
A single driver registers multiple minor numbers under one major number. Each minor number corresponds to a separate device instance. During `open()`, the kernel identifies the device using `inode->i_rdev` and `inode->i_cdev`. The driver retrieves the device-specific structure using `container_of()` and stores it in `file->private_data`.

---

### Q2: Why is `file->private_data` used?

**Answer:**
It stores the device instance pointer obtained during `open()`. Subsequent operations (`read`, `write`, `ioctl`, `poll`, etc.) can directly access the correct device context without repeatedly looking up the device.

---

### Q3: How does the kernel distinguish `/dev/mydrv0` and `/dev/mydrv1`?

**Answer:**
By the **minor number**. Both may share the same major number and driver code, but different minor numbers map to different `cdev` objects and device instances.

---

### Q4: Why use `container_of()`?

**Answer:**
`inode->i_cdev` gives only the embedded `cdev`. `container_of()` retrieves the enclosing device structure that contains driver-specific data.

This is the exact mechanism used by many Linux drivers such as UART, GPIO character devices, I2C character interfaces, watchdogs, and vendor drivers in AMD, NVIDIA, and Qualcomm kernels.


To support multiple instances of the same hardware (or virtual device) using a single character driver, the Linux kernel employs a **"One Driver, Many Objects"** architecture. Instead of hardcoding device-specific data into global variables, the driver encapsulates this data within a structure and maps that structure to specific device nodes.

Here is the in-depth explanation of how this works internally.

---

### 1. The Core Concept: The `cdev` and `container_of`

To handle multiple instances, your driver must not use global variables for device state. Instead, you create a custom structure representing your device.

```c
struct my_device_data {
    struct cdev cdev;           // The kernel's char device structure
    struct device *device;      // For sysfs/udev
    int device_id;              // Instance identifier
    unsigned char *buffer;      // Instance-specific memory
    struct semaphore sem;       // Instance-specific lock
};

```

When a user process performs a system call (like `read()` or `write()`), the kernel provides a pointer to a `struct file`. The `file->private_data` field is the secret weapon used to link the system call back to the specific instance.

---

### 2. Internals: The Registration Lifecycle

#### A. Probing/Initialization

When your driver initializes (or probes a bus like PCI/platform), it allocates memory for the `my_device_data` structure for each instance it finds.

1. **Allocate** the structure (e.g., `kzalloc`).
2. **Initialize** the `cdev` member: `cdev_init(&my_data->cdev, &fops)`.
3. **Register** with the kernel: `cdev_add(&my_data->cdev, dev_number, 1)`.

#### B. The `open()` System Call

This is where the magic happens. When a user calls `open("/dev/mydevice0")`, the kernel locates the specific `cdev` associated with that major/minor number.

Inside your `my_driver_open` function:

1. **Retrieve the structure:** Use the `container_of` macro to find the base of your `my_device_data` from the `cdev` pointer.
2. **Assign `private_data`:** Set `file->private_data = my_data;`.

```c
int my_open(struct inode *inode, struct file *file) {
    struct my_device_data *my_data;

    // Get the instance data from the inode's cdev
    my_data = container_of(inode->i_cdev, struct my_device_data, cdev);

    // Store it in the file structure for future calls (read/write/close)
    file->private_data = my_data;

    return 0;
}

```

#### C. Handling Read/Write/Ioctl

When a user calls `read()`, the kernel passes the same `file` pointer. Because you set `file->private_data` in `open()`, your read function now knows exactly which hardware instance it is talking to:

```c
ssize_t my_read(struct file *file, char __user *buf, size_t count, loff_t *f_pos) {
    // Retrieve the instance-specific data
    struct my_device_data *my_data = file->private_data;

    // Perform operations using instance-specific buffers/locks
    // ...
}

```

---

### 3. Summary of Internal Workflow

| Stage | Action | Internal Mechanism |
| --- | --- | --- |
| **Registration** | Driver creates multiple `cdev` objects. | Each `cdev` has a unique minor number. |
| **Open** | User opens `/dev/deviceN`. | Kernel finds `cdev` via minor number; driver maps `file->private_data`. |
| **IO Ops** | User performs `read`/`write`. | Driver extracts `my_data` from `file->private_data` to access instance-specific state. |
| **Release** | User closes file. | Driver cleans up or frees temporary resources. |

---

### Why this is effective

* **Isolation:** Each `file` structure has its own `private_data`. If User A is reading from `/dev/mydevice0` and User B is reading from `/dev/mydevice1`, they are accessing different memory addresses within the `my_device_data` objects, preventing race conditions between instances.
* **Scalability:** You can register as many minor numbers as needed (up to the limits of the minor number range), all using the exact same code logic.
* **Encapsulation:** The driver logic remains clean and object-oriented, mirroring how other kernel subsystems (like network devices) operate.

Would you like to see a minimal, compilable C code example that demonstrates how to implement this `container_of` pattern for two distinct instances?
