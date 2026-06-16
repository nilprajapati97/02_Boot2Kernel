# Descriptor Fields: AttrIndx, AP, SH, AF, nG, DBM, Cont, PXN, UXN

**Category**: Page Tables and Translation  
**Targeted**: ARM, Qualcomm, NVIDIA

---

## 1. Concept Foundation

ARM64 page/block descriptors pack all memory access attributes into a single 64-bit value alongside the physical address. Understanding each attribute field is critical for:
- Writing correct memory mappings (e.g., DMA-coherent vs cached)
- Configuring proper permissions (kernel vs user, RW vs RO, exec vs no-exec)
- Implementing page reclaim (AF), dirty tracking (DBM), and TLB optimization (Cont)
- Ensuring security (PXN/UXN prevent unwanted code execution)

---

## 2. Full 64-bit Page/Block Descriptor Layout

```
63      59 58  55 54 53 52 51 48 47                 12 11 10 9  8  7  6  5  4  3  2  1  0
┌─────────┬──────┬──┬──┬──┬─────┬────────────────────┬──┬──┬───┬──┬──┬──┬──┬──┬──┬──┬──┐
│ UpperAtt│SWuse │XN│PX│Ct│RES0 │  Output Address     │nG│AF│ SH│AP│NS│  AttrIndx  │T │V │
│ [63:59] │[58:55│54│53│52│51:48│     [47:12]         │11│10│9:8│7:6│5 │ [4:2]     │1 │0 │
└─────────┴──────┴──┴──┴──┴─────┴────────────────────┴──┴──┴───┴──┴──┴──┴──┴──┴──┴──┴──┘

V (bit 0)    : Valid bit (1=valid, 0=invalid/fault)
T (bit 1)    : Type (0=block at L1/L2, 1=page at L3 or table at L0/L1/L2)
AttrIndx[4:2]: 3-bit MAIR_EL1 index for memory type
NS (bit 5)   : Non-Secure (TrustZone, EL3 use)
AP[7:6]      : Access Permissions (data read/write)
SH[9:8]      : Shareability domain
AF (bit 10)  : Access Flag
nG (bit 11)  : Non-Global (ASID-tagged)
OA[47:12]    : Output (Physical) Address
Cont (bit 52): Contiguous hint (group of 16 entries share TLB)
PXN (bit 53) : Privileged Execute Never
UXN (bit 54) : User Execute Never (also written XN)
SW[58:55]    : 4 bits for OS software use
UpperAtt     : APTable, XNTable, PXNTable, NSTable propagation
```

---

## 3. AttrIndx [4:2] — Memory Attribute Index

### Mechanism

3-bit field indexing into `MAIR_EL1` (Memory Attribute Indirection Register). Each index selects one of 8 possible attribute bytes, which defines the memory type for this mapping.

```
MAIR_EL1 layout:
  Bits[7:0]   = Attr0
  Bits[15:8]  = Attr1
  Bits[23:16] = Attr2
  Bits[31:24] = Attr3
  Bits[39:32] = Attr4
  Bits[47:40] = Attr5
  Bits[55:48] = Attr6
  Bits[63:56] = Attr7

PTE.AttrIndx = 0 → uses Attr0 (bits[7:0] of MAIR_EL1)
PTE.AttrIndx = 1 → uses Attr1 (bits[15:8])
...
```

### Linux MAIR_EL1 Configuration

```c
// arch/arm64/include/asm/memory.h
#define MT_DEVICE_nGnRnE     0  // Strongly Ordered / Device: no gather/reorder/early
#define MT_DEVICE_nGnRE      1  // Device: no gather/no reorder, early write ack
#define MT_DEVICE_GRE        2  // Device: gather, reorder, early write ack
#define MT_NORMAL_NC         3  // Normal, Non-Cacheable
#define MT_NORMAL            4  // Normal, Inner WB RA WA; Outer WB RA WA
#define MT_NORMAL_WT         5  // Normal, Write-Through, non-Transient
#define MT_NORMAL_TAGGED     6  // Normal, Inner WB RA WA + MTE tag support
#define MT_NORMAL_RA         7  // (varies; sometimes Inner WB RA; Outer WB RA)

// Linux initializes MAIR_EL1 in cpu_do_switch_mm() and arch/arm64/mm/proc.S:
// MAIR_EL1 = 
//   (MAIR_ATTR_DEVICE_nGnRnE << (MT_DEVICE_nGnRnE * 8)) |
//   (MAIR_ATTR_DEVICE_nGnRE  << (MT_DEVICE_nGnRE * 8))  |
//   ...etc...
//   (MAIR_ATTR_NORMAL         << (MT_NORMAL * 8))
```

### MAIR Attribute Byte Format

```
For Normal memory (AttrIndx points to Normal):
  Bits[7:4] = Outer cache attributes
  Bits[3:0] = Inner cache attributes
  
  Cache attribute encoding (4-bit field):
    0b0000 = Device (only for Outer; Inner uses device-specific)
    0b0100 = Normal Non-Cacheable
    0b1000 = Normal Write-Through, non-Transient, no Read/Write-Allocate
    0b1001 = Normal Write-Back, non-Transient, no Read/Write-Allocate
    0b1100 = Normal Write-Through, non-Transient, Read-Allocate
    0b1101 = Normal Write-Back, non-Transient, Read-Allocate
    0b1110 = Normal Write-Through, Transient, Read/Write-Allocate
    0b1111 = Normal Write-Back, Transient, Read/Write-Allocate (Full WB)

Standard MAIR_ATTR_NORMAL = 0xFF = Inner WB RA WA + Outer WB RA WA (highest performance)
```

---

## 4. AP [7:6] — Access Permissions

```
AP[1] AP[0]  EL1 Access     EL0 Access    Linux Use
  0     0    Read/Write      No Access     Kernel-only RW
  0     1    Read/Write      Read/Write    User + Kernel RW
  1     0    Read-Only       No Access     Kernel-only RO
  1     1    Read-Only       Read-Only     User + Kernel RO

Linux constants (arch/arm64/include/asm/pgtable-hwdef.h):
  PTE_WRITE  = 0 (AP[0]=0 means writable for EL1)
  PTE_RDONLY = (1UL << 7)   // AP[0]=1 → read-only
  PTE_USER   = (1UL << 6)   // AP[1]=1 → user access allowed

User-writable mapping: AP = 0b01 (AP[1]=0, AP[0]=1)
  → User can read/write, kernel can read/write
  
Kernel-only RW mapping: AP = 0b00
  → EL0 access causes Permission Fault

Note: "Write" permission for EL0 requires BOTH AP[0]=1 AND SCTLR_EL1.WXN=0
      With WXN=1 (Write-XOR-Execute), writable pages cannot be executable.
```

---

## 5. SH [9:8] — Shareability

Shareability defines which agents share the memory view (cache coherency domain):

```
SH[1] SH[0]   Domain             Usage
  0     0     Non-Shareable      Private to this CPU only, no coherency needed
  0     1     Reserved           (UNPREDICTABLE)
  1     0     Outer Shareable    Coherent with GPU, DMA, other agents in Outer domain
  1     1     Inner Shareable    Coherent with other CPUs in Inner domain (SMP coherency)

Linux uses:
  Normal memory: SH = 0b11 (Inner Shareable) — CPU SMP coherency
  Device memory: SH = 0b00 (Non-Shareable) — device registers don't need CPU coherency
  Normal NC:     SH = 0b11 (Inner Shareable) or 0b10 (Outer Shareable) depending on use

PTE_SHARED = (3UL << 8) = Inner Shareable
```

**Important**: Device memory MUST use Non-Shareable to avoid speculative accesses.
Cacheable Normal memory must use Inner or Outer Shareable for correct SMP behavior.

---

## 6. AF [10] — Access Flag

### Purpose

The AF bit implements page aging for the Linux page reclaim subsystem:

```
AF=0: First access to this page generates an Access Flag Fault (ELx to EL1)
      Exception syndrome: ESR_EL1.EC = 0x21 (Data Abort from EL0)
                          or 0x25 (Data Abort from EL1)
      With DFSC = 0x011 (Level 3 AF fault) or 0x010/0x001/0x000 (L2/L1/L0)

AF=1: Normal access, no fault

Software AF management (without TCR_EL1.HA):
  1. New page mapped: AF=0
  2. First access → AF fault → kernel sets AF=1, marks page accessed
  3. Periodically (workingset_refault_distance): kernel clears AF=0 for aging
  4. If page accessed again before reclaim → fault → AF=1 again (page stays)
  5. If not accessed → page reclaim selects it as cold page

Hardware AF (TCR_EL1.HA=1, ARMv8.1):
  Hardware sets AF=1 on access without generating a fault
  Reduces fault overhead; Linux enables HA on capable systems
```

---

## 7. nG [11] — Non-Global

```
nG=0 (Global):
  TLB entry not tagged with ASID
  Survives context switches — valid for all processes
  Used for: kernel mappings (TTBR1 region)

nG=1 (Non-Global):
  TLB entry tagged with current ASID from TTBR0_EL1[63:48]
  Only valid for the process with matching ASID
  Used for: all user space mappings (TTBR0 region)
  
  On context switch: change ASID → old nG entries automatically invalid
  No need to flush TLB on every context switch (ASID provides isolation)
```

---

## 8. DBM [51] — Dirty Bit Management (ARMv8.1)

### Mechanism

Without DBM, the kernel must perform a software write-protect dance to detect dirty pages:
1. Map page as read-only
2. First write → Permission Fault → kernel marks page dirty, maps RW
3. On reclaim/swap: check if dirty by checking if software dirty bit set

With **DBM (Dirty Bit Management)** hardware support (ARMv8.1+):

```
DBM=1 in descriptor enables hardware dirty tracking:
  When EL0 or EL1 performs a write to this page:
    → Hardware automatically clears AP[0] (write permission) in the descriptor
    → Hardware does NOT generate a Permission Fault
    → The change from AP[0]=1→0 serves as the "dirty" indicator

Detecting dirty: check if AP[0] was cleared by hardware
  pte_dirty(pte): returns true if AP[0] changed from initial value

Linux DBM usage (arch/arm64/include/asm/pgtable.h):
  PTE_WRITE  = PTE_DBM  (on DBM-capable systems, write enabled = DBM set)
  
  Hardware writes clear PTE_DBM by setting AP[0]=1 (meaning read-only on its own,
  but AP[0]=1 combined with DBM=1 triggers the hardware dirty mechanism)
```

### DBM vs Software Dirty Tracking

```c
// arch/arm64/include/asm/pgtable.h
#ifdef CONFIG_ARM64_HW_AFDBM
static inline int pte_dirty(pte_t pte)
{
    // Hardware DBM: page is dirty if DBM=1 and AP[1:0]=0b11 (was RW, now write was captured)
    return !!(pte_val(pte) & PTE_DIRTY);
}
#else
static inline int pte_dirty(pte_t pte)
{
    // Software dirty: check the software dirty bit
    return !!(pte_val(pte) & PTE_DIRTY);
}
#endif
```

---

## 9. Cont [52] — Contiguous Hint

```
Purpose: Allows TLB to cache multiple consecutive page descriptors as a single entry.

Requirements:
  - For 4KB pages: 16 consecutive PTEs covering 64KB (16 × 4KB)
    All 16 must have Cont=1, map PA range starting at 64KB-aligned address,
    and have identical attributes (same AttrIndx, AP, SH, etc.)
    
  - For 2MB blocks (L2): 16 consecutive PMD entries covering 32MB
  - For 1GB blocks (L1): 16 consecutive PUD entries covering 16GB

TLB result: Instead of 16 separate 4KB TLB entries, one "contig TLB entry" = 64KB
  → 16× reduction in TLB entries for densely-mapped contiguous regions

Linux uses Cont hint for:
  - Kernel linear map (arch/arm64/mm/mmu.c: use_1G_block/cont_hint checks)
  - Large file-backed mappings (set_pte_range with cont hint on aligned regions)

Warning: ALL 16 entries must be cleared together when unmapping:
  Cannot modify one entry in a contiguous group — must clear all 16
  (partial modification = UNPREDICTABLE; TLB may cache wrong PA)
```

---

## 10. PXN [53] — Privileged Execute Never

```
PXN=0: EL1 (kernel) can fetch instructions from this page
PXN=1: EL1 cannot fetch instructions (Permission Fault if attempted)

Linux usage:
  All user pages: PXN=0 (but UXN=1 is usually also set for user-controlled pages)
  Data pages (kernel heap, stacks, vmalloc data): PXN=1
  Kernel code (.text): PXN=0

WXN (Write XOR No-execute): SCTLR_EL1.WXN=1 forces PXN=1 for all writable pages.
  Enabled in Linux for kernel pages: prevents writing AND executing the same page.
```

---

## 11. UXN/XN [54] — User Execute Never

```
UXN=0: EL0 (user) can fetch instructions from this page
UXN=1: EL0 cannot fetch instructions (Permission Fault if attempted)

Linux usage:
  Kernel pages: UXN=1 (user cannot execute kernel memory)
  User code pages (.text): UXN=0
  User data pages (heap, stack, mmap anon): UXN=1 (W^X policy)

PROT_EXEC: Only user code pages with mmap PROT_EXEC get UXN=0.
  mmap(PROT_READ|PROT_WRITE) → UXN=1 (no execute)
  mmap(PROT_READ|PROT_EXEC)  → UXN=0, AP[1]=1 (read-only), PXN=0
```

---

## 12. Software Use Bits [58:55]

```
4 bits available for OS software use:
  Bit[55] = PTE_DIRTY     — Software dirty tracking (without DBM hardware)
  Bit[56] = PTE_SPECIAL   — Special page (e.g., zero page, device page)
  Bit[57] = PTE_PROT_NONE — Page mapped but PROT_NONE (for mprotect/userfaultfd)
  Bit[58] = PTE_DEVMAP    — Page is part of a PFN-mapped device region (DAX)

These bits are purely software — hardware ignores them during the walk.
Linux reads them in software after the page table walk returns the full descriptor.
```

---

## 13. Interview Questions & Answers

**Q1: How does the Contiguous hint improve TLB efficiency, and what are the constraints?**

With `Cont=1` on 16 consecutive 4KB page descriptors covering a 64KB-aligned PA range, the TLB can store all 16 mappings as a single "contiguous TLB entry." This reduces TLB entries by 16×. The constraints: all 16 PTEs must cover a 64KB-aligned PA range (PA[15:0]=0 for the first entry), all must have identical attributes (same AttrIndx, AP, SH, PXN, UXN), and when unmapping, all 16 must be cleared atomically — modifying any single entry in a contiguous group individually is UNPREDICTABLE behavior.

**Q2: Why does Linux use Inner Shareable (SH=0b11) for Normal memory?**

ARM64 SMP systems use a multi-cluster cache hierarchy. Inner Shareable ensures all CPUs in the same inner domain see a coherent memory view — writes from one CPU are visible to all others without explicit cache flushes. If Normal memory were marked Non-Shareable, cache lines could be duplicated across CPUs without coherency, leading to data corruption in SMP scenarios (e.g., kernel data structures modified concurrently). Device memory uses Non-Shareable because device registers don't participate in CPU cache coherency and should not be speculatively accessed.

**Q3: What is the difference between PXN and UXN, and how does the W^X policy use them?**

`PXN` (Privileged Execute Never) prevents EL1 (kernel) from executing instructions from a page. `UXN` (User Execute Never) prevents EL0 (user) from executing. The W^X (Write XOR Execute) policy ensures no page is both writable and executable: user data pages (`PROT_READ|PROT_WRITE`, no `PROT_EXEC`) get `UXN=1` so user code cannot execute them. Code pages (`PROT_READ|PROT_EXEC`) get `AP[0]=1` (read-only) and `UXN=0`. The combination prevents classic shellcode injection: even if an attacker writes malicious bytes to a data page (heap, stack), the page cannot be executed.

---

## 14. Quick Reference

| Field | Bits | Values | Purpose |
|---|---|---|---|
| V | [0] | 0/1 | Valid (1=mapped) |
| T | [1] | 0/1 | Block(0) / Table or Page(1) |
| AttrIndx | [4:2] | 0-7 | MAIR_EL1 memory type index |
| NS | [5] | 0/1 | Non-Secure (TrustZone) |
| AP | [7:6] | 00-11 | Access permissions |
| SH | [9:8] | 00/10/11 | Non/Outer/Inner Shareable |
| AF | [10] | 0/1 | Access Flag (reclaim hint) |
| nG | [11] | 0/1 | Non-global (ASID-tagged) |
| Cont | [52] | 0/1 | Contiguous hint (16 entries) |
| PXN | [53] | 0/1 | Privileged Execute Never |
| UXN | [54] | 0/1 | User Execute Never |
| SW | [58:55] | — | Software-defined |
| DBM | [51] | 0/1 | Hardware Dirty Bit Mgmt (v8.1+) |
