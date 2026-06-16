# Layer 2 — Early Assembly Boot (head.S)

[← Layer 1: Hardware Entry](01_Hardware_Entry_and_Bootloader.md) | **Layer 2** | [Layer 3: Kernel Virtual Mapping →](03_Kernel_Virtual_Mapping.md)

---

## What This Layer Covers

The very first kernel code that runs is assembly language in `arch/arm64/kernel/head.S`.
This layer explains every step from the first instruction to the moment the MMU is turned
on. After this layer, the CPU executes from an **identity map** — physical addresses equal
virtual addresses.

---

## 1. The Entry Point: `primary_entry`

**Source**: `arch/arm64/kernel/head.S`

The kernel image starts with an ARM64 image header. The very first instruction at offset
0x00 is:

```
    b   primary_entry       // Branch to the real entry point
```

This skips over the image header (magic number, image size, flags, etc.) and lands at
`primary_entry` — the true beginning of Linux on ARM64.

### 1.1 Register Conventions

During the entire boot path (from `primary_entry` through `start_kernel`), the kernel
uses callee-saved registers as persistent state:

```
┌──────────┬────────────────────────────────────┬─────────────────────────┐
│ Register │ Scope                               │ Purpose                 │
├──────────┼────────────────────────────────────┼─────────────────────────┤
│   x19    │ primary_entry → start_kernel       │ Was MMU on at entry?    │
│   x20    │ primary_entry → __primary_switch   │ CPU boot mode (EL1/EL2) │
│   x21    │ primary_entry → start_kernel       │ FDT pointer (from x0)  │
└──────────┴────────────────────────────────────┴─────────────────────────┘
```

### 1.2 The `primary_entry` Sequence

```
primary_entry
    │
    ├──► bl record_mmu_state           [Step 1]
    │       Read SCTLR_ELx to check if MMU is on/off.
    │       Store result in x19. Fix endianness if needed.
    │
    ├──► bl preserve_boot_args         [Step 2]
    │       Save x0 (FDT pointer), x1, x2, x3 into boot_args[] array.
    │       If MMU was off: invalidate dcache for that region
    │       (because dcache is off, data was written to RAM directly;
    │        need to ensure coherency later).
    │
    ├──► bl __pi_create_init_idmap     [Step 3]  ★ CRITICAL ★
    │       Create the IDENTITY MAP page tables in init_idmap_pg_dir.
    │       This maps the kernel image at PA == VA.
    │       (Explained in detail in Section 2 below)
    │
    ├──► Cache maintenance              [Step 4]
    │       If MMU off: invalidate idmap page tables from dcache.
    │       If MMU on: clean idmap text to Point of Coherency.
    │
    ├──► bl init_kernel_el             [Step 5]
    │       Determine if booting from EL2 or EL1.
    │       If EL2: configure HCR_EL2, install hypervisor stub,
    │               set up VHE (Virtualization Host Extensions)
    │               if supported, or drop to EL1.
    │       Returns BOOT_CPU_MODE_EL1 or BOOT_CPU_MODE_EL2 in x0.
    │       Saved in x20.
    │
    ├──► bl __cpu_setup                [Step 6]
    │       Initialize MAIR, TCR, compute SCTLR value.
    │       (Explained in detail in Section 3 below)
    │       Returns SCTLR value (with MMU enable bit set) in x0.
    │
    └──► b  __primary_switch           [Step 7]
            Enable MMU, jump to virtual address.
            (Explained in detail in Section 4 below)
```

---

## 2. Creating the Identity Map: `create_init_idmap`

**Source**: `arch/arm64/kernel/pi/map_range.c`

### 2.1 Why an Identity Map?

When the MMU is off, the CPU uses physical addresses. When we turn the MMU on, the CPU
immediately starts using virtual addresses. The instruction right after enabling the MMU
must be at the same address in both the physical and virtual worlds — otherwise the CPU
would fetch from the wrong place and crash.

The solution: create a page table where `virtual address == physical address` for the
kernel code. This is called an **identity map** (or "idmap").

```
Before MMU ON:      CPU fetches from physical address 0x4080_0000
After  MMU ON:      CPU fetches from virtual address  0x4080_0000
                    Identity map translates: VA 0x4080_0000 → PA 0x4080_0000
                    Same instruction executes successfully!
```

### 2.2 What Gets Identity-Mapped

`create_init_idmap()` creates two mappings in `init_idmap_pg_dir`:

```
init_idmap_pg_dir (identity map page table)
════════════════════════════════════════════

Mapping 1:  [_stext  →  __initdata_begin)
            Kernel text + rodata
            Protection: Read-Only + Execute (ROX)
            These are the code sections that run during boot.

Mapping 2:  [__initdata_begin  →  _end)
            Kernel data + BSS + page table areas
            Protection: Read-Write (RW), No Execute
            These are data sections and boot-time page tables.

In both mappings: Virtual Address == Physical Address
```

### 2.3 The `map_range()` Recursive Page Table Builder

`map_range()` in `arch/arm64/kernel/pi/map_range.c` is a position-independent recursive
function that builds page tables at any level. It works even before the MMU is on
(because all pointers are physical addresses at this point).

```
map_range(pte_pool, va_start, va_end, pa_start, prot, level, ...)
    │
    │   For each entry in the table at this level:
    │
    ├── If level allows BLOCK mapping and range is aligned:
    │       Write a BLOCK entry (1GB at L1, 2MB at L2)
    │       No need for deeper page table levels.
    │
    └── Otherwise:
            Allocate a subordinate table from pte_pool.
            Write a TABLE entry pointing to the new table.
            Recurse into the next level.
            (At the bottom level, write PAGE entries for 4KB pages.)
```

The `pte_pool` pointer is a simple linear allocator: each time a new page table page is
needed, it advances the pointer by one page. These pages come from the `init_idmap_pg_dir`
region reserved in the kernel's linker script.

### 2.4 Page Table Directories (Linker Script)

The kernel linker script (`arch/arm64/kernel/vmlinux.lds.S`) reserves space for multiple
page table directories within the kernel image itself:

```
Kernel Image Layout (BSS region — zero-initialized)
═══════════════════════════════════════════════════════

      Symbol              Size              Purpose
      ──────              ────              ───────
      idmap_pg_dir        1 page            Runtime identity map (created later)
      tramp_pg_dir        1 page            KPTI trampoline (if enabled)
      reserved_pg_dir     1 page            Empty PGD (safe TTBR1 during switches)
      swapper_pg_dir      1 page            Final kernel page table root
      init_idmap_pg_dir   INIT_IDMAP_DIR_SIZE   Boot-time identity map ★ created now
      init_pg_dir         INIT_DIR_SIZE     Initial kernel VA mapping (Layer 3)
```

**Why so many page table directories?**
- `init_idmap_pg_dir` — Used RIGHT NOW (before MMU ON) for the identity map
- `init_pg_dir` — Will hold the kernel's virtual mapping (Layer 3)
- `swapper_pg_dir` — Will become the FINAL kernel page table root (used for kernel lifetime)
- `idmap_pg_dir` — A compact runtime identity map (just for .idmap.text section)
- `reserved_pg_dir` — An empty table, used as a safe TTBR1 value during page table switches

---

## 3. CPU Setup: `__cpu_setup`

**Source**: `arch/arm64/mm/proc.S`

Before enabling the MMU, the CPU's memory management registers must be configured.
`__cpu_setup` initializes three critical registers:

### 3.1 MAIR_EL1 — Memory Attribute Indirection Register

MAIR defines the memory types that page table entries can reference by index:

```
MAIR_EL1 Attribute Configuration
═════════════════════════════════

Index    Name              Encoding    Description
─────    ────              ────────    ───────────
  0      MT_NORMAL         0xFF        Normal memory, Write-Back cacheable
                                       (Inner + Outer Write-Back, Read+Write Allocate)
                                       ➜ Used for RAM

  1      MT_NORMAL_TAGGED  0xFF        Same as MT_NORMAL (upgraded to Tagged if MTE)
                                       ➜ Used for MTE-tagged memory

  2      MT_NORMAL_NC      0x44        Normal memory, Non-Cacheable
                                       ➜ Used for coherent DMA buffers

  3      MT_DEVICE_nGnRnE  0x00        Device memory: non-Gathering, non-Reordering,
                                       non-Early-write-acknowledgement
                                       ➜ Strongly-ordered device MMIO

  4      MT_DEVICE_nGnRE   0x04        Device memory: non-Gathering, non-Reordering,
                                       Early-write-acknowledgement
                                       ➜ Standard device MMIO
```

When a page table entry says "use memory type index 0", the hardware looks up MAIR
index 0 and applies Write-Back cacheable attributes.

### 3.2 TCR_EL1 — Translation Control Register

TCR controls the behavior of both TTBR0 and TTBR1 translations:

```
TCR_EL1 Key Fields
═══════════════════

Field      Bits    Value for Linux             Meaning
─────      ────    ───────────────             ───────
T0SZ       [5:0]   64 - IDMAP_VA_BITS (= 16)  TTBR0 VA size: 48-bit for idmap
T1SZ       [21:16] 64 - VA_BITS_MIN (= 16)    TTBR1 VA size: 48-bit (or 39-bit, etc.)
TG0        [15:14] 0b00                        TTBR0 granule: 4KB
TG1        [31:30] 0b10                        TTBR1 granule: 4KB
SH0/SH1             Inner Shareable             Shareability for page table walks
ORGN/IRGN            Write-Back cacheable        Cacheability for page table walks
IPS        [34:32] From ID_AA64MMFR0_EL1       Physical Address Size
                                                (auto-detected: 40/44/48/52 bits)
HA         [39]    1 (if supported)             Hardware Access flag management
HD         [40]    1 (if supported)             Hardware Dirty bit management
```

**Important**: The IPS (Intermediate Physical address Size) field is read from
`ID_AA64MMFR0_EL1` at runtime — the kernel detects how many physical address bits the
CPU supports (typically 40 or 48 bits).

### 3.3 SCTLR_EL1 — System Control Register

`__cpu_setup` computes the SCTLR value and returns it in x0. The value has these flags:

```
SCTLR_EL1 = INIT_SCTLR_EL1_MMU_ON

Key bits set:
  M   (bit 0)  = 1    MMU Enable
  C   (bit 2)  = 1    Data Cache Enable
  I   (bit 12) = 1    Instruction Cache Enable
  WXN (bit 19) = 1    Write permission implies Execute-Never
                       (prevents executing writable memory — security feature)
```

### 3.4 TLB Invalidation

Before enabling the MMU, `__cpu_setup` also executes:

```
    tlbi    vmalle1         // Invalidate all TLB entries at EL1
    dsb     nsh             // Data Synchronization Barrier
    isb                     // Instruction Synchronization Barrier
```

This ensures no stale TLB entries exist from firmware or bootloader.

---

## 4. Enabling the MMU: `__primary_switch` → `__enable_mmu`

**Source**: `arch/arm64/kernel/head.S`

This is the most delicate moment in the boot process — the transition from physical to
virtual addressing.

### 4.1 `__primary_switch`

```
__primary_switch:
    │
    ├──► Load reserved_pg_dir into x1    (empty page table for TTBR1)
    ├──► Load init_idmap_pg_dir into x2  (identity map for TTBR0)
    │
    ├──► bl __enable_mmu                 ★ MMU TURNS ON HERE ★
    │
    │    After this point:
    │    ─ TTBR0 = identity map (VA == PA for kernel image)
    │    ─ TTBR1 = empty/reserved (no kernel VA mapping yet)
    │    ─ CPU executes from TTBR0 identity map
    │
    ├──► bl __pi_early_map_kernel        Map kernel at virtual address
    │                                    (This is Layer 3)
    │
    ├──► ldr x8, =__primary_switched    Load VIRTUAL address
    └──► br  x8                          Jump to virtual address!
                                         (Now using TTBR1)
```

### 4.2 `__enable_mmu`

```
__enable_mmu:
    │
    ├──► Check if selected granule size is supported
    │    (read ID_AA64MMFR0_EL1, verify 4K/16K/64K is available)
    │
    ├──► msr ttbr0_el1, x2     Load identity map into TTBR0
    ├──► msr ttbr1_el1, x1     Load reserved (empty) PGD into TTBR1
    │
    ├──► isb                    Synchronization barrier
    │
    └──► set_sctlr_el1 x0      Write SCTLR with M=1 → MMU IS NOW ON
         │
         │   The VERY NEXT instruction fetch uses the MMU.
         │   Because TTBR0 has the identity map, the CPU
         │   translates its current physical PC to the same
         │   physical address → execution continues normally.
         │
         └──► ret               Return to __primary_switch
```

### 4.3 Why It Works: The Identity Map Trick

```
BEFORE __enable_mmu:
    CPU PC = 0x4080_1234 (physical)
    MMU OFF → fetches from physical 0x4080_1234

AFTER __enable_mmu (MMU ON):
    CPU PC = 0x4080_1238 (next instruction, still looks "physical")
    MMU ON → TTBR0 identity map translates:
        VA 0x4080_1238 → PA 0x4080_1238 (identity!)
    → Same physical location, execution continues!

LATER (after Layer 3 maps kernel at high VA):
    Jump to 0xFFFF_8000_1000_0000 (kernel virtual address)
    MMU ON → TTBR1 translates:
        VA 0xFFFF_8000_1000_0000 → PA 0x4080_0000 (kernel physical)
    → Now running from kernel's virtual address space!
```

---

## 5. State After Layer 2

```
┌────────────────────────────────────────────────────────────────┐
│              CPU STATE AFTER __enable_mmu                       │
│                                                                │
│  MMU:          ON ✓                                             │
│  D-cache:      ON ✓                                             │
│  I-cache:      ON ✓                                             │
│                                                                │
│  TTBR0:        init_idmap_pg_dir (identity map)                 │
│                VA == PA for kernel image                        │
│                                                                │
│  TTBR1:        reserved_pg_dir (empty — no kernel VA yet)       │
│                                                                │
│  MAIR:         5 memory types configured                        │
│  TCR:          VA/PA sizes set, granule configured               │
│  SCTLR:       MMU + Caches + WXN enabled                        │
│                                                                │
│  Execution:    Running from TTBR0 identity map                  │
│                Physical address == Virtual address               │
│                                                                │
│  What exists:                                                   │
│    ✓ Identity map page tables (init_idmap_pg_dir)               │
│    ✗ Kernel virtual mapping (not yet — Layer 3)                 │
│    ✗ Linear map of RAM (not yet — Layer 6)                      │
│    ✗ Any memory allocator (not yet — Layers 5-8)                │
│                                                                │
│  Still using physical addresses for everything!                 │
│  The kernel cannot access arbitrary RAM yet.                    │
└────────────────────────────────────────────────────────────────┘
```

---

## Summary: What Happened in Layer 2

| Step | Function | What It Did |
|------|----------|-------------|
| 1 | `record_mmu_state` | Detected MMU was off (saved in x19) |
| 2 | `preserve_boot_args` | Saved FDT pointer from x0 (saved in x21) |
| 3 | `create_init_idmap` | Built identity map page tables (PA==VA for kernel) |
| 4 | Cache maintenance | Ensured page table coherency |
| 5 | `init_kernel_el` | Handled EL2→EL1 transition if needed |
| 6 | `__cpu_setup` | Configured MAIR, TCR, prepared SCTLR value |
| 7 | `__enable_mmu` | Loaded TTBRs, set SCTLR.M=1 — **MMU is ON** |

**Next**: The kernel still runs at physical addresses (via identity map). Layer 3 will
map the kernel at its proper high virtual address and jump there.

---

[← Layer 1: Hardware Entry](01_Hardware_Entry_and_Bootloader.md) | **Layer 2** | [Layer 3: Kernel Virtual Mapping →](03_Kernel_Virtual_Mapping.md)
