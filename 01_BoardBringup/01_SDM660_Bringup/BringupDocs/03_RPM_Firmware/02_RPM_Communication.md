# RPM Communication Protocol

## Overview

The Application Processor communicates with the RPM co-processor using a **message-based protocol** over **SMD (Shared Memory Device)** or **GLINK** channels, with messages stored in **RPM Message RAM** and **SMEM**. This document covers how Linux talks to RPM.

---

## Communication Architecture

```
┌─────────────────────────┐          ┌─────────────────────────┐
│   Linux Kernel           │          │   RPM Firmware           │
│                         │          │                         │
│  ┌───────────────────┐  │          │  ┌───────────────────┐  │
│  │  RPM SMD Driver   │  │          │  │  RPM Message       │  │
│  │  (smd-rpm.c)      │  │          │  │  Handler           │  │
│  └────────┬──────────┘  │          │  └────────┬──────────┘  │
│           │              │          │           │              │
│  ┌────────▼──────────┐  │          │  ┌────────▼──────────┐  │
│  │  SMD Transport    │  │          │  │  SMD Transport    │  │
│  └────────┬──────────┘  │          │  └────────┬──────────┘  │
│           │              │          │           │              │
└───────────┼──────────────┘          └───────────┼──────────────┘
            │                                     │
    ┌───────▼─────────────────────────────────────▼───────┐
    │              RPM Message RAM                         │
    │              @ 0x0077_8000                            │
    │                                                      │
    │  ┌─────────────────┐    ┌─────────────────┐         │
    │  │  TX FIFO        │    │  RX FIFO        │         │
    │  │  (Apps → RPM)   │    │  (RPM → Apps)   │         │
    │  └─────────────────┘    └─────────────────┘         │
    └──────────────────────────────────────────────────────┘
```

---

## RPM Message Format

### Request Message (Apps → RPM)

```
┌────────────────────────────────────────────┐
│  RPM Request Message                        │
│                                            │
│  Header (8 bytes):                         │
│  ├── msg_type: 0x00716572 ("req")          │
│  └── msg_length: payload size              │
│                                            │
│  Resource Header (8 bytes):                │
│  ├── resource_type: e.g., "smpa" (SMPS)    │
│  ├── resource_id: e.g., 1 (S1)            │
│  └── set_type: ACTIVE(0) or SLEEP(1)      │
│                                            │
│  Key-Value Pairs:                          │
│  ├── key: "uv" (microvolts)               │
│  │   value: 872000                         │
│  ├── key: "ma" (milliamps max)             │
│  │   value: 300                            │
│  └── key: "md" (mode)                      │
│      value: 2 (AUTO)                       │
└────────────────────────────────────────────┘
```

### Response Message (RPM → Apps)

```
┌────────────────────────────────────────────┐
│  RPM Response Message                       │
│                                            │
│  Header:                                   │
│  ├── msg_type: 0x006B6361 ("ack")          │
│  └── msg_length: payload size              │
│                                            │
│  Status: 0 = success, non-zero = error     │
└────────────────────────────────────────────┘
```

---

## RPM Resource Types

| Type Code | Resource | Description |
|-----------|----------|-------------|
| `smpa` | SMPS (Buck) | Switch-mode power supply (PM660) |
| `smpb` | SMPS (Buck) | Switch-mode power supply (PM660L) |
| `ldoa` | LDO | Low dropout regulator (PM660) |
| `ldob` | LDO | Low dropout regulator (PM660L) |
| `clka` | Clock | RPM-managed clock enable/rate |
| `bwma` | Bandwidth | Bus bandwidth vote |
| `rwcx` | CX floor | VDD_CX corner vote |
| `rwmx` | MX floor | VDD_MX corner vote |

---

## Linux Kernel RPM Interface

### SMD-RPM Driver

```c
/* drivers/soc/qcom/smd-rpm.c */

/* Send a request to RPM */
int qcom_rpm_smd_write(struct qcom_smd_rpm *rpm,
                        int state,           /* ACTIVE or SLEEP */
                        u32 resource_type,   /* e.g., QCOM_SMD_RPM_SMPA */
                        u32 resource_id,     /* e.g., 1 for S1 */
                        void *buf,           /* key-value data */
                        size_t count)
{
    /* 1. Format message header + payload */
    /* 2. Write to SMD channel TX FIFO */
    /* 3. Trigger RPM interrupt */
    /* 4. Wait for ACK response */
}
```

### RPM Regulator Driver

```c
/* drivers/regulator/qcom_smd-regulator.c */

/* Called when a kernel driver requests a voltage change */
static int rpm_reg_set_voltage(struct regulator_dev *rdev,
                                int min_uV, int max_uV,
                                unsigned *selector)
{
    struct qcom_rpm_reg *vreg = rdev_get_drvdata(rdev);

    /* Build key-value: key="uv", value=min_uV */
    rpm_reg_write_active(vreg, "uv", min_uV);

    /* Send to RPM via SMD */
    return qcom_rpm_smd_write(vreg->rpm, QCOM_RPM_ACTIVE_STATE,
                               vreg->type, vreg->id,
                               &vreg->buf, vreg->buf_len);
}
```

---

## RPM Sleep Set

The "sleep set" allows Linux to pre-program RPM with the resource levels needed during Apps processor sleep:

```
While Apps is awake:
────────────────────
1. Driver sets active voltage: rpm_reg_set_voltage(S1, 872000)
   → RPM applies immediately

2. Driver sets sleep voltage: rpm_reg_set_sleep_voltage(S1, 500000)
   → RPM stores in sleep set (not applied yet)

When Apps enters sleep:
───────────────────────
1. PSCI triggers power collapse
2. RPM receives "Apps sleeping" signal
3. RPM applies sleep set values:
   S1: 872 mV → 500 mV (or OFF if no other voter)
4. Power consumption drops significantly

When Apps wakes up:
──────────────────
1. Wake interrupt arrives
2. RPM restores active set values:
   S1: 500 mV → 872 mV
3. CPU resumes
```

---

## RPM Communication Handshake (Boot)

During boot, the kernel establishes communication with RPM:

```
1. XBL loads RPM firmware and releases RPM from reset
2. RPM initializes and sets SMSM_RPM_READY bit
3. Kernel SMD driver opens "rpm_requests" channel
4. Kernel sends first message → RPM responds with ACK
5. Communication established
6. Kernel registers RPM regulators and clocks
```

### Kernel Boot Log

```
[    0.512] qcom-smd-rpm rpm_requests: RPM channel opened
[    0.520] qcom_smd_regulator: pm660_s1: Set voltage 872000 uV
[    0.521] qcom_smd_regulator: pm660_s2: Set voltage 1050000 uV
[    0.523] rpmcc-sdm660: Registered RPM clocks
```

---

## Debugging RPM Communication

```bash
# RPM log
adb shell cat /sys/kernel/debug/rpm_log

# RPM stats
adb shell cat /sys/kernel/debug/rpm_stats

# SMD channel status
adb shell cat /sys/kernel/debug/smd/ch

# RPM regulator votes
adb shell cat /sys/kernel/debug/regulator/regulator_summary
```

### Common Issues

| Problem | Symptom | Fix |
|---------|---------|-----|
| RPM not responding | Kernel hangs at regulator init | Check RPM firmware, XBL loading |
| SMD channel not opening | "rpm_requests: open failed" | Check SMEM init, SMSM handshake |
| Voltage not changing | Regulator shows wrong voltage | Check RPM message format, key-value |
| Sleep not working | Device never enters deep sleep | Check sleep set programming |

---

## Related Documents

- [01_RPM_Overview.md](01_RPM_Overview.md) — RPM architecture
- [../08_IPC_Mechanisms/04_GLINK_SMD.md](../08_IPC_Mechanisms/04_GLINK_SMD.md) — SMD transport details
- [../08_IPC_Mechanisms/01_SMEM_Shared_Memory.md](../08_IPC_Mechanisms/01_SMEM_Shared_Memory.md) — SMEM base for RPM
- [../05_Linux_Kernel_Boot/06_Regulator_Framework.md](../05_Linux_Kernel_Boot/06_Regulator_Framework.md) — Kernel regulator framework
