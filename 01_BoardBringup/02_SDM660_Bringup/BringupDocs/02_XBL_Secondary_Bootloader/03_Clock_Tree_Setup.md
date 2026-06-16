# Clock Tree Setup — GCC & PLL Configuration in XBL

## Overview

Before any peripheral can operate, its **clock** must be configured. During XBL, the clock tree is initialized from the crystal oscillator (XO) through PLLs to generate all the frequencies needed by the SoC subsystems. This is one of the earliest and most fundamental initialization steps.

---

## Clock Hierarchy

```
┌─────────────────────────────────────────────────────────────┐
│                    SDM660 Clock Tree                         │
│                                                             │
│  ┌──────────┐                                               │
│  │  XO      │  19.2 MHz Crystal Oscillator                  │
│  │ (TCXO)   │  Always-on reference clock                    │
│  └────┬─────┘                                               │
│       │                                                     │
│       ├───────────────────┐                                  │
│       │                   │                                  │
│  ┌────▼────┐         ┌────▼────┐                            │
│  │  GPLL0  │         │  GPLL1  │    ... more PLLs           │
│  │ 600 MHz │         │ 480 MHz │                            │
│  └────┬────┘         └────┬────┘                            │
│       │                   │                                  │
│  ┌────▼──────────────────▼───────────────────────────┐      │
│  │              GCC (Global Clock Controller)         │      │
│  │              @ 0x0010_0000                         │      │
│  │                                                    │      │
│  │  Dividers, Muxes, and Gates for:                   │      │
│  │  ├── CPU clocks (to APCC/CPR)                     │      │
│  │  ├── BIMC clock (DDR memory controller)           │      │
│  │  ├── System NoC clocks (SNOC, CNOC, A2NOC)        │      │
│  │  ├── Peripheral clocks:                           │      │
│  │  │   ├── GCC_BLSP1_QUP1_I2C_APPS_CLK  (I2C)     │      │
│  │  │   ├── GCC_BLSP1_UART1_APPS_CLK     (UART)    │      │
│  │  │   ├── GCC_SDCC1_APPS_CLK           (eMMC)    │      │
│  │  │   ├── GCC_UFS_AXI_CLK              (UFS)     │      │
│  │  │   └── GCC_USB3_MASTER_CLK          (USB)     │      │
│  │  ├── AHB clocks (bus access for config registers) │      │
│  │  └── Debug clocks (QDSS)                          │      │
│  └───────────────────────────────────────────────────┘      │
│       │               │               │                      │
│  ┌────▼────┐     ┌────▼────┐     ┌────▼────┐               │
│  │  GPUCC  │     │  MMCC   │     │  CPUCC  │               │
│  │ GPU     │     │ Display │     │ CPU     │               │
│  │ Clock   │     │ Camera  │     │ Clock   │               │
│  │ Ctrl    │     │ Video   │     │ Ctrl    │               │
│  └─────────┘     └─────────┘     └─────────┘               │
└─────────────────────────────────────────────────────────────┘
```

---

## PLL (Phase-Locked Loop) Basics

A PLL multiplies the 19.2 MHz crystal frequency to generate higher frequencies:

```
XO (19.2 MHz) → PLL → Output Frequency

PLL Output = XO × (L value) / (Post Divider)

Example: GPLL0
  XO = 19.2 MHz
  L = 31 (feedback divider)
  Post Div = 1
  Output = 19.2 × 31.25 = 600 MHz
```

### Key PLLs in SDM660

| PLL | Output Freq | Consumers |
|-----|------------|-----------|
| GPLL0 | 600 MHz | System NoCs, peripherals |
| GPLL1 | 480 MHz | USB, camera |
| GPLL4 | 384 MHz | Alternative source |
| APCS PLL | Variable | CPU cores (per cluster) |
| GPU PLL | Variable | Adreno 509 |
| BIMC PLL | Variable | DDR memory controller |
| DSI PLL | Variable | Display output clock |

---

## Clock Configuration in XBL

### What XBL Configures (Minimal Set)

XBL only brings up clocks needed for the boot chain — the kernel configures the rest:

```
XBL Clock Init:
───────────────
1. Enable XO (19.2 MHz) — already running from PBL
2. Configure and lock GPLL0 (600 MHz)
   └── Source for most GCC branch clocks
3. Configure BIMC PLL for DDR frequency
   └── Start low (200 MHz), scale up during training
4. Enable GCC_BLSP1_UART_APPS_CLK
   └── For debug UART output
5. Enable GCC_SDCC/UFS clocks
   └── For storage access (read partitions)
6. Enable AHB clocks for config register access
   └── GCC_BLSP1_AHB_CLK, etc.
7. Configure basic NoC clocks (CNOC, SNOC)
   └── For register access and data movement
```

### What the Kernel Configures (Full Set)

```
Kernel Clock Init:
──────────────────
1. GCC driver probes (compatible = "qcom,gcc-sdm660")
2. Registers ALL GCC clocks with common clock framework
3. Each driver requests its clocks via DT and clk_get()
4. Framework enables/disables clocks on demand
5. Unused clocks disabled for power savings
```

---

## Clock Gating

A critical power optimization — clocks to unused peripherals are **gated** (turned off) to save power. The gate is a single bit in a GCC register.

```
GCC Register: GCC_BLSP1_QUP2_I2C_APPS_CBCR
  (Clock Branch Control Register)

  Bit 0 (CLK_ENABLE): 0 = gated (off), 1 = enabled
  Bit 31 (CLK_OFF): Read-only status, 1 = clock is off

I2C driver calls clk_prepare_enable():
  → Sets CLK_ENABLE = 1
  → Waits for CLK_OFF = 0 (clock is running)

I2C driver calls clk_disable_unprepare():
  → Sets CLK_ENABLE = 0
  → Clock is gated, saves power
```

---

## Clock Tree for Common Peripherals

### I2C Clock Path

```
XO (19.2 MHz)
  └── GPLL0 (600 MHz)
       └── GCC_BLSP1_QUP2_I2C_APPS_CLK_SRC (mux + divider)
            └── /6 divider → 100 MHz
                 └── GCC_BLSP1_QUP2_I2C_APPS_CLK (gate)
                      └── I2C controller gets 100 MHz
                           └── I2C SCL = 400 KHz (further divided in QUP)
```

### UART Clock Path

```
XO (19.2 MHz)
  └── GPLL0 (600 MHz)
       └── GCC_BLSP1_UART1_APPS_CLK_SRC
            └── Configured for 7.3728 MHz (baud rate generation)
                 └── GCC_BLSP1_UART1_APPS_CLK (gate)
                      └── UART controller
                           └── 115200 baud = 7372800 / 64
```

### CPU Clock Path

```
XO (19.2 MHz)
  └── APCS PLL (per cluster)
       └── Gold PLL: up to 2.2 GHz
       └── Silver PLL: up to 1.8 GHz
            └── CPU Core Clock (via CPUCC mux)
                 └── DVFS: voltage scales with frequency
```

---

## GCC Register Map

| Register | Offset | Purpose |
|----------|--------|---------|
| GCC_GPLL0_MODE | 0x21000 | GPLL0 PLL control |
| GCC_GPLL0_L_VAL | 0x21004 | GPLL0 L divider value |
| GCC_GPLL0_STATUS | 0x2101C | GPLL0 lock status |
| GCC_BLSP1_AHB_CBCR | 0x17004 | BLSP1 AHB bus clock gate |
| GCC_BLSP1_QUP2_I2C_APPS_CBCR | 0x1A008 | I2C QUP2 clock gate |
| GCC_BLSP1_UART1_APPS_CBCR | 0x1900C | UART1 clock gate |
| GCC_SDCC1_APPS_CBCR | 0x42004 | eMMC clock gate |
| GCC_USB30_MASTER_CBCR | 0x0F008 | USB3 master clock gate |

---

## Debugging Clock Issues

```bash
# View all registered clocks
adb shell cat /sys/kernel/debug/clk/clk_summary

# Check a specific clock
adb shell cat /sys/kernel/debug/clk/gcc_blsp1_qup2_i2c_apps_clk/clk_rate
adb shell cat /sys/kernel/debug/clk/gcc_blsp1_qup2_i2c_apps_clk/clk_enable_count

# View clock tree
adb shell cat /sys/kernel/debug/clk/clk_dump
```

### Common Clock Problems

| Problem | Symptom | Fix |
|---------|---------|-----|
| Peripheral not responding | MMIO reads return 0 or bus error | Enable AHB + peripheral clock |
| Wrong baud rate | UART garbled output | Check UART clock source frequency |
| I2C timeout | No ACK from slave | Verify I2C clock is enabled and at correct rate |
| NoC error on register access | Kernel panic / data abort | Enable CNOC clock for that peripheral |

---

## Related Documents

- [01_XBL_Overview.md](01_XBL_Overview.md) — XBL clock init context
- [02_DDR_Init_Training.md](02_DDR_Init_Training.md) — BIMC PLL for DDR
- [../05_Linux_Kernel_Boot/03_GCC_Clock_Framework.md](../05_Linux_Kernel_Boot/03_GCC_Clock_Framework.md) — Kernel clock driver
