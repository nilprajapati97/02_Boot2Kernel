# Virtual Memory Basics

Virtual memory lets software use virtual addresses instead of directly using physical addresses. The MMU translates virtual addresses into physical addresses using page tables.

## Before MMU enable

Before the MMU is enabled:

- The translation machinery is not actively used for normal virtual addressing.
- Early boot code must be careful about where it executes from.
- The kernel uses a temporary identity mapping strategy so code can survive the transition.

## After MMU enable

After the MMU is enabled:

- `TTBR0_EL1` and `TTBR1_EL1` point at translation tables.
- `TCR_EL1` controls how the address space is interpreted.
- `MAIR_EL1` defines memory types used by page-table attributes.
- TLB entries cache translations.

## Key concepts

### Page table
A hierarchical structure used by the MMU to translate addresses.

### TLB
A cache of recent translations. If a translation is already in the TLB, the CPU avoids a full page-table walk.

### Memory attributes
Not all memory should behave the same way. Device memory and normal RAM need different ordering and caching rules.

### Identity map
A temporary map where the virtual address corresponds to the physical address closely enough to keep execution safe while translation is being turned on.

## Why `__cpu_setup` is central

`__cpu_setup` does not build page tables. That work is done earlier.

What it does is tell the CPU how to interpret those page tables and what behavior to apply when translation begins.
