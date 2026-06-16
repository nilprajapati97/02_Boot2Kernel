# Interrupt Subsystem — Questions & Answers

---

## Q1. [L1] What is the GIC (Generic Interrupt Controller)? Describe GICv3 architecture.

**Answer:**

```
GIC is ARM's standard interrupt controller. GICv3 is the current
version used with ARMv8-A processors.

GICv3 components:
  ┌──────────────────────────────────────────────────────────────┐
  │                        GIC-600 / GIC-700                     │
  │                                                              │
  │  ┌──────────────────────┐                                   │
  │  │    Distributor        │ (one per system)                  │
  │  │    GICD_*             │                                   │
  │  │  • Routes SPIs to CPUs│                                   │
  │  │  • Priority/enable   │                                   │
  │  │  • Group assignment  │                                   │
  │  │  • Affinity routing  │                                   │
  │  └──────────┬───────────┘                                   │
  │             │                                                │
  │  ┌──────────┴───────────┐                                   │
  │  │   Redistributor (×N) │ (one per CPU core)                │
  │  │   GICR_*             │                                   │
  │  │  • Handles SGI/PPI   │                                   │
  │  │  • LPI configuration │                                   │
  │  │  • Wake request      │                                   │
  │  │  • Power management  │                                   │
  │  └──────────┬───────────┘                                   │
  │             │                                                │
  │  ┌──────────┴───────────┐                                   │
  │  │  CPU Interface        │ (system registers — NOT MMIO!)   │
  │  │  ICC_*_EL1            │                                   │
  │  │  • IAR (acknowledge) │                                   │
  │  │  • EOIR (end of int) │                                   │
  │  │  • PMR (priority mask)│                                  │
  │  │  • BPR (preemption)  │                                   │
  │  │  • RPR (running pri) │                                   │
  │  └──────────────────────┘                                   │
  │                                                              │
  │  ┌──────────────────────┐                                   │
  │  │   ITS (optional)      │ (Interrupt Translation Service)  │
  │  │  • Translates DevID + │                                   │
  │  │    EventID → INTID   │                                   │
  │  │  • Routes LPIs from  │                                   │
  │  │    PCIe MSI/MSI-X    │                                   │
  │  └──────────────────────┘                                   │
  └──────────────────────────────────────────────────────────────┘

GICv3 vs GICv2 key differences:
  • CPU interface via SYSTEM REGISTERS (not MMIO) → faster!
  • Affinity routing (MPIDR-based, not flat CPU targets)
  • LPI support (locality-specific peripheral interrupts)
  • ITS for PCIe MSI → scalable interrupt routing
  • Security groups: Group 0 (Secure FIQ), Group 1 NS (IRQ)
  • Support for >8 cores (GICv2 limited to 8)
```

---

## Q2. [L2] Explain the four interrupt types: SGI, PPI, SPI, and LPI. Give examples of each.

**Answer:**

```
┌─────────┬──────────┬────────────────────────────────────────────┐
│ Type    │ INTID    │ Description & Examples                      │
├─────────┼──────────┼────────────────────────────────────────────┤
│ SGI     │ 0-15     │ Software Generated Interrupt               │
│         │          │ • Triggered by writing to ICC_SGI1R_EL1   │
│         │          │ • Core-to-core signaling                   │
│         │          │ • Linux: IPI for TLB shootdown (INTID 0)  │
│         │          │ • Linux: IPI for reschedule (INTID 1)     │
│         │          │ • Linux: IPI for call function (INTID 5)  │
│         │          │ • Banked per-core (each core has its own) │
│         │          │ • Can target specific cores via MPIDR     │
├─────────┼──────────┼────────────────────────────────────────────┤
│ PPI     │ 16-31    │ Private Peripheral Interrupt               │
│         │          │ • Per-core private devices                 │
│         │          │ • INTID 27: Virtual timer (EL1)           │
│         │          │ • INTID 30: Non-secure physical timer     │
│         │          │ • INTID 29: Secure physical timer         │
│         │          │ • INTID 25: Virtual maintenance interrupt │
│         │          │ • INTID 23: PMU overflow                  │
│         │          │ • Banked: same INTID on each core is      │
│         │          │   independent                              │
├─────────┼──────────┼────────────────────────────────────────────┤
│ SPI     │ 32-1019  │ Shared Peripheral Interrupt                │
│         │          │ • System-wide device interrupts            │
│         │          │ • UART, SPI, I2C, GPIO, DMA, Ethernet    │
│         │          │ • Routed to ANY core via GICD_IROUTER     │
│         │          │ • Can be level-triggered or edge-triggered│
│         │          │ • Example: UART0 = SPI #33 (INTID 65)    │
│         │          │ • Affinity routing: target specific core  │
│         │          │   or "any available core" (Aff*.*.*.*)    │
├─────────┼──────────┼────────────────────────────────────────────┤
│ LPI     │ 8192+    │ Locality-specific Peripheral Interrupt     │
│         │          │ • Message-based (no wire — written to mem)│
│         │          │ • PCIe MSI/MSI-X interrupts               │
│         │          │ • Translated by ITS (DevID+EventID→INTID)│
│         │          │ • Configuration in memory (LPI tables)    │
│         │          │ • Can have 1000s of interrupt sources      │
│         │          │ • Efficient for PCIe devices with many    │
│         │          │   endpoints (NVMe, network cards)          │
└─────────┴──────────┴────────────────────────────────────────────┘

SGI generation example:
  // Send IPI to Core 2 in current cluster:
  // ICC_SGI1R_EL1 format:
  //   [55:48] = Aff3, [39:32] = Aff2, [23:16] = Aff1
  //   [15:0]  = target list (bit per core)
  //   [27:24] = INTID (0-15)
  MOV X0, #(1 << 2)         // Target core 2 (bit 2)
  ORR X0, X0, #(1 << 24)    // INTID = 1 (reschedule IPI)
  MSR ICC_SGI1R_EL1, X0     // Fire!
```

---

## Q3. [L2] How does interrupt priority and preemption work in GICv3?

**Answer:**

```
GICv3 uses 8-bit priority values (0 = highest, 255 = lowest).
Implementation may support fewer bits (e.g., 5 bits → 32 levels).

Priority configuration:
  GICD_IPRIORITYR<n>: priority for SPI n
  GICR_IPRIORITYR<n>: priority for SGI/PPI n
  
  Values written as 8-bit, but lower bits may be RAZ/WI:
  5-bit priority: values 0x00, 0x08, 0x10, ... 0xF8

Priority masking:
  ICC_PMR_EL1 (Priority Mask Register):
    Only interrupts with priority < PMR are forwarded to CPU.
    PMR = 0xFF → all priorities allowed
    PMR = 0x80 → only priorities 0x00-0x78 forwarded
    PMR = 0x00 → all interrupts masked

Running priority:
  ICC_RPR_EL1 (Running Priority Register):
    Shows priority of currently active interrupt.
    Read-only. Used to implement priority-based preemption.

Preemption via Binary Point Register (BPR):
  ICC_BPR1_EL1 divides priority into GROUP and SUBPRIORITY:
  
  ┌────────────────────────────────────────────────────────┐
  │ BPR value │ Group bits │ Subpriority │ Preemption     │
  ├───────────┼────────────┼─────────────┼────────────────┤
  │ 0         │ [7:1]      │ [0]         │ 128 groups     │
  │ 1         │ [7:2]      │ [1:0]       │ 64 groups      │
  │ 2         │ [7:3]      │ [2:0]       │ 32 groups      │
  │ 3         │ [7:4]      │ [3:0]       │ 16 groups      │
  │ 7         │ [7]        │ [6:0]       │ 2 groups       │
  └───────────┴────────────┴─────────────┴────────────────┘
  
  Only GROUP priority determines preemption.
  Within same group: no preemption (FIFO or subpriority order).
  
  Example with BPR=3:
    Currently handling interrupt at priority 0x40 (group 4)
    New interrupt at priority 0x30 (group 3) → group 3 < 4 → PREEMPT!
    New interrupt at priority 0x48 (group 4) → same group → no preempt

Preemption flow:
  1. CPU handling IRQ A (priority 0x40)
  2. New IRQ B arrives (priority 0x20 = higher)
  3. GIC compares: B's group < A's group → signal new IRQ
  4. CPU: nested interrupt:
     a. Save state of A's handler (push to stack)
     b. Acknowledge B (read ICC_IAR1_EL1 → get B's INTID)
     c. Handle B
     d. EOI for B (write ICC_EOIR1_EL1)
     e. Restore A's state, continue handling A
     f. EOI for A
```

---

## Q4. [L3] What is the ITS (Interrupt Translation Service)? How does it handle MSI/MSI-X?

**Answer:**

```
ITS translates device-generated message-based interrupts (MSI/MSI-X)
into LPIs routed to specific CPU cores.

Why ITS?
  PCIe MSI: device writes to a specific address → generates interrupt
  Without ITS: each MSI needs a dedicated SPI → only ~1000 SPIs!
  With ITS: thousands of devices × thousands of MSIs → LPIs
  
  Modern NVMe SSD: 64 MSI-X vectors
  Modern NIC: 256 MSI-X vectors
  Server with 100 PCIe devices: 10000+ interrupt sources!
  → SPIs can't handle this. LPIs via ITS scale to millions.

ITS Architecture:
  ┌──────────────────────────────────────────────────────────┐
  │                     ITS Operation                        │
  │                                                          │
  │  PCIe device writes to GITS_TRANSLATER register:        │
  │    Address = ITS base + 0x10040 (TRANSLATER offset)     │
  │    Data = EventID (device-specific interrupt number)    │
  │    DeviceID = from PCIe RequesterID (BDF)               │
  │                                                          │
  │  ITS translates:                                        │
  │    (DeviceID, EventID) → (INTID, Collection)            │
  │                                                          │
  │  Using two tables in MEMORY:                            │
  │  ┌─────────────────┐     ┌─────────────────────┐       │
  │  │ Device Table     │     │ ITT (per-device)     │       │
  │  │ DevID → ITT base │────▶│ EventID → INTID,    │       │
  │  │                  │     │          Collection  │       │
  │  └─────────────────┘     └──────────┬──────────┘       │
  │                                      │                   │
  │  ┌──────────────────────────────────┘                   │
  │  │  Collection Table:                                    │
  │  │  Collection → Target Redistributor (MPIDR)           │
  │  │  → Routes LPI to specific CPU core                   │
  │  └──────────────────────────────────────────────────────┘
  │                                                          │
  │  Result: LPI INTID delivered to target CPU core         │
  │  CPU reads ICC_IAR1_EL1 → gets LPI INTID               │
  └──────────────────────────────────────────────────────────┘

ITS commands (software configures via command queue):
  MAPD:    Map Device (DeviceID → ITT base)
  MAPTI:   Map Translation (EventID → INTID + Collection)
  MAPC:    Map Collection (Collection → target PE/MPIDR)
  MOVI:    Move interrupt to different collection (CPU migration)
  INV:     Invalidate cached translation
  INVALL:  Invalidate all for a collection
  SYNC:    Synchronize (ensure prior commands completed)
  
  Command queue: ring buffer in memory, ITS processes commands
  GITS_CBASER: base address of command queue
  GITS_CWRITER: software write pointer
  GITS_CREADR: ITS read pointer

Linux: drivers/irqchip/irq-gic-v3-its.c
  Manages ITS configuration, creates IRQ domains for MSI.
```

---

## Q5. [L2] What is the difference between level-triggered and edge-triggered interrupts?

**Answer:**

```
Level-triggered:
  Interrupt line is ACTIVE as long as the condition exists.
  GIC samples the level: high = pending, low = not pending.
  
  ┌──────────────────────────────────────────────────────────┐
  │ Signal: ─────────┐                    ┌─────────────    │
  │                   │     ACTIVE         │                  │
  │                   └────────────────────┘                  │
  │ Device holds line high until condition cleared            │
  │                                                          │
  │ Handler MUST clear the source:                           │
  │   1. Acknowledge interrupt (ICC_IAR1_EL1)               │
  │   2. Read device status register (clears condition)     │
  │   3. Device de-asserts line                              │
  │   4. EOI (ICC_EOIR1_EL1)                                │
  │                                                          │
  │ If handler doesn't clear source:                        │
  │   After EOI, interrupt re-fires immediately → STORM!    │
  │                                                          │
  │ Advantage: can't miss interrupts (level stays asserted) │
  │ Disadvantage: must clear source before EOI               │
  └──────────────────────────────────────────────────────────┘

Edge-triggered:
  Interrupt fires on TRANSITION (rising edge).
  GIC latches the pending state on the edge.
  
  ┌──────────────────────────────────────────────────────────┐
  │ Signal: ─────────┐          ┌───┐       ┌───────────    │
  │                   │          │   │       │                │
  │                   └──────────┘   └───────┘                │
  │                   ↑ edge       ↑ edge   ↑ edge          │
  │                   │            │         │                │
  │                 int#1       int#2      int#3             │
  │                                                          │
  │ Each rising edge latches a PENDING state                │
  │                                                          │
  │ Problem: if two edges occur before handler runs,         │
  │ only ONE is captured. Second edge is LOST!              │
  │                                                          │
  │ Handler should:                                          │
  │   1. Acknowledge interrupt                              │
  │   2. Process ALL pending work (check device FIFO)       │
  │   3. EOI                                                │
  │                                                          │
  │ Advantage: doesn't re-fire if source stays active       │
  │ Disadvantage: can lose interrupts if edges too fast      │
  └──────────────────────────────────────────────────────────┘

GIC configuration:
  GICD_ICFGR<n>: 2 bits per SPI:
    00 = level-sensitive
    10 = edge-triggered
    
  SGIs: always edge-triggered (software-generated)
  PPIs: device-dependent (timer = level, PMU = edge typically)
  SPIs: configured per interrupt

Linux kernel:
  irq_set_irq_type(irq, IRQ_TYPE_LEVEL_HIGH);
  irq_set_irq_type(irq, IRQ_TYPE_EDGE_RISING);
```

---

## Q6. [L2] How does interrupt routing work in GICv3? What is affinity routing?

**Answer:**

```
GICv3 uses affinity-based routing (MPIDR-based) instead of
GICv2's flat target list.

GICv2 routing (old):
  GICD_ITARGETSR: 8-bit CPU target bitmask (up to 8 CPUs)
  CPU 0 → bit 0, CPU 1 → bit 1, ...
  Limitation: only 8 CPUs! Useless for 64+ core servers.

GICv3 affinity routing:
  GICD_IROUTER<n>: routes SPI n using MPIDR-style affinity
  
  ┌───────────────────────────────────────────────────────┐
  │ GICD_IROUTER<n>:                                     │
  │  [39:32] = Aff3                                      │
  │  [23:16] = Aff2                                      │
  │  [15:8]  = Aff1                                      │
  │  [7:0]   = Aff0                                      │
  │  [31]    = Interrupt Routing Mode (IRM)               │
  │            0 = route to specific PE (Aff3.2.1.0)     │
  │            1 = route to ANY available PE ("1-of-N")   │
  └───────────────────────────────────────────────────────┘

  1-of-N routing (IRM=1):
    GIC picks any core that has interrupts enabled.
    Distributes across cores for load balancing.
    Implementation-defined: round-robin, random, etc.
    
  Specific PE routing (IRM=0):
    Delivers to exactly one core (e.g., Aff3=0, Aff2=0,
    Aff1=1, Aff0=2 → Cluster 1, Core 2).

Linux irqbalance / /proc/irq/N/smp_affinity:
  echo 4 > /proc/irq/33/smp_affinity  // Route IRQ 33 to Core 2
  
  Kernel: set_irq_affinity() → writes GICD_IROUTER
  
  irqbalance daemon: periodically redistributes IRQs across
  cores based on load, NUMA topology, and interrupt rate.

For LPIs (via ITS):
  Routing via ITS Collection → each collection maps to a
  specific CPU (Redistributor). Use ITS MOVI command to
  migrate LPI to different CPU.
```

---

## Q7. [L3] How are virtual interrupts handled in GICv3/v4? Explain vLPIs and direct injection.

**Answer:**

```
Virtual interrupts allow a hypervisor to inject interrupts into
guest VMs without expensive VM exits.

GICv3 Virtual Interrupt Support:
  List Registers (ICH_LR<n>_EL2): up to 16 registers
  Each LR represents one virtual interrupt pending for the guest.
  
  ┌──────────────────────────────────────────────────────────┐
  │ ICH_LR<n>_EL2:                                          │
  │  [63:62] = State (00=invalid, 01=pending, 10=active,   │
  │                    11=pending+active)                     │
  │  [61]    = HW: 1=physical interrupt mapped               │
  │  [60]    = Group: 0=Group 0, 1=Group 1                   │
  │  [55:48] = Priority                                      │
  │  [44:32] = pINTID (physical INTID, if HW=1)             │
  │  [31:0]  = vINTID (virtual INTID seen by guest)         │
  └──────────────────────────────────────────────────────────┘
  
  Flow without direct injection:
  1. Physical interrupt → trap to EL2 (hypervisor)
  2. Hypervisor: identify which VM this belongs to
  3. Write ICH_LR with vINTID + priority
  4. ERET to guest VM at EL1
  5. Guest sees virtual interrupt (via ICC_IAR1_EL1)
  6. Guest acknowledges → reads vINTID
  7. Guest handles interrupt, writes EOI
  8. HW: clears LR entry, deactivates physical interrupt
  
  Cost: 2 VM exits per interrupt (entry + EOI)!

GICv4 Direct Injection of vLPIs:
  Eliminates hypervisor involvement for LPIs entirely!
  
  ┌──────────────────────────────────────────────────────────┐
  │ GICv4:                                                   │
  │  1. ITS: translates (DeviceID, EventID) → vINTID       │
  │  2. GIC: looks up vPE table (virtual PE mapping)       │
  │  3. If vPE is running on a physical core:              │
  │     → Inject vINTID DIRECTLY into guest                │
  │     → NO VM exit! Guest gets interrupt immediately!     │
  │  4. If vPE is NOT scheduled:                           │
  │     → Park interrupt as pending in vPE table           │
  │     → Notify hypervisor to schedule the vPE            │
  └──────────────────────────────────────────────────────────┘
  
  Performance improvement:
    GICv3: ~2000 cycles per device interrupt (2 VM exits)
    GICv4: ~200 cycles (direct injection, no exit)
    → 10x faster interrupt handling for assigned devices!
  
  Use case: PCIe device passthrough (VFIO)
    NVMe SSD assigned to VM → interrupts go directly to VM
    No hypervisor involvement → near-native I/O performance

GICv4.1 improvements:
  • VPE doorbell → non-maskable (wakes suspended vPE)
  • Direct injection of vSGIs (not just vLPIs)
  • Better migration support (move vPE between physical PEs)
```

---

## Q8. [L1] What is an IPI (Inter-Processor Interrupt)? How does Linux use it?

**Answer:**

```
IPI = Inter-Processor Interrupt: one CPU core signals another
using SGI (Software Generated Interrupt, INTID 0-15).

Linux ARM64 IPIs:
┌─────┬────────────────────────────────────────────────────────┐
│ SGI │ Purpose                                                │
├─────┼────────────────────────────────────────────────────────┤
│ 0   │ IPI_RESCHEDULE: tell target core to run scheduler     │
│     │ → Core A: wake_up_process(task on Core B)            │
│     │ → Send IPI 0 to Core B                                │
│     │ → Core B: scheduler picks up new task                 │
├─────┼────────────────────────────────────────────────────────┤
│ 1   │ IPI_CALL_FUNC: execute function on target core        │
│     │ → smp_call_function_single(cpu, func, data)          │
│     │ → Target core executes func(data) in interrupt context│
├─────┼────────────────────────────────────────────────────────┤
│ 2   │ IPI_CPU_STOP: halt a core (panic or shutdown)        │
│     │ → Target core enters infinite WFI loop               │
├─────┼────────────────────────────────────────────────────────┤
│ 3   │ IPI_CPU_CRASH_STOP: halt for crash dump              │
├─────┼────────────────────────────────────────────────────────┤
│ 4   │ IPI_TIMER: broadcast timer event (not common)        │
├─────┼────────────────────────────────────────────────────────┤
│ 5   │ IPI_IRQ_WORK: deferred work processing               │
├─────┼────────────────────────────────────────────────────────┤
│ 6   │ IPI_WAKEUP: wake core from deep sleep                │
└─────┴────────────────────────────────────────────────────────┘

TLB shootdown via IPI:
  1. Core A: unmaps a page shared by all cores
  2. Core A: must invalidate TLB on ALL cores
  3. Core A: TLBI VALE1IS broadcast (hardware handles this!)
  4. On ARM: TLB invalidation is broadcast by hardware
     → No need for explicit IPI for TLB shootdown!
     (Unlike x86 which needs IPI-based TLB shootdown)
  
  This is an ARM advantage: inner-shareable TLBI broadcast
  eliminates a major source of IPI overhead.

Sending an IPI:
  // arch/arm64/kernel/smp.c:
  void smp_cross_call(const struct cpumask *target, unsigned int ipinr)
  {
      // Write ICC_SGI1R_EL1 with target cores and INTID
      gic_send_sgi(ipinr, target);
  }
```

---

## Q9. [L2] Explain the interrupt acknowledge and EOI (End Of Interrupt) flow in GICv3.

**Answer:**

```
Two critical registers control the interrupt lifecycle:

ICC_IAR1_EL1 (Interrupt Acknowledge Register):
  CPU READ → returns INTID of highest-priority pending interrupt
  Side effect: interrupt state changes Pending → Active
  
ICC_EOIR1_EL1 (End Of Interrupt Register):
  CPU WRITE INTID → signals interrupt handling complete
  Side effect: interrupt state changes Active → Inactive
  
  Two EOI modes:
  1. EOImode=0 (default): EOIR does priority drop AND deactivate
     → Simple: just write EOIR when done
     
  2. EOImode=1 (split): EOIR does priority drop only
     → Must also write ICC_DIR_EL1 to deactivate
     → Allows priority drop before handler completes
     → Enables preemption of same-priority group
     → Used when hypervisor and guest share interrupt handling

Complete flow:
  ┌────────────────────────────────────────────────────────────┐
  │ 1. Device → GICD: interrupt pending                       │
  │ 2. GICD → GICR: route to target PE                       │
  │ 3. GICR → CPU: assert IRQ signal                         │
  │ 4. CPU: checks PSTATE.I → if 0, take exception           │
  │ 5. CPU: vector to IRQ handler                             │
  │                                                            │
  │ 6. Handler: MRS X0, ICC_IAR1_EL1  // Acknowledge         │
  │    → Returns INTID (e.g., 33 for UART SPI)               │
  │    → GIC: interrupt Pending → Active                      │
  │    → GIC: running priority = this interrupt's priority    │
  │    → GIC: can preempt if higher priority arrives          │
  │                                                            │
  │ 7. Handler: process interrupt (read UART FIFO, etc.)      │
  │                                                            │
  │ 8. Handler: MSR ICC_EOIR1_EL1, X0  // End of Interrupt   │
  │    → GIC: interrupt Active → Inactive                     │
  │    → GIC: running priority returns to previous level     │
  │    → GIC: checks for pending interrupts                   │
  │                                                            │
  │ 9. If another interrupt pending: repeat from step 3       │
  │ 10. If no more: return from exception (ERET)              │
  └────────────────────────────────────────────────────────────┘

Special INTID values:
  1020: pending interrupt belongs to Group 0 (but CPU is in NS)
  1021: pending interrupt is Secure Group 1
  1023: SPURIOUS (no valid pending interrupt)
        → Can happen if interrupt cleared between signal and IAR
        → Handler must check for 1023 and return without EOI!
```

---

## Q10. [L2] How does Linux's interrupt handling architecture work on ARM64? (irq_desc, irq_chip, genirq)

**Answer:**

```
Linux interrupt handling layers:

  ┌─────────────────────────────────────────────────────────────┐
  │ Hardware Level:                                             │
  │   Device → GIC → CPU (exception at VBAR+0x480)            │
  │                                                             │
  │ Assembly Level (arch/arm64/kernel/entry.S):                │
  │   el1h_irq → save registers → call handle_arch_irq       │
  │                                                             │
  │ GIC Driver (drivers/irqchip/irq-gic-v3.c):               │
  │   gic_handle_irq():                                        │
  │     Read ICC_IAR1_EL1 → get INTID                        │
  │     Call generic_handle_domain_irq(domain, INTID)         │
  │                                                             │
  │ Generic IRQ Layer (kernel/irq/):                           │
  │   irq_desc[INTID] → contains:                             │
  │     • action list (handlers registered by drivers)        │
  │     • irq_chip (GIC operations)                           │
  │     • irq_data (hwirq, domain info)                       │
  │     • state (enabled, disabled, pending, active)          │
  │                                                             │
  │ Driver Handler:                                            │
  │   irqreturn_t uart_irq_handler(int irq, void *dev)       │
  │     Read UART status → process data → return IRQ_HANDLED │
  └─────────────────────────────────────────────────────────────┘

irq_chip (GIC operations):
  struct irq_chip gic_chip = {
      .name           = "GICv3",
      .irq_mask       = gic_mask_irq,      // Disable interrupt
      .irq_unmask     = gic_unmask_irq,    // Enable interrupt
      .irq_eoi        = gic_eoi_irq,       // Write ICC_EOIR
      .irq_set_type   = gic_set_type,      // Level/edge
      .irq_set_affinity = gic_set_affinity, // Route to CPU
  };

IRQ domain (maps HW INTID → Linux IRQ number):
  HW INTID 33 (SPI #1) → Linux virq 45 (allocated at boot)
  
  Device tree: specifies INTID, driver requests IRQ
  devm_request_irq(dev, irq, handler, flags, name, data);

Threaded IRQs:
  Top-half: runs in interrupt context (fast, minimal work)
  Bottom-half: runs in kernel thread context (can sleep)
  
  request_threaded_irq(irq, hard_handler, thread_fn, ...);
  
  hard_handler: acknowledge interrupt, return IRQ_WAKE_THREAD
  thread_fn: do actual work (can sleep, take mutexes)
```

---

## Q11. [L3] What is GICv3's support for interrupt grouping (Group 0, Secure Group 1, Non-Secure Group 1)?

**Answer:**

```
GICv3 sorts interrupts into groups that determine delivery
(FIQ vs IRQ) and security policy.

┌──────────────────────────────────────────────────────────────┐
│ Group         │ Delivery │ Target                            │
├───────────────┼──────────┼───────────────────────────────────┤
│ Group 0       │ FIQ      │ EL3 (Secure Monitor)             │
│               │          │ Highest priority, non-maskable   │
│               │          │ Used for: secure interrupts      │
│               │          │ configured by SCR_EL3.FIQ=1      │
├───────────────┼──────────┼───────────────────────────────────┤
│ Secure G1     │ FIQ/IRQ  │ S-EL1 (Secure OS, OP-TEE)       │
│               │          │ Delivered as FIQ to NS-EL1       │
│               │          │ Delivered as IRQ to S-EL1        │
├───────────────┼──────────┼───────────────────────────────────┤
│ Non-Secure G1 │ IRQ      │ NS-EL1 (Linux kernel)            │
│               │          │ Normal interrupts for Normal OS  │
│               │          │ All standard peripheral IRQs     │
└───────────────┴──────────┴───────────────────────────────────┘

Linux pseudo-NMI trick:
  Linux uses Group 0 (FIQ) as a pseudo-NMI:
  • GIC: mark certain interrupts as Group 0 → delivered as FIQ
  • FIQ cannot be masked by PSTATE.I (only by PSTATE.F)
  • Linux: never masks FIQ during normal operation
  • Result: FIQ-based interrupts always get through
  • Used for: watchdog, performance profiling, crash dump
  
  With FEAT_NMI (ARMv8.8): replaced by proper superpriority NMI.

Configuration:
  GICD_IGROUPR<n>:    Group bit (0=Group 0, 1=Group 1)
  GICD_IGRPMODR<n>:   Group modifier (selects Secure vs NS Group 1)
  
  Group 0:   IGROUPR=0, IGRPMODR=0
  Secure G1: IGROUPR=0, IGRPMODR=1
  NS G1:     IGROUPR=1, IGRPMODR=0

  SCR_EL3.FIQ: if 1, FIQ routes to EL3 (not EL1)
  SCR_EL3.IRQ: if 1, IRQ routes to EL3 (unusual)
  HCR_EL2.IMO: if 1, IRQ routes to EL2 (virtualization)
  HCR_EL2.FMO: if 1, FIQ routes to EL2
```

---

## Q12. [L2] What happens when an interrupt arrives while the CPU is in WFI (Wait For Interrupt)?

**Answer:**

```
WFI puts the CPU into a low-power state, waiting for an event.
An interrupt is the primary wake-up mechanism.

WFI behavior:
  ┌──────────────────────────────────────────────────────────┐
  │ CPU executes WFI:                                       │
  │  → CPU enters standby (clock gated, no instruction exec)│
  │  → Cache state preserved                                │
  │  → Pipeline idle, most logic powered down               │
  │  → Waiting for "wake-up event"                          │
  │                                                          │
  │ Wake-up events:                                          │
  │  • IRQ (even if PSTATE.I = 1!)                          │
  │  • FIQ (even if PSTATE.F = 1!)                          │
  │  • Physical/virtual timer expiry                        │
  │  • Debug request                                        │
  │  • Implementation-specific events                       │
  │                                                          │
  │ Key insight: WFI wakes even if interrupts are MASKED!   │
  │  → CPU wakes up and executes the NEXT instruction       │
  │  → If I=1: interrupt stays pending, CPU continues past WFI│
  │  → If I=0: interrupt taken as normal exception          │
  └──────────────────────────────────────────────────────────┘

  Usage pattern in Linux (cpuidle):
    local_irq_disable();          // PSTATE.I = 1
    // Check: is there work to do?
    if (need_resched())
        goto out;                  // Don't WFI, go schedule
    
    // No work → enter idle
    wfi();                         // CPU sleeps
    
    // Woken by interrupt
    local_irq_enable();           // PSTATE.I = 0
    // → Pending interrupt now taken as exception
    // → Handler runs
    
  Why disable IRQ before WFI?
    Prevents race condition:
      Without disable:
        Check: no work → about to WFI
        IRQ arrives HERE → handled → sets need_resched
        WFI → CPU sleeps! Missed the work!
      
      With disable:
        Check: no work
        WFI → IRQ wakes CPU instantly → enable → take IRQ

WFE (Wait For Event) — different:
  Used for spinlocks:
    spin_lock:
      LDAXR W0, [X1]
      CBNZ W0, wait
      STXR W2, W3, [X1]
      CBNZ W2, spin_lock
      RET
    wait:
      WFE                // Sleep until someone does SEV
      B spin_lock        // Retry
    
    spin_unlock:
      STLR WZR, [X1]
      SEV                // Wake all WFE-waiting cores
```

---

Back to [Question & Answers Index](./README.md)
