# Day 04 — Exception Vectors (`VBAR_EL1`)

> **Goal**: Install the AArch64 exception vector table, implement save/restore macros, and dispatch synchronous exceptions / IRQs to C handlers. Deliberately trigger an UNDEF to prove the path.
>
> **Why today**: Every later subsystem (timer IRQ Day 5, page fault Day 11, syscall Day 17) enters the kernel via this table. Building it now means every bug becomes a readable trace instead of a reset.

---

## 1. Background

### 1.1 AArch64 vector table layout (`VBAR_EL1`)
16 entries × 128 bytes each = 2 KiB, **must be 2 KiB aligned**.

| Offset | Source state | Type |
|---|---|---|
| 0x000 | Current EL with SP_EL0 | Sync |
| 0x080 | "  | IRQ |
| 0x100 | "  | FIQ |
| 0x180 | "  | SError |
| 0x200 | Current EL with SP_ELx | Sync |
| 0x280 | "  | IRQ |
| 0x300 | "  | FIQ |
| 0x380 | "  | SError |
| 0x400 | Lower EL using AArch64 | Sync (← syscalls land here Day 17) |
| 0x480 | "  | IRQ |
| 0x500 | "  | FIQ |
| 0x580 | "  | SError |
| 0x600 | Lower EL using AArch32 | Sync (we won't use AArch32) |
| 0x680 | "  | IRQ |
| 0x700 | "  | FIQ |
| 0x780 | "  | SError |

### 1.2 `ESR_EL1` Exception Class (EC) values
| EC | Meaning |
|---|---|
| 0x00 | Unknown |
| 0x0E | Illegal execution state |
| 0x15 | SVC from AArch64 (syscall) |
| 0x20 | Instruction abort from lower EL |
| 0x21 | Instruction abort from same EL |
| 0x24 | Data abort from lower EL |
| 0x25 | Data abort from same EL |
| 0x2C | FP/SIMD trap |
| 0x3C | BRK instruction |

`FAR_EL1` holds faulting VA on aborts; `ELR_EL1` = return address; `SPSR_EL1` = saved PSTATE.

### 1.3 `pt_regs` structure
What we save on every exception (used by syscall, ptrace later):
```c
struct pt_regs {
    u64 regs[31];     // x0..x30
    u64 sp;           // SP_EL0 at time of exception (or kernel SP if from EL1)
    u64 pc;           // ELR_EL1
    u64 pstate;       // SPSR_EL1
};
```
Size = 34 × 8 = 272 bytes (round to 16-byte alignment).

---

## 2. Design

### 2.1 New files
```
arch/arm64/kernel/entry.S        (vector table + save/restore macros)
arch/arm64/kernel/exceptions.c   (do_sync, do_irq, do_fiq, do_serror)
include/asm-arm64/pt_regs.h
include/kernel/exceptions.h
```

### 2.2 Dispatch flow
```
exception ──► VBAR_EL1+offset ──► kernel_entry (save regs) ──► C handler ──► kernel_exit (restore + eret)
```

---

## 3. Implementation

### 3.1 `arch/arm64/kernel/entry.S`
```asm
#include "asm-offsets.h"   // generated; for now use literal 272

#define S_FRAME_SIZE     272

    .macro kernel_entry, el
    sub     sp, sp, #S_FRAME_SIZE
    stp     x0,  x1,  [sp, #16 * 0]
    stp     x2,  x3,  [sp, #16 * 1]
    stp     x4,  x5,  [sp, #16 * 2]
    stp     x6,  x7,  [sp, #16 * 3]
    stp     x8,  x9,  [sp, #16 * 4]
    stp     x10, x11, [sp, #16 * 5]
    stp     x12, x13, [sp, #16 * 6]
    stp     x14, x15, [sp, #16 * 7]
    stp     x16, x17, [sp, #16 * 8]
    stp     x18, x19, [sp, #16 * 9]
    stp     x20, x21, [sp, #16 * 10]
    stp     x22, x23, [sp, #16 * 11]
    stp     x24, x25, [sp, #16 * 12]
    stp     x26, x27, [sp, #16 * 13]
    stp     x28, x29, [sp, #16 * 14]
    .if \el == 0
        mrs x21, sp_el0
    .else
        add x21, sp, #S_FRAME_SIZE
    .endif
    mrs     x22, elr_el1
    mrs     x23, spsr_el1
    stp     x30, x21, [sp, #16 * 15]
    stp     x22, x23, [sp, #16 * 16]
    .endm

    .macro kernel_exit, el
    ldp     x22, x23, [sp, #16 * 16]
    ldp     x30, x21, [sp, #16 * 15]
    .if \el == 0
        msr sp_el0, x21
    .endif
    msr     elr_el1,  x22
    msr     spsr_el1, x23
    ldp     x0,  x1,  [sp, #16 * 0]
    ldp     x2,  x3,  [sp, #16 * 1]
    ldp     x4,  x5,  [sp, #16 * 2]
    ldp     x6,  x7,  [sp, #16 * 3]
    ldp     x8,  x9,  [sp, #16 * 4]
    ldp     x10, x11, [sp, #16 * 5]
    ldp     x12, x13, [sp, #16 * 6]
    ldp     x14, x15, [sp, #16 * 7]
    ldp     x16, x17, [sp, #16 * 8]
    ldp     x18, x19, [sp, #16 * 9]
    ldp     x20, x21, [sp, #16 * 10]
    ldp     x22, x23, [sp, #16 * 11]
    ldp     x24, x25, [sp, #16 * 12]
    ldp     x26, x27, [sp, #16 * 13]
    ldp     x28, x29, [sp, #16 * 14]
    add     sp, sp, #S_FRAME_SIZE
    eret
    .endm

    .align 11
    .global vectors
vectors:
    /* Current EL with SP_EL0 (we don't use SP_EL0 in kernel) */
    .align 7 ; b   bad_mode
    .align 7 ; b   bad_mode
    .align 7 ; b   bad_mode
    .align 7 ; b   bad_mode

    /* Current EL with SP_ELx (kernel-mode exceptions) */
    .align 7
el1_sync:
    kernel_entry 1
    mov     x0, sp
    bl      do_sync
    kernel_exit 1

    .align 7
el1_irq:
    kernel_entry 1
    mov     x0, sp
    bl      do_irq
    kernel_exit 1

    .align 7 ; b bad_mode    // FIQ
    .align 7                 // SError
el1_error:
    kernel_entry 1
    mov     x0, sp
    bl      do_serror
    kernel_exit 1

    /* Lower EL using AArch64 (syscalls, EL0 faults — Day 16+) */
    .align 7
el0_sync:
    kernel_entry 0
    mov     x0, sp
    bl      do_sync
    kernel_exit 0

    .align 7
el0_irq:
    kernel_entry 0
    mov     x0, sp
    bl      do_irq
    kernel_exit 0

    .align 7 ; b bad_mode
    .align 7 ; b bad_mode

    /* Lower EL using AArch32 — unused */
    .align 7 ; b bad_mode
    .align 7 ; b bad_mode
    .align 7 ; b bad_mode
    .align 7 ; b bad_mode

bad_mode:
    mov     x0, sp
    bl      do_bad_mode
1:  b       1b
```

### 3.2 `arch/arm64/kernel/exceptions.c`
```c
#include <kernel/printk.h>
#include <kernel/panic.h>
#include <asm-arm64/pt_regs.h>

static const char *ec_str(u32 ec)
{
    switch (ec) {
    case 0x15: return "SVC (syscall)";
    case 0x20: return "Inst abort lower EL";
    case 0x21: return "Inst abort same EL";
    case 0x24: return "Data abort lower EL";
    case 0x25: return "Data abort same EL";
    case 0x3C: return "BRK";
    default:   return "unknown";
    }
}

void do_sync(struct pt_regs *r)
{
    u64 esr, far;
    asm volatile("mrs %0, esr_el1" : "=r"(esr));
    asm volatile("mrs %0, far_el1" : "=r"(far));
    u32 ec = esr >> 26;
    printk(KERN_ERR "SYNC EC=0x%x (%s) FAR=%p ELR=%p\n",
           ec, ec_str(ec), (void *)far, (void *)r->pc);
    panic("unhandled sync exception");
}

void do_irq(struct pt_regs *r)
{
    (void)r;
    printk(KERN_WARN "Unrouted IRQ\n");
}

void do_serror(struct pt_regs *r)
{
    (void)r;
    panic("SError");
}

void do_bad_mode(struct pt_regs *r)
{
    (void)r;
    panic("bad_mode: vector reached unexpectedly");
}
```

### 3.3 Install the vector table
Add to `kmain` early:
```c
extern char vectors[];
asm volatile("msr vbar_el1, %0" :: "r"(vectors));
asm volatile("isb");
```

### 3.4 Deliberate fault test
```c
/* Trigger UNDEF to validate sync path */
printk("Triggering UNDEF for self-test...\n");
asm volatile(".inst 0x00000000");
```
Expected: kernel prints `SYNC EC=0x0 (unknown)` then panics.

---

## 4. ARM64 Cheat-Sheet (Day 4)

```
VBAR_EL1            base of vector table (2 KiB aligned)
ESR_EL1.EC[31:26]   exception class
ESR_EL1.IL          1 if instruction is 32-bit (always 1 in AArch64)
ESR_EL1.ISS[24:0]   syndrome (e.g. data-abort fault status code)
FAR_EL1             faulting virtual address
ELR_EL1             preferred return address
SPSR_EL1            saved PSTATE
DAIF.D/A/I/F        debug/SError/IRQ/FIQ mask bits
```

---

## 5. Pitfalls

1. **Vector table alignment**: `.align 11` (= 2^11 = 2048). Forgetting this → `vbar_el1` write silently ignores low bits, vectors point into garbage.
2. **Each slot must be ≤ 32 instructions** (128 bytes / 4). Don't inline handlers — branch out.
3. **Saving `sp_el0` for EL0 entries only**: confusing kernel/user SP boundaries causes stack corruption.
4. **Reentrancy**: IRQs are masked on exception entry by hardware (PSTATE.I). Don't unmask until handler explicitly safe.
5. **`pt_regs` size mismatch**: `S_FRAME_SIZE` macro and `struct pt_regs` must agree byte-for-byte. Add `asm-offsets.h` generator in Day 17 to enforce.

---

## 6. Verification

```
make run
# Expect:
# Triggering UNDEF for self-test...
# [ERR] SYNC EC=0x0 (unknown) FAR=0x0 ELR=0x40080xxx
# *** KERNEL PANIC ***
# Call trace:
#   [0] 0x40080xxx
```

GDB:
```
(gdb) b do_sync
(gdb) c     # should hit on UNDEF
(gdb) p/x ((struct pt_regs*)$x0)->pc
```

---

## 7. Stretch

- Decode `ESR_EL1.ISS` for data-abort FSC (translation fault, permission fault, etc.) — useful for Day 11.
- Add `regs_to_string` helper printing all x0..x30.
- Stack overflow detection: place guard page below `__stack_bottom` (real impl after MMU).

---

## 8. References

- ARM ARM §D1.10 (Exception handling), §D13 (`ESR_EL1`).
- Linux `arch/arm64/kernel/entry.S` — gold-standard reference.
