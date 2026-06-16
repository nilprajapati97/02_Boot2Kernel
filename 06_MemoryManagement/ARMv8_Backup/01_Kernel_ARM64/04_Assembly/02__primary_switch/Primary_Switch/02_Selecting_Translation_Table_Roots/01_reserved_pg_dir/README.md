# `adrp x1, reserved_pg_dir` — Instruction Reference Overview

**Instruction:** `adrp x1, reserved_pg_dir`  
**Function:** `__primary_switch`  
**Source:** `arch/arm64/kernel/head.S`, Line 509  
**MMU State:** OFF | **Cache State:** D-Cache OFF, I-Cache ON  
**Detailed deep-dive document:** [adrp_x1_reserved_pg_dir_deep_dive.md](adrp_x1_reserved_pg_dir_deep_dive.md)

---

## Instruction in Context

```asm
SYM_FUNC_START_LOCAL(__primary_switch)
    adrp    x1, reserved_pg_dir        // ← x1 = PA of reserved_pg_dir (TTBR1)
    adrp    x2, __pi_init_idmap_pg_dir // ← x2 = PA of idmap (TTBR0)
    bl      __enable_mmu               //    MMU ON using x0, x1, x2
```

`x1` loaded here is passed directly to `__enable_mmu` as the `TTBR1_EL1` value — the root of the kernel-half (high VA) page table.

---

## Topic 1 — CPU Hardware Level: How `adrp` Works

`adrp` is a **PC-relative, page-aligned address computation** instruction. It performs no memory access — it is pure ALU arithmetic.

**What the CPU does:**

```
page_base = PC & ~0xFFF              // strip bottom 12 bits of current PC
offset    = sign_extend(imm21) << 12 // assembler-encoded 21-bit page offset
x1        = page_base + offset       // physical address of reserved_pg_dir page
```

**Key point:** The 21-bit immediate (`imm21`) is computed by the linker at link time as:

```
imm21 = PAGE(reserved_pg_dir) - PAGE(adrp_instruction_address)
```

**Pipeline stages involved:**

| Stage | Activity |
|---|---|
| Fetch | Read 4-byte opcode from physical RAM (no TLB, no D-cache — MMU off) |
| Decode | Identify as `adrp`, extract `Rd=x1`, extract `imm21` |
| Execute | ALU: page-align PC, sign-extend + shift imm21, add |
| Writeback | Write result into x1; PC ← PC + 4 |

No TLB lookup, no cache fill, no memory bus transaction — only register and ALU operations.

---

## Topic 2 — Memory Layout: Linker Script Definition

`reserved_pg_dir` is defined in `arch/arm64/kernel/vmlinux.lds.S`:

```ld
idmap_pg_dir  = .;  . += PAGE_SIZE;   // 4 KB — identity map tables (FILLED)
reserved_pg_dir = .; . += PAGE_SIZE;  // 4 KB — all zeros  ← adrp target
swapper_pg_dir  = .; . += PAGE_SIZE;  // 4 KB — built after MMU-on
```

**Physical memory layout (kernel loaded at example base `0x4000_0000`):**

```
  ┌─────────────────────────────────┐
  │  .idmap.text  (code running now)│
  ├─────────────────────────────────┤ ← idmap_pg_dir     (+0 KB)
  │  idmap_pg_dir   [ 4 KB ]        │   Identity-map PGD (populated)
  ├─────────────────────────────────┤ ← reserved_pg_dir  (+4 KB) ◄── x1 points here
  │  reserved_pg_dir [ 4 KB ]       │   ALL ZEROS — INVALID entries
  ├─────────────────────────────────┤ ← swapper_pg_dir   (+8 KB)
  │  swapper_pg_dir  [ 4 KB ]       │   Empty now (built later)
  └─────────────────────────────────┘
```

**Linker assert — guaranteed spacing:**

```ld
ASSERT(swapper_pg_dir - reserved_pg_dir == RESERVED_SWAPPER_OFFSET, ...)
// RESERVED_SWAPPER_OFFSET = PAGE_SIZE = 4096 bytes  (asm/memory.h)
```

---

## Topic 3 — Why Zeroed = Safe: INVALID PGD Descriptors

The 4 KB region at `reserved_pg_dir` is all-zero bytes — every 8-byte PGD entry is `0x0000_0000_0000_0000`.

**AArch64 descriptor type encoding (`arch/arm64/include/asm/pgtable-hwdef.h`):**

```
bits[1:0] = 0b11 → PGD_TYPE_TABLE  (valid table descriptor)
bits[1:0] = 0b01 → valid block descriptor
bits[1:0] = 0b00 → INVALID         ← what all-zeros gives us
```

**Hardware behaviour when MMU walker reads an INVALID entry:**

```
MMU table walk hits entry = 0x0000...0000
  → bits[1:0] = 0b00 = INVALID
  → table walk aborts immediately
  → CPU raises Translation Fault (Data Abort for loads/stores,
                                  Prefetch Abort for instruction fetch)
```

This is **intentional and correct** — any accidental kernel VA access before real tables are ready should fault loudly rather than silently produce wrong data.

---

## Topic 4 — Why `reserved_pg_dir`, Not `swapper_pg_dir`

`swapper_pg_dir` is the **final kernel page table**. It does not exist at this point in the boot sequence.

**Boot sequence timeline:**

```
bl __cpu_setup
    └─ returns SCTLR_EL1_MMU_ON in x0
b  __primary_switch
    adrp x1, reserved_pg_dir      ← x1 = safe empty table
    adrp x2, __pi_init_idmap_pg_dir
    bl   __enable_mmu              ← MMU ON, TTBR1 = reserved_pg_dir
                                      (swapper_pg_dir does not exist yet)
    bl   __pi_early_map_kernel
        └─ map_kernel()
              └─ builds init_pg_dir entries
              └─ idmap_cpu_replace_ttbr1(init_pg_dir)
              └─ copies to swapper_pg_dir
              └─ idmap_cpu_replace_ttbr1(swapper_pg_dir)  ← TTBR1 updated HERE
    br x8 → __primary_switched     ← first full virtual-space execution
```

`reserved_pg_dir` bridges the gap: the MMU is ON and functional, but the kernel-half is intentionally unmapped until `swapper_pg_dir` is populated. Any premature kernel VA access faults (which is correct — no kernel VA should be used between `__enable_mmu` and `__primary_switched`).

---

## Topic 5 — Complete Execution Trace

```
─────────────────────────────────────────────────────────
STATE BEFORE:
  MMU         = OFF
  D-Cache     = OFF
  I-Cache     = ON (or OFF)
  PC          = physical address inside .idmap.text
  x0          = INIT_SCTLR_EL1_MMU_ON  (returned by __cpu_setup)
  x1          = (unused, caller-saved)
  x20         = CPU boot mode  (EL1 or EL2)
  x21         = physical address of FDT (device tree blob)

─────────────────────────────────────────────────────────
INSTRUCTION:   adrp  x1, reserved_pg_dir
─────────────────────────────────────────────────────────

  Fetch:     CPU reads 4 bytes at physical address PC
             Path: physical RAM → L1 I-cache (if on) → decode
             (No TLB, no D-cache, no MMU — all bypassed)

  Decode:    Opcode class: PC-relative address form (bit 31 = 1, op = adrp)
             Rd  = 1  (x1)
             imm = signed 21-bit page offset  (set by linker)

  Execute:   page_base = PC & ~0xFFF
             offset    = sign_extend(imm21) << 12
             result    = page_base + offset
                       = physical address of reserved_pg_dir

  Writeback: x1 ← result
             PC ← PC + 4

  Memory:    NONE ACCESSED

─────────────────────────────────────────────────────────
STATE AFTER:
  x1 = physical address of reserved_pg_dir  (4KB-aligned, all-zeros page)
  x0, x20, x21 = unchanged
  PC = next instruction (adrp x2, __pi_init_idmap_pg_dir)
─────────────────────────────────────────────────────────
```

---

## Topic 6 — Runtime Reuse: TTBR0 Sentinel for Kernel Threads

After boot, `reserved_pg_dir` serves a second role as the TTBR0 sentinel for kernel threads that have no user address space.

**`arch/arm64/include/asm/mmu_context.h`:**

```c
static inline void cpu_set_reserved_ttbr0(void)
{
    unsigned long ttbr = phys_to_ttbr(__pa_symbol(reserved_pg_dir));
    write_sysreg(ttbr, ttbr0_el1);
    isb();
}
```

**`arch/arm64/kernel/setup.c`:**

```c
// pid 0 (swapper/init_task) has reserved_pg_dir as its user page table root
init_task.thread_info.ttbr0 = phys_to_ttbr(__pa_symbol(reserved_pg_dir));
```

**Why:**

```
Normal user process:
  TTBR0 → process page tables → user VA space accessible

Kernel thread (no user process):
  TTBR0 → reserved_pg_dir (all zeros, all INVALID)
         → any accidental user VA access → Translation Fault immediately
         → prevents kernel from accidentally walking stale user tables
```

This reuse means `reserved_pg_dir` is a **permanent fixture** in the kernel's memory map — it is never repurposed or freed after boot.

---

## Quick Reference

| Property | Value |
|---|---|
| Instruction | `adrp x1, reserved_pg_dir` |
| Operation | PC-relative page-aligned address load — no memory access |
| Result | Physical address of `reserved_pg_dir` (4 KB, all zeros) |
| Used as | `TTBR1_EL1` root passed to `__enable_mmu` |
| Defined in | `arch/arm64/kernel/vmlinux.lds.S` (linker symbol) |
| Size | `PAGE_SIZE` = 4096 bytes |
| Content | All zeros → bits[1:0]=0b00 → INVALID PGD descriptor per ARMv8 spec |
| MMU effect | Every kernel VA lookup → Translation Fault (by design) |
| Replaced at | `__pi_early_map_kernel` → `idmap_cpu_replace_ttbr1(swapper_pg_dir)` |
| Runtime role | TTBR0 sentinel for kernel threads (no user address space) |

---

*Detailed per-topic document:* [adrp_x1_reserved_pg_dir_deep_dive.md](adrp_x1_reserved_pg_dir_deep_dive.md)

*Source files:*
- `arch/arm64/kernel/head.S`
- `arch/arm64/kernel/vmlinux.lds.S`
- `arch/arm64/include/asm/assembler.h`
- `arch/arm64/include/asm/mmu_context.h`
- `arch/arm64/include/asm/memory.h`
- `arch/arm64/include/asm/pgtable-hwdef.h`
- `arch/arm64/kernel/pi/map_kernel.c`

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