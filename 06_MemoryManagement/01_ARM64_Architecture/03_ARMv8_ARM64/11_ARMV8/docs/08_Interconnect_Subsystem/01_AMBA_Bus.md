# AMBA Bus Overview

## 1. What Is AMBA?

**AMBA** (Advanced Microcontroller Bus Architecture) is ARM's open standard for on-chip interconnect. It defines how components communicate within an SoC.

```
AMBA is NOT a single bus — it's a FAMILY of protocols:

┌──────────────────────────────────────────────────────────────────┐
│ Protocol │ Version │ Use Case                   │ Bandwidth     │
├──────────┼─────────┼────────────────────────────┼───────────────┤
│ APB      │ AMBA 2+ │ Low-speed peripherals      │ ~100 MB/s    │
│          │         │ (UART, SPI, I2C, GPIO)     │              │
├──────────┼─────────┼────────────────────────────┼───────────────┤
│ AHB      │ AMBA 2  │ Medium-speed, single       │ ~1 GB/s     │
│          │         │ master at a time (DMA,     │              │
│          │         │ simple CPUs, flash ctrl)   │              │
├──────────┼─────────┼────────────────────────────┼───────────────┤
│ AXI      │ AMBA 3/4│ High-speed, multiple       │ ~10+ GB/s   │
│          │         │ outstanding transactions   │              │
│          │         │ (CPU, GPU, memory ctrl)    │              │
├──────────┼─────────┼────────────────────────────┼───────────────┤
│ ACE      │ AMBA 4  │ AXI + cache coherency      │ ~10+ GB/s   │
│          │         │ (multi-core CPUs)          │              │
├──────────┼─────────┼────────────────────────────┼───────────────┤
│ CHI      │ AMBA 5  │ Scalable mesh coherency    │ ~100+ GB/s  │
│          │         │ (server-class SoCs)        │              │
├──────────┼─────────┼────────────────────────────┼───────────────┤
│ AXI-S    │ AMBA 4  │ Streaming data (no addr)   │ Variable    │
│ (Stream) │         │ (video pipeline, DSP)      │              │
├──────────┼─────────┼────────────────────────────┼───────────────┤
│ ATB      │ AMBA 3  │ Trace data transport       │ Variable    │
│          │         │ (CoreSight trace fabric)   │              │
└──────────┴─────────┴────────────────────────────┴───────────────┘
```

---

## 2. APB — Advanced Peripheral Bus

```
APB is the simplest AMBA protocol for slow peripherals:

Key characteristics:
  • Single master (bridge from AXI/AHB)
  • No burst support (one transfer per transaction)
  • No pipelining (2-cycle minimum per transfer)
  • Low power, low gate count
  • No outstanding transactions

Signals:
┌───────────────┬──────────────────────────────────────────┐
│ Signal        │ Description                               │
├───────────────┼──────────────────────────────────────────┤
│ PCLK          │ Clock                                     │
│ PRESETn       │ Reset (active low)                        │
│ PADDR[31:0]   │ Address                                   │
│ PSEL          │ Slave select (1=this slave selected)     │
│ PENABLE       │ Enable (2nd cycle of transfer)           │
│ PWRITE        │ Direction (1=write, 0=read)              │
│ PWDATA[31:0]  │ Write data                                │
│ PRDATA[31:0]  │ Read data                                 │
│ PREADY        │ Ready (slave can extend with wait states)│
│ PSLVERR       │ Slave error                               │
└───────────────┴──────────────────────────────────────────┘

APB Transfer timing:
  Clock:    ─┐  ┌──┐  ┌──┐  ┌──┐  ┌──
             └──┘  └──┘  └──┘  └──┘
  PSEL:     ─────┐                 ┌───
                  └─────────────────┘
  PENABLE:  ─────────┐        ┌────────
                      └────────┘
  PADDR:    ─────XXXX─ADDR─────XXXX────
  PREADY:   ─────────────┐  ┌─────────
                          (wait)

  Phase 1 (Setup):   PSEL=1, PENABLE=0, address/data driven
  Phase 2 (Access):  PSEL=1, PENABLE=1, slave responds
  If PREADY=0:       Insert wait states
  If PREADY=1:       Transfer completes
```

---

## 3. AHB — Advanced High-Performance Bus

```
AHB is a single-master pipelined bus:

Key characteristics:
  • Pipelined: address phase overlaps data phase of prev. transfer
  • Burst support (4, 8, 16 beat bursts + incrementing/wrapping)
  • Single master at a time (arbiter selects master)
  • Split/retry transactions possible
  • In ARMv8 SoCs: mainly used for legacy or simple subsystems

Signals (key):
┌────────────────┬──────────────────────────────────────────┐
│ Signal         │ Description                               │
├────────────────┼──────────────────────────────────────────┤
│ HCLK           │ Clock                                     │
│ HRESETn        │ Reset                                     │
│ HADDR[31:0]    │ Address                                   │
│ HTRANS[1:0]    │ Transfer type (IDLE/BUSY/NONSEQ/SEQ)    │
│ HSIZE[2:0]     │ Transfer size (byte/half/word/dword)     │
│ HBURST[2:0]    │ Burst type (SINGLE/INCR/WRAP4/INCR4...) │
│ HWRITE         │ Direction                                 │
│ HWDATA[31:0]   │ Write data                                │
│ HRDATA[31:0]   │ Read data                                 │
│ HREADY         │ Transfer done / insert wait              │
│ HRESP          │ Response (OKAY / ERROR)                  │
│ HGRANT         │ Arbiter grants bus to this master        │
│ HBUSREQ        │ Master requests bus                      │
└────────────────┴──────────────────────────────────────────┘

AHB Pipelining (key advantage over APB):
  Cycle:   1      2      3      4      5
  Addr:   [A1]   [A2]   [A3]   
  Data:          [D1]   [D2]   [D3]

  Address of transfer N overlaps with data of transfer N-1.
  Result: one transfer per clock (after initial latency).

Limitation: Only ONE master can use the bus at a time.
  → AXI replaces AHB with multiple outstanding transactions
```

---

## 4. How Protocols Connect in an SoC

```
Typical ARMv8 SoC Bus Architecture:

  ┌─────────────────────────────────────────────────────────────┐
  │                                                              │
  │   CPU Cluster                          GPU                   │
  │   ┌──────────────────┐                ┌──────┐             │
  │   │ Core0 Core1      │                │      │             │
  │   │ Core2 Core3      │                │      │             │
  │   │  (ACE/CHI)       │                │(AXI) │             │
  │   └────────┬─────────┘                └──┬───┘             │
  │            │                              │                  │
  │   ┌────────▼──────────────────────────────▼──────────┐      │
  │   │        Coherent Interconnect (CCI/CMN)            │      │
  │   │        (manages coherency, QoS, routing)          │      │
  │   └────┬──────────┬──────────┬──────────┬────────────┘      │
  │        │          │          │          │                     │
  │   ┌────▼───┐ ┌────▼───┐ ┌───▼────┐ ┌──▼──────────┐        │
  │   │DDR Ctrl│ │PCIe RC │ │USB Ctrl│ │NIC-400/GPV  │        │
  │   │ (AXI)  │ │ (AXI)  │ │ (AXI)  │ │(AXI→APB    │        │
  │   └────────┘ └────────┘ └────────┘ │ Bridge)     │        │
  │                                      └──┬──────────┘        │
  │                                         │ APB                │
  │                              ┌──────────┼──────────┐        │
  │                              │          │          │        │
  │                          ┌───▼──┐ ┌────▼──┐ ┌────▼──┐     │
  │                          │UART  │ │ SPI   │ │ I2C   │     │
  │                          │(APB) │ │ (APB) │ │ (APB) │     │
  │                          └──────┘ └───────┘ └───────┘     │
  └─────────────────────────────────────────────────────────────┘

Protocol selection guide:
  High-bandwidth, latency-sensitive → AXI or ACE/CHI
  Low-bandwidth, simple control     → APB
  Legacy or simple subsystems       → AHB
  Streaming data (no addresses)     → AXI-Stream
```

---

## 5. ARM Interconnect IP

```
ARM provides interconnect IP blocks:

┌──────────────┬───────────────────────────────────────────────────┐
│ IP           │ Description                                       │
├──────────────┼───────────────────────────────────────────────────┤
│ NIC-400      │ Non-coherent interconnect                         │
│              │ AXI switch fabric, QoS, AXI↔APB bridge           │
│              │ Used for: peripheral subsystems                   │
├──────────────┼───────────────────────────────────────────────────┤
│ CCI-400/500  │ Cache Coherent Interconnect                       │
│              │ 2-6 ACE ports + 3 AXI ports                      │
│              │ Snoop-based coherency, DVM messages               │
│              │ Used in: Cortex-A53/A57 big.LITTLE               │
├──────────────┼───────────────────────────────────────────────────┤
│ CCN-502/504  │ Cache Coherent Network                            │
│              │ Crossbar, 8-12 requester ports                    │
│              │ Snoop filter, directory-based coherency           │
│              │ Used in: server-class SoCs                        │
├──────────────┼───────────────────────────────────────────────────┤
│ CMN-600/700  │ Coherent Mesh Network                             │
│              │ Mesh topology, CHI protocol                       │
│              │ 32-256 nodes, scalable                            │
│              │ Used in: Neoverse N1/V1 server platforms          │
├──────────────┼───────────────────────────────────────────────────┤
│ DSU          │ DynamIQ Shared Unit                               │
│              │ Cluster-level coherency (within cluster)          │
│              │ Shared L3 cache, snoop filter                     │
│              │ Used in: Cortex-A76/A78/X1 clusters              │
│              │ DSU connects to CMN/CCI for inter-cluster         │
└──────────────┴───────────────────────────────────────────────────┘
```

---

Next: [AXI Protocol →](./02_AXI_Protocol.md) | Back to [Interconnect Overview](./README.md)
