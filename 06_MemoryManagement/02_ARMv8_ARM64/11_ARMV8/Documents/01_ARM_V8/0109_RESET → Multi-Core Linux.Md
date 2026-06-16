# ARMv8 End-to-End Boot Flow (RESET → Multi-Core Linux)

---

# 1. Overview

This document provides a complete deep dive into ARMv8-A boot flow:

* RESET → Boot ROM
* BL1 → BL2 → BL31 (ARM Trusted Firmware)
* Bootloader (U-Boot)
* Linux Kernel boot
* Multi-core bring-up via PSCI

---

# 2. High-Level Boot Flow

```
RESET
 ↓
Boot ROM (EL3)
 ↓
BL1 (EL3)
 ↓
BL2 (EL3)
 ↓
BL31 (EL3 Runtime Firmware)
 ↓
BL33 (Bootloader - EL2/EL1)
 ↓
Linux Kernel (EL1)
 ↓
SMP Bring-up (PSCI)
 ↓
User Space
```

---

# 3. Detailed Stage Breakdown

## 3.1 RESET + Boot ROM (EL3)

* Only CPU0 is active
* Other CPUs are held in reset or WFE
* Boot ROM:

  * Initializes minimal hardware
  * Loads BL1 from storage

---

## 3.2 BL1 (EL3)

Responsibilities:

* Setup stack
* Setup exception vectors
* Load BL2 into SRAM

Key flow:

```
bl1_main()
  → bl1_load_bl2()
  → jump to BL2
```

---

## 3.3 BL2 (EL3)

Responsibilities:

* Initialize DRAM
* Load images:

  * BL31 (runtime firmware)
  * BL33 (bootloader)

Key flow:

```
bl2_main()
  → load BL31
  → load BL33
  → pass entry info
```

---

## 3.4 BL31 (EL3 Runtime Firmware)

This is the **most critical stage for multi-core boot**.

Responsibilities:

* Provide PSCI implementation
* Handle CPU power control
* Manage secure monitor calls

Key entry:

```
bl31_main()
  → runtime_svc_init()
  → psci_setup()
```

---

# 4. PSCI Implementation (ATF Code Mapping)

## 4.1 CPU_ON Handler

```
psci_cpu_on(target_cpu, entry_point)
  → validate parameters
  → platform_cpu_on()
  → set entry point
  → send event (SEV)
```

Platform hook:

```
plat_psci_ops.cpu_on()
```

---

## 4.2 Secondary Core Boot (ATF Side)

```
secondary CPU reset vector
  → enter EL3
  → read entry address
  → jump to non-secure world (EL1/EL2)
```

---

# 5. Bootloader Stage (BL33)

Typical: U-Boot

Responsibilities:

* Initialize peripherals
* Load kernel image
* Load device tree
* Jump to kernel entry

Flow:

```
board_init_f()
  → board_init_r()
  → bootm()
  → jump to kernel
```

---

# 6. Linux Kernel Boot (Primary Core)

Entry:

```
start_kernel()
```

Flow:

```
start_kernel()
  → setup_arch()
  → mm_init()
  → smp_init()
  → rest_init()
```

---

# 7. SMP Bring-Up (Linux)

## 7.1 smp_init()

```
smp_init()
  → bringup_nonboot_cpus()
```

## 7.2 CPU Bring-up Loop

```
for_each_cpu:
  cpu_up(cpu)
```

## 7.3 PSCI Call

```
psci_cpu_on(cpu, secondary_entry)
```

---

# 8. Secondary CPU Boot (Linux Side)

## 8.1 Entry (Assembly)

```
secondary_entry:
  → secondary_startup
```

## 8.2 C Entry

```
secondary_start_kernel()
```

Flow:

```
secondary_start_kernel()
  → cpu_init()
  → enable MMU
  → setup interrupts
  → cpu_startup_entry()
```

---

# 9. Multi-Core PSCI Sequence Diagram

```
CPU0 (EL1)                EL3 (BL31)              CPU1
----------------------------------------------------------
cpu_up(1)
  → psci_cpu_on()
        → SMC call
            → psci_cpu_on handler
                → power on CPU1
                → set entry address
                → SEV ------------------------->
                                               Wake from WFE
                                               Enter EL3
                                               Jump to entry
                                               Enter EL1
                                               secondary_start_kernel()
                                               Join scheduler
```

---

# 10. End-to-End Timeline

```
RESET
 ↓
Boot ROM
 ↓
BL1
 ↓
BL2
 ↓
BL31 (PSCI ready)
 ↓
Bootloader
 ↓
Kernel start (CPU0)
 ↓
smp_init()
 ↓
psci_cpu_on()
 ↓
Secondary CPUs start
 ↓
SMP Ready
 ↓
User Space
```

---

# 11. Key Takeaways

* Only one CPU starts at reset
* EL3 firmware controls power (PSCI)
* Kernel cannot directly wake CPUs
* Secondary CPUs start via a provided entry point
* SMP is enabled only after all CPUs join scheduler

---

# 12. Debugging Tips

* Check dmesg for:

  * "psci: CPUx powered up"
  * "Booted secondary processor"
* Validate device tree CPU nodes
* Ensure firmware PSCI implementation is correct

---

# 13. Mental Model

* CPU0 = orchestrator
* EL3 = authority (power control)
* Kernel = requester
* Secondary CPUs = workers

---

# End of Document
