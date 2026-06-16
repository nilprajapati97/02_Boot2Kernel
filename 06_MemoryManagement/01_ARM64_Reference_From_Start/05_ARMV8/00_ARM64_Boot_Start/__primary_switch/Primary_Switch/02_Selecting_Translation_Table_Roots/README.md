# 02 — Selecting Translation Table Roots (TTBR0 / TTBR1)

**Source file:** `arch/arm64/kernel/head.S`  
**Function:** `__primary_switch`  
**Lines in focus:**

```asm
SYM_FUNC_START_LOCAL(__primary_switch)
    adrp    x1, reserved_pg_dir        // TTBR1_EL1 — kernel half
    adrp    x2, __pi_init_idmap_pg_dir // TTBR0_EL1 — identity map (low half)
    bl      __enable_mmu
```

---

## Background — AArch64 Split Virtual Address Space

AArch64 uses a **split VA space** controlled by two Translation Table Base Registers:

| Register   | VA Range                     | Purpose at boot                        |
|------------|------------------------------|----------------------------------------|
| `TTBR0_EL1` | `0x0000_xxxx_xxxx_xxxx` (low)  | Identity map — used during MMU-on transition |
| `TTBR1_EL1` | `0xFFFF_xxxx_xxxx_xxxx` (high) | Kernel virtual address space           |

The split point is controlled by `T0SZ` and `T1SZ` fields in `TCR_EL1`, which were programmed by `__cpu_setup` before arriving here.

---

## 1. TTBR1 — `reserved_pg_dir` (Kernel Half)

```asm
adrp    x1, reserved_pg_dir
```

### What is `reserved_pg_dir`?

Defined in the linker script `arch/arm64/kernel/vmlinux.lds.S`:

```ld
reserved_pg_dir = .;
. += PAGE_SIZE;

swapper_pg_dir = .;
. += PAGE_SIZE;
```

- It is exactly **one PAGE_SIZE (4 KB)** before `swapper_pg_dir`.
- It is a **blank / zeroed page directory** — intentionally has no valid mappings.
- Its purpose is to act as a **safe sentinel for TTBR0** (and temporarily TTBR1) where no translation can succeed, preventing accidental user-space access.

### Why use `reserved_pg_dir` as TTBR1 here?

At this point in boot, the **final kernel page tables are not yet built**.  
`__pi_early_map_kernel` (called just after `__enable_mmu`) will construct the real kernel mappings and later update TTBR1 to `swapper_pg_dir`.

Using `reserved_pg_dir` as TTBR1 during MMU enable ensures:
- The CPU has a valid (but empty) root page table to walk.
- No stale or incorrect kernel virtual translations are served.
- Any kernel VA access before `__pi_early_map_kernel` finishes will fault safely.

### Relationship to `swapper_pg_dir`

```c
/* arch/arm64/include/asm/memory.h */
#define RESERVED_SWAPPER_OFFSET  (PAGE_SIZE)
// swapper_pg_dir - reserved_pg_dir == PAGE_SIZE (one page apart)
```

```c
/* arch/arm64/include/asm/mmu_context.h */
/*
 * Set TTBR0 to reserved_pg_dir.
 * No translations will be possible via TTBR0.
 */
static inline void cpu_set_reserved_ttbr0_nosync(void)
{
    unsigned long ttbr = phys_to_ttbr(__pa_symbol(reserved_pg_dir));
    write_sysreg(ttbr, ttbr0_el1);
}
```

The same `reserved_pg_dir` is also used **at runtime** (after boot) to blank TTBR0 when switching to a kernel thread with no user address space.

---

## 2. TTBR0 — `__pi_init_idmap_pg_dir` (Low Half / Identity Map)

```asm
adrp    x2, __pi_init_idmap_pg_dir
```

### What is `__pi_init_idmap_pg_dir`?

- The **position-independent** identity map page directory.
- Built earlier in `primary_entry` by `__pi_create_init_idmap`.
- Maps **physical address = virtual address** for a limited range covering:
  - The kernel's own `.idmap.text` section (the code currently executing).
  - Enough of RAM for the early boot sequence.

### Why does TTBR0 need the identity map?

When `SCTLR_EL1.M` is set to 1 (MMU turned ON), the very next instruction fetch must still resolve correctly. The CPU's PC still holds a **physical address** from the pre-MMU world. The identity map guarantees:

```
Virtual Address (PC)  ==  Physical Address
     0x40081234       =>       0x40081234   ✓ (idmap entry)
```

Without this, the first fetch after MMU-on would fault.

### How does `phys_to_ttbr` work?

Inside `__enable_mmu`:

```asm
phys_to_ttbr x2, x2       // convert physical addr to TTBR format
msr          ttbr0_el1, x2 // load TTBR0
```

`phys_to_ttbr` is a macro that handles CNP (Common Not Private) bit and any 52-bit PA adjustments for LPA2, then writes the formatted value into `TTBR0_EL1`.

---

## 3. Flow Through `__enable_mmu`

`__enable_mmu` (`arch/arm64/kernel/head.S`) receives the two roots:

```asm
// Inputs:
//   x0 = SCTLR_EL1 value (MMU ON) — from __cpu_setup
//   x1 = TTBR1_EL1 value          — reserved_pg_dir physical addr
//   x2 = TTBR0_EL1 / ID map root  — __pi_init_idmap_pg_dir physical addr

SYM_FUNC_START(__enable_mmu)
    mrs     x3, ID_AA64MMFR0_EL1              // Read CPU memory model features
    ubfx    x3, x3, #ID_AA64MMFR0_EL1_TGRAN_SHIFT, 4
    cmp     x3, #ID_AA64MMFR0_EL1_TGRAN_SUPPORTED_MIN
    b.lt    __no_granule_support              // Unsupported granule → park CPU
    cmp     x3, #ID_AA64MMFR0_EL1_TGRAN_SUPPORTED_MAX
    b.gt    __no_granule_support

    phys_to_ttbr x2, x2
    msr     ttbr0_el1, x2                     // ← TTBR0 = identity map root
    load_ttbr1 x1, x1, x3                    // ← TTBR1 = reserved_pg_dir root

    set_sctlr_el1   x0                       // ← MMU ON (M bit = 1)
    ret
SYM_FUNC_END(__enable_mmu)
```

### Granule check
Before writing any TTBR, `__enable_mmu` reads `ID_AA64MMFR0_EL1` to verify the CPU supports the configured granule size (4K/16K/64K). If unsupported, the CPU is parked in `__no_granule_support` with a `wfe/wfi` loop and the boot status register is flagged with `CPU_STUCK_REASON_NO_GRAN`.

---

## 4. Memory Layout at This Point

```
Physical Memory (simplified):

  +---------------------------+
  | __pi_init_idmap_pg_dir    |  <-- x2 (TTBR0_EL1)
  |  (identity map tables)    |      PA == VA mapping for .idmap.text
  +---------------------------+
  | reserved_pg_dir           |  <-- x1 (TTBR1_EL1)
  |  (empty page, 4KB)        |      No kernel VA translations yet
  +---------------------------+
  | swapper_pg_dir            |      Will become TTBR1 after early_map_kernel
  |  (empty, 4KB)             |
  +---------------------------+
```

---

## 5. What Happens to These Roots After This Function?

| Stage                         | TTBR0_EL1           | TTBR1_EL1              |
|-------------------------------|---------------------|------------------------|
| `bl __enable_mmu` (this step) | `init_idmap_pg_dir` | `reserved_pg_dir`      |
| `bl __pi_early_map_kernel`    | `init_idmap_pg_dir` | Updated to `swapper_pg_dir` (real kernel map built) |
| `br x8` → `__primary_switched` | Still idmap       | `swapper_pg_dir`       |
| `start_kernel()` onward       | Per-process TTBR0   | `swapper_pg_dir`       |

---

## 6. Key Takeaways

- `reserved_pg_dir` → safe empty TTBR1 root; prevents any kernel VA resolution before real tables are ready.
- `__pi_init_idmap_pg_dir` → identity map TTBR0 root; allows CPU to keep fetching instructions after MMU is turned on.
- The two-root design ensures a **safe and crash-free MMU-on transition**.
- Granule size is verified before any TTBR write; unsupported granule = CPU parked.
- Both roots are physical addresses converted via `phys_to_ttbr` before being written to hardware registers.

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