11. SysFS in Linux Kernel
=============================================================================================
1.SysFS is a virtual filesystem in the Linux kernel that exposes information about kernel objects to user space in a hierarchical file-like structure.
   It is typically mounted at /sys .
2.Export system information from the kernel space to the user space for specific devices.
   The SysFS is tied to the device driver model of the kernel.

🔹 Key Concepts
============================================================================================
-: Virtual filesystem (like /proc, but for devices and kernel objects)
-: Backed by kobject, kset, and related infrastructure
-: Allows exposing attributes and even some control interfaces
-: Mounted automatically by most modern distros at /sys

🔹 Common SysFS Structure (Example from /sys)
============================================================================================

/sys
 ├── class/             -> High-level device classes (e.g. net, block)
 ├── devices/           -> Physical device tree
 ├── bus/               -> Bus types (e.g. PCI, USB)
 ├── module/            -> Kernel modules
 ├── firmware/          -> Firmware loading interface
 ├── fs/                -> Filesystem-specific controls
 └── kernel/            -> Kernel settings


🔹 Creating SysFS Entries in a Kernel Module
============================================================================================

-: Here’s a simple example of how to create a SysFS file that a user can read/write from /sys.

1. Define a kobject and attributes
===================================

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/kobject.h>
#include <linux/sysfs.h>

static struct kobject *example_kobj;
static int my_value;

static ssize_t my_show(struct kobject *kobj, struct kobj_attribute *attr, char *buf) {
    return sprintf(buf, "%d\n", my_value);
}

static ssize_t my_store(struct kobject *kobj, struct kobj_attribute *attr,
                        const char *buf, size_t count) {
    sscanf(buf, "%d", &my_value);
    return count;
}

static struct kobj_attribute my_attr = __ATTR(my_value, 0660, my_show, my_store);

2. Initialize in module
===================================

static int __init example_init(void) {
    example_kobj = kobject_create_and_add("example", kernel_kobj);
    if (!example_kobj)
        return -ENOMEM;

    return sysfs_create_file(example_kobj, &my_attr.attr);
}

3. Cleanup in exit
=====================

static void __exit example_exit(void) {
    sysfs_remove_file(example_kobj, &my_attr.attr);
    kobject_put(example_kobj);
}

module_init(example_init);
module_exit(example_exit);
MODULE_LICENSE("GPL");


After loading the module:
==================================

-:  cat /sys/kernel/example/my_value
-:  echo 42 > /sys/kernel/example/my_value

🔹 When to Use SysFS
=====================================
-:  To expose kernel variables or device states to user space
-:  For debugging or configuring drivers/devices at runtime
-:  To allow user space to trigger certain behaviors in the kernel


🔹 Guidelines / Best Practices
==========================================
-: Keep it simple: SysFS is not for large data transfer or complex interfaces.
-: Permissions: Use proper permissions (0444 for readonly, 0660 for read/write).
-: Avoid complex logic: Each file should ideally represent one variable.
-: No binary files: All data should be exposed in ASCII format (human-readabl

