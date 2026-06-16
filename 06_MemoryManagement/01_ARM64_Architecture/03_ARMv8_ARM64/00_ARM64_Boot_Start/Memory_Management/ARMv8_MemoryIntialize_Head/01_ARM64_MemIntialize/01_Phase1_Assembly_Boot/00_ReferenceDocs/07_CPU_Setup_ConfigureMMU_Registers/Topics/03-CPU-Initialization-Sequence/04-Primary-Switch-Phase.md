# Primary Switch Phase

After `__cpu_setup`, the primary CPU enters `__primary_switch` and then `__enable_mmu`.

## What happens here

### Load translation base registers
The code provides the idmap table for `TTBR0_EL1` and the kernel page table for `TTBR1_EL1`.

### Write `SCTLR_EL1`
The `x0` value returned by `__cpu_setup` is written into `SCTLR_EL1` through `set_sctlr_el1`.

### Synchronize execution
Barriers and I-cache invalidation ensure the CPU executes with the new architectural state cleanly.

### Continue in mapped kernel context
After the transition, execution moves toward the fully mapped kernel address space and eventually into `start_kernel`.

## Critical point

This is the phase where the MMU really becomes active.

If you remember one distinction, remember this:

- `__cpu_setup` prepares
- `__enable_mmu` activates

## Idmap to kernel map transition

The identity map exists so the CPU does not lose its footing at the moment translation is enabled. Once the proper kernel tables are installed, the kernel can continue in its intended higher virtual address layout.
