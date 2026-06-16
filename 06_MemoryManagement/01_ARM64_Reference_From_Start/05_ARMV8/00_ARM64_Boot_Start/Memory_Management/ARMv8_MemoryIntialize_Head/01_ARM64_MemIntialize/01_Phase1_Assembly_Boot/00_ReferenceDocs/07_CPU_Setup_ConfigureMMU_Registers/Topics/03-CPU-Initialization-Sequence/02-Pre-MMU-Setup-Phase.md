# Pre-MMU Setup Phase

This phase covers what must be true before the MMU can be enabled.

## What the kernel prepares before `__cpu_setup`

### Early stack
The CPU needs a reliable stack for early subroutine calls.

### Boot arguments
The device tree pointer and other bootloader-provided values are preserved.

### Identity-mapped page tables
The kernel creates an idmap page-table structure that lets execution continue safely when the MMU turns on.

### CPU mode state
The code in `init_kernel_el` ensures the processor is configured for the kernel's intended execution level.

## What is not fully ready yet

- the normal kernel virtual address space is not fully active yet
- high-level allocator infrastructure is not available
- the permanent runtime environment is not yet established

## Role of `__cpu_setup` in this phase boundary

Everything before `__cpu_setup` builds the conditions needed for MMU activation.
Everything inside `__cpu_setup` programs the register policy needed for MMU activation.
Everything after `__cpu_setup` moves toward actually switching translation on.
