# `prepare_namespace()` — Mount Root Filesystem

## Purpose

Mounts the root filesystem, transitioning from the in-memory initramfs (temporary root) to the real on-disk root filesystem. After this function, the kernel can find `/sbin/init` and launch it.

## Source File

`init/do_mounts.c`

```c
void __init prepare_namespace(void)
{
    if (root_delay) {
        printk(KERN_INFO "Waiting %d sec before mounting root device...\n",
               root_delay);
        ssleep(root_delay);
    }
    
    // Wait for all devices to settle (NVMe, USB may be slow):
    wait_for_device_probe();
    md_run_setup();         // RAID autodetect
    
    // Mount root filesystem:
    if (saved_root_name[0]) {
        root_device_name = saved_root_name;
        // Parse root= cmdline parameter
    }
    
    if (initrd_load())      // Try initrd (legacy)
        goto out;
    
    mount_root();           // Mount the real root
    
out:
    devtmpfs_mount();
    init_mount(".", "/", NULL, MS_MOVE, NULL);
    init_chroot(".");
}
```

## Boot Methods

### Method 1: initramfs (Modern, Recommended)

```
bootloader
    → loads kernel + initramfs image into RAM
    → kernel boots with initramfs as initial rootfs
    → kernel_init_freeable() unpacks initramfs (populate_rootfs)
    → PID 1 starts: /init (from initramfs)
    
/init script:
    → loads modules (SCSI, NVMe, crypto for dm-crypt, etc.)
    → assembles md-raid if needed
    → unlocks dm-crypt if needed
    → mounts real root at /new_root
    → pivot_root or switch_root to /new_root
    → exec /sbin/init on real root
```

### Method 2: Legacy initrd (older)

```
/initrd.img → loaded to RAM by bootloader
→ kernel mounts as /dev/ram0
→ kernel runs /linuxrc
→ linuxrc mounts real root at /newroot
→ kernel pivots to /newroot
```

### Method 3: Direct mount (embedded, simple)

```
root=/dev/sda1 rootfstype=ext4
→ kernel mounts ext4 directly
→ no initramfs needed
→ all drivers must be built-in (not modules)
```

## `root=` Command Line

```bash
# By device name:
root=/dev/sda1

# By UUID (preferred — stable across device renaming):
root=UUID=550e8400-e29b-41d4-a716-446655440000

# By label:
root=LABEL=rootfs

# Network root (NFS):
root=/dev/nfs nfsroot=192.168.1.1:/export/root
```

## `switch_root` vs `pivot_root`

| | `pivot_root` | `switch_root` |
|--|-------------|---------------|
| Old root | Accessible at old_root | Deleted/freed |
| /proc, /sys | Still mounted | Unmounted first |
| Memory | Old initramfs stays in RAM | Old initramfs freed |
| Use case | Systems that need old root | Typical initramfs |

Modern initramfs uses `switch_root` (busybox) or `systemd-switch-root` to free the initramfs memory after mounting the real root.

## Cross-references

- [Parent: kernel_init_freeable](../README.md)
