# ARM64 Assembly Cheat Sheet

This short guide covers the assembly forms you repeatedly see around `__cpu_setup`.

## Common instructions

### `mov`
Move an immediate or register value.

### `mov_q`
Assembler helper for loading a full constant that may not fit in a single simple immediate instruction.

### `mrs reg, sysreg`
Read a system register into a general-purpose register.

### `mrs_s reg, sysreg`
A helper form used when special encoding handling is needed.

### `msr sysreg, reg`
Write a general-purpose register value into a system register.

### `tlbi vmalle1`
Invalidate all relevant EL1 stage-1 TLB entries for the local CPU.

### `dsb nsh`
Barrier that ensures certain memory-side effects are complete before execution proceeds.

### `isb`
Flushes the instruction pipeline view so future instructions execute with the latest architectural state.

### `ubfx`
Unsigned bit-field extract. Reads selected bits from a register.

### `orr`
Bitwise OR. Commonly used to set control bits.

### `bic`
Bit clear. Commonly used to clear control bits.

### `cbz reg, label`
Branch if register is zero.

### `b.lt label`
Branch if the previous compare indicates less-than.

## Assembler conveniences

### `.req`
Names a register for readability.

### `.unreq`
Removes that local name.

### `alternative_if`
Linux runtime patching framework for feature-dependent code paths.

## Good reading habit

When you read early boot assembly, keep asking:

- Is this changing architectural state or just moving data?
- Is this a normal instruction or a Linux helper macro?
- Is this register a CPU system register or just a temporary general-purpose register?
