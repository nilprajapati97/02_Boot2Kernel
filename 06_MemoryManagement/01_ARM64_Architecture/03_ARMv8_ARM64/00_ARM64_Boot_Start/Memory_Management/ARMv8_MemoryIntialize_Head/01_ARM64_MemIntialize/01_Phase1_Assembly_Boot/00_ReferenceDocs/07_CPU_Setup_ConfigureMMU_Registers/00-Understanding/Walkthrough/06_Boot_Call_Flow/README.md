# 06 Boot Call Flow

This chapter places `__cpu_setup` inside the arm64 boot call chain.

## Primary CPU Path

At a high level, the primary CPU path is:

1. `primary_entry`
2. `record_mmu_state`
3. `preserve_boot_args`
4. create and validate idmap support
5. `init_kernel_el`
6. `__cpu_setup`
7. `__primary_switch`
8. `__enable_mmu`
9. `__pi_early_map_kernel`
10. `__primary_switched`
11. `start_kernel`

The important observation is that `__cpu_setup` happens before `__enable_mmu` and before the normal kernel mapping is live.

## Secondary CPU Path

The secondary path reuses the same architectural setup logic.

1. `secondary_entry`
2. `init_kernel_el`
3. optional VA52 check
4. `__cpu_setup`
5. `__enable_mmu`
6. `__secondary_switched`

This shows that `__cpu_setup` is not only a boot-CPU helper. It establishes a required per-CPU baseline.

## Resume Path

The suspend/resume path in `sleep.S` also reuses it:

1. `cpu_resume`
2. `init_kernel_el`
3. `__cpu_setup`
4. `__enable_mmu`
5. `_cpu_resume`

That is a strong clue about the function's real contract: restore the architectural state required before enabling EL1 translation again.