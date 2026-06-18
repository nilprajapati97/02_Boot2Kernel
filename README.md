<div align="center">

<div style="background: linear-gradient(135deg, #FF4500, #3A1500); padding: 30px 40px; border-radius: 16px; margin-bottom: 20px; box-shadow: 0 4px 15px rgba(0,0,0,0.3);">

<h1 style="color: white; margin: 0; font-size: 2.2em; letter-spacing: 1px;">🐧 Boot2Kernel</h1>

<p style="color: rgba(255,255,255,0.85); margin: 10px 0 0 0; font-size: 1.1em;">Embedded Linux — from board bring-up to kernel internals</p>

</div>

</div>

![Topic](https://img.shields.io/badge/Topic-Embedded%20Linux-FF4500?style=flat-square) ![Sections](https://img.shields.io/badge/Sections-14-6A5ACD?style=flat-square) ![Format](https://img.shields.io/badge/Format-Markdown-2E8B57?style=flat-square)

---

## 📖 About

**Boot2Kernel** is a curated knowledge base that walks through the full embedded
Linux stack — starting from powering up a custom board, through the bootloader and
BSP, and down into the Linux kernel internals (drivers, memory management, threads,
interrupts, debugging, and security). It is organised as a set of topic directories,
each containing notes, diagrams, code snippets, and interview-style questions.

Every section has its own `README.md` with more detail; use the table below to
navigate.

---

## 🗂️ Table of Contents

| | Section | Description |
|:---:|:---|:---|
| 🛠️ | **[01_BoardBringup](01_BoardBringup/)** | Custom board bring-up flows and templates |
| 🚀 | **[02_UBoot](02_UBoot/)** | U-Boot, boot images, bootloader and boot optimization |
| 📦 | **[03_BSP](03_BSP/README.md)** | Board Support Package topics, reuse flow and timings |
| 🔌 | **[04_Drivers](04_Drivers/README.md)** | Char, network, platform, GPIO and I2C device drivers |
| 🌲 | **[05_DeviceTree](05_DeviceTree/)** | Device tree concepts, parsing and kernel integration |
| 🧠 | **[06_MemoryManagement](06_MemoryManagement/)** | ARM64 memory architecture, APIs, allocation internals |
| 🧵 | **[07_KernelThread](07_KernelThread/README.md)** | Kernel threads and synchronization primitives |
| ⚡ | **[08_Interrupt](08_Interrupt/README.md)** | Interrupt handling, GIC, ARM64 internals and nesting |
| 📁 | **[09_SysFiles](09_SysFiles/README.md)** | SysFS, wait queues and related kernel files |
| 🧰 | **[10_ToolChain](10_ToolChain/README.md)** | Toolchains, Buildroot and project lifecycle |
| 🐛 | **[11_Linux_Kernel_Debug](11_Linux_Kernel_Debug/README.md)** | Kernel debugging — JTAG, kdump, RAM dump, crash utility |
| 🔒 | **[12_LinuxSecurity](12_LinuxSecurity/README.md)** | LSM, memory protection and filesystem integrity |
| 📡 | **[13_Protocols](13_Protocols/README.md)** | Embedded protocols — J1939 and Modbus |
| 🔭 | **[Explore](Explore/)** | Deep-dives, RTOS notes and interview questions |

---

## 🚦 How to Use

1. Pick a section from the table above based on the topic you want to study.
2. Open the section's `README.md` (where present) for an overview and links to its files.
3. Follow the numbered files within each directory in order for a guided reading path.

---

<div align="center">
<sub style="color: #666;">📚 <b style="color: #FF4500;">Boot2Kernel</b> — embedded Linux knowledge base</sub>
</div>
