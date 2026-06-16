# Banked Registers Per Exception Level

**Category**: ARM64 Architecture Fundamentals  
**Targeted**: ARM, Qualcomm

---

## 1. Concept Foundation

"Banked registers" refers to the fact that certain ARM64 system registers have **separate physical storage per Exception Level** — or per security state (Secure vs Non-Secure). When the processor switches between ELs or worlds, these registers automatically refer to a different physical register without any explicit save/restore by software.

This is fundamentally different from general-purpose registers (X0–X30), which are shared across all ELs and must be saved/restored by software on exception entry/exit.

**Why banking matters**:
1. Each EL has its own stack pointer without conflicts.
2. Each EL has its own exception return address.
3. Secure world state is hidden from Normal world — even if a Secure EL1 register is read from Normal EL1, it reads the Non-Secure banked copy.
4. The kernel doesn't need to save/restore EL3 registers when handling an EL1 exception.

---

## 2. Registers Banked Per Exception Level

### 2.1 Stack Pointer (SP)

```
SP_EL0  —  EL0/EL1 (thread mode) stack pointer
SP_EL1  —  EL1 handler mode stack pointer
SP_EL2  —  EL2 stack pointer
SP_EL3  —  EL3 stack pointer

All 4 are independent 64-bit registers with separate physical storage.
When executing at EL1 with SPSel=1: 'SP' refers to SP_EL1
When executing at EL1 with SPSel=0: 'SP' refers to SP_EL0
```

### 2.2 Exception Link Register (ELR)

```
ELR_EL1  —  Saved PC when exception taken to EL1
ELR_EL2  —  Saved PC when exception taken to EL2
ELR_EL3  —  Saved PC when exception taken to EL3

Hardware automatically writes ELR_ELx when an exception is taken to ELx.
ERET reads ELR_ELx (where x = current EL) to restore the PC.
```

### 2.3 Saved Program Status Register (SPSR)

```
SPSR_EL1  —  Saved PSTATE when exception taken to EL1
SPSR_EL2  —  Saved PSTATE when exception taken to EL2
SPSR_EL3  —  Saved PSTATE when exception taken to EL3

Hardware automatically saves interrupted PSTATE to SPSR_ELx.
ERET restores PSTATE from SPSR_ELx.
```

### 2.4 Fault Address Register (FAR) and Exception Syndrome Register (ESR)

```
ESR_EL1  —  Exception syndrome (cause) for EL1 exceptions
ESR_EL2  —  Exception syndrome for EL2 exceptions
ESR_EL3  —  Exception syndrome for EL3 exceptions

FAR_EL1  —  Fault address for EL1 exceptions (data/instruction aborts)
FAR_EL2  —  Fault address for EL2 exceptions
FAR_EL3  —  Fault address for EL3 exceptions
```

These are written by hardware on exception entry and read by the exception handler.

### 2.5 Vector Base Address Register (VBAR)

```
VBAR_EL1  —  Exception vector table base for EL1
VBAR_EL2  —  Exception vector table base for EL2
VBAR_EL3  —  Exception vector table base for EL3

Each EL has its own vector table, set during initialization.
Linux: VBAR_EL1 = kernel 'vectors' symbol
KVM:   VBAR_EL2 = KVM hyp vector table
TF-A:  VBAR_EL3 = TF-A exception vectors
```

---

## 3. Virtual Memory System Registers Banked Per EL

### Translation Table Base Registers

```
TTBR0_EL1  —  User space page table base (EL1's TTBR0)
TTBR1_EL1  —  Kernel page table base (EL1's TTBR1)
TTBR0_EL2  —  EL2's page table base (hypervisor or VHE host)
TTBR1_EL2  —  VHE host kernel page table (VHE only)
TTBR0_EL3  —  EL3's page table base (TF-A)
VTTBR_EL2  —  Stage 2 page table base (guest VM)
```

### Translation Control Registers

```
TCR_EL1   —  Translation control for EL1
TCR_EL2   —  Translation control for EL2
TCR_EL3   —  Translation control for EL3
VTCR_EL2  —  Stage 2 translation control
```

### System Control Registers

```
SCTLR_EL1  —  EL1 system control (MMU, cache, alignment)
SCTLR_EL2  —  EL2 system control
SCTLR_EL3  —  EL3 system control

MAIR_EL1  —  Memory attribute indirection for EL1
MAIR_EL2  —  Memory attribute indirection for EL2
MAIR_EL3  —  Memory attribute indirection for EL3
```

---

## 4. Registers Banked Per Security State (Secure vs Non-Secure)

This is the banking for TrustZone. Many EL1 registers have separate physical copies for Secure and Non-Secure worlds:

```
TTBR0_EL1 [Non-Secure]  —  Normal world user page table base
TTBR0_EL1 [Secure]      —  Secure world user page table base
                             (Separate physical registers, selected by SCR_EL3.NS)

SCTLR_EL1 [Non-Secure]  —  Normal world system control
SCTLR_EL1 [Secure]      —  Secure world system control

TCR_EL1 [Non-Secure]    —  Normal world translation control
TCR_EL1 [Secure]        —  Secure world translation control

VBAR_EL1 [Non-Secure]   —  Normal world exception vectors (Linux)
VBAR_EL1 [Secure]       —  Secure world exception vectors (OP-TEE)

SP_EL0 [Non-Secure]     —  Normal world user stack pointer
SP_EL0 [Secure]         —  Secure world user stack pointer

SP_EL1 [Non-Secure]     —  Normal world kernel stack pointer
SP_EL1 [Secure]         —  Secure world kernel stack pointer
```

**Key implication**: TF-A must save the full Non-Secure EL1 state before switching to Secure world, and restore it when returning. This save/restore is done in software by TF-A's world switch code.

---

## 5. Thread ID Registers Banked Per EL

```
TPIDR_EL0   —  EL0 (user) thread pointer (TLS base for libc/pthreads)
TPIDRRO_EL0 —  EL0 read-only thread ID (set by kernel, read by user; e.g., CPU number)
TPIDR_EL1   —  EL1 (kernel) thread pointer (per-task kernel state)
TPIDR_EL2   —  EL2 thread pointer (hypervisor per-vCPU data)
TPIDR_EL3   —  EL3 thread pointer (TF-A per-CPU data)
```

In Linux:
- `TPIDR_EL0`: Set by `__clone()` / `set_thread_area()` for TLS. User applications read it directly with `MRS x0, TPIDR_EL0`.
- `TPIDRRO_EL0`: Set by the kernel to contain the CPU number (used by `raw_smp_processor_id()` in some implementations).
- `TPIDR_EL1`: Set during context switch to point to the current `task_struct` or per-CPU data.

```c
// arch/arm64/kernel/entry.S
// During context switch:
msr tpidr_el1, x23    // Save current task pointer in TPIDR_EL1
                       // Allows fast access to current task without memory load

// arch/arm64/include/asm/current.h
static __always_inline struct task_struct *get_current(void)
{
    unsigned long sp;
    asm ("mov %0, sp" : "=r" (sp));
    return (struct task_struct *)(sp & ~(THREAD_SIZE - 1));
    // OR alternatively via TPIDR_EL1:
    // asm ("mrs %0, tpidr_el1" : "=r" (tsk));
}
```

---

## 6. EL1 Register Banking Under a Hypervisor

When KVM runs a guest, the guest sees EL1 registers normally. But from the hypervisor's perspective, these registers are "virtual" — KVM can save/restore them independently per vCPU. The hardware only has one physical copy per EL (plus Secure/Non-Secure banking), so KVM must explicitly save the guest EL1 registers when exiting to the host and restore host EL1 registers:

```c
// arch/arm64/kvm/hyp/include/hyp/sysreg-sr.h
static inline void __sysreg_save_el1_state(struct kvm_cpu_context *ctxt)
{
    ctxt->sys_regs[SCTLR_EL1]   = read_sysreg_el1(SYS_SCTLR);
    ctxt->sys_regs[CPACR_EL1]   = read_sysreg_el1(SYS_CPACR);
    ctxt->sys_regs[TTBR0_EL1]   = read_sysreg_el1(SYS_TTBR0);
    ctxt->sys_regs[TTBR1_EL1]   = read_sysreg_el1(SYS_TTBR1);
    ctxt->sys_regs[ESR_EL1]     = read_sysreg_el1(SYS_ESR);
    ctxt->sys_regs[AFSR0_EL1]   = read_sysreg_el1(SYS_AFSR0);
    ctxt->sys_regs[AFSR1_EL1]   = read_sysreg_el1(SYS_AFSR1);
    ctxt->sys_regs[FAR_EL1]     = read_sysreg_el1(SYS_FAR);
    ctxt->sys_regs[MAIR_EL1]    = read_sysreg_el1(SYS_MAIR);
    ctxt->sys_regs[VBAR_EL1]    = read_sysreg_el1(SYS_VBAR);
    ctxt->sys_regs[CONTEXTIDR_EL1] = read_sysreg_el1(SYS_CONTEXTIDR);
    ctxt->sys_regs[AMAIR_EL1]   = read_sysreg_el1(SYS_AMAIR);
    ctxt->sys_regs[CNTKCTL_EL1] = read_sysreg_el1(SYS_CNTKCTL);
    ctxt->sys_regs[PAR_EL1]     = read_sysreg(par_el1);
    ctxt->sys_regs[TPIDR_EL1]   = read_sysreg(tpidr_el1);
}
```

This happens at every guest VM exit — it's a significant overhead cost.

---

## 7. Context Switch Register Saving in Linux

During a context switch (task A → task B), Linux saves and restores:

**Per-task saved registers** (in `struct cpu_context`):
```c
// arch/arm64/include/asm/processor.h
struct cpu_context {
    unsigned long x19;
    unsigned long x20;
    unsigned long x21;
    unsigned long x22;
    unsigned long x23;
    unsigned long x24;
    unsigned long x25;
    unsigned long x26;
    unsigned long x27;
    unsigned long x28;
    unsigned long fp;    // X29
    unsigned long sp;    // SP_EL1
    unsigned long pc;    // Return address (LR value at switch)
};
```

**Important**: Only callee-saved registers (X19–X28, FP, SP, PC) are saved in `cpu_context`. The kernel switch point in `cpu_switch_to()` is a function call, so caller-saved registers (X0–X18) are naturally saved/restored on the caller's stack.

**System registers per-task** (saved in `thread_struct`):
```c
struct thread_struct {
    struct cpu_context  cpu_context;    // callee-saved GPRs
    // FP/SIMD state
    struct user_fpsimd_state fpsimd_state;
    u64                 fpsimd_cpu;
    // SVE state
    void                *sve_state;
    unsigned int        sve_vl;
    // TLS
    unsigned long       uw_tpidr_tls;  // User TPIDR_EL0 value
    // ...
};
```

The actual system registers (TTBR0_EL1, CONTEXTIDR_EL1) are saved as part of the ASID/page table switch, not in `thread_struct` directly.

---

## 8. Interview Questions & Answers

**Q1: When a Synchronous Exception from EL1 is taken to EL1 (e.g., BRK instruction in kernel), what is saved in ELR_EL1?**

`ELR_EL1` is saved with the address of the `BRK` instruction itself (or the instruction after, depending on the exception type). For a `BRK #imm` instruction, `ELR_EL1 = address of BRK`. The debug handler can inspect the instruction encoding and resume execution after `ELR_EL1 + 4` by modifying `ELR_EL1` before `ERET`.

**Q2: Does hardware automatically save GP registers (X0–X30) on exception entry?**

No. Hardware only saves `PC → ELR_ELx` and `PSTATE → SPSR_ELx`. The kernel software (`kernel_entry` macro in `entry.S`) is responsible for saving X0–X30 to the kernel stack (`struct pt_regs`) immediately upon exception entry. This is why exception entry code is carefully written in assembly with no C function calls before the register save.

**Q3: If Linux calls EL3 (TF-A) via SMC, are any EL1 registers corrupted?**

Potentially yes, if TF-A is not careful. TF-A's SMC handler at EL3 might change EL1 registers if switching worlds (Secure/Non-Secure). TF-A is contractually obligated (by the SMCCC specification) to preserve all registers that are not explicitly documented as return values. In practice, TF-A saves/restores the full EL1 state during world switches. For fast-path SMC calls (PSCI, some debug), TF-A only preserves X0–X3 as per SMCCC.

**Q4: Why does Linux need TPIDR_EL1 if it already has the current task pointer via sp & ~(THREAD_SIZE-1)?**

`TPIDR_EL1` provides a **direct** register read to get the current task pointer, which is faster than the stack pointer masking approach on some pipelines. ARM64 Linux typically uses `sp & TASK_MASK` to find the task, but `TPIDR_EL1` is set during context switch to `current` and can be used in hot paths like `current_thread_info()` with a single `MRS` instruction.

**Q5: How many physically distinct TTBR0_EL1 registers exist on a TrustZone-capable ARM64 processor?**

At minimum 2 (Secure and Non-Secure versions). These are selected by `SCR_EL3.NS`. If the processor also implements EL2 (virtualization), there are additional `TTBR0_EL2` and `VTTBR_EL2`. Under VHE (`E2H=1`), `TTBR0_EL2` and `TTBR1_EL2` are the EL2 equivalents. So a fully-featured ARM64 processor has: `TTBR0_EL1[S]`, `TTBR0_EL1[NS]`, `TTBR1_EL1[S]`, `TTBR1_EL1[NS]`, `TTBR0_EL2`, `TTBR1_EL2`, `TTBR0_EL3`, `VTTBR_EL2` — eight translation table bases.

---

## 9. Quick Reference: Register Banking Summary

| Register | Banked Per | Physical Copies |
|---|---|---|
| SP_ELx | Per EL | 4 (SP_EL0, SP_EL1, SP_EL2, SP_EL3) |
| ELR_ELx | Per EL | 3 (ELR_EL1, ELR_EL2, ELR_EL3) |
| SPSR_ELx | Per EL | 3 (SPSR_EL1, SPSR_EL2, SPSR_EL3) |
| VBAR_ELx | Per EL | 3 (VBAR_EL1, VBAR_EL2, VBAR_EL3) |
| ESR_ELx | Per EL | 3 |
| FAR_ELx | Per EL | 3 |
| SCTLR_ELx | Per EL × Secure/NS | 6+ |
| TTBR0_ELx | Per EL × Secure/NS | 6+ |
| TCR_ELx | Per EL × Secure/NS | 6+ |
| MAIR_ELx | Per EL × Secure/NS | 6+ |
| TPIDR_ELx | Per EL | 4 |
