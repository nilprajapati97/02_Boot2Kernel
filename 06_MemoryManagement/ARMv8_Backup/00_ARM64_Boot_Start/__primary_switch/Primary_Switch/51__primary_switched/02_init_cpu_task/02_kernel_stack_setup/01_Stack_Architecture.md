# Kernel Stack Architecture — The 16KB Boot Stack

## Why Every Task Has Its Own Stack

The Linux kernel uses a **per-task kernel stack** model:
- Each task (`struct task_struct`) has a dedicated kernel stack
- The kernel stack is used ONLY during kernel execution for that task
- User-space execution uses the user-space stack (different address space)
- Interrupt handling uses a SEPARATE IRQ stack (configured by `cpu_init`)

---

## `init_stack` — The Boot CPU's Permanent Stack

```c
// arch/arm64/kernel/init_task.c (or vmlinux.lds.S):
__attribute__((section(".data..init_stack")))
__attribute__((aligned(THREAD_SIZE)))
unsigned long init_stack[THREAD_SIZE / sizeof(unsigned long)];
```

Properties:
- Size: `THREAD_SIZE` = `1 << THREAD_SHIFT` = `1 << 14` = **16,384 bytes** (16KB)
- Alignment: `THREAD_SIZE` = 16KB (address is multiple of 16384)
- Section: `.data..init_stack` — not freed after init
- Initial content: zero (BSS-like initialization)

---

## Why 16KB?

ARM64 uses 16KB stacks (vs. x86's 8KB). The reasons:

1. **AARCH64 calling convention (AAPCS64) is 64-bit**: Each saved register is 8 bytes.
   A typical interrupt stack frame saves 20-30 registers = 160-240 bytes per frame.
   Deeply nested kernel calls + interrupt stack can easily exceed 8KB.

2. **ARM64 has more kernel features that use stack**: SCS bookkeeping, KASAN checks,
   LTO inlining — all use stack space.

3. **Guard pages**: With 16KB stacks, the probability of stack overflow without
   detection is lower. VMAP_STACK (CONFIG_VMAP_STACK) places a guard page immediately
   below the stack — any overflow hits the guard page → fault detected.

---

## Stack Layout After `init_cpu_task`

```
VA (high addresses)
┌────────────────────────────────┐  ← init_stack base + THREAD_SIZE = STACK TOP
│  [SP reserved for pt_regs]     │  ← THREAD_SIZE - PT_REGS_SIZE = 16384 - 336
│                                │
│  [pt_regs.regs[0..30]]         │  (x0..x30 save area)
│  [pt_regs.sp]                  │
│  [pt_regs.pc]                  │
│  [pt_regs.pstate]              │
│  [pt_regs.orig_x0]             │
│  [pt_regs.sdei_ttbr1]          │
│  [pt_regs.pmr_save]            │
│  [pt_regs.stackframe.fp = 0]   │  ← S_STACKFRAME offset; fp=0 = unwind sentinel
│  [pt_regs.stackframe.pc = 0]   │
│  [pt_regs.stackframe_type = 1] │  ← FRAME_META_TYPE_FINAL
│                                │
│  ← SP = init_stack + THREAD_SIZE - PT_REGS_SIZE
│  ← x29 = SP + S_STACKFRAME (points into pt_regs.stackframe)
│                                │
│  [available stack space]       │  ← grows DOWN from here
│  [start_kernel() frame]        │  ← first frame pushed by bl start_kernel
│  [setup_arch() frame]          │
│  [...]                         │
│                                │
│  [... 15KB of stack space ...]  │
│                                │
└────────────────────────────────┘  ← init_stack base (thread_info NOT here on ARM64)
VA (low addresses)
```

---

## The Stack Switch Instructions — Detailed Analysis

```asm
// OP 2a: Load init_task.stack field
ldr     x5, [x4, #TSK_STACK]
// x5 = init_task.stack = &init_stack[0] (bottom of stack buffer)

// OP 2b: Compute top of stack
add     sp, x5, #THREAD_SIZE
// sp = &init_stack[0] + 16384
// sp now points to ONE BYTE PAST the end of init_stack
// (This is valid — ARM64 allows sp to point one past the allocation)

// OP 2c: Reserve pt_regs at the top
sub     sp, sp, #PT_REGS_SIZE
// sp = &init_stack[0] + 16384 - 336
// sp now points to the start of the synthetic pt_regs at the stack top
```

After this, `sp` is the new stack pointer. When `start_kernel` is called:
```asm
bl     start_kernel   // pushes x29+x30 below sp, decrements sp by 16
```
The first real stack frame is placed BELOW the pt_regs reservation.

---

## `THREAD_SHIFT` — The Constant That Determines Everything

```c
// arch/arm64/include/asm/thread_info.h
#ifdef CONFIG_KASAN
#define KASAN_SHADOW_SCALE_SHIFT    3
// KASAN instrumentation adds stack space
#define MIN_THREAD_SHIFT    (14 + KASAN_SHADOW_SCALE_SHIFT)
#else
#define MIN_THREAD_SHIFT    14
#endif

#define THREAD_SHIFT        max(MIN_THREAD_SHIFT, (int)PAGE_SHIFT)
#define THREAD_SIZE         (1UL << THREAD_SHIFT)
```

With `PAGE_SHIFT=12` (4KB pages) and no KASAN: `THREAD_SHIFT=14`, `THREAD_SIZE=16KB`
With KASAN enabled: `THREAD_SHIFT=17`, `THREAD_SIZE=128KB` — KASAN needs more stack!

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