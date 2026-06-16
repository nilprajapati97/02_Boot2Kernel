# PAC — Pointer Authentication Codes Deep Dive

**Category**: Security Extensions  
**Platform**: ARM64 (AArch64) — ARMv8.3-A

---

## 1. Concept Foundation

```
PAC (Pointer Authentication Codes): hardware-based CFI for pointers

Attack: Return-Oriented Programming (ROP)
  Attacker: overwrites saved return address on stack
            chains together "gadgets" (small instruction sequences ending in RET)
            builds arbitrary computation from legitimate code fragments
  Historical: ~50% of exploits use some form of control-flow hijacking

Without PAC:
  Function call stack frame:
    [SP + 0]:  saved X29 (frame pointer)
    [SP + 8]:  saved X30 (return address) ← attacker writes here
  On RET: CPU blindly jumps to whatever is at saved X30

With PAC (PACIASP / AUTIASP pattern):
  Function entry:  PACIASP → sign X30 with SP + APIA_KEY → signed_lr
  Stack:           stores signed_lr (not raw X30)
  Function exit:   AUTIASP → verify + strip PAC from signed_lr
                   if valid: X30 = original return address
                   if forged: X30 = corrupted address → crash on RET
  
  Attacker: overwrites signed_lr on stack
            BUT: cannot forge valid PAC without the secret APIA_KEY
            Result: AUTIASP corrupts the forged address → controlled crash

Key design:
  PAC bits: stored in unused VA bits (bits[63:48] for user, or [55:48] for 48-bit VA)
  For a 48-bit VA space (TTBR1 with TBI):
    User pointer: bits[63:56] = PAC value (8 bits in 64KB granule case)
                  bits[55:48] = PAC/address extension
  PAC algorithm: QARMA (Qualcomm ARMv8 AES-based MAC) — fast, hardware-implemented
  Security strength: PAC width depends on VA size (more bits = stronger PAC)
```

---

## 2. ARM64 Hardware Detail

### 2.1 PAC Key Registers

```
Five key pairs, each 128-bit (two 64-bit system registers):

IA key (Instruction Authentication A — most common for return addresses):
  APIAKeyLo_EL1: lower 64 bits of IA key
  APIAKeyHi_EL1: upper 64 bits of IA key

IB key (Instruction Authentication B):
  APIBKeyLo_EL1: lower 64 bits
  APIBKeyHi_EL1: upper 64 bits

DA key (Data Authentication A):
  APDAKeyLo_EL1: lower 64 bits
  APDAKeyHi_EL1: upper 64 bits

DB key (Data Authentication B):
  APDBKeyLo_EL1: lower 64 bits
  APDBKeyHi_EL1: upper 64 bits

GA key (Generic Authentication — for user-space generic pointer auth):
  APGAKeyLo_EL1: lower 64 bits
  APGAKeyHi_EL1: upper 64 bits

Key access:
  EL0: CANNOT read/write key registers (EL1 privilege required)
  EL1: can write keys (Linux kernel sets per-process keys on context switch)
  EL2/EL3: can write all keys (hypervisor/firmware use)
  
  Reading keys:
    MRS X0, APIAKeyLo_EL1   // read IA lower key
    MSR APIAKeyLo_EL1, X0   // write IA lower key
```

### 2.2 PAC Instructions Reference

```
Signing instructions (generate PAC and insert into pointer):

PACIA Xd, Xn:    Sign Xd using IA key, modifier = Xn
PACIB Xd, Xn:    Sign Xd using IB key, modifier = Xn
PACDA Xd, Xn:    Sign Xd using DA key, modifier = Xn
PACDB Xd, Xn:    Sign Xd using DB key, modifier = Xn
PACGA Xd, Xn, Xm: Generic auth: Xd = PAC of (Xm) using GA key mod Xn

Special forms:
PACIASP:         PACIA X30, SP (sign LR with IA key, modifier = SP)
PACIBSP:         PACIB X30, SP (sign LR with IB key, modifier = SP)
PACIAZ:          PACIA X30, XZR (sign LR with IA key, modifier = 0)
PACIZA  Xd:      PACIA Xd, XZR
PACDZB  Xd:      PACDB Xd, XZR

Authentication instructions (verify PAC and strip):

AUTIA Xd, Xn:    Authenticate Xd using IA key, modifier = Xn
                 If valid: Xd = original pointer (PAC bits cleared)
                 If FAIL:  Xd[63:62] or Xd[55] set to error indicator
                           → any branch using this address → fault

AUTIB, AUTDA, AUTDB: same pattern with respective keys

AUTIASP:         AUTIA X30, SP (authenticate LR with IA key, SP modifier)
AUTIAZ:          AUTIA X30, XZR

Strip instructions (no authentication — just remove PAC bits):

XPACI  Xd:       Strip IA/IB PAC bits from Xd (no key check — unsafe!)
XPACD  Xd:       Strip DA/DB PAC bits from Xd
XPACLRI:         Strip PAC from LR (X30) — for stack unwinding without key
```

### 2.3 PAC Bit Width and Position

```
PAC bit location depends on VA size (T0SZ/T1SZ in TCR_EL1):

For 48-bit VA (T0SZ = 16):
  User pointer (TTBR0): VA range 0x0000_0000_0000_0000 to 0x0000_FFFF_FFFF_FFFF
  PAC bits: bits[63:48] = 16 bits of PAC
  With TBI: top byte bits[63:56] used for PAC (8 bits accessible to SW)
  
  Example signed pointer:
  Original: 0x0000_7FFF_DEAD_BEEF  (48-bit user address)
  Signed:   0x1234_7FFF_DEAD_BEEF  (PAC = 0x1234 in bits[63:48])
  
For 52-bit VA (T0SZ = 12, FEAT_LPA):
  PAC bits: bits[63:52] = 12 bits of PAC
  Fewer bits → slightly weaker (4096 guesses to brute force vs 65536)

Kernel pointer (TTBR1, 48-bit VA):
  Original: 0xFFFF_8000_1234_5678
  PAC bits: must avoid interfering with sign extension
  Kernel PAC: uses bits[63:56] where sign extension is (all-ones)
  PAC replaces some of those all-ones bits
  
  Strip: XPACI extends sign bit to restore all-ones in top bits

QARMA (Qualcomm ARMv8 Message Authentication):
  Algorithm: lightweight tweakable block cipher
  Input:  64-bit pointer + 64-bit modifier (SP or 0)
  Key:    128-bit key (from APIAKeyLo/Hi_EL1)
  Output: 64-bit MAC; truncated to fit in available PAC bits
  Hardware: single-cycle on modern ARM cores (< 3 cycles typical)
```

### 2.4 SCTLR_EL1 PAC Enable Bits

```
SCTLR_EL1 bit fields controlling PAC:
  EnIA[31]: Enable IA key instructions (PACIA, AUTIA, etc.)
            0 = PACIA/AUTIA treated as NOP
            1 = PACIA/AUTIA execute normally (default in Linux)
  EnIB[30]: Enable IB key instructions
  EnDA[27]: Enable DA key instructions  
  EnDB[13]: Enable DB key instructions
  
  Linux sets: SCTLR_EL1.EnIA=1, EnIB=1, EnDA=1, EnDB=1
  (all key types enabled)
  
  Per-process control (EL0 can control via prctl):
    prctl(PR_PAC_RESET_KEYS, mask, 0, 0, 0): regenerate keys
    prctl(PR_GET_TAGGED_ADDR_CTRL): read current PAC settings
    prctl(PR_SET_TAGGED_ADDR_CTRL, ..., PR_TAGGED_ADDR_ENABLE): enable tagged addr
```

---

## 3. Linux Kernel Implementation

### 3.1 Kernel PAC for Stack Protection

```c
// arch/arm64/include/asm/scs.h + arch/arm64/include/asm/assembler.h

// CONFIG_ARM64_PTR_AUTH_KERNEL: enable PAC for kernel stack frames

// Compiler generates (with -mbranch-protection=pac-ret+leaf or similar):
// Function prolog:
//   paciasp        // PACIA X30, SP → sign LR with IA key + SP
//   stp x29, x30, [sp, #-16]!  // save signed LR to stack
// Function epilog:
//   ldp x29, x30, [sp], #16    // restore signed LR from stack
//   autiasp        // AUTIA X30, SP → verify and strip
//   ret            // branch to authenticated address
```

### 3.2 Per-Process Key Management

```c
// arch/arm64/include/asm/ptrauth.h

struct ptrauth_keys_user {
    struct ptrauth_key apia;   // {lo, hi} = 128-bit key
    struct ptrauth_key apib;
    struct ptrauth_key apda;
    struct ptrauth_key apdb;
    struct ptrauth_key apga;
};

struct ptrauth_keys_kernel {
    struct ptrauth_key apia;   // kernel uses only IA key
};

// Stored in: task->thread.keys_user (user keys)
//            task->thread.keys_kernel (kernel key)

// arch/arm64/kernel/process.c
ptrauth_thread_init_user():
    // Called at exec() time:
    get_random_bytes(&task->thread.keys_user, sizeof(...));
    // Each new exec: new random keys → unique per-process PAC

ptrauth_thread_switch_user(tsk):
    // Called on context switch:
    // Restore this process's keys into hardware registers
    __ptrauth_key_install(APIA, tsk->thread.keys_user.apia);
    // = MSR APIAKeyLo_EL1 + MSR APIAKeyHi_EL1
    // Similarly for IB, DA, DB, GA

// Thread fork: child gets DIFFERENT random keys
// (different keys = different PAC for same pointer → correct, no information leak from parent)
```

### 3.3 Stack Unwinding with PAC

```c
// arch/arm64/kernel/stacktrace.c
// Problem: unwinding stacks requires reading saved LR (X30) values
// PAC-signed LR cannot be directly used as address for unwinding
// Solution: XPACLRI (strip without authentication) or use AUTIA with known modifier

// Linux frame unwinder:
unwind_frame():
    // Read saved X30 from stack (PAC-signed value)
    lr = READ_ONCE(*(unsigned long *)(frame->fp + 8));
    
    // Strip PAC bits for address translation (unsafe, but OK for unwinding):
    lr = ptrauth_strip_insn_pac(lr);  // = XPACI equivalent
    // OR use: __builtin_return_address() with -mbranch-protection
    
    // Result: original (unsigned) return address for symbol lookup
```

---

## 4. Hardware-Software Interaction

```
Complete call/return lifecycle with PAC:

Caller (main):
  BL func         // X30 = next_instruction_addr (unsigned)
  
Callee (func):
  PACIASP         // X30 = PACIA(X30, SP) = PAC-signed LR
  STP X29, X30, [SP, #-16]!   // signed LR on stack

  ... function body ...
  
  (Attacker overwrites [SP+8] with forged address: 0xDEADBEEF)
  
  LDP X29, X30, [SP], #16   // X30 = 0xDEADBEEF (attacker's value)
  AUTIASP                    // AUTIA(0xDEADBEEF, SP) → checks PAC
                             // 0xDEADBEEF has no valid PAC for current key+SP
                             // → AUTIA sets X30[63:62] or X30[55] = error bits
  RET                        // branch to corrupted X30 → fault
                             // SIGSEGV sent to process (not ROP gadget executed)
  
Security guarantee: attacker must know:
  - The 128-bit APIA key (stored in EL1 registers, inaccessible from EL0)
  - The exact SP value at the signing time (changes per call site)
  - Brute force: 2^16 attempts for 16-bit PAC field = 65536 tries
    (OS can detect and kill process after N faults)
```

---

## 5. Interview Q&A

**Q1: What is the modifier in PAC and why is it SP?**
The modifier feeds an extra context value into the QARMA MAC computation. By using SP (stack pointer) as the modifier for `PACIASP`, the signed LR becomes unique to BOTH the key AND the current stack depth. An attacker who successfully guesses a valid PAC for one stack frame cannot reuse it at a different stack depth (different SP → different PAC). This prevents gadget reuse across call frames even if a key were somehow leaked at one depth.

**Q2: Can an attacker bypass PAC by brute force?**
Theoretically: for a 16-bit PAC field (48-bit VA), 65536 attempts. But: each failed `AUTIA` causes the subsequent branch to fault → SIGSEGV. The OS sees the process crash immediately. Even if the attacker could retry (e.g., `fork()` + test), Linux regenerates keys at each `exec()`. A fork doesn't help because the parent's keys differ. Rate limiting: the kernel can monitor SIGSEGV frequency and kill/throttle the process. In practice: brute force is not viable for a running process.

**Q3: What is XPACI and when should it be used?**
`XPACI` strips PAC bits from a pointer WITHOUT verifying the signature. It is NOT a security operation — it provides no protection. Valid use cases: (1) stack unwinding/debug tools that need the original address without having access to the correct authentication context; (2) signals: the kernel strips PAC before delivering saved PC in `ucontext_t` (userspace expects a clean address). Using XPACI for actual control flow (instead of AUTIA) completely defeats PAC protection.

**Q4: How does PAC interact with fork()?**
At `fork()`: child inherits the parent's address space and stack. But child gets its own register set. Linux regenerates new random PAC keys for the child on exec() (not on fork()). Important: if child doesn't exec(), it inherits parent's keys. This is safe for stack frames because child's stack is a copy of parent's, and the PAC values on the stack were signed with parent's (= child's same) keys. After exec(): keys are replaced, so inherited stack data (which doesn't exist after exec) is irrelevant.

**Q5: What is the difference between PACIA/AUTIA (IA key) vs PACIB/AUTIB (IB key)?**
They use separate 128-bit keys, providing independent security domains. Typical Linux convention: IA key = return address protection (`PACIASP`). IB key = function pointers, vtable pointers. DA/DB = data pointer authentication (less common). GA = generic, for user-space use. By using separate keys for different pointer types: compromise of one key doesn't affect others. Also: if a vulnerability allows forging IA-signed pointers, it doesn't automatically allow forging IB-signed function pointers.

---

## 6. Quick Reference

| Instruction | Key | Typical Use |
|---|---|---|
| `PACIASP` | IA + SP | Sign return address (function entry) |
| `AUTIASP` | IA + SP | Verify return address (function exit) |
| `PACIA Xd, Xn` | IA + Xn | Sign arbitrary pointer |
| `AUTIA Xd, Xn` | IA + Xn | Verify arbitrary pointer |
| `XPACI Xd` | — | Strip IA/IB PAC (no verify!) |
| `PACGA Xd, Xn, Xm` | GA | Generic auth (user-defined) |

| Config | Purpose |
|---|---|
| `CONFIG_ARM64_PTR_AUTH` | Enable PAC in kernel |
| `CONFIG_ARM64_PTR_AUTH_KERNEL` | Kernel stack frames protected |
| `-mbranch-protection=pac-ret` | GCC: add PACIASP/AUTIASP in all functions |
| `-mbranch-protection=standard` | GCC: add both PAC + BTI |
