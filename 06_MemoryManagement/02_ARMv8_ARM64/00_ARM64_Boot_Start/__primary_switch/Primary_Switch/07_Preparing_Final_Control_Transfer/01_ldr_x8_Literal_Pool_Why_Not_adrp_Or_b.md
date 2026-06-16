# `ldr x8, =__primary_switched` — Why Not `adrp` or `b`?

**Instruction:** `ldr x8, =__primary_switched` in `__primary_switch`  
**Alternative 1:** `adrp x8, __primary_switched; add x8, x8, :lo12:__primary_switched`  
**Alternative 2:** `b __primary_switched` (direct branch)  
**Question:** Why is `ldr` from literal pool the correct choice here?

---

## 0. The Problem: Crossing the PA/VA Boundary

The `__primary_switch` function executes at a **physical address** (via the
identity map after MMU enable). But `__primary_switched` must be reached at
its **virtual address**.

```
__primary_switch PA:          0x4000_1000  (example PA)
__primary_switch VA:          0xFFFF_8000_4000_1000  (identity map)

__primary_switched PA:        0x4000_2000  (example PA)
__primary_switched VA:        0xFFFF_8000_4000_2000  (kernel VA, NOT identity-mapped)
```

The jump must load the VIRTUAL ADDRESS `0xFFFF_8000_4000_2000` and branch to it.

---

## 1. Why `b __primary_switched` Does NOT Work

`b` (branch) is a **PC-relative** instruction:

```asm
b   __primary_switched
// Encodes as: PC + 26-bit offset
// At link time: offset = VA(__primary_switched) - VA(b instruction)
//             = 0xFFFF_8000_4000_2000 - 0xFFFF_8000_4000_1004
//             = 0x0000_0000_0000_0FFC
```

The offset is a reasonable value. **However:**

When executing at PA `0x4000_1004`:
- `b` adds the offset to the **current PC** = `0x4000_1004`
- Target PC = `0x4000_1004 + 0x0FFC = 0x4000_2000`
- This is the **PA**, not the VA!
- CPU tries to execute at PA `0x4000_2000` via the identity map
- The identity map maps this PA to itself: VA `0x4000_2000` → PA `0x4000_2000`
- Wait — this works? The CPU fetches from PA `0x4000_2000` via TTBR0 identity map

**But `__primary_switched` is supposed to be reached via TTBR1 (kernel VA range)!**

The identity map (TTBR0) maps the PA range at low VAs (`0x4000_0000...`). The
kernel expects `__primary_switched` to run in the TTBR1 range
(`0xFFFF_8000_...`). When running via identity map at VA `0x4000_2000`, the
CPU would use TTBR0 for all data accesses, which maps to a different page table
than expected. The `adrp x4, init_thread_union` in `__primary_switched` would
compute a PA-range address, not a kernel VA!

**Conclusion: `b` gives the PA (identity-mapped VA), but we need the kernel VA.**

---

## 2. Why `adrp`/`add` Does NOT Work

`adrp` (Address, PC-relative, Page):

```asm
adrp    x8, __primary_switched
add     x8, x8, :lo12:__primary_switched
```

`adrp` computes:

```
x8 = (PC & ~0xFFF) + adrp_offset
```

where `adrp_offset` is a page-aligned PC-relative offset compiled into the
instruction.

When executing at PA `0x4000_1000`:
```
PC = 0x4000_1000
adrp_offset = linker computes: (page of VA(__primary_switched)) - (page of VA(adrp))
            = 0xFFFF_8000_4000_2000 & ~FFF) - (0xFFFF_8000_4000_1000 & ~FFF)
            = 0xFFFF_8000_4000_2000 - 0xFFFF_8000_4000_1000
            = 0x0000_0000_0000_1000

x8 = (0x4000_1000 & ~0xFFF) + 0x1000
   = 0x4000_0000 + 0x1000
   = 0x4000_1000  ← WRONG! This is a PA, not the kernel VA
```

`adrp` is PC-relative and gives a value relative to the current PC. When
running at a PA, it gives a PA-based result, not a VA.

---

## 3. Why `ldr x8, =__primary_switched` DOES Work

```asm
ldr     x8, =__primary_switched
```

This is a **literal pool load**. The assembler places the 64-bit value
`VA(__primary_switched) = 0xFFFF_8000_4000_2000` into the literal pool
(a data area near the instruction), and generates:

```asm
ldr     x8, [pc, #offset_to_literal]
// Where literal = 8 bytes containing: 0xFFFF_8000_4000_2000
```

**Key difference:** The literal pool contains the **compile-time VA** of
`__primary_switched` as a hard-coded 64-bit value. It is NOT computed relative
to the PC — it's a fixed value embedded in the binary.

When executing at PA (identity map):
```
PC = 0x4000_1004
offset_to_literal = small positive value (nearby in .text section)
Address of literal = 0x4000_10xx (also in identity-mapped region → accessible)
Value loaded: 0xFFFF_8000_4000_2000 (the VA of __primary_switched)
x8 = 0xFFFF_8000_4000_2000
```

**Wait — this works only if the literal was NOT relocated by KASLR.**

With KASLR, the kernel is loaded at a different PA, and `kimage_voffset` is
applied. The literal pool value `0xFFFF_8000_4000_2000` is the **pre-KASLR VA**.
With KASLR, the correct VA is `0xFFFF_8000_4000_2000 + KASLR_offset`.

**This is why `relocate_kernel` runs before this code path.** The relocation
step patches the literal pool entry containing `VA(__primary_switched)` to
the KASLR-adjusted VA.

---

## 4. What `br x8` Does After `ldr`

After the `ldr`:
- `x8 = VA(__primary_switched)` (KASLR-adjusted = correct kernel VA)
- `br x8` branches to that VA

The CPU's PC becomes `0xFFFF_8000_4000_2000`:
- VA[47] = 1 → TTBR1_EL1 range
- TTBR1_EL1 = `swapper_pg_dir`
- `swapper_pg_dir` maps this VA → correct PA
- Instruction fetch succeeds

First execution in the kernel virtual address space!

---

## 5. Why `bl` Is Not Used (No Return)

`br x8` (unconditional branch to register) is used, not `blr x8` (branch with
link to register). The difference:
- `br x8` jumps without setting the link register (no return)
- `blr x8` jumps AND sets `x30 = PC + 4` (expects a return)

`__primary_switched` does NOT return to `__primary_switch`. The boot sequence
is one-way: once execution enters `__primary_switched`, the stack is replaced
and `start_kernel` is called (which also never returns).

Using `bl` or `blr` would set `x30` to an address in the identity map range.
If the linked register were accidentally used as a return address (from a bug
or stack corruption), the CPU would jump to the identity map — which may not
be accessible at that point (if the identity map was torn down). Using `br`
avoids leaving a stale return address.

---

## 6. Summary: Instruction Choice Comparison

| Instruction | Computes | For VA crossing? | Correct choice? |
|---|---|---|---|
| `b __primary_switched` | PA + offset = PA | No | ❌ Wrong — gives PA |
| `adrp`/`add` | PC (PA) + relative = PA-based | No | ❌ Wrong — gives PA |
| `ldr x8, =sym; br x8` | literal VA (patched by reloc) | Yes | ✅ Correct |
| `adr x8, sym` | PC + 21-bit offset (PA-based) | No | ❌ Wrong — gives PA |

---

## ARMv8 CPU / Kernel / Memory Context

### CPU Perspective (ARMv8-A)
ARMv8-A supports PC-relative literal pool loads via the LDR (literal) instruction. The encoding uses a 19-bit signed offset in units of 4 bytes, giving a range of +/-1 MB from the PC. The assembler places the literal value at the end of the section (or within range) and emits an LDR Xn, [PC + offset] instruction. At execution time, the CPU: (1) computes the EA = PC + (offset * 4), (2) loads 8 bytes from that address, (3) writes the value to Xn. This is a memory load instruction -- it goes through the I/D cache and TLB if the MMU is on (or directly to memory if not). It is NOT a pure register operation.

### Kernel Perspective (Linux ARM64)
In arch/arm64/kernel/head.S, the instruction ldr x8, =primary_switched (or an equivalent ADRP+ADD pair for new binutils) loads the VA of the primary_switched label. Using LDR with a literal pool rather than ADRP+ADD is appropriate when the target symbol's address cannot be guaranteed within a 4 GB range at link time, or when the assembler must encode a full 64-bit address. The literal pool entry is placed in the .text section (or .rodata), so it is loaded from kernel text memory.

### Memory Perspective (ARMv8 Memory Model)
The literal pool is part of the instruction stream in memory. When the LDR (literal) executes, it issues a data load from the physical address of the pool entry. If the I-cache and D-cache are separate and the pool is in a cached region, the load goes to the D-cache (not the I-cache). The pool value is the linked VA of primary_switched, which is correct for the post-MMU, post-relocation kernel because the literal pool is itself updated by the ELF relocation process (the .rela.dyn section contains a relocation for the literal pool entry). Without relocation, the pool would contain the link-time VA, which would be wrong if KASLR moved the image.