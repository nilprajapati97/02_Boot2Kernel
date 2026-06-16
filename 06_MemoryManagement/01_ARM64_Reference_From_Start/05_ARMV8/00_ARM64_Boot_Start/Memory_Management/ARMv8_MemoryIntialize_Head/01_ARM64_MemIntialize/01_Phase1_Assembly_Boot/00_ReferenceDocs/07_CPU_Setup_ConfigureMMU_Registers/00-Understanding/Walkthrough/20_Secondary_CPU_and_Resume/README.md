# 20 Secondary CPU And Resume Reuse

Understanding reuse is how you confirm the true contract of `__cpu_setup`.

## Secondary CPUs

`secondary_startup` in `head.S` calls `__cpu_setup` before `__enable_mmu` just as the primary path does. That means the routine is not a one-off boot CPU helper. It prepares any CPU that is about to enter Linux's EL1 translation regime.

## Resume Path

`cpu_resume` in `sleep.S` also calls:

1. `init_kernel_el`
2. `__cpu_setup`
3. `__enable_mmu`

That is decisive evidence that the function's contract is "re-establish the required architectural state before EL1 translation is enabled," not "perform cold-boot-only setup."

## System Design Meaning

Linux chose a reusable low-level transition primitive. That keeps the boot, secondary bring-up, and resume paths consistent and reduces the risk that one path forgets to restore a critical register.