# Memory After Early Boot

This learning repo focuses on early memory setup around `__cpu_setup`, but it helps to know what comes after.

## After the early MMU transition

The kernel continues toward:

- full memory discovery
- zone initialization
- allocator setup
- later page-table refinement
- normal runtime mappings and subsystems

## Why this matters

`__cpu_setup` is not the whole memory-management story. It is the bridge between raw early boot and the fully managed memory world that later kernel code expects.

## Correct perspective

If you understand `__cpu_setup`, you understand how Linux prepares the CPU to start using its memory-management design. You do not yet understand the entire memory subsystem, but you understand the gateway into it.
