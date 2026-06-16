# Page Table Setup

`__cpu_setup` depends on page tables that were prepared earlier in the boot sequence.

## Two important table bases in the early boot path

### `TTBR0_EL1`
Used here with the identity map side.

### `TTBR1_EL1`
Used for the kernel higher virtual-address space.

## Why the tables must exist before MMU enable

Turning on translation without valid translation tables would make the CPU interpret addresses through incomplete or invalid state.

## What `__cpu_setup` contributes

It does not allocate or populate these tables. Instead, it programs the rules used when those tables are later activated.

## Practical reading note

If you want to understand the full MMU enable transition, pair this document with:

- `../03-CPU-Initialization-Sequence/02-Pre-MMU-Setup-Phase.md`
- `../03-CPU-Initialization-Sequence/04-Primary-Switch-Phase.md`
