# `early_init_stack` — Definition and Memory Layout

**Source:** `arch/arm64/kernel/vmlinux.lds.S` — `early_init_stack = .;` (after BSS)  
**Used by:** `primary_entry` — `adr_l x27, early_init_stack; mov sp, x27`

---

## 0. Why the Linker Script Defines the Stack

ARM64 does not have a fixed stack pointer at reset. After reset, `sp` (SP_EL1)
is UNKNOWN. The kernel cannot call any function (which requires a valid stack)
until `sp` is initialized.

`early_init_stack` is the kernel's answer: a statically-allocated 4KB stack
placed at the end of the BSS section, whose address is embedded in the kernel
binary as a linker symbol.

---

## 1. The Linker Script Fragment

```ld
// arch/arm64/kernel/vmlinux.lds.S (simplified, ~line 350-370)

.bss : {
    __bss_start = .;
    
    *(.bss..page_aligned)           // page-aligned BSS data (init_thread_union etc.)
    . = ALIGN(PAGE_SIZE);
    
    *(.bss)                         // normal BSS data
    *(.bss.*)
    *(.dynbss)
    *(.gnu.linkonce.b.*)
    . = ALIGN(PAGE_SIZE);           // align to page boundary before stack
    
    . += SZ_4K;                     // Reserve 4096 bytes for boot stack
    
    early_init_stack = .;           // Symbol marks TOP of reserved area
    
    __bss_stop = .;
}
```

**Key observations:**

1. The symbol `early_init_stack` is placed **after** the reserved bytes
   (`.` += SZ_4K), meaning it points to the **high end** of the stack area.
2. Since ARM64 stack grows downward, `sp = early_init_stack` means the stack
   starts at the top and expands toward lower addresses.
3. The `ALIGN(PAGE_SIZE)` before the reservation ensures the stack area starts
   on a page boundary (useful if the kernel ever wants to add a guard page).

---

## 2. Memory Layout Diagram

```
Kernel BSS section memory layout:

High address
    ┌──────────────────────────────────────────────┐
    │  early_init_stack = .  (symbol here)         │  ← SP initialized to this
    ├──────────────────────────────────────────────┤
    │  4096 bytes (SZ_4K) reserved for boot stack  │  ← Stack grows downward
    ├──────────────────────────────────────────────┤  PAGE boundary (ALIGN)
    │  Normal .bss variables                       │
    │  (zero-initialized by __cpu_setup or head.S) │
    ├──────────────────────────────────────────────┤
    │  .bss..page_aligned section                  │
    │  (init_thread_union is here at 16KB aligned) │
    ├──────────────────────────────────────────────┤  PAGE boundary
    │  ...other data sections...                   │
Low address
```

---

## 3. How `primary_entry` Sets Up SP

```asm
// arch/arm64/kernel/head.S — primary_entry:
SYM_CODE_START(primary_entry)
    ...
    adr_l   x27, early_init_stack   // x27 = physical address of symbol
    mov     sp, x27                  // Set SP_EL1 = early_init_stack PA
    ...
    bl      __primary_switch         // Can now call functions
SYM_CODE_END(primary_entry)
```

`adr_l` is a PC-relative macro that works without MMU:

```asm
.macro adr_l, dst, sym
    adrp    \dst, \sym              // Load page-aligned PC-relative address
    add     \dst, \dst, :lo12:\sym  // Add the lower 12 bits
.endm
```

Before the MMU is enabled, the PC contains the **physical address** of the
currently executing instruction. `adrp` computes:

```
x27 = (PC & ~0xFFF) + PAGE_OFFSET_of_early_init_stack_from_PC
```

This is the **physical address** of `early_init_stack`. The resulting `x27`
value is something like `0x4000_8000` (PA).

---

## 4. Stack Usage Before MMU Enable

Functions called with this stack:
1. `__cpu_setup` — sets up CPU registers (no heavy stack usage)
2. `__pi_early_map_kernel` — this is a C function and uses the stack significantly:
   - Local variables (loop counters, PTE values, temporary pointers)
   - Saved registers (callee-saved registers that C compiler saves in prologue)
   - Nested calls (`__pi_create_init_idmap`, `__pi_create_pgd_next_table`, etc.)

**Stack depth estimation:**

```
__pi_early_map_kernel frame: ~100 bytes
    __pi_create_init_idmap frame: ~80 bytes
        __pi_alloc_table frame: ~32 bytes
            max nesting: ~3 levels

Total: ~300-400 bytes of stack usage
Available: 4096 bytes → safe margin
```

The 4KB is more than sufficient for the simple page table building operations.

---

## 5. Physical vs. Virtual Address Identity

Before the MMU is enabled:

```
early_init_stack linker symbol VA:  0xFFFF_8000_1234_0000  (example kernel VA)
early_init_stack physical address:  0x4000_8000_0000_0000  (KASLR randomized PA)
```

The `adr_l` instruction computes the **physical address** because the PC holds
a PA value. So `x27 = 0x4000_8000_0000_0000` (the PA).

After the MMU is enabled and `__primary_switched` runs, the kernel switches to
the `init_thread_union` stack (a VA). The `early_init_stack` PA value in the
old `sp` is simply abandoned.

---

## 6. BSS Zeroing and Stack Initialization

The `.bss` section (including `early_init_stack`'s backing memory) is zeroed by:

```asm
// arch/arm64/kernel/head.S — __primary_switched:
adrp    x4, __bss_start             // BSS start PA
adr_l   x5, __bss_stop              // BSS end PA
bl      __pi_memset_aligned          // Zero the range
```

Wait — this happens **after** the MMU is enabled, in `__primary_switched`. But
the boot stack uses BSS memory **before** this zeroing.

**Is the stack corrupted?**

No. The stack is used for TEMPORARY storage (function frames). The data written
to the stack during pre-MMU execution is never read again after `__primary_switched`
switches to the `init_thread_union` stack. BSS zeroing after MMU enable is safe
because the old stack data is simply overwritten with zeros — it doesn't matter.

The BSS zero is important for global variables (`init_task` fields, for example),
not for the transient stack data.

---

## 7. No Stack Guard / Protection

The `early_init_stack` has no stack overflow protection:
- No guard page (the page before it is regular BSS data — writable)
- No stack canary (GCC `-fstack-protector` is not in effect for early boot code)
- No hardware MPU protection (no EL1 memory permission settings during early boot)

A stack overflow during pre-MMU boot would silently corrupt BSS data below the
stack. This would cause mysterious failures in `__primary_switched` when BSS
is initialized and the corrupted values are used.

In practice, this is not a problem because:
1. The call depth is shallow (2-3 levels)
2. Local variable usage is minimal
3. The 4KB stack is generously sized for these operations

---

## 8. Relationship to `init_thread_union`

The `init_thread_union` (post-MMU stack) is also in BSS:

```c
// init/init_task.c
union thread_union init_thread_union __init_task_data
    __aligned(THREAD_ALIGN)
= { INIT_THREAD_INFO(init_task) };
```

It is in the `.data..init_task` or `.bss..page_aligned` subsection, placed
by:

```ld
*(.bss..page_aligned)
```

Both stacks are in BSS, but they serve different epochs:

| Stack | VA Range | When Used | How Reached |
|---|---|---|---|
| `early_init_stack` | VA of BSS symbol | Pre-MMU boot | `adr_l` + `mov sp` in `primary_entry` |
| `init_thread_union` | VA of BSS symbol | Post-MMU boot | `adrp` + `add sp` in `__primary_switched` |

The transition happens in `__primary_switched` and is irreversible (the old SP
value is discarded).

---

## ARMv8 CPU / Kernel / Memory Context

### CPU Perspective (ARMv8-A)
In ARMv8-A, the stack pointer is a dedicated register (SP_EL1 at EL1, SP_EL0 at EL0). SP_EL1 is the stack pointer used by the kernel during normal execution. The AAPCS64 ABI requires the stack to be 16-byte aligned at any instruction that may cause an exception. SCTLR_EL1.SA (bit 3) enables hardware enforcement of this alignment: if SP_EL1 is not 16-byte aligned when a load/store using SP is executed, an SP alignment fault is raised. The frame pointer (x29) is a general-purpose register used by convention to hold the base of the current stack frame. Writing x29 is the first act of any C function that wishes to be unwound.

### Kernel Perspective (Linux ARM64)
After the MMU is enabled, __primary_switch reinitializes the stack pointer to a virtual address. The early boot stack is defined as:
  __INIT_DATA: init_thread_union (size THREAD_SIZE, typically 16 KB)
The LDR instruction loads the VA of init_thread_union + THREAD_SIZE into x0, then MOV sp, x0 sets SP_EL1. This is necessary because the old stack pointer was set to a physical address (before the MMU) and that PA is no longer the correct address for the kernel VA layout. x29 is set to zero (zero frame pointer) to terminate the unwind chain at the first kernel stack frame.

### Memory Perspective (ARMv8 Memory Model)
The kernel stack resides in Normal Inner-Shareable Write-Back Cacheable memory (MT_NORMAL). Once the MMU and D-cache are enabled, all stack accesses (PUSH/POP equivalents: STP/LDP) go through the L1 D-cache. The L1 D-cache write-back policy means that the stack contents are not immediately visible to physical memory until a cache clean or eviction. This is safe for the stack because the kernel does not use DMA to read stack memory. The stack pointer reinitalization at VA is a hard cut: all future kernel stack frames exist in the high VA kernel mapping.