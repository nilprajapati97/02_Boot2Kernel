# `init_stack` Memory Layout — Detailed Breakdown

## `init_stack` in the Kernel Image

```
Kernel Image VA Layout (simplified):
┌────────────────────────────────────────────────┐
│ .text    (kernel code)                         │
│ .rodata  (read-only data)                      │
│ .data    (initialized writable data)           │
│   init_task  (struct task_struct)              │  ← &init_task
│   __per_cpu_offset[NR_CPUS]                    │
│   ...                                          │
│ .data..init_stack                              │
│   init_stack[16384/8]    (16KB, 16KB-aligned)  │  ← &init_stack
│ .bss     (zero-initialized data)               │
│ .init.data  (freed after boot)                 │
│ .init.text  (freed after boot)                 │
└────────────────────────────────────────────────┘
```

`init_stack` is in `.data` (specifically `.data..init_stack`) and is NEVER freed.
It becomes the CPU0 idle task stack for the entire kernel lifetime.

---

## Physical Memory Addresses

Typical values on a device with 4GB RAM:

```
Physical address of kernel image load: 0x40_2000_0000 (typical for ARM64 SoC)
KASLR offset (example):               0x00_0080_0000
Physical load PA:                     0x40_2080_0000

Kernel linked VA:                     0xFFFF_8000_1000_0000
Actual VA with KASLR:                 0xFFFF_8000_1080_0000

kimage_voffset = VA - PA =            0xFFFF_0000_0F80_0000 (example)

init_stack PA:  0x40_2080_0000 + (offset of init_stack in image)
init_stack VA:  0xFFFF_8000_1080_0000 + (offset of init_stack in image)
```

After `init_cpu_task`:
- `sp = init_stack_VA + 16384 - 336`
- All stack frames go to virtual addresses in the `0xFFFF_8000_xxxx_xxxx` range

---

## VMAP_STACK — Virtual Guard Pages (Production Config)

On a production kernel (`CONFIG_VMAP_STACK=y`):

```
VA layout per stack:
┌────────────────────────────────────────┐
│  [guard page - 4KB, unmapped]          │  ← any access → fault!
├────────────────────────────────────────┤  ← stack bottom
│  [15KB of stack space]                 │
│  [pt_regs reservation - 336 bytes]    │
├────────────────────────────────────────┤  ← stack top (sp starts here - 336)
│  [guard page - 4KB, unmapped]          │  ← (optional, at top)
└────────────────────────────────────────┘
```

`init_stack` does NOT use VMAP_STACK (it's statically allocated, not vmalloc'd).
But all OTHER task stacks allocated by `copy_process` → `dup_task_struct` USE
`alloc_thread_stack_node` which with `CONFIG_VMAP_STACK=y` uses `vmalloc` with
guard pages.

This means `init_stack` has NO guard page protection. If the boot CPU overflows
`init_stack` during early init, it silently corrupts adjacent kernel data. This is
an acceptable risk — early boot code is carefully written to not recurse deeply.

---

## Stack Pointer Register (`sp`) on ARM64

ARM64 uses a **banked** SP model:

```
When at EL1 (kernel mode):
    sp  → SP_EL1 (16-byte aligned, kernel stack)

When at EL0 (user mode):
    sp  → SP_EL0 (user stack pointer)
```

Note: `PSTATE.M[4]` (Stack Selector) bit = 0 means "use SPELx for current EL"
(`SPSel=0` would use SP_EL0 at all levels, which Linux doesn't use).

After `init_cpu_task` sets the stack pointer, `sp` always refers to `SP_EL1`
(the kernel stack) when running at EL1.

---

## `PT_REGS_SIZE` Reservation — The "Synthetic" `pt_regs`

At the TOP of every kernel stack is a `struct pt_regs` allocation. For `init_stack`,
this is the "synthetic" pt_regs that init_cpu_task writes the frame sentinel into.

For all OTHER kernel stacks (created by `dup_task_struct`), this top-of-stack
`pt_regs` is where the REAL register context is saved on every kernel entry
(syscall, IRQ, exception).

```c
// arch/arm64/include/asm/processor.h
#define task_pt_regs(p) \
    ((struct pt_regs *)(THREAD_SIZE + task_stack_page(p)) - 1)

// This computes: stack_base + THREAD_SIZE - sizeof(pt_regs)
// = the ADDRESS of pt_regs at the top of the stack
```

`init_cpu_task`'s `sub sp, sp, #PT_REGS_SIZE` computes the SAME address manually
in assembly.

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