# TrustZone and ARM Security Extensions

**Category**: Security Extensions  
**Platform**: ARM64 (AArch64)

---

## 1. TrustZone Concept

```
ARM TrustZone: hardware-enforced isolation between two worlds
  
  Non-Secure world (Normal World):
    Runs: Linux, Android, applications
    Exception levels: EL0 (apps), EL1 (kernel), EL2 (hypervisor)
    Cannot access: Secure world memory
    
  Secure world:
    Runs: Trusted OS (OP-TEE), Trusted Applications (TAs)
    Exception levels: S-EL0 (trusted apps), S-EL1 (trusted OS), EL3 (secure monitor)
    CAN access: Non-Secure memory (if needed)
    Protected from: Non-Secure world
    
  EL3 (Secure Monitor, EL3):
    Runs: TF-A (Trusted Firmware-A) / ARM Trusted Firmware
    Privilege: highest in system (always Secure)
    Role: world switch, boot, power management, platform security services

World switch mechanism:
  Non-secure → Secure:
    Application: SMC (Secure Monitor Call) instruction
    → CPU: exception to EL3
    → TF-A: context save (non-secure regs), context restore (secure regs)
    → TF-A: ERET to S-EL1 (OP-TEE) or S-EL0 (trusted app)
  
  Secure → Non-secure:
    Trusted OS: SMC return
    → TF-A: context save (secure regs), context restore (non-secure regs)
    → TF-A: ERET to NS-EL1 (Linux) or NS-EL0 (caller)
  
  SCR_EL3 (Secure Configuration Register at EL3):
    NS[0]: 0 = Secure world, 1 = Non-Secure world
    NSE[62]: (ARMv9) extended security state
    IRQ[1]: route IRQs to EL3 when set
    FIQ[2]: route FIQs to EL3 when set
    EA[3]: route SError to EL3 when set
    SMD[7]: disable SMC instructions
    HCE[8]: HVC enable from NS-EL1
    SIF[9]: Secure instruction fetch only from Secure memory
    RW[10]: non-secure EL2/EL1 AArch64 mode
    ST[11]: traps to EL3 for secure timer
    TWI[12]: trap WFI from NS world to EL3
    TWE[13]: trap WFE from NS world to EL3
```

---

## 2. NS Bit in Page Tables

```
NS (Non-Secure) bit in ARM64 page table entries:

Location: Stage 1 page table entries (EL3 and Secure EL1 only)
  Bit[5] in ARM64 long-descriptor block/page entry

Effect:
  NS=0: Physical address is in Secure PA space
  NS=1: Physical address is in Non-Secure PA space
  
  Secure world: can map NS=0 (secure PA) and NS=1 (non-secure PA)
  Non-Secure world: all translations always access Non-Secure PA space
                    (NS bit in entries ignored / always treated as NS=1)

TZASC (TrustZone Address Space Controller):
  Separate IP (not in CPU): typically AXI bus sideband IP
  Partitions DRAM into: Secure regions and Non-Secure regions
  Secure region: only Secure world can access (TZASC enforces on bus)
  Non-Secure world: access to Secure DRAM → bus error
  
  ARM TrustZone-400/380: typical TZASC for ARM64 SoCs
  Registers (at EL3 or trusted code only):
    TZASC_REGION_BASE_LOW[n]:  base address of region n
    TZASC_REGION_SIZE[n]:      size of region n
    TZASC_REGION_ATTRIBUTES[n]: S (secure bit), SP (subregion permissions)
  
  TZASC provides protection EVEN IF SMMU is bypassed or disabled:
    DRAM controller: checks TZASC before completing any request
    Non-secure DMA (even without SMMU): blocked by TZASC for secure regions

OP-TEE secure memory:
  Configured at boot by TF-A:
    TF-A: configures TZASC to protect OP-TEE's memory region
    Linux: cannot access this region (bus error)
    OP-TEE: runs in Secure world → can access its own secure memory
  
  Linux memory map avoids OP-TEE region:
    TF-A passes memory map to Linux (DT reserved-memory or ACPI)
    Linux: marks region as reserved → does not use for kernel/user allocations
    /proc/iomem: shows "TrustZone reserved" region

SMC (Secure Monitor Call) calling convention:
  SMCCC (Secure Monitor Call Calling Convention):
    Standard: ARM DEN0028 (SMCCC specification)
    Register x0: function identifier (32-bit)
    Registers x1-x7: function arguments
    Registers x0-x3: return values
    
  Function ID format:
    Bit[31]: Call type (0=SMC32, 1=SMC64)
    Bits[29:24]: OEN (Owner Entity Number)
      0x00-0x3F: Arm architecture calls
      0x04: Standard Secure Service (OP-TEE: 0x32000000-0x3200FFFF)
      0x05: Standard Hypervisor Service (KVM PSCI: 0x84000000-0x8400FFFF)
      0x06: Vendor Specific (platform: 0xC2000000-0xC200FFFF)
    Bits[15:0]: Function number within OEN
  
  PSCI (Power State Coordination Interface): SMC-based power management
    PSCI_CPU_ON (0xC4000003): bring up secondary CPU
    PSCI_CPU_OFF (0x84000002): power off this CPU
    PSCI_SYSTEM_RESET (0x84000009): system reset
    PSCI_SYSTEM_SUSPEND (0xC400000E): system suspend
    Linux: arm_smccc_smc() → SMC instruction → TF-A handles
```

---

## 3. Memory Tagging Extension (MTE)

```
MTE (Memory Tagging Extension): ARMv8.5-A feature
  Purpose: hardware-assisted memory safety (detect use-after-free, buffer overflow)
  
  Core idea: every 16-byte chunk of memory has a 4-bit "allocation tag"
             every pointer has a 4-bit "logical tag"
             on every load/store: hardware compares logical tag vs allocation tag
             mismatch → MTE fault (synchronous or asynchronous)

ARM64 MTE details:

Tag granule: 16 bytes (one tag per 16 bytes)
Tag size: 4 bits → 16 possible tag values
Tag storage: separate physical memory (Tag RAM)
  Each 4KB page: 256 bytes of tag storage (256 × 16B chunks × 4 bits = 256 bytes)
  ARM64: tag memory access via GCR_EL1, RGSR_EL1 system registers

Pointer tag: bits[59:56] of the virtual address
  ARM64 virtual address: 64 bits, but only 48 bits used (bits[47:0])
  Top byte: bits[63:56] used for TBI (Top Byte Ignore) extension
  MTE: bits[59:56] = 4-bit logical tag
  Pointer with tag: 0x1234_F000_12345678 (tag = 0xF in bits 59:56)

MTE instructions:
  IRG X0, X1, X2:  Insert Random tag, use tag exclusion mask in X2
                   Result: X0 = X1 with random tag inserted in bits[59:56]
  STG X0, [X1]:    Store Tag to memory (writes 4-bit tag to Tag RAM at [X1])
  LDG X0, [X1]:    Load Tag from memory (reads 4-bit tag from Tag RAM at [X1])
  STZG X0, [X1]:   Store Tag and Zero memory (tag + zero 16-byte chunk)
  ST2G X0, [X1]:   Store 2 Tags (32-byte region)

MTE memory access check (hardware automatic):
  LDR X2, [X0]:   // X0 has logical tag T
    1. Logical tag = X0[59:56] = T
    2. Allocation tag = Tag_RAM[X0 >> 4] = T'
    3. If T != T': MTE tag fault
       SYNC mode: synchronous exception, precise fault address
       ASYNC mode: accumulated in TFSR_EL1, imprecise
       ASYMM mode: store = async, load = sync
  
MTE modes:
  Synchronous (SCTLR_EL1.TCF=0b01):
    Fault on first tag mismatch, precise address in ESR/FAR
    Performance: ~5% overhead
    Use: debugging, catching bugs in development
  
  Asynchronous (SCTLR_EL1.TCF=0b10):
    Accumulates in TFSR_EL1 (no immediate fault)
    Performance: ~1-2% overhead
    Use: production (catch bugs without stopping on each one)
  
  Asymmetric (SCTLR_EL1.TCF=0b11):
    Stores: async, Loads: sync
    Balance: catches most load bugs precisely
  
Linux MTE support:
  CONFIG_ARM64_MTE: kernel MTE support
  prctl(PR_SET_TAGGED_ADDR_CTRL, PR_TAGGED_ADDR_ENABLE | PR_MTE_TCF_SYNC, 0, 0, 0)
  mmap(NULL, size, PROT_MTE | PROT_RW, ...): allocate MTE-tagged memory
  Glibc: malloc() with MTE: tags each allocation (IRG + STG)
  Kernel KASAN: uses MTE for hardware-assisted KASAN (CONFIG_KASAN_HW_TAGS)
```

---

## 4. PAC (Pointer Authentication) and BTI (Branch Target Identification)

```
PAC (Pointer Authentication): ARMv8.3-A
  Purpose: protect return addresses and function pointers from tampering
  
  Keys (stored in secure system registers):
    APIA_EL1: IA key (instruction address A, for return addresses)
    APIB_EL1: IB key (instruction address B)
    APDA_EL1: DA key (data address A)
    APDB_EL1: DB key (data address B)
    APGA_EL1: GA key (generic code, for GCR)
  
  PACIASP: sign x30 (link register) with SP + APIA key
           Insert MAC (Message Authentication Code) into bits[63:48] of x30
           signed_lr = PAC(x30, SP, APIA_EL1)
  
  AUTIASP: authenticate x30 with SP + APIA key
           If PAC matches: strip PAC bits, restore original address
           If PAC FAILS: set bits[63:2] to garbage (crash on use)
  
  XPACLRI: strip PAC from LR without authentication (for unwinding)
  
  ARM64 function prolog/epilog with PAC:
    PACIASP       // sign LR at function entry
    STP X29, X30, [SP, #-16]!  // save LR to stack
    ...
    LDP X29, X30, [SP], #16    // restore LR from stack
    AUTIASP       // authenticate LR at function exit
    RET           // use authenticated LR
  
  Attacker: cannot fake a valid return address without the key
    ROP (Return-Oriented Programming) attack: chained returns
    With PAC: each return address has MAC → forged addresses → AUTIASP fails → crash
  
  Linux PAC:
    CONFIG_ARM64_PTR_AUTH: enable PAC
    CONFIG_ARM64_PTR_AUTH_KERNEL: kernel uses PAC for its own call frames
    User PAC: per-process key in mm_context_t.ptrauth_key
              Stored in APIA_EL1 etc. on context switch
    Kernel PAC: kernel key in EL1 registers (not changed on user context switch)

BTI (Branch Target Identification): ARMv8.5-A
  Purpose: restrict which instructions are valid branch targets (prevent JOP/COP attacks)
  
  GP (Guarded Page) bit in page table entry:
    GP=1: this page uses BTI (all indirect branches must land on BTI instruction)
  
  BTI instruction types:
    BTI c:  valid target for BLR (call with link) only
    BTI j:  valid target for BR (jump) only
    BTI jc: valid target for both BR and BLR
    NOP:    in BTI context, NOP = BTI jc (compatible)
  
  Without BTI label: indirect branch to non-BTI code on GP=1 page → fault
  
  Linux BTI:
    CONFIG_ARM64_BTI_KERNEL: kernel code compiled with BTI
    GNU: -mbranch-protection=standard (GCC/Clang option)
    Kernel: all function entries have HINT #34 (BTI c encoding) or BTI c
    PROT_BTI mmap flag: enable BTI for user mappings
    Process: prctl(PR_SET_TAGGED_ADDR_CTRL) with PR_BTI_ENABLE
```

---

## 5. KPTI (Kernel Page Table Isolation)

```
KPTI (Kernel Page Table Isolation):
  Mitigation for: Meltdown (CVE-2017-5754) — speculative execution attack
  
  Meltdown attack:
    Attacker code (EL0): speculatively reads from kernel VA
    CPU speculation: runs "impossible" code path before permission check
    Data ends up in cache as side effect
    Attacker: infers kernel memory via cache timing
    Result: read all kernel memory from userspace!
  
  KPTI mitigation:
    Two separate page table sets:
      User page table (TTBR0_EL1 trampoline):
        Contains: only user mappings + minimal kernel trampoline
        Does NOT contain: full kernel address space
      Kernel page table (TTBR1_EL1 full):
        Contains: full kernel + user mappings
    
    Syscall/exception entry:
      CPU in EL0 → EL1 exception → must switch to full kernel PT
      Trampoline: tiny piece of code mapped in BOTH user and kernel PTs
      Trampoline: switch TTBR0 to kernel page table → full kernel visible
    
    Return to user:
      Clear TTBR0_EL1 (or switch to user-only PT)
      Kernel no longer speculatively accessible from EL0
  
  ARM64 KPTI:
    CONFIG_UNMAP_KERNEL_AT_EL0: enable KPTI on ARM64
    ARM64 vs x86: most ARM64 CPUs NOT vulnerable to Meltdown
                  (ARM speculative execution does not cross privilege boundaries
                   the same way Intel does)
    Caveat: some early ARM64 cores ARE vulnerable (listed in errata)
    KPTI on unvulnerable ARM64: wasted performance (disabled by kernel if safe)
    
    Check: /sys/devices/system/cpu/vulnerabilities/meltdown
    "Not affected" on modern ARM64 (Neoverse N1/N2, Cortex-A76+)
    "Mitigation: Stage-1 PTI" on vulnerable ARM64
  
  Performance of KPTI:
    syscall overhead: 2 TLBI instructions + TTBR switch = ~200 cycles extra
    Eliminated: on unvulnerable ARM64 at runtime
    spectre_v2 on ARM64: different mitigation (not KPTI)
```

---

## 6. Interview Questions & Answers

**Q1: How does ARM TrustZone separate secure and non-secure memory?**

TrustZone uses three complementary hardware mechanisms:

**1. SCR_EL3.NS bit**: The current security state is encoded in the CPU's `SCR_EL3.NS` bit. When NS=0, the CPU is in Secure world and can generate Secure PA bus transactions. When NS=1, all bus transactions are Non-Secure.

**2. NS bit in page tables**: Page table entries in Secure world include an NS bit per entry. NS=0 → access Secure physical memory. NS=1 → access Non-Secure physical memory. Non-Secure world page tables always produce Non-Secure bus transactions (NS bit in entry is ignored).

**3. TZASC (TrustZone Address Space Controller)**: A bus-level IP that intercepts ALL memory transactions. TZASC marks DRAM regions as "Secure only" or "Non-Secure accessible". A Non-Secure read transaction to a Secure DRAM region: TZASC returns a bus error (DECERR/SLVERR). This provides protection even if the SMMU is bypassed or disabled.

Practical setup: TF-A at EL3 configures TZASC to protect OP-TEE's memory (e.g., 32MB at top of DRAM). Linux sees this as a "reserved" region (from DT `reserved-memory` or ACPI). Linux cannot access this memory even if it tries directly via physical address.

**Q2: What is Pointer Authentication (PAC) and how does it prevent ROP attacks?**

PAC adds cryptographic signatures to pointers (specifically return addresses) using hardware-generated MACs. Here's how it prevents Return-Oriented Programming (ROP):

**Without PAC**: An attacker who has write access to the stack can overwrite a saved return address with an arbitrary value. When the function returns, the CPU jumps to the attacker's chosen address (chained via "gadgets" — small instruction sequences ending in `RET`).

**With PAC**: The function prolog executes `PACIASP` which computes MAC(x30, SP, APIA_KEY) and stores the result in the upper bits of x30 (the link register). This is saved to the stack. When the function returns: `AUTIASP` recomputes the MAC and verifies it matches. If an attacker overwrites the saved LR on the stack: the MAC won't match the (unmodified) key → `AUTIASP` corrupts the pointer → `RET` jumps to garbage → crash (not attacker's gadget).

Security guarantees: keys are stored in EL1 system registers (`APIA_EL1`), inaccessible to userspace. Different processes have different keys. Key is randomized per-process at exec() time. Even if attacker reads the stack (e.g., info-leak), they cannot compute a valid MAC without the key (cryptographically secure ~128-bit strength).

---

## 7. Quick Reference

| Security Feature | Purpose | ARM Version |
|---|---|---|
| TrustZone | Two security worlds (S/NS) | ARMv6+ |
| TZASC | DRAM secure region protection | External IP |
| MTE | Memory tag hardware safety | ARMv8.5 |
| PAC | Pointer authentication (ROP defense) | ARMv8.3 |
| BTI | Branch target restriction (JOP/COP defense) | ARMv8.5 |
| KPTI | Kernel page table isolation (Meltdown) | ARMv8.0 (SW) |

| TrustZone World | EL | Runs |
|---|---|---|
| Non-Secure | EL0, EL1, EL2 | Linux, apps, KVM |
| Secure Monitor | EL3 | TF-A |
| Secure | S-EL0 | Trusted Applications |
| Secure | S-EL1 | OP-TEE |
