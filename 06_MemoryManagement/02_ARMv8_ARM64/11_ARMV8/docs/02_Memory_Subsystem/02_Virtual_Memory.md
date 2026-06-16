# Virtual Memory & Address Translation

## 1. Address Translation Overview

Virtual memory provides each process with its own isolated address space.
The MMU translates Virtual Addresses (VA) to Physical Addresses (PA) using
**page tables** stored in memory.

```
  ┌──────────────────┐
  │  Virtual Address │  (from CPU instruction)
  └────────┬─────────┘
           │
  ┌────────▼─────────┐
  │       TLB        │  Fast cache of recent translations
  └────┬────────┬────┘
       │ HIT    │ MISS
       │        │
       │   ┌────▼──────────────┐
       │   │  Page Table Walk  │  Hardware walker traverses tables
       │   │  (in memory)      │  in DRAM
       │   └────────┬──────────┘
       │            │
  ┌────▼────────────▼────┐
  │   Physical Address   │  (sent to cache / memory controller)
  └──────────────────────┘
```

---

## 2. Translation Granules

ARMv8 supports three page sizes (translation granules):

```
┌──────────────┬───────────┬──────────────┬────────────────┐
│  Granule     │ Page Size │ Table Levels │ VA Bits        │
├──────────────┼───────────┼──────────────┼────────────────┤
│  4 KB        │ 4 KB      │ 4 (L0→L3)    │ 48 (or 52)     │
│  16 KB       │ 16 KB     │ 4 (L0→L3)    │ 48 (or 52)     │
│  64 KB       │ 64 KB     │ 3 (L1→L3)    │ 48 (or 52)     │
└──────────────┴───────────┴──────────────┴────────────────┘

  4 KB pages: Most common (Linux default on most ARM64 platforms)
  16 KB pages: Apple Silicon (macOS/iOS) uses 16 KB
  64 KB pages: Some server configurations
```

---

## 3. Page Table Walk — 4 KB Granule (Most Common)

With 4 KB granule and 48-bit VA, translation uses **4 levels** (L0 → L3):

```
48-bit Virtual Address decomposition:
┌─────────┬─────────┬─────────┬─────────┬──────────────┐
│ L0 idx  │ L1 idx  │ L2 idx  │ L3 idx  │ Page Offset  │
│ [47:39] │ [38:30] │ [29:21] │ [20:12] │   [11:0]     │
│ 9 bits  │ 9 bits  │ 9 bits  │ 9 bits  │  12 bits     │
│(512 ent)│(512 ent)│(512 ent)│(512 ent)│(4096 bytes)  │
└─────────┴─────────┴─────────┴─────────┴──────────────┘

Address space covered by each level:
  L0 entry → 512 GB (2^39)     Each L0 entry points to one L1 table
  L1 entry → 1 GB   (2^30)     Each L1 entry points to one L2 table
  L2 entry → 2 MB   (2^21)     Each L2 entry points to one L3 table
  L3 entry → 4 KB   (2^12)     Each L3 entry maps one 4 KB page
```

### Walk Process Step-by-Step

```
Example: Translate VA = 0x0000_7F80_1234_5678

Step 0: Get base address from TTBR0_EL1 (or TTBR1_EL1 for kernel)
        TTBR0_EL1 → Physical address of L0 table

Step 1: L0 Lookup
        Index = VA[47:39] = 0x0FF (bits 47:39 of 0x7F8012345678)
        Table entry address = TTBR + (index × 8)
        Read L0 descriptor → contains PA of L1 table

Step 2: L1 Lookup
        Index = VA[38:30] = 0x000
        Table entry address = L1_base + (index × 8)
        Read L1 descriptor → contains PA of L2 table
        (OR: L1 block descriptor → 1 GB huge page, done!)

Step 3: L2 Lookup
        Index = VA[29:21] = 0x091
        Table entry address = L2_base + (index × 8)
        Read L2 descriptor → contains PA of L3 table
        (OR: L2 block descriptor → 2 MB huge page, done!)

Step 4: L3 Lookup
        Index = VA[20:12] = 0x345
        Table entry address = L3_base + (index × 8)
        Read L3 page descriptor → PA of 4 KB page

Step 5: Combine
        Physical Address = PA_from_L3[47:12] | VA[11:0]
                        = PA_base | 0x678
```

### Visual Walk

```
TTBR0_EL1
    │
    ▼
┌──────────────────────┐
│   L0 Table           │
│  (512 entries × 8B)  │
│  [0] → ...           │
│  [0xFF] → L1 base ───┼───┐
│  [511] → ...         │   │
└──────────────────────┘   │
                           │
                    ┌──────▼────────────────┐
                    │   L1 Table            │
                    │  [0] → L2 base ───────┼──┐
                    │  [1] → ...            │  │
                    │  [511] → ...          │  │
                    └───────────────────────┘  │
                                               │
                         ┌─────────────────────▼──┐
                         │   L2 Table             │
                         │  [0x91] → L3 base ─────┼──┐
                         │  ...                   │  │
                         └────────────────────────┘  │
                                                     │
                              ┌────────────────────────▼──┐
                              │   L3 Table                │
                              │  [0x345] → PA 0x8000_0000 │
                              │  → PA + offset = final PA │
                              └───────────────────────────┘
```

---

## 4. Page Table Descriptors (4 KB Granule)

Each table entry (descriptor) is **8 bytes (64 bits)**:

### L0/L1/L2 Table Descriptor (points to next-level table)

```
63  62:59  58:52   51  50  49:48  47:12           11:2     1  0
┌───┬──────┬──────┬───┬───┬──────┬─────────────────┬────────┬──┬──┐
│NST│ res  │ res  │res│res│ res  │ Next-level      │ Ignored│ 1│ 1│
│   │      │      │   │   │      │ table PA[47:12] │        │  │  │
└───┴──────┴──────┴───┴───┴──────┴─────────────────┴────────┴──┴──┘
                                                             11 = Table

Bits [63:59] can hold table attributes:
  NSTable, APTable, UXNTable, PXNTable
  These restrict permissions for ALL entries in the next-level table
```

### L1/L2 Block Descriptor (maps a huge page: 1 GB or 2 MB)

```
63  54  53  52  51  50  49:48  47:n          n-1:21/30  20:12  11:2  1  0
┌───┬───┬───┬───┬───┬───┬──────┬─────────────┬──────────┬────────┬───────┬──┬──┐
│ - │PXN│UXN│ - │GP │DBM│Contig│ Output PA   │ res0     │nG,AF   │SH,AP  │ 0│ 1│
│   │   │   │   │   │   │      │ [47:n]      │          │AttrIdx │MemAttr│  │  │
└───┴───┴───┴───┴───┴───┴──────┴─────────────┴──────────┴────────┴───────┴──┴──┘
                                                                        01 = Block

Key attribute bits:
  AttrIndx[2:0] — Index into MAIR_EL1 (memory type)
  AP[2:1]       — Access Permission
  SH[1:0]       — Shareability
  AF            — Access Flag (set by HW on first access)
  nG            — Not Global (ASID-specific)
  PXN           — Privileged Execute Never
  UXN           — Unprivileged Execute Never
  DBM           — Dirty Bit Modifier (HW dirty tracking)
  Contig        — Contiguous hint (for TLB efficiency)
```

### L3 Page Descriptor (maps a 4 KB page)

```
Same format as Block descriptor but bits [1:0] = 11 (Page)
and output address is PA[47:12] (4 KB aligned)
```

### Invalid Descriptor

```
Bit [0] = 0  → Invalid/unmapped → causes Translation Fault
```

---

## 5. Access Permissions

```
┌──────────┬────────────────────┬─────────────────────┐
│ AP[2:1]  │  EL1 (Kernel)      │  EL0 (User)         │
├──────────┼────────────────────┼─────────────────────┤
│  00      │  Read/Write        │  No access          │
│  01      │  Read/Write        │  Read/Write         │
│  10      │  Read-Only         │  No access          │
│  11      │  Read-Only         │  Read-Only          │
└──────────┴────────────────────┴─────────────────────┘

Execute permissions (separate from R/W):
  PXN = 1 → Cannot execute at EL1+ (kernel no-exec)
  UXN = 1 → Cannot execute at EL0 (user no-exec)
  
  Typical: Code pages have PXN=0 (or UXN=0), data pages have both =1
           (W^X: writable pages are not executable, and vice versa)
```

---

## 6. Address Space Identifiers (ASID)

ASID avoids TLB flushes on context switch:

```
Without ASID:
  Process A runs → TLB filled with A's translations
  Context switch to B → MUST flush entire TLB
  Process B runs → TLB is cold, many misses (slow!)

With ASID:
  Process A runs → TLB entries tagged with ASID=1
  Context switch to B → Just change TTBR0_EL1 (ASID=2)
  Process B runs → TLB entries for ASID=1 ignored, B fills TLB
  Context switch back to A → A's entries still in TLB!

TTBR0_EL1:
┌───────────┬──────────────────────────────────────┐
│ ASID      │ Translation Table Base Address       │
│ [63:48]   │ [47:1], CnP[0]                       │
│ 8 or 16   │                                      │
│ bits      │                                      │
└───────────┴──────────────────────────────────────┘

TCR_EL1.AS → 0 = 8-bit ASID (256 ASIDs), 1 = 16-bit (65536 ASIDs)
```

---

## 7. Huge Pages (Block Mappings)

Huge pages reduce TLB pressure for large memory regions:

```
┌───────────────┬────────────────┬────────────────────────┐
│  Granule      │  Block size    │  Benefit               │
├───────────────┼────────────────┼────────────────────────┤
│  4 KB         │  2 MB (L2)     │  One TLB entry = 2 MB  │
│  4 KB         │  1 GB (L1)     │  One TLB entry = 1 GB  │
│  16 KB        │  32 MB (L2)    │  One TLB entry = 32 MB │
│  64 KB        │  512 MB (L2)   │  One TLB entry = 512 MB│
└───────────────┴────────────────┴────────────────────────┘

Linux: echo 'always' > /sys/kernel/mm/transparent_hugepage/enabled
  → Kernel automatically uses 2 MB pages where possible
```

---

## 8. Two-Stage Translation (Virtualization)

When a hypervisor is active, address translation has two stages:

```
┌───────────────────────────────────────────────────────────────────┐
│                                                                   │
│   Guest Application (EL0)                                         │
│   ┌──────────────────┐                                            │
│   │  Virtual Address  │     "VA"                                  │
│   │  (Guest VA)       │                                           │
│   └────────┬─────────┘                                            │
│            │                                                      │
│   ┌────────▼─────────┐                                            │
│   │  Stage 1 (S1)    │  Controlled by Guest OS (EL1)              │
│   │  TTBR0/1_EL1     │  Guest page tables                         │
│   │  VA → IPA         │                                           │
│   └────────┬─────────┘                                            │
│            │                                                      │
│   ┌────────▼──────────────────┐                                   │
│   │  Intermediate Physical     │     "IPA"                        │
│   │  Address (Guest Physical)  │     Guest thinks this is PA      │
│   └────────┬──────────────────┘                                   │
│            │                                                      │
│   ┌────────▼─────────┐                                            │
│   │  Stage 2 (S2)    │  Controlled by Hypervisor (EL2)            │
│   │  VTTBR_EL2       │  Hypervisor page tables                    │
│   │  IPA → PA         │                                           │
│   └────────┬─────────┘                                            │
│            │                                                      │
│   ┌────────▼─────────┐                                            │
│   │ Physical Address  │     Actual hardware address               │
│   └──────────────────┘                                            │
│                                                                   │
│  Total: VA → IPA → PA (worst case: 4×4 = 16 memory accesses!)     │
└───────────────────────────────────────────────────────────────────┘
```

---

## 9. Translation Control Register (TCR_EL1)

```
Key fields:
┌────────────────────────────────────────────────────────────────────┐
│  Field        Bits     Description                                 │
├────────────────────────────────────────────────────────────────────┤
│  T0SZ         [5:0]   Size offset for TTBR0 region                 │
│                        VA size = 64 - T0SZ                         │
│                        T0SZ=16 → 48-bit VA (256 TB)                │
│                        T0SZ=25 → 39-bit VA (512 GB)                │
│  T1SZ         [21:16] Size offset for TTBR1 region                 │
│  TG0          [15:14] Granule for TTBR0 (00=4K, 01=64K, 10=16K)    │
│  TG1          [31:30] Granule for TTBR1                            │
│  SH0          [13:12] Shareability for TTBR0 walks                 │
│  SH1          [29:28] Shareability for TTBR1 walks                 │
│  ORGN0/IRGN0  [11:8]  Cacheability for TTBR0 walks                 │
│  A1           [22]    ASID select (0=TTBR0, 1=TTBR1)               │
│  AS           [36]    ASID size (0=8-bit, 1=16-bit)                │
│  IPS          [34:32] Physical address size                        │
│                        000=32-bit, 010=40-bit, 101=48-bit          │
│  EPD0         [7]     Disable walks for TTBR0                      │
│  EPD1         [23]    Disable walks for TTBR1                      │
└────────────────────────────────────────────────────────────────────┘
```

---

Next: [MMU →](./03_MMU.md)
