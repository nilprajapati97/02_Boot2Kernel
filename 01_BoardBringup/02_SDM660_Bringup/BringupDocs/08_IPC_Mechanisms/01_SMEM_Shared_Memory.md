# SMEM — Shared Memory

## Overview

**SMEM (Shared Memory)** is a region of DDR that is shared between all processors on the SDM660 SoC (Apps, Modem, ADSP, CDSP, SLPI, RPM). It provides a lock-protected key-value store for exchanging data and state between processors.

---

## SMEM Architecture

```
┌──────────────────────────────────────────────────────────────┐
│                    DDR Physical Memory                        │
│                                                              │
│  ┌────────────────────────────────────────────────────────┐  │
│  │  SMEM Region @ 0x8600_0000 (~4 MB)                    │  │
│  │                                                        │  │
│  │  ┌──────────────┐                                     │  │
│  │  │ SMEM Header  │  Magic, version, num_items          │  │
│  │  ├──────────────┤                                     │  │
│  │  │ Item Table   │  Array of (id, offset, size) tuples │  │
│  │  │ (TOC)        │  Up to ~512 items                   │  │
│  │  ├──────────────┤                                     │  │
│  │  │ Item 0 data  │  Heap allocation area               │  │
│  │  │ Item 1 data  │                                     │  │
│  │  │ Item 2 data  │                                     │  │
│  │  │ ...          │                                     │  │
│  │  ├──────────────┤                                     │  │
│  │  │ Partition-   │  Per-processor-pair private areas    │  │
│  │  │ based items  │  (e.g., Apps↔Modem, Apps↔ADSP)      │  │
│  │  └──────────────┘                                     │  │
│  └────────────────────────────────────────────────────────┘  │
│                                                              │
│  Accessed by:                                                │
│  ├── Apps CPU (Linux)                                       │
│  ├── Modem DSP                                              │
│  ├── ADSP                                                   │
│  ├── CDSP                                                   │
│  ├── SLPI                                                   │
│  └── RPM                                                    │
└──────────────────────────────────────────────────────────────┘
```

---

## SMEM Items

SMEM stores data as numbered items. Each item has a fixed ID:

| Item ID | Name | Size | Description |
|---------|------|------|-------------|
| 0 | SMEM_PROC_COMM | 64 | Legacy processor communication |
| 2 | SMEM_HEAP_INFO | 16 | Heap metadata |
| 13 | SMEM_VERSION_INFO | 128 | Per-processor version info |
| 134 | SMEM_HW_SW_BUILD_ID | 128 | Build ID string |
| 137 | SMEM_CHANNEL_ALLOC_TBL | 2 KB | SMD channel allocation |
| 497 | SMEM_SOCINFO | 256 | SoC ID, revision, serial |
| 498 | SMEM_SMSM | varies | SMSM state bits |

---

## SMEM Initialization

```
XBL:
1. Allocates SMEM physical region (from reserved memory)
2. Initializes SMEM header (magic number, version)
3. Creates item table (TOC)
4. Allocates initial items (version info, SoC info)
5. Other processors (RPM, modem) can now access SMEM

Kernel:
1. Maps SMEM physical address to virtual (ioremap)
2. Verifies SMEM header magic and version
3. Can read/write SMEM items via smem_get_entry() API
```

---

## Kernel SMEM API

```c
/* drivers/soc/qcom/smem.c */

/* Read an SMEM item */
void *qcom_smem_get(unsigned host, unsigned item, size_t *size)
{
    /* host: target processor (or SMEM_ANY_HOST for global) */
    /* item: SMEM item ID */
    /* Returns pointer to item data (mapped), size in *size */
}

/* Allocate a new SMEM item */
int qcom_smem_alloc(unsigned host, unsigned item, size_t size)
{
    /* Allocates item in SMEM heap */
    /* Thread-safe via spinlock in SMEM header */
}

/* Example: Read SoC info */
struct socinfo *info = qcom_smem_get(SMEM_ANY_HOST, SMEM_SOCINFO, &size);
pr_info("SoC ID: %d, Rev: %d\n", info->id, info->rev);
```

---

## Device Tree

```dts
smem {
    compatible = "qcom,smem";
    memory-region = <&smem_mem>;
    hwlocks = <&tcsr_mutex 3>;  /* Hardware spinlock for thread safety */
};

reserved-memory {
    smem_mem: smem_region@86000000 {
        reg = <0x0 0x86000000 0x0 0x200000>;  /* 2 MB */
        no-map;
    };
};
```

---

## Debugging SMEM

```bash
# SMEM info
adb shell cat /sys/kernel/debug/smem/info

# SoC info from SMEM
adb shell cat /sys/devices/soc0/soc_id     # 317 (SDM660)
adb shell cat /sys/devices/soc0/revision   # 2.0
adb shell cat /sys/devices/soc0/serial_number
```

---

## Related Documents

- [02_SMSM.md](02_SMSM.md) — State machine built on SMEM
- [03_SMP2P.md](03_SMP2P.md) — Point-to-point signaling via SMEM
- [04_GLINK_SMD.md](04_GLINK_SMD.md) — Message channels using SMEM FIFOs
