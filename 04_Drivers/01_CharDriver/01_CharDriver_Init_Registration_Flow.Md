# CharDriver Init and Registration Flow

Character device driver initialization registers the driver with the kernel so that VFS can route `/dev` syscalls to the correct `file_operations` table.

---

## Registration Steps

### 1. Allocate Device Number

`alloc_chrdev_region()` dynamically assigns a `dev_t` (major + minor).

```c
dev_t devno;
alloc_chrdev_region(&devno, 0 /*first_minor*/, 1 /*count*/, "mydev");
```

Prefer dynamic allocation over static `register_chrdev_region()` to avoid major number conflicts.

---

### 2. Initialize cdev

`cdev_init()` links the `struct cdev` to the driver's `file_operations`.

```c
struct cdev my_cdev;
cdev_init(&my_cdev, &my_fops);
my_cdev.owner = THIS_MODULE;
```

---

### 3. Add cdev to Kernel

`cdev_add()` makes the device live — from this point VFS can dispatch calls.

```c
cdev_add(&my_cdev, devno, 1);
```

---

### 4. Create Device Node via udev

`class_create()` + `device_create()` populate `/sys/class/` and trigger udev to create `/dev/mydev`.

```c
struct class *my_class = class_create(THIS_MODULE, "mydev_class");
device_create(my_class, NULL, devno, NULL, "mydev");
```

---

## Initialization Flow

```
module_init()
    │
    ├─ alloc_chrdev_region()   → assigns dev_t (major:minor)
    │
    ├─ cdev_init()             → links cdev ↔ file_operations
    │
    ├─ cdev_add()              → registers cdev with kernel VFS
    │
    ├─ class_create()          → creates /sys/class/<name>/
    │
    └─ device_create()         → udev creates /dev/<name>
```

---

## Cleanup (module_exit)

Reverse order of initialization:

```c
device_destroy(my_class, devno);
class_destroy(my_class);
cdev_del(&my_cdev);
unregister_chrdev_region(devno, 1);
```
