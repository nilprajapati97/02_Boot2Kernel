# `trap_init()` — CPU Exception Handlers (IDT)

## Purpose

Installs the x86 Interrupt Descriptor Table (IDT) with handlers for all CPU exceptions (divide-by-zero, page fault, general protection fault, etc.) and system call gates. After this call, CPU exceptions produce proper kernel panic messages instead of silent triple-faults.

## Source File

`arch/x86/kernel/traps.c`

## Background: The IDT

The **Interrupt Descriptor Table** (IDT) is a 256-entry hardware table that the CPU indexes on every exception or interrupt. Each entry (a "gate descriptor") specifies:
- The handler entry point address
- Privilege level check (ring 0/3 access)
- Gate type (interrupt gate, trap gate, task gate)

```
IDT[0]  → divide_error handler
IDT[1]  → debug handler
IDT[2]  → NMI handler
IDT[3]  → int3 (breakpoint) handler
IDT[4]  → overflow handler
IDT[5]  → bounds handler
IDT[6]  → invalid_op (undefined instruction) handler
IDT[7]  → device_not_available (FPU) handler
IDT[8]  → double_fault handler
IDT[13] → general_protection handler
IDT[14] → page_fault handler
IDT[32+]→ IRQ / device interrupt handlers
IDT[128]→ ia32_syscall (int 0x80) — 32-bit syscall gate
```

## Key Exception Handlers

### Page Fault (#PF, vector 14)

The most important handler — fires every time the CPU accesses a page that is not present, not accessible, or with wrong permissions.

```
CPU detects page fault
    → saves state on stack
    → IDT[14] = asm_exc_page_fault
        → exc_page_fault()
            → handle_mm_fault()
                → On valid fault: allocate page, update PTE, return
                → On kernel fault: check exception table → fixup or panic
                → On user fault: send SIGSEGV
```

### General Protection Fault (#GP, vector 13)

Fires on privilege violations, segment violations, misaligned stack, etc.

### Double Fault (#DF, vector 8)

Fires when a fault occurs while handling a fault. Uses a dedicated IST (Interrupt Stack Table) stack to ensure it can always run even if the kernel stack is corrupt.

### System Call Gate

x86-64 uses `SYSCALL/SYSRET` instructions (not IDT), but 32-bit compatibility mode uses `int 0x80` which is an IDT entry with DPL=3 (user-accessible).

## x86 TSS and IST

For critical exceptions (NMI, Double Fault, Machine Check), the CPU switches to a dedicated "IST stack" defined in the TSS (Task State Segment):

```c
// In setup_tss_struct():
tss->ist[0] = (unsigned long)estacks->doublefault_stack;
tss->ist[1] = (unsigned long)estacks->nmi_stack;
tss->ist[2] = (unsigned long)estacks->debug_stack;
tss->ist[3] = (unsigned long)estacks->mce_stack;
```

This ensures these handlers can run even when the regular kernel stack is exhausted or corrupt.

## Pre-conditions

- Per-CPU areas must be set up (`setup_per_cpu_areas()`)
- Stack memory for IST stacks must be allocated

## Post-conditions

- IDT is loaded (`lidt` instruction executed)
- All CPU exception vectors have valid handlers
- Double-fault and NMI use dedicated IST stacks
- SIMD/FPU exception handling is configured

## Sub-topics

- [Exception vectors detail](exception_vectors/README.md)

## Cross-references

- [Phase overview](../README.md)
- `sort_main_extable()`: [../sort_main_extable/README.md](../sort_main_extable/README.md) — exception table used by page fault handler
