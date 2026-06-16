# SMSM — Shared Memory State Machine

## Overview

**SMSM (Shared Memory State Machine)** is a mechanism where each processor maintains a set of state bits in SMEM. Other processors can read these bits to determine the state of a remote processor. Changes to state bits trigger interrupts to notify interested processors.

---

## SMSM Architecture

```
SMEM Region:
┌──────────────────────────────────────────────────────┐
│  SMSM Entry Array (one per processor)                │
│                                                      │
│  ┌─────────────┐ ┌─────────────┐ ┌─────────────┐   │
│  │ Apps State   │ │ Modem State │ │ ADSP State  │   │
│  │ Bits (32)    │ │ Bits (32)   │ │ Bits (32)   │   │
│  │              │ │             │ │             │   │
│  │ Bit 0: INIT  │ │ Bit 0: INIT│ │ Bit 0: INIT│   │
│  │ Bit 2: SLEEP │ │ Bit 2:SLEEP│ │             │   │
│  │ Bit 3: RUN   │ │ Bit 3: RUN │ │             │   │
│  │ Bit 17: RPM  │ │            │ │             │   │
│  │   READY      │ │            │ │             │   │
│  └─────────────┘ └─────────────┘ └─────────────┘   │
│                                                      │
│  ┌─────────────┐ ┌─────────────┐                    │
│  │ RPM State   │ │ SLPI State  │                    │
│  │ Bits (32)   │ │ Bits (32)   │                    │
│  └─────────────┘ └─────────────┘                    │
└──────────────────────────────────────────────────────┘

Each processor can:
  - Set/clear its own state bits
  - Read any processor's state bits
  - Register callbacks for state bit changes on other processors
```

---

## Key SMSM State Bits

| Bit | Name | Meaning |
|-----|------|---------|
| 0 | SMSM_INIT | Processor has initialized |
| 1 | SMSM_OSENABLED | OS is running |
| 2 | SMSM_SLEEP | Processor entering sleep |
| 3 | SMSM_RUN | Processor is running |
| 6 | SMSM_SYSTEM_DOWNLOAD | Entering crash dump mode |
| 17 | SMSM_RPM_READY | RPM firmware is ready |

---

## Usage Example

```c
/* Kernel waits for RPM to be ready */
/* RPM sets SMSM_RPM_READY bit when its firmware is initialized */

/* Register callback for RPM state changes */
smsm_change_state(SMSM_RPM_STATE, 0, SMSM_RPM_READY);

/* In RPM ready callback: */
static void rpm_ready_callback(void *data, uint32_t old, uint32_t new)
{
    if (new & SMSM_RPM_READY) {
        pr_info("RPM is ready, starting communication\n");
        /* Open SMD/GLINK channels to RPM */
    }
}
```

---

## SMSM Notification Flow

```
Modem sets SMSM_INIT bit:
─────────────────────────
1. Modem writes to its SMSM entry in SMEM
2. Modem triggers IPC interrupt to Apps processor
3. Apps SMSM driver reads modem state bits
4. Compares old vs new → bit 0 changed (INIT set)
5. Calls registered callbacks for modem SMSM_INIT
6. PIL driver: "Modem has initialized"
```

---

## Related Documents

- [01_SMEM_Shared_Memory.md](01_SMEM_Shared_Memory.md) — SMEM base
- [03_SMP2P.md](03_SMP2P.md) — More flexible signaling mechanism
