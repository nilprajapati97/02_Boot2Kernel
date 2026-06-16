# GIC Architecture — Generic Interrupt Controller

## 1. What is the GIC?

The GIC is ARM's standard interrupt controller. It collects interrupts from
all devices, prioritizes them, and delivers them to the appropriate CPU core.

### GIC Versions

```
┌──────────┬──────────────────────────────────────────────────────┐
│ Version  │ Features                                              │
├──────────┼──────────────────────────────────────────────────────┤
│ GICv1    │ Basic interrupt distribution (deprecated)            │
│ GICv2    │ Up to 8 cores, memory-mapped CPU interface           │
│          │ Used in: Cortex-A15, A7, older SoCs                  │
│ GICv3    │ Affinity routing, system register CPU interface,     │
│          │ LPI support, up to 2^32 interrupt IDs                │
│          │ Used in: Cortex-A53+, all modern ARMv8               │
│ GICv4    │ Direct virtual interrupt injection (for VMs)         │
│          │ GICv4.1: improved virtual SGI support                │
│          │ Used in: Cortex-A78+, server platforms               │
└──────────┴──────────────────────────────────────────────────────┘
```

---

## 2. GICv3 Architecture Components

```
┌──────────────────────────────────────────────────────────────────────┐
│                         GICv3 Architecture                           │
│                                                                       │
│  ┌──────────────────────────────────────────────────────┐            │
│  │              Distributor (GICD)                       │            │
│  │  • One per system (global)                            │            │
│  │  • Receives all SPIs (Shared Peripheral Interrupts)  │            │
│  │  • Priority, enable/disable, routing for SPIs         │            │
│  │  • Affinity routing to target specific cores          │            │
│  │  • Memory-mapped registers (GICD_*)                   │            │
│  └──────────────────┬───────────────────────────────────┘            │
│                      │                                                │
│          ┌───────────┼───────────┐                                    │
│          │           │           │                                    │
│  ┌───────▼─────┐ ┌──▼────────┐ ┌▼───────────┐                       │
│  │Redistributor│ │Redistributor│ │Redistributor│                      │
│  │  (GICR)     │ │  (GICR)    │ │  (GICR)     │                      │
│  │  Core 0     │ │  Core 1    │ │  Core N     │                      │
│  │             │ │            │ │             │                      │
│  │ • One per   │ │            │ │             │                      │
│  │   core      │ │            │ │             │                      │
│  │ • Manages   │ │            │ │             │                      │
│  │   SGIs+PPIs │ │            │ │             │                      │
│  │ • LPI       │ │            │ │             │                      │
│  │   pending   │ │            │ │             │                      │
│  │   table     │ │            │ │             │                      │
│  └──────┬──────┘ └─────┬─────┘ └──────┬──────┘                      │
│         │              │              │                               │
│  ┌──────▼──────┐ ┌─────▼─────┐ ┌─────▼──────┐                      │
│  │CPU Interface│ │CPU Interf.│ │CPU Interf. │                      │
│  │  (ICC_*)    │ │ (ICC_*)   │ │ (ICC_*)    │                      │
│  │             │ │           │ │            │                      │
│  │ • System    │ │           │ │            │                      │
│  │   registers│ │           │ │            │                      │
│  │ • Ack, EOI │ │           │ │            │                      │
│  │ • Priority │ │           │ │            │                      │
│  │   masking  │ │           │ │            │                      │
│  └──────┬──────┘ └─────┬─────┘ └─────┬──────┘                      │
│         │              │              │                               │
│     IRQ/FIQ        IRQ/FIQ        IRQ/FIQ                            │
│         │              │              │                               │
│  ┌──────▼──────┐ ┌─────▼─────┐ ┌─────▼──────┐                      │
│  │   Core 0    │ │  Core 1   │ │  Core N    │                      │
│  └─────────────┘ └───────────┘ └────────────┘                      │
│                                                                       │
│  Optional:                                                           │
│  ┌──────────────────────────────┐                                    │
│  │  ITS (Interrupt Translation  │  MSI/MSI-X → LPI translation     │
│  │  Service) — for PCIe/LPIs   │                                    │
│  └──────────────────────────────┘                                    │
└──────────────────────────────────────────────────────────────────────┘
```

---

## 3. Interrupt Types

```
┌──────────────────────────────────────────────────────────────────┐
│  Type │ ID Range    │ Description                                │
├───────┼─────────────┼────────────────────────────────────────────┤
│  SGI  │ 0-15        │ Software Generated Interrupt               │
│       │             │ • Core-to-core communication (IPI)         │
│       │             │ • Triggered by writing ICC_SGI1R_EL1      │
│       │             │ • Per-CPU (each core has its own 0-15)    │
│       │             │ • Used for: TLB shootdown, reschedule IPI│
│                                                                   │
│  PPI  │ 16-31       │ Private Peripheral Interrupt               │
│       │             │ • Per-core private (timer, PMU, etc.)     │
│       │             │ • Each core has its own copy               │
│       │             │ • Common PPIs:                             │
│       │             │   - 25: Virtual timer (EL1)                │
│       │             │   - 27: Virtual timer (hypervisor)         │
│       │             │   - 29: Secure physical timer              │
│       │             │   - 30: Non-secure physical timer          │
│       │             │   - 23: PMU overflow                       │
│                                                                   │
│  SPI  │ 32-1019     │ Shared Peripheral Interrupt                │
│       │             │ • From external devices (UART, eth, etc.) │
│       │             │ • Can be routed to any core                │
│       │             │ • Managed by Distributor (GICD)            │
│       │             │ • Most device interrupts are SPIs          │
│                                                                   │
│  LPI  │ 8192+       │ Locality-specific Peripheral Interrupt     │
│       │             │ • Message-based (MSI/MSI-X)               │
│       │             │ • Used for PCIe devices                    │
│       │             │ • Managed by ITS + Redistributor           │
│       │             │ • Up to millions of interrupt IDs          │
│       │             │ • Configuration in memory (tables)         │
└──────────────────────────────────────────────────────────────────┘
```

---

## 4. Interrupt Lifecycle

```
Step-by-step interrupt flow:

  1. GENERATE  — Device asserts interrupt signal
                 (or software writes SGI register)

  2. DISTRIBUTE — GIC Distributor determines:
                  • Is this interrupt enabled?
                  • What is its priority?
                  • Which core(s) should receive it?
                  • Route based on affinity (GICv3)

  3. DELIVER   — GIC signals IRQ or FIQ to target core
                  • Core's CPU Interface checks priority mask
                  • If priority > current running priority → signal CPU

  4. ACTIVATE  — CPU takes the interrupt
                  • Reads ICC_IAR1_EL1 (Acknowledge)
                  • Returns INTID (interrupt ID)
                  • Interrupt state: Pending → Active

  5. HANDLE    — Software interrupt handler runs
                  • Identifies device from INTID
                  • Calls device-specific handler
                  • Clears device interrupt source

  6. COMPLETE  — Writes ICC_EOIR1_EL1 (End of Interrupt)
                  • Interrupt state: Active → Inactive
                  • Priority drop (allows lower-priority interrupts)
                  • GIC ready for next interrupt

  State diagram:
  ┌──────────┐  Assert   ┌──────────┐  Ack(IAR)  ┌──────────┐
  │ Inactive │ ────────▶ │ Pending  │ ──────────▶ │  Active  │
  └──────────┘           └──────────┘             └────┬─────┘
       ▲                                                │
       │              EOI (EOIR)                        │
       └────────────────────────────────────────────────┘
  
  Also possible:
  Pending+Active: New interrupt of same INTID arrives while handling
```

---

## 5. GIC Registers

### Distributor Registers (GICD_*)

```
┌──────────────────┬─────────────────────────────────────────────┐
│  Register        │ Description                                  │
├──────────────────┼─────────────────────────────────────────────┤
│  GICD_CTLR       │ Distributor Control (enable/disable)        │
│  GICD_TYPER      │ Interrupt Controller Type (# of SPIs, etc.) │
│  GICD_ISENABLER  │ Interrupt Set-Enable (per-SPI enable)       │
│  GICD_ICENABLER  │ Interrupt Clear-Enable (per-SPI disable)    │
│  GICD_ISPENDR    │ Interrupt Set-Pending                        │
│  GICD_ICPENDR    │ Interrupt Clear-Pending                      │
│  GICD_IPRIORITYR │ Interrupt Priority (8-bit per interrupt)    │
│  GICD_ITARGETSR  │ Target processor (GICv2, bitmask)           │
│  GICD_IROUTER    │ Affinity routing (GICv3, 64-bit)            │
│  GICD_ICFGR      │ Interrupt Configuration (edge/level)        │
│  GICD_IGRPMODR   │ Interrupt Group Modifier                     │
└──────────────────┴─────────────────────────────────────────────┘
```

### Redistributor Registers (GICR_*)

```
┌──────────────────┬─────────────────────────────────────────────┐
│  Register        │ Description                                  │
├──────────────────┼─────────────────────────────────────────────┤
│  GICR_CTLR       │ Redistributor Control                       │
│  GICR_WAKER      │ Wake management (power control)             │
│  GICR_ISENABLER0 │ SGI/PPI enable                               │
│  GICR_IPRIORITYR │ SGI/PPI priorities                           │
│  GICR_ICFGR      │ SGI/PPI configuration                       │
│  GICR_PROPBASER  │ LPI property table base address             │
│  GICR_PENDBASER  │ LPI pending table base address              │
└──────────────────┴─────────────────────────────────────────────┘
```

### CPU Interface (System Registers — ICC_*)

```
┌──────────────────────┬───────────────────────────────────────────┐
│  Register            │ Description                                │
├──────────────────────┼───────────────────────────────────────────┤
│  ICC_SRE_EL1         │ System Register Enable                    │
│  ICC_PMR_EL1         │ Priority Mask (mask interrupts below this)│
│  ICC_BPR1_EL1        │ Binary Point (preemption grouping)        │
│  ICC_IAR1_EL1        │ Interrupt Acknowledge (read = ack + INTID)│
│  ICC_EOIR1_EL1       │ End of Interrupt (write = EOI)            │
│  ICC_HPPIR1_EL1      │ Highest Priority Pending Interrupt        │
│  ICC_RPR_EL1         │ Running Priority                          │
│  ICC_SGI1R_EL1       │ SGI Generation (write to send SGI)       │
│  ICC_CTLR_EL1        │ CPU Interface Control                     │
│  ICC_DIR_EL1         │ Deactivate Interrupt                      │
│  ICC_IGRPEN1_EL1     │ Interrupt Group 1 Enable                  │
└──────────────────────┴───────────────────────────────────────────┘
```

---

## 6. Interrupt Priority

```
Priority scheme:
  • 8-bit priority value (0-255)
  • LOWER number = HIGHER priority
  • 0 = highest priority, 255 = lowest
  • Implementations may support fewer bits (e.g., 5 bits = 32 levels)

Priority masking:
  ICC_PMR_EL1 = 0x80
  → Only interrupts with priority < 0x80 (higher) can preempt

Preemption:
  ICC_BPR1_EL1 controls preemption granularity
  Binary Point splits priority into group and subgroup:
  
  BPR=3: [7:3] = group priority, [2:0] = subgroup
  → Interrupt must have higher GROUP priority to preempt
  → Same group = queued, not preempted
  
  Example:
    Current running: priority 0x40 (group 0x40 >> 3 = 8)
    New pending:     priority 0x30 (group 0x30 >> 3 = 6)
    → 6 < 8 → preempts! (higher priority = lower number)
```

---

## 7. Interrupt Routing (GICv3 Affinity Routing)

```
GICv3 routes SPIs using MPIDR-based affinity:

  GICD_IROUTER[n]:
  ┌───────────────────────────────────────────────────────────────┐
  │  Bit 31     │ Aff2 [23:16] │ Aff1 [15:8] │ Aff0 [7:0]     │
  │  IRM        │ Cluster      │ Sub-cluster  │ Core ID         │
  │  (1=any)    │              │              │                  │
  └───────────────────────────────────────────────────────────────┘

  IRM = 0: Route to specific core (Aff2.Aff1.Aff0)
  IRM = 1: Route to any participating core (1-of-N)

  Example: Route SPI 42 to Core 3 of Cluster 0:
    GICD_IROUTER[42] = 0x00_00_00_03   // Aff2=0, Aff1=0, Aff0=3

  Example: Route SPI 42 to any available core:
    GICD_IROUTER[42] = 0x80_00_00_00   // IRM=1
```

---

## 8. Interrupt Groups & Security

```
GICv3 supports three interrupt groups:

┌─────────────────────────────────────────────────────────────────┐
│  Group          │ Signaled as │ Handled at │ Use                │
├─────────────────┼─────────────┼────────────┼────────────────────┤
│  Group 0        │ FIQ         │ EL3        │ Secure firmware    │
│  Secure Group 1 │ FIQ (at NS) │ S-EL1      │ Secure OS (TEE)   │
│                 │ IRQ (at S)  │            │                    │
│  NS Group 1     │ IRQ         │ EL1/EL2   │ Normal OS (Linux)  │
└─────────────────┴─────────────┴────────────┴────────────────────┘

Configuration bits per interrupt:
  GICD_IGROUPR[n]     → Group (0 or 1)
  GICD_IGRPMODR[n]    → Group modifier
  
  Group=0, Mod=0  → Group 0 (Secure, FIQ)
  Group=1, Mod=0  → Non-Secure Group 1 (Normal, IRQ)
  Group=0, Mod=1  → Secure Group 1

Typical Linux setup:
  All device SPIs: NS Group 1 → signaled as IRQ to EL1
  Timer PPIs: NS Group 1 → IRQ to EL1
  Secure interrupts: Group 0 → FIQ to EL3 (firmware)
```

---

Next: [Exception Handling →](./02_Exception_Handling.md)
