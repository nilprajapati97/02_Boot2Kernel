# System Registers: MSR/MRS Instructions vs ARM32's CP15 MCR/MRC

**Category**: ARM64 Architecture Fundamentals  
**Targeted**: ARM, Qualcomm

---

## 1. Concept Foundation

ARM64 (AArch64) replaces the ARM32 coprocessor register access mechanism with a unified, symbolic system register access model. This is a fundamental design improvement: instead of cryptic coprocessor encodings, each system register has a **named symbolic identifier** that the assembler or compiler resolves.

**Why this matters for kernel development**:
- Incorrect coprocessor encoding in ARM32 was a common source of bugs (silently writing to wrong register).
- AArch64 system register names are self-documenting (`SCTLR_EL1`, `TCR_EL1`, etc.).
- The access checking (what EL can access what register) is enforced by hardware based on the register's defined access permissions.

---

## 2. AArch64 System Register Access: MSR / MRS

### MRS — Move to Register from System register
```asm
MRS <Xt>, <systemreg>    // Read system register → Xt (64-bit)
```

### MSR — Move to System register from Register
```asm
MSR <systemreg>, <Xt>    // Write Xt → system register
```

### Examples
```asm
// Read the current EL1 System Control Register
mrs x0, SCTLR_EL1

// Enable MMU (bit 0), D-cache (bit 2), I-cache (bit 12)
orr x0, x0, #(1 << 0)   // M bit
orr x0, x0, #(1 << 2)   // C bit
orr x0, x0, #(1 << 12)  // I bit
msr SCTLR_EL1, x0

// Read Translation Control Register
mrs x1, TCR_EL1

// Read current exception level
mrs x2, CurrentEL
lsr x2, x2, #2           // EL in bits [3:2]

// Write ASID + page table base to TTBR0_EL1
msr TTBR0_EL1, x3
isb                        // Mandatory ISB after TTBR write
```

---

## 3. AArch32 Coprocessor Register Access: MRC / MCR

ARM32 uses coprocessor 15 (CP15) for system/MMU control registers. The encoding is:

### MRC — Move to Register from Coprocessor
```asm
MRC p15, <opc1>, <Rt>, <CRn>, <CRm>, <opc2>
// Reads CP15 register → Rt (32-bit)
```

### MCR — Move to Coprocessor from Register
```asm
MCR p15, <opc1>, <Rt>, <CRn>, <CRm>, <opc2>
// Writes Rt → CP15 register
```

### ARM32 Examples
```asm
// Read SCTLR (System Control Register) — ARM32
MRC p15, 0, r0, c1, c0, 0

// Write TTBR0 — ARM32
MCR p15, 0, r1, c2, c0, 0

// Write DACR (Domain Access Control Register) — ARM32
MCR p15, 0, r2, c3, c0, 0

// Invalidate entire TLB — ARM32
MCR p15, 0, r0, c8, c7, 0

// Data Synchronization Barrier — ARM32 (encoded as CP15 operation)
MCR p15, 0, r0, c7, c10, 4
```

---

## 4. Encoding Comparison Table

The following table maps key ARM32 CP15 registers to their AArch64 equivalents:

| Function | ARM32 (CP15) | AArch64 |
|---|---|---|
| System Control | `MRC p15, 0, Rd, c1, c0, 0` | `MRS Xd, SCTLR_EL1` |
| Translation Table Base 0 | `MRC p15, 0, Rd, c2, c0, 0` | `MRS Xd, TTBR0_EL1` |
| Translation Table Base 1 | `MRC p15, 1, Rd, c2, c0, 0` | `MRS Xd, TTBR1_EL1` |
| Translation Table Control | `MRC p15, 0, Rd, c2, c0, 2` | `MRS Xd, TCR_EL1` |
| Domain Access Control | `MRC p15, 0, Rd, c3, c0, 0` | N/A (no domains in AArch64) |
| Data Fault Status | `MRC p15, 0, Rd, c5, c0, 0` | `MRS Xd, ESR_EL1` (partial) |
| Data Fault Address | `MRC p15, 0, Rd, c6, c0, 0` | `MRS Xd, FAR_EL1` |
| Cache Type | `MRC p15, 0, Rd, c0, c0, 1` | `MRS Xd, CTR_EL0` |
| TLB Invalidate All | `MCR p15, 0, R0, c8, c7, 0` | `TLBI VMALLE1` |
| Context ID | `MRC p15, 0, Rd, c13, c0, 1` | `MRS Xd, CONTEXTIDR_EL1` |
| ASID | Encoded in TTBR0 [7:0] (ARM32) | `TTBR0_EL1[63:48]` (AArch64) |
| Vector Base | `MRC p15, 0, Rd, c12, c0, 0` | `MRS Xd, VBAR_EL1` |
| Memory Attribute Indirection | `MRC p15, 0, Rd, c10, c2, 0` | `MRS Xd, MAIR_EL1` |

---

## 5. MSR Immediate Form — Special PSR Fields

AArch64 also has an immediate form of `MSR` for writing specific PSTATE fields:

```asm
MSR DAIFSet, #imm    // Set DAIF interrupt mask bits
MSR DAIFClr, #imm    // Clear DAIF interrupt mask bits
MSR SPSel, #0        // Select SP_EL0 (thread stack)
MSR SPSel, #1        // Select SP_EL1 (handler stack)
MSR PAN, #1          // Enable PAN (Privileged Access Never)
MSR UAO, #1          // Enable UAO (User Access Override)
```

---

## 6. System Register Encoding (how the hardware sees it)

Every AArch64 system register has an underlying 5-field encoding used in the instruction binary:

```
System register encoding: op0:op1:CRn:CRm:op2

Examples:
SCTLR_EL1 = op0=3, op1=0, CRn=c1, CRm=c0, op2=0   → encoding: 3,0,c1,c0,0
TCR_EL1   = op0=3, op1=0, CRn=c2, CRm=c0, op2=2   → encoding: 3,0,c2,c0,2
TTBR0_EL1 = op0=3, op1=0, CRn=c2, CRm=c0, op2=0   → encoding: 3,0,c2,c0,0
MAIR_EL1  = op0=3, op1=0, CRn=c10, CRm=c2, op2=0  → encoding: 3,0,c10,c2,0
```

You can also use the raw encoding with `S<op0>_<op1>_<CRn>_<CRm>_<op2>` syntax for registers not known to the assembler (common for implementation-defined registers):

```asm
// Access an implementation-defined register (e.g., Qualcomm-specific)
mrs x0, S3_1_c15_c0_0
msr S3_1_c15_c0_0, x0
```

---

## 7. Access Control: Which EL Can Access Which Register?

System register accesses are access-controlled by the hardware. Attempting to access a register from an insufficient EL causes a **trap** to the appropriate higher EL.

```
Register        Minimum EL for access
SCTLR_EL1       EL1
TCR_EL1         EL1
TTBR0_EL1       EL1
VBAR_EL1        EL1
HCR_EL2         EL2
VTTBR_EL2       EL2
SCR_EL3         EL3
SCTLR_EL3       EL3
```

From EL0, any `MSR`/`MRS` to a system register (except `DAIF` via `MSR DAIFSet` which is allowed) causes a Synchronous Exception to EL1 with `ESR_EL1.EC = 0x18` (MSR/MRS/SYS trap).

**Exception**: Some EL0-accessible registers:
- `CTR_EL0` — Cache Type Register (readable from EL0).
- `DCZID_EL0` — Data Cache Zero ID (readable from EL0).
- `TPIDR_EL0` — Thread Pointer ID Register (used for `__thread`/TLS by libc).
- `TPIDRRO_EL0` — Read-only thread pointer (set by kernel for CPU number etc).

---

## 8. Barriers Required After System Register Writes

After writing certain system registers, explicit barriers are required to ensure the change is visible to subsequent instructions:

```asm
// After writing TTBR0/1_EL1 — must ISB to make new translation take effect
msr TTBR0_EL1, x0
isb

// After writing SCTLR_EL1 (enabling MMU) — must ISB
msr SCTLR_EL1, x0
isb

// After writing TCR_EL1 — must ISB before any translation using new settings
msr TCR_EL1, x1
isb

// After TLB invalidation — DSB then ISB sequence mandatory
tlbi vmalle1
dsb ish
isb
```

Without `ISB`, the processor may continue executing instructions using the old register values due to speculative execution and out-of-order pipelines.

---

## 9. Linux Kernel Usage

Linux ARM64 uses wrapper macros for system register access:

```c
// arch/arm64/include/asm/sysreg.h

// Read system register
#define read_sysreg(r) ({                               \
    u64 __val;                                          \
    asm volatile("mrs %0, " __stringify(r) : "=r" (__val)); \
    __val;                                              \
})

// Write system register
#define write_sysreg(v, r) do {                         \
    u64 __val = (u64)(v);                               \
    asm volatile("msr " __stringify(r) ", %x0"          \
                 : : "rZ" (__val));                     \
} while (0)

// Usage in kernel:
u64 sctlr = read_sysreg(sctlr_el1);
write_sysreg(INIT_SCTLR_EL1_MMU_ON, sctlr_el1);
isb();   // Always required after SCTLR write
```

```c
// Reading MAIR_EL1
u64 mair = read_sysreg(mair_el1);

// Setting TTBR0_EL1 during context switch
// arch/arm64/mm/context.c
static void cpu_switch_mm(pgd_t *pgd, struct mm_struct *mm)
{
    BUG_ON(pgd == swapper_pg_dir);
    cpu_set_reserved_ttbr0();          // Temporary TTBR0 = reserved page
    local_flush_tlb_all();             // Flush stale TLB entries
    write_sysreg(ttbr1, ttbr1_el1);   // Update TTBR1 if needed
    write_sysreg(ttbr0, ttbr0_el1);   // Install new process page table
    isb();
}
```

---

## 10. Common Implementation-Defined Registers

SoC vendors add their own registers accessed via the raw S3_x_cx_cx_x syntax. In ARM IP:

```asm
// ARM Cortex-A CPU Extended Control Register (CPUECTLR_EL1)
mrs x0, S3_1_c15_c2_1    // Read CPUECTLR_EL1

// ARM CPU Activity Monitor (AMCNTENSET0_EL0)
mrs x0, S3_3_c13_c2_1
```

Qualcomm Kryo CPUs have additional registers for:
- Cache line size control.
- Prefetch control.
- Error detection configuration.

---

## 11. Interview Questions & Answers

**Q1: Why did AArch64 abandon the CP15 coprocessor model?**

The CP15 model was confusing: many system functions were crammed into a single coprocessor with overlapping encodings, and the `CRn/CRm/opc1/opc2` fields were not mnemonic. AArch64 introduces named registers (`SCTLR_EL1`, `TCR_EL1`, etc.) that are self-documenting and organized per EL. Hardware enforces access control based on the current EL, and the assembler prevents typos by checking register names at assembly time.

**Q2: What happens if EL1 kernel code tries to read `SCR_EL3`?**

It causes a Synchronous Exception (trap) to EL3, because `SCR_EL3` is only accessible at EL3. `ESR_EL3.EC = 0x18` (System instruction trap). TF-A (running at EL3) would handle this trap; typically returning an error or taking action depending on the SMCCC implementation. Linux kernel code never does this since TF-A exposes needed secure functionality via `SMC` calls.

**Q3: After writing `SCTLR_EL1` to enable the MMU, why is `ISB` mandatory?**

The CPU pipeline may have already fetched instructions past the `MSR SCTLR_EL1` write. Without `ISB`, those fetched instructions might execute using the old MMU-off state (i.e., physical = virtual addressing). The `ISB` flushes the pipeline, ensuring subsequent instructions are fetched with the new MMU-on context, using virtual-to-physical translation.

**Q4: Can you read TTBR1_EL1 from EL0?**

No. `TTBR1_EL1` is accessible only at EL1 and higher. An attempt from EL0 causes a Synchronous Exception to EL1 (`ESR_EL1.EC = 0x18`). The kernel's handler would send `SIGILL` to the offending process. User space never needs to read page table bases — the MMU handles translation transparently.

**Q5: What is TPIDR_EL0 used for?**

`TPIDR_EL0` (Thread Pointer ID Register, EL0 accessible) holds the thread-local storage (TLS) base pointer. The C library's `__thread` keyword and POSIX `pthread_key_create`/`pthread_getspecific` use this register. In Linux, when a thread is created (`clone()`), the kernel writes the TLS base to `TPIDR_EL0` for that thread. User space reads it with `MRS x0, TPIDR_EL0` directly (no syscall needed), making TLS access O(1).

---

## 12. Quick Reference

| ARM32 Instruction | AArch64 Instruction | Register | Notes |
|---|---|---|---|
| `MRC p15, 0, Rd, c1, c0, 0` | `MRS Xd, SCTLR_EL1` | System Control | Requires EL1 |
| `MRC p15, 0, Rd, c2, c0, 0` | `MRS Xd, TTBR0_EL1` | Translation Table Base 0 | ISB after write |
| `MRC p15, 0, Rd, c2, c0, 2` | `MRS Xd, TCR_EL1` | Translation Control | ISB after write |
| `MRC p15, 0, Rd, c10, c2, 0` | `MRS Xd, MAIR_EL1` | Memory Attributes | ISB after write |
| `MCR p15, 0, R0, c8, c7, 0` | `TLBI VMALLE1` | TLB Invalidate | DSB+ISB after |
| `MRC p15, 0, Rd, c12, c0, 0` | `MRS Xd, VBAR_EL1` | Vector Base | Requires EL1 |
| `MRC p15, 0, Rd, c13, c0, 4` | `MRS Xd, TPIDR_EL1` | Kernel thread ptr | EL1 only |
| `MRC p15, 0, Rd, c13, c0, 2` | `MRS Xd, TPIDR_EL0` | User thread ptr (TLS) | EL0 readable |
