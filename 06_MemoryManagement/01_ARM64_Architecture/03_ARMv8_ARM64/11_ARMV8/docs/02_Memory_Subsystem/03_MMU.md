# MMU — Memory Management Unit

## 1. What is the MMU?

The MMU is a hardware unit inside each CPU core that translates virtual addresses
to physical addresses. It enforces memory protection, access permissions, and
memory attribute control.

```
┌─────────────────────────────────────────────────────────────┐
│                    MMU Block Diagram                          │
│                                                               │
│  CPU Pipeline                                                │
│  ┌──────────┐     ┌──────────────────────────────────────┐  │
│  │  Load /   │────▶│              MMU                     │  │
│  │  Store    │     │                                      │  │
│  │  Unit     │     │  ┌──────────┐    ┌──────────────┐   │  │
│  └──────────┘     │  │   TLB    │    │  Table Walk   │   │  │
│                    │  │  (Fast)  │    │   Unit (TWU)  │   │  │
│                    │  │          │    │  (Slow - goes │   │  │
│                    │  │ VA→PA    │    │   to memory)  │   │  │
│                    │  │ cache    │    │               │   │  │
│                    │  └────┬─────┘    └───────┬──────┘   │  │
│                    │       │                   │          │  │
│                    │       └─────────┬─────────┘          │  │
│                    │                 │                     │  │
│                    │       ┌─────────▼──────────┐         │  │
│                    │       │  Permission Check   │         │  │
│                    │       │  • AP (access perms) │         │  │
│                    │       │  • XN (execute never)│         │  │
│                    │       │  • Domain            │         │  │
│                    │       └─────────┬──────────┘         │  │
│                    │                 │                     │  │
│                    │       ┌─────────▼──────────┐         │  │
│                    │       │  Memory Attributes   │         │  │
│                    │       │  • Cache policy       │         │  │
│                    │       │  • Shareability       │         │  │
│                    │       │  • Device type        │         │  │
│                    │       └─────────┬──────────┘         │  │
│                    │                 │                     │  │
│                    └─────────────────┼────────────────────┘  │
│                                      │                       │
│                             ┌────────▼────────┐              │
│                             │ Physical Address │              │
│                             │ → Cache / Memory │              │
│                             └─────────────────┘              │
└─────────────────────────────────────────────────────────────┘
```

---

## 2. Enabling the MMU

The MMU is disabled at reset. Boot code must set up page tables before enabling it.

```
// Simplified MMU enable sequence (EL1, 4 KB granule):

// 1. Set up MAIR_EL1 (memory attributes)
LDR X0, =0x00000000000000FF    // Attr0 = Normal WB RA+WA
MSR MAIR_EL1, X0

// 2. Set up TCR_EL1 (translation control)
LDR X0, =0x00000000B5193519    // T0SZ=25, T1SZ=25, 4KB, 39-bit VA
MSR TCR_EL1, X0

// 3. Set TTBR0_EL1 and TTBR1_EL1 (page table base addresses)
LDR X0, =page_table_base_user
MSR TTBR0_EL1, X0
LDR X0, =page_table_base_kernel
MSR TTBR1_EL1, X0

// 4. Invalidate TLB
TLBI VMALLE1
DSB SY
ISB

// 5. Enable MMU (and caches)
MRS X0, SCTLR_EL1
ORR X0, X0, #1         // M bit = MMU enable
ORR X0, X0, #(1 << 2)  // C bit = D-cache enable
ORR X0, X0, #(1 << 12) // I bit = I-cache enable
MSR SCTLR_EL1, X0
ISB                      // Ensure pipeline sees new settings

// CRITICAL: The code that enables the MMU must be identity-mapped
// (VA == PA) so the PC remains valid after MMU is turned on!
```

---

## 3. MMU Faults

When translation fails or permissions are violated, the MMU generates an **abort**:

```
┌─────────────────────────────────────────────────────────────────┐
│  Fault Type              Cause                                   │
├─────────────────────────────────────────────────────────────────┤
│  Translation Fault       No valid mapping (PTE bit[0] = 0)     │
│  (Level 0/1/2/3)         → Page not present in page tables      │
│                                                                   │
│  Access Flag Fault       AF bit = 0 in page descriptor          │
│                           → First access to this page            │
│                                                                   │
│  Permission Fault        AP/XN bits deny the access              │
│                           → Write to read-only page              │
│                           → User access to kernel page            │
│                           → Execute from no-exec page             │
│                                                                   │
│  Address Size Fault      VA or PA exceeds configured size        │
│                           → Out-of-range address                  │
│                                                                   │
│  Alignment Fault         Unaligned access to Device memory       │
│                           → Or if SCTLR_EL1.A = 1                │
└─────────────────────────────────────────────────────────────────┘

ESR_EL1 contains the fault details:
  EC = 0x24 (Data Abort from lower EL) or 0x25 (same EL)
  EC = 0x20 (Instruction Abort from lower EL) or 0x21 (same EL)
  
  ISS field decoding for Data Aborts:
    DFSC [5:0]  — Fault Status Code
      0b000100 = Translation Fault, Level 0
      0b000101 = Translation Fault, Level 1
      0b000110 = Translation Fault, Level 2
      0b000111 = Translation Fault, Level 3
      0b001001 = Access Flag Fault, Level 1
      0b001101 = Permission Fault, Level 1
    WnR [6]     — Write not Read (1 = write caused fault)
    CM [8]      — Cache maintenance instruction
    S1PTW [7]   — Stage 2 fault during Stage 1 walk

FAR_EL1 — Faulting virtual address
```

---

## 4. Page Table Walk Hardware

The **Table Walk Unit (TWU)** is dedicated hardware in the MMU that performs
page table walks automatically on TLB miss.

```
Walk performance (4 KB granule, 48-bit VA):
  Levels to walk: up to 4 (L0 → L3)
  Each level: 1 memory read (~4 cycles if cached, ~100+ if not)

  Best case (all table entries cached):
    4 × ~4 cycles = ~16 cycles

  Worst case (cache-cold):
    4 × ~100 cycles = ~400 cycles!

  With Stage 2 (virtualization):
    Each S1 walk entry needs S2 translation
    Worst case: 4 S1 levels × 4 S2 levels = 16 memory reads
    → ~1600 cycles (mitigated by TLB and walk caches)

Walk caches (intermediate table caching):
  ┌─────────────────────────────────────────┐
  │  Walk Cache                              │
  │  Caches intermediate page table entries │
  │  (L0, L1, L2 descriptors)               │
  │  → Avoids re-reading upper-level tables │
  │  → Reduces 4-level walk to 1-2 reads    │
  └─────────────────────────────────────────┘
```

---

## 5. PAN — Privileged Access Never (ARMv8.1)

PAN prevents kernel code from accidentally accessing user memory:

```
Without PAN:
  Kernel (EL1) can freely read/write user (EL0) memory
  → Security risk: kernel bugs can be exploited to access user data
  → copy_from_user() / copy_to_user() add no hardware protection

With PAN enabled (PSTATE.PAN = 1):
  Kernel access to EL0-accessible pages → Permission Fault!
  → Kernel MUST use special accessors (LDTR/STTR) for user memory
  → Or temporarily clear PAN when doing intentional user access

  LDTR X0, [X1]     // Load using EL0 permissions (respects PAN)
  STTR X0, [X1]     // Store using EL0 permissions (respects PAN)

  Enable: MSR PAN, #1   or   set SCTLR_EL1.SPAN = 0
  Disable temporarily for copy_from_user:
    MSR PAN, #0      // Disable PAN
    LDR X0, [X1]     // Access user memory
    MSR PAN, #1      // Re-enable PAN
```

---

## 6. UAO — User Access Override (ARMv8.2)

UAO simplifies user memory access from kernel mode:

```
With UAO enabled (PSTATE.UAO = 1):
  LDTR/STTR behave like normal LDR/STR at EL1
  → When kernel KNOWS the access is privileged, LDTR doesn't downgrade
  
This is used with PAN:
  When PAN=1, regular LDR/STR to user pages fault
  When PAN=1 and UAO=1, LDTR/STTR ALSO fault to user pages
  → Provides consistent behavior for copy_from_user() helpers
```

---

## 7. Common Page Table Operations

### Creating a Mapping

```
// Map VA 0x1000_0000 to PA 0x8000_0000 (4 KB page, Normal WB, RW, User)

// Calculate page table indices (4 KB granule, 39-bit VA):
//   L1 index = VA[38:30] = 0
//   L2 index = VA[29:21] = 0x80
//   L3 index = VA[20:12] = 0x000

// L3 page descriptor:
//   Output Address = 0x8000_0000 (PA[47:12] = 0x80000)
//   AttrIndx = 0 (Normal WB from MAIR)
//   AP[2:1] = 01 (EL1+EL0 RW)
//   SH[1:0] = 11 (Inner Shareable)
//   AF = 1 (Access Flag set)
//   UXN = 1, PXN = 1 (No Execute — it's data)
//   nG = 1 (Not Global — per-process)

descriptor = PA | (UXN<<54) | (PXN<<53) | (nG<<11) | (AF<<10) 
           | (SH<<8) | (AP<<6) | (AttrIndx<<2) | 0b11
```

### Context Switch (Changing Address Space)

```
// Switch from Process A to Process B:

// 1. Set new ASID and page table base
LDR X0, =new_ttbr0_with_asid     // ASID in bits [63:48]
MSR TTBR0_EL1, X0

// 2. Ensure completion
ISB

// No TLB flush needed! ASID provides isolation.
// (Unless ASID was recycled — then TLBI for old ASID first)
```

---

Next: [TLB →](./04_TLB.md)
