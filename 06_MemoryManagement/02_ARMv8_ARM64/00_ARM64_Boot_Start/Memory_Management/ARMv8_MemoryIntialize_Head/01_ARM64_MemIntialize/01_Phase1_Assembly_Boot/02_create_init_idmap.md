# `__pi_create_init_idmap()` — Identity Page Table Creation

**Source:** `arch/arm64/kernel/pi/map_range.c` lines 87–106
**Phase:** Assembly Boot (MMU OFF)
**Memory Allocator:** None (uses static BSS)
**Called by:** `primary_entry()` in head.S
**Calls:** `map_range()`

---

## What This Function Does

Creates **identity-mapped page tables** where `virtual address == physical address` for the kernel image. This is the prerequisite for enabling the MMU — without identity mapping, the CPU would lose access to the code it's currently executing the moment the MMU turns on.

---

## Why Identity Mapping?

When the MMU is OFF, the CPU fetches instructions from **physical addresses**. The program counter (PC) holds a physical address. When we set SCTLR_EL1.M=1 (MMU enable), the very next instruction fetch goes through the MMU's translation tables. If those tables don't have an entry for the current PC value, the CPU takes a **translation fault** and crashes.

The solution: create page tables where physical address X maps to virtual address X (identity = same). This way, the instruction at physical address 0x4080_0000 is also accessible at virtual address 0x4080_0000.

```
Before MMU ON:   PC = 0x4080_0000 → fetch from physical 0x4080_0000  ✓
After MMU ON:    PC = 0x4080_0000 → translate → PTE says 0x4080_0000 → ✓
```

---

## How It Works With Memory

### Input Memory

| Input | Source | Description |
|-------|--------|-------------|
| `pg_dir` | `__pi_init_idmap_pg_dir` (BSS) | Pre-zeroed 4KB page for the PGD |
| `_stext` | Linker symbol | Start of kernel code |
| `__initdata_begin` | Linker symbol | End of code, start of init data |
| `_end` | Linker symbol | End of kernel image |

### Output Memory

| Created | Location | Size | Description |
|---------|----------|------|-------------|
| PGD entries | `init_idmap_pg_dir` | 4 KB | Top-level page table |
| PUD/PMD/PTE pages | Adjacent BSS pages | 4 KB each | Lower-level tables |

The function returns `ptep` — a pointer past the last PTE page used. This tells the caller how much BSS memory was consumed for page tables.

---

## Function Implementation

```c
asmlinkage phys_addr_t __init create_init_idmap(pgd_t *pg_dir, ptdesc_t clrmask)
{
    phys_addr_t ptep = (phys_addr_t)pg_dir + PAGE_SIZE;  // PTE allocator starts here
    pgprot_t text_prot = PAGE_KERNEL_ROX;    // Read-Only eXecutable
    pgprot_t data_prot = PAGE_KERNEL;        // Read-Write

    pgprot_val(text_prot) &= ~clrmask;       // Clear bits for LPA2 if needed
    pgprot_val(data_prot) &= ~clrmask;

    // Map kernel TEXT (identity: phys == virt)
    map_range(&ptep,
              (u64)_stext,              // Virtual start = physical start
              (u64)__initdata_begin,    // Virtual end
              (phys_addr_t)_stext,      // Physical start
              text_prot,                // PAGE_KERNEL_ROX
              IDMAP_ROOT_LEVEL,         // Root level of page table hierarchy
              (pte_t *)pg_dir,          // PGD base
              false,                    // No contiguous PTEs
              0);                       // VA offset = 0 (identity mapping!)

    // Map kernel DATA (identity: phys == virt)
    map_range(&ptep,
              (u64)__initdata_begin,    // Virtual start
              (u64)_end,                // Virtual end
              (phys_addr_t)__initdata_begin,  // Physical start
              data_prot,                // PAGE_KERNEL (RW)
              IDMAP_ROOT_LEVEL,
              (pte_t *)pg_dir,
              false,
              0);

    return ptep;  // Return end of allocated PTE pages
}
```

### Key Parameter: VA Offset = 0

The last parameter `0` is the **virtual address offset**. Setting it to zero means:
- `virtual_address = physical_address + 0` → identity mapping
- Later, when mapping the kernel at its link address, this offset will be non-zero

---

## The `map_range()` Algorithm — Page Table Walk Builder

`map_range()` is the core function that builds the multi-level page table hierarchy. It works recursively, creating entries at each level.

### ARM64 Page Table Hierarchy (4KB granule, 48-bit VA)

```
Virtual Address (48 bits):
┌──────────┬──────────┬──────────┬──────────┬──────────────┐
│ [47:39]  │ [38:30]  │ [29:21]  │ [20:12]  │ [11:0]       │
│ PGD idx  │ PUD idx  │ PMD idx  │ PTE idx  │ Page offset  │
│ 9 bits   │ 9 bits   │ 9 bits   │ 9 bits   │ 12 bits      │
└──────────┴──────────┴──────────┴──────────┴──────────────┘
     │           │           │           │
     ▼           ▼           ▼           ▼
  PGD Table → PUD Table → PMD Table → PTE Table → Physical Page
  512 entries  512 entries  512 entries  512 entries    4 KB
  (4 KB)       (4 KB)       (4 KB)       (4 KB)
```

### How map_range() Builds Tables

```
For each virtual address in [virt_start, virt_end):

1. Calculate PGD index = VA[47:39]
   - If PGD[index] is empty:
     - Allocate a new PUD table from ptep (bump allocator)
     - Write PGD[index] = physical_addr_of_new_PUD | TABLE_DESCRIPTOR
   - Follow PGD[index] to PUD table

2. Calculate PUD index = VA[38:30]
   - If mapping is PUD-aligned AND >= 1GB:
     - Write PUD[index] = phys_addr | BLOCK_DESCRIPTOR  (1GB block mapping)
     - Skip to next 1GB chunk
   - Else if PUD[index] is empty:
     - Allocate a new PMD table from ptep
     - Write PUD[index] = physical_addr_of_new_PMD | TABLE_DESCRIPTOR
   - Follow PUD[index] to PMD table

3. Calculate PMD index = VA[29:21]
   - If mapping is PMD-aligned AND >= 2MB:
     - Write PMD[index] = phys_addr | BLOCK_DESCRIPTOR  (2MB block mapping)
     - Skip to next 2MB chunk
   - Else if PMD[index] is empty:
     - Allocate a new PTE table from ptep
     - Write PMD[index] = physical_addr_of_new_PTE | TABLE_DESCRIPTOR
   - Follow PMD[index] to PTE table

4. Calculate PTE index = VA[20:12]
   - Write PTE[index] = phys_addr | PAGE_DESCRIPTOR | attributes
```

### Bump Allocator for PTE Pages

The function uses a **bump allocator** (`ptep`) — the simplest possible allocator:

```
ptep starts at pg_dir + PAGE_SIZE (right after the PGD)

When a new page table level is needed:
  new_table = ptep
  ptep += PAGE_SIZE       // Bump the pointer forward
  memset(new_table, 0, PAGE_SIZE)  // Zero the new table
  return new_table
```

This works because the linker script reserves enough contiguous BSS space after `init_idmap_pg_dir` for all the intermediate page table levels.

---

## Page Table Entry Format

### ARM64 Descriptor Types

```
Bits [1:0] determine the descriptor type:

00 = Invalid (unmapped)
01 = Block descriptor (1GB at PUD level, 2MB at PMD level)
10 = Invalid
11 = Table descriptor (points to next-level table) or Page descriptor (at PTE level)
```

### Block Descriptor (2MB or 1GB)

```
┌──────┬──────────────────────┬─────────────────────────┬────┐
│63..52│51..48    47..n       │n-1..12  11..2           │1..0│
│Upper │Reserved  Output Addr│Reserved Attributes       │ 01 │
│Attrs │          [47:n]      │         (AP,SH,AF,etc.) │    │
└──────┴──────────────────────┴─────────────────────────┴────┘
```

### Page Descriptor (4KB at PTE level)

```
┌──────┬──────────────────────┬─────────────────────────┬────┐
│63..52│51..48    47..12      │11..2                    │1..0│
│Upper │Reserved  Output Addr│Attributes               │ 11 │
│Attrs │          [47:12]     │(AP,SH,AF,nG,etc.)      │    │
└──────┴──────────────────────┴─────────────────────────┴────┘
```

### Attribute Fields

| Bits | Field | Meaning |
|------|-------|---------|
| [4:2] | AttrIndx | Index into MAIR_EL1 (selects memory type) |
| [7:6] | AP | Access Permission (RW, RO, kernel-only, user+kernel) |
| [9:8] | SH | Shareability (Non-shareable, Inner, Outer) |
| [10] | AF | Access Flag (must be 1 for valid entry) |
| [53] | PXN | Privileged Execute Never |
| [54] | UXN/XN | Execute Never (user/unprivileged) |

---

## Protection Attributes Used

### `PAGE_KERNEL_ROX` — For Kernel Text

```
AttrIndx = MT_NORMAL (index 0 → MAIR: Normal, Write-Back Cacheable)
AP       = AP_KERNEL_RO (Read-Only, kernel only)
SH       = INNER_SHAREABLE
AF       = 1 (Access Flag set)
PXN      = 0 (Privileged Execute: ALLOWED)
UXN      = 1 (User Execute: NOT allowed)
```

**Result:** Kernel code is Read-Only + Executable. Prevents code modification, prevents user-space execution.

### `PAGE_KERNEL` — For Kernel Data

```
AttrIndx = MT_NORMAL (Normal, Write-Back Cacheable)
AP       = AP_KERNEL_RW (Read-Write, kernel only)
SH       = INNER_SHAREABLE
AF       = 1
PXN      = 1 (Privileged Execute: NOT allowed)
UXN      = 1 (User Execute: NOT allowed)
```

**Result:** Kernel data is Read-Write + Non-Executable. Prevents executing data as code (W^X policy).

---

## Memory Consumed

For a typical kernel image of ~30 MB loaded at physical address 0x4080_0000:

```
init_idmap_pg_dir:
├── PGD (Level 0):    1 × 4 KB = 4 KB    (512 entries, only 1 used)
├── PUD (Level 1):    1 × 4 KB = 4 KB    (512 entries, only 1 used)
├── PMD (Level 2):    1 × 4 KB = 4 KB    (512 entries, ~15 used for 30MB)
└── PTE (Level 3):    0 × 4 KB = 0 KB    (using 2MB block mappings)

Total: ~12 KB of BSS for identity page tables
```

With 2MB block mappings at PMD level, no PTE tables are needed for a 30MB kernel image. Each PMD entry covers 2MB, so 15 entries cover the entire image.

---

## Relationship to Other Functions

```
primary_entry()
    │
    ├── __pi_create_init_idmap()   ← THIS FUNCTION
    │       Creates identity map: phys == virt
    │       Used for: MMU enable transition
    │
    ├── __cpu_setup()
    │       Configures TCR to use these page tables
    │
    ├── __primary_switch()
    │       __enable_mmu() loads init_idmap_pg_dir into TTBR0_EL1
    │       __pi_early_map_kernel() creates kernel mapping (phys ≠ virt)
    │
    └── Later: paging_init() → create_idmap()
            Recreates identity map with full memblock knowledge
```

---

## Key Takeaways

1. **Bump allocator** — the simplest possible page table allocator (just increment a pointer)
2. **Block mappings** (2MB) are used when possible for efficiency — fewer page table levels
3. **W^X enforcement** starts here — text is RO+X, data is RW+NX, even before the MMU is on
4. **Identity mapping is temporary** — it's only needed during the MMU-on transition. Later, `paging_init()` creates a proper identity map with full memory knowledge
5. **No dynamic allocation** — all memory comes from pre-reserved BSS space
