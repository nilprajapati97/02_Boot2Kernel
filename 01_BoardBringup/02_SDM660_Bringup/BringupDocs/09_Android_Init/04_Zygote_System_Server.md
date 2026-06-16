# Zygote & System Server

## Overview

**Zygote** is the parent process for all Android application processes. It pre-loads common Java classes and resources, then forks to create each app process efficiently. **System Server** is the first process forked from Zygote, containing all core Android framework services.

---

## Zygote Boot Flow

```
Init starts Zygote:
────────────────────
1. init.rc → service zygote /system/bin/app_process64 -Xzygote
   └── app_process creates ART (Android Runtime) VM

2. Zygote initialization:
   ├── Start ART VM (dalvik.vm.* properties)
   ├── Preload Java classes (~8000 classes)
   ├── Preload shared resources (drawables, layouts)
   ├── Preload shared libraries (OpenGL, ICU, etc.)
   ├── Open /dev/binder (Binder driver)
   └── Listen on zygote socket (/dev/socket/zygote)

3. Fork System Server:
   └── Zygote.forkSystemServer()
       ├── fork() → child becomes system_server
       └── Parent (Zygote) continues listening

4. Main loop:
   └── Zygote waits for connections on socket
       ├── ActivityManager sends fork request
       ├── Zygote.fork() → creates app process
       └── App process inherits preloaded classes (fast start)
```

---

## System Server Startup

```
system_server process (forked from Zygote):
───────────────────────────────────────────
SystemServer.main()
  └── SystemServer.run()
      ├── Looper.prepareMainLooper()
      ├── createSystemContext()
      │
      ├── startBootstrapServices():
      │   ├── ActivityManagerService (AMS)
      │   ├── PowerManagerService
      │   ├── PackageManagerService (PMS)
      │   ├── DisplayManagerService
      │   ├── SensorService ← connects to Sensor HAL (BMI160)
      │   └── UserManagerService
      │
      ├── startCoreServices():
      │   ├── BatteryService
      │   ├── UsageStatsService
      │   └── WebViewUpdateService
      │
      ├── startOtherServices():
      │   ├── WindowManagerService (WMS)
      │   ├── InputManagerService
      │   ├── ConnectivityService
      │   ├── TelephonyRegistry
      │   ├── AudioService ← connects to Audio HAL (ADSP)
      │   ├── CameraService ← connects to Camera HAL
      │   ├── LocationManagerService
      │   ├── NotificationManagerService
      │   └── ... (~100+ services)
      │
      ├── AMS.systemReady():
      │   ├── Start launcher/home app
      │   ├── Send ACTION_BOOT_COMPLETED broadcast
      │   └── Set sys.boot_completed=1
      │
      └── Looper.loop() (infinite event loop)
```

---

## SensorService Connection to BMI160

```
SensorService startup (in system_server):
──────────────────────────────────────────
1. SensorService.onFirstRef()
2. SensorDevice::getInstance()
   └── Opens HIDL connection to Sensor HAL
3. getSensorList() → HAL returns available sensors:
   ├── BMI160 Accelerometer (type=1)
   ├── BMI160 Gyroscope (type=4)
   ├── Gravity (virtual, derived from accel)
   ├── Linear Acceleration (virtual)
   └── Step Counter (virtual, derived from accel)
4. Apps register sensor listeners via SensorManager
5. SensorService routes requests to HAL
6. HAL → SLPI (or kernel IIO driver) → BMI160
```

---

## Zygote Process Forking

```
App launch:
───────────
1. User taps app icon
2. ActivityManagerService.startActivity()
3. AMS → Process.start() → sends command to Zygote socket
4. Zygote receives fork request
5. Zygote.fork():
   ├── Child: executes app's Application.onCreate()
   │          (inherits preloaded classes = fast startup)
   └── Parent: continues listening
6. App process registers with AMS
7. AMS schedules Activity launch in app process
```

---

## Related Documents

- [01_Init_Process.md](01_Init_Process.md) — Init starts Zygote
- [03_HAL_Services.md](03_HAL_Services.md) — HAL services connected by System Server
- [05_Boot_Complete.md](05_Boot_Complete.md) — Final boot completion
