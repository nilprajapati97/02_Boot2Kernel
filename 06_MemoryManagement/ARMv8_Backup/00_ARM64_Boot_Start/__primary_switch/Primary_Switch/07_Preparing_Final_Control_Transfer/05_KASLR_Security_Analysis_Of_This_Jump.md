# KASLR Security Analysis of `br x8` — Why Indirect Branch?

**Context:** Why `br x8` instead of `b __primary_switched` at the end of `__primary_switch`  
**Threat Model:** KASLR bypass via static analysis of the kernel binary

---

## 0. KASLR's Security Assumption

KASLR (Kernel Address Space Layout Randomization) protects by making kernel
addresses **unpredictable at runtime**. An attacker who wants to redirect
execution (e.g., via a kernel memory corruption bug) needs to know the address
of the gadget or function they want to target.

KASLR's benefit relies on the attacker NOT being able to determine:
1. Where the kernel is loaded (PA randomization)
2. What VA the kernel uses (VA randomization)

If the binary leaks information about the KASLR offset, the security guarantee
is reduced.

---

## 1. Why `b __primary_switched` Would Leak Information

Suppose the instruction `b __primary_switched` were used instead of `ldr; br`:

```asm
// Hypothetical: direct branch
b   __primary_switched

// Disassembled from the vmlinux binary (pre-KASLR values):
// Offset 0x1234: 14 00 00 14   b   0x5274
// Decode: offset = 0x5274 - 0x1234 = 0x4040 bytes
```

What does this reveal?

1. **Distance between `__primary_switch` and `__primary_switched`:** An attacker
   reading the binary (e.g., a compromised bootloader, or a research kernel
   shipped without stripping) can measure the relative offset between the two
   functions: `offset = __primary_switched - __primary_switch = constant`.

2. **KASLR only moves the entire image — relative offsets are preserved:**
   ```
   With KASLR:
   VA(__primary_switch)    = KIMAGE_VADDR + KASLR_offset + compile_time_offset_A
   VA(__primary_switched)  = KIMAGE_VADDR + KASLR_offset + compile_time_offset_B
   
   distance = offset_B - offset_A = SAME AS WITHOUT KASLR
   ```
   The `b` instruction encodes this fixed distance. An attacker who knows one
   kernel address can use the distance to compute the other:
   ```
   VA(__primary_switched) = VA(__primary_switch) + known_distance
   ```

3. **Practical implication:** If an attacker finds the PA/VA of any kernel symbol
   (e.g., via a kernel leak), they can compute `__primary_switched` (or any
   other symbol) by adding the known binary offset. This is true even with KASLR.
   However, using `b` makes the DISTANCE visible in the binary instructions,
   making reverse engineering easier.

---

## 2. How `br x8` (Indirect Branch) Differs

With `ldr x8, =__primary_switched; br x8`:

```asm
// In the binary:
ldr     x8, [pc, #N]         // opcode: E8 0x.. 0x.. 0x58
br      x8                   // opcode: 00 01 1F D6
// Literal pool: 00 00 xx xx xx ff ff ff  (KASLR-adjusted VA, patched at boot)
```

What does this reveal to a **static analysis** of the binary (before KASLR
relocation is applied)?

1. The **pre-relocation binary** contains the COMPILE-TIME VA of
   `__primary_switched` in the literal pool. Without KASLR, this is just
   `KIMAGE_VADDR + offset`. An attacker with the binary can read this from the
   literal pool — no additional information compared to `b`.

2. **At runtime (post-relocation):** The literal pool entry contains the actual
   KASLR-adjusted VA. An attacker who can READ the literal pool at runtime has
   the exact VA of `__primary_switched`. This is equally true for `b` (the
   instruction encodes the offset, revealing relative position).

So is `br x8` actually more secure than `b`?

---

## 3. The Real Security Benefit: Opaque Indirect Branch

The key advantage of `br x8` is **runtime opacity**:

1. **In the instruction stream**, `br x8` doesn't encode the target address.
   The 32-bit instruction `D61F0100` (br x8) is the same regardless of where
   `__primary_switched` is located.

2. **Return-Oriented Programming (ROP) / JOP protection:** Direct branches
   (`b`) are statically predictable chains. An attacker building a code-reuse
   attack (ROP/JOP) works with gadgets. If the branch to `__primary_switched`
   is indirect, the gadget must first set x8 correctly AND then branch via br.
   This adds a constraint.

3. **BTI (Branch Target Identification):** The `br x8` instruction can only
   land at a `BTI jc` or `BTI j` landing pad. `__primary_switched` must be a
   valid BTI target. Direct `b` doesn't require BTI targets (it's a static
   branch). Requiring BTI at `__primary_switched` ensures that if an attacker
   hijacks x8, they can ONLY reach valid BTI landing pads, not arbitrary code.

---

## 4. BTI Interaction with `__primary_switched`

With `CONFIG_ARM64_BTI_KERNEL=y`:

```asm
SYM_FUNC_START_LOCAL(__primary_switched)
    // First instruction after label is BTI (landing pad):
    hint    #36    // == BTI jc (valid target for BR with x registers)
    ...
```

The CPU hardware enforces:
- `br x8` can only land at a `BTI j` or `BTI jc` landing pad
- If `__primary_switched` is not a BTI target, taking `br x8` to its address
  causes a `BranchTarget` exception

This means:
- **Correct boot path:** `br x8` → `__primary_switched` (has BTI j) → no fault
- **Attacker scenario:** `br x8` → arbitrary code without BTI → fault → DoS
  (attacker cannot execute arbitrary code via redirected `br x8`)

---

## 5. Spectre v2 and Indirect Branches

Indirect branches (like `br x8`) are vulnerable to **Spectre v2** (branch
target injection). A cross-process attacker can train the branch predictor for
`br x8` to speculatively execute code at an attacker-controlled address.

Mitigations on ARM64:
1. **IBRS (Indirect Branch Restricted Speculation):** Mode that prevents
   unprivileged prediction of EL1 branches. `SCTLR_EL1.EnIA` and MSR IBRS.
2. **IBPB (Indirect Branch Predictor Barrier):** Flush branch predictor on
   context switch. `PRFM pldl1keep` IBPB equivalent.
3. **CSV2 (Clean Slate speculation for V2):** CPU feature that isolates branch
   prediction between privilege levels.

During early boot (when `br x8` executes), Spectre v2 is not a concern:
- No user processes are running
- No cross-process attacker can have trained the branch predictor
- Only the bootloader code has run before this point

---

## 6. Summary: `b` vs `br x8` — Security Tradeoffs

| Property | `b __primary_switched` | `ldr x8, =sym; br x8` |
|---|---|---|
| KASLR info in binary | Relative offset visible | Absolute VA in literal pool |
| Runtime opacity | Branch target predictable from offset | Target only in register at runtime |
| BTI compatibility | N/A (direct branch, BTI not required) | BTI landing pad required at target |
| ROP/JOP resistance | Weaker (static chain) | Stronger (requires valid x8) |
| Spectre v2 risk | None (direct branch) | Low (early boot, no attacker) |
| Code size | 1 instruction | 2 instructions + 8-byte literal |

The `br x8` approach is chosen for correctness (must load VA, not PA) and
provides BTI-compatibility as a bonus security property.

---

## ARMv8 CPU / Kernel / Memory Context

### CPU Perspective (ARMv8-A)
KASLR (Kernel Address Space Layout Randomization) on ARMv8-A is implemented by choosing a random physical load address (phys_offset) and a random virtual mapping offset (kimage_voffset) at boot time. The CPU does not know or care about randomization: it simply uses whatever address is in TTBR1_EL1 as the root of the kernel page table. The EL1 exception vector base (VBAR_EL1) is also randomized as a consequence of the kernel image being loaded at a random VA. The hardware has no KASLR-awareness.

### Kernel Perspective (Linux ARM64)
KASLR is implemented in __pi_kaslr_early_init (arch/arm64/kernel/pi/kaslr_early.c). It uses the EFI random number generator (or RNDR instruction on ARMv8.5+) to pick a random offset that is aligned to the minimum KASLR granularity (2 MB for 4 KB pages). The offset is applied by:
  1. Choosing a random VA base for the kernel image within the TTBR1_EL1 region.
  2. Updating kimage_voffset = VA - PA.
  3. Updating all ELF RELA relocation entries to reflect the new VA.
  4. Flushing the D-cache for all modified sections.

### Memory Perspective (ARMv8 Memory Model)
KASLR changes the kernel's VA layout but not its PA layout (for a given boot). The TTBR1_EL1 page tables point to the randomized kernel VA. kimage_voffset is a compile-time-unknown runtime constant used to convert between kernel PA and kernel VA: VA = PA + kimage_voffset. Because the kernel uses position-independent code (__pi_ prefix) during the relocation phase, all accesses are PA-relative until the relocations are applied. After __primary_switch, all kernel code runs at the final randomized VA.