# ARM64 Literal Pool — Assembler Mechanics

**Instruction:** `ldr xN, =symbol` — pseudo-instruction using a literal pool  
**Scope:** How the assembler and linker implement literal pool loads in AArch64

---

## 0. What Is a Literal Pool?

A **literal pool** is a region of read-only data placed within or adjacent to
code sections. The ARM64 assembler places immediate values that are too large
to encode in instructions (as 12-bit or 16-bit immediates) into literal pools,
and generates PC-relative load instructions to retrieve them.

```asm
// Source:
ldr     x0, =0xDEAD_BEEF_1234_5678

// Assembled to:
ldr     x0, [pc, #offset]           // Load from nearby literal pool
...
// (some instructions later, in the same .text section)
.quad   0xDEAD_BEEF_1234_5678       // The literal pool entry
```

---

## 1. The `=` (Equals) Pseudo-Instruction

In GNU Assembler (GAS) ARM64 syntax, `ldr xN, =expr` is a pseudo-instruction:

```
ldr     x8, =__primary_switched

→ Assembler creates literal pool entry containing: absolute_address_of(__primary_switched)
→ Assembler generates: ldr x8, [pc, #N]    where N is the offset to the literal
```

**What value is placed in the literal pool?**

The assembler / linker places the **link-time virtual address** of
`__primary_switched` as a 64-bit value. For example:

```
Link-time VA of __primary_switched = 0xFFFF_8000_0001_0000 (pre-KASLR)

Literal pool entry: 0xFFFF_8000_0001_0000 (8 bytes, little-endian in memory)
```

After KASLR relocation (applied by `relocate_kernel`), the literal pool entry
is patched to:
```
Literal pool entry: 0xFFFF_8000_0001_0000 + KASLR_offset (KASLR-adjusted VA)
```

---

## 2. The `ldr` Instruction Encoding

The literal pool load `ldr x8, [pc, #N]` is encoded as:

```
AArch64 instruction encoding for LDR (literal), 64-bit variant:

[31:30] = 0b01         ← size: 01 = 64-bit (X register)
[29:27] = 0b011         ← encoding for LDR (literal)
[26]    = 0             ← vector/SIMD: 0 = general purpose register
[25:24] = 0b00          ← encoding for LDR (literal)
[23:5]  = imm19         ← 19-bit signed immediate (PC-relative page offset)
[4:0]   = Rt = 8        ← destination register x8
```

**`imm19`** is a 19-bit signed offset:
```
byte_offset = imm19 × 4     (imm19 is in 4-byte units)
load_address = PC + byte_offset
```

The `imm19` field gives a range of ±(2^18 × 4) = ±1MB from the `ldr` instruction.
If the literal pool is more than 1MB away from the `ldr`, the assembler must
use a different technique (indirect load).

---

## 3. Literal Pool Placement Constraints

The literal pool must be within 1MB of the `ldr` instruction that references it.

GNU Assembler places literal pool entries:
1. At `.ltorg` directives in the source
2. At the end of a function (`SYM_FUNC_END` typically triggers `.ltorg`)
3. Automatically if a pool item would otherwise exceed the 1MB range limit

In `head.S`:
```asm
SYM_FUNC_START_LOCAL(__primary_switch)
    ...
    ldr     x8, =__primary_switched         // Uses literal pool
    br      x8
SYM_FUNC_END_LOCAL(__primary_switch)        // .ltorg emitted here by convention
// OR:
.ltorg                                       // Explicit literal pool dump
// 8-byte entry for __primary_switched VA placed here
```

---

## 4. Why 1MB Is Not a Concern for `head.S`

`head.S` is at the very beginning of the kernel binary (`.head.text` section).
The literal pool is placed within the same function or immediately after.
`__primary_switched` is also in the kernel binary (within `head.S` or early
`.text`). The distance between the literal pool and the `ldr` instruction is
typically only a few hundred bytes.

The 1MB limit would only be an issue in very large single-file assembly
programs. The kernel avoids this by strategic use of `.ltorg`.

---

## 5. Literal Pool vs. `mov` Immediate Loading

An alternative to literal pool loads: load a 64-bit constant via `movz`/`movk`:

```asm
// Load 0xFFFF_8000_0001_0000 using mov instructions:
movz    x8, #0x0000, lsl #48   // x8 = 0x0000_0000_0000_0000
movk    x8, #0xFFFF, lsl #48   // x8 = 0xFFFF_0000_0000_0000
movk    x8, #0x8000, lsl #32   // x8 = 0xFFFF_8000_0000_0000
movk    x8, #0x0001, lsl #16   // x8 = 0xFFFF_8000_0001_0000
movk    x8, #0x0000             // x8 = 0xFFFF_8000_0001_0000
```

This requires 5 instructions for a 64-bit value. The `ldr` from literal pool
uses only 1 instruction. However, the `mov` approach:
- Does NOT require a relocation entry (the value is encoded in the instructions)
- BUT: With KASLR, each `movk` encoding would need to be patched individually
  → very complex

The `ldr` + literal pool approach is preferred because:
1. Single instruction
2. Single relocation entry for the 8-byte literal pool value
3. KASLR-compatible with minimal relocation overhead

---

## 6. The Literal Pool in the Binary (objdump view)

```bash
$ aarch64-linux-gnu-objdump -d vmlinux | grep -A 5 'ldr.*primary_switched'

ffff800080001234:   58000108   ldr x8, ffff80008000124c   <.Lpool_1234>
ffff800080001238:   d61f0100   br  x8
...
ffff80008000124c:   00 10 00 80 00 80 ff ff   .quad 0xffff800080001000
                                               # = VA of __primary_switched
```

After KASLR relocation:
```
ffff80008000124c:   xx xx xx xx xx xx ff ff   .quad 0xffff80xxxxxxxx
                                               # = KASLR-adjusted VA
```

The `br x8` instruction uses the loaded (possibly relocated) VA.

---

## 7. Difference Between `ldr x8, =sym` and `ldr x8, sym`

| Syntax | Meaning | What's loaded |
|---|---|---|
| `ldr x8, =sym` | Load ADDRESS of sym | 8-byte VA of sym from literal pool |
| `ldr x8, sym` | Load VALUE at sym | 8 bytes of memory at sym's address |

For `br x8` to jump to `__primary_switched`, we need the **address** of the
function, not the instruction bytes at that address. So `=sym` (address load)
is correct.

Equivalent in C: `x8 = (unsigned long)&__primary_switched;` not `x8 = *__primary_switched`.

---

## ARMv8 CPU / Kernel / Memory Context

### CPU Perspective (ARMv8-A)
ARMv8-A supports PC-relative literal pool loads via the LDR (literal) instruction. The encoding uses a 19-bit signed offset in units of 4 bytes, giving a range of +/-1 MB from the PC. The assembler places the literal value at the end of the section (or within range) and emits an LDR Xn, [PC + offset] instruction. At execution time, the CPU: (1) computes the EA = PC + (offset * 4), (2) loads 8 bytes from that address, (3) writes the value to Xn. This is a memory load instruction -- it goes through the I/D cache and TLB if the MMU is on (or directly to memory if not). It is NOT a pure register operation.

### Kernel Perspective (Linux ARM64)
In arch/arm64/kernel/head.S, the instruction ldr x8, =primary_switched (or an equivalent ADRP+ADD pair for new binutils) loads the VA of the primary_switched label. Using LDR with a literal pool rather than ADRP+ADD is appropriate when the target symbol's address cannot be guaranteed within a 4 GB range at link time, or when the assembler must encode a full 64-bit address. The literal pool entry is placed in the .text section (or .rodata), so it is loaded from kernel text memory.

### Memory Perspective (ARMv8 Memory Model)
The literal pool is part of the instruction stream in memory. When the LDR (literal) executes, it issues a data load from the physical address of the pool entry. If the I-cache and D-cache are separate and the pool is in a cached region, the load goes to the D-cache (not the I-cache). The pool value is the linked VA of primary_switched, which is correct for the post-MMU, post-relocation kernel because the literal pool is itself updated by the ELF relocation process (the .rela.dyn section contains a relocation for the literal pool entry). Without relocation, the pool would contain the link-time VA, which would be wrong if KASLR moved the image.