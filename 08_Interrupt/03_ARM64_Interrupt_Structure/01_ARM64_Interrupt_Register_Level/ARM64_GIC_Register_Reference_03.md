I'll provide a deep dive into GICv2 vs GICv3 architecture and explain how interrupts flow through each component.

## GIC Architecture: GICv2 vs GICv3 - Complete Flow Analysis

### **GICv2 Architecture (ARM Cortex-A9, A15)**

```
Hardware Interrupt → GICD → GICC (per-CPU) → CPU Core
                      ↓
                   (MMIO)
```

#### **1. GICv2 Distributor (GICD) - Global Component**

**Location**: One per SoC, MMIO base address (e.g., `0x08000000`)

**Role**: Central hub for all interrupts except SGIs (which are per-CPU)

**Key Registers & Flow**:

```
Interrupt arrives at GICD:
   ↓
Step 1: Check ENABLE state
   GICD_ISENABLER<n> @ 0x0100 + (INTID/32)*4
   - Bit per interrupt (32 bits per register)
   - If bit = 0 → interrupt ignored
   - If bit = 1 → proceed
   
Step 2: Set PENDING state
   GICD_ISPENDR<n> @ 0x0200 + (INTID/32)*4
   - Hardware sets bit when interrupt asserts
   - Software can read to see pending interrupts
   
Step 3: Check PRIORITY
   GICD_IPRIORITYR<n> @ 0x0400 + INTID
   - 8 bits per interrupt (1 byte per INTID)
   - Values 0x00-0xFF (lower = higher priority)
   - Example: INTID 103 → offset 0x0400 + 103 = 0x0467
   
Step 4: Check TRIGGER TYPE
   GICD_ICFGR<n> @ 0x0C00 + (INTID/16)*4
   - 2 bits per interrupt
   - Bit[1] = 0: Level-sensitive
   - Bit[1] = 1: Edge-triggered
   
Step 5: Determine TARGET CPU (GICv2 only)
   GICD_ITARGETSR<n> @ 0x0800 + INTID
   - 8 bits per interrupt (1 byte per INTID)
   - Each bit represents a CPU (bit 0 = CPU0, bit 1 = CPU1, etc.)
   - Example: 0x05 = 0b00000101 → target CPU0 and CPU2
   - Can target multiple CPUs (first to ACK wins)
   
Step 6: Forward to target CPU's GICC
   GICD selects highest-priority pending interrupt
   → sends to GICC of target CPU(s)
```

**GICv2 GICD Register Map**:
```
0x0000  GICD_CTLR          Enable distributor (bit 0)
0x0004  GICD_TYPER         Read: # of INTIDs, # CPUs
0x0100  GICD_ISENABLER0    Enable bits for INTID 0-31
0x0104  GICD_ISENABLER1    Enable bits for INTID 32-63
0x0180  GICD_ICENABLER0    Disable (write 1 to clear enable)
0x0200  GICD_ISPENDR0      Pending state (set by HW/SW)
0x0280  GICD_ICPENDR0      Clear pending (write 1)
0x0300  GICD_ISACTIVER0    Active state (set on ACK, cleared on EOI)
0x0400  GICD_IPRIORITYR0   Priority byte for INTID 0
0x0401  GICD_IPRIORITYR1   Priority byte for INTID 1
...
0x0800  GICD_ITARGETSR0    Target CPU bits for INTID 0 (GICv2)
0x0801  GICD_ITARGETSR1    Target CPU bits for INTID 1
...
0x0C00  GICD_ICFGR0        Config: edge/level for INTID 0-15
0x0C04  GICD_ICFGR1        Config: edge/level for INTID 16-31
0x0F00  GICD_SGIR          Software Generated Interrupt Register
```

---

#### **2. GICv2 CPU Interface (GICC) - Per-CPU Component**

**Location**: One GICC per CPU core, MMIO (e.g., CPU0 @ `0x08010000`, CPU1 @ `0x08020000`)

**Role**: Interface between GICD and CPU core; handles ACK, EOI, priority filtering

**Detailed Flow**:

```
GICD forwards interrupt to GICC:
   ↓
Step 1: Priority Filtering
   GICC_PMR @ 0x0004 (Priority Mask Register)
   - 8-bit mask value (e.g., 0xF0)
   - Only interrupts with priority < PMR can signal CPU
   - Example: PMR=0xF0, INTID priority=0xA0 → allowed (0xA0 < 0xF0)
            PMR=0xF0, INTID priority=0xFF → blocked
   
Step 2: Running Priority Check
   GICC_RPR @ 0x0014 (Running Priority Register)
   - Tracks priority of currently active interrupt
   - New interrupt must have priority < RPR to preempt
   
Step 3: Signal CPU
   If priority checks pass:
   - GICC asserts nIRQ pin on CPU core
   - CPU takes exception → jumps to VBAR_EL1 + 0x280/0x480
   
Step 4: CPU Reads GICC_IAR (Interrupt Acknowledge)
   GICC_IAR @ 0x000C (read-only)
   - Reading this register = ACKNOWLEDGMENT
   - Returns INTID of highest-priority pending interrupt
   - Side effects:
     • GICD_ISPENDR bit cleared (no longer pending)
     • GICD_ISACTIVER bit set (now active)
     • GICC_RPR updated to this interrupt's priority
   
   Example:
   u32 irqnr = readl(GICC_BASE + GICC_IAR);  // Returns 103
   
Step 5: Interrupt Handler Executes
   - Linux kernel processes interrupt
   - Calls device driver handler
   
Step 6: CPU Writes GICC_EOIR (End of Interrupt)
   GICC_EOIR @ 0x0010 (write-only)
   - Write INTID value to signal completion
   - Side effects:
     • GICD_ISACTIVER bit cleared (no longer active)
     • GICC_RPR restored to previous priority
     • Allows lower-priority interrupts to signal again
   
   Example:
   writel(103, GICC_BASE + GICC_EOIR);
```

**GICv2 GICC Register Map**:
```
0x0000  GICC_CTLR       Enable CPU interface (bit 0)
0x0004  GICC_PMR        Priority Mask (8-bit)
0x0008  GICC_BPR        Binary Point (preemption grouping)
0x000C  GICC_IAR        Interrupt Acknowledge (read = ACK)
0x0010  GICC_EOIR       End of Interrupt (write INTID)
0x0014  GICC_RPR        Running Priority (read-only)
0x0018  GICC_HPPIR      Highest Priority Pending IRQ (read, no side-effect)
0x00D0  GICC_APR0       Active Priority Register
```

---

### **GICv3 Architecture (ARM Cortex-A53, A57, A72, Neoverse)**

```
Hardware Interrupt → GICD → GICR (per-CPU) → ICC_* sysregs → CPU Core
                      ↓              ↓
                   (MMIO)         (MMIO)
                   
PCIe MSI → ITS → LPI → GICR → ICC_* sysregs → CPU Core
```

**Major Changes from GICv2**:
1. **CPU Interface is now system registers** (not MMIO)
2. **Redistributor added** for per-CPU SGI/PPI management
3. **Affinity Routing**: Uses MPIDR, not bitmask (supports >8 CPUs)
4. **ITS + LPI**: Message-based interrupts for PCIe

---

#### **3. GICv3 Distributor (GICD) - Enhanced**

**Key Difference**: `GICD_ITARGETSR` is **removed**, replaced by `GICD_IROUTER`

**New Register: `GICD_IROUTER<n>` (64-bit per SPI)**

```
GICD_IROUTER<n> @ 0x6000 + (INTID * 8)

Bit Layout:
┌─────────┬─────────┬─────────┬─────────┬─────────┬─────────┐
│ [63:40] │ [39:32] │ [31:24] │ [23:16] │ [15:8]  │  [7:0]  │
│  Aff3   │   IRM   │  Aff2   │  Aff1   │  Aff0   │  Rsvd   │
└─────────┴─────────┴─────────┴─────────┴─────────┴─────────┘

IRM (Interrupt Routing Mode):
- Bit[31] = 0: Route to specific CPU with MPIDR = Aff3.Aff2.Aff1.Aff0
- Bit[31] = 1: Route to "any" CPU (GIC picks, load balance)

Example:
GICD_IROUTER103 = 0x0000000000000002
→ Target CPU with MPIDR_EL1 = 0x00000002 (CPU2, Aff0=2)

GICD_IROUTER103 = 0x0000000080000000
→ IRM=1, any CPU can handle (1-of-N mode)
```

**Flow for SPI (Shared Peripheral Interrupt)**:
```
1. GPIO asserts → SPI 71 (INTID 103)
   ↓
2. GICD_ISENABLER[103] checked → enabled
   ↓
3. GICD_ISPENDR[103] set → pending
   ↓
4. GICD_IPRIORITYR103 = 0xA0 (priority)
   ↓
5. GICD_IROUTER103 = 0x0000000000000002
   → Target CPU2
   ↓
6. GICD forwards to CPU2's GICR
```

---

#### **4. GICv3 Redistributor (GICR) - New Component**

**Location**: One per CPU core, MMIO (e.g., CPU0 @ `0x080A0000`, each 128KB apart)

**Role**: 
- Manages **SGIs (0-15)** and **PPIs (16-31)** locally
- Acts as conduit for **SPIs** targeted at this CPU
- Replaces per-CPU portions of GICv2 GICD

**GICR Structure**: Two frames per CPU

```
Frame 0: RD_base (Redistributor)
   0x0000  GICR_CTLR           Control
   0x0008  GICR_TYPER          Read: affinity, last CPU indicator
   0x0014  GICR_WAKER          Wake/sleep control
   0x0018  GICR_PROPBASER      LPI config table base
   0x0020  GICR_PENDBASER      LPI pending table base

Frame 1: SGI_base (offset +0x10000 from RD_base)
   0x10080 GICR_IGROUPR0       Group 0/1 for SGI/PPI
   0x10100 GICR_ISENABLER0     Enable SGI/PPI (bits 0-31)
   0x10180 GICR_ICENABLER0     Disable SGI/PPI
   0x10200 GICR_ISPENDR0       Set pending SGI/PPI
   0x10280 GICR_ICPENDR0       Clear pending
   0x10300 GICR_ISACTIVER0     Active state
   0x10400 GICR_IPRIORITYR0    Priority for SGI 0
   0x10401 GICR_IPRIORITYR1    Priority for SGI 1
   ...
   0x10C00 GICR_ICFGR0         Edge/level for SGI/PPI
```

**Flow for PPI (Private Peripheral Interrupt)**:
```
Example: ARM Generic Timer interrupt (PPI 30, INTID 30)

1. Timer expires on CPU2
   ↓
2. PPI 30 signal arrives at CPU2's GICR
   ↓
3. GICR_ISENABLER0[30] checked → enabled
   ↓
4. GICR_ISPENDR0[30] set → pending
   ↓
5. GICR_IPRIORITYR30 = 0x80 (priority)
   ↓
6. GICR forwards to CPU2's ICC_* sysregs
```

---

#### **5. GICv3 CPU Interface (ICC_*) - System Registers**

**Major Change**: **No MMIO** in GICv3! Uses `mrs`/`msr` instructions.

**Why the Change?**:
- Faster access (on-core register vs memory bus)
- Virtualization-friendly (hypervisor can trap)
- Scales to many CPUs (no MMIO address space exhaustion)

**Key System Registers**:

```asm
// Enable system register access (must be done at boot)
mrs  x0, ICC_SRE_EL1
orr  x0, x0, #1              // Set SRE bit
msr  ICC_SRE_EL1, x0
isb

// Enable Group 1 interrupts
mov  x0, #1
msr  ICC_IGRPEN1_EL1, x0
isb

// Set priority mask (allow all)
mov  x0, #0xFF
msr  ICC_PMR_EL1, x0
isb
```

**Interrupt Handling Flow**:

```c
// Step 1: CPU takes IRQ exception → el1_irq/el0_irq

// Step 2: Read IAR (Acknowledge)
u32 irqnr;
asm volatile("mrs %0, ICC_IAR1_EL1" : "=r" (irqnr));
// or: irqnr = read_sysreg_s(SYS_ICC_IAR1_EL1);

// This read has side effects:
// - Returns highest-priority pending INTID
// - Moves interrupt from pending → active
// - Updates ICC_RPR_EL1 (running priority)

// Step 3: Handle interrupt
if (irqnr == 1023) {
    // Spurious interrupt
    return;
}

generic_handle_domain_irq(domain, irqnr);

// Step 4: Write EOI
asm volatile("msr ICC_EOIR1_EL1, %0" :: "r" (irqnr));
// or: write_sysreg_s(irqnr, SYS_ICC_EOIR1_EL1);

// This write has side effects:
// - Clears active state
// - Drops running priority
// - Allows lower-priority interrupts
```

**Complete ICC_* Register Set**:

| Register | Encoding | Function | Access |
|----------|----------|----------|--------|
| `ICC_IAR0_EL1` | S3_0_C12_C8_0 | Acknowledge Group 0 (FIQ) | R |
| `ICC_IAR1_EL1` | S3_0_C12_C12_0 | **Acknowledge Group 1 (IRQ)** | R |
| `ICC_EOIR0_EL1` | S3_0_C12_C8_1 | EOI Group 0 | W |
| `ICC_EOIR1_EL1` | S3_0_C12_C12_1 | **EOI Group 1** | W |
| `ICC_DIR_EL1` | S3_0_C12_C11_1 | Deactivate (if EOImode=1) | W |
| `ICC_PMR_EL1` | S3_0_C4_C6_0 | Priority Mask | R/W |
| `ICC_BPR0_EL1` | S3_0_C12_C8_3 | Binary Point Group 0 | R/W |
| `ICC_BPR1_EL1` | S3_0_C12_C12_3 | Binary Point Group 1 | R/W |
| `ICC_RPR_EL1` | S3_0_C12_C11_3 | Running Priority | R |
| `ICC_CTLR_EL1` | S3_0_C12_C12_6 | Control (EOImode, CBPR) | R/W |
| `ICC_SRE_EL1` | S3_0_C12_C12_5 | System Register Enable | R/W |
| `ICC_IGRPEN0_EL1` | S3_0_C12_C12_6 | Enable Group 0 | R/W |
| `ICC_IGRPEN1_EL1` | S3_0_C12_C12_7 | **Enable Group 1** | R/W |
| `ICC_SGI1R_EL1` | S3_0_C12_C11_5 | Generate SGI (IPI) | W |

---

#### **6. GICv3 ITS (Interrupt Translation Service)**

**Purpose**: Handle **message-based interrupts** (PCIe MSI/MSI-X)

**Problem Solved**: 
- PCIe devices can't generate wire interrupts
- Instead, they write to a memory address (Message Signaled Interrupt)
- ITS translates: (DeviceID, EventID) → LPI INTID

**Flow for PCIe MSI**:

```
1. PCIe Device wants to interrupt:
   - Device writes to MSI address (configured by OS)
   - Write contains: DeviceID + EventID
   
Example:
   NVMe controller (DeviceID = 0x1234) writes to 0x08080000
   Write data = EventID 5 (completion queue)

2. ITS receives write:
   ITS_BASE @ 0x08080000 (GITS_TRANSLATER register)
   
3. ITS looks up in Device Table:
   GITS_BASER0 (Device Table base address)
   Index by DeviceID 0x1234
   → finds ITT (Interrupt Translation Table) pointer

4. ITS looks up in ITT:
   Index by EventID 5
   → finds LPI INTID 8195
   → finds target GICR (e.g., CPU3's GICR)
   → finds collection ID

5. ITS sends LPI command to CPU3's GICR:
   - GICR sets bit in LPI Pending Table
   - GICR signals ICC_IAR1_EL1
   
6. CPU3 reads ICC_IAR1_EL1 → returns 8195
   
7. Kernel handles LPI:
   - LPIs bypass GICD priority/enable logic
   - Configured via per-CPU tables in memory
```

**ITS Registers (MMIO)**:

```
ITS_BASE: 0x08080000

0x0000  GITS_CTLR         Enable ITS
0x0008  GITS_TYPER        Read: supported features
0x0080  GITS_CBASER       Command Queue base address
0x0088  GITS_CWRITER      Command queue write pointer
0x0090  GITS_CREADR       Command queue read pointer
0x0100  GITS_BASER0       Device Table base
0x0108  GITS_BASER1       Collection Table base
...
0x10040 GITS_TRANSLATER   Write-only: PCIe writes here
```

**ITS Commands** (written to command queue):

```c
struct its_cmd {
    u64 cmd;
    u64 args[3];
};

// Map device
MAPD: Associate DeviceID with ITT
// Map event
MAPTI: (DeviceID, EventID) → (LPI INTID, CollectionID)
// Map collection
MAPC: CollectionID → target GICR (CPU)
// Invalidate
INV: Flush cached state
```

---

#### **7. LPI (Locality-specific Peripheral Interrupts)**

**INTID Range**: 8192 - 2^32

**Key Differences from SPI/PPI**:
- **Message-based** (no wire)
- **Huge ID space** (vs 1020 max for SPI)
- **Configuration in memory tables** (not MMIO registers)

**LPI Configuration**:

```
Per-CPU LPI Pending Table:
   Base address: GICR_PENDBASER
   - 1 bit per LPI INTID
   - Example: LPI 8195 → byte offset (8195-8192)/8 = 0
   
Global LPI Configuration Table:
   Base address: GICR_PROPBASER
   - 1 byte per LPI INTID
   - Bits[7:2] = priority
   - Bit[1] = Group (0 or 1)
   - Bit[0] = Enable
   
Example for LPI 8195:
   Config byte @ (PROPBASER + (8195 - 8192)) = PROPBASER + 3
   Value = 0xA1
   → Priority 0xA0, Group 1, Enabled
```

---

## **Complete Flow Comparison: GICv2 vs GICv3**

### **GICv2 GPIO Interrupt Flow**

```
1. GPIO pin toggles
   ↓
2. GPIO controller asserts SPI 71 (INTID 103)
   ↓
3. GICD (MMIO @ 0x08000000):
   - GICD_ISENABLER[103] = 1? ✓
   - GICD_ISPENDR[103] ← 1 (pending)
   - GICD_IPRIORITYR103 = 0xA0
   - GICD_ITARGETSR103 = 0x04 (bit 2 = CPU2)
   ↓
4. GICD forwards to CPU2's GICC
   ↓
5. GICC (MMIO @ 0x08020000):
   - GICC_PMR = 0xF0 (mask check: 0xA0 < 0xF0 ✓)
   - GICC_RPR = 0xFF (running pri check: 0xA0 < 0xFF ✓)
   - Assert nIRQ to CPU2
   ↓
6. CPU2 takes exception → el1_irq
   ↓
7. Kernel reads GICC_IAR:
   u32 irqnr = readl(GICC_BASE + 0x000C);  // Returns 103
   Side effect: pending → active
   ↓
8. Handle interrupt
   ↓
9. Kernel writes GICC_EOIR:
   writel(103, GICC_BASE + 0x0010);
   Side effect: active → inactive
```

### **GICv3 GPIO Interrupt Flow**

```
1. GPIO pin toggles
   ↓
2. GPIO controller asserts SPI 71 (INTID 103)
   ↓
3. GICD (MMIO @ 0x08000000):
   - GICD_ISENABLER[103] = 1? ✓
   - GICD_ISPENDR[103] ← 1 (pending)
   - GICD_IPRIORITYR103 = 0xA0
   - GICD_IROUTER103 = 0x0000000000000002 (Aff0=2 → CPU2)
   ↓
4. GICD forwards to CPU2's GICR
   ↓
5. GICR (MMIO @ 0x080C0000 for CPU2):
   - Receives SPI from GICD
   - Checks priority vs ICC_PMR_EL1
   - Signals CPU2's ICC_* interface
   ↓
6. CPU2 takes exception → el1_irq
   ↓
7. Kernel reads ICC_IAR1_EL1 (system register):
   mrs x0, ICC_IAR1_EL1  // Returns 103
   Side effect: pending → active, RPR updated
   ↓
8. Handle interrupt
   ↓
9. Kernel writes ICC_EOIR1_EL1:
   msr ICC_EOIR1_EL1, x0  // Write 103
   Side effect: active → inactive, RPR restored
```

### **GICv3 PCIe MSI Flow**

```
1. NVMe device completion
   ↓
2. NVMe writes to MSI address:
   Address: 0x08080000 (GITS_TRANSLATER)
   Data: (DeviceID=0x1234, EventID=5)
   ↓
3. ITS receives write:
   - Look up DeviceID 0x1234 in Device Table
   - Find ITT pointer
   - Look up EventID 5 in ITT
   - Find: LPI INTID 8195, CollectionID 3
   ↓
4. ITS sends LPI to CPU3's GICR:
   - GICR sets bit in LPI Pending Table
   - GICR signals CPU3
   ↓
5. CPU3 takes exception → el1_irq
   ↓
6. Kernel reads ICC_IAR1_EL1:
   mrs x0, ICC_IAR1_EL1  // Returns 8195
   ↓
7. Handle NVMe completion
   ↓
8. Kernel writes ICC_EOIR1_EL1:
   msr ICC_EOIR1_EL1, x0  // Write 8195
```

---

## **Summary Table**

| Feature | GICv2 | GICv3 |
|---------|-------|-------|
| **Distributor (GICD)** | MMIO, manages all interrupts | MMIO, manages SPIs only |
| **CPU Interface** | MMIO per-CPU (GICC) | System registers (ICC_*) |
| **Per-CPU Component** | Part of GICD for SGI/PPI | GICR (Redistributor) |
| **CPU Targeting** | GICD_ITARGETSR (8-bit mask) | GICD_IROUTER (64-bit affinity) |
| **Max CPUs** | 8 (bitmask limitation) | Unlimited (affinity routing) |
| **SGI/PPI Management** | GICD registers | GICR registers (per-CPU) |
| **MSI Support** | External controller needed | Built-in ITS |
| **LPI Support** | No | Yes (8192+ INTID range) |
| **Virtualization** | Basic | Enhanced (system registers) |

This architecture evolution enables modern ARM systems to scale to hundreds of cores and efficiently handle thousands of PCIe devices with MSI-X.