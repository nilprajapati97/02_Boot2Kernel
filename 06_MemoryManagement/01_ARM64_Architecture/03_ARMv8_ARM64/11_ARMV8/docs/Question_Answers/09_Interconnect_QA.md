# Interconnect Subsystem — Questions & Answers

---

## Q1. [L1] What is AMBA? Describe the AMBA bus protocol family.

**Answer:**

```
AMBA (Advanced Microcontroller Bus Architecture) is ARM's
on-chip interconnect standard. Multiple protocols for different
performance/complexity levels.

AMBA protocol family:
  ┌──────────────────────────────────────────────────────────┐
  │ Protocol │ Generation │ Use Case                        │
  ├──────────┼────────────┼─────────────────────────────────┤
  │ APB      │ AMBA 2     │ Low-bandwidth peripherals       │
  │          │            │ UART, SPI, I2C, GPIO, Timer     │
  │          │            │ Simple: single master, no pipeline│
  │          │            │ 2-cycle minimum transfer        │
  ├──────────┼────────────┼─────────────────────────────────┤
  │ AHB      │ AMBA 2/3   │ Medium-bandwidth                │
  │          │            │ DMA, flash controller           │
  │          │            │ Single-channel, burst support   │
  │          │            │ Pipelined address/data          │
  ├──────────┼────────────┼─────────────────────────────────┤
  │ AXI      │ AMBA 3/4   │ High-performance                │
  │          │            │ CPU, GPU, memory controller     │
  │          │            │ 5 channels, out-of-order        │
  │          │            │ AXI4: max 256-beat bursts       │
  ├──────────┼────────────┼─────────────────────────────────┤
  │ ACE      │ AMBA 4     │ Cache-coherent interconnect     │
  │          │            │ Multi-core CPU clusters         │
  │          │            │ Adds snoop channels to AXI      │
  ├──────────┼────────────┼─────────────────────────────────┤
  │ CHI      │ AMBA 5     │ Mesh/scalable coherent          │
  │          │            │ Server/HPC SoCs (100+ cores)    │
  │          │            │ Flit-based, directory protocol  │
  │          │            │ Replaces AXI/ACE for high-end   │
  └──────────┴────────────┴─────────────────────────────────┘

Typical SoC interconnect:
  ┌──────────────────────────────────────────────────────────┐
  │                                                          │
  │  CPU Cluster ──ACE/CHI──→ Interconnect (CCI/CMN)       │
  │  GPU         ──AXI──────→ Interconnect                  │
  │  DMA         ──AXI──────→ Interconnect                  │
  │                              │                           │
  │                    ┌─────────┴───────────┐               │
  │                    │                     │               │
  │              DDR Controller      Peripheral bus          │
  │              (AXI slave)          (AHB/APB bridge)       │
  │                                   │                      │
  │                             ┌─────┴──────────┐          │
  │                             │                │          │
  │                           UART (APB)    SPI (APB)      │
  │                                                          │
  └──────────────────────────────────────────────────────────┘
```

---

## Q2. [L2] Explain the AXI protocol channels and handshake mechanism.

**Answer:**

```
AXI (Advanced eXtensible Interface) uses 5 independent channels
with a VALID/READY handshake protocol.

Five AXI channels:
  ┌──────────────────────────────────────────────────────────┐
  │                     AXI Channels                         │
  │                                                          │
  │  Write transaction:                                      │
  │    Master ──AW──→ Slave    (Write Address channel)      │
  │    Master ──W───→ Slave    (Write Data channel)         │
  │    Slave  ──B───→ Master   (Write Response channel)     │
  │                                                          │
  │  Read transaction:                                       │
  │    Master ──AR──→ Slave    (Read Address channel)       │
  │    Slave  ──R───→ Master   (Read Data channel)          │
  └──────────────────────────────────────────────────────────┘

  AW (Write Address):
    AWADDR: target address (32/64-bit)
    AWLEN:  burst length (0-255 → 1-256 beats)
    AWSIZE: beat size (0-7 → 1-128 bytes)
    AWBURST: burst type (FIXED, INCR, WRAP)
    AWID:   transaction ID (for out-of-order)
    AWCACHE: cacheable, bufferable, allocate hints
    AWPROT: secure/non-secure, privileged, instruction/data
  
  W (Write Data):
    WDATA:  write data (data bus width: 32/64/128/256 bits)
    WSTRB:  byte strobe (which bytes are valid)
    WLAST:  last beat in burst
  
  B (Write Response):
    BRESP:  response (OKAY, EXOKAY, SLVERR, DECERR)
    BID:    matches AWID
  
  AR (Read Address):
    Same signals as AW (ARADDR, ARLEN, ARSIZE, etc.)
  
  R (Read Data):
    RDATA:  read data
    RRESP:  response
    RLAST:  last beat in burst
    RID:    matches ARID

Handshake (VALID/READY):
  ┌──────────────────────────────────────────────────────────┐
  │ Transfer occurs when VALID=1 AND READY=1                │
  │                                                          │
  │ CLK:     ─┐ ┌─┐ ┌─┐ ┌─┐ ┌─┐ ┌─┐ ┌─                  │
  │            └─┘ └─┘ └─┘ └─┘ └─┘ └─┘                     │
  │ VALID:   ─────┐         ┌───────────                    │
  │               └─────────┘                                │
  │ READY:   ──────────┐    ┌───────────                    │
  │                    └────┘                                │
  │                              ↑                           │
  │                        Transfer happens here             │
  │                        (VALID=1 AND READY=1)             │
  │                                                          │
  │ Rules:                                                   │
  │   • VALID must not depend on READY (no deadlock)        │
  │   • READY can depend on VALID (okay)                    │
  │   • Once VALID asserted, cannot be deasserted until     │
  │     READY is asserted (transfer completes)              │
  └──────────────────────────────────────────────────────────┘

Outstanding transactions (pipelining):
  AXI allows multiple outstanding transactions:
    Issue Read A (ARID=0) → don't wait for data
    Issue Read B (ARID=1) → don't wait for data  
    Issue Read C (ARID=2) → don't wait for data
    ← Data for B arrives (ARID=1) → out of order!
    ← Data for A arrives (ARID=0)
    ← Data for C arrives (ARID=2)
  
  → Hides latency, maximizes bandwidth utilization
  → Typical: 8-16 outstanding transactions supported
```

---

## Q3. [L3] What is ACE (AXI Coherency Extensions)? How does it handle snooping?

**Answer:**

```
ACE extends AXI with snoop channels for hardware cache
coherency between multiple CPU clusters.

ACE adds 3 snoop channels to AXI's 5:
  ┌──────────────────────────────────────────────────────────┐
  │                     ACE Channels                         │
  │                                                          │
  │  Standard AXI channels (5):                             │
  │    AR, R, AW, W, B                                      │
  │                                                          │
  │  Snoop channels (3):                                     │
  │    AC: Snoop Address (Interconnect → Master)            │
  │      ACADDR: address to snoop                           │
  │      ACSNOOP: snoop type (ReadOnce, ReadShared,         │
  │               ReadUnique, CleanInvalid, MakeInvalid)    │
  │                                                          │
  │    CR: Snoop Response (Master → Interconnect)           │
  │      CRRESP: response (DataTransfer, Error, PassDirty,  │
  │              IsShared, WasUnique)                        │
  │                                                          │
  │    CD: Snoop Data (Master → Interconnect)               │
  │      CDDATA: snooped data (dirty cache line)            │
  │      CDLAST: last beat of snoop data                    │
  └──────────────────────────────────────────────────────────┘

Coherent read flow:
  ┌──────────────────────────────────────────────────────────┐
  │ CPU A reads address X (not in its cache):               │
  │                                                          │
  │ 1. CPU A → AR (ReadShared, addr=X) → Interconnect      │
  │                                                          │
  │ 2. Interconnect checks: who has addr X?                 │
  │    (Snoop filter / directory lookup)                    │
  │                                                          │
  │ 3. If CPU B has X in Modified state:                    │
  │    Interconnect → AC (ReadShared, addr=X) → CPU B      │
  │                                                          │
  │ 4. CPU B snooped:                                       │
  │    • Changes state: Modified → Shared                   │
  │    • CPU B → CR (DataTransfer=1, PassDirty=1) → Intc   │
  │    • CPU B → CD (dirty data) → Interconnect            │
  │                                                          │
  │ 5. Interconnect:                                        │
  │    • Forwards data to CPU A via R channel               │
  │    • Writes dirty data back to memory                   │
  │                                                          │
  │ 6. CPU A gets data in Shared state                      │
  │                                                          │
  │ Total: ~50-100 cycles (snoop hit)                       │
  │ Vs DRAM: ~200+ cycles (if had to go to memory)         │
  └──────────────────────────────────────────────────────────┘

ACE-Lite:
  Simplified version for non-caching masters (GPU, DMA).
  Can issue coherent transactions but does NOT respond to snoops.
  → GPU can read cache-coherent data from CPU caches
  → But GPU itself is not snooped (no cache to snoop)

Snoop types:
  ReadOnce:      Read without caching (non-cacheable coherent)
  ReadShared:    Read into Shared state
  ReadUnique:    Read into Unique state (for write)
  CleanInvalid:  Clean and invalidate target cache line
  MakeInvalid:   Invalidate without writeback (ownership grab)
  CleanShared:   Clean to PoC, leave Shared
```

---

## Q4. [L2] What is CHI (Coherent Hub Interface)? How does it differ from ACE?

**Answer:**

```
CHI (AMBA 5) is a scalable, flit-based protocol for large SoCs
with many coherent agents (100+ cores).

ACE vs CHI:
  ┌──────────────────────────────────────────────────────────┐
  │ Feature      │ ACE (AMBA 4)       │ CHI (AMBA 5)        │
  ├──────────────┼────────────────────┼─────────────────────┤
  │ Topology     │ Crossbar           │ Mesh / Ring         │
  │ Scalability  │ 4-8 masters        │ 100+ nodes          │
  │ Protocol     │ Channel-based      │ Flit-based          │
  │ Coherency    │ Snoop broadcast    │ Directory / Snoop   │
  │              │                    │ filter              │
  │ Interconnect │ CCI-400/500        │ CMN-600/700         │
  │ Ordering     │ Channel ordering   │ Per-flit ordering   │
  │ Data width   │ Fixed (128/256-bit)│ Flit-based (flexible)│
  │ Use case     │ Mobile SoC (<8 CPU)│ Server/HPC (64+ CPU)│
  └──────────────┴────────────────────┴─────────────────────┘

CHI flit-based protocol:
  ┌──────────────────────────────────────────────────────────┐
  │ CHI messages are "flits" (flow control digits)          │
  │                                                          │
  │ Three flit channels:                                     │
  │   REQ: Request channel (read, write, snoop request)     │
  │   RSP: Response channel (completion, snoop response)    │
  │   DAT: Data channel (read data, write data, snoop data) │
  │   SNP: Snoop channel (snoop requests from HN to RN)    │
  │                                                          │
  │ Node types:                                              │
  │   RN (Request Node):                                    │
  │     RN-F: Fully coherent (CPU cluster)                  │
  │     RN-I: I/O coherent (GPU, DMA with ACE-Lite)        │
  │     RN-D: I/O non-coherent (basic DMA)                  │
  │                                                          │
  │   HN (Home Node):                                       │
  │     HN-F: Fully coherent home (manages coherency)      │
  │     HN-I: Non-coherent I/O home                        │
  │                                                          │
  │   SN (Slave Node):                                      │
  │     SN-F: Memory controller (DDR)                       │
  │     SN-I: Peripheral                                    │
  └──────────────────────────────────────────────────────────┘

CHI coherent read flow:
  1. RN-F (CPU) → REQ (ReadShared) → HN-F
  2. HN-F: check directory (who has this line?)
  3. If another RN-F has line:
     HN-F → SNP (SnpShared) → owning RN-F
  4. Owning RN-F → RSP (SnpResp) → HN-F
     Owning RN-F → DAT (CompData) → requesting RN-F
  5. OR: HN-F → REQ → SN-F (memory) → DAT back
  
  Directory avoids broadcast:
    ACE: snoop ALL cores → O(N) traffic
    CHI: look up directory → snoop only owner → O(1) traffic
    For 64+ cores: massive bandwidth savings!

CMN-700 (Coherent Mesh Network):
  ARM's CHI-based interconnect IP
  • Up to 256 RN-F ports (256 CPU clusters!)
  • Mesh topology: XP (crosspoints) connected in grid
  • Built-in snoop filter / directory
  • System cache (SLC) option at HN-F nodes
  • MPAM support for QoS partitioning
```

---

## Q5. [L2] How does the interconnect handle transaction ordering?

**Answer:**

```
ARM memory model requires specific ordering guarantees that
the interconnect must enforce.

AXI ordering rules:
  ┌──────────────────────────────────────────────────────────┐
  │ Same ID, same direction:                                 │
  │   Transactions with same ID MUST complete in order       │
  │   Read A (ID=0) before Read B (ID=0) → A data first    │
  │                                                          │
  │ Different IDs:                                           │
  │   No ordering requirement (can complete out-of-order)   │
  │   Read (ID=0) and Read (ID=1) → either can finish first│
  │                                                          │
  │ Read vs Write (same ID):                                 │
  │   AXI does NOT guarantee read-write ordering            │
  │   Software must use barriers (DMB, DSB)                 │
  │                                                          │
  │ Write ordering:                                          │
  │   Same ID writes to same slave → ordered                │
  │   Same ID writes to different slaves → may reorder!     │
  └──────────────────────────────────────────────────────────┘

Barrier handling in interconnect:
  DMB (Data Memory Barrier) → barriers visible on bus
    ┌──────────────────────────────────────────────────────┐
    │ CPU executes DMB:                                    │
    │   All prior memory accesses must be observable       │
    │   before any subsequent memory accesses.             │
    │                                                      │
    │ Interconnect sees barrier:                           │
    │   Ensures all prior transactions from this master    │
    │   have reached their destination before allowing     │
    │   subsequent transactions to proceed.                │
    │                                                      │
    │ Implementation:                                      │
    │   Drain write buffer                                 │
    │   Wait for outstanding responses                     │
    │   → Stalls subsequent transactions                   │
    └──────────────────────────────────────────────────────┘

  DSB (Data Synchronization Barrier):
    Stronger: CPU stalls until ALL prior memory accesses
    are COMPLETE (not just observable).
    
    Used before:
      ISB (to ensure instruction stream sees new mappings)
      WFI (to ensure all stores visible before sleeping)
      TLBI (to ensure old translations are flushed first)

Device memory ordering:
  Device-nGnRnE: No Gathering, No Reordering, No Early write ack
    → Strictest: every access exactly as programmed
    → Used for: most MMIO devices
  
  Device-nGnRE: allow Early write acknowledgment
    → Write can be acknowledged before reaching device
    → Slightly faster for write-heavy MMIO
  
  Device-nGRE: allow Gathering and Reording
    → Multiple narrow writes can merge into one wide write
    → Used for: framebuffer, write-combining regions
  
  Device-GRE: most relaxed device memory
    → Full gathering and reordering permitted
```

---

## Q6. [L2] What is CCI (Cache Coherent Interconnect)? Compare CCI-400/500 with CMN.

**Answer:**

```
CCI provides full hardware cache coherency between multiple
ACE masters (CPU clusters, GPU).

CCI-400:
  ┌──────────────────────────────────────────────────────────┐
  │  • 2 ACE master ports (2 CPU clusters)                  │
  │  • 3 ACE-Lite master ports (GPU, DMA)                   │
  │  • Snoop broadcast (all masters snooped)                │
  │  • Used in: big.LITTLE (Cortex-A53 + Cortex-A57)       │
  │  • Bandwidth: ~10 GB/s                                  │
  │                                                          │
  │  CPU Cluster 0 ──ACE──→ ┌─────────────┐                │
  │  (Cortex-A53)            │             │ ──→ DDR        │
  │                          │   CCI-400   │                 │
  │  CPU Cluster 1 ──ACE──→ │             │ ──→ Peripherals│
  │  (Cortex-A57)            │             │                 │
  │  GPU ──────ACE-Lite───→ └─────────────┘                │
  └──────────────────────────────────────────────────────────┘

CCI-500:
  • 4 ACE master ports (up to 4 CPU clusters)
  • 6 ACE-Lite master ports
  • Snoop filter (avoids broadcast → reduces traffic)
  • Used in: Cortex-A72/A73 based SoCs
  • Bandwidth: ~30 GB/s

CCI-550:
  • 6 ACE master ports
  • 6 ACE-Lite ports
  • Larger snoop filter
  • Higher bandwidth

CMN-600/700 (CHI-based, replacing CCI):
  ┌───────────────────────────────────────────────────────────┐
  │  • CHI protocol (not ACE)                                │
  │  • Mesh topology (scales to 256 masters)                 │
  │  • Directory-based coherency (no broadcast)              │
  │  • Optional system-level cache (SLC)                     │
  │  • MPAM QoS partitioning                                 │
  │  • Used in: Neoverse N2/V2, server platforms             │
  │  • Bandwidth: 100+ GB/s                                  │
  │                                                          │
  │  ┌────┐ ┌────┐ ┌────┐ ┌────┐                           │
  │  │XP00│─│XP01│─│XP02│─│XP03│  XP = crosspoint          │
  │  └─┬──┘ └─┬──┘ └─┬──┘ └─┬──┘  Each XP connects         │
  │    │       │       │       │    2 devices + neighbors    │
  │  ┌─┴──┐ ┌─┴──┐ ┌─┴──┐ ┌─┴──┐                          │
  │  │XP10│─│XP11│─│XP12│─│XP13│                           │
  │  └────┘ └────┘ └────┘ └────┘                            │
  │                                                          │
  │  CPU clusters, memory controllers, I/O agents           │
  │  connect to XP ports.                                   │
  └──────────────────────────────────────────────────────────┘

Evolution:
  CCI-400 (ACE, 2 clusters) → CCI-500 (4 clusters)
  → CMN-600 (CHI, mesh, 64+ cores) → CMN-700 (256+ cores)
```

---

## Q7. [L2] How does QoS (Quality of Service) work in ARM interconnects?

**Answer:**

```
QoS ensures critical traffic gets priority over less important
traffic in shared interconnects.

AXI QoS signals:
  AxQOS[3:0]: 4-bit QoS value on each transaction
    0x0 = lowest priority (background DMA)
    0xF = highest priority (CPU data, real-time)
  
  Interconnect uses QoS to:
    • Prioritize in arbitration (higher QoS wins)
    • Allocate bandwidth proportionally
    • Manage buffer allocation

MPAM (Memory System Performance Resource Partitioning and Monitoring):
  ARMv8.4 feature for fine-grained QoS control.
  
  ┌──────────────────────────────────────────────────────────┐
  │ MPAM Architecture:                                      │
  │                                                          │
  │ Each transaction carries PARTID (Partition ID):         │
  │   CPU thread → PARTID (via MPAM registers)             │
  │   Transaction → interconnect with PARTID tag            │
  │   Resource → applies QoS rules based on PARTID         │
  │                                                          │
  │ Partitioned resources:                                   │
  │   • Cache: way partitioning per PARTID                  │
  │     PARTID 0 (critical): 12 out of 16 ways             │
  │     PARTID 1 (background): 4 out of 16 ways            │
  │                                                          │
  │   • Memory bandwidth: bandwidth allocation per PARTID  │
  │     PARTID 0: 70% of DDR bandwidth                     │
  │     PARTID 1: 30% of DDR bandwidth                     │
  │                                                          │
  │   • Interconnect: buffer allocation per PARTID         │
  │                                                          │
  │ Monitoring:                                              │
  │   MPAM can count cache usage, bandwidth usage per PARTID│
  │   → Detect: "VM X is using 80% of L3 cache" → throttle│
  └──────────────────────────────────────────────────────────┘

  MPAM registers:
    MPAM0_EL1: PARTID for EL0 (user space)
    MPAM1_EL1: PARTID for EL1 (kernel)
    MPAM2_EL2: PARTID for EL2 (hypervisor)
    MPAM3_EL3: PARTID for EL3 (secure monitor)
    
    Each register:
      PARTID_D: data partition ID
      PARTID_I: instruction partition ID
      PMG:      performance monitoring group

Use case — Cloud server:
  Tenant A (latency-sensitive): PARTID 0
    L3 cache: 75% of ways reserved
    Memory BW: minimum 40 GB/s guaranteed
    
  Tenant B (batch processing): PARTID 1
    L3 cache: 25% of ways
    Memory BW: best-effort (whatever's left)
    
  → Tenant A's latency is stable regardless of Tenant B's load
  → "Noisy neighbor" problem solved at hardware level
```

---

Back to [Question & Answers Index](./README.md)
