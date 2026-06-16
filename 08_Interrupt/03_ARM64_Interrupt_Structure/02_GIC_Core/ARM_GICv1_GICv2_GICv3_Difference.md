# ARM Generic Interrupt Controller (GIC) Detailed Design

## Table of Contents
1. Introduction
2. GICv1 Detailed Design
3. GICv2 Detailed Design
4. GICv3 Detailed Design
5. Comparison Table
6. References

---

## 1. Introduction
The ARM Generic Interrupt Controller (GIC) is a hardware block that manages interrupts in ARM-based systems. It is responsible for prioritizing, routing, and delivering interrupts to the appropriate CPU cores. Over time, the GIC architecture has evolved through several versions: GICv1, GICv2, and GICv3, each introducing new features and improvements.

---

## 2. GICv1 Detailed Design
### 2.1 Overview
- Introduced with ARMv7-A architecture.
- Supports up to 8 CPU interfaces.
- Consists of a Distributor and CPU Interface.

### 2.2 Key Components
- **Distributor**: Manages interrupt prioritization and distribution.
- **CPU Interface**: Delivers interrupts to the processor.

### 2.3 Features
- Supports Software Generated Interrupts (SGIs), Private Peripheral Interrupts (PPIs), and Shared Peripheral Interrupts (SPIs).
- Basic interrupt prioritization and masking.
- No support for security extensions (TrustZone).

### 2.4 Limitations
- Limited scalability (max 8 CPUs).
- No virtualization or security support.

---

## 3. GICv2 Detailed Design
### 3.1 Overview
- Backward compatible with GICv1.
- Supports up to 8 CPU interfaces.
- Adds support for security extensions (TrustZone).

### 3.2 Key Components
- **Distributor**: Enhanced for security and more flexible interrupt routing.
- **CPU Interface**: Improved for TrustZone support.

### 3.3 Features
- Security extensions: Secure/Non-secure interrupt handling.
- Improved software interrupt generation.
- Grouping of interrupts (Group 0: Secure, Group 1: Non-secure).
- Binary Point Register for finer priority control.

### 3.4 Limitations
- Still limited to 8 CPUs.
- No support for virtualization.

---

## 4. GICv3 Detailed Design
### 4.1 Overview
- Major architectural update for ARMv8-A.
- Supports thousands of CPUs (scalable to large systems).
- Introduces new components: Redistributor and GIC CPU Interface.

### 4.2 Key Components
- **Distributor**: Central interrupt management.
- **Redistributor**: Handles interrupt targeting for each CPU.
- **CPU Interface**: Now memory-mapped (System Register Interface).

### 4.3 Features
- Scalable to large multicore systems (up to 64K CPUs).
- Support for virtualization (virtual interrupts, List Registers).
- Enhanced security and interrupt grouping (Group 0, 1 Secure, 1 Non-secure).
- System Register Interface for low-latency access.
- Message-based interrupts (MBIs) for PCIe/MSI support.
- LPIs (Locality-specific Peripheral Interrupts) for scalable device interrupt support.

### 4.4 Limitations
- Increased complexity.
- Requires software and OS support for advanced features.

---

## 5. Comparison Table
| Feature                | GICv1         | GICv2         | GICv3         |
|------------------------|---------------|---------------|---------------|
| Max CPUs               | 8             | 8             | 64K           |
| Security Extensions    | No            | Yes           | Yes           |
| Virtualization         | No            | No            | Yes           |
| Redistributor          | No            | No            | Yes           |
| System Register IF     | No            | No            | Yes           |
| Message-based IRQs     | No            | No            | Yes           |
| LPIs                   | No            | No            | Yes           |

---

## 6. References
- ARM GIC Architecture Specification
- ARMv7-A and ARMv8-A Architecture Reference Manuals
- ARM Technical Documentation
