# QMI — Qualcomm Messaging Interface

## Overview

**QMI (Qualcomm Messaging Interface)** is a request/response messaging protocol used for communication between the Apps processor and subsystem services (modem, ADSP, SLPI). It runs on top of **IPC Router** which uses GLINK/SMD as transport.

---

## QMI Stack

```
┌──────────────────────────────────────────────────────────────┐
│  Apps Processor                    Remote Processor           │
│                                    (Modem/ADSP/SLPI)         │
│  ┌────────────┐                    ┌────────────┐            │
│  │ QMI Client │                    │ QMI Service│            │
│  │ (kernel or │                    │ (firmware) │            │
│  │  userspace)│                    │            │            │
│  └─────┬──────┘                    └─────┬──────┘            │
│        │ QMI encode/decode                │                   │
│  ┌─────▼──────┐                    ┌─────▼──────┐            │
│  │ IPC Router │◄───── GLINK ─────►│ IPC Router │            │
│  │ (qrtr)     │    (transport)     │            │            │
│  └────────────┘                    └────────────┘            │
└──────────────────────────────────────────────────────────────┘
```

---

## QMI Message Format

```
QMI Message:
┌──────────────────────────────────┐
│  QMI Header                       │
│  ├── Type: Request/Response/Ind  │
│  ├── Transaction ID              │
│  ├── Message ID                  │
│  └── Length                      │
├──────────────────────────────────┤
│  TLV Payload (Type-Length-Value) │
│  ├── TLV 1: type=0x01, len, val │
│  ├── TLV 2: type=0x02, len, val │
│  └── TLV N: ...                 │
└──────────────────────────────────┘
```

---

## QMI Message Types

| Type | Direction | Purpose |
|------|-----------|---------|
| Request (0x00) | Client → Service | Ask for action or data |
| Response (0x02) | Service → Client | Reply to request |
| Indication (0x04) | Service → Client | Unsolicited notification |

---

## Example: Sensor Data via QMI (SLPI)

```
Android Sensor HAL → SSC (Sensor Service Client)
    │
    ├── QMI Request: "Enable accelerometer at 100 Hz"
    │   Service ID: SNS_CLIENT_SVC (0x400)
    │   Message ID: SNS_CLIENT_REQ
    │   TLV: sensor_type=ACCEL, sample_rate=100
    │
    ▼ IPC Router → GLINK → SLPI
    │
SLPI SNS Framework:
    ├── Receives QMI request
    ├── Enables BMI160 accelerometer on SLPI
    ├── BMI160 generates data at 100 Hz
    │
    ├── QMI Indication: "Accel data: x=0.1g, y=0.0g, z=1.0g"
    │   (sent every 10 ms)
    │
    ▼ GLINK → IPC Router → Apps
    │
Android Sensor HAL → SensorManager → App
```

---

## IPC Router (QRTR)

IPC Router provides service discovery and routing:

```
Service addressing:
  Service ID:  Unique per service type (e.g., 0x03 = NAS)
  Instance ID: Unique per instance
  Node ID:     Processor identifier

Lookup flow:
  1. Client: "I need NAS service (0x03)"
  2. IPC Router: "NAS is on node=modem, port=1234"
  3. Client connects to modem:1234 via GLINK
  4. QMI messages flow through IPC Router
```

---

## Kernel QMI Framework

```c
/* drivers/soc/qcom/qmi_interface.c */

/* Define QMI service */
static struct qmi_handle qmi;

/* Register QMI request handler */
static struct qmi_msg_handler handlers[] = {
    {
        .type = QMI_REQUEST,
        .msg_id = MY_REQ_MSG_ID,
        .fn = my_request_handler,
        .decoded_size = sizeof(struct my_req_msg),
        .decode = my_req_decode,
    },
};

/* Send QMI request */
qmi_txn_init(&qmi, &txn, my_resp_decode, &resp);
qmi_send_request(&qmi, &sq, &txn, MY_REQ_MSG_ID,
                  MY_REQ_MSG_SIZE, my_req_encode, &req);
qmi_txn_wait(&txn, timeout);
/* resp now contains the response */
```

---

## Debugging QMI

```bash
# QMI service list
adb shell cat /sys/kernel/debug/qmi/services

# IPC Router ports
adb shell cat /sys/kernel/debug/ipc_router/dump_local_ports

# QRTR service lookup (if qrtr-ns running)
adb shell qrtr-lookup

# QMI log (verbose)
adb shell dmesg | grep -i qmi
```

---

## Related Documents

- [04_GLINK_SMD.md](04_GLINK_SMD.md) — Transport layer for QMI
- [../07_Subsystem_Loading/04_SLPI.md](../07_Subsystem_Loading/04_SLPI.md) — Sensor QMI services
- [../07_Subsystem_Loading/05_Modem.md](../07_Subsystem_Loading/05_Modem.md) — Modem QMI services
