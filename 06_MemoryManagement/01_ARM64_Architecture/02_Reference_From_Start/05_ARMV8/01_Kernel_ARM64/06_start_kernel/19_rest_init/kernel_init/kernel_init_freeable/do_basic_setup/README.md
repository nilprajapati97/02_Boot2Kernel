# `do_basic_setup()` — Run All initcalls

## Purpose

Calls `do_initcalls()`, which executes every function registered with `__initcall()` (and its variants). This is the point where all built-in device drivers, subsystems, and modules compile-linked to the kernel run their initialization code.

## Source File

`init/main.c`

```c
static void __init do_basic_setup(void)
{
    cpuset_init_smp();
    driver_init();         // Core driver model (bus, device, class)
    init_irq_proc();       // /proc/irq
    do_ctors();            // C++ constructors (rare in kernel)
    do_initcalls();        // ALL init functions
}
```

## `do_initcalls()` — The initcall Mechanism

### Registration

Kernel code registers init functions using macros:

```c
// Different priority levels:
early_initcall(my_func)        // Level 0 — very early
core_initcall(my_func)         // Level 1
postcore_initcall(my_func)     // Level 2
arch_initcall(my_func)         // Level 3
subsys_initcall(my_func)       // Level 4
fs_initcall(my_func)           // Level 5
rootfs_initcall(my_func)       // Level rootfs
device_initcall(my_func)       // Level 6 (default __initcall)
late_initcall(my_func)         // Level 7

// Most drivers use module_init() which expands to device_initcall():
module_init(e1000_init_module)  // Level 6
```

### How Registration Works

```c
// The macro expands to a linker section entry:
#define __define_initcall(fn, id) \
    static initcall_t __initcall_##fn##id \
    __used __attribute__((__section__(".initcall" #id ".init"))) = fn

// Creates a pointer in section .initcall6.init:
// e.g., __initcall_e1000_init_module6 = e1000_init_module
```

### Execution Order

```c
static void __init do_initcalls(void)
{
    int level;
    size_t len = cmdline_size;
    char *command_line;
    
    command_line = kzalloc(len, GFP_KERNEL);
    if (!command_line)
        panic("%s: Failed to allocate %zu bytes\n", __func__, len);
    
    for (level = 0; level < ARRAY_SIZE(initcall_levels) - 1; level++) {
        /* Need to save and restore the command line */
        memcpy(command_line, saved_command_line, len);
        do_initcall_level(level, command_line);
    }
    kfree(command_line);
}
```

## See Also: Initcall Levels Detail

See [do_initcalls/README.md](do_initcalls/README.md) for the full list of what runs at each level.

## `driver_init()`

Before `do_initcalls()`, `driver_init()` sets up the driver model infrastructure:

```c
void __init driver_init(void)
{
    devtmpfs_init();    // Early /dev filesystem
    devices_init();     // /sys/devices
    buses_init();       // /sys/bus
    classes_init();     // /sys/class
    firmware_init();    // /sys/firmware
    hypervisor_init();  // /sys/hypervisor
    platform_bus_init();// Platform devices
    
    cpu_dev_init();     // /sys/devices/system/cpu
    memory_dev_init();  // /sys/devices/system/memory
    container_dev_init();
}
```

This creates the sysfs hierarchy that all device drivers populate.

## What Runs at Level 6 (device_initcall)

This is where most device drivers initialize:

- PCI bus scan
- USB host controller drivers
- Network interface card drivers (e1000, ixgbe, r8169, ...)
- SCSI subsystem
- NVMe driver
- Sound drivers (ALSA)
- GPU drivers (i915, amdgpu, nouveau)
- Filesystem registration (ext4, xfs, btrfs, ...)

## Cross-references

- [Parent: kernel_init_freeable](../README.md)
- [do_initcalls/](do_initcalls/README.md) — detailed level-by-level breakdown
