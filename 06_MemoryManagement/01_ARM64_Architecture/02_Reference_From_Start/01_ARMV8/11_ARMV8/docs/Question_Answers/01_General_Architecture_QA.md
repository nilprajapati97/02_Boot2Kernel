# General ARMv8 Architecture — Questions & Answers

---

## Q1. [L1] What is the difference between ARMv8-A, ARMv8-R, and ARMv8-M profiles? Why does ARM have multiple profiles?

**Answer:**

ARM designs a single instruction set architecture (ISA) but targets vastly different markets — from tiny microcontrollers to server CPUs. One-size-fits-all would mean a 1000-gate MCU carries the complexity of server virtualization, which is wasteful. So ARM splits the architecture into **profiles**:

```
┌──────────┬───────────────────────────────────────────────────────────┐
│ Profile  │ Purpose & Characteristics                                 │
├──────────┼───────────────────────────────────────────────────────────┤
│ A        │ Application Profile                                       │
│ (ARMv8-A)│ • Full virtual memory (MMU with page tables)             │
│          │ • 4 Exception Levels (EL0-EL3)                           │
│          │ • Virtualization support (EL2)                            │
│          │ • TrustZone security                                     │
│          │ • Runs rich OS (Linux, Windows, Android)                  │
│          │ • Examples: Cortex-A53, A76, X4, Neoverse N2            │
│          │ • Markets: phones, laptops, servers, automotive infotain.│
├──────────┼───────────────────────────────────────────────────────────┤
│ R        │ Real-Time Profile                                         │
│ (ARMv8-R)│ • MPU (Memory Protection Unit) — no page tables          │
│          │   (optional MMU in newer specs)                          │
│          │ • Deterministic interrupt latency (hard real-time)       │
│          │ • Lockstep/ECC for safety-critical applications          │
│          │ • 2 Exception Levels (EL0-EL2)                          │
│          │ • Examples: Cortex-R52, R82                              │
│          │ • Markets: automotive ADAS, industrial, storage ctrlrs   │
├──────────┼───────────────────────────────────────────────────────────┤
│ M        │ Microcontroller Profile                                   │
│ (ARMv8-M)│ • No MMU, simple MPU for memory protection              │
│          │ • Vector table for interrupt dispatch (NVIC)             │
│          │ • TrustZone-M (simplified security)                     │
│          │ • Thumb-2 instruction set (compact code)                │
│          │ • Examples: Cortex-M33, M55, M85                        │
│          │ • Markets: IoT sensors, wearables, motor control        │
└──────────┴───────────────────────────────────────────────────────────┘
```

**Key insight for senior interviews**: The R profile is expanding — Cortex-R82 added MMU support and can run Linux, blurring the A/R boundary for safety-critical automotive compute (ASIL-D). Understanding where profiles overlap shows architectural depth.

---

## Q2. [L1] Explain the two Execution States in ARMv8: AArch64 and AArch32. When and how does the CPU switch between them?

**Answer:**

ARMv8 defines two **execution states** that determine the register width, instruction set, and exception model:

```
AArch64:
  • 64-bit general-purpose registers (X0-X30), 64-bit PC, 64-bit SP
  • A64 instruction set (fixed 32-bit encoding, all new)
  • 4 Exception Levels (EL0-EL3) with independent stack pointers
  • 31 GPRs + SP + PC (not accessible as GPR)
  • Condition flags in PSTATE (not a full CPSR)

AArch32:
  • 32-bit registers (R0-R15, R15=PC)
  • A32 (ARM) and T32 (Thumb) instruction sets
  • Backward compatible with ARMv7
  • Banked registers per mode (FIQ, IRQ, SVC, etc.)
```

**Switching rules** — this is where senior engineers get tripped up:

```
Switching can ONLY happen on exception entry/return:

  Higher EL determines lower EL's execution state:
  ┌───────────────────────────────────────────────────────────────┐
  │ EL3 (SCR_EL3.RW)  → controls EL2 execution state           │
  │ EL2 (HCR_EL2.RW)  → controls EL1 execution state           │
  │ EL1                → controls EL0 execution state            │
  │                      (PSTATE on ERET determines EL0 state)  │
  └───────────────────────────────────────────────────────────────┘

  Rules:
  1. A higher EL can run AArch64 while lower EL runs AArch32
  2. A higher EL CANNOT run AArch32 if lower EL was AArch64
     (you can't go "down" in width at higher privilege)
  3. EL3 and EL2 typically always run AArch64
  4. EL1 may run AArch32 (e.g., 32-bit guest OS in a VM)
  5. EL0 may run AArch32 (e.g., 32-bit app on 64-bit kernel)

  Valid combinations:
    EL3=64, EL2=64, EL1=64, EL0=64  ✓  (pure 64-bit)
    EL3=64, EL2=64, EL1=64, EL0=32  ✓  (32-bit apps)
    EL3=64, EL2=64, EL1=32, EL0=32  ✓  (32-bit guest OS)
    EL3=64, EL2=32, EL1=32, EL0=32  ✗  INVALID
```

**Practical example**: Android running a legacy 32-bit APK — the kernel (EL1) stays AArch64, but the process runs AArch32 at EL0. On context switch to a 64-bit process, the kernel changes PSTATE in SPSR and ERETs to AArch64 EL0.

---

## Q3. [L2] Explain all four Exception Levels (EL0-EL3) in detail. What software runs at each level and why?

**Answer:**

```
┌─────────────────────────────────────────────────────────────────┐
│ EL3 — Secure Monitor (HIGHEST PRIVILEGE)                        │
│ ───────────────────────────────────────                          │
│ • Only code that can switch between Secure and Normal worlds   │
│ • SCR_EL3.NS bit controls world: 0=Secure, 1=Normal           │
│ • Handles SMC (Secure Monitor Call) instructions               │
│ • Implements PSCI (CPU power management)                       │
│ • Software: ARM Trusted Firmware (TF-A / ATF)                 │
│ • Survives entire system lifetime (never unloaded)             │
│ • Has its own VBAR_EL3, SCTLR_EL3, page tables               │
│                                                                  │
│ Why separate? Trust boundary — even if hypervisor is           │
│ compromised, it cannot affect Secure World operations.         │
├─────────────────────────────────────────────────────────────────┤
│ EL2 — Hypervisor                                                │
│ ───────────────────                                              │
│ • Controls Stage-2 address translation (IPA → PA)             │
│ • Can trap any EL1/EL0 operation via HCR_EL2                  │
│ • Manages virtual interrupts (ICH_LR registers)               │
│ • Owns VMID namespace (isolates VMs)                           │
│ • Software: KVM, Xen, Hafnium, proprietary hypervisors        │
│ • Optional: if no hypervisor, boot firmware drops to EL1       │
│                                                                  │
│ With VHE (ARMv8.1): Host kernel runs AT EL2 directly          │
│ → Linux kernel at EL2, apps at EL0, guests at EL1             │
├─────────────────────────────────────────────────────────────────┤
│ EL1 — OS Kernel                                                 │
│ ──────────────                                                   │
│ • Manages virtual memory (Stage-1 page tables)                 │
│ • Handles exceptions/interrupts from EL0                       │
│ • Controls system registers: SCTLR_EL1, TCR_EL1, etc.        │
│ • Has its own VBAR_EL1 exception vector table                  │
│ • Software: Linux kernel, Windows kernel, RTOS                │
│ • In a VM: thinks it has full control, but EL2 oversees       │
├─────────────────────────────────────────────────────────────────┤
│ EL0 — User Applications (LOWEST PRIVILEGE)                      │
│ ──────────────────────────────────────────                       │
│ • Cannot access system registers (trapped to EL1)             │
│ • Cannot execute privileged instructions                       │
│ • Memory access governed by EL1's page tables                  │
│ • Uses SVC instruction to make system calls → EL1             │
│ • Software: all user-space apps, libraries                    │
│ • Isolation: one EL0 process cannot access another's memory   │
└─────────────────────────────────────────────────────────────────┘
```

**Senior-level follow-up**: "What happens if EL2 is not implemented?" 
→ SCR_EL3.HCE and EL2 trap controls don't exist. Boot firmware must enter EL1 directly. Linux detects no EL2 and disables KVM. PSCI CPU_ON still works (EL3 handles it). Some SoCs implement EL2 but firmware "uses it up" and enters EL1, preventing Linux KVM — a common problem on Android devices.

---

## Q4. [L2] What is PSTATE? How does it differ from ARMv7's CPSR?

**Answer:**

In ARMv7 (AArch32), the CPSR (Current Program Status Register) was a single 32-bit register containing condition flags, interrupt masks, mode bits, and execution state — all in one place, directly readable/writable.

In AArch64, ARM broke this apart:

```
PSTATE is NOT a single register — it's a collection of fields
scattered across multiple system registers and accessed via
individual MSR/MRS instructions:

┌──────────────────────────────────────────────────────────────┐
│ PSTATE Field │ Bits │ Description                            │
├──────────────┼──────┼────────────────────────────────────────┤
│ N            │ [31] │ Negative flag                          │
│ Z            │ [30] │ Zero flag                              │
│ C            │ [29] │ Carry flag                             │
│ V            │ [28] │ Overflow flag                          │
│ D            │ [9]  │ Debug exception mask                   │
│ A            │ [8]  │ SError (async abort) mask              │
│ I            │ [7]  │ IRQ mask                               │
│ F            │ [6]  │ FIQ mask                               │
│ SS           │ [21] │ Software Step active                   │
│ IL           │ [20] │ Illegal Execution state                │
│ EL           │[3:2] │ Current Exception Level (read-only)    │
│ nRW          │ [4]  │ Execution state: 0=AArch64             │
│ SP           │ [0]  │ Stack pointer select: 0=SP_EL0, 1=SP_ELx│
│ PAN          │ [22] │ Privileged Access Never (ARMv8.1)      │
│ UAO          │ [23] │ User Access Override (ARMv8.2)         │
│ DIT          │ [24] │ Data Independent Timing (ARMv8.4)      │
│ TCO          │ [25] │ Tag Check Override — MTE (ARMv8.5)     │
│ BTYPE        │[11:10]│ Branch Type (BTI, ARMv8.5)            │
│ SSBS         │ [12] │ Speculative Store Bypass Safe          │
│ ALLINT       │ [13] │ All interrupt mask (ARMv8.8)           │
└──────────────┴──────┴────────────────────────────────────────┘

Key differences from ARMv7 CPSR:
  1. No "mode bits" — EL replaces processor modes (SVC/IRQ/FIQ/etc.)
  2. PSTATE.EL is read-only — you change EL via exceptions/ERET
  3. No T bit — AArch64 is always A64 (no ARM/Thumb switching)
  4. DAIF masking replaces I/F bits with finer control
  5. PSTATE is saved to SPSR_ELx on exception entry
  6. Individual fields accessed via: MSR DAIFSet, #imm / MSR PAN, #1

Access:
  MSR DAIFSet, #0xF    // Mask all interrupts (DAIF = 1111)
  MSR DAIFClr, #0x2    // Unmask IRQ (clear I bit)
  MRS X0, NZCV         // Read condition flags
  MRS X0, CurrentEL    // Read current EL
```

---

## Q5. [L1] What is the SVC, HVC, and SMC instruction? When does software use each one?

**Answer:**

These three instructions are the **software-generated exception gateways** between exception levels:

```
┌─────────┬────────────────────────────────────────────────────────┐
│ Instr.  │ Meaning                                                │
├─────────┼────────────────────────────────────────────────────────┤
│ SVC #n  │ Supervisor Call: EL0 → EL1                            │
│         │ User app calls kernel (system call)                   │
│         │ Example: read(), write(), mmap() in Linux             │
│         │ Registers: X8 = syscall number, X0-X5 = args          │
│         │ Returns via ERET from EL1 back to EL0                 │
├─────────┼────────────────────────────────────────────────────────┤
│ HVC #n  │ Hypervisor Call: EL1 → EL2                            │
│         │ Guest OS calls hypervisor                              │
│         │ Example: KVM_HYPERCALL, Xen PV operations             │
│         │ Can be disabled: HCR_EL2.HCD=1 → UNDEFINED           │
│         │ Also trapped: if SCR_EL3.HCE=0 → UNDEFINED           │
├─────────┼────────────────────────────────────────────────────────┤
│ SMC #n  │ Secure Monitor Call: EL1/EL2 → EL3                   │
│         │ Normal World calls Secure Monitor                     │
│         │ Example: PSCI_CPU_ON, OP-TEE trusted service calls    │
│         │ Can be trapped at EL2: HCR_EL2.TSC=1 → EL2 first    │
│         │ Can be disabled: SCR_EL3.SMD=1 → UNDEFINED           │
└─────────┴────────────────────────────────────────────────────────┘

Call flow example — Linux syscall + PSCI:

  User app (EL0):
    MOV X8, #64          // SYS_write
    MOV X0, #1           // fd = stdout
    SVC #0               // → EL1
      │
  Linux kernel (EL1):
    // Handles write() syscall
    // Later, wants to power off a CPU:
    MOV X0, #0xC4000002  // PSCI_CPU_OFF
    SMC #0               // → EL3
      │
  ATF Secure Monitor (EL3):
    // Powers off the core
    // Returns to EL1 via ERET

FAQ in interviews:
  Q: "Can EL0 call SMC directly?"
  A: No! SMC from EL0 is UNDEFINED. EL0 must go through EL1 first
     (SVC → kernel → SMC).
  
  Q: "What if a guest VM (EL1) executes SMC?"
  A: If HCR_EL2.TSC=1, it traps to EL2 (hypervisor intercepts).
     Hypervisor decides whether to forward to EL3 or emulate.
```

---

## Q6. [L2] Describe the complete exception entry and return sequence in AArch64. What exactly does the hardware do?

**Answer:**

```
When an exception occurs (e.g., IRQ, SVC, page fault):

═══ HARDWARE DOES THIS AUTOMATICALLY ═══

Step 1: Save PSTATE → SPSR_ELx
  The ENTIRE current PSTATE (flags, masks, EL, SP selection)
  is saved into SPSR of the TARGET exception level.
  e.g., IRQ from EL0 → SPSR_EL1 = current PSTATE

Step 2: Save return address → ELR_ELx
  For synchronous exceptions: ELR = address of the faulting instr.
  For async (IRQ/FIQ/SError): ELR = address of next instruction
  (the one that would have executed if no interrupt)

Step 3: Update PSTATE:
  • PSTATE.EL = target exception level
  • PSTATE.SP = 1 (use SP_ELx, not SP_EL0)
  • PSTATE.DAIF: masks set based on exception type
    - IRQ: I=1 (mask IRQ), A may be set
    - FIQ: F=1, I=1
    - SError/Debug: appropriate masks
  • PSTATE.nRW = 0 (always enter AArch64 at EL1+)
  • PSTATE.SS = 0 (clear single-step)
  • PSTATE.IL = 0

Step 4: Branch to exception vector
  PC = VBAR_ELx + offset
  
  Vector table offsets (VBAR_ELx base):
  ┌────────┬──────────────────────────────────────────┐
  │ Offset │ Exception from...                         │
  ├────────┼──────────────────────────────────────────┤
  │ 0x000  │ Synchronous, Current EL with SP_EL0     │
  │ 0x080  │ IRQ/vIRQ,    Current EL with SP_EL0     │
  │ 0x100  │ FIQ/vFIQ,    Current EL with SP_EL0     │
  │ 0x180  │ SError,      Current EL with SP_EL0     │
  │ 0x200  │ Synchronous, Current EL with SP_ELx     │
  │ 0x280  │ IRQ/vIRQ,    Current EL with SP_ELx     │
  │ 0x300  │ FIQ/vFIQ,    Current EL with SP_ELx     │
  │ 0x380  │ SError,      Current EL with SP_ELx     │
  │ 0x400  │ Synchronous, Lower EL, AArch64          │
  │ 0x480  │ IRQ/vIRQ,    Lower EL, AArch64          │
  │ 0x500  │ FIQ/vFIQ,    Lower EL, AArch64          │
  │ 0x580  │ SError,      Lower EL, AArch64          │
  │ 0x600  │ Synchronous, Lower EL, AArch32          │
  │ 0x680  │ IRQ/vIRQ,    Lower EL, AArch32          │
  │ 0x700  │ FIQ/vFIQ,    Lower EL, AArch32          │
  │ 0x780  │ SError,      Lower EL, AArch32          │
  └────────┴──────────────────────────────────────────┘
  
  Each vector entry has 0x80 bytes (32 instructions) — enough for
  a branch to the real handler.

═══ SOFTWARE DOES THIS ═══

Step 5: Vector entry code saves context:
  STP X29, X30, [SP, #-16]!   // Push frame pointer + LR
  STP X0, X1, [SP, #-16]!     // Push argument registers
  ...save all needed registers...
  MRS X0, ESR_EL1              // Read Exception Syndrome Register
  MRS X1, FAR_EL1              // Read Fault Address Register (if relevant)
  // Determine exception cause from ESR_EL1.EC (Exception Class)
  // Dispatch to appropriate handler

Step 6: Handle the exception (interrupt, fault, syscall, etc.)

Step 7: Restore context and return:
  ...restore registers from stack...
  LDP X0, X1, [SP], #16
  LDP X29, X30, [SP], #16
  ERET                         // Return from exception

═══ HARDWARE ON ERET ═══

Step 8: ERET atomically does:
  • PC = ELR_ELx (return address)
  • PSTATE = SPSR_ELx (restored state)
  • Resume execution at the return EL
```

---

## Q7. [L3] What is ESR_ELx (Exception Syndrome Register)? How does software use it to determine exception cause?

**Answer:**

```
ESR_ELx is the KEY register for determining WHY an exception occurred.

ESR_EL1 Format:
┌─────────────────────────────────────────────────────────────┐
│ Bits [31:26]: EC — Exception Class (what TYPE of exception) │
│ Bit  [25]:    IL — Instruction Length (0=16-bit, 1=32-bit) │
│ Bits [24:0]:  ISS — Instruction-Specific Syndrome (details)│
└─────────────────────────────────────────────────────────────┘

Common EC values (the ones you MUST know):
┌────────┬───────────────────────────────────────────────────────┐
│ EC     │ Exception Class                                       │
├────────┼───────────────────────────────────────────────────────┤
│ 0x00   │ Unknown reason                                        │
│ 0x01   │ Trapped WFI/WFE                                       │
│ 0x07   │ SVE/SIMD/FP access trap (CPACR_EL1 disabled)        │
│ 0x15   │ SVC instruction (system call from AArch64)           │
│ 0x16   │ HVC instruction (hypervisor call)                    │
│ 0x17   │ SMC instruction (secure monitor call)                │
│ 0x18   │ MSR/MRS/System instruction trap                      │
│ 0x20   │ Instruction Abort from lower EL                      │
│ 0x21   │ Instruction Abort from current EL                    │
│ 0x22   │ PC Alignment fault                                   │
│ 0x24   │ Data Abort from lower EL (page fault, permission)   │
│ 0x25   │ Data Abort from current EL                           │
│ 0x26   │ SP Alignment fault                                   │
│ 0x2C   │ Trapped floating-point exception                     │
│ 0x30   │ Breakpoint from lower EL                            │
│ 0x31   │ Breakpoint from current EL                          │
│ 0x32   │ Software Step from lower EL                         │
│ 0x34   │ Watchpoint from lower EL                            │
│ 0x38   │ BKPT instruction (AArch32)                          │
│ 0x3C   │ BRK instruction (AArch64)                           │
└────────┴───────────────────────────────────────────────────────┘

ISS for Data Abort (EC=0x24/0x25) — most complex:
  [24]    ISV  — ISS Valid (1 = below fields meaningful)
  [23:22] SAS  — Access Size (00=byte, 01=half, 10=word, 11=dword)
  [21]    SSE  — Sign extend
  [20:16] SRT  — Source/destination register (Xt)
  [15]    SF   — 64-bit register (1=Xt, 0=Wt)
  [10]    FnV  — FAR not Valid
  [9]     EA   — External Abort
  [8]     CM   — Cache Maintenance fault
  [7]     S1PTW — Stage-2 fault during Stage-1 walk
  [6]     WnR  — Write not Read (1=write caused fault)
  [5:0]   DFSC — Data Fault Status Code:
                 0x04 = Translation fault, Level 0
                 0x05 = Translation fault, Level 1
                 0x06 = Translation fault, Level 2
                 0x07 = Translation fault, Level 3
                 0x09-0x0B = Access flag fault, L1-L3
                 0x0D-0x0F = Permission fault, L1-L3
                 0x21 = Alignment fault
                 0x10 = TLB conflict abort

Real Linux kernel usage (arch/arm64/kernel/entry.S + fault.c):
  1. Read ESR_EL1
  2. Extract EC = ESR >> 26
  3. Jump table based on EC:
     EC=0x15 → el0_svc() (system call handler)
     EC=0x24 → el0_da()  (data abort from user)
     EC=0x20 → el0_ia()  (instruction abort from user)
  4. For data abort: extract DFSC, WnR
     DFSC=0x07 (translation L3) + WnR=0 → page fault, need to
     allocate page and map it (demand paging)
```

---

## Q8. [L1] What is the difference between synchronous and asynchronous exceptions in ARMv8?

**Answer:**

```
Synchronous exceptions:
  • Caused BY the currently executing instruction
  • PC of the faulting instruction is KNOWN and EXACT
  • ELR_ELx = address of the instruction that caused it
  • Examples:
    - SVC / HVC / SMC (deliberate exception)
    - Page fault (data abort, instruction abort)
    - Undefined instruction
    - Alignment fault
    - Breakpoint / watchpoint
    - MSR/MRS trap
  • The handler can FIX the cause and retry the instruction
    (e.g., map a page, then ERET back to the faulting load)

Asynchronous exceptions:
  • NOT caused by the current instruction
  • Arrive at an IMPRECISE point (between any two instructions)
  • ELR_ELx = address of the NEXT instruction (not the cause)
  • Examples:
    - IRQ (hardware interrupt from peripheral)
    - FIQ (fast interrupt)
    - SError (asynchronous abort — e.g., uncorrectable ECC error
      from a buffered write that already completed)
  • Cannot retry — must handle the external event

Why SError is dangerous:
  SError is a DELAYED hardware error. Example:
  1. CPU executes: STR X0, [X1]    (store to memory)
  2. Store goes to write buffer (CPU continues executing)
  3. Write buffer sends to bus → memory controller → ECC error!
  4. By now, CPU has executed 50 more instructions
  5. SError arrives — but PC is 50 instructions AFTER the store
  6. Very hard to diagnose — which store caused it?

  Linux: SError often causes kernel panic (unrecoverable).
  RAS (Reliability, Availability, Serviceability) extensions
  in ARMv8.2 add DISR_EL1 to give more info about SError source.
```

---

## Q9. [L2] What is VBAR_ELx? How is the exception vector table structured and why is it aligned to 2KB?

**Answer:**

```
VBAR_ELx (Vector Base Address Register) holds the base address of
the exception vector table for each exception level.

  VBAR_EL1: exception vectors for EL1
  VBAR_EL2: exception vectors for EL2
  VBAR_EL3: exception vectors for EL3

Alignment: VBAR must be aligned to 2KB (0x800 = 2048 bytes)
  Because: the table has 16 entries × 0x80 bytes = 0x800 bytes

Why 0x80 (128 bytes = 32 instructions) per entry?
  ARM chose this to allow:
  • Small handlers to fit entirely in the vector entry
  • Larger handlers to branch out (B handler_function)
  • Each entry is exactly one cache line multiple (good for I-cache)

Table structure (4 groups × 4 exceptions = 16 entries):

Group 1: Exception from CURRENT EL, using SP_EL0 (unusual)
  +0x000: Synchronous
  +0x080: IRQ
  +0x100: FIQ
  +0x180: SError

Group 2: Exception from CURRENT EL, using SP_ELx (normal kernel)
  +0x200: Synchronous
  +0x280: IRQ
  +0x300: FIQ
  +0x380: SError

Group 3: Exception from LOWER EL, in AArch64
  +0x400: Synchronous (← syscalls from 64-bit apps land here)
  +0x480: IRQ         (← device interrupts from user mode)
  +0x500: FIQ
  +0x580: SError

Group 4: Exception from LOWER EL, in AArch32
  +0x600: Synchronous (← syscalls from 32-bit apps)
  +0x680: IRQ
  +0x700: FIQ
  +0x780: SError

Linux sets VBAR_EL1 during boot:
  LDR X0, =vectors         // Symbol from entry.S
  MSR VBAR_EL1, X0
  ISB                       // Ensure new vectors take effect

Interview trick question: "Which vector entry handles a Linux syscall?"
  Answer: +0x400 (Synchronous from lower EL, AArch64)
  Because: SVC is a synchronous exception from EL0 (lower) in AArch64.
  
  "What about a 32-bit app's syscall?"
  Answer: +0x600 (Synchronous from lower EL, AArch32)
```

---

## Q10. [L3] Explain the boot flow of an ARMv8 system from power-on to Linux user-space init. What runs at each EL?

**Answer:**

```
Power-On Reset → init process:

┌─────────────────────────────────────────────────────────────────┐
│ 1. RESET VECTOR (EL3, Secure)                                   │
│    • CPU fetches first instruction from ROM (fixed address)     │
│    • Could be 0x0 or vendor-defined (e.g., 0xFFFF0000)         │
│    • This is BL1 (Boot ROM) — immutable, burned at fab         │
│    • All cores start here; secondary cores enter holding pen   │
├─────────────────────────────────────────────────────────────────┤
│ 2. BL1 — Boot ROM at EL3                                        │
│    • Minimal init: set up stack, enable I-cache                 │
│    • Initialize the secure exception vectors                    │
│    • Load BL2 from flash/eMMC into secure SRAM                 │
│    • Authenticate BL2 (verify signature vs OTP root key)       │
│    • Jump to BL2                                                │
├─────────────────────────────────────────────────────────────────┤
│ 3. BL2 — Trusted Boot Firmware at S-EL1 (or EL3)               │
│    • Initialize DRAM controller (DDR training)                  │
│    • Load BL31, BL32, BL33 from storage into DRAM             │
│    • Authenticate each image (chain of trust)                  │
│    • Pass control to BL31 at EL3                               │
├─────────────────────────────────────────────────────────────────┤
│ 4. BL31 — EL3 Runtime Firmware (STAYS RESIDENT FOREVER)        │
│    • Initialize GIC (interrupt controller)                      │
│    • Install SMC dispatcher (PSCI, SCMI handlers)              │
│    • Set up SCR_EL3 (configure EL2/EL1 properties)             │
│    • Launch BL32 (OP-TEE) at S-EL1 — it initializes & returns │
│    • Set SCR_EL3.NS = 1 (switch to Normal World)              │
│    • Set HCR_EL2.RW = 1 (EL1 is AArch64)                     │
│    • ERET to BL33 at EL2 (or EL1 if no hypervisor)            │
├─────────────────────────────────────────────────────────────────┤
│ 5. BL33 — Normal World Bootloader at EL2/EL1                   │
│    • U-Boot / UEFI / GRUB                                      │
│    • Loads Linux kernel Image + DTB (device tree blob)         │
│    • May verify kernel signature (Verified Boot / AVB)         │
│    • Sets up kernel arguments in X0 (DTB address)              │
│    • Jumps to kernel entry point at EL2 (preferred) or EL1    │
├─────────────────────────────────────────────────────────────────┤
│ 6. Linux Kernel — starts at EL2 (with VHE) or EL1             │
│    • head.S: create initial page tables, enable MMU            │
│    • Switch to virtual addressing                              │
│    • start_kernel() → setup_arch()                             │
│    • Initialize subsystems: memory, scheduler, drivers         │
│    • Mount rootfs (initramfs or storage)                       │
│    • Kernel stays at EL2 (VHE) or drops to EL1                │
├─────────────────────────────────────────────────────────────────┤
│ 7. init process (EL0)                                           │
│    • Kernel execs /sbin/init (or systemd)                      │
│    • First user-space process at EL0                           │
│    • Starts services, brings up userland                       │
│    • System is fully booted                                    │
└─────────────────────────────────────────────────────────────────┘

Secondary CPUs:
  • Primary CPU does all the above
  • Secondary CPUs spin in a holding pen (WFI loop)
  • Linux calls PSCI_CPU_ON via SMC for each secondary
  • BL31 powers on the core, points it to Linux secondary entry
  • Secondary core initializes its own TLB/cache and joins scheduler
```

---

## Q11. [L2] What is the ARMv8 register file layout in AArch64? Explain SP, LR, FP, and the zero register.

**Answer:**

```
AArch64 General-Purpose Registers:

  X0-X30:  31 general-purpose 64-bit registers
  XZR:     Zero register (reads as 0, writes are discarded)
  SP:      Stack Pointer (separate from GPRs)
  PC:      Program Counter (not directly accessible as GPR)

  W0-W30:  Lower 32 bits of X0-X30
  WZR:     32-bit zero register

Special-purpose by convention (AAPCS64):
┌─────────┬──────────────────────────────────────────────────────┐
│Register │ Purpose                                               │
├─────────┼──────────────────────────────────────────────────────┤
│ X0-X7   │ Arguments & return values                            │
│         │ X0 = 1st arg / return value                          │
│         │ X0-X1 = 128-bit return value                        │
│ X8      │ Indirect result / syscall number                    │
│ X9-X15  │ Temporary / caller-saved (scratch)                  │
│ X16-X17 │ Intra-Procedure-call (IP0/IP1) — linker veneers    │
│ X18     │ Platform register (TLS on some OS, shadow stack)    │
│ X19-X28 │ Callee-saved (must preserve across function calls)  │
│ X29     │ Frame Pointer (FP) — points to stack frame          │
│ X30     │ Link Register (LR) — return address for BL          │
│ SP      │ Stack Pointer — always 16-byte aligned              │
│ XZR/WZR │ Zero register — useful for:                         │
│         │   MOV X0, XZR  → clear register                    │
│         │   CMP X1, XZR  → compare with zero                 │
│         │   STR XZR, [X0] → store zero to memory             │
└─────────┴──────────────────────────────────────────────────────┘

Stack Pointers (there are MULTIPLE!):
  SP_EL0: Used by EL0 (user space) AND optionally by EL1
  SP_EL1: Dedicated to EL1 exception handling
  SP_EL2: Dedicated to EL2
  SP_EL3: Dedicated to EL3
  
  PSTATE.SP selects which SP is used:
    SPSel=0 → use SP_EL0 (even at higher ELs)
    SPSel=1 → use SP_ELx (dedicated stack for current EL)
  
  Linux kernel: uses SP_EL0 for the per-task kernel stack
  (the IRQ vector switches to SP_EL1 for the interrupt stack)

Why XZR instead of R31?
  ARM reused the R31 encoding for BOTH SP and XZR:
  • In instructions where a register operand can be SP → it's SP
  • In instructions where SP doesn't make sense → it's XZR
  • The assembler/disassembler knows which based on the opcode
  Example: ADD X0, SP, #16  (SP is R31)
           ADD X0, XZR, X1  (XZR is R31, same encoding bit pattern!)
```

---

## Q12. [L3] What are System Registers in ARMv8? How are they organized and accessed?

**Answer:**

```
AArch64 has ~1000+ system registers (vs ~50 CP15 registers in ARMv7).
They control every aspect of the CPU: MMU, caches, exceptions, debug,
performance monitoring, virtualization, security, etc.

Naming convention: NAME_ELn
  SCTLR_EL1  = System Control Register for EL1
  TTBR0_EL2  = Translation Table Base Register 0 for EL2
  VBAR_EL3   = Vector Base Address Register for EL3

Access: via MRS (read) and MSR (write):
  MRS X0, SCTLR_EL1       // Read SCTLR_EL1 into X0
  MSR SCTLR_EL1, X0       // Write X0 into SCTLR_EL1
  MSR DAIFSet, #0xF        // Special form: set DAIF mask bits

Encoding in instruction:
  MSR/MRS encode: Op0, Op1, CRn, CRm, Op2
  Example: SCTLR_EL1 = (3, 0, 1, 0, 0)
  This replaces ARMv7's MCR/MRC p15, op1, Rd, CRn, CRm, op2

Access rules (privilege):
  _EL0 registers: accessible from any EL
  _EL1 registers: accessible from EL1, EL2 (if not trapped), EL3
  _EL2 registers: accessible only from EL2 and EL3
  _EL3 registers: accessible only from EL3
  
  Exception: EL2 can trap EL1 accesses to _EL1 registers
  via HCR_EL2 trap bits (TVM, TID3, etc.)

Key system register categories:
┌─────────────────────────────────────────────────────────────┐
│ Category              │ Key Registers                       │
├───────────────────────┼─────────────────────────────────────┤
│ System control        │ SCTLR_ELx, ACTLR_EL1               │
│ Exception handling    │ VBAR_ELx, ESR_ELx, FAR_ELx, ELR_ELx│
│ MMU / Translation     │ TTBR0/1_ELx, TCR_ELx, MAIR_ELx   │
│ Cache                 │ CTR_EL0, CLIDR_EL1, CCSIDR_EL1    │
│ TLB                   │ (TLBI instructions, not registers)  │
│ Security              │ SCR_EL3, CPTR_ELx                  │
│ Virtualization        │ HCR_EL2, VTTBR_EL2, VTCR_EL2     │
│ Debug                 │ MDSCR_EL1, DBGBVR<n>, DBGWVR<n>  │
│ Performance           │ PMCR_EL0, PMEVCNTR<n>_EL0         │
│ Timer                 │ CNTPCT_EL0, CNTP_CTL_EL0           │
│ Thread/Process ID     │ TPIDR_EL0, TPIDR_EL1, CONTEXTIDR  │
│ CPU identification    │ MIDR_EL1, MPIDR_EL1, ID_AA64*     │
│ Pointer Auth          │ APIA/BKeyHi/Lo_EL1                 │
│ MTE                   │ GCR_EL1, RGSR_EL1, TFSR_EL1      │
└───────────────────────┴─────────────────────────────────────┘

Interview question: "After writing to a system register, when
does the new value take effect?"
Answer: It depends.
  • Most registers: take effect after an ISB (instruction barrier)
  • TLBI: must follow with DSB + ISB
  • TCR/TTBR changes: must TLBI + DSB + ISB to ensure no stale use
  • Some are immediate: DAIF masks, NZCV
  ISB flushes the pipeline, ensuring subsequent instructions see
  the new system register value.
```

---

## Q13. [L2] What happens when a CPU core receives an IRQ in ARMv8? Walk through the entire flow from hardware signal to C handler and back.

**Answer:**

```
Complete IRQ flow (EL0 → EL1 → handler → EL0):

  1. Device asserts interrupt line (e.g., UART has data)
     │
  2. GIC Distributor receives SPI, checks:
     • Is this SPI enabled? (GICD_ISENABLER)
     • What priority? (GICD_IPRIORITYR)
     • Which CPU to route to? (GICD_IROUTER)
     │
  3. GIC Redistributor for target core:
     • Checks group (Group 0=FIQ, Group 1=IRQ)
     • Checks running priority vs pending priority
     │
  4. GIC CPU Interface signals PE:
     • Asserts IRQ line to CPU core
     │
  5. CPU checks PSTATE.I:
     • If I=1 (masked): IRQ stays pending, no exception
     • If I=0 (unmasked): take exception
     │
  6. HARDWARE exception entry:
     • SPSR_EL1 = PSTATE (save current state)
     • ELR_EL1 = PC (return address — next instruction)
     • PSTATE.I = 1 (mask further IRQs)
     • PSTATE.EL = EL1
     • PC = VBAR_EL1 + 0x480 (IRQ from lower EL, AArch64)
     │
  7. VECTOR ENTRY (assembly):
     kernel_ventry el0_irq
       STP X0, X1, [SP, #-frame_size]!    // Save all regs
       STP X2, X3, [SP, #16]
       ... save X4-X30, LR onto exception stack frame ...
       MOV X0, SP                          // arg0 = pt_regs
       BL  el0_irq_handler                 // → C code
     │
  8. C IRQ HANDLER:
     el0_irq_handler(struct pt_regs *regs) {
         irq_enter();
         // Read ICC_IAR1_EL1 (Interrupt Acknowledge Register)
         // Returns the INTID (which interrupt)
         u32 intid = read_sysreg(ICC_IAR1_EL1);
         
         // Look up handler in irq_desc[] table
         generic_handle_irq(intid);
           → UART driver ISR runs:
              read data from UART FIFO
              wake up process waiting on read()
         
         // Signal EOI (End Of Interrupt)
         write_sysreg(intid, ICC_EOIR1_EL1);
         // Deactivate
         write_sysreg(intid, ICC_DIR_EL1);  // or combined EOI+deactivate
         
         irq_exit();
         // Check if rescheduling needed
     }
     │
  9. RETURN FROM EXCEPTION:
     kernel_exit el0
       LDP X0, X1, [SP]                   // Restore regs
       ... restore all registers ...
       ERET                                // → back to EL0
     │
  10. User-space resumes at ELR_EL1 address
      (doesn't know interrupt happened)

Total cycles: ~100-500 depending on handler complexity
```

---

## Q14. [L1] What is MPIDR_EL1? Why is it important in multi-core systems?

**Answer:**

```
MPIDR_EL1 = Multiprocessor Affinity Register

It uniquely identifies each core in a multi-core, multi-cluster,
multi-chip system using a hierarchical affinity scheme:

  MPIDR_EL1 format:
  ┌────────────────────────────────────────────────────────────┐
  │ Bits [39:32]: Aff3 — highest level (chip/socket/die)     │
  │ Bits [23:16]: Aff2 — cluster group / package             │
  │ Bits [15:8]:  Aff1 — cluster                             │
  │ Bits [7:0]:   Aff0 — core within cluster                 │
  │ Bit  [30]:    U    — Uniprocessor (1=only one core)      │
  │ Bit  [24]:    MT   — Multi-Threading (1=SMT core)        │
  └────────────────────────────────────────────────────────────┘

Example: 2-socket server, 4 clusters of 4 cores each:
  Socket 0, Cluster 0, Core 0: MPIDR = 0.0.0.0
  Socket 0, Cluster 0, Core 3: MPIDR = 0.0.0.3
  Socket 0, Cluster 1, Core 0: MPIDR = 0.0.1.0
  Socket 1, Cluster 0, Core 0: MPIDR = 0.1.0.0 (Aff2 changes)

  Mobile big.LITTLE (Cortex-A55 + A78):
  LITTLE Cluster, Core 0: MPIDR = 0.0.0.0
  LITTLE Cluster, Core 3: MPIDR = 0.0.0.3
  big Cluster, Core 0:    MPIDR = 0.0.1.0
  big Cluster, Core 1:    MPIDR = 0.0.1.1

Why important:
  1. GICv3 uses MPIDR for interrupt routing (GICD_IROUTER)
  2. PSCI CPU_ON takes MPIDR to identify which core to power on
  3. Linux uses it to build CPU topology (cluster/package awareness)
  4. Scheduler uses topology for task placement (prefer same cluster)
  5. Cache coherency: cores in same cluster share L2/L3 vs cross-cluster

Read my own MPIDR:
  MRS X0, MPIDR_EL1
  AND X0, X0, #0xFF       // Aff0 = my core number
```

---

## Q15. [L3] What are the key differences between ARMv8.0 and later extensions (8.1 through 9.0)? Name the most impactful features.

**Answer:**

```
┌───────────┬─────────────────────────────────────────────────────────┐
│ Version   │ Key Features & Impact                                   │
├───────────┼─────────────────────────────────────────────────────────┤
│ ARMv8.0   │ Baseline: AArch64, A64 ISA, 4 ELs, NEON, Crypto       │
│ (2013)    │ First 64-bit ARM                                        │
├───────────┼─────────────────────────────────────────────────────────┤
│ ARMv8.1   │ • LSE (Large System Extensions): hardware atomics      │
│ (2014)    │   LDADD, SWAP, CAS → 10x faster locks on many-core    │
│           │ • VHE: host kernel at EL2 (faster KVM)                 │
│           │ • PAN: kernel can't access user memory accidentally    │
│           │ • VMID16: 16-bit VMID (64K VMs)                       │
│           │ • Limited ordering regions (LOR)                       │
├───────────┼─────────────────────────────────────────────────────────┤
│ ARMv8.2   │ • SVE: Scalable Vector Extension (128-2048 bit)       │
│ (2016)    │ • FP16: half-precision SIMD (ML inference)             │
│           │ • Statistical Profiling (SPE)                          │
│           │ • RAS (Reliability): better error reporting            │
│           │ • IESB: implicit error synchronization barrier         │
│           │ • DotProd: INT8 dot product (ML acceleration)          │
│           │ • Secure EL2 (S-EL2) for secure partition manager     │
├───────────┼─────────────────────────────────────────────────────────┤
│ ARMv8.3   │ • Pointer Authentication (PAC): anti-ROP/JOP          │
│ (2016)    │ • Nested Virtualization (NV)                           │
│           │ • RCPC (Release Consistency PC): weaker loads for perf │
│           │ • Complex number SIMD (FCMLA, FCADD)                  │
├───────────┼─────────────────────────────────────────────────────────┤
│ ARMv8.4   │ • S2FWB: Stage-2 Forced Write-Back (faster virt.)    │
│ (2017)    │ • MPAM: Memory Partitioning and Monitoring (QoS)      │
│           │ • Activity Monitors (AMU): hardware counters           │
│           │ • Secure EL2 improvements                              │
│           │ • Flag manipulation (SETF8/16, CFINV, RMIF)           │
├───────────┼─────────────────────────────────────────────────────────┤
│ ARMv8.5   │ • MTE: Memory Tagging Extension (catches UAF/overflow)│
│ (2018)    │ • BTI: Branch Target Identification (JOP protection)  │
│           │ • RNDR/RNDRRS: hardware random numbers                │
│           │ • Speculative Store Bypass Safe (SSBS)                 │
│           │ • E0PD: privileged access never from EL0              │
├───────────┼─────────────────────────────────────────────────────────┤
│ ARMv8.6   │ • BFloat16 (BF16): ML training format                 │
│ (2019)    │ • I8MM: INT8 matrix multiply                          │
│           │ • Fine-grained traps (FEAT_FGT)                       │
│           │ • Enhanced Counter Virtualization (ECV)                │
│           │ • WFxT: WFI/WFE with timeout                          │
├───────────┼─────────────────────────────────────────────────────────┤
│ ARMv8.7   │ • Alternate FP: deterministic FP behavior             │
│ (2020)    │ • Atomic 64-byte loads/stores (LD64B/ST64B)           │
│           │ • WFI/WFE trap control enhancements                   │
│           │ • PMU snapshot (freeze counters)                       │
├───────────┼─────────────────────────────────────────────────────────┤
│ ARMv8.8   │ • HBC: Hinted conditional branches (compiler opt)     │
│ (2021)    │ • MOPS: Memory operations (memcpy/set instructions)   │
│           │ • NMI support (non-maskable interrupts at GIC)        │
│           │ • Standardized debug controls                         │
├───────────┼─────────────────────────────────────────────────────────┤
│ ARMv9.0   │ • MANDATORY SVE2 (no more NEON-only cores)            │
│ (2021)    │ • RME: Realm Management Extension (CCA)               │
│           │   4 security states: Root/Secure/Normal/Realm         │
│           │ • SME: Scalable Matrix Extension (outer product)      │
│           │ • TRBE: Trace Buffer Extension (self-hosted trace)    │
│           │ • ETE: Embedded Trace Extension (replaces ETMv4)      │
└───────────┴─────────────────────────────────────────────────────────┘

Most impactful for interviews:
  1. LSE atomics (8.1) — "How does LDADD help spinlocks?"
  2. SVE/SVE2 (8.2/9.0) — "Explain VLA programming model"
  3. PAC (8.3) — "How does pointer authentication prevent ROP?"
  4. MTE (8.5) — "How does MTE detect use-after-free?"
  5. VHE (8.1) — "Why run host kernel at EL2?"
```

---

## Q16. [L1] What does SCTLR_EL1 control? Name the most critical bits.

**Answer:**

```
SCTLR_EL1 = System Control Register for EL1
Controls fundamental CPU behavior: MMU, caches, alignment, endianness.

Critical bits:
┌─────┬──────┬────────────────────────────────────────────────────┐
│ Bit │ Name │ Function                                           │
├─────┼──────┼────────────────────────────────────────────────────┤
│  0  │ M    │ MMU Enable: 0=off (flat physical), 1=on           │
│  1  │ A    │ Alignment check: 1=unaligned access faults        │
│  2  │ C    │ Data cache enable: 1=on                           │
│  3  │ SA   │ SP alignment check: EL1 stack must be 16B aligned │
│  4  │ SA0  │ SP alignment check for EL0                        │
│ 12  │ I    │ Instruction cache enable: 1=on                    │
│ 19  │ WXN  │ Write-implies-eXecute-Never: writable=non-exec    │
│ 25  │ EE   │ Exception Endianness: 0=LE, 1=BE                 │
│ 26  │ UCI  │ EL0 access to cache maintenance (DC CVAU, IC IVAU)│
│ 44  │ DSSBS│ Data speculative store default (Spectre mitigation)│
│ 57  │ EPAN │ Enhanced PAN (ARMv8.7): instruction + data protect│
└─────┴──────┴────────────────────────────────────────────────────┘

Classic boot sequence:
  // At boot, MMU is off. Must enable carefully:
  // 1. Set up page tables first
  // 2. Set TTBR0_EL1, TTBR1_EL1, TCR_EL1, MAIR_EL1
  // 3. TLBI VMALLE1 (invalidate all TLB entries)
  // 4. DSB ISH
  // 5. Then enable MMU:
  MRS X0, SCTLR_EL1
  ORR X0, X0, #(1 << 0)    // M=1 (MMU on)
  ORR X0, X0, #(1 << 2)    // C=1 (D-cache on)
  ORR X0, X0, #(1 << 12)   // I=1 (I-cache on)
  MSR SCTLR_EL1, X0
  ISB                        // Pipeline flush — vital!
  // From this point, all addresses are VIRTUAL

  WARNING: The code enabling the MMU must be identity-mapped
  (VA=PA) because the instruction after MSR SCTLR_EL1 is
  fetched using the NEW translation regime. If it's not mapped,
  the CPU will immediately take a translation fault!
```

---

## Q17. [L2] How does ARMv8 handle endianness? Can you run big-endian and little-endian simultaneously?

**Answer:**

```
ARMv8 supports both byte orderings but has nuanced rules:

  Data endianness: controlled per-EL
    SCTLR_EL1.EE:  EL1 data endianness (and exception entry)
    SCTLR_EL1.E0E: EL0 data endianness
    SCTLR_EL2.EE:  EL2 endianness
    SCTLR_EL3.EE:  EL3 endianness

  Instruction fetch: ALWAYS little-endian in AArch64
    (A64 instructions are always LE regardless of data endianness)

  So YES, you can have:
    EL1 data = Big-Endian, EL0 data = Little-Endian
    Instructions are still fetched LE at both levels

  Practical reality:
    • Almost everything runs Little-Endian
    • Networking code sometimes wants BE (network byte order)
    • REV, REV16, REV32 instructions for byte-swapping
    • Linux ARM64: BE support exists but rarely used
    • Some storage controllers run BE for legacy reasons
```

---

## Q18. [L3] What is the difference between ID_AA64MMFR0_EL1, ID_AA64ISAR0_EL1, and ID_AA64PFR0_EL1? How does Linux use them?

**Answer:**

```
These are CPU FEATURE IDENTIFICATION registers. Each reports
which optional features the CPU implements.

ID_AA64MMFR0_EL1: Memory Model Feature Register 0
  Reports: physical address size, ASID size, granule support,
           snoop control, big-endian support, TLB maintenance
  Example: PARange[3:0] = 0x5 → 48-bit PA (256 TB)
           TGran4[31:28] = 0x0 → 4KB granule supported
           TGran64[27:24] = 0x0 → 64KB granule supported

ID_AA64ISAR0_EL1: Instruction Set Attribute Register 0
  Reports: cryptography, atomics, CRC32, RDM support
  Example: AES[7:4] = 0x2 → AES + PMULL supported
           Atomic[23:20] = 0x2 → LSE atomics supported
           SHA2[15:12] = 0x2 → SHA256 + SHA512 supported
           DP[47:44] = 0x1 → Dot Product supported
           RNDR[63:60] = 0x1 → RNDR instruction supported

ID_AA64PFR0_EL1: Processor Feature Register 0
  Reports: exception levels, FP, AdvSIMD, GIC, SVE, RAS
  Example: EL0[3:0] = 0x2 → EL0 supports AArch64+AArch32
           EL1[7:4] = 0x2 → EL1 supports AArch64+AArch32
           EL2[11:8] = 0x1 → EL2 supports AArch64 only
           EL3[15:12] = 0x1 → EL3 supports AArch64 only
           FP[19:16] = 0x0 → FP supported (0xF = not impl)
           AdvSIMD[23:20] = 0x0 → NEON supported
           GIC[27:24] = 0x1 → GICv3/v4 system registers
           SVE[35:32] = 0x1 → SVE supported

Linux uses these in:
  arch/arm64/kernel/cpufeature.c
  
  At boot, reads ALL ID registers for each CPU core.
  Builds a "capability" bitmap.
  If feature present → enable alternative code patching:
    - LSE atomics: replace LDXR/STXR loops with LDADD/CAS
    - PAC: enable pointer authentication
    - MTE: enable memory tagging
    - PAN: enable Privileged Access Never
  
  Reports to userspace via:
    /proc/cpuinfo (Features: line)
    HWCAP/HWCAP2 (ELF auxiliary vector)
    
  Security: EL2 can trap ID register reads from EL1 (HCR_EL2.TID3)
  → hypervisor can hide features from guests or present fake IDs.
```

---

## Q19. [L2] Explain LSE (Large System Extensions) atomics. Why were they introduced and how do they improve performance?

**Answer:**

```
Before LSE (ARMv8.0):
  Atomic operations used Load-Exclusive / Store-Exclusive loops:

  compare_and_swap:
    LDXR  W0, [X1]           // Load-Exclusive
    CMP   W0, W2             // Compare with expected
    B.NE  fail               // Not equal → fail
    STXR  W3, W4, [X1]       // Store-Exclusive
    CBNZ  W3, compare_and_swap // Retry if exclusive failed
  
  Problem on many-core systems (8+ cores):
  • Exclusive monitor thrashing — multiple cores retry
  • O(N²) bus traffic in worst case
  • Forward progress not guaranteed under contention
  • A 64-core server could see spinlocks taking 100x longer

LSE (ARMv8.1) adds SINGLE-INSTRUCTION atomics:
  CAS  W0, W1, [X2]         // Compare-and-Swap
  LDADD W0, W1, [X2]        // Atomic load-add
  LDSET W0, W1, [X2]        // Atomic load-OR (set bits)
  LDCLR W0, W1, [X2]        // Atomic load-AND-NOT (clear bits)
  SWP  W0, W1, [X2]         // Atomic swap
  
  Ordering variants:
  CASA  (acquire), CASL (release), CASAL (acquire+release)
  LDADDA, LDADDL, LDADDAL — same for all atomic ops

Performance improvement:
  ┌──────────────────────────────────────────────────────────────┐
  │ Cores │ LDXR/STXR latency │ LSE CAS latency │ Speedup     │
  ├───────┼────────────────────┼─────────────────┼─────────────┤
  │ 2     │ ~50 ns            │ ~30 ns          │ 1.7x        │
  │ 8     │ ~200 ns           │ ~40 ns          │ 5x          │
  │ 32    │ ~2000 ns          │ ~60 ns          │ 33x         │
  │ 64    │ ~8000 ns          │ ~80 ns          │ 100x        │
  └───────┴────────────────────┴─────────────────┴─────────────┘

  Why? LSE atomics are handled by the cache/interconnect as a
  SINGLE coherency transaction. No retry loop, no exclusive
  monitor contention. The interconnect arbitrates fairly.

Linux: patches LDXR/STXR → LSE at boot if CPU supports them
  (static_branch / alternatives framework)
```

---

## Q20. [L3] What is FEAT_NMI (ARMv8.8)? How does it change interrupt handling?

**Answer:**

```
Before FEAT_NMI:
  • IRQs can be masked by PSTATE.I = 1
  • When kernel holds a spinlock: interrupts are disabled
  • Problem: a critical interrupt (watchdog, perf NMI) can't
    break through — kernel hangs go undetected
  • Workaround: use FIQ as pseudo-NMI (Linux does this!)
    GIC Group 0 → FIQ → higher priority than IRQ

FEAT_NMI (ARMv8.8) adds real NMI support:
  • PSTATE.ALLINT: single bit to mask everything (including NMI)
  • Superpriority interrupts: GIC can mark IRQs as "superpriority"
  • Superpriority IRQs IGNORE PSTATE.I masking
  • They can only be masked by PSTATE.ALLINT=1

  How it works:
  1. GIC: mark watchdog IRQ as superpriority
     (GICD_IPRIORITYR with superpriority bit)
  2. Kernel: normal IRQs masked during spinlock (PSTATE.I=1)
  3. Watchdog fires: GIC sends superpriority IRQ
  4. CPU: PSTATE.I=1 but superpriority ignores it → NMI taken!
  5. NMI handler: captures registers, stack trace for debugging
  6. If kernel is hung: NMI handler triggers panic/dump

  Only PSTATE.ALLINT=1 stops NMI. Kernel sets ALLINT=1 only in
  the NMI handler itself (prevent NMI nesting).

  Linux: replaces the FIQ-as-NMI hack with proper NMI support
  when FEAT_NMI is available.
```

---

Back to [Question & Answers Index](./README.md)
