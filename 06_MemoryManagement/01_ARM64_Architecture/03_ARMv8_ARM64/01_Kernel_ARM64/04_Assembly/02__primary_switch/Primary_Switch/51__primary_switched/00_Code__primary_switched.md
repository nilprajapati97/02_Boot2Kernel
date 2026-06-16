# Deep Dive: `ldr x8, =__primary_switched` on ARMv8

## 1. It's a Pseudo-Instruction — What the Assembler Actually Generates

`ldr x8, =__primary_switched` is **not a real ARM64 instruction**. The GNU assembler (GAS) transforms it into a **literal pool load**:

```asm
// What the assembler actually emits:
ldr  x8, [pc, #offset]        // Real instruction: PC-relative load of 8 bytes
...
.Lpool: .quad 0xFFFFFF8010XXXXXX   // Literal pool: absolute virtual address of __primary_switched
```

The real instruction encoding is `LDR (literal)`:

```
Bits [31:30] = 01       → 64-bit load
Bits [29:27] = 011      → LDR literal variant
Bits [23:5]  = imm19    → signed offset in units of 4 bytes (±1MB range from PC)
Bits [4:0]   = 01000    → destination = x8
```

Effective address: `EA = PC + SignExtend(imm19 << 2)`

---

## 2. WHY `ldr =` and NOT `adrp` / `adr_l` — The Core Insight

This is the most important concept. Look at the execution context:

```
__primary_switch:
    bl  __enable_mmu          ← MMU turned ON here
    bl  __pi_early_map_kernel ← Kernel page tables set up
    ldr x8, =__primary_switched  ← *** THIS LINE ***
    br  x8                    ← Jump to virtual address space
```

| Addressing Method | What It Computes | Problem |
|---|---|---|
| `adrp x8, __primary_switched` | `PC + signed_offset` | PC is in **physical/idmap space** (~0x40080000). Result is still a physical-space address |
| `ldr x8, =__primary_switched` | **Absolute virtual address** from literal pool | Correctly gets the linker-assigned virtual address (~0xFFFFFF8010XXXXXX) |

The literal pool stores a value embedded by the **linker** at link time — the absolute virtual address — not computed relative to the current PC.

---

## 3. Memory Map at This Point in Execution

```
TTBR0_EL1 → Identity Map (physical ≈ virtual, low addresses 0x00000000_XXXXXXXX)
TTBR1_EL1 → Kernel Map  (virtual high addresses 0xFFFFFF80_XXXXXXXX → physical RAM)

Physical RAM layout:
┌──────────────────────────────────────────────────┐
│ 0x40080000  __primary_switch code  (.idmap.text) │  ← CPU is HERE
│ 0x40080010  [literal pool entry]                 │  ← .quad 0xFFFFFF8010080000
└──────────────────────────────────────────────────┘
                        │
                        │ TTBR1_EL1 maps this ↓
┌──────────────────────────────────────────────────┐
│ 0xFFFFFF8010080000  __primary_switched           │  ← CPU will JUMP here
└──────────────────────────────────────────────────┘
```

---

## 4. CPU Pipeline Level (ARMv8 Out-of-Order Core, e.g., Cortex-A76/Neoverse)

### Stage 1 — Instruction Fetch (IF)
- PC points into `.idmap.text` (identity-mapped, low physical address)
- TTBR0_EL1 identity map is active for this range → TLB hit (already populated)
- I-Cache fetches the instruction `0x58xxxxxx` (LDR literal encoding)

### Stage 2 — Decode (ID)
- Decode unit recognizes `LDR (literal)` — a **load from PC-relative address**
- No base register needed; AGU computes: `EA = PC + (imm19 << 2)`
- Destination register: x8

### Stage 3 — Address Generation (AG/EX)
```
EA = 0x40080008 (current PC) + 0x8 (imm19 offset) = 0x40080010
```
- This is the **literal pool address**, sitting right after the `br x8` instruction in the `.idmap.text` section

### Stage 4 — Memory Access (MEM)
- Load 8 bytes from `EA = 0x40080010`
- MMU Translation:
  - Address `0x40080010` → TTBR0_EL1 (identity map) → physical `0x40080010`
  - This is a **normal cacheable memory** region
- L1 D-Cache lookup: likely a **cache hit** (code nearby, prefetcher may have pulled it)
- 8 bytes loaded = `0xFFFFFF8010XXXXXX` (the virtual address of `__primary_switched`)

### Stage 5 — Write Back (WB)
- `x8 ← 0xFFFFFF8010XXXXXX`
- The **absolute virtual address** of `__primary_switched` is now in x8

---

## 5. What the Literal Pool Value Is and How It Gets There

The linker script vmlinux.lds.S places `__primary_switched` at a specific **virtual address** in the `.text` section:

```
KERNEL_VIRTUAL_BASE = 0xFFFFFF8010000000  (example, depends on CONFIG)
__primary_switched  = KERNEL_VIRTUAL_BASE + TEXT_OFFSET + delta
```

The assembler emits a **relocation entry** for the literal pool. The linker resolves it and patches the `.quad` with this final virtual address.

With **KASLR** (Kernel Address Space Layout Randomization), `__pi_early_map_kernel` patches these relocation entries at runtime to reflect the randomized virtual base — so by the time `ldr x8, =__primary_switched` executes, the literal pool already contains the **correct randomized virtual address**.

---

## 6. The `br x8` That Follows — The Virtual Address Switchover

```asm
ldr  x8, =__primary_switched   // x8 = 0xFFFFFF8010XXXXXX
adrp x0, KERNEL_START          // x0 = __pa(KERNEL_START)
br   x8                        // *** JUMP INTO VIRTUAL ADDRESS SPACE ***
```

After `br x8`:
- **Before**: CPU fetching from `0x40080xxx` (physical, TTBR0_EL1, identity map)
- **After**: CPU fetching from `0xFFFFFF80_1XXXXXXX` (virtual, TTBR1_EL1, kernel map)

This is the **irreversible crossing** from the boot identity-map world into the kernel virtual address world.

The indirect branch (`br x8`) goes through the CPU's **Indirect Branch Predictor / Branch Target Buffer (BTB)**. Since this is the first execution, the BTB has no entry — the CPU stalls the pipeline until x8 is forwarded from the load result, then redirects the fetch to `0xFFFFFF80...`.

---

## 7. Why Not Just Use `adr_l`?

```asm
// adr_l expands to:
adrp x8, __primary_switched         // x8 = PC + page_offset  → PHYSICAL domain
add  x8, x8, :lo12:__primary_switched
br   x8  // WRONG — jumps to physical address, not virtual!
```

`adr_l` would produce `0x40080XXX` — a physical address. After the MMU is on, jumping there would **still work** only because of the identity map, but `__primary_switched` at the physical address contains bootstrap code meant to run once. The virtual address `0xFFFFFF80...` is where `__primary_switched` is permanently mapped for the kernel's lifetime.

---

## 8. Complete Annotated Summary

```asm
SYM_FUNC_START_LOCAL(__primary_switch)
    // Step 1: Enable MMU
    // TTBR0 = identity map, TTBR1 = kernel page tables
    // CPU still executing at physical ≈ virtual (low addr, TTBR0 range)
    adrp  x1, reserved_pg_dir
    adrp  x2, __pi_init_idmap_pg_dir
    bl    __enable_mmu

    // Step 2: Build kernel page tables + apply KASLR relocations
    // After this, literal pool is patched with correct virtual addr
    bl    __pi_early_map_kernel

    // Step 3: Load ABSOLUTE VIRTUAL address from literal pool
    // Assembler emits: ldr x8, [pc, #offset]
    // Literal pool (in .idmap.text, accessible via identity map TTBR0):
    //   .quad <linker-assigned virtual addr of __primary_switched>
    // x8 ← 0xFFFFFF8010XXXXXX  (kernel virtual address)
    ldr   x8, =__primary_switched

    // x0 = physical address of KERNEL_START (argument to __primary_switched)
    adrp  x0, KERNEL_START

    // Step 4: VIRTUAL ADDRESS SWITCHOVER
    // Indirect branch: PC ← x8 = 0xFFFFFF8010XXXXXX
    // Now executing via TTBR1_EL1, in kernel virtual address space
    br    x8
SYM_FUNC_END(__primary_switch)
```

---

## Key Takeaways for the Nvidia Interview

| Concept | Detail |
|---|---|
| **Pseudo-instruction** | `ldr =` → assembler emits `ldr [pc, #offset]` + literal pool `.quad` |
| **Literal pool** | 8-byte constant embedded in `.idmap.text`, holds linker-assigned virtual address |
| **PC-relative load** | `EA = PC + SignExtend(imm19 << 2)`, range ±1MB, accesses TTBR0 identity map |
| **Why not `adrp`** | `adrp` is PC-relative → gives physical-domain address, not the kernel virtual address |
| **MMU state** | Both TTBR0 (identity) and TTBR1 (kernel) are active; literal pool accessed via TTBR0 |
| **KASLR interaction** | `__pi_early_map_kernel` patches the literal pool before this load executes |
| **`br x8`** | Indirect branch into TTBR1_EL1 space — the virtual address switchover moment |



Searched for regex `TSK_STACK|TSK_TI_CPU|S_STACKFRAME|THREAD_SIZE|PT_REGS_SIZE|scs_load_current` (`**/arch/arm64/**`), 20 results

Searched for regex `kimage_voffset|__fdt_pointer|__per_cpu_offset` (`**/arch/arm64/**`), 20 results

Searched for text `struct task_struct` (`**/include/linux/sched.h`), 10 results

Searched for text `DEFINE(TSK_STACK` (`**/arch/arm64/**`), 2 results

Searched for text `THREAD_SIZE` (`**/arch/arm64/include/asm/thread_info.h`), no results

Read [](file:///c%3A/My_Projects/Kernel_Repo/linux/arch/arm64/kernel/asm-offsets.c#1-1), lines 1 to 80

Searched for text `THREAD_SIZE` (`**/arch/arm64/include/asm/**`), 8 results

Searched for text `scs_load_current` (`**/arch/arm64/include/asm/scs.h`), 4 results

Read [](file:///c%3A/My_Projects/Kernel_Repo/linux/arch/arm64/include/asm/scs.h#1-1), lines 1 to 50

Read [](file:///c%3A/My_Projects/Kernel_Repo/linux/arch/arm64/include/asm/memory.h#120-120), lines 120 to 145

Searched for text `adr_l|str_l` (`**/arch/arm64/include/asm/assembler.h`), 3 results

Searched for text `set_this_cpu_offset|get_current_task` (`**/arch/arm64/include/asm/**`), 5 results

Read [](file:///c%3A/My_Projects/Kernel_Repo/linux/arch/arm64/include/asm/assembler.h#170-170), lines 170 to 240

Read [](file:///c%3A/My_Projects/Kernel_Repo/linux/arch/arm64/include/asm/assembler.h#585-585), lines 585 to 605

# Deep Dive: `__primary_switched` — Memory Perspective (ARMv8)

## System State at Entry

When `br x8` fires from `__primary_switch`, the CPU crosses into this function with:

```
MMU:   ON  (TTBR0 = identity map,  TTBR1 = kernel page tables)
PC:    0xFFFFFF8010XXXXXX  (kernel virtual address space, TTBR1 range)
x0  =  __pa(KERNEL_START)  (physical address, passed as argument)
x20 =  cpu_boot_mode       (EL1 or EL2)
x21 =  FDT physical address (saved since primary_entry)
```

---

## Block 1 — `init_cpu_task`: Building the Initial Kernel Stack

```asm
adr_l   x4, init_task
init_cpu_task x4, x5, x6
```

### Step 1: `adr_l x4, init_task`

`adr_l` is a macro (expands to `adrp` + `add`):
```asm
adrp  x4, init_task        // x4 = PAGE_ALIGN(PC) + page_offset_of(init_task)
add   x4, x4, :lo12:init_task  // x4 = exact virtual address of init_task
```

`init_task` is the **statically allocated** `task_struct` for PID 0 (the swapper/idle process). It lives in `.data` in the kernel image. Now `x4 = &init_task` (virtual).

### Step 2: `init_cpu_task` Macro Expanded — Memory Layout

```asm
// 1. Register init_task as "current"
msr   sp_el0, x4           // sp_el0 = &init_task (ARM convention: current task)
```

`sp_el0` at EL1 is unused as a stack pointer — the kernel **repurposes it as the `current` pointer**. Any code calling `get_current_task` reads this register (`mrs rd, sp_el0`).

```asm
// 2. Find the kernel stack base
ldr   x5, [x4, #TSK_STACK]  // x5 = init_task.stack (pointer to stack allocation)
```

`TSK_STACK = offsetof(struct task_struct, stack)` — a compile-time constant from asm-offsets.c. `init_task.stack` points to the **bottom** of a statically allocated 16KB region (`init_stack` in `.bss`).

```
init_stack memory layout (16KB = THREAD_SIZE):
┌─────────────────────────────────┐ ← init_task.stack  (LOW address, stack BOTTOM)
│  thread_info  (at stack bottom) │
│  ...                            │
│  (16KB of stack space)          │
│                                 │
│  pt_regs  ← SP will point here  │
└─────────────────────────────────┘ ← init_task.stack + THREAD_SIZE (HIGH address, stack TOP)
```

```asm
// 3. Set SP to top of stack, minus pt_regs reservation
add   sp, x5, #THREAD_SIZE   // sp = top of stack (16KB up from base)
sub   sp, sp, #PT_REGS_SIZE  // sp = top_of_stack - sizeof(pt_regs)
```

`PT_REGS_SIZE = sizeof(struct pt_regs) = 336 bytes`. This **pre-reserves** a `pt_regs` frame at the very top of the kernel stack. This is intentional — every task's stack has `pt_regs` at the top for the unwinder.

```
Stack after SP is set:
┌─────────────────────────────┐ ← init_task.stack + THREAD_SIZE   (TOP, high addr)
│   pt_regs (336 bytes)       │
│   ├── [S_STACKFRAME]        │ ← stackframe.fp / stackframe.lr
│   ├── [S_STACKFRAME_TYPE]   │ ← FRAME_META_TYPE_FINAL
│   └── ... other regs ...    │
├─────────────────────────────┘ ← SP  (x29 will point into here)
│   (free stack space)        │
│                             │
└─────────────────────────────┘ ← init_task.stack  (BOTTOM, low addr)
```

```asm
// 4. Stamp the FINAL stack frame marker
stp   xzr, xzr, [sp, #S_STACKFRAME]        // stackframe.fp = 0, stackframe.lr = 0
mov   x5, #FRAME_META_TYPE_FINAL
str   x5,  [sp, #S_STACKFRAME_TYPE]         // Mark as end-of-stack for unwinder
add   x29, sp, #S_STACKFRAME                // FP → stackframe (start of frame chain)
```

The `S_STACKFRAME` offset sits inside `pt_regs`. Setting `fp=0, lr=0` signals to the stack unwinder: **"stop here, this is the bottom of all call stacks"**. `x29` (frame pointer) now points to it.

```asm
// 5. Shadow Call Stack (if CONFIG_SHADOW_CALL_STACK)
scs_load_current   // x18 = init_task.thread_info.scs_sp (SCS pointer)
```

SCS is a **separate shadow stack** stored in `task_struct`, used to protect return addresses from ROP attacks.

```asm
// 6. Set per-CPU offset into tpidr_el1
adr_l  x5, __per_cpu_offset           // x5 = &__per_cpu_offset[0]
ldr    w6, [x4, #TSK_TI_CPU]          // w6 = init_task.thread_info.cpu  (= 0)
ldr    x5, [x5, x6, lsl #3]           // x5 = __per_cpu_offset[0]
set_this_cpu_offset  x5               // msr tpidr_el1, x5
```

`tpidr_el1` is the **per-CPU base register**. Every per-cpu variable access (`this_cpu_read`, `per_cpu`) uses this register offset. Boot CPU is CPU 0, so `__per_cpu_offset[0]` is loaded.

---

## Block 2 — Exception Vector Table

```asm
adr_l  x8, vectors      // x8 = virtual address of exception vector table
msr    vbar_el1, x8     // VBAR_EL1 = &vectors
isb                     // Instruction Sync Barrier — flush pipeline, make VBAR live
```

**Memory significance**: `vectors` is a 2KB-aligned (ARMv8 requirement) block in the kernel `.text` section. Until this `msr`, **any exception would go to address 0** (or whatever garbage is in VBAR_EL1). The `isb` ensures no speculative instructions after this use the old VBAR value.

---

## Block 3 — Push a C-callable Stack Frame

```asm
stp   x29, x30, [sp, #-16]!   // push {fp, lr} — allocate 16 bytes on stack
mov   x29, sp                  // update frame pointer
```

This is standard AArch64 **AAPCS64** function prologue. `x30` holds the return address to `__primary_switch` (which will never be used, since `start_kernel` never returns, but the unwinder needs it).

Stack now:
```
┌──────────────────────────┐ ← old SP (pt_regs boundary)
│  [x29] saved frame ptr   │ ← SP-16
│  [x30] return address    │ ← SP-8
└──────────────────────────┘ ← SP (new, after stp!)
```

---

## Block 4 — Save FDT Pointer

```asm
str_l  x21, __fdt_pointer, x5    // __fdt_pointer = x21 (FDT physical address)
```

`str_l` expands to:
```asm
adrp  x5, __fdt_pointer
str   x21, [x5, :lo12:__fdt_pointer]
```

This writes the FDT physical address (carried in `x21` all the way from `primary_entry`) into the global `__fdt_pointer` variable (in `.init.data`). Later, `setup_machine_fdt(__fdt_pointer)` in C will use this to parse the device tree.

---

## Block 5 — `kimage_voffset`: The Virtual-to-Physical Rosetta Stone

```asm
adrp  x4, _text          // x4 = virtual address of kernel text start
sub   x4, x4, x0         // x4 = virt(_text) - phys(KERNEL_START)
str_l x4, kimage_voffset, x5
```

This computes and stores the most critical value in the kernel:

$$\texttt{kimage\_voffset} = \text{VA}(\texttt{\_text}) - \text{PA}(\texttt{KERNEL\_START})$$

**Why it matters**:
- `x0 = __pa(KERNEL_START)` was passed from `__primary_switch` via `adrp x0, KERNEL_START` — this gives a **physical** address because `adrp` is PC-relative and PC was in the identity-mapped region at that point.
- `adrp x4, _text` executes NOW with MMU ON and PC in virtual space — gives a **virtual** address.
- The difference is the V→P offset used everywhere:

```c
// In include/asm/memory.h:
#define __kimg_to_phys(addr)   ((addr) - kimage_voffset)
#define __phys_to_kimg(x)      ((unsigned long)((x) + kimage_voffset))
```

With KASLR, this offset is NOT the same as the linker-assigned offset — it reflects the **runtime randomized** placement. This single value enables all `virt_to_phys` / `phys_to_virt` conversions in the kernel.

```
Physical RAM:                     Virtual (TTBR1):
┌──────────────────┐              ┌──────────────────────────────┐
│ 0x40080000       │              │ 0xFFFFFF8010080000           │
│ KERNEL_START     │◄─────────────│ _text                        │
│  (physical)      │  kimage_voffset = 0xFFFF_FF7F_F007_8000     │
└──────────────────┘              └──────────────────────────────┘
```

---

## Block 6 — Boot Mode, KASAN, VHE

```asm
mov   x0, x20
bl    set_cpu_boot_mode_flag     // Write EL1/EL2 mode to __boot_cpu_mode
```

```asm
bl    kasan_early_init           // (if KASAN) Map the KASAN shadow memory region
```

KASAN requires mapping a **shadow memory region** — typically 1/8th of the kernel virtual address space — before any C code runs. `kasan_early_init` sets up those page table entries.

```asm
mov   x0, x20
bl    finalise_el2               // Negotiate VHE (Virtualization Host Extensions)
```

If VHE is available (`HCR_EL2.E2H=1`), promotes EL1 kernel to run "at EL2" transparently — improves KVM performance by eliminating EL1↔EL2 world switches.

---

## Block 7 — The Point of No Return

```asm
ldp   x29, x30, [sp], #16   // restore frame (but x30 = __primary_switch, never used)
bl    start_kernel           // ← JUMP INTO C KERNEL
ASM_BUG()                   // Should never reach here — panic if start_kernel returns
```

`start_kernel()` is in main.c. The stack, `sp_el0` (current), VBAR, per-cpu offset, FDT pointer, and `kimage_voffset` are all now set up. The kernel is fully operational in virtual memory.

---

## Complete Memory State Transition Summary

| Checkpoint | SP points to | sp_el0 | VBAR_EL1 | tpidr_el1 | kimage_voffset |
|---|---|---|---|---|---|
| Entry to `__primary_switched` | **garbage** (boot stack) | 0 | 0 | 0 | 0 |
| After `init_cpu_task` | `init_stack` top − `pt_regs` | `&init_task` | 0 | `__per_cpu_offset[0]` | 0 |
| After `msr vbar_el1` | same | same | `&vectors` | same | 0 |
| After `kimage_voffset` store | same | same | same | same | **virt−phys delta** |
| `bl start_kernel` | same | same | same | same | **set** |

---

## Key Interview Points for Nvidia

| Concept | Detail |
|---|---|
| **`sp_el0` = `current`** | ARM64 kernel repurposes `sp_el0` as the current `task_struct *` pointer, read by `mrs rd, sp_el0` |
| **Stack layout** | Kernel stack = `THREAD_SIZE` (16KB), grows down; `pt_regs` always at top, `thread_info` always at bottom |
| **Final frame marker** | `fp=0, lr=0` at `pt_regs.stackframe` terminates the unwinder chain — every task's stack has this |
| **`kimage_voffset`** | Computed as `VA(_text) − PA(KERNEL_START)` — the single value bridging virtual↔physical for the entire kernel; critical for KASLR |
| **`tpidr_el1`** | Per-CPU base register — loaded here with `__per_cpu_offset[0]`, enables `this_cpu_*` accessors |
| **VBAR before C code** | Exception vector table is wired up before the `stp`/stack frame push — any fault after `isb` goes to `vectors` |
| **`__fdt_pointer`** | Physical address of DTB, carried in `x21` since `primary_entry`, persisted to a `.init.data` global here |You've used 50% of your weekly rate limit. Your weekly rate limit will reset on April 27 at 5:30 AM. [Learn More](https://aka.ms/github-copilot-rate-limit-error)

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