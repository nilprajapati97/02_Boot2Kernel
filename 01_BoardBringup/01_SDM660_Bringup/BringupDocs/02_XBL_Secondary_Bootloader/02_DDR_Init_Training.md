# DDR Initialization & Training

## Overview

DDR initialization is one of the most critical steps in XBL — until DDR is available, the SoC has only ~256 KB of IMEM to work with. XBL configures the **BIMC (Bus Interconnect Memory Controller)** and the **DDR PHY**, then performs **training** to calibrate signal timing for reliable data transfer.

---

## DDR Init Sequence

```
XBL starts (running from IMEM)
    │
    ▼
┌──────────────────────────────────────────────────────────┐
│                  DDR INITIALIZATION                       │
│                                                          │
│  Step 1: Read DDR Parameters                             │
│  ├── From xbl_config partition (CDT blob)                │
│  ├── Parameters: DDR type, rank, speed grade, size       │
│  └── Board-specific: impedance, termination settings     │
│                                                          │
│  Step 2: Configure BIMC Controller                       │
│  ├── Set address mapping (rank, bank, row, column)       │
│  ├── Configure timing parameters (tCAS, tRAS, tRCD...)   │
│  ├── Set refresh rate (tREFI)                            │
│  └── Enable controller state machine                     │
│                                                          │
│  Step 3: Configure DDR PHY                               │
│  ├── Set PHY clock dividers                              │
│  ├── Configure I/O pad drivers (drive strength, ODT)     │
│  ├── Set DQ/DQS/CA pad impedance                        │
│  └── Enable PHY PLL (for DDR clock generation)           │
│                                                          │
│  Step 4: DDR Device Initialization                       │
│  ├── Send MRW (Mode Register Write) commands to DRAM     │
│  │   ├── MR1: Device feature set 1                       │
│  │   ├── MR2: Device feature set 2 (RL/WL)              │
│  │   ├── MR3: I/O config (impedance, data width)        │
│  │   └── MR11: ODT control                              │
│  └── DRAM chip is now in operational mode                │
│                                                          │
│  Step 5: DDR Training (Calibration)                      │
│  ├── CA Training (Command/Address)                       │
│  ├── Write Leveling                                      │
│  ├── Read Gate Training                                  │
│  ├── Read DQ Training (per-bit)                          │
│  ├── Write DQ Training (per-bit)                         │
│  └── VREF Training (voltage reference calibration)       │
│                                                          │
│  Step 6: Verify                                          │
│  ├── Write test patterns to DDR                          │
│  ├── Read back and compare                               │
│  └── If PASS → DDR is ready                             │
│                                                          │
│  Step 7: Store Training Results                          │
│  └── Save to flash for faster subsequent boots           │
└──────────────────────────────────────────────────────────┘
    │
    ▼
DDR is available — XBL relocates itself to DDR
```

---

## DDR Training Details

### Why Training is Needed

DDR4 operates at very high speeds (up to 1866 MHz clock = 3733 MT/s). At these speeds, signal propagation delays through PCB traces, package, and die vary due to:
- Manufacturing tolerances
- Temperature variations
- Voltage variations
- PCB trace length mismatches

Training **calibrates per-bit delays** to ensure every data bit arrives at the correct time.

### Training Steps

```
CA Training (Command/Address)
─────────────────────────────
Purpose: Align command/address signals to clock
Method:  Send known CA patterns, adjust CA delay until DRAM responds correctly

Write Leveling
──────────────
Purpose: Align DQS (data strobe) to clock at DRAM input
Method:  Toggle DQS, DRAM samples and feeds back timing → adjust DQS delay
Result:  Each byte lane has its own DQS delay

    Clock ─────┐     ┐─────┐     ┐─────
               │     │     │     │
    DQS0  ─────┘     └─────┘     └─────  (adjusted per byte lane)
                  ▲
                  │ delay
    DQS1  ──────┘     └─────┘     └────  (different delay)

Read Gate Training
──────────────────
Purpose: Find the optimal window to sample incoming DQS from DRAM
Method:  Read known data, sweep gate timing, find passing window center

Read/Write DQ Training (Per-bit deskew)
────────────────────────────────────────
Purpose: Calibrate individual data bit delays
Method:  For each of the 32 data bits:
         - Sweep delay from early to late
         - Find passing window (data matches expected)
         - Set delay to window center

    ◀─────── Sweep Range ──────────▶
    FAIL FAIL │ PASS PASS PASS │ FAIL FAIL
              │    ▲ center    │
              │    └── Set delay here

VREF Training
─────────────
Purpose: Calibrate voltage threshold for data sampling
Method:  Sweep VREF voltage, find optimal level for maximum margin
```

---

## DDR Configuration Parameters

### From CDT (Config Data Table) in `xbl_config`

| Parameter | Typical Value | Description |
|-----------|--------------|-------------|
| DDR Type | LPDDR4x | Memory technology |
| Channels | 2 | Dual channel |
| Ranks | 1 or 2 | Single or dual rank per channel |
| Density | 4 Gbit or 8 Gbit | Per die |
| Total Size | 3 GB, 4 GB, 6 GB | Total DDR capacity |
| Speed Grade | 1866 MHz | Maximum clock frequency |
| tCAS (CL) | 14 | CAS latency (clock cycles) |
| tRCD | 15 | RAS to CAS delay |
| tRP | 15 | Row precharge time |
| tRAS | 34 | Row active time |
| tREFI | 3.9 μs | Refresh interval |

### LPDDR4 vs LPDDR4x

| Feature | LPDDR4 | LPDDR4x |
|---------|--------|---------|
| I/O Voltage | 1.1V | 0.6V |
| Core Voltage | 1.1V | 1.1V |
| Power Savings | Baseline | ~15-20% I/O power reduction |
| SDM660 Support | Yes | Yes (preferred) |

---

## DDR Training Results Storage

Training takes ~50-200 ms. To speed up subsequent boots, results are saved:

```
First Boot:
  XBL → Full DDR Training (50-200 ms) → Save results to flash partition

Subsequent Boots:
  XBL → Load saved training results → Quick verify → DDR ready (~5-10 ms)

If verify fails:
  XBL → Re-run full training → Save new results
```

---

## Debugging DDR Issues

### Common DDR Problems During Bring-Up

| Symptom | Possible Cause | Debug Approach |
|---------|---------------|----------------|
| XBL hangs during DDR init | Wrong DDR params in CDT | Verify DDR chip P/N matches CDT config |
| Training fails | PCB signal integrity issue | Check trace impedance, length matching |
| Random data corruption | Marginal timing | Run stress tests, check VREF levels |
| Reduced DDR size detected | Rank detection failure | Check board strapping resistors |
| Boot loop | DDR not retaining data | Check VDD_DDR voltage (must be 1.35V stable) |

### XBL DDR Log Messages

```
[DDR] Init Start
[DDR] CDT: LPDDR4x, 2 Ranks, 4096 MB
[DDR] Channel 0: Samsung K3UH7H70AM
[DDR] Channel 1: Samsung K3UH7H70AM
[DDR] Training: CA Training - PASS
[DDR] Training: Write Leveling - PASS
[DDR] Training: Read Gate - PASS
[DDR] Training: Read DQ - PASS
[DDR] Training: Write DQ - PASS
[DDR] Training: VREF - PASS
[DDR] Freq: 1017 MHz → 1866 MHz
[DDR] Init Complete: 4096 MB, Dual Channel, Dual Rank
```

---

## DDR in Device Tree (Linux Kernel)

After XBL initializes DDR, the kernel learns about memory from the device tree:

```dts
/ {
    #address-cells = <2>;
    #size-cells = <2>;

    memory {
        device_type = "memory";
        /* XBL may update this based on detected DDR size */
        reg = <0x0 0x80000000 0x0 0x80000000>;  /* 2 GB */
              <0x0 0x100000000 0x0 0x80000000>;  /* +2 GB (if 4 GB total) */
    };
};
```

XBL updates the DT `memory` node before passing it to ABL/kernel, reflecting the actual detected DDR size.

---

## Related Documents

- [01_XBL_Overview.md](01_XBL_Overview.md) — XBL overall flow
- [03_Clock_Tree_Setup.md](03_Clock_Tree_Setup.md) — BIMC clock setup
- [../00_SDM660_Architecture/03_Memory_Subsystem.md](../00_SDM660_Architecture/03_Memory_Subsystem.md) — Memory architecture
