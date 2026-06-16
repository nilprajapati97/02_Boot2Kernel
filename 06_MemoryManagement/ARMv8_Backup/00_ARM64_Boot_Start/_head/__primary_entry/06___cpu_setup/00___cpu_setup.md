# `__cpu_setup` — ARM64 CPU MMU Preparation

**File**: `arch/arm64/mm/proc.S`
**Section**: `.idmap.text` (must be identity-mapped — called with MMU off)
**Called from**: `primary_entry` (after `init_kernel_el`)
**Also called from**: `secondary_startup` (secondary CPUs)

---

## Purpose

`__cpu_setup` is the **last step before turning the MMU on**. By the time
it is called:
- Exception level has been configured (`init_kernel_el` done)
- Identity-map page tables are built (`__pi_create_init_idmap` done)
- Cache maintenance is complete

`__cpu_setup` programs the CPU's memory subsystem registers
(`MAIR_EL1`, `TCR_EL1`, `TCR2_EL1`) with the correct values for Linux,
applies CPU-specific errata workarounds, and returns the value that will
be written to `SCTLR_EL1` to actually **enable the MMU** — but it does
NOT flip the MMU on itself. That happens in `__enable_mmu`.

---

## Return Value

```
x0 = INIT_SCTLR_EL1_MMU_ON
```

This is the SCTLR_EL1 value with:

| Bit            | Value | Meaning                              |
|----------------|-------|--------------------------------------|
| `M`            | 1     | MMU enabled                          |
| `C`            | 1     | D-cache enabled                      |
| `I`            | 1     | I-cache enabled                      |
| `SA`           | 1     | Stack alignment check at EL1         |
| `SA0`          | 1     | Stack alignment check at EL0         |
| `SPAN`         | 1     | SMAP protection enabled (PAN)        |
| `nTWE`         | 1     | WFE not trapped from EL0             |
| `UCI`          | 1     | Cache instructions accessible at EL0 |
| `EPAN`         | 1     | Enhanced PAN                         |
| `EOS`          | 1     | Exception exit is context sync       |
| `EIS`          | 1     | Exception entry is context sync      |

Caller (`__primary_switch` → `__enable_mmu`) writes this value to
`SCTLR_EL1` to enable the MMU.

---

## Call Flow

```
primary_entry
│
└── bl __cpu_setup                    arch/arm64/mm/proc.S
        │
        ├── [1] TLB Invalidation
        │     tlbi vmalle1            Invalidate all EL1 TLB entries
        │     dsb  nsh                Wait for TLB invalidation to complete
        │                             (Non-Shareable domain — local CPU only)
        │     Reason: stale TLB entries from bootloader could cause
        │     incorrect translations when MMU is enabled
        │
        ├── [2] Reset CPU Access Controls
        │     msr CPACR_EL1 = 0      Disable FP/SIMD/SVE traps
        │                             (will be re-enabled by kernel later)
        │     msr MDSCR_EL1 = TDCC   Reset debug control register
        │                             Disable DCC (Debug Comms Channel) from EL0
        │     reset_pmuserenr_el0     Disable PMU access from EL0
        │     reset_amuserenr_el0     Disable AMU access from EL0
        │     Reason: ensure no unprivileged access to debug/perf hardware
        │
        ├── [3] Set MAIR_EL1 — Memory Attribute Indirection Register
        │
        │     MAIR_EL1 is an 8-entry lookup table.
        │     Each entry (8 bits) defines memory type attributes.
        │     Page table entries use a 3-bit index (MT_*) to point here.
        │
        │     Index   Attribute Type        Use
        │     ─────────────────────────────────────────────────────
        │     MT_DEVICE_nGnRnE    0x00  Device: no gather/reorder/early-ack
        │                               Strictest device memory (MMIO)
        │     MT_DEVICE_nGnRE     0x04  Device: no gather/reorder
        │                               Less strict device (PCIe etc.)
        │     MT_NORMAL_NC        0x44  Normal Non-Cacheable RAM
        │     MT_NORMAL           0xff  Normal Cached RAM (writeback)
        │     MT_NORMAL_TAGGED    0xf0  Normal Cached + MTE tag support
        │
        ├── [4] Build TCR_EL1 — Translation Control Register
        │
        │     TCR_EL1 controls the MMU's address translation behavior.
        │     Built up from multiple fields:
        │
        │     Field              Value / Meaning
        │     ─────────────────────────────────────────────────────
        │     T0SZ (TTBR0 size) = 64 - IDMAP_VA_BITS
        │                         Controls VA range for user space (TTBR0)
        │     T1SZ (TTBR1 size) = 64 - VA_BITS_MIN
        │                         Controls VA range for kernel space (TTBR1)
        │     Cache flags         Inner/outer writeback cacheable page walks
        │     Shareability        Inner shareable page table walks
        │     Granule flags       4KB / 16KB / 64KB page granule
        │     KASLR flags         Top-byte ignore for KASLR tagging
        │     AS (ASID size)      16-bit ASID support
        │     TBI0                Top byte ignore for TTBR0 (user pointers)
        │     A1                  ASID defined by TTBR1 (kernel controls ASID)
        │
        ├── [5] Apply CPU Errata to TCR_EL1
        │     tcr_clear_errata_bits tcr
        │     Clears TCR bits that trigger known CPU bugs on this specific
        │     microarchitecture (identified at compile time or via
        │     alternative patching)
        │
        ├── [6] VA_BITS_52 adjustment (if CONFIG_ARM64_VA_BITS_52)
        │     alternative_if ARM64_HAS_VA52
        │       tcr_set_t1sz tcr   Expand kernel VA space to 52-bit
        │       [LPA2] orr TCR_EL1_DS  Enable 52-bit output address support
        │     Reason: ARMv8.2-LVA allows 52-bit virtual addresses on
        │     CPUs that support it — doubles kernel address space
        │
        ├── [7] Compute and set IPS (Intermediate Physical address Size)
        │     tcr_compute_pa_size tcr, TCR_EL1_IPS_SHIFT
        │     Reads ID_AA64MMFR0_EL1.PARange field
        │     Sets TCR_EL1.IPS to maximum PA size the CPU supports:
        │       0b000 = 32-bit PA (4GB)
        │       0b001 = 36-bit PA (64GB)
        │       0b010 = 40-bit PA (1TB)
        │       0b101 = 48-bit PA (256TB)
        │       0b110 = 52-bit PA (4PB)
        │
        ├── [8] Hardware Access Flag / Dirty Bit Management
        │     (if CONFIG_ARM64_HW_AFDBM)
        │     Read ID_AA64MMFR1_EL1.HAFDBS
        │     If CPU supports hardware AF update:
        │       orr TCR_EL1_HA → CPU auto-sets Access Flag in PTEs
        │       Eliminates need for kernel to take AF faults for
        │       every first access to a page
        │     [HAFT] If CPU supports HAFT:
        │       orr TCR2_EL1_HAFT → Hardware managed Access Flag
        │       for table descriptors too
        │
        ├── [9] Write MAIR_EL1 and TCR_EL1 to hardware
        │     msr mair_el1, mair
        │     msr tcr_el1,  tcr
        │     These are now active and will govern all future
        │     page table walks once MMU is enabled
        │
        ├── [10] Permission Indirection Extension (PIE)
        │     (if CPU advertises ID_AA64MMFR3_EL1.S1PIE)
        │     Write PIRE0_EL1 = PIE_E0_ASM   (EL0 permission overlay)
        │     Write PIR_EL1   = PIE_E1_ASM   (EL1 permission overlay)
        │     orr TCR2_EL1_PIE
        │     PIE allows overlaying additional permission controls on
        │     top of existing PTE permission bits (ARMv8.9)
        │
        ├── [11] Write TCR2_EL1 (if CPU supports FEAT_TCR2)
        │     (if ID_AA64MMFR3_EL1.TCRX != 0)
        │     msr TCR2_EL1, tcr2
        │     TCR2 extends TCR with additional controls:
        │       PIE enable, HAFT enable, etc.
        │
        ├── [12] Prepare SCTLR_EL1 return value
        │     mov x0, INIT_SCTLR_EL1_MMU_ON
        │     Does NOT write to SCTLR_EL1 here
        │     Returns value to caller — MMU enable deferred to __enable_mmu
        │     Reason: TTBR0/TTBR1 must be loaded first (done in __enable_mmu)
        │             so page tables are in place before M bit is set
        │
        └── ret → primary_entry → b __primary_switch
                  x0 = INIT_SCTLR_EL1_MMU_ON
```

---

## Register Map — What Gets Programmed

### MAIR_EL1 — Memory Type Lookup Table

```
Bits [63:0]  =  8 entries × 8 bits each

  [7:0]   Index 0 (MT_DEVICE_nGnRnE) = 0x00  ← strictest MMIO
  [15:8]  Index 1 (MT_DEVICE_nGnRE)  = 0x04  ← normal device
  [23:16] Index 2 (MT_NORMAL_NC)     = 0x44  ← non-cached RAM
  [31:24] Index 3 (MT_NORMAL)        = 0xff  ← cached RAM (WB)
  [39:32] Index 4 (MT_NORMAL_TAGGED) = 0xf0  ← MTE-tagged RAM
```

Page table entries (PTEs) use a 3-bit `AttrIndx` field to index into
this table. This decouples the PTE encoding from the actual memory
attributes, allowing attributes to be changed by updating MAIR_EL1
rather than walking all page tables.

### TCR_EL1 — Translation Control Register

```
  T0SZ    [5:0]   VA size for TTBR0 (user space)
  T1SZ   [21:16]  VA size for TTBR1 (kernel space)
  IRGN0  [9:8]    Inner cache attrs for TTBR0 walks
  ORGN0 [11:10]   Outer cache attrs for TTBR0 walks
  SH0   [13:12]   Shareability for TTBR0 walks
  TG0   [15:14]   Granule for TTBR0 (4KB/16KB/64KB)
  IRGN1 [25:24]   Inner cache attrs for TTBR1 walks
  ORGN1 [27:26]   Outer cache attrs for TTBR1 walks
  SH1   [29:28]   Shareability for TTBR1 walks
  TG1   [31:30]   Granule for TTBR1
  IPS   [34:32]   Intermediate physical address size
  AS      [36]    ASID size (0=8-bit, 1=16-bit)
  TBI0    [37]    Top byte ignore for TTBR0 VA
  HA      [39]    Hardware access flag update
  A1      [22]    ASID from TTBR1 (kernel owns ASID space)
```

---

## Why `__cpu_setup` Returns SCTLR Value Instead of Setting It

This is a common interview question.

`__cpu_setup` sets `MAIR_EL1` and `TCR_EL1` but **does not** write
`SCTLR_EL1`. Instead, it returns the target value in `x0`.

The caller chain is:

```
primary_entry
  bl __cpu_setup          → x0 = INIT_SCTLR_EL1_MMU_ON
  b  __primary_switch
       bl __enable_mmu    → loads TTBR0, TTBR1 FIRST
                          → THEN writes x0 to SCTLR_EL1 (MMU ON)
```

If `__cpu_setup` wrote `SCTLR_EL1.M = 1` directly:
- TTBR0/TTBR1 would not yet point to the identity-map page tables
- The very next instruction fetch after `M=1` would fault
- There would be no valid mapping for the current PC

By splitting the responsibility:
- `__cpu_setup` = prepare all supporting registers (MAIR, TCR)
- `__enable_mmu` = load page tables **then** flip M bit atomically

---

## Key Design Decisions

| Decision | Reason |
|----------|--------|
| TLB invalidate first | Bootloader TLB entries must not pollute early kernel translations |
| `dsb nsh` after `tlbi` | TLB op must complete before any memory access that depends on it |
| MAIR separate from PTE | Decouples memory type from PTE encoding; easier to change types without page table walk |
| TCR built dynamically | PA size, VA size, granule size vary per CPU — must be read from ID registers at runtime |
| Hardware AF (HA bit) | Eliminates Access Flag faults on first page access — significant performance gain |
| Return SCTLR, don't set it | Page tables must be installed in TTBRs before MMU enable — see above |
| `.idmap.text` section | This code runs with MMU off; must be identity-mapped so it works at its physical load address |
