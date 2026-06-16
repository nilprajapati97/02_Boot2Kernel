# MAIR_EL1: Memory Attribute Indirection Register — Complete Reference

**Category**: Memory Attributes MAIR_EL1  
**Targeted**: ARM, Qualcomm, NVIDIA, AMD

---

## 1. Why MAIR_EL1 Exists: The Indirection Problem

ARM64 supports many different memory types (Normal WB, Normal WT, Normal NC, Device nGnRnE, etc.). Without MAIR, each PTE would need to encode the full memory type directly. That would require ~8 bits per PTE for memory type — consuming precious descriptor space.

```
Problem without MAIR:
  Encode memory type directly in each PTE: needs 8 bits
  × 512 entries per page table = 4096 bits = 512 bytes per table just for type
  × thousands of page tables per process = megabytes wasted on type encoding

ARM solution: Indirection through MAIR_EL1

MAIR_EL1 = Memory Attribute Indirection Register (EL1)
  Contains 8 × 8-bit "slots" (attribute entries)
  Each slot holds one complete memory attribute encoding (8 bits)
  PTE uses AttrIndx[2:0] (3 bits) to select one of 8 slots

PTE AttrIndx → index into MAIR_EL1 table → 8-bit memory attribute → cache policy

Result:
  PTE only needs 3 bits (AttrIndx) to specify memory type
  vs. 8 bits for direct encoding
  MAIR is set once at boot, shared by all processes
```

---

## 2. MAIR_EL1 Register Layout

```
MAIR_EL1 (64-bit register):

  Bits[7:0]   = Attr0 (slot 0) — 8-bit attribute for AttrIndx=0
  Bits[15:8]  = Attr1 (slot 1) — 8-bit attribute for AttrIndx=1
  Bits[23:16] = Attr2 (slot 2) — 8-bit attribute for AttrIndx=2
  Bits[31:24] = Attr3 (slot 3)
  Bits[39:32] = Attr4 (slot 4)
  Bits[47:40] = Attr5 (slot 5)
  Bits[55:48] = Attr6 (slot 6)
  Bits[63:56] = Attr7 (slot 7) — 8-bit attribute for AttrIndx=7

Each 8-bit attribute encodes:
  Bits[7:4] = Outer attributes (outer cache / memory type)
  Bits[3:0] = Inner attributes (inner cache / memory type)

For Device memory: the full 8 bits encode Device type, not inner/outer split.
```

---

## 3. Normal Memory Encoding: Outer + Inner

```
For Normal memory (non-Device):
  Each nibble (4 bits) encodes a cache policy:

  Nibble encoding (for both outer [7:4] and inner [3:0]):
    0b0000 = Non-Cacheable (NC) — Normal Memory, Non-Cacheable
    0b0100 = Write-Through, Read-Allocate, Non-Write-Allocate (WT RA)
    0b0101 = Write-Through, Read-Allocate, Write-Allocate (WT RA WA) — ARMv8.4
    0b0110 = Write-Through, Read-Allocate, Write-Allocate
    0b0111 = Write-Through, Read-Allocate, Non-Write-Allocate
    0b1000 = Write-Back, Non-Allocate (WB no-alloc, same as 0b1100)
    0b1001 = Write-Back, Read-Allocate (WB RA, Write-Through on hit) — Unusual
    0b1010 = Write-Back, Write-Allocate (WB WA) — less common
    0b1011 = Write-Back, Read/Write-Allocate
    0b1100 = Write-Back, Non-Allocate
    0b1101 = Write-Back, Read-Allocate, Non-Write-Allocate (WB RA)
    0b1110 = Write-Back, Read-Allocate, Write-Allocate (WB RA WA)
    0b1111 = Write-Back, Read-Allocate, Write-Allocate (WB RA WA)

Most common Normal memory encodings:
  Fully-cached WB RA WA (inner and outer):
    Outer = 0b1111, Inner = 0b1111
    8-bit attribute = 0xFF
    → Full write-back, read and write allocate, both L1/L2 and LLC

  Non-Cacheable Normal memory:
    Outer = 0b0100 (Non-Cacheable), Inner = 0b0100 (Non-Cacheable)
    8-bit attribute = 0x44
    → No caching at any level (useful for DMA bounce buffers, uncached IO)
```

---

## 4. Device Memory Encoding

```
Device memory uses a DIFFERENT encoding scheme from Normal memory.
Device memory: bits[7:4] = 0b0000 (marks it as Device type)
               bits[3:0] = Device type sub-encoding

Device Types (bits[3:0] when bits[7:4] = 0b0000):
  0b0000 = nGnRnE (non-Gathering, non-Reordering, no Early Write Ack)
  0b0100 = nGnRE  (non-Gathering, non-Reordering, Early Write Ack)
  0b1000 = nGRE   (non-Gathering, Reordering allowed, Early Write Ack)
  0b1100 = GRE    (Gathering, Reordering, Early Write Ack)

G = Gathering: multiple small accesses may be merged into one larger transaction
R = Reordering: accesses to the same device may be reordered by hardware
E = Early Write Acknowledgment: write is acknowledged before reaching device

Device memory complete attribute byte:
  nGnRnE = 0b0000_0000 = 0x00 (strongest ordering, no merging, no reordering)
  nGnRE  = 0b0000_0100 = 0x04
  nGRE   = 0b0000_1000 = 0x08
  GRE    = 0b0000_1100 = 0x0C

In practice:
  Memory-mapped registers (UART, GPIO, interrupt controller):
    Must be nGnRnE (0x00) — each access must reach the device in order,
    cannot be merged (no gathering), cannot be reordered
    A read of a UART status register must read the ACTUAL current value

  PCIe MMIO (standard):
    nGnRnE or nGnRE — device ordering usually required

  Write-combining memory (frame buffers, GPU BARs):
    GRE or nGRE — gathering and reordering allowed for throughput
    GPU frame buffer: individual pixel writes can be merged (gathering)
    into cache-line-sized bus transactions for efficiency
```

---

## 5. Linux MAIR_EL1 Configuration

```c
// arch/arm64/include/asm/pgtable-hwdef.h

// MAIR attribute index definitions:
#define MT_DEVICE_nGnRnE    0   // AttrIndx=0: Device nGnRnE
#define MT_DEVICE_nGnRE     1   // AttrIndx=1: Device nGnRE
#define MT_DEVICE_GRE       2   // AttrIndx=2: Device GRE
#define MT_NORMAL_NC        3   // AttrIndx=3: Normal Non-Cacheable
#define MT_NORMAL           4   // AttrIndx=4: Normal WB RA WA (fully cached)
#define MT_NORMAL_WT        5   // AttrIndx=5: Normal Write-Through

// MAIR_EL1 slot values (8-bit attribute bytes):
#define MAIR_ATTRIDX(attr, idx) ((attr) << ((idx) * 8))

#define MAIR_EL1_SET                                               \
  MAIR_ATTRIDX(0x00, MT_DEVICE_nGnRnE) | /* slot 0: 0x00 */     \
  MAIR_ATTRIDX(0x04, MT_DEVICE_nGnRE)  | /* slot 1: 0x04 */     \
  MAIR_ATTRIDX(0x0c, MT_DEVICE_GRE)    | /* slot 2: 0x0c */     \
  MAIR_ATTRIDX(0x44, MT_NORMAL_NC)     | /* slot 3: 0x44 */     \
  MAIR_ATTRIDX(0xff, MT_NORMAL)        | /* slot 4: 0xff */     \
  MAIR_ATTRIDX(0xbb, MT_NORMAL_WT)       /* slot 5: 0xbb */

// AT BOOT (arch/arm64/kernel/head.S):
ldr  x5, =MAIR_EL1_SET
msr  MAIR_EL1, x5
isb

// When creating a PTE for Normal cached memory:
pgprot_t prot = __pgprot(PTE_VALID | PTE_AF | PTE_SHARED |
                          PTE_ATTRINDX(MT_NORMAL) |   // AttrIndx=4
                          PTE_TYPE_PAGE);
// PTE_ATTRINDX(MT_NORMAL) = (4 << 2) = 0b10000 = bits[4:2] of PTE

// For device memory ioremap:
pgprot_t prot = pgprot_noncached(PAGE_KERNEL);
// Sets AttrIndx = MT_DEVICE_nGnRnE (0) in the PTE
```

---

## 6. PTE AttrIndx Field

```
In a page/block descriptor, AttrIndx (bits[4:2]):

  AttrIndx[2:0] = index 0–7 into MAIR_EL1
  
  PTE bits[4:2] are interpreted by hardware as MAIR_EL1 slot index.
  Hardware looks up Attr[AttrIndx] from MAIR_EL1 to determine
  the memory type for the access.

Example PTE bit fields:
  Bit[1:0] = 0b11 (valid page descriptor)
  Bit[2]   = AttrIndx[0]
  Bit[3]   = AttrIndx[1]
  Bit[4]   = AttrIndx[2]
  ...
  Bit[10]  = AF (Access Flag)
  Bit[11]  = nG (non-Global)

  For MT_NORMAL (index 4 = 0b100):
    Bit[4]=1, Bit[3]=0, Bit[2]=0 → AttrIndx = 0b100 = 4 → Attr4 = 0xFF

  For MT_DEVICE_nGnRnE (index 0 = 0b000):
    Bit[4]=0, Bit[3]=0, Bit[2]=0 → AttrIndx = 0b000 = 0 → Attr0 = 0x00
```

---

## 7. Interview Questions & Answers

**Q1: What is MAIR_EL1 and why does ARM64 use indirection for memory attributes?**

`MAIR_EL1` is an 8-slot lookup table where each slot holds an 8-bit memory attribute encoding. Page table entries (PTEs) use a 3-bit `AttrIndx` field to select one of the 8 slots, rather than encoding the full 8-bit memory type directly in each PTE. This saves 5 bits per PTE (3 bits instead of 8). More importantly, it allows one system register to be configured once at boot with all needed memory types, and all subsequent PTE creations simply reference a slot index. The OS can change a system-wide memory policy by updating one MAIR slot rather than walking all page tables. MAIR is written once by the boot code and remains constant throughout Linux operation.

**Q2: What is the encoding for Device nGnRnE and why must device registers use it?**

`nGnRnE` (non-Gathering, non-Reordering, no Early Write Acknowledgment) has MAIR attribute byte = `0x00`. It must be used for device registers because: **non-Gathering** prevents the CPU from combining adjacent accesses (writing two 4-byte registers must generate two separate 4-byte bus transactions, not one 8-byte burst); **non-Reordering** prevents the CPU from reordering accesses (a status register read must happen AFTER a command register write, in order); **no Early Write Ack** means the CPU waits until the device acknowledges the write before proceeding (prevents the CPU from "assuming" the write was received when it's still in a buffer). Violating any of these for device registers causes hardware bugs: merged writes hit the wrong register, reordered reads see stale values, un-acknowledged writes appear lost.

---

## 8. Quick Reference

| Slot | Index | Attribute Byte | Linux Name | Type |
|---|---|---|---|---|
| 0 | 0b000 | 0x00 | MT_DEVICE_nGnRnE | Device (strongest order) |
| 1 | 0b001 | 0x04 | MT_DEVICE_nGnRE | Device (early write ack) |
| 2 | 0b010 | 0x0C | MT_DEVICE_GRE | Device (gather+reorder) |
| 3 | 0b011 | 0x44 | MT_NORMAL_NC | Normal Non-Cacheable |
| 4 | 0b100 | 0xFF | MT_NORMAL | Normal WB RA WA (fully cached) |
| 5 | 0b101 | 0xBB | MT_NORMAL_WT | Normal Write-Through |
| 6–7 | — | — | (unused in Linux) | — |
