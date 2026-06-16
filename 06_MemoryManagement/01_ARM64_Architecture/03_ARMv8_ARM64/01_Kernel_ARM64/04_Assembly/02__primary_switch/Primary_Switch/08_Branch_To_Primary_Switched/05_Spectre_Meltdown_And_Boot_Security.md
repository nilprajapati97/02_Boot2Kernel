# Spectre, Meltdown, and Boot Security in ARM64

**Context:** How hardware vulnerabilities relate to the `__primary_switch` code path  
**Scope:** Security mitigations relevant to the boot sequence

---

## 0. Why Boot Security Matters

The boot sequence (`primary_entry` through `start_kernel`) initializes the CPU
state that ALL subsequent security depends on:
- SCTLR_EL1 bits (PAN, UAO, WXN, BTI, MTE, etc.)
- TTBR state (page table security)
- VBAR_EL1 (exception handler integrity)
- IBRS, SSBS, TRBE, PMCR state

If an attacker can influence the boot sequence, they can disable mitigations
before the kernel checks for them.

---

## 1. Meltdown (CVE-2017-5754) and KPTI

**Meltdown** exploits the fact that speculative execution reads kernel memory
from user mode — before the permission check is completed. A user-mode program
can speculatively access kernel VAs and exfiltrate the data via cache timing.

**ARM64 vulnerability status:**
- **Cortex-A75 and earlier:** NOT vulnerable to Meltdown (strict EL permission
  checks before speculative access)
- **Some early Cortex-A73/A72:** May be vulnerable to Meltdown-variant attacks
- **Cortex-A55 and later, Neoverse:** NOT vulnerable

**KPTI (Kernel Page Table Isolation):**
When vulnerable, the kernel uses KPTI: separate page tables for user mode and
kernel mode. In user mode, the TTBR1 kernel mapping is reduced to only the
syscall entry trampolines (not full kernel). In kernel mode, full kernel
mapping is active.

```
Without KPTI:
  TTBR1 always = full kernel page tables (kernel can be read speculatively)

With KPTI:
  User mode TTBR1 = trampoline pages only (kernel not visible in user mode)
  On syscall: switch TTBR1 to full kernel tables
  On return: switch back to trampoline TTBR1
```

**Boot-time detection:**
```c
// arch/arm64/kernel/cpufeature.c
static bool has_no_hw_prefetch(const struct arm64_cpu_capabilities *entry, int scope)
{
    // Check for Meltdown-vulnerable cores
}
```

KPTI is enabled/disabled based on CPU feature detection at boot. The
`__primary_switch` code path sets up initial page tables — KPTI doubles the
work, but the page table structure difference is transparent to `__primary_switch`.

**VHE and KPTI:**
With VHE (`HCR_EL2.E2H=1`), the kernel runs at EL2. Meltdown is an EL0→EL1
attack. With VHE, user mode (EL0) cannot speculatively read EL2 memory.
Therefore, **KPTI is not needed (and not used) on VHE systems**.

---

## 2. Spectre v1 (CVE-2017-5753) — Bounds Check Bypass

**Spectre v1** allows an attacker to bypass array bounds checks by training
the branch predictor, then executing a gadget that reads out-of-bounds and
exfiltrates via cache timing.

**Mitigation in kernel code:**
```c
// In kernel array access code:
index = array_index_nospec(user_index, array_size);
// This masks index to be safe (AND with mask) even if bounds check is bypassed
```

**Boot-time relevance:** Minimal. Spectre v1 requires an attacker-controlled
index and a cache-timing covert channel. During early boot, no user processes
run, so there is no attacker.

After `start_kernel`, the kernel enables:
- `SCTLR_EL1.UCI=0` (no cache invalidation from user mode)
- `SCTLR_EL1.DZE=0` (no DC ZVA from user mode in some contexts)
These limit covert channel options.

---

## 3. Spectre v2 (CVE-2017-5715) — Branch Target Injection

**Spectre v2** allows an attacker to train the indirect branch predictor (IBP)
to cause the kernel's `br`/`blr` instructions to speculatively execute
attacker-chosen gadgets.

**ARM64 Mitigations:**

| Mitigation | Mechanism | When Enabled |
|---|---|---|
| **IBRS** (Indirect Branch Restricted Speculation) | `MSR IBRS_EL1, 1` restricts IBP speculation | `arm64_ibrs_on` at kernel entry |
| **IBPB** (Indirect Branch Predictor Barrier) | Flush IBP at EL0→EL1 transition | `arm64_ibpb_on` at syscall entry |
| **CSV2** (Clean Slate V2) | CPU feature: EL0 can't poison EL1 IBP | Detected at boot via `ID_AA64PFR0_EL1` |
| **EIBRS** (Enhanced IBRS) | Always-on IBRS without performance penalty | Supported on newer CPUs |

**Boot sequence for Spectre v2:**
1. `start_kernel` → `cpu_detect_mitigations()`
2. CPU features read from `ID_AA64PFR0_EL1`, `ID_AA64ISAR1_EL1`
3. Appropriate IBRS/IBPB setup applied
4. Mitigation status reported via `dmesg`

**`br x8` in `__primary_switch` and Spectre v2:**
Not a concern at boot time — no user processes have run, so the IBP cannot be
attacker-trained. The branch predictor is in a clean state (or stale from
bootloader use, but not attacker-controlled).

---

## 4. Spectre-BHB (CVE-2022-23960) — Branch History Buffer Injection

**Spectre-BHB** (Branch History Buffer) is a variant where the attacker
controls entries in the Branch History Buffer (BHB), which affects speculative
execution in the kernel even when IBRS is active.

**ARM64 Mitigation:**
```c
// arch/arm64/kernel/entry.S (on EL0→EL1 transition):
// Clear branch history buffer using one of:
// 1. CLRBHB instruction (if FEAT_CLRBHB available, ARMv8.9)
// 2. Firmware workaround (PSCI workaround 3)
// 3. Software loop to flush BHB (32-iteration loop to fill BHB with safe entries)
```

The `CLRBHB` instruction is the cleanest solution when available:
```asm
// Inserted at EL0→EL1 transition:
CLRBHB  // Clear Branch History Buffer
```

**Boot relevance:** Not applicable before `start_kernel` — no user processes.

---

## 5. BTI (Branch Target Identification) — Boot Setup

BTI is enabled as part of SCTLR_EL1 setup during `__cpu_setup`:

```asm
// arch/arm64/kernel/cpu_setup.S
// __cpu_setup sets SCTLR_EL1 including:
// SCTLR_EL1_BT1 (BT1 bit) if CONFIG_ARM64_BTI_KERNEL=y
```

When BTI is active:
- Every `br`/`blr` must land at a `BTI` instruction
- `__primary_switched` must start with `BTI j` or `BTI jc`
- Any kernel function that can be called indirectly must have BTI landing pads

**`SYM_FUNC_START` macro with BTI:**
```asm
// include/linux/linkage.h → arch/arm64/include/asm/linkage.h
#define SYM_FUNC_START(name) \
    .globl name; \
    ALIGN_SYMBOL; \
    name:; \
    AARCH64_VALID_BTI_TARGET  // expands to: hint #34 (BTI c)
```

So every function marked with `SYM_FUNC_START` automatically gets a BTI landing
pad. `__primary_switched` uses `SYM_FUNC_START_LOCAL`, which gets the same BTI
insertion.

---

## 6. PAC (Pointer Authentication Code) — Stack Protection

PAC uses a cryptographic MAC on return addresses and function pointers to
detect tampering:

```asm
// Function prologue (compiler-generated with -mbranch-protection=pac-ret):
paciasp     // PA-C: sign LR using SP and instruction address key A
stp x29, x30, [sp, #-16]!

// Function epilogue:
ldp x29, x30, [sp], #16
autiasp     // AUT-I: authenticate and strip signature from LR
ret
```

**`__primary_switch` and PAC:**
The very early boot code (`head.S`) is compiled WITHOUT PAC (PAC keys are not
set up yet). PAC keys are initialized in `__cpu_setup`:

```asm
// arch/arm64/kernel/cpu_setup.S
// Initialize PAC keys (APIAKey, APIBKey, APDAKey, APDBKey, APGAKey):
// Write random values to APIAKEYLO_EL1, APIAKEYHI_EL1, etc.
```

After `__cpu_setup` (which runs before `__primary_switch`), PAC is active.
Functions in `__primary_switched` and later can use PAC safely.

---

## 7. MTE (Memory Tagging Extension) — Heap Safety

MTE (ARMv8.5+) adds 4-bit tags to every 16-byte granule of memory, and embeds
the expected tag in pointer bits [59:56]. On access with wrong tag → exception.

**MTE setup at boot:**
```c
// arch/arm64/kernel/mte.c
void mte_init_tags(unsigned long max_tag)
{
    // Sets up early tag storage
}
```

MTE is not relevant to `__primary_switch` itself (no allocations). MTE is
enabled later when the first user process is created and the MTE MMAP flag is
used.

---

## 8. Threat Model Summary for `__primary_switch` Path

| Attack Vector | Risk During Boot | Mitigation |
|---|---|---|
| Meltdown | None (no user space) | KPTI setup at boot for when user starts |
| Spectre v1 | None (no attacker-controlled index) | Array masking in kernel code |
| Spectre v2 | None (IBP not attacker-trained) | IBPB/IBRS set up post-`start_kernel` |
| Spectre-BHB | None (no user processes) | CLRBHB on EL0→EL1 set up post-`start_kernel` |
| BTI bypass | Present if BTI not enabled | SCTLR_EL1.BT1 set in `__cpu_setup` |
| Stack corruption | Possible (no canary yet) | Stack canary added in `start_kernel` |
| PAC bypass | Low (PAC keys initialized in `__cpu_setup`) | Keys set, assembly uses PAC |
| Physical attack | Out of scope for kernel | Secure boot chain |

The critical insight: during `__primary_switch`, the threat model is not
"user-space attacker" but "compromised bootloader" or "physical hardware
attack." These are addressed by Secure Boot (TrustZone, UEFI SecureBoot),
not by kernel mitigations.

---

## ARMv8 CPU / Kernel / Memory Context

### CPU Perspective (ARMv8-A)
Spectre (variant 1 and 2) and Meltdown (variant 3) exploit speculative execution in out-of-order ARMv8-A cores. Variant 1: bounds check bypass -- the CPU speculatively executes past an array bounds check and leaks data via a cache side-channel. Variant 2: branch target injection -- an attacker trains the indirect branch predictor (IBP) to redirect speculative execution to a gadget. Meltdown: exception handler bypass -- the CPU speculatively reads kernel memory from user context. ARM Cortex-A57/A72/A75 are affected. Cortex-A53/A55 are NOT affected by Meltdown.

### Kernel Perspective (Linux ARM64)
Linux ARM64 mitigations include:
- Spectre-v1: array_index_nospec() barriers in critical syscall paths.
- Spectre-v2: IBRS (Indirect Branch Restricted Speculation) or retpolines, enabled via CONFIG_HARDEN_BRANCH_PREDICTOR. The early boot path in __primary_switch is not directly exposed but the BR x8 at the PA->VA transition is the first indirect branch that could be mispredicted.
- Meltdown: not applicable to ARM64 (no user->kernel speculation of that type).
- Spectre-BHB (v3a): CSV2/CSV3 and BHI_DIS_EL1 mitigations for Branch History Injection, added in Linux 5.17 for Cortex-A78/X1/X2.

### Memory Perspective (ARMv8 Memory Model)
Spectre attacks are memory attacks: the speculative load fills a cache line at an attacker-controlled physical address. The attacker then times a probe load to determine which cache line was filled. The ARMv8 memory model does not define cache timing -- the timing channel is microarchitectural, not architectural. Linux mitigation for Spectre-v1 uses barrier instructions (e.g., CSDB -- Consumption of Speculative Data Barrier, ARMv8.3) to prevent the speculative value from being used as a memory address. CSDB is a new ARMv8.3 instruction that stops speculative memory accesses derived from a value loaded after an mispredicted branch.