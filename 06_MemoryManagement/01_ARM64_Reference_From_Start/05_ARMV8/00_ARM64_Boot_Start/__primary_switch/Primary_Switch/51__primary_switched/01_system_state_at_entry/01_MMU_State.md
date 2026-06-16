# MMU State at Entry — TTBR0, TTBR1, SCTLR_EL1

## The MMU Is ON — What Exactly That Means

When `__primary_switched` begins executing, the Memory Management Unit is fully
active. Three registers define the complete MMU state:

---

## SCTLR_EL1 — System Control Register

```
SCTLR_EL1 relevant bits at entry:
  Bit [0]  M   = 1  → MMU enabled
  Bit [2]  C   = 1  → D-cache enabled
  Bit [12] I   = 1  → I-cache enabled
  Bit [25] EE  = 0  → Little-endian (for LE kernels)
  Bit [19] WXN = 0  → Write does NOT imply Execute-Never (relaxed for early boot)
```

**Who set this:** `set_sctlr_el1 x0` at the end of `__enable_mmu` in `__primary_switch`.
This is a macro that writes to SCTLR_EL1 and issues a context synchronization event.

---

## TCR_EL1 — Translation Control Register (set during `__cpu_setup`)

```
TCR_EL1 key fields:
  T0SZ = 64 - VA_BITS_MIN  → size of TTBR0 address space (small, for identity map)
  T1SZ = 64 - VA_BITS       → size of TTBR1 address space (full kernel VA)
  TG0  = 0b00               → TTBR0 granule = 4KB
  TG1  = 0b10               → TTBR1 granule = 4KB
  IRGN0/ORGN0               → Inner/Outer Write-Back Cacheable, Read-Allocate
  IRGN1/ORGN1               → Inner/Outer Write-Back Cacheable, Read-Allocate
  SH0/SH1 = 0b11            → Inner Shareable
  EPD0 = 0                  → TTBR0 translation NOT disabled
  EPD1 = 0                  → TTBR1 translation NOT disabled
  IPS = configured           → Intermediate Physical Address size
```

---

## TTBR0_EL1 — Identity Map

**Points to:** `idmap_pg_dir` (identity page table)

**VA range covered:** `[0x0000_0000_0000_0000 .. 2^(64-T0SZ)-1]`
With `T0SZ = 64 - VA_BITS_MIN = 64 - 48 = 16`, this covers `[0..0x0000_FFFF_FFFF_FFFF]`.

**What it maps:**
```
VA 0x40080000  →  PA 0x40080000  (kernel `.idmap.text` — 1:1 mapping)
VA 0x4000_xxxx  →  PA 0x4000_xxxx  (early boot code region)
```

**Purpose at this point:**
- Allows the CPU to still read the literal pool for `ldr x8, =__primary_switched`
  which was in `.idmap.text` (identity-mapped low physical address)
- After `br x8`, the PC jumps to TTBR1 range — TTBR0 is no longer used for code
  execution on the primary CPU

**Security concern:** Identity map allows physical addresses to be read as virtual.
The kernel eventually disables TTBR0 (sets `TCR_EL1.EPD0 = 1`) after switching to
the user page table mechanism, preventing user-space exploits from using identity map.

---

## TTBR1_EL1 — Kernel Page Tables

**Points to:** `swapper_pg_dir` (the permanent kernel page table)

**VA range covered:** `[0xFFFF_8000_0000_0000 .. 0xFFFF_FFFF_FFFF_FFFF]`
(all addresses with top bit set, using TTBR1 when VA[63] = 1)

**What it maps (as built by `__pi_early_map_kernel`):**
```
0xFFFFFF80_10080000  →  0x40080000  (kernel .text)
0xFFFFFF80_10xxxxxx  →  0x400xxxxx  (kernel .data, .bss, .init)
0xFFFFFF80_00000000  →  RAM_BASE    (linear map — all physical RAM accessible as VA)
[KASAN shadow region] →  mapped by kasan_early_init (not yet at entry)
```

**Page table format:** ARM64 uses a 4-level page table:
```
PGD (L0) → PUD (L1) → PMD (L2) → PTE (L3)
 48-bit VA: [47:39] [38:30] [29:21] [20:12] [11:0]
              PGD     PUD     PMD     PTE    offset
```

---

## Cache State

**I-cache (Instruction Cache):**
- `SCTLR_EL1.I = 1` — Instructions fetched from cache
- Cache was invalidated early in boot to remove stale pre-MMU speculative lines
- `dcache_clean_poc` / `dcache_inval_poc` called on relevant regions

**D-cache (Data Cache):**
- `SCTLR_EL1.C = 1` — Data accessed via cache
- Write-back, write-allocate policy (from TCR Inner/Outer attributes)
- All `str_l`, `stp`, `str` instructions in `__primary_switched` hit D-cache

**TLB state:**
- Populated by `__enable_mmu` and `__pi_early_map_kernel`
- Both the identity map (TTBR0) and kernel map (TTBR1) entries are in TLB
- `TLBI VMALLE1` was issued during `__cpu_setup` to clear stale TLB entries

---

## Two-Map Overlap — Why Both TTBR0 and TTBR1 Are Active

At entry to `__primary_switched`, BOTH TTBR0 and TTBR1 are active simultaneously:

```
VA lookup rule (ARM64):
  VA[63:VA_BITS] = all 0 → use TTBR0 (identity map)
  VA[63:VA_BITS] = all 1 → use TTBR1 (kernel map)
  Other values → Translation Fault (TCR_EL1 fault)
```

The PC is in TTBR1 range (all-ones prefix) — so all instruction fetches use TTBR1.
The `str_l` macro uses `adrp` (PC-relative, gives TTBR1 VA) — all data accesses use TTBR1.

TTBR0 is still live but DORMANT from the kernel's perspective at this point. It will
be actively used again when user processes run (each process installs its own TTBR0).

---

## MAIR_EL1 — Memory Attribute Indirection Register

Configured in `__cpu_setup` before MMU enable:
```
Attr0 = 0xFF  → Normal, Inner/Outer Write-Back Non-transient, Read/Write Allocate
Attr1 = 0x04  → Device-nGnRE (Device memory, Non-Gathering, Non-Reordering, Early-ACK)
Attr2 = 0x00  → Device-nGnRnE (Strongly ordered)
Attr3 = 0x44  → Normal, Inner/Outer Write-Through Non-transient
```

The page table entries reference these attribute indices via `AttrIdx[2:0]`.
Kernel text/data use `Attr0` (Normal WB). MMIO uses `Attr1` (Device-nGnRE).

---

## ARMv8 CPU / Kernel / Memory Context

### CPU Perspective (ARMv8-A)
This document describes a stage in the ARMv8-A Linux ARM64 boot path. ARMv8-A is the 64-bit ARM architecture (AArch64 execution state) introduced with the ARM Cortex-A53/A57 generation. Key architectural features relevant to boot:
- Exception levels: EL0 (user), EL1 (OS kernel), EL2 (hypervisor), EL3 (secure monitor).
- Two-stage translation: TTBR0_EL1 (user/low VA) and TTBR1_EL1 (kernel/high VA).
- System registers accessed via MRS/MSR instructions (not memory-mapped).
- PSTATE: condition flags + CPU mode + interrupt mask bits.
- Mandatory ISB after system register writes that affect instruction fetch.

### Kernel Perspective (Linux ARM64)
The Linux ARM64 boot path follows this sequence:
  stext (head.S) -> __primary_switch -> __pi_early_map_kernel -> __enable_mmu
  -> __primary_switched -> start_kernel -> setup_arch -> paging_init
Each stage initializes one more layer of the memory system. Before start_kernel, all memory management is done with physical addresses or the early identity/kernel maps. After paging_init(), the full kernel virtual memory map is active.

### Memory Perspective (ARMv8 Memory Model)
The ARMv8 memory model (based on the ARM ARM's "Arm Memory Model" chapter) defines:
- Normal memory: cacheable, reorderable, speculatable. Used for DRAM (kernel code, data, stack, heap).
- Device memory: non-cacheable, strictly ordered. Used for MMIO (UART, GIC, etc.).
- Barriers: DSB (Data Synchronization Barrier), DMB (Data Memory Barrier), ISB (Instruction Synchronization Barrier) enforce ordering guarantees.
At boot, the kernel transitions from a world where every address is physical (pre-MMU) to the full ARMv8 virtual memory model where TTBR0 and TTBR1 map the user and kernel address spaces respectively.