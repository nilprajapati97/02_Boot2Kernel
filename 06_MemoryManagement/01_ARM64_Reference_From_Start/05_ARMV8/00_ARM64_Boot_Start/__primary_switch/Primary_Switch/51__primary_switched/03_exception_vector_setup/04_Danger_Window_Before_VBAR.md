# Danger Window — Exception Handling Before and After VBAR Setup

## Three Phases of VBAR in Linux Boot

### Phase 1: Before `__primary_switched` (firmware VBAR)

During early boot (before MMU on, during `__primary_switch`):
- `VBAR_EL1` points to firmware exception vectors (or is undefined)
- Linux does NOT install its own vectors yet
- Exceptions are FATAL — they vector to firmware which doesn't know about Linux

The kernel prevents this by:
1. Running with `PSTATE.DAIF = 1111` (all exceptions masked)
2. Avoiding any code that could trigger a synchronous exception (no memory faults,
   no undefined instructions, no division by zero)

### Phase 2: In `__primary_switched` Before `msr vbar_el1`

```asm
SYM_FUNC_START_LOCAL(__primary_switched)
    adr_l   x4, init_task
    init_cpu_task x4, x5, x6        // ← VBAR still points to firmware

    adr_l   x8, vectors             // ← DANGER WINDOW START
    msr     vbar_el1, x8            //
    isb                             // ← DANGER WINDOW END
```

During `init_cpu_task` and the `adr_l + msr + isb` sequence:
- An exception would vector to FIRMWARE (wrong)
- BUT: exceptions are still masked (PSTATE.DAIF = 1111 from earlier)
- Synchronous exceptions (page faults, alignment faults) CAN occur even with DAIF=1

If an alignment fault occurred during `stp xzr, xzr, [sp, #S_STACKFRAME]` in
`init_cpu_task` (impossible — sp is aligned — but hypothetically):
- CPU vectors to firmware `VBAR_EL1 + 0x200` (sync exception from EL1)
- Firmware handler has no idea about Linux kernel state
- Catastrophic crash

**Protection:** The assembly code is carefully written to never trigger synchronous
exceptions. All memory accesses are properly aligned. `sp` is always valid.

### Phase 3: After `isb` Following `msr vbar_el1`

- `VBAR_EL1` = `&vectors` (the Linux exception vector table)
- All exceptions vector correctly to Linux handlers
- Linux handlers can now access `current`, `sp`, `tpidr_el1` — all valid
- IRQ masking (`PSTATE.I=1`) still in effect until `local_irq_enable()` in `start_kernel`

---

## The Precise DAIF State Through Boot

```
primary_entry:
    PSTATE.DAIF = ????  (firmware-controlled at start)
    
    bl init_kernel_el
        // init_kernel_el sets PSTATE for EL1 entry:
        mov_q x0, INIT_PSTATE_EL1    // = 0x3C5 = DAIF=1111, M=EL1t
        msr spsr_el1, x0
        eret                          // enter EL1 with DAIF=1111 (all masked)

After eret into __primary_switch:
    PSTATE.DAIF = 1111 (D=1, A=1, I=1, F=1) ← ALL MASKED

__primary_switched:
    init_cpu_task...      // DAIF=1111 still
    msr vbar_el1...       // DAIF=1111 still — safe during danger window
    isb                   // DAIF=1111 still

    bl start_kernel:
        setup_arch()...
        ...
        local_irq_enable():  // PSTATE.I = 0 ← IRQs now enabled
                             // VBAR_EL1 must be valid before this!
```

`local_irq_enable()` clears `PSTATE.I`, allowing hardware IRQs. By this point:
- `vbar_el1` = `&vectors` ✓ (set 100s of instructions earlier)
- Stack, current, tpidr_el1 all valid ✓
- GIC configured to route IRQs to this CPU ✓

---

## What If VBAR Was NOT Set Before `local_irq_enable`?

```
SCENARIO: vbar_el1 = firmware vector table, IRQ arrives during start_kernel

CPU jumps to firmware VBAR + 0x280 (IRQ handler)
    ↓
Firmware IRQ handler:
    1. Tries to read firmware interrupt controller registers
    2. Firmware interrupt controller was already taken over by Linux (GIC programmed)
    3. Firmware sees no pending interrupt (Linux already took it)
    4. Firmware returns via ERET
    5. Back in Linux start_kernel — interrupt not processed
    6. Linux IRQ subsystem never knew about the interrupt
    7. Device driver's ISR never called
    8. Timeout → hang or incorrect behavior
```

OR worse:
```
Firmware IRQ handler:
    1. Tries to use firmware's EL1 stack
    2. sp = Linux stack pointer (set by init_cpu_task)
    3. Firmware corrupts the Linux stack
    4. Kernel crashes
```

This is why VBAR MUST be set before any interrupt can arrive.

---

## ARMv8 CPU / Kernel / Memory Context

### CPU Perspective (ARMv8-A)
VBAR_EL1 (Vector Base Address Register, EL1) holds the base address of the EL1 exception vector table. When an exception is taken to EL1 (IRQ, FIQ, SError, Synchronous abort), the CPU computes the vector offset from the exception type and PSTATE.SP, adds it to VBAR_EL1, and jumps to the resulting address (this is the hardware branch, not a software branch). The vector table has 16 entries (4 types x 4 SP variants) each spaced 0x80 bytes apart. VBAR_EL1 must be aligned to a 2 KB boundary. With KASLR, VBAR_EL1 is randomized as part of the kernel image.

### Kernel Perspective (Linux ARM64)
Linux sets VBAR_EL1 in __primary_switched (arch/arm64/kernel/head.S) using:
  adr_l  x8, vectors          // load VA of the vectors table
  msr    vbar_el1, x8         // write to VBAR_EL1
  isb                          // synchronize
This is done after the MMU is enabled and the kernel VA is active. The 'vectors' symbol is defined in arch/arm64/kernel/entry.S and maps the 16 exception handlers. Until this point, any exception would take the CPU to whatever garbage is at VBAR_EL1 (undefined on reset), so the early boot path must not trigger any exceptions.

### Memory Perspective (ARMv8 Memory Model)
VBAR_EL1 stores a VA (in the kernel text mapping). The exception vector table at that VA is Normal Inner-Shareable Read-Only (from an architectural view; the Linux kernel maps it as read-only/execute). When an exception fires, the CPU reads VBAR_EL1, computes the target VA, and fetches the instruction at that VA via the TLB/I-cache. The vector table is part of the kernel text and benefits from I-cache warming; exception entry latency is minimized once the I-cache has been populated with the vector code.