# Lauterbach TRACE32 — ARM64 Linux Kernel Debug: Detailed Design Document

---

## Table of Contents

1. [Overview](#1-overview)
2. [Architecture: ARM64 (AArch64) Debug Infrastructure](#2-architecture-arm64-aarch64-debug-infrastructure)
3. [TRACE32 Hardware Components](#3-trace32-hardware-components)
4. [JTAG / CoreSight Integration](#4-jtag--coresight-integration)
5. [TRACE32 Software Stack](#5-trace32-software-stack)
6. [Linux Kernel Debug Workflow](#6-linux-kernel-debug-workflow)
7. [Startup Scripts (CMM)](#7-startup-scripts-cmm)
8. [Symbol Loading and DWARF Debug Info](#8-symbol-loading-and-dwarf-debug-info)
9. [SMP Multi-Core Debugging](#9-smp-multi-core-debugging)
10. [MMU & Virtual Address Debugging](#10-mmu--virtual-address-debugging)
11. [Kernel Module Debugging](#11-kernel-module-debugging)
12. [Trace & ETM Configuration](#12-trace--etm-configuration)
13. [Linux-Aware Debugging (RTOS Awareness)](#13-linux-aware-debugging-rtos-awareness)
14. [Common Debug Scenarios](#14-common-debug-scenarios)
15. [Security Considerations (TrustZone / EL Levels)](#15-security-considerations-trustzone--el-levels)
16. [Frequently Used TRACE32 Commands](#16-frequently-used-trace32-commands)
17. [Reference Architecture Diagram](#17-reference-architecture-diagram)

---

## 1. Overview

### Purpose
This document describes the detailed design and methodology for debugging the Linux Kernel on ARM64 (AArch64) targets using Lauterbach TRACE32 PowerDebug.

### Scope
- ARM64 Linux Kernel (v5.x / v6.x)
- TRACE32 PowerDebug Pro / USB3 / Universal
- CoreSight ETM/ETB/ETF/TMC trace infrastructure
- Multi-core SMP (up to 8 cores typical)
- EL0 (User), EL1 (Kernel), EL2 (Hypervisor), EL3 (Secure Monitor)

### Key Goals
| Goal | Description |
|------|-------------|
| Source-level debug | Step through kernel C/ASM code |
| Symbol resolution | Map virtual addresses to function/variable names |
| Trace capture | ETM instruction/data trace for root-cause analysis |
| Linux awareness | Inspect tasks, vmalloc, modules, scheduler |
| Multi-core debug | Synchronized halt/resume across all CPUs |

---

## 2. Architecture: ARM64 (AArch64) Debug Infrastructure

### Exception Levels (EL)

```
+-------------------+
|  EL3 - Secure     |  ARM TrustZone Secure Monitor (BL31 / ATF)
+-------------------+
|  EL2 - Hypervisor |  KVM / Hypervisor (optional)
+-------------------+
|  EL1 - Kernel     |  Linux Kernel (vmlinux)
+-------------------+
|  EL0 - User       |  User-space processes
+-------------------+
```

### ARM64 Debug Registers (Key)

| Register | Purpose |
|----------|---------|
| MDSCR_EL1 | Monitor Debug System Control Register |
| DBGBVR<n>_EL1 | Breakpoint Value Registers (up to 16) |
| DBGBCR<n>_EL1 | Breakpoint Control Registers |
| DBGWVR<n>_EL1 | Watchpoint Value Registers |
| DBGWCR<n>_EL1 | Watchpoint Control Registers |
| EDSCR | External Debug Status and Control Register |
| OSLAR_EL1 | OS Lock Access Register (must be cleared) |
| EDLAR | External Debug Lock Access Register |
| OSLSR_EL1 | OS Lock Status Register |

### Critical: OS Lock
The ARM64 OS Lock (`OSLAR_EL1`) must be cleared before external debug access is possible.

```
OSLAR_EL1 = 0x0  ; Unlock OS lock
; TRACE32 handles this automatically via SYStem.Up
```

---

## 3. TRACE32 Hardware Components

### PowerDebug Probe Options

```
+------------------------+        +----------------------+
|  TRACE32 PowerDebug    |        |   Target Board       |
|  +-----------------+  |        |  +-----------------+ |
|  | Debug Cable     |<-+--------+->| JTAG/SWD Header | |
|  | (LAUTERBACH)    |  |        |  |  TDI/TDO/TCK    | |
|  +-----------------+  |        |  |  TMS/TRST/nRST  | |
|  | AutoFocus II    |  |        |  +-----------------+ |
|  | (Trace Port)    |<-+--------+->| CoreSight Trace | |
|  +-----------------+  |        |  |  TPIU / ETF/TMC | |
+------------------------+        +----------------------+
```

### Connection Interfaces

| Interface | Connector | Speed | Use Case |
|-----------|-----------|-------|----------|
| JTAG | 20-pin ARM | Up to 50 MHz | Debug + Trace |
| SWD | 10-pin Cortex | Up to 50 MHz | Debug only |
| MIPI 20 | 20-pin MIPI | Up to 100 MHz | Trace + Debug |
| MIPI 34 | 34-pin MIPI | Up to 400 MHz | High-speed trace |
| cJTAG (IEEE 1149.7) | 2-pin | Compact | Embedded debug |

---

## 4. JTAG / CoreSight Integration

### CoreSight Architecture on ARM64 SoC

```
CPU Cluster (A55/A72/A78)
+----------------------------------------------+
|  Core 0    Core 1    Core 2    Core 3         |
|  +------+  +------+  +------+  +------+       |
|  | ETM  |  | ETM  |  | ETM  |  | ETM  |       |  ETM = Embedded Trace Macrocell
|  +--+---+  +--+---+  +--+---+  +--+---+       |
|     |         |         |         |            |
|  +--+---------+---------+---------+--+         |
|  |         ATB Funnel                |         |  ATB = Advanced Trace Bus
|  +------------------+----------------+         |
|                     |                          |
|              +------+------+                   |
|              |   TMC/ETF   |                   |  TMC = Trace Memory Controller
|              +------+------+                   |  ETF = Embedded Trace FIFO
|                     |                          |
|              +------+------+                   |
|              |    TPIU     |                   |  TPIU = Trace Port Interface Unit
|              +------+------+                   |
+---------------------|----------------------------+
                       | (Trace Port Pins)
                  TRACE32 Probe
```

### DAP (Debug Access Port) Topology

```
JTAG TDI → [IR/DR Scan Chain]
             ↓
        ARM DAP (ADIv5/ADIv6)
         ├── MEM-AP (AHB/AXI) → System Memory Access
         ├── APB-AP            → CoreSight ROM Table → ETM/CTI/PTM
         └── JTAG-AP           → Cross-trigger / CTI
```

### Cross Trigger Interface (CTI)

CTI enables synchronized halt/resume across all cores:

```
Core 0 CTI ─┐
Core 1 CTI ─┤──→ CTM (Cross Trigger Matrix) ──→ All cores halt simultaneously
Core 2 CTI ─┤
Core 3 CTI ─┘
```

---

## 5. TRACE32 Software Stack

### Component Layers

```
+---------------------------------------------+
|          TRACE32 GUI (t32marm64.exe)         |
+---------------------------------------------+
|     Practice Script Engine (CMM Scripts)     |
+---------------------------------------------+
|    TRACE32 API / JTAG Protocol Layer         |
+---------------------------------------------+
|    Debug Cable Driver (USB / Ethernet)       |
+---------------------------------------------+
|    ARM DAP Driver (ADIv5 / ADIv6)           |
+---------------------------------------------+
|    Target Hardware (ARM64 SoC)               |
+---------------------------------------------+
```

### Key TRACE32 Executables

| Executable | Architecture |
|------------|-------------|
| t32marm64 | AArch64 (ARM64) |
| t32marm | AArch32 / ARM |
| t32marm64s | ARM64 + Trace support |

### config.t32 — Connection Configuration

```tcl
; config.t32 - TRACE32 Hardware Configuration
PBI=USB                          ; PowerDebug via USB
USB=                             ; Auto-detect USB
SCREEN=
FONT=SMALL
HEADER=TRACE32 ARM64 Linux

; Multicore support
SYStem.CPU.MAXCORES=8

; Trace buffer (on-board ETF)
RCL=NETASSIST
PORT=20000
```

---

## 6. Linux Kernel Debug Workflow

### Phase 1: Hardware Reset & Connect

```
Target Power ON
     ↓
TRACE32 Probe Connect (JTAG/SWD)
     ↓
SYStem.CPU <cpu-name>            ; e.g. CORTEXA55
SYStem.CONFIG COREBASE ...       ; CoreSight base addresses
SYStem.Mode.Attach               ; Attach without reset  (OR)
SYStem.Up                        ; Full system reset + attach
     ↓
All CPUs halt at current PC
```

### Phase 2: Symbol Loading

```
; Load kernel ELF with DWARF symbols
Data.LOAD.Elf vmlinux /NOCODE /NOCLEAR
; OR
Data.LOAD.Linux vmlinux          ; Linux-aware load
```

### Phase 3: Set Breakpoints & Resume

```
Break.Set kernel_init /Program   ; BP at kernel_init()
Go                               ; Resume execution
; Wait for BP hit
```

### Phase 4: Inspect State

```
Register.View                    ; View ARM64 registers
List.auto                        ; Source view at current PC
Frame.view /Locals               ; Stack frame + local vars
Var.View %SpotLight sched_class  ; Inspect kernel struct
```

### Debug Flow Diagram

```
TRACE32 Connect
      │
      ▼
SYStem.Up / Attach
      │
      ▼
Load vmlinux Symbols ──→ MMU Table Walk ──→ Virtual Address Resolution
      │
      ▼
Set Breakpoints / Watchpoints
      │
      ▼
Go (Resume Target)
      │
      ▼
Breakpoint Hit ──→ Inspect Registers / Memory / Variables
      │
      ▼
Step Over / Into / Out  ←──────┐
      │                        │
      ▼                        │
Root Cause Found               │
      │                    Continue
      ▼
Fix & Rebuild
```

---

## 7. Startup Scripts (CMM)

### linux_arm64_attach.cmm — Attach to Running Kernel

```tcl
; =============================================================
; linux_arm64_attach.cmm
; Attach to running ARM64 Linux kernel (no reset)
; =============================================================

LOCAL &kernelelf
&kernelelf="~/build/vmlinux"

; ----- Hardware Setup -----
SYStem.CPU CORTEXA55              ; Set CPU type
SYStem.CONFIG COREBASE DAP:0x80000000  ; DAP base (SoC specific)
SYStem.CONFIG MEMORYACCESSPORT 0
SYStem.CONFIG DEBUGACCESSPORT 1

SYStem.Option DUALPORT ON         ; Enable DCC fast memory access
SYStem.Option ResBreak OFF        ; Don't break on reset
SYStem.Option EnReset OFF         ; Don't drive nRST

SYStem.Mode.Attach                ; Non-intrusive attach

; ----- Verify OS Lock cleared -----
IF (Data.Long(C15:0x10) &0x1)
(
  PRINT "WARNING: OS Lock is set, clearing..."
  Data.Set C15:0x10 %Long 0x0
)

; ----- Load Symbols -----
Data.LOAD.Elf &kernelelf /NOCODE /NOCLEAR /NoClear
PRINT "Symbols loaded from: " &kernelelf

; ----- Enable Linux Awareness -----
TASK.CONFIG ~~/demo/arm64/kernel/linux/linux.t32
MENU.ReProgram ~~/demo/arm64/kernel/linux/linux.men

; ----- Done -----
PRINT "ARM64 Linux Kernel Debug Ready"
```

### linux_arm64_boot_debug.cmm — Debug from Early Boot

```tcl
; =============================================================
; linux_arm64_boot_debug.cmm
; Debug ARM64 Linux from early boot (after U-Boot loads kernel)
; =============================================================

LOCAL &kernelelf &loadaddr
&kernelelf="~/build/vmlinux"
&loadaddr=0xFFFF800010000000     ; Kernel virtual load address

SYStem.CPU CORTEXA72
SYStem.CONFIG COREBASE DAP:0x80000000
SYStem.Mode.Prepare               ; Halt CPU immediately at reset

Go.direct                         ; Release reset, CPU halts at reset vector

; Wait for bootloader to decompress and load kernel
; Set BP at kernel start_kernel()
Break.Set start_kernel /Program

Go
WAIT !STATE.RUN() 60.s            ; Wait up to 60 seconds

IF STATE.RUN()
(
  Break
  PRINT "ERROR: start_kernel BP not hit in 60s"
  ENDDO
)

; Load symbols now that MMU is potentially ON
Data.LOAD.Elf &kernelelf /NOCODE /NOCLEAR
TASK.CONFIG ~~/demo/arm64/kernel/linux/linux.t32

PRINT "Halted at start_kernel — ready to debug"
```

---

## 8. Symbol Loading and DWARF Debug Info

### Kernel Build Requirements

```makefile
# In kernel .config
CONFIG_DEBUG_INFO=y
CONFIG_DEBUG_INFO_DWARF4=y      # or DWARF5
CONFIG_KALLSYMS=y
CONFIG_KALLSYMS_ALL=y
CONFIG_FRAME_POINTER=y          # Proper stack unwinding
CONFIG_RANDOMIZE_BASE=n         # Disable KASLR for static debug
```

### Symbol File Hierarchy

```
vmlinux                          ; Main kernel ELF (uncompressed)
  └── .debug_info                ; DWARF type/variable info
  └── .debug_line                ; Source line mapping
  └── .debug_frame               ; CFI for stack unwinding
  └── .symtab / .strtab          ; Symbol table

drivers/net/ethernet/foo.ko      ; Module ELF (loaded separately)
```

### KASLR (Kernel Address Space Layout Randomization)

When KASLR is enabled, symbols must be relocated at runtime:

```tcl
; Get KASLR offset from kernel (via /proc/kallsyms or JTAG read)
LOCAL &kaslr_offset
&kaslr_offset=0x1234000          ; Read from target or boot log

; Relocate all symbols
Data.LOAD.Elf vmlinux /NOCODE /NOCLEAR /RelocationDelta &kaslr_offset
```

Or use TRACE32 Linux awareness auto-relocation:

```tcl
TASK.CONFIG linux.t32
; Linux plugin reads kaslr_offset from kimage_voffset symbol
```

---

## 9. SMP Multi-Core Debugging

### Core Affinity and Halting

```tcl
; Halt all cores simultaneously (via CTI cross-trigger)
Break

; Switch between cores
CORE.Select 0                    ; View Core 0
Register.View
CORE.Select 1                    ; View Core 1
Register.View

; Resume all cores
Go

; Resume only Core 0
Go.Core 0
```

### Per-Core Breakpoints

```tcl
; Break on Core 2 only
Break.Set kernel_function /Program /CORE 2

; Break on all cores
Break.Set schedule /Program      ; Hits whichever core runs schedule()
```

### SMP State View

```tcl
CORE.view                        ; Show state of all cores
; Output:
; CORE  STATE     PC             FUNCTION
;   0   halted    0xffff80001234 __schedule
;   1   running   --------       --------
;   2   halted    0xffff80005678 do_IRQ
;   3   halted    0xffff8000abcd sys_read
```

### Halt Mode Synchronization

```
Core 0 hits BP
    │
    ▼
CTI TRIGOUT → CTM → CTI TRIGIN (all other cores)
    │
    ▼
All cores halt within 1–2 clock cycles
```

---

## 10. MMU & Virtual Address Debugging

### ARM64 Virtual Memory Layout (Linux Default)

```
0xFFFF_FFFF_FFFF_FFFF ┐
                       │  Kernel Space (EL1)
0xFFFF_8000_0000_0000 ┘   vmalloc / modules / direct map

0x0000_FFFF_FFFF_FFFF ┐
                       │  User Space (EL0)
0x0000_0000_0000_0000 ┘
```

### TRACE32 MMU Configuration

```tcl
; Enable MMU-aware translation (requires EL1 TTBR registers)
MMU.FORMAT LINUX &kernelelf         ; Tell TRACE32 Linux page table format
MMU.ON                              ; Activate VA→PA translation in TRACE32

; TRACE32 reads TTBR0_EL1 / TTBR1_EL1 automatically
; and walks the 4-level page table (PGD→PUD→PMD→PTE)
```

### Address Translation Commands

```tcl
; Translate virtual to physical
MMU.TRANSLATION 0xFFFF800010000000  ; Shows PA for given VA

; Access physical memory directly
Data.Dump AXI:0x80000000            ; Bypass MMU using AXI/AHB AP

; Access virtual memory (with MMU walk)
Data.Dump 0xFFFF800010000000        ; Uses TTBR1_EL1 for kernel VA
```

### Page Table Walk

```tcl
MMU.DUMP PAGETABLE 0xFFFF800010000000  ; Dump page table for address
; Shows: PGD[idx] → PUD[idx] → PMD[idx] → PTE[idx] → Physical Page
```

---

## 11. Kernel Module Debugging

### Module Load Address Discovery

When a module is loaded, its .text/.data sections are placed at dynamic vmalloc addresses.

```tcl
; Method 1: Read from /sys/module/<name>/sections/.text
; On running system, capture via TRACE32 OS awareness:

TASK.sYmbol.AddModule mydriver.ko  ; Auto-discovers load address

; Method 2: Manual relocation
; Read module .text base from kernel struct module
Var.View %SpotLight ((struct module*)module_ptr)->core_layout.base
```

### Module Symbol Loading Script

```tcl
; load_module.cmm
LOCAL &modname &modelf &textbase
&modname="mydriver"
&modelf="~/build/mydriver.ko"

; Get text base from Linux awareness
&textbase=TASK.sYmbol.GetModuleBase("&modname")

; Load module ELF at discovered address
Data.LOAD.Elf &modelf &textbase /NOCODE /NOCLEAR
PRINT "Module &modname loaded at " &textbase
```

---

## 12. Trace & ETM Configuration

### ETM (Embedded Trace Macrocell) — ARM64

ARM64 ETMv4 supports:
- Instruction trace (Program Flow)
- Data trace (limited in ETMv4)
- Context ID tracing (for process tracking)
- Cycle-accurate trace

### ETM Configuration in TRACE32

```tcl
; Configure ETM on Core 0
ETM.OFF
ETM.PortMode WRAPPED              ; Use on-chip ETF buffer
ETM.PortSize 4                    ; 4-bit trace port width (ETF mode)

; Trace all kernel instructions (EL1)
ETM.CONTEXTID 32                  ; Trace 32-bit context IDs (PID)
ETM.ON                            ; Enable trace

; Set trace filter — kernel only (EL1), exclude EL0
ETM.FILTER.Set EL1                ; Only trace EL1
```

### On-Chip Buffer (ETF/TMC) Configuration

```tcl
; Configure Trace Memory Controller (TMC) in ETF mode
TPIU.OFF
ETF.ON                            ; Use on-chip circular buffer
ETF.ClearBuffer

Go                                ; Run kernel
; ... trigger event ...
Break                             ; Stop

ETF.ReadBuffer                    ; Pull trace from ETF into TRACE32
Trace.List List.auto              ; Show decoded instruction trace
```

### Streaming Trace via TPIU (Lauterbach AutoFocus)

```tcl
; Use external high-speed trace (requires AutoFocus II / CombiProbe)
TPIU.ON
TPIU.PortSize 16                  ; 16-bit parallel trace port

ETM.ON
Go
; Trace captured in TRACE32 host memory (GBytes possible)
```

### Trace Filtering (Function-Level)

```tcl
; Trace only specific function
ETM.TRIGGER.Set ADDRESS schedule   ; Start trace when schedule() called
ETM.FILTER.Set RANGE schedule schedule+0x200  ; Trace only within schedule

Go
```

### Trace Analysis

```tcl
Trace.List                         ; Instruction-level trace list
Trace.Chart.FUNC                   ; Function call timeline chart
Trace.Statistics.FUNC              ; Function execution statistics (profiling)
Trace.PROfile.FUNC                 ; CPU profiling from trace
```

---

## 13. Linux-Aware Debugging (RTOS Awareness)

### TRACE32 Linux Awareness Plugin

TRACE32 ships with Linux awareness scripts at:
```
~~/demo/arm64/kernel/linux/linux.t32
~~/demo/arm64/kernel/linux/linux.men
```

### Activation

```tcl
TASK.CONFIG ~~/demo/arm64/kernel/linux/linux.t32
MENU.ReProgram ~~/demo/arm64/kernel/linux/linux.men
```

### Linux-Aware Commands

```tcl
; List all tasks (processes/threads)
TASK.List.Tasks

; Show task details
TASK.List.Thread

; Switch context to a specific task
TASK.Select <pid>
Frame.view /Locals                 ; Stack for selected task

; Memory maps per process
TASK.List.Maps <pid>

; Show kernel modules
TASK.List.Modules

; vmalloc regions
TASK.List.VMAreas

; Show all IRQs
TASK.List.Interrupts

; Inspect scheduler runqueues
Var.View %SpotLight runqueue
```

### Task-Aware Breakpoint

```tcl
; Break only when process "myapp" (pid=1234) runs this code
Break.Set do_sys_open /Program /TASK 1234
```

### Per-Task Stack Unwinding

```tcl
TASK.Select 1234                   ; Select task by PID
Frame.view                         ; Unwind stack for that task context
; Shows proper backtrace even if task is sleeping (using thread_info)
```

---

## 14. Common Debug Scenarios

### Scenario 1: Kernel Panic / Oops

```tcl
; After panic, kernel writes debug info to serial
; Attach TRACE32 post-mortem

SYStem.Mode.Attach
Data.LOAD.Elf vmlinux /NOCODE /NOCLEAR

; Read panic PC from SPSR_EL1 / ELR_EL1
Register.View
; ELR_EL1 = return address from exception (panic PC)

; Decode stack trace manually
Frame.view                         ; ARM64 stack unwind
Data.Dump SP--0x200                ; Raw stack dump around SP
```

### Scenario 2: Hung Task / Deadlock

```tcl
; Attach without reset
SYStem.Mode.Attach

; Check all task states
TASK.List.Tasks
; Look for tasks in 'D' state (uninterruptible sleep)

; For each suspicious task, unwind stack
TASK.Select <pid>
Frame.view
; Identify what lock/resource the task is waiting on

; Check mutex/spinlock owner
Var.View mutex_variable
```

### Scenario 3: Memory Corruption

```tcl
; Set hardware watchpoint on corrupted address
Break.Set 0xFFFF800012345678 /Write /Long  ; Watch 4-byte write

Go
; TRACE32 breaks when that address is written
; Register and stack state reveals the culprit
```

### Scenario 4: IRQ Debugging

```tcl
; Break at interrupt handler entry
Break.Set irq_handler_entry /Program

; Or trace IRQ path with ETM
ETM.TRIGGER.Set ADDRESS __handle_irq
ETM.ON
Go
```

### Scenario 5: Boot Debug (early_printk phase)

```tcl
; Set BP before MMU is enabled (physical address)
Break.Set 0x80080000 /Program       ; Physical address of startup_64

; After MMU on, switch to virtual
MMU.ON
Break.Set start_kernel /Program     ; Virtual address now works
```

---

## 15. Security Considerations (TrustZone / EL Levels)

### ARM64 Exception Level Debug Access

| EL | Description | TRACE32 Access |
|----|-------------|----------------|
| EL0 | User Space | Via EL1 kernel debug or EL0 debug |
| EL1 | Linux Kernel | Direct via EDSCR |
| EL2 | Hypervisor (KVM) | Requires EL2 debug enabled |
| EL3 | Secure Monitor (ATF) | Requires SPIDEN/SPNIDEN signals |

### TrustZone Debug

```tcl
; Check security state
; TRACE32 shows NS bit in CPSR/SPSR

; Switch to Secure World (if SPIDEN asserted)
SYStem.Option.TRUSTZONE ON
; Now can debug EL3 (BL31 / OP-TEE)
```

### Debug Authentication Signals

```
DBGEN  — Non-secure invasive debug enable
NIDEN  — Non-secure non-invasive trace enable
SPIDEN — Secure invasive debug enable
SPNIDEN— Secure non-invasive trace enable
```

These are hardware signals on the SoC. TRACE32 reads their state:

```tcl
SYStem.CONFIG DEBUGAUTHSIGNAL      ; Show current debug auth state
```

### Production Devices

On production devices with JTAG fused/disabled:
- TRACE32 cannot perform JTAG attach
- Alternative: Use KGDB/KDB over serial (software debug)
- Or: Configure platform to enable debug in non-production mode via eFuse

---

## 16. Frequently Used TRACE32 Commands

### System Control

```tcl
SYStem.CPU CORTEXA55               ; Set CPU type
SYStem.Up                          ; Reset + connect
SYStem.Mode.Attach                 ; Attach without reset
SYStem.Down                        ; Disconnect
```

### Execution Control

```tcl
Go                                 ; Resume all cores
Break                              ; Halt all cores
Step                               ; Single step (source level)
StepOver                           ; Step over function call
StepOut                            ; Step out of current function
Go.direct                          ; Resume at instruction level
```

### Breakpoints & Watchpoints

```tcl
Break.Set <addr_or_sym> /Program   ; SW/HW code breakpoint
Break.Set <addr> /Write /Long      ; Hardware watchpoint (write)
Break.Set <addr> /Read             ; Hardware watchpoint (read)
Break.Delete                        ; Remove all breakpoints
Break.List                          ; List all breakpoints
```

### Memory & Registers

```tcl
Data.Dump <addr>                   ; Hex dump memory
Data.List <addr>                   ; Disassemble at address
Data.Set <addr> %Long <value>      ; Write to memory
Register.View                      ; Show all registers
Register.Set PC <addr>             ; Set program counter
```

### Symbols & Variables

```tcl
Symbol.List                        ; List loaded symbols
Var.View <varname>                 ; View C variable
Var.Set <varname> = <value>        ; Set C variable
Frame.view                         ; Call stack / locals
```

### Trace

```tcl
ETM.ON / ETM.OFF                   ; Enable/disable ETM
Trace.List                         ; Show trace listing
Trace.Chart.FUNC                   ; Function timeline
Trace.Statistics.FUNC              ; Profiling statistics
ETF.ReadBuffer                     ; Read on-chip trace buffer
```

### Scripting

```tcl
DO script.cmm                      ; Execute CMM script
PRINT "message"                    ; Print to TRACE32 area
DIALOG.OK "message"                ; Show dialog
LOCAL &var                         ; Declare local variable
&var=Register(PC)                  ; Read PC into variable
WAIT !STATE.RUN() 30.s             ; Wait for halt (30s timeout)
```

---

## 17. Reference Architecture Diagram

```
┌─────────────────────────────────────────────────────────────────────┐
│                        Host Workstation                             │
│  ┌─────────────────────────────────────────────────────────────┐   │
│  │            TRACE32 PowerView (t32marm64)                    │   │
│  │  ┌──────────┐  ┌──────────┐  ┌──────────┐  ┌──────────┐   │   │
│  │  │ Source   │  │ Register │  │  Trace   │  │  Linux   │   │   │
│  │  │ Window   │  │  View    │  │  List    │  │  Tasks   │   │   │
│  │  └──────────┘  └──────────┘  └──────────┘  └──────────┘   │   │
│  └────────────────────────┬────────────────────────────────────┘   │
│                           │ USB 3.0 / GbE                          │
└───────────────────────────┼─────────────────────────────────────────┘
                            │
                 ┌──────────┴──────────┐
                 │  TRACE32 PowerDebug │
                 │  (Debug Probe)      │
                 │  + AutoFocus II     │
                 │  (Trace Capture)    │
                 └──────────┬──────────┘
                            │ MIPI 20/34 pin
              ┌─────────────┴──────────────────────┐
              │           ARM64 SoC                 │
              │                                     │
              │  ┌─────────┐  ┌─────────┐          │
              │  │ Core 0  │  │ Core 1  │  ...      │
              │  │ A55/A72 │  │ A55/A72 │          │
              │  │  ETM    │  │  ETM    │          │
              │  └────┬────┘  └────┬────┘          │
              │       └──────┬─────┘               │
              │         ┌────┴────┐                 │
              │         │ATB Funnl│                 │
              │         └────┬────┘                 │
              │         ┌────┴────┐                 │
              │         │TMC(ETF) │                 │
              │         └────┬────┘                 │
              │         ┌────┴────┐                 │
              │         │  TPIU  │                  │
              │         └────┬────┘                 │
              │   ┌──────────┴──────────┐           │
              │   │  ARM DAP (ADIv5)    │           │
              │   │  MEM-AP / APB-AP    │           │
              │   └─────────────────────┘           │
              │                                     │
              │   RAM (LPDDR4/5)   Flash (eMMC)     │
              └─────────────────────────────────────┘
```

---

## Appendix A: CMM Script Template Library

### Template: Quick Attach + Break

```tcl
; quick_attach.cmm
SYStem.CPU CORTEXA55
SYStem.Mode.Attach
Data.LOAD.Elf ~/build/vmlinux /NOCODE /NOCLEAR
TASK.CONFIG ~~/demo/arm64/kernel/linux/linux.t32
Break.Set <your_function> /Program
Go
PRINT "Attached and BP set. Running..."
```

### Template: Watchdog Disable

```tcl
; Some SoCs reset via watchdog during debug halt
; Disable watchdog before long debug sessions
Data.Set AXI:0xFE000000 %Long 0x0   ; WDT disable register (SoC specific)
```

### Template: Read CPU Affinity Registers

```tcl
; Read MPIDR_EL1 to identify current core
LOCAL &mpidr
&mpidr=Data.Long(C15:0xD005)        ; MPIDR_EL1 via coprocessor read
PRINT "MPIDR_EL1 = " &mpidr
```

---

## Appendix B: Troubleshooting

| Problem | Cause | Resolution |
|---------|-------|------------|
| "Cannot access target" | OS Lock set | `Data.Set C15:0x10 %Long 0` |
| Symbols not resolving | KASLR offset mismatch | Re-read `kimage_voffset` and relocate |
| BP not hitting | MMU off / wrong address space | Check TTBR, use `/PA` for physical BP |
| Trace garbled | Clock skew on trace port | Reduce ETM clock or use ETF instead of TPIU |
| Core stuck in WFI | CPU halted in idle | `CORE.Select N` + `Step` to wake |
| Stack unwind wrong | Frame pointer omitted | Rebuild with `CONFIG_FRAME_POINTER=y` |
| Module symbols missing | Module loaded dynamically | Use `TASK.sYmbol.AddModule` after load |
| EL3 not accessible | SPIDEN not asserted | Assert SPIDEN on board or via boot strap |

---

*Document Version: 1.0 | Target: ARM64 AArch64 | Tool: Lauterbach TRACE32 PowerDebug*
