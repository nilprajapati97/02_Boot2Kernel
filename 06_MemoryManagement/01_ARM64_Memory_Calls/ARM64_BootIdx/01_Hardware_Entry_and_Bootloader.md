# Layer 1 — Hardware Entry & Bootloader Handoff

[← Index](00_Index.md) | **Layer 1** | [Layer 2 — Early Assembly Boot →](02_Early_Assembly_Boot.md)

---

## What This Layer Covers

Before a single line of Linux kernel code executes, the hardware must be powered up and
a bootloader must load the kernel into RAM. This layer explains the ARMv8-A architecture
fundamentals that matter for memory initialization and what the bootloader does before
handing control to the kernel.

---

## 1. ARMv8-A Architecture Fundamentals (What You Need to Know)

### 1.1 Exception Levels (Privilege Hierarchy)

ARMv8-A defines four exception levels (ELs), from least to most privileged:

```
┌─────────────────────────────────────────────────────────┐
│  EL3 — Secure Monitor (ARM Trusted Firmware / TF-A)    │
│        Manages transitions between Secure & Non-Secure  │
│        worlds. Runs at the highest privilege.            │
├─────────────────────────────────────────────────────────┤
│  EL2 — Hypervisor (KVM, Xen, or bootloader stub)       │
│        Controls Stage-2 address translation.             │
│        Virtualizes hardware for guest OSes.              │
├─────────────────────────────────────────────────────────┤
│  EL1 — OS Kernel (Linux runs here)                      │
│        Controls Stage-1 address translation (page        │
│        tables). Manages all memory for the system.       │
├─────────────────────────────────────────────────────────┤
│  EL0 — User Applications                                │
│        No direct hardware access. Uses syscalls to       │
│        request memory (mmap, brk, malloc).               │
└─────────────────────────────────────────────────────────┘
```

**Key point**: Linux runs at EL1. The bootloader may hand off control at either EL2 or
EL1. If at EL2, the kernel's early boot code (`init_kernel_el` in head.S) installs a
minimal hypervisor stub and drops down to EL1.

### 1.2 The Two-TTBR Split: Kernel vs User Address Space

ARMv8-A has a critical feature for memory management: **two Translation Table Base
Registers (TTBRs)** that simultaneously provide two independent virtual address spaces:

```
Virtual Address Space (e.g., 48-bit VA = 256 TB total)
═══════════════════════════════════════════════════════

0xFFFF_FFFF_FFFF_FFFF  ┐
                        │  TTBR1_EL1 — Kernel Space
                        │  Addresses starting with 0xFFFF...
                        │  Used by the kernel (Linux)
                        │  Kernel page tables live here
0xFFFF_0000_0000_0000  ┘

  ─── Unmapped hole (fault if accessed) ───

0x0000_FFFF_FFFF_FFFF  ┐
                        │  TTBR0_EL1 — User Space
                        │  Addresses starting with 0x0000...
                        │  Used by user-space processes
                        │  Each process has its own TTBR0
0x0000_0000_0000_0000  ┘
```

**How the hardware decides which TTBR to use**:
- The CPU looks at the **top bits** of the virtual address
- If the top bits are all 1s → use TTBR1 (kernel)
- If the top bits are all 0s → use TTBR0 (user)
- Any other pattern → translation fault (unmapped hole)

**Why this matters for boot**: When the MMU is first turned on, the kernel needs an
**identity map** in TTBR0 (where physical address == virtual address) so the code that
enables the MMU can continue executing. Once the kernel is mapped at its high virtual
address in TTBR1, the identity map in TTBR0 is removed.

### 1.3 Address Translation Basics

ARMv8 translates virtual addresses to physical addresses through a multi-level page
table walk:

```
Virtual Address (48-bit, 4KB granule)
┌──────┬──────┬──────┬──────┬──────────────┐
│ [63] │47:39 │38:30 │29:21 │ 20:12  │11:0 │
│ TTBR │ L0   │ L1   │ L2   │ L3     │Off  │
│ sel  │(PGD) │(PUD) │(PMD) │(PTE)   │set  │
└──┬───┴──┬───┴──┬───┴──┬───┴──┬─────┴──┬──┘
   │      │      │      │      │        │
   │      │      │      │      │        └─► 12-bit offset within 4KB page
   │      │      │      │      └──────────► Level 3: Page Table Entry (PTE)
   │      │      │      └─────────────────► Level 2: PMD (or 2MB block)
   │      │      └────────────────────────► Level 1: PUD (or 1GB block)
   │      └───────────────────────────────► Level 0: PGD (top-level)
   └──────────────────────────────────────► Selects TTBR0 or TTBR1
```

**Translation granule sizes**: ARMv8 supports three page sizes:
- **4KB** (most common for Linux) — 4 levels of page tables for 48-bit VA
- **16KB** — 4 levels for 47-bit VA
- **64KB** — 3 levels for 42-bit VA

**Block mappings**: At L1 (PUD) and L2 (PMD), the hardware supports "block" entries
that map a large contiguous region without requiring lower-level tables:
- L1 block = **1 GB** (with 4KB granule)
- L2 block = **2 MB** (with 4KB granule)

The kernel uses these block mappings extensively during boot to map large regions of RAM
efficiently.

### 1.4 Key System Registers for Memory

| Register | Purpose |
|----------|---------|
| `SCTLR_EL1` | System Control — bit 0 (M) enables/disables MMU |
| `TTBR0_EL1` | Translation Table Base Register 0 — user/identity map page table base |
| `TTBR1_EL1` | Translation Table Base Register 1 — kernel page table base |
| `TCR_EL1` | Translation Control — VA size (T0SZ/T1SZ), granule, cacheability |
| `MAIR_EL1` | Memory Attribute Indirection Register — defines memory types |
| `ID_AA64MMFR0_EL1` | Read-only: reports supported PA size, granule sizes |

### 1.5 Cache Architecture

ARMv8 has separate instruction and data caches. During early boot:
- **D-cache is OFF** (SCTLR.C = 0) — all memory accesses go directly to RAM
- **I-cache may be ON or OFF** — the kernel handles both cases
- The boot code must perform **cache maintenance** (invalidation) when creating page
  tables with the D-cache off, to ensure coherency when caches are later enabled

---

## 2. What the Bootloader Does

### 2.1 The Bootloader's Job

The bootloader (typically **U-Boot** on embedded boards, or **UEFI** on server platforms)
performs these steps before the kernel:

```
┌──────────────────────────────────────────────────────────┐
│                    BOOTLOADER                             │
│                                                           │
│  1. Initialize DRAM controller                            │
│     └─ Board-specific: configure DDR timing, size,        │
│        interleaving. After this, RAM is accessible.       │
│                                                           │
│  2. Load kernel Image into RAM                            │
│     └─ From flash, SD card, eMMC, TFTP, etc.              │
│        Loaded at a 2MB-aligned physical address.           │
│                                                           │
│  3. Load Device Tree Blob (DTB) into RAM                  │
│     └─ .dtb file compiled from .dts source.                │
│        Contains hardware description including             │
│        memory regions, reserved memory, peripherals.       │
│                                                           │
│  4. Optionally load initrd/initramfs into RAM             │
│     └─ Initial root filesystem.                            │
│                                                           │
│  5. Set CPU state for kernel entry:                        │
│     ┌─────────────────────────────────────────────┐        │
│     │  x0 = physical address of DTB               │        │
│     │  x1 = 0 (reserved)                          │        │
│     │  x2 = 0 (reserved)                          │        │
│     │  x3 = 0 (reserved)                          │        │
│     │  MMU = OFF                                   │        │
│     │  D-cache = OFF (must be)                     │        │
│     │  I-cache = ON or OFF (kernel handles both)   │        │
│     │  Interrupts = DISABLED (DAIF masked)         │        │
│     │  Exception level = EL2 (preferred) or EL1    │        │
│     │  Primary CPU only (secondaries held in pen)  │        │
│     └─────────────────────────────────────────────┘        │
│                                                           │
│  6. Jump to kernel entry point                            │
│     └─ Branch to start of Image (b primary_entry)          │
└──────────────────────────────────────────────────────────┘
```

### 2.2 The Kernel Image in Memory

When the bootloader loads the kernel, the binary has a specific header format:

```
Offset   Field              Description
─────────────────────────────────────────────────────
0x00     branch instruction  "b primary_entry" — jumps over the header
0x08     text_offset         Offset of kernel text from load address
0x10     image_size          Size of the image (for bootloader)
0x18     flags               Endianness, page size, etc.
0x20     reserved fields     ...
0x38     magic               "ARM\x64" magic number
0x3C     pe_header_offset    For UEFI PE/COFF stub
```

The first instruction at offset 0x00 is a branch that jumps directly to `primary_entry`
— the true start of the Linux kernel on ARM64.

### 2.3 The Device Tree Blob (DTB) — Memory Description

The DTB is crucial for memory initialization. It contains a hardware description that
the kernel parses during early boot. The memory-relevant nodes are:

```
/ {
    /* Root properties — needed to parse addresses */
    #address-cells = <2>;          /* 64-bit addresses */
    #size-cells = <2>;             /* 64-bit sizes */

    /* Memory nodes — THE source of physical RAM information */
    memory@40000000 {
        device_type = "memory";
        reg = <0x00 0x40000000 0x00 0x80000000>;
        /*     ^^^^ ^^^^^^^^^^  ^^^^ ^^^^^^^^^^
               high  low addr    high  low size
               = base 0x40000000, size 0x80000000 (2 GB)  */
    };

    /* Optional: second memory bank */
    memory@100000000 {
        device_type = "memory";
        reg = <0x01 0x00000000 0x00 0x80000000>;
        /*     base 0x100000000, size 2 GB */
    };

    /* Chosen node — boot parameters */
    chosen {
        bootargs = "console=ttyS0,115200 root=/dev/mmcblk0p2";
        linux,initrd-start = <0x00 0x48000000>;
        linux,initrd-end = <0x00 0x49000000>;
    };

    /* Reserved memory — regions firmware or hardware needs */
    reserved-memory {
        #address-cells = <2>;
        #size-cells = <2>;
        ranges;

        /* Example: GPU memory, TEE memory, etc. */
        gpu_reserved: gpu@50000000 {
            reg = <0x00 0x50000000 0x00 0x10000000>;
            no-map;    /* Do NOT include in kernel's linear map */
        };

        /* CMA pool for DMA allocations */
        linux,cma {
            compatible = "shared-dma-pool";
            reusable;
            size = <0x00 0x10000000>;  /* 256 MB */
            linux,cma-default;
        };
    };
};
```

**Key points about the DTB**:
1. `/memory` nodes tell the kernel **where physical RAM is** and **how large** each bank is
2. `/chosen` provides the kernel command line and initrd location
3. `/reserved-memory` tells the kernel which regions are off-limits (firmware, GPU, TEE)
4. The kernel calls `memblock_add()` for each `/memory` region — this is how the kernel
   first learns about available RAM

---

## 3. Physical Memory Layout at Kernel Entry

When the bootloader jumps to the kernel, physical memory looks like this:

```
Physical Address Space
═══════════════════════════════════════════════════════

High addr  ┌─────────────────────────────────────┐
           │                                     │
           │         (may be empty or            │
           │          second DRAM bank)           │
           │                                     │
           ├─────────────────────────────────────┤  ◄── Top of DRAM bank 1
           │                                     │
           │          Free RAM                    │
           │       (majority of memory)           │
           │                                     │
           ├─────────────────────────────────────┤
           │  initrd / initramfs                  │  ◄── Loaded by bootloader
           ├─────────────────────────────────────┤
           │                                     │
           │          Free RAM                    │
           │                                     │
           ├─────────────────────────────────────┤
           │  Device Tree Blob (DTB/FDT)          │  ◄── x0 points here
           ├─────────────────────────────────────┤
           │                                     │
           │          Free RAM                    │
           │                                     │
           ├─────────────────────────────────────┤
           │  Linux Kernel Image                  │  ◄── Entry point here
           │  ┌─ .text (code)                     │
           │  ├─ .rodata (read-only data)         │
           │  ├─ .init.text (init code)           │
           │  ├─ .init.data (init data)           │
           │  ├─ .data (read-write data)          │
           │  ├─ .bss (zero-initialized data)     │
           │  └─ page table dirs (idmap, swapper) │
           ├─────────────────────────────────────┤
           │                                     │
           │          Free RAM                    │
           │                                     │
           ├─────────────────────────────────────┤  ◄── Base of DRAM (e.g., 0x40000000)
           │                                     │
           │   Memory-mapped I/O (peripherals)    │
           │   Interrupt controller, timers, etc.  │
           │                                     │
Low addr   └─────────────────────────────────────┘  0x00000000
```

**The kernel knows nothing about this layout yet**. It has:
- Register `x0` pointing to the DTB
- Its own code loaded at some physical address
- MMU is off, so it uses physical addresses directly

Everything else — the size of RAM, what's reserved, where other devices are — will be
discovered by parsing the DTB in Layer 4.

---

## 4. The Moment of Handoff

At the exact instant the bootloader branches to the kernel:

```
┌────────────────────────────────────────────────────────────────┐
│                     CPU STATE AT KERNEL ENTRY                  │
│                                                                │
│  Registers:                                                    │
│    x0  = Physical address of DTB (Flattened Device Tree)       │
│    x1  = 0                                                     │
│    x2  = 0                                                     │
│    x3  = 0                                                     │
│    PC  = Physical address of kernel Image start                │
│    SP  = Undefined (kernel sets its own stack)                  │
│                                                                │
│  MMU:          OFF (SCTLR_EL1.M = 0)                           │
│  Data Cache:   OFF (SCTLR_EL1.C = 0)                           │
│  Interrupts:   ALL MASKED (PSTATE.DAIF = 0b1111)               │
│  Exception:    EL2 (typical) or EL1                             │
│  Endianness:   Little-endian (for Linux)                        │
│                                                                │
│  Memory:                                                       │
│    All addresses are PHYSICAL                                  │
│    No virtual-to-physical translation happening                │
│    CPU fetches instructions directly from physical RAM          │
└────────────────────────────────────────────────────────────────┘
```

The very first kernel instruction executes at offset 0x00 of the loaded Image: a branch
to `primary_entry` in `arch/arm64/kernel/head.S`. This is where **Layer 2** begins.

---

## Summary: What We Know After Layer 1

| Aspect | Status |
|--------|--------|
| CPU exception level | EL2 or EL1 — kernel will drop to EL1 |
| MMU state | **OFF** — all addresses are physical |
| Caches | D-cache OFF, I-cache may be on |
| RAM knowledge | **NONE** — kernel doesn't yet know how much RAM exists |
| Memory allocator | **NONE** — no allocation capability at all |
| Page tables | **NONE** — no virtual address translation |
| What kernel has | x0 = DTB physical address, code loaded in RAM |
| What comes next | head.S creates identity map, enables MMU (Layer 2) |

---

[← Index](00_Index.md) | **Layer 1** | [Layer 2 — Early Assembly Boot →](02_Early_Assembly_Boot.md)
