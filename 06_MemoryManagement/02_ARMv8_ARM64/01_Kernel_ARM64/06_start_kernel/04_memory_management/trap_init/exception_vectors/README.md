# Exception Vectors — x86 IDT Reference

## All 256 IDT Vectors

### CPU Exceptions (Intel-defined, vectors 0–31)

| Vector | Mnemonic | Name | Type | Handler |
|--------|----------|------|------|---------|
| 0 | #DE | Divide Error | Fault | `exc_divide_error` |
| 1 | #DB | Debug | Fault/Trap | `exc_debug` (IST) |
| 2 | — | NMI | Interrupt | `exc_nmi` (IST) |
| 3 | #BP | Breakpoint | Trap | `exc_int3` (DPL=3) |
| 4 | #OF | Overflow | Trap | `exc_overflow` |
| 5 | #BR | Bound Range | Fault | `exc_bounds` |
| 6 | #UD | Invalid Opcode | Fault | `exc_invalid_op` |
| 7 | #NM | Device Not Available | Fault | `exc_device_not_available` |
| 8 | #DF | Double Fault | Abort | `exc_double_fault` (IST) |
| 9 | — | Coprocessor Segment Overrun | Fault | `exc_coproc_segment_overrun` |
| 10 | #TS | Invalid TSS | Fault | `exc_invalid_tss` |
| 11 | #NP | Segment Not Present | Fault | `exc_segment_not_present` |
| 12 | #SS | Stack Fault | Fault | `exc_stack_segment` |
| 13 | #GP | General Protection | Fault | `exc_general_protection` |
| 14 | #PF | Page Fault | Fault | `exc_page_fault` |
| 15 | — | Reserved | — | — |
| 16 | #MF | FPU Math Fault | Fault | `exc_coprocessor_error` |
| 17 | #AC | Alignment Check | Fault | `exc_alignment_check` |
| 18 | #MC | Machine Check | Abort | `exc_machine_check` (IST) |
| 19 | #XM/#XF | SIMD Float Exception | Fault | `exc_simd_coprocessor_error` |
| 20 | #VE | Virtualization Exception | Fault | `exc_virtualization_exception` |
| 21 | #CP | Control Protection | Fault | `exc_control_protection` |
| 29 | #VC | VMM Communication | Fault | `exc_vmm_communication` (AMD SEV) |

### IST (Interrupt Stack Table) Usage

IST allows the CPU to switch to a dedicated stack **before** pushing exception frame, guaranteeing the handler can always run:

| Exception | IST# | Reason |
|-----------|------|--------|
| #DB Debug | IST 2 | Can occur on stack switch |
| NMI | IST 1 | Can interrupt any code including stack-unsafe code |
| #DF Double Fault | IST 0 | Stack is corrupt |
| #MC Machine Check | IST 3 | Hardware error, stack may be bad |

### System Call Vectors

| Vector | Name | Description |
|--------|------|-------------|
| 0x80 (128) | `ia32_syscall` | 32-bit `int 0x80` syscall |
| — | `entry_SYSCALL_64` | 64-bit `syscall` instruction (not IDT) |

### External Interrupts (vectors 32–255)

These are programmed by the APIC/PIC driver:
- Vectors 32–47: Legacy 8259A PIC IRQs (IRQ0=timer, IRQ1=keyboard, ...)
- Vectors 48+: APIC-assigned, device-specific
- Vector 0xFF (255): APIC spurious interrupt vector

## How a Fault Flows Through the IDT

```
1. CPU detects fault condition (e.g., page not present)
2. CPU checks privilege: ring 3 → ring 0 stack switch using TSS.RSP0
3. CPU pushes error frame on kernel stack:
       [SS, RSP, RFLAGS, CS, RIP, error_code]
4. CPU indexes IDT[vector] → loads handler segment:offset
5. If IST != 0: switch to IST stack first
6. Jump to handler (e.g., asm_exc_page_fault)
7. Handler pushes GP registers (SAVE_ALL)
8. Handler calls C function (exc_page_fault)
9. C function resolves fault or kills process
10. IRET restores RIP, CS, RFLAGS, RSP, SS
```

## Cross-references

- [trap_init parent](../README.md)
- [Phase overview](../../README.md)
