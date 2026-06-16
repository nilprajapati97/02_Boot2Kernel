# Memory Model — ARMv8-A

## 1. ARM Memory Types

ARMv8 defines two fundamental memory types, configured via page table attributes:

```
┌──────────────────────────────────────────────────────────────────┐
│                     Memory Types                                  │
│                                                                    │
│  ┌─────────────────────────┐  ┌─────────────────────────────┐   │
│  │       Normal             │  │         Device               │   │
│  │                          │  │                               │   │
│  │  • DRAM, SRAM            │  │  • MMIO registers             │   │
│  │  • Cacheable (WB/WT)     │  │  • NOT cacheable              │   │
│  │  • Speculative access OK │  │  • NO speculative access      │   │
│  │  • Can be reordered      │  │  • Side-effect sensitive      │   │
│  │  • Can be merged/split   │  │  • Access size preserved      │   │
│  │  • Prefetchable          │  │  • NO merging/splitting       │   │
│  └─────────────────────────┘  └─────────────────────────────┘   │
└──────────────────────────────────────────────────────────────────┘
```

### Normal Memory Attributes

```
Cacheability:
  ┌────────────────────────────────────────────────────────────────┐
  │  Attribute         Abbreviation    Description                 │
  ├────────────────────────────────────────────────────────────────┤
  │  Write-Back        WB              Data written to cache only; │
  │                                    memory updated later        │
  │                                    (most common for DRAM)      │
  │                                                                │
  │  Write-Through     WT              Data written to cache AND   │
  │                                    memory simultaneously       │
  │                                                                │
  │  Non-Cacheable     NC              Bypasses cache entirely     │
  │                                    (e.g., DMA buffers)         │
  └────────────────────────────────────────────────────────────────┘

Allocation policies:
  • Read-Allocate (RA): Allocate cache line on read miss
  • Write-Allocate (WA): Allocate cache line on write miss
  • No-allocate: Don't allocate on miss (streaming data)

Typical DRAM config: Normal, WB, RA+WA (Inner + Outer)
```

### Device Memory Types

```
┌───────────────────────────────────────────────────────────────────┐
│  Type        Gathering  Reordering  Early Write Ack              │
├───────────────────────────────────────────────────────────────────┤
│  Device-nGnRnE    No         No          No                     │
│  (most strict)     Cannot merge, reorder, or acknowledge early   │
│                    Use for: interrupt controllers, timers         │
│                                                                   │
│  Device-nGnRE     No         No          Yes                    │
│                    Write may be ack'd before reaching device     │
│                                                                   │
│  Device-nGRE      No         Yes         Yes                    │
│                    Accesses to same device can be reordered      │
│                                                                   │
│  Device-GRE       Yes        Yes         Yes                    │
│  (most relaxed)    Multiple accesses can be merged               │
│                    Use for: frame buffers, bulk MMIO              │
└───────────────────────────────────────────────────────────────────┘

  G = Gathering (merging multiple accesses)
  R = Reordering (changing access order)
  E = Early write acknowledgement
  n = "not" (prefix negation)
```

---

## 2. MAIR_EL1 — Memory Attribute Indirection Register

Page table entries don't store memory attributes directly. Instead, they contain an
**index** (AttrIndx[2:0]) into MAIR_EL1, which holds up to 8 attribute definitions.

```
MAIR_EL1 (64-bit register):
┌────────┬────────┬────────┬────────┬────────┬────────┬────────┬────────┐
│ Attr7  │ Attr6  │ Attr5  │ Attr4  │ Attr3  │ Attr2  │ Attr1  │ Attr0  │
│[63:56] │[55:48] │[47:40] │[39:32] │[31:24] │[23:16] │[15:8]  │[7:0]   │
└────────┴────────┴────────┴────────┴────────┴────────┴────────┴────────┘

Each Attr field is 8 bits:
  Bits [7:4] = Outer attribute
  Bits [3:0] = Inner attribute

  Normal memory encoding:
    0b0000 = Non-cacheable
    0b0100 = Write-Through, no allocate
    0b0101 = Write-Through, read-allocate
    0b0110 = Write-Through, write-allocate
    0b0111 = Write-Through, read+write allocate
    0b1000 = Write-Back, no allocate
    0b1001 = Write-Back, read-allocate
    0b1010 = Write-Back, write-allocate
    0b1011 = Write-Back, read+write allocate

  Device memory encoding (Attr[7:4] = 0b0000):
    0b00000000 = Device-nGnRnE
    0b00000100 = Device-nGnRE
    0b00001000 = Device-nGRE
    0b00001100 = Device-GRE

Typical Linux setup:
  Attr0 = 0xFF → Normal, WB, RA+WA (Inner & Outer) → for DRAM
  Attr1 = 0x00 → Device-nGnRnE → for MMIO
  Attr2 = 0x44 → Normal, WT, RA → for specific regions
  Attr3 = 0x04 → Device-nGnRE → for PCIe config space
```

---

## 3. Shareability Domains

Shareability defines which observers (cores, GPUs, DMA) must see a coherent view
of memory:

```
┌──────────────────────────────────────────────────────────────────┐
│                    Shareability Domains                           │
│                                                                    │
│   ┌────────────────────────────────────────────────────────┐     │
│   │                Full System                              │     │
│   │                                                          │     │
│   │   ┌────────────────────────────────────────────┐        │     │
│   │   │            Outer Shareable (OSH)            │        │     │
│   │   │                                              │        │     │
│   │   │   ┌────────────────────────────┐            │        │     │
│   │   │   │    Inner Shareable (ISH)    │            │        │     │
│   │   │   │                              │            │        │     │
│   │   │   │  Core0  Core1  Core2  Core3 │            │        │     │
│   │   │   │  (same cluster or SoC)       │            │        │     │
│   │   │   └────────────────────────────┘            │        │     │
│   │   │                                              │        │     │
│   │   │   GPU, DMA engines, other masters           │        │     │
│   │   └────────────────────────────────────────────┘        │     │
│   │                                                          │     │
│   │   Remote memory (NUMA, ...)                              │     │
│   └────────────────────────────────────────────────────────┘     │
│                                                                    │
│   Non-Shareable (NSH): Local to a single PE, not coherent         │
└──────────────────────────────────────────────────────────────────┘

In page tables:
  SH[1:0] encoding:
    00 = Non-Shareable
    01 = Reserved
    10 = Outer Shareable
    11 = Inner Shareable

Most OS kernels map everything as Inner Shareable for simplicity.
```

---

## 4. Address Spaces

### Virtual Address Space Layout (AArch64)

```
                                                    TTBR1_EL1
  0xFFFF_FFFF_FFFF_FFFF  ┌───────────────────┐  ◄── Kernel space
                          │  Kernel virtual    │      (high addresses)
                          │  address range      │
                          │  (uses TTBR1_EL1)   │
  0xFFFF_0000_0000_0000  ├───────────────────┤
                          │                     │
                          │  ███████████████    │  ◄── Hole (invalid)
                          │  (unmapped gap)     │      Causes fault
                          │                     │
  0x0000_FFFF_FFFF_FFFF  ├───────────────────┤
                          │  User virtual       │
                          │  address range      │      TTBR0_EL1
                          │  (uses TTBR0_EL1)   │  ◄── User space
  0x0000_0000_0000_0000  └───────────────────┘      (low addresses)

With 48-bit VA (default):
  User range:   0x0000_0000_0000_0000 — 0x0000_FFFF_FFFF_FFFF (256 TB)
  Kernel range: 0xFFFF_0000_0000_0000 — 0xFFFF_FFFF_FFFF_FFFF (256 TB)

With 52-bit VA (ARMv8.2 LVA):
  User range:   0 — 0x000F_FFFF_FFFF_FFFF (4 PB)
  Kernel range: 0xFFF0_0000_0000_0000 — 0xFFFF_FFFF_FFFF_FFFF (4 PB)
```

### Physical Address Space

```
  ARMv8.0: 48-bit PA → 256 TB physical address space
  ARMv8.2: 52-bit PA → 4 PB physical address space (with LPA)

  Physical address map (typical SoC):
  ┌─────────────────────────────────────────────┐
  │  0x00_0000_0000 — 0x00_3FFF_FFFF           │  Boot ROM
  │  0x00_4000_0000 — 0x00_7FFF_FFFF           │  MMIO (peripherals)
  │  0x00_8000_0000 — 0x0F_FFFF_FFFF           │  DRAM (main memory)
  │  0x10_0000_0000 — 0xFF_FFFF_FFFF           │  More DRAM / PCIe   
  └─────────────────────────────────────────────┘
```

---

## 5. Endianness

ARMv8-A supports both Little-Endian (LE) and Big-Endian (BE):

```
  Word: 0x12345678 at address 0x1000

  Little-Endian (default, most common):
  Address:  0x1000  0x1001  0x1002  0x1003
  Content:   0x78    0x56    0x34    0x12
             (LSB first)

  Big-Endian:
  Address:  0x1000  0x1001  0x1002  0x1003
  Content:   0x12    0x34    0x56    0x78
             (MSB first)

  Controlled by:
    SCTLR_EL1.EE  — Exception endianness (EL1+ data)
    SCTLR_EL1.E0E — EL0 data endianness
    
  AArch64 instruction fetch is ALWAYS little-endian.
  Page table walks are ALWAYS little-endian.
```

---

## 6. Memory Tagging Extension — MTE (ARMv8.5)

MTE helps detect memory safety bugs (use-after-free, buffer overflow):

```
  ┌──────────────────────────────────────────────────────────────┐
  │  How MTE Works:                                               │
  │                                                                │
  │  1. Memory Allocation:                                        │
  │     malloc(64) → returns pointer with TAG in bits [59:56]     │
  │     Physical memory at that address gets tagged with same tag │
  │                                                                │
  │  2. Tagged Pointer:                                            │
  │     ┌──────┬────────────────────────────────────────┐         │
  │     │ Tag  │  Virtual Address (bits 55:0)           │         │
  │     │[59:56]│                                       │         │
  │     │ 0x5  │  0x0000_DEAD_BEEF_0000                 │         │
  │     └──────┴────────────────────────────────────────┘         │
  │                                                                │
  │  3. On every load/store:                                      │
  │     Hardware compares: pointer tag vs memory tag              │
  │     Match → access allowed                                    │
  │     Mismatch → Tag Check Fault (sync or async)                │
  │                                                                │
  │  4. After free(ptr):                                          │
  │     Memory tag changed to different value                     │
  │     Old pointer still has old tag → mismatch → FAULT          │
  │     → Use-after-free detected!                                │
  └──────────────────────────────────────────────────────────────┘
```

---

Next: [Virtual Memory & Address Translation →](./02_Virtual_Memory.md)
