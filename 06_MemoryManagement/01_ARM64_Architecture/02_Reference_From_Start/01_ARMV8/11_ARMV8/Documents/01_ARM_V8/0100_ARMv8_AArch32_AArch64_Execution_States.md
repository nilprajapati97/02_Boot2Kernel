# ARMv8 Execution States (AArch64 vs AArch32) — Complete Guide

## 1. Introduction to ARMv8

ARMv8 is a major evolution of the ARM architecture that introduced **64-bit computing** while maintaining compatibility with existing **32-bit software**. This dual capability is achieved through two execution states:

* **AArch64** → 64-bit execution (modern mode)
* **AArch32** → 32-bit execution (legacy compatibility)

---

## 2. What is an Execution State?

An **execution state** defines how the CPU operates, including:

* Register size and structure
* Instruction set format
* Memory addressing capability
* Overall CPU behavior

ARMv8 processors can switch between two execution states depending on system requirements.

---

## 3. AArch64 (64-bit Execution State)

### 3.1 Overview

AArch64 is the **primary execution state** of ARMv8, designed for modern operating systems and applications.

---

### 3.2 Register Architecture

* 31 General Purpose Registers:

  * `X0 – X30` (64-bit each)
* Special Registers:

  * `SP` → Stack Pointer
  * `PC` → Program Counter

Lower 32 bits of each register can be accessed as:

* `W0 – W30`

---

### 3.3 Memory Addressing

* Supports **64-bit addressing**
* Practical implementations use ~48-bit virtual addresses
* Enables access to very large memory spaces

---

### 3.4 Instruction Set

* Fixed-length instructions (32-bit)
* Simplified and efficient design
* No Thumb mode (unlike AArch32)

---

### 3.5 Features

* More registers → better performance
* Cleaner instruction design
* Improved security mechanisms
* Better compiler optimization support

---

### 3.6 Example

```asm
ADD X0, X1, X2   // 64-bit addition
```

---

## 4. AArch32 (32-bit Execution State)

### 4.1 Overview

AArch32 exists for **backward compatibility** with older ARM architectures such as ARMv7.

---

### 4.2 Register Architecture

* 16 General Purpose Registers:

  * `R0 – R15` (32-bit each)

Special roles:

* `R13` → Stack Pointer
* `R14` → Link Register
* `R15` → Program Counter

---

### 4.3 Instruction Sets

AArch32 supports two instruction sets:

| Instruction Set | Description                   |
| --------------- | ----------------------------- |
| ARM             | 32-bit instructions           |
| Thumb           | 16-bit / mixed (more compact) |

---

### 4.4 Memory Addressing

* 32-bit addressing
* Maximum addressable memory: **4 GB**

---

### 4.5 Example

```asm
ADD R0, R1, R2   // 32-bit addition
```

---

## 5. Why ARMv8 Has Two Execution States

### 5.1 Backward Compatibility

* Millions of existing applications were built for 32-bit ARM
* AArch32 allows them to run without modification

---

### 5.2 Smooth Transition

* Developers can gradually migrate to 64-bit
* Systems can support both old and new applications

---

### 5.3 Mixed Workloads

* Operating system can run in AArch64
* Legacy applications can run in AArch32

---

## 6. Switching Between AArch64 and AArch32

* Controlled by system configuration and privilege levels
* Managed by the operating system
* Not frequently switched during normal execution

Typical scenario:

* OS runs in AArch64
* Some applications run in AArch32

---

## 7. Exception Levels in ARMv8

ARMv8 introduces **Exception Levels (ELs)** for privilege control:

| Level | Description             |
| ----- | ----------------------- |
| EL0   | User applications       |
| EL1   | Operating system kernel |
| EL2   | Hypervisor              |
| EL3   | Secure monitor          |

Each level can operate in either:

* AArch64
* AArch32

(depending on configuration)

---

## 8. Key Differences

| Feature             | AArch64          | AArch32        |
| ------------------- | ---------------- | -------------- |
| Register Size       | 64-bit           | 32-bit         |
| Number of Registers | 31               | 16             |
| Address Space       | Large (48+ bits) | 4 GB           |
| Instruction Set     | Simplified       | ARM + Thumb    |
| Performance         | Higher           | Lower          |
| Usage               | Modern systems   | Legacy support |

---

## 9. Conceptual Understanding

ARMv8 can be viewed as a system that:

* Uses **AArch64** for modern, high-performance computing
* Retains **AArch32** for compatibility with older software

This dual approach ensures both **innovation and stability**.

---

## 10. Real-World Usage

* Modern systems use AArch64 as the default
* Some legacy applications still rely on AArch32
* Newer processors are gradually removing AArch32 support

---

## 11. Conclusion

ARMv8’s dual execution states provide:

* A powerful 64-bit architecture for modern computing
* Continued support for legacy 32-bit applications

This design enables a smooth transition from older systems while supporting future advancements.

---
