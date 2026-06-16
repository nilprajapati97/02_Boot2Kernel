# Interconnect — Network-on-Chip (NoC)

## Overview

The SDM660 uses a **Network-on-Chip (NoC)** architecture to connect all internal subsystems. Unlike a simple shared bus, the NoC is a packet-switched fabric that allows multiple simultaneous data transfers with QoS (Quality of Service) guarantees.

---

## NoC Topology

```
┌────────────────────────────────────────────────────────────────┐
│                     SDM660 NoC Architecture                     │
│                                                                │
│  ┌──────┐  ┌──────┐  ┌──────┐  ┌──────┐  ┌──────┐            │
│  │ CPU  │  │ GPU  │  │ DSP  │  │Modem │  │Camera│            │
│  │Cores │  │Adreno│  │ADSP/ │  │ MDM  │  │ ISP  │            │
│  │      │  │ 509  │  │CDSP  │  │      │  │      │            │
│  └──┬───┘  └──┬───┘  └──┬───┘  └──┬───┘  └──┬───┘            │
│     │         │         │         │         │                  │
│  ┌──▼─────────▼─────────▼─────────▼─────────▼──────────────┐  │
│  │                        GNOC                              │  │
│  │              (Global Network-on-Chip)                     │  │
│  │         Top-level arbitration & routing                   │  │
│  └──┬──────────┬──────────┬──────────┬──────────┬──────────┘  │
│     │          │          │          │          │              │
│  ┌──▼───┐  ┌──▼───┐  ┌──▼───┐  ┌──▼───┐  ┌──▼───┐          │
│  │ SNOC │  │ CNOC │  │A2NOC │  │ MNOC │  │ BIMC │          │
│  │System│  │Config│  │Apps  │  │Media │  │Memory│          │
│  │ NoC  │  │ NoC  │  │to NoC│  │ NoC  │  │ NoC  │          │
│  └──┬───┘  └──┬───┘  └──┬───┘  └──┬───┘  └──┬───┘          │
│     │         │         │         │         │                │
│     ▼         ▼         ▼         ▼         ▼                │
│  System    Config     Periph   Display    DDR               │
│  buses     regs       I/O      Video     Memory             │
│  (USB,     (clk,      (I2C,    (DSI,     (LPDDR4)           │
│   UFS)     gpio)      SPI)     Venus)                       │
└────────────────────────────────────────────────────────────────┘
```

---

## NoC Components

### GNOC (Global NoC)

| Property | Value |
|----------|-------|
| Purpose | Top-level interconnect, connects all NoCs |
| Masters | CPU clusters, GPU, DSPs |
| Slaves | All other NoCs |
| QoS | Highest-level arbitration |

### SNOC (System NoC)

| Property | Value |
|----------|-------|
| Purpose | System-level data traffic |
| Masters | CPU, DMA engines |
| Slaves | USB, UFS/eMMC, crypto, IPA |
| Bandwidth | High-throughput data paths |

### CNOC (Config NoC)

| Property | Value |
|----------|-------|
| Purpose | Configuration and control register access |
| Masters | CPU (for MMIO register reads/writes) |
| Slaves | All peripheral config registers |
| Bandwidth | Low (config traffic only) |
| Note | Used when CPU writes to GCC, TLMM, QUP registers |

### A2NOC (Apps-to-NoC)

| Property | Value |
|----------|-------|
| Purpose | Connects application processor peripherals |
| Masters | BLSP (I2C/SPI/UART), TSIF |
| Slaves | System memory via BIMC |
| Note | Carries peripheral DMA traffic to/from DDR |

### MNOC (Multimedia NoC)

| Property | Value |
|----------|-------|
| Purpose | Multimedia engine interconnect |
| Masters | Display (MDP/DPU), Camera ISP, Video (Venus) |
| Slaves | DDR via BIMC |
| Bandwidth | Very high (display/camera frame data) |

### BIMC (Bus Interconnect Memory Controller)

| Property | Value |
|----------|-------|
| Purpose | Memory controller + interconnect to DDR |
| Masters | All NoCs connect here |
| Slaves | External DDR (LPDDR4) |
| Bandwidth | Peak ~29.9 GB/s |

---

## Bandwidth Voting (ICC Framework)

The Linux kernel uses the **Interconnect (ICC) framework** to request bandwidth on NoC paths. Drivers "vote" for the bandwidth they need, and the framework aggregates votes to set NoC clock frequencies.

### How Bandwidth Voting Works

```
Driver (e.g., Display)
    │
    ├── icc_set_bw(path, avg_bw=3000000, peak_bw=6000000)
    │   "I need 3 GB/s average, 6 GB/s peak"
    │
    ▼
ICC Framework
    │
    ├── Aggregates all votes on this NoC path
    ├── Finds minimum clock frequency to satisfy all votes
    │
    ▼
NoC Clock (via RPM)
    │
    └── Set BIMC clock to 768 MHz (satisfies bandwidth need)
```

### Device Tree Example

```dts
&mdss_mdp {
    /* Display needs high bandwidth to BIMC */
    interconnects = <&mdp_0_mem &bimc_0_mem>;
    interconnect-names = "mdp0-mem";
};

&ufs_ice {
    /* Storage needs moderate bandwidth */
    interconnects = <&a2noc_0_snoc &bimc_0_mem>;
    interconnect-names = "ufs-mem";
};
```

### Kernel ICC API

```c
#include <linux/interconnect.h>

/* Get an interconnect path */
struct icc_path *path = icc_get(dev, src_id, dst_id);

/* Set bandwidth vote (in KBps) */
icc_set_bw(path, avg_bw, peak_bw);

/* Remove bandwidth vote */
icc_set_bw(path, 0, 0);

/* Release path */
icc_put(path);
```

---

## NoC QoS (Quality of Service)

Each NoC master has a QoS configuration that determines its priority:

| Priority Level | Typical Masters | Behavior |
|---------------|-----------------|----------|
| Fixed High | Real-time display, camera | Always gets bandwidth |
| Regulator | CPU, GPU | Bandwidth regulated by DCVS |
| Best Effort | DMA, crypto | Gets remaining bandwidth |
| Low Latency | ADSP (audio) | Low-latency path |

### QoS in Device Tree

```dts
&snoc {
    qcom,bus-dev = <&fab_snoc>;
    qcom,bus-type = <1>;

    mas_snoc_cfg: mas-snoc-cfg {
        cell-id = <1>;
        label = "mas-snoc-cfg";
        qcom,buswidth = <16>;
        qcom,qos-mode = "bypass";      /* No QoS regulation */
    };

    mas_mdp: mas-mdp {
        cell-id = <22>;
        label = "mas-mdp";
        qcom,buswidth = <32>;
        qcom,qos-mode = "fixed";       /* Fixed high priority */
        qcom,prio-lvl = <0>;           /* Highest priority */
        qcom,prio-rd = <0>;
    };
};
```

---

## NoC During Boot

| Boot Phase | NoC State |
|------------|-----------|
| PBL | Minimal NoC config (ROM-based, fixed paths) |
| XBL | CNOC and BIMC initialized for DDR access |
| ABL | SNOC added for storage access (read kernel) |
| Kernel | Full NoC initialization via DT-driven ICC framework |
| Android | Dynamic bandwidth voting by all drivers |

---

## Debugging NoC Issues

```bash
# View interconnect nodes
ls /sys/kernel/debug/interconnect/

# Check bandwidth votes
cat /sys/kernel/debug/interconnect/qcom,bimc/node_list

# View NoC errors (if NoC error interrupt fires)
adb shell dmesg | grep -i "noc\|bimc\|snoc"
```

Common NoC issues during bring-up:
- **NoC error interrupt**: A master accessed an unmapped or disabled slave → check clock gating
- **Bandwidth starvation**: Display jank due to insufficient BIMC bandwidth → increase vote
- **Hang on register access**: CNOC path not enabled for a peripheral → enable AHB clock

---

## Related Documents

- [03_Memory_Subsystem.md](03_Memory_Subsystem.md) — BIMC and DDR details
- [01_SoC_Block_Diagram.md](01_SoC_Block_Diagram.md) — Overall SoC layout
- [../05_Linux_Kernel_Boot/03_GCC_Clock_Framework.md](../05_Linux_Kernel_Boot/03_GCC_Clock_Framework.md) — NoC clocks
