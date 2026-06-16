# Exception Levels: EL0, EL1, EL2, EL3 — Roles and Transitions

**Category**: ARM64 Architecture Fundamentals  
**Targeted**: ARM, Qualcomm (baseline for all kernel/hypervisor interviews)

---

## 1. Concept Foundation — Why Exception Levels Exist

ARM64 uses a **privilege hierarchy** called Exception Levels (EL0–EL3) to enforce isolation between software components running on the same physical hardware. Each level grants progressively more hardware access. The model directly maps to real software stacks:

```
EL3  ─── Secure Monitor (ARM Trusted Firmware / TF-A)
EL2  ─── Hypervisor (KVM, Xen, Type-1 VMMs)
EL1  ─── OS Kernel (Linux kernel, guest OS kernel)
EL0  ─── User Applications (processes, libc, apps)
```

**Why not just one privilege level?**  
A flat model means a bug in any software component compromises the entire system. ELs enforce that:
- A user application (EL0) **cannot** directly access kernel data structures or hardware registers.
- A guest OS kernel (EL1) **cannot** directly see or modify hypervisor page tables (EL2).
- Firmware (EL3) can enforce security properties that no software above it can bypass.

---

## 2. Each Exception Level in Detail

### EL0 — Unprivileged (User Space)
- **Who runs here**: User applications, libc, shared libraries, JIT-compiled code.
- **Privileges**: Minimal. Cannot access system registers directly. Cannot configure the MMU. Cannot execute `MSR`/`MRS` to EL1+ registers (causes `EL0t` to EL1 trap).
- **Stack pointer**: Uses `SP_EL0` exclusively.
- **Key constraint**: Any attempt to access a privileged register or execute a privileged instruction triggers a **Synchronous Exception** taken to EL1.
- **Linux context**: Every userspace process runs at EL0. `fork()`, `exec()`, `mmap()` syscalls all transition EL0 → EL1 via the `SVC #0` instruction.

### EL1 — Privileged (OS Kernel)
- **Who runs here**: Linux kernel, guest OS kernels (under a hypervisor).
- **Privileges**: Full access to EL1 system registers: `SCTLR_EL1`, `TCR_EL1`, `TTBR0/1_EL1`, `MAIR_EL1`, `VBAR_EL1`, etc. Can configure MMU, caches, interrupt handling.
- **Stack pointers**: `SP_EL0` (used for user stack) and `SP_EL1` (used for kernel stack). Selected by `SPSel`.
- **Exception vector**: `VBAR_EL1` — points to the kernel's exception vector table (`vectors` in `arch/arm64/kernel/entry.S`).
- **Linux context**: The kernel runs at EL1. All kernel threads, interrupt handlers, system call handlers run at EL1.

### EL2 — Hypervisor
- **Who runs here**: Hypervisors (KVM on ARM64 via VHE, Xen, Type-1 VMMs). Absent if `HCR_EL2.VM=0` and no virtualization.
- **Privileges**: Controls whether EL1 (guest OS) can access certain resources. Sets up Stage 2 page tables (`VTTBR_EL2`). Can trap EL1 instructions (`HCR_EL2` trap bits).
- **Key registers**: `HCR_EL2`, `VTTBR_EL2`, `VTCR_EL2`, `VMPIDR_EL2`, `VBAR_EL2`.
- **VHE (ARMv8.1)**: With `HCR_EL2.E2H=1`, the Linux host kernel can run directly at EL2, eliminating EL1/EL2 transition overhead.
- **Linux/KVM context**: When `CONFIG_KVM` is enabled, KVM initializes EL2 at boot. The host Linux kernel itself runs at EL1 (non-VHE) or EL2 (VHE).

### EL3 — Secure Monitor
- **Who runs here**: ARM Trusted Firmware (TF-A / ATF). The most privileged level. Always exists.
- **Privileges**: Controls TrustZone. Sets `SCR_EL3.NS` to switch between Secure and Non-Secure worlds. Manages EL3-banked registers. Cannot be trapped or overridden by any lower EL.
- **Key registers**: `SCR_EL3`, `SCTLR_EL3`, `VBAR_EL3`, `ELR_EL3`, `SPSR_EL3`.
- **Linux context**: Linux never runs at EL3. TF-A initializes the platform, then drops to EL2 or EL1 to boot Linux.

---

## 3. Exception Level Transitions

### 3.1 Transition Rules

```
HIGHER EL → LOWER EL:  Always via ERET instruction
LOWER EL → HIGHER EL:  Via exception (SVC, HVC, SMC, IRQ, FIQ, SError, abort)
```

**You can NEVER jump to a higher EL directly** — only exceptions can elevate privilege. This is the fundamental security property of the exception model.

### 3.2 Exception Return (ERET)

`ERET` restores state from two registers:
- **`ELR_ELx`** — Exception Link Register: the address to return to (PC to restore).
- **`SPSR_ELx`** — Saved Program Status Register: the PSTATE to restore (EL, SP, flags, interrupt masks).

```
# Sequence when Linux kernel returns to user space (EL1 → EL0):
ERET instruction:
  PC    ← ELR_EL1       (saved user-space return address)
  PSTATE ← SPSR_EL1     (restores EL0t mode, enables interrupts, restores flags)
  SP    ← SP_EL0         (user stack pointer takes effect)
```

### 3.3 Exceptions that Cause EL Transitions

| Exception Type | Instruction | Destination EL |
|---|---|---|
| System Call | `SVC #imm` | EL0 → EL1 |
| Hypervisor Call | `HVC #imm` | EL1 → EL2 (if enabled) |
| Secure Monitor Call | `SMC #imm` | EL1/EL2 → EL3 |
| IRQ | Hardware line | EL0/EL1 → EL1 (or EL2 if routed) |
| FIQ | Hardware line | EL0/EL1/EL2 → EL3 (or EL1/EL2 if routed) |
| Synchronous Abort | Data/Instruction fault | Current or higher EL |
| SError | Asynchronous bus error | Current or higher EL |

### 3.4 Exception Routing Registers

**`SCR_EL3` controls EL3 routing**:
- `SCR_EL3.IRQ` = 1 → IRQs taken to EL3 (not EL1).
- `SCR_EL3.FIQ` = 1 → FIQs taken to EL3.

**`HCR_EL2` controls EL2 routing**:
- `HCR_EL2.IMO` = 1 → Virtual IRQ routing via EL2.
- `HCR_EL2.FMO` = 1 → Virtual FIQ routing via EL2.
- `HCR_EL2.AMO` = 1 → SError routing via EL2.

### 3.5 Exception Level Transition Diagram

```
┌─────────────────────────────────────────────────────────────────┐
│                           EL3 (TF-A)                            │
│   SCR_EL3, SCTLR_EL3, VBAR_EL3, ELR_EL3, SPSR_EL3               │
└────────────────────▲──────────────────────┬─────────────────────┘
                   SMC                    ERET
┌────────────────────┴──────────────────────▼─────────────────────┐
│                        EL2 (KVM/Hypervisor)                     │
│   HCR_EL2, VTTBR_EL2, VTCR_EL2, VBAR_EL2, ELR_EL2, SPSR_EL2     │
└────────────────────▲──────────────────────┬─────────────────────┘
                   HVC                    ERET
┌────────────────────┴──────────────────────▼─────────────────────┐
│                     EL1 (Linux Kernel)                          │
│  SCTLR_EL1, TCR_EL1, TTBR0/1_EL1, VBAR_EL1, ELR_EL1, SPSR_EL1   │
└────────────────────▲──────────────────────┬─────────────────────┘
                   SVC                    ERET
┌────────────────────┴──────────────────────▼─────────────────────┐
│                       EL0 (User Space)                          │
│                   SP_EL0, TPIDR_EL0, TPIDRRO_EL0                │
└─────────────────────────────────────────────────────────────────┘
```

---

## 4. ARM64 Exception Vector Table Structure

Each EL has its own vector base register (`VBAR_ELx`). The table has 16 entries, 4 per exception class × 4 classes:

```
VBAR_EL1 + 0x000  — Synchronous (current EL, SP_EL0)
VBAR_EL1 + 0x080  — IRQ/vIRQ  (current EL, SP_EL0)
VBAR_EL1 + 0x100  — FIQ/vFIQ  (current EL, SP_EL0)
VBAR_EL1 + 0x180  — SError    (current EL, SP_EL0)
VBAR_EL1 + 0x200  — Synchronous (current EL, SP_ELx)
VBAR_EL1 + 0x280  — IRQ/vIRQ  (current EL, SP_ELx)
VBAR_EL1 + 0x300  — FIQ/vFIQ  (current EL, SP_ELx)
VBAR_EL1 + 0x380  — SError    (current EL, SP_ELx)
VBAR_EL1 + 0x400  — Synchronous (lower EL, AArch64)
VBAR_EL1 + 0x480  — IRQ/vIRQ  (lower EL, AArch64)
VBAR_EL1 + 0x500  — FIQ/vFIQ  (lower EL, AArch64)
VBAR_EL1 + 0x580  — SError    (lower EL, AArch64)
VBAR_EL1 + 0x600  — Synchronous (lower EL, AArch32)
VBAR_EL1 + 0x680  — IRQ/vIRQ  (lower EL, AArch32)
VBAR_EL1 + 0x700  — FIQ/vFIQ  (lower EL, AArch32)
VBAR_EL1 + 0x780  — SError    (lower EL, AArch32)
```

In Linux, `VBAR_EL1` is set to `vectors` in `arch/arm64/kernel/entry.S`. The exception class is determined by the ESR_EL1.EC field.

---

## 5. Linux Kernel Implementation

### 5.1 EL Detection at Boot

```c
// arch/arm64/kernel/head.S
// At boot, Linux checks what EL it was booted at:
mrs     x0, CurrentEL          // Read current EL
lsr     x0, x0, #2             // CurrentEL[3:2] = EL value
cmp     x0, #2                 // Are we at EL2?
b.eq    init_el2               // Yes → initialize EL2, then drop to EL1
b       init_el1               // No  → initialize EL1 directly
```

### 5.2 System Call Entry (EL0 → EL1)

```c
// arch/arm64/kernel/entry.S — SVC handler
SYM_CODE_START(vectors)
    // offset 0x400: Lower EL AArch64 Synchronous
    kernel_entry 0, 64          // Save user regs, switch to kernel stack
    mov     x0, sp
    bl      el0_sync            // -> el0_sync() in entry-common.c
    b       ret_to_user

// arch/arm64/kernel/entry-common.c
asmlinkage void noinstr el0_sync(struct pt_regs *regs)
{
    el0_sync_handler(regs);     // Dispatches on ESR_EL1.EC
    // EC = 0x15 → SVC64 → invoke_syscall()
}
```

### 5.3 ERET back to EL0

```c
// arch/arm64/kernel/entry.S
ret_to_user:
    disable_daif                // Disable interrupts for final checks
    ldr     x19, [tsk, #TSK_TI_FLAGS]
    // Handle pending signals, reschedule if needed
    kernel_exit 0               // Restore user regs
    eret                        // PC ← ELR_EL1, PSTATE ← SPSR_EL1
```

### 5.4 KVM EL2 Initialization

```c
// arch/arm64/kvm/hyp/nvhe/hyp-init.S
// KVM installs its own EL2 vectors:
msr     vbar_el2, x0           // x0 = KVM EL2 vector table
isb
// Then configures HCR_EL2 for guest trapping
```

---

## 6. Key Registers Per Exception Level

| Register | EL | Purpose |
|---|---|---|
| `SP_EL0` | EL0/EL1 | User/thread stack pointer |
| `SP_EL1` | EL1 | Kernel stack pointer |
| `SP_EL2` | EL2 | Hypervisor stack pointer |
| `SP_EL3` | EL3 | Secure Monitor stack pointer |
| `ELR_EL1` | EL1 | Exception return address (saved PC) |
| `ELR_EL2` | EL2 | Exception return address |
| `ELR_EL3` | EL3 | Exception return address |
| `SPSR_EL1` | EL1 | Saved PSTATE on exception entry |
| `SPSR_EL2` | EL2 | Saved PSTATE on exception entry |
| `SPSR_EL3` | EL3 | Saved PSTATE on exception entry |
| `ESR_EL1` | EL1 | Exception Syndrome Register (EC, ISS) |
| `FAR_EL1` | EL1 | Fault Address Register |
| `VBAR_EL1` | EL1 | Vector Base Address Register |
| `CurrentEL` | Any | Read-only: current exception level |

---

## 7. SPSR_ELx Layout (Saved Program Status Register)

When an exception is taken to ELx, `SPSR_ELx` saves the PSTATE of the interrupted level:

```
Bit[63:32]  — Reserved
Bit[31]     — N  (Negative condition flag)
Bit[30]     — Z  (Zero condition flag)
Bit[29]     — C  (Carry condition flag)
Bit[28]     — V  (Overflow condition flag)
Bit[27:26]  — Reserved
Bit[25]     — TCO (Tag Check Override, ARMv8.5)
Bit[24]     — DIT (Data Independent Timing, ARMv8.4)
Bit[23]     — UAO (User Access Override, ARMv8.2)
Bit[22]     — PAN (Privileged Access Never, ARMv8.1)
Bit[21]     — SS  (Software Step)
Bit[20]     — IL  (Illegal Execution state)
Bit[19:10]  — Reserved
Bit[9]      — D   (Debug exception mask)
Bit[8]      — A   (SError mask)
Bit[7]      — I   (IRQ mask)
Bit[6]      — F   (FIQ mask)
Bit[5]      — Reserved (0 for AArch64)
Bit[4]      — M[4] — 0 = AArch64, 1 = AArch32
Bit[3:2]    — EL  (Exception Level of interrupted state)
Bit[1]      — Reserved (0)
Bit[0]      — SP  — Stack pointer selection (0=SP_EL0, 1=SP_ELx)
```

---

## 8. Interview Questions & Answers

***Q1: What happens when a user process executes an illegal instruction?***
**####################################################################################**

An Undefined Instruction exception is generated at EL0 and taken to EL1. The ARM hardware:
1. Saves PC to `ELR_EL1` (address of the faulting instruction).
2. Saves PSTATE to `SPSR_EL1`.
3. Sets `ESR_EL1.EC = 0x00` (Unknown reason) or `0x0E/0x1C` for trapped FP/SVE.
4. PC jumps to `VBAR_EL1 + 0x400` (synchronous exception from lower EL AArch64).
5. Linux dispatches to `el0_sync()`, determines EC = unknown, sends `SIGILL` to the process.

When an unprivileged user process at **EL0** attempts to execute an illegal or undefined instruction, the hardware takes a synchronous exception, forcing an immediate transition up to **EL1** so the operating system kernel can handle the failure.

Here is the exact hardware-to-software chain of events that occurs down to the clock cycle:

---

### 1. The Hardware Intercept (EL0)

During the instruction decode phase within the pipeline, the CPU's execution unit encounters an invalid opcode (e.g., a corrupted binary sequence, or a privileged instruction like `MSR SCTLR_EL1, X0` executed from user space).

* The instruction is instantly retired as an **Undefined Instruction** exception.
* The CPU pipeline is completely flushed of any speculatively fetched instructions behind it.

### 2. Automatic Hardware State Preservation

Before running a single line of kernel code, the hardware executes a hardwired routing routine to transition from EL0 to EL1:

1. **Saves Return Address:** The address of the offending illegal instruction is written directly into **`ELR_EL1`** (Exception Link Register).
2. **Saves Processor State:** The current user-space `PSTATE` (including ALU flags and interrupt masks) is copied into **`SPSR_EL1`** (Saved Processor State Register).
3. **Elevates Privilege:** `PSTATE.EL` is updated from `00` (EL0) to `01` (EL1).
4. **Selects Stack Pointer:** `PSTATE.SP` is automatically forced to `1`, meaning the active `SP` alias instantly switches from the user stack (`SP_EL0`) to the secure kernel stack (**`SP_EL1`**).
5. **Captures Fault Syndrome:** The hardware populates **`ESR_EL1`** (Exception Syndrome Register) with a specific bit configuration defining *why* the exception occurred. For an illegal instruction, the Exception Class bits (`ESR_EL1.EC`) are set to `0x25` (Unallocated Instruction).

---

### 3. Vector Table Jump (EL1)

The processor looks at **`VBAR_EL1`** (Vector Base Address Register), which holds the starting physical address of the kernel's exception vector table.

Because the exception originated from a *lower* exception level (EL0) and the target execution width is 64-bit AArch64, the hardware applies a specific memory offset to `VBAR_EL1`. It jumps directly to the entry point located at:

$$\text{Target Address} = \text{VBAR\_EL1} + \text{0x400}$$

```
Vector Table Layout Structure (VBAR_EL1):
+---------------------------------------------------------+
| ...                                                     |
+---------------------------------------------------------+
| VBAR_EL1 + 0x400 : Exceptions from Lower EL (AArch64)   |  <-- CPU Jumps Here
|   - Entry point reads ESR_EL1 to identify the fault     |
+---------------------------------------------------------+
| ...                                                     |
+---------------------------------------------------------+

```

---

### 4. Kernel Software Handling & Signal Dispatch

The code sitting at `VBAR_EL1 + 0x400` is a generic low-level assembly assembly routine inside the OS kernel (e.g., Linux's `el0_sync` routine):

1. **Context Save:** The kernel quickly uses `SP_EL1` to push user registers (`X0`–`X29` and `X30`) onto the kernel thread stack to preserve the application's exact state at the moment of death.
2. **Syndrome Decoding:** The kernel reads `ESR_EL1`. Seeing the `0x25` (Unallocated Instruction) class, it routes execution to its internal exception handler (e.g., `do_undefinstr`).
3. **Signal Construction:** The OS realizes this is an unrecoverable application error. It constructs a architecture-specific error signal—on Linux, this is **`SIGILL`** (Illegal Instruction).
4. **Process Termination:** The kernel schedules the delivery of `SIGILL` to the offending user process. Unless the application has configured a highly specialized custom signal handler, the default action is triggered: the process is killed instantly, and the OS dumps a core file (`Core dumped`).




***Q2: Can EL1 code execute an SMC instruction?***

Yes. An `SMC` from EL1 causes a Secure Monitor Call exception taken to EL3. However, `SCR_EL3.SMD` (Secure Monitor Call Disable) can be set to make SMC from EL1 generate an Undefined Instruction exception instead. Under a hypervisor, `HCR_EL2.TSC=1` traps `SMC` from EL1 to EL2.

**Yes, EL1 code can execute an `SMC` (Secure Monitor Call) instruction**, but whether it succeeds or causes an immediate processor panic depends entirely on how **EL2 (Hypervisor)** has configured its trap settings.

Here is the exact architectural breakdown of what happens when an `SMC` instruction is executed at EL1.

---

### The Baseline Architectural Flow

Under normal, non-virtualized conditions (or when permitted by the hypervisor), the `SMC` instruction is the designated software gateway used by an OS Kernel (EL1) to request services from the Secure Monitor (EL3), such as power management commands via PSCI (Power State Coordination Interface) or trusted security services.

```
                  [ SMC Trapped by HCR_EL2.TSC ]
                     /                      \
         If TSC == 1 /                       \ If TSC == 0
                    v                         v
       Trapped locally to EL2       Routes up to EL3
     (Hypervisor intercepts it)     (Secure Monitor executes it)

```

---

### The Gatekeeper: `HCR_EL2.TSC`

If a Hypervisor is active at EL2, it has complete veto power over whether the guest kernel is allowed to talk directly to the Secure Monitor. This is governed by the **`TSC` (Trap Secure Monitor Call)** bit (Bit 19) in the **`HCR_EL2`** register.

The hardware evaluates this bit instantly during the instruction decode phase at EL1:

#### Scenario A: `HCR_EL2.TSC == 1` (Trapped to Hypervisor)

The Hypervisor wants to abstract or hide the underlying physical hardware and security layers from the guest OS.

* **The Hardware Action:** The `SMC` instruction is intercepted and trapped **locally to EL2**. It never reaches EL3.
* **State Registers:**
* `ELR_EL2` captures the address of the `SMC` instruction.
* `ESR_EL2.EC` (Exception Class) is populated with `0x17` (`SMC` instruction execution in AArch64).


* **The Result:** The Hypervisor inspects the request, potentially emulates a response, and returns control back to the guest kernel without the Secure World ever knowing it fired.

#### Scenario B: `HCR_EL2.TSC == 0` (Routed to EL3)

The Hypervisor permits the guest OS to communicate directly with the firmware layout, or EL2 virtualization is absent/disabled.

* **The Hardware Action:** The processor takes an exception and elevates execution straight to **EL3**.
* **State Registers:**
* `ELR_EL3` captures the return address at EL1.
* `SPSR_EL3` preserves the EL1 `PSTATE`.
* `ESR_EL3.EC` is populated with `0x17`.


* **The Result:** The Secure Monitor processes the secure call natively.

---

### The Ultimate Fail-Safe: Secure World Disablement

There is a hardwired hardware condition where executing an `SMC` instruction at EL1 will fail instantly with an **Undefined Instruction exception** (trapped right back to EL1 or EL2):

> If the core configuration has permanently disabled the Secure World features at boot (for example, if the **`SIF`** or secure configuration fuses dictate that EL3 is non-existent or inaccessible), the `SMC` opcode becomes an unallocated instruction.

In this state, trying to execute an `SMC` at EL1 results in an immediate **Exception Class `0x25` (Unallocated Instruction)** fault, meaning the instruction is treated exactly like an illegal instruction sequence.

**No, EL2 is not always present on ARM64.**

While the ARMv8-A and ARMv9-A architectures define the *concept* of Exception Level 2 (EL2) for virtualization, its physical implementation on a piece of silicon is entirely optional for the hardware designer.

Here is the exact architectural reality of how EL2 is deployed, discovered, and emulated.

---

### 1. Hardware Implementation is Optional

When an chip designer (like Apple, Qualcomm, or an industrial MCU manufacturer) licenses an ARM64 core, they choose which Exception Levels to physically build based on the target market:

* **Server & Enterprise Chips (Cortex-X, Neoverse):** EL2 is **always** physically present because virtualization (running hypervisors like KVM, ESXi, or Xen) is a core requirement for cloud infrastructure.
* **Mobile SoCs:** EL2 is almost always present to support secure containerization, android micro-virtualization, and hypervisor-enforced kernel protections.
* **Embedded & Microcontrollers (Cortex-R82, low-power IoT):** EL2 is frequently **omitted entirely** from the silicon layout to save physical die space, reduce gate counts, and lower power consumption. If a chip doesn't need to run virtual machines, the silicon space for Stage 2 MMU translation tables and registers like `HCR_EL2` is completely stripped out.

---

### 2. Software Discovery: How a Kernel Checks

Because an OS kernel (EL1) cannot assume EL2 is there, it must query the hardware at boot. It does this by reading the system register **`ID_AA64PFR0_EL1`** (Processor Feature Register 0).

The kernel inspects bits `[11:8]` (`EL2` field):

```
ID_AA64PFR0_EL1 [11:8] Value:
+-------------------------------------------------------+
| 0b0000 : EL2 is NOT implemented (Silicon lacks EL2)   |
+-------------------------------------------------------+
| 0b0001 : EL2 IS implemented in AArch64 state          |
+-------------------------------------------------------+

```

If these bits read `0b0000`, any software attempt to read or write an EL2 register (like `MRS X0, HCR_EL2`) will instantly cause an **Undefined Instruction exception** trapped directly back to EL1.

---

### 3. The Security Split: Non-Secure vs. Secure EL2

To make things more interesting, the existence of EL2 used to be tied strictly to the **Non-Secure World**.

```
   Traditional ARMv8.0 Model               Modern ARMv8.4+ / ARMv9 Model
      (Non-Secure Only)                        (Secure EL2 Added)

      EL3: Secure Monitor                      EL3: Secure Monitor
     /                   \                    /                   \
Secure EL1            Non-Secure EL2     Secure EL2           Non-Secure EL2
(No Virtualization)   (Hypervisor)       (Secure Hyp)         (Normal Hyp)
                                              |                    |
                                         Secure EL1           Non-Secure EL1

```

* **Before ARMv8.4:** EL2 only existed in the Non-Secure world. The Secure World jumped straight from EL3 down to Secure EL1. There was no way to run a "Secure Hypervisor."
* **ARMv8.4-A onwards (and ARMv9):** ARM introduced **Secure EL2**. If an updated core implements EL2, it can now optionally support virtualization in *both* worlds, managed via the `SCR_EL3.EEL2` (Enhanced EL2) enable bit.

---

### 4. What happens if EL2 is missing but software wants it?

If a software stack requires virtualization but is deployed on a chip where EL2 is absent, **hardware-accelerated virtualization is completely impossible**.

The system cannot run a Type-1 hypervisor. To run a guest OS, the host platform must fall back to **pure software emulation** (binary translation via tools like QEMU), which incurs a massive performance penalty because every guest instruction and memory access must be intercepted and parsed in software by the EL1 OS.


***Q3: What is the difference between ELR_EL1 and LR (X30)?***

`ELR_EL1` is the **hardware-saved** exception return address — automatically saved by the CPU when an exception is taken to EL1. `LR` (X30) is a **software convention** — it holds the return address for function calls (set by `BL`/`BLR` instructions). They serve completely different purposes. When the kernel calls a C function, it uses LR. When it returns from an exception to user space, it uses `ERET` which restores PC from `ELR_EL1`.

While both registers store return addresses, they belong to completely different layers of the architecture. **`X30` (LR)** is a general-purpose register used for standard, software-controlled **function calls**, while **`ELR_EL1`** is a dedicated system register used exclusively by the hardware for **exception handling**.

Here is the exact architectural breakdown of the differences in ownership, behavior, and mechanics.

---

### 1. Functional Roles & Intended Use

#### `X30` (Link Register / LR)

* **The Concept:** It is a software-level register used to remember where to go back to after a standard function call finishes.
* **How it is updated:** When you execute a branch with link instruction (`BL` or `BLR`), the CPU automatically calculates the address of the *next* sequential instruction and writes it into `X30`.
* **How it returns:** To return from the function, software explicitly executes a sub-routine return instruction: `RET X30` (or simply `RET`).

#### `ELR_EL1` (Exception Link Register, EL1)

* **The Concept:** It is a hardware-driven system register used to remember where to return after a disruptive system exception (like a page fault, system call, or hardware interrupt) has been serviced by the kernel.
* **How it is updated:** Software never writes to this during normal execution. Instead, the moment an exception triggers and forces execution up to EL1, the **hardware automatically snapshots** the current Program Counter (`PC`) and forces that address into `ELR_EL1`.
* **How it returns:** To return from an exception, software must execute the specialized **`ERET`** instruction. `ERET` forces the hardware to read `ELR_EL1` and copy its contents directly back into the `PC`.

---

### 2. Side-by-Side Architectural Comparison

| Feature | `X30` (Link Register) | `ELR_EL1` (Exception Link Register) |
| --- | --- | --- |
| **Register Type** | General-Purpose Register (Part of core file) | Privileged System Register |
| **Accessibility** | Read/Writeable at any level (**EL0–EL3**) | Restricted to **EL1 or higher** (EL0 access traps) |
| **Modification** | Modified via standard branches (`BL`, `BLR`) | Modified automatically by **Hardware Exception Logic** |
| **Return Command** | `RET` (or `BR X30`) | `ERET` |
| **Target Return** | The instruction following a function call | The instruction that *caused* or was *interrupted* by a fault |

---

### 3. Contextual Isolation (How they coexist)

To see how they operate together without corrupting each other, consider this real-world execution sequence where a user application gets interrupted by a hardware timer:

```
  User Space (EL0)                                 Kernel Space (EL1)
====================                             ======================

  main() {
    0x4000: BL printf  ---->  printf() {
                                0x5020: ADD X0, X0, #1  <--- [ HARDWARE INTERRUPT FIRES HERE ]
                                                             - Hardware snapshots PC (0x5020) -> ELR_EL1
                                                             - Elevates privilege to EL1
                                                             - Jumps to VBAR_EL1
                              }                                      |
                                                                     v
                                                          VBAR_EL1 Vector Routine:
                                                             - Kernel handles timer interrupt
                                                             - Note: X30 still safely holds 0x4004!
                                                             - Executed: ERET
                                                             - Hardware copies ELR_EL1 (0x5020) -> PC
                                                             v
                                0x5020: ADD X0, X0, #1  <--- Resumes printf natively
                                0x5024: RET             ----> Jumps to X30 (0x4004)
                              }
    0x4004: ...
  }

```

1. An application at EL0 calls a function (`BL printf`). The hardware sets **`X30 = 0x4004`** (the return point inside `main`).
2. While executing `printf`, an external hardware timer interrupt fires.
3. The hardware instantly steps in. It takes the current execution pointer inside `printf` and saves it into **`ELR_EL1`**. It elevates the system to EL1 and jumps to the kernel's interrupt vector table.
4. **Crucial Detail:** Throughout this entire kernel interrupt routine, the user app's **`X30` register remains completely untouched**. The kernel doesn't lose track of where `printf` needs to return to later because the exception return track used `ELR_EL1` instead.
5. Once the kernel finishes handling the timer, it runs `ERET`. The hardware reads `ELR_EL1`, dropping the CPU back down to EL0 right where `printf` was interrupted.
6. `printf` continues execution normally and eventually runs its own `RET` instruction, cleanly reading **`X30`** to jump back to `main()`.

Great question—this gets right to the heart of how ARM64 handles **normal function calls vs exceptions**.

---

# 🧠 One-Line Difference

> **`LR (X30)`** is used for *function returns*, while **`ELR_EL1`** is used for *returning from exceptions*.

---

# 🔁 Side-by-Side Concept

| Feature            | `LR (X30)`               | `ELR_EL1`                       |
| ------------------ | ------------------------ | ------------------------------- |
| Used for           | Function calls (`BL`)    | Exceptions (interrupts, faults) |
| Set by             | CPU during `BL`          | CPU during exception entry      |
| Return instruction | `RET`                    | `ERET`                          |
| Scope              | General-purpose register | System register (EL1)           |
| Overwritten by     | Function calls           | Exceptions                      |

---

# 📞 1. Function Call Flow (Using LR / X30)

```text
Caller (EL0/EL1)       CPU
      │                 │
      │ BL function     │
      ├────────────────▶│
      │                 │ Save return addr → X30 (LR)
      │                 │
      │        Function executes
      │                 │
      │ RET             │
      ◀─────────────────┤ Jump to X30
```

### 🧠 Key Points

* `BL` (Branch with Link) sets **X30**
* `RET` jumps back using **X30**
* Purely software-level control flow

---

# ⚠️ 2. Exception Flow (Using ELR_EL1)

```text
User (EL0)         CPU / HW              Kernel (EL1)
     │                  │                     │
     │ Fault / IRQ      │                     │
     ├─────────────────▶│                     │
     │                  │ Save PC → ELR_EL1   │
     │                  │ Save state → SPSR   │
     │                  │ Switch to EL1       │
     │                  ├────────────────────▶│
     │                  │                     │ Handle exception
     │                  │                     │
     │                  │ ERET                │
     │◀─────────────────┤ Restore PC from ELR │
```

### 🧠 Key Points

* Hardware saves return address into **`ELR_EL1`**
* `ERET` returns using that value
* Used for:

  * Interrupts
  * Syscalls
  * Faults

---

# 🔍 Visual Comparison

```text
           NORMAL FLOW                    EXCEPTION FLOW
        ────────────────               ───────────────────

        BL func                         Exception occurs
           │                                 │
           ▼                                 ▼
     X30 = return addr               ELR_EL1 = return addr
           │                                 │
        RET                              ERET
           │                                 │
     Resume caller                   Resume interrupted code
```

---

# ⚙️ Register Nature

### 🔹 `LR (X30)`

* General-purpose register
* Part of:

  ```text
  X0–X30 register set
  ```
* Can be modified freely by software

---

### 🔹 `ELR_EL1`

* Special **system register**
* Only accessible via:

```asm
MRS Xt, ELR_EL1
MSR ELR_EL1, Xt
```

* Managed automatically by hardware on exceptions

---

# ⚠️ Critical Difference (Very Important)

### 🔥 Overwrite Behavior

* `X30`:

  * Overwritten on every `BL`
  * Must be saved manually in nested calls

* `ELR_EL1`:

  * Overwritten on every exception
  * Kernel must save it if nested exceptions occur

---

# 🧠 Real-World Analogy

* **X30 (LR)** → “Return address for a function call”
* **ELR_EL1** → “Return address for an *interrupting event*”

---

# 🧩 Bonus: Interaction Case

What if a function is interrupted?

```text
Function running (uses X30)
        │
        ▼
Interrupt occurs
        │
        ▼
ELR_EL1 stores return address (NOT X30!)
```

👉 Important:

* Exception does **not use X30**
* It uses **ELR_EL1 independently**

---

# 🎯 Final Takeaway

> `LR (X30)` handles **software control flow**, while `ELR_EL1` handles **hardware exception return flow**—they serve similar purposes but in completely different mechanisms.

---

If you want, I can go deeper into:

* How `SPSR_EL1` works with `ELR_EL1`
* Or show a **real Linux exception entry/exit assembly path**



***Q4: Is EL2 always present on ARM64?***

No. EL2 is optional in the hardware. If the CPU implements the Virtualization Extensions (mandatory in ARMv8.1+, optional in ARMv8.0), EL2 exists. Even when present, EL2 is only "active" when `HCR_EL2.VM=1` (Stage 2 enabled) or when a hypervisor has configured it. On a system without a hypervisor, Linux may boot directly at EL1, with EL2 effectively bypassed (TF-A drops to EL1 directly).

Short answer:

> **No — EL2 is *not* always present on ARM64 systems.**

---

# 🧠 Why EL2 Might Not Exist

In the ARM architecture, **Exception Levels are optional** (except EL0 and EL1 for a usable OS).

| Exception Level | Mandatory? | Purpose                     |
| --------------- | ---------- | --------------------------- |
| EL0             | ✅ Yes      | User applications           |
| EL1             | ✅ Yes      | OS kernel                   |
| EL2             | ❌ Optional | Hypervisor (virtualization) |
| EL3             | ❌ Optional | Secure monitor (TrustZone)  |

👉 So a CPU **can be fully ARM64-compliant without EL2**.

---

# 🧩 When EL2 *Is Present*

EL2 exists only if the CPU implements:

> **Virtualization Extension**

You’ll find EL2 in:

* Server CPUs (AWS Graviton, Ampere)
* High-end mobile SoCs
* Systems running:

  * KVM
  * Xen
  * Hypervisors

---

# ❌ When EL2 Is NOT Present

EL2 may be missing in:

* Low-cost embedded SoCs
* Microcontrollers running AArch64 (rare but possible)
* Systems that **don’t need virtualization**

In such CPUs:

* No hypervisor support
* No `HCR_EL2`, `VTTBR_EL2`, etc.

---

# 🔍 How Software Detects EL2

Software checks CPU feature registers:

Example:

```asm
MRS X0, ID_AA64PFR0_EL1
```

Look at:

* **Virtualization field**

If:

```text
Virtualization == 0 → EL2 not implemented
```

---

# 🔁 What Happens If EL2 Is Missing?

* OS runs at **EL1 (kernel)**
* No hypervisor layer
* No VM support
* Simpler exception model

---

# 🧠 Practical Insight

Even if EL2 exists:

👉 It might **not be used**

Example:

* Many phones have EL2 hardware
* But OS may run entirely in EL1

---

# 🎯 Key Takeaways

* EL2 is **optional in ARMv8-A**
* Requires **virtualization extension**
* Absent → no hypervisor support
* Present → enables VMs and isolation

---

# 💡 One-Line Answer

> EL2 exists only on ARM64 CPUs that implement virtualization; otherwise, it simply isn’t there.

---

If you want, I can show:

* How Linux boots differently **with vs without EL2**
* Or how **KVM switches between EL1 and EL2** in real systems


**Q5: What is the purpose of ELR_EL1 vs ELR_EL2?**

They are banked registers. When an exception is taken to EL1, `ELR_EL1` is written by hardware. When taken to EL2, `ELR_EL2` is written. Each EL has its own independent copy. The `ERET` instruction uses the ELR of the current EL to determine where to return.

**Q6: In KVM, when a guest executes HVC, what happens?**

A Hypervisor Call exception is taken. If the guest is at EL1 (AArch64), the exception goes to EL2. The CPU:
1. Saves guest EL1 PSTATE to `SPSR_EL2`.
2. Saves guest PC to `ELR_EL2`.
3. Jumps to `VBAR_EL2` + appropriate vector offset.
4. KVM handles the HVC (e.g., PSCI calls, paravirtualized I/O).
5. ERET returns to guest EL1, restoring state from `ELR_EL2`/`SPSR_EL2`.

---

## 9. Common Pitfalls & Gotchas

1. **Confusing EL with privilege rings**: ARM64 ELs are NOT the same as x86 protection rings 0–3. ARM64 only has EL0/EL1 in the most common case (no virtualization, no TrustZone active). There is no ring 1 or 2.

2. **Exceptions always go UP, never down**: A fault in EL2 code cannot be handled at EL3 unless it escalates. Exception routing is configured by `SCR_EL3` and `HCR_EL2` — exceptions default to the current EL or one above, never below.

3. **ERET does not flush the pipeline**: After `ERET`, the CPU returns to the lower EL immediately but the ISB effect is implicit — the new PC and PSTATE are guaranteed to be in effect before any subsequent instruction fetches at the returned-to EL.

4. **CurrentEL register reads EL, not mode**: `mrs x0, CurrentEL` returns `EL[3:2]` in bits [3:2]. You must shift right by 2 to get the numerical EL value (0–3).

5. **EL1t vs EL1h**: Within EL1, the processor can use `SP_EL0` (EL1t = "thread") or `SP_EL1` (EL1h = "handler"). Linux kernel runs with `SPSel=1` so it always uses `SP_EL1`. When saving/restoring user state, it explicitly reads/writes `SP_EL0`.

---

## 10. Quick Reference

| EL | Software Layer | Stack Pointer | Key System Registers |
|---|---|---|---|
| EL0 | User processes | SP_EL0 | TPIDR_EL0, TPIDRRO_EL0 |
| EL1 | Linux kernel | SP_EL1 (kernel) / SP_EL0 (user) | SCTLR_EL1, TCR_EL1, TTBR0/1_EL1, VBAR_EL1 |
| EL2 | KVM hypervisor | SP_EL2 | HCR_EL2, VTTBR_EL2, VTCR_EL2 |
| EL3 | ARM Trusted Firmware | SP_EL3 | SCR_EL3, SCTLR_EL3 |

| Transition | Instruction | Direction |
|---|---|---|
| User → Kernel | `SVC #0` | EL0 → EL1 |
| Kernel → Hypervisor | `HVC #imm` | EL1 → EL2 |
| Any → Secure Monitor | `SMC #imm` | EL1/EL2 → EL3 |
| Any → Lower EL | `ERET` | ELx → EL(x-1) or lower |
