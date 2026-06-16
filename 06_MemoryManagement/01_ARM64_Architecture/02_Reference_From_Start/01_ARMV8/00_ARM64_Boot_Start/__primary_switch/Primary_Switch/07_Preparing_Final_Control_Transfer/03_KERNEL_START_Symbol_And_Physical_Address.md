# `KERNEL_START`, `_text`, and the Physical Address of `__primary_switched`

**Context:** Understanding what `ldr x8, =__primary_switched` loads  
**Source:** `arch/arm64/include/asm/memory.h`, `arch/arm64/kernel/vmlinux.lds.S`

---

## 0. What Is `KERNEL_START`?

From `arch/arm64/include/asm/memory.h`:

```c
// arch/arm64/include/asm/memory.h (line ~68)
#define KERNEL_START    _text
```

`_text` is a linker-script symbol defined in `arch/arm64/kernel/vmlinux.lds.S`:

```ld
/* vmlinux.lds.S */
. = KIMAGE_VADDR;      /* Set the current address to kernel VA base */
                        /* KIMAGE_VADDR is typically CONFIG_PAGE_OFFSET - SZ_2G */
                        /* or similar compile-time constant */

.head.text : {
    _text = .;          /* _text = VA of first byte of the kernel image */
    HEAD_TEXT           /* arch/arm64/kernel/head.S output sections */
}
```

So:
- `_text` = the **virtual address** of the very first instruction of the kernel
- `KERNEL_START = _text` = the same — the VA starting point of the kernel image
- `__primary_switched` is a function in `.head.text` or early `.text`, so its
  VA is very close to `_text`

---

## 1. `KIMAGE_VADDR` — Where the Kernel Is Linked

The kernel is **linked** at a specific VA called `KIMAGE_VADDR`:

```c
// arch/arm64/include/asm/memory.h
// KIMAGE_VADDR is the VA at which the kernel image is linked:
#define KIMAGE_VADDR    (-(UL(1) << VA_BITS) / 2 + UL(SZ_2G))
// For VA_BITS=48:
//   -(2^48)/2 + 2^31
//   = -(2^47) + 2^31
//   = 0xFFFF_8000_0000_0000 + 0x0000_0000_8000_0000
//   = 0xFFFF_8000_8000_0000
```

So the compile-time (pre-KASLR) base VA of the kernel is approximately
`0xFFFF_8000_8000_0000`. The exact value depends on `VA_BITS` configuration.

---

## 2. The VA of `__primary_switched` at Compile Time

`__primary_switched` is defined in `arch/arm64/kernel/head.S`, in the
`.head.text` section. It follows `__primary_switch` in the binary:

```
_text                     = 0xFFFF_8000_8000_0000  (KIMAGE_VADDR, pre-KASLR)
__primary_switch offset   ~ +0x500 (few hundred bytes from _text)
__primary_switched offset ~ +0x600 (a bit after __primary_switch)
```

Compile-time VA example:
```
VA(__primary_switched) = 0xFFFF_8000_8000_0600
```

This is the value placed in the literal pool by the linker.

---

## 3. After KASLR — What VA Is Loaded into x8

KASLR randomizes the **load address** of the kernel. Two components:
1. **Physical address randomization:** The kernel is loaded at a random PA instead of the default PA
2. **VA offset:** `kimage_voffset = actual_text_VA - _text_link_VA`

After relocation (`relocate_kernel`), the literal pool entry for
`__primary_switched` is patched to:

```
patched_VA = VA(__primary_switched) + KASLR_offset
           = 0xFFFF_8000_8000_0600 + 0x0000_0000_1234_0000
           = 0xFFFF_8000_9234_0600   (example with KASLR offset)
```

When `ldr x8, =__primary_switched` executes, `x8` gets this patched VA.
`br x8` jumps to `0xFFFF_8000_9234_0600`.

---

## 4. Physical Address of `__primary_switched`

The physical address of `__primary_switched` is:
```
PA = VA - kimage_voffset
   = 0xFFFF_8000_9234_0600 - 0xFFFF_8000_8000_0000 + default_load_PA
```

Or more precisely:
```
PA = runtime_PA_of_text + (VA(__primary_switched) - VA(_text))
```

This PA is what TTBR1 (`swapper_pg_dir`) maps when the CPU does the PTW at
`br x8`.

---

## 5. The Linker Symbol `__primary_switched`

The symbol `__primary_switched` is defined as a `SYM_FUNC_START` in `head.S`:

```asm
// arch/arm64/kernel/head.S
SYM_FUNC_START_LOCAL(__primary_switched)
    // ... function body ...
SYM_FUNC_END(__primary_switched)
```

The macro `SYM_FUNC_START_LOCAL` expands to (approximately):
```asm
.type __primary_switched, %function
.local __primary_switched
__primary_switched:
```

The linker assigns `__primary_switched` the VA of the instruction immediately
following the label. This VA is what goes into the literal pool.

---

## 6. Verifying in `/proc/kallsyms`

After boot, the runtime VA of `__primary_switched` can be read from:

```bash
$ grep __primary_switched /proc/kallsyms
ffff8000804010a0 t __primary_switched
```

The `t` means local text (`.text` section). This VA is the KASLR-adjusted VA
that was loaded into `x8` and jumped to. It is the address that `br x8` used.

---

## 7. `__primary_switched` and TTBR1 Mapping

When the CPU executes `br x8` where `x8 = 0xffff8000804010a0`:

```
VA = 0xffff_8000_804010a0

VA[63:48] = 0xFFFF → upper half → TTBR1_EL1
TTBR1_EL1 = swapper_pg_dir (physical address, set by __enable_mmu)

Translation:
  PGD index = VA[47:39] = bits 47:39 of 0xffff_8000_804010a0
  PUD index = VA[38:30]
  PMD index = VA[29:21] → points to a 2MB block entry
  offset    = VA[20:0]  → byte within the 2MB block
```

`swapper_pg_dir` was built by `__pi_early_map_kernel` to include exactly this
2MB block. The PTW finds the block entry → retrieves PA of the 2MB block →
adds offset → CPU fetches `__primary_switched` instruction from PA.

First instruction in the kernel virtual address space executes.

---

## 8. What `KERNEL_START` Is Used For in Code

`KERNEL_START` appears in several kernel contexts:

```c
// arch/arm64/include/asm/memory.h
#define KERNEL_START    _text
#define KERNEL_END      _end

// Used in:
// __pi_early_map_kernel: maps [KERNEL_START, KERNEL_END) into swapper_pg_dir
// memblock_reserve(pa(_text), _end - _text): reserves kernel image in memblock
// is_kernel_text(addr): addr in [KERNEL_START, KERNEL_END)? (for stack unwinder)
```

The kernel image reservation ensures the bootloader-provided physical memory
doesn't get re-allocated over the running kernel code.

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