<div align="center">

<div style="background: linear-gradient(135deg, #FF4500, #3A1500); padding: 40px 40px; border-radius: 16px; margin-bottom: 20px; box-shadow: 0 4px 15px rgba(0,0,0,0.3);">

<h1 style="color: white; margin: 0; font-size: 2.6em; letter-spacing: 1px;">🐧 Boot2Kernel</h1>

<p style="color: rgba(255,255,255,0.90); margin: 14px 0 0 0; font-size: 1.2em;">From power-on to userspace — a hands-on journey through the embedded Linux stack</p>

</div>

![Topic](https://img.shields.io/badge/Topic-Embedded%20Linux-FF4500?style=flat-square)
![Arch](https://img.shields.io/badge/Arch-ARM64%20%2F%20ARMv8-6A5ACD?style=flat-square)
![Layer](https://img.shields.io/badge/Layer-Bootloader%20→%20Kernel%20→%20Drivers-2E8B57?style=flat-square)
![Sections](https://img.shields.io/badge/Sections-14-2C7BE5?style=flat-square)
![Docs](https://img.shields.io/badge/Format-Markdown%20%2B%20Patches-555?style=flat-square)

</div>

---

## 📖 Overview

**Boot2Kernel** is a structured knowledge base and lab notebook for **embedded Linux** and the **Linux kernel internals**, following the real-world path a system takes from the first instruction after reset all the way up to a running root filesystem.

It is organized as the boot-and-build pipeline an engineer actually walks through:

```
Power-On ──▶ Bootloader (U-Boot) ──▶ Board Bringup / BSP ──▶ Device Tree
   ──▶ Kernel (Memory, Threads, Interrupts, sysfs) ──▶ Drivers ──▶ Userspace
```

Each section is self-contained, cross-referenced, and pairs **conceptual notes** with **practical artifacts** — kernel patches (`.patch`), code snippets (`.c`), reference PDFs, bringup logs, and interview-style Q&A.

---

## 🗺️ The Boot-to-Kernel Pipeline

| Stage | What Happens | Section |
|:---:|:---|:---|
| 1️⃣ | CPU resets, ROM hands off to the bootloader | [02_UBoot](02_UBoot/README.md) |
| 2️⃣ | Bootloader initializes RAM, clocks, peripherals and loads the kernel | [01_BoardBringup](01_BoardBringup/README.md) · [03_BSP](03_BSP/README.md) |
| 3️⃣ | Kernel discovers hardware via the device tree | [05_DeviceTree](05_DeviceTree/README.md) |
| 4️⃣ | Core kernel subsystems come alive — memory, scheduling, IRQs | [06_MemoryManagement](06_MemoryManagement) · [07_KernelThread](07_KernelThread/README.md) · [08_Interrupt](08_Interrupt) |
| 5️⃣ | Drivers bind to devices and expose interfaces to userspace | [04_Drivers](04_Drivers/README.md) · [09_SysFiles](09_SysFiles/README.md) |
| 6️⃣ | Debug, secure, and integrate the running system | [11_Linux_Kernel_Debug](11_Linux_Kernel_Debug/README.md) · [12_LinuxSecurity](12_LinuxSecurity/README.md) · [13_Protocols](13_Protocols/README.md) |

---

## 📂 Sections

<h3 style="color: #FF4500;">🥾 Boot & Board</h3>

| | Section | Description |
|:---:|:---|:---|
| 📚 | **[00_Reference](00_Reference)** | Foundational reading — *Understanding the Linux Kernel* & kernel development notes |
| 🔌 | **[01_BoardBringup](01_BoardBringup/README.md)** | Board bringup process — bootloader, kernel, rootfs, peripherals (incl. SDM660) |
| 🥾 | **[02_UBoot](02_UBoot/README.md)** | U-Boot bootloader deep-dive — boot flow, image formats, boot-time optimization |
| 📦 | **[03_BSP](03_BSP/README.md)** | Board Support Package development — bootloader, reuse flow, compilation timings |

<h3 style="color: #FF4500;">🧩 Kernel Core</h3>

| | Section | Description |
|:---:|:---|:---|
| 🌳 | **[05_DeviceTree](05_DeviceTree/README.md)** | Device tree — DTS/DTB, bindings, overlays, kernel parsing |
| 🧠 | **[06_MemoryManagement](06_MemoryManagement)** | Memory management — ARMv8/ARM64 model, allocation APIs, paging, common issues |
| 🧵 | **[07_KernelThread](07_KernelThread/README.md)** | Kernel threads and synchronization — spinlocks, mutexes, semaphores, RCU |
| ⚡ | **[08_Interrupt](08_Interrupt)** | Interrupt handling — top/bottom half, GIC, softirq, tasklets, workqueues, threaded IRQ |
| 📂 | **[09_SysFiles](09_SysFiles/README.md)** | sysfs and procfs virtual filesystem entries, wait queues |

<h3 style="color: #FF4500;">🔧 Drivers, Tooling & Beyond</h3>

| | Section | Description |
|:---:|:---|:---|
| 🔌 | **[04_Drivers](04_Drivers/README.md)** | Linux device drivers — char, network, platform, GPIO, I2C |
| 🔧 | **[10_ToolChain](10_ToolChain/README.md)** | Cross-compilation toolchain setup — Buildroot, project lifecycle |
| 🐛 | **[11_Linux_Kernel_Debug](11_Linux_Kernel_Debug/README.md)** | Kernel debugging — JTAG, kdump, RAM dump, panic/OOPS, crash utility |
| 🛡️ | **[12_LinuxSecurity](12_LinuxSecurity/README.md)** | Linux security — LSM, memory protection, FS integrity, networking security |
| 📡 | **[13_Protocols](13_Protocols/README.md)** | Industrial protocols — J1939, Modbus |
| 🔬 | **[Explore](Explore/README.md)** | Advanced exploration — cache coherence, RTOS, embedded C, interview prep |

> 📄 See also: **[SystemCall_Interface.Md](SystemCall_Interface.Md)** — the kernel/userspace boundary.

---

## 🛠️ Tech Stack & Focus Areas

<div align="center">

![Linux Kernel](https://img.shields.io/badge/Linux%20Kernel-Internals-FCC624?style=for-the-badge&logo=linux&logoColor=black)
![ARM64](https://img.shields.io/badge/ARMv8-AArch64-0091BD?style=for-the-badge&logo=arm&logoColor=white)
![U-Boot](https://img.shields.io/badge/U--Boot-Bootloader-EE0000?style=for-the-badge)
![Device Tree](https://img.shields.io/badge/Device%20Tree-DTS%2FDTB-2E8B57?style=for-the-badge)
![C](https://img.shields.io/badge/C-Kernel%20Space-A8B9CC?style=for-the-badge&logo=c&logoColor=black)

</div>

- **Architecture:** ARMv8 / ARM64 (AArch64), including SoC bringup (e.g. SDM660)
- **Boot chain:** ROM → U-Boot → Kernel → init / rootfs
- **Kernel subsystems:** memory management, scheduling & threads, interrupts (GIC), synchronization, sysfs/procfs
- **Driver model:** character, platform, network, GPIO, I2C peripheral drivers
- **Build & debug:** Buildroot toolchains, kdump, crash analysis, JTAG
- **Security & protocols:** LSM, memory protection, J1939, Modbus

---

## 🧭 How to Use This Repository

1. **New to the stack?** Start at **[00_Reference](00_Reference)**, then follow the pipeline top-to-bottom (`01` → `13`).
2. **Targeting a topic?** Jump straight into any section folder — each has its own styled `README.md` index.
3. **Want hands-on material?** Look for `.patch`, `.c`, `.md`, and bringup-log files inside the section subdirectories.
4. **Interview prep?** Many sections include dedicated **Questions** notes; see also **[Explore](Explore/README.md)**.

---

<div align="center">

<sub>🐧 Crafted as a deep-dive companion for embedded Linux & kernel engineers — boot it up, dig into the kernel.</sub>

</div>
