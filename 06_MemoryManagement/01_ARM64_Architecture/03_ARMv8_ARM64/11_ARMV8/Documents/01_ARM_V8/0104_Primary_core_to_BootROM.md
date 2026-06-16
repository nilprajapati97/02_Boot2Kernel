# ARMv8 Boot Flow – Primary Core to Application (Detailed Guide)

This document explains the complete boot flow in ARMv8-A architecture, starting from reset on the primary core (CPU0) all the way to application start, including firmware stages and multi-core bring-up.

---

# 🧭 1. Overview

* Architecture: ARMv8-A
* Initial execution level: EL3 (Secure)
* Only **primary core (CPU0)** starts execution after reset
* Secondary cores remain in reset or low-power state (WFE)

---

# 🔁 2. High-Level Boot Flow

```mermaid
flowchart TD
    A[Reset] --> B[BootROM]
    B --> C[BL1]
    C --> D[BL2]
    D --> E[BL31]
    E --> F[Bootloader U-Boot]
    F --> G[Linux Kernel]
    G --> H[SMP Init]
    H --> I[Secondary Cores Online]
    I --> J[User Space]
    J --> K[Application Start]
```

---

# 🔬 3. Detailed Sequence Diagram (Primary Core → Firmware)

```mermaid
sequenceDiagram
    participant CPU0 as CPU0 (Primary Core)
    participant SoC as Reset Logic / SoC
    participant ROM as BootROM
    participant BL1 as BL1
    participant BL2 as BL2
    participant BL31 as BL31 (EL3 Runtime FW)

    SoC->>CPU0: Power-on Reset
    Note right of CPU0: EL3, Secure
    SoC->>CPU0: Set PC to Reset Vector

    CPU0->>ROM: Execute BootROM
    ROM->>ROM: Init stack (SP_EL3)
    ROM->>ROM: Detect boot source
    ROM->>ROM: Load BL1 to SRAM
    ROM->>CPU0: Jump to BL1

    CPU0->>BL1: Execute BL1
    BL1->>BL1: Setup VBAR_EL3
    BL1->>BL1: Init secure env
    BL1->>BL1: Load BL2
    BL1->>CPU0: Jump to BL2

    CPU0->>BL2: Execute BL2
    BL2->>BL2: Init DRAM
    BL2->>BL2: Enable MMU + caches
    BL2->>BL2: Load BL31 & BL33
    BL2->>CPU0: Jump to BL31

    CPU0->>BL31: Execute BL31
    BL31->>BL31: Init PSCI
    BL31->>BL31: Setup SMC handler
    BL31->>CPU0: ERET to Non-secure world
```

---

# 🧠 4. Key Concepts

## Exception Levels

| Level | Description               |
| ----- | ------------------------- |
| EL3   | Secure Monitor (Firmware) |
| EL2   | Hypervisor (optional)     |
| EL1   | OS Kernel                 |
| EL0   | User Applications         |

---

## Important Registers (EL3)

* `VBAR_EL3` – Exception vector base
* `SCR_EL3` – Secure configuration
* `SPSR_EL3` – Saved program state
* `ELR_EL3` – Return address after ERET

---

# 🔥 5. Secondary Core Bring-Up (SMP)

```mermaid
sequenceDiagram
    participant CPU0 as Primary Core
    participant K as Linux Kernel
    participant FW as EL3 Firmware (PSCI)
    participant CPUX as Secondary Core

    K->>FW: psci_cpu_on(cpu, entry)
    FW->>CPUX: Power ON
    FW->>CPUX: Set entry point
    FW->>CPUX: SEV (wake event)

    CPUX->>K: secondary_start_kernel()
    CPUX->>K: cpu_startup_entry()
```

---

# 🐧 6. Kernel to User Space

```mermaid
sequenceDiagram
    participant K as Linux Kernel
    participant US as User Space

    K->>K: start_kernel()
    K->>K: smp_init()
    K->>K: rest_init()
    K->>US: run_init_process()

    US->>US: init (systemd)
    US->>US: Start services
    US->>US: Launch application
```

---

# ⚡ 7. Summary

* Boot starts in EL3 on primary core
* Firmware initializes system and loads next stages
* Kernel boots on CPU0 first
* Secondary CPUs are brought up using PSCI
* System becomes SMP
* User space starts and applications run

---

# 🎯 Mental Model

* CPU0 = Boot orchestrator
* EL3 firmware = Power & security controller
* Kernel = System manager
* Secondary CPUs = Workers
* User space = Applications

---

# 📌 Notes

* Some systems may use spin-table instead of PSCI
* EL2 is optional (used for virtualization)
* Boot flow may vary slightly by SoC vendor

---

End of Document
