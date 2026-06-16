# Entry Conditions Contract — What Is Guaranteed When `__primary_switched` Starts

## The Formal Precondition Set

Every precondition listed here is GUARANTEED by the code that runs before `__primary_switched`.
If any of these were violated, `__primary_switched` would malfunction or hang before
reaching `start_kernel`.

---

## Precondition 1 — MMU Is ON With Correct Page Tables

**State:** `SCTLR_EL1.M = 1`

**TTBR0_EL1:** Points to the identity map page table (`idmap_pg_dir`).
Maps physical addresses to the same virtual addresses (PA = VA) in the low range.
This mapping allows any residual code in `.idmap.text` to still be addressable.

**TTBR1_EL1:** Points to `swapper_pg_dir` (primary CPU) — the kernel's permanent
page tables. Maps the full kernel image at virtual address `PAGE_OFFSET` (~0xFFFFFF80).

**Proof:** Set by `__enable_mmu` in `__primary_switch`:
```asm
msr   ttbr0_el1, x2    // x2 = PA of idmap_pg_dir
load_ttbr1 x1, x1, x3 // x1 = PA of swapper_pg_dir
set_sctlr_el1 x0       // x0 = SCTLR value with M=1, C=1, I=1
```

**TLB state:** Populated by `__enable_mmu` and `__pi_early_map_kernel`. No TLB
invalidation is needed at this point — the entries are consistent.

---

## Precondition 2 — PC Is in Kernel Virtual Address Space

**PC value:** `~0xFFFFFF80_1xxxxxxx` (depending on KASLR randomization)

**Section:** `.text` — NOT `.idmap.text`

**Proof:** The `br x8` in `__primary_switch` loaded x8 with the absolute virtual
address of `__primary_switched` from the literal pool. The literal pool entry was
placed by the linker (at link time) using the kernel virtual address assigned to
`__primary_switched`. KASLR patched this at runtime via `__pi_early_map_kernel`.

---

## Precondition 3 — x0 = `__pa(KERNEL_START)`

**Value:** Physical address of the kernel image start. Example: `0x40080000`

**Proof:**
```asm
// In __primary_switch, AFTER __pi_early_map_kernel, BEFORE br x8:
adrp  x0, KERNEL_START   // At this point, PC is in identity-mapped space
                          // so adrp gives PA (PC ≈ VA in identity map)
```
`KERNEL_START` is the linker-assigned start of the kernel image. `adrp` from within
identity-mapped space gives the physical address because PA ≈ VA in that mapping.

**KASLR impact:** If KASLR moved the kernel to a different physical base, `adrp` reflects
the ACTUAL physical load address (because the identity map maps the actual physical RAM
where the kernel was relocated to).

---

## Precondition 4 — x20 = `cpu_boot_mode`

**Value:** Either `BOOT_CPU_MODE_EL1 (0x1)` or `BOOT_CPU_MODE_EL2 (0x2)`

**Proof:**
```asm
// In primary_entry:
bl   init_kernel_el    // returns BOOT_CPU_MODE_EL1 or EL2 in w0
mov  x20, x0           // save as callee-saved register
```
`init_kernel_el` reads `CurrentEL`, determines EL1 or EL2, and returns the constant.
`x20` is callee-saved — preserved through `__cpu_setup`, `__primary_switch`, `__enable_mmu`,
`__pi_early_map_kernel`, and the `br x8` jump.

**Top 32 bits:** May contain `BOOT_CPU_FLAG_E2H` flag if VHE is active. This flag is NOT
stored in `__boot_cpu_mode` (which is why `set_cpu_boot_mode_flag` masks it with `w0`
using 32-bit `str w0` — the top 32 bits of x20 are dropped).

---

## Precondition 5 — x21 = FDT Physical Address

**Value:** Physical address of the DTB blob in RAM. Example: `0x48000000`

**Proof:**
```asm
// In preserve_boot_args (called from primary_entry):
mov  x21, x0     // bootloader put FDT PA in x0 per Linux boot protocol
```
x21 is callee-saved — carried intact across the entire boot chain from here to
`__primary_switched`. This is the physical address the bootloader placed in `x0` at
kernel entry, per the ARM64 Linux boot protocol (Documentation/arm64/booting.rst).

---

## Precondition 6 — SP = `early_init_stack` (Temporary Stack)

**Value:** Virtual address of the top of `early_init_stack`

**Proof:**
```asm
// In __primary_switch, AFTER __enable_mmu:
adrp  x1, early_init_stack
mov   sp, x1
```
`early_init_stack` is defined in `arch/arm64/kernel/vmlinux.lds.S` as a 4-page
(16KB) per-CPU stack in `.bss`. It is used ONLY until `init_cpu_task` switches to
`init_stack`. No C ABI frame has been pushed on it yet at entry.

---

## Precondition 7 — Caches Are ON

**State:** `SCTLR_EL1.C = 1` (D-cache), `SCTLR_EL1.I = 1` (I-cache)

**Proof:** Set by `set_sctlr_el1 x0` in `__enable_mmu`. D-cache enabled along with MMU.

**Why it matters:** `str_l` macros use cache-coherent stores. `stp`/`ldp` on the stack
hit D-cache. Without D-cache, stack operations would be uncached and potentially
incoherent with speculative prefetches.

---

## Precondition 8 — x29 = 0 (Null Frame Pointer)

**Value:** `xzr` (all zeros)

**Proof:**
```asm
// In __primary_switch, after resetting SP:
mov  x29, xzr
```
This null frame pointer becomes the "saved x29" when `init_cpu_task` stamps the
final unwind frame. The unwinder sees `fp=0` and stops — preventing it from reading
garbage memory when unwinding the boot CPU's stack.

---

## Precondition 9 — VBAR_EL1 = 0 (GARBAGE — Danger!)

**State:** Unknown / zero / residual from firmware

**This is NOT a guarantee of correctness — it is a known UNSAFE condition.**

No exception can be safely handled between the `br x8` jump and the
`msr vbar_el1, x8` instruction in `__primary_switched`.

If any exception fires in this window:
```
CPU fetches handler: VBAR_EL1 + offset = 0x0 + 0x200 = 0x200
Virtual address 0x200 → not mapped by TTBR0 (identity map covers low PA, but VA 0x200
in TTBR1 range? No — TTBR1 handles 0xFFFFFF80_00000000+)
Result: Translation fault → CPU tries to dispatch THAT fault → same broken VBAR → hang
```

This is the primary reason `init_cpu_task` (which establishes SP) must complete
immediately, and `msr vbar_el1` must follow immediately after.

---

## Entry State Summary Table

| Item | Value | Set By | Safe? |
|---|---|---|---|
| MMU | ON | `__enable_mmu` | YES |
| TTBR0 | identity map | `__enable_mmu` | YES |
| TTBR1 | kernel page tables | `__enable_mmu` | YES |
| PC | kernel virtual space | `br x8` | YES |
| x0 | `__pa(KERNEL_START)` | `adrp` in `__primary_switch` | YES |
| x20 | `cpu_boot_mode` | `init_kernel_el` | YES |
| x21 | FDT physical address | `preserve_boot_args` | YES |
| x29 | 0 | `mov x29, xzr` in `__primary_switch` | YES |
| SP | `early_init_stack` | `__primary_switch` | TEMP |
| VBAR_EL1 | 0 (garbage) | NOT SET YET | **DANGER** |
| D-cache | ON | `__enable_mmu` | YES |
| I-cache | ON | `__enable_mmu` | YES |

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