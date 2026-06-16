# CPACR_EL1 — FP/SIMD/SVE Access and Traps

**Category**: ARM64 Architecture Fundamentals  
**Targeted**: ARM, NVIDIA, Qualcomm

---

## 1. Concept Foundation

`CPACR_EL1` (Coprocessor Access Control Register, EL1) controls whether floating-point (FP), SIMD (Advanced SIMD / Neon), and SVE (Scalable Vector Extension) instructions can be executed at EL0 (user) and EL1 (kernel).

Linux uses a **lazy FPU context switching** strategy:
1. On a context switch, the kernel does NOT immediately save/restore FP/SIMD registers.
2. Instead, it disables FP/SIMD access for the new task by writing `CPACR_EL1`.
3. When the new task executes an FP instruction, a trap fires to EL1.
4. The kernel's trap handler then saves the old task's FP state (if needed) and restores the new task's FP state — only then enables access.

This saves the save/restore overhead for tasks that never use FP/SIMD.

---

## 2. CPACR_EL1 Bit Field Layout

```
Bits [63:22]  — Reserved (RES0)

Bit[21]  TTA   — Trace Trap (0=trace access from EL0/EL1 not trapped, 1=trapped to EL1)
Bits[21:20]    — RES0
Bits[21:20]  (Note: some sources show bit21=TTA, bit20=reserved)

Bit[19:18]  ZEN  — SVE access control (ARMv8.2 FEAT_SVE):
                   0b00 = SVE trapped at EL0 and EL1 (UNDEF at EL0/EL1)
                   0b01 = SVE at EL0 trapped to EL1, SVE at EL1 OK
                   0b10 = (reserved)
                   0b11 = SVE allowed at EL0 and EL1 (no trap)

Bit[17:16]  FPEN — FP/SIMD access control:
                   0b00 = FP/SIMD trapped at EL0 and EL1
                   0b01 = FP/SIMD at EL0 trapped to EL1, EL1 OK
                   0b10 = (reserved)
                   0b11 = FP/SIMD allowed at EL0 and EL1 (no trap)

Bits[15:0]  — Reserved (RES0)
```

---

## 3. FPEN — FP/SIMD Control

### FPEN=0b00: All FP/SIMD Trapped

Any FP/SIMD instruction (FADD, VMUL, LDP Q0, etc.) at EL0 or EL1 generates a Synchronous Exception:

```
ESR_EL1.EC = 0x07  (FP/SIMD access trapped)
```

This is used during: kernel initialization (before FP context is set up), after context switch (lazy switching), and when the kernel wants to prevent FP use.

### FPEN=0b01: EL0 Trapped, EL1 Allowed

EL1 kernel code can use FP/SIMD normally. EL0 user code traps. This is the intermediate state used during context switch to allow the kernel's trap handler to run.

### FPEN=0b11: Both EL0 and EL1 Allowed

Normal state for a task that has its FP state loaded. Both user and kernel FP/SIMD instructions execute without trapping.

---

## 4. Linux Lazy FPU Context Switching

### Core Concept

```
Task A (has FP state loaded):
  CPACR_EL1.FPEN = 0b11  → FP works normally

context_switch(A → B):
  // Do NOT save A's FP regs to memory yet
  // Just disable FP for the new context
  CPACR_EL1.FPEN = 0b00  → FP trapped for task B

Task B executes:
  If B never uses FP: no overhead at all — FPEN stays 0b00
  If B executes FP instruction:
    → Trap fires to EL1 (FPSIMD exception)
    → Kernel saves A's FP state (if A's state is "live" in registers)
    → Kernel loads B's FP state from memory
    → Sets FPEN=0b11
    → Returns to B — re-executes the faulting FP instruction
```

### Key Data Structures

```c
// arch/arm64/include/asm/fpsimd.h
struct user_fpsimd_state {
    __uint128_t vregs[32];   // Q0-Q31 (128-bit SIMD registers)
    __u32       fpsr;         // Floating-point Status Register
    __u32       fpcr;         // Floating-point Control Register
    // SVE follows (if SVE enabled):
    // sve_state: variable-length SVE register file
};

// Per-CPU live FP state tracking:
// arch/arm64/kernel/fpsimd.c
static DEFINE_PER_CPU(struct task_struct *, fpsimd_last_state);
// Which task's FP state is currently "live" in the hardware registers.
```

### Trap Handler — fpsimd_exception_handler

```c
// arch/arm64/kernel/fpsimd.c
void do_fpsimd_exc(unsigned long esr, struct pt_regs *regs)
{
    // 1. Check if this is a genuine FP access trap (EC=0x07)
    
    // 2. Is current task's FP state loaded?
    if (test_thread_flag(TIF_FOREIGN_FPSTATE)) {
        // State not loaded — restore from thread_struct
        fpsimd_restore_current_state();
        // fpsimd_restore_current_state():
        //   Reads current->thread.uw.fpsimd_state
        //   Loads Q0-Q31, FPSR, FPCR into hardware
        //   Sets fpsimd_last_state[cpu] = current
        //   Clears TIF_FOREIGN_FPSTATE
    }
    
    // 3. Enable FP for this task
    local_irq_disable();
    update_cpacr(CPACR_EL1_FPEN_EL0EN | CPACR_EL1_FPEN_EL1EN);
    local_irq_enable();
    
    // 4. Return — kernel re-executes the faulting instruction
}
```

### Context Switch FP Handling

```c
// arch/arm64/kernel/fpsimd.c
void fpsimd_thread_switch(struct task_struct *next)
{
    bool wrong_task, wrong_cpu;
    
    wrong_task = __this_cpu_read(fpsimd_last_state) != next;
    wrong_cpu  = next->thread.fpsimd_cpu != smp_processor_id();
    
    if (wrong_task || wrong_cpu) {
        // next's FP state is not in hardware registers
        set_tsk_thread_flag(next, TIF_FOREIGN_FPSTATE);
    }
    // Don't actually load FP regs yet — let lazy trap do it
}

// Also: when prev was using FP, is its state preserved?
// The hardware FP registers still hold prev's values.
// fpsimd_last_state tracks this.
// Only when some OTHER task needs FP do we evict prev's state.
```

---

## 5. Kernel FP Usage — kernel_neon_begin/end

The Linux kernel itself can use SIMD/NEON instructions (e.g., for crypto, CRC, memcpy optimization). But kernel use requires careful management:

```c
// arch/arm64/kernel/fpsimd.c
void kernel_neon_begin(void)
{
    // Disable preemption (FP state must not be context-switched)
    preempt_disable();
    
    // Save current user FP state if it was live in registers
    if (!test_thread_flag(TIF_FOREIGN_FPSTATE)) {
        // Save current task's FP state to thread_struct
        fpsimd_save_state(&current->thread.uw.fpsimd_state);
        set_thread_flag(TIF_FOREIGN_FPSTATE);
    }
    
    // Enable FP at EL1 (for kernel use)
    sysreg_clear_set(cpacr_el1, 0, CPACR_EL1_FPEN_EL1EN);
    isb();
}

void kernel_neon_end(void)
{
    // Disable kernel FP access again
    sysreg_clear_set(cpacr_el1, CPACR_EL1_FPEN_EL1EN, 0);
    isb();
    
    preempt_enable();
}
```

Why `preempt_disable()`? If the kernel is preempted while using FP registers, the scheduler would switch to a new task. The new task might try to load its own FP state, corrupting the kernel's mid-operation FP registers. By disabling preemption, the kernel FP operation runs atomically from a scheduling perspective.

---

## 6. SVE (Scalable Vector Extension) — ZEN Field

ARMv8.2 introduces SVE with variable-length vectors (128–2048 bits). `CPACR_EL1.ZEN` mirrors `FPEN` semantics but for SVE:

```
ZEN=0b00: SVE trapped at EL0 and EL1
ZEN=0b01: SVE at EL0 trapped, EL1 OK
ZEN=0b11: SVE allowed at EL0 and EL1
```

SVE state is much larger than FP/SIMD:
- 32 Z registers (each up to 2048 bits wide = 256 bytes max)
- 16 P predicate registers (each up to 256 bits)
- 1 FFR (First Fault Register)
- ZCRRR_EL1 controls vector length

```c
// arch/arm64/kernel/fpsimd.c
void sve_kernel_enable(const struct arm64_cpu_capabilities *__unused)
{
    // Enable SVE at EL1 kernel boot time
    write_sysreg(read_sysreg(cpacr_el1) | CPACR_EL1_ZEN_EL1EN, cpacr_el1);
    isb();
}

// SVE context size depends on VL (vector length):
static int sve_context_size(unsigned int vl)
{
    return SVE_SIG_REGS_SIZE(sve_vq_from_vl(vl));
    // = 32 * vq * 16  (Z regs) + 16 * vq * 2 + 2 (P regs + FFR)
}
```

---

## 7. CPTR_EL2 — Hypervisor FP/SIMD/SVE Trap Control

At EL2, the equivalent register is `CPTR_EL2`:

```
CPTR_EL2.TFP   (bit 10): Trap all FP/SIMD at EL0/EL1 to EL2
CPTR_EL2.TZ    (bit 8):  Trap SVE at EL0/EL1 to EL2 (ARMv8.2)
CPTR_EL2.TCPAC (bit 31): Trap CPACR_EL1 writes from EL1 to EL2
```

KVM uses `CPTR_EL2.TFP=0` to allow guests to use FP/SIMD without trapping to EL2. If a guest's FP context needs to be virtualized (e.g., for a 32-bit guest), `TFP=1` is used to intercept and handle in software.

---

## 8. SME — Streaming SVE Mode (ARMv9)

ARMv9 adds SME (Scalable Matrix Extension). `CPACR_EL1.SMEN` (bits 25:24) controls streaming SVE mode access:
```
SMEN=0b11: SME allowed at EL0 and EL1
SMEN=0b01: SME at EL0 trapped to EL1, EL1 allowed
SMEN=0b00: SME trapped at EL0 and EL1
```
Linux 6.x adds SME lazy context saving with SMCR_EL1, ZA array storage, etc.

---

## 9. Interview Questions & Answers

**Q1: What is lazy FPU context switching and why does ARM64 Linux use it?**

Lazy FPU context switching defers saving/restoring FP/SIMD registers until they are actually needed. On context switch, Linux sets `CPACR_EL1.FPEN=0b00` to disable FP for the new task. If the task never uses FP, no register save/restore happens. If it does use FP, a trap fires, and the kernel handles the state switch at that point. This optimization avoids the overhead of saving 32 × 128-bit Q registers (512 bytes) on every context switch, even for tasks that are purely integer-based.

**Q2: What happens when the kernel needs to use SIMD for crypto operations?**

The kernel calls `kernel_neon_begin()`, which: (1) disables preemption to prevent scheduling while SIMD registers are in use; (2) saves the current task's SIMD state if it was live in registers; (3) enables SIMD at EL1 via `CPACR_EL1.FPEN=0b01` or `0b11`. After the SIMD operation, `kernel_neon_end()` disables SIMD and re-enables preemption. This ensures kernel SIMD use doesn't corrupt user task state.

**Q3: What ESR value does an FP trap generate?**

`ESR_EL1.EC = 0x07` for a trapped FP/SIMD access. `ESR_EL1.EC = 0x19` for an SVE access trap. `ESR_EL1.ISS` in the FP case is typically 0 (the ISS is mostly RES0 for this exception class). The kernel's exception handler dispatches based on EC.

**Q4: Can the kernel disable FP access for specific user threads?**

Yes, by setting `CPACR_EL1.FPEN=0b01`. In this state, user (EL0) FP traps to EL1, but EL1 kernel code can still use FP. Normally the kernel would then handle the trap and load the user's FP state (`0b11`). But if the kernel wanted to deny FP to a specific thread (e.g., for sandboxing), it could keep FPEN at `0b01` and SIGSEGV/SIGILL the process.

**Q5: Why is CPACR_EL1 involved in security?**

If a user process could bypass `CPACR_EL1.FPEN=0` (e.g., by directly writing `CPACR_EL1`), it could re-enable FP and potentially read FP registers left by another process (information leak). ARM64 prevents this: `CPACR_EL1` is only writable at EL1. EL0 writing to it generates a trap. The kernel controls the FP state boundaries. This prevents one user process from accessing another's FP register values.

---

## 10. Quick Reference

| Field | Bits | Value | Meaning |
|---|---|---|---|
| FPEN | [17:16] | 0b00 | FP trapped EL0+EL1 |
| FPEN | [17:16] | 0b01 | FP EL0 trapped, EL1 OK |
| FPEN | [17:16] | 0b11 | FP allowed EL0+EL1 |
| ZEN  | [19:18] | 0b00 | SVE trapped EL0+EL1 |
| ZEN  | [19:18] | 0b01 | SVE EL0 trapped, EL1 OK |
| ZEN  | [19:18] | 0b11 | SVE allowed EL0+EL1 |
| TTA  | [28]   | 1 | Trace registers trapped to EL1 |

| Function | Purpose |
|---|---|
| `kernel_neon_begin()` | Enable kernel SIMD, save user state |
| `kernel_neon_end()` | Disable kernel SIMD, re-enable preemption |
| `fpsimd_restore_current_state()` | Load current task's FP state into hardware |
| `fpsimd_thread_switch()` | Mark FP state as foreign on context switch |
| `do_fpsimd_exc()` | FP trap handler — loads FP state lazily |
