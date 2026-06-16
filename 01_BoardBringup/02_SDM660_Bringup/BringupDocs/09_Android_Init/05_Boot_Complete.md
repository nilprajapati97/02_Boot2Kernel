# Boot Complete — End-to-End Summary

## Overview

This document covers the final boot stages and provides an end-to-end timing summary of the entire SDM660 boot sequence from power-on to Android home screen.

---

## Final Boot Steps

```
After System Server starts all services:
─────────────────────────────────────────
1. ActivityManagerService.systemReady()
   ├── Start Launcher (home screen app)
   ├── Restore persistent apps
   └── Send boot phase notifications

2. Launcher starts:
   ├── Load app icons
   ├── Inflate widgets
   └── Display home screen

3. Boot animation stops:
   ├── SurfaceFlinger.bootFinished()
   └── Stop bootanim service

4. ACTION_BOOT_COMPLETED broadcast:
   ├── Apps receive broadcast
   ├── Start background services
   └── Alarm restoration

5. sys.boot_completed=1:
   ├── init.qcom.post_boot.sh executes
   ├── CPU governor tuning
   ├── I/O scheduler adjustment
   └── Memory management tuning

6. Boot complete!
```

---

## End-to-End Boot Timeline

```
Time (ms)   Stage                           Duration
─────────────────────────────────────────────────────
     0      Power button pressed
    50      PMIC ramp-up complete            50 ms
   100      PBL starts (ROM)                 50 ms
   400      PBL → XBL handoff              300 ms
   500      XBL early init (DDR training)   100 ms
   900      XBL main init (clocks, PMIC)    400 ms
  1200      TrustZone init                  300 ms
  1500      RPM firmware loaded             300 ms
  1800      ABL/LK starts                   300 ms
  2000      ABL loads kernel + DTB          200 ms
  2300      Kernel decompression + early boot 300 ms
  2800      Kernel device probe (clocks,     500 ms
            pinctrl, regulators, I2C)
  3500      PIL loads subsystems             700 ms
            (modem, ADSP, CDSP, SLPI)
  4000      Init first stage                 500 ms
            (mount, SELinux)
  4500      Init second stage                500 ms
            (parse .rc, start services)
  5000      HAL services start               500 ms
  5500      Zygote + System Server          1000 ms
  6500      Framework services ready         1000 ms
  7500      Launcher displayed              1000 ms
  8000      Boot animation stops
  9000      BOOT_COMPLETED broadcast
 10000      Post-boot tuning complete       1000 ms
─────────────────────────────────────────────────────
Total:      ~8-12 seconds (cold boot)

Note: Times are approximate and vary by board,
storage speed, and number of installed apps.
```

---

## Boot Timeline Diagram

```
0s        2s        4s        6s        8s       10s
│─────────│─────────│─────────│─────────│─────────│
├─PBL/XBL─┤                                      
│          ├─ABL/LK─┤                              
│          │         ├─Kernel──┤                    
│          │         │         ├─Init/HAL┤          
│          │         │         │         ├─Zygote──┤
│          │         │         │         │         ├─Home
│          │         │                              
│          │         ├─PIL: Modem, ADSP, CDSP, SLPI─┤
│          │                                        
│    ├─RPM FW─────────────────────────────────────── (runs continuously)
│    ├─TZ─────────────────────────────────────────── (runs continuously)
```

---

## Boot Optimization Checklist

| Area | Optimization | Impact |
|------|-------------|--------|
| XBL | Pre-trained DDR parameters | -200 ms |
| ABL | Skip boot animation render | -100 ms |
| Kernel | Deferred probe for non-critical | -300 ms |
| Kernel | Async probe for I2C/SPI | -200 ms |
| PIL | Parallel subsystem loading | -500 ms |
| Init | Minimize .rc trigger chains | -200 ms |
| Zygote | Profile-guided class preload | -300 ms |
| Apps | Reduce boot_completed receivers | -500 ms |

---

## Debugging Boot Time

```bash
# Kernel boot time breakdown
adb shell dmesg | grep "initcall.*took"

# Android boot stages
adb shell cat /proc/bootprof     # MediaTek
adb logcat -b events | grep "boot"

# Boot time properties
adb shell getprop ro.boottime.init
adb shell getprop ro.boottime.SurfaceFlinger
adb shell getprop ro.boottime.zygote
adb shell getprop ro.boottime.system_server

# Bootchart (visual)
adb shell touch /data/bootchart/enabled
# Reboot, then collect:
adb shell tar -czf /data/bootchart.tgz /data/bootchart/
adb pull /data/bootchart.tgz
# Process with bootchart tool

# Systrace during boot
python systrace.py --boot -o boot_trace.html \
    sched freq idle am wm
```

---

## Complete Boot Chain Summary

```
┌─────────────────────────────────────────────────────────────┐
│                SDM660 Complete Boot Chain                     │
│                                                             │
│  ┌─────┐   ┌─────┐   ┌─────┐   ┌─────┐   ┌─────────────┐ │
│  │ PBL │──▶│ XBL │──▶│ RPM │──▶│ ABL │──▶│ Linux Kernel│ │
│  │(ROM)│   │(SBL)│   │(FW) │   │(LK) │   │   (4.4)     │ │
│  └─────┘   └──┬──┘   └─────┘   └─────┘   └──────┬──────┘ │
│               │                                    │        │
│               ▼                                    ▼        │
│           TrustZone                           ┌────────┐   │
│           (EL3)                               │  Init  │   │
│                                               │ (PID1) │   │
│                                               └───┬────┘   │
│                                                   │        │
│                                    ┌──────────────┼─────┐  │
│                                    ▼              ▼     ▼  │
│                               ┌───────┐    ┌─────┐ ┌────┐ │
│                               │Zygote │    │HALs │ │QCOM│ │
│                               └───┬───┘    │     │ │svcs│ │
│                                   │        └─────┘ └────┘ │
│                                   ▼                        │
│                             ┌──────────┐                   │
│                             │System    │                   │
│                             │Server    │                   │
│                             └────┬─────┘                   │
│                                  │                         │
│                                  ▼                         │
│                            ┌──────────┐                    │
│                            │ Launcher │                    │
│                            │(Home App)│                    │
│                            └──────────┘                    │
│                                                            │
│  BMI160 Sensor Path:                                       │
│  BMI160 ─I2C─▶ SLPI ─QMI─▶ Sensor HAL ─▶ SensorService  │
│  (0x68)  Bus3  (PIL)  GLINK  (HIDL)        (System Server)│
└─────────────────────────────────────────────────────────────┘
```

---

## Related Documents

- [01_Init_Process.md](01_Init_Process.md) — Init process details
- [04_Zygote_System_Server.md](04_Zygote_System_Server.md) — Framework startup
- [../../README.md](../../README.md) — SDM660 overview
