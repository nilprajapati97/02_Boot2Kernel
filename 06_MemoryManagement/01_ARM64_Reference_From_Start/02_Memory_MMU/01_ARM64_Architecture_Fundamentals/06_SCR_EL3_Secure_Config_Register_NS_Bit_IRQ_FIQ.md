# SCR_EL3 — Secure Configuration Register: NS Bit and IRQ/FIQ Routing

**Category**: ARM64 Architecture Fundamentals  
**Targeted**: ARM, Qualcomm (TrustZone/Secure World roles)

---

## 1. Concept Foundation

`SCR_EL3` (Secure Configuration Register, EL3) is the master control register for TrustZone security configuration. It is only accessible from EL3 (ARM Trusted Firmware / TF-A), making it the root of trust for the entire security architecture.

`SCR_EL3` controls three fundamental aspects:
1. **World switching**: The `NS` bit determines whether the current execution context is in the Secure world or Normal (Non-Secure) world.
2. **Exception routing**: Which EL captures IRQ, FIQ, SError, and other exceptions.
3. **Execution state**: Whether EL2 and below operate in AArch64 or AArch32.

Everything above is software-observable only from EL3 — EL1 and EL2 cannot read or write `SCR_EL3`.

---

## 2. SCR_EL3 Bit Field Layout

```
Bits [63:21] — Reserved (FEAT_* extension bits in higher architectures)

Key bits:
Bit[20]  FIEN   — Fault Injection enable (ARMv8.4)
Bit[19]  Reserved
Bit[18]  EEL2   — Secure EL2 enable (ARMv8.4 FEAT_SEL2)
Bit[17]  API    — API instructions enable (PAC)
Bit[16]  APK    — APK instructions enable (PAC)
Bit[15]  TERR   — Trap Error record accesses
Bit[14]  TLOR   — Trap LOR (Limited Order Regions) registers
Bit[13]  TWE    — Trap WFE from EL0/EL1/EL2 to EL3
Bit[12]  TWI    — Trap WFI from EL0/EL1/EL2 to EL3
Bit[11]  ST     — Trap Secure EL1 access to timer registers
Bit[10]  RW     — Execution state for lower ELs (0=AArch32, 1=AArch64)
Bit[9]   SIF    — Secure Instruction Fetch (0=fetch from NS allowed, 1=forbidden)
Bit[8]   HCE    — HVC instruction enable (0=HVC disabled/UNDefined, 1=enabled)
Bit[7]   SMD    — Secure Monitor Call Disable (0=SMC enabled, 1=SMC from EL0/EL1=UNDEF)
Bit[6]   Reserved
Bit[5]   Reserved
Bit[4]   Reserved (was EA in earlier versions)
Bit[3]   FIQ    — Physical FIQ routing (0=not EL3, 1=FIQ taken to EL3)
Bit[2]   IRQ    — Physical IRQ routing (0=not EL3, 1=IRQ taken to EL3)
Bit[1]   Reserved (was IRQ in some older versions)
Bit[0]   NS     — Non-Secure bit (0=Secure world, 1=Non-Secure world)
```

---

## 3. The NS Bit — World Switching

### NS = 0: Secure World
- EL0 and EL1 operate in the **Secure** physical address space.
- Normal (Non-Secure) memory regions are inaccessible from Secure world unless specifically permitted.
- Secure world software: OP-TEE (Secure EL1), Secure user TAs (Secure EL0).
- TZASC (TrustZone Address Space Controller) enforces bus-level isolation.

### NS = 1: Non-Secure World
- EL0 and EL1 operate in the **Normal** (Non-Secure) physical address space.
- All Linux processes and the Linux kernel run here.
- Linux kernel cannot access Secure physical memory (access causes bus error or ignored).

### World Switch Mechanism

The `SCR_EL3.NS` bit is only written by EL3 code (TF-A). The world switch sequence:

```asm
// TF-A SMC handler: switching from Normal to Secure world
// Save Normal world state (EL1 registers: SCTLR, TCR, TTBR, etc.)
stp     x0, x1, [sp, #NORMAL_CTX_X0]
// ... save all EL1/EL2 registers

// Switch to Secure world
msr     SCR_EL3, xSecureValue    // Set NS=0
isb

// Restore Secure world EL1 state
ldp     x0, x1, [sp, #SECURE_CTX_X0]
// ... restore Secure EL1/EL0 registers

// ERET to Secure EL1 (OP-TEE)
eret
```

**Critical detail**: When `NS=0`, the EL1/EL0 system registers (`TTBR0_EL1`, `SCTLR_EL1`, etc.) are physically **banked** — there is a separate set of register values for Secure and Non-Secure worlds. TF-A is responsible for saving/restoring the full architectural state when switching worlds.

```
Physical register file view:
  TTBR0_EL1 (Secure)      ← accessed when NS=0
  TTBR0_EL1 (Non-Secure)  ← accessed when NS=1
  (Both exist simultaneously, selected by NS bit)
```

---

## 4. IRQ/FIQ Routing via SCR_EL3

### SCR_EL3.IRQ (Bit 2)

**IRQ=0** (default for Linux systems):
- IRQs are taken to EL1 (or EL2 if `HCR_EL2.IMO=1`).
- The Linux IRQ handler processes the interrupt.

**IRQ=1**:
- All physical IRQs are taken to EL3, regardless of the current EL.
- TF-A handles them at EL3.
- This is used when TF-A implements an interrupt controller driver at EL3 (rare).
- After handling, TF-A `ERET` back to the interrupted world.

### SCR_EL3.FIQ (Bit 3)

**FIQ=0**:
- FIQs can be taken to EL1 or EL2 depending on `HCR_EL2` configuration.

**FIQ=1** (most common configuration):
- All physical FIQs are taken to EL3.
- In ARM's reference implementation, **Secure interrupts are signaled as FIQ** to ensure they always reach TF-A (EL3), even when Linux is running.
- TF-A's FIQ handler at EL3 identifies the interrupt, switches to Secure world, and routes to OP-TEE at Secure EL1 if needed.

### Typical TrustZone IRQ/FIQ Setup

```
Normal World setup:
  SCR_EL3.NS  = 1    (running Normal world)
  SCR_EL3.IRQ = 0    (Non-Secure IRQs → EL1/EL2, handled by Linux GIC driver)
  SCR_EL3.FIQ = 1    (FIQs → EL3, allows Secure interrupts to preempt Linux)

GIC (Generic Interrupt Controller) configuration:
  Non-Secure interrupts: Group 1 → signaled as IRQ to Normal world
  Secure interrupts:     Group 0 → signaled as FIQ → EL3 → TF-A → OP-TEE
```

This means a Secure interrupt (e.g., a secure timer for OP-TEE) can preempt Linux at any time by generating a FIQ, which is routed to EL3 via `SCR_EL3.FIQ=1`.

---

## 5. SCR_EL3.RW — Lower EL Execution State

```
SCR_EL3.RW:
  0 = EL2 (and below) is AArch32
  1 = EL2 (and below) is AArch64
```

For a 64-bit Linux system, TF-A sets `SCR_EL3.RW=1` before `ERET`-ing to EL2 or EL1. This allows EL2 (KVM) and EL1 (Linux kernel) to run in AArch64.

---

## 6. SCR_EL3.HCE — Hypervisor Call Enable

```
HCE=0: HVC instruction at EL1 generates UNDEF exception (not trapped to EL2 or EL3)
HCE=1: HVC instruction routes to EL2 normally
```

TF-A sets `SCR_EL3.HCE=1` when a hypervisor is present (KVM). Without this, any `HVC` from Linux kernel code would fault with an Undefined Instruction exception.

---

## 7. SCR_EL3.SMD — Secure Monitor Call Disable

```
SMD=0: SMC from EL0/EL1 works normally (routed to EL3)
SMD=1: SMC from EL0 or EL1 generates UNDEF exception
```

If TF-A determines that the secure monitor services are no longer needed after boot (e.g., in a locked-down system), it can set `SMD=1` to prevent any further `SMC` calls. This reduces attack surface.

---

## 8. SCR_EL3.SIF — Secure Instruction Fetch

```
SIF=0: Secure EL0/EL1 can fetch instructions from Non-Secure memory
SIF=1: Instruction fetch by Secure EL0/EL1 from Non-Secure memory is forbidden (abort)
```

This prevents code injection attacks where Normal world memory is mapped into Secure world and then executed. With `SIF=1`, even if Secure world accidentally maps Non-Secure memory as executable, the CPU will abort the instruction fetch.

---

## 9. TF-A (ARM Trusted Firmware) and SCR_EL3

TF-A is the reference EL3 firmware. Its initialization sequence:

```c
// plat/arm/common/arm_bl31_setup.c (simplified)
void bl31_platform_setup(void)
{
    // Configure SCR_EL3 for secure boot handoff
    uint64_t scr = read_scr_el3();
    
    // Set NS=0 initially (we are in Secure world at EL3)
    scr &= ~SCR_NS_BIT;       // NS=0
    
    // Enable AArch64 for lower ELs
    scr |= SCR_RW_BIT;        // RW=1: EL2/EL1 = AArch64
    
    // Route FIQs to EL3 (for secure interrupt handling)
    scr |= SCR_FIQ_BIT;       // FIQ=1: FIQs → EL3
    
    // Enable HVC
    scr |= SCR_HCE_BIT;       // HCE=1
    
    write_scr_el3(scr);
    isb();
}

// Before ERET to Normal world (Linux):
void bl31_exit_to_normal_world(void)
{
    uint64_t scr = read_scr_el3();
    scr |= SCR_NS_BIT;         // NS=1: switch to Normal world
    write_scr_el3(scr);
    isb();
    eret();                    // → EL2 (KVM) or EL1 (Linux kernel)
}
```

---

## 10. Linux Kernel Interaction with SCR_EL3

Linux itself does NOT access `SCR_EL3` — it cannot (requires EL3 privilege). However, Linux interacts with TF-A services via `SMC`:

```c
// arch/arm64/kernel/psci.c — PSCI calls to TF-A
static noinline int cpu_psci_cpu_on(unsigned int cpu)
{
    int err = psci_ops.cpu_on(cpu_logical_map(cpu), __pa_symbol(secondary_entry));
    // This internally does:
    // SMC #0 with PSCI_CPU_ON in X0
    // → EL3 → TF-A handles → starts secondary CPU
}

// arch/arm64/kernel/smccc-call.S — SMC calling convention
SYM_FUNC_START(__arm_smccc_smc)
    smc #0        // Trigger SMC exception → EL3
    ret
SYM_FUNC_END(__arm_smccc_smc)
```

---

## 11. Interview Questions & Answers

**Q1: What is the NS bit and what does it control?**

`SCR_EL3.NS` is the Non-Secure bit. When `NS=0`, the processor is in the Secure world — EL0/EL1 have access to Secure physical memory, and system registers like `TTBR0_EL1` refer to the Secure-banked version. When `NS=1`, the processor is in the Normal (Non-Secure) world — Linux runs here. Physical memory protected by TZASC is inaccessible in this state. Only TF-A (EL3) can change this bit. The hardware banks many EL1/EL0 registers so both worlds have independent state.

**Q2: Why are FIQs typically routed to EL3?**

In ARM's TrustZone design, Secure interrupts (from Secure peripherals or OP-TEE) are signaled as FIQ in the GIC (Group 0 interrupts). By setting `SCR_EL3.FIQ=1`, FIQs are routed to EL3 (TF-A) regardless of what EL the CPU is currently executing at. TF-A can then switch to Secure world and dispatch the interrupt to OP-TEE. If FIQs were not routed to EL3, a Secure interrupt firing while Linux is running would go to EL1 (Linux kernel), which does not understand Secure interrupts.

**Q3: Can Linux modify SCR_EL3?**

No. `SCR_EL3` is accessible only at EL3. Any attempt by Linux (EL1) to write `SCR_EL3` via `MSR SCR_EL3, x0` would cause a Synchronous Exception to EL3. TF-A would handle this as an unsupported operation. Linux never attempts to access EL3-only registers.

**Q4: What happens when TF-A sets SCR_EL3.SMD=1?**

With `SMD=1`, any `SMC` instruction executed at EL0 or EL1 generates an Undefined Instruction exception (taken to EL1). The SMC is NOT forwarded to EL3. This is a security hardening measure used in systems where the secure monitor services are no longer needed after boot. Some embedded systems permanently set this after completing initialization to reduce attack surface.

**Q5: How does the NS bit interact with page tables?**

When `NS=1` (Normal world), the TTBR0_EL1 used by the MMU is the Non-Secure banked version, pointing to Normal-world page tables. Physical addresses generated by translation go through the TZASC: if the PA falls in a Secure region (as configured by TF-A), the bus access is rejected (error response). This is independent of the page table permissions — even if the PTE maps a VA to a Secure PA, the TZASC blocks the bus transaction.

---

## 12. Quick Reference

| Bit | Name | Value 0 | Value 1 |
|---|---|---|---|
| 0 | NS | Secure world | Normal (Non-Secure) world |
| 2 | IRQ | IRQ → EL1/EL2 | IRQ → EL3 |
| 3 | FIQ | FIQ → EL1/EL2 | FIQ → EL3 |
| 7 | SMD | SMC allowed | SMC from EL1=UNDEF |
| 8 | HCE | HVC=UNDEF | HVC enabled |
| 9 | SIF | Secure can fetch NS code | Secure cannot fetch NS code |
| 10 | RW | EL2 = AArch32 | EL2 = AArch64 |
