# `adrp x1, reserved_pg_dir` — CPU Level and Memory Level Deep Dive

**Function:** `__primary_switch`  
**Source:** `arch/arm64/kernel/head.S` | `arch/arm64/kernel/vmlinux.lds.S`  
**Context:** First instruction inside `__primary_switch`. MMU is **OFF**. Running at physical address in `.idmap.text`.

---

## The Instruction in Context

```asm
SYM_FUNC_START_LOCAL(__primary_switch)
    adrp    x1, reserved_pg_dir        // ← THIS INSTRUCTION
    adrp    x2, __pi_init_idmap_pg_dir
    bl      __enable_mmu
```

`x1` loaded here becomes `TTBR1_EL1` — the kernel-half translation table base — when `__enable_mmu` writes it to the hardware register.

---

## Part 1 — The `adrp` Instruction at CPU Hardware Level

### What `adrp` means

`adrp` = **Address of Page, PC-relative**

It computes a **4KB-page-aligned physical address** relative to the current Program Counter (PC) and writes it into the destination register. It performs **no memory access** — it is pure arithmetic inside the CPU pipeline.

### Instruction Encoding

```
31 30 29 28 27 26 25 24 23 ... 5  4 3 2 1 0
 1  immlo[1:0]  1  0  0  0  0   immhi[18:0]  Rd[4:0]
```

- `Rd` = destination register (x1 here)
- `imm` = signed 21-bit page offset (immhi:immlo), computed by the linker at link time

### Step-by-Step CPU Execution

**Step 1 — Read the current PC (physical address)**

```
PC = 0x4008_1350   (example — physical address of this adrp instruction)
```

**Step 2 — Page-align the PC (strip bottom 12 bits)**

```
PC_page = PC & ~0xFFF
        = 0x4008_1350 & 0xFFFF_F000
        = 0x4008_1000
```

**Step 3 — Compute the signed page-relative offset**

The assembler encoded at link time:
```
imm21 = PAGE(reserved_pg_dir) - PAGE(adrp_instruction)
      = (reserved_pg_dir & ~0xFFF) - (PC & ~0xFFF)
      = some signed integer N  (number of 4KB pages away)
```

**Step 4 — Scale and add**

```
x1 = PC_page + (sign_extend(imm21) << 12)
   = physical address of the 4KB page containing reserved_pg_dir
```

Since `reserved_pg_dir` is itself page-aligned (see linker script), this is **exactly** the physical address of `reserved_pg_dir`.

### CPU Pipeline Units Involved

| Pipeline Unit | What Happens |
|---|---|
| **Instruction Fetch** | Reads 4-byte opcode from physical RAM (MMU off → direct, no TLB) |
| **Instruction Decoder** | Decodes `0x90xxxxxx` as `adrp`, extracts Rd=x1, imm21 |
| **PC Register** | Provides physical address to the ALU |
| **ALU** | Page-aligns PC, sign-extends + shifts imm21, adds both values |
| **Register File** | Writes result into x1 |

**Nothing else is involved.** No TLB, no D-cache, no memory bus, no MMU.

---

## Part 2 — `reserved_pg_dir` in Memory

### Linker Script Definition (`arch/arm64/kernel/vmlinux.lds.S`)

```ld
idmap_pg_dir  = .;   . += PAGE_SIZE;   // 4 KB — identity-map page tables
                                         // (tramp_pg_dir here if UNMAP_KERNEL_AT_EL0)
reserved_pg_dir = .; . += PAGE_SIZE;   // 4 KB — ALL ZEROS  ← THIS
swapper_pg_dir  = .; . += PAGE_SIZE;   // 4 KB — final kernel page table
```

`reserved_pg_dir` is a **linker symbol** — not a C variable with initializers. It marks the start of a raw 4KB region embedded in the kernel binary. The region is zeroed at boot by `early_map_kernel()` (BSS clear).

### Linker Assert — Exact Offset Guaranteed

```ld
// arch/arm64/kernel/vmlinux.lds.S
ASSERT(swapper_pg_dir - reserved_pg_dir == RESERVED_SWAPPER_OFFSET,
       "RESERVED_SWAPPER_OFFSET is wrong!")

// arch/arm64/include/asm/memory.h
#define RESERVED_SWAPPER_OFFSET  (PAGE_SIZE)   // = 4096 bytes
```

`swapper_pg_dir` is always exactly **4096 bytes (one page)** after `reserved_pg_dir`.

### Physical Memory Layout at This Moment

```
Physical RAM (kernel loaded, example at 0x4000_0000):

  ┌────────────────────────────────┐ ← __idmap_text_start
  │  .idmap.text                   │   Code executing RIGHT NOW
  │  (primary_entry, __primary_switch,
  │   __enable_mmu, ...)           │
  ├────────────────────────────────┤ ← idmap_pg_dir
  │  idmap_pg_dir     [ 4 KB ]     │   Identity-map tables (FILLED)
  ├────────────────────────────────┤ ← reserved_pg_dir  ◄── adrp target
  │  reserved_pg_dir  [ 4 KB ]     │   ALL ZEROS — no valid entries
  ├────────────────────────────────┤ ← swapper_pg_dir
  │  swapper_pg_dir   [ 4 KB ]     │   Empty now, built by __pi_early_map_kernel
  ├────────────────────────────────┤ ← __init_begin
  │  .init.text / .init.data       │
  └────────────────────────────────┘
```

### What a Zeroed 4KB Page Means to the MMU Hardware

Every 8-byte entry in `reserved_pg_dir` = `0x0000_0000_0000_0000`.

Per the AArch64 architecture (ARMv8 DDI 0487, `arch/arm64/include/asm/pgtable-hwdef.h`):

```
PGD_TYPE_TABLE = bits[1:0] = 0b11  →  valid table descriptor
0x0000...0000  = bits[1:0] = 0b00  →  INVALID descriptor
```

When the MMU hardware table walker reads an INVALID descriptor:
- It **immediately raises a Translation Fault**
- It does **not** walk further down the table hierarchy
- The CPU generates a Data Abort (for loads/stores) or Prefetch Abort (for instruction fetches)

So **any virtual address lookup through `reserved_pg_dir` will fault** — by design.

---

## Part 3 — Why `reserved_pg_dir` as TTBR1 During MMU Enable

### AArch64 Split Virtual Address Space

```
VA[63:48] = 0xFFFF → TTBR1_EL1  (kernel space, high VA)
VA[63:48] = 0x0000 → TTBR0_EL1  (user space, low VA)
```

`x1` (= physical address of `reserved_pg_dir`) → becomes `TTBR1_EL1`.

### The Problem `reserved_pg_dir` Solves

When `__enable_mmu` is called:
1. It writes `TTBR1_EL1` and `TTBR0_EL1`
2. Then writes `SCTLR_EL1.M = 1` → **MMU ON**

At this exact moment, `swapper_pg_dir` (the real kernel page tables) **does not exist yet**. It is built inside `__pi_early_map_kernel`, which runs **after** `__enable_mmu` returns.

Timeline:
```
bl  __enable_mmu           ← MMU ON, TTBR1 = reserved_pg_dir (empty, safe)
                                     any kernel VA access → Translation Fault
                                     (correct — no kernel VA should be used yet)

bl  __pi_early_map_kernel  ← builds swapper_pg_dir
                             then calls idmap_cpu_replace_ttbr1(swapper_pg_dir)
                             TTBR1 now → swapper_pg_dir (real kernel tables)

br  x8 → __primary_switched ← first execution in full kernel virtual space
```

Using `reserved_pg_dir` as TTBR1 fills the gap safely — the MMU is active but the kernel half is intentionally unmapped until real tables are ready.

### What `load_ttbr1` Does With This Address (Inside `__enable_mmu`)

```asm
// arch/arm64/include/asm/assembler.h
.macro load_ttbr1, pgtbl, tmp1, tmp2
    phys_to_ttbr  tmp1, pgtbl    // format physical addr for TTBR register
    offset_ttbr1  tmp1, tmp2     // handle 52-bit VA offset (if needed)
    msr           ttbr1_el1, tmp1 // write to TTBR1_EL1 hardware register
    isb                           // instruction sync barrier
.endm
```

`phys_to_ttbr` for standard 48-bit PA builds:
```asm
mov  ttbr, phys    // TTBR format: bits[47:1] = page-table base address
```

For 52-bit PA (LPA2) it additionally encodes high bits:
```asm
orr  ttbr, phys, phys, lsr #46
and  ttbr, ttbr, #TTBR_BADDR_MASK_52
```

After `msr ttbr1_el1, tmp1` + `isb`:
- The hardware MMU is now pointing its kernel-half walker at `reserved_pg_dir`
- Every entry it reads will be `0x0` = INVALID → Translation Fault on any kernel VA

---

## Part 4 — Runtime Reuse of `reserved_pg_dir` (After Boot)

The same `reserved_pg_dir` is used at runtime as a **TTBR0 sentinel** for kernel threads:

```c
// arch/arm64/include/asm/mmu_context.h
/*
 * Set TTBR0 to reserved_pg_dir.
 * No translations will be possible via TTBR0.
 */
static inline void cpu_set_reserved_ttbr0(void)
{
    unsigned long ttbr = phys_to_ttbr(__pa_symbol(reserved_pg_dir));
    write_sysreg(ttbr, ttbr0_el1);
    isb();
}
```

```c
// arch/arm64/kernel/setup.c
// init_task (pid 0, swapper) always has reserved_pg_dir as its TTBR0:
init_task.thread_info.ttbr0 = phys_to_ttbr(__pa_symbol(reserved_pg_dir));
```

When a kernel thread has no user address space, TTBR0 is pointed at `reserved_pg_dir` so any accidental user-VA access faults immediately instead of walking stale user page tables.

---

## Part 5 — Complete Execution Trace for This Instruction

```
State before this instruction:
  MMU       = OFF
  D-Cache   = OFF
  I-Cache   = ON (or OFF)
  PC        = physical address inside .idmap.text
  x0        = SCTLR_EL1_MMU_ON  (from __cpu_setup)
  x19..x21  = boot state registers (callee-saved, untouched by __cpu_setup)

─────────────────────────────────────────────────────
INSTRUCTION:   adrp  x1, reserved_pg_dir
─────────────────────────────────────────────────────

  Fetch:    CPU reads 4 bytes from physical address PC → gets 0x90xxxxxx
            No TLB (MMU off), no D-cache (off), direct to RAM / I-cache

  Decode:   opcode class = PC-relative address form (adrp)
            Rd  = x1
            imm = signed 21-bit page offset to reserved_pg_dir

  Execute:  page_base = PC & ~0xFFF
            offset    = sign_extend(imm21) << 12
            result    = page_base + offset
                      = physical address of reserved_pg_dir (4KB-aligned)

  Writeback: x1 ← result
             PC ← PC + 4

  Memory:   NONE ACCESSED

─────────────────────────────────────────────────────
State after this instruction:
  x1 = physical address of reserved_pg_dir (4KB-aligned, all-zeros page)
  All other registers unchanged
─────────────────────────────────────────────────────
```

---

## Part 6 — Summary

| Item | Value / Explanation |
|---|---|
| Instruction | `adrp x1, reserved_pg_dir` |
| Type | PC-relative arithmetic — no memory access |
| Result in x1 | Physical address of `reserved_pg_dir` (4KB-aligned) |
| `reserved_pg_dir` size | 4 KB |
| `reserved_pg_dir` content | All zeros — every PGD entry = INVALID (bits[1:0]=0b00) |
| Defined in | `arch/arm64/kernel/vmlinux.lds.S` as a linker symbol |
| Offset from `swapper_pg_dir` | Exactly `PAGE_SIZE = 4096` bytes before it |
| Used as | `TTBR1_EL1` root during MMU-on transition (passed to `__enable_mmu` via `x1`) |
| Effect on MMU hardware | Every kernel VA lookup → Translation Fault (intentional) |
| Why not `swapper_pg_dir` | `swapper_pg_dir` doesn't exist yet — built after MMU is on |
| Replaced by | `idmap_cpu_replace_ttbr1(swapper_pg_dir)` inside `__pi_early_map_kernel` |
| Runtime reuse | TTBR0 sentinel for kernel threads with no user address space |

---

*Source references:*  
- `arch/arm64/kernel/head.S` — `__primary_switch`, `__enable_mmu`  
- `arch/arm64/kernel/vmlinux.lds.S` — linker layout, ASSERT  
- `arch/arm64/include/asm/assembler.h` — `load_ttbr1`, `phys_to_ttbr`, `set_sctlr_el1`  
- `arch/arm64/include/asm/mmu_context.h` — `cpu_set_reserved_ttbr0`  
- `arch/arm64/include/asm/memory.h` — `RESERVED_SWAPPER_OFFSET`  
- `arch/arm64/include/asm/pgtable-hwdef.h` — descriptor type definitions  
- `arch/arm64/kernel/pi/map_kernel.c` — `early_map_kernel`, `map_kernel`, `idmap_cpu_replace_ttbr1`

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