# `THREAD_SIZE` and Stack Constants — Source and Meaning

## Constant Definitions

```c
// arch/arm64/include/asm/thread_info.h

// Page shift: 12 for 4KB pages, 14 for 16KB pages (64KB = 16)
// ARM64 Linux typically uses 4KB pages → PAGE_SHIFT = 12

#ifdef CONFIG_KASAN
  #define KASAN_THREAD_SHIFT  3      // KASAN needs 8x more stack
  #define MIN_THREAD_SHIFT    (14 + KASAN_THREAD_SHIFT)   // = 17
#else
  #define MIN_THREAD_SHIFT    14
#endif

// THREAD_SHIFT is at least PAGE_SHIFT (must be at least one page)
#define THREAD_SHIFT        max(MIN_THREAD_SHIFT, PAGE_SHIFT)
                            // = max(14, 12) = 14 (no KASAN)
                            // = max(17, 12) = 17 (with KASAN)

#define THREAD_SIZE         (UL(1) << THREAD_SHIFT)
                            // = 1 << 14 = 16384 (16KB, no KASAN)
                            // = 1 << 17 = 131072 (128KB, with KASAN)

#define THREAD_MASK         (~(THREAD_SIZE - 1))
                            // = ~0x3FFF = 0xFFFF_FFFF_FFFF_C000
                            // Used to find thread base: sp & THREAD_MASK
```

---

## `PT_REGS_SIZE` — The Other Key Constant

```c
// arch/arm64/include/asm/ptrace.h
struct pt_regs {
    union {
        struct user_pt_regs user_regs;
        struct {
            u64 regs[31];  // x0-x30 (31 registers × 8 bytes = 248 bytes)
            u64 sp;        // 8 bytes
            u64 pc;        // 8 bytes
            u64 pstate;    // 8 bytes
            // subtotal: 272 bytes
        };
    };
    u64 orig_x0;            // 8 bytes (for syscall restart)
    s32 syscallno;          // 4 bytes
    u32 unused2;            // 4 bytes padding
    // subtotal: 288 bytes
    u64 sdei_ttbr1;         // 8 bytes (SDEI emergency handler)
    u64 pmr_save;           // 8 bytes (priority mask register)
    // subtotal: 304 bytes
    u64 stackframe[2];      // 16 bytes (fp + pc for stack unwinder) ← S_STACKFRAME
    // subtotal: 320 bytes
    u64 lockdep_hardirqs;   // 8 bytes (for lockdep IRQ tracking)
    u64 exit_rcu;           // 8 bytes (for RCU exit tracking)
    // TOTAL: 336 bytes = PT_REGS_SIZE
};
```

`PT_REGS_SIZE = sizeof(struct pt_regs) = 336 bytes`

This is generated at build time:
```c
// arch/arm64/kernel/asm-offsets.c:
DEFINE(PT_REGS_SIZE, sizeof(struct pt_regs));
// → PT_REGS_SIZE = 336
```

---

## `S_STACKFRAME` Offset

```c
// asm-offsets.c:
DEFINE(S_STACKFRAME,
       offsetof(struct pt_regs, stackframe));
```

```
pt_regs layout with offsets:
 [  0] regs[0]         (x0)
 [  8] regs[1]         (x1)
 ...
 [240] regs[30]        (x30)
 [248] sp
 [256] pc
 [264] pstate
 [272] orig_x0
 [280] syscallno + unused2
 [288] sdei_ttbr1
 [296] pmr_save
 [304] stackframe[0]   (fp)  ← S_STACKFRAME = 304
 [312] stackframe[1]   (pc)
 [320] lockdep_hardirqs
 [328] exit_rcu
 [336] END             ← PT_REGS_SIZE = 336
```

So `S_STACKFRAME = 304` (the actual value may vary with kernel version).

---

## `FRAME_META_TYPE_FINAL` — The Termination Sentinel

```c
// arch/arm64/include/asm/stacktrace/frame.h (or similar)

enum {
    FRAME_META_TYPE_UNKNOWN = 0,
    FRAME_META_TYPE_FINAL   = 1,   // ← this is what init_cpu_task sets
    FRAME_META_TYPE_PT_REGS = 2,   // a saved pt_regs context (e.g., exception entry)
};
```

The unwinder in `arch/arm64/kernel/stacktrace.c` checks the type:
```c
static int notrace unwind_next(struct unwind_state *state)
{
    // ... read fp from x29 ...
    struct stackframe_record *record = (void *)fp;
    if (record->type == FRAME_META_TYPE_FINAL)
        return -ENOENT;  // stop unwinding
    // ... continue unwind ...
}
```

`init_cpu_task` sets this type field to `FRAME_META_TYPE_FINAL (=1)` for the
boot frame, guaranteeing the unwinder stops cleanly when it reaches the bottom
of the boot stack.

---

## Summary — All Constants at a Glance

| Constant | Value | Purpose |
|---|---|---|
| `THREAD_SIZE` | 16384 | Total kernel stack size |
| `THREAD_SHIFT` | 14 | log2(THREAD_SIZE) |
| `PT_REGS_SIZE` | 336 | Bytes reserved at stack top for pt_regs |
| `S_STACKFRAME` | 304 | Offset of stackframe within pt_regs |
| `S_STACKFRAME_TYPE` | ~320 | Offset of type field in stackframe |
| `FRAME_META_TYPE_FINAL` | 1 | Unwind terminator value |
| `TSK_STACK` | ~24 | Offset of stack field in task_struct |
| `TSK_TI_CPU` | ~24 | Offset of thread_info.cpu in task_struct |

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