# Security Extensions: Complete Interview Reference

**Category**: Security Extensions  
**Platform**: ARM64 (AArch64)

---

## 1. ARM64 Security Architecture Summary

```
ARM64 security features overview:

Hardware security features:
  TrustZone:    Two-world (S/NS) hardware isolation
  MTE:          Memory tag checking (ARMv8.5)
  PAC:          Pointer authentication (ARMv8.3)
  BTI:          Branch target protection (ARMv8.5)
  KPTI:         Kernel page table isolation (Meltdown mitigation)
  RME/CCA:      Realm Management Extension (ARMv9.2, hardware-level VM isolation)

Software security features (using hardware):
  KASAN:        Kernel Address Sanitizer (uses MTE on ARMv8.5+)
  SMMU:         DMA isolation via IOMMU
  CFI:          Control Flow Integrity (uses BTI)
  Stack Canaries: Software protection (complemented by PAC)
  KASLR:        Address space randomization (see Category 15)

ARM64 security threat model:
  Ring 0 exploits:     KPTI, SMEP/PAN (privileged access never)
  Userspace exploits:  MTE, PAC, BTI, ASLR
  VM escapes:          Stage 2 translation, pKVM
  DMA attacks:         SMMU/IOMMU
  Physical attacks:    TrustZone + TZASC (secure boot)
  Supply chain:        Secure boot (BL1/BL2/BL31 chain of trust)
```

---

## 2. TF-A Boot Flow

```
TF-A (Trusted Firmware-A) boot sequence:

BL1 (Boot Loader stage 1):
  ROM code or early SRAM code at EL3
  First code executed after reset
  Task: basic platform init, loads BL2
  Verifies: BL2 signature (chain of trust)
  Transfer: ERET to BL2 at EL3

BL2 (Boot Loader stage 2):
  At EL3 (secure)
  Task: load other firmware images:
    BL31 (EL3 runtime firmware) = EL3 secure monitor
    BL32 (optional: OP-TEE = trusted OS) = S-EL1
    BL33 (non-trusted firmware: UEFI or Linux) = NS-EL2 or NS-EL1
  Verifies: all loaded images via signature/hash
  Configures: TZASC (protect OP-TEE memory)
  Configures: SMMU (if applicable, lock down before handoff)

BL31 (EL3 Runtime Firmware = Secure Monitor):
  Remains resident at EL3 for entire system lifetime
  Handles: SMC calls from NS world + Secure world
  Provides: PSCI, SMCCC services
  Mediates: world switch between NS and Secure

BL32 (OP-TEE, optional):
  Runs at S-EL1 (Secure EL1)
  Provides: TEE (Trusted Execution Environment)
  Hosts: Trusted Applications (TAs)
  Uses: Secure world MMU (Stage 1 in secure world)
  Exposed to NS world via: TEE client API + SMC calls

BL33 (UEFI / U-Boot / Linux):
  Runs at NS-EL2 (if hypervisor) or NS-EL1 (if bare-metal)
  For server: loads UEFI firmware → boots Linux
  For embedded: U-Boot → loads Linux or Android

Chain of trust:
  Hardware Root of Trust (RoT): ROM code / OTP fuses
  Each stage: signed by known key
  Verification: hash of next stage vs expected hash (in current stage or OTP)
  If any signature fails: boot stops → system locked
  
  Linux: /sys/firmware/efi/efivars/ for UEFI secure boot state
  ARM TBBR (Trusted Board Boot Requirements): specification for this
```

---

## 3. PAN and PXN/UXN (Privileged Access Control)

```
PAN (Privileged Access Never): ARMv8.1-A
  PSTATE.PAN bit: when set, EL1 (kernel) CANNOT access EL0 (user) virtual memory
  Purpose: prevent kernel from accidentally/maliciously accessing user memory
           (defense against kernel exploits that redirect kernel to read user data)
  
  Without PAN: kernel can dereference any user pointer directly
               Exploit: pass malicious user pointer to kernel syscall → kernel reads it
  With PAN: kernel access to user memory → EL1 fault
            Kernel must explicitly use:
              ldtr / sttr: load/store unprivileged instructions (still work with PAN)
              OR: clear PAN bit temporarily (uaccess_enable()/uaccess_disable())
  
  Linux PAN:
    CONFIG_ARM64_PAN: enable PAN
    access_ok() + copy_from_user(): properly uses unprivileged access
    uaccess_enable(): clears PSTATE.PAN (MSR PAN, #0) before user memory access
    uaccess_disable(): sets PSTATE.PAN (MSR PAN, #1) after user memory access
    Kernel modules: must use copy_from_user/to_user (cannot use direct dereference)

UAO (User Access Override): ARMv8.2-A
  PSTATE.UAO bit: when set, unprivileged accesses (ldtr/sttr) treated as privileged
  Combined with PAN: allows efficient user access without clearing/setting PAN
  
PXN (Privileged Execute Never): in page table entries
  PXN=1: EL1 (kernel) cannot execute this page
  Used for: all user pages (kernel should not execute user code)
  ARM64: PXN bit in every user mapping
  Prevents: return-to-user code attacks from kernel

UXN (User Execute Never): in page table entries
  UXN=1: EL0 (user) cannot execute this page
  Used for: kernel mappings (kernel code mapped in TTBR1 should not be executable from EL0)
  Note: KPTI means kernel is not even mapped at EL0, so UXN redundant but defensive

XOM (Execute Only Memory): ARM extensions
  Memory mapped as PROT_EXEC but NOT PROT_READ
  Purpose: prevent code reading (harder to JIT/ROP)
  ARM64: page attributes can set AP=10 (write) + PXN + UXN variants
  Limited use: compiler needs to support XOM for data/code separation
```

---

## 4. SPECTRE Mitigations on ARM64

```
Spectre attacks: exploit CPU branch prediction and speculative execution
  Spectre v1 (bounds check bypass):
    Code: if (i < array_size) { x = array[i]; y = array2[x * 64]; }
    Attack: train branch predictor → speculate past bounds check
            Cache state reveals array[i] (out-of-bounds read)
    Mitigation: CSDB (Consumption of Speculative Data Barrier) instruction
                lfence equivalent on x86
                ARM64 CSDB: blocks speculative use of load results after barrier
    Linux: array_index_nospec() macro: inserts CSDB after bounds checks
  
  Spectre v2 (branch target injection):
    Attack: inject branch target into BTB (branch target buffer)
            Victim speculatively executes at attacker-controlled address
    Mitigation: IBRS (Indirect Branch Restricted Speculation) equivalent
    ARM64: SB (Speculation Barrier) instruction
            Retpoline: replace indirect branch with safe sequence
            EIBRS: Enhanced IBRS mode (kept permanently enabled)
    Linux: CONFIG_MITIGATE_SPECTRE_BRANCH_HISTORY
    ARM Cortex-A: SPECTRE_BHI (Branch History Injection) mitigations
  
  Spectre-BHB (Branch History Buffer):
    ARM-specific variant (CSE-2022-003)
    Branch history: used to predict indirect branches
    Attack: manipulate branch history → speculate to wrong target
    Mitigation: CLRBHB instruction (clear branch history buffer)
               CSV2 (Clear Speculative Values 2): architectural mitigation
    Affected: Cortex-A76/A77/A78/X1, Neoverse N1/N2
    Linux: arch/arm64/kernel/spectre.c

ARM64 CPU vulnerability check:
  /sys/devices/system/cpu/vulnerabilities/:
    meltdown: "Not affected" (most ARM64) or "Mitigation: Stage-1 PTI"
    spectre_v1: "Mitigation: __user pointer sanitization"
    spectre_v2: "Mitigation: CSV2, BHB"
    spec_store_bypass: "Not affected" (most ARM64)
```

---

## 5. Top 15 Interview Questions

**Q1: What is ARM TrustZone and how does it provide security isolation?**
TrustZone is an ARM hardware feature that creates two parallel execution environments: Secure world (EL3, S-EL0, S-EL1) and Non-Secure world (EL0, EL1, EL2). The `SCR_EL3.NS` bit controls which world the CPU is in. TZASC hardware marks DRAM regions as Secure-only; Non-Secure accesses receive bus errors. World switch: via SMC instruction → TF-A at EL3 → context save/restore → ERET to other world.

**Q2: How does MTE detect use-after-free bugs?**
MTE assigns a 4-bit tag to each 16-byte memory chunk (stored in Tag RAM). A pointer contains a 4-bit logical tag in bits[59:56]. On every load/store, hardware compares logical tag vs allocation tag — mismatch = MTE fault. `free()` changes the allocation tag (different from the freed pointer's tag). If attacker tries to use freed pointer: old logical tag ≠ new allocation tag → fault detected at the exact instruction that triggered use-after-free.

**Q3: What is PAC and how does it prevent ROP attacks?**
PAC (Pointer Authentication) computes a MAC of a pointer using a hardware key and stores the MAC in unused pointer bits. `PACIASP` signs the return address at function entry. `AUTIASP` verifies the MAC at return. A ROP attacker who overwrites the saved return address creates an invalid MAC → `AUTIASP` corrupts the pointer → crash on `RET`. The key (stored in `APIA_EL1`) is inaccessible to userspace.

**Q4: What is BTI and what attacks does it prevent?**
BTI (Branch Target Identification) restricts valid branch targets. Pages with the GP bit set: indirect branches (`BR`, `BLR`) can only land on `BTI` instructions (`BTI c`, `BTI j`, `BTI jc`). Landing on any other instruction → BTI fault. Prevents JOP (Jump-Oriented Programming) and COP (Call-Oriented Programming) attacks where attacker chains arbitrary instruction sequences without proper BTI labels.

**Q5: Explain TF-A boot chain (BL1 → BL2 → BL31 → BL33).**
BL1: ROM code, first-boot, loads and verifies BL2. BL2: loads BL31 (EL3 monitor), BL32 (OP-TEE), BL33 (UEFI/Linux), verifies all via signature chain. BL31: permanent EL3 resident (secure monitor), handles SMC calls, PSCI. BL32 (optional): OP-TEE trusted OS at S-EL1. BL33: Normal world (UEFI → Linux or U-Boot). Each stage verifies the next — Hardware Root of Trust in ROM ensures chain cannot be broken.

**Q6: What is KPTI and is it needed on modern ARM64?**
KPTI (Kernel Page Table Isolation) uses separate page tables for kernel and user mode to prevent Meltdown-style speculative kernel memory reads from userspace. Most modern ARM64 CPUs (Cortex-A76+, Neoverse N1/N2) are NOT vulnerable to Meltdown — ARM's speculative execution does not cross privilege boundaries. Linux checks: if CPU is Meltdown-safe, KPTI is disabled. `cat /sys/devices/system/cpu/vulnerabilities/meltdown` → "Not affected" on safe ARM64.

**Q7: How does PAN protect against kernel exploits?**
PAN (Privileged Access Never) sets a PSTATE bit that prevents EL1 (kernel) from directly dereferencing EL0 (user) virtual addresses. Without PAN: a kernel exploit could redirect the kernel to read any user pointer. With PAN: kernel access to user VA → fault. Kernel must use `copy_from_user()` (which temporarily clears PAN via `PSTATE.PAN=0`) for legitimate user memory access. This prevents entire classes of kernel info-leak exploits.

**Q8: What is SMCCC and how does Linux interact with TF-A?**
SMCCC (Secure Monitor Call Calling Convention) is the ARM standard for SMC call parameters. x0 = 32-bit function ID (encoding OEN + function number), x1-x7 = arguments, x0-x3 = return values. Linux uses: PSCI SMCs for CPU power management (`CPU_ON`, `CPU_OFF`, `SYSTEM_RESET`). KVM uses: HVC calls that TF-A may trap for certain features. Linux calls `arm_smccc_smc()` which issues the `SMC` instruction. TF-A at EL3 handles it and returns via `ERET`.

**Q9: How does OP-TEE memory isolation work?**
TF-A (BL2) configures TZASC to mark OP-TEE's memory region (e.g., 32MB at top of DRAM) as "Secure only". Linux cannot access this (bus error from TZASC). TF-A passes Linux a device tree with this region in `reserved-memory` — Linux leaves it alone. OP-TEE at S-EL1 manages its own secure heap with its own MMU (Stage 1 translation in Secure world). Normal world communicates via SMC calls (TF-A forwards to OP-TEE via `ERET` to S-EL1 after context switch).

**Q10: What is KASAN and how does it differ from MTE?**
KASAN (Kernel Address Sanitizer): software instrumentation that adds bounds checking for every memory access. Shadow memory: 1 byte per 8 bytes of kernel memory tracks valid/invalid state. Every load/store: checks shadow byte (software). Overhead: 2-8× performance hit. MTE: HARDWARE tag checking, 4-bit tags, 16-byte granularity, ~1-5% overhead. `CONFIG_KASAN_HW_TAGS` on ARM64 with MTE: uses MTE hardware for KASAN → much lower overhead. KASAN catches more bug types; MTE has lower overhead.

**Q11: Explain Spectre-BHB (Branch History Buffer injection) on ARM64.**
Spectre-BHB exploits the branch history buffer used by the indirect branch predictor. An attacker can manipulate the history of taken branches to affect how the victim CPU speculates on indirect branches. On affected ARM64 cores (Cortex-A76, Neoverse N1): attacker in one privilege level can influence indirect branch targets in another level. Mitigation: `CLRBHB` instruction clears the branch history buffer at privilege boundary crossing (e.g., kernel entry). Linux: inserts `CLRBHB` in kernel entry paths.

**Q12: What is the NS bit in ARM64 page tables and when does it matter?**
The NS (Non-Secure) bit in Secure world Stage 1 page table entries controls whether a virtual address maps to Secure PA space (NS=0) or Non-Secure PA space (NS=1). Only meaningful in Secure world (EL3, S-EL1). Non-Secure world: all accesses always target Non-Secure PA (NS bit ignored). OP-TEE uses NS=0 entries to access its own Secure memory, and NS=1 entries when it needs to read/write the Normal world's memory (e.g., shared memory for TEE client communication).

**Q13: How does Secure boot protect against firmware tampering?**
Secure boot uses a Hardware Root of Trust (RoT) — typically a hash or public key burned into OTP fuses or ROM. BL1 (ROM code) uses this RoT to verify BL2's digital signature. BL2 verifies BL31, BL32, BL33. If any signature fails → boot halts. Attacker cannot: replace firmware with malicious version (won't have valid signature), modify BL2 to skip verification (BL1 in ROM is read-only), forge signatures (no private key access). Result: software running on the device was authorized by the device manufacturer.

**Q14: What is the relationship between MTE and KASAN_HW_TAGS?**
`CONFIG_KASAN_HW_TAGS` implements KASAN using MTE hardware instead of software shadow memory. Regular KASAN (software): instruments every pointer dereference with shadow byte lookup (8 bytes per pointer). HW KASAN: uses MTE's hardware tag checking. Allocation: assigns random tag via `IRG`, stamps tag into allocated memory via `STG`. Free: changes allocation tag (new allocation, different tag). Out-of-bounds/use-after-free: MTE hardware detects tag mismatch. Overhead: 2-8× (SW KASAN) vs ~1-2% (HW KASAN with MTE).

**Q15: How do PAC and BTI complement each other for control flow integrity?**
PAC and BTI together provide comprehensive Control Flow Integrity (CFI):
- **PAC** protects **backward edges** (return addresses): prevents attacker from redirecting returns to arbitrary code. Specifically: saves RAP-like "return address protection".
- **BTI** protects **forward edges** (indirect calls and jumps): ensures indirect branches only land on explicitly marked `BTI` instructions. Prevents JOP/COP attacks that chain code gadgets via indirect branches.
- Together: attacker cannot change where a function returns to (PAC) AND cannot jump to arbitrary code via indirect branch (BTI). Only legitimate, compiler-generated control flow is allowed.

---

## 6. Quick Reference

| Feature | Hardware Version | Protection Against | Performance Cost |
|---|---|---|---|
| TrustZone | ARMv6+ | OS-level attacks | N/A (separate world) |
| PAC | ARMv8.3 | ROP attacks | ~1% |
| BTI | ARMv8.5 | JOP/COP attacks | ~1% |
| MTE | ARMv8.5 | Memory safety bugs | 1-5% |
| KPTI | ARMv8.0 (SW) | Meltdown | 5-30% (disabled if not needed) |
| PAN | ARMv8.1 | User pointer deref in kernel | ~0% |
| Spectre v1 | ARMv8.0 (SW) | Bounds check bypass | ~1% |

| TF-A Stage | Location | What It Does |
|---|---|---|
| BL1 | ROM | Verify + load BL2 |
| BL2 | Secure SRAM | Verify + load BL31/32/33 |
| BL31 | SRAM (resident) | EL3 monitor, PSCI, SMC handler |
| BL32 | Secure DRAM | OP-TEE trusted OS (optional) |
| BL33 | DRAM | UEFI / U-Boot (normal world) |
