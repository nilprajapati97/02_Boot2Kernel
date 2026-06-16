# Exception Levels — EL0 to EL3

## 1. What are Exception Levels?

Exception Levels (ELs) are **privilege levels** in ARMv8. They control what the software
can access — higher ELs have more privilege (access to more system registers, memory
regions, and hardware features).

```mermaid
flowchart TD
    EL3["<b>EL3 — Secure Monitor</b><br/>ARM Trusted Firmware / TF-A<br/>Controls Secure ↔ Non-Secure transitions<br/><i>Highest Privilege</i>"]
    EL2["<b>EL2 — Hypervisor</b><br/>KVM, Xen, or bare-metal<br/>Controls virtualization, stage-2 translation"]
    EL1["<b>EL1 — OS Kernel</b><br/>Linux, Windows, RTOS<br/>Controls virtual memory (stage-1), device drivers"]
    EL0["<b>EL0 — User Applications</b><br/>Your programs, web browsers, etc.<br/><i>Lowest Privilege</i>"]

    EL3 -->|"Decreasing Privilege"| EL2
    EL2 --> EL1
    EL1 --> EL0

    style EL3 fill:#d32f2f,color:#fff,stroke:#b71c1c
    style EL2 fill:#f57c00,color:#fff,stroke:#e65100
    style EL1 fill:#1976d2,color:#fff,stroke:#0d47a1
    style EL0 fill:#388e3c,color:#fff,stroke:#1b5e20
```

---

## 2. Detailed Role of Each Exception Level

### EL0 — User/Application Level

- **What runs here**: User applications (ls, vim, Chrome, your code)
- **Privileges**: Minimal
  - Can access unprivileged system registers only
  - Cannot directly access hardware or change page tables
  - Cannot disable interrupts
  - Must use `SVC` (supervisor call) to request OS services
- **Key registers accessible**: General-purpose registers, NZCV flags, some timers

### EL1 — Kernel/Supervisor Level

- **What runs here**: Operating system kernel
- **Privileges**: Controls the application environment
  - Configures virtual memory (page tables, MMU)
  - Handles exceptions from EL0 (syscalls, page faults, etc.)
  - Manages interrupts (with GIC programming)
  - Access to `SCTLR_EL1`, `TCR_EL1`, `TTBR0_EL1`, `TTBR1_EL1`, etc.
- **Key system registers**:
  ```
  SCTLR_EL1   — System Control Register (MMU enable, caches, alignment)
  TCR_EL1      — Translation Control Register
  TTBR0_EL1    — Translation Table Base Register 0 (user space)
  TTBR1_EL1    — Translation Table Base Register 1 (kernel space)
  VBAR_EL1     — Vector Base Address Register (exception vectors)
  ESR_EL1      — Exception Syndrome Register (exception cause)
  FAR_EL1      — Fault Address Register
  MAIR_EL1     — Memory Attribute Indirection Register
  ```

### EL2 — Hypervisor Level

- **What runs here**: Hypervisor (virtual machine manager)
- **Privileges**: Controls guest OS environments 
  - Stage-2 address translation (guest physical → physical)
  - Trap-and-emulate guest OS register accesses
  - Virtual interrupt injection
  - Controls what EL1 can see/do
- **Key system registers**:
  ```
  HCR_EL2     — Hypervisor Configuration Register
  VTTBR_EL2   — Virtualization Translation Table Base
  VTCR_EL2    — Virtualization Translation Control
  VMPIDR_EL2  — Virtualized Multiprocessor Affinity Register
  VPIDR_EL2   — Virtualized Processor ID Register
  ```
- **VHE (Virtualization Host Extensions)** — ARMv8.1:
  Allows the host OS kernel to run at EL2 directly, avoiding EL2→EL1 overhead

### EL3 — Secure Monitor Level

- **What runs here**: Secure monitor firmware (ARM Trusted Firmware / TF-A)
- **Privileges**: Absolute — controls security state
  - Manages Secure ↔ Non-Secure world transitions
  - Controls which EL2/EL1 configurations are possible
  - Cannot be bypassed by any other level
- **Key system registers**:
  ```
  SCR_EL3     — Secure Configuration Register
  SCTLR_EL3   — System Control Register for EL3
  ```

---

## 3. Exception Level Transitions

Software can only change ELs through **exceptions** (going up) and **exception returns** (going down):

```mermaid
flowchart TB
    subgraph entry["Exception Entry — to Same or Higher EL"]
        direction TB
        e0["EL0"] -->|"SVC / Page Fault"| e1["EL1"]
        e1 -->|"HVC / Trap"| e2["EL2"]
        e1 -->|"SMC"| e3["EL3"]
        eA["Any EL"] -.->|"IRQ if routed"| e1
        eA -.->|"FIQ if routed"| e3
    end

    subgraph ret["Exception Return — ERET to Same or Lower EL"]
        direction TB
        r3["EL3"] -->|"ERET"| r1a["EL1"]
        r2["EL2"] -->|"ERET"| r1b["EL1"]
        r1c["EL1"] -->|"ERET"| r0["EL0"]
    end

    style e0 fill:#388e3c,color:#fff,stroke:#1b5e20
    style e1 fill:#1976d2,color:#fff,stroke:#0d47a1
    style e2 fill:#f57c00,color:#fff,stroke:#e65100
    style e3 fill:#d32f2f,color:#fff,stroke:#b71c1c
    style eA fill:#7b1fa2,color:#fff,stroke:#4a148c
    style r0 fill:#388e3c,color:#fff,stroke:#1b5e20
    style r1a fill:#1976d2,color:#fff,stroke:#0d47a1
    style r1b fill:#1976d2,color:#fff,stroke:#0d47a1
    style r1c fill:#1976d2,color:#fff,stroke:#0d47a1
    style r2 fill:#f57c00,color:#fff,stroke:#e65100
    style r3 fill:#d32f2f,color:#fff,stroke:#b71c1c
```

### Exception Entry Process (Hardware)

When an exception is taken to target ELn:

```
Step 1: Save the return address
        ELR_ELn = address to return to

Step 2: Save the processor state
        SPSR_ELn = PSTATE (condition flags, masks, EL, etc.)

Step 3: Set the new PSTATE
        PSTATE.EL = n (target exception level)
        PSTATE.SP = 1 (use SP_ELn by default)
        PSTATE.{D,A,I,F} = 1 (mask all interrupts/debug)
        PSTATE.nRW = 0 (AArch64 if target is AArch64)

Step 4: Jump to the exception vector
        PC = VBAR_ELn + offset (based on exception type)
```

### Exception Return Process (Software)

```
ERET instruction:
  Step 1: PC = ELR_ELn     (restore saved return address)
  Step 2: PSTATE = SPSR_ELn (restore saved processor state)
  Step 3: Continue execution at old EL
```

---

## 4. Exception Vectors (VBAR)

Each exception level has its own vector table, pointed to by `VBAR_ELn`.
The vector table has 16 entries, organized by the source of the exception:

```mermaid
flowchart TD
    VBAR["VBAR_ELn"]

    subgraph G1["Current EL with SP_EL0"]
        A1["0x000: Synchronous"]
        A2["0x080: IRQ / vIRQ"]
        A3["0x100: FIQ / vFIQ"]
        A4["0x180: SError / vSError"]
    end

    subgraph G2["Current EL with SP_ELn"]
        B1["0x200: Synchronous"]
        B2["0x280: IRQ / vIRQ"]
        B3["0x300: FIQ / vFIQ"]
        B4["0x380: SError / vSError"]
    end

    subgraph G3["Lower EL using AArch64"]
        C1["0x400: Synchronous"]
        C2["0x480: IRQ / vIRQ"]
        C3["0x500: FIQ / vFIQ"]
        C4["0x580: SError / vSError"]
    end

    subgraph G4["Lower EL using AArch32"]
        D1["0x600: Synchronous"]
        D2["0x680: IRQ / vIRQ"]
        D3["0x700: FIQ / vFIQ"]
        D4["0x780: SError / vSError"]
    end

    VBAR --> G1
    VBAR --> G2
    VBAR --> G3
    VBAR --> G4

    style VBAR fill:#1565c0,color:#fff,stroke:#0d47a1
    style G1 fill:#e3f2fd,stroke:#1565c0
    style G2 fill:#fff3e0,stroke:#e65100
    style G3 fill:#e8f5e9,stroke:#2e7d32
    style G4 fill:#fce4ec,stroke:#c62828
```

> Each vector entry has 128 bytes (32 instructions) of space.
> Typically, the handler saves context and branches to a full handler.

---

## 5. Exception Types

### 5.1 Synchronous Exceptions

Generated by the executing instruction, deterministic:

| Exception            | Cause                                    | ESR.EC Code |
|----------------------|------------------------------------------|-------------|
| SVC                  | Supervisor Call from EL0                 | 0x15        |
| HVC                  | Hypervisor Call from EL1                 | 0x16        |
| SMC                  | Secure Monitor Call                      | 0x17        |
| Instruction Abort    | Failed instruction fetch (page fault)    | 0x20/0x21   |
| Data Abort           | Failed data access (page fault)          | 0x24/0x25   |
| SP Alignment Fault   | Unaligned stack pointer                  | 0x26        |
| PC Alignment Fault   | Unaligned PC                             | 0x22        |
| Illegal Execution    | Illegal instruction execution state      | 0x0E        |
| Trapped instruction  | Access to trapped system register/insn   | Various     |
| Breakpoint           | Software breakpoint (BRK)                | 0x3C        |
| Watchpoint           | Data watchpoint hit                      | 0x34/0x35   |

### 5.2 Asynchronous Exceptions

Events from outside the instruction stream:

| Exception | Source                         | Typical Routing      |
|-----------|--------------------------------|----------------------|
| IRQ       | Normal interrupts (GIC)        | EL1 (OS kernel)      |
| FIQ       | Fast/secure interrupts         | EL3 (Secure Monitor) |
| SError    | Asynchronous system errors     | Configurable         |

---

## 6. Exception Syndrome Register (ESR_ELn)

When a synchronous exception occurs, the hardware writes the **cause** into ESR_ELn:

**ESR_ELn** (64-bit in ARMv8.2+, 32-bit prior):

| Field | Bits | Size | Description |
|-------|------|------|-------------|
| **EC** (Exception Class) | [31:26] | 6 bits | What kind of exception |
| **IL** (Instruction Length) | [25] | 1 bit | 0 = 16-bit instruction, 1 = 32-bit instruction |
| **ISS** (Instruction Specific Syndrome) | [24:0] | 25 bits | Additional details depending on EC (e.g., read/write, access size) |

### Reading Exception Cause Example

```
MRS X0, ESR_EL1         // Read exception syndrome
LSR X1, X0, #26         // Extract EC field (bits 31:26)

// X1 now contains the exception class:
//   0x15 = SVC from AArch64
//   0x24 = Data Abort from lower EL
//   0x25 = Data Abort from same EL
```

---

## 7. Security States

ARMv8 defines two security states, controlled by EL3:

```mermaid
flowchart TD
    subgraph Secure["Secure World — SCR_EL3.NS = 0"]
        direction TB
        SEL3["EL3 — Secure Monitor"]
        SEL2["S-EL2 — Secure Hypervisor<br/>(ARMv8.4+, optional)"]
        SEL1["S-EL1 — Secure OS<br/>(OP-TEE, Trusty)"]
        SEL0["S-EL0 — Secure Applications<br/>(Trusted Apps)"]
        SEL3 --> SEL2 --> SEL1 --> SEL0
    end

    subgraph NonSecure["Non-Secure World — SCR_EL3.NS = 1"]
        direction TB
        NEL2["EL2 — Hypervisor"]
        NEL1["EL1 — OS Kernel (Linux)"]
        NEL0["EL0 — User Applications"]
        NEL2 --> NEL1 --> NEL0
    end

    style Secure fill:#ffebee,stroke:#c62828
    style NonSecure fill:#e3f2fd,stroke:#0d47a1
    style SEL3 fill:#d32f2f,color:#fff,stroke:#b71c1c
    style SEL2 fill:#e53935,color:#fff,stroke:#b71c1c
    style SEL1 fill:#ef5350,color:#fff,stroke:#c62828
    style SEL0 fill:#ef9a9a,color:#000,stroke:#c62828
    style NEL2 fill:#1565c0,color:#fff,stroke:#0d47a1
    style NEL1 fill:#1976d2,color:#fff,stroke:#0d47a1
    style NEL0 fill:#64b5f6,color:#000,stroke:#0d47a1
```

---

## 8. Practical: Boot Flow Through Exception Levels

```mermaid
flowchart TD
    PO["⚡ Power On"] --> EL3
    EL3["<b>EL3</b><br/>ROM / Bootloader (BL1 in TF-A)<br/>• Initialize secure world<br/>• Set SCR_EL3 (configure EL2 width, routing)<br/>• Load BL2 (trusted boot firmware)"]
    EL3 -->|"ERET to EL2"| EL2
    EL2["<b>EL2</b><br/>Bootloader stage (BL33 = U-Boot or UEFI)<br/>• Optional: configure hypervisor<br/>• For bare metal: may go directly to EL1"]
    EL2 -->|"ERET to EL1"| EL1
    EL1["<b>EL1</b><br/>OS Kernel (Linux)<br/>• Set up page tables (TTBR0 / TTBR1)<br/>• Enable MMU<br/>• Initialize drivers, mount rootfs"]
    EL1 -->|"ERET to EL0"| EL0
    EL0["<b>EL0</b><br/>init process → user applications<br/>• System calls via SVC → EL1"]

    style PO fill:#424242,color:#fff,stroke:#212121
    style EL3 fill:#d32f2f,color:#fff,stroke:#b71c1c
    style EL2 fill:#f57c00,color:#fff,stroke:#e65100
    style EL1 fill:#1976d2,color:#fff,stroke:#0d47a1
    style EL0 fill:#388e3c,color:#fff,stroke:#1b5e20
```

---

Next: [Registers →](./03_Registers.md)
