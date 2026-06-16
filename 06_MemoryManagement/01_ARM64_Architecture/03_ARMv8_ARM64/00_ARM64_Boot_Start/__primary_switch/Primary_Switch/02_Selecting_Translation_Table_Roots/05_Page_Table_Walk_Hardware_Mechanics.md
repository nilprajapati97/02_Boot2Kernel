# AArch64 Page Table Walk Hardware Mechanics

**Context:** Understanding exactly what the hardware does when TTBR0/TTBR1 are
written and the MMU is enabled in `__enable_mmu`  
**Reference:** ARM Architecture Reference Manual, D5 — The AArch64 Virtual Memory System Architecture

---

## 0. The Big Picture

When the CPU generates a virtual address (for an instruction fetch, load, or
store), the hardware performs this sequence:

```
VA Generated
    │
    ▼
TLB Lookup (< 2 cycles if hit)
    │
    ├── HIT ──► PA + Attributes → proceed with access
    │
    └── MISS ──► Hardware Page Table Walker (PTW)
                     │
                     ├── Reads TTBR0 or TTBR1 (see previous document)
                     ├── Walks page table levels in memory (5-50 cycles)
                     ├── Checks fault conditions at each level
                     ├── Installs result in TLB
                     └── PA + Attributes → proceed with access
```

This document covers the **MISS path** — the hardware page table walk.

---

## 1. The 4-Level Walk for 4KB Granule, 48-bit VA

With `TCR_EL1` configured for 4KB granule and 48-bit VA:

```
VA[63:0] = 0xFFFF_8000_4020_1234   (example kernel VA)

Bit assignment:
VA[63:48] = 0xFFFF                  ← all ones → TTBR1 selected
VA[47:39] = 0b1_0000_0000 = 256     ← PGD (L0) index
VA[38:30] = 0b0_0000_0000 = 0       ← PUD (L1) index
VA[29:21] = 0b0_0010_0000 = 32      ← PMD (L2) index
VA[20:12] = 0b0_0001_0000 = 16      ← PTE (L3) index
VA[11:0]  = 0x234                   ← Page offset within 4KB page
```

---

## 2. Level 0 — PGD (Page Global Directory)

### Step 1: Locate the PGD Base

```
PGD base PA = TTBR1_EL1[47:1] << 1    // 48-bit PA (typical)
            = (TTBR1_EL1 & 0x0000_FFFF_FFFF_FFFE)
```

For the boot path, `TTBR1_EL1` = physical address of `reserved_pg_dir`.

### Step 2: Compute the PGD Entry Address

```
PGD entry PA = PGD base PA + (VA[47:39] × 8)
             = PA_of_reserved_pg_dir + (256 × 8)
             = PA_of_reserved_pg_dir + 2048
```

### Step 3: Fetch the PGD Entry

The PTW fetches 8 bytes from the computed physical address. This is a
**cacheable physical memory read** (because `TCR_EL1.ORGN1 = Write-Back`,
so the PTW uses the outer cache attributes for page table walks).

During boot, `reserved_pg_dir` is all zeros → PGD entry = 0x0000_0000_0000_0000.

### Step 4: Check the Entry

```
Entry bits[1:0]:
    0b00 → INVALID descriptor → Translation Fault (Level 0)
    0b01 → Block entry (only valid at L1/L2, not L0 in 4KB granule)
    0b11 → Table descriptor → continue to next level
```

Entry = 0x00...00 → bits[1:0] = `0b00` → **INVALID → Translation Fault**.

This is exactly what happens if the CPU tries to access a kernel VA while
`TTBR1 = reserved_pg_dir`. The fault is raised. During boot, IRQs are masked
and this path should never be taken.

After `__pi_early_map_kernel` runs, `swapper_pg_dir` replaces `reserved_pg_dir`
in TTBR1, and valid entries allow the walk to succeed for kernel VAs.

---

## 3. Full Walk with Valid Tables (after `__pi_early_map_kernel`)

Using VA `0xFFFF_8000_4020_1234` after the kernel is mapped:

### Level 0 (PGD) — Table Descriptor

```
PGD entry (fetched from PA_of_swapper_pg_dir + VA[47:39]*8):

Bits [63:52] = attributes (NSTable, APTable, UXNTable, PXNTable)
Bits [51:12] = PA[47:12] of Level 1 (PUD) table      ← next level base
Bits [11:2]  = RES0
Bits [1:0]   = 0b11                                    ← TABLE descriptor

0b11 = table → follow to Level 1
```

### Level 1 (PUD) — Table Descriptor

```
PUD base PA = bits[51:12] of PGD entry << 12

PUD entry PA = PUD base PA + (VA[38:30] × 8)

Entry bits[1:0] = 0b11 → TABLE descriptor → follow to Level 2
```

### Level 2 (PMD) — Block or Table Descriptor

For the kernel `.text` section, Linux uses **2MB block mappings** (not 4KB pages):

```
PMD entry (2MB block descriptor):

Bits [63] = 0        (ignored)
Bits [62] = PBHA[3]  (Page-Based Hardware Attributes — for MPAM, etc.)
Bits [59:58] = SW    (software-defined bits, e.g., young/dirty tracking)
Bit  [54] = UXN=1    (Unprivileged Execute Never — kernel code not user-executable)
Bit  [53] = PXN=0    (Privileged Execute Never = 0 → kernel code IS executable)
Bits [51:30] = PA[51:30] of 2MB block   ← Physical base of 2MB region
Bit  [10] = AF=1     (Access Flag = 1 → set by software, avoids AF fault)
Bit  [9]  = nSH=0    (Inner Shareable)
Bits [8:7] = SH=0b11 (Inner Shareable)
Bits [6:5] = AP=0b00 (Read/Write, Privileged Only — kernel RW)
Bit  [5]  = NS=0     (Non-Secure)
Bits [4:2] = AttrIdx=4 (MT_NORMAL — Write-Back Cacheable)
Bits [1:0] = 0b01    ← BLOCK descriptor at L2 (NOT 0b11 table!)
```

**Block descriptor vs Table descriptor:**

| bits[1:0] | Meaning at L2 (PMD) |
|---|---|
| `0b00` | INVALID |
| `0b01` | BLOCK (2MB) — PA in bits[51:21] |
| `0b11` | TABLE — next level address in bits[51:12] |

For a 2MB block: the PA is bits[51:21] of the entry. VA bits[20:0] are the
2MB-region offset and are added by hardware to get the final PA.

---

## 4. TLB Entry Installation

After the walk completes:

```
TLB entry installed:
    VA[47:12] (or VA[47:21] for 2MB)  → PA[47:12]
    ASID                               → from TTBR ASID field
    AttrIdx decoded from entry         → MT_NORMAL attributes
    nG bit                             → 0 (global) for kernel
    AF bit                             → checked/updated
    Shareability                       → Inner Shareable (SH=0b11)
    Permissions                        → AP decoded
    UXN / PXN                          → from descriptor
```

For subsequent accesses to the same 2MB region, the TLB hit avoids the entire
multi-level walk.

---

## 5. Access Flag (AF) — Hardware Auto-Update

### The AF Bit in PTEs

`AF` (Access Flag, bit 10 of a page table descriptor) was historically managed
by software: set to 0 when a page is first mapped; when the CPU accesses a page
with `AF=0`, it takes an **Access Flag Fault** to the kernel, which sets `AF=1`
and returns. This was used to implement page reclaim algorithms.

### Hardware AF Update (FEAT_HAFDBS)

ARMv8.1 introduced **Hardware Access Flag and Dirty state update support**
(`FEAT_HAFDBS`). When enabled:

```
TCR_EL1.HA = 1  → Hardware automatically sets AF when a page is first accessed
TCR_EL1.HD = 1  → Hardware automatically sets the dirty bit when a page is written
```

With `TCR_EL1.HA = 1` (enabled by `__cpu_setup` if supported):
- The PTW sets `AF=1` in the descriptor atomically when AF=0 is found.
- No Access Flag Fault is taken.
- The kernel page reclaim code (`mm/vmscan.c`) reads the AF bit to determine
  if a page was accessed recently.

Linux enables `TCR_EL1.HA` when `ID_AA64MMFR1_EL1.HAFDBS` is non-zero.

---

## 6. Dirty Bit (DBM) — Hardware Dirty State Management

### Dirty Tracking Without FEAT_HAFDBS

Without hardware dirty bit management:
- Pages are mapped read-only initially.
- First write causes a **Permission Fault**.
- Kernel fault handler marks page dirty and remaps read-write.
- Slow path — every first write to a clean page traps to the kernel.

### With FEAT_HAFDBS (`TCR_EL1.HD = 1`)

The hardware uses the **DBM** (Dirty Bit Modifier) bit in PTEs:

```
PTE bit [51] = DBM = 1  → Hardware dirty bit management enabled for this entry

When the CPU writes to a page:
    If AP[2] = 1 (read-only) AND DBM = 1:
        Hardware atomically sets AP[2] = 0 (read-write)
        No fault taken
        Page is now "dirty" (indicated by AP[2] having changed)
```

The kernel scans `AP[2]` to detect dirty pages for copy-on-write and msync.

---

## 7. What Happens During the Boot Walk

When `br x8` jumps to `__primary_switched` (VA `0xFFFF_8000_yyyy_yyyy`):

1. Instruction fetch unit generates VA `0xFFFF_8000_yyyy_yyyy`
2. L1 I-TLB: **MISS** (TLB is empty after TLBI in `__cpu_setup`)
3. Hardware PTW starts:
   - Reads `TTBR1_EL1` → `swapper_pg_dir` PA (just installed by `__pi_early_map_kernel`)
   - Level 0 walk → valid PGD table descriptor
   - Level 1 walk → valid PUD table descriptor
   - Level 2 walk → valid 2MB block entry for kernel `.text`
   - PA computed: `PA = bits[51:21] of PMD entry + VA[20:0]`
4. TLB entry installed for this 2MB region
5. Instruction at computed PA is fetched
6. Execution continues in `__primary_switched`

All subsequent instruction fetches in the same 2MB region hit the TLB.

---

## 8. Walk Cache Optimizations

Modern ARM64 CPUs (Cortex-A76, Neoverse N1, etc.) implement a **walk cache**
that caches intermediate-level translation results:

```
L1 walk cache: caches PGD → PUD mappings (bits[47:39] → PUD base PA)
L2 walk cache: caches PUD → PMD mappings (bits[47:30] → PMD base PA)
```

After the first walk, subsequent VA translations within the same 1GB region
skip the PGD lookup entirely (walk cache hit). This reduces average TLB miss
penalty from 4 memory reads to 2.

Walk caches are included in the scope of `TLBI VMALLE1` — they are also
invalidated along with the L1/L2 TLBs.

---

## 9. Summary: PTW Performance Numbers (Cortex-A78 approximation)

| Scenario | Latency (cycles) |
|---|---|
| L1 TLB hit | 1 |
| L2 TLB hit | 4-8 |
| L2 walk cache hit (2 DRAM reads) | ~20 |
| Full 4-level walk, L2 cache hits | ~30-50 |
| Full 4-level walk, DRAM reads | ~200-400 |

**Why this matters for `__primary_switch`:** The identity map uses 2MB blocks
(3-level walk), the kernel mapping uses 2MB blocks (also 3-level after
`__pi_early_map_kernel`). This minimizes walk latency during the critical
early boot phase when no walk cache is warm.

---

## ARMv8 CPU / Kernel / Memory Context

### CPU Perspective (ARMv8-A)
ARMv8-A uses a multi-level page-table walk starting at the physical address stored in TTBR0_EL1 (low VA, user) or TTBR1_EL1 (high VA, kernel). With 4 KB granule and 48-bit VA (T0SZ/T1SZ=16): the walk has 4 levels: L0 (PGD), L1 (PUD), L2 (PMD), L3 (PTE). Each table entry is 8 bytes. A leaf entry (block or page descriptor) contains the PA, memory attributes (AttrIdx into MAIR_EL1), access permissions (AP bits), shareability, and XN/PXN bits. The hardware page-table walker is fully autonomous: on a TLB miss, it reads TTBR, walks the tables in memory, and populates the TLB entry without software intervention.

### Kernel Perspective (Linux ARM64)
Linux organizes the early boot page tables into:
- __idmap_pg_dir: identity map (VA==PA) covering the .idmap.text section, used during MMU enable.
- init_pg_dir / swapper_pg_dir: kernel page table covering the TTBR1_EL1 region.
__pi_early_map_kernel (arch/arm64/mm/mmu.c) allocates and populates these tables using fixmap and the early pgtable allocator before __enable_mmu is called. After start_kernel, the definitive kernel page tables are built by paging_init().

### Memory Perspective (ARMv8 Memory Model)
Each page-table entry encodes both the physical address and the memory type for that region. The ARMv8 memory model treats adjacent pages independently: two adjacent 4 KB pages can have different memory attributes (e.g., one Normal cacheable, one Device). Block entries (2 MB at L2, 1 GB at L1) allow the walker to terminate early, reducing TLB fill latency and the number of memory accesses per walk. The hardware guarantees that table walks are coherent with the data cache if the inner-shareable domain includes the page-table memory (which is true for all Linux ARM64 configurations).