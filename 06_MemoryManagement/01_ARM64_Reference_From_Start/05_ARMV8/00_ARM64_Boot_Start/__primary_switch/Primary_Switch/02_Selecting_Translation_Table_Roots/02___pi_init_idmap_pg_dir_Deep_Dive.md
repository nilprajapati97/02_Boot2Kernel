# `__pi_init_idmap_pg_dir` — What The Identity Map Contains and Why

**Built by:** `__pi_create_init_idmap` (called in `primary_entry`)  
**Used as:** `TTBR0_EL1` argument (`x2`) passed to `__enable_mmu`  
**Lives at:** Physical address computed by `adrp x2, __pi_init_idmap_pg_dir`  
**Purpose:** Provide a PA=VA identity map that keeps the CPU instruction stream continuous through the MMU-on transition

---

## 0. The Fundamental Problem This Solves

Before the MMU is on, the CPU fetches instructions from **physical addresses**.
At the precise moment `SCTLR_EL1.M` is set, the next instruction fetch uses a
**virtual address**. If the VA of the next instruction is not mapped in the
page tables, the CPU immediately takes a translation fault — which is
catastrophic because exception vectors (`VBAR_EL1`) are also at a VA that is
not yet mapped.

The solution is the **identity map**: a page table that maps VA `X` → PA `X`
for the range covering the `.idmap.text` section. When the MMU turns on, the
fetch unit requests VA == PC. The identity map translates VA → the same PA.
The instruction at that PA is read. Boot continues uninterrupted.

---

## 1. Where `__pi_init_idmap_pg_dir` Lives

Defined in the linker script (`arch/arm64/kernel/vmlinux.lds.S`):

```ld
__pi_init_idmap_pg_dir = .;
. += INIT_IDMAP_DIR_SIZE;
```

`INIT_IDMAP_DIR_SIZE` is computed in `arch/arm64/include/asm/kernel-pgtable.h`
based on the number of page table levels needed to map the identity region.

**Physical memory layout (example: kernel loaded at PA 0x4000_0000):**

```
Physical Address                     Content
─────────────────────────────────────────────────────
0x4000_0000                          .idmap.text (code we're currently running)
     │  ...                          primary_entry, __primary_switch, __enable_mmu
0x4000_xxxx
     │  ...                          other kernel sections
0x4000_yyyy                          __pi_init_idmap_pg_dir   ← x2 points here
     │  PAGE                         PGD (Level 0 table)
     │  PAGE                         PUD or PMD (Level 1/2 tables, if needed)
0x4000_zzzz                          __pi_init_pg_dir         (final kernel PGD - empty at this point)
     │  ...
0x4000_aaaa                          reserved_pg_dir
0x4000_bbbb                          swapper_pg_dir (to be filled by __pi_early_map_kernel)
```

---

## 2. What `__pi_create_init_idmap` Builds

Called from `primary_entry`:

```asm
// primary_entry:
adrp    x0, __pi_init_idmap_pg_dir   // arg0: start of page table region to populate
mov     x1, xzr                       // arg1: 0 (no additional range)
bl      __pi_create_init_idmap        // returns: x0 = end of used region
```

`__pi_create_init_idmap` is a C function in `arch/arm64/mm/pi/map_range.c`.
It creates **identity mappings** (VA = PA) for:

1. The entire `.idmap.text` section (`__idmap_text_start` to `__idmap_text_end`)
2. Optionally the FDT region (if `CONFIG_RANDOMIZE_BASE` and the FDT is in
   the physical range that would alias with `.idmap.text`)

### 2.1 What `.idmap.text` Contains

```c
// arch/arm64/include/asm/sections.h
extern char __idmap_text_start[], __idmap_text_end[];
```

The `.idmap.text` section contains:
- `primary_entry`
- `record_mmu_state`
- `preserve_boot_args`
- `__pi_create_init_idmap` (position-independent C code)
- `__primary_switch`
- `__enable_mmu`
- `init_kernel_el`
- `secondary_holding_pen`
- `secondary_entry`
- `secondary_startup`

All code that must execute **at physical addresses** (either before or during
the MMU-on transition) lives in `.idmap.text`.

---

## 3. Mapping Granularity

For the `.idmap.text` identity map, `__pi_create_init_idmap` uses the
**largest possible granularity** to minimize the number of page table entries
needed:

### 3.1 2MB Block Mapping (the typical case)

If the entire `.idmap.text` section fits within a single 2MB-aligned region:

```asm
// Level 2 (PMD) block descriptor:
// VA[47:21] → PMD index
// VA[20:0]  → block offset (2MB)
//
// Descriptor bits[1:0] = 0b01 → block entry (at L2)
// Descriptor bits[51:30] = Physical address of 2MB block
// Descriptor bits[4:2] = AttrIdx = MT_NORMAL (4)
// Descriptor bits[8:7] = SH = Inner Shareable (0b11)
// Descriptor bits[6] = AP[2:1] = Read-only for user (0b00 = RW kernel)
// Descriptor bits[9] = AF = 1 (Access Flag — avoid AF fault on first access)
// Descriptor bits[10] = nG = 0 (global — no ASID)
// Descriptor bits[53] = PXN = 0 (Privileged Executable)
// Descriptor bits[54] = UXN = 1 (Unprivileged Not Executable)
```

A 2MB block entry is a **Level 2 PMD descriptor** with bits[1:0]=01 instead
of bits[1:0]=11 (which would be a table pointer). The PA in the descriptor is
the 2MB-aligned base physical address, and the page table walker adds the
lower 21 VA bits as an offset.

### 3.2 Why 2MB and not 4KB?

- The identity map only needs to cover `.idmap.text`, which is typically
  well under 2MB.
- A 2MB block mapping requires only 3 levels of page tables vs 4 levels for
  4KB pages — fewer table pages to allocate in the limited linker-reserved space.
- The hardware PTW is faster with fewer levels.

### 3.3 If `.idmap.text` Spans Multiple 2MB Regions

In rare cases (e.g., KASLR randomization places the kernel at an odd alignment),
the section may not fit in one 2MB block. `__pi_create_init_idmap` handles this
by creating multiple PMD entries or, if necessary, falling back to 4KB PTE
granularity for the affected sub-region.

---

## 4. Page Table Level Structure (4KB granule, 48-bit VA)

The identity map needs to map VAs of the form `0x4000_xxxx` (example PA).

```
VA = 0x0000_0000_4000_0000

PGD (Level 0):  VA[47:39] = 0b0_0000_0000 = index 0  → points to PUD table
PUD (Level 1):  VA[38:30] = 0b0_0000_0001 = index 1  → points to PMD table
PMD (Level 2):  VA[29:21] = 0b0_0000_0010 = index 2  → BLOCK ENTRY (2MB)
                                                         PA = 0x4000_0000
                                                         (covers 0x4000_0000 - 0x401F_FFFF)
```

For this example:
- 1 PGD table (4KB) with 1 valid entry pointing to PUD
- 1 PUD table (4KB) with 1 valid entry pointing to PMD
- 1 PMD table (4KB) with 1 valid 2MB block entry
- **Total: 3 × 4KB = 12KB** of page table data

This is why `INIT_IDMAP_DIR_SIZE` is small — just a few pages.

---

## 5. The Critical Property: PA=VA Continuity

The identity map has exactly this property:

```
For any address X in the range [__idmap_text_start, __idmap_text_end):
    translate(VA=X) = PA=X
```

This means:

```
Before MMU-on:  CPU fetches from PA 0x4000_1234 (example)
                (PC = physical address 0x4000_1234)

MMU turns on:   CPU issues fetch for VA = 0x4000_1234
                MMU walks __pi_init_idmap_pg_dir
                Finds: VA 0x4000_1234 → PA 0x4000_1234
                Instruction at PA 0x4000_1234 is fetched

After MMU-on:   Instruction stream is uninterrupted
```

---

## 6. The Safety of TTBR0 After MMU Enable

After `__enable_mmu`, `TTBR0_EL1` points to `__pi_init_idmap_pg_dir`.
`TTBR1_EL1` points to `reserved_pg_dir` (empty).

The CPU continues executing code from `.idmap.text` (identity-mapped range
under TTBR0). This works because:

1. All PCs immediately after MMU-on are in the range covered by the identity map.
2. The `ret` instruction in `__enable_mmu` returns to `__primary_switch`,
   which is also in `.idmap.text`.
3. `__primary_switch` then calls `__pi_early_map_kernel`, also in `.idmap.text`.

The identity map remains valid until `__pi_early_map_kernel` completes and
the final jump to `__primary_switched` executes. At that point, `__primary_switched`
runs in TTBR1 (kernel VA space), and the identity map in TTBR0 is no longer
the active instruction stream.

Eventually, `cpu_replace_ttbr1` (called much later, post-`start_kernel`) will
replace TTBR0 with `init_mm.pgd` (user space page table for the init process)
and the identity map is gone.

---

## 7. Why `__pi_` Prefix

The `__pi_` prefix stands for **position-independent**. These functions are
compiled with `-fno-omit-frame-pointer -fno-pic` disabled and linked at a
specific virtual address but executed at a different physical address.

For `__pi_create_init_idmap`:
- **Link address** (`__pi_create_init_idmap`): somewhere in the high kernel VA
  range (e.g., `0xFFFF_8000_4020_1234`)
- **Physical load address** (where the instruction bytes actually are):
  `0x4020_1234` (example)

The function uses `adrp`/`adr` instructions (PC-relative) for all address
references, so it works correctly regardless of whether the PA or VA is used
as the PC. This is what makes it safe to call before the MMU is on — it
doesn't use any absolute addresses that would point to the kernel VA range.

---

## 8. Interview Question: What Happens If `.idmap.text` Contains a Hole?

If `.idmap.text` had a gap — e.g., some section placed between
`__idmap_text_start` and `__idmap_text_end` that is NOT present in physical
memory — the identity map would map the VA corresponding to the hole to a
physical address that contains garbage data.

In practice, the linker script ensures `.idmap.text` is a contiguous section
with no holes. The `ASSERT` statements in `vmlinux.lds.S` verify this at
link time.

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