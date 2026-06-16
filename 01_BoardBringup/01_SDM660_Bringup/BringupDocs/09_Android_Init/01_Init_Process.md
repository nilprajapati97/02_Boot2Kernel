# Android Init Process

## Overview

**init** is the first userspace process (PID 1) spawned by the Linux kernel after mounting the root filesystem. On SDM660 running Android 9 (Pie), init reads `.rc` (init script) files and orchestrates the entire userspace bring-up: mounting filesystems, starting services, setting permissions, and launching Zygote.

---

## Kernel → Init Transition

```
Kernel Boot Completion:
──────────────────────
1. Kernel finishes hardware initialization
2. Mounts rootfs (from ramdisk or system-as-root)
3. Executes /init (Android init binary)
   └── Determined by kernel cmdline: init=/init

Init Binary Location:
  Android 9 (SDM660): /init in ramdisk (boot.img)
  OR system-as-root: /system/bin/init
```

---

## Init Stages

```
┌──────────────────────────────────────────────────────┐
│  Stage 1: First Stage Init                            │
│  ├── Mount /dev, /proc, /sys                         │
│  ├── Mount /system, /vendor, /odm partitions         │
│  ├── Initialize SELinux (load policy)                │
│  └── Re-exec init with SELinux context               │
├──────────────────────────────────────────────────────┤
│  Stage 2: Second Stage Init                           │
│  ├── Initialize property service                     │
│  ├── Parse .rc files:                                │
│  │   ├── /init.rc (core Android)                     │
│  │   ├── /init.qcom.rc (QCOM platform)              │
│  │   ├── /init.target.rc (target board)              │
│  │   └── /vendor/etc/init/ (vendor services)         │
│  ├── Execute early-init triggers                     │
│  ├── Execute init triggers                           │
│  ├── Execute late-init triggers                      │
│  └── Enter main event loop                           │
├──────────────────────────────────────────────────────┤
│  Stage 3: Event Loop                                  │
│  ├── Monitor child processes (restart on crash)      │
│  ├── Handle property changes                         │
│  ├── Process keychord events                         │
│  └── Run until shutdown                              │
└──────────────────────────────────────────────────────┘
```

---

## Init .rc File Syntax

```
# Action triggers
on <trigger>
    <command>
    <command>

# Service definition
service <name> <pathname> [<argument>]*
    <option>
    <option>

# Example from init.rc:
on init
    mkdir /dev/stune
    mount cgroup none /dev/stune schedtune

on boot
    chown system system /sys/class/leds/lcd-backlight/brightness
    chmod 0660 /sys/class/leds/lcd-backlight/brightness
    
    # Start core services
    start servicemanager
    start hwservicemanager
    start vndservicemanager

service surfaceflinger /system/bin/surfaceflinger
    class core animation
    user system
    group graphics drmrpc readproc
    onrestart restart zygote
    writepid /dev/stune/foreground/tasks

service zygote /system/bin/app_process64 -Xzygote /system/bin \
        --zygote --start-system-server
    class main
    socket zygote stream 660 root system
    socket usap_pool_primary stream 660 root system
```

---

## Init Trigger Order

```
Execution order of triggers:
───────────────────────────
1. early-init
   └── Start ueventd (device node creation)

2. init
   └── Mount filesystems, create directories

3. late-init
   ├── trigger early-fs
   ├── trigger fs         → Mount /system, /vendor, /data
   ├── trigger post-fs    → Post-mount setup
   ├── trigger late-fs    
   ├── trigger post-fs-data → /data setup (encryption)
   ├── trigger zygote-start → Start Zygote
   └── trigger boot       → Final boot actions

4. boot
   ├── Start services (class main)
   ├── Set properties
   └── Start QCOM services
```

---

## Key Directories Created by Init

| Path | Purpose |
|------|---------|
| /dev | Device nodes (created by ueventd) |
| /proc | Kernel proc filesystem |
| /sys | Kernel sysfs |
| /system | System partition (read-only) |
| /vendor | Vendor partition (QCOM BSP) |
| /data | User data partition (encrypted) |
| /mnt | Mount points |
| /dev/socket | Unix domain sockets |

---

## Related Documents

- [02_QCOM_Init_Scripts.md](02_QCOM_Init_Scripts.md) — Qualcomm-specific init scripts
- [03_HAL_Services.md](03_HAL_Services.md) — Hardware abstraction services
- [04_Zygote_System_Server.md](04_Zygote_System_Server.md) — Java world startup
