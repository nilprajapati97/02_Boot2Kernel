# Memory Attribute Registers

`MAIR_EL1` defines what each memory-attribute index means.

## Why Linux needs this register

A page-table entry does not directly say in full detail, "this is device memory with these ordering rules". Instead, it often uses an attribute index. `MAIR_EL1` tells the hardware what that index means.

## `MAIR_EL1_SET` in `proc.S`

The early boot code builds `MAIR_EL1_SET` from several attribute encodings:

- device nGnRnE
- device nGnRE
- normal non-cacheable
- normal cacheable
- normal tagged slot initially treated as normal memory

## Why device and normal memory differ

### Device memory
Used for MMIO. Access ordering and speculation rules are stricter.

### Normal memory
Used for RAM. Cache-friendly behavior is appropriate.

## Why this matters to `__cpu_setup`

If Linux enabled translation without a valid `MAIR_EL1`, page-table entries using attribute indexes would not mean what the kernel expects.
