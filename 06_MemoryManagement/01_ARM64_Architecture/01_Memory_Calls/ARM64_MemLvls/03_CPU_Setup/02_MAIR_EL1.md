# MAIR_EL1 — Memory Attribute Indirection Register

**Source:** `arch/arm64/mm/proc.S`, `arch/arm64/include/asm/memory.h`

## Purpose

MAIR_EL1 defines **8 memory attribute slots** (indices 0–7). Page table entries don't contain the full memory attribute — they contain a 3-bit **index** into MAIR. This indirection allows changing memory attributes for all pages of a given type by modifying a single register.

## How It Works

```
Page Table Entry (PTE):
┌────────────────────────────────────────────────┐
│ ... │ AttrIndx[2:0] │ ... │ Physical Address │
│     │  (3 bits)      │     │                  │
└─────┴───────┬────────┴─────┴──────────────────┘
              │
              ▼ index into MAIR_EL1
┌──────┬──────┬──────┬──────┬──────┬──────┬──────┬──────┐
│Attr7 │Attr6 │Attr5 │Attr4 │Attr3 │Attr2 │Attr1 │Attr0 │
│[63:56]│[55:48]│[47:40]│[39:32]│[31:24]│[23:16]│[15:8] │[7:0] │
└──────┴──────┴──────┴──────┴──────┴──────┴──────┴──────┘
         MAIR_EL1 (64 bits = 8 × 8-bit attribute fields)
```

## Linux Kernel MAIR Attribute Map

```c
// From arch/arm64/include/asm/memory.h
#define MT_DEVICE_nGnRnE    0    // Device, non-Gathering, non-Reordering, no Early-ack
#define MT_DEVICE_nGnRE     1    // Device, non-Gathering, non-Reordering, Early-ack
#define MT_DEVICE_GRE       2    // Device, Gathering, Reordering, Early-ack
#define MT_NORMAL_NC        3    // Normal, Non-Cacheable
#define MT_NORMAL           4    // Normal, Write-Back cacheable
#define MT_NORMAL_WT        5    // Normal, Write-Through cacheable
#define MT_NORMAL_TAG       6    // Normal, Tagged (MTE)
```

| Index | Name | Encoding | Used For |
|-------|------|----------|----------|
| 0 | `MT_DEVICE_nGnRnE` | `0x00` | MMIO registers — strictest ordering |
| 1 | `MT_DEVICE_nGnRE` | `0x04` | MMIO with early write acknowledgment |
| 2 | `MT_DEVICE_GRE` | `0x0C` | MMIO allowing gathering and reordering |
| 3 | `MT_NORMAL_NC` | `0x44` | Normal memory, not cached (DMA buffers) |
| 4 | `MT_NORMAL` | `0xFF` | Regular RAM — **most kernel/user memory** |
| 5 | `MT_NORMAL_WT` | `0xBB` | Write-through cacheable |
| 6 | `MT_NORMAL_TAG` | `0xF0` | MTE-tagged normal memory |

## Attribute Encoding (8-bit field)

Each 8-bit attribute field is split into outer and inner cache policies:

```
Attr[7:4] = Outer cache attribute
Attr[3:0] = Inner cache attribute

For Normal memory (Attr[3:0] != 0x0):
  0b0000 = (reserved, use Device encoding)
  0b0100 = Non-cacheable
  0b1011 = Write-Through, Read-Allocate, Write-Allocate
  0b1111 = Write-Back, Read-Allocate, Write-Allocate

For Device memory (Attr[3:0] == 0x0):
  0b0000_0000 = Device-nGnRnE
  0b0000_0100 = Device-nGnRE
  0b0000_1000 = Device-nGRE
  0b0000_1100 = Device-GRE
```

## `MT_NORMAL` (0xFF) Breakdown

```
0xFF = 0b1111_1111
       ├── Outer: 0b1111 = Write-Back, Read-Allocate, Write-Allocate
       └── Inner: 0b1111 = Write-Back, Read-Allocate, Write-Allocate
```

This is the most performant setting: both inner and outer caches use write-back with full allocation. Used for all regular kernel and user memory.

## `MT_DEVICE_nGnRnE` (0x00) Breakdown

```
0x00 = 0b0000_0000
       Device memory: No Gathering, No Reordering, No Early-write-ack
```

The strictest device memory type:
- **No Gathering**: Each access goes individually to the device (no merging)
- **No Reordering**: Accesses arrive in program order
- **No Early-ack**: Write not acknowledged until device confirms receipt

Used for MMIO registers where ordering and individual access matters.

## How PTEs Reference MAIR

When the kernel creates a page table entry:

```c
// PAGE_KERNEL uses MT_NORMAL (index 4):
pgprot_t PAGE_KERNEL = PTE_ATTRINDX(MT_NORMAL) | PTE_SHARED | ...;
//                     ^^^^^^^^^^^^^^^^^^^^
//                     Sets AttrIndx = 4 in the PTE

// PAGE_KERNEL_DEVICE uses MT_DEVICE_nGnRnE (index 0):
pgprot_t PAGE_KERNEL_DEVICE = PTE_ATTRINDX(MT_DEVICE_nGnRnE) | ...;
//                            Sets AttrIndx = 0 in the PTE
```

## Key Takeaway

MAIR_EL1 is a lookup table for memory attributes. It decouples the page table entry format from the actual cache/device policy. The kernel defines 7 attribute types covering everything from strict device MMIO to fully cacheable RAM. Every PTE carries a 3-bit index into this table, determining how that page's memory accesses behave in the cache hierarchy.
