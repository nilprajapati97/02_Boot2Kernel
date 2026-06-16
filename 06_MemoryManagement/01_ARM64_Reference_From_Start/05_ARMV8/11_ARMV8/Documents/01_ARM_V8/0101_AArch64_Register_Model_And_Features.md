# AArch64 (ARMv8 Execution State) — Deep Dive from First Principles

---

# 1. Big Picture: What is AArch64?

AArch64 is the **64-bit execution state** of the ARMv8-A architecture.

It defines:

* A **64-bit register model**
* A **modern instruction set (A64)**
* A **clean, orthogonal architecture design**
* A **scalable memory model**

👉 In simple terms:
**AArch64 is not just “ARM but 64-bit” — it is a redesigned architecture.**

---

# 2. Design Philosophy (Why AArch64 Exists)

ARMv7 had:

* Limited registers (16)
* Multiple instruction modes (ARM + Thumb)
* Complex condition execution

AArch64 was designed to:

* Remove legacy complexity
* Improve compiler efficiency
* Enable large memory systems
* Support modern OS features (virtualization, security)

---

# 3. Execution State Basics

AArch64 defines:

* **Execution state = AArch64**
* **Instruction set = A64**

👉 Important distinction:

* AArch64 → CPU state
* A64 → Instruction encoding used in that state

---

# 4. Register Architecture (Core of AArch64)

## 4.1 General Purpose Registers

* 31 registers:
  `X0 – X30` (each 64-bit)

### Dual View:

* `Xn` → 64-bit access
* `Wn` → lower 32-bit access

Example:

```asm
ADD X0, X1, X2
ADD W0, W1, W2
```

👉 Writing to `Wn` **zero-extends** into `Xn`

---

## 4.2 Special Registers

* `SP` → Stack Pointer
* `PC` → Program Counter (not directly accessible like ARMv7)
* `XZR` → Zero register (reads 0, writes ignored)

---

## 4.3 Link Register

* `X30` = Link Register (LR)
* Stores return address during function calls

---

## 4.4 Frame Pointer

* Conventionally: `X29`

Used for:

* Stack frame tracking
* Debugging

---

# 5. Instruction Set (A64)

## 5.1 Fixed-Length Design

* Every instruction = **32 bits**
* Simplifies decoding and pipelining

---

## 5.2 Load/Store Architecture

* Only load/store access memory
* Arithmetic works on registers only

Example:

```asm
LDR X0, [X1]
ADD X2, X0, X3
STR X2, [X1]
```

---

## 5.3 No Conditional Execution (Mostly Removed)

ARMv7:

* Every instruction could be conditional

AArch64:

* Uses:

  * Conditional branches (`B.eq`, `B.ne`)
  * Conditional select (`CSEL`)

---

## 5.4 Immediate Encoding

* Flexible but structured
* Not all constants directly encodable
* Uses MOVZ, MOVK sequences

---

# 6. Memory Model

## 6.1 Addressing

* Virtual addresses: up to **64-bit**
* Common implementation: **48-bit VA**

---

## 6.2 Address Space Layout

* User space
* Kernel space
* Controlled via translation tables

---

## 6.3 Alignment

* Most accesses require alignment
* Misaligned access may cause:

  * Performance penalty
  * Exception (depending on config)

---

## 6.4 Endianness

* Supports:

  * Little-endian (default)
  * Big-endian

---

# 7. Exception Model (Critical for OS)

## 7.1 Exception Levels (EL)

| Level | Purpose           |
| ----- | ----------------- |
| EL0   | User applications |
| EL1   | OS kernel         |
| EL2   | Hypervisor        |
| EL3   | Secure monitor    |

---

## 7.2 Execution State per EL

Each EL can run in:

* AArch64
* AArch32 (optional)

---

## 7.3 Exception Types

* Synchronous (e.g., divide by zero)
* IRQ (interrupt request)
* FIQ (fast interrupt)
* SError (system error)

---

## 7.4 Exception Handling Flow

1. Exception occurs
2. CPU switches to higher EL
3. Context saved in:

   * `ELR_ELx` (return address)
   * `SPSR_ELx` (status)

---

# 8. System Registers (Control Layer)

System registers control:

* Memory management (MMU)
* Caches
* Exception handling
* Debug features

Examples:

* `TTBR0_EL1` → Translation table base
* `SCTLR_EL1` → System control
* `ELR_EL1` → Exception return address

---

# 9. Virtual Memory & MMU

## 9.1 Translation Tables

* Multi-level page tables
* Typically 4KB pages

---

## 9.2 Address Translation Flow

Virtual Address → Page Table → Physical Address

---

## 9.3 Features

* Address space isolation
* Memory protection
* Efficient context switching

---

# 10. Calling Convention (AAPCS64)

## 10.1 Argument Passing

* `X0 – X7` → arguments
* Return value → `X0`

---

## 10.2 Register Roles

| Register | Role          |
| -------- | ------------- |
| X0–X7    | Arguments     |
| X9–X15   | Temporary     |
| X19–X28  | Callee-saved  |
| X29      | Frame pointer |
| X30      | Link register |

---

# 11. Stack Model

* Stack grows **downward**
* `SP` must be **16-byte aligned**

---

## Example Function

```asm
stp x29, x30, [sp, #-16]!
mov x29, sp
...
ldp x29, x30, [sp], #16
ret
```

---

# 12. SIMD & Floating Point (NEON)

* 32 registers:

  * `V0 – V31` (128-bit)

Supports:

* Vector operations
* Floating point

---

# 13. Security Features

## 13.1 Privilege Separation

* EL0 vs EL1 isolation

---

## 13.2 TrustZone

* Secure world vs normal world
* Managed via EL3

---

## 13.3 Pointer Authentication

* Prevents return address corruption
* Uses cryptographic signatures

---

# 14. Virtualization Support

* EL2 for hypervisor
* Supports:

  * Multiple virtual machines
  * Guest OS isolation

---

# 15. Performance Features

* Large register file
* Reduced instruction complexity
* Better branch prediction
* Efficient pipeline design

---

# 16. Key Differences from AArch32

| Feature          | AArch64     | AArch32      |
| ---------------- | ----------- | ------------ |
| Registers        | 31 (64-bit) | 16 (32-bit)  |
| Instruction sets | A64 only    | ARM + Thumb  |
| Addressing       | 64-bit      | 32-bit       |
| Complexity       | Simplified  | Legacy-heavy |

---

# 17. Common Misconceptions

❌ “AArch64 is just ARMv7 extended”
✔ Completely redesigned

❌ “More bits = just bigger memory”
✔ Also improves performance, ABI, and security

---

# 18. Real-World Systems

AArch64 is used in:

* Modern smartphones
* Servers
* Embedded systems

---

# 19. Mental Model (Expert View)

Think of AArch64 as:

* A **RISC architecture refined for modern computing**
* With:

  * Clean ISA
  * Strong OS support
  * Scalable memory model

---

# 20. Final Insight

AArch64 represents:

* The **future direction of ARM**
* A balance of:

  * Performance
  * Simplicity
  * Scalability
  * Security

👉 It is designed not just to run programs, but to support entire modern computing ecosystems.

---
