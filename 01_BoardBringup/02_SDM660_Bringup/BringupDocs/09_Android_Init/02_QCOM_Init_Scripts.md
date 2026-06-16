# Qualcomm Init Scripts

## Overview

Qualcomm's BSP adds platform-specific init scripts that configure hardware, start proprietary services, and set up subsystem communication. These scripts extend Android's base `init.rc` with QCOM-specific triggers and services.

---

## QCOM Init Script Files

```
/                                    (ramdisk)
├── init.qcom.rc                     Main QCOM init script
├── init.qcom.sh                     Shell script (called from init.qcom.rc)
├── init.target.rc                   Board/target-specific init
├── init.qcom.usb.rc                 USB configuration
├── init.qcom.usb.sh                 USB mode script
└── init.qcom.post_boot.sh           Post-boot tuning (CPU governor, scheduler)

/vendor/etc/init/                    (vendor partition)
├── hw/
│   ├── android.hardware.sensors@1.0-service.rc
│   ├── android.hardware.audio@4.0-service.rc
│   └── ...
├── init.qcom.sensors.sh             Sensor subsystem init
├── init.qti.qseecomd.rc            TrustZone daemon
└── ...
```

---

## init.qcom.rc — Key Sections

```
# Subsystem loading triggers
on property:sys.boot_completed=1
    # Post-boot tuning
    start qcom-post-boot
    
on boot
    # Subsystem restart level
    write /sys/bus/msm_subsys/devices/subsys0/restart_level related
    write /sys/bus/msm_subsys/devices/subsys1/restart_level related
    write /sys/bus/msm_subsys/devices/subsys2/restart_level related
    
    # QCOM RPS/XPS tuning
    write /proc/sys/net/core/rps_sock_flow_entries 3072
    
    # Sensor permissions
    chown system system /sys/class/sensors/
    chmod 0775 /sys/class/sensors/
    
# Subsystem loader service
service subsystem_ramdump /vendor/bin/subsystem_ramdump
    class late_start
    disabled
    user system
    group system
    
# IPC Router
service irsc_util /vendor/bin/irsc_util "/vendor/etc/sec_config"
    class core
    user root
    oneshot

# QMI / QMUX daemon
service qmuxd /vendor/bin/qmuxd
    class main
    user root
    group radio audio bluetooth gps diag

# Time daemon
service time_daemon /vendor/bin/time_daemon
    class core
    user system
    group system
```

---

## init.qcom.post_boot.sh

This script runs after boot completes and tunes CPU/GPU performance:

```bash
#!/vendor/bin/sh

# SDM660 post-boot tuning

# Big cluster (Gold cores 4-7)
echo "schedutil" > /sys/devices/system/cpu/cpu4/cpufreq/scaling_governor
echo 1401600 > /sys/devices/system/cpu/cpu4/cpufreq/scaling_min_freq
echo 2208000 > /sys/devices/system/cpu/cpu4/cpufreq/scaling_max_freq

# Little cluster (Silver cores 0-3)
echo "schedutil" > /sys/devices/system/cpu/cpu0/cpufreq/scaling_governor
echo 633600 > /sys/devices/system/cpu/cpu0/cpufreq/scaling_min_freq
echo 1843200 > /sys/devices/system/cpu/cpu0/cpufreq/scaling_max_freq

# I/O scheduler
echo "cfq" > /sys/block/sda/queue/scheduler
echo 512 > /sys/block/sda/queue/read_ahead_kb

# GPU max frequency
echo 585000000 > /sys/class/kgsl/kgsl-3d0/devfreq/max_freq

# EAS (Energy Aware Scheduling)
echo 85 > /proc/sys/kernel/sched_upmigrate
echo 85 > /proc/sys/kernel/sched_downmigrate

# Memory management
echo 100 > /proc/sys/vm/swappiness
echo 0 > /proc/sys/vm/page-cluster

echo "Post-boot tuning complete"
```

---

## init.target.rc — Board-Specific

```
# SDM660 MTP/QRD specific configuration
on init
    # Set thermal governors
    write /sys/class/thermal/thermal_zone0/policy step_wise
    
on fs
    # Mount vendor partition
    mount_all /vendor/etc/fstab.qcom
    
    # Start QSEE daemon early (for encryption)
    start qseecomd

on post-fs-data
    # Subsystem firmware directories
    mkdir /data/vendor/tombstones 0771 system system
    mkdir /data/vendor/ssrdump 0771 system system
    
    # Sensor calibration data
    mkdir /persist/sensors 0775 system system
```

---

## QCOM Service Boot Order

```
Init sequence:
──────────────
1. ueventd (device nodes)
2. logd (logging)
3. servicemanager (binder)
4. hwservicemanager (HIDL binder)
5. vndservicemanager (vendor binder)
6. qseecomd (TrustZone daemon)
7. irsc_util (IPC router security config)
8. rmt_storage (remote storage for modem)
9. tftp_server (TFTP for modem/subsystems)
10. subsystem loaders (PIL triggers)
11. qmuxd (QMI multiplexer)
12. netd (network daemon)
13. surfaceflinger (display compositor)
14. zygote (Java VM)
15. system_server (Android framework)
16. qcom-post-boot (performance tuning)
```

---

## Related Documents

- [01_Init_Process.md](01_Init_Process.md) — Base init framework
- [03_HAL_Services.md](03_HAL_Services.md) — HAL services started by init
- [../07_Subsystem_Loading/01_PIL_Framework.md](../07_Subsystem_Loading/01_PIL_Framework.md) — Subsystem loading
