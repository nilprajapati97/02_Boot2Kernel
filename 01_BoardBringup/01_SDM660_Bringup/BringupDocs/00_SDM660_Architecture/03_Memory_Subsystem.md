# Memory Subsystem — DDR & BIMC

## Overview

The SDM660 uses **LPDDR4/LPDDR4x** external memory connected through the **BIMC (Bus Interconnect Memory Controller)**. The memory subsystem is critical — it's one of the first things initialized during boot (by XBL) and is the backbone for all data flow in the system.

---

## Memory Architecture

```
┌─────────────────────────────────────────────────────┐
│                    SDM660 SoC                        │
│                                                     │
│   CPU ──┐                                           │
│   GPU ──┤                                           │
│   DSP ──┤    ┌──────────────────────┐               │
│   DMA ──┼───▶│       BIMC           │               │
│   USB ──┤    │  Bus Interconnect    │               │
│   UFS ──┤    │  Memory Controller   │               │
│   CAM ──┘    │                      │               │
│              │  ┌────────┐ ┌──────┐ │               │
│              │  │Channel │ │Chan  │ │               │
│              │  │   0    │ │  1   │ │               │
│              │  └────┬───┘ └──┬───┘ │               │
│              └───────┼────────┼─────┘               │
│                      │        │                     │
└──────────────────────┼────────┼─────────────────────┘
                       │        │
              ┌────────▼────────▼────────┐
              │    LPDDR4/LPDDR4x        │
              │    External DRAM         │
              │                          │
              │  Up to 8 GB              │
              │  Dual Channel            │
              │  Up to 1866 MHz          │
              │  (3733 MT/s effective)   │
              └──────────────────────────┘
```

---

## BIMC (Bus Interconnect Memory Controller)

The BIMC is not just a simple memory controller — it's a sophisticated interconnect that:

1. **Arbitrates** memory access from multiple masters (CPU, GPU, DSP, DMA, etc.)
2. **Provides QoS** (Quality of Service) — priority-based bandwidth allocation
3. **Manages power** — frequency scaling, self-refresh modes
4. **Handles coherency** — ensures cache coherency between CPU clusters

### BIMC Configuration

| Parameter | Value |
|-----------|-------|
| Channels | 2 (Dual Channel) |
| Data Width | 32-bit per channel (64-bit total) |
| Max DDR Type | LPDDR4x |
| Max Frequency | 1866 MHz (per channel) |
| Peak Bandwidth | ~29.9 GB/s |
| Address | 0x0040_0000 (register base) |

### Memory Map (DDR Regions)

```
Physical Address Map:
────────────────────
0x0000_0000 - 0x07FF_FFFF    Reserved / MMIO
0x0800_0000 - 0x0FFF_FFFF    Kernel text + data (typical)
0x1000_0000 - ...            General purpose DDR
    ...
0x8000_0000 - 0x83FF_FFFF    SMEM (Shared Memory)
0x8400_0000 - 0x85FF_FFFF    TZ App region (secure)
0x8600_0000 - 0x863F_FFFF    PIL region (firmware loading)
    ...
0x8700_0000 - 0xXXXX_XXXX    Linux managed memory
    ...
Top of DDR                     End of physical RAM
```

**Note**: Exact addresses vary by BSP configuration. The above is representative.

---

## DDR Initialization (During Boot)

DDR initialization is one of the most critical bring-up steps. Before DDR is initialized, the SoC can only use on-chip SRAM (IMEM/OCIMEM, ~256 KB).

### Boot Stages and Memory

| Boot Stage | Memory Available | Who Initializes |
|------------|-----------------|-----------------|
| PBL | On-chip ROM + IMEM (~256 KB) | N/A (hardwired) |
| XBL (early) | IMEM only | PBL loads XBL to IMEM |
| XBL (DDR init) | DDR becomes available | XBL initializes DDR controller |
| ABL onward | Full DDR | Already initialized |
| Kernel | Full DDR | Uses DDR, reads size from DT |

### DDR Training

DDR training is a calibration process that XBL performs to ensure reliable data transfer:

```
XBL DDR Init Sequence:
──────────────────────
1. Configure BIMC clock (from GCC)
2. Set DDR PHY timing parameters
3. Perform Write Leveling
   └── Aligns DQS strobe to clock at DRAM
4. Perform Read Leveling (Gate Training)
   └── Finds optimal DQS gate timing
5. Perform Read/Write DQ Training
   └── Per-bit delay calibration
6. Verify with memory test patterns
7. Store training results in partition (for faster subsequent boots)
```

---

## Shared Memory (SMEM)

A critical region of DDR is reserved for **SMEM (Shared Memory)** — used for inter-processor communication between the Application Processor, RPM, Modem, and DSPs.

```
SMEM Layout (typical):
──────────────────────
┌─────────────────────────────┐  0x8600_0000
│  SMEM Header                │  (Version, size, etc.)
├─────────────────────────────┤
│  SMEM Item Table            │  (ID → offset mapping)
├─────────────────────────────┤
│  Global Partition            │  (Shared by all processors)
│  - SMSM state bits          │
│  - Clock data               │
│  - Boot info                │
├─────────────────────────────┤
│  Apps ↔ RPM Partition       │  (Apps processor ↔ RPM)
├─────────────────────────────┤
│  Apps ↔ Modem Partition     │  (Apps ↔ Modem processor)
├─────────────────────────────┤
│  Apps ↔ ADSP Partition      │  (Apps ↔ Audio DSP)
├─────────────────────────────┤
│  Apps ↔ CDSP Partition      │  (Apps ↔ Compute DSP)
├─────────────────────────────┤
│  Apps ↔ SLPI Partition      │  (Apps ↔ Sensor Island)
└─────────────────────────────┘  0x8640_0000 (example end)
```

### Key SMEM Items

| SMEM Item ID | Name | Purpose |
|-------------|------|---------|
| 0 | `SMEM_PROC_COMM` | Legacy IPC |
| 2 | `SMEM_HEAP_INFO` | Heap metadata |
| 13 | `SMEM_POWER_ON_STATUS` | Power-on reason |
| 134 | `SMEM_ERR_CRASH_LOG` | Crash log from subsystems |
| 453 | `SMEM_CHANNEL_ALLOC_TBL` | SMD channel table |

---

## Memory Regions in Device Tree

```dts
/ {
    #address-cells = <2>;
    #size-cells = <2>;

    memory {
        device_type = "memory";
        reg = <0x0 0x80000000 0x0 0x80000000>;  /* 2 GB at 0x80000000 */
    };

    reserved-memory {
        #address-cells = <2>;
        #size-cells = <2>;
        ranges;

        /* TrustZone reserved region */
        tz_mem: tz@86200000 {
            reg = <0x0 0x86200000 0x0 0x2600000>;
            no-map;
        };

        /* SMEM region */
        smem_region: smem@86000000 {
            reg = <0x0 0x86000000 0x0 0x200000>;
            no-map;
        };

        /* PIL regions for firmware loading */
        pil_modem_mem: modem_region@8ac00000 {
            reg = <0x0 0x8ac00000 0x0 0x7e00000>;
            no-map;
        };

        pil_adsp_mem: adsp_region@92a00000 {
            reg = <0x0 0x92a00000 0x0 0x1e00000>;
            no-map;
        };

        /* GPU memory */
        gpu_mem: gpu_region@8f200000 {
            compatible = "shared-dma-pool";
            reg = <0x0 0x8f200000 0x0 0x800000>;
        };

        /* Continuous DMA pool */
        linux,cma {
            compatible = "shared-dma-pool";
            alloc-ranges = <0x0 0x00000000 0x0 0xffffffff>;
            reusable;
            alignment = <0x0 0x400000>;
            size = <0x0 0x20000000>;  /* 512 MB CMA */
            linux,cma-default;
        };
    };
};
```

---

## Memory Power Management

### Self-Refresh Modes

During system sleep, DDR enters self-refresh to retain data while consuming minimal power:

| Mode | Power | Data Retained | Wake Latency |
|------|-------|---------------|--------------|
| Active | Full | Yes | 0 |
| Auto Self-Refresh | Low | Yes | ~7 μs |
| Deep Self-Refresh | Very Low | Yes | ~15 μs |
| Power Down | Zero | No | Full retrain |

### BIMC Frequency Scaling

The BIMC clock is scaled based on bandwidth demand:

```
Frequency levels (typical):
  200 MHz  →  Low traffic (idle, light background)
  547 MHz  →  Medium traffic (normal usage)
  768 MHz  →  High traffic (camera, video)
  1017 MHz →  Peak traffic (gaming, benchmarks)
  1866 MHz →  Maximum (burst only)
```

---

## Related Documents

- [04_Interconnect_NoC.md](04_Interconnect_NoC.md) — NoC connects masters to BIMC
- [../02_XBL_Secondary_Bootloader/02_DDR_Init_Training.md](../02_XBL_Secondary_Bootloader/02_DDR_Init_Training.md) — DDR initialization details
- [../08_IPC_Mechanisms/01_SMEM_Shared_Memory.md](../08_IPC_Mechanisms/01_SMEM_Shared_Memory.md) — SMEM deep-dive
