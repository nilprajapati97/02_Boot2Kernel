# `__primary_switched` — NVIDIA / Qualcomm Grade Interview Deep Dive

## HOW TO USE THIS DOCUMENT

This document is structured as a real interview answer.
- **Layer 0** = The opening frame — why this function exists and what contract it upholds
- **Layer 1–9** = One layer per code block — each with instruction level, architectural level, and security/performance angle
- **Closing** = The synthesis answer that wins the interview

Read Layer 0 first. Then any layer on demand.

---

## LAYER 0 — The Opening Frame (Say This First in Any Interview)

> "Before I walk the function line by line, let me tell you the CONTRACT this function must
> uphold — because every single instruction makes sense only in that context."

### Position in the Boot Flow

```
Bootloader (EFI/U-Boot)
  │
  └─► primary_entry()              [physical addresses, identity-map, MMU OFF]
        │  bl record_mmu_state
        │  bl preserve_boot_args   ← x21 = FDT physical address saved here
        │  bl __pi_create_init_idmap
        │  bl init_kernel_el       ← x20 = cpu_boot_mode saved here
        │  bl __cpu_setup
        └─► __primary_switch()
              │  bl __enable_mmu   ← MMU turns ON. TTBR1 active.
              │  bl __pi_early_map_kernel  ← KASLR: kernel relocated to random PA+VA
              │  ldr x8, =__primary_switched   ← absolute VA from literal pool
              │  adrp x0, KERNEL_START          ← x0 = __pa(KERNEL_START)
              └─► br x8            ← PC JUMPS to kernel VIRTUAL address space
                    │
                    └─► __primary_switched()   ◄──── YOU ARE HERE
                          └─► start_kernel()   [C code — never returns]
```

### The Architectural Boundary

The `br x8` instruction in `__primary_switch` is **the hardest line in the entire boot sequence**.

| Before `br x8` | After `br x8` |
|---|---|
| PC = physical address (~0x40080000) | PC = kernel virtual address (0xFFFFFF80_1xxxxxxx) |
| TTBR0 identity map active | TTBR1 kernel page tables active |
| Code in `.idmap.text` section | Code in `.text` section |
| Temporary `early_init_stack` | Will be switched to `init_stack` |
| VBAR_EL1 = 0 (garbage) | VBAR_EL1 must be set — NO exceptions can be safely handled yet |

`__primary_switched` is the **FIRST function ever executed entirely in kernel virtual space**.
Every instruction removes exactly one unsafe condition.

### Register State On Entry

```
Register    Value                        Why it still lives here
─────────────────────────────────────────────────────────────────────────────
x0          __pa(KERNEL_START)           Set by adrp in __primary_switch, just before br x8
x20         cpu_boot_mode                Callee-saved x20, set in primary_entry → mov x20, x0
x21         FDT blob physical address    Callee-saved x21, set in preserve_boot_args → mov x21, x0
x29         xzr (0)                      Cleared in __primary_switch: mov x29, xzr
x30         return address to            Set by bl chain; NEVER USED — start_kernel never returns
            __primary_switch
SP          early_init_stack (top)       Temporary 4-page boot stack — not the permanent stack yet
```

### The Danger Window

VBAR_EL1 = 0 at entry. **Any exception before `msr vbar_el1, x8` = CPU fetches handler
from virtual address 0x0 + offset. That address is unmapped. Instant crash. No diagnostic.**

This is why `init_cpu_task` (which establishes a valid SP) MUST run before VBAR is written.
A valid SP is the prerequisite for safe exception entry.

---

## LAYER 1 — `init_cpu_task x4, x5, x6` — CPU Identity Establishment

### The Code

```asm
adr_l   x4, init_task       // x4 = virtual address of init_task (PID 0)
init_cpu_task x4, x5, x6    // macro — 5 operations inside
```

### The Macro Expanded

```asm
// tsk  = x4 = &init_task
// tmp1 = x5
// tmp2 = x6

msr     sp_el0, x4                          // (1) Register current task
ldr     x5, [x4, #TSK_STACK]               // (2a) x5 = init_task.stack (base of stack)
add     sp, x5, #THREAD_SIZE               // (2b) sp = stack base + 16384 (top of 16KB stack)
sub     sp, sp, #PT_REGS_SIZE              // (2c) sp = top - 336 bytes (reserve pt_regs)
stp     xzr, xzr, [sp, #S_STACKFRAME]     // (3a) stackframe.fp=0, stackframe.lr=0
mov     x5, #FRAME_META_TYPE_FINAL         // (3b)
str     x5, [sp, #S_STACKFRAME_TYPE]       // (3c) stamp final unwind marker
add     x29, sp, #S_STACKFRAME             // (3d) x29 (FP) → stackframe
scs_load_current                           // (4)  x18 = init_task.thread_info.scs_sp
adr_l   x5, __per_cpu_offset               // (5a) x5 = &__per_cpu_offset[0]
ldr     w6, [x4, #TSK_TI_CPU]             // (5b) w6 = init_task.thread_info.cpu (= 0)
ldr     x5, [x5, x6, lsl #3]              // (5c) x5 = __per_cpu_offset[0]
set_this_cpu_offset x5                     // (5d) msr tpidr_el1, x5
```

---

### Sub-Operation 1 — `msr sp_el0, x4` → Establish "current"

**What:** `sp_el0` is the EL0 (user) stack pointer. At EL1, it is not used as a stack
pointer. Linux repurposes it as the fastest possible "current task" pointer.

**Why sp_el0 specifically:**
- ARMv8 guarantees `sp_el0` is preserved across EL1 exception entry/exit
- No memory dereference needed — `current` is a single register read: `mrs xN, sp_el0`
- No TLB miss, no cache miss, no pointer dereference

**C expansion:**
```c
static __always_inline struct task_struct *get_current(void) {
    unsigned long sp;
    asm ("mrs %0, sp_el0" : "=r" (sp));
    return (struct task_struct *)sp;
}
#define current  get_current()
```

**Assembly expansion:**
```asm
.macro get_current_task, rd
    mrs  \rd, sp_el0
.endm
```

At SoC clock rates (3GHz+), `current` is called millions of times per second (every
syscall, every interrupt, every scheduler tick). One register read vs. one memory load
is measurable in system throughput.

---

### Sub-Operation 2 — Stack Switch

```asm
ldr     x5, [x4, #TSK_STACK]    // x5 = init_task.stack  (stack base address)
add     sp, x5, #THREAD_SIZE    // sp = base + 16384  (top of stack)
sub     sp, sp, #PT_REGS_SIZE   // sp = top - 336  (reserve pt_regs space)
```

**Key constants:**
- `TSK_STACK` = `offsetof(struct task_struct, stack)` — from `asm-offsets.c`
- `THREAD_SIZE` = `1 << THREAD_SHIFT` = 16384 bytes (16KB) on ARM64
- `PT_REGS_SIZE` = `sizeof(struct pt_regs)` = 336 bytes

**init_stack Memory Layout after these three instructions:**

```
HIGH ADDRESS (init_task.stack + THREAD_SIZE = init_task.stack + 16384)
┌─────────────────────────────────────────────────────────┐  ← stack top
│  pt_regs  [336 bytes reserved]                          │
│    ├── [S_STACKFRAME]  →  fp=0, lr=0 (xzr,xzr)         │
│    ├── [S_STACKFRAME_TYPE] → FRAME_META_TYPE_FINAL      │
│    └── x0..x30, sp, pc, pstate  (rest of pt_regs)       │
├─────────────────────────────────────────────────────────┤  ← SP set HERE
│                                                         │
│   Usable kernel stack space (grows downward)            │
│   Any bl/function call pushes here                      │
│                                                         │
├─────────────────────────────────────────────────────────┤
│   thread_info  (at stack base, embedded in task_struct) │
└─────────────────────────────────────────────────────────┘  ← init_task.stack (LOW ADDRESS)
```

**Why reserve `PT_REGS_SIZE` at the top?**
Every kernel task reserves a `pt_regs` block at the very top of its stack.
The stack unwinder uses this as the "bottom of this task's stack" marker.
Consistency with user tasks and kthreads is intentional — the unwinder
has one unified model that works for all task types.

**Stack direction:** grows DOWN (high → low addresses). `[sp, #-16]!` pre-decrements.

---

### Sub-Operation 3 — Final Frame Marker (Unwind Sentinel)

```asm
stp     xzr, xzr, [sp, #S_STACKFRAME]    // fp=0, lr=0
mov     x5, #FRAME_META_TYPE_FINAL
str     x5, [sp, #S_STACKFRAME_TYPE]     // type = FINAL
add     x29, sp, #S_STACKFRAME           // x29 (FP) points to this frame record
```

**What is `S_STACKFRAME`?**
`S_STACKFRAME = offsetof(struct pt_regs, stackframe)` — defined in `asm-offsets.c`.
The `stackframe` sits INSIDE the `pt_regs` struct reserved at the top of the stack.

**struct frame_record layout:**
```c
struct frame_record {
    unsigned long fp;   // ← set to 0  (null = chain terminates here)
    unsigned long lr;   // ← set to 0
};
u64 type;               // ← FRAME_META_TYPE_FINAL
```

**Why fp=0, lr=0?**
The stack unwinder walks the frame pointer chain: `x29 → prev_fp → prev_prev_fp → ...`
When `fp == 0`, the unwinder stops — this is the architectural BOTTOM of the call stack.
FRAME_META_TYPE_FINAL is a belt-and-suspenders check; the unwinder stops at either.

**Result for boot CPU:**
Any kernel stack trace (oops, KASAN report, lockdep warning) will unwind cleanly through
all call frames and terminate at this sentinel — without walking off into garbage memory.

---

### Sub-Operation 4 — `scs_load_current` → Shadow Call Stack

**CONFIG_SHADOW_CALL_STACK enabled (expansion):**
```asm
mrs     x18, sp_el0                      // x18 = &init_task
ldr     x18, [x18, #TSK_TI_SCS_SP]      // x18 = init_task.thread_info.scs_sp
```

**CONFIG_SHADOW_CALL_STACK disabled:**
```asm
// (empty — assembler emits nothing)
```

**What is the Shadow Call Stack?**
A SEPARATE stack used ONLY for return addresses. Lives in a different memory region.
`x18` is the ABI-reserved SCS pointer register — no compiler or driver may use x18
for any other purpose.

```
Normal stack (init_stack):           Shadow stack (scs_base):
┌────────────────────┐               ┌────────────────────┐
│  saved x30 (ret)   │               │  x30 copy          │  ← verified on every ret
│  saved x29 (fp)    │               │  ...               │
│  local variables   │               └────────────────────┘
└────────────────────┘               ← x18 points here
```

**Security angle:** Defeats ROP (Return-Oriented Programming) attacks. Even if an
attacker overwrites a return address on the normal stack, the return address is
checked against the shadow stack copy. Mismatch → kernel panic.
Used on production Android kernels for Qualcomm Snapdragon devices.

---

### Sub-Operation 5 — Per-CPU Offset → `tpidr_el1`

```asm
adr_l   x5, __per_cpu_offset            // x5 = &__per_cpu_offset[0]
ldr     w6, [x4, #TSK_TI_CPU]          // w6 = init_task.thread_info.cpu  (= 0 for boot CPU)
ldr     x5, [x5, x6, lsl #3]           // x5 = __per_cpu_offset[cpu_id]  (lsl #3 = x8 = sizeof u64)
set_this_cpu_offset x5                  // msr tpidr_el1, x5  (or tpidr_el2 under VHE)
```

**What is `__per_cpu_offset`?**
An array: `u64 __per_cpu_offset[NR_CPUS]`.
Each entry = base virtual address of that CPU's per-CPU data section.

**What is `tpidr_el1`?**
ARMv8 "Thread Pointer ID Register EL1" — a system register reserved for per-CPU base.
No memory walk needed — one register read gives the per-CPU base address.

**How every `this_cpu_read(var)` works after this:**
```c
// this_cpu_read(runqueues)  compiles to:
//   mrs   x0, tpidr_el1              ← get per-CPU base (one instruction)
//   ldr   x0, [x0, #offset_of_var]  ← load var relative to base
```

**Memory layout:**
```
Physical RAM (per-CPU sections):
┌─────────────────────────────────────┐
│  CPU0 per-cpu section               │ ← __per_cpu_offset[0]  (tpidr_el1 = this)
├─────────────────────────────────────┤
│  CPU1 per-cpu section               │ ← __per_cpu_offset[1]
├─────────────────────────────────────┤
│  ...  (up to NR_CPUS)               │
└─────────────────────────────────────┘
```

> **INTERVIEW KEY POINT — Why does `init_cpu_task` come FIRST?**
>
> The ORDER of operations in `init_cpu_task` is a dependency graph, not a checklist:
> - `sp_el0` (current) must be set before the stack switch — if an NMI fires after
>   the stack switch but before `sp_el0` is set, `current` returns garbage.
> - The stack switch must complete before `VBAR_EL1` is written — exception entry
>   needs a valid SP or the kernel will corrupt random memory.
> - `tpidr_el1` must be set before any C code — C code may call `this_cpu_read()` in
>   its very first instruction.
> - EVERYTHING in `init_cpu_task` must complete before `bl finalise_el2` or
>   `bl start_kernel` — those branches generate return addresses that get pushed to
>   the stack, requiring a valid SP and valid stack frame.

---

## LAYER 2 — `msr vbar_el1, x8` + `isb` — Arming the Exception System

### The Code

```asm
adr_l   x8, vectors          // x8 = kernel virtual address of exception vector table
msr     vbar_el1, x8         // VBAR_EL1 = &vectors
isb                           // flush pipeline — make VBAR effective immediately
```

### What is VBAR_EL1?

Vector Base Address Register EL1. Every exception the CPU takes (IRQ, FIQ, SError,
Synchronous fault, Data abort, Instruction abort) dispatches to:

```
Handler address = VBAR_EL1 + offset
```

Where offset is determined by exception type and source EL (see table below).

### ARMv8 Exception Vector Table Layout (2KB aligned)

```
Offset   Exception type          Source EL / SP
──────   ────────────────────   ────────────────────────────────
0x000    Synchronous             Current EL,  SP_EL0
0x080    IRQ / vIRQ              Current EL,  SP_EL0
0x100    FIQ / vFIQ              Current EL,  SP_EL0
0x180    SError / vSError        Current EL,  SP_EL0
0x200    Synchronous             Current EL,  SP_ELx  ← kernel uses THIS group
0x280    IRQ                     Current EL,  SP_ELx
0x300    FIQ                     Current EL,  SP_ELx
0x380    SError                  Current EL,  SP_ELx
0x400    Synchronous             Lower EL, AArch64
0x480    IRQ                     Lower EL, AArch64
0x500    FIQ                     Lower EL, AArch64
0x580    SError                  Lower EL, AArch64
0x600    Synchronous             Lower EL, AArch32
...
```

Kernel EL1 exceptions use the `0x200` group (SP_ELx = SP_EL1 = kernel SP).
`vectors` is defined in `arch/arm64/kernel/entry.S`, mapped via TTBR1.

### Why `isb` is Mandatory

`msr` writes to a system register. The ARMv8 architecture does NOT guarantee that the
new VBAR value is visible to the instruction fetch pipeline without a context
synchronization event.

Without `isb`: A fault in the instruction pipeline FOLLOWING the `msr` could still
use the old VBAR (= 0). The CPU fetches the handler from `0x0 + offset` = unmapped
→ translation fault → CPU tries to take THAT exception → uses old VBAR again → hang.

`isb` (Instruction Synchronization Barrier) flushes the pipeline and refetches all
subsequent instructions, ensuring they see the new VBAR value.

### The Before/After Safety Boundary

```
Before msr vbar_el1:   Any exception → dispatch to 0x0 + offset → CRASH (unmapped)
After  msr vbar_el1:   Any exception → dispatch to &vectors + offset → HANDLED safely
```

---

## LAYER 3 — `stp x29, x30, [sp, #-16]!` — C ABI Stack Frame

### The Code

```asm
stp     x29, x30, [sp, #-16]!   // pre-decrement SP by 16, store {FP, LR}
mov     x29, sp                  // update frame pointer = new SP
```

### Why This Is Here (Not Just "Function Prologue")

This is AAPCS64 (ARM64 Procedure Call Standard) function prologue.

The `stp` instruction pre-decrements SP by 16, then stores:
- `x29` (FP = frame pointer from `init_cpu_task`) at `[sp+0]`
- `x30` (LR = return address back to `__primary_switch`) at `[sp+8]`

Then `mov x29, sp` makes x29 point to this new frame record.

**Critical insight:** `bl finalise_el2` and `bl start_kernel` BOTH overwrite `x30`.
Without the `stp` here, the ORIGINAL `x30` (return address to `__primary_switch`) would
be permanently lost after the first `bl`. The stack frame would be broken for the unwinder.

**Stack state after these two instructions:**

```
Before:                          After:
                                 SP────►┌───────────────────┐ (= new x29)
         SP────►┌─────────┐     │      │  saved x29 (FP)   │ [sp+0]
                │  free   │     │      │  saved x30 (LR)   │ [sp+8]
                │  space  │     │      └───────────────────┘
```

**Frame pointer chain at this point:**

```
x29 (current) → [saved_x29_from_init_cpu_task, saved_x30]
     ↑
saved_x29_from_init_cpu_task → [fp=0, lr=0]  (the FINAL frame from init_cpu_task)
     ↑
STOP (fp == 0 → unwinder terminates)
```

**Why the return address will never be used:** `start_kernel()` ends in `rest_init()` which
enters the idle loop. It is architecturally guaranteed never to return. `ASM_BUG()` enforces
this contract in machine code.

---

## LAYER 4 — `str_l x21, __fdt_pointer, x5` — The Hardware Handshake

### The Code

```asm
str_l   x21, __fdt_pointer, x5
```

**Macro expansion of `str_l`:**
```asm
adrp    x5, __fdt_pointer                // x5 = page-aligned VA of __fdt_pointer
str     x21, [x5, :lo12:__fdt_pointer]  // store x21 → *__fdt_pointer
```

### The Register x21

`x21` has carried the FDT physical address as a callee-saved register since the VERY FIRST
instruction of `primary_entry` (in `preserve_boot_args`: `mov x21, x0`). It has survived
uncorrupted through:

```
preserve_boot_args → create_init_idmap → init_kernel_el → __cpu_setup →
__primary_switch → __enable_mmu → __pi_early_map_kernel → br x8 → HERE
```

This is a 10+ function chain where x21 is never touched. This is why ARM64 dedicates
x19-x28 as callee-saved registers — functions must preserve them.

### The Global Variable `__fdt_pointer`

```c
// arch/arm64/kernel/setup.c
phys_addr_t __fdt_pointer __initdata;
```

`__initdata` means it lives in `.init.data` section — freed after `kernel_init` runs.

### Why Store Physical Address (Not Virtual)?

The FDT blob is NOT yet mapped in kernel virtual address space.
`setup_machine_fdt()` calls `early_memremap(__fdt_pointer, size)` to create a temporary
mapping — and for that it needs the PHYSICAL address.

### Who Uses It

```
start_kernel()
  └─► setup_arch()
        └─► setup_machine_fdt(__fdt_pointer)
              └─► of_flat_dt_check_header()
              └─► early_init_dt_scan()
                    ├─► Discovers memory nodes (RAM banks)
                    ├─► Discovers CPU topology (big.LITTLE, DSU clusters)
                    ├─► Discovers interrupt controllers (GIC-600, etc.)
                    ├─► Discovers clocks, power domains, thermal zones
                    └─► Platform: Qualcomm SM8550, NVIDIA Tegra, etc.
```

---

## LAYER 5 — `kimage_voffset` — The Linchpin of Virtual↔Physical Translation

### The Code

```asm
adrp    x4, _text                    // x4 = VIRTUAL address of kernel .text start
sub     x4, x4, x0                   // x4 = VA(_text) - PA(KERNEL_START)
str_l   x4, kimage_voffset, x5      // store into global
```

### The Formula

```
kimage_voffset = VA(_text) - PA(KERNEL_START)
```

This single value is the foundation of EVERY `virt_to_phys()` and `phys_to_virt()`
call in the entire kernel — millions of calls per second.

```c
// include/asm/memory.h
static inline phys_addr_t __virt_to_phys(unsigned long x) {
    return (phys_addr_t)(x - kimage_voffset);
}
static inline unsigned long __phys_to_virt(phys_addr_t x) {
    return (unsigned long)(x + kimage_voffset);
}
```

### How `adrp` Gives Virtual Address

`adrp` is PC-relative. After `br x8`, the PC is in kernel VIRTUAL address space
(TTBR1 active). Therefore `adrp x4, _text` computes:

```
x4 = PAGE_ALIGN(PC_virtual) + page_offset(_text)
   = Virtual address of _text
```

### How x0 Gives Physical Address

`x0` was loaded in `__primary_switch` by:
```asm
adrp    x0, KERNEL_START    // PC was in identity-mapped physical space (TTBR0 active)
```
At that point, `adrp` gave a physical address (since PC = PA ≈ VA in identity map).
That value was carried through the `br x8` jump unchanged.

### Concrete Example

```
x4 (virtual):  0xFFFFFF80_10080000
x0 (physical): 0x00000000_40080000
               ─────────────────────────────────────────
kimage_voffset: 0xFFFFFF7F_D0000000
```

### KASLR Impact (Interview Bonus)

Without KASLR, `kimage_voffset` is the same every boot. An attacker can hardcode it.

With `CONFIG_RANDOMIZE_BASE`, `__pi_early_map_kernel()` picks random PA and VA
for the kernel image. By the time we reach this point:
- `adrp x4, _text` → gives RANDOMIZED virtual address
- `x0` → holds RANDOMIZED physical load address

Result: `kimage_voffset` is computed CORRECTLY for whatever KASLR chose.

```
Without KASLR (every boot):
  VA = 0xFFFFFF80_10080000, PA = 0x40080000
  kimage_voffset = 0xFFFFFF7F_D0000000  ← predictable

With KASLR (changes each boot):
  VA = 0xFFFFFF9A_B3200000, PA = 0x52A00000
  kimage_voffset = 0xFFFFFF9A_60E00000  ← unpredictable
```

**Security property:** `kimage_voffset` is declared `__ro_after_init`:
```c
// arch/arm64/mm/mmu.c
u64 kimage_voffset __ro_after_init;
```
After `kernel_init` completes, the page table entry for `kimage_voffset` is flipped
to read-only at the hardware level. No kernel code can corrupt it afterward.

---

## LAYER 6 — `set_cpu_boot_mode_flag` — The EL Decision Preserved

### The Code

```asm
mov     x0, x20                 // x20 = cpu_boot_mode (EL1 or EL2)
bl      set_cpu_boot_mode_flag
```

### `set_cpu_boot_mode_flag` Internals

```asm
SYM_FUNC_START_LOCAL(set_cpu_boot_mode_flag)
    adr_l   x1, __boot_cpu_mode        // x1 = &__boot_cpu_mode[0]
    cmp     w0, #BOOT_CPU_MODE_EL2
    b.ne    1f
    add     x1, x1, #4                 // if EL2: x1 → __boot_cpu_mode[1]
1:  str     w0, [x1]                   // store boot mode word
    ret
SYM_FUNC_END(set_cpu_boot_mode_flag)
```

### `__boot_cpu_mode` Layout

```c
// arch/arm64/include/asm/virt.h
extern u32 __boot_cpu_mode[2];
//   [0] = EL1 slot  — written if booted at EL1 (BOOT_CPU_MODE_EL1 = 0x1)
//   [1] = EL2 slot  — written if booted at EL2 (BOOT_CPU_MODE_EL2 = 0x2)
```

### Who Reads `__boot_cpu_mode` Later

| Subsystem | Function | What it checks |
|---|---|---|
| KVM | `kvm_arch_init_vm()` | Was EL2 available? Can we init hypervisor? |
| Hotplug | Secondary CPU startup | Must boot at same EL as primary |
| Query | `is_hyp_mode_available()` | Is EL2 / virtualization available? |
| Query | `is_kernel_in_hyp_mode()` | Is kernel running AT EL2 (VHE mode)? |

**Two-slot design rationale:** Secondary CPUs can check their own boot mode against slot[0]
and slot[1] in a lockless, concurrent-safe way — no synchronization primitive needed
because each CPU writes only its own designated slot.

---

## LAYER 7 — `kasan_early_init` (Conditional) — Shadow Memory Mapping

### The Code

```c
#if defined(CONFIG_KASAN_GENERIC) || defined(CONFIG_KASAN_SW_TAGS)
    bl    kasan_early_init
#endif
```

### What KASAN Does

KASAN (Kernel Address SANitizer) instruments EVERY memory access at compile time.
For each access, the compiler inserts a shadow byte check:

```c
// Before every load/store, compiler inserts something equivalent to:
shadow_addr = (real_addr >> 3) + KASAN_SHADOW_OFFSET;
if (*shadow_addr != 0) {
    kasan_report(real_addr, size, is_write, ip);
}
```

**Errors detected:** use-after-free, out-of-bounds, use-before-init, double-free,
stack buffer overflow, global buffer overflow.

### Why `kasan_early_init` Must Run HERE

```
The very first instruction of start_kernel() is KASAN-instrumented.
If the shadow region is not mapped → first instrumented access → page fault
→ exception dispatch (needs VBAR, needs SP — both already set) → no shadow mapping
→ takes another page fault → infinite recursion → hang / stack overflow
```

### Shadow Memory Calculation

$$\text{shadow size} = \frac{\text{kernel VA space}}{8}$$

On ARM64 with 48-bit VA: `256TB / 8 = 32TB` of shadow region.
`kasan_early_init()` maps this using huge pages (block descriptors) to keep the page
table small and TLB pressure minimal.

### KASAN Variant Comparison

| Variant | Shadow memory | Overhead | Use case |
|---|---|---|---|
| `KASAN_GENERIC` | Software, needs early init | ~2x memory, ~2-3x runtime | Debug kernels |
| `KASAN_SW_TAGS` | Software + pointer tags | Lower than generic | Android kernel testing |
| `KASAN_HW_TAGS` | ARM MTE hardware | Near zero | Production Android (Pixel 6+) |

`KASAN_HW_TAGS` does NOT appear here because it requires no early shadow mapping —
the hardware tag checking is done by the memory system itself.

---

## LAYER 8 — `finalise_el2` — The VHE Decision (Performance Critical)

### The Code

```asm
mov     x0, x20         // pass cpu_boot_mode
bl      finalise_el2    // decide: stay at EL2 (VHE) or drop to EL1
```

### The VHE Architecture (ARMv8.1+)

**Without VHE — Classic two-level hypervisor:**
```
EL0 = Userspace
EL1 = Linux kernel
EL2 = KVM hypervisor

Every KVM guest entry:  EL1 → EL2  (world switch = flush TLBs, change TTBR, etc.)
Every KVM guest exit:   EL2 → EL1  (reverse world switch)
```

**With VHE (HCR_EL2.E2H = 1) — Merged EL1/EL2:**
```
EL0 = Userspace (unchanged)
EL1/EL2 merged: Linux kernel runs "at EL2" transparently
KVM host code executes without EL level transitions = FAST
```

### How `finalise_el2` Decides

```
x0 == BOOT_CPU_MODE_EL2  AND  HCR_EL2.E2H == 1  ?
  YES → Keep VHE active. Kernel continues at EL2.
  NO  → Execute ERET to EL1. Standard non-VHE operation.
```

### ARM System Register Aliasing Under VHE

With HCR_EL2.E2H=1, the hardware TRANSPARENTLY redirects:

| Kernel writes... | Hardware actually writes... |
|---|---|
| `sctlr_el1` | `sctlr_el2` |
| `ttbr0_el1` | `ttbr0_el2` |
| `tcr_el1` | `tcr_el2` |
| `vbar_el1` | `vbar_el2` |

**Zero kernel code changes required to benefit from VHE.** This is by ARM architectural design.

### Performance Impact (Qualcomm/NVIDIA Relevance)

On Qualcomm SM8550 (Snapdragon 8 Gen 2) running Android:
- VHE eliminates ~300ns per KVM entry/exit
- With pKVM (protected KVM in Android), VHE enables ~40% better hypervisor performance
- NVIDIA Orin uses VHE for DRIVE OS virtualization stack

### After `finalise_el2`

The exception level for kernel execution is FINAL. Every subsequent call — including
`start_kernel()` — runs at this EL. All sysreg accesses, exception handling, and
KVM operations are based on this decision.

---

## LAYER 9 — `bl start_kernel` + `ASM_BUG()` — The Point of No Return

### The Code

```asm
ldp     x29, x30, [sp], #16    // restore stack frame pushed at Layer 3
bl      start_kernel            // call into C kernel — NEVER returns
ASM_BUG()                       // panic if start_kernel ever returns (BUG contract)
```

### `ldp` — ABI Restoration Before `bl`

Pops `{x29, x30}` saved at Layer 3. SP post-increments by 16.
This restores the frame pointer chain to the state after `init_cpu_task`.
**The restored x30 will never be used** — it is done for ABI correctness only,
so the stack is in a consistent state when `start_kernel` begins executing.

### `bl start_kernel`

`bl` sets `x30 = PC+4` (return address after the `bl` instruction) then branches.
`start_kernel()` is in `init/main.c`. It performs:

```
start_kernel()
  ├── setup_arch()              ← parses FDT, sets up memory model
  ├── mm_init()                 ← buddy allocator, slab, vmalloc
  ├── sched_init()              ← scheduler data structures
  ├── irq_init()                ← interrupt controller setup
  ├── timekeeping_init()        ← clocksource, clockevent
  ├── early_boot_irqs_disabled_check()
  ├── console_init()
  └── rest_init()
        ├── kernel_thread(kernel_init)  ← PID 1 (init process)
        └── cpu_startup_entry()         ← CPU0 enters idle loop — NEVER RETURNS
```

### `ASM_BUG()` — The Machine Code Contract

```c
// include/asm/bug.h (arm64)
#define ASM_BUG()  \
    brk #BUG_BRK_IMM   // BRK instruction — software breakpoint
```

`BRK` causes a Synchronous Debug Exception. The exception handler converts it to
`BUG()` → kernel panic with:
```
------------[ cut here ]------------
kernel BUG at arch/arm64/kernel/head.S:XXX!
```

This is not defensive programming. It is a DESIGN CONTRACT encoded in machine code:
"If you are reading this, the kernel's fundamental architectural invariant has been violated."

### System State Handed to `start_kernel()`

```
╔══════════════════════════════════════════════════════════════════════╗
║  Resource           State                                           ║
║  ─────────────────────────────────────────────────────────────────  ║
║  MMU                ON — TTBR0 (identity) + TTBR1 (kernel) active  ║
║  Kernel stack       Valid — init_stack, 16KB, SP properly set       ║
║  current (sp_el0)   &init_task  (PID 0, swapper)                   ║
║  VBAR_EL1           &vectors  (all exceptions handled safely)       ║
║  tpidr_el1          __per_cpu_offset[0]  (per-CPU variables work)   ║
║  kimage_voffset     Computed  (virt_to_phys / phys_to_virt work)   ║
║  __fdt_pointer      Set  (device tree accessible to setup_arch)     ║
║  __boot_cpu_mode    Set  (EL1/EL2 known to KVM and hotplug)        ║
║  KASAN shadow       Mapped (if CONFIG_KASAN_GENERIC/SW_TAGS)       ║
║  Exception level    Final  (EL1 or EL2/VHE — decided by finalise)  ║
║  Frame chain        Valid — terminates at fp=0 in init_stack        ║
║  Shadow call stack  Loaded — x18 = init_task scs_sp (if CONFIG)    ║
╚══════════════════════════════════════════════════════════════════════╝
→ All infrastructure needed by C code is in place.
→ start_kernel() can safely use any kernel subsystem.
```

---

## THE CLOSING ANSWER — Say This Last

> "`__primary_switched` has no optional steps and no redundant steps.
> Every instruction removes exactly one unsafe condition that would cause `start_kernel()`
> to fault. The order is a **dependency graph**, not a checklist:
>
> 1. Stack before VBAR — because exception entry needs a valid SP.
> 2. VBAR before any `bl` — because `bl` modifies x30 and the branch predictor's
>    return stack, either of which could trigger a fault.
> 3. `kimage_voffset` before KASAN — because KASAN's shadow mapping itself calls
>    `phys_to_virt()` internally.
> 4. KASAN before `start_kernel` — because the first C instruction is already instrumented.
> 5. `finalise_el2` before `start_kernel` — because the EL decision must be final before
>    C code touches any sysreg.
>
> Remove any one step, or reorder any two adjacent steps, and the kernel dies silently
> with no diagnostic output — because the diagnostic infrastructure you need to debug
> the failure IS the thing you just broke.
>
> This function is 28 lines of assembly that make a 20 million line C codebase possible."

---

## QUICK REFERENCE — Instruction → Purpose Table

| Line | Instruction(s) | What it Enables |
|---|---|---|
| 1-2 | `adr_l x4, init_task` + `init_cpu_task` | Current task, valid SP, frame chain, SCS, per-CPU |
| 3-5 | `adr_l x8, vectors` + `msr vbar_el1` + `isb` | Safe exception handling |
| 6-7 | `stp x29,x30,[sp,#-16]!` + `mov x29,sp` | C ABI stack frame for `bl` calls |
| 8 | `str_l x21, __fdt_pointer` | Device tree accessible to `setup_arch` |
| 9-11 | `adrp x4,_text` + `sub x4,x4,x0` + `str_l kimage_voffset` | `virt_to_phys` / `phys_to_virt` work |
| 12-13 | `mov x0,x20` + `bl set_cpu_boot_mode_flag` | KVM and hotplug know the EL |
| 14-16 | `bl kasan_early_init` (conditional) | KASAN shadow accessible before first C access |
| 17-18 | `mov x0,x20` + `bl finalise_el2` | Exception level finalized |
| 19-21 | `ldp` + `bl start_kernel` + `ASM_BUG()` | Kernel handoff |

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