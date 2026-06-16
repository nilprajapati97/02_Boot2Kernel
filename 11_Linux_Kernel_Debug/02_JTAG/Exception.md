Lauterbach TRACE32
Create detailed design document for ARM64 for Linux KErnel

---

Creating detailed design document for Lauterbach TRACE32 ARM64 Linux Kernel debugging

---



## Document Structure (17 Sections)

**Hardware & Architecture**
- ARM64 exception levels (EL0–EL3) and debug registers (MDSCR, DBGBVR, OSLAR)
- TRACE32 probe options — PowerDebug Pro, AutoFocus II, MIPI 20/34 connectors
- CoreSight topology: ETM → ATB Funnel → TMC/ETF → TPIU → TRACE32
- DAP (ADIv5/ADIv6) with MEM-AP / APB-AP and CTI cross-trigger matrix

**Software & Workflow**
- Full debug workflow from hardware connect → symbol load → breakpoint → inspect
- CMM startup scripts for two key scenarios: attach-to-running-kernel and early-boot-debug
- KASLR handling — runtime relocation of symbols using `kimage_voffset`

**Key Debug Capabilities**
- SMP multi-core: CTI-synchronized halt, per-core BPs, `CORE.view`
- MMU-aware debugging: 4-level page table walk, VA→PA translation, `MMU.TRANSLATION`
- Kernel module symbols via `TASK.sYmbol.AddModule` (dynamic vmalloc addresses)
- ETM trace: ETF on-chip buffer, TPIU streaming, function-level filters, profiling

**Linux Awareness**
- `TASK.CONFIG linux.t32` plugin — task list, per-PID stack unwind, vmalloc regions, IRQ list
- Task-aware breakpoints (`/TASK <pid>`)

**Common Scenarios**
- Kernel panic/oops, hung task/deadlock, memory corruption watchpoints, IRQ debug, early-boot (pre-MMU)

**Security**
- TrustZone EL3 debug, SPIDEN/SPNIDEN signals, production device limitations

**Appendices**
- CMM script templates (quick attach, watchdog disable, MPIDR read)
- Troubleshooting table (OS lock, KASLR, trace garbled, module symbols, EL3 access)

---

Switched model to Claude Sonnet 4.5

---

wHAT IS EXCEPTION ? iNTERRUPT? AND what is Exception mode

---


## Exceptions, Interrupts, and Exception Modes

Let me explain these fundamental ARM64 concepts:

---

## 1. Exception (ARM64 Context)

**Definition**: An exception is an event that causes the processor to stop normal execution and switch to a handler routine at a specific Exception Level (EL).

**Types of Exceptions in ARM64**:

```
┌─────────────────────────────────────────────────┐
│           ARM64 Exception Types                 │
├─────────────────────────────────────────────────┤
│ 1. Synchronous Exception                        │
│    - Instruction abort (page fault on fetch)    │
│    - Data abort (page fault on load/store)      │
│    - Undefined instruction                      │
│    - System call (SVC, HVC, SMC)                │
│    - Alignment fault                            │
│    - Breakpoint / Watchpoint (debug)            │
│                                                 │
│ 2. IRQ (Interrupt Request)                      │
│    - Normal hardware interrupt (peripheral)     │
│                                                 │
│ 3. FIQ (Fast Interrupt Request)                 │
│    - High-priority interrupt (lower latency)    │
│                                                 │
│ 4. SError (System Error)                        │
│    - Asynchronous abort (bus error, ECC error)  │
└─────────────────────────────────────────────────┘
```

**Key Characteristics**:
- **Synchronous**: Caused directly by instruction execution (e.g., page fault, syscall)
- **Asynchronous**: Caused by external events (e.g., IRQ, FIQ, SError)

---

## 2. Interrupt (Subset of Exceptions)

**Definition**: An interrupt is a specific type of **asynchronous exception** triggered by external hardware or internal timers.

**Interrupt vs Exception**:

```
All Interrupts are Exceptions
BUT
Not all Exceptions are Interrupts

Exception = Superset
   ├── Synchronous Exceptions (NOT interrupts)
   │     ├── Page faults
   │     ├── System calls
   │     └── Undefined instructions
   │
   └── Asynchronous Exceptions (INTERRUPTS + SError)
         ├── IRQ ← Hardware interrupt
         ├── FIQ ← Fast interrupt
         └── SError ← Async abort
```

**IRQ vs FIQ**:

| Feature | IRQ | FIQ |
|---------|-----|-----|
| Priority | Normal | Higher |
| Latency | Standard | Lower (dedicated registers) |
| Use Case | General peripherals | Time-critical (e.g., secure world) |
| Banked Registers | None | R8–R14 (ARM32 legacy) |

---

## 3. Exception Mode (ARM32 Legacy Concept)

In **ARM32 (AArch32)**, the CPU had distinct **processor modes**:

```
ARM32 Processor Modes (OBSOLETE in ARM64)
┌──────────────────────────────────────┐
│ Mode          | CPSR[4:0] | EL equiv │
├──────────────────────────────────────┤
│ User          | 0b10000   | EL0      │
│ FIQ           | 0b10001   | EL1      │  ← Exception mode
│ IRQ           | 0b10010   | EL1      │  ← Exception mode
│ Supervisor    | 0b10011   | EL1      │  ← Exception mode
│ Abort         | 0b10111   | EL1      │  ← Exception mode
│ Undefined     | 0b11011   | EL1      │  ← Exception mode
│ System        | 0b11111   | EL1      │
│ Monitor (EL3) | 0b10110   | EL3      │
│ Hyp (EL2)     | 0b11010   | EL2      │
└──────────────────────────────────────┘
```

**ARM32 Exception Modes**:
- Each exception type switched the CPU into a dedicated **mode** (FIQ mode, IRQ mode, etc.)
- Each mode had its own **banked stack pointer (SP)** and some banked registers
- Mode encoded in **CPSR[4:0]** (Current Program Status Register)

---

## 4. ARM64 Exception Model (Modern)

**ARM64 ELIMINATED exception modes** and replaced them with **Exception Levels (EL0–EL3)**.

### Exception Level Hierarchy

```
┌─────────────────────────────────────────┐
│  EL3 - Secure Monitor (ATF / BL31)      │  Highest privilege
│    Handles: SMC, Secure World entry     │
├─────────────────────────────────────────┤
│  EL2 - Hypervisor (KVM)                 │
│    Handles: HVC, VM context switch      │
├─────────────────────────────────────────┤
│  EL1 - Kernel (Linux)                   │
│    Handles: IRQ, FIQ, SVC, page faults  │
├─────────────────────────────────────────┤
│  EL0 - User Space (Applications)        │  Lowest privilege
│    Cannot handle exceptions directly    │
└─────────────────────────────────────────┘
```

### Exception Vectors (ARM64)

Each EL has a **vector table** with 16 entries (4 exception types × 4 sources):

```
Vector Table for EL1 (Linux Kernel)
Base Address: VBAR_EL1 (Vector Base Address Register)

Offset    Exception Type        Source
─────────────────────────────────────────────────────
0x000     Synchronous           Current EL, SP_EL0
0x080     IRQ                   Current EL, SP_EL0
0x100     FIQ                   Current EL, SP_EL0
0x180     SError                Current EL, SP_EL0

0x200     Synchronous           Current EL, SP_ELx
0x280     IRQ                   Current EL, SP_ELx
0x300     FIQ                   Current EL, SP_ELx
0x380     SError                Current EL, SP_ELx

0x400     Synchronous           Lower EL (AArch64)
0x480     IRQ                   Lower EL (AArch64)
0x500     FIQ                   Lower EL (AArch64)
0x580     SError                Lower EL (AArch64)

0x600     Synchronous           Lower EL (AArch32)
0x680     IRQ                   Lower EL (AArch32)
0x700     FIQ                   Lower EL (AArch32)
0x780     SError                Lower EL (AArch32)
```

**Linux Kernel Example**:

```c
// arch/arm64/kernel/entry.S
.align 11
ENTRY(vectors)
    ventry  el1_sync_invalid            // Synchronous EL1t
    ventry  el1_irq_invalid             // IRQ EL1t
    ventry  el1_fiq_invalid             // FIQ EL1t
    ventry  el1_error_invalid           // Error EL1t

    ventry  el1_sync                    // Synchronous EL1h ← Kernel IRQ/fault
    ventry  el1_irq                     // IRQ EL1h         ← Kernel IRQ
    ventry  el1_fiq                     // FIQ EL1h
    ventry  el1_error                   // Error EL1h

    ventry  el0_sync                    // Synchronous EL0  ← Syscall from user
    ventry  el0_irq                     // IRQ EL0          ← IRQ while in user
    ventry  el0_fiq                     // FIQ EL0
    ventry  el0_error                   // Error EL0
    ...
END(vectors)
```

---

## 5. What Happens During an Exception?

### Exception Entry Sequence (Hardware)

```
1. CPU saves state:
   ELR_ELx  ← PC (return address)
   SPSR_ELx ← PSTATE (processor state: NZCV, DAIF, EL, SP)

2. CPU updates state:
   PC       ← VBAR_ELx + offset  (jump to vector)
   PSTATE   ← Set exception masks (disable IRQ/FIQ)
   EL       ← Target EL (usually same or higher)
   SP       ← SP_ELx (switch to kernel stack)

3. Execute exception handler (software)

4. Return via ERET instruction:
   PC       ← ELR_ELx
   PSTATE   ← SPSR_ELx
```

### Saved Registers

| Register | Full Name | Purpose |
|----------|-----------|---------|
| **ELR_ELx** | Exception Link Register | Return address (PC at exception) |
| **SPSR_ELx** | Saved Program Status Register | PSTATE at exception (flags, EL, SP select) |
| **ESR_ELx** | Exception Syndrome Register | Exception type and cause (e.g., data abort at address X) |
| **FAR_ELx** | Fault Address Register | Virtual address that caused the fault |

Example: **Page Fault (Data Abort)**

```
1. User program: LDR X0, [X1]     // X1 = 0x1000 (unmapped)
2. Hardware:
   - ESR_EL1 ← 0x96000004         // Data abort, translation fault level 0
   - FAR_EL1 ← 0x1000              // Faulting address
   - ELR_EL1 ← PC of LDR           // Return here after handling
   - SPSR_EL1 ← PSTATE             // Save processor state
   - PC ← VBAR_EL1 + 0x400         // Jump to el0_sync vector
3. Linux handler:
   - Read ESR_EL1 → determine fault type
   - Read FAR_EL1 → get faulting address
   - Call do_page_fault()
   - Allocate page / swap in / kill process
4. Return: ERET → back to ELR_EL1
```

---

## 6. Exception Masking (DAIF Bits)

ARM64 uses **PSTATE.DAIF** to mask exceptions:

```
PSTATE.DAIF (Bits in PSTATE register)
┌───┬───┬───┬───┐
│ D │ A │ I │ F │
└───┴───┴───┴───┘
 │   │   │   └── F=1: FIQ masked (disabled)
 │   │   └────── I=1: IRQ masked (disabled)
 │   └────────── A=1: SError masked (disabled)
 └────────────── D=1: Debug exceptions masked
```

**Control Instructions**:

```assembly
MSR DAIFSet, #0xF   ; Disable all interrupts (D=A=I=F=1)
MSR DAIFClr, #0x2   ; Enable IRQ (I=0)
```

**Linux Usage**:

```c
// Disable interrupts
local_irq_disable();         // Sets PSTATE.I = 1

// Enable interrupts
local_irq_enable();          // Clears PSTATE.I = 0

// Save and restore
unsigned long flags;
local_irq_save(flags);       // Save DAIF, set I=1
// ... critical section ...
local_irq_restore(flags);    // Restore previous DAIF
```

---

## 7. ARM32 vs ARM64 Exception Handling

| Aspect | ARM32 (AArch32) | ARM64 (AArch64) |
|--------|----------------|----------------|
| **Privilege Model** | 7 processor modes | 4 Exception Levels (EL0–EL3) |
| **Exception Modes** | FIQ, IRQ, SVC, ABT, UND modes | No modes — all handled at current or higher EL |
| **Stack Pointers** | Banked SP per mode | SP_EL0, SP_EL1, SP_EL2, SP_EL3 |
| **Return Address** | LR (Link Register) | ELR_ELx (Exception Link Register) |
| **Saved State** | SPSR (one per mode) | SPSR_ELx (one per EL) |
| **Vector Table** | 8 entries (fixed offset) | 16 entries (4 types × 4 sources) |
| **Return Instruction** | `MOVS PC, LR` or `SUBS PC, LR, #4` | `ERET` |

---

## 8. Summary Table

| Term | Definition | Example |
|------|------------|---------|
| **Exception** | Event causing CPU to jump to handler | Page fault, IRQ, syscall, undefined instruction |
| **Interrupt** | Asynchronous exception from hardware | Timer tick, UART RX, GPIO edge |
| **Synchronous Exception** | Caused by instruction execution | `SVC` (syscall), memory fault, breakpoint |
| **Asynchronous Exception** | External event | IRQ, FIQ, SError |
| **Exception Level (EL)** | ARM64 privilege ring (0=user, 1=kernel, 2=hypervisor, 3=secure) | Linux runs at EL1 |
| **Exception Mode** | ARM32 concept — dedicated CPU mode per exception type | **Obsolete in ARM64** |
| **Vector Table** | Jump table for exception handlers | `VBAR_EL1` → 16 vectors × 128 bytes each |
| **ELR_ELx** | Return address after exception | Saved PC |
| **SPSR_ELx** | Saved processor state | Saved PSTATE (flags, EL, etc.) |
| **ESR_ELx** | Exception cause/syndrome | Data abort syndrome, syscall number |
| **DAIF** | Interrupt mask bits | D=Debug, A=SError, I=IRQ, F=FIQ |

---

## 9. Practical Example: System Call (SVC)

```
User Application (EL0):
──────────────────────
    mov  x8, #64              // Syscall number: write()
    mov  x0, #1               // fd = stdout
    ldr  x1, =msg             // buf
    mov  x2, #13              // count
    svc  #0                   // Trigger exception
    // ← Return here after syscall

Hardware (automatic):
─────────────────────
    ELR_EL1  ← PC + 4         // Return to next instruction
    SPSR_EL1 ← PSTATE         // Save flags, EL0, etc.
    ESR_EL1  ← 0x5600xxxx     // SVC syndrome
    PC       ← VBAR_EL1 + 0x400  // el0_sync vector
    PSTATE   ← EL1, I=1, SP=SP_EL1

Linux Kernel Handler (EL1):
────────────────────────────
el0_sync:
    kernel_entry 0            // Save all registers to stack
    mrs x25, ESR_EL1          // Read exception cause
    lsr x24, x25, #26         // Extract EC (Exception Class)
    cmp x24, #0x15            // Is it SVC?
    b.eq el0_svc              // Jump to syscall handler

el0_svc:
    adrp stbl, sys_call_table
    ldr x16, [stbl, x8, lsl #3]  // Load syscall handler address
    blr x16                      // Call sys_write()
    kernel_exit 0             // Restore registers
    eret                      // Return to EL0 (ELR_EL1 → PC)
```

---

## 10. TRACE32 Commands for Exception Debugging

```tcl
; View vector table address
Register.View VBAR_EL1

; View exception syndrome after exception
Register.View ESR_EL1

; View faulting address (data abort)
Register.View FAR_EL1

; View return address
Register.View ELR_EL1

; View saved processor state
Register.View SPSR_EL1

; Set breakpoint at exception vector
Break.Set VBAR_EL1+0x400 /Program   ; el0_sync entry

; Trace IRQ handling
ETM.TRIGGER.Set ADDRESS el1_irq
ETM.ON
Go
```

---

**Key Takeaway**: In ARM64, there are **no exception modes** — only **Exception Levels**. All exceptions at EL1 (Linux kernel) are handled by a unified vector table, with the specific exception type determined by reading **ESR_EL1** in software.
