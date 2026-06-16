# Primary Entry Phase

The primary CPU begins in `arch/arm64/kernel/head.S` at `primary_entry`.

## Responsibilities of this phase

- detect whether the MMU was already on
- preserve boot arguments such as the FDT pointer
- establish a safe early stack
- create the initial identity-mapped page tables
- normalize the exception-level state through `init_kernel_el`
- call `__cpu_setup`

## Important registers carried through early boot

- `x19`: whether we entered with MMU on
- `x20`: boot mode returned by `init_kernel_el`
- `x21`: FDT pointer passed from the bootloader

## Why the identity map is created first

The system cannot safely turn the MMU on until it has valid page tables. The early boot code prepares an identity map so the CPU can continue fetching instructions across the transition.

## Why `init_kernel_el` runs before `__cpu_setup`

The kernel must first establish the correct execution context at EL1. Only then does it make sense to program EL1 memory-management registers.

## Key takeaway

By the time `__cpu_setup` is called, Linux has already solved the privilege-level problem and the initial page-table availability problem. `__cpu_setup` then solves the control-register problem.
