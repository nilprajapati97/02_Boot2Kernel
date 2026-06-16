# Exit Conditions Contract — What Is Guaranteed When `bl start_kernel` Executes

## The Formal Postcondition Set

When `bl start_kernel` executes, every item below is GUARANTEED to be true.
`start_kernel()` relies on ALL of these — it calls into subsystems that depend on
each one from its very first line of C code.

---

## Postcondition 1 — Valid Kernel Stack

**State:** SP points into `init_stack`, 16KB task stack of PID 0 (swapper).

```
SP value: init_task.stack + THREAD_SIZE - PT_REGS_SIZE - 16

Memory layout:
init_task.stack + 16384  ← top
  [pt_regs — 336 bytes, includes final frame sentinel]
  ← SP_after_init_cpu_task
  [stp frame — 16 bytes: saved x29, saved x30]
  ← SP_at_bl_start_kernel (current SP)
  [grows downward for start_kernel's own frames]
init_task.stack + 0      ← bottom (thread_info)
```

**Guaranteed by:** `init_cpu_task` sub-operation 2 (stack switch).

**Why it matters:** `start_kernel` is C code. Its very first instruction generates a
function prologue that pushes callee-saved registers onto the stack. If SP is invalid,
that push overwrites garbage memory → undefined behavior immediately.

---

## Postcondition 2 — `current` Task Is `init_task` (PID 0)

**State:** `sp_el0` = virtual address of `init_task`

**C access:** `current` macro → `mrs xN, sp_el0` → `(struct task_struct *)xN`

**`init_task` properties:**
- PID 0 (the "swapper" or "idle" process)
- Statically allocated in `.data` section at link time
- Has a valid `stack` pointer → `init_stack`
- Has `thread_info.cpu = 0` (boot CPU)
- Is the root of the task tree — `init_process.parent = &init_task`

**Guaranteed by:** `msr sp_el0, x4` in `init_cpu_task`.

**Why it matters:** `start_kernel` immediately calls things like `set_task_stack_end_magic(current)`
and `smp_setup_processor_id()` — both call `current`. If `current` returned garbage,
these would corrupt random kernel memory.

---

## Postcondition 3 — Exception Vectors Installed

**State:** `VBAR_EL1 = &vectors` (virtual address of exception vector table in `entry.S`)

**Guaranteed by:** `msr vbar_el1, x8` + `isb`

**Why it matters:**
- `start_kernel` enables interrupts (`local_irq_enable()`) inside `init_irq()`
- Before that, IRQs are disabled — but synchronous exceptions (page faults, illegal
  instructions, alignment faults) can still occur
- `setup_arch()` calls `early_trap_init()` which ALSO writes VBAR, but that's later —
  the initial VBAR installed here must survive until then

**If this were missing:** The very first page fault in `start_kernel` (e.g., during
`memblock_add()` when accessing a not-yet-mapped memblock region) would dispatch to
`VBAR_EL1 + offset` = garbage → crash.

---

## Postcondition 4 — Per-CPU Base Pointer in `tpidr_el1`

**State:** `tpidr_el1 = __per_cpu_offset[0]` (boot CPU's per-CPU section base)

**Guaranteed by:** `set_this_cpu_offset` in `init_cpu_task`.

**Why it matters:** `start_kernel` calls `smp_setup_processor_id()` which calls
`this_cpu_read()` which expands to:
```asm
mrs  x0, tpidr_el1           // get per-CPU base — if 0, this is garbage
ldr  x0, [x0, #var_offset]  // load variable — if base=0, reads wrong address
```
If `tpidr_el1 = 0`, every `this_cpu_read()` and `this_cpu_write()` operates on
address `0 + offset` — likely unmapped → immediate page fault.

---

## Postcondition 5 — `kimage_voffset` Is Computed

**State:** `kimage_voffset` global variable = `VA(_text) - PA(KERNEL_START)`

**Guaranteed by:** `adrp x4, _text` + `sub x4, x4, x0` + `str_l kimage_voffset`

**Why it matters:** The first C macro that accesses any physical↔virtual address
translation calls:
```c
#define __phys_to_virt(x)  ((unsigned long)((x) + kimage_voffset))
#define __virt_to_phys(x)  ((phys_addr_t)((x) - kimage_voffset))
```
`setup_arch()` → `setup_machine_fdt()` → `early_init_dt_scan_memory()` →
`memblock_add()` → `__phys_to_virt()` — this is called within the first ~50 lines
of `start_kernel`. If `kimage_voffset = 0`, all phys↔virt translations are wrong
and memblock adds memory at incorrect virtual addresses.

---

## Postcondition 6 — `__fdt_pointer` Is Set

**State:** `__fdt_pointer` = physical address of FDT blob

**Guaranteed by:** `str_l x21, __fdt_pointer, x5`

**Why it matters:** `setup_arch()` in `start_kernel` calls:
```c
setup_machine_fdt(__fdt_pointer);
```
This is the FIRST call in `setup_arch`. If `__fdt_pointer = 0`, then
`early_memremap(0, ...)` maps address 0 → likely maps page zero → reads garbage DTB
header → either `of_flat_dt_check_header()` rejects it with "No FDT" or reads wrong
memory sizes → catastrophic memory model misconfiguration.

---

## Postcondition 7 — `__boot_cpu_mode` Flag Set

**State:** `__boot_cpu_mode[0] = BOOT_CPU_MODE_EL1` OR `__boot_cpu_mode[1] = BOOT_CPU_MODE_EL2`

**Guaranteed by:** `bl set_cpu_boot_mode_flag`

**Why it matters:** `start_kernel` → `setup_arch` → `kvm_hyp_init()` queries:
```c
if (!is_hyp_mode_available()) {
    pr_info("KVM not supported\n");
    return -ENODEV;
}
```
`is_hyp_mode_available()` reads `__boot_cpu_mode`. If unset (random), KVM may
incorrectly enable or disable virtualization support.

---

## Postcondition 8 — KASAN Shadow Mapped (If Configured)

**State:** If `CONFIG_KASAN_GENERIC` or `CONFIG_KASAN_SW_TAGS`, the entire KASAN
shadow region is mapped in the kernel page tables.

**Guaranteed by:** `bl kasan_early_init` (conditional)

**Why it matters:** KASAN instruments every memory access at compile time:
```c
// Compiler inserts before every store/load:
shadow_byte = *(u8 *)((addr >> 3) + KASAN_SHADOW_OFFSET);
if (shadow_byte) kasan_report(addr, ...);
```
The very first C instruction in `start_kernel` is instrumented. If the shadow page
is not mapped, the shadow access causes a translation fault → panic before any
diagnostic infrastructure is available.

---

## Postcondition 9 — Exception Level Is Finalized

**State:** CPU is at final EL (EL1 or EL2/VHE) — will NOT change again during kernel operation.

**Guaranteed by:** `bl finalise_el2`

**Why it matters:** After `finalise_el2`, all system register accesses (`msr`/`mrs`)
target the correct EL. Under VHE, registers like `sctlr_el1` are aliased to `sctlr_el2`
by hardware. If `finalise_el2` ran AFTER `start_kernel` started writing sysregs, some
writes would target the wrong EL's registers → catastrophic hardware state.

---

## Complete Postcondition Table

| Resource | Value | Established By | First Consumer |
|---|---|---|---|
| SP (stack) | `init_stack` top minus PT_REGS | `init_cpu_task` | `start_kernel` prologue |
| `sp_el0` (current) | `&init_task` | `init_cpu_task` | `set_task_stack_end_magic(current)` |
| `VBAR_EL1` | `&vectors` | `msr vbar_el1` | First exception in `start_kernel` |
| `tpidr_el1` | `__per_cpu_offset[0]` | `set_this_cpu_offset` | `this_cpu_read()` calls |
| `kimage_voffset` | `VA(_text) - PA(start)` | `sub + str_l` | `__phys_to_virt()` in memblock |
| `__fdt_pointer` | FDT physical address | `str_l x21` | `setup_machine_fdt()` |
| `__boot_cpu_mode` | EL1/EL2 value | `set_cpu_boot_mode_flag` | `is_hyp_mode_available()` |
| KASAN shadow | Fully mapped | `kasan_early_init` | First instrumented memory access |
| Exception level | EL1 or EL2/VHE | `finalise_el2` | All sysreg accesses in C |
| Frame chain | Valid, terminates at `fp=0` | `init_cpu_task` | Stack unwinder (first oops) |
| Shadow call stack | `x18 = scs_sp` | `scs_load_current` | Every function return |

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