# Chapter 01 — Getting Started with the Linux Kernel

> **Book:** Linux Kernel Development — Robert Love (3rd Edition)
> **Goal:** Set up the development environment, understand the kernel source tree, build the kernel from scratch, and understand how to contribute.

---

## Learning Objectives
- Navigate the kernel source tree confidently
- Configure and build the kernel
- Install a custom-built kernel
- Understand the kernel development community and contribution process

---

## Topic Index

| File | Description |
|------|-------------|
| [01_Kernel_Source_Tree_Layout.md](./01_Kernel_Source_Tree_Layout.md) | Full directory structure explained |
| [02_Building_The_Kernel.md](./02_Building_The_Kernel.md) | Configuration, compilation, cross-compilation |
| [03_Kernel_Configuration.md](./03_Kernel_Configuration.md) | Kconfig, menuconfig, .config file |
| [04_Installing_The_Kernel.md](./04_Installing_The_Kernel.md) | Installing modules, initramfs, GRUB |
| [05_Kernel_Development_Community.md](./05_Kernel_Development_Community.md) | LKML, submitting patches, coding style |

---

## Chapter Flow

```mermaid
flowchart TD
    A[Clone kernel source] --> B[Explore source tree]
    B --> C[Configure: make menuconfig]
    C --> D[Build: make -j\$(nproc\)]
    D --> E[Install modules: make modules_install]
    E --> F[Install kernel: make install]
    F --> G[Update bootloader\nGRUB rebuild]
    G --> H[Boot new kernel]
    H --> I[Develop → Patch → Submit to LKML]
```mermaid
flowchart TD
    Kernel["Linux Kernel Source Tree"] --> Arch["arch/"]
    Kernel --> Drivers["drivers/"]
    Kernel --> Fs["fs/"]
    Kernel --> Include["include/"]
    Kernel --> KernelDir["kernel/"]
    Kernel --> Lib["lib/"]
    Kernel --> Mm["mm/"]
    Kernel --> Tools["tools/"]
    Kernel --> Documentation["Documentation/"]
```
