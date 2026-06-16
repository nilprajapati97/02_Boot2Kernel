# 03 — Kernel Architecture Overview

## 1. Definition

The **kernel** is the core of the operating system. It is the software that manages hardware resources and provides abstractions (processes, files, network) for user programs to use safely and efficiently.

Linux is a **monolithic kernel** — all kernel services run in a single large program in a single address space with full hardware access.

---

## 2. What the Kernel Does

```mermaid
flowchart TD
    HW[Hardware\nCPU · RAM · Disk · NIC · GPU]
    K[Linux Kernel]
    US[User Space Programs\nnginx · bash · python · vim]

    HW --> |Interrupts, DMA, MMIO| K
    K --> |Abstractions: syscalls| US
    US --> |System calls| K
    K --> |reads/writes| HW
```

### Six Core Responsibilities

| Responsibility | Description | Kernel Subsystem |
|----------------|-------------|-----------------|
| **Process Management** | Create, schedule, terminate processes/threads | `kernel/` |
| **Memory Management** | Virtual memory, page allocation, swap | `mm/` |
| **File System** | File I/O, filesystem abstraction (VFS) | `fs/` |
| **Device Drivers** | Communicate with hardware | `drivers/` |
| **Networking** | TCP/IP stack, sockets | `net/` |
| **Security** | Permissions, capabilities, LSM (SELinux) | `security/` |

---

## 3. Kernel Subsystem Architecture

```mermaid
graph TB
    subgraph Kernel["Linux Kernel Space"]
        SCI[System Call Interface]
        
        subgraph Core["Core Kernel"]
            PM[Process\nManagement]
            SCH[Scheduler\nCFS]
            MM[Memory\nManagement]
            IPC[IPC\npipes/signals/sockets]
        end

        subgraph FS["File System Layer"]
            VFS[Virtual File System]
            EXT4[ext4]
            BTRFS[btrfs]
            XFS[xfs]
            TMPFS[tmpfs/proc/sys]
        end

        subgraph Net["Networking"]
            SOCK[Socket Layer]
            TCP[TCP/IP Stack]
            ETH[Ethernet/WiFi]
        end

        subgraph DD["Device Drivers"]
            BLKD[Block Devices\nNVMe/SATA]
            CHRD[Char Devices\nTTY/GPIO]
            NETD[Network Drivers]
        end

        subgraph Arch["Architecture-Specific"]
            x86[x86_64]
            ARM[ARM64]
            RISCV[RISC-V]
        end

        SCI --> Core
        SCI --> FS
        SCI --> Net
        Core --> MM
        FS --> VFS
        VFS --> EXT4 & BTRFS & XFS & TMPFS
        Net --> SOCK --> TCP --> ETH
        Core --> DD
        DD --> Arch
    end

    US[User Space] --> SCI
    Arch --> HW[Hardware]
```

---

## 4. Kernel Source Tree Layout

```
linux/
├── arch/           # Architecture-specific code (x86, arm64, riscv...)
├── block/          # Block layer (bio, request queue, I/O schedulers)
├── crypto/         # Cryptographic API
├── drivers/        # All device drivers (huge — ~60% of kernel)
├── fs/             # Filesystems (ext4, btrfs, xfs, vfs core...)
├── include/        # Kernel headers
│   ├── linux/      # Core kernel headers
│   └── uapi/       # User-space API headers
├── init/           # Kernel init (main.c — start_kernel())
├── ipc/            # IPC: pipes, semaphores, shared memory
├── kernel/         # Core kernel: scheduler, fork, signal, time...
├── lib/            # General-purpose library functions
├── mm/             # Memory management
├── net/            # Networking stack
├── security/       # LSM framework: SELinux, AppArmor, seccomp
├── sound/          # Audio subsystem (ALSA)
├── tools/          # User-space tools for kernel development
├── Documentation/  # Kernel documentation
├── Makefile        # Top-level build file
└── Kconfig         # Configuration system root
```

---

## 5. Kernel Boot Flow

```mermaid
sequenceDiagram
    participant BIOS as BIOS/UEFI
    participant Boot as Bootloader (GRUB2)
    participant Decomp as Kernel Decompressor
    participant Arch as arch/x86/boot/
    participant Init as init/main.c
    participant PID1 as /sbin/init (PID 1)

    BIOS->>Boot: Power-on, load bootloader from disk
    Boot->>Decomp: Load bzImage into RAM, jump to entry
    Decomp->>Arch: Decompress kernel, jump to startup_32/startup_64
    Arch->>Arch: Setup GDT, IDT, paging, CPU features
    Arch->>Init: Call start_kernel()
    Init->>Init: setup_arch(), mm_init(), sched_init()
    Init->>Init: init_IRQ(), time_init(), softirq_init()
    Init->>Init: rest_init() — creates kernel threads
    Init->>PID1: kernel_execve("/sbin/init")
    PID1->>PID1: systemd/sysvinit takes over
```

### Key Functions in Boot
| Function | File | What it does |
|----------|------|-------------|
| `start_kernel()` | `init/main.c` | Main kernel entry point |
| `setup_arch()` | `arch/x86/kernel/setup.c` | Architecture initialization |
| `mm_init()` | `init/main.c` | Initialize memory management |
| `sched_init()` | `kernel/sched/core.c` | Initialize the scheduler |
| `rest_init()` | `init/main.c` | Create kernel threads, hand off to init |

---

## 6. User Space vs Kernel Space (Deep Dive)

```mermaid
flowchart LR
    subgraph User_Space["User Space (Unprivileged)"]
        P1[Process A\nVirtual Addr 0x0000...]
        P2[Process B\nVirtual Addr 0x0000...]
        P3[Process C\nVirtual Addr 0x0000...]
    end

    subgraph Kernel_Space["Kernel Space (Privileged — shared)"]
        KV[Kernel Virtual Memory\n0xffff000000000000+]
        KCode[Kernel Code + Data]
        PageTables[Page Tables\nper process]
        KernelStack[Kernel Stack\nper process]
    end

    P1 --> |syscall / interrupt| KV
    P2 --> |syscall / interrupt| KV
    P3 --> |syscall / interrupt| KV
    KV --> KCode
    KV --> PageTables
    KV --> KernelStack
```

### Virtual Address Space (x86-64 Linux)
```
0x0000000000000000 - 0x00007fffffffffff  → User space (128 TB)
0xffff800000000000 - 0xffffffffffffffff  → Kernel space (128 TB)
```

---

## 7. System Call Interface — The Bridge

```mermaid
sequenceDiagram
    participant App as User Application
    participant LibC as glibc wrapper
    participant CPU as CPU (x86-64)
    participant Entry as entry_64.S
    participant SCT as sys_call_table[]
    participant Handler as sys_xyz() handler

    App->>LibC: write(fd, buf, len)
    LibC->>CPU: mov rax, 1 (write syscall #)\nsyscall instruction
    CPU->>Entry: Ring 3 → Ring 0, save registers
    Entry->>SCT: Look up sys_call_table[1]
    SCT->>Handler: Call sys_write(fd, buf, len)
    Handler->>Handler: VFS → driver → hardware
    Handler->>Entry: return value in rax
    Entry->>CPU: Ring 0 → Ring 3, restore registers
    CPU->>LibC: return from syscall
    LibC->>App: returns ssize_t
```

---

## 8. Kernel Mode Switch Triggers

| Trigger | Description |
|---------|-------------|
| **System Call** | User program uses `syscall` instruction |
| **Hardware Interrupt** | CPU receives IRQ (disk, NIC, timer...) |
| **Software Exception** | Page fault, divide-by-zero, invalid opcode |
| **Software Interrupt** | `int 0x80` (legacy x86 syscall mechanism) |

---

## 9. Kernel Portability

The kernel abstracts hardware via `arch/` directories:

```mermaid
graph TB
    GenericKernel[Generic Linux Kernel Code\nkernel/ mm/ fs/ net/ drivers/]
    
    GenericKernel --> x86[arch/x86\nDesktops, Servers]
    GenericKernel --> arm64[arch/arm64\nSmartphones, Raspberry Pi]
    GenericKernel --> riscv[arch/riscv\nSifive, RISC-V boards]
    GenericKernel --> mips[arch/mips\nRouters]
    GenericKernel --> ppc[arch/powerpc\nIBM Power]
    GenericKernel --> s390[arch/s390\nIBM mainframes]
```

---

## 10. Key Data Types & Conventions in Kernel Code

| Convention | Meaning |
|-----------|---------|
| `__u8`, `__u16`, `__u32`, `__u64` | Unsigned fixed-width integers |
| `__s8`, `__s16`, `__s32`, `__s64` | Signed fixed-width integers |
| `pid_t` | Process ID |
| `uid_t`, `gid_t` | User/group ID |
| `dev_t` | Device number |
| `sector_t` | Disk sector number |
| `pgoff_t` | Page offset in file |
| `gfp_t` | Memory allocation flags |
| `atomic_t` | Atomic integer (thread-safe) |

---

## 11. Related Concepts
- [04_Monolithic_vs_Microkernel.md](./04_Monolithic_vs_Microkernel.md) — Why Linux is monolithic
- [../01_Getting_Started_With_The_Kernel/01_Kernel_Source_Tree_Layout.md](../01_Getting_Started_With_The_Kernel/01_Kernel_Source_Tree_Layout.md) — Navigating the source tree
- [../04_System_Calls/01_What_Are_System_Calls.md](../04_System_Calls/01_What_Are_System_Calls.md) — System calls in depth
