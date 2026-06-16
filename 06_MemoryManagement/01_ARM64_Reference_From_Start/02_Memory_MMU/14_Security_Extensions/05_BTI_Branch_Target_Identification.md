# BTI — Branch Target Identification Deep Dive

**Category**: Security Extensions  
**Platform**: ARM64 (AArch64) — ARMv8.5-A

---

## 1. Concept Foundation

```
BTI (Branch Target Identification): prevents JOP and COP attacks

Attack background:
  JOP (Jump-Oriented Programming): like ROP but uses JMP gadgets
  COP (Call-Oriented Programming): uses CALL gadgets
  
  After PAC broke simple ROP: attackers adapted to JOP/COP
  JOP: chain gadgets via indirect BR/BLR instructions
  
  Example JOP without BTI:
    Attacker overflows a function pointer field → set to &gadget_1
    gadget_1: ...code... ; BR X3   (indirect branch)
    X3: → gadget_2 (attacker controlled)
    → arbitrary code execution via indirect branch chain
  
  BTI defense:
    Mark all valid indirect-branch targets with BTI instruction
    All other indirect-branch destinations: fault
    Attacker: cannot land on arbitrary gadgets (must be BTI-labeled)
    Only compiler-generated BTI labels are valid targets
    → dramatically reduces usable gadget set

BTI instruction encoding:
  BTI is encoded as HINT #N (NOP-space instructions)
  HINT #32: BTI (general — valid for any indirect branch)
  HINT #34: BTI c (call — valid target for BLR only)
  HINT #36: BTI j (jump — valid target for BR only)
  HINT #38: BTI jc (both — valid for BR and BLR)
  
  On non-BTI hardware: HINT executes as NOP (backward compatible)
  On BTI hardware with BTI disabled: HINT executes as NOP
  On BTI hardware with BTI enabled: HINT enforces target validity
```

---

## 2. ARM64 Hardware Detail

### 2.1 GP Bit in Page Descriptors

```
GP (Guarded Page) bit: controls BTI enforcement per-page

ARM64 page table entry (stage 1 leaf descriptor):
  Bit[50]: GP (Guarded Page)
  GP=0:   BTI not enforced (all indirect branches valid targets)
  GP=1:   BTI enforced (indirect branches MUST land on BTI instruction)
  
  For GP=1 enforcement:
    BLR X0: CPU checks if instruction at [X0] is BTI c or BTI jc
            If not: Branch Target Exception
    BR X0:  CPU checks if instruction at [X0] is BTI j or BTI jc
            If not: Branch Target Exception
  
  Direct branches (B, BL): ALWAYS valid (never need BTI label)
  Indirect branches only: BR, BLR, RET (RET is special, see below)
  
  RET:  RET is NOT subject to BTI checking
        (RET target = X30 = return address; if PA-signed, PAC protects this)
        PAC + BTI complement each other:
          PAC: protects backward edges (RET targets)
          BTI: protects forward edges (BR/BLR targets)

BTYPE field in PSTATE:
  2-bit field that tracks indirect branch state
  Used internally by CPU to check BTI validity
  
  BTYPE = 0b00: not a branch target (normal instruction flow)
  BTYPE = 0b01: taken by indirect branch (BR Xn) — requires BTI j or jc
  BTYPE = 0b10: taken by indirect call (BLR Xn or BLR XZR) — requires BTI c or jc
  BTYPE = 0b11: taken by some other indirect branch form
  
  On instruction fetch: if BTYPE ≠ 0 and instruction ≠ BTI (matching type)
  → Branch Target Exception (ESR_EL1.EC = 0x24, instruction abort)
```

### 2.2 SCTLR_EL1 BTI Control Bits

```
SCTLR_EL1.BT0[35]:  BTI enforcement for EL0 (user space)
  0: BTI not enforced at EL0
  1: BTI enforced at EL0 (GP=1 pages subject to BTI checking)

SCTLR_EL1.BT1[36]:  BTI enforcement for EL1 (kernel)
  0: BTI not enforced at EL1
  1: BTI enforced at EL1 (GP=1 kernel pages subject to BTI checking)

Linux kernel:
  Sets SCTLR_EL1.BT1=1 if CONFIG_ARM64_BTI_KERNEL=y
  Sets SCTLR_EL1.BT0=1 for user BTI (prctl controlled per-process)
  
  Per-process BTI (user space):
    prctl(PR_SET_TAGGED_ADDR_CTRL, PR_BTI_ENABLE, ...): enable for process
    mmap(PROT_BTI): enable BTI for specific mapping
    This sets GP=1 in PTEs for the VMA
```

### 2.3 PLT Stubs and BTI

```
PLT (Procedure Linkage Table): used for calls to shared libraries

Without BTI:
  PLT entry:
    ADRP X16, GOT_ENTRY@PAGE    // load GOT address
    LDR X16, [X16, #:lo12:GOT]  // load actual function address from GOT
    BR X16                       // indirect jump to function

With BTI:
  PLT entry needs BTI label because:
    BLR X17 (from caller) → PLT entry (indirect call)
    PLT entry first instruction must be BTI c or BTI jc
  
  BTI PLT entry:
    BTI c                        // landing pad for BLR (call)
    ADRP X16, GOT_ENTRY@PAGE
    LDR X16, [X16, #:lo12:GOT]
    BR X16                       // jump to function (NOT a call)
    
  BUT: PLT's BR X16 lands on function entry
    function entry should be: BTI c (for calls) or BTI jc
    GCC: -mbranch-protection=bti adds BTI c at start of each function
    
  .note.gnu.property (ELF note):
    Marks binary as BTI-compatible: GNU_PROPERTY_AARCH64_FEATURE_1_BTI
    Dynamic linker (ld.so): checks this → if present: maps with PROT_BTI
    If absent: maps WITHOUT PROT_BTI (backward compat for old binaries)
```

### 2.4 BTI Exception Details

```
BTI fault:
  ESR_EL1.EC = 0x24 (0b100100): Instruction Abort from lower EL
  ESR_EL1.ISS.BTITRAP = 1: indicates BTI violation (not page fault)
  FAR_EL1: virtual address of the invalid target instruction
  
  Linux fault handler:
    do_el0_ia() → do_page_fault() → checks BTITRAP bit
    → sends SIGILL (not SIGSEGV) to process
    SI_CODE: ILL_ILLOPC (illegal opcode, from CPU perspective)
    
  Note: SIGILL for BTI violations, SIGSEGV for MTE violations
  Different signal: helps debuggers distinguish the fault type
```

---

## 3. Linux Kernel Implementation

### 3.1 Kernel BTI Setup

```c
// arch/arm64/kernel/head.S
// At boot (if CONFIG_ARM64_BTI_KERNEL):
// SCTLR_EL1.BT1 is set in CPU init

// arch/arm64/kernel/cpufeature.c
// CPU feature detection:
ARM64_CPUCAP_STRICT_BOOT_CPU_FEATURE: CPU_FEATURE_BTI
// Checks: ID_AA64PFR1_EL1.BT field
// ID_AA64PFR1_EL1.BT[3:0]:
//   0b0000: BTI not implemented
//   0b0001: BTI implemented (BTI instructions + GP page attribute available)

// arch/arm64/mm/mmap.c
// Kernel text mapped with GP=1:
// arch/arm64/kernel/vmlinux.lds.S marks kernel code sections with BTI

// arch/arm64/include/asm/pgtable-hwdef.h
#define PTE_GP          (_AT(pteval_t, 1) << 50)  // GP bit
```

### 3.2 User-Space BTI via mmap/prctl

```c
// mm/mmap.c + arch/arm64/mm/mmap.c

// PROT_BTI = 0x10 (ARM64 specific mmap protection flag)
// Maps to: GP bit set in PTE

arch64_calc_vm_prot_bits():
    if (prot & PROT_BTI)
        vm_flags |= VM_ARM64_BTI;

// When PTEs are created for BTI VMA:
set_pte_at():
    if (vma->vm_flags & VM_ARM64_BTI)
        pte |= PTE_GP;

// Dynamic linker sets PROT_BTI based on .note.gnu.property:
// ld.so/glibc: mprotect(addr, len, PROT_READ|PROT_EXEC|PROT_BTI)
// For BTI-annotated ELF executables and libraries
```

### 3.3 BTI Feature Checking

```c
// arch/arm64/kernel/cpufeature.c
// Feature advertised to userspace via hwcap:
HWCAP2_BTI: set if ID_AA64PFR1_EL1.BT ≥ 1

// User can check:
// getauxval(AT_HWCAP2) & HWCAP2_BTI
// or: /proc/cpuinfo: "Features: ... bti ..."

// ID_AA64PFR1_EL1 register:
// [3:0]  BT: BTI support
// [7:4]  SSBS: Speculative Store Bypass Safe
// [11:8] MTE: Memory Tagging Extension support
// [19:16] RNDR_trap: random number trap support
```

---

## 4. Hardware-Software Interaction

```
Full BTI call chain (library call with PROT_BTI):

  main() calls printf() (in libc.so, mapped with PROT_BTI, GP=1):
  
  main:
    BLR X17   ← indirect call to PLT entry (for printf)
              ← BLR: sets BTYPE = 0b10 (indirect call)
              ← CPU: fetches instruction at PLT entry
  
  PLT entry (in libc.so, GP=1 page):
    BTI c    ← BTYPE=0b10, instruction=BTI c → VALID (BTI c accepts BLR)
               BTYPE cleared to 0 after BTI passes
    ADRP X16, printf@PAGE
    LDR X16, [X16, #:lo12:printf_GOT]
    BR X16   ← indirect jump to printf; sets BTYPE = 0b01 (BR)
  
  printf() in libc.so (GP=1 page):
    BTI c    ← BTYPE=0b01 from BR, instruction=BTI c
               Wait: BTI c only accepts BLR (0b10), not BR (0b01)!
               
  Actually: function entries use BTI jc (accept both) OR:
            PLT uses BLR to call function after loading address:
    
  Actual pattern used by GCC:
    PLT:
      BTI c
      ADRP X16, ...
      LDR X16, [...]
      BR X16          ← BR to function
    
    Function entry:
      BTI j           ← accepts BR (0b01) ✓
      (function body)
    
  OR: function uses BTI jc for maximum flexibility

Attacker attempt:
  Overwrites function pointer → arbitrary address X in libc.so
  BLR X → sets BTYPE=0b10 → fetches instruction at X
  If X is NOT a BTI c/jc instruction → Branch Target Exception
  → SIGILL to process (attacker cannot redirect control flow)
```

---

## 5. Interview Q&A

**Q1: How does BTI differ from PAC in protecting control flow?**
PAC and BTI protect different transfer types (complementary, not competing):
- **PAC protects backward edges**: return addresses (`RET`). Prevents ROP (Return-Oriented Programming) where return addresses are corrupted on the stack. Signed with `PACIASP`, verified with `AUTIASP`.
- **BTI protects forward edges**: indirect branches (`BR`, `BLR`). Prevents JOP/COP (Jump/Call-Oriented Programming) where function pointers or vtable entries are corrupted. Valid landing pads must have `BTI c/j/jc`.
Both together: every control flow transfer (forward and backward) is validated by hardware.

**Q2: What happens if an old (non-BTI) binary calls a new BTI-enabled library?**
The ELF `.note.gnu.property` section marks binaries as BTI-compatible. The dynamic linker (ld.so) checks this: if the main executable does NOT have the BTI property, ld.so maps all libraries WITHOUT `PROT_BTI` (GP=0). The BTI check is skipped for that process. If the main executable IS BTI-compatible, all loaded libraries must also declare BTI support (or be loaded without GP). This ensures backward compatibility: old binaries still work, new binaries get full BTI protection.

**Q3: What is the GP bit and where does it live?**
The GP (Guarded Page) bit is bit[50] in an ARM64 Stage 1 leaf page table descriptor. When GP=1, the CPU enforces BTI for that page: indirect branches to non-BTI instructions cause a Branch Target Exception. GP=0: no BTI enforcement (legacy behavior). Linux sets GP via the `PTE_GP` flag when a VMA is mapped with `PROT_BTI`. The kernel text section itself is mapped with GP=1 when `CONFIG_ARM64_BTI_KERNEL=y`.

**Q4: Does BTI affect `RET` instructions?**
No. `RET` (which branches to X30) is NOT subject to BTI checking. `RET` is protected separately by PAC (`AUTIASP` verifies X30 before `RET` uses it). This division is intentional: BTI defends forward-edge control flow (BR/BLR), PAC defends backward-edge (RET). Note: `ERET` (exception level return) is also exempt from BTI checking.

**Q5: How does BTI affect performance?**
Near-zero overhead for correct code. Each `BTI c/j/jc` instruction executes as a NOP (1 cycle). The branch target check is done in hardware at instruction fetch — it's part of the fetch pipeline, not an extra check. Overhead is only felt on violations (which cause exceptions). Code size: each protected function entry gets one `BTI` instruction = 4 bytes per function. For a typical kernel with 100K functions: 400KB extra code size. Performance regression: typically < 0.1% for normal workloads.

---

## 6. Quick Reference

| BTI Instruction | Encoding | Valid For |
|---|---|---|
| `BTI` (jc) | HINT #32 | Any indirect branch (BR or BLR) |
| `BTI c` | HINT #34 | BLR only (function call target) |
| `BTI j` | HINT #36 | BR only (jump target) |
| `BTI jc` | HINT #38 | Both BR and BLR |

| Control | Purpose |
|---|---|
| `SCTLR_EL1.BT0` | BTI enforcement for EL0 |
| `SCTLR_EL1.BT1` | BTI enforcement for EL1 (kernel) |
| PTE GP bit[50] | Per-page BTI enable |
| `PROT_BTI` | mmap flag to set GP bit |
| `.note.gnu.property` | ELF BTI annotation |

| Config | Purpose |
|---|---|
| `CONFIG_ARM64_BTI_KERNEL` | Kernel code protected by BTI |
| `-mbranch-protection=bti` | GCC: add BTI labels to functions |
| `-mbranch-protection=standard` | GCC: PAC + BTI combined |
