# `do_initcalls()` — Initcall Levels 0–7

## Purpose

Executes all registered `__initcall` functions in priority order (levels 0–7). Each level corresponds to a linker section in the kernel binary.

## Linker Sections

The kernel binary contains these sections for initcalls:

```
.initcallearly.init      ← early_initcall()     level: early
.initcall0.init          ← pure_initcall()       level: 0
.initcall1.init          ← core_initcall()       level: 1
.initcall1s.init         ← core_initcall_sync()  level: 1s
.initcall2.init          ← postcore_initcall()   level: 2
.initcall2s.init         ← postcore_initcall_sync()
.initcall3.init          ← arch_initcall()       level: 3
.initcall3s.init
.initcall4.init          ← subsys_initcall()     level: 4
.initcall4s.init
.initcall5.init          ← fs_initcall()         level: 5
.initcall5s.init
.initcallrootfs.init     ← rootfs_initcall()     level: rootfs
.initcall6.init          ← device_initcall()     level: 6
.initcall6s.init
.initcall7.init          ← late_initcall()       level: 7
.initcall7s.init
```

## Level-by-Level Breakdown

### Level early: `early_initcall()`
Runs before SMP and before `do_basic_setup()` (in `do_pre_smp_initcalls()`).
Examples: boot parameters, architecture-specific early setup.

### Level 0: `pure_initcall()` / `core_initcall()`
Core kernel infrastructure.
Examples: `init_workqueues`, `gpiolib_dev_init`, `net_inuse_init`

### Level 1: `core_initcall()`
Core kernel initialization.
Examples: memory notifiers, cpu hotplug infrastructure

### Level 2: `postcore_initcall()`
After core initialization.
Examples: `sock_init` (socket layer), `misc_init` (misc devices)

### Level 3: `arch_initcall()`
Architecture-specific initialization.
Examples: IOAPIC setup completion, TSC synchronization verification

### Level 4: `subsys_initcall()`
Kernel subsystems.
Examples:
- `pci_driver_init()` — PCI bus driver
- `usb_init()` — USB core
- `scsi_init()` — SCSI layer
- `crypto_init()` — Crypto API
- `ksysfs_init()` — /sys/kernel

### Level 5: `fs_initcall()`
Filesystems.
Examples:
- `ext4_init_fs()` — ext4 registration
- `xfs_init()` — XFS registration
- `tmpfs_init()` — tmpfs registration
- `nfs_init()` — NFS client

### Level rootfs: `rootfs_initcall()`
Root filesystem population (initramfs extraction).
Examples:
- `populate_rootfs()` — unpack initramfs/initrd to rootfs

### Level 6: `device_initcall()` / `module_init()`
Device drivers. This is the largest level:
- NIC drivers (e1000, igb, r8169, bnxt_en, ...)
- Storage drivers (ahci, nvme, virtio_blk, ...)
- GPU drivers (i915, amdgpu, radeon, nouveau, ...)
- USB device drivers
- Sound drivers (snd-hda-intel, ...)
- Platform drivers

### Level 7: `late_initcall()`
Final initialization, after all devices are ready.
Examples:
- `init_soundcore_late()` — sound system finalization
- `fbcon_init()` — framebuffer console
- `ib_register_client()` — InfiniBand

## Debugging initcalls

```bash
# Show all initcall timing:
initcall_debug

# Boot output shows:
# calling  e1000_init_module+0x0/0x18 @ 1
# initcall e1000_init_module+0x0/0x18 returned 0 after 45 usecs
```

## Cross-references

- [Parent: do_basic_setup](../README.md)
