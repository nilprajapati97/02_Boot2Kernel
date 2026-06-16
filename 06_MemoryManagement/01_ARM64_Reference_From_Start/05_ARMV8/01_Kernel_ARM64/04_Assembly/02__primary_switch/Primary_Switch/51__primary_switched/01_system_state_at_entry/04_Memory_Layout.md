# Memory Layout at Entry — RAM, Virtual Space, Stack

## Physical RAM Map at Entry to `__primary_switched`

```
PHYSICAL ADDRESS SPACE
════════════════════════════════════════════════════════════════════

0x0000_0000  ────────────────────────────────────────
             │  ROM / SoC Boot ROM                  │ (read-only, vendor code)
0x0004_0000  ────────────────────────────────────────
             │  ATF (ARM Trusted Firmware) BL31     │ (secure world, EL3)
             │  Typically at 0x4000_0000 on Qualcomm │
0x4000_0000  ────────────────────────────────────────
             │  early_init_stack                    │ ← SP at entry to __primary_switched
             │  (4 pages = 16KB, part of .bss)      │   (physical, but accessed via VA)
             ────────────────────────────────────────
             │  __pi_init_idmap_pg_dir               │
             │  (identity page table — in .bss)     │
             ────────────────────────────────────────
0x4008_0000  ────────────────────────────────────────  ← KERNEL_START (example)
             │  Kernel Image                         │
             │  ├── .head.text (_head, primary_entry)│
             │  ├── .idmap.text                      │ ← contains __primary_switch
             │  ├── .text                            │ ← contains __primary_switched
             │  ├── .rodata                          │
             │  ├── .data                            │ ← contains init_task, boot_args
             │  ├── .bss                             │ ← contains init_stack, page tables
             │  └── .init.*                          │ ← contains __fdt_pointer, early allocs
             ────────────────────────────────────────
0x4800_0000  ────────────────────────────────────────  (example)
             │  DTB / FDT blob                      │ ← x21 points here
             │  (placed by bootloader)               │
             ────────────────────────────────────────
             │  (free RAM — for buddy allocator)     │
             ────────────────────────────────────────
0xFFFF_FFFF  ────────────────────────────────────────
```

---

## Kernel Virtual Address Space Map at Entry

```
VIRTUAL ADDRESS SPACE (48-bit, ARM64 default)
════════════════════════════════════════════════════════════════════

0x0000_0000_0000_0000  ──────────────────────────────────────────
                        │  User space (TTBR0 range)              │
                        │  Each process has its own page table    │
                        │  At boot: identity map (PA=VA)          │
0x0000_FFFF_FFFF_FFFF  ──────────────────────────────────────────
                        │  UNMAPPED (address hole)                │
                        │  VA[63:48] must be all-0 or all-1       │
0xFFFF_0000_0000_0000  ──────────────────────────────────────────
                        │  Kernel space (TTBR1 range)             │
                        │                                          │
                        │  [KASAN shadow — not mapped yet]         │
                        │  kasan_early_init will map this          │
                        │                                          │
0xFFFF_8000_0000_0000  ──────────────────────────────────────────
                        │  Linear map (physmem → VA)              │
                        │  All physical RAM accessible here        │
                        │                                          │
0xFFFF_FF80_0000_0000  ──────────────────────────────────────────
                        │  vmalloc / ioremap region               │
                        │  modules / kprobes text                 │
                        │                                          │
0xFFFF_FF80_1008_0000  ──────────────────────────────────────────  ← PAGE_OFFSET
                        │  Kernel image                           │
                        │  ├── .text (PC IS HERE at entry)        │ ← __primary_switched
                        │  ├── .rodata                            │
                        │  ├── .data                              │ ← init_task, boot_args
                        │  ├── .bss                               │ ← init_stack, page tables
                        │  └── .init.*                            │ ← __fdt_pointer
                        │                                          │
0xFFFF_FFFF_FFFF_FFFF  ──────────────────────────────────────────
```

---

## Stack Memory Layout at Entry

### early_init_stack (SP at entry — TEMPORARY)

```
early_init_stack (4 pages = 16384 bytes in .bss):

[stack_base + 16384]  ← top of stack
      │
      │ SP points here at entry to __primary_switched
      │ (set in __primary_switch: adrp x1,early_init_stack; mov sp,x1)
      │
[stack_base + 0]      ← bottom

Properties:
  - No thread_info at base — not associated with any task_struct
  - No final frame sentinel — unwinder cannot terminate cleanly
  - No SP_EL0 / current task — exception handlers cannot call current
  - Used ONLY for the call to __pi_early_map_kernel and the br x8 jump
  - ABANDONED by init_cpu_task which switches SP to init_stack
```

### init_stack (after init_cpu_task runs — PERMANENT for boot CPU)

```
init_stack (16384 bytes in .bss, part of init_task):

[init_task.stack + 16384]  ← top
      ─────────────────────────────────────── ← sub sp, sp, #PT_REGS_SIZE
      │  pt_regs [336 bytes]                 │
      │    ├── regs[0..30]   = 0             │ (not explicitly zeroed but in .bss)
      │    ├── sp            = ?             │
      │    ├── pc            = ?             │
      │    ├── pstate        = ?             │
      │    ├── stackframe.fp = 0 (xzr)      │ ← stp xzr,xzr sets this
      │    ├── stackframe.lr = 0 (xzr)      │
      │    └── stackframe_type = FINAL       │ ← str FRAME_META_TYPE_FINAL
      ─────────────────────────────────────── ← SP after init_cpu_task
      │                                      │
      │  Usable kernel stack                 │
      │  (grows downward as functions call)  │
      │                                      │
      ───────────────────────────────────────
      │  thread_info (at stack base)         │
      │    ├── flags                         │
      │    ├── addr_limit                    │
      │    └── ...                           │
[init_task.stack + 0]  ← bottom
```

---

## init_task Location in Virtual Space

```c
// kernel/init_task.c
struct task_struct init_task
__init_task_data = {
    .state   = 0,
    .stack   = init_stack,
    .pid     = 0,
    ...
};
```

`init_task` is in `.data` section (statically initialized, fixed VA at link time).
`init_stack` is in `.bss` (zero-initialized, fixed VA at link time).

Both are accessible via TTBR1 kernel page tables from the moment `__enable_mmu` runs.

**KASLR impact on `init_task` address:**
With KASLR, `_text` is at a random virtual base. All `.text`, `.data`, `.bss` symbols
are at random VAs (their offsets from `_text` are fixed, but the base changes).
`adr_l x4, init_task` in `__primary_switched` correctly computes the RANDOMIZED VA
of `init_task` because `adr_l` is PC-relative and the PC is in the randomized VA space.

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