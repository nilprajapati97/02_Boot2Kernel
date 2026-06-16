# GLINK & SMD — Message Channels

## Overview

**SMD (Shared Memory Device)** and **GLINK (Generic Link)** are the primary message-passing frameworks between processors on SDM660. They provide reliable, bidirectional, named channels over shared memory FIFOs. GLINK is the newer replacement for SMD, but both coexist on SDM660.

---

## Architecture

```
┌──────────────────────────────────────────────────────────────┐
│                    Message Channel Stack                       │
│                                                              │
│  ┌───────────────────────────────────────────────────────┐   │
│  │  Clients (Kernel Drivers)                              │   │
│  │  ├── RPM SMD driver (rpm_requests channel)            │   │
│  │  ├── QMI driver (various QMI channels)                │   │
│  │  ├── APR driver (apr_audio_svc channel)               │   │
│  │  └── Diag driver (DIAG channel)                       │   │
│  └───────────────────────┬───────────────────────────────┘   │
│                          │                                    │
│  ┌───────────────────────▼───────────────────────────────┐   │
│  │  GLINK Core / SMD Core                                 │   │
│  │  ├── Channel management (open/close/state)            │   │
│  │  ├── Flow control                                     │   │
│  │  ├── Intent-based receive (GLINK)                     │   │
│  │  └── Named channels with state machine                │   │
│  └───────────────────────┬───────────────────────────────┘   │
│                          │                                    │
│  ┌───────────────────────▼───────────────────────────────┐   │
│  │  Transport Layer                                       │   │
│  │                                                        │   │
│  │  ┌──────────────────────────────────────────────────┐ │   │
│  │  │ SMEM-based Transport                              │ │   │
│  │  │                                                    │ │   │
│  │  │  Shared Memory:                                    │ │   │
│  │  │  ┌────────────┐  ┌────────────┐                   │ │   │
│  │  │  │ TX FIFO    │  │ RX FIFO    │                   │ │   │
│  │  │  │ (Apps→Rem) │  │ (Rem→Apps) │                   │ │   │
│  │  │  └────────────┘  └────────────┘                   │ │   │
│  │  │                                                    │ │   │
│  │  │  IPC Interrupt:                                    │ │   │
│  │  │  ├── Apps → Remote: write to APCS_IPC register    │ │   │
│  │  │  └── Remote → Apps: GIC SPI interrupt             │ │   │
│  │  └──────────────────────────────────────────────────┘ │   │
│  └───────────────────────────────────────────────────────┘   │
└──────────────────────────────────────────────────────────────┘
```

---

## SMD vs GLINK

| Feature | SMD (Legacy) | GLINK (New) |
|---------|-------------|-------------|
| FIFO | Fixed-size per channel | Shared across channels |
| Flow control | FIFO watermarks | Intent-based |
| Channels | Pre-allocated in SMEM | Dynamic |
| Multi-channel | Per-channel FIFO pair | Multiplexed |
| Used for | RPM, legacy modem | ADSP, CDSP, SLPI, modem |

---

## Named Channels

| Channel Name | Endpoint | Protocol | Purpose |
|-------------|----------|----------|---------|
| `rpm_requests` | RPM | SMD | Regulator/clock/bandwidth votes |
| `IPCRTR` | Modem | GLINK | IPC Router (QMI transport) |
| `IPCRTR` | ADSP | GLINK | IPC Router (QMI transport) |
| `IPCRTR` | SLPI | GLINK | IPC Router (QMI/SNS transport) |
| `apr_audio_svc` | ADSP | GLINK | Audio Processing Router |
| `DIAG` | Modem | GLINK | Diagnostic messages (QXDM) |

---

## Channel State Machine

```
Channel States:
───────────────
CLOSED → OPENING → OPENED → CLOSING → CLOSED

Both sides must open:
  Local:  CLOSED → OPENING (local open request)
  Remote: CLOSED → OPENING (remote open request)
  Both OPENING → OPENED (channel is ready)

Message flow (OPENED state):
  1. Sender writes message to TX FIFO
  2. Sender triggers IPC interrupt to remote
  3. Remote reads from its RX FIFO
  4. Remote processes message
  5. Remote may send response via its TX FIFO
```

---

## IPC Interrupts

Processors notify each other of FIFO updates via hardware interrupts:

```
Apps → Modem:   Write to APCS_IPC register → triggers modem IRQ
Apps → ADSP:    Write to APCS_IPC register → triggers ADSP IRQ
Apps → RPM:     Write to APCS_IPC register → triggers RPM IRQ
Modem → Apps:   Modem writes IPC → GIC SPI interrupt to Apps
ADSP → Apps:    ADSP writes IPC → GIC SPI interrupt to Apps
RPM → Apps:     RPM writes IPC → GIC SPI interrupt to Apps
```

---

## Kernel API

```c
/* Open a GLINK channel */
void *handle = glink_open(&cfg);

struct glink_open_config cfg = {
    .transport = "smem",
    .edge = "mpss",              /* modem */
    .name = "my_channel",
    .notify_rx = my_rx_callback,
    .notify_tx_done = my_tx_done_callback,
    .notify_state = my_state_callback,
};

/* Send data */
glink_tx(handle, pkt_priv, data, size, GLINK_TX_REQ_INTENT);

/* Receive callback */
void my_rx_callback(void *handle, void *priv, void *pkt_priv,
                     void *ptr, size_t size)
{
    /* Process received data */
    glink_rx_done(handle, ptr, false);
}
```

---

## Debugging

```bash
# SMD channel status
adb shell cat /sys/kernel/debug/smd/ch

# GLINK channel status
adb shell cat /sys/kernel/debug/glink/ch

# IPC interrupt counters
adb shell cat /proc/interrupts | grep "ipc\|smd\|glink"
```

---

## Related Documents

- [01_SMEM_Shared_Memory.md](01_SMEM_Shared_Memory.md) — Underlying shared memory
- [05_QMI.md](05_QMI.md) — QMI protocol over GLINK
- [../03_RPM_Firmware/02_RPM_Communication.md](../03_RPM_Firmware/02_RPM_Communication.md) — RPM uses SMD
