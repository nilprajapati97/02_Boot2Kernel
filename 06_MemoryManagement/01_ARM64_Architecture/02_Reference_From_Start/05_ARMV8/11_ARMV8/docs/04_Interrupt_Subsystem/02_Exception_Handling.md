# Exception Handling for Interrupts

## 1. Interrupt Entry Flow

When an IRQ is delivered to a core, the hardware performs these steps automatically:

```
┌─────────────────────────────────────────────────────────────────┐
│  Hardware Steps (automatic on IRQ/FIQ):                          │
│                                                                   │
│  1. Check PSTATE.I (IRQ mask) or PSTATE.F (FIQ mask)            │
│     → If masked (=1): interrupt remains pending, no action       │
│     → If unmasked (=0): proceed                                  │
│                                                                   │
│  2. Save return state:                                           │
│     SPSR_EL1 = PSTATE          (save processor state)            │
│     ELR_EL1 = PC               (save return address)             │
│                                                                   │
│  3. Update PSTATE:                                               │
│     PSTATE.I = 1               (mask IRQs)                       │
│     PSTATE.F = 1               (mask FIQs)                       │
│     PSTATE.A = 1               (mask SError)                     │
│     PSTATE.D = 1               (mask debug)                      │
│     PSTATE.EL = 1              (target EL)                       │
│     PSTATE.SP = 1              (use SP_EL1)                      │
│                                                                   │
│  4. Jump to vector:                                              │
│     PC = VBAR_EL1 + offset                                       │
│     (offset depends on source EL and type)                       │
│                                                                   │
│  IRQ from EL0 (AArch64): VBAR_EL1 + 0x480                      │
│  IRQ from EL1 (SP_ELn):  VBAR_EL1 + 0x280                      │
└─────────────────────────────────────────────────────────────────┘
```

---

## 2. Full IRQ Handler Flow

```
┌─────────────────────────────────────────────────────────────────┐
│                  Complete IRQ Handler Flow                        │
│                                                                   │
│  1. Vector entry (VBAR_EL1 + offset)                             │
│     → Save all general-purpose registers on stack                │
│                                                                   │
│  2. Acknowledge interrupt                                        │
│     MRS X0, ICC_IAR1_EL1       // Read INTID (acknowledges it)  │
│     → INTID = interrupt number                                   │
│     → If INTID = 1023 → spurious, skip handling                  │
│                                                                   │
│  3. Optionally re-enable interrupts (for nesting)                │
│     MSR DAIFClr, #2            // Clear PSTATE.I to allow nesting│
│                                                                   │
│  4. Call device-specific handler                                 │
│     → Look up handler from interrupt descriptor table            │
│     → Execute handler (e.g., read UART, process network packet)  │
│     → Clear device interrupt source                              │
│                                                                   │
│  5. Mask interrupts again (if nested)                            │
│     MSR DAIFSet, #2            // Set PSTATE.I                   │
│                                                                   │
│  6. Signal End of Interrupt                                      │
│     MSR ICC_EOIR1_EL1, X0     // Write INTID to EOI register    │
│     → Priority drop: allows same-priority interrupts             │
│     → Deactivation: interrupt goes to Inactive state             │
│                                                                   │
│  7. Restore registers from stack                                 │
│                                                                   │
│  8. Exception return                                             │
│     ERET                       // Restore PSTATE from SPSR_EL1   │
│                                // Restore PC from ELR_EL1        │
│                                // Resume interrupted code         │
└─────────────────────────────────────────────────────────────────┘
```

### Assembly Example: IRQ Vector Handler

```asm
// Exception vector entry (at VBAR_EL1 + 0x280 for EL1 IRQ)
.align 7                          // 128-byte aligned
el1_irq:
    // Save context (all caller-saved registers + LR)
    STP X0,  X1,  [SP, #-16]!
    STP X2,  X3,  [SP, #-16]!
    STP X4,  X5,  [SP, #-16]!
    STP X6,  X7,  [SP, #-16]!
    STP X8,  X9,  [SP, #-16]!
    STP X10, X11, [SP, #-16]!
    STP X12, X13, [SP, #-16]!
    STP X14, X15, [SP, #-16]!
    STP X16, X17, [SP, #-16]!
    STP X18, X29, [SP, #-16]!
    STP X30, XZR, [SP, #-16]!

    // Acknowledge interrupt
    MRS X0, ICC_IAR1_EL1          // X0 = INTID
    
    // Check for spurious
    MOV X1, #1023
    CMP X0, X1
    B.EQ spurious_irq
    
    // Save INTID for EOI later
    MOV X19, X0
    
    // Call C handler: void handle_irq(uint32_t intid)
    BL  handle_irq
    
    // End of interrupt
    MSR ICC_EOIR1_EL1, X19

spurious_irq:
    // Restore context
    LDP X30, XZR, [SP], #16
    LDP X18, X29, [SP], #16
    LDP X16, X17, [SP], #16
    LDP X14, X15, [SP], #16
    LDP X12, X13, [SP], #16
    LDP X10, X11, [SP], #16
    LDP X8,  X9,  [SP], #16
    LDP X6,  X7,  [SP], #16
    LDP X4,  X5,  [SP], #16
    LDP X2,  X3,  [SP], #16
    LDP X0,  X1,  [SP], #16

    ERET                          // Return from exception
```

---

## 3. IRQ vs FIQ Routing

```
IRQ and FIQ are separate interrupt signals to each core:

┌──────────────────────────────────────────────────────────────┐
│  Signal │ Typical Use        │ Controlled by                │
├─────────┼────────────────────┼──────────────────────────────┤
│  IRQ    │ Normal interrupts  │ PSTATE.I masks it            │
│         │ (NS Group 1)       │ SCR_EL3.IRQ routes to EL3   │
│         │                     │ HCR_EL2.IMO routes to EL2   │
│                                                              │
│  FIQ    │ Secure / fast      │ PSTATE.F masks it            │
│         │ (Group 0 or S-G1)  │ SCR_EL3.FIQ routes to EL3   │
│         │                     │ HCR_EL2.FMO routes to EL2   │
└──────────────────────────────────────────────────────────────┘

Routing control:
  SCR_EL3.IRQ = 1 → All IRQs taken to EL3
  SCR_EL3.FIQ = 1 → All FIQs taken to EL3
  HCR_EL2.IMO = 1 → EL1/EL0 IRQs routed to EL2 (hypervisor)
  HCR_EL2.FMO = 1 → EL1/EL0 FIQs routed to EL2

Typical configuration:
  Secure firmware: FIQ at EL3 (SCR_EL3.FIQ=1)
  Hypervisor: IRQ at EL2 (HCR_EL2.IMO=1) for virtual interrupt inject
  Linux (no hypervisor): IRQ at EL1
```

---

## 4. Interrupt Masking

```
DAIF — Debug, SError, IRQ, FIQ mask bits in PSTATE:

  PSTATE.D = 1 → Debug exceptions masked
  PSTATE.A = 1 → SError (asynchronous abort) masked
  PSTATE.I = 1 → IRQ masked
  PSTATE.F = 1 → FIQ masked

Instructions:
  MSR DAIFSet, #imm      // Set (mask) selected DAIF bits
  MSR DAIFClr, #imm      // Clear (unmask) selected DAIF bits

  // imm is a 4-bit field: D=bit3, A=bit2, I=bit1, F=bit0
  
  MSR DAIFSet, #0xF      // Mask ALL (D+A+I+F)
  MSR DAIFClr, #0x2      // Unmask IRQ only (bit 1)
  MSR DAIFSet, #0x3      // Mask IRQ + FIQ (bits 1+0)

  MRS X0, DAIF           // Read current mask state

On exception entry: Hardware automatically sets D,A,I,F = 1 (all masked)
On ERET: PSTATE restored from SPSR (including DAIF bits)
```

---

## 5. SError (System Error)

```
SError is an asynchronous abort — caused by:
  • Uncorrectable memory errors (ECC failure)
  • Slave errors on bus transactions
  • External abort on page table walk
  • Poisoned data from cache

Properties:
  • Asynchronous: not tied to any specific instruction
  • Can be masked by PSTATE.A
  • Implementation-defined latency (may arrive much later)
  • ESR_EL1.EC = 0x2F for SError

RAS (Reliability, Availability, Serviceability) — ARMv8.2:
  • Adds structured error reporting
  • Error syndrome in ESR_ELx.ISS
  • Correctable (CE) vs Uncorrectable (UE) classification
  • Hardware error containment
```

---

## 6. Interrupt Nesting (Preemption)

```
Interrupt nesting allows higher-priority interrupts to preempt
lower-priority handlers:

  ┌──────────────────────────────────────────────────────────┐
  │  Without nesting:                                        │
  │    IRQ A (priority 10) arrives → handler starts          │
  │    IRQ B (priority 5, HIGHER) arrives → WAITS            │
  │    IRQ A handler finishes → EOI                          │
  │    IRQ B handler starts                                  │
  │    Total latency for B: handler_A_time + handler_B_time  │
  │                                                           │
  │  With nesting:                                            │
  │    IRQ A (priority 10) arrives → handler starts          │
  │    IRQ B (priority 5, HIGHER) arrives → PREEMPTS A       │
  │    IRQ B handler runs immediately                        │
  │    IRQ B finishes → EOI → resumes IRQ A handler          │
  │    Total latency for B: very low (immediate preemption)  │
  └──────────────────────────────────────────────────────────┘

  To enable nesting:
  1. In handler: Write IAR to acknowledge (priority drops)
  2. Re-enable IRQ: MSR DAIFClr, #2
  3. Now higher-priority IRQ can preempt
  4. Before EOI: MSR DAIFSet, #2 (mask IRQ again)
  5. Write EOIR
```

---

Next: [Interrupt Types & Routing →](./03_Interrupt_Types.md)
