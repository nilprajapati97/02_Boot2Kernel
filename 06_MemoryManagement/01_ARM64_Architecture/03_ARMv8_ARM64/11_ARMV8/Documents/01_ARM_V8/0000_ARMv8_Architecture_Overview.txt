# Major Concepts of ARMv8 Architecture

---

## 📌 Overview
ARMv8 is a modern **64-bit architecture (AArch64)** designed for:
- High performance
- Power efficiency
- Scalability (mobile → servers)

It introduces several key architectural concepts that define how the system operates.

---

# 🧠 1. Execution States

ARMv8 supports two execution states:

## AArch64 (64-bit)
- Native 64-bit execution
- Uses 64-bit registers
- Main mode for modern OS (Linux, Android)

## AArch32 (32-bit)
- Backward compatibility with ARMv7
- Supports legacy applications

---

# 🔐 2. Exception Levels (Privilege Model)

ARMv8 uses a hierarchical privilege model:

| Level | Description |
|------|------------|
| EL3 | Secure Monitor (highest privilege) |
| EL2 | Hypervisor (virtualization) |
| EL1 | Operating System (Kernel) |
| EL0 | User Applications |

### Key Idea
- Higher EL can control lower EL
- Provides isolation and security

---

# 🧩 3. TrustZone (Security Model)

- Splits system into:
  - **Secure world**
  - **Non-secure world**

### Features:
- Secure OS runs in EL3
- Sensitive operations isolated
- Used in:
  - Secure boot
  - DRM
  - Cryptography

---

# 🔁 4. Memory System & MMU

## Memory Management Unit (MMU)
- Translates:
  - Virtual Address → Physical Address

## Features:
- Paging
- Address translation tables
- Memory protection

---

# 🧠 5. Cache Hierarchy

- L1 Cache (per core)
- L2 Cache (shared or per cluster)
- L3 Cache (optional)

## Key Concepts:
- Cache coherency across cores
- Improves performance by reducing memory access latency

---

# 🔗 6. Cache Coherency (ACE / CHI)

- Ensures all cores see consistent data

## Protocols:
- ACE (AMBA Coherency Extensions)
- CHI (Coherent Hub Interface)

## Mechanism:
- Snooping
- Directory-based coherency

---

# ⚡ 7. Interrupt Handling (GIC)

## Generic Interrupt Controller (GIC)
- Routes interrupts to CPU cores

### Types:
- IRQ (normal interrupt)
- FIQ (fast interrupt)

### Flow:
- Device → GIC → CPU → ISR

---

# 🚀 8. DMA (Direct Memory Access)

- Transfers data:
  - Device ↔ Memory
- CPU not involved in bulk transfer

## Benefits:
- High throughput
- Reduced CPU load

---

# 🔄 9. Interconnect (AMBA / AXI)

- Connects all SoC components

## Protocols:
- AXI (Advanced eXtensible Interface)
- AHB / APB (simpler buses)

## Role:
- Data movement between:
  - CPU
  - Memory
  - Peripherals

---

# 🧱 10. Boot Architecture

## Boot Stages:
1. BL1 (Boot ROM)
2. BL2 (Initialize memory)
3. BL31 (Runtime firmware)
4. BL33 (Bootloader / OS)

## Key Idea:
- Primary core boots first
- Secondary cores brought up later

---

# 🔁 11. Multi-Core Processing (SMP)

- Multiple cores run same OS

## Features:
- Shared memory
- Cache coherency
- Parallel execution

---

# 🧮 12. Register Model

## General Purpose Registers
- X0–X30 (64-bit)

## Special Registers
- PC (Program Counter)
- SP (Stack Pointer)
- PSTATE (Processor state)

---

# 🔧 13. Exception Handling

- Handles:
  - Interrupts
  - System calls
  - Faults

## Vector Table
- Contains handler addresses

---

# 💡 14. Power Management

- Uses **PSCI interface**

## Features:
- CPU on/off
- Sleep states
- Low-power modes

---

# 🔑 Summary of Key Concepts

| Concept | Purpose |
|--------|--------|
| Execution State | 32-bit / 64-bit support |
| Exception Levels | Privilege control |
| TrustZone | Security isolation |
| MMU | Address translation |
| Cache | Performance optimization |
| Coherency | Multi-core consistency |
| GIC | Interrupt handling |
| DMA | Efficient data transfer |
| AXI | Interconnect |
| Boot Flow | System initialization |
| SMP | Multi-core execution |

---

# 🚀 Final Insight

ARMv8 architecture is designed to:
- Balance **performance and power**
- Support **modern OS and virtualization**
- Enable **secure and scalable systems**

Used in:
- Smartphones
- Embedded systems
- Cloud servers (ARM Neoverse)
