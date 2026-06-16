# Interrupt Types & Routing

## 1. SGI — Software Generated Interrupts (IPI)

SGIs are used for inter-processor interrupts (IPIs) — one core signals another.

```
SGI Generation (GICv3):
  Write to ICC_SGI1R_EL1 system register:
  
  ┌────────────────────────────────────────────────────────────┐
  │  ICC_SGI1R_EL1 Format:                                     │
  │  [55:48] Aff3    — Target cluster affinity level 3         │
  │  [47:44] RS      — Range Selector (for >16 targets)       │
  │  [40]    IRM     — 0=target list, 1=all except self        │
  │  [39:32] Aff2    — Target cluster affinity level 2         │
  │  [27:24] INTID   — SGI number (0-15)                       │
  │  [23:16] Aff1    — Target cluster affinity level 1         │
  │  [15:0]  TargetList — Bitmask of target cores in cluster   │
  └────────────────────────────────────────────────────────────┘

  Example: Send SGI 1 to Core 2 in Cluster 0
    MOV X0, #(1 << 24)         // INTID = 1
    ORR X0, X0, #(1 << 2)     // TargetList bit 2 = Core 2
    MSR ICC_SGI1R_EL1, X0      // Send SGI

  Example: Send SGI 0 to ALL other cores
    MOV X0, #(1 << 40)        // IRM = 1 (all except self)
    MSR ICC_SGI1R_EL1, X0

Common SGI uses in Linux:
  SGI 0: Reschedule IPI (wake up core to run scheduler)
  SGI 1: Call function on remote CPU
  SGI 2: Function call (single target)
  SGI 3: CPU stop
  SGI 5: Timer broadcast
  SGI 6: IRQ work
  SGI 7: Wakeup
```

---

## 2. PPI — Private Peripheral Interrupts

PPIs are per-core interrupts. Each core has its own set of PPIs.

```
Standard PPIs:
┌────────┬────────────────────────────────────────────────────┐
│ INTID  │ Description                                        │
├────────┼────────────────────────────────────────────────────┤
│  23    │ Virtual Maintenance Interrupt (for VM management)  │
│  25    │ Virtual Timer (EL1 virtual timer — CNTVCT)        │
│  26    │ Hypervisor Timer (EL2 physical timer)              │
│  27    │ Virtual Timer (EL2 virtual timer, ARMv8.1)        │
│  29    │ Secure Physical Timer (CNTPS)                      │
│  30    │ Non-Secure Physical Timer (CNTP — IRQ to EL1)     │
│  31    │ Reserved                                            │
│  16-22 │ Implementation-defined (PMU, debug, etc.)          │
└────────┴────────────────────────────────────────────────────┘

Timer PPI flow:
  1. ARM Generic Timer counts down to zero
  2. Timer asserts PPI to local core only
  3. GIC Redistributor forwards to CPU Interface
  4. Core takes IRQ exception
  5. Handler reads timer status, programs next deadline
  6. EOI sent to GIC
```

---

## 3. SPI — Shared Peripheral Interrupts

SPIs come from external devices and can be routed to any core.

```
SPI flow:
  Device → GIC Distributor → Route → Redistributor → CPU Interface → Core

  ┌──────────────────────────────────────────────────────────────┐
  │  Device          │ SPI ID  │ Trigger          │ Notes        │
  ├──────────────────┼─────────┼──────────────────┼──────────────┤
  │ UART0            │ 32      │ Level-high       │ Data avail.  │
  │ UART1            │ 33      │ Level-high       │              │
  │ SPI controller   │ 34      │ Level-high       │              │
  │ I2C controller   │ 35      │ Level-high       │              │
  │ Ethernet (GbE)   │ 40      │ Level-high       │              │
  │ USB controller   │ 45      │ Level-high       │              │
  │ GPIO bank 0      │ 50-81   │ Edge/Level config│              │
  │ DMA controller   │ 90-97   │ Edge-rising      │ DMA complete │
  │ PCIe INTx        │ 100-103 │ Level-high       │ Legacy PCI   │
  │ Watchdog timer   │ 110     │ Level-high       │ WDT timeout  │
  └──────────────────┴─────────┴──────────────────┴──────────────┘

  Note: Actual SPI assignments are SoC-specific (defined in device tree)
```

### SPI Trigger Configuration

```
GICD_ICFGR[n]: 2 bits per interrupt

  00 = Level-sensitive (active high)
       Interrupt stays asserted until device clears it
       GIC: remains pending as long as signal is high
       Use: most MMIO device interrupts

  10 = Edge-triggered (rising edge)
       Interrupt pulses briefly
       GIC: latches on rising edge
       Use: DMA completion, button press
```

---

## 4. LPI — Locality-Specific Peripheral Interrupts

LPIs support MSI/MSI-X (message-signaled interrupts) used by PCIe devices.

```
┌──────────────────────────────────────────────────────────────────┐
│                          LPI Flow                                 │
│                                                                    │
│  PCIe Device writes MSI message:                                  │
│  ┌──────────────┐                                                 │
│  │  PCIe Device  │─── Write to GITS_TRANSLATER register ──┐      │
│  └──────────────┘    (DeviceID + EventID)                  │      │
│                                                             │      │
│  ┌──────────────────────────────────────────────────────────▼─┐   │
│  │              ITS (Interrupt Translation Service)            │   │
│  │                                                              │   │
│  │  1. Use DeviceID to look up Device Table → ITT pointer      │   │
│  │  2. Use EventID to look up ITT → INTID + Collection        │   │
│  │  3. Use Collection Table → target Redistributor             │   │
│  │                                                              │   │
│  │  Translation:                                                │   │
│  │    (DeviceID, EventID) → (INTID, target core)               │   │
│  └──────────────────────────┬───────────────────────────────────┘   │
│                              │                                      │
│  ┌──────────────────────────▼────────────────────┐                 │
│  │  Redistributor                                  │                 │
│  │  • LPI Pending Table (in memory, 1 bit per LPI)│                 │
│  │  • LPI Config Table (priority, enable)          │                 │
│  │  • Sets pending bit → signals CPU Interface     │                 │
│  └──────────────────────────┬────────────────────┘                 │
│                              │                                      │
│                      IRQ to target core                             │
└──────────────────────────────────────────────────────────────────────┘

ITS Tables:
  Device Table: DeviceID → pointer to ITT
  ITT (Interrupt Translation Table): EventID → (INTID, CollectionID)
  Collection Table: CollectionID → target Redistributor (core)

LPI advantages:
  • Millions of possible interrupt IDs (vs ~1000 SPIs)
  • Tables in memory (not registers — more scalable)
  • PCIe MSI/MSI-X native support
  • Efficient for virtualization (GICv4 direct injection)
```

---

## 5. Affinity Routing (GICv3)

```
GICv3 uses MPIDR-based affinity for routing, not simple CPU bitmask:

  MPIDR_EL1: Aff3.Aff2.Aff1.Aff0

  GICD_IROUTER[n] specifies target for SPI n:
    Aff3.Aff2.Aff1.Aff0 = specific core
    IRM=1 = any core (1-of-N distribution)

  1-of-N routing distributes interrupts across cores:
    SPI 32 → any core → GIC picks least-busy/lowest-affinity
    Useful for: high-throughput network interrupts

  Example: Dual-cluster big.LITTLE topology
  ┌─────────────────────────────────────────────────────────┐
  │  Cluster 0 (LITTLE)     Cluster 1 (big)                │
  │  Aff1=0                 Aff1=1                          │
  │  ┌──────┐ ┌──────┐     ┌──────┐ ┌──────┐              │
  │  │Core 0│ │Core 1│     │Core 0│ │Core 1│              │
  │  │0.0.0 │ │0.0.1 │     │0.1.0 │ │0.1.1 │              │
  │  └──────┘ └──────┘     └──────┘ └──────┘              │
  │                                                         │
  │  Route SPI to big Core 1:                               │
  │    GICD_IROUTER[n] = {Aff2=0, Aff1=1, Aff0=1}        │
  └─────────────────────────────────────────────────────────┘
```

---

## 6. GICv4 — Direct Virtual Interrupt Injection

```
GICv4 allows virtual interrupts to be delivered directly to a VM
without trapping to the hypervisor:

Without GICv4:
  Device IRQ → GIC → EL2 trap → hypervisor → inject vIRQ → VM
  (expensive: 2 world switches per interrupt!)

With GICv4:
  Device IRQ → GIC (via ITS) → direct to vPE → VM sees interrupt
  (fast: no hypervisor trap needed!)

  vPE = Virtual Processing Element (a VCPU)
  
  ITS maps: (DeviceID, EventID) → (vINTID, vPE)
  GIC delivers vINTID directly to the guest
  
  GICv4.1 adds: Direct injection of virtual SGIs
```

---

## 7. Interrupt Latency

```
Interrupt latency = time from device assertion to handler execution

Components:
  ┌──────────────────────────────────────────────────────┐
  │  Component              │ Typical Cycles │ Notes     │
  ├─────────────────────────┼────────────────┼──────────┤
  │  GIC processing         │ 10-20          │ Route,   │
  │                          │                │ prioritize│
  │  CPU pipeline flush      │ 10-15          │ Flush    │
  │                          │                │ speculated│
  │  Vector fetch + decode   │ 5-10           │ I-cache  │
  │  Context save (SW)       │ 20-50          │ Push regs│
  │  IAR read (acknowledge)  │ 5-10           │ System   │
  │                          │                │ register │
  │  Handler code start      │ 0              │ Ready!   │
  ├─────────────────────────┼────────────────┼──────────┤
  │  TOTAL                   │ ~50-100 cycles │          │
  │  @ 2 GHz                 │ ~25-50 ns      │          │
  └──────────────────────────┴────────────────┴──────────┘

  Factors that increase latency:
  • IRQ masked (PSTATE.I=1) — delayed until unmasked
  • Cache miss on vector table — adds ~100 cycles
  • Higher-priority interrupt already running — must wait
  • WFI (Wait For Interrupt) — wake-up latency from sleep state
```

---

Next: Back to [Interrupt Subsystem Overview](./README.md) | Continue to [Security Subsystem →](../05_Security_Subsystem/)
