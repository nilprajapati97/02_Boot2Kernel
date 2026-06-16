# `__cpu_setup()` — MMU Hardware Register Configuration

**Source:** `arch/arm64/mm/proc.S` lines 483–550
**Phase:** Assembly Boot (MMU OFF)
**Memory Allocator:** None
**Called by:** `primary_entry()` in head.S
**Returns:** x0 = prepared SCTLR_EL1 value (for `__enable_mmu()`)

---

## What This Function Does

Configures the CPU's memory management hardware registers **while the MMU is still off**. These registers define:

1. **MAIR_EL1** — What each memory type means (cacheable, device, write-through, etc.)
2. **TCR_EL1** — How the MMU translates addresses (page size, VA width, cacheability of page walks)
3. **SCTLR_EL1** — System control (MMU enable bit, cache enable, alignment checks)

**No memory is allocated.** This function only writes CPU system registers.

---

## Why This Matters for Memory

Every page table entry (PTE) contains an `AttrIndx` field (bits [4:2]) that indexes into MAIR_EL1 to select a memory type. Without proper MAIR configuration, the PTE attributes created by `create_init_idmap()` would be meaningless.

Similarly, TCR_EL1 tells the MMU how to interpret page table entries — granule size, number of VA bits, cacheability of the page table walk itself.

---

## Step-by-Step Execution

### Step 1: TLB and Cache Invalidation

```asm
tlbi  vmalle1           // Invalidate ALL TLB entries for EL1
dsb   nsh               // Data Synchronization Barrier (non-shareable)
```

**Why:** The TLB (Translation Lookaside Buffer) may contain stale entries from the bootloader. Must be cleared before installing new page tables.

### Step 2: Disable Coprocessor Access

```asm
msr   cpacr_el1, xzr    // Disable FP/SIMD/SVE access traps
mov   x1, MDSCR_EL1_TDCC
msr   mdscr_el1, x1     // Configure debug register
```

Not directly memory-related — prevents unexpected traps during boot.

### Step 3: Configure MAIR_EL1 — Memory Attribute Indirection Register

```asm
mair  .req  x17
mov_q mair, MAIR_EL1_SET
msr   mair_el1, mair
```

**MAIR_EL1** defines 8 memory attribute entries (8 bytes, 64 bits total). Each PTE's `AttrIndx[2:0]` selects one of these 8 entries.

### MAIR_EL1 Attribute Entries

| Index | Name | Value | Description |
|-------|------|-------|-------------|
| 0 | `MT_DEVICE_nGnRnE` | `0x00` | Device memory: no Gathering, no Reordering, no Early write ack |
| 1 | `MT_DEVICE_nGnRE` | `0x04` | Device memory: no Gathering, no Reordering, Early write ack |
| 2 | `MT_DEVICE_GRE` | `0x0C` | Device memory: Gathering + Reordering + Early write ack |
| 3 | `MT_NORMAL_NC` | `0x44` | Normal memory: Non-Cacheable |
| 4 | `MT_NORMAL` | `0xFF` | Normal memory: Write-Back, Read+Write Allocate (Inner + Outer) |
| 5 | `MT_NORMAL_WT` | `0xBB` | Normal memory: Write-Through, Read Allocate |
| 6 | `MT_NORMAL_TAGGED` | `0xF0` | Normal memory with MTE (Memory Tagging Extension) tags |
| 7 | Reserved | — | — |

### How MAIR Encoding Works

Each 8-bit entry encodes memory attributes:

```
Bits [7:4] = Outer attribute
Bits [3:0] = Inner attribute

For Normal memory:
  0b0000 = Non-Cacheable
  0b0100 = Write-Through, Read Allocate, No Write Allocate
  0b1011 = Write-Through, Read+Write Allocate
  0b1111 = Write-Back, Read+Write Allocate  ← Best performance

For Device memory:
  0b0000_00RR where RR selects device type
```

### Example: MT_NORMAL (index 4, value 0xFF)

```
Outer: 0xFF >> 4 = 0xF = Write-Back, Read+Write Allocate
Inner: 0xFF & 0xF = 0xF = Write-Back, Read+Write Allocate

→ Both inner and outer caches use Write-Back with full allocation
→ Best performance for regular RAM
```

### Example: MT_DEVICE_nGnRnE (index 0, value 0x00)

```
Device memory: Strictest ordering
- No Gathering:   CPU cannot merge multiple accesses into one
- No Reordering:  Accesses complete in program order
- No Early ack:   Write completes only when device acknowledges

→ Used for MMIO registers where access order and size matter
```

---

### Step 4: Configure TCR_EL1 — Translation Control Register

```asm
tcr   .req  x16
mov_q tcr, TCR_T0SZ(IDMAP_VA_BITS) | TCR_T1SZ(VA_BITS) | TCR_TG0_FLAGS | ...
```

TCR_EL1 is a 64-bit register that controls how the MMU performs address translation.

### TCR_EL1 Fields

```
┌─────────────────────────────────────────────────────────────────┐
│ [63:60] │ [59] │ [37:36]│ [35:34]│ [31:30]│ [29:28]│ [21:16] │
│ Res     │ DS   │ SH1    │ ORGN1  │ TG1    │ SH0    │ T0SZ    │
├─────────┴──────┴────────┴────────┴────────┴────────┴─────────┤
│ [15:0]  │ [14] │ [13:12]│ [11:10]│ [9:8]  │ [7]    │ [5:0]   │
│ ...     │ TG0  │ SH0    │ ORGN0  │ IRGN0  │ EPD0   │ T0SZ    │
└─────────────────────────────────────────────────────────────────┘
```

### Key TCR Fields

| Field | Bits | Value | Meaning |
|-------|------|-------|---------|
| `T0SZ` | [5:0] | `64 - IDMAP_VA_BITS` | Size of TTBR0 VA region (identity map). E.g., T0SZ=16 → 48-bit VA |
| `T1SZ` | [21:16] | `64 - VA_BITS` | Size of TTBR1 VA region (kernel). E.g., T1SZ=16 → 48-bit VA |
| `TG0` | [15:14] | `0b00` = 4KB | Page granule for TTBR0 region |
| `TG1` | [31:30] | `0b10` = 4KB | Page granule for TTBR1 region |
| `IRGN0` | [9:8] | `0b01` = WB-WA | Inner cacheability of TTBR0 page walks |
| `ORGN0` | [11:10] | `0b01` = WB-WA | Outer cacheability of TTBR0 page walks |
| `IRGN1` | [25:24] | `0b01` = WB-WA | Inner cacheability of TTBR1 page walks |
| `ORGN1` | [27:26] | `0b01` = WB-WA | Outer cacheability of TTBR1 page walks |
| `SH0` | [13:12] | `0b11` = ISH | Shareability of TTBR0 walks (Inner Shareable) |
| `SH1` | [29:28] | `0b11` = ISH | Shareability of TTBR1 walks (Inner Shareable) |
| `IPS` | [34:32] | Auto-detected | Intermediate Physical Address Size |
| `HA` | [39] | 1 if supported | Hardware Access Flag update |
| `HD` | [40] | 1 if supported | Hardware Dirty Bit management |

### Two Translation Regimes

TCR sets up **two independent translation regimes**:

```
Address Range                   TTBR          Page Tables          Purpose
────────────────────────────────────────────────────────────────────────────
0x0000_0000_0000_0000           TTBR0_EL1     init_idmap_pg_dir    User/Identity map
  to 0x0000_FFFF_FFFF_FFFF     (T0SZ controls VA width)

0xFFFF_0000_0000_0000           TTBR1_EL1     init_pg_dir          Kernel
  to 0xFFFF_FFFF_FFFF_FFFF     (T1SZ controls VA width)
```

- **TTBR0** (lower addresses): Initially used for identity mapping, later for user space
- **TTBR1** (upper addresses): Always for kernel space

### Page Table Walk Cacheability

The `IRGN`/`ORGN` fields control whether the MMU's page table walker uses caches when reading page table entries:

```
WB-WA (Write-Back, Write-Allocate):
  The page table walker uses the data cache.
  Page table reads/writes go through cache → FAST.
  Cache coherency maintained by ISH (Inner Shareable).
```

This is why cache maintenance in `primary_entry()` is critical — the walker reads from cache, so page tables must be flushed to cache first.

---

### Step 5: Physical Address Size Detection

```asm
// Read ID_AA64MMFR0_EL1 to detect PA size
mrs   x3, ID_AA64MMFR0_EL1
ubfx  x3, x3, #ID_AA64MMFR0_EL1_PARANGE_SHIFT, #4
// Set TCR.IPS = detected PA size
bfi   tcr, x3, #TCR_IPS_SHIFT, #3
```

The CPU advertises its maximum physical address width in `ID_AA64MMFR0_EL1.PARange`:

| PARange | Physical Address Bits | Addressable Memory |
|---------|----------------------|-------------------|
| 0b0000 | 32 bits | 4 GB |
| 0b0001 | 36 bits | 64 GB |
| 0b0010 | 40 bits | 1 TB |
| 0b0011 | 42 bits | 4 TB |
| 0b0100 | 44 bits | 16 TB |
| 0b0101 | 48 bits | 256 TB |
| 0b0110 | 52 bits | 4 PB |

This is set in `TCR.IPS` so the MMU knows the valid output address range.

---

### Step 6: Prepare SCTLR_EL1

```asm
mov_q x0, INIT_SCTLR_EL1_MMU_ON
ret                              // Return x0 = SCTLR value for __enable_mmu
```

The function does **not** write SCTLR_EL1 directly. It prepares the value in x0 and returns it. `__enable_mmu()` will write it to actually enable the MMU.

### SCTLR_EL1 Key Bits

| Bit | Name | Value | Meaning |
|-----|------|-------|---------|
| [0] | M | 1 | **MMU Enable** |
| [1] | A | 1 | Alignment check enable |
| [2] | C | 1 | **Data cache enable** |
| [12] | I | 1 | **Instruction cache enable** |
| [19] | WXN | 1 | Write permission implies Execute Never (W^X) |
| [25] | EE | 0 | Little-endian data access |
| [44] | DSSBS | 1 | Speculative Store Bypass Safe |

---

## Summary of Register Configuration

```
After __cpu_setup() returns:

MAIR_EL1 = [8 memory attribute entries defined]
           Index 0: Device-nGnRnE    (strictest device)
           Index 4: Normal-WB-WA     (regular RAM)
           ...

TCR_EL1  = [Translation control configured]
           T0SZ: identity map VA bits
           T1SZ: kernel VA bits
           TG0/TG1: 4KB granule
           IPS: auto-detected PA size
           Walk cacheability: WB-WA, Inner Shareable

x0       = [Prepared SCTLR_EL1 value]
           M=1 (MMU ON), C=1 (D-cache ON), I=1 (I-cache ON)
           WXN=1 (Write implies no-Execute)

TTBR0/TTBR1 = NOT YET LOADED (done by __enable_mmu)
```

---

## Key Takeaways

1. **MAIR defines the vocabulary** — every PTE's memory type is just an index into MAIR
2. **TCR defines the grammar** — page size, VA width, cacheability of walks
3. **SCTLR is the on switch** — but not written here (deferred to `__enable_mmu`)
4. **Page table walks use caches** — this is why cache maintenance before MMU-on is critical
5. **Two VA spaces** — TTBR0 (lower, identity/user) and TTBR1 (upper, kernel) are independently configured
6. **W^X is enforced in hardware** — SCTLR.WXN=1 means writable pages are automatically non-executable
